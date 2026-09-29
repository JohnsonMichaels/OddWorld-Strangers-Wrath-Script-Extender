// SWSE frame hook (M2). Inline-hooks wglSwapBuffers with a minimal
// unhook -> call -> rehook pattern (safe for a single-threaded render loop).
// Each frame, before the game presents, we draw a small marker quad in the
// corner to PROVE SWSE controls the frame. No game state is disturbed:
// glPushAttrib/glPushMatrix save and restore everything we touch.
//
// M3 will replace the marker with FBO capture + the graphics-mod shader passes.

#include "framehook.h"
#include "gfx.h"
#include "glspy.h"
#include "console.h"
#include "shaderspy.h"
#include "foliage.h"
#include "materials.h"
#include "wind.h"
#include "granny.h"
#include "input.h"
#include "scriptvm.h"
#include "selftest.h"
#include "aitune.h"
#include "triggers.h"
#include "positions.h"
#include "features.h"
#include "levelwatch.h"
#include "playertune.h"
#include "prefsedit.h"
#include "playnpc.h"
#include "freecam.h"
#include "gamebuild.h"
#include "mute.h"
#include "plugins.h"
#include "hookreg.h"
#include <gl/GL.h>
#include <string>
#include <fstream>

#pragma comment(lib, "opengl32.lib")

typedef BOOL(WINAPI* wglSwapBuffers_t)(HDC);
typedef void (WINAPI* wglGetProcAddress_t)(const char*);
typedef void (APIENTRY* glUseProgram_t)(GLuint);

static wglSwapBuffers_t g_realSwap = nullptr;
static glUseProgram_t   g_glUseProgram = nullptr;      // GLSL unbind
static BYTE  g_origBytes[5];
static bool  g_hooked = false;
static bool  g_loggedFirstFrame = false;

// ARB program enables (not in gl/GL.h's 1.1 subset)
#ifndef GL_VERTEX_PROGRAM_ARB
#define GL_VERTEX_PROGRAM_ARB   0x8620
#define GL_FRAGMENT_PROGRAM_ARB 0x8804
#endif

static void LogFH(const std::string& s) {
    char path[MAX_PATH];
    GetModuleFileNameA(GetModuleHandleA(NULL), path, MAX_PATH);
    std::string p(path);
    std::ofstream f(p.substr(0, p.find_last_of("\\/")) + "\\swse_log.txt", std::ios::app);
    f << s << "\n";
}

// --- minimal x86 inline hook helpers -------------------------------------
static void WriteJump(void* target, void* dest) {
    DWORD old;
    VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old);
    BYTE* t = (BYTE*)target;
    t[0] = 0xE9;  // JMP rel32
    *(DWORD*)(t + 1) = (DWORD)((BYTE*)dest - (t + 5));
    VirtualProtect(target, 5, old, &old);
}

static void RestoreBytes(void* target) {
    DWORD old;
    VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy(target, g_origBytes, 5);
    VirtualProtect(target, 5, old, &old);
}

// --- our per-frame work ---------------------------------------------------
static void DrawMarker() {
    GLint vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    int w = vp[2], h = vp[3];
    if (w <= 0 || h <= 0) return;

    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
    glOrtho(0, w, h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);  glPushMatrix(); glLoadIdentity();

    // Unbind the game's Cg/GLSL shaders so fixed-function color actually shows.
    // (Without this the bound shader decides the fragment color -> our quad is black.)
    if (g_glUseProgram) g_glUseProgram(0);
    glDisable(GL_VERTEX_PROGRAM_ARB);
    glDisable(GL_FRAGMENT_PROGRAM_ARB);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_LIGHTING);
    glDisable(GL_BLEND);

    // SWSE-gold square, top-left - proof we own the frame + color.
    // glOrtho above puts (0,0) at top-left; draw a clearly on-screen box.
    glColor3f(0.84f, 0.66f, 0.33f);
    glBegin(GL_QUADS);
        glVertex2f(12, 12); glVertex2f(64, 12);
        glVertex2f(64, 64); glVertex2f(12, 64);
    glEnd();

    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(GL_MODELVIEW);  glPopMatrix();
    glPopAttrib();
}

