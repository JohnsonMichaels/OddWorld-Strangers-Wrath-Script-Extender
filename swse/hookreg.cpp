// SWSE hook registry - see hookreg.h.

#include "hookreg.h"
#include "gamebuild.h"      // safe mode: `hooks` shows what was refused
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define MAX_HOOKS 128
#define MAX_REFUSED 32

struct HookEntry {
    unsigned addr, len;
    char     owner[32];
    char     kind[16];
    char     label[48];
    DWORD    tick;          // GetTickCount when noted
};

static HookEntry g_hook[MAX_HOOKS];
static int       g_hookN = 0;
static CRITICAL_SECTION g_hookLock;
static struct HookLockInit { HookLockInit() { InitializeCriticalSection(&g_hookLock); } } s_hookLockInit;

// Patches refused on an unknown game build (safe mode).
struct RefusedEntry { char owner[32]; char label[48]; };
static RefusedEntry g_ref[MAX_REFUSED];
static int          g_refN = 0;

void SWSE_HookRefused(const char* owner, const char* label) {
    if (!owner) owner = "?";
    if (!label) label = "?";
    EnterCriticalSection(&g_hookLock);
    bool dup = false;
    for (int i = 0; i < g_refN && !dup; i++)
        dup = !lstrcmpA(g_ref[i].owner, owner) && !lstrcmpA(g_ref[i].label, label);
    if (!dup && g_refN < MAX_REFUSED) {
        lstrcpynA(g_ref[g_refN].owner, owner, sizeof(g_ref[0].owner));
        lstrcpynA(g_ref[g_refN].label, label, sizeof(g_ref[0].label));
        g_refN++;
    }
    LeaveCriticalSection(&g_hookLock);
}

void SWSE_HookNote(const void* addr, unsigned len, const char* owner, const char* kind, const char* label) {
    if (!addr || !len) return;
    unsigned a = (unsigned)(uintptr_t)addr;
    EnterCriticalSection(&g_hookLock);
    int at = -1;
    for (int i = 0; i < g_hookN; i++) if (g_hook[i].addr == a) { at = i; break; }   // re-installed
    if (at < 0 && g_hookN < MAX_HOOKS) at = g_hookN++;
    if (at >= 0) {
        HookEntry& h = g_hook[at];
        h.addr = a;
        h.len = len;
        lstrcpynA(h.owner, owner ? owner : "?", sizeof(h.owner));
        lstrcpynA(h.kind, kind ? kind : "?", sizeof(h.kind));
        lstrcpynA(h.label, label ? label : "?", sizeof(h.label));
        h.tick = GetTickCount();
    }
    LeaveCriticalSection(&g_hookLock);
}

void SWSE_HookForget(const void* addr) {
    unsigned a = (unsigned)(uintptr_t)addr;
    EnterCriticalSection(&g_hookLock);
    for (int i = 0; i < g_hookN; i++) {
        if (g_hook[i].addr != a) continue;
        g_hook[i] = g_hook[--g_hookN];
        break;
    }
    LeaveCriticalSection(&g_hookLock);
}

int SWSE_HookOwner(unsigned addr, unsigned len, char* out, int outLen) {
    if (!len) len = 1;
    unsigned long long lo = addr, hi = (unsigned long long)addr + len;
    int found = 0;
    EnterCriticalSection(&g_hookLock);
    for (int i = 0; i < g_hookN && !found; i++) {
        unsigned long long hlo = g_hook[i].addr, hhi = hlo + g_hook[i].len;
        if (lo < hhi && hlo < hi) {
            found = 1;
            if (out && outLen > 0)
                _snprintf_s(out, outLen, _TRUNCATE, "%s: %s", g_hook[i].owner, g_hook[i].label);
        }
    }
    LeaveCriticalSection(&g_hookLock);
    return found;
}

int SWSE_HookCount() { return g_hookN; }

// "opengl32.dll+0x1A2B0", so an address in the list means something without a
// debugger - and survives ASLR moving the module between launches.
static void Where(unsigned addr, char* out, int outLen) {
    HMODULE m = nullptr;
    char path[MAX_PATH];
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)addr, &m) && m && GetModuleFileNameA(m, path, MAX_PATH)) {
        const char* b = strrchr(path, '\\');
        _snprintf_s(out, outLen, _TRUNCATE, "%s+0x%X", b ? b + 1 : path, addr - (unsigned)(uintptr_t)m);
    } else {
        _snprintf_s(out, outLen, _TRUNCATE, "%08X", addr);
    }
}

void SWSE_HooksList(void (*emit)(const char*)) {
    if (!emit) return;
    HookEntry copy[MAX_HOOKS];
    EnterCriticalSection(&g_hookLock);
    int n = g_hookN;
    memcpy(copy, g_hook, sizeof(HookEntry) * n);
    LeaveCriticalSection(&g_hookLock);
    char b[240];
    _snprintf_s(b, sizeof(b), _TRUNCATE, "%d live patch(es) - SWSE's own and any a plugin registered:", n);
    emit(b);
    for (int i = 0; i < n; i++) {
        char w[80];
        Where(copy[i].addr, w, sizeof(w));
        _snprintf_s(b, sizeof(b), _TRUNCATE, "  %-24s %2u bytes  %-12s %-13s %s",
                    copy[i].label, copy[i].len, copy[i].owner, copy[i].kind, w);
        emit(b);
    }
    if (!SWSE_GameBuildSafeMode()) return;
    // Safe mode: nothing above is in game code - the frame hook, OpenGL,
    // DirectInput and import slots are found by name on any build.
    RefusedEntry ref[MAX_REFUSED];
    EnterCriticalSection(&g_hookLock);
    int r = g_refN;
    memcpy(ref, g_ref, sizeof(RefusedEntry) * r);
    LeaveCriticalSection(&g_hookLock);
    emit("safe mode (unknown game build): no patch into game code - the ones above are "
         "OpenGL, DirectInput and import slots, found by name");
    if (!r) { emit("  refused this session: none (nothing has asked for one)"); return; }
    _snprintf_s(b, sizeof(b), _TRUNCATE, "  refused this session, %d:", r);
    emit(b);
    for (int i = 0; i < r; i++) {
        _snprintf_s(b, sizeof(b), _TRUNCATE, "  %-24s %-12s refused", ref[i].label, ref[i].owner);
        emit(b);
    }
}
