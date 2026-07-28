// SWSE shader reconnaissance - see shaderspy.h for why this exists.
//
// There is no GL call that lists every program, so both namespaces are probed
// by id. glIsProgram / glIsProgramARB are cheap and return false for ids that
// were never created, so a scan to a few thousand costs nothing and is run
// once on request rather than per frame.
//
// C-style throughout (no std::string, no iostream): the whole dump runs inside
// __try, and MSVC forbids __try in a function that needs object unwinding.

#include "shaderspy.h"
#include <windows.h>
#include <gl/GL.h>
#include <stdio.h>
#include <string.h>

// ---- GL constants we need that gl.h (GL 1.1) does not declare -------------
#define GL_VERTEX_SHADER            0x8B31
#define GL_FRAGMENT_SHADER          0x8B30
#define GL_ATTACHED_SHADERS         0x8B85
#define GL_SHADER_TYPE              0x8B4F
#define GL_SHADER_SOURCE_LENGTH     0x8B88
#define GL_VERTEX_PROGRAM_ARB       0x8620
#define GL_FRAGMENT_PROGRAM_ARB     0x8804
#define GL_PROGRAM_LENGTH_ARB       0x8627
#define GL_PROGRAM_STRING_ARB       0x8628
#define GL_PROGRAM_BINDING_ARB      0x8677

typedef char GLchar;

typedef GLboolean (APIENTRY* PFN_ISPROGRAM)(GLuint);
typedef GLboolean (APIENTRY* PFN_ISSHADER)(GLuint);
typedef void      (APIENTRY* PFN_GETPROGRAMIV)(GLuint, GLenum, GLint*);
typedef void      (APIENTRY* PFN_GETATTACHEDSHADERS)(GLuint, GLsizei, GLsizei*, GLuint*);
typedef void      (APIENTRY* PFN_GETSHADERIV)(GLuint, GLenum, GLint*);
typedef void      (APIENTRY* PFN_GETSHADERSOURCE)(GLuint, GLsizei, GLsizei*, GLchar*);
typedef GLboolean (APIENTRY* PFN_ISPROGRAMARB)(GLuint);
typedef void      (APIENTRY* PFN_BINDPROGRAMARB)(GLenum, GLuint);
typedef void      (APIENTRY* PFN_GETPROGRAMIVARB)(GLenum, GLenum, GLint*);
typedef void      (APIENTRY* PFN_GETPROGRAMSTRINGARB)(GLenum, GLenum, void*);

static PFN_ISPROGRAM           p_IsProgram;
static PFN_ISSHADER            p_IsShader;
static PFN_GETPROGRAMIV        p_GetProgramiv;
static PFN_GETATTACHEDSHADERS  p_GetAttachedShaders;
static PFN_GETSHADERIV         p_GetShaderiv;
static PFN_GETSHADERSOURCE     p_GetShaderSource;
static PFN_ISPROGRAMARB        p_IsProgramARB;
static PFN_BINDPROGRAMARB      p_BindProgramARB;
static PFN_GETPROGRAMIVARB     p_GetProgramivARB;
static PFN_GETPROGRAMSTRINGARB p_GetProgramStringARB;
static bool g_resolved = false;

static char          g_path[MAX_PATH];
static volatile LONG g_pending = 0;
static int g_nGlslProg = 0, g_nGlslSh = 0, g_nArb = 0, g_nFixedMatrix = 0, g_done = 0;

// How far to scan the id space. GL hands out small ids; a few thousand covers
// any real engine and an id that was never created just returns false.
#define SCAN_MAX 8192
#define SRC_MAX  (256 * 1024)

