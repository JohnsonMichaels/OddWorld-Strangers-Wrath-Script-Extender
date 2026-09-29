// SWSE native plugins - see plugins.h. The design, with the reasoning behind
// every rule, is swse/research/PLUGIN_SYSTEM.md; what the fault guard can and
// cannot catch was measured in PLUGIN_QA.md.
//
// This file is the host side of sdk/swse_plugin_api.h and is compiled from the
// same header (SWSE_HOST), so the layout plugins see is the layout SWSE serves.
// The guard, attribution and command rules were proven first in the stand-in
// host, sdk/tools/mockhost/mockhost.cpp, and lifted from there. In order:
//   the plugin table, handles, threads, timing, the log
//   fault guards - every call into a plugin goes through one Call* function
//   the registries (commands, frame callbacks, events, bind listeners,
//     reports) and their dispatch
//   the API tables, one section per table (Console.Post's queue with its
//     table), and GetInterface
//   discovery, the loader, the switch handlers
//   per-frame entry points, and the GL notifications from glspy's hooks
//   console integration: plugin commands, reports, `plugins`, `query plugins`
//
// For whoever edits this next:
//   * No C++ objects near a __try: SEH and destructors do not mix under /EHsc
//     (C2712), so every guarded frame is a plain function with plain locals.
//   * Formatting is bounded (_snprintf_s, _TRUNCATE). Plugin strings are any
//     length, and wsprintfA assumes 1024 bytes of room whatever the buffer.
//   * The registries are fixed static arrays. Removing an entry marks it dead;
//     dead entries are dropped at the top of the next frame, when no plugin
//     call is on the stack. Every dispatch loop runs to the count it read at
//     the start. So a callback may register or unregister anything, itself
//     included: a removal takes effect at once, an addition from the next
//     dispatch, and no loop ever has its array moved under it (QA D7).

#include <windows.h>
#include <gl/GL.h>
#include <bcrypt.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define SWSE_HOST                   // the host side: no export prototypes
#define SWSE_PLUGIN_NO_HELPERS      // the helpers are for plugins
#include "swse_plugin_api.h"        // sdk\ (build.bat passes /I ..\sdk)

#include "plugins.h"
#include "swse_version.h"
#include "features.h"
#include "console.h"
#include "modregistry.h"
#include "levelwatch.h"
#include "scriptvm.h"
#include "positions.h"
#include "prefsedit.h"
#include "input.h"
#include "glspy.h"
#include "hookreg.h"
#include "gamebuild.h"      // safe mode: the Game calls that reach the game, and Memory.Write

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "version.lib")

#define SWSE_HOST_VERSION SWSE_MAKE_VERSION(SWSE_VER_MAJOR, SWSE_VER_MINOR, SWSE_VER_PATCH, 0)

#define MAX_PLUGINS    SWSE_MAX_PLUGIN_FEATS  // one switch each
#define MAX_CMDS       256
#define MAX_FRAMECBS   128                    // at most 64 per phase
#define MAX_PER_PHASE  64
#define MAX_EVENTS     128
#define MAX_BINDS      32                     // glBindTexture listeners
#define MAX_REPORTS    64
#define MAX_POSTS      64                     // Console.Post queue
#define MAX_SHADOWS    32
#define HANDLE_TAG     0x53570000u            // 'SW' in the high word, index + 1 in the low

// ==========================================================================
//  The plugin table
// ==========================================================================
enum PState { PS_FOUND = 0, PS_LOADED, PS_FAULTED, PS_REFUSED };

struct Plugin {
    char     name[32];            // the switch: file name without .dll, lower case
    char     path[MAX_PATH];      // the DLL
    char     mod[96];             // its mod folder's name...
    char     modDir[MAX_PATH];    // ...and path
    int      feat;                // feature id; -1 when refused before it became a switch
    int      order;               // rank in load order at the last scan (later = higher)
    bool     present;             // found by the last scan
    bool     seen;                // scratch, while a scan runs
    PState   state;
    HMODULE  h;                   // set once LoadLibrary succeeded, and never cleared
    unsigned base, imgSize;
    SWSEInterface iface;          // handed to Query and Load; valid for the session
    // what Query said
    unsigned version, minSwse, apiVersion, flags;
    char     desc[128], author[64], aliases[96];
    // what its version resource says, read without running it
    char     vDesc[128], vVer[24];
    // Features.SetHandlers
    SWSESwitchFn onEnable, onDisable;
    void*    swUser;
    // why it is REFUSED or FAULTED, and the raw fault the filter recorded
    char     why[256];
    DWORD    faultCode;
    unsigned faultAddr, faultThrow, faultRet;
    const char* faultWhere;
    // provenance: the file's SHA-256 at load (PLUGIN_QA.md D8)
    char     sha[65];
    // cost in ms: this frame so far, the last frame, a running average, the worst
    double   msNow, msLast, msAvg, msWorst;
    unsigned framesTimed;
    // evidence of work for the self-test: calls into it since it was switched
    // on (bind listener calls counted apart - thousands a frame), and binds
    // seen this frame and the last
    unsigned calls, binds, bindsNow, bindsLast;
    bool     faultNotice;         // a FEATURE (off) event owed for its fault
};

static Plugin   g_pl[MAX_PLUGINS];
static int      g_plN = 0;
static int      g_loadedN = 0;        // plugins whose DLL is loaded
static bool     g_inited = false;
static DWORD    g_renderTid = 0;
static int      g_querying = -1;      // the plugin inside its Query: only Log is served
static int      g_dispatch = 0;       // calls into plugins on the stack right now
static unsigned g_frameNo = 0;

// Two enabled mods shipping one name: the later mod's is used, the other is
// listed (rebuilt by every scan).
struct Shadow { char name[32]; char mod[96]; char by[96]; bool loadedKept; };
static Shadow g_shadow[MAX_SHADOWS];
static int    g_shadowN = 0;
static int    g_scanMods = 0;         // enabled mods with a plugins\ folder

static void Fmt(char* out, int len, const char* fmt, ...) {
    if (!out || len <= 0) return;
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(out, len, _TRUNCATE, fmt, ap);
    va_end(ap);
}

static bool OnRender() { return g_renderTid && GetCurrentThreadId() == g_renderTid; }

static double NowMs() {
    static double s_toMs = 0;
    LARGE_INTEGER c;
    if (s_toMs == 0) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); s_toMs = 1000.0 / (double)f.QuadPart; }
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * s_toMs;
}

// A plugin's handle is valid once its DLL is loaded (it is first handed out to
// Query), whatever has happened since. Guards against mistakes, not malice.
static Plugin* FromHandle(SWSEPluginHandle h) {
    if ((h & 0xFFFF0000u) != HANDLE_TAG) return nullptr;
    unsigned i = h & 0xFFFFu;
    if (i < 1 || i > (unsigned)g_plN) return nullptr;
    Plugin* p = &g_pl[i - 1];
    return p->h ? p : nullptr;
}

static Plugin* ByFeat(int feat) {
    if (feat < 0) return nullptr;
    for (int i = 0; i < g_plN; i++) if (g_pl[i].feat == feat) return &g_pl[i];
    return nullptr;
}

static Plugin* ByName(const char* name) {
    if (!name) return nullptr;
    for (int i = 0; i < g_plN; i++) if (!lstrcmpiA(g_pl[i].name, name)) return &g_pl[i];
    return nullptr;
}

// Dispatch goes only to plugins that are loaded, not faulted, and switched on.
static bool IsOn(const Plugin* p) {
    return p->state == PS_LOADED && p->feat >= 0 && SWSE_Feature((SwseFeature)p->feat);
}

// ---- the log ---------------------------------------------------------------------
// One line per call, opened and closed each time so the last line before a
// crash survives, under a lock because Log.Write is [any thread].
static CRITICAL_SECTION g_logLock;
static struct LogLockInit { LogLockInit() { InitializeCriticalSection(&g_logLock); } } s_logLockInit;

static void LogLine(const char* s) {
    char dir[MAX_PATH], full[MAX_PATH];
    DWORD n = GetModuleFileNameA(GetModuleHandleA(NULL), dir, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    char* sl = strrchr(dir, '\\');
    if (!sl) return;
    *sl = 0;
    _snprintf_s(full, MAX_PATH, _TRUNCATE, "%s\\swse_log.txt", dir);
    char line[1100];
    int len = _snprintf_s(line, sizeof(line), _TRUNCATE, "%s\r\n", s);
    if (len < 0) { len = (int)sizeof(line) - 1; line[len - 2] = '\r'; line[len - 1] = '\n'; }
    EnterCriticalSection(&g_logLock);
    HANDLE f = CreateFileA(full, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(f, line, (DWORD)len, &w, NULL);     // one write: lines never interleave
        CloseHandle(f);
    }
    LeaveCriticalSection(&g_logLock);
}

// The console and the log. Render thread (the console buffer has no lock).
static void Say(const char* line) {
    if (OnRender()) SWSE_ConsolePrint(line);
    LogLine(line);
}

// ==========================================================================
//  Fault guards
//
//  Every call SWSE makes into a plugin goes through one of the Call*
//  functions. The filter runs in the first pass of exception dispatch, the
//  one moment the faulting stack can still be read, and records only raw
//  facts - it may be running on the last page of an overflowed stack, so the
//  text is built after the unwind (Faulted). Three cases are told apart
//  (PLUGIN_SYSTEM.md 6.1): a fault in the plugin's own code; a C++ exception
//  escaping (named by the image its ThrowInfo lives in); and a fault inside
//  something the plugin called, named by the plugin's own call on the stack.
// ==========================================================================
static bool InImage(const Plugin* p, unsigned a) {
    return p->base && a >= p->base && a < p->base + p->imgSize;
}

static void ModuleOf(unsigned addr, char* out, int len, unsigned* baseOut) {
    HMODULE m = nullptr;
    char path[MAX_PATH];
    lstrcpynA(out, "?", len);
    if (baseOut) *baseOut = 0;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)addr, &m) && m) {
        if (baseOut) *baseOut = (unsigned)(uintptr_t)m;
        if (GetModuleFileNameA(m, path, MAX_PATH)) {
            const char* b = strrchr(path, '\\');
            lstrcpynA(out, b ? b + 1 : path, len);
        }
    }
}

