// SWSE materials - see materials.h.
//
// v2: the draw-hook mask. v1 tried to stamp reflectivity into destination
// alpha via bind-time glColorMask, and died on a measured fact: this engine
// OWNS its scene FBO's alpha channel (the HD port's post pipeline writes a
// full shaded luminance image there, per pixel, from its Cg shaders). No
// bind-time state control survives that.
//
// So the mask lives where the engine cannot touch it: our own FBO. A
// glDrawElements trampoline (same shape as the foliage bind hook) re-issues
// every draw whose bound texture the owner graded reflective:
//   - into the mask FBO, sharing the GAME'S depth texture (LEQUAL, writes
//     off) so occlusion stays exact,
//   - vertex program left ON (identical transforms),
//   - fragment program OFF, all units off except a 1x1 white texture in
//     REPLACE mode - the vertex program's color output cannot darken it,
//   - everything state-scoped by glPushAttrib and restored.
// The composite samples the mask texture's red channel on unit 3.

#include "materials.h"
#include "modregistry.h"
#include "glspy.h"
#include "geocapture.h"
#include "wind.h"
#include "hookreg.h"    // every patch reported to the one list (`hooks`)
#include <windows.h>
#include <gl/GL.h>
#include <stdio.h>
#include <stdlib.h>

#pragma comment(lib, "opengl32.lib")

static void LogM(const char* s) {
    char path[MAX_PATH];
    GetModuleFileNameA(GetModuleHandleA(NULL), path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) *(slash + 1) = 0;
    lstrcatA(path, "swse_log.txt");
    FILE* f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "%s\n", s);
    fclose(f);
}

// ---- the fingerprint list -------------------------------------------------
#define MAX_MATERIALS 1024
static unsigned      g_fp[MAX_MATERIALS];
static unsigned char g_code[MAX_MATERIALS];   // 'M','G','C','A'
static int  g_fpN = 0;
static bool g_loaded = false;

#define MAX_TEXID 65536
static unsigned char g_isRefl[MAX_TEXID];
static unsigned*     g_hash = nullptr;        // own texid->fingerprint record
static int g_known = 0;

// ---- runtime state --------------------------------------------------------
static int g_stampOn = 0;      // owner param ssr_mask_stamp, via SetStamp
static int g_suspend = 0;      // 1 during our post pass + UI tail
static int g_binds = 0, g_bindsLast = 0;
static int g_marks = 0;        // FrameMark heartbeat - proves hook ordering
static int g_curRefl = 0;      // is the CURRENTLY bound texture reflective?
static int g_redraws = 0, g_redrawsLast = 0;
static int g_drawsSeen = 0, g_drawsSeenLast = 0;
static int g_glslSkips = 0, g_glslSkipsLast = 0;  // tagged draws under a GLSL program
static int g_depthTest = 1;    // ssr_mask_depth: 0 = diagnostic draw-through

// ---- the normal G-buffer --------------------------------------------------
// True vertex normals, rendered by OUR ARB vertex program during the same
// draw-hook pass. Transforms come from the canonical camera matrix the wind
// system exports (sign-corrected once, so per-program Cg quirks cannot skew
// us). Skinned draws are excluded by the wind system's own law: a vertex
// program with >64 parameter rows is a bone palette.
static int    g_gbufOn = 0;
static int    g_gbufW = 0, g_gbufH = 0;     // requested (window) size, from gfx
static GLuint g_nrmFbo = 0, g_nrmTex = 0, g_nrmDepthRb = 0;
static int    g_nrmW = 0, g_nrmH = 0;
static int    g_nrmBroken = 0;
static GLuint g_myVp = 0;
static int    g_vpBroken = 0;
static int    g_vpLocalsSet = 0;            // canonical VP loaded this frame?
static int    g_gbufDraws = 0, g_gbufDrawsLast = 0;
static int    g_skinSkips = 0, g_skinSkipsLast = 0;
// program id -> 0 unknown, 1 static (redraw), 2 skinned/oversized (skip)
static unsigned char g_progClass[65536];

static bool IsReflHash(unsigned h) {
    for (int i = g_fpN - 1; i >= 0; i--) if (g_fp[i] == h) return true;
    return false;
}

// One mod's materials.txt: lines of "<8 hex> <letter>", '#' comments free.
static void LoadListFile(const char* path, const char* modName, void*) {
    FILE* f = fopen(path, "r");
    if (!f) return;
    int before = g_fpN;
    char line[512];
    while (fgets(line, sizeof(line), f) && g_fpN < MAX_MATERIALS) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        unsigned v = 0; int n = 0;
        const char* p = line;
        for (; *p && n < 8; p++) {
            int d = (*p >= '0' && *p <= '9') ? *p - '0'
                  : (*p >= 'a' && *p <= 'f') ? *p - 'a' + 10
                  : (*p >= 'A' && *p <= 'F') ? *p - 'A' + 10 : -1;
            if (d < 0) break;
            v = v * 16 + (unsigned)d; n++;
        }
        if (n != 8) continue;
        while (*p == ' ' || *p == '\t') p++;
        char c = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : *p;
        if (c != 'M' && c != 'G' && c != 'C' && c != 'A') continue;
        g_code[g_fpN] = (unsigned char)c;
        g_fp[g_fpN++] = v;
    }
    fclose(f);
    char b[220];
    wsprintfA(b, "materials: +%d from [%s]", g_fpN - before, modName);
    LogM(b);
}

static void LoadList() {
    g_fpN = 0;
    SWSE_ForEachModFile("materials.txt", LoadListFile, nullptr);
    char b[160];
    if (g_fpN == 0)
        wsprintfA(b, "materials: no materials.txt in any enabled mod");
    else
        wsprintfA(b, "materials: %d reflective fingerprint(s) loaded", g_fpN);
    LogM(b);
}

