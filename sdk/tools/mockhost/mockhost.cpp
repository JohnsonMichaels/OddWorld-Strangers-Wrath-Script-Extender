// mockhost.cpp - a stand-in for SWSE, for testing a plugin outside the game.
//
// It serves the MVP tables of swse_plugin_api.h with the dispatch rules SWSE
// is specified to follow (swse/research/PLUGIN_SYSTEM.md): a plugin is loaded
// only when its switch goes on; Query, Load, onEnable in that order; callbacks
// only while on; built-in command names refused, a name two plugins offer run
// by the one later in load order (here: later on the command line);
// render-thread-only calls refused from other threads; commands nested at most
// 8 deep; every call into a plugin fault-guarded, and a plugin that faults is
// switched off for the session and named with its DLL offset - of the faulting
// instruction, of the code that threw, or of its call on the stack.
//
// It is NOT SWSE. There is no game: level and player functions report "not
// ready", prefs lookups find nothing, and only a handful of console commands
// exist. Use it to exercise a plugin's Query/Load, commands and switch
// handling, and as a reference for the host side of the ABI.
//
//   mockhost <plugin.dll>... [-- <line>...]
//
// Each line is a console line, or one of: frame [n] | levelup | leveldown |
// modsreload | plugins | bench [n] (the guard's cost per call, T-P1). Build
// with build.bat (x86).
#define SWSE_HOST
#define SWSE_PLUGIN_NO_HELPERS
#include "swse_plugin_api.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#define MOCK_SWSE_VERSION SWSE_MAKE_VERSION(1, 1, 0, 0)
#define MAX_PLUGINS 16

// ---- plugins ----------------------------------------------------------------------
enum PState { PS_OFF = 0, PS_LOADED, PS_FAULTED, PS_REFUSED };
static const char* kStateName[] = { "off", "loaded", "FAULTED", "REFUSED" };

struct Plugin {
    char name[32], path[MAX_PATH], desc[128], why[240];
    HMODULE mod; unsigned base, sizeImg, version;
    SWSEInterface iface;
    PState state; bool on; bool inQuery;
    SWSESwitchFn onEnable, onDisable; void* swUser;
    DWORD faultCode; unsigned faultAddr; char faultWhere[64];
    double msLast, msWorst;
};
static Plugin g_pl[MAX_PLUGINS];
static int    g_plN = 0;
static DWORD  g_renderTid = 0;
static int    g_querying = -1;           // plugin inside Query: only Log is served
static int    g_inPluginCall = 0;        // depth of host->plugin calls (for compaction)

static Plugin* FromHandle(SWSEPluginHandle h) {
    unsigned i = (h & 0xFFu) - 1u;
    if ((h >> 8) != 0x5E5Eu || i >= (unsigned)g_plN) return nullptr;
    return &g_pl[i];
}
static bool OnRender() { return GetCurrentThreadId() == g_renderTid; }
static void Out(const char* s) { printf("  | %s\n", s); }     // the console

// ---- registries -----------------------------------------------------------------------
struct PCmd   { char name[32]; SWSECommandFn fn; void* user; int owner; unsigned flags; bool dead; };
struct PFrame { unsigned phase; SWSEFrameFn fn; void* user; int owner; bool dead; };
struct PEvent { unsigned id; SWSEEventFn fn; void* user; int owner; bool dead; };
static PCmd   g_cmds[128];   static int g_cmdN = 0;
static PFrame g_frames[64];  static int g_frameN = 0;
static PEvent g_events[64];  static int g_eventN = 0;
static const char* kBuiltins[] = { "echo", "features", "help", "plugins", "hp", "status", "wind", "foliage" };

static bool IsBuiltin(const char* n) {
    for (int i = 0; i < (int)(sizeof(kBuiltins) / sizeof(kBuiltins[0])); i++)
        if (!lstrcmpiA(kBuiltins[i], n)) return true;
    return false;
}
static bool ValidCmdName(const char* n) {
    int len = n ? lstrlenA(n) : 0;
    if (len < 1 || len > 31) return false;
    for (int i = 0; i < len; i++) {
        char c = n[i] | 0x20;
        if (!((c >= 'a' && c <= 'z') || (n[i] >= '0' && n[i] <= '9') ||
              n[i] == '_' || n[i] == '-' || n[i] == '.')) return false;
    }
    return true;
}

// ---- every call into a plugin goes through one of these ---------------------------------
// Plain functions with no C++ objects: __try cannot share a frame with
// destructors under /EHsc, the rule SWSE's own SEH blocks follow.
static bool InImage(const Plugin* p, unsigned a) { return a >= p->base && a < p->base + p->sizeImg; }

static void ModuleOf(unsigned addr, char* out, int len) {
    HMODULE m = nullptr; char path[MAX_PATH];
    lstrcpynA(out, "?", len);
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)addr, &m) && GetModuleFileNameA(m, path, MAX_PATH)) {
        const char* b = strrchr(path, '\\');
        lstrcpynA(out, b ? b + 1 : path, len);
    }
}

