#ifndef _INCLUDE_SOURCEMOD_EXTENSION_CONFIG_H_
#define _INCLUDE_SOURCEMOD_EXTENSION_CONFIG_H_

#define SMEXT_CONF_NAME			"Flashlight Optimization"
#define SMEXT_CONF_DESCRIPTION	"Makes the flashlight be visible only to the owner"
#define SMEXT_CONF_VERSION		"1.0"
#define SMEXT_CONF_AUTHOR		"Madness (null138)"
#define SMEXT_CONF_URL			"https://github.com/null138/sm-ext-flashlight-opt"
#define SMEXT_CONF_LOGTAG		"FLASHLIGHT_OPT"
#define SMEXT_CONF_LICENSE		"GPL"
#define SMEXT_CONF_DATESTRING	__DATE__

#define SMEXT_LINK(name) SDKExtension *g_pExtensionIface = name;

#define SMEXT_CONF_METAMOD

#define SMEXT_ENABLE_FORWARDSYS
#define SMEXT_ENABLE_PLAYERHELPERS
#define SMEXT_ENABLE_GAMECONF
#define SMEXT_ENABLE_TIMERSYS

#endif
