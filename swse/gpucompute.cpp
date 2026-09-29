// SWSE GPU compute probe - see gpucompute.h.
//
// Written in C style on purpose: the whole probe runs inside __try, and SEH
// cannot live in a function that needs C++ object unwinding. No std::, no
// destructors - plain buffers, malloc/free, wsprintfA.

#include "gpucompute.h"
#include <windows.h>
#include <gl/GL.h>
#include <stdlib.h>
#include <math.h>

#pragma comment(lib, "opengl32.lib")

// ---- GL constants this header predates -------------------------------------
#define GL_COMPUTE_SHADER                 0x91B9
#define GL_SHADER_STORAGE_BUFFER          0x90D2
#define GL_SHADER_STORAGE_BARRIER_BIT     0x00002000
#define GL_ALL_BARRIER_BITS               0xFFFFFFFF
#define GL_MAX_COMPUTE_WORK_GROUP_COUNT   0x91BE
#define GL_MAX_COMPUTE_WORK_GROUP_SIZE    0x91BF
#define GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS 0x90EB
#define GL_MAX_COMPUTE_SHARED_MEMORY_SIZE 0x8262
#define GL_MAX_SHADER_STORAGE_BLOCK_SIZE  0x90DE
#define GL_SHADING_LANGUAGE_VERSION       0x8B8C
#define GL_COMPILE_STATUS                 0x8B81
#define GL_LINK_STATUS                    0x8B82
#define GL_CURRENT_PROGRAM                0x8B8D
#define GL_DYNAMIC_DRAW                   0x88E8
#define GL_STATIC_DRAW                    0x88E4

typedef char GLchar_;
typedef ptrdiff_t GLsizeiptr_;
typedef ptrdiff_t GLintptr_;

typedef GLuint (APIENTRY* pfnCreateShader)(GLenum);
typedef void   (APIENTRY* pfnShaderSource)(GLuint, GLsizei, const GLchar_* const*, const GLint*);
typedef void   (APIENTRY* pfnCompileShader)(GLuint);
typedef void   (APIENTRY* pfnGetShaderiv)(GLuint, GLenum, GLint*);
typedef void   (APIENTRY* pfnGetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar_*);
typedef GLuint (APIENTRY* pfnCreateProgram)(void);
typedef void   (APIENTRY* pfnAttachShader)(GLuint, GLuint);
typedef void   (APIENTRY* pfnLinkProgram)(GLuint);
typedef void   (APIENTRY* pfnGetProgramiv)(GLuint, GLenum, GLint*);
typedef void   (APIENTRY* pfnGetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar_*);
typedef void   (APIENTRY* pfnUseProgram)(GLuint);
typedef void   (APIENTRY* pfnDeleteShader)(GLuint);
typedef void   (APIENTRY* pfnDeleteProgram)(GLuint);
typedef GLint  (APIENTRY* pfnGetUniformLocation)(GLuint, const GLchar_*);
typedef void   (APIENTRY* pfnUniform1i)(GLint, GLint);
typedef void   (APIENTRY* pfnGenBuffers)(GLsizei, GLuint*);
typedef void   (APIENTRY* pfnDeleteBuffers)(GLsizei, const GLuint*);
typedef void   (APIENTRY* pfnBindBuffer)(GLenum, GLuint);
typedef void   (APIENTRY* pfnBufferData)(GLenum, GLsizeiptr_, const void*, GLenum);
typedef void   (APIENTRY* pfnBindBufferBase)(GLenum, GLuint, GLuint);
typedef void   (APIENTRY* pfnGetBufferSubData)(GLenum, GLintptr_, GLsizeiptr_, void*);
typedef void   (APIENTRY* pfnDispatchCompute)(GLuint, GLuint, GLuint);
typedef void   (APIENTRY* pfnMemoryBarrier)(GLbitfield);
typedef void   (APIENTRY* pfnGetIntegeri_v)(GLenum, GLuint, GLint*);

