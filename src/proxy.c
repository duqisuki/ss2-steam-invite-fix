// SS2 Steam Invite Fix вЂ” proxy steam_api.dll for Serious Sam 2 (Steam, patch 2.200+).
//
// The game hosts online sessions in a Steam lobby and can join a lobby by ID
// (console: sam_strServerName="<lobby id>"; samStartClient();), but it never
// handles Steam's "friend accepted invite / Join Game" events, so accepting an
// invite does nothing. This proxy forwards every export to the original
// steam_api (renamed steam_api_orig.dll) and adds the missing handlers:
//   * GameLobbyJoinRequested_t       (invite accepted while the game is running)
//   * GameRichPresenceJoinRequested_t (connect string with +connect_lobby)
//   * "+connect_lobby <id>" on the command line (Steam launched the game from an invite)
// The join itself runs inside SteamAPI_RunCallbacks, i.e. on the game's main thread.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

typedef unsigned long long u64;

// ---- original steam_api ----------------------------------------------------
static HMODULE g_orig;
typedef int  (__cdecl *PFN_Init)(void);
typedef void (__cdecl *PFN_Void)(void);
typedef void (__cdecl *PFN_RegCb)(void *cb, int id);
typedef void (__cdecl *PFN_UnregCb)(void *cb);
static PFN_Init  o_Init, o_InitSafe;
static PFN_Void  o_RunCallbacks, o_Shutdown;
static PFN_RegCb o_RegisterCallback;
static PFN_UnregCb o_UnregisterCallback;

// ---- engine console --------------------------------------------------------
typedef int (__cdecl *PFN_scrExecuteCommand)(const char *cmd, unsigned long flags);
static PFN_scrExecuteCommand p_scrExecuteCommand;

// ---- state -----------------------------------------------------------------
typedef void *(__cdecl *PFN_Iface)(void);
static PFN_Iface o_SteamMatchmaking;
// ISteamMatchmaking009 vtable slot 14: SteamAPICall_t JoinLobby(CSteamID) (same slot the game uses)
typedef u64 (__attribute__((thiscall)) *PFN_JoinLobby)(void *self, u64 lobby);

static volatile u64 g_pendingLobby;   // lobby to join, 0 = none
static u64   g_joiningLobby;          // JoinLobby sent, waiting for LobbyEnter_t
static volatile int g_lobbyEntered;   // LobbyEnter_t arrived for g_joiningLobby
static DWORD g_joinStartTick;
static int   g_fromCommandLine;       // pending join came from startup args
static DWORD g_gameReadyTick;         // when Sam2Game.dll was first seen
static int   g_callbacksRegistered;
static char  g_logPath[MAX_PATH];

