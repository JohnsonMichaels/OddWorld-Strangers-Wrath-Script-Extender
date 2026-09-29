// SWSE ray tracer - see raytrace.h.
//
// BVH: median split on the widest axis, built once over the harvested soup.
// Not a SAH build - this is the correctness milestone, and a simple structure
// that is provably right beats a clever one that is hard to debug. The compute
// throughput probe measured 62.9 billion ray-triangle tests/sec, so there is
// enormous headroom to spend on build quality later.

#include "raytrace.h"
#include "geocapture.h"
#include "scriptvm.h"   // SWSE_PosGet - to check where our matrix puts the player
#include "wind.h"
#include <windows.h>
#include <gl/GL.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#pragma comment(lib, "opengl32.lib")

static void LogR(const char* s) {
    char path[MAX_PATH];
    GetModuleFileNameA(GetModuleHandleA(NULL), path, MAX_PATH);
    char* sl = strrchr(path, '\\'); if (sl) *(sl + 1) = 0;
    lstrcatA(path, "swse_log.txt");
    FILE* f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "%s\n", s);
    fclose(f);
}

// ---- GL plumbing -----------------------------------------------------------
typedef ptrdiff_t GLsizeiptr_;
typedef char GLchar_;
typedef GLuint (APIENTRY* pfnCreateShader)(GLenum);
typedef void   (APIENTRY* pfnShaderSource)(GLuint, GLsizei, const GLchar_* const*, const GLint*);
typedef void   (APIENTRY* pfnCompileShader)(GLuint);
typedef void   (APIENTRY* pfnGetShaderiv)(GLuint, GLenum, GLint*);
typedef void   (APIENTRY* pfnGetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar_*);
typedef GLuint (APIENTRY* pfnCreateProgram)(void);
typedef void   (APIENTRY* pfnAttachShader)(GLuint, GLuint);
typedef void   (APIENTRY* pfnLinkProgram)(GLuint);
typedef void   (APIENTRY* pfnGetProgramiv)(GLuint, GLenum, GLint*);
typedef void   (APIENTRY* pfnUseProgram)(GLuint);
typedef void   (APIENTRY* pfnDeleteShader)(GLuint);
typedef GLint  (APIENTRY* pfnGetUniformLocation)(GLuint, const GLchar_*);
typedef void   (APIENTRY* pfnUniform1f)(GLint, GLfloat);
typedef void   (APIENTRY* pfnUniform2f)(GLint, GLfloat, GLfloat);
typedef void   (APIENTRY* pfnUniform3f)(GLint, GLfloat, GLfloat, GLfloat);
typedef void   (APIENTRY* pfnUniform1i)(GLint, GLint);
typedef void   (APIENTRY* pfnActiveTexture)(GLenum);
typedef void   (APIENTRY* pfnUniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat*);
typedef void   (APIENTRY* pfnGenBuffers)(GLsizei, GLuint*);
typedef void   (APIENTRY* pfnBindBuffer)(GLenum, GLuint);
typedef void   (APIENTRY* pfnBufferData)(GLenum, GLsizeiptr_, const void*, GLenum);
typedef void   (APIENTRY* pfnBindBufferBase)(GLenum, GLuint, GLuint);
typedef void   (APIENTRY* pfnDispatchCompute)(GLuint, GLuint, GLuint);
typedef void   (APIENTRY* pfnMemoryBarrier)(GLbitfield);
typedef void   (APIENTRY* pfnBindImageTexture)(GLuint, GLuint, GLint, GLboolean, GLint, GLenum, GLenum);

static pfnCreateShader p_CreateShader; static pfnShaderSource p_ShaderSource;
static pfnCompileShader p_CompileShader; static pfnGetShaderiv p_GetShaderiv;
static pfnGetShaderInfoLog p_GetShaderInfoLog; static pfnCreateProgram p_CreateProgram;
static pfnAttachShader p_AttachShader; static pfnLinkProgram p_LinkProgram;
static pfnGetProgramiv p_GetProgramiv; static pfnUseProgram p_UseProgram;
static pfnDeleteShader p_DeleteShader; static pfnGetUniformLocation p_GetUniformLocation;
static pfnUniform1f p_Uniform1f; static pfnUniform2f p_Uniform2f;
static pfnUniform3f p_Uniform3f;
static pfnUniform1i p_Uniform1i;
static pfnActiveTexture p_ActiveTexture;
static pfnUniformMatrix4fv p_UniformMatrix4fv; static pfnGenBuffers p_GenBuffers;
static pfnBindBuffer p_BindBuffer; static pfnBufferData p_BufferData;
static pfnBindBufferBase p_BindBufferBase; static pfnDispatchCompute p_DispatchCompute;
static pfnMemoryBarrier p_MemoryBarrier; static pfnBindImageTexture p_BindImageTexture;

static void* Gp(const char* n) {
    HMODULE gl = GetModuleHandleA("opengl32.dll");
    typedef PROC (WINAPI* w_t)(LPCSTR);
    static w_t wg = 0;
    if (!wg) wg = (w_t)GetProcAddress(gl, "wglGetProcAddress");
    void* p = wg ? (void*)wg(n) : 0;
    if (!p) p = (void*)GetProcAddress(gl, n);
    return p;
}

static int ResolveAll() {
    if (p_DispatchCompute) return 1;
    p_CreateShader=(pfnCreateShader)Gp("glCreateShader");
    p_ShaderSource=(pfnShaderSource)Gp("glShaderSource");
    p_CompileShader=(pfnCompileShader)Gp("glCompileShader");
    p_GetShaderiv=(pfnGetShaderiv)Gp("glGetShaderiv");
    p_GetShaderInfoLog=(pfnGetShaderInfoLog)Gp("glGetShaderInfoLog");
    p_CreateProgram=(pfnCreateProgram)Gp("glCreateProgram");
    p_AttachShader=(pfnAttachShader)Gp("glAttachShader");
    p_LinkProgram=(pfnLinkProgram)Gp("glLinkProgram");
    p_GetProgramiv=(pfnGetProgramiv)Gp("glGetProgramiv");
    p_UseProgram=(pfnUseProgram)Gp("glUseProgram");
    p_DeleteShader=(pfnDeleteShader)Gp("glDeleteShader");
    p_GetUniformLocation=(pfnGetUniformLocation)Gp("glGetUniformLocation");
    p_Uniform1f=(pfnUniform1f)Gp("glUniform1f");
    p_Uniform2f=(pfnUniform2f)Gp("glUniform2f");
    p_Uniform3f=(pfnUniform3f)Gp("glUniform3f");
    p_Uniform1i=(pfnUniform1i)Gp("glUniform1i");
    p_ActiveTexture=(pfnActiveTexture)Gp("glActiveTextureARB");
    if (!p_ActiveTexture) p_ActiveTexture=(pfnActiveTexture)Gp("glActiveTexture");
    p_UniformMatrix4fv=(pfnUniformMatrix4fv)Gp("glUniformMatrix4fv");
    p_GenBuffers=(pfnGenBuffers)Gp("glGenBuffers");
    p_BindBuffer=(pfnBindBuffer)Gp("glBindBuffer");
    p_BufferData=(pfnBufferData)Gp("glBufferData");
    p_BindBufferBase=(pfnBindBufferBase)Gp("glBindBufferBase");
    p_DispatchCompute=(pfnDispatchCompute)Gp("glDispatchCompute");
    p_MemoryBarrier=(pfnMemoryBarrier)Gp("glMemoryBarrier");
    p_BindImageTexture=(pfnBindImageTexture)Gp("glBindImageTexture");
    return p_DispatchCompute && p_BindImageTexture && p_BindBufferBase;
}

// ---- BVH -------------------------------------------------------------------
// GPU layout, 32 bytes/node: two vec4s. The ints ride in the .w lanes as raw
// bits (floatBitsToInt on the shader side) so the whole thing is one array.
typedef struct { float bmin[3]; int leftFirst; float bmax[3]; int count; } Node;

static Node*   g_nodes = 0;
static int     g_nNodes = 0, g_nodeCap = 0;
// 0 = packed only (loses the terrain), 1 = everything (readmits camera-attached
// overlay planes - measured negative), 2 = every DEPTH-WRITING draw. 2 is the
// default because it is the only one that asks the right question.
static int     g_useAll = 2;
// Defaults measured in play, not guessed. Marking is deliberately GENEROUS
// (any program, area > 2) because marking is now cheap: a misclassified solid
// just becomes slightly translucent. Under the old drop-it behaviour the same
// generosity deleted 33619 triangles and gutted the AO.
static float   g_cardSize = 2.0f;   // triangle area above which a triangle
static int     g_cardAny  = 1;      // counts as a leaf card
static float   g_cardOpacity = 0.12f;     // how solid a leaf card is to rays
static unsigned char* g_cardFlag = 0;     // per triangle, indexed like g_tris
static int     g_clipYNeg = 1;            // engine y flip; measure, don't assume
static int     g_errProbe = 0;            // 1 = read back and report camera error
// DEFAULT 0. At 1 this collapses the temporal reprojection to the IDENTITY:
// SWSE_RtAo sets useClip = g_prevVP, inverts it into uInvVP, then uploads THAT
// SAME array as uPrevVP - so uPrevVP == inverse(uInvVP), pc = uPrevVP*(inv*ndc)
// cancels, and with rtao_histflip 1 the history UV equals the current UV bit
// for bit. History is then fetched from the same SCREEN pixel every frame: at
// rtao_blend 0.12 that is ~88% of each pixel inherited from whatever was
// previously at that screen position, so occlusion is painted on the screen and
// drags across the world as the camera turns, settling when it stops.
// Three independent analyses derived this from the source. Introduced
// 2026-07-31 in 6f5102d, a commit about character vertex strides, defaulted ON
// and undocumented. Fixing it properly needs TWO stored matrices (pairing depth
// with the previous frame's camera was a real fix); until then, off.
static int     g_pairPrev = 0;            // unproject depth with LAST frame's VP
static int*    g_order = 0;          // triangle indices, reordered by the build
static const float* g_tris = 0;      // 9 floats per triangle (the harvest)
static int     g_nTris = 0;
static int     g_maxDepth = 0, g_traceTris = 0;
static double  g_buildMs = 0;
static int     g_built = 0;

static void TriBounds(int t, float* lo, float* hi) {
    const float* v = g_tris + (size_t)t * 9;
    for (int k = 0; k < 3; k++) { lo[k] = hi[k] = v[k]; }
    for (int i = 1; i < 3; i++)
        for (int k = 0; k < 3; k++) {
            float c = v[i * 3 + k];
            if (c < lo[k]) lo[k] = c;
            if (c > hi[k]) hi[k] = c;
        }
}

static void TriCentroid(int t, float* c) {
    const float* v = g_tris + (size_t)t * 9;
    for (int k = 0; k < 3; k++) c[k] = (v[k] + v[3 + k] + v[6 + k]) / 3.0f;
}

static int NewNode() {
    if (g_nNodes >= g_nodeCap) return -1;
    return g_nNodes++;
}

// Median split on the widest axis. Recursion depth is bounded, and a node is
// a leaf at <= 4 triangles or when the split degenerates.
static void Build(int node, int first, int count, int depth) {
    if (depth > g_maxDepth) g_maxDepth = depth;
    Node* n = &g_nodes[node];
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (int i = 0; i < count; i++) {
        float tl[3], th[3];
        TriBounds(g_order[first + i], tl, th);
        for (int k = 0; k < 3; k++) { if (tl[k] < lo[k]) lo[k] = tl[k]; if (th[k] > hi[k]) hi[k] = th[k]; }
    }
    for (int k = 0; k < 3; k++) { n->bmin[k] = lo[k]; n->bmax[k] = hi[k]; }

    if (count <= 4 || depth >= 40) { n->leftFirst = first; n->count = count; return; }

    int axis = 0;
    float ext = hi[0] - lo[0];
    if (hi[1] - lo[1] > ext) { axis = 1; ext = hi[1] - lo[1]; }
    if (hi[2] - lo[2] > ext) { axis = 2; ext = hi[2] - lo[2]; }
    float mid = (lo[axis] + hi[axis]) * 0.5f;

    int i = first, j = first + count - 1;
    while (i <= j) {
        float c[3];
        TriCentroid(g_order[i], c);
        if (c[axis] < mid) i++;
        else { int tmp = g_order[i]; g_order[i] = g_order[j]; g_order[j] = tmp; j--; }
    }
    int leftCount = i - first;
    if (leftCount == 0 || leftCount == count) leftCount = count / 2;   // degenerate split

    int l = NewNode(), r = NewNode();
    if (l < 0 || r < 0) { n->leftFirst = first; n->count = count; return; }
    n = &g_nodes[node];              // the array may have been indexed since
    n->leftFirst = l; n->count = 0;
    Build(l, first, leftCount, depth + 1);
    Build(r, first + leftCount, count - leftCount, depth + 1);
}

