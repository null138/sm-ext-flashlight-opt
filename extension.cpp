#include "extension.h"
#include "dt_send.h"
#include "CDetour/detours.h"
#include "iserver.h"
#include "iclient.h"

#define EF_DIMLIGHT 4

FlashlightOptExt g_FlashlightOptExt;
SMEXT_LINK(&g_FlashlightOptExt);

CGlobalVars			 *gpGlobals			  = nullptr;
IGameConfig			 *g_pGameConf		  = nullptr;
static SendVarProxyFn g_OriginalVarProxy  = nullptr;
static SendProp		 *g_pFEffectsProp	  = nullptr;
static IServer		 *g_pIServer		  = nullptr;
IServerGameEnts		 *gameents			  = nullptr;

static int	g_iCurrentRecipient		 = -1;
static bool g_bCallingForNull		 = false;
static bool g_bFirstTimeCalled		 = true;
static bool g_bSendSnapshots		 = false;
static bool g_bInPerClientCall		 = false;
static void *g_pCurrentGameClientPtr = nullptr;

CDetour *g_Detour_SendClientMessages  = nullptr;
CDetour *g_Detour_ShouldSendMessages  = nullptr;
CDetour *g_Detour_ComputeClientPacks  = nullptr;

SH_DECL_HOOK0(IServer, GetClientCount, const, false, int);

int Hook_GetClientCount()
{
	if (g_iCurrentRecipient > 0)
		RETURN_META_VALUE(MRES_SUPERCEDE, g_iCurrentRecipient);
	RETURN_META_VALUE(MRES_IGNORED, 0);
}

// im using some parts of code from SendProxy by Afronanny because i couldnt find another way to do this. the flashlights were blinking so this seems like the only way
DETOUR_DECL_MEMBER0(CGameClient_ShouldSendMessages, bool)
{
	if (!g_bSendSnapshots)
		return DETOUR_MEMBER_CALL(CGameClient_ShouldSendMessages)();

	if (g_bCallingForNull)
	{
		IClient *pClient = reinterpret_cast<IClient *>(reinterpret_cast<char *>(this) + 4);
		if (pClient->IsHLTV() || (pClient->IsConnected() && !pClient->IsActive()))
			return true;
		return false;
	}

	bool bOriginalResult = DETOUR_MEMBER_CALL(CGameClient_ShouldSendMessages)();
	if (!bOriginalResult)
		return false;

	if (this == g_pCurrentGameClientPtr)
		return true;

	return false;
}

DETOUR_DECL_MEMBER1(CGameServer_SendClientMessages, void, bool, bSendSnapshots)
{
	if (!bSendSnapshots)
	{
		g_bSendSnapshots = false;
		return DETOUR_MEMBER_CALL(CGameServer_SendClientMessages)(false);
	}
	g_bSendSnapshots = true;

	if (g_bInPerClientCall)
		return DETOUR_MEMBER_CALL(CGameServer_SendClientMessages)(true);

	if (!g_pIServer)
		g_pIServer = engine->GetIServer();
	if (!g_pIServer)
		return DETOUR_MEMBER_CALL(CGameServer_SendClientMessages)(true);

	if (g_bFirstTimeCalled)
	{
		SH_ADD_HOOK(IServer, GetClientCount, g_pIServer, SH_STATIC(Hook_GetClientCount), false);
		g_bFirstTimeCalled = false;
	}

	bool bCalledForNullThisTime = false;
	for (int i = 1; i <= playerhelpers->GetMaxClients(); i++)
	{
		IGamePlayer *pPlayer = playerhelpers->GetGamePlayer(i);
		bool bFake = pPlayer->IsFakeClient() && !pPlayer->IsSourceTV();

		volatile IClient *pClient = nullptr;
		if (!pPlayer->IsConnected() || bFake || (pClient = g_pIServer->GetClient(i - 1)) == nullptr)
		{
			if (!bCalledForNullThisTime && !g_bCallingForNull)
			{
				g_bCallingForNull		= true;
				g_iCurrentRecipient		= -1;
				g_pCurrentGameClientPtr = nullptr;
				DETOUR_MEMBER_CALL(CGameServer_SendClientMessages)(true);
				g_bCallingForNull		= false;
			}
			bCalledForNullThisTime = true;
			continue;
		}

		if (!pPlayer->IsInGame())
		{
			g_pCurrentGameClientPtr = nullptr;
			g_iCurrentRecipient		= -1;
		}
		else
		{
			g_pCurrentGameClientPtr = reinterpret_cast<char *>(const_cast<IClient *>(pClient)) - 4;
			g_iCurrentRecipient		= i;
		}

		g_bInPerClientCall = true;
		DETOUR_MEMBER_CALL(CGameServer_SendClientMessages)(true);
		g_bInPerClientCall = false;

		g_pCurrentGameClientPtr = nullptr;
		g_iCurrentRecipient		= -1;
	}

	g_pCurrentGameClientPtr = nullptr;
	g_iCurrentRecipient		= -1;
	g_bSendSnapshots		= false;
}