// ---- the draw hook --------------------------------------------------------
typedef void (APIENTRY* glDrawElements_t)(GLenum, GLsizei, GLenum, const void*);
typedef void (APIENTRY* glActiveTexture_t)(GLenum);
typedef void (APIENTRY* glBindFramebuffer_t)(GLenum, GLuint);
typedef void (APIENTRY* glGenFramebuffers_t)(GLsizei, GLuint*);
typedef void (APIENTRY* glFramebufferTexture2D_t)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum (APIENTRY* glCheckFramebufferStatus_t)(GLenum);
typedef void (APIENTRY* glGenRenderbuffers_t)(GLsizei, GLuint*);
typedef void (APIENTRY* glBindRenderbuffer_t)(GLenum, GLuint);
typedef void (APIENTRY* glRenderbufferStorage_t)(GLenum, GLenum, GLsizei, GLsizei);
typedef void (APIENTRY* glFramebufferRenderbuffer_t)(GLenum, GLenum, GLenum, GLuint);
typedef void (APIENTRY* glGenProgramsARB_t)(GLsizei, GLuint*);
typedef void (APIENTRY* glBindProgramARB_t)(GLenum, GLuint);
typedef void (APIENTRY* glProgramStringARB_t)(GLenum, GLenum, GLsizei, const void*);
typedef void (APIENTRY* glProgramLocalParam4f_t)(GLenum, GLuint, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (APIENTRY* glGetProgramivARB_t)(GLenum, GLenum, GLint*);

static glDrawElements_t g_drawTramp = nullptr;
static BYTE* g_drawTrampMem = nullptr;
static bool  g_drawHooked = false;

static glActiveTexture_t         p_activeTex = nullptr;
static glBindFramebuffer_t       p_bindFbo = nullptr;
static glGenFramebuffers_t       p_genFbo = nullptr;
static glFramebufferTexture2D_t  p_fboTex2D = nullptr;
static glCheckFramebufferStatus_t p_fboStatus = nullptr;
static glGenRenderbuffers_t      p_genRb = nullptr;
static glBindRenderbuffer_t      p_bindRb = nullptr;
static glRenderbufferStorage_t   p_rbStorage = nullptr;
static glFramebufferRenderbuffer_t p_fboRb = nullptr;
static glGenProgramsARB_t        p_genProg = nullptr;
static glBindProgramARB_t        p_bindProg = nullptr;
static glProgramStringARB_t      p_progString = nullptr;
static glProgramLocalParam4f_t   p_progLocal4f = nullptr;
static glGetProgramivARB_t       p_getProgiv = nullptr;
// Vertex-attribute query, for the skinned census: attribute 0 is position
// everywhere in this engine, so its ARRAY_STRIDE is the per-vertex byte size
// the GPU was actually fed - the number the .smb 36-byte stride is tested
// against.
typedef void (APIENTRY* glGetVertexAttribiv_t)(GLuint, GLenum, GLint*);
static glGetVertexAttribiv_t     p_getVAiv = nullptr;
// Index-buffer readback, so a draw's true VERTEX count (max index + 1) can be
// measured rather than its index count.
typedef void (APIENTRY* glGetBufferParameteriv_t)(GLenum, GLenum, GLint*);
typedef void (APIENTRY* glGetBufferSubData_t)(GLenum, GLint, GLsizei, void*);
static glGetBufferParameteriv_t  p_getBufParam   = nullptr;
static glGetBufferSubData_t      p_getBufSubData = nullptr;
// Attribute 0's base pointer doubles as its byte offset inside the bound VBO,
// which is where the position lane actually starts - it is not assumed to be 0.
typedef void (APIENTRY* glGetVertexAttribPointerv_t)(GLuint, GLenum, GLvoid**);
static glGetVertexAttribPointerv_t p_getVAptr = nullptr;

// Reading the pose out of the vertex program, and reaching the texcoord VBO -
// both needed by the mesh dump, neither used elsewhere in this file.
typedef void (APIENTRY* glGetProgramLocalParam_t)(GLenum, GLuint, GLfloat*);
static glGetProgramLocalParam_t  p_getProgLocal = nullptr;
typedef void (APIENTRY* glBindBuffer_t)(GLenum, GLuint);
static glBindBuffer_t            p_bindBuffer = nullptr;
// Highest local-parameter count seen on any bound vertex program. The class-2
// test is "more than 64 locals"; if nothing ever exceeds 64 then that law does
// not separate characters here, and the threshold - not the hook - is what is
// wrong. Declared here because ClassifyDraw writes it.
static int g_maxLocals = 0;

// The mask FBO. Color = RGBA8 at the game depth texture's size; depth = the
// game's own scene depth texture (shared attachment - legal, and the whole
// point: identical transforms + LEQUAL means occlusion for free).
static GLuint g_maskFbo = 0, g_maskTex = 0, g_whiteTex = 0;
static int    g_maskW = 0, g_maskH = 0;
static unsigned g_attachedDepth = 0;
static int    g_fboBroken = 0;   // completeness failed - stop retrying every draw

static void* GlProc(const char* a, const char* b) {
    void* p = (void*)wglGetProcAddress(a);
    if (!p && b) p = (void*)wglGetProcAddress(b);
    return p;
}

static bool EnsureFboFuncs() {
    if (p_bindFbo) return true;
    p_getVAiv   = (glGetVertexAttribiv_t)GlProc("glGetVertexAttribivARB", "glGetVertexAttribiv");
    p_getBufParam   = (glGetBufferParameteriv_t)GlProc("glGetBufferParameterivARB", "glGetBufferParameteriv");
    p_getBufSubData = (glGetBufferSubData_t)GlProc("glGetBufferSubDataARB", "glGetBufferSubData");
    p_getVAptr      = (glGetVertexAttribPointerv_t)GlProc("glGetVertexAttribPointervARB", "glGetVertexAttribPointerv");
    p_activeTex = (glActiveTexture_t)GlProc("glActiveTextureARB", "glActiveTexture");
    p_bindFbo   = (glBindFramebuffer_t)GlProc("glBindFramebufferEXT", "glBindFramebuffer");
    p_genFbo    = (glGenFramebuffers_t)GlProc("glGenFramebuffersEXT", "glGenFramebuffers");
    p_fboTex2D  = (glFramebufferTexture2D_t)GlProc("glFramebufferTexture2DEXT", "glFramebufferTexture2D");
    p_fboStatus = (glCheckFramebufferStatus_t)GlProc("glCheckFramebufferStatusEXT", "glCheckFramebufferStatus");
    p_genRb     = (glGenRenderbuffers_t)GlProc("glGenRenderbuffersEXT", "glGenRenderbuffers");
    p_bindRb    = (glBindRenderbuffer_t)GlProc("glBindRenderbufferEXT", "glBindRenderbuffer");
    p_rbStorage = (glRenderbufferStorage_t)GlProc("glRenderbufferStorageEXT", "glRenderbufferStorage");
    p_fboRb     = (glFramebufferRenderbuffer_t)GlProc("glFramebufferRenderbufferEXT", "glFramebufferRenderbuffer");
    p_genProg   = (glGenProgramsARB_t)GlProc("glGenProgramsARB", nullptr);
    p_bindProg  = (glBindProgramARB_t)GlProc("glBindProgramARB", nullptr);
    p_progString = (glProgramStringARB_t)GlProc("glProgramStringARB", nullptr);
    p_progLocal4f = (glProgramLocalParam4f_t)GlProc("glProgramLocalParameter4fARB", nullptr);
    p_getProgiv = (glGetProgramivARB_t)GlProc("glGetProgramivARB", nullptr);
    p_getProgLocal = (glGetProgramLocalParam_t)GlProc("glGetProgramLocalParameterfvARB", nullptr);
    p_bindBuffer   = (glBindBuffer_t)GlProc("glBindBufferARB", "glBindBuffer");
    return p_bindFbo && p_genFbo && p_fboTex2D && p_fboStatus && p_activeTex;
}

// Our normal-writing vertex program. Position math replicates the engine's
// canonical convention (rows dotted with the vertex, then clip-y negated -
// the same form the temporal reprojection already validated). Color carries
// the TRUE vertex normal, packed 0..1.
static const char* kNormalVp =
    "!!ARBvp1.0\n"
    "PARAM r1 = program.local[1];\n"
    "PARAM r2 = program.local[2];\n"
    "PARAM r3 = program.local[3];\n"
    "PARAM r4 = program.local[4];\n"
    "PARAM half = { 0.5, 0.5, 0.5, 1.0 };\n"
    // ALL-GENERIC attributes: the engine's own 137 programs never touch a
    // conventional slot (measured in the shader dump - attrib[0] position,
    // attrib[2] the traditional normal slot, 77/137 programs). Conventional
    // vertex.normal aliased to nothing and read as a constant.
    "TEMP p;\n"
    "DP4 p.x, r1, vertex.attrib[0];\n"
    "DP4 p.y, r2, vertex.attrib[0];\n"
    "DP4 p.z, r3, vertex.attrib[0];\n"
    "DP4 p.w, r4, vertex.attrib[0];\n"
    "MOV result.position.x, p.x;\n"
    "MOV result.position.y, -p.y;\n"
    "MOV result.position.z, p.z;\n"
    "MOV result.position.w, p.w;\n"
    "MAD result.color.xyz, vertex.attrib[2], half, half;\n"
    "MOV result.color.w, half.w;\n"
    "END\n";

static bool EnsureNormalVp() {
    if (g_myVp) return true;
    if (g_vpBroken) return false;
    if (!p_genProg || !p_bindProg || !p_progString || !p_progLocal4f || !p_getProgiv) {
        g_vpBroken = 1;
        LogM("gbuf: ARB program entry points missing");
        return false;
    }
    GLint prevVp = 0;
    if (p_getProgiv) p_getProgiv(0x8620, 0x8677 /*PROGRAM_BINDING*/, &prevVp);
    p_genProg(1, &g_myVp);
    p_bindProg(0x8620 /*VERTEX_PROGRAM_ARB*/, g_myVp);
    p_progString(0x8620, 0x8875 /*PROGRAM_FORMAT_ASCII*/, (GLsizei)strlen(kNormalVp), kNormalVp);
    GLint errPos = -1;
    glGetIntegerv(0x864B /*PROGRAM_ERROR_POSITION_ARB*/, &errPos);
    p_bindProg(0x8620, (GLuint)prevVp);
    if (errPos != -1) {
        char b[120];
        wsprintfA(b, "gbuf: normal vp COMPILE FAILED at char %d", errPos);
        LogM(b);
        g_myVp = 0;
        g_vpBroken = 1;
        return false;
    }
    LogM("gbuf: normal vertex program ready");
    return true;
}

// The G-buffer FBO: window-sized RGBA8 normals + our own depth renderbuffer
// (we re-rasterize the world, so we own occlusion; nothing shared, nothing
// the engine can pollute).
static bool EnsureNrm() {
    if (g_nrmBroken || g_gbufW < 64 || g_gbufH < 64) return false;
    if (!p_genRb || !p_bindRb || !p_rbStorage || !p_fboRb) {
        g_nrmBroken = 1;
        LogM("gbuf: renderbuffer entry points missing");
        return false;
    }
    if (g_nrmFbo && g_nrmW == g_gbufW && g_nrmH == g_gbufH) return true;

    GLint prevTex = 0, prevFbo = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(0x8CA6, &prevFbo);

    if (!g_nrmTex) glGenTextures(1, &g_nrmTex);
    glBindTexture(GL_TEXTURE_2D, g_nrmTex);
    glTexImage2D(GL_TEXTURE_2D, 0, 0x8058 /*RGBA8*/, g_gbufW, g_gbufH, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);

    if (!g_nrmDepthRb) p_genRb(1, &g_nrmDepthRb);
    p_bindRb(0x8D41 /*RENDERBUFFER*/, g_nrmDepthRb);
    p_rbStorage(0x8D41, 0x81A6 /*DEPTH_COMPONENT24*/, g_gbufW, g_gbufH);

    if (!g_nrmFbo) p_genFbo(1, &g_nrmFbo);
    p_bindFbo(0x8D40, g_nrmFbo);
    p_fboTex2D(0x8D40, 0x8CE0, GL_TEXTURE_2D, g_nrmTex, 0);
    p_fboRb(0x8D40, 0x8D00 /*DEPTH_ATTACHMENT*/, 0x8D41, g_nrmDepthRb);
    GLenum st = p_fboStatus(0x8D40);
    p_bindFbo(0x8D40, (GLuint)prevFbo);
    if (st != 0x8CD5) {
        char b[120];
        wsprintfA(b, "gbuf: FBO incomplete (0x%X) - disabled", st);
        LogM(b);
        g_nrmBroken = 1;
        return false;
    }
    g_nrmW = g_gbufW; g_nrmH = g_gbufH;
    char b[120];
    wsprintfA(b, "gbuf: normal G-buffer ready %dx%d", g_nrmW, g_nrmH);
    LogM(b);
    return true;
}

// Build (or rebuild on size change) the mask FBO against the current game
// depth texture. Returns false when anything is missing - the redraw is
// simply skipped that frame; instruments show it.
static bool EnsureFbo() {
    if (g_fboBroken) return false;
    if (!EnsureFboFuncs()) { g_fboBroken = 1; LogM("materials: FBO entry points missing"); return false; }
    unsigned depth = SWSE_SceneDepthTex();
    if (!depth) return false;

    GLint prevTex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glBindTexture(GL_TEXTURE_2D, (GLuint)depth);
    GLint dw = 0, dh = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &dw);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &dh);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    if (dw < 64 || dh < 64) return false;

    if (g_maskFbo && dw == g_maskW && dh == g_maskH && depth == g_attachedDepth)
        return true;

    if (!g_maskTex) glGenTextures(1, &g_maskTex);
    glBindTexture(GL_TEXTURE_2D, g_maskTex);
    glTexImage2D(GL_TEXTURE_2D, 0, 0x8058 /*GL_RGBA8*/, dw, dh, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F /*CLAMP_TO_EDGE*/);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);

    if (!g_whiteTex) {
        static const unsigned char white[4] = { 255, 255, 255, 255 };
        glGenTextures(1, &g_whiteTex);
        glBindTexture(GL_TEXTURE_2D, g_whiteTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    }

    if (!g_maskFbo) p_genFbo(1, &g_maskFbo);
    GLint prevFbo = 0;
    glGetIntegerv(0x8CA6 /*GL_FRAMEBUFFER_BINDING*/, &prevFbo);
    p_bindFbo(0x8D40 /*GL_FRAMEBUFFER*/, g_maskFbo);
    p_fboTex2D(0x8D40, 0x8CE0 /*COLOR0*/, GL_TEXTURE_2D, g_maskTex, 0);
    p_fboTex2D(0x8D40, 0x8D00 /*DEPTH*/,  GL_TEXTURE_2D, (GLuint)depth, 0);
    GLenum st = p_fboStatus(0x8D40);
    p_bindFbo(0x8D40, (GLuint)prevFbo);
    if (st != 0x8CD5 /*COMPLETE*/) {
        char b[120];
        wsprintfA(b, "materials: mask FBO incomplete (0x%X) - mask disabled", st);
        LogM(b);
        g_fboBroken = 1;
        return false;
    }
    g_maskW = dw; g_maskH = dh; g_attachedDepth = depth;
    char b[120];
    wsprintfA(b, "materials: mask FBO ready %dx%d (shared game depth %u)", dw, dh, depth);
    LogM(b);
    return true;
}