// ---- compute shader --------------------------------------------------------
static const char* kTraceCs =
"#version 430\n"
"layout(local_size_x = 8, local_size_y = 8) in;\n"
"layout(std430, binding = 0) buffer Nodes { vec4 nodes[]; };\n"
"layout(std430, binding = 1) buffer Tris  { vec4 tris[]; };\n"
"layout(std430, binding = 2) buffer Order { int  order[]; };\n"
"layout(rgba8, binding = 0) uniform writeonly image2D uOut;\n"
"uniform mat4  uInvVP;\n"
"uniform vec2  uRes;\n"
"uniform float uMaxDist;\n"
// Which NDC z is the near plane: -1 for OpenGL's convention, 0 for the
// D3D/console one. This engine is a port, so the convention is measured
// rather than assumed.
"uniform float uNearZ;\n"
// 0 = traced depth, 1 = ray direction as colour, 2 = ray origin spread.
// Looking at the rays themselves beats theorising about why they miss.
"uniform float uMode;\n"
"uniform vec3  uEye;\n"
"uniform float uFlipY;\n"
"bool slab(vec3 bmin, vec3 bmax, vec3 ro, vec3 inv, float tmax, out float tn){\n"
"  vec3 t0 = (bmin - ro) * inv, t1 = (bmax - ro) * inv;\n"
"  vec3 a = min(t0, t1), b = max(t0, t1);\n"
"  float lo = max(max(a.x, a.y), max(a.z, 0.0));\n"
"  float hi = min(min(b.x, b.y), min(b.z, tmax));\n"
"  tn = lo; return hi >= lo;\n"
"}\n"
"void main(){\n"
"  ivec2 px = ivec2(gl_GlobalInvocationID.xy);\n"
"  if (px.x >= int(uRes.x) || px.y >= int(uRes.y)) return;\n"
"  vec2 ndc = ((vec2(px) + 0.5) / uRes) * 2.0 - 1.0;\n"
// The compute image's row 0 and the composite's texture origin need not
// agree, and the engine already negates clip y once. One switch, measured
// rather than reasoned about.
"  if (uFlipY > 0.5) ndc.y = -ndc.y;\n"
// The eye is DERIVED on the CPU (where two screen rays intersect), never
// synthesised from a guessed near-plane depth. Every pixel's ray therefore
// starts at the real camera, and only its direction comes from unprojection.
"  vec4 pf = uInvVP * vec4(ndc, uNearZ, 1.0);\n"
"  vec3 ro = uEye;\n"
"  vec3 rd = normalize(pf.xyz / pf.w - ro);\n"
"  if (uMode > 0.5 && uMode < 1.5) { imageStore(uOut, px, vec4(rd * 0.5 + 0.5, 1.0)); return; }\n"
// Ray origin mapped from the level's own extent [-2600, 2600] to 0..1, so the
// colour reads directly as a world position: mid-grey means the origin sits at
// the world origin rather than at the camera.
"  if (uMode > 1.5 && uMode < 2.5) { imageStore(uOut, px, vec4((ro + vec3(2600.0)) / 5200.0, 1.0)); return; }\n"
"  vec3 inv = 1.0 / (rd + vec3(equal(rd, vec3(0.0))) * 1e-9);\n"
"  float best = uMaxDist;\n"
"  int visited = 0;\n"
"  int stack[48]; int sp = 0; stack[sp++] = 0;\n"
"  while (sp > 0) {\n"
"    visited++;\n"
"    int ni = stack[--sp];\n"
"    vec4 a = nodes[ni * 2], b = nodes[ni * 2 + 1];\n"
"    float tn;\n"
"    if (!slab(a.xyz, b.xyz, ro, inv, best, tn)) continue;\n"
"    int cnt = floatBitsToInt(b.w);\n"
"    int lf  = floatBitsToInt(a.w);\n"
"    if (cnt > 0) {\n"
"      for (int k = 0; k < cnt; k++) {\n"
"        int t = order[lf + k];\n"
"        vec3 v0 = tris[t * 3].xyz, v1 = tris[t * 3 + 1].xyz, v2 = tris[t * 3 + 2].xyz;\n"
"        vec3 e1 = v1 - v0, e2 = v2 - v0;\n"
"        vec3 pv = cross(rd, e2); float det = dot(e1, pv);\n"
"        if (abs(det) < 1e-9) continue;\n"
"        float id = 1.0 / det; vec3 tv = ro - v0;\n"
"        float u = dot(tv, pv) * id; if (u < 0.0 || u > 1.0) continue;\n"
"        vec3 qv = cross(tv, e1);\n"
"        float vv = dot(rd, qv) * id; if (vv < 0.0 || u + vv > 1.0) continue;\n"
"        float tt = dot(e2, qv) * id;\n"
"        if (tt > 0.01 && tt < best) best = tt;\n"
"      }\n"
"    } else if (sp < 46) { stack[sp++] = lf; stack[sp++] = lf + 1; }\n"
"  }\n"
// Mode 3: traversal heat. Black means the ray never even entered the root
// box - a ray/bounds problem. Bright means the tree was walked and the
// failure is in the triangles. This separates the two in one look.
"  if (uMode > 2.5) {\n"
"    float h = float(visited) / 60.0;\n"
"    imageStore(uOut, px, vec4(h, h * 0.4, 1.0 - h, 1.0)); return;\n"
"  }\n"
"  float shade = (best >= uMaxDist) ? 0.0 : (1.0 - best / uMaxDist);\n"
"  imageStore(uOut, px, vec4(shade, shade, shade, 1.0));\n"
"}\n";