// The innermost return address on the stack that points into the plugin AND
// follows a CALL instruction. A fault inside code the plugin called (the CRT,
// the game, the driver) is still the plugin's call. Candidates without a real
// CALL before them are skipped: stale stack words look like return addresses,
// and plain frame-pointer walks invent frames in frame-pointer-omitted code.
static unsigned PluginReturnOnStack(const Plugin* p, const CONTEXT* c) {
    const NT_TIB* tib = (const NT_TIB*)NtCurrentTeb();
    unsigned sp = c->Esp, top = (unsigned)(uintptr_t)tib->StackBase;
    for (unsigned a = sp; a + 4 <= top && a < sp + 256 * 1024; a += 4) {
        unsigned v = *(const unsigned*)(uintptr_t)a;
        if (!InImage(p, v) || v < p->base + 8) continue;
        const BYTE* r = (const BYTE*)(uintptr_t)v;
        if (r[-5] == 0xE8) return v;                                   // call rel32
        if (r[-6] == 0xFF && (r[-5] == 0x15 || (r[-5] & 0xF8) == 0x90)) return v;  // call [abs] / [reg+d32]
        if (r[-3] == 0xFF && (r[-2] & 0xF8) == 0x50) return v;         // call [reg+d8]
        if (r[-2] == 0xFF && ((r[-1] & 0xF8) == 0xD0 || (r[-1] & 0xF8) == 0x10)) return v;  // call reg / [reg]
    }
    return 0;
}

