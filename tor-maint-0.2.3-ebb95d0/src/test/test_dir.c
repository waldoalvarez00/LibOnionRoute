/* Copyright (c) 2001-2004, Roger Dingledine.
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2012, The Tor Project, Inc. */
/* See LICENSE for licensing information */

#include "orconfig.h"
#define DIRSERV_PRIVATE
#define DIRVOTE_PRIVATE
#define ROUTER_PRIVATE
#define HIBERNATE_PRIVATE
#include "or.h"
#include "directory.h"
#include "dirserv.h"
#include "dirvote.h"
#include "hibernate.h"
#include "networkstatus.h"
#include "router.h"
#include "routerlist.h"
#include "routerparse.h"
#include "test.h"

static void
test_dir_nicknames(void)
{
  test_assert( is_legal_nickname("a"));
  test_assert(!is_legal_nickname(""));
  test_assert(!is_legal_nickname("abcdefghijklmnopqrst")); /* 20 chars */
  test_assert(!is_legal_nickname("hyphen-")); /* bad char */
  test_assert( is_legal_nickname("abcdefghijklmnopqrs")); /* 19 chars */
  test_assert(!is_legal_nickname("$AAAAAAAA01234AAAAAAAAAAAAAAAAAAAAAAAAAAA"));
  /* valid */
  test_assert( is_legal_nickname_or_hexdigest(
                                 "$AAAAAAAA01234AAAAAAAAAAAAAAAAAAAAAAAAAAA"));
  test_assert( is_legal_nickname_or_hexdigest(
                         "$AAAAAAAA01234AAAAAAAAAAAAAAAAAAAAAAAAAAA=fred"));
  test_assert( is_legal_nickname_or_hexdigest(
                         "$AAAAAAAA01234AAAAAAAAAAAAAAAAAAAAAAAAAAA~fred"));
  /* too short */
  test_assert(!is_legal_nickname_or_hexdigest(
                                 "$AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"));
  /* illegal char */
  test_assert(!is_legal_nickname_or_hexdigest(
                                 "$AAAAAAzAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"));
  /* hex part too long */
  test_assert(!is_legal_nickname_or_hexdigest(
                         "$AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"));
  test_assert(!is_legal_nickname_or_hexdigest(
                         "$AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=fred"));
  /* Bad nickname */
  test_assert(!is_legal_nickname_or_hexdigest(
                         "$AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="));
  test_assert(!is_legal_nickname_or_hexdigest(
                         "$AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA~"));
  test_assert(!is_legal_nickname_or_hexdigest(
                       "$AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA~hyphen-"));
  test_assert(!is_legal_nickname_or_hexdigest(
                       "$AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA~"
                       "abcdefghijklmnoppqrst"));
  /* Bad extra char. */
  test_assert(!is_legal_nickname_or_hexdigest(
                         "$AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA!"));
  test_assert(is_legal_nickname_or_hexdigest("xyzzy"));
  test_assert(is_legal_nickname_or_hexdigest("abcdefghijklmnopqrs"));
  test_assert(!is_legal_nickname_or_hexdigest("abcdefghijklmnopqrst"));
 done:
  ;
}

/** Run unit tests for router descriptor generation logic. */
static void
test_dir_formats(void)
{
  char buf[8192], buf2[8192];
  char platform[256];
  char fingerprint[FINGERPRINT_LEN+1];
  char *pk1_str = NULL, *pk2_str = NULL, *pk3_str = NULL, *cp;
  size_t pk1_str_len, pk2_str_len, pk3_str_len;
  routerinfo_t *r1=NULL, *r2=NULL;
  crypto_pk_t *pk1 = NULL, *pk2 = NULL, *pk3 = NULL;
  routerinfo_t *rp1 = NULL;
  addr_policy_t *ex1, *ex2;
  routerlist_t *dir1 = NULL, *dir2 = NULL;

  pk1 = pk_generate(0);
  pk2 = pk_generate(1);
  pk3 = pk_generate(2);

  test_assert(pk1 && pk2 && pk3);

  hibernate_set_state_for_testing_(HIBERNATE_STATE_LIVE);

  get_platform_str(platform, sizeof(platform));
  r1 = tor_malloc_zero(sizeof(routerinfo_t));
  r1->address = tor_strdup("18.244.0.1");
  r1->addr = 0xc0a80001u; /* 192.168.0.1 */
  r1->cache_info.published_on = 0;
  r1->or_port = 9000;
  r1->dir_port = 9003;
  tor_addr_parse(&r1->ipv6_addr, "1:2:3:4::");
  r1->ipv6_orport = 9999;
  r1->onion_pkey = crypto_pk_dup_key(pk1);
  r1->identity_pkey = crypto_pk_dup_key(pk2);
  r1->bandwidthrate = 1000;
  r1->bandwidthburst = 5000;
  r1->bandwidthcapacity = 10000;
  r1->exit_policy = NULL;
  r1->nickname = tor_strdup("Magri");
  r1->platform = tor_strdup(platform);

  ex1 = tor_malloc_zero(sizeof(addr_policy_t));
  ex2 = tor_malloc_zero(sizeof(addr_policy_t));
  ex1->policy_type = ADDR_POLICY_ACCEPT;
  tor_addr_from_ipv4h(&ex1->addr, 0);
  ex1->maskbits = 0;
  ex1->prt_min = ex1->prt_max = 80;
  ex2->policy_type = ADDR_POLICY_REJECT;
  tor_addr_from_ipv4h(&ex2->addr, 18<<24);
  ex2->maskbits = 8;
  ex2->prt_min = ex2->prt_max = 24;
  r2 = tor_malloc_zero(sizeof(routerinfo_t));
  r2->address = tor_strdup("1.1.1.1");
  r2->addr = 0x0a030201u; /* 10.3.2.1 */
  r2->platform = tor_strdup(platform);
  r2->cache_info.published_on = 5;
  r2->or_port = 9005;
  r2->dir_port = 0;
  r2->onion_pkey = crypto_pk_dup_key(pk2);
  r2->identity_pkey = crypto_pk_dup_key(pk1);
  r2->bandwidthrate = r2->bandwidthburst = r2->bandwidthcapacity = 3000;
  r2->exit_policy = smartlist_new();
  smartlist_add(r2->exit_policy, ex2);
  smartlist_add(r2->exit_policy, ex1);
  r2->nickname = tor_strdup("Fred");

  test_assert(!crypto_pk_write_public_key_to_string(pk1, &pk1_str,
                                                    &pk1_str_len));
  test_assert(!crypto_pk_write_public_key_to_string(pk2 , &pk2_str,
                                                    &pk2_str_len));
  test_assert(!crypto_pk_write_public_key_to_string(pk3 , &pk3_str,
                                                    &pk3_str_len));

  memset(buf, 0, 2048);
  test_assert(router_dump_router_to_string(buf, 2048, r1, pk2)>0);

  strlcpy(buf2, "router Magri 18.244.0.1 9000 0 9003\n"
          "or-address [1:2:3:4::]:9999\n"
          "platform Tor "VERSION" on ", sizeof(buf2));
  strlcat(buf2, get_uname(), sizeof(buf2));
  strlcat(buf2, "\n"
          "opt protocols Link 1 2 Circuit 1\n"
          "published 1970-01-01 00:00:00\n"
          "opt fingerprint ", sizeof(buf2));
  test_assert(!crypto_pk_get_fingerprint(pk2, fingerprint, 1));
  strlcat(buf2, fingerprint, sizeof(buf2));
  strlcat(buf2, "\nuptime 0\n"
  /* XXX the "0" above is hard-coded, but even if we made it reflect
   * uptime, that still wouldn't make it right, because the two
   * descriptors might be made on different seconds... hm. */
         "bandwidth 1000 5000 10000\n"
          "onion-key\n", sizeof(buf2));
  strlcat(buf2, pk1_str, sizeof(buf2));
  strlcat(buf2, "signing-key\n", sizeof(buf2));
  strlcat(buf2, pk2_str, sizeof(buf2));
  strlcat(buf2, "opt hidden-service-dir\n", sizeof(buf2));
  strlcat(buf2, "reject *:*\nrouter-signature\n", sizeof(buf2));
  buf[strlen(buf2)] = '\0'; /* Don't compare the sig; it's never the same
                             * twice */

  test_streq(buf, buf2);

  test_assert(router_dump_router_to_string(buf, 2048, r1, pk2)>0);
  cp = buf;
  rp1 = router_parse_entry_from_string((const char*)cp,NULL,1,0,NULL);
  test_assert(rp1);
  test_streq(rp1->address, r1->address);
  test_eq(rp1->or_port, r1->or_port);
  //test_eq(rp1->dir_port, r1->dir_port);
  test_eq(rp1->bandwidthrate, r1->bandwidthrate);
  test_eq(rp1->bandwidthburst, r1->bandwidthburst);
  test_eq(rp1->bandwidthcapacity, r1->bandwidthcapacity);
  test_assert(crypto_pk_cmp_keys(rp1->onion_pkey, pk1) == 0);
  test_assert(crypto_pk_cmp_keys(rp1->identity_pkey, pk2) == 0);
  //test_assert(rp1->exit_policy == NULL);

#if 0
  /* XXX Once we have exit policies, test this again. XXX */
  strlcpy(buf2, "router tor.tor.tor 9005 0 0 3000\n", sizeof(buf2));
  strlcat(buf2, pk2_str, sizeof(buf2));
  strlcat(buf2, "signing-key\n", sizeof(buf2));
  strlcat(buf2, pk1_str, sizeof(buf2));
  strlcat(buf2, "accept *:80\nreject 18.*:24\n\n", sizeof(buf2));
  test_assert(router_dump_router_to_string(buf, 2048, &r2, pk2)>0);
  test_streq(buf, buf2);

  cp = buf;
  rp2 = router_parse_entry_from_string(&cp,1);
  test_assert(rp2);
  test_streq(rp2->address, r2.address);
  test_eq(rp2->or_port, r2.or_port);
  test_eq(rp2->dir_port, r2.dir_port);
  test_eq(rp2->bandwidth, r2.bandwidth);
  test_assert(crypto_pk_cmp_keys(rp2->onion_pkey, pk2) == 0);
  test_assert(crypto_pk_cmp_keys(rp2->identity_pkey, pk1) == 0);
  test_eq(rp2->exit_policy->policy_type, EXIT_POLICY_ACCEPT);
  test_streq(rp2->exit_policy->string, "accept *:80");
  test_streq(rp2->exit_policy->address, "*");
  test_streq(rp2->exit_policy->port, "80");
  test_eq(rp2->exit_policy->next->policy_type, EXIT_POLICY_REJECT);
  test_streq(rp2->exit_policy->next->string, "reject 18.*:24");
  test_streq(rp2->exit_policy->next->address, "18.*");
  test_streq(rp2->exit_policy->next->port, "24");
  test_assert(rp2->exit_policy->next->next == NULL);

  /* Okay, now for the directories. */
  {
    fingerprint_list = smartlist_new();
    crypto_pk_get_fingerprint(pk2, buf, 1);
    add_fingerprint_to_dir("Magri", buf, fingerprint_list);
    crypto_pk_get_fingerprint(pk1, buf, 1);
    add_fingerprint_to_dir("Fred", buf, fingerprint_list);
  }

  {
  char d[DIGEST_LEN];
  const char *m;
  /* XXXX NM re-enable. */
  /* Make sure routers aren't too far in the past any more. */
  r1->cache_info.published_on = time(NULL);
  r2->cache_info.published_on = time(NULL)-3*60*60;
  test_assert(router_dump_router_to_string(buf, 2048, r1, pk2)>0);
  test_eq(dirserv_add_descriptor(buf,&m,""), ROUTER_ADDED_NOTIFY_GENERATOR);
  test_assert(router_dump_router_to_string(buf, 2048, r2, pk1)>0);
  test_eq(dirserv_add_descriptor(buf,&m,""), ROUTER_ADDED_NOTIFY_GENERATOR);
  get_options()->Nickname = tor_strdup("DirServer");
  test_assert(!dirserv_dump_directory_to_string(&cp,pk3, 0));
  crypto_pk_get_digest(pk3, d);
  test_assert(!router_parse_directory(cp));
  test_eq(2, smartlist_len(dir1->routers));
  tor_free(cp);
  }
#endif
  dirserv_free_fingerprint_list();

 done:
  if (r1)
    routerinfo_free(r1);
  if (r2)
    routerinfo_free(r2);

  tor_free(pk1_str);
  tor_free(pk2_str);
  tor_free(pk3_str);
  if (pk1) crypto_pk_free(pk1);
  if (pk2) crypto_pk_free(pk2);
  if (pk3) crypto_pk_free(pk3);
  if (rp1) routerinfo_free(rp1);
  tor_free(dir1); /* XXXX And more !*/
  tor_free(dir2); /* And more !*/
}