// ---- STAGE 2: ray-traced ambient occlusion ---------------------------------
// No primary rays: the depth buffer already tells us where every pixel is in
// the world. We unproject it, rebuild a normal from neighbouring depths, and
// fire short occlusion rays into the BVH. Unlike screen-space AO these rays
// see geometry that is BEHIND other surfaces or entirely off-screen - the
// thing the owner has been waiting for since the V1/V2 comparison.
static const char* kAoCs =
"#version 430\n"
"layout(local_size_x = 8, local_size_y = 8) in;\n"
"layout(std430, binding = 0) buffer Nodes { vec4 nodes[]; };\n"
"layout(std430, binding = 1) buffer Tris  { vec4 tris[]; };\n"
"layout(std430, binding = 2) buffer Order { int  order[]; };\n"
// rgba16f, NOT rgba8. The .g channel is the surface-identity marker
// (eye distance * 0.001) that the temporal history test keys on. In 8 bits
// one step is 3.92 WORLD UNITS, and the rejection threshold was one step -
// so any two surfaces within ~4 units of eye distance were indistinguishable
// and history bled freely between them. On ground/dirt, where everything sits
// at similar depth, that meant constant bleeding - the artefact the owner
// described as smearing "onto dirt and everything else". 16F resolves ~0.03
// units at this scale, which makes the identity test actually selective.
"layout(rgba16f, binding = 0) uniform writeonly image2D uOut;\n"
"uniform sampler2D uDepth;\n"
"uniform mat4  uInvVP;\n"
"uniform vec2  uRes;\n"
"uniform float uNear, uFar, uDepthInvert, uDepthFlipV;\n"
"uniform float uRadius, uRayCount, uFrame, uStrength;\n"
"uniform sampler2D uHist;\n"
"uniform mat4  uPrevVP;\n"
"uniform float uHasHist, uBlend, uAoClamp;\n"
"uniform vec3  uEyeAo;\n"
// The eye the HISTORY was written from. Needed because the history's stored
// distance was measured from there, not from where the camera is now.
"uniform vec3  uPrevEyeAo;\n"
"uniform float uHistFlip;\n"
"uniform float uRequireHit;\n"
"uniform float uCardOpacity;\n"
"uniform float uSkipCards;\n"
"uniform float uCardCell;\n"
"uniform float uFovScale;\n"
"uniform float uGeoNormal;\n"
"float rawDepth(vec2 uv){\n"
"  vec2 d = vec2(uv.x, (uDepthFlipV > 0.5) ? (1.0 - uv.y) : uv.y);\n"
"  float z = texture(uDepth, d).r;\n"
"  return (uDepthInvert > 0.5) ? (1.0 - z) : z;\n"
"}\n"
"vec3 worldAt(vec2 uv){\n"
"  float z = rawDepth(uv);\n"
// Same vertical convention the camera trace needs (owner saw upside-down
// trees until it was applied): screen y runs opposite to clip y here.
// uFovScale widens or narrows the effective field of view of the inverse we
// unproject with. Scaling NDC x,y by k is equivalent to scaling tan(fov/2) by
// k, so sweeping k and minimising |Pdepth - hp| MEASURES the true FOV instead
// of trusting either the heap scan (which has reported 80, 90 and 106 across
// runs) or the draw-derived decomposition (exact only if its program is the
// scene camera). k = tan(trueFov/2) / tan(ourFov/2).
"  vec2 n = vec2(uv.x * 2.0 - 1.0, -(uv.y * 2.0 - 1.0)) * uFovScale;\n"
"  vec4 c = uInvVP * vec4(n, z * 2.0 - 1.0, 1.0);\n"
"  return c.xyz / c.w;\n"
"}\n"
// The view ray through a pixel WITHOUT consulting the depth buffer. Unproject
// the same pixel at two NDC depths and take the difference: depth cancels, so
// this direction is exact even where the reconstructed distance is not.
// Aiming the primary ray via worldAt() folded every depth-reconstruction error
// into the ray's DIRECTION, which lands it on the wrong surface - and the
// measured error grows with distance (near buildings agree, far cliffs are off
// by 10+ units), which for a fixed angular error is exactly a lateral miss
// that scales with range. A wrong surface gives AO that belongs somewhere else
// and shifts as the camera turns.
"vec3 rayDirAt(vec2 uv){\n"
"  vec2 n = vec2(uv.x * 2.0 - 1.0, -(uv.y * 2.0 - 1.0)) * uFovScale;\n"
"  vec4 a = uInvVP * vec4(n, -1.0, 1.0);\n"
"  vec4 b = uInvVP * vec4(n,  1.0, 1.0);\n"
"  return normalize(b.xyz / b.w - a.xyz / a.w);\n"
"}\n"
// Closest hit, returning the triangle's GEOMETRIC normal. Depth-derived
// normals swing as the camera orbits (the two screen-space difference
// vectors go near-parallel at grazing angles), which tilted the AO
// hemisphere and produced dark blobs that moved with the view. A normal
// taken from the triangle itself cannot do that - it is a property of the
// world, not of where we are standing.
"bool closestHit(vec3 ro, vec3 rd, float maxT, out vec3 hitP, out vec3 hitN){\n"
"  vec3 inv = 1.0 / (rd + vec3(equal(rd, vec3(0.0))) * 1e-9);\n"
"  float best = maxT; bool got = false;\n"
// STACK 48, GUARD 46 - must cover the BUILDER's cap at raytrace.cpp:192, which
// forces a leaf only at depth >= 40. Both children are always pushed and every
// ancestor's sibling stays pending, so after popping a node at depth d, sp == d
// exactly - ray-independent. At stack[40]/sp<38 every node at depth >= 38 was
// silently NOT expanded and its whole subtree was unreachable to every ray,
// from every camera, with no counter and no message. It fired on real scenes:
// a 78,337-triangle build reported depth 40, the builder's hard cap.
// The debug x-ray (raytrace.cpp:267/290) already used stack[48]/sp<46 and so
// covered depth 40 completely - which is why six weeks of 'the BVH looks
// complete' checks never saw this: they used the OTHER traversal.
// If the builder's cap at :192 changes, change these two together.
"  int stack[48]; int sp = 0; stack[sp++] = 0;\n"
"  while (sp > 0) {\n"
"    int ni = stack[--sp];\n"
"    vec4 a = nodes[ni * 2], b = nodes[ni * 2 + 1];\n"
"    vec3 t0 = (a.xyz - ro) * inv, t1 = (b.xyz - ro) * inv;\n"
"    vec3 lo = min(t0, t1), hi = max(t0, t1);\n"
"    float en = max(max(lo.x, lo.y), max(lo.z, 0.0));\n"
"    float ex = min(min(hi.x, hi.y), min(hi.z, best));\n"
"    if (ex < en) continue;\n"
"    int cnt = floatBitsToInt(b.w); int lf = floatBitsToInt(a.w);\n"
"    if (cnt > 0) {\n"
"      for (int k = 0; k < cnt; k++) {\n"
"        int t = order[lf + k];\n"
"        vec3 v0 = tris[t*3].xyz, v1 = tris[t*3+1].xyz, v2 = tris[t*3+2].xyz;\n"
"        vec3 e1 = v1 - v0, e2 = v2 - v0, pv = cross(rd, e2);\n"
"        float det = dot(e1, pv); if (abs(det) < 1e-9) continue;\n"
"        float id = 1.0/det; vec3 tv = ro - v0;\n"
"        float u = dot(tv, pv) * id; if (u < 0.0 || u > 1.0) continue;\n"
"        vec3 qv = cross(tv, e1);\n"
"        float v = dot(rd, qv) * id; if (v < 0.0 || u + v > 1.0) continue;\n"
"        float tt = dot(e2, qv) * id;\n"
// The PRIMARY ray must find the surface this pixel actually SHOWS, because
// that is what the game's depth buffer recorded. Leaf cards are mostly
// transparent, so the pixel almost always shows what is BEHIND them - but
// closestHit had no card logic (only anyHit did), so the primary ray stopped
// on the card and hp landed metres in front of the real surface. That inflates
// |Pdepth - hp|, which is exactly the error being chased, and it appeared the
// moment cards were kept in the BVH instead of dropped.
"        if (uSkipCards > 0.5 && tris[t*3].w > 0.5) continue;\n"
"        if (tt > 0.01 && tt < best) {\n"
"          best = tt; got = true;\n"
"          hitP = ro + rd * tt;\n"
"          vec3 gn = normalize(cross(e1, e2));\n"
"          hitN = (dot(gn, rd) > 0.0) ? -gn : gn;\n"   // face the viewer
"        }\n"
"      }\n"
"    } else if (sp < 46) { stack[sp++] = lf; stack[sp++] = lf + 1; }\n"
"  }\n"
"  return got;\n"
"}\n"
"bool anyHit(vec3 ro, vec3 rd, float maxT){\n"
"  vec3 inv = 1.0 / (rd + vec3(equal(rd, vec3(0.0))) * 1e-9);\n"
"  int stack[48]; int sp = 0; stack[sp++] = 0;\n"
"  while (sp > 0) {\n"
"    int ni = stack[--sp];\n"
"    vec4 a = nodes[ni * 2], b = nodes[ni * 2 + 1];\n"
"    vec3 t0 = (a.xyz - ro) * inv, t1 = (b.xyz - ro) * inv;\n"
"    vec3 lo = min(t0, t1), hi = max(t0, t1);\n"
"    float en = max(max(lo.x, lo.y), max(lo.z, 0.0));\n"
"    float ex = min(min(hi.x, hi.y), min(hi.z, maxT));\n"
"    if (ex < en) continue;\n"
"    int cnt = floatBitsToInt(b.w); int lf = floatBitsToInt(a.w);\n"
"    if (cnt > 0) {\n"
"      for (int k = 0; k < cnt; k++) {\n"
"        int t = order[lf + k];\n"
"        vec3 v0 = tris[t*3].xyz, v1 = tris[t*3+1].xyz, v2 = tris[t*3+2].xyz;\n"
"        vec3 e1 = v1 - v0, e2 = v2 - v0, pv = cross(rd, e2);\n"
"        float det = dot(e1, pv); if (abs(det) < 1e-9) continue;\n"
"        float id = 1.0/det; vec3 tv = ro - v0;\n"
"        float u = dot(tv, pv) * id; if (u < 0.0 || u > 1.0) continue;\n"
"        vec3 qv = cross(tv, e1);\n"
"        float v = dot(rd, qv) * id; if (v < 0.0 || u + v > 1.0) continue;\n"
"        float tt = dot(e2, qv) * id;\n"
"        if (tt > 0.02 && tt < maxT) {\n"
// LEAF CARDS ARE NOT OPAQUE. tris[t*3].w carries 1.0 for a triangle drawn as
// foliage. A card is a large quad with a mostly-transparent leaf texture; the
// tracer has no alpha, so counting it as a solid hit turns every tree into a
// black slab that swings through the AO radius as the camera moves.
//
// Treat it as PARTIALLY covered instead: let the ray through unless a cheap
// hash of the hit position says otherwise. Hashing the POSITION rather than a
// per-ray counter keeps the pattern fixed to the world, so it does not shimmer
// when the camera moves - the whole point. uCardOpacity 0 makes cards
// invisible to rays, 1 restores the old solid behaviour.
"          if (tris[t*3].w > 0.5 && uCardOpacity < 0.999) {\n"
"            vec3 hp = ro + rd * tt;\n"
// DISTANCE FALLOFF. The test was distance-blind: a canopy twenty units away
// occluded exactly as hard as a leaf touching the surface, so a whole tree
// laid a flat, uniform darkening over everything beneath it with a hard
// silhouette - the slab the owner kept seeing. Real foliage shade fades with
// distance because the gaps subtend more angle the further away they are.
// Fade the card's effective opacity linearly to zero across the AO radius.
"            float fall = clamp(1.0 - tt / max(uRadius, 0.001), 0.0, 1.0);\n"
"            float op = uCardOpacity * fall;\n"
// The cell size decides whether a card reads as SOFT SHADE or as a visible
// polygon. At 1/3-unit cells every pixel on one card got nearly the same
// answer, so the card shaded uniformly and appeared as a hard-edged grey slab
// laid over the image - the owner described it as an undefined-texture glitch
// rather than as lighting, which is exactly right. Fine cells make neighbouring
// pixels disagree, and the spatial denoiser then resolves that into dappled
// shade. Still hashed on WORLD POSITION, so it stays fixed to the world and
// does not crawl when the camera moves.
"            float h = fract(sin(dot(floor(hp * uCardCell), vec3(12.9898, 78.233, 37.719))) * 43758.5453);\n"
"            if (h > op) continue;\n"                 // this ray passes through
"          }\n"
"          return true;\n"
"        }\n"
"      }\n"
"    } else if (sp < 46) { stack[sp++] = lf; stack[sp++] = lf + 1; }\n"
"  }\n"
"  return false;\n"
"}\n"
"void main(){\n"
"  ivec2 px = ivec2(gl_GlobalInvocationID.xy);\n"
"  if (px.x >= int(uRes.x) || px.y >= int(uRes.y)) return;\n"
"  vec2 uv = (vec2(px) + 0.5) / uRes;\n"
"  float z = rawDepth(uv);\n"
// SKY. 0.999, not 0.9999: a sky pixel a hair under the tighter threshold fell
// through into the geometry path, got an AO computed for a point ~1000 units
// away, and then ACCUMULATED HISTORY - which is what smears dark diagonal
// streaks across the sky as the camera pitches and geometry leaves the frame.
// The marker matters as much as the threshold: sky writes .g = 1.0 while
// geometry is clamped to 0.99 below, so the history test can tell them apart.
// Before, geometry at 1000 units also produced .g = 1.0 and history from a
// building was happily accepted onto a sky pixel.
"  if (z >= 0.999) { imageStore(uOut, px, vec4(1.0)); return; }\n"
"  vec3 N = vec3(0.0, 0.0, 1.0); bool haveN = false;\n"   // sky
"  vec3 P = worldAt(uv);\n"
// Primary ray from the eye through this pixel: the triangle it lands on
// gives BOTH position and normal, straight from the world. The depth-derived
// P above is kept only to aim the ray and as a fallback.
"  vec3 Pdepth = P;\n"
"  if (uGeoNormal > 0.5) {\n"
// Direction from the projection alone; range from the depth, generously, so a
// distance that reads short cannot clip the ray before the real surface.
"    vec3 rd = rayDirAt(uv);\n"
"    vec3 hp, hn;\n"
"    if (closestHit(uEyeAo, rd, length(P - uEyeAo) * 1.5 + 8.0, hp, hn)) {\n"
"      P = hp; N = hn; haveN = true;\n"
// -6: IS OUR CAMERA THE GAME'S CAMERA? Pdepth comes from the game's depth
// buffer through OUR inverse view-projection; hp comes from tracing the BVH.
// They are the same physical point, so if our matrix matches the one the game
// rendered with, the two agree and this reads black. Any disagreement is
// camera error, and a wrong FOV makes it grow toward the screen edges.
// Green = the gap in world units / 10; red channel saturates past 10 units.
"      if (uStrength < -5.5 && uStrength > -6.5) {\n"
"        float d = length(Pdepth - hp);\n"
"        imageStore(uOut, px, vec4(min(d / 10.0, 1.0), min(d, 1.0), 0.0, 1.0));\n"
"        return;\n"
"      }\n"
// A MISS means this pixel's surface is not in the BVH at all, so there is no
// honest occlusion to compute for it. Falling through to the depth-derived
// P and N casts rays from a surface the BVH does not contain; they intersect
// erratically and produce scattered false-occlusion specks that dance as the
// camera moves. Measured with `rt all 0`, where the terrain is absent: the
// whole ground came back speckled black on an otherwise clean buffer, and the
// owner identified those dark spots as the remaining artefact.
// Report "unoccluded" instead of guessing. Flat is better than wrong.
"    } else if (uRequireHit > 0.5) {\n"
// -7: BVH COMPLETENESS. A miss means this pixel's surface is not in the BVH
// at all. Red here, black on a hit, so a readback counts the miss rate - the
// real measure of how much of the visible world the harvest actually holds.
"      if (uStrength < -6.5 && uStrength > -7.5) {\n"
"        imageStore(uOut, px, vec4(1.0, 0.0, 0.0, 1.0)); return;\n"
"      }\n"
"      imageStore(uOut, px, vec4(1.0, length(P - uEyeAo) * 0.001, 0.5, 1.0));\n"
"      return;\n"
"    }\n"
"    if (uStrength < -6.5 && uStrength > -7.5) {\n"
"      imageStore(uOut, px, vec4(0.0, 0.0, 0.0, 1.0)); return;\n"
"    }\n"
"  }\n"
"  vec2 tx = 1.0 / uRes;\n"
// Guarded reconstruction. An unguarded normalize() of a degenerate cross
// product yields NaN, and a NaN ray direction is INVISIBLE in traversal:
// every NaN comparison is false, so the slab test never rejects and the
// triangle test never accepts. The ray silently finds nothing - which is
// exactly the uniform, radius-independent AO=1 we measured.
"  vec3 dx = worldAt(uv + vec2(tx.x, 0.0)) - P;\n"
"  vec3 dy = worldAt(uv + vec2(0.0, tx.y)) - P;\n"
"  vec3 cr = cross(dx, dy);\n"
"  float crl = length(cr);\n"
"  if (!haveN) N = (crl > 1e-8) ? (cr / crl) : vec3(0.0, 0.0, 1.0);\n"
"  if (uStrength < -2.5 && uStrength > -3.5) { float f = (crl > 1e-8) ? 0.0 : 1.0;\n"
"    imageStore(uOut, px, vec4(f, 1.0 - f, 0.0, 1.0)); return; }\n"
// -4: what does THIS program actually see in the BVH buffer? Green = a real
// root box, red = zeros (the buffers are not visible to this program). The
// camera trace binds the same SSBOs and works, so this separates 'the maths
// is wrong' from 'the data never arrived'.
// -5: does the depth-derived position live in the SAME SPACE as the BVH?
// Green = P is inside the root box, red = outside. The position diagnostic
// only proved smoothness; this proves agreement. If rays start outside the
// tree entirely they can never hit anything, at any radius.
"  if (uStrength < -4.5) {\n"
"    vec3 lo = nodes[0].xyz, hi = nodes[1].xyz;\n"
"    bool inside = all(greaterThanEqual(P, lo)) && all(lessThanEqual(P, hi));\n"
"    imageStore(uOut, px, inside ? vec4(0.0,1.0,0.0,1.0) : vec4(1.0,0.0,0.0,1.0));\n"
"    return;\n"
"  }\n"
"  if (uStrength < -3.5) {\n"
"    vec4 a = nodes[0], b = nodes[1];\n"
"    float e = max(max(b.x - a.x, b.y - a.y), b.z - a.z);\n"
"    int cnt = floatBitsToInt(b.w);\n"
"    if (e > 1.0) imageStore(uOut, px, vec4(0.0, min(e / 500.0, 1.0), 0.0, 1.0));\n"
"    else imageStore(uOut, px, vec4(1.0, 0.0, float(cnt != 0), 1.0));\n"
"    return;\n"
"  }\n"
// Diagnostics, driven by uStrength as a sentinel to avoid a new uniform:
//   -1 = reconstructed world position (level extent mapped to colour)
//   -2 = reconstructed normal
// If the position does not look like the level, the rays start in the wrong
// place and no amount of radius will help.
"  if (uStrength < -1.5) { imageStore(uOut, px, vec4(N * 0.5 + 0.5, 1.0)); return; }\n"
"  if (uStrength < -0.5) { imageStore(uOut, px, vec4(fract(P * 0.002 + 0.5), 1.0)); return; }\n"
"  vec3 up = (abs(N.z) < 0.9) ? vec3(0,0,1) : vec3(1,0,0);\n"
"  vec3 T = normalize(cross(up, N)), B = cross(N, T);\n"
"  float seed = fract(sin(dot(vec2(px), vec2(12.9898, 78.233))) * 43758.5453 + uFrame);\n"
"  int n = int(uRayCount);\n"
"  float occ = 0.0;\n"
"  for (int i = 0; i < 16; i++) {\n"
"    if (i >= n) break;\n"
"    float a1 = 6.2831853 * fract(seed + float(i) * 0.618034);\n"
"    float r2 = fract(seed * 7.13 + float(i) * 0.7548777);\n"
"    float r = sqrt(r2);\n"
"    vec3 dir = normalize(T * (r * cos(a1)) + B * (r * sin(a1)) + N * sqrt(1.0 - r2));\n"
"    if (anyHit(P + N * 0.05, dir, uRadius)) occ += 1.0;\n"
"  }\n"
"  float ao = 1.0 - (occ / float(n)) * uStrength;\n"
// Temporal accumulation, reprojected through WORLD SPACE. We already know
// this pixel's exact world position, so we can project it with the previous
// frame's camera and find precisely where it was - no motion vectors, no
// guesswork. 16 rays/frame accumulate into a clean result within a few
// frames while the jitter cycles.
"  if (uHasHist > 0.5) {\n"
"    vec4 pc = uPrevVP * vec4(P, 1.0);\n"
"    if (pc.w > 0.0001) {\n"
"      vec2 puv = (pc.xy / pc.w) * 0.5 + 0.5;\n"
// The history is written by imageStore at px, i.e. y-UP, and this reprojection
// also produces y-up UVs - so the flip may be wrong, which would fetch history
// from the vertically MIRRORED pixel and drag it across the world as the
// camera turns. Switchable rather than argued: `rtao_histflip 0|1`.
"      if (uHistFlip > 0.5) puv.y = 1.0 - puv.y;\n"
"      if (puv.x > 0.001 && puv.x < 0.999 && puv.y > 0.001 && puv.y < 0.999) {\n"
// texelFetch, NOT texture(). .g and .b are surface-identity KEYS, not signals:
// GL_LINEAR blends them with three neighbours and FABRICATES a value belonging
// to no surface. While the reprojection was the identity (pairprev 1, or any
// stationary frame) puv landed on exact texel centres and nothing was ever
// blended, so this was harmless and invisible. Making the reprojection real
// moves puv ~17.5 texels/frame at 100 deg/s at arbitrary sub-texel phase - the
// fabricated .g then fails the 1.5-unit gate, the pixel loses history entirely
// and reverts to the raw 8-ray estimate (8.04/255 vs 2.63). Motion-only,
// settles when still: the exact artefact the pairprev fix was meant to remove.
// The two MUST land together.
"        ivec2 hsz = textureSize(uHist, 0);\n"
"        ivec2 hpx = clamp(ivec2(puv * vec2(hsz)), ivec2(0), hsz - 1);\n"
"        vec3 h = texelFetch(uHist, hpx, 0).rgb;\n"
// Reject on SURFACE IDENTITY, not on value. Two independent tests:
//
// DISTANCE, measured from the PREVIOUS eye. h.g was written last frame using
// last frame's eye position, so comparing it against a distance measured from
// THIS frame's eye compares two different quantities. A third-person orbit
// camera translates the eye on every mouse move, so the same surface point
// legitimately changes distance each frame: the old test threw away valid
// history AND accepted history from unrelated surfaces that happened to match
// the new distance. That second case is the AO sliding across the world as
// the camera turns, which is exactly what the owner reported.
"        float dPrev = min(length(P - uPrevEyeAo) * 0.001, 0.99);\n"
// Reject SKY history outright. 1.0 is reserved for sky; anything at or above
// 0.995 in the history is sky and must never blend into a geometry pixel.
"        if (h.g >= 0.995) { imageStore(uOut, px, vec4(ao, min(length(P - uEyeAo) * 0.001, 0.99), N.z * 0.5 + 0.5, 1.0)); return; }\n"
// ORIENTATION, which is view-independent and catches what distance cannot.
// .g saturates at 1000 units and quantises to ~3.9 units per step in RGBA8,
// so distant surfaces are all indistinguishable by depth alone.
"        float nNow = N.z * 0.5 + 0.5;\n"
// 0.0015 (1.5 world units), not 0.004. The old threshold was one RGBA8
// quantisation step - the floor the format imposed, not a chosen tolerance.
// With 16F the marker resolves ~0.03 units, so the test can finally be
// selective enough to stop history crossing between nearby surfaces.
"        if (abs(h.g - dPrev) < 0.0015 && abs(h.b - nNow) < 0.1) {\n"
// CLAMP, then blend. The identity test above is binary and UNBOUNDED: an accept
// blends 88% of whatever history holds. The owner-approved V2 stack was stable
// precisely because it never trusted history - it clamped it to the current
// frame +- a tolerance (gfx.cpp ~805), so a wrong accept can only shift the
// result by that tolerance instead of replacing it. RTAO dropped that and
// became safe-only-if-the-scalar-test-is-right; the architecture review found
// that test's usable window is EMPTY (>= 2.4 units needed for grazing ground,
// but 3.92 units is measured bleeding). Bound the damage structurally instead.
// `rt aoclamp 0` restores the old unbounded behaviour for A/B.
"          float hr = (uAoClamp > 0.0) ? clamp(h.r, ao - uAoClamp, ao + uAoClamp) : h.r;\n"
"          ao = mix(hr, ao, uBlend);\n"
"        }\n"
"      }\n"
"    }\n"
"  }\n"
// Green carries this pixel's eye distance so the NEXT frame can tell whether
// its reprojected history is the same surface.
// Blue carried a duplicate of ao and nothing read it (the composite samples .r
// only). It now carries the normal's up-component, remapped to 0..1, so the
// spatial denoiser can stop at a surface BEND - a wall meeting a floor is
// continuous in depth, and a depth-only filter smears AO across that corner.
// Geometry clamps its distance marker to 0.99 so 1.0 stays reserved for sky.
"  imageStore(uOut, px, vec4(ao, min(length(P - uEyeAo) * 0.001, 0.99), N.z * 0.5 + 0.5, 1.0));\n"
"}\n";

