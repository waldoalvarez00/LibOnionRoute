/* Copyright (c) 2001 Matej Pfajfar.
 * Copyright (c) 2001-2004, Roger Dingledine.
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2012, The Tor Project, Inc. */
/* See LICENSE for licensing information */

/**
 * \file config.h
 * \brief Header file for config.c.
 **/

#ifndef _TOR_CONFIG_H
#define _TOR_CONFIG_H

const char *get_dirportfrontpage(void);
const or_options_t *get_options(void);
or_options_t *get_options_mutable(void);
int set_options(or_options_t *new_val, char **msg);
void config_free_all(void);
const char *safe_str_client(const char *address);
const char *safe_str(const char *address);
const char *escaped_safe_str_client(const char *address);
const char *escaped_safe_str(const char *address);
const char *get_version(void);
const char *get_short_version(void);

int config_get_lines(const char *string, config_line_t **result, int extended);
void config_free_lines(config_line_t *front);
setopt_err_t options_trial_assign(config_line_t *list, int use_defaults,
                                  int clear_first, char **msg);
int resolve_my_address(int warn_severity, const or_options_t *options,
                       uint32_t *addr, char **hostname_out);
int is_local_addr(const tor_addr_t *addr);
void options_init(or_options_t *options);
char *options_dump(const or_options_t *options, int minimal);
int options_init_from_torrc(int argc, char **argv);
setopt_err_t options_init_from_string(const char *cf_defaults, const char *cf,
                            int command, const char *command_arg, char **msg);
int option_is_recognized(const char *key);
const char *option_get_canonical_name(const char *key);
config_line_t *option_get_assignment(const or_options_t *options,
                                     const char *key);
int options_save_current(void);
const char *get_torrc_fname(int defaults_fname);
char *options_get_datadir_fname2_suffix(const or_options_t *options,
                                        const char *sub1, const char *sub2,
                                        const char *suffix);
#define get_datadir_fname2_suffix(sub1, sub2, suffix) \
  options_get_datadir_fname2_suffix(get_options(), (sub1), (sub2), (suffix))
/** Return a newly allocated string containing datadir/sub1.  See
 * get_datadir_fname2_suffix.  */
#define get_datadir_fname(sub1) get_datadir_fname2_suffix((sub1), NULL, NULL)
/** Return a newly allocated string containing datadir/sub1/sub2.  See
 * get_datadir_fname2_suffix.  */
#define get_datadir_fname2(sub1,sub2) \
  get_datadir_fname2_suffix((sub1), (sub2), NULL)
/** Return a newly allocated string containing datadir/sub1suffix.  See
 * get_datadir_fname2_suffix. */
#define get_datadir_fname_suffix(sub1, suffix) \
  get_datadir_fname2_suffix((sub1), NULL, (suffix))

int get_num_cpus(const or_options_t *options);

or_state_t *get_or_state(void);
int did_last_state_file_write_fail(void);
int or_state_save(time_t now);

const smartlist_t *get_configured_ports(void);
int get_first_advertised_port_by_type_af(int listener_type,
                                         int address_family);
#define get_primary_or_port() \
  (get_first_advertised_port_by_type_af(CONN_TYPE_OR_LISTENER, AF_INET))
#define get_primary_dir_port() \
  (get_first_advertised_port_by_type_af(CONN_TYPE_DIR_LISTENER, AF_INET))

char *get_first_listener_addrport_string(int listener_type);

int options_need_geoip_info(const or_options_t *options,
                            const char **reason_out);

void save_transport_to_state(const char *transport_name,
                             const tor_addr_t *addr, uint16_t port);
char *get_stored_bindaddr_for_server_transport(const char *transport);

int getinfo_helper_config(control_connection_t *conn,
                          const char *question, char **answer,
                          const char **errmsg);

const char *tor_get_digests(void);
uint32_t get_effective_bwrate(const or_options_t *options);
uint32_t get_effective_bwburst(const or_options_t *options);

#ifdef CONFIG_PRIVATE
/* Used only by config.c and test.c */
or_options_t *options_new(void);
#endif

void config_register_addressmaps(const or_options_t *options);
/* XXXX024 move to connection_edge.h */
int addressmap_register_auto(const char *from, const char *to,
                             time_t expires,
                             addressmap_entry_source_t addrmap_source,
                             const char **msg);

#endif

