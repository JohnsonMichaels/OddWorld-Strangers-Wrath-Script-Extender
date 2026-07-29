// SWSE triggers - see triggers.h.

#include "triggers.h"
#include "positions.h"
#include "modregistry.h"
#include "console.h"
#include "scriptvm.h"
#include "granny.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TRIGGERS 128
#define MAX_ACTIONS  8

enum WhenKind { W_NONE = 0, W_ENTER, W_LEAVE, W_LEVELLOAD, W_EVERY, W_KILLED, W_CLEARED };

struct Trigger {
    char     name[48];
    int      kind;
    char     label[48];          // for enter/leave, if given by name
    float    x, y, z, radius;
    bool     havePoint;
    float    seconds;            // for `every`
    unsigned typeHash;           // for killed/cleared
    int      needCount;
    char     level[40];          // "" = any level
    int      chance;             // percent, 100 = always
    float    cooldown;           // seconds
    int      onceMode;           // 0 none, 1 per level, 2 ever
    char     actions[MAX_ACTIONS][160];
    int      actionN;

    // runtime
    bool     inside;             // last proximity state, for edge detection
    bool     primed;             // has the condition been false at least once
    DWORD    lastFire;
    int      fired;
    int      firedThisLevel;
    int      peakAlive;      // highest live count seen, for `killed`
    char     whenText[96];
};

static Trigger g_trg[MAX_TRIGGERS];
static int     g_trgN = 0;
static bool    g_on = true;
static DWORD   g_levelStamp = 0;