// The innermost stack word that points into the plugin AND follows a CALL
// instruction: the plugin's call into whatever faulted. Words without a real
// CALL before them are skipped - stale stack words look like return
// addresses, and unvalidated walks invent frames in frame-pointer-omitted
// code (the project's own lesson). Guarded: this runs inside a filter.
static unsigned PluginReturnOnStack(const Plugin* p, const CONTEXT* c) {
    __try {
        const NT_TIB* tib = (const NT_TIB*)NtCurrentTeb();
        unsigned sp = c->Esp, top = (unsigned)(uintptr_t)tib->StackBase;
        for (unsigned a = sp; a + 4 <= top && a < sp + 256 * 1024; a += 4) {
            unsigned v = *(const unsigned*)(uintptr_t)a;
            if (!InImage(p, v) || v < p->base + 8) continue;
            const BYTE* r = (const BYTE*)(uintptr_t)v;
            if (r[-5] == 0xE8) return v;                                              // call rel32
            if (r[-6] == 0xFF && (r[-5] == 0x15 || (r[-5] & 0xF8) == 0x90)) return v; // call [abs] / [reg+d32]
            if (r[-3] == 0xFF && (r[-2] & 0xF8) == 0x50) return v;                    // call [reg+d8]
            if (r[-2] == 0xFF && ((r[-1] & 0xF8) == 0xD0 || (r[-1] & 0xF8) == 0x10)) return v;  // call reg / [reg]
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}

static int FaultFilter(Plugin* p, const char* where, EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    p->faultCode  = er->ExceptionCode;
    p->faultAddr  = (unsigned)(uintptr_t)er->ExceptionAddress;
    p->faultWhere = where;
    p->faultThrow = (er->ExceptionCode == 0xE06D7363 && er->NumberParameters >= 3)
                    ? (unsigned)er->ExceptionInformation[2] : 0;     // the ThrowInfo
    p->faultRet   = (!p->faultThrow && !InImage(p, p->faultAddr))
                    ? PluginReturnOnStack(p, ep->ContextRecord) : 0;
    return EXCEPTION_EXECUTE_HANDLER;
}

static int CallQuery(Plugin* p, SWSEPlugin_QueryFn fn, SWSEPluginInfo* info) {
    __try { return fn(&p->iface, info) ? 1 : 0; }
    __except (FaultFilter(p, "Query", GetExceptionInformation())) { return -1; }
}
static int CallLoad(Plugin* p, SWSEPlugin_LoadFn fn) {
    __try { return fn(&p->iface) ? 1 : 0; }
    __except (FaultFilter(p, "Load", GetExceptionInformation())) { return -1; }
}
static int CallSwitch(Plugin* p, SWSESwitchFn fn, char* msg, int len, const char* where) {
    __try { return fn(msg, len, p->swUser) ? 1 : 0; }
    __except (FaultFilter(p, where, GetExceptionInformation())) { return -1; }
}
static bool CallCommand(Plugin* p, SWSECommandFn fn, int argc, const char* const* argv, void* u) {
    __try { fn(argc, argv, u); return true; }
    __except (FaultFilter(p, "a command", GetExceptionInformation())) { return false; }
}
static bool CallFrame(Plugin* p, SWSEFrameFn fn, const SWSEFrameInfo* fi, void* u, const char* where) {
    __try { fn(fi, u); return true; }
    __except (FaultFilter(p, where, GetExceptionInformation())) { return false; }
}
static bool CallEvent(Plugin* p, SWSEEventFn fn, unsigned id, const void* d, void* u, const char* where) {
    __try { fn(id, d, u); return true; }
    __except (FaultFilter(p, where, GetExceptionInformation())) { return false; }
}
static bool CallBind(Plugin* p, SWSEBindTextureFn fn, unsigned target, unsigned tex, void* u) {
    __try { fn(target, tex, u); return true; }
    __except (FaultFilter(p, "a bind listener", GetExceptionInformation())) { return false; }
}
static bool CallReport(Plugin* p, SWSEReportFn fn, unsigned kind, const void* sink, void* u, const char* where) {
    __try { fn(kind, sink, u); return true; }
    __except (FaultFilter(p, where, GetExceptionInformation())) { return false; }
}

// Plugin memory SWSE reads (strings in descriptors, Query's answers) is read
// under a guard too, so a bad pointer is a refusal rather than a fault in
// SWSE's own frame.
static bool SafeCopyStr(char* dst, const char* src, int len) {
    if (!dst || len <= 0) return false;
    dst[0] = 0;
    if (!src) return true;
    __try {
        int i = 0;
        for (; i < len - 1 && src[i]; i++) dst[i] = src[i];
        dst[i] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { dst[0] = 0; return false; }
}
static bool GuardedCopy(void* dst, const void* src, unsigned len) {
    __try { memcpy(dst, src, len); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void BuildWhy(Plugin* p) {
    const char* where = p->faultWhere ? p->faultWhere : "a call";
    char mod[64];
    unsigned mb = 0;
    if (p->faultCode == 0xE06D7363 && p->faultThrow) {
        ModuleOf(p->faultThrow, mod, sizeof(mod), nullptr);
        Fmt(p->why, sizeof(p->why), "a C++ exception escaped %s (thrown by %s)", where, mod);
    } else if (InImage(p, p->faultAddr)) {
        Fmt(p->why, sizeof(p->why), "exception %08X in %s at %s.dll+0x%X",
            (unsigned)p->faultCode, where, p->name, p->faultAddr - p->base);
    } else {
        ModuleOf(p->faultAddr, mod, sizeof(mod), &mb);
        if (p->faultRet)
            Fmt(p->why, sizeof(p->why), "exception %08X in %s at %s+0x%X, called from %s.dll+0x%X",
                (unsigned)p->faultCode, where, mod, p->faultAddr - mb, p->name, p->faultRet - p->base);
        else
            Fmt(p->why, sizeof(p->why), "exception %08X in %s at %08X (%s)",
                (unsigned)p->faultCode, where, p->faultAddr, mod);
    }
}

static void RecountGl();

// A fault switches the plugin off for the session (PLUGIN_SYSTEM.md 6.3): all
// dispatch stops, `features <name> on` is refused until the game restarts
// (its state may be half-updated), and features.txt keeps its line, so a
// fixed build loads normally next launch. Called after the guard has unwound -
// possibly deep inside glBindTexture, so the FEATURE event it owes the other
// plugins waits for the next frame.
static void Faulted(Plugin* p) {
    if (p->faultCode == EXCEPTION_STACK_OVERFLOW) _resetstkoflw();   // re-arm the guard page
    BuildWhy(p);
    bool wasOn = p->feat >= 0 && SWSE_Feature((SwseFeature)p->feat);
    p->state = PS_FAULTED;
    if (p->feat >= 0) SWSE_FeatureSetFlag((SwseFeature)p->feat, false);   // not saved
    p->faultNotice = wasOn;
    RecountGl();
    char b[400];
    Fmt(b, sizeof(b), "plugin '%s' FAULTED: %s - switched off for this session", p->name, p->why);
    Say(b);
    Fmt(b, sizeof(b), "  plugin '%s' is %s from mod '%s'", p->name, p->path, p->mod);
    LogLine(b);
}

// ==========================================================================
//  Registries
// ==========================================================================
struct PCmd {
    char name[32], cat[32], help[160];
    SWSECommandFn fn; void* user; int owner; unsigned flags; bool dead;
};
struct PFrame  { unsigned phase; SWSEFrameFn fn; void* user; int owner; bool dead; };
struct PEvent  { unsigned id; SWSEEventFn fn; void* user; int owner; bool dead; };
struct PBind   { SWSEBindTextureFn fn; void* user; int owner; bool dead; };
struct PReport { unsigned kind; SWSEReportFn fn; void* user; int owner; bool dead; };

static PCmd    g_cmd[MAX_CMDS];       static int g_cmdN = 0;
static PFrame  g_frame[MAX_FRAMECBS]; static int g_frameN = 0;
static PEvent  g_ev[MAX_EVENTS];      static int g_evN = 0;
static PBind   g_bind[MAX_BINDS];     static int g_bindN = 0;
static PReport g_rep[MAX_REPORTS];    static int g_repN = 0;

// Deliveries from inside a GL hook on the stack (bind listeners, the upload
// event): there, Execute and (un)registering answer SWSE_E_BUSY.
static int g_inGl = 0;
volatile int g_swsePluginBindListeners = 0;
volatile int g_swsePluginUploadListeners = 0;

static int OwnerOf(const Plugin* p) { return (int)(p - g_pl); }

// The GL hooks test these counts on every bind and upload, so they count only
// listeners whose plugin is on. Dispatch still checks each plugin, so a count
// that is briefly high costs a few tests and is never wrong.
static void RecountGl() {
    int b = 0, u = 0;
    for (int i = 0; i < g_bindN; i++)
        if (!g_bind[i].dead && IsOn(&g_pl[g_bind[i].owner])) b++;
    for (int i = 0; i < g_evN; i++)
        if (!g_ev[i].dead && g_ev[i].id == SWSE_EV_TEXTURE_UPLOAD && IsOn(&g_pl[g_ev[i].owner])) u++;
    g_swsePluginBindListeners = b;
    g_swsePluginUploadListeners = u;
}

// Load returned 0 or faulted: nothing it registered may survive (QA D6). Keyed
// on the owner, so it covers every registry.
static void Rollback(Plugin* p) {
    int o = OwnerOf(p);
    for (int i = 0; i < g_cmdN; i++)   if (g_cmd[i].owner == o)   g_cmd[i].dead = true;
    for (int i = 0; i < g_frameN; i++) if (g_frame[i].owner == o) g_frame[i].dead = true;
    for (int i = 0; i < g_evN; i++)    if (g_ev[i].owner == o)    g_ev[i].dead = true;
    for (int i = 0; i < g_bindN; i++)  if (g_bind[i].owner == o)  g_bind[i].dead = true;
    for (int i = 0; i < g_repN; i++)   if (g_rep[i].owner == o)   g_rep[i].dead = true;
    p->onEnable = p->onDisable = nullptr;
    p->swUser = nullptr;
    RecountGl();
}

// Only with no plugin call on the stack (the top of a frame).
static void Compact() {
    if (g_dispatch) return;
    int w = 0;
    for (int r = 0; r < g_cmdN; r++)   if (!g_cmd[r].dead)   g_cmd[w++] = g_cmd[r];
    g_cmdN = w; w = 0;
    for (int r = 0; r < g_frameN; r++) if (!g_frame[r].dead) g_frame[w++] = g_frame[r];
    g_frameN = w; w = 0;
    for (int r = 0; r < g_evN; r++)    if (!g_ev[r].dead)    g_ev[w++] = g_ev[r];
    g_evN = w; w = 0;
    for (int r = 0; r < g_bindN; r++)  if (!g_bind[r].dead)  g_bind[w++] = g_bind[r];
    g_bindN = w; w = 0;
    for (int r = 0; r < g_repN; r++)   if (!g_rep[r].dead)   g_rep[w++] = g_rep[r];
    g_repN = w;
}

// Raise an event to every subscriber that is on. Loops to the count read at
// the start: a subscription made during delivery waits for the next event.
static void Raise(unsigned id, const void* data, const char* where) {
    int n = g_evN;
    for (int i = 0; i < n; i++) {
        PEvent& e = g_ev[i];
        if (e.dead || e.id != id) continue;
        Plugin* p = &g_pl[e.owner];
        if (!IsOn(p)) continue;
        SWSEEventFn fn = e.fn; void* u = e.user;
        double t0 = NowMs();
        g_dispatch++;
        bool ok = CallEvent(p, fn, id, data, u, where);
        g_dispatch--;
        p->msNow += NowMs() - t0;
        p->calls++;
        if (!ok) Faulted(p);
    }
}

// ==========================================================================
//  The API tables
// ==========================================================================

// ---- Log (1) --------------------------------------------------------------------
static void SWSE_CALL L_Write(SWSEPluginHandle self, int32_t level, const char* text) {
    Plugin* p = FromHandle(self);
    char body[1024];
    if (!SafeCopyStr(body, text, sizeof(body))) lstrcpynA(body, "(unreadable text)", sizeof(body));
    const char* tag = level == SWSE_LOG_ERROR ? "ERROR: " : level == SWSE_LOG_WARN ? "WARN: " : "";
    // One log line per line of text, each tagged, so grep finds every one.
    char* s = body;
    for (;;) {
        char* nl = strchr(s, '\n');
        if (nl) { *nl = 0; if (nl > s && nl[-1] == '\r') nl[-1] = 0; }
        char line[1100];
        Fmt(line, sizeof(line), "[%s] %s%s", p ? p->name : "?", tag, s);
        LogLine(line);
        if (!nl) break;
        s = nl + 1;
        if (!*s) break;
    }
}
static const SWSELogAPI kLog = { sizeof(SWSELogAPI), 1, L_Write };

// ---- Console (2) -----------------------------------------------------------------
static bool ValidCmdName(const char* n) {
    int len = n ? lstrlenA(n) : 0;
    if (len < 1 || len > 31) return false;
    for (int i = 0; i < len; i++) {
        char c = n[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '-' || c == '.';
        if (!ok) return false;
    }
    return true;
}

static void SWSE_CALL C_Print(SWSEPluginHandle self, const char* text) {
    if (!OnRender() || !FromHandle(self)) return;
    char body[2048];
    if (!SafeCopyStr(body, text, sizeof(body))) return;
    char line[240];
    for (const char* s = body; *s; ) {                  // one line per '\n', cut at 239
        int n = 0;
        while (s[n] && s[n] != '\n') n++;
        int k = n < 239 ? n : 239;
        if (k && s[k - 1] == '\r') k--;
        memcpy(line, s, k);
        line[k] = 0;
        SWSE_ConsolePrint(line);
        s += n;
        if (*s == '\n') s++;
    }
}

static SWSEStatus SWSE_CALL C_Register(SWSEPluginHandle self, const SWSECommandDesc* d) {
    if (!OnRender()) return SWSE_E_THREAD;
    Plugin* p = FromHandle(self);
    if (!p) return SWSE_E_BADHANDLE;
    if (p->state == PS_FAULTED) return SWSE_E_OFF;
    if (g_inGl) return SWSE_E_BUSY;
    SWSECommandDesc dc;
    memset(&dc, 0, sizeof(dc));
    if (!d || !GuardedCopy(&dc, d, sizeof(uint32_t))) return SWSE_E_BADARG;
    if (dc.size < sizeof(SWSECommandDesc) || !GuardedCopy(&dc, d, sizeof(SWSECommandDesc))) return SWSE_E_BADARG;
    PCmd c;
    memset(&c, 0, sizeof(c));
    char nm[64];                      // room past 31, so a long name is refused, not cut
    if (!dc.fn || !SafeCopyStr(nm, dc.name, sizeof(nm)) || !ValidCmdName(nm)) return SWSE_E_BADARG;
    lstrcpynA(c.name, nm, sizeof(c.name));
    if (!SafeCopyStr(c.cat, dc.category, sizeof(c.cat)) || !SafeCopyStr(c.help, dc.help, sizeof(c.help)))
        return SWSE_E_BADARG;
    if (!c.cat[0]) lstrcpynA(c.cat, p->name, sizeof(c.cat));
    // Built-ins are never replaced: tools rely on SWSE's commands meaning one thing.
    if (SWSE_ConsoleIsBuiltin(c.name)) return SWSE_E_TAKEN;
    int o = OwnerOf(p);
    for (int i = 0; i < g_cmdN; i++)
        if (!g_cmd[i].dead && g_cmd[i].owner == o && !lstrcmpiA(g_cmd[i].name, c.name)) return SWSE_E_TAKEN;
    if (g_cmdN >= MAX_CMDS) return SWSE_E_FULL;
    // Another plugin may offer the name too: both stand, and the one later in
    // load order runs (the load_order.txt rule), whatever the switch-on order.
    for (int i = 0; i < g_cmdN; i++) {
        if (g_cmd[i].dead || g_cmd[i].owner == o || lstrcmpiA(g_cmd[i].name, c.name)) continue;
        char b[240];
        Fmt(b, sizeof(b), "plugins: command '%s' is offered by '%s' and '%s' - the one later in load order runs",
            c.name, g_pl[g_cmd[i].owner].name, p->name);
        LogLine(b);
        break;
    }
    c.fn = dc.fn; c.user = dc.user; c.flags = dc.flags; c.owner = o; c.dead = false;
    g_cmd[g_cmdN++] = c;
    return SWSE_OK;
}

static SWSEStatus SWSE_CALL C_Unregister(SWSEPluginHandle self, const char* name) {
    if (!OnRender()) return SWSE_E_THREAD;
    Plugin* p = FromHandle(self);
    if (!p) return SWSE_E_BADHANDLE;
    if (g_inGl) return SWSE_E_BUSY;
    char n[40];
    if (!SafeCopyStr(n, name, sizeof(n)) || !n[0]) return SWSE_E_BADARG;
    int o = OwnerOf(p);
    for (int i = 0; i < g_cmdN; i++)
        if (!g_cmd[i].dead && g_cmd[i].owner == o && !lstrcmpiA(g_cmd[i].name, n)) {
            g_cmd[i].dead = true;
            return SWSE_OK;
        }
    return SWSE_E_NOTFOUND;
}

// SWSE's Execute counts nesting only for scripts and aliases; a command that
// runs a command is counted here (PLUGIN_SYSTEM.md C5), so a plugin command
// that runs itself stops at 8 levels with SWSE_E_NESTING.
static SWSEStatus SWSE_CALL C_Execute(SWSEPluginHandle self, const char* line) {
    if (!OnRender()) return SWSE_E_THREAD;
    Plugin* p = FromHandle(self);
    if (!p) return SWSE_E_BADHANDLE;
    if (p->state == PS_FAULTED) return SWSE_E_OFF;
    if (g_inGl) return SWSE_E_BUSY;           // not from inside glBindTexture
    char buf[240];
    if (!SafeCopyStr(buf, line, sizeof(buf)) || !buf[0]) return SWSE_E_BADARG;
    return SWSE_ConsoleExecNested(buf) == 0 ? SWSE_OK : SWSE_E_NESTING;
}

static int32_t SWSE_CALL C_IsOpen(void) { return SWSE_ConsoleOpen() ? 1 : 0; }

// Post (version 2) [any thread]: the way back from a worker thread. A small
// queue under a lock, drained at the top of the next frame in order; SWSE
// copies it out before running anything, so it never calls into a plugin
// while holding the lock. A line runs only if its plugin is still on then.
struct Posted { int owner; char line[240]; };
static Posted g_post[MAX_POSTS];
static int    g_postN = 0;
static CRITICAL_SECTION g_postLock;
static struct PostLockInit { PostLockInit() { InitializeCriticalSection(&g_postLock); } } s_postLockInit;

static SWSEStatus SWSE_CALL C_Post(SWSEPluginHandle self, const char* line) {
    Plugin* p = FromHandle(self);
    if (!p) return SWSE_E_BADHANDLE;
    if (p->state == PS_FAULTED) return SWSE_E_OFF;
    char buf[240];
    if (!SafeCopyStr(buf, line, sizeof(buf)) || !buf[0]) return SWSE_E_BADARG;
    SWSEStatus st = SWSE_OK;
    EnterCriticalSection(&g_postLock);
    if (g_postN >= MAX_POSTS) {
        st = SWSE_E_FULL;
    } else {
        g_post[g_postN].owner = OwnerOf(p);
        lstrcpynA(g_post[g_postN].line, buf, sizeof(g_post[g_postN].line));
        g_postN++;
    }
    LeaveCriticalSection(&g_postLock);
    return st;
}

static void DrainPosts() {
    static Posted local[MAX_POSTS];            // render thread only
    EnterCriticalSection(&g_postLock);
    int n = g_postN;
    if (n) memcpy(local, g_post, sizeof(Posted) * n);
    g_postN = 0;
    LeaveCriticalSection(&g_postLock);
    for (int i = 0; i < n; i++)
        if (IsOn(&g_pl[local[i].owner])) SWSE_ConsoleExecNested(local[i].line);
}

static const SWSEConsoleAPI kConsole = { sizeof(SWSEConsoleAPI), 2,
    C_Print, C_Register, C_Unregister, C_Execute, C_IsOpen, C_Post };

// ---- Frame (3) --------------------------------------------------------------------
static SWSEStatus SWSE_CALL F_Register(SWSEPluginHandle self, uint32_t phase, SWSEFrameFn fn, void* user) {
    if (!OnRender()) return SWSE_E_THREAD;
    Plugin* p = FromHandle(self);
    if (!p) return SWSE_E_BADHANDLE;
    if (p->state == PS_FAULTED) return SWSE_E_OFF;
    if (g_inGl) return SWSE_E_BUSY;
    if (!fn || phase > SWSE_FRAME_OVERLAY) return SWSE_E_BADARG;
    int inPhase = 0;
    for (int i = 0; i < g_frameN; i++) if (!g_frame[i].dead && g_frame[i].phase == phase) inPhase++;
    if (inPhase >= MAX_PER_PHASE || g_frameN >= MAX_FRAMECBS) return SWSE_E_FULL;
    PFrame f = { phase, fn, user, OwnerOf(p), false };
    g_frame[g_frameN++] = f;
    return SWSE_OK;
}
static SWSEStatus SWSE_CALL F_Unregister(SWSEPluginHandle self, uint32_t phase, SWSEFrameFn fn, void* user) {
    if (!OnRender()) return SWSE_E_THREAD;
    Plugin* p = FromHandle(self);
    if (!p) return SWSE_E_BADHANDLE;
    if (g_inGl) return SWSE_E_BUSY;
    int o = OwnerOf(p);
    for (int i = 0; i < g_frameN; i++) {
        PFrame& f = g_frame[i];
        if (!f.dead && f.owner == o && f.phase == phase && f.fn == fn && f.user == user) {
            f.dead = true;
            return SWSE_OK;
        }
    }
    return SWSE_E_NOTFOUND;
}
static int32_t SWSE_CALL F_IsRender(void) { return OnRender() ? 1 : 0; }
static const SWSEFrameAPI kFrame = { sizeof(SWSEFrameAPI), 1, F_Register, F_Unregister, F_IsRender };

// ---- Events (4) -------------------------------------------------------------------
static bool EventServed(uint32_t id) {
    return id == SWSE_EV_LEVEL_UP || id == SWSE_EV_LEVEL_DOWN || id == SWSE_EV_MODS_RELOADED ||
           id == SWSE_EV_FEATURE || id == SWSE_EV_TEXTURE_UPLOAD;
}
static SWSEStatus SWSE_CALL E_Subscribe(SWSEPluginHandle self, uint32_t id, SWSEEventFn fn, void* user) {
    if (!OnRender()) return SWSE_E_THREAD;
    Plugin* p = FromHandle(self);
    if (!p) return SWSE_E_BADHANDLE;
    if (p->state == PS_FAULTED) return SWSE_E_OFF;
    if (g_inGl) return SWSE_E_BUSY;
    if (!fn) return SWSE_E_BADARG;
    if (!EventServed(id)) return SWSE_E_UNSUPPORTED;     // never a silent no-op
    if (g_evN >= MAX_EVENTS) return SWSE_E_FULL;
    // Uploads are seen by glspy's glCompressedTexImage2D hook - SWSE's own,
    // the one HD textures and foliage use - so subscribing installs it.
    if (id == SWSE_EV_TEXTURE_UPLOAD) SWSE_InstallGLSpy();
    PEvent e = { id, fn, user, OwnerOf(p), false };
    g_ev[g_evN++] = e;
    RecountGl();
    return SWSE_OK;
}
static SWSEStatus SWSE_CALL E_Unsubscribe(SWSEPluginHandle self, uint32_t id, SWSEEventFn fn, void* user) {
    if (!OnRender()) return SWSE_E_THREAD;
    Plugin* p = FromHandle(self);
    if (!p) return SWSE_E_BADHANDLE;
    if (g_inGl) return SWSE_E_BUSY;
    int o = OwnerOf(p);
    for (int i = 0; i < g_evN; i++) {
        PEvent& e = g_ev[i];
        if (!e.dead && e.owner == o && e.id == id && e.fn == fn && e.user == user) {
            e.dead = true;
            RecountGl();
            return SWSE_OK;
        }
    }
    return SWSE_E_NOTFOUND;
}
static const SWSEEventsAPI kEvents = { sizeof(SWSEEventsAPI), 1, E_Subscribe, E_Unsubscribe };

// ---- Features (5) -----------------------------------------------------------------
static SWSEStatus SWSE_CALL W_SetHandlers(SWSEPluginHandle self, SWSESwitchFn on, SWSESwitchFn off, void* user) {
    if (!OnRender()) return SWSE_E_THREAD;
    Plugin* p = FromHandle(self);
    if (!p) return SWSE_E_BADHANDLE;
    if (p->state == PS_FAULTED) return SWSE_E_OFF;
    if (g_inGl) return SWSE_E_BUSY;
    p->onEnable = on; p->onDisable = off; p->swUser = user;
    return SWSE_OK;
}
static int32_t SWSE_CALL W_IsOn(const char* name) {
    if (!OnRender()) return SWSE_E_THREAD;
    char n[40];
    if (!SafeCopyStr(n, name, sizeof(n)) || !n[0]) return -1;
    int f = SWSE_FeatureFind(n);
    if (f < 0) return -1;
    return SWSE_Feature((SwseFeature)f) ? 1 : 0;
}
static const SWSEFeaturesAPI kFeatures = { sizeof(SWSEFeaturesAPI), 1, W_SetHandlers, W_IsOn };

// ---- Mods (6) -----------------------------------------------------------------------
// A callback that faults unwinds to the call from SWSE that is on the stack
// (the command or frame callback that asked), which blames its plugin.
struct ModCb { SWSEModFileFn fn; void* ctx; };
static void ModAdapter(const char* abs, const char* mod, void* c) {
    ModCb* m = (ModCb*)c;
    m->fn(abs, mod, m->ctx);
}
static void SWSE_CALL M_ForEach(const char* relative, SWSEModFileFn fn, void* ctx) {
    if (!OnRender() || !fn) return;
    char rel[MAX_PATH];
    if (!SafeCopyStr(rel, relative, sizeof(rel)) || !rel[0]) return;
    ModCb cb = { fn, ctx };
    SWSE_ForEachModFile(rel, ModAdapter, &cb);
}
static int32_t SWSE_CALL M_Find(const char* relative, char* out, int32_t outLen) {
    if (!OnRender()) return SWSE_E_THREAD;
    if (!out || outLen <= 0) return SWSE_E_BADARG;
    char rel[MAX_PATH], found[MAX_PATH];
    out[0] = 0;
    if (!SafeCopyStr(rel, relative, sizeof(rel)) || !rel[0]) return SWSE_E_BADARG;
    if (!SWSE_FindModFile(rel, found, sizeof(found))) return 0;
    lstrcpynA(out, found, outLen);
    return 1;
}
static int32_t SWSE_CALL M_Count(const char* relative) {
    if (!OnRender()) return SWSE_E_THREAD;
    char rel[MAX_PATH];
    if (!SafeCopyStr(rel, relative, sizeof(rel)) || !rel[0]) return SWSE_E_BADARG;
    return SWSE_CountModFile(rel);
}
static int32_t SWSE_CALL M_ModCount(void) {
    if (!OnRender()) return SWSE_E_THREAD;
    return SWSE_ModCount();
}
static SWSEStatus SWSE_CALL M_ModAt(int32_t index, char* name, int32_t nameLen, char* path, int32_t pathLen) {
    if (!OnRender()) return SWSE_E_THREAD;
    if (index < 0 || index >= SWSE_ModCount()) return SWSE_E_NOTFOUND;
    if (name && nameLen > 0) lstrcpynA(name, SWSE_ModName(index), nameLen);
    if (path && pathLen > 0) lstrcpynA(path, SWSE_ModPath(index), pathLen);
    return SWSE_OK;
}
static SWSEStatus SWSE_CALL M_Root(char* out, int32_t outLen) {
    if (!OnRender()) return SWSE_E_THREAD;
    if (!out || outLen <= 0) return SWSE_E_BADARG;
    SWSE_ModsInit();
    lstrcpynA(out, SWSE_ModsRoot(), outLen);
    return SWSE_OK;
}
static SWSEStatus SWSE_CALL M_BinDir(char* out, int32_t outLen) {
    if (!OnRender()) return SWSE_E_THREAD;
    if (!out || outLen <= 0) return SWSE_E_BADARG;
    char exe[MAX_PATH];
    GetModuleFileNameA(GetModuleHandleA(NULL), exe, MAX_PATH);
    char* sl = strrchr(exe, '\\');
    if (sl) *sl = 0;
    lstrcpynA(out, exe, outLen);
    return SWSE_OK;
}
static SWSEStatus SWSE_CALL M_SelfDir(SWSEPluginHandle self, char* out, int32_t outLen) {   // [any thread]
    Plugin* p = FromHandle(self);
    if (!p) return SWSE_E_BADHANDLE;
    if (!out || outLen <= 0) return SWSE_E_BADARG;
    lstrcpynA(out, p->modDir, outLen);
    return SWSE_OK;
}
static SWSEStatus SWSE_CALL M_SelfPath(SWSEPluginHandle self, char* out, int32_t outLen) {  // [any thread]
    Plugin* p = FromHandle(self);
    if (!p) return SWSE_E_BADHANDLE;
    if (!out || outLen <= 0) return SWSE_E_BADARG;
    lstrcpynA(out, p->path, outLen);
    return SWSE_OK;
}
static const SWSEModsAPI kMods = { sizeof(SWSEModsAPI), 1, M_ForEach, M_Find, M_Count, M_ModCount,
    M_ModAt, M_Root, M_BinDir, M_SelfDir, M_SelfPath };

// ---- Game (7) -----------------------------------------------------------------------
// Cheap, render-thread, already-verified wrappers only; anything expensive
// stays behind a console command, where its cost is visible and attributed.
// In safe mode (an unknown game build, gamebuild.h) the calls that reach the
// player call the game at Steam addresses: they answer SWSE_E_UNSUPPORTED,
// not SWSE_E_NOTREADY, so a plugin does not wait for a level that never comes.
// HashPath and ResourceLookup answer 0 (scriptvm.cpp, prefsedit.cpp).
static bool SafeMode() { return SWSE_GameBuildSafeMode() != 0; }
static SWSEStatus SWSE_CALL G_Level(SWSELevelInfo* o) {
    if (!OnRender()) return SWSE_E_THREAD;
    if (!o || o->size < sizeof(SWSELevelInfo)) return SWSE_E_BADARG;
    o->up = SWSE_LevelUp() ? 1 : 0;
    o->epoch = SWSE_LevelEpoch();
    o->ageMs = SWSE_LevelAgeMs();
    lstrcpynA(o->name, SWSE_CurrentLevel(), sizeof(o->name));
    return SWSE_OK;
}
static int32_t SWSE_CALL G_LevelDue(uint32_t* lastEpoch, uint32_t settleMs) {
    if (!OnRender() || !lastEpoch) return 0;
    return SWSE_LevelDue(lastEpoch, settleMs) ? 1 : 0;
}
static SWSEStatus SWSE_CALL G_Pos(float xyz[3]) {
    if (!OnRender()) return SWSE_E_THREAD;
    if (!xyz) return SWSE_E_BADARG;
    if (SafeMode()) return SWSE_E_UNSUPPORTED;
    float p[3];
    if (SWSE_PosGet(p) != 1) return SWSE_E_NOTREADY;
    xyz[0] = p[0]; xyz[1] = p[1]; xyz[2] = p[2];
    return SWSE_OK;
}
static SWSEStatus SWSE_CALL G_Yaw(float* deg) {
    if (!OnRender()) return SWSE_E_THREAD;
    if (!deg) return SWSE_E_BADARG;
    if (SafeMode()) return SWSE_E_UNSUPPORTED;
    float y;
    if (SWSE_PlayerYawGet(&y) != 1) return SWSE_E_NOTREADY;
    *deg = y;
    return SWSE_OK;
}
static SWSEStatus SWSE_CALL G_SetYaw(float deg) {
    if (!OnRender()) return SWSE_E_THREAD;
    if (!(deg > -1.0e7f && deg < 1.0e7f)) return SWSE_E_BADARG;      // also refuses NaN
    if (SafeMode()) return SWSE_E_UNSUPPORTED;
    return SWSE_PlayerYawSet(deg) == 1 ? SWSE_OK : SWSE_E_NOTREADY;
}
static SWSEStatus SWSE_CALL G_Health(float* cur, float* mx) {
    if (!OnRender()) return SWSE_E_THREAD;
    if (SafeMode()) return SWSE_E_UNSUPPORTED;
    float c = 0, m = 0, b = 0;
    if (SWSE_PlayerHealth(&c, &m, &b) != 1) return SWSE_E_NOTREADY;
    if (cur) *cur = c;
    if (mx) *mx = m;
    return SWSE_OK;
}
static SWSEStatus SWSE_CALL G_Stamina(float* cur, float* mx) {
    if (!OnRender()) return SWSE_E_THREAD;
    if (SafeMode()) return SWSE_E_UNSUPPORTED;
    float c = 0, m = 0, b = 0;
    if (SWSE_PlayerStamina(&c, &m, &b) != 1) return SWSE_E_NOTREADY;
    if (cur) *cur = c;
    if (mx) *mx = m;
    return SWSE_OK;
}
static SWSEStatus SWSE_CALL G_PosGet(const char* label, float xyz[3], float* yawDeg) {
    if (!OnRender()) return SWSE_E_THREAD;
    char l[64];
    if (!xyz || !SafeCopyStr(l, label, sizeof(l)) || !l[0]) return SWSE_E_BADARG;
    float p[3];
    if (!SWSE_PositionGet(l, p, nullptr)) return SWSE_E_NOTFOUND;
    xyz[0] = p[0]; xyz[1] = p[1]; xyz[2] = p[2];
    float y;
    if (yawDeg && SWSE_PositionYaw(l, &y)) *yawDeg = y;     // none saved: left untouched
    return SWSE_OK;
}
static uint32_t SWSE_CALL G_Hash(const char* path) {
    if (!OnRender()) return 0;
    char pth[260];
    if (!SafeCopyStr(pth, path, sizeof(pth)) || !pth[0]) return 0;
    return SWSE_HashPath(pth);              // the game's own hasher: render thread
}
static uint32_t SWSE_CALL G_Res(uint32_t hash) {
    if (!OnRender()) return 0;
    return SWSE_ResourceLookup(hash);
}
static int32_t SWSE_CALL G_IsA(uint32_t obj, const char* cls) {
    if (!OnRender()) return 0;
    char c[128];
    if (!SafeCopyStr(c, cls, sizeof(c)) || !c[0]) return 0;
    return SWSE_ObjIsA(obj, c) ? 1 : 0;
}
static SWSEStatus SWSE_CALL G_Class(uint32_t obj, char* out, int32_t outLen) {
    if (!OnRender()) return SWSE_E_THREAD;
    if (!out || outLen <= 0) return SWSE_E_BADARG;
    if (SWSE_RttiName(obj, out, outLen)) return SWSE_OK;
    out[0] = 0;
    return SWSE_E_NOTFOUND;
}
static int32_t SWSE_CALL G_Focus(void) {
    if (!OnRender()) return SWSE_E_THREAD;
    return SWSE_InputReallyFocused() ? 1 : 0;
}
static const SWSEGameAPI kGame = { sizeof(SWSEGameAPI), 1, G_Level, G_LevelDue, G_Pos, G_Yaw,
    G_SetYaw, G_Health, G_Stamina, G_PosGet, G_Hash, G_Res, G_IsA, G_Class, G_Focus };

// ---- Memory (8) ---------------------------------------------------------------------
// The guard is on SWSE's side, so compilers without __try (MinGW, Rust, Zig)
// get fault-safe access too.
static bool ProtReadable(DWORD pr) {
    if (pr & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    pr &= 0xFF;
    return pr == PAGE_READONLY || pr == PAGE_READWRITE || pr == PAGE_WRITECOPY ||
           pr == PAGE_EXECUTE_READ || pr == PAGE_EXECUTE_READWRITE || pr == PAGE_EXECUTE_WRITECOPY;
}
static bool ProtWritable(DWORD pr) {
    if (pr & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    pr &= 0xFF;
    return pr == PAGE_READWRITE || pr == PAGE_WRITECOPY ||
           pr == PAGE_EXECUTE_READWRITE || pr == PAGE_EXECUTE_WRITECOPY;
}
// Every page of [a, end) committed and passing `ok`.
static bool RangeIs(uintptr_t a, uintptr_t end, bool (*ok)(DWORD)) {
    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void*)a, &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT || !ok(mbi.Protect)) return false;
        uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (next <= a) return false;
        a = next;
    }
    return true;
}

static SWSEStatus SWSE_CALL X_Read(uint32_t addr, void* out, uint32_t len) {        // [any thread]
    if (!out) return SWSE_E_BADARG;
    if (!len) return SWSE_OK;
    if ((uintptr_t)addr + len < (uintptr_t)addr) return SWSE_E_BADARG;
    return GuardedCopy(out, (const void*)(uintptr_t)addr, len) ? SWSE_OK : SWSE_E_FAULT;
}

// From any thread only an aligned store of 1, 2 or 4 bytes to memory that is
// already writable: x86 cannot tear it, which is what aitune.cpp's worker
// relies on. Anything bigger, or anything that must lift page protection
// (process-wide while it lasts), is render-thread only (PLUGIN_QA.md D3).
// Protection is lifted and restored region by region, so a range spanning
// pages with different protections gets each one back as it was.
static SWSEStatus SWSE_CALL X_Write(uint32_t addr, const void* in, uint32_t len) {
    if (!in) return SWSE_E_BADARG;
    if (!len) return SWSE_OK;
    // Safe mode: the addresses a plugin writes were measured on some build,
    // and on this one SWSE cannot tell. Reads stay (they cannot hurt the game).
    if (SafeMode()) return SWSE_E_UNSUPPORTED;
    uintptr_t a = addr, end = (uintptr_t)addr + len;
    if (end < a) return SWSE_E_BADARG;
    bool atomic = (len == 1 || len == 2 || len == 4) && (addr % len) == 0;
    bool writable = RangeIs(a, end, ProtWritable);
    if (!OnRender() && !(atomic && writable)) return SWSE_E_THREAD;
    // Game data only: bytes SWSE (or anyone registered) has patched are not
    // a plugin's to overwrite - `hooks` lists them.
    if (SWSE_HookOwner(addr, len, nullptr, 0)) return SWSE_E_TAKEN;
    if (writable)
        return GuardedCopy((void*)a, in, len) ? SWSE_OK : SWSE_E_FAULT;
    const BYTE* src = (const BYTE*)in;
    bool code = false;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void*)a, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            return SWSE_E_FAULT;                  // never lift a guard or a no-access page
        uintptr_t rEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        uintptr_t n = (rEnd < end ? rEnd : end) - a;
        if ((mbi.Protect & 0xF0) != 0) code = true;          // PAGE_EXECUTE_*
        bool ok;
        if (ProtWritable(mbi.Protect)) {
            ok = GuardedCopy((void*)a, src, (unsigned)n);
        } else {
            DWORD old = 0, tmp = 0;
            if (!VirtualProtect((void*)a, n, PAGE_EXECUTE_READWRITE, &old)) return SWSE_E_FAULT;
            ok = GuardedCopy((void*)a, src, (unsigned)n);
            VirtualProtect((void*)a, n, old, &tmp);
        }
        if (!ok) return SWSE_E_FAULT;
        a += n; src += n;
    }
    if (code) FlushInstructionCache(GetCurrentProcess(), (void*)(uintptr_t)addr, len);
    return SWSE_OK;
}

static int32_t SWSE_CALL X_Readable(uint32_t addr, uint32_t len) {                  // [any thread]
    if (!len) return 1;
    uintptr_t a = addr, end = (uintptr_t)addr + len;
    if (end < a) return 0;
    return RangeIs(a, end, ProtReadable) ? 1 : 0;
}

static SWSEStatus SWSE_CALL X_GameInfo(SWSEGameInfo* o) {                          // [any thread]
    if (!o || o->size < sizeof(SWSEGameInfo)) return SWSE_E_BADARG;
    HMODULE exe = GetModuleHandleA(NULL);
    const IMAGE_NT_HEADERS* nt =
        (const IMAGE_NT_HEADERS*)((const BYTE*)exe + ((const IMAGE_DOS_HEADER*)exe)->e_lfanew);
    o->imageBase = (uint32_t)(uintptr_t)exe;
    o->imageSize = nt->OptionalHeader.SizeOfImage;
    o->timeDateStamp = nt->FileHeader.TimeDateStamp;
    GetModuleFileNameA(exe, o->exePath, sizeof(o->exePath));
    o->exePath[sizeof(o->exePath) - 1] = 0;
    return SWSE_OK;
}
static const SWSEMemoryAPI kMemory = { sizeof(SWSEMemoryAPI), 1, X_Read, X_Write, X_Readable, X_GameInfo };

// ---- GL (9) ---------------------------------------------------------------------------
// SWSE owns the GL hooks and fans them out; a plugin never patches GL. The
// bind listener rides the core glBindTexture hook (glspy.cpp) - the one foliage
// uses - installed by the first listener if foliage has not put it in.
static void* SWSE_CALL GL_GetProc(const char* name) {
    if (!OnRender()) return nullptr;
    char n[128];
    if (!SafeCopyStr(n, name, sizeof(n)) || !n[0]) return nullptr;
    HMODULE gl = GetModuleHandleA("opengl32.dll");
    if (!gl) return nullptr;
    typedef PROC (WINAPI* Gpa)(LPCSTR);
    Gpa gpa = (Gpa)GetProcAddress(gl, "wglGetProcAddress");
    void* f = gpa ? (void*)gpa(n) : nullptr;
    // Some drivers answer 1, 2, 3 or -1 instead of NULL for "not mine".
    if ((uintptr_t)f <= 3 || (intptr_t)f == -1) f = nullptr;
    if (!f) f = (void*)GetProcAddress(gl, n);              // opengl32's own 1.1 exports
    return f;
}
static SWSEStatus SWSE_CALL GL_AddBind(SWSEPluginHandle self, SWSEBindTextureFn fn, void* user) {
    if (!OnRender()) return SWSE_E_THREAD;
    Plugin* p = FromHandle(self);
    if (!p) return SWSE_E_BADHANDLE;
    if (p->state == PS_FAULTED) return SWSE_E_OFF;
    if (g_inGl) return SWSE_E_BUSY;
    if (!fn) return SWSE_E_BADARG;
    if (g_bindN >= MAX_BINDS) return SWSE_E_FULL;
    char m[200], who[48];
    Fmt(who, sizeof(who), "plugin %s", p->name);
    if (SWSE_BindHookInstall(who, m, sizeof(m)) <= 0) {
        char b[300];
        Fmt(b, sizeof(b), "plugins: '%s' asked for bind notifications, but the hook could not go in: %s", p->name, m);
        LogLine(b);
        return SWSE_E_UNSUPPORTED;
    }
    PBind e = { fn, user, OwnerOf(p), false };
    g_bind[g_bindN++] = e;
    RecountGl();
    return SWSE_OK;
}
static SWSEStatus SWSE_CALL GL_RemoveBind(SWSEPluginHandle self, SWSEBindTextureFn fn, void* user) {
    if (!OnRender()) return SWSE_E_THREAD;
    Plugin* p = FromHandle(self);
    if (!p) return SWSE_E_BADHANDLE;
    if (g_inGl) return SWSE_E_BUSY;
    int o = OwnerOf(p);
    for (int i = 0; i < g_bindN; i++) {
        PBind& e = g_bind[i];
        if (!e.dead && e.owner == o && e.fn == fn && e.user == user) {
            e.dead = true;
            RecountGl();
            return SWSE_OK;
        }
    }
    return SWSE_E_NOTFOUND;
}
static uint32_t SWSE_CALL GL_Bound2D(void) { return OnRender() ? SWSE_BoundTexture2D() : 0; }
static const SWSEGLAPI kGL = { sizeof(SWSEGLAPI), 1, GL_GetProc, GL_AddBind, GL_RemoveBind, GL_Bound2D };

// ---- Reports (10) ---------------------------------------------------------------------
// A plugin's lines in `selftest`, `perf` and `status`, after SWSE's own. The
// sink handed to its callback is only valid during that callback.
#define SINK_TAG 0x534E4B31u                  // 'SNK1'
struct ReportSink {
    unsigned tag, kind;
    void (*emit)(const char* line);                                 // perf, status
    void (*check)(int ok, const char* name, const char* detail);    // selftest
};

static void SWSE_CALL R_Line(const void* sink, const char* text) {
    if (!OnRender()) return;
    const ReportSink* s = (const ReportSink*)sink;
    if (!s || s->tag != SINK_TAG) return;
    char t[240];
    if (!SafeCopyStr(t, text, sizeof(t))) return;
    if (s->emit) s->emit(t);
    else if (s->check) SWSE_ConsolePrint(t);             // a plain line inside the self-test
}
// SWSE_CHECK_PASS 1 / FAIL 0 / WARN -1: the self-test's own convention.
static void SWSE_CALL R_Check(const void* sink, int32_t result, const char* name, const char* detail) {
    if (!OnRender()) return;
    const ReportSink* s = (const ReportSink*)sink;
    if (!s || s->tag != SINK_TAG) return;
    char n[32], d[200];
    if (!SafeCopyStr(n, name, sizeof(n)) || !SafeCopyStr(d, detail, sizeof(d))) return;
    int ok = result > 0 ? 1 : result < 0 ? -1 : 0;
    if (s->check) { s->check(ok, n, d); return; }
    char l[260];                                         // outside the self-test: shown, not counted
    Fmt(l, sizeof(l), "[%s] %-14s %s", ok > 0 ? "PASS" : ok < 0 ? "WARN" : "FAIL", n, d);
    if (s->emit) s->emit(l);
}
static SWSEStatus SWSE_CALL R_Register(SWSEPluginHandle self, uint32_t kind, SWSEReportFn fn, void* user) {
    if (!OnRender()) return SWSE_E_THREAD;
    Plugin* p = FromHandle(self);
    if (!p) return SWSE_E_BADHANDLE;
    if (p->state == PS_FAULTED) return SWSE_E_OFF;
    if (g_inGl) return SWSE_E_BUSY;
    if (!fn) return SWSE_E_BADARG;
    if (kind != SWSE_REPORT_SELFTEST && kind != SWSE_REPORT_PERF && kind != SWSE_REPORT_STATUS)
        return SWSE_E_UNSUPPORTED;
    if (g_repN >= MAX_REPORTS) return SWSE_E_FULL;
    PReport r = { kind, fn, user, OwnerOf(p), false };
    g_rep[g_repN++] = r;
    return SWSE_OK;
}
static const SWSEReportsAPI kReports = { sizeof(SWSEReportsAPI), 1, R_Register, R_Line, R_Check };

// Run one plugin's reports of one kind into a sink. False if it faulted.
static bool RunReports(Plugin* p, unsigned kind, const ReportSink* sink, int* ran) {
    static const char* kWhere[] = { "", "its self-test report", "its perf report", "its status report" };
    int o = OwnerOf(p), n = g_repN;
    if (ran) *ran = 0;
    for (int i = 0; i < n; i++) {
        PReport& r = g_rep[i];
        if (r.dead || r.owner != o || r.kind != kind) continue;
        if (ran) (*ran)++;
        SWSEReportFn fn = r.fn; void* u = r.user;
        double t0 = NowMs();
        g_dispatch++;
        bool ok = CallReport(p, fn, kind, sink, u, kWhere[kind <= 3 ? kind : 0]);
        g_dispatch--;
        p->msNow += NowMs() - t0;
        p->calls++;
        if (!ok) { Faulted(p); return false; }
    }
    return true;
}

// ---- GetInterface ------------------------------------------------------------------
// [any thread]. Tables are static, so plugins may cache the pointers. During
// a Query on the render thread only the Log is served: Query describes, it
// does not register.
static const void* SWSE_CALL GetInterface(uint32_t id, uint32_t minVersion) {
    if (g_querying >= 0 && OnRender())
        return (id == SWSE_IFACE_LOG && minVersion <= kLog.version) ? &kLog : nullptr;
    const void* t = nullptr;
    uint32_t v = 0;
    switch (id) {
    case SWSE_IFACE_LOG:      t = &kLog;      v = kLog.version;      break;
    case SWSE_IFACE_CONSOLE:  t = &kConsole;  v = kConsole.version;  break;
    case SWSE_IFACE_FRAME:    t = &kFrame;    v = kFrame.version;    break;
    case SWSE_IFACE_EVENTS:   t = &kEvents;   v = kEvents.version;   break;
    case SWSE_IFACE_FEATURES: t = &kFeatures; v = kFeatures.version; break;
    case SWSE_IFACE_MODS:     t = &kMods;     v = kMods.version;     break;
    case SWSE_IFACE_GAME:     t = &kGame;     v = kGame.version;     break;
    case SWSE_IFACE_MEMORY:   t = &kMemory;   v = kMemory.version;   break;
    case SWSE_IFACE_GL:       t = &kGL;       v = kGL.version;       break;
    case SWSE_IFACE_REPORTS:  t = &kReports;  v = kReports.version;  break;
    default: return nullptr;                   // prefs, script, hooks: not served yet
    }
    return (minVersion <= v) ? t : nullptr;
}

// ==========================================================================
//  Discovery
// ==========================================================================
static bool ValidPluginName(const char* n) {
    int len = n ? lstrlenA(n) : 0;
    if (len < 1 || len > 31) return false;
    for (int i = 0; i < len; i++) {
        char c = n[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}

// A plugin's version resource, read without running any of its code
// (GetFileVersionInfo maps the file as data): `features` and `plugins` can
// say what a switched-off plugin is before the user decides to load it.
static void ReadVersionInfo(Plugin* p) {
    p->vDesc[0] = p->vVer[0] = 0;
    DWORD dummy = 0;
    DWORD n = GetFileVersionInfoSizeA(p->path, &dummy);
    if (!n || n > 256 * 1024) return;
    void* buf = malloc(n);
    if (!buf) return;
    if (GetFileVersionInfoA(p->path, 0, n, buf)) {
        struct LangCp { WORD lang, cp; }* tr = nullptr;
        UINT len = 0;
        char key[80];
        if (VerQueryValueA(buf, "\\VarFileInfo\\Translation", (void**)&tr, &len) && tr && len >= sizeof(LangCp))
            Fmt(key, sizeof(key), "\\StringFileInfo\\%04x%04x\\FileDescription", tr->lang, tr->cp);
        else
            Fmt(key, sizeof(key), "\\StringFileInfo\\040904b0\\FileDescription");
        char* s = nullptr;
        UINT sl = 0;
        if (VerQueryValueA(buf, key, (void**)&s, &sl) && s && sl) lstrcpynA(p->vDesc, s, sizeof(p->vDesc));
        VS_FIXEDFILEINFO* ffi = nullptr;
        UINT fl = 0;
        if (VerQueryValueA(buf, "\\", (void**)&ffi, &fl) && ffi && fl >= sizeof(VS_FIXEDFILEINFO))
            Fmt(p->vVer, sizeof(p->vVer), "%u.%u.%u", HIWORD(ffi->dwFileVersionMS),
                LOWORD(ffi->dwFileVersionMS), HIWORD(ffi->dwFileVersionLS));
    }
    free(buf);
}

static void AddShadow(const char* name, const char* mod, const char* by, bool loadedKept) {
    if (g_shadowN >= MAX_SHADOWS) return;
    Shadow& s = g_shadow[g_shadowN++];
    lstrcpynA(s.name, name, sizeof(s.name));
    lstrcpynA(s.mod, mod, sizeof(s.mod));
    lstrcpynA(s.by, by, sizeof(s.by));
    s.loadedKept = loadedKept;
}

// One DLL found in one enabled mod, in load order.
static void AddFound(const char* mod, const char* modDir, const char* path, const char* file, int pos) {
    char name[MAX_PATH];
    lstrcpynA(name, file, sizeof(name));
    int len = lstrlenA(name);
    if (len <= 4 || lstrcmpiA(name + len - 4, ".dll")) return;   // *.dll also matches 8.3 names
    name[len - 4] = 0;
    CharLowerA(name);
    char b[600];
    if (!ValidPluginName(name)) {
        Fmt(b, sizeof(b), "plugins: skipping %s - '%s' is not a valid switch name "
                          "(a-z 0-9 _ -, up to 31 characters)", path, name);
        LogLine(b);
        return;
    }
    Plugin* p = ByName(name);
    if (p && p->seen) {
        // An earlier mod in this scan ships the same name: the later mod wins
        // (the SWSE_FindModFile rule) - unless the earlier one is loaded,
        // which cannot be undone until the game restarts.
        if (!p->h) {
            AddShadow(name, p->mod, mod, false);
            lstrcpynA(p->path, path, sizeof(p->path));
            lstrcpynA(p->mod, mod, sizeof(p->mod));
            lstrcpynA(p->modDir, modDir, sizeof(p->modDir));
            p->order = pos;
            ReadVersionInfo(p);
        } else {
            AddShadow(name, mod, p->mod, true);
        }
        return;
    }
    if (p) {                                  // known from an earlier scan
        p->seen = true;
        if (!p->h) {
            lstrcpynA(p->path, path, sizeof(p->path));
            lstrcpynA(p->mod, mod, sizeof(p->mod));
            lstrcpynA(p->modDir, modDir, sizeof(p->modDir));
            ReadVersionInfo(p);
        } else if (lstrcmpiA(p->path, path)) {
            AddShadow(name, mod, p->mod, true);
        }
        p->order = pos;
        return;
    }
    if (g_plN >= MAX_PLUGINS) {
        Fmt(b, sizeof(b), "plugins: skipping %s - SWSE lists at most %d plugins", path, MAX_PLUGINS);
        LogLine(b);
        return;
    }
    p = &g_pl[g_plN];
    memset(p, 0, sizeof(*p));
    lstrcpynA(p->name, name, sizeof(p->name));
    lstrcpynA(p->path, path, sizeof(p->path));
    lstrcpynA(p->mod, mod, sizeof(p->mod));
    lstrcpynA(p->modDir, modDir, sizeof(p->modDir));
    p->order = pos;
    p->seen = true;
    p->iface.size = sizeof(SWSEInterface);
    p->iface.apiVersion = SWSE_PLUGIN_API_VERSION;
    p->iface.swseVersion = SWSE_HOST_VERSION;
    p->iface.self = HANDLE_TAG | (unsigned)(g_plN + 1);
    p->iface.GetInterface = GetInterface;
    ReadVersionInfo(p);
    g_plN++;
    if (SWSE_FeatureNameReserved(name)) {
        // The switch would be ambiguous in features.txt and `features x on`.
        p->state = PS_REFUSED;
        p->feat = -1;
        Fmt(p->why, sizeof(p->why), "'%s' is the name of a built-in switch or nickname - rename the DLL", name);
        Fmt(b, sizeof(b), "plugin '%s' REFUSED: %s (%s)", name, p->why, path);
        Say(b);
        return;
    }
    p->feat = SWSE_FeatureAddPlugin(name);
    if (p->feat < 0) {
        p->state = PS_REFUSED;
        Fmt(p->why, sizeof(p->why), "no room for another plugin switch (%d)", SWSE_MAX_PLUGIN_FEATS);
        Fmt(b, sizeof(b), "plugin '%s' REFUSED: %s (%s)", name, p->why, path);
        Say(b);
    }
}

struct ScanCtx { int pos; };

// One enabled mod's plugins\ folder: its DLLs by name, so the order within a
// mod is predictable.
static void ScanPluginsDir(const char* dir, const char* modName, void* ctx) {
    ScanCtx* sc = (ScanCtx*)ctx;
    DWORD attr = GetFileAttributesA(dir);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return;
    g_scanMods++;
    char modDir[MAX_PATH];
    lstrcpynA(modDir, dir, sizeof(modDir));
    char* sl = strrchr(modDir, '\\');
    if (sl) *sl = 0;
    static char files[MAX_PLUGINS][MAX_PATH];   // render thread only
    int n = 0;
    char glob[MAX_PATH];
    Fmt(glob, sizeof(glob), "%s\\*.dll", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(glob, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (n < MAX_PLUGINS) lstrcpynA(files[n++], fd.cFileName, MAX_PATH);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    for (int i = 1; i < n; i++)                      // insertion sort, case-insensitive
        for (int j = i; j > 0 && lstrcmpiA(files[j - 1], files[j]) > 0; j--) {
            char t[MAX_PATH];
            lstrcpynA(t, files[j], MAX_PATH);
            lstrcpynA(files[j], files[j - 1], MAX_PATH);
            lstrcpynA(files[j - 1], t, MAX_PATH);
        }
    for (int i = 0; i < n; i++) {
        char path[MAX_PATH];
        Fmt(path, sizeof(path), "%s\\%s", dir, files[i]);
        AddFound(modName, modDir, path, files[i], sc->pos++);
    }
}

static void Scan() {
    for (int i = 0; i < g_plN; i++) g_pl[i].seen = false;
    g_shadowN = 0;
    g_scanMods = 0;
    ScanCtx sc = { 0 };
    SWSE_ForEachModFile("plugins", ScanPluginsDir, &sc);
    for (int i = 0; i < g_plN; i++) g_pl[i].present = g_pl[i].seen;
}

static void LogUnclaimed(const char* key, bool on, void*) {
    if (!on) return;
    char b[200];
    Fmt(b, sizeof(b), "plugins: features.txt switches on '%s', but no plugin by that name is installed", key);
    LogLine(b);
}

static void LogScan(const char* what) {
    int shipped = 0;
    for (int i = 0; i < g_plN; i++) if (g_pl[i].present) shipped++;
    char b[400];
    Fmt(b, sizeof(b), "plugins: %s - %d found in %d mod(s)", what, shipped, g_scanMods);
    LogLine(b);
    for (int i = 0; i < g_plN; i++) {
        const Plugin* p = &g_pl[i];
        if (!p->present) continue;
        Fmt(b, sizeof(b), "  plugin %-12s %s  [%s]  %s", p->name,
            p->state == PS_REFUSED ? "REFUSED" : p->feat >= 0 && SWSE_FeatureSaved((SwseFeature)p->feat)
                                                  ? "on in features.txt" : "off", p->mod, p->path);
        LogLine(b);
    }
    for (int i = 0; i < g_shadowN; i++) {
        Fmt(b, sizeof(b), g_shadow[i].loadedKept
                ? "  plugin %s also in '%s' - not used: the one from '%s' is loaded (restart to change)"
                : "  plugin %s also in '%s' - shadowed by '%s' (later in load order)",
            g_shadow[i].name, g_shadow[i].mod, g_shadow[i].by);
        LogLine(b);
    }
}

void SWSE_PluginsInit() {
    if (g_inited) return;
    g_inited = true;
    g_renderTid = GetCurrentThreadId();       // before any plugin can load
    SWSE_FeaturesInit();
    Scan();
    LogScan("at launch");
    SWSE_FeaturesForEachUnclaimed(LogUnclaimed, nullptr);
    // Plugins load in safe mode too (gamebuild.h): the API reaches the game
    // only through SWSE, and those calls are gated here, so a plugin keeps
    // what works without the game - commands, frame and GL callbacks, files.
    if (SWSE_GameBuildSafeMode() && g_plN > 0)
        LogLine("plugins: safe mode (unknown game build) - they load, but Game.PlayerPos, "
                "PlayerYaw, SetPlayerYaw, PlayerHealth, PlayerStamina and Memory.Write answer "
                "SWSE_E_UNSUPPORTED, HashPath and ResourceLookup 0, and no level event comes");
}

void SWSE_PluginsRescan() {
    if (!g_inited) return;
    int before = g_plN;
    Scan();
    LogScan("rescanned");
    char b[200];
    for (int i = before; i < g_plN; i++) {
        Fmt(b, sizeof(b), "new plugin '%s' in '%s' - `features %s on` to load it",
            g_pl[i].name, g_pl[i].mod, g_pl[i].name);
        if (g_pl[i].state != PS_REFUSED) Say(b);
    }
}

// ==========================================================================
//  Loading
// ==========================================================================
static bool Sha256File(const char* path, char hex[65]) {
    hex[0] = 0;
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hh = nullptr;
    bool ok = false;
    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0)) &&
        BCRYPT_SUCCESS(BCryptCreateHash(alg, &hh, NULL, 0, NULL, 0, 0))) {
        static BYTE buf[65536];                   // render thread only
        DWORD got = 0;
        ok = true;
        while (ReadFile(f, buf, sizeof(buf), &got, NULL) && got)
            if (!BCRYPT_SUCCESS(BCryptHashData(hh, buf, got, 0))) { ok = false; break; }
        BYTE d[32];
        if (ok && BCRYPT_SUCCESS(BCryptFinishHash(hh, d, sizeof(d), 0))) {
            for (int i = 0; i < 32; i++) Fmt(hex + i * 2, 3, "%02x", d[i]);
        } else {
            ok = false;
        }
    }
    if (hh) BCryptDestroyHash(hh);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return ok;
}

static bool Refuse(Plugin* p, const char* why) {
    p->state = PS_REFUSED;
    lstrcpynA(p->why, why, sizeof(p->why));
    char b[400];
    Fmt(b, sizeof(b), "plugin '%s' REFUSED: %s", p->name, p->why);
    Say(b);
    Fmt(b, sizeof(b), "  plugin '%s' is %s from mod '%s'", p->name, p->path, p->mod);
    LogLine(b);
    return false;
}

// The sequence of PLUGIN_SYSTEM.md 3.3, on the render thread. A refused or
// faulted plugin's DLL stays mapped: its DllMain has run, and unmapping is not
// worth the risk (6.5).
static bool LoadPlugin(Plugin* p) {
    char b[400];
    // Provenance first (QA D8): the path and the hash of exactly what runs, in
    // the log before any of its code does.
    if (!Sha256File(p->path, p->sha)) lstrcpynA(p->sha, "unreadable", sizeof(p->sha));
    Fmt(b, sizeof(b), "plugin '%s': loading %s (mod '%s') sha256 %s", p->name, p->path, p->mod, p->sha);
    LogLine(b);

    // No "cannot load" system dialog on the render thread: a bad image must be
    // an error code, not a modal box behind the game.
    DWORD oldMode = 0;
    SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &oldMode);
    // Dependencies beside the plugin resolve first.
    HMODULE h = LoadLibraryExA(p->path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    DWORD err = GetLastError();
    SetThreadErrorMode(oldMode, NULL);
    if (!h) {
        Fmt(b, sizeof(b), "could not load the DLL (error %u; 193 = not a 32-bit DLL)", (unsigned)err);
        return Refuse(p, b);
    }
    p->h = h;
    g_loadedN++;
    const IMAGE_NT_HEADERS* nt =
        (const IMAGE_NT_HEADERS*)((const BYTE*)h + ((const IMAGE_DOS_HEADER*)h)->e_lfanew);
    p->base = (unsigned)(uintptr_t)h;
    p->imgSize = nt->OptionalHeader.SizeOfImage;

    SWSEPlugin_QueryFn q = (SWSEPlugin_QueryFn)GetProcAddress(h, SWSE_PLUGIN_QUERY_NAME);
    SWSEPlugin_LoadFn  l = (SWSEPlugin_LoadFn)GetProcAddress(h, SWSE_PLUGIN_LOAD_NAME);
    if (!q || !l) return Refuse(p, "not an SWSE plugin (no SWSEPlugin_Query / SWSEPlugin_Load export)");

    SWSEPluginInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    g_querying = OwnerOf(p);
    g_dispatch++;
    int r = CallQuery(p, q, &info);
    g_dispatch--;
    g_querying = -1;
    if (r < 0) { Faulted(p); return false; }
    if (!r) return Refuse(p, "its Query declined to load (see its log lines)");
    if (info.apiVersion != SWSE_PLUGIN_API_VERSION) {
        Fmt(b, sizeof(b), "built for plugin API %u; this SWSE serves %u", info.apiVersion, SWSE_PLUGIN_API_VERSION);
        return Refuse(p, b);
    }
    char declared[64];
    if (!SafeCopyStr(declared, info.name, sizeof(declared)))
        return Refuse(p, "its Query returned an unreadable name");
    if (lstrcmpiA(declared, p->name)) {
        Fmt(b, sizeof(b), "declares the name '%s' but the file is %s.dll", declared, p->name);
        return Refuse(p, b);
    }
    if (info.minSwseVersion > SWSE_HOST_VERSION) {
        Fmt(b, sizeof(b), "needs SWSE %u.%u or newer (this is %s)", SWSE_VERSION_MAJOR(info.minSwseVersion),
            SWSE_VERSION_MINOR(info.minSwseVersion), SWSE_VERSION);
        return Refuse(p, b);
    }
    if (!SafeCopyStr(p->desc, info.description, sizeof(p->desc)) ||
        !SafeCopyStr(p->author, info.author, sizeof(p->author)) ||
        !SafeCopyStr(p->aliases, info.aliases, sizeof(p->aliases)))
        return Refuse(p, "its Query returned an unreadable string");
    p->version = info.version;
    p->minSwse = info.minSwseVersion;
    p->apiVersion = info.apiVersion;
    p->flags = info.flags;

    g_dispatch++;
    r = CallLoad(p, l);
    g_dispatch--;
    if (r <= 0) Rollback(p);                  // nothing it registered survives
    if (r < 0) { Faulted(p); return false; }
    if (!r) return Refuse(p, "its Load returned 0 (see its log lines)");
    p->state = PS_LOADED;
    SWSE_FeatureSetPluginInfo(p->feat, p->desc, p->aliases);
    Fmt(b, sizeof(b), "plugin '%s' %u.%u.%u loaded (plugin API %u, needs SWSE %u.%u)%s%s", p->name,
        SWSE_VERSION_MAJOR(p->version), SWSE_VERSION_MINOR(p->version), SWSE_VERSION_PATCH(p->version),
        p->apiVersion, SWSE_VERSION_MAJOR(p->minSwse), SWSE_VERSION_MINOR(p->minSwse),
        p->author[0] ? " by " : "", p->author);
    LogLine(b);
    return true;
}

bool SWSE_PluginFeatureStart(int feat, char* msg, int msgLen) {
    Plugin* p = ByFeat(feat);
    char m[300] = { 0 };
    if (!p) {
        Fmt(msg, msgLen, "no such plugin");
        return false;
    }
    if (p->state == PS_FAULTED) {
        Fmt(msg, msgLen, "%s faulted this session and stays off until the game restarts (%s)", p->name, p->why);
        return false;
    }
    if (p->state == PS_REFUSED) {
        Fmt(msg, msgLen, "refused: %s", p->why);
        return false;
    }
    if (!p->h) {
        if (!p->present) {
            Fmt(msg, msgLen, "%s.dll is no longer in an enabled mod - put it back and `mods reload`", p->name);
            return false;
        }
        if (!LoadPlugin(p)) {
            Fmt(msg, msgLen, "%s: %s", p->state == PS_FAULTED ? "FAULTED" : "refused", p->why);
            return false;
        }
    }
    if (p->onEnable) {
        double t0 = NowMs();
        g_dispatch++;
        int r = CallSwitch(p, p->onEnable, m, (int)sizeof(m), "onEnable");
        g_dispatch--;
        p->msNow += NowMs() - t0;
        m[sizeof(m) - 1] = 0;
        if (r < 0) {
            Faulted(p);
            Fmt(msg, msgLen, "FAULTED: %s", p->why);
            return false;
        }
        if (!r) {
            Fmt(msg, msgLen, "declined by the plugin: %s", m[0] ? m : "its onEnable returned 0");
            return false;
        }
    }
    p->msAvg = p->msWorst = 0;                // costs and evidence are per switch-on
    p->framesTimed = 0;
    p->calls = p->binds = 0;
    RecountGl();                              // its flag is already on (the caller set it)
    Fmt(msg, msgLen, "%s", m[0] ? m : "on");
    return true;
}

bool SWSE_PluginFeatureStop(int feat, char* msg, int msgLen) {
    Plugin* p = ByFeat(feat);
    char m[300] = { 0 };
    if (!p || p->state != PS_LOADED) {
        Fmt(msg, msgLen, "off");
        return true;
    }
    if (p->onDisable) {
        double t0 = NowMs();
        g_dispatch++;
        int r = CallSwitch(p, p->onDisable, m, (int)sizeof(m), "onDisable");
        g_dispatch--;
        p->msNow += NowMs() - t0;
        m[sizeof(m) - 1] = 0;
        if (r < 0) {
            Faulted(p);
            Fmt(msg, msgLen, "FAULTED in onDisable: %s", p->why);
            return true;                      // off either way
        }
    }
    Fmt(msg, msgLen, "%s", m[0] ? m : "off");
    return true;
}

void SWSE_PluginDescribeFeature(int feat, char* out, int outLen) {
    const Plugin* p = ByFeat(feat);
    if (!p) { Fmt(out, outLen, "(plugin)"); return; }
    switch (p->state) {
    case PS_FOUND:
        if (!p->present)   Fmt(out, outLen, "(plugin, gone from SWSEMods) %s", p->path);
        else if (p->vDesc[0])
            Fmt(out, outLen, "(plugin, not loaded) %s - %s\\plugins\\%s.dll", p->vDesc, p->mod, p->name);
        else               Fmt(out, outLen, "(plugin, not loaded) %s\\plugins\\%s.dll", p->mod, p->name);
        break;
    case PS_LOADED:
        Fmt(out, outLen, "(plugin%s) %s", p->present ? "" : ", file gone - loaded until exit",
            p->desc[0] ? p->desc : p->name);
        break;
    case PS_FAULTED: Fmt(out, outLen, "(plugin, FAULTED - off until restart) %s", p->why); break;
    case PS_REFUSED: Fmt(out, outLen, "(plugin, refused) %s", p->why); break;
    }
}

// ==========================================================================
//  Per frame
// ==========================================================================
static float    g_dtMs = 0;
static double   g_lastBegin = 0;
static DWORD    g_tickMs = 0;
static int      g_sizeFrame = -1, g_w = 0, g_h = 0;

void SWSE_PluginsNotifyFeature(int feat, bool on);

void SWSE_PluginsFrameBegin(HDC hdc) {
    (void)hdc;
    g_frameNo++;
    if (!g_loadedN) return;                    // no plugin loaded: nothing to do
    double now = NowMs();
    g_dtMs = g_lastBegin > 0 ? (float)(now - g_lastBegin) : 0.0f;
    g_lastBegin = now;
    g_tickMs = GetTickCount();
    for (int i = 0; i < g_plN; i++) {
        Plugin* p = &g_pl[i];
        if (!p->h) continue;
        p->msLast = p->msNow;
        p->msNow = 0;
        p->bindsLast = p->bindsNow;
        p->bindsNow = 0;
        if (!IsOn(p)) continue;
        p->msAvg = p->framesTimed ? p->msAvg + (p->msLast - p->msAvg) / 64.0 : p->msLast;
        if (p->msLast > p->msWorst) p->msWorst = p->msLast;
        p->framesTimed++;
    }
    Compact();
    RecountGl();                               // switches may have changed since
    // A plugin that faulted since the last frame is a switch that went off:
    // the other plugins hear it here, not from inside whatever faulted.
    for (int i = 0; i < g_plN; i++)
        if (g_pl[i].faultNotice) { g_pl[i].faultNotice = false; SWSE_PluginsNotifyFeature(g_pl[i].feat, false); }
    // Lines plugins' threads queued with Console.Post, in order.
    if (g_postN) DrainPosts();
}

// LEVEL_UP when the watcher's epoch changes with a body present; LEVEL_DOWN
// when the body goes. Tracked every frame (two reads), raised only to
// subscribers. Anything that rebuilds the player's motion object is a new
// epoch, so LEVEL_UP can come without a LEVEL_DOWN before it.
void SWSE_PluginsLevelEvents() {
    if (!g_inited) return;
    static unsigned s_epoch = 0;
    static bool     s_up = false;
    unsigned ep = SWSE_LevelEpoch();
    bool up = SWSE_LevelUp();
    if (up && ep != s_epoch) {
        s_epoch = ep;
        s_up = true;
        if (g_evN) {
            SWSELevelEvent e = { sizeof(SWSELevelEvent), ep, 0, 0 };
            unsigned pl = 0, mo = 0;
            SWSE_LevelBody(&pl, &mo);
            e.player = pl;
            e.motion = mo;
            Raise(SWSE_EV_LEVEL_UP, &e, "the level-up event");
        }
    } else if (!up && s_up) {
        s_up = false;
        if (g_evN) {
            SWSELevelEvent e = { sizeof(SWSELevelEvent), s_epoch, 0, 0 };
            Raise(SWSE_EV_LEVEL_DOWN, &e, "the level-down event");
        }
    }
}

void SWSE_PluginsModsReloaded() {
    if (g_evN) Raise(SWSE_EV_MODS_RELOADED, nullptr, "the mods-reloaded event");
}

// Any switch changed live, built-in or plugin (`features`, `auto`, a fault).
void SWSE_PluginsNotifyFeature(int feat, bool on) {
    if (!g_evN || feat < 0) return;
    SWSEFeatureEvent e = { sizeof(SWSEFeatureEvent), SWSE_FeatureName((SwseFeature)feat), on ? 1u : 0u };
    Raise(SWSE_EV_FEATURE, &e, "the feature event");
}

// ---- GL notifications (glspy.cpp) ---------------------------------------------
// Listeners are counted, not timed: two QPC reads per bind, thousands of times
// a frame, would cost more than the listeners themselves.
void SWSE_PluginsNotifyBind(unsigned target, unsigned tex) {
    if (!OnRender()) return;                  // callbacks arrive on the render thread only
    int n = g_bindN;
    g_inGl++;
    for (int i = 0; i < n; i++) {
        PBind& b = g_bind[i];
        if (b.dead) continue;
        Plugin* p = &g_pl[b.owner];
        if (!IsOn(p)) continue;
        SWSEBindTextureFn fn = b.fn; void* u = b.user;
        g_dispatch++;
        bool ok = CallBind(p, fn, target, tex, u);
        g_dispatch--;
        p->binds++;
        p->bindsNow++;
        if (!ok) Faulted(p);                  // only that plugin stops; binds go on
    }
    g_inGl--;
}

void SWSE_PluginsNotifyUpload(unsigned texId, unsigned fingerprint, int w, int h,
                              unsigned glFormat, int dataSize, const void* data) {
    if (!OnRender()) return;
    SWSETextureUploadEvent e;
    e.size = sizeof(e);
    e.texId = texId;
    e.fingerprint = fingerprint;
    e.width = w;
    e.height = h;
    e.glFormat = glFormat;
    e.dataSize = dataSize;
    e.data = data;
    g_inGl++;
    Raise(SWSE_EV_TEXTURE_UPLOAD, &e, "the texture upload event");
    g_inGl--;
}

// ---- the OVERLAY state snapshot (PLUGIN_SYSTEM.md 6.7) -------------------------
// Taken only when an OVERLAY callback will run. The attribute stacks cover
// enables, blending, viewport, matrix mode and the active texture unit; the
// matrices are saved by value (the projection stack can be as shallow as 2);
// the object bindings are saved and put back when they changed. A plugin that
// leaves a stack pushed has it popped back to where SWSE found it.
#define GL_CURRENT_PROGRAM_             0x8B8D
#define GL_VERTEX_PROGRAM_ARB_          0x8620
#define GL_FRAGMENT_PROGRAM_ARB_        0x8804
#define GL_PROGRAM_BINDING_ARB_         0x8677
#define GL_FRAMEBUFFER_BINDING_EXT_     0x8CA6
#define GL_ARRAY_BUFFER_ARB_            0x8892
#define GL_ELEMENT_ARRAY_BUFFER_ARB_    0x8893
#define GL_ARRAY_BUFFER_BINDING_ARB_    0x8894
#define GL_ELEMENT_ARRAY_BUFFER_BINDING_ARB_ 0x8895

typedef void (APIENTRY* PfnUseProgram)(GLuint);
typedef void (APIENTRY* PfnBindProgramARB)(GLenum, GLuint);
typedef void (APIENTRY* PfnGetProgramivARB)(GLenum, GLenum, GLint*);
typedef void (APIENTRY* PfnBindBufferARB)(GLenum, GLuint);
static PfnUseProgram      s_useProgram = nullptr;
static PfnBindProgramARB  s_bindProgram = nullptr;
static PfnGetProgramivARB s_getProgramiv = nullptr;
static PfnBindBufferARB   s_bindBuffer = nullptr;

static void ResolveGl() {
    static bool done = false;
    if (done) return;
    done = true;
    HMODULE gl = GetModuleHandleA("opengl32.dll");
    typedef PROC (WINAPI* Gpa)(LPCSTR);
    Gpa gpa = gl ? (Gpa)GetProcAddress(gl, "wglGetProcAddress") : nullptr;
    if (!gpa) return;
    s_useProgram = (PfnUseProgram)gpa("glUseProgram");
    if (!s_useProgram) s_useProgram = (PfnUseProgram)gpa("glUseProgramObjectARB");
    s_bindProgram = (PfnBindProgramARB)gpa("glBindProgramARB");
    s_getProgramiv = (PfnGetProgramivARB)gpa("glGetProgramivARB");
    s_bindBuffer = (PfnBindBufferARB)gpa("glBindBufferARB");
    if (!s_bindBuffer) s_bindBuffer = (PfnBindBufferARB)gpa("glBindBuffer");
}

struct GlSnap {
    GLint attrib, client, depth[3], mode;
    GLfloat mat[3][16];
    GLint program, vp, fp, fbo, arrayBuf, elemBuf;
};
static const GLenum kModes[3]  = { GL_MODELVIEW, GL_PROJECTION, GL_TEXTURE };
static const GLenum kDepths[3] = { GL_MODELVIEW_STACK_DEPTH, GL_PROJECTION_STACK_DEPTH, GL_TEXTURE_STACK_DEPTH };
static const GLenum kMats[3]   = { GL_MODELVIEW_MATRIX, GL_PROJECTION_MATRIX, GL_TEXTURE_MATRIX };

static void SnapSave(GlSnap* s) {
    ResolveGl();
    memset(s, 0, sizeof(*s));
    glGetIntegerv(GL_ATTRIB_STACK_DEPTH, &s->attrib);
    glGetIntegerv(GL_CLIENT_ATTRIB_STACK_DEPTH, &s->client);
    glGetIntegerv(GL_MATRIX_MODE, &s->mode);
    for (int i = 0; i < 3; i++) {
        glGetIntegerv(kDepths[i], &s->depth[i]);
        glGetFloatv(kMats[i], s->mat[i]);
    }
    if (s_useProgram) glGetIntegerv(GL_CURRENT_PROGRAM_, &s->program);
    if (s_getProgramiv) {
        s_getProgramiv(GL_VERTEX_PROGRAM_ARB_, GL_PROGRAM_BINDING_ARB_, &s->vp);
        s_getProgramiv(GL_FRAGMENT_PROGRAM_ARB_, GL_PROGRAM_BINDING_ARB_, &s->fp);
    }
    glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT_, &s->fbo);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING_ARB_, &s->arrayBuf);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING_ARB_, &s->elemBuf);
    while (glGetError() != GL_NO_ERROR) {}    // queries a driver lacks leave errors
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);
}