// A start that is refused must not leave the switch reading on: `features`,
// `query features`, `status` and the self-test would all report a system that
// is not running. So SWSE_FeatureStart drops the in-memory flag itself - at
// launch, where the loop below ignores its answer, as well as live - and the
// log says why. features.txt keeps its line, so the next launch tries again.
static bool RefuseStart(SwseFeature f, const char* why, char* msg, int msgLen) {
    SWSE_FeatureSetFlag(f, false);
    LogFH(std::string("feature refused: ") + SWSE_FeatureName(f) + " - " + why +
          " (off for this session; features.txt unchanged)");
    if (msg) lstrcpynA(msg, why, msgLen);
    return false;
}

// --- starting and stopping systems ----------------------------------------
// One place that knows how to bring each system up and down, used both at
// launch (for whatever features.txt switches on) and live from the console
// (`features <name> on|off`). Runs on the render thread with a GL context
// current: the frame hook calls it, and so does every console command.
static bool g_startedOnce[FEAT_COUNT];

// Safe mode (gamebuild.h): the systems that act on the game. hitreact and
// npctuning ride hooks in game code; aituning, playertune, prefsedit and
// triggers find and change game objects through Steam addresses, once per
// level of a level watcher that does not run there. What stays: the console,
// graphics, hdtextures, foliage and raytrace, which work on OpenGL - and
// plugins, which reach the game only through SWSE's own gated calls.
const char* SWSE_FeatureSafeModeRefusal(SwseFeature f) {
    switch (f) {
    case FEAT_HITREACT:   return "hit reactions";
    case FEAT_NPCTUNING:  return "character tuning (it applies from the NPC spawn hook)";
    case FEAT_AITUNING:   return "AI tuning";
    case FEAT_TRIGGERS:   return "triggers";
    case FEAT_PLAYERTUNE: return "player tuning";
    case FEAT_PREFSEDIT:  return "the prefs editor";
    default:              return nullptr;
    }
}

