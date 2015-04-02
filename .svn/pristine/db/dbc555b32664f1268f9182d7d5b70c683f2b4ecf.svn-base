/* Copyright (c) 2013 Waldo Alvarez Cañizares, http://onionroute.org */
/* See LICENSE for licensing information */

#ifndef _TORLIB_INTERNAL_H_
#define _TORLIB_INTERNAL_H_

/* XXX: remove me once the library becomes reentrant */
typedef void (*onionroute_command_processor_t)(void* data);

extern smartlist_t *cqueue;

typedef struct onionroute_command_t
{

  onionroute_command_processor_t processor;
  void *data;

} onionroute_command_t;

#endif