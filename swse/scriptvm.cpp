// SWSE Script-VM bridge â€” see scriptvm.h + swse/research/SCRIPT_VM.md.
//
// The game's script verbs are __cdecl native handlers taking a ScriptContext*
// as arg0 (for player ops that's effectively the player script object). We
// can't fabricate one, so we CAPTURE a real one: inline-hook GiveAmmo, and
// whenever the game calls it (ammo pickup / tutorial), record ctx = arg0.
// Console commands then replay: handler(ctx, arg).
//
// All replays run under SEH; a fault disables that path instead of crashing.

#include "scriptvm.h"
#include "scriptvm_gen.h"
#include <windows.h>
#include <tlhelp32.h>   // Thread32First: watchpoints must arm every thread, not just ours
#include <cstring>
#include <cstdlib>

// Every object scan below looks for game objects, and those all live in the
// game heap -- the scans already discarded hits outside it, but only AFTER
// walking all 2GB, which froze the game for ~10s on each command. Skipping the
// regions up front costs nothing and finds the same objects.
//
// HEAP_HI was 0x20000000 while stranger.exe was limited to a 2GB address space.
// With the LARGE_ADDRESS_AWARE patch (tools/laa_patcher.py) the process gets
// the full 4GB, and Windows may hand out heap blocks above the old ceiling.
// A stale bound here would not error -- the scans would simply stop finding
// NPCs that happened to land high, which is the worst kind of bug: silent and
// intermittent. Raised to 0x40000000, which still skips the bulk of the address
// space (so the 10s freeze does not come back) while covering a heap that has
// room to grow well past where it sits today.
#define HEAP_LO 0x10000000u
#define HEAP_HI 0x40000000u
static inline bool HeapRegion(const MEMORY_BASIC_INFORMATION& mbi) {
    unsigned base = (unsigned)(uintptr_t)mbi.BaseAddress;
    return base < HEAP_HI && (base + (unsigned)mbi.RegionSize) > HEAP_LO;
}

static void LogS(const char* s);   // defined below; used by the RTTI helpers

// Ask an object what class it is, at runtime, via MSVC RTTI:
//   vtable[-1] -> CompleteObjectLocator -> +0x0C TypeDescriptor -> +0x08 name
// Names arrive mangled (".?AVNPCTag@@"), so skip the ".?AV" and stop at '@'.
// Worth having permanently: the static RTTI dump has gaps, and guessing a
// class from a vtable address is how we ended up feeding the spawn routine a
// GeometryInst when it wanted something else entirely.
static bool RttiName(unsigned obj, char* out, int outLen) {
    if (outLen < 2) return false;
    out[0] = 0;
    __try {
        if (obj < HEAP_LO || obj > HEAP_HI) return false;
        unsigned vt = *(unsigned*)obj;
        unsigned base = (unsigned)(uintptr_t)GetModuleHandleA(NULL);
        if (vt < base || vt > base + 0x01000000) return false;
        unsigned col = *(unsigned*)(vt - 4);
        if (col < base || col > base + 0x01000000) return false;
        unsigned td = *(unsigned*)(col + 0x0C);
        if (td < base || td > base + 0x01000000) return false;
        const char* nm = (const char*)(td + 8);
        if (nm[0] == '.' && nm[1] == '?' && nm[2] == 'A') nm += 4;
        int i = 0;
        for (; i < outLen - 1 && nm[i] && nm[i] != '@'; i++) {
            char c = nm[i];
            if (c < 32 || c > 126) return false;
            out[i] = c;
        }
        out[i] = 0;
        return i > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; return false; }
}