bool SWSE_FeatureStart(SwseFeature f, char* msg, int msgLen) {
    char m[300] = { 0 };
    // A plugin's switch (1.1): the first switch-on loads its DLL - Query, Load
    // - then its onEnable runs (plugins.cpp). False leaves the switch off.
    if (SWSE_FeatureIsPlugin(f)) {
        bool ok = SWSE_PluginFeatureStart(f, m, sizeof(m));
        LogFH(std::string("feature start: ") + SWSE_FeatureName(f) + (ok ? " - " : " FAILED - ") + m);
        if (msg) lstrcpynA(msg, m, msgLen);
        return ok;
    }
    // Safe mode: the systems that act on the game refuse to come on (see
    // SWSE_FeatureSafeModeRefusal) - refused rather than left idle, so no
    // switch reads on while nothing runs (RefuseStart). The hooks the two
    // hook-riding ones would have put in are listed as refused (`hooks`).
    if (const char* what = SWSE_GameBuildSafeMode() ? SWSE_FeatureSafeModeRefusal(f) : nullptr) {
        if (f == FEAT_HITREACT) {
            SWSE_HookRefused("granny", "BuildWorldPose");
            SWSE_HookRefused("granny", "Bolt::vfunc17");
        } else if (f == FEAT_NPCTUNING) {
            SWSE_HookRefused("scriptvm", "SpawnNPCFromTag");
        }
        SWSE_GameBuildRefusal(what, m, sizeof(m));
        return RefuseStart(f, m, msg, msgLen);
    }
    switch (f) {
    case FEAT_CONSOLE:
        lstrcpynA(m, "console is on", sizeof(m));
        break;
    case FEAT_GRAPHICS:
        if (!SWSE_GfxReady()) SWSE_GfxInit();
        // The SSR mask's reflective fingerprints (materials.txt). At start,
        // not only at launch, so a live `features graphics on` has them too.
        SWSE_MaterialsInit();
        // glspy carries BOTH the FBO tracking the graphics pipeline needs and
        // the texture-upload hook HD replacement rides on.
        SWSE_InstallGLSpy();
        lstrcpynA(m, SWSE_GfxReady()
                     ? "graphics pipeline loaded - F10 (or `gfx on`) shows the effect"
                     : "graphics pipeline FAILED to initialise (see swse_log.txt)",
                  sizeof(m));
        break;
    case FEAT_HDTEXTURES: {
        SWSE_InstallGLSpy();
        SWSE_HdPrepare();
        int avail = 0, loaded = 0, failed = 0;
        SWSE_HdStats(&avail, &loaded, &failed);
        wsprintfA(m, "HD textures on - %d replacement(s) installed; they apply as "
                     "textures load (warp or reload to see a whole level)", avail);
        break;
    }
    case FEAT_HITREACT:
        // (Refused in safe mode, above: its pose and bolt hooks patch game
        // code at Steam-measured addresses, which granny.cpp refuses too.)
        // hitreact.txt decides whether the reactions themselves come up on.
        SWSE_HitReactLoadSettings();
        lstrcpynA(m, SWSE_HitReactEnabled()
                     ? "hit reactions on"
                     : "hit reactions loaded, but hitreact.txt has enabled 0 - `hitreact on`",
                  sizeof(m));
        break;
    case FEAT_FOLIAGE:
        // Plants are recognised by fingerprinting textures AS THEY UPLOAD, in
        // glspy's upload hook. 1.0.x only installed glspy for graphics or HD,
        // so foliage on its own never saw a plant - hidden while every feature
        // defaulted on.
        SWSE_InstallGLSpy();
        SWSE_FoliageInit();          // the foliage fingerprint list
        SWSE_WindLoadSettings();     // may enable wind + the foliage tracker
        lstrcpynA(m, SWSE_LevelUp()
                     ? "foliage on - plants are recognised as a level loads, so it "
                       "starts with the next level (warp or load a save)"
                     : "foliage on - wind follows wind.txt (`wind` for status)", sizeof(m));
        break;
    case FEAT_AITUNING: {
        // Only the PROFILES are read here; the `active` one is applied by the
        // tick once a level is up (now, if one already is).
        SWSE_AiTuneLoad(m, sizeof(m));
        SWSE_AiTuneKick();
        break;
    }
    case FEAT_TRIGGERS: {
        int n = SWSE_TriggersLoad();
        SWSE_TriggersEnable(1);
        SWSE_TriggersArm();          // mid-level: no burst of levelload triggers
        wsprintfA(m, "triggers on - %d loaded", n);
        break;
    }
    case FEAT_NPCTUNING:
        // (Refused in safe mode, above: its rules apply from the NPC spawn
        // hook, a code patch scriptvm.cpp refuses on an unknown build.)
        SWSE_NpcTuningLoadAll(m, sizeof(m));
        break;
    case FEAT_PLAYERTUNE: {
        char lm[300];
        SWSE_PlayerTuneLoad(lm, sizeof(lm));
        if (SWSE_LevelUp()) SWSE_PlayerTuneApply(m, sizeof(m));
        else lstrcpynA(m, lm, sizeof(m));
        break;
    }
    case FEAT_PREFSEDIT:
        SWSE_PrefsEditLoad(m, sizeof(m));
        SWSE_PrefsEditKick();
        break;
    case FEAT_RAYTRACE:
        // The tracer runs inside the graphics pipeline (it reads the scene
        // depth the pipeline captures), so it cannot stand alone. Refusing
        // is clearer than switching graphics on behind the user's back. At
        // launch this used to leave `raytrace = on` reading on (the loop
        // ignored the answer); RefuseStart drops the flag.
        if (!SWSE_Feature(FEAT_GRAPHICS))
            return RefuseStart(f, "ray tracing needs the graphics pipeline - `features graphics on` first",
                               msg, msgLen);
        if (!SWSE_GfxReady())
            return RefuseStart(f, "ray tracing needs the graphics pipeline, which failed to initialise "
                                  "(see swse_log.txt)", msg, msgLen);
        lstrcpynA(m, "ray tracing on (EXPERIMENTAL) - RTAO follows rtao_enable in graphics.txt; "
                     "`rt` shows the BVH", sizeof(m));
        break;
    default:
        lstrcpynA(m, "unknown feature", sizeof(m));
        if (msg) lstrcpynA(msg, m, msgLen);
        return false;
    }
    if (f >= 0 && f < FEAT_COUNT) g_startedOnce[f] = true;
    LogFH(std::string("feature start: ") + SWSE_FeatureName(f) + " - " + m);
    if (msg) lstrcpynA(msg, m, msgLen);
    return true;
}

