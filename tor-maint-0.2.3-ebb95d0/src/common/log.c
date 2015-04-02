/* Copyright (c) 2001, Matej Pfajfar.
 * Copyright (c) 2001-2004, Roger Dingledine.
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2012, The Tor Project, Inc. 
 * Copyright (c) 2013 Waldo Alvarez Ca�izares, http://onionroute.org */
/* See LICENSE for licensing information */

/**
 * \file log.c
 * \brief Functions to send messages to log files or the console.
 **/

#ifdef LIBRARY
#include "../onionroute.h"
#endif

#include "orconfig.h"
#include <stdarg.h>
#include <assert.h>
// #include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#ifdef HAVE_TIME_H
#include <time.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#include "compat.h"
#include "util.h"
#define LOG_PRIVATE
#include "torlog.h"
#include "container.h"

/** @{ */
/** The string we stick at the end of a log message when it is too long,
 * and its length. */
#define TRUNCATED_STR "[...truncated]"
#define TRUNCATED_STR_LEN 14
/** @} */

/** Information for a single logfile; only used in log.c */
typedef struct logfile_t {
  struct logfile_t *next; /**< Next logfile_t in the linked list. */
  char *filename; /**< Filename to open. */
  int fd; /**< fd to receive log messages, or -1 for none. */
  int seems_dead; /**< Boolean: true if the stream seems to be kaput. */
  int needs_close; /**< Boolean: true if the stream gets closed on shutdown. */
  int is_temporary; /**< Boolean: close after initializing logging subsystem.*/
  int is_syslog; /**< Boolean: send messages to syslog. */
  log_callback callback; /**< If not NULL, send messages to this function. */
  log_severity_list_t *severities; /**< Which severity of messages should we
                                    * log for each log domain? */
} logfile_t;

static void log_free(logfile_t *victim);

/** Helper: map a log severity to descriptive string. */
static INLINE const char *
sev_to_string(int severity)
{
  switch (severity) {
    case LOG_DEBUG:   return "debug";
    case LOG_INFO:    return "info";
    case LOG_NOTICE:  return "notice";
    case LOG_WARN:    return "warn";
    case LOG_ERR:     return "err";
    default:          /* Call assert, not tor_assert, since tor_assert
                       * calls log on failure. */
                      assert(0); return "UNKNOWN";
  }
}

/** Helper: decide whether to include the function name in the log message. */
static INLINE int
should_log_function_name(log_domain_mask_t domain, int severity)
{
  switch (severity) {
    case LOG_DEBUG:
    case LOG_INFO:
      /* All debugging messages occur in interesting places. */
      return 1;
    case LOG_NOTICE:
    case LOG_WARN:
    case LOG_ERR:
      /* We care about places where bugs occur. */
      return (domain == LD_BUG);
    default:
      /* Call assert, not tor_assert, since tor_assert calls log on failure. */
      assert(0); return 0;
  }
}

/** A mutex to guard changes to logfiles and logging. */
static tor_mutex_t log_mutex;
/** True iff we have initialized log_mutex */
static int log_mutex_initialized = 0;

/** Linked list of logfile_t. */
static logfile_t *logfiles = NULL;
/** Boolean: do we report logging domains? */
static int log_domains_are_logged = 0;

#ifdef HAVE_SYSLOG_H
/** The number of open syslog log handlers that we have.  When this reaches 0,
 * we can close our connection to the syslog facility. */
static int syslog_count = 0;
#endif

/** Represents a log message that we are going to send to callback-driven
 * loggers once we can do so in a non-reentrant way. */
typedef struct pending_cb_message_t {
  int severity; /**< The severity of the message */
  log_domain_mask_t domain; /**< The domain of the message */
  char *msg; /**< The content of the message */
} pending_cb_message_t;

/** Log messages waiting to be replayed onto callback-based logs */
static smartlist_t *pending_cb_messages = NULL;

/** Lock the log_mutex to prevent others from changing the logfile_t list */
#define LOCK_LOGS() STMT_BEGIN                                          \
  tor_mutex_acquire(&log_mutex);                                        \
  STMT_END
/** Unlock the log_mutex */
#define UNLOCK_LOGS() STMT_BEGIN tor_mutex_release(&log_mutex); STMT_END

/** What's the lowest log level anybody cares about?  Checking this lets us
 * bail out early from log_debug if we aren't debugging.  */
int _log_global_min_severity = LOG_NOTICE;

static void delete_log(logfile_t *victim);
static void close_log(logfile_t *victim);

static char *domain_to_string(log_domain_mask_t domain,
                             char *buf, size_t buflen);
static INLINE char *format_msg(char *buf, size_t buf_len,
           log_domain_mask_t domain, int severity, const char *funcname,
           const char *format, va_list ap, size_t *msg_len_out)
  CHECK_PRINTF(6,0);