static void SnapRestore(const GlSnap* s, bool faulted) {
    if (faulted) {
        // Harmless outside glBegin/glEnd, and closes one if the fault struck
        // inside: the console, drawn next, must not inherit a half primitive.
        glEnd();
        for (int i = 0; i < 64 && glGetError() != GL_NO_ERROR; i++) {}
    }
    GLint d = 0;
    glGetIntegerv(GL_CLIENT_ATTRIB_STACK_DEPTH, &d);
    for (; d > s->client; d--) glPopClientAttrib();
    glGetIntegerv(GL_ATTRIB_STACK_DEPTH, &d);
    for (; d > s->attrib; d--) glPopAttrib();       // the last pop is SWSE's own
    for (int i = 0; i < 3; i++) {
        glMatrixMode(kModes[i]);
        glGetIntegerv(kDepths[i], &d);
        for (; d > s->depth[i]; d--) glPopMatrix();
        glLoadMatrixf(s->mat[i]);
    }
    glMatrixMode((GLenum)s->mode);
    GLint now = 0;
    if (s_useProgram) {
        glGetIntegerv(GL_CURRENT_PROGRAM_, &now);
        if (now != s->program) s_useProgram((GLuint)s->program);
    }
    if (s_getProgramiv && s_bindProgram) {
        s_getProgramiv(GL_VERTEX_PROGRAM_ARB_, GL_PROGRAM_BINDING_ARB_, &now);
        if (now != s->vp) s_bindProgram(GL_VERTEX_PROGRAM_ARB_, (GLuint)s->vp);
        s_getProgramiv(GL_FRAGMENT_PROGRAM_ARB_, GL_PROGRAM_BINDING_ARB_, &now);
        if (now != s->fp) s_bindProgram(GL_FRAGMENT_PROGRAM_ARB_, (GLuint)s->fp);
    }
    glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT_, &now);
    if (now != s->fbo) SWSE_GlBindFramebufferQuiet((unsigned)s->fbo);
    if (s_bindBuffer) {
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING_ARB_, &now);
        if (now != s->arrayBuf) s_bindBuffer(GL_ARRAY_BUFFER_ARB_, (GLuint)s->arrayBuf);
        glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING_ARB_, &now);
        if (now != s->elemBuf) s_bindBuffer(GL_ELEMENT_ARRAY_BUFFER_ARB_, (GLuint)s->elemBuf);
    }
    while (glGetError() != GL_NO_ERROR) {}
}

