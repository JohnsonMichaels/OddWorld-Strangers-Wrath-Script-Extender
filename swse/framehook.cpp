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
#include "wind.h"
#include "granny.h"
#include "input.h"
#include "scriptvm.h"
#include "selftest.h"
#include "aitune.h"
#include "triggers.h"
#include "positions.h"
#include "features.h"
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
    }
    SWSE_TraceFBOFrameMark();   // delimits frames in the FBO bind trace
    // init the post-process pipeline once (GL context is current here)
    if (!g_gfxTried) {
        g_gfxTried = true;
        // Features are resolved before anything installs a hook, so a disabled
        // system leaves the game genuinely untouched rather than hooking and
        // returning early.
        SWSE_FeaturesInit();

        if (SWSE_Feature(FEAT_GRAPHICS)) SWSE_GfxInit();
        // glspy carries BOTH the FBO tracking the graphics pipeline needs and
        // the texture-upload hook HD replacement rides on, so it installs if
        // either is wanted.
        if (SWSE_Feature(FEAT_GRAPHICS) || SWSE_Feature(FEAT_HDTEXTURES))
            SWSE_InstallGLSpy();
        if (SWSE_Feature(FEAT_FOLIAGE)) {
            SWSE_FoliageInit();       // load the foliage fingerprint list
            SWSE_WindLoadSettings();  // may enable wind + the foliage tracker
        }
        // Parse aiprefs.txt up front so `difficulty <name>` works immediately.
        // Only the PROFILES are read here - nothing is applied, because the
        // prefs objects do not exist until a level is loaded.
        if (SWSE_Feature(FEAT_AITUNING)) { char m[220]; SWSE_AiTuneLoad(m, sizeof(m)); }
        if (SWSE_Feature(FEAT_TRIGGERS)) {
            SWSE_PositionsLoad();
            SWSE_TriggersLoad();
        }
    }

    // Hit reactions come up on their own too. Deferred to the first frame
    // rather than DllMain: installing hooks needs the exe's imports resolved.
    static bool hitReactInit = false;
    if (!hitReactInit) {
        hitReactInit = true;
        if (SWSE_Feature(FEAT_HITREACT)) SWSE_HitReactLoadSettings();
    }

    // AgentDebugMode is NOT started automatically. It exists to let the game be
    // driven while alt-tabbed, which serves development rather than play, and
    // it manipulates focus reporting and input suppression - the kind of thing
    // that should never be on unless it was asked for. Enable with
    // `agentdebug on`.
    if (SWSE_Feature(FEAT_FOLIAGE)) {
        SWSE_FoliageFrameMark();
        SWSE_WindFrame();      // injects on request, then advances the wind
    }
    if (SWSE_Feature(FEAT_AITUNING))
        SWSE_AiTuneTick();     // applies aiprefs.txt `active` profile on level load
    if (SWSE_Feature(FEAT_TRIGGERS))
        SWSE_TriggersTick();   // evaluates mod-defined triggers
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

    if (SWSE_GfxReady()) {
        SWSE_GfxFrame(hdc);            // uses true window size; no-op until F10 ON
    } else {
        DrawMarker();                  // gfx init failed: at least prove frame control
    }

    // Serviced here because reading ARB programs requires binding them, and a
    // GL context is current at swap. No-op unless a dump was requested.
    SWSE_ShaderDumpService();

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
        char b[200];
        wsprintfA(b, "FRAMESTALL: frame %d ms, SWSE work %d ms, swap %d ms -> %s",
                  (int)frame, (int)tOurs, (int)(frame - tOurs),
                  (tOurs > frame * 0.5) ? "SWSE" : "game/driver");
        LogFH(b);
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
    LogFH(g_glUseProgram ? "frame hook installed (GLSL unbind available)"
                         : "frame hook installed (no glUseProgram; ARB unbind only)");
    return 0;   // glspy is installed from the swap hook, where a GL context is current
}

void SWSE_StartFrameHook() {
    CreateThread(nullptr, 0, HookThread, nullptr, 0, nullptr);
}
