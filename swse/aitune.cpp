// SWSE AI tuning - see aitune.h.

#include "aitune.h"
#include "modregistry.h"
#include "scriptvm.h"
#include "levelwatch.h"  // the level-is-up signal (no longer hit reactions')
#include "prefsedit.h"   // the resource registry, for the identity check
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
// 32: Stranger: Armed to the Teeth appends its [Racketeer] profile at the END,
// so a cap of 8 silently dropped it for anyone with five profiles of their own.
#define MAX_PROFILES 32
static Profile g_prof[MAX_PROFILES];
static int     g_profN = 0;

// misstime is given in standard milliseconds (10 = vanilla) and -1 means "not
// given, leave the shipped value". See ApplyToWeapon.
static void ProfileDefaults(Profile* p) {
    p->seedist = p->sixthsense = p->viewcone = p->instantsight = 1.0f;
    p->hidevolsee = p->relax = 1.0f;
    p->firerate = p->reload = p->accuracy = p->decisionrate = 1.0f;
    p->misstime = -1.0f;
}

// ---- baselines -------------------------------------------------------------
// The shipped values, captured the first time an object is touched. Everything
// is computed from these, so re-applying is idempotent and "off" is exact.
//
// A baseline is only as good as the claim "this address is still that
// object". 1.0.x kept bare addresses: restoring after a level change
// (`difficulty off` with the feature off) wrote 25 floats per object into
// whatever the heap had put there since, and the per-level tick threw the
// whole table away even while the SAME objects lived on - re-capturing
// already-tuned values as "shipped", so each re-apply compounded and `off`
// restored the tuned numbers.
//
// Both kinds of tuned object are, or live inside, a prefs RESOURCE that
// carries its path hash at +0x0C: a weapon is an NPCWeaponPrefs, and a
// perception block is the m_spAIPrefs embedded at +0x118 of an NPCPrefs (the
// only class that embeds one - research/REFLECT_FIELDS.tsv). A baseline keeps
// that resource's vtable and hash, and is used - written back or re-applied -
// only while both still match. When the game's resource registry answers, the
// hash must also map to this very object rather than to a newer copy.
#define AIP_IN_NPCPREFS 0x118
#define NPCPREFS_RVA    0x367E1C
#define RES_OWNHASH     0x00C

static unsigned ModBase() { return (unsigned)(uintptr_t)GetModuleHandleA(NULL); }