static void Resolve() {
    if (g_resolved) return;
    g_resolved = true;
    HMODULE gl = GetModuleHandleA("opengl32.dll");
    if (!gl) return;
    typedef PROC (WINAPI* wglGPA_t)(LPCSTR);
    wglGPA_t gpa = (wglGPA_t)GetProcAddress(gl, "wglGetProcAddress");
    if (!gpa) return;
    p_IsProgram           = (PFN_ISPROGRAM)gpa("glIsProgram");
    p_IsShader            = (PFN_ISSHADER)gpa("glIsShader");
    p_GetProgramiv        = (PFN_GETPROGRAMIV)gpa("glGetProgramiv");
    p_GetAttachedShaders  = (PFN_GETATTACHEDSHADERS)gpa("glGetAttachedShaders");
    p_GetShaderiv         = (PFN_GETSHADERIV)gpa("glGetShaderiv");
    p_GetShaderSource     = (PFN_GETSHADERSOURCE)gpa("glGetShaderSource");
    p_IsProgramARB        = (PFN_ISPROGRAMARB)gpa("glIsProgramARB");
    p_BindProgramARB      = (PFN_BINDPROGRAMARB)gpa("glBindProgramARB");
    p_GetProgramivARB     = (PFN_GETPROGRAMIVARB)gpa("glGetProgramivARB");
    p_GetProgramStringARB = (PFN_GETPROGRAMSTRINGARB)gpa("glGetProgramStringARB");
}

// Does this vertex program read the fixed-function matrix stack? That is the
// whole question: if it does, a modelview shear reaches the vertices.
static bool UsesFixedMatrix(const char* src) {
    return strstr(src, "state.matrix") != NULL
        || strstr(src, "ftransform") != NULL
        || strstr(src, "gl_ModelViewProjectionMatrix") != NULL
        || strstr(src, "gl_ModelViewMatrix") != NULL;
}