static void logv(int severity, log_domain_mask_t domain, const char *funcname,
                 const char *format, va_list ap)
  CHECK_PRINTF(4,0);

/** Name of the application: used to generate the message we write at the
 * start of each new log. */
static char *appname = NULL;

/** Set the "application name" for the logs to <b>name</b>: we'll use this
 * name in the message we write when starting up, and at the start of each new
 * log.
 *
 * Tor uses this string to write the version number to the log file. */
void
log_set_application_name(const char *name)
{
  tor_free(appname);
  appname = name ? tor_strdup(name) : NULL;
}

/** Log time granularity in milliseconds. */
static int log_time_granularity = 1;

/** Define log time granularity for all logs to be <b>granularity_msec</b>
 * milliseconds. */
void
set_log_time_granularity(int granularity_msec)
{
  log_time_granularity = granularity_msec;
}

/** Helper: Write the standard prefix for log lines to a
 * <b>buf_len</b> character buffer in <b>buf</b>.
 */
static INLINE size_t
_log_prefix(char *buf, size_t buf_len, int severity)
{
  time_t t;
  struct timeval now;
  struct tm tm;
  size_t n;
  int r, ms;

  tor_gettimeofday(&now);
  t = (time_t)now.tv_sec;
  ms = (int)now.tv_usec / 1000;
  if (log_time_granularity >= 1000) {
    t -= t % (log_time_granularity / 1000);
    ms = 0;
  } else {
    ms -= ((int)now.tv_usec / 1000) % log_time_granularity;
  }

  n = strftime(buf, buf_len, "%b %d %H:%M:%S", tor_localtime_r(&t, &tm));
  r = tor_snprintf(buf+n, buf_len-n, ".%.3i [%s] ", ms,
                   sev_to_string(severity));

  if (r<0)
    return buf_len-1;
  else
    return n+r;
}

/** If lf refers to an actual file that we have just opened, and the file
 * contains no data, log an "opening new logfile" message at the top.
 *
 * Return -1 if the log is broken and needs to be deleted, else return 0.
 */
static int
log_tor_version(logfile_t *lf, int reset)
{
  char buf[256];
  size_t n;
  int is_new;

  if (!lf->needs_close)
    /* If it doesn't get closed, it isn't really a file. */
    return 0;
  if (lf->is_temporary)
    /* If it's temporary, it isn't really a file. */
    return 0;

  is_new = lf->fd >= 0 && tor_fd_getpos(lf->fd) == 0;

  if (reset && !is_new)
    /* We are resetting, but we aren't at the start of the file; no
     * need to log again. */
    return 0;
  n = _log_prefix(buf, sizeof(buf), LOG_NOTICE);
  if (appname) {
    tor_snprintf(buf+n, sizeof(buf)-n,
                 "%s opening %slog file.\n", appname, is_new?"new ":"");
  } else {
    tor_snprintf(buf+n, sizeof(buf)-n,
                 "Tor %s opening %slog file.\n", VERSION, is_new?"new ":"");
  }
  if (write_all(lf->fd, buf, strlen(buf), 0) < 0) /* error */
    return -1; /* failed */
  return 0;
}



/** Helper: Format a log message into a fixed-sized buffer. (This is
 * factored out of <b>logv</b> so that we never format a message more
 * than once.)  Return a pointer to the first character of the message
 * portion of the formatted string.
 */
