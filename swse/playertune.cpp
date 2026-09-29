// SWSE player tuning - see playertune.h.

#include "playertune.h"
#include "levelwatch.h"
#include "modregistry.h"
#include "scriptvm.h"
#include "playnpc.h"     // playnpc owns health/stamina while a character is played
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PT_HEALTH, PT_STAMINA, PT_SPEED, PT_JUMP, PT_GRAVITY, PT_AIRCONTROL, PT_COUNT };
static const char* kKeys[PT_COUNT] = {
    "health", "stamina", "speed", "jump", "gravity", "aircontrol"
};
// SWSE_MotionField selectors for the motion-backed keys (-1 = not motion).
static const int kMotionField[PT_COUNT] = { -1, -1, 1, 0, 2, 3 };

static float g_want[PT_COUNT];
static bool  g_has[PT_COUNT];
static bool  g_loaded = false;
static char  g_path[MAX_PATH] = "";

// What the game had before this level's first write. Health and stamina are
// (current, max, base) triples; the motion values use [0] only.
static unsigned g_baseEpoch = 0;
static bool     g_baseHave[PT_COUNT];
static float    g_base[PT_COUNT][3];

static unsigned g_appliedEpoch = 0;
static DWORD    g_nextWatch = 0;
static int      g_reapplies = 0;

static void LogP(const char* s) {
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

// Every caller's buffer is at least 24 bytes. Truncating, never faulting: a
// huge value (`hp 99999999`, a 1e30) must not overflow or trip sprintf_s's
// invalid-parameter handler, which ends the process.
static void FmtF(char* out, float v) {
    if (v != v) { lstrcpyA(out, "nan"); return; }
    float a = v < 0.0f ? -v : v;
    _snprintf_s(out, 24, _TRUNCATE, (a >= 1.0e7f) ? "%.4g" : "%.2f", v);
}

static void PrefsPath(char* out) {
    if (SWSE_FindModFile("playerprefs.txt", out, MAX_PATH)) return;
    char exe[MAX_PATH];
    GetModuleFileNameA(GetModuleHandleA(NULL), exe, MAX_PATH);
    char* sl = strrchr(exe, '\\'); if (sl) *sl = 0;     // ...\bin
    sl = strrchr(exe, '\\'); if (sl) *sl = 0;           // game root
    wsprintfA(out, "%s\\SWSEMods\\SWSE Console\\playerprefs.txt", exe);
}

int SWSE_PlayerTuneLoad(char* msg, int msgLen) {
    for (int i = 0; i < PT_COUNT; i++) { g_has[i] = false; g_want[i] = 0.0f; }
    g_loaded = true;
    PrefsPath(g_path);

    FILE* f = fopen(g_path, "r");
    if (!f) {
        char t[1100];
        wsprintfA(t, "playertune: no playerprefs.txt (looked in %s)", g_path);
        lstrcpynA(msg, t, msgLen);
        return 0;
    }
    char line[256];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        char* s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#' || *s == ';' || *s == '\r' || *s == '\n') continue;
        // key [=] value   - both forms, since hand-written files use either
        char key[32] = { 0 }, val[48] = { 0 };
        int ki = 0, vi = 0; bool inVal = false;
        for (char* c = s; *c; c++) {
            if (*c == '#' || *c == ';' || *c == '\r' || *c == '\n') break;
            if (*c == '=') { inVal = true; continue; }
            if (*c == ' ' || *c == '\t') { if (ki) inVal = true; continue; }
            if (!inVal) { if (ki < 31) key[ki++] = *c; }
            // A comma is a decimal point: Stranger: Armed to the Teeth writes
            // this file with the Windows number format, and strtod stops at
            // the comma of "1,5" - reading 1.
            else        { if (vi < 47) val[vi++] = (*c == ',') ? '.' : *c; }
        }
        if (!key[0]) continue;
        for (int i = 0; i < PT_COUNT; i++) {
            if (lstrcmpiA(key, kKeys[i])) continue;
            // blank, '-' or 'default' = leave the game's own value
            if (!val[0] || !lstrcmpA(val, "-") || !lstrcmpiA(val, "default")) break;
            char* end = nullptr;
            double d = strtod(val, &end);
            if (end == val || !(d > -1.0e9 && d < 1.0e9)) break;   // NaN fails too
            g_want[i] = (float)d;
            g_has[i] = true;
            n++;
            break;
        }
    }
    fclose(f);

    char t[1100];
    wsprintfA(t, "playertune: %d value(s) from %s", n, g_path);
    lstrcpynA(msg, t, msgLen);
    LogP(t);
    return n;
}