// `auto` (features.h): does this system's own file ask for anything?
// aituning: an `active = <profile>` line; playertune: any value at all.
static bool AutoWanted(SwseFeature f, char* why, int whyLen) {
    char m[300];
    if (f == FEAT_AITUNING) {
        SWSE_AiTuneLoad(m, sizeof(m));
        const char* want = SWSE_AiTuneWanted();
        if (want && *want) { _snprintf_s(why, whyLen, _TRUNCATE, "aiprefs.txt asks for '%s'", want); return true; }
        lstrcpynA(why, "aiprefs.txt says active = off", whyLen);
        return false;
    }
    if (f == FEAT_PLAYERTUNE) {
        SWSE_PlayerTuneLoad(m, sizeof(m));
        int values = 0, here = 0;
        SWSE_PlayerTuneStats(&values, &here);
        if (values > 0) { _snprintf_s(why, whyLen, _TRUNCATE, "playerprefs.txt sets %d value(s)", values); return true; }
        lstrcpynA(why, "no playerprefs.txt values", whyLen);
        return false;
    }
    lstrcpynA(why, "not an auto switch", whyLen);
    return false;
}

bool SWSE_FeatureAutoEvaluate(SwseFeature f, char* msg, int msgLen) {
    char why[160], m[300] = { 0 };
    bool want = AutoWanted(f, why, sizeof(why));
    bool on = SWSE_Feature(f);
    if (want && !on) {
        SWSE_FeatureSetFlag(f, true);
        if (!SWSE_FeatureStart(f, m, sizeof(m))) SWSE_FeatureSetFlag(f, false);
    } else if (!want && on) {
        if (SWSE_FeatureStop(f, m, sizeof(m))) SWSE_FeatureSetFlag(f, false);
    }
    char t[400];
    _snprintf_s(t, sizeof(t), _TRUNCATE, "%s auto: %s (%s)%s%s", SWSE_FeatureName(f),
                SWSE_Feature(f) ? "ON" : "off", why, m[0] ? " - " : "", m);
    LogFH(t);
    if (msg) lstrcpynA(msg, t, msgLen);
    return SWSE_Feature(f);
}

