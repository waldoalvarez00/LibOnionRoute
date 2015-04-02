/* Copyright (c) 2013 Waldo Alvarez Cañizares, http://onionroute.org */
/* See LICENSE for licensing information */
/* dllmain.c : Defines the entry point for the DLL application. */
#include "stdafx.h"



BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
					 )
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
	case DLL_PROCESS_DETACH:
		break;
	}
	return TRUE;
}