// Runs in the first pass of exception dispatch, while the faulting stack is
// intact - the only moment the stack can be read for attribution.
static int FaultFilter(Plugin* p, const char* where, EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    p->faultCode = er->ExceptionCode;
    p->faultAddr = (unsigned)(uintptr_t)er->ExceptionAddress;
    lstrcpynA(p->faultWhere, where, sizeof(p->faultWhere));
    char* w = p->why;
    const int wl = (int)sizeof(p->why);
    if (er->ExceptionCode == 0xE06D7363 && er->NumberParameters >= 3) {
        // An MSVC C++ exception: [2] is the ThrowInfo, in the thrower's image.
        unsigned ti = (unsigned)er->ExceptionInformation[2];
        char mod[64]; ModuleOf(ti, mod, sizeof(mod));
        wsprintfA(w, "a C++ exception escaped %s (thrown by %s)", where, mod);
    } else if (InImage(p, p->faultAddr)) {
        wsprintfA(w, "exception %08X in %s at %s.dll+0x%X", p->faultCode, where, p->name, p->faultAddr - p->base);
    } else {
        char mod[64]; ModuleOf(p->faultAddr, mod, sizeof(mod));
        unsigned ret = PluginReturnOnStack(p, ep->ContextRecord);
        if (ret) wsprintfA(w, "exception %08X in %s at %s+0x%X, called from %s.dll+0x%X", p->faultCode, where,
                           mod, p->faultAddr - (unsigned)(uintptr_t)GetModuleHandleA(mod), p->name, ret - p->base);
        else     wsprintfA(w, "exception %08X in %s at %08X (%s)", p->faultCode, where, p->faultAddr, mod);
    }
    w[wl - 1] = 0;
    return EXCEPTION_EXECUTE_HANDLER;
}
static void Faulted(Plugin* p) {
    if (p->faultCode == EXCEPTION_STACK_OVERFLOW) _resetstkoflw();   // re-arm the guard page
    p->state = PS_FAULTED; p->on = false;
    char b[320];
    wsprintfA(b, "plugin '%s' FAULTED: %s - switched off for this session", p->name, p->why);
    Out(b);
}
static int CallQuery(Plugin* p, SWSEPlugin_QueryFn fn, SWSEPluginInfo* info) {
    __try { return fn(&p->iface, info); }
    __except (FaultFilter(p, "Query", GetExceptionInformation())) { return -1; }
}
static int CallLoad(Plugin* p, SWSEPlugin_LoadFn fn) {
    __try { return fn(&p->iface); }
    __except (FaultFilter(p, "Load", GetExceptionInformation())) { return -1; }
}
static int CallSwitch(Plugin* p, SWSESwitchFn fn, char* msg, int len, const char* where) {
    __try { return fn(msg, len, p->swUser); }
    __except (FaultFilter(p, where, GetExceptionInformation())) { return -1; }
}
static bool CallCommand(Plugin* p, SWSECommandFn fn, int argc, const char* const* argv, void* u) {
    __try { fn(argc, argv, u); return true; }
    __except (FaultFilter(p, "a command", GetExceptionInformation())) { return false; }
}
static bool CallFrame(Plugin* p, SWSEFrameFn fn, const SWSEFrameInfo* fi, void* u) {
    __try { fn(fi, u); return true; }
    __except (FaultFilter(p, "a frame callback", GetExceptionInformation())) { return false; }
}
static bool CallEvent(Plugin* p, SWSEEventFn fn, unsigned id, const void* d, void* u) {
    __try { fn(id, d, u); return true; }
    __except (FaultFilter(p, "an event callback", GetExceptionInformation())) { return false; }
}
static double NowMs() {
    LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

// ---- log ---------------------------------------------------------------------------------
static void SWSE_CALL L_Write(SWSEPluginHandle self, int32_t level, const char* text) {
    Plugin* p = FromHandle(self);
    printf("  [swse_log] [%s] %s%s\n", p ? p->name : "?",
           level == SWSE_LOG_ERROR ? "ERROR: " : level == SWSE_LOG_WARN ? "WARN: " : "", text ? text : "");
}
static const SWSELogAPI kLog = { sizeof(SWSELogAPI), 1, L_Write };

// ---- console -----------------------------------------------------------------------------
static void Execute(const char* line);
static int g_execDepth = 0;

static void SWSE_CALL C_Print(SWSEPluginHandle self, const char* text) {
    if (!OnRender() || !FromHandle(self) || !text) return;
    char line[240];
    for (const char* s = text; *s; ) {                       // one line per '\n', cut at 239
        int n = 0;
        while (s[n] && s[n] != '\n') n++;
        int k = n < 239 ? n : 239;
        memcpy(line, s, k); line[k] = 0; Out(line);
        s += n; if (*s == '\n') s++;
    }
}
static SWSEStatus SWSE_CALL C_Register(SWSEPluginHandle self, const SWSECommandDesc* d) {
    if (!OnRender()) return SWSE_E_THREAD;
    Plugin* p = FromHandle(self); if (!p) return SWSE_E_BADHANDLE;
    if (!d || d->size < sizeof(SWSECommandDesc) || !d->fn || !ValidCmdName(d->name)) return SWSE_E_BADARG;
    if (IsBuiltin(d->name)) return SWSE_E_TAKEN;             // built-ins are never replaced
    for (int i = 0; i < g_cmdN; i++)                          // yours already
        if (!g_cmds[i].dead && g_cmds[i].owner == (int)(p - g_pl) && !lstrcmpiA(g_cmds[i].name, d->name))
            return SWSE_E_TAKEN;
    if (g_cmdN >= 128) return SWSE_E_FULL;
    for (int i = 0; i < g_cmdN; i++) {                        // another plugin offers it too
        if (g_cmds[i].dead || lstrcmpiA(g_cmds[i].name, d->name)) continue;
        // Both registrations stand; load order decides at run time.
        printf("  [swse_log] command '%s' is offered by %s and %s - the one later in load order runs\n",
               d->name, g_pl[g_cmds[i].owner].name, p->name);
    }
    PCmd& c = g_cmds[g_cmdN++];
    lstrcpynA(c.name, d->name, 32); c.fn = d->fn; c.user = d->user; c.flags = d->flags;
    c.owner = (int)(p - g_pl); c.dead = false;
    return SWSE_OK;
}
static SWSEStatus SWSE_CALL C_Unregister(SWSEPluginHandle self, const char* name) {
    if (!OnRender()) return SWSE_E_THREAD;
    Plugin* p = FromHandle(self); if (!p) return SWSE_E_BADHANDLE;
    for (int i = 0; i < g_cmdN; i++)
        if (!g_cmds[i].dead && g_cmds[i].owner == (int)(p - g_pl) && name && !lstrcmpiA(g_cmds[i].name, name)) {
            g_cmds[i].dead = true; return SWSE_OK;
        }
    return SWSE_E_NOTFOUND;
}
// SWSE's Execute only counts nesting for scripts and aliases, so a command that
// runs a command is counted here: the plugin API's Execute adds a level.
static SWSEStatus SWSE_CALL C_Execute(SWSEPluginHandle self, const char* line) {
    if (!OnRender()) return SWSE_E_THREAD;
    if (!FromHandle(self)) return SWSE_E_BADHANDLE;
    if (!line || !*line) return SWSE_E_BADARG;
    if (g_execDepth >= 8) { Out("command nesting too deep - aborting."); return SWSE_E_NESTING; }
    g_execDepth++; Execute(line); g_execDepth--;
    return SWSE_OK;
}
static int32_t SWSE_CALL C_IsOpen(void) { return 0; }
// Served at version 1: `size` stops before Post, exactly as an MVP SWSE would.
static const SWSEConsoleAPI kConsole = { (uint32_t)offsetof(SWSEConsoleAPI, Post), 1,
    C_Print, C_Register, C_Unregister, C_Execute, C_IsOpen, nullptr };

// ---- frame -------------------------------------------------------------------------------
static SWSEStatus SWSE_CALL F_Register(SWSEPluginHandle self, uint32_t phase, SWSEFrameFn fn, void* user) {
    if (!OnRender()) return SWSE_E_THREAD;
    Plugin* p = FromHandle(self); if (!p) return SWSE_E_BADHANDLE;
    if (!fn || phase > SWSE_FRAME_OVERLAY) return SWSE_E_BADARG;
    if (g_frameN >= 64) return SWSE_E_FULL;
    PFrame f = { phase, fn, user, (int)(p - g_pl), false };
    g_frames[g_frameN++] = f;
    return SWSE_OK;
}
static SWSEStatus SWSE_CALL F_Unregister(SWSEPluginHandle self, uint32_t phase, SWSEFrameFn fn, void* user) {
    if (!OnRender()) return SWSE_E_THREAD;
    Plugin* p = FromHandle(self); if (!p) return SWSE_E_BADHANDLE;
    for (int i = 0; i < g_frameN; i++) {
        PFrame& f = g_frames[i];
        if (!f.dead && f.owner == (int)(p - g_pl) && f.phase == phase && f.fn == fn && f.user == user) {
            f.dead = true; return SWSE_OK;
        }
    }
    return SWSE_E_NOTFOUND;
}
static int32_t SWSE_CALL F_IsRender(void) { return OnRender() ? 1 : 0; }
static const SWSEFrameAPI kFrame = { sizeof(SWSEFrameAPI), 1, F_Register, F_Unregister, F_IsRender };

// ---- events ------------------------------------------------------------------------------
static SWSEStatus SWSE_CALL E_Subscribe(SWSEPluginHandle self, uint32_t id, SWSEEventFn fn, void* user) {
    if (!OnRender()) return SWSE_E_THREAD;
    Plugin* p = FromHandle(self); if (!p) return SWSE_E_BADHANDLE;
    if (!fn) return SWSE_E_BADARG;
    if (id != SWSE_EV_LEVEL_UP && id != SWSE_EV_LEVEL_DOWN && id != SWSE_EV_MODS_RELOADED)
        return SWSE_E_UNSUPPORTED;                        // phase-2 events on an MVP host
    if (g_eventN >= 64) return SWSE_E_FULL;
    PEvent e = { id, fn, user, (int)(p - g_pl), false };
    g_events[g_eventN++] = e;
    return SWSE_OK;
}
static SWSEStatus SWSE_CALL E_Unsubscribe(SWSEPluginHandle self, uint32_t id, SWSEEventFn fn, void* user) {
    if (!OnRender()) return SWSE_E_THREAD;
    Plugin* p = FromHandle(self); if (!p) return SWSE_E_BADHANDLE;
    for (int i = 0; i < g_eventN; i++) {
        PEvent& e = g_events[i];
        if (!e.dead && e.owner == (int)(p - g_pl) && e.id == id && e.fn == fn && e.user == user) {
            e.dead = true; return SWSE_OK;
        }
    }
    return SWSE_E_NOTFOUND;
}
static const SWSEEventsAPI kEvents = { sizeof(SWSEEventsAPI), 1, E_Subscribe, E_Unsubscribe };

// ---- features ----------------------------------------------------------------------------
static SWSEStatus SWSE_CALL W_SetHandlers(SWSEPluginHandle self, SWSESwitchFn on, SWSESwitchFn off, void* user) {
    if (!OnRender()) return SWSE_E_THREAD;
    Plugin* p = FromHandle(self); if (!p) return SWSE_E_BADHANDLE;
    p->onEnable = on; p->onDisable = off; p->swUser = user;
    return SWSE_OK;
}
static int32_t SWSE_CALL W_IsOn(const char* name) {
    if (!name) return -1;
    for (int i = 0; i < g_plN; i++) if (!lstrcmpiA(g_pl[i].name, name)) return g_pl[i].on ? 1 : 0;
    return -1;
}
static const SWSEFeaturesAPI kFeatures = { sizeof(SWSEFeaturesAPI), 1, W_SetHandlers, W_IsOn };

// ---- mods: the plugin's own folder is real; there are no other mods ------------------------
static void SWSE_CALL M_ForEach(const char*, SWSEModFileFn, void*) {}
static int32_t SWSE_CALL M_Find(const char*, char* out, int32_t n) { if (out && n > 0) out[0] = 0; return 0; }
static int32_t SWSE_CALL M_Count(const char*) { return 0; }
static int32_t SWSE_CALL M_ModCount(void) { return 0; }
static SWSEStatus SWSE_CALL M_ModAt(int32_t, char*, int32_t, char*, int32_t) { return SWSE_E_NOTFOUND; }
static SWSEStatus SWSE_CALL M_Dir(char* out, int32_t n) {
    if (!out || n <= 0) return SWSE_E_BADARG;
    GetCurrentDirectoryA((DWORD)n, out); return SWSE_OK;
}
static SWSEStatus SWSE_CALL M_SelfDir(SWSEPluginHandle self, char* out, int32_t n) {
    Plugin* p = FromHandle(self); if (!p) return SWSE_E_BADHANDLE;
    if (!out || n <= 0) return SWSE_E_BADARG;
    lstrcpynA(out, p->path, n);
    char* s = strrchr(out, '\\'); if (s) *s = 0;         // ...\plugins
    s = strrchr(out, '\\'); if (s) *s = 0;               // the mod folder
    return SWSE_OK;
}
static SWSEStatus SWSE_CALL M_SelfPath(SWSEPluginHandle self, char* out, int32_t n) {
    Plugin* p = FromHandle(self); if (!p) return SWSE_E_BADHANDLE;
    if (!out || n <= 0) return SWSE_E_BADARG;
    lstrcpynA(out, p->path, n); return SWSE_OK;
}
static const SWSEModsAPI kMods = { sizeof(SWSEModsAPI), 1, M_ForEach, M_Find, M_Count, M_ModCount,
    M_ModAt, M_Dir, M_Dir, M_SelfDir, M_SelfPath };

// ---- game: there is none - everything reports "not ready", as at the main menu -----------
static SWSEStatus SWSE_CALL G_Level(SWSELevelInfo* o) {
    if (!o || o->size < sizeof(SWSELevelInfo)) return SWSE_E_BADARG;
    o->up = 0; o->epoch = 0; o->ageMs = 0; o->name[0] = 0; return SWSE_OK;
}
static int32_t SWSE_CALL G_LevelDue(uint32_t*, uint32_t) { return 0; }
static SWSEStatus SWSE_CALL G_Pos(float*) { return SWSE_E_NOTREADY; }
static SWSEStatus SWSE_CALL G_Yaw(float*) { return SWSE_E_NOTREADY; }
static SWSEStatus SWSE_CALL G_SetYaw(float) { return SWSE_E_NOTREADY; }
static SWSEStatus SWSE_CALL G_Two(float*, float*) { return SWSE_E_NOTREADY; }
static SWSEStatus SWSE_CALL G_PosGet(const char*, float*, float*) { return SWSE_E_NOTFOUND; }
static uint32_t   SWSE_CALL G_Hash(const char*) { return 0; }
static uint32_t   SWSE_CALL G_Res(uint32_t) { return 0; }
static int32_t    SWSE_CALL G_IsA(uint32_t, const char*) { return 0; }
static SWSEStatus SWSE_CALL G_Class(uint32_t, char* o, int32_t n) { if (o && n > 0) o[0] = 0; return SWSE_E_NOTFOUND; }
static int32_t    SWSE_CALL G_Focus(void) { return 0; }
static const SWSEGameAPI kGame = { sizeof(SWSEGameAPI), 1, G_Level, G_LevelDue, G_Pos, G_Yaw,
    G_SetYaw, G_Two, G_Two, G_PosGet, G_Hash, G_Res, G_IsA, G_Class, G_Focus };

// ---- memory: real, SEH on the host side ---------------------------------------------------
static SWSEStatus SWSE_CALL X_Read(uint32_t addr, void* out, uint32_t len) {
    if (!out) return SWSE_E_BADARG;
    __try { memcpy(out, (const void*)(uintptr_t)addr, len); return SWSE_OK; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return SWSE_E_FAULT; }
}
static SWSEStatus SWSE_CALL X_Write(uint32_t addr, const void* in, uint32_t len) {
    if (!in) return SWSE_E_BADARG;
    DWORD old = 0;
    BOOL prot = VirtualProtect((void*)(uintptr_t)addr, len, PAGE_EXECUTE_READWRITE, &old);
    SWSEStatus st = SWSE_OK;
    __try { memcpy((void*)(uintptr_t)addr, in, len); }
    __except (EXCEPTION_EXECUTE_HANDLER) { st = SWSE_E_FAULT; }
    if (prot) VirtualProtect((void*)(uintptr_t)addr, len, old, &old);
    return st;
}
static int32_t SWSE_CALL X_Readable(uint32_t addr, uint32_t len) {
    uintptr_t a = addr, end = (uintptr_t)addr + len;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void*)a, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
        a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    return 1;
}
static SWSEStatus SWSE_CALL X_GameInfo(SWSEGameInfo* o) {
    if (!o || o->size < sizeof(SWSEGameInfo)) return SWSE_E_BADARG;
    HMODULE exe = GetModuleHandleA(nullptr);
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((BYTE*)exe + ((IMAGE_DOS_HEADER*)exe)->e_lfanew);
    o->imageBase = (uint32_t)(uintptr_t)exe;
    o->imageSize = nt->OptionalHeader.SizeOfImage;
    o->timeDateStamp = nt->FileHeader.TimeDateStamp;
    GetModuleFileNameA(exe, o->exePath, sizeof(o->exePath));
    return SWSE_OK;
}
static const SWSEMemoryAPI kMemory = { sizeof(SWSEMemoryAPI), 1, X_Read, X_Write, X_Readable, X_GameInfo };