// Re-issue the draw into the mask FBO, painted flat white. The vertex
// program stays bound (identical transforms); the fragment side is forced
// to a 1x1 white texture in REPLACE mode so no shading can darken the mask.
static void MaskRedraw(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    // A GLSL program owns BOTH stages: killing it would break the vertex
    // transform, keeping it would shade the mask. Neither works - skip and
    // count, so the instrument shows how much geometry this route misses.
    GLint curProg = 0;
    glGetIntegerv(0x8B8D /*GL_CURRENT_PROGRAM*/, &curProg);
    if (curProg != 0) { g_glslSkips++; return; }
    if (!EnsureFbo()) return;
    GLint prevFbo = 0;
    glGetIntegerv(0x8CA6, &prevFbo);
    glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
                 GL_CURRENT_BIT | GL_TEXTURE_BIT);
    p_bindFbo(0x8D40, g_maskFbo);

    glDisable(0x8804 /*GL_FRAGMENT_PROGRAM_ARB*/);
    // Cg-on-GeForce era leftovers that also hijack the fragment stage and
    // are NOT silenced by the ARB disable. Blind-disabling unsupported
    // enums just sets GL_INVALID_ENUM, which is harmless.
    glDisable(0x8522 /*GL_REGISTER_COMBINERS_NV*/);
    glDisable(0x86DE /*GL_TEXTURE_SHADER_NV*/);
    glDisable(GL_LIGHTING);
    glDisable(GL_FOG);
    glDisable(GL_BLEND);
    glDisable(GL_ALPHA_TEST);
    for (int u = 3; u >= 0; u--) {
        p_activeTex(0x84C0 + u /*GL_TEXTURE0+u*/);
        glDisable(GL_TEXTURE_2D);
    }
    // unit 0: the white texture, REPLACE - immune to vertex colors
    p_activeTex(0x84C0);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_whiteTex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

    if (g_depthTest) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
    } else {
        glDisable(GL_DEPTH_TEST);   // diagnostic: isolate depth-partials vs shading
    }
    glDepthMask(GL_FALSE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    g_drawTramp(mode, count, type, indices);
    g_redraws++;

    p_bindFbo(0x8D40, (GLuint)prevFbo);
    glPopAttrib();
}

