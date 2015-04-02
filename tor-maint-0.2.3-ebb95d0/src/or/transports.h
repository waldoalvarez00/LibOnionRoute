/* Copyright (c) 2003-2004, Roger Dingledine
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2012, The Tor Project, Inc. */
/* See LICENSE for licensing information */

/**
 * \file transports.h
 * \brief Headers for transports.c
 **/

#ifndef TOR_TRANSPORTS_H
#define TOR_TRANSPORTS_H

void pt_kickstart_proxy(const smartlist_t *transport_list, char **proxy_argv,
                        int is_server);

#define pt_kickstart_client_proxy(tl, pa)  \
  pt_kickstart_proxy(tl, pa, 0)
#define pt_kickstart_server_proxy(tl, pa) \
  pt_kickstart_proxy(tl, pa, 1)

void pt_configure_remaining_proxies(void);

int pt_proxies_configuration_pending(void);

void pt_free_all(void);

void pt_prepare_proxy_list_for_config_read(void);
void sweep_proxy_list(void);

#ifdef PT_PRIVATE
/** State of the managed proxy configuration protocol. */
enum pt_proto_state {
  PT_PROTO_INFANT, /* was just born */
  PT_PROTO_LAUNCHED, /* was just launched */
  PT_PROTO_ACCEPTING_METHODS, /* accepting methods */
  PT_PROTO_CONFIGURED, /* configured successfully */
  PT_PROTO_COMPLETED, /* configure and registered its transports */
  PT_PROTO_BROKEN, /* broke during the protocol */
  PT_PROTO_FAILED_LAUNCH /* failed while launching */
};

/** Structure containing information of a managed proxy. */
typedef struct {
  enum pt_proto_state conf_state; /* the current configuration state */
  char **argv; /* the cli arguments of this proxy */
  int conf_protocol; /* the configuration protocol version used */

  int is_server; /* is it a server proxy? */

  /* A pointer to the process handle of this managed proxy. */
  process_handle_t *process_handle;

  int pid; /* The Process ID this managed proxy is using. */

  /** Boolean: We are re-parsing our config, and we are going to
   * remove this managed proxy if we don't find it any transport
   * plugins that use it. */
  unsigned int marked_for_removal : 1;

  /** Boolean: We got a SIGHUP while this proxy was running. We use
   * this flag to signify that this proxy might need to be restarted
   * so that it can listen for other transports according to the new
   * torrc. */
  unsigned int got_hup : 1;

  /* transports to-be-launched by this proxy */
  smartlist_t *transports_to_launch;

  /* The 'transports' list contains all the transports this proxy has
     launched.

     Before a managed_proxy_t reaches the PT_PROTO_COMPLETED phase,
     this smartlist contains a 'transport_t' for every transport it
     has launched.

     When the managed_proxy_t reaches the PT_PROTO_COMPLETED phase, it
     registers all its transports to the circuitbuild.c subsystem. At
     that point the 'transport_t's are owned by the circuitbuild.c
     subsystem.

     To avoid carrying dangling 'transport_t's in this smartlist,
     right before the managed_proxy_t reaches the PT_PROTO_COMPLETED
     phase we replace all 'transport_t's with strings of their
     transport names.

     So, tl;dr:
     When (conf_state != PT_PROTO_COMPLETED) this list carries
     (transport_t *).
     When (conf_state == PT_PROTO_COMPLETED) this list carries
     (char *).
   */
  smartlist_t *transports;
} managed_proxy_t;

int parse_cmethod_line(const char *line, managed_proxy_t *mp);
int parse_smethod_line(const char *line, managed_proxy_t *mp);

int parse_version(const char *line, managed_proxy_t *mp);
void parse_env_error(const char *line);
void handle_proxy_line(const char *line, managed_proxy_t *mp);

#endif

#endif