static INLINE char *
format_msg(char *buf, size_t buf_len,
           log_domain_mask_t domain, int severity, const char *funcname,
           const char *format, va_list ap, size_t *msg_len_out)
{
  size_t n;
  int r;
  char *end_of_prefix;
  char *buf_end;

  assert(buf_len >= 16); /* prevent integer underflow and general stupidity */
  buf_len -= 2; /* subtract 2 characters so we have room for \n\0 */
  buf_end = buf+buf_len; /* point *after* the last char we can write to */

  n = _log_prefix(buf, buf_len, severity);
  end_of_prefix = buf+n;

  if (log_domains_are_logged) {
    char *cp = buf+n;
    if (cp == buf_end) goto format_msg_no_room_for_domains;
    *cp++ = '{';
    if (cp == buf_end) goto format_msg_no_room_for_domains;
    cp = domain_to_string(domain, cp, (buf+buf_len-cp));
    if (cp == buf_end) goto format_msg_no_room_for_domains;
    *cp++ = '}';
    if (cp == buf_end) goto format_msg_no_room_for_domains;
    *cp++ = ' ';
    if (cp == buf_end) goto format_msg_no_room_for_domains;
    end_of_prefix = cp;
    n = cp-buf;
  format_msg_no_room_for_domains:
    /* This will leave end_of_prefix and n unchanged, and thus cause
     * whatever log domain string we had written to be clobbered. */
    ;
  }

  if (funcname && should_log_function_name(domain, severity)) {
    r = tor_snprintf(buf+n, buf_len-n, "%s(): ", funcname);
    if (r<0)
      n = strlen(buf);
    else
      n += r;
  }

  if (domain == LD_BUG && buf_len-n > 6) {
    memcpy(buf+n, "Bug: ", 6);
    n += 5;
  }

  r = tor_vsnprintf(buf+n,buf_len-n,format,ap);
  if (r < 0) {
    /* The message was too long; overwrite the end of the buffer with
     * "[...truncated]" */
    if (buf_len >= TRUNCATED_STR_LEN) {
      size_t offset = buf_len-TRUNCATED_STR_LEN;
      /* We have an extra 2 characters after buf_len to hold the \n\0,
       * so it's safe to add 1 to the size here. */
      strlcpy(buf+offset, TRUNCATED_STR, buf_len-offset+1);
    }
    /* Set 'n' to the end of the buffer, where we'll be writing \n\0.
     * Since we already subtracted 2 from buf_len, this is safe.*/
    n = buf_len;
  } else {
    n += r;
  }
  buf[n]='\n';
  buf[n+1]='\0';
  *msg_len_out = n+1;
  return end_of_prefix;
}

#ifdef LIBRARY

char *
onionroute_format_msg_v1(char *buf, size_t buf_len,
           log_domain_mask_t domain, int severity, const char *funcname,
           const char *format, va_list ap, size_t *msg_len_out)
{
	return format_msg(buf, buf_len, domain, severity, funcname, format, ap, msg_len_out);
}

#endif

/** Helper: sends a message to the appropriate logfiles, at loglevel
 * <b>severity</b>.  If provided, <b>funcname</b> is prepended to the
 * message.  The actual message is derived as from tor_snprintf(format,ap).
 */

#ifdef LIBRARY

onionroute_log_callback_t_v1 i_log_callback = 0;

void onionroute_set_log_callback_v1(onionroute_log_callback_t_v1 c)
{
	i_log_callback = c;
}

static void
logv(int severity, log_domain_mask_t domain, const char *funcname,
     const char *format, va_list ap)
{
	/* Call assert, not tor_assert, since tor_assert calls log on failure. */
    assert(format);
    /* check that severity is sane.  Overrunning the masks array leads to
     * interesting and hard to diagnose effects */
    assert(severity >= LOG_ERR && severity <= LOG_DEBUG);

	if(i_log_callback)
	{
		i_log_callback(severity, domain, funcname, format, ap);
	}
}

#else

static void
logv(int severity, log_domain_mask_t domain, const char *funcname,
     const char *format, va_list ap)
{
  char buf[10024];
  size_t msg_len = 0;
  int formatted = 0;
  logfile_t *lf;
  char *end_of_prefix=NULL;
  int callbacks_deferred = 0;

  /* Call assert, not tor_assert, since tor_assert calls log on failure. */
  assert(format);
  /* check that severity is sane.  Overrunning the masks array leads to
   * interesting and hard to diagnose effects */
  assert(severity >= LOG_ERR && severity <= LOG_DEBUG);
  LOCK_LOGS();

  if ((! (domain & LD_NOCB)) && smartlist_len(pending_cb_messages))
    flush_pending_log_callbacks();

  lf = logfiles;
  while (lf) {
    if (! (lf->severities->masks[SEVERITY_MASK_IDX(severity)] & domain)) {
      lf = lf->next;
      continue;
    }
    if (! (lf->fd >= 0 || lf->is_syslog || lf->callback)) {
      lf = lf->next;
      continue;
    }
    if (lf->seems_dead) {
      lf = lf->next;
      continue;
    }

    if (!formatted) {
      end_of_prefix =
        format_msg(buf, sizeof(buf), domain, severity, funcname, format, ap,
                   &msg_len);
      formatted = 1;
    }

    if (lf->is_syslog) {
#ifdef HAVE_SYSLOG_H
      char *m = end_of_prefix;
#ifdef MAXLINE
      /* Some syslog implementations have limits on the length of what you can
       * pass them, and some very old ones do not detect overflow so well.
       * Regrettably, they call their maximum line length MAXLINE. */
#if MAXLINE < 64
#warn "MAXLINE is a very low number; it might not be from syslog.h after all"
#endif
      if (msg_len >= MAXLINE)
        m = tor_strndup(end_of_prefix, MAXLINE-1);
#endif
      syslog(severity, "%s", m);
#ifdef MAXLINE
      if (m != end_of_prefix) {
        tor_free(m);
      }
#endif
#endif
      lf = lf->next;
      continue;
    } else if (lf->callback) {
      if (domain & LD_NOCB) {
        if (!callbacks_deferred && pending_cb_messages) {
          pending_cb_message_t *msg = tor_malloc(sizeof(pending_cb_message_t));
          msg->severity = severity;
          msg->domain = domain;
          msg->msg = tor_strdup(end_of_prefix);
          smartlist_add(pending_cb_messages, msg);

          callbacks_deferred = 1;
        }
      } else {
        lf->callback(severity, domain, end_of_prefix);
      }
      lf = lf->next;
      continue;
    }
    if (write_all(lf->fd, buf, msg_len, 0) < 0) { /* error */
      /* don't log the error! mark this log entry to be blown away, and
       * continue. */
      lf->seems_dead = 1;
    }
    lf = lf->next;
  }
  UNLOCK_LOGS();
}
#endif

