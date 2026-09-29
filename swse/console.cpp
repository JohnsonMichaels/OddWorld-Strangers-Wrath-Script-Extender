// SWSE Console - in-game dev console rendered by SWSE.
//
// Toggle: the `/~ key (VK_OEM_3). Type a command, Enter runs it, output scrolls
// in a translucent overlay across the top of the screen. The font is rasterized
// once via GDI into a GL texture atlas (no embedded font blob, crisp text).
//
// v1 commands drive what SWSE already controls (graphics/effects/settings).
// Game-state commands (god/ammo/tp/steef) are registered as stubs that report
// "pending" until the script-VM bridge lands - the registry makes adding them
// a one-liner.

#include "console.h"
#include "gamebuild.h"
#include "menu.h"
#include "mute.h"
#include "modregistry.h"
#include "triggers.h"
#include "positions.h"
#include "gfx.h"
#include "scriptvm.h"
#include "input.h"
#include "glspy.h"
#include "granny.h"
#include "shaderspy.h"
#include "uispy.h"
#include "aitune.h"
#include "features.h"
#include "foliage.h"
#include "materials.h"
#include "gpucompute.h"
#include "geocapture.h"
#include "raytrace.h"
#include "wind.h"
#include "framehook.h"
#include "selftest.h"
#include "levelwatch.h"
#include "playertune.h"
#include "prefsedit.h"
#include "playnpc.h"
#include "plugins.h"
#include "hookreg.h"
#include "freecam.h"
#include <gl/GL.h>
#include <string>
#include <cstring>
#include <cstdio>
#include <cstdlib>

#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

// ---- GL bits not in the 1.1 header ----
#define GL_CLAMP_TO_EDGE 0x812F
#ifndef GL_VERTEX_PROGRAM_ARB
#define GL_VERTEX_PROGRAM_ARB   0x8620
#define GL_FRAGMENT_PROGRAM_ARB 0x8804
#endif
typedef void (APIENTRY* glUseProgram_t)(GLuint);
static glUseProgram_t p_useProgram = nullptr;

// ---- font atlas ----
static const int CELL_W = 8, CELL_H = 16;   // glyph cell in the atlas
static const int ATLAS_COLS = 16, ATLAS_ROWS = 6;   // 96 chars, 0x20..0x7F
static const int ATLAS_W = CELL_W * ATLAS_COLS;     // 128
static const int ATLAS_H = CELL_H * ATLAS_ROWS;     // 96
static GLuint g_font = 0;
static bool   g_inited = false;

// ---- console state ----
static bool  g_open = false;
static char  g_input[256] = {0};
static int   g_inputLen = 0;
static bool  g_prevKey[256] = {false};

static const int LOG_MAX = 256;
// 240: help prints one wrapping line per category, which needs the width.
static char  g_log[LOG_MAX][240];
static int   g_logCount = 0;
static int   g_scroll = 0;   // lines scrolled up from bottom

bool SWSE_ConsoleOpen() { return g_open; }

// When the console last opened and closed. The game reads keys from its own
// message queue, which it may drain a frame after the console saw them, so
// input.cpp uses this window to keep what was typed into the console - the
// Escape or Enter that closed it included - from reaching the game late.
static DWORD g_openedAt = 0, g_closedAt = 0;
static void SetOpen(bool open) {
    if (open && !g_open) g_openedAt = GetTickCount();
    if (!open && g_open) g_closedAt = GetTickCount();
    g_open = open;
}
void SWSE_ConsoleOpenWindow(DWORD* openedAt, DWORD* closedAt) {
    if (openedAt) *openedAt = g_openedAt;
    if (closedAt) *closedAt = g_closedAt;
}

// ---- remote channel capture ----------------------------------------------
// While a remote command runs, every console line is also copied into this
// buffer so it can be written back out to remote_out.txt. See RemotePoll().
static char g_capBuf[16384];
static int  g_capLen = 0;
static bool g_capOn  = false;

static void CapAppend(const char* s) {
    int n = lstrlenA(s);
    if (g_capLen + n + 2 >= (int)sizeof(g_capBuf)) return;   // full; drop
    memcpy(g_capBuf + g_capLen, s, n); g_capLen += n;
    g_capBuf[g_capLen++] = '\r'; g_capBuf[g_capLen++] = '\n';
    g_capBuf[g_capLen] = 0;
}

// ---- line colouring -------------------------------------------------------
// Classified from the text itself so every existing call site gets colour for
// free. Ordered by specificity: the first match wins.
enum LineCol { LC_NORMAL = 0, LC_HEADING, LC_ECHO, LC_GOOD, LC_BAD, LC_DIM };
static unsigned char g_logCol[LOG_MAX];
static int g_cmdsRun = 0;        // 0 = show the big welcome title

static bool HasCI(const char* hay, const char* needle) {
    for (const char* p = hay; *p; p++) {
        const char *a = p, *b = needle;
        while (*a && *b && ((*a | 0x20) == (*b | 0x20))) { a++; b++; }
        if (!*b) return true;
    }
    return false;
}

static unsigned char ClassifyLine(const char* s) {
    if (s[0] == '-' && s[1] == '-' && s[2] == '-')        return LC_HEADING;
    if (s[0] == '>' && s[1] == ' ')                        return LC_ECHO;
    if (HasCI(s, "FAULT") || HasCI(s, "no such") ||
        HasCI(s, "could not") || HasCI(s, "failed") ||
        HasCI(s, "not working") || HasCI(s, "no script context") ||
        HasCI(s, "out of range") || HasCI(s, "unknown"))   return LC_BAD;
    if (HasCI(s, "granted") || HasCI(s, ": done") ||
        HasCI(s, " ON") || HasCI(s, "installed") ||
        HasCI(s, "primed") || HasCI(s, "saved") ||
        HasCI(s, "called") || HasCI(s, "->"))              return LC_GOOD;
    if (HasCI(s, "usage:") || HasCI(s, "(none)"))          return LC_DIM;
    return LC_NORMAL;
}

void SWSE_ConsolePrint(const char* text) {
    if (g_capOn) CapAppend(text);
    if (g_logCount < LOG_MAX) {
        lstrcpynA(g_log[g_logCount++], text, 240);
    } else {
        for (int i = 1; i < LOG_MAX; i++) memcpy(g_log[i - 1], g_log[i], 240);
        lstrcpynA(g_log[LOG_MAX - 1], text, 240);
    }
}
static void Printf(const char* fmt, ...) {
    // wvsprintfA writes up to 1024 characters whatever the buffer is; a 240
    // byte buffer here let a long alias/after/bind line overwrite the stack.
    // Full size to format, then SWSE_ConsolePrint keeps the first 239.
    char b[1100]; va_list ap; va_start(ap, fmt);
    wvsprintfA(b, fmt, ap); va_end(ap);
    SWSE_ConsolePrint(b);
}

// ---- command registry ----------------------------------------------------
typedef void (*CmdFn)(int argc, char** argv);
// `cat` groups commands in `help`. The list grew past 80 entries and a flat
// dump was unreadable. Keep these strings short - they are printed as headings.
struct Cmd { const char* name; const char* cat; const char* help; CmdFn fn; };

// Display order for the help screen; anything with an unlisted category still
// prints, under "other".
// "raytrace" and "input" joined in 1.1: those commands were filed under "gfx"
// and "input", which were not listed here, so they showed only on help's
// "other" line and `help input` answered "no such command or category".
static const char* kCats[] = {
    "player", "movement", "items", "world", "tuning", "graphics", "raytrace",
    "input", "scripting", "debug", "music", "console",
};
static const int N_CATS = sizeof(kCats) / sizeof(kCats[0]);

static void Cmd_help(int, char**);
static void Cmd_scripts(int, char**);
static void Cmd_reloadscripts(int, char**);

// user-defined script commands (declared here so Tab-completion can see the
// list; loaded/run further down near Execute - see the comment there)
struct DynCmd { char name[32]; char lines[24][128]; int lineCount; };
static DynCmd g_dynCmds[48];
static int    g_dynCmdCount = 0;
static void Cmd_clear(int, char**) { g_logCount = 0; g_scroll = 0; }
static void Cmd_echo(int argc, char** argv) {
    // Bounded: 1.0.x strcat'ed every word into 160 bytes, and an alias that
    // echoes its arguments reaches that easily.
    char line[240] = {0};
    int used = 0;
    for (int i = 1; i < argc; i++) {
        int len = lstrlenA(argv[i]);
        if (used + len + 2 > (int)sizeof(line)) len = (int)sizeof(line) - used - 2;
        if (len <= 0) break;
        memcpy(line + used, argv[i], len); used += len;
        line[used++] = ' ';
        line[used] = 0;
    }
    SWSE_ConsolePrint(line);
}
#include "swse_version.h"      // SWSE_VERSION
static void Cmd_ver(int, char**) {
    SWSE_ConsolePrint("SWSE " SWSE_VERSION " - Stranger's Wrath Script Extender");
    SWSE_ConsolePrint("console-only by default; `features` lists every system and switches them");
}
static void Cmd_gfx(int argc, char** argv) {
    if (!SWSE_Feature(FEAT_GRAPHICS)) {
        SWSE_ConsolePrint("graphics is off (the 1.1 default) - `features graphics on` loads it");
        return;
    }
    if (argc < 2) { SWSE_ConsolePrint("usage: gfx on|off|toggle|reload"); return; }
    if (!lstrcmpiA(argv[1], "on"))        { SWSE_GfxSetEnabled(1); SWSE_ConsolePrint("post-process ON"); }
    else if (!lstrcmpiA(argv[1], "off"))  { SWSE_GfxSetEnabled(0); SWSE_ConsolePrint("post-process OFF"); }
    else if (!lstrcmpiA(argv[1], "toggle")){ int s=!SWSE_GfxIsEnabled(); SWSE_GfxSetEnabled(s); Printf("post-process %s", s?"ON":"OFF"); }
    else if (!lstrcmpiA(argv[1], "reload")){ SWSE_GfxReloadSettings(); SWSE_ConsolePrint("settings reloaded"); }
    else SWSE_ConsolePrint("usage: gfx on|off|toggle|reload");
}
static bool TrySetPointer(const char* name, const char* val);   // fwd
static void Cmd_remote(int argc, char** argv);                  // fwd (remote channel)
// Safe mode (gamebuild.h): the one line for a command, a pointer chain or a
// game function that would reach the game on this build.
static void SafeModeRefuse(const char* what) {
    char m[300];
    SWSE_GameBuildRefusal(what, m, sizeof(m));
    SWSE_ConsolePrint(m);
}
static void Cmd_set(int argc, char** argv) {
    if (argc < 3) { SWSE_ConsolePrint("usage: set <name> <value>   (pointer chain, or a graphics key)"); return; }
    // pointer chains first (health, etc.), then fall back to graphics settings.
    // A chain starts at a Steam address (pointers.txt), so safe mode refuses
    // one; a graphics key still sets.
    if (SWSE_GameBuildSafeMode() && SWSE_PtrFind(argv[1]) >= 0) {
        char w[96];
        _snprintf_s(w, sizeof(w), _TRUNCATE, "set %s (a pointers.txt chain)", argv[1]);
        SafeModeRefuse(w);
        return;
    }
    if (TrySetPointer(argv[1], argv[2])) return;
    // Nothing is written for a key SWSE does not read or a value that is not
    // a number: a typo used to land in graphics.txt with "set ... =" printed.
    int r = SWSE_GfxSetSetting(argv[1], argv[2]);
    if (r == 1)       Printf("set %s = %s", argv[1], argv[2]);
    else if (r == 0)  Printf("unknown setting or pointer: %s - nothing written ('ptr' lists pointers)", argv[1]);
    else if (r == -2) Printf("%s: '%s' is not a number - nothing written", argv[1], argv[2]);
    else              Printf("%s: could not write graphics.txt - not saved", argv[1]);
}
// The "no script context" line, saying what is true now. SWSE finds the
// context itself: autoprime scans the heap for one, which exists once a save
// is loaded (not at the main menu); an ammo pickup also captures one on the
// Steam build. Picking up ammo used to be the only way, and these messages
// still said so. `searches`: the command looks by itself (EnsureCtx, at most
// every 3 s); the low-level ones (vcall, poke, freeze, watch) do not.
static void NoContext(const char* what, bool searches) {
    if (!SWSE_GameBuildKnown())
        Printf("%s: no script context - SWSE recognises it by the Steam build's vtable, "
               "and this game build is not one it knows (`status`)", what);
    else if (searches)
        Printf("%s: no script context yet - load a save first ('autoprime' searches again now)", what);
    else
        Printf("%s: no script context - load a save, then run 'autoprime' (this command does not search)", what);
}

// game-state commands routed through the script-VM bridge (scriptvm.cpp)
static void RunVM(const char* action, int arg) {
    int r = SWSE_ScriptDo(action, arg);
    if (r == 1)       Printf("%s: done", action);
    else if (r == 0)  NoContext(action, true);
    else if (r == -2) Printf("%s: faulted - the script context was stale and has been dropped; "
                             "the next command finds a fresh one", action);
    else if (r == -3) SafeModeRefuse(action);
    else              Printf("%s: unknown to bridge", action);
}
// each cheat command is a thin wrapper that forwards its name to the bridge.
static void VMcmd(int argc, char** argv) {
    int arg = (argc > 1) ? atoi(argv[1]) : 0;
    RunVM(argv[0], arg);
}
static void Cmd_money(int argc, char** argv) {
    RunVM("money", (argc > 1) ? atoi(argv[1]) : 10000);
}
static void Cmd_god(int, char**) {
    int r = SWSE_ScriptToggleGod();
    if (r == 0)      SWSE_ConsolePrint("god: no player yet - load a save first.");
    else             Printf("god mode %s", r == 1 ? "ON" : "OFF");
}
static void Cmd_ctxinfo(int, char**) {
    SWSE_ScriptCtxInfo();
    SWSE_ConsolePrint("ctxinfo written to bin\\swse_log.txt - check for a class MATCH.");
}
static void Cmd_vcall(int argc, char** argv) {
    if (argc < 2) { SWSE_ConsolePrint("usage: vcall <slot#> [arg0] [arg1]  (run ctxinfo first)"); return; }
    int slot = atoi(argv[1]);
    int a0 = (argc > 2) ? atoi(argv[2]) : 0;
    int a1 = (argc > 3) ? atoi(argv[3]) : 0;
    int r = SWSE_ScriptVCall(slot, argc - 2, a0, a1);
    if (r == 1)      Printf("vcall %d: done (see log for return value)", slot);
    else if (r == 0) NoContext("vcall", false);
    else             Printf("vcall %d: faulted - probably wrong slot for this object", slot);
}
static bool ContainsCI(const char* hay, const char* needle) {
    if (!needle || !*needle) return true;
    for (const char* h = hay; *h; h++) {
        const char *a = h, *b = needle;
        while (*a && *b && (*a | 0x20) == (*b | 0x20)) { a++; b++; }
        if (!*b) return true;
    }
    return false;
}

// artifact list, extracted from game data (swse/research/artifacts.txt)
static const char* kArtifacts[] = {
    "activatehivequeen","activatehivequeencharged","ammobaglarge","ammobagmedium",
    "ammobagsmall","attracterbolamite","attracterchipmunk","attracterfuzzle",
    "attracterimmobilizeskunkbomb","attracterthudslug","binoculars","breederbag",
    "damagearmadillo","damagearmadilloloader","damagearmadilloloaderextender",
    "damagebeegun","damagebeegunextendersmall","damageboombatqueen","damagedynamite",
    "damagedynamiteextender","damagedynamiteextenderloader","damageriotslug",
    "damagestingbee","endurancemaxboost1","endurancemaxboost2","endurancemaxboost3",
    "enduranceregenboost1","enduranceregenboost2","immobilizebolablast",
    "immobilizeskunkbomb","immobilizeskunkbombextender","immobilizesparkstunkz",
    "immobilizespiderbola","immobilizespiderbolaextendermedium",
    "immobilizespiderbolaextendersmall","microphone","mongoriverpass",
    "sendtochipmunk","sendtochipmunkextender","sendtohowlerpunk","sniperdart",
    "steefarmorheavy","steefarmorlight","steefarmormedium","steefarmornone",
    "strangerarmorbrassknuckles","strangerarmorsteelknuckles","surgerybid",
    "trapfuzzle","trapfuzzleloader","trapfuzzleloaderextender","trapfuzzlerabid",
};
static const int N_ARTIFACTS = sizeof(kArtifacts)/sizeof(kArtifacts[0]);

// --- artifact hunt: snapshot memory, buy one, diff to find the flag ---
static void Cmd_dumpaddr(int argc, char** argv) {
    if (argc < 2) { SWSE_ConsolePrint("usage: dumpaddr <hexAddress> [dwords]"); return; }
    unsigned a = strtoul(argv[1], nullptr, 16);
    int n = (argc > 2) ? atoi(argv[2]) : 24;
    if (SWSE_DumpAddr(a, n)) Printf("dumped %08X to the log", a);
    else SWSE_ConsolePrint("address out of range");
}
// ---- the store's own grant path (found via the moolah watchpoint) ----
static void Cmd_grant(int argc, char** argv) {
    if (argc < 2) { SWSE_ConsolePrint("usage: grant <artifact|prefs path> [qty]"); return; }
    int qty = (argc > 2) ? atoi(argv[2]) : 1;
    if (qty < 1) qty = 1;
    char msg[256] = {0};
    SWSE_GrantItem(argv[1], qty, msg, sizeof(msg));
    SWSE_ConsolePrint(msg);
}
static void Cmd_giveweapon(int argc, char** argv) {
    if (argc < 2) {
        SWSE_ConsolePrint("usage: giveweapon <name>   e.g. giveweapon crossbowupgrade");
        return;
    }
    char msg[256] = {0};
    SWSE_GiveWeapon(argv[1], msg, sizeof(msg));
    SWSE_ConsolePrint(msg);
}
static const char* kLevels[] = {
    "lm_level_00", "lm_level_01", "lm_level_02", "lm_level_02a",
    "lm_level_03", "lm_level_04", "lm_level_05", "lm_level_06",
};
static void Cmd_levels(int, char**) {
    SWSE_ConsolePrint("levels (use: warp <name>, or warp 4 for lm_level_04):");
    char row[160] = {0};
    for (int i = 0; i < 8; i++) {
        lstrcatA(row, kLevels[i]); lstrcatA(row, "  ");
        if (i % 4 == 3) { SWSE_ConsolePrint(row); row[0] = 0; }
    }
    if (row[0]) SWSE_ConsolePrint(row);
    SWSE_ConsolePrint("region_04 is where the crossbow upgrade prefs live.");
}
// Report the camera the game is actually using. Exact near/far/FOV is what
// any screen-space effect needs to linearise the depth buffer correctly, and
// it is useful on its own for FOV and culling work.
static void Cmd_proj(int argc, char** argv) {
    if (argc > 1 && !lstrcmpiA(argv[1], "consts")) {
        if (argc > 2) {
            unsigned pid = (unsigned)strtoul(argv[2], nullptr, 10);
            int rows = (argc > 3) ? atoi(argv[3]) : 190;
            SWSE_WindProgConstsOf(pid, rows);
            SWSE_ConsolePrint("single-program dump written to swse_log.txt");
            return;
        }
        SWSE_WindProgConsts();
        SWSE_ConsolePrint("constant dump written to swse_log.txt");
        return;
    }
    if (argc > 1 && !lstrcmpiA(argv[1], "why")) {
        SWSE_WindClipCameraWhy();
        SWSE_ConsolePrint("clipcam diagnostics written to swse_log.txt");
        return;
    }
    // 'proj scan' lists every frustum candidate; plain 'proj' reports the camera.
    if (argc > 1 && !lstrcmpiA(argv[1], "scan")) {
        unsigned a[64]; float n[64], f[64], v[64];
        int c = SWSE_ScanCameraFrustum(a, n, f, v, 64);
        Printf("%d frustum candidate(s)%s", c, c >= 64 ? " (AT THE CAP)" : "");
        for (int i = 0; i < c && i < 12; i++)
            Printf("  %08X  near=%d.%03d far=%d.%03d fov=%d.%02d deg", a[i],
                   (int)n[i], (int)(n[i] * 1000) % 1000,
                   (int)f[i], (int)(f[i] * 1000) % 1000,
                   (int)(v[i] * 57.2957795f), (int)(v[i] * 5729.57795f) % 100);
        return;
    }
    float n = 0, f = 0, fov = 0;
    if (!SWSE_SceneProjection(&n, &f, &fov)) {
        // The GL path finds nothing (this engine feeds matrices to shaders
        // itself), so fall back to locating the frustum struct in memory.
        if (!SWSE_AdoptScannedCamera()) {
            SWSE_ConsolePrint("no camera frustum found (try 'proj scan')");
            return;
        }
        SWSE_SceneProjection(&n, &f, &fov);
    }
    Printf("camera: near=%d.%03d far=%d.%03d fovY=%d.%02d deg",
           (int)n, (int)((n < 0 ? -n : n) * 1000) % 1000,
           (int)f, (int)((f < 0 ? -f : f) * 1000) % 1000,
           (int)fov, (int)(fov * 100) % 100);
    Printf("depth tex id=%u  (0 = none captured)", SWSE_SceneDepthTex());
    // The camera as the DRAWS used it this frame - the active one by
    // construction. Printed alongside the scan pick so a mismatch is visible
    // at a glance; `set cam_draw 1` makes the shader use this instead.
    {
        float dn = 0, df = 0, dv = 0, da = 0; unsigned dp = 0; int dc = 0, dg = 0;
        if (SWSE_WindClipCamera(&dn, &df, &dv, &da, &dp, &dc, &dg))
            Printf("draw-derived : near=%d.%03d far=%d.%03d fovY=%d.%02d deg "
                   "aspect=%d.%02d  (program %u, %s, consensus %d)",
                   (int)dn, (int)(dn * 1000) % 1000,
                   (int)df, (int)(df * 1000) % 1000,
                   (int)dv, (int)(dv * 100) % 100,
                   (int)da, (int)(da * 100) % 100,
                   dp, dc ? "D3D conv" : "GL conv", dg);
        else
            SWSE_ConsolePrint("draw-derived : unavailable (no known world program"
                              " decomposed - walk near foliage first)");
    }
}

// List the game's depth textures and report the NEAREST surface each one
// contains. That is the measurement that matters: a depth texture whose nearest
// value is several units away does not include the first-person weapon, so any
// screen-space effect shades those pixels with the wall behind them, and the
// weapon looks like the background is bleeding through it.
// 'depthtex <id>' pins one; 'depthtex 0' goes back to automatic.
static void Cmd_depthtex(int argc, char** argv) {
    if (argc > 1 && !lstrcmpiA(argv[1], "auto")) {
        SWSE_ForceSceneDepthTex(0);
        if (SWSE_AutoPickDepthTex()) Printf("auto-picked depth texture %u", SWSE_SceneDepthTex());
        else SWSE_ConsolePrint("no depth texture with real geometry found");
        return;
    }
    if (argc > 1) {
        unsigned id = (unsigned)atoi(argv[1]);
        SWSE_ForceSceneDepthTex(id);
        if (id) Printf("scene depth pinned to texture %u", id);
        else    SWSE_ConsolePrint("scene depth back to automatic (last bound)");
        return;
    }
    unsigned ids[16];
    int n = SWSE_DepthTexList(ids, 16);
    if (!n) { SWSE_ConsolePrint("no depth textures seen yet (render a 3D frame first)"); return; }

    float nearZ = 0.5f, farZ = 1000.0f;
    SWSE_SceneProjection(&nearZ, &farZ, 0);
    Printf("%d depth texture(s)   camera near=%d.%03d far=%d", n,
           (int)nearZ, (int)(nearZ*1000)%1000, (int)farZ);

    GLint prevTex = 0;
    glGetIntegerv(0x8069 /*GL_TEXTURE_BINDING_2D*/, &prevTex);
    for (int i = 0; i < n; i++) {
        glBindTexture(GL_TEXTURE_2D, (GLuint)ids[i]);
        GLint tw = 0, th = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
        if (tw <= 0 || th <= 0) { Printf("  tex %u: not a 2D texture here", ids[i]); continue; }
        float* buf = (float*)malloc((size_t)tw * th * sizeof(float));
        if (!buf) { Printf("  tex %u: %dx%d (out of memory)", ids[i], tw, th); continue; }
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, buf);
        float mn = 1.0f;
        int nPix = tw * th;
        // Stride the scan; a full 3360x2100 walk is pointless for a min.
        for (int k = 0; k < nPix; k += 7) {
            float v = buf[k];
            if (v < mn && v > 0.0f) mn = v;      // >0 rejects cleared/unwritten
        }
        free(buf);
        // window depth -> world distance, same formula the shader uses
        float denom = farZ - mn * (farZ - nearZ);
        float dist = (denom > 1e-6f) ? (nearZ * farZ / denom) : farZ;
        Printf("  tex %-3u %4dx%-4d  nearest = %d.%02d units%s", ids[i], tw, th,
               (int)dist, (int)(dist*100)%100,
               (ids[i] == SWSE_SceneDepthTex()) ? "   <- in use" : "");
    }
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    SWSE_ConsolePrint("a nearest of >3 units means the first-person weapon is NOT in it");
}

// Frames rendered, counted in the frame hook. Used to measure the cost of the
// early scene-FBO pass, which runs at supersampled resolution.
static volatile long g_frameCount = 0;
void SWSE_CountFrame() { g_frameCount++; }

static void Cmd_fps(int, char**) {
    static long  prevFrames = 0;
    static DWORD prevTick   = 0;
    long  f = g_frameCount;
    DWORD t = GetTickCount();
    if (prevTick == 0 || t <= prevTick) {
        prevFrames = f; prevTick = t;
        SWSE_ConsolePrint("fps: baseline set - run 'fps' again after a few seconds");
        return;
    }
    DWORD ms = t - prevTick;
    long  df = f - prevFrames;
    prevFrames = f; prevTick = t;
    if (ms == 0) { SWSE_ConsolePrint("fps: no elapsed time"); return; }
    int fps100 = (int)((df * 100000LL) / ms);
    Printf("%d.%02d fps  (%d frames over %d ms)", fps100/100, fps100%100, df, ms);
}

// Log the FBO bind order for a few frames. Used to find where the scene render
// ends and UI compositing begins -- the point our post-process should run at,
// so it stops shading UI pixels with the world depth behind them.
static void Cmd_fbotrace(int argc, char** argv) {
    int n = (argc > 1) ? atoi(argv[1]) : 2;
    if (n < 1) n = 1;
    if (n > 10) n = 10;
    SWSE_TraceFBO(n);
    Printf("tracing FBO binds for %d frame(s) -> bin\\swse_glspy.txt (FBOSEQ)", n);
}

// Promote a fraction of a type's live NPCs into elites, so a level has the
// occasional dangerous one instead of being uniformly converted.
//   npcelite <typeHex|*> <percent> [hp]
static void Cmd_npcelite(int argc, char** argv) {
    if (argc < 3) {
        SWSE_ConsolePrint("usage: npcelite <typeHex|*> <percent> [hp]");
        SWSE_ConsolePrint("  e.g. npcelite 072A64D6 10 10000  - 1 in 10 becomes a heavy");
        SWSE_ConsolePrint("  '*' = any type. Health only: hurtReaction is per-TYPE,");
        SWSE_ConsolePrint("  so use 'npchurt <type> 2' if you want them unflinchable too.");
        return;
    }
    unsigned hash = (!lstrcmpA(argv[1], "*")) ? 0u : (unsigned)strtoul(argv[1], 0, 16);
    int pct = atoi(argv[2]);
    float hp = (argc > 3) ? (float)atof(argv[3]) : 10000.0f;
    char msg[220] = {0};
    SWSE_MakeElites(hash, pct, hp, msg, sizeof(msg));
    SWSE_ConsolePrint(msg);
}

