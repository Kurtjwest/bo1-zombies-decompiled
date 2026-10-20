// Steam API library stand-in for the builds Valve ships no Steam API library for here: libsteam_api.so for 32-bit ARM
// Linux (and 32-bit x86 Linux when the SDK's linux32 copy is not in src/steam/redistributable_bin), steam_api64.dll
// for 64-bit Windows (the SDK copy has only the 32-bit steam_api.dll). It exports what the engine links against
// (src/steam headers) and behaves as the real library does without a running Steam client: SteamAPI_Init fails,
// every interface accessor returns null and no callback ever arrives, so the game runs offline, as it does on the
// other platforms when Steam is not running. Valve's own library of the same name can take its place.

#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#define S_API extern "C" __declspec(dllexport)
#else
#define S_API extern "C" __attribute__((visibility("default")))
#endif

typedef int32_t HSteamUser;
typedef int32_t HSteamPipe;
typedef uint64_t SteamAPICall_t;
class CCallbackBase;

namespace
{
const char kNoSteam[] = "this build has no Steam support (no Steam API library for its processor)";
}

// SteamAPI_Init / SteamAPI_InitEx (steam_api.h): k_ESteamAPIInitResult_NoSteamClient
S_API int SteamInternal_SteamAPI_Init(const char *, char *pOutErrMsg)
{
    if (pOutErrMsg)
        snprintf(pOutErrMsg, 1024, "%s", kNoSteam);
    return 2;
}
S_API bool SteamAPI_Init() { return false; }
S_API int SteamAPI_InitFlat(char *pOutErrMsg) { return SteamInternal_SteamAPI_Init(nullptr, pOutErrMsg); }
S_API void SteamAPI_Shutdown() {}
S_API bool SteamAPI_RestartAppIfNecessary(uint32_t) { return false; }
S_API bool SteamAPI_IsSteamRunning() { return false; }
S_API void SteamAPI_RunCallbacks() {}
S_API void SteamAPI_ReleaseCurrentThreadMemory() {}

S_API HSteamPipe SteamAPI_GetHSteamPipe() { return 0; }
S_API HSteamUser SteamAPI_GetHSteamUser() { return 0; }
S_API HSteamPipe SteamGameServer_GetHSteamPipe() { return 0; }
S_API HSteamUser SteamGameServer_GetHSteamUser() { return 0; }
S_API bool SteamGameServer_Init(uint32_t, uint16_t, uint16_t, int, const char *) { return false; }
S_API int SteamInternal_GameServer_Init_V2(uint32_t, uint16_t, uint16_t, int, const char *, const char *, char *pOutErrMsg)
{
    if (pOutErrMsg)
        snprintf(pOutErrMsg, 1024, "%s", kNoSteam);
    return 2;
}
S_API void SteamGameServer_Shutdown() {}
S_API void SteamGameServer_RunCallbacks() {}

// The interface accessors (steam_api_internal.h): a context { init function, counter, interface pointer } whose
// init function stores what SteamInternal_FindOrCreate*Interface returns, null here.
S_API void *SteamInternal_ContextInit(void *pContextInitData)
{
    void **ctx = (void **)pContextInitData;
    if (ctx[1] == nullptr)
    {
        ctx[1] = (void *)1;
        ((void (*)(void *))ctx[0])(&ctx[2]);
    }
    return &ctx[2];
}
S_API void *SteamInternal_CreateInterface(const char *) { return nullptr; }
S_API void *SteamInternal_FindOrCreateUserInterface(HSteamUser, const char *) { return nullptr; }
S_API void *SteamInternal_FindOrCreateGameServerInterface(HSteamUser, const char *) { return nullptr; }

S_API void SteamAPI_RegisterCallback(CCallbackBase *, int) {}
S_API void SteamAPI_UnregisterCallback(CCallbackBase *) {}
S_API void SteamAPI_RegisterCallResult(CCallbackBase *, SteamAPICall_t) {}
S_API void SteamAPI_UnregisterCallResult(CCallbackBase *, SteamAPICall_t) {}
