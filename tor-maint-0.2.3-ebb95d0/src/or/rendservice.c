/* Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2012, The Tor Project, Inc. */
/* See LICENSE for licensing information */

/**
 * \file rendservice.c
 * \brief The hidden-service side of rendezvous functionality.
 **/

#include "or.h"
#include "circuitbuild.h"
#include "circuitlist.h"
#include "circuituse.h"
#include "config.h"
#include "directory.h"
#include "networkstatus.h"
#include "nodelist.h"
#include "rendclient.h"
#include "rendcommon.h"
#include "rendservice.h"
#include "router.h"
#include "relay.h"
#include "rephist.h"
#include "routerlist.h"
#include "routerparse.h"

static origin_circuit_t *find_intro_circuit(rend_intro_point_t *intro,
                                            const char *pk_digest);
static rend_intro_point_t *find_intro_point(origin_circuit_t *circ);

static int intro_point_accepted_intro_count(rend_intro_point_t *intro);
static int intro_point_should_expire_now(rend_intro_point_t *intro,
                                         time_t now);

/** Represents the mapping from a virtual port of a rendezvous service to
 * a real port on some IP.
 */
typedef struct rend_service_port_config_t {
  uint16_t virtual_port;
  uint16_t real_port;
  tor_addr_t real_addr;
} rend_service_port_config_t;

/** Try to maintain this many intro points per service by default. */
#define NUM_INTRO_POINTS_DEFAULT 3
/** Maintain no more than this many intro points per hidden service. */
#define NUM_INTRO_POINTS_MAX 10

/** If we can't build our intro circuits, don't retry for this long. */
#define INTRO_CIRC_RETRY_PERIOD (60*5)
/** Don't try to build more than this many circuits before giving up
 * for a while.*/
#define MAX_INTRO_CIRCS_PER_PERIOD 10
/** How many times will a hidden service operator attempt to connect to
 * a requested rendezvous point before giving up? */
#define MAX_REND_FAILURES 30
/** How many seconds should we spend trying to connect to a requested
 * rendezvous point before giving up? */
#define MAX_REND_TIMEOUT 30

/** How many seconds should we wait for new HS descriptors to reach
 * our clients before we close an expiring intro point? */
#define INTRO_POINT_EXPIRATION_GRACE_PERIOD 5*60

/** Represents a single hidden service running at this OP. */
typedef struct rend_service_t {
  /* Fields specified in config file */
  char *directory; /**< where in the filesystem it stores it */
  smartlist_t *ports; /**< List of rend_service_port_config_t */
  rend_auth_type_t auth_type; /**< Client authorization type or 0 if no client
                               * authorization is performed. */
  smartlist_t *clients; /**< List of rend_authorized_client_t's of
                         * clients that may access our service. Can be NULL
                         * if no client authorization is performed. */
  /* Other fields */
  crypto_pk_t *private_key; /**< Permanent hidden-service key. */
  char service_id[REND_SERVICE_ID_LEN_BASE32+1]; /**< Onion address without
                                                  * '.onion' */
  char pk_digest[DIGEST_LEN]; /**< Hash of permanent hidden-service key. */
  smartlist_t *intro_nodes; /**< List of rend_intro_point_t's we have,
                             * or are trying to establish. */
  time_t intro_period_started; /**< Start of the current period to build
                                * introduction points. */
  int n_intro_circuits_launched; /**< Count of intro circuits we have
                                  * established in this period. */
  unsigned int n_intro_points_wanted; /**< Number of intro points this
                                       * service wants to have open. */
  rend_service_descriptor_t *desc; /**< Current hidden service descriptor. */
  time_t desc_is_dirty; /**< Time at which changes to the hidden service
                         * descriptor content occurred, or 0 if it's
                         * up-to-date. */
  time_t next_upload_time; /**< Scheduled next hidden service descriptor
                            * upload time. */
  /** Map from digests of Diffie-Hellman values INTRODUCE2 to time_t
   * of when they were received.  Clients may send INTRODUCE1 cells
   * for the same rendezvous point through two or more different
   * introduction points; when they do, this digestmap keeps us from
   * launching multiple simultaneous attempts to connect to the same
   * rend point. */
  digestmap_t *accepted_intro_dh_parts;
  /** Time at which we last removed expired values from
   * accepted_intro_dh_parts. */
  time_t last_cleaned_accepted_intro_dh_parts;
} rend_service_t;

/** A list of rend_service_t's for services run on this OP.
 */
static smartlist_t *rend_service_list = NULL;

/** Return the number of rendezvous services we have configured. */
int
num_rend_services(void)
{
  if (!rend_service_list)
    return 0;
  return smartlist_len(rend_service_list);
}

/** Return a string identifying <b>service</b>, suitable for use in a
 * log message.  The result does not need to be freed, but may be
 * overwritten by the next call to this function. */
static const char *
rend_service_describe_for_log(rend_service_t *service)
{
  /* XXX024 Use this function throughout rendservice.c. */
  /* XXX024 Return a more useful description? */
  return safe_str_client(service->service_id);
}

/** Helper: free storage held by a single service authorized client entry. */
static void
rend_authorized_client_free(rend_authorized_client_t *client)
{
  if (!client)
    return;
  if (client->client_key)
    crypto_pk_free(client->client_key);
  tor_free(client->client_name);
  tor_free(client);
}

/** Helper for strmap_free. */
static void
rend_authorized_client_strmap_item_free(void *authorized_client)
{
  rend_authorized_client_free(authorized_client);
}

/** Release the storage held by <b>service</b>.
 */
static void
rend_service_free(rend_service_t *service)
{
  if (!service)
    return;

  tor_free(service->directory);
  SMARTLIST_FOREACH(service->ports, void*, p, tor_free(p));
  smartlist_free(service->ports);
  if (service->private_key)
    crypto_pk_free(service->private_key);
  if (service->intro_nodes) {
    SMARTLIST_FOREACH(service->intro_nodes, rend_intro_point_t *, intro,
      rend_intro_point_free(intro););
    smartlist_free(service->intro_nodes);
  }

  rend_service_descriptor_free(service->desc);
  if (service->clients) {
    SMARTLIST_FOREACH(service->clients, rend_authorized_client_t *, c,
      rend_authorized_client_free(c););
    smartlist_free(service->clients);
  }
  digestmap_free(service->accepted_intro_dh_parts, _tor_free);
  tor_free(service);
}

/** Release all the storage held in rend_service_list.
 */
void
rend_service_free_all(void)
{
  if (!rend_service_list)
    return;

  SMARTLIST_FOREACH(rend_service_list, rend_service_t*, ptr,
                    rend_service_free(ptr));
  smartlist_free(rend_service_list);
  rend_service_list = NULL;
}

/** Validate <b>service</b> and add it to rend_service_list if possible.
 */
static void
rend_add_service(rend_service_t *service)
{
  int i;
  rend_service_port_config_t *p;

  service->intro_nodes = smartlist_new();

  if (service->auth_type != REND_NO_AUTH &&
      smartlist_len(service->clients) == 0) {
    log_warn(LD_CONFIG, "Hidden service (%s) with client authorization but no "
                        "clients; ignoring.",
             escaped(service->directory));
    rend_service_free(service);
    return;
  }

  if (!smartlist_len(service->ports)) {
    log_warn(LD_CONFIG, "Hidden service (%s) with no ports configured; "
             "ignoring.",
             escaped(service->directory));
    rend_service_free(service);
  } else {
    int dupe = 0;
    /* XXX This duplicate check has two problems:
     *
     * a) It's O(n^2), but the same comment from the bottom of
     *    rend_config_services() should apply.
     *
     * b) We only compare directory paths as strings, so we can't
     *    detect two distinct paths that specify the same directory
     *    (which can arise from symlinks, case-insensitivity, bind
     *    mounts, etc.).
     *
     * It also can't detect that two separate Tor instances are trying
     * to use the same HiddenServiceDir; for that, we would need a
     * lock file.  But this is enough to detect a simple mistake that
     * at least one person has actually made.
     */
    SMARTLIST_FOREACH(rend_service_list, rend_service_t*, ptr,
                      dupe = dupe ||
                             !strcmp(ptr->directory, service->directory));
    if (dupe) {
      log_warn(LD_REND, "Another hidden service is already configured for "
               "directory %s, ignoring.", service->directory);
      rend_service_free(service);
      return;
    }
    smartlist_add(rend_service_list, service);
    log_debug(LD_REND,"Configuring service with directory \"%s\"",
              service->directory);
    for (i = 0; i < smartlist_len(service->ports); ++i) {
      p = smartlist_get(service->ports, i);
      log_debug(LD_REND,"Service maps port %d to %s:%d",
                p->virtual_port, fmt_addr(&p->real_addr), p->real_port);
    }
  }
}

/** Parses a real-port to virtual-port mapping and returns a new
 * rend_service_port_config_t.
 *
 * The format is: VirtualPort (IP|RealPort|IP:RealPort)?
 *
 * IP defaults to 127.0.0.1; RealPort defaults to VirtualPort.
 */
static rend_service_port_config_t *
parse_port_config(const char *string)
{
  smartlist_t *sl;
  int virtport;
  int realport;
  uint16_t p;
  tor_addr_t addr;
  const char *addrport;
  rend_service_port_config_t *result = NULL;

  sl = smartlist_new();
  smartlist_split_string(sl, string, " ",
                         SPLIT_SKIP_SPACE|SPLIT_IGNORE_BLANK, 0);
  if (smartlist_len(sl) < 1 || smartlist_len(sl) > 2) {
    log_warn(LD_CONFIG, "Bad syntax in hidden service port configuration.");
    goto err;
  }

  virtport = (int)tor_parse_long(smartlist_get(sl,0), 10, 1, 65535, NULL,NULL);
  if (!virtport) {
    log_warn(LD_CONFIG, "Missing or invalid port %s in hidden service port "
             "configuration", escaped(smartlist_get(sl,0)));
    goto err;
  }

  if (smartlist_len(sl) == 1) {
    /* No addr:port part; use default. */
    realport = virtport;
    tor_addr_from_ipv4h(&addr, 0x7F000001u); /* 127.0.0.1 */
  } else {
    addrport = smartlist_get(sl,1);
    if (strchr(addrport, ':') || strchr(addrport, '.')) {
      if (tor_addr_port_lookup(addrport, &addr, &p)<0) {
        log_warn(LD_CONFIG,"Unparseable address in hidden service port "
                 "configuration.");
        goto err;
      }
      realport = p?p:virtport;
    } else {
      /* No addr:port, no addr -- must be port. */
      realport = (int)tor_parse_long(addrport, 10, 1, 65535, NULL, NULL);
      if (!realport) {
        log_warn(LD_CONFIG,"Unparseable or out-of-range port %s in hidden "
                 "service port configuration.", escaped(addrport));
        goto err;
      }
      tor_addr_from_ipv4h(&addr, 0x7F000001u); /* Default to 127.0.0.1 */
    }
  }

  result = tor_malloc(sizeof(rend_service_port_config_t));
  result->virtual_port = virtport;
  result->real_port = realport;
  tor_addr_copy(&result->real_addr, &addr);
 err:
  SMARTLIST_FOREACH(sl, char *, c, tor_free(c));
  smartlist_free(sl);
  return result;
}

/** Set up rend_service_list, based on the values of HiddenServiceDir and
 * HiddenServicePort in <b>options</b>.  Return 0 on success and -1 on
 * failure.  (If <b>validate_only</b> is set, parse, warn and return as
 * normal, but don't actually change the configured services.)
 */
