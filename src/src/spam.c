/*************************************************
*     Exim - an Internet mail transport agent    *
*************************************************/

/* Copyright (c) Tom Kistner <tom@duncanthrax.net> 2003-???? */
/* License: GPL */

/* Code for calling spamassassin's spamd. Called from acl.c. */

#include "exim.h"
#ifdef WITH_CONTENT_SCAN
#include "spam.h"

uschar spam_score_buffer[16];
uschar spam_score_int_buffer[16];
uschar spam_bar_buffer[128];
uschar spam_action_buffer[32];
uschar spam_report_buffer[32600];
uschar prev_user_name[128] = "";
int spam_ok = 0;
int spam_rc = 0;
uschar *prev_spamd_address_work = NULL;

static const uschar * loglabel = US"spam acl condition:";

static int
spamd_param_init(spamd_address_container *spamd)
{
/* default spamd server weight, time and backup value */
spamd->weight = SPAMD_WEIGHT;
spamd->is_failed = FALSE;
spamd->is_backup = FALSE;
return 0;
}


static int
spamd_param(const uschar *param, spamd_address_container *spamd)
{
static int timesinceday = -1;
uschar buffer[256];

/* check backup parameter */
if (Ustrcmp(param, "backup") == 0)
  {
  spamd->is_backup = TRUE;
  return 0; /* OK */
  }

/*XXX more clever parsing could discard embedded spaces? */

/* check weight parameter */
if (sscanf(param, "weight=%u", &spamd->weight))
  {
  if (spamd->weight == 0) /* this server disabled: skip it */
    return 1;
  return 0; /* OK */
  }

/* check time parameter */
if (sscanf(param, "time=%s", buffer))
  {
  unsigned int start_h = 0, start_m = 0, start_s = 0;
  unsigned int end_h = 24, end_m = 0, end_s = 0;
  unsigned int time_start, time_end;
  uschar *start_string;
  uschar *end_string;
  uschar *delimiter;

  if ((delimiter = US strchr(CS buffer, '-')))
    {
    *delimiter   = '\0';
    start_string = buffer;
    end_string   = delimiter + 1;
    if (sscanf(CS end_string, "%u.%u.%u", &end_h, &end_m, &end_s) == 0 ||
      sscanf(CS start_string, "%u.%u.%u", &start_h, &start_m, &start_s) == 0)
      {
      log_write(0, LOG_MAIN,
	"%s warning - invalid spamd time value: '%s'", loglabel, buffer);
      return -1; /* syntax error */
      }
    }
  else
    {
    log_write(0, LOG_MAIN,
    "%s warning - invalid spamd time value: '%s'", loglabel, buffer);
    return -1; /* syntax error */
    }

  if (timesinceday < 0)
    {
    time_t now = time(NULL);
    struct tm *tmp = localtime(&now);
    timesinceday = tmp->tm_hour*3600 + tmp->tm_min*60 + tmp->tm_sec;
    }

  time_start = start_h*3600 + start_m*60 + start_s;
  time_end = end_h*3600 + end_m*60 + end_s;

  if (timesinceday < time_start || timesinceday >= time_end)
    return 1; /* skip spamd server */

  return 0; /* OK */
  }

if (Ustrcmp(param, "variant=rspamd") == 0)
  {
  spamd->is_rspamd = TRUE;
  return 0;
  }

log_write(0, LOG_MAIN, "%s warning - invalid spamd parameter: '%s'",
  loglabel, param);
return -1; /* syntax error */
}


