// SWSE live prefs editor - see prefsedit.h.

#include "prefsedit.h"
#include "reflect_gen.h"
#include "modregistry.h"
#include "levelwatch.h"
#include "features.h"
#include "scriptvm.h"          // SWSE_HashPath - the game's own path hasher
#include "gamebuild.h"         // safe mode: no read through the Steam ResourceManager
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The same heap window every other SWSE scan uses (see scriptvm.cpp HEAP_LO).
#define PE_HEAP_LO   0x10000000u
#define PE_HEAP_HI   0x40000000u
#define PE_OWNHASH   0x0C          // a prefs object's own path hash
#define PE_MAXHITS   256

// ---- logging -------------------------------------------------------------
static void LogE(const char* s) {
    char path[MAX_PATH];
    GetModuleFileNameA(GetModuleHandleA(NULL), path, MAX_PATH);
    char* sl = strrchr(path, '\\'); if (sl) *sl = 0;
    char full[MAX_PATH];
    wsprintfA(full, "%s\\swse_log.txt", path);
    FILE* f = fopen(full, "a");
    if (!f) return;
    fprintf(f, "%s\n", s);
    fclose(f);
}

// ---- the module image, for vtable / RTTI validation -----------------------
static unsigned g_modBase = 0, g_modSize = 0;

static void ModuleRange() {
    if (g_modBase) return;
    HMODULE m = GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)m;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((BYTE*)m + dos->e_lfanew);
    g_modSize = nt->OptionalHeader.SizeOfImage;
    g_modBase = (unsigned)(uintptr_t)m;
}

static bool InModule(unsigned a) {
    return a >= g_modBase && a < g_modBase + g_modSize;
}

// Class name from a vtable, via MSVC RTTI: vt[-1] -> CompleteObjectLocator
// (signature 0 on x86) -> +0x0C TypeDescriptor -> +8 ".?AVName@@". Nested
// classes keep their scope the way the reflection table spells them:
// ".?AVGPrefs@GamePrefs@@" -> "GPrefs@GamePrefs".
static bool TdName(unsigned td, char* out, int outLen) {
    out[0] = 0;
    if (!InModule(td)) return false;
    __try {
        const char* nm = (const char*)(td + 8);
        if (nm[0] != '.' || nm[1] != '?' || nm[2] != 'A') return false;
        nm += 4;
        int i = 0;
        for (; i < outLen - 1 && nm[i] && !(nm[i] == '@' && nm[i + 1] == '@'); i++) {
            char c = nm[i];
            if (c < 32 || c > 126) return false;
            out[i] = c;
        }
        out[i] = 0;
        return i > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; return false; }
}

static bool VtName(unsigned vt, char* out, int outLen) {
    out[0] = 0;
    ModuleRange();
    if (!InModule(vt)) return false;
    __try {
        unsigned col = *(unsigned*)(vt - 4);
        if (!InModule(col) || *(unsigned*)col != 0) return false;
        return TdName(*(unsigned*)(col + 0x0C), out, outLen);
    } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; return false; }
}

