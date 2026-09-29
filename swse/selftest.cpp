// SWSE self-test - see selftest.h.

#include "features.h"
#include "levelwatch.h"
#include "selftest.h"
#include "console.h"
#include "granny.h"
#include "foliage.h"
#include "wind.h"
#include "glspy.h"
#include "gfx.h"
#include "input.h"
#include "framehook.h"
#include "scriptvm.h"
#include "gamebuild.h"
#include "aitune.h"
#include "triggers.h"
#include "playertune.h"
#include "prefsedit.h"
#include "mute.h"
#include "plugins.h"
#include <windows.h>
#include <gl/GL.h>
#include <stdio.h>
#include <string.h>

static int g_pass = 0, g_warn = 0, g_fail = 0, g_ranAt = 0;
// Systems switched off in features.txt, reported on one line at the end. A
// console-only install (the 1.1 default) used to print five "disabled" WARN
// lines every level - noise that buried the lines that mattered, and a
// system the user switched off is not a warning.
static char g_off[200];
static void Off(const char* name) {
    if (g_off[0]) lstrcatA(g_off, ", ");
    lstrcatA(g_off, name);
}
// Safe mode (gamebuild.h): the systems that read or call the game cannot run
// on this build at all, whatever features.txt says. They get their own line
// rather than [OFF ] - whose advice, `features <name> on`, would be refused -
// and never a FAIL: nothing is broken, SWSE is keeping out of the game.
static char g_safe[200];
static void OffOrSafe(const char* name) {
    if (!SWSE_GameBuildSafeMode()) { Off(name); return; }
    if (g_safe[0]) lstrcatA(g_safe, ", ");
    lstrcatA(g_safe, name);
}

static void LogT(const char* s) {
    char path[MAX_PATH];
    GetModuleFileNameA(GetModuleHandleA(NULL), path, MAX_PATH);
    char* sl = strrchr(path, '\\');
    if (sl) *(sl + 1) = 0;
    lstrcatA(path, "swse_selftest.txt");
    FILE* f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "%s\n", s);
    fclose(f);
}

// Report one check. `ok` 1 = pass, 0 = fail, -1 = warn (not applicable here,
// e.g. no foliage in this level - a warn must never be read as "working").
static void Check(int ok, const char* name, const char* fmt, ...) {
    // Bounded: wvsprintfA assumes 1024 bytes of room (QA Q1's bug class).
    char detail[200];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(detail, sizeof(detail), _TRUNCATE, fmt, ap);
    va_end(ap);

    const char* tag = (ok > 0) ? "PASS" : (ok < 0) ? "WARN" : "FAIL";
    if (ok > 0) g_pass++; else if (ok < 0) g_warn++; else g_fail++;

    char line[280];
    wsprintfA(line, "[%s] %-14s %s", tag, name, detail);
    SWSE_ConsolePrint(line);
    LogT(line);
}

// Native plugins (1.1) report through these: their checks are counted in the
// summary like SWSE's own, and a plugin that is off joins the [OFF ] line.
static void PluginCheck(int ok, const char* name, const char* detail) { Check(ok, name, "%s", detail); }
static void PluginOff(const char* name) {
    if (lstrlenA(g_off) + lstrlenA(name) + 3 < (int)sizeof(g_off)) Off(name);
}