void __cdecl SV_ComputeClientPacks_ActualCall(int iClientCount, void **pClients, void *pSnapShot);

DETOUR_DECL_STATIC3(SV_ComputeClientPacks, void, int, iClientCount, void **, pClients, void *, pSnapShot)
{
	if (!iClientCount || !g_bSendSnapshots || pClients[0] != g_pCurrentGameClientPtr)
		return SV_ComputeClientPacks_ActualCall(iClientCount, pClients, pSnapShot);

	if (g_iCurrentRecipient > 0 && g_iCurrentRecipient < 65)
	{
		for (int i = 1; i <= playerhelpers->GetMaxClients(); i++)
		{
			IGamePlayer *pPlayer = playerhelpers->GetGamePlayer(i);
			if (!pPlayer || !pPlayer->IsInGame()) continue;
			edict_t *pEdict = pPlayer->GetEdict();
			if (!pEdict) continue;

			CBaseEntity *pEnt = gameents->EdictToBaseEntity(pEdict);
			if (!pEnt) continue;

			int fEffects = *(int *)((unsigned char *)pEnt + g_pFEffectsProp->GetOffset());
			if (fEffects & EF_DIMLIGHT)
				pEdict->m_fStateFlags |= FL_FULL_EDICT_CHANGED | FL_EDICT_CHANGED;
			else if (!(pEdict->m_fStateFlags & FL_EDICT_CHANGED))
				pEdict->m_fStateFlags |= FL_EDICT_CHANGED;
		}
	}

	return SV_ComputeClientPacks_ActualCall(iClientCount, pClients, pSnapShot);
}

void __cdecl SV_ComputeClientPacks_ActualCall(int iClientCount, void **pClients, void *pSnapShot)
{
	return DETOUR_STATIC_CALL(SV_ComputeClientPacks)(iClientCount, pClients, pSnapShot);
}

static void Proxy_fEffects(const SendProp *pProp, const void *pStruct, const void *pData, DVariant *pOut, int iElement, int objectID)
{
	g_OriginalVarProxy(pProp, pStruct, pData, pOut, iElement, objectID);
	if (g_iCurrentRecipient > 0 && g_iCurrentRecipient < 65 && objectID > 0 && objectID < 65 && g_iCurrentRecipient != objectID)
		pOut->m_Int &= ~EF_DIMLIGHT;
}

static SendProp *FindSendPropByName(SendTable *pTable, const char *name)
{
	if (!pTable) return nullptr;
	for (int i = 0; i < pTable->GetNumProps(); i++)
	{
		SendProp *p = pTable->GetProp(i);
		if (!p) continue;
		if (strcmp(p->GetName(), name) == 0) return p;
		if (p->GetType() == DPT_DataTable)
		{
			SendProp *f = FindSendPropByName(p->GetDataTable(), name);
			if (f) return f;
		}
	}
	return nullptr;
}