static pfnCreateShader       p_CreateShader;
static pfnShaderSource       p_ShaderSource;
static pfnCompileShader      p_CompileShader;
static pfnGetShaderiv        p_GetShaderiv;
static pfnGetShaderInfoLog   p_GetShaderInfoLog;
static pfnCreateProgram      p_CreateProgram;
static pfnAttachShader       p_AttachShader;
static pfnLinkProgram        p_LinkProgram;
static pfnGetProgramiv       p_GetProgramiv;
static pfnGetProgramInfoLog  p_GetProgramInfoLog;
static pfnUseProgram         p_UseProgram;
static pfnDeleteShader       p_DeleteShader;
static pfnDeleteProgram      p_DeleteProgram;
static pfnGetUniformLocation p_GetUniformLocation;
static pfnUniform1i          p_Uniform1i;
static pfnGenBuffers         p_GenBuffers;
static pfnDeleteBuffers      p_DeleteBuffers;
static pfnBindBuffer         p_BindBuffer;
static pfnBufferData         p_BufferData;
static pfnBindBufferBase     p_BindBufferBase;
static pfnGetBufferSubData   p_GetBufferSubData;
static pfnDispatchCompute    p_DispatchCompute;
static pfnMemoryBarrier      p_MemoryBarrier;
static pfnGetIntegeri_v      p_GetIntegeri_v;

static void* GlProc(const char* name) {
    HMODULE gl = GetModuleHandleA("opengl32.dll");
    if (!gl) return 0;
    typedef PROC (WINAPI* wglGPA_t)(LPCSTR);
    static wglGPA_t wglGPA = 0;
    if (!wglGPA) wglGPA = (wglGPA_t)GetProcAddress(gl, "wglGetProcAddress");
    if (!wglGPA) return 0;
    void* p = (void*)wglGPA(name);
    // Modern entry points live in the ICD, but a few old ones are exported by
    // opengl32.dll itself and return null from wglGetProcAddress.
    if (!p) p = (void*)GetProcAddress(gl, name);
    return p;
}

// ---- stage 4: does compute run at all? -------------------------------------
static const char* kTrivialCs =
    "#version 430\n"
    "layout(local_size_x = 64) in;\n"
    "layout(std430, binding = 0) buffer Out { float data[]; };\n"
    "void main() {\n"
    "  uint i = gl_GlobalInvocationID.x;\n"
    "  data[i] = float(i) * 2.0 + 1.0;\n"
    "}\n";

// ---- stage 5: the actual ray tracing kernel --------------------------------
// Moller-Trumbore ray/triangle intersection - the primitive every tracer is
// built on. Brute force over all triangles here BY DESIGN: a BVH would make
// the throughput number flattering and unrepresentative of the worst case.
static const char* kRayCs =
    "#version 430\n"
    "layout(local_size_x = 64) in;\n"
    "layout(std430, binding = 0) buffer Tris { vec4 tri[]; };\n"
    "layout(std430, binding = 1) buffer Rays { vec4 ray[]; };\n"
    "layout(std430, binding = 2) buffer Hits { vec4 hit[]; };\n"
    "uniform int uTris;\n"
    "uniform int uRays;\n"
    "void main() {\n"
    "  uint gid = gl_GlobalInvocationID.x;\n"
    "  if (gid >= uint(uRays)) return;\n"
    "  vec3 ro = ray[gid * 2u].xyz;\n"
    "  vec3 rd = ray[gid * 2u + 1u].xyz;\n"
    "  float bestT = 1e30; float bestI = -1.0; float bu = 0.0; float bv = 0.0;\n"
    "  for (int t = 0; t < uTris; t++) {\n"
    "    vec3 v0 = tri[t * 3].xyz;\n"
    "    vec3 v1 = tri[t * 3 + 1].xyz;\n"
    "    vec3 v2 = tri[t * 3 + 2].xyz;\n"
    "    vec3 e1 = v1 - v0;\n"
    "    vec3 e2 = v2 - v0;\n"
    "    vec3 pv = cross(rd, e2);\n"
    "    float det = dot(e1, pv);\n"
    "    if (abs(det) < 1e-8) continue;\n"
    "    float inv = 1.0 / det;\n"
    "    vec3 tv = ro - v0;\n"
    "    float u = dot(tv, pv) * inv;\n"
    "    if (u < 0.0 || u > 1.0) continue;\n"
    "    vec3 qv = cross(tv, e1);\n"
    "    float v = dot(rd, qv) * inv;\n"
    "    if (v < 0.0 || u + v > 1.0) continue;\n"
    "    float tt = dot(e2, qv) * inv;\n"
    "    if (tt > 1e-4 && tt < bestT) { bestT = tt; bestI = float(t); bu = u; bv = v; }\n"
    "  }\n"
    "  hit[gid] = vec4(bestT, bestI, bu, bv);\n"
    "}\n";

// ---- reporting helpers -----------------------------------------------------
static SWSE_GpuEmit g_emit;
static char g_line[512];