// Every live object sharing a vtable. General-purpose companion to whatis:
// once one object of a class is known, this finds the rest.
int SWSE_FindByVtable(unsigned vt, unsigned* out, int maxOut) {
    int n = 0;
    SYSTEM_INFO si; GetSystemInfo(&si);
    BYTE* p  = (BYTE*)si.lpMinimumApplicationAddress;
    BYTE* hi = (BYTE*)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi;
    while (p < hi && n < maxOut) {
        if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
        bool ok = mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE
               && HeapRegion(mbi)
               && (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))
               && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS));
        if (ok) {
            __try {
                unsigned* q = (unsigned*)mbi.BaseAddress;
                size_t cnt = mbi.RegionSize / 4;
                for (size_t k = 0; k < cnt && n < maxOut; k++) {
                    if (q[k] != vt) continue;
                    unsigned a = (unsigned)(uintptr_t)(q + k);
                    if (a < HEAP_LO || a > HEAP_HI) continue;
                    out[n++] = a;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        p = (BYTE*)mbi.BaseAddress + mbi.RegionSize;
    }
    return n;
}

static unsigned g_instBuf[2048];

int SWSE_Instances(unsigned addr, char* msg, int msgLen) {
    char nm[96], tmp[200], b[160];
    unsigned vt = 0;
    __try { vt = *(unsigned*)addr; } __except (EXCEPTION_EXECUTE_HANDLER) {
        lstrcpynA(msg, "unreadable address", msgLen); return 0;
    }
    if (!RttiName(addr, nm, sizeof(nm))) lstrcpynA(nm, "?", 2);
    int n = SWSE_FindByVtable(vt, g_instBuf, 2048);
    for (int i = 0; i < n && i < 8; i++) {
        wsprintfA(b, "  %s %08X", nm, g_instBuf[i]);
        LogS(b);
    }
    wsprintfA(tmp, "%d live %s object(s)%s", n, nm, (n == 2048) ? " - AT THE CAP" : "");
    lstrcpynA(msg, tmp, msgLen);
    return n;
}


// Compare two live objects and report only the dwords that differ.
//
// Prefs told us what a character IS; they do not say who it FIGHTS -- every
// character in a town reads affiliation 1, yet outlaws attack and townsfolk
// flee. So hostility lives on the live NPC, not in prefs. Diffing an outlaw
// against a townsfolk is the way to find it: whatever separates "attacks the
// player" from "runs away" has to show up as a differing field.
int SWSE_DiffObjects(unsigned a, unsigned b, int len, char* msg, int msgLen) {
    char tmp[200], line[190];
    if (len <= 0 || len > 0x1000) len = 0x400;
    int diffs = 0, shown = 0;
    __try {
        for (int o = 0; o < len; o += 4) {
            unsigned va = *(unsigned*)(a + o);
            unsigned vb = *(unsigned*)(b + o);
            if (va == vb) continue;
            diffs++;
            if (shown < 40) {
                wsprintfA(line, "  +%03X  %08X | %08X", o, va, vb);
                LogS(line);
                shown++;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        lstrcpynA(msg, "unreadable - one of those addresses is not live", msgLen);
        return -2;
    }
    wsprintfA(tmp, "%08X vs %08X: %d of %d dwords differ (first %d logged)",
              a, b, diffs, len / 4, shown);
    lstrcpynA(msg, tmp, msgLen);
    return diffs;
}

int SWSE_WhatIs(unsigned addr, char* msg, int msgLen) {
    char nm[96], tmp[200];
    unsigned vt = 0;
    __try { vt = *(unsigned*)addr; } __except (EXCEPTION_EXECUTE_HANDLER) {
        lstrcpynA(msg, "unreadable address", msgLen); return 0;
    }
    if (RttiName(addr, nm, sizeof(nm)))
        wsprintfA(tmp, "%08X is a %s (vtable %08X, module+0x%X)", addr, nm, vt,
                  vt - (unsigned)(uintptr_t)GetModuleHandleA(NULL));
    else
        wsprintfA(tmp, "%08X: no RTTI (vtable %08X) - probably not a live object",
                  addr, vt);
    lstrcpynA(msg, tmp, msgLen);
    return 1;
}

// ---- handler RVAs (image base 0x400000; addresses from script_handlers.tsv) ----
#define RVA_GiveAmmo                 0x1610F0
#define RVA_GiveAllAmmo              0x160E30
#define RVA_GiveDefaultAmmo          0x160E90
#define RVA_TakeAllAmmo             0x160B90
#define RVA_GiveCrossbow            0x161770
#define RVA_TakeAllWeapons          0x160A80
#define RVA_MakePlayerSteef         0x14D2F0
#define RVA_MakePlayerStranger      0x14D390
#define RVA_SetSteefNaked           0x14D820
#define RVA_SetToMaxHealth          0x1552F0
#define RVA_SetToMaxStamina         0x1554A0
#define RVA_SetToMaxHealthAndStam   0x155260
#define RVA_SetHealth               0x155290
#define RVA_Kill                    0x155380
#define RVA_TeleportHome            0x14E2C0
#define RVA_TeleportReset           0x14E200
#define RVA_ForceFPS                0x158470
#define RVA_ForceNotFPS             0x158490
#define RVA_ForceSniper             0x1584B0
#define RVA_GiveArtifact            0x163560
#define RVA_TakeAllArtifacts        0x1634D0
#define RVA_QuickSave               0x14D8A0
#define RVA_Checkpoint              0x14D860
#define RVA_LoadLastSave            0x1622A0
#define RVA_ShowHealthBars          0x158710
#define RVA_OpenWeaponHUD           0x1622F0
#define RVA_SetMoolah               0x163140
#define RVA_GetMoolah               0x1630E0
// music (all zero-arg except Push/Transition which take EMusicType)
#define RVA_EnableCombatMusic       0x1566D0
#define RVA_DisableCombatMusic      0x156710
#define RVA_EnableTensionMusic      0x156650
#define RVA_DisableTensionMusic     0x156690
#define RVA_PopMusic                0x156590
#define RVA_PushMusic               0x1564E0
#define RVA_TransitionMusic         0x1565D0

typedef void (__cdecl* fn1_t)(void* ctx);
typedef void (__cdecl* fn2_t)(void* ctx, int a);

static BYTE*  g_base = nullptr;
static void*  g_ctx  = nullptr;     // captured live ScriptContext
static bool   g_inited = false;

static void* Addr(unsigned rva) { return g_base + rva; }

static void LogS(const char* s) {
    char path[MAX_PATH]; GetModuleFileNameA(GetModuleHandleA(NULL), path, MAX_PATH);
    char* sl = strrchr(path, '\\'); if (sl) *sl = 0;
    char full[MAX_PATH]; wsprintfA(full, "%s\\swse_log.txt", path);
    HANDLE h = CreateFileA(full, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wr; char line[200]; int n = wsprintfA(line, "%s\r\n", s);
        SetFilePointer(h, 0, NULL, FILE_END); WriteFile(h, line, n, &wr, NULL); CloseHandle(h);
    }
}

// ==========================================================================
//  POINTER CHAINS â€” the Cheat-Engine-independent path to any value.
//
//  A CE pointer chain like:
//      "stranger.exe"+0065178C  ->  +0 -> +54 -> +620 -> +1F8 -> +F4 -> +1C -> +70
//  is fully resolvable from inside the process: read the static global at
//  moduleBase+RVA, then walk each offset (deref every hop except the last,
//  which is a plain add). No script context, no ammo priming, no CE.
//
//  Chains live in SWSEMods\SWSE Console\pointers.txt so new ones can
//  be added from CE findings WITHOUT rebuilding the DLL:
//      health = f, 65178C, 0, 54, 620, 1F8, F4, 1C, 70
//      (name  = type(f|i), baseRVA, offsets...  â€” all hex except the type)
// ==========================================================================
#define MAX_CHAIN_OFFSETS 12
#define MAX_CHAINS        64

struct PtrChain {
    char     name[32];
    char     type;                        // 'f' float, 'i' int
    unsigned baseRVA;
    unsigned offsets[MAX_CHAIN_OFFSETS];
    int      nOffsets;
    bool     frozen;
    float    frozenF;
    int      frozenI;
};
static PtrChain g_chains[MAX_CHAINS];
static int      g_chainCount = 0;

// Resolve a chain to its final address. Returns nullptr on any bad hop.
// `trace` (optional) receives a human-readable resolution log.
static void* ResolveChain(const PtrChain& c, char* trace, int traceLen) {
    __try {
        unsigned addr = *(unsigned*)(g_base + c.baseRVA);
        if (trace) {
            char b[96];
            wsprintfA(b, "  base+0x%X -> %08X", c.baseRVA, addr);
            lstrcpynA(trace, b, traceLen);
        }
        for (int i = 0; i < c.nOffsets; i++) {
            if (addr < 0x10000 || addr >= 0x7F000000) return nullptr;
            if (i == c.nOffsets - 1) {
                addr = addr + c.offsets[i];       // last hop: add only, no deref
                if (trace) {
                    char b[64]; wsprintfA(b, " +0x%X = %08X", c.offsets[i], addr);
                    lstrcatA(trace, b);
                }
            } else {
                unsigned next = *(unsigned*)(addr + c.offsets[i]);
                if (trace) {
                    char b[64]; wsprintfA(b, " [+0x%X]->%08X", c.offsets[i], next);
                    lstrcatA(trace, b);
                }
                addr = next;
            }
        }
        if (addr < 0x10000 || addr >= 0x7F000000) return nullptr;
        return (void*)addr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

static void PointersPath(char* out) {
    char exe[MAX_PATH]; GetModuleFileNameA(GetModuleHandleA(NULL), exe, MAX_PATH);
    char* sl = strrchr(exe, '\\'); if (sl) *sl = 0;      // ...\bin
    sl = strrchr(exe, '\\'); if (sl) *sl = 0;            // game root
    wsprintfA(out, "%s\\SWSEMods\\SWSE Console\\pointers.txt", exe);
}

int SWSE_PtrLoad() {
    g_chainCount = 0;
    char path[MAX_PATH]; PointersPath(path);
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    char buf[8192]; DWORD n = 0;
    ReadFile(f, buf, sizeof(buf) - 1, &n, NULL); CloseHandle(f);
    buf[n] = 0;

    char* line = strtok(buf, "\r\n");
    while (line && g_chainCount < MAX_CHAINS) {
        while (*line == ' ' || *line == '\t') line++;
        if (*line && *line != '#') {
            char* eq = strchr(line, '=');
            if (eq) {
                *eq = 0;
                PtrChain& c = g_chains[g_chainCount];
                // trim trailing space off the name
                lstrcpynA(c.name, line, 32);
                for (int k = lstrlenA(c.name) - 1; k >= 0 && (c.name[k]==' '||c.name[k]=='\t'); k--)
                    c.name[k] = 0;
                c.nOffsets = 0; c.frozen = false; c.type = 'i';
                // parse: type, baseRVA, offsets...
                char* tok = strtok(eq + 1, ", \t");
                int field = 0;
                bool ok = true;
                while (tok) {
                    if (field == 0) {
                        c.type = (tok[0] == 'f' || tok[0] == 'F') ? 'f' : 'i';
                    } else if (field == 1) {
                        c.baseRVA = strtoul(tok, nullptr, 16);
                        if (!c.baseRVA) ok = false;
                    } else if (c.nOffsets < MAX_CHAIN_OFFSETS) {
                        c.offsets[c.nOffsets++] = strtoul(tok, nullptr, 16);
                    }
                    field++;
                    tok = strtok(nullptr, ", \t");
                }
                if (ok && c.name[0] && c.nOffsets > 0) g_chainCount++;
            }
        }
        line = strtok(nullptr, "\r\n");
    }
    char msg[80]; wsprintfA(msg, "ptr: loaded %d pointer chain(s)", g_chainCount);
    LogS(msg);
    return g_chainCount;
}

int SWSE_PtrCount() { return g_chainCount; }

const char* SWSE_PtrName(int i) {
    return (i >= 0 && i < g_chainCount) ? g_chains[i].name : nullptr;
}

// Read a chain's current value. ok=false if it can't resolve right now.
bool SWSE_PtrRead(int i, float* fOut, int* iOut, char* trace, int traceLen) {
    if (i < 0 || i >= g_chainCount) return false;
    void* p = ResolveChain(g_chains[i], trace, traceLen);
    if (!p) return false;
    __try {
        if (g_chains[i].type == 'f') { if (fOut) memcpy(fOut, p, 4); }
        else                          { if (iOut) memcpy(iOut, p, 4); }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool SWSE_PtrWrite(int i, float fVal, int iVal) {
    if (i < 0 || i >= g_chainCount) return false;
    void* p = ResolveChain(g_chains[i], nullptr, 0);
    if (!p) return false;
    __try {
        if (g_chains[i].type == 'f') memcpy(p, &fVal, 4);
        else                         memcpy(p, &iVal, 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void* SWSE_PtrResolve(int i) {
    if (i < 0 || i >= g_chainCount) return nullptr;
    return ResolveChain(g_chains[i], nullptr, 0);
}

int SWSE_PtrFind(const char* name) {
    for (int i = 0; i < g_chainCount; i++)
        if (!lstrcmpiA(name, g_chains[i].name)) return i;
    return -1;
}

bool SWSE_PtrSetFrozen(int i, bool on, float fVal, int iVal) {
    if (i < 0 || i >= g_chainCount) return false;
    g_chains[i].frozen = on;
    g_chains[i].frozenF = fVal;
    g_chains[i].frozenI = iVal;
    return true;
}

char SWSE_PtrType(int i) {
    return (i >= 0 && i < g_chainCount) ? g_chains[i].type : 'i';
}

// called every frame â€” reapply frozen chain values
static void ApplyFrozenChains() {
    for (int i = 0; i < g_chainCount; i++) {
        if (!g_chains[i].frozen) continue;
        void* p = ResolveChain(g_chains[i], nullptr, 0);
        if (!p) continue;
        __try {
            if (g_chains[i].type == 'f') memcpy(p, &g_chains[i].frozenF, 4);
            else                         memcpy(p, &g_chains[i].frozenI, 4);
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_chains[i].frozen = false; }
    }
}

// ==========================================================================
//  HEAP DIFF SCANNER â€” find the artifact inventory.
//
//  Store purchases never reach the script VM, so no script call can grant an
//  artifact. The inventory has to be written directly. This snapshots every
//  writable private heap region, then reports the dwords that changed after
//  you buy one â€” which is the artifact flag.
// ==========================================================================
struct ScanRegion { unsigned char* base; size_t size; unsigned char* copy; };
static ScanRegion g_scan[512];
static int    g_scanN = 0;
static size_t g_scanBytes = 0;

static void ScanFree() {
    for (int i = 0; i < g_scanN; i++) free(g_scan[i].copy);
    g_scanN = 0; g_scanBytes = 0;
}

// Snapshot committed, writable, private memory (where game state lives).
int SWSE_ScanStart() {
    ScanFree();
    SYSTEM_INFO si; GetSystemInfo(&si);
    unsigned char* p = (unsigned char*)si.lpMinimumApplicationAddress;
    unsigned char* hi = (unsigned char*)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi;
    while (p < hi && g_scanN < 512 && g_scanBytes < (192u << 20)) {
        if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
        bool ok = mbi.State == MEM_COMMIT
               && mbi.Type == MEM_PRIVATE
               && (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))
               && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))
               && mbi.RegionSize >= 0x1000 && mbi.RegionSize <= (32u << 20);
        if (ok) {
            unsigned char* c = (unsigned char*)malloc(mbi.RegionSize);
            if (c) {
                __try {
                    memcpy(c, mbi.BaseAddress, mbi.RegionSize);
                    g_scan[g_scanN].base = (unsigned char*)mbi.BaseAddress;
                    g_scan[g_scanN].size = mbi.RegionSize;
                    g_scan[g_scanN].copy = c;
                    g_scanN++;
                    g_scanBytes += mbi.RegionSize;
                } __except (EXCEPTION_EXECUTE_HANDLER) { free(c); }
            }
        }
        p = (unsigned char*)mbi.BaseAddress + mbi.RegionSize;
    }
    char b[128];
    wsprintfA(b, "scan: snapshot %d regions, %u KB", g_scanN, (unsigned)(g_scanBytes >> 10));
    LogS(b);
    return g_scanN;
}

// Candidate list, so successive passes can NARROW instead of restarting.
static unsigned g_cand[4096];
static unsigned g_candVal[4096];
static int      g_candN = 0;

// Second and later passes: keep only candidates that changed AGAIN (buy
// another artifact) â€” this is what eliminates timers and frame counters.
int SWSE_ScanRefine(int mode) {
    if (!g_candN) { LogS("scan: no candidates yet â€” run scandiff first"); return 0; }
    int kept = 0;
    LogS(mode ? "==== refine: kept (changed again) ===="
              : "==== refine: kept (held steady) ====");
    for (int i = 0; i < g_candN; i++) {
        unsigned a = g_cand[i];
        __try {
            unsigned now = *(unsigned*)a;
            bool changed = (now != g_candVal[i]);
            if (changed == (mode != 0) && now <= 64) {
                char ln[96];
                wsprintfA(ln, "  %08X : %u -> %u", a, g_candVal[i], now);
                LogS(ln);
                g_cand[kept] = a; g_candVal[kept] = now; kept++;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    g_candN = kept;
    char b[80]; wsprintfA(b, "==== refine: %d remaining ====", kept);
    LogS(b);
    return kept;
}

// Find values CLEARED to zero since the snapshot. Much cleaner than hunting
// 0->1 flips: run 'scan' while holding artifacts, then takeallartifacts,
// then this. That function touches only the artifact inventory, so whatever
// it zeroes IS the inventory.
int SWSE_ScanCleared(int maxReport) {
    if (!g_scanN) { LogS("scan: no snapshot â€” run 'scan' first"); return 0; }
    LogS("==== scan cleared: values wiped to 0 (the inventory) ====");
    g_candN = 0;
    int found = 0;
    for (int i = 0; i < g_scanN && found < maxReport; i++) {
        unsigned char* base = g_scan[i].base;
        unsigned* now = (unsigned*)base;
        unsigned* was = (unsigned*)g_scan[i].copy;
        size_t cnt = g_scan[i].size / 4;
        __try {
            for (size_t k = 0; k < cnt && found < maxReport; k++) {
                unsigned a = was[k], b2 = now[k];
                if (b2 != 0 || a == 0 || a > 64) continue;   // nonzero -> 0
                unsigned addr = (unsigned)(uintptr_t)(base + k * 4);
                char ln[128];
                wsprintfA(ln, "  %08X : %u -> 0", addr, a);
                LogS(ln);
                if (g_candN < 4096) { g_cand[g_candN] = addr; g_candVal[g_candN] = 0; g_candN++; }
                found++;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    char b[96];
    wsprintfA(b, "==== scan cleared: %d candidate(s) ====", found);
    LogS(b);
    return found;
}

// Write `value` to every tracked candidate at once â€” if the candidate list is
// the artifact inventory, this grants everything in one go.
int SWSE_ScanPokeAll(int value) {
    int ok = 0;
    for (int i = 0; i < g_candN; i++) {
        __try { *(int*)g_cand[i] = value; ok++; }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    char b[80]; wsprintfA(b, "scan: wrote %d to %d candidate(s)", value, ok);
    LogS(b);
    return ok;
}

// Report dwords that changed to a small value (an ownership flag going 0->1,
// or a count incrementing). Logs address + old/new so we can poke it.
int SWSE_ScanDiff(int maxReport) {
    if (!g_scanN) { LogS("scan: no snapshot â€” run 'scan' first"); return 0; }
    LogS("==== scan diff: candidate inventory flags (0->1 first) ====");
    g_candN = 0;
    int found = 0;
    // Two passes: exact 0->1 transitions first (what an ownership flag does),
    // then other small increases. Counters/timers mostly land in pass 2.
    for (int pass = 0; pass < 2 && found < maxReport; pass++) {
        if (pass == 1) LogS("  --- other small increases (likely counters) ---");
        for (int i = 0; i < g_scanN && found < maxReport; i++) {
            unsigned char* base = g_scan[i].base;
            unsigned* now = (unsigned*)base;
            unsigned* was = (unsigned*)g_scan[i].copy;
            size_t cnt = g_scan[i].size / 4;
            __try {
                for (size_t k = 0; k < cnt && found < maxReport; k++) {
                    unsigned a = was[k], b2 = now[k];
                    if (a == b2) continue;
                    bool isFlag = (a == 0 && b2 == 1);
                    if (pass == 0 ? !isFlag : (isFlag || !(a <= 8 && b2 <= 8 && b2 > a)))
                        continue;
                    unsigned addr = (unsigned)(uintptr_t)(base + k * 4);
                    char ln[128];
                    wsprintfA(ln, "  %08X : %u -> %u", addr, a, b2);
                    LogS(ln);
                    if (g_candN < 4096) { g_cand[g_candN] = addr; g_candVal[g_candN] = b2; g_candN++; }
                    found++;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }
    char b[96];
    wsprintfA(b, "==== scan diff: %d candidate(s) tracked ====", found);
    LogS(b);
    return found;
}

// Write a raw address (for testing a candidate found by the scanner).
int SWSE_PokeAddr(unsigned addr, int value) {
    if (addr < 0x10000 || addr >= 0x7F000000) return 0;
    __try { *(int*)addr = value; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -2; }
}

// ==========================================================================
//  INVENTORY LOCATOR â€” ask the game where its artifact data is.
//
//  TakeAllArtifacts (0x5634D0) doesn't use a global; it calls 0x4588A0 twice
//  to obtain an object, then does  cmp [eax+8],0 / mov eax,[eax] / mov ecx,[eax].
//  Since SWSE lives in the process we can call that same getter and walk the
//  structure it returns â€” no memory scanning, no guessing.
// ==========================================================================
#define RVA_InvGetter 0x0588A0

typedef void* (__cdecl* getter_t)();

static void DumpBlock(const char* tag, unsigned addr, int dwords) {
    if (addr < 0x10000 || addr >= 0x7F000000) {
        char b[96]; wsprintfA(b, "  %s %08X : not a readable pointer", tag, addr);
        LogS(b); return;
    }
    char b[160];
    wsprintfA(b, "  --- %s @ %08X ---", tag, addr);
    LogS(b);
    __try {
        unsigned* p = (unsigned*)addr;
        for (int i = 0; i < dwords; i++) {
            float f; memcpy(&f, &p[i], 4);
            char fs[32] = "";
            if (f > 0.0001f && f < 1e7f) wsprintfA(fs, " f=%d", (int)f);
            wsprintfA(b, "    +0x%02X = %08X (%d)%s", i * 4, p[i], (int)p[i], fs);
            LogS(b);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { LogS("    (unreadable)"); }
}

// Snapshot the player object so successive invdump runs can be diffed. The
// artifact state is somewhere in here; comparing "with artifact" against
// "without" isolates it without scanning the whole heap.
#define PLAYER_DWORDS 512
static unsigned g_playerSnap[PLAYER_DWORDS];
static unsigned g_playerBase = 0;
static bool     g_havePlayerSnap = false;

void SWSE_PlayerSnapshot(unsigned base) {
    __try {
        unsigned* p = (unsigned*)base;
        if (g_havePlayerSnap && g_playerBase == base) {
            LogS("  --- player object CHANGES since last invdump ---");
            int n = 0;
            for (int i = 0; i < PLAYER_DWORDS; i++) {
                if (p[i] == g_playerSnap[i]) continue;
                // skip the position/velocity floats that always drift
                float f; memcpy(&f, &p[i], 4);
                if (f > 1e-6f && f < 1e7f && (i * 4 >= 0x24 && i * 4 <= 0x30)) continue;
                char b[128];
                wsprintfA(b, "    +0x%03X : %08X -> %08X   (%d -> %d)",
                          i * 4, g_playerSnap[i], p[i], (int)g_playerSnap[i], (int)p[i]);
                LogS(b);
                if (++n >= 60) { LogS("    ...(truncated)"); break; }
            }
            if (!n) LogS("    (nothing changed)");
        } else {
            LogS("  --- player snapshot taken; run invdump again after the"
                 " artifact changes to see the diff ---");
        }
        memcpy(g_playerSnap, p, sizeof(g_playerSnap));
        g_playerBase = base;
        g_havePlayerSnap = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogS("  (player snapshot faulted)");
    }
}

// Call the getter TakeAllArtifacts uses and dump the object graph it returns.
int SWSE_DumpInventory() {
    __try {
        void* o = ((getter_t)Addr(RVA_InvGetter))();
        unsigned a = (unsigned)(uintptr_t)o;
        char b[128];
        wsprintfA(b, "==== inventory getter 0x4588A0 returned %08X ====", a);
        LogS(b);
        if (!a) { LogS("  null â€” no inventory object right now"); return 0; }
        DumpBlock("object", a, 12);
        // TakeAllArtifacts does: mov eax,[eax]; mov ecx,[eax]
        __try {
            unsigned lvl1 = *(unsigned*)a;
            DumpBlock("[obj]", lvl1, 12);
            // [[obj]] is the PLAYER object: +0x24/+0x28/+0x2C read as a
            // position vector. Dump it wide â€” artifact state is likely deeper
            // in this struct than the first 24 dwords.
            unsigned lvl2 = *(unsigned*)lvl1;
            DumpBlock("[[obj]] PLAYER", lvl2, 160);
            // NOTE: +0x1C was briefly suspected to be the inventory (it went
            // null -> pointer when an artifact was acquired) but a later run
            // with the same artifact read null again â€” it's transient state,
            // not the inventory. Left unfollowed deliberately.
            //
            // Instead: snapshot the player object so two runs (with and
            // without an artifact) can be diffed automatically.
            SWSE_PlayerSnapshot(lvl2);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        LogS("==== end inventory dump ====");
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogS("inventory getter FAULTED");
        return -2;
    }
}

// Dump arbitrary memory â€” for walking the inventory structure by hand.
int SWSE_DumpAddr(unsigned addr, int dwords) {
    if (addr < 0x10000 || addr >= 0x7F000000) return 0;
    if (dwords < 1) dwords = 16;
    if (dwords > 128) dwords = 128;
    DumpBlock("addr", addr, dwords);
    return 1;
}

// ==========================================================================
//  SCRIPT SPY â€” trace the game calling its OWN script functions.
//
//  Rather than guessing how to invoke a handler, watch the interpreter do it.
//  Each traced function gets a generated stub:
//        pushad; pushfd; push esp; push index; call SpyLog; add esp,8;
//        popfd; popad; jmp trampoline
//  SpyLog receives a pointer to the saved registers, from which the ORIGINAL
//  stack is at +36 â€” so we can read retBuf/ctx/args exactly as the caller
//  passed them, and pull the VM's own arguments via ctx->vtable[0x74](n).
//  Nothing is modified; the original bytes run from the trampoline.
// ==========================================================================
#define SPY_MAX 64
struct SpyEntry {
    unsigned rva;
    const char* name;
    BYTE*    tramp;
    BYTE*    stub;
    unsigned hits;
};
static SpyEntry g_spy[SPY_MAX];
static int  g_spyN = 0;
static bool g_spyOn = false;
static int  g_spyBudget = 0;      // lines left to log (avoid flooding)

// Read one VM argument through the context's accessor, formatting what we can.
static void SpyDescribeArg(void* ctx, int idx, char* out, int outLen) {
    out[0] = 0;
    __try {
        unsigned* vt = *(unsigned**)ctx;
        typedef void* (__thiscall* getarg_t)(void*, int);
        void* v = ((getarg_t)vt[0x74 / 4])(ctx, idx);
        if (!v) { lstrcpynA(out, "(null)", outLen); return; }
        unsigned* w = (unsigned*)v;
        unsigned payload = w[4], type = w[5];      // +0x10 payload, +0x14 type
        char txt[64] = "";
        // if the payload looks like a string pointer, show the text
        if (payload > 0x10000 && payload < 0x7F000000) {
            __try {
                const char* s = (const char*)payload;
                int k = 0;
                while (k < 48 && s[k] >= 0x20 && s[k] < 0x7F) { txt[k] = s[k]; k++; }
                txt[k] = 0;
                if (k < 3) txt[0] = 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) { txt[0] = 0; }
        }
        if (txt[0]) wsprintfA(out, "type=%u val=%08X \"%s\"", type, payload, txt);
        else        wsprintfA(out, "type=%u val=%08X (%d)", type, payload, (int)payload);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        lstrcpynA(out, "(unreadable)", outLen);
    }
}

// Called from every stub. savedRegs points at the pushfd slot; the original
// stack begins 36 bytes above it (32 pushad + 4 pushfd).
extern "C" void __cdecl SpyLog(int idx, unsigned* savedRegs) {
    if (!g_spyOn || idx < 0 || idx >= g_spyN) return;
    g_spy[idx].hits++;
    if (g_spyBudget <= 0) return;
    g_spyBudget--;
    __try {
        unsigned* orig = (unsigned*)((BYTE*)savedRegs + 36);
        unsigned retBuf = orig[1];     // arg0
        unsigned ctx    = orig[2];     // arg1
        char b[220];
        wsprintfA(b, "SPY %-28s ctx=%08X ret=%08X", g_spy[idx].name, ctx, retBuf);
        LogS(b);
        if (ctx > 0x10000 && ctx < 0x7F000000) {
            for (int a = 0; a < 3; a++) {
                char d[160];
                SpyDescribeArg((void*)ctx, a, d, 160);
                if (!d[0] || !lstrcmpA(d, "(null)")) break;
                wsprintfA(b, "      arg%d: %s", a, d);
                LogS(b);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Build the stub + trampoline for one function.
static bool SpyInstall(unsigned rva, const char* name, int copyLen) {
    if (g_spyN >= SPY_MAX) return false;
    BYTE* fn = (BYTE*)Addr(rva);
    int idx = g_spyN;
    BYTE* tramp = (BYTE*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE,
                                      PAGE_EXECUTE_READWRITE);
    BYTE* stub  = (BYTE*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE,
                                      PAGE_EXECUTE_READWRITE);
    if (!tramp || !stub) return false;

    memcpy(tramp, fn, copyLen);
    tramp[copyLen] = 0xE9;
    *(DWORD*)(tramp + copyLen + 1) = (DWORD)((fn + copyLen) - (tramp + copyLen + 5));

    BYTE* p = stub;
    *p++ = 0x60;                                   // pushad
    *p++ = 0x9C;                                   // pushfd
    *p++ = 0x54;                                   // push esp
    *p++ = 0x68; *(DWORD*)p = (DWORD)idx; p += 4;  // push idx
    *p++ = 0xE8;                                   // call SpyLog
    *(DWORD*)p = (DWORD)((BYTE*)&SpyLog - (p + 4)); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x08;         // add esp,8
    *p++ = 0x9D;                                   // popfd
    *p++ = 0x61;                                   // popad
    *p++ = 0xE9;                                   // jmp tramp
    *(DWORD*)p = (DWORD)(tramp - (p + 4)); p += 4;

    DWORD old;
    VirtualProtect(fn, copyLen, PAGE_EXECUTE_READWRITE, &old);
    fn[0] = 0xE9;
    *(DWORD*)(fn + 1) = (DWORD)(stub - (fn + 5));
    for (int i = 5; i < copyLen; i++) fn[i] = 0x90;   // pad sliced bytes
    VirtualProtect(fn, copyLen, old, &old);

    g_spy[idx].rva = rva; g_spy[idx].name = name;
    g_spy[idx].tramp = tramp; g_spy[idx].stub = stub; g_spy[idx].hits = 0;
    g_spyN++;
    return true;
}

// Functions worth watching, with the instruction-aligned prologue length of
// each (5 unless the 5-byte boundary would slice an instruction).
struct SpyTarget { unsigned rva; const char* name; int len; };
static const SpyTarget kSpyTargets[] = {
    { 0x163560, "GiveArtifact",      6 },   // 55|8bec|83e4f8
    { 0x163350, "TakeArtifact",      5 },
    { 0x163210, "HasArtifactCount",  5 },
    { 0x1634D0, "TakeAllArtifacts",  5 },
    { 0x168E50, "ArtifactPref",      5 },
    { 0x1610F0, "GiveAmmo",          5 },
    { 0x160E30, "GiveAllAmmo",       5 },
    { 0x163140, "SetMoolah",         5 },
    { 0x1630E0, "GetMoolah",         5 },
    { 0x162A50, "GeneralStore_SetItemQuantity", 5 },
};

int SWSE_SpyStart(int budget) {
    if (g_spyN == 0) {
        for (auto& t : kSpyTargets) SpyInstall(t.rva, t.name, t.len);
        char b[96]; wsprintfA(b, "spy: installed %d hooks", g_spyN); LogS(b);
    }
    g_spyBudget = budget > 0 ? budget : 200;
    g_spyOn = true;
    LogS("==== SPY ON â€” play normally; every script call is logged ====");
    return g_spyN;
}

void SWSE_SpyStop() {
    g_spyOn = false;
    LogS("==== SPY OFF â€” hit counts ====");
    for (int i = 0; i < g_spyN; i++) {
        char b[128];
        wsprintfA(b, "  %-28s %u call(s)", g_spy[i].name, g_spy[i].hits);
        LogS(b);
    }
}

// ==========================================================================
//  HARDWARE WATCHPOINT â€” "find out what writes to this address".
//
//  Store purchases bypass the script VM entirely (spy proved it: 0 calls),
//  so the code that adds an artifact is plain C++ we haven't located. A debug
//  register watchpoint on the inventory head catches it: when the game writes
//  there, the CPU raises a single-step exception and our vectored handler
//  logs the instruction that did it.
// ==========================================================================
static unsigned g_watchAddr = 0;
static PVOID    g_vehHandle = nullptr;
// One-shot mode. Some targets execute for EVERY character EVERY frame - the
// Granny pose builder does - and a breakpoint that keeps firing floods the
// handler faster than the console can answer, wedging the game hard enough to
// need a restart. With this set, the handler captures once and immediately
// disarms itself from inside the trapped CONTEXT.
static volatile LONG g_watchOnce = 0;
static int      g_watchHits = 0;

static LONG CALLBACK WatchVEH(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP && g_watchAddr) {
        unsigned eip = ep->ContextRecord->Eip;
        unsigned base = (unsigned)g_base;
        char b[180];
        wsprintfA(b, "WATCH HIT: code at %08X (module+0x%X) wrote to %08X",
                  eip, eip - base, g_watchAddr);
        LogS(b);
        __try {
            unsigned v = *(unsigned*)g_watchAddr;
            // All GP registers: a copy loop's SOURCE is usually in edi/ebx, and
            // logging only eax/ecx/edx/esi cost us a rebuild when the player
            // position turned out to be copied from [edi].
            wsprintfA(b, "   new value = %08X    eax=%08X ecx=%08X edx=%08X ebx=%08X",
                      v, ep->ContextRecord->Eax, ep->ContextRecord->Ecx,
                      ep->ContextRecord->Edx, ep->ContextRecord->Ebx);
            LogS(b);
            wsprintfA(b, "   esi=%08X edi=%08X ebp=%08X esp=%08X",
                      ep->ContextRecord->Esi, ep->ContextRecord->Edi,
                      ep->ContextRecord->Ebp, ep->ContextRecord->Esp);
            LogS(b);
            // Stack at the break. On an execution breakpoint placed at a
            // function's entry this is [retaddr, arg0, arg1, ...] -- which is
            // how we recover a factory's signature from a real call instead of
            // guessing it.
            unsigned* sp = (unsigned*)ep->ContextRecord->Esp;
            wsprintfA(b, "   stack: ret=%08X a0=%08X a1=%08X a2=%08X a3=%08X",
                      sp[0], sp[1], sp[2], sp[3], sp[4]);
            LogS(b);
            wsprintfA(b, "          a4=%08X a5=%08X a6=%08X a7=%08X a8=%08X",
                      sp[5], sp[6], sp[7], sp[8], sp[9]);
            LogS(b);
            // a few bytes of the writing instruction, for identification
            BYTE* p = (BYTE*)(eip - 8);
            wsprintfA(b, "   bytes around EIP: %02X %02X %02X %02X %02X %02X %02X %02X | %02X %02X %02X %02X",
                      p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],p[8],p[9],p[10],p[11]);
            LogS(b);

            // THE OBJECT ITSELF, dumped here rather than later. On a thiscall
            // ecx is 'this', and that is the thing worth seeing - but by the
            // time the console can peek at it the allocator has usually reused
            // the memory (a MoveBoltRayReq capture came back as a free list).
            // Dumping inside the trap is the only way to see it as it was.
            unsigned self = ep->ContextRecord->Ecx;
            if (self >= 0x10000 && self < 0x7F000000) {
                LogS("   [ecx] object dump (hex | float):");
                for (int row = 0; row < 8; row++) {
                    unsigned* d = (unsigned*)(self + row * 16);
                    float*    f = (float*)d;
                    char line[200];
                    int used = wsprintfA(line, "     +%02X: %08X %08X %08X %08X  ",
                                         row * 16, d[0], d[1], d[2], d[3]);
                    for (int k = 0; k < 4; k++) {
                        float v = f[k];
                        // Only print plausible world/local magnitudes; pointers
                        // read as floats are noise.
                        if (v > -100000.0f && v < 100000.0f &&
                            (v == 0.0f || v > 1e-6f || v < -1e-6f))
                            used += wsprintfA(line + used, "%d.%03d ", (int)v,
                                              (int)((v < 0 ? -v : v) * 1000) % 1000);
                        else used += wsprintfA(line + used, "- ");
                    }
                    LogS(line);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        g_watchHits++;
        ep->ContextRecord->Dr6 = 0;          // clear the debug status
        // ONE-SHOT: disarm from inside the trapped context. Writing Dr0/Dr7
        // here is applied by the OS when execution resumes, so the breakpoint
        // is gone before the next character is posed. Clearing g_watchAddr also
        // stops this handler doing any further work.
        if (g_watchOnce) {
            ep->ContextRecord->Dr0 = 0;
            ep->ContextRecord->Dr7 &= ~0xFu;
            g_watchAddr = 0;
            InterlockedExchange(&g_watchOnce, 0);
            LogS("WATCH: one-shot capture complete, breakpoint disarmed");
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Arm a 4-byte write watchpoint on `addr` for the calling thread.
// Debug registers are PER-THREAD. Arming only the console's thread is why an
// execution breakpoint on the spawner tick and a read watch on the critter
// enable byte both reported zero hits: that code runs on another thread. The
// position writer happened to share our thread, which made the tooling look
// fine. Arm every thread in the process instead.
// dr7 carries the RW/LEN bits already positioned for slot 0.
static int ArmAllThreads(unsigned addr, unsigned rwLen) {
    int armed = 0;
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te; te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            HANDLE th = (te.th32ThreadID == me)
                ? GetCurrentThread()
                : OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
                             THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
            if (!th) continue;
            bool other = (te.th32ThreadID != me);
            if (other) SuspendThread(th);
            CONTEXT c; memset(&c, 0, sizeof(c));
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(th, &c)) {
                c.Dr0 = addr;
                c.Dr7 = (c.Dr7 & ~0xF0000u) | (1u << 0) | rwLen;
                c.Dr6 = 0;
                if (SetThreadContext(th, &c)) armed++;
            }
            if (other) { ResumeThread(th); CloseHandle(th); }
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return armed;
}

static void DisarmAllThreads() {
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te; te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            HANDLE th = (te.th32ThreadID == me)
                ? GetCurrentThread()
                : OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
                             THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
            if (!th) continue;
            bool other = (te.th32ThreadID != me);
            if (other) SuspendThread(th);
            CONTEXT c; memset(&c, 0, sizeof(c));
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(th, &c)) {
                c.Dr0 = 0; c.Dr7 &= ~1u; c.Dr6 = 0;
                SetThreadContext(th, &c);
            }
            if (other) { ResumeThread(th); CloseHandle(th); }
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

int SWSE_WatchWrite(unsigned addr) {
    if (addr < 0x10000 || addr >= 0x7F000000) return 0;
    if (!g_vehHandle) g_vehHandle = AddVectoredExceptionHandler(1, WatchVEH);
    g_watchAddr = addr; g_watchHits = 0;

    // RW0 = 01 (write), LEN0 = 11 (4 bytes)
    int n = ArmAllThreads(addr, (0x1u << 16) | (0x3u << 18));
    char b[120];
    wsprintfA(b, "watch: armed on %08X (write, 4 bytes) across %d thread(s)", addr, n);
    LogS(b);
    return n ? 1 : -1;
}

// Execution breakpoint on a module RVA: "is this code ever reached?".
// Same debug register, different DR7 encoding - RW=00 and LEN=00 mean execute.
// Reading a function and reasoning about which branch it takes is how I got
// the spawner wrong four times; this answers it by observation instead.
int SWSE_WatchExec(unsigned rva, int once) {
    unsigned addr = (unsigned)(uintptr_t)GetModuleHandleA(NULL) + rva;
    InterlockedExchange(&g_watchOnce, once ? 1 : 0);
    if (!g_vehHandle) g_vehHandle = AddVectoredExceptionHandler(1, WatchVEH);
    g_watchAddr = addr; g_watchHits = 0;

    int n = ArmAllThreads(addr, 0);            // RW0=00 (exec), LEN0=00
    char b[120];
    wsprintfA(b, "watchexec: armed on module+0x%X (%08X) across %d thread(s)", rva, addr, n);
    LogS(b);
    return n ? 1 : -1;
}

// Read-or-write watchpoint (DR7 RW=11). "What code READS this field?" is the
// question that finds callers you cannot reach any other way: CritterPath has
// no tick in its vtable, so whatever reads its enable byte is the spawner
// manager -- and that code contains the actor factory.
// len must be 1, 2 or 4; the address must be aligned to it.
int SWSE_WatchRW(unsigned addr, int len) {
    if (addr < 0x10000 || addr >= 0x7F000000) return 0;
    unsigned lenBits;
    switch (len) {
        case 1:  lenBits = 0x0; break;
        case 2:  lenBits = 0x1; break;
        default: lenBits = 0x3; len = 4; break;
    }
    if (!g_vehHandle) g_vehHandle = AddVectoredExceptionHandler(1, WatchVEH);
    g_watchAddr = addr; g_watchHits = 0;

    int n = ArmAllThreads(addr, (0x3u << 16) | (lenBits << 18));
    char b[120];
    wsprintfA(b, "watchrw: armed on %08X (read or write, %d byte) across %d thread(s)",
              addr, len, n);
    LogS(b);
    return n ? 1 : -1;
}

void SWSE_WatchOff() {
    DisarmAllThreads();
    char b[96];
    wsprintfA(b, "watch: disarmed (%d hit(s))", g_watchHits);
    LogS(b);
    g_watchAddr = 0;
}

// Resolve player+0x1C (the inventory head) and watch it â€” the caller doesn't
// need to know the address, which changes every session.
int SWSE_WatchInventory() {
    __try {
        void* o = ((getter_t)Addr(RVA_InvGetter))();
        if (!o) return 0;
        unsigned lvl1 = *(unsigned*)o;
        unsigned player = *(unsigned*)lvl1;
        unsigned slot = player + 0x1C;
        char b[128];
        wsprintfA(b, "watch: player=%08X, inventory head at %08X", player, slot);
        LogS(b);
        return SWSE_WatchWrite(slot);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// ==========================================================================
//  PLAYER FIELDS â€” reached via the getter chain, no priming, no Cheat Engine.
//
//  invdump revealed the layout: at player+0x78 sit three floats reading 300
//  (the normal-difficulty health the user reported: 600 easy / 300 normal /
//  150 hard), and another triple at +0x8C reading 150. Resolving
//  getter -> [obj] -> [[obj]] gives the player every session, so these are
//  directly readable and writable.
// ==========================================================================
#define PF_HEALTH   0x78      // current / max / base triple
#define PF_STAMINA  0x8C
#define PF_POS      0x24      // x / y / z

static void GodTick();        // fwd: refills health/stamina from the player obj

static unsigned PlayerObj() {
    __try {
        void* o = ((getter_t)Addr(RVA_InvGetter))();
        if (!o) return 0;
        unsigned lvl1 = *(unsigned*)o;
        if (lvl1 < 0x10000 || lvl1 >= 0x7F000000) return 0;
        unsigned p = *(unsigned*)lvl1;
        if (p < 0x10000 || p >= 0x7F000000) return 0;
        return p;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Read the three floats at `off`. Returns 1 on success.
int SWSE_PlayerGet(int off, float* a, float* b, float* c) {
    unsigned p = PlayerObj();
    if (!p) return 0;
    __try {
        float* f = (float*)(p + off);
        if (a) *a = f[0];
        if (b) *b = f[1];
        if (c) *c = f[2];
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -2; }
}

// Write all three (current, max, base) so the value sticks rather than being
// clamped back to the old maximum on the next update.
int SWSE_PlayerSet(int off, float v) {
    unsigned p = PlayerObj();
    if (!p) return 0;
    __try {
        float* f = (float*)(p + off);
        f[0] = v; f[1] = v; f[2] = v;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -2; }
}

int SWSE_PlayerHealth(float* cur, float* mx, float* base) {
    return SWSE_PlayerGet(PF_HEALTH, cur, mx, base);
}
int SWSE_PlayerSetHealth(float v)  { return SWSE_PlayerSet(PF_HEALTH, v); }
int SWSE_PlayerStamina(float* cur, float* mx, float* base) {
    return SWSE_PlayerGet(PF_STAMINA, cur, mx, base);
}
int SWSE_PlayerSetStamina(float v) { return SWSE_PlayerSet(PF_STAMINA, v); }

// ==========================================================================
//  EXACT VALUE SEARCH â€” far cleaner than diffing.
//
//  Diff scans drowned in noise (60-200 hits of unrelated churn). An exact
//  match on a number the player can SEE (moolah) gives a handful of hits
//  instead. Watch that address, buy something, and the purchase code â€” which
//  also adds the item â€” is what trips the watchpoint.
// ==========================================================================
static unsigned g_hits[64];
static int      g_hitN = 0;

int SWSE_FindValue(int value, int maxHits) {
    g_hitN = 0;
    SYSTEM_INFO si; GetSystemInfo(&si);
    unsigned char* p = (unsigned char*)si.lpMinimumApplicationAddress;
    unsigned char* hi = (unsigned char*)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi;
    char b[128];
    wsprintfA(b, "==== findvalue %d ====", value);
    LogS(b);
    while (p < hi && g_hitN < maxHits && g_hitN < 64) {
        if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
        bool ok = mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE
               && HeapRegion(mbi)
               && (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))
               && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS));
        if (ok) {
            __try {
                int* q = (int*)mbi.BaseAddress;
                size_t n = mbi.RegionSize / 4;
                for (size_t k = 0; k < n && g_hitN < 64; k++) {
                    if (q[k] != value) continue;
                    unsigned a = (unsigned)(uintptr_t)(q + k);
                    g_hits[g_hitN++] = a;
                    wsprintfA(b, "  %08X", a);
                    LogS(b);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        p = (unsigned char*)mbi.BaseAddress + mbi.RegionSize;
    }
    wsprintfA(b, "==== findvalue: %d hit(s) ====", g_hitN);
    LogS(b);
    return g_hitN;
}

// Narrow the previous hits to those now holding `value` (spend some moolah,
// then re-check) â€” the survivor is the real one.
int SWSE_FindNarrow(int value) {
    int kept = 0;
    char b[128];
    wsprintfA(b, "==== narrow to %d ====", value);
    LogS(b);
    for (int i = 0; i < g_hitN; i++) {
        __try {
            if (*(int*)g_hits[i] == value) {
                wsprintfA(b, "  %08X still matches", g_hits[i]);
                LogS(b);
                g_hits[kept++] = g_hits[i];
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    g_hitN = kept;
    wsprintfA(b, "==== narrow: %d left ====", kept);
    LogS(b);
    return kept;
}

unsigned SWSE_FindHit(int i) {
    return (i >= 0 && i < g_hitN) ? g_hits[i] : 0;
}

// ==========================================================================
//  STORE GRANT PATH â€” how the game itself gives you an item.
//
//  Found by watchpointing moolah. Buying fires the store UI message
//  "buy_item" -> dispatcher 0x87880 -> purchase 0x87A20, which deducts moolah
//  (fstp [wallet+0x0C]) and then calls the grant:
//
//      0x87C80(ecx = quantity, arg0 = playerObj, arg1 = wallet, arg2 = entry)
//      entry = { StringObj* name; int price; int stock }
//
//  No store object and no UI are required. price/stock are read by 0x87A20,
//  not by the grant, so they can be anything here. arg0 looks unused in the
//  main path but the cache-hit branch at 0x87D67 vtable-calls it.
//
//  This is the path the script VM could never reach: GiveArtifact needs an
//  ArtifactPref and faults on both call conventions.
// ==========================================================================
#define RVA_INV_GETTER   0x0588A0
#define RVA_GRANT_ITEM   0x087C80
#define VOFF_GET_WALLET  0x234        // virtual slot the purchase fn uses

typedef void* (__cdecl    *TInvGetter)();
typedef void* (__thiscall *TGetWallet)(void*);
typedef void  (__fastcall *TGrantItem)(int qty, int ignored,
                                       void* a0, void* wallet, void* entry);

static void* RvaPtr(unsigned r) {
    return (void*)((unsigned char*)GetModuleHandleA(NULL) + r);
}

// Resolve the wallet the same way the purchase function does, so it survives
// save reloads â€” no scanning, no cached addresses.
// Returns the wallet, and optionally the object it was reached through. That
// object is arg0 of the grant: the cache-hit branch at 0x87D67 calls its
// virtual slot +0x230 (the wallet getter is +0x234 on the same class).
static void* WalletFrom(void** objOut) {
    if (objOut) *objOut = nullptr;
    __try {
        TInvGetter get = (TInvGetter)RvaPtr(RVA_INV_GETTER);
        void* g = get();
        if (!g || !((void**)g)[2]) return nullptr;   // the cmp [eax+8],0 guard
        void* a = *(void**)g;      if (!a)   return nullptr;
        void* obj = *(void**)a;    if (!obj) return nullptr;
        void** vt = *(void***)obj; if (!vt)  return nullptr;
        TGetWallet gw = (TGetWallet)vt[VOFF_GET_WALLET / 4];
        if (!gw) return nullptr;
        if (objOut) *objOut = obj;
        return gw(obj);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

void* SWSE_WalletObj() { return WalletFrom(nullptr); }

int SWSE_Moolah(float* out) {
    void* w = SWSE_WalletObj();
    if (!w) return 0;
    __try { *out = *(float*)((unsigned char*)w + 0x0C); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -2; }
}

int SWSE_SetMoolah(float v) {
    void* w = SWSE_WalletObj();
    if (!w) return 0;
    __try { *(float*)((unsigned char*)w + 0x0C) = v; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -2; }
}

// Log exactly where a faulting call died, as a module offset we can look up in
// the disassembly. "it faulted" was costing us guesses; an RVA is an answer.
static DWORD GrantFilter(EXCEPTION_POINTERS* ep, const char* what) {
    char b[220];
    unsigned addr = (unsigned)(uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    unsigned base = (unsigned)(uintptr_t)GetModuleHandleA(NULL);
    wsprintfA(b, "%s: EXCEPTION %08X at %08X (module+0x%X)", what,
              (unsigned)ep->ExceptionRecord->ExceptionCode, addr,
              addr >= base ? addr - base : 0);
    LogS(b);
    if (ep->ExceptionRecord->NumberParameters >= 2) {
        wsprintfA(b, "   access type=%u target=%08X",
                  (unsigned)ep->ExceptionRecord->ExceptionInformation[0],
                  (unsigned)ep->ExceptionRecord->ExceptionInformation[1]);
        LogS(b);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// The name must outlive the call, so it lives in statics rather than on the
// stack. g_nameField IS the StringObj: the grant only reads its first dword.
static char g_itemName[192];

// The game's string object, recovered from 0x210890 (which allocates exactly
// 0x10 bytes for one). Every field matters:
//   +0x00 char* data
//   +0x08 length INCLUDING the terminator â€” 0x210890 does `mov ecx,[P+8]; dec
//         ecx` and skips out when it is 0. Leaving this zero made every lookup
//         see an empty name, which is what produced blank "blob" items.
//   +0x0C refcount â€” 0x1CD100 increments it; ==1 means "safe to mutate in
//         place", anything else forces a copy.
struct NameObj {
    const char* data;      // +0x00
    int         pad04;
    int         length;    // +0x08
    int         refcount;  // +0x0C
    int         pad[12];   // slack for fields we have not identified
};
static NameObj g_nameObj;
struct ItemEntry { const void* name; int price; int stock; };

int SWSE_GrantItem(const char* name, int qty, char* msg, int msgLen) {
    char tmp[256];
    void* obj = nullptr;
    void* w = WalletFrom(&obj);
    if (!w) { lstrcpynA(msg, "no wallet object â€” load a save first", msgLen); return 0; }

    // Accept a bare artifact name or an explicit prefs path. Backslashes are
    // what the store actually passes, and using them means 0x210890 finds no
    // '/' to rewrite â€” so it never tries to reallocate our static buffer.
    if (strchr(name, '/') || strchr(name, '\\'))
        lstrcpynA(g_itemName, name, sizeof(g_itemName));
    else
        wsprintfA(g_itemName, "\\data\\prefs\\artifacts\\%s.txt", name);
    for (char* p = g_itemName; *p; p++) if (*p == '/') *p = '\\';

    memset(&g_nameObj, 0, sizeof(g_nameObj));
    g_nameObj.data   = g_itemName;
    g_nameObj.length = lstrlenA(g_itemName) + 1;   // counts the terminator
    // Start high so the release path can never drive it to 0: a zero refcount
    // frees the object, and ours is a static the game must never free.
    g_nameObj.refcount = 0x10000000;

    ItemEntry entry;
    entry.name  = &g_nameObj;     // [entry] -> NameObj, [[entry]] -> char*
    entry.price = 0;
    entry.stock = -1;

    __try {
        // arg0 is NOT unused: names already in the game's table take the
        // cache-hit branch, which vtable-calls arg0. Passing null faulted on
        // every previously-seen item (e.g. damagestingbee).
        ((TGrantItem)RvaPtr(RVA_GRANT_ITEM))(qty, 0, obj, w, &entry);
    } __except (GrantFilter(GetExceptionInformation(), "grant")) {
        wsprintfA(tmp, "FAULTED granting %s â€” see log for the fault address", g_itemName);
        lstrcpynA(msg, tmp, msgLen);
        return -2;
    }
    wsprintfA(tmp, "granted %d x %s (wallet %08X)", qty, g_itemName, (unsigned)(uintptr_t)w);
    lstrcpynA(msg, tmp, msgLen);
    return 1;
}

// ---- grant spy: learn the item-name format from a real purchase ----------
// grant() faulted on a guessed path ("/data/prefs/artifacts/<n>.txt"), and the
// two string helpers both take a plain char*, so the entry layout is right and
// the name itself is wrong. Rather than guess formats, log the exact string a
// genuine store purchase hands to 0x87C80.
//
// PLEN = 8: "55 8B EC 83 E4 F8 6A FF" is push ebp(1) + mov ebp,esp(2) +
// and esp,-8(3) + push -1(2). Copying only 5 would slice the `and` in half â€”
// the same bug that broke the auto-prime hooks.
#define GRANT_PLEN 8
static BYTE* g_grantFn    = nullptr;
static BYTE* g_grantTramp = nullptr;
static bool  g_grantSpy   = false;
static int   g_grantHits  = 0;
static void* g_lastWallet = nullptr;   // captured from a genuine purchase
static void* g_lastEntry  = nullptr;

// Replay a call the game itself made. If this works and grant() doesn't, the
// difference is my synthetic entry; if this faults too, it's the call itself.
int SWSE_GrantLast(int qty, char* msg, int msgLen) {
    char tmp[200];
    if (!g_lastEntry) {
        lstrcpynA(msg, "nothing captured yet â€” run grantspy, then buy once", msgLen);
        return 0;
    }
    __try {
        ((TGrantItem)RvaPtr(RVA_GRANT_ITEM))(qty, 0, nullptr, g_lastWallet, g_lastEntry);
    } __except (GrantFilter(GetExceptionInformation(), "grantlast")) {
        lstrcpynA(msg, "FAULTED replaying the captured call â€” see log", msgLen);
        return -2;
    }
    wsprintfA(tmp, "replayed captured grant x%d (entry %08X)",
              qty, (unsigned)(uintptr_t)g_lastEntry);
    lstrcpynA(msg, tmp, msgLen);
    return 1;
}

extern "C" void __cdecl GrantLogRaw(void* saved) {
    if (!g_grantSpy) return;
    __try {
        // saved -> flags; pushad block follows: edi esi ebp esp ebx edx ecx eax
        BYTE* s = (BYTE*)saved;
        unsigned qty    = *(unsigned*)(s + 28);   // ecx = quantity (fastcall)
        unsigned wallet = *(unsigned*)(s + 44);   // arg1
        unsigned entry  = *(unsigned*)(s + 48);   // arg2
        char b[400];
        void** pp = (void**)entry;                // entry
        void** P  = pp ? (void**)pp[0] : nullptr; // [entry]
        const char* nm = P ? (const char*)P[0] : nullptr;   // [[entry]]
        g_lastWallet = (void*)wallet;   // remember a known-good call so we can
        g_lastEntry  = (void*)entry;    // replay it and isolate call vs entry
        wsprintfA(b, "GRANT SPY #%d: qty=%u wallet=%08X entry=%08X",
                  ++g_grantHits, qty, wallet, entry);
        LogS(b);
        wsprintfA(b, "   NAME = \"%s\"", nm ? nm : "(null)");
        LogS(b);
        int* e = (int*)entry;
        wsprintfA(b, "   entry: [0]=%08X price=%d stock=%d", e[0], e[1], e[2]);
        LogS(b);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogS("GRANT SPY: faulted reading the entry");
    }
}

__declspec(naked) static void HookGrant() {
    __asm {
        pushad
        pushfd
        push esp
        call GrantLogRaw
        add  esp, 4
        popfd
        popad
        jmp  dword ptr [g_grantTramp]
    }
}

int SWSE_GrantSpy(int on) {
    g_grantSpy = (on != 0);
    if (g_grantFn) return 1;                     // already installed
    if (!on) return 1;
    g_grantFn = (BYTE*)RvaPtr(RVA_GRANT_ITEM);
    g_grantTramp = (BYTE*)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE,
                                       PAGE_EXECUTE_READWRITE);
    if (!g_grantTramp) { g_grantFn = nullptr; return -1; }
    memcpy(g_grantTramp, g_grantFn, GRANT_PLEN);
    g_grantTramp[GRANT_PLEN] = 0xE9;
    *(DWORD*)(g_grantTramp + GRANT_PLEN + 1) =
        (DWORD)((g_grantFn + GRANT_PLEN) - (g_grantTramp + GRANT_PLEN + 5));
    DWORD old;
    VirtualProtect(g_grantFn, GRANT_PLEN, PAGE_EXECUTE_READWRITE, &old);
    g_grantFn[0] = 0xE9;
    *(DWORD*)(g_grantFn + 1) = (DWORD)((BYTE*)&HookGrant - (g_grantFn + 5));
    for (int i = 5; i < GRANT_PLEN; i++) g_grantFn[i] = 0x90;
    VirtualProtect(g_grantFn, GRANT_PLEN, old, &old);
    LogS("grant spy: hook installed on 0x87C80 â€” buy something now");
    return 1;
}

// ==========================================================================
//  MOTION PREFS â€” jump height, run speed, gravity, air control.
//
//  The game reflects its own fields: each one is registered in .text as
//      mov eax,"m_name" ; mov [edi],-1 ; call hash ; mov [esi+8],<offset>
//  so the offsets below are the game's, not guesses. 1707 of them are dumped
//  in swse/research/FIELD_OFFSETS.tsv.
//
//  Instances are located by scanning for their RTTI vtable, exactly the way
//  autoprime finds a ScriptContext. MotionImpl and MotionImplDummy share a
//  layout (126 methods each), so both are treated the same.
// ==========================================================================
#define VT_MOTIONIMPL      0x37A924
#define VT_MOTIONDUMMY     0x37AC0C
#define VT_GLOBALMOTION    0x37AB24

#define MI_JUMP_MIN        0x14      // m_jumpHeightMin
#define MI_JUMP_MAX        0x18      // m_jumpHeightMax
#define MI_JUMP_SCALE      0x1C      // m_jumpHeightSpeedScale
#define MI_VEL_WALK        0x78      // m_overVel_Walk
#define MI_VEL_TROT        0x7C
#define MI_VEL_CANTER      0x80
#define MI_VEL_RUN         0x84      // m_overVel_Run
#define GM_AIRCTRL_LO      0x20      // m_airControlLo
#define GM_AIRCTRL_HI      0x24
#define GM_GRAVITY         0x68      // m_gravityForFall

// Instance addresses are CACHED. Scanning the whole address space per call
// froze the game outright: four commands in one batch meant four full scans
// inside a single frame. The scan now runs once per vtable set and the results
// are reused; entries are re-validated cheaply (vtable still matches) so a
// freed object is dropped rather than written to.
struct InstCache {
    unsigned key;                 // first vtable RVA, identifies the set
    unsigned addr[256];
    int      count;
    bool     valid;
};
static InstCache g_inst[2];       // [0] = motion impls, [1] = global prefs

static void ScanInstances(InstCache& c, const unsigned* vts, int nvt) {
    unsigned rt[4];
    unsigned base = (unsigned)(uintptr_t)GetModuleHandleA(NULL);
    for (int i = 0; i < nvt; i++) rt[i] = base + vts[i];
    c.count = 0;
    SYSTEM_INFO si; GetSystemInfo(&si);
    BYTE* p  = (BYTE*)si.lpMinimumApplicationAddress;
    BYTE* hi = (BYTE*)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi;
    while (p < hi && c.count < 256) {
        if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
        bool ok = mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE
               && HeapRegion(mbi)
               && (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))
               && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS));
        if (ok) {
            __try {
                unsigned* q = (unsigned*)mbi.BaseAddress;
                size_t n = mbi.RegionSize / 4;
                for (size_t k = 0; k < n && c.count < 256; k++) {
                    for (int i = 0; i < nvt; i++) {
                        if (q[k] != rt[i]) continue;
                        c.addr[c.count++] = (unsigned)(uintptr_t)(q + k);
                        break;
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        p = (BYTE*)mbi.BaseAddress + mbi.RegionSize;
    }
    c.key = vts[0];
    c.valid = true;
}

// onlyMax: write ONLY the instance holding the highest current value, instead
// of every match. Setting speed on all 94 MotionImpl matches crashed the game â€”
// a matching vtable dword does not prove an object, and even the real NPCs did
// not survive it. One instance is a far smaller thing to be wrong about.
static int VisitInstances(InstCache& c, const unsigned* vts, int nvt, int off,
                          const float* set, float* first, bool onlyMax = false) {
    if (!c.valid || c.key != vts[0]) ScanInstances(c, vts, nvt);
    unsigned rt[4];
    unsigned base = (unsigned)(uintptr_t)GetModuleHandleA(NULL);
    for (int i = 0; i < nvt; i++) rt[i] = base + vts[i];

    // A dword equal to the vtable is NOT proof of an object: the same pointer
    // appears in other structures and on stacks, and reading a field off one of
    // those returns garbage. Reject implausible values â€” and crucially, never
    // WRITE to an entry that fails, or we scribble on unrelated memory.
    // These are motion prefs: heights, speeds, gravity. Nothing is astronomical.
    int hits = 0;
    float best = 0.0f;
    bool gotAny = false;
    float* bestPtr = nullptr;
    for (int j = 0; j < c.count; j++) {
        __try {
            unsigned vt = *(unsigned*)c.addr[j];
            bool still = false;
            for (int i = 0; i < nvt; i++) if (vt == rt[i]) { still = true; break; }
            if (!still) continue;              // object died; skip it
            float* f = (float*)(c.addr[j] + off);
            float v = *f;
            // NaN fails every comparison, so this rejects it too.
            if (!(v >= -100000.0f && v <= 100000.0f)) continue;
            if (!gotAny || v > best) { best = v; gotAny = true; bestPtr = f; }
            if (set && !onlyMax) *f = *set;
            hits++;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (set && onlyMax && bestPtr) {
        __try { *bestPtr = *set; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        hits = 1;
    }
    if (first && gotAny) *first = best;
    return hits;
}

// Force a fresh scan (after a level load, when objects have been recreated).
void SWSE_MotionRescan() { g_inst[0].valid = false; g_inst[1].valid = false; }

// The player OWNS its motion objects â€” no scanning or guessing needed.
// player+0xB0 and player+0x1A4 each hold a MotionImpl pointer (the Stranger
// and Steef forms, most likely; +0x188 mirrors +0xB0). Scanning for the
// "fastest instance" was a guess that could just as easily hit an NPC.
#define PF_MOTION_A 0x0B0
#define PF_MOTION_B 0x1A4

static int PlayerMotionField(int off, const float* set, float* cur) {
    unsigned p = PlayerObj();
    if (!p) return 0;
    unsigned base = (unsigned)(uintptr_t)GetModuleHandleA(NULL);
    unsigned vtA = base + VT_MOTIONIMPL, vtB = base + VT_MOTIONDUMMY;
    const int slots[2] = { PF_MOTION_A, PF_MOTION_B };
    int hits = 0;
    float best = 0.0f; bool gotAny = false;
    for (int i = 0; i < 2; i++) {
        __try {
            unsigned mo = *(unsigned*)(p + slots[i]);
            if (mo < 0x10000) continue;
            unsigned vt = *(unsigned*)mo;
            if (vt != vtA && vt != vtB) continue;      // not a MotionImpl
            float* f = (float*)(mo + off);
            float v = *f;
            if (!(v >= -100000.0f && v <= 100000.0f)) continue;
            if (!gotAny || v > best) { best = v; gotAny = true; }
            if (set) *f = *set;
            hits++;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (cur && gotAny) *cur = best;
    return hits;
}

// field: 0=jump 1=run speed 2=gravity 3=air control.
// Returns instances touched; *cur receives the current value.
int SWSE_MotionField(int field, const float* set, float* cur) {
    static const unsigned kMotion[2] = { VT_MOTIONIMPL, VT_MOTIONDUMMY };
    static const unsigned kGlobal[1] = { VT_GLOBALMOTION };
    switch (field) {
    case 0:     // jump height, player's own motion objects only
        return PlayerMotionField(MI_JUMP_MAX, set, cur);
    case 1:     // run speed, player's own motion objects only
        return PlayerMotionField(MI_VEL_RUN, set, cur);
    case 2:  return VisitInstances(g_inst[1], kGlobal, 1, GM_GRAVITY, set, cur);
    case 3: {
        int n = VisitInstances(g_inst[1], kGlobal, 1, GM_AIRCTRL_HI, set, cur);
        if (set) VisitInstances(g_inst[1], kGlobal, 1, GM_AIRCTRL_LO, set, nullptr);
        return n;
    }
    }
    return 0;
}

// ==========================================================================
//  NPC SPAWNING via ActorSpawner
//
//  There is no script function that spawns an actor -- every Spawn* verb in
//  the VM makes effects, not characters. But levels are full of ActorSpawner
//  objects, and ActorSpawner::vfunc17 (RVA 0x230A0) is a timed tick:
//
//      cmp byte [this+0x24], 0      ; enabled?
//      mov eax, [this+0x64]         ; max count
//      cmp [this+0xBC], eax         ; spawned so far
//      fld  qword [0x9D5540]        ; game time
//      fsub qword [this+0xC0]       ; last spawn time
//      fcomp dword [this+0x70]      ; interval
//
//  So we do not need a spawn function: enable the spawner, clear its counter,
//  raise its cap and zero its interval, and its own tick does the work.
// ==========================================================================
#define VT_ACTORSPAWNER 0x3669FC     // the 52-method ActorSpawner vtable
#define AS_ENABLED      0x24         // byte
#define AS_MAXCOUNT     0x64         // int, lifetime cap
// +0x68 is the CONCURRENT limit and it is the real gate: the tick counts how
// many of its actors are currently busy and bails on
//     cmp eax,[this+0x68] / jge bail
// It ships as 1, so a spawner with even one live actor never fires again.
// Raising +0x64 alone does nothing, which is why arming did not spawn.
#define AS_CONCURRENT   0x68         // int
#define AS_INTERVAL     0x70         // float, seconds between spawns
#define AS_ARRAY        0xB0         // array of live spawned actors
#define AS_LIVECOUNT    0xB8         // int, how many are alive right now
#define AS_CURCOUNT     0xBC         // int, only used by the tick's cap compare
#define AS_LASTTIME     0xC0         // double, game time of last spawn

// The spawner carries its own transform: +0x30..+0x4C is a rotation matrix and
// +0x50/54/58 the translation (+0x5C = 1.0, the homogeneous row). Writing the
// player's coordinates there relocates the spawner to us, which is how we get
// actors to appear nearby instead of wherever the level placed them.
#define AS_POS          0x50
#define AS_TYPEHASH     0x9C         // what it spawns (a path hash)

static float* PlayerPos();           // fwd: defined with the position work

// Move the first usable spawner to the player and arm it. Returns 1 ok,
// 0 = no player, -1 = no spawner found.
int SWSE_SpawnHere(int count) {
    float* pp = PlayerPos();
    if (!pp) return 0;
    float px = pp[0], py = pp[1], pz = pp[2];

    unsigned vt = (unsigned)(uintptr_t)GetModuleHandleA(NULL) + VT_ACTORSPAWNER;
    SYSTEM_INFO si; GetSystemInfo(&si);
    BYTE* p  = (BYTE*)si.lpMinimumApplicationAddress;
    BYTE* hi = (BYTE*)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi;
    while (p < hi) {
        if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
        bool ok = mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE
               && HeapRegion(mbi)
               && (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))
               && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS));
        if (ok) {
            __try {
                unsigned* q = (unsigned*)mbi.BaseAddress;
                size_t n = mbi.RegionSize / 4;
                for (size_t k = 0; k < n; k++) {
                    if (q[k] != vt) continue;
                    BYTE* o = (BYTE*)(q + k);
                    BYTE  en  = *(BYTE*)(o + AS_ENABLED);
                    float ivl = *(float*)(o + AS_INTERVAL);
                    float w   = *(float*)(o + AS_POS + 12);
                    if (en > 1) continue;
                    if (!(ivl >= 0.0f && ivl <= 100000.0f)) continue;
                    if (!(w > 0.99f && w < 1.01f)) continue;   // transform row
                    float* t = (float*)(o + AS_POS);
                    t[0] = px; t[1] = py; t[2] = pz;
                    *(BYTE*)  (o + AS_ENABLED)    = 1;
                    *(int*)   (o + AS_CURCOUNT)   = 0;
                    *(int*)   (o + AS_MAXCOUNT)   = count;
                    *(int*)   (o + AS_CONCURRENT) = count;
                    *(float*) (o + AS_INTERVAL)   = 0.0f;
                    *(double*)(o + AS_LASTTIME)   = 0.0;
                    char b[140];
                    wsprintfA(b, "spawnhere: spawner %08X moved to player, max=%d",
                              (unsigned)(uintptr_t)o, count);
                    LogS(b);
                    return 1;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        p = (BYTE*)mbi.BaseAddress + mbi.RegionSize;
    }
    return -1;
}

// ==========================================================================
//  CRITTER SPAWNING via CritterPath
//
//  ActorSpawner was a dead end: an execution breakpoint on its tick
//  (module+0x230A0) recorded ZERO hits, so those objects are never ticked and
//  every field written to them was irrelevant.
//
//  CritterPath is different, and documented by the game itself.
//  SetCritterPathSpawnEnabled boils down to one instruction:
//      mov byte ptr [obj+0x60], bl
//  and CritterPathPrefs is a reflected class, so its fields are named:
//      m_count 0x10, m_spawnCapacity 0x14, m_spawnFrequency 0x28,
//      m_spawnRandomness 0x2C, m_spawnEnabled 0x38
// ==========================================================================
#define VT_CRITTERPATH       0x368878
#define VT_CRITTERPATHPREFS  0x36896C
#define CP_ENABLE            0x60     // byte; what the script verb writes
#define CPP_COUNT            0x10
#define CPP_CAPACITY         0x14
#define CPP_SPAWNFREQ        0x28     // float, seconds
#define CPP_SPAWNRAND        0x2C
#define CPP_ENABLED          0x38

// Enable critter paths and make them spawn fast. Returns paths + prefs touched.
int SWSE_Critters(int count, int* prefsOut) {
    unsigned base = (unsigned)(uintptr_t)GetModuleHandleA(NULL);
    unsigned vtPath  = base + VT_CRITTERPATH;
    unsigned vtPrefs = base + VT_CRITTERPATHPREFS;
    int paths = 0, prefs = 0;
    char b[140];

    SYSTEM_INFO si; GetSystemInfo(&si);
    BYTE* p  = (BYTE*)si.lpMinimumApplicationAddress;
    BYTE* hi = (BYTE*)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi;
    while (p < hi) {
        if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
        bool ok = mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE
               && HeapRegion(mbi)
               && (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))
               && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS));
        if (ok) {
            __try {
                unsigned* q = (unsigned*)mbi.BaseAddress;
                size_t n = mbi.RegionSize / 4;
                for (size_t k = 0; k < n; k++) {
                    if (q[k] == vtPath) {
                        BYTE* o = (BYTE*)(q + k);
                        BYTE cur = *(BYTE*)(o + CP_ENABLE);
                        if (cur > 1) continue;            // not a real path
                        if (count > 0) *(BYTE*)(o + CP_ENABLE) = 1;
                        if (paths < 6) {
                            wsprintfA(b, "critterpath %08X: enable was %d",
                                      (unsigned)(uintptr_t)o, cur);
                            LogS(b);
                        }
                        paths++;
                    } else if (q[k] == vtPrefs) {
                        BYTE* o = (BYTE*)(q + k);
                        float f = *(float*)(o + CPP_SPAWNFREQ);
                        if (!(f >= 0.0f && f <= 100000.0f)) continue;
                        if (prefs < 6) {
                            wsprintfA(b, "critterprefs %08X: count=%d cap=%d freq=%d enabled=%d",
                                      (unsigned)(uintptr_t)o,
                                      *(int*)(o + CPP_COUNT),
                                      *(int*)(o + CPP_CAPACITY),
                                      (int)f, *(int*)(o + CPP_ENABLED));
                            LogS(b);
                        }
                        if (count > 0) {
                            *(int*)  (o + CPP_COUNT)     = count;
                            *(int*)  (o + CPP_CAPACITY)  = count;
                            *(float*)(o + CPP_SPAWNFREQ) = 0.25f;
                            *(float*)(o + CPP_SPAWNRAND) = 0.0f;
                            *(int*)  (o + CPP_ENABLED)   = 1;
                        }
                        prefs++;
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        p = (BYTE*)mbi.BaseAddress + mbi.RegionSize;
    }
    wsprintfA(b, "critters: %d path(s), %d prefs", paths, prefs);
    LogS(b);
    if (prefsOut) *prefsOut = prefs;
    return paths;
}

// Returns spawners touched. `count` is the new cap; pass 0 just to count them.
int SWSE_Spawn(int count) {
    unsigned vt = (unsigned)(uintptr_t)GetModuleHandleA(NULL) + VT_ACTORSPAWNER;
    int hits = 0;
    SYSTEM_INFO si; GetSystemInfo(&si);
    BYTE* p  = (BYTE*)si.lpMinimumApplicationAddress;
    BYTE* hi = (BYTE*)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi;
    char b[140];
    while (p < hi) {
        if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
        bool ok = mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE
               && HeapRegion(mbi)
               && (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))
               && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS));
        if (ok) {
            __try {
                unsigned* q = (unsigned*)mbi.BaseAddress;
                size_t n = mbi.RegionSize / 4;
                for (size_t k = 0; k < n; k++) {
                    if (q[k] != vt) continue;
                    BYTE* o = (BYTE*)(q + k);
                    // Sanity: a real spawner's enabled byte is 0 or 1 and its
                    // interval is a small positive time. Stack copies of the
                    // vtable pointer fail this and must never be written.
                    BYTE  en  = *(BYTE*)(o + AS_ENABLED);
                    float ivl = *(float*)(o + AS_INTERVAL);
                    if (en > 1) continue;
                    if (!(ivl >= 0.0f && ivl <= 100000.0f)) continue;
                    if (count > 0) {
                        *(BYTE*)  (o + AS_ENABLED)    = 1;
                        *(int*)   (o + AS_CURCOUNT)   = 0;
                        *(int*)   (o + AS_MAXCOUNT)   = count;
                        *(int*)   (o + AS_CONCURRENT) = count;
                        *(float*) (o + AS_INTERVAL)   = 0.0f;
                        *(double*)(o + AS_LASTTIME)   = 0.0;
                        // The tick also compares each existing actor's own
                        // timestamp at [entry+0x84] against the interval, so a
                        // recently-spawned actor keeps it saying "too soon".
                        // Zero them so those comparisons pass too.
                        unsigned arr = *(unsigned*)(o + AS_ARRAY);
                        int      na  = *(int*)(o + AS_LIVECOUNT);
                        if (arr > 0x10000 && na > 0 && na < 256) {
                            for (int e = 0; e < na; e++) {
                                unsigned ent = ((unsigned*)arr)[e];
                                if (ent > 0x10000) *(double*)(ent + 0x84) = 0.0;
                            }
                        }
                    }
                    if (hits < 8) {
                        // +0xB8 is the LIVE count (entries in the array at
                        // +0xB0). +0xBC only feeds the tick's cap compare and
                        // stays 0, which made a working spawner look inert.
                        wsprintfA(b, "spawner %08X: enabled=%d max=%d live=%d",
                                  (unsigned)(uintptr_t)o, en,
                                  *(int*)(o + AS_MAXCOUNT),
                                  *(int*)(o + AS_LIVECOUNT));
                        LogS(b);
                    }
                    hits++;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        p = (BYTE*)mbi.BaseAddress + mbi.RegionSize;
    }
    wsprintfA(b, "spawn: %d ActorSpawner(s) found", hits);
    LogS(b);
    return hits;
}

// ==========================================================================
//  NPC FACTORY capture + replay
//
//  0x25A50 allocates 0x260 bytes, calls the NPC constructor (vtable matches
//  RTTI's NPC, 97 methods) and initialises it via 0x25FF0. Its caller at
//  0x184E34 sets it up as:
//
//      push 0 ; push 0 ; push esi+0x3C ; push esi+0x34 ; push esi+0x30
//      push ebx+0x3C ; push [ebx+0x14] ; push <tmp> ; push <tmp>
//      lea edi,[esp+0x60]        ; edi = OUT pointer for the new NPC
//      call 0x25A50 ; add esp,0x28
//
//  Rather than synthesise nine arguments (several of which point into level
//  tag structures), capture a real call and replay it -- the same approach
//  that finally worked for artifacts via grantspy/grantlast.
//
//  An inline hook, NOT an execution breakpoint: watchexec on this function
//  took 21106 hits during a level load and killed the game.
// ==========================================================================
#define RVA_NPC_FACTORY  0x25A50
#define RVA_NPC_REGISTER 0x63010     // 0x63010(npc, tag): adds it to the world
#define NPCF_PLEN        6           // "64 A1 00 00 00 00" = mov eax,fs:[0]

static BYTE*    g_npcFn    = nullptr;
static BYTE*    g_npcTramp = nullptr;
static bool     g_npcSpy   = false;
static int      g_npcHits  = 0;
static unsigned g_npcArgs[10];
static unsigned g_npcDeref0[8];      // contents of *a0 at capture time
static unsigned g_npcDeref1[8];      // contents of *a1 -- an INPUT, not an out
static unsigned g_npcEdi   = 0;
static bool     g_npcHave  = false;

// ---- whole-routine spawn ---------------------------------------------------
// 0x184D90 is SpawnNPCFromTag(this=ecx, arg0), ret 4. It does everything:
// allocate, construct, init, set facing, and register. Replaying just the
// factory (0x25A50) produced a valid NPC that then faulted in registration,
// because three init calls in between were skipped. Replaying the whole
// routine avoids reproducing that sequence by hand.
#define RVA_NPC_SPAWNROUTINE 0x184D90
#define NPCR_PLEN            6        // "64 A1 00 00 00 00" = mov eax,fs:[0]

static BYTE*    g_nprFn    = nullptr;
static BYTE*    g_nprTramp = nullptr;
static bool     g_nprSpy   = false;
static int      g_nprHits  = 0;
static unsigned g_nprThis  = 0;       // ecx (the spawn record)
static unsigned g_nprArg0  = 0;
// arg0's vtable, captured live. RTTI names it InstancedObjectTag -- NOT the
// GeometryInst I had assumed, which is why every on-demand call faulted.
static unsigned g_nprArg0Vt = 0;
static bool     g_nprHave  = false;
// 0x200, not 0x60: the record is larger than the fields we identified, and a
// truncated clone leaves zeros where the routine expects live pointers.
static unsigned char g_nprRec[0x200];
// arg0's bytes too. It is an InstancedObjectTag (0x7C bytes) and it OWNS the
// NPCTag at +0x78 -- the spawn call is a virtual on the pair, so a lone tag was
// always half a structure.
static unsigned char g_nprA0Rec[0x7C];

// Every distinct type hash a load goes past. The captured hash is whichever
// tag happened to be processed last, which is why a spawn produced a character
// that belongs elsewhere in the level -- collecting them all turns that
// accident into a menu. These are the game's own hashes, so they resolve.
#define MAX_NPC_TYPES 64
static unsigned g_npcTypeHash[MAX_NPC_TYPES];
static int      g_npcTypeCount = 0;

static void RecordNpcType(unsigned hash) {
    if (!hash || hash == 0x2DFD1072) return;      // unset-resource sentinel
    for (int i = 0; i < g_npcTypeCount; i++)
        if (g_npcTypeHash[i] == hash) return;
    if (g_npcTypeCount < MAX_NPC_TYPES)
        g_npcTypeHash[g_npcTypeCount++] = hash;
}

// Cleared whenever npcspy is armed. The list previously accumulated across
// every level of a session, so "35 harvested types" described nowhere in
// particular -- and the dupetype safety check, which trusts it to mean "loaded
// HERE", would happily pass a hash from another level and crash the load.
void SWSE_ClearNpcTypes() { g_npcTypeCount = 0; }

int SWSE_NpcTypeCount() { return g_npcTypeCount; }
unsigned SWSE_NpcTypeHash(int i) {
    return (i >= 0 && i < g_npcTypeCount) ? g_npcTypeHash[i] : 0;
}

// Rather than fabricate a tag, re-drive the game's own routine with the game's
// own arguments, while they are still live. The capture showed the game calling
// 0x184D90 three times with an identical this/arg0 pair, so an extra call with
// that same pair is something the game already does -- nothing is invented.
static int  g_nprDupe = 0;      // extra calls to make per capture
static int  g_nprCount = 0;     // if >0, patch m_countToSpawn (+0x0C) first
static int  g_nprBuildTest = 0; // constructed-object spawns to run during a load
static unsigned g_dupeType  = 0; // if set, retype the game's own tags on load
static int      g_dupeEvery = 1; // retype every Nth spawn (1 = all of them)
static int      g_dupeSeen  = 0;

// ---- character tuning, applied automatically on every level load ----------
// Typing npchealth/npcgib after each load is not a mod, it is a debug session.
// This reads SWSEMods\SWSE Console\characters.txt once and applies it
// from inside the spawn hook, so every character the level uses is tuned as it
// is spawned -- including variants you did not know existed, which is what let
// one immortal townsfolk survive a sweep aimed at a single hash.
//
//   # hash      health  gib     ('-' leaves a setting alone)
//   5CEE67FD    30      1       townsfolk: killable and they gib
//   *           45      -       everyone else: outlaw-tier health
// m_hurtReaction (+0x484) is how a character reacts to being hit: 0 = normal
// (staggers, gets knocked about), 2 = unflinchable, as used by the 10000hp
// heavy. It is an enum rather than a bool, so it is stored as-is.
struct CharTune { unsigned hash; float health; int gib; int hurt; };
static CharTune g_tune[64];
static int      g_tuneCount   = 0;
static float    g_tuneAllHp   = -1.0f;
static int      g_tuneAllGib  = -1;
static int      g_tuneAllHurt = -1;

// Named feature toggles, the layer above per-hash rules. A rule beats an
// enumeration: "anything the game shipped at 100000 becomes mortal" catches
// townsfolk variants nobody has identified, which a hash list never will --
// exactly how the farmers stayed immortal after a sweep aimed at one hash.
static int   g_noImmortals   = -1;    // target hp, or -1 for off
static int   g_immortalsGib  = -1;    // gib the characters noimmortals freed
static float kImmortalHp     = 100000.0f;
static bool     g_tuneLoaded  = false;

static unsigned ResolvePrefs(unsigned hash);   // defined below

static void ApplyTuningForType(unsigned hash) {
    if (!g_tuneLoaded || !hash || hash == 0x2DFD1072) return;
    float hp = g_tuneAllHp;
    int   gb = g_tuneAllGib;
    int   hr = g_tuneAllHurt;
    for (int i = 0; i < g_tuneCount; i++) {          // specific beats wildcard
        if (g_tune[i].hash != hash) continue;
        if (g_tune[i].health >= 0.0f) hp = g_tune[i].health;
        if (g_tune[i].gib    >= 0)    gb = g_tune[i].gib;
        if (g_tune[i].hurt   >= 0)    hr = g_tune[i].hurt;
        break;
    }
    unsigned prefs = ResolvePrefs(hash);
    if (!prefs) return;

    // noimmortals: applied by measuring the character rather than naming it.
    if (g_noImmortals >= 0 && hp < 0.0f) {
        __try {
            if (*(float*)(prefs + 0x448) >= kImmortalHp) {
                hp = (float)g_noImmortals;
                if (gb < 0 && g_immortalsGib >= 0) gb = g_immortalsGib;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (hp < 0.0f && gb < 0 && hr < 0) return;
    __try {
        if (hp >= 0.0f) {
            *(float*)(prefs + 0x448) = hp;
            *(float*)(prefs + 0x44C) = hp;
        }
        if (gb >= 0) {
            *(unsigned char*)(prefs + 0x460) = (unsigned char)(gb ? 1 : 0);
            if (gb) *(unsigned char*)(prefs + 0x461) = 1;
        }
        if (hr >= 0) *(unsigned*)(prefs + 0x484) = (unsigned)hr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// Defined further down, next to the constructors it needs.
static unsigned ConstructedSpawnFwd(unsigned useHash, float* pos);
static bool g_nprBusy = false;

// this=ecx, one stack arg, callee cleans it (ret 4). Calling the trampoline
// rather than the patched entry means the hook is not re-entered.
static void CallSpawnRoutine(unsigned thisPtr, unsigned arg0) {
    void* tramp = g_nprTramp;
    if (!tramp) return;
    __asm {
        mov  eax, tramp
        push arg0
        mov  ecx, thisPtr
        call eax
    }
}

extern "C" void __cdecl NpcRoutineLog(void* saved) {
    if (!g_nprSpy) return;
    __try {
        BYTE* s = (BYTE*)saved;
        // pushad order: edi esi ebp esp ebx edx ecx eax  (ecx at +28)
        unsigned ecx = *(unsigned*)(s + 28);
        unsigned* stk = (unsigned*)(s + 36);     // [ret, arg0]
        g_nprThis = ecx;
        g_nprArg0 = stk[1];
        // Snapshot the record HERE, while the call is in flight. Copying it
        // later read a recycled object -- the hash came back as a vtable
        // pointer instead of the real value.
        __try { memcpy(g_nprRec, (const void*)ecx, sizeof(g_nprRec)); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        g_nprHave = true;
        // Type recording stays outside the cap -- the menu needs every type.
        // Tuning is applied here too: every character the level spawns passes
        // through this hook, so nothing is missed and it survives every load.
        __try {
            unsigned th = *(unsigned*)(ecx + 0x08);
            RecordNpcType(th);
            ApplyTuningForType(th);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (++g_nprHits <= 3) {
            // The anchor template comes from one of the FIRST spawns of the
            // load, which is what the build that produced visible NPCs did.
            // Moving this out of the cap looked like a harmless tidy-up, but it
            // swapped the template for the LAST anchor of the load -- a
            // different object, and every spawn is built from it.
            __try {
                g_nprArg0Vt = *(unsigned*)stk[1];
                memcpy(g_nprA0Rec, (const void*)stk[1], sizeof(g_nprA0Rec));
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            char b[200];
            // Name arg0's class outright. Assuming it was a GeometryInst --
            // from nothing but a plausible-looking vtable -- cost several
            // rounds of faults.
            char a0nm[64];
            if (!RttiName(stk[1], a0nm, sizeof(a0nm))) lstrcpynA(a0nm, "?", 2);
            wsprintfA(b, "NPC ROUTINE #%d: tag=%08X arg0=%08X is a %s  ret=%08X",
                      g_nprHits, ecx, stk[1], a0nm, stk[0]);
            LogS(b);
            // Log the snapshot's own bytes so it can be compared against a
            // dumpaddr of the same address -- three captures produced three
            // different "hashes" and guessing which was right was going nowhere.
            // The full record: this is an NPCTag (vtable 0x77FC84 at +0x00),
            // so the reflected offsets apply -- m_npcPref +0x08 (a hash),
            // m_countToSpawn +0x0C, m_maxAliveAtATime +0x10.
            unsigned* r = (unsigned*)g_nprRec;
            wsprintfA(b, "   snap 00-1C: %08X %08X %08X %08X %08X %08X %08X %08X",
                      r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
            LogS(b);
            wsprintfA(b, "   snap 20-3C: %08X %08X %08X %08X %08X %08X %08X %08X",
                      r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
            LogS(b);
            wsprintfA(b, "   snap 40-5C: %08X %08X %08X %08X %08X %08X %08X %08X",
                      r[16], r[17], r[18], r[19], r[20], r[21], r[22], r[23]);
            LogS(b);
        }
        // Choose the character by patching the GAME'S OWN tag, in flight,
        // rather than building one. Constructed pairs register but never enter
        // the world -- proven both after a load and during one -- while the
        // real tag demonstrably works. This runs before the trampoline, so the
        // level's own spawn is retyped too.
        // Retype every Nth spawn, not necessarily all of them. Turning all 157
        // spawns in a level into a 5000hp boss crashed the game -- each carries
        // a boss's model, AI and physics, which is far past what a level built
        // for a handful of them budgets for.
        if (g_dupeType) {
            g_dupeSeen++;
            if (g_dupeEvery <= 1 || (g_dupeSeen % g_dupeEvery) == 0) {
                __try { *(unsigned*)(ecx + 0x08) = g_dupeType; }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
        }
        if (g_nprCount > 0) {
            __try { *(unsigned*)(ecx + 0x0C) = (unsigned)g_nprCount; }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        // Single-variable test. npcdupe (the game's real objects, during a
        // load) produces NPCs you can see and fight. npcnow (objects we build,
        // after the load) produces objects that exist, type-check and register
        // -- but are not in the world. This runs OUR objects during the load,
        // leaving the captured transform alone, so the result is directly
        // comparable to the real spawn happening beside it.
        if (g_nprBuildTest > 0 && !g_nprBusy) {
            g_nprBusy = true;
            int n = g_nprBuildTest;
            g_nprBuildTest = 0;          // once per load, not once per spawn
            for (int i = 0; i < n; i++) {
                unsigned ok = ConstructedSpawnFwd(0, nullptr);
                char bb[120];
                wsprintfA(bb, "buildtest %d: constructed spawn returned %d", i, ok);
                LogS(bb);
            }
            g_nprBusy = false;
        }
        if (g_nprDupe > 0 && !g_nprBusy) {
            g_nprBusy = true;
            for (int i = 0; i < g_nprDupe; i++)
                CallSpawnRoutine(ecx, g_nprArg0);
            g_nprBusy = false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

int SWSE_NpcBuildTest(int n) { g_nprBuildTest = n; return 1; }
int SWSE_DupeType(unsigned hash) { g_dupeType = hash; g_dupeSeen = 0; return 1; }
int SWSE_DupeEvery(int n) { g_dupeEvery = (n < 1) ? 1 : n; g_dupeSeen = 0; return 1; }
int SWSE_NpcDupe(int n)  { g_nprDupe  = n; return 1; }
int SWSE_NpcCount(int n) { g_nprCount = n; return 1; }

// On-demand spawning. The captured tag is freed after its load, but a scan
// found one NPCTag that OUTLIVES the load -- so there is no need to forge one
// or keep a dead one alive: drive the routine with that live tag and a live
// GeometryInst (world geometry, which persists) for the location.
// Counts NPCs before and after, because "it looked like more enemies" is
// exactly the reasoning that made me call the old spawn command working when
// it wasn't.
int SWSE_FindNpcs(unsigned* out, int maxOut);
int SWSE_NpcTags(unsigned* out, int maxOut);
int SWSE_FindGeomInst(unsigned* out, int maxOut);

static unsigned g_snScan[1024];
static unsigned g_snGeom[1024];
// The scan is ordered by ADDRESS, not by creation time, so "the last few
// entries" are not the new NPCs -- reading those reported the type of whichever
// unrelated NPC sat highest in memory. Keep the before-set and diff it.
static unsigned g_snPrev[1024];
// An actor's own position copy is at +0x24 (PF_POS), the same field PlayerPos
// validates against. Reading NPC positions from +0x44 returned (x, 0, -1) for
// every NPC in the game and made every distance reported tonight meaningless.
#define NPC_POS 0x24
static float    g_spawnRadius = 1.5f;   // the value that demonstrably worked
int SWSE_SetSpawnRadius(int tenths) { g_spawnRadius = (float)tenths / 10.0f; return 1; }

// SpawnNPCFromTag opens with a guard:
//     call 0x3590 ; cmp byte ptr [eax+0x30],0 ; jne <work> ; mov al,1 ; ret 4
// so it CAN silently no-op. Measured, though, that byte reads 1 both in the
// world and during a load, so the guard is not what blocks on-demand spawning
// -- it is checked here only to rule it out. Past the guard the routine also
// demands tag+0x0C == 1 and tag+0x22 != 0 before it will resolve tag+0x08.
#define RVA_SPAWN_GATE  0x3590
#define RVA_TAG_CTOR    0x184C20   // NPCTag: allocates 0x5C, runs the real ctor
// InstancedObjectTag: allocates 0x7C, writes vtable 0x77F640, sets +0x60/64/68
// to the unset-resource sentinel and +0x78 (the NPCTag slot) to null.
#define RVA_IOT_CTOR    0x182DA0
// The game's own entry point:
//   mov eax,ecx / mov ecx,[eax+0x78] / push eax / call [vtable+0x18]
// i.e. a method on the InstancedObjectTag that dispatches to SpawnNPCFromTag.
#define RVA_SPAWN_THUNK 0x182EF0

// Returns the routine's own success flag (al). It sets al=1 only after the
// NPC is registered; the early-out and the factory-failure path both return 0.
// Discarding it meant "spawned 5" was counting objects in memory rather than
// NPCs the game accepted into the world.
static unsigned ConstructedSpawnFwd(unsigned useHash, float* pos);

static unsigned CallSpawnThunk(unsigned iot) {
    unsigned fn = (unsigned)(uintptr_t)RvaPtr(RVA_SPAWN_THUNK), r = 0;
    __asm {
        mov ecx, iot
        call fn
        movzx eax, al
        mov  r, eax
    }
    return r;
}

static unsigned SpawnGateObj() {
    unsigned fn = (unsigned)(uintptr_t)RvaPtr(RVA_SPAWN_GATE), r = 0;
    __try {
        __asm {
            call fn
            mov  r, eax
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return r;
}

int SWSE_SpawnGate(char* msg, int msgLen) {
    char tmp[160];
    unsigned g = SpawnGateObj();
    if (!g) { lstrcpynA(msg, "gate object unavailable", msgLen); return 0; }
    unsigned char v = 0;
    __try { v = *(unsigned char*)(g + 0x30); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        lstrcpynA(msg, "gate object unreadable", msgLen); return 0;
    }
    wsprintfA(tmp, "spawn gate: obj %08X byte[+0x30] = %d (%s)", g, v,
              v ? "OPEN - spawning allowed" : "SHUT - routine returns instantly");
    lstrcpynA(msg, tmp, msgLen);
    return v ? 1 : 0;
}

// Build the same pair npcnow builds, callable from inside the load hook so
// "objects we built" and "outside a load" can be separated as explanations for
// why npcnow's NPCs are not in the world.
static unsigned ConstructedSpawnFwd(unsigned useHash, float* pos) {
    unsigned ctor  = (unsigned)(uintptr_t)RvaPtr(RVA_TAG_CTOR), tag = 0;
    unsigned ictor = (unsigned)(uintptr_t)RvaPtr(RVA_IOT_CTOR), iot = 0;
    unsigned* rec = (unsigned*)g_nprRec;
    unsigned* a0  = (unsigned*)g_nprA0Rec;
    __try {
        __asm {
            call ctor
            mov  tag, eax
        }
        if (!tag) return 0;
        *(unsigned*)(tag + 0x08)      = useHash ? useHash : rec[2];
        *(unsigned*)(tag + 0x0C)      = 1;
        *(unsigned char*)(tag + 0x22) = 1;
        if (pos) {
            float* t = (float*)(tag + 0x30);
            t[0] = pos[0]; t[1] = pos[1]; t[2] = pos[2];
        }
        __asm {
            call ictor
            mov  iot, eax
        }
        if (!iot) return 0;
        *(unsigned*)(iot + 0x08) = a0[0x08 / 4];
        *(unsigned*)(iot + 0x14) = a0[0x14 / 4];
        *(unsigned*)(iot + 0x1C) = a0[0x1C / 4];
        *(unsigned*)(iot + 0x28) = a0[0x28 / 4];
        memcpy((void*)(iot + 0x3C), g_nprA0Rec + 0x3C, 0x24);
        if (pos) {
            float* t = (float*)(iot + 0x3C);
            t[0] = pos[0]; t[1] = pos[1]; t[2] = pos[2];
        }
        *(unsigned*)(iot + 0x78) = tag;
        return CallSpawnThunk(iot);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Point at what you want. Naming the type hashes offline failed -- only 20 of
// the game's character prefs are referenced by path at all, and a hand-written
// CRC-32 variant disagreed with the game's own hasher anyway. But the player is
// usually standing next to the thing they want more of, so identify THAT and
// report which spawn index produces it.
//
// 0x23880 resolves a type handle to its NPCPrefs: called as
//   lea ecx,[tag+8] ; push <out> ; call 0x23880
// so ecx is the handle's address, not its value.
#define RVA_RESOLVE_PREFS 0x23880

static unsigned ResolvePrefs(unsigned hash) {
    unsigned handle = hash;
    unsigned out[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    unsigned fn    = (unsigned)(uintptr_t)RvaPtr(RVA_RESOLVE_PREFS);
    unsigned pOut  = (unsigned)(uintptr_t)&out[0];
    unsigned pH    = (unsigned)(uintptr_t)&handle;
    __try {
        // esp is saved and restored: the callee's cleanup convention here is
        // not established, and guessing wrong corrupts the stack silently.
        __asm {
            mov  edi, esp
            mov  ecx, pH
            push pOut
            call fn
            mov  esp, edi
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return out[0];
}

// ---- AI / combat tuning ---------------------------------------------------
//
// The game's difficulty menu is a UI class only (`MyCallback@Difficulty@@`);
// there is no `difficulty` field anywhere in the reflection schema and no
// 600/300/150 table in the executable. The per-character AI knobs below are a
// separate system, always present, and they are what actually decides how hard
// an encounter plays.
//
// Three objects hang off a character's NPCPrefs:
//
//   NPCPrefs +0x118  m_spAIPrefs      -> perception / alert-state object
//   NPCPrefs +0x498  m_rangedWeapon   -> NPCWeaponPrefs (fire rate, accuracy)
//   NPCPrefs +0x28   m_aiLowDetail    \ AI level-of-detail, the engine's own
//   NPCPrefs +0x2C   m_aiHighDetail   / notion of how much thinking to do
//
// The perception object is dumped by the schema under the name `CoverDuration`,
// which is a misnomer: its fields are sight distances, view-cone angles and a
// separate block per alert state (normal / agitated / combat / panic).
#define AIP_6THSENSE      0x000
#define AIP_SIGHTNORMAL   0x004   // m_seeDistance shares this offset
#define AIP_HIDEVOLSEE    0x01C
#define AIP_SIGHTCOMBAT   0x14C
#define AIP_RELAXAGIT     0x2A0
#define AIP_ATTACKPARAMS  0x2A8

#define NPCW_FIRERATE     0x17C
#define NPCW_RELOADTIME   0x184
#define NPCW_RELOADMAX    0x188
#define NPCW_ACCURACY     0x1A8
#define NPCW_MISSTIME     0x1AC

// The engine's "field is not set" sentinel. It appears as the value of any
// unset hash-valued pref, so it must be distinguished from a real reference.
#define HASH_UNSET        0x2DFD1072

#define NPCP_AILOWDETAIL  0x028
#define NPCP_AIHIGHDETAIL 0x02C
#define NPCP_SPAIPREFS    0x118
#define NPCP_RANGEDWEAPON 0x498

// A read that cannot fault. Returns false rather than a plausible zero, so a
// bad offset is visibly bad instead of quietly reading as "0.0, disabled".
static bool RdU32(unsigned addr, unsigned* out) {
    __try { *out = *(unsigned*)addr; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool RdF32(unsigned addr, float* out) {
    __try { *out = *(float*)addr; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

unsigned SWSE_AiPrefsOf(unsigned typeHash, unsigned* weapon, unsigned* npcPrefs) {
    unsigned p = ResolvePrefs(typeHash);
    if (npcPrefs) *npcPrefs = p;
    if (!p) return 0;
    unsigned ai = 0, w = 0;
    RdU32(p + NPCP_SPAIPREFS, &ai);
    RdU32(p + NPCP_RANGEDWEAPON, &w);
    if (weapon) *weapon = w;
    return ai;
}

// Report the whole tuning block for a character type. Emitting via a callback
// keeps this free of any console dependency, so the same dump can be written
// to a file or driven from a script.
int SWSE_AiDump(unsigned typeHash, void (*emit)(const char*)) {
    char b[220], nm[64];
    unsigned npcPrefs = 0, weapon = 0;
    unsigned ai = SWSE_AiPrefsOf(typeHash, &weapon, &npcPrefs);
    if (!npcPrefs) {
        wsprintfA(b, "%08X did not resolve", typeHash);
        emit(b);
        return 0;
    }
    if (!RttiName(npcPrefs, nm, sizeof(nm))) lstrcpynA(nm, "?", 2);
    wsprintfA(b, "%08X -> NPCPrefs %08X (%s)", typeHash, npcPrefs, nm);
    emit(b);

    unsigned lo = 0, hi = 0;
    if (RdU32(npcPrefs + NPCP_AILOWDETAIL, &lo) &&
        RdU32(npcPrefs + NPCP_AIHIGHDETAIL, &hi)) {
        wsprintfA(b, "  aiDetail       low=%08X high=%08X", lo, hi);
        emit(b);
    }

    // The two linked objects are reported as HASHES with whatever they resolve
    // to. Decoded field values are deliberately NOT printed yet: the resolver
    // hands back objects whose RTTI reads `NPCPrefs` for weapon and AI hashes
    // too, so the per-class offsets cannot be trusted against them. Printing
    // "fireRate=707ms" from a direction vector would look like data.
    // See research/AI_SYSTEMS.md - "the unresolved link".
    unsigned r = 0;
    if (ai == HASH_UNSET || !ai) {
        emit("  m_spAIPrefs    unset");
    } else {
        r = ResolvePrefs(ai);
        if (r && RttiName(r, nm, sizeof(nm))) {}
        else lstrcpynA(nm, "?", 2);
        wsprintfA(b, "  m_spAIPrefs    hash %08X -> %08X (%s)", ai, r, nm);
        emit(b);
    }

    if (weapon == HASH_UNSET || !weapon) {
        emit("  m_rangedWeapon unset (melee-only character)");
    } else {
        r = ResolvePrefs(weapon);
        if (r && RttiName(r, nm, sizeof(nm))) {}
        else lstrcpynA(nm, "?", 2);
        wsprintfA(b, "  m_rangedWeapon hash %08X -> %08X (%s)", weapon, r, nm);
        emit(b);
        emit("    (peek it - field offsets not yet confirmed for this class)");
    }
    return 1;
}

// Write one float field on an AI or weapon prefs object. `which` selects the
// object so a caller never has to know the layout.
int SWSE_AiSet(unsigned typeHash, int onWeapon, int offset, float value) {
    unsigned weapon = 0, npcPrefs = 0;
    unsigned ai = SWSE_AiPrefsOf(typeHash, &weapon, &npcPrefs);
    unsigned obj = onWeapon ? weapon : ai;
    if (!obj) return 0;
    __try {
        DWORD old;
        VirtualProtect((void*)(obj + offset), 4, PAGE_READWRITE, &old);
        *(float*)(obj + offset) = value;
        VirtualProtect((void*)(obj + offset), 4, old, &old);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Find the perception prefs object by its SHAPE rather than by a hash.
//
// The prefs resolver is NPCPrefs-specific: handed a weapon or AI hash it
// returns an unrelated character record instead of failing, so the object
// cannot be reached that way (see research/AI_SYSTEMS.md).
//
// It does not need to be. The class has a near-unique memory signature: four
// sight blocks at a fixed 0xA4 stride, each starting with a sixth-sense
// distance and a sight distance and carrying two view-cone angles in degrees.
// Four of those in a row, all self-consistent, does not occur by accident.
//
//   +0x00  m_6thSenseDistance      0 .. 200
//   +0x04  m_seeDistance           1 .. 1000
//   +0x10  m_horizontalAngleDeg    1 .. 360
//   +0x14  m_verticalAngleDeg      1 .. 360
#define AISIGHT_STRIDE 0xA4
#define AISIGHT_STATES 4

static double AiNowMs() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static bool LooksLikeSight(unsigned p) {
    float f[8];
    __try {
        for (int i = 0; i < 8; i++) f[i] = *(float*)(p + i * 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!(f[0] >= 0.0f   && f[0] <= 200.0f))  return false;   // 6th sense
    if (!(f[1] > 0.5f    && f[1] <= 1000.0f)) return false;   // see distance
    if (!(f[4] > 0.5f    && f[4] <= 360.0f))  return false;   // horiz angle
    if (!(f[5] > 0.5f    && f[5] <= 360.0f))  return false;   // vert angle
    return true;
}

// The object tail, which is what actually pins alignment.
//
//   +0x294 m_allowPanic              a bool - must read as exactly 0 or 1
//   +0x298 m_minWaitForConversation  seconds
//   +0x29C m_maxWaitForConversation  seconds, >= the minimum
//   +0x2A0 m_relaxAgitatedToNormal   seconds
//
// Shipped values are 1 / 45 / 60 / 200. A bool reading 0 or 1 at a fixed
// offset is a strong alignment check: shifted by four bytes it lands on a
// float and fails immediately.
static bool TailIsAiPrefs(unsigned obj) {
    unsigned ap;
    float mn, mx, relax;
    __try {
        ap    = *(unsigned*)(obj + 0x294);
        mn    = *(float*)(obj + 0x298);
        mx    = *(float*)(obj + 0x29C);
        relax = *(float*)(obj + 0x2A0);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (ap > 1) return false;
    if (!(mn >= 0.0f && mn <= 100000.0f)) return false;
    if (!(mx >= mn   && mx <= 100000.0f)) return false;
    if (!(relax >= 0.0f && relax <= 100000.0f)) return false;
    return true;
}

// Cross-state consistency - the check that actually makes this safe to WRITE.
//
// A per-block plausibility test is far too weak on its own: any window of
// believable floats stays believable, so unrelated objects passed it, got
// written, and produced a flashing character model. Twelve of nineteen "hits"
// in one level were not perception objects at all.
//
// Real data has a signature no unrelated object reproduces. Across the four
// alert states, only TWO fields move:
//
//     6th sense   10   10   10   10      identical
//     seeAbove  1001 1001 1001 1001      identical, and == seeBelow
//     seeBelow  1001 1001 1001 1001      identical
//     h angle     90   90   90   90      identical
//     v angle     90   90   90   90      identical
//     instant     10   10   10   10      identical
//     -----------------------------------------------
//     seeDist     50  100  100   75      varies  <- the model
//     hideVol      1    2    3    3      varies  <- the model
//
// Requiring the six invariants to hold across all four blocks is a demanding
// structural test, and it is the difference between "these floats look sane"
// and "this is that class".
static bool CrossStateConsistent(unsigned obj) {
    float v[AISIGHT_STATES][8];
    __try {
        for (int s = 0; s < AISIGHT_STATES; s++) {
            unsigned blk = obj + 0x04 + s * AISIGHT_STRIDE;
            for (int f = 0; f < 8; f++) v[s][f] = *(float*)(blk + f * 4);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }

    // indices: 0=6th 1=see 2=above 3=below 4=hAng 5=vAng 6=instant 7=hideVol
    static const int invariant[] = { 0, 2, 3, 4, 5, 6 };
    for (int i = 0; i < 6; i++) {
        int f = invariant[i];
        for (int s = 1; s < AISIGHT_STATES; s++)
            if (v[s][f] != v[0][f]) return false;
    }
    if (v[0][2] != v[0][3]) return false;      // seeAbove == seeBelow
    if (v[0][2] < 10.0f)     return false;     // vertical reach is generous
    // A perception object whose sight never changes with alertness would be
    // indistinguishable from a coincidence; require the model to be present.
    bool varies = false;
    for (int s = 1; s < AISIGHT_STATES; s++)
        if (v[s][1] != v[0][1] || v[s][7] != v[0][7]) { varies = true; break; }
    return varies;
}

// ---- NPCWeaponPrefs, located by shape (READ ONLY) -------------------------
//
// Same problem as the perception object: the prefs resolver is NPCPrefs-only,
// so a weapon hash cannot be turned into its object. Same solution - match the
// class by its layout. NPCWeaponPrefs registers a contiguous 4-byte-stepped
// block, which is a usable signature:
//
//   +0x178 m_weaponType        small enum
//   +0x17C m_fireRate          seconds between shots
//   +0x184 m_reloadTime        seconds
//   +0x188 m_reloadTimeMax     >= m_reloadTime
//   +0x18C m_hitConeAngleXY    degrees
//   +0x190 m_hitConeAngleZ     degrees
//   +0x1A8 m_accuracyWidth
//   +0x1AC m_missTime          seconds spent deliberately missing
//
// This signature is WEAKER than the perception one (no cross-state invariants
// to lean on), so nothing here writes. It reports what it finds and the values
// are read by eye. Writing through a weak signature is exactly what corrupted
// the game earlier.
#define NW_BASE       0x178
#define NW_FIRERATE   0x17C
#define NW_RELOAD     0x184
#define NW_RELOADMAX  0x188
#define NW_CONEXY     0x18C
#define NW_CONEZ      0x190
#define NW_ACCURACY   0x1A8
#define NW_MISSTIME   0x1AC

// The class is identified by its VTABLE, recovered from RTTI
// (`.?AVNPCWeaponPrefs@@` -> 0x776454 at the linked base of 0x400000, so
// RVA 0x376454). This replaced a shape-based guess that produced nothing but
// false positives: matching "a run of plausible floats" is not identification,
// and writing through it is how the character model got corrupted earlier.
//
// The RVA is self-checking - the same recovery gives NPCPrefs 0x767E1C, which
// matches the 0x367E1C already hardcoded in PrefsOfNpc from separate work.
#define NPCWEAPONPREFS_RVA 0x376454

static unsigned NpcWeaponVTable() {
    return (unsigned)(uintptr_t)GetModuleHandleA(NULL) + NPCWEAPONPREFS_RVA;
}

static bool LooksLikeWeaponPrefs(unsigned obj) {
    unsigned wt;
    float fr, rt, rm, cxy, cz, acc, miss;
    __try {
        wt   = *(unsigned*)(obj + NW_BASE);
        fr   = *(float*)(obj + NW_FIRERATE);
        rt   = *(float*)(obj + NW_RELOAD);
        rm   = *(float*)(obj + NW_RELOADMAX);
        cxy  = *(float*)(obj + NW_CONEXY);
        cz   = *(float*)(obj + NW_CONEZ);
        acc  = *(float*)(obj + NW_ACCURACY);
        miss = *(float*)(obj + NW_MISSTIME);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    // The vtable has already established what this object IS. These are only
    // sanity bounds, to skip an allocated-but-not-yet-populated instance.
    // A fire rate of exactly 0 is legitimate (melee, or a non-firing entry),
    // so it must NOT be rejected the way a shape-matcher would have to.
    (void)wt;
    if (!(fr   >= 0.0f && fr   <= 600.0f))  return false;
    if (!(rt   >= 0.0f && rt   <= 600.0f))  return false;
    if (!(rm   >= 0.0f && rm   <= 600.0f))  return false;
    if (!(cxy  >= 0.0f && cxy  <= 720.0f))  return false;
    if (!(cz   >= 0.0f && cz   <= 720.0f))  return false;
    if (!(acc  >= 0.0f && acc  <= 10000.0f))return false;
    if (!(miss >= 0.0f && miss <= 600.0f))  return false;
    return true;
}

int SWSE_FindWeaponPrefs(unsigned* out, int max, double budgetMs) {
    int found = 0;
    double t0 = AiNowMs();
    MEMORY_BASIC_INFORMATION mbi;
    unsigned addr = HEAP_LO;
    while (addr < HEAP_HI && found < max) {
        if (!VirtualQuery((void*)addr, &mbi, sizeof(mbi))) break;
        unsigned base = (unsigned)(uintptr_t)mbi.BaseAddress;
        unsigned size = (unsigned)mbi.RegionSize;
        bool usable = (mbi.State == MEM_COMMIT) &&
                      (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_READONLY);
        if (usable) {
            unsigned end = base + size;
            if (end > HEAP_HI) end = HEAP_HI;
            unsigned want = NpcWeaponVTable();
            for (unsigned p = base; p + 0x1C0 < end; p += 4) {
                if ((p & 0xFFFF) == 0 && AiNowMs() - t0 > budgetMs) return found;
                unsigned vt;
                __try { vt = *(unsigned*)p; }
                __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
                if (vt != want) continue;
                // The vtable pins the class; the range test then rejects the
                // rare object that is real but not yet initialised.
                if (!LooksLikeWeaponPrefs(p)) continue;
                out[found++] = p;
                if (found >= max) break;
            }
        }
        addr = base + size;
        if (size == 0) break;
    }
    return found;
}

// ---- join characters to their guns ---------------------------------------
//
// A bare list of fire rates is not actionable - "1300ms" tells you nothing
// without knowing WHOSE gun it is. Both classes are locatable by vtable and
// both carry their own path hash at +0x0C, so the two can be joined:
//
//     NPCPrefs +0x498  m_rangedWeapon  ==  NPCWeaponPrefs +0x0C  (own hash)
//
// Health and kill bounty come along for identification, because
// research/NPC_TUNING.md indexes the recovered character NAMES by stock health
// (outlaw cutter 45, outlaw shooter 60, Jo Momma 1500, ...).
#define NPCPREFS_RVA      0x367E1C
#define NPCP_OWNHASH      0x00C
#define NPCP_HEALTH       0x448
#define NPCP_KILLMOOLAH   0x4A8
#define NPCP_RANGED       0x498
#define PREFS_OWNHASH     0x00C

// Walk every object of one class, calling `visit` for each. One helper for
// both passes keeps the region/protection/budget logic in a single place.
typedef void (*PrefsVisit)(unsigned obj, void* ctx);

static void WalkByVTable(unsigned wantVT, double t0, double budgetMs,
                         PrefsVisit visit, void* ctx) {
    MEMORY_BASIC_INFORMATION mbi;
    unsigned addr = HEAP_LO;
    while (addr < HEAP_HI) {
        if (!VirtualQuery((void*)addr, &mbi, sizeof(mbi))) break;
        unsigned base = (unsigned)(uintptr_t)mbi.BaseAddress;
        unsigned size = (unsigned)mbi.RegionSize;
        bool usable = (mbi.State == MEM_COMMIT) &&
                      (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_READONLY);
        if (usable) {
            unsigned end = base + size;
            if (end > HEAP_HI) end = HEAP_HI;
            for (unsigned p = base; p + 0x500 < end; p += 4) {
                if ((p & 0xFFFF) == 0 && AiNowMs() - t0 > budgetMs) return;
                unsigned vt;
                __try { vt = *(unsigned*)p; }
                __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
                if (vt == wantVT) visit(p, ctx);
            }
        }
        addr = base + size;
        if (size == 0) break;
    }
}

struct WepIndex { unsigned hash[128]; unsigned addr[128]; int n; };

static void CollectWeapon(unsigned obj, void* ctx) {
    WepIndex* w = (WepIndex*)ctx;
    if (w->n >= 128) return;
    __try {
        w->hash[w->n] = *(unsigned*)(obj + PREFS_OWNHASH);
        w->addr[w->n] = obj;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    w->n++;
}

struct GunJoin { NpcGunRow* out; int max; int n; WepIndex* wep; };

static void CollectNpc(unsigned obj, void* ctx) {
    GunJoin* j = (GunJoin*)ctx;
    if (j->n >= j->max) return;
    NpcGunRow r;
    __try {
        r.npcHash    = *(unsigned*)(obj + NPCP_OWNHASH);
        r.health     = *(float*)(obj + NPCP_HEALTH);
        r.killMoolah = *(float*)(obj + NPCP_KILLMOOLAH);
        r.weaponHash = *(unsigned*)(obj + NPCP_RANGED);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    r.weaponAddr = 0;
    r.fireRate   = -1.0f;
    for (int i = 0; i < j->wep->n; i++) {
        if (j->wep->hash[i] != r.weaponHash) continue;
        r.weaponAddr = j->wep->addr[i];
        __try { r.fireRate = *(float*)(r.weaponAddr + 0x17C); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        break;
    }
    if (!r.weaponAddr) return;              // melee-only, or unset
    for (int i = 0; i < j->n; i++)
        if (j->out[i].npcHash == r.npcHash) return;   // already have this type
    j->out[j->n++] = r;
}

int SWSE_NpcGuns(NpcGunRow* out, int max, double budgetMs) {
    double t0 = AiNowMs();
    static WepIndex wep;
    wep.n = 0;
    WalkByVTable(NpcWeaponVTable(), t0, budgetMs * 0.5, CollectWeapon, &wep);

    GunJoin j;
    j.out = out; j.max = max; j.n = 0; j.wep = &wep;
    unsigned npcVT = (unsigned)(uintptr_t)GetModuleHandleA(NULL) + NPCPREFS_RVA;
    WalkByVTable(npcVT, t0, budgetMs, CollectNpc, &j);
    return j.n;
}

// Scan the heap for the four-block pattern. Budgeted like the NPC scan: this
// is called from the console, never from a render or bind hook.
int SWSE_FindAiPrefs(unsigned* out, int max, double budgetMs) {
    int found = 0;
    double t0 = AiNowMs();
    MEMORY_BASIC_INFORMATION mbi;
    unsigned addr = HEAP_LO;
    while (addr < HEAP_HI && found < max) {
        if (!VirtualQuery((void*)addr, &mbi, sizeof(mbi))) break;
        unsigned base = (unsigned)(uintptr_t)mbi.BaseAddress;
        unsigned size = (unsigned)mbi.RegionSize;
        bool usable = (mbi.State == MEM_COMMIT) &&
                      (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_READONLY);
        if (usable) {
            unsigned end = base + size;
            if (end > HEAP_HI) end = HEAP_HI;
            unsigned lastAccepted = 0;
            for (unsigned p = base; p + 0x2C0 < end; p += 4) {
                if ((p & 0xFFFF) == 0 && AiNowMs() - t0 > budgetMs) return found;
                // sightNormal sits at +4, so a candidate object starts 4 back
                if (!LooksLikeSight(p)) continue;
                if (!LooksLikeSight(p + AISIGHT_STRIDE)) continue;
                if (!LooksLikeSight(p + AISIGHT_STRIDE * 2)) continue;
                if (!LooksLikeSight(p + AISIGHT_STRIDE * 3)) continue;
                unsigned obj = p - 4;
                // The sight test alone matches at several consecutive +4
                // slides, because a window of plausible floats stays plausible
                // when shifted. Writing through a misaligned base corrupts the
                // real object's neighbouring fields - it crashed the game once.
                // The tail pins alignment exactly: allowPanic is a bool, and
                // the conversation waits and relax timer are bounded.
                if (!TailIsAiPrefs(obj)) continue;
                if (!CrossStateConsistent(obj)) continue;
                if (lastAccepted && obj - lastAccepted < 0x2B0) continue;
                lastAccepted = obj;
                out[found++] = obj;
                if (found >= max) break;
            }
        }
        addr = base + size;
        if (size == 0) break;
    }
    return found;
}

// An NPC holds a pointer to its NPCPrefs somewhere in its header; rather than
// hardcode an offset, look for the one field that points at an object with the
// NPCPrefs vtable.
static unsigned PrefsOfNpc(unsigned npc) {
    unsigned want = (unsigned)(uintptr_t)GetModuleHandleA(NULL) + 0x367E1C;  // NPCPrefs
    __try {
        for (int o = 0; o < 0x600; o += 4) {
            unsigned p = *(unsigned*)(npc + o);
            if (p < HEAP_LO || p > HEAP_HI) continue;
            if (*(unsigned*)p == want) return p;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}

// Does the game's resolver actually accept a given type hash? Type selection
// keeps producing the captured character, and the two candidate explanations --
// "the hash never resolves" and "the type is read from somewhere else" -- need
// separating before any more guessing at fields.
int SWSE_Resolve(unsigned hash, char* msg, int msgLen) {
    char tmp[220], nm[64];
    unsigned p = ResolvePrefs(hash);
    if (!p) {
        wsprintfA(tmp, "%08X did not resolve (null)", hash);
        lstrcpynA(msg, tmp, msgLen);
        return 0;
    }
    unsigned own = 0;
    __try { own = *(unsigned*)(p + 0x0C); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (!RttiName(p, nm, sizeof(nm))) lstrcpynA(nm, "?", 2);
    wsprintfA(tmp, "%08X -> %08X (%s) whose own hash is %08X %s",
              hash, p, nm, own, (own == hash) ? "- consistent" : "- MISMATCH");
    lstrcpynA(msg, tmp, msgLen);
    return 1;
}

// Relocate live NPCs to the player.
//
// This engine never creates NPCs at runtime -- measured across a sleg ambush, a
// boss fight and tunnel emergences, all with zero calls to the spawn routine
// and a strictly falling NPC count. Every NPC exists from level load, and
// encounters simply walk pre-placed ones out of hiding. So "summon enemies" here
// means MOVING ones that already exist, which is also what makes a pre-loaded
// pool of spare NPCs workable.
//
// The actor's +0x24 position is a COPY; the motion object owns the real one
// (back-pointer at +0x4C holds actor+8, position at +0x50). Writing only the
// copy is why player teleports reverted -- the source overwrites it next frame.
// One heap pass collects motion objects for every NPC we intend to move,
// instead of a full scan per NPC.
int SWSE_BringNpcs(int count, unsigned typeHash, char* msg, int msgLen) {
    char tmp[220], b[160];
    float* pp = PlayerPos();
    if (!pp) { lstrcpynA(msg, "no player position", msgLen); return 0; }
    if (pp[0] == 0.0f && pp[1] == 0.0f && pp[2] == 0.0f) {
        lstrcpynA(msg, "player at origin - level still loading", msgLen);
        return 0;
    }
    float px = pp[0], py = pp[1], pz = pp[2];

    int n = SWSE_FindNpcs(g_snScan, 1024);
    if (n <= 0) { lstrcpynA(msg, "no live NPCs", msgLen); return 0; }

    // Pick candidates: right type, and not already on top of us.
    static unsigned pick[64];
    int np = 0;
    for (int i = 0; i < n && np < count && np < 64; i++) {
        unsigned a = g_snScan[i];
        __try {
            if (typeHash) {
                unsigned pf = PrefsOfNpc(a);
                if (!pf || *(unsigned*)(pf + 0x0C) != typeHash) continue;
            }
            float* q = (float*)(a + NPC_POS);
            float dx = q[0] - px, dy = q[1] - py;
            if (dx * dx + dy * dy < 64.0f) continue;      // already nearby
            pick[np++] = a;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (!np) { lstrcpynA(msg, "no NPCs of that type to bring", msgLen); return 0; }

    // One pass for the motion objects that own their positions.
    static unsigned motion[64];
    for (int i = 0; i < np; i++) motion[i] = 0;
    SYSTEM_INFO si; GetSystemInfo(&si);
    BYTE* p  = (BYTE*)si.lpMinimumApplicationAddress;
    BYTE* hi = (BYTE*)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi;
    while (p < hi) {
        if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
        bool ok = mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE
               && HeapRegion(mbi)
               && (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))
               && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS));
        if (ok) {
            __try {
                unsigned* q = (unsigned*)mbi.BaseAddress;
                size_t cnt = mbi.RegionSize / 4;
                for (size_t k = 0; k < cnt; k++) {
                    unsigned v = q[k];
                    for (int i = 0; i < np; i++) {
                        if (motion[i] || v != pick[i] + 8) continue;
                        unsigned h = (unsigned)(uintptr_t)(q + k);
                        if (h < 0x4C) continue;
                        motion[i] = h - 0x4C;              // MO_OWNER
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        p = (BYTE*)mbi.BaseAddress + mbi.RegionSize;
    }

    static const float kRing[8][2] = {
        { 1.000f,  0.000f}, { 0.707f,  0.707f}, { 0.000f,  1.000f}, {-0.707f,  0.707f},
        {-1.000f,  0.000f}, {-0.707f, -0.707f}, { 0.000f, -1.000f}, { 0.707f, -0.707f},
    };
    int moved = 0, noMotion = 0;
    for (int i = 0; i < np; i++) {
        float r = 5.0f + 2.5f * (float)(i / 8);
        float tx = px + kRing[i % 8][0] * r;
        float ty = py + kRing[i % 8][1] * r;
        __try {
            // The copy, so anything reading the actor directly agrees...
            float* c = (float*)(pick[i] + NPC_POS);
            c[0] = tx; c[1] = ty; c[2] = pz;
            // ...and the source, so it is not reverted next frame.
            if (motion[i]) {
                float* m = (float*)(motion[i] + 0x50);
                m[0] = tx; m[1] = ty; m[2] = pz;
                moved++;
            } else {
                noMotion++;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (i < 4) {
            wsprintfA(b, "  bring %08X motion %08X -> %d %d %d",
                      pick[i], motion[i], (int)tx, (int)ty, (int)pz);
            LogS(b);
        }
    }
    wsprintfA(tmp, "brought %d of %d (%d had no motion object) to %d %d %d",
              moved, np, noMotion, (int)px, (int)py, (int)pz);
    lstrcpynA(msg, tmp, msgLen);
    return moved;
}

// Every named object near the player, whatever its class.
// npcnear only matches the NPC vtable, so a Clakkerz standing right next to
// the player did not show up in it. This scans anything carrying RTTI whose
// +0x24 (PF_POS, the actor position copy) lands near the player and names it.
// Heuristic: +0x24 is only a position on actors, so unrelated objects can
// slip through when their bytes happen to look like nearby coordinates.
int SWSE_Nearby(int radius, char* msg, int msgLen) {
    char tmp[200], b[190];
    float* pp = PlayerPos();
    if (!pp) { lstrcpynA(msg, "no player position", msgLen); return 0; }
    float px = pp[0], py = pp[1], pz = pp[2];
    float r2 = (float)(radius * radius);

    int found = 0;
    SYSTEM_INFO si; GetSystemInfo(&si);
    BYTE* p  = (BYTE*)si.lpMinimumApplicationAddress;
    BYTE* hi = (BYTE*)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi;
    unsigned base = (unsigned)(uintptr_t)GetModuleHandleA(NULL);
    while (p < hi && found < 40) {
        if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
        bool ok = mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE
               && HeapRegion(mbi)
               && (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))
               && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS));
        if (ok) {
            __try {
                unsigned* q = (unsigned*)mbi.BaseAddress;
                size_t cnt = mbi.RegionSize / 4;
                for (size_t k = 0; k + 16 < cnt && found < 40; k++) {
                    unsigned vt = q[k];
                    if (vt < base || vt > base + 0x01000000) continue;
                    unsigned a = (unsigned)(uintptr_t)(q + k);
                    if (a < HEAP_LO || a > HEAP_HI) continue;
                    float* pos = (float*)(a + NPC_POS);
                    float dx = pos[0] - px, dy = pos[1] - py, dz = pos[2] - pz;
                    float d2 = dx * dx + dy * dy + dz * dz;
                    if (!(d2 >= 0.0f && d2 < r2)) continue;
                    char nm[80];
                    if (!RttiName(a, nm, sizeof(nm))) continue;
                    // For characters, name the TYPE too -- otherwise every
                    // townsfolk variant reads as an identical "NPC" and you
                    // cannot tell which one is still unkillable.
                    unsigned th = 0;
                    __try {
                        unsigned pf = PrefsOfNpc(a);
                        if (pf) th = *(unsigned*)(pf + 0x0C);
                    } __except (EXCEPTION_EXECUTE_HANDLER) {}
                    int dist = (int)((dx * dx + dy * dy + dz * dz) / 8.0f);
                    if (th)
                        wsprintfA(b, "  %-22s %08X type %08X  at %d %d %d (d2/8=%d)",
                                  nm, a, th, (int)pos[0], (int)pos[1], (int)pos[2], dist);
                    else
                        wsprintfA(b, "  %-22s %08X                at %d %d %d (d2/8=%d)",
                                  nm, a, (int)pos[0], (int)pos[1], (int)pos[2], dist);
                    LogS(b);
                    found++;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        p = (BYTE*)mbi.BaseAddress + mbi.RegionSize;
    }
    wsprintfA(tmp, "%d named object(s) within %d units - see the log", found, radius);
    lstrcpynA(msg, tmp, msgLen);
    return found;
}

// Set a character TYPE's health, and every live NPC of that type with it.
//
// Townsfolk (Clakkerz) are not flagged invulnerable and are not a special
// class -- NPCPrefs.m_health is simply 100000.0 where an outlaw cutter has
// 45.0. Immortality is a big number, so mortality is a small one.
//
// Writing the prefs governs anything spawned later; live NPCs carry their own
// health (PF_HEALTH, the current/max/base triple at +0x78) and are set too, so
// the change takes effect without a reload.
#define NPCP_HEALTH_OFF 0x448

int SWSE_SetTypeHealth(unsigned hash, float health, char* msg, int msgLen) {
    char tmp[220];
    unsigned prefs = ResolvePrefs(hash);
    if (!prefs) {
        wsprintfA(tmp, "%08X did not resolve to a prefs object", hash);
        lstrcpynA(msg, tmp, msgLen);
        return 0;
    }
    float was = 0.0f;
    __try {
        was = *(float*)(prefs + NPCP_HEALTH_OFF);
        *(float*)(prefs + NPCP_HEALTH_OFF)     = health;
        *(float*)(prefs + NPCP_HEALTH_OFF + 4) = health;   // the max alongside it
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        lstrcpynA(msg, "prefs unwritable", msgLen);
        return -2;
    }

    // Live ones too, or nothing changes until the level reloads.
    int live = 0;
    int n = SWSE_FindNpcs(g_snScan, 1024);
    for (int i = 0; i < n; i++) {
        unsigned a = g_snScan[i];
        __try {
            unsigned pf = PrefsOfNpc(a);
            if (!pf || *(unsigned*)(pf + 0x0C) != hash) continue;
            float* h = (float*)(a + 0x78);          // current / max / base
            if (h[0] > health) h[0] = health;
            h[1] = health;
            h[2] = health;
            live++;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    wsprintfA(tmp, "%08X health %d -> %d (%d live updated)",
              hash, (int)was, (int)health, live);
    lstrcpynA(msg, tmp, msgLen);
    return 1;
}

// Promote a FRACTION of the live NPCs of a type into elites.
//
// npchealth/npchurt act on the type, so they convert every cutter in the level
// at once - an encounter of nothing but 10,000hp heavies. What makes a level
// interesting is the occasional one, so this writes health on individual actors
// instead: same field the damage watch already reads (actor+0x78, the
// current/max/base triple), just applied to a random subset.
//
// NOTE: m_hurtReaction lives on the PREFS (+0x484), not the actor, so
// "unflinchable" cannot be made per-NPC this way - it is a property of the
// type. Health can, and health is what makes an elite survive long enough to
// matter.
int SWSE_MakeElites(unsigned hash, int percent, float health, char* msg, int msgLen) {
    char tmp[220];
    if (percent < 1)   percent = 1;
    if (percent > 100) percent = 100;

    int n = SWSE_FindNpcs(g_snScan, 1024);
    if (n <= 0) { lstrcpynA(msg, "no live NPCs", msgLen); return 0; }

    // Collect the candidates first so the percentage is of the matching type,
    // not of every NPC in the level.
    static unsigned cand[1024];
    int nc = 0;
    for (int i = 0; i < n && nc < 1024; i++) {
        unsigned a = g_snScan[i];
        __try {
            unsigned pf = PrefsOfNpc(a);
            if (!pf) continue;
            if (hash && *(unsigned*)(pf + 0x0C) != hash) continue;
            cand[nc++] = a;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (!nc) { lstrcpynA(msg, "no live NPCs of that type", msgLen); return 0; }

    int want = (nc * percent) / 100;
    if (want < 1) want = 1;

    // Spread the picks evenly rather than clustering at the start of the list,
    // so the elites are scattered through the level.
    int done = 0;
    for (int k = 0; k < want; k++) {
        int idx = (int)(((long long)k * nc) / want);
        if (idx >= nc) idx = nc - 1;
        __try {
            float* h = (float*)(cand[idx] + 0x78);
            h[0] = health; h[1] = health; h[2] = health;
            done++;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    wsprintfA(tmp, "%d of %d live %08X promoted to %d hp (%d%%)",
              done, nc, hash, (int)health, percent);
    lstrcpynA(msg, tmp, msgLen);
    return done;
}

// Gib a character type on death instead of playing a death animation.
// m_onDeathGib is a bool at prefs+0x460 and reads 0 for every character dumped
// so far, including outlaws -- so gibbing is opt-in per character rather than
// something only certain enemies do. m_allowOnDeathGibFromBolts (+0x461) is
// already 1 on both, so bolt kills should gib once the first flag is set.
// m_hurtReaction: 0 = staggers normally, 2 = unflinchable (what the game's
// heavies use). Also applied to live NPCs of the type so it takes effect now.
int SWSE_SetTypeHurt(unsigned hash, int value, char* msg, int msgLen) {
    char tmp[200];
    unsigned prefs = ResolvePrefs(hash);
    if (!prefs) {
        wsprintfA(tmp, "%08X did not resolve to a prefs object", hash);
        lstrcpynA(msg, tmp, msgLen);
        return 0;
    }
    unsigned was = 0;
    __try {
        was = *(unsigned*)(prefs + 0x484);
        *(unsigned*)(prefs + 0x484) = (unsigned)value;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        lstrcpynA(msg, "prefs unwritable", msgLen);
        return -2;
    }
    wsprintfA(tmp, "%08X hurtReaction %u -> %d  (0=staggers, 2=unflinchable)",
              hash, was, value);
    lstrcpynA(msg, tmp, msgLen);
    return 1;
}

// m_affGenerally (+0x4E8) is a character's affiliation. Outlaws and townsfolk
// both read 1, which is why armed outlaws stand around in a town without
// anyone minding: same side. Everyone's hostility points at the player instead.
// Changing it is the obvious lever for making a town raid -- enemies that the
// townsfolk actually treat as enemies.
int SWSE_SetTypeAff(unsigned hash, int value, char* msg, int msgLen) {
    char tmp[200];
    unsigned prefs = ResolvePrefs(hash);
    if (!prefs) {
        wsprintfA(tmp, "%08X did not resolve to a prefs object", hash);
        lstrcpynA(msg, tmp, msgLen);
        return 0;
    }
    unsigned was = 0;
    __try {
        was = *(unsigned*)(prefs + 0x4E8);
        *(unsigned*)(prefs + 0x4E8) = (unsigned)value;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        lstrcpynA(msg, "prefs unwritable", msgLen);
        return -2;
    }
    // Live NPCs of this type may cache it; report how many exist so a lack of
    // effect can be told apart from "the change did not reach anyone".
    int live = 0;
    int n = SWSE_FindNpcs(g_snScan, 1024);
    for (int i = 0; i < n; i++) {
        __try {
            unsigned pf = PrefsOfNpc(g_snScan[i]);
            if (pf && *(unsigned*)(pf + 0x0C) == hash) live++;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    wsprintfA(tmp, "%08X affiliation %u -> %d  (%d live of this type)",
              hash, was, value, live);
    lstrcpynA(msg, tmp, msgLen);
    return 1;
}

int SWSE_SetTypeGib(unsigned hash, int on, char* msg, int msgLen) {
    char tmp[200];
    unsigned prefs = ResolvePrefs(hash);
    if (!prefs) {
        wsprintfA(tmp, "%08X did not resolve to a prefs object", hash);
        lstrcpynA(msg, tmp, msgLen);
        return 0;
    }
    unsigned char was = 0, wasBolts = 0;
    __try {
        was      = *(unsigned char*)(prefs + 0x460);
        wasBolts = *(unsigned char*)(prefs + 0x461);
        *(unsigned char*)(prefs + 0x460) = (unsigned char)(on ? 1 : 0);
        *(unsigned char*)(prefs + 0x461) = (unsigned char)(on ? 1 : wasBolts);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        lstrcpynA(msg, "prefs unwritable", msgLen);
        return -2;
    }
    wsprintfA(tmp, "%08X onDeathGib %d -> %d (allowFromBolts was %d)",
              hash, was, on ? 1 : 0, wasBolts);
    lstrcpynA(msg, tmp, msgLen);
    return 1;
}

// Apply to EVERY character this level uses, rather than one hash at a time.
// The harvested type list is exactly "what this level spawns", so it is the
// right set to sweep -- and since it is per-level, nothing foreign gets touched.
// health < 0 leaves health alone; gib < 0 leaves gibbing alone.
int SWSE_SetAllTypes(float health, int gib, char* msg, int msgLen) {
    char tmp[220], sub[220];
    int n = SWSE_NpcTypeCount();
    if (n <= 0) {
        lstrcpynA(msg, "no types harvested - run npcspy, then warp or load", msgLen);
        return 0;
    }
    int done = 0;
    for (int i = 0; i < n; i++) {
        unsigned h = SWSE_NpcTypeHash(i);
        if (!h) continue;
        if (health >= 0.0f && SWSE_SetTypeHealth(h, health, sub, sizeof(sub)) == 1) done++;
        if (gib >= 0) SWSE_SetTypeGib(h, gib, sub, sizeof(sub));
    }
    wsprintfA(tmp, "applied to %d of %d character type(s) in this level", done, n);
    lstrcpynA(msg, tmp, msgLen);
    return done;
}

// Read characters.txt. Called at startup and by the `tuning` command, so a file
// edit takes effect on the next level load without restarting the game.
// settings.txt -- named toggles, e.g.
//     noimmortals  = 100      (or "off")
//     immortalsgib = on
// Kept separate from characters.txt: one is "what do I want on", the other is
// per-character detail.
static bool SettingTruthy(const char* v) {
    return !lstrcmpiA(v, "on") || !lstrcmpiA(v, "true") || !lstrcmpiA(v, "yes")
        || !lstrcmpiA(v, "1");
}

int SWSE_LoadSettings(const char* path, char* msg, int msgLen) {
    char tmp[220];
    g_noImmortals = -1; g_immortalsGib = -1;

    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        lstrcpynA(msg, "no settings.txt - all features off", msgLen);
        return 0;
    }
    static char buf[4096];
    DWORD got = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &got, NULL);
    CloseHandle(h);
    buf[got] = 0;

    int applied = 0;
    char* p = buf;
    while (*p) {
        char* line = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = 0;
        while (*line == ' ' || *line == '\t') line++;
        if (!*line || *line == '#' || *line == ';' || *line == '\r') continue;

        char key[48] = { 0 }, val[48] = { 0 };
        int ki = 0, vi = 0;
        bool afterEq = false;
        for (char* c = line; *c; c++) {
            if (*c == '#' || *c == ';' || *c == '\r') break;
            if (*c == '=') { afterEq = true; continue; }
            if (*c == ' ' || *c == '\t') continue;
            if (!afterEq) { if (ki < 47) key[ki++] = *c; }
            else          { if (vi < 47) val[vi++] = *c; }
        }
        if (!key[0] || !val[0]) continue;

        if (!lstrcmpiA(key, "noimmortals")) {
            if (!lstrcmpiA(val, "off") || !lstrcmpiA(val, "false")) g_noImmortals = -1;
            else g_noImmortals = SettingTruthy(val) ? 100 : atoi(val);
            applied++;
        } else if (!lstrcmpiA(key, "immortalsgib")) {
            g_immortalsGib = SettingTruthy(val) ? 1 : 0;
            applied++;
        }
    }
    g_tuneLoaded = true;
    wsprintfA(tmp, "settings: noimmortals=%d immortalsgib=%d (%d applied)",
              g_noImmortals, g_immortalsGib, applied);
    lstrcpynA(msg, tmp, msgLen);
    return applied;
}

int SWSE_LoadTuning(const char* path, char* msg, int msgLen) {
    char tmp[220];
    g_tuneCount = 0; g_tuneAllHp = -1.0f; g_tuneAllGib = -1;
    g_tuneAllHurt = -1; g_tuneLoaded = false;

    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        wsprintfA(tmp, "no characters.txt (looked in %s)", path);
        lstrcpynA(msg, tmp, msgLen);
        return 0;
    }
    static char buf[8192];
    DWORD got = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &got, NULL);
    CloseHandle(h);
    buf[got] = 0;

    int lines = 0;
    char* p = buf;
    while (*p && g_tuneCount < 64) {
        char* line = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = 0;
        while (*line == ' ' || *line == '\t') line++;
        if (!*line || *line == '#' || *line == ';' || *line == '\r') continue;

        char tok[4][32] = { { 0 }, { 0 }, { 0 }, { 0 } };
        int ti = 0, ci = 0;
        for (char* c = line; *c && ti < 4; c++) {
            if (*c == ' ' || *c == '\t' || *c == '\r') {
                if (ci) { ti++; ci = 0; }
            } else if (ci < 31) {
                tok[ti][ci++] = *c;
            }
        }
        if (!tok[0][0]) continue;

        float hp = (tok[1][0] == '-' && !tok[1][1]) ? -1.0f
                 : (tok[1][0] ? (float)atoi(tok[1]) : -1.0f);
        int  gb = (tok[2][0] == '-' && !tok[2][1]) ? -1
                 : (tok[2][0] ? atoi(tok[2]) : -1);
        int  hr = (tok[3][0] == '-' && !tok[3][1]) ? -1
                 : (tok[3][0] ? atoi(tok[3]) : -1);

        if (tok[0][0] == '*' && !tok[0][1]) {
            g_tuneAllHp = hp; g_tuneAllGib = gb; g_tuneAllHurt = hr;
        } else {
            g_tune[g_tuneCount].hash   = (unsigned)strtoul(tok[0], nullptr, 16);
            g_tune[g_tuneCount].health = hp;
            g_tune[g_tuneCount].gib    = gb;
            g_tune[g_tuneCount].hurt   = hr;
            g_tuneCount++;
        }
        lines++;
    }
    g_tuneLoaded = true;
    wsprintfA(tmp, "tuning: %d rule(s) + wildcard hp=%d gib=%d - applies on next load",
              g_tuneCount, (int)g_tuneAllHp, g_tuneAllGib);
    lstrcpynA(msg, tmp, msgLen);
    return lines;
}

// Read a character type's current health and gib flag.
int SWSE_TypeInfo2(unsigned hash, int* hpOut, int* gibOut, int* hurtOut, int* affOut) {
    unsigned prefs = ResolvePrefs(hash);
    if (!prefs) return 0;
    __try {
        if (hpOut)   *hpOut   = (int)*(float*)(prefs + 0x448);
        if (gibOut)  *gibOut  = *(unsigned char*)(prefs + 0x460);
        if (hurtOut) *hurtOut = (int)*(unsigned*)(prefs + 0x484);
        if (affOut)  *affOut  = (int)*(unsigned*)(prefs + 0x4E8);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 1;
}

int SWSE_TypeInfo(unsigned hash, int* hpOut, int* gibOut) {
    return SWSE_TypeInfo2(hash, hpOut, gibOut, nullptr, nullptr);
}

// Where are all the NPCs of a given type? Wandering a level looking for one
// retyped spawn in ten does not work, and "I cannot see them" does not
// distinguish "they are far away" from "this type has no visible body".
// The first live NPC of a given type, so two characters can be diffed by name
// rather than by hunting addresses by hand.
unsigned SWSE_FirstNpcOfType(unsigned hash) {
    int n = SWSE_FindNpcs(g_snScan, 1024);
    for (int i = 0; i < n; i++) {
        __try {
            unsigned pf = PrefsOfNpc(g_snScan[i]);
            if (pf && *(unsigned*)(pf + 0x0C) == hash) return g_snScan[i];
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return 0;
}

int SWSE_WhereIs(unsigned hash, char* msg, int msgLen) {
    char tmp[200], b[190];
    float* pp = PlayerPos();
    float px = pp ? pp[0] : 0.0f, py = pp ? pp[1] : 0.0f, pz = pp ? pp[2] : 0.0f;
    int n = SWSE_FindNpcs(g_snScan, 1024);
    int found = 0, shown = 0;
    float nearest = 1e30f;
    for (int i = 0; i < n; i++) {
        unsigned a = g_snScan[i];
        __try {
            unsigned pf = PrefsOfNpc(a);
            if (!pf || *(unsigned*)(pf + 0x0C) != hash) continue;
            found++;
            float* q = (float*)(a + NPC_POS);
            float dx = q[0] - px, dy = q[1] - py, dz = q[2] - pz;
            float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < nearest) nearest = d2;
            if (shown < 8) {
                wsprintfA(b, "  %08X at %d %d %d   (you: %d %d %d)", a,
                          (int)q[0], (int)q[1], (int)q[2], (int)px, (int)py, (int)pz);
                LogS(b);
                shown++;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (!found) {
        wsprintfA(tmp, "no live NPCs of type %08X in this level", hash);
        lstrcpynA(msg, tmp, msgLen);
        return 0;
    }
    int d = 0;
    float g = 1.0f;
    for (int k = 0; k < 400 && g * g < nearest; k++) g += 1.0f;
    d = (int)g;
    wsprintfA(tmp, "%d live %08X; nearest ~%d units away (positions in the log)",
              found, hash, d);
    lstrcpynA(msg, tmp, msgLen);
    return found;
}

// The town panic system: townsfolk fleeing, alarm bells, turrets waking up.
// Every cue exists twice -- a Steef version and an "Other" version -- so the
// engine was built to panic at threats that are not the player-as-Steef. That
// is the hook for raids: something hostile arrives, the town reacts.
//   +0x14 m_minDistToSteefSqr   +0x1C growRadiusBy   +0x20 beginningRadius
//   +0x24 m_radiusMax           +0x28 followTeleportals   +0x29 panicForever
#define VT_TOWNPANICPREFS 0x370B8C

// PostAlarm({void}|ID) and RingBell -- the game's own "something is happening
// in town" verbs. The generated call table lists PostAlarm as taking no args,
// which is why calling it bare faulted: it wants an ID, almost certainly the
// panic zone id found at controller+0x08.
//
// This matters because killing an NPC already makes the town react -- the
// trigger exists, it is just always attributed to the player. If the alarm can
// be posted directly, a raid does not need NPC-vs-NPC hostility (which this
// engine does not have) -- only a reason for the town to panic.
#define RVA_PostAlarm 0x14CE10
#define RVA_RingBell  0x15DBD0

// The VM argument-injection machinery lives further down.
static bool  InstallArgHook();
static void  RemoveArgHook();
static void  SetArg(int i, unsigned payload, unsigned type);
static void  SetArgId(int i, unsigned id);
static void* CallWithArgs(unsigned rva, void* retBuf);

int SWSE_PostAlarm(unsigned zoneId, int useBell, char* msg, int msgLen) {
    char tmp[200];
    if (!InstallArgHook()) {
        lstrcpynA(msg, "no script context - could not install the arg hook", msgLen);
        return 0;
    }
    int ok = 1;
    __try {
        SetArgId(0, zoneId);             // IDs live at Value+0x04, not +0x10
        char ret[64] = { 0 };
        CallWithArgs(useBell ? RVA_RingBell : RVA_PostAlarm, ret);
    } __except (GrantFilter(GetExceptionInformation(), "postalarm")) { ok = 0; }
    RemoveArgHook();
    wsprintfA(tmp, "%s(%08X): %s", useBell ? "RingBell" : "PostAlarm", zoneId,
              ok ? "called - watch the town" : "faulted (see log)");
    lstrcpynA(msg, tmp, msgLen);
    return ok;
}

// The four panic zone ids of the current level, read from the live controllers.
int SWSE_PanicZones(unsigned* out, int maxOut) {
    unsigned vt = (unsigned)(uintptr_t)((BYTE*)GetModuleHandleA(NULL) + 0x370B5C);
    static unsigned ctrl[16];
    int n = SWSE_FindByVtable(vt, ctrl, 16);
    int k = 0;
    for (int i = 0; i < n && k < maxOut; i++) {
        __try { out[k++] = *(unsigned*)(ctrl[i] + 0x08); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return k;
}

int SWSE_TownPanic(int forever, int radius, char* msg, int msgLen) {
    char tmp[220], b[190];
    unsigned vt = (unsigned)(uintptr_t)((BYTE*)GetModuleHandleA(NULL) + VT_TOWNPANICPREFS);
    static unsigned found[16];
    int n = SWSE_FindByVtable(vt, found, 16);
    if (n <= 0) {
        lstrcpynA(msg, "no TownPanicPrefs in this level", msgLen);
        return 0;
    }
    int touched = 0;
    for (int i = 0; i < n; i++) {
        unsigned p = found[i];
        __try {
            wsprintfA(b, "townpanic %08X: steefDistSqr=%d beginR=%d maxR=%d grow=%d forever=%d",
                      p, (int)*(float*)(p + 0x14), (int)*(float*)(p + 0x20),
                      (int)*(float*)(p + 0x24), (int)*(float*)(p + 0x1C),
                      *(unsigned char*)(p + 0x29));
            LogS(b);
            if (forever >= 0) *(unsigned char*)(p + 0x29) = (unsigned char)(forever ? 1 : 0);
            if (radius > 0) {
                *(float*)(p + 0x20) = (float)radius;          // beginning radius
                *(float*)(p + 0x24) = (float)(radius * 4);    // max radius
            }
            touched++;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    // The prefs say HOW panic behaves; the live controller should say what it
    // is panicking AT. Forcing m_panicForever turned the town hostile to the
    // player (the Steef path) and rang the bells -- so the reaction works, and
    // what remains is pointing it at an enemy instead. The cue fields come in
    // Steef/Other pairs, so that target is chosen somewhere outside the prefs.
    unsigned cvt = (unsigned)(uintptr_t)((BYTE*)GetModuleHandleA(NULL) + 0x370B5C);
    static unsigned ctrl[16];
    int cn = SWSE_FindByVtable(cvt, ctrl, 16);
    for (int i = 0; i < cn && i < 4; i++) {
        __try {
            unsigned* c = (unsigned*)ctrl[i];
            wsprintfA(b, "  controller %08X: +04=%08X +08=%08X +0C=%08X +10=%08X +14=%08X +18=%08X",
                      ctrl[i], c[1], c[2], c[3], c[4], c[5], c[6]);
            LogS(b);
            wsprintfA(b, "                  +1C=%08X +20=%08X +24=%08X +28=%08X +2C=%08X +30=%08X",
                      c[7], c[8], c[9], c[10], c[11], c[12]);
            LogS(b);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    wsprintfA(tmp, "%d prefs %s, %d live controller(s) dumped",
              n, (forever < 0 && radius < 0) ? "read" : "updated", cn);
    lstrcpynA(msg, tmp, msgLen);
    return touched;
}

// Make NPCs of one type attack NPCs of another, via the game's own AI.
//
// This engine has no NPC-vs-NPC hostility: every character reads affiliation 1,
// and armed outlaws ignore townsfolk entirely. So "outlaws attack chickens"
// cannot be configured -- it has to be commanded. CombatGoto takes an Object,
// and a verb acts on ITS CONTEXT'S actor, so the attacker's own VM instance
// (NPC+0x18) is the context and the victim's handle is the argument.
#define RVA_CombatGoto 0x16C540
#define RVA_GotoRun    0x16BC00
#define NPC_MIND       0xB4      // -> MindBasic, null while the NPC is idle
// The Mind caches its target in four places, all found by scanning an outlaw
// that was chasing the player for the player's own handle and coordinates:
#define MIND_TARGET    0x60      // the target's handle {u16 index, u16 gen}
#define MIND_TARGET2   0x1D0     // mirror
#define MIND_TARGET3   0x340     // mirror
#define MIND_TARGETPOS 0x3E8     // last-known position (3 floats)
// Writing only the handles is why retargeted NPCs hunted blind and gave up with
// "lost him" -- they knew WHO but not WHERE.

// TakeDamage({void}|Object) -- "you were hurt BY this object".
//
// Writing Mind+0x60 changes who an NPC hunts, but it never engages: acquisition
// runs through perception, and a townsfolk is filtered out as not-an-enemy.
// Damage is the AI's own legitimate route to hostility -- shooting an outlaw
// makes him drop everything and come for the shooter. So rather than fight the
// perception filter, tell him the victim hurt him and let his threat logic do
// the targeting properly.
#define RVA_TakeDamage 0x155320

// All defined further down, with the Object-argument machinery.
static bool     HandleForObject(unsigned obj, unsigned short* idxOut, unsigned short* genOut);
static bool     SetArgObject(int i, unsigned obj);
static void     SetArgId(int i, unsigned id);
static unsigned VmInstanceOf(unsigned npc);

int SWSE_MakeAttack(unsigned attackerHash, unsigned victimHash, int count,
                    int useRun, char* msg, int msgLen) {
    char tmp[220], b[190];
    int n = SWSE_FindNpcs(g_snScan, 1024);
    if (n <= 0) { lstrcpynA(msg, "no live NPCs", msgLen); return 0; }

    // Collect the victims up front. Each attacker gets its NEAREST one:
    // pointing four outlaws at a single Clakkerz somewhere across town left
    // them turning in circles hunting a target they could not see. A victim
    // they can actually reach is the difference between "has a grudge" and
    // "opens fire".
    static unsigned victims[256];
    int vn = 0;
    for (int i = 0; i < n && vn < 256; i++) {
        __try {
            unsigned pf = PrefsOfNpc(g_snScan[i]);
            if (pf && *(unsigned*)(pf + 0x0C) == victimHash) victims[vn++] = g_snScan[i];
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (!vn) {
        wsprintfA(tmp, "no live NPC of victim type %08X here", victimHash);
        lstrcpynA(msg, tmp, msgLen);
        return 0;
    }

    void* savedCtx = g_ctx;
    int sent = 0, noInst = 0, faulted = 0;
    if (count < 1) count = 1;
    for (int i = 0; i < n && sent < count; i++) {
        unsigned a = g_snScan[i];
        __try {
            unsigned pf = PrefsOfNpc(a);
            if (!pf || *(unsigned*)(pf + 0x0C) != attackerHash) continue;
        } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        // Nearest victim to THIS attacker.
        unsigned victim = 0;
        float best = 1e30f;
        __try {
            float* ap = (float*)(a + NPC_POS);
            for (int k = 0; k < vn; k++) {
                if (victims[k] == a) continue;
                float* vp = (float*)(victims[k] + NPC_POS);
                float dx = vp[0] - ap[0], dy = vp[1] - ap[1], dz = vp[2] - ap[2];
                float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 < best) { best = d2; victim = victims[k]; }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (!victim) continue;

        unsigned short vidx = 0, vgen = 0;
        if (!HandleForObject(victim, &vidx, &vgen)) continue;

        // The target is a HANDLE in the AI's Mind: +0x60, mirrored at +0x1D0.
        // Found by taking the player's own handle and scanning an NPC that was
        // shooting at him -- an outlaw hunting you is simply an outlaw with
        // your handle in that field. CombatGoto alone did not redirect them,
        // because it sets a goal rather than the hostility target.
        unsigned mind = 0;
        __try { mind = *(unsigned*)(a + NPC_MIND); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (mind < HEAP_LO || mind > HEAP_HI) { noInst++; continue; }

        unsigned packed = ((unsigned)vgen << 16) | vidx;
        int wrote = 0;
        __try {
            *(unsigned*)(mind + MIND_TARGET)  = packed;
            *(unsigned*)(mind + MIND_TARGET2) = packed;
            *(unsigned*)(mind + MIND_TARGET3) = packed;
            float* vp = (float*)(victim + NPC_POS);
            float* tp = (float*)(mind + MIND_TARGETPOS);
            tp[0] = vp[0]; tp[1] = vp[1]; tp[2] = vp[2];   // where to go look
            wrote = 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (!wrote) { faulted++; continue; }

        // Then tell the attacker the victim hurt it, which is how the AI
        // legitimately becomes hostile -- and follow with a move order.
        unsigned inst = VmInstanceOf(a);
        if (inst) {
            g_ctx = (void*)inst;
            if (InstallArgHook()) {
                __try {
                    SetArgObject(0, victim);
                    char ret[64] = { 0 };
                    CallWithArgs(RVA_TakeDamage, ret);      // "he hit you"
                    SetArgObject(0, victim);
                    CallWithArgs(useRun ? RVA_GotoRun : RVA_CombatGoto, ret);
                } __except (GrantFilter(GetExceptionInformation(), "makeattack")) { faulted++; }
                RemoveArgHook();
            }
        }
        sent++;
        if (sent <= 4) {
            wsprintfA(b, "  %08X mind %08X -> target %08X (handle %u:%u)%s",
                      a, mind, victim, vidx, vgen, inst ? " + combatgoto" : "");
            LogS(b);
        }
    }
    g_ctx = savedCtx;
    wsprintfA(tmp, "%d retargeted onto their nearest of %d victim(s); %d idle, %d faulted",
              sent, vn, noInst, faulted);
    lstrcpynA(msg, tmp, msgLen);
    return sent;
}

// Who is fighting whom, right now.
//
// Every active NPC's Mind holds its target as a handle at +0x60. Resolving
// those gives a live picture of the level's hostilities -- and specifically
// finds any NPC that is legitimately hostile to a townsfolk, which is the
// thing we need to study: our forced retargets never engage, so a scripted
// attacker that DOES is the counter-example that shows what we are missing.
// Doing it by scan means not having to walk 700 units to watch it happen.
// Start a feud: repeatedly tell NPCs that another NPC hurt them.
//
// A single accidental hit from one outlaw onto a chicken produced no
// retaliation. But chickens are passive -- they flee rather than fight -- so
// the honest test is two combat-capable NPCs. TakeDamage takes the source as an
// Object, so each can be told the other hurt it, repeatedly, which is the one
// route to hostility the AI accepts from the player ("he retargeted me after I
// attacked him").
int SWSE_Feud(unsigned typeA, unsigned typeB, int rounds, char* msg, int msgLen) {
    char tmp[220], b[190];
    int n = SWSE_FindNpcs(g_snScan, 1024);
    if (n <= 0) { lstrcpynA(msg, "no live NPCs", msgLen); return 0; }

    // Two active NPCs, each with a Mind, of the requested types.
    unsigned A = 0, B = 0;
    for (int i = 0; i < n && (!A || !B); i++) {
        unsigned a = g_snScan[i];
        __try {
            unsigned mind = *(unsigned*)(a + NPC_MIND);
            if (mind < HEAP_LO || mind > HEAP_HI) continue;
            unsigned pf = PrefsOfNpc(a);
            if (!pf) continue;
            unsigned h = *(unsigned*)(pf + 0x0C);
            if (!A && h == typeA) { A = a; continue; }
            if (!B && h == typeB && a != A) B = a;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (!A || !B) {
        wsprintfA(tmp, "need one ACTIVE NPC of each type (found A=%08X B=%08X)", A, B);
        lstrcpynA(msg, tmp, msgLen);
        return 0;
    }

    void* saved = g_ctx;
    int hits = 0, faulted = 0;
    if (rounds < 1) rounds = 8;
    for (int r = 0; r < rounds; r++) {
        // Tell A that B hurt it, then B that A hurt it.
        for (int side = 0; side < 2; side++) {
            unsigned me  = side ? B : A;
            unsigned him = side ? A : B;
            unsigned inst = VmInstanceOf(me);
            if (!inst) continue;
            g_ctx = (void*)inst;
            if (!InstallArgHook()) continue;
            __try {
                SetArgObject(0, him);
                char ret[64] = { 0 };
                CallWithArgs(RVA_TakeDamage, ret);
                hits++;
            } __except (GrantFilter(GetExceptionInformation(), "feud")) { faulted++; }
            RemoveArgHook();
        }
    }
    g_ctx = saved;
    wsprintfA(b, "feud: A=%08X B=%08X", A, B);
    LogS(b);
    wsprintfA(tmp, "%08X vs %08X: %d damage events injected, %d faulted",
              A, B, hits, faulted);
    lstrcpynA(msg, tmp, msgLen);
    return hits;
}

int SWSE_ScanTargets(char* msg, int msgLen) {
    char tmp[220], b[190];
    int n = SWSE_FindNpcs(g_snScan, 1024);
    if (n <= 0) { lstrcpynA(msg, "no live NPCs", msgLen); return 0; }
    unsigned player = PlayerObj();
    unsigned short pidx = 0, pgen = 0;
    HandleForObject(player, &pidx, &pgen);
    unsigned pPacked = ((unsigned)pgen << 16) | pidx;

    int active = 0, atPlayer = 0, atNpc = 0, shown = 0;
    for (int i = 0; i < n; i++) {
        unsigned a = g_snScan[i];
        __try {
            unsigned mind = *(unsigned*)(a + NPC_MIND);
            if (mind < HEAP_LO || mind > HEAP_HI) continue;
            active++;
            unsigned t = *(unsigned*)(mind + MIND_TARGET);
            if (!t || t == 0xFFFFFFFF) continue;
            if (t == pPacked) { atPlayer++; continue; }

            // Resolve the handle back to an object and name both sides.
            unsigned char* tbl = (unsigned char*)RvaPtr(0x5D55F0);
            unsigned idx = t & 0xFFFF;
            unsigned tgt = *(unsigned*)(tbl + idx * 6);
            atNpc++;
            if (shown < 12) {
                unsigned apf = PrefsOfNpc(a), tpf = PrefsOfNpc(tgt);
                wsprintfA(b, "  %08X (type %08X) -> %08X (type %08X)", a,
                          apf ? *(unsigned*)(apf + 0x0C) : 0, tgt,
                          tpf ? *(unsigned*)(tpf + 0x0C) : 0);
                LogS(b);
                shown++;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    wsprintfA(tmp, "%d active of %d NPCs: %d target the player, %d target other NPCs",
              active, n, atPlayer, atNpc);
    lstrcpynA(msg, tmp, msgLen);
    return atNpc;
}

// ---- raid mode: keep raiders hostile, re-picking targets as they move ------
//
// A single write makes a raider tunnel-vision on one victim and give up when it
// loses them ("lost him"). A raid needs standing hostility: every so often,
// point each raider at the nearest thing it should hate -- and the player
// counts, so a raider that cannot reach a townsfolk will come for you instead
// of wandering off.
static bool     g_raidOn       = false;
static unsigned g_raidAttacker = 0;
static unsigned g_raidVictim   = 0;
static int      g_raidTick     = 0;
static int      g_raidEvery    = 60;      // frames between re-targets
static int      g_raidLast     = 0;

int SWSE_RaidMode(unsigned attacker, unsigned victim, int on, int everyFrames) {
    g_raidAttacker = attacker;
    g_raidVictim   = victim;
    g_raidOn       = (on != 0);
    if (everyFrames > 0) g_raidEvery = everyFrames;
    g_raidTick = 0;
    return 1;
}
int SWSE_RaidCount() { return g_raidLast; }
bool SWSE_RaidOn()   { return g_raidOn; }

// The NPC list, cached. Rescanning every tick meant a full heap walk several
// times a second, which produced visible frame spikes -- that scan is cheap
// enough on demand and far too expensive per frame. NPCs do not appear or
// vanish quickly, so refreshing every few seconds is plenty; the per-tick work
// is then just reading positions off a known list.
static unsigned g_tickNpcs[1024];
static int      g_tickCount = 0;
static int      g_tickAge   = 0;
#define TICK_RESCAN_FRAMES 600      // ~10s at 60fps

static int TickNpcList() {
    if (g_tickCount == 0 || ++g_tickAge >= TICK_RESCAN_FRAMES) {
        g_tickAge = 0;
        g_tickCount = SWSE_FindNpcs(g_tickNpcs, 1024);
    }
    return g_tickCount;
}

// ---- decoy mode: shoot the victim without targeting it --------------------
//
// The AI refuses to ENGAGE another NPC -- blanket, tested outlaw->townsfolk and
// slog->townsfolk. But two things it will do: fire at a POSITION, and damage
// whatever its projectiles actually hit (NPCs kill each other with stray
// explosives all the time). So do not fight the veto: leave the target handle
// pointing at the player, so the AI stays engaged and willing to shoot, and
// write the VICTIM's coordinates into the last-known-position cache. The NPC
// believes the player is over there and opens fire on that spot -- and the
// thing standing in it is a chicken.
static bool     g_decoyOn     = false;
static unsigned g_decoyShooter = 0;
static unsigned g_decoyVictim  = 0;
static int      g_decoyTick   = 0;
static int      g_decoyEvery  = 10;
static int      g_decoyLast   = 0;

int SWSE_DecoyMode(unsigned shooter, unsigned victim, int on, int everyFrames) {
    g_decoyShooter = shooter; g_decoyVictim = victim;
    g_decoyOn = (on != 0);
    if (everyFrames > 0) g_decoyEvery = everyFrames;
    g_decoyTick = 0;
    return 1;
}
int SWSE_DecoyCount() { return g_decoyLast; }

void SWSE_DecoyTick() {
    if (!g_decoyOn || !g_decoyShooter) return;
    if (++g_decoyTick < g_decoyEvery) return;
    g_decoyTick = 0;

    int n = TickNpcList();          // cached: a per-frame heap scan caused lag spikes
    if (n <= 0) return;
    memcpy(g_snScan, g_tickNpcs, sizeof(unsigned) * (n < 1024 ? n : 1024));
    static unsigned vic[256];
    int vn = 0;
    for (int i = 0; i < n && vn < 256; i++) {
        __try {
            unsigned pf = PrefsOfNpc(g_snScan[i]);
            if (pf && *(unsigned*)(pf + 0x0C) == g_decoyVictim) vic[vn++] = g_snScan[i];
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (!vn) return;

    int done = 0;
    for (int i = 0; i < n; i++) {
        unsigned a = g_snScan[i];
        __try {
            unsigned pf = PrefsOfNpc(a);
            if (!pf || *(unsigned*)(pf + 0x0C) != g_decoyShooter) continue;
            unsigned mind = *(unsigned*)(a + NPC_MIND);
            if (mind < HEAP_LO || mind > HEAP_HI) continue;

            // Nearest victim to this shooter.
            float* ap = (float*)(a + NPC_POS);
            unsigned best = 0; float bestD = 1e30f;
            for (int k = 0; k < vn; k++) {
                if (vic[k] == a) continue;
                float* vp = (float*)(vic[k] + NPC_POS);
                float dx = vp[0]-ap[0], dy = vp[1]-ap[1], dz = vp[2]-ap[2];
                float d2 = dx*dx + dy*dy + dz*dz;
                if (d2 < bestD) { bestD = d2; best = vic[k]; }
            }
            if (!best) continue;

            // Handle untouched -- only the believed position is a lie.
            float* bp = (float*)(best + NPC_POS);
            float* tp = (float*)(mind + MIND_TARGETPOS);
            tp[0] = bp[0]; tp[1] = bp[1]; tp[2] = bp[2];
            done++;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    g_decoyLast = done;
}

void SWSE_RaidTick() {
    if (!g_raidOn || !g_raidAttacker) return;
    if (++g_raidTick < g_raidEvery) return;
    g_raidTick = 0;

    int n = TickNpcList();
    if (n <= 0) return;
    memcpy(g_snScan, g_tickNpcs, sizeof(unsigned) * (n < 1024 ? n : 1024));

    // Candidate victims: the chosen type, plus the player.
    static unsigned vic[256];
    int vn = 0;
    for (int i = 0; i < n && vn < 255; i++) {
        __try {
            unsigned pf = PrefsOfNpc(g_snScan[i]);
            if (pf && *(unsigned*)(pf + 0x0C) == g_raidVictim) vic[vn++] = g_snScan[i];
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    unsigned player = PlayerObj();
    if (player) vic[vn++] = player;
    if (!vn) return;

    int done = 0;
    for (int i = 0; i < n; i++) {
        unsigned a = g_snScan[i];
        __try {
            unsigned pf = PrefsOfNpc(a);
            if (!pf || *(unsigned*)(pf + 0x0C) != g_raidAttacker) continue;
            unsigned mind = *(unsigned*)(a + NPC_MIND);
            if (mind < HEAP_LO || mind > HEAP_HI) continue;   // idle: no Mind

            float* ap = (float*)(a + NPC_POS);
            unsigned best = 0; float bestD = 1e30f;
            for (int k = 0; k < vn; k++) {
                if (vic[k] == a) continue;
                float* vp = (float*)(vic[k] + NPC_POS);
                float dx = vp[0] - ap[0], dy = vp[1] - ap[1], dz = vp[2] - ap[2];
                float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 < bestD) { bestD = d2; best = vic[k]; }
            }
            if (!best) continue;

            unsigned short bi = 0, bg = 0;
            if (!HandleForObject(best, &bi, &bg)) continue;
            unsigned packed = ((unsigned)bg << 16) | bi;
            *(unsigned*)(mind + MIND_TARGET)  = packed;
            *(unsigned*)(mind + MIND_TARGET2) = packed;
            *(unsigned*)(mind + MIND_TARGET3) = packed;
            float* bp = (float*)(best + NPC_POS);
            float* tp = (float*)(mind + MIND_TARGETPOS);
            tp[0] = bp[0]; tp[1] = bp[1]; tp[2] = bp[2];   // refresh where it is
            done++;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    g_raidLast = done;
}

// Find where an NPC stores "who I am fighting".
//
// CombatGoto executes cleanly but the outlaws keep shooting the player, so the
// hostility target is not the goal we set -- it is a field we have not found.
// Rather than guess offsets: take the PLAYER's handle and pointer, then scan an
// aggro'd NPC (and its Mind) for those exact values. Whatever holds them is the
// target, and that is the field a raid needs to overwrite.
static unsigned g_lastNearNpc = 0;
unsigned SWSE_LastNearNpc() { return g_lastNearNpc; }

int SWSE_FindTarget(unsigned npc, char* msg, int msgLen) {
    char tmp[220], b[190];
    unsigned player = PlayerObj();
    if (!player) { lstrcpynA(msg, "no player object", msgLen); return 0; }
    unsigned short pidx = 0, pgen = 0;
    bool haveH = HandleForObject(player, &pidx, &pgen);
    unsigned packed = ((unsigned)pgen << 16) | pidx;

    wsprintfA(b, "findtarget: player %08X handle %u:%u (packed %08X)",
              player, pidx, pgen, packed);
    LogS(b);

    int hits = 0;
    // The NPC itself.
    __try {
        for (int o = 0; o < 0x400; o += 4) {
            unsigned v = *(unsigned*)(npc + o);
            if (v != player && !(haveH && v == packed)) continue;
            wsprintfA(b, "  NPC+%03X = %08X  (%s)", o, v,
                      (v == player) ? "player POINTER" : "player HANDLE");
            LogS(b);
            hits++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    // And its Mind, which only exists while it is active.
    unsigned mind = 0;
    __try { mind = *(unsigned*)(npc + 0xB4); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (mind >= HEAP_LO && mind <= HEAP_HI) {
        __try {
            for (int o = 0; o < 0x400; o += 4) {
                unsigned v = *(unsigned*)(mind + o);
                if (v != player && !(haveH && v == packed)) continue;
                wsprintfA(b, "  MIND+%03X = %08X  (%s)", o, v,
                          (v == player) ? "player POINTER" : "player HANDLE");
                LogS(b);
                hits++;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        // Also hunt the player's POSITION. A retargeted NPC hunts but barks
        // "lost him" -- it knows WHO but not WHERE, so the Mind must cache a
        // last-known position, and writing that alongside the handle is what
        // would let it actually find a new victim.
        float* pp = PlayerPos();
        if (pp) {
            __try {
                for (int o = 0; o < 0x400 - 8; o += 4) {
                    float* f = (float*)(mind + o);
                    // Written as "must be less than", not "skip if greater":
                    // NaN fails every comparison, so `> 4.0f` let uninitialised
                    // garbage through and reported six bogus position hits.
                    float dx = f[0] - pp[0], dy = f[1] - pp[1], dz = f[2] - pp[2];
                    float d2 = dx * dx + dy * dy + dz * dz;
                    if (!(d2 < 4.0f)) continue;                        // within 2 units
                    if (!(f[0] > -1e6f && f[0] < 1e6f)) continue;      // and finite
                    wsprintfA(b, "  MIND+%03X = %d %d %d  <-- PLAYER POSITION (you: %d %d %d)",
                              o, (int)f[0], (int)f[1], (int)f[2],
                              (int)pp[0], (int)pp[1], (int)pp[2]);
                    LogS(b);
                    hits++;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }
    wsprintfA(tmp, "%d field(s) reference the player (npc %08X, mind %08X) - see log",
              hits, npc, mind);
    lstrcpynA(msg, tmp, msgLen);
    return hits;
}

int SWSE_NpcNear(char* msg, int msgLen) {
    char tmp[220], b[190];
    float* pp = PlayerPos();
    if (!pp) { lstrcpynA(msg, "no player position", msgLen); return 0; }
    int n = SWSE_FindNpcs(g_snScan, 1024);
    if (n <= 0) { lstrcpynA(msg, "no live NPCs", msgLen); return 0; }

    unsigned best = 0;
    float bestD = 1e30f;
    for (int i = 0; i < n; i++) {
        __try {
            float* q = (float*)(g_snScan[i] + NPC_POS);   // NPC position
            float dx = q[0] - pp[0], dy = q[1] - pp[1], dz = q[2] - pp[2];
            float d = dx * dx + dy * dy + dz * dz;
            if (d > 0.01f && d < bestD) { bestD = d; best = g_snScan[i]; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (!best) { lstrcpynA(msg, "no NPC with a readable position", msgLen); return 0; }

    unsigned prefs = PrefsOfNpc(best);
    if (!prefs) {
        wsprintfA(tmp, "nearest NPC %08X - could not find its prefs pointer", best);
        lstrcpynA(msg, tmp, msgLen);
        return 0;
    }
    // NPCPrefs stores its own path hash at +0x0C. Dumping a Clakkerz's prefs
    // showed 2F1D4FF5 there, which is exactly one of the hashes harvested from
    // that level's load -- so the type can be read straight off any NPC, with
    // no harvested list, no offline hashing, and no name table.
    unsigned hash = 0;
    __try { hash = *(unsigned*)(prefs + 0x0C); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    char nm[64];
    if (!RttiName(best, nm, sizeof(nm))) lstrcpynA(nm, "?", 2);
    g_lastNearNpc = best;
    wsprintfA(b, "npcnear: %08X (%s) prefs=%08X hash=%08X", best, nm, prefs, hash);
    LogS(b);
    if (!hash || hash == 0x2DFD1072) {
        wsprintfA(tmp, "nearest NPC %08X prefs %08X - no usable type hash", best, prefs);
        lstrcpynA(msg, tmp, msgLen);
        return 0;
    }
    // Distance matters: "nearest" says nothing about whether it is actually
    // close, and spawns were being reported as adjacent while nothing was
    // visible on screen.
    int dist = 0;
    __try {
        float d = bestD, g = 1.0f;
        for (int k = 0; k < 24 && g * g < d; k++) g += 1.0f;   // integer-ish sqrt
        dist = (int)g;
        float* q = (float*)(best + NPC_POS);
        wsprintfA(b, "npcnear: at %d %d %d, player %d %d %d",
                  (int)q[0], (int)q[1], (int)q[2], (int)pp[0], (int)pp[1], (int)pp[2]);
        LogS(b);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    wsprintfA(tmp, "nearest is type %08X, ~%d units away - more with: npcnow 3 %08X",
              hash, dist, hash);
    lstrcpynA(msg, tmp, msgLen);
    return 1;
}

int SWSE_SpawnNow(int count, int geomIndex, char* msg, int msgLen) {
    char tmp[220], b[190];
    if (!g_nprHave) {
        lstrcpynA(msg, "no capture yet - run npcspy, warp once, then npcnow", msgLen);
        return 0;
    }
    if (!g_nprTramp) {
        lstrcpynA(msg, "spawn routine not hooked - run npcspy once", msgLen);
        return 0;
    }
    unsigned gate = SpawnGateObj();
    if (!gate) { lstrcpynA(msg, "gate object unavailable", msgLen); return 0; }

    // Refuse to spawn at the origin. Right after a load the player has not been
    // placed yet and PlayerPos reads (0,0,0); spawning then puts everything at
    // world origin and still reports success, which is indistinguishable from
    // "the spawn did not work" and wasted a lot of time looking like exactly
    // that.
    {
        float* chk = PlayerPos();
        if (!chk) { lstrcpynA(msg, "no player position yet - wait for the level", msgLen); return 0; }
        if (chk[0] == 0.0f && chk[1] == 0.0f && chk[2] == 0.0f) {
            lstrcpynA(msg, "player still at origin - level not finished loading", msgLen);
            return 0;
        }
    }

    // A tag built by the game's own constructor is valid by construction --
    // unlike the captured corpse, whose resource references the loader released.
    unsigned ctor = (unsigned)(uintptr_t)RvaPtr(RVA_TAG_CTOR), tag = 0;
    __try {
        __asm {
            call ctor
            mov  tag, eax
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        lstrcpynA(msg, "NPCTag constructor faulted", msgLen);
        return -2;
    }
    if (!tag) { lstrcpynA(msg, "NPCTag constructor returned null", msgLen); return 0; }

    // Only the fields the routine actually reads, taken from a REAL tag the
    // game built. The hash especially: computing it ourselves produced a value
    // the resolver rejected, so copy one known to resolve.
    unsigned* rec = (unsigned*)g_nprRec;
    float* pp = PlayerPos();
    unsigned wantHash = 0;
    __try {
        // Type selector: -1 keeps whatever the capture ended on, a small value
        // indexes the harvested list, and anything larger is a raw hash (as
        // reported by npcnear straight off a live NPC's prefs).
        unsigned useHash = rec[2];
        if (geomIndex > 255)
            useHash = (unsigned)geomIndex;
        else if (geomIndex >= 0 && geomIndex < g_npcTypeCount)
            useHash = g_npcTypeHash[geomIndex];
        *(unsigned*)(tag + 0x08)      = useHash;  // type hash
        wantHash = useHash;   // the outer object gets it too, once it exists
        *(unsigned*)(tag + 0x0C)      = 1;        // count: routine requires exactly 1
        *(unsigned char*)(tag + 0x22) = 1;        // the flag it checks next
        if (pp) {
            float* t = (float*)(tag + 0x30);
            t[0] = pp[0]; t[1] = pp[1]; t[2] = pp[2];
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        lstrcpynA(msg, "could not fill the new tag", msgLen);
        return -2;
    }

    // Build the outer half too. Nothing from the load survives it -- not the
    // tag, not the anchor -- so scavenging was never going to work; construct
    // both with the game's own constructors and link them the way it does.
    unsigned ictor = (unsigned)(uintptr_t)RvaPtr(RVA_IOT_CTOR), geom = 0;
    __try {
        __asm {
            call ictor
            mov  geom, eax
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        lstrcpynA(msg, "InstancedObjectTag constructor faulted", msgLen);
        return -2;
    }
    if (!geom) {
        lstrcpynA(msg, "InstancedObjectTag constructor returned null", msgLen);
        return 0;
    }

    // Only the fields SpawnNPCFromTag actually reads off arg0 (+0x08, +0x14,
    // +0x1C, +0x28, +0x3C per the disassembly), copied from a genuine capture.
    // Copying all 0x7C bytes would drag dangling pointers in with them.
    unsigned* a0 = (unsigned*)g_nprA0Rec;
    __try {
        // The captured value, NOT the type hash. Writing a hash here faulted
        // the routine outright, so despite `mov ecx,[ebx+8]` feeding the
        // factory, this field is structural rather than a character selector.
        // EXACTLY the fields the one build that produced visible NPCs used.
        // Copying the whole object instead was a guess; this configuration is
        // the only one with evidence behind it, so changes get re-applied on
        // top of it one at a time rather than piled on.
        *(unsigned*)(geom + 0x08) = a0[0x08 / 4];
        *(unsigned*)(geom + 0x14) = a0[0x14 / 4];
        *(unsigned*)(geom + 0x1C) = a0[0x1C / 4];
        *(unsigned*)(geom + 0x28) = a0[0x28 / 4];
        memcpy((void*)(geom + 0x3C), g_nprA0Rec + 0x3C, 0x24);
        *(unsigned*)(geom + 0x78) = tag;      // the link the thunk follows
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        lstrcpynA(msg, "could not fill the InstancedObjectTag", msgLen);
        return -2;
    }

    // Log what the routine is about to see. The anchor came from a load-time
    // capture and may since have been freed; 0x388304 is GeometryInst's vtable,
    // so a mismatch says the anchor is the problem rather than the tag.
    {
        char tn[64], an[64];
        if (!RttiName(tag, tn, sizeof(tn)))  lstrcpynA(tn, "?", 2);
        if (!RttiName(geom, an, sizeof(an))) lstrcpynA(an, "?", 2);
        wsprintfA(tmp, "npcnow: tag %08X (%s) hash=%08X cnt=%d f22=%d | outer %08X (%s) +78=%08X",
                  tag, tn, *(unsigned*)(tag + 0x08),
                  *(unsigned*)(tag + 0x0C), *(unsigned char*)(tag + 0x22),
                  geom, an, *(unsigned*)(geom + 0x78));
        LogS(tmp);
        // The transform block as integers, to spot which three are the
        // translation by comparing them against the player's own position.
        float* f = (float*)(g_nprA0Rec + 0x3C);
        wsprintfA(tmp, "  outer transform +3C..: %d %d %d %d %d %d %d %d %d",
                  (int)f[0], (int)f[1], (int)f[2], (int)f[3], (int)f[4],
                  (int)f[5], (int)f[6], (int)f[7], (int)f[8]);
        LogS(tmp);
        if (pp) {
            wsprintfA(tmp, "  player is at: %d %d %d", (int)pp[0], (int)pp[1], (int)pp[2]);
            LogS(tmp);
        }
    }

    if (count < 1) count = 1;
    int before = SWSE_FindNpcs(g_snPrev, 1024);
    unsigned char oldGate = 0;
    int faulted = 0;
    __try {
        oldGate = *(unsigned char*)(gate + 0x30);
        *(unsigned char*)(gate + 0x30) = 1;       // open
        // Through the thunk, so the game does its own vtable dispatch.
        // Translation is outer+0x3C..+0x44: the capture read (14,-53,0) while
        // the player stood at (4,-45,0), so x/y are horizontal and z is up.
        // Place them on a RING around the player, never at the player: a 1.5
        // unit offset starting at zero spawned a Wolvark inside him and pinned
        // him in place. Successive rings step outward.
        static const float kRing[8][2] = {
            { 1.000f,  0.000f}, { 0.707f,  0.707f}, { 0.000f,  1.000f}, {-0.707f,  0.707f},
            {-1.000f,  0.000f}, {-0.707f, -0.707f}, { 0.000f, -1.000f}, { 0.707f, -0.707f},
        };
        // Radius is a parameter, not a constant: spawns were visible with a
        // 1.5-unit scatter and invisible once it became a 4-unit ring, and
        // rebuilding to try each value is far slower than sweeping it live.
        // radius 0 puts them exactly on the player.
        for (int i = 0; i < count; i++) {
            if (pp) {
                // The grid the working build used, not the ring. g_spawnRadius
                // scales it so the shape can still be swept at runtime, but at
                // its default of 1.5 this is byte-for-byte the arrangement that
                // put a Wolvark inside the player.
                float step = g_spawnRadius;
                float* t = (float*)(geom + 0x3C);
                t[0] = pp[0] + (float)(i % 3) * step;
                t[1] = pp[1] + (float)(i / 3) * step;
                t[2] = pp[2];
                float r = step;
                if (i < 4) {
                    wsprintfA(b, "  spawn %d aiming at %d %d %d (r=%d)", i,
                              (int)t[0], (int)t[1], (int)t[2], (int)r);
                    LogS(b);
                }
            }
            unsigned ok = CallSpawnThunk(geom);
            if (i < 4) {
                wsprintfA(b, "  spawn %d returned %d (%s)", i, ok,
                          ok ? "game accepted it" : "GAME REJECTED IT");
                LogS(b);
            }
        }
    } __except (GrantFilter(GetExceptionInformation(), "npcnow")) { faulted = 1; }
    __try { *(unsigned char*)(gate + 0x30) = oldGate; }   // always close again
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    if (faulted) {
        wsprintfA(tmp, "faulted: tag %08X hash %08X anchor %08X (gate was %d)",
                  tag, rec[2], geom, oldGate);
        lstrcpynA(msg, tmp, msgLen);
        return -2;
    }
    int after = SWSE_FindNpcs(g_snScan, 1024);
    // wsprintfA has no %+d -- it printed a literal "+d", so compute the delta.
    int delta = after - before;
    // Report the type of what was ACTUALLY created, read back off a new NPC's
    // prefs, rather than the type that was requested. Asking whether the right
    // character appeared should not require someone to look at the screen.
    unsigned gotHash = 0;
    int newSeen = 0;
    for (int i = 0; i < after; i++) {
        unsigned a = g_snScan[i];
        bool existed = false;
        for (int j = 0; j < before; j++)
            if (g_snPrev[j] == a) { existed = true; break; }
        if (existed) continue;
        newSeen++;
        unsigned pf = PrefsOfNpc(a);
        unsigned h = 0;
        if (pf) __try { h = *(unsigned*)(pf + 0x0C); }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (!gotHash) gotHash = h;
        __try {
            float* q = (float*)(a + NPC_POS);
            wsprintfA(b, "  NEW npc %08X type %08X at %d %d %d", a, h,
                      (int)q[0], (int)q[1], (int)q[2]);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            wsprintfA(b, "  NEW npc %08X type %08X (position unreadable)", a, h);
        }
        LogS(b);
    }
    if (pp) {
        wsprintfA(b, "  player at %d %d %d", (int)pp[0], (int)pp[1], (int)pp[2]);
        LogS(b);
    } else {
        LogS("  player position UNAVAILABLE - spawns kept the captured location");
    }
    wsprintfA(tmp, "spawned %d (%d new): asked %08X, got %08X %s",
              delta, newSeen, wantHash, gotHash,
              (gotHash && gotHash == wantHash) ? "MATCH" : "(mismatch)");
    lstrcpynA(msg, tmp, msgLen);
    return (after > before) ? 1 : 0;
}

// npcdupe proved the routine spawns correctly when called with the game's own
// live tag DURING a load. The open question is lifetime: is that tag still
// valid afterwards? Replay the identical call outside the load and see.
// Checking the vtable first distinguishes "the object died" from "the call
// itself is wrong" -- freed memory gets reused, so a changed vtable means we
// would be driving the routine over a dead object.
#define VT_NPCTAG 0x37FC84            // NPCTag, per RTTI (file VA 0x77FC84)

int SWSE_NpcReplay(char* msg, int msgLen) {
    char tmp[220];
    if (!g_nprHave || !g_nprTramp) {
        lstrcpynA(msg, "nothing captured - run npcdupe/npcspy, then warp", msgLen);
        return 0;
    }
    unsigned want = (unsigned)(uintptr_t)GetModuleHandleA(NULL) + VT_NPCTAG;
    unsigned cur  = 0;
    __try { cur = *(unsigned*)g_nprThis; }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        wsprintfA(tmp, "tag %08X is unreadable - freed", g_nprThis);
        lstrcpynA(msg, tmp, msgLen);
        return 0;
    }
    if (cur != want) {
        wsprintfA(tmp, "tag %08X died: vtable %08X, NPCTag is %08X", g_nprThis, cur, want);
        lstrcpynA(msg, tmp, msgLen);
        return 0;
    }
    __try {
        CallSpawnRoutine(g_nprThis, g_nprArg0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        wsprintfA(tmp, "tag %08X still live but the call faulted", g_nprThis);
        lstrcpynA(msg, tmp, msgLen);
        return -2;
    }
    wsprintfA(tmp, "replayed tag %08X arg0 %08X - count NPCs to confirm",
              g_nprThis, g_nprArg0);
    lstrcpynA(msg, tmp, msgLen);
    return 1;
}

__declspec(naked) static void HookNpcRoutine() {
    __asm {
        pushad
        pushfd
        push esp
        call NpcRoutineLog
        add  esp, 4
        popfd
        popad
        jmp  dword ptr [g_nprTramp]
    }
}

// Total hook hits, reported on demand. Counting log LINES instead was wrong:
// only the first three hits per DLL load ever print, so the log saturated on
// the session's first level load and every later spawn -- including any the
// engine does mid-game -- left no trace at all.
int SWSE_NpcHits() { return g_nprHits; }

int SWSE_NpcRoutineSpy(int on) {
    g_nprSpy = (on != 0);
    // Fresh window each arming: hit count AND the harvested type list, so both
    // describe the next level rather than the whole session.
    if (on) { g_nprHits = 0; g_npcTypeCount = 0; }
    if (g_nprFn) return 1;
    if (!on) return 1;
    g_nprFn = (BYTE*)RvaPtr(RVA_NPC_SPAWNROUTINE);
    g_nprTramp = (BYTE*)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE,
                                     PAGE_EXECUTE_READWRITE);
    if (!g_nprTramp) { g_nprFn = nullptr; return -1; }
    memcpy(g_nprTramp, g_nprFn, NPCR_PLEN);
    g_nprTramp[NPCR_PLEN] = 0xE9;
    *(DWORD*)(g_nprTramp + NPCR_PLEN + 1) =
        (DWORD)((g_nprFn + NPCR_PLEN) - (g_nprTramp + NPCR_PLEN + 5));
    DWORD old;
    VirtualProtect(g_nprFn, NPCR_PLEN, PAGE_EXECUTE_READWRITE, &old);
    g_nprFn[0] = 0xE9;
    *(DWORD*)(g_nprFn + 1) = (DWORD)((BYTE*)&HookNpcRoutine - (g_nprFn + 5));
    for (int i = 5; i < NPCR_PLEN; i++) g_nprFn[i] = 0x90;
    VirtualProtect(g_nprFn, NPCR_PLEN, old, &old);
    LogS("npcspy: hook installed on the whole spawn routine (0x184D90)");
    return 1;
}

// GeometryInst is arg0 of SpawnNPCFromTag: it supplies WHERE, via a transform
// with the position at +0x20. Unlike the tag (load-time scratch that is freed),
// these are world geometry and persist -- so one can be found live and its
// position patched to put a spawn wherever we want.
#define VT_GEOMETRYINST 0x388304
#define GI_POS          0x20

int SWSE_FindGeomInst(unsigned* out, int maxOut) {
    unsigned vt = (unsigned)(uintptr_t)GetModuleHandleA(NULL) + VT_GEOMETRYINST;
    int n = 0;
    SYSTEM_INFO si; GetSystemInfo(&si);
    BYTE* p  = (BYTE*)si.lpMinimumApplicationAddress;
    BYTE* hi = (BYTE*)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi;
    char b[170];
    while (p < hi && n < maxOut) {
        if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
        bool ok = mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE
               && HeapRegion(mbi)
               && (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))
               && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS));
        if (ok) {
            __try {
                unsigned* q = (unsigned*)mbi.BaseAddress;
                size_t cnt = mbi.RegionSize / 4;
                for (size_t k = 0; k < cnt && n < maxOut; k++) {
                    if (q[k] != vt) continue;
                    unsigned a = (unsigned)(uintptr_t)(q + k);
                    if (a < HEAP_LO || a > HEAP_HI) continue;   // heap only
                    float* t = (float*)(a + GI_POS);
                    // A real instance sits at plausible world coordinates.
                    if (!(t[0] > -100000.0f && t[0] < 100000.0f)) continue;
                    if (!(t[1] > -100000.0f && t[1] < 100000.0f)) continue;
                    out[n++] = a;
                    if (n <= 8) {
                        wsprintfA(b, "geominst %08X: pos %d %d %d",
                                  a, (int)t[0], (int)t[1], (int)t[2]);
                        LogS(b);
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        p = (BYTE*)mbi.BaseAddress + mbi.RegionSize;
    }
    wsprintfA(b, "geominst: %d found", n);
    LogS(b);
    return n;
}

// Find live NPCPrefs -- the NPC TYPE definitions (health, geometry, motion, AI).
// Unlike tags, these are loaded with the level and persist, so each one is a
// spawnable type: pointing an NPCTag's m_npcPref at one selects what spawns.
#define VT_NPCPREFS      0x367E1C
#define NPCP_HEALTH      0x448     // m_health
#define NPCP_STAMINA     0x44C     // m_stamina
#define NPCP_GEOMETRY    0x438     // m_geometry
#define NPCP_SCALEMIN    0x440     // m_geoScaleMin

int SWSE_FindNpcPrefs(unsigned* out, int maxOut) {
    unsigned vt = (unsigned)(uintptr_t)GetModuleHandleA(NULL) + VT_NPCPREFS;
    int n = 0;
    SYSTEM_INFO si; GetSystemInfo(&si);
    BYTE* p  = (BYTE*)si.lpMinimumApplicationAddress;
    BYTE* hi = (BYTE*)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi;
    char b[190];
    while (p < hi && n < maxOut) {
        if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
        bool ok = mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE
               && HeapRegion(mbi)
               && (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))
               && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS));
        if (ok) {
            __try {
                unsigned* q = (unsigned*)mbi.BaseAddress;
                size_t cnt = mbi.RegionSize / 4;
                for (size_t k = 0; k < cnt && n < maxOut; k++) {
                    if (q[k] != vt) continue;
                    unsigned a = (unsigned)(uintptr_t)(q + k);
                    if (a < HEAP_LO || a > HEAP_HI) continue;  // heap only
                    float hp = *(float*)(a + NPCP_HEALTH);
                    // A real type definition has plausible health; stack noise
                    // and unrelated matches do not.
                    if (!(hp > 0.0f && hp < 100000.0f)) continue;
                    out[n++] = a;
                    if (n <= 12) {
                        wsprintfA(b, "npcprefs %08X: health=%d stamina=%d geo=%08X scale=%d",
                                  a, (int)hp, (int)*(float*)(a + NPCP_STAMINA),
                                  *(unsigned*)(a + NPCP_GEOMETRY),
                                  (int)(*(float*)(a + NPCP_SCALEMIN) * 100));
                        LogS(b);
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        p = (BYTE*)mbi.BaseAddress + mbi.RegionSize;
    }
    wsprintfA(b, "npcprefs: %d type definition(s) found", n);
    LogS(b);
    return n;
}

// Find live NPC objects. A living NPC already carries everything the load-time
// tag would have supplied, so it is both a reference for what a correct NPC
// looks like and the basis for cloning one.
#define VT_NPC 0x3671B4

int SWSE_FindNpcs(unsigned* out, int maxOut) {
    unsigned vt = (unsigned)(uintptr_t)GetModuleHandleA(NULL) + VT_NPC;
    int n = 0;
    SYSTEM_INFO si; GetSystemInfo(&si);
    BYTE* p  = (BYTE*)si.lpMinimumApplicationAddress;
    BYTE* hi = (BYTE*)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi;
    char b[160];
    while (p < hi && n < maxOut) {
        if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
        bool ok = mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE
               && HeapRegion(mbi)
               && (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))
               && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS));
        if (ok) {
            __try {
                unsigned* q = (unsigned*)mbi.BaseAddress;
                size_t cnt = mbi.RegionSize / 4;
                for (size_t k = 0; k < cnt && n < maxOut; k++) {
                    if (q[k] != vt) continue;
                    unsigned a = (unsigned)(uintptr_t)(q + k);
                    // Heap only: stack frames transiently hold this value too,
                    // which is exactly how the NPCTag scan fooled us.
                    if (a < HEAP_LO || a > HEAP_HI) continue;
                    // A live NPC points at itself at +0x54 (same as the player).
                    if (*(unsigned*)(a + 0x54) != a) continue;
                    out[n++] = a;
                    if (n <= 8) {
                        wsprintfA(b, "npc %08X: +0x0C=%d +0x44=%d +0x50=%d",
                                  a, *(int*)(a + 0x0C),
                                  (int)*(float*)(a + NPC_POS), (int)*(float*)(a + 0x50));
                        LogS(b);
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        p = (BYTE*)mbi.BaseAddress + mbi.RegionSize;
    }
    wsprintfA(b, "npcs: %d live NPC(s) found", n);
    LogS(b);
    return n;
}

// Incremental form of the scan above.
//
// The full walk costs 150-270 ms in a busy level, and it runs on the RENDER
// thread, so it lands as a visible freeze every refresh - measured as
// "FRAMESTALL: frame 190 ms, SWSE work 156 ms" and reported as little hitches
// during play. The work itself is unavoidable (there is no actor registry to
// ask), but it does not have to happen in one frame.
//
// This keeps the region cursor between calls and spends a bounded slice per
// call, publishing the new list only when a whole pass finishes. The old list
// stays valid meanwhile, so health polling never sees a half-built set. It
// also stays on the render thread, so no locking is needed and nothing else
// about the actor reads changes.
static BYTE*    g_incP = nullptr;
static BYTE*    g_incHi = nullptr;
static unsigned g_incBuf[1024];
static int      g_incN = 0;
static bool     g_incRunning = false;
static size_t   g_incOff = 0;      // word offset within the current region

int SWSE_FindNpcsStep(unsigned* out, int maxOut, double budgetMs, int* complete) {
    if (complete) *complete = 0;
    unsigned vt = (unsigned)(uintptr_t)GetModuleHandleA(NULL) + VT_NPC;
    LARGE_INTEGER f, c0, c1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c0);

    if (!g_incRunning) {
        // Walk only the heap window, not the whole address space.
        //
        // A candidate is rejected anyway unless it lies in [HEAP_LO, HEAP_HI],
        // so scanning outside it can never produce a hit - it is pure cost.
        // That matters now the scan is time-sliced: at 1.5 ms per frame a full
        // address-space walk took so long that no pass ever completed, the
        // actor list stayed empty, and damage detection silently stopped
        // working ("damage watch ON: 0 polls").
        SYSTEM_INFO si; GetSystemInfo(&si);
        BYTE* lo = (BYTE*)(uintptr_t)HEAP_LO;
        BYTE* hi = (BYTE*)(uintptr_t)HEAP_HI;
        if (lo < (BYTE*)si.lpMinimumApplicationAddress) lo = (BYTE*)si.lpMinimumApplicationAddress;
        if (hi > (BYTE*)si.lpMaximumApplicationAddress) hi = (BYTE*)si.lpMaximumApplicationAddress;
        g_incP  = lo;
        g_incHi = hi;
        g_incN  = 0;
        g_incOff = 0;
        g_incRunning = true;
    }

    const int cap = (int)(sizeof(g_incBuf) / sizeof(g_incBuf[0]));
    MEMORY_BASIC_INFORMATION mbi;
    bool yielded = false;
    size_t startOff = g_incOff;      // to report progress per call
    while (g_incP < g_incHi && g_incN < cap) {
        if (!VirtualQuery(g_incP, &mbi, sizeof(mbi))) { g_incP = g_incHi; break; }
        bool ok = mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE
               && HeapRegion(mbi)
               && (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))
               && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS));
        if (ok) {
            // Resume WITHIN the region. Checking the budget only between
            // regions was not enough: one committed heap region can be
            // hundreds of MB, and scanning it to the end took the same
            // 260 ms the whole walk used to, so the freeze survived.
            unsigned* q = (unsigned*)mbi.BaseAddress;
            size_t cnt = mbi.RegionSize / 4;
            size_t k   = g_incOff;
            __try {
                for (; k < cnt && g_incN < cap; k++) {
                    // Time check every 64 KB. 256 KB was too coarse: at the
                    // measured ~85 MB/s that is already ~3 ms, so a slice
                    // always overshot a small budget by a whole chunk.
                    if ((k & 0x3FFF) == 0) {
                        QueryPerformanceCounter(&c1);
                        double el = (double)(c1.QuadPart - c0.QuadPart) * 1000.0
                                  / (double)f.QuadPart;
                        if (el >= budgetMs) break;
                    }
                    if (q[k] != vt) continue;
                    unsigned a = (unsigned)(uintptr_t)(q + k);
                    if (a < HEAP_LO || a > HEAP_HI) continue;
                    if (*(unsigned*)(a + 0x54) != a) continue;
                    g_incBuf[g_incN++] = a;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) { k = cnt; }
            if (k < cnt) { g_incOff = k; yielded = true; break; }  // resume here
        }
        g_incOff = 0;
        g_incP = (BYTE*)mbi.BaseAddress + mbi.RegionSize;

        QueryPerformanceCounter(&c1);
        double ms = (double)(c1.QuadPart - c0.QuadPart) * 1000.0 / (double)f.QuadPart;
        if (ms >= budgetMs) { yielded = true; break; }
    }
    if (yielded) {
        // Is the cursor actually moving? "never completes" could be slow
        // progress or no progress, and those are different bugs.
        static int dbg = 0;
        if ((++dbg % 240) == 0) {
            char b[200];
            wsprintfA(b, "watchscan: at %08X+%08X of %08X, %d found, %d KB this call",
                      (unsigned)(uintptr_t)g_incP, (unsigned)(g_incOff * 4),
                      (unsigned)(uintptr_t)g_incHi, g_incN,
                      (int)((g_incOff - startOff) * 4 / 1024));
            LogS(b);
        }
        return -1;                              // more to do next frame
    }

    int n = (g_incN > maxOut) ? maxOut : g_incN;
    for (int i = 0; i < n; i++) out[i] = g_incBuf[i];
    g_incRunning = false;
    if (complete) *complete = 1;
    return n;                                    // no per-NPC logging: this
}                                                // runs continuously now

// Cheaply re-check an actor list we already have, compacting out anything no
// longer valid. Returns the surviving count, or -1 if the memory could not be
// read at all (caller should rebuild from scratch).
//
// This is what makes refreshes nearly free. Rediscovering the same ~260
// objects means walking 768 MB; re-checking known pointers is ~260 reads.
// Safe because the engine NEVER creates NPCs at runtime - every actor exists
// from level load, and encounters walk pre-placed ones out of hiding (see
// ROADMAP.md) - so within a level the set cannot grow behind our back. A level
// change frees them, the checks below fail, and the caller falls back to a
// full scan.
int SWSE_ValidateNpcs(unsigned* list, int n) {
    unsigned vt = (unsigned)(uintptr_t)GetModuleHandleA(NULL) + VT_NPC;
    int w = 0;
    __try {
        for (int i = 0; i < n; i++) {
            unsigned a = list[i];
            if (a < HEAP_LO || a > HEAP_HI) continue;
            if (*(unsigned*)a != vt) continue;              // still an NPC
            if (*(unsigned*)(a + 0x54) != a) continue;      // still points at itself
            list[w++] = a;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return w;
}

// Find live NPCTag objects in the CURRENT level. Replaying captured pointers
// fails because a tag's spawn data is consumed during level load -- these are
// the ones that actually exist right now.
#define VT_NPCTAG 0x37FC84

int SWSE_NpcTags(unsigned* out, int maxOut) {
    unsigned vt = (unsigned)(uintptr_t)GetModuleHandleA(NULL) + VT_NPCTAG;
    int n = 0;
    SYSTEM_INFO si; GetSystemInfo(&si);
    BYTE* p  = (BYTE*)si.lpMinimumApplicationAddress;
    BYTE* hi = (BYTE*)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi;
    char b[120];
    while (p < hi && n < maxOut) {
        if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
        bool ok = mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE
               && HeapRegion(mbi)
               && (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))
               && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS));
        if (ok) {
            __try {
                unsigned* q = (unsigned*)mbi.BaseAddress;
                size_t cnt = mbi.RegionSize / 4;
                for (size_t k = 0; k < cnt && n < maxOut; k++) {
                    if (q[k] != vt) continue;
                    unsigned a = (unsigned)(uintptr_t)(q + k);
                    // Heap only, and the count field must be sane. Without
                    // this the scan reported a match at 0x3415CE74 -- outside
                    // the range real tags live in -- and calling the spawn
                    // routine on it faulted. A vtable-shaped dword is not an
                    // object; this scan needed the same gate the NPC scan has.
                    if (a < HEAP_LO || a > HEAP_HI) continue;
                    unsigned c = *(unsigned*)(a + 0x0C);
                    if (c > 64) continue;
                    out[n++] = a;
                    if (n <= 8) {
                        wsprintfA(b, "npctag %08X: hash=%08X count=%d flag22=%d",
                                  a, *(unsigned*)(a + 0x08), c,
                                  *(unsigned char*)(a + 0x22));
                        LogS(b);
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        p = (BYTE*)mbi.BaseAddress + mbi.RegionSize;
    }
    wsprintfA(b, "npctags: %d found", n);
    LogS(b);
    return n;
}

// ---- build a real NPCTag instead of reanimating a dead one -----------------
// The captured tag is a corpse: its memory survives the load but the loader
// releases every resource reference, so spawning from it faults on one null
// after another. 0x184C20 allocates 0x5C bytes and runs the real constructor,
// with no arguments, giving a tag whose fields are all valid by construction.
// Then we only have to set what we actually want: type and position.
#define RVA_NPCTAG_NEW  0x184C20
#define TAG_NPCPREF     0x08     // m_npcPref        -> NPCPrefs (the TYPE)
#define TAG_COUNT       0x0C     // m_countToSpawn
#define TAG_MAXALIVE    0x10     // m_maxAliveAtATime
#define TAG_SCATTER     0x1C     // m_spawnScatterRadius
#define TAG_POS         0x30     // transform translation (WHERE)

// m_npcPref is a path HASH, not a pointer: 0x23880 compares it against the
// sentinel 0x2DFD1072 and otherwise resolves it. Writing a raw NPCPrefs
// address there is why every attempt produced a null further down. Use the
// game's own hasher (0x24D920: '/'->'\', tolower, table at 0x7F7478) so the
// value matches exactly what the loader would have stored.
#define RVA_PATH_HASH 0x24D920

static unsigned HashPath(const char* path) {
    unsigned out = 0xFFFFFFFF;
    unsigned fn = (unsigned)(uintptr_t)RvaPtr(RVA_PATH_HASH);
    __try {
        __asm {
            mov eax, path
            lea edi, out
            call fn
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return out;
}

unsigned SWSE_HashPath(const char* path) { return HashPath(path); }

// The characters the game ships. Index selects which one to spawn.
static const char* kNpcPaths[] = {
    "/data/prefs/characters/outlawcutterprefs.txt",
    "/data/prefs/characters/outlawshooterprefs.txt",
    "/data/prefs/characters/outlawboss_elbowsfreelyprefs.txt",
    "/data/prefs/characters/elbowzfreelyprefs.txt",
    "/data/prefs/characters/wolvarkshooterprefs.txt",
    "/data/prefs/characters/wolvarkgrenadierprefs.txt",
    "/data/prefs/characters/wolvarksniperprefs.txt",
    "/data/prefs/characters/slegprefs.txt",
    "/data/prefs/characters/sewerslegprefs.txt",
    "/data/prefs/characters/giantslegprefs.txt",
    "/data/prefs/characters/sloghandlerspawnedprefs.txt",
};
static const int kNpcPathCount = sizeof(kNpcPaths) / sizeof(kNpcPaths[0]);

const char* SWSE_NpcPathName(int i) {
    return (i >= 0 && i < kNpcPathCount) ? kNpcPaths[i] : nullptr;
}
int SWSE_NpcPathCount() { return kNpcPathCount; }

// Clone a captured spawn record and retarget it. The OBSERVED layout of the
// object SpawnNPCFromTag receives (dumped from a real call) is:
//     +0x08 pref hash    +0x14/18/1C position    +0x30 count
//     +0x34 maxAlive     +0x48 self pointer      +0x4C/50/54 position copy
// Note this is NOT the reflected NPCTag layout -- there, +0x30 is position and
// +0x08 is m_npcPref. Using the reflection offsets wrote position over the
// count, which is why every constructed tag failed downstream.
#define REC_HASH     0x08
#define REC_POS      0x14
#define REC_COUNT    0x30
#define REC_MAXALIVE 0x34
#define REC_SELF     0x48
#define REC_POS2     0x4C
#define REC_SIZE     0x200

int SWSE_SpawnCloned(char* msg, int msgLen) {
    char tmp[220];
    if (!g_nprHave) {
        lstrcpynA(msg, "nothing captured - run npcspy, then warp", msgLen);
        return 0;
    }
    float* pp = PlayerPos();
    if (!pp) { lstrcpynA(msg, "no player position", msgLen); return 0; }

    // Use the snapshot taken in the hook, not the live pointer: the original
    // record is recycled within seconds of the load finishing.
    static unsigned char rec[REC_SIZE];
    memcpy(rec, g_nprRec, REC_SIZE);
    unsigned self = (unsigned)(uintptr_t)rec;
    __try {
        float* p1 = (float*)(rec + REC_POS);
        float* p2 = (float*)(rec + REC_POS2);
        p1[0] = pp[0]; p1[1] = pp[1]; p1[2] = pp[2];
        p2[0] = pp[0]; p2[1] = pp[1]; p2[2] = pp[2];
        *(unsigned*)(rec + REC_SELF)  = self;      // self pointer -> our copy
        *(int*)     (rec + REC_COUNT) = 1;
        *(int*)     (rec + REC_MAXALIVE) = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    // Anchor: a live GeometryInst moved to the player.
    unsigned gi[32];
    int ng = SWSE_FindGeomInst(gi, 32);
    unsigned anchor = g_nprArg0;
    for (int i = 0; i < ng; i++) {
        __try {
            float* t = (float*)(gi[i] + GI_POS);
            if (t[0] == 0.0f && t[1] == 0.0f && t[2] == 0.0f) continue;
            t[0] = pp[0]; t[1] = pp[1]; t[2] = pp[2];
            anchor = gi[i]; break;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    wsprintfA(tmp, "spawnclone: record %08X hash %08X anchor %08X",
              self, *(unsigned*)(rec + REC_HASH), anchor);
    LogS(tmp);

    unsigned fn = (unsigned)(uintptr_t)RvaPtr(RVA_NPC_SPAWNROUTINE);
    unsigned ok = 0;
    __try {
        __asm {
            push anchor
            mov  ecx, self
            call fn
            mov  ok, eax
        }
    } __except (GrantFilter(GetExceptionInformation(), "spawnclone")) {
        lstrcpynA(msg, "FAULTED - see log", msgLen);
        return -2;
    }
    wsprintfA(tmp, "spawned a clone of the captured NPC at you -> %08X", ok);
    lstrcpynA(msg, tmp, msgLen);
    return 1;
}

int SWSE_SpawnNpc(int typeIndex, char* msg, int msgLen) {
    char tmp[220];
    if (typeIndex < 0 || typeIndex >= kNpcPathCount) typeIndex = 0;

    unsigned gi[32];
    int ng = SWSE_FindGeomInst(gi, 32);
    if (ng <= 0) { lstrcpynA(msg, "no spawn anchor (GeometryInst) found", msgLen); return 0; }

    float* pp = PlayerPos();
    if (!pp) { lstrcpynA(msg, "no player position", msgLen); return 0; }

    // A fresh, fully-constructed tag.
    unsigned tag = 0;
    unsigned mk  = (unsigned)(uintptr_t)RvaPtr(RVA_NPCTAG_NEW);
    __try {
        __asm { call mk
                mov tag, eax }
    } __except (GrantFilter(GetExceptionInformation(), "npctag-new")) {
        lstrcpynA(msg, "FAULTED creating the NPCTag", msgLen);
        return -2;
    }
    if (!tag) { lstrcpynA(msg, "NPCTag allocation returned null", msgLen); return -2; }

    // Anchor: a live GeometryInst moved to the player. Skip degenerate ones.
    unsigned anchor = 0;
    for (int i = 0; i < ng && !anchor; i++) {
        __try {
            float* t = (float*)(gi[i] + GI_POS);
            if (t[0] == 0.0f && t[1] == 0.0f && t[2] == 0.0f) continue;
            t[0] = pp[0]; t[1] = pp[1]; t[2] = pp[2];
            anchor = gi[i];
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (!anchor) { lstrcpynA(msg, "no usable anchor", msgLen); return 0; }

    unsigned prefHash = HashPath(kNpcPaths[typeIndex]);
    __try {
        *(unsigned*)(tag + TAG_NPCPREF)  = prefHash;   // a HASH, not a pointer
        *(int*)     (tag + TAG_COUNT)    = 1;
        *(int*)     (tag + TAG_MAXALIVE) = 1;
        *(float*)   (tag + TAG_SCATTER)  = 0.0f;
        float* tp = (float*)(tag + TAG_POS);
        tp[0] = pp[0]; tp[1] = pp[1]; tp[2] = pp[2];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        lstrcpynA(msg, "could not populate the tag", msgLen);
        return -2;
    }

    wsprintfA(tmp, "npcspawn: tag %08X hash %08X (%s) anchor %08X",
              tag, prefHash, kNpcPaths[typeIndex], anchor);
    LogS(tmp);

    unsigned fn = (unsigned)(uintptr_t)RvaPtr(RVA_NPC_SPAWNROUTINE);
    unsigned ok = 0;
    __try {
        __asm {
            push anchor
            mov  ecx, tag
            call fn
            mov  ok, eax
        }
    } __except (GrantFilter(GetExceptionInformation(), "npcspawn")) {
        lstrcpynA(msg, "FAULTED in the spawn routine - see log", msgLen);
        return -2;
    }
    wsprintfA(tmp, "spawned %s -> %08X", kNpcPaths[typeIndex], ok);
    lstrcpynA(msg, tmp, msgLen);
    return 1;
}

// Replay the entire routine. tagOverride != 0 uses a patched copy of the tag.
int SWSE_NpcRoutineRun(unsigned tagOverride, char* msg, int msgLen, int typeIndex) {
    if (!g_nprHave) {
        lstrcpynA(msg, "nothing captured - run npcspy, then warp", msgLen);
        return 0;
    }
    unsigned fn   = (unsigned)(uintptr_t)RvaPtr(RVA_NPC_SPAWNROUTINE);
    unsigned self = tagOverride ? tagOverride : g_nprThis;
    unsigned a0   = g_nprArg0;
    unsigned ok   = 0;

    // arg0 is a GeometryInst (the spawn anchor). The captured one belongs to a
    // level-load frame and is stale by now, which is the likely cause of the
    // earlier fault. Use a LIVE one instead and put it at the player, so the
    // NPC appears next to us rather than at some authored anchor.
    // The captured tag's memory survives the load but its smart pointers are
    // released when loading ends -- which is why the spawn faults in
    // SmartPtr::operator= with a NULL source. Repoint m_npcPref (+0x08) at a
    // live NPCPrefs. This is also how the TYPE gets chosen: index into the
    // types npctypes lists (Floyd, Looten Duke, cutter, ...).
    unsigned prefs[32];
    int np = SWSE_FindNpcPrefs(prefs, 32);
    if (np > 0) {
        int pick = (typeIndex >= 0 && typeIndex < np) ? typeIndex : 0;
        __try {
            *(unsigned*)(self + 0x08) = prefs[pick];
            char pb[120];
            wsprintfA(pb, "npcspawn: tag %08X m_npcPref <- %08X (type %d of %d)",
                      self, prefs[pick], pick, np);
            LogS(pb);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    unsigned gi[32];
    int ng = SWSE_FindGeomInst(gi, 32);
    float* pp = PlayerPos();
    for (int i = 0; i < ng; i++) {
        __try {
            float* t = (float*)(gi[i] + GI_POS);
            // Skip degenerate entries: the first anchor found sits at 0,0,0,
            // which looks like a template rather than a placed instance.
            if (t[0] == 0.0f && t[1] == 0.0f && t[2] == 0.0f) continue;
            if (pp) { t[0] = pp[0]; t[1] = pp[1]; t[2] = pp[2]; }
            a0 = gi[i];
            break;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    __try {
        __asm {
            push a0
            mov  ecx, self
            call fn
            mov  ok, eax
        }
    } __except (GrantFilter(GetExceptionInformation(), "npcroutine")) {
        lstrcpynA(msg, "FAULTED running the spawn routine - see log", msgLen);
        return -2;
    }
    char tmp[190];
    wsprintfA(tmp, "spawn ran: tag %08X anchor %08X -> %08X", self, a0, ok);
    lstrcpynA(msg, tmp, msgLen);
    return 1;
}

extern "C" void __cdecl NpcLogRaw(void* saved) {
    if (!g_npcSpy) return;
    __try {
        BYTE* s = (BYTE*)saved;
        // saved -> flags; pushad follows: edi esi ebp esp ebx edx ecx eax
        unsigned edi = *(unsigned*)(s + 4);
        unsigned* stk = (unsigned*)(s + 36);      // [ret, a0, a1, ...]
        g_npcEdi = edi;
        for (int i = 0; i < 10; i++) g_npcArgs[i] = stk[1 + i];
        // a0 and a1 are stack pointers whose frame dies with the caller, so we
        // must also record what they POINT AT. a1 in particular is an input:
        // the factory does `mov ecx,[a1]; cmp ecx,0; je bail`, which is why
        // replaying with a zeroed buffer returned null.
        for (int i = 0; i < 8; i++) {
            g_npcDeref0[i] = 0;
            g_npcDeref1[i] = 0;
        }
        __try {
            unsigned* p0 = (unsigned*)g_npcArgs[0];
            for (int i = 0; i < 8; i++) g_npcDeref0[i] = p0[i];
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        __try {
            unsigned* p1 = (unsigned*)g_npcArgs[1];
            for (int i = 0; i < 8; i++) g_npcDeref1[i] = p1[i];
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        g_npcHave = true;
        if (++g_npcHits <= 3) {
            char b[220];
            wsprintfA(b, "NPC FACTORY #%d: edi=%08X ret=%08X", g_npcHits, edi, stk[0]);
            LogS(b);
            wsprintfA(b, "   a0=%08X a1=%08X a2=%08X a3=%08X a4=%08X",
                      g_npcArgs[0], g_npcArgs[1], g_npcArgs[2],
                      g_npcArgs[3], g_npcArgs[4]);
            LogS(b);
            wsprintfA(b, "   a5=%08X a6=%08X a7=%08X a8=%08X a9=%08X",
                      g_npcArgs[5], g_npcArgs[6], g_npcArgs[7],
                      g_npcArgs[8], g_npcArgs[9]);
            LogS(b);
            wsprintfA(b, "   *a0 = %08X %08X %08X %08X",
                      g_npcDeref0[0], g_npcDeref0[1], g_npcDeref0[2], g_npcDeref0[3]);
            LogS(b);
            wsprintfA(b, "   *a1 = %08X %08X %08X %08X",
                      g_npcDeref1[0], g_npcDeref1[1], g_npcDeref1[2], g_npcDeref1[3]);
            LogS(b);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

__declspec(naked) static void HookNpcFactory() {
    __asm {
        pushad
        pushfd
        push esp
        call NpcLogRaw
        add  esp, 4
        popfd
        popad
        jmp  dword ptr [g_npcTramp]
    }
}

int SWSE_NpcSpy(int on) {
    g_npcSpy = (on != 0);
    if (g_npcFn) return 1;
    if (!on) return 1;
    g_npcFn = (BYTE*)RvaPtr(RVA_NPC_FACTORY);
    g_npcTramp = (BYTE*)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE,
                                     PAGE_EXECUTE_READWRITE);
    if (!g_npcTramp) { g_npcFn = nullptr; return -1; }
    memcpy(g_npcTramp, g_npcFn, NPCF_PLEN);
    g_npcTramp[NPCF_PLEN] = 0xE9;
    *(DWORD*)(g_npcTramp + NPCF_PLEN + 1) =
        (DWORD)((g_npcFn + NPCF_PLEN) - (g_npcTramp + NPCF_PLEN + 5));
    DWORD old;
    VirtualProtect(g_npcFn, NPCF_PLEN, PAGE_EXECUTE_READWRITE, &old);
    g_npcFn[0] = 0xE9;
    *(DWORD*)(g_npcFn + 1) = (DWORD)((BYTE*)&HookNpcFactory - (g_npcFn + 5));
    for (int i = 5; i < NPCF_PLEN; i++) g_npcFn[i] = 0x90;
    VirtualProtect(g_npcFn, NPCF_PLEN, old, &old);
    LogS("npcspy: hook installed on the NPC factory (0x25A50)");
    return 1;
}

// Replay the captured call. The caller cleans 0x28 (10 dwords), so push 10 and
// clean 10 -- a spare argument the callee ignores is harmless, a wrong stack
// adjustment is not.
// atPlayer: override a4 with a pointer to the player's coordinates.
// The [vtable+0x104] call the caller makes afterwards is a FACING vector
// ({-tag+0x1C, -tag+0x28, 0} -- two negated fields and a zero Z), not a
// position, so the spawn location has to arrive through the factory's own
// arguments. a4/a5/a6 point at +0x30/+0x34/+0x3C of a source object; a4 is
// the candidate for the transform. This is a hypothesis test, not a certainty.
int SWSE_NpcLastAt(char* msg, int msgLen, bool atPlayer);

int SWSE_NpcLast(char* msg, int msgLen) { return SWSE_NpcLastAt(msg, msgLen, false); }

int SWSE_NpcLastAt(char* msg, int msgLen, bool atPlayer) {
    if (!g_npcHave) {
        lstrcpynA(msg, "nothing captured - run npcspy, then load/warp a level", msgLen);
        return 0;
    }
    // a0, a1 (and a9, which mirrors a1) were STACK temporaries in the captured
    // call -- 0x3416Fxxx, a frame that is long gone. Replaying those addresses
    // would have the factory write into whatever occupies that stack now.
    // Substitute our own scratch; the heap arguments (a3..a6, the level tag
    // data) are still live and get reused as-is.
    static unsigned outSlot;
    static unsigned char scratch0[128], scratch1[128];
    outSlot = 0;
    memset(scratch0, 0, sizeof(scratch0));
    memset(scratch1, 0, sizeof(scratch1));
    // Restore what those pointers held in the real call, or the factory's
    // `cmp [a1],0` check bails and we get a null result.
    memcpy(scratch0, g_npcDeref0, sizeof(g_npcDeref0));
    memcpy(scratch1, g_npcDeref1, sizeof(g_npcDeref1));

    unsigned local[10];
    for (int i = 0; i < 10; i++) local[i] = g_npcArgs[i];
    local[0] = (unsigned)(uintptr_t)scratch0;
    local[1] = (unsigned)(uintptr_t)scratch1;
    local[9] = (unsigned)(uintptr_t)scratch1;   // mirrored a1 in the capture

    // Copy the source transform block and patch the player's coordinates into
    // it, rather than pointing the factory at raw floats -- a4/a5/a6 are three
    // offsets into ONE structure (+0x30/+0x34/+0x3C), so it has to stay intact.
    static float xform[32];
    if (atPlayer) {
        float* pp = PlayerPos();
        if (!pp) { lstrcpynA(msg, "no player position", msgLen); return 0; }
        __try {
            const float* src = (const float*)(g_npcArgs[4] - 0x30);
            for (int i = 0; i < 32; i++) xform[i] = src[i];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            lstrcpynA(msg, "could not read the source transform", msgLen);
            return -2;
        }
        unsigned base = (unsigned)(uintptr_t)xform;
        xform[0x30 / 4] = pp[0];
        xform[0x34 / 4] = pp[1];
        xform[0x38 / 4] = pp[2];
        local[4] = base + 0x30;
        local[5] = base + 0x34;
        local[6] = base + 0x3C;
    }

    unsigned fn = (unsigned)(uintptr_t)RvaPtr(RVA_NPC_FACTORY);
    unsigned* a = local;
    unsigned  out = (unsigned)(uintptr_t)&outSlot;
    __try {
        __asm {
            mov  esi, a
            push dword ptr [esi+36]
            push dword ptr [esi+32]
            push dword ptr [esi+28]
            push dword ptr [esi+24]
            push dword ptr [esi+20]
            push dword ptr [esi+16]
            push dword ptr [esi+12]
            push dword ptr [esi+8]
            push dword ptr [esi+4]
            push dword ptr [esi]
            mov  edi, out
            call fn
            add  esp, 0x28
        }
    } __except (GrantFilter(GetExceptionInformation(), "npclast")) {
        lstrcpynA(msg, "FAULTED replaying the factory - see log", msgLen);
        return -2;
    }
    char tmp[200];
    if (!outSlot) {
        lstrcpynA(msg, "factory returned null - it bailed, see log", msgLen);
        return 2;
    }

    // Creating the NPC is not enough: the caller then hands it to the world.
    //     0x63010(npc, tag)      ; cdecl, caller cleans 8
    // The tag is recoverable from the capture -- a3 was tag+0x3C.
    unsigned npc = outSlot;
    unsigned tag = g_npcArgs[3] - 0x3C;
    unsigned reg = (unsigned)(uintptr_t)RvaPtr(RVA_NPC_REGISTER);
    __try {
        __asm {
            push tag
            push npc
            call reg
            add  esp, 8
        }
    } __except (GrantFilter(GetExceptionInformation(), "npcregister")) {
        wsprintfA(tmp, "NPC %08X created, but registration FAULTED", npc);
        lstrcpynA(msg, tmp, msgLen);
        return -2;
    }
    wsprintfA(tmp, "NPC %08X created and registered (tag %08X)", npc, tag);
    lstrcpynA(msg, tmp, msgLen);
    return 1;
}

// ---- capture hook on GiveAmmo -------------------------------------------
// GiveAmmo prologue: 51 8b 44 24 0c ...  -> first 5 bytes (push ecx; mov
// eax,[esp+0xc]) form a clean boundary for the jmp patch.
static BYTE*  g_giveAmmo = nullptr;
static BYTE   g_orig[5];
static BYTE*  g_tramp = nullptr;     // [orig 5 bytes][jmp back]

// Captured raw args, for the one-time diagnostic below.
static unsigned g_capA0 = 0, g_capA1 = 0, g_capA2 = 0, g_capA3 = 0, g_capA4 = 0;
static volatile bool g_capLogged = false;

// naked hook: entered via JMP from GiveAmmo's first byte, so the stack is
// exactly as the game left it: [esp]=retaddr, [esp+4]=arg0, [esp+8]=arg1,
// [esp+0xC]=arg2.
//
// IMPORTANT (from disassembling TakeAllArtifacts/GiveArtifact/HasArtifactCount):
// the real convention is  handler(retBuf, ctx, args...)  â€” arg0 is a
// caller-allocated RETURN BUFFER (TakeAllArtifacts writes a vtable into it and
// returns it in eax), and arg1 is the ScriptContext (the one that gets
// vtable-called at +0x74). We previously captured arg0, i.e. the return
// buffer, not the context â€” capture BOTH here so the diagnostic can prove
// which is which at runtime instead of us guessing from static reads.
__declspec(naked) static void HookGiveAmmo() {
    __asm {
        mov eax, [esp+4]        // arg0 (return buffer, per disassembly)
        mov g_capA0, eax
        mov eax, [esp+8]        // arg1 (ScriptContext, per disassembly)
        mov g_capA1, eax
        mov g_ctx, eax          // <-- use arg1 as the context now
        mov eax, [esp+0Ch]      // arg2 (first real script arg â€” the String)
        mov g_capA2, eax
        mov eax, [esp+10h]      // arg3 (second script arg â€” the int count)
        mov g_capA3, eax
        mov eax, [esp+14h]      // arg4 (spare, to see the pattern)
        mov g_capA4, eax
        jmp g_tramp             // run original prologue + continue in GiveAmmo
    }
}

static void InstallCapture() {
    g_giveAmmo = (BYTE*)Addr(RVA_GiveAmmo);
    // sanity: expect 51 8b 44 24 0c
    if (!(g_giveAmmo[0] == 0x51 && g_giveAmmo[1] == 0x8B)) {
        LogS("scriptvm: GiveAmmo prologue mismatch â€” capture NOT installed");
        return;
    }
    // trampoline: 5 original bytes + jmp to GiveAmmo+5
    g_tramp = (BYTE*)VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(g_tramp, g_giveAmmo, 5);
    g_tramp[5] = 0xE9;
    *(DWORD*)(g_tramp + 6) = (DWORD)((g_giveAmmo + 5) - (g_tramp + 10));
    memcpy(g_orig, g_giveAmmo, 5);
    // patch GiveAmmo entry -> jmp HookGiveAmmo
    DWORD old; VirtualProtect(g_giveAmmo, 5, PAGE_EXECUTE_READWRITE, &old);
    g_giveAmmo[0] = 0xE9;
    *(DWORD*)(g_giveAmmo + 1) = (DWORD)((BYTE*)&HookGiveAmmo - (g_giveAmmo + 5));
    VirtualProtect(g_giveAmmo, 5, old, &old);
    LogS("scriptvm: GiveAmmo capture hook installed (pick up ammo to prime)");
}

// ---- additional capture hooks: don't make the user hunt for ammo. --------
// GetHealth/GetStamina/GetMoolah are polled by the HUD every frame, so
// hooking them too means priming happens the instant a save loads â€” no
// player action needed. Whichever fires first wins (first come, first
// captured); g_ctx just needs to be A valid live ctx, not a specific one.
// PLEN = full instruction-aligned copy length (NOT just 5). The JMP patch
// itself only overwrites 5 bytes at the hook site, but the TRAMPOLINE must
// replay whole, uncut instructions â€” copying exactly 5 bytes is only safe
// when a 5-byte boundary happens to land on a real instruction edge (true
// for GiveAmmo's "51 8B44240C" = 1+4 bytes). GetHealth/GetStamina's
// "6AFF 6889C17000" = 2+5 bytes needs PLEN=7; GetMoolah's "51 C704240000000" =
// 1+7 needs PLEN=8. Copying only 5 there would slice a push-imm32 in half â€”
// exactly the bug that broke priming: those hooks fire every frame, so the
// corrupted trampoline ran constantly.
#define DEFINE_CAPTURE_HOOK(NAME, RVA, B0, B1, PLEN)                        \
    static BYTE*  g_##NAME = nullptr;                                       \
    static BYTE   g_##NAME##Orig[PLEN];                                     \
    static BYTE*  g_##NAME##Tramp = nullptr;                                \
    __declspec(naked) static void Hook##NAME() {                            \
        __asm mov eax, [esp+4]                                              \
        __asm mov g_ctx, eax                                                \
        __asm jmp g_##NAME##Tramp                                           \
    }                                                                       \
    static void InstallCapture##NAME() {                                    \
        g_##NAME = (BYTE*)Addr(RVA);                                        \
        if (!(g_##NAME[0] == B0 && g_##NAME[1] == B1)) {                    \
            LogS("scriptvm: " #NAME " prologue mismatch â€” hook NOT installed"); \
            return;                                                         \
        }                                                                   \
        g_##NAME##Tramp = (BYTE*)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE); \
        memcpy(g_##NAME##Tramp, g_##NAME, PLEN);                            \
        g_##NAME##Tramp[PLEN] = 0xE9;                                       \
        *(DWORD*)(g_##NAME##Tramp + PLEN + 1) = (DWORD)((g_##NAME + PLEN) - (g_##NAME##Tramp + PLEN + 5)); \
        memcpy(g_##NAME##Orig, g_##NAME, PLEN);                             \
        DWORD old; VirtualProtect(g_##NAME, PLEN, PAGE_EXECUTE_READWRITE, &old); \
        g_##NAME[0] = 0xE9;                                                 \
        *(DWORD*)(g_##NAME + 1) = (DWORD)((BYTE*)&Hook##NAME - (g_##NAME + 5)); \
        VirtualProtect(g_##NAME, PLEN, old, &old);                          \
        LogS("scriptvm: " #NAME " capture hook installed");                 \
    }

#define RVA_GetHealth  0x169240
#define RVA_GetStamina 0x169490

DEFINE_CAPTURE_HOOK(GetHealth,  RVA_GetHealth,  0x6A, 0xFF, 7)
DEFINE_CAPTURE_HOOK(GetStamina, RVA_GetStamina, 0x6A, 0xFF, 7)
DEFINE_CAPTURE_HOOK(GetMoolah,  RVA_GetMoolah,  0x51, 0xC7, 8)

// ---- capture a REAL GiveArtifact call (user buys/picks up an artifact) ----
// Far better than guessing the ABI: when the game itself gives an artifact we
// grab the live ctx, then ask it for argument 0 exactly the way the handler
// does (ctx->vtable[0x74](0)) and dump the Value it returns. That reveals the
// true Value layout AND a real ArtifactPref we can replay.
// GiveArtifact prologue: 55 | 8b ec | 83 e4 f8  = 1+2+3 -> PLEN 6.
static BYTE*  g_giveArt = nullptr;
static BYTE   g_giveArtOrig[6];
static BYTE*  g_giveArtTramp = nullptr;
static volatile unsigned g_artRet = 0, g_artCtx = 0;
static volatile unsigned g_artHit = 0;
static bool g_artLogged = false;

__declspec(naked) static void HookGiveArtifact() {
    __asm mov eax, [esp+4]
    __asm mov g_artRet, eax
    __asm mov eax, [esp+8]
    __asm mov g_artCtx, eax
    __asm inc g_artHit
    __asm jmp g_giveArtTramp
}

static void InstallGiveArtifactCapture() {
    g_giveArt = (BYTE*)Addr(RVA_GiveArtifact);
    if (!(g_giveArt[0] == 0x55 && g_giveArt[1] == 0x8B && g_giveArt[2] == 0xEC)) {
        LogS("scriptvm: GiveArtifact prologue mismatch â€” capture NOT installed");
        return;
    }
    g_giveArtTramp = (BYTE*)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE,
                                         PAGE_EXECUTE_READWRITE);
    memcpy(g_giveArtTramp, g_giveArt, 6);
    g_giveArtTramp[6] = 0xE9;
    *(DWORD*)(g_giveArtTramp + 7) = (DWORD)((g_giveArt + 6) - (g_giveArtTramp + 11));
    memcpy(g_giveArtOrig, g_giveArt, 6);
    DWORD old; VirtualProtect(g_giveArt, 6, PAGE_EXECUTE_READWRITE, &old);
    g_giveArt[0] = 0xE9;
    *(DWORD*)(g_giveArt + 1) = (DWORD)((BYTE*)&HookGiveArtifact - (g_giveArt + 5));
    g_giveArt[5] = 0x90;                 // pad the sliced instruction
    VirtualProtect(g_giveArt, 6, old, &old);
    LogS("scriptvm: GiveArtifact capture installed (buy/pick up an artifact)");
}

// Ask the real context for argument 0 the same way the handlers do, and dump
// the Value. Runs once, after a genuine GiveArtifact call has been seen.
typedef void* (__thiscall* getarg_t)(void* ctx, int index);
static void LogArtifactCapture() {
    if (g_artLogged || !g_artHit) return;
    g_artLogged = true;
    char b[200];
    wsprintfA(b, "==== REAL GiveArtifact seen (hits=%u) ret=%08X ctx=%08X ====",
              g_artHit, g_artRet, g_artCtx);
    LogS(b);
    __try {
        unsigned* vt = *(unsigned**)g_artCtx;
        wsprintfA(b, "  ctx vtable=%08X  slot0x74=%08X", (unsigned)(uintptr_t)vt, vt[0x74/4]);
        LogS(b);
        void* val = ((getarg_t)vt[0x74/4])((void*)g_artCtx, 0);
        wsprintfA(b, "  ctx->GetArg(0) = %08X", (unsigned)(uintptr_t)val);
        LogS(b);
        if (val) {
            unsigned* v = (unsigned*)val;
            for (int i = 0; i < 8; i++) {
                wsprintfA(b, "    Value+0x%02X = %08X (int %d)", i*4, v[i], (int)v[i]);
                LogS(b);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogS("  (fault while reading the captured context)");
    }
    LogS("==== end GiveArtifact capture ====");
}

void SWSE_ScriptVMInit() {
    if (g_inited) return;
    g_inited = true;
    g_base = (BYTE*)GetModuleHandleA(NULL);      // stranger.exe base (0x400000)
    InstallCapture();
    InstallGiveArtifactCapture();

    // Load character tuning at startup, and arm the spawn hook that applies it.
    // Without this the settings only existed after someone typed `tuning`, which
    // makes it a console command with extra steps rather than a mod -- the file
    // was silently ignored on every fresh launch.
    {
        char exe[MAX_PATH], path[MAX_PATH], msg[220];
        GetModuleFileNameA(GetModuleHandleA(NULL), exe, MAX_PATH);
        char* slash = exe;
        for (char* c = exe; *c; c++) if (*c == '\\') slash = c;
        *slash = 0;                                   // strip stranger.exe
        slash = exe;
        for (char* c = exe; *c; c++) if (*c == '\\') slash = c;
        *slash = 0;                                   // strip \bin
        wsprintfA(path, "%s\\SWSEMods\\SWSE Console\\settings.txt", exe);
        if (SWSE_LoadSettings(path, msg, sizeof(msg)) > 0) {
            LogS(msg);
            SWSE_NpcRoutineSpy(1);
        }
        wsprintfA(path, "%s\\SWSEMods\\SWSE Console\\characters.txt", exe);
        if (SWSE_LoadTuning(path, msg, sizeof(msg)) > 0) {
            LogS(msg);
            SWSE_NpcRoutineSpy(1);                    // tuning applies from the hook
        }
    }
    // GetHealth/GetStamina/GetMoolah hooks install cleanly but NEVER fire â€”
    // the HUD reads these values directly in C++, not through the script-VM
    // entry points. Disabled; GiveAmmo (an actual ammo pickup) remains the
    // only proven-reliable capture trigger. See if a level-entry .foo hook
    // (OnEnter handlers) is a better zero-action target before re-attempting.
    // InstallCaptureGetHealth();
    // InstallCaptureGetStamina();
    // InstallCaptureGetMoolah();
}

// ---- auto-prime: get a ScriptContext without the ammo dance ---------------
// Priming used to require the player to trigger GiveAmmo so we could steal a
// ScriptContext out of its arguments. But a ScriptContext is just an object
// whose first dword is its vtable â€” so we can go find one, using the same
// exact-value scan that located moolah. No player action, no hooks, no waiting.
//
// Hooking the HUD getters was tried first and failed: the HUD reads health and
// moolah directly in C++, so those handlers never fire during normal play.
#define RVA_CTX_VTABLE 0x3897E4    // seen as VA 0x00D197E4 at module base 0x00990000

// Is the captured context still a live ScriptContext? After a level load the
// old one can be freed, leaving g_ctx dangling -- reading its vtable then faults
// and InstallArgHook bails with "could not hook ctx vtable", which is why warp
// failed intermittently rather than never.
static bool CtxIsLive() {
    if (!g_ctx) return false;
    unsigned want = (unsigned)(uintptr_t)((BYTE*)GetModuleHandleA(NULL) + 0x3897E4);
    __try { return *(unsigned*)g_ctx == want; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

int SWSE_AutoPrime() {
    if (CtxIsLive()) return 1;
    g_ctx = nullptr;          // stale: drop it and go find a live one
    unsigned vt = (unsigned)(uintptr_t)((BYTE*)GetModuleHandleA(NULL) + RVA_CTX_VTABLE);
    SYSTEM_INFO si; GetSystemInfo(&si);
    BYTE* p  = (BYTE*)si.lpMinimumApplicationAddress;
    BYTE* hi = (BYTE*)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi;
    char b[160];
    while (p < hi) {
        if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
        bool ok = mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE
               && HeapRegion(mbi)
               && (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))
               && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS));
        if (ok) {
            __try {
                unsigned* q = (unsigned*)mbi.BaseAddress;
                size_t n = mbi.RegionSize / 4;
                for (size_t k = 0; k < n; k++) {
                    if (q[k] != vt) continue;
                    unsigned a = (unsigned)(uintptr_t)(q + k);
                    // HEAP ONLY. Stack frames transiently hold this vtable
                    // value too, and calling a handler through a stack "context"
                    // crashes the game -- which is exactly what happened when
                    // autoprime ran at the menu and loadsave used the result.
                    if (a < HEAP_LO || a > HEAP_HI) continue;
                    g_ctx = (void*)a;
                    wsprintfA(b, "autoprime: ScriptContext at %08X (vtable %08X)", a, vt);
                    LogS(b);
                    return 1;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        p = (BYTE*)mbi.BaseAddress + mbi.RegionSize;
    }
    LogS("autoprime: no ScriptContext in memory yet â€” load a save first");
    return 0;
}

// Scanning all of memory is not free, so a failed search backs off instead of
// repeating on every call.
static unsigned g_nextPrime = 0;
static void EnsureCtx() {
    if (g_ctx) return;
    unsigned now = GetTickCount();
    if (now < g_nextPrime) return;
    g_nextPrime = now + 3000;
    SWSE_AutoPrime();
}

bool SWSE_ScriptHaveCtx() { EnsureCtx(); return g_ctx != nullptr; }

// ---- ctxinfo: is our captured context a known RTTI class instance? --------
// If ctx is a real polymorphic object, *(unsigned*)ctx is its vtable pointer.
// Compare against vtables recovered from RTTI (swse/research/RTTI_CLASSES.md)
// to find out EXACTLY what class we're holding â€” this tells us whether we
// can call PlayerImpl's other 194 methods through this same pointer.
struct KnownVT { unsigned rva; const char* name; int methods; };
static const KnownVT kKnownVT[] = {
    { 0x36ACE4, "PlayerImpl (main)",   195 },
    { 0x36B024, "PlayerImpl (iface2)",  31 },
    { 0x36B0A4, "PlayerImpl (iface3)",  18 },
    { 0x36AFF4, "PlayerImpl (iface4)",   6 },
    { 0x375994, "NPCJump",              43 },
    { 0x366064, "FlyCamera",            29 },
    { 0x365C3C, "Camera (base)",        29 },
    { 0x366134, "FollowCamera",         29 },
    { 0x365FA4, "FixedCamera",          29 },
    { 0x36AC84, "PlayerPrefs",          22 },
};

void SWSE_ScriptCtxInfo() {
    if (!g_ctx) { LogS("ctxinfo: no context (prime with ammo first)"); return; }
    __try {
        unsigned vtable = *(unsigned*)g_ctx;
        unsigned rva = vtable - 0x400000;
        char b[128];
        wsprintfA(b, "ctxinfo: ctx=0x%p  vtable VA=0x%X  RVA=0x%X", g_ctx, vtable, rva);
        LogS(b);
        bool matched = false;
        for (auto& kv : kKnownVT) {
            if (kv.rva == rva) {
                wsprintfA(b, "  MATCH: this IS a %s instance! (%d methods available)",
                          kv.name, kv.methods);
                LogS(b);
                matched = true;
            }
        }
        if (!matched)
            LogS("  no match in the known-class table â€” check RTTI_CLASSES.md for this RVA.");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogS("ctxinfo: faulted reading vtable pointer.");
    }
}

// Call a virtual method through ctx's OWN vtable by slot index â€” only useful
// once ctxinfo confirms ctx's class, so we know what each slot means.
// argc scalar args (0-2), same calling convention as the generic dispatcher.
int SWSE_ScriptVCall(int slot, int argc, int a0, int a1) {
    if (!g_ctx) return 0;
    __try {
        unsigned vtable = *(unsigned*)g_ctx;
        unsigned fn = *((unsigned*)vtable + slot);
        typedef int (__thiscall* vfn2_t)(void*, int, int);
        int r = ((vfn2_t)fn)(g_ctx, a0, a1);
        char b[64]; wsprintfA(b, "vcall slot %d -> returned %d", slot, r);
        LogS(b);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogS("vcall: FAULTED â€” context invalidated, re-prime with ammo");
        g_ctx = nullptr;
        return -2;
    }
}

// Diagnostic: probe the captured player object AND follow one level of
// pointers, snapshotting floats. On the 2nd+ probe it logs only what CHANGED
// since the previous probe â€” so "probe, walk, probe" isolates the position
// (and any moving field) with zero guesswork. SEH-guarded.
#define PROBE_N 2048
static float g_prevSnap[PROBE_N];
static bool  g_haveSnap = false;

static float ReadF(void* base, int dwordIdx) {
    float f; memcpy(&f, (unsigned*)base + dwordIdx, 4); return f;
}

// Shared snapshot builder: self (96 dwords) + one level of member pointers
// that look like heap objects (24 dwords each). Same layout every call, so
// indices line up across repeated calls (used by both probe and watch).
// When set, snapshots are taken around this address instead of the script
// context. The ScriptContext holds no gameplay state, so scanning it finds
// nothing useful â€” but a verified pointer chain (e.g. health) lands inside
// the real player object, where health/artifacts/ammo actually live.
static void* g_watchAnchor = nullptr;
void SWSE_SetWatchAnchor(void* p) { g_watchAnchor = p; }

static int BuildSnapshot(float* snap, char origin[][24]) {
    int n = 0;
    if (g_watchAnchor) {
        // scan a window centred on the anchor: -0x200 .. +0x600 bytes
        unsigned char* base = (unsigned char*)g_watchAnchor - 0x200;
        for (int off = 0; off < 0x800 && n < PROBE_N; off += 4) {
            __try {
                memcpy(&snap[n], base + off, 4);
                if (origin) wsprintfA(origin[n], "anchor%+d", off - 0x200);
                n++;
            } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        }
        return n;
    }
    unsigned* obj = (unsigned*)g_ctx;
    for (int i = 0; i < 96 && n < PROBE_N; i++) {
        memcpy(&snap[n], &obj[i], 4);
        if (origin) wsprintfA(origin[n], "self+0x%X", i*4);
        n++;
    }
    for (int i = 0; i < 96 && n < PROBE_N - 160; i++) {
        unsigned v = obj[i];
        if (v >= 0x00800000 && v < 0x7F000000 && (v & 3) == 0) {
            __try {
                unsigned* sub = (unsigned*)v;
                // 160 dwords = 0x280 bytes, deep enough to cover the +0x1E8
                // jump-flag offset Cheat Engine found (was 24 = 0x60, too shallow).
                for (int j = 0; j < 160 && n < PROBE_N; j++) {
                    memcpy(&snap[n], &sub[j], 4);
                    if (origin) wsprintfA(origin[n], "[+0x%X]+0x%X", i*4, j*4);
                    n++;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }
    return n;
}

// ---- find: search the SAME object graph probe/watch cover for a known
// value â€” the built-in equivalent of a Cheat Engine "exact value" scan, but
// scoped to what we can already reach from ctx, so it's fast and low-noise.
// If a hit shows up, that [+selfOff]+subOff pair is usable with poke/freeze
// immediately, AND is a candidate for a permanent hardcoded SWSE offset
// (same technique that found teleport's position fields).
void SWSE_ScriptFind(int value) {
    if (!g_ctx) { LogS("find: no context (prime with ammo first)"); return; }
    __try {
        static float snap[PROBE_N];
        static char  origin[PROBE_N][24];
        int n = BuildSnapshot(snap, origin);
        int hits = 0;
        char hdr[64]; wsprintfA(hdr, "==== find %d: searching %d fields ====", value, n);
        LogS(hdr);
        for (int i = 0; i < n; i++) {
            int iv; memcpy(&iv, &snap[i], 4);
            if (iv == value) {
                char ln[64]; wsprintfA(ln, "  MATCH: %s = %d", origin[i], value);
                LogS(ln);
                hits++;
            }
        }
        char ftr[48]; wsprintfA(ftr, "==== find: %d match(es) ====", hits);
        LogS(ftr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogS("find: faulted reading context");
    }
}

// ---- watch: auto-capture a time WINDOW instead of two manual snapshots ---
// Fixes the "I can't time my alt-tab right" problem: start it, then do the
// action at your own pace â€” every frame is sampled for you, so nothing is
// missed regardless of timing.
static bool g_watching = false;
static DWORD g_watchEndTick = 0;
static float g_watchBase[PROBE_N];
static float g_watchMin[PROBE_N];
static float g_watchMax[PROBE_N];
static bool  g_watchChanged[PROBE_N];
static char  g_watchOrigin[PROBE_N][24];
static int   g_watchN = 0;
static char  g_watchLabel[64] = "";

int SWSE_ScriptWatchStart(int durationMs, const char* label) {
    if (!g_ctx) return 0;
    __try {
        g_watchN = BuildSnapshot(g_watchBase, g_watchOrigin);
        for (int i = 0; i < g_watchN; i++) {
            g_watchMin[i] = g_watchMax[i] = g_watchBase[i];
            g_watchChanged[i] = false;
        }
        lstrcpynA(g_watchLabel, label ? label : "", 64);
        g_watchEndTick = GetTickCount() + (DWORD)durationMs;
        g_watching = true;
        char b[96]; wsprintfA(b, "watch%s%s: capturing â€” do the action now.",
                              g_watchLabel[0] ? " " : "", g_watchLabel);
        LogS(b);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -2; }
}

static void DumpWatchResults() {
    char hdr[112];
    wsprintfA(hdr, "==== watch%s%s: fields that changed during the window ====",
             g_watchLabel[0] ? " " : "", g_watchLabel);
    LogS(hdr);
    LogS("-- flag-like (small integers 0-8) --");
    for (int i = 0; i < g_watchN; i++) {
        if (!g_watchChanged[i]) continue;
        unsigned ub, umn, umx;
        memcpy(&ub, &g_watchBase[i], 4);
        memcpy(&umn, &g_watchMin[i], 4);
        memcpy(&umx, &g_watchMax[i], 4);
        if (ub <= 8 && umn <= 8 && umx <= 8) {
            char ln[110];
            wsprintfA(ln, "  %-16s base=%u  min=%u  max=%u", g_watchOrigin[i], ub, umn, umx);
            LogS(ln);
        }
    }
    LogS("-- coord-like (floats that moved) --");
    for (int i = 0; i < g_watchN; i++) {
        if (!g_watchChanged[i]) continue;
        float a = g_watchBase[i], mn = g_watchMin[i], mx = g_watchMax[i];
        bool coordish = (a > 0.01f && a < 1e6f) || (a < -0.01f && a > -1e6f);
        float spread = mx - mn;
        if (coordish && spread > 0.001f && spread < 5000.0f) {
            char ln[140];
            wsprintfA(ln, "  %-16s base=%d.%03d  range=[%d.%03d .. %d.%03d]",
                      g_watchOrigin[i],
                      (int)a,  (int)((a<0?-a:a)*1000)%1000,
                      (int)mn, (int)((mn<0?-mn:mn)*1000)%1000,
                      (int)mx, (int)((mx<0?-mx:mx)*1000)%1000);
            LogS(ln);
        }
    }
    char ftr[96]; wsprintfA(ftr, "==== end watch%s%s ====", g_watchLabel[0] ? " " : "", g_watchLabel);
    LogS(ftr);
}

// call every frame from SWSE_ScriptTick while watching is active
static void WatchTick() {
    if (!g_watching) return;
    if ((long)(GetTickCount() - g_watchEndTick) >= 0) {
        g_watching = false;
        DumpWatchResults();
        return;
    }
    __try {
        static float cur[PROBE_N];
        int n = BuildSnapshot(cur, nullptr);
        for (int i = 0; i < n && i < g_watchN; i++) {
            if (cur[i] < g_watchMin[i]) g_watchMin[i] = cur[i];
            if (cur[i] > g_watchMax[i]) g_watchMax[i] = cur[i];
            unsigned ua, ub; memcpy(&ua, &g_watchBase[i], 4); memcpy(&ub, &cur[i], 4);
            if (ua != ub) g_watchChanged[i] = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_watching = false; }
}

void SWSE_ScriptDumpContext() {
    if (!g_ctx) { LogS("probe: no context (prime with ammo first)"); return; }
    __try {
        // Build a flat snapshot: [0..95] = the object itself; then for each of
        // up to a few pointer members, [k..] = 32 floats from the pointee.
        float snap[PROBE_N];
        char  origin[PROBE_N][24];
        int   n = 0;
        unsigned* obj = (unsigned*)g_ctx;
        for (int i = 0; i < 96 && n < PROBE_N; i++) {
            memcpy(&snap[n], &obj[i], 4);
            wsprintfA(origin[n], "self+0x%X", i*4);
            n++;
        }
        // follow member pointers that look like heap objects, dump 32 floats each
        for (int i = 0; i < 96 && n < PROBE_N - 32; i++) {
            unsigned v = obj[i];
            if (v >= 0x00800000 && v < 0x7F000000 && (v & 3) == 0) {
                __try {
                    unsigned* sub = (unsigned*)v;
                    for (int j = 0; j < 24 && n < PROBE_N; j++) {
                        memcpy(&snap[n], &sub[j], 4);
                        wsprintfA(origin[n], "[+0x%X]+0x%X", i*4, j*4);
                        n++;
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
        }

        if (!g_haveSnap) {
            LogS("==== probe: baseline captured. Now MOVE and 'probe' again. ====");
        } else {
            LogS("==== probe: fields that CHANGED since last probe ====");
            LogS("-- position-like (floats that moved a little) --");
            for (int i = 0; i < n; i++) {
                float a = g_prevSnap[i], b = snap[i];
                float d = b - a; if (d < 0) d = -d;
                // report plausible world-coord floats that moved a little
                bool coordA = (a > 0.01f && a < 1e6f) || (a < -0.01f && a > -1e6f);
                bool coordB = (b > 0.01f && b < 1e6f) || (b < -0.01f && b > -1e6f);
                if (coordA && coordB && d > 0.001f && d < 5000.0f) {
                    char ln[128];
                    wsprintfA(ln, "  %-16s %d.%03d -> %d.%03d", origin[i],
                              (int)a, (int)(((a<0?-a:a))*1000)%1000,
                              (int)b, (int)(((b<0?-b:b))*1000)%1000);
                    LogS(ln);
                }
            }
            LogS("-- flag-like (small integers 0-8 that flipped) --");
            for (int i = 0; i < n; i++) {
                unsigned ua, ub;
                memcpy(&ua, &g_prevSnap[i], 4);
                memcpy(&ub, &snap[i], 4);
                if (ua != ub && ua <= 8 && ub <= 8) {
                    char ln[96];
                    wsprintfA(ln, "  %-16s %u -> %u", origin[i], ua, ub);
                    LogS(ln);
                }
            }
            LogS("==== end changed ====");
            // dump the full float layout of the two nested objects that hold
            // position, so we can read the contiguous XYZ triplet cleanly.
            for (int mi = 0; mi < 96; mi++) {
                if (mi*4 != 0x84 && mi*4 != 0x94) continue;
                unsigned pv = obj[mi];
                if (pv < 0x00800000 || pv >= 0x7F000000) continue;
                char hdr[48]; wsprintfA(hdr, "  --- nested [+0x%X] floats ---", mi*4);
                LogS(hdr);
                __try {
                    unsigned* sub = (unsigned*)pv;
                    for (int j = 0; j < 32; j++) {
                        float f; memcpy(&f, &sub[j], 4);
                        if ((f > 0.01f && f < 1e6f) || (f < -0.01f && f > -1e6f)) {
                            char ln[80];
                            wsprintfA(ln, "    +0x%02X = %d.%03d", j*4,
                                      (int)f, (int)((f<0?-f:f)*1000)%1000);
                            LogS(ln);
                        }
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
        }
        memcpy(g_prevSnap, snap, sizeof(float)*n);
        g_haveSnap = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogS("probe: faulted reading context");
    }
}

// ---- replay ---------------------------------------------------------------
// True script-handler convention (established by disassembling
// TakeAllArtifacts / GiveArtifact / HasArtifactCount):
//     handler(void* retBuf, void* ctx, args...)
// arg0 is a caller-allocated return buffer the handler constructs into (and
// returns in eax); arg1 is the ScriptContext that gets vtable-called at +0x74.
// We give it a generous scratch buffer so any struct it builds has room.
typedef void (__cdecl* hnd0_t)(void* ret, void* ctx);
typedef void (__cdecl* hnd1_t)(void* ret, void* ctx, int a);
static unsigned char g_retBuf[256];

static int CallProtected(unsigned rva, int arg, bool hasArg) {
    EnsureCtx();                       // find a context ourselves if we lack one
    if (!g_ctx) return 0;
    __try {
        memset(g_retBuf, 0, sizeof(g_retBuf));
        if (hasArg) ((hnd1_t)Addr(rva))(g_retBuf, g_ctx, arg);
        else        ((hnd0_t)Addr(rva))(g_retBuf, g_ctx);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Self-heal instead of spamming: a fault means the captured context is
        // dead (freed/reallocated), so clear it. This also stops any per-frame
        // caller (god mode) from retrying every frame forever â€” g_ctx==null
        // short-circuits those checks. Re-prime with ammo to recover.
        // Self-heal instead of spamming: a fault means this context is dead
        // (freed/reallocated), so drop it. EnsureCtx will scan for a fresh one
        // on the next call rather than making the player go find ammo.
        LogS("scriptvm: replay FAULTED â€” context dropped, will re-scan");
        g_ctx = nullptr;
        return -2;
    }
}

// ==========================================================================
//  STRING-ARG MARSHALLING  (artifacts first; unlocks ~124 other functions)
//
//  Script usage is:  GiveArtifact( ArtifactPref("/data/prefs/artifacts/x.txt") )
//  ArtifactPref is a converter: String -> ArtifactPref handle.
//
//  The exact ABI for a String arg isn't 100% certain from static reads, so we
//  try both plausible shapes and VERIFY with HasArtifactCount (count must go
//  up). Everything is SEH-guarded, so a wrong guess is a no-op, not a crash.
// ==========================================================================
#define RVA_ArtifactPref      0x168E50
#define RVA_HasArtifactCount  0x163210
// Weapons take the same String->Pref->Give shape as artifacts:
//   WeaponPref(String) -> WeaponPref ; GiveCrossbow(WeaponPref)
// This is the only route to /data/prefs/weapons/crossbowupgrade.txt, which has
// no artifacts/ counterpart and so cannot be granted via the store path.
#define RVA_WeaponPref        0x168F40

// shape A: full struct-return convention  (retBuf, ctx, args...)
typedef void* (__cdecl* prefA_t)(void* ret, void* ctx, const char* s);
typedef void* (__cdecl* giveA_t)(void* ret, void* ctx, void* pref);
typedef void* (__cdecl* hasA_t )(void* ret, void* ctx, void* pref);
// shape B: simple-return convention      (ctx, args...)
typedef void* (__cdecl* prefB_t)(void* ctx, const char* s);
typedef void* (__cdecl* giveB_t)(void* ctx, void* pref);
typedef int   (__cdecl* hasB_t )(void* ctx, void* pref);

static unsigned char g_prefBuf[256];
static unsigned char g_auxBuf[256];

// Read an int result out of either eax or the return buffer.
static int CountFromResult(void* eaxRet, unsigned char* buf) {
    // a script Value object usually carries its payload a few dwords in;
    // scan the first few slots for a small plausible count.
    unsigned* p = (unsigned*)buf;
    for (int i = 0; i < 8; i++) {
        if (p[i] <= 64) return (int)p[i];
    }
    return (int)(unsigned)(uintptr_t)eaxRet;
}

// Try to give an artifact by path. mode 0 = shape A, 1 = shape B.
// Returns: 1 ok(verified), 2 called-but-unverified, -2 faulted, 0 no ctx.
static int GiveArtifactByPath(const char* path, int mode, char* msg, int msgLen) {
    if (!g_ctx) return 0;
    __try {
        void* pref = nullptr;
        memset(g_prefBuf, 0, sizeof(g_prefBuf));
        memset(g_auxBuf, 0, sizeof(g_auxBuf));

        if (mode == 0) {
            pref = ((prefA_t)Addr(RVA_ArtifactPref))(g_prefBuf, g_ctx, path);
            if (!pref) pref = g_prefBuf;
        } else {
            pref = ((prefB_t)Addr(RVA_ArtifactPref))(g_ctx, path);
        }
        if (!pref) { lstrcpynA(msg, "ArtifactPref returned null", msgLen); return -2; }

        // count before
        int before = 0;
        memset(g_auxBuf, 0, sizeof(g_auxBuf));
        if (mode == 0) {
            void* r = ((hasA_t)Addr(RVA_HasArtifactCount))(g_auxBuf, g_ctx, pref);
            before = CountFromResult(r, g_auxBuf);
        } else {
            before = ((hasB_t)Addr(RVA_HasArtifactCount))(g_ctx, pref);
        }

        // give it
        memset(g_auxBuf, 0, sizeof(g_auxBuf));
        if (mode == 0) ((giveA_t)Addr(RVA_GiveArtifact))(g_auxBuf, g_ctx, pref);
        else           ((giveB_t)Addr(RVA_GiveArtifact))(g_ctx, pref);

        // count after
        int after = 0;
        memset(g_auxBuf, 0, sizeof(g_auxBuf));
        if (mode == 0) {
            void* r = ((hasA_t)Addr(RVA_HasArtifactCount))(g_auxBuf, g_ctx, pref);
            after = CountFromResult(r, g_auxBuf);
        } else {
            after = ((hasB_t)Addr(RVA_HasArtifactCount))(g_ctx, pref);
        }

        wsprintfA(msg, "mode %c: pref=%08X count %d -> %d",
                  mode == 0 ? 'A' : 'B', (unsigned)(uintptr_t)pref, before, after);
        return (after > before) ? 1 : 2;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        wsprintfA(msg, "mode %c: FAULTED", mode == 0 ? 'A' : 'B');
        return -2;
    }
}

// ==========================================================================
//  VM ARGUMENT INJECTION  â€” the actual fix.
//
//  Handlers don't read args off our stack; they ask the context for them:
//      mov ecx,[ctx]; mov edx,[ecx+0x74]; push <index>; call edx
//      cmp [eax+0x14],<type>      ; Value type tag
//      mov eax,[eax+0x10]         ; Value payload
//
//  So we temporarily replace vtable slot 0x74 on the LIVE context with our
//  own accessor that hands back Values we built. Then a normal
//  handler(retBuf, ctx) call receives exactly the arguments we want.
// ==========================================================================
#pragma pack(push, 1)
struct VmValue {            // only the fields the handlers actually read
    unsigned pad0[4];       // +0x00..+0x0F
    unsigned payload;       // +0x10  the value itself
    unsigned type;          // +0x14  type tag (5 = object/pref in GiveArtifact)
    unsigned pad1[4];
};
#pragma pack(pop)

static VmValue  g_argVals[4];
static int      g_argCount = 0;
static unsigned g_origVF74 = 0;
static unsigned* g_vtable  = nullptr;

// __thiscall(ctx, index) -> VmValue*
static VmValue* __fastcall FakeGetArg(void* thisPtr, void* /*edx*/, int index) {
    if (index >= 0 && index < g_argCount) return &g_argVals[index];
    return &g_argVals[0];
}

static bool CtxIsLive();
int SWSE_AutoPrime();

static bool InstallArgHook() {
    // Re-acquire automatically if the context died with the last level, rather
    // than failing the command and making the user re-prime by hand.
    if (!CtxIsLive()) { g_ctx = nullptr; SWSE_AutoPrime(); }
    if (!g_ctx) return false;
    __try {
        g_vtable = *(unsigned**)g_ctx;
        if (!g_vtable) return false;
        g_origVF74 = g_vtable[0x74 / 4];
        DWORD old;
        if (!VirtualProtect(&g_vtable[0x74 / 4], 4, PAGE_READWRITE, &old)) return false;
        g_vtable[0x74 / 4] = (unsigned)(uintptr_t)&FakeGetArg;
        VirtualProtect(&g_vtable[0x74 / 4], 4, old, &old);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void RemoveArgHook() {
    if (!g_vtable || !g_origVF74) return;
    __try {
        DWORD old;
        if (VirtualProtect(&g_vtable[0x74 / 4], 4, PAGE_READWRITE, &old)) {
            g_vtable[0x74 / 4] = g_origVF74;
            VirtualProtect(&g_vtable[0x74 / 4], 4, old, &old);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_vtable = nullptr; g_origVF74 = 0;
}

static void SetArg(int i, unsigned payload, unsigned type) {
    if (i < 0 || i >= 4) return;
    memset(&g_argVals[i], 0, sizeof(VmValue));
    g_argVals[i].payload = payload;
    g_argVals[i].type    = type;
    if (i + 1 > g_argCount) g_argCount = i + 1;
}

static void* CallWithArgs(unsigned rva, void* retBuf);   // defined just below

// ---- Object arguments -----------------------------------------------------
// An Object argument is NOT a pointer. 0x168540 decodes it as a handle:
//   index = u16 at Value+0x0C, generation = u16 at Value+0x0E
//   entry = table + index*6   ->  pointer at +0x00, generation at +0x04
//   the generation must match, else it yields null
// That is why passing raw pointers as objects was never going to work, and it
// is what kept every Object-taking verb -- GotoPlayerAggressive, TeleportReset,
// SetHomePosition -- out of the generated call table.
#define RVA_HANDLE_TABLE 0x5D55F0
#define HANDLE_ENTRY     6
#define HANDLE_MAX       0x10000

// Find a live object's handle by searching the table for its pointer.
static bool HandleForObject(unsigned obj, unsigned short* idxOut,
                            unsigned short* genOut) {
    unsigned char* tbl = (unsigned char*)RvaPtr(RVA_HANDLE_TABLE);
    __try {
        for (unsigned i = 0; i < HANDLE_MAX; i++) {
            unsigned char* e = tbl + i * HANDLE_ENTRY;
            if (*(unsigned*)e != obj) continue;
            *idxOut = (unsigned short)i;
            *genOut = *(unsigned short*)(e + 4);
            return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

// An ID argument sits at Value+0x04 with a type tag of 0 or 1 -- a third
// layout again. PostAlarm reads `mov eax,[value+4]` after checking the tag,
// so writing the id at +0x10 (where scalars go) left it reading zero and then
// dereferencing it, which is what crashed at module+0x60731.
static void SetArgId(int i, unsigned id) {
    if (i < 0 || i >= 4) return;
    memset(&g_argVals[i], 0, sizeof(VmValue));
    g_argVals[i].pad0[1] = id;      // +0x04
    g_argVals[i].type    = 0;       // +0x14
    if (i + 1 > g_argCount) g_argCount = i + 1;
}

// pad0[3] is Value+0x0C -- the handle sits there as {index, generation}.
static bool SetArgObject(int i, unsigned obj) {
    if (i < 0 || i >= 4) return false;
    unsigned short idx = 0, gen = 0;
    if (!HandleForObject(obj, &idx, &gen)) return false;
    memset(&g_argVals[i], 0, sizeof(VmValue));
    g_argVals[i].pad0[3] = ((unsigned)gen << 16) | idx;
    if (i + 1 > g_argCount) g_argCount = i + 1;
    return true;
}

// Verbs that take an Object, absent from kVmFns for exactly that reason.
#define RVA_GotoPlayerAggressive 0x16B760   // ShortGoal|Object
#define RVA_SetHomePosition      0x14E380   // {void}|Object
#define RVA_TeleportReset        0x14E200   // {void}|Object|EMotionTeleport

// Find the VMInstanceInternal that belongs to a given NPC.
//
// The object autoprime captures is one of ~879 VM instances (RTTI names the
// class), not a single global context -- 190 NPCs, 879 instances. A script verb
// acts on ITS context's actor, so calling GotoPlayerAggressive through the
// player's instance told the player to go to the player, which is precisely the
// no-op we measured: handles resolved, calls returned, nothing moved.
// The instance that owns an NPC holds a pointer to it.
static unsigned g_vmInst[2048];
static int      g_vmInstCount = 0;

static void CollectVmInstances() {
    unsigned vt = (unsigned)(uintptr_t)((BYTE*)GetModuleHandleA(NULL) + 0x3897E4);
    g_vmInstCount = SWSE_FindByVtable(vt, g_vmInst, 2048);
}

// An active NPC points at its own VM instance at +0x18.
//
// This was the missing link. The search went the wrong way all along: scanning
// 879 instances for one containing the NPC's pointer or handle found almost
// nothing, because the reference runs the other way. It is also null on idle
// NPCs -- a VM instance exists only while something is running on that actor --
// which is exactly why roughly one NPC in ten ever matched.
#define NPC_VMINSTANCE 0x18

static unsigned VmInstanceOf(unsigned npc) {
    __try {
        unsigned inst = *(unsigned*)(npc + NPC_VMINSTANCE);
        if (inst < HEAP_LO || inst > HEAP_HI) return 0;
        unsigned want = (unsigned)(uintptr_t)((BYTE*)GetModuleHandleA(NULL) + 0x3897E4);
        if (*(unsigned*)inst != want) return 0;      // must really be a VMInstanceInternal
        return inst;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static unsigned VmInstanceFor(unsigned npc) {
    // Look for the actor's HANDLE as well as its pointer. Nothing matched on
    // pointer alone across 907 instances, and this engine refers to objects by
    // {index, generation} everywhere else -- script arguments included -- so the
    // instance almost certainly stores the handle too.
    unsigned short idx = 0, gen = 0;
    bool haveHandle = HandleForObject(npc, &idx, &gen);
    unsigned packed = ((unsigned)gen << 16) | idx;
    for (int i = 0; i < g_vmInstCount; i++) {
        unsigned inst = g_vmInst[i];
        __try {
            // 0x40, not 0x400: a dump showed the vtable repeating at +0x40, so
            // instances are 0x40 bytes and a deeper scan reads fifteen
            // neighbours -- any "match" past the end belongs to another object.
            for (int o = 4; o < 0x40; o += 4) {
                unsigned v = *(unsigned*)(inst + o);
                bool hit = (v == npc) || (haveHandle && v == packed);
                if (!hit) continue;
                // Log WHICH field matched: that names the actor link, instead
                // of leaving it as "something in the first 0x40 bytes".
                char lb[140];
                wsprintfA(lb, "vmlink: inst %08X +%02X == %s (npc %08X)",
                          inst, o, (v == npc) ? "pointer" : "handle", npc);
                LogS(lb);
                return inst;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return 0;
}

// When nothing matches, dump one instance so the actor link can be found by
// inspection rather than by another round of guessing at offsets.
static void DumpVmInstance(unsigned inst, unsigned npc) {
    char b[200];
    unsigned short idx = 0, gen = 0;
    HandleForObject(npc, &idx, &gen);
    wsprintfA(b, "vminst %08X (looking for npc %08X / handle %u:%u)", inst, npc, idx, gen);
    LogS(b);
    __try {
        for (int o = 0; o < 0x40; o += 0x10) {
            wsprintfA(b, "   +%02X: %08X %08X %08X %08X", o,
                      *(unsigned*)(inst + o), *(unsigned*)(inst + o + 4),
                      *(unsigned*)(inst + o + 8), *(unsigned*)(inst + o + 12));
            LogS(b);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Send NPCs at the player using the game's own AI, rather than moving them:
// direct position writes are ignored for NPCs just as they were for the player.
int SWSE_SendNpcs(int count, unsigned typeHash, char* msg, int msgLen) {
    char tmp[220], b[160];
    if (!g_ctx) { lstrcpynA(msg, "no script context - grab ammo once to prime", msgLen); return 0; }
    float* pp = PlayerPos();
    if (!pp) { lstrcpynA(msg, "no player position", msgLen); return 0; }

    int n = SWSE_FindNpcs(g_snScan, 1024);
    if (n <= 0) { lstrcpynA(msg, "no live NPCs", msgLen); return 0; }
    if (count < 1) count = 1;

    CollectVmInstances();
    void* savedCtx = g_ctx;
    int sent = 0, noVm = 0, faulted = 0;
    for (int i = 0; i < n && sent + noVm < count; i++) {
        unsigned a = g_snScan[i];
        __try {
            if (typeHash) {
                unsigned pf = PrefsOfNpc(a);
                if (!pf || *(unsigned*)(pf + 0x0C) != typeHash) continue;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }

        // Command the NPC through ITS OWN VM instance -- that is what decides
        // which actor a verb acts on.
        unsigned inst = VmInstanceFor(a);
        if (!inst) {
            // Dump a few instances against the NPC we want, so the field that
            // links an instance to its actor can be identified from data.
            if (noVm == 0) {
                for (int k = 0; k < 3 && k < g_vmInstCount; k++)
                    DumpVmInstance(g_vmInst[k], a);
            }
            noVm++;
            continue;
        }
        g_ctx = (void*)inst;
        if (!InstallArgHook()) { noVm++; continue; }
        g_argCount = 0;
        __try {
            char ret[64] = { 0 };
            CallWithArgs(RVA_GotoPlayerAggressive, ret);
            sent++;
            if (sent <= 4) {
                wsprintfA(b, "  sent %08X via its VM instance %08X", a, inst);
                LogS(b);
            }
        } __except (GrantFilter(GetExceptionInformation(), "sendnpc")) { faulted++; }
        RemoveArgHook();
    }
    g_ctx = savedCtx;

    wsprintfA(tmp, "sent %d NPC(s); %d had no VM instance, %d faulted (%d instances scanned)",
              sent, noVm, faulted, g_vmInstCount);
    lstrcpynA(msg, tmp, msgLen);
    return sent;
}

// Call handler(retBuf, ctx) with injected arguments. Returns the retBuf.
typedef void* (__cdecl* vmcall_t)(void* ret, void* ctx);
static void* CallWithArgs(unsigned rva, void* retBuf) {
    return ((vmcall_t)Addr(rva))(retBuf, g_ctx);
}

// Give an artifact using proper VM argument injection.
// type tag 5 = the object/pref type GiveArtifact checks for.
static int GiveArtifactVM(const char* path, char* msg, int msgLen) {
    if (!g_ctx) { lstrcpynA(msg, "no context", msgLen); return 0; }
    int result = -2;
    if (!InstallArgHook()) { lstrcpynA(msg, "could not hook ctx vtable", msgLen); return -2; }
    __try {
        // 1) string -> ArtifactPref
        g_argCount = 0;
        SetArg(0, (unsigned)(uintptr_t)path, 5);       // try object-type tag first
        memset(g_prefBuf, 0, sizeof(g_prefBuf));
        void* r = CallWithArgs(RVA_ArtifactPref, g_prefBuf);

        // the pref handle is normally returned via the retBuf's payload slot
        unsigned pref = ((VmValue*)g_prefBuf)->payload;
        if (!pref) pref = (unsigned)(uintptr_t)r;

        // 2) count before
        g_argCount = 0;
        SetArg(0, pref, 5);
        memset(g_auxBuf, 0, sizeof(g_auxBuf));
        CallWithArgs(RVA_HasArtifactCount, g_auxBuf);
        int before = (int)((VmValue*)g_auxBuf)->payload;

        // 3) give
        g_argCount = 0;
        SetArg(0, pref, 5);
        memset(g_auxBuf, 0, sizeof(g_auxBuf));
        CallWithArgs(RVA_GiveArtifact, g_auxBuf);

        // 4) count after
        g_argCount = 0;
        SetArg(0, pref, 5);
        memset(g_auxBuf, 0, sizeof(g_auxBuf));
        CallWithArgs(RVA_HasArtifactCount, g_auxBuf);
        int after = (int)((VmValue*)g_auxBuf)->payload;

        wsprintfA(msg, "VM: pref=%08X count %d -> %d", pref, before, after);
        result = (after > before) ? 1 : 2;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        lstrcpynA(msg, "VM: FAULTED", msgLen);
        result = -2;
    }
    RemoveArgHook();
    return result;
}

// Public: give artifact by short name ("breederbag") or full path.
// Auto-tries both ABI shapes and reports which (if either) verified.
int SWSE_GiveArtifact(const char* name, char* msg, int msgLen) {
    if (!g_ctx) { lstrcpynA(msg, "no script context yet", msgLen); return 0; }
    char path[160];
    if (strchr(name, '/') || strchr(name, '\\'))
        lstrcpynA(path, name, 160);
    else
        wsprintfA(path, "/data/prefs/artifacts/%s.txt", name);
    // strip a trailing .txt the user may have typed twice
    int L = lstrlenA(path);
    if (L > 8 && !lstrcmpiA(path + L - 8, ".txt.txt")) path[L - 4] = 0;

    LogS(path);
    // Primary path: proper VM argument injection (args come from
    // ctx->vtable[0x74], not our stack â€” see GiveArtifactVM).
    char mv[160] = {0};
    int rv = GiveArtifactVM(path, mv, 160);
    LogS(mv);
    if (rv == 1) { lstrcpynA(msg, mv, msgLen); return 1; }

    // Fallbacks: the two naive stack-arg shapes, in case this build differs.
    char m0[160] = {0}, m1[160] = {0};
    int r0 = GiveArtifactByPath(path, 0, m0, 160);
    LogS(m0);
    if (r0 == 1) { lstrcpynA(msg, m0, msgLen); return 1; }
    int r1 = GiveArtifactByPath(path, 1, m1, 160);
    LogS(m1);
    if (r1 == 1) { lstrcpynA(msg, m1, msgLen); return 1; }

    wsprintfA(msg, "%s | %s | %s", mv, m0, m1);
    return (rv == 2 || r0 == 2 || r1 == 2) ? 2 : -2;
}

// Give a weapon by prefs name, e.g. "crossbowupgrade". Unlike artifacts there
// is no store path for these: weapons have no artifacts/ token, so this goes
// through the script VM. Every earlier attempt at this shape ran on a context
// primed the old way; auto-prime makes it worth retrying.
int SWSE_GiveWeapon(const char* name, char* msg, int msgLen) {
    EnsureCtx();
    if (!g_ctx) { lstrcpynA(msg, "no script context â€” load a save first", msgLen); return 0; }

    // static: the string object below holds a pointer to this, and the game may
    // keep that reference alive past our call.
    static char path[160];
    if (strchr(name, '/') || strchr(name, '\\')) lstrcpynA(path, name, 160);
    else wsprintfA(path, "/data/prefs/weapons/%s.txt", name);
    int L = lstrlenA(path);
    if (L > 8 && !lstrcmpiA(path + L - 8, ".txt.txt")) path[L - 4] = 0;
    LogS(path);

    // A String argument is a pointer to the game's string OBJECT, not a raw
    // char*. Passing the char* made WeaponPref hash the ASCII bytes as if they
    // were a pointer â€” the fault at 0x24D926 (cmp byte ptr [esi],bl) with a
    // garbage target. Same layout the store grant path needed.
    static NameObj s_wpnName;
    memset(&s_wpnName, 0, sizeof(s_wpnName));
    s_wpnName.data     = path;
    s_wpnName.length   = lstrlenA(path) + 1;
    s_wpnName.refcount = 0x10000000;

    if (!InstallArgHook()) { lstrcpynA(msg, "could not hook ctx vtable", msgLen); return -2; }
    int result = -2;

    // GiveCrossbow takes the STRING, not a WeaponPref â€” the signature table is
    // misleading. Its own code does:
    //     getArg(0); cmp [val+0x14],5; mov eax,[val+0x10]; mov eax,[eax]
    // i.e. tag 5, payload -> a pointer whose first dword is the char*, then it
    // hashes that itself. Calling WeaponPref first was the mistake: that
    // function has a different ABI entirely and is what faulted at 0x24D926.
    __try {
        g_argCount = 0;
        SetArg(0, (unsigned)(uintptr_t)&s_wpnName, 5);
        memset(g_auxBuf, 0, sizeof(g_auxBuf));
        CallWithArgs(RVA_GiveCrossbow, g_auxBuf);
        wsprintfA(msg, "GiveCrossbow(\"%s\") called â€” check your crossbow", path);
        result = 1;
    } __except (GrantFilter(GetExceptionInformation(), "givecrossbow")) {
        lstrcpynA(msg, "FAULTED â€” see log for the fault address", msgLen);
        result = -2;
    }
    RemoveArgHook();
    return result;
}

// ---- level warp ----------------------------------------------------------
// LoadLevel / LevelTransition both take a String, and GiveCrossbow established
// how the VM passes one: tag 5, payload = pointer to a string object whose
// first dword is the char*. Levels are lm_level_00 .. lm_level_06 (+ 02a).
//
// This is also the only way to reach level-scoped prefs: crossbowupgrade lives
// in region_04's bundle and cannot resolve anywhere else.
#define RVA_LoadLevel        0x160700
#define RVA_LevelTransition  0x160510

int SWSE_LoadLevel(const char* name, bool transition, char* msg, int msgLen) {
    EnsureCtx();
    if (!g_ctx) { lstrcpynA(msg, "no script context â€” load a save first", msgLen); return 0; }

    static char     s_lvl[128];
    static NameObj  s_lvlName;
    lstrcpynA(s_lvl, name, sizeof(s_lvl));
    memset(&s_lvlName, 0, sizeof(s_lvlName));
    s_lvlName.data     = s_lvl;
    s_lvlName.length   = lstrlenA(s_lvl) + 1;
    s_lvlName.refcount = 0x10000000;

    if (!InstallArgHook()) { lstrcpynA(msg, "could not hook ctx vtable", msgLen); return -2; }
    int result = -2;
    __try {
        g_argCount = 0;
        SetArg(0, (unsigned)(uintptr_t)&s_lvlName, 5);
        memset(g_auxBuf, 0, sizeof(g_auxBuf));
        CallWithArgs(transition ? RVA_LevelTransition : RVA_LoadLevel, g_auxBuf);
        char tmp[200];
        wsprintfA(tmp, "%s(\"%s\") called",
                  transition ? "LevelTransition" : "LoadLevel", s_lvl);
        lstrcpynA(msg, tmp, msgLen);
        result = 1;
    } __except (GrantFilter(GetExceptionInformation(), "loadlevel")) {
        lstrcpynA(msg, "FAULTED â€” see log for the fault address", msgLen);
        result = -2;
    }
    RemoveArgHook();
    return result;
}

int SWSE_ScriptDo(const char* action, int arg) {
    EnsureCtx();                 // scan for a context rather than demanding ammo
    if (!g_ctx) return 0;
    // ammo / weapons
    if (!lstrcmpiA(action, "ammo"))        return CallProtected(RVA_GiveAllAmmo, 1, true);
    if (!lstrcmpiA(action, "defaultammo")) return CallProtected(RVA_GiveDefaultAmmo, 0, false);
    if (!lstrcmpiA(action, "noammo"))      return CallProtected(RVA_TakeAllAmmo, 0, false);
    if (!lstrcmpiA(action, "crossbow"))    return CallProtected(RVA_GiveCrossbow, 0, false);
    if (!lstrcmpiA(action, "noweapons"))   return CallProtected(RVA_TakeAllWeapons, 0, false);
    // health / stamina â€” direct player-object writes. The VM handlers below
    // were called for months and never changed a value; hp/stam proved the
    // fields at +0x78 / +0x8C work.
    if (!lstrcmpiA(action, "heal"))       { GodTick(); return PlayerObj() ? 1 : 0; }
    if (!lstrcmpiA(action, "maxhealth"))  { GodTick(); return PlayerObj() ? 1 : 0; }
    if (!lstrcmpiA(action, "maxstamina")) { GodTick(); return PlayerObj() ? 1 : 0; }
    if (!lstrcmpiA(action, "sethealth"))   return SWSE_PlayerSetHealth((float)arg);
    if (!lstrcmpiA(action, "kill"))        return CallProtected(RVA_Kill, 0, false);
    // form
    if (!lstrcmpiA(action, "steef"))       return CallProtected(RVA_MakePlayerSteef, 0, false);
    if (!lstrcmpiA(action, "stranger"))    return CallProtected(RVA_MakePlayerStranger, 0, false);
    if (!lstrcmpiA(action, "naked"))       return CallProtected(RVA_SetSteefNaked, 0, false);
    // camera / view
    if (!lstrcmpiA(action, "fps"))         return CallProtected(RVA_ForceFPS, 0, false);
    if (!lstrcmpiA(action, "nofps"))       return CallProtected(RVA_ForceNotFPS, 0, false);
    if (!lstrcmpiA(action, "sniper"))      return CallProtected(RVA_ForceSniper, 0, false);
    // artifacts / money
    if (!lstrcmpiA(action, "artifact"))    return CallProtected(RVA_GiveArtifact, arg, true);
    if (!lstrcmpiA(action, "artifacts"))   return CallProtected(RVA_TakeAllArtifacts, 0, false);
    if (!lstrcmpiA(action, "money"))       return CallProtected(RVA_SetMoolah, arg, true);
    // world / save / hud
    if (!lstrcmpiA(action, "tphome"))      return CallProtected(RVA_TeleportHome, 0, false);
    if (!lstrcmpiA(action, "tpreset"))     return CallProtected(RVA_TeleportReset, 0, false);
    if (!lstrcmpiA(action, "save"))        return CallProtected(RVA_QuickSave, 0, false);
    if (!lstrcmpiA(action, "checkpoint"))  return CallProtected(RVA_Checkpoint, 0, false);
    if (!lstrcmpiA(action, "loadsave"))    return CallProtected(RVA_LoadLastSave, 0, false);
    if (!lstrcmpiA(action, "healthbars"))  return CallProtected(RVA_ShowHealthBars, 1, true);
    if (!lstrcmpiA(action, "weaponhud"))   return CallProtected(RVA_OpenWeaponHUD, 0, false);
    // music
    if (!lstrcmpiA(action, "combatmusic")) return CallProtected(arg ? RVA_EnableCombatMusic : RVA_DisableCombatMusic, 0, false);
    if (!lstrcmpiA(action, "tensionmusic"))return CallProtected(arg ? RVA_EnableTensionMusic : RVA_DisableTensionMusic, 0, false);
    if (!lstrcmpiA(action, "popmusic"))    return CallProtected(RVA_PopMusic, 0, false);
    if (!lstrcmpiA(action, "pushmusic"))   return CallProtected(RVA_PushMusic, arg, true);
    if (!lstrcmpiA(action, "transmusic"))  return CallProtected(RVA_TransitionMusic, arg, true);
    return -1;
}

// ---- generic dispatcher: call ANY callable-now function by name ----------
// All are __cdecl(ctx, scalar...). cdecl is caller-cleaned, so we can always
// call through a 2-arg prototype regardless of the real arity (extra pushes
// are harmless). Float args are passed as their 32-bit bit-pattern.
// same corrected convention: (retBuf, ctx, args...)
typedef void (__cdecl* fnN_t)(void*, void*, unsigned, unsigned);

static unsigned ParseArg(char type, const char* s) {
    if (type == 'f') { float f = (float)atof(s); unsigned u; memcpy(&u, &f, 4); return u; }
    return (unsigned)atoi(s);            // i / b / e
}

int SWSE_ScriptCallByName(const char* name, int argc, char** argv) {
    EnsureCtx();
    if (!g_ctx) return 0;
    for (int i = 0; i < kVmFnCount; i++) {
        if (lstrcmpiA(name, kVmFns[i].name)) continue;
        const char* fmt = kVmFns[i].args;
        unsigned w[2] = {0, 0};
        for (int a = 0; a < 2 && fmt[a]; a++) {
            const char* s = (a + 1 < argc) ? argv[a + 1] : "0";
            w[a] = ParseArg(fmt[a], s);
        }
        __try {
            memset(g_retBuf, 0, sizeof(g_retBuf));
            ((fnN_t)Addr(kVmFns[i].rva))(g_retBuf, g_ctx, w[0], w[1]);
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LogS("scriptvm: generic call FAULTED â€” context invalidated, re-prime with ammo");
            g_ctx = nullptr;
            return -2;
        }
    }
    return -1;   // not in the table
}

// case-insensitive substring (no shlwapi dependency)
static bool ContainsI(const char* hay, const char* needle) {
    if (!needle || !*needle) return true;
    for (const char* h = hay; *h; h++) {
        const char *a = h, *b = needle;
        while (*a && *b && (*a | 0x20) == (*b | 0x20)) { a++; b++; }
        if (!*b) return true;
    }
    return false;
}

static bool StartsWithI(const char* s, const char* pre) {
    while (*pre) { if ((*s | 0x20) != (*pre | 0x20)) return false; s++; pre++; }
    return true;
}

// enumerate matching function names into a caller buffer (for `list`)
int SWSE_ScriptList(const char* filter, const char** out, int maxOut) {
    int n = 0;
    for (int i = 0; i < kVmFnCount && n < maxOut; i++)
        if (ContainsI(kVmFns[i].name, filter)) out[n++] = kVmFns[i].name;
    return n;
}

// prefix match (for Tab completion)
int SWSE_ScriptComplete(const char* prefix, const char** out, int maxOut) {
    int n = 0;
    for (int i = 0; i < kVmFnCount && n < maxOut; i++)
        if (StartsWithI(kVmFns[i].name, prefix)) out[n++] = kVmFns[i].name;
    return n;
}

// arg format string ("", "i", "if", ...) for a function name, or nullptr.
const char* SWSE_ScriptArgs(const char* name) {
    for (int i = 0; i < kVmFnCount; i++)
        if (!lstrcmpiA(name, kVmFns[i].name)) return kVmFns[i].args;
    return nullptr;
}

// ---- position read/write: teleport + vertical launch --------------------
// From the probe diff: player position vec3 lives at *(ctx+0x94) + 0x5C.
// ---- the REAL position -----------------------------------------------------
// player+0x24 is only a per-frame COPY: a watchpoint caught module+0x21E0F2
// doing `mov [esi+4],edx` from [edi], so writing the copy changes what we read
// and moves nothing. The authority is a motion object at edi-0x50.
//
// Nothing points to that object from the player â€” searching for its address
// found only stack slots. It points back at us though: +0x4C holds player+8.
// So we scan for that back-pointer. Several sibling objects match, so the
// candidate is confirmed by checking its own position equals the copy.
#define MO_OWNER 0x4C     // back-pointer to player+8
#define MO_POS   0x50     // xyz float triple

static float*   g_posSrc       = nullptr;
static unsigned g_posSrcPlayer = 0;

static bool PosMatchesCopy(float* cand, unsigned player) {
    __try {
        const float* copy = (const float*)(player + PF_POS);
        for (int i = 0; i < 3; i++) {
            float d = cand[i] - copy[i];
            if (d > 1.0f || d < -1.0f) return false;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static float* PlayerPos() {
    unsigned player = PlayerObj();
    if (!player) return nullptr;
    // Cache per player object. Deliberately NOT re-validated every call: right
    // after a teleport the source and the copy legitimately disagree for a
    // frame, and re-resolving then would reject the correct object.
    // Cached, but not blindly. The cache must survive a teleport, where the
    // source and the copy legitimately disagree for a frame -- but it must NOT
    // survive a level load, where the motion object is replaced and every
    // distance then gets measured from the player's position in the old level.
    // A large disagreement means stale, not lag.
    if (g_posSrc && g_posSrcPlayer == player) {
        __try {
            const float* copy = (const float*)(player + PF_POS);
            float dx = g_posSrc[0] - copy[0];
            float dy = g_posSrc[1] - copy[1];
            float dz = g_posSrc[2] - copy[2];
            if (dx * dx + dy * dy + dz * dz < 2500.0f) return g_posSrc;   // <50 units
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        LogS("possrc: cached motion object looks stale - re-resolving");
        g_posSrc = nullptr;
        g_posSrcPlayer = 0;
    }

    g_posSrc = nullptr;
    g_posSrcPlayer = 0;
    unsigned target = player + 8;
    SYSTEM_INFO si; GetSystemInfo(&si);
    BYTE* p  = (BYTE*)si.lpMinimumApplicationAddress;
    BYTE* hi = (BYTE*)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi;
    while (p < hi) {
        if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
        bool ok = mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE
               && HeapRegion(mbi)
               && (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))
               && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS));
        if (ok) {
            __try {
                unsigned* q = (unsigned*)mbi.BaseAddress;
                size_t n = mbi.RegionSize / 4;
                for (size_t k = 0; k < n; k++) {
                    if (q[k] != target) continue;
                    unsigned h = (unsigned)(uintptr_t)(q + k);
                    if (h < MO_OWNER) continue;
                    float* cand = (float*)(h - MO_OWNER + MO_POS);
                    if (!PosMatchesCopy(cand, player)) continue;
                    char b[110];
                    wsprintfA(b, "possrc: motion object %08X -> position @ %08X",
                              h - MO_OWNER, (unsigned)(uintptr_t)cand);
                    LogS(b);
                    g_posSrc = cand;
                    g_posSrcPlayer = player;
                    return cand;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        p = (BYTE*)mbi.BaseAddress + mbi.RegionSize;
    }
    LogS("possrc: no motion object matched the player's position");
    return nullptr;
}

static float g_savedPos[3];
static bool  g_haveSaved = false;

// r: 1 ok, 0 no context/pos, -2 fault. Fills out[3] with current pos if given.
int SWSE_PosGet(float* out) {
    __try {
        float* p = PlayerPos();
        if (!p) return 0;
        if (out) { out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; }
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -2; }
}
int SWSE_PosSave() {
    float p[3]; int r = SWSE_PosGet(p);
    if (r == 1) { memcpy(g_savedPos, p, 12); g_haveSaved = true; }
    return r;
}
int SWSE_PosRestore() {
    if (!g_haveSaved) return 3;                 // nothing saved
    __try {
        float* p = PlayerPos();
        if (!p) return 0;
        p[0] = g_savedPos[0]; p[1] = g_savedPos[1]; p[2] = g_savedPos[2];
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -2; }
}
// add delta to one axis (0/1/2). For "up"/out-of-bounds launching.
int SWSE_PosNudge(int axis, float delta) {
    if (axis < 0 || axis > 2) return -1;
    __try {
        float* p = PlayerPos();
        if (!p) return 0;
        p[axis] += delta;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -2; }
}

// ---- generic poke/freeze: same [+selfOff]+subOff addressing as probe/watch
// output, so any field found there can be written directly from the console
// (a built-in Cheat Engine-style freeze, no external tool needed). --------
static int* FieldAddr(int selfOff, int subOff) {
    if (!g_ctx) return nullptr;
    unsigned* obj = (unsigned*)g_ctx;
    unsigned sub = obj[selfOff / 4];
    if (sub < 0x00800000 || sub >= 0x7F000000) return nullptr;
    return (int*)((char*)sub + subOff);
}

int SWSE_Poke(int selfOff, int subOff, int value) {
    __try {
        int* p = FieldAddr(selfOff, subOff);
        if (!p) return 0;
        *p = value;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_ctx = nullptr; return -2; }
}

struct FrozenField { int selfOff, subOff, value; bool active; };
static FrozenField g_frozen[16];
static int g_frozenN = 0;

int SWSE_Freeze(int selfOff, int subOff, int value) {
    if (!g_ctx) return 0;
    for (int i = 0; i < g_frozenN; i++)
        if (g_frozen[i].selfOff == selfOff && g_frozen[i].subOff == subOff) {
            g_frozen[i].value = value; g_frozen[i].active = true; return 1;
        }
    if (g_frozenN >= 16) return -1;
    g_frozen[g_frozenN].selfOff = selfOff; g_frozen[g_frozenN].subOff = subOff;
    g_frozen[g_frozenN].value = value; g_frozen[g_frozenN].active = true;
    g_frozenN++;
    return 1;
}

int SWSE_Unfreeze(int selfOff, int subOff) {
    for (int i = 0; i < g_frozenN; i++)
        if (g_frozen[i].selfOff == selfOff && g_frozen[i].subOff == subOff) {
            g_frozen[i].active = false; return 1;
        }
    return 0;
}

void SWSE_UnfreezeAll() { for (int i = 0; i < g_frozenN; i++) g_frozen[i].active = false; }

static void ApplyFrozen() {
    for (int i = 0; i < g_frozenN; i++) {
        if (!g_frozen[i].active) continue;
        __try {
            int* p = FieldAddr(g_frozen[i].selfOff, g_frozen[i].subOff);
            if (p) *p = g_frozen[i].value;
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_frozen[i].active = false; }
    }
}

// ---- god mode: re-max health+stamina every frame while enabled -----------
static bool g_god = false;
// God mode refills from the player object each frame. It used to call
// SetToMaxHealthAndStam through the VM, which needed a context and never
// actually took effect.
static void GodTick() {
    unsigned p = PlayerObj();
    if (!p) return;
    __try {
        float* h = (float*)(p + PF_HEALTH);
        if (h[1] > 0.0f) h[0] = h[1];       // current = max
        float* s = (float*)(p + PF_STAMINA);
        if (s[1] > 0.0f) s[0] = s[1];
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

int SWSE_ScriptToggleGod() {
    if (!PlayerObj()) return 0;             // no save loaded; no ctx needed now
    g_god = !g_god;
    return g_god ? 1 : 2;   // 1 = now ON, 2 = now OFF
}
// One-time proof of the calling convention: when the game itself calls
// GiveAmmo we record arg0/arg1/arg2. A real ScriptContext is a polymorphic
// object, so *arg should be a vtable pointer into the module's .rdata. Log
// both candidates and say which one actually looks like an object.
static void LogCaptureDiag() {
    if (g_capLogged || !g_capA1) return;
    g_capLogged = true;
    char b[200];
    wsprintfA(b, "==== capture diag: arg0=%08X arg1=%08X arg2=%08X ====",
              g_capA0, g_capA1, g_capA2);
    LogS(b);
    unsigned modLo = (unsigned)g_base, modHi = modLo + 0x00B00000;
    for (int i = 0; i < 2; i++) {
        unsigned a = i ? g_capA1 : g_capA0;
        const char* nm = i ? "arg1" : "arg0";
        if (a < 0x10000 || a >= 0x7F000000) {
            wsprintfA(b, "  %s=%08X : not a valid pointer", nm, a);
            LogS(b); continue;
        }
        __try {
            unsigned vt = *(unsigned*)a;
            bool looksObj = (vt >= modLo && vt < modHi);
            wsprintfA(b, "  %s=%08X : first dword=%08X %s", nm, a, vt,
                      looksObj ? "<-- VTABLE, this is the ScriptContext"
                               : "(not a module pointer â€” likely the return buffer)");
            LogS(b);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            wsprintfA(b, "  %s=%08X : unreadable", nm, a);
            LogS(b);
        }
    }
    // The game's own call is GiveAmmo("<ammo>.txt", -1), so arg2 should be the
    // STRING and arg3 the count. If arg3 is a small/negative literal then ints
    // pass raw and only strings are pointers â€” which is all we need to know.
    wsprintfA(b, "  module base = %08X", (unsigned)g_base);
    LogS(b);
    wsprintfA(b, "  arg3=%08X (int %d)   arg4=%08X (int %d)",
              g_capA3, (int)g_capA3, g_capA4, (int)g_capA4);
    LogS(b);

    // Dump arg2 both as dwords and as text â€” if it (or what it points to) is a
    // readable path, String args are just char*/a struct holding one.
    for (int pass = 0; pass < 2; pass++) {
        unsigned a = pass ? 0 : g_capA2;
        if (pass) {
            __try { a = *(unsigned*)g_capA2; } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        }
        if (a < 0x10000 || a >= 0x7F000000) continue;
        wsprintfA(b, "  --- %s %08X ---", pass ? "deref *arg2 ->" : "arg2 @", a);
        LogS(b);
        __try {
            unsigned* v = (unsigned*)a;
            for (int i = 0; i < 6; i++) {
                wsprintfA(b, "    +0x%02X = %08X (int %d)", i*4, v[i], (int)v[i]);
                LogS(b);
            }
            char txt[64]; const char* s = (const char*)a;
            int k = 0;
            for (; k < 60 && s[k]; k++) {
                if ((unsigned char)s[k] < 0x20 || (unsigned char)s[k] > 0x7E) break;
                txt[k] = s[k];
            }
            txt[k] = 0;
            if (k >= 4) { wsprintfA(b, "    as TEXT: \"%s\"", txt); LogS(b); }
        } __except (EXCEPTION_EXECUTE_HANDLER) { LogS("    unreadable"); }
    }
    LogS("==== end capture diag ====");
}

void SWSE_ScriptTick() {
    LogCaptureDiag();
    LogArtifactCapture();
    if (g_god) GodTick();          // direct write; no script context required
    ApplyFrozen();
    ApplyFrozenChains();     // pointer-chain freezes (no ctx needed)
    WatchTick();
}

// ===========================================================================
//  ANIMATION PROBES  -  groundwork for impact reactions
// ===========================================================================
// The engine has no ragdoll (established earlier: no ragdoll/physique class in
// 432 RTTI classes, no blood/decal system). But it DOES have layered animation,
// which is enough to build hit reactions on:
//
//   PlayAnim            0x56CE80  ShortGoal, Object
//   PlayAnimBlend       0x56D0D0  ShortGoal, Object, float   <- blend weight
//   PlayAnimBlendHold   0x56D320  ShortGoal, Object, float
//   StartCineTorsoAnim  0x56ABD0  Object, int   <- TORSO ONLY, legs unaffected
//   EndCineTorsoAnim    0x56AE20  Object
//   StopAnimation       0x55A680  Object
//
// StartCineTorsoAnim is the interesting one: an upper-body-only animation that
// leaves the legs alone is exactly the shape of a flinch, and means the engine
// already supports partial-body layering rather than whole-body replacement.
//
// Addresses in the research docs are absolute against the preferred base
// 0x400000; SWSE works in RVAs, hence the 0x400000 subtraction.
#define RVA_StartCineTorsoAnim 0x16ABD0
#define RVA_EndCineTorsoAnim   0x16AE20
#define RVA_StopAnimation      0x15A680
#define RVA_PlayAnim           0x16CE80
#define RVA_PlayAnimBlend      0x16D0D0
#define RVA_PlayAnimBlendHold  0x16D320

// The NPC closest to the player - the one you are looking at when testing.
static unsigned NearestNpc(float* outDist) {
    static unsigned list[1024];
    int n = SWSE_FindNpcs(list, 1024);
    float* pp = PlayerPos();
    if (!pp || n <= 0) return 0;
    unsigned best = 0;
    float bestD2 = 1e30f;
    for (int i = 0; i < n; i++) {
        __try {
            float* q = (float*)(list[i] + NPC_POS);
            float dx = q[0]-pp[0], dy = q[1]-pp[1], dz = q[2]-pp[2];
            float d2 = dx*dx + dy*dy + dz*dz;
            // NaN-safe: written so a NaN fails rather than sneaking through,
            // the same trap that produced bogus "player position" hits before.
            if (d2 < bestD2) { bestD2 = d2; best = list[i]; }
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
    if (outDist) *outDist = (best && bestD2 < 1e29f) ? (float)sqrt((double)bestD2) : -1.0f;
    return best;
}

// Call a verb of the form (Object) or (Object, int) on the nearest NPC.
// The int's VM type tag is NOT known yet, so it is a parameter: this is a probe
// for finding which tag the handler accepts, not a finished feature.
int SWSE_AnimVerb(const char* which, int intArg, int tag, char* msg, int msgLen) {
    unsigned rva = 0;
    bool wantsInt = false;
    if      (!lstrcmpiA(which, "torso"))    { rva = RVA_StartCineTorsoAnim; wantsInt = true; }
    else if (!lstrcmpiA(which, "endtorso")) { rva = RVA_EndCineTorsoAnim; }
    else if (!lstrcmpiA(which, "stop"))     { rva = RVA_StopAnimation; }
    else { lstrcpynA(msg, "unknown verb (torso|endtorso|stop)", msgLen); return 0; }

    float dist = -1.0f;
    unsigned npc = NearestNpc(&dist);
    if (!npc) { lstrcpynA(msg, "no NPC found nearby", msgLen); return 0; }

    EnsureCtx();
    if (!g_ctx) { lstrcpynA(msg, "no script context", msgLen); return 0; }
    if (!InstallArgHook()) { lstrcpynA(msg, "could not hook ctx vtable", msgLen); return -2; }

    int r = -2;
    __try {
        g_argCount = 0;
        SetArgObject(0, npc);
        if (wantsInt) SetArg(1, (unsigned)intArg, (unsigned)tag);
        char ret[64] = { 0 };
        CallWithArgs(rva, ret);
        char tmp[220];
        wsprintfA(tmp, "%s on npc %08X (%d.%02d units away)%s",
                  which, npc, (int)dist, (int)(dist*100)%100,
                  wantsInt ? "" : " [no int arg]");
        lstrcpynA(msg, tmp, msgLen);
        r = 1;
    } __except (GrantFilter(GetExceptionInformation(), "animverb")) {
        lstrcpynA(msg, "FAULTED - see log for the fault address", msgLen);
        r = -2;
    }
    RemoveArgHook();
    return r;
}
