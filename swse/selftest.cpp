// SWSE self-test - see selftest.h.

#include "features.h"
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
#include <windows.h>
#include <gl/GL.h>
#include <stdio.h>
#include <string.h>

static int g_pass = 0, g_warn = 0, g_fail = 0, g_ranAt = 0;

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
    char detail[200];
    va_list ap; va_start(ap, fmt);
    wvsprintfA(detail, fmt, ap);
    va_end(ap);

    const char* tag = (ok > 0) ? "PASS" : (ok < 0) ? "WARN" : "FAIL";
    if (ok > 0) g_pass++; else if (ok < 0) g_warn++; else g_fail++;

    char line[280];
    wsprintfA(line, "[%s] %-14s %s", tag, name, detail);
    SWSE_ConsolePrint(line);
    LogT(line);
}

int SWSE_SelfTestRun() {
    g_pass = g_warn = g_fail = 0;
    g_ranAt = (int)GetTickCount();

    char hdr[120];
    wsprintfA(hdr, "=== SWSE self-test ===");
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
            Check(-1, "hit reacts", "disabled in features.txt");
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
            Check(-1, "foliage", "disabled in features.txt");
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
            Check(-1, "wind", "disabled in features.txt (foliage)");
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
            Check(-1, "HD textures", "disabled in features.txt");
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
        Check(-1, "graphics", "disabled in features.txt");
    else Check(SWSE_GfxReady() ? 1 : -1, "graphics",
          SWSE_GfxReady() ? "post-process pipeline ready"
                          : "post-process not initialised");

    // ---- agent debug mode -------------------------------------------------
    Check(SWSE_AgentDebugModeOn() ? 1 : -1, "agentdebug",
          SWSE_AgentDebugModeOn() ? "ON - runs unfocused, desktop usable"
                                  : "off");

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
// Fires once the level is genuinely up, using the actor list going from empty
// to populated as the "level ready" signal - that transition happens on every
// level load, because the list is invalidated and rebuilt. A short settle
// delay after it lets wind injection and texture substitution happen first, so
// the test measures a running game rather than one mid-startup.
void SWSE_SelfTestTick() {
    static int  lastHadActors = 0;
    static DWORD dueAt = 0;

    unsigned act[4];
    int have = SWSE_WatchActors(act, 4) > 0 ? 1 : 0;

    if (have && !lastHadActors) dueAt = GetTickCount() + 5000;   // arm
    lastHadActors = have;

    if (dueAt && GetTickCount() >= dueAt) {
        dueAt = 0;
        SWSE_SelfTestRun();
    }
}