/** Output a message to the log.  It gets logged to all logfiles that
 * care about messages with <b>severity</b> in <b>domain</b>. The content
 * is formatted printf-style based on <b>format</b> and extra arguments.
 * */
void
tor_log(int severity, log_domain_mask_t domain, const char *format, ...)
{
  va_list ap;
  if (severity > _log_global_min_severity)
    return;
  va_start(ap,format);
  logv(severity, domain, NULL, format, ap);
  va_end(ap);
}

/** Output a message to the log, prefixed with a function name <b>fn</b>. */
#ifdef __GNUC__
/** GCC-based implementation of the log_fn backend, used when we have
 * variadic macros. All arguments are as for log_fn, except for
 * <b>fn</b>, which is the name of the calling functions. */
void
_log_fn(int severity, log_domain_mask_t domain, const char *fn,
        const char *format, ...)
{
  va_list ap;
  if (severity > _log_global_min_severity)
    return;
  va_start(ap,format);
  logv(severity, domain, fn, format, ap);
  va_end(ap);
}
#else
/** @{ */
/** Variant implementation of log_fn, log_debug, log_info,... for C compilers
 * without variadic macros.  In this case, the calling function sets
 * _log_fn_function_name to the name of the function, then invokes the
 * appropriate _log_fn, _log_debug, etc. */
const char *_log_fn_function_name=NULL;
void
_log_fn(int severity, log_domain_mask_t domain, const char *format, ...)
{
  va_list ap;
  if (severity > _log_global_min_severity)
    return;
  va_start(ap,format);
  logv(severity, domain, _log_fn_function_name, format, ap);
  va_end(ap);
  _log_fn_function_name = NULL;
}
void
_log_debug(log_domain_mask_t domain, const char *format, ...)
{
  va_list ap;
  /* For GCC we do this check in the macro. */
  if (PREDICT_LIKELY(LOG_DEBUG > _log_global_min_severity))
    return;
  va_start(ap,format);
  logv(LOG_DEBUG, domain, _log_fn_function_name, format, ap);
  va_end(ap);
  _log_fn_function_name = NULL;
}
void
_log_info(log_domain_mask_t domain, const char *format, ...)
{
  va_list ap;
  if (LOG_INFO > _log_global_min_severity)
    return;
  va_start(ap,format);
  logv(LOG_INFO, domain, _log_fn_function_name, format, ap);
  va_end(ap);
  _log_fn_function_name = NULL;
}
void
_log_notice(log_domain_mask_t domain, const char *format, ...)
{
  va_list ap;
  if (LOG_NOTICE > _log_global_min_severity)
    return;
  va_start(ap,format);
  logv(LOG_NOTICE, domain, _log_fn_function_name, format, ap);
  va_end(ap);
  _log_fn_function_name = NULL;
}
void
_log_warn(log_domain_mask_t domain, const char *format, ...)
{
  va_list ap;
  if (LOG_WARN > _log_global_min_severity)
    return;
  va_start(ap,format);
  logv(LOG_WARN, domain, _log_fn_function_name, format, ap);
  va_end(ap);
  _log_fn_function_name = NULL;
}
void
_log_err(log_domain_mask_t domain, const char *format, ...)
{
  va_list ap;
  if (LOG_ERR > _log_global_min_severity)
    return;
  va_start(ap,format);
  logv(LOG_ERR, domain, _log_fn_function_name, format, ap);
  va_end(ap);
  _log_fn_function_name = NULL;
}
/** @} */
#endif

/** Free all storage held by <b>victim</b>. */
static void
log_free(logfile_t *victim)
{
  if (!victim)
    return;
  tor_free(victim->severities);
  tor_free(victim->filename);
  tor_free(victim);
}