// Re-issue a world draw into the normal G-buffer with OUR vertex program.
// Skinned/oversized programs are skipped by parameter count (the wind
// system's 64-row law); GLSL and fixed-function draws are skipped outright.
// What kind of draw is this? 0 = not world geometry (GLSL, UI, effects),
// 1 = static world, 2 = skinned (a >64-row parameter block is a bone palette,
// the wind system's law). Shared by the G-buffer and the geometry probe.
static int ClassifyDraw(GLint* vpIdOut) {
    GLint curProg = 0;
    glGetIntegerv(0x8B8D /*GL_CURRENT_PROGRAM*/, &curProg);
    if (curProg != 0) return 0;
    if (!glIsEnabled(0x8620)) return 0;
    if (!p_getProgiv) return 0;
    GLint vpId = 0;
    p_getProgiv(0x8620, 0x8677 /*PROGRAM_BINDING*/, &vpId);
    if (vpId <= 0 || vpId >= 65536) return 0;
    if (g_progClass[vpId] == 0) {
        GLint nparam = 0;
        p_getProgiv(0x8620, 0x88A8 /*PROGRAM_PARAMETERS_ARB*/, &nparam);
        // Record the largest local count seen, so a census can tell "no
        // character draws reach this hook" apart from "the >64 threshold does
        // not separate them here". The first run reported zero class-2 draws,
        // which alone cannot distinguish those two.
        if (nparam > g_maxLocals) g_maxLocals = nparam;
        g_progClass[vpId] = (nparam > 64) ? 2 : 1;
    }
    if (vpIdOut) *vpIdOut = vpId;
    return g_progClass[vpId];
}

static void GBufRedraw(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    GLint vpId = 0;
    int cls = ClassifyDraw(&vpId);
    if (cls == 0) return;
    if (cls == 2) { g_skinSkips++; return; }

    float vp16[16];
    if (!SWSE_WindClipVPLast(vp16)) return;         // no camera yet this session
    if (!EnsureNrm() || !EnsureNormalVp()) return;

    GLint prevFbo = 0, vpSave[4];
    glGetIntegerv(0x8CA6, &prevFbo);
    glGetIntegerv(GL_VIEWPORT, vpSave);
    glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
                 GL_CURRENT_BIT | GL_TEXTURE_BIT);
    p_bindFbo(0x8D40, g_nrmFbo);
    glViewport(0, 0, g_nrmW, g_nrmH);

    glDisable(0x8804 /*FRAGMENT_PROGRAM_ARB*/);
    glDisable(GL_LIGHTING);
    glDisable(GL_FOG);
    glDisable(GL_BLEND);
    glDisable(GL_ALPHA_TEST);
    for (int u = 3; u >= 0; u--) {
        p_activeTex(0x84C0 + u);
        glDisable(GL_TEXTURE_2D);
    }
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);                            // we OWN this depth buffer
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    p_bindProg(0x8620, g_myVp);
    if (!g_vpLocalsSet) {                            // canonical VP, once per frame
        for (int r = 0; r < 4; r++)
            p_progLocal4f(0x8620, (GLuint)(r + 1),
                          vp16[r*4+0], vp16[r*4+1], vp16[r*4+2], vp16[r*4+3]);
        g_vpLocalsSet = 1;
    }
    g_drawTramp(mode, count, type, indices);
    g_gbufDraws++;
    p_bindProg(0x8620, (GLuint)vpId);                // engine's program back

    p_bindFbo(0x8D40, (GLuint)prevFbo);
    glViewport(vpSave[0], vpSave[1], vpSave[2], vpSave[3]);
    glPopAttrib();
}

// ---- skinned-draw census (Oddview) ----------------------------------------
// One frame of character draws: how many vertices each uses, the attribute
// stride, and the program. The question it exists to answer is whether the
// 36-byte record stride found in the .smb mesh blobs is bytes-per-vertex.
#define SKIN_MAX 96
static int g_skinCensus = 0;
static struct { int prog, count, stride, vsize, vtype, nAttr, verts; } g_skin[SKIN_MAX];
static int g_nSkin = 0;
static int g_clsCount[3] = { 0, 0, 0 };   // draws seen per ClassifyDraw result

// True VERTEX count for a draw: max(index)+1. glDrawElements reports an INDEX
// count, which is a different number and is what made the first comparison
// against the .smb record counts meaningless. The mesh blob records are
// per-vertex, so this is the quantity that decides bytes-per-vertex.
static int MaxIndexPlusOne(GLsizei count, GLenum type, const void* indices) {
    if (!p_getBufParam || !p_getBufSubData) return 0;
    GLint ebo = 0;
    glGetIntegerv(0x8895 /*ELEMENT_ARRAY_BUFFER_BINDING*/, &ebo);
    int isShort = (type == 0x1403 /*UNSIGNED_SHORT*/);
    int esz = isShort ? 2 : 4;
    if (type != 0x1403 && type != 0x1405 /*UNSIGNED_INT*/) return 0;
    if (count <= 0 || count > 200000) return 0;
    static unsigned char buf[200000 * 4];
    const unsigned char* src = 0;
    if (ebo) {
        GLint bufSize = 0;
        p_getBufParam(0x8893 /*ELEMENT_ARRAY_BUFFER*/, 0x8764 /*BUFFER_SIZE*/, &bufSize);
        size_t off = (size_t)indices;
        size_t need = (size_t)count * esz;
        if (off + need > (size_t)bufSize) return 0;
        p_getBufSubData(0x8893, (GLint)off, (GLsizei)need, buf);
        src = buf;
    } else {
        src = (const unsigned char*)indices;      // client-memory indices
        if (!src) return 0;
    }
    unsigned mx = 0;
    for (int i = 0; i < count; i++) {
        unsigned v = isShort ? *(const unsigned short*)(src + i*2)
                             : *(const unsigned*)(src + i*4);
        if (v > mx) mx = v;
    }
    return (int)mx + 1;
}