bool SWSE_FeatureStop(SwseFeature f, char* msg, int msgLen) {
    char m[300] = { 0 };
    // A plugin: its onDisable. The DLL stays loaded (plugins are never
    // unloaded - PLUGIN_SYSTEM.md 6.5); with the switch off its callbacks stop.
    if (SWSE_FeatureIsPlugin(f)) {
        bool ok = SWSE_PluginFeatureStop(f, m, sizeof(m));
        LogFH(std::string("feature stop: ") + SWSE_FeatureName(f) + " - " + m);
        if (msg) lstrcpynA(msg, m, msgLen);
        return ok;
    }
    switch (f) {
    case FEAT_CONSOLE:
        lstrcpynA(m, "the console cannot switch itself off live - "
                     "set console = off in features.txt", sizeof(m));
        if (msg) lstrcpynA(msg, m, msgLen);
        return false;
    case FEAT_GRAPHICS:
        SWSE_GfxSetEnabled(0);
        // Ray tracing runs inside the pipeline (FeatureStart refuses it without
        // graphics), so it goes off too: for the session, as a refused start
        // does. features.txt keeps its line - a launch with graphics on brings
        // it back. Left on, it read "on" in `features` with nothing tracing.
        if (SWSE_Feature(FEAT_RAYTRACE)) {
            char rm[200] = { 0 };
            SWSE_FeatureStop(FEAT_RAYTRACE, rm, sizeof(rm));
            SWSE_FeatureSetFlag(FEAT_RAYTRACE, false);
            SWSE_PluginsNotifyFeature(FEAT_RAYTRACE, false);
            lstrcpynA(m, "graphics off - the game renders as shipped; ray tracing went off with it "
                         "(features.txt keeps its line)", sizeof(m));
        } else {
            lstrcpynA(m, "graphics off - the game renders as shipped", sizeof(m));
        }
        break;
    case FEAT_HDTEXTURES:
        lstrcpynA(m, "HD textures off - textures loaded from now on are vanilla "
                     "(warp or reload for a whole level)", sizeof(m));
        break;
    case FEAT_HITREACT:
        SWSE_HitReactWatch(0);
        SWSE_HitReactEnable(0);
        SWSE_HitReactRemove();
        lstrcpynA(m, "hit reactions off (hook left in place, inert)", sizeof(m));
        break;
    case FEAT_FOLIAGE: {
        char wm[200] = { 0 }, fm[200] = { 0 };
        SWSE_WindSet(0, wm, sizeof(wm));
        // Now, not on a later frame: with the switch off the frame hook no
        // longer services wind, so a queued restore would never run.
        SWSE_WindRestoreNow();
        SWSE_FoliageTrack(0, fm, sizeof(fm));
        lstrcpynA(m, "foliage off - plants are static again", sizeof(m));
        break;
    }
    case FEAT_AITUNING:
        // Restoring touches only objects already tuned - no scan, so no stall.
        SWSE_AiTuneApply("off", m, sizeof(m));
        break;
    case FEAT_TRIGGERS:
        SWSE_TriggersEnable(0);
        lstrcpynA(m, "triggers off", sizeof(m));
        break;
    case FEAT_NPCTUNING:
        SWSE_NpcTuningDisable();
        lstrcpynA(m, "character tuning off - characters already tuned keep it "
                     "until the level reloads", sizeof(m));
        break;
    case FEAT_RAYTRACE:
        // The gfx frame stops calling the tracer, so its AO texture is no
        // longer bound and the composite reverts on the next frame.
        lstrcpynA(m, "ray tracing off - the pipeline renders without it from the next frame", sizeof(m));
        break;
    case FEAT_PLAYERTUNE:
        SWSE_PlayerTuneRestore(m, sizeof(m));
        break;
    case FEAT_PREFSEDIT:
        SWSE_PrefsEditRestoreAll(m, sizeof(m));
        break;
    default:
        lstrcpynA(m, "unknown feature", sizeof(m));
        if (msg) lstrcpynA(msg, m, msgLen);
        return false;
    }
    LogFH(std::string("feature stop: ") + SWSE_FeatureName(f) + " - " + m);
    if (msg) lstrcpynA(msg, m, msgLen);
    return true;
}

// --- the hook -------------------------------------------------------------
static bool g_gfxTried = false;

// ---- frame-stall attribution ----------------------------------------------
// An intermittent freeze is only diagnosable if we can say whether SWSE caused
// it. Two clocks per frame: how long OUR work took, and how long the whole
// frame took. A long frame with tiny SWSE work is the game or the driver; a
// long frame with long SWSE work is us.
static double g_lastFrameEnd = 0;
static double g_worstOurs = 0, g_worstFrame = 0;
static int    g_frameStalls = 0;
static double g_lastOurs = 0, g_lastFrame = 0;

static double FhNowMs() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