bool FlashlightOptExt::SDK_OnLoad(char *error, size_t maxlength, bool late)
{
	char conf_error[256] = "";
	if (!gameconfs->LoadGameConfigFile("flashlight_opt.games", &g_pGameConf, conf_error, sizeof(conf_error)))
	{
		snprintf(error, maxlength, "Could not load flashlight_opt.games: %s", conf_error);
		return false;
	}

	CDetourManager::Init(g_pSM->GetScriptingEngine(), g_pGameConf);

	g_Detour_SendClientMessages = DETOUR_CREATE_MEMBER(CGameServer_SendClientMessages, "CGameServer_SendClientMessages");
	if (!g_Detour_SendClientMessages)
	{
		snprintf(error, maxlength, "Failed to detour CGameServer::SendClientMessages");
		return false;
	}

	g_Detour_ShouldSendMessages = DETOUR_CREATE_MEMBER(CGameClient_ShouldSendMessages, "CGameClient_ShouldSendMessages");
	if (!g_Detour_ShouldSendMessages)
	{
		snprintf(error, maxlength, "Failed to detour CGameClient::ShouldSendMessages");
		return false;
	}

	g_Detour_ComputeClientPacks = DETOUR_CREATE_STATIC(SV_ComputeClientPacks, "SV_ComputeClientPacks");
	if (!g_Detour_ComputeClientPacks)
	{
		snprintf(error, maxlength, "Failed to detour SV_ComputeClientPacks");
		return false;
	}

	g_Detour_SendClientMessages->EnableDetour();
	g_Detour_ShouldSendMessages->EnableDetour();
	g_Detour_ComputeClientPacks->EnableDetour();

	ServerClass *pClass = nullptr;
	for (ServerClass *c = gamedll->GetAllServerClasses(); c; c = c->m_pNext)
		if (strcmp(c->GetName(), "CCSPlayer") == 0) { pClass = c; break; }

	if (!pClass)
	{ snprintf(error, maxlength, "CCSPlayer not found"); return false; }

	g_pFEffectsProp = FindSendPropByName(pClass->m_pTable, "m_fEffects");
	if (!g_pFEffectsProp)
	{ snprintf(error, maxlength, "m_fEffects not found"); return false; }

	g_OriginalVarProxy = g_pFEffectsProp->GetProxyFn();
	if (!g_OriginalVarProxy)
	{ snprintf(error, maxlength, "m_fEffects proxy not found"); return false; }

	g_pFEffectsProp->SetProxyFn(Proxy_fEffects);

	g_pIServer = engine->GetIServer();

	return true;
}

bool FlashlightOptExt::SDK_OnMetamodLoad(ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
	GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer, INTERFACEVERSION_VENGINESERVER);
	GET_V_IFACE_ANY(GetServerFactory, gameents, IServerGameEnts, INTERFACEVERSION_SERVERGAMEENTS);
	gpGlobals = ismm->GetCGlobals();
	return true;
}

void FlashlightOptExt::SDK_OnUnload()
{
	if (g_pFEffectsProp && g_OriginalVarProxy)
		g_pFEffectsProp->SetProxyFn(g_OriginalVarProxy);

	if (g_pIServer && !g_bFirstTimeCalled)
		SH_REMOVE_HOOK(IServer, GetClientCount, g_pIServer, SH_STATIC(Hook_GetClientCount), false);

	if (g_Detour_SendClientMessages)
	{ g_Detour_SendClientMessages->Destroy(); g_Detour_SendClientMessages = nullptr; }
	if (g_Detour_ShouldSendMessages)
	{ g_Detour_ShouldSendMessages->Destroy(); g_Detour_ShouldSendMessages = nullptr; }
	if (g_Detour_ComputeClientPacks)
	{ g_Detour_ComputeClientPacks->Destroy(); g_Detour_ComputeClientPacks = nullptr; }

	if (g_pGameConf)
	{ gameconfs->CloseGameConfigFile(g_pGameConf); g_pGameConf = nullptr; }

	g_pFEffectsProp			= nullptr;
	g_OriginalVarProxy		= nullptr;
	g_pIServer				= nullptr;
	g_iCurrentRecipient		= -1;
	g_pCurrentGameClientPtr = nullptr;
	g_bFirstTimeCalled		= true;
	g_bInPerClientCall		= false;
	g_bSendSnapshots		= false;
	g_bCallingForNull		= false;
}