// SWSE AI tuning - see aitune.h.

#include "aitune.h"
#include "modregistry.h"
#include "scriptvm.h"
#include "granny.h"      // SWSE_WatchActors - the level-is-up signal
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void LogA(const char* s) {
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

// ---- the perception object layout (confirmed live) ------------------------
// Four sight blocks at a 0xA4 stride; each block is 8 floats.
#define SIGHT_NORMAL 0x004
#define SIGHT_STRIDE 0x0A4
#define SIGHT_STATES 4
#define S_6THSENSE   0x00
#define S_SEEDIST    0x04
#define S_SEEABOVE   0x08
#define S_SEEBELOW   0x0C
#define S_HANGLE     0x10
#define S_VANGLE     0x14
#define S_INSTANT    0x18
#define S_HIDEVOL    0x1C
#define AI_RELAX     0x2A0

// ---- profile ---------------------------------------------------------------
struct Profile {
    char  name[24];
    float seedist, sixthsense, viewcone, instantsight, hidevolsee, relax;
    float firerate, reload, accuracy, misstime, decisionrate;
};
#define MAX_PROFILES 8
static Profile g_prof[MAX_PROFILES];
static int     g_profN = 0;

static void ProfileDefaults(Profile* p) {
    p->seedist = p->sixthsense = p->viewcone = p->instantsight = 1.0f;
    p->hidevolsee = p->relax = 1.0f;
    p->firerate = p->reload = p->accuracy = p->misstime = p->decisionrate = 1.0f;
}

// ---- baselines -------------------------------------------------------------
// The shipped values, captured the first time an object is touched. Everything
// is computed from these, so re-applying is idempotent and "off" is exact.
#define MAX_TUNED 64
struct Baseline {
    unsigned addr;
    float    sight[SIGHT_STATES][8];
    float    relax;
    bool     used;
};
static Baseline g_base[MAX_TUNED];
static int      g_baseN = 0;
static char     g_active[24] = "";
// Declared early: the file parser sets it, the tick reads it.
static char     g_wantProfile[24];

// Weapon baselines, kept separate: these objects are found by VTABLE (a strong
// identification) rather than by shape, and hold a different field set.
#define MAX_WEAPONS 64
#define NW_FIRERATE 0x17C
#define NW_RELOAD   0x184
#define NW_RELOADMX 0x188
#define NW_ACCURACY 0x1A8
// Characters with this much health are the engine's "protected" cast -
// townsfolk, Clakkerz, natives, storekeepers. They are not flagged
// invulnerable, they simply have an enormous health value (see
// research/NPC_TUNING.md). It doubles as a reliable "this is not an enemy"
// test, which is how the tuning avoids buffing the wrong people.
#define PROTECTED_HP 100000.0f
struct WBase {
    unsigned addr;
    float fireRate, accuracy, reload, reloadMax;
    bool  used;
};
static WBase g_wbase[MAX_WEAPONS];
static int   g_wbaseN = 0;

static Baseline* FindBaseline(unsigned addr) {
    for (int i = 0; i < g_baseN; i++)
        if (g_base[i].used && g_base[i].addr == addr) return &g_base[i];
    return nullptr;
}

static Baseline* CaptureBaseline(unsigned addr) {
    Baseline* b = FindBaseline(addr);
    if (b) return b;
    if (g_baseN >= MAX_TUNED) return nullptr;
    b = &g_base[g_baseN];
    __try {
        for (int s = 0; s < SIGHT_STATES; s++) {
            unsigned blk = addr + SIGHT_NORMAL + s * SIGHT_STRIDE;
            for (int f = 0; f < 8; f++) b->sight[s][f] = *(float*)(blk + f * 4);
        }
        b->relax = *(float*)(addr + AI_RELAX);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    b->addr = addr;
    b->used = true;
    g_baseN++;
    return b;
}

static bool WriteF(unsigned addr, float v) {
    __try {
        DWORD old;
        if (!VirtualProtect((void*)addr, 4, PAGE_READWRITE, &old)) return false;
        *(float*)addr = v;
        VirtualProtect((void*)addr, 4, old, &old);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Apply one profile (or the identity, to restore) to one object.
static bool ApplyToObject(unsigned addr, const Profile* p) {
    Baseline* b = CaptureBaseline(addr);
    if (!b) return false;
    for (int s = 0; s < SIGHT_STATES; s++) {
        unsigned blk = addr + SIGHT_NORMAL + s * SIGHT_STRIDE;
        WriteF(blk + S_6THSENSE, b->sight[s][0] * p->sixthsense);
        WriteF(blk + S_SEEDIST,  b->sight[s][1] * p->seedist);
        // seeAbove/seeBelow are left alone: they ship at 1001, effectively
        // "unlimited", and scaling an unlimited value means nothing.
        // The view cone is clamped - an NPC with a >360 degree cone is not
        // "more alert", it is a broken value that could read as always-seeing.
        float ha = b->sight[s][4] * p->viewcone;
        float va = b->sight[s][5] * p->viewcone;
        if (ha > 360.0f) ha = 360.0f;
        if (va > 360.0f) va = 360.0f;
        WriteF(blk + S_HANGLE, ha);
        WriteF(blk + S_VANGLE, va);
        WriteF(blk + S_INSTANT, b->sight[s][6] * p->instantsight);
        WriteF(blk + S_HIDEVOL, b->sight[s][7] * p->hidevolsee);
    }
    WriteF(addr + AI_RELAX, b->relax * p->relax);
    return true;
}

// `firerate` scales m_fireRate, which is a RATE IN SHOTS PER SECOND, not a
// delay - so HIGHER = FASTER. This was documented backwards at first and the
// error was only caught in play ("they feel like they fire slower"), because
// the numbers were self-consistent and wrong. The shipped values settle it:
// outlaw semiauto is 10.0 and the sniper is 0.4, which only makes sense as
// shots/second. Read as a delay the semiauto would fire once every 10 seconds.
//
// A shipped value of exactly 0 is left alone: those entries are melee or
// non-firing, and scaling 0 is meaningless rather than harmless-looking.
//
// `accuracy` scales m_accuracyWidth, which is a SPREAD: the observed values are
// 0.05 .. 1.0, and a wider cone is a worse shot. So a multiplier BELOW 1.0
// makes NPCs more accurate. The file says this plainly, because getting it
// backwards would make an intended difficulty increase into a decrease and
// look like the feature simply not working.
static bool ApplyToWeapon(unsigned addr, const Profile* p) {
    WBase* b = nullptr;
    for (int i = 0; i < g_wbaseN; i++)
        if (g_wbase[i].used && g_wbase[i].addr == addr) { b = &g_wbase[i]; break; }
    if (!b) {
        if (g_wbaseN >= MAX_WEAPONS) return false;
        b = &g_wbase[g_wbaseN];
        __try {
            b->fireRate  = *(float*)(addr + NW_FIRERATE);
            b->accuracy  = *(float*)(addr + NW_ACCURACY);
            b->reload    = *(float*)(addr + NW_RELOAD);
            b->reloadMax = *(float*)(addr + NW_RELOADMX);
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        b->addr = addr; b->used = true; g_wbaseN++;
    }
    bool did = false;
    // A shipped 0 means the field is unused for this weapon; scaling it would
    // write 0 and look like it worked.
    if (b->fireRate > 0.0f)
        did |= WriteF(addr + NW_FIRERATE, b->fireRate * p->firerate);
    if (b->accuracy > 0.0f)
        did |= WriteF(addr + NW_ACCURACY, b->accuracy * p->accuracy);
    // Reload is a TIME in seconds, so lower = quicker. Both the base and the
    // max are scaled together: they are a range, and moving only one of them
    // would invert it (max < base) on any weapon where they are close.
    if (b->reload > 0.0f)
        did |= WriteF(addr + NW_RELOAD, b->reload * p->reload);
    if (b->reloadMax > 0.0f)
        did |= WriteF(addr + NW_RELOADMX, b->reloadMax * p->reload);
    return did;
}

// ---- file ------------------------------------------------------------------
static void TunePath(char* out, int n) {
    char exe[MAX_PATH];
    GetModuleFileNameA(GetModuleHandleA(NULL), exe, MAX_PATH);
    char* sl = strrchr(exe, '\\'); if (sl) *sl = 0;      // ...\bin
    sl = strrchr(exe, '\\'); if (sl) *sl = 0;            // game root
    // Profiles may come from any mod folder, not just the shipped one.
    if (SWSE_FindModFile("aiprefs.txt", out, MAX_PATH)) return;
    wsprintfA(out, "%s\\SWSEMods\\SWSE Console\\aiprefs.txt", exe);
    (void)n;
}

static float* FieldOf(Profile* p, const char* key) {
    if (!lstrcmpiA(key, "seedist"))      return &p->seedist;
    if (!lstrcmpiA(key, "sixthsense"))   return &p->sixthsense;
    if (!lstrcmpiA(key, "viewcone"))     return &p->viewcone;
    if (!lstrcmpiA(key, "instantsight")) return &p->instantsight;
    if (!lstrcmpiA(key, "hidevolsee"))   return &p->hidevolsee;
    if (!lstrcmpiA(key, "relax"))        return &p->relax;
    if (!lstrcmpiA(key, "firerate"))     return &p->firerate;
    if (!lstrcmpiA(key, "reload"))       return &p->reload;
    if (!lstrcmpiA(key, "accuracy"))     return &p->accuracy;
    if (!lstrcmpiA(key, "misstime"))     return &p->misstime;
    if (!lstrcmpiA(key, "decisionrate")) return &p->decisionrate;
    return nullptr;
}

int SWSE_AiTuneLoad(char* msg, int msgLen) {
    char path[MAX_PATH];
    TunePath(path, MAX_PATH);
    g_profN = 0;

    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        char t[220];
        wsprintfA(t, "no aiprefs.txt (looked in %s)", path);
        lstrcpynA(msg, t, msgLen);
        return 0;
    }
    static char buf[8192];
    DWORD got = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &got, NULL);
    CloseHandle(h);
    buf[got] = 0;

    Profile* cur = nullptr;
    char* p = buf;
    while (*p) {
        char* line = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = 0;
        while (*line == ' ' || *line == '\t') line++;
        if (!*line || *line == '#' || *line == ';' || *line == '\r') continue;

        if (*line == '[') {                       // [profilename]
            if (g_profN >= MAX_PROFILES) continue;
            cur = &g_prof[g_profN++];
            ProfileDefaults(cur);
            int i = 0;
            for (char* c = line + 1; *c && *c != ']' && i < 23; c++) cur->name[i++] = *c;
            cur->name[i] = 0;
            continue;
        }

        char key[32] = {0}, val[32] = {0};
        int ki = 0, vi = 0; bool eq = false;
        for (char* c = line; *c; c++) {
            if (*c == '#' || *c == ';' || *c == '\r') break;
            if (*c == '=') { eq = true; continue; }
            if (*c == ' ' || *c == '\t') continue;
            if (!eq) { if (ki < 31) key[ki++] = *c; }
            else     { if (vi < 31) val[vi++] = *c; }
        }
        if (!key[0] || !val[0]) continue;

        // `active = <profile>` sits OUTSIDE any section and is what turns this
        // file into a mod: the named profile is applied automatically on every
        // level load, with no console command needed.
        if (!cur && !lstrcmpiA(key, "active")) {
            if (!lstrcmpiA(val, "off") || !lstrcmpiA(val, "none")) g_wantProfile[0] = 0;
            else lstrcpynA(g_wantProfile, val, 24);
            continue;
        }
        if (!cur) continue;
        float* f = FieldOf(cur, key);
        if (f) *f = (float)atof(val);
    }
    char t[220];
    if (g_wantProfile[0])
        wsprintfA(t, "aiprefs.txt: %d profile(s), active = %s", g_profN, g_wantProfile);
    else
        wsprintfA(t, "aiprefs.txt: %d profile(s), active = off", g_profN);
    lstrcpynA(msg, t, msgLen);
    return g_profN;
}

const char* SWSE_AiTuneActive() { return g_active; }
int SWSE_AiTuneCount() { return g_baseN; }

int SWSE_AiTuneApply(const char* profile, char* msg, int msgLen) {
    char t[220];
    bool off = (!profile || !*profile || !lstrcmpiA(profile, "off"));

    Profile ident;
    const Profile* use = nullptr;
    if (off) {
        ProfileDefaults(&ident);
        lstrcpynA(ident.name, "off", 24);
        use = &ident;
    } else {
        for (int i = 0; i < g_profN; i++)
            if (!lstrcmpiA(g_prof[i].name, profile)) { use = &g_prof[i]; break; }
        if (!use) {
            wsprintfA(t, "no profile '%s' in aiprefs.txt", profile);
            lstrcpynA(msg, t, msgLen);
            return -1;
        }
    }

    // Restoring only ever touches objects already tuned; applying goes looking
    // for new ones, because a level load builds fresh prefs objects.
    int n = 0, w = 0;
    if (off) {
        for (int i = 0; i < g_baseN; i++)
            if (g_base[i].used && ApplyToObject(g_base[i].addr, use)) n++;
        for (int i = 0; i < g_wbaseN; i++)
            if (g_wbase[i].used && ApplyToWeapon(g_wbase[i].addr, use)) w++;
        g_active[0] = 0;
        wsprintfA(t, "AI tuning OFF - %d perception + %d weapon object(s) restored", n, w);
    } else {
        unsigned hits[MAX_TUNED];
        int found = SWSE_FindAiPrefs(hits, MAX_TUNED, 1500.0);
        for (int i = 0; i < found; i++)
            if (ApplyToObject(hits[i], use)) n++;

        // Weapons are reached THROUGH their owning character, not by scanning
        // for weapon objects directly. That is what makes the tuning
        // selective: the join gives each gun's owner, so the protected cast
        // (townsfolk, Clakkerz, natives - all at 100000 health) can be skipped
        // and only actual enemies are buffed. Scanning weapons alone has no
        // way to tell whose gun it is.
        static NpcGunRow rows[MAX_WEAPONS];
        int rn = SWSE_NpcGuns(rows, MAX_WEAPONS, 4000.0);
        for (int i = 0; i < rn; i++) {
            if (rows[i].health >= PROTECTED_HP) continue;    // not an enemy
            if (!rows[i].weaponAddr) continue;
            if (ApplyToWeapon(rows[i].weaponAddr, use)) w++;
        }

        lstrcpynA(g_active, use->name, 24);
        wsprintfA(t, "AI profile '%s': %d perception + %d weapon object(s)",
                  use->name, n, w);
    }
    lstrcpynA(msg, t, msgLen);
    LogA(t);
    return n;
}

// ---- automatic application ------------------------------------------------
//
// `active = <profile>` in aiprefs.txt makes the tuning a MOD rather than a
// command you have to remember to type. Without it the profile silently lapses
// on every level change, because a level load builds fresh prefs objects and
// the old addresses become meaningless.
//
// Applying costs a heap scan of several seconds, which must never happen on
// the render thread - that is a visible freeze. It runs on a worker instead.
// This is safe here specifically because every write is a single aligned
// 4-byte float: x86 cannot tear those, so the worst a racing reader sees is
// the old value or the new one, never a mixture.
static volatile LONG g_applyBusy = 0;

static DWORD WINAPI ApplyWorker(LPVOID) {
    char msg[220];
    SWSE_AiTuneApply(g_wantProfile, msg, sizeof(msg));
    LogA(msg);
    InterlockedExchange(&g_applyBusy, 0);
    return 0;
}

void SWSE_AiTuneTick() {
    if (!g_wantProfile[0]) return;

    static int   lastHadActors = 0;
    static DWORD dueAt = 0;

    unsigned act[4];
    int have = SWSE_WatchActors(act, 4) > 0 ? 1 : 0;

    // Same edge the self-test uses: actors appearing means a level is up.
    // The delay lets the cast finish building before anything is scanned.
    if (have && !lastHadActors) dueAt = GetTickCount() + 4000;
    if (!have) { dueAt = 0; g_active[0] = 0; }   // level gone; tuning lapsed
    lastHadActors = have;

    if (!dueAt || GetTickCount() < dueAt) return;
    dueAt = 0;
    if (InterlockedCompareExchange(&g_applyBusy, 1, 0) != 0) return;

    // A new level means every captured address is stale.
    g_baseN = 0;
    for (int i = 0; i < MAX_TUNED; i++) g_base[i].used = false;
    g_wbaseN = 0;
    for (int i = 0; i < MAX_WEAPONS; i++) g_wbase[i].used = false;

    HANDLE h = CreateThread(nullptr, 0, ApplyWorker, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
    else InterlockedExchange(&g_applyBusy, 0);
}

void SWSE_AiTuneRefresh() {
    if (!g_active[0]) return;
    char msg[220];
    char want[24];
    lstrcpynA(want, g_active, 24);
    // A level change invalidates every captured address.
    g_baseN = 0;
    for (int i = 0; i < MAX_TUNED; i++) g_base[i].used = false;
    g_wbaseN = 0;
    for (int i = 0; i < MAX_WEAPONS; i++) g_wbase[i].used = false;
    SWSE_AiTuneApply(want, msg, sizeof(msg));
}