int
rend_config_services(const or_options_t *options, int validate_only)
{
  config_line_t *line;
  rend_service_t *service = NULL;
  rend_service_port_config_t *portcfg;
  smartlist_t *old_service_list = NULL;

  if (!validate_only) {
    old_service_list = rend_service_list;
    rend_service_list = smartlist_new();
  }

  for (line = options->RendConfigLines; line; line = line->next) {
    if (!strcasecmp(line->key, "HiddenServiceDir")) {
      if (service) { /* register the one we just finished parsing */
        if (validate_only)
          rend_service_free(service);
        else
          rend_add_service(service);
      }
      service = tor_malloc_zero(sizeof(rend_service_t));
      service->directory = tor_strdup(line->value);
      service->ports = smartlist_new();
      service->intro_period_started = time(NULL);
      service->n_intro_points_wanted = NUM_INTRO_POINTS_DEFAULT;
      continue;
    }
    if (!service) {
      log_warn(LD_CONFIG, "%s with no preceding HiddenServiceDir directive",
               line->key);
      rend_service_free(service);
      return -1;
    }
    if (!strcasecmp(line->key, "HiddenServicePort")) {
      portcfg = parse_port_config(line->value);
      if (!portcfg) {
        rend_service_free(service);
        return -1;
      }
      smartlist_add(service->ports, portcfg);
    } else if (!strcasecmp(line->key, "HiddenServiceAuthorizeClient")) {
      /* Parse auth type and comma-separated list of client names and add a
       * rend_authorized_client_t for each client to the service's list
       * of authorized clients. */
      smartlist_t *type_names_split, *clients;
      const char *authname;
      int num_clients;
      if (service->auth_type != REND_NO_AUTH) {
        log_warn(LD_CONFIG, "Got multiple HiddenServiceAuthorizeClient "
                 "lines for a single service.");
        rend_service_free(service);
        return -1;
      }
      type_names_split = smartlist_new();
      smartlist_split_string(type_names_split, line->value, " ", 0, 2);
      if (smartlist_len(type_names_split) < 1) {
        log_warn(LD_BUG, "HiddenServiceAuthorizeClient has no value. This "
                         "should have been prevented when parsing the "
                         "configuration.");
        smartlist_free(type_names_split);
        rend_service_free(service);
        return -1;
      }
      authname = smartlist_get(type_names_split, 0);
      if (!strcasecmp(authname, "basic")) {
        service->auth_type = REND_BASIC_AUTH;
      } else if (!strcasecmp(authname, "stealth")) {
        service->auth_type = REND_STEALTH_AUTH;
      } else {
        log_warn(LD_CONFIG, "HiddenServiceAuthorizeClient contains "
                 "unrecognized auth-type '%s'. Only 'basic' or 'stealth' "
                 "are recognized.",
                 (char *) smartlist_get(type_names_split, 0));
        SMARTLIST_FOREACH(type_names_split, char *, cp, tor_free(cp));
        smartlist_free(type_names_split);
        rend_service_free(service);
        return -1;
      }
      service->clients = smartlist_new();
      if (smartlist_len(type_names_split) < 2) {
        log_warn(LD_CONFIG, "HiddenServiceAuthorizeClient contains "
                            "auth-type '%s', but no client names.",
                 service->auth_type == REND_BASIC_AUTH ? "basic" : "stealth");
        SMARTLIST_FOREACH(type_names_split, char *, cp, tor_free(cp));
        smartlist_free(type_names_split);
        continue;
      }
      clients = smartlist_new();
      smartlist_split_string(clients, smartlist_get(type_names_split, 1),
                             ",", SPLIT_SKIP_SPACE, 0);
      SMARTLIST_FOREACH(type_names_split, char *, cp, tor_free(cp));
      smartlist_free(type_names_split);
      /* Remove duplicate client names. */
      num_clients = smartlist_len(clients);
      smartlist_sort_strings(clients);
      smartlist_uniq_strings(clients);
      if (smartlist_len(clients) < num_clients) {
        log_info(LD_CONFIG, "HiddenServiceAuthorizeClient contains %d "
                            "duplicate client name(s); removing.",
                 num_clients - smartlist_len(clients));
        num_clients = smartlist_len(clients);
      }
      SMARTLIST_FOREACH_BEGIN(clients, const char *, client_name)
      {
        rend_authorized_client_t *client;
        size_t len = strlen(client_name);
        if (len < 1 || len > REND_CLIENTNAME_MAX_LEN) {
          log_warn(LD_CONFIG, "HiddenServiceAuthorizeClient contains an "
                              "illegal client name: '%s'. Length must be "
                              "between 1 and %d characters.",
                   client_name, REND_CLIENTNAME_MAX_LEN);
          SMARTLIST_FOREACH(clients, char *, cp, tor_free(cp));
          smartlist_free(clients);
          rend_service_free(service);
          return -1;
        }
        if (strspn(client_name, REND_LEGAL_CLIENTNAME_CHARACTERS) != len) {
          log_warn(LD_CONFIG, "HiddenServiceAuthorizeClient contains an "
                              "illegal client name: '%s'. Valid "
                              "characters are [A-Za-z0-9+_-].",
                   client_name);
          SMARTLIST_FOREACH(clients, char *, cp, tor_free(cp));
          smartlist_free(clients);
          rend_service_free(service);
          return -1;
        }
        client = tor_malloc_zero(sizeof(rend_authorized_client_t));
        client->client_name = tor_strdup(client_name);
        smartlist_add(service->clients, client);
        log_debug(LD_REND, "Adding client name '%s'", client_name);
      }
      SMARTLIST_FOREACH_END(client_name);
      SMARTLIST_FOREACH(clients, char *, cp, tor_free(cp));
      smartlist_free(clients);
      /* Ensure maximum number of clients. */
      if ((service->auth_type == REND_BASIC_AUTH &&
            smartlist_len(service->clients) > 512) ||
          (service->auth_type == REND_STEALTH_AUTH &&
            smartlist_len(service->clients) > 16)) {
        log_warn(LD_CONFIG, "HiddenServiceAuthorizeClient contains %d "
                            "client authorization entries, but only a "
                            "maximum of %d entries is allowed for "
                            "authorization type '%s'.",
                 smartlist_len(service->clients),
                 service->auth_type == REND_BASIC_AUTH ? 512 : 16,
                 service->auth_type == REND_BASIC_AUTH ? "basic" : "stealth");
        rend_service_free(service);
        return -1;
      }
    } else {
      tor_assert(!strcasecmp(line->key, "HiddenServiceVersion"));
      if (strcmp(line->value, "2")) {
        log_warn(LD_CONFIG,
                 "The only supported HiddenServiceVersion is 2.");
        rend_service_free(service);
        return -1;
      }
    }
  }
  if (service) {
    if (validate_only)
      rend_service_free(service);
    else
      rend_add_service(service);
  }

  /* If this is a reload and there were hidden services configured before,
   * keep the introduction points that are still needed and close the
   * other ones. */
  if (old_service_list && !validate_only) {
    smartlist_t *surviving_services = smartlist_new();
    circuit_t *circ;

    /* Copy introduction points to new services. */
    /* XXXX This is O(n^2), but it's only called on reconfigure, so it's
     * probably ok? */
    SMARTLIST_FOREACH_BEGIN(rend_service_list, rend_service_t *, new) {
      SMARTLIST_FOREACH_BEGIN(old_service_list, rend_service_t *, old) {
        if (!strcmp(old->directory, new->directory)) {
          smartlist_add_all(new->intro_nodes, old->intro_nodes);
          smartlist_clear(old->intro_nodes);
          smartlist_add(surviving_services, old);
          break;
        }
      } SMARTLIST_FOREACH_END(old);
    } SMARTLIST_FOREACH_END(new);

    /* Close introduction circuits of services we don't serve anymore. */
    /* XXXX it would be nicer if we had a nicer abstraction to use here,
     * so we could just iterate over the list of services to close, but
     * once again, this isn't critical-path code. */
    for (circ = _circuit_get_global_list(); circ; circ = circ->next) {
      if (!circ->marked_for_close &&
          circ->state == CIRCUIT_STATE_OPEN &&
          (circ->purpose == CIRCUIT_PURPOSE_S_ESTABLISH_INTRO ||
           circ->purpose == CIRCUIT_PURPOSE_S_INTRO)) {
        origin_circuit_t *oc = TO_ORIGIN_CIRCUIT(circ);
        int keep_it = 0;
        tor_assert(oc->rend_data);
        SMARTLIST_FOREACH(surviving_services, rend_service_t *, ptr, {
          if (tor_memeq(ptr->pk_digest, oc->rend_data->rend_pk_digest,
                      DIGEST_LEN)) {
            keep_it = 1;
            break;
          }
        });
        if (keep_it)
          continue;
        log_info(LD_REND, "Closing intro point %s for service %s.",
                 safe_str_client(extend_info_describe(
                                            oc->build_state->chosen_exit)),
                 oc->rend_data->onion_address);
        circuit_mark_for_close(circ, END_CIRC_REASON_FINISHED);
        /* XXXX Is there another reason we should use here? */
      }
    }
    smartlist_free(surviving_services);
    SMARTLIST_FOREACH(old_service_list, rend_service_t *, ptr,
                      rend_service_free(ptr));
    smartlist_free(old_service_list);
  }

  return 0;
}

/** Replace the old value of <b>service</b>-\>desc with one that reflects
 * the other fields in service.
 */
static void
rend_service_update_descriptor(rend_service_t *service)
{
  rend_service_descriptor_t *d;
  origin_circuit_t *circ;
  int i;

  rend_service_descriptor_free(service->desc);
  service->desc = NULL;

  d = service->desc = tor_malloc_zero(sizeof(rend_service_descriptor_t));
  d->pk = crypto_pk_dup_key(service->private_key);
  d->timestamp = time(NULL);
  d->intro_nodes = smartlist_new();
  /* Support intro protocols 2 and 3. */
  d->protocols = (1 << 2) + (1 << 3);

  for (i = 0; i < smartlist_len(service->intro_nodes); ++i) {
    rend_intro_point_t *intro_svc = smartlist_get(service->intro_nodes, i);
    rend_intro_point_t *intro_desc;

    /* This intro point won't be listed in the descriptor... */
    intro_svc->listed_in_last_desc = 0;

    if (intro_svc->time_expiring != -1) {
      /* This intro point is expiring.  Don't list it. */
      continue;
    }

    circ = find_intro_circuit(intro_svc, service->pk_digest);
    if (!circ || circ->_base.purpose != CIRCUIT_PURPOSE_S_INTRO) {
      /* This intro point's circuit isn't finished yet.  Don't list it. */
      continue;
    }

    /* ...unless this intro point is listed in the descriptor. */
    intro_svc->listed_in_last_desc = 1;

    /* We have an entirely established intro circuit.  Publish it in
     * our descriptor. */
    intro_desc = tor_malloc_zero(sizeof(rend_intro_point_t));
    intro_desc->extend_info = extend_info_dup(intro_svc->extend_info);
    if (intro_svc->intro_key)
      intro_desc->intro_key = crypto_pk_dup_key(intro_svc->intro_key);
    smartlist_add(d->intro_nodes, intro_desc);

    if (intro_svc->time_published == -1) {
      /* We are publishing this intro point in a descriptor for the
       * first time -- note the current time in the service's copy of
       * the intro point. */
      intro_svc->time_published = time(NULL);
    }
  }
}

/** Load and/or generate private keys for all hidden services, possibly
 * including keys for client authorization.  Return 0 on success, -1 on
 * failure.
 */