void SWSE_FramePerf(double* lastOurs, double* worstOurs,
                    double* lastFrame, double* worstFrame, int* stalls) {
    if (lastOurs)   *lastOurs   = g_lastOurs;
    if (worstOurs)  *worstOurs  = g_worstOurs;
    if (lastFrame)  *lastFrame  = g_lastFrame;
    if (worstFrame) *worstFrame = g_worstFrame;
    if (stalls)     *stalls     = g_frameStalls;
}

static BOOL WINAPI HookedSwap(HDC hdc) {
    double tStart = FhNowMs();
    if (!g_loggedFirstFrame) {
        g_loggedFirstFrame = true;
        LogFH("frame hook LIVE - wglSwapBuffers intercepted");
        char gb[240];
        SWSE_GameBuildDescribe(gb, sizeof(gb));
        LogFH(gb);
    }
    // Plugins (1.1): roll per-plugin timings and drop dead registrations -
    // here, where no plugin call is on the stack. One test with none loaded.
    SWSE_PluginsFrameBegin(hdc);
    SWSE_TraceFBOFrameMark();   // delimits frames in the FBO bind trace
    // Bring up whatever features.txt switches on, once, on the first frame (GL
    // context current; the exe's imports are resolved by now, which installing
    // hooks needs - DllMain is too early).
    if (!g_gfxTried) {
        g_gfxTried = true;
        // Features are resolved before anything installs a hook, so a disabled
        // system leaves the game genuinely untouched rather than hooking and
        // returning early. In 1.1 that is every system but the console unless
        // features.txt asks for more.
        SWSE_FeaturesInit();
        // Native plugins (1.1): record the render thread and find every
        // plugin, each becoming a switch - before any of their code can run.
        // No DLL is loaded here; the loop below loads those switched on.
        SWSE_PluginsInit();
        // Built-ins first, then plugins in load order (their ids follow).
        for (int i = 0; i < SWSE_FeatureTotal(); i++) {
            if (i == FEAT_CONSOLE) continue;
            if (SWSE_FeatureIsAuto((SwseFeature)i)) {
                SWSE_FeatureAutoEvaluate((SwseFeature)i, nullptr, 0);
                continue;
            }
            if (!SWSE_Feature((SwseFeature)i)) continue;
            // A plugin that did not come on (refused, faulted, declined) is
            // off for the session; features.txt keeps its line.
            if (!SWSE_FeatureStart((SwseFeature)i, nullptr, 0) && SWSE_FeatureIsPlugin(i))
                SWSE_FeatureSetFlag((SwseFeature)i, false);
        }
        // Named positions are console data (writepos/goto/positions), not a
        // trigger feature; 1.0.x only loaded them with triggers on.
        SWSE_PositionsLoad();
        // Background mode for tools and agents: a launch script that sets
        // SWSE_AGENTDEBUG=1 gets a game that keeps running behind other
        // windows (and minimized) from the first frame, with the desktop's
        // mouse and keyboard left alone. The `agentdebug on` command cannot do
        // that by itself: a game in the background draws no frames, so it
        // never reads the command. Players never set the variable.
        char ad[8] = { 0 };
        if (GetEnvironmentVariableA("SWSE_AGENTDEBUG", ad, sizeof(ad)) && ad[0] == '1') {
            char m[200] = { 0 };
            SWSE_AgentDebugMode(1, m, sizeof(m));
            LogFH(std::string("SWSE_AGENTDEBUG=1: ") + m);
            // ...and silent: a game playing sound behind the owner's other
            // apps is the first thing they notice. `mute off` undoes it.
            SWSE_SetGameMute(1, 0);
            LogFH("SWSE_AGENTDEBUG=1: game audio muted (`mute off` to hear it)");
        }
    }

    // AgentDebugMode is NOT started automatically. It exists to let the game be
    // driven while alt-tabbed, which serves development rather than play, and
    // it manipulates focus reporting and input suppression - the kind of thing
    // that should never be on unless it was asked for. Enable with
    // `agentdebug on`.
    SWSE_LevelTick();          // the level-is-up signal the appliers below use
    SWSE_PluginsLevelEvents(); // ...and plugins' LEVEL_UP / LEVEL_DOWN from it
    if (SWSE_Feature(FEAT_FOLIAGE)) {
        SWSE_FoliageFrameMark();
        SWSE_WindFrame();      // injects on request, then advances the wind
    }
    if (SWSE_Feature(FEAT_AITUNING))
        SWSE_AiTuneTick();     // applies aiprefs.txt `active` profile on level load
    if (SWSE_Feature(FEAT_TRIGGERS))
        SWSE_TriggersTick();   // evaluates mod-defined triggers
    if (SWSE_Feature(FEAT_PLAYERTUNE))
        SWSE_PlayerTuneTick(); // applies playerprefs.txt once per level
    if (SWSE_Feature(FEAT_PREFSEDIT))
        SWSE_PrefsEditTick();  // applies prefs.txt once per level
    SWSE_PlayNpcTick();        // playnpc's per-frame rules (no-op unless playing an NPC)
    SWSE_FreecamTick();        // freecam's safety checks (no-op unless the view is flying)
    SWSE_PluginsFrame(0, hdc); // plugins' TICK callbacks, after SWSE's own systems
    SWSE_SelfTestTick();       // verifies every feature once a level is up

    // one-time depth-availability probe (race-free: runs at swap, not at load)
    static bool probed = false;
    if (!probed) {
        probed = true;
        GLint depthBits = -1, fbBinding = -1, redBits = -1;
        glGetIntegerv(0x0D56 /*GL_DEPTH_BITS*/, &depthBits);
        glGetIntegerv(0x0D52 /*GL_RED_BITS*/, &redBits);
        glGetIntegerv(0x8CA6 /*GL_FRAMEBUFFER_BINDING*/, &fbBinding);
        char b[200];
        wsprintfA(b, "DEPTHPROBE: default-fb depthBits=%d redBits=%d framebufferBinding=%d",
                  depthBits, redBits, fbBinding);
        LogFH(b);
    }

    // `snap` without the pipeline: SWSE_GfxFrame services it when graphics
    // runs; otherwise it is serviced here, so a screenshot never depends on a
    // feature switch.
    if (!(SWSE_Feature(FEAT_GRAPHICS) && SWSE_GfxReady()))
        SWSE_GfxServiceSnapshot(hdc);

    if (SWSE_Feature(FEAT_GRAPHICS)) {
        if (SWSE_GfxReady()) {
            SWSE_GfxFrame(hdc);        // uses true window size; no-op until F10 ON
        } else {
            // Graphics was ASKED FOR and failed to initialise: the marker at
            // least proves frame control. With graphics off (the 1.1 default)
            // nothing is drawn - 1.0.x drew this square whenever the pipeline
            // was absent, including when it had been switched off on purpose.
            DrawMarker();
        }
    }
    // SSR mask frame mark - MUST run AFTER the gfx pass, as the last word
    // before the next frame's scene draws. It lifts the pass's stamping
    // suspension and preps the clears; when it ran before GfxFrame in this
    // same callback, the pass re-suspended immediately and every scene draw
    // of every frame rendered with stamping dead. Measured: a one-frame
    // census saw 9 graded metals bound while the bind counter read 0.
    if (SWSE_Feature(FEAT_GRAPHICS))
        SWSE_MaterialsFrameMark();

    // Serviced here because reading ARB programs requires binding them, and a
    // GL context is current at swap. No-op unless a dump was requested.
    SWSE_ShaderDumpService();

    // Plugins' OVERLAY callbacks: after the post-process, before the console,
    // so the console stays on top; SWSE saves and restores the GL state
    // around them (only when one exists).
    SWSE_PluginsFrame(1, hdc);

    // The console owns the remote mailbox as well as the overlay, so turning
    // it off also turns off external scripting of the game.
    if (SWSE_Feature(FEAT_CONSOLE))
        SWSE_ConsoleFrame(hdc);       // SWSE Console overlay (drawn on top)

    double tOurs = FhNowMs() - tStart;

    // unhook -> call real -> rehook (safe for single render thread)
    RestoreBytes((void*)g_realSwap);
    BOOL r = g_realSwap(hdc);
    WriteJump((void*)g_realSwap, (void*)&HookedSwap);

    double tEnd = FhNowMs();
    double frame = (g_lastFrameEnd > 0) ? (tEnd - g_lastFrameEnd) : 0;
    g_lastFrameEnd = tEnd;
    g_lastOurs = tOurs; g_lastFrame = frame;
    if (tOurs > g_worstOurs)  g_worstOurs  = tOurs;
    if (frame > g_worstFrame) g_worstFrame = frame;

    // 80 ms is ~5 dropped frames at 60 - well past "felt it" and well clear of
    // ordinary jitter.
    if (frame > 80.0) {
        g_frameStalls++;
        // At most one line every 5 s: behind other windows the driver
        // throttles every frame to ~90 ms, and one line per frame buried
        // the log. A stall SWSE caused is always logged.
        static double s_nextLog = 0;
        static int    s_quiet = 0;
        bool ours = tOurs > frame * 0.5;
        if (ours || tEnd >= s_nextLog) {
            // Name the plugin when one owns most of SWSE's share (1.1).
            char culprit[80] = "";
            char who[72];
            if (SWSE_PluginsStallCulprit(tOurs, who, sizeof(who)))
                _snprintf_s(culprit, sizeof(culprit), _TRUNCATE, ", %s", who);
            char b[320];
            _snprintf_s(b, sizeof(b), _TRUNCATE, "FRAMESTALL: frame %d ms, SWSE work %d ms, swap %d ms -> %s%s%s",
                        (int)frame, (int)tOurs, (int)(frame - tOurs),
                        ours ? "SWSE" : "game/driver", culprit,
                        s_quiet ? " (and more since the last line - is the game in the background?)" : "");
            LogFH(b);
            s_nextLog = tEnd + 5000.0;
            s_quiet = 0;
        } else {
            s_quiet++;
        }
    }
    return r;
}