/** Close all open log files, and free other static memory. */
void
logs_free_all(void)
{
  logfile_t *victim, *next;
  smartlist_t *messages;
  LOCK_LOGS();
  next = logfiles;
  logfiles = NULL;
  messages = pending_cb_messages;
  pending_cb_messages = NULL;
  UNLOCK_LOGS();
  while (next) {
    victim = next;
    next = next->next;
    close_log(victim);
    log_free(victim);
  }
  tor_free(appname);

  SMARTLIST_FOREACH(messages, pending_cb_message_t *, msg, {
      tor_free(msg->msg);
      tor_free(msg);
    });
  smartlist_free(messages);

  /* We _could_ destroy the log mutex here, but that would screw up any logs
   * that happened between here and the end of execution. */
}

/** Remove and free the log entry <b>victim</b> from the linked-list
 * logfiles (it is probably present, but it might not be due to thread
 * racing issues). After this function is called, the caller shouldn't
 * refer to <b>victim</b> anymore.
 *
 * Long-term, we need to do something about races in the log subsystem
 * in general. See bug 222 for more details.
 */
static void
delete_log(logfile_t *victim)
{
  logfile_t *tmpl;
  if (victim == logfiles)
    logfiles = victim->next;
  else {
    for (tmpl = logfiles; tmpl && tmpl->next != victim; tmpl=tmpl->next) ;
//    tor_assert(tmpl);
//    tor_assert(tmpl->next == victim);
    if (!tmpl)
      return;
    tmpl->next = victim->next;
  }
  log_free(victim);
}

/** Helper: release system resources (but not memory) held by a single
 * logfile_t. */
static void
close_log(logfile_t *victim)
{
  if (victim->needs_close && victim->fd >= 0) {
    close(victim->fd);
    victim->fd = -1;
  } else if (victim->is_syslog) {
#ifdef HAVE_SYSLOG_H
    if (--syslog_count == 0) {
      /* There are no other syslogs; close the logging facility. */
      closelog();
    }
#endif
  }
}

/** Adjust a log severity configuration in <b>severity_out</b> to contain
 * every domain between <b>loglevelMin</b> and <b>loglevelMax</b>, inclusive.
 */
void
set_log_severity_config(int loglevelMin, int loglevelMax,
                        log_severity_list_t *severity_out)
{
  int i;
  tor_assert(loglevelMin >= loglevelMax);
  tor_assert(loglevelMin >= LOG_ERR && loglevelMin <= LOG_DEBUG);
  tor_assert(loglevelMax >= LOG_ERR && loglevelMax <= LOG_DEBUG);
  memset(severity_out, 0, sizeof(log_severity_list_t));
  for (i = loglevelMin; i >= loglevelMax; --i) {
    severity_out->masks[SEVERITY_MASK_IDX(i)] = ~0u;
  }
}

/** Add a log handler named <b>name</b> to send all messages in <b>severity</b>
 * to <b>fd</b>. Copies <b>severity</b>. Helper: does no locking. */
static void
add_stream_log_impl(const log_severity_list_t *severity,
                    const char *name, int fd)
{
  logfile_t *lf;
  lf = tor_malloc_zero(sizeof(logfile_t));
  lf->fd = fd;
  lf->filename = tor_strdup(name);
  lf->severities = tor_memdup(severity, sizeof(log_severity_list_t));
  lf->next = logfiles;

  logfiles = lf;
  _log_global_min_severity = get_min_log_level();
}

/** Add a log handler named <b>name</b> to send all messages in <b>severity</b>
 * to <b>fd</b>. Steals a reference to <b>severity</b>; the caller must
 * not use it after calling this function. */
void
add_stream_log(const log_severity_list_t *severity, const char *name, int fd)
{
  LOCK_LOGS();
  add_stream_log_impl(severity, name, fd);
  UNLOCK_LOGS();
}

/** Initialize the global logging facility */
void
init_logging(void)
{
  if (!log_mutex_initialized) {
    tor_mutex_init(&log_mutex);
    log_mutex_initialized = 1;
  }
  if (pending_cb_messages == NULL)
    pending_cb_messages = smartlist_new();
}

/** Set whether we report logging domains as a part of our log messages.
 */
void
logs_set_domain_logging(int enabled)
{
  LOCK_LOGS();
  log_domains_are_logged = enabled;
  UNLOCK_LOGS();
}

/** Add a log handler to receive messages during startup (before the real
 * logs are initialized).
 */
void
add_temp_log(int min_severity)
{
  log_severity_list_t *s = tor_malloc_zero(sizeof(log_severity_list_t));
  set_log_severity_config(min_severity, LOG_ERR, s);
  LOCK_LOGS();
  add_stream_log_impl(s, "<temp>", fileno(stdout));
  tor_free(s);
  logfiles->is_temporary = 1;
  UNLOCK_LOGS();
}

/**
 * Add a log handler to send messages in <b>severity</b>
 * to the function <b>cb</b>.
 */