int
rend_service_load_keys(void)
{
  int r = 0;
  char fname[512];
  char buf[1500];

  SMARTLIST_FOREACH_BEGIN(rend_service_list, rend_service_t *, s) {
    if (s->private_key)
      continue;
    log_info(LD_REND, "Loading hidden-service keys from \"%s\"",
             s->directory);

    /* Check/create directory */
    if (check_private_dir(s->directory, CPD_CREATE, get_options()->User) < 0)
      return -1;

    /* Load key */
    if (strlcpy(fname,s->directory,sizeof(fname)) >= sizeof(fname) ||
        strlcat(fname,PATH_SEPARATOR"private_key",sizeof(fname))
                                                  >= sizeof(fname)) {
      log_warn(LD_CONFIG, "Directory name too long to store key file: \"%s\".",
               s->directory);
      return -1;
    }
    s->private_key = init_key_from_file(fname, 1, LOG_ERR);
    if (!s->private_key)
      return -1;

    /* Create service file */
    if (rend_get_service_id(s->private_key, s->service_id)<0) {
      log_warn(LD_BUG, "Internal error: couldn't encode service ID.");
      return -1;
    }
    if (crypto_pk_get_digest(s->private_key, s->pk_digest)<0) {
      log_warn(LD_BUG, "Couldn't compute hash of public key.");
      return -1;
    }
    if (strlcpy(fname,s->directory,sizeof(fname)) >= sizeof(fname) ||
        strlcat(fname,PATH_SEPARATOR"hostname",sizeof(fname))
                                                  >= sizeof(fname)) {
      log_warn(LD_CONFIG, "Directory name too long to store hostname file:"
               " \"%s\".", s->directory);
      return -1;
    }
    tor_snprintf(buf, sizeof(buf),"%s.onion\n", s->service_id);
    if (write_str_to_file(fname,buf,0)<0) {
      log_warn(LD_CONFIG, "Could not write onion address to hostname file.");
      return -1;
    }

    /* If client authorization is configured, load or generate keys. */
    if (s->auth_type != REND_NO_AUTH) {
      char *client_keys_str = NULL;
      strmap_t *parsed_clients = strmap_new();
      char cfname[512];
      FILE *cfile, *hfile;
      open_file_t *open_cfile = NULL, *open_hfile = NULL;

      /* Load client keys and descriptor cookies, if available. */
      if (tor_snprintf(cfname, sizeof(cfname), "%s"PATH_SEPARATOR"client_keys",
                       s->directory)<0) {
        log_warn(LD_CONFIG, "Directory name too long to store client keys "
                 "file: \"%s\".", s->directory);
        goto err;
      }
      client_keys_str = read_file_to_str(cfname, RFTS_IGNORE_MISSING, NULL);
      if (client_keys_str) {
        if (rend_parse_client_keys(parsed_clients, client_keys_str) < 0) {
          log_warn(LD_CONFIG, "Previously stored client_keys file could not "
                              "be parsed.");
          goto err;
        } else {
          log_info(LD_CONFIG, "Parsed %d previously stored client entries.",
                   strmap_size(parsed_clients));
          tor_free(client_keys_str);
        }
      }

      /* Prepare client_keys and hostname files. */
      if (!(cfile = start_writing_to_stdio_file(cfname,
                                                OPEN_FLAGS_REPLACE | O_TEXT,
                                                0600, &open_cfile))) {
        log_warn(LD_CONFIG, "Could not open client_keys file %s",
                 escaped(cfname));
        goto err;
      }
      if (!(hfile = start_writing_to_stdio_file(fname,
                                                OPEN_FLAGS_REPLACE | O_TEXT,
                                                0600, &open_hfile))) {
        log_warn(LD_CONFIG, "Could not open hostname file %s", escaped(fname));
        goto err;
      }

      /* Either use loaded keys for configured clients or generate new
       * ones if a client is new. */
      SMARTLIST_FOREACH_BEGIN(s->clients, rend_authorized_client_t *, client)
      {
        char desc_cook_out[3*REND_DESC_COOKIE_LEN_BASE64+1];
        char service_id[16+1];
        rend_authorized_client_t *parsed =
            strmap_get(parsed_clients, client->client_name);
        int written;
        size_t len;
        /* Copy descriptor cookie from parsed entry or create new one. */
        if (parsed) {
          memcpy(client->descriptor_cookie, parsed->descriptor_cookie,
                 REND_DESC_COOKIE_LEN);
        } else {
          crypto_rand(client->descriptor_cookie, REND_DESC_COOKIE_LEN);
        }
        if (base64_encode(desc_cook_out, 3*REND_DESC_COOKIE_LEN_BASE64+1,
                          client->descriptor_cookie,
                          REND_DESC_COOKIE_LEN) < 0) {
          log_warn(LD_BUG, "Could not base64-encode descriptor cookie.");
          strmap_free(parsed_clients, rend_authorized_client_strmap_item_free);
          return -1;
        }
        /* Copy client key from parsed entry or create new one if required. */
        if (parsed && parsed->client_key) {
          client->client_key = crypto_pk_dup_key(parsed->client_key);
        } else if (s->auth_type == REND_STEALTH_AUTH) {
          /* Create private key for client. */
          crypto_pk_t *prkey = NULL;
          if (!(prkey = crypto_pk_new())) {
            log_warn(LD_BUG,"Error constructing client key");
            goto err;
          }
          if (crypto_pk_generate_key(prkey)) {
            log_warn(LD_BUG,"Error generating client key");
            crypto_pk_free(prkey);
            goto err;
          }
          if (crypto_pk_check_key(prkey) <= 0) {
            log_warn(LD_BUG,"Generated client key seems invalid");
            crypto_pk_free(prkey);
            goto err;
          }
          client->client_key = prkey;
        }
        /* Add entry to client_keys file. */
        desc_cook_out[strlen(desc_cook_out)-1] = '\0'; /* Remove newline. */
        written = tor_snprintf(buf, sizeof(buf),
                               "client-name %s\ndescriptor-cookie %s\n",
                               client->client_name, desc_cook_out);
        if (written < 0) {
          log_warn(LD_BUG, "Could not write client entry.");
          goto err;
        }
        if (client->client_key) {
          char *client_key_out = NULL;
          crypto_pk_write_private_key_to_string(client->client_key,
                                                &client_key_out, &len);
          if (rend_get_service_id(client->client_key, service_id)<0) {
            log_warn(LD_BUG, "Internal error: couldn't encode service ID.");
            tor_free(client_key_out);
            goto err;
          }
          written = tor_snprintf(buf + written, sizeof(buf) - written,
                                 "client-key\n%s", client_key_out);
          tor_free(client_key_out);
          if (written < 0) {
            log_warn(LD_BUG, "Could not write client entry.");
            goto err;
          }
        }

        if (fputs(buf, cfile) < 0) {
          log_warn(LD_FS, "Could not append client entry to file: %s",
                   strerror(errno));
          goto err;
        }

        /* Add line to hostname file. */
        if (s->auth_type == REND_BASIC_AUTH) {
          /* Remove == signs (newline has been removed above). */
          desc_cook_out[strlen(desc_cook_out)-2] = '\0';
          tor_snprintf(buf, sizeof(buf),"%s.onion %s # client: %s\n",
                       s->service_id, desc_cook_out, client->client_name);
        } else {
          char extended_desc_cookie[REND_DESC_COOKIE_LEN+1];
          memcpy(extended_desc_cookie, client->descriptor_cookie,
                 REND_DESC_COOKIE_LEN);
          extended_desc_cookie[REND_DESC_COOKIE_LEN] =
              ((int)s->auth_type - 1) << 4;
          if (base64_encode(desc_cook_out, 3*REND_DESC_COOKIE_LEN_BASE64+1,
                            extended_desc_cookie,
                            REND_DESC_COOKIE_LEN+1) < 0) {
            log_warn(LD_BUG, "Could not base64-encode descriptor cookie.");
            goto err;
          }
          desc_cook_out[strlen(desc_cook_out)-3] = '\0'; /* Remove A= and
                                                            newline. */
          tor_snprintf(buf, sizeof(buf),"%s.onion %s # client: %s\n",
                       service_id, desc_cook_out, client->client_name);
        }

        if (fputs(buf, hfile)<0) {
          log_warn(LD_FS, "Could not append host entry to file: %s",
                   strerror(errno));
          goto err;
        }
      }
      SMARTLIST_FOREACH_END(client);

      goto done;
    err:
      r = -1;
    done:
      tor_free(client_keys_str);
      strmap_free(parsed_clients, rend_authorized_client_strmap_item_free);
      if (r<0) {
        if (open_cfile)
          abort_writing_to_file(open_cfile);
        if (open_hfile)
          abort_writing_to_file(open_hfile);
        return r;
      } else {
        finish_writing_to_file(open_cfile);
        finish_writing_to_file(open_hfile);
      }
    }
  } SMARTLIST_FOREACH_END(s);
  return r;
}

/** Return the service whose public key has a digest of <b>digest</b>, or
 * NULL if no such service exists.
 */
static rend_service_t *
rend_service_get_by_pk_digest(const char* digest)
{
  SMARTLIST_FOREACH(rend_service_list, rend_service_t*, s,
                    if (tor_memeq(s->pk_digest,digest,DIGEST_LEN))
                        return s);
  return NULL;
}

/** Return 1 if any virtual port in <b>service</b> wants a circuit
 * to have good uptime. Else return 0.
 */
static int
rend_service_requires_uptime(rend_service_t *service)
{
  int i;
  rend_service_port_config_t *p;

  for (i=0; i < smartlist_len(service->ports); ++i) {
    p = smartlist_get(service->ports, i);
    if (smartlist_string_num_isin(get_options()->LongLivedPorts,
                                  p->virtual_port))
      return 1;
  }
  return 0;
}

/** Check client authorization of a given <b>descriptor_cookie</b> for
 * <b>service</b>. Return 1 for success and 0 for failure. */
static int
rend_check_authorization(rend_service_t *service,
                         const char *descriptor_cookie)
{
  rend_authorized_client_t *auth_client = NULL;
  tor_assert(service);
  tor_assert(descriptor_cookie);
  if (!service->clients) {
    log_warn(LD_BUG, "Can't check authorization for a service that has no "
                     "authorized clients configured.");
    return 0;
  }

  /* Look up client authorization by descriptor cookie. */
  SMARTLIST_FOREACH(service->clients, rend_authorized_client_t *, client, {
    if (tor_memeq(client->descriptor_cookie, descriptor_cookie,
                REND_DESC_COOKIE_LEN)) {
      auth_client = client;
      break;
    }
  });
  if (!auth_client) {
    char descriptor_cookie_base64[3*REND_DESC_COOKIE_LEN_BASE64];
    base64_encode(descriptor_cookie_base64, sizeof(descriptor_cookie_base64),
                  descriptor_cookie, REND_DESC_COOKIE_LEN);
    log_info(LD_REND, "No authorization found for descriptor cookie '%s'! "
                      "Dropping cell!",
             descriptor_cookie_base64);
    return 0;
  }

  /* Allow the request. */
  log_debug(LD_REND, "Client %s authorized for service %s.",
            auth_client->client_name, service->service_id);
  return 1;
}

/** Remove elements from <b>service</b>'s replay cache that are old enough to
 * be noticed by timestamp checking. */
static void
clean_accepted_intro_dh_parts(rend_service_t *service, time_t now)
{
  const time_t cutoff = now - REND_REPLAY_TIME_INTERVAL;

  service->last_cleaned_accepted_intro_dh_parts = now;
  if (!service->accepted_intro_dh_parts)
    return;

  DIGESTMAP_FOREACH_MODIFY(service->accepted_intro_dh_parts, digest,
                           time_t *, t) {
    if (*t < cutoff) {
      tor_free(t);
      MAP_DEL_CURRENT(digest);
    }
  } DIGESTMAP_FOREACH_END;
}

/** Called when <b>intro</b> will soon be removed from
 * <b>service</b>'s list of intro points. */
