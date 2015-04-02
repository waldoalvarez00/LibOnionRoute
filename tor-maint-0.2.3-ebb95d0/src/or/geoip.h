/* Copyright (c) 2001 Matej Pfajfar.
 * Copyright (c) 2001-2004, Roger Dingledine.
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2012, The Tor Project, Inc. */
/* See LICENSE for licensing information */

/**
 * \file geoip.h
 * \brief Header file for geoip.c.
 **/

#ifndef _TOR_GEOIP_H
#define _TOR_GEOIP_H

#ifdef GEOIP_PRIVATE
int geoip_parse_entry(const char *line);
#endif
int should_record_bridge_info(const or_options_t *options);
int geoip_load_file(const char *filename, const or_options_t *options);
int geoip_get_country_by_ip(uint32_t ipaddr);
int geoip_get_country_by_addr(const tor_addr_t *addr);
int geoip_get_n_countries(void);
const char *geoip_get_country_name(country_t num);
int geoip_is_loaded(void);
const char *geoip_db_digest(void);
country_t geoip_get_country(const char *countrycode);

void geoip_note_client_seen(geoip_client_action_t action,
                            const tor_addr_t *addr, time_t now);
void geoip_remove_old_clients(time_t cutoff);

void geoip_note_ns_response(geoip_client_action_t action,
                            geoip_ns_response_t response);
char *geoip_get_client_history(geoip_client_action_t action);
char *geoip_get_request_history(geoip_client_action_t action);
int getinfo_helper_geoip(control_connection_t *control_conn,
                         const char *question, char **answer,
                         const char **errmsg);
void geoip_free_all(void);

void geoip_start_dirreq(uint64_t dirreq_id, size_t response_size,
                        geoip_client_action_t action, dirreq_type_t type);
void geoip_change_dirreq_state(uint64_t dirreq_id, dirreq_type_t type,
                               dirreq_state_t new_state);

void geoip_dirreq_stats_init(time_t now);
void geoip_reset_dirreq_stats(time_t now);
char *geoip_format_dirreq_stats(time_t now);
time_t geoip_dirreq_stats_write(time_t now);
void geoip_dirreq_stats_term(void);
void geoip_entry_stats_init(time_t now);
time_t geoip_entry_stats_write(time_t now);
void geoip_entry_stats_term(void);
void geoip_reset_entry_stats(time_t now);
char *geoip_format_entry_stats(time_t now);
void geoip_bridge_stats_init(time_t now);
char *geoip_format_bridge_stats(time_t now);
time_t geoip_bridge_stats_write(time_t now);
void geoip_bridge_stats_term(void);
const char *geoip_get_bridge_stats_extrainfo(time_t);
char *geoip_get_bridge_stats_controller(time_t);

#endif