// General memory inspector. Every structure we have reverse engineered so far
// (skeletons, poses, NPC records, the camera frustum) started with someone
// guessing a layout; being able to look at raw memory as hex, float and ASCII
// at once turns that guessing into reading. Deliberately generic - no
// knowledge of any particular struct - so it stays useful for the next one.
//   peek <hexaddr> [dwords]
static void Cmd_peek(int argc, char** argv) {
    if (argc < 2) { SWSE_ConsolePrint("usage: peek <hexaddr> [dwords]"); return; }
    unsigned addr = (unsigned)strtoul(argv[1], 0, 16);
    int n = (argc > 2) ? atoi(argv[2]) : 16;
    if (n < 1) n = 1;
    if (n > 128) n = 128;
    for (int i = 0; i < n; i += 4) {
        char line[256];
        int  used = wsprintfA(line, "%08X ", addr + i * 4);
        char asc[32]; int a = 0;
        for (int k = 0; k < 4 && i + k < n; k++) {
            const void* p = (const void*)(uintptr_t)(addr + (i + k) * 4);
            MEMORY_BASIC_INFORMATION mbi;
            bool ok = VirtualQuery(p, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT &&
                      !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
            if (!ok) { used += wsprintfA(line + used, "???????? "); continue; }
            unsigned v = *(const unsigned*)p;
            used += wsprintfA(line + used, "%08X ", v);
            for (int b = 0; b < 4; b++) {
                char ch = (char)((v >> (b * 8)) & 0xFF);
                asc[a++] = (ch >= 0x20 && ch <= 0x7E) ? ch : '.';
            }
        }
        asc[a] = 0;
        // Floats on the same row: struct fields are usually one or the other,
        // and seeing both at once is what makes a layout obvious.
        used += wsprintfA(line + used, " |%s| ", asc);
        for (int k = 0; k < 4 && i + k < n; k++) {
            const void* p = (const void*)(uintptr_t)(addr + (i + k) * 4);
            MEMORY_BASIC_INFORMATION mbi;
            if (!(VirtualQuery(p, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT &&
                  !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))) continue;
            float f = *(const float*)p;
            // Only print plausible magnitudes; pointers as floats are noise.
            if (f > -1.0e6f && f < 1.0e6f && (f == 0.0f || f > 1.0e-6f || f < -1.0e-6f))
                used += wsprintfA(line + used, "%d.%03d ", (int)f,
                                  (int)((f < 0 ? -f : f) * 1000) % 1000);
            else
                used += wsprintfA(line + used, "- ");
        }
        SWSE_ConsolePrint(line);
    }
}

// Find every live object of a class, by its vtable pointer.
//
// Bolts stick to characters and follow their limbs, so the engine must record
// which bone each stuck bolt is attached to. A bolt caught mid-flight showed
// 0xFFFFFFFF at +0x7C - the shape of an "unattached" sentinel - so comparing
// flying bolts against stuck ones should expose the field that holds the bone.
// Listing objects by vtable is the general form of that question and is worth
// having permanently: it answers "where are all the X" for any class whose
// vtable we can name.
//   vtscan <hexVtable> [maxOut] [dumpOffset]
static void Cmd_vtscan(int argc, char** argv) {
    if (argc < 2) {
        SWSE_ConsolePrint("usage: vtscan <hexVtable> [max] [fieldOffsetHex]");
        SWSE_ConsolePrint("  lists objects whose first dword is that vtable");
        return;
    }
    unsigned vt   = (unsigned)strtoul(argv[1], 0, 16);
    int      maxN = (argc > 2) ? atoi(argv[2]) : 16;
    int      foff = (argc > 3) ? (int)strtoul(argv[3], 0, 16) : 0x7C;
    if (maxN < 1) maxN = 1;
    if (maxN > 64) maxN = 64;

    int found = 0;
    MEMORY_BASIC_INFORMATION mbi;
    for (unsigned char* p = (unsigned char*)0x10000000;
         p < (unsigned char*)0x40000000 && found < maxN; ) {
        if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
        unsigned char* next = (unsigned char*)mbi.BaseAddress + mbi.RegionSize;
        bool usable = (mbi.State == MEM_COMMIT) &&
                      !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                      (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE |
                                      PAGE_READONLY  | PAGE_EXECUTE_READ));
        if (usable) {
            __try {
                unsigned* w = (unsigned*)mbi.BaseAddress;
                SIZE_T n = mbi.RegionSize / 4;
                for (SIZE_T i = 0; i + 64 < n && found < maxN; i++) {
                    if (w[i] != vt) continue;
                    unsigned obj = (unsigned)(uintptr_t)(w + i);
                    int  fv = *(int*)(obj + foff);
                    Printf("  %08X  +%02X = %d (%08X)", obj, foff, fv, (unsigned)fv);
                    // Candidate world positions: three consecutive floats in a
                    // plausible level range. The bolt's position at impact IS
                    // the impact point, which is all the bone search needs -
                    // no need to find where the engine stores the bone itself.
                    const float* f = (const float*)obj;
                    for (int o = 0; o < 40; o++) {
                        float a = f[o], b2 = f[o+1], c2 = f[o+2];
                        if (a != a || b2 != b2 || c2 != c2) continue;      // NaN
                        if (a == 0.0f && b2 == 0.0f && c2 == 0.0f) continue;
                        bool ok = true;
                        float v[3] = { a, b2, c2 };
                        for (int k = 0; k < 3; k++) {
                            float m = v[k] < 0 ? -v[k] : v[k];
                            if (!(m < 4000.0f) || (m > 0.0f && m < 0.001f)) ok = false;
                        }
                        if (!ok) continue;
                        Printf("      +%02X pos? %d.%02d %d.%02d %d.%02d", o * 4,
                               (int)a,  ((int)(a  < 0 ? -a  : a ) * 100) % 100,
                               (int)b2, ((int)(b2 < 0 ? -b2 : b2) * 100) % 100,
                               (int)c2, ((int)(c2 < 0 ? -c2 : c2) * 100) % 100);
                        o += 2;
                    }
                    found++;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        if (next <= p) break;
        p = next;
    }
    Printf("%d object(s) with vtable %08X", found, vt);
}

// In-engine screenshot. Works while the game is unfocused or occluded, which
// no outside-the-process capture can do - and unfocused is exactly when it is
// needed, since AgentDebugMode exists so the user can work in another app.
static void JoinArgs(int argc, char** argv, int from, char* out, int outLen);   // below
static void Cmd_snap(int argc, char** argv) {
    char path[MAX_PATH];
    if (argc > 1 && argv[1][0]) {
        // The whole rest of the line, not argv[1]: the tokenizer splits on
        // spaces, so `snap C:\Users\me\New folder\x.tga` saved to "C:\Users\
        // me\New". Quotes around the path are taken off.
        JoinArgs(argc, argv, 1, path, MAX_PATH);
        int n = lstrlenA(path);
        if (n >= 2 && path[0] == '"' && path[n - 1] == '"') {
            memmove(path, path + 1, n - 2);
            path[n - 2] = 0;
        }
    } else {
        GetModuleFileNameA(GetModuleHandleA(NULL), path, MAX_PATH);
        char* slash = strrchr(path, '\\');
        if (slash) *slash = 0;
        lstrcatA(path, "\\swse_snap.tga");
    }
    SWSE_GfxRequestSnapshot(path);
    Printf("snapshot queued -> %s", path);
}

// Dump every shader program the game has built. The number that matters is
// vertex programs using fixed-function matrix state: if that is non-zero, a
// modelview shear reaches the vertices and foliage wind needs no shader
// rewrite. Serviced at the next swap, so results are reported on a second call.
static void Cmd_shaderdump(int argc, char** argv) {
    int glslP = 0, glslS = 0, arb = 0, fixedM = 0, done = 0;
    SWSE_ShaderDumpStats(&glslP, &glslS, &arb, &fixedM, &done);
    if (argc > 1 && !lstrcmpiA(argv[1], "stats")) {
        if (done == 0)  { Printf("no dump has completed yet - run 'shaderdump'"); return; }
        if (done < 0)   { Printf("last dump FAULTED"); return; }
        Printf("glsl programs %d (%d shaders), arb programs %d", glslP, glslS, arb);
        Printf("vertex programs using fixed-function matrix state: %d", fixedM);
        Printf(fixedM > 0 ? "  -> modelview shear WILL reach vertices"
                          : "  -> shear will NOT work; wind needs shader injection");
        return;
    }
    char path[MAX_PATH];
    if (argc > 1 && argv[1][0]) {
        lstrcpynA(path, argv[1], MAX_PATH);
    } else {
        path[0] = 0;   // shaderspy picks the default next to the exe
    }
    SWSE_ShaderDumpRequest(path[0] ? path : NULL);
    Printf("shader dump queued - run 'shaderdump stats' in a second for the verdict");
}

// Is every feature actually doing work? Runs automatically after a level
// loads; this is the manual trigger.
static void Cmd_selftest(int, char**) { SWSE_SelfTestRun(); }

// Where a stutter came from. Answers the only question that matters first -
// is it SWSE at all - then which subsystem.
static void Cmd_perf(int argc, char** argv) {
    double lo = 0, wo = 0, lf = 0, wf = 0; int st = 0;
    SWSE_FramePerf(&lo, &wo, &lf, &wf, &st);
    Printf("frames over 80ms : %d", st);
    Printf("worst frame      : %d ms (SWSE work worst %d ms)", (int)wf, (int)wo);
    Printf("last frame       : %d ms (SWSE work %d ms)", (int)lf, (int)lo);
    Printf(wo > 40.0 ? "  -> SWSE has been slow at least once"
                     : "  -> SWSE per-frame work has stayed small");

    double im = 0, ix = 0, cm = 0, cx = 0; int ws = 0;
    SWSE_WindPerf(&im, &ix, &cm, &cx, &ws);
    Printf("wind inject      : last %d ms, worst %d ms", (int)im, (int)ix);
    Printf("wind revert-check: last %d ms, worst %d ms", (int)cm, (int)cx);
    Printf("wind stalls >30ms: %d", ws);

    double sc = 0, po = 0, pl = 0, scM = 0, poM = 0, plM = 0;
    SWSE_HitReactTickStats(&sc, &po, &pl, &scM, &poM, &plM);
    Printf("hitreact scan    : last %d ms, worst %d ms", (int)sc, (int)scM);
    Printf("hitreact poll    : last %d ms, worst %d ms", (int)po, (int)poM);
    // Native plugins (1.1): each one's frame cost, then its own perf lines.
    SWSE_PluginsPerf(SWSE_ConsolePrint);
    Printf("see swse_log.txt for FRAMESTALL / WINDSTALL lines with timings");
}

// Foliage wind. `wind test` exaggerates the bend so the direction and the
// pivot can be confirmed by eye; the shipping values are far smaller.
static void Cmd_wind(int argc, char** argv) {
    char msg[200] = {0};
    if (argc > 1 && !lstrcmpiA(argv[1], "on"))  { SWSE_WindSet(1, msg, sizeof(msg)); Printf("%s", msg); return; }
    if (argc > 1 && !lstrcmpiA(argv[1], "off")) { SWSE_WindSet(0, msg, sizeof(msg)); Printf("%s", msg); return; }
    if (argc > 1 && !lstrcmpiA(argv[1], "dump")) {
        SWSE_WindDump(msg, sizeof(msg));
        Printf("%s", msg);
        Printf("send that file if plants or nearby geometry look wrong");
        return;
    }
    if (argc > 1 && !lstrcmpiA(argv[1], "push")) {
        if (argc > 2) SWSE_WindPush((float)atof(argv[2]),
                                    (argc > 3) ? (float)atof(argv[3]) : -1.0f,
                                    (argc > 4) ? (float)atof(argv[4]) : -1.0f);
        float a = 0, r = 0, m = 0; SWSE_WindGetPush(&a, &r, &m);
        Printf("push %d.%03d over radius %d.%03d, only plants under %d.%03d tall",
               (int)a, (int)((a - (int)a) * 1000), (int)r, (int)((r - (int)r) * 1000),
               (int)m, (int)((m - (int)m) * 1000));
        Printf("usage: wind push <amount> [radius] [maxheight]; 0 disables");
        return;
    }
    if (argc > 1 && !lstrcmpiA(argv[1], "weight")) {
        if (argc > 2) SWSE_WindWeight((float)atof(argv[2]));
        float w = SWSE_WindGetWeight();
        Printf("weight (stiffness) = %d.%03d", (int)w, (int)((w - (int)w) * 1000));
        Printf("lower = stiffer; caps how far a TALL plant may bend");
        return;
    }
    if (argc > 1 && !lstrcmpiA(argv[1], "save")) {
        if (!SWSE_WindSaveSettings()) {
            Printf("could not save wind.txt - the old file is unchanged (see swse_log.txt)");
            return;
        }
        Printf("saved -> SWSEMods\\SWSE Wind\\wind.txt");
        int inj = 0, fail = 0, on = 0; float cx = 0, cz = 0;
        SWSE_WindStats(&inj, &fail, &on, &cx, &cz);
        Printf(on ? "wind comes back on at launch while the foliage switch is on"
                  : "saved with wind off - it stays off at launch");
        return;
    }
    if (argc > 1 && !lstrcmpiA(argv[1], "axis")) {
        if (argc > 2) SWSE_WindAxis(!lstrcmpiA(argv[2], "z") ? 1 : 0);
        Printf("up axis = %s (re-injecting)", SWSE_WindGetAxis() ? "Z" : "Y");
        return;
    }
    if (argc > 1 && !lstrcmpiA(argv[1], "seed")) {
        if (argc > 2) SWSE_WindSeed(!lstrcmpiA(argv[2], "on") || atoi(argv[2]) != 0);
        // wind.cpp's own finding: the seed is per PLANT (attrib[1] is the
        // same on every vertex of one), and the twisting once blamed on it
        // was the wrong up axis. It used to warn "makes plants twist".
        bool seed = SWSE_WindGetSeed() != 0;
        Printf("per-plant phase seed = %s - %s", seed ? "ON" : "OFF",
               !seed ? "every plant sways in step"
               : SWSE_WindGetAxis() ? "each plant sways on its own phase"
               : "each plant on its own phase; plants that twist mean the axis is wrong, not the seed (`wind axis z`)");
        return;
    }
    if (argc > 1 && !lstrcmpiA(argv[1], "gate")) {
        int on = (argc > 2) ? (!lstrcmpiA(argv[2], "on") || atoi(argv[2]) != 0) : 1;
        SWSE_WindGateEnable(on);
        Printf("per-draw gate %s%s", on ? "ON" : "OFF",
               on ? "" : " - WARNING: shared programs, non-foliage will move");
        return;
    }
    if (argc > 1 && !lstrcmpiA(argv[1], "test")) {
        int on = (argc > 2) ? (atoi(argv[2]) != 0 || !lstrcmpiA(argv[2], "on")) : 1;
        SWSE_WindTest(on);
        Printf("wind test mode %s (exaggerated bend)", on ? "ON" : "OFF");
        return;
    }
    if (argc > 1 && (argv[1][0] == '.' || (argv[1][0] >= '0' && argv[1][0] <= '9'))) {
        float s = (float)atof(argv[1]);
        float sp = (argc > 2) ? (float)atof(argv[2]) : -1.0f;
        SWSE_WindParams(s, sp);
        Printf("wind strength %d.%03d", (int)s, (int)((s - (int)s) * 1000));
        return;
    }
    int inj = 0, fail = 0, on = 0; float cx = 0, cz = 0;
    SWSE_WindStats(&inj, &fail, &on, &cx, &cz);
    float st = 0, sp = 0; SWSE_WindGetParams(&st, &sp);
    Printf("wind          : %s", on ? "ON" : "OFF");
    // The refused count is the character-mesh guard's only visible output. An
    // invisible guard is indistinguishable from an absent one - and the guard
    // is what keeps wind displacement out of skinned character programs while
    // the world-space push touches every eligible plant program.
    int rejSkin = SWSE_WindRejectedSkinned();
    Printf("programs      : %d injected, %d failed, %d refused (character meshes)",
           inj, fail, rejSkin);
    Printf("per-draw gate : %s", SWSE_WindGateEnabled() ? "ON (foliage only)" : "OFF");
    Printf("strength/speed: %d.%03d / %d.%03d",
           (int)st, (int)((st - (int)st) * 1000), (int)sp, (int)((sp - (int)sp) * 1000));
    float wt = SWSE_WindGetWeight();
    Printf("weight (cap)  : %d.%03d - max height that still bends",
           (int)wt, (int)((wt - (int)wt) * 1000));
    float pa = 0, pr = 0, pm = 0; SWSE_WindGetPush(&pa, &pr, &pm);
    Printf("player push   : %d.%03d over radius %d.%03d",
           (int)pa, (int)((pa - (int)pa) * 1000), (int)pr, (int)((pr - (int)pr) * 1000));
    Printf("  pushes only plants under %d.%03d tall (trees stay put)",
           (int)pm, (int)((pm - (int)pm) * 1000));
    {
        float sn = 0, ss = 0, sr = 0, sb = 0, br = 0;
        SWSE_WindSpeedInfo(&sn, &ss, &sr, &sb, &br);
        float now = br * (1.0f + (sb - 1.0f) * sn);
        Printf("sprint push   : %d%% toward a full sprint; radius now %d.%02d",
               (int)(sn * 100.0f), (int)now, (int)(now * 100) % 100);
        Printf("  walk %d.%02d -> sprint %d.%02d   (pushsprint %d.%02d)",
               (int)br, (int)(br * 100) % 100,
               (int)(br * sb), (int)(br * sb * 100) % 100,
               (int)sb, (int)(sb * 100) % 100);
        Printf("  fastest sustained speed %d.%02d vs sprintspeed %d.%02d (tune in wind.txt)",
               (int)ss, (int)(ss * 100) % 100, (int)sr, (int)(sr * 100) % 100);
        Printf("  sustained = held over 4 frames, so a warp cannot inflate it");
    }
    Printf("current bend  : x %d.%03d  z %d.%03d",
           (int)cx, (int)fabs((cx - (int)cx) * 1000),
           (int)cz, (int)fabs((cz - (int)cz) * 1000));
}

// Foliage identification. This exists to VERIFY that the wind effect can tell
// plants from everything else BEFORE anything is displaced - a wind that moves
// the wrong surfaces is far harder to diagnose after the fact than a bind
// count that reads zero.
// `hd` - state of the HD texture pipeline.
//
// A replacement that fails to load falls back to the vanilla texture, which
// looks completely normal in play. That is the failure mode worth having a
// command for: you cannot see it, so it has to be counted and named.
static void Cmd_hd(int argc, char** argv) {
    int avail = 0, loaded = 0, failed = 0;
    SWSE_HdStats(&avail, &loaded, &failed);
    Printf("HD textures: %d installed, %d substituted this session, %d failed",
           avail, loaded, failed);
    if (avail <= 0) {
        Printf("none installed - put <FINGERPRINT>.oft in");
        Printf("  SWSEMods\\SWSE HD\\textures");
        return;
    }
    if (failed > 0) {
        unsigned bad[64];
        int n = SWSE_HdFailures(bad, 64);
        Printf("failed to load (re-pack these):");
        for (int i = 0; i < n; i++) Printf("  %08X", bad[i]);
        if (failed > n) Printf("  ...and %d more", failed - n);
        return;
    }
    if (loaded <= 0)
        Printf("none substituted yet - counts rise as the level uploads textures");
    else
        Printf("no failures.");
}

static void Cmd_foliage(int argc, char** argv) {
    if (argc > 1 && !lstrcmpiA(argv[1], "on")) {
        char msg[200] = {0};
        int ok = SWSE_FoliageTrack(1, msg, sizeof(msg));
        Printf("%s", msg[0] ? msg : (ok ? "foliage tracking ON" : "failed"));
        return;
    }
    if (argc > 1 && !lstrcmpiA(argv[1], "scan")) {
        char path[MAX_PATH];
        GetModuleFileNameA(GetModuleHandleA(NULL), path, MAX_PATH);
        char* slash = strrchr(path, '\\');
        if (slash) *slash = 0;
        lstrcatA(path, "\\swse_frame_textures.txt");
        SWSE_FoliageScanRequest(path);
        Printf("scanning one frame -> swse_frame_textures.txt");
        Printf("(needs 'foliage on' - the scan rides the bind hook)");
        return;
    }
    if (argc > 1 && !lstrcmpiA(argv[1], "scanned")) {
        int n = SWSE_FoliageScanDone();
        Printf(n > 0 ? "scan wrote %d texture(s)" : (n < 0 ? "scan FAILED" : "scan still pending"), n);
        return;
    }
    if (argc > 1 && !lstrcmpiA(argv[1], "reload")) {
        int n = SWSE_FoliageReload();
        Printf("reloaded foliage.txt: %d fingerprint(s)", n);
        Printf("new entries apply immediately - no restart needed");
        return;
    }
    if (argc > 1 && !lstrcmpiA(argv[1], "progs")) {
        unsigned p[64];
        int n = SWSE_FoliagePrograms(p, 64);
        if (!n) { Printf("no foliage vertex programs known - run 'foliage scan'"); return; }
        for (int i = 0; i < n; i++) Printf("  vertex program %u", p[i]);
        return;
    }
    int listN = 0, known = 0, binds = 0, peak = 0, hooked = 0;
    SWSE_FoliageStats(&listN, &known, &binds, &peak, &hooked);
    Printf("fingerprints loaded : %d", listN);
    Printf("texture ids flagged : %d  (foliage seen by this level)", known);
    Printf("bind tracking       : %s", hooked ? "ON" : "OFF - run 'foliage on'");
    Printf("foliage binds       : %d last frame, %d peak", binds, peak);
    if (!listN)
        Printf("no list: expected SWSEMods\\SWSE Wind\\foliage.txt");
    else if (hooked && peak == 0)
        Printf("no foliage bound yet - are there plants in view?");
}

// AgentDebugMode: one switch that makes the game co-operative to work on.
// OFF (default) the game behaves exactly as shipped - every hook involved is
// installed but inert, so there is no behaviour to regress. ON, the game keeps
// simulating while alt-tabbed AND gives up the cursor and the keyboard, so the
// desktop stays usable. The first version only did the first half and trapped
// the pointer inside the game window.
static void Cmd_agentdebug(int argc, char** argv) {
    int on = 1;
    if (argc > 1) on = (!lstrcmpiA(argv[1], "on") || atoi(argv[1]) != 0);
    char msg[160] = {0};
    if (!SWSE_AgentDebugMode(on, msg, sizeof(msg))) { SWSE_ConsolePrint(msg); return; }
    SWSE_ConsolePrint(msg);
    // Report the REAL focus state. A stale value here is what made the game
    // ignore the user's own keyboard and mouse, so it must be observable
    // rather than assumed.
    Printf("real focus: %s (input %s)",
           SWSE_InputReallyFocused() ? "FOCUSED" : "elsewhere",
           SWSE_InputReallyFocused() ? "goes to the game" : "stays with the desktop");
    if (on) SWSE_ConsolePrint("game keeps running unfocused; mouse+keyboard stay yours");
}

// Additive hit reactions. Toggleable like the RTGI and the NPC tuning.
//   hitreact on|off          install/enable
//   hitreact test [bone]     fire a reaction on every character (visual check)
//   hitreact <strength> <ms> tune
static void Cmd_hitreact(int argc, char** argv) {
    if (argc < 2) {
        Printf("hit reactions: %s", SWSE_HitReactEnabled() ? "ON" : "off");
        // The counters are the actual proof the hook works. A screenshot diff
        // cannot separate "we moved a bone" from "the NPC took a step".
        unsigned calls = 0, writes = 0, skel = 0; int bones = 0;
        SWSE_HitReactStats(&calls, &writes, &skel, &bones);
        Printf("  hook calls %u, bones written %u", calls, writes);
        Printf("  last pose: skeleton %08X, %d bones", skel, bones);
        unsigned polled = 0, seen = 0;
        SWSE_HitReactWatchStats(&polled, &seen);
        Printf("  damage watch %s: %u polls, %u hits seen",
               SWSE_HitReactWatching() ? "ON" : "off", polled, seen);
        unsigned mt = 0, rj = 0, nm = 0, tf = 0, br = 0;
        SWSE_HitReactMatchStats(&mt, &rj, &nm, &tf, &br);
        Printf("  match %u, posReject %u, noMatrix %u, torsoFail %u, boneRange %u",
               mt, rj, nm, tf, br);
        unsigned psame = 0, pdiff = 0;
        SWSE_HitReactPoseStats(&psame, &pdiff);
        Printf("  pose buffer: %u persisted, %u rebuilt  (%s)", psame, pdiff,
               (psame > pdiff) ? "PERSISTS -> use delta" : "REBUILT -> use absolute");
        double ts = 0, tp = 0, tpl = 0, tsm = 0, tpm = 0, tplm = 0;
        SWSE_HitReactTickStats(&ts, &tp, &tpl, &tsm, &tpm, &tplm);
        Printf("  tick ms: scan %d.%02d (max %d.%02d), player %d.%02d (max %d.%02d), poll %d.%02d (max %d.%02d)",
               (int)ts, ((int)(ts*100))%100, (int)tsm, ((int)(tsm*100))%100,
               (int)tpl, ((int)(tpl*100))%100, (int)tplm, ((int)(tplm*100))%100,
               (int)tp, ((int)(tp*100))%100, (int)tpm, ((int)(tpm*100))%100);
        unsigned ir = 0, ifail = 0; int lb = -1; float ld = 0;
        SWSE_HitReactImpactStats(&ir, &ifail, &lb, &ld);
        Printf("  impact-resolved %u (failed %u), last bone %d at %d.%02d units",
               ir, ifail, lb, (int)ld, ((int)(ld * 100)) % 100);
        unsigned bolts = 0;
        SWSE_BoltStats(&bolts);
        Printf("  bolt positions recorded: %u", bolts);
        unsigned faults = 0;
        SWSE_HitReactFaults(&faults);
        if (faults) Printf("  *** hook FAULTED %u time(s) - reactions auto-disabled ***", faults);
        SWSE_ConsolePrint("usage: hitreact on|off | hitreact test [bone] | hitreact <strength> <ms>");
        return;
    }
    if (!lstrcmpiA(argv[1], "on")) {
        char msg[200] = {0};
        if (!SWSE_HitReactInstall(msg, sizeof(msg))) { SWSE_ConsolePrint(msg); return; }
        SWSE_HitReactEnable(1);
        // Projectile tracking supplies the impact point, without which every
        // reaction lands on the torso regardless of where the shot hit.
        char bmsg[160] = {0};
        SWSE_BoltHookInstall(bmsg, sizeof(bmsg));
        SWSE_ConsolePrint(bmsg);
        // Turning the feature on should make shooting people feel different;
        // requiring a second command for that is a trap. 'watch off' remains
        // available for isolating the hook during debugging.
        SWSE_HitReactWatch(1);
        SWSE_ConsolePrint(msg);
        SWSE_ConsolePrint("enabled + watching for damage - shoot someone");
        return;
    }
    if (!lstrcmpiA(argv[1], "impact")) {
        // Fire an impact-resolved reaction at a world point. Lets the
        // nearest-bone path be tested before the projectile hook exists:
        // aim at a body part, read off which bone it picked.
        //   hitreact impact <x> <y> <z>     explicit point
        //   hitreact impact head|chest|legs  relative to the nearest character
        float p[3] = { 0, 0, 0 };
        if (argc >= 5) {
            p[0] = (float)atof(argv[2]); p[1] = (float)atof(argv[3]);
            p[2] = (float)atof(argv[4]);
        } else if (SWSE_PosGet(p) == 1) {
            // Default: the player's own position, raised to a body height.
            float lift = 12.0f;
            if (argc > 2) {
                if (!lstrcmpiA(argv[2], "head"))       lift = 20.0f;
                else if (!lstrcmpiA(argv[2], "chest")) lift = 13.0f;
                else if (!lstrcmpiA(argv[2], "legs"))  lift = 4.0f;
                else lift = (float)atof(argv[2]);
            }
            p[2] += lift;
        } else { SWSE_ConsolePrint("no position"); return; }
        int ok = SWSE_HitReactImpact(p[0], p[1], p[2], 1.0f, 0.0f, 0.0f, 0.0f);
        Printf("impact at %d %d %d (%s)", (int)p[0], (int)p[1], (int)p[2],
               ok ? "queued" : "no free slot");
        return;
    }
    if (!lstrcmpiA(argv[1], "layout")) {
        if (argc > 2 && !lstrcmpiA(argv[2], "show")) {
            unsigned off = 0, base = 0; int count = 0, bones = 0;
            SWSE_HitReactLayoutGet(-1, 0, &base, &count, &bones);
            if (!count) { SWSE_ConsolePrint("no layout capture yet"); return; }
            Printf("pose %08X, %d bones, %d unit quaternions found", base, bones, count);
            unsigned prev = 0;
            for (int i = 0; i < count; i++) {
                if (!SWSE_HitReactLayoutGet(i, &off, 0, 0, 0)) break;
                if (i == 0) Printf("  [%2d] +0x%X   (first = orientation offset)", i, off);
                else        Printf("  [%2d] +0x%X   stride 0x%X", i, off, off - prev);
                prev = off;
            }
            return;
        }
        SWSE_HitReactLayoutRequest();
        SWSE_ConsolePrint("scanning next character pose - 'hitreact layout show'");
        return;
    }
    if (!lstrcmpiA(argv[1], "wpos")) {
        // Composed bone positions for one character. If the composition is
        // right these form a character-shaped cloud: a couple of units wide and
        // roughly a body tall, centred on the character. Garbage means the
        // matrix convention is transposed.
        int minB = (argc > 2) ? atoi(argv[2]) : 30;
        SWSE_HitReactWposRequest(minB);
        SWSE_ConsolePrint("snapshot requested - run 'hitreact wpos show'");
        return;
    }
    if (!lstrcmpiA(argv[1], "wposshow") ||
        (!lstrcmpiA(argv[1], "wpos") && argc > 2 && !lstrcmpiA(argv[2], "show"))) {
        unsigned skel = 0; int count = 0; float p[3];
        SWSE_HitReactWposGet(-1, 0, &skel, &count);
        if (!count) { SWSE_ConsolePrint("no snapshot yet"); return; }
        float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
        for (int i = 0; i < count; i++) {
            if (!SWSE_HitReactWposGet(i, p, 0, 0)) break;
            for (int k = 0; k < 3; k++) {
                if (p[k] < lo[k]) lo[k] = p[k];
                if (p[k] > hi[k]) hi[k] = p[k];
            }
            if (i < 12) Printf("  bone %2d  %d %d %d", i, (int)p[0], (int)p[1], (int)p[2]);
        }
        Printf("skeleton %08X, %d bones", skel, count);
        Printf("extent: x %d..%d  y %d..%d  z %d..%d",
               (int)lo[0], (int)hi[0], (int)lo[1], (int)hi[1], (int)lo[2], (int)hi[2]);
        // Raw inputs, to tell bad math from bad pointers.
        float rp[3], rq[4], ra[16];
        if (SWSE_HitReactWposRaw(rp, rq, ra)) {
            Printf("raw bone0 pos %d/1000 %d/1000 %d/1000", (int)(rp[0]*1000),
                   (int)(rp[1]*1000), (int)(rp[2]*1000));
            Printf("raw bone0 quat %d %d %d %d /1000", (int)(rq[0]*1000),
                   (int)(rq[1]*1000), (int)(rq[2]*1000), (int)(rq[3]*1000));
            Printf("a4 row0 %d %d %d /1000", (int)(ra[0]*1000), (int)(ra[1]*1000), (int)(ra[2]*1000));
            Printf("a4 row1 %d %d %d /1000", (int)(ra[4]*1000), (int)(ra[5]*1000), (int)(ra[6]*1000));
            Printf("a4 row2 %d %d %d /1000", (int)(ra[8]*1000), (int)(ra[9]*1000), (int)(ra[10]*1000));
            Printf("a4 trans %d %d %d", (int)ra[12], (int)ra[13], (int)ra[14]);
        }
        unsigned head[24], addr = 0;
        if (SWSE_HitReactWposHead(head, &addr)) {
            Printf("localPose %08X - use 'peek %08X 24' for float view", addr, addr);
            for (int r = 0; r < 6; r++)
                Printf("  +%02X: %08X %08X %08X %08X", r*16,
                       head[r*4+0], head[r*4+1], head[r*4+2], head[r*4+3]);
        }
        return;
    }
    if (!lstrcmpiA(argv[1], "offsets")) {
        // hitreact offsets <base> <stride> <pos> <orient>   (hex, no 0x)
        // Two candidates: the measured 10/30/0C/18, and the original
        // 0/44/04/10 which - although wrong by every structural check - is what
        // actually deformed characters on screen.
        if (argc >= 6) {
            SWSE_HitReactSetOffsets((int)strtoul(argv[2], 0, 16),
                                    (int)strtoul(argv[3], 0, 16),
                                    (int)strtoul(argv[4], 0, 16),
                                    (int)strtoul(argv[5], 0, 16));
        }
        int b = 0, st = 0, p = 0, o = 0;
        SWSE_HitReactGetOffsets(&b, &st, &p, &o);
        Printf("offsets: base 0x%X stride 0x%X pos 0x%X orient 0x%X", b, st, p, o);
        return;
    }
    if (!lstrcmpiA(argv[1], "mode")) {
        if (argc > 2) SWSE_HitReactApplyMode(!lstrcmpiA(argv[2], "delta"));
        Printf("apply mode: %s", SWSE_HitReactGetApplyMode() ? "delta" : "absolute");
        SWSE_ConsolePrint("usage: hitreact mode delta|absolute");
        return;
    }
    if (!lstrcmpiA(argv[1], "chainpath")) {
        // Prefer the biggest probed skeleton - the last one posed is usually a
        // prop or a piece of ammo, not the character we care about.
        int path[16]; unsigned skel = 0; int bc = 0;
        unsigned ps = 0; int pb = 0, pv = 0; float pp[3]; unsigned pa4 = 0;
        for (int i = 0; i < 24; i++) {
            if (!SWSE_HitReactProbeGet(i, &ps, &pb, &pa4, pp, &pv)) break;
            if (pb > bc) { bc = pb; skel = ps; }
        }
        int n = SWSE_HitReactChainPath(path, 16, &skel, &bc);
        if (!n) { SWSE_ConsolePrint("no chain (no skeleton seen yet?)"); return; }
        char line[200]; int used = wsprintfA(line, "skel %08X (%d bones) chain:", skel, bc);
        for (int i = 0; i < n; i++) used += wsprintfA(line + used, " %d", path[i]);
        SWSE_ConsolePrint(line);
        return;
    }
    if (!lstrcmpiA(argv[1], "save")) {
        SWSE_HitReactSaveSettings();
        Printf("saved -> SWSEMods\\SWSE Combat\\hitreact.txt");
        Printf("hit reactions will come back on automatically at launch");
        return;
    }
    if (!lstrcmpiA(argv[1], "ease")) {
        if (argc >= 4)      SWSE_HitReactEase((float)atof(argv[2]), (float)atof(argv[3]));
        else if (argc == 3) SWSE_HitReactEase((float)atof(argv[2]), -1.0f);
        float a = 0, o = 0;
        SWSE_HitReactEaseGet(&a, &o);
        Printf("ease: attack %d%% of duration, overshoot %d%%",
               (int)(a * 100), (int)(o * 100));
        SWSE_ConsolePrint("usage: hitreact ease <attack 0..0.8> [overshoot 0..0.6]");
        SWSE_ConsolePrint("  attack 0 = instant snap (the old behaviour)");
        return;
    }
    if (!lstrcmpiA(argv[1], "limbmass")) {
        if (argc > 2) SWSE_HitReactLimbMass(atoi(argv[2]));
        Printf("limb mass %d - a reaction climbs to a bone with at least this "
               "many bones below it", SWSE_HitReactGetLimbMass());
        SWSE_ConsolePrint("usage: hitreact limbmass <n>  (1 = use the exact hit bone)");
        return;
    }
    if (!lstrcmpiA(argv[1], "armdamp")) {
        if (argc > 2) SWSE_HitReactArmDamp((float)atof(argv[2]));
        float d = SWSE_HitReactGetArmDamp();
        Printf("arm damping %d/100 - %d%% of the body's rotation is cancelled "
               "at the arms", (int)(d * 100), (int)(d * 100));
        SWSE_ConsolePrint("usage: hitreact armdamp <0..1>  (1 = weapon holds station)");
        return;
    }
    if (!lstrcmpiA(argv[1], "chain")) {
        if (argc >= 4) SWSE_HitReactChain(atoi(argv[2]), (float)atof(argv[3]));
        else if (argc == 3) SWSE_HitReactChain(atoi(argv[2]), -1.0f);
        int l = 0; float f = 0;
        SWSE_HitReactChainGet(&l, &f);
        Printf("chain: %d link(s), falloff %d/100 per link", l, (int)(f * 100));
        SWSE_ConsolePrint("usage: hitreact chain <links> [falloff]  (1 = single bone)");
        return;
    }
    if (!lstrcmpiA(argv[1], "freeze")) {
        int on = (argc > 2) ? (!lstrcmpiA(argv[2], "on") || atoi(argv[2]) != 0) : 1;
        SWSE_HitReactFreeze(on);
        Printf("animation freeze %s - %s", on ? "ON" : "off",
               on ? "pose held; only hit reactions can move anyone"
                  : "animation resumed");
        return;
    }
    if (!lstrcmpiA(argv[1], "tpose")) {
        int on = (argc > 2) ? (!lstrcmpiA(argv[2], "on") || atoi(argv[2]) != 0) : 1;
        SWSE_HitReactTPose(on);
        Printf("T-pose %s - animation flattened, so ANY movement is a reaction",
               on ? "ON" : "off");
        return;
    }
    if (!lstrcmpiA(argv[1], "minbones")) {
        if (argc > 2) SWSE_HitReactMinBones(atoi(argv[2]));
        Printf("minbones %d (below this = prop/ammo, never perturbed)",
               SWSE_HitReactGetMinBones());
        return;
    }
    if (!lstrcmpiA(argv[1], "bolts")) {
        // Where the tracked projectiles actually are. If these do not sit near
        // the player while being shot, the position offset in the bolt object
        // is wrong and impact matching cannot work.
        float p[3]; unsigned age = 0; int n = 0;
        float me[3] = { 0, 0, 0 };
        int haveMe = SWSE_PlayerGet(0x24, &me[0], &me[1], &me[2]);
        if (haveMe) Printf("player at %d %d %d", (int)me[0], (int)me[1], (int)me[2]);
        for (int i = 0; i < 16; i++) {
            if (!SWSE_BoltRecent(i, p, &age)) break;
            if (haveMe) {
                float dx = p[0]-me[0], dy = p[1]-me[1], dz = p[2]-me[2];
                int d = (int)sqrt((double)(dx*dx+dy*dy+dz*dz));
                Printf("  %2d  %d %d %d   %ums ago   %d units away",
                       i, (int)p[0], (int)p[1], (int)p[2], age, d);
            } else {
                Printf("  %2d  %d %d %d   %ums ago", i, (int)p[0], (int)p[1], (int)p[2], age);
            }
            n++;
        }
        if (!n) SWSE_ConsolePrint("no bolt positions recorded");
        return;
    }
    if (!lstrcmpiA(argv[1], "hits")) {
        // What each weapon actually deals. Tuning "weak weapons feel weak"
        // against guessed damage numbers does not work.
        // Optional window: 'hitreact hits 20' shows only the last 20 seconds,
        // which is how you answer "how many shots landed and where".
        unsigned windowMs = (argc > 2) ? (unsigned)(atoi(argv[2]) * 1000) : 0;
        float dmg = 0, sc = 0; unsigned age = 0; int n = 0, unresolved = 0;
        int bone = -1, bones = 0; unsigned skel = 0;
        for (int i = 0; i < 24; i++) {
            if (!SWSE_HitReactHitLog(i, &dmg, &sc, &age, &bone, &bones, &skel)) break;
            if (windowMs && age > windowMs) break;
            if (bone < 0) {
                unresolved++;
                Printf("  %2d  %d.%02d dmg  scale %d.%02d  BONE UNRESOLVED  %ums", i,
                       (int)dmg, ((int)(dmg*100))%100, (int)sc, ((int)(sc*100))%100, age);
            } else {
                Printf("  %2d  %d.%02d dmg  scale %d.%02d  bone %d/%d  skel %08X  %ums", i,
                       (int)dmg, ((int)(dmg*100))%100, (int)sc, ((int)(sc*100))%100,
                       bone, bones, skel, age);
            }
            n++;
        }
        if (!n) { SWSE_ConsolePrint("no hits recorded yet"); return; }
        Printf("%d hit(s)%s, %d never reached a posed character",
               n, windowMs ? " in window" : "", unresolved);
        return;
    }
    if (!lstrcmpiA(argv[1], "ratio")) {
        if (argc > 2) SWSE_HitReactDamageRatio(!lstrcmpiA(argv[2], "on") || atoi(argv[2]) != 0);
        Printf("damage measured as %s",
               SWSE_HitReactDamageRatioOn()
                 ? "%% of the victim's max health (one curve fits every character)"
                 : "raw damage (needs per-character tuning)");
        return;
    }
    if (!lstrcmpiA(argv[1], "curve")) {
        if (argc >= 5) {
            SWSE_HitReactCurve((float)atof(argv[2]), (float)atof(argv[3]),
                               (float)atof(argv[4]));
        }
        float f = 0, p = 0, m = 0;
        SWSE_HitReactCurveGet(&f, &p, &m);
        Printf("curve: scale = %d/1000 + dmg * %d/1000, max %d/1000",
               (int)(f * 1000), (int)(p * 1000), (int)(m * 1000));
        SWSE_ConsolePrint("usage: hitreact curve <floor> <perDamage> <max>");
        return;
    }
    if (!lstrcmpiA(argv[1], "here")) {
        // Position-matched reaction at the player's own feet. The player is
        // always being posed, so this isolates "position matching + torso
        // lookup work" from "that NPC was too far away to be animated".
        float p[3] = { 0, 0, 0 };
        if (SWSE_PosGet(p) != 1) { SWSE_ConsolePrint("no player position"); return; }
        float radius = (argc > 2) ? (float)atof(argv[2]) : 3.0f;
        int n = SWSE_HitReactTriggerAt(p[0], p[1], p[2], radius, -1,
                                       1.0f, 0.0f, 0.0f, 0.0f);
        Printf("queued at %d %d %d r=%d (%s)", (int)p[0], (int)p[1], (int)p[2],
               (int)radius, n ? "ok" : "no free slot");
        return;
    }
    if (!lstrcmpiA(argv[1], "watch")) {
        int on = (argc > 2) ? (!lstrcmpiA(argv[2], "on") || atoi(argv[2]) != 0) : 1;
        SWSE_HitReactWatch(on);
        Printf("damage watch %s", on ? "ON" : "off");
        return;
    }
    if (!lstrcmpiA(argv[1], "off")) {
        SWSE_HitReactEnable(0);
        SWSE_HitReactRemove();
        SWSE_ConsolePrint("hit reactions off (the hook stays in place, inert - unpatching is not thread safe)");
        return;
    }
    if (!lstrcmpiA(argv[1], "probe")) {
        // 'hitreact probe' starts capturing; 'hitreact probe show' prints.
        if (argc > 2 && !lstrcmpiA(argv[2], "show")) {
            unsigned skel = 0, a4 = 0; int bones = 0, valid = 0; float p[3];
            int n = 0;
            for (int i = 0; i < 24; i++) {
                if (!SWSE_HitReactProbeGet(i, &skel, &bones, &a4, p, &valid)) break;
                if (valid)
                    Printf("  %2d skel %08X %3d bones  pos %d %d %d",
                           i, skel, bones, (int)p[0], (int)p[1], (int)p[2]);
                else
                    Printf("  %2d skel %08X %3d bones  (no matrix)", i, skel, bones);
                unsigned a5 = 0, a6 = 0, head[8]; int hv = 0;
                if (SWSE_HitReactProbeArgs(i, &a5, &a6, head, &hv)) {
                    if (hv)
                        Printf("       a5 %08X -> %08X %08X %08X %08X %08X %08X  a6 %08X",
                               a5, head[0], head[1], head[2], head[3], head[4], head[5], a6);
                    else
                        Printf("       a5 %08X (unreadable)  a6 %08X", a5, a6);
                }
                n++;
            }
            Printf("%d distinct skeleton(s) captured", n);
            return;
        }
        SWSE_HitReactProbe(1);
        SWSE_ConsolePrint("probing - wait a moment then 'hitreact probe show'");
        return;
    }
    if (!lstrcmpiA(argv[1], "bones")) {
        // 'hitreact bones [n]' names the bones of the nth probed skeleton
        // (default: the one with the most bones, i.e. a full character).
        int want = (argc > 2) ? atoi(argv[2]) : -1;
        unsigned skel = 0, a4 = 0; int bones = 0, valid = 0; float p[3];
        unsigned bestSkel = 0; int bestBones = 0;
        for (int i = 0; i < 24; i++) {
            if (!SWSE_HitReactProbeGet(i, &skel, &bones, &a4, p, &valid)) break;
            if (want >= 0) { if (i == want) { bestSkel = skel; bestBones = bones; break; } }
            else if (bones > bestBones) { bestBones = bones; bestSkel = skel; }
        }
        if (!bestSkel) { SWSE_ConsolePrint("no probe data - run 'hitreact probe' first"); return; }
        Printf("skeleton %08X (%d bones) - 'peek %08X' to inspect", bestSkel, bestBones, bestSkel);
        char msg[200] = {0}; int stride = 0; unsigned bonesPtr = 0;
        int n = SWSE_GrannySkeletonInfo(bestSkel, bestBones, &stride, &bonesPtr,
                                        msg, sizeof(msg));
        SWSE_ConsolePrint(msg);
        if (!n) return;
        for (int b = 0; b < n; b++) {
            char nm[64] = {0}; int parent = -2;
            if (!SWSE_GrannyBoneInfo(bonesPtr, stride, b, nm, sizeof(nm), &parent)) break;
            Printf("  %2d parent %2d  %s", b, parent, nm);
        }
        return;
    }
    if (!lstrcmpiA(argv[1], "test")) {
        // skeleton 0 = any character, so the effect is obvious without needing
        // impact detection wired up yet.
        // 'test all' fires on a spread of bone indices at once. A single bone
        // may be a finger or a prop attachment and move nothing you can see, so
        // this answers "is the effect visible at all" before tuning which bone.
        if (argc > 2 && !lstrcmpiA(argv[2], "all")) {
            int queued = 0;
            for (int b = 0; b < SWSE_MAX_REACTS; b++)
                if (SWSE_HitReactTrigger(0, b, 1.0f, 0.0f, 0.0f)) queued++;
            Printf("queued %d reactions (bones 0-%d)", queued, SWSE_MAX_REACTS - 1);
            return;
        }
        int bone = (argc > 2) ? atoi(argv[2]) : 5;
        int n = SWSE_HitReactTrigger(0, bone, 1.0f, 0.0f, 0.0f);
        Printf("queued reaction on bone %d (%s)", bone, n ? "ok" : "no free slot");
        return;
    }
    float strength = (float)atof(argv[1]);
    int ms = (argc > 2) ? atoi(argv[2]) : 0;
    SWSE_HitReactTune(strength, ms);
    Printf("tuned: strength %d/1000 rad, %d ms", (int)(strength*1000), ms);
}

// Granny pose scanner. Groundwork for additive hit reactions: find the
// per-bone local transforms so a decaying offset can be added to the bone
// nearest an impact. Read-only by design - we have no disassembler to place a
// safe inline hook inside Granny, and guessing at one is what crashed the
// early graphics pass.
static void Cmd_granny(int argc, char** argv) {
    if (argc > 1 && !lstrcmpiA(argv[1], "dump")) {
        if (argc < 3) { SWSE_ConsolePrint("usage: granny dump <address> [bones]"); return; }
        unsigned a = (unsigned)strtoul(argv[2], 0, 16);
        int n = (argc > 3) ? atoi(argv[3]) : 6;
        int stride = (argc > 4) ? atoi(argv[4]) : 0x44;
        char msg[200] = {0};
        SWSE_GrannyDumpBones(a, n, stride, msg, sizeof(msg));
        SWSE_ConsolePrint(msg);
        return;
    }
    // 'wide' opens the scan past the game-heap window, because Granny uses its
    // own allocator; 'strict' re-enables the near-identity scale-shear test.
    // "near <hexaddr> [kb]" restricts the scan to a window around an address -
    // used to find a character's LOCAL pose given its WORLD pose.
    if (argc > 2 && !lstrcmpiA(argv[1], "near")) {
        unsigned base = (unsigned)strtoul(argv[2], 0, 16);
        unsigned kb   = (argc > 3) ? (unsigned)atoi(argv[3]) : 64;
        SWSE_GrannySetNear(base, kb * 1024);
        static unsigned na[64]; static int nc[64];
        int nn = SWSE_GrannyScan(na, nc, 64, (argc > 4) ? atoi(argv[4]) : 30, 0, 0, 0);
        SWSE_GrannySetNear(0, 0);
        Printf("%d transform run(s) within %u KB of %08X", nn, kb, base);
        for (int i = 0; i < nn && i < 16; i++) Printf("  %08X   %d bones", na[i], nc[i]);
        return;
    }
    int minBones = (argc > 1) ? atoi(argv[1]) : 12;
    int wide = 0, strict = 0;
    for (int i = 1; i < argc; i++) {
        if (!lstrcmpiA(argv[i], "wide"))   wide = 1;
        if (!lstrcmpiA(argv[i], "strict")) strict = 1;
    }
    static unsigned addrs[64];
    static int counts[64];
    // mode 1 = hunt 4x4 matrices (64B) instead of granny_transforms (68B)
    int mode = 0;
    for (int i = 1; i < argc; i++) if (!lstrcmpiA(argv[i], "mat")) mode = 1;
    int n = SWSE_GrannyScan(addrs, counts, 64, minBones, wide, strict, mode);
    Printf("%d candidate pose(s) with >=%d bones%s%s%s", n, minBones,
           wide ? " [wide]" : "", strict ? " [strict]" : "",
           n >= 64 ? "  (AT THE CAP)" : "");
    for (int i = 0; i < n && i < 16; i++)
        Printf("  %08X   %d bones", addrs[i], counts[i]);
    if (n) SWSE_ConsolePrint("inspect one with: granny dump <address>");
}

// Animation probe. Groundwork for impact reactions: the engine has no ragdoll,
// but StartCineTorsoAnim plays a TORSO-ONLY animation, which is the right shape
// for a flinch (legs keep walking). The int argument's VM type tag is not known
// yet, hence the optional tag parameter - sweep it to find which one lands.
static void Cmd_anim(int argc, char** argv) {
    if (argc < 2) {
        SWSE_ConsolePrint("usage: anim <torso|endtorso|stop> [n] [tag]");
        SWSE_ConsolePrint("  acts on the NPC nearest you. 'torso' takes an int;");
        SWSE_ConsolePrint("  sweep tag 0..6 to find the type the handler accepts.");
        return;
    }
    int n   = (argc > 2) ? atoi(argv[2]) : 0;
    int tag = (argc > 3) ? atoi(argv[3]) : 0;
    char msg[256] = {0};
    SWSE_AnimVerb(argv[1], n, tag, msg, sizeof(msg));
    SWSE_ConsolePrint(msg);
}

// ---- synthetic input ----------------------------------------------------
// These go through our dinput8 proxy (input.cpp), so they work at the main
// menu and during cutscenes - places where there is no script VM and where
// OS-level injection (SendKeys/keybd_event) fails because the fullscreen
// window does not reliably hold foreground focus.

static void Cmd_key(int argc, char** argv) {
    if (argc < 2) {
        SWSE_ConsolePrint("usage: key <name> [name...]   taps each in sequence");
        SWSE_ConsolePrint("  names: up down left right enter esc space tab a-z 0-9 f1-f12, or 0x1C");
        return;
    }
    if (!SWSE_InputReady())
        SWSE_ConsolePrint("warning: no keyboard device seen yet - run 'inputst'");
    // Space taps out so a menu reads them as separate presses, not one hold.
    int delay = 0, sent = 0;
    char bad[128] = {0};
    for (int i = 1; i < argc; i++) {
        int scan = SWSE_ScanForName(argv[i]);
        if (scan < 0) { if (!bad[0]) lstrcpynA(bad, argv[i], sizeof(bad)); continue; }
        SWSE_QueueKey(scan, delay, 90);
        delay += 220;
        sent++;
    }
    if (bad[0]) Printf("unknown key name: %s", bad);
    if (sent)   Printf("queued %d press(es) over %dms", sent, delay);
}

static void Cmd_inputst(int, char**) {
    char s[320];
    SWSE_InputStatus(s, sizeof(s));
    SWSE_ConsolePrint(s);
    SWSE_InputGuardStatus(s, sizeof(s));
    SWSE_ConsolePrint(s);
}

// Main menu order: NEW GAME / CONTINUE / LOAD GAME / OPTIONS / EXTRAS / QUIT.
// Nothing is highlighted when the menu first appears, so the Nth item needs N
// Downs. Kept generic because the same primitive drives every other menu.
static void MenuPick(int index, const char* what) {
    if (index < 1) index = 1;
    int delay = 600;                    // let the menu settle before the first tap
    for (int i = 0; i < index; i++) { SWSE_QueueKey(0xD0, delay, 90); delay += 260; }
    SWSE_QueueKey(0x1C, delay + 200, 90);
    Printf("menu: %d x Down then Enter (%s)", index, what);
}

// menu list | fs <screen> <cmd> [arg] | continue | skip | resume | <n>
// The words drive the game's Flash menus directly, the way a button press
// does (menu.h) - no input, so they work with the game behind other windows.
// A number keeps the old key-press form: n x Down, then Enter.
static void Cmd_menu(int argc, char** argv) {
    if (argc < 2) {
        SWSE_ConsolePrint("usage: menu list                      live menu screens");
        SWSE_ConsolePrint("       menu continue                  MainMenu CONTINUE (loads the last save)");
        SWSE_ConsolePrint("       menu skip | resume             a paused movie's SKIP / PLAY");
        SWSE_ConsolePrint("       menu fs <screen> <cmd> [arg]   any screen's command, e.g. menu fs Pause quick_save");
        SWSE_ConsolePrint("       menu <n>                       n x Down then Enter (key presses)");
        return;
    }
    const char* sub = argv[1];
    if (sub[0] >= '0' && sub[0] <= '9') { MenuPick(atoi(sub), "manual"); return; }
    if (!lstrcmpiA(sub, "list")) { SWSE_MenuList(SWSE_ConsolePrint); return; }
    char msg[300];
    if (!lstrcmpiA(sub, "continue")) SWSE_MenuFs("MainMenu", "goto_continue", "", msg, sizeof(msg));
    else if (!lstrcmpiA(sub, "skip")) SWSE_MenuFs("movie", "skip", "", msg, sizeof(msg));
    else if (!lstrcmpiA(sub, "resume")) SWSE_MenuFs("movie", "play", "", msg, sizeof(msg));
    else if (!lstrcmpiA(sub, "fs") && argc >= 4)
        SWSE_MenuFs(argv[2], argv[3], argc > 4 ? argv[4] : "", msg, sizeof(msg));
    else { SWSE_ConsolePrint("usage: menu list | continue | skip | resume | fs <screen> <cmd> [arg] | <n>"); return; }
    SWSE_ConsolePrint(msg);
}

// Full new-game chain in one command. NEW GAME leads to a difficulty card
// (EASY / NORMAL / HARD / BACK), also starting with nothing highlighted, so
// both screens are scheduled up front with a gap for the second to appear.
static void Cmd_newgame(int argc, char** argv) {
    int diff = (argc > 1) ? atoi(argv[1]) : 2;          // default NORMAL
    if (diff < 1 || diff > 3) diff = 2;
    int t = 600;
    SWSE_QueueKey(0xD0, t, 90); t += 260;               // Down -> NEW GAME
    SWSE_QueueKey(0x1C, t, 90); t += 2600;              // Enter, then let the card appear
    for (int i = 0; i < diff; i++) { SWSE_QueueKey(0xD0, t, 90); t += 260; }
    SWSE_QueueKey(0x1C, t + 200, 90);                   // Enter -> start
    Printf("newgame: NEW GAME then difficulty %d (%s) queued",
           diff, diff == 1 ? "EASY" : diff == 2 ? "NORMAL" : "HARD");
}
// CONTINUE: the main menu's own command when its movie is live (no input
// needed, so it works in background mode); key presses otherwise.
static void Cmd_continue(int, char**) {
    char msg[300];
    if (SWSE_MenuScreenLive("MainMenu") && SWSE_MenuFs("MainMenu", "goto_continue", "", msg, sizeof(msg)) == 1) {
        Printf("continue: %s", msg);
        return;
    }
    MenuPick(2, "CONTINUE");
}

// Cutscene skip. MEASURED: Esc during a movie opens a PAUSE card whose items
// are SKIP MOVIE / CONTINUE, with nothing highlighted, so it takes one Down to
// land on SKIP MOVIE and then Enter. Esc alone just pauses.
static void Cmd_skipcut(int, char**) {
    SWSE_QueueKey(0x01, 0,   90);      // Esc      -> PAUSE card
    SWSE_QueueKey(0xD0, 700, 90);      // Down     -> SKIP MOVIE
    SWSE_QueueKey(0x1C, 1100, 90);     // Enter    -> confirm
    SWSE_ConsolePrint("skip: Esc, Down, Enter queued (PAUSE > SKIP MOVIE)");
}

static void Cmd_warp(int argc, char** argv) {
    if (argc < 2) { SWSE_ConsolePrint("usage: warp <level|0-6> [transition]  (see 'levels')"); return; }
    char name[160];
    if (strchr(argv[1], '/') || strchr(argv[1], '\\')) {
        lstrcpynA(name, argv[1], sizeof(name));       // caller gave a full path
    } else {
        // LoadLevel's validator (0x1FD570) rejects anything whose first
        // character is not '/' or '\' - a bare level name is silently ignored.
        // Levels live at /data/bundles/region_<n>/lm_level_<n>.lvl
        const char* sfx = argv[1];
        if (!strncmp(sfx, "lm_level_", 9)) sfx += 9;
        // Directories are two-digit: region_04, not region_4. "02a" stays as-is.
        char pad[16];
        if (sfx[0] >= '0' && sfx[0] <= '9' && !sfx[1]) wsprintfA(pad, "0%s", sfx);
        else                                           lstrcpynA(pad, sfx, sizeof(pad));
        wsprintfA(name, "/data/bundles/region_%s/lm_level_%s.lvl", pad, pad);
    }
    bool trans = (argc > 2 && !lstrcmpiA(argv[2], "transition"));
    char msg[256] = {0};
    SWSE_LoadLevel(name, trans, msg, sizeof(msg));
    SWSE_ConsolePrint(msg);
}
// ---- motion prefs: jump height, run speed, gravity, air control ----------
static void MotionCmd(int field, const char* label, int argc, char** argv) {
    float cur = 0;
    if (argc > 1) {
        float v = (float)atof(argv[1]);
        int n = SWSE_MotionField(field, &v, &cur);
        if (n) Printf("%s: %d.%03d -> %d.%03d  (%d instance(s))", label,
                      (int)cur, (int)((cur < 0 ? -cur : cur) * 1000) % 1000,
                      (int)v,   (int)((v   < 0 ? -v   : v)   * 1000) % 1000, n);
        else   Printf("%s: no instances found (load a save first)", label);
        return;
    }
    int n = SWSE_MotionField(field, nullptr, &cur);
    if (n) Printf("%s = %d.%03d  (%d instance(s))", label,
                  (int)cur, (int)((cur < 0 ? -cur : cur) * 1000) % 1000, n);
    else   Printf("%s: no instances found (load a save first)", label);
}
static void Cmd_jump(int argc, char** argv)    { MotionCmd(0, "jump height", argc, argv); }
static void Cmd_speed(int argc, char** argv)   { MotionCmd(1, "run speed",   argc, argv); }
static void Cmd_gravity(int argc, char** argv) { MotionCmd(2, "gravity",     argc, argv); }
static void Cmd_aircontrol(int argc, char** argv) { MotionCmd(3, "air control", argc, argv); }
static void Cmd_spawn(int argc, char** argv) {
    if (argc < 2) {
        int n = SWSE_Spawn(0);
        Printf("%d ActorSpawner(s) in this level (see log). 'spawn <n>' to fire them.", n);
        return;
    }
    int c = atoi(argv[1]);
    if (c < 1) c = 1;
    int n = SWSE_Spawn(c);
    Printf("armed %d spawner(s) for %d actor(s) each", n, c);
}
static void Cmd_spawnhere(int argc, char** argv) {
    int c = (argc > 1) ? atoi(argv[1]) : 5;
    if (c < 1) c = 1;
    int r = SWSE_SpawnHere(c);
    if (r == 1)      Printf("spawner moved to you, armed for %d - look around", c);
    else if (r == 0) SWSE_ConsolePrint("no player position (load a save first)");
    else             SWSE_ConsolePrint("no usable spawner in this level");
}
static void Cmd_npcspy(int argc, char** argv) {
    int on = !(argc > 1 && !lstrcmpiA(argv[1], "off"));
    // Hook the WHOLE spawn routine, not just the factory: replaying the factory
    // alone produced an NPC whose registration faulted on skipped init.
    if (SWSE_NpcRoutineSpy(on) != 1) { SWSE_ConsolePrint("could not hook the spawn routine"); return; }
    if (!on) { SWSE_ConsolePrint("npcspy OFF"); return; }
    SWSE_ConsolePrint("npcspy ON - now warp or load a level to capture a real NPC spawn");
}
// The two commands below spawn using ONLY the game's own routine and the game's
// own arguments, at the moment they are live. Every fabricated-tag attempt has
// faulted, so nothing here is fabricated: npcdupe re-calls the routine with the
// captured this/arg0 pair (the game itself calls it repeatedly with that same
// pair), and npccount raises m_countToSpawn and lets the game do the rest.
static void Cmd_npcdupe(int argc, char** argv) {
    int n = (argc > 1) ? atoi(argv[1]) : 1;
    SWSE_NpcRoutineSpy(1);
    SWSE_NpcDupe(n);
    Printf("npcdupe = %d - now warp or load a save; each spawn fires %d extra times", n, n);
}
static void Cmd_whatis(int argc, char** argv) {
    if (argc < 2) { SWSE_ConsolePrint("usage: whatis <hex address>"); return; }
    unsigned a = (unsigned)strtoul(argv[1], nullptr, 16);
    char msg[200];
    SWSE_WhatIs(a, msg, sizeof(msg));
    SWSE_ConsolePrint(msg);
}
// Clakkerz are townsfolkprefs.txt (5CEE67FD) with m_health = 100000, which is
// the whole of their "immortality". Default to 45, an outlaw cutter's value.
static void PrefsGet(const char* target, const char* field);   // fwd (1.1 block)

// m_affGenerally is "player ammo affects this character" (a bool, with
// m_affList as the exceptions) - 1.0.x called it affiliation, and its default
// of 2 made the type immune to most ammo. Now it only reports unless a value
// is given. `npc <type> ammorule` is the same field through the prefs editor.
static void Cmd_npcaff(int argc, char** argv) {
    if (argc < 2) {
        SWSE_ConsolePrint("usage: npcaff <typeHash> [0|1]   1 = player ammo affects it (shipped), 0 = immune");
        SWSE_ConsolePrint("  (1.0.x documented this as affiliation - it is ammo susceptibility)");
        return;
    }
    unsigned h = (unsigned)strtoul(argv[1], nullptr, 16);
    if (argc < 3) { char hs[16]; wsprintfA(hs, "%08X", h); PrefsGet(hs, "m_affGenerally"); return; }
    int v = atoi(argv[2]);
    char msg[200];
    SWSE_SetTypeAff(h, v, msg, sizeof(msg));
    SWSE_ConsolePrint(msg);
}
static void Cmd_npchurt(int argc, char** argv) {
    if (argc < 2) { SWSE_ConsolePrint("usage: npchurt <typeHash> [0|2]   0=staggers, 2=unflinchable"); return; }
    unsigned h = (unsigned)strtoul(argv[1], nullptr, 16);
    int v = (argc > 2) ? atoi(argv[2]) : 2;
    char msg[200];
    SWSE_SetTypeHurt(h, v, msg, sizeof(msg));
    SWSE_ConsolePrint(msg);
}
// raid [zoneIndex] -- post the town alarm through the game's own verb.
// With no argument it lists the level's panic zones.
// attack <attackerType> <victimType> [count] [run]
// e.g.  attack 072A64D6 5CEE67FD 5     -- five outlaw cutters go for a Clakkerz
// findtarget [npcAddr] - with no address, uses the NPC nearest you (the one
// most likely to be actively shooting at you).
// raidmode <attackerHash> <victimHash> [everyFrames]  /  raidmode off
// Standing hostility, re-evaluated every few frames: raiders go for the nearest
// townsfolk OR the player, whichever is closer.
// decoy <shooterType> <victimType> [everyFrames]  /  decoy off
// Keeps the shooter hostile to the player but feeds it the victim's position,
// so its shots land where the victim is standing.
// feud <typeA> <typeB> [rounds] - inject mutual damage between two live NPCs.
// Untested as of writing; the retaliation route is the last idea standing for
// NPC-vs-NPC combat.
static void Cmd_feud(int argc, char** argv) {
    if (argc < 3) { SWSE_ConsolePrint("usage: feud <typeA> <typeB> [rounds]   both must be ACTIVE"); return; }
    unsigned a = (unsigned)strtoul(argv[1], nullptr, 16);
    unsigned b = (unsigned)strtoul(argv[2], nullptr, 16);
    int rounds = (argc > 3) ? atoi(argv[3]) : 8;
    char msg[220];
    SWSE_Feud(a, b, rounds, msg, sizeof(msg));
    SWSE_ConsolePrint(msg);
}
static void Cmd_decoy(int argc, char** argv) {
    if (argc > 1 && !lstrcmpiA(argv[1], "off")) {
        SWSE_DecoyMode(0, 0, 0, 0);
        SWSE_ConsolePrint("decoy OFF");
        return;
    }
    if (argc < 3) {
        Printf("decoy: %d shooter(s) fed a false position last tick", SWSE_DecoyCount());
        SWSE_ConsolePrint("usage: decoy <shooterHash> <victimHash> [everyFrames]");
        SWSE_ConsolePrint("  e.g. decoy 072A64D6 5CEE67FD   (outlaws shoot at the chickens' spot)");
        return;
    }
    unsigned sh = (unsigned)strtoul(argv[1], nullptr, 16);
    unsigned v  = (unsigned)strtoul(argv[2], nullptr, 16);
    int every = (argc > 3) ? atoi(argv[3]) : 10;
    SWSE_DecoyMode(sh, v, 1, every);
    Printf("decoy ON: %08X believes you are standing where %08X is", sh, v);
}
static void Cmd_scantargets(int, char**) {
    char msg[220];
    SWSE_ScanTargets(msg, sizeof(msg));
    SWSE_ConsolePrint(msg);
    SWSE_ConsolePrint("  pairs logged to swse_log.txt");
}
static void Cmd_raidmode(int argc, char** argv) {
    if (argc > 1 && !lstrcmpiA(argv[1], "off")) {
        SWSE_RaidMode(0, 0, 0, 0);
        SWSE_ConsolePrint("raid mode OFF");
        return;
    }
    if (argc < 3) {
        Printf("raid mode is %s (%d retargeted last tick)",
               SWSE_RaidOn() ? "ON" : "off", SWSE_RaidCount());
        SWSE_ConsolePrint("usage: raidmode <attackerHash> <victimHash> [everyFrames]");
        SWSE_ConsolePrint("  e.g. raidmode 072A64D6 5CEE67FD    (outlaws raid the townsfolk)");
        return;
    }
    unsigned a = (unsigned)strtoul(argv[1], nullptr, 16);
    unsigned v = (unsigned)strtoul(argv[2], nullptr, 16);
    int every = (argc > 3) ? atoi(argv[3]) : 60;
    SWSE_RaidMode(a, v, 1, every);
    Printf("raid mode ON: %08X hunt %08X (and you), retargeting every %d frames", a, v, every);
}
static void Cmd_findtarget(int argc, char** argv) {
    unsigned npc = 0;
    if (argc > 1) npc = (unsigned)strtoul(argv[1], nullptr, 16);
    else {
        char m[220];
        SWSE_NpcNear(m, sizeof(m));
        npc = SWSE_LastNearNpc();
    }
    if (!npc) { SWSE_ConsolePrint("no NPC - stand near one or pass an address"); return; }
    char msg[220];
    SWSE_FindTarget(npc, msg, sizeof(msg));
    SWSE_ConsolePrint(msg);
}
static void Cmd_attack(int argc, char** argv) {
    if (argc < 3) { SWSE_ConsolePrint("usage: attack <attackerHash> <victimHash> [count] [run]"); return; }
    unsigned a = (unsigned)strtoul(argv[1], nullptr, 16);
    unsigned v = (unsigned)strtoul(argv[2], nullptr, 16);
    int n = (argc > 3) ? atoi(argv[3]) : 3;
    int run = (argc > 4 && !lstrcmpiA(argv[4], "run"));
    char msg[220];
    SWSE_MakeAttack(a, v, n, run, msg, sizeof(msg));
    SWSE_ConsolePrint(msg);
}
static void Cmd_raid(int argc, char** argv) {
    unsigned zones[8];
    int n = SWSE_PanicZones(zones, 8);
    if (!n) { SWSE_ConsolePrint("no panic zones in this level (not a town?)"); return; }
    if (argc < 2) {
        for (int i = 0; i < n; i++) Printf("  zone %d: %08X", i, zones[i]);
        Printf("%d panic zone(s) - use: raid <index>   or  raid <index> bell", n);
        return;
    }
    int idx = atoi(argv[1]);
    if (idx < 0 || idx >= n) idx = 0;
    int bell = (argc > 2 && !lstrcmpiA(argv[2], "bell"));
    char msg[200];
    SWSE_PostAlarm(zones[idx], bell, msg, sizeof(msg));
    SWSE_ConsolePrint(msg);
}
static void Cmd_townpanic(int argc, char** argv) {
    int forever = -1, radius = -1;
    if (argc > 1) forever = (!lstrcmpiA(argv[1], "on") || atoi(argv[1]) == 1) ? 1
                          : (!lstrcmpiA(argv[1], "off") ? 0 : -1);
    if (argc > 2) radius = atoi(argv[2]);
    char msg[220];
    SWSE_TownPanic(forever, radius, msg, sizeof(msg));
    SWSE_ConsolePrint(msg);
}
static void Cmd_whereis(int argc, char** argv) {
    if (argc < 2) { SWSE_ConsolePrint("usage: whereis <typeHash>  (e.g. whereis F4DC66D8)"); return; }
    char msg[200];
    SWSE_WhereIs((unsigned)strtoul(argv[1], nullptr, 16), msg, sizeof(msg));
    SWSE_ConsolePrint(msg);
}
static void Cmd_npchealth(int argc, char** argv) {
    if (argc < 2) { SWSE_ConsolePrint("usage: npchealth <typeHash> [health]  (e.g. npchealth 5CEE67FD 45)"); return; }
    unsigned h = (unsigned)strtoul(argv[1], nullptr, 16);
    float v = (argc > 2) ? (float)atoi(argv[2]) : 45.0f;
    char msg[220];
    SWSE_SetTypeHealth(h, v, msg, sizeof(msg));
    SWSE_ConsolePrint(msg);
}
// "Apply to everything in this level" -- the sweep version of npchealth/npcgib.
// Reload characters.txt. Editing the file and running this is the whole
// authoring loop -- no rebuild, no restart.
static void GetModDir(char* out);   // defined with the remote mailbox below

// Names for the hashes we have identified, so a dumped file is readable
// instead of being a wall of hex. Everything else is dumped as "unknown" with
// its stock health, which is still enough to tune it.
struct KnownChar { unsigned hash; const char* name; };
static const KnownChar kKnownChars[] = {
    { 0x072A64D6, "outlawcutter" },      { 0xFFFC00CB, "outlawshooter" },
    { 0xFDBD2F9C, "wolvarkshooter" },    { 0xF0315D54, "wolvarkgrenadier" },
    { 0x31D1280E, "wolvarksniper" },     { 0xC387D977, "vykkerdoc" },
    { 0x7C6717E1, "slog" },              { 0x2F1D4FF5, "sewerslog" },
    { 0x018AD12D, "giantslog" },         { 0x59DB2D18, "elbowzfreely" },
    { 0xB53065FF, "outlawboss_elbowz" }, { 0xE2D7415C, "sloghandler" },
    { 0x5CEE67FD, "townsfolk_clakkerz" },{ 0x76C75C78, "armadillo" },
    { 0x16956EF7, "fuzzle" },            { 0xC97B758C, "skunk" },
    { 0xF73A6BF0, "spider" },            { 0x1833B954, "squirrel" },
    { 0x1BC27DCA, "stingbee" },          { 0x55F1EA59, "sulphurbat" },
};
static const char* KnownCharName(unsigned h) {
    for (int i = 0; i < (int)(sizeof(kKnownChars) / sizeof(kKnownChars[0])); i++)
        if (kKnownChars[i].hash == h) return kKnownChars[i].name;
    return "unknown";
}

// Every character type this level uses, with its health and gib flag.
// `types dump` appends them to characters_all.txt in characters.txt format, so
// walking through the levels builds a complete, editable roster -- far better
// than hunting immortal variants one at a time by standing next to them.
static void Cmd_types(int argc, char** argv) {
    int n = SWSE_NpcTypeCount();
    if (!n) { SWSE_ConsolePrint("no types yet - run npcspy, then warp or load a save"); return; }
    bool dump = (argc > 1 && !lstrcmpiA(argv[1], "dump"));

    HANDLE f = INVALID_HANDLE_VALUE;
    if (dump) {
        char dir[MAX_PATH], path[MAX_PATH];
        GetModDir(dir);
        wsprintfA(path, "%s\\characters_all.txt", dir);
        f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) SetFilePointer(f, 0, NULL, FILE_END);
        Printf("appending to %s", path);
    }

    int immortal = 0;
    for (int i = 0; i < n; i++) {
        unsigned h = SWSE_NpcTypeHash(i);
        int hp = 0, gib = 0, hurt = 0, aff = 0;
        if (SWSE_TypeInfo2(h, &hp, &gib, &hurt, &aff) != 1) {
            Printf("%2d  %08X  (did not resolve)", i, h);
            continue;
        }
        if (hp > 1000) immortal++;
        Printf("%2d  %08X  hp=%-7d gib=%d hurt=%d aff=%d  %s",
               i, h, hp, gib, hurt, aff, KnownCharName(h));
        if (f != INVALID_HANDLE_VALUE) {
            char line[200]; DWORD wrote = 0;
            wsprintfA(line, "%08X    %-7d %d    # %s\r\n", h, hp, gib, KnownCharName(h));
            WriteFile(f, line, lstrlenA(line), &wrote, NULL);
        }
    }
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    Printf("%d type(s); %d still have huge health", n, immortal);
}
// Reads console.txt (1.0.x looked for a settings.txt that never shipped, so
// `noimmortals` was silently ignored) and every mod's characters.txt, then arms
// the spawn hook. The npctuning feature does this at launch; this command is
// the manual form, and works whether or not the feature is on.
static void Cmd_tuning(int, char**) {
    char msg[300];
    int rules = SWSE_NpcTuningLoadAll(msg, sizeof(msg));
    SWSE_ConsolePrint(msg);
    if (rules < 0) return;                  // no spawn hook to apply them; msg says why
    SWSE_ConsolePrint("  spawn hook armed - warp or load a save to apply");
    if (!SWSE_Feature(FEAT_NPCTUNING))
        SWSE_ConsolePrint("  (npctuning is off: this lasts until restart - `features npctuning on` keeps it)");
}
static void Cmd_allnpcs(int argc, char** argv) {
    if (argc < 2) {
        SWSE_ConsolePrint("usage: allnpcs <health|-> [gib 0|1]   e.g. allnpcs 45 1");
        SWSE_ConsolePrint("  '-' leaves health alone: allnpcs - 1  = gib everything");
        return;
    }
    float hp = (argv[1][0] == '-' && argv[1][1] == 0) ? -1.0f : (float)atoi(argv[1]);
    int gib = (argc > 2) ? atoi(argv[2]) : -1;
    char msg[220];
    SWSE_SetAllTypes(hp, gib, msg, sizeof(msg));
    SWSE_ConsolePrint(msg);
}
static void Cmd_npcgib(int argc, char** argv) {
    if (argc < 2) { SWSE_ConsolePrint("usage: npcgib <typeHash> [0|1]  (e.g. npcgib 5CEE67FD 1)"); return; }
    unsigned h = (unsigned)strtoul(argv[1], nullptr, 16);
    int on = (argc > 2) ? atoi(argv[2]) : 1;
    char msg[200];
    SWSE_SetTypeGib(h, on, msg, sizeof(msg));
    SWSE_ConsolePrint(msg);
}
static void Cmd_diff(int argc, char** argv) {
    if (argc < 3) { SWSE_ConsolePrint("usage: diff <addrA> <addrB> [len]  - differing fields only"); return; }
    unsigned a = (unsigned)strtoul(argv[1], nullptr, 16);
    unsigned b = (unsigned)strtoul(argv[2], nullptr, 16);
    int len = (argc > 3) ? (int)strtoul(argv[3], nullptr, 16) : 0x400;
    char msg[200];
    SWSE_DiffObjects(a, b, len, msg, sizeof(msg));
    SWSE_ConsolePrint(msg);
}
// Find one live NPC of each of two types and diff them -- the practical form,
// since hunting two addresses by hand is the slow part.
static void Cmd_difftypes(int argc, char** argv) {
    if (argc < 3) { SWSE_ConsolePrint("usage: difftypes <hashA> <hashB> [len]"); return; }
    unsigned ha = (unsigned)strtoul(argv[1], nullptr, 16);
    unsigned hb = (unsigned)strtoul(argv[2], nullptr, 16);
    int len = (argc > 3) ? (int)strtoul(argv[3], nullptr, 16) : 0x400;
    unsigned a = SWSE_FirstNpcOfType(ha), b = SWSE_FirstNpcOfType(hb);
    if (!a || !b) { Printf("could not find a live NPC of %08X (%08X) and %08X (%08X)", ha, a, hb, b); return; }
    Printf("A=%08X (%08X)   B=%08X (%08X)", a, ha, b, hb);
    char msg[200];
    SWSE_DiffObjects(a, b, len, msg, sizeof(msg));
    SWSE_ConsolePrint(msg);
}
static void Cmd_nearby(int argc, char** argv) {
    int r = (argc > 1) ? atoi(argv[1]) : 15;
    char msg[200];
    SWSE_Nearby(r, msg, sizeof(msg));
    SWSE_ConsolePrint(msg);
}
static void Cmd_instances(int argc, char** argv) {
    if (argc < 2) { SWSE_ConsolePrint("usage: instances <hex address>"); return; }
    unsigned a = (unsigned)strtoul(argv[1], nullptr, 16);
    char msg[200];
    SWSE_Instances(a, msg, sizeof(msg));
    SWSE_ConsolePrint(msg);
}
static void Cmd_spawngate(int, char**) {
    char msg[200];
    SWSE_SpawnGate(msg, sizeof(msg));
    SWSE_ConsolePrint(msg);
}
static void Cmd_npcnow(int argc, char** argv) {
    char msg[220];
    int n = (argc > 1) ? atoi(argv[1]) : 1;
    // -1 = whatever the capture ended on; a small number indexes the harvested
    // list; an 8-digit hex value is a raw type hash from npcnear.
    int t = -1;
    if (argc > 2) t = (int)strtoul(argv[2], nullptr, 16);
    if (argc > 2 && lstrlenA(argv[2]) <= 2) t = atoi(argv[2]);
    SWSE_SpawnNow(n, t, msg, sizeof(msg));
    SWSE_ConsolePrint(msg);
}
// Name a hash by guessing its path. Most character prefs are referenced by
// hash only -- just 20 of them appear as strings anywhere -- so the way to
// label the harvested types is to hash candidate filenames with the game's own
// hasher and see which land on a known value. Reimplementing that hash offline
// did not reproduce it, so it has to happen in-process.
static void Cmd_strhash(int argc, char** argv) {
    if (argc < 2) { SWSE_ConsolePrint("usage: strhash <path>  (try /data/prefs/characters/xprefs.txt)"); return; }
    unsigned h = SWSE_HashPath(argv[1]);
    int idx = -1;
    for (int i = 0; i < SWSE_NpcTypeCount(); i++)
        if (SWSE_NpcTypeHash(i) == h) { idx = i; break; }
    if (idx >= 0) Printf("%08X  %s  == harvested type %d - MATCH", h, argv[1], idx);
    else          Printf("%08X  %s  (no match among %d harvested types)", h, argv[1], SWSE_NpcTypeCount());
}
static void Cmd_spawnradius(int argc, char** argv) {
    int t = (argc > 1) ? atoi(argv[1]) : 20;
    SWSE_SetSpawnRadius(t);
    Printf("spawn radius = %d.%d units", t / 10, t % 10);
}
static void Cmd_resolve(int argc, char** argv) {
    if (argc < 2) { SWSE_ConsolePrint("usage: resolve <hex type hash>"); return; }
    char msg[220];
    SWSE_Resolve((unsigned)strtoul(argv[1], nullptr, 16), msg, sizeof(msg));
    SWSE_ConsolePrint(msg);
}
// `ai <hash>` - dump a character's AI and weapon tuning.
// `ai <hash> <field> <value>` - set one of them.
static void AiEmit(const char* s) { SWSE_ConsolePrint(s); }

static void Cmd_ai(int argc, char** argv) {
    if (argc < 2) {
        SWSE_ConsolePrint("usage: ai <typehash> [field value]");
        SWSE_ConsolePrint("  fields: firerate reload accuracy misstime");
        SWSE_ConsolePrint("          seedist 6thsense sightcombat relax");
        SWSE_ConsolePrint("  ai <hash>            - dump everything");
        SWSE_ConsolePrint("  npcnear tells you a nearby character's hash");
        return;
    }
    unsigned h = (unsigned)strtoul(argv[1], nullptr, 16);
    if (argc < 4) { SWSE_AiDump(h, AiEmit); return; }

    // name -> (object, offset). Weapon fields and AI fields live on different
    // objects, so the table carries which one each belongs to. AI offsets are
    // relative to the AIPrefs object embedded at NPCPrefs+0x118 (reflection-
    // confirmed); 1.0.x's were 4 bytes short, starting ON the vtable.
    // isTime: stored in seconds, shown in ms. firerate is shots per second.
    struct { const char* name; int onWeapon; int off; int isTime; } F[] = {
        { "firerate",    1, 0x17C, 0 }, { "reload",      1, 0x184, 1 },
        { "reloadmax",   1, 0x188, 1 }, { "accuracy",    1, 0x1A8, 0 },
        { "misstime",    1, 0x1AC, 1 },
        { "6thsense",    0, 0x004, 0 }, { "seedist",     0, 0x008, 0 },
        { "hidevolsee",  0, 0x020, 0 }, { "sightcombat",0, 0x150, 0 },
        { "relax",       0, 0x2A0, 0 },
    };
    for (int i = 0; i < (int)(sizeof(F) / sizeof(F[0])); i++) {
        if (lstrcmpiA(argv[2], F[i].name)) continue;
        float v = (float)atof(argv[3]);
        if (!SWSE_AiSet(h, F[i].onWeapon, F[i].off, v)) {
            Printf("%08X: no %s prefs object to write",
                   h, F[i].onWeapon ? "ranged weapon" : "AI");
            return;
        }
        char vs[32];
        if (F[i].isTime) sprintf_s(vs, sizeof(vs), "%d ms", (int)(v * 1000.0f));
        else             sprintf_s(vs, sizeof(vs), "%.3g", v);
        Printf("%08X %s = %s", h, F[i].name, vs);
        SWSE_ConsolePrint("(affects the whole species; live NPCs use it next decision)");
        return;
    }
    Printf("unknown field '%s' - run `ai <hash>` for the list", argv[2]);
}

// `uispy` - watch the menu system. Answers "what does pressing this button
// actually run?" by observation instead of static analysis.
// `findai` - locate the perception prefs objects by memory shape.
// The hash resolver is NPCPrefs-only and cannot reach them, so they are found
// by their signature instead: four sight blocks at a 0xA4 stride.
// `difficulty [name|off]` - apply an AI profile from aiprefs.txt.
// `weapons` - dump every NPC weapon prefs found. READ ONLY, by design: the
// signature is weaker than the perception one, so nothing is written.
// Character names recovered in research/NPC_TUNING.md (hashed from the
// animation configs and model folders, then matched against live types).
// Kept here so `npcguns` prints something a person can act on rather than a
// column of hashes.
static const struct { unsigned hash; const char* name; } kNpcNames[] = {
    { 0x35179A52, "Jo Momma" },          { 0x2B6A3743, "Bad Mortar" },
    { 0x0BB34EB7, "'Splosives McGree" }, { 0x57009F0C, "Fatty McBoomBoom" },
    { 0xB53065FF, "Elbows Freely" },     { 0xFF61B694, "Filthy Hands Floyd" },
    { 0x151F4B4E, "shocktank" },         { 0x450E9598, "gloktigi" },
    { 0xCAB666AF, "giant boss" },        { 0xF4DC66D8, "heavy" },
    { 0x018AD12D, "giant slog" },        { 0x155A0299, "movie boss" },
    { 0xC9F81B7E, "castaraider" },       { 0xFDBD2F9C, "wolvark shooter" },
    { 0x5629AD35, "outlaw bomber" },     { 0xFF32E523, "outlaw nailer" },
    { 0x2D8CF05F, "outlaw semiauto" },   { 0xFF0100ED, "outlaw mortar" },
    { 0x20F622E5, "wolvark slog handler" }, { 0xF0315D54, "wolvark grenadier" },
    { 0xFFFC00CB, "outlaw shooter" },    { 0x072A64D6, "outlaw cutter" },
    { 0x6A6A4558, "outlaw sniper" },     { 0x31D1280E, "wolvark sniper" },
    { 0x2F1D4FF5, "sewer slog" },        { 0x7C6717E1, "slog" },
    { 0x5CEE67FD, "townsfolk" },         { 0xB6FE5A75, "native (Grubb)" },
    { 0x3EE6BB58, "native female" },     { 0x0E997260, "townsfolk female" },
    { 0x606283C9, "farmer foster" },     { 0xBAF35C16, "storekeeper" },
    { 0xAB444625, "sewer worker" },      { 0xC766CCA2, "ugenius" },
    { 0x214F50C8, "native rebel" },      { 0xC387D977, "vykker doc" },
};

static const char* NpcName(unsigned h) {
    for (int i = 0; i < (int)(sizeof(kNpcNames) / sizeof(kNpcNames[0])); i++)
        if (kNpcNames[i].hash == h) return kNpcNames[i].name;
    return "?";
}

// `npcguns` - who carries what. Joins each character to its ranged weapon.
static void Cmd_npcguns(int argc, char** argv) {
    static NpcGunRow rows[64];
    int n = SWSE_NpcGuns(rows, 64, (argc > 1) ? atof(argv[1]) : 4000.0);
    if (!n) { SWSE_ConsolePrint("no armed characters found (load a level)"); return; }
    Printf("%d armed character type(s):", n);
    SWSE_ConsolePrint("  hash      hp   bounty  shots/s  name");
    for (int i = 0; i < n; i++) {
        // m_fireRate is shots per SECOND (aitune.cpp) - 1.0.x printed x1000
        // as "ms between shots", so the 10-shot semiauto read as 10000 ms.
        char fr[16];
        sprintf_s(fr, sizeof(fr), "%.2f", rows[i].fireRate);
        Printf("  %08X %5d %6d %7s  %s", rows[i].npcHash,
               (int)rows[i].health, (int)rows[i].killMoolah, fr, NpcName(rows[i].npcHash));
    }
    SWSE_ConsolePrint("'?' = name not yet recovered");
}

static void Cmd_weapons(int argc, char** argv) {
    unsigned hits[48];
    int n = SWSE_FindWeaponPrefs(hits, 48, (argc > 1) ? atof(argv[1]) : 2500.0);
    if (!n) { SWSE_ConsolePrint("no weapon prefs found (load a level with armed NPCs)"); return; }
    Printf("%d weapon prefs object(s)   [read-only]", n);
    SWSE_ConsolePrint("  addr      fire  reload   acc   MISS  cone");
    for (int i = 0; i < n; i++) {
        float fr = 0, rt = 0, acc = 0, miss = 0, cxy = 0, cz = 0;
        __try {
            fr   = *(float*)(hits[i] + 0x17C);
            rt   = *(float*)(hits[i] + 0x184);
            acc  = *(float*)(hits[i] + 0x1A8);
            miss = *(float*)(hits[i] + 0x1AC);
            cxy  = *(float*)(hits[i] + 0x18C);
            cz   = *(float*)(hits[i] + 0x190);
        } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        // milliseconds for the timings; they are all fractions of a second
        Printf("  %08X %5d %6d %5d %6d  %dx%d", hits[i],
               (int)(fr * 1000.0f), (int)(rt * 1000.0f),
               (int)(acc * 100.0f), (int)(miss * 1000.0f), (int)cxy, (int)cz);
    }
    SWSE_ConsolePrint("fire/reload/MISS in ms, acc x100. MISS = deliberate-miss time");
}


// One switch to on or off, the way `features <name> on|off [temp]` does it,
// with its line printed. True when something changed - live, or the value
// features.txt should hold - so the caller saves (or says "this session
// only"). `named`: the user named this switch, so "already on" is said too;
// `all` and the presets stay quiet about switches already right.
static bool SwitchOne(int i, bool on, bool temp, bool named) {
    SwseFeature f = (SwseFeature)i;
    if (SWSE_Feature(f) == on) {
        // Live already matches - but the FILE may not: `features console
        // off` saves off while the console stays up, and `features foliage
        // on temp` leaves the file at off. Answering "already on" without
        // saving made the first undoable only by hand-editing
        // features.txt, and the second impossible to make permanent.
        if (!temp && (SWSE_FeatureSaved(f) != on || SWSE_FeatureIsAuto(f))) {
            SWSE_FeatureSetSaved(f, on);
            Printf("%s is already %s - saved that to features.txt",
                   SWSE_FeatureName(f), on ? "on" : "off");
            return true;
        }
        if (named) Printf("%s is already %s", SWSE_FeatureName(f), on ? "on" : "off");
        return false;
    }
    // On: flag first, because systems consult it while starting (HD looks
    // itself up per upload). Off: stop first, then drop the flag.
    char msg[300] = { 0 };
    bool ok;
    if (on) {
        SWSE_FeatureSetFlag(f, true);
        ok = SWSE_FeatureStart(f, msg, sizeof(msg));
        if (!ok) SWSE_FeatureSetFlag(f, false);
    } else {
        ok = SWSE_FeatureStop(f, msg, sizeof(msg));
        if (ok) SWSE_FeatureSetFlag(f, false);
    }
    // A refused start (safe mode, raytrace without graphics) says so: it
    // printed "ON" ahead of the reason, with the switch left off.
    Printf("%s %s: %s", SWSE_FeatureName(f), on ? (ok ? "ON" : "refused") : "off", msg);
    if (!ok) return false;
    if (!temp) SWSE_FeatureSetSaved(f, on);       // only non-temp reaches the file
    // Plugins that are on hear every live switch change (the FEATURE event).
    SWSE_PluginsNotifyFeature(i, on);
    return true;
}

// Save after a batch of switches, or say that it was for this session only.
static void SaveOrSay(int changed, bool temp) {
    if (changed && !temp) {
        char sm[200];
        SWSE_FeaturesSave(sm, sizeof(sm));
        SWSE_ConsolePrint(sm);
    } else if (changed) {
        SWSE_ConsolePrint("(this session only - features.txt unchanged)");
    }
}

// `features preset full|default|list [temp]` (1.1): several switches in one
// word, saved like `features` (or for the session with `temp`).
//   full    - the classic SWSE, the 1.0.x look: console, graphics,
//             hdtextures, hitreact, foliage and aituning on; every other
//             switch as it is.
//   default - the 1.1 default: console on, aituning and playertune auto,
//             everything else off - plugins too, which are off unless named.
// In safe mode the switches it refuses are left alone and named in one line.
// SWSE Setup offers the same two as buttons (oddforge/swsefeatures.py).
#define PRESET_KEEP -1
#define PRESET_OFF   0
#define PRESET_ON    1
#define PRESET_AUTO  2
static void PresetTargets(bool full, int* want) {
    for (int i = 0; i < FEAT_COUNT; i++) want[i] = full ? PRESET_KEEP : PRESET_OFF;
    want[FEAT_CONSOLE] = PRESET_ON;
    if (full) {
        want[FEAT_GRAPHICS] = want[FEAT_HDTEXTURES] = want[FEAT_HITREACT] =
            want[FEAT_FOLIAGE] = want[FEAT_AITUNING] = PRESET_ON;
    } else {
        want[FEAT_AITUNING] = want[FEAT_PLAYERTUNE] = PRESET_AUTO;
    }
}

static void Cmd_featuresPreset(int argc, char** argv) {
    const char* which = (argc > 2) ? argv[2] : "list";
    bool temp = (argc > 3 && !lstrcmpiA(argv[3], "temp"));
    bool full = !lstrcmpiA(which, "full") || !lstrcmpiA(which, "classic");
    bool def  = !lstrcmpiA(which, "default");
    if (!full && !def) {
        SWSE_ConsolePrint("features preset full      the classic SWSE (the 1.0.x look): console, graphics,");
        SWSE_ConsolePrint("                          hdtextures, hitreact, foliage and aituning on; the rest as they are");
        SWSE_ConsolePrint("features preset default   the 1.1 default: console on, aituning and playertune auto,");
        SWSE_ConsolePrint("                          everything else off (plugins too)");
        SWSE_ConsolePrint("add `temp` to try one for this session only; without it, features.txt is saved");
        return;
    }
    int want[FEAT_COUNT];
    PresetTargets(full, want);
    int changed = 0;
    char skipped[200] = "", later[200] = "";
    for (int i = 0; i < FEAT_COUNT; i++) {
        if (want[i] == PRESET_KEEP) continue;
        SwseFeature f = (SwseFeature)i;
        // Safe mode: a switch it refuses is left as it is, and named once
        // below rather than refused line by line.
        if (want[i] != PRESET_OFF && SWSE_GameBuildSafeMode() && SWSE_FeatureSafeModeRefusal(f)) {
            if (skipped[0]) lstrcatA(skipped, ", ");
            lstrcatA(skipped, SWSE_FeatureName(f));
            continue;
        }
        if (want[i] == PRESET_AUTO) {
            // As `features <name> auto`: follow its own file from now on.
            if (SWSE_FeatureIsAuto(f)) continue;
            if (!temp) SWSE_FeatureSetSavedAuto(f);
            char msg[400];
            bool was = SWSE_Feature(f);
            SWSE_FeatureAutoEvaluate(f, msg, sizeof(msg));
            SWSE_ConsolePrint(msg);
            if (SWSE_Feature(f) != was) SWSE_PluginsNotifyFeature(i, !was);
            changed++;
            continue;
        }
        bool on = (want[i] == PRESET_ON), was = SWSE_Feature(f);
        if (SwitchOne(i, on, temp, false)) changed++;
        // Plants and textures are recognised as a level uploads them, and hit
        // reactions first find the level's characters (features.txt says so).
        if (on && !was && SWSE_Feature(f) &&
            (f == FEAT_HDTEXTURES || f == FEAT_HITREACT || f == FEAT_FOLIAGE)) {
            if (later[0]) lstrcatA(later, ", ");
            lstrcatA(later, SWSE_FeatureName(f));
        }
    }
    // default: plugins off as well - a plugin runs only when named.
    if (def)
        for (int i = FEAT_COUNT; i < SWSE_FeatureTotal(); i++)
            if (SwitchOne(i, false, temp, false)) changed++;
    if (!changed) Printf("preset %s: every switch is already so", full ? "full" : "default");
    SaveOrSay(changed, temp);
    if (later[0])
        Printf("%s: full effect from the next level load (warp, or load a save)", later);
    if (skipped[0])
        Printf("left as they are - not on this game build (safe mode): %s", skipped);
}

// `features` - which SWSE systems are switched on, and switching them.
//   features                      list, with what each one does
//   features <name> on|off        switch now AND remember it in features.txt
//   features <name> on|off temp   switch now, this session only
//   features all off              everything but the console (plugins too)
//   features all on               every built-in but the console - never a
//                                 plugin, which loads only when named (1.1)
//   features preset full|default  several at once (Cmd_featuresPreset, 1.1)
// A feature that was off at launch has no hooks installed; switching it on
// here installs them at this point. Switching one off stops its effect (and
// restores shipped values where the system keeps them); hooks it already
// installed stay, inert, until the next launch.
static void Cmd_features(int argc, char** argv) {
    // Presets (1.1): several switches in one word.
    if (argc > 1 && !lstrcmpiA(argv[1], "preset")) { Cmd_featuresPreset(argc, argv); return; }
    if (argc < 3) {
        Printf("SWSE %s features (%s):", SWSE_VERSION,
               SWSE_FeaturesFromFile() ? "SWSEMods\\features.txt"
                                       : "no features.txt - console only");
        // Built-ins, then one switch per native plugin (1.1), in load order.
        for (int i = 0; i < SWSE_FeatureTotal(); i++) {
            SwseFeature f = (SwseFeature)i;
            const char* st = SWSE_FeatureIsAuto(f) ? (SWSE_Feature(f) ? "auto:on" : "auto:off")
                                                   : (SWSE_Feature(f) ? "on" : "off");
            char pd[300];
            if (SWSE_FeatureIsPlugin(i)) SWSE_PluginDescribeFeature(i, pd, sizeof(pd));
            // Safe mode: the switches that act on the game cannot come on.
            bool refused = !SWSE_FeatureIsPlugin(i) && SWSE_GameBuildSafeMode() &&
                           SWSE_FeatureSafeModeRefusal(f);
            Printf("  %-11s %-8s  %s%s", SWSE_FeatureName(f), refused ? "refused" : st,
                   SWSE_FeatureIsPlugin(i) ? pd : SWSE_FeatureDescribe(f),
                   refused ? " (not on this game build: safe mode)" : "");
        }
        SWSE_ConsolePrint("usage: features <name> on|off|auto [temp]   e.g. `features foliage on`");
        SWSE_ConsolePrint("presets: `features preset full` (the classic SWSE), `features preset default`, `features preset list`");
        SWSE_ConsolePrint("auto (aituning, playertune): runs only while aiprefs.txt / playerprefs.txt asks for something");
        if (SWSE_FeatureTotal() > FEAT_COUNT)
            SWSE_ConsolePrint("plugins load only when named: `features all on` skips them, `features all off` includes them");
        return;
    }
    bool temp = (argc > 3 && !lstrcmpiA(argv[3], "temp"));
    // `auto` (features.h): aituning and playertune follow their own file.
    if (!lstrcmpiA(argv[2], "auto")) {
        int f = SWSE_FeatureFind(argv[1]);
        if (f < 0 || !SWSE_FeatureAutoCapable((SwseFeature)f)) {
            SWSE_ConsolePrint("only aituning and playertune have an auto setting");
            return;
        }
        if (!temp) SWSE_FeatureSetSavedAuto((SwseFeature)f);
        char msg[400];
        bool was = SWSE_Feature((SwseFeature)f);
        SWSE_FeatureAutoEvaluate((SwseFeature)f, msg, sizeof(msg));
        SWSE_ConsolePrint(msg);
        if (SWSE_Feature((SwseFeature)f) != was) SWSE_PluginsNotifyFeature(f, !was);
        if (!temp) { char sm[200]; SWSE_FeaturesSave(sm, sizeof(sm)); SWSE_ConsolePrint(sm); }
        return;
    }
    bool on;
    if (!lstrcmpiA(argv[2], "on") || !lstrcmpiA(argv[2], "1"))       on = true;
    else if (!lstrcmpiA(argv[2], "off") || !lstrcmpiA(argv[2], "0")) on = false;
    else { SWSE_ConsolePrint("usage: features <name> on|off|auto [temp]"); return; }

    int first = 0, last = SWSE_FeatureTotal() - 1;
    if (lstrcmpiA(argv[1], "all")) {
        int f = SWSE_FeatureFind(argv[1]);
        if (f < 0) { Printf("no feature called '%s' - `features` lists them", argv[1]); return; }
        first = last = f;
    }
    int changed = 0;
    for (int i = first; i <= last; i++) {
        SwseFeature f = (SwseFeature)i;
        if (f == FEAT_CONSOLE && first != last) continue;     // `all` never touches it
        // `all on` never loads a plugin: a DLL runs only when the user names
        // it. `all off` does include them - it is the kill switch.
        if (on && first != last && SWSE_FeatureIsPlugin(i)) continue;
        if (f == FEAT_CONSOLE && !on) {
            // Only the file can do this - a live console cannot remove itself.
            if (temp) { SWSE_ConsolePrint("the console cannot be switched off for a session"); return; }
            SWSE_FeatureSetSaved(f, false);
            char sm[200]; SWSE_FeaturesSave(sm, sizeof(sm));
            SWSE_ConsolePrint("console = off saved to features.txt - takes effect next launch");
            SWSE_ConsolePrint("(with it off there is no console or remote mailbox to switch it back)");
            return;
        }
        if (SwitchOne(i, on, temp, first == last)) changed++;
    }
    SaveOrSay(changed, temp);
}


// ---- named positions, triggers, and the reserve pool ----------------------

static void Cmd_writepos(int argc, char** argv) {
    if (argc < 2) {
        SWSE_ConsolePrint("usage: writepos <label>    e.g. writepos enemyambush1");
        SWSE_ConsolePrint("saves where you are standing, by name, to positions.txt");
        return;
    }
    char msg[300];
    SWSE_PositionWrite(argv[1], msg, sizeof(msg));
    SWSE_ConsolePrint(msg);
}

// Floats for display. The console mostly prints integers; positions and
// facings want a decimal or two.
static void Ff(char* out, float v, int decimals) {
    // Truncating: `%.1f` of 1e30 is 40 characters, and sprintf_s answers an
    // overflow by ending the process (QA Q18: `yaw 1e30`).
    if (v != v) { lstrcpyA(out, "nan"); return; }
    float a = v < 0.0f ? -v : v;
    if (a >= 1.0e9f) _snprintf_s(out, 32, _TRUNCATE, "%.4g", v);
    else             _snprintf_s(out, 32, _TRUNCATE, "%.*f", decimals, v);
}

// A number typed at the console that the engine can safely take: finite and
// within +-1e6 (coordinates, facings). NaN, inf and 1e30 are refused.
static bool SaneFloat(const char* s, float* out) {
    char* e = nullptr;
    double d = strtod(s, &e);
    if (e == s || d != d || d > 1.0e6 || d < -1.0e6) return false;
    *out = (float)d;
    return true;
}

static void Cmd_positions(int, char**) {
    int n = SWSE_PositionCount();
    if (!n) {
        SWSE_ConsolePrint("no positions - stand somewhere and run: writepos <label>");
        return;
    }
    Printf("%d saved position(s):", n);
    for (int i = 0; i < n; i++) {
        float p[3]; const char* lvl = "";
        SWSE_PositionAt(i, p, &lvl);
        float yaw = 0; char ys[32] = "   -";
        if (SWSE_PositionYawAt(i, &yaw)) Ff(ys, yaw, 1);
        Printf("  %-22s %6d %6d %6d  yaw %6s  %s",
               SWSE_PositionName(i), (int)p[0], (int)p[1], (int)p[2], ys, lvl);
    }
}

// ---- did the teleport stick? (QA Q33) --------------------------------------------
// For a moment after a level load or CONTINUE the engine puts the player back
// where it had them, so "moved to" could be untrue. Each teleport is checked
// half a second later. If the player is nearer the spot they left than the
// spot they were sent to, the console says so. A level change in between
// cancels the check.
static struct { bool armed; DWORD due; unsigned epoch; float from[3], to[3]; } g_tpCheck;

static void TpCheckArm(const float* from, float x, float y, float z) {
    g_tpCheck.armed = (from != nullptr);
    if (!from) return;
    memcpy(g_tpCheck.from, from, sizeof(g_tpCheck.from));
    g_tpCheck.to[0] = x; g_tpCheck.to[1] = y; g_tpCheck.to[2] = z;
    g_tpCheck.due = GetTickCount() + 500;
    g_tpCheck.epoch = SWSE_LevelEpoch();
}

static float Dist2(const float* a, const float* b) {
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

static void TpCheckTick() {
    if (!g_tpCheck.armed || (int)(GetTickCount() - g_tpCheck.due) < 0) return;
    g_tpCheck.armed = false;
    if (SWSE_LevelEpoch() != g_tpCheck.epoch) return;
    float p[3];
    if (SWSE_PosGet(p) != 1) return;
    float dTo = Dist2(p, g_tpCheck.to), dFrom = Dist2(p, g_tpCheck.from);
    if (dTo <= 0.25f || dFrom >= dTo) return;                  // it stuck
    // Since 1.1 the move is the engine's own teleport, so this should stay
    // silent; it is kept as the independent check (research/TELEPORT.md).
    Printf("the teleport did not stick - you are back at %d %d %d (swse_log.txt has the "
           "teleport line); please report it", (int)p[0], (int)p[1], (int)p[2]);
}

// A refused or failed teleport, in words.
static void PrintTeleportFailure(int r) {
    const char* why = SWSE_TeleportWhy();
    if (r == 0)                      SWSE_ConsolePrint("teleport: no player (load a level first)");
    else if (r == -2)                SWSE_ConsolePrint("teleport faulted inside the game (see swse_log.txt)");
    else if (why && why[0])          Printf("teleport refused: %s", why);
    else                             SWSE_ConsolePrint("teleport refused");
}

static void Cmd_goto(int argc, char** argv) {
    if (argc < 2) { SWSE_ConsolePrint("usage: goto <label>"); return; }
    float p[3]; const char* lvl = "";
    if (!SWSE_PositionGet(argv[1], p, &lvl)) {
        Printf("no position called '%s' - see `positions`", argv[1]);
        return;
    }
    if (lvl && *lvl && SWSE_CurrentLevel()[0] && lstrcmpiA(lvl, SWSE_CurrentLevel()))
        Printf("warning: '%s' was saved in %s, you are in %s",
               argv[1], lvl, SWSE_CurrentLevel());
    float from[3]; bool haveFrom = (SWSE_PosGet(from) == 1);
    int tr = SWSE_PlayerTeleport(p[0], p[1], p[2]);
    if (tr != 1) { PrintTeleportFailure(tr); return; }
    TpCheckArm(haveFrom ? from : nullptr, p[0], p[1], p[2]);
    float yaw = 0;
    if (SWSE_PositionYaw(argv[1], &yaw) && SWSE_PlayerYawSet(yaw) == 1) {
        char ys[32]; Ff(ys, yaw, 1);
        Printf("moved to %s (%d %d %d), facing %s deg", argv[1],
               (int)p[0], (int)p[1], (int)p[2], ys);
    } else {
        Printf("moved to %s (%d %d %d)", argv[1], (int)p[0], (int)p[1], (int)p[2]);
    }
}

// tpxyz <x> <y> <z> [yaw] - teleport to explicit coordinates.
static void Cmd_tpxyz(int argc, char** argv) {
    if (argc < 4) { SWSE_ConsolePrint("usage: tpxyz <x> <y> <z> [yawDeg]   (see `pos`)"); return; }
    float x, y, z, yw = 0;
    if (!SaneFloat(argv[1], &x) || !SaneFloat(argv[2], &y) || !SaneFloat(argv[3], &z) ||
        (argc > 4 && !SaneFloat(argv[4], &yw))) {
        SWSE_ConsolePrint("tpxyz: each value must be a number within +-1000000");
        return;
    }
    float from[3]; bool haveFrom = (SWSE_PosGet(from) == 1);
    int tr = SWSE_PlayerTeleport(x, y, z);
    if (tr != 1) { PrintTeleportFailure(tr); return; }
    TpCheckArm(haveFrom ? from : nullptr, x, y, z);
    if (argc > 4) {
        if (SWSE_PlayerYawSet(yw) != 1)
            SWSE_ConsolePrint("(facing could not be set)");
    }
    Printf("moved to %d %d %d", (int)x, (int)y, (int)z);
}

// yaw [deg] - read or set the direction the player faces.
static void Cmd_yaw(int argc, char** argv) {
    if (argc > 1) {
        float d;
        if (!SaneFloat(argv[1], &d)) { SWSE_ConsolePrint("yaw: give a number of degrees"); return; }
        if (SWSE_PlayerYawSet(d) == 1) { char s[32]; Ff(s, d, 1); Printf("facing %s deg", s); }
        else SWSE_ConsolePrint("facing could not be set (no player yet?)");
        return;
    }
    float d = 0;
    if (SWSE_PlayerYawGet(&d) == 1) { char s[32]; Ff(s, d, 1); Printf("facing %s deg", s); }
    else SWSE_ConsolePrint("facing not readable (no player yet?)");
}

// spawnat <label|here> [count] [typehash]
static void Cmd_spawnat(int argc, char** argv) {
    if (argc < 2) {
        SWSE_ConsolePrint("usage: spawnat <label> [count] [typehash]");
        SWSE_ConsolePrint("  moves NPCs to a saved position - this is how an ambush works");
        return;
    }
    float p[3];
    if (!lstrcmpiA(argv[1], "here")) {
        if (!SWSE_PosGet(p)) { SWSE_ConsolePrint("no player position"); return; }
    } else if (!SWSE_PositionGet(argv[1], p, nullptr)) {
        Printf("no position called '%s' - see `positions`", argv[1]);
        return;
    }
    int count = (argc > 2) ? atoi(argv[2]) : 3;
    unsigned h = (argc > 3) ? (unsigned)strtoul(argv[3], nullptr, 16) : 0;
    char msg[300];
    int moved = SWSE_BringNpcsTo(p[0], p[1], p[2], count, h, msg, sizeof(msg));
    SWSE_ConsolePrint(msg);
    if (moved == 0)
        SWSE_ConsolePrint("(the engine cannot create NPCs - none were spare. try `reserve`)");
}

// reserve [n] - build a pool of spare NPCs and park them out of the way.
static void Cmd_reserve(int argc, char** argv) {
    if (argc > 1 && !lstrcmpiA(argv[1], "park")) {
        char msg[300];
        int n = SWSE_ReservePark(msg, sizeof(msg));
        SWSE_ConsolePrint(msg);
        (void)n;
        return;
    }
    int extra = (argc > 1) ? atoi(argv[1]) : 1;
    char msg[300];
    SWSE_ReserveBuild(extra, msg, sizeof(msg));
    SWSE_ConsolePrint(msg);
}

static void Cmd_triggers(int argc, char** argv) {
    char msg[300];
    if (argc > 1 && !lstrcmpiA(argv[1], "reload")) {
        int n = SWSE_TriggersLoad();
        SWSE_PositionsLoad();
        Printf("reloaded: %d trigger(s), %d position(s)", n, SWSE_PositionCount());
        return;
    }
    if (argc > 1 && !lstrcmpiA(argv[1], "on"))  { SWSE_TriggersEnable(1); SWSE_ConsolePrint("triggers ON"); return; }
    if (argc > 1 && !lstrcmpiA(argv[1], "off")) { SWSE_TriggersEnable(0); SWSE_ConsolePrint("triggers OFF"); return; }
    if (argc > 2 && !lstrcmpiA(argv[1], "test")) {
        SWSE_TriggerTest(argv[2], msg, sizeof(msg));
        SWSE_ConsolePrint(msg);
        return;
    }
    int n = SWSE_TriggerCount();
    Printf("triggers: %s, %d loaded", SWSE_TriggersEnabled() ? "ON" : "OFF", n);
    for (int i = 0; i < n; i++)
        Printf("  %-20s %-34s fired %d%s",
               SWSE_TriggerName(i), SWSE_TriggerWhen(i),
               SWSE_TriggerFireCount(i), SWSE_TriggerArmed(i) ? "" : "  (spent)");
    if (!n) SWSE_ConsolePrint("put a triggers.txt in any SWSEMods folder, then `triggers reload`");
    else    SWSE_ConsolePrint("`triggers test <name>` fires one now, ignoring chance");
}

static void Cmd_difficulty(int argc, char** argv) {
    char msg[220];
    if (argc < 2) {
        const char* a = SWSE_AiTuneActive();
        if (a && *a && SWSE_AiTuneActiveEpoch() != SWSE_LevelEpoch())
            // A level change built fresh prefs objects; the profile is only in
            // force here if the automatic apply (`active =`) ran again.
            Printf("AI profile: %s was applied in an earlier level - `difficulty %s` applies it here", a, a);
        else if (a && *a) Printf("AI profile: %s  (%d object(s) tuned)", a, SWSE_AiTuneCount());
        else SWSE_ConsolePrint("AI profile: off (shipped values)");
        SWSE_ConsolePrint("usage: difficulty <name|off>   e.g. `difficulty keen`");
        SWSE_ConsolePrint("profiles: keen, relentless, obvious - edit or add your own in");
        SWSE_ConsolePrint("  SWSEMods\\SWSE Console\\aiprefs.txt");
        SWSE_ConsolePrint("set `active = <name>` there to apply it on every level load");
        return;
    }
    int n = SWSE_AiTuneApply(argv[1], msg, sizeof(msg));
    SWSE_ConsolePrint(msg);
    if (n == 0 && lstrcmpiA(argv[1], "off"))
        SWSE_ConsolePrint("(no perception objects found - load a level with NPCs)");
}

static void Cmd_aitune(int, char**) {
    char msg[220];
    SWSE_AiTuneLoad(msg, sizeof(msg));
    SWSE_ConsolePrint(msg);
    if (SWSE_AiTuneActive()[0]) {
        SWSE_AiTuneRefresh();
        Printf("re-applied '%s'", SWSE_AiTuneActive());
    }
}

static void Cmd_findai(int argc, char** argv) {
    unsigned hits[16];
    int n = SWSE_FindAiPrefs(hits, 16, (argc > 1) ? atof(argv[1]) : 400.0);
    if (!n) {
        SWSE_ConsolePrint("no perception objects found (load a level with NPCs)");
        SWSE_ConsolePrint("try a bigger budget: findai 1500");
        return;
    }
    Printf("%d perception object(s):", n);
    for (int i = 0; i < n; i++) {
        float sd = 0, ha = 0, va = 0, s6 = 0;
        __try {
            s6 = *(float*)(hits[i] + 0x04);
            sd = *(float*)(hits[i] + 0x08);
            ha = *(float*)(hits[i] + 0x14);
            va = *(float*)(hits[i] + 0x18);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        Printf("  %08X  normal: 6th=%d see=%d cone=%dx%d",
               hits[i], (int)s6, (int)sd, (int)ha, (int)va);
    }
    SWSE_ConsolePrint("peek <addr> 12  - sightNormal at +4, Agit +A8, Combat +14C, Panic +1F0");
}

static void Cmd_uispy(int argc, char** argv) {
    char msg[200];
    if (argc > 1 && !lstrcmpiA(argv[1], "off")) {
        SWSE_UiSpy(0, msg, sizeof(msg));
        SWSE_ConsolePrint(msg);
        return;
    }
    if (argc > 1 && !lstrcmpiA(argv[1], "reset")) {
        SWSE_UiSpyReset();
        SWSE_ConsolePrint("uispy counters cleared");
        return;
    }
    if (argc > 1 && !lstrcmpiA(argv[1], "on")) {
        SWSE_UiSpy(1, msg, sizeof(msg));
        SWSE_ConsolePrint(msg);
        return;
    }
    if (!SWSE_UiSpyOn()) {
        SWSE_ConsolePrint("uispy is off - `uispy on`, then open a menu");
        return;
    }
    const char* names[24]; int counts[24];
    int n = SWSE_UiSpyStats(names, counts, 24);
    int shown = 0;
    for (int i = 0; i < n; i++) {
        if (!counts[i]) continue;
        Printf("  %-14s %d call(s)", names[i], counts[i]);
        shown++;
    }
    if (!shown) SWSE_ConsolePrint("no menu callbacks fired yet");
    SWSE_ConsolePrint("(details in swse_log.txt; `uispy reset` to isolate a press)");
}

static void Cmd_sendnpc(int argc, char** argv) {
    int n = (argc > 1) ? atoi(argv[1]) : 3;
    unsigned h = (argc > 2) ? (unsigned)strtoul(argv[2], nullptr, 16) : 0;
    char msg[220];
    SWSE_SendNpcs(n, h, msg, sizeof(msg));
    SWSE_ConsolePrint(msg);
}
static void Cmd_bring(int argc, char** argv) {
    int n = (argc > 1) ? atoi(argv[1]) : 3;
    unsigned h = (argc > 2) ? (unsigned)strtoul(argv[2], nullptr, 16) : 0;
    char msg[220];
    SWSE_BringNpcs(n, h, msg, sizeof(msg));
    SWSE_ConsolePrint(msg);
}
static void Cmd_npcnear(int, char**) {
    char msg[220];
    SWSE_NpcNear(msg, sizeof(msg));
    SWSE_ConsolePrint(msg);
}
static void Cmd_spawntypes(int, char**) {
    int n = SWSE_NpcTypeCount();
    if (!n) {
        SWSE_ConsolePrint("no types yet - run npcspy, then warp or load a save");
        return;
    }
    char line[240] = "";
    for (int i = 0; i < n; i++) {
        char one[24];
        wsprintfA(one, "%d:%08X ", i, SWSE_NpcTypeHash(i));
        if (lstrlenA(line) + lstrlenA(one) >= 230) { SWSE_ConsolePrint(line); line[0] = 0; }
        lstrcatA(line, one);
    }
    if (line[0]) SWSE_ConsolePrint(line);
    Printf("%d NPC type(s) seen since npcspy was armed - safe to dupetype here", n);
}
static void Cmd_npcreplay(int, char**) {
    char msg[220];
    SWSE_NpcReplay(msg, sizeof(msg));
    SWSE_ConsolePrint(msg);
}
static void Cmd_npchits(int, char**) {
    Printf("spawn routine has run %d time(s) since npcspy was armed", SWSE_NpcHits());
}
static void Cmd_dupetype(int argc, char** argv) {
    unsigned h = (argc > 1) ? (unsigned)strtoul(argv[1], nullptr, 16) : 0;
    SWSE_NpcRoutineSpy(1);
    if (!h) { SWSE_DupeType(0); SWSE_ConsolePrint("dupetype off"); return; }
    // A character whose assets the target level does not load will CRASH it on
    // spawn -- the hash still resolves, but its geometry is not in the bundle.
    // Only types harvested from a level are known-safe there, so require one
    // unless the second argument explicitly says otherwise.
    bool known = false;
    for (int i = 0; i < SWSE_NpcTypeCount(); i++)
        if (SWSE_NpcTypeHash(i) == h) { known = true; break; }
    // dupetype <hash> [everyN] [force] -- everyN keeps heavy characters from
    // replacing an entire level, which is what crashed it with 157 bosses.
    bool force = false;
    int every = 1;
    for (int i = 2; i < argc; i++) {
        if (!lstrcmpiA(argv[i], "force")) force = true;
        else if (atoi(argv[i]) > 0)       every = atoi(argv[i]);
    }
    SWSE_DupeEvery(every);
    if (!known && !force) {
        Printf("%08X is not among the %d types harvested here - it would CRASH.",
               h, SWSE_NpcTypeCount());
        SWSE_ConsolePrint("  warp there with npcspy on first, or: dupetype <hash> force");
        return;
    }
    SWSE_DupeType(h);
    if (every > 1)
        Printf("dupetype = %08X, every %dth spawn%s - now warp",
               h, every, known ? "" : " (FORCED)");
    else
        Printf("dupetype = %08X, ALL spawns%s - now warp (use 'dupetype %08X 10' for fewer)",
               h, known ? "" : " (FORCED - may crash)", h);
}
static void Cmd_buildtest(int argc, char** argv) {
    int n = (argc > 1) ? atoi(argv[1]) : 3;
    SWSE_NpcRoutineSpy(1);
    SWSE_NpcBuildTest(n);
    Printf("buildtest = %d - now warp; constructed spawns run DURING the load", n);
}
static void Cmd_npccount(int argc, char** argv) {
    int n = (argc > 1) ? atoi(argv[1]) : 4;
    SWSE_NpcRoutineSpy(1);
    SWSE_NpcCount(n);
    Printf("npccount = %d - now warp or load a save", n);
}
// Quit the game. Exists so the DLL can be reinstalled without the player
// having to alt-tab and close it by hand -- that round trip dominated the
// development loop. ExitProcess rather than a clean shutdown: we want it gone
// promptly, and nothing here is worth saving.
// The SSR material-mask instrument. "ssrmask" prints the whole chain's
// state; "ssrmask reload" re-reads materials.txt so the owner can grade
// more textures without a restart.
static void Cmd_ssrmask(int argc, char** argv) {
    if (argc > 1 && !lstrcmpiA(argv[1], "reload")) {
        int n = SWSE_MaterialsReload();
        Printf("reloaded materials.txt: %d fingerprint(s)", n);
        return;
    }
    if (argc > 1 && !lstrcmpiA(argv[1], "alpha")) {
        int mn = 0, mx = 0, mean = 0, pct = 0;
        if (SWSE_GfxMaskAlphaStats(&mn, &mx, &mean, &pct))
            Printf("capture alpha: min=%d max=%d mean=%d nonzero=%d%%", mn, mx, mean, pct);
        else
            SWSE_ConsolePrint("no RGBA capture yet (need ssr_mask_stamp 1 + F10 on)");
        return;
    }
    Printf("fingerprints loaded : %d", SWSE_MaterialsListN());
    Printf("texids flagged live : %d", SWSE_MaterialsKnownTexids());
    Printf("refl binds last frame: %d", SWSE_MaterialsBindsLastFrame());
    Printf("stamping active     : %s", SWSE_MaterialsStampState() ? "yes" : "no (set ssr_mask_stamp 1)");
    Printf("frame marks / suspended now: %d / %s", SWSE_MaterialsMarks(),
           SWSE_MaterialsSuspended() ? "yes" : "no");
    Printf("draw hook: %s, draws/frame %d, mask redraws/frame %d, glsl skips %d",
           SWSE_MaterialsDrawHooked() ? "installed" : "NOT installed",
           SWSE_MaterialsDrawsSeenLastFrame(), SWSE_MaterialsRedrawsLastFrame(),
           SWSE_MaterialsGlslSkipsLastFrame());
    {
        int sp = 0, dp = 0, ld = 0, lf = 0, lh = 0, hf = 0;
        SWSE_GfxMaskDebugInfo(&sp, &dp, &ld, &lf, &lh, &hf);
        Printf("gfx params x100: stamp=%d debug=%d | locs: dbg=%d fbo=%d has=%d | fboBound=%d",
               sp, dp, ld, lf, lh, hf);
    }
    Printf("gbuf: draws/frame %d, skinned skips %d",
           SWSE_MaterialsGBufDrawsLastFrame(), SWSE_MaterialsSkinSkipsLastFrame());
    SWSE_ConsolePrint("chain: stamp -> RGBA capture -> uScene.a -> gate (debug_ssrmask x-rays it)");
}

// Can this 2005 context run modern compute - and how much ray math does the
// GPU deliver through it? Runs at swap time, where a GL context is current.
// One frame hitches (the probe calls glFinish to time honestly).
static void GpuEmit(const char* line) { SWSE_ConsolePrint(line); }

static void Cmd_gpu(int, char**) {
    SWSE_ConsolePrint("probing GPU compute (this frame will hitch)...");
    SWSE_GpuProbe(GpuEmit);
}

// Can a ray tracer read the geometry the engine draws, and is it world space?
// Arms a one-frame capture; results are read back with 'geo show' because the
// draws happen after this command's response has already been sent.
// The ray-tracing tools need the tracer switched on: with it off the draw
// hook they read from is not installed, and an armed capture waits forever.
static bool RtGate() {
    if (SWSE_Feature(FEAT_RAYTRACE)) return true;
    SWSE_ConsolePrint("ray tracing is off (the default) - `features graphics on`, then `features raytrace on`");
    return false;
}

static void Cmd_geo(int argc, char** argv) {
    if (!RtGate()) return;
    if (argc > 1 && !lstrcmpiA(argv[1], "show")) { SWSE_GeoReport(GpuEmit); return; }
    SWSE_GeoArm(4);
    SWSE_ConsolePrint("armed: capturing the next frame's world draws - run 'geo show'");
}

// Stage 1b: BVH over the harvested world, and the camera trace that proves it.
static void Cmd_rt(int argc, char** argv) {
    if (!RtGate()) return;
    if (argc > 1 && !lstrcmpiA(argv[1], "build")) { SWSE_RtBuild(GpuEmit); return; }
    if (argc > 2 && !lstrcmpiA(argv[1], "nearz")) {
        SWSE_RtSetNearZ((float)atof(argv[2]));
        Printf("rt: near-plane NDC z = %s", argv[2]);
        return;
    }
    if (argc > 2 && !lstrcmpiA(argv[1], "mode")) {
        SWSE_RtSetMode((float)atof(argv[2]));
        Printf("rt: view mode %s (0 depth, 1 ray direction, 2 ray origin)", argv[2]);
        return;
    }
    if (argc > 2 && !lstrcmpiA(argv[1], "flipy")) {
        SWSE_RtSetFlipY((float)atof(argv[2]));
        Printf("rt: flip y = %s", argv[2]);
        return;
    }
    if (argc > 2 && !lstrcmpiA(argv[1], "cardsize")) {
        SWSE_RtCardSize((float)atof(argv[2]));
        Printf("rt: leaf-card area threshold = %s (now run 'rt build')", argv[2]);
        return;
    }
    // One switch for the whole live pipeline: keep harvesting as the player
    // moves, and rebuild the BVH when enough new geometry has arrived.
    if (argc > 1 && !lstrcmpiA(argv[1], "live")) {
        int on = (argc > 2) ? atoi(argv[2]) : 1;
        int th = (argc > 3) ? atoi(argv[3]) : 150;
        SWSE_GeoHarvestContinuous(on);
        SWSE_RtAutoBuild(on, th);
        Printf("rt: LIVE harvest %s, auto-rebuild every %d new meshes",
               on ? "ON" : "off", th);
        return;
    }
    if (argc > 2 && !lstrcmpiA(argv[1], "fovscale")) {
        SWSE_RtFovScale((float)atof(argv[2]));
        Printf("rt: FOV scale = %s (1 = the derived matrix's own FOV)", argv[2]);
        return;
    }
    if (argc > 1 && !lstrcmpiA(argv[1], "aoclamp")) {
        if (argc > 2) SWSE_RtAoClamp((float)atof(argv[2]));
        Printf("rt: history clamp = %d/1000 of the current estimate%s",
               (int)(SWSE_RtAoGetClamp() * 1000.0f),
               SWSE_RtAoGetClamp() > 0.0f ? "" : "  (OFF - history is unbounded)");
        return;
    }
    if (argc > 2 && !lstrcmpiA(argv[1], "pairprev")) {
        SWSE_RtPairPrev(atoi(argv[2]));
        Printf("rt: pair depth with previous frame's camera = %s", argv[2]);
        return;
    }
    if (argc > 1 && !lstrcmpiA(argv[1], "fovsweep")) {
        SWSE_RtFovSweep();
        SWSE_ConsolePrint("rt: sweeping FOV scale, minimising |Pdepth-hp| -> swse_log.txt");
        return;
    }
    if (argc > 1 && !lstrcmpiA(argv[1], "camerr")) {
        SWSE_RtErrProbe();
        SWSE_ConsolePrint("rt: camera error probe armed - needs 'set rtao_strength -6'; result in swse_log.txt");
        return;
    }
    if (argc > 2 && !lstrcmpiA(argv[1], "clipyneg")) {
        SWSE_RtClipYNeg(atoi(argv[2]));
        Printf("rt: clip-y negation = %s", argv[2]);
        return;
    }
    if (argc > 2 && !lstrcmpiA(argv[1], "cardcell")) {
        SWSE_RtCardCell((float)atof(argv[2]));
        Printf("rt: leaf-card hash cells/unit = %s (immediate)", argv[2]);
        return;
    }
    if (argc > 2 && !lstrcmpiA(argv[1], "cardopacity")) {
        SWSE_RtCardOpacity((float)atof(argv[2]));
        Printf("rt: leaf-card opacity = %s (takes effect immediately)", argv[2]);
        return;
    }
    if (argc > 2 && !lstrcmpiA(argv[1], "cardany")) {
        SWSE_RtCardAny(atoi(argv[2]));
        Printf("rt: drop big triangles from ANY program = %s (now 'rt build')", argv[2]);
        return;
    }
    if (argc > 2 && !lstrcmpiA(argv[1], "foliage")) {
        SWSE_GeoExcludeFoliage(atoi(argv[2]));
        Printf("rt: exclude foliage-program draws = %s (re-harvest, then 'rt build')", argv[2]);
        return;
    }
    if (argc > 1 && !lstrcmpiA(argv[1], "tiltlog")) {
        int n = (argc > 2) ? atoi(argv[2]) : 30;
        SWSE_WindTiltLog(n);
        Printf("rt: reporting tilt-test survivors for %d frames -> swse_log.txt", n);
        return;
    }
    if (argc > 1 && !lstrcmpiA(argv[1], "camlog")) {
        int n = (argc > 2) ? atoi(argv[2]) : 60;
        SWSE_RtCamLog(n);
        Printf("rt: logging the derived camera for %d frames -> swse_log.txt", n);
        return;
    }
    if (argc > 2 && !lstrcmpiA(argv[1], "all")) {
        SWSE_RtSetUseAll(atoi(argv[2]));
        Printf("rt: keep float-attribute draws = %s (run 'rt build' again)", argv[2]);
        return;
    }
    if (argc > 2 && !lstrcmpiA(argv[1], "dist")) {
        SWSE_RtSetMaxDist((float)atof(argv[2]));
        Printf("rt: trace range = %s", argv[2]);
        return;
    }
    SWSE_RtStatus(GpuEmit);
}

static void Cmd_progsrc(int argc, char** argv) {
    if (argc < 2) { SWSE_ConsolePrint("usage: progsrc <program id>"); return; }
    SWSE_GeoDumpProgram((unsigned)strtoul(argv[1], nullptr, 10), GpuEmit);
}

// skin, meshdump, hidepart, nohat, noponcho and nodreads all work through the
// graphics pipeline's glDrawElements hook (materials.cpp), which goes in when
// the graphics feature starts. Without it they armed a census or a hide that
// nothing ever ran - meshdump wrote an empty file - so they say so instead.
// What only undoes or reports (show, list, clear, off, stop) needs no hook.
static bool DrawHookGate(const char* cmd) {
    if (!SWSE_Feature(FEAT_GRAPHICS)) {
        Printf("%s needs graphics on (it works through the pipeline's draw hook) - `features graphics on`", cmd);
        return false;
    }
    if (!SWSE_MaterialsDrawHooked()) {
        Printf("%s: the pipeline's draw hook is not installed (see swse_log.txt) - nothing to work through", cmd);
        return false;
    }
    return true;
}

// Stage 1: harvest the whole visible world as a world-space triangle soup.
// Skinned-draw census for the Oddview model-format work: 'skin' arms it,
// 'skin show' reports. Characters are excluded from the geometry probe, so
// this is the only way to see their vertex counts.
// 'meshdump' captures a few frames of character geometry for Oddview, then
// 'meshdump stop' closes the file.
static void Cmd_meshdump(int argc, char** argv) {
    if (argc > 1 && !lstrcmpiA(argv[1], "stop")) {
        int n = SWSE_MeshDumpFinish();
        Printf("meshdump: wrote %d meshes to bin\\swse_meshes.odv", n);
        return;
    }
    if (!DrawHookGate("meshdump")) return;
    char path[260] = { 0 };
    if (SWSE_MeshDumpStart(path, sizeof(path)))
        Printf("meshdump: capturing character geometry -> %s   ('meshdump stop' to close)", path);
    else
        SWSE_ConsolePrint("meshdump: could not open the output file");
}
// Hide a character part by its index count - the value 'skin show' prints.
//   hidepart <n>      hide the piece drawn with n indices
//   hidepart show <n> put it back
//   hidepart list     what is hidden
//   hidepart clear    show everything again
static void Cmd_hidepart(int argc, char** argv) {
    if (argc > 1 && !lstrcmpiA(argv[1], "clear")) {
        SWSE_MaterialsHideClear();
        SWSE_ConsolePrint("hidepart: all parts visible again");
        return;
    }
    if (argc > 1 && !lstrcmpiA(argv[1], "list")) {
        unsigned h[16];
        int n = SWSE_MaterialsHideList(h, 16);
        if (!n) { SWSE_ConsolePrint("hidepart: nothing hidden"); return; }
        for (int i = 0; i < n; i++) Printf("  hidden: %u indices", h[i]);
        return;
    }
    if (argc > 2 && !lstrcmpiA(argv[1], "show")) {
        SWSE_MaterialsHidePart((unsigned)atoi(argv[2]), 0);
        Printf("hidepart: %s visible again", argv[2]);
        return;
    }
    if (argc < 2) {
        SWSE_ConsolePrint("usage: hidepart <indexCount> | show <n> | list | clear");
        SWSE_ConsolePrint("run 'skin' then 'skin show' to see the index counts");
        return;
    }
    if (!DrawHookGate("hidepart")) return;
    int n = SWSE_MaterialsHidePart((unsigned)atoi(argv[1]), 1);
    Printf("hidepart: hiding the %s-index part (%d hidden)", argv[1], n);
}

// Stranger's poncho and hat are separate skinned draws. These are named
// wrappers over hidepart, using the index counts measured in play.
// Measured in play by hiding each index count from the 'skin show' table and
// looking at the result.
//   272 / 1567  poncho (and the hat rides the same draw)
//   1173        dreadlocks
//   5688        eyeballs  (also the source of the stray eye-fan triangles the
//                          world harvest kept picking up)
//   22145       body
//
// A name holds a LIST, not one number, because the same garment is drawn with
// DIFFERENT index counts in different places - the poncho is 272 inside the
// bounty office and 1567 out in the street. One fixed count silently stops
// working when you walk through a door, which is exactly what happened on the
// first attempt. Teaching a count ADDS to the list rather than replacing it.
#define NAMED_MAX 8
struct NamedPart { unsigned c[NAMED_MAX]; int n; };
static NamedPart g_poncho = { { 272, 1567 }, 2 };
static NamedPart g_hat    = { { 272, 1567 }, 2 };
static NamedPart g_dreads = { { 1173 }, 1 };

static void ToggleNamed(const char* what, NamedPart* p, int argc, char** argv) {
    if (argc > 1 && (!lstrcmpiA(argv[1], "off") || !lstrcmpiA(argv[1], "show"))) {
        for (int i = 0; i < p->n; i++) SWSE_MaterialsHidePart(p->c[i], 0);
        Printf("%s: back on", what);
        return;
    }
    if (!DrawHookGate(what)) return;
    if (argc > 1 && atoi(argv[1]) > 0) {
        unsigned v = (unsigned)atoi(argv[1]);
        int have = 0;
        for (int i = 0; i < p->n; i++) if (p->c[i] == v) have = 1;
        if (!have && p->n < NAMED_MAX) p->c[p->n++] = v;
    }
    if (!p->n) {
        Printf("%s: which draw is it? run 'skin' then 'skin show', then", what);
        Printf("  %s <indexCount>   - it is remembered after that", argv[0]);
        return;
    }
    for (int i = 0; i < p->n; i++) SWSE_MaterialsHidePart(p->c[i], 1);
    char b[160]; int o = 0;
    for (int i = 0; i < p->n && o < 120; i++)
        o += wsprintfA(b + o, i ? ", %u" : "%u", p->c[i]);
    Printf("%s: hidden (%s indices)", what, b);
}
static void Cmd_noponcho(int argc, char** argv) { ToggleNamed("noponcho", &g_poncho, argc, argv); }
static void Cmd_nohat(int argc, char** argv)    { ToggleNamed("nohat",    &g_hat,    argc, argv); }
static void Cmd_nodreads(int argc, char** argv) { ToggleNamed("nodreads", &g_dreads, argc, argv); }

static void Cmd_skin(int argc, char** argv) {
    if (argc > 1 && !lstrcmpiA(argv[1], "show")) { SWSE_SkinCensusReport(GpuEmit); return; }
    if (!DrawHookGate("skin")) return;
    SWSE_SkinCensusArm();
    SWSE_ConsolePrint("skin: capturing this frame's CHARACTER draws - run 'skin show'");
}
static void Cmd_harvest(int argc, char** argv) {
    if (!RtGate()) return;
    if (argc > 1 && !lstrcmpiA(argv[1], "show")) { SWSE_GeoHarvestReport(GpuEmit); return; }
    if (argc > 1 && !lstrcmpiA(argv[1], "dump")) {
        char path[MAX_PATH] = { 0 };
        int n = SWSE_GeoHarvestDumpObj(path, MAX_PATH, (argc > 2) ? atoi(argv[2]) : 0);
        if (n) Printf("wrote %d triangles -> %s", n, path);
        else   SWSE_ConsolePrint("nothing harvested yet");
        return;
    }
    SWSE_GeoHarvestArm();
    SWSE_GeoHarvestArmRetries(20);   // wait for the camera after a level load
    // Default 1: multi-frame accumulation dedups by buffer identity, which
    // MERGES INSTANCES - the engine draws one mesh many times at different
    // places (884 draws/frame collapse to 339 keys), so anything above 1
    // currently loses most of the world. Kept for when instance identity
    // (quantised world position) is added to the key.
    int frames = (argc > 1) ? atoi(argv[1]) : 1;
    if (frames < 1) frames = 1;
    SWSE_GeoHarvestFrames(frames);
    Printf("harvesting %d frames (brief hitch) - then 'harvest show'", frames);
}

static void Cmd_exit(int, char**) {
    SWSE_ConsolePrint("exiting...");
    ExitProcess(0);
}
// These scans are the measuring instruments for whether a spawn actually
// happened. A 64-entry buffer silently reported exactly "64" for any larger
// level, which read as a real count and wasn't one -- hence a shared big
// buffer and an explicit warning when a result lands on the cap.
#define SCAN_MAX 1024
static unsigned g_scanBuf[SCAN_MAX];
static void Cmd_geominst(int, char**) {
    int n = SWSE_FindGeomInst(g_scanBuf, SCAN_MAX);
    if (!n) { SWSE_ConsolePrint("no GeometryInst found"); return; }
    Printf("%d GeometryInst(s) - positions logged (these are spawn anchors)%s", n,
           (n == SCAN_MAX) ? " - AT THE CAP" : "");
}
static void Cmd_npctypes(int, char**) {
    unsigned prefs[64];
    int n = SWSE_FindNpcPrefs(prefs, 64);
    if (!n) { SWSE_ConsolePrint("no NPC type definitions found"); return; }
    Printf("%d spawnable NPC type(s) - health/geometry logged for each", n);
}
static void Cmd_npcs(int, char**) {
    int n = SWSE_FindNpcs(g_scanBuf, SCAN_MAX);
    if (!n) { SWSE_ConsolePrint("no live NPCs found"); return; }
    Printf("%d live NPC(s)%s", n, (n == SCAN_MAX) ? " - AT THE CAP, count is unreliable" : "");
}
static void Cmd_npctags(int, char**) {
    int n = SWSE_NpcTags(g_scanBuf, SCAN_MAX);
    if (!n) { SWSE_ConsolePrint("no NPCTag objects in this level"); return; }
    Printf("%d NPCTag(s) in this level%s", n, (n == SCAN_MAX) ? " - AT THE CAP" : "");
}
static void Cmd_spawnclone(int, char**) {
    char msg[256] = {0};
    SWSE_SpawnCloned(msg, sizeof(msg));
    SWSE_ConsolePrint(msg);
}
static void Cmd_spawnnpc(int argc, char** argv) {
    char msg[256] = {0};
    SWSE_SpawnNpc((argc > 1) ? atoi(argv[1]) : 0, msg, sizeof(msg));
    SWSE_ConsolePrint(msg);
}
static void Cmd_npcspawn(int argc, char** argv) {
    char msg[256] = {0};
    SWSE_NpcRoutineRun(0, msg, sizeof(msg), (argc > 1) ? atoi(argv[1]) : 0);
    SWSE_ConsolePrint(msg);
}
static void Cmd_npclast(int, char**) {
    char msg[256] = {0};
    SWSE_NpcLast(msg, sizeof(msg));
    SWSE_ConsolePrint(msg);
}
static void Cmd_npchere(int, char**) {
    char msg[256] = {0};
    SWSE_NpcLastAt(msg, sizeof(msg), true);
    SWSE_ConsolePrint(msg);
}
static void Cmd_critters(int argc, char** argv) {
    int c = (argc > 1) ? atoi(argv[1]) : 0;
    if (c < 0) c = 0;
    int prefs = 0;
    int paths = SWSE_Critters(c, &prefs);
    if (!paths && !prefs) { SWSE_ConsolePrint("no critter paths in this level"); return; }
    if (c) Printf("enabled %d critter path(s), %d prefs set to %d each", paths, prefs, c);
    else   Printf("%d critter path(s), %d prefs (see log). 'critters <n>' to enable.", paths, prefs);
}
static void Cmd_autoprime(int, char**) {
    if (SWSE_AutoPrime()) SWSE_ConsolePrint("primed - script commands available (no ammo needed)");
    else SWSE_ConsolePrint("no ScriptContext found yet - load a save, then retry");
}
static void Cmd_grantlast(int argc, char** argv) {
    int qty = (argc > 1) ? atoi(argv[1]) : 1;
    if (qty < 1) qty = 1;
    char msg[256] = {0};
    SWSE_GrantLast(qty, msg, sizeof(msg));
    SWSE_ConsolePrint(msg);
}
static void Cmd_grantspy(int argc, char** argv) {
    int on = !(argc > 1 && !lstrcmpiA(argv[1], "off"));
    int r = SWSE_GrantSpy(on);
    if (r != 1)   { SWSE_ConsolePrint("could not install the grant hook"); return; }
    if (!on)      { SWSE_ConsolePrint("grant spy OFF"); return; }
    SWSE_ConsolePrint("grant spy ON - now BUY an artifact.");
    SWSE_ConsolePrint("the log records the exact item name the store uses.");
}
static void Cmd_moolah(int argc, char** argv) {
    if (argc > 1) {
        int r = SWSE_SetMoolah((float)atoi(argv[1]));
        if (r == 1)      Printf("moolah set to %d", atoi(argv[1]));
        else if (r == 0) SWSE_ConsolePrint("no wallet - load a save first");
        else             SWSE_ConsolePrint("write faulted");
        return;
    }
    float v = 0;
    int r = SWSE_Moolah(&v);
    if (r == 1)      Printf("moolah = %d  (wallet %08X)", (int)v,
                            (unsigned)(uintptr_t)SWSE_WalletObj());
    else if (r == 0) SWSE_ConsolePrint("no wallet - load a save first");
    else             SWSE_ConsolePrint("read faulted");
}
// ---- exact-value search: anchor on a number you can SEE (moolah) ----
static void Cmd_findval(int argc, char** argv) {
    if (argc < 2) {
        SWSE_ConsolePrint("usage: findval <number>   (e.g. your moolah count)");
        return;
    }
    int v = atoi(argv[1]);
    int n = SWSE_FindValue(v, 64);
    Printf("%d address(es) hold %d - listed in the log", n, v);
    if (n > 1) SWSE_ConsolePrint("spend some moolah, then: narrow <newAmount>");
    else if (n == 1) Printf("that's the one: watchaddr %08X", SWSE_FindHit(0));
}
static void Cmd_narrow(int argc, char** argv) {
    if (argc < 2) { SWSE_ConsolePrint("usage: narrow <newNumber>"); return; }
    int n = SWSE_FindNarrow(atoi(argv[1]));
    Printf("%d address(es) left", n);
    if (n >= 1) Printf("use: watchaddr %08X", SWSE_FindHit(0));
}
// ---- player stats via the getter chain (no priming needed) ----
static void ShowTriple(const char* label, int r, float a, float b, float c) {
    if (r == 1) Printf("%s: %d / %d / %d  (current/max/base)",
                       label, (int)a, (int)b, (int)c);
    else if (r == 0) SWSE_ConsolePrint("no player object (load a save first)");
    else SWSE_ConsolePrint("read faulted");
}
static void Cmd_hp(int argc, char** argv) {
    if (argc > 1) {
        float v = (float)atof(argv[1]);
        int r = SWSE_PlayerSetHealth(v);
        if (r == 1) Printf("health set to %d", (int)v);
        else if (r == 0) SWSE_ConsolePrint("no player object (load a save first)");
        else SWSE_ConsolePrint("write faulted");
        return;
    }
    float a = 0, b = 0, c = 0;
    ShowTriple("health", SWSE_PlayerHealth(&a, &b, &c), a, b, c);
}
static void Cmd_stam(int argc, char** argv) {
    if (argc > 1) {
        float v = (float)atof(argv[1]);
        int r = SWSE_PlayerSetStamina(v);
        if (r == 1) Printf("stamina set to %d", (int)v);
        else if (r == 0) SWSE_ConsolePrint("no player object (load a save first)");
        else SWSE_ConsolePrint("write faulted");
        return;
    }
    float a = 0, b = 0, c = 0;
    ShowTriple("stamina", SWSE_PlayerStamina(&a, &b, &c), a, b, c);
}
static void Cmd_pfield(int argc, char** argv) {
    if (argc < 2) { SWSE_ConsolePrint("usage: pfield <hexOffset> [value]  (explore player fields)"); return; }
    int off = (int)strtoul(argv[1], nullptr, 16);
    if (argc > 2) {
        int r = SWSE_PlayerSet(off, (float)atof(argv[2]));
        Printf(r == 1 ? "player+0x%X set" : "player+0x%X failed", off);
        return;
    }
    float a = 0, b = 0, c = 0;
    int r = SWSE_PlayerGet(off, &a, &b, &c);
    if (r == 1) Printf("player+0x%X: %d / %d / %d", off, (int)a, (int)b, (int)c);
    else SWSE_ConsolePrint("read failed");
}
static void Cmd_watchinv(int, char**) {
    int r = SWSE_WatchInventory();
    if (r == 1) {
        SWSE_ConsolePrint("watching the inventory pointer.");
        SWSE_ConsolePrint("now BUY AN ARTIFACT - the log records what code adds it.");
    } else if (r == 0) SWSE_ConsolePrint("no player object yet (load a save)");
    else               SWSE_ConsolePrint("could not arm the watchpoint");
}
static void Cmd_watchaddr(int argc, char** argv) {
    if (argc < 2) { SWSE_ConsolePrint("usage: watchaddr <hexAddress>"); return; }
    unsigned a = strtoul(argv[1], nullptr, 16);
    int r = SWSE_WatchWrite(a);
    if (r == 1)      Printf("watching writes to %08X - go trigger it.", a);
    else if (r == 0) SWSE_ConsolePrint("address out of range");
    else             SWSE_ConsolePrint("could not arm the watchpoint");
}
static void Cmd_watchrw(int argc, char** argv) {
    if (argc < 2) { SWSE_ConsolePrint("usage: watchrw <hexAddr> [len 1|2|4]"); return; }
    unsigned a = strtoul(argv[1], nullptr, 16);
    int len = (argc > 2) ? atoi(argv[2]) : 4;
    int r = SWSE_WatchRW(a, len);
    if (r == 1)      Printf("watching reads AND writes of %08X", a);
    else if (r == 0) SWSE_ConsolePrint("address out of range");
    else             SWSE_ConsolePrint("could not arm the watchpoint");
}
static void Cmd_watchexec(int argc, char** argv) {
    if (argc < 2) { SWSE_ConsolePrint("usage: watchexec <hexRVA>   e.g. watchexec 230A0"); return; }
    unsigned r = strtoul(argv[1], nullptr, 16);
    // "once" captures a single hit then disarms. Essential on per-frame paths:
    // a free-running breakpoint on the Granny pose builder wedged the game.
    int once = 0;
    for (int i = 2; i < argc; i++) if (!lstrcmpiA(argv[i], "once")) once = 1;
    int rc = SWSE_WatchExec(r, once);
    if (rc == 1) Printf("watching for execution of module+0x%X", r);
    else         SWSE_ConsolePrint("could not arm the execution breakpoint");
}
static void Cmd_watchoff(int, char**) {
    SWSE_WatchOff();
    SWSE_ConsolePrint("watchpoint disarmed - see the log.");
}
static void Cmd_spy(int argc, char** argv) {
    if (argc > 1 && !lstrcmpiA(argv[1], "off")) {
        SWSE_SpyStop();
        SWSE_ConsolePrint("spy OFF - hit counts written to the log.");
        return;
    }
    int budget = (argc > 1) ? atoi(argv[1]) : 200;
    int n = SWSE_SpyStart(budget);
    Printf("spy ON - %d functions watched, %d lines budget.", n, budget);
    SWSE_ConsolePrint("play normally (buy an artifact); then 'spy off'.");
}
static void Cmd_invdump(int, char**) {
    int r = SWSE_DumpInventory();
    if (r == 1)      SWSE_ConsolePrint("inventory object dumped to bin\\swse_log.txt");
    else if (r == 0) SWSE_ConsolePrint("getter returned null (load a save first)");
    else             SWSE_ConsolePrint("getter faulted");
}
static void Cmd_artifacts(int argc, char** argv) {
    const char* filt = (argc > 1) ? argv[1] : "";
    SWSE_ConsolePrint("artifacts (use: giveartifact <name>):");
    char row[160]; int col = 0; row[0] = 0; int n = 0;
    for (int i = 0; i < N_ARTIFACTS; i++) {
        if (*filt && !ContainsCI(kArtifacts[i], filt)) continue;
        char cell[40]; wsprintfA(cell, "%-26s", kArtifacts[i]);
        lstrcatA(row, cell); n++;
        if (++col == 3) { SWSE_ConsolePrint(row); row[0] = 0; col = 0; }
    }
    if (col) SWSE_ConsolePrint(row);
    Printf("%d artifact(s)%s", n, *filt ? " matching" : "");
}
// Both of these now go through the store's own grant path (SWSE_GrantItem).
// The old script-VM route needed an ArtifactPref, faulted on both calling
// conventions, and never actually delivered an item - see 'grant'.
static void Cmd_giveartifact(int argc, char** argv) {
    if (argc < 2) { SWSE_ConsolePrint("usage: giveartifact <name>   (see 'artifacts')"); return; }
    int qty = (argc > 2) ? atoi(argv[2]) : 1;
    if (qty < 1) qty = 1;
    char msg[256] = {0};
    SWSE_GrantItem(argv[1], qty, msg, sizeof(msg));
    SWSE_ConsolePrint(msg);
}
static void Cmd_allartifacts(int, char**) {
    // Probe with one first so a broken path reports once instead of 53 times.
    char msg[256] = {0};
    if (SWSE_GrantItem(kArtifacts[0], 1, msg, sizeof(msg)) != 1) {
        Printf("grant path not working [%s] - aborting.", msg);
        return;
    }
    int ok = 1;
    for (int i = 1; i < N_ARTIFACTS; i++)
        if (SWSE_GrantItem(kArtifacts[i], 1, msg, sizeof(msg)) == 1) ok++;
    Printf("allartifacts: %d/%d granted", ok, N_ARTIFACTS);
}
// ---- pointer chains: direct memory read/write, no script VM involved -----
static void PrintChain(int i) {
    char trace[200] = {0};
    float f = 0; int v = 0;
    bool ok = SWSE_PtrRead(i, &f, &v, trace, 200);
    if (!ok) { Printf("  %-14s <unresolved>", SWSE_PtrName(i)); return; }
    if (SWSE_PtrType(i) == 'f') {
        int whole = (int)f, frac = (int)((f < 0 ? -f : f) * 1000) % 1000;
        Printf("  %-14s = %d.%03d", SWSE_PtrName(i), whole, frac);
    } else {
        Printf("  %-14s = %d", SWSE_PtrName(i), v);
    }
}
static void Cmd_ptr(int, char**) {
    int n = SWSE_PtrCount();
    if (!n) { SWSE_ConsolePrint("no pointer chains loaded - see SWSEMods\\SWSE Console\\pointers.txt"); return; }
    Printf("%d pointer chain(s):", n);
    for (int i = 0; i < n; i++) PrintChain(i);
}
static void Cmd_ptrreload(int, char**) {
    Printf("reloaded - %d pointer chain(s).", SWSE_PtrLoad());
}
static void Cmd_get(int argc, char** argv) {
    if (argc < 2) { SWSE_ConsolePrint("usage: get <name>   (see 'ptr')"); return; }
    int i = SWSE_PtrFind(argv[1]);
    if (i < 0) { Printf("no such pointer: %s", argv[1]); return; }
    PrintChain(i);
}
// Shared by the `set` command: returns true if name matched a pointer chain.
static bool TrySetPointer(const char* name, const char* val) {
    int i = SWSE_PtrFind(name);
    if (i < 0) return false;
    float f = (float)atof(val); int v = atoi(val);
    if (SWSE_PtrWrite(i, f, v)) { Printf("set %s = %s", name, val); PrintChain(i); }
    else Printf("%s: could not resolve right now", name);
    return true;
}
static void Cmd_hold(int argc, char** argv) {
    if (argc < 3) { SWSE_ConsolePrint("usage: hold <name> <value>   (freeze every frame)"); return; }
    int i = SWSE_PtrFind(argv[1]);
    if (i < 0) { Printf("no such pointer: %s", argv[1]); return; }
    SWSE_PtrSetFrozen(i, true, (float)atof(argv[2]), atoi(argv[2]));
    Printf("holding %s at %s (unhold to release)", argv[1], argv[2]);
}
static void Cmd_unhold(int argc, char** argv) {
    if (argc < 2) { SWSE_ConsolePrint("usage: unhold <name>"); return; }
    int i = SWSE_PtrFind(argv[1]);
    if (i < 0) { Printf("no such pointer: %s", argv[1]); return; }
    SWSE_PtrSetFrozen(i, false, 0, 0);
    Printf("released %s", argv[1]);
}
// anchor watch/probe onto real player memory via a pointer chain
static void Cmd_anchor(int argc, char** argv) {
    if (argc < 2) {
        SWSE_ConsolePrint("usage: anchor <pointerName>   (e.g. anchor health)");
        SWSE_ConsolePrint("       anchor off   - go back to the script context");
        return;
    }
    if (!lstrcmpiA(argv[1], "off")) {
        SWSE_SetWatchAnchor(nullptr);
        SWSE_ConsolePrint("anchor cleared - watch/probe use the script context again.");
        return;
    }
    int i = SWSE_PtrFind(argv[1]);
    if (i < 0) { Printf("no such pointer: %s   (try 'ptr')", argv[1]); return; }
    void* p = SWSE_PtrResolve(i);
    if (!p) { Printf("%s: could not resolve right now", argv[1]); return; }
    SWSE_SetWatchAnchor(p);
    Printf("anchored on %s (%08X) - watch/probe now scan the player object.",
           argv[1], (unsigned)(uintptr_t)p);
    SWSE_ConsolePrint("now: watch 15 buy artifact   then buy one.");
}
static void Cmd_probe(int, char**) {
    SWSE_ScriptDumpContext();
    SWSE_ConsolePrint("probe done (baseline/diff written to log).");
}
static void Cmd_find(int argc, char** argv) {
    if (argc < 2) { SWSE_ConsolePrint("usage: find <value>  (e.g. find 300 - searches the ctx object graph)"); return; }
    SWSE_ScriptFind(atoi(argv[1]));
    SWSE_ConsolePrint("find results written to bin\\swse_log.txt (look for MATCH lines).");
}
// poke/freeze use the exact "[+0x5C]+0x184" addressing from probe/watch logs -
// pass the two hex numbers (with or without 0x) and a decimal value.
static void Cmd_poke(int argc, char** argv) {
    if (argc < 4) { SWSE_ConsolePrint("usage: poke <selfHex> <subHex> <value>  (from a probe/watch line)"); return; }
    int selfOff = strtol(argv[1], nullptr, 16), subOff = strtol(argv[2], nullptr, 16);
    int val = atoi(argv[3]);
    int r = SWSE_Poke(selfOff, subOff, val);
    if (r == 1)      Printf("poked [+0x%X]+0x%X = %d", selfOff, subOff, val);
    else if (r == 0) SWSE_ConsolePrint("poke: no script context, or no field at those offsets - "
                                       "load a save and run 'autoprime', then check the offsets.");
    else             SWSE_ConsolePrint("poke faulted.");
}
static void Cmd_freeze(int argc, char** argv) {
    if (argc < 4) { SWSE_ConsolePrint("usage: freeze <selfHex> <subHex> <value>  (from a probe/watch line)"); return; }
    int selfOff = strtol(argv[1], nullptr, 16), subOff = strtol(argv[2], nullptr, 16);
    int val = atoi(argv[3]);
    int r = SWSE_Freeze(selfOff, subOff, val);
    if (r == 1)       Printf("frozen [+0x%X]+0x%X = %d  (unfreeze to release)", selfOff, subOff, val);
    else if (r == 0)  NoContext("freeze", false);
    else if (r == -1) SWSE_ConsolePrint("freeze list full (16 max) - unfreeze one first.");
}
static void Cmd_unfreeze(int argc, char** argv) {
    if (argc < 3) { SWSE_ConsolePrint("usage: unfreeze <selfHex> <subHex>"); return; }
    int selfOff = strtol(argv[1], nullptr, 16), subOff = strtol(argv[2], nullptr, 16);
    Printf("unfreeze [+0x%X]+0x%X: %s", selfOff, subOff,
          SWSE_Unfreeze(selfOff, subOff) ? "released" : "wasn't frozen");
}
static void Cmd_unfreezeall(int, char**) { SWSE_UnfreezeAll(); SWSE_ConsolePrint("all fields released."); }

static void Cmd_watch(int argc, char** argv) {
    int secs = (argc > 1) ? atoi(argv[1]) : 4;
    if (secs < 1) secs = 1; if (secs > 20) secs = 20;
    // everything after the seconds arg is the label, spaces and all - no
    // quoting needed: `watch 5 teleport test` labels it "teleport test".
    char label[64] = "";
    for (int i = 2; i < argc; i++) {
        if (i > 2) lstrcatA(label, " ");
        char word[48]; lstrcpynA(word, argv[i], 48);
        // strip a leading/trailing quote char if the user did quote it
        int len = lstrlenA(word);
        if (len >= 2 && (word[0] == '\'' || word[0] == '"') && word[len-1] == word[0]) {
            word[len-1] = 0; lstrcatA(label, word + 1);
        } else {
            lstrcatA(label, word);
        }
    }
    int r = SWSE_ScriptWatchStart(secs * 1000, label);
    if (r == 1) {
        Printf("watching%s%s for %ds - close this console (~) and act now.",
              label[0] ? " '" : "", label[0] ? label : "", secs);
        if (label[0]) SWSE_ConsolePrint("(label attached so it's easy to find in the log later.)");
        SWSE_ConsolePrint("results land in bin\\swse_log.txt when the timer's up.");
    } else if (r == 0) {
        NoContext("watch", false);
    } else {
        SWSE_ConsolePrint("watch failed to start.");
    }
}
static void PrintPosResult(int r, const char* ok) {
    const char* why = SWSE_TeleportWhy();
    if (r == 1)       { if (ok && ok[0]) SWSE_ConsolePrint(ok); }
    else if (r == 0)  SWSE_ConsolePrint("no position - no level is up, or the player has no body yet.");
    else if (r == 3)  SWSE_ConsolePrint("nothing saved yet - use 'savepos' first.");
    else if (r == -1) Printf("teleport refused: %s", why && why[0] ? why : "see swse_log.txt");
    else if (r == -3) Printf("move: %s", why && why[0] ? why : "bad axis");
    else              SWSE_ConsolePrint("position access faulted (see swse_log.txt).");
}
static void Cmd_pos(int, char**) {
    float p[3]; int r = SWSE_PosGet(p);
    if (r != 1) { PrintPosResult(r, ""); return; }
    char xs[32], ys[32], zs[32];
    Ff(xs, p[0], 3); Ff(ys, p[1], 3); Ff(zs, p[2], 3);
    float yaw = 0;
    if (SWSE_PlayerYawGet(&yaw) == 1) {
        char ws[32]; Ff(ws, yaw, 1);
        Printf("pos: x=%s  y=%s  z=%s  yaw=%s", xs, ys, zs, ws);
    } else {
        Printf("pos: x=%s  y=%s  z=%s", xs, ys, zs);
    }
}
static void Cmd_writepos(int argc, char** argv);   // named form, above
// savepos           - quick slot: position AND facing (1.1)
// savepos <label>   - the same as writepos <label>: saved to file, with facing
static void Cmd_savepos(int argc, char** argv) {
    if (argc > 1) { Cmd_writepos(argc, argv); return; }
    int r = SWSE_PosSave();
    if (r != 1) { PrintPosResult(r, ""); return; }
    float p[3], yaw = 0; int hy = 0;
    SWSE_PosSaved(p, &yaw, &hy);
    if (hy) {
        char ws[32]; Ff(ws, yaw, 1);
        Printf("position saved: %d %d %d, facing %s deg  (`tp` returns here)",
               (int)p[0], (int)p[1], (int)p[2], ws);
    } else {
        Printf("position saved: %d %d %d (facing unknown)  (`tp` returns here)",
               (int)p[0], (int)p[1], (int)p[2]);
    }
}
// tp          - back to the quick slot, facing restored
// tp <label>  - the same as goto <label>
static void Cmd_tp(int argc, char** argv) {
    if (argc > 1) { Cmd_goto(argc, argv); return; }
    float from[3]; bool haveFrom = (SWSE_PosGet(from) == 1);
    int r = SWSE_PosRestore();
    PrintPosResult(r, "teleported to saved spot.");
    float to[3], yaw = 0; int hy = 0;
    if (r == 1 && haveFrom) { SWSE_PosSaved(to, &yaw, &hy); TpCheckArm(from, to[0], to[1], to[2]); }
}
static void Cmd_up(int argc, char** argv) {
    // Z is up in this engine (hit reactions lift p[2]; the foliage up axis
    // measured z). 1.0.x nudged axis 1, which moved the player sideways.
    // Since the move is a real teleport now, a lift is a real drop: the
    // default is a modest 10 units (it was 100 when nothing moved), and there
    // is no ceiling check - indoors, a lift can end above the roof.
    float d = 10.0f;
    if (argc > 1 && !SaneFloat(argv[1], &d)) { SWSE_ConsolePrint("usage: up [dist]"); return; }
    PrintPosResult(SWSE_PosNudge(2, d), "launched up.");
}
static void Cmd_move(int argc, char** argv) {
    if (argc < 3) { SWSE_ConsolePrint("usage: move <axis 0|1|2> <delta>"); return; }
    float d = 0.0f;
    if (!SaneFloat(argv[2], &d)) { SWSE_ConsolePrint("usage: move <axis 0|1|2> <delta>"); return; }
    PrintPosResult(SWSE_PosNudge(atoi(argv[1]), d), "moved.");
}
// `list [filter]` - search the 181 auto-exposed game functions.
static void Cmd_list(int argc, char** argv) {
    const char* names[256];
    int n = SWSE_ScriptList(argc > 1 ? argv[1] : "", names, 256);
    Printf("%d game functions%s (call <name> [args]):", n,
           argc > 1 ? " matching" : "");
    if (SWSE_GameBuildSafeMode())
        SWSE_ConsolePrint("(safe mode: none of them can be called on this game build - 'status')");
    char row[160]; int col = 0; row[0] = 0;
    for (int i = 0; i < n; i++) {
        char cell[40]; wsprintfA(cell, "%-22s", names[i]);
        lstrcatA(row, cell);
        if (++col == 3) { SWSE_ConsolePrint(row); row[0] = 0; col = 0; }
    }
    if (col) SWSE_ConsolePrint(row);
}
// `call <name> [args...]` - invoke any game function by its real name.
static void Cmd_call(int argc, char** argv) {
    if (argc < 2) { SWSE_ConsolePrint("usage: call <function> [args]  (see 'list')"); return; }
    int r = SWSE_ScriptCallByName(argv[1], argc - 1, argv + 1);
    if (r == 1)       Printf("%s: called", argv[1]);
    else if (r == 0)  NoContext(argv[1], true);
    else if (r == -1) Printf("no such function: %s  (try 'list %s')", argv[1], argv[1]);
    else              Printf("%s: faulted - args wrong? try different values", argv[1]);
}

// ==========================================================================
//  1.1 ADDITIONS
// ==========================================================================
static void Execute(const char* line);          // fwd - defined with the registry
static void RunLines(char lines[][240], int n); // fwd - sequences with `wait`
// How deep scripts, aliases, `exec` and `repeat` are nested right now; each
// refuses past 8, so a file or alias that invokes itself stops instead of
// running the stack out.
static int  g_execDepth = 0;
static bool IsBuiltinCmd(const char* name);     // fwd - needs the command table
static void LoadDynCmds();                      // fwd - scripts\*.txt commands

// ---- player tuning (playerprefs.txt) ---------------------------------------
static void Cmd_playertune(int argc, char** argv) {
    char msg[300];
    if (argc > 1 && !lstrcmpiA(argv[1], "reload")) {
        SWSE_PlayerTuneLoad(msg, sizeof(msg)); SWSE_ConsolePrint(msg);
        if (SWSE_LevelUp()) { SWSE_PlayerTuneApply(msg, sizeof(msg)); SWSE_ConsolePrint(msg); }
        return;
    }
    if (argc > 1 && !lstrcmpiA(argv[1], "apply"))   { SWSE_PlayerTuneApply(msg, sizeof(msg));   SWSE_ConsolePrint(msg); return; }
    if (argc > 1 && !lstrcmpiA(argv[1], "restore")) { SWSE_PlayerTuneRestore(msg, sizeof(msg)); SWSE_ConsolePrint(msg); return; }
    Printf("playertune is %s%s", SWSE_Feature(FEAT_PLAYERTUNE) ? "ON" : "off",
           SWSE_Feature(FEAT_PLAYERTUNE) ? " - applied once per level" : " - `features playertune on` to apply every level");
    SWSE_PlayerTuneStatus(AiEmit);
    SWSE_ConsolePrint("usage: playertune [reload|apply|restore]");
}

// ---- the live prefs editor ---------------------------------------------------
static void Cmd_prefs(int argc, char** argv) { SWSE_PrefsCommand(argc, argv, AiEmit); }

// ---- ammo knockback -------------------------------------------------------------
// Every player ammo type is a prefs record at /data/prefs/weapons/<name>.txt
// (BoltDamagePrefs and relatives, all deriving from BoltPrefs) with
// m_maxKnockSpeed (+0x140, how hard it throws what it hits) and
// m_maxKnockSpeedPlayer (+0x144, what to use when it hits YOU). The engine
// (0x4A1490) uses the player value when it is >= 0 and m_maxKnockSpeed
// otherwise, and only knocks when the result is > 0 - so -1 (every shipped
// type) means "the same as for NPCs" and 0 means "never knocks you". The same
// two fields Stranger: Armed to the Teeth patches in the bundles - here, live.
static const struct { const char* name; const char* nick; } kAmmo[] = {
    { "damagearmadillo", "thudslug" },          { "damagearmadilloloader", "thudslug1" },
    { "damagearmadilloloaderextender", "thudslug2" },
    { "damageriotslug", "riotslug" },           { "damagebeegun", "stingbee" },
    { "damagebeegunextendersmall", "stingbee1" }, { "damagestingbee", "superstingbee" },
    { "trapfuzzle", "fuzzle" },                 { "trapfuzzleloader", "fuzzle1" },
    { "trapfuzzleloaderextender", "fuzzle2" },  { "trapfuzzlerabid", "rabidfuzzle" },
    { "trapfuzzlejump", "fuzzlepounce" },       { "trapfuzzlerabidjump", "rabidpounce" },
    { "immobilizespiderbola", "bolamite" },     { "immobilizespiderbolaextendersmall", "bolamite1" },
    { "immobilizespiderbolaextendermedium", "bolamite2" },
    { "immobilizebolablast", "bolablast" },     { "sniperdart", "sniperwasp" },
    { "immobilizeskunkbomb", "stunkz" },        { "immobilizeskunkbombextender", "stunkz1" },
    { "immobilizesparkstunkz", "sparkstunkz" }, { "damagedynamite", "boombat" },
    { "damagedynamiteextender", "boombat1" },   { "damagedynamiteextenderloader", "boombat2" },
    { "damageboombatqueen", "boombatseekers" }, { "sendtochipmunk", "chippunk" },
    { "sendtochipmunkextender", "chippunk1" },  { "sendtohowlerpunk", "howlerpunk" },
    { "activatehivequeen", "hivequeen" },       { "activatehivequeencharged", "hivequeen2" },
    { "punch", "punch" },
};
static const int N_AMMO = (int)(sizeof(kAmmo) / sizeof(kAmmo[0]));

static void Cmd_knockback(int argc, char** argv) {
    static char paths[64][96];
    static const char* ptr[64];
    if (argc < 3) {
        for (int i = 0; i < N_AMMO; i++) {
            wsprintfA(paths[i], "/data/prefs/weapons/%s.txt", kAmmo[i].name);
            ptr[i] = paths[i];
        }
        static float kn[64], kp[64]; static int f1[64], f2[64];
        int a = SWSE_PrefsGetMany(ptr, N_AMMO, "m_maxKnockSpeed", kn, f1);
        SWSE_PrefsGetMany(ptr, N_AMMO, "m_maxKnockSpeedPlayer", kp, f2);
        if (!a) SWSE_ConsolePrint("no ammo prefs loaded yet (load a save first)");
        SWSE_ConsolePrint("  ammo              npc knock   player knock   (-1 = same as npc, 0 = never)");
        for (int i = 0; i < N_AMMO; i++) {
            if (!f1[i]) continue;
            char s1[32], s2[32]; Ff(s1, kn[i], 2); Ff(s2, kp[i], 2);
            Printf("  %-17s %9s %12s   %s", kAmmo[i].nick, s1, s2, kAmmo[i].name);
        }
        SWSE_ConsolePrint("usage: knockback <ammo|all> <npcSpeed|-> [playerSpeed|-] [keep]");
        SWSE_ConsolePrint("  e.g. knockback boombat 40 25   - boombats throw NPCs harder, and you at 25");
        SWSE_ConsolePrint("       knockback all - 0         - nothing you fire can knock YOU back");
        return;
    }
    bool keep = !lstrcmpiA(argv[argc - 1], "keep");
    int nargs = keep ? argc - 1 : argc;          // the arguments before `keep`
    if (nargs < 3) {
        SWSE_ConsolePrint("usage: knockback <ammo|all> <npcSpeed|-> [playerSpeed|-] [keep]");
        return;
    }
    int n = 0;
    for (int i = 0; i < N_AMMO; i++) {
        if (lstrcmpiA(argv[1], "all") && lstrcmpiA(argv[1], kAmmo[i].name) &&
            lstrcmpiA(argv[1], kAmmo[i].nick)) continue;
        wsprintfA(paths[n], "/data/prefs/weapons/%s.txt", kAmmo[i].name);
        ptr[n] = paths[n];
        n++;
    }
    if (!n) { Printf("no ammo called '%s' - `knockback` lists them", argv[1]); return; }
    const char* npc = argv[2];
    const char* ply = (nargs > 3) ? argv[3] : "-";
    // Numbers or "-" only, checked before anything is set or kept:
    // `knockback thudslug keep` used to save the word `keep` into prefs.txt.
    const char* vals[2] = { npc, ply };
    for (int k = 0; k < 2; k++) {
        if (!lstrcmpA(vals[k], "-")) continue;
        char* e = nullptr;
        strtod(vals[k], &e);
        if (e == vals[k] || *e) {
            Printf("'%s' is not a speed - give a number, or - to leave it as it is", vals[k]);
            return;
        }
    }
    if (!lstrcmpA(npc, "-") && !lstrcmpA(ply, "-")) {
        SWSE_ConsolePrint("nothing to change - both speeds are '-'");
        return;
    }
    char msg[300] = "";
    if (lstrcmpA(npc, "-")) {
        int r = SWSE_PrefsSetMany(ptr, n, "m_maxKnockSpeed", npc, AiEmit);
        if (keep && r >= 0)
            for (int i = 0; i < n; i++) SWSE_PrefsKeep(ptr[i], "m_maxKnockSpeed", npc, msg, sizeof(msg));
    }
    if (lstrcmpA(ply, "-")) {
        int r = SWSE_PrefsSetMany(ptr, n, "m_maxKnockSpeedPlayer", ply, AiEmit);
        if (keep && r >= 0)
            for (int i = 0; i < n; i++) SWSE_PrefsKeep(ptr[i], "m_maxKnockSpeedPlayer", ply, msg, sizeof(msg));
    }
    if (keep) { if (msg[0]) SWSE_ConsolePrint(msg); }
    else SWSE_ConsolePrint("this level only - add `keep` to re-apply every level (prefsedit feature)");
}

// ---- npc / wpn: every reflected character and weapon field, by name ---------
// Thin fronts on the live prefs editor, so every change is recorded and
// `prefs restore` puts the shipped value back, and `keep` writes it into
// prefs.txt to re-apply every level. Field names come from the game's own
// reflection (swse/research/REFLECT_FIELDS.tsv); the aliases are shorthand.
// Mapped from the proposals in swse/research/AT3_DISCOVERIES.md (P1-P10).
// NOTE: fields an NPC copies when it is CREATED (motion, attachments, some
// AI) only reach NPCs spawned afterwards - `keep` + a level load settles it.

static void PrefsGet(const char* target, const char* field) {
    char a0[] = "prefs", a1[] = "get";
    char t[160], f[120];
    lstrcpynA(t, target, sizeof(t)); lstrcpynA(f, field, sizeof(f));
    char* av[4] = { a0, a1, t, f };
    SWSE_PrefsCommand(4, av, AiEmit);
}

// Is a prefs target (path or 8-digit hash) loaded in this level? Only a
// definite "no" counts: when the registry cannot be read, the caller goes on
// and lets the editor's heap scan decide.
static bool TargetNotLoaded(const char* target) {
    unsigned h = (strchr(target, '/') || strchr(target, '\\'))
                 ? SWSE_HashPath(target) : (unsigned)strtoul(target, nullptr, 16);
    if (!h) return false;
    return SWSE_ResourceLookup(h) == 0 && SWSE_ResourceRegistryOk() == 1;
}

// on/off/none -> what the editor parses. none = the engine's unset token.
static const char* NormValue(const char* v) {
    if (!lstrcmpiA(v, "on") || !lstrcmpiA(v, "yes") || !lstrcmpiA(v, "true"))   return "1";
    if (!lstrcmpiA(v, "off") || !lstrcmpiA(v, "no") || !lstrcmpiA(v, "false"))  return "0";
    if (!lstrcmpiA(v, "none")) return "2DFD1072";
    return v;
}

static const struct { const char* alias; const char* field; } kNpcAlias[] = {
    { "health", "m_health" },              { "stamina", "m_stamina" },
    { "healthregen", "m_healthRecoverTime" }, { "staminaregen", "m_staminaRecoverTime" },
    { "exhaust", "m_exhaustTime" },        { "species", "m_spActorPrefs.m_species" },
    { "gibfx", "m_spActorPrefs.m_gibEffectName" }, { "gibsound", "m_spActorPrefs.m_gibSoundName" },
    { "gib", "m_onDeathGib" },             { "gibspawn", "m_onGibSpawnNPC" },
    { "hurt", "m_hurtReaction" },          { "moolah", "m_killMoolah" },
    { "capturemoolah", "m_captureMoolah" }, { "bountyable", "m_canBeBountied" },
    { "melee", "m_meleeWeapon" },          { "ranged", "m_rangedWeapon" },
    { "ammorule", "m_affGenerally" },      { "lootlimit", "m_collectableSpawnLimit" },
    { "loot.damage", "m_onDamageCollectableSpawner" },
    { "loot.exhaust", "m_onExhaustCollectableSpawner" },
    { "loot.death", "m_onDeathCollectableSpawner" },
    { "loot.steefram", "m_onSteefRamAliveCollectableSpawner" },
    { "loot.ram", "m_onStrangerRamAliveCollectableSpawner" },
    { "loot.steeframdead", "m_onSteefRamDeadCollectableSpawner" },
    { "loot.ramdead", "m_onStrangerRamDeadCollectableSpawner" },
    { "mass", "m_spMotionPrefs.m_mass" },  { "turn", "m_spMotionPrefs.m_bipedTurnSpeedDegrees" },
    { "walk", "m_spMotionPrefs.m_overVel_Walk" }, { "trot", "m_spMotionPrefs.m_overVel_Trot" },
    { "canter", "m_spMotionPrefs.m_overVel_Canter" }, { "run", "m_spMotionPrefs.m_overVel_Run" },
    { "jump", "m_spMotionPrefs.m_jumpHeightMax" }, { "pushable", "m_spMotionPrefs.m_canBePushed" },
    { "knockable", "m_spMotionPrefs.m_canBeKnocked" }, { "fade", "m_fadeOutOnDeath" },
    { "aimradius", "m_autoAimRadius" },    { "relax", "m_spAIPrefs.m_relaxAgitatedToNormal" },
    { "allowpanic", "m_spAIPrefs.m_allowPanic" },
};
static const char* kNpcSummary[] = {
    "health", "stamina", "species", "gib", "gibfx", "gibspawn", "hurt", "moolah",
    "loot.death", "mass", "run", "jump", "sight.normal.see", "sight.combat.see", "relax",
};

// sight.<normal|agit|combat|panic>.<6th|see|above|below|hangle|vangle|instant|hidevol>
// and attack.<param> expand to the dotted AIPrefs members.
static bool NpcFieldName(const char* in, char* out, int outLen) {
    for (int i = 0; i < (int)(sizeof(kNpcAlias) / sizeof(kNpcAlias[0])); i++)
        if (!lstrcmpiA(in, kNpcAlias[i].alias)) { lstrcpynA(out, kNpcAlias[i].field, outLen); return true; }
    if (!_strnicmp(in, "sight.", 6)) {
        char st[16] = { 0 }; const char* dot = strchr(in + 6, '.');
        if (!dot || dot - (in + 6) >= (int)sizeof(st)) return false;
        memcpy(st, in + 6, dot - (in + 6));
        static const char* S[][2] = { { "normal", "Normal" }, { "agit", "Agit" }, { "combat", "Combat" }, { "panic", "Panic" } };
        static const char* F[][2] = { { "6th", "6thSenseDistance" }, { "see", "seeDistance" },
            { "above", "seeAbove" }, { "below", "seeBelow" }, { "hangle", "horizontalAngleDeg" },
            { "vangle", "verticalAngleDeg" }, { "instant", "instantSightDistance" },
            { "hidevol", "hideVolSeeDistance" } };
        const char* s = nullptr; const char* f = nullptr;
        for (int i = 0; i < 4; i++) if (!lstrcmpiA(st, S[i][0])) s = S[i][1];
        for (int i = 0; i < 8; i++) if (!lstrcmpiA(dot + 1, F[i][0])) f = F[i][1];
        if (!s || !f) return false;
        _snprintf_s(out, outLen, _TRUNCATE, "m_spAIPrefs.m_sight%s.m_%s", s, f);
        return true;
    }
    if (!_strnicmp(in, "attack.", 7)) {
        _snprintf_s(out, outLen, _TRUNCATE, "m_spAIPrefs.m_attackParams.%s", in + 7);
        return true;
    }
    lstrcpynA(out, in, outLen);                  // a raw reflected name or offset
    return true;
}

static const char* NpcName(unsigned h);          // fwd - the known-names table

// A character name ("outlawcutter", "heavy") or an 8-digit type hash.
static bool NpcTypeArg(const char* s, char* hashOut) {
    int n = 0;
    for (const char* p = s; *p; p++) {
        char c = *p;
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) { n = -1; break; }
        n++;
    }
    if (n == 8) { lstrcpynA(hashOut, s, 9); return true; }
    char want[48] = { 0 }; int w = 0;
    for (const char* p = s; *p && w < 47; p++) if (*p != ' ' && *p != '\'' && *p != '_') want[w++] = (char)(*p | 0x20);
    for (int i = 0; i < (int)(sizeof(kNpcNames) / sizeof(kNpcNames[0])); i++) {
        char nm[48] = { 0 }; int k = 0;
        for (const char* p = kNpcNames[i].name; *p && k < 47; p++)
            if (*p != ' ' && *p != '\'' && *p != '(' && *p != ')') nm[k++] = (char)(*p | 0x20);
        if (!lstrcmpA(nm, want)) { wsprintfA(hashOut, "%08X", kNpcNames[i].hash); return true; }
    }
    return false;
}

static void Cmd_npc(int argc, char** argv) {
    if (argc < 2) {
        SWSE_ConsolePrint("usage: npc <type> [field [value]] [keep]   (type = 8-digit hash or a name)");
        SWSE_ConsolePrint("  npc <type>                 the main fields of that character");
        SWSE_ConsolePrint("  npc <type> dump [filter]   every reflected field with its value");
        SWSE_ConsolePrint("  fields: health stamina species gib gibfx gibspawn hurt moolah melee ranged");
        SWSE_ConsolePrint("          loot.<damage|exhaust|death|steefram|ram|...> lootlimit ammorule mass");
        SWSE_ConsolePrint("          turn walk trot canter run jump pushable knockable fade relax allowpanic");
        SWSE_ConsolePrint("          sight.<normal|agit|combat|panic>.<6th|see|above|below|hangle|vangle|instant|hidevol>");
        SWSE_ConsolePrint("          attack.<param> (see `npc <type> dump attack`) or any m_ name / 0x offset");
        SWSE_ConsolePrint("  values: numbers; hashes as 8 hex digits or a /data/... path; on/off; none");
        SWSE_ConsolePrint("  `npcguns` / `types` list the characters here; `prefs restore` undoes");
        return;
    }
    char hash[16];
    if (!NpcTypeArg(argv[1], hash)) { Printf("no character called '%s' - give its 8-digit hash (`npcguns`)", argv[1]); return; }
    if (argc == 2) {
        unsigned h = (unsigned)strtoul(hash, nullptr, 16);
        if (TargetNotLoaded(hash)) {
            Printf("%s (%s) is not loaded in this level - `npcguns` lists the characters here", hash, NpcName(h));
            return;
        }
        Printf("%s  %s", hash, NpcName(h));
        for (int i = 0; i < (int)(sizeof(kNpcSummary) / sizeof(kNpcSummary[0])); i++) {
            char f[120]; NpcFieldName(kNpcSummary[i], f, sizeof(f));
            PrefsGet(hash, f);
        }
        return;
    }
    if (!lstrcmpiA(argv[2], "dump")) {
        char a0[] = "prefs", a1[] = "dump";
        char* av[4] = { a0, a1, hash, (argc > 3) ? argv[3] : nullptr };
        SWSE_PrefsCommand(argc > 3 ? 4 : 3, av, AiEmit);
        return;
    }
    char field[120];
    if (!NpcFieldName(argv[2], field, sizeof(field))) { Printf("unknown field '%s' - `npc` lists them", argv[2]); return; }
    if (argc == 3) { PrefsGet(hash, field); return; }
    bool keep = (argc > 4 && !lstrcmpiA(argv[4], "keep"));
    const char* v = NormValue(argv[3]);
    int r = SWSE_PrefsSet(hash, field, v, AiEmit);
    if (keep && r < 0) SWSE_ConsolePrint("not kept - fix the field or value first");
    else if (keep) { char m[240]; SWSE_PrefsKeep(hash, field, v, m, sizeof(m)); SWSE_ConsolePrint(m); }
}

// Weapons: player ammo (/data/prefs/weapons/<ammo>.txt) and NPC weapons
// (/data/prefs/weapons/npc/<name>.txt) share BoltPrefs, so one set of names
// serves both. `firerate`/`reload` pick the right member for each.
static const struct { const char* alias; const char* npcField; const char* ammoField; } kWpnAlias[] = {
    { "damage", "m_damage", "m_damage" },           { "dmgobj", "m_damageDestructable", "m_damageDestructable" },
    { "stamina", "m_stamina", "m_stamina" },        { "clip", "m_clipCapacity", "m_clipCapacity" },
    { "clipmin", "m_clipCapacityMin", "m_clipCapacityMin" }, { "total", "m_totalCapacity", "m_totalCapacity" },
    { "aoe", "m_areaOfEffect", "m_areaOfEffect" },  { "aoeedge", "m_damageAtAreaEdgeMultiplier", "m_damageAtAreaEdgeMultiplier" },
    { "blast", "m_explosionSpeed", "m_explosionSpeed" }, { "range", "m_range", "m_range" },
    { "speed", "m_speed", "m_speed" },              { "gravity", "m_gravity", "m_gravity" },
    { "homing", "m_magnetismOverride", "m_magnetismOverride" }, { "bounce", "m_bounceParameter", "m_bounceParameter" },
    { "knock", "m_maxKnockSpeed", "m_maxKnockSpeed" }, { "knockplayer", "m_maxKnockSpeedPlayer", "m_maxKnockSpeedPlayer" },
    { "gibkill", "m_gibOnKill", "m_gibOnKill" },    { "duration", "m_duration", "m_duration" },
    { "immobilize", "m_immobilizeDuration", "m_immobilizeDuration" },
    { "firerate", "m_fireRate", "m_fireRateNormal" }, { "reload", "m_reloadTime", "m_reloadTimeNormal" },
    { "reloadmax", "m_reloadTimeMax", "m_reloadTimeNormal" }, { "accuracy", "m_accuracyWidth", "m_accuracyWidth" },
    { "misstime", "m_missTime", "m_missTime" },     { "kick", "m_kick", "m_kick" },
};
static const char* kWpnSummary[] = {
    "damage", "dmgobj", "clip", "total", "aoe", "range", "speed", "gravity", "homing", "bounce",
    "knock", "knockplayer", "firerate", "reload",
};

// Resolve a weapon argument to a prefs target: ammo name/nickname, "npc/<name>"
// or a bare NPC weapon name, an 8-digit hash (a weapon, or a character whose
// ranged weapon is meant). *isAmmo says which member names apply.
static bool WpnTarget(const char* s, char* out, int outLen, bool* isAmmo) {
    *isAmmo = false;
    for (int i = 0; i < N_AMMO; i++)
        if (!lstrcmpiA(s, kAmmo[i].name) || !lstrcmpiA(s, kAmmo[i].nick)) {
            _snprintf_s(out, outLen, _TRUNCATE, "/data/prefs/weapons/%s.txt", kAmmo[i].name);
            *isAmmo = true;
            return true;
        }
    int n = 0; bool hex = true;
    for (const char* p = s; *p; p++, n++) {
        char c = *p;
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) hex = false;
    }
    if (hex && n == 8) {
        unsigned h = (unsigned)strtoul(s, nullptr, 16);
        unsigned obj = SWSE_ResourceLookup(h);
        if (obj && SWSE_ObjIsA(obj, "NPCPrefs")) {
            unsigned w = 0;
            __try { w = *(unsigned*)(obj + 0x498); } __except (EXCEPTION_EXECUTE_HANDLER) { w = 0; }
            if (!w || w == 0x2DFD1072) return false;           // melee-only character
            _snprintf_s(out, outLen, _TRUNCATE, "%08X", w);
            return true;
        }
        if (obj) *isAmmo = !SWSE_ObjIsA(obj, "NPCWeaponPrefs");
        lstrcpynA(out, s, outLen);
        return true;
    }
    const char* nm = (!_strnicmp(s, "npc/", 4)) ? s + 4 : s;
    _snprintf_s(out, outLen, _TRUNCATE, "/data/prefs/weapons/npc/%s.txt", nm);
    return true;
}

static void Cmd_wpn(int argc, char** argv) {
    if (argc < 2) {
        SWSE_ConsolePrint("usage: wpn <ammo|npcweapon|hash|all> [field [value]] [keep]");
        SWSE_ConsolePrint("  ammo: thudslug boombat stingbee bolamite... (see `knockback`)");
        SWSE_ConsolePrint("  npc weapons: outlawshooter wolvarkshooter slogbolt... or a character's hash");
        SWSE_ConsolePrint("  fields: damage dmgobj stamina clip clipmin total aoe aoeedge blast range speed");
        SWSE_ConsolePrint("          gravity homing bounce knock knockplayer gibkill duration immobilize");
        SWSE_ConsolePrint("          firerate reload reloadmax accuracy misstime kick, or any m_ name");
        SWSE_ConsolePrint("  e.g. wpn thudslug homing 360   wpn FFFC00CB damage 5   wpn all clip 20 keep");
        return;
    }
    const char* aliasIn = (argc > 2) ? argv[2] : nullptr;
    int ai = -1;
    if (aliasIn)
        for (int i = 0; i < (int)(sizeof(kWpnAlias) / sizeof(kWpnAlias[0])); i++)
            if (!lstrcmpiA(aliasIn, kWpnAlias[i].alias)) { ai = i; break; }
    bool keep = (argc > 4 && !lstrcmpiA(argv[4], "keep"));

    if (!lstrcmpiA(argv[1], "all")) {
        if (argc < 4) { SWSE_ConsolePrint("usage: wpn all <field> <value> [keep]  (every player ammo type)"); return; }
        static char paths[64][96]; static const char* ptr[64];
        for (int i = 0; i < N_AMMO; i++) {
            wsprintfA(paths[i], "/data/prefs/weapons/%s.txt", kAmmo[i].name); ptr[i] = paths[i];
        }
        const char* field = (ai >= 0) ? kWpnAlias[ai].ammoField : argv[2];
        const char* v = NormValue(argv[3]);
        int r = SWSE_PrefsSetMany(ptr, N_AMMO, field, v, AiEmit);
        if (keep && r < 0) SWSE_ConsolePrint("not kept - fix the field or value first");
        else if (keep) {
            char m[240] = "";
            for (int i = 0; i < N_AMMO; i++) SWSE_PrefsKeep(ptr[i], field, v, m, sizeof(m));
            SWSE_ConsolePrint(m);
        }
        return;
    }
    char target[160]; bool isAmmo = false;
    if (!WpnTarget(argv[1], target, sizeof(target), &isAmmo)) {
        Printf("%s has no ranged weapon (melee-only character)", argv[1]);
        return;
    }
    if (argc == 2) {
        // One answer for an unknown or unloaded weapon, not one per field.
        if (TargetNotLoaded(target)) {
            Printf("no weapon '%s' is loaded in this level - `knockback` lists player ammo, "
                   "`npcguns` the armed characters", argv[1]);
            return;
        }
        for (int i = 0; i < (int)(sizeof(kWpnSummary) / sizeof(kWpnSummary[0])); i++)
            for (int k = 0; k < (int)(sizeof(kWpnAlias) / sizeof(kWpnAlias[0])); k++)
                if (!lstrcmpiA(kWpnSummary[i], kWpnAlias[k].alias)) {
                    PrefsGet(target, isAmmo ? kWpnAlias[k].ammoField : kWpnAlias[k].npcField);
                    break;
                }
        return;
    }
    const char* field = (ai >= 0) ? (isAmmo ? kWpnAlias[ai].ammoField : kWpnAlias[ai].npcField) : argv[2];
    if (argc == 3) { PrefsGet(target, field); return; }
    const char* v = NormValue(argv[3]);
    int r = SWSE_PrefsSet(target, field, v, AiEmit);
    if (keep && r < 0) SWSE_ConsolePrint("not kept - fix the field or value first");
    else if (keep) { char m[240]; SWSE_PrefsKeep(target, field, v, m, sizeof(m)); SWSE_ConsolePrint(m); }
}

// ---- key binds ----------------------------------------------------------------
// bind F5 savepos  - a key (optionally ctrl+/alt+/shift+) runs a console line.
// Saved to SWSE Console\binds.txt. Binds fire only while the console is closed
// and the game really has focus, so typing elsewhere never triggers them.
struct KeyBind { int vk; int mods; char cmd[200]; };
static KeyBind g_binds[64];
static int     g_bindN = 0;
enum { MOD_C = 1, MOD_A = 2, MOD_S = 4 };

static const struct { const char* name; int vk; } kVkNames[] = {
    { "space", VK_SPACE }, { "tab", VK_TAB }, { "enter", VK_RETURN }, { "backspace", VK_BACK },
    { "insert", VK_INSERT }, { "ins", VK_INSERT }, { "delete", VK_DELETE }, { "del", VK_DELETE },
    { "home", VK_HOME }, { "end", VK_END }, { "pgup", VK_PRIOR }, { "pageup", VK_PRIOR },
    { "pgdn", VK_NEXT }, { "pagedown", VK_NEXT }, { "up", VK_UP }, { "down", VK_DOWN },
    { "left", VK_LEFT }, { "right", VK_RIGHT }, { "pause", VK_PAUSE }, { "scrolllock", VK_SCROLL },
    { "minus", VK_OEM_MINUS }, { "equals", VK_OEM_PLUS }, { "lbracket", VK_OEM_4 },
    { "rbracket", VK_OEM_6 }, { "semicolon", VK_OEM_1 }, { "quote", VK_OEM_7 },
    { "comma", VK_OEM_COMMA }, { "period", VK_OEM_PERIOD }, { "slash", VK_OEM_2 },
    { "backslash", VK_OEM_5 }, { "kpmultiply", VK_MULTIPLY }, { "kpadd", VK_ADD },
    { "kpsubtract", VK_SUBTRACT }, { "kpdivide", VK_DIVIDE }, { "kpdecimal", VK_DECIMAL },
    { "mouse3", VK_MBUTTON }, { "mouse4", VK_XBUTTON1 }, { "mouse5", VK_XBUTTON2 },
};

static int VkFromName(const char* s) {
    if (!s || !*s) return 0;
    if ((s[0] == 'f' || s[0] == 'F') && s[1] >= '1' && s[1] <= '9') {
        // Digits only: "f1x" used to be read as F1.
        bool digits = true;
        for (const char* d = s + 1; *d; d++) if (*d < '0' || *d > '9') { digits = false; break; }
        int n = digits ? atoi(s + 1) : 0;
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    if (!s[1]) {
        char c = s[0];
        if (c >= 'a' && c <= 'z') return c - 'a' + 'A';
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
    }
    if (!_strnicmp(s, "numpad", 6) && s[6] >= '0' && s[6] <= '9' && !s[7]) return VK_NUMPAD0 + (s[6] - '0');
    if (!_strnicmp(s, "kp", 2) && s[2] >= '0' && s[2] <= '9' && !s[3]) return VK_NUMPAD0 + (s[2] - '0');
    for (int i = 0; i < (int)(sizeof(kVkNames) / sizeof(kVkNames[0])); i++)
        if (!lstrcmpiA(s, kVkNames[i].name)) return kVkNames[i].vk;
    return 0;
}

static void VkToName(int vk, int mods, char* out) {
    char key[24] = "?";
    if (vk >= VK_F1 && vk <= VK_F24) wsprintfA(key, "F%d", vk - VK_F1 + 1);
    else if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) wsprintfA(key, "%c", vk);
    else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) wsprintfA(key, "numpad%d", vk - VK_NUMPAD0);
    else for (int i = 0; i < (int)(sizeof(kVkNames) / sizeof(kVkNames[0])); i++)
        if (kVkNames[i].vk == vk) { lstrcpynA(key, kVkNames[i].name, sizeof(key)); break; }
    wsprintfA(out, "%s%s%s%s", (mods & MOD_C) ? "ctrl+" : "", (mods & MOD_A) ? "alt+" : "",
              (mods & MOD_S) ? "shift+" : "", key);
}

// "ctrl+alt+F5" -> vk + modifier mask
static bool ParseKey(const char* spec, int* vk, int* mods) {
    char s[48]; lstrcpynA(s, spec, sizeof(s));
    *mods = 0;
    char* p = s;
    for (;;) {
        char* plus = strchr(p, '+');
        if (!plus || !plus[1]) break;
        *plus = 0;
        if (!lstrcmpiA(p, "ctrl"))       *mods |= MOD_C;
        else if (!lstrcmpiA(p, "alt"))   *mods |= MOD_A;
        else if (!lstrcmpiA(p, "shift")) *mods |= MOD_S;
        else return false;
        p = plus + 1;
    }
    *vk = VkFromName(p);
    // ~ opens the console and Esc closes it; neither can be a bind.
    return *vk != 0 && *vk != VK_OEM_3 && *vk != VK_ESCAPE;
}

static void BindsPath(char* out) { char d[MAX_PATH]; GetModDir(d); wsprintfA(out, "%s\\binds.txt", d); }

// binds.txt and aliases.txt are rewritten from memory - atomically (written
// beside, then swapped in) and keeping the author's comments: every '#' line
// already in the file is carried over, in order, ahead of the entries. 1.0.x
// opened the file "w" and wrote from memory, so hand comments were lost and a
// crash mid-write left the file empty.
static FILE* BeginListRewrite(const char* path, char* tmp, int tmpLen,
                              const char* hdr1, const char* hdr2) {
    _snprintf_s(tmp, tmpLen, _TRUNCATE, "%s.tmp", path);
    FILE* out = fopen(tmp, "w");
    if (!out) return nullptr;
    bool any = false;
    FILE* in = fopen(path, "r");
    if (in) {
        char line[400];
        while (fgets(line, sizeof(line), in)) {
            const char* p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (*p != '#') continue;
            fputs(line, out);
            int n = lstrlenA(line);
            if (n && line[n - 1] != '\n') fputc('\n', out);
            any = true;
        }
        fclose(in);
    }
    if (!any) { fputs(hdr1, out); fputs(hdr2, out); }
    return out;
}

static void EndListRewrite(FILE* out, const char* tmp, const char* path) {
    bool ok = !ferror(out);
    if (fclose(out) != 0) ok = false;
    if (!ok || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        DeleteFileA(tmp);
}

static void SaveBinds() {
    char path[MAX_PATH], tmp[MAX_PATH + 8]; BindsPath(path);
    FILE* f = BeginListRewrite(path, tmp, sizeof(tmp),
        "# SWSE key binds - `bind <key> <command>` writes this file.\n",
        "# <key> <console command line>   (ctrl+/alt+/shift+ prefixes allowed)\n");
    if (!f) return;
    for (int i = 0; i < g_bindN; i++) {
        char k[48]; VkToName(g_binds[i].vk, g_binds[i].mods, k);
        fprintf(f, "%-12s %s\n", k, g_binds[i].cmd);
    }
    EndListRewrite(f, tmp, path);
}

static void SetBind(int vk, int mods, const char* cmd) {
    for (int i = 0; i < g_bindN; i++) {
        if (g_binds[i].vk != vk || g_binds[i].mods != mods) continue;
        if (!cmd) { g_binds[i] = g_binds[--g_bindN]; return; }
        lstrcpynA(g_binds[i].cmd, cmd, sizeof(g_binds[i].cmd));
        return;
    }
    if (!cmd || g_bindN >= 64) return;
    g_binds[g_bindN].vk = vk; g_binds[g_bindN].mods = mods;
    lstrcpynA(g_binds[g_bindN].cmd, cmd, sizeof(g_binds[g_bindN].cmd));
    g_bindN++;
}

static void LoadBinds() {
    g_bindN = 0;
    char path[MAX_PATH]; BindsPath(path);
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[260];
    while (fgets(line, sizeof(line), f)) {
        char* s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#' || *s == '\r' || *s == '\n') continue;
        char* e = s; while (*e && *e != ' ' && *e != '\t') e++;
        if (!*e) continue;
        *e++ = 0;
        while (*e == ' ' || *e == '\t') e++;
        char* nl = e + lstrlenA(e);
        while (nl > e && (nl[-1] == '\r' || nl[-1] == '\n' || nl[-1] == ' ')) *--nl = 0;
        int vk, mods;
        if (*e && ParseKey(s, &vk, &mods)) SetBind(vk, mods, e);
    }
    fclose(f);
}

// Joins argv[from..] back into one line.
static void JoinArgs(int argc, char** argv, int from, char* out, int outLen) {
    out[0] = 0;
    for (int i = from; i < argc; i++) {
        if (i > from && lstrlenA(out) + 1 < outLen) lstrcatA(out, " ");
        if (lstrlenA(out) + lstrlenA(argv[i]) < outLen) lstrcatA(out, argv[i]);
    }
}

static void Cmd_bind(int argc, char** argv) {
    if (argc < 3) {
        SWSE_ConsolePrint("usage: bind <key> <command...>   e.g. bind F5 savepos");
        SWSE_ConsolePrint("  keys: F1-F24 A-Z 0-9 numpad0-9 insert delete home end pgup pgdn");
        SWSE_ConsolePrint("        mouse3-5, with ctrl+/alt+/shift+ prefixes. `binds` lists them.");
        SWSE_ConsolePrint("  a bound key still reaches the game too - pick ones it does not use");
        return;
    }
    int vk, mods;
    if (!ParseKey(argv[1], &vk, &mods)) { Printf("unknown key '%s'", argv[1]); return; }
    char cmd[200]; JoinArgs(argc, argv, 2, cmd, sizeof(cmd));
    SetBind(vk, mods, cmd);
    SaveBinds();
    char k[48]; VkToName(vk, mods, k);
    Printf("%s -> %s  (saved to binds.txt)", k, cmd);
}
static void Cmd_unbind(int argc, char** argv) {
    if (argc < 2) { SWSE_ConsolePrint("usage: unbind <key> | unbind all"); return; }
    if (!lstrcmpiA(argv[1], "all")) { g_bindN = 0; SaveBinds(); SWSE_ConsolePrint("all binds removed"); return; }
    int vk, mods;
    if (!ParseKey(argv[1], &vk, &mods)) { Printf("unknown key '%s'", argv[1]); return; }
    SetBind(vk, mods, nullptr);
    SaveBinds();
    Printf("unbound %s", argv[1]);
}
static void Cmd_binds(int, char**) {
    if (!g_bindN) { SWSE_ConsolePrint("no binds - `bind F5 savepos` makes one"); return; }
    for (int i = 0; i < g_bindN; i++) {
        char k[48]; VkToName(g_binds[i].vk, g_binds[i].mods, k);
        Printf("  %-14s %s", k, g_binds[i].cmd);
    }
}

// Called from HandleInput on a key's down edge while the console is closed.
static void RunBind(int vk) {
    if (!g_bindN || !SWSE_InputReallyFocused()) return;
    int mods = ((GetAsyncKeyState(VK_CONTROL) & 0x8000) ? MOD_C : 0) |
               ((GetAsyncKeyState(VK_MENU)    & 0x8000) ? MOD_A : 0) |
               ((GetAsyncKeyState(VK_SHIFT)   & 0x8000) ? MOD_S : 0);
    for (int i = 0; i < g_bindN; i++)
        if (g_binds[i].vk == vk && g_binds[i].mods == mods) { Execute(g_binds[i].cmd); return; }
    // Shift alone falls back to the plain bind: a key bound as F5 used to do
    // nothing while Shift was held (running, say) unless shift+F5 was bound
    // too. Ctrl and Alt combinations still need a bind of their own - those
    // are the system's and the game's shortcuts.
    if (mods == MOD_S)
        for (int i = 0; i < g_bindN; i++)
            if (g_binds[i].vk == vk && g_binds[i].mods == 0) { Execute(g_binds[i].cmd); return; }
}

// ---- aliases ------------------------------------------------------------------------
// alias heal2 hp %1; stam %1   - a new command made of others. %1..%9 are its
// arguments, %* all of them. Saved to SWSE Console\aliases.txt.
struct Alias { char name[32]; char body[220]; };
static Alias g_alias[64];
static int   g_aliasN = 0;

static void AliasPath(char* out) { char d[MAX_PATH]; GetModDir(d); wsprintfA(out, "%s\\aliases.txt", d); }

static void SaveAliases() {
    char path[MAX_PATH], tmp[MAX_PATH + 8]; AliasPath(path);
    FILE* f = BeginListRewrite(path, tmp, sizeof(tmp),
        "# SWSE aliases - `alias <name> <commands>` writes this file.\n",
        "# <name> <command line; more; wait 500; more>   %1..%9 = arguments\n");
    if (!f) return;
    for (int i = 0; i < g_aliasN; i++) fprintf(f, "%-16s %s\n", g_alias[i].name, g_alias[i].body);
    EndListRewrite(f, tmp, path);
}

static void SetAlias(const char* name, const char* body) {
    for (int i = 0; i < g_aliasN; i++) {
        if (lstrcmpiA(g_alias[i].name, name)) continue;
        if (!body) { g_alias[i] = g_alias[--g_aliasN]; return; }
        lstrcpynA(g_alias[i].body, body, sizeof(g_alias[i].body));
        return;
    }
    if (!body || g_aliasN >= 64) return;
    lstrcpynA(g_alias[g_aliasN].name, name, sizeof(g_alias[g_aliasN].name));
    lstrcpynA(g_alias[g_aliasN].body, body, sizeof(g_alias[g_aliasN].body));
    g_aliasN++;
}

static void LoadAliases() {
    g_aliasN = 0;
    char path[MAX_PATH]; AliasPath(path);
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[300];
    while (fgets(line, sizeof(line), f)) {
        char* s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#' || *s == '\r' || *s == '\n') continue;
        char* e = s; while (*e && *e != ' ' && *e != '\t') e++;
        if (!*e) continue;
        *e++ = 0;
        while (*e == ' ' || *e == '\t') e++;
        char* nl = e + lstrlenA(e);
        while (nl > e && (nl[-1] == '\r' || nl[-1] == '\n' || nl[-1] == ' ')) *--nl = 0;
        if (*e && lstrlenA(s) < 32) SetAlias(s, e);
    }
    fclose(f);
}

static const Alias* FindAlias(const char* name) {
    for (int i = 0; i < g_aliasN; i++)
        if (!lstrcmpiA(g_alias[i].name, name)) return &g_alias[i];
    return nullptr;
}

// Expand %1..%9 and %* in an alias body.
static void ExpandArgs(const char* body, int argc, char** argv, char* out, int outLen) {
    int o = 0;
    for (const char* p = body; *p && o < outLen - 1; p++) {
        if (p[0] == '%' && p[1] >= '1' && p[1] <= '9') {
            int k = p[1] - '0';
            const char* a = (k < argc) ? argv[k] : "";
            while (*a && o < outLen - 1) out[o++] = *a++;
            p++;
        } else if (p[0] == '%' && p[1] == '*') {
            for (int k = 1; k < argc; k++) {
                const char* a = argv[k];
                if (k > 1 && o < outLen - 1) out[o++] = ' ';
                while (*a && o < outLen - 1) out[o++] = *a++;
            }
            p++;
        } else {
            out[o++] = *p;
        }
    }
    out[o] = 0;
}

static void Cmd_alias(int argc, char** argv) {
    if (argc < 3) {
        SWSE_ConsolePrint("usage: alias <name> <commands>   e.g. alias fullheal hp 1000; stam 500");
        SWSE_ConsolePrint("  ';' separates commands, `wait <ms>` pauses, %1..%9 are arguments");
        return;
    }
    if (IsBuiltinCmd(argv[1])) {
        Printf("'%s' is a built-in command - an alias of that name would never run", argv[1]);
        return;
    }
    // Plugin commands resolve before aliases too (1.1).
    if (SWSE_PluginCmdExists(argv[1])) {
        Printf("'%s' is a plugin's command - an alias of that name would never run", argv[1]);
        return;
    }
    // Names are stored in 32 bytes. A longer one used to be cut on the way
    // in, so the full name never matched again - not to run, not to unalias.
    if (lstrlenA(argv[1]) > 31) { SWSE_ConsolePrint("alias names are up to 31 characters"); return; }
    char body[220]; JoinArgs(argc, argv, 2, body, sizeof(body));
    SetAlias(argv[1], body);
    SaveAliases();
    Printf("alias %s = %s  (saved to aliases.txt)", argv[1], body);
}
static void Cmd_unalias(int argc, char** argv) {
    if (argc < 2) { SWSE_ConsolePrint("usage: unalias <name>"); return; }
    SetAlias(argv[1], nullptr);
    SaveAliases();
    Printf("removed alias %s", argv[1]);
}
static void Cmd_aliases(int, char**) {
    if (!g_aliasN) { SWSE_ConsolePrint("no aliases - `alias <name> <commands>` makes one"); return; }
    for (int i = 0; i < g_aliasN; i++) Printf("  %-16s %s", g_alias[i].name, g_alias[i].body);
}

// ---- timing: after / wait / repeat ------------------------------------------------
// A small scheduler, ticked every frame. `wait <ms>` inside a ';' chain, an
// alias, a script or a remote batch delays everything after it.
struct Sched { DWORD due; char line[240]; };
static Sched g_sched[64];
static int   g_schedN = 0;

static void Schedule(const char* line, DWORD delayMs) {
    if (g_schedN >= 64) { SWSE_ConsolePrint("scheduler full (64) - dropped a delayed command"); return; }
    g_sched[g_schedN].due = GetTickCount() + delayMs;
    lstrcpynA(g_sched[g_schedN].line, line, sizeof(g_sched[g_schedN].line));
    g_schedN++;
}

static void SchedTick() {
    DWORD now = GetTickCount();
    for (int i = 0; i < g_schedN; ) {
        if ((int)(now - g_sched[i].due) < 0) { i++; continue; }
        char line[240];
        lstrcpynA(line, g_sched[i].line, sizeof(line));
        g_sched[i] = g_sched[--g_schedN];      // remove before running: it may reschedule
        Execute(line);
    }
}

// `wait` on its own does nothing - it only means something inside a sequence,
// where RunLines consumes it.
static void Cmd_wait(int, char**) {
    SWSE_ConsolePrint("wait <ms> works inside a sequence: `cmd1; wait 500; cmd2`, aliases, scripts");
}

static void Cmd_after(int argc, char** argv) {
    if (argc < 3) { SWSE_ConsolePrint("usage: after <seconds> <command...>   e.g. after 5 heal"); return; }
    double s = atof(argv[1]);
    if (s < 0) s = 0;
    char cmd[240]; JoinArgs(argc, argv, 2, cmd, sizeof(cmd));
    Schedule(cmd, (DWORD)(s * 1000.0));
    Printf("in %s s: %s", argv[1], cmd);
}

static void Cmd_repeat(int argc, char** argv) {
    if (argc < 3) { SWSE_ConsolePrint("usage: repeat <n> <command...>   (n up to 50)"); return; }
    int n = atoi(argv[1]);
    if (n < 1) n = 1;
    if (n > 50) n = 50;
    char cmd[240]; JoinArgs(argc, argv, 2, cmd, sizeof(cmd));
    // Counted as nesting: `repeat 50 repeat 50 repeat 50 repeat 50 x` was
    // 6.25 million commands on the render thread. Execute's per-line budget
    // stops a runaway; the depth stops the pyramid being built at all.
    if (g_execDepth >= 8) { SWSE_ConsolePrint("command nesting too deep - aborting."); return; }
    g_execDepth++;
    for (int i = 0; i < n; i++) Execute(cmd);
    g_execDepth--;
}

// exec <file> - run a text file of commands (path relative to SWSE Console).
static void Cmd_exec(int argc, char** argv) {
    if (argc < 2) { SWSE_ConsolePrint("usage: exec <file.txt>   (relative to SWSEMods\\SWSE Console)"); return; }
    // A file that execs itself used to recurse until the stack ran out, and
    // a nested exec overwrote the outer file's lines (one static buffer).
    if (g_execDepth >= 8) { SWSE_ConsolePrint("exec nesting too deep - aborting."); return; }
    char path[MAX_PATH];
    if (strchr(argv[1], ':') || argv[1][0] == '\\') lstrcpynA(path, argv[1], MAX_PATH);
    else { char d[MAX_PATH]; GetModDir(d); _snprintf_s(path, MAX_PATH, _TRUNCATE, "%s\\%s", d, argv[1]); }
    FILE* f = fopen(path, "r");
    if (!f) { Printf("could not open %s", path); return; }
    char (*lines)[240] = (char (*)[240])malloc(64 * 240);
    if (!lines) { fclose(f); return; }
    int n = 0;
    char buf[260];
    while (fgets(buf, sizeof(buf), f) && n < 64) {
        char* s = buf;
        while (*s == ' ' || *s == '\t') s++;
        char* e = s + lstrlenA(s);
        while (e > s && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' ')) *--e = 0;
        if (!*s || *s == '#') continue;
        lstrcpynA(lines[n++], s, 240);
    }
    fclose(f);
    g_execDepth++;
    RunLines(lines, n);
    g_execDepth--;
    free(lines);
}

// ---- status for people, query for tools ----------------------------------------------
static void Cmd_status(int, char**) {
    Printf("SWSE %s   level %s  (epoch %u, up %u s)", SWSE_VERSION,
           SWSE_LevelUp() ? "up" : "not loaded", SWSE_LevelEpoch(), SWSE_LevelAgeMs() / 1000);
    if (!SWSE_GameBuildKnown() || lstrcmpA(SWSE_GameBuildName(), "Steam HD")) {
        char gb[240]; SWSE_GameBuildDescribe(gb, sizeof(gb)); SWSE_ConsolePrint(gb);
    }
    // Safe mode: no position, health or moolah lines - every read of the
    // player calls the game, and scriptvm.cpp answers "no player" there.
    if (SWSE_GameBuildSafeMode())
        SWSE_ConsolePrint("  safe mode: the console, mailbox, graphics, HD textures, foliage and ray "
                          "tracing work; nothing that patches, calls or reads the game runs - `help` "
                          "marks the commands this refuses");
    float p[3];
    if (SWSE_PosGet(p) == 1) {
        char xs[32], ys[32], zs[32], ws[32] = "?";
        Ff(xs, p[0], 1); Ff(ys, p[1], 1); Ff(zs, p[2], 1);
        float yaw; if (SWSE_PlayerYawGet(&yaw) == 1) Ff(ws, yaw, 1);
        Printf("  at %s %s %s facing %s%s%s", xs, ys, zs, ws,
               SWSE_CurrentLevel()[0] ? " in " : "", SWSE_CurrentLevel());
    }
    float c = 0, m = 0, b = 0;
    if (SWSE_PlayerHealth(&c, &m, &b) == 1) {
        float sc = 0, sm = 0, sb = 0; SWSE_PlayerStamina(&sc, &sm, &sb);
        float mo = 0;
        if (SWSE_Moolah(&mo) == 1)
            Printf("  health %d/%d  stamina %d/%d  moolah %d",
                   (int)c, (int)m, (int)sc, (int)sm, (int)mo);
        else
            Printf("  health %d/%d  stamina %d/%d", (int)c, (int)m, (int)sc, (int)sm);
    }
    // Bounded: with plugin switches (1.1) the list has no fixed length.
    char on[230] = "";
    int ou = 0;
    for (int i = 0; i < SWSE_FeatureTotal(); i++) {
        if (!SWSE_Feature((SwseFeature)i)) continue;
        int k = _snprintf_s(on + ou, sizeof(on) - ou, _TRUNCATE, "%s%s", ou ? ", " : "",
                            SWSE_FeatureName((SwseFeature)i));
        if (k < 0) break;
        ou += k;
    }
    Printf("  features on: %s", on);
    SWSE_PluginsStatus(SWSE_ConsolePrint);    // plugins found/on, and their status lines
}

// query <player|features|position|version> - one line of key=value pairs, for
// tools on the remote mailbox (no prose to parse).
static void Cmd_query(int argc, char** argv) {
    const char* what = (argc > 1) ? argv[1] : "player";
    char out[480]; int u = 0;
    if (!lstrcmpiA(what, "features")) {
        // Every switch, plugins' too (1.1) - bounded, since that list grows.
        // The console shows 239 characters; the mailbox reply gets it all.
        static char all[4096];
        int au = 0;
        all[0] = 0;
        for (int i = 0; i < SWSE_FeatureTotal(); i++) {
            int k = _snprintf_s(all + au, sizeof(all) - au, _TRUNCATE, "%s%s=%s", i ? " " : "",
                                SWSE_FeatureName((SwseFeature)i), SWSE_Feature((SwseFeature)i) ? "on" : "off");
            if (k < 0) break;
            au += k;
        }
        SWSE_ConsolePrint(all);
        return;
    }
    if (!lstrcmpiA(what, "plugins")) {
        // <plugin>=on|off|faulted|refused for every plugin found (1.1).
        static char pl[4096];
        SWSE_PluginsQuery(pl, sizeof(pl));
        SWSE_ConsolePrint(pl);
        return;
    }
    if (!lstrcmpiA(what, "contract")) {
        // The tool contract's version (TOOL_CONTRACT.md): bumped only by a
        // change that breaks what that page promises.
        SWSE_ConsolePrint("contract=1 swse=" SWSE_VERSION);
        return;
    }
    if (!lstrcmpiA(what, "version")) {
        // build= names the game exe SWSE recognised ("Steam HD"), or unknown.
        char b[120];
        char name[40]; lstrcpynA(name, SWSE_GameBuildName(), sizeof(name));
        for (char* c = name; *c; c++) if (*c == ' ') *c = '_';
        _snprintf_s(b, sizeof(b), _TRUNCATE, "swse=%s build=%s", SWSE_VERSION, name);
        SWSE_ConsolePrint(b);
        return;
    }
    // player / position
    u += wsprintfA(out + u, "levelup=%d epoch=%u level=%s", SWSE_LevelUp() ? 1 : 0,
                   SWSE_LevelEpoch(), SWSE_CurrentLevel()[0] ? SWSE_CurrentLevel() : "-");
    // Safe mode (gamebuild.h): the level watcher does not run, so levelup
    // stays 0, and every other key would be a read of the game. safemode=1
    // tells a tool it is not "no level yet" but "never on this build".
    if (SWSE_GameBuildSafeMode()) {
        wsprintfA(out + u, " safemode=1");
        SWSE_ConsolePrint(out);
        return;
    }
    float p[3];
    if (SWSE_PosGet(p) == 1) {
        char xs[32], ys[32], zs[32];
        Ff(xs, p[0], 3); Ff(ys, p[1], 3); Ff(zs, p[2], 3);
        u += wsprintfA(out + u, " x=%s y=%s z=%s", xs, ys, zs);
    }
    float yaw;
    if (SWSE_PlayerYawGet(&yaw) == 1) { char ws[32]; Ff(ws, yaw, 2); u += wsprintfA(out + u, " yaw=%s", ws); }
    if (lstrcmpiA(what, "position")) {
        float c = 0, m = 0, b = 0;
        if (SWSE_PlayerHealth(&c, &m, &b) == 1) {
            char a1[32], a2[32]; Ff(a1, c, 1); Ff(a2, m, 1);
            u += wsprintfA(out + u, " health=%s healthmax=%s", a1, a2);
        }
        if (SWSE_PlayerStamina(&c, &m, &b) == 1) {
            char a1[32], a2[32]; Ff(a1, c, 1); Ff(a2, m, 1);
            u += wsprintfA(out + u, " stamina=%s staminamax=%s", a1, a2);
        }
        float mo = 0;
        if (SWSE_Moolah(&mo) == 1) u += wsprintfA(out + u, " moolah=%d", (int)mo);
    }
    SWSE_ConsolePrint(out);
}

// mods [reload] - the enabled mod folders in load order, and which conventional
// files each provides. `reload` rescans SWSEMods (a folder added while the game
// runs) and re-reads every per-mod file the running systems use.
static void Cmd_mods(int argc, char** argv) {
    if (argc > 1 && !lstrcmpiA(argv[1], "reload")) {
        int n = SWSE_ModsReload();
        int pos = SWSE_PositionsLoad();
        char msg[300];
        Printf("rescanned: %d enabled mod(s), %d position(s)", n, pos);
        if (SWSE_Feature(FEAT_AITUNING))   { SWSE_AiTuneLoad(msg, sizeof(msg)); SWSE_ConsolePrint(msg); }
        if (SWSE_Feature(FEAT_TRIGGERS))   Printf("triggers: %d", SWSE_TriggersLoad());
        if (SWSE_Feature(FEAT_PLAYERTUNE)) { SWSE_PlayerTuneLoad(msg, sizeof(msg)); SWSE_ConsolePrint(msg); }
        if (SWSE_Feature(FEAT_PREFSEDIT))  { SWSE_PrefsEditLoad(msg, sizeof(msg)); SWSE_ConsolePrint(msg); }
        if (SWSE_Feature(FEAT_NPCTUNING))  { SWSE_NpcTuningLoadAll(msg, sizeof(msg)); SWSE_ConsolePrint(msg); }
        LoadDynCmds();
        // Plugins (1.1): new DLLs become switches (off); plugins that are on
        // hear MODS_RELOADED and re-read their own files.
        SWSE_PluginsRescan();
        SWSE_PluginsModsReloaded();
        return;
    }
    static const char* kFiles[] = {
        "features.txt", "aiprefs.txt", "playerprefs.txt", "prefs.txt", "characters.txt",
        "console.txt", "triggers.txt", "positions.txt", "sites.txt", "foliage.txt",
        "wind.txt", "hitreact.txt", "graphics.txt", "pointers.txt", "textures", "scripts",
        "plugins",
    };
    int n = SWSE_ModCount();
    Printf("%d enabled mod(s) in load order (later wins for single-value files):", n);
    for (int i = 0; i < n; i++) {
        char have[200] = "";
        for (int k = 0; k < (int)(sizeof(kFiles) / sizeof(kFiles[0])); k++) {
            char path[MAX_PATH];
            wsprintfA(path, "%s\\%s", SWSE_ModPath(i), kFiles[k]);
            if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;
            if (lstrlenA(have) + lstrlenA(kFiles[k]) + 2 >= (int)sizeof(have)) break;
            if (have[0]) lstrcatA(have, " ");
            lstrcatA(have, kFiles[k]);
        }
        Printf("  %2d %-22s %s", i + 1, SWSE_ModName(i), have[0] ? have : "(no SWSE files)");
    }
    SWSE_ConsolePrint("`mods reload` after adding a folder or editing load_order.txt");
}

// npccache [rebuild] - SWSE's own live-NPC list (what triggers' killed/cleared
// count). A worker thread rescans the heap every 20 s while a trigger needs
// it, and at once after a level change; with hit reactions on, their
// watcher's list is used instead.
static void Cmd_npccache(int argc, char** argv) {
    if (argc > 1 && !lstrcmpiA(argv[1], "rebuild")) {
        SWSE_NpcCacheReset();
        SWSE_ConsolePrint("npc list dropped - rescanned in the background (while a trigger needs it)");
        return;
    }
    int n = 0, sc = 0, passes = 0, lastMs = 0, w = 0;
    SWSE_NpcCacheInfo(&n, &sc, &passes, &lastMs, &w);
    if (w) SWSE_ConsolePrint("hit reactions are watching - their actor list is used");
    Printf("own list: %s%d NPC(s), %s, %d pass(es), the last took %d ms (worker thread)",
           n < 0 ? "not built, " : "", n < 0 ? 0 : n, sc ? "scanning" : "idle", passes, lastMs);
    Printf("counted now (all types): %d", SWSE_CountNpcsOfType(0));
}

// mute [on|off] - the game's own audio session (Volume Mixer's per-app mute),
// so the setting is the player's, not a change to the game's sound options.
static void Cmd_mute(int argc, char** argv) {
    int on = 1;
    if (argc > 1) on = !(!lstrcmpiA(argv[1], "off") || !lstrcmpiA(argv[1], "0"));
    else if (SWSE_GameMuted()) on = 0;            // bare `mute` toggles
    int n = SWSE_SetGameMute(on, 2000);
    if (n > 0)       Printf("game audio %s (%d device(s))", on ? "MUTED" : "on", n);
    else if (n == 0) SWSE_ConsolePrint("no playback device took the change - is any audio device active?");
    else             SWSE_ConsolePrint("could not reach Windows audio (Core Audio unavailable)");
}

// hooks - the hook registry (1.1): every patch SWSE has made to code or a
// pointer table, and which system owns it. Two patches on one function break
// each other silently, so the list is the first thing to check when something
// else in the process hooks too.
static void Cmd_hooks(int, char**) {
    SWSE_HooksList(SWSE_ConsolePrint);
    SWSE_ConsolePrint("(what is patched depends on the switches on; a plugin's Memory.Write refuses these ranges)");
}

// hide / show - close or open the console overlay (for tools on the mailbox,
// which can type commands but cannot press the ~ key).
static void Cmd_hide(int, char**) { SetOpen(false); }
static void Cmd_show(int, char**) { SetOpen(true); }

// log <text> - write a line to swse_log.txt (for scripts and tools).
static void Cmd_log(int argc, char** argv) {
    char line[240]; JoinArgs(argc, argv, 1, line, sizeof(line));
    char path[MAX_PATH];
    GetModuleFileNameA(GetModuleHandleA(NULL), path, MAX_PATH);
    char* sl = strrchr(path, '\\'); if (sl) *sl = 0;
    lstrcatA(path, "\\swse_log.txt");
    FILE* f = fopen(path, "a");
    if (f) { fprintf(f, "LOG: %s\n", line); fclose(f); }
}

static Cmd g_cmds[] = {
    // ---- player -----------------------------------------------------------
    { "hp",         "player", "hp [value] - read/set health",           Cmd_hp },
    { "stam",       "player", "stam [value] - read/set stamina",        Cmd_stam },
    { "sethealth",  "player", "sethealth <n> - sets max too",           VMcmd },
    { "heal",       "player", "restore health + stamina",               VMcmd },
    { "maxhealth",  "player", "max health",                             VMcmd },
    { "maxstamina", "player", "max stamina",                            VMcmd },
    { "god",        "player", "toggle invulnerability",                 Cmd_god },
    { "kill",       "player", "kill current target/self",               VMcmd },
    { "pfield",     "player", "pfield <hexOff> [v] - any player field", Cmd_pfield },

    // ---- movement ---------------------------------------------------------
    { "pos",        "movement", "print your X Y Z and facing",          Cmd_pos },
    { "savepos",    "movement", "savepos [label] - save spot AND facing (label = to file)", Cmd_savepos },
    { "tp",         "movement", "tp [label] - back to the saved spot, facing restored", Cmd_tp },
    { "tpxyz",      "movement", "tpxyz <x> <y> <z> [yaw] - teleport to coordinates", Cmd_tpxyz },
    { "yaw",        "movement", "yaw [deg] - read or set the way you face", Cmd_yaw },
    { "up",         "movement", "up [dist] - lift you dist units (default 10) and let you fall", Cmd_up },
    { "move",       "movement", "move <axis 0|1|2> <delta> - step along x/y (onto the floor) or z", Cmd_move },
    { "gravity",    "movement", "gravity [value] - 27 default, 7 floats", Cmd_gravity },
    { "aircontrol", "movement", "aircontrol [value] - midair steering", Cmd_aircontrol },
    { "jump",       "movement", "jump [height] - player motion obj",    Cmd_jump },
    { "speed",      "movement", "speed [value] - player motion obj",    Cmd_speed },

    // ---- items ------------------------------------------------------------
    { "grant",        "items", "grant <artifact|path> [qty]",           Cmd_grant },
    { "giveartifact", "items", "giveartifact <name> [qty]",             Cmd_giveartifact },
    { "allartifacts", "items", "give every artifact",                   Cmd_allartifacts },
    { "artifacts",    "items", "artifacts [filter] - list all 53",      Cmd_artifacts },
    { "giveweapon",   "items", "giveweapon <name> - e.g. crossbow",     Cmd_giveweapon },
    { "moolah",       "items", "moolah [value] - read/set",             Cmd_moolah },
    { "money",        "items", "money [amount] (default 10000)",        Cmd_money },
    { "ammo",         "items", "give all ammo",                         VMcmd },
    { "defaultammo",  "items", "give default ammo",                     VMcmd },
    { "noammo",       "items", "take all ammo",                         VMcmd },
    { "crossbow",     "items", "give the crossbow",                     VMcmd },
    { "noweapons",    "items", "take all weapons",                      VMcmd },
    { "artifact",     "items", "give artifact (raw VM call)",           VMcmd },

    // ---- world ------------------------------------------------------------
    { "spawn",      "world", "spawn [n] - fire the level's spawners",    Cmd_spawn },
    { "spawnhere",  "world", "spawnhere [n] - move a spawner to you",    Cmd_spawnhere },
    { "critters",   "world", "critters [n] - enable critter spawning",   Cmd_critters },
    { "npcspy",     "debug", "npcspy [off] - capture a real NPC creation", Cmd_npcspy },
    { "npchits",    "debug", "npchits - how many times the spawn routine ran", Cmd_npchits },
    { "npcdupe",    "world", "npcdupe [n] - re-run the game's own spawn n extra times", Cmd_npcdupe },
    { "buildtest",  "debug", "buildtest [n] - constructed spawns during a load", Cmd_buildtest },
    { "dupetype",   "world", "dupetype <hash> - make a level spawn that character", Cmd_dupetype },
    { "npccount",   "world", "npccount [n] - raise a spawn tag's count to n", Cmd_npccount },
    { "npcreplay",  "world", "npcreplay - re-run the captured spawn right now", Cmd_npcreplay },
    { "npcnow",     "world", "npcnow [n] [type] - spawn n NPCs at you now",   Cmd_npcnow },
    { "spawntypes", "world", "spawntypes - NPC types harvested from this level", Cmd_spawntypes },
    { "npcnear",    "world", "npcnear - identify the NPC next to you, to clone it", Cmd_npcnear },
    { "bring",      "world", "bring [n] [type] - teleport live NPCs to you",  Cmd_bring },
    { "sendnpc",    "world", "sendnpc [n] [type] - send NPCs at you (their AI)", Cmd_sendnpc },
    { "resolve",    "debug", "resolve <hash> - resolve a type hash to its prefs", Cmd_resolve },
    { "ai",         "world", "ai <hash> [field value] - AI perception + weapon timing", Cmd_ai },
    { "uispy",      "debug", "uispy [on|off|reset] - log every menu command (fscommand) by screen", Cmd_uispy },
    { "findai",     "debug", "findai [ms] - locate NPC perception prefs by memory shape", Cmd_findai },
    { "npcguns",    "world", "npcguns [ms] - which character carries which gun, with hp/bounty", Cmd_npcguns },
    { "weapons",    "debug", "weapons [ms] - NPC weapon timing: fire rate, accuracy, miss time", Cmd_weapons },
    { "features",   "tuning", "features [name on|off] | preset full|default - list/switch SWSE systems live", Cmd_features },
    { "playertune", "tuning", "playertune [reload|apply|restore] - playerprefs.txt", Cmd_playertune },
    { "prefs",      "tuning", "prefs find|get|set|keep|dump|fields - live prefs editor", Cmd_prefs },
    { "knockback",  "tuning", "knockback [ammo|all] [npc] [player] [keep] - ammo knockback", Cmd_knockback },
    { "npc",        "tuning", "npc <type> [field [value]] [keep] - any character field by name", Cmd_npc },
    { "wpn",        "tuning", "wpn <ammo|npcweapon|hash|all> [field [value]] [keep] - weapon fields", Cmd_wpn },
    { "writepos",   "world", "writepos <label> - save where you stand + facing, by name", Cmd_writepos },
    { "positions",  "world", "positions - list saved positions", Cmd_positions },
    { "goto",       "world", "goto <label> - teleport to a saved position (and facing)", Cmd_goto },
    { "spawnat",    "world", "spawnat <label> [n] [type] - move NPCs to a position", Cmd_spawnat },
    { "playnpc",    "world", "playnpc [name|off|status] - play as a character from this level (loads it first if needed)", SWSE_PlayNpcCmd },
    { "freecam",    "world", "freecam [on|off|toggle] | pos <x> <y> <z> | look <yaw> [pitch] | speed [n] | sens [n] - fly the view with the game's own dev camera", SWSE_FreecamCmd },
    { "reserve",    "world", "reserve [n] | reserve park - spare NPCs for ambushes", Cmd_reserve },
    { "triggers",   "world", "triggers [list|reload|test <name>|on|off]", Cmd_triggers },
    { "difficulty", "world", "difficulty <name|off> - apply an AI profile from aiprefs.txt", Cmd_difficulty },
    { "aitune",     "world", "aitune - reload aiprefs.txt", Cmd_aitune },
    { "spawnradius","world", "spawnradius <tenths> - how far spawns ring out", Cmd_spawnradius },
    { "strhash",    "debug", "strhash <path> - hash a path with the game's hasher", Cmd_strhash },
    { "spawngate",  "debug", "spawngate - is the spawn routine's guard open?", Cmd_spawngate },
    { "whatis",     "debug", "whatis <addr> - name an object's class via RTTI", Cmd_whatis },
    { "instances",  "debug", "instances <addr> - all live objects of that class", Cmd_instances },
    { "nearby",     "debug", "nearby [radius] - name every object around you", Cmd_nearby },
    { "diff",       "debug", "diff <a> <b> [len] - differing fields of 2 objects", Cmd_diff },
    { "difftypes",  "debug", "difftypes <hashA> <hashB> - diff two characters", Cmd_difftypes },
    { "npchealth",  "world", "npchealth <type> [hp] - set a character's health", Cmd_npchealth },
    { "npcelite",   "world", "npcelite <type|*> <pct> [hp] - promote a fraction to elites", Cmd_npcelite },
    { "npcgib",     "world", "npcgib <type> [0|1] - gib that character on death", Cmd_npcgib },
    { "whereis",    "world", "whereis <type> - locate every NPC of that type",   Cmd_whereis },
    { "townpanic",  "world", "townpanic [on|off] [radius] - the town alarm system", Cmd_townpanic },
    { "raid",       "world", "raid [zone] [bell] - post the town alarm",        Cmd_raid },
    { "attack",     "world", "attack <atkType> <victimType> [n] - NPC vs NPC", Cmd_attack },
    { "findtarget", "debug", "findtarget [npc] - where an NPC stores its target", Cmd_findtarget },
    { "raidmode",   "world", "raidmode <atk> <victim> - standing NPC hostility", Cmd_raidmode },
    { "scantargets","debug", "scantargets - who every active NPC is fighting", Cmd_scantargets },
    { "decoy",      "world", "decoy <shooter> <victim> - make shots land on the victim", Cmd_decoy },
    { "feud",       "world", "feud <typeA> <typeB> - inject mutual damage (untested)", Cmd_feud },
    { "npchurt",    "world", "npchurt <type> [0|2] - 2 = cannot be staggered",  Cmd_npchurt },
    { "npcaff",     "world", "npcaff <type> [0|1] - player ammo affects it (0 = immune; NOT affiliation)", Cmd_npcaff },
    { "allnpcs",    "world", "allnpcs <hp|-> [gib] - apply to every character here", Cmd_allnpcs },
    { "tuning",     "world", "tuning - reload characters.txt (hp/gib per character)", Cmd_tuning },
    { "types",      "world", "types [dump] - characters here + hp/gib, or to a file", Cmd_types },
    { "npclast",    "world", "npclast - replay the captured NPC creation", Cmd_npclast },
    { "npchere",    "world", "npchere - spawn the captured NPC at you",  Cmd_npchere },
    { "spawnnpc",   "world", "spawnnpc - retired: it moved a live piece of the level (npcnow instead)", Cmd_spawnnpc },
    { "spawnclone", "world", "spawnclone - clone a captured NPC at you", Cmd_spawnclone },
    { "npcspawn",   "world", "npcspawn - replay a captured spawn",       Cmd_npcspawn },
    { "npctags",    "debug", "npctags - find live NPC spawn tags",       Cmd_npctags },
    { "npcs",       "debug", "npcs - find live NPCs in the level",       Cmd_npcs },
    { "npctypes",   "debug", "npctypes - list spawnable NPC types",      Cmd_npctypes },
    { "geominst",   "debug", "geominst - find spawn anchors",            Cmd_geominst },
    { "anim",       "world", "anim <torso|endtorso|stop> [n] [tag] - animation probe", Cmd_anim },
    { "granny",     "debug", "granny [minBones] | granny dump <addr> - find bone poses", Cmd_granny },
    { "peek",       "debug", "peek <hexaddr> [dwords] - hex/float/ascii memory view", Cmd_peek },
    { "agentdebug", "debug", "agentdebug [on|off] - run unfocused, desktop stays usable", Cmd_agentdebug },
    { "mute",       "debug", "mute [on|off] - mute the game's sound (Windows per-app mute)", Cmd_mute },
    { "snap",       "debug", "snap [file.tga] - screenshot from inside the engine", Cmd_snap },
    { "shaderdump", "graphics", "shaderdump [file|stats] - dump the game's shader programs", Cmd_shaderdump },
    { "hd",         "graphics", "hd - HD texture replacement stats, and which .oft files failed", Cmd_hd },
    { "foliage",    "graphics", "foliage [on|scan|scanned|progs|reload] - identify foliage draws by texture", Cmd_foliage },
    { "wind",       "graphics", "wind on|off|save|weight|push|test|axis|seed|gate|<strength> - foliage wind", Cmd_wind },
    { "vtscan",     "debug", "vtscan <hexVtable> [max] [fieldHex] - find objects of a class", Cmd_vtscan },
    { "background", "debug", "alias for agentdebug",                     Cmd_agentdebug },
    { "hitreact",   "world", "hitreact on|off|test|<strength> <ms> - additive hit reactions", Cmd_hitreact },
    { "proj",       "graphics", "proj [scan] - the game's real near/far/FOV", Cmd_proj },
    { "depthtex",   "graphics", "depthtex [id|auto] - list/pin the scene depth texture", Cmd_depthtex },
    { "fbotrace",   "graphics", "fbotrace [n] - log the frame's FBO bind order", Cmd_fbotrace },
    // NOT "fps": that name was already taken by the first-person view command
    // below, and registering it twice silently shadowed it.
    { "framerate",  "graphics", "framerate - fps since the previous call",  Cmd_fps },
    { "perf",       "graphics", "perf - where a stutter came from (SWSE or the game)", Cmd_perf },
    { "selftest",   "debug", "selftest - check every SWSE feature is operational", Cmd_selftest },
    { "key",        "input", "key <name...> - tap keys into the game",   Cmd_key },
    { "menu",       "input", "menu list|continue|skip|fs ... - drive the menus without keys", Cmd_menu },
    { "newgame",    "input", "newgame [1-3] - new game (1 easy/2 normal/3 hard)", Cmd_newgame },
    { "continue",   "input", "continue - CONTINUE from the menu",        Cmd_continue },
    { "skipcut",    "input", "skipcut - try to skip the current cutscene", Cmd_skipcut },
    { "inputst",    "input", "inputst - what the game's input looks like", Cmd_inputst },
    { "warp",       "world", "warp <level|0-6> - load a level",         Cmd_warp },
    { "levels",     "world", "list the 8 level names",                  Cmd_levels },
    { "steef",      "world", "transform into Steef",                    VMcmd },
    { "stranger",   "world", "transform back to Stranger",              VMcmd },
    { "naked",      "world", "Steef naked toggle",                      VMcmd },
    { "fps",        "world", "force first-person view",                 VMcmd },
    { "nofps",      "world", "force third-person view",                 VMcmd },
    { "sniper",     "world", "force sniper view",                       VMcmd },
    { "tphome",     "world", "teleport home",                           VMcmd },
    { "tpreset",    "world", "teleport reset",                          VMcmd },
    { "save",       "world", "quick save",                              VMcmd },
    { "checkpoint", "world", "set checkpoint",                          VMcmd },
    { "loadsave",   "world", "load last save",                          VMcmd },
    { "healthbars", "world", "show enemy health bars",                  VMcmd },
    { "weaponhud",  "world", "open weapon HUD",                         VMcmd },

    // ---- graphics ---------------------------------------------------------
    { "gfx",  "graphics", "gfx on|off|toggle|reload",                   Cmd_gfx },
    { "set",  "graphics", "set <key> <value> - live tune",              Cmd_set },

    // ---- scripting --------------------------------------------------------
    { "list",          "scripting", "list [filter] - 181 game functions", Cmd_list },
    { "call",          "scripting", "call <function> [args]",           Cmd_call },
    { "scripts",       "scripting", "your custom .txt commands",        Cmd_scripts },
    { "reloadscripts", "scripting", "reload scripts\\ after editing",   Cmd_reloadscripts },
    { "bind",          "scripting", "bind <key> <command> - run a command from a key", Cmd_bind },
    { "unbind",        "scripting", "unbind <key>|all",                  Cmd_unbind },
    { "binds",         "scripting", "list key binds",                    Cmd_binds },
    { "alias",         "scripting", "alias <name> <cmd; cmd> - make a command", Cmd_alias },
    { "unalias",       "scripting", "unalias <name>",                    Cmd_unalias },
    { "aliases",       "scripting", "list aliases",                      Cmd_aliases },
    { "after",         "scripting", "after <sec> <command> - run it later", Cmd_after },
    { "wait",          "scripting", "wait <ms> - pause inside a ; sequence/script", Cmd_wait },
    { "repeat",        "scripting", "repeat <n> <command>",              Cmd_repeat },
    { "exec",          "scripting", "exec <file> - run a file of commands", Cmd_exec },
    { "log",           "scripting", "log <text> - write a line to swse_log.txt", Cmd_log },
    { "ptr",           "scripting", "list pointer chains + values",     Cmd_ptr },
    { "get",           "scripting", "get <name> - read a chain",        Cmd_get },
    { "hold",          "scripting", "hold <name> <value> - freeze",     Cmd_hold },
    { "unhold",        "scripting", "unhold <name>",                    Cmd_unhold },
    { "ptrreload",     "scripting", "reload pointers.txt",              Cmd_ptrreload },

    // ---- debug / reverse engineering --------------------------------------
    { "autoprime",  "debug", "find a ScriptContext (no ammo needed)",   Cmd_autoprime },
    { "findval",    "debug", "findval <n> - exact search",              Cmd_findval },
    { "narrow",     "debug", "narrow <n> - keep hits now holding n",    Cmd_narrow },
    { "watchaddr",  "debug", "watchaddr <hex> - what writes here?",     Cmd_watchaddr },
    { "watchexec",  "debug", "watchexec <rva> [once] - is this code reached?", Cmd_watchexec },
    { "watchrw",    "debug", "watchrw <hex> [len] - what READS this?",  Cmd_watchrw },
    { "watchoff",   "debug", "disarm the watchpoint",                   Cmd_watchoff },
    { "watchinv",   "debug", "watch the inventory pointer",             Cmd_watchinv },
    { "dumpaddr",   "debug", "dumpaddr <hex> [n] - inspect memory",     Cmd_dumpaddr },
    { "invdump",    "debug", "dump the player/inventory objects",       Cmd_invdump },
    { "probe",      "debug", "dump player object to log",               Cmd_probe },
    { "find",       "debug", "find <value> - scan the ctx graph",       Cmd_find },
    { "ctxinfo",    "debug", "identify ctx's class via vtable",         Cmd_ctxinfo },
    { "vcall",      "debug", "vcall <slot> [a0] [a1]",                  Cmd_vcall },
    { "watch",      "debug", "watch [secs] [label] - log changes",      Cmd_watch },
    { "poke",       "debug", "poke <selfHex> <subHex> <value>",         Cmd_poke },
    { "freeze",     "debug", "freeze <selfHex> <subHex> <value>",       Cmd_freeze },
    { "unfreeze",   "debug", "unfreeze <selfHex> <subHex>",             Cmd_unfreeze },
    { "unfreezeall","debug", "release every frozen field",              Cmd_unfreezeall },
    { "anchor",     "debug", "anchor <ptr>|off - aim watch/probe",      Cmd_anchor },
    { "spy",        "debug", "spy [lines] | off - trace script calls",  Cmd_spy },
    { "grantspy",   "debug", "grantspy [off] - log a real purchase",    Cmd_grantspy },
    { "grantlast",  "debug", "grantlast [qty] - replay it",             Cmd_grantlast },
    { "remote",     "debug", "remote [on|off] - external channel",      Cmd_remote },

    // ---- music ------------------------------------------------------------
    { "combatmusic",  "music", "combatmusic <0|1>",                     VMcmd },
    { "tensionmusic", "music", "tensionmusic <0|1>",                    VMcmd },
    { "popmusic",     "music", "pop current music layer",               VMcmd },
    { "pushmusic",    "music", "pushmusic <type#>",                     VMcmd },
    { "transmusic",   "music", "transmusic <type#>",                    VMcmd },

    // ---- console ----------------------------------------------------------
    { "help",  "console", "help [category|command]",                    Cmd_help },
    { "status", "console", "status - where you are, health, what is on", Cmd_status },
    { "query",  "console", "query [player|position|features|plugins|version] - key=value for tools", Cmd_query },
    { "mods",   "console", "mods [reload] - enabled mod folders and their files", Cmd_mods },
    { "plugins", "console", "plugins [name|rescan] - native plugins: state, cost, faults", SWSE_PluginsCommand },
    { "hooks",  "debug", "hooks - every live code/table patch and the system that owns it", Cmd_hooks },
    { "npccache", "debug", "npccache [rebuild] - SWSE's live-NPC list (triggers)", Cmd_npccache },
    { "hide",   "console", "hide - close the console overlay",       Cmd_hide },
    { "show",   "console", "show - open the console overlay",        Cmd_show },
    { "clear", "console", "clear the console",                          Cmd_clear },
    { "echo",  "console", "echo text",                                  Cmd_echo },
    { "ver",   "console", "show version",                               Cmd_ver },
    { "ssrmask", "graphics", "ssrmask [reload] - SSR material mask status",  Cmd_ssrmask },
    { "gpu",     "raytrace", "gpu - compute + ray-math capability probe",    Cmd_gpu },
    { "geo",     "raytrace", "geo [show] - capture draw geometry, prove space", Cmd_geo },
    { "harvest", "raytrace", "harvest [show|dump N] - world triangle soup",  Cmd_harvest },
    { "skin",    "debug", "skin [show] - census this frame's CHARACTER draws", Cmd_skin },
    { "meshdump","debug", "meshdump [stop] - capture character meshes for Oddview", Cmd_meshdump },
    { "hidepart","debug", "hidepart <n>|show <n>|list|clear - drop a character part", Cmd_hidepart },
    { "noponcho","debug", "noponcho [off|<indexCount>] - hide Stranger's poncho", Cmd_noponcho },
    { "nohat",   "debug", "nohat [off|<indexCount>] - hide Stranger's hat", Cmd_nohat },
    { "nodreads","debug", "nodreads [off|<indexCount>] - hide Stranger's dreadlocks", Cmd_nodreads },
    { "rt",      "raytrace", "rt [build] - BVH + camera trace status",       Cmd_rt },
    { "progsrc", "raytrace", "progsrc <id> - a program's position math",     Cmd_progsrc },
    { "exit",  "console", "quit the game (lets the DLL be reinstalled)", Cmd_exit },
};
static const int N_CMDS = sizeof(g_cmds) / sizeof(g_cmds[0]);

// ---- safe mode: the commands the console refuses on an unknown game build --
// gamebuild.h: on a build SWSE's addresses were not measured on, nothing may
// patch, call or read the game through a Steam address. Decided here, once,
// by category, then by name where a category is mixed; Execute refuses with
// SWSE_GameBuildRefusal's text and `help` marks the same commands. What each
// category does (the review behind the lists):
//   player, movement, items, music - the player object, its motion, the wallet
//     and the game's script verbs: every command calls or reads the game.
//     Refused.
//   world - NPCs, spawners, AI, zones, warp and teleports, hit reactions,
//     playnpc, freecam, triggers; `levels` and `positions` only list names for
//     warp and goto. Refused.
//   tuning - the prefs registry and the player's fields. Refused, except
//     `features`, SWSE's own switchboard.
//   debug - mostly game structures: NPC lists, the script context, the spies,
//     Steam vtables. Refused, except SWSE's own (hooks, selftest, snap, mute,
//     agentdebug/background, remote), the OpenGL draw hook's (skin, meshdump,
//     hidepart, noponcho, nohat, nodreads), and the plain memory readers that
//     read only an address you type, never a Steam one (peek, dumpaddr, whatis,
//     diff, instances, vtscan, findval, narrow) - what porting SWSE to another
//     build needs.
//   graphics, raytrace - OpenGL: the pipeline, texture uploads, shader
//     programs, the draw hook, a camera from shader constants. Run. (`set`
//     refuses a pointer chain itself; wind's player push reads nothing here.)
//   input - DirectInput key presses; `menu` refuses its keyless half itself
//     (menu.cpp). Run.
//   scripting - binds, aliases, exec, after, repeat, wait, scripts: SWSE's
//     own, and every command they run meets this gate itself. Run, except
//     `call` (a game function) and the pointers.txt chains (ptr, get, hold,
//     unhold, ptrreload), each of which starts at a Steam address.
//   console - SWSE's own: help, status, query, mods, ver, plugins... Run.
// Plugins' commands are the plugins' own code and run; what they reach of the
// game goes through the plugin API, which safe mode gates (plugins.cpp).
static const char* kSafeModeCats[] = {
    "player", "movement", "items", "world", "tuning", "debug", "music",
};
static const char* kSafeModeAllowed[] = {       // in a refused category, but SWSE's own
    "features",
    "hooks", "selftest", "snap", "mute", "agentdebug", "background", "remote",
    "skin", "meshdump", "hidepart", "noponcho", "nohat", "nodreads",
    "peek", "dumpaddr", "whatis", "diff", "instances", "vtscan", "findval", "narrow",
};
static const char* kSafeModeRefused[] = {       // in a category that runs, but into the game
    "call", "ptr", "get", "hold", "unhold", "ptrreload",
};
static bool NameIn(const char* name, const char* const* list, int n) {
    for (int i = 0; i < n; i++) if (!lstrcmpiA(name, list[i])) return true;
    return false;
}
#define NAMES_IN(name, list) NameIn(name, list, (int)(sizeof(list) / sizeof(list[0])))
// True when safe mode refuses this built-in (never on a known build).
static bool SafeModeBlocks(const Cmd& c) {
    if (!SWSE_GameBuildSafeMode()) return false;
    if (NAMES_IN(c.name, kSafeModeRefused)) return true;
    if (NAMES_IN(c.name, kSafeModeAllowed)) return false;
    return NAMES_IN(c.cat, kSafeModeCats);
}

// One wrapping line per category: "player  : hp  stam  sethealth ...".
// A 4-column grid pushed help past 35 lines, so the top scrolled off the panel
// and whole categories (items, and therefore grant) looked missing.
static const int HELP_WRAP = 228;      // stay inside the 240-char line buffer

// One word onto a wrapping help row.
static void HelpRowAdd(char* row, int* len, const char* word) {
    int need = lstrlenA(word) + 2;
    if (*len + need >= HELP_WRAP) {
        SWSE_ConsolePrint(row);
        wsprintfA(row, "%-10s ", "");         // continuation, aligned
        *len = lstrlenA(row);
    }
    lstrcatA(row, " ");
    lstrcatA(row, word);
    *len += need - 1;
}

static bool IsHelpCat(const char* cat) {
    for (int c = 0; c < N_CATS; c++) if (!lstrcmpiA(cat, kCats[c])) return true;
    return false;
}

// A category's row. Plugin commands (1.1) join the row of the category they
// name - their plugin's name unless they gave one - while their plugin is on.
static void HelpCategory(const char* cat, bool builtins) {
    char row[240];
    _snprintf_s(row, sizeof(row), _TRUNCATE, "%-10.31s:", cat);
    int len = lstrlenA(row);
    int n = 0;
    for (int i = 0; builtins && i < N_CMDS; i++) {
        if (lstrcmpiA(g_cmds[i].cat, cat)) continue;
        if (SafeModeBlocks(g_cmds[i])) {                  // safe mode: marked, see the legend
            char w[40];
            _snprintf_s(w, sizeof(w), _TRUNCATE, "%s*", g_cmds[i].name);
            HelpRowAdd(row, &len, w);
        } else {
            HelpRowAdd(row, &len, g_cmds[i].name);
        }
        n++;
    }
    const char *pn, *pc; int on;
    for (int i = 0; SWSE_PluginCmdAt(i, &pn, &pc, nullptr, nullptr, &on); i++) {
        if (!on || lstrcmpiA(pc, cat)) continue;
        // Two plugins offering one name: listed once (one of them runs it).
        bool dup = false;
        const char *qn, *qc; int qon;
        for (int j = 0; j < i && SWSE_PluginCmdAt(j, &qn, &qc, nullptr, nullptr, &qon); j++)
            if (qon && !lstrcmpiA(qn, pn) && !lstrcmpiA(qc, cat)) { dup = true; break; }
        if (dup) continue;
        HelpRowAdd(row, &len, pn);
        n++;
    }
    if (n) SWSE_ConsolePrint(row);
}

static int PluginCmdsOn() {
    int n = 0, on;
    for (int i = 0; SWSE_PluginCmdAt(i, nullptr, nullptr, nullptr, nullptr, &on); i++) if (on) n++;
    return n;
}

static void Cmd_help(int argc, char** argv) {
    if (argc > 1) {
        // help <command> -> one-line detail. A name can be both a command and
        // a category (`tuning`), so both are shown: returning after the
        // command made the category unreachable.
        bool shown = false;
        for (int i = 0; i < N_CMDS; i++)
            if (!lstrcmpiA(argv[1], g_cmds[i].name)) {
                Printf("%s  [%s]  %s%s", g_cmds[i].name, g_cmds[i].cat, g_cmds[i].help,
                       SafeModeBlocks(g_cmds[i]) ? "  - unavailable on this game build (safe mode)" : "");
                shown = true;
                break;
            }
        // A plugin command (1.1): every plugin that offers it, and which runs.
        const char *pn, *pc, *ph, *pp; int on;
        for (int i = 0; SWSE_PluginCmdAt(i, &pn, &pc, &ph, &pp, &on); i++) {
            if (lstrcmpiA(argv[1], pn)) continue;
            Printf("%s  [plugin %s, %s]  %s", pn, pp, on ? "on" : "off - `features <name> on`",
                   ph[0] ? ph : "(no help given)");
            shown = true;
        }
        // help <category> -> that section, with descriptions
        for (int c = 0; c < N_CATS; c++) {
            if (lstrcmpiA(argv[1], kCats[c])) continue;
            Printf("--- %s ---", kCats[c]);
            for (int i = 0; i < N_CMDS; i++)
                if (!lstrcmpiA(g_cmds[i].cat, kCats[c]))
                    Printf("  %-15s %s%s", g_cmds[i].name, g_cmds[i].help,
                           SafeModeBlocks(g_cmds[i]) ? "  (not on this game build)" : "");
            for (int i = 0; SWSE_PluginCmdAt(i, &pn, &pc, &ph, &pp, &on); i++)
                if (on && !lstrcmpiA(pc, kCats[c]))
                    Printf("  %-15s %s  (plugin %s)", pn, ph, pp);
            return;
        }
        // help <plugin> or a plugin's own category -> its commands, described.
        bool header = false;
        for (int i = 0; SWSE_PluginCmdAt(i, &pn, &pc, &ph, &pp, &on); i++) {
            if (lstrcmpiA(argv[1], pp) && lstrcmpiA(argv[1], pc)) continue;
            if (!header) { Printf("--- %s ---", argv[1]); header = true; }
            Printf("  %-15s %s%s", pn, ph, on ? "" : "  (off)");
        }
        if (header) return;
        if (!shown && SWSE_PluginIsName(argv[1])) {
            Printf("%s is a plugin with no commands registered - `plugins %s` for more", argv[1], argv[1]);
            return;
        }
        if (!shown) Printf("no such command or category: %s", argv[1]);
        return;
    }

    Printf("SWSE Console - %d commands.  'help <category>' for descriptions.", N_CMDS + PluginCmdsOn());
    for (int c = 0; c < N_CATS; c++) HelpCategory(kCats[c], true);

    // Anything with a category not in kCats still has to be reachable.
    char row[240]; wsprintfA(row, "%-10s:", "other");
    int len = lstrlenA(row), n = 0;
    for (int i = 0; i < N_CMDS; i++) {
        bool known = false;
        for (int c = 0; c < N_CATS; c++)
            if (!lstrcmpiA(g_cmds[i].cat, kCats[c])) { known = true; break; }
        if (known) continue;
        int need = lstrlenA(g_cmds[i].name) + 2;
        if (len + need >= HELP_WRAP) {
            SWSE_ConsolePrint(row); wsprintfA(row, "%-10s ", ""); len = lstrlenA(row);
        }
        lstrcatA(row, " "); lstrcatA(row, g_cmds[i].name); len += need - 1; n++;
    }
    if (n) SWSE_ConsolePrint(row);
    // Plugins (1.1): a row per category no built-in uses - normally one per
    // plugin that is on, under its own name.
    const char* seen[64]; int ns = 0;
    const char *pn, *pc; int on;
    for (int i = 0; SWSE_PluginCmdAt(i, &pn, &pc, nullptr, nullptr, &on) && ns < 64; i++) {
        if (!on || IsHelpCat(pc)) continue;
        bool dup = false;
        for (int k = 0; k < ns; k++) if (!lstrcmpiA(seen[k], pc)) dup = true;
        if (dup) continue;
        seen[ns++] = pc;
        HelpCategory(pc, false);
    }
    SWSE_ConsolePrint("also: 'list' = 181 game functions, 'scripts' = your own .txt commands");
    if (SWSE_GameBuildSafeMode())
        SWSE_ConsolePrint("* = unavailable on this game build: SWSE runs in safe mode here ('status')");
    if (SWSE_PluginsFound()) SWSE_ConsolePrint("plugins: 'plugins' lists them, 'help <plugin>' shows one's commands");
}

static bool StartsWithCI(const char* s, const char* pre) {
    while (*pre) { if ((*s | 0x20) != (*pre | 0x20)) return false; s++; pre++; }
    return true;
}

// human-readable arg hint for a game function, e.g. "<int> <int>"
static void ArgHint(const char* fmt, char* out) {
    out[0] = 0;
    for (const char* f = fmt; *f; f++) {
        const char* t = (*f == 'f') ? "<float> " : (*f == 'b') ? "<0|1> "
                      : (*f == 'e') ? "<enum#> " : "<int> ";
        lstrcatA(out, t);
    }
}

// Tab completion: complete the FIRST token against built-ins + game functions.
static void Complete() {
    if (strchr(g_input, ' ')) return;          // only complete the command name
    const char* m[300]; int n = 0;
    for (int i = 0; i < N_CMDS && n < 300; i++)
        if (StartsWithCI(g_cmds[i].name, g_input)) m[n++] = g_cmds[i].name;
    for (int i = 0; i < g_dynCmdCount && n < 300; i++)
        if (StartsWithCI(g_dynCmds[i].name, g_input)) m[n++] = g_dynCmds[i].name;
    n += SWSE_PluginCmdComplete(g_input, m + n, 300 - n);   // plugins' commands (1.1)
    n += SWSE_ScriptComplete(g_input, m + n, 300 - n);
    if (n == 0) return;
    if (n == 1) {                              // unique -> complete + space
        wsprintfA(g_input, "%s ", m[0]);
        g_inputLen = lstrlenA(g_input);
        return;
    }
    // multiple -> extend to the longest common prefix, and list the matches
    char common[128]; lstrcpynA(common, m[0], 128);
    for (int i = 1; i < n; i++) {
        int k = 0;
        while (common[k] && ((m[i][k] | 0x20) == (common[k] | 0x20))) k++;
        common[k] = 0;
    }
    if ((int)lstrlenA(common) > g_inputLen) {
        lstrcpynA(g_input, common, 250); g_inputLen = lstrlenA(g_input);
    }
    Printf("%d matches:", n);
    char row[160]; int col = 0; row[0] = 0;
    for (int i = 0; i < n && i < 60; i++) {
        char cell[30]; wsprintfA(cell, "%-20s", m[i]);
        lstrcatA(row, cell);
        if (++col == 4) { SWSE_ConsolePrint(row); row[0] = 0; col = 0; }
    }
    if (col) SWSE_ConsolePrint(row);
}

// ---- user-defined script commands -----------------------------------------
// Any .txt file in SWSEMods\SWSE Console\scripts\ becomes a new
// console command: its filename (no extension) is the command name, and each
// non-comment line is run in sequence exactly like typed input - so a script
// can chain built-ins, any of the 181 exposed game functions, freeze/poke,
// anything. This is the actual "write my own functions" extension point -
// no C++, no rebuild, just a text file.
// (g_execDepth, the recursion guard, is declared with the 1.1 forward
// declarations so `exec` and `repeat` can count into it too.)

static void GetScriptsDir(char* out) {
    char exe[MAX_PATH]; GetModuleFileNameA(GetModuleHandleA(NULL), exe, MAX_PATH);
    char* sl = strrchr(exe, '\\'); if (sl) *sl = 0;      // ...\bin
    sl = strrchr(exe, '\\'); if (sl) *sl = 0;            // game root
    if (SWSE_FindModFile("scripts", out, MAX_PATH)) return;
    wsprintfA(out, "%s\\SWSEMods\\SWSE Console\\scripts", exe);
}

static void LoadDynCmds() {
    g_dynCmdCount = 0;
    char dir[MAX_PATH]; GetScriptsDir(dir);
    char pattern[MAX_PATH]; wsprintfA(pattern, "%s\\*.txt", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (g_dynCmdCount >= 48) break;
        DynCmd& d = g_dynCmds[g_dynCmdCount];
        lstrcpynA(d.name, fd.cFileName, 32);
        char* dot = strrchr(d.name, '.'); if (dot) *dot = 0;   // strip .txt
        d.lineCount = 0;

        char path[MAX_PATH]; wsprintfA(path, "%s\\%s", dir, fd.cFileName);
        HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f == INVALID_HANDLE_VALUE) continue;
        char buf[4096]; DWORD n = 0;
        ReadFile(f, buf, sizeof(buf) - 1, &n, NULL); CloseHandle(f);
        buf[n] = 0;
        char* line = strtok(buf, "\r\n");
        while (line && d.lineCount < 24) {
            while (*line == ' ' || *line == '\t') line++;    // trim leading space
            if (*line && *line != '#') lstrcpynA(d.lines[d.lineCount++], line, 128);
            line = strtok(nullptr, "\r\n");
        }
        g_dynCmdCount++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void Execute(const char* line);   // fwd decl (scripts call back into Execute)

static void RunLines(char lines[][240], int n);   // fwd - honours `wait`

static bool RunDynCmd(const char* name) {
    for (int i = 0; i < g_dynCmdCount; i++) {
        if (lstrcmpiA(name, g_dynCmds[i].name)) continue;
        if (g_execDepth >= 8) { SWSE_ConsolePrint("script recursion too deep - aborting."); return true; }
        // Copied out so a `wait` line can defer the rest (1.1).
        char lines[24][240];
        int n = g_dynCmds[i].lineCount;
        for (int j = 0; j < n; j++) lstrcpynA(lines[j], g_dynCmds[i].lines[j], 240);
        g_execDepth++;
        RunLines(lines, n);
        g_execDepth--;
        return true;
    }
    return false;
}

static void Cmd_scripts(int, char**) {
    char dir[MAX_PATH]; GetScriptsDir(dir);
    Printf("%d custom script command(s) loaded from:", g_dynCmdCount);
    SWSE_ConsolePrint(dir);
    for (int i = 0; i < g_dynCmdCount; i++)
        Printf("  %-16s (%d lines)", g_dynCmds[i].name, g_dynCmds[i].lineCount);
    if (g_dynCmdCount == 0)
        SWSE_ConsolePrint("  (none yet - drop a .txt file there, e.g. moolah.txt "
                          "containing 'money 999999', then 'reloadscripts')");
}
static void Cmd_reloadscripts(int, char**) {
    LoadDynCmds();
    Printf("reloaded - %d custom script command(s).", g_dynCmdCount);
}

// ==========================================================================
//  REMOTE CHANNEL - drive the console from outside the game.
//
//  The DLL already lives inside the process, so it only needs a mailbox.
//  Each frame we stat remote_in.txt; when its first line (a sequence number)
//  changes, every following line is executed exactly as if typed, with all
//  console output captured and written to remote_out.txt as:
//
//      <seq>
//      ...output...
//      <<END>>
//
//  The trailing marker means a reader can tell a finished response from one
//  caught mid-write. On the first poll we only record the sequence number -
//  otherwise a stale file would replay itself on every launch.
// ==========================================================================
static bool     g_remoteOn    = true;
static int      g_remoteSeq   = -1;
static bool     g_remotePrimed = false;
static unsigned g_remoteNext  = 0;      // next tick we're allowed to poll

static void GetModDir(char* out) {
    char exe[MAX_PATH]; GetModuleFileNameA(GetModuleHandleA(NULL), exe, MAX_PATH);
    char* sl = strrchr(exe, '\\'); if (sl) *sl = 0;      // ...\bin
    sl = strrchr(exe, '\\'); if (sl) *sl = 0;            // game root
    wsprintfA(out, "%s\\SWSEMods\\SWSE Console", exe);
}

static void RemoteWriteOut(int seq, const char* body) {
    char path[MAX_PATH], dir[MAX_PATH];
    GetModDir(dir); wsprintfA(path, "%s\\remote_out.txt", dir);
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char hdr[32]; wsprintfA(hdr, "%d\r\n", seq);
    DWORD w;
    WriteFile(h, hdr, lstrlenA(hdr), &w, nullptr);
    WriteFile(h, body, lstrlenA(body), &w, nullptr);
    WriteFile(h, "<<END>>\r\n", 9, &w, nullptr);
    CloseHandle(h);
}

static void RemotePoll() {
    if (!g_remoteOn) return;
    unsigned now = GetTickCount();
    if (now < g_remoteNext) return;
    g_remoteNext = now + 120;                    // ~8 checks/sec is plenty

    char path[MAX_PATH], dir[MAX_PATH];
    GetModDir(dir); wsprintfA(path, "%s\\remote_in.txt", dir);
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        // No mailbox at launch means there is no stale command to skip, so we
        // are primed already and the next command sent can run immediately.
        g_remotePrimed = true;
        return;
    }
    // The write time as well as the sequence number: a tool that starts its
    // numbering at 1 every session (Stranger: Armed to the Teeth does) would
    // otherwise have its first command dropped whenever a stale file from the
    // last session happened to carry the same number.
    FILETIME wt = { 0, 0 };
    GetFileTime(h, nullptr, nullptr, &wt);
    static char buf[65536]; DWORD got = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(h);
    if (!got) return;
    buf[got] = 0;

    int seq = atoi(buf);
    ULONGLONG t = ((ULONGLONG)wt.dwHighDateTime << 32) | wt.dwLowDateTime;
    static ULONGLONG s_lastTime = 0;
    // Priming must consume whatever was already on disk at launch - otherwise
    // a leftover file replays itself every time the game starts. Doing it here
    // (rather than on the first *new* seq) means it costs a poll, not a command.
    if (!g_remotePrimed) { g_remotePrimed = true; g_remoteSeq = seq; s_lastTime = t; return; }
    if (seq == g_remoteSeq && t == s_lastTime) return;   // nothing new
    // A batch caught mid-write would run in part now and, once the write time
    // settled, again in full - doubling anything not idempotent. Both known
    // writers (swsecmd.ps1, Stranger: Armed to the Teeth) end the file with a
    // newline, so a file without one is taken as still being written - unless
    // it sits unchanged for a poll, for a writer that leaves the last one off.
    static ULONGLONG s_pendTime = 0;
    static DWORD     s_pendSize = 0;
    if (buf[got - 1] != '\n' && (t != s_pendTime || got != s_pendSize)) {
        s_pendTime = t;
        s_pendSize = got;
        return;
    }
    s_pendTime = 0;
    s_pendSize = 0;
    g_remoteSeq = seq;
    s_lastTime = t;

    const char* p = strchr(buf, '\n');
    if (!p) { RemoteWriteOut(seq, "(no command lines)\r\n"); return; }
    p++;

    // Collected first and run as a sequence, so a `wait <ms>` line delays the
    // lines after it (their output then goes to the console, not this reply).
    // 256 lines of up to 239 characters (what a command line holds); anything
    // beyond is reported in the reply rather than dropped silently.
    static char lines[256][240];
    int n = 0, over = 0, cut = 0;
    char line[256]; int li = 0; bool longLine = false;
    for (;; p++) {
        if (*p == '\r') continue;
        if (*p == '\n' || *p == 0) {
            line[li] = 0;
            if (li && line[0] != '#') {
                if (n < 256) { lstrcpynA(lines[n++], line, 240); if (longLine || li > 239) cut++; }
                else over++;
            }
            li = 0; longLine = false;
            if (*p == 0) break;
            continue;
        }
        if (li < (int)sizeof(line) - 1) line[li++] = *p;
        else longLine = true;
    }
    g_capLen = 0; g_capBuf[0] = 0; g_capOn = true;
    if (over) Printf("(mailbox: %d line(s) past the first 256 were not run)", over);
    if (cut)  Printf("(mailbox: %d line(s) were cut to 239 characters)", cut);
    RunLines(lines, n);
    g_capOn = false;
    RemoteWriteOut(seq, g_capLen ? g_capBuf : "(no output)\r\n");
}

static void Cmd_remote(int argc, char** argv) {
    if (argc > 1 && !lstrcmpiA(argv[1], "off")) { g_remoteOn = false; SWSE_ConsolePrint("remote channel OFF"); return; }
    if (argc > 1 && !lstrcmpiA(argv[1], "on"))  { g_remoteOn = true;  SWSE_ConsolePrint("remote channel ON"); return; }
    char dir[MAX_PATH]; GetModDir(dir);
    Printf("remote channel %s (seq %d)", g_remoteOn ? "ON" : "OFF", g_remoteSeq);
    Printf("mailbox: %s\\remote_in.txt", dir);
}

static bool IsBuiltinCmd(const char* name) {
    for (int i = 0; i < N_CMDS; i++)
        if (!lstrcmpiA(name, g_cmds[i].name)) return true;
    return false;
}

// Run a list of command lines in order. `wait <ms>` delays everything after it
// by handing the rest to the scheduler, so a sequence never blocks a frame.
static void RunLines(char lines[][240], int n) {
    DWORD delay = 0;
    for (int i = 0; i < n; i++) {
        const char* l = lines[i];
        while (*l == ' ' || *l == '\t') l++;
        if (!*l) continue;
        if (!_strnicmp(l, "wait", 4) && (l[4] == ' ' || l[4] == '\t' || !l[4])) {
            int ms = atoi(l + 4);
            if (ms > 0) delay += (DWORD)ms;
            continue;
        }
        if (delay) Schedule(l, delay);
        else Execute(l);
    }
}

// "a; b; wait 300; c" -> a sequence. A ';' inside the line splits it; there is
// no quoting, which no built-in command needs.
static bool RunChain(const char* line) {
    if (!strchr(line, ';')) return false;
    // Commands that take a command keep the whole rest of the line, ';'s and
    // all: `alias x a; b` defines x as "a; b", `after 5 a; b` delays both.
    // The stored line is split when it finally runs.
    {
        const char* w = line;
        while (*w == ' ' || *w == '\t') w++;
        static const char* kTakesCommand[] = { "alias", "bind", "after", "repeat" };
        for (int i = 0; i < 4; i++) {
            int n = lstrlenA(kTakesCommand[i]);
            if (!_strnicmp(w, kTakesCommand[i], n) && (w[n] == ' ' || w[n] == '\t'))
                return false;
        }
    }
    // A local copy per call: a part may itself run a chain (an alias).
    char local[32][240];
    int n = 0;
    const char* p = line;
    while (*p && n < 32) {
        const char* e = strchr(p, ';');
        int len = e ? (int)(e - p) : lstrlenA(p);
        if (len > 239) len = 239;
        memcpy(local[n], p, len); local[n][len] = 0;
        n++;
        if (!e) break;
        p = e + 1;
    }
    RunLines(local, n);
    return true;
}

// Commands one typed (or mailed, bound, scheduled) line may run in total,
// counting everything its repeats, aliases and scripts expand into. The depth
// limit alone still allowed 50^7 from a self-repeating alias.
#define LINE_BUDGET 2000
static int  g_lineBudget = LINE_BUDGET;
static bool g_budgetWarned = false;

static void Execute(const char* line) {
    if (g_execDepth >= 8) { SWSE_ConsolePrint("command nesting too deep - aborting."); return; }
    if (g_execDepth == 0) { g_lineBudget = LINE_BUDGET; g_budgetWarned = false; }
    if (--g_lineBudget < 0) {
        if (!g_budgetWarned) {
            g_budgetWarned = true;
            Printf("one line expanded past %d commands - the rest were skipped", LINE_BUDGET);
        }
        return;
    }
    if (RunChain(line)) return;
    g_cmdsRun++;                    // retires the big welcome title
    Printf("> %s", line);
    char buf[256]; lstrcpynA(buf, line, 256);
    // 32 tokens: alias/bind/after bodies are whole command lines.
    char* argv[32]; int argc = 0;
    char* tok = strtok(buf, " \t");
    while (tok && argc < 32) { argv[argc++] = tok; tok = strtok(nullptr, " \t"); }
    if (argc == 0) return;
    for (int i = 0; i < N_CMDS; i++) {
        if (!lstrcmpiA(argv[0], g_cmds[i].name)) {
            // Safe mode's one gate for every way a command arrives: typed,
            // mailed, bound, aliased, scheduled, from a script or a plugin.
            if (SafeModeBlocks(g_cmds[i])) { SafeModeRefuse(g_cmds[i].name); return; }
            g_cmds[i].fn(argc, argv);
            return;
        }
    }
    // A native plugin's command (1.1): after the built-ins, before scripts,
    // aliases and the game's functions. Guarded and timed in plugins.cpp.
    if (SWSE_PluginCmdRun(argc, argv)) return;
    if (RunDynCmd(argv[0])) return;      // user-defined .txt script command
    if (const Alias* a = FindAlias(argv[0])) {
        char expanded[480];
        ExpandArgs(a->body, argc, argv, expanded, sizeof(expanded));
        g_execDepth++;
        Execute(expanded);
        g_execDepth--;
        return;
    }
    // not a built-in - try the 181 auto-exposed game functions by real name
    const char* fmt = SWSE_ScriptArgs(argv[0]);
    // Each one is a game function: none is called in safe mode.
    if (fmt && SWSE_GameBuildSafeMode()) { SafeModeRefuse(argv[0]); return; }
    if (fmt && *fmt && argc == 1) {           // needs args but none given -> show usage
        char hint[64]; ArgHint(fmt, hint);
        Printf("%s needs args:  %s %s", argv[0], argv[0], hint);
        return;
    }
    int r = SWSE_ScriptCallByName(argv[0], argc, argv);
    if (r == 1)       Printf("%s: called", argv[0]);
    else if (r == 0)  NoContext(argv[0], true);
    else if (r == -2) Printf("%s: faulted - try different args", argv[0]);
    else if (r == -3) SafeModeRefuse(argv[0]);
    else {
        // near-miss suggestions via prefix
        const char* m[8]; int n = SWSE_ScriptComplete(argv[0], m, 8);
        if (n) { char b[160]; wsprintfA(b, "unknown '%s'. did you mean: ", argv[0]);
                 for (int i=0;i<n;i++){lstrcatA(b,m[i]);lstrcatA(b," ");} SWSE_ConsolePrint(b); }
        else Printf("unknown: %s  (type 'help', or 'list' for game functions)", argv[0]);
    }
}

void SWSE_ConsoleExec(const char* line) {
    if (line && *line) Execute(line);
}

// ---- for the plugin API (plugins.cpp) -------------------------------------------
bool SWSE_ConsoleIsBuiltin(const char* name) { return name && IsBuiltinCmd(name); }

// Execute counts nesting only for scripts, aliases, `exec` and `repeat`; a
// command that runs a command was not counted, so a plugin command running
// itself would have recursed until the stack died (PLUGIN_SYSTEM.md C5). The
// plugin API's Execute comes through here and adds its level. A line run from
// a plugin's frame callback starts a fresh line budget, as a typed line does.
int SWSE_ConsoleExecNested(const char* line) {
    if (!line || !*line) return 0;
    // Execute refuses at depth 8 on its own, silently to its caller; refuse the
    // level that would reach it here, so the plugin gets SWSE_E_NESTING back.
    if (g_execDepth >= 7) { SWSE_ConsolePrint("command nesting too deep - aborting."); return -11; }
    if (g_execDepth == 0) { g_lineBudget = LINE_BUDGET; g_budgetWarned = false; }
    g_execDepth++;
    Execute(line);
    g_execDepth--;
    return 0;
}


// ---- font atlas build (GDI -> GL texture) --------------------------------
static void BuildFont() {
    HDC dc = CreateCompatibleDC(NULL);
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = ATLAS_W;
    bi.bmiHeader.biHeight = -ATLAS_H;        // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HGDIOBJ oldbmp = SelectObject(dc, dib);

    HFONT font = CreateFontA(CELL_H, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    HGDIOBJ oldfont = SelectObject(dc, font);
    SetBkColor(dc, RGB(0, 0, 0));
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, OPAQUE);
    RECT full = {0, 0, ATLAS_W, ATLAS_H};
    FillRect(dc, &full, (HBRUSH)GetStockObject(BLACK_BRUSH));

    for (int i = 0; i < ATLAS_COLS * ATLAS_ROWS; i++) {
        char ch = (char)(0x20 + i);
        int cx = (i % ATLAS_COLS) * CELL_W;
        int cy = (i / ATLAS_COLS) * CELL_H;
        RECT r = { cx, cy, cx + CELL_W, cy + CELL_H };
        DrawTextA(dc, &ch, 1, &r, DT_LEFT | DT_TOP | DT_NOPREFIX);
    }
    GdiFlush();

    // convert BGRA DIB -> RGBA where alpha = luminance (so text blends cleanly)
    unsigned char* px = (unsigned char*)bits;
    int npx = ATLAS_W * ATLAS_H;
    unsigned char* rgba = (unsigned char*)malloc(npx * 4);
    for (int i = 0; i < npx; i++) {
        unsigned char b = px[i*4+0], g = px[i*4+1], r = px[i*4+2];
        unsigned char lum = (unsigned char)((r*30 + g*59 + b*11) / 100);
        rgba[i*4+0] = 210; rgba[i*4+1] = 255; rgba[i*4+2] = 190; rgba[i*4+3] = lum;
    }
    glGenTextures(1, &g_font);
    glBindTexture(GL_TEXTURE_2D, g_font);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ATLAS_W, ATLAS_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    free(rgba);

    SelectObject(dc, oldfont); DeleteObject(font);
    SelectObject(dc, oldbmp);  DeleteObject(dib);
    DeleteDC(dc);
}

// ---- text drawing --------------------------------------------------------
static void DrawText(float x, float y, float scale, const char* s) {
    float gw = CELL_W * scale, gh = CELL_H * scale;
    glBindTexture(GL_TEXTURE_2D, g_font);
    glBegin(GL_QUADS);
    for (const char* p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c > 0x7F) c = '?';
        int idx = c - 0x20;
        float u0 = (float)((idx % ATLAS_COLS) * CELL_W) / ATLAS_W;
        float v0 = (float)((idx / ATLAS_COLS) * CELL_H) / ATLAS_H;
        float u1 = u0 + (float)CELL_W / ATLAS_W;
        float v1 = v0 + (float)CELL_H / ATLAS_H;
        glTexCoord2f(u0, v0); glVertex2f(x, y);
        glTexCoord2f(u1, v0); glVertex2f(x + gw, y);
        glTexCoord2f(u1, v1); glVertex2f(x + gw, y + gh);
        glTexCoord2f(u0, v1); glVertex2f(x, y + gh);
        x += gw;
    }
    glEnd();
}