static void FrameSize(HDC hdc) {
    if (g_sizeFrame == (int)g_frameNo) return;
    g_sizeFrame = (int)g_frameNo;
    HWND wnd = hdc ? WindowFromDC(hdc) : nullptr;
    RECT rc;
    if (wnd && GetClientRect(wnd, &rc)) { g_w = rc.right - rc.left; g_h = rc.bottom - rc.top; return; }
    GLint vp[4] = { 0, 0, 0, 0 };
    glGetIntegerv(GL_VIEWPORT, vp);
    g_w = vp[2]; g_h = vp[3];
}

void SWSE_PluginsFrame(unsigned phase, HDC hdc) {
    if (!g_loadedN) return;
    int n = g_frameN;                          // registered during the loop: next frame
    bool any = false;
    for (int i = 0; i < n && !any; i++)
        any = !g_frame[i].dead && g_frame[i].phase == phase && IsOn(&g_pl[g_frame[i].owner]);
    if (!any) return;
    FrameSize(hdc);
    SWSEFrameInfo fi;
    memset(&fi, 0, sizeof(fi));
    fi.size = sizeof(fi);
    fi.phase = phase;
    fi.frame = g_frameNo;
    fi.tickMs = g_tickMs;
    fi.dtMs = g_dtMs;
    fi.width = g_w;
    fi.height = g_h;
    fi.hdc = hdc;
    fi.levelUp = SWSE_LevelUp() ? 1 : 0;
    fi.levelEpoch = SWSE_LevelEpoch();
    fi.consoleOpen = SWSE_ConsoleOpen() ? 1 : 0;
    const char* where = phase == SWSE_FRAME_OVERLAY ? "an overlay callback" : "a frame callback";
    GlSnap snap;
    if (phase == SWSE_FRAME_OVERLAY) SnapSave(&snap);
    bool faulted = false;
    for (int i = 0; i < n; i++) {
        PFrame& f = g_frame[i];
        if (f.dead || f.phase != phase) continue;
        Plugin* p = &g_pl[f.owner];
        if (!IsOn(p)) continue;
        SWSEFrameFn fn = f.fn; void* u = f.user;
        double t0 = NowMs();
        g_dispatch++;
        bool ok = CallFrame(p, fn, &fi, u, where);
        g_dispatch--;
        p->msNow += NowMs() - t0;
        p->calls++;
        if (!ok) { Faulted(p); faulted = true; }
    }
    if (phase == SWSE_FRAME_OVERLAY) SnapRestore(&snap, faulted);
}