static void DumpBody() {
    FILE* f = fopen(g_path, "w");
    if (!f) return;

    const char* ver = (const char*)glGetString(GL_VERSION);
    const char* ven = (const char*)glGetString(GL_VENDOR);
    const char* ren = (const char*)glGetString(GL_RENDERER);
    fprintf(f, "SWSE shader dump\n");
    fprintf(f, "GL_VERSION : %s\n", ver ? ver : "(null)");
    fprintf(f, "GL_VENDOR  : %s\n", ven ? ven : "(null)");
    fprintf(f, "GL_RENDERER: %s\n\n", ren ? ren : "(null)");

    char* src = (char*)malloc(SRC_MAX);
    if (!src) { fclose(f); return; }

    // ---- GLSL ------------------------------------------------------------
    int nProg = 0, nSh = 0, nFixed = 0;
    if (p_IsProgram && p_GetShaderSource && p_GetShaderiv) {
        for (GLuint id = 1; id < SCAN_MAX; id++) {
            if (!p_IsProgram(id)) continue;
            nProg++;
            GLuint sh[16]; GLsizei n = 0;
            if (p_GetAttachedShaders) p_GetAttachedShaders(id, 16, &n, sh);
            fprintf(f, "=== GLSL PROGRAM %u  (%d attached) ===\n", id, (int)n);
            for (GLsizei i = 0; i < n; i++) {
                GLint type = 0, len = 0;
                p_GetShaderiv(sh[i], GL_SHADER_TYPE, &type);
                p_GetShaderiv(sh[i], GL_SHADER_SOURCE_LENGTH, &len);
                if (len <= 0 || len >= SRC_MAX) { fprintf(f, "  shader %u: no source\n", sh[i]); continue; }
                GLsizei got = 0;
                src[0] = 0;
                p_GetShaderSource(sh[i], SRC_MAX, &got, src);
                src[SRC_MAX - 1] = 0;
                const char* tn = (type == GL_VERTEX_SHADER) ? "VERTEX"
                               : (type == GL_FRAGMENT_SHADER) ? "FRAGMENT" : "OTHER";
                bool fixed = UsesFixedMatrix(src);
                if (type == GL_VERTEX_SHADER && fixed) nFixed++;
                fprintf(f, "--- shader %u %s  fixed-matrix=%s ---\n%s\n",
                        sh[i], tn, fixed ? "YES" : "no", src);
                nSh++;
            }
        }
    } else {
        fprintf(f, "(GLSL entry points unavailable)\n");
    }

    // ---- ARB assembly programs (Cg compiles to these) --------------------
    // Reading one requires binding it, so the current binding is saved first.
    int nArb = 0;
    if (p_IsProgramARB && p_BindProgramARB && p_GetProgramivARB && p_GetProgramStringARB) {
        const GLenum targets[2] = { GL_VERTEX_PROGRAM_ARB, GL_FRAGMENT_PROGRAM_ARB };
        const char*  tnames[2]  = { "ARB_VERTEX", "ARB_FRAGMENT" };
        for (int t = 0; t < 2; t++) {
            GLint prev = 0;
            glGetIntegerv(GL_PROGRAM_BINDING_ARB, &prev);
            while (glGetError() != GL_NO_ERROR) {}
            for (GLuint id = 1; id < SCAN_MAX; id++) {
                if (!p_IsProgramARB(id)) continue;
                p_BindProgramARB(targets[t], id);
                if (glGetError() != GL_NO_ERROR) continue;   // wrong target for this id
                GLint len = 0;
                p_GetProgramivARB(targets[t], GL_PROGRAM_LENGTH_ARB, &len);
                if (len <= 0 || len >= SRC_MAX) { while (glGetError() != GL_NO_ERROR) {} continue; }
                memset(src, 0, len + 1);
                p_GetProgramStringARB(targets[t], GL_PROGRAM_STRING_ARB, src);
                src[len] = 0;
                if (glGetError() != GL_NO_ERROR) continue;
                bool fixed = UsesFixedMatrix(src);
                if (t == 0 && fixed) nFixed++;
                fprintf(f, "=== %s PROGRAM %u  (%d bytes)  fixed-matrix=%s ===\n%s\n",
                        tnames[t], id, (int)len, fixed ? "YES" : "no", src);
                nArb++;
            }
            p_BindProgramARB(targets[t], (GLuint)prev);
            while (glGetError() != GL_NO_ERROR) {}
        }
    } else {
        fprintf(f, "(ARB program entry points unavailable)\n");
    }

    fprintf(f, "\nSUMMARY: glsl_programs=%d glsl_shaders=%d arb_programs=%d "
               "vertex_programs_using_fixed_matrix=%d\n", nProg, nSh, nArb, nFixed);
    free(src);
    fclose(f);

    g_nGlslProg = nProg; g_nGlslSh = nSh; g_nArb = nArb; g_nFixedMatrix = nFixed;
    g_done = 1;
}

void SWSE_ShaderDumpRequest(const char* path) {
    if (path && *path) {
        lstrcpynA(g_path, path, MAX_PATH);
    } else {
        GetModuleFileNameA(GetModuleHandleA(NULL), g_path, MAX_PATH);
        char* slash = strrchr(g_path, '\\');
        if (slash) *(slash + 1) = 0;
        lstrcatA(g_path, "swse_shaders.txt");
    }
    g_done = 0;
    InterlockedExchange(&g_pending, 1);
}

void SWSE_ShaderDumpService() {
    if (InterlockedCompareExchange(&g_pending, 0, 1) != 1) return;
    Resolve();
    // A driver fault here would take the game down; a failed dump must not.
    __try {
        DumpBody();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_done = -1;
    }
}

void SWSE_ShaderDumpStats(int* glslPrograms, int* glslShaders,
                          int* arbPrograms, int* usesFixedMatrix, int* done) {
    if (glslPrograms)   *glslPrograms   = g_nGlslProg;
    if (glslShaders)    *glslShaders    = g_nGlslSh;
    if (arbPrograms)    *arbPrograms    = g_nArb;
    if (usesFixedMatrix)*usesFixedMatrix= g_nFixedMatrix;
    if (done)           *done           = g_done;
}