static void
test_dir_versions(void)
{
  tor_version_t ver1;

  /* Try out version parsing functionality */
  test_eq(0, tor_version_parse("0.3.4pre2-cvs", &ver1));
  test_eq(0, ver1.major);
  test_eq(3, ver1.minor);
  test_eq(4, ver1.micro);
  test_eq(VER_PRE, ver1.status);
  test_eq(2, ver1.patchlevel);
  test_eq(0, tor_version_parse("0.3.4rc1", &ver1));
  test_eq(0, ver1.major);
  test_eq(3, ver1.minor);
  test_eq(4, ver1.micro);
  test_eq(VER_RC, ver1.status);
  test_eq(1, ver1.patchlevel);
  test_eq(0, tor_version_parse("1.3.4", &ver1));
  test_eq(1, ver1.major);
  test_eq(3, ver1.minor);
  test_eq(4, ver1.micro);
  test_eq(VER_RELEASE, ver1.status);
  test_eq(0, ver1.patchlevel);
  test_eq(0, tor_version_parse("1.3.4.999", &ver1));
  test_eq(1, ver1.major);
  test_eq(3, ver1.minor);
  test_eq(4, ver1.micro);
  test_eq(VER_RELEASE, ver1.status);
  test_eq(999, ver1.patchlevel);
  test_eq(0, tor_version_parse("0.1.2.4-alpha", &ver1));
  test_eq(0, ver1.major);
  test_eq(1, ver1.minor);
  test_eq(2, ver1.micro);
  test_eq(4, ver1.patchlevel);
  test_eq(VER_RELEASE, ver1.status);
  test_streq("alpha", ver1.status_tag);
  test_eq(0, tor_version_parse("0.1.2.4", &ver1));
  test_eq(0, ver1.major);
  test_eq(1, ver1.minor);
  test_eq(2, ver1.micro);
  test_eq(4, ver1.patchlevel);
  test_eq(VER_RELEASE, ver1.status);
  test_streq("", ver1.status_tag);

#define tt_versionstatus_op(vs1, op, vs2)                               \
  tt_assert_test_type(vs1,vs2,#vs1" "#op" "#vs2,version_status_t,       \
                      (val1_ op val2_),"%d",TT_EXIT_TEST_FUNCTION)
#define test_v_i_o(val, ver, lst)                                       \
  tt_versionstatus_op(val, ==, tor_version_is_obsolete(ver, lst))

  /* make sure tor_version_is_obsolete() works */
  test_v_i_o(VS_OLD, "0.0.1", "Tor 0.0.2");
  test_v_i_o(VS_OLD, "0.0.1", "0.0.2, Tor 0.0.3");
  test_v_i_o(VS_OLD, "0.0.1", "0.0.2,Tor 0.0.3");
  test_v_i_o(VS_OLD, "0.0.1","0.0.3,BetterTor 0.0.1");
  test_v_i_o(VS_RECOMMENDED, "0.0.2", "Tor 0.0.2,Tor 0.0.3");
  test_v_i_o(VS_NEW_IN_SERIES, "0.0.2", "Tor 0.0.2pre1,Tor 0.0.3");
  test_v_i_o(VS_OLD, "0.0.2", "Tor 0.0.2.1,Tor 0.0.3");
  test_v_i_o(VS_NEW, "0.1.0", "Tor 0.0.2,Tor 0.0.3");
  test_v_i_o(VS_RECOMMENDED, "0.0.7rc2", "0.0.7,Tor 0.0.7rc2,Tor 0.0.8");
  test_v_i_o(VS_OLD, "0.0.5.0", "0.0.5.1-cvs");
  test_v_i_o(VS_NEW_IN_SERIES, "0.0.5.1-cvs", "0.0.5, 0.0.6");
  /* Not on list, but newer than any in same series. */
  test_v_i_o(VS_NEW_IN_SERIES, "0.1.0.3",
             "Tor 0.1.0.2,Tor 0.0.9.5,Tor 0.1.1.0");
  /* Series newer than any on list. */
  test_v_i_o(VS_NEW, "0.1.2.3", "Tor 0.1.0.2,Tor 0.0.9.5,Tor 0.1.1.0");
  /* Series older than any on list. */
  test_v_i_o(VS_OLD, "0.0.1.3", "Tor 0.1.0.2,Tor 0.0.9.5,Tor 0.1.1.0");
  /* Not on list, not newer than any on same series. */
  test_v_i_o(VS_UNRECOMMENDED, "0.1.0.1",
             "Tor 0.1.0.2,Tor 0.0.9.5,Tor 0.1.1.0");
  /* On list, not newer than any on same series. */
  test_v_i_o(VS_UNRECOMMENDED,
             "0.1.0.1", "Tor 0.1.0.2,Tor 0.0.9.5,Tor 0.1.1.0");
  test_eq(0, tor_version_as_new_as("Tor 0.0.5", "0.0.9pre1-cvs"));
  test_eq(1, tor_version_as_new_as(
          "Tor 0.0.8 on Darwin 64-121-192-100.c3-0."
          "sfpo-ubr1.sfrn-sfpo.ca.cable.rcn.com Power Macintosh",
          "0.0.8rc2"));
  test_eq(0, tor_version_as_new_as(
          "Tor 0.0.8 on Darwin 64-121-192-100.c3-0."
          "sfpo-ubr1.sfrn-sfpo.ca.cable.rcn.com Power Macintosh", "0.0.8.2"));

  /* Now try svn revisions. */
  test_eq(1, tor_version_as_new_as("Tor 0.2.1.0-dev (r100)",
                                   "Tor 0.2.1.0-dev (r99)"));
  test_eq(1, tor_version_as_new_as("Tor 0.2.1.0-dev (r100) on Banana Jr",
                                   "Tor 0.2.1.0-dev (r99) on Hal 9000"));
  test_eq(1, tor_version_as_new_as("Tor 0.2.1.0-dev (r100)",
                                   "Tor 0.2.1.0-dev on Colossus"));
  test_eq(0, tor_version_as_new_as("Tor 0.2.1.0-dev (r99)",
                                   "Tor 0.2.1.0-dev (r100)"));
  test_eq(0, tor_version_as_new_as("Tor 0.2.1.0-dev (r99) on MCP",
                                   "Tor 0.2.1.0-dev (r100) on AM"));
  test_eq(0, tor_version_as_new_as("Tor 0.2.1.0-dev",
                                   "Tor 0.2.1.0-dev (r99)"));
  test_eq(1, tor_version_as_new_as("Tor 0.2.1.1",
                                   "Tor 0.2.1.0-dev (r99)"));

  /* Now try git revisions */
  test_eq(0, tor_version_parse("0.5.6.7 (git-ff00ff)", &ver1));
  test_eq(0, ver1.major);
  test_eq(5, ver1.minor);
  test_eq(6, ver1.micro);
  test_eq(7, ver1.patchlevel);
  test_eq(3, ver1.git_tag_len);
  test_memeq(ver1.git_tag, "\xff\x00\xff", 3);
  test_eq(-1, tor_version_parse("0.5.6.7 (git-ff00xx)", &ver1));
  test_eq(-1, tor_version_parse("0.5.6.7 (git-ff00fff)", &ver1));
  test_eq(0, tor_version_parse("0.5.6.7 (git ff00fff)", &ver1));
 done:
  ;
}