static void
rend_service_note_removing_intro_point(rend_service_t *service,
                                       rend_intro_point_t *intro)
{
  time_t now = time(NULL);

  /* Don't process an intro point twice here. */
  if (intro->rend_service_note_removing_intro_point_called) {
    return;
  } else {
    intro->rend_service_note_removing_intro_point_called = 1;
  }

  /* Update service->n_intro_points_wanted based on how long intro
   * lasted and how many introductions it handled. */
  if (intro->time_published == -1) {
    /* This intro point was never used.  Don't change
     * n_intro_points_wanted. */
  } else {
    /* We want to increase the number of introduction points service
     * operates if intro was heavily used, or decrease the number of
     * intro points if intro was lightly used.
     *
     * We consider an intro point's target 'usage' to be
     * INTRO_POINT_LIFETIME_INTRODUCTIONS introductions in
     * INTRO_POINT_LIFETIME_MIN_SECONDS seconds.  To calculate intro's
     * fraction of target usage, we divide the fraction of
     * _LIFETIME_INTRODUCTIONS introductions that it has handled by
     * the fraction of _LIFETIME_MIN_SECONDS for which it existed.
     *
     * Then we multiply that fraction of desired usage by a fudge
     * factor of 1.5, to decide how many new introduction points
     * should ideally replace intro (which is now closed or soon to be
     * closed).  In theory, assuming that introduction load is
     * distributed equally across all intro points and ignoring the
     * fact that different intro points are established and closed at
     * different times, that number of intro points should bring all
     * of our intro points exactly to our target usage.
     *
     * Then we clamp that number to a number of intro points we might
     * be willing to replace this intro point with and turn it into an
     * integer. then we clamp it again to the number of new intro
     * points we could establish now, then we adjust
     * service->n_intro_points_wanted and let rend_services_introduce
     * create the new intro points we want (if any).
     */
    const double intro_point_usage =
      intro_point_accepted_intro_count(intro) /
      (double)(now - intro->time_published);
    const double intro_point_target_usage =
      INTRO_POINT_LIFETIME_INTRODUCTIONS /
      (double)INTRO_POINT_LIFETIME_MIN_SECONDS;
    const double fractional_n_intro_points_wanted_to_replace_this_one =
      (1.5 * (intro_point_usage / intro_point_target_usage));
    unsigned int n_intro_points_wanted_to_replace_this_one;
    unsigned int n_intro_points_wanted_now;
    unsigned int n_intro_points_really_wanted_now;
    int n_intro_points_really_replacing_this_one;

    if (fractional_n_intro_points_wanted_to_replace_this_one >
        NUM_INTRO_POINTS_MAX) {
      n_intro_points_wanted_to_replace_this_one = NUM_INTRO_POINTS_MAX;
    } else if (fractional_n_intro_points_wanted_to_replace_this_one < 0) {
      n_intro_points_wanted_to_replace_this_one = 0;
    } else {
      n_intro_points_wanted_to_replace_this_one = (unsigned)
        fractional_n_intro_points_wanted_to_replace_this_one;
    }

    n_intro_points_wanted_now =
      service->n_intro_points_wanted +
      n_intro_points_wanted_to_replace_this_one - 1;

    if (n_intro_points_wanted_now < NUM_INTRO_POINTS_DEFAULT) {
      /* XXXX This should be NUM_INTRO_POINTS_MIN instead.  Perhaps
       * another use of NUM_INTRO_POINTS_DEFAULT should be, too. */
      n_intro_points_really_wanted_now = NUM_INTRO_POINTS_DEFAULT;
    } else if (n_intro_points_wanted_now > NUM_INTRO_POINTS_MAX) {
      n_intro_points_really_wanted_now = NUM_INTRO_POINTS_MAX;
    } else {
      n_intro_points_really_wanted_now = n_intro_points_wanted_now;
    }

    n_intro_points_really_replacing_this_one =
      n_intro_points_really_wanted_now - service->n_intro_points_wanted + 1;

    log_info(LD_REND, "Replacing closing intro point for service %s "
             "with %d new intro points (wanted %g replacements); "
             "service will now try to have %u intro points",
             rend_service_describe_for_log(service),
             n_intro_points_really_replacing_this_one,
             fractional_n_intro_points_wanted_to_replace_this_one,
             n_intro_points_really_wanted_now);

    service->n_intro_points_wanted = n_intro_points_really_wanted_now;
  }
}

/******
 * Handle cells
 ******/

/** Respond to an INTRODUCE2 cell by launching a circuit to the chosen
 * rendezvous point.
 */
 /* XXXX024 this function sure could use some organizing. -RD */
int
rend_service_introduce(origin_circuit_t *circuit, const uint8_t *request,
                       size_t request_len)
{
  char *ptr, *r_cookie;
  extend_info_t *extend_info = NULL;
  char buf[RELAY_PAYLOAD_SIZE];
  char keys[DIGEST_LEN+CPATH_KEY_MATERIAL_LEN]; /* Holds KH, Df, Db, Kf, Kb */
  rend_service_t *service;
  rend_intro_point_t *intro_point;
  int r, i, v3_shift = 0;
  size_t len, keylen;
  crypto_dh_t *dh = NULL;
  origin_circuit_t *launched = NULL;
  crypt_path_t *cpath = NULL;
  char serviceid[REND_SERVICE_ID_LEN_BASE32+1];
  char hexcookie[9];
  int circ_needs_uptime;
  int reason = END_CIRC_REASON_TORPROTOCOL;
  crypto_pk_t *intro_key;
  char intro_key_digest[DIGEST_LEN];
  int auth_type;
  size_t auth_len = 0;
  char auth_data[REND_DESC_COOKIE_LEN];
  crypto_digest_t *digest = NULL;
  time_t now = time(NULL);
  char diffie_hellman_hash[DIGEST_LEN];
  time_t *access_time;
  const or_options_t *options = get_options();

  if (circuit->_base.purpose != CIRCUIT_PURPOSE_S_INTRO) {
    log_warn(LD_PROTOCOL,
             "Got an INTRODUCE2 over a non-introduction circuit %d.",
             circuit->_base.n_circ_id);
    return -1;
  }

#ifndef NON_ANONYMOUS_MODE_ENABLED
  tor_assert(!(circuit->build_state->onehop_tunnel));
#endif
  tor_assert(circuit->rend_data);

  base32_encode(serviceid, REND_SERVICE_ID_LEN_BASE32+1,
                circuit->rend_data->rend_pk_digest, REND_SERVICE_ID_LEN);
  log_info(LD_REND, "Received INTRODUCE2 cell for service %s on circ %d.",
           escaped(serviceid), circuit->_base.n_circ_id);

  /* min key length plus digest length plus nickname length */
  if (request_len < DIGEST_LEN+REND_COOKIE_LEN+(MAX_NICKNAME_LEN+1)+
      DH_KEY_LEN+42) {
    log_warn(LD_PROTOCOL, "Got a truncated INTRODUCE2 cell on circ %d.",
             circuit->_base.n_circ_id);
    return -1;
  }

  /* look up service depending on circuit. */
  service = rend_service_get_by_pk_digest(
                circuit->rend_data->rend_pk_digest);
  if (!service) {
    log_warn(LD_BUG, "Internal error: Got an INTRODUCE2 cell on an intro "
             "circ for an unrecognized service %s.",
             escaped(serviceid));
    return -1;
  }

  /* use intro key instead of service key. */
  intro_key = circuit->intro_key;

  /* first DIGEST_LEN bytes of request is intro or service pk digest */
  crypto_pk_get_digest(intro_key, intro_key_digest);
  if (tor_memneq(intro_key_digest, request, DIGEST_LEN)) {
    base32_encode(serviceid, REND_SERVICE_ID_LEN_BASE32+1,
                  (char*)request, REND_SERVICE_ID_LEN);
    log_warn(LD_REND, "Got an INTRODUCE2 cell for the wrong service (%s).",
             escaped(serviceid));
    return -1;
  }

  keylen = crypto_pk_keysize(intro_key);
  if (request_len < keylen+DIGEST_LEN) {
    log_warn(LD_PROTOCOL,
             "PK-encrypted portion of INTRODUCE2 cell was truncated.");
    return -1;
  }

  intro_point = find_intro_point(circuit);
  if (intro_point == NULL) {
    log_warn(LD_BUG, "Internal error: Got an INTRODUCE2 cell on an intro circ "
             "(for service %s) with no corresponding rend_intro_point_t.",
             escaped(serviceid));
    return -1;
  }

  if (!service->accepted_intro_dh_parts)
    service->accepted_intro_dh_parts = digestmap_new();

  if (!intro_point->accepted_intro_rsa_parts)
    intro_point->accepted_intro_rsa_parts = digestmap_new();

  {
    char pkpart_digest[DIGEST_LEN];
    /* Check for replay of PK-encrypted portion. */
    crypto_digest(pkpart_digest, (char*)request+DIGEST_LEN, keylen);
    access_time = digestmap_get(intro_point->accepted_intro_rsa_parts,
                                pkpart_digest);
    if (access_time != NULL) {
      log_warn(LD_REND, "Possible replay detected! We received an "
               "INTRODUCE2 cell with same PK-encrypted part %d seconds ago. "
               "Dropping cell.", (int)(now-*access_time));
      return -1;
    }
    access_time = tor_malloc(sizeof(time_t));
    *access_time = now;
    digestmap_set(intro_point->accepted_intro_rsa_parts,
                  pkpart_digest, access_time);
  }

  /* Next N bytes is encrypted with service key */
  note_crypto_pk_op(REND_SERVER);
  r = crypto_pk_private_hybrid_decrypt(
       intro_key,buf,sizeof(buf),
       (char*)(request+DIGEST_LEN),request_len-DIGEST_LEN,
       PK_PKCS1_OAEP_PADDING,1);
  if (r<0) {
    log_warn(LD_PROTOCOL, "Couldn't decrypt INTRODUCE2 cell.");
    return -1;
  }
  len = r;
  if (*buf == 3) {
    /* Version 3 INTRODUCE2 cell. */
    v3_shift = 1;
    auth_type = buf[1];
    switch (auth_type) {
      case REND_BASIC_AUTH:
        /* fall through */
      case REND_STEALTH_AUTH:
        auth_len = ntohs(get_uint16(buf+2));
        if (auth_len != REND_DESC_COOKIE_LEN) {
          log_info(LD_REND, "Wrong auth data size %d, should be %d.",
                   (int)auth_len, REND_DESC_COOKIE_LEN);
          return -1;
        }
        memcpy(auth_data, buf+4, sizeof(auth_data));
        v3_shift += 2+REND_DESC_COOKIE_LEN;
        break;
      case REND_NO_AUTH:
        break;
      default:
        log_info(LD_REND, "Unknown authorization type '%d'", auth_type);
    }

    /* Skip the timestamp field.  We no longer use it. */
    v3_shift += 4;
  }
  if (*buf == 2 || *buf == 3) {
    /* Version 2 INTRODUCE2 cell. */
    int klen;
    extend_info = tor_malloc_zero(sizeof(extend_info_t));
    tor_addr_from_ipv4n(&extend_info->addr, get_uint32(buf+v3_shift+1));
    extend_info->port = ntohs(get_uint16(buf+v3_shift+5));
    memcpy(extend_info->identity_digest, buf+v3_shift+7,
           DIGEST_LEN);
    extend_info->nickname[0] = '$';
    base16_encode(extend_info->nickname+1, sizeof(extend_info->nickname)-1,
                  extend_info->identity_digest, DIGEST_LEN);

    klen = ntohs(get_uint16(buf+v3_shift+7+DIGEST_LEN));
    if ((int)len != v3_shift+7+DIGEST_LEN+2+klen+20+128) {
      log_warn(LD_PROTOCOL, "Bad length %u for version %d INTRODUCE2 cell.",
               (int)len, *buf);
      reason = END_CIRC_REASON_TORPROTOCOL;
      goto err;
    }
    extend_info->onion_key =
        crypto_pk_asn1_decode(buf+v3_shift+7+DIGEST_LEN+2, klen);
    if (!extend_info->onion_key) {
      log_warn(LD_PROTOCOL, "Error decoding onion key in version %d "
                            "INTRODUCE2 cell.", *buf);
      reason = END_CIRC_REASON_TORPROTOCOL;
      goto err;
    }
    ptr = buf+v3_shift+7+DIGEST_LEN+2+klen;
    len -= v3_shift+7+DIGEST_LEN+2+klen;
  } else {
    char *rp_nickname;
    size_t nickname_field_len;
    const node_t *node;
    int version;
    if (*buf == 1) {
      rp_nickname = buf+1;
      nickname_field_len = MAX_HEX_NICKNAME_LEN+1;
      version = 1;
    } else {
      nickname_field_len = MAX_NICKNAME_LEN+1;
      rp_nickname = buf;
      version = 0;
    }
    ptr=memchr(rp_nickname,0,nickname_field_len);
    if (!ptr || ptr == rp_nickname) {
      log_warn(LD_PROTOCOL,
               "Couldn't find a nul-padded nickname in INTRODUCE2 cell.");
      return -1;
    }
    if ((version == 0 && !is_legal_nickname(rp_nickname)) ||
        (version == 1 && !is_legal_nickname_or_hexdigest(rp_nickname))) {
      log_warn(LD_PROTOCOL, "Bad nickname in INTRODUCE2 cell.");
      return -1;
    }
    /* Okay, now we know that a nickname is at the start of the buffer. */
    ptr = rp_nickname+nickname_field_len;
    len -= nickname_field_len;
    len -= rp_nickname - buf; /* also remove header space used by version, if
                               * any */
    node = node_get_by_nickname(rp_nickname, 0);
    if (!node) {
      log_info(LD_REND, "Couldn't find router %s named in introduce2 cell.",
               escaped_safe_str_client(rp_nickname));
      /* XXXX Add a no-such-router reason? */
      reason = END_CIRC_REASON_TORPROTOCOL;
      goto err;
    }

    extend_info = extend_info_from_node(node, 0);
  }

  if (len != REND_COOKIE_LEN+DH_KEY_LEN) {
    log_warn(LD_PROTOCOL, "Bad length %u for INTRODUCE2 cell.", (int)len);
    reason = END_CIRC_REASON_TORPROTOCOL;
    goto err;
  }

  /* Check if we'd refuse to talk to this router */
  if (options->StrictNodes &&
      routerset_contains_extendinfo(options->ExcludeNodes, extend_info)) {
    log_warn(LD_REND, "Client asked to rendezvous at a relay that we "
             "exclude, and StrictNodes is set. Refusing service.");
    reason = END_CIRC_REASON_INTERNAL; /* XXX might leak why we refused */
    goto err;
  }

  r_cookie = ptr;
  base16_encode(hexcookie,9,r_cookie,4);

  /* Determine hash of Diffie-Hellman, part 1 to detect replays. */
  digest = crypto_digest_new();
  crypto_digest_add_bytes(digest, ptr+REND_COOKIE_LEN, DH_KEY_LEN);
  crypto_digest_get_digest(digest, diffie_hellman_hash, DIGEST_LEN);
  crypto_digest_free(digest);

  /* Check whether there is a past request with the same Diffie-Hellman,
   * part 1. */
  access_time = digestmap_get(service->accepted_intro_dh_parts,
                              diffie_hellman_hash);
  if (access_time != NULL) {
    /* A Tor client will send a new INTRODUCE1 cell with the same rend
     * cookie and DH public key as its previous one if its intro circ
     * times out while in state CIRCUIT_PURPOSE_C_INTRODUCE_ACK_WAIT .
     * If we received the first INTRODUCE1 cell (the intro-point relay
     * converts it into an INTRODUCE2 cell), we are already trying to
     * connect to that rend point (and may have already succeeded);
     * drop this cell. */
    log_info(LD_REND, "We received an "
             "INTRODUCE2 cell with same first part of "
             "Diffie-Hellman handshake %d seconds ago. Dropping "
             "cell.",
             (int) (now - *access_time));
    goto err;
  }

  /* Add request to access history, including time and hash of Diffie-Hellman,
   * part 1, and possibly remove requests from the history that are older than
   * one hour. */
  access_time = tor_malloc(sizeof(time_t));
  *access_time = now;
  digestmap_set(service->accepted_intro_dh_parts,
                diffie_hellman_hash, access_time);
  if (service->last_cleaned_accepted_intro_dh_parts + REND_REPLAY_TIME_INTERVAL
      < now)
    clean_accepted_intro_dh_parts(service, now);

  /* If the service performs client authorization, check included auth data. */
  if (service->clients) {
    if (auth_len > 0) {
      if (rend_check_authorization(service, auth_data)) {
        log_info(LD_REND, "Authorization data in INTRODUCE2 cell are valid.");
      } else {
        log_info(LD_REND, "The authorization data that are contained in "
                 "the INTRODUCE2 cell are invalid. Dropping cell.");
        reason = END_CIRC_REASON_CONNECTFAILED;
        goto err;
      }
    } else {
      log_info(LD_REND, "INTRODUCE2 cell does not contain authentication "
               "data, but we require client authorization. Dropping cell.");
      reason = END_CIRC_REASON_CONNECTFAILED;
      goto err;
    }
  }

  /* Try DH handshake... */
  dh = crypto_dh_new(DH_TYPE_REND);
  if (!dh || crypto_dh_generate_public(dh)<0) {
    log_warn(LD_BUG,"Internal error: couldn't build DH state "
             "or generate public key.");
    reason = END_CIRC_REASON_INTERNAL;
    goto err;
  }
  if (crypto_dh_compute_secret(LOG_PROTOCOL_WARN, dh, ptr+REND_COOKIE_LEN,
                               DH_KEY_LEN, keys,
                               DIGEST_LEN+CPATH_KEY_MATERIAL_LEN)<0) {
    log_warn(LD_BUG, "Internal error: couldn't complete DH handshake");
    reason = END_CIRC_REASON_INTERNAL;
    goto err;
  }

  circ_needs_uptime = rend_service_requires_uptime(service);

  /* help predict this next time */
  rep_hist_note_used_internal(now, circ_needs_uptime, 1);

  /* Launch a circuit to alice's chosen rendezvous point.
   */
  for (i=0;i<MAX_REND_FAILURES;i++) {
    int flags = CIRCLAUNCH_NEED_CAPACITY | CIRCLAUNCH_IS_INTERNAL;
    if (circ_needs_uptime) flags |= CIRCLAUNCH_NEED_UPTIME;
    launched = circuit_launch_by_extend_info(
                        CIRCUIT_PURPOSE_S_CONNECT_REND, extend_info, flags);

    if (launched)
      break;
  }
  if (!launched) { /* give up */
    log_warn(LD_REND, "Giving up launching first hop of circuit to rendezvous "
             "point %s for service %s.",
             safe_str_client(extend_info_describe(extend_info)),
             serviceid);
    reason = END_CIRC_REASON_CONNECTFAILED;
    goto err;
  }
  log_info(LD_REND,
           "Accepted intro; launching circuit to %s "
           "(cookie %s) for service %s.",
           safe_str_client(extend_info_describe(extend_info)),
           hexcookie, serviceid);
  tor_assert(launched->build_state);
  /* Fill in the circuit's state. */
  launched->rend_data = tor_malloc_zero(sizeof(rend_data_t));
  memcpy(launched->rend_data->rend_pk_digest,
         circuit->rend_data->rend_pk_digest,
         DIGEST_LEN);
  memcpy(launched->rend_data->rend_cookie, r_cookie, REND_COOKIE_LEN);
  strlcpy(launched->rend_data->onion_address, service->service_id,
          sizeof(launched->rend_data->onion_address));

  launched->build_state->service_pending_final_cpath_ref =
    tor_malloc_zero(sizeof(crypt_path_reference_t));
  launched->build_state->service_pending_final_cpath_ref->refcount = 1;

  launched->build_state->service_pending_final_cpath_ref->cpath = cpath =
    tor_malloc_zero(sizeof(crypt_path_t));
  cpath->magic = CRYPT_PATH_MAGIC;
  launched->build_state->expiry_time = now + MAX_REND_TIMEOUT;

  cpath->dh_handshake_state = dh;
  dh = NULL;
  if (circuit_init_cpath_crypto(cpath,keys+DIGEST_LEN,1)<0)
    goto err;
  memcpy(cpath->handshake_digest, keys, DIGEST_LEN);
  if (extend_info) extend_info_free(extend_info);

  memwipe(keys, 0, sizeof(keys));
  return 0;
 err:
  memwipe(keys, 0, sizeof(keys));
  if (dh) crypto_dh_free(dh);
  if (launched)
    circuit_mark_for_close(TO_CIRCUIT(launched), reason);
  if (extend_info) extend_info_free(extend_info);
  return -1;
}