int
add_callback_log(const log_severity_list_t *severity, log_callback cb)
{
  logfile_t *lf;
  lf = tor_malloc_zero(sizeof(logfile_t));
  lf->fd = -1;
  lf->severities = tor_memdup(severity, sizeof(log_severity_list_t));
  lf->filename = tor_strdup("<callback>");
  lf->callback = cb;
  lf->next = logfiles;

  LOCK_LOGS();
  logfiles = lf;
  _log_global_min_severity = get_min_log_level();
  UNLOCK_LOGS();
  return 0;
}

/** Adjust the configured severity of any logs whose callback function is
 * <b>cb</b>. */
void
change_callback_log_severity(int loglevelMin, int loglevelMax,
                             log_callback cb)
{
  logfile_t *lf;
  log_severity_list_t severities;
  set_log_severity_config(loglevelMin, loglevelMax, &severities);
  LOCK_LOGS();
  for (lf = logfiles; lf; lf = lf->next) {
    if (lf->callback == cb) {
      memcpy(lf->severities, &severities, sizeof(severities));
    }
  }
  _log_global_min_severity = get_min_log_level();
  UNLOCK_LOGS();
}

/** If there are any log messages that were generated with LD_NOCB waiting to
 * be sent to callback-based loggers, send them now. */
void
flush_pending_log_callbacks(void)
{
  logfile_t *lf;
  smartlist_t *messages, *messages_tmp;

  LOCK_LOGS();
  if (0 == smartlist_len(pending_cb_messages)) {
    UNLOCK_LOGS();
    return;
  }

  messages = pending_cb_messages;
  pending_cb_messages = smartlist_new();
  do {
    SMARTLIST_FOREACH_BEGIN(messages, pending_cb_message_t *, msg) {
      const int severity = msg->severity;
      const int domain = msg->domain;
      for (lf = logfiles; lf; lf = lf->next) {
        if (! lf->callback || lf->seems_dead ||
            ! (lf->severities->masks[SEVERITY_MASK_IDX(severity)] & domain)) {
          continue;
        }
        lf->callback(severity, domain, msg->msg);
      }
      tor_free(msg->msg);
      tor_free(msg);
    } SMARTLIST_FOREACH_END(msg);
    smartlist_clear(messages);

    messages_tmp = pending_cb_messages;
    pending_cb_messages = messages;
    messages = messages_tmp;
  } while (smartlist_len(messages));

  smartlist_free(messages);

  UNLOCK_LOGS();
}

/** Close any log handlers added by add_temp_log() or marked by
 * mark_logs_temp(). */
void
close_temp_logs(void)
{
  logfile_t *lf, **p;

  LOCK_LOGS();
  for (p = &logfiles; *p; ) {
    if ((*p)->is_temporary) {
      lf = *p;
      /* we use *p here to handle the edge case of the head of the list */
      *p = (*p)->next;
      close_log(lf);
      log_free(lf);
    } else {
      p = &((*p)->next);
    }
  }

  _log_global_min_severity = get_min_log_level();
  UNLOCK_LOGS();
}

/** Make all currently temporary logs (set to be closed by close_temp_logs)
 * live again, and close all non-temporary logs. */
void
rollback_log_changes(void)
{
  logfile_t *lf;
  LOCK_LOGS();
  for (lf = logfiles; lf; lf = lf->next)
    lf->is_temporary = ! lf->is_temporary;
  UNLOCK_LOGS();
  close_temp_logs();
}

/** Configure all log handles to be closed by close_temp_logs(). */
void
mark_logs_temp(void)
{
  logfile_t *lf;
  LOCK_LOGS();
  for (lf = logfiles; lf; lf = lf->next)
    lf->is_temporary = 1;
  UNLOCK_LOGS();
}

/**
 * Add a log handler to send messages to <b>filename</b>. If opening the
 * logfile fails, -1 is returned and errno is set appropriately (by open(2)).
 */
int
add_file_log(const log_severity_list_t *severity, const char *filename)
{
  int fd;
  logfile_t *lf;

  fd = tor_open_cloexec(filename, O_WRONLY|O_CREAT|O_APPEND, 0644);
  if (fd<0)
    return -1;
  if (tor_fd_seekend(fd)<0)
    return -1;

  LOCK_LOGS();
  add_stream_log_impl(severity, filename, fd);
  logfiles->needs_close = 1;
  lf = logfiles;
  _log_global_min_severity = get_min_log_level();

  if (log_tor_version(lf, 0) < 0) {
    delete_log(lf);
  }
  UNLOCK_LOGS();

  return 0;
}

#ifdef HAVE_SYSLOG_H
/**
 * Add a log handler to send messages to they system log facility.
 */