int SWSE_SelfTestRun() {
    g_pass = g_warn = g_fail = 0;
    g_ranAt = (int)GetTickCount();
    g_off[0] = 0;
    g_safe[0] = 0;

    char hdr[120];
    wsprintfA(hdr, SWSE_GameBuildSafeMode() ? "=== SWSE self-test - unknown build: safe mode ==="
                                            : "=== SWSE self-test ===");
    SWSE_ConsolePrint(hdr);
    LogT(hdr);

    // ---- frame hook: are we even running? -------------------------------
    {
        double lo = 0, wo = 0, lf = 0, wf = 0; int st = 0;
        SWSE_FramePerf(&lo, &wo, &lf, &wf, &st);
        Check(lf > 0.0 ? 1 : 0, "frame hook",
              "last frame %d ms, worst %d ms, %d stalls >80ms",
              (int)lf, (int)wf, st);
    }

    // ---- hit reactions ---------------------------------------------------
    // The check that would have caught the regression: enabled AND has actors
    // AND is polling them. Any one of those alone is not evidence of work.
    {
        int on = SWSE_HitReactEnabled();
        unsigned act[8];
        int nActors = SWSE_WatchActors(act, 8);
        // WatchActors caps at the buffer, so ask for the real count separately
        unsigned polled = 0, hits = 0;
        SWSE_HitReactWatchStats(&polled, &hits);
        if (!SWSE_Feature(FEAT_HITREACT)) {
            OffOrSafe("hitreact");
        } else if (!on) {
            Check(0, "hit reacts", "OFF - shooting will produce no reaction");
        } else if (nActors <= 0) {
            Check(0, "hit reacts", "ON but NO actors - damage cannot be detected");
        } else if (polled == 0) {
            Check(0, "hit reacts", "ON with actors but 0 polls - watch is not running");
        } else {
            Check(1, "hit reacts", "ON, actors present, %u polls, %u hits seen",
                  polled, hits);
        }
    }

    // ---- foliage identification -----------------------------------------
    {
        int listN = 0, known = 0, binds = 0, peak = 0, hooked = 0;
        SWSE_FoliageStats(&listN, &known, &binds, &peak, &hooked);
        if (!SWSE_Feature(FEAT_FOLIAGE))
            Off("foliage (and wind)");
        else if (listN <= 0)
            Check(0, "foliage", "no fingerprints loaded - foliage.txt missing?");
        else if (!hooked)
            Check(0, "foliage", "%d fingerprints but bind tracking OFF", listN);
        else if (known <= 0)
            Check(-1, "foliage", "%d fingerprints, none in this level", listN);
        else
            Check(1, "foliage", "%d fingerprints, %d live here, %d binds/frame",
                  listN, known, peak);
    }

    // ---- wind -------------------------------------------------------------
    {
        int inj = 0, fail = 0, on = 0; float cx = 0, cz = 0;
        SWSE_WindStats(&inj, &fail, &on, &cx, &cz);
        int listN = 0, known = 0, binds = 0, peak = 0, hooked = 0;
        SWSE_FoliageStats(&listN, &known, &binds, &peak, &hooked);
        // Wind rides on the foliage tracker, so it is disabled by the same
        // switch. Reported as "off", never as a failure - a feature the user
        // deliberately turned off is not broken.
        if (!SWSE_Feature(FEAT_FOLIAGE))
            ;                                   // reported with foliage
        else if (!on)
            Check(0, "wind", "OFF - foliage will not move");
        else if (inj <= 0 && known > 0)
            Check(0, "wind", "ON but 0 programs injected - nothing will move");
        else if (inj <= 0)
            Check(-1, "wind", "ON, nothing injected yet (no foliage drawn)");
        else
            Check(1, "wind", "ON, %d programs injected, %d failed", inj, fail);

        // Character-mesh guard. Injecting a skinned program is what deformed
        // characters - some skinned programs draw both bone-rigged plants and
        // character meshes, so refusing them at injection is the only safe
        // rule. Its activity has to be visible here, not only in `wind`.
        //
        // Zero refusals is only WARN, not FAIL: it is the correct answer in a
        // level where no character program was ever offered for injection.
        if (SWSE_Feature(FEAT_FOLIAGE) && on) {
            int refused = SWSE_WindRejectedSkinned();
            if (refused > 0)
                Check(1, "wind guard", "%d character mesh program(s) refused", refused);
            else if (inj > 0)
                Check(-1, "wind guard", "0 refused - none offered here, or guard inactive");
        }
    }

    // ---- HD textures ------------------------------------------------------
    {
        int avail = 0, loaded = 0, failed = 0;
        SWSE_HdStats(&avail, &loaded, &failed);
        if (!SWSE_Feature(FEAT_HDTEXTURES))
            Off("hdtextures");
        else if (avail <= 0)
            Check(0, "HD textures", "no .oft replacements installed");
        else if (failed > 0)
            // A failed replacement silently falls back to vanilla, so this has
            // to be loud: it is the one HD failure play cannot show you.
            Check(0, "HD textures", "%d installed, %d substituted, %d FAILED to load (run `hd`)",
                  avail, loaded, failed);
        else if (loaded <= 0)
            Check(-1, "HD textures", "%d installed, none substituted here yet", avail);
        else
            Check(1, "HD textures", "%d installed, %d substituted so far", avail, loaded);
    }

    // ---- graphics / RTGI --------------------------------------------------
    if (!SWSE_Feature(FEAT_GRAPHICS))
        Off("graphics");
    else Check(SWSE_GfxReady() ? 1 : -1, "graphics",
          SWSE_GfxReady() ? "post-process pipeline ready"
                          : "post-process not initialised");

    // ---- 1.1 systems ------------------------------------------------------
    {
        char gb[240];
        SWSE_GameBuildDescribe(gb, sizeof(gb));
        // An unknown build is not a failure - SWSE runs in safe mode - but
        // nothing that patches, calls or reads the game runs there, so say it.
        if (SWSE_GameBuildSafeMode())
            Check(-1, "game build", "unknown build: safe mode - the checks below are the systems "
                                    "that run without the game (`status` names the exe)");
        else
            Check(1, "game build", "%s", gb + 12);   // skip "game build: "
    }
    // The watcher finds the player by calling the game: it never starts in
    // safe mode (levelwatch.cpp), so "no level" there is not a failure.
    if (SWSE_GameBuildSafeMode())
        OffOrSafe("level watch");
    else
        Check(SWSE_LevelUp() ? 1 : 0, "level watch", SWSE_LevelUp()
              ? "level up (epoch %u, %u s)" : "no level up - the watcher sees no player (epoch %u)",
              SWSE_LevelEpoch(), SWSE_LevelAgeMs() / 1000);
    if (!SWSE_Feature(FEAT_AITUNING)) OffOrSafe("aituning");
    else {
        const char* want = SWSE_AiTuneWanted();
        const char* act = SWSE_AiTuneActive();
        if (!want || !*want)
            Check(-1, "AI tuning", "on, but aiprefs.txt says active = off (nothing to apply)");
        else if (act && !lstrcmpiA(act, want) && SWSE_AiTuneActiveEpoch() == SWSE_LevelEpoch())
            Check(1, "AI tuning", "'%s' applied in this level, %d object(s)", want, SWSE_AiTuneCount());
        else
            Check(-1, "AI tuning", "'%s' not applied in this level yet (it applies 4 s after load)", want);
    }
    if (!SWSE_Feature(FEAT_TRIGGERS)) OffOrSafe("triggers");
    else {
        int n = SWSE_TriggerCount();
        Check(n > 0 ? 1 : -1, "triggers", n > 0 ? "%d loaded and armed" : "on, but no triggers.txt loaded", n);
    }
    if (!SWSE_Feature(FEAT_NPCTUNING)) OffOrSafe("npctuning");
    else {
        int rules = 0;
        int loaded = SWSE_NpcTuningStats(&rules);
        Check(loaded ? 1 : 0, "char tuning", loaded ? "%d character rule(s) loaded; applied as characters spawn"
                                                   : "on, but no rules loaded (%d)", rules);
    }
    if (!SWSE_Feature(FEAT_PLAYERTUNE)) OffOrSafe("playertune");
    else {
        int values = 0, here = 0;
        SWSE_PlayerTuneStats(&values, &here);
        if (values == 0)
            Check(-1, "player tune", "on, but playerprefs.txt sets nothing (blank keeps the game's values)");
        else
            Check(here ? 1 : -1, "player tune", here ? "%d value(s) applied in this level"
                                                    : "%d value(s), not applied here yet (3 s after load)", values);
    }
    if (!SWSE_Feature(FEAT_PREFSEDIT)) OffOrSafe("prefsedit");
    else {
        int edits = 0, changed = 0;
        SWSE_PrefsEditStats(&edits, &changed);
        if (edits == 0)
            Check(-1, "prefs edits", "on, but no prefs.txt edits loaded");
        else
            Check(changed > 0 ? 1 : -1, "prefs edits", changed > 0 ? "%d edit(s), %d value(s) changed from shipped"
                                                               : "%d edit(s), none written in this level yet", edits, changed);
    }

    if (!SWSE_Feature(FEAT_RAYTRACE)) Off("raytrace");
    else Check(SWSE_Feature(FEAT_GRAPHICS) && SWSE_GfxReady() ? 1 : 0, "ray tracing",
               SWSE_Feature(FEAT_GRAPHICS) && SWSE_GfxReady()
                   ? "on (experimental) - RTAO follows rtao_enable in graphics.txt"
                   : "on, but the graphics pipeline is not running - it cannot trace");

    // ---- native plugins (1.1) -----------------------------------------------
    // After SWSE's own lines: each plugin that is on runs its own checks if
    // it registered any, else SWSE checks that it is being called at all.
    SWSE_PluginsSelfTest(PluginCheck, PluginOff);

    // ---- agent debug mode -------------------------------------------------
    // Informational, not a check: off is the normal state for a player.
    {
        char line[240];
        wsprintfA(line, "[INFO] %-14s %s%s", "agentdebug",
                  SWSE_AgentDebugModeOn() ? "ON - runs unfocused, desktop usable" : "off",
                  SWSE_GameMuted() ? " (game audio muted)" : "");
        SWSE_ConsolePrint(line);
        LogT(line);
    }
    if (g_safe[0]) {
        char line[300];
        _snprintf_s(line, sizeof(line), _TRUNCATE, "[SAFE] %-14s %s - they read or call the game, "
                    "so they do not run on this build", "unknown build", g_safe);
        SWSE_ConsolePrint(line);
        LogT(line);
    }
    if (g_off[0]) {
        char line[300];
        wsprintfA(line, "[OFF ] %-14s %s  (`features <name> on` switches one on)", "features", g_off);
        SWSE_ConsolePrint(line);
        LogT(line);
    }

    char sum[160];
    wsprintfA(sum, "=== %d passed, %d warned, %d FAILED ===", g_pass, g_warn, g_fail);
    SWSE_ConsolePrint(sum);
    LogT(sum);
    if (g_fail > 0)
        SWSE_ConsolePrint("something is not operational - see the lines marked FAIL");
    return g_fail;
}

void SWSE_SelfTestStats(int* passed, int* warned, int* failed, int* ranAt) {
    if (passed) *passed = g_pass;
    if (warned) *warned = g_warn;
    if (failed) *failed = g_fail;
    if (ranAt)  *ranAt  = g_ranAt;
}

// ---- automatic trigger -----------------------------------------------------
// Fires once per level, a settle delay after the level watcher sees a new
// player body, so wind injection and texture substitution have happened first
// and the test measures a running game rather than one mid-startup. (1.0.x
// used the hit-reaction actor list, which never fills with hit reactions off.)
void SWSE_SelfTestTick() {
    static unsigned lastEpoch = 0;
    if (SWSE_LevelDue(&lastEpoch, 5000)) { SWSE_SelfTestRun(); return; }
    // Safe mode has no level watcher to time a run by. Once, 10 s after the
    // first frame (the main menu), so swse_selftest.txt says what runs here.
    if (SWSE_GameBuildSafeMode()) {
        static DWORD s_first = 0;
        static bool  s_done = false;
        if (s_done) return;
        DWORD now = GetTickCount();
        if (!s_first) { s_first = now ? now : 1; return; }
        if (now - s_first >= 10000) { s_done = true; SWSE_SelfTestRun(); }
    }
}