// The frame hook's FRAMESTALL line names a plugin when one owns most of SWSE's
// share of a slow frame - "name the program, not the symptom", for stalls.
int SWSE_PluginsStallCulprit(double swseMs, char* out, int outLen) {
    if (!g_loadedN || !out || outLen <= 0) return 0;
    const Plugin* worst = nullptr;
    for (int i = 0; i < g_plN; i++)
        if (g_pl[i].h && (!worst || g_pl[i].msNow > worst->msNow)) worst = &g_pl[i];
    if (!worst || worst->msNow <= swseMs * 0.5 || worst->msNow < 1.0) return 0;
    Fmt(out, outLen, "plugin %s %d ms", worst->name, (int)(worst->msNow + 0.5));
    return 1;
}

// ==========================================================================
//  Console integration
// ==========================================================================
// Among the providers of a name, the one LATER in load order that is on runs
// (PLUGIN_SYSTEM.md 5.1): the load_order.txt rule, the same answer whatever
// the switch-on order, and "off" keeps meaning "as if not there".
static bool CanRun(const PCmd& c) {
    const Plugin* p = &g_pl[c.owner];
    return IsOn(p) || ((c.flags & SWSE_CMD_F_WHILE_OFF) && p->state == PS_LOADED);
}

bool SWSE_PluginCmdRun(int argc, char** argv) {
    if (!g_cmdN || argc < 1 || !argv || !argv[0]) return false;
    int run = -1, any = -1;
    int n = g_cmdN;
    for (int i = 0; i < n; i++) {
        const PCmd& c = g_cmd[i];
        if (c.dead || lstrcmpiA(c.name, argv[0])) continue;
        int rank = g_pl[c.owner].order;
        if (any < 0 || rank > g_pl[g_cmd[any].owner].order) any = i;
        if (CanRun(c) && (run < 0 || rank > g_pl[g_cmd[run].owner].order)) run = i;
    }
    if (run >= 0) {
        Plugin* p = &g_pl[g_cmd[run].owner];
        SWSECommandFn fn = g_cmd[run].fn;
        void* u = g_cmd[run].user;
        double t0 = NowMs();
        g_dispatch++;
        bool ok = CallCommand(p, fn, argc, (const char* const*)argv, u);
        g_dispatch--;
        p->msNow += NowMs() - t0;
        p->calls++;
        if (!ok) Faulted(p);
        return true;
    }
    if (any >= 0) {
        const Plugin* p = &g_pl[g_cmd[any].owner];
        char b[240];
        if (p->state == PS_FAULTED)
            Fmt(b, sizeof(b), "'%s' belongs to plugin %s, which faulted and is off until the game restarts",
                argv[0], p->name);
        else
            Fmt(b, sizeof(b), "'%s' belongs to plugin %s, which is off - `features %s on`",
                argv[0], p->name, p->name);
        SWSE_ConsolePrint(b);
        return true;
    }
    return false;
}