static void SkinCensusDraw(GLenum mode, GLsizei count, GLenum type,
                           const void* indices, GLint vpId) {
    (void)mode;
    if (g_nSkin >= SKIN_MAX) return;
    // Attribute 0 is position everywhere in this engine; its stride is the
    // per-vertex byte size the GPU was actually fed.
    GLint stride = 0, vsize = 0, vtype = 0, nAttr = 0;
    if (p_getVAiv) {
        p_getVAiv(0, 0x8624 /*ARRAY_STRIDE*/, &stride);
        p_getVAiv(0, 0x8623 /*ARRAY_SIZE*/,   &vsize);
        p_getVAiv(0, 0x8625 /*ARRAY_TYPE*/,   &vtype);
        for (int a = 0; a < 16; a++) {
            GLint en = 0;
            p_getVAiv(a, 0x8622 /*ARRAY_ENABLED*/, &en);
            if (en) nAttr++;
        }
    }
    for (int i = 0; i < g_nSkin; i++)          // one row per distinct shape
        if (g_skin[i].prog == vpId && g_skin[i].count == (int)count &&
            g_skin[i].stride == stride) return;
    g_skin[g_nSkin].prog   = vpId;
    g_skin[g_nSkin].count  = (int)count;
    g_skin[g_nSkin].stride = stride;
    g_skin[g_nSkin].vsize  = vsize;
    g_skin[g_nSkin].vtype  = vtype;
    g_skin[g_nSkin].nAttr  = nAttr;
    g_skin[g_nSkin].verts  = MaxIndexPlusOne(count, type, indices);
    g_nSkin++;
}

// ---- mesh dump: the decoded geometry, straight off the GPU -----------------
// The .smb mesh blobs are encoded in a way not yet cracked, but the ENGINE
// decodes them every frame into vertex buffers. Reading those gives real
// geometry now, and the viewer that consumes the dump is still standalone -
// the capture is a one-time extraction, like ripping.
//
// File format (little-endian), written to bin\swse_meshes.odv:
//   'ODVM' u32 magic, u32 version=4, u32 meshCount
//   per mesh: u32 prog, verts, indices, stride, vbo, texId
//             u32 frame, nLocals, haveUV, 0
//             u32 attr[4][4]                size, type, offset, enabled
//             f32 locals[nLocals*4]         the POSE for this frame
//             u8  vertex[verts*stride]      bind pose, constant across frames
//             f32 uv[verts*2]               if haveUV
//             u16 idx[indices]              triangle strip
//
// v4 adds the two things a static viewer could never have: the per-frame pose,
// and real texture coordinates.
static int g_meshDump = 0;
static FILE* g_dumpFile = 0;
static int g_dumpCount = 0;
static long g_dumpCountPos = 0;
static int g_dumpFrame = 0;

// Program locals captured per draw. The skinning program indexes bones as
// c[A0 + 8] and c[A0 + 9] and references up to c[174], so 192 covers the whole
// bank with room to spare.
#define DUMP_LOCALS 192

// Textures already written this session, so each is emitted once.
#define DUMP_TEX_MAX 512
static unsigned g_dumpTex[DUMP_TEX_MAX];
static int g_nDumpTex = 0;

// Write the bound texture as raw BGRA into swse_meshes.tex, keyed by GL id.
// Format: u32 id, u32 w, u32 h, then w*h*4 bytes. The viewer pairs these with
// meshes by id.
static FILE* g_texFile = 0;
static void DumpTexture(unsigned texId) {
    if (!g_texFile || !texId) return;
    for (int i = 0; i < g_nDumpTex; i++) if (g_dumpTex[i] == texId) return;
    if (g_nDumpTex >= DUMP_TEX_MAX) return;

    GLint prevTex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glBindTexture(GL_TEXTURE_2D, texId);
    GLint w = 0, h = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, 0x1000 /*TEXTURE_WIDTH*/,  &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, 0x1001 /*TEXTURE_HEIGHT*/, &h);
    if (w > 0 && h > 0 && w <= 2048 && h <= 2048) {
        size_t n = (size_t)w * h * 4;
        unsigned char* px = (unsigned char*)malloc(n);
        if (px) {
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
            unsigned hdr[3] = { texId, (unsigned)w, (unsigned)h };
            fwrite(hdr, 4, 3, g_texFile);
            fwrite(px, 1, n, g_texFile);
            free(px);
            g_dumpTex[g_nDumpTex++] = texId;
        }
    }
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
}