static bool ReadTriple(int k, float* a, float* b, float* c) {
    if (k == PT_HEALTH)  return SWSE_PlayerHealth(a, b, c) == 1;
    if (k == PT_STAMINA) return SWSE_PlayerStamina(a, b, c) == 1;
    return false;
}

// Capture the game's own value the first time a key is touched this level.
static void CaptureBase(int k) {
    unsigned ep = SWSE_LevelEpoch();
    if (g_baseEpoch != ep) {
        for (int i = 0; i < PT_COUNT; i++) g_baseHave[i] = false;
        g_baseEpoch = ep;
        SWSE_MotionRescan();      // a new level means new motion objects
    }
    if (g_baseHave[k]) return;
    if (kMotionField[k] < 0) {
        if (!ReadTriple(k, &g_base[k][0], &g_base[k][1], &g_base[k][2])) return;
    } else {
        float cur = 0.0f;
        if (SWSE_MotionField(kMotionField[k], nullptr, &cur) <= 0) return;
        g_base[k][0] = cur;
    }
    g_baseHave[k] = true;
}

static bool WriteKey(int k, float v) {
    if (k == PT_HEALTH)  return SWSE_PlayerSetHealth(v) == 1;
    if (k == PT_STAMINA) return SWSE_PlayerSetStamina(v) == 1;
    float cur = 0.0f;
    return SWSE_MotionField(kMotionField[k], &v, &cur) > 0;
}

static int ApplyAll(char* detail, int detailLen) {
    int done = 0;
    detail[0] = 0;
    for (int k = 0; k < PT_COUNT; k++) {
        if (!g_has[k]) continue;
        CaptureBase(k);
        bool ok = WriteKey(k, g_want[k]);
        if (ok) done++;
        char v[24]; FmtF(v, g_want[k]);
        char one[48];
        wsprintfA(one, " %s=%s%s", kKeys[k], v, ok ? "" : "(FAILED)");
        if (lstrlenA(detail) + lstrlenA(one) < detailLen - 1) lstrcatA(detail, one);
    }
    return done;
}

int SWSE_PlayerTuneApply(char* msg, int msgLen) {
    if (!g_loaded) { char m[300]; SWSE_PlayerTuneLoad(m, sizeof(m)); }
    if (!SWSE_LevelUp()) {
        lstrcpynA(msg, "playertune: no player yet (load a save first)", msgLen);
        return 0;
    }
    bool any = false;
    for (int k = 0; k < PT_COUNT; k++) if (g_has[k]) any = true;
    if (!any) {
        lstrcpynA(msg, "playertune: playerprefs.txt sets nothing (every value blank)", msgLen);
        return 0;
    }
    char detail[200];
    int n = ApplyAll(detail, sizeof(detail));
    g_appliedEpoch = SWSE_LevelEpoch();
    char t[1100];
    wsprintfA(t, "playertune: applied %d value(s):%s", n, detail);
    lstrcpynA(msg, t, msgLen);
    LogP(t);
    return n;
}

int SWSE_PlayerTuneRestore(char* msg, int msgLen) {
    if (g_baseEpoch != SWSE_LevelEpoch() || !SWSE_LevelUp()) {
        lstrcpynA(msg, "playertune: nothing to restore in this level", msgLen);
        return 0;
    }
    int n = 0;
    for (int k = 0; k < PT_COUNT; k++) {
        if (!g_baseHave[k]) continue;
        if (kMotionField[k] < 0) {
            // Max and base go back exactly; current is clamped to the old max
            // so restoring never heals the player past what the game allows.
            float c = 0, m = 0, b = 0;
            if (!ReadTriple(k, &c, &m, &b)) continue;
            float cur = (c > g_base[k][1]) ? g_base[k][1] : c;
            int off = (k == PT_HEALTH) ? 0x78 : 0x8C;
            if (SWSE_PlayerSet3(off, cur, g_base[k][1], g_base[k][2]) == 1) n++;
        } else {
            if (WriteKey(k, g_base[k][0])) n++;
        }
        g_baseHave[k] = false;
    }
    char t[1100];
    wsprintfA(t, "playertune: restored %d value(s) to the game's own", n);
    lstrcpynA(msg, t, msgLen);
    LogP(t);
    return n;
}