int
add_syslog_log(const log_severity_list_t *severity)
{
  logfile_t *lf;
  if (syslog_count++ == 0)
    /* This is the first syslog. */
    openlog("Tor", LOG_PID | LOG_NDELAY, LOGFACILITY);

  lf = tor_malloc_zero(sizeof(logfile_t));
  lf->fd = -1;
  lf->severities = tor_memdup(severity, sizeof(log_severity_list_t));
  lf->filename = tor_strdup("<syslog>");
  lf->is_syslog = 1;

  LOCK_LOGS();
  lf->next = logfiles;
  logfiles = lf;
  _log_global_min_severity = get_min_log_level();
  UNLOCK_LOGS();
  return 0;
}
#endif

/** If <b>level</b> is a valid log severity, return the corresponding
 * numeric value.  Otherwise, return -1. */
int
parse_log_level(const char *level)
{
  if (!strcasecmp(level, "err"))
    return LOG_ERR;
  if (!strcasecmp(level, "warn"))
    return LOG_WARN;
  if (!strcasecmp(level, "notice"))
    return LOG_NOTICE;
  if (!strcasecmp(level, "info"))
    return LOG_INFO;
  if (!strcasecmp(level, "debug"))
    return LOG_DEBUG;
  return -1;
}

/** Return the string equivalent of a given log level. */
const char *
log_level_to_string(int level)
{
  return sev_to_string(level);
}

/** NULL-terminated array of names for log domains such that domain_list[dom]
 * is a description of <b>dom</b>. */
static const char *domain_list[] = {
  "GENERAL", "CRYPTO", "NET", "CONFIG", "FS", "PROTOCOL", "MM",
  "HTTP", "APP", "CONTROL", "CIRC", "REND", "BUG", "DIR", "DIRSERV",
  "OR", "EDGE", "ACCT", "HIST", "HANDSHAKE", "HEARTBEAT", NULL
};

/** Return a bitmask for the log domain for which <b>domain</b> is the name,
 * or 0 if there is no such name. */
static log_domain_mask_t
parse_log_domain(const char *domain)
{
  int i;
  for (i=0; domain_list[i]; ++i) {
    if (!strcasecmp(domain, domain_list[i]))
      return (1u<<i);
  }
  return 0;
}

/** Translate a bitmask of log domains to a string. */
static char *
domain_to_string(log_domain_mask_t domain, char *buf, size_t buflen)
{
  char *cp = buf;
  char *eos = buf+buflen;

  buf[0] = '\0';
  if (! domain)
    return buf;
  while (1) {
    const char *d;
    int bit = tor_log2(domain);
    size_t n;
    if (bit >= N_LOGGING_DOMAINS) {
      tor_snprintf(buf, buflen, "<BUG:Unknown domain %lx>", (long)domain);
      return buf+strlen(buf);
    }
    d = domain_list[bit];
    n = strlcpy(cp, d, eos-cp);
    if (n >= buflen) {
      tor_snprintf(buf, buflen, "<BUG:Truncating domain %lx>", (long)domain);
      return buf+strlen(buf);
    }
    cp += n;
    domain &= ~(1<<bit);

    if (domain == 0 || (eos-cp) < 2)
      return cp;

    memcpy(cp, ",", 2); /*Nul-terminated ,"*/
    cp++;
  }
}

/** Parse a log severity pattern in *<b>cfg_ptr</b>.  Advance cfg_ptr after
 * the end of the severityPattern.  Set the value of <b>severity_out</b> to
 * the parsed pattern.  Return 0 on success, -1 on failure.
 *
 * The syntax for a SeverityPattern is:
 * <pre>
 *   SeverityPattern = *(DomainSeverity SP)* DomainSeverity
 *   DomainSeverity = (DomainList SP)? SeverityRange
 *   SeverityRange = MinSeverity ("-" MaxSeverity )?
 *   DomainList = "[" (SP? DomainSpec SP? ",") SP? DomainSpec "]"
 *   DomainSpec = "*" | Domain | "~" Domain
 * </pre>
 * A missing MaxSeverity defaults to ERR.  Severities and domains are
 * case-insensitive.  "~" indicates negation for a domain; negation happens
 * last inside a DomainList.  Only one SeverityRange without a DomainList is
 * allowed per line.
 */