static bool ResourceStill(unsigned res, unsigned vt, unsigned hash) {
    __try {
        if (*(unsigned*)res != vt) return false;
        if (*(unsigned*)(res + RES_OWNHASH) != hash) return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    // Only a POSITIVE contrary answer counts. A miss says nothing (the vector
    // may be mid-reallocation), and dropping a live baseline is exactly what
    // brings the compounding back.
    unsigned live = SWSE_ResourceLookup(hash);
    if (live && live != res) return false;
    return true;
}

#define MAX_TUNED 256
struct Baseline {
    unsigned addr;              // the AIPrefs block (NPCPrefs + 0x118)
    unsigned vt, hash;          // the owning NPCPrefs' identity
    float    sight[SIGHT_STATES][8];
    float    relax;
    bool     used;
};
static Baseline g_base[MAX_TUNED];
static int      g_baseN = 0;    // high-water mark; `used` marks the live ones
static char     g_active[24] = "";
static unsigned g_activeEpoch = 0;      // the level epoch it was applied in
// Declared early: the file parser sets it, the tick reads it.
static char     g_wantProfile[24];

// Weapon baselines, kept separate: these objects are found by VTABLE (a strong
// identification) rather than by shape, and hold a different field set.
#define MAX_WEAPONS 128
#define NW_FIRERATE 0x17C
#define NW_RELOAD   0x184
#define NW_RELOADMX 0x188
#define NW_ACCURACY 0x1A8
#define NW_MISSTIME 0x1AC      // m_missTime, seconds (shipped 0.010 = 10 ms)
// Characters shipped with this much m_health are the game's "protected" cast -
// townsfolk, Clakkerz, natives, storekeepers. They take no damage because the
// engine skips damage at or above 10000.0 health (health core 0x46A670,
// constant 0x806F20). The 100000 value doubles as a reliable "this is not an
// enemy" test, which is how the tuning avoids buffing the wrong people.
#define PROTECTED_HP 100000.0f
struct WBase {
    unsigned addr;
    unsigned vt, hash;          // the NPCWeaponPrefs' own identity
    float fireRate, accuracy, reload, reloadMax, missTime;
    bool  used;
};
static WBase g_wbase[MAX_WEAPONS];
static int   g_wbaseN = 0;

static bool BaseValid(const Baseline* b) {
    return b->used && ResourceStill(b->addr - AIP_IN_NPCPREFS, b->vt, b->hash);
}
static bool WBaseValid(const WBase* b) {
    return b->used && ResourceStill(b->addr, b->vt, b->hash);
}

// Drop every entry whose object is gone, so a restore never writes into it
// and its slot can be reused.
static void PruneBaselines() {
    for (int i = 0; i < g_baseN; i++)
        if (g_base[i].used && !BaseValid(&g_base[i])) g_base[i].used = false;
    for (int i = 0; i < g_wbaseN; i++)
        if (g_wbase[i].used && !WBaseValid(&g_wbase[i])) g_wbase[i].used = false;
    while (g_baseN > 0 && !g_base[g_baseN - 1].used) g_baseN--;
    while (g_wbaseN > 0 && !g_wbase[g_wbaseN - 1].used) g_wbaseN--;
}

static Baseline* FindBaseline(unsigned addr) {
    for (int i = 0; i < g_baseN; i++) {
        if (!g_base[i].used || g_base[i].addr != addr) continue;
        if (BaseValid(&g_base[i])) return &g_base[i];
        g_base[i].used = false;         // a different object lives here now
    }
    return nullptr;
}

static Baseline* CaptureBaseline(unsigned addr) {
    Baseline* b = FindBaseline(addr);
    if (b) return b;
    // Only a block that really is an NPCPrefs' m_spAIPrefs: the perception
    // scan matches on shape, and this is where a look-alike is turned away.
    unsigned res = addr - AIP_IN_NPCPREFS, vt = 0, hash = 0;
    __try {
        vt = *(unsigned*)res;
        hash = *(unsigned*)(res + RES_OWNHASH);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    if (vt != ModBase() + NPCPREFS_RVA || !hash) return nullptr;
    // Only the record the registry knows: an unregistered copy would fail the
    // check at the next apply and be re-captured from its tuned values.
    if (!ResourceStill(res, vt, hash)) return nullptr;
    int slot = -1;
    for (int i = 0; i < g_baseN; i++) if (!g_base[i].used) { slot = i; break; }
    if (slot < 0) {
        if (g_baseN >= MAX_TUNED) PruneBaselines();
        if (g_baseN >= MAX_TUNED) return nullptr;
        slot = g_baseN;
    }
    b = &g_base[slot];
    __try {
        for (int s = 0; s < SIGHT_STATES; s++) {
            unsigned blk = addr + SIGHT_NORMAL + s * SIGHT_STRIDE;
            for (int f = 0; f < 8; f++) b->sight[s][f] = *(float*)(blk + f * 4);
        }
        b->relax = *(float*)(addr + AI_RELAX);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    b->addr = addr;
    b->vt = vt;
    b->hash = hash;
    b->used = true;
    if (slot == g_baseN) g_baseN++;
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
    for (int i = 0; i < g_wbaseN; i++) {
        if (!g_wbase[i].used || g_wbase[i].addr != addr) continue;
        if (WBaseValid(&g_wbase[i])) { b = &g_wbase[i]; break; }
        g_wbase[i].used = false;        // a different object lives here now
    }
    if (!b) {
        int slot = -1;
        for (int i = 0; i < g_wbaseN; i++) if (!g_wbase[i].used) { slot = i; break; }
        if (slot < 0) {
            if (g_wbaseN >= MAX_WEAPONS) PruneBaselines();
            if (g_wbaseN >= MAX_WEAPONS) return false;
            slot = g_wbaseN;
        }
        b = &g_wbase[slot];
        __try {
            b->vt        = *(unsigned*)addr;
            b->hash      = *(unsigned*)(addr + RES_OWNHASH);
            b->fireRate  = *(float*)(addr + NW_FIRERATE);
            b->accuracy  = *(float*)(addr + NW_ACCURACY);
            b->reload    = *(float*)(addr + NW_RELOAD);
            b->reloadMax = *(float*)(addr + NW_RELOADMX);
            b->missTime  = *(float*)(addr + NW_MISSTIME);
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        // As for perception blocks: only the registered record.
        if (!b->hash || !ResourceStill(addr, b->vt, b->hash)) return false;
        b->addr = addr; b->used = true;
        if (slot == g_wbaseN) g_wbaseN++;
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
    // m_missTime - the window in which a shooter deliberately misses (stored
    // in seconds; 0.010 on nearly every NPC weapon). `misstime` is given in
    // the game's STANDARD milliseconds: 10 is vanilla, 20 doubles every
    // weapon's window, 1000 makes enemies almost unable to land a shot. That is
    // the number Stranger: Armed to the Teeth writes (10 x its "Intentional
    // Miss" multiplier). Each weapon scales from its OWN shipped value, so the
    // odd weapon that ships at ~500 ms is not flattened to 10 at vanilla, and
    // one that ships 0 (no deliberate misses) keeps 0. Only firing weapons -
    // a melee entry has no shot to miss. -1 (not given) and "off" both land on
    // the stored shipped value.
    if (b->fireRate > 0.0f) {
        float mt = (p->misstime >= 0.0f) ? b->missTime * (p->misstime / 10.0f)
                                         : b->missTime;
        did |= WriteF(addr + NW_MISSTIME, mt);
    }
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
    // What the file asks for is read from scratch every time. A file that has
    // lost its `active` line, or is gone, asks for nothing - which is what
    // lets `auto` switch AI tuning back off. (This kept the last profile it
    // had seen, so `features aituning auto` still found one.) Built here and
    // stored at the end, so the worker never sees a half-parsed value.
    char want[24] = "";

    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        g_wantProfile[0] = 0;
        char t[MAX_PATH + 40];
        wsprintfA(t, "no aiprefs.txt (looked in %s)", path);
        lstrcpynA(msg, t, msgLen);
        return 0;
    }
    // 64 KB: the shipped file is ~7.6 KB and Stranger: Armed to the Teeth
    // appends its [Racketeer] profile at the END, so an 8 KB buffer (1.0.x)
    // would have silently cut off any profile added after it.
    static char buf[65536];
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
            // A comma is a decimal point: Stranger: Armed to the Teeth writes
            // these with the Windows number format, so on a French or German
            // system `reload = 0,667` arrives - and atof reads that as 0,
            // which is instant reloads and perfect aim.
            else     { if (vi < 31) val[vi++] = (*c == ',') ? '.' : *c; }
        }
        if (!key[0] || !val[0]) continue;

        // `active = <profile>` sits OUTSIDE any section and is what turns this
        // file into a mod: the named profile is applied automatically on every
        // level load, with no console command needed.
        if (!cur && !lstrcmpiA(key, "active")) {
            if (!lstrcmpiA(val, "off") || !lstrcmpiA(val, "none")) want[0] = 0;
            else lstrcpynA(want, val, 24);
            continue;
        }
        if (!cur) continue;
        float* f = FieldOf(cur, key);
        if (f) *f = (float)atof(val);
    }
    lstrcpynA(g_wantProfile, want, 24);
    char t[220];
    if (g_wantProfile[0])
        wsprintfA(t, "aiprefs.txt: %d profile(s), active = %s", g_profN, g_wantProfile);
    else
        wsprintfA(t, "aiprefs.txt: %d profile(s), active = off", g_profN);
    lstrcpynA(msg, t, msgLen);
    return g_profN;
}

const char* SWSE_AiTuneActive() { return g_active; }
unsigned SWSE_AiTuneActiveEpoch() { return g_activeEpoch; }
const char* SWSE_AiTuneWanted() { return g_wantProfile; }
int SWSE_AiTuneCount() {
    int n = 0;
    for (int i = 0; i < g_baseN; i++) if (g_base[i].used) n++;
    return n;
}

// ---- locking -----------------------------------------------------------------
// The `active` profile is applied on a worker thread (seconds of heap scan)
// while console commands run on the render thread, and both use the baseline
// tables, the row buffer and g_active. One lock covers all of it. The render
// thread must never wait seconds for it, so commands TRY the lock and answer
// "busy" - except `off`, which is queued and run by the worker as it finishes,
// because a switch-off that silently did nothing would leave the tuning on.
static CRITICAL_SECTION g_aiCs;
static struct AiLocks { AiLocks() { InitializeCriticalSection(&g_aiCs); } } g_aiLocks;
static volatile LONG g_applyBusy = 0;
static volatile LONG g_pendingOff = 0;

static int ApplyLocked(const char* profile, char* msg, int msgLen) {
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
            _snprintf_s(t, sizeof(t), _TRUNCATE, "no profile '%s' in aiprefs.txt", profile);
            lstrcpynA(msg, t, msgLen);
            return -1;
        }
    }

    // Objects that have gone since the last apply (a level change) are
    // dropped first, so nothing below writes into their memory.
    PruneBaselines();

    // Restoring only ever touches objects already tuned; applying goes looking
    // for new ones, because a level load builds fresh prefs objects. Objects
    // that survived the load keep the baseline they already have - they hold
    // tuned values now, and re-capturing those would compound.
    int n = 0, w = 0;
    if (off) {
        for (int i = 0; i < g_baseN; i++)
            if (g_base[i].used && ApplyToObject(g_base[i].addr, use)) n++;
        for (int i = 0; i < g_wbaseN; i++)
            if (g_wbase[i].used && ApplyToWeapon(g_wbase[i].addr, use)) w++;
        g_active[0] = 0;
        _snprintf_s(t, sizeof(t), _TRUNCATE,
                    "AI tuning OFF - %d perception + %d weapon object(s) restored", n, w);
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
            // ...and still not one after character tuning made it mortal:
            // the test is about who they ARE, which the shipped value says.
            if (SWSE_NpcWasProtected(rows[i].npcHash)) continue;
            if (!rows[i].weaponAddr) continue;
            if (ApplyToWeapon(rows[i].weaponAddr, use)) w++;
        }

        lstrcpynA(g_active, use->name, 24);
        g_activeEpoch = SWSE_LevelEpoch();
        _snprintf_s(t, sizeof(t), _TRUNCATE, "AI profile '%s': %d perception + %d weapon object(s)",
                    use->name, n, w);
    }
    lstrcpynA(msg, t, msgLen);
    LogA(t);
    return n;
}