static const void* SWSE_CALL GetInterface(uint32_t id, uint32_t minVersion) {
    if (g_querying >= 0) return (id == SWSE_IFACE_LOG && minVersion <= kLog.version) ? &kLog : nullptr;
    const void* t = nullptr; uint32_t v = 0;
    switch (id) {
    case SWSE_IFACE_LOG:      t = &kLog;      v = kLog.version;      break;
    case SWSE_IFACE_CONSOLE:  t = &kConsole;  v = kConsole.version;  break;
    case SWSE_IFACE_FRAME:    t = &kFrame;    v = kFrame.version;    break;
    case SWSE_IFACE_EVENTS:   t = &kEvents;   v = kEvents.version;   break;
    case SWSE_IFACE_FEATURES: t = &kFeatures; v = kFeatures.version; break;
    case SWSE_IFACE_MODS:     t = &kMods;     v = kMods.version;     break;
    case SWSE_IFACE_GAME:     t = &kGame;     v = kGame.version;     break;
    case SWSE_IFACE_MEMORY:   t = &kMemory;   v = kMemory.version;   break;
    default: return nullptr;                                  // phase 2+: not served
    }
    return (minVersion <= v) ? t : nullptr;
}

// ---- lifecycle -----------------------------------------------------------------------------
static void Rollback(int idx) {
    for (int i = 0; i < g_cmdN; i++)   if (g_cmds[i].owner == idx)   g_cmds[i].dead = true;
    for (int i = 0; i < g_frameN; i++) if (g_frames[i].owner == idx) g_frames[i].dead = true;
    for (int i = 0; i < g_eventN; i++) if (g_events[i].owner == idx) g_events[i].dead = true;
}
static bool Refuse(Plugin* p, const char* why) {
    p->state = PS_REFUSED; lstrcpynA(p->why, why, sizeof(p->why)); return false;
}