int
parse_log_severity_config(const char **cfg_ptr,
                          log_severity_list_t *severity_out)
{
  const char *cfg = *cfg_ptr;
  int got_anything = 0;
  int got_an_unqualified_range = 0;
  memset(severity_out, 0, sizeof(*severity_out));

  cfg = eat_whitespace(cfg);
  while (*cfg) {
    const char *dash, *space;
    char *sev_lo, *sev_hi;
    int low, high, i;
    log_domain_mask_t domains = ~0u;

    if (*cfg == '[') {
      int err = 0;
      char *domains_str;
      smartlist_t *domains_list;
      log_domain_mask_t neg_domains = 0;
      const char *closebracket = strchr(cfg, ']');
      if (!closebracket)
        return -1;
      domains = 0;
      domains_str = tor_strndup(cfg+1, closebracket-cfg-1);
      domains_list = smartlist_new();
      smartlist_split_string(domains_list, domains_str, ",", SPLIT_SKIP_SPACE,
                             -1);
      tor_free(domains_str);
      SMARTLIST_FOREACH_BEGIN(domains_list, const char *, domain) {
            if (!strcmp(domain, "*")) {
              domains = ~0u;
            } else {
              int d;
              int negate=0;
              if (*domain == '~') {
                negate = 1;
                ++domain;
              }
              d = parse_log_domain(domain);
              if (!d) {
                log_warn(LD_CONFIG, "No such logging domain as %s", domain);
                err = 1;
              } else {
                if (negate)
                  neg_domains |= d;
                else
                  domains |= d;
              }
            }
      } SMARTLIST_FOREACH_END(domain);
      SMARTLIST_FOREACH(domains_list, char *, d, tor_free(d));
      smartlist_free(domains_list);
      if (err)
        return -1;
      if (domains == 0 && neg_domains)
        domains = ~neg_domains;
      else
        domains &= ~neg_domains;
      cfg = eat_whitespace(closebracket+1);
    } else {
      ++got_an_unqualified_range;
    }
    if (!strcasecmpstart(cfg, "file") ||
        !strcasecmpstart(cfg, "stderr") ||
        !strcasecmpstart(cfg, "stdout") ||
        !strcasecmpstart(cfg, "syslog")) {
      goto done;
    }
    if (got_an_unqualified_range > 1)
      return -1;

    space = strchr(cfg, ' ');
    dash = strchr(cfg, '-');
    if (!space)
      space = strchr(cfg, '\0');
    if (dash && dash < space) {
      sev_lo = tor_strndup(cfg, dash-cfg);
      sev_hi = tor_strndup(dash+1, space-(dash+1));
    } else {
      sev_lo = tor_strndup(cfg, space-cfg);
      sev_hi = tor_strdup("ERR");
    }
    low = parse_log_level(sev_lo);
    high = parse_log_level(sev_hi);
    tor_free(sev_lo);
    tor_free(sev_hi);
    if (low == -1)
      return -1;
    if (high == -1)
      return -1;

    got_anything = 1;
    for (i=low; i >= high; --i)
      severity_out->masks[SEVERITY_MASK_IDX(i)] |= domains;

    cfg = eat_whitespace(space);
  }

 done:
  *cfg_ptr = cfg;
  return got_anything ? 0 : -1;
}

/** Return the least severe log level that any current log is interested in. */
int
get_min_log_level(void)
{
  logfile_t *lf;
  int i;
  int min = LOG_ERR;
  for (lf = logfiles; lf; lf = lf->next) {
    for (i = LOG_DEBUG; i > min; --i)
      if (lf->severities->masks[SEVERITY_MASK_IDX(i)])
        min = i;
  }
  return min;
}

/** Switch all logs to output at most verbose level. */
void
switch_logs_debug(void)
{
  logfile_t *lf;
  int i;
  LOCK_LOGS();
  for (lf = logfiles; lf; lf=lf->next) {
    for (i = LOG_DEBUG; i >= LOG_ERR; --i)
      lf->severities->masks[SEVERITY_MASK_IDX(i)] = ~0u;
  }
  _log_global_min_severity = get_min_log_level();
  UNLOCK_LOGS();
}

#if 0
static void
dump_log_info(logfile_t *lf)
{
  const char *tp;

  if (lf->filename) {
    printf("=== log into \"%s\" (%s-%s) (%stemporary)\n", lf->filename,
           sev_to_string(lf->min_loglevel),
           sev_to_string(lf->max_loglevel),
           lf->is_temporary?"":"not ");
  } else if (lf->is_syslog) {
    printf("=== syslog (%s-%s) (%stemporary)\n",
           sev_to_string(lf->min_loglevel),
           sev_to_string(lf->max_loglevel),
           lf->is_temporary?"":"not ");
  } else {
    printf("=== log (%s-%s) (%stemporary)\n",
           sev_to_string(lf->min_loglevel),
           sev_to_string(lf->max_loglevel),
           lf->is_temporary?"":"not ");
  }
}

void
describe_logs(void)
{
  logfile_t *lf;
  printf("==== BEGIN LOGS ====\n");
  for (lf = logfiles; lf; lf = lf->next)
    dump_log_info(lf);
  printf("==== END LOGS ====\n");
}
#endif