static void logf(const char *fmt, ...)
{
    if (!g_logPath[0]) return;
    FILE *f = fopen(g_logPath, "a");
    if (!f) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(f, "[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static const char *u64s(u64 v) // decimal, rotating buffers (no %llu in old msvcrt)
{
    static char buf[4][24]; static int k;
    char *b = buf[k++ & 3], t[24]; int i = 0, j = 0;
    do { t[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i) b[j++] = t[--i];
    b[j] = 0; return b;
}

static int is_lobby_id(u64 id)
{
    unsigned hi = (unsigned)(id >> 32);
    return (hi & 0xF00000) == 0x800000 && (hi & 0x40000); // Chat account type + lobby flag
}

// Finds "+connect_lobby <digits>" in s; returns 0 if absent.
static u64 parse_connect_lobby(const char *s)
{
    if (!s) return 0;
    const char *p = strstr(s, "+connect_lobby");
    if (!p) return 0;
    p += 14;
    while (*p == ' ' || *p == '\t' || *p == '"') p++;
    u64 v = _strtoui64(p, NULL, 10);
    return is_lobby_id(v) ? v : 0;
}

static void queue_join(u64 lobby, const char *why)
{
    if (!is_lobby_id(lobby)) { logf("ignored %s: %s is not a lobby id", why, u64s(lobby)); return; }
    g_pendingLobby = lobby;
    logf("join queued (%s): lobby %s", why, u64s(lobby));
}

// ---- Steam callback objects (old CCallbackBase ABI, MSVC vtable order) -----
// vtable: [0] Run(void*, bool, SteamAPICall_t)  [1] Run(void*)  [2] GetCallbackSizeBytes()
typedef struct CallbackObj {
    void **vtbl;
    unsigned char flags;
    int id;
} CallbackObj;

#pragma pack(push, 8)
typedef struct { u64 lobby; u64 friendId; } GameLobbyJoinRequested_t;       // 333
typedef struct { u64 friendId; char connect[256]; } GameRichPresenceJoinRequested_t; // 337
typedef struct { u64 lobby; unsigned perms; unsigned char locked; unsigned response; } LobbyEnter_t; // 504
#pragma pack(pop)

static void handle_callback(CallbackObj *self, void *param)
{
    if (!param) return;
    if (self->id == 333) {
        GameLobbyJoinRequested_t *e = (GameLobbyJoinRequested_t *)param;
        queue_join(e->lobby, "GameLobbyJoinRequested");
    } else if (self->id == 337) {
        GameRichPresenceJoinRequested_t *e = (GameRichPresenceJoinRequested_t *)param;
        e->connect[255] = 0;
        logf("GameRichPresenceJoinRequested: '%s'", e->connect);
        u64 l = parse_connect_lobby(e->connect);
        if (l) queue_join(l, "GameRichPresenceJoinRequested");
    } else if (self->id == 504) {
        LobbyEnter_t *e = (LobbyEnter_t *)param;
        if (g_joiningLobby && e->lobby == g_joiningLobby) {
            logf("LobbyEnter: lobby %s response %u", u64s(e->lobby), e->response);
            g_lobbyEntered = (e->response == 1) ? 1 : -1;
        }
    }
}

static void __attribute__((thiscall)) cb_RunResult(CallbackObj *self, void *param, int ioFailure, u64 call)
{ (void)ioFailure; (void)call; handle_callback(self, param); }
static void __attribute__((thiscall)) cb_Run(CallbackObj *self, void *param)
{ handle_callback(self, param); }
static int __attribute__((thiscall)) cb_Size333(CallbackObj *self)
{ (void)self; return sizeof(GameLobbyJoinRequested_t); }
static int __attribute__((thiscall)) cb_Size337(CallbackObj *self)
{ (void)self; return sizeof(GameRichPresenceJoinRequested_t); }

static void *vt333[3] = { (void *)cb_RunResult, (void *)cb_Run, (void *)cb_Size333 };
static int __attribute__((thiscall)) cb_Size504(CallbackObj *self)
{ (void)self; return sizeof(LobbyEnter_t); }
static void *vt337[3] = { (void *)cb_RunResult, (void *)cb_Run, (void *)cb_Size337 };
static void *vt504[3] = { (void *)cb_RunResult, (void *)cb_Run, (void *)cb_Size504 };
static CallbackObj cbLobbyJoin  = { vt333, 0, 333 };
static CallbackObj cbRPJoin     = { vt337, 0, 337 };
static CallbackObj cbLobbyEnter = { vt504, 0, 504 };

static void register_callbacks(void)
{
    if (g_callbacksRegistered || !o_RegisterCallback) return;
    o_RegisterCallback(&cbLobbyJoin, 333);
    o_RegisterCallback(&cbRPJoin, 337);
    o_RegisterCallback(&cbLobbyEnter, 504);
    g_callbacksRegistered = 1;
    logf("Steam callbacks registered");
}

// ---- join ------------------------------------------------------------------
// Step 2: we are a lobby member now, so GetLobbyOwner() works and the engine
// can connect to the host. Same as the game's own server browser does.
// Sam2Game.dll (2.224) function at RVA 0x19dc50 that closes every open menu screen.
// The game's own invite handler (CMSOnlineAcceptGameInvite::ConnectingToHost_OnSuccess)
// calls it right before starting the client; without it the joiner is stuck in a
// loading/menu screen while already connected. Called only if its bytes match.
static void close_menus(void)
{
    unsigned char *base = (unsigned char *)GetModuleHandleA("Sam2Game.dll");
    if (!base) return;
    unsigned char *fn = base + 0x19dc50;
    unsigned listVa = (unsigned)(base + 0x40a270);             // menu stack global
    static const unsigned char head[2] = { 0x8B, 0x0D };       // mov ecx, [list]
    static const unsigned char tail[4] = { 0x85, 0xC9, 0x74, 0x15 }; // test ecx,ecx; je
    if (IsBadReadPtr(fn, 32) || memcmp(fn, head, 2) || *(unsigned *)(fn + 2) != listVa || memcmp(fn + 6, tail, 4)) {
        logf("close_menus: game version mismatch, skipped");
        return;
    }
    ((void (__cdecl *)(void))fn)();
    logf("menus closed");
}

static void start_client(u64 lobby)
{
    char cmd[200];
    _snprintf(cmd, sizeof cmd,
              "sam_bJoinAsPrivate = 1; sam_bJoinAsObserver = 0; sam_strServerName = \"%s\";",
              u64s(lobby));
    cmd[sizeof cmd - 1] = 0;
    int r1 = p_scrExecuteCommand(cmd, 1);
    close_menus();
    int r2 = p_scrExecuteCommand("samStartClient();", 1);
    logf("samStartClient for lobby %s (set=%d start=%d)", u64s(lobby), r1, r2);
}

static void try_join(void)
{
    if (g_joiningLobby) {
        int entered = g_lobbyEntered;
        if (entered || GetTickCount() - g_joinStartTick > 15000) {
            u64 l = g_joiningLobby;
            g_joiningLobby = 0;
            if (entered < 0) logf("Steam refused lobby entry (lobby full/closed?)");
            else if (!entered) logf("LobbyEnter timeout, trying anyway");
            start_client(l);
        }
        return;
    }

    u64 lobby = g_pendingLobby;
    if (!lobby) return;

    HMODULE game = GetModuleHandleA("Sam2Game.dll");
    if (!game) return;                                   // game module not up yet
    if (!g_gameReadyTick) g_gameReadyTick = GetTickCount();

    // After a cold start from an invite, let the game finish booting into the menu.
    if (g_fromCommandLine && GetTickCount() - g_gameReadyTick < 12000) return;

    if (!p_scrExecuteCommand) {
        HMODULE eng = GetModuleHandleA("Engine.dll");
        if (eng) p_scrExecuteCommand = (PFN_scrExecuteCommand)GetProcAddress(eng, "?scrExecuteCommand@SeriousEngine@@YAHPBDK@Z");
        if (!p_scrExecuteCommand) { logf("scrExecuteCommand not found; cannot join"); g_pendingLobby = 0; return; }
    }

    g_pendingLobby = 0;
    g_fromCommandLine = 0;

    // Step 1: enter the host's Steam lobby.
    void *mm = o_SteamMatchmaking ? o_SteamMatchmaking() : NULL;
    if (!mm) { logf("SteamMatchmaking unavailable; starting client directly"); start_client(lobby); return; }
    PFN_JoinLobby join = (PFN_JoinLobby)(*(void ***)mm)[14];
    g_lobbyEntered = 0;
    g_joiningLobby = lobby;
    g_joinStartTick = GetTickCount();
    u64 call = join(mm, lobby);
    logf("JoinLobby %s sent (call %s)", u64s(lobby), u64s(call));
}

// ---- hooked exports --------------------------------------------------------
int __cdecl SteamAPI_Init(void)
{
    int r = o_Init ? o_Init() : 0;
    logf("SteamAPI_Init -> %d", r);
    if (r) register_callbacks();
    return r;
}

int __cdecl SteamAPI_InitSafe(void)
{
    int r = o_InitSafe ? o_InitSafe() : 0;
    logf("SteamAPI_InitSafe -> %d", r);
    if (r) register_callbacks();
    return r;
}

void __cdecl SteamAPI_RunCallbacks(void)
{
    if (o_RunCallbacks) o_RunCallbacks();
    try_join();
}

void __cdecl SteamAPI_Shutdown(void)
{
    if (g_callbacksRegistered && o_UnregisterCallback) {
        o_UnregisterCallback(&cbLobbyJoin);
        o_UnregisterCallback(&cbRPJoin);
        o_UnregisterCallback(&cbLobbyEnter);
        g_callbacksRegistered = 0;
    }
    if (o_Shutdown) o_Shutdown();
}

// ---- load ------------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(inst);

    char dir[MAX_PATH];
    DWORD n = GetModuleFileNameA(inst, dir, MAX_PATH);
    while (n && dir[n - 1] != '\\') n--;
    dir[n] = 0;
    _snprintf(g_logPath, MAX_PATH, "%sss2_invite_fix.log", dir);
    g_logPath[MAX_PATH - 1] = 0;

    char orig[MAX_PATH];
    _snprintf(orig, MAX_PATH, "%ssteam_api_orig.dll", dir);
    orig[MAX_PATH - 1] = 0;
    g_orig = LoadLibraryA(orig);
    if (!g_orig) {
        MessageBoxA(NULL, "steam_api_orig.dll not found next to steam_api.dll.\n"
                          "Reinstall the SS2 invite fix or verify game files in Steam.",
                    "SS2 Invite Fix", MB_ICONERROR);
        return FALSE;
    }
    o_Init               = (PFN_Init)GetProcAddress(g_orig, "SteamAPI_Init");
    o_InitSafe           = (PFN_Init)GetProcAddress(g_orig, "SteamAPI_InitSafe");
    o_RunCallbacks       = (PFN_Void)GetProcAddress(g_orig, "SteamAPI_RunCallbacks");
    o_Shutdown           = (PFN_Void)GetProcAddress(g_orig, "SteamAPI_Shutdown");
    o_RegisterCallback   = (PFN_RegCb)GetProcAddress(g_orig, "SteamAPI_RegisterCallback");
    o_UnregisterCallback = (PFN_UnregCb)GetProcAddress(g_orig, "SteamAPI_UnregisterCallback");
    o_SteamMatchmaking   = (PFN_Iface)GetProcAddress(g_orig, "SteamMatchmaking");

    logf("---- loaded (cmdline: %s)", GetCommandLineA());
    u64 l = parse_connect_lobby(GetCommandLineA());
    if (l) { g_fromCommandLine = 1; queue_join(l, "command line"); }
    return TRUE;
}
