// qaplug.cpp - test plugins for SWSE's plugin loader, one source built many
// ways (build.bat). Each build is a separate DLL whose file name is its switch
// (QA_NAME) and whose behaviour is picked by one QA_MODE_* define. They exist
// to be driven by tools\swse_plugin_tests.ps1 in the real game; NEVER SHIP
// THEM - several misbehave on purpose.
//
//   QA_MODE_IDLE       empty TICK and OVERLAY callbacks (frame cost T17; rescan T15)
//   QA_MODE_API99      Query claims plugin API 99                 (refused, T13)
//   QA_MODE_DECLINE    Query returns 0                             (refused, T13)
//   QA_MODE_QUERYFAULT Query writes through a null pointer         (faulted)
//   QA_MODE_LOADFAULT  Load registers a command, then faults       (faulted, rolled back)
//   QA_MODE_ENABLEFAULT onEnable faults                            (faulted)
//   QA_MODE_ROLLBACK   Load registers one of everything, returns 0 (refused, rolled back: QA D6)
//   QA_MODE_MANY       registers commands until SWSE_E_FULL, then removes them all
//   QA_MODE_OVERLAY    draws a square in OVERLAY and leaves a program, blending
//                      and its own framebuffer bound (T18); `<name> crash` faults
//                      mid-glBegin in the next overlay
//   QA_MODE_EVENTS     every event, printed; a log line every 200 frames (T15, T16)
//   QA_MODE_GL         a bind listener and the texture upload event, Post from a
//                      worker, reports, BUSY and TAKEN probes (phase 2)
#include "swse_plugin_api.h"
#include <windows.h>
#include <gl/GL.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "user32.lib")

#ifndef QA_NAME
#error "build with /DQA_NAME=\"name\" and one QA_MODE_*"
#endif

static SWSEPluginHandle       g_self;
static const SWSELogAPI*      g_log;
static const SWSEConsoleAPI*  g_con;
static const SWSEFrameAPI*    g_frame;
static const SWSEEventsAPI*   g_ev;
static const SWSEFeaturesAPI* g_feat;
static const SWSEGLAPI*       g_gl;
static const SWSEReportsAPI*  g_rep;
static const SWSEMemoryAPI*   g_mem;

#define LOG(...)   SWSE_Logf(g_log, g_self, SWSE_LOG_INFO, __VA_ARGS__)
#define SAY(...)   SWSE_Printf(g_con, g_self, __VA_ARGS__)

static int32_t SWSE_CALL OnEnable(char* msg, int32_t len, void*) {
#ifdef QA_MODE_ENABLEFAULT
    *(volatile int*)0x20 = 1;                       // fault in onEnable
#endif
    SWSE_Msg(msg, len, QA_NAME " on");
    return 1;
}
static int32_t SWSE_CALL OnDisable(char* msg, int32_t len, void*) {
    SWSE_Msg(msg, len, QA_NAME " off");
    return 1;
}

// ---- idle ---------------------------------------------------------------------------
#if defined(QA_MODE_IDLE) || defined(QA_MODE_ROLLBACK)
static void SWSE_CALL Nothing(const SWSEFrameInfo*, void*) {}
#endif

// ---- overlay --------------------------------------------------------------------------
#ifdef QA_MODE_OVERLAY
typedef void (APIENTRY* PfnGenPrograms)(GLsizei, GLuint*);
typedef void (APIENTRY* PfnBindProgram)(GLenum, GLuint);
typedef void (APIENTRY* PfnProgramString)(GLenum, GLenum, GLsizei, const void*);
typedef void (APIENTRY* PfnGenFbo)(GLsizei, GLuint*);
typedef void (APIENTRY* PfnBindFbo)(GLenum, GLuint);
typedef void (APIENTRY* PfnFboTex)(GLenum, GLenum, GLenum, GLuint, GLint);
static PfnBindProgram s_bindProg;
static PfnBindFbo     s_bindFbo;
static GLuint s_prog, s_fbo, s_fboTex;
static bool   s_setup, s_armCrash;
static unsigned s_frames;