static void DumpOneMesh(GLsizei count, GLenum type, const void* indices, GLint vpId) {
    if (!g_dumpFile || !p_getVAiv || !p_getBufParam || !p_getBufSubData) return;
    if (count <= 0 || count > 200000) return;
    if (type != 0x1403 /*UNSIGNED_SHORT*/) return;      // 16-bit indices only

    GLint stride = 0, vsize = 0, vtype = 0, vbo = 0;
    p_getVAiv(0, 0x8624 /*ARRAY_STRIDE*/, &stride);
    p_getVAiv(0, 0x8623 /*ARRAY_SIZE*/,   &vsize);
    p_getVAiv(0, 0x8625 /*ARRAY_TYPE*/,   &vtype);
    p_getVAiv(0, 0x889F /*ARRAY_BUFFER_BINDING*/, &vbo);
    if (vtype != 0x1402 /*GL_SHORT*/ || vsize != 3 || stride < 6) return;
    if (!vbo) return;

    // indices first, to learn the vertex count
    static unsigned short idx[200000];
    GLint ebo = 0;
    glGetIntegerv(0x8895 /*ELEMENT_ARRAY_BUFFER_BINDING*/, &ebo);
    if (!ebo) return;
    GLint eboSize = 0;
    p_getBufParam(0x8893, 0x8764 /*BUFFER_SIZE*/, &eboSize);
    size_t ioff = (size_t)indices, ineed = (size_t)count * 2;
    if (ioff + ineed > (size_t)eboSize) return;
    p_getBufSubData(0x8893, (GLint)ioff, (GLsizei)ineed, idx);

    unsigned maxi = 0;
    for (int i = 0; i < count; i++) if (idx[i] > maxi) maxi = idx[i];
    int nverts = (int)maxi + 1;
    if (nverts <= 0 || nverts > 100000) return;

    // attribute 0's base pointer is its offset within the bound VBO
    GLvoid* aptr = 0;
    if (p_getVAptr) p_getVAptr(0, 0x8645 /*ARRAY_POINTER*/, &aptr);
    GLint vboSize = 0;
    p_getBufParam(0x8892 /*ARRAY_BUFFER*/, 0x8764, &vboSize);
    size_t need = (size_t)(nverts - 1) * stride + 6;    // last vertex needs only its 6 bytes
    size_t base = (size_t)aptr;
    if (base + need > (size_t)vboSize) return;

    // Whole vertices, not just positions: the UV lanes live in the other 12
    // bytes and are needed for texturing.
    //
    // The LAST vertex does not occupy a full stride. Asking for
    // nverts * stride overruns the buffer, glGetBufferSubData then writes
    // NOTHING, and every draw is silently rejected - which is exactly the bug
    // recorded in GRAPHICS_RTGI.md for the world harvest ("want offset 4 + 80
    // bytes, BUFFER IS 80 bytes", 882 of 884 draws lost). Same mistake, same
    // fix: ask for what is actually there.
    static unsigned char vbuf[100000 * 64];
    need = (size_t)nverts * stride;
    if (base + need > (size_t)vboSize) need = (size_t)vboSize - base;
    if (need > sizeof(vbuf) || need < (size_t)(nverts - 1) * stride + 6) return;
    p_getBufSubData(0x8892, (GLint)base, (GLsizei)need, vbuf);
    size_t haveBytes = need;

    // Attribute descriptors, so the viewer can find UVs instead of guessing.
    unsigned attr[4][4];      // size, type, offset-from-attr0, enabled
    for (int a = 0; a < 4; a++) {
        GLint asz = 0, aty = 0, aen = 0;
        GLvoid* ap = 0;
        p_getVAiv(a, 0x8623 /*ARRAY_SIZE*/, &asz);
        p_getVAiv(a, 0x8625 /*ARRAY_TYPE*/, &aty);
        p_getVAiv(a, 0x8622 /*ARRAY_ENABLED*/, &aen);
        if (p_getVAptr) p_getVAptr(a, 0x8645 /*ARRAY_POINTER*/, &ap);
        attr[a][0] = (unsigned)asz;
        attr[a][1] = (unsigned)aty;
        attr[a][2] = (unsigned)((size_t)ap >= base ? (size_t)ap - base : 0);
        attr[a][3] = (unsigned)aen;
    }

    GLint texId = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texId);
    DumpTexture((unsigned)texId);

    // ---- the animation ------------------------------------------------------
    // The vertex buffer holds the BIND POSE and never changes; the movement is
    // in the program locals, which the engine rewrites every frame. Measured
    // from the skinning program:
    //
    //   MAD R0.xy, vertex.attrib[1], c[7].y, c[7].x   bone indices, scaled
    //   ARL A0.x, R5.x                                address register
    //   MUL R4.xyz, R3.zxyw, c[A0.x + 9].yzxw         cross product, i.e.
    //   MAD R3.xyz, R3.yzxw, c[A0.x + 9].zxyw, -R4    quaternion rotate
    //   MUL R1, vertex.attrib[1].z, R1                weights are in .zw
    //
    // So it is two-bone quaternion skinning: attrib[1].xy select bones,
    // attrib[1].zw weight them, and each bone occupies TWO vec4 locals. Capture
    // the locals every frame and the pose is recoverable without touching
    // Granny or the .gr2 files.
    float locals[DUMP_LOCALS * 4];
    unsigned nLocals = 0;
    if (p_getProgLocal) {
        nLocals = DUMP_LOCALS;
        for (unsigned i = 0; i < nLocals; i++)
            p_getProgLocal(0x8620 /*VERTEX_PROGRAM_ARB*/, i, &locals[i * 4]);
    }

    // ---- texture coordinates ------------------------------------------------
    // Not a generic attribute: this engine feeds UVs through fixed-function
    // glTexCoordPointer, which is why scanning attribs 1..3 for a 2-component
    // lane never found them and every capture came out untextured.
    static float uv[100000 * 2];
    unsigned haveUV = 0;
    {
        GLint tcSize = 0, tcType = 0, tcStride = 0, tcVbo = 0;
        GLvoid* tcPtr = 0;
        glGetIntegerv(0x8088 /*TEXTURE_COORD_ARRAY_SIZE*/,   &tcSize);
        glGetIntegerv(0x8089 /*TEXTURE_COORD_ARRAY_TYPE*/,   &tcType);
        glGetIntegerv(0x808A /*TEXTURE_COORD_ARRAY_STRIDE*/, &tcStride);
        glGetIntegerv(0x889A /*TEXCOORD_ARRAY_BUFFER_BINDING*/, &tcVbo);
        glGetPointerv(0x8092 /*TEXTURE_COORD_ARRAY_POINTER*/, &tcPtr);
        if (glIsEnabled(0x8078 /*TEXTURE_COORD_ARRAY*/) && tcSize >= 2 && tcVbo) {
            int esz = (tcType == 0x1406 /*FLOAT*/) ? 4 :
                      (tcType == 0x1402 /*SHORT*/) ? 2 : 0;
            int tstride = tcStride ? tcStride : esz * tcSize;
            if (esz && tstride > 0) {
                GLint tvboSize = 0;
                GLint prevArr = 0;
                glGetIntegerv(0x8894 /*ARRAY_BUFFER_BINDING*/, &prevArr);
                p_bindBuffer(0x8892, (GLuint)tcVbo);
                p_getBufParam(0x8892, 0x8764 /*BUFFER_SIZE*/, &tvboSize);
                size_t tbase = (size_t)tcPtr;
                size_t tneed = (size_t)(nverts - 1) * tstride + esz * 2;
                if (tbase + tneed <= (size_t)tvboSize && tneed <= sizeof(vbuf)) {
                    static unsigned char tbuf[100000 * 16];
                    if (tneed <= sizeof(tbuf)) {
                        p_getBufSubData(0x8892, (GLint)tbase, (GLsizei)tneed, tbuf);
                        for (int v = 0; v < nverts; v++) {
                            const unsigned char* s = tbuf + (size_t)v * tstride;
                            if (tcType == 0x1406) {
                                uv[v * 2 + 0] = ((const float*)s)[0];
                                uv[v * 2 + 1] = ((const float*)s)[1];
                            } else {
                                uv[v * 2 + 0] = ((const short*)s)[0] / 32767.0f;
                                uv[v * 2 + 1] = ((const short*)s)[1] / 32767.0f;
                            }
                        }
                        haveUV = 1;
                    }
                }
                p_bindBuffer(0x8892, (GLuint)prevArr);
            }
        }
    }

    // VBO id groups a character's PARTS. Head, torso and limbs are separate
    // draws out of one vertex buffer, so the buffer id is the natural "this is
    // one model" key - and without it the viewer normalises each part into its
    // own box and the character comes apart.
    unsigned hdr[6] = { (unsigned)vpId, (unsigned)nverts, (unsigned)count,
                        (unsigned)stride, (unsigned)vbo, (unsigned)texId };
    fwrite(hdr, 4, 6, g_dumpFile);
    // v4: which frame this draw belongs to, the pose, and whether UVs follow.
    unsigned ext[4] = { (unsigned)g_dumpFrame, nLocals, haveUV, 0 };
    fwrite(ext, 4, 4, g_dumpFile);
    fwrite(attr, 4, 16, g_dumpFile);
    if (nLocals) fwrite(locals, 4, (size_t)nLocals * 4, g_dumpFile);
    // Pad the tail so the reader can assume nverts * stride bytes.
    static unsigned char pad[64];
    fwrite(vbuf, 1, haveBytes, g_dumpFile);
    size_t want = (size_t)nverts * stride;
    if (want > haveBytes) {
        memset(pad, 0, sizeof(pad));
        size_t rem = want - haveBytes;
        while (rem) { size_t k = rem > sizeof(pad) ? sizeof(pad) : rem;
                      fwrite(pad, 1, k, g_dumpFile); rem -= k; }
    }
    if (haveUV) fwrite(uv, 4, (size_t)nverts * 2, g_dumpFile);
    fwrite(idx, 2, (size_t)count, g_dumpFile);
    g_dumpCount++;
}

// One dump frame. Called from FrameMark so every draw in a frame carries the
// same number and the viewer can replay them in order.
void SWSE_MeshDumpFrameMark() { if (g_meshDump) g_dumpFrame++; }
int  SWSE_MeshDumpFrames()    { return g_dumpFrame; }

int SWSE_MeshDumpStart(char* pathOut, int pathLen) {
    char path[MAX_PATH];
    GetModuleFileNameA(GetModuleHandleA(NULL), path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) *(slash + 1) = 0;
    lstrcatA(path, "swse_meshes.odv");
    g_dumpFile = fopen(path, "wb");
    if (!g_dumpFile) return 0;
    // Companion texture file, same base name.
    char tpath[MAX_PATH];
    lstrcpynA(tpath, path, MAX_PATH);
    char* dot = strrchr(tpath, '.');
    if (dot) lstrcpyA(dot, ".tex");
    g_texFile = fopen(tpath, "wb");
    g_nDumpTex = 0;

    unsigned magic = 0x4D56444F;   // 'ODVM'
    unsigned ver = 4, zero = 0;    // v4: frame number, pose locals, real UVs
    g_dumpFrame = 0;
    fwrite(&magic, 4, 1, g_dumpFile);
    fwrite(&ver, 4, 1, g_dumpFile);
    g_dumpCountPos = ftell(g_dumpFile);
    fwrite(&zero, 4, 1, g_dumpFile);    // patched on finish
    g_dumpCount = 0;
    g_meshDump = 1;
    if (pathOut) lstrcpynA(pathOut, path, pathLen);
    return 1;
}