/** Run unit tests for directory fp_pair functions. */
static void
test_dir_fp_pairs(void)
{
  smartlist_t *sl = smartlist_new();
  fp_pair_t *pair;

  dir_split_resource_into_fingerprint_pairs(
       /* Two pairs, out of order, with one duplicate. */
       "73656372657420646174612E0000000000FFFFFF-"
       "557365204145532d32353620696e73746561642e+"
       "73656372657420646174612E0000000000FFFFFF-"
       "557365204145532d32353620696e73746561642e+"
       "48657861646563696d616c2069736e277420736f-"
       "676f6f6420666f7220686964696e6720796f7572.z", sl);

  test_eq(smartlist_len(sl), 2);
  pair = smartlist_get(sl, 0);
  test_memeq(pair->first,  "Hexadecimal isn't so", DIGEST_LEN);
  test_memeq(pair->second, "good for hiding your", DIGEST_LEN);
  pair = smartlist_get(sl, 1);
  test_memeq(pair->first,  "secret data.\0\0\0\0\0\xff\xff\xff", DIGEST_LEN);
  test_memeq(pair->second, "Use AES-256 instead.", DIGEST_LEN);

 done:
  SMARTLIST_FOREACH(sl, fp_pair_t *, pair, tor_free(pair));
  smartlist_free(sl);
}

static void
test_dir_split_fps(void *testdata)
{
  smartlist_t *sl = smartlist_new();
  char *mem_op_hex_tmp = NULL;
  (void)testdata;

  /* Some example hex fingerprints and their base64 equivalents */
#define HEX1 "Fe0daff89127389bc67558691231234551193EEE"
#define HEX2 "Deadbeef99999991111119999911111111f00ba4"
#define HEX3 "b33ff00db33ff00db33ff00db33ff00db33ff00d"
#define HEX256_1 \
    "f3f3f3f3fbbbbf3f3f3f3fbbbf3f3f3f3fbbbbf3f3f3f3fbbbf3f3f3f3fbbbbf"
#define HEX256_2 \
    "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccCCc"
#define HEX256_3 \
    "0123456789ABCdef0123456789ABCdef0123456789ABCdef0123456789ABCdef"
#define B64_1 "/g2v+JEnOJvGdVhpEjEjRVEZPu4"
#define B64_2 "3q2+75mZmZERERmZmRERERHwC6Q"
#define B64_3 "sz/wDbM/8A2zP/ANsz/wDbM/8A0"
#define B64_256_1 "8/Pz8/u7vz8/Pz+7vz8/Pz+7u/Pz8/P7u/Pz8/P7u78"
#define B64_256_2 "zMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMw"
#define B64_256_3 "ASNFZ4mrze8BI0VniavN7wEjRWeJq83vASNFZ4mrze8"

  /* no flags set */
  dir_split_resource_into_fingerprints("A+C+B", sl, NULL, 0);
  tt_int_op(smartlist_len(sl), ==, 3);
  tt_str_op(smartlist_get(sl, 0), ==, "A");
  tt_str_op(smartlist_get(sl, 1), ==, "C");
  tt_str_op(smartlist_get(sl, 2), ==, "B");
  SMARTLIST_FOREACH(sl, char *, cp, tor_free(cp));
  smartlist_clear(sl);

  /* uniq strings. */
  dir_split_resource_into_fingerprints("A+C+B+A+B+B", sl, NULL, DSR_SORT_UNIQ);
  tt_int_op(smartlist_len(sl), ==, 3);
  tt_str_op(smartlist_get(sl, 0), ==, "A");
  tt_str_op(smartlist_get(sl, 1), ==, "B");
  tt_str_op(smartlist_get(sl, 2), ==, "C");
  SMARTLIST_FOREACH(sl, char *, cp, tor_free(cp));
  smartlist_clear(sl);

  /* Decode hex. */
  dir_split_resource_into_fingerprints(HEX1"+"HEX2, sl, NULL, DSR_HEX);
  tt_int_op(smartlist_len(sl), ==, 2);
  test_mem_op_hex(smartlist_get(sl, 0), ==, HEX1);
  test_mem_op_hex(smartlist_get(sl, 1), ==, HEX2);
  SMARTLIST_FOREACH(sl, char *, cp, tor_free(cp));
  smartlist_clear(sl);

  /* decode hex and drop weirdness. */
  dir_split_resource_into_fingerprints(HEX1"+bogus+"HEX2"+"HEX256_1,
                                       sl, NULL, DSR_HEX);
  tt_int_op(smartlist_len(sl), ==, 2);
  test_mem_op_hex(smartlist_get(sl, 0), ==, HEX1);
  test_mem_op_hex(smartlist_get(sl, 1), ==, HEX2);
  SMARTLIST_FOREACH(sl, char *, cp, tor_free(cp));
  smartlist_clear(sl);

  /* Decode long hex */
  dir_split_resource_into_fingerprints(HEX256_1"+"HEX256_2"+"HEX2"+"HEX256_3,
                                       sl, NULL, DSR_HEX|DSR_DIGEST256);
  tt_int_op(smartlist_len(sl), ==, 3);
  test_mem_op_hex(smartlist_get(sl, 0), ==, HEX256_1);
  test_mem_op_hex(smartlist_get(sl, 1), ==, HEX256_2);
  test_mem_op_hex(smartlist_get(sl, 2), ==, HEX256_3);
  SMARTLIST_FOREACH(sl, char *, cp, tor_free(cp));
  smartlist_clear(sl);

  /* Decode hex and sort. */
  dir_split_resource_into_fingerprints(HEX1"+"HEX2"+"HEX3"+"HEX2,
                                       sl, NULL, DSR_HEX|DSR_SORT_UNIQ);
  tt_int_op(smartlist_len(sl), ==, 3);
  test_mem_op_hex(smartlist_get(sl, 0), ==, HEX3);
  test_mem_op_hex(smartlist_get(sl, 1), ==, HEX2);
  test_mem_op_hex(smartlist_get(sl, 2), ==, HEX1);
  SMARTLIST_FOREACH(sl, char *, cp, tor_free(cp));
  smartlist_clear(sl);

  /* Decode long hex and sort */
  dir_split_resource_into_fingerprints(HEX256_1"+"HEX256_2"+"HEX256_3
                                       "+"HEX256_1,
                                       sl, NULL,
                                       DSR_HEX|DSR_DIGEST256|DSR_SORT_UNIQ);
  tt_int_op(smartlist_len(sl), ==, 3);
  test_mem_op_hex(smartlist_get(sl, 0), ==, HEX256_3);
  test_mem_op_hex(smartlist_get(sl, 1), ==, HEX256_2);
  test_mem_op_hex(smartlist_get(sl, 2), ==, HEX256_1);
  SMARTLIST_FOREACH(sl, char *, cp, tor_free(cp));
  smartlist_clear(sl);

  /* Decode base64 */
  dir_split_resource_into_fingerprints(B64_1"-"B64_2, sl, NULL, DSR_BASE64);
  tt_int_op(smartlist_len(sl), ==, 2);
  test_mem_op_hex(smartlist_get(sl, 0), ==, HEX1);
  test_mem_op_hex(smartlist_get(sl, 1), ==, HEX2);
  SMARTLIST_FOREACH(sl, char *, cp, tor_free(cp));
  smartlist_clear(sl);

  /* Decode long base64 */
  dir_split_resource_into_fingerprints(B64_256_1"-"B64_256_2,
                                       sl, NULL, DSR_BASE64|DSR_DIGEST256);
  tt_int_op(smartlist_len(sl), ==, 2);
  test_mem_op_hex(smartlist_get(sl, 0), ==, HEX256_1);
  test_mem_op_hex(smartlist_get(sl, 1), ==, HEX256_2);
  SMARTLIST_FOREACH(sl, char *, cp, tor_free(cp));
  smartlist_clear(sl);

  dir_split_resource_into_fingerprints(B64_256_1,
                                       sl, NULL, DSR_BASE64|DSR_DIGEST256);
  tt_int_op(smartlist_len(sl), ==, 1);
  test_mem_op_hex(smartlist_get(sl, 0), ==, HEX256_1);
  SMARTLIST_FOREACH(sl, char *, cp, tor_free(cp));
  smartlist_clear(sl);

 done:
  SMARTLIST_FOREACH(sl, char *, cp, tor_free(cp));
  smartlist_free(sl);
  tor_free(mem_op_hex_tmp);
}