static void Say(const char* s) { if (g_emit) g_emit(s); }

// wsprintfA has no float support; render as int.milli by hand.
static void SayF(const char* label, double v, const char* unit) {
    double a = v < 0 ? -v : v;
    int whole = (int)a;
    int milli = (int)((a - (double)whole) * 1000.0);
    wsprintfA(g_line, "%s%s%d.%03d %s", label, v < 0 ? "-" : "", whole, milli, unit);
    Say(g_line);
}

static GLuint BuildComputeProgram(const char* src, const char* tag) {
    GLuint sh = p_CreateShader(GL_COMPUTE_SHADER);
    if (!sh) { wsprintfA(g_line, "gpu: %s - glCreateShader(COMPUTE) returned 0", tag); Say(g_line); return 0; }
    p_ShaderSource(sh, 1, &src, 0);
    p_CompileShader(sh);
    GLint ok = 0;
    p_GetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512]; log[0] = 0;
        p_GetShaderInfoLog(sh, sizeof(log) - 1, 0, log);
        wsprintfA(g_line, "gpu: %s COMPILE FAILED: %s", tag, log);
        Say(g_line);
        p_DeleteShader(sh);
        return 0;
    }
    GLuint prog = p_CreateProgram();
    p_AttachShader(prog, sh);
    p_LinkProgram(prog);
    GLint linked = 0;
    p_GetProgramiv(prog, GL_LINK_STATUS, &linked);
    p_DeleteShader(sh);
    if (!linked) {
        char log[512]; log[0] = 0;
        p_GetProgramInfoLog(prog, sizeof(log) - 1, 0, log);
        wsprintfA(g_line, "gpu: %s LINK FAILED: %s", tag, log);
        Say(g_line);
        p_DeleteProgram(prog);
        return 0;
    }
    return prog;
}

// Deterministic pseudo-random so a rerun is comparable to the last run.
static unsigned g_rngState = 12345u;
static float Rnd() {
    g_rngState = g_rngState * 1664525u + 1013904223u;
    return (float)((g_rngState >> 8) & 0xFFFFFF) / (float)0x1000000;
}