// Is obj a `cls` (itself or by inheritance)? Walks the RTTI class hierarchy:
// COL+0x10 -> ClassHierarchyDescriptor {+8 count, +0xC base-class array},
// each BaseClassDescriptor starting with its TypeDescriptor.
bool SWSE_ObjIsA(unsigned obj, const char* cls) {
    ModuleRange();
    __try {
        unsigned vt = *(unsigned*)obj;
        if (!InModule(vt)) return false;
        unsigned col = *(unsigned*)(vt - 4);
        if (!InModule(col) || *(unsigned*)col != 0) return false;
        unsigned chd = *(unsigned*)(col + 0x10);
        if (!InModule(chd)) return false;
        unsigned n = *(unsigned*)(chd + 8), bca = *(unsigned*)(chd + 0x0C);
        if (!InModule(bca) || n > 64) return false;
        for (unsigned k = 0; k < n; k++) {
            unsigned bcd = *(unsigned*)(bca + k * 4);
            if (!InModule(bcd)) continue;
            char nm[128];
            if (TdName(*(unsigned*)bcd, nm, sizeof(nm)) && !lstrcmpiA(nm, cls)) return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

// ---- the game's own resource registry ---------------------------------------
// ResourceManager* lives at RVA 0x5D55A8; every loaded resource (every prefs
// object among them) is registered keyed by Resource+0x0C, the path hash. Two
// containers exist: a vector sorted by key at mgr+0x30 (what a running game
// uses - measured) and a hash map at mgr+0x2C (static RE: buckets from map+8 to
// map+0xC, node {+0 next, +4 key, +8 Resource*}, bucket key % (nbuckets-1),
// running from buckets[i] up to buckets[i+1]). Read-only: no call, no
// reference count touched. (The engine's own lookup, RVA 0x251960, raises the
// refcount; RVA 0x23880 CREATES a default NPCPrefs on a miss - neither used.)
#define RVA_RESMGR 0x5D55A8

static int g_mgrOk = -1;          // -1 unknown, 0 unusable, 1 working
int SWSE_ResourceRegistryOk() { return g_mgrOk; }

unsigned SWSE_ResourceLookup(unsigned key) {
    // Safe mode (gamebuild.h): RVA_RESMGR is a Steam address. The prefs
    // commands and the prefsedit switch are refused there already; this is
    // for the plugin API's Game.ResourceLookup.
    if (SWSE_GameBuildSafeMode()) return 0;
    ModuleRange();
    if (!key || key == 0x2DFD1072) return 0;      // the null token
    __try {
        unsigned mgr = *(unsigned*)(g_modBase + RVA_RESMGR);
        if (mgr < 0x10000) { g_mgrOk = 0; return 0; }
        // MEASURED LIVE: in a running game the hash map at +0x2C is empty and
        // the registry is the vector at +0x30 - {data, capacity, count} of
        // Resource* sorted ascending by Resource+0x0C (6416 entries in
        // lm_level_01). Binary search it: ~13 reads, no call, nothing touched.
        unsigned vec = *(unsigned*)(mgr + 0x30);
        if (vec >= 0x10000) {
            unsigned data = *(unsigned*)vec, cap = *(unsigned*)(vec + 4);
            unsigned cnt = *(unsigned*)(vec + 8);
            if (data >= 0x10000 && cnt && cnt <= cap && cnt < 1000000) {
                g_mgrOk = 1;
                unsigned lo = 0, hi = cnt;
                while (lo < hi) {
                    unsigned mid = lo + (hi - lo) / 2;
                    unsigned res = *(unsigned*)(data + mid * 4);
                    unsigned k = *(unsigned*)(res + PE_OWNHASH);
                    if (k == key) return res;
                    if (k < key) lo = mid + 1; else hi = mid;
                }
                return 0;
            }
        }
        // The hash-map form the constructor sets up (static RE); used when the
        // vector is absent.
        unsigned map = *(unsigned*)(mgr + 0x2C);
        if (map < 0x10000) { g_mgrOk = 0; return 0; }
        unsigned begin = *(unsigned*)(map + 8), end = *(unsigned*)(map + 0x0C);
        if (end <= begin) { g_mgrOk = 0; return 0; }
        unsigned nb = (end - begin) / 4;
        if (nb < 2) { g_mgrOk = 0; return 0; }
        g_mgrOk = 1;
        unsigned i = key % (nb - 1);
        unsigned node = *(unsigned*)(begin + i * 4);
        unsigned stop = *(unsigned*)(begin + (i + 1) * 4);
        for (int guard = 0; node && node != stop && guard < 4096; guard++) {
            if (*(unsigned*)(node + 4) == key) {
                unsigned res = *(unsigned*)(node + 8);
                // Trust but verify: a real resource carries the key itself.
                if (res >= 0x10000 && *(unsigned*)(res + PE_OWNHASH) == key) return res;
                return 0;
            }
            node = *(unsigned*)node;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_mgrOk = 0; }
    return 0;
}

static bool ObjVt(unsigned obj, unsigned* vt) {
    __try { *vt = *(unsigned*)obj; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool ObjName(unsigned obj, char* out, int outLen) {
    unsigned vt = 0;
    if (!ObjVt(obj, &vt)) { out[0] = 0; return false; }
    return VtName(vt, out, outLen);
}

static unsigned OwnHash(unsigned obj) {
    __try { return *(unsigned*)(obj + PE_OWNHASH); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Every vtable belonging to an RTTI class name. Finds the TypeDescriptor by its
// decorated name, then the locators that point at it, then the vtables whose
// [-1] slot points at a locator. Multiple inheritance gives several.
static int ClassVtables(const char* cls, unsigned* out, int maxOut) {
    ModuleRange();
    char want[160];
    wsprintfA(want, ".?AV%s@@", cls);
    int wl = lstrlenA(want);
    unsigned tds[4]; int ntd = 0;

    // Pass 1: the TypeDescriptor name string (also try the struct form .?AU).
    MEMORY_BASIC_INFORMATION mbi;
    for (int form = 0; form < 2 && !ntd; form++) {
        if (form == 1) want[3] = 'U';
        for (unsigned a = g_modBase; a < g_modBase + g_modSize && ntd < 4; ) {
            if (!VirtualQuery((void*)(uintptr_t)a, &mbi, sizeof(mbi))) break;
            unsigned end = (unsigned)(uintptr_t)mbi.BaseAddress + (unsigned)mbi.RegionSize;
            bool ok = mbi.State == MEM_COMMIT &&
                      !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
            if (ok) {
                __try {
                    const char* p = (const char*)(uintptr_t)a;
                    const char* e = (const char*)(uintptr_t)end - wl - 1;
                    for (; p < e && ntd < 4; p++) {
                        if (p[0] != '.' || p[1] != '?' || memcmp(p, want, wl)) continue;
                        if (p[wl] != 0) continue;
                        tds[ntd++] = (unsigned)(uintptr_t)p - 8;
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
            a = end;
        }
    }
    if (!ntd) return 0;

    // Pass 2: locators (+0x0C == TD, signature 0), then vtables (dword == COL).
    unsigned cols[16]; int ncol = 0;
    int n = 0;
    for (int step = 0; step < 2; step++) {
        for (unsigned a = g_modBase; a < g_modBase + g_modSize; ) {
            if (!VirtualQuery((void*)(uintptr_t)a, &mbi, sizeof(mbi))) break;
            unsigned end = (unsigned)(uintptr_t)mbi.BaseAddress + (unsigned)mbi.RegionSize;
            bool ok = mbi.State == MEM_COMMIT &&
                      !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
            if (ok) {
                __try {
                    for (unsigned q = a; q + 4 <= end; q += 4) {
                        unsigned v = *(unsigned*)(uintptr_t)q;
                        if (step == 0) {
                            for (int t = 0; t < ntd; t++) {
                                if (v != tds[t] || q < a + 0x0C) continue;
                                unsigned col = q - 0x0C;
                                if (*(unsigned*)(uintptr_t)col == 0 && ncol < 16) cols[ncol++] = col;
                            }
                        } else {
                            for (int c = 0; c < ncol; c++) {
                                if (v != cols[c] || n >= maxOut) continue;
                                out[n++] = q + 4;
                            }
                        }
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
            a = end;
        }
        if (step == 0 && !ncol) return 0;
    }
    return n;
}

// ---- one heap pass for many hashes and vtables -----------------------------
struct PeHit { unsigned obj; unsigned key; int isClass; };

// Two locks. g_scanCs serialises heap passes (they share the prefilter) and
// the apply path's static buffers; g_cs guards the baseline table. The scan
// lock is always taken first, so they cannot deadlock.
static CRITICAL_SECTION g_cs, g_scanCs;
static struct PeLocks {
    PeLocks()  { InitializeCriticalSection(&g_cs); InitializeCriticalSection(&g_scanCs); }
} s_peLocks;
static void Lock()   { EnterCriticalSection(&g_cs); }
static void Unlock() { LeaveCriticalSection(&g_cs); }

// Thread stacks are MEM_PRIVATE read/write memory too, and a stale vtable
// pointer on a stack is not an object - writing "its" field would corrupt a
// live frame. Every stack reservation carries a guard page, so an allocation
// with one is skipped.
static bool AllocationIsStack(unsigned allocBase) {
    MEMORY_BASIC_INFORMATION m;
    unsigned a = allocBase;
    for (int i = 0; i < 256; i++) {
        if (!VirtualQuery((void*)(uintptr_t)a, &m, sizeof(m))) return false;
        if ((unsigned)(uintptr_t)m.AllocationBase != allocBase) return false;
        if (m.Protect & PAGE_GUARD) return true;
        a = (unsigned)(uintptr_t)m.BaseAddress + (unsigned)m.RegionSize;
    }
    return false;
}

// 64K-bit prefilter on the low 16 bits, then an exact check: a pass over the
// whole heap window costs the same for one key as for a few hundred.
static unsigned char g_filter[8192];

static int HeapPass(const unsigned* hashes, int nh, const unsigned* vts, int nv,
                    PeHit* out, int maxOut) {
    memset(g_filter, 0, sizeof(g_filter));
    for (int i = 0; i < nh; i++) g_filter[(hashes[i] & 0xFFFF) >> 3] |= 1 << (hashes[i] & 7);
    for (int i = 0; i < nv; i++) g_filter[(vts[i] & 0xFFFF) >> 3] |= 1 << (vts[i] & 7);
    int n = 0;
    unsigned lastAlloc = 0xFFFFFFFF; bool lastStack = false;
    MEMORY_BASIC_INFORMATION mbi;
    for (unsigned a = PE_HEAP_LO; a < PE_HEAP_HI && n < maxOut; ) {
        if (!VirtualQuery((void*)(uintptr_t)a, &mbi, sizeof(mbi))) break;
        unsigned end = (unsigned)(uintptr_t)mbi.BaseAddress + (unsigned)mbi.RegionSize;
        bool ok = mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
                  (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) &&
                  !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS));
        if (ok) {
            unsigned ab = (unsigned)(uintptr_t)mbi.AllocationBase;
            if (ab != lastAlloc) { lastAlloc = ab; lastStack = AllocationIsStack(ab); }
            if (lastStack) ok = false;
        }
        if (ok) {
            __try {
                for (unsigned q = a; q + 4 <= end && n < maxOut; q += 4) {
                    unsigned v = *(unsigned*)(uintptr_t)q;
                    if (!(g_filter[(v & 0xFFFF) >> 3] & (1 << (v & 7)))) continue;
                    for (int i = 0; i < nv; i++) {
                        if (v != vts[i]) continue;
                        out[n].obj = q; out[n].key = v; out[n].isClass = 1; n++;
                        break;
                    }
                    if (q < a + PE_OWNHASH) continue;
                    for (int i = 0; i < nh && n < maxOut; i++) {
                        if (v != hashes[i]) continue;
                        // Only a real object counts: a valid RTTI vtable where a
                        // prefs object keeps it. A bare copy of the hash inside
                        // another structure (an NPCTag, m_rangedWeapon) fails.
                        unsigned obj = q - PE_OWNHASH;
                        char nm[96];
                        if (!VtName(*(unsigned*)(uintptr_t)obj, nm, sizeof(nm))) break;
                        out[n].obj = obj; out[n].key = v; out[n].isClass = 0; n++;
                        break;
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        a = end;
    }
    return n;
}

// ---- targets ----------------------------------------------------------------
enum { TGT_BAD = 0, TGT_HASH, TGT_CLASS, TGT_ADDR };
struct Target { int kind; unsigned value; char cls[96]; };

static bool IsHex8(const char* s) {
    int n = 0;
    for (; s[n]; n++) {
        char c = s[n];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    }
    return n == 8;
}

static bool ParseTarget(const char* s, Target* t, char* err, int errLen) {
    memset(t, 0, sizeof(*t));
    if (!s || !*s) { lstrcpynA(err, "no target", errLen); return false; }
    if (s[0] == '@') {
        t->kind = TGT_CLASS;
        lstrcpynA(t->cls, s + 1, sizeof(t->cls));
        return t->cls[0] != 0;
    }
    if (s[0] == '&') {
        t->kind = TGT_ADDR;
        t->value = (unsigned)strtoul(s + 1, nullptr, 16);
        return t->value != 0;
    }
    if (strchr(s, '/') || strchr(s, '\\') || strstr(s, ".txt")) {
        // Shorthand: "weapons/damagedynamite" means
        // /data/prefs/weapons/damagedynamite.txt.
        char full[260];
        if (s[0] == '/' || s[0] == '\\') lstrcpynA(full, s, sizeof(full));
        else wsprintfA(full, "/data/prefs/%s", s);
        int fl = lstrlenA(full);
        if (fl < 4 || lstrcmpiA(full + fl - 4, ".txt")) lstrcatA(full, ".txt");
        t->kind = TGT_HASH;
        t->value = SWSE_HashPath(full);
        return true;
    }
    const char* h = s;
    if (h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) h += 2;
    if (IsHex8(h)) {
        t->kind = TGT_HASH;
        t->value = (unsigned)strtoul(h, nullptr, 16);
        return true;
    }
    char tmp[1100];
    wsprintfA(tmp, "'%s' is not a prefs path, an 8-digit hash, @Class or &address", s);
    lstrcpynA(err, tmp, errLen);
    return false;
}

// How the last FindObjects resolved its hash targets, for `prefs find`.
static int g_lastViaRegistry = 0, g_lastViaScan = 0;

// Resolve targets to live objects. Hash targets go through the game's resource
// registry first (instant, exact); only what it cannot answer - or everything,
// if the registry is unreadable - falls back to one heap pass, shared with the
// class targets.
static int FindObjects(const Target* ts, int nt, PeHit* out, int maxOut) {
    unsigned hashes[128]; int nh = 0;
    unsigned vts[64];     int nv = 0;
    int n = 0;
    g_lastViaRegistry = g_lastViaScan = 0;
    EnterCriticalSection(&g_scanCs);
    for (int i = 0; i < nt; i++) {
        if (ts[i].kind == TGT_HASH) {
            unsigned res = SWSE_ResourceLookup(ts[i].value);
            char nm[8]; unsigned rvt = 0;
            if (res && n < maxOut && ObjVt(res, &rvt) && VtName(rvt, nm, sizeof(nm))) {
                out[n].obj = res; out[n].key = ts[i].value; out[n].isClass = 0; n++;
                g_lastViaRegistry++;
                continue;
            }
            // Registry working and the key absent = not loaded; scanning the
            // heap would only find the same nothing, slowly.
            if (g_mgrOk == 1) continue;
            if (nh < 128) hashes[nh++] = ts[i].value;
        }
        else if (ts[i].kind == TGT_CLASS && nv < 60)
            nv += ClassVtables(ts[i].cls, vts + nv, 64 - nv);
        else if (ts[i].kind == TGT_ADDR && n < maxOut) {
            out[n].obj = ts[i].value; out[n].key = ts[i].value; out[n].isClass = 2; n++;
        }
    }
    if (nh || nv) {
        int before = n;
        n += HeapPass(hashes, nh, vts, nv, out + n, maxOut - n);
        for (int k = before; k < n; k++) if (out[k].isClass == 0) g_lastViaScan++;
    }
    LeaveCriticalSection(&g_scanCs);
    return n;
}

// A class-wide target found by vtable alone is weaker evidence than a hash
// match, so before writing a FLOAT through one, the value already there must be
// a plausible prefs number. Garbage there means "not really an object" - the
// motion-prefs lesson, where writing every vtable match crashed the game.
static bool PlausibleFloatAt(unsigned addr) {
    float v = 0.0f;
    __try { v = *(float*)(uintptr_t)addr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return v >= -10000000.0f && v <= 10000000.0f;     // NaN fails both
}

static bool HitMatches(const PeHit& h, const Target& t) {
    if (t.kind == TGT_ADDR)  return h.isClass == 2 && h.obj == t.value;
    if (t.kind == TGT_HASH)  return h.isClass == 0 && h.key == t.value;
    if (t.kind == TGT_CLASS) {
        if (h.isClass != 1) return false;
        char nm[96];
        return VtName(h.key, nm, sizeof(nm)) && !lstrcmpiA(nm, t.cls);
    }
    return false;
}

// ---- fields -------------------------------------------------------------------
// ro: a struct/vector/array (code 8) or a string handle (code 6). Shown, but
// never written unless the user names a type (`m_x:i`) - a number written
// into a struct corrupts it, and a raw value in a string handle is a crash
// waiting for the game to use it.
struct FieldRef { int off; char type; bool ro; char label[96]; };

// reflect_gen.h type codes -> editor types. 's' = not a scalar (struct,
// vector, array): shown, never written unless the user names a type.
static char TypeOfCode(int code) {
    if (code == 2) return 'i';
    if (code == 3) return 'b';
    if (code == 4 || code == 6 || code == 7) return 'h';
    if (code == 8) return 's';
    return 'f';
}

static const char* TypeLabel(int code) {
    static const char* k[] = { "?", "f32", "int", "bool", "hash", "vec", "string", "color", "struct" };
    return (code >= 0 && code <= 8) ? k[code] : "?";
}

// Does the reflection table describe this class at all?
static bool ClassKnown(const char* cls) {
    if (!cls || !*cls) return false;
    for (int i = 0; i < kReflectFieldCount; i++)
        if (!lstrcmpiA(kReflectFields[i].cls, cls)) return true;
    return false;
}

// One name, one class (or the object's own class), no component suffix.
// code receives the table's type code (0 for a raw name match without one).
static bool ResolveFieldCore(const char* cls, const char* name, const char* objClass,
                             FieldRef* f, int* code, char* err, int errLen) {
    // Exact class first - the qualified one, else the object's own RTTI class.
    // The table lists inherited fields under every class, so this is complete.
    const char* want = (cls && *cls) ? cls : objClass;
    if (want && *want) {
        for (int i = 0; i < kReflectFieldCount; i++) {
            if (lstrcmpiA(kReflectFields[i].field, name) ||
                lstrcmpiA(kReflectFields[i].cls, want)) continue;
            f->off = kReflectFields[i].off;
            *code = kReflectFields[i].type;
            _snprintf_s(f->label, sizeof(f->label), _TRUNCATE, "%s::%s",
                        kReflectFields[i].cls, kReflectFields[i].field);
            return true;
        }
        // A class the table fully describes (inherited fields included) that
        // lacks the name does NOT have the field. Falling back to another
        // class's offset there would write the wrong member - m_fireRate is
        // NPCWeaponPrefs +0x17C, but +0x17C of an ammo object is a hash.
        if ((cls && *cls) || ClassKnown(want)) {
            char t[1100];
            wsprintfA(t, "no field %s in class %s (try `prefs fields %s`)", name, want, want);
            lstrcpynA(err, t, errLen);
            return false;
        }
    }
    // Otherwise (a class the table does not know) the name alone, but only if
    // every class agrees on the offset.
    int first = -1, distinct = 0;
    for (int i = 0; i < kReflectFieldCount; i++) {
        if (lstrcmpiA(kReflectFields[i].field, name)) continue;
        if (first < 0) { first = i; distinct = 1; }
        else if (kReflectFields[i].off != kReflectFields[first].off) distinct++;
    }
    if (first < 0) {
        char t[1100];
        wsprintfA(t, "unknown field '%s' (try `prefs fields %s`, or a hex offset)", name, name);
        lstrcpynA(err, t, errLen);
        return false;
    }
    if (distinct > 1) {
        char t[1100];
        int used = wsprintfA(t, "'%s' is ambiguous here - use Class::%s or an offset:", name, name);
        int shown = 0;
        for (int i = 0; i < kReflectFieldCount && shown < 4; i++) {
            if (lstrcmpiA(kReflectFields[i].field, name)) continue;
            used += wsprintfA(t + used, " %s=0x%X", kReflectFields[i].cls, kReflectFields[i].off);
            shown++;
        }
        lstrcpynA(err, t, errLen);
        return false;
    }
    f->off = kReflectFields[first].off;
    *code = kReflectFields[first].type;
    lstrcpynA(f->label, kReflectFields[first].field, sizeof(f->label));
    return true;
}

// field spec: m_name | Class::m_name | 0x1A4 | +1A4, then optionally .x/.y/.z/.w
// (or .r/.g/.b/.a) for a vector field's component, then :f :i :b :h.
static bool ResolveField(const char* spec, const char* objClass, FieldRef* f,
                         char* err, int errLen) {
    char s[96];
    lstrcpynA(s, spec, sizeof(s));
    f->type = 0;
    f->ro = false;
    char forced = 0;
    int len = lstrlenA(s);
    if (len > 2 && s[len - 2] == ':' && (len < 3 || s[len - 3] != ':')) {
        char c = s[len - 1] | 0x20;
        if (c == 'f' || c == 'i' || c == 'b' || c == 'h') { forced = c; s[len - 2] = 0; }
    }
    if ((s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) || s[0] == '+') {
        f->off = (int)strtoul(s + (s[0] == '+' ? 1 : 2), nullptr, 16);
        f->type = forced ? forced : 'f';
        wsprintfA(f->label, "+0x%X", f->off);
        return true;
    }
    char cls[64] = { 0 };
    char* name = s;
    char* dc = strstr(s, "::");
    if (dc) { *dc = 0; lstrcpynA(cls, s, sizeof(cls)); name = dc + 2; }

    int code = 0;
    if (ResolveFieldCore(cls, name, objClass, f, &code, err, errLen)) {
        f->type = forced ? forced : TypeOfCode(code);
        f->ro = !forced && (code == 8 || code == 6);
        return true;
    }
    // A vector component: m_pos.y is m_pos + 4.
    len = lstrlenA(name);
    if (len > 2 && name[len - 2] == '.') {
        const char* comps = "xyzwrgba";
        const char* at = strchr(comps, name[len - 1] | 0x20);
        if (at) {
            int idx = (int)(at - comps) & 3;
            char base[96];
            lstrcpynA(base, name, len - 1);                 // drop ".c"
            char err2[240];
            FieldRef bf;
            int bcode = 0;
            if (ResolveFieldCore(cls, base, objClass, &bf, &bcode, err2, sizeof(err2)) &&
                bcode == 5) {
                f->off = bf.off + idx * 4;
                f->type = forced ? forced : 'f';
                _snprintf_s(f->label, sizeof(f->label), _TRUNCATE, "%s.%c", bf.label, name[len - 1]);
                return true;
            }
        }
    }
    return false;       // err holds the first attempt's reason
}

static int TypeSize(char t) { return t == 'b' ? 1 : 4; }

// The whole token must be the value. 1.0.x took any leading number, so the
// null hash "2DFD1072" given to a float field wrote 2, and "010" was octal 8.
static bool TokenEnd(const char* e) {
    while (*e == ' ' || *e == '\t' || *e == '\r' || *e == '\n') e++;
    return !*e;
}

static bool ParseValue(const char* s, char type, unsigned char* bytes) {
    if (!s || !*s) return false;
    if (type == 'f') {
        // A comma is a decimal point (a French or German Windows writes 0,5).
        char t[64]; lstrcpynA(t, s, sizeof(t));
        for (char* c = t; *c; c++) if (*c == ',') *c = '.';
        char* e = nullptr; float v = (float)strtod(t, &e);
        if (e == t || !TokenEnd(e)) return false;
        memcpy(bytes, &v, 4); return true;
    }
    if (type == 'h') {
        unsigned v;
        if (strchr(s, '/') || strchr(s, '\\')) v = SWSE_HashPath(s);
        else {
            char* e = nullptr;
            v = (unsigned)strtoul(s, &e, 16);
            if (e == s || !TokenEnd(e)) return false;
        }
        memcpy(bytes, &v, 4); return true;
    }
    const char* q = s;
    if (*q == '-' || *q == '+') q++;
    int base = (q[0] == '0' && (q[1] == 'x' || q[1] == 'X')) ? 16 : 10;
    char* e = nullptr; long v = strtol(s, &e, base);
    if (e == s || !TokenEnd(e)) return false;
    if (type == 'b') { bytes[0] = (unsigned char)v; return true; }
    memcpy(bytes, &v, 4); return true;
}

static void FmtFloat(char* out, float v) {
    if (v != v) { lstrcpyA(out, "nan"); return; }
    sprintf_s(out, 32, "%.4g", v);
}

static void FmtValue(char* out, char type, const unsigned char* b) {
    if (type == 'b') { wsprintfA(out, "%u", b[0]); return; }
    unsigned u; memcpy(&u, b, 4);
    if (type == 'i') { wsprintfA(out, "%d", (int)u); return; }
    if (type == 'h') { wsprintfA(out, "%08X", u); return; }
    float f; memcpy(&f, b, 4);
    char fs[32]; FmtFloat(fs, f);
    wsprintfA(out, "%s", fs);
}

static bool ReadBytes(unsigned addr, int n, unsigned char* out) {
    __try { memcpy(out, (void*)(uintptr_t)addr, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool WriteBytes(unsigned addr, int n, const unsigned char* in) {
    __try { memcpy((void*)(uintptr_t)addr, in, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- baselines: the shipped value of everything written --------------------
struct PeBase {
    unsigned obj, off, vt, own;
    unsigned char size, orig[4];
};
#define PE_MAXBASE 2048
static PeBase g_base[PE_MAXBASE];
static int    g_baseN = 0;

// Still the same object? Same vtable, and for hash-found objects the same own
// hash - a freed address reused by something else must never be written.
static bool BaseValid(const PeBase& b) {
    unsigned vt = 0;
    if (!ObjVt(b.obj, &vt) || vt != b.vt) return false;
    if (b.own && OwnHash(b.obj) != b.own) return false;
    return true;
}

static bool WriteWithBaseline(unsigned obj, int off, int size, const unsigned char* val) {
    unsigned vt = 0;
    if (!ObjVt(obj, &vt)) return false;
    unsigned own = OwnHash(obj);
    Lock();
    int found = -1;
    for (int i = 0; i < g_baseN; i++) {
        if (g_base[i].obj != obj || g_base[i].off != (unsigned)off) continue;
        if (BaseValid(g_base[i])) { found = i; break; }
        g_base[i] = g_base[--g_baseN];      // stale - a new object lives here
        i--;
    }
    if (found < 0 && g_baseN < PE_MAXBASE) {
        PeBase& b = g_base[g_baseN];
        b.obj = obj; b.off = (unsigned)off; b.vt = vt; b.own = own;
        b.size = (unsigned char)size;
        if (ReadBytes(obj + off, size, b.orig)) g_baseN++;
    }
    bool ok = WriteBytes(obj + off, size, val);
    Unlock();
    return ok;
}

static volatile LONG g_busy = 0;             // a worker apply is running
static volatile LONG g_restorePending = 0;    // a restore waits for it

static int RestoreAllLocked(char* msg, int msgLen) {
    Lock();
    int restored = 0, stale = 0;
    for (int i = 0; i < g_baseN; i++) {
        if (BaseValid(g_base[i]) &&
            WriteBytes(g_base[i].obj + g_base[i].off, g_base[i].size, g_base[i].orig))
            restored++;
        else stale++;
    }
    g_baseN = 0;
    Unlock();
    char t[1100];
    wsprintfA(t, "prefs: restored %d value(s) to shipped%s", restored,
              stale ? " (some objects were already rebuilt by a level load)" : "");
    if (msg) lstrcpynA(msg, t, msgLen);
    LogE(t);
    return restored;
}

// An apply in flight on the worker keeps writing - and adding baselines -
// after a restore that took only the table lock, so `prefs restore` (or
// `features prefsedit off`) during the ~1 s apply left values changed. The
// restore runs under the scan lock; if the worker holds it, the worker runs
// the restore itself the moment its apply ends.
int SWSE_PrefsEditRestoreAll(char* msg, int msgLen) {
    if (!TryEnterCriticalSection(&g_scanCs)) {
        InterlockedExchange(&g_restorePending, 1);
        if (g_busy || !TryEnterCriticalSection(&g_scanCs)) {
            if (msg) lstrcpynA(msg, "prefs: restore queued - a background apply is "
                                    "finishing, then everything is put back", msgLen);
            return 0;
        }
        InterlockedExchange(&g_restorePending, 0);
    }
    int r = RestoreAllLocked(msg, msgLen);
    LeaveCriticalSection(&g_scanCs);
    return r;
}

// ---- prefs.txt ----------------------------------------------------------------
struct PeEntry { char target[160]; char field[80]; char value[64]; char mod[48]; };
#define PE_MAXENTRY 256
static PeEntry g_ent[PE_MAXENTRY];
static int     g_entN = 0;
static bool    g_loaded = false;

static void LoadOneFile(const char* path, const char* modName, void*) {
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[400];
    while (fgets(line, sizeof(line), f) && g_entN < PE_MAXENTRY) {
        char* s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#' || *s == ';' || *s == '\r' || *s == '\n') continue;
        char* hash = strchr(s, '#'); if (hash) *hash = 0;
        PeEntry& e = g_ent[g_entN];
        char a[160] = { 0 }, b[80] = { 0 }, c[64] = { 0 };
        if (sscanf_s(s, "%159s %79s %63s", a, (unsigned)sizeof(a), b, (unsigned)sizeof(b),
                     c, (unsigned)sizeof(c)) != 3) continue;
        lstrcpynA(e.target, a, sizeof(e.target));
        lstrcpynA(e.field, b, sizeof(e.field));
        lstrcpynA(e.value, c, sizeof(e.value));
        lstrcpynA(e.mod, modName, sizeof(e.mod));
        g_entN++;
    }
    fclose(f);
}

int SWSE_PrefsEditLoad(char* msg, int msgLen) {
    Lock();
    g_entN = 0;
    SWSE_ForEachModFile("prefs.txt", LoadOneFile, nullptr);
    int n = g_entN, files = SWSE_CountModFile("prefs.txt");
    g_loaded = true;
    Unlock();
    char t[1100];
    wsprintfA(t, "prefs.txt: %d edit(s) from %d mod(s)", n, files);
    if (msg) lstrcpynA(msg, t, msgLen);
    LogE(t);
    return n;
}

// Apply every entry. Safe on a worker thread: each write is one aligned 4-byte
// (or 1-byte) store that x86 cannot tear, and the baseline table is locked.
static char g_lastApply[240] = "not applied yet";

static int ApplyEntriesLocked(bool verbose, void (*emit)(const char*));

// The static buffers below belong to whoever holds the scan lock; the worker
// and a console `prefs reload` can therefore never share them.
static int ApplyEntries(bool verbose, void (*emit)(const char*)) {
    EnterCriticalSection(&g_scanCs);
    int r = ApplyEntriesLocked(verbose, emit);
    LeaveCriticalSection(&g_scanCs);
    return r;
}

static int ApplyEntriesLocked(bool verbose, void (*emit)(const char*)) {
    static PeEntry ents[PE_MAXENTRY];
    Lock();
    int ne = g_entN;
    memcpy(ents, g_ent, sizeof(PeEntry) * ne);
    Unlock();
    if (!ne) { lstrcpynA(g_lastApply, "prefs.txt has no edits", sizeof(g_lastApply)); return 0; }

    static Target ts[PE_MAXENTRY];
    char err[240];
    for (int i = 0; i < ne; i++) {
        if (!ParseTarget(ents[i].target, &ts[i], err, sizeof(err))) {
            ts[i].kind = TGT_BAD;
            char t[1100];
            wsprintfA(t, "prefs: [%s] %s", ents[i].mod, err);
            LogE(t);
            if (verbose && emit) emit(t);
        }
    }
    static PeHit hits[PE_MAXHITS * 4];
    int nh = FindObjects(ts, ne, hits, PE_MAXHITS * 4);

    int writes = 0, missing = 0, bad = 0;
    for (int i = 0; i < ne; i++) {
        if (ts[i].kind == TGT_BAD) { bad++; continue; }
        int touched = 0;
        for (int k = 0; k < nh; k++) {
            if (!HitMatches(hits[k], ts[i])) continue;
            char cls[96] = { 0 };
            ObjName(hits[k].obj, cls, sizeof(cls));
            FieldRef f;
            if (!ResolveField(ents[i].field, cls, &f, err, sizeof(err))) {
                char t[1100];
                wsprintfA(t, "prefs: [%s] %s %s: %s", ents[i].mod, ents[i].target, ents[i].field, err);
                LogE(t);
                if (verbose && emit) emit(t);
                bad++;
                break;
            }
            if (f.ro) {
                char t[1100];
                _snprintf_s(t, sizeof(t), _TRUNCATE,
                            "prefs: [%s] %s is a struct or string field - not written "
                            "(name a type, e.g. %s:i, only if you know its layout)",
                            ents[i].mod, f.label, ents[i].field);
                LogE(t);
                if (verbose && emit) emit(t);
                bad++;
                break;
            }
            unsigned char v[4] = { 0 };
            if (!ParseValue(ents[i].value, f.type, v)) {
                char t[1100];
                wsprintfA(t, "prefs: [%s] bad value '%s' for %s", ents[i].mod, ents[i].value, f.label);
                LogE(t);
                if (verbose && emit) emit(t);
                bad++;
                break;
            }
            if (hits[k].isClass == 1 && f.type == 'f' && !PlausibleFloatAt(hits[k].obj + f.off))
                continue;                       // a vtable copy, not an object
            if (WriteWithBaseline(hits[k].obj, f.off, TypeSize(f.type), v)) { writes++; touched++; }
        }
        if (!touched && ts[i].kind != TGT_BAD) {
            missing++;
            char t[1100];
            wsprintfA(t, "prefs: [%s] %s is not loaded in this level", ents[i].mod, ents[i].target);
            LogE(t);
            if (verbose && emit) emit(t);
        }
    }
    wsprintfA(g_lastApply, "prefs.txt: %d edit(s) -> %d value(s) written, %d not loaded here, %d bad",
              ne, writes, missing, bad);
    LogE(g_lastApply);
    if (verbose && emit) emit(g_lastApply);
    return writes;
}

static unsigned      g_appliedEpoch = 0;

static DWORD WINAPI ApplyWorker(LPVOID) {
    ApplyEntries(false, nullptr);
    InterlockedExchange(&g_busy, 0);
    if (InterlockedExchange(&g_restorePending, 0)) {
        EnterCriticalSection(&g_scanCs);
        RestoreAllLocked(nullptr, 0);
        LeaveCriticalSection(&g_scanCs);
    }
    return 0;
}

void SWSE_PrefsEditKick() { g_appliedEpoch = 0; }

void SWSE_PrefsEditTick() {
    if (!g_loaded) SWSE_PrefsEditLoad(nullptr, 0);
    if (!SWSE_LevelUp() || g_busy) return;
    if (!SWSE_LevelDue(&g_appliedEpoch, 4500)) return;
    if (!g_entN) return;
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) return;
    HANDLE h = CreateThread(nullptr, 0, ApplyWorker, nullptr, 0, nullptr);
    if (h) CloseHandle(h); else InterlockedExchange(&g_busy, 0);
}

// ---- console ------------------------------------------------------------------
static void EmitF(void (*emit)(const char*), const char* fmt, ...) {
    char b[1100];
    va_list ap; va_start(ap, fmt);
    wvsprintfA(b, fmt, ap);
    va_end(ap);
    emit(b);
}

// Resolve a single console target to its objects.
static int ConsoleFind(const char* spec, PeHit* hits, int maxHits, void (*emit)(const char*)) {
    Target t; char err[240];
    if (!ParseTarget(spec, &t, err, sizeof(err))) { emit(err); return -1; }
    int n = FindObjects(&t, 1, hits, maxHits);
    int k = 0;
    for (int i = 0; i < n; i++) if (HitMatches(hits[i], t)) hits[k++] = hits[i];
    if (!k) {
        if (t.kind == TGT_HASH)
            EmitF(emit, "%08X: no live prefs object carries that hash (not loaded in this level?)", t.value);
        else if (t.kind == TGT_CLASS)
            EmitF(emit, "no live objects of class %s (check the name with `whatis`)", t.cls);
        else emit("no object");
    }
    return k;
}

static void ShowField(unsigned obj, const FieldRef& f, void (*emit)(const char*)) {
    unsigned char b[4] = { 0 };
    if (!ReadBytes(obj + f.off, TypeSize(f.type), b)) {
        EmitF(emit, "  %08X %-40s <unreadable>", obj, f.label);
        return;
    }
    char v[48]; FmtValue(v, f.type, b);
    unsigned raw; memcpy(&raw, b, 4);
    EmitF(emit, "  %08X +%03X %-36s = %s   (%08X)", obj, f.off, f.label, v,
          f.type == 'b' ? (unsigned)b[0] : raw);
}

int SWSE_PrefsSet(const char* target, const char* field, const char* value,
                  void (*emit)(const char*)) {
    static PeHit hits[PE_MAXHITS];
    int n = ConsoleFind(target, hits, PE_MAXHITS, emit);
    if (n < 0) return -1;                       // the target itself is bad
    if (n == 0) return 0;                       // not loaded in this level
    int done = 0;
    char err[240];
    for (int i = 0; i < n; i++) {
        char cls[96] = { 0 };
        ObjName(hits[i].obj, cls, sizeof(cls));
        FieldRef f;
        if (!ResolveField(field, cls, &f, err, sizeof(err))) { emit(err); return -1; }
        if (f.ro) { EmitF(emit, "%s is a struct or string field - not written as a number "
                                "(add :i/:f/:h only if you know its layout)", f.label); return -1; }
        unsigned char v[4] = { 0 };
        if (!ParseValue(value, f.type, v)) { EmitF(emit, "bad value '%s' for %s", value, f.label); return -1; }
        unsigned char before[4] = { 0 };
        ReadBytes(hits[i].obj + f.off, TypeSize(f.type), before);
        if (hits[i].isClass == 1 && f.type == 'f' && !PlausibleFloatAt(hits[i].obj + f.off))
            continue;                           // a vtable copy, not an object
        if (WriteWithBaseline(hits[i].obj, f.off, TypeSize(f.type), v)) {
            done++;
            if (done <= 8) {
                char a[48], b[48];
                FmtValue(a, f.type, before); FmtValue(b, f.type, v);
                EmitF(emit, "  %08X %s  %s: %s -> %s", hits[i].obj, cls, f.label, a, b);
            }
        }
    }
    if (done > 8) EmitF(emit, "  ...and %d more", done - 8);
    EmitF(emit, "set %s on %d object(s)  (`prefs restore` puts the shipped values back)",
          field, done);
    return done;
}

int SWSE_PrefsSetMany(const char* const* targets, int n, const char* field,
                      const char* value, void (*emit)(const char*)) {
    static Target ts[128];
    static PeHit hits[PE_MAXHITS * 4];
    if (n > 128) n = 128;
    char err[240];
    int nt = 0;
    for (int i = 0; i < n; i++) {
        if (ParseTarget(targets[i], &ts[nt], err, sizeof(err))) nt++;
        else if (emit) emit(err);
    }
    int nh = FindObjects(ts, nt, hits, PE_MAXHITS * 4);
    int done = 0, missing = 0;
    for (int t = 0; t < nt; t++) {
        int touched = 0;
        for (int k = 0; k < nh; k++) {
            if (!HitMatches(hits[k], ts[t])) continue;
            char cls[96] = { 0 };
            ObjName(hits[k].obj, cls, sizeof(cls));
            FieldRef f;
            if (!ResolveField(field, cls, &f, err, sizeof(err))) { if (emit) emit(err); return -1; }
            if (f.ro) {
                if (emit) EmitF(emit, "%s is a struct or string field - not written as a number", f.label);
                return -1;
            }
            unsigned char v[4] = { 0 };
            if (!ParseValue(value, f.type, v)) {
                if (emit) EmitF(emit, "bad value '%s' for %s", value, f.label);
                return -1;
            }
            if (hits[k].isClass == 1 && f.type == 'f' && !PlausibleFloatAt(hits[k].obj + f.off))
                continue;
            if (WriteWithBaseline(hits[k].obj, f.off, TypeSize(f.type), v)) { done++; touched++; }
        }
        if (!touched) missing++;
    }
    if (emit) EmitF(emit, "%s = %s on %d object(s); %d target(s) not loaded in this level",
                    field, value, done, missing);
    return done;
}

int SWSE_PrefsGetMany(const char* const* targets, int n, const char* field,
                      float* out, int* found) {
    static Target ts[128];
    static int    idx[128];
    static PeHit  hits[PE_MAXHITS * 4];
    if (n > 128) n = 128;
    char err[240];
    int nt = 0;
    for (int i = 0; i < n; i++) {
        found[i] = 0; out[i] = 0.0f;
        if (ParseTarget(targets[i], &ts[nt], err, sizeof(err))) idx[nt++] = i;
    }
    int nh = FindObjects(ts, nt, hits, PE_MAXHITS * 4);
    int got = 0;
    for (int t = 0; t < nt; t++) {
        for (int k = 0; k < nh; k++) {
            if (!HitMatches(hits[k], ts[t])) continue;
            char cls[96] = { 0 };
            ObjName(hits[k].obj, cls, sizeof(cls));
            FieldRef f;
            if (!ResolveField(field, cls, &f, err, sizeof(err))) break;
            float v = 0.0f;
            if (!ReadBytes(hits[k].obj + f.off, 4, (unsigned char*)&v)) break;
            out[idx[t]] = v; found[idx[t]] = 1; got++;
            break;
        }
    }
    return got;
}

// Something ParseValue could take for SOME field type: a number, hex, or a
// /data/ path (hashed). Anything else - `keep` itself, a typo - is refused,
// because the target may not be loaded to check against.
static bool LooksLikeValue(const char* v) {
    if (!v || !*v) return false;
    if (strchr(v, '/') || strchr(v, '\\')) return true;
    char t[64]; lstrcpynA(t, v, sizeof(t));
    for (char* c = t; *c; c++) if (*c == ',') *c = '.';
    char* e = nullptr;
    strtod(t, &e);
    if (e != t && TokenEnd(e)) return true;
    e = nullptr;
    strtoul(v, &e, 16);
    return e != v && TokenEnd(e);
}

bool SWSE_PrefsKeep(const char* target, const char* field, const char* value,
                    char* msg, int msgLen) {
    if (!LooksLikeValue(value)) {
        char t[300];
        _snprintf_s(t, sizeof(t), _TRUNCATE, "not kept - '%s' is not a value", value ? value : "");
        if (msg) lstrcpynA(msg, t, msgLen);
        return false;
    }
    char exe[MAX_PATH], path[MAX_PATH];
    GetModuleFileNameA(GetModuleHandleA(NULL), exe, MAX_PATH);
    char* sl = strrchr(exe, '\\'); if (sl) *sl = 0;      // ...\bin
    sl = strrchr(exe, '\\'); if (sl) *sl = 0;            // game root
    wsprintfA(path, "%s\\SWSEMods\\SWSE Console\\prefs.txt", exe);

    // Keep the file a list of settings, not a history: an earlier line for
    // the same target and field is replaced rather than stacked up.
    static char keepBuf[65536];
    int used = 0;
    FILE* in = fopen(path, "r");
    if (in) {
        char line[400];
        while (fgets(line, sizeof(line), in)) {
            char a[160] = { 0 }, b[80] = { 0 };
            if (line[0] != '#' &&
                sscanf_s(line, "%159s %79s", a, (unsigned)sizeof(a), b, (unsigned)sizeof(b)) == 2 &&
                !lstrcmpiA(a, target) && !lstrcmpiA(b, field))
                continue;
            int l = lstrlenA(line);
            if (used + l >= (int)sizeof(keepBuf) - 256) break;
            memcpy(keepBuf + used, line, l); used += l;
        }
        fclose(in);
    } else {
        used += wsprintfA(keepBuf,
            "# SWSE live prefs edits - applied on every level load (feature `prefsedit`).\n"
            "#   <target> <field> <value>   - see `prefs` in the console\n");
    }
    if (used && keepBuf[used - 1] != '\n') keepBuf[used++] = '\n';
    used += sprintf_s(keepBuf + used, sizeof(keepBuf) - used, "%-48s %-28s %s\n",
                      target, field, value);
    FILE* f = fopen(path, "w");
    if (!f) {
        char t[1100]; wsprintfA(t, "could not write %s", path);
        if (msg) lstrcpynA(msg, t, msgLen);
        return false;
    }
    fwrite(keepBuf, 1, used, f);
    fclose(f);
    SWSE_PrefsEditLoad(nullptr, 0);          // so status/reload see it at once
    if (msg) lstrcpynA(msg, SWSE_Feature(FEAT_PREFSEDIT)
                            ? "kept in SWSE Console\\prefs.txt - re-applied every level"
                            : "kept in SWSE Console\\prefs.txt - `features prefsedit on` "
                              "to re-apply it every level", msgLen);
    return true;
}

static void Usage(void (*emit)(const char*)) {
    emit("usage: prefs find <target>             - live objects for a path/hash/@Class");
    emit("       prefs get  <target> <field>");
    emit("       prefs set  <target> <field> <value>");
    emit("       prefs keep <target> <field> <value>  - set now AND every level (prefs.txt)");
    emit("       prefs dump <target> [filter]      - every known field with its value");
    emit("       prefs fields <filter>             - search the reflected field names");
    emit("       prefs restore | reload | status");
    emit("  target: /data/prefs/.../x.txt | 8-digit hash | @ClassName | &address");
    emit("  field : m_name | Class::m_name | 0x1A4   (suffix :i :b :h :f for the type)");
}

void SWSE_PrefsCommand(int argc, char** argv, void (*emit)(const char*)) {
    if (argc < 2) {
        if (!g_loaded) SWSE_PrefsEditLoad(nullptr, 0);
        EmitF(emit, "prefs editor: %d prefs.txt edit(s), %d value(s) changed and restorable",
              g_entN, g_baseN);
        emit(g_lastApply);
        Usage(emit);
        return;
    }
    const char* sub = argv[1];
    char msg[240];
    static PeHit hits[PE_MAXHITS];

    if (!lstrcmpiA(sub, "restore")) { SWSE_PrefsEditRestoreAll(msg, sizeof(msg)); emit(msg); return; }
    if (!lstrcmpiA(sub, "registry")) {
        // The resource registry's shape, and one lookup traced step by step.
        ModuleRange();
        unsigned mgr = 0, map = 0, begin = 0, end = 0, cnt = 0;
        __try {
            mgr = *(unsigned*)(g_modBase + RVA_RESMGR);
            if (mgr >= 0x10000) map = *(unsigned*)(mgr + 0x2C);
            if (map >= 0x10000) {
                begin = *(unsigned*)(map + 8); end = *(unsigned*)(map + 0x0C);
                cnt = *(unsigned*)(map + 0x14);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        unsigned vec = 0, vdata = 0, vcap = 0, vcnt = 0;
        __try {
            if (mgr >= 0x10000) vec = *(unsigned*)(mgr + 0x30);
            if (vec >= 0x10000) { vdata = *(unsigned*)vec; vcap = *(unsigned*)(vec + 4); vcnt = *(unsigned*)(vec + 8); }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        EmitF(emit, "mgr %08X  sorted vector %08X: data %08X, %d of %d  (live form)",
              mgr, vec, vdata, (int)vcnt, (int)vcap);
        EmitF(emit, "  hash map %08X  buckets %08X..%08X (%d)  count %d  state %d",
              map, begin, end, (end > begin) ? (int)((end - begin) / 4) : 0, (int)cnt, g_mgrOk);
        if (mgr >= 0x10000) {
            unsigned w[16] = { 0 };
            ReadBytes(mgr, 64, (unsigned char*)w);
            EmitF(emit, "  mgr+00: %08X %08X %08X %08X %08X %08X %08X %08X",
                  w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
            EmitF(emit, "  mgr+20: %08X %08X %08X %08X %08X %08X %08X %08X",
                  w[8], w[9], w[10], w[11], w[12], w[13], w[14], w[15]);
        }
        if (map >= 0x10000) {
            unsigned w[8] = { 0 };
            ReadBytes(map, 32, (unsigned char*)w);
            EmitF(emit, "  map+00: %08X %08X %08X %08X %08X %08X %08X %08X",
                  w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
        }
        if (argc > 2) {
            Target t; char err[240];
            if (!ParseTarget(argv[2], &t, err, sizeof(err)) || t.kind != TGT_HASH) { emit(err); return; }
            unsigned key = t.value;
            EmitF(emit, "lookup %08X -> %08X", key, SWSE_ResourceLookup(key));
            if (end > begin + 4) {
                unsigned nb = (end - begin) / 4, i = key % (nb - 1);
                unsigned node = 0, stop = 0;
                ReadBytes(begin + i * 4, 4, (unsigned char*)&node);
                ReadBytes(begin + (i + 1) * 4, 4, (unsigned char*)&stop);
                EmitF(emit, "  bucket %d: first %08X, next bucket's %08X", i, node, stop);
                for (int g = 0; g < 8 && node && node != stop; g++) {
                    unsigned nd[3] = { 0 };
                    if (!ReadBytes(node, 12, (unsigned char*)nd)) { emit("  (node unreadable)"); break; }
                    EmitF(emit, "  node %08X: next %08X key %08X res %08X", node, nd[0], nd[1], nd[2]);
                    node = nd[0];
                }
            }
        }
        return;
    }
    if (!lstrcmpiA(sub, "reload")) {
        SWSE_PrefsEditLoad(msg, sizeof(msg)); emit(msg);
        if (SWSE_LevelUp()) ApplyEntries(true, emit);
        else emit("no level up - applies when one loads");
        return;
    }
    if (!lstrcmpiA(sub, "status")) {
        if (!g_loaded) SWSE_PrefsEditLoad(nullptr, 0);
        EmitF(emit, "%d edit(s) loaded, %d restorable value(s)", g_entN, g_baseN);
        emit(g_lastApply);
        for (int i = 0; i < g_entN && i < 20; i++)
            EmitF(emit, "  [%s] %s %s %s", g_ent[i].mod, g_ent[i].target, g_ent[i].field, g_ent[i].value);
        return;
    }
    if (!lstrcmpiA(sub, "fields")) {
        const char* flt = (argc > 2) ? argv[2] : "";
        int shown = 0, total = 0;
        for (int i = 0; i < kReflectFieldCount; i++) {
            const ReflectField& r = kReflectFields[i];
            bool m = !*flt;
            if (!m) {
                // case-insensitive substring on class or field
                for (int w = 0; w < 2 && !m; w++) {
                    const char* hay = w ? r.field : r.cls;
                    for (const char* h = hay; *h && !m; h++) {
                        const char *a = h, *b = flt;
                        while (*a && *b && ((*a | 0x20) == (*b | 0x20))) { a++; b++; }
                        if (!*b) m = true;
                    }
                }
            }
            if (!m) continue;
            total++;
            if (shown < 40) { EmitF(emit, "  %-24s %-36s +0x%X", r.cls, r.field, r.off); shown++; }
        }
        EmitF(emit, "%d field(s)%s", total, total > shown ? " - narrow the filter to see all" : "");
        return;
    }
    if (argc < 3) { Usage(emit); return; }

    if (!lstrcmpiA(sub, "find")) {
        int n = ConsoleFind(argv[2], hits, PE_MAXHITS, emit);
        for (int i = 0; i < n && i < 24; i++) {
            char cls[96] = { 0 };
            ObjName(hits[i].obj, cls, sizeof(cls));
            EmitF(emit, "  %08X  %-28s own hash %08X", hits[i].obj, cls, OwnHash(hits[i].obj));
        }
        if (n > 24) EmitF(emit, "  ...%d in all", n);
        if (n > 0 && (g_lastViaRegistry || g_lastViaScan))
            EmitF(emit, "  (found via %s)", g_lastViaRegistry ? "the game's resource registry" : "a heap scan");
        return;
    }
    if (!lstrcmpiA(sub, "get") && argc >= 4) {
        int n = ConsoleFind(argv[2], hits, PE_MAXHITS, emit);
        char err[240];
        for (int i = 0; i < n && i < 16; i++) {
            char cls[96] = { 0 };
            ObjName(hits[i].obj, cls, sizeof(cls));
            FieldRef f;
            if (!ResolveField(argv[3], cls, &f, err, sizeof(err))) { emit(err); return; }
            ShowField(hits[i].obj, f, emit);
        }
        return;
    }
    if (!lstrcmpiA(sub, "set") && argc >= 5) {
        SWSE_PrefsSet(argv[2], argv[3], argv[4], emit);
        return;
    }
    if (!lstrcmpiA(sub, "keep") && argc >= 5) {
        // Kept only if the editor accepted the field and value (or the target
        // is simply not loaded here): a rejected line would be re-applied -
        // and rejected - at every level load.
        if (SWSE_PrefsSet(argv[2], argv[3], argv[4], emit) < 0) {
            emit("not kept - fix the field or value first");
            return;
        }
        SWSE_PrefsKeep(argv[2], argv[3], argv[4], msg, sizeof(msg));
        emit(msg);
        return;
    }
    if (!lstrcmpiA(sub, "dump")) {
        int n = ConsoleFind(argv[2], hits, PE_MAXHITS, emit);
        if (n <= 0) return;
        unsigned obj = hits[0].obj;
        char cls[96] = { 0 };
        ObjName(obj, cls, sizeof(cls));
        const char* flt = (argc > 3) ? argv[3] : "";
        EmitF(emit, "%08X  %s  own hash %08X%s", obj, cls, OwnHash(obj),
              n > 1 ? "  (first of several)" : "");
        int shown = 0;
        for (int i = 0; i < kReflectFieldCount; i++) {
            const ReflectField& r = kReflectFields[i];
            if (lstrcmpiA(r.cls, cls)) continue;
            if (*flt) {
                bool m = false;
                for (const char* h = r.field; *h && !m; h++) {
                    const char *a = h, *b = flt;
                    while (*a && *b && ((*a | 0x20) == (*b | 0x20))) { a++; b++; }
                    if (!*b) m = true;
                }
                if (!m) continue;
            }
            FieldRef f; f.off = r.off; f.type = TypeOfCode(r.type); f.ro = false;
            lstrcpynA(f.label, r.field, sizeof(f.label));
            if (shown < 60) ShowField(obj, f, emit);
            shown++;
        }
        if (!shown)
            EmitF(emit, "no reflected fields recorded for %s - `peek %08X 64` shows raw memory", cls, obj);
        else if (shown > 60)
            EmitF(emit, "...%d fields in all - add a filter: prefs dump %s <text>", shown, argv[2]);
        return;
    }
    Usage(emit);
}

// For the self-test: edits loaded from every prefs.txt, and values currently
// changed from shipped (restorable with `prefs restore`).
void SWSE_PrefsEditStats(int* edits, int* changed) {
    if (edits)   *edits   = g_entN;
    if (changed) *changed = g_baseN;
}