static bool LoadPlugin(Plugin* p) {
    int idx = (int)(p - g_pl);
    char b[200];
    p->mod = LoadLibraryExA(p->path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!p->mod) { wsprintfA(b, "could not load the DLL (error %u; 193 = not a 32-bit DLL)", GetLastError()); return Refuse(p, b); }
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((BYTE*)p->mod + ((IMAGE_DOS_HEADER*)p->mod)->e_lfanew);
    p->base = (unsigned)(uintptr_t)p->mod; p->sizeImg = nt->OptionalHeader.SizeOfImage;
    SWSEPlugin_QueryFn q = (SWSEPlugin_QueryFn)GetProcAddress(p->mod, SWSE_PLUGIN_QUERY_NAME);
    SWSEPlugin_LoadFn  l = (SWSEPlugin_LoadFn) GetProcAddress(p->mod, SWSE_PLUGIN_LOAD_NAME);
    if (!q || !l) return Refuse(p, "not an SWSE plugin (no SWSEPlugin_Query / SWSEPlugin_Load export)");

    SWSEPluginInfo info; memset(&info, 0, sizeof(info)); info.size = sizeof(info);
    g_querying = idx;
    int r = CallQuery(p, q, &info);
    g_querying = -1;
    if (r < 0) { Faulted(p); return false; }
    if (!r) return Refuse(p, "its Query declined to load (see its log lines)");
    if (info.apiVersion != SWSE_PLUGIN_API_VERSION) {
        wsprintfA(b, "built for plugin API %u; this SWSE serves %u", info.apiVersion, SWSE_PLUGIN_API_VERSION);
        return Refuse(p, b);
    }
    if (!info.name || lstrcmpiA(info.name, p->name)) {
        wsprintfA(b, "declares the name '%s' but the file is %s.dll", info.name ? info.name : "", p->name);
        return Refuse(p, b);
    }
    if (info.minSwseVersion > MOCK_SWSE_VERSION) return Refuse(p, "needs a newer SWSE");
    lstrcpynA(p->desc, info.description ? info.description : "", sizeof(p->desc));
    p->version = info.version;

    r = CallLoad(p, l);
    if (r <= 0) Rollback(idx);                     // nothing it registered survives a failed Load
    if (r < 0) { Faulted(p); return false; }
    if (!r) return Refuse(p, "its Load returned 0 (see its log lines)");
    p->state = PS_LOADED;
    return true;
}