static void
test_dir_measured_bw(void)
{
  measured_bw_line_t mbwl;
  int i;
  const char *lines_pass[] = {
    "node_id=$557365204145532d32353620696e73746561642e bw=1024\n",
    "node_id=$557365204145532d32353620696e73746561642e\t  bw=1024 \n",
    " node_id=$557365204145532d32353620696e73746561642e  bw=1024\n",
    "\tnoise\tnode_id=$557365204145532d32353620696e73746561642e  "
                "bw=1024 junk=007\n",
    "misc=junk node_id=$557365204145532d32353620696e73746561642e  "
                "bw=1024 junk=007\n",
    "end"
  };
  const char *lines_fail[] = {
    /* Test possible python stupidity on input */
    "node_id=None bw=1024\n",
    "node_id=$None bw=1024\n",
    "node_id=$557365204145532d32353620696e73746561642e bw=None\n",
    "node_id=$557365204145532d32353620696e73746561642e bw=1024.0\n",
    "node_id=$557365204145532d32353620696e73746561642e bw=.1024\n",
    "node_id=$557365204145532d32353620696e73746561642e bw=1.024\n",
    "node_id=$557365204145532d32353620696e73746561642e bw=1024 bw=0\n",
    "node_id=$557365204145532d32353620696e73746561642e bw=1024 bw=None\n",
    "node_id=$557365204145532d32353620696e73746561642e bw=-1024\n",
    /* Test incomplete writes due to race conditions, partial copies, etc */
    "node_i",
    "node_i\n",
    "node_id=",
    "node_id=\n",
    "node_id=$557365204145532d32353620696e73746561642e bw=",
    "node_id=$557365204145532d32353620696e73746561642e bw=1024",
    "node_id=$557365204145532d32353620696e73746561642e bw=\n",
    "node_id=$557365204145532d32353620696e7374",
    "node_id=$557365204145532d32353620696e7374\n",
    "",
    "\n",
    " \n ",
    " \n\n",
    /* Test assorted noise */
    " node_id= ",
    "node_id==$557365204145532d32353620696e73746561642e bw==1024\n",
    "node_id=$55736520414552d32353620696e73746561642e bw=1024\n",
    "node_id=557365204145532d32353620696e73746561642e bw=1024\n",
    "node_id= $557365204145532d32353620696e73746561642e bw=0.23\n",
    "end"
  };

  for (i = 0; strcmp(lines_fail[i], "end"); i++) {
    //fprintf(stderr, "Testing: %s\n", lines_fail[i]);
    test_assert(measured_bw_line_parse(&mbwl, lines_fail[i]) == -1);
  }

  for (i = 0; strcmp(lines_pass[i], "end"); i++) {
    //fprintf(stderr, "Testing: %s %d\n", lines_pass[i], TOR_ISSPACE('\n'));
    test_assert(measured_bw_line_parse(&mbwl, lines_pass[i]) == 0);
    test_assert(mbwl.bw == 1024);
    test_assert(strcmp(mbwl.node_hex,
                "557365204145532d32353620696e73746561642e") == 0);
  }

 done:
  return;
}

static void
test_dir_param_voting(void)
{
  networkstatus_t vote1, vote2, vote3, vote4;
  smartlist_t *votes = smartlist_new();
  char *res = NULL;

  /* dirvote_compute_params only looks at the net_params field of the votes,
     so that's all we need to set.
   */
  memset(&vote1, 0, sizeof(vote1));
  memset(&vote2, 0, sizeof(vote2));
  memset(&vote3, 0, sizeof(vote3));
  memset(&vote4, 0, sizeof(vote4));
  vote1.net_params = smartlist_new();
  vote2.net_params = smartlist_new();
  vote3.net_params = smartlist_new();
  vote4.net_params = smartlist_new();
  smartlist_split_string(vote1.net_params,
                         "ab=90 abcd=20 cw=50 x-yz=-99", NULL, 0, 0);
  smartlist_split_string(vote2.net_params,
                         "ab=27 cw=5 x-yz=88", NULL, 0, 0);
  smartlist_split_string(vote3.net_params,
                         "abcd=20 c=60 cw=500 x-yz=-9 zzzzz=101", NULL, 0, 0);
  smartlist_split_string(vote4.net_params,
                         "ab=900 abcd=200 c=1 cw=51 x-yz=100", NULL, 0, 0);
  test_eq(100, networkstatus_get_param(&vote4, "x-yz", 50, 0, 300));
  test_eq(222, networkstatus_get_param(&vote4, "foobar", 222, 0, 300));
  test_eq(80, networkstatus_get_param(&vote4, "ab", 12, 0, 80));
  test_eq(-8, networkstatus_get_param(&vote4, "ab", -12, -100, -8));
  test_eq(0, networkstatus_get_param(&vote4, "foobar", 0, -100, 8));

  smartlist_add(votes, &vote1);

  /* Do the first tests without adding all the other votes, for
   * networks without many dirauths. */

  res = dirvote_compute_params(votes, 11, 6);
  test_streq(res, "ab=90 abcd=20 cw=50 x-yz=-99");
  tor_free(res);

  res = dirvote_compute_params(votes, 12, 2);
  test_streq(res, "");
  tor_free(res);

  res = dirvote_compute_params(votes, 12, 1);
  test_streq(res, "ab=90 abcd=20 cw=50 x-yz=-99");
  tor_free(res);

  smartlist_add(votes, &vote2);

  res = dirvote_compute_params(votes, 11, 2);
  test_streq(res, "ab=27 abcd=20 cw=5 x-yz=-99");
  tor_free(res);

  res = dirvote_compute_params(votes, 12, 2);
  test_streq(res, "ab=27 cw=5 x-yz=-99");
  tor_free(res);

  res = dirvote_compute_params(votes, 12, 3);
  test_streq(res, "ab=27 cw=5 x-yz=-99");
  tor_free(res);

  res = dirvote_compute_params(votes, 12, 6);
  test_streq(res, "");
  tor_free(res);

  smartlist_add(votes, &vote3);

  res = dirvote_compute_params(votes, 11, 3);
  test_streq(res, "ab=27 abcd=20 c=60 cw=50 x-yz=-9 zzzzz=101");
  tor_free(res);

  res = dirvote_compute_params(votes, 12, 3);
  test_streq(res, "ab=27 abcd=20 cw=50 x-yz=-9");
  tor_free(res);

  res = dirvote_compute_params(votes, 12, 5);
  test_streq(res, "cw=50 x-yz=-9");
  tor_free(res);

  res = dirvote_compute_params(votes, 12, 9);
  test_streq(res, "cw=50 x-yz=-9");
  tor_free(res);

  smartlist_add(votes, &vote4);

  res = dirvote_compute_params(votes, 11, 4);
  test_streq(res, "ab=90 abcd=20 c=1 cw=50 x-yz=-9 zzzzz=101");
  tor_free(res);

  res = dirvote_compute_params(votes, 12, 4);
  test_streq(res, "ab=90 abcd=20 cw=50 x-yz=-9");
  tor_free(res);

  res = dirvote_compute_params(votes, 12, 5);
  test_streq(res, "ab=90 abcd=20 cw=50 x-yz=-9");
  tor_free(res);

  /* Test that the special-cased "at least three dirauths voted for
   * this param" logic works as expected. */
  res = dirvote_compute_params(votes, 12, 6);
  test_streq(res, "ab=90 abcd=20 cw=50 x-yz=-9");
  tor_free(res);

  res = dirvote_compute_params(votes, 12, 10);
  test_streq(res, "ab=90 abcd=20 cw=50 x-yz=-9");
  tor_free(res);

 done:
  tor_free(res);
  SMARTLIST_FOREACH(vote1.net_params, char *, cp, tor_free(cp));
  SMARTLIST_FOREACH(vote2.net_params, char *, cp, tor_free(cp));
  SMARTLIST_FOREACH(vote3.net_params, char *, cp, tor_free(cp));
  SMARTLIST_FOREACH(vote4.net_params, char *, cp, tor_free(cp));
  smartlist_free(vote1.net_params);
  smartlist_free(vote2.net_params);
  smartlist_free(vote3.net_params);
  smartlist_free(vote4.net_params);
  smartlist_free(votes);

  return;
}

