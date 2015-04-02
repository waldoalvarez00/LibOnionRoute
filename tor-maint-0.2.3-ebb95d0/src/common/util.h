/* Copyright (c) 2003-2004, Roger Dingledine
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2012, The Tor Project, Inc. */
/* See LICENSE for licensing information */

/**
 * \file util.h
 * \brief Headers for util.c
 **/

#ifndef _TOR_UTIL_H
#define _TOR_UTIL_H

#include "orconfig.h"
#include "torint.h"
#include "compat.h"
#include "di_ops.h"
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
/* for the correct alias to struct stat */
#include <sys/stat.h>
#endif

#ifndef O_BINARY
#define O_BINARY 0
#endif
#ifndef O_TEXT
#define O_TEXT 0
#endif

/* Replace assert() with a variant that sends failures to the log before
 * calling assert() normally.
 */
#ifdef NDEBUG
/* Nobody should ever want to build with NDEBUG set.  99% of our asserts will
 * be outside the critical path anyway, so it's silly to disable bug-checking
 * throughout the entire program just because a few asserts are slowing you
 * down.  Profile, optimize the critical path, and keep debugging on.
 *
 * And I'm not just saying that because some of our asserts check
 * security-critical properties.
 */
#error "Sorry; we don't support building with NDEBUG."
#endif

/** Like assert(3), but send assertion failures to the log as well as to
 * stderr. */