static void SwitchPlugin(Plugin* p, bool on) {
    char msg[300] = { 0 }, line[400];
    if (on) {
        if (p->on) { wsprintfA(line, "%s is already on", p->name); Out(line); return; }
        if (p->state == PS_FAULTED) { wsprintfA(line, "%s faulted this session and stays off until the game restarts", p->name); Out(line); return; }
        if (p->state == PS_REFUSED) { wsprintfA(line, "%s ON failed: %s", p->name, p->why); Out(line); return; }
        if (p->state == PS_OFF && !LoadPlugin(p)) {
            if (p->state == PS_REFUSED) { wsprintfA(line, "%s ON failed: %s", p->name, p->why); Out(line); }
            return;
        }
        p->on = true;                                // flag first, as SWSE's Cmd_features does
        if (p->onEnable) {
            int r = CallSwitch(p, p->onEnable, msg, sizeof(msg), "onEnable");
            if (r < 0) { Faulted(p); return; }
            if (!r) { p->on = false; wsprintfA(line, "%s ON refused by the plugin: %s", p->name, msg); Out(line); return; }
        }
        wsprintfA(line, "%s ON: %s", p->name, msg[0] ? msg : "on"); Out(line);
    } else {
        if (!p->on) { wsprintfA(line, "%s is already off", p->name); Out(line); return; }
        if (p->onDisable && CallSwitch(p, p->onDisable, msg, sizeof(msg), "onDisable") < 0) { Faulted(p); return; }
        p->on = false;
        wsprintfA(line, "%s off: %s", p->name, msg[0] ? msg : "off"); Out(line);
    }
}

static void ListPlugins() {
    char b[400];
    wsprintfA(b, "plugins: %d found", g_plN); Out(b);
    for (int i = 0; i < g_plN; i++) {
        Plugin* p = &g_pl[i];
        const char* st = p->on ? "on" : (p->state == PS_LOADED ? "off (loaded)" : kStateName[p->state]);
        char ver[24] = "-";
        if (p->state == PS_LOADED || p->state == PS_FAULTED)
            wsprintfA(ver, "%u.%u.%u", SWSE_VERSION_MAJOR(p->version), SWSE_VERSION_MINOR(p->version), SWSE_VERSION_PATCH(p->version));
        char cost[64] = "";
        if (p->state == PS_LOADED)
            wsprintfA(cost, "frame %d.%02d ms, worst %d.%02d", (int)p->msLast, (int)(p->msLast * 100) % 100,
                      (int)p->msWorst, (int)(p->msWorst * 100) % 100);
        wsprintfA(b, "  %-10s %-13s %-7s %s", p->name, st, ver, (p->state == PS_REFUSED || p->state == PS_FAULTED) ? p->why : cost);
        Out(b);
    }
}

