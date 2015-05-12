/* Copyright (c) 2013 Waldo Alvarez Cañizares, http://onionroute.org */
/* See LICENSE for licensing information */

#ifndef _LIBONIONROUTE_H_
#define _LIBONIONROUTE_H_

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

//#include "common\torint.h"

#ifndef HAVE_UINT32_T
typedef unsigned int uint32_t;
#define HAVE_UINT32_T
#endif

#ifdef _WIN32

#define tor_socket_t intptr_t
#define SOCKET_OK(s) ((SOCKET)(s) != INVALID_SOCKET)
#define TOR_INVALID_SOCKET INVALID_SOCKET

#ifdef STATIC
#define ONIONROUTE_API
#else
#ifdef DLL_EXPORTS
#if defined(__GNUC__)
  #define ONIONROUTE_API __attribute__ ((__visibility__("default")))
#elif defined(WIN32)
  #define ONIONROUTE_API __declspec(dllexport)
#endif
#else
#define ONIONROUTE_API __declspec(dllimport)
#endif
#endif

#else
/** Type used for a network socket. */
#define tor_socket_t int
/** Macro: true iff 's' is a possible value for a valid initialized socket. */
#define SOCKET_OK(s) ((s) >= 0)
/** Error/uninitialized value for a tor_socket_t. */
#define TOR_INVALID_SOCKET (-1)

#define ONIONROUTE_API

#endif

/** Debug-level severity: for hyper-verbose messages of no interest to
 * anybody but developers. */
#define LOG_DEBUG   7
/** Info-level severity: for messages that appear frequently during normal
 * operation. */
#define LOG_INFO    6
/** Notice-level severity: for messages that appear infrequently
 * during normal operation; that the user will probably care about;
 * and that are not errors.
 */
#define LOG_NOTICE  5
/** Warn-level severity: for messages that only appear when something has gone
 * wrong. */
#define LOG_WARN    4
/** Error-level severity: for messages that only appear when something has gone
 * very wrong, and the Tor process can no longer proceed. */
#define LOG_ERR     3

/** Enum describing various stages of bootstrapping, The values range from 0 to 100. */
typedef enum {
  BOOTSTRAP_STATUS_UNDEF=-1,
  BOOTSTRAP_STATUS_STARTING=0,
  BOOTSTRAP_STATUS_CONN_DIR=5,
  BOOTSTRAP_STATUS_HANDSHAKE=-2,
  BOOTSTRAP_STATUS_HANDSHAKE_DIR=10,
  BOOTSTRAP_STATUS_ONEHOP_CREATE=15,
  BOOTSTRAP_STATUS_REQUESTING_STATUS=20,
  BOOTSTRAP_STATUS_LOADING_STATUS=25,
  BOOTSTRAP_STATUS_LOADING_KEYS=40,
  BOOTSTRAP_STATUS_REQUESTING_DESCRIPTORS=45,
  BOOTSTRAP_STATUS_LOADING_DESCRIPTORS=50,
  BOOTSTRAP_STATUS_CONN_OR=80,
  BOOTSTRAP_STATUS_HANDSHAKE_OR=85,
  BOOTSTRAP_STATUS_CIRCUIT_CREATE=90,
  BOOTSTRAP_STATUS_DONE=100
} bootstrap_status_t_v1;

/** initializes library */
ONIONROUTE_API
int
onionroute_init_v1();

ONIONROUTE_API
int onionroute_shutdown_v1();

/** perform library main loop */
ONIONROUTE_API
int
onionroute_do_main_loop_v1(void);

/** Mask of zero or more log domains, OR'd together. */
typedef uint32_t log_domain_mask_t_v1;

/** bootstrap notification */
typedef void (*onionroute_event_bootstrap_t_v1)(bootstrap_status_t_v1 status, int progress);

/** use to receive bootstrap notifications */
ONIONROUTE_API
void
onionroute_set_bootstrap_callback_v1(onionroute_event_bootstrap_t_v1 callback);

/* logging */
typedef void (*onionroute_log_callback_t_v1)(int severity, log_domain_mask_t_v1 domain, const char *funcname, const char *format, va_list ap);

ONIONROUTE_API
void
onionroute_set_log_callback_v1(onionroute_log_callback_t_v1 c);

/* configuration */
/* this API is subject to change */
ONIONROUTE_API
int
onionroute_setconf(char *body, int use_defaults);

/* misc */

ONIONROUTE_API
char * 
onionroute_format_msg_v1(char *buf, size_t buf_len,
           log_domain_mask_t_v1 domain, int severity, const char *funcname,
           const char *format, va_list ap, size_t *msg_len_out);

/* stream close */
typedef void (*onionroute_event_stream_close_t_v1)(void*);
typedef void (*onionroute_event_stream_close_t_v2)(void*, void*);

ONIONROUTE_API
void
onionroute_set_stream_close_callback_v1(onionroute_event_stream_close_t_v1 callback);

ONIONROUTE_API
void
onionroute_set_stream_close_callback_v2(onionroute_event_stream_close_t_v2 callback);

/* stream open */
typedef void (*onionroute_event_stream_open_t_v1)(void *);

ONIONROUTE_API
void
onionroute_set_stream_open_callback_v1(onionroute_event_stream_open_t_v1 callback);

typedef void (*onionroute_event_stream_open_t_v2)(void *, void *);

ONIONROUTE_API
void
onionroute_set_stream_open_callback_v2(onionroute_event_stream_open_t_v2 callback);



/* stream read */
typedef void (*onionroute_event_stream_data_received_t_v1)(void *, size_t len, char* data);
typedef void (*onionroute_event_stream_data_received_t_v2)(void *, void*, size_t len, char* data);

ONIONROUTE_API
void
onionroute_set_stream_data_received_callback_v1(onionroute_event_stream_data_received_t_v1 callback);

ONIONROUTE_API
void
onionroute_set_stream_data_received_callback_v2(onionroute_event_stream_data_received_t_v2 callback);





ONIONROUTE_API
int onionroute_stream_open_v1(char *addr, int port);

ONIONROUTE_API
int onionroute_stream_open_v2(char *addr, int port, void *obj);

ONIONROUTE_API
int
onionroute_closestream_v1(void *id);

ONIONROUTE_API int onionroute_stream_write_v1(void *id, char* data, int size);
ONIONROUTE_API int onionroute_stream_printf_v1(void *id, const char *format, ...);
ONIONROUTE_API int onionroute_stream_flush_v1(void *id);



/* control */
ONIONROUTE_API int onionroute_clear_dns_cache_signal_v1();
ONIONROUTE_API int onionroute_switch_to_new_circuits_v1();





/* this API function is to easily replace code using sockets like read(socket, ...) as the library is async */
ONIONROUTE_API
int
onionroute_recv_stream_data_v1(void *id, char* buffer, size_t buffersize);

/* send data to the queque so it can be read */
ONIONROUTE_API
int
onionroute_queue_recvd_data_v1(void *id, size_t len, char* data);

ONIONROUTE_API
int
onionroute_queue_closed_stream_v1(void *id);

#ifdef __cplusplus
}
#endif

#endif