extern const char AUTHORITY_CERT_1[];
extern const char AUTHORITY_SIGNKEY_1[];
extern const char AUTHORITY_CERT_2[];
extern const char AUTHORITY_SIGNKEY_2[];
extern const char AUTHORITY_CERT_3[];
extern const char AUTHORITY_SIGNKEY_3[];

/** Helper: Test that two networkstatus_voter_info_t do in fact represent the
 * same voting authority, and that they do in fact have all the same
 * information. */
static void
test_same_voter(networkstatus_voter_info_t *v1,
                networkstatus_voter_info_t *v2)
{
  test_streq(v1->nickname, v2->nickname);
  test_memeq(v1->identity_digest, v2->identity_digest, DIGEST_LEN);
  test_streq(v1->address, v2->address);
  test_eq(v1->addr, v2->addr);
  test_eq(v1->dir_port, v2->dir_port);
  test_eq(v1->or_port, v2->or_port);
  test_streq(v1->contact, v2->contact);
  test_memeq(v1->vote_digest, v2->vote_digest, DIGEST_LEN);
 done:
  ;
}

/** Helper: Make a new routerinfo containing the right information for a
 * given vote_routerstatus_t. */
static routerinfo_t *
generate_ri_from_rs(const vote_routerstatus_t *vrs)
{
  routerinfo_t *r;
  const routerstatus_t *rs = &vrs->status;
  static time_t published = 0;

  r = tor_malloc_zero(sizeof(routerinfo_t));
  memcpy(r->cache_info.identity_digest, rs->identity_digest, DIGEST_LEN);
  memcpy(r->cache_info.signed_descriptor_digest, rs->descriptor_digest,
         DIGEST_LEN);
  r->cache_info.do_not_cache = 1;
  r->cache_info.routerlist_index = -1;
  r->cache_info.signed_descriptor_body =
    tor_strdup("123456789012345678901234567890123");
  r->cache_info.signed_descriptor_len =
    strlen(r->cache_info.signed_descriptor_body);
  r->exit_policy = smartlist_new();
  r->cache_info.published_on = ++published + time(NULL);
  return r;
}

/** Helper: get a detached signatures document for one or two
 * consensuses. */
static char *
get_detached_sigs(networkstatus_t *ns, networkstatus_t *ns2)
{
  char *r;
  smartlist_t *sl;
  tor_assert(ns && ns->flavor == FLAV_NS);
  sl = smartlist_new();
  smartlist_add(sl,ns);
  if (ns2)
    smartlist_add(sl,ns2);
  r = networkstatus_get_detached_signatures(sl);
  smartlist_free(sl);
  return r;
}

/** Run unit tests for generating and parsing V3 consensus networkstatus
 * documents. */