static void ProbeInner() {
    Say("gpu: --- stage 1: what the driver claims ---");
    const char* ver = (const char*)glGetString(GL_VERSION);
    const char* ren = (const char*)glGetString(GL_RENDERER);
    const char* ven = (const char*)glGetString(GL_VENDOR);
    const char* sl  = (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION);
    wsprintfA(g_line, "gpu: GL_VERSION  : %s", ver ? ver : "(null)"); Say(g_line);
    wsprintfA(g_line, "gpu: GLSL        : %s", sl ? sl : "(null)");   Say(g_line);
    wsprintfA(g_line, "gpu: RENDERER    : %s", ren ? ren : "(null)"); Say(g_line);
    wsprintfA(g_line, "gpu: VENDOR      : %s", ven ? ven : "(null)"); Say(g_line);

    Say("gpu: --- stage 2: do the compute entry points exist? ---");
    p_CreateShader       = (pfnCreateShader)GlProc("glCreateShader");
    p_ShaderSource       = (pfnShaderSource)GlProc("glShaderSource");
    p_CompileShader      = (pfnCompileShader)GlProc("glCompileShader");
    p_GetShaderiv        = (pfnGetShaderiv)GlProc("glGetShaderiv");
    p_GetShaderInfoLog   = (pfnGetShaderInfoLog)GlProc("glGetShaderInfoLog");
    p_CreateProgram      = (pfnCreateProgram)GlProc("glCreateProgram");
    p_AttachShader       = (pfnAttachShader)GlProc("glAttachShader");
    p_LinkProgram        = (pfnLinkProgram)GlProc("glLinkProgram");
    p_GetProgramiv       = (pfnGetProgramiv)GlProc("glGetProgramiv");
    p_GetProgramInfoLog  = (pfnGetProgramInfoLog)GlProc("glGetProgramInfoLog");
    p_UseProgram         = (pfnUseProgram)GlProc("glUseProgram");
    p_DeleteShader       = (pfnDeleteShader)GlProc("glDeleteShader");
    p_DeleteProgram      = (pfnDeleteProgram)GlProc("glDeleteProgram");
    p_GetUniformLocation = (pfnGetUniformLocation)GlProc("glGetUniformLocation");
    p_Uniform1i          = (pfnUniform1i)GlProc("glUniform1i");
    p_GenBuffers         = (pfnGenBuffers)GlProc("glGenBuffers");
    p_DeleteBuffers      = (pfnDeleteBuffers)GlProc("glDeleteBuffers");
    p_BindBuffer         = (pfnBindBuffer)GlProc("glBindBuffer");
    p_BufferData         = (pfnBufferData)GlProc("glBufferData");
    p_BindBufferBase     = (pfnBindBufferBase)GlProc("glBindBufferBase");
    p_GetBufferSubData   = (pfnGetBufferSubData)GlProc("glGetBufferSubData");
    p_DispatchCompute    = (pfnDispatchCompute)GlProc("glDispatchCompute");
    p_MemoryBarrier      = (pfnMemoryBarrier)GlProc("glMemoryBarrier");
    p_GetIntegeri_v      = (pfnGetIntegeri_v)GlProc("glGetIntegeri_v");

    wsprintfA(g_line, "gpu: glDispatchCompute %s | glMemoryBarrier %s | SSBO bind %s",
              p_DispatchCompute ? "OK" : "MISSING",
              p_MemoryBarrier ? "OK" : "MISSING",
              p_BindBufferBase ? "OK" : "MISSING");
    Say(g_line);
    if (!p_DispatchCompute || !p_MemoryBarrier || !p_BindBufferBase ||
        !p_CreateShader || !p_GenBuffers || !p_BufferData || !p_GetBufferSubData) {
        Say("gpu: VERDICT - no compute on this context. Plan B would be a second");
        Say("gpu: shared context created with wglCreateContextAttribsARB.");
        return;
    }

    Say("gpu: --- stage 3: compute limits ---");
    GLint inv = 0, shared = 0;
    glGetIntegerv(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, &inv);
    glGetIntegerv(GL_MAX_COMPUTE_SHARED_MEMORY_SIZE, &shared);
    GLint cnt[3] = { 0, 0, 0 };
    if (p_GetIntegeri_v) {
        for (GLuint i = 0; i < 3; i++) p_GetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, i, &cnt[i]);
    }
    wsprintfA(g_line, "gpu: max invocations/group %d, shared mem %d bytes", inv, shared);
    Say(g_line);
    wsprintfA(g_line, "gpu: max work groups %d x %d x %d", cnt[0], cnt[1], cnt[2]);
    Say(g_line);

    // Save what the game had bound; the probe must be invisible to the frame.
    GLint savedProg = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &savedProg);
    while (glGetError() != GL_NO_ERROR) { }   // clear stale errors

    Say("gpu: --- stage 4: trivial compute dispatch ---");
    GLuint progA = BuildComputeProgram(kTrivialCs, "trivial cs");
    if (!progA) {
        Say("gpu: VERDICT - GLSL 4.30 compute did not compile here.");
        if (p_UseProgram) p_UseProgram((GLuint)savedProg);
        return;
    }
    const int N = 1024;
    GLuint bufA = 0;
    p_GenBuffers(1, &bufA);
    p_BindBuffer(GL_SHADER_STORAGE_BUFFER, bufA);
    p_BufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr_)(N * sizeof(float)), 0, GL_DYNAMIC_DRAW);
    p_BindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, bufA);
    p_UseProgram(progA);
    p_DispatchCompute(N / 64, 1, 1);
    p_MemoryBarrier(GL_ALL_BARRIER_BITS);

    float* back = (float*)malloc(N * sizeof(float));
    int trivialOk = 0;
    if (back) {
        for (int i = 0; i < N; i++) back[i] = -1.0f;
        p_GetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr_)(N * sizeof(float)), back);
        trivialOk = 1;
        for (int i = 0; i < N; i++) {
            float want = (float)i * 2.0f + 1.0f;
            float got = back[i];
            if (fabsf(got - want) > 0.001f) {
                wsprintfA(g_line, "gpu: MISMATCH at %d (expected %d, got %d)",
                          i, (int)want, (int)got);
                Say(g_line);
                trivialOk = 0;
                break;
            }
        }
        free(back);
    }
    GLenum err = glGetError();
    if (trivialOk) Say("gpu: PASS - 1024 values computed on the GPU and read back correct");
    else           Say("gpu: FAIL - dispatch ran but results are wrong");
    if (err != GL_NO_ERROR) { wsprintfA(g_line, "gpu: (GL error 0x%X during stage 4)", err); Say(g_line); }
    p_DeleteBuffers(1, &bufA);
    p_DeleteProgram(progA);
    if (!trivialOk) { p_UseProgram((GLuint)savedProg); return; }

    Say("gpu: --- stage 5: ray-triangle intersection (Moller-Trumbore) ---");
    GLuint progB = BuildComputeProgram(kRayCs, "ray cs");
    if (!progB) { p_UseProgram((GLuint)savedProg); return; }
    GLint locTris = p_GetUniformLocation(progB, "uTris");
    GLint locRays = p_GetUniformLocation(progB, "uRays");

    // --- 5a: correctness against hand-computed answers ---
    // Two triangles facing the camera at z=5 and z=10, plus rays whose hits
    // are known exactly. Anything but 5.0 / 3.0 / miss means broken math.
    float triC[2 * 3 * 4] = {
        -10, -10, 5, 0,   10, -10, 5, 0,   0, 10, 5, 0,
        -10, -10, 10, 0,  10, -10, 10, 0,  0, 10, 10, 0,
    };
    float rayC[3 * 2 * 4] = {
        0, 0, 0, 0,   0, 0, 1, 0,      // straight ahead -> hits tri 0 at t=5
        0, 0, 7, 0,   0, 0, 1, 0,      // past the first -> hits tri 1 at t=3
        0, 0, 0, 0,   0, 1, 0, 0,      // straight up    -> misses everything
    };
    float hitC[3 * 4];
    GLuint bufs[3] = { 0, 0, 0 };
    p_GenBuffers(3, bufs);
    p_BindBuffer(GL_SHADER_STORAGE_BUFFER, bufs[0]);
    p_BufferData(GL_SHADER_STORAGE_BUFFER, sizeof(triC), triC, GL_STATIC_DRAW);
    p_BindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, bufs[0]);
    p_BindBuffer(GL_SHADER_STORAGE_BUFFER, bufs[1]);
    p_BufferData(GL_SHADER_STORAGE_BUFFER, sizeof(rayC), rayC, GL_STATIC_DRAW);
    p_BindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, bufs[1]);
    p_BindBuffer(GL_SHADER_STORAGE_BUFFER, bufs[2]);
    p_BufferData(GL_SHADER_STORAGE_BUFFER, sizeof(hitC), 0, GL_DYNAMIC_DRAW);
    p_BindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, bufs[2]);

    p_UseProgram(progB);
    if (locTris >= 0) p_Uniform1i(locTris, 2);
    if (locRays >= 0) p_Uniform1i(locRays, 3);
    p_DispatchCompute(1, 1, 1);
    p_MemoryBarrier(GL_ALL_BARRIER_BITS);
    p_BindBuffer(GL_SHADER_STORAGE_BUFFER, bufs[2]);
    for (int i = 0; i < 12; i++) hitC[i] = -12345.0f;
    p_GetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(hitC), hitC);

    SayF("gpu: ray 0 hit distance (expect 5.000): ", hitC[0], "");
    SayF("gpu: ray 1 hit distance (expect 3.000): ", hitC[4], "");
    int miss = (hitC[9] < 0.0f);   // triangle index of ray 2 should stay -1
    wsprintfA(g_line, "gpu: ray 2 (aimed at empty space): %s", miss ? "correctly missed" : "FALSE HIT");
    Say(g_line);
    int rayOk = (fabsf(hitC[0] - 5.0f) < 0.01f) && (fabsf(hitC[4] - 3.0f) < 0.01f) && miss;
    Say(rayOk ? "gpu: PASS - real ray tracing math is correct on the GPU"
              : "gpu: FAIL - intersection results are wrong");

    // --- 5b: throughput, the number that decides feasibility ---
    if (rayOk) {
        Say("gpu: --- stage 6: throughput (brute force, no BVH) ---");
        const int NT = 128;            // triangles
        const int NR = 262144;         // rays (256K)
        float* tris = (float*)malloc((size_t)NT * 3 * 4 * sizeof(float));
        float* rays = (float*)malloc((size_t)NR * 2 * 4 * sizeof(float));
        if (tris && rays) {
            for (int t = 0; t < NT; t++) {
                float cx = Rnd() * 40.0f - 20.0f, cy = Rnd() * 40.0f - 20.0f, cz = Rnd() * 40.0f + 5.0f;
                for (int v = 0; v < 3; v++) {
                    tris[(t * 3 + v) * 4 + 0] = cx + Rnd() * 6.0f - 3.0f;
                    tris[(t * 3 + v) * 4 + 1] = cy + Rnd() * 6.0f - 3.0f;
                    tris[(t * 3 + v) * 4 + 2] = cz + Rnd() * 6.0f - 3.0f;
                    tris[(t * 3 + v) * 4 + 3] = 0.0f;
                }
            }
            for (int r = 0; r < NR; r++) {
                rays[(r * 2) * 4 + 0] = Rnd() * 4.0f - 2.0f;
                rays[(r * 2) * 4 + 1] = Rnd() * 4.0f - 2.0f;
                rays[(r * 2) * 4 + 2] = 0.0f;
                rays[(r * 2) * 4 + 3] = 0.0f;
                float dx = Rnd() * 2.0f - 1.0f, dy = Rnd() * 2.0f - 1.0f, dz = Rnd() * 0.5f + 0.5f;
                float len = sqrtf(dx * dx + dy * dy + dz * dz);
                rays[(r * 2 + 1) * 4 + 0] = dx / len;
                rays[(r * 2 + 1) * 4 + 1] = dy / len;
                rays[(r * 2 + 1) * 4 + 2] = dz / len;
                rays[(r * 2 + 1) * 4 + 3] = 0.0f;
            }
            p_BindBuffer(GL_SHADER_STORAGE_BUFFER, bufs[0]);
            p_BufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr_)((size_t)NT * 3 * 4 * sizeof(float)), tris, GL_STATIC_DRAW);
            p_BindBuffer(GL_SHADER_STORAGE_BUFFER, bufs[1]);
            p_BufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr_)((size_t)NR * 2 * 4 * sizeof(float)), rays, GL_STATIC_DRAW);
            p_BindBuffer(GL_SHADER_STORAGE_BUFFER, bufs[2]);
            p_BufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr_)((size_t)NR * 4 * sizeof(float)), 0, GL_DYNAMIC_DRAW);
            p_BindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, bufs[0]);
            p_BindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, bufs[1]);
            p_BindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, bufs[2]);
            if (locTris >= 0) p_Uniform1i(locTris, NT);
            if (locRays >= 0) p_Uniform1i(locRays, NR);

            // warm up first: the first dispatch pays shader/driver setup.
            p_DispatchCompute(NR / 64, 1, 1);
            p_MemoryBarrier(GL_ALL_BARRIER_BITS);
            glFinish();

            LARGE_INTEGER freq, t0, t1;
            QueryPerformanceFrequency(&freq);
            QueryPerformanceCounter(&t0);
            const int REPS = 4;
            for (int i = 0; i < REPS; i++) {
                p_DispatchCompute(NR / 64, 1, 1);
                p_MemoryBarrier(GL_ALL_BARRIER_BITS);
            }
            glFinish();
            QueryPerformanceCounter(&t1);

            double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
            double msPer = ms / (double)REPS;
            double tests = (double)NR * (double)NT;          // per dispatch
            double perSec = (msPer > 0.0) ? (tests / (msPer / 1000.0)) : 0.0;
            wsprintfA(g_line, "gpu: %d rays x %d triangles = %d million tests per dispatch",
                      NR, NT, (int)(tests / 1e6));
            Say(g_line);
            SayF("gpu: time per dispatch: ", msPer, "ms");
            wsprintfA(g_line, "gpu: throughput: %d million ray-triangle tests/second",
                      (int)(perSec / 1e6));
            Say(g_line);
            // What that means for a frame at 60 fps (16.6ms), spending 1/3 on tracing.
            double budget = perSec * 0.005;                  // 5 ms of tracing
            wsprintfA(g_line, "gpu: => ~%d million tests fit in a 5ms frame budget",
                      (int)(budget / 1e6));
            Say(g_line);
            // With a BVH, a ray costs ~20-40 tests instead of all N triangles.
            wsprintfA(g_line, "gpu: => at ~30 tests/ray with a BVH, that is ~%d million rays/frame",
                      (int)(budget / 30.0 / 1e6));
            Say(g_line);
        }
        if (tris) free(tris);
        if (rays) free(rays);
    }

    p_DeleteBuffers(3, bufs);
    p_DeleteProgram(progB);
    p_BindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    p_UseProgram((GLuint)savedProg);      // hand the game its shader back
    while (glGetError() != GL_NO_ERROR) { }
    Say("gpu: probe complete, game state restored");
}

void SWSE_GpuProbe(SWSE_GpuEmit emit) {
    g_emit = emit;
    __try {
        ProbeInner();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Say("gpu: FAULTED mid-probe - the driver rejected something. See how far");
        Say("gpu: the stage lines above got; that is the failing link.");
    }
    g_emit = 0;
}
