// crashy.cpp - a deliberately badly behaved plugin: a seed for fault-injection
// tests of the plugin loader, in the mock host now and in the game once SWSE
// loads plugins. NEVER SHIP IT.
//
//   crash              null write inside a command
//   crashy frame       the next frame callback faults
//   crashy throw       a C++ exception escapes a command
//   crashy overflow    unbounded recursion (stack overflow) inside a command
//   crashy callee      a fault inside ntdll, in a call the plugin made
//                      (attribution must still name crashy.dll)
//   crashy slow        the next frame callback sleeps 120 ms (a stall to attribute)
//   crashy thread      calls Execute from a worker thread (expects SWSE_E_THREAD)
//   crashy recurse     Execute("crashy recurse") until the host stops it
//                      (expects SWSE_E_NESTING)
// Its Load also registers `hello`, which the hello example offers too (both
// stand; the plugin later in load order runs it), `echo` (a built-in: refused)
// and an invalid name (refused).
//
// Built with /DCRASHY_NAME=\"crashy2\" it is a second copy under another name
// (file crashy2.dll, command `crashy2 <sub>`, no `crash`/`hello`): a fault
// switches a plugin off for the session, so the in-game fault pass
// (tools\swse_plugin_tests.ps1) needs one copy per fault it injects.
#include "swse_plugin_api.h"
#include <windows.h>

#ifndef CRASHY_NAME
#define CRASHY_NAME "crashy"
#define CRASHY_MAIN 1
#endif

static SWSEPluginHandle      g_self;
static const SWSEConsoleAPI* g_con;
static const SWSELogAPI*     g_log;
static volatile SWSEStatus   g_threadStatus = 12345;
static volatile int          g_armFault, g_armSlow;

static DWORD WINAPI Worker(LPVOID) {
    g_threadStatus = g_con->Execute(g_self, "echo from-a-worker-thread");
    return 0;
}

#pragma warning(push)
#pragma warning(disable: 4717)        // "recursive on all control paths" - that is the point
static int Recurse(volatile int* depth) {
    volatile char pad[4096];
    pad[0] = (char)*depth;
    (*depth)++;
    return Recurse(depth) + pad[0];
}
#pragma warning(pop)

static void SWSE_CALL OnTick(const SWSEFrameInfo*, void*) {
    if (g_armSlow) { g_armSlow = 0; Sleep(120); }
    if (g_armFault) { g_armFault = 0; *(volatile int*)0x10 = 1; }
}