/** Called when we fail building a rendezvous circuit at some point other
 * than the last hop: launches a new circuit to the same rendezvous point.
 */
void
rend_service_relaunch_rendezvous(origin_circuit_t *oldcirc)
{
  origin_circuit_t *newcirc;
  cpath_build_state_t *newstate, *oldstate;

  tor_assert(oldcirc->_base.purpose == CIRCUIT_PURPOSE_S_CONNECT_REND);

  /* Don't relaunch the same rend circ twice. */
  if (oldcirc->hs_service_side_rend_circ_has_been_relaunched) {
    log_info(LD_REND, "Rendezvous circuit to %s has already been relaunched; "
             "not relaunching it again.",
             oldcirc->build_state ?
             safe_str(extend_info_describe(oldcirc->build_state->chosen_exit))
             : "*unknown*");
    return;
  }
  oldcirc->hs_service_side_rend_circ_has_been_relaunched = 1;

  if (!oldcirc->build_state ||
      oldcirc->build_state->failure_count > MAX_REND_FAILURES ||
      oldcirc->build_state->expiry_time < time(NULL)) {
    log_info(LD_REND,
             "Attempt to build circuit to %s for rendezvous has failed "
             "too many times or expired; giving up.",
             oldcirc->build_state ?
             safe_str(extend_info_describe(oldcirc->build_state->chosen_exit))
             : "*unknown*");
    return;
  }

  oldstate = oldcirc->build_state;
  tor_assert(oldstate);

  if (oldstate->service_pending_final_cpath_ref == NULL) {
    log_info(LD_REND,"Skipping relaunch of circ that failed on its first hop. "
             "Initiator will retry.");
    return;
  }

  log_info(LD_REND,"Reattempting rendezvous circuit to '%s'",
           safe_str(extend_info_describe(oldstate->chosen_exit)));

  newcirc = circuit_launch_by_extend_info(CIRCUIT_PURPOSE_S_CONNECT_REND,
                            oldstate->chosen_exit,
                            CIRCLAUNCH_NEED_CAPACITY|CIRCLAUNCH_IS_INTERNAL);

  if (!newcirc) {
    log_warn(LD_REND,"Couldn't relaunch rendezvous circuit to '%s'.",
             safe_str(extend_info_describe(oldstate->chosen_exit)));
    return;
  }
  newstate = newcirc->build_state;
  tor_assert(newstate);
  newstate->failure_count = oldstate->failure_count+1;
  newstate->expiry_time = oldstate->expiry_time;
  newstate->service_pending_final_cpath_ref =
    oldstate->service_pending_final_cpath_ref;
  ++(newstate->service_pending_final_cpath_ref->refcount);

  newcirc->rend_data = rend_data_dup(oldcirc->rend_data);
}

/** Launch a circuit to serve as an introduction point for the service
 * <b>service</b> at the introduction point <b>nickname</b>
 */
static int
rend_service_launch_establish_intro(rend_service_t *service,
                                    rend_intro_point_t *intro)
{
  origin_circuit_t *launched;

  log_info(LD_REND,
           "Launching circuit to introduction point %s for service %s",
           safe_str_client(extend_info_describe(intro->extend_info)),
           service->service_id);

  rep_hist_note_used_internal(time(NULL), 1, 0);

  ++service->n_intro_circuits_launched;
  launched = circuit_launch_by_extend_info(CIRCUIT_PURPOSE_S_ESTABLISH_INTRO,
                             intro->extend_info,
                             CIRCLAUNCH_NEED_UPTIME|CIRCLAUNCH_IS_INTERNAL);

  if (!launched) {
    log_info(LD_REND,
             "Can't launch circuit to establish introduction at %s.",
             safe_str_client(extend_info_describe(intro->extend_info)));
    return -1;
  }

  if (tor_memneq(intro->extend_info->identity_digest,
      launched->build_state->chosen_exit->identity_digest, DIGEST_LEN)) {
    char cann[HEX_DIGEST_LEN+1], orig[HEX_DIGEST_LEN+1];
    base16_encode(cann, sizeof(cann),
                  launched->build_state->chosen_exit->identity_digest,
                  DIGEST_LEN);
    base16_encode(orig, sizeof(orig),
                  intro->extend_info->identity_digest, DIGEST_LEN);
    log_info(LD_REND, "The intro circuit we just cannibalized ends at $%s, "
                      "but we requested an intro circuit to $%s. Updating "
                      "our service.", cann, orig);
    extend_info_free(intro->extend_info);
    intro->extend_info = extend_info_dup(launched->build_state->chosen_exit);
  }

  launched->rend_data = tor_malloc_zero(sizeof(rend_data_t));
  strlcpy(launched->rend_data->onion_address, service->service_id,
          sizeof(launched->rend_data->onion_address));
  memcpy(launched->rend_data->rend_pk_digest, service->pk_digest, DIGEST_LEN);
  launched->intro_key = crypto_pk_dup_key(intro->intro_key);
  if (launched->_base.state == CIRCUIT_STATE_OPEN)
    rend_service_intro_has_opened(launched);
  return 0;
}

/** Return the number of introduction points that are or have been
 * established for the given service address in <b>query</b>. */
static int
count_established_intro_points(const char *query)
{
  int num_ipos = 0;
  circuit_t *circ;
  for (circ = _circuit_get_global_list(); circ; circ = circ->next) {
    if (!circ->marked_for_close &&
        circ->state == CIRCUIT_STATE_OPEN &&
        (circ->purpose == CIRCUIT_PURPOSE_S_ESTABLISH_INTRO ||
         circ->purpose == CIRCUIT_PURPOSE_S_INTRO)) {
      origin_circuit_t *oc = TO_ORIGIN_CIRCUIT(circ);
      if (oc->rend_data &&
          !rend_cmp_service_ids(query, oc->rend_data->onion_address))
        num_ipos++;
    }
  }
  return num_ipos;
}