static void LogT(const char* s) {
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

void SWSE_TriggersEnable(int on) { g_on = (on != 0); }
int  SWSE_TriggersEnabled()      { return g_on ? 1 : 0; }

// ---- parsing ---------------------------------------------------------------
static void Trim(char* s) {
    int n = lstrlenA(s);
    while (n > 0 && (s[n-1] == '\r' || s[n-1] == '\n' || s[n-1] == ' ' || s[n-1] == '\t'))
        s[--n] = 0;
}

// "enter enemyambush1 radius 18"  |  "enter 1250 40 -300 radius 18"
static void ParseWhen(Trigger* t, const char* v) {
    lstrcpynA(t->whenText, v, sizeof(t->whenText));
    char word[64] = {0};
    int n = 0;
    const char* p = v;
    while (*p == ' ') p++;
    while (*p && *p != ' ' && n < 63) word[n++] = *p++;
    word[n] = 0;

    if (!lstrcmpiA(word, "enter") || !lstrcmpiA(word, "leave")) {
        t->kind = !lstrcmpiA(word, "enter") ? W_ENTER : W_LEAVE;
        t->radius = 12.0f;
        // Either three numbers, or a label. A label is resolved at fire time
        // rather than here, because positions.txt may load after triggers.txt.
        float a, b, c;
        if (sscanf(p, " %f %f %f", &a, &b, &c) == 3) {
            t->x = a; t->y = b; t->z = c; t->havePoint = true;
        } else {
            char lbl[48] = {0};
            if (sscanf(p, " %47s", lbl) == 1) lstrcpynA(t->label, lbl, sizeof(t->label));
        }
        const char* r = strstr(p, "radius");
        if (!r) r = strstr(p, "RADIUS");
        if (r) t->radius = (float)atof(r + 6);
        return;
    }
    if (!lstrcmpiA(word, "levelload")) { t->kind = W_LEVELLOAD; return; }
    if (!lstrcmpiA(word, "every"))     { t->kind = W_EVERY; t->seconds = (float)atof(p); return; }
    if (!lstrcmpiA(word, "killed") || !lstrcmpiA(word, "cleared")) {
        t->kind = !lstrcmpiA(word, "killed") ? W_KILLED : W_CLEARED;
        char hash[24] = {0};
        if (sscanf(p, " %23s", hash) == 1) t->typeHash = (unsigned)strtoul(hash, nullptr, 16);
        const char* c = strstr(p, "count");
        t->needCount = c ? atoi(c + 5) : 1;
        return;
    }
}

static void LoadOneFile(const char* path, const char* modName, void*) {
    FILE* f = fopen(path, "r");
    if (!f) return;
    int before = g_trgN;
    Trigger* cur = nullptr;
    char line[400];
    while (fgets(line, sizeof(line), f)) {
        char* s = line;
        while (*s == ' ' || *s == '\t') s++;
        Trim(s);
        if (!*s || *s == '#' || *s == ';') continue;

        if (*s == '[') {
            if (g_trgN >= MAX_TRIGGERS) { cur = nullptr; continue; }
            cur = &g_trg[g_trgN++];
            memset(cur, 0, sizeof(*cur));
            cur->chance = 100;
            cur->radius = 12.0f;
            cur->needCount = 1;
            int i = 0;
            for (char* c = s + 1; *c && *c != ']' && i < 47; c++) cur->name[i++] = *c;
            cur->name[i] = 0;
            continue;
        }
        if (!cur) continue;

        char* eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char* key = s;
        char* val = eq + 1;
        Trim(key);
        while (*val == ' ' || *val == '\t') val++;
        // strip a trailing comment
        for (char* c = val; *c; c++) if (*c == '#' || *c == ';') { *c = 0; break; }
        Trim(val);
        // trailing spaces on the key
        int kn = lstrlenA(key);
        while (kn > 0 && (key[kn-1] == ' ' || key[kn-1] == '\t')) key[--kn] = 0;

        if      (!lstrcmpiA(key, "when"))     ParseWhen(cur, val);
        else if (!lstrcmpiA(key, "chance"))   cur->chance = atoi(val);
        else if (!lstrcmpiA(key, "cooldown")) cur->cooldown = (float)atof(val);
        else if (!lstrcmpiA(key, "level"))    lstrcpynA(cur->level, val, sizeof(cur->level));
        else if (!lstrcmpiA(key, "radius"))   cur->radius = (float)atof(val);
        else if (!lstrcmpiA(key, "once")) {
            cur->onceMode = !lstrcmpiA(val, "ever") ? 2 : 1;
        }
        else if (!lstrcmpiA(key, "do")) {
            if (cur->actionN < MAX_ACTIONS)
                lstrcpynA(cur->actions[cur->actionN++], val, 160);
        }
    }
    fclose(f);
    char b[220];
    wsprintfA(b, "triggers: +%d from [%s]", g_trgN - before, modName);
    LogT(b);
}

int SWSE_TriggersLoad() {
    g_trgN = 0;
    SWSE_ForEachModFile("triggers.txt", LoadOneFile, nullptr);
    char b[160];
    wsprintfA(b, "triggers: %d loaded from %d mod(s)",
              g_trgN, SWSE_CountModFile("triggers.txt"));
    LogT(b);
    return g_trgN;
}

// ---- firing ----------------------------------------------------------------
static void RunActions(Trigger* t) {
    char b[300];
    for (int i = 0; i < t->actionN; i++) {
        wsprintfA(b, "trigger [%s] -> %s", t->name, t->actions[i]);
        LogT(b);
        SWSE_ConsoleExec(t->actions[i]);
    }
    t->fired++;
    t->firedThisLevel++;
    t->lastFire = GetTickCount();
}

static bool LevelMatches(const Trigger* t) {
    if (!t->level[0]) return true;
    const char* now = SWSE_CurrentLevel();
    return now[0] && !lstrcmpiA(now, t->level);
}

// Resolve the point: an explicit x/y/z, or a label looked up now (positions
// may load after triggers, and a label can be redefined at runtime).
static bool PointOf(const Trigger* t, float* out3) {
    if (t->havePoint) { out3[0] = t->x; out3[1] = t->y; out3[2] = t->z; return true; }
    if (!t->label[0]) return false;
    return SWSE_PositionGet(t->label, out3, nullptr);
}

int SWSE_TriggerTest(const char* name, char* msg, int msgLen) {
    for (int i = 0; i < g_trgN; i++) {
        if (lstrcmpiA(g_trg[i].name, name)) continue;
        RunActions(&g_trg[i]);
        char b[200];
        wsprintfA(b, "fired [%s] (%d action(s))", g_trg[i].name, g_trg[i].actionN);
        lstrcpynA(msg, b, msgLen);
        return 1;
    }
    lstrcpynA(msg, "no trigger by that name", msgLen);
    return 0;
}

void SWSE_TriggersTick() {
    if (!g_on || g_trgN == 0) return;
    // Seed once. Without this every session rolls the same sequence, so a 40%
    // ambush would fire or not fire identically on every playthrough.
    static bool seeded = false;
    if (!seeded) { seeded = true; srand(GetTickCount()); }

    // Level-change detection, using the same actor-presence edge the self-test
    // and AI tuning use. Cheap and already computed each frame.
    static int lastHadActors = 0;
    unsigned act[4];
    int have = SWSE_WatchActors(act, 4) > 0 ? 1 : 0;
    bool levelJustLoaded = (have && !lastHadActors);
    if (levelJustLoaded) {
        g_levelStamp = GetTickCount();
        for (int i = 0; i < g_trgN; i++) {
            g_trg[i].firedThisLevel = 0;
            g_trg[i].peakAlive = 0;
            g_trg[i].inside = false;
            g_trg[i].primed = false;
        }
    }
    if (!have) { lastHadActors = 0; return; }   // no level up: nothing to do
    lastHadActors = have;

    float pp[3];
    bool havePos = SWSE_PosGet(pp) && !(pp[0] == 0 && pp[1] == 0 && pp[2] == 0);
    DWORD now = GetTickCount();

    for (int i = 0; i < g_trgN; i++) {
        Trigger* t = &g_trg[i];
        if (t->actionN == 0) continue;
        if (!LevelMatches(t)) continue;
        if (t->onceMode == 2 && t->fired > 0) continue;
        if (t->onceMode == 1 && t->firedThisLevel > 0) continue;
        if (t->cooldown > 0.0f && t->lastFire &&
            (now - t->lastFire) < (DWORD)(t->cooldown * 1000.0f)) continue;

        bool cond = false;
        switch (t->kind) {
        case W_LEVELLOAD:
            cond = levelJustLoaded;
            break;
        case W_EVERY:
            if (t->seconds <= 0.0f) break;
            if (!t->lastFire) t->lastFire = g_levelStamp ? g_levelStamp : now;
            cond = (now - t->lastFire) >= (DWORD)(t->seconds * 1000.0f);
            break;
        case W_ENTER:
        case W_LEAVE: {
            if (!havePos) break;
            float pt[3];
            if (!PointOf(t, pt)) break;
            float dx = pp[0] - pt[0], dy = pp[1] - pt[1], dz = pp[2] - pt[2];
            bool in = (dx*dx + dy*dy + dz*dz) <= (t->radius * t->radius);
            // EDGE, not state: a trigger must fire on the crossing, otherwise
            // standing inside the radius re-fires it every frame. `primed`
            // additionally requires having been outside once, so loading a
            // save while already inside does not immediately fire it.
            if (t->kind == W_ENTER) cond = in && !t->inside && t->primed;
            else                    cond = !in && t->inside;
            if (!in) t->primed = true;
            t->inside = in;
            break;
        }
        case W_KILLED:
        case W_CLEARED: {
            // Throttled to 2 Hz: this walks the cached actor list and reads
            // each one's prefs, which is cheap but not free, and combat state
            // does not change meaningfully between frames.
            static DWORD nextCount = 0;
            static int   cachedAlive[MAX_TRIGGERS];
            static bool  haveCached = false;
            if (now >= nextCount) {
                nextCount = now + 500;
                for (int k = 0; k < g_trgN; k++)
                    if (g_trg[k].kind == W_KILLED || g_trg[k].kind == W_CLEARED)
                        cachedAlive[k] = SWSE_CountNpcsOfType(g_trg[k].typeHash);
                haveCached = true;
            }
            if (!haveCached) break;
            int alive = cachedAlive[i];
            if (alive < 0) break;                 // cache not populated yet

            if (t->kind == W_CLEARED) {
                cond = (alive == 0);
            } else {
                // `killed <type> count N`: N of them have DIED since the level
                // loaded. The peak seen is the level's population, so deaths
                // are peak minus alive. Comparing alive to N directly (the
                // first version of this) fired when a level merely had few of
                // them, which is not the same event at all.
                if (alive > t->peakAlive) t->peakAlive = alive;
                cond = (t->peakAlive - alive) >= t->needCount;
            }
            break;
        }
        default: break;
        }
        if (!cond) continue;

        // Chance is rolled at the moment the condition becomes true, not per
        // frame, because the edge detection above means this runs once.
        if (t->chance < 100) {
            if ((rand() % 100) >= t->chance) continue;
        }
        RunActions(t);
    }
}

int         SWSE_TriggerCount()          { return g_trgN; }
const char* SWSE_TriggerName(int i)      { return (i>=0 && i<g_trgN) ? g_trg[i].name : ""; }
int         SWSE_TriggerFireCount(int i) { return (i>=0 && i<g_trgN) ? g_trg[i].fired : 0; }
const char* SWSE_TriggerWhen(int i)      { return (i>=0 && i<g_trgN) ? g_trg[i].whenText : ""; }
bool        SWSE_TriggerArmed(int i) {
    if (i < 0 || i >= g_trgN) return false;
    const Trigger* t = &g_trg[i];
    if (t->onceMode == 2 && t->fired > 0) return false;
    if (t->onceMode == 1 && t->firedThisLevel > 0) return false;
    return LevelMatches(t);
}