static int
spamd_get_server(spamd_address_container **spamds, int num_servers)
{
unsigned int i;
long rnd, weights = 0;
static BOOL srandomed = 0;
BOOL usebackup = FALSE;

for (;;)
  {
  /* seedup, if we have only 1 server */
  if (num_servers == 1)
    return (spamds[0]->is_failed ? -1 : 0);

  /* init ranmod */
  if (!srandomed)
    {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    srandom((unsigned int)(tv.tv_usec/1000));
    srandomed = TRUE;
    }

  /* get sum of all weights */
  for (i = 0; i < num_servers; i++)
    if (!spamds[i]->is_failed && spamds[i]->is_backup == usebackup)
      weights += spamds[i]->weight;

  if (weights != 0)
    break;
  if (usebackup)	/* all servers failed (backups too) */
    return -1;
  usebackup = TRUE;
  }

rnd = random() % weights;

for (i = 0; i < num_servers; i++)
  if (!spamds[i]->is_failed && spamds[i]->is_backup == usebackup)
    if ((rnd -= spamds[i]->weight) < 0)
      return i;

log_write(0, LOG_MAIN|LOG_PANIC,
  "%s unknown error (memory/cpu corruption?)", loglabel);
return -1;
}


int
spam(uschar **listptr)
{
int sep = 0;
uschar *list = *listptr;
uschar *user_name;
uschar user_name_buffer[128];
unsigned long mbox_size;
FILE *mbox_file;
int spamd_sock = -1;
uschar spamd_buffer[32600];
int i, j, offset, result;
BOOL is_rspamd;
uschar spamd_version[8];
uschar spamd_short_result[8];
uschar spamd_score_char;
double spamd_threshold, spamd_score, spamd_reject_score;
int spamd_report_offset;
uschar *p,*q;
int override = 0;
time_t start;
size_t read, wrote;
struct sockaddr_un server;
#ifndef NO_POLL_H
struct pollfd pollfd;
#else                               /* Patch posted by Erik ? for OS X */
struct timeval select_tv;         /* and applied by PH */
fd_set select_fd;
#endif
uschar *spamd_address_work;

/* stop compiler warning */
result = 0;

/* find the username from the option list */
if ((user_name = string_nextinlist(&list, &sep,
				   user_name_buffer,
				   sizeof(user_name_buffer))) == NULL)
  {
  /* no username given, this means no scanning should be done */
  return FAIL;
  }

/* if username is "0" or "false", do not scan */
if ( (Ustrcmp(user_name,"0") == 0) ||
     (strcmpic(user_name,US"false") == 0) )
  return FAIL;

/* if there is an additional option, check if it is "true" */
if (strcmpic(list,US"true") == 0)
  /* in that case, always return true later */
  override = 1;

/* expand spamd_address if needed */
if (*spamd_address == '$')
  {
  spamd_address_work = expand_string(spamd_address);
  if (spamd_address_work == NULL)
    {
    log_write(0, LOG_MAIN|LOG_PANIC,
      "%s spamd_address starts with $, but expansion failed: %s",
      loglabel, expand_string_message);
    return DEFER;
    }
  }
else
  spamd_address_work = spamd_address;

HDEBUG(D_acl) debug_printf("spamd: addrlist '%s'\n", spamd_address_work);

/* check if previous spamd_address was expanded and has changed. dump cached results if so */
if (  spam_ok
   && prev_spamd_address_work != NULL
   && Ustrcmp(prev_spamd_address_work, spamd_address_work) != 0
   )
  spam_ok = 0;

/* if we scanned for this username last time, just return */
if (spam_ok && Ustrcmp(prev_user_name, user_name) == 0)
  return override ? OK : spam_rc;

/* make sure the eml mbox file is spooled up */
mbox_file = spool_mbox(&mbox_size, NULL);

if (mbox_file == NULL)
  {
  /* error while spooling */
  log_write(0, LOG_MAIN|LOG_PANIC,
	 "%s error while creating mbox spool file", loglabel);
  return DEFER;
  }

start = time(NULL);

  {
  int num_servers = 0;
  int current_server;
  uschar *address;
  uschar *spamd_address_list_ptr = spamd_address_work;
  spamd_address_container * spamd_address_vector[32];
  spamd_address_container * sd;

  /* Check how many spamd servers we have
     and register their addresses */
  while ((address = string_nextinlist(&spamd_address_list_ptr, &sep,
				      NULL, 0)) != NULL)
    {
    uschar * sublist;
    int sublist_sep = -(int)' ';	/* default space-sep */
    unsigned args;
    uschar * s;

    HDEBUG(D_acl) debug_printf("spamd: addr entry '%s'\n", address);
    sd = (spamd_address_container *)store_get(sizeof(spamd_address_container));

    for (sublist = address, args = 0, spamd_param_init(sd);
	 s = string_nextinlist(&sublist, &sublist_sep, NULL, 0);
	 args++
	 )
      {
	HDEBUG(D_acl) debug_printf("spamd: addr parm '%s'\n", s);
	switch (args)
	{
	case 0:   sd->hostspec = s;
		  if (*s == '/') args++;	/* local; no port */
		  break;
	case 1:   sd->hostspec = string_sprintf("%s %s", sd->hostspec, s);
		  break;
	default:  spamd_param(s, sd);
		  break;
	}
      }
    if (args < 2)
      {
      log_write(0, LOG_MAIN,
	"%s warning - invalid spamd address: '%s'", loglabel, address);
      continue;
      }

    spamd_address_vector[num_servers] = sd;
    if (++num_servers > 31)
      break;
    }

  /* check if we have at least one server */
  if (!num_servers)
    {
    log_write(0, LOG_MAIN|LOG_PANIC,
       "%s no useable spamd server addresses in spamd_address configuration option.",
       loglabel);
    goto defer;
    }

  while (1)
    {
    uschar * errstr;

    current_server = spamd_get_server(spamd_address_vector, num_servers);
    sd = spamd_address_vector[current_server];

    debug_printf("trying server %s\n", sd->hostspec);

    /* contact a spamd */
    if ((spamd_sock = ip_streamsocket(sd->hostspec, &errstr, 5)) >= 0)
      break;

    log_write(0, LOG_MAIN, "%s spamd: %s", loglabel, errstr);
    sd->is_failed = TRUE;

    current_server = spamd_get_server(spamd_address_vector, num_servers);
    if (current_server < 0)
      {
      log_write(0, LOG_MAIN|LOG_PANIC, "%s all spamd servers failed",
	loglabel);
      goto defer;
      }
    }
    is_rspamd = sd->is_rspamd;
  }

if (spamd_sock == -1)
  {
  log_write(0, LOG_MAIN|LOG_PANIC,
      "programming fault, spamd_sock unexpectedly unset");
  goto defer;
  }

(void)fcntl(spamd_sock, F_SETFL, O_NONBLOCK);
/* now we are connected to spamd on spamd_sock */
if (is_rspamd)
  {				/* rspamd variant */
  uschar *req_str;
  const char *helo;
  const char *fcrdns;

  req_str = string_sprintf("CHECK RSPAMC/1.3\r\nContent-length: %lu\r\n"
    "Queue-Id: %s\r\nFrom: <%s>\r\nRecipient-Number: %d\r\n", mbox_size,
    message_id, sender_address, recipients_count);
  for (i = 0; i < recipients_count; i ++)
    req_str = string_sprintf("%sRcpt: <%s>\r\n", req_str, recipients_list[i].address);
  if ((helo = expand_string(US"$sender_helo_name")) != NULL && *helo != '\0')
    req_str = string_sprintf("%sHelo: %s\r\n", req_str, helo);
  if ((fcrdns = expand_string(US"$sender_host_name")) != NULL && *fcrdns != '\0')
    req_str = string_sprintf("%sHostname: %s\r\n", req_str, fcrdns);
  if (sender_host_address != NULL)
    req_str = string_sprintf("%sIP: %s\r\n", req_str, sender_host_address);
  req_str = string_sprintf("%s\r\n", req_str);
  wrote = send(spamd_sock, req_str, Ustrlen(req_str), 0); 
  }
  else
  {				/* spamassassin variant */
  (void)string_format(spamd_buffer,
	  sizeof(spamd_buffer),
	  "REPORT SPAMC/1.2\r\nUser: %s\r\nContent-length: %ld\r\n\r\n",
	  user_name,
	  mbox_size);
  /* send our request */
  wrote = send(spamd_sock, spamd_buffer, Ustrlen(spamd_buffer), 0);
  }
if (wrote == -1)
  {
  (void)close(spamd_sock);
  log_write(0, LOG_MAIN|LOG_PANIC,
       "%s spamd send failed: %s", loglabel, strerror(errno));
  goto defer;
  }

/* now send the file */
/* spamd sometimes accepts conections but doesn't read data off
 * the connection.  We make the file descriptor non-blocking so
 * that the write will only write sufficient data without blocking
 * and we poll the desciptor to make sure that we can write without
 * blocking.  Short writes are gracefully handled and if the whole
 * trasaction takes too long it is aborted.
 * Note: poll() is not supported in OSX 10.2 and is reported to be
 *       broken in more recent versions (up to 10.4).
 */
#ifndef NO_POLL_H
pollfd.fd = spamd_sock;
pollfd.events = POLLOUT;
#endif
(void)fcntl(spamd_sock, F_SETFL, O_NONBLOCK);
do
  {
  read = fread(spamd_buffer,1,sizeof(spamd_buffer),mbox_file);
  if (read > 0)
    {
    offset = 0;
again:
#ifndef NO_POLL_H
    result = poll(&pollfd, 1, 1000);

/* Patch posted by Erik ? for OS X and applied by PH */
#else
    select_tv.tv_sec = 1;
    select_tv.tv_usec = 0;
    FD_ZERO(&select_fd);
    FD_SET(spamd_sock, &select_fd);
    result = select(spamd_sock+1, NULL, &select_fd, NULL, &select_tv);
#endif
/* End Erik's patch */

    if (result == -1 && errno == EINTR)
      goto again;
    else if (result < 1)
      {
      if (result == -1)
	log_write(0, LOG_MAIN|LOG_PANIC,
	  "%s %s on spamd socket", loglabel, strerror(errno));
      else
	{
	if (time(NULL) - start < SPAMD_TIMEOUT)
	  goto again;
	log_write(0, LOG_MAIN|LOG_PANIC,
	  "%s timed out writing spamd socket", loglabel);
	}
      (void)close(spamd_sock);
      goto defer;
      }

    wrote = send(spamd_sock,spamd_buffer + offset,read - offset,0);
    if (wrote == -1)
      {
      log_write(0, LOG_MAIN|LOG_PANIC,
	  "%s %s on spamd socket", loglabel, strerror(errno));
      (void)close(spamd_sock);
      goto defer;
      }
    if (offset + wrote != read)
      {
      offset += wrote;
      goto again;
      }
    }
  }
while (!feof(mbox_file) && !ferror(mbox_file));

if (ferror(mbox_file))
  {
  log_write(0, LOG_MAIN|LOG_PANIC,
    "%s error reading spool file: %s", loglabel, strerror(errno));
  (void)close(spamd_sock);
  goto defer;
  }

(void)fclose(mbox_file);

/* we're done sending, close socket for writing */
shutdown(spamd_sock,SHUT_WR);

/* read spamd response using what's left of the timeout.  */
memset(spamd_buffer, 0, sizeof(spamd_buffer));
offset = 0;
while ((i = ip_recv(spamd_sock,
		   spamd_buffer + offset,
		   sizeof(spamd_buffer) - offset - 1,
		   SPAMD_TIMEOUT - time(NULL) + start)) > 0 )
  offset += i;

/* error handling */
if (i <= 0 && errno != 0)
  {
  log_write(0, LOG_MAIN|LOG_PANIC,
       "%s error reading from spamd socket: %s", loglabel, strerror(errno));
  (void)close(spamd_sock);
  return DEFER;
  }

/* reading done */
(void)close(spamd_sock);

if (is_rspamd)
  {				/* rspamd variant of reply */
  int r;
  if ((r = sscanf(CS spamd_buffer,
	  "RSPAMD/%7s 0 EX_OK\r\nMetric: default; %7s %lf / %lf / %lf\r\n%n",
	  spamd_version, spamd_short_result, &spamd_score, &spamd_threshold,
	  &spamd_reject_score, &spamd_report_offset)) != 5)
    {
      log_write(0, LOG_MAIN|LOG_PANIC,
		"%s cannot parse spamd output: %d", loglabel, r);
      return DEFER;
    }
  /* now parse action */
  p = &spamd_buffer[spamd_report_offset];

  if (Ustrncmp(p, "Action: ", sizeof("Action: ") - 1) == 0)
    {
    p += sizeof("Action: ") - 1;
    q = &spam_action_buffer[0];
    while (*p && *p != '\r' && (q - spam_action_buffer) < sizeof(spam_action_buffer) - 1)
      *q++ = *p++;
    *q = '\0';
    }
  }
else
  {				/* spamassassin */
  /* dig in the spamd output and put the report in a multiline header,
  if requested */
  if (sscanf(CS spamd_buffer,
       "SPAMD/%7s 0 EX_OK\r\nContent-length: %*u\r\n\r\n%lf/%lf\r\n%n",
       spamd_version,&spamd_score,&spamd_threshold,&spamd_report_offset) != 3)
    {
      /* try to fall back to pre-2.50 spamd output */
      if (sscanf(CS spamd_buffer,
	   "SPAMD/%7s 0 EX_OK\r\nSpam: %*s ; %lf / %lf\r\n\r\n%n",
	   spamd_version,&spamd_score,&spamd_threshold,&spamd_report_offset) != 3)
	{
	log_write(0, LOG_MAIN|LOG_PANIC,
		  "%s cannot parse spamd output", loglabel);
	return DEFER;
	}
    }

  Ustrcpy(spam_action_buffer,
    spamd_score >= spamd_threshold ? "reject" : "no action");
  }

/* Create report. Since this is a multiline string,
we must hack it into shape first */
p = &spamd_buffer[spamd_report_offset];
q = spam_report_buffer;
while (*p != '\0')
  {
  /* skip \r */
  if (*p == '\r')
    {
    p++;
    continue;
    }
  *q++ = *p;
  if (*p++ == '\n')
    {
    /* add an extra space after the newline to ensure
    that it is treated as a header continuation line */
    *q++ = ' ';
    }
  }
/* NULL-terminate */
*q-- = '\0';
/* cut off trailing leftovers */
while (*q <= ' ')
  *q-- = '\0';

spam_report = spam_report_buffer;
spam_action = spam_action_buffer;

/* create spam bar */
spamd_score_char = spamd_score > 0 ? '+' : '-';
j = abs((int)(spamd_score));
i = 0;
if (j != 0)
  while ((i < j) && (i <= MAX_SPAM_BAR_CHARS))
     spam_bar_buffer[i++] = spamd_score_char;
else
  {
  spam_bar_buffer[0] = '/';
  i = 1;
  }
spam_bar_buffer[i] = '\0';
spam_bar = spam_bar_buffer;

/* create "float" spam score */
(void)string_format(spam_score_buffer, sizeof(spam_score_buffer),
	"%.1f", spamd_score);
spam_score = spam_score_buffer;

/* create "int" spam score */
j = (int)((spamd_score + 0.001)*10);
(void)string_format(spam_score_int_buffer, sizeof(spam_score_int_buffer),
	"%d", j);
spam_score_int = spam_score_int_buffer;

/* compare threshold against score */
spam_rc = spamd_score >= spamd_threshold
  ? OK	/* spam as determined by user's threshold */
  : FAIL;	/* not spam */

/* remember expanded spamd_address if needed */
if (spamd_address_work != spamd_address)
  prev_spamd_address_work = string_copy(spamd_address_work);

/* remember user name and "been here" for it */
Ustrcpy(prev_user_name, user_name);
spam_ok = 1;

return override
  ? OK		/* always return OK, no matter what the score */
  : spam_rc;

defer:
  (void)fclose(mbox_file);
  return DEFER;
}

#endif
/* vi: aw ai sw=2
*/