void SWSE_PlayerTuneTick() {
    if (!g_loaded) { char m[300]; SWSE_PlayerTuneLoad(m, sizeof(m)); }
    if (!SWSE_LevelUp()) return;
    // playnpc owns health and stamina while a character is played: nothing
    // here runs until it ends, and a level's first apply waits for that too.
    if (SWSE_PlayNpcActive()) return;

    // Once per level, after the game's own difficulty initialisation.
    if (SWSE_LevelDue(&g_appliedEpoch, 3000)) {
        bool any = false;
        for (int k = 0; k < PT_COUNT; k++) if (g_has[k]) any = true;
        if (any) {
            char detail[200];
            int n = ApplyAll(detail, sizeof(detail));
            char t[1100];
            wsprintfA(t, "playertune: level %u - applied %d value(s):%s",
                      SWSE_LevelEpoch(), n, detail);
            LogP(t);
        }
        g_nextWatch = GetTickCount() + 1000;
        return;
    }

    // The game re-initialised a max back to its own number: apply again.
    DWORD now = GetTickCount();
    if ((int)(now - g_nextWatch) < 0) return;
    g_nextWatch = now + 1000;
    if (g_appliedEpoch != SWSE_LevelEpoch() || g_baseEpoch != SWSE_LevelEpoch()) return;
    for (int k = PT_HEALTH; k <= PT_STAMINA; k++) {
        if (!g_has[k] || !g_baseHave[k]) continue;
        float c = 0, m = 0, b = 0;
        if (!ReadTriple(k, &c, &m, &b)) continue;
        float d0 = m - g_base[k][1], d1 = g_want[k] - g_base[k][1];
        if (d0 < 0) d0 = -d0;
        if (d1 < 0) d1 = -d1;
        if (d0 < 0.5f && d1 >= 0.5f) {
            WriteKey(k, g_want[k]);
            g_reapplies++;
            char t[1100];
            wsprintfA(t, "playertune: game reset %s to its own max - applied again", kKeys[k]);
            LogP(t);
        }
    }
}

void SWSE_PlayerTuneStatus(void (*emit)(const char*)) {
    if (!g_loaded) { char m[300]; SWSE_PlayerTuneLoad(m, sizeof(m)); }
    char t[1100];
    wsprintfA(t, "playerprefs.txt: %s", g_path[0] ? g_path : "(not found)");
    emit(t);
    for (int k = 0; k < PT_COUNT; k++) {
        char want[64] = "-", live[96] = "?", base[64] = "-";
        if (g_has[k]) FmtF(want, g_want[k]);
        if (kMotionField[k] < 0) {
            float c = 0, m = 0, b = 0;
            if (ReadTriple(k, &c, &m, &b)) {
                char a1[24], a2[24];
                FmtF(a1, c); FmtF(a2, m);
                wsprintfA(live, "%s/%s", a1, a2);
            }
            if (g_baseHave[k] && g_baseEpoch == SWSE_LevelEpoch()) FmtF(base, g_base[k][1]);
        } else {
            float cur = 0.0f;
            if (SWSE_MotionField(kMotionField[k], nullptr, &cur) > 0) FmtF(live, cur);
            if (g_baseHave[k] && g_baseEpoch == SWSE_LevelEpoch()) FmtF(base, g_base[k][0]);
        }
        wsprintfA(t, "  %-10s want %-9s live %-15s game's own %s",
                  kKeys[k], want, live, base);
        emit(t);
    }
    wsprintfA(t, "  applied in level epoch %u (now %u), re-applied %d time(s)",
              g_appliedEpoch, SWSE_LevelEpoch(), g_reapplies);
    emit(t);
}

// For the self-test: how many values playerprefs.txt sets, and whether they
// were applied in the level that is up now.
void SWSE_PlayerTuneStats(int* values, int* appliedHere) {
    int n = 0;
    for (int i = 0; i < PT_COUNT; i++) if (g_has[i]) n++;
    if (values) *values = n;
    if (appliedHere) *appliedHere = (g_appliedEpoch != 0 && g_appliedEpoch == SWSE_LevelEpoch()) ? 1 : 0;
}