/** Called when we're done building a circuit to an introduction point:
 *  sends a RELAY_ESTABLISH_INTRO cell.
 */
void
rend_service_intro_has_opened(origin_circuit_t *circuit)
{
  rend_service_t *service;
  size_t len;
  int r;
  char buf[RELAY_PAYLOAD_SIZE];
  char auth[DIGEST_LEN + 9];
  char serviceid[REND_SERVICE_ID_LEN_BASE32+1];
  int reason = END_CIRC_REASON_TORPROTOCOL;
  crypto_pk_t *intro_key;

  tor_assert(circuit->_base.purpose == CIRCUIT_PURPOSE_S_ESTABLISH_INTRO);
#ifndef NON_ANONYMOUS_MODE_ENABLED
  tor_assert(!(circuit->build_state->onehop_tunnel));
#endif
  tor_assert(circuit->cpath);
  tor_assert(circuit->rend_data);

  base32_encode(serviceid, REND_SERVICE_ID_LEN_BASE32+1,
                circuit->rend_data->rend_pk_digest, REND_SERVICE_ID_LEN);

  service = rend_service_get_by_pk_digest(
                circuit->rend_data->rend_pk_digest);
  if (!service) {
    log_warn(LD_REND, "Unrecognized service ID %s on introduction circuit %d.",
             serviceid, circuit->_base.n_circ_id);
    reason = END_CIRC_REASON_NOSUCHSERVICE;
    goto err;
  }

  /* If we already have enough introduction circuits for this service,
   * redefine this one as a general circuit or close it, depending. */
  if (count_established_intro_points(serviceid) >
      (int)service->n_intro_points_wanted) { /* XXX023 remove cast */
    const or_options_t *options = get_options();
    if (options->ExcludeNodes) {
      /* XXXX in some future version, we can test whether the transition is
         allowed or not given the actual nodes in the circuit.  But for now,
         this case, we might as well close the thing. */
      log_info(LD_CIRC|LD_REND, "We have just finished an introduction "
               "circuit, but we already have enough.  Closing it.");
      circuit_mark_for_close(TO_CIRCUIT(circuit), END_CIRC_REASON_NONE);
      return;
    } else {
      tor_assert(circuit->build_state->is_internal);
      log_info(LD_CIRC|LD_REND, "We have just finished an introduction "
               "circuit, but we already have enough. Redefining purpose to "
               "general; leaving as internal.");

      circuit_change_purpose(TO_CIRCUIT(circuit), CIRCUIT_PURPOSE_C_GENERAL);

      {
        rend_data_t *rend_data = circuit->rend_data;
        circuit->rend_data = NULL;
        rend_data_free(rend_data);
      }
      {
        crypto_pk_t *intro_key = circuit->intro_key;
        circuit->intro_key = NULL;
        crypto_pk_free(intro_key);
      }

      circuit_has_opened(circuit);
      return;
    }
  }

  log_info(LD_REND,
           "Established circuit %d as introduction point for service %s",
           circuit->_base.n_circ_id, serviceid);

  /* Use the intro key instead of the service key in ESTABLISH_INTRO. */
  intro_key = circuit->intro_key;
  /* Build the payload for a RELAY_ESTABLISH_INTRO cell. */
  r = crypto_pk_asn1_encode(intro_key, buf+2,
                            RELAY_PAYLOAD_SIZE-2);
  if (r < 0) {
    log_warn(LD_BUG, "Internal error; failed to establish intro point.");
    reason = END_CIRC_REASON_INTERNAL;
    goto err;
  }
  len = r;
  set_uint16(buf, htons((uint16_t)len));
  len += 2;
  memcpy(auth, circuit->cpath->prev->handshake_digest, DIGEST_LEN);
  memcpy(auth+DIGEST_LEN, "INTRODUCE", 9);
  if (crypto_digest(buf+len, auth, DIGEST_LEN+9))
    goto err;
  len += 20;
  note_crypto_pk_op(REND_SERVER);
  r = crypto_pk_private_sign_digest(intro_key, buf+len, sizeof(buf)-len,
                                    buf, len);
  if (r<0) {
    log_warn(LD_BUG, "Internal error: couldn't sign introduction request.");
    reason = END_CIRC_REASON_INTERNAL;
    goto err;
  }
  len += r;

  if (relay_send_command_from_edge(0, TO_CIRCUIT(circuit),
                                   RELAY_COMMAND_ESTABLISH_INTRO,
                                   buf, len, circuit->cpath->prev)<0) {
    log_info(LD_GENERAL,
             "Couldn't send introduction request for service %s on circuit %d",
             serviceid, circuit->_base.n_circ_id);
    reason = END_CIRC_REASON_INTERNAL;
    goto err;
  }

  return;
 err:
  circuit_mark_for_close(TO_CIRCUIT(circuit), reason);
}

/** Called when we get an INTRO_ESTABLISHED cell; mark the circuit as a
 * live introduction point, and note that the service descriptor is
 * now out-of-date. */
int
rend_service_intro_established(origin_circuit_t *circuit,
                               const uint8_t *request,
                               size_t request_len)
{
  rend_service_t *service;
  char serviceid[REND_SERVICE_ID_LEN_BASE32+1];
  (void) request;
  (void) request_len;

  if (circuit->_base.purpose != CIRCUIT_PURPOSE_S_ESTABLISH_INTRO) {
    log_warn(LD_PROTOCOL,
             "received INTRO_ESTABLISHED cell on non-intro circuit.");
    goto err;
  }
  tor_assert(circuit->rend_data);
  service = rend_service_get_by_pk_digest(
                circuit->rend_data->rend_pk_digest);
  if (!service) {
    log_warn(LD_REND, "Unknown service on introduction circuit %d.",
             circuit->_base.n_circ_id);
    goto err;
  }
  service->desc_is_dirty = time(NULL);
  circuit_change_purpose(TO_CIRCUIT(circuit), CIRCUIT_PURPOSE_S_INTRO);

  base32_encode(serviceid, REND_SERVICE_ID_LEN_BASE32 + 1,
                circuit->rend_data->rend_pk_digest, REND_SERVICE_ID_LEN);
  log_info(LD_REND,
           "Received INTRO_ESTABLISHED cell on circuit %d for service %s",
           circuit->_base.n_circ_id, serviceid);

  return 0;
 err:
  circuit_mark_for_close(TO_CIRCUIT(circuit), END_CIRC_REASON_TORPROTOCOL);
  return -1;
}

/** Called once a circuit to a rendezvous point is established: sends a
 *  RELAY_COMMAND_RENDEZVOUS1 cell.
 */
void
rend_service_rendezvous_has_opened(origin_circuit_t *circuit)
{
  rend_service_t *service;
  char buf[RELAY_PAYLOAD_SIZE];
  crypt_path_t *hop;
  char serviceid[REND_SERVICE_ID_LEN_BASE32+1];
  char hexcookie[9];
  int reason;

  tor_assert(circuit->_base.purpose == CIRCUIT_PURPOSE_S_CONNECT_REND);
  tor_assert(circuit->cpath);
  tor_assert(circuit->build_state);
#ifndef NON_ANONYMOUS_MODE_ENABLED
  tor_assert(!(circuit->build_state->onehop_tunnel));
#endif
  tor_assert(circuit->rend_data);
  hop = circuit->build_state->service_pending_final_cpath_ref->cpath;

  base16_encode(hexcookie,9,circuit->rend_data->rend_cookie,4);
  base32_encode(serviceid, REND_SERVICE_ID_LEN_BASE32+1,
                circuit->rend_data->rend_pk_digest, REND_SERVICE_ID_LEN);

  log_info(LD_REND,
           "Done building circuit %d to rendezvous with "
           "cookie %s for service %s",
           circuit->_base.n_circ_id, hexcookie, serviceid);

  /* Clear the 'in-progress HS circ has timed out' flag for
   * consistency with what happens on the client side; this line has
   * no effect on Tor's behaviour. */
  circuit->hs_circ_has_timed_out = 0;

  /* If hop is NULL, another rend circ has already connected to this
   * rend point.  Close this circ. */
  if (hop == NULL) {
    log_info(LD_REND, "Another rend circ has already reached this rend point; "
             "closing this rend circ.");
    reason = END_CIRC_REASON_NONE;
    goto err;
  }

  /* Remove our final cpath element from the reference, so that no
   * other circuit will try to use it.  Store it in
   * pending_final_cpath for now to ensure that it will be freed if
   * our rendezvous attempt fails. */
  circuit->build_state->pending_final_cpath = hop;
  circuit->build_state->service_pending_final_cpath_ref->cpath = NULL;

  service = rend_service_get_by_pk_digest(
                circuit->rend_data->rend_pk_digest);
  if (!service) {
    log_warn(LD_GENERAL, "Internal error: unrecognized service ID on "
             "rendezvous circuit.");
    reason = END_CIRC_REASON_INTERNAL;
    goto err;
  }

  /* All we need to do is send a RELAY_RENDEZVOUS1 cell... */
  memcpy(buf, circuit->rend_data->rend_cookie, REND_COOKIE_LEN);
  if (crypto_dh_get_public(hop->dh_handshake_state,
                           buf+REND_COOKIE_LEN, DH_KEY_LEN)<0) {
    log_warn(LD_GENERAL,"Couldn't get DH public key.");
    reason = END_CIRC_REASON_INTERNAL;
    goto err;
  }
  memcpy(buf+REND_COOKIE_LEN+DH_KEY_LEN, hop->handshake_digest,
         DIGEST_LEN);

  /* Send the cell */
  if (relay_send_command_from_edge(0, TO_CIRCUIT(circuit),
                                   RELAY_COMMAND_RENDEZVOUS1,
                                   buf, REND_COOKIE_LEN+DH_KEY_LEN+DIGEST_LEN,
                                   circuit->cpath->prev)<0) {
    log_warn(LD_GENERAL, "Couldn't send RENDEZVOUS1 cell.");
    reason = END_CIRC_REASON_INTERNAL;
    goto err;
  }

  crypto_dh_free(hop->dh_handshake_state);
  hop->dh_handshake_state = NULL;

  /* Append the cpath entry. */
  hop->state = CPATH_STATE_OPEN;
  /* set the windows to default. these are the windows
   * that bob thinks alice has.
   */
  hop->package_window = circuit_initial_package_window();
  hop->deliver_window = CIRCWINDOW_START;

  onion_append_to_cpath(&circuit->cpath, hop);
  circuit->build_state->pending_final_cpath = NULL; /* prevent double-free */

  /* Change the circuit purpose. */
  circuit_change_purpose(TO_CIRCUIT(circuit), CIRCUIT_PURPOSE_S_REND_JOINED);

  return;
 err:
  circuit_mark_for_close(TO_CIRCUIT(circuit), reason);
}

/*
 * Manage introduction points
 */

/** Return the (possibly non-open) introduction circuit ending at
 * <b>intro</b> for the service whose public key is <b>pk_digest</b>.
 * (<b>desc_version</b> is ignored). Return NULL if no such service is
 * found.
 */
static origin_circuit_t *
find_intro_circuit(rend_intro_point_t *intro, const char *pk_digest)
{
  origin_circuit_t *circ = NULL;

  tor_assert(intro);
  while ((circ = circuit_get_next_by_pk_and_purpose(circ,pk_digest,
                                                  CIRCUIT_PURPOSE_S_INTRO))) {
    if (tor_memeq(circ->build_state->chosen_exit->identity_digest,
                intro->extend_info->identity_digest, DIGEST_LEN) &&
        circ->rend_data) {
      return circ;
    }
  }

  circ = NULL;
  while ((circ = circuit_get_next_by_pk_and_purpose(circ,pk_digest,
                                        CIRCUIT_PURPOSE_S_ESTABLISH_INTRO))) {
    if (tor_memeq(circ->build_state->chosen_exit->identity_digest,
                intro->extend_info->identity_digest, DIGEST_LEN) &&
        circ->rend_data) {
      return circ;
    }
  }
  return NULL;
}

/** Return a pointer to the rend_intro_point_t corresponding to the
 * service-side introduction circuit <b>circ</b>. */