static void Setup() {
    s_setup = true;
    PfnGenPrograms   gen = (PfnGenPrograms)g_gl->GetProc("glGenProgramsARB");
    PfnProgramString str = (PfnProgramString)g_gl->GetProc("glProgramStringARB");
    PfnGenFbo        gfb = (PfnGenFbo)g_gl->GetProc("glGenFramebuffersEXT");
    PfnFboTex        ftx = (PfnFboTex)g_gl->GetProc("glFramebufferTexture2DEXT");
    s_bindProg = (PfnBindProgram)g_gl->GetProc("glBindProgramARB");
    s_bindFbo  = (PfnBindFbo)g_gl->GetProc("glBindFramebufferEXT");
    if (gen && str && s_bindProg) {
        static const char kFp[] = "!!ARBfp1.0\nMOV result.color, {1.0, 0.0, 1.0, 1.0};\nEND\n";
        gen(1, &s_prog);
        s_bindProg(0x8804 /*GL_FRAGMENT_PROGRAM_ARB*/, s_prog);
        str(0x8804, 0x8875 /*GL_PROGRAM_FORMAT_ASCII_ARB*/, (GLsizei)(sizeof(kFp) - 1), kFp);
        s_bindProg(0x8804, 0);
    }
    if (gfb && ftx && s_bindFbo) {
        GLint prevTex = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
        glGenTextures(1, &s_fboTex);
        glBindTexture(GL_TEXTURE_2D, s_fboTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
        gfb(1, &s_fbo);
        GLint prevFbo = 0;
        glGetIntegerv(0x8CA6, &prevFbo);
        s_bindFbo(0x8D40, s_fbo);
        ftx(0x8D40, 0x8CE0 /*COLOR_ATTACHMENT0*/, GL_TEXTURE_2D, s_fboTex, 0);
        s_bindFbo(0x8D40, (GLuint)prevFbo);
    }
    LOG("overlay setup: fragment program %u, framebuffer %u", s_prog, s_fbo);
}

// Draws a magenta square with its own fragment program, then leaves the
// program bound and enabled, blending on and its framebuffer bound: SWSE must
// put all of it back before the console draws (T18).
static void SWSE_CALL OnOverlay(const SWSEFrameInfo* f, void*) {
    if (!s_setup) Setup();
    s_frames++;
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();                                   // left pushed on purpose, too
    glLoadIdentity();
    glOrtho(0, f->width, f->height, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_TEXTURE_2D);
    if (s_prog) { glEnable(0x8804); s_bindProg(0x8804, s_prog); }
    glEnable(GL_BLEND);
    glBegin(GL_QUADS);
    glVertex2f((float)f->width - 90, 20); glVertex2f((float)f->width - 20, 20);
    glVertex2f((float)f->width - 20, 90); glVertex2f((float)f->width - 90, 90);
    if (s_armCrash) { s_armCrash = false; *(volatile int*)0x30 = 1; }   // faults inside glBegin
    glEnd();
    if (s_fbo) s_bindFbo(0x8D40, s_fbo);              // ...and leave it all bound
}

static void SWSE_CALL Cmd(int32_t argc, const char* const* argv, void*) {
    if (argc > 1 && !lstrcmpiA(argv[1], "crash")) {
        s_armCrash = true;
        SAY(QA_NAME ": the next overlay callback faults inside glBegin");
        return;
    }
    SAY(QA_NAME ": %u overlay frames; program %u, framebuffer %u left bound each frame", s_frames, s_prog, s_fbo);
}
#endif

// ---- events ---------------------------------------------------------------------------
#ifdef QA_MODE_EVENTS
static unsigned s_up, s_down, s_mods, s_feat, s_frames;
static void SWSE_CALL OnEvent(uint32_t id, const void* data, void*) {
    if (id == SWSE_EV_LEVEL_UP)   { s_up++;   SAY(QA_NAME ": LEVEL_UP epoch %u", ((const SWSELevelEvent*)data)->epoch); }
    if (id == SWSE_EV_LEVEL_DOWN) { s_down++; SAY(QA_NAME ": LEVEL_DOWN epoch %u", ((const SWSELevelEvent*)data)->epoch); }
    if (id == SWSE_EV_MODS_RELOADED) { s_mods++; SAY(QA_NAME ": MODS_RELOADED"); LOG("MODS_RELOADED received"); }
    if (id == SWSE_EV_FEATURE) {
        const SWSEFeatureEvent* e = (const SWSEFeatureEvent*)data;
        s_feat++;
        SAY(QA_NAME ": FEATURE %s %s", e->name, e->on ? "on" : "off");
    }
}
static void SWSE_CALL OnTick(const SWSEFrameInfo* f, void*) {
    if (++s_frames % 200 == 0) LOG("tick %u (frame %u, %dx%d)", s_frames, f->frame, f->width, f->height);
}
static void SWSE_CALL Cmd(int32_t, const char* const*, void*) {
    SAY(QA_NAME ": level up %u, down %u, mods reloaded %u, feature %u, frames %u", s_up, s_down, s_mods, s_feat, s_frames);
}
#endif

// ---- GL, Post, reports, BUSY, TAKEN ------------------------------------------------------
#ifdef QA_MODE_GL
static volatile unsigned s_binds, s_uploads;
static unsigned s_lastFp, s_lastTex, s_fpChecked, s_fpWrong;
static bool     s_armFault, s_armBusy, s_fpFile;
static SWSEStatus s_busyExec = 1, s_busyReg = 1;
static char     s_fpPath[MAX_PATH];

static void SWSE_CALL NoopCmd(int32_t, const char* const*, void*) {}

static void SWSE_CALL OnBind(uint32_t target, uint32_t tex, void*) {
    (void)target; (void)tex;
    s_binds++;
    if (s_armBusy) {                                   // inside glBindTexture: both must be BUSY
        s_armBusy = false;
        s_busyExec = g_con->Execute(g_self, "echo from-inside-a-bind-listener");
        SWSECommandDesc d = { 0 };
        d.size = sizeof(d); d.name = QA_NAME "busy"; d.fn = NoopCmd;
        s_busyReg = g_con->RegisterCommand(g_self, &d);
    }
    if (s_armFault) { s_armFault = false; *(volatile int*)0x40 = 1; }
}
static void SWSE_CALL OnUpload(uint32_t, const void* data, void*) {
    const SWSETextureUploadEvent* e = (const SWSETextureUploadEvent*)data;
    s_uploads++;
    s_lastFp = e->fingerprint;
    s_lastTex = e->texId;
    // The documented fingerprint (the header, and tools/texmap.py): FNV-1a
    // over at most the first 4096 bytes, xor w*73856093, xor h*19349663.
    if (e->data && e->dataSize > 0) {
        unsigned h = 2166136261u;
        const unsigned char* p = (const unsigned char*)e->data;
        int n = e->dataSize < 4096 ? e->dataSize : 4096;
        for (int i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
        h ^= (unsigned)e->width * 73856093u;
        h ^= (unsigned)e->height * 19349663u;
        s_fpChecked++;
        if (h != e->fingerprint) s_fpWrong++;
    }
    if (s_fpFile) {                                    // same format as swse_frame_textures.txt
        FILE* f = nullptr;
        fopen_s(&f, s_fpPath, "a");
        if (f) { fprintf(f, "%08X  texid=%u  %dx%d fmt=0x%X\n", e->fingerprint, e->texId, e->width, e->height, e->glFormat); fclose(f); }
    }
}
// Posted lines run at the top of the next frame, outside any mailbox reply,
// so they write to the log (`log`), where the test looks for them in order.
static DWORD WINAPI Poster(LPVOID) {
    char l[64];
    for (int i = 1; i <= 3; i++) {
        _snprintf_s(l, sizeof(l), _TRUNCATE, "log " QA_NAME " post %d of 3", i);
        g_con->Post(g_self, l);                        // [any thread]
    }
    return 0;
}
static void SWSE_CALL OnReport(uint32_t kind, const void* sink, void*) {
    char b[160];
    if (kind == SWSE_REPORT_SELFTEST) {
        _snprintf_s(b, sizeof(b), _TRUNCATE, "%u binds and %u uploads seen", s_binds, s_uploads);
        g_rep->Check(sink, s_binds > 0 ? SWSE_CHECK_PASS : SWSE_CHECK_WARN, QA_NAME, b);
    } else {
        _snprintf_s(b, sizeof(b), _TRUNCATE, "%-17s: %u binds, %u uploads so far", QA_NAME, s_binds, s_uploads);
        g_rep->Line(sink, b);
    }
}
static void SWSE_CALL Cmd(int32_t argc, const char* const* argv, void*) {
    const char* sub = argc > 1 ? argv[1] : "";
    if (!lstrcmpiA(sub, "fault")) { s_armFault = true; SAY(QA_NAME ": the next bind listener call faults"); return; }
    if (!lstrcmpiA(sub, "busy"))  { s_armBusy = true;  SAY(QA_NAME ": the next bind listener call tries Execute and RegisterCommand"); return; }
    if (!lstrcmpiA(sub, "post")) {
        HANDLE h = CreateThread(nullptr, 0, Poster, nullptr, 0, nullptr);
        if (h) { WaitForSingleObject(h, 5000); CloseHandle(h); }
        SAY(QA_NAME ": a worker thread posted 3 lines; they run next frame, in order");
        return;
    }
    if (!lstrcmpiA(sub, "fps")) {                      // log fingerprints to a file from now on
        s_fpFile = true;
        SAY(QA_NAME ": upload fingerprints go to %s", s_fpPath);
        return;
    }
    if (!lstrcmpiA(sub, "write")) {                    // into a patched range: must be refused
        HMODULE gl = GetModuleHandleA("opengl32.dll");
        uint32_t a = gl ? (uint32_t)(uintptr_t)GetProcAddress(gl, "glBindTexture") : 0;
        unsigned char b = 0;
        SWSEStatus rd = g_mem->Read(a, &b, 1);
        SWSEStatus wr = g_mem->Write(a, &b, 1);        // the same byte back
        SAY(QA_NAME ": Memory.Write(glBindTexture) -> %d (SWSE_E_TAKEN is %d); Read -> %d", (int)wr, SWSE_E_TAKEN, (int)rd);
        return;
    }
    SAY(QA_NAME ": %u binds, %u uploads (last fp %08X texid %u); fingerprints recomputed %u, wrong %u; bound2D %u",
        s_binds, s_uploads, s_lastFp, s_lastTex, s_fpChecked, s_fpWrong, g_gl->BoundTexture2D());
    SAY(QA_NAME ": BUSY probe: Execute %d, Register %d (SWSE_E_BUSY is %d)", (int)s_busyExec, (int)s_busyReg, SWSE_E_BUSY);
}
#endif

// ---- many -----------------------------------------------------------------------------
#ifdef QA_MODE_MANY
static void SWSE_CALL NoopCmd(int32_t, const char* const*, void*) {}
static int s_registered, s_firstFail;
static void RegisterMany() {
    static char names[300][16];
    SWSECommandDesc d = { 0 };
    d.size = sizeof(d); d.fn = NoopCmd;
    s_registered = 0; s_firstFail = 0;
    for (int i = 0; i < 300; i++) {
        _snprintf_s(names[i], sizeof(names[i]), _TRUNCATE, "qamany%03d", i);
        d.name = names[i];
        SWSEStatus st = g_con->RegisterCommand(g_self, &d);
        if (st != SWSE_OK) { s_firstFail = st; break; }
        s_registered++;
    }
    for (int i = 0; i < s_registered; i++) g_con->UnregisterCommand(g_self, names[i]);
    LOG("registered %d command(s) before status %d (SWSE_E_FULL is %d); removed them all", s_registered, s_firstFail, SWSE_E_FULL);
}
static void SWSE_CALL Cmd(int32_t, const char* const*, void*) {
    SAY(QA_NAME ": %d registered before status %d (SWSE_E_FULL is %d), then all removed", s_registered, s_firstFail, SWSE_E_FULL);
}
#endif

// ---- the two exports -------------------------------------------------------------------
SWSE_PLUGIN_EXPORT int32_t SWSE_CALL SWSEPlugin_Query(const SWSEInterface* swse, SWSEPluginInfo* info) {
    (void)swse;
#ifdef QA_MODE_QUERYFAULT
    *(volatile int*)0x10 = 1;
#endif
    info->apiVersion     = SWSE_PLUGIN_API_VERSION;
#ifdef QA_MODE_API99
    info->apiVersion     = 99;
#endif
    info->name           = QA_NAME;
    info->version        = SWSE_MAKE_VERSION(0, 9, 0, 0);
    info->minSwseVersion = SWSE_MAKE_VERSION(1, 1, 0, 0);
    info->description    = "SWSE loader test plugin (" QA_NAME ") - never ship";
    info->author         = "SWSE QA";
#ifdef QA_MODE_DECLINE
    SWSE_Logf(SWSE_GET(swse, SWSELogAPI, SWSE_IFACE_LOG, 1), swse->self, SWSE_LOG_WARN,
              "declining to load, on purpose");
    return 0;
#else
    return 1;
#endif
}

SWSE_PLUGIN_EXPORT int32_t SWSE_CALL SWSEPlugin_Load(const SWSEInterface* swse) {
    g_self  = swse->self;
    g_log   = SWSE_GET(swse, SWSELogAPI,      SWSE_IFACE_LOG,      1);
    g_con   = SWSE_GET(swse, SWSEConsoleAPI,  SWSE_IFACE_CONSOLE,  1);
    g_frame = SWSE_GET(swse, SWSEFrameAPI,    SWSE_IFACE_FRAME,    1);
    g_ev    = SWSE_GET(swse, SWSEEventsAPI,   SWSE_IFACE_EVENTS,   1);
    g_feat  = SWSE_GET(swse, SWSEFeaturesAPI, SWSE_IFACE_FEATURES, 1);
    g_gl    = SWSE_GET(swse, SWSEGLAPI,       SWSE_IFACE_GL,       1);
    g_rep   = SWSE_GET(swse, SWSEReportsAPI,  SWSE_IFACE_REPORTS,  1);
    g_mem   = SWSE_GET(swse, SWSEMemoryAPI,   SWSE_IFACE_MEMORY,   1);
    if (!g_log || !g_con || !g_frame || !g_ev || !g_feat) return 0;
    g_feat->SetHandlers(g_self, OnEnable, OnDisable, nullptr);

    SWSECommandDesc d = { 0 };
    d.size = sizeof(d);
    d.name = QA_NAME;
    d.help = QA_NAME " - loader test plugin";

#ifdef QA_MODE_IDLE
    g_frame->Register(g_self, SWSE_FRAME_TICK, Nothing, nullptr);
    g_frame->Register(g_self, SWSE_FRAME_OVERLAY, Nothing, nullptr);
#endif
#ifdef QA_MODE_LOADFAULT
    d.fn = [](int32_t, const char* const*, void*) {};
    g_con->RegisterCommand(g_self, &d);                // must not survive the fault
    *(volatile int*)0x14 = 1;
#endif
#ifdef QA_MODE_ROLLBACK
    // One of everything, then refuse: SWSE must undo all of it.
    d.fn = [](int32_t, const char* const*, void*) {};
    LOG("rollback: command %d, frame %d, event %d, report %d, listener %d - then Load returns 0",
        (int)g_con->RegisterCommand(g_self, &d),
        (int)g_frame->Register(g_self, SWSE_FRAME_TICK, Nothing, nullptr),
        (int)g_ev->Subscribe(g_self, SWSE_EV_LEVEL_UP, [](uint32_t, const void*, void*) {}, nullptr),
        g_rep ? (int)g_rep->Register(g_self, SWSE_REPORT_PERF, [](uint32_t, const void*, void*) {}, nullptr) : 1,
        g_gl ? (int)g_gl->AddBindTextureListener(g_self, [](uint32_t, uint32_t, void*) {}, nullptr) : 1);
    return 0;
#endif
#ifdef QA_MODE_MANY
    d.fn = Cmd;
    g_con->RegisterCommand(g_self, &d);
    RegisterMany();
#endif
#ifdef QA_MODE_OVERLAY
    if (!g_gl) return 0;
    d.fn = Cmd;
    d.help = QA_NAME " [crash] - overlay that leaves GL state behind";
    g_con->RegisterCommand(g_self, &d);
    g_frame->Register(g_self, SWSE_FRAME_OVERLAY, OnOverlay, nullptr);
#endif
#ifdef QA_MODE_EVENTS
    d.fn = Cmd;
    g_con->RegisterCommand(g_self, &d);
    g_ev->Subscribe(g_self, SWSE_EV_LEVEL_UP, OnEvent, nullptr);
    g_ev->Subscribe(g_self, SWSE_EV_LEVEL_DOWN, OnEvent, nullptr);
    g_ev->Subscribe(g_self, SWSE_EV_MODS_RELOADED, OnEvent, nullptr);
    LOG("subscribe FEATURE -> %d", (int)g_ev->Subscribe(g_self, SWSE_EV_FEATURE, OnEvent, nullptr));
    LOG("subscribe an event id SWSE does not raise (99) -> %d (SWSE_E_UNSUPPORTED is %d)",
        (int)g_ev->Subscribe(g_self, 99, OnEvent, nullptr), SWSE_E_UNSUPPORTED);
    g_frame->Register(g_self, SWSE_FRAME_TICK, OnTick, nullptr);
#endif
#ifdef QA_MODE_GL
    if (!g_gl || !g_rep || !g_mem || !SWSE_HAS(g_con, SWSEConsoleAPI, Post)) return 0;
    const SWSEModsAPI* mods = SWSE_GET(swse, SWSEModsAPI, SWSE_IFACE_MODS, 1);
    char dir[MAX_PATH] = "";
    if (mods) mods->GameBinDir(dir, sizeof(dir));
    _snprintf_s(s_fpPath, sizeof(s_fpPath), _TRUNCATE, "%s\\swse_" QA_NAME "_uploads.txt", dir);
    d.fn = Cmd;
    d.help = QA_NAME " [fault|busy|post|fps|write] - GL notification test plugin";
    g_con->RegisterCommand(g_self, &d);
    LOG("bind listener -> %d", (int)g_gl->AddBindTextureListener(g_self, OnBind, nullptr));
    LOG("upload event  -> %d", (int)g_ev->Subscribe(g_self, SWSE_EV_TEXTURE_UPLOAD, OnUpload, nullptr));
    g_rep->Register(g_self, SWSE_REPORT_SELFTEST, OnReport, nullptr);
    g_rep->Register(g_self, SWSE_REPORT_PERF, OnReport, nullptr);
    g_rep->Register(g_self, SWSE_REPORT_STATUS, OnReport, nullptr);
#endif
#ifndef QA_MODE_ROLLBACK
    LOG("loaded (console table v%u, Post %s)", g_con->version, SWSE_HAS(g_con, SWSEConsoleAPI, Post) ? "served" : "absent");
    return 1;
#endif
}