static void SWSE_CALL Cmd_Crashy(int32_t argc, const char* const* argv, void*) {
    const char* sub = (argc > 1) ? argv[1] : "";
    if (!lstrcmpiA(sub, "thread")) {
        HANDLE h = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        if (h) { WaitForSingleObject(h, 5000); CloseHandle(h); }
        SWSE_Printf(g_con, g_self, CRASHY_NAME ": Execute from a worker thread returned %d (SWSE_E_THREAD is %d)",
                    (int)g_threadStatus, SWSE_E_THREAD);
    } else if (!lstrcmpiA(sub, "recurse")) {
        SWSEStatus st = g_con->Execute(g_self, CRASHY_NAME " recurse");
        if (st != SWSE_OK)
            SWSE_Printf(g_con, g_self, CRASHY_NAME ": nested Execute stopped with %d (SWSE_E_NESTING is %d)",
                        (int)st, SWSE_E_NESTING);
    } else if (!lstrcmpiA(sub, "frame")) {
        g_armFault = 1; SWSE_Printf(g_con, g_self, CRASHY_NAME ": the next frame callback will fault");
    } else if (!lstrcmpiA(sub, "slow")) {
        g_armSlow = 1;  SWSE_Printf(g_con, g_self, CRASHY_NAME ": the next frame callback will take 120 ms");
    } else if (!lstrcmpiA(sub, "throw")) {
        SWSE_Printf(g_con, g_self, CRASHY_NAME ": throwing a C++ exception out of a command");
        throw 42;
    } else if (!lstrcmpiA(sub, "overflow")) {
        volatile int depth = 0;
        SWSE_Printf(g_con, g_self, CRASHY_NAME ": recursing until the stack runs out");
        Recurse(&depth);
    } else if (!lstrcmpiA(sub, "callee")) {
        typedef void (WINAPI* MoveFn)(void*, const void*, SIZE_T);
        MoveFn move = (MoveFn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlMoveMemory");
        char dst[16];
        SWSE_Printf(g_con, g_self, CRASHY_NAME ": handing ntdll a bad pointer");
        if (move) move(dst, (const void*)0x10, sizeof(dst));
    } else {
        SWSE_Printf(g_con, g_self, "usage: " CRASHY_NAME " thread|recurse|frame|slow|throw|overflow|callee   (and `crash`)");
    }
}

static void SWSE_CALL Cmd_Crash(int32_t, const char* const*, void*) {
    SWSE_Printf(g_con, g_self, CRASHY_NAME ": writing through a null pointer");
    *(volatile int*)0 = 1;
}

static int32_t SWSE_CALL OnEnable(char* msg, int32_t len, void*) {
    SWSE_Msg(msg, len, "crashy on - every `crashy` subcommand misbehaves on purpose");
    return 1;
}

SWSE_PLUGIN_EXPORT int32_t SWSE_CALL SWSEPlugin_Query(const SWSEInterface*, SWSEPluginInfo* info) {
    info->apiVersion     = SWSE_PLUGIN_API_VERSION;
    info->name           = CRASHY_NAME;
    info->version        = SWSE_MAKE_VERSION(0, 1, 0, 0);
    info->minSwseVersion = SWSE_MAKE_VERSION(1, 1, 0, 0);
    info->description    = "fault-injection test plugin - never ship";
    return 1;
}

SWSE_PLUGIN_EXPORT int32_t SWSE_CALL SWSEPlugin_Load(const SWSEInterface* swse) {
    g_self = swse->self;
    g_con = SWSE_GET(swse, SWSEConsoleAPI, SWSE_IFACE_CONSOLE, 1);
    g_log = SWSE_GET(swse, SWSELogAPI, SWSE_IFACE_LOG, 1);
    const SWSEFrameAPI*    frame = SWSE_GET(swse, SWSEFrameAPI, SWSE_IFACE_FRAME, 1);
    const SWSEFeaturesAPI* feat  = SWSE_GET(swse, SWSEFeaturesAPI, SWSE_IFACE_FEATURES, 1);
    if (!g_con || !g_log || !frame || !feat) return 0;

    // Tables newer than the host serves must come back NULL / absent.
    SWSE_Logf(g_log, g_self, SWSE_LOG_INFO, "console v2: %s, SWSE_HAS(Post): %d, GL table: %s",
              swse->GetInterface(SWSE_IFACE_CONSOLE, 2) ? "served" : "NULL",
              SWSE_HAS(g_con, SWSEConsoleAPI, Post) ? 1 : 0,
              swse->GetInterface(SWSE_IFACE_GL, 1) ? "served" : "NULL");

    SWSECommandDesc d = { 0 };
    d.size = sizeof(d); d.fn = Cmd_Crash;
#ifdef CRASHY_MAIN
    d.name = "hello";      // the hello example offers it too: both stand
    SWSE_Logf(g_log, g_self, SWSE_LOG_INFO, "register 'hello'     -> %d (SWSE_OK is %d)",
              (int)g_con->RegisterCommand(g_self, &d), SWSE_OK);
    SWSE_Logf(g_log, g_self, SWSE_LOG_INFO, "register 'hello' 2nd -> %d (SWSE_E_TAKEN is %d)",
              (int)g_con->RegisterCommand(g_self, &d), SWSE_E_TAKEN);
    d.name = "echo";       // a built-in
    SWSE_Logf(g_log, g_self, SWSE_LOG_INFO, "register 'echo'      -> %d (SWSE_E_TAKEN is %d)",
              (int)g_con->RegisterCommand(g_self, &d), SWSE_E_TAKEN);
    d.name = "bad name!";  // not a-z 0-9 _ - .
    SWSE_Logf(g_log, g_self, SWSE_LOG_INFO, "register 'bad name!' -> %d (SWSE_E_BADARG is %d)",
              (int)g_con->RegisterCommand(g_self, &d), SWSE_E_BADARG);

    d.name = "crash";  d.fn = Cmd_Crash;  d.help = "crash - fault on purpose";
    if (g_con->RegisterCommand(g_self, &d) != SWSE_OK) return 0;
#endif
    d.name = CRASHY_NAME; d.fn = Cmd_Crashy; d.help = CRASHY_NAME " thread|recurse|frame|slow|throw|overflow|callee";
    if (g_con->RegisterCommand(g_self, &d) != SWSE_OK) return 0;
    if (frame->Register(g_self, SWSE_FRAME_TICK, OnTick, nullptr) != SWSE_OK) return 0;
    feat->SetHandlers(g_self, OnEnable, nullptr, nullptr);
    return 1;
}