bool SWSE_PluginCmdExists(const char* name) {
    if (!name) return false;
    for (int i = 0; i < g_cmdN; i++)
        if (!g_cmd[i].dead && !lstrcmpiA(g_cmd[i].name, name)) return true;
    return false;
}

static bool StartsWithCI(const char* s, const char* pre) {
    while (*pre) { if ((*s | 0x20) != (*pre | 0x20)) return false; s++; pre++; }
    return true;
}

int SWSE_PluginCmdComplete(const char* prefix, const char** out, int maxOut) {
    int n = 0;
    if (!prefix) return 0;
    for (int i = 0; i < g_cmdN && n < maxOut; i++) {
        const PCmd& c = g_cmd[i];
        if (c.dead || !CanRun(c) || !StartsWithCI(c.name, prefix)) continue;
        bool dup = false;
        for (int k = 0; k < n; k++) if (!lstrcmpiA(out[k], c.name)) dup = true;
        if (!dup) out[n++] = c.name;
    }
    return n;
}

int SWSE_PluginCmdAt(int i, const char** name, const char** category,
                     const char** help, const char** plugin, int* on) {
    int k = 0;
    for (int j = 0; j < g_cmdN; j++) {
        const PCmd& c = g_cmd[j];
        if (c.dead) continue;
        if (k++ != i) continue;
        if (name) *name = c.name;
        if (category) *category = c.cat;
        if (help) *help = c.help;
        if (plugin) *plugin = g_pl[c.owner].name;
        if (on) *on = CanRun(c) ? 1 : 0;
        return 1;
    }
    return 0;
}

