/* Copyright (c) 2013 Waldo Alvarez Cañizares, http://onionroute.org */
/* See LICENSE for licensing information */

#include "stdafx.h"
#include <Windows.h>
#include <onionroute.h>
#include <malloc.h>

void *conn;

void close_callback(void *c)
{
	//conn = onionroute_stream_open_v1("mesra.kl.my.dal.net", 6667);
	//onionroute_stream_open_v1("onionroute.org", 80);
	//conn = onionroute_stream_open_v1("www.google.org", 80);
	//onionroute_stream_open_v1("www.altavista.com", 80);
	//onionroute_stream_open_v1("silkroadvb5piz3r.onion", 80);
	//onionroute_stream_open_v1("kpvz7ki2v5agwt35.onion", 80);
	//onionroute_stream_open_v1("4srv4q3apzqylwob.onion", 80);
	onionroute_stream_open_v1("fhostingesps6bly.onion", 80);
}

void open_callback(void *c)
{
    #define GETREQUEST "GET / HTTP/1.0\r\n\r\n"
	//#define GETREQUEST "GET http://silkroadvb5piz3r.onion/silkroad/home HTTP/1.0\r\n\r\n"
	conn = c;
	onionroute_stream_write_v1(conn, GETREQUEST, sizeof(GETREQUEST));
	onionroute_stream_flush_v1(conn);
}

void rcv_callback(void *c, size_t size, char* data)
{
	char* ndata = (char*) _alloca(size + 1);
	memcpy(ndata, data, size);
	ndata[size] = 0;
	printf("----------> Received data: %s", ndata);
}

/* only do this once as Tor connection could have gone down */
int launched = 0;

void progresscallback(bootstrap_status_t_v1 status, int progress)
{
	if(BOOTSTRAP_STATUS_DONE == status)
	{

		/* open a connection */

		if(!launched)
		{
		    //onionroute_stream_open_v1("mesra.kl.my.dal.net", 6667);

			//onionroute_stream_open_v1("onionroute.org", 80); // <--- Seems our address is being filtered on several Tor exit nodes by IP.
			                                                 
			//onionroute_stream_open_v1("www.wikimedia.org", 80);
			onionroute_stream_open_v1("fhostingesps6bly.onion", 80);
			//onionroute_stream_open_v1("www.torproject.org", 80);
			//onionroute_stream_open_v1("hpuuigeld2cz2fd3.onion", 80);
			//onionroute_stream_open_v1("xycpusearchon2mc.onion", 80);
			//onionroute_stream_open_v1("silkroadvb5piz3r.onion", 80);
			//onionroute_stream_open_v1("kpvz7ki2v5agwt35.onion", 80);
			//onionroute_stream_open_v1("4srv4q3apzqylwob.onion", 80);
		    launched = 1;
		}
	}
}

void logcallback(int severity, log_domain_mask_t_v1 domain, const char *funcname, const char *format, va_list ap)
{
	char buf[10024];
	char *end_of_prefix=NULL;
	size_t msg_len = 0;

	/* call helper function */
	end_of_prefix = onionroute_format_msg_v1(buf, sizeof(buf), domain, severity, funcname, format, ap, &msg_len);

	OutputDebugStringA(buf);

	if(LOG_WARN == severity || LOG_NOTICE == severity)
	printf("%s", buf);
}

int _tmain(int argc, _TCHAR* argv[])
{
	int result;

	onionroute_set_bootstrap_callback_v1(&progresscallback);
	onionroute_set_log_callback_v1(&logcallback);
	onionroute_set_stream_close_callback_v1(&close_callback);
	onionroute_set_stream_open_callback_v1(&open_callback);
	onionroute_set_stream_data_received_callback_v1(&rcv_callback);

	onionroute_init_v1();
	
	/* this API function is subject to change */
	onionroute_setconf("Log=debug\r\n", 1);

	result = onionroute_do_main_loop_v1();
	return 0;
}