// ---- input ---------------------------------------------------------------
static void HandleInput() {
    // Only the game's own keyboard. GetAsyncKeyState is the GLOBAL key state,
    // so without this, while the console was open, whatever the user typed in
    // ANY app went into the console line and Enter ran it - and the ~ toggle
    // fired from other apps too (plugin QA, 2026-09-28). While the game is not
    // the foreground window every key counts as already held, so a key held
    // across the switch back produces no press.
    if (!SWSE_InputReallyFocused()) {
        memset(g_prevKey, 1, sizeof(g_prevKey));
        return;
    }
    BYTE ks[256] = {0};
    if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) ks[VK_SHIFT]   = 0x80;
    if (GetKeyState(VK_CAPITAL)      & 0x0001) ks[VK_CAPITAL] = 0x01;

    // From 4 (middle mouse, then X1/X2) so those can be bound; they translate
    // to no character, so typing is unaffected. 1 and 2 (left/right click)
    // are deliberately never scanned.
    for (int vk = 4; vk < 256; vk++) {
        if (vk == 7) continue;                 // unassigned
        bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        bool edge = down && !g_prevKey[vk];
        g_prevKey[vk] = down;
        if (!edge) continue;

        if (vk == VK_OEM_3) {                 // ` / ~  toggles the console
            SetOpen(!g_open);
            g_inputLen = 0; g_input[0] = 0;
            continue;
        }
        if (!g_open) { RunBind(vk); continue; }   // key binds (1.1)

        if (vk == VK_ESCAPE) { SetOpen(false); continue; }
        if (vk == VK_RETURN) {
            if (g_inputLen) { Execute(g_input); g_inputLen = 0; g_input[0] = 0; g_scroll = 0; }
            continue;
        }
        if (vk == VK_BACK) {
            if (g_inputLen) g_input[--g_inputLen] = 0;
            continue;
        }
        if (vk == VK_TAB) { Complete(); continue; }         // autocomplete
        if (vk == VK_PRIOR) { g_scroll += 3; continue; }   // PageUp
        if (vk == VK_NEXT)  { g_scroll = (g_scroll > 3) ? g_scroll - 3 : 0; continue; }

        // translate to a printable char (honours shift/caps)
        UINT scan = MapVirtualKeyA(vk, 0);
        WORD out = 0;
        if (ToAscii(vk, scan, ks, &out, 0) == 1) {
            char c = (char)(out & 0xFF);
            if (c == '`' || c == '~') continue;             // never type the toggle key
            if (c >= 0x20 && c < 0x7F && g_inputLen < 250) {
                g_input[g_inputLen++] = c; g_input[g_inputLen] = 0;
            }
        }
    }
}