static rend_intro_point_t *
find_intro_point(origin_circuit_t *circ)
{
  const char *serviceid;
  rend_service_t *service = NULL;

  tor_assert(TO_CIRCUIT(circ)->purpose == CIRCUIT_PURPOSE_S_ESTABLISH_INTRO ||
             TO_CIRCUIT(circ)->purpose == CIRCUIT_PURPOSE_S_INTRO);
  tor_assert(circ->rend_data);
  serviceid = circ->rend_data->onion_address;

  SMARTLIST_FOREACH(rend_service_list, rend_service_t *, s,
    if (tor_memeq(s->service_id, serviceid, REND_SERVICE_ID_LEN_BASE32)) {
      service = s;
      break;
    });

  if (service == NULL) return NULL;

  SMARTLIST_FOREACH(service->intro_nodes, rend_intro_point_t *, intro_point,
    if (crypto_pk_cmp_keys(intro_point->intro_key, circ->intro_key) == 0) {
      return intro_point;
    });

  return NULL;
}

/** Determine the responsible hidden service directories for the
 * rend_encoded_v2_service_descriptor_t's in <b>descs</b> and upload them;
 * <b>service_id</b> and <b>seconds_valid</b> are only passed for logging
 * purposes. */
static void
directory_post_to_hs_dir(rend_service_descriptor_t *renddesc,
                         smartlist_t *descs, const char *service_id,
                         int seconds_valid)
{
  int i, j, failed_upload = 0;
  smartlist_t *responsible_dirs = smartlist_new();
  smartlist_t *successful_uploads = smartlist_new();
  routerstatus_t *hs_dir;
  for (i = 0; i < smartlist_len(descs); i++) {
    rend_encoded_v2_service_descriptor_t *desc = smartlist_get(descs, i);
    /* Determine responsible dirs. */
    if (hid_serv_get_responsible_directories(responsible_dirs,
                                             desc->desc_id) < 0) {
      log_warn(LD_REND, "Could not determine the responsible hidden service "
                        "directories to post descriptors to.");
      smartlist_free(responsible_dirs);
      smartlist_free(successful_uploads);
      return;
    }
    for (j = 0; j < smartlist_len(responsible_dirs); j++) {
      char desc_id_base32[REND_DESC_ID_V2_LEN_BASE32 + 1];
      char *hs_dir_ip;
      const node_t *node;
      hs_dir = smartlist_get(responsible_dirs, j);
      if (smartlist_digest_isin(renddesc->successful_uploads,
                                hs_dir->identity_digest))
        /* Don't upload descriptor if we succeeded in doing so last time. */
        continue;
      node = node_get_by_id(hs_dir->identity_digest);
      if (!node || !node_has_descriptor(node)) {
        log_info(LD_REND, "Not launching upload for for v2 descriptor to "
                          "hidden service directory %s; we don't have its "
                          "router descriptor. Queuing for later upload.",
                 safe_str_client(routerstatus_describe(hs_dir)));
        failed_upload = -1;
        continue;
      }
      /* Send publish request. */
      directory_initiate_command_routerstatus(hs_dir,
                                              DIR_PURPOSE_UPLOAD_RENDDESC_V2,
                                              ROUTER_PURPOSE_GENERAL,
                                              1, NULL, desc->desc_str,
                                              strlen(desc->desc_str), 0);
      base32_encode(desc_id_base32, sizeof(desc_id_base32),
                    desc->desc_id, DIGEST_LEN);
      hs_dir_ip = tor_dup_ip(hs_dir->addr);
      log_info(LD_REND, "Launching upload for v2 descriptor for "
                        "service '%s' with descriptor ID '%s' with validity "
                        "of %d seconds to hidden service directory '%s' on "
                        "%s:%d.",
               safe_str_client(service_id),
               safe_str_client(desc_id_base32),
               seconds_valid,
               hs_dir->nickname,
               hs_dir_ip,
               hs_dir->or_port);
      tor_free(hs_dir_ip);
      /* Remember successful upload to this router for next time. */
      if (!smartlist_digest_isin(successful_uploads, hs_dir->identity_digest))
        smartlist_add(successful_uploads, hs_dir->identity_digest);
    }
    smartlist_clear(responsible_dirs);
  }
  if (!failed_upload) {
    if (renddesc->successful_uploads) {
      SMARTLIST_FOREACH(renddesc->successful_uploads, char *, c, tor_free(c););
      smartlist_free(renddesc->successful_uploads);
      renddesc->successful_uploads = NULL;
    }
    renddesc->all_uploads_performed = 1;
  } else {
    /* Remember which routers worked this time, so that we don't upload the
     * descriptor to them again. */
    if (!renddesc->successful_uploads)
      renddesc->successful_uploads = smartlist_new();
    SMARTLIST_FOREACH(successful_uploads, const char *, c, {
      if (!smartlist_digest_isin(renddesc->successful_uploads, c)) {
        char *hsdir_id = tor_memdup(c, DIGEST_LEN);
        smartlist_add(renddesc->successful_uploads, hsdir_id);
      }
    });
  }
  smartlist_free(responsible_dirs);
  smartlist_free(successful_uploads);
}

/** Encode and sign an up-to-date service descriptor for <b>service</b>,
 * and upload it/them to the responsible hidden service directories.
 */
static void
upload_service_descriptor(rend_service_t *service)
{
  time_t now = time(NULL);
  int rendpostperiod;
  char serviceid[REND_SERVICE_ID_LEN_BASE32+1];
  int uploaded = 0;

  rendpostperiod = get_options()->RendPostPeriod;

  /* Upload descriptor? */
  if (get_options()->PublishHidServDescriptors) {
    networkstatus_t *c = networkstatus_get_latest_consensus();
    if (c && smartlist_len(c->routerstatus_list) > 0) {
      int seconds_valid, i, j, num_descs;
      smartlist_t *descs = smartlist_new();
      smartlist_t *client_cookies = smartlist_new();
      /* Either upload a single descriptor (including replicas) or one
       * descriptor for each authorized client in case of authorization
       * type 'stealth'. */
      num_descs = service->auth_type == REND_STEALTH_AUTH ?
                      smartlist_len(service->clients) : 1;
      for (j = 0; j < num_descs; j++) {
        crypto_pk_t *client_key = NULL;
        rend_authorized_client_t *client = NULL;
        smartlist_clear(client_cookies);
        switch (service->auth_type) {
          case REND_NO_AUTH:
            /* Do nothing here. */
            break;
          case REND_BASIC_AUTH:
            SMARTLIST_FOREACH(service->clients, rend_authorized_client_t *,
                cl, smartlist_add(client_cookies, cl->descriptor_cookie));
            break;
          case REND_STEALTH_AUTH:
            client = smartlist_get(service->clients, j);
            client_key = client->client_key;
            smartlist_add(client_cookies, client->descriptor_cookie);
            break;
        }
        /* Encode the current descriptor. */
        seconds_valid = rend_encode_v2_descriptors(descs, service->desc,
                                                   now, 0,
                                                   service->auth_type,
                                                   client_key,
                                                   client_cookies);
        if (seconds_valid < 0) {
          log_warn(LD_BUG, "Internal error: couldn't encode service "
                   "descriptor; not uploading.");
          smartlist_free(descs);
          smartlist_free(client_cookies);
          return;
        }
        /* Post the current descriptors to the hidden service directories. */
        rend_get_service_id(service->desc->pk, serviceid);
        log_info(LD_REND, "Launching upload for hidden service %s",
                     serviceid);
        directory_post_to_hs_dir(service->desc, descs, serviceid,
                                 seconds_valid);
        /* Free memory for descriptors. */
        for (i = 0; i < smartlist_len(descs); i++)
          rend_encoded_v2_service_descriptor_free(smartlist_get(descs, i));
        smartlist_clear(descs);
        /* Update next upload time. */
        if (seconds_valid - REND_TIME_PERIOD_OVERLAPPING_V2_DESCS
            > rendpostperiod)
          service->next_upload_time = now + rendpostperiod;
        else if (seconds_valid < REND_TIME_PERIOD_OVERLAPPING_V2_DESCS)
          service->next_upload_time = now + seconds_valid + 1;
        else
          service->next_upload_time = now + seconds_valid -
              REND_TIME_PERIOD_OVERLAPPING_V2_DESCS + 1;
        /* Post also the next descriptors, if necessary. */
        if (seconds_valid < REND_TIME_PERIOD_OVERLAPPING_V2_DESCS) {
          seconds_valid = rend_encode_v2_descriptors(descs, service->desc,
                                                     now, 1,
                                                     service->auth_type,
                                                     client_key,
                                                     client_cookies);
          if (seconds_valid < 0) {
            log_warn(LD_BUG, "Internal error: couldn't encode service "
                     "descriptor; not uploading.");
            smartlist_free(descs);
            smartlist_free(client_cookies);
            return;
          }
          directory_post_to_hs_dir(service->desc, descs, serviceid,
                                   seconds_valid);
          /* Free memory for descriptors. */
          for (i = 0; i < smartlist_len(descs); i++)
            rend_encoded_v2_service_descriptor_free(smartlist_get(descs, i));
          smartlist_clear(descs);
        }
      }
      smartlist_free(descs);
      smartlist_free(client_cookies);
      uploaded = 1;
      log_info(LD_REND, "Successfully uploaded v2 rend descriptors!");
    }
  }

  /* If not uploaded, try again in one minute. */
  if (!uploaded)
    service->next_upload_time = now + 60;

  /* Unmark dirty flag of this service. */
  service->desc_is_dirty = 0;
}

/** Return the number of INTRODUCE2 cells this hidden service has received
 * from this intro point. */
static int
intro_point_accepted_intro_count(rend_intro_point_t *intro)
{
  if (intro->accepted_intro_rsa_parts == NULL) {
    return 0;
  } else {
    return digestmap_size(intro->accepted_intro_rsa_parts);
  }
}

/** Return non-zero iff <b>intro</b> should 'expire' now (i.e. we
 * should stop publishing it in new descriptors and eventually close
 * it). */
static int
intro_point_should_expire_now(rend_intro_point_t *intro,
                              time_t now)
{
  tor_assert(intro != NULL);

  if (intro->time_published == -1) {
    /* Don't expire an intro point if we haven't even published it yet. */
    return 0;
  }

  if (intro->time_expiring != -1) {
    /* We've already started expiring this intro point.  *Don't* let
     * this function's result 'flap'. */
    return 1;
  }

  if (intro_point_accepted_intro_count(intro) >=
      INTRO_POINT_LIFETIME_INTRODUCTIONS) {
    /* This intro point has been used too many times.  Expire it now. */
    return 1;
  }

  if (intro->time_to_expire == -1) {
    /* This intro point has been published, but we haven't picked an
     * expiration time for it.  Pick one now. */
    int intro_point_lifetime_seconds =
      INTRO_POINT_LIFETIME_MIN_SECONDS +
      crypto_rand_int(INTRO_POINT_LIFETIME_MAX_SECONDS -
                      INTRO_POINT_LIFETIME_MIN_SECONDS);

    /* Start the expiration timer now, rather than when the intro
     * point was first published.  There shouldn't be much of a time
     * difference. */
    intro->time_to_expire = now + intro_point_lifetime_seconds;

    return 0;
  }

  /* This intro point has a time to expire set already.  Use it. */
  return (now >= intro->time_to_expire);
}

/** For every service, check how many intro points it currently has, and:
 *  - Pick new intro points as necessary.
 *  - Launch circuits to any new intro points.
 */
