// SWSE level watcher - see levelwatch.h.

#include "levelwatch.h"
#include "scriptvm.h"
#include "gamebuild.h"      // safe mode: the watcher does not start
#include <windows.h>
#include <stdio.h>
#include <string.h>

static unsigned g_player = 0, g_form0 = 0, g_form1 = 0;
static unsigned g_epoch = 0;
static DWORD    g_since = 0;
static DWORD    g_nextPoll = 0;
static DWORD    g_missSince = 0;     // first failed poll of the current gap
static DWORD    g_lastEvent = 0;     // tick of the last logged level event

// How long the body must be missing before the level counts as gone. One
// failed poll used to be enough: the next good poll then always looked like a
// new body, even for the very same pair, and every per-level applier ran again
// - AI tuning, player tuning (which then re-captured its own tuned values as
// the game's), the `levelload` triggers. A menu or a load is seconds long; a
// pointer caught mid-update is one poll.
#define GONE_MS 1500

static void LogL(const char* s) {
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

void SWSE_LevelTick() {
    // Safe mode (gamebuild.h): the watcher finds the player by calling the
    // game (SWSE_PlayerForms -> PlayerObj), so on an unknown build it never
    // starts. The level stays "not up" at epoch 0, and everything keyed on it
    // stays idle: the per-level appliers, the self-test's per-level run,
    // plugins' LEVEL_UP.
    if (SWSE_GameBuildSafeMode()) {
        static bool s_said = false;
        if (!s_said) {
            s_said = true;
            LogL("level: not watched - safe mode (unknown game build): finding the player "
                 "means calling the game");
        }
        return;
    }
    DWORD now = GetTickCount();
    if ((int)(now - g_nextPoll) < 0) return;
    g_nextPoll = now + 250;

    // Keyed on the player and its two FORM motion objects (Steef, Stranger),
    // not on the current motion: that one flips between the two forms on every
    // transformation, and each flip counted as a new level - re-running every
    // per-level applier (a full heal from player tuning, `levelload` triggers
    // again). A load rebuilds the form motions; it may reuse the player
    // object at the same address (seen on a warp), so the player alone is not
    // enough either.
    unsigned p = 0, f0 = 0, f1 = 0;
    if (SWSE_PlayerForms(&p, &f0, &f1) != 1) {
        if (!g_player) return;                       // already gone
        if (!g_missSince) g_missSince = now ? now : 1;
        if (now - g_missSince < GONE_MS) return;     // a blip: same level
        char g[120];
        wsprintfA(g, "level: player body gone (menu or loading), %u ms after the last level event",
                  g_lastEvent ? (unsigned)(now - g_lastEvent) : 0u);
        LogL(g);
        g_lastEvent = now;
        g_player = g_form0 = g_form1 = 0;
        g_since = 0;
        g_missSince = 0;
        return;
    }
    g_missSince = 0;
    // A form slot that CHANGES (one non-zero value to another) is a rebuild;
    // one that merely appears (0 -> x) is a form created late, not a level.
    // After a real absence g_player is 0, so even a body rebuilt at the same
    // addresses is a new level.
    bool changed = (p != g_player) ||
                   (g_form0 && f0 && f0 != g_form0) ||
                   (g_form1 && f1 && f1 != g_form1);
    g_player = p;
    if (f0) g_form0 = f0;
    if (f1) g_form1 = f1;
    if (changed) {
        g_epoch++;
        g_since = now;
        char b[200];
        wsprintfA(b, "level: new player body (epoch %u) player %08X forms %08X %08X, %u ms after the last level event",
                  g_epoch, p, f0, f1, g_lastEvent ? (unsigned)(now - g_lastEvent) : 0u);
        LogL(b);
        g_lastEvent = now;
    }
}

unsigned SWSE_LevelEpoch() { return g_epoch; }

bool SWSE_LevelUp() { return g_player != 0; }

unsigned SWSE_LevelAgeMs() {
    if (!g_player || !g_since) return 0;
    return (unsigned)(GetTickCount() - g_since);
}

bool SWSE_LevelDue(unsigned* lastEpoch, unsigned settleMs) {
    if (!g_player || !lastEpoch) return false;
    if (*lastEpoch == g_epoch) return false;
    if (SWSE_LevelAgeMs() < settleMs) return false;
    *lastEpoch = g_epoch;
    return true;
}

void SWSE_LevelBody(unsigned* player, unsigned* motion) {
    if (player) *player = g_player;
    if (motion) *motion = g_player ? (g_form0 ? g_form0 : g_form1) : 0;
}