static void Execute(const char* line) {
    char echo[300]; wsprintfA(echo, "> %s", line); Out(echo);
    char buf[256]; lstrcpynA(buf, line, sizeof(buf));
    char* argv[32]; int argc = 0; char* ctx = nullptr;
    for (char* t = strtok_s(buf, " \t", &ctx); t && argc < 32; t = strtok_s(nullptr, " \t", &ctx)) argv[argc++] = t;
    if (!argc) return;
    if (!lstrcmpiA(argv[0], "features")) {
        if (argc < 3) { Out("usage: features <name> on|off"); return; }
        for (int i = 0; i < g_plN; i++)
            if (!lstrcmpiA(g_pl[i].name, argv[1])) { SwitchPlugin(&g_pl[i], !lstrcmpiA(argv[2], "on")); return; }
        char m[200]; wsprintfA(m, "no feature called '%s' - `features` lists them", argv[1]); Out(m);
        return;
    }
    if (!lstrcmpiA(argv[0], "plugins")) { ListPlugins(); return; }
    if (!lstrcmpiA(argv[0], "echo"))    { Out(argc > 1 ? argv[1] : ""); return; }
    if (!lstrcmpiA(argv[0], "help")) {
        for (int i = 0; i < g_cmdN; i++) {
            if (g_cmds[i].dead || (argc > 1 && lstrcmpiA(argv[1], g_cmds[i].name))) continue;
            char m[120]; wsprintfA(m, "  %-15s (plugin %s)", g_cmds[i].name, g_pl[g_cmds[i].owner].name); Out(m);
        }
        return;
    }
    // Built-ins were checked above; now plugin commands. Several plugins may
    // offer one name: the provider LATEST in load order that is switched on
    // runs (load order = plugin index here), so the result never depends on
    // the order in which plugins were switched on.
    int run = -1, any = -1;
    for (int i = 0; i < g_cmdN; i++) {
        PCmd& c = g_cmds[i];
        if (c.dead || lstrcmpiA(c.name, argv[0])) continue;
        Plugin* p = &g_pl[c.owner];
        if (any < 0 || c.owner > g_cmds[any].owner) any = i;
        if ((p->on || (c.flags & SWSE_CMD_F_WHILE_OFF)) && (run < 0 || c.owner > g_cmds[run].owner)) run = i;
    }
    if (run >= 0) {
        PCmd& c = g_cmds[run];
        Plugin* p = &g_pl[c.owner];
        g_inPluginCall++;
        bool ok = CallCommand(p, c.fn, argc, (const char* const*)argv, c.user);
        g_inPluginCall--;
        if (!ok) Faulted(p);
        return;
    }
    if (any >= 0) {
        Plugin* p = &g_pl[g_cmds[any].owner];
        char m[240];
        if (p->state == PS_FAULTED)
            wsprintfA(m, "'%s' belongs to plugin %s, which faulted and is off until the game restarts", argv[0], p->name);
        else
            wsprintfA(m, "'%s' belongs to plugin %s, which is off - `features %s on`", argv[0], p->name, p->name);
        Out(m); return;
    }
    char m[200]; wsprintfA(m, "unknown: %s", argv[0]); Out(m);
}

// Dead entries are removed only when no call into a plugin is on the stack, so
// a callback that unregisters itself never has the array compacted under the
// loop that is calling it. SWSE does this at the top of each frame.
static void Compact() {
    if (g_inPluginCall) return;
    int w = 0;
    for (int r = 0; r < g_cmdN; r++)   if (!g_cmds[r].dead)   g_cmds[w++] = g_cmds[r];
    g_cmdN = w; w = 0;
    for (int r = 0; r < g_frameN; r++) if (!g_frames[r].dead) g_frames[w++] = g_frames[r];
    g_frameN = w; w = 0;
    for (int r = 0; r < g_eventN; r++) if (!g_events[r].dead) g_events[w++] = g_events[r];
    g_eventN = w;
}

static unsigned g_frameNo = 0, g_epoch = 0;
static void RunFrame() {
    Compact();
    SWSEFrameInfo fi; memset(&fi, 0, sizeof(fi));
    fi.size = sizeof(fi); fi.frame = ++g_frameNo; fi.tickMs = GetTickCount(); fi.dtMs = 16.7f;
    fi.width = 1920; fi.height = 1080; fi.levelEpoch = g_epoch;
    for (int i = 0; i < g_plN; i++) g_pl[i].msLast = 0;
    for (unsigned phase = SWSE_FRAME_TICK; phase <= SWSE_FRAME_OVERLAY; phase++) {
        fi.phase = phase;
        int n = g_frameN;                             // registered during the loop: next frame
        for (int i = 0; i < n; i++) {
            PFrame& f = g_frames[i];
            Plugin* p = &g_pl[f.owner];
            if (f.dead || f.phase != phase || !p->on) continue;
            double t0 = NowMs();
            g_inPluginCall++;
            bool ok = CallFrame(p, f.fn, &fi, f.user);
            g_inPluginCall--;
            p->msLast += NowMs() - t0;
            if (!ok) Faulted(p);
        }
    }
    for (int i = 0; i < g_plN; i++) if (g_pl[i].msLast > g_pl[i].msWorst) g_pl[i].msWorst = g_pl[i].msLast;
}