bool SWSE_PluginIsName(const char* name) { return ByName(name) != nullptr; }

int SWSE_PluginsFound() {
    int n = 0;
    for (int i = 0; i < g_plN; i++) if (g_pl[i].present || g_pl[i].h) n++;
    return n;
}
int SWSE_PluginsOn() {
    int n = 0;
    for (int i = 0; i < g_plN; i++) if (IsOn(&g_pl[i])) n++;
    return n;
}

// ==========================================================================
//  Reports: selftest, perf, status (PLUGIN_SYSTEM.md 5.7)
// ==========================================================================
// The self-test asserts on evidence of work done, never on a flag. A plugin
// with a SELFTEST report of its own runs it; for one without, SWSE's evidence
// is that it has been called at all since it was switched on.
void SWSE_PluginsSelfTest(void (*check)(int, const char*, const char*), void (*off)(const char*)) {
    if (!g_inited || !check) return;
    ReportSink sink = { SINK_TAG, SWSE_REPORT_SELFTEST, nullptr, check };
    char d[320];
    for (int i = 0; i < g_plN; i++) {
        Plugin* p = &g_pl[i];
        if (!p->present && !p->h) continue;
        if (p->state == PS_FAULTED) {
            Fmt(d, sizeof(d), "FAULTED this session: %s", p->why);
            check(0, p->name, d);
            continue;
        }
        if (p->state == PS_REFUSED) {
            // Only a refusal the user ran into: its switch says on.
            if (p->feat >= 0 && SWSE_FeatureSaved((SwseFeature)p->feat)) {
                Fmt(d, sizeof(d), "switched on but refused: %s", p->why);
                check(0, p->name, d);
            }
            continue;
        }
        if (!IsOn(p)) { if (off) off(p->name); continue; }
        int ran = 0;
        if (!RunReports(p, SWSE_REPORT_SELFTEST, &sink, &ran)) {
            Fmt(d, sizeof(d), "FAULTED in its self-test: %s", p->why);
            check(0, p->name, d);
            continue;
        }
        if (ran) continue;
        if (p->calls + p->binds > 0)
            Fmt(d, sizeof(d), "plugin on: %u call(s)%s, %.2f ms/frame (no self-test of its own)",
                p->calls, p->binds ? " + bind notifications" : "", p->msAvg);
        else
            Fmt(d, sizeof(d), "plugin on, but nothing has called into it yet (no self-test of its own)");
        check(p->calls + p->binds > 0 ? 1 : -1, p->name, d);
    }
}