void
rend_services_introduce(void)
{
  int i,j,r;
  const node_t *node;
  rend_service_t *service;
  rend_intro_point_t *intro;
  int intro_point_set_changed, prev_intro_nodes;
  unsigned int n_intro_points_unexpired;
  unsigned int n_intro_points_to_open;
  smartlist_t *intro_nodes;
  time_t now;
  const or_options_t *options = get_options();

  intro_nodes = smartlist_new();
  now = time(NULL);

  for (i=0; i < smartlist_len(rend_service_list); ++i) {
    smartlist_clear(intro_nodes);
    service = smartlist_get(rend_service_list, i);

    tor_assert(service);

    /* intro_point_set_changed becomes non-zero iff the set of intro
     * points to be published in service's descriptor has changed. */
    intro_point_set_changed = 0;

    /* n_intro_points_unexpired collects the number of non-expiring
     * intro points we have, so that we know how many new intro
     * circuits we need to launch for this service. */
    n_intro_points_unexpired = 0;

    if (now > service->intro_period_started+INTRO_CIRC_RETRY_PERIOD) {
      /* One period has elapsed; we can try building circuits again. */
      service->intro_period_started = now;
      service->n_intro_circuits_launched = 0;
    } else if (service->n_intro_circuits_launched >=
               MAX_INTRO_CIRCS_PER_PERIOD) {
      /* We have failed too many times in this period; wait for the next
       * one before we try again. */
      continue;
    }

    /* Find out which introduction points we have in progress for this
       service. */
    SMARTLIST_FOREACH_BEGIN(service->intro_nodes, rend_intro_point_t *,
                            intro) {
      origin_circuit_t *intro_circ =
        find_intro_circuit(intro, service->pk_digest);

      if (intro->time_expiring + INTRO_POINT_EXPIRATION_GRACE_PERIOD > now) {
        /* This intro point has completely expired.  Remove it, and
         * mark the circuit for close if it's still alive. */
        if (intro_circ != NULL) {
          circuit_mark_for_close(TO_CIRCUIT(intro_circ),
                                 END_CIRC_REASON_FINISHED);
        }
        rend_intro_point_free(intro);
        intro = NULL; /* SMARTLIST_DEL_CURRENT takes a name, not a value. */
        SMARTLIST_DEL_CURRENT(service->intro_nodes, intro);
        /* We don't need to set intro_point_set_changed here, because
         * this intro point wouldn't have been published in a current
         * descriptor anyway. */
        continue;
      }

      node = node_get_by_id(intro->extend_info->identity_digest);
      if (!node || !intro_circ) {
        int removing_this_intro_point_changes_the_intro_point_set = 1;
        log_info(LD_REND, "Giving up on %s as intro point for %s"
                 " (circuit disappeared).",
                 safe_str_client(extend_info_describe(intro->extend_info)),
                 safe_str_client(service->service_id));
        rend_service_note_removing_intro_point(service, intro);
        if (intro->time_expiring != -1) {
          log_info(LD_REND, "We were already expiring the intro point; "
                   "no need to mark the HS descriptor as dirty over this.");
          removing_this_intro_point_changes_the_intro_point_set = 0;
        } else if (intro->listed_in_last_desc) {
          log_info(LD_REND, "The intro point we are giving up on was "
                   "included in the last published descriptor. "
                   "Marking current descriptor as dirty.");
          service->desc_is_dirty = now;
        }
        rend_intro_point_free(intro);
        intro = NULL; /* SMARTLIST_DEL_CURRENT takes a name, not a value. */
        SMARTLIST_DEL_CURRENT(service->intro_nodes, intro);
        if (removing_this_intro_point_changes_the_intro_point_set)
          intro_point_set_changed = 1;
      }

      if (intro != NULL && intro_point_should_expire_now(intro, now)) {
        log_info(LD_REND, "Expiring %s as intro point for %s.",
                 safe_str_client(extend_info_describe(intro->extend_info)),
                 safe_str_client(service->service_id));

        rend_service_note_removing_intro_point(service, intro);

        /* The polite (and generally Right) way to expire an intro
         * point is to establish a new one to replace it, publish a
         * new descriptor that doesn't list any expiring intro points,
         * and *then*, once our upload attempts for the new descriptor
         * have ended (whether in success or failure), close the
         * expiring intro points.
         *
         * Unfortunately, we can't find out when the new descriptor
         * has actually been uploaded, so we'll have to settle for a
         * five-minute timer.  Start it.  XXXX024 This sucks. */
        intro->time_expiring = now;

        intro_point_set_changed = 1;
      }

      if (intro != NULL && intro->time_expiring == -1)
        ++n_intro_points_unexpired;

      if (node)
        smartlist_add(intro_nodes, (void*)node);
    } SMARTLIST_FOREACH_END(intro);

    if (!intro_point_set_changed &&
        (n_intro_points_unexpired >= service->n_intro_points_wanted)) {
      continue;
    }

    /* Remember how many introduction circuits we started with.
     *
     * prev_intro_nodes serves a different purpose than
     * n_intro_points_unexpired -- this variable tells us where our
     * previously-created intro points end and our new ones begin in
     * the intro-point list, so we don't have to launch the circuits
     * at the same time as we create the intro points they correspond
     * to.  XXXX This is daft. */
    prev_intro_nodes = smartlist_len(service->intro_nodes);

    /* We have enough directory information to start establishing our
     * intro points. We want to end up with n_intro_points_wanted
     * intro points, but if we're just starting, we launch two extra
     * circuits and use the first n_intro_points_wanted that complete.
     *
     * The ones after the first three will be converted to 'general'
     * internal circuits in rend_service_intro_has_opened(), and then
     * we'll drop them from the list of intro points next time we
     * go through the above "find out which introduction points we have
     * in progress" loop. */
    n_intro_points_to_open = (service->n_intro_points_wanted +
                              (prev_intro_nodes == 0 ? 2 : 0));
    for (j = (int)n_intro_points_unexpired;
         j < (int)n_intro_points_to_open;
         ++j) { /* XXXX remove casts */
      router_crn_flags_t flags = CRN_NEED_UPTIME|CRN_NEED_DESC;
      if (get_options()->_AllowInvalid & ALLOW_INVALID_INTRODUCTION)
        flags |= CRN_ALLOW_INVALID;
      node = router_choose_random_node(intro_nodes,
                                       options->ExcludeNodes, flags);
      if (!node) {
        log_warn(LD_REND,
                 "Could only establish %d introduction points for %s; "
                 "wanted %u.",
                 smartlist_len(service->intro_nodes), service->service_id,
                 n_intro_points_to_open);
        break;
      }
      intro_point_set_changed = 1;
      smartlist_add(intro_nodes, (void*)node);
      intro = tor_malloc_zero(sizeof(rend_intro_point_t));
      intro->extend_info = extend_info_from_node(node, 0);
      intro->intro_key = crypto_pk_new();
      tor_assert(!crypto_pk_generate_key(intro->intro_key));
      intro->time_published = -1;
      intro->time_to_expire = -1;
      intro->time_expiring = -1;
      smartlist_add(service->intro_nodes, intro);
      log_info(LD_REND, "Picked router %s as an intro point for %s.",
               safe_str_client(node_describe(node)),
               safe_str_client(service->service_id));
    }

    /* If there's no need to launch new circuits, stop here. */
    if (!intro_point_set_changed)
      continue;

    /* Establish new introduction points. */
    for (j=prev_intro_nodes; j < smartlist_len(service->intro_nodes); ++j) {
      intro = smartlist_get(service->intro_nodes, j);
      r = rend_service_launch_establish_intro(service, intro);
      if (r<0) {
        log_warn(LD_REND, "Error launching circuit to node %s for service %s.",
                 safe_str_client(extend_info_describe(intro->extend_info)),
                 safe_str_client(service->service_id));
      }
    }
  }
  smartlist_free(intro_nodes);
}

/** Regenerate and upload rendezvous service descriptors for all
 * services, if necessary. If the descriptor has been dirty enough
 * for long enough, definitely upload; else only upload when the
 * periodic timeout has expired.
 *
 * For the first upload, pick a random time between now and two periods
 * from now, and pick it independently for each service.
 */
void
rend_consider_services_upload(time_t now)
{
  int i;
  rend_service_t *service;
  int rendpostperiod = get_options()->RendPostPeriod;

  if (!get_options()->PublishHidServDescriptors)
    return;

  for (i=0; i < smartlist_len(rend_service_list); ++i) {
    service = smartlist_get(rend_service_list, i);
    if (!service->next_upload_time) { /* never been uploaded yet */
      /* The fixed lower bound of 30 seconds ensures that the descriptor
       * is stable before being published. See comment below. */
      service->next_upload_time =
        now + 30 + crypto_rand_int(2*rendpostperiod);
    }
    if (service->next_upload_time < now ||
        (service->desc_is_dirty &&
         service->desc_is_dirty < now-30)) {
      /* if it's time, or if the directory servers have a wrong service
       * descriptor and ours has been stable for 30 seconds, upload a
       * new one of each format. */
      rend_service_update_descriptor(service);
      upload_service_descriptor(service);
    }
  }
}

/** True if the list of available router descriptors might have changed so
 * that we should have a look whether we can republish previously failed
 * rendezvous service descriptors. */
static int consider_republishing_rend_descriptors = 1;

/** Called when our internal view of the directory has changed, so that we
 * might have router descriptors of hidden service directories available that
 * we did not have before. */
void
rend_hsdir_routers_changed(void)
{
  consider_republishing_rend_descriptors = 1;
}

/** Consider republication of v2 rendezvous service descriptors that failed
 * previously, but without regenerating descriptor contents.
 */
void
rend_consider_descriptor_republication(void)
{
  int i;
  rend_service_t *service;

  if (!consider_republishing_rend_descriptors)
    return;
  consider_republishing_rend_descriptors = 0;

  if (!get_options()->PublishHidServDescriptors)
    return;

  for (i=0; i < smartlist_len(rend_service_list); ++i) {
    service = smartlist_get(rend_service_list, i);
    if (service->desc && !service->desc->all_uploads_performed) {
      /* If we failed in uploading a descriptor last time, try again *without*
       * updating the descriptor's contents. */
      upload_service_descriptor(service);
    }
  }
}

/** Log the status of introduction points for all rendezvous services
 * at log severity <b>severity</b>.
 */
void
rend_service_dump_stats(int severity)
{
  int i,j;
  rend_service_t *service;
  rend_intro_point_t *intro;
  const char *safe_name;
  origin_circuit_t *circ;

  for (i=0; i < smartlist_len(rend_service_list); ++i) {
    service = smartlist_get(rend_service_list, i);
    log(severity, LD_GENERAL, "Service configured in \"%s\":",
        service->directory);
    for (j=0; j < smartlist_len(service->intro_nodes); ++j) {
      intro = smartlist_get(service->intro_nodes, j);
      safe_name = safe_str_client(intro->extend_info->nickname);

      circ = find_intro_circuit(intro, service->pk_digest);
      if (!circ) {
        log(severity, LD_GENERAL, "  Intro point %d at %s: no circuit",
            j, safe_name);
        continue;
      }
      log(severity, LD_GENERAL, "  Intro point %d at %s: circuit is %s",
          j, safe_name, circuit_state_to_string(circ->_base.state));
    }
  }
}

/** Given <b>conn</b>, a rendezvous exit stream, look up the hidden service for
 * 'circ', and look up the port and address based on conn-\>port.
 * Assign the actual conn-\>addr and conn-\>port. Return -1 if failure,
 * or 0 for success.
 */
int
rend_service_set_connection_addr_port(edge_connection_t *conn,
                                      origin_circuit_t *circ)
{
  rend_service_t *service;
  char serviceid[REND_SERVICE_ID_LEN_BASE32+1];
  smartlist_t *matching_ports;
  rend_service_port_config_t *chosen_port;

  tor_assert(circ->_base.purpose == CIRCUIT_PURPOSE_S_REND_JOINED);
  tor_assert(circ->rend_data);
  log_debug(LD_REND,"beginning to hunt for addr/port");
  base32_encode(serviceid, REND_SERVICE_ID_LEN_BASE32+1,
                circ->rend_data->rend_pk_digest, REND_SERVICE_ID_LEN);
  service = rend_service_get_by_pk_digest(
                circ->rend_data->rend_pk_digest);
  if (!service) {
    log_warn(LD_REND, "Couldn't find any service associated with pk %s on "
             "rendezvous circuit %d; closing.",
             serviceid, circ->_base.n_circ_id);
    return -1;
  }
  matching_ports = smartlist_new();
  SMARTLIST_FOREACH(service->ports, rend_service_port_config_t *, p,
  {
    if (conn->_base.port == p->virtual_port) {
      smartlist_add(matching_ports, p);
    }
  });
  chosen_port = smartlist_choose(matching_ports);
  smartlist_free(matching_ports);
  if (chosen_port) {
    tor_addr_copy(&conn->_base.addr, &chosen_port->real_addr);
    conn->_base.port = chosen_port->real_port;
    return 0;
  }
  log_info(LD_REND, "No virtual port mapping exists for port %d on service %s",
           conn->_base.port,serviceid);
  return -1;
}