// ---- per-frame entry -----------------------------------------------------
static void RenderConsole(int w, int h) {
    int panelH = (int)(h * 0.45f);
    float scale = (h > 1200) ? 2.0f : 1.0f;
    float lineH = CELL_H * scale + 2.0f;

    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
    glOrtho(0, w, h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();

    if (p_useProgram) p_useProgram(0);
    glDisable(GL_VERTEX_PROGRAM_ARB);
    glDisable(GL_FRAGMENT_PROGRAM_ARB);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glDisable(GL_ALPHA_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // background panel
    glDisable(GL_TEXTURE_2D);
    glColor4f(0.04f, 0.05f, 0.04f, 0.86f);
    glBegin(GL_QUADS);
        glVertex2f(0, 0); glVertex2f((float)w, 0);
        glVertex2f((float)w, (float)panelH); glVertex2f(0, (float)panelH);
    glEnd();
    // accent line
    glColor4f(0.84f, 0.66f, 0.33f, 1.0f);
    glBegin(GL_QUADS);
        glVertex2f(0, (float)panelH - 2); glVertex2f((float)w, (float)panelH - 2);
        glVertex2f((float)w, (float)panelH); glVertex2f(0, (float)panelH);
    glEnd();

    // text
    glEnable(GL_TEXTURE_2D);
    glColor4f(1, 1, 1, 1);
    // input line at the bottom of the panel
    char prompt[300];
    wsprintfA(prompt, "> %s_", g_input);
    DrawText(6, panelH - lineH - 4, scale, prompt);

    // scrollback above the input, newest at bottom
    int visible = (int)((panelH - lineH - 10) / lineH);
    int last = g_logCount - 1 - g_scroll;
    float y = panelH - lineH * 2 - 6;
    for (int i = 0; i < visible && last - i >= 0; i++) {
        DrawText(6, y, scale, g_log[last - i]);
        y -= lineH;
    }

    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(GL_MODELVIEW); glPopMatrix();
    glPopAttrib();
}

static void FrameProtectedBody(HDC hdc) {
    if (!g_inited) {
        g_inited = true;
        HMODULE gl = GetModuleHandleA("opengl32.dll");
        typedef PROC(WINAPI* gpa_t)(LPCSTR);
        gpa_t gpa = (gpa_t)GetProcAddress(gl, "wglGetProcAddress");
        if (gpa) {
            p_useProgram = (glUseProgram_t)gpa("glUseProgram");
            if (!p_useProgram) p_useProgram = (glUseProgram_t)gpa("glUseProgramObjectARB");
        }
        BuildFont();
        SWSE_ScriptVMInit();
        LoadDynCmds();
        LoadBinds();
        LoadAliases();
        SWSE_PtrLoad();
        // Deliberately plain. The font atlas is ASCII only, so decorative
        // separators drew as '?', and four dense lines buried the one thing a
        // new user needs to know.
        SWSE_ConsolePrint("SWSE Console " SWSE_VERSION);
        SWSE_ConsolePrint("type 'help' for commands, 'features' for what is switched on");
        if (SWSE_GameBuildSafeMode())
            SWSE_ConsolePrint("safe mode: this game build is not the one SWSE was measured on - 'status'");
    }
    SWSE_ScriptTick();        // apply god mode each frame (no-op unless enabled)
    SchedTick();              // `after` / `wait` commands that have come due
    HandleInput();
    if (!g_open) return;
    HWND hwnd = WindowFromDC(hdc);
    RECT rc;
    if (!hwnd || !GetClientRect(hwnd, &rc)) return;
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w > 0 && h > 0) RenderConsole(w, h);
}

void SWSE_ConsoleFrame(HDC hdc) {
    // Remote commands run in their own SEH block: a faulting experiment should
    // cost us the response, not the game.
    __try { RemotePoll(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_capOn = false;
        RemoteWriteOut(g_remoteSeq, "*** FAULTED - command crashed (game survived) ***\r\n");
    }
    // Raid mode re-targets raiders periodically. In its own SEH block so a bad
    // frame costs the raid rather than the game.
    __try { SWSE_RaidTick(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try { SWSE_DecoyTick(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    // Synthetic input: installs the keyboard probes on the first frame and
    // pushes queued presses onto the window-message channel.
    __try { SWSE_InputTick(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    // Additive hit reactions: notices NPC health drops and queues a flinch.
    // Inert unless 'hitreact watch on'.
    __try { SWSE_HitReactTick(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try { TpCheckTick(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    SWSE_CountFrame();
    __try { FrameProtectedBody(hdc); }
    __except (EXCEPTION_EXECUTE_HANDLER) { SetOpen(false); }
}