static void Raise(unsigned id, const void* data) {
    int n = g_eventN;
    for (int i = 0; i < n; i++) {
        PEvent& e = g_events[i];
        Plugin* p = &g_pl[e.owner];
        if (e.dead || e.id != id || !p->on) continue;
        g_inPluginCall++;
        bool ok = CallEvent(p, e.fn, id, data, e.user);
        g_inPluginCall--;
        if (!ok) Faulted(p);
    }
}

// T-P1 (PLUGIN_SYSTEM.md 9.1): what the guard costs. Every frame callback that
// is on is called n times through CallFrame (SEH frame + filter setup) and n
// times directly, and the difference per call is reported. The guard is the
// same shape SWSE uses (swse/plugins.cpp CallFrame).
static void Bench(int n) {
    SWSEFrameInfo fi; memset(&fi, 0, sizeof(fi));
    fi.size = sizeof(fi); fi.phase = SWSE_FRAME_TICK;
    for (int i = 0; i < g_frameN; i++) {
        PFrame& f = g_frames[i];
        Plugin* p = &g_pl[f.owner];
        if (f.dead || !p->on) continue;
        double t0 = NowMs();
        for (int k = 0; k < n; k++) if (!CallFrame(p, f.fn, &fi, f.user)) break;
        double t1 = NowMs();
        for (int k = 0; k < n; k++) f.fn(&fi, f.user);
        double t2 = NowMs();
        double guarded = (t1 - t0) * 1.0e6 / n, plain = (t2 - t1) * 1.0e6 / n;
        printf("  bench %s: %d calls - guarded %.1f ns/call, plain %.1f ns/call, guard %.1f ns/call\n",
               p->name, n, guarded, plain, guarded - plain);
    }
}

static void AddPlugin(const char* path) {
    if (g_plN >= MAX_PLUGINS) return;
    Plugin* p = &g_pl[g_plN];
    memset(p, 0, sizeof(*p));
    GetFullPathNameA(path, MAX_PATH, p->path, nullptr);
    const char* base = strrchr(p->path, '\\'); base = base ? base + 1 : p->path;
    lstrcpynA(p->name, base, sizeof(p->name));
    char* dot = strrchr(p->name, '.'); if (dot) *dot = 0;
    CharLowerA(p->name);
    p->iface.size = sizeof(SWSEInterface);
    p->iface.apiVersion = SWSE_PLUGIN_API_VERSION;
    p->iface.swseVersion = MOCK_SWSE_VERSION;
    p->iface.self = (0x5E5Eu << 8) | (unsigned)(g_plN + 1);
    p->iface.GetInterface = GetInterface;
    g_plN++;
}

int main(int argc, char** argv) {
    g_renderTid = GetCurrentThreadId();               // this thread plays the render thread
    int i = 1;
    for (; i < argc && lstrcmpA(argv[i], "--"); i++) AddPlugin(argv[i]);
    if (!g_plN) {
        printf("usage: mockhost <plugin.dll>... [-- <line>...]\n"
               "  a line is a console line, or: frame [n] | levelup | leveldown | modsreload | plugins | bench [n]\n");
        return 1;
    }
    printf("mockhost: %d plugin(s) found, all off - none loaded:", g_plN);
    for (int k = 0; k < g_plN; k++) printf(" %s=%s", g_pl[k].name, GetModuleHandleA(g_pl[k].path) ? "LOADED" : "unloaded");
    printf("\n");
    for (i++; i < argc; i++) {
        const char* l = argv[i];
        if (!_strnicmp(l, "frame", 5) && (!l[5] || l[5] == ' ')) {
            int n = l[5] ? atoi(l + 6) : 1;
            for (int k = 0; k < (n > 0 ? n : 1); k++) RunFrame();
            printf("  (ran %d frame%s)\n", n > 0 ? n : 1, n == 1 ? "" : "s");
        } else if (!lstrcmpiA(l, "levelup")) {
            SWSELevelEvent e = { sizeof(SWSELevelEvent), ++g_epoch, 0x10000000u + g_epoch, 0x20000000u + g_epoch };
            printf("  (level up, epoch %u)\n", g_epoch);
            Raise(SWSE_EV_LEVEL_UP, &e);
        } else if (!lstrcmpiA(l, "leveldown")) {
            SWSELevelEvent e = { sizeof(SWSELevelEvent), g_epoch, 0, 0 };
            printf("  (level down)\n");
            Raise(SWSE_EV_LEVEL_DOWN, &e);
        } else if (!lstrcmpiA(l, "modsreload")) {
            printf("  (mods reload)\n");
            Raise(SWSE_EV_MODS_RELOADED, nullptr);
        } else if (!_strnicmp(l, "bench", 5) && (!l[5] || l[5] == ' ')) {
            int n = l[5] ? atoi(l + 6) : 1000000;
            Bench(n > 0 ? n : 1000000);
        } else {
            Execute(l);
        }
    }
    printf("mockhost: finished, process alive\n");
    return 0;
}