void SWSE_PluginsPerf(void (*emit)(const char*)) {
    if (!g_inited || !emit || !g_loadedN) return;
    ReportSink sink = { SINK_TAG, SWSE_REPORT_PERF, emit, nullptr };
    char b[240];
    for (int i = 0; i < g_plN; i++) {
        Plugin* p = &g_pl[i];
        if (!p->h || (p->state != PS_LOADED && !p->framesTimed)) continue;
        char label[32];
        Fmt(label, sizeof(label), "plugin %s", p->name);
        char binds[48] = "";
        if (p->bindsLast) Fmt(binds, sizeof(binds), "; %u binds seen last frame", p->bindsLast);
        Fmt(b, sizeof(b), "%-17s: last %.2f ms, avg %.2f, worst %.2f%s%s", label,
            p->msLast, p->msAvg, p->msWorst, binds,
            p->state == PS_FAULTED ? " (FAULTED)" : IsOn(p) ? "" : " (off)");
        emit(b);
        if (IsOn(p)) RunReports(p, SWSE_REPORT_PERF, &sink, nullptr);
    }
}

void SWSE_PluginsStatus(void (*emit)(const char*)) {
    if (!g_inited || !emit) return;
    int found = SWSE_PluginsFound();
    if (!found) return;
    int on = 0, faulted = 0;
    for (int i = 0; i < g_plN; i++) {
        if (IsOn(&g_pl[i])) on++;
        if (g_pl[i].state == PS_FAULTED) faulted++;
    }
    char b[240];
    Fmt(b, sizeof(b), "  plugins: %d found, %d on%s", found, on,
        faulted ? " - some FAULTED this session (`plugins`)" : "");
    emit(b);
    ReportSink sink = { SINK_TAG, SWSE_REPORT_STATUS, emit, nullptr };
    for (int i = 0; i < g_plN; i++)
        if (IsOn(&g_pl[i])) RunReports(&g_pl[i], SWSE_REPORT_STATUS, &sink, nullptr);
}

// ---- `plugins` ----------------------------------------------------------------------
static const char* StateName(const Plugin* p) {
    switch (p->state) {
    case PS_LOADED:  return IsOn(p) ? "on" : "off (loaded)";
    case PS_FAULTED: return "FAULTED";
    case PS_REFUSED: return "REFUSED";
    default:         return p->present ? "off" : "gone";
    }
}

static int CountOwned(const Plugin* p, int* frames, int* events, int* listeners = nullptr, int* reports = nullptr) {
    int o = OwnerOf(p), c = 0;
    if (frames) { *frames = 0; for (int i = 0; i < g_frameN; i++) if (!g_frame[i].dead && g_frame[i].owner == o) (*frames)++; }
    if (events) { *events = 0; for (int i = 0; i < g_evN; i++) if (!g_ev[i].dead && g_ev[i].owner == o) (*events)++; }
    if (listeners) { *listeners = 0; for (int i = 0; i < g_bindN; i++) if (!g_bind[i].dead && g_bind[i].owner == o) (*listeners)++; }
    if (reports) { *reports = 0; for (int i = 0; i < g_repN; i++) if (!g_rep[i].dead && g_rep[i].owner == o) (*reports)++; }
    for (int i = 0; i < g_cmdN; i++) if (!g_cmd[i].dead && g_cmd[i].owner == o) c++;
    return c;
}

static void ListPlugins() {
    char b[400];
    int found = 0, on = 0;
    for (int i = 0; i < g_plN; i++) { if (g_pl[i].present || g_pl[i].h) found++; if (IsOn(&g_pl[i])) on++; }
    Fmt(b, sizeof(b), "plugins: %d found in %d mod(s), %d on", found, g_scanMods, on);
    SWSE_ConsolePrint(b);
    if (!found) {
        SWSE_ConsolePrint("  none - a plugin is SWSEMods\\<Mod>\\plugins\\<name>.dll (see sdk\\README.md)");
        return;
    }
    for (int i = 0; i < g_plN; i++) {
        const Plugin* p = &g_pl[i];
        if (!p->present && !p->h) continue;
        char ver[24] = "-", detail[300];
        if (p->state == PS_LOADED || (p->state == PS_FAULTED && p->version))
            Fmt(ver, sizeof(ver), "%u.%u.%u", SWSE_VERSION_MAJOR(p->version), SWSE_VERSION_MINOR(p->version),
                SWSE_VERSION_PATCH(p->version));
        else if (p->vVer[0])
            Fmt(ver, sizeof(ver), "%s", p->vVer);        // its version resource, read without loading it
        if (p->state == PS_REFUSED || p->state == PS_FAULTED) {
            Fmt(detail, sizeof(detail), "%s", p->why);
        } else if (p->state == PS_LOADED) {
            int cmds = CountOwned(p, nullptr, nullptr);
            Fmt(detail, sizeof(detail), "frame %.2f ms avg, %.2f worst; %d command%s; sha256 %.12s",
                p->msAvg, p->msWorst, cmds, cmds == 1 ? "" : "s", p->sha);
        } else {
            Fmt(detail, sizeof(detail), "not loaded%s%s", p->vDesc[0] ? " - " : "", p->vDesc);
        }
        Fmt(b, sizeof(b), "  %-12s %-13s %-7s %-16s %s", p->name, StateName(p), ver, p->mod, detail);
        SWSE_ConsolePrint(b);
    }
    for (int i = 0; i < g_shadowN; i++) {
        Fmt(b, sizeof(b), g_shadow[i].loadedKept
                ? "  (%s also in '%s': not used - the one from '%s' is loaded; restart to change)"
                : "  (%s also in '%s': shadowed by %s)",
            g_shadow[i].name, g_shadow[i].mod, g_shadow[i].by);
        SWSE_ConsolePrint(b);
    }
    SWSE_ConsolePrint("`plugins <name>` for one plugin; `features <name> on` loads and switches one on");
}

static void ShowPlugin(const Plugin* p) {
    char b[600];
    if (p->state == PS_LOADED || p->version) {
        Fmt(b, sizeof(b), "%s %u.%u.%u%s%s - %s", p->name, SWSE_VERSION_MAJOR(p->version),
            SWSE_VERSION_MINOR(p->version), SWSE_VERSION_PATCH(p->version),
            p->author[0] ? " by " : "", p->author, p->desc[0] ? p->desc : "(no description)");
    } else if (p->vDesc[0] || p->vVer[0]) {
        Fmt(b, sizeof(b), "%s %s - %s  (from its version resource; not loaded)", p->name,
            p->vVer[0] ? p->vVer : "?", p->vDesc[0] ? p->vDesc : "(no description)");
    } else {
        Fmt(b, sizeof(b), "%s - not loaded, so it has not described itself yet", p->name);
    }
    SWSE_ConsolePrint(b);
    Fmt(b, sizeof(b), "  file   : %s (mod '%s')%s", p->path, p->mod, p->present ? "" : " - no longer in an enabled mod");
    SWSE_ConsolePrint(b);
    if (p->h) {
        Fmt(b, sizeof(b), "  api    : plugin API %u, needs SWSE %u.%u; loaded at %08X",
            p->apiVersion, SWSE_VERSION_MAJOR(p->minSwse), SWSE_VERSION_MINOR(p->minSwse), p->base);
        SWSE_ConsolePrint(b);
        Fmt(b, sizeof(b), "  sha256 : %s", p->sha);
        SWSE_ConsolePrint(b);
    }
    int frames = 0, events = 0, listeners = 0, reports = 0;
    int cmds = CountOwned(p, &frames, &events, &listeners, &reports);
    if (p->state == PS_LOADED) {
        Fmt(b, sizeof(b), "  state  : %s; frame %.2f ms avg, %.2f worst, %.2f last", StateName(p),
            p->msAvg, p->msWorst, p->msLast);
        SWSE_ConsolePrint(b);
        Fmt(b, sizeof(b), "  has    : %d frame callback%s, %d event%s, %d bind listener%s, %d report%s, "
                          "%d command%s; %u calls, %u bind notifications since switched on",
            frames, frames == 1 ? "" : "s", events, events == 1 ? "" : "s", listeners, listeners == 1 ? "" : "s",
            reports, reports == 1 ? "" : "s", cmds, cmds == 1 ? "" : "s", p->calls, p->binds);
    }
    else if (p->state == PS_FAULTED || p->state == PS_REFUSED)
        Fmt(b, sizeof(b), "  state  : %s - %s", StateName(p), p->why);
    else
        Fmt(b, sizeof(b), "  state  : %s - `features %s on` loads it", StateName(p), p->name);
    SWSE_ConsolePrint(b);
    int o = OwnerOf(p);
    for (int i = 0; i < g_cmdN; i++) {
        if (g_cmd[i].dead || g_cmd[i].owner != o) continue;
        Fmt(b, sizeof(b), "  command: %-14s %s", g_cmd[i].name, g_cmd[i].help);
        SWSE_ConsolePrint(b);
    }
}

void SWSE_PluginsCommand(int argc, char** argv) {
    if (argc > 1 && !lstrcmpiA(argv[1], "rescan")) {
        SWSE_PluginsRescan();
        ListPlugins();
        return;
    }
    if (argc > 1) {
        const Plugin* p = ByName(argv[1]);
        if (!p) {
            int f = SWSE_FeatureFind(argv[1]);              // a nickname
            p = (f >= 0) ? ByFeat(f) : nullptr;
        }
        if (!p) {
            char b[200];
            Fmt(b, sizeof(b), "no plugin called '%s' - `plugins` lists them", argv[1]);
            SWSE_ConsolePrint(b);
            return;
        }
        ShowPlugin(p);
        return;
    }
    ListPlugins();
}

// `query plugins`: <name>=on|off|faulted|refused for every plugin found.
void SWSE_PluginsQuery(char* out, int outLen) {
    if (!out || outLen <= 0) return;
    out[0] = 0;
    int u = 0;
    for (int i = 0; i < g_plN && u < outLen - 1; i++) {
        const Plugin* p = &g_pl[i];
        if (!p->present && !p->h) continue;
        const char* st = p->state == PS_FAULTED ? "faulted" : p->state == PS_REFUSED ? "refused"
                       : IsOn(p) ? "on" : "off";
        int k = _snprintf_s(out + u, outLen - u, _TRUNCATE, "%s%s=%s", u ? " " : "", p->name, st);
        if (k < 0) break;
        u += k;
    }
}