// ---- spatial denoiser ------------------------------------------------------
// Temporal accumulation alone still ghosts when the camera moves fast: history
// gets rejected on those pixels (correctly - it belongs to other geometry) and
// what remains is the raw 8-ray estimate, which is noisy. This is the standard
// answer: a separable cross-bilateral blur that averages only across pixels on
// the SAME SURFACE, using the eye distance in .g and the normal in .b as
// edge stops. Separable = 2*N taps instead of N*N.
//
// It deliberately does NOT feed back into the temporal history. Blurring the
// history input compounds every frame and washes contact shadows out entirely.
static const char* kDnSrc =
"#version 430\n"
"layout(local_size_x = 8, local_size_y = 8) in;\n"
// 16F for the same reason as the AO shader: .g/.b are the edge stops the
// vertical pass reads back, and 8-bit quantisation makes them useless.
"layout(rgba16f, binding = 0) uniform writeonly image2D uOut;\n"
"uniform sampler2D uSrc;\n"
"uniform vec2  uRes;\n"
"uniform vec2  uDir;        // (1,0) then (0,1)\n"
"uniform float uRadius;     // taps per side\n"
"uniform float uDepthTol;   // eye-distance units (already scaled by 0.001)\n"
"uniform float uNormalTol;\n"
"void main() {\n"
"  ivec2 px = ivec2(gl_GlobalInvocationID.xy);\n"
"  if (px.x >= int(uRes.x) || px.y >= int(uRes.y)) return;\n"
"  vec2 tuv = (vec2(px) + 0.5) / uRes;\n"
"  vec4 c = texture(uSrc, tuv);\n"
// SKY passes through untouched. Sky is marked .g = 1.0 and geometry clamps to
// 0.99; blurring the two together painted the geometry's AO into the sky as
// VERTICAL STREAKS (the second pass blurs vertically) that trailed every pole
// and tower as the camera pitched.
"  if (c.g >= 0.995) { imageStore(uOut, px, c); return; }\n"
"  float sum = c.r, wsum = 1.0;\n"
"  int R = int(uRadius);\n"
"  float sig = max(uRadius * 0.5, 0.5);\n"
"  for (int i = 1; i <= 8; i++) {\n"
"    if (i > R) break;\n"
"    float wg = exp(-float(i * i) / (2.0 * sig * sig));\n"
"    for (int s = -1; s <= 1; s += 2) {\n"
"      vec2 t2 = tuv + uDir * (float(i * s) / uRes);\n"
"      if (t2.x < 0.0 || t2.x > 1.0 || t2.y < 0.0 || t2.y > 1.0) continue;\n"
"      vec4 t = texture(uSrc, t2);\n"
"      if (t.g <= 0.0) continue;\n"                    // nothing was written there
"      if (t.g >= 0.995) continue;\n"                  // sky is not a surface
"      if (abs(t.g - c.g) > uDepthTol) continue;\n"    // different surface
"      if (abs(t.b - c.b) > uNormalTol) continue;\n"   // different orientation
"      sum += t.r * wg; wsum += wg;\n"
"    }\n"
"  }\n"
// .g and .b pass through untouched: the vertical pass needs the same edge
// stops, and the composite reads .r regardless.
"  imageStore(uOut, px, vec4(sum / max(wsum, 1e-4), c.g, c.b, 1.0));\n"
"}\n";

