#ifndef _FLASHLIGHT_EXT_H
#define _FLASHLIGHT_EXT_H

#include "smsdk_ext.h"
#include "dt_send.h"
#include "server_class.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include "eiface.h"

extern CGlobalVars	  *gpGlobals;

class FlashlightOptExt : public SDKExtension
{
public:
	virtual bool SDK_OnLoad(char *error, size_t maxlength, bool late);
	virtual void SDK_OnUnload();
	virtual bool SDK_OnMetamodLoad(ISmmAPI *ismm, char *error, size_t maxlen, bool late);
};

#endif