int SWSE_AiTuneApply(const char* profile, char* msg, int msgLen) {
    bool off = (!profile || !*profile || !lstrcmpiA(profile, "off"));
    if (!TryEnterCriticalSection(&g_aiCs)) {
        if (!off) {
            lstrcpynA(msg, "busy - the active AI profile is being applied in the "
                           "background; try again in a few seconds", msgLen);
            return -2;
        }
        InterlockedExchange(&g_pendingOff, 1);
        // The worker may have finished between the failed try and the flag,
        // in which case nobody else will run it.
        if (g_applyBusy || !TryEnterCriticalSection(&g_aiCs)) {
            lstrcpynA(msg, "AI tuning off - a background apply is finishing; "
                           "everything it tuned is restored the moment it ends", msgLen);
            return 0;
        }
        InterlockedExchange(&g_pendingOff, 0);
    }
    int r = ApplyLocked(profile, msg, msgLen);
    LeaveCriticalSection(&g_aiCs);
    return r;
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
static DWORD WINAPI ApplyWorker(LPVOID) {
    char msg[220];
    EnterCriticalSection(&g_aiCs);
    ApplyLocked(g_wantProfile, msg, sizeof(msg));
    LeaveCriticalSection(&g_aiCs);
    InterlockedExchange(&g_applyBusy, 0);
    // An `off` that arrived while this ran could not take the lock.
    if (InterlockedExchange(&g_pendingOff, 0)) {
        EnterCriticalSection(&g_aiCs);
        ApplyLocked("off", msg, sizeof(msg));
        LeaveCriticalSection(&g_aiCs);
    }
    return 0;
}

// The epoch this was last applied in. The level watcher's epoch is the
// level-is-up signal; 1.0.x used the hit-reaction actor list instead, which
// never populates with hit reactions off and so silently disabled this.
static unsigned g_tunedEpoch = 0;

void SWSE_AiTuneKick() { g_tunedEpoch = 0; }

void SWSE_AiTuneTick() {
    if (!SWSE_LevelUp()) { g_active[0] = 0; return; }   // level gone; tuning lapsed
    if (!g_wantProfile[0]) return;
    if (g_applyBusy) return;                 // retry once the worker finishes
    // The delay lets the cast finish building before anything is scanned.
    if (!SWSE_LevelDue(&g_tunedEpoch, 4000)) return;
    if (InterlockedCompareExchange(&g_applyBusy, 1, 0) != 0) return;
    // No table reset here: the apply prunes what is gone and keeps the
    // baselines of objects that outlived the load (see PruneBaselines).
    HANDLE h = CreateThread(nullptr, 0, ApplyWorker, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
    else InterlockedExchange(&g_applyBusy, 0);
}

void SWSE_AiTuneRefresh() {
    if (!g_active[0]) return;
    char msg[220];
    char want[24];
    lstrcpynA(want, g_active, 24);
    SWSE_AiTuneApply(want, msg, sizeof(msg));
}