static GLuint g_prog = 0, g_bufNodes = 0, g_bufTris = 0, g_bufOrder = 0, g_tex = 0;
static GLuint g_aoProg = 0, g_aoTex = 0, g_aoTexB = 0;
static GLuint g_dnProg = 0, g_dnTexA = 0, g_dnTexB = 0;
static GLint  dn_src=-1, dn_res=-1, dn_dir=-1, dn_radius=-1, dn_dtol=-1, dn_ntol=-1;
static int    g_dnOn = 1;          // on by default: the noise is the complaint
static float  g_dnRadius = 4.0f;   // taps per side, per axis
// 0.002 (2 world units), not 0.01 (10!). The old tolerance was ~2.5 RGBA8
// quantisation steps - with everything on the ground within 10 units of its
// neighbours, the blur crossed every silhouette and smeared AO onto dirt and
// anything nearby. Same disease as the temporal threshold: a tolerance set by
// the FORMAT's floor, not by the geometry. 16F removes the floor.
static float  g_dnDepthTol = 0.002f;
static float  g_dnNormalTol = 0.15f;
static int    g_aoW = 0, g_aoH = 0, g_aoCur = 0;
static float  g_prevVP[16];
static int    g_hasPrevVP = 0;
static GLint  ao_hist=-1, ao_prevVP=-1, ao_hasHist=-1, ao_blend=-1, ao_clamp=-1;
static GLint  ao_prevEye=-1, ao_hflip=-1, ao_reqhit=-1, ao_cardop=-1, ao_skipcard=-1, ao_cardcell=-1;
static float  g_cardCell = 40.0f;   // hash cells per world unit across a card
static GLint  ao_fovscale = -1;
static float  g_fovScale = 1.0f;    // 1 = trust the derived matrix's own FOV
// 0, and MEASURED: making the primary ray skip cards took the camera error
// from 2.47 to 9.80 units with 97% of pixels beyond 5 units. The game's depth
// buffer DOES contain the canopy wherever the leaf texture is opaque, so the
// primary ray must be allowed to stop on a card - skipping it sends the ray
// through the tree and onto distant geometry the pixel never showed.
// Occlusion rays are the opposite case and still pass through stochastically.
static float  g_skipCards = 0.0f;
static float  g_histFlip = 1.0f;   // the original behaviour, until measured
static float  g_requireHit = 1.0f; // no BVH surface -> report unoccluded
static int    g_camLog = 0;        // frames of camera logging still to emit

// Unproject an NDC point with a given inverse view-projection.
static void UnprojRt(const float* m, float x, float y, float z, float* out) {
    float w = m[12]*x + m[13]*y + m[14]*z + m[15];
    if (w > -1e-9f && w < 1e-9f) w = 1e-9f;
    out[0] = (m[0]*x + m[1]*y + m[2]*z  + m[3])  / w;
    out[1] = (m[4]*x + m[5]*y + m[6]*z  + m[7])  / w;
    out[2] = (m[8]*x + m[9]*y + m[10]*z + m[11]) / w;
}

// Local float formatter; the geocapture one is static to that file.
static void RtF3(char* out, float v) {
    float a = v < 0 ? -v : v;
    if (!(a < 1e9f)) { lstrcpyA(out, (v != v) ? "NaN" : "HUGE"); return; }
    int w = (int)a;
    int m = (int)((a - (float)w) * 1000.0f);
    if (m < 0) m = 0;
    wsprintfA(out, "%s%d.%03d", v < 0 ? "-" : "", w, m);
}
static float  g_prevEye[3] = { 0, 0, 0 };   // the eye the history was written from
static GLint  ao_eye=-1, ao_geoN=-1;
static float  g_aoGeoNormal = 1.0f;   // geometric normals from the BVH
static float  g_aoBlend = 0.12f;   // weight of the NEW frame; low = smoother
static float   g_aoClamp = 0.15f;   // V2's tolerance; 0 = unbounded (old)
static GLint  ao_invVP=-1, ao_res=-1, ao_depth=-1, ao_near=-1, ao_far=-1;
static GLint  ao_dinv=-1, ao_dflip=-1, ao_radius=-1, ao_rays=-1, ao_frame=-1, ao_strength=-1;
static float  g_aoRadius = 4.0f, g_aoRays = 8.0f, g_aoStrength = 1.0f;
static int    g_aoFrame = 0;
static int    g_texW = 0, g_texH = 0;
static GLint  u_invVP = -1, u_res = -1, u_maxDist = -1, u_nearZ = -1, u_mode = -1, u_eye = -1, u_flipY = -1;
static float  g_nearZ = 0.0f, g_maxDist = 900.0f, g_mode = 0.0f;
static float  g_eye[3] = { 0, 0, 0 };
// Default ON: verified in play once the geometry was correct - trees stood
// the right way up. The earlier test that rejected it was run against the
// broken buffer reads, so it proved nothing.
static float  g_flipY = 1.0f;
void SWSE_RtSetFlipY(float f) { g_flipY = f; }

// clip (x,y,z,1) -> world, homogeneous divide included. inv is row-major.
static void Unproj(const float* inv, float x, float y, float z, float* out) {
    float w = inv[12]*x + inv[13]*y + inv[14]*z + inv[15];
    if (w > -1e-9f && w < 1e-9f) w = 1e-9f;
    out[0] = (inv[0]*x  + inv[1]*y  + inv[2]*z  + inv[3])  / w;
    out[1] = (inv[4]*x  + inv[5]*y  + inv[6]*z  + inv[7])  / w;
    out[2] = (inv[8]*x  + inv[9]*y  + inv[10]*z + inv[11]) / w;
}
void SWSE_RtSetMode(float m) { g_mode = m; }
void SWSE_RtSetNearZ(float z) { g_nearZ = z; }
void SWSE_RtSetMaxDist(float d) { g_maxDist = d; }
// 0 = packed only, 1 = everything, 2 = every DEPTH-WRITING draw. 2 is the one
// that means "solid world geometry": it keeps the terrain that 0 loses without
// readmitting the camera-attached overlay planes that 1 lets back in.
void SWSE_RtSetUseAll(int on) { g_useAll = (on < 0 || on > 2) ? 0 : on; }
static double g_traceMs = 0;

unsigned SWSE_RtTex() { return g_built ? g_tex : 0; }

static int BuildProgram(SWSE_RtEmit emit) {
    if (g_prog) return 1;
    GLuint sh = p_CreateShader(0x91B9 /*COMPUTE_SHADER*/);
    p_ShaderSource(sh, 1, &kTraceCs, 0);
    p_CompileShader(sh);
    GLint ok = 0; p_GetShaderiv(sh, 0x8B81, &ok);
    if (!ok) {
        char log[900]; log[0] = 0;
        p_GetShaderInfoLog(sh, sizeof(log) - 1, 0, log);
        char b[1000]; wsprintfA(b, "rt: trace shader FAILED: %s", log);
        if (emit) emit(b);
        LogR(b);
        p_DeleteShader(sh);
        return 0;
    }
    g_prog = p_CreateProgram();
    p_AttachShader(g_prog, sh);
    p_LinkProgram(g_prog);
    GLint lk = 0; p_GetProgramiv(g_prog, 0x8B82, &lk);
    p_DeleteShader(sh);
    if (!lk) { if (emit) emit("rt: trace program link FAILED"); g_prog = 0; return 0; }
    u_invVP   = p_GetUniformLocation(g_prog, "uInvVP");
    u_res     = p_GetUniformLocation(g_prog, "uRes");
    u_maxDist = p_GetUniformLocation(g_prog, "uMaxDist");
    u_nearZ   = p_GetUniformLocation(g_prog, "uNearZ");
    u_mode    = p_GetUniformLocation(g_prog, "uMode");
    u_eye     = p_GetUniformLocation(g_prog, "uEye");
    u_flipY   = p_GetUniformLocation(g_prog, "uFlipY");
    return 1;
}

void SWSE_RtBuild(SWSE_RtEmit emit) {
    char b[220];
    g_built = 0;
    g_tris = SWSE_GeoHarvestTris(&g_nTris);
    if (!g_tris || g_nTris < 1) { if (emit) emit("rt: no harvest yet - run 'harvest' first"); return; }
    if (!ResolveAll()) { if (emit) emit("rt: compute entry points missing"); return; }

    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);

    free(g_order); free(g_nodes); free(g_cardFlag);
    g_cardFlag = (unsigned char*)calloc((size_t)g_nTris, 1);
    g_order = (int*)malloc((size_t)g_nTris * sizeof(int));
    g_nodeCap = g_nTris * 2 + 16;
    g_nodes = (Node*)malloc((size_t)g_nodeCap * sizeof(Node));
    if (!g_order || !g_nodes || !g_cardFlag) { if (emit) emit("rt: out of memory"); return; }
    // Billboards excluded: sky, water and fog planes are camera-attached, so
    // every ray hits them at distance zero (the first traced frame came back
    // solid white). Only packed-position draws are real world geometry.
    // That rule dates from when float-attribute draws decoded to garbage, so
    // "packed" doubled as a proxy for "decodes correctly". Now that every draw
    // decodes (429/429, 0 collapsed), it excludes real world geometry - half
    // the harvest, the ground among it. `rt all 1` keeps everything.
    const unsigned char* packed = SWSE_GeoHarvestPackedFlags();
    const unsigned char* world  = SWSE_GeoHarvestWorldFlags();
    const unsigned char* fol    = SWSE_GeoHarvestFoliageFlags();
    const float*         area   = SWSE_GeoHarvestAreas();
    int nUse = 0, nCards = 0;
    for (int i = 0; i < g_nTris; i++) {
        int keep;
        if (g_useAll == 2)      keep = !world || world[i];       // depth-writing
        else if (g_useAll == 1) keep = 1;                        // everything
        else                    keep = !packed || packed[i];     // packed only
        // LEAF CARDS ARE KEPT, AND MARKED. Dropping them was a binary decision
        // that failed badly in both directions: too strict deleted terrain and
        // gutted the AO, too loose left black tree slabs. Marked instead, they
        // stay in the BVH and anyHit() lets most rays through, which is what a
        // mostly-transparent leaf texture actually does. A misclassification
        // now costs a little translucency rather than a missing surface.
        int isCard = 0;
        if (g_cardSize > 0.0f && area && area[i] > g_cardSize) {
            if (g_cardAny || (fol && fol[i])) { isCard = 1; nCards++; }
        }
        if (keep) { g_cardFlag[i] = (unsigned char)isCard; g_order[nUse++] = i; }
    }
    if (emit) {
        char cb[180];
        wsprintfA(cb, "rt: leaf cards MARKED %d (area > %d, %s) - opacity %d%%",
                  nCards, (int)g_cardSize,
                  g_cardAny ? "any program" : "foliage programs only",
                  (int)(g_cardOpacity * 100.0f));
        emit(cb);
    }
    if (nUse < 1) { if (emit) emit("rt: no world triangles after billboard filter"); return; }
    g_traceTris = nUse;
    g_nNodes = 0; g_maxDepth = 0;
    int root = NewNode();
    Build(root, 0, nUse, 0);

    QueryPerformanceCounter(&t1);
    g_buildMs = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;

    // --- upload: nodes as 2 vec4, triangles as 3 vec4 ---
    if (!g_bufNodes) p_GenBuffers(1, &g_bufNodes);
    if (!g_bufTris)  p_GenBuffers(1, &g_bufTris);
    if (!g_bufOrder) p_GenBuffers(1, &g_bufOrder);

    float* gn = (float*)malloc((size_t)g_nNodes * 8 * sizeof(float));
    float* gt = (float*)malloc((size_t)g_nTris * 12 * sizeof(float));
    if (!gn || !gt) { free(gn); free(gt); if (emit) emit("rt: upload alloc failed"); return; }
    for (int i = 0; i < g_nNodes; i++) {
        gn[i*8+0]=g_nodes[i].bmin[0]; gn[i*8+1]=g_nodes[i].bmin[1]; gn[i*8+2]=g_nodes[i].bmin[2];
        *(int*)&gn[i*8+3] = g_nodes[i].leftFirst;
        gn[i*8+4]=g_nodes[i].bmax[0]; gn[i*8+5]=g_nodes[i].bmax[1]; gn[i*8+6]=g_nodes[i].bmax[2];
        *(int*)&gn[i*8+7] = g_nodes[i].count;
    }
    const unsigned char* cardFlag = g_cardFlag;
    for (int i = 0; i < g_nTris; i++)
        for (int v = 0; v < 3; v++) {
            gt[i*12+v*4+0] = g_tris[(size_t)i*9+v*3+0];
            gt[i*12+v*4+1] = g_tris[(size_t)i*9+v*3+1];
            gt[i*12+v*4+2] = g_tris[(size_t)i*9+v*3+2];
            // .w of the first vertex carries the leaf-card flag; the other two
            // are spare. anyHit() reads it to decide whether this triangle is
            // a solid occluder or a partially transparent leaf card.
            gt[i*12+v*4+3] = (v == 0 && cardFlag && cardFlag[i]) ? 1.0f : 0.0f;
        }
    p_BindBuffer(0x90D2, g_bufNodes);
    p_BufferData(0x90D2, (GLsizeiptr_)((size_t)g_nNodes * 8 * sizeof(float)), gn, 0x88E4);
    p_BindBuffer(0x90D2, g_bufTris);
    p_BufferData(0x90D2, (GLsizeiptr_)((size_t)g_nTris * 12 * sizeof(float)), gt, 0x88E4);
    p_BindBuffer(0x90D2, g_bufOrder);
    p_BufferData(0x90D2, (GLsizeiptr_)((size_t)g_nTris * sizeof(int)), g_order, 0x88E4);
    p_BindBuffer(0x90D2, 0);
    free(gn); free(gt);

    if (!BuildProgram(emit)) return;
    g_built = 1;

    wsprintfA(b, "rt: BVH %d nodes, depth %d, %d world tris (of %d harvested), %d ms",
              g_nNodes, g_maxDepth, g_traceTris, g_nTris, (int)g_buildMs);
    if (emit) emit(b);
    LogR(b);
    wsprintfA(b, "rt: uploaded %d KB nodes + %d KB tris",
              (int)((size_t)g_nNodes * 32 / 1024), (int)((size_t)g_nTris * 48 / 1024));
    if (emit) emit(b);
    // The root box must enclose the whole level. If it is small, the tree is
    // not holding what the harvest collected - measured after the traversal
    // heat map showed rays exiting at the root everywhere but one small patch.
    wsprintfA(b, "rt: root box x[%d..%d] y[%d..%d] z[%d..%d]",
              (int)g_nodes[0].bmin[0], (int)g_nodes[0].bmax[0],
              (int)g_nodes[0].bmin[1], (int)g_nodes[0].bmax[1],
              (int)g_nodes[0].bmin[2], (int)g_nodes[0].bmax[2]);
    if (emit) emit(b);
}