static DWORD WINAPI HookThread(LPVOID) {
    // wait for the GL library to be present, then resolve + hook
    HMODULE gl = nullptr;
    for (int i = 0; i < 6000 && !gl; ++i) {   // up to ~60s
        gl = GetModuleHandleA("opengl32.dll");
        if (!gl) Sleep(10);
    }
    if (!gl) { LogFH("frame hook: opengl32.dll never loaded"); return 0; }

    g_realSwap = (wglSwapBuffers_t)GetProcAddress(gl, "wglSwapBuffers");
    if (!g_realSwap) { LogFH("frame hook: wglSwapBuffers not found"); return 0; }

    // resolve glUseProgram (GLSL) via wglGetProcAddress for shader unbinding
    typedef PROC(WINAPI* wglGPA_t)(LPCSTR);
    wglGPA_t wglGPA = (wglGPA_t)GetProcAddress(gl, "wglGetProcAddress");
    if (wglGPA) {
        g_glUseProgram = (glUseProgram_t)wglGPA("glUseProgram");
        if (!g_glUseProgram)
            g_glUseProgram = (glUseProgram_t)wglGPA("glUseProgramObjectARB");
    }

    memcpy(g_origBytes, (void*)g_realSwap, 5);
    WriteJump((void*)g_realSwap, (void*)&HookedSwap);
    g_hooked = true;
    SWSE_HookNote((void*)g_realSwap, 5, "framehook", SWSE_HOOK_REHOOK, "wglSwapBuffers");
    LogFH(g_glUseProgram ? "frame hook installed (GLSL unbind available)"
                         : "frame hook installed (no glUseProgram; ARB unbind only)");
    return 0;   // glspy is installed from the swap hook, where a GL context is current
}

void SWSE_StartFrameHook() {
    CreateThread(nullptr, 0, HookThread, nullptr, 0, nullptr);
}