static void
test_dir_v3_networkstatus(void)
{
  authority_cert_t *cert1=NULL, *cert2=NULL, *cert3=NULL;
  crypto_pk_t *sign_skey_1=NULL, *sign_skey_2=NULL, *sign_skey_3=NULL;
  crypto_pk_t *sign_skey_leg1=NULL;
  const char *msg=NULL;

  time_t now = time(NULL);
  networkstatus_voter_info_t *voter;
  document_signature_t *sig;
  networkstatus_t *vote=NULL, *v1=NULL, *v2=NULL, *v3=NULL, *con=NULL,
    *con_md=NULL;
  vote_routerstatus_t *vrs;
  routerstatus_t *rs;
  char *v1_text=NULL, *v2_text=NULL, *v3_text=NULL, *consensus_text=NULL, *cp;
  smartlist_t *votes = smartlist_new();

  /* For generating the two other consensuses. */
  char *detached_text1=NULL, *detached_text2=NULL;
  char *consensus_text2=NULL, *consensus_text3=NULL;
  char *consensus_text_md2=NULL, *consensus_text_md3=NULL;
  char *consensus_text_md=NULL;
  networkstatus_t *con2=NULL, *con_md2=NULL, *con3=NULL, *con_md3=NULL;
  ns_detached_signatures_t *dsig1=NULL, *dsig2=NULL;

  /* Parse certificates and keys. */
  cert1 = authority_cert_parse_from_string(AUTHORITY_CERT_1, NULL);
  test_assert(cert1);
  test_assert(cert1->is_cross_certified);
  cert2 = authority_cert_parse_from_string(AUTHORITY_CERT_2, NULL);
  test_assert(cert2);
  cert3 = authority_cert_parse_from_string(AUTHORITY_CERT_3, NULL);
  test_assert(cert3);
  sign_skey_1 = crypto_pk_new();
  sign_skey_2 = crypto_pk_new();
  sign_skey_3 = crypto_pk_new();
  sign_skey_leg1 = pk_generate(4);

  test_assert(!crypto_pk_read_private_key_from_string(sign_skey_1,
                                                   AUTHORITY_SIGNKEY_1, -1));
  test_assert(!crypto_pk_read_private_key_from_string(sign_skey_2,
                                                   AUTHORITY_SIGNKEY_2, -1));
  test_assert(!crypto_pk_read_private_key_from_string(sign_skey_3,
                                                   AUTHORITY_SIGNKEY_3, -1));

  test_assert(!crypto_pk_cmp_keys(sign_skey_1, cert1->signing_key));
  test_assert(!crypto_pk_cmp_keys(sign_skey_2, cert2->signing_key));

  /*
   * Set up a vote; generate it; try to parse it.
   */
  vote = tor_malloc_zero(sizeof(networkstatus_t));
  vote->type = NS_TYPE_VOTE;
  vote->published = now;
  vote->valid_after = now+1000;
  vote->fresh_until = now+2000;
  vote->valid_until = now+3000;
  vote->vote_seconds = 100;
  vote->dist_seconds = 200;
  vote->supported_methods = smartlist_new();
  smartlist_split_string(vote->supported_methods, "1 2 3", NULL, 0, -1);
  vote->client_versions = tor_strdup("0.1.2.14,0.1.2.15");
  vote->server_versions = tor_strdup("0.1.2.14,0.1.2.15,0.1.2.16");
  vote->known_flags = smartlist_new();
  smartlist_split_string(vote->known_flags,
                     "Authority Exit Fast Guard Running Stable V2Dir Valid",
                     0, SPLIT_SKIP_SPACE|SPLIT_IGNORE_BLANK, 0);
  vote->voters = smartlist_new();
  voter = tor_malloc_zero(sizeof(networkstatus_voter_info_t));
  voter->nickname = tor_strdup("Voter1");
  voter->address = tor_strdup("1.2.3.4");
  voter->addr = 0x01020304;
  voter->dir_port = 80;
  voter->or_port = 9000;
  voter->contact = tor_strdup("voter@example.com");
  crypto_pk_get_digest(cert1->identity_key, voter->identity_digest);
  smartlist_add(vote->voters, voter);
  vote->cert = authority_cert_dup(cert1);
  vote->net_params = smartlist_new();
  smartlist_split_string(vote->net_params, "circuitwindow=101 foo=990",
                         NULL, 0, 0);
  vote->routerstatus_list = smartlist_new();
  /* add the first routerstatus. */
  vrs = tor_malloc_zero(sizeof(vote_routerstatus_t));
  rs = &vrs->status;
  vrs->version = tor_strdup("0.1.2.14");
  rs->published_on = now-1500;
  strlcpy(rs->nickname, "router2", sizeof(rs->nickname));
  memset(rs->identity_digest, 3, DIGEST_LEN);
  memset(rs->descriptor_digest, 78, DIGEST_LEN);
  rs->addr = 0x99008801;
  rs->or_port = 443;
  rs->dir_port = 8000;
  /* all flags but running cleared */
  rs->is_flagged_running = 1;
  smartlist_add(vote->routerstatus_list, vrs);
  test_assert(router_add_to_routerlist(generate_ri_from_rs(vrs), &msg,0,0)>=0);

  /* add the second routerstatus. */
  vrs = tor_malloc_zero(sizeof(vote_routerstatus_t));
  rs = &vrs->status;
  vrs->version = tor_strdup("0.2.0.5");
  rs->published_on = now-1000;
  strlcpy(rs->nickname, "router1", sizeof(rs->nickname));
  memset(rs->identity_digest, 5, DIGEST_LEN);
  memset(rs->descriptor_digest, 77, DIGEST_LEN);
  rs->addr = 0x99009901;
  rs->or_port = 443;
  rs->dir_port = 0;
  rs->is_exit = rs->is_stable = rs->is_fast = rs->is_flagged_running =
    rs->is_valid = rs->is_v2_dir = rs->is_possible_guard = 1;
  smartlist_add(vote->routerstatus_list, vrs);
  test_assert(router_add_to_routerlist(generate_ri_from_rs(vrs), &msg,0,0)>=0);

  /* add the third routerstatus. */
  vrs = tor_malloc_zero(sizeof(vote_routerstatus_t));
  rs = &vrs->status;
  vrs->version = tor_strdup("0.1.0.3");
  rs->published_on = now-1000;
  strlcpy(rs->nickname, "router3", sizeof(rs->nickname));
  memset(rs->identity_digest, 33, DIGEST_LEN);
  memset(rs->descriptor_digest, 79, DIGEST_LEN);
  rs->addr = 0xAA009901;
  rs->or_port = 400;
  rs->dir_port = 9999;
  rs->is_authority = rs->is_exit = rs->is_stable = rs->is_fast =
    rs->is_flagged_running = rs->is_valid = rs->is_v2_dir =
    rs->is_possible_guard = 1;
  smartlist_add(vote->routerstatus_list, vrs);
  test_assert(router_add_to_routerlist(generate_ri_from_rs(vrs), &msg,0,0)>=0);

  /* add a fourth routerstatus that is not running. */
  vrs = tor_malloc_zero(sizeof(vote_routerstatus_t));
  rs = &vrs->status;
  vrs->version = tor_strdup("0.1.6.3");
  rs->published_on = now-1000;
  strlcpy(rs->nickname, "router4", sizeof(rs->nickname));
  memset(rs->identity_digest, 34, DIGEST_LEN);
  memset(rs->descriptor_digest, 47, DIGEST_LEN);
  rs->addr = 0xC0000203;
  rs->or_port = 500;
  rs->dir_port = 1999;
  /* Running flag (and others) cleared */
  smartlist_add(vote->routerstatus_list, vrs);
  test_assert(router_add_to_routerlist(generate_ri_from_rs(vrs), &msg,0,0)>=0);

  /* dump the vote and try to parse it. */
  v1_text = format_networkstatus_vote(sign_skey_1, vote);
  test_assert(v1_text);
  v1 = networkstatus_parse_vote_from_string(v1_text, NULL, NS_TYPE_VOTE);
  test_assert(v1);

  /* Make sure the parsed thing was right. */
  test_eq(v1->type, NS_TYPE_VOTE);
  test_eq(v1->published, vote->published);
  test_eq(v1->valid_after, vote->valid_after);
  test_eq(v1->fresh_until, vote->fresh_until);
  test_eq(v1->valid_until, vote->valid_until);
  test_eq(v1->vote_seconds, vote->vote_seconds);
  test_eq(v1->dist_seconds, vote->dist_seconds);
  test_streq(v1->client_versions, vote->client_versions);
  test_streq(v1->server_versions, vote->server_versions);
  test_assert(v1->voters && smartlist_len(v1->voters));
  voter = smartlist_get(v1->voters, 0);
  test_streq(voter->nickname, "Voter1");
  test_streq(voter->address, "1.2.3.4");
  test_eq(voter->addr, 0x01020304);
  test_eq(voter->dir_port, 80);
  test_eq(voter->or_port, 9000);
  test_streq(voter->contact, "voter@example.com");
  test_assert(v1->cert);
  test_assert(!crypto_pk_cmp_keys(sign_skey_1, v1->cert->signing_key));
  cp = smartlist_join_strings(v1->known_flags, ":", 0, NULL);
  test_streq(cp, "Authority:Exit:Fast:Guard:Running:Stable:V2Dir:Valid");
  tor_free(cp);
  test_eq(smartlist_len(v1->routerstatus_list), 4);
  /* Check the first routerstatus. */
  vrs = smartlist_get(v1->routerstatus_list, 0);
  rs = &vrs->status;
  test_streq(vrs->version, "0.1.2.14");
  test_eq(rs->published_on, now-1500);
  test_streq(rs->nickname, "router2");
  test_memeq(rs->identity_digest,
             "\x3\x3\x3\x3\x3\x3\x3\x3\x3\x3\x3\x3\x3\x3\x3\x3\x3\x3\x3\x3",
             DIGEST_LEN);
  test_memeq(rs->descriptor_digest, "NNNNNNNNNNNNNNNNNNNN", DIGEST_LEN);
  test_eq(rs->addr, 0x99008801);
  test_eq(rs->or_port, 443);
  test_eq(rs->dir_port, 8000);
  test_eq(vrs->flags, U64_LITERAL(16)); // no flags except "running"
  /* Check the second routerstatus. */
  vrs = smartlist_get(v1->routerstatus_list, 1);
  rs = &vrs->status;
  test_streq(vrs->version, "0.2.0.5");
  test_eq(rs->published_on, now-1000);
  test_streq(rs->nickname, "router1");
  test_memeq(rs->identity_digest,
             "\x5\x5\x5\x5\x5\x5\x5\x5\x5\x5\x5\x5\x5\x5\x5\x5\x5\x5\x5\x5",
             DIGEST_LEN);
  test_memeq(rs->descriptor_digest, "MMMMMMMMMMMMMMMMMMMM", DIGEST_LEN);
  test_eq(rs->addr, 0x99009901);
  test_eq(rs->or_port, 443);
  test_eq(rs->dir_port, 0);
  test_eq(vrs->flags, U64_LITERAL(254)); // all flags except "authority."

  {
    measured_bw_line_t mbw;
    memset(mbw.node_id, 33, sizeof(mbw.node_id));
    mbw.bw = 1024;
    test_assert(measured_bw_line_apply(&mbw,
                v1->routerstatus_list) == 1);
    vrs = smartlist_get(v1->routerstatus_list, 2);
    test_assert(vrs->status.has_measured_bw &&
                vrs->status.measured_bw == 1024);
  }

  /* Generate second vote. It disagrees on some of the times,
   * and doesn't list versions, and knows some crazy flags */
  vote->published = now+1;
  vote->fresh_until = now+3005;
  vote->dist_seconds = 300;
  authority_cert_free(vote->cert);
  vote->cert = authority_cert_dup(cert2);
  vote->net_params = smartlist_new();
  smartlist_split_string(vote->net_params, "bar=2000000000 circuitwindow=20",
                         NULL, 0, 0);
  tor_free(vote->client_versions);
  tor_free(vote->server_versions);
  voter = smartlist_get(vote->voters, 0);
  tor_free(voter->nickname);
  tor_free(voter->address);
  voter->nickname = tor_strdup("Voter2");
  voter->address = tor_strdup("2.3.4.5");
  voter->addr = 0x02030405;
  crypto_pk_get_digest(cert2->identity_key, voter->identity_digest);
  smartlist_add(vote->known_flags, tor_strdup("MadeOfCheese"));
  smartlist_add(vote->known_flags, tor_strdup("MadeOfTin"));
  smartlist_sort_strings(vote->known_flags);
  vrs = smartlist_get(vote->routerstatus_list, 2);
  smartlist_del_keeporder(vote->routerstatus_list, 2);
  tor_free(vrs->version);
  tor_free(vrs);
  vrs = smartlist_get(vote->routerstatus_list, 0);
  vrs->status.is_fast = 1;
  /* generate and parse. */
  v2_text = format_networkstatus_vote(sign_skey_2, vote);
  test_assert(v2_text);
  v2 = networkstatus_parse_vote_from_string(v2_text, NULL, NS_TYPE_VOTE);
  test_assert(v2);
  /* Check that flags come out right.*/
  cp = smartlist_join_strings(v2->known_flags, ":", 0, NULL);
  test_streq(cp, "Authority:Exit:Fast:Guard:MadeOfCheese:MadeOfTin:"
             "Running:Stable:V2Dir:Valid");
  tor_free(cp);
  vrs = smartlist_get(v2->routerstatus_list, 1);
  /* 1023 - authority(1) - madeofcheese(16) - madeoftin(32) */
  test_eq(vrs->flags, U64_LITERAL(974));

  /* Generate the third vote. */
  vote->published = now;
  vote->fresh_until = now+2003;
  vote->dist_seconds = 250;
  authority_cert_free(vote->cert);
  vote->cert = authority_cert_dup(cert3);
  vote->net_params = smartlist_new();
  smartlist_split_string(vote->net_params, "circuitwindow=80 foo=660",
                         NULL, 0, 0);
  smartlist_add(vote->supported_methods, tor_strdup("4"));
  vote->client_versions = tor_strdup("0.1.2.14,0.1.2.17");
  vote->server_versions = tor_strdup("0.1.2.10,0.1.2.15,0.1.2.16");
  voter = smartlist_get(vote->voters, 0);
  tor_free(voter->nickname);
  tor_free(voter->address);
  voter->nickname = tor_strdup("Voter3");
  voter->address = tor_strdup("3.4.5.6");
  voter->addr = 0x03040506;
  crypto_pk_get_digest(cert3->identity_key, voter->identity_digest);
  /* This one has a legacy id. */
  memset(voter->legacy_id_digest, (int)'A', DIGEST_LEN);
  vrs = smartlist_get(vote->routerstatus_list, 0);
  smartlist_del_keeporder(vote->routerstatus_list, 0);
  tor_free(vrs->version);
  tor_free(vrs);
  vrs = smartlist_get(vote->routerstatus_list, 0);
  memset(vrs->status.descriptor_digest, (int)'Z', DIGEST_LEN);
  test_assert(router_add_to_routerlist(generate_ri_from_rs(vrs), &msg,0,0)>=0);

  v3_text = format_networkstatus_vote(sign_skey_3, vote);
  test_assert(v3_text);

  v3 = networkstatus_parse_vote_from_string(v3_text, NULL, NS_TYPE_VOTE);
  test_assert(v3);

  /* Compute a consensus as voter 3. */
  smartlist_add(votes, v3);
  smartlist_add(votes, v1);
  smartlist_add(votes, v2);
  consensus_text = networkstatus_compute_consensus(votes, 3,
                                                   cert3->identity_key,
                                                   sign_skey_3,
                                                   "AAAAAAAAAAAAAAAAAAAA",
                                                   sign_skey_leg1,
                                                   FLAV_NS);
  test_assert(consensus_text);
  con = networkstatus_parse_vote_from_string(consensus_text, NULL,
                                             NS_TYPE_CONSENSUS);
  test_assert(con);
  //log_notice(LD_GENERAL, "<<%s>>\n<<%s>>\n<<%s>>\n",
  //           v1_text, v2_text, v3_text);
  consensus_text_md = networkstatus_compute_consensus(votes, 3,
                                                   cert3->identity_key,
                                                   sign_skey_3,
                                                   "AAAAAAAAAAAAAAAAAAAA",
                                                   sign_skey_leg1,
                                                   FLAV_MICRODESC);
  test_assert(consensus_text_md);
  con_md = networkstatus_parse_vote_from_string(consensus_text_md, NULL,
                                                NS_TYPE_CONSENSUS);
  test_assert(con_md);
  test_eq(con_md->flavor, FLAV_MICRODESC);

  /* Check consensus contents. */
  test_assert(con->type == NS_TYPE_CONSENSUS);
  test_eq(con->published, 0); /* this field only appears in votes. */
  test_eq(con->valid_after, now+1000);
  test_eq(con->fresh_until, now+2003); /* median */
  test_eq(con->valid_until, now+3000);
  test_eq(con->vote_seconds, 100);
  test_eq(con->dist_seconds, 250); /* median */
  test_streq(con->client_versions, "0.1.2.14");
  test_streq(con->server_versions, "0.1.2.15,0.1.2.16");
  cp = smartlist_join_strings(v2->known_flags, ":", 0, NULL);
  test_streq(cp, "Authority:Exit:Fast:Guard:MadeOfCheese:MadeOfTin:"
             "Running:Stable:V2Dir:Valid");
  tor_free(cp);
  cp = smartlist_join_strings(con->net_params, ":", 0, NULL);
  test_streq(cp, "circuitwindow=80:foo=660");
  tor_free(cp);

  test_eq(4, smartlist_len(con->voters)); /*3 voters, 1 legacy key.*/
  /* The voter id digests should be in this order. */
  test_assert(memcmp(cert2->cache_info.identity_digest,
                     cert1->cache_info.identity_digest,DIGEST_LEN)<0);
  test_assert(memcmp(cert1->cache_info.identity_digest,
                     cert3->cache_info.identity_digest,DIGEST_LEN)<0);
  test_same_voter(smartlist_get(con->voters, 1),
                  smartlist_get(v2->voters, 0));
  test_same_voter(smartlist_get(con->voters, 2),
                  smartlist_get(v1->voters, 0));
  test_same_voter(smartlist_get(con->voters, 3),
                  smartlist_get(v3->voters, 0));

  test_assert(!con->cert);
  test_eq(2, smartlist_len(con->routerstatus_list));
  /* There should be two listed routers: one with identity 3, one with
   * identity 5. */
  /* This one showed up in 2 digests. */
  rs = smartlist_get(con->routerstatus_list, 0);
  test_memeq(rs->identity_digest,
             "\x3\x3\x3\x3\x3\x3\x3\x3\x3\x3\x3\x3\x3\x3\x3\x3\x3\x3\x3\x3",
             DIGEST_LEN);
  test_memeq(rs->descriptor_digest, "NNNNNNNNNNNNNNNNNNNN", DIGEST_LEN);
  test_assert(!rs->is_authority);
  test_assert(!rs->is_exit);
  test_assert(!rs->is_fast);
  test_assert(!rs->is_possible_guard);
  test_assert(!rs->is_stable);
  /* (If it wasn't running it wouldn't be here) */
  test_assert(rs->is_flagged_running);
  test_assert(!rs->is_v2_dir);
  test_assert(!rs->is_valid);
  test_assert(!rs->is_named);
  /* XXXX check version */

  rs = smartlist_get(con->routerstatus_list, 1);
  /* This one showed up in 3 digests. Twice with ID 'M', once with 'Z'.  */
  test_memeq(rs->identity_digest,
             "\x5\x5\x5\x5\x5\x5\x5\x5\x5\x5\x5\x5\x5\x5\x5\x5\x5\x5\x5\x5",
             DIGEST_LEN);
  test_streq(rs->nickname, "router1");
  test_memeq(rs->descriptor_digest, "MMMMMMMMMMMMMMMMMMMM", DIGEST_LEN);
  test_eq(rs->published_on, now-1000);
  test_eq(rs->addr, 0x99009901);
  test_eq(rs->or_port, 443);
  test_eq(rs->dir_port, 0);
  test_assert(!rs->is_authority);
  test_assert(rs->is_exit);
  test_assert(rs->is_fast);
  test_assert(rs->is_possible_guard);
  test_assert(rs->is_stable);
  test_assert(rs->is_flagged_running);
  test_assert(rs->is_v2_dir);
  test_assert(rs->is_valid);
  test_assert(!rs->is_named);
  /* XXXX check version */

  /* Check signatures.  the first voter is a pseudo-entry with a legacy key.
   * The second one hasn't signed.  The fourth one has signed: validate it. */
  voter = smartlist_get(con->voters, 1);
  test_eq(smartlist_len(voter->sigs), 0);

  voter = smartlist_get(con->voters, 3);
  test_eq(smartlist_len(voter->sigs), 1);
  sig = smartlist_get(voter->sigs, 0);
  test_assert(sig->signature);
  test_assert(!sig->good_signature);
  test_assert(!sig->bad_signature);

  test_assert(!networkstatus_check_document_signature(con, sig, cert3));
  test_assert(sig->signature);
  test_assert(sig->good_signature);
  test_assert(!sig->bad_signature);

  {
    const char *msg=NULL;
    /* Compute the other two signed consensuses. */
    smartlist_shuffle(votes);
    consensus_text2 = networkstatus_compute_consensus(votes, 3,
                                                      cert2->identity_key,
                                                      sign_skey_2, NULL,NULL,
                                                      FLAV_NS);
    consensus_text_md2 = networkstatus_compute_consensus(votes, 3,
                                                      cert2->identity_key,
                                                      sign_skey_2, NULL,NULL,
                                                      FLAV_MICRODESC);
    smartlist_shuffle(votes);
    consensus_text3 = networkstatus_compute_consensus(votes, 3,
                                                      cert1->identity_key,
                                                      sign_skey_1, NULL,NULL,
                                                      FLAV_NS);
    consensus_text_md3 = networkstatus_compute_consensus(votes, 3,
                                                      cert1->identity_key,
                                                      sign_skey_1, NULL,NULL,
                                                      FLAV_MICRODESC);
    test_assert(consensus_text2);
    test_assert(consensus_text3);
    test_assert(consensus_text_md2);
    test_assert(consensus_text_md3);
    con2 = networkstatus_parse_vote_from_string(consensus_text2, NULL,
                                                NS_TYPE_CONSENSUS);
    con3 = networkstatus_parse_vote_from_string(consensus_text3, NULL,
                                                NS_TYPE_CONSENSUS);
    con_md2 = networkstatus_parse_vote_from_string(consensus_text_md2, NULL,
                                                NS_TYPE_CONSENSUS);
    con_md3 = networkstatus_parse_vote_from_string(consensus_text_md3, NULL,
                                                NS_TYPE_CONSENSUS);
    test_assert(con2);
    test_assert(con3);
    test_assert(con_md2);
    test_assert(con_md3);

    /* All three should have the same digest. */
    test_memeq(&con->digests, &con2->digests, sizeof(digests_t));
    test_memeq(&con->digests, &con3->digests, sizeof(digests_t));

    test_memeq(&con_md->digests, &con_md2->digests, sizeof(digests_t));
    test_memeq(&con_md->digests, &con_md3->digests, sizeof(digests_t));

    /* Extract a detached signature from con3. */
    detached_text1 = get_detached_sigs(con3, con_md3);
    tt_assert(detached_text1);
    /* Try to parse it. */
    dsig1 = networkstatus_parse_detached_signatures(detached_text1, NULL);
    tt_assert(dsig1);

    /* Are parsed values as expected? */
    test_eq(dsig1->valid_after, con3->valid_after);
    test_eq(dsig1->fresh_until, con3->fresh_until);
    test_eq(dsig1->valid_until, con3->valid_until);
    {
      digests_t *dsig_digests = strmap_get(dsig1->digests, "ns");
      test_assert(dsig_digests);
      test_memeq(dsig_digests->d[DIGEST_SHA1], con3->digests.d[DIGEST_SHA1],
                 DIGEST_LEN);
      dsig_digests = strmap_get(dsig1->digests, "microdesc");
      test_assert(dsig_digests);
      test_memeq(dsig_digests->d[DIGEST_SHA256],
                 con_md3->digests.d[DIGEST_SHA256],
                 DIGEST256_LEN);
    }
    {
      smartlist_t *dsig_signatures = strmap_get(dsig1->signatures, "ns");
      test_assert(dsig_signatures);
      test_eq(1, smartlist_len(dsig_signatures));
      sig = smartlist_get(dsig_signatures, 0);
      test_memeq(sig->identity_digest, cert1->cache_info.identity_digest,
                 DIGEST_LEN);
      test_eq(sig->alg, DIGEST_SHA1);

      dsig_signatures = strmap_get(dsig1->signatures, "microdesc");
      test_assert(dsig_signatures);
      test_eq(1, smartlist_len(dsig_signatures));
      sig = smartlist_get(dsig_signatures, 0);
      test_memeq(sig->identity_digest, cert1->cache_info.identity_digest,
                 DIGEST_LEN);
      test_eq(sig->alg, DIGEST_SHA256);
    }

    /* Try adding it to con2. */
    detached_text2 = get_detached_sigs(con2,con_md2);
    test_eq(1, networkstatus_add_detached_signatures(con2, dsig1, "test",
                                                     LOG_INFO, &msg));
    tor_free(detached_text2);
    test_eq(1, networkstatus_add_detached_signatures(con_md2, dsig1, "test",
                                                     LOG_INFO, &msg));
    tor_free(detached_text2);
    detached_text2 = get_detached_sigs(con2,con_md2);
    //printf("\n<%s>\n", detached_text2);
    dsig2 = networkstatus_parse_detached_signatures(detached_text2, NULL);
    test_assert(dsig2);
    /*
    printf("\n");
    SMARTLIST_FOREACH(dsig2->signatures, networkstatus_voter_info_t *, vi, {
        char hd[64];
        base16_encode(hd, sizeof(hd), vi->identity_digest, DIGEST_LEN);
        printf("%s\n", hd);
      });
    */
    test_eq(2,
            smartlist_len((smartlist_t*)strmap_get(dsig2->signatures, "ns")));
    test_eq(2,
            smartlist_len((smartlist_t*)strmap_get(dsig2->signatures,
                                                   "microdesc")));

    /* Try adding to con2 twice; verify that nothing changes. */
    test_eq(0, networkstatus_add_detached_signatures(con2, dsig1, "test",
                                                     LOG_INFO, &msg));

    /* Add to con. */
    test_eq(2, networkstatus_add_detached_signatures(con, dsig2, "test",
                                                     LOG_INFO, &msg));
    /* Check signatures */
    voter = smartlist_get(con->voters, 1);
    sig = smartlist_get(voter->sigs, 0);
    test_assert(sig);
    test_assert(!networkstatus_check_document_signature(con, sig, cert2));
    voter = smartlist_get(con->voters, 2);
    sig = smartlist_get(voter->sigs, 0);
    test_assert(sig);
    test_assert(!networkstatus_check_document_signature(con, sig, cert1));
  }

 done:
  smartlist_free(votes);
  tor_free(v1_text);
  tor_free(v2_text);
  tor_free(v3_text);
  tor_free(consensus_text);
  tor_free(consensus_text_md);

  if (vote)
    networkstatus_vote_free(vote);
  if (v1)
    networkstatus_vote_free(v1);
  if (v2)
    networkstatus_vote_free(v2);
  if (v3)
    networkstatus_vote_free(v3);
  if (con)
    networkstatus_vote_free(con);
  if (con_md)
    networkstatus_vote_free(con_md);
  if (sign_skey_1)
    crypto_pk_free(sign_skey_1);
  if (sign_skey_2)
    crypto_pk_free(sign_skey_2);
  if (sign_skey_3)
    crypto_pk_free(sign_skey_3);
  if (sign_skey_leg1)
    crypto_pk_free(sign_skey_leg1);
  if (cert1)
    authority_cert_free(cert1);
  if (cert2)
    authority_cert_free(cert2);
  if (cert3)
    authority_cert_free(cert3);

  tor_free(consensus_text2);
  tor_free(consensus_text3);
  tor_free(consensus_text_md2);
  tor_free(consensus_text_md3);
  tor_free(detached_text1);
  tor_free(detached_text2);
  if (con2)
    networkstatus_vote_free(con2);
  if (con3)
    networkstatus_vote_free(con3);
  if (con_md2)
    networkstatus_vote_free(con_md2);
  if (con_md3)
    networkstatus_vote_free(con_md3);
  if (dsig1)
    ns_detached_signatures_free(dsig1);
  if (dsig2)
    ns_detached_signatures_free(dsig2);
}

#define DIR_LEGACY(name)                                                   \
  { #name, legacy_test_helper, TT_FORK, &legacy_setup, test_dir_ ## name }

#define DIR(name)                               \
  { #name, test_dir_##name, 0, NULL, NULL }

struct testcase_t dir_tests[] = {
  DIR_LEGACY(nicknames),
  DIR_LEGACY(formats),
  DIR_LEGACY(versions),
  DIR_LEGACY(fp_pairs),
  DIR(split_fps),
  DIR_LEGACY(measured_bw),
  DIR_LEGACY(param_voting),
  DIR_LEGACY(v3_networkstatus),
  END_OF_TESTCASES
};