// Every camera ray passes through the eye, so two rays from different screen
// positions intersect there. Unproject two points along each and take the
// closest approach of the two lines - no assumption about which NDC z is the
// near plane.
//
// SHARED by the camera trace and the AO pass. The AO used to read g_eye
// without ever computing it, so unless the camera trace happened to be
// running, its primary rays started from a stale origin - which is why the
// artifacts got worse the further the camera rotated from wherever the eye
// was last computed.
static void DeriveEye(const float* inv) {
    float A[3], B[3], C[3], D[3];
    Unproj(inv, -0.5f, -0.5f, 0.0f, A);
    Unproj(inv, -0.5f, -0.5f, 0.8f, B);
    Unproj(inv,  0.5f,  0.5f, 0.0f, C);
    Unproj(inv,  0.5f,  0.5f, 0.8f, D);
    float d1[3], d2[3], r[3];
    for (int i = 0; i < 3; i++) { d1[i] = B[i]-A[i]; d2[i] = D[i]-C[i]; r[i] = A[i]-C[i]; }
    float a = d1[0]*d1[0]+d1[1]*d1[1]+d1[2]*d1[2];
    float bq= d1[0]*d2[0]+d1[1]*d2[1]+d1[2]*d2[2];
    float cq= d2[0]*d2[0]+d2[1]*d2[1]+d2[2]*d2[2];
    float dq= d1[0]*r[0] +d1[1]*r[1] +d1[2]*r[2];
    float eq= d2[0]*r[0] +d2[1]*r[1] +d2[2]*r[2];
    float den = a*cq - bq*bq;
    if (den > -1e-9f && den < 1e-9f) return;     // parallel: no camera to find
    float t = (bq*eq - cq*dq) / den;
    float s = (a*eq - bq*dq) / den;
    for (int i = 0; i < 3; i++)
        g_eye[i] = 0.5f * ((A[i] + t*d1[i]) + (C[i] + s*d2[i]));
}

void SWSE_RtTrace(int w, int h) {
    if (!g_built || !g_prog) return;
    // Half resolution: this is the debug view, and the roadmap's real target
    // is secondary rays at half res anyway.
    int tw = w / 2, th = h / 2;
    if (tw < 16 || th < 16) return;

    // DERIVE the camera, do not read a cache nobody filled. SWSE_WindClipVPLast
    // only returns whatever the last SWSE_WindClipCamera call published, and
    // during normal play the ONLY caller is gfx.cpp's cam_draw block, which is
    // opt-in and off by default. So the tracer was reconstructing world
    // positions with a matrix frozen at harvest time: as the player moved, the
    // real camera diverged from the frozen one and every position drifted -
    // the AO "bouncing around everywhere" when the camera moved. Same bug as
    // the harvest had, in a second place.
    { float n_, f_, fv_, asp_; unsigned pr_; int cv_, gp_;
      SWSE_WindClipCamera(&n_, &f_, &fv_, &asp_, &pr_, &cv_, &gp_); }
    float vp[16];
    if (!SWSE_WindClipVPLast(vp)) return;
    float clip[16];
    for (int i = 0; i < 16; i++) clip[i] = vp[i];
    for (int i = 4; i < 8; i++) clip[i] = -clip[i];      // engine's clip-y flip
    float inv[16];
    if (!SWSE_GeoInvert4x4(clip, inv)) return;   // same inverse the harvest uses

    DeriveEye(inv);

    GLint prevProg = 0, prevTex = 0;
    glGetIntegerv(0x8B8D, &prevProg);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);

    if (!g_tex || tw != g_texW || th != g_texH) {
        if (!g_tex) glGenTextures(1, &g_tex);
        glBindTexture(GL_TEXTURE_2D, g_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, 0x8058 /*RGBA8*/, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
        g_texW = tw; g_texH = th;
    }
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);

    p_UseProgram(g_prog);
    p_BindBufferBase(0x90D2, 0, g_bufNodes);
    p_BindBufferBase(0x90D2, 1, g_bufTris);
    p_BindBufferBase(0x90D2, 2, g_bufOrder);
    p_BindImageTexture(0, g_tex, 0, GL_FALSE, 0, 0x88B9 /*WRITE_ONLY*/, 0x8058);
    if (u_invVP   >= 0) p_UniformMatrix4fv(u_invVP, 1, GL_TRUE, inv);   // row-major
    if (u_res     >= 0) p_Uniform2f(u_res, (float)tw, (float)th);
    if (u_maxDist >= 0) p_Uniform1f(u_maxDist, g_maxDist);
    if (u_nearZ   >= 0) p_Uniform1f(u_nearZ, g_nearZ);
    if (u_mode    >= 0) p_Uniform1f(u_mode, g_mode);
    if (u_eye     >= 0 && p_Uniform3f) p_Uniform3f(u_eye, g_eye[0], g_eye[1], g_eye[2]);
    if (u_flipY   >= 0) p_Uniform1f(u_flipY, g_flipY);
    p_DispatchCompute((tw + 7) / 8, (th + 7) / 8, 1);
    p_MemoryBarrier(0xFFFFFFFF);
    p_UseProgram((GLuint)prevProg);
}

// Same non-fatal contract: if the denoiser fails to build, SWSE_RtAoTex falls
// back to the raw accumulation and the picture is noisy rather than absent.
static int BuildDnProgram(void) {
    if (g_dnProg) return 1;
    static int tried = 0;
    if (tried) return 0;
    tried = 1;
    GLuint sh = p_CreateShader(0x91B9);
    p_ShaderSource(sh, 1, &kDnSrc, 0);
    p_CompileShader(sh);
    GLint ok = 0; p_GetShaderiv(sh, 0x8B81, &ok);
    if (!ok) {
        char log[900]; log[0] = 0;
        p_GetShaderInfoLog(sh, sizeof(log) - 1, 0, log);
        char b[1000]; wsprintfA(b, "rtao denoise: shader FAILED: %s", log);
        LogR(b);
        p_DeleteShader(sh);
        return 0;
    }
    g_dnProg = p_CreateProgram();
    p_AttachShader(g_dnProg, sh);
    p_LinkProgram(g_dnProg);
    GLint lk = 0; p_GetProgramiv(g_dnProg, 0x8B82, &lk);
    p_DeleteShader(sh);
    if (!lk) { LogR("rtao denoise: link FAILED"); g_dnProg = 0; return 0; }
    dn_src    = p_GetUniformLocation(g_dnProg, "uSrc");
    dn_res    = p_GetUniformLocation(g_dnProg, "uRes");
    dn_dir    = p_GetUniformLocation(g_dnProg, "uDir");
    dn_radius = p_GetUniformLocation(g_dnProg, "uRadius");
    dn_dtol   = p_GetUniformLocation(g_dnProg, "uDepthTol");
    dn_ntol   = p_GetUniformLocation(g_dnProg, "uNormalTol");
    LogR("rtao denoise: separable cross-bilateral ready");
    return 1;
}