int SWSE_MeshDumpFinish() {
    if (!g_dumpFile) return 0;
    g_meshDump = 0;
    fseek(g_dumpFile, g_dumpCountPos, SEEK_SET);
    unsigned n = (unsigned)g_dumpCount;
    fwrite(&n, 4, 1, g_dumpFile);
    fclose(g_dumpFile);
    g_dumpFile = 0;
    if (g_texFile) { fclose(g_texFile); g_texFile = 0; }
    return g_dumpCount;
}

void SWSE_SkinCensusArm() {
    g_nSkin = 0; g_skinCensus = 1; g_maxLocals = 0;
    g_clsCount[0] = g_clsCount[1] = g_clsCount[2] = 0;
}
void SWSE_SkinCensusReport(SWSE_MatEmit emit) {
    if (!emit) return;
    char b[200];
    g_skinCensus = 0;
    wsprintfA(b, "skin: draws by class - 0(not world)=%d  1(static)=%d  2(skinned)=%d",
              g_clsCount[0], g_clsCount[1], g_clsCount[2]);
    emit(b);
    wsprintfA(b, "skin: most locals on any bound program = %d (class 2 needs >64)",
              g_maxLocals);
    emit(b);
    wsprintfA(b, "skin: %d distinct skinned draw shapes", g_nSkin);
    emit(b);
    emit("skin:  prog  indices   VERTS  stride  attribs   verts*36   verts*18");
    for (int i = 0; i < g_nSkin; i++) {
        int v = g_skin[i].verts;
        wsprintfA(b, "skin: %5d %8d %7d %7d %6d %10d %10d",
                  g_skin[i].prog, g_skin[i].count, v, g_skin[i].stride,
                  g_skin[i].nAttr, v * 36, v * 18);
        emit(b);
    }
    // The .smb mesh blobs for npc_0 divide by 36 into these record counts.
    emit("skin: npc_0.smb mesh record counts were 514 201 573 526 557 166 ...");
    emit("skin: a MATCH means 36 = bytes per vertex; no match means 36 is");
    emit("skin: something else (keyframe, strip run) and meshes live elsewhere.");
}

// ---- hidden character parts ------------------------------------------------
// A character is drawn as several skinned draws - hat, poncho, body, and so on
// are separate. Dropping one before it reaches the driver removes that piece
// and leaves the rest of the character intact.
//
// The key is the INDEX COUNT: it is fixed per mesh and already visible in the
// 'skin show' table, so a part can be identified and hidden without any new
// probe. Deliberately general - 'hidepart <n>' works on any piece; noponcho
// and nohat are named wrappers over the counts that were measured in play.
#define MAX_HIDE 16
static unsigned g_hideCount[MAX_HIDE];
static int g_nHide = 0;

int SWSE_MaterialsHidePart(unsigned indexCount, int on) {
    for (int i = 0; i < g_nHide; i++) {
        if (g_hideCount[i] == indexCount) {
            if (!on) g_hideCount[i] = g_hideCount[--g_nHide];
            return g_nHide;
        }
    }
    if (on && g_nHide < MAX_HIDE) g_hideCount[g_nHide++] = indexCount;
    return g_nHide;
}
void SWSE_MaterialsHideClear() { g_nHide = 0; }
int  SWSE_MaterialsHideList(unsigned* out, int maxOut) {
    int n = g_nHide < maxOut ? g_nHide : maxOut;
    for (int i = 0; i < n; i++) out[i] = g_hideCount[i];
    return n;
}
int  SWSE_MaterialsHidden(unsigned indexCount) {
    for (int i = 0; i < g_nHide; i++) if (g_hideCount[i] == indexCount) return 1;
    return 0;
}

static void APIENTRY HookedDrawElements(GLenum mode, GLsizei count, GLenum type,
                                        const void* indices) {
    // Checked BEFORE the trampoline - the draw has to be dropped, not undone.
    //
    // NOT restricted to class 2. The first version required it, and the hidden
    // part went on casting a shadow: the shadow pass draws the same mesh with a
    // different, simpler vertex program that has too few locals to classify as
    // skinned. Matching on index count alone catches every pass the mesh
    // appears in, which is what "remove it" has to mean.
    if (g_nHide && !g_suspend && SWSE_MaterialsHidden((unsigned)count))
        return;
    g_drawTramp(mode, count, type, indices);
    g_drawsSeen++;
    if (!g_suspend) {
        if (g_stampOn && g_curRefl)
            MaskRedraw(mode, count, type, indices);
        if (g_gbufOn && p_getProgiv)
            GBufRedraw(mode, count, type, indices);
        // Geometry probe: only static world draws, only while armed. Resolves
        // its own entry points - it must work with the mask and G-buffer both
        // off, which is exactly the state that made the first run capture 0.
        if (SWSE_GeoArmed()) {
            if (!p_getProgiv) EnsureFboFuncs();
            if (p_getProgiv && ClassifyDraw(0) == 1)
                SWSE_GeoCaptureDraw(mode, count, type, indices);
        }
        // SKINNED census, for the Oddview format work. The geometry probe
        // above takes class 1 (static world) only; characters are class 2 and
        // are deliberately skipped there. This is the mirror image: record one
        // frame's CHARACTER draws so their vertex counts can be compared
        // against the record counts in the .smb mesh blobs (18504/36 = 514).
        // Counts are what decide whether 36 is bytes-per-vertex.
        // One lazy resolve for every consumer below. p_getProgiv is filled in
        // by EnsureFboFuncs, which used to run only for the geometry probe - so
        // the census and the mesh dump both reported zero of everything unless
        // a harvest happened to have run first in the same session.
        if ((g_skinCensus > 0 || g_meshDump) && !p_getProgiv) EnsureFboFuncs();
        if (g_skinCensus > 0 && p_getProgiv) {
            GLint vpId = 0;
            int cls = ClassifyDraw(&vpId);
            // Tally EVERY class, not just 2. The first run of this probe
            // reported "0 skinned draws", which on its own cannot distinguish
            // "characters are not class 2" from "this hook never sees them".
            // The histogram tells those apart.
            if (cls >= 0 && cls <= 2) g_clsCount[cls]++;
            if (cls == 2) SkinCensusDraw(mode, count, type, indices, vpId);
        }
        // Mesh dump runs independently of the census: it needs every character
        // draw, not one row per distinct shape.
        //
        if (g_meshDump && p_getProgiv) {
            GLint vpId = 0;
            if (ClassifyDraw(&vpId) == 2) DumpOneMesh(count, type, indices, vpId);
        }
    }
}

// Same prologue discipline as the foliage bind hook: verify before patching,
// refuse and log rather than guess.
static int PrologueLen(const BYTE* t) {
    if (t[0] == 0x8B && t[1] == 0xFF && t[2] == 0x55 && t[3] == 0x8B && t[4] == 0xEC)
        return 5;   // mov edi,edi ; push ebp ; mov ebp,esp
    if (t[0] == 0x55 && t[1] == 0x8B && t[2] == 0xEC && t[3] == 0x83 && t[4] == 0xEC)
        return 6;   // push ebp ; mov ebp,esp ; sub esp,imm8
    if (t[0] == 0x55 && t[1] == 0x8B && t[2] == 0xEC &&
        (t[3] == 0x56 || t[3] == 0x57 || t[3] == 0x53))
        return 5;
    return 0;
}

