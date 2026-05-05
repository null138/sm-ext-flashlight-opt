#include "extension.h"
#include "dt_send.h"
#include "CDetour/detours.h"
#include "iserver.h"
#include "iclient.h"

#define EF_DIMLIGHT 4

FlashlightOptExt g_FlashlightOptExt;
SMEXT_LINK(&g_FlashlightOptExt);

CGlobalVars			 *gpGlobals			 = nullptr;
IGameConfig			 *g_pGameConf		 = nullptr;
static SendVarProxyFn g_OriginalVarProxy = nullptr;
static SendProp		 *g_pFEffectsProp	 = nullptr;
static IServer		 *g_pIServer		 = nullptr;
static int			  g_iCurrentRecipient = -1;
static bool			  g_bCallingForNull	 = false;

CDetour *g_Detour_SendClientMessages = nullptr;
CDetour *g_Detour_ShouldSendMessages = nullptr;

SH_DECL_HOOK0(IServer, GetClientCount, const, false, int);

int Hook_GetClientCount()
{
	if (g_iCurrentRecipient > 0)
		RETURN_META_VALUE(MRES_SUPERCEDE, g_iCurrentRecipient);
	RETURN_META_VALUE(MRES_IGNORED, 0);
}

// i couldnt find another way to get the current player about to receive the snapshot. having to hook 2 functions just for that is meh but should be ok?
DETOUR_DECL_MEMBER0(CGameClient_ShouldSendMessages, bool)
{
	if (!g_bCallingForNull)
		return DETOUR_MEMBER_CALL(CGameClient_ShouldSendMessages)();

	IClient *pClient = reinterpret_cast<IClient *>(reinterpret_cast<char *>(this) + 4);
	if (pClient->IsHLTV() || (pClient->IsConnected() && !pClient->IsActive()))
		return true;
	return false;
}

// part 2 of this BS...
DETOUR_DECL_MEMBER1(CGameServer_SendClientMessages, void, bool, bSendSnapshots)
{
	if (!bSendSnapshots)
		return DETOUR_MEMBER_CALL(CGameServer_SendClientMessages)(false);
	if (!g_pIServer)
		g_pIServer = engine->GetIServer();
	if (!g_pIServer)
		return DETOUR_MEMBER_CALL(CGameServer_SendClientMessages)(true);

	bool bNullDone = false;
	for (int i = 1; i <= playerhelpers->GetMaxClients(); i++)
	{
		IGamePlayer *pPlayer = playerhelpers->GetGamePlayer(i);
		bool bRealPlayer = pPlayer && pPlayer->IsConnected() && pPlayer->IsInGame() && !pPlayer->IsFakeClient();

		if (bRealPlayer)
		{
			IClient *pClient = g_pIServer->GetClient(i - 1);
			if (!pClient) goto do_null;

			g_iCurrentRecipient = i;
			DETOUR_MEMBER_CALL(CGameServer_SendClientMessages)(true);
			g_iCurrentRecipient = -1;
			continue;
		}

	do_null:
		if (!bNullDone && !g_bCallingForNull)
		{
			g_bCallingForNull	= true;
			g_iCurrentRecipient = -1;
			DETOUR_MEMBER_CALL(CGameServer_SendClientMessages)(true);
			g_bCallingForNull	= false;
			bNullDone = true;
		}
	}

	g_iCurrentRecipient = -1;
}

static void Proxy_fEffects(const SendProp *pProp, const void *pStruct, const void *pData, DVariant *pOut, int iElement, int objectID)
{
	// resend with just changed values
	g_OriginalVarProxy(pProp, pStruct, pData, pOut, iElement, objectID);
	// we dont need to touch anything beside players
	if (g_iCurrentRecipient > 0 && g_iCurrentRecipient < 64 && objectID > 0 && objectID < 64 && g_iCurrentRecipient != objectID)
	{
		pOut->m_Int &= ~EF_DIMLIGHT;
	}
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

	g_Detour_SendClientMessages->EnableDetour();
	g_Detour_ShouldSendMessages->EnableDetour();

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
	if (g_pIServer)
		SH_ADD_HOOK(IServer, GetClientCount, g_pIServer, SH_STATIC(Hook_GetClientCount), false);

	return true;
}

bool FlashlightOptExt::SDK_OnMetamodLoad(ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
	GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer, INTERFACEVERSION_VENGINESERVER);
	gpGlobals = ismm->GetCGlobals();
	return true;
}

void FlashlightOptExt::SDK_OnUnload()
{
	if (g_pFEffectsProp && g_OriginalVarProxy)
		g_pFEffectsProp->SetProxyFn(g_OriginalVarProxy);

	if (g_pIServer)
		SH_REMOVE_HOOK(IServer, GetClientCount, g_pIServer, SH_STATIC(Hook_GetClientCount), false);

	if (g_Detour_SendClientMessages)
	{ g_Detour_SendClientMessages->Destroy(); g_Detour_SendClientMessages = nullptr; }
	if (g_Detour_ShouldSendMessages)
	{ g_Detour_ShouldSendMessages->Destroy(); g_Detour_ShouldSendMessages = nullptr; }

	if (g_pGameConf)
	{ gameconfs->CloseGameConfigFile(g_pGameConf); g_pGameConf = nullptr; }

	g_pFEffectsProp = nullptr;
	g_OriginalVarProxy = nullptr;
	g_pIServer = nullptr;
	g_iCurrentRecipient = -1;
}