// Horizontal into A, vertical into B. `src` is the freshly accumulated AO.
static void RunDenoise(GLuint src, int tw, int th) {
    if (!g_dnOn || !BuildDnProgram()) return;
    if (!g_dnTexA || tw != g_aoW || th != g_aoH) {
        if (!g_dnTexA) glGenTextures(1, &g_dnTexA);
        if (!g_dnTexB) glGenTextures(1, &g_dnTexB);
        GLuint pair[2] = { g_dnTexA, g_dnTexB };
        for (int i = 0; i < 2; i++) {
            glBindTexture(GL_TEXTURE_2D, pair[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, 0x881A /*RGBA16F*/, tw, th, 0, GL_RGBA, 0x1406 /*GL_FLOAT*/, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
        }
    }
    p_UseProgram(g_dnProg);
    if (dn_res    >= 0) p_Uniform2f(dn_res, (float)tw, (float)th);
    if (dn_radius >= 0) p_Uniform1f(dn_radius, g_dnRadius);
    if (dn_dtol   >= 0) p_Uniform1f(dn_dtol, g_dnDepthTol);
    if (dn_ntol   >= 0) p_Uniform1f(dn_ntol, g_dnNormalTol);
    if (dn_src    >= 0 && p_Uniform1i) p_Uniform1i(dn_src, 6);   // reuse unit 6
    GLuint from = src;
    for (int pass = 0; pass < 2; pass++) {
        if (p_ActiveTexture) {
            p_ActiveTexture(0x84C0 + 6);
            glBindTexture(GL_TEXTURE_2D, from);
            p_ActiveTexture(0x84C0);
        }
        GLuint to = pass ? g_dnTexB : g_dnTexA;
        p_BindImageTexture(0, to, 0, GL_FALSE, 0, 0x88B9, 0x881A /*RGBA16F*/);
        if (dn_dir >= 0) p_Uniform2f(dn_dir, pass ? 0.0f : 1.0f, pass ? 1.0f : 0.0f);
        p_DispatchCompute((tw + 7) / 8, (th + 7) / 8, 1);
        p_MemoryBarrier(0xFFFFFFFF);
        from = to;
    }
}

// Build the AO program on first use; same non-fatal contract as everything
// else here - a failure disables the stage rather than the game.
static int BuildAoProgram(void) {
    if (g_aoProg) return 1;
    static int tried = 0;
    if (tried) return 0;
    tried = 1;
    GLuint sh = p_CreateShader(0x91B9);
    p_ShaderSource(sh, 1, &kAoCs, 0);
    p_CompileShader(sh);
    GLint ok = 0; p_GetShaderiv(sh, 0x8B81, &ok);
    if (!ok) {
        char log[900]; log[0] = 0;
        p_GetShaderInfoLog(sh, sizeof(log) - 1, 0, log);
        char b[1000]; wsprintfA(b, "rtao: shader FAILED: %s", log);
        LogR(b);
        p_DeleteShader(sh);
        return 0;
    }
    g_aoProg = p_CreateProgram();
    p_AttachShader(g_aoProg, sh);
    p_LinkProgram(g_aoProg);
    GLint lk = 0; p_GetProgramiv(g_aoProg, 0x8B82, &lk);
    p_DeleteShader(sh);
    if (!lk) { LogR("rtao: link FAILED"); g_aoProg = 0; return 0; }
    ao_invVP   = p_GetUniformLocation(g_aoProg, "uInvVP");
    ao_res     = p_GetUniformLocation(g_aoProg, "uRes");
    ao_depth   = p_GetUniformLocation(g_aoProg, "uDepth");
    ao_near    = p_GetUniformLocation(g_aoProg, "uNear");
    ao_far     = p_GetUniformLocation(g_aoProg, "uFar");
    ao_dinv    = p_GetUniformLocation(g_aoProg, "uDepthInvert");
    ao_dflip   = p_GetUniformLocation(g_aoProg, "uDepthFlipV");
    ao_radius  = p_GetUniformLocation(g_aoProg, "uRadius");
    ao_rays    = p_GetUniformLocation(g_aoProg, "uRayCount");
    ao_frame   = p_GetUniformLocation(g_aoProg, "uFrame");
    ao_strength= p_GetUniformLocation(g_aoProg, "uStrength");
    ao_hist    = p_GetUniformLocation(g_aoProg, "uHist");
    ao_prevVP  = p_GetUniformLocation(g_aoProg, "uPrevVP");
    ao_hasHist = p_GetUniformLocation(g_aoProg, "uHasHist");
    ao_blend   = p_GetUniformLocation(g_aoProg, "uBlend");
    ao_clamp   = p_GetUniformLocation(g_aoProg, "uAoClamp");
    ao_eye     = p_GetUniformLocation(g_aoProg, "uEyeAo");
    ao_prevEye = p_GetUniformLocation(g_aoProg, "uPrevEyeAo");
    ao_hflip   = p_GetUniformLocation(g_aoProg, "uHistFlip");
    ao_reqhit  = p_GetUniformLocation(g_aoProg, "uRequireHit");
    ao_cardop  = p_GetUniformLocation(g_aoProg, "uCardOpacity");
    ao_skipcard= p_GetUniformLocation(g_aoProg, "uSkipCards");
    ao_cardcell= p_GetUniformLocation(g_aoProg, "uCardCell");
    ao_fovscale= p_GetUniformLocation(g_aoProg, "uFovScale");
    ao_geoN    = p_GetUniformLocation(g_aoProg, "uGeoNormal");
    LogR("rtao: program ready");
    return 1;
}

// The texture just WRITTEN this frame (the pair alternates).
unsigned SWSE_RtAoTex() {
    if (!g_built || !g_aoProg) return 0;
    // The denoised result when the spatial pass ran; otherwise the raw
    // accumulation, so a failed shader build degrades instead of blanking.
    if (g_dnOn && g_dnProg && g_dnTexB) return g_dnTexB;
    return g_aoCur ? g_aoTexB : g_aoTex;
}
void SWSE_RtAoHistFlip(float on) { g_histFlip = on; }
void SWSE_RtAoRequireHit(float on) { g_requireHit = on; }
void SWSE_RtCamLog(int frames) { g_camLog = frames; }
void SWSE_RtCardSize(float a) { g_cardSize = a; }
void SWSE_RtCardAny(int on)   { g_cardAny = on ? 1 : 0; }
void SWSE_RtCardOpacity(float o) { g_cardOpacity = (o < 0.0f) ? 0.0f : (o > 1.0f ? 1.0f : o); }
void SWSE_RtClipYNeg(int on) { g_clipYNeg = on ? 1 : 0; }
void SWSE_RtErrProbe() { g_errProbe = 1; }
void SWSE_RtPairPrev(int on) { g_pairPrev = on ? 1 : 0; }
void SWSE_RtSkipCards(float on) { g_skipCards = on; }
void SWSE_RtCardCell(float c) { if (c > 0.0f) g_cardCell = c; }
void SWSE_RtFovScale(float k) { if (k > 0.05f) g_fovScale = k; }

// MEASURE the true field of view instead of trusting either source. Step the
// FOV scale across a range, read the mean |Pdepth - hp| at each step, and the
// minimum is the FOV the game actually rendered the depth buffer with - the
// only value at which the depth reconstruction and the BVH agree.
// Runs one step per frame so each measurement uses a settled frame.
static int   g_fovSweep = 0;
static float g_fovBestK = 1.0f, g_fovBestErr = 1e30f;
static const float kFovLo = 0.60f, kFovHi = 2.60f, kFovStep = 0.10f;
void SWSE_RtFovSweep() {
    g_fovSweep = 1;
    g_fovScale = kFovLo;
    g_fovBestK = kFovLo; g_fovBestErr = 1e30f;
    g_errProbe = 1;
}
// Called with the mean error for the current scale; advances the sweep.
static void FovSweepStep(double meanErr) {
    if (!g_fovSweep) return;
    char b[190];
    char ks[24], es[24];
    RtF3(ks, g_fovScale);
    RtF3(es, (float)meanErr);
    wsprintfA(b, "fovsweep: scale %s -> mean err %s", ks, es);
    LogR(b);
    if (meanErr < g_fovBestErr) { g_fovBestErr = meanErr; g_fovBestK = g_fovScale; }
    g_fovScale += kFovStep;
    if (g_fovScale > kFovHi) {
        g_fovSweep = 0;
        g_fovScale = g_fovBestK;
        char bk[24], be[24];
        RtF3(bk, g_fovBestK); RtF3(be, (float)g_fovBestErr);
        wsprintfA(b, "fovsweep: BEST scale %s (mean err %s) - FOV multiplier on tan(fov/2)",
                  bk, be);
        LogR(b);
    } else {
        g_errProbe = 1;      // keep going next frame
    }
}

// Rebuild the BVH when enough NEW geometry has arrived to matter. Rebuilding
// every frame is not an option - it measures 30-50 ms at 60-90K triangles,
// which is a visible hitch - but rebuilding rarely is fine because the world
// is static and new meshes only appear when the player reaches somewhere they
// have not been. Threshold in newly-seen meshes, not frames.
static int g_autoBuild = 0, g_lastBuildMeshes = 0, g_autoThresh = 150;
void SWSE_RtAutoBuild(int on, int threshold) {
    g_autoBuild = on ? 1 : 0;
    if (threshold > 0) g_autoThresh = threshold;
    g_lastBuildMeshes = SWSE_GeoHarvestMeshCount();
}
void SWSE_RtAutoBuildTick() {
    if (!g_autoBuild) return;
    int n = SWSE_GeoHarvestMeshCount();
    if (n - g_lastBuildMeshes < g_autoThresh) return;
    g_lastBuildMeshes = n;
    char b[160];
    wsprintfA(b, "rt: auto-rebuild, %d meshes collected", n);
    LogR(b);
    SWSE_RtBuild(0);
}
void SWSE_RtAoDenoise(float on, float radius, float depthTol, float normalTol) {
    g_dnOn = (on != 0.0f);
    if (radius    > 0.0f) g_dnRadius    = (radius > 8.0f) ? 8.0f : radius;
    if (depthTol  > 0.0f) g_dnDepthTol  = depthTol;
    if (normalTol > 0.0f) g_dnNormalTol = normalTol;
}
void SWSE_RtAoBlend(float b) { if (b > 0.0f && b <= 1.0f) g_aoBlend = b; }
// How far a single frame's history may pull the current estimate. 0 = the old
// unbounded behaviour (history replaces the value outright when the identity
// test accepts). 0.15 is the value the approved V2 stack shipped with.
void SWSE_RtAoClamp(float c) { g_aoClamp = (c > 0.0f) ? c : 0.0f; }
float SWSE_RtAoGetClamp(void) { return g_aoClamp; }
void SWSE_RtAoGeoNormal(float on) { g_aoGeoNormal = on; }
void SWSE_RtAoParams(float radius, float rays, float strength) {
    if (radius > 0) g_aoRadius = radius;
    if (rays > 0)   g_aoRays = rays;
    g_aoStrength = strength;      // negative values select a diagnostic view
}

void SWSE_RtAo(int w, int h, unsigned depthTex, float depthInvert, float depthFlipV,
               float nearZ, float farZ) {
    if (!g_built || !depthTex) return;
    if (!BuildAoProgram()) return;
    int tw = w / 2, th = h / 2;          // half res, denoised afterwards
    if (tw < 16 || th < 16) return;

    // Derive this frame's camera before using it - see the note in SWSE_RtTrace.
    // Reading SWSE_WindClipVPLast alone gave the AO a matrix frozen at harvest
    // time, so every reconstructed world position drifted as the player moved.
    float n_, f_, fv_, asp_; unsigned pr_ = 0; int cv_, gp_ = 0;
    SWSE_WindClipCamera(&n_, &f_, &fv_, &asp_, &pr_, &cv_, &gp_);
    float vp[16], clip[16], inv[16];
    if (!SWSE_WindClipVPLast(vp)) return;
    for (int i = 0; i < 16; i++) clip[i] = vp[i];
    if (g_clipYNeg) for (int i = 4; i < 8; i++) clip[i] = -clip[i];
    // PAIR THE DEPTH WITH ITS OWN FRAME'S CAMERA. Measured: the reconstruction
    // error is 2.11 units stationary and 4.8-6.3 the moment the camera pitches
    // - a spike that exists ONLY during motion is a frame mismatch between the
    // depth buffer and the matrix unprojecting it. The depth copy is a frame
    // older than the locals we read the camera from, so unprojecting with the
    // CURRENT matrix displaces every reconstructed position by one frame of
    // camera motion: shadows smear across buildings and grass under a slight
    // tilt and settle when still - the reported bug, finally as a number.
    // g_prevVP is last frame's matrix (stored below, same y-neg convention),
    // i.e. the camera that actually rendered the depth we are about to read.
    float useClip[16];
    for (int i = 0; i < 16; i++)
        useClip[i] = (g_pairPrev && g_hasPrevVP) ? g_prevVP[i] : clip[i];
    if (!SWSE_GeoInvert4x4(useClip, inv)) return;
    DeriveEye(inv);
    // Is the camera we derive stable, and does it TRACK PITCH? Logged after
    // the inverse exists because the view direction comes from unprojecting.
    if (g_camLog > 0) {
          g_camLog--;
          // Log the VIEW DIRECTION, not just the eye. Pitch changes where the
          // camera LOOKS; whether the eye also translates depends on whether
          // the game orbits or pitches in place. Measuring only the eye led to
          // a wrong conclusion that the camera "does not pitch" - the vertical
          // component of the forward vector is the quantity that must change.
          float na[3], nb[3];
          UnprojRt(inv, 0.0f, 0.0f, -1.0f, na);
          UnprojRt(inv, 0.0f, 0.0f,  1.0f, nb);
          float fx = nb[0]-na[0], fy = nb[1]-na[1], fz = nb[2]-na[2];
          float fl = (float)sqrt(fx*fx + fy*fy + fz*fz);
          if (fl > 1e-6f) { fx /= fl; fy /= fl; fz /= fl; }
          char b[240], e0[24], e1[24], e2[24], d0[24], d1[24], d2[24];
          RtF3(e0, g_eye[0]); RtF3(e1, g_eye[1]); RtF3(e2, g_eye[2]);
          RtF3(d0, fx); RtF3(d1, fy); RtF3(d2, fz);
          // WHERE DOES OUR MATRIX PUT THE PLAYER ON SCREEN? This is the
          // unambiguous test. In a third-person game the character sits near
          // the middle of the frame, so a correct view-projection must put the
          // player near NDC (0,0). Inferring the error from dot products got
          // the axis label wrong twice; this measures it directly.
          float pw[3]; char n0[24], n1[24];
          if (SWSE_PosGet(pw) == 1) {
              float cx = clip[0]*pw[0] + clip[1]*pw[1] + clip[2]*pw[2]  + clip[3];
              float cy = clip[4]*pw[0] + clip[5]*pw[1] + clip[6]*pw[2]  + clip[7];
              float cwp= clip[12]*pw[0]+ clip[13]*pw[1]+ clip[14]*pw[2] + clip[15];
              if (cwp > 1e-4f) { RtF3(n0, cx/cwp); RtF3(n1, cy/cwp); }
              else { lstrcpyA(n0, "behind"); lstrcpyA(n1, "behind"); }
          } else { lstrcpyA(n0, "?"); lstrcpyA(n1, "?"); }
          wsprintfA(b, "camlog: prog %u cons %d fwd (%s,%s,%s) playerNDC (%s,%s)",
                    pr_, gp_, d0, d1, d2, n0, n1);
          LogR(b);
    }
    GLint prevProg = 0, prevTex = 0;
    glGetIntegerv(0x8B8D, &prevProg);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);

    if (!g_aoTex || tw != g_aoW || th != g_aoH) {
        if (!g_aoTex)  glGenTextures(1, &g_aoTex);
        if (!g_aoTexB) glGenTextures(1, &g_aoTexB);
        GLuint pair[2] = { g_aoTex, g_aoTexB };
        for (int i = 0; i < 2; i++) {
            glBindTexture(GL_TEXTURE_2D, pair[i]);
            // RGBA16F: the .g identity marker needs better than 3.92 units/step
            glTexImage2D(GL_TEXTURE_2D, 0, 0x881A /*RGBA16F*/, tw, th, 0, GL_RGBA, 0x1406 /*GL_FLOAT*/, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
        }
        g_aoW = tw; g_aoH = th;
        g_hasPrevVP = 0;                 // history is meaningless after a resize
    }
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);

    // Alternate: write into one, read the other as history.
    g_aoCur = !g_aoCur;
    GLuint writeTex = g_aoCur ? g_aoTexB : g_aoTex;
    GLuint histTex  = g_aoCur ? g_aoTex  : g_aoTexB;

    p_UseProgram(g_aoProg);
    p_BindBufferBase(0x90D2, 0, g_bufNodes);
    p_BindBufferBase(0x90D2, 1, g_bufTris);
    p_BindBufferBase(0x90D2, 2, g_bufOrder);
    p_BindImageTexture(0, writeTex, 0, GL_FALSE, 0, 0x88B9, 0x881A /*RGBA16F*/);
    if (p_ActiveTexture) {                     // history on unit 6
        p_ActiveTexture(0x84C0 + 6);
        glBindTexture(GL_TEXTURE_2D, histTex);
        p_ActiveTexture(0x84C0);
    }
    if (ao_hist    >= 0 && p_Uniform1i) p_Uniform1i(ao_hist, 6);
    if (ao_hasHist >= 0) p_Uniform1f(ao_hasHist, g_hasPrevVP ? 1.0f : 0.0f);
    if (ao_blend   >= 0) p_Uniform1f(ao_blend, g_aoBlend);
    if (ao_clamp   >= 0) p_Uniform1f(ao_clamp, g_aoClamp);
    if (ao_prevVP  >= 0) p_UniformMatrix4fv(ao_prevVP, 1, GL_TRUE, g_prevVP);
    if (ao_eye     >= 0 && p_Uniform3f) p_Uniform3f(ao_eye, g_eye[0], g_eye[1], g_eye[2]);
    if (ao_prevEye >= 0 && p_Uniform3f) p_Uniform3f(ao_prevEye, g_prevEye[0], g_prevEye[1], g_prevEye[2]);
    if (ao_hflip   >= 0) p_Uniform1f(ao_hflip, g_histFlip);
    if (ao_reqhit  >= 0) p_Uniform1f(ao_reqhit, g_requireHit);
    if (ao_cardop  >= 0) p_Uniform1f(ao_cardop, g_cardOpacity);
    if (ao_skipcard>= 0) p_Uniform1f(ao_skipcard, g_skipCards);
    if (ao_cardcell>= 0) p_Uniform1f(ao_cardcell, g_cardCell);
    if (ao_fovscale>= 0) p_Uniform1f(ao_fovscale, g_fovScale);
    if (ao_geoN    >= 0) p_Uniform1f(ao_geoN, g_aoGeoNormal);
    // the game's depth on unit 7, clear of every other stage's slots
    if (p_ActiveTexture) {
        p_ActiveTexture(0x84C0 + 7);
        glBindTexture(GL_TEXTURE_2D, (GLuint)depthTex);
        glTexParameteri(GL_TEXTURE_2D, 0x884C /*COMPARE_MODE*/, GL_NONE);
        p_ActiveTexture(0x84C0);
    }
    if (ao_depth   >= 0 && p_Uniform1i) p_Uniform1i(ao_depth, 7);
    if (ao_invVP   >= 0) p_UniformMatrix4fv(ao_invVP, 1, GL_TRUE, inv);
    if (ao_res     >= 0) p_Uniform2f(ao_res, (float)tw, (float)th);
    if (ao_near    >= 0) p_Uniform1f(ao_near, nearZ);
    if (ao_far     >= 0) p_Uniform1f(ao_far, farZ);
    if (ao_dinv    >= 0) p_Uniform1f(ao_dinv, depthInvert);
    if (ao_dflip   >= 0) p_Uniform1f(ao_dflip, depthFlipV);
    if (ao_radius  >= 0) p_Uniform1f(ao_radius, g_aoRadius);
    if (ao_rays    >= 0) p_Uniform1f(ao_rays, g_aoRays);
    if (ao_strength>= 0) p_Uniform1f(ao_strength, g_aoStrength);
    if (ao_frame   >= 0) p_Uniform1f(ao_frame, (float)(g_aoFrame++ & 63) * 0.015625f);
    p_DispatchCompute((tw + 7) / 8, (th + 7) / 8, 1);
    p_MemoryBarrier(0xFFFFFFFF);
    // Spatial pass over the freshly accumulated result. Runs AFTER the barrier
    // so it reads finished data, and writes to its own textures so next
    // frame's history still sees the unblurred accumulation.
    RunDenoise(writeTex, tw, th);
    // CAMERA ERROR AS A NUMBER. The -6 debug view writes |Pdepth - hp| into
    // red (/10) and green (raw, clamped). Reading the buffer back and
    // averaging turns "looks darker" into a figure that can be compared across
    // convention settings. If our VP is exact this goes to ~0, because the
    // harvest and the AO then land in the same true world space.
    if (g_errProbe) {
        g_errProbe = 0;
        int n = tw * th;
        unsigned char* px = (unsigned char*)malloc((size_t)n * 4);
        if (px) {
            glBindTexture(GL_TEXTURE_2D, writeTex);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
            glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
            // -7 mode: red = the primary ray found no BVH surface at all.
            // Sky is (255,255,255) from the early-out, so green separates it.
            if (g_aoStrength < -6.5f && g_aoStrength > -7.5f) {
                int miss = 0, tot = 0;
                for (int i = 0; i < n; i++) {
                    if (px[i*4+1] > 200) continue;              // sky
                    tot++;
                    if (px[i*4+0] > 128) miss++;
                }
                char mb[190];
                wsprintfA(mb, "bvhmiss: %d%% of visible pixels have NO BVH surface (%d of %d)",
                          (tot > 0) ? (miss * 100 / tot) : 0, miss, tot);
                LogR(mb);
                free(px);
                p_UseProgram((GLuint)prevProg);
                for (int i = 0; i < 16; i++) g_prevVP[i] = clip[i];
                for (int i = 0; i < 3; i++)  g_prevEye[i] = g_eye[i];
                g_hasPrevVP = 1;
                return;
            }
            double sum = 0.0; int cnt = 0, big = 0;
            // Split by NEAR/FAR using the blue channel, which the -6 view
            // leaves at 0 - so instead bucket by the error itself to see the
            // shape of the distribution. A few large outliers and a mostly
            // clean field is a different bug from a uniform offset.
            int hist[5] = {0,0,0,0,0};      // <0.1, <0.5, <1, <5, >=5 units
            for (int i = 0; i < n; i++) {
                // Skip sky: it is written as pure white by the early-out.
                if (px[i*4+0] == 255 && px[i*4+1] == 255) continue;
                double d = (double)px[i*4+0] / 255.0 * 10.0;   // red = d/10
                sum += d; cnt++;
                if (d > 1.0) big++;
                if      (d < 0.1) hist[0]++;
                else if (d < 0.5) hist[1]++;
                else if (d < 1.0) hist[2]++;
                else if (d < 5.0) hist[3]++;
                else              hist[4]++;
            }
            if (cnt > 0) {
                char hb[200];
                wsprintfA(hb, "camerr hist: <0.1u %d%%  <0.5u %d%%  <1u %d%%  <5u %d%%  >=5u %d%%",
                          hist[0]*100/cnt, hist[1]*100/cnt, hist[2]*100/cnt,
                          hist[3]*100/cnt, hist[4]*100/cnt);
                LogR(hb);
            }
            char b[200];
            int mean_m = (cnt > 0) ? (int)(sum / cnt * 1000.0) : -1;
            wsprintfA(b, "camerr: mean |Pdepth-hp| = %d.%03d units over %d px; %d%% exceed 1 unit"
                         "  (clipYNeg %d, depthFlipV %d)",
                      mean_m / 1000, mean_m % 1000, cnt,
                      (cnt > 0) ? (big * 100 / cnt) : 0, g_clipYNeg, (int)depthFlipV);
            LogR(b);
            FovSweepStep((cnt > 0) ? (sum / cnt) : 1e30);
            free(px);
        }
    }
    p_UseProgram((GLuint)prevProg);

    // This frame's camera becomes next frame's reprojection matrix. The EYE
    // must travel with it: the distances just written into .g were measured
    // from here, so next frame has to compare against this position, not its
    // own. Keeping only the matrix is what let the AO slide when the camera
    // turned - a third-person orbit moves the eye on every mouse motion.
    for (int i = 0; i < 16; i++) g_prevVP[i] = clip[i];
    for (int i = 0; i < 3; i++)  g_prevEye[i] = g_eye[i];
    g_hasPrevVP = 1;
}

void SWSE_RtStatus(SWSE_RtEmit emit) {
    char b[220];
    if (!emit) return;
    wsprintfA(b, "rt: built %s, %d nodes, depth %d, %d triangles",
              g_built ? "yes" : "no", g_nNodes, g_maxDepth, g_nTris);
    emit(b);
    wsprintfA(b, "rt: trace target %dx%d, texture %u", g_texW, g_texH, g_tex);
    emit(b);
    // The derived eye, printed for direct comparison with `pos`. This is the
    // acceptance number for the ray setup: it should sit within a few units of
    // the player, slightly above and behind them in third person.
    {
        char x[24], y[24], z[24];
        for (int i = 0; i < 3; i++) {
            float v = g_eye[i];
            float av = v < 0 ? -v : v;
            char* d = (i == 0) ? x : (i == 1) ? y : z;
            wsprintfA(d, "%s%d.%02d", v < 0 ? "-" : "", (int)av, (int)((av - (int)av) * 100));
        }
        wsprintfA(b, "rt: derived eye = (%s, %s, %s)   <- compare with 'pos'", x, y, z);
        emit(b);
    }
    emit("rt: 'harvest' then 'rt build', then set debug_rt 1 to see the traced world");
}