static void InstallDrawHook() {
    if (g_drawHooked) return;
    HMODULE gl = GetModuleHandleA("opengl32.dll");
    if (!gl) { LogM("materials: opengl32 not loaded"); return; }
    BYTE* t = (BYTE*)GetProcAddress(gl, "glDrawElements");
    if (!t) { LogM("materials: glDrawElements not found"); return; }
    int len = PrologueLen(t);
    if (len == 0) {
        char b[160];
        wsprintfA(b, "materials: UNRECOGNISED glDrawElements prologue %02X %02X %02X %02X %02X %02X",
                  t[0], t[1], t[2], t[3], t[4], t[5]);
        LogM(b);
        return;
    }
    g_drawTrampMem = (BYTE*)VirtualAlloc(0, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_drawTrampMem) { LogM("materials: trampoline alloc failed"); return; }
    memcpy(g_drawTrampMem, t, len);
    g_drawTrampMem[len] = 0xE9;
    *(DWORD*)(g_drawTrampMem + len + 1) = (DWORD)((t + len) - (g_drawTrampMem + len + 5));
    g_drawTramp = (glDrawElements_t)g_drawTrampMem;

    DWORD old;
    VirtualProtect(t, len, PAGE_EXECUTE_READWRITE, &old);
    t[0] = 0xE9;
    *(DWORD*)(t + 1) = (DWORD)((BYTE*)HookedDrawElements - (t + 5));
    for (int i = 5; i < len; i++) t[i] = 0x90;
    VirtualProtect(t, len, old, &old);
    g_drawHooked = true;
    SWSE_HookNote(t, len, "materials", SWSE_HOOK_TRAMPOLINE, "glDrawElements");
    char b[120];
    wsprintfA(b, "materials: glDrawElements hooked (prologue %d bytes)", len);
    LogM(b);
}

// ---- public API -----------------------------------------------------------
void SWSE_MaterialsInit() {
    if (g_loaded) return;
    g_loaded = true;
    LoadList();
    InstallDrawHook();
}

// Flags are RECOMPUTED from recorded upload hashes, not cleared - same rule
// the foliage reload learned: clearing leaves every already-loaded texture
// unflagged until the next level change.
int SWSE_MaterialsReload() {
    LoadList();
    g_known = 0;
    if (g_hash) {
        for (int i = 0; i < MAX_TEXID; i++) {
            if (!g_hash[i]) continue;
            g_isRefl[i] = IsReflHash(g_hash[i]) ? 1 : 0;
            if (g_isRefl[i]) g_known++;
        }
    }
    return g_fpN;
}

void SWSE_MaterialsNoteUpload(unsigned hash, unsigned texid) {
    if (texid >= MAX_TEXID) return;
    if (!g_hash) {
        g_hash = (unsigned*)calloc(MAX_TEXID, sizeof(unsigned));
        if (!g_hash) return;
    }
    g_hash[texid] = hash;
    unsigned char was = g_isRefl[texid];
    unsigned char now = IsReflHash(hash) ? 1 : 0;
    if (was != now) {
        g_isRefl[texid] = now;
        g_known += now ? 1 : -1;
    }
}

// Bind tap: v2 only TRACKS. No GL state is touched here - that was v1's
// whole failure mode. The draw hook consumes g_curRefl.
void SWSE_MaterialsOnBind(unsigned texid) {
    g_curRefl = (texid < MAX_TEXID && g_isRefl[texid]) ? 1 : 0;
    if (g_curRefl) g_binds++;
}

void SWSE_MaterialsFrameMark() {
    g_marks++;
    SWSE_MeshDumpFrameMark();      // stamps this frame's draws for playback
    SWSE_GeoFrameEnd();            // closes a one-frame geometry capture
    g_bindsLast = g_binds;         g_binds = 0;
    g_redrawsLast = g_redraws;     g_redraws = 0;
    g_drawsSeenLast = g_drawsSeen; g_drawsSeen = 0;
    g_glslSkipsLast = g_glslSkips; g_glslSkips = 0;
    g_gbufDrawsLast = g_gbufDraws; g_gbufDraws = 0;
    g_skinSkipsLast = g_skinSkips; g_skinSkips = 0;
    g_vpLocalsSet = 0;             // fresh canonical VP next frame
    // Clear the G-buffer (color AND depth - both are ours alone).
    if (g_gbufOn && g_nrmFbo && !g_nrmBroken && p_bindFbo) {
        GLint prevFbo = 0;
        glGetIntegerv(0x8CA6, &prevFbo);
        glPushAttrib(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        p_bindFbo(0x8D40, g_nrmFbo);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glClearColor(0, 0, 0, 0);
        glClearDepth(1.0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        p_bindFbo(0x8D40, (GLuint)prevFbo);
        glPopAttrib();
    }
    g_suspend = 0;
    // Clear the mask for the new frame (color only - the depth attachment
    // is the game's living depth texture and must never be touched).
    if (g_stampOn && g_maskFbo && !g_fboBroken && p_bindFbo) {
        GLint prevFbo = 0;
        glGetIntegerv(0x8CA6, &prevFbo);
        glPushAttrib(GL_COLOR_BUFFER_BIT);
        p_bindFbo(0x8D40, g_maskFbo);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        p_bindFbo(0x8D40, (GLuint)prevFbo);
        glPopAttrib();
    }
}

void SWSE_MaterialsPassGuard(int inPass) { g_suspend = inPass ? 1 : 0; }
void SWSE_MaterialsSetStamp(int on)      { g_stampOn = on ? 1 : 0; }
void SWSE_MaterialsSetDepthTest(int on)  { g_depthTest = on ? 1 : 0; }

int SWSE_MaterialsMarks()          { return g_marks; }
int SWSE_MaterialsSuspended()      { return g_suspend; }
int SWSE_MaterialsListN()          { return g_fpN; }
int SWSE_MaterialsKnownTexids()    { return g_known; }
int SWSE_MaterialsBindsLastFrame() { return g_bindsLast; }
int SWSE_MaterialsStampState()     { return g_stampOn; }
unsigned SWSE_MaterialsMaskTex()   { return (g_fboBroken || !g_stampOn) ? 0 : g_maskTex; }
int SWSE_MaterialsRedrawsLastFrame() { return g_redrawsLast; }
int SWSE_MaterialsDrawsSeenLastFrame() { return g_drawsSeenLast; }
int SWSE_MaterialsGlslSkipsLastFrame() { return g_glslSkipsLast; }

void SWSE_MaterialsGBuf(int on, int w, int h) {
    g_gbufOn = on ? 1 : 0;
    g_gbufW = w; g_gbufH = h;
    // Called from the render pass with a GL context current - the one safe
    // place to resolve entry points when the mask path (which used to do
    // it as a side effect) is disabled. Unconditional: the geometry probe
    // needs these too, and it runs with every other stage switched off.
    if (!p_getProgiv) EnsureFboFuncs();
}
unsigned SWSE_MaterialsGBufTex() {
    return (g_gbufOn && !g_nrmBroken && g_nrmW > 0) ? g_nrmTex : 0;
}
int SWSE_MaterialsGBufDrawsLastFrame() { return g_gbufDrawsLast; }
int SWSE_MaterialsSkinSkipsLastFrame() { return g_skinSkipsLast; }
int SWSE_MaterialsDrawHooked()     { return g_drawHooked ? 1 : 0; }