#define tor_assert(expr) STMT_BEGIN                                     \
    if (PREDICT_UNLIKELY(!(expr))) {                                    \
      log_err(LD_BUG, "%s:%d: %s: Assertion %s failed; aborting.",      \
          _SHORT_FILE_, __LINE__, __func__, #expr);                     \
      fprintf(stderr,"%s:%d %s: Assertion %s failed; aborting.\n",      \
              _SHORT_FILE_, __LINE__, __func__, #expr);                 \
      abort();                                                          \
    } STMT_END

/* If we're building with dmalloc, we want all of our memory allocation
 * functions to take an extra file/line pair of arguments.  If not, not.
 * We define DMALLOC_PARAMS to the extra parameters to insert in the
 * function prototypes, and DMALLOC_ARGS to the extra arguments to add
 * to calls. */
#ifdef USE_DMALLOC
#define DMALLOC_PARAMS , const char *file, const int line
#define DMALLOC_ARGS , _SHORT_FILE_, __LINE__
#else
#define DMALLOC_PARAMS
#define DMALLOC_ARGS
#endif

/** Define this if you want Tor to crash when any problem comes up,
 * so you can get a coredump and track things down. */
// #define tor_fragile_assert() tor_assert(0)
#define tor_fragile_assert()

/* Memory management */
void *_tor_malloc(size_t size DMALLOC_PARAMS) ATTR_MALLOC;
void *_tor_malloc_zero(size_t size DMALLOC_PARAMS) ATTR_MALLOC;
void *_tor_malloc_roundup(size_t *size DMALLOC_PARAMS) ATTR_MALLOC;
void *_tor_calloc(size_t nmemb, size_t size DMALLOC_PARAMS) ATTR_MALLOC;
void *_tor_realloc(void *ptr, size_t size DMALLOC_PARAMS);
char *_tor_strdup(const char *s DMALLOC_PARAMS) ATTR_MALLOC ATTR_NONNULL((1));
char *_tor_strndup(const char *s, size_t n DMALLOC_PARAMS)
  ATTR_MALLOC ATTR_NONNULL((1));
void *_tor_memdup(const void *mem, size_t len DMALLOC_PARAMS)
  ATTR_MALLOC ATTR_NONNULL((1));
void _tor_free(void *mem);
#ifdef USE_DMALLOC
extern int dmalloc_free(const char *file, const int line, void *pnt,
                        const int func_id);
#define tor_free(p) STMT_BEGIN \
    if (PREDICT_LIKELY((p)!=NULL)) {                \
      dmalloc_free(_SHORT_FILE_, __LINE__, (p), 0); \
      (p)=NULL;                                     \
    }                                               \
  STMT_END
#else
/** Release memory allocated by tor_malloc, tor_realloc, tor_strdup, etc.
 * Unlike the free() function, tor_free() will still work on NULL pointers,
 * and it sets the pointer value to NULL after freeing it.
 *
 * This is a macro.  If you need a function pointer to release memory from
 * tor_malloc(), use _tor_free().
 */
#define tor_free(p) STMT_BEGIN                                 \
    if (PREDICT_LIKELY((p)!=NULL)) {                           \
      free(p);                                                 \
      (p)=NULL;                                                \
    }                                                          \
  STMT_END
#endif

#define tor_malloc(size)       _tor_malloc(size DMALLOC_ARGS)
#define tor_malloc_zero(size)  _tor_malloc_zero(size DMALLOC_ARGS)
#define tor_calloc(nmemb,size) _tor_calloc(nmemb, size DMALLOC_ARGS)
#define tor_malloc_roundup(szp) _tor_malloc_roundup(szp DMALLOC_ARGS)
#define tor_realloc(ptr, size) _tor_realloc(ptr, size DMALLOC_ARGS)
#define tor_strdup(s)          _tor_strdup(s DMALLOC_ARGS)
#define tor_strndup(s, n)      _tor_strndup(s, n DMALLOC_ARGS)
#define tor_memdup(s, n)       _tor_memdup(s, n DMALLOC_ARGS)

void tor_log_mallinfo(int severity);

/** Return the offset of <b>member</b> within the type <b>tp</b>, in bytes */
#if defined(__GNUC__) && __GNUC__ > 3
#define STRUCT_OFFSET(tp, member) __builtin_offsetof(tp, member)
#else
 #define STRUCT_OFFSET(tp, member) \
   ((off_t) (((char*)&((tp*)0)->member)-(char*)0))
#endif

/** Macro: yield a pointer to the field at position <b>off</b> within the
 * structure <b>st</b>.  Example:
 * <pre>
 *   struct a { int foo; int bar; } x;
 *   off_t bar_offset = STRUCT_OFFSET(struct a, bar);
 *   int *bar_p = STRUCT_VAR_P(&x, bar_offset);
 *   *bar_p = 3;
 * </pre>
 */
#define STRUCT_VAR_P(st, off) ((void*) ( ((char*)(st)) + (off) ) )

/** Macro: yield a pointer to an enclosing structure given a pointer to
 * a substructure at offset <b>off</b>. Example:
 * <pre>
 *   struct base { ... };
 *   struct subtype { int x; struct base b; } x;
 *   struct base *bp = &x.base;
 *   struct *sp = SUBTYPE_P(bp, struct subtype, b);
 * </pre>
 */
#define SUBTYPE_P(p, subtype, basemember) \
  ((void*) ( ((char*)(p)) - STRUCT_OFFSET(subtype, basemember) ))

/* Logic */
/** Macro: true if two values have the same boolean value. */
#define bool_eq(a,b) (!(a)==!(b))
/** Macro: true if two values have different boolean values. */
#define bool_neq(a,b) (!(a)!=!(b))

/* Math functions */
double tor_mathlog(double d) ATTR_CONST;
long tor_lround(double d) ATTR_CONST;
int tor_log2(uint64_t u64) ATTR_CONST;
uint64_t round_to_power_of_2(uint64_t u64);
unsigned round_to_next_multiple_of(unsigned number, unsigned divisor);
uint32_t round_uint32_to_next_multiple_of(uint32_t number, uint32_t divisor);
uint64_t round_uint64_to_next_multiple_of(uint64_t number, uint64_t divisor);
int n_bits_set_u8(uint8_t v);

/* Compute the CEIL of <b>a</b> divided by <b>b</b>, for nonnegative <b>a</b>
 * and positive <b>b</b>.  Works on integer types only. Not defined if a+b can
 * overflow. */
#define CEIL_DIV(a,b) (((a)+(b)-1)/(b))

/* String manipulation */

/** Allowable characters in a hexadecimal string. */
#define HEX_CHARACTERS "0123456789ABCDEFabcdef"
void tor_strlower(char *s) ATTR_NONNULL((1));
void tor_strupper(char *s) ATTR_NONNULL((1));
int tor_strisprint(const char *s) ATTR_NONNULL((1));
int tor_strisnonupper(const char *s) ATTR_NONNULL((1));
int strcmp_opt(const char *s1, const char *s2);
int strcmpstart(const char *s1, const char *s2) ATTR_NONNULL((1,2));
int strcmp_len(const char *s1, const char *s2, size_t len) ATTR_NONNULL((1,2));
int strcasecmpstart(const char *s1, const char *s2) ATTR_NONNULL((1,2));
int strcmpend(const char *s1, const char *s2) ATTR_NONNULL((1,2));
int strcasecmpend(const char *s1, const char *s2) ATTR_NONNULL((1,2));
int fast_memcmpstart(const void *mem, size_t memlen, const char *prefix);

void tor_strstrip(char *s, const char *strip) ATTR_NONNULL((1,2));
long tor_parse_long(const char *s, int base, long min,
                    long max, int *ok, char **next);
unsigned long tor_parse_ulong(const char *s, int base, unsigned long min,
                              unsigned long max, int *ok, char **next);
double tor_parse_double(const char *s, double min, double max, int *ok,
                        char **next);
uint64_t tor_parse_uint64(const char *s, int base, uint64_t min,
                         uint64_t max, int *ok, char **next);
const char *hex_str(const char *from, size_t fromlen) ATTR_NONNULL((1));
const char *eat_whitespace(const char *s);
const char *eat_whitespace_eos(const char *s, const char *eos);
const char *eat_whitespace_no_nl(const char *s);
const char *eat_whitespace_eos_no_nl(const char *s, const char *eos);
const char *find_whitespace(const char *s);
const char *find_whitespace_eos(const char *s, const char *eos);
const char *find_str_at_start_of_line(const char *haystack,
                                      const char *needle);
int string_is_C_identifier(const char *string);

int tor_mem_is_zero(const char *mem, size_t len);
int tor_digest_is_zero(const char *digest);
int tor_digest256_is_zero(const char *digest);
char *esc_for_log(const char *string) ATTR_MALLOC;
const char *escaped(const char *string);
struct smartlist_t;
void wrap_string(struct smartlist_t *out, const char *string, size_t width,
                 const char *prefix0, const char *prefixRest);
int tor_vsscanf(const char *buf, const char *pattern, va_list ap)
#ifdef __GNUC__
  __attribute__((format(scanf, 2, 0)))
#endif
  ;
int tor_sscanf(const char *buf, const char *pattern, ...)
#ifdef __GNUC__
  __attribute__((format(scanf, 2, 3)))
#endif
  ;

void smartlist_add_asprintf(struct smartlist_t *sl, const char *pattern, ...)
  CHECK_PRINTF(2, 3);
void smartlist_add_vasprintf(struct smartlist_t *sl, const char *pattern,
                             va_list args)
  CHECK_PRINTF(2, 0);

int hex_decode_digit(char c);
void base16_encode(char *dest, size_t destlen, const char *src, size_t srclen);
int base16_decode(char *dest, size_t destlen, const char *src, size_t srclen);

/* Time helpers */
double tv_to_double(const struct timeval *tv);
int64_t tv_to_msec(const struct timeval *tv);
int64_t tv_to_usec(const struct timeval *tv);
long tv_udiff(const struct timeval *start, const struct timeval *end);
long tv_mdiff(const struct timeval *start, const struct timeval *end);
int tor_timegm(const struct tm *tm, time_t *time_out);
#define RFC1123_TIME_LEN 29
void format_rfc1123_time(char *buf, time_t t);
int parse_rfc1123_time(const char *buf, time_t *t);
#define ISO_TIME_LEN 19
#define ISO_TIME_USEC_LEN (ISO_TIME_LEN+7)
void format_local_iso_time(char *buf, time_t t);
void format_iso_time(char *buf, time_t t);
void format_iso_time_nospace(char *buf, time_t t);
void format_iso_time_nospace_usec(char *buf, const struct timeval *tv);
int parse_iso_time(const char *buf, time_t *t);
int parse_http_time(const char *buf, struct tm *tm);
int format_time_interval(char *out, size_t out_len, long interval);

/* Cached time */
#ifdef TIME_IS_FAST
#define approx_time() time(NULL)
#define update_approx_time(t) STMT_NIL
#else
time_t approx_time(void);
void update_approx_time(time_t now);
#endif

/* Rate-limiter */

/** A ratelim_t remembers how often an event is occurring, and how often
 * it's allowed to occur.  Typical usage is something like:
 *
   <pre>
    if (possibly_very_frequent_event()) {
      const int INTERVAL = 300;
      static ratelim_t warning_limit = RATELIM_INIT(INTERVAL);
      char *m;
      if ((m = rate_limit_log(&warning_limit, approx_time()))) {
        log_warn(LD_GENERAL, "The event occurred!%s", m);
        tor_free(m);
      }
    }
   </pre>
 */
typedef struct ratelim_t {
  int rate;
  time_t last_allowed;
  int n_calls_since_last_time;
} ratelim_t;

#define RATELIM_INIT(r) { (r), 0, 0 }

char *rate_limit_log(ratelim_t *lim, time_t now);

/* File helpers */
ssize_t write_all(tor_socket_t fd, const char *buf, size_t count,int isSocket);
ssize_t read_all(tor_socket_t fd, char *buf, size_t count, int isSocket);

/** Status of an I/O stream. */
enum stream_status {
  IO_STREAM_OKAY,
  IO_STREAM_EAGAIN,
  IO_STREAM_TERM,
  IO_STREAM_CLOSED
};

enum stream_status get_string_from_pipe(FILE *stream, char *buf, size_t count);

/** Return values from file_status(); see that function's documentation
 * for details. */
typedef enum { FN_ERROR, FN_NOENT, FN_FILE, FN_DIR } file_status_t;
file_status_t file_status(const char *filename);

/** Possible behaviors for check_private_dir() on encountering a nonexistent
 * directory; see that function's documentation for details. */
typedef unsigned int cpd_check_t;
#define CPD_NONE 0
#define CPD_CREATE 1
#define CPD_CHECK 2
#define CPD_GROUP_OK 4
#define CPD_CHECK_MODE_ONLY 8
int check_private_dir(const char *dirname, cpd_check_t check,
                      const char *effective_user);
#define OPEN_FLAGS_REPLACE (O_WRONLY|O_CREAT|O_TRUNC)
#define OPEN_FLAGS_APPEND (O_WRONLY|O_CREAT|O_APPEND)
#define OPEN_FLAGS_DONT_REPLACE (O_CREAT|O_EXCL|O_APPEND|O_WRONLY)
typedef struct open_file_t open_file_t;
int start_writing_to_file(const char *fname, int open_flags, int mode,
                          open_file_t **data_out);
FILE *start_writing_to_stdio_file(const char *fname, int open_flags, int mode,
                                  open_file_t **data_out);
FILE *fdopen_file(open_file_t *file_data);
int finish_writing_to_file(open_file_t *file_data);
int abort_writing_to_file(open_file_t *file_data);
int write_str_to_file(const char *fname, const char *str, int bin);
int write_bytes_to_file(const char *fname, const char *str, size_t len,
                        int bin);
/** An ad-hoc type to hold a string of characters and a count; used by
 * write_chunks_to_file. */
typedef struct sized_chunk_t {
  const char *bytes;
  size_t len;
} sized_chunk_t;
int write_chunks_to_file(const char *fname, const struct smartlist_t *chunks,
                         int bin);
int append_bytes_to_file(const char *fname, const char *str, size_t len,
                         int bin);
int write_bytes_to_new_file(const char *fname, const char *str, size_t len,
                            int bin);

/** Flag for read_file_to_str: open the file in binary mode. */
#define RFTS_BIN            1
/** Flag for read_file_to_str: it's okay if the file doesn't exist. */
#define RFTS_IGNORE_MISSING 2

#ifndef _WIN32
struct stat;
#endif
char *read_file_to_str(const char *filename, int flags, struct stat *stat_out)
  ATTR_MALLOC;
const char *parse_config_line_from_str(const char *line,
                                       char **key_out, char **value_out);
char *expand_filename(const char *filename);
struct smartlist_t *tor_listdir(const char *dirname);
int path_is_relative(const char *filename);

/* Process helpers */
void start_daemon(void);
void finish_daemon(const char *desired_cwd);
void write_pidfile(char *filename);

/* Port forwarding */
void tor_check_port_forwarding(const char *filename,
                               int dir_port, int or_port, time_t now);

typedef struct process_handle_t process_handle_t;
typedef struct process_environment_t process_environment_t;
int tor_spawn_background(const char *const filename, const char **argv,
                         process_environment_t *env,
                         process_handle_t **process_handle_out);

#define SPAWN_ERROR_MESSAGE "ERR: Failed to spawn background process - code "

#ifdef _WIN32
HANDLE load_windows_system_library(const TCHAR *library_name);
#endif

int environment_variable_names_equal(const char *s1, const char *s2);

/* DOCDOC process_environment_t */
struct process_environment_t {
  /** A pointer to a sorted empty-string-terminated sequence of
   * NUL-terminated strings of the form "NAME=VALUE". */
  char *windows_environment_block;
  /** A pointer to a NULL-terminated array of pointers to
   * NUL-terminated strings of the form "NAME=VALUE". */
  char **unixoid_environment_block;
};

process_environment_t *process_environment_make(struct smartlist_t *env_vars);
void process_environment_free(process_environment_t *env);

struct smartlist_t *get_current_process_environment_variables(void);

void set_environment_variable_in_smartlist(struct smartlist_t *env_vars,
                                           const char *new_var,
                                           void (*free_old)(void*),
                                           int free_p);

/* Values of process_handle_t.status. PROCESS_STATUS_NOTRUNNING must be
 * 0 because tor_check_port_forwarding depends on this being the initial
 * statue of the static instance of process_handle_t */
#define PROCESS_STATUS_NOTRUNNING 0
#define PROCESS_STATUS_RUNNING 1
#define PROCESS_STATUS_ERROR -1

#ifdef UTIL_PRIVATE
/** Structure to represent the state of a process with which Tor is
 * communicating. The contents of this structure are private to util.c */
struct process_handle_t {
  /** One of the PROCESS_STATUS_* values */
  int status;
#ifdef _WIN32
  HANDLE stdout_pipe;
  HANDLE stderr_pipe;
  PROCESS_INFORMATION pid;
#else
  int stdout_pipe;
  int stderr_pipe;
  FILE *stdout_handle;
  FILE *stderr_handle;
  pid_t pid;
#endif // _WIN32
};
#endif

/* Return values of tor_get_exit_code() */
#define PROCESS_EXIT_RUNNING 1
#define PROCESS_EXIT_EXITED 0
#define PROCESS_EXIT_ERROR -1
int tor_get_exit_code(const process_handle_t *process_handle,
                      int block, int *exit_code);
int tor_split_lines(struct smartlist_t *sl, char *buf, int len);
#ifdef _WIN32
ssize_t tor_read_all_handle(HANDLE h, char *buf, size_t count,
                            const process_handle_t *process);
#else
ssize_t tor_read_all_handle(FILE *h, char *buf, size_t count,
                            const process_handle_t *process,
                            int *eof);
#endif
ssize_t tor_read_all_from_process_stdout(
    const process_handle_t *process_handle, char *buf, size_t count);
ssize_t tor_read_all_from_process_stderr(
    const process_handle_t *process_handle, char *buf, size_t count);
char *tor_join_win_cmdline(const char *argv[]);

int tor_process_get_pid(process_handle_t *process_handle);
#ifdef _WIN32
HANDLE tor_process_get_stdout_pipe(process_handle_t *process_handle);
#else
FILE *tor_process_get_stdout_pipe(process_handle_t *process_handle);
#endif

int tor_terminate_process(process_handle_t *process_handle);
void tor_process_handle_destroy(process_handle_t *process_handle,
                                int also_terminate_process);

#ifdef UTIL_PRIVATE
/* Prototypes for private functions only used by util.c (and unit tests) */

int format_hex_number_for_helper_exit_status(unsigned int x, char *buf,
                                             int max_len);
int format_helper_exit_status(unsigned char child_state,
                              int saved_errno, char *hex_errno);

/* Space for hex values of child state, a slash, saved_errno (with
   leading minus) and newline (no null) */
#define HEX_ERRNO_SIZE (sizeof(char) * 2 + 1 + \
                        1 + sizeof(int) * 2 + 1)
#endif

const char *libor_get_digests(void);

#endif

