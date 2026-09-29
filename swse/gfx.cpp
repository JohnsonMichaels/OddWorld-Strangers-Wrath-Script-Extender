// SWSE graphics pipeline (M3).
//
// Technique (works on the game's GL2/Cg-era pipeline, no FBO needed):
//   1. glCopyTexSubImage2D copies the finished back buffer into our texture.
//   2. We draw a full-screen quad with our fragment shader sampling that
//      texture, which overwrites the screen with the processed image.
//
// The fragment shader is GLSL 1.20 and reads gl_TexCoord[0] from the
// fixed-function vertex path, so no vertex shader is required.
//
// M3 uses an embedded, deliberately-visible shader (saturation + contrast) to
// prove the pipeline end-to-end. M3.5 swaps this for the graphics-mod shader
// files (sharpen/bloom/ssao/ssgi) and multi-pass config.

#include "gfx.h"
#include "materials.h"
#include "raytrace.h"
#include "features.h"     // FEAT_RAYTRACE gates every call into the tracer
#include "modregistry.h"
#include "glspy.h"
#include "wind.h"
#include <gl/GL.h>
#include <string>
#include <fstream>
#include <cstdio>
#include <cstring>
#include <cstdlib>        // strtod: `set` checks its value is a number

#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "user32.lib")

// ---- GL constants not in the 1.1 header ---------------------------------
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER   0x8B31
#define GL_COMPILE_STATUS  0x8B81
#define GL_LINK_STATUS     0x8B82
#define GL_CLAMP_TO_EDGE   0x812F
#define GL_TEXTURE0        0x84C0
#define GL_CURRENT_PROGRAM 0x8B8D
#define GL_ACTIVE_TEXTURE  0x84E0
#define GL_VERTEX_PROGRAM_ARB   0x8620
#define GL_FRAGMENT_PROGRAM_ARB 0x8804

typedef char GLchar;
typedef void (APIENTRY* PFNGLACTIVETEXTURE)(GLenum);
static PFNGLACTIVETEXTURE p_glActiveTexture = nullptr;

// ---- FBO entry points, for the pre-UI pass -------------------------------
// Needed so we can attach the game's finished scene-colour texture to an FBO of
// our own and process it BEFORE the game composites the frame and draws UI.
// Doing it at swap time (after UI exists) is what makes scenery ghost through
// the inventory screen: at UI pixels the depth buffer holds the world behind.
#define GL_FRAMEBUFFER_E        0x8D40
#define GL_COLOR_ATTACHMENT0_E  0x8CE0
#define GL_FRAMEBUFFER_COMPLETE_E 0x8CD5
#define GL_FRAMEBUFFER_BINDING_E  0x8CA6
typedef void   (APIENTRY* PFNGLGENFRAMEBUFFERS)(GLsizei, GLuint*);
typedef void   (APIENTRY* PFNGLBINDFRAMEBUFFER)(GLenum, GLuint);
typedef void   (APIENTRY* PFNGLFRAMEBUFFERTEXTURE2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum (APIENTRY* PFNGLCHECKFRAMEBUFFERSTATUS)(GLenum);
static PFNGLGENFRAMEBUFFERS        p_glGenFramebuffers        = nullptr;
static PFNGLBINDFRAMEBUFFER        p_glBindFramebuffer        = nullptr;
static PFNGLFRAMEBUFFERTEXTURE2D   p_glFramebufferTexture2D   = nullptr;
static PFNGLCHECKFRAMEBUFFERSTATUS p_glCheckFramebufferStatus = nullptr;

// ---- GL2.0 entry points (resolved via wglGetProcAddress) ----------------
typedef GLuint (APIENTRY* PFNGLCREATESHADER)(GLenum);
typedef void   (APIENTRY* PFNGLSHADERSOURCE)(GLuint, GLsizei, const GLchar* const*, const GLint*);
typedef void   (APIENTRY* PFNGLCOMPILESHADER)(GLuint);
typedef void   (APIENTRY* PFNGLGETSHADERIV)(GLuint, GLenum, GLint*);
typedef void   (APIENTRY* PFNGLGETSHADERINFOLOG)(GLuint, GLsizei, GLsizei*, GLchar*);
typedef GLuint (APIENTRY* PFNGLCREATEPROGRAM)(void);
typedef void   (APIENTRY* PFNGLATTACHSHADER)(GLuint, GLuint);
typedef void   (APIENTRY* PFNGLLINKPROGRAM)(GLuint);
typedef void   (APIENTRY* PFNGLGETPROGRAMIV)(GLuint, GLenum, GLint*);
typedef void   (APIENTRY* PFNGLUSEPROGRAM)(GLuint);
typedef GLint  (APIENTRY* PFNGLGETUNIFORMLOCATION)(GLuint, const GLchar*);
typedef void   (APIENTRY* PFNGLUNIFORM1F)(GLint, GLfloat);
typedef void   (APIENTRY* PFNGLUNIFORM2F)(GLint, GLfloat, GLfloat);
typedef void   (APIENTRY* PFNGLUNIFORM1I)(GLint, GLint);

static PFNGLCREATESHADER       p_glCreateShader;
static PFNGLSHADERSOURCE       p_glShaderSource;
static PFNGLCOMPILESHADER      p_glCompileShader;
static PFNGLGETSHADERIV        p_glGetShaderiv;
static PFNGLGETSHADERINFOLOG   p_glGetShaderInfoLog;
static PFNGLCREATEPROGRAM      p_glCreateProgram;
static PFNGLATTACHSHADER       p_glAttachShader;
static PFNGLLINKPROGRAM        p_glLinkProgram;
static PFNGLGETPROGRAMIV       p_glGetProgramiv;
static PFNGLUSEPROGRAM         p_glUseProgram;
static PFNGLGETUNIFORMLOCATION p_glGetUniformLocation;
static PFNGLUNIFORM1F          p_glUniform1f;
static PFNGLUNIFORM2F          p_glUniform2f;
static PFNGLUNIFORM1I          p_glUniform1i;

static GLuint g_prog = 0, g_tex = 0;
static int    g_texW = 0, g_texH = 0;   // power-of-two texture dims
static bool   g_texHasA = false;        // capture texture allocated as RGBA?
static int    g_frameW = 0, g_frameH = 0; // actual captured frame dims
static unsigned char* g_cpu = nullptr;  // CPU frame buffer for the read/process/draw path
static int    g_cpuSize = 0;
static bool   g_ready = false;

static int NextPOT(int v) { int p = 1; while (p < v) p <<= 1; return p; }
static bool   g_enabled = false;   // start OFF - game renders normally until toggled
// Re-run the depth-texture choice on the next frame that needs it.
static bool   g_needDepthPick = true;
static bool   g_prevKey = false;
static GLint  u_scene = -1, u_texel = -1;

static void SetAllUniforms(int w, int h, bool haveDepth);   // defined below

static void Log(const std::string& s) {
    char path[MAX_PATH];
    GetModuleFileNameA(GetModuleHandleA(NULL), path, MAX_PATH);
    std::string p(path);
    std::ofstream f(p.substr(0, p.find_last_of("\\/")) + "\\swse_log.txt", std::ios::app);
    f << s << "\n";
}

// C-string logger safe to call inside __try/__except (no C++ unwinding).
static void LogC(const char* s) {
    char path[MAX_PATH];
    GetModuleFileNameA(GetModuleHandleA(NULL), path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) *slash = 0;
    char full[MAX_PATH];
    wsprintfA(full, "%s\\swse_log.txt", path);
    HANDLE h = CreateFileA(full, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wr; char line[256];
        int n = wsprintfA(line, "%s\r\n", s);
        SetFilePointer(h, 0, NULL, FILE_END);
        WriteFile(h, line, n, &wr, NULL);
        CloseHandle(h);
    }
}

// Vertex shader: transform the quad AND forward the texture coordinate we set
// per-vertex (glTexCoord2f -> gl_MultiTexCoord0) to the fragment shader as a
// varying. This is the robust path - it doesn't depend on the driver's
// fixed-function gl_TexCoord interpolation (solid-color bug) or gl_FragCoord
// (shear bug). We do our own transform so the game's ARB program state is
// irrelevant.
// Vertex shader: pass gl_Vertex STRAIGHT THROUGH as clip-space coords. We feed
// vertices already in NDC (-1..1), so there is NO matrix transform of any kind
// -> the game's matrix/ARB state cannot skew our fullscreen quad. UV forwarded
// as a varying (the only reliable texcoord path on this driver).
static const char* kVertShader =
    "#version 120\n"
    "varying vec2 vUV;\n"
    "void main(){\n"
    "  gl_Position = gl_Vertex;\n"          // already in clip space, no transform
    "  vUV = gl_MultiTexCoord0.xy;\n"
    "}\n";

// Fragment: the SWSE Graphics "reliably beautiful" stack. Effects that
// don't depend on fragile depth-normal reconstruction - sharpen, gentle
// contact-shadow AO, soft bloom, filmic tonemap + colour grade, vignette -
// each gated by an enable uniform and fed live from settings.txt. One pass.
static const char* kProofShader =
    "#version 120\n"
    "uniform sampler2D uScene;\n"
    "uniform sampler2D uDepth;\n"      // scene depth (0=near .. 1=far)
    "uniform vec2  uTexel;\n"          // (1/w, 1/h)
    "uniform float uIntensity;\n"      // master blend vanilla<->graded
    "uniform float uHasDepth;\n"       // 1 = real scene depth is bound
    // contact-shadow AO (gentle, depth-only)
    "uniform float uAOEnable;\n"
    "uniform float uAOIntensity;\n"
    "uniform float uAORadius;\n"
    "uniform float uNear;\n"
    "uniform float uFar;\n"
    // soft bloom
    "uniform float uBloomEnable;\n"
    "uniform float uBloomThreshold;\n"
    "uniform float uBloomIntensity;\n"
    "uniform float uBloomRadius;\n"
    // RTGI / SSGI (hemisphere indirect-light bounce - optional, experimental)
    "uniform float uSSGIEnable;\n"
    "uniform float uSSGIIntensity;\n"
    "uniform float uSSGIRadius;\n"
    "uniform float uSSGIThickness;\n"  // max occluder depth: stops silhouette halos
    "uniform float uSSGIMaxScreen;\n"  // max gather radius as a fraction of the screen
    "uniform int   uSSGISamples;\n"
    "uniform float uDebugGI;\n"  // 1 = visualize the RTGI pass output
    "uniform float uDebugNormals;\n" // 1 = normals x-ray (view normals as colour)
    // GTAO-family horizon AO: replaces the tap-count AO when enabled. Consumes
    // the reconstructed normals + true camera - the two inputs that did not
    // exist before this rebuild.
    "uniform float uGTAOEnable;\n"
    "uniform float uGTAOIntensity;\n"
    "uniform float uGTAORadius;\n"     // world-units gather radius
    "uniform float uGTAODirs;\n"       // directions over the full circle (2..4)
    "uniform float uGTAOSteps;\n"      // march steps per direction (2..8)
    "uniform float uDebugAO;\n"        // 1 = grayscale AO x-ray
    // HBIL: indirect bounce riding the SAME horizon scan. Each march step that
    // RAISES the horizon just revealed an occluder surface; its colour bounces
    // onto the receiver, weighted by the solid angle it subtends (the horizon
    // delta). Directional colour bleed for the ray budget already spent.
    "uniform float uHBILEnable;\n"
    "uniform float uHBILIntensity;\n"
    "uniform float uDebugBounce;\n"    // 1 = bounce-only x-ray
    "uniform sampler2D uGITex;\n"     // the pre-pass output (unit 2)
    // SSR - wet-ground screen-space reflections. v1 applies to upward-
    // facing surfaces (dot(N, uViewUp) gate); material-tagged surfaces
    // follow once the MaterialDef/fingerprint map is built with the owner.
    "uniform float uSSREnable;\n"
    "uniform float uSSRIntensity;\n"
    "uniform float uSSRSteps;\n"
    "uniform float uSSRThickness;\n"
    "uniform float uSSRUpDot;\n"
    "uniform vec3  uViewUp;\n"
    "uniform float uDebugSSR;\n"
    // Material mask: reflective surfaces stamped their coverage into dest
    // alpha during scene draws; the RGBA capture carries it here in uScene.a.
    "uniform float uSSRMaskUse;\n"
    "uniform float uSSRMaskFresnel;\n"
    "uniform float uDebugSSRMask;\n"
    // The mask is read from the game's scene FBO color attachment (unit 3),
    // not the backbuffer capture: the stamps land there, and the engine's
    // own blit drops alpha before the backbuffer (measured: capture alpha
    // all-zero with 79 reflective binds/frame). Same V-flip as the depth
    // texture - both are attachments of the same FBO.
    "uniform sampler2D uSceneFBO;\n"
    "uniform float uHasFBOMask;\n"
    // True-normal G-buffer (unit 5): rgb = packed world normal, a = coverage
    // (0 where no world draw landed - characters, sky, GLSL-drawn things).
    "uniform sampler2D uGBufTex;\n"
    "uniform float uHasGBuf;\n"
    "uniform float uDebugGNormals;\n"
    // Stage 1b: the compute tracer's own view of the world (unit 6).
    "uniform sampler2D uRtTex;\n"
    "uniform float uDebugRt;\n"
    // Stage 2: ray-traced AO on unit 8, and its x-ray.
    "uniform sampler2D uRtaoTex;\n"
    "uniform float uRtaoUse;\n"
    "uniform float uRtaoFlipV;\n"
    "uniform float uDebugRtao;\n"
    "uniform float uFov;\n"      // vertical FOV (deg) for position reconstruction
    "uniform float uAspect;\n"   // width/height
    // sharpen
    "uniform float uSharpenEnable;\n"
    "uniform float uSharpenStrength;\n"
    // tonemap + colour grade
    "uniform float uGradeEnable;\n"
    "uniform float uExposure;\n"
    "uniform float uTonemap;\n"
    "uniform float uSaturation;\n"
    "uniform float uContrast;\n"
    "uniform float uBrightness;\n"
    "uniform float uTemperature;\n"
    "uniform float uVignette;\n"
    "varying vec2 vUV;\n"
    "uniform float uAOSamples;\n"     // AO sample count: higher = less dither grid
    // Anything closer than this (world units) is the first-person weapon, not
    // scene geometry, and is excluded from every depth-based effect.
    "uniform float uNearCutoff;\n"
    // far-field detail softening (distances are REAL world units from the camera)
    "uniform float uDofEnable;\n"
    "uniform float uDofStart;\n"      // world distance where softening begins
    "uniform float uDofEnd;\n"        // world distance of full softening
    "uniform float uDofStrength;\n"   // max blur radius in texels
    "uniform float uDepthInvert;\n"   // 1 = treat the depth buffer as reverse-Z
    "uniform float uDepthFlipV;\n"    // 1 = sample depth vertically mirrored
    // Material-mask fetch. Defined HERE, after uDepthFlipV's declaration -
    // defining it up with the SSR uniforms broke the whole composite with
    // 'undeclared identifier' (GLSL reads top-down; the pass silently died).
    // The mask's own flip - NOT uDepthFlipV: this engine's passes disagree
    // about orientation (depth texture mirrored, scene FBO color upright),
    // so the mask FBO gets an independently-verified switch.
    "uniform float uMaskFlipV;\n"
    "float maskAt(vec2 uv){\n"
    "  if (uHasFBOMask < 0.5) return 0.0;\n"
    "  vec2 muv = vec2(uv.x, (uMaskFlipV > 0.5) ? (1.0 - uv.y) : uv.y);\n"
    "  return texture2D(uSceneFBO, muv).r;\n"   // v2: our FBO, white = tagged
    "}\n"
    // Depth fetch, then linearize to world distance.
    //
    // MEASURED by A/B-ing both switches in game:
    //  * invert OFF. This engine uses STANDARD GL depth (near=0, far=1) -- the
    //    scene sits at raw 0.96..0.99. Inverting it and linearizing with the
    //    real near/far collapses the entire frame into z=0.502..0.518, i.e.
    //    flat, which is what made AO saturate over the whole image. This was
    //    THE bug behind "the RTGI looks terrible".
    //  * flipV ON. The game's depth attachment really is mirrored relative to
    //    the captured frame; with it off, geometry ghosts in upside down.
    // Kept as switches because another build or driver could differ.
    "float rawDepth(vec2 uv){\n"
    "  vec2 duv = vec2(uv.x, (uDepthFlipV > 0.5) ? (1.0 - uv.y) : uv.y);\n"
    "  float d = texture2D(uDepth, duv).r;\n"
    "  return (uDepthInvert > 0.5) ? (1.0 - d) : d;\n"
    "}\n"
    "float linDepth(float d){ return uNear*uFar/(uFar-d*(uFar-uNear)); }\n"
    "float depthAt(vec2 uv){ return linDepth(rawDepth(uv)); }\n"
    "float ign(vec2 p){ return fract(52.9829189*fract(dot(p, vec2(0.06711056,0.00583715)))); }\n"
    // ACES-ish filmic tonemap: rolls off highlights, keeps colour, no clipping.
    "vec3 aces(vec3 x){\n"
    "  const float a=2.51,b=0.03,c=2.43,d=0.59,e=0.14;\n"
    "  return clamp((x*(a*x+b))/(x*(c*x+d)+e),0.0,1.0);\n"
    "}\n"
    // ---- RTGI geometry helpers (view-space reconstruction from depth) ----
    "float g_tanHalf;\n"
    "vec3 viewPos(vec2 uv){\n"
    "  float z = depthAt(uv);\n"
    "  vec2 ndc = uv*2.0-1.0;\n"
    "  return vec3(ndc.x*g_tanHalf*uAspect, ndc.y*g_tanHalf, 1.0) * z;\n"
    "}\n"
    "vec2 viewToUV(vec3 vp){\n"
    "  vec2 ndc = vec2(vp.x/(g_tanHalf*uAspect), vp.y/g_tanHalf)/max(vp.z,0.001);\n"
    "  return ndc*0.5+0.5;\n"
    "}\n"
    // Surface normal from depth: multi-texel baseline, closer-neighbour per axis
    // to avoid smearing across depth discontinuities.
    "vec3 viewNormal(vec2 uv, vec3 P){\n"
    "  vec2 e = uTexel * 2.0;\n"
    "  vec3 pL=viewPos(uv-vec2(e.x,0)); vec3 pR=viewPos(uv+vec2(e.x,0));\n"
    "  vec3 pD=viewPos(uv-vec2(0,e.y)); vec3 pU=viewPos(uv+vec2(0,e.y));\n"
    "  vec3 dx = (abs(pR.z-P.z) < abs(P.z-pL.z)) ? (pR-P) : (P-pL);\n"
    "  vec3 dy = (abs(pU.z-P.z) < abs(P.z-pD.z)) ? (pU-P) : (P-pD);\n"
    "  vec3 n = cross(dx, dy);\n"
    "  float L = length(n);\n"
    "  if (L < 1e-6) return vec3(0.0,0.0,-1.0);\n"
    "  n /= L;\n"
    "  if (n.z > 0.0) n = -n;\n"
    "  return n;\n"
    "}\n"
    // cosine-weighted hemisphere sample around N
    "vec3 hemi(float i, vec3 N, float seed){\n"
    "  float u=fract(i*0.618034+seed), v=fract(i*0.375+seed*1.7);\n"
    "  float ph=u*6.2831853; float ct=sqrt(1.0-v); float st=sqrt(v);\n"
    "  vec3 d=vec3(cos(ph)*st, sin(ph)*st, ct);\n"
    "  vec3 up = abs(N.z)<0.9 ? vec3(0,0,1) : vec3(1,0,0);\n"
    "  vec3 T=normalize(cross(up,N)); vec3 B=cross(N,T);\n"
    "  return normalize(d.x*T + d.y*B + d.z*N);\n"
    "}\n"
    "void main(){\n"
    "  vec3 orig = texture2D(uScene, vUV).rgb;\n"
    "  vec3 c = orig;\n"
    "  bool hasD = uHasDepth > 0.5;\n"
    "  float rd  = hasD ? rawDepth(vUV) : 1.0;\n"
    "  bool sky  = hasD && rd >= 0.9999;\n"
    // Normals x-ray - the first consumer viewNormal() has ever had. Smooth
    // surfaces must read as flat colour and edges as crisp colour changes;
    // per-pixel sparkle means the reconstruction or the camera is wrong.
    "  if (uDebugNormals > 0.5 && hasD) {\n"
    "    if (sky) { gl_FragColor = vec4(0.5,0.5,1.0,1.0); return; }\n"
    "    g_tanHalf = tan(radians(uFov)*0.5);\n"
    "    vec3 dbgP = viewPos(vUV);\n"
    "    vec3 dbgN = viewNormal(vUV, dbgP);\n"
    "    gl_FragColor = vec4(dbgN*0.5+0.5, 1.0); return;\n"
    "  }\n"
    "  float aspect = uTexel.x/uTexel.y;\n"   // (1/w)/(1/h)=h/w -> circular kernels
    // --- sharpen FIRST, on the raw scene (keeps the high-pass DC-correct) ---
    "  if (uSharpenEnable > 0.5) {\n"
    "    vec3 n = texture2D(uScene, vUV+vec2(0.0,uTexel.y)).rgb\n"
    "           + texture2D(uScene, vUV-vec2(0.0,uTexel.y)).rgb\n"
    "           + texture2D(uScene, vUV+vec2(uTexel.x,0.0)).rgb\n"
    "           + texture2D(uScene, vUV-vec2(uTexel.x,0.0)).rgb;\n"
    "    c += (orig*4.0 - n) * uSharpenStrength;\n"
    "  }\n"
    // --- gentle contact-shadow AO from depth (subtle; darkens creases only) ---
    // The first-person weapon sits ~1-2 world units from the camera while the
    // world starts around 5+. Screen-space GI has no business shading it: the
    // crossbow and the critters mounted on it occlude EACH OTHER and bleed
    // darkness across their overlap, because the effect cannot tell a held
    // object from scenery. Excluding the near field removes that entirely.
    // near_cutoff is NOT an exclusion any more. Excluding near pixels from the
    // effect stopped the critters on the bow and approaching chickens being lit
    // at all, and they visibly popped in and out. It now means "geometry this
    // close may be LIT but may not OCCLUDE" -- see the sample rejections below.
    // ---- GTAO-family horizon occlusion --------------------------------------
    // Per pixel: reconstruct position P and normal N, then march a few screen
    // directions. For each direction find the HORIZON - the highest angle
    // (in cos space, relative to the view vector V) that nearby geometry
    // subtends - and compare it against the surface's own tangent plane in
    // that direction. Occlusion is how far the horizon rises above the
    // tangent: a genuine visibility arc, not a count of nearer taps.
    // Distance falloff keeps far geometry from occluding as hard as near.
    // GI terms are FETCHED from the pre-pass texture (multi-pass skeleton).
    // The computation lives in kGIShader; temporal and denoise stages will
    // blend that texture before this shader ever reads it.
    "  if ((uGTAOEnable > 0.5 || uHBILEnable > 0.5) && hasD && !sky) {\n"
    "    vec4 gi = texture2D(uGITex, vUV);\n"
    "    if (uDebugAO > 0.5) { gl_FragColor = vec4(gi.a, gi.a, gi.a, 1.0); return; }\n"
    "    if (uDebugBounce > 0.5) { gl_FragColor = vec4(gi.rgb, 1.0); return; }\n"
    "    if (uGTAOEnable > 0.5) c *= gi.a;\n"
    "    if (uHBILEnable > 0.5) c += (orig*0.6+0.4) * gi.rgb;\n"
    "  }\n"
    "  else if (uDebugAO > 0.5 && hasD && !sky) { gl_FragColor = vec4(1.0); return; }\n"
    // old tap-count AO: stands aside when GTAO is on, so the two A/B cleanly
    "  if (uAOEnable > 0.5 && uGTAOEnable < 0.5 && hasD && !sky) {\n"
    "    float lin0 = linDepth(rd);\n"
    // The per-pixel random rotation below is what shows up as a fine diagonal
    // "grid": ign() is interleaved gradient noise, and with too few samples and
    // no denoise pass the jitter never averages out. Variance falls as 1/N, so
    // the sample count is tunable (ao_samples) -- the loop uses a constant
    // bound with an early break because GLSL 1.20 wants constant loop limits.
    "    float ang0 = ign(gl_FragCoord.xy) * 6.2831853;\n"
    "    float occ = 0.0;\n"
    "    int   AON = int(clamp(float(uAOSamples), 4.0, 32.0));\n"
    "    for (int i=0;i<32;i++){\n"
    "      if (i>=AON) break;\n"
    "      float t = (float(i)+0.5)/float(AON);\n"
    // Golden-angle spiral: successive samples land far apart, so the same
    // sample budget covers the disc far more evenly than a linear sweep.
    "      float a = ang0 + float(i)*2.39996323;\n"
    "      float r = uAORadius * 0.012 * (0.35 + t);\n"
    "      vec2 off = vec2(cos(a)*aspect, sin(a)) * r;\n"
    "      float lin = linDepth(rawDepth(vUV + off));\n"
    // The first-person weapon must not CAST occlusion onto the world. Rejecting
    // it as an occluder (rather than excluding near pixels from the effect, as
    // an earlier attempt did) is what lets the critters on the bow and a chicken
    // walking up to you keep their own lighting while the weapon stops smearing
    // a dark blob across the ground beneath it.
    "      if (uNearCutoff > 0.0 && lin < uNearCutoff) continue;\n"
    "      float diff = lin0 - lin;\n"                 // >0 => neighbour nearer
    "      if (diff > 0.02 && diff < uAORadius*3.0)\n"
    "        occ += smoothstep(uAORadius*3.0, 0.02, diff);\n"  // nearer=more occ
    "    }\n"
    "    float ao = 1.0 - (occ/float(AON)) * uAOIntensity;\n"
    "    c *= clamp(ao, 0.35, 1.0);\n"
    "  }\n"
    // --- RTGI: SCREEN-SPACE RAY TRACING (the Gilcher-RTGI technique).
    //     Rays leave each pixel and MARCH through the depth buffer step by
    //     step until they hit real geometry; the hit surface's colour is the
    //     light that bounces back. Long-range colour bleed off walls/rocks -
    //     the actual "light bouncing" look. Depth-only; no normals needed. ---
    // old SSGI march: stands aside when HBIL is on - one bounce system at a time
    "  if (uSSGIEnable > 0.5 && uHBILEnable < 0.5 && hasD && !sky) {\n"
    "    g_tanHalf = tan(radians(uFov)*0.5);\n"
    "    float z0 = depthAt(vUV);\n"                   // linear world depth
    "    float seed = ign(gl_FragCoord.xy);\n"
    "    int RAYS = int(clamp(float(uSSGISamples)/8.0, 2.0, 8.0));\n"
    // how far a ray can travel across the screen (world radius -> UV at depth)
    // THE NEAR-OBJECT BLEED. This clamp used to top out at 0.45, meaning a
    // pixel could gather from 45% of the screen. Screen-space radius grows as
    // objects get closer (radius / z), so anything nearer than ~22 units hit
    // that cap -- and the first-person crossbow at ~1 unit hit it hard. Two
    // critters mounted side by side then sampled each other across half the
    // frame and bled darkness into one another.
    //
    // Bounding the gather fixes that at the source, WITHOUT excluding near
    // geometry from the effect: critters on the bow and a chicken walking up to
    // you keep their lighting instead of popping out of it.
    "    float maxUV = clamp(uSSGIRadius / max(z0*g_tanHalf*2.0, 0.001), 0.03, uSSGIMaxScreen);\n"
    "    vec3 gi = vec3(0.0); float occ = 0.0;\n"
    "    for (int rr=0; rr<8; rr++){\n"
    "      if (rr>=RAYS) break;\n"
    "      float ang = (float(rr)+seed)*6.2831853/float(RAYS);\n"
    "      vec2 dir = vec2(cos(ang)*aspect, sin(ang));\n"
    // ray elevation: mix of rays skimming the surface and rising toward camera
    "      float slope = -0.3 + 1.3*fract(seed + float(rr)*0.618034);\n"
    "      for (int s2=1; s2<=16; s2++){\n"
    "        float t = (float(s2)-0.5+seed)/16.0;\n"
    "        float d = t*t*maxUV;\n"                   // quadratic stride: dense near
    "        vec2 suv = vUV + dir*d;\n"
    "        if (suv.x<0.001||suv.x>0.999||suv.y<0.001||suv.y>0.999) break;\n"
    "        float zray = z0 - t*slope*uSSGIRadius;\n" // ray's own depth along its path
    "        float zs = depthAt(suv);\n"
    // THICKNESS TEST. The old condition was `zs < zray - 0.05`: any nearer
    // surface counted as a blocker, with no upper bound. A character standing
    // in front therefore occluded rays cast by ground pixels far behind it, and
    // its silhouette smeared outward as a dark halo -- the "crown of thorns".
    // A depth buffer only stores the FRONT surface, so a hit is only real if the
    // occluder is plausibly thick; anything further in front is something the
    // ray should pass behind. Both bounds scale with distance because a fixed
    // world-space epsilon is far too tight at the far end of the range.
    // Same rule for the ray march: the weapon may be lit, but it may not block
    // rays cast by the world behind it.
    "        if (uNearCutoff > 0.0 && zs < uNearCutoff) continue;\n"
    "        float dz    = zray - zs;\n"
    "        float bias  = max(0.05, z0*0.01);\n"
    "        float thick = uSSGIThickness * max(1.0, z0*0.05);\n"
    "        if (dz > bias && dz < thick) {\n"         // real occluder -> HIT
    "          float att = 1.0 - t*0.7;\n"             // nearer hits bounce more light
    "          gi  += texture2D(uScene, suv).rgb * att;\n"
    "          occ += (1.0 - t);\n"
    "          break;\n"                               // ray absorbed at first surface
    "        }\n"
    "      }\n"
    "    }\n"
    "    float of  = occ/float(RAYS);\n"               // fraction of rays occluded near
    "    float aoT = pow(clamp(1.0 - of*0.8, 0.0, 1.0), 1.5);\n"
    "    vec3  giL = (gi/float(RAYS)) * uSSGIIntensity;\n"
    "    if (uDebugGI > 0.5) {\n"                      // red=occlusion, green=bounced light
    "      gl_FragColor = vec4(1.0-aoT, dot(giL,vec3(0.5)), 0.0, 1.0); return;\n"
    "    }\n"
    "    c *= max(aoT, 0.35);\n"                       // occlusion darkening
    "    c += (orig*0.6+0.4) * giL;\n"                 // bounce (visible on dark surfaces too)
    "  }\n"
    // --- SSR: wet-ground reflections -----------------------------------
    // Reflect the eye ray off the reconstructed normal, march the depth
    // buffer (same thickness rules as SSGI), shade with a grazing-angle
    // Fresnel so reflections live at shallow angles like real wet ground.
    // Misses and screen edges fade out; sky never reflects into geometry.
    // Mask x-ray: destination alpha as grayscale. White = a surface the
    // owner graded reflective stamped here; solid black = the stamp chain
    // is broken somewhere between glColorMask and the RGBA capture.
    "  if (uDebugSSRMask > 0.5) {\n"
    "    float mv = maskAt(vUV);\n"
    "    gl_FragColor = vec4(mv, mv, mv, 1.0); return;\n"
    "  }\n"
    // G-buffer x-ray: packed normals as color; MAGENTA marks coverage holes
    // (expected: characters, sky). Black = the buffer never filled at all.
    // Ray-traced camera view: brightness is proximity, black is a miss. If
    // this shows the level's silhouette, the BVH, the geometry decode and
    // the camera all agree - the acceptance test for stage 1b.
    "  if (uDebugRt > 0.5) {\n"
    "    gl_FragColor = vec4(texture2D(uRtTex, vUV).rgb, 1.0); return;\n"
    "  }\n"
    // Red only: green carries the eye distance used for history rejection,
    // so showing rgb paints the screen magenta rather than showing occlusion.
    // The AO is written by a compute shader with imageStore at px (y-UP) and
    // read here with vUV. If those two conventions disagree the occlusion is
    // MIRRORED vertically: it lands on the wrong part of the screen and slides
    // the wrong way when the camera pitches, which reads as a texture-style
    // glitch rather than as lighting. Switchable so it can be measured.
    "  vec2 aoUV = (uRtaoFlipV > 0.5) ? vec2(vUV.x, 1.0 - vUV.y) : vUV;\n"
    "  if (uDebugRtao > 0.5) {\n"
    "    float a = texture2D(uRtaoTex, aoUV).r;\n"
    "    gl_FragColor = vec4(a, a, a, 1.0); return;\n"
    "  }\n"
    // Real ray-traced occlusion multiplied into the scene. This darkens by
    // what is actually AROUND a point in the world, including geometry the
    // screen never shows - the ceiling screen-space AO cannot cross.
    "  if (uRtaoUse > 0.5) {\n"
    "    c *= max(texture2D(uRtaoTex, aoUV).r, 0.25);\n"
    "  }\n"
    "  if (uDebugGNormals > 0.5) {\n"
    "    vec4 gn = (uHasGBuf > 0.5) ? texture2D(uGBufTex, vUV) : vec4(0.0);\n"
    "    if (gn.a > 0.5) { gl_FragColor = vec4(gn.rgb, 1.0); }\n"
    "    else { gl_FragColor = vec4(1.0, 0.0, 1.0, 1.0); }\n"
    "    return;\n"
    "  }\n"
    "  if (uSSREnable > 0.5 && hasD && !sky) {\n"
    "    g_tanHalf = tan(radians(uFov)*0.5);\n"
    "    vec3 sP = viewPos(vUV);\n"
    "    vec3 sN = viewNormal(vUV, sP);\n"
    "    float upd = dot(sN, normalize(uViewUp));\n"
    // Two doors in: the geometric gate (upward faces = wet ground) or the
    // material mask (owner-graded metal/glass/water at ANY orientation).
    "    float mval = (uSSRMaskUse > 0.5) ? maskAt(vUV) : 0.0;\n"
    "    if (upd > uSSRUpDot || mval > 0.25) {\n"
    "      vec3 vd = normalize(sP);\n"
    "      vec3 rdir = reflect(vd, sN);\n"
    "      float fres = pow(1.0 - clamp(dot(-vd, sN), 0.0, 1.0), 2.0);\n"
    // Metals reflect head-on, not only at grazing angles - give masked
    // surfaces a reflectivity floor instead of the pure dielectric curve.
    "      if (mval > 0.25) fres = max(fres, uSSRMaskFresnel);\n"
    "      vec3 refl = vec3(0.0); float hitW = 0.0;\n"
    "      float tAcc = 0.0; float stepT = max(sP.z * 0.02, 0.05);\n"
    "      for (int si=1; si<=32; si++){\n"
    "        if (si > int(uSSRSteps)) break;\n"
    "        tAcc += stepT; stepT *= 1.15;\n"
    "        vec3 sp2 = sP + rdir * tAcc;\n"
    "        if (sp2.z < uNear) break;\n"
    "        vec2 suv = viewToUV(sp2);\n"
    "        if (suv.x<0.002||suv.x>0.998||suv.y<0.002||suv.y>0.998) break;\n"
    "        float zs = depthAt(suv);\n"
    "        float dz = sp2.z - zs;\n"
    "        if (dz > 0.02*max(sp2.z,1.0) && dz < uSSRThickness * max(1.0, sp2.z*0.05)) {\n"
    "          float edge = min(min(suv.x,1.0-suv.x), min(suv.y,1.0-suv.y));\n"
    "          hitW = clamp(edge*8.0, 0.0, 1.0) * (1.0 - float(si)/uSSRSteps);\n"
    "          refl = texture2D(uScene, suv).rgb;\n"
    "          break;\n"
    "        }\n"
    "      }\n"
    "      float rw = clamp(hitW * fres * uSSRIntensity, 0.0, 0.85);\n"
    "      if (uDebugSSR > 0.5) { gl_FragColor = vec4(refl * hitW, 1.0); return; }\n"
    "      c = mix(c, refl, rw);\n"
    "    }\n"
    "    else if (uDebugSSR > 0.5) { gl_FragColor = vec4(0.0,0.0,0.0,1.0); return; }\n"
    "  }\n"
    // --- far-field detail softening -----------------------------------------
    // Deliberately NOT a photographic depth of field: there is no focal plane,
    // and nothing at combat range is touched. This game is about shooting
    // things at distance, so blurring mid-field would hurt playability --
    // distant enemies are targets, not background. It only softens geometry
    // well beyond the range the player engages at, to hide 2005-era texture
    // detail and LOD seams that alias badly.
    //
    // Runs after AO/GI (so those are computed on sharp depth) and before bloom
    // (so bloom gathers the softened image naturally).
    "  if (uDofEnable > 0.5 && hasD && !sky) {\n"
    "    float zc = depthAt(vUV);\n"
    "    float f  = clamp((zc - uDofStart) / max(uDofEnd - uDofStart, 0.001), 0.0, 1.0);\n"
    "    f = f*f;\n"                                   // ease in: onset stays invisible
    "    if (f > 0.01) {\n"
    "      float rad = uDofStrength * f;\n"
    "      float ang = ign(gl_FragCoord.xy)*6.2831853;\n"
    "      vec3 acc = texture2D(uScene, vUV).rgb; float wsum = 1.0;\n"
    "      for (int i=0;i<8;i++){\n"
    "        float a = ang + float(i)*0.78539816;\n"
    "        vec2 off = vec2(cos(a)*aspect, sin(a)) * rad * uTexel * 6.0;\n"
    "        vec2 suv = clamp(vUV+off, vec2(0.001), vec2(0.999));\n"
    // Depth-weighted tap. Without this the sharp foreground smears outward
    // into the blurred distance -- the same failure family as the SSGI halo:
    // a screen-space gather that ignores depth pulls in geometry that is not
    // actually there. Reject anything markedly NEARER than this pixel.
    "        float w = step(zc*0.85, depthAt(suv));\n"
    "        acc += texture2D(uScene, suv).rgb * w; wsum += w;\n"
    "      }\n"
    "      c = mix(c, acc/wsum, f);\n"
    "    }\n"
    "  }\n"
    // --- soft bloom: wide bright-pass gather (single pass, dithered spokes) ---
    "  if (uBloomEnable > 0.5) {\n"
    "    vec3 bloom = vec3(0.0); float tot = 0.0;\n"
    "    float s = ign(gl_FragCoord.yx);\n"
    "    for (int i=0;i<32;i++){\n"
    "      float t = (float(i)+0.5)/32.0;\n"
    "      float a = (t + s)*6.2831853*3.7;\n"
    "      float rr = sqrt(t) * uBloomRadius * 0.10;\n"
    "      vec2 suv = vUV + vec2(cos(a)*aspect, sin(a)) * rr;\n"
    "      vec3 sc = texture2D(uScene, suv).rgb;\n"
    "      float lum = dot(sc, vec3(0.299,0.587,0.114));\n"
    "      vec3 bright = sc * max(lum - uBloomThreshold, 0.0) / max(lum, 0.001);\n"
    "      float w = 1.0 - t;\n"
    "      bloom += bright * w; tot += w;\n"
    "    }\n"
    "    if (tot > 0.0) bloom /= tot;\n"
    "    c += bloom * uBloomIntensity;\n"
    "  }\n"
    // --- tonemap + colour grade ---
    "  if (uGradeEnable > 0.5) {\n"
    "    c *= uExposure;\n"
    "    if (uTonemap > 0.5) c = aces(c);\n"
    "    c.r *= 1.0 + uTemperature*0.08;\n"           // white balance: +warm/-cool
    "    c.b *= 1.0 - uTemperature*0.08;\n"
    "    float l = dot(c, vec3(0.299,0.587,0.114));\n"
    "    c = mix(vec3(l), c, uSaturation);\n"
    "    c = (c-0.5)*uContrast + 0.5;\n"
    "    c *= uBrightness;\n"
    "  }\n"
    // --- vignette (subtle darkening toward the corners) ---
    "  if (uVignette > 0.001) {\n"
    "    float dcen = length(vUV - 0.5);\n"
    "    c *= 1.0 - smoothstep(0.35, 0.75, dcen) * uVignette;\n"
    "  }\n"
    "  c = mix(orig, c, uIntensity);\n"
    // Alpha 0, deliberately: this quad covers every pixel every frame, which
    // makes it the material mask's CLEAR. The game never clears backbuffer
    // color (sky covers it), untagged draws have alpha writes off, so without
    // this the mask saturates to 1 everywhere (measured: all-white x-ray).
    "  gl_FragColor = vec4(clamp(c,0.0,1.0), 0.0);\n"
    "}\n";

// ---- live-tunable parameters (read from settings.txt) --------------------

// ---- GI pre-pass shader (multi-pass skeleton) ------------------------------
// Computes GTAO occlusion (alpha) + HBIL bounce (rgb) into their own render
// target, so later stages (temporal blend, bilateral denoise) have a texture
// to read and refine. Helpers are duplicated from the main shader on purpose:
// the two programs must stay independently compilable.
static const char* kGIShader =
    "#version 120\n"
    "uniform sampler2D uScene;\n"
    "uniform sampler2D uDepth;\n"
    "uniform vec2  uTexel;\n"
    "uniform float uHasDepth;\n"
    "uniform float uNear;\n"
    "uniform float uFar;\n"
    "uniform float uFov;\n"
    "uniform float uAspect;\n"
    "uniform float uNearCutoff;\n"
    "uniform float uDepthInvert;\n"
    "uniform float uDepthFlipV;\n"
    "uniform float uGTAOIntensity;\n"
    "uniform float uGTAORadius;\n"
    "uniform float uGTAODirs;\n"
    "uniform float uGTAOSteps;\n"
    "uniform float uHBILEnable;\n"
    "uniform float uHBILIntensity;\n"
    // temporal accumulation: previous accumulated GI + the matrices to find
    // where this pixel WAS last frame. uTemporalOK=0 resets history (camera
    // cut, matrices unavailable).
    "uniform sampler2D uHist;\n"
    "uniform float uTemporalEnable;\n"
    "uniform float uTemporalBlend;\n"
    "uniform float uTemporalOK;\n"
    "uniform mat4  uInvVPCur;\n"
    "uniform mat4  uVPPrev;\n"
    "uniform float uFrameSeed;\n"
    "uniform float uLumaSplit;\n" // 0=flat AO, 1=occlusion scaled by (1-luma): baked light protected
    "varying vec2 vUV;\n"
    "float rawDepth(vec2 uv){\n"
    "  vec2 duv = vec2(uv.x, (uDepthFlipV > 0.5) ? (1.0 - uv.y) : uv.y);\n"
    "  float d = texture2D(uDepth, duv).r;\n"
    "  return (uDepthInvert > 0.5) ? (1.0 - d) : d;\n"
    "}\n"
    "float linDepth(float d){ return uNear*uFar/(uFar-d*(uFar-uNear)); }\n"
    "float depthAt(vec2 uv){ return linDepth(rawDepth(uv)); }\n"
    "float ign(vec2 p){ return fract(52.9829189*fract(dot(p, vec2(0.06711056,0.00583715)))); }\n"
    "float g_tanHalf;\n"
    "vec3 viewPos(vec2 uv){\n"
    "  float z = depthAt(uv);\n"
    "  vec2 ndc = uv*2.0-1.0;\n"
    "  return vec3(ndc.x*g_tanHalf*uAspect, ndc.y*g_tanHalf, 1.0) * z;\n"
    "}\n"
    "vec3 viewNormal(vec2 uv, vec3 P){\n"
    "  vec2 e = uTexel * 2.0;\n"
    "  vec3 pL=viewPos(uv-vec2(e.x,0)); vec3 pR=viewPos(uv+vec2(e.x,0));\n"
    "  vec3 pD=viewPos(uv-vec2(0,e.y)); vec3 pU=viewPos(uv+vec2(0,e.y));\n"
    "  vec3 dx = (abs(pR.z-P.z) < abs(P.z-pL.z)) ? (pR-P) : (P-pL);\n"
    "  vec3 dy = (abs(pU.z-P.z) < abs(P.z-pD.z)) ? (pU-P) : (P-pD);\n"
    "  vec3 n = cross(dx, dy);\n"
    "  float L = length(n);\n"
    "  if (L < 1e-6) return vec3(0.0,0.0,-1.0);\n"
    "  n /= L;\n"
    "  if (n.z > 0.0) n = -n;\n"
    "  return n;\n"
    "}\n"
    "void main(){\n"
    "  if (uHasDepth < 0.5) { gl_FragColor = vec4(0.0,0.0,0.0,1.0); return; }\n"
    "  float rd = rawDepth(vUV);\n"
    "  if (rd >= 0.9999) { gl_FragColor = vec4(0.0,0.0,0.0,1.0); return; }\n"
    "  float aspect = uTexel.x/uTexel.y;\n"
    "  g_tanHalf = tan(radians(uFov)*0.5);\n"
    "  vec3 P = viewPos(vUV);\n"
    "  vec3 N = viewNormal(vUV, P);\n"
    "  vec3 V = -normalize(P);\n"
    "  vec3 giAcc = vec3(0.0);\n"
    "  float jit = fract(ign(gl_FragCoord.xy) + uFrameSeed);\n"
    "  float radUV = clamp(uGTAORadius / max(P.z*g_tanHalf*2.0, 0.001), 0.004, 0.30);\n"
    "  int DIRS = int(clamp(uGTAODirs, 2.0, 4.0));\n"
    "  int STEPS = int(clamp(uGTAOSteps, 2.0, 8.0));\n"
    "  float occ = 0.0;\n"
    "  for (int s2=0; s2<4; s2++){\n"
    "    if (s2>=DIRS) break;\n"
    "    float phi = (float(s2)+jit)*6.2831853/float(DIRS);\n"
    "    vec2 dir = vec2(cos(phi)*aspect, sin(phi));\n"
    "    vec3 d3 = normalize(vec3(dir.x/aspect, dir.y, 0.0));\n"
    "    vec3 t3 = d3 - N*dot(d3, N);\n"
    "    float tl = length(t3);\n"
    "    float cosT = tl > 1e-4 ? dot(t3/tl, V) : 0.0;\n"
    "    float cosH = cosT;\n"
    "    for (int i2=1; i2<=8; i2++){\n"
    "      if (i2>STEPS) break;\n"
    "      float t = (float(i2)-0.5+jit)/float(STEPS);\n"
    "      vec2 suv = vUV + dir*(t*t*radUV);\n"
    "      if (suv.x<0.001||suv.x>0.999||suv.y<0.001||suv.y>0.999) break;\n"
    "      float zs = depthAt(suv);\n"
    "      if (uNearCutoff > 0.0 && zs < uNearCutoff) continue;\n"
    "      vec3 S = viewPos(suv);\n"
    "      vec3 D = S - P;\n"
    "      float dl = length(D);\n"
    "      if (dl < 1e-4 || dl > uGTAORadius*3.0) continue;\n"
    "      float cc = dot(D/dl, V);\n"
    "      float fall = 1.0 - (dl/(uGTAORadius*3.0));\n"
    "      float cNew = mix(cosT, cc, fall);\n"
    "      if (cNew > cosH) {\n"
    "        if (uHBILEnable > 0.5) {\n"
    "          float ndl = max(0.0, dot(N, D/dl));\n"
    "          giAcc += texture2D(uScene, suv).rgb * (cNew - cosH) * ndl;\n"
    "        }\n"
    "        cosH = cNew;\n"
    "      }\n"
    "    }\n"
    "    occ += max(0.0, cosH - cosT);\n"
    "  }\n"
    "  float gtao = 1.0 - (occ/float(DIRS)) * uGTAOIntensity;\n"
    // THE AMBIENT/DIRECT SPLIT, baked-lighting edition. This game's lighting
    // is painted into the frame (measured: no live sun vector exists in the
    // program constants; c[5] is a baked per-object colour scale). So the
    // frame itself is the light map: bright pixels ARE lit surfaces, and
    // occlusion physically belongs to the ambient term, not to direct light.
    // Scaling the occlusion by (1 - luma) protects sunlit surfaces and lets
    // shadowed creases go genuinely dark - the contrast baked-AO cannot give.
    "  if (uLumaSplit > 0.001) {\n"
    "    float luma = dot(texture2D(uScene, vUV).rgb, vec3(0.299, 0.587, 0.114));\n"
    "    float occAmt = (1.0 - gtao) * mix(1.0, clamp(1.0 - luma, 0.0, 1.0), uLumaSplit);\n"
    "    gtao = 1.0 - occAmt;\n"
    "  }\n"
    "  gtao = clamp(gtao, 0.35, 1.0);\n"
    "  vec3 bounce = giAcc / float(DIRS) * uHBILIntensity;\n"
    "  vec4 cur = vec4(bounce, gtao);\n"
    // ---- temporal accumulation ------------------------------------------
    // Where was this pixel last frame? Unproject current uv+depth to WORLD
    // with the inverse of this frame's view-projection, project with last
    // frame's - both matrices read from the game's own draws. History is
    // clamped to the current 4-neighbour range before blending (standard TAA
    // neighbourhood clamp): a reprojection that lands on different geometry
    // gets pulled to plausible values instead of ghosting.
    "  if (uTemporalEnable > 0.5 && uTemporalOK > 0.5) {\n"
    "    vec3 ndc = vec3(vUV.x*2.0-1.0, -(vUV.y*2.0-1.0), rd*2.0-1.0);\n"
    "    vec4 wp = uInvVPCur * vec4(ndc, 1.0);\n"
    "    if (abs(wp.w) > 1e-6) {\n"
    "      wp /= wp.w;\n"
    "      vec4 pc = uVPPrev * vec4(wp.xyz, 1.0);\n"
    "      if (pc.w > 1e-4) {\n"
    "        vec2 puv = vec2(pc.x/pc.w, -(pc.y/pc.w))*0.5+0.5;\n"
    "        if (puv.x>0.002 && puv.x<0.998 && puv.y>0.002 && puv.y<0.998) {\n"
    "          vec4 hist = texture2D(uHist, puv);\n"
    "          vec4 n1 = cur;\n"
    "          vec4 c0 = texture2D(uHist, puv);\n"          // placeholder read
    "          vec4 mn = cur; vec4 mx = cur;\n"
    "          vec4 s1; \n"
    "          s1 = vec4(0.0);\n"
    "          {\n"
    // 4-neighbour bounds of the CURRENT frame's signal, cheaply approximated
    // by re-evaluating depth-only AO proxies is too costly - instead sample
    // the current result's neighbours from the PREVIOUS accumulation target
    // is wrong too. Pragmatic clamp: widen current by a fixed tolerance.
    "            vec4 tol = vec4(0.15, 0.15, 0.15, 0.12);\n"
    "            mn = cur - tol; mx = cur + tol;\n"
    "          }\n"
    "          hist = clamp(hist, mn, mx);\n"
    "          cur = mix(cur, hist, clamp(uTemporalBlend, 0.0, 0.95));\n"
    "        }\n"
    "      }\n"
    "    }\n"
    "  }\n"
    "  gl_FragColor = cur;\n"
    "}\n";

// ---- GI denoise shader ------------------------------------------------------
// Depth-aware bilateral blur over the accumulated GI texture, run between
// temporal accumulation and the composite. Kills sampling grain without
// smearing across depth edges; bounce (rgb) and occlusion (a) both benefit.
static const char* kDenoiseShader =
    "#version 120\n"
    "uniform sampler2D uGI;\n"
    "uniform sampler2D uDepth;\n"
    "uniform vec2  uTexel;\n"
    "uniform float uNear;\n"
    "uniform float uFar;\n"
    "uniform float uDepthInvert;\n"
    "uniform float uDepthFlipV;\n"
    "uniform float uRadius;\n"
    "uniform float uDepthSigma;\n"
    "varying vec2 vUV;\n"
    "float rawDepth(vec2 uv){\n"
    "  vec2 duv = vec2(uv.x, (uDepthFlipV > 0.5) ? (1.0 - uv.y) : uv.y);\n"
    "  float d = texture2D(uDepth, duv).r;\n"
    "  return (uDepthInvert > 0.5) ? (1.0 - d) : d;\n"
    "}\n"
    "float linDepth(float d){ return uNear*uFar/(uFar-d*(uFar-uNear)); }\n"
    "float depthAt(vec2 uv){ return linDepth(rawDepth(uv)); }\n"
    "void main(){\n"
    "  float z0 = depthAt(vUV);\n"
    "  float sig = max(z0 * uDepthSigma, 0.02);\n"
    "  vec4 acc = texture2D(uGI, vUV);\n"
    "  float wsum = 1.0;\n"
    "  vec2 r = uTexel * uRadius;\n"
    "  {\n"
    "    vec2 o = vec2(1.0, 0.0) * r;\n"
    "    float z = depthAt(vUV + o);\n"
    "    float w = 1.0 * exp(-abs(z - z0) / sig);\n"
    "    acc += texture2D(uGI, vUV + o) * w; wsum += w;\n"
    "  }\n"
    "  {\n"
    "    vec2 o = vec2(-1.0, 0.0) * r;\n"
    "    float z = depthAt(vUV + o);\n"
    "    float w = 1.0 * exp(-abs(z - z0) / sig);\n"
    "    acc += texture2D(uGI, vUV + o) * w; wsum += w;\n"
    "  }\n"
    "  {\n"
    "    vec2 o = vec2(0.0, 1.0) * r;\n"
    "    float z = depthAt(vUV + o);\n"
    "    float w = 1.0 * exp(-abs(z - z0) / sig);\n"
    "    acc += texture2D(uGI, vUV + o) * w; wsum += w;\n"
    "  }\n"
    "  {\n"
    "    vec2 o = vec2(0.0, -1.0) * r;\n"
    "    float z = depthAt(vUV + o);\n"
    "    float w = 1.0 * exp(-abs(z - z0) / sig);\n"
    "    acc += texture2D(uGI, vUV + o) * w; wsum += w;\n"
    "  }\n"
    "  {\n"
    "    vec2 o = vec2(1.0, 1.0) * r;\n"
    "    float z = depthAt(vUV + o);\n"
    "    float w = 0.7 * exp(-abs(z - z0) / sig);\n"
    "    acc += texture2D(uGI, vUV + o) * w; wsum += w;\n"
    "  }\n"
    "  {\n"
    "    vec2 o = vec2(-1.0, 1.0) * r;\n"
    "    float z = depthAt(vUV + o);\n"
    "    float w = 0.7 * exp(-abs(z - z0) / sig);\n"
    "    acc += texture2D(uGI, vUV + o) * w; wsum += w;\n"
    "  }\n"
    "  {\n"
    "    vec2 o = vec2(1.0, -1.0) * r;\n"
    "    float z = depthAt(vUV + o);\n"
    "    float w = 0.7 * exp(-abs(z - z0) / sig);\n"
    "    acc += texture2D(uGI, vUV + o) * w; wsum += w;\n"
    "  }\n"
    "  {\n"
    "    vec2 o = vec2(-1.0, -1.0) * r;\n"
    "    float z = depthAt(vUV + o);\n"
    "    float w = 0.7 * exp(-abs(z - z0) / sig);\n"
    "    acc += texture2D(uGI, vUV + o) * w; wsum += w;\n"
    "  }\n"
    "  {\n"
    "    vec2 o = vec2(2.0, 0.0) * r;\n"
    "    float z = depthAt(vUV + o);\n"
    "    float w = 0.5 * exp(-abs(z - z0) / sig);\n"
    "    acc += texture2D(uGI, vUV + o) * w; wsum += w;\n"
    "  }\n"
    "  {\n"
    "    vec2 o = vec2(-2.0, 0.0) * r;\n"
    "    float z = depthAt(vUV + o);\n"
    "    float w = 0.5 * exp(-abs(z - z0) / sig);\n"
    "    acc += texture2D(uGI, vUV + o) * w; wsum += w;\n"
    "  }\n"
    "  {\n"
    "    vec2 o = vec2(0.0, 2.0) * r;\n"
    "    float z = depthAt(vUV + o);\n"
    "    float w = 0.5 * exp(-abs(z - z0) / sig);\n"
    "    acc += texture2D(uGI, vUV + o) * w; wsum += w;\n"
    "  }\n"
    "  {\n"
    "    vec2 o = vec2(0.0, -2.0) * r;\n"
    "    float z = depthAt(vUV + o);\n"
    "    float w = 0.5 * exp(-abs(z - z0) / sig);\n"
    "    acc += texture2D(uGI, vUV + o) * w; wsum += w;\n"
    "  }\n"
    "  gl_FragColor = acc / wsum;\n"
    "}\n";

struct GfxParams {
    float camDraw = 0.0f;   // 1 = take near/far/fov from the draws (SWSE_WindClipCamera)
    float debugNormals = 0.0f; // 1 = normals x-ray view
    // GTAO-family horizon AO. OFF by default per the rebuild discipline -
    // nothing ships on until the owner approves it in play.
    float gtaoEnable    = 0.0f;
    float gtaoIntensity = 1.0f;
    float gtaoRadius    = 1.2f;   // match the tuned ao_radius starting point
    float gtaoDirs      = 4.0f;
    float gtaoSteps     = 6.0f;
    float debugAO       = 0.0f;
    float hbilEnable    = 0.0f;   // bounce off, like every unapproved stage
    float hbilIntensity = 1.0f;
    float debugBounce   = 0.0f;
    float debugSSR      = 0.0f;
    float debugSSRMask  = 0.0f;
    float ssrMaskStamp   = 0.0f;  // scene draws stamp dest alpha (needs RGBA capture)
    float ssrMaskUse     = 0.0f;  // shader honors the mask as an SSR gate
    float ssrMaskFresnel = 0.35f; // reflectivity floor for masked (metal) pixels
    float ssrMaskFlip    = 0.0f;  // mask FBO's own V-flip (independent of depth)
    float ssrMaskDepth   = 1.0f;  // redraw depth test (0 = diagnostic: draw through)
    float gbufNormals    = 0.0f;  // true-normal G-buffer via the draw hook
    float debugGNormals  = 0.0f;  // x-ray: G-buffer normals (magenta = no coverage)
    float debugRt        = 0.0f;  // x-ray: the ray-traced camera view (stage 1b)
    float rtaoEnable     = 0.0f;  // stage 2: ray-traced ambient occlusion
    float rtaoRadius     = 4.0f;  // world units an occlusion ray travels
    float rtaoRays       = 8.0f;  // rays per pixel (denoised afterwards)
    float rtaoStrength   = 1.0f;
    float debugRtao      = 0.0f;  // x-ray: the AO term alone
    float rtaoBlend      = 0.12f; // temporal: weight of the new frame
    // Spatial denoise. Temporal alone still ghosts on fast camera motion,
    // because history is correctly REJECTED on those pixels and what is left
    // is the raw few-ray estimate.
    float rtaoDn         = 1.0f;  // 0 = raw accumulation
    float rtaoHistFlip   = 1.0f;  // temporal reprojection V flip; measure it
    float rtaoFlipV      = 0.0f;  // AO sample V flip in the composite
    float rtaoRequireHit = 1.0f;  // no BVH surface -> unoccluded, not guessed
    float rtaoDnRadius   = 4.0f;  // taps per side, per axis (max 8)
    float rtaoDnDepth    = 0.002f;// 2 units; 0.01 was the RGBA8 floor and blurred
                                  // across every silhouette on the ground
    float rtaoDnNormal   = 0.15f; // normal up-component tolerance
    float temporalEnable = 0.0f;  // OFF until owner-approved, like every stage
    float temporalBlend  = 0.85f;
    float lumaSplit      = 0.0f;  // 0 = off; ~0.7 protects lit surfaces
    float denoiseEnable  = 0.0f;
    float denoiseRadius  = 2.0f;   // tap spacing in texels
    float denoiseSigma   = 0.03f;  // relative depth tolerance
    float ssrEnable      = 0.0f;
    float ssrIntensity   = 0.6f;
    float ssrSteps       = 24.0f;
    float ssrThickness   = 1.5f;
    float ssrUpDot       = 0.65f;  // -1 = every surface (testing)
    float intensity     = 1.0f;
    // contact-shadow AO
    float aoEnable      = 1.0f;
    // Tuned in game against the fixed depth pipeline. Higher values (1.8/1.5)
    // carve creases hard enough that the frame reads as over-sharpened, and
    // push visible speckle onto rock faces.
    float aoIntensity   = 0.9f;
    float aoRadius      = 1.2f;
    float aoSamples     = 24.0f;  // raise to soften the dither grid
    // World units. Below this is the first-person weapon, which must not be
    // shaded by screen-space effects (its parts occlude each other).
    // World units. Geometry closer than this may be LIT but may not OCCLUDE, so
    // the held crossbow stops shadowing its own lower half and stops casting a
    // dark patch on the ground, while the critters mounted on it and any NPC
    // that walks up to you keep their lighting. 0 = off.
    float nearCutoff    = 2.0f;
    float depthNear     = 1.0f;
    float depthFar      = 120.0f;
    // soft bloom
    float bloomEnable   = 1.0f;
    float bloomThreshold= 0.62f;
    float bloomIntensity= 0.45f;
    float bloomRadius   = 1.0f;
    // RTGI / SSGI (optional, experimental - off in the default preset)
    float ssgiEnable    = 0.0f;
    // Above ~1.2 the bounce reads as a white rim around characters and blows
    // out emissive signs, because rays that pass behind a silhouette pick up
    // the bright ground/sky behind it.
    float ssgiIntensity = 0.8f;
    float ssgiRadius    = 1.0f;
    // How deep an occluder may be and still block a ray. Without this bound,
    // foreground objects halo outward across the screen.
    float ssgiThickness = 2.0f;
    // Cap on how far a ray may travel across the screen. Was hardcoded 0.45,
    // which let near objects gather from half the frame and bleed into each
    // other. 0.12 keeps the effect while killing that.
    float ssgiMaxScreen = 0.12f;
    // Far-field softening. Distances are real world units, which only became
    // meaningful once the camera's true near/far were read from the game.
    // Run the post-process at the scene FBO instead of at swap, so UI and the
    // first-person weapon (drawn afterwards) are never touched. Off by default
    // until the performance cost is measured -- the scene FBO is supersampled.
    float earlyPass     = 0.0f;
    float dofEnable     = 1.0f;
    float dofStart      = 90.0f;
    float dofEnd        = 400.0f;
    float dofStrength   = 2.5f;
    int   ssgiSamples   = 32;
    float debugGI       = 0.0f;
    float fov           = 65.0f;   // overridden by the camera's real FOV
    // Depth-buffer conventions. Both default OFF: measured on this engine, the
    // depth buffer is standard (not reverse-Z) and shares the frame's
    // orientation. The old shader did both and flattened the depth to nothing.
    float depthInvert   = 0.0f;   // standard depth, NOT reverse-Z (verified)
    float depthFlipV    = 1.0f;   // depth is V-flipped vs the captured frame (verified)
    // sharpen
    float sharpenEnable = 1.0f;
    float sharpenStrength = 0.22f;
    // tonemap + colour grade
    float gradeEnable   = 1.0f;
    float exposure      = 1.05f;
    float tonemap       = 1.0f;
    float saturation    = 1.12f;
    float contrast      = 1.06f;
    float brightness    = 1.0f;
    float temperature   = 0.15f;
    float vignette      = 0.30f;
};
static GfxParams g_params;
static GLuint g_depthTex = 0;
static GLint u_intensity=-1, u_depth=-1, u_hasDepth=-1;
static GLint u_aoEnable=-1, u_aoIntensity=-1, u_aoRadius=-1, u_near=-1, u_far=-1;
static GLint u_aoSamples=-1, u_nearCutoff=-1;
static GLint u_bloomEnable=-1, u_bloomThreshold=-1, u_bloomIntensity=-1, u_bloomRadius=-1;
static GLint u_ssgiEnable=-1, u_ssgiIntensity=-1, u_ssgiRadius=-1, u_ssgiSamples=-1;
static GLint u_ssgiThickness=-1, u_ssgiMaxScreen=-1;
static GLint u_dofEnable=-1, u_dofStart=-1, u_dofEnd=-1, u_dofStrength=-1;
static GLint u_debugGI=-1, u_fov=-1, u_aspect=-1, u_debugNormals=-1;
static GLint u_gtaoEnable=-1, u_gtaoIntensity=-1, u_gtaoRadius=-1;
static GLint u_gtaoDirs=-1, u_gtaoSteps=-1, u_debugAO=-1;
static GLint u_hbilEnable=-1, u_hbilIntensity=-1, u_debugBounce=-1;
static GLint u_giTex = -1;
static GLint u_ssrEnable=-1, u_ssrIntensity=-1, u_ssrSteps=-1;
static GLint u_ssrThickness=-1, u_ssrUpDot=-1, u_viewUp=-1, u_debugSSR=-1;
static GLint u_ssrMaskUse=-1, u_ssrMaskFresnel=-1, u_debugSSRMask=-1;
static GLint u_sceneFBO=-1, u_hasFBOMask=-1, u_maskFlipV=-1;
static bool  g_haveFBOMask = false;   // scene FBO color bound on unit 3 this frame
static GLint u_gbufTex=-1, u_hasGBuf=-1, u_debugGN=-1;
static GLint u_rtTex=-1, u_debugRt=-1;
static GLint u_rtaoTex=-1, u_rtaoUse=-1, u_debugRtao=-1, u_rtaoFlipV=-1;
static bool  g_haveRtao = false;
static bool  g_haveGBuf = false;      // normal G-buffer bound on unit 5 this frame
typedef void (APIENTRY* PFNGLUNIFORM3F)(GLint, GLfloat, GLfloat, GLfloat);
static PFNGLUNIFORM3F p_glUniform3f = nullptr;
// GI pre-pass objects + uniform locations
static GLuint g_giProg = 0, g_giTex = 0, g_giFbo = 0;
static int    g_giW = 0, g_giH = 0;
static bool   g_giOk = false;
static GLint gi_scene=-1, gi_depth=-1, gi_texel=-1, gi_hasDepth=-1;
static GLint gi_near=-1, gi_far=-1, gi_fov=-1, gi_aspect=-1, gi_nearCutoff=-1;
static GLint gi_depthInvert=-1, gi_depthFlipV=-1;
static GLint gi_gtaoI=-1, gi_gtaoR=-1, gi_gtaoD=-1, gi_gtaoS=-1;
static GLint gi_hbilE=-1, gi_hbilI=-1;
static GLint gi_hist=-1, gi_tempE=-1, gi_tempB=-1, gi_tempOK=-1;
static GLint gi_invVP=-1, gi_vpPrev=-1, gi_frameSeed=-1, gi_lumaSplit=-1;
static GLuint g_dnProg = 0, g_giTexC = 0;
static GLint dn_gi=-1, dn_depth=-1, dn_texel=-1, dn_near=-1, dn_far=-1;
static GLint dn_dinv=-1, dn_dflip=-1, dn_radius=-1, dn_sigma=-1;
static int   g_giFrame = 0;
static GLuint g_giTexB = 0;          // ping-pong partner of g_giTex
static int    g_giCur = 0;           // which of the pair was written this frame
static float  g_vpPrev[16];
static bool   g_vpPrevValid = false;
typedef void (APIENTRY* PFNGLUNIFORMMATRIX4FV)(GLint, GLsizei, GLboolean, const GLfloat*);
static PFNGLUNIFORMMATRIX4FV p_glUniformMatrix4fv = nullptr;

// General 4x4 inverse (row-major), for unprojecting with the draws' VP.
static bool Invert4x4(const float m[16], float out[16]) {
    float inv[16];
    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2]  =  m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3]  = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (det > -1e-12f && det < 1e-12f) return false;
    det = 1.0f / det;
    for (int i = 0; i < 16; i++) out[i] = inv[i] * det;
    return true;
}
static GLint u_depthInvert=-1, u_depthFlipV=-1;
static GLint u_sharpenEnable=-1, u_sharpenStrength=-1;
static GLint u_gradeEnable=-1, u_exposure=-1, u_tonemap=-1;
static GLint u_saturation=-1, u_contrast=-1, u_brightness=-1, u_temperature=-1, u_vignette=-1;

// Post-process settings. Any mod may provide `graphics.txt`; the last enabled
// one wins, so a mod can ship a whole look.
//
// DO NOT fall back to a bare "settings.txt" here. Both SWSE Graphics and SWSE
// Console ship a file by that name, and SWSE_FindModFile returns the LAST
// enabled provider - Console loads after Graphics, so the graphics pipeline
// silently read the console's settings and RTGI died. A shared namespace needs
// unambiguous names; that is why `graphics.txt` and `console.txt` exist.
static void SettingsPath(char* out) {
    char exe[MAX_PATH]; GetModuleFileNameA(GetModuleHandleA(NULL), exe, MAX_PATH);
    char* sl = strrchr(exe, '\\'); if (sl) *sl = 0;      // ...\bin
    sl = strrchr(exe, '\\'); if (sl) *sl = 0;            // ...\Stranger's Wrath
    if (SWSE_FindModFile("graphics.txt", out, MAX_PATH)) return;
    // Legacy layout only: the shipped mod's own file, by exact path.
    wsprintfA(out, "%s\\SWSEMods\\SWSE Graphics\\settings.txt", exe);
}

// One settings key onto a params block. False for a key nothing reads: that
// is how `set` tells a typo from a setting, where it used to write the typo
// into graphics.txt and report success (RT_QA_STATE_AUDIT 2.1a: `set rtao 1`
// and `set debut_rt 1` both sat in the live file doing nothing).
static bool ApplySetting(GfxParams& p, const char* key, float val) {
    if      (!lstrcmpiA(key, "intensity"))       p.intensity      = val;
    else if (!lstrcmpiA(key, "depth_invert"))    p.depthInvert    = val;
    else if (!lstrcmpiA(key, "depth_flipv"))     p.depthFlipV     = val;
    else if (!lstrcmpiA(key, "depth_near"))      p.depthNear      = val;
    else if (!lstrcmpiA(key, "depth_far"))       p.depthFar       = val;
    else if (!lstrcmpiA(key, "cam_draw"))        p.camDraw        = val;
    else if (!lstrcmpiA(key, "debug_normals"))   p.debugNormals   = val;
    else if (!lstrcmpiA(key, "gtao_enable"))     p.gtaoEnable     = val;
    else if (!lstrcmpiA(key, "gtao_intensity"))  p.gtaoIntensity  = val;
    else if (!lstrcmpiA(key, "gtao_radius"))     p.gtaoRadius     = val;
    else if (!lstrcmpiA(key, "gtao_dirs"))       p.gtaoDirs       = val;
    else if (!lstrcmpiA(key, "gtao_steps"))      p.gtaoSteps      = val;
    else if (!lstrcmpiA(key, "debug_ao"))        p.debugAO        = val;
    else if (!lstrcmpiA(key, "hbil_enable"))     p.hbilEnable     = val;
    else if (!lstrcmpiA(key, "hbil_intensity"))  p.hbilIntensity  = val;
    else if (!lstrcmpiA(key, "debug_bounce"))    p.debugBounce    = val;
    else if (!lstrcmpiA(key, "temporal_enable")) p.temporalEnable = val;
    else if (!lstrcmpiA(key, "temporal_blend"))  p.temporalBlend  = val;
    else if (!lstrcmpiA(key, "luma_split"))      p.lumaSplit      = val;
    else if (!lstrcmpiA(key, "denoise_enable"))  p.denoiseEnable  = val;
    else if (!lstrcmpiA(key, "denoise_radius"))  p.denoiseRadius  = val;
    else if (!lstrcmpiA(key, "denoise_sigma"))   p.denoiseSigma   = val;
    else if (!lstrcmpiA(key, "ssr_enable"))      p.ssrEnable      = val;
    else if (!lstrcmpiA(key, "ssr_intensity"))   p.ssrIntensity   = val;
    else if (!lstrcmpiA(key, "ssr_steps"))       p.ssrSteps       = val;
    else if (!lstrcmpiA(key, "ssr_thickness"))   p.ssrThickness   = val;
    else if (!lstrcmpiA(key, "ssr_updot"))       p.ssrUpDot       = val;
    else if (!lstrcmpiA(key, "debug_ssr"))       p.debugSSR       = val;
    else if (!lstrcmpiA(key, "debug_ssrmask"))   p.debugSSRMask   = val;
    else if (!lstrcmpiA(key, "ssr_mask_stamp"))  p.ssrMaskStamp   = val;
    else if (!lstrcmpiA(key, "ssr_mask_use"))    p.ssrMaskUse     = val;
    else if (!lstrcmpiA(key, "ssr_mask_fresnel")) p.ssrMaskFresnel = val;
    else if (!lstrcmpiA(key, "ssr_mask_flip"))   p.ssrMaskFlip    = val;
    else if (!lstrcmpiA(key, "ssr_mask_depth"))  p.ssrMaskDepth   = val;
    else if (!lstrcmpiA(key, "gbuf_normals"))    p.gbufNormals    = val;
    else if (!lstrcmpiA(key, "debug_gnormals"))  p.debugGNormals  = val;
    else if (!lstrcmpiA(key, "debug_rt"))        p.debugRt        = val;
    else if (!lstrcmpiA(key, "rtao_enable"))     p.rtaoEnable     = val;
    else if (!lstrcmpiA(key, "rtao_radius"))     p.rtaoRadius     = val;
    else if (!lstrcmpiA(key, "rtao_rays"))       p.rtaoRays       = val;
    else if (!lstrcmpiA(key, "rtao_strength"))   p.rtaoStrength   = val;
    else if (!lstrcmpiA(key, "debug_rtao"))      p.debugRtao      = val;
    else if (!lstrcmpiA(key, "rtao_blend"))      p.rtaoBlend      = val;
    else if (!lstrcmpiA(key, "rtao_dn"))         p.rtaoDn         = val;
    else if (!lstrcmpiA(key, "rtao_histflip"))   p.rtaoHistFlip   = val;
    else if (!lstrcmpiA(key, "rtao_flipv"))      p.rtaoFlipV      = val;
    else if (!lstrcmpiA(key, "rtao_requirehit")) p.rtaoRequireHit = val;
    else if (!lstrcmpiA(key, "rtao_dn_radius"))  p.rtaoDnRadius   = val;
    else if (!lstrcmpiA(key, "rtao_dn_depth"))   p.rtaoDnDepth    = val;
    else if (!lstrcmpiA(key, "rtao_dn_normal"))  p.rtaoDnNormal   = val;
    else if (!lstrcmpiA(key, "ao_enable"))       p.aoEnable       = val;
    else if (!lstrcmpiA(key, "ao_intensity"))    p.aoIntensity    = val;
    else if (!lstrcmpiA(key, "ao_radius"))       p.aoRadius       = val;
    else if (!lstrcmpiA(key, "ao_samples"))      p.aoSamples      = val;
    else if (!lstrcmpiA(key, "near_cutoff"))     p.nearCutoff     = val;
    else if (!lstrcmpiA(key, "bloom_enable"))    p.bloomEnable    = val;
    else if (!lstrcmpiA(key, "bloom_threshold")) p.bloomThreshold = val;
    else if (!lstrcmpiA(key, "bloom_intensity")) p.bloomIntensity = val;
    else if (!lstrcmpiA(key, "bloom_radius"))    p.bloomRadius    = val;
    else if (!lstrcmpiA(key, "ssgi_enable"))     p.ssgiEnable     = val;
    else if (!lstrcmpiA(key, "ssgi_intensity"))  p.ssgiIntensity  = val;
    else if (!lstrcmpiA(key, "ssgi_radius"))     p.ssgiRadius     = val;
    else if (!lstrcmpiA(key, "ssgi_thickness"))  p.ssgiThickness  = val;
    else if (!lstrcmpiA(key, "ssgi_maxscreen"))  p.ssgiMaxScreen  = val;
    // early_pass is DISABLED IN CODE, not merely defaulted off. It crashes the
    // driver and the value persists into settings.txt the moment anyone runs
    // `set early_pass 1`, so a plain default would come back and crash the game
    // on the next launch -- which is exactly what happened. Re-enable this line
    // only once the pass is attached to the correct render target (see
    // research/GRAPHICS_RTGI.md: the scene colour is a window-sized texture
    // bound during earlier passes, not at the fbo=0 transition).
    else if (!lstrcmpiA(key, "early_pass"))      p.earlyPass      = val;
    else if (!lstrcmpiA(key, "dof_enable"))      p.dofEnable      = val;
    else if (!lstrcmpiA(key, "dof_start"))       p.dofStart       = val;
    else if (!lstrcmpiA(key, "dof_end"))         p.dofEnd         = val;
    else if (!lstrcmpiA(key, "dof_strength"))    p.dofStrength    = val;
    else if (!lstrcmpiA(key, "ssgi_samples"))    p.ssgiSamples    = (int)val;
    else if (!lstrcmpiA(key, "debug_gi"))        p.debugGI        = val;
    else if (!lstrcmpiA(key, "fov"))             p.fov            = val;
    else if (!lstrcmpiA(key, "sharpen_enable"))  p.sharpenEnable  = val;
    else if (!lstrcmpiA(key, "sharpen_strength"))p.sharpenStrength= val;
    else if (!lstrcmpiA(key, "grade_enable"))    p.gradeEnable    = val;
    else if (!lstrcmpiA(key, "exposure"))        p.exposure       = val;
    else if (!lstrcmpiA(key, "tonemap"))         p.tonemap        = val;
    else if (!lstrcmpiA(key, "saturation"))      p.saturation     = val;
    else if (!lstrcmpiA(key, "contrast"))        p.contrast       = val;
    else if (!lstrcmpiA(key, "brightness"))      p.brightness     = val;
    else if (!lstrcmpiA(key, "temperature"))     p.temperature    = val;
    else if (!lstrcmpiA(key, "vignette"))        p.vignette       = val;
    else return false;
    return true;
}

// A key ApplySetting reads - what `set` checks before writing anything.
int SWSE_GfxKnownKey(const char* key) {
    GfxParams scratch;
    return ApplySetting(scratch, key, 0.0f) ? 1 : 0;
}

static void LoadSettings() {
    char path[MAX_PATH]; SettingsPath(path);
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { LogC("gfx: no settings.txt (using defaults)"); return; }
    // 64 K, not 4 K. The WRITER (`set`) appends to this file without bound, and
    // the RTAO block lives at the END - so a 4 K read silently truncated exactly
    // the settings being tuned, and the straddling line could sscanf to a wrong
    // but plausible value with nothing logged. Measured at 4050 of 4095 bytes
    // with the tuning session still running. Match the writer, and say so when
    // the buffer fills rather than quietly dropping the tail.
    static char buf[65536]; DWORD n = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &n, NULL); CloseHandle(h);
    buf[n] = 0;
    if (n >= sizeof(buf) - 1)
        LogC("gfx: WARNING settings file filled the read buffer - tail may be lost");
    char key[64]; float val;
    char* line = strtok(buf, "\r\n");
    while (line) {
        if (line[0] != '#' && sscanf(line, "%63s %f", key, &val) == 2)
            ApplySetting(g_params, key, val);
        line = strtok(NULL, "\r\n");
    }
    char msg[200];
    wsprintfA(msg, "gfx: settings loaded (ao=%d bloom=%d ssgi=%d dbgGI=%d sharpen=%d grade=%d, intensity x1000=%d)",
              (int)g_params.aoEnable, (int)g_params.bloomEnable, (int)g_params.ssgiEnable,
              (int)g_params.debugGI, (int)g_params.sharpenEnable,
              (int)g_params.gradeEnable, (int)(g_params.intensity*1000));
    LogC(msg);
}

template <class T> static T Resolve(const char* name, bool& ok) {
    HMODULE gl = GetModuleHandleA("opengl32.dll");
    typedef PROC(WINAPI* wglGPA_t)(LPCSTR);
    static wglGPA_t wglGPA = (wglGPA_t)GetProcAddress(gl, "wglGetProcAddress");
    T fn = (T)wglGPA(name);
    if (!fn) { Log(std::string("gfx: missing GL fn ") + name); ok = false; }
    return fn;
}

bool SWSE_GfxInit() {
    bool ok = true;
    // FBO functions: try EXT first (this is a GL2-era driver path), then core.
    bool fboOk = true;
    p_glGenFramebuffers = Resolve<PFNGLGENFRAMEBUFFERS>("glGenFramebuffersEXT", fboOk);
    if (!p_glGenFramebuffers) { fboOk = true; p_glGenFramebuffers = Resolve<PFNGLGENFRAMEBUFFERS>("glGenFramebuffers", fboOk); }
    fboOk = true;
    p_glBindFramebuffer = Resolve<PFNGLBINDFRAMEBUFFER>("glBindFramebufferEXT", fboOk);
    if (!p_glBindFramebuffer) { fboOk = true; p_glBindFramebuffer = Resolve<PFNGLBINDFRAMEBUFFER>("glBindFramebuffer", fboOk); }
    fboOk = true;
    p_glFramebufferTexture2D = Resolve<PFNGLFRAMEBUFFERTEXTURE2D>("glFramebufferTexture2DEXT", fboOk);
    if (!p_glFramebufferTexture2D) { fboOk = true; p_glFramebufferTexture2D = Resolve<PFNGLFRAMEBUFFERTEXTURE2D>("glFramebufferTexture2D", fboOk); }
    fboOk = true;
    p_glCheckFramebufferStatus = Resolve<PFNGLCHECKFRAMEBUFFERSTATUS>("glCheckFramebufferStatusEXT", fboOk);
    if (!p_glCheckFramebufferStatus) { fboOk = true; p_glCheckFramebufferStatus = Resolve<PFNGLCHECKFRAMEBUFFERSTATUS>("glCheckFramebufferStatus", fboOk); }

    p_glCreateShader       = Resolve<PFNGLCREATESHADER>("glCreateShader", ok);
    p_glShaderSource       = Resolve<PFNGLSHADERSOURCE>("glShaderSource", ok);
    p_glCompileShader      = Resolve<PFNGLCOMPILESHADER>("glCompileShader", ok);
    p_glGetShaderiv        = Resolve<PFNGLGETSHADERIV>("glGetShaderiv", ok);
    p_glGetShaderInfoLog   = Resolve<PFNGLGETSHADERINFOLOG>("glGetShaderInfoLog", ok);
    p_glCreateProgram      = Resolve<PFNGLCREATEPROGRAM>("glCreateProgram", ok);
    p_glAttachShader       = Resolve<PFNGLATTACHSHADER>("glAttachShader", ok);
    p_glLinkProgram        = Resolve<PFNGLLINKPROGRAM>("glLinkProgram", ok);
    p_glGetProgramiv       = Resolve<PFNGLGETPROGRAMIV>("glGetProgramiv", ok);
    p_glUseProgram         = Resolve<PFNGLUSEPROGRAM>("glUseProgram", ok);
    p_glGetUniformLocation = Resolve<PFNGLGETUNIFORMLOCATION>("glGetUniformLocation", ok);
    p_glUniform1f          = Resolve<PFNGLUNIFORM1F>("glUniform1f", ok);
    p_glUniform2f          = Resolve<PFNGLUNIFORM2F>("glUniform2f", ok);
    p_glUniform1i          = Resolve<PFNGLUNIFORM1I>("glUniform1i", ok);
    { bool mok = true; p_glUniformMatrix4fv = Resolve<PFNGLUNIFORMMATRIX4FV>("glUniformMatrix4fv", mok); }
    { bool uok = true; p_glUniform3f = Resolve<PFNGLUNIFORM3F>("glUniform3f", uok); }
    if (!ok) { Log("gfx: GL2.0 not fully available - post-process disabled"); return false; }

    // compile vertex shader
    GLuint vs = p_glCreateShader(GL_VERTEX_SHADER);
    p_glShaderSource(vs, 1, &kVertShader, nullptr);
    p_glCompileShader(vs);
    GLint okv = 0; p_glGetShaderiv(vs, GL_COMPILE_STATUS, &okv);
    if (!okv) {
        char log[1024]; p_glGetShaderInfoLog(vs, 1024, nullptr, log);
        Log(std::string("gfx: VERTEX shader compile FAILED: ") + log);
        return false;
    }
    // compile fragment shader
    GLuint fs = p_glCreateShader(GL_FRAGMENT_SHADER);
    p_glShaderSource(fs, 1, &kProofShader, nullptr);
    p_glCompileShader(fs);
    GLint okc = 0; p_glGetShaderiv(fs, GL_COMPILE_STATUS, &okc);
    if (!okc) {
        char log[1024]; p_glGetShaderInfoLog(fs, 1024, nullptr, log);
        Log(std::string("gfx: FRAGMENT shader compile FAILED: ") + log);
        return false;
    }
    g_prog = p_glCreateProgram();
    p_glAttachShader(g_prog, vs);
    p_glAttachShader(g_prog, fs);
    p_glLinkProgram(g_prog);
    GLint okl = 0; p_glGetProgramiv(g_prog, GL_LINK_STATUS, &okl);
    if (!okl) { Log("gfx: program link FAILED"); return false; }

    // GI pre-pass program. Failure is NON-FATAL by design: g_giProg stays 0,
    // the pre-pass never runs, and the GTAO/HBIL stages read an unwritten
    // texture - the auto-fallback contract (frame stays sane, log says why).
    {
        GLuint gfs = p_glCreateShader(GL_FRAGMENT_SHADER);
        p_glShaderSource(gfs, 1, &kGIShader, nullptr);
        p_glCompileShader(gfs);
        GLint okg = 0; p_glGetShaderiv(gfs, GL_COMPILE_STATUS, &okg);
        if (!okg) {
            char glog[1024]; p_glGetShaderInfoLog(gfs, 1024, nullptr, glog);
            Log(std::string("gfx: GI shader compile FAILED: ") + glog);
        } else {
            g_giProg = p_glCreateProgram();
            p_glAttachShader(g_giProg, vs);
            p_glAttachShader(g_giProg, gfs);
            p_glLinkProgram(g_giProg);
            GLint okgl = 0; p_glGetProgramiv(g_giProg, GL_LINK_STATUS, &okgl);
            if (!okgl) { Log("gfx: GI program link FAILED"); g_giProg = 0; }
        }
        if (g_giProg) {
            gi_scene      = p_glGetUniformLocation(g_giProg, "uScene");
            gi_depth      = p_glGetUniformLocation(g_giProg, "uDepth");
            gi_texel      = p_glGetUniformLocation(g_giProg, "uTexel");
            gi_hasDepth   = p_glGetUniformLocation(g_giProg, "uHasDepth");
            gi_near       = p_glGetUniformLocation(g_giProg, "uNear");
            gi_far        = p_glGetUniformLocation(g_giProg, "uFar");
            gi_fov        = p_glGetUniformLocation(g_giProg, "uFov");
            gi_aspect     = p_glGetUniformLocation(g_giProg, "uAspect");
            gi_nearCutoff = p_glGetUniformLocation(g_giProg, "uNearCutoff");
            gi_depthInvert= p_glGetUniformLocation(g_giProg, "uDepthInvert");
            gi_depthFlipV = p_glGetUniformLocation(g_giProg, "uDepthFlipV");
            gi_gtaoI      = p_glGetUniformLocation(g_giProg, "uGTAOIntensity");
            gi_gtaoR      = p_glGetUniformLocation(g_giProg, "uGTAORadius");
            gi_gtaoD      = p_glGetUniformLocation(g_giProg, "uGTAODirs");
            gi_gtaoS      = p_glGetUniformLocation(g_giProg, "uGTAOSteps");
            gi_hbilE      = p_glGetUniformLocation(g_giProg, "uHBILEnable");
            gi_hbilI      = p_glGetUniformLocation(g_giProg, "uHBILIntensity");
            gi_hist       = p_glGetUniformLocation(g_giProg, "uHist");
            gi_tempE      = p_glGetUniformLocation(g_giProg, "uTemporalEnable");
            gi_tempB      = p_glGetUniformLocation(g_giProg, "uTemporalBlend");
            gi_tempOK     = p_glGetUniformLocation(g_giProg, "uTemporalOK");
            gi_invVP      = p_glGetUniformLocation(g_giProg, "uInvVPCur");
            gi_vpPrev     = p_glGetUniformLocation(g_giProg, "uVPPrev");
            gi_frameSeed  = p_glGetUniformLocation(g_giProg, "uFrameSeed");
            gi_lumaSplit  = p_glGetUniformLocation(g_giProg, "uLumaSplit");
            Log("gfx: GI pre-pass program ready");
        }
        // denoise program - same non-fatal contract
        GLuint dfs = p_glCreateShader(GL_FRAGMENT_SHADER);
        p_glShaderSource(dfs, 1, &kDenoiseShader, nullptr);
        p_glCompileShader(dfs);
        GLint okd = 0; p_glGetShaderiv(dfs, GL_COMPILE_STATUS, &okd);
        if (!okd) {
            char dlog[1024]; p_glGetShaderInfoLog(dfs, 1024, nullptr, dlog);
            Log(std::string("gfx: denoise shader compile FAILED: ") + dlog);
        } else {
            g_dnProg = p_glCreateProgram();
            p_glAttachShader(g_dnProg, vs);
            p_glAttachShader(g_dnProg, dfs);
            p_glLinkProgram(g_dnProg);
            GLint okdl = 0; p_glGetProgramiv(g_dnProg, GL_LINK_STATUS, &okdl);
            if (!okdl) { Log("gfx: denoise program link FAILED"); g_dnProg = 0; }
        }
        if (g_dnProg) {
            dn_gi     = p_glGetUniformLocation(g_dnProg, "uGI");
            dn_depth  = p_glGetUniformLocation(g_dnProg, "uDepth");
            dn_texel  = p_glGetUniformLocation(g_dnProg, "uTexel");
            dn_near   = p_glGetUniformLocation(g_dnProg, "uNear");
            dn_far    = p_glGetUniformLocation(g_dnProg, "uFar");
            dn_dinv   = p_glGetUniformLocation(g_dnProg, "uDepthInvert");
            dn_dflip  = p_glGetUniformLocation(g_dnProg, "uDepthFlipV");
            dn_radius = p_glGetUniformLocation(g_dnProg, "uRadius");
            dn_sigma  = p_glGetUniformLocation(g_dnProg, "uDepthSigma");
            Log("gfx: denoise program ready");
        }
    }

    // Dest-alpha recon for the SSR material mask. The plan is to let tagged
    // draws stamp reflectivity into the back buffer's alpha channel - which
    // only works if the pixel format HAS alpha storage. Runs once, with the
    // window-system framebuffer bound (we are mid-capture). A0 here = pivot
    // to a mask FBO instead; nothing else in this build changes behaviour.
    {
        GLint rb = -1, gb = -1, bb = -1, ab = -1;
        glGetIntegerv(0x0D52 /*GL_RED_BITS*/,   &rb);
        glGetIntegerv(0x0D53 /*GL_GREEN_BITS*/, &gb);
        glGetIntegerv(0x0D54 /*GL_BLUE_BITS*/,  &bb);
        glGetIntegerv(0x0D55 /*GL_ALPHA_BITS*/, &ab);
        Log(std::string("gfx: backbuffer bits R") + std::to_string(rb)
            + " G" + std::to_string(gb) + " B" + std::to_string(bb)
            + " A" + std::to_string(ab) + " (dest-alpha probe)");
    }

    u_scene = p_glGetUniformLocation(g_prog, "uScene");
    u_texel = p_glGetUniformLocation(g_prog, "uTexel");
    u_intensity      = p_glGetUniformLocation(g_prog, "uIntensity");
    u_depth          = p_glGetUniformLocation(g_prog, "uDepth");
    u_hasDepth       = p_glGetUniformLocation(g_prog, "uHasDepth");
    u_near           = p_glGetUniformLocation(g_prog, "uNear");
    u_far            = p_glGetUniformLocation(g_prog, "uFar");
    u_aoEnable       = p_glGetUniformLocation(g_prog, "uAOEnable");
    u_aoIntensity    = p_glGetUniformLocation(g_prog, "uAOIntensity");
    u_aoRadius       = p_glGetUniformLocation(g_prog, "uAORadius");
    u_aoSamples      = p_glGetUniformLocation(g_prog, "uAOSamples");
    u_nearCutoff     = p_glGetUniformLocation(g_prog, "uNearCutoff");
    u_bloomEnable    = p_glGetUniformLocation(g_prog, "uBloomEnable");
    u_bloomThreshold = p_glGetUniformLocation(g_prog, "uBloomThreshold");
    u_bloomIntensity = p_glGetUniformLocation(g_prog, "uBloomIntensity");
    u_bloomRadius    = p_glGetUniformLocation(g_prog, "uBloomRadius");
    u_ssgiEnable     = p_glGetUniformLocation(g_prog, "uSSGIEnable");
    u_ssgiIntensity  = p_glGetUniformLocation(g_prog, "uSSGIIntensity");
    u_ssgiRadius     = p_glGetUniformLocation(g_prog, "uSSGIRadius");
    u_ssgiThickness  = p_glGetUniformLocation(g_prog, "uSSGIThickness");
    u_ssgiMaxScreen  = p_glGetUniformLocation(g_prog, "uSSGIMaxScreen");
    u_dofEnable      = p_glGetUniformLocation(g_prog, "uDofEnable");
    u_dofStart       = p_glGetUniformLocation(g_prog, "uDofStart");
    u_dofEnd         = p_glGetUniformLocation(g_prog, "uDofEnd");
    u_dofStrength    = p_glGetUniformLocation(g_prog, "uDofStrength");
    u_ssgiSamples    = p_glGetUniformLocation(g_prog, "uSSGISamples");
    u_debugGI        = p_glGetUniformLocation(g_prog, "uDebugGI");
    u_debugNormals   = p_glGetUniformLocation(g_prog, "uDebugNormals");
    u_gtaoEnable     = p_glGetUniformLocation(g_prog, "uGTAOEnable");
    u_gtaoIntensity  = p_glGetUniformLocation(g_prog, "uGTAOIntensity");
    u_gtaoRadius     = p_glGetUniformLocation(g_prog, "uGTAORadius");
    u_gtaoDirs       = p_glGetUniformLocation(g_prog, "uGTAODirs");
    u_gtaoSteps      = p_glGetUniformLocation(g_prog, "uGTAOSteps");
    u_debugAO        = p_glGetUniformLocation(g_prog, "uDebugAO");
    u_hbilEnable     = p_glGetUniformLocation(g_prog, "uHBILEnable");
    u_hbilIntensity  = p_glGetUniformLocation(g_prog, "uHBILIntensity");
    u_debugBounce    = p_glGetUniformLocation(g_prog, "uDebugBounce");
    u_giTex          = p_glGetUniformLocation(g_prog, "uGITex");
    u_ssrEnable      = p_glGetUniformLocation(g_prog, "uSSREnable");
    u_ssrIntensity   = p_glGetUniformLocation(g_prog, "uSSRIntensity");
    u_ssrSteps       = p_glGetUniformLocation(g_prog, "uSSRSteps");
    u_ssrThickness   = p_glGetUniformLocation(g_prog, "uSSRThickness");
    u_ssrUpDot       = p_glGetUniformLocation(g_prog, "uSSRUpDot");
    u_viewUp         = p_glGetUniformLocation(g_prog, "uViewUp");
    u_debugSSR       = p_glGetUniformLocation(g_prog, "uDebugSSR");
    u_ssrMaskUse     = p_glGetUniformLocation(g_prog, "uSSRMaskUse");
    u_ssrMaskFresnel = p_glGetUniformLocation(g_prog, "uSSRMaskFresnel");
    u_debugSSRMask   = p_glGetUniformLocation(g_prog, "uDebugSSRMask");
    u_sceneFBO       = p_glGetUniformLocation(g_prog, "uSceneFBO");
    u_hasFBOMask     = p_glGetUniformLocation(g_prog, "uHasFBOMask");
    u_maskFlipV      = p_glGetUniformLocation(g_prog, "uMaskFlipV");
    u_gbufTex        = p_glGetUniformLocation(g_prog, "uGBufTex");
    u_hasGBuf        = p_glGetUniformLocation(g_prog, "uHasGBuf");
    u_debugGN        = p_glGetUniformLocation(g_prog, "uDebugGNormals");
    u_rtTex          = p_glGetUniformLocation(g_prog, "uRtTex");
    u_debugRt        = p_glGetUniformLocation(g_prog, "uDebugRt");
    u_rtaoTex        = p_glGetUniformLocation(g_prog, "uRtaoTex");
    u_rtaoUse        = p_glGetUniformLocation(g_prog, "uRtaoUse");
    u_rtaoFlipV      = p_glGetUniformLocation(g_prog, "uRtaoFlipV");
    u_debugRtao      = p_glGetUniformLocation(g_prog, "uDebugRtao");
    u_depthInvert    = p_glGetUniformLocation(g_prog, "uDepthInvert");
    u_depthFlipV     = p_glGetUniformLocation(g_prog, "uDepthFlipV");
    u_fov            = p_glGetUniformLocation(g_prog, "uFov");
    u_aspect         = p_glGetUniformLocation(g_prog, "uAspect");
    u_sharpenEnable  = p_glGetUniformLocation(g_prog, "uSharpenEnable");
    u_sharpenStrength= p_glGetUniformLocation(g_prog, "uSharpenStrength");
    u_gradeEnable    = p_glGetUniformLocation(g_prog, "uGradeEnable");
    // A uniform the driver cannot see resolves to -1 and every write to it is
    // silently dropped -- which looks exactly like "the setting does nothing".
    // Log the ones that matter so this is never guesswork again.
    {
        char b[220];
        wsprintfA(b, "UNIFORMS: grade=%d exposure=%d intensity=%d vignette=%d "
                     "aoInt=%d ssgiInt=%d nearCut=%d debugGI=%d",
                  (int)u_gradeEnable, (int)p_glGetUniformLocation(g_prog, "uExposure"),
                  (int)u_intensity, (int)p_glGetUniformLocation(g_prog, "uVignette"),
                  (int)u_aoIntensity, (int)u_ssgiIntensity,
                  (int)u_nearCutoff, (int)u_debugGI);
        LogC(b);
    }
    u_exposure       = p_glGetUniformLocation(g_prog, "uExposure");
    u_tonemap        = p_glGetUniformLocation(g_prog, "uTonemap");
    u_saturation     = p_glGetUniformLocation(g_prog, "uSaturation");
    u_contrast       = p_glGetUniformLocation(g_prog, "uContrast");
    u_brightness     = p_glGetUniformLocation(g_prog, "uBrightness");
    u_temperature    = p_glGetUniformLocation(g_prog, "uTemperature");
    u_vignette       = p_glGetUniformLocation(g_prog, "uVignette");
    LoadSettings();   // read tunable params from settings.txt

    // optional but important: force texture unit 0 when we bind/sample
    {
        HMODULE gl = GetModuleHandleA("opengl32.dll");
        typedef PROC(WINAPI* wglGPA_t)(LPCSTR);
        wglGPA_t wglGPA = (wglGPA_t)GetProcAddress(gl, "wglGetProcAddress");
        p_glActiveTexture = (PFNGLACTIVETEXTURE)wglGPA("glActiveTexture");
        if (!p_glActiveTexture)
            p_glActiveTexture = (PFNGLACTIVETEXTURE)wglGPA("glActiveTextureARB");
    }

    glGenTextures(1, &g_tex);
    glGenTextures(1, &g_depthTex);
    g_ready = true;
    Log("gfx: M3 pipeline READY - post-process starts OFF, press F10 in-game to toggle");
    return true;
}

bool SWSE_GfxReady() { return g_ready; }

static bool g_firstEnabledFrame = true;

// All GL work in one function with NO C++ objects, so it can be wrapped in SEH.
// On the first enabled frame it logs each step; if a step faults, the log shows
// the last step reached -> we know exactly which GL call is unsupported.
static int g_enabledFrames = 0;

static void RenderRaw(int w, int h) {
    bool trace = g_firstEnabledFrame;
    if (trace) LogC("gfx step: begin");
    g_enabledFrames++;

    // ===================== CLEAN GPU PIPELINE (NPOT + NDC) ==================
    // Modern GPU (maxTex=32768) supports NPOT textures, so capture into an
    // EXACT w x h texture (no POT padding, no coordinate scaling). Draw a
    // fullscreen quad in NDC (-1..1) with gl_Position = gl_Vertex (no matrices).
    // These remove every source of the earlier shear.

    GLint savedProgram = 0, savedActiveTex = GL_TEXTURE0, savedTex2D = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &savedProgram);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &savedActiveTex);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex2D);

    glPushAttrib(GL_ALL_ATTRIB_BITS);
    // Materials mask handshake: our own quads must never lose alpha writes
    // (the GI texture carries occlusion in .a), so stamping suspends for the
    // rest of the frame - the swap-time frame mark re-arms it. PopAttrib
    // restores the game's own colormask on the way out.
    SWSE_MaterialsSetStamp(g_params.ssrMaskStamp > 0.5f ? 1 : 0);
    SWSE_MaterialsSetDepthTest(g_params.ssrMaskDepth > 0.5f ? 1 : 0);
    SWSE_MaterialsPassGuard(1);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glReadBuffer(GL_BACK);
    glDisable(GL_DEPTH_TEST); glDisable(GL_LIGHTING);
    glDisable(GL_BLEND); glDisable(GL_ALPHA_TEST);
    glDisable(GL_VERTEX_PROGRAM_ARB); glDisable(GL_FRAGMENT_PROGRAM_ARB);
    glEnable(GL_TEXTURE_2D);
    if (p_glActiveTexture) p_glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_tex);

    // Capture via glReadPixels -> glTexImage2D (NOT glCopyTexImage2D).
    // PROVEN: the tex-dump showed glCopyTexImage2D shears the frame (row-stride
    // mismatch), while glReadPixels returns a perfect frame. So read to CPU with
    // explicit PACK alignment, then upload with explicit UNPACK alignment. The
    // shader pass still runs on the GPU (fast, RTGI-ready) - only capture changed.
    // The material mask lives in destination alpha, so with stamping on the
    // capture widens to RGBA (+33% readback). RGB otherwise - no idle cost.
    bool maskA = g_params.ssrMaskStamp > 0.5f;
    GLenum capFmt = maskA ? GL_RGBA : GL_RGB;
    int nbytes = w * h * (maskA ? 4 : 3);
    if (nbytes != g_cpuSize) {
        free(g_cpu); g_cpu = (unsigned char*)malloc(nbytes); g_cpuSize = nbytes;
    }
    if (!g_cpu) { if (trace) LogC("gfx step: CPU alloc FAILED"); glPopAttrib(); return; }
    // ROOT CAUSE FIX: the game leaves GL_UNPACK_ROW_LENGTH (and friends) set for
    // its own texture uploads. glPushAttrib does NOT save pixel-store state, so
    // that leaked into our glTexImage2D -> wrong stride -> 3x tripling. Reset ALL
    // pack + unpack pixel-store params to defaults before our transfers.
    glPushClientAttrib(0x00000001 /*GL_CLIENT_PIXEL_STORE_BIT*/);
    glPixelStorei(0x0CF2 /*GL_UNPACK_ROW_LENGTH*/,  0);
    glPixelStorei(0x0CF3 /*GL_UNPACK_SKIP_ROWS*/,   0);
    glPixelStorei(0x0CF4 /*GL_UNPACK_SKIP_PIXELS*/, 0);
    glPixelStorei(0x0CF5 /*GL_UNPACK_ALIGNMENT*/,   1);
    glPixelStorei(0x0D02 /*GL_PACK_ROW_LENGTH*/,    0);
    glPixelStorei(0x0D03 /*GL_PACK_SKIP_ROWS*/,     0);
    glPixelStorei(0x0D04 /*GL_PACK_SKIP_PIXELS*/,   0);
    glPixelStorei(0x0D05 /*GL_PACK_ALIGNMENT*/,     1);
    glReadPixels(0, 0, w, h, capFmt, GL_UNSIGNED_BYTE, g_cpu);
    // Format changes force a realloc too: glTexSubImage2D RGBA into an RGB
    // texture "works" - by silently dropping the alpha the mask rides in.
    if (w != g_texW || h != g_texH || maskA != g_texHasA) {
        glTexImage2D(GL_TEXTURE_2D, 0, maskA ? 0x8058 /*GL_RGBA8*/ : GL_RGB,
                     w, h, 0, capFmt, GL_UNSIGNED_BYTE, g_cpu);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        g_texW = w; g_texH = h; g_texHasA = maskA;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, capFmt, GL_UNSIGNED_BYTE, g_cpu);
    }
    if (trace) LogC("gfx step: frame captured (glReadPixels->glTexImage2D)");

    // Bind the GAME'S scene depth TEXTURE directly to unit 1 (found live by the
    // FBO hook). This is the real per-pixel scene depth - no readback needed.
    // Choose a depth texture that actually has geometry in it. The old rule
    // ("whichever FBO was bound last") could land on a completely empty buffer,
    // which silently produced meaningless AO/GI. Done once, and again whenever
    // the effect is re-enabled, since the cost is a full texture readback.
    if (g_needDepthPick) { g_needDepthPick = !SWSE_AutoPickDepthTex(); }
    unsigned int gameDepth = SWSE_SceneDepthTex();
    bool haveDepth = false;
    if (gameDepth != 0) {
        if (p_glActiveTexture) p_glActiveTexture(GL_TEXTURE0 + 1);
        glBindTexture(GL_TEXTURE_2D, (GLuint)gameDepth);
        // make sure it returns the raw depth value (not a shadow-compare result)
        glTexParameteri(GL_TEXTURE_2D, 0x884C /*GL_TEXTURE_COMPARE_MODE*/, GL_NONE);
        // REMOVED: a one-shot depth dump used to run here at frame 120. It read
        // the whole depth texture back (28 MB of floats) and wrote a 7 MB file,
        // which is a serious stall on a GPU with no fast depth-readback path.
        // It was investigation scaffolding, not something the effect needs.
        // 'depthtex' does the same job on demand if it is ever needed again.
        if (p_glActiveTexture) p_glActiveTexture(GL_TEXTURE0);
        haveDepth = true;
        if (trace) LogC("gfx step: bound game depth texture to unit 1");
    } else if (trace) {
        LogC("gfx step: scene depth texture not detected yet");
    }

    // Material mask source on unit 3: the scene FBO color attachment. The
    // stamps land in ITS alpha; the engine's blit drops them before the
    // backbuffer, so the capture never sees them.
    g_haveFBOMask = false;
    {
        unsigned maskTex = SWSE_MaterialsMaskTex();
        if (maskTex != 0 && g_params.ssrMaskStamp > 0.5f && p_glActiveTexture) {
            // Unit 4: unit 3 belongs to the temporal GI history, which is
            // bound AFTER this spot - it silently replaced the mask and the
            // composite read accumulated GI as "reflectivity" (measured:
            // the x-ray showed an AO-looking ghost instead of silhouettes).
            p_glActiveTexture(GL_TEXTURE0 + 4);
            glBindTexture(GL_TEXTURE_2D, (GLuint)maskTex);
            p_glActiveTexture(GL_TEXTURE0);
            g_haveFBOMask = true;
        }
    }
    // Stage 1b: trace the world into our own texture, bind it on unit 6.
    // Only while the debug view is up - this is a correctness milestone, not
    // a shipping cost.
    if (g_params.debugRt > 0.5f && SWSE_Feature(FEAT_RAYTRACE)) {
        SWSE_RtTrace(w, h);
        unsigned rtT = SWSE_RtTex();
        if (rtT && p_glActiveTexture) {
            p_glActiveTexture(GL_TEXTURE0 + 6);
            glBindTexture(GL_TEXTURE_2D, (GLuint)rtT);
            p_glActiveTexture(GL_TEXTURE0);
        }
    }

    // Stage 2: ray-traced AO. Needs the scene depth, which is already found
    // and bound above; the pass unprojects it to world space itself.
    g_haveRtao = false;
    // The `raytrace` feature is the master switch: graphics.txt's rtao_enable
    // alone no longer turns the tracer on (1.1).
    if (g_params.rtaoEnable > 0.5f && haveDepth && SWSE_Feature(FEAT_RAYTRACE)) {
        SWSE_RtAoParams(g_params.rtaoRadius, g_params.rtaoRays, g_params.rtaoStrength);
        SWSE_RtAutoBuildTick();       // BVH follows the player
        SWSE_RtAoBlend(g_params.rtaoBlend);
        SWSE_RtAoDenoise(g_params.rtaoDn, g_params.rtaoDnRadius,
                         g_params.rtaoDnDepth, g_params.rtaoDnNormal);
        SWSE_RtAoHistFlip(g_params.rtaoHistFlip);
        SWSE_RtAoRequireHit(g_params.rtaoRequireHit);
        SWSE_RtAo(w, h, gameDepth, g_params.depthInvert, g_params.depthFlipV,
                  g_params.depthNear, g_params.depthFar);
        unsigned aoT = SWSE_RtAoTex();
        if (aoT && p_glActiveTexture) {
            p_glActiveTexture(GL_TEXTURE0 + 8);
            glBindTexture(GL_TEXTURE_2D, (GLuint)aoT);
            p_glActiveTexture(GL_TEXTURE0);
            g_haveRtao = true;
        }
    }

    // Normal G-buffer on unit 5 (3 = GI history, 4 = material mask).
    SWSE_MaterialsGBuf(g_params.gbufNormals > 0.5f ? 1 : 0, w, h);
    g_haveGBuf = false;
    {
        unsigned nrmTex = SWSE_MaterialsGBufTex();
        if (nrmTex != 0 && p_glActiveTexture) {
            p_glActiveTexture(GL_TEXTURE0 + 5);
            glBindTexture(GL_TEXTURE_2D, (GLuint)nrmTex);
            p_glActiveTexture(GL_TEXTURE0);
            g_haveGBuf = true;
        }
    }

    // ---- GI pre-pass (multi-pass skeleton) --------------------------------
    // GTAO+HBIL render into their own texture; the main shader fetches the
    // result. This is the structural prerequisite for temporal accumulation
    // and denoise - both operate on this texture before the composite reads it.
    bool wantGI = (g_params.gtaoEnable > 0.5f || g_params.hbilEnable > 0.5f);
    if (wantGI && g_giProg && p_glBindFramebuffer && p_glFramebufferTexture2D) {
        if (w != g_giW || h != g_giH) {
            if (!g_giTex)  glGenTextures(1, &g_giTex);
            if (!g_giTexB) glGenTextures(1, &g_giTexB);
            if (!g_giTexC) glGenTextures(1, &g_giTexC);
            if (p_glActiveTexture) p_glActiveTexture(GL_TEXTURE0 + 2);
            GLuint pair[3] = { g_giTex, g_giTexB, g_giTexC };
            for (int ti = 0; ti < 3; ti++) {
                glBindTexture(GL_TEXTURE_2D, pair[ti]);
                glTexImage2D(GL_TEXTURE_2D, 0, 0x8058 /*GL_RGBA8*/, w, h, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            }
            if (p_glActiveTexture) p_glActiveTexture(GL_TEXTURE0);
            if (!g_giFbo && p_glGenFramebuffers) p_glGenFramebuffers(1, &g_giFbo);
            p_glBindFramebuffer(0x8D40 /*GL_FRAMEBUFFER_EXT*/, g_giFbo);
            p_glFramebufferTexture2D(0x8D40, 0x8CE0 /*COLOR_ATTACHMENT0*/,
                                     GL_TEXTURE_2D, g_giTex, 0);
            unsigned st = p_glCheckFramebufferStatus
                        ? p_glCheckFramebufferStatus(0x8D40) : 0x8CD5u;
            g_giOk = (st == 0x8CD5 /*FRAMEBUFFER_COMPLETE*/);
            p_glBindFramebuffer(0x8D40, 0);
            g_giW = w; g_giH = h;
            g_vpPrevValid = false;   // resolution change invalidates history
            char gib[120];
            wsprintfA(gib, "gfx: GI pre-pass target %dx%d %s", w, h,
                      g_giOk ? "READY" : "INCOMPLETE - stages fall back");
            LogC(gib);
        }
        float giNear = g_params.depthNear, giFar = g_params.depthFar;
        float giFov  = g_params.fov;
        if (g_giOk) {
            SWSE_SceneProjection(&giNear, &giFar, &giFov);
            if (g_params.camDraw > 0.5f) {
                float dn, df, dv, da; unsigned dp; int dc, dg;
                if (SWSE_WindClipCamera(&dn, &df, &dv, &da, &dp, &dc, &dg)) {
                    giNear = dn; giFar = df; giFov = dv;
                }
            }
            // ping-pong: write the target we did NOT write last frame, read
            // the other as history on unit 3
            GLuint writeTex = g_giCur ? g_giTex : g_giTexB;
            GLuint histTex  = g_giCur ? g_giTexB : g_giTex;
            // Temporal matrices from the draws' camera. DERIVE it - do not
            // read the cache. SWSE_WindClipVPLast returns whatever the last
            // SWSE_WindClipCamera call published and derives nothing itself.
            //
            // With nothing refreshing it, vpPrev and vpCur come out IDENTICAL,
            // the reprojection collapses to the identity, and history is
            // fetched from the same SCREEN pixel every frame - so the shading
            // sticks to the screen and slides across the world as the camera
            // turns. That is the artefact the owner has been reporting, and it
            // is why it survived `rtao_enable 0`: the AO pass was the only
            // thing still calling the deriver, so switching it off froze this
            // camera completely.
            //
            // FOURTH occurrence of this bug class today (harvest path, the AO's
            // eye, the AO's matrix, now the GI temporal). In this codebase a
            // `...Last()` accessor is a CACHE; anything needing current data
            // must call the deriver itself.
            { float n_, f_, fv_, a_; unsigned p_; int c_, g_;
              SWSE_WindClipCamera(&n_, &f_, &fv_, &a_, &p_, &c_, &g_); }
            float vpCur[16], invCur[16];
            bool haveVP = SWSE_WindClipVPLast(vpCur) && Invert4x4(vpCur, invCur);
            bool tempOK = haveVP && g_vpPrevValid
                          && g_params.temporalEnable > 0.5f;
            p_glBindFramebuffer(0x8D40, g_giFbo);
            p_glFramebufferTexture2D(0x8D40, 0x8CE0, GL_TEXTURE_2D, writeTex, 0);
            if (p_glActiveTexture) {
                p_glActiveTexture(GL_TEXTURE0 + 3);
                glBindTexture(GL_TEXTURE_2D, histTex);
                p_glActiveTexture(GL_TEXTURE0);
            }
            p_glUseProgram(g_giProg);
            if (gi_hist       >= 0) p_glUniform1i(gi_hist, 3);
            if (gi_tempE      >= 0) p_glUniform1f(gi_tempE, g_params.temporalEnable);
            if (gi_tempB      >= 0) p_glUniform1f(gi_tempB, g_params.temporalBlend);
            if (gi_tempOK     >= 0) p_glUniform1f(gi_tempOK, tempOK ? 1.0f : 0.0f);
            if (haveVP && p_glUniformMatrix4fv) {
                if (gi_invVP  >= 0) p_glUniformMatrix4fv(gi_invVP, 1, GL_TRUE, invCur);
                if (gi_vpPrev >= 0) p_glUniformMatrix4fv(gi_vpPrev, 1, GL_TRUE,
                                        g_vpPrevValid ? g_vpPrev : vpCur);
            }
            if (gi_scene      >= 0) p_glUniform1i(gi_scene, 0);
            if (gi_depth      >= 0) p_glUniform1i(gi_depth, 1);
            if (gi_texel      >= 0) p_glUniform2f(gi_texel, 1.0f/(float)w, 1.0f/(float)h);
            if (gi_hasDepth   >= 0) p_glUniform1f(gi_hasDepth, haveDepth ? 1.0f : 0.0f);
            if (gi_near       >= 0) p_glUniform1f(gi_near, giNear);
            if (gi_far        >= 0) p_glUniform1f(gi_far,  giFar);
            if (gi_fov        >= 0) p_glUniform1f(gi_fov,  giFov);
            if (gi_aspect     >= 0) p_glUniform1f(gi_aspect, (float)w/(float)h);
            if (gi_nearCutoff >= 0) p_glUniform1f(gi_nearCutoff, g_params.nearCutoff);
            if (gi_depthInvert>= 0) p_glUniform1f(gi_depthInvert, g_params.depthInvert);
            if (gi_depthFlipV >= 0) p_glUniform1f(gi_depthFlipV,  g_params.depthFlipV);
            if (gi_gtaoI      >= 0) p_glUniform1f(gi_gtaoI, g_params.gtaoIntensity);
            if (gi_gtaoR      >= 0) p_glUniform1f(gi_gtaoR, g_params.gtaoRadius);
            if (gi_gtaoD      >= 0) p_glUniform1f(gi_gtaoD, g_params.gtaoDirs);
            if (gi_gtaoS      >= 0) p_glUniform1f(gi_gtaoS, g_params.gtaoSteps);
            if (gi_hbilE      >= 0) p_glUniform1f(gi_hbilE, g_params.hbilEnable);
            // 8-frame jitter cycle. Without it every frame drew the SAME sample
            // pattern, so accumulation had nothing to average (measured:
            // frame-to-frame diff got 3x WORSE with temporal on).
            g_giFrame = (g_giFrame + 1) & 7;
            if (gi_frameSeed  >= 0) p_glUniform1f(gi_frameSeed, (float)g_giFrame * 0.125f);
            if (gi_lumaSplit  >= 0) p_glUniform1f(gi_lumaSplit, g_params.lumaSplit);
            if (gi_hbilI      >= 0) p_glUniform1f(gi_hbilI, g_params.hbilIntensity);
            glColor3f(1, 1, 1);
            glBegin(GL_QUADS);
                glTexCoord2f(0, 0); glVertex2f(-1.0f, -1.0f);
                glTexCoord2f(1, 0); glVertex2f( 1.0f, -1.0f);
                glTexCoord2f(1, 1); glVertex2f( 1.0f,  1.0f);
                glTexCoord2f(0, 1); glVertex2f(-1.0f,  1.0f);
            glEnd();
            p_glBindFramebuffer(0x8D40, 0);
            // remember this frame's camera for next frame's reprojection
            if (haveVP) {
                for (int mi = 0; mi < 16; mi++) g_vpPrev[mi] = vpCur[mi];
                g_vpPrevValid = true;
            }
            g_giCur = !g_giCur;
            if (trace) LogC("gfx step: GI pre-pass drawn");
        }
        // ---- denoise pass: accumulated GI -> g_giTexC ----------------------
        GLuint accumTex = g_giCur ? g_giTexB : g_giTex;
        bool giDenoised = false;
        if (g_giOk && g_dnProg && g_params.denoiseEnable > 0.5f) {
            p_glBindFramebuffer(0x8D40, g_giFbo);
            p_glFramebufferTexture2D(0x8D40, 0x8CE0, GL_TEXTURE_2D, g_giTexC, 0);
            p_glUseProgram(g_dnProg);
            if (p_glActiveTexture) {
                p_glActiveTexture(GL_TEXTURE0 + 2);
                glBindTexture(GL_TEXTURE_2D, accumTex);
                p_glActiveTexture(GL_TEXTURE0);
            }
            if (dn_gi     >= 0) p_glUniform1i(dn_gi, 2);
            if (dn_depth  >= 0) p_glUniform1i(dn_depth, 1);
            if (dn_texel  >= 0) p_glUniform2f(dn_texel, 1.0f/(float)w, 1.0f/(float)h);
            if (dn_near   >= 0) p_glUniform1f(dn_near, giNear);
            if (dn_far    >= 0) p_glUniform1f(dn_far,  giFar);
            if (dn_dinv   >= 0) p_glUniform1f(dn_dinv, g_params.depthInvert);
            if (dn_dflip  >= 0) p_glUniform1f(dn_dflip, g_params.depthFlipV);
            if (dn_radius >= 0) p_glUniform1f(dn_radius, g_params.denoiseRadius);
            if (dn_sigma  >= 0) p_glUniform1f(dn_sigma,  g_params.denoiseSigma);
            glColor3f(1, 1, 1);
            glBegin(GL_QUADS);
                glTexCoord2f(0, 0); glVertex2f(-1.0f, -1.0f);
                glTexCoord2f(1, 0); glVertex2f( 1.0f, -1.0f);
                glTexCoord2f(1, 1); glVertex2f( 1.0f,  1.0f);
                glTexCoord2f(0, 1); glVertex2f(-1.0f,  1.0f);
            glEnd();
            p_glBindFramebuffer(0x8D40, 0);
            giDenoised = true;
        }
        // expose the freshly WRITTEN accumulation to the main pass on unit 2
        if (p_glActiveTexture) {
            p_glActiveTexture(GL_TEXTURE0 + 2);
            glBindTexture(GL_TEXTURE_2D, giDenoised ? g_giTexC : accumTex);
            p_glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, g_tex);
        }
    }

    // draw fullscreen quad in NDC - gl_Position = gl_Vertex, zero matrices
    // First-frame breakdown: a 130-SECOND stall was measured on the first
    // post-process frame, and it is not the capture or the depth pick. Time the
    // program bind and the draw separately, with a glFinish so driver work
    // cannot hide behind async submission. Deferred shader compilation happens
    // at first use, so if the cost lands on the draw, that is what it is.
    bool first = (g_enabledFrames <= 1);
    DWORD tu = GetTickCount();
    SetAllUniforms(w, h, haveDepth);
    DWORD tUniforms = GetTickCount() - tu;

    DWORD td = GetTickCount();
    glColor3f(1, 1, 1);
    glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(-1.0f, -1.0f);
        glTexCoord2f(1, 0); glVertex2f( 1.0f, -1.0f);
        glTexCoord2f(1, 1); glVertex2f( 1.0f,  1.0f);
        glTexCoord2f(0, 1); glVertex2f(-1.0f,  1.0f);
    glEnd();
    if (first) glFinish();          // force the driver to finish before timing
    DWORD tDraw = GetTickCount() - td;
    if (first) {
        char b[180];
        wsprintfA(b, "FIRSTFRAME: uniforms=%u ms  draw+finish=%u ms", tUniforms, tDraw);
        LogC(b);
    }
    if (trace) LogC("gfx step: quad drawn (NDC)");

    // restore state
    glPopClientAttrib();   // restore the game's pixel-store settings
    glPopAttrib();
    if (p_glActiveTexture) p_glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)savedTex2D);
    if (p_glActiveTexture) p_glActiveTexture((GLenum)savedActiveTex);
    if (p_glUseProgram) p_glUseProgram((GLuint)savedProgram);
    if (trace) LogC("gfx step: done OK (pixel-store reset)");
}

// All shader uniforms in one place, shared by both render paths (the swap-time
// pass and the earlier scene-FBO pass).
static void SetAllUniforms(int w, int h, bool haveDepth) {
    p_glUseProgram(g_prog);
    if (u_scene >= 0) p_glUniform1i(u_scene, 0);
    if (u_depth >= 0) p_glUniform1i(u_depth, 1);   // depth on texture unit 1
    if (u_giTex >= 0) p_glUniform1i(u_giTex, 2);   // GI pre-pass on unit 2
    if (u_ssrEnable    >= 0) p_glUniform1f(u_ssrEnable,    g_params.ssrEnable);
    if (u_ssrIntensity >= 0) p_glUniform1f(u_ssrIntensity, g_params.ssrIntensity);
    if (u_ssrSteps     >= 0) p_glUniform1f(u_ssrSteps,     g_params.ssrSteps);
    if (u_ssrThickness >= 0) p_glUniform1f(u_ssrThickness, g_params.ssrThickness);
    if (u_ssrUpDot     >= 0) p_glUniform1f(u_ssrUpDot,     g_params.ssrUpDot);
    if (u_debugSSR     >= 0) p_glUniform1f(u_debugSSR,     g_params.debugSSR);
    // Both mask consumers are ANDed with the stamp param: without stamping
    // the capture is RGB and samples alpha as constant 1.0, which would read
    // as "everything is metal" - accidental chrome world.
    {
        float stampOn = (g_params.ssrMaskStamp > 0.5f) ? 1.0f : 0.0f;
        if (u_ssrMaskUse     >= 0) p_glUniform1f(u_ssrMaskUse,     g_params.ssrMaskUse * stampOn);
        if (u_ssrMaskFresnel >= 0) p_glUniform1f(u_ssrMaskFresnel, g_params.ssrMaskFresnel);
        if (u_debugSSRMask   >= 0) p_glUniform1f(u_debugSSRMask,   g_params.debugSSRMask * stampOn);
        if (u_sceneFBO       >= 0) p_glUniform1i(u_sceneFBO, 4);
        if (u_hasFBOMask     >= 0) p_glUniform1f(u_hasFBOMask, g_haveFBOMask ? 1.0f : 0.0f);
        if (u_maskFlipV      >= 0) p_glUniform1f(u_maskFlipV, g_params.ssrMaskFlip);
        if (u_gbufTex        >= 0) p_glUniform1i(u_gbufTex, 5);
        if (u_hasGBuf        >= 0) p_glUniform1f(u_hasGBuf, g_haveGBuf ? 1.0f : 0.0f);
        if (u_debugGN        >= 0) p_glUniform1f(u_debugGN, g_params.debugGNormals);
        if (u_rtTex          >= 0) p_glUniform1i(u_rtTex, 6);
        if (u_debugRt        >= 0) p_glUniform1f(u_debugRt, g_params.debugRt);
        if (u_rtaoTex        >= 0) p_glUniform1i(u_rtaoTex, 8);
        if (u_rtaoFlipV      >= 0) p_glUniform1f(u_rtaoFlipV, g_params.rtaoFlipV);
        if (u_rtaoUse        >= 0) p_glUniform1f(u_rtaoUse, g_haveRtao ? 1.0f : 0.0f);
        if (u_debugRtao      >= 0) p_glUniform1f(u_debugRtao,
                                                 g_haveRtao ? g_params.debugRtao : 0.0f);
    }
    // world-up expressed in view space, from the draws' VP rows: for world
    // z-up, the view-space up vector is the z column of the view rotation.
    if (u_viewUp >= 0 && p_glUniform3f) {
        float vp16[16];
        float ux = 0.0f, uy = 1.0f, uz = 0.0f;   // fallback: screen up
        if (SWSE_WindClipVPLast(vp16)) {
            float l0 = sqrtf(vp16[0]*vp16[0]+vp16[1]*vp16[1]+vp16[2]*vp16[2]);
            float l1 = sqrtf(vp16[4]*vp16[4]+vp16[5]*vp16[5]+vp16[6]*vp16[6]);
            float l3 = sqrtf(vp16[12]*vp16[12]+vp16[13]*vp16[13]+vp16[14]*vp16[14]);
            if (l0 > 1e-4f && l1 > 1e-4f && l3 > 1e-4f) {
                ux = vp16[2]/l0; uy = vp16[6]/l1; uz = -vp16[14]/l3;
            }
        }
        p_glUniform3f(u_viewUp, ux, uy, uz);
    }
    if (u_texel >= 0) p_glUniform2f(u_texel, 1.0f/(float)w, 1.0f/(float)h);
    if (u_intensity      >= 0) p_glUniform1f(u_intensity,      g_params.intensity);
    if (u_hasDepth       >= 0) p_glUniform1f(u_hasDepth,       haveDepth ? 1.0f : 0.0f);
    // Prefer the camera's REAL near/far, read from the game's projection matrix.
    // depth_near/depth_far in settings.txt were hand-guessed, and a wrong
    // linearisation is fatal here: the scene depth buffer spans only ~0.96..0.99,
    // so the per-pixel deltas that normals get reconstructed from are tiny, and
    // the wrong constants turn them into noise.
    // ONE call, not two. This used to be queried again further down for the FOV,
    // and each query could trigger a memory scan -- so the cost was paid twice
    // per frame.
    float mNear = g_params.depthNear, mFar = g_params.depthFar;
    float mFov  = g_params.fov;
    SWSE_SceneProjection(&mNear, &mFar, &mFov);
    // Draw-derived camera (work item #0 of the RTGI rebuild). The heap scan
    // cannot tell which frustum copy is ACTIVE; the view-projection rows the
    // engine uploaded for this frame's world draws can. Opt-in via cam_draw 1
    // so the owner's defaults are untouched; falls back silently to the scan
    // value whenever no known program decomposes to a sane frustum.
    if (g_params.camDraw > 0.5f) {
        float dn, df, dv, da; unsigned dp; int dc, dg;
        if (SWSE_WindClipCamera(&dn, &df, &dv, &da, &dp, &dc, &dg)) {
            static int logged = 0;
            if (!logged) {
                char b[160];
                wsprintfA(b, "gfx: cam_draw ACTIVE - near/far/fov from program %u "
                             "(%s convention)", dp, dc ? "D3D" : "GL");
                LogC(b); logged = 1;
            }
            mNear = dn; mFar = df; mFov = dv;
        }
    }
    if (u_near           >= 0) p_glUniform1f(u_near,           mNear);
    if (u_far            >= 0) p_glUniform1f(u_far,            mFar);
    if (u_aoEnable       >= 0) p_glUniform1f(u_aoEnable,       g_params.aoEnable);
    if (u_aoIntensity    >= 0) p_glUniform1f(u_aoIntensity,    g_params.aoIntensity);
    if (u_aoRadius       >= 0) p_glUniform1f(u_aoRadius,       g_params.aoRadius);
    if (u_aoSamples      >= 0) p_glUniform1f(u_aoSamples,      g_params.aoSamples);
    if (u_nearCutoff     >= 0) p_glUniform1f(u_nearCutoff,     g_params.nearCutoff);
    if (u_bloomEnable    >= 0) p_glUniform1f(u_bloomEnable,    g_params.bloomEnable);
    if (u_bloomThreshold >= 0) p_glUniform1f(u_bloomThreshold, g_params.bloomThreshold);
    if (u_bloomIntensity >= 0) p_glUniform1f(u_bloomIntensity, g_params.bloomIntensity);
    if (u_bloomRadius    >= 0) p_glUniform1f(u_bloomRadius,    g_params.bloomRadius);
    if (u_ssgiEnable     >= 0) p_glUniform1f(u_ssgiEnable,     g_params.ssgiEnable);
    if (u_ssgiIntensity  >= 0) p_glUniform1f(u_ssgiIntensity,  g_params.ssgiIntensity);
    if (u_ssgiRadius     >= 0) p_glUniform1f(u_ssgiRadius,     g_params.ssgiRadius);
    if (u_ssgiThickness  >= 0) p_glUniform1f(u_ssgiThickness,  g_params.ssgiThickness);
    if (u_ssgiMaxScreen  >= 0) p_glUniform1f(u_ssgiMaxScreen,  g_params.ssgiMaxScreen);
    if (u_dofEnable      >= 0) p_glUniform1f(u_dofEnable,      g_params.dofEnable);
    if (u_dofStart       >= 0) p_glUniform1f(u_dofStart,       g_params.dofStart);
    if (u_dofEnd         >= 0) p_glUniform1f(u_dofEnd,         g_params.dofEnd);
    if (u_dofStrength    >= 0) p_glUniform1f(u_dofStrength,    g_params.dofStrength);
    if (u_ssgiSamples    >= 0) p_glUniform1i(u_ssgiSamples,    g_params.ssgiSamples);
    if (u_debugGI        >= 0) p_glUniform1f(u_debugGI,        g_params.debugGI);
    if (u_debugNormals   >= 0) p_glUniform1f(u_debugNormals,   g_params.debugNormals);
    if (u_gtaoEnable     >= 0) p_glUniform1f(u_gtaoEnable,     g_params.gtaoEnable);
    if (u_gtaoIntensity  >= 0) p_glUniform1f(u_gtaoIntensity,  g_params.gtaoIntensity);
    if (u_gtaoRadius     >= 0) p_glUniform1f(u_gtaoRadius,     g_params.gtaoRadius);
    if (u_gtaoDirs       >= 0) p_glUniform1f(u_gtaoDirs,       g_params.gtaoDirs);
    if (u_gtaoSteps      >= 0) p_glUniform1f(u_gtaoSteps,      g_params.gtaoSteps);
    if (u_debugAO        >= 0) p_glUniform1f(u_debugAO,        g_params.debugAO);
    if (u_hbilEnable     >= 0) p_glUniform1f(u_hbilEnable,     g_params.hbilEnable);
    if (u_hbilIntensity  >= 0) p_glUniform1f(u_hbilIntensity,  g_params.hbilIntensity);
    if (u_debugBounce    >= 0) p_glUniform1f(u_debugBounce,    g_params.debugBounce);
    if (u_depthInvert    >= 0) p_glUniform1f(u_depthInvert,    g_params.depthInvert);
    if (u_depthFlipV     >= 0) p_glUniform1f(u_depthFlipV,     g_params.depthFlipV);
    // FOV was guessed too (65 deg vs the camera's real 80). It scales the
    // view-space ray reconstruction, so a wrong value skews every normal the
    // same way a wrong near/far does. mFov came from the single query above.
    if (u_fov            >= 0) p_glUniform1f(u_fov,            mFov);
    if (u_aspect         >= 0) p_glUniform1f(u_aspect,         (float)w/(float)h);
    if (u_sharpenEnable  >= 0) p_glUniform1f(u_sharpenEnable,  g_params.sharpenEnable);
    if (u_sharpenStrength>= 0) p_glUniform1f(u_sharpenStrength,g_params.sharpenStrength);
    if (u_gradeEnable    >= 0) p_glUniform1f(u_gradeEnable,    g_params.gradeEnable);
    if (u_exposure       >= 0) p_glUniform1f(u_exposure,       g_params.exposure);
    if (u_tonemap        >= 0) p_glUniform1f(u_tonemap,        g_params.tonemap);
    if (u_saturation     >= 0) p_glUniform1f(u_saturation,     g_params.saturation);
    if (u_contrast       >= 0) p_glUniform1f(u_contrast,       g_params.contrast);
    if (u_brightness     >= 0) p_glUniform1f(u_brightness,     g_params.brightness);
    if (u_temperature    >= 0) p_glUniform1f(u_temperature,    g_params.temperature);
    if (u_vignette       >= 0) p_glUniform1f(u_vignette,       g_params.vignette);
}

// ---- EARLY PASS: process the scene while its FBO is still bound ------------
// Measured with 'fbotrace': every frame is 26 binds of the scene FBO followed
// by exactly ONE bind of fbo=0, after which the game composites and draws UI.
// Running at wglSwapBuffers means running after the UI exists, so UI and the
// first-person weapon get shaded with the world depth behind them (scenery
// ghosts through the inventory poster). Processing here, at the moment the game
// leaves the scene FBO, happens before any UI is drawn.
//
// Cost warning: the scene FBO is supersampled (3360x2100 or 6720x4200 against a
// 1680x1050 window), so this touches 4-16x the pixels of the swap-time pass.
// That is exactly why it is behind a switch and measured rather than assumed.
static GLuint g_fboTex = 0;          // ping-pong copy of the scene
static int    g_fboTexW = 0, g_fboTexH = 0;
static GLuint g_ourFbo = 0;          // our own FBO, scene texture attached

void SWSE_GfxProcessSceneFBO() {
    if (!g_ready || !g_enabled || !g_prog) return;
    if (g_params.earlyPass < 0.5f) return;
    if (!p_glGenFramebuffers || !p_glBindFramebuffer ||
        !p_glFramebufferTexture2D || !p_glCheckFramebufferStatus) return;

    // Target the game's finished SCENE COLOUR texture, not whatever framebuffer
    // happens to be bound. Measured: the pass right before fbo=0 is depth-only,
    // so the previous implementation copied colour from a framebuffer with none
    // and crashed the driver. glspy tracks the last screen-shaped colour
    // attachment instead -- texture 7 at 3840x2160 in the traced frame.
    GLuint sceneTex = (GLuint)SWSE_SceneColorTex();
    if (!sceneTex) return;

    // Use the TEXTURE'S OWN dimensions, not the viewport. The viewport at that
    // moment was 3840x2160, but if the attachment is a different size the copy
    // and the draw disagree on row stride and the frame comes out sheared into
    // diagonal stripes -- which is exactly what the first attempt produced.
    GLint prevBind = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevBind);
    glBindTexture(GL_TEXTURE_2D, sceneTex);
    GLint tw = 0, th = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevBind);
    if (tw != SWSE_SceneColorW() || th != SWSE_SceneColorH()) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            char b[160];
            wsprintfA(b, "EARLYPASS: viewport %dx%d but texture %u is %dx%d - using the texture",
                      SWSE_SceneColorW(), SWSE_SceneColorH(), sceneTex, tw, th);
            LogC(b);
        }
    }
    // MEASURED: texture 7 is 7680x4320 but the game renders into only a
    // 3840x2160 region of it. So process the REGION, not the whole texture:
    // copy that many pixels, set the viewport to it, and let the fullscreen
    // quad cover exactly it. Mixing the two sizes is what sheared the frame.
    int w = SWSE_SceneColorW(), h = SWSE_SceneColorH();
    if (w > tw) w = tw;
    if (h > th) h = th;
    if (w < 16 || h < 16) return;

    GLint savedProgram = 0, savedActiveTex = GL_TEXTURE0, savedTex2D = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &savedProgram);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &savedActiveTex);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex2D);

    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glDisable(GL_DEPTH_TEST); glDisable(GL_LIGHTING);
    glDisable(GL_BLEND); glDisable(GL_ALPHA_TEST);
    glDisable(GL_VERTEX_PROGRAM_ARB); glDisable(GL_FRAGMENT_PROGRAM_ARB);
    glEnable(GL_TEXTURE_2D);

    // Our own FBO, with the game's scene texture attached as the draw target.
    GLint savedFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING_E, &savedFbo);
    if (!g_ourFbo) p_glGenFramebuffers(1, &g_ourFbo);
    p_glBindFramebuffer(GL_FRAMEBUFFER_E, g_ourFbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER_E, GL_COLOR_ATTACHMENT0_E,
                             GL_TEXTURE_2D, sceneTex, 0);
    if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER_E) != GL_FRAMEBUFFER_COMPLETE_E) {
        p_glBindFramebuffer(GL_FRAMEBUFFER_E, (GLuint)savedFbo);
        glPopAttrib();
        return;                       // not usable this frame; do no harm
    }

    if (!g_fboTex) glGenTextures(1, &g_fboTex);
    if (p_glActiveTexture) p_glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_fboTex);
    if (w != g_fboTexW || h != g_fboTexH) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        g_fboTexW = w; g_fboTexH = h;
    }
    // Ping-pong: a texture cannot be sampled and rendered to at the same time.
    // Copy the scene out of the attachment first, then draw back into it while
    // sampling the copy. glCopyTexSubImage2D reads the bound framebuffer, which
    // is now our FBO with the scene texture attached -- and it ignores
    // pixel-store state, so the stride bugs that plagued glReadPixels cannot
    // occur here.
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
    glViewport(0, 0, w, h);

    // Deliberately NOT calling SWSE_AutoPickDepthTex() here. It reads back whole
    // depth textures (up to ~113MB) and doing that from inside the FBO bind hook,
    // mid-frame, was part of what made this path unstable. The swap-time path
    // already keeps the choice up to date.
    unsigned int gameDepth = SWSE_SceneDepthTex();
    bool haveDepth = false;
    if (gameDepth) {
        if (p_glActiveTexture) p_glActiveTexture(GL_TEXTURE0 + 1);
        glBindTexture(GL_TEXTURE_2D, (GLuint)gameDepth);
        glTexParameteri(GL_TEXTURE_2D, 0x884C /*GL_TEXTURE_COMPARE_MODE*/, GL_NONE);
        if (p_glActiveTexture) p_glActiveTexture(GL_TEXTURE0);
        haveDepth = true;
    }

    SetAllUniforms(w, h, haveDepth);
    glColor3f(1, 1, 1);
    glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(-1.0f, -1.0f);
        glTexCoord2f(1, 0); glVertex2f( 1.0f, -1.0f);
        glTexCoord2f(1, 1); glVertex2f( 1.0f,  1.0f);
        glTexCoord2f(0, 1); glVertex2f(-1.0f,  1.0f);
    glEnd();

    // Detach so we never hold a reference to a texture the game may delete,
    // then restore the framebuffer the game had bound.
    p_glFramebufferTexture2D(GL_FRAMEBUFFER_E, GL_COLOR_ATTACHMENT0_E,
                             GL_TEXTURE_2D, 0, 0);
    p_glBindFramebuffer(GL_FRAMEBUFFER_E, (GLuint)savedFbo);

    glPopAttrib();                   // restores the viewport we changed
    if (p_glActiveTexture) p_glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)savedTex2D);
    if (p_glActiveTexture) p_glActiveTexture((GLenum)savedActiveTex);
    if (p_glUseProgram) p_glUseProgram((GLuint)savedProgram);
}

// SEH wrapper: a fault in the early pass turns it off instead of crashing.
void SWSE_GfxProcessSceneFBOProtected() {
    __try { SWSE_GfxProcessSceneFBO(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_params.earlyPass = 0.0f;
        LogC("gfx: early scene-FBO pass FAULTED - disabled, falling back to swap-time");
    }
}

// Own function because SEH cannot live in a routine that needs C++ unwinding
// (SWSE_GfxFrame uses std::string).
static void PickDepthProtected() {
    __try { g_needDepthPick = !SWSE_AutoPickDepthTex(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_needDepthPick = false; }
}

// SEH wrapper - a fault in RenderRaw disables the effect instead of crashing.
// Also times every frame: a stall here is invisible from the console (which
// only measures how long a command took to return, not how long the frames
// afterwards took), so slow frames are logged with their cost.
static bool RenderProtected(int w, int h) {
    __try {
        DWORD t0 = GetTickCount();
        RenderRaw(w, h);
        DWORD ms = GetTickCount() - t0;
        if (ms > 100) {
            char b[140];
            wsprintfA(b, "SLOWFRAME: post-process frame %d took %u ms", g_enabledFrames, ms);
            LogC(b);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogC("gfx: FAULT during render - post-process auto-disabled (see last step above)");
        return false;
    }
}

// ---- in-engine screenshot --------------------------------------------------
// Capturing the window from outside needs it in the foreground, which is
// useless once AgentDebugMode lets the game run while the user works in another
// app - and it is exactly then that seeing the game matters most. Reading the
// framebuffer from inside the renderer works regardless of focus, window
// occlusion, or which monitor it is on.
//
// Writes an uncompressed 24-bit TGA: an 18-byte header and BGR rows, bottom-up,
// which is precisely the order glReadPixels returns. No encoder, no library.
static char          g_snapPath[MAX_PATH];
static volatile LONG g_snapPending = 0;

void SWSE_GfxRequestSnapshot(const char* path) {
    lstrcpynA(g_snapPath, path, MAX_PATH);
    InterlockedExchange(&g_snapPending, 1);
}

static void WriteTGA(const char* path, int w, int h, const unsigned char* rgb) {
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, 0);
    if (f == INVALID_HANDLE_VALUE) return;
    unsigned char hdr[18];
    memset(hdr, 0, sizeof(hdr));
    hdr[2]  = 2;                       // uncompressed true-colour
    hdr[12] = (unsigned char)(w & 0xFF);
    hdr[13] = (unsigned char)((w >> 8) & 0xFF);
    hdr[14] = (unsigned char)(h & 0xFF);
    hdr[15] = (unsigned char)((h >> 8) & 0xFF);
    hdr[16] = 24;
    DWORD wr = 0;
    WriteFile(f, hdr, sizeof(hdr), &wr, 0);
    // RGB -> BGR, a row at a time so the whole frame is not duplicated in memory.
    unsigned char* row = (unsigned char*)malloc((size_t)w * 3);
    if (row) {
        for (int y = 0; y < h; y++) {
            const unsigned char* src = rgb + (size_t)y * w * 3;
            for (int x = 0; x < w; x++) {
                row[x*3+0] = src[x*3+2];
                row[x*3+1] = src[x*3+1];
                row[x*3+2] = src[x*3+0];
            }
            WriteFile(f, row, (DWORD)w * 3, &wr, 0);
        }
        free(row);
    }
    CloseHandle(f);
}

static void LogSnap(const char* s) {
    char path[MAX_PATH];
    GetModuleFileNameA(GetModuleHandleA(NULL), path, MAX_PATH);
    char* sl = strrchr(path, '\\'); if (sl) *sl = 0;
    char full[MAX_PATH];
    wsprintfA(full, "%s\\swse_log.txt", path);
    HANDLE f = CreateFileA(full, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(f, s, lstrlenA(s), &w, NULL);
    WriteFile(f, "\r\n", 2, &w, NULL);
    CloseHandle(f);
}

static void ServiceSnapshot(HDC hdc) {
    if (!g_snapPending) return;
    HWND hwnd = WindowFromDC(hdc);
    RECT rc;
    if (!hwnd || !GetClientRect(hwnd, &rc)) { InterlockedExchange(&g_snapPending, 0); return; }
    // A minimized window owns no pixels: say so, rather than silently writing
    // nothing (background mode minimizes the game - restore it without
    // activation, behind other windows, to take a picture).
    if (IsIconic(hwnd)) {
        LogSnap("snap: the game window is minimized - no pixels to read. Restore it "
                "behind other windows (no activation) and snap again.");
        InterlockedExchange(&g_snapPending, 0);
        return;
    }
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) { InterlockedExchange(&g_snapPending, 0); return; }
    unsigned char* buf = (unsigned char*)malloc((size_t)w * h * 3);
    if (!buf) { InterlockedExchange(&g_snapPending, 0); return; }
    // The game leaves pixel-store state set for its own uploads; reset it or the
    // rows come back with the wrong stride (this cost a tripled image once).
    glPushClientAttrib(0x00000001 /*GL_CLIENT_PIXEL_STORE_BIT*/);
    glPixelStorei(0x0D02 /*GL_PACK_ROW_LENGTH*/,    0);
    glPixelStorei(0x0D03 /*GL_PACK_SKIP_ROWS*/,     0);
    glPixelStorei(0x0D04 /*GL_PACK_SKIP_PIXELS*/,   0);
    glPixelStorei(0x0D05 /*GL_PACK_ALIGNMENT*/,     1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, buf);
    glPopClientAttrib();
    WriteTGA(g_snapPath, w, h, buf);
    free(buf);
    InterlockedExchange(&g_snapPending, 0);
}

// SEH cannot live in SWSE_GfxFrame (it uses std::string, so it requires object
// unwinding) - the same constraint that forced PickDepthProtected.
static void ServiceSnapshotProtected(HDC hdc) {
    __try { ServiceSnapshot(hdc); }
    __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedExchange(&g_snapPending, 0); }
}

// For the frame hook when the pipeline is not running: `snap` is a console
// tool, and with graphics off (the 1.1 default) SWSE_GfxFrame never ran, so
// the request sat pending forever and no file appeared.
void SWSE_GfxServiceSnapshot(HDC hdc) { ServiceSnapshotProtected(hdc); }

void SWSE_GfxFrame(HDC hdc) {
    if (!g_ready) return;

    // Serviced before the enabled check: a screenshot must work whether or not
    // the post-process stack is switched on.
    ServiceSnapshotProtected(hdc);

    // F10 toggles the effect on/off (edge-detected). Default OFF => game normal.
    bool key = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (key && !g_prevKey) {
        g_enabled = !g_enabled;
        // Only re-choose the depth texture if we have not already got one.
        // Forcing a re-pick on every toggle meant every F10 press paid for a
        // full depth-texture readback -- a ~20 second freeze each time.
        if (g_enabled && SWSE_SceneDepthTex() == 0) g_needDepthPick = true;
        Log(g_enabled ? "post-process: ON" : "post-process: OFF");
    }
    g_prevKey = key;

    // F11 reloads settings.txt live - edit the file, press F11, see it change.
    static bool prevReload = false;
    bool reload = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    if (reload && !prevReload) { LoadSettings(); LogC("gfx: settings reloaded (F11)"); }
    prevReload = reload;

    // Numpad 9 toggles GTAO vs the old tap-count AO, for in-play A/B testing.
    // Runtime-only by design: it does NOT write settings.txt, so the shipped
    // default stays whatever the owner set, and an F11 reload or `set
    // gtao_enable` wins over the last keypress.
    static bool prevGtaoKey = false;
    bool gtaoKey = (GetAsyncKeyState(VK_NUMPAD9) & 0x8000) != 0;
    if (gtaoKey && !prevGtaoKey) {
        g_params.gtaoEnable = (g_params.gtaoEnable > 0.5f) ? 0.0f : 1.0f;
        LogC(g_params.gtaoEnable > 0.5f ? "gfx: GTAO ON (numpad 9)"
                                        : "gfx: GTAO OFF - old AO active (numpad 9)");
    }
    prevGtaoKey = gtaoKey;

    // Numpad 8 toggles the HBIL bounce, same contract as numpad 9: runtime
    // only, never writes settings, logged per flip.
    static bool prevHbilKey = false;
    bool hbilKey = (GetAsyncKeyState(VK_NUMPAD8) & 0x8000) != 0;
    if (hbilKey && !prevHbilKey) {
        g_params.hbilEnable = (g_params.hbilEnable > 0.5f) ? 0.0f : 1.0f;
        LogC(g_params.hbilEnable > 0.5f ? "gfx: HBIL bounce ON (numpad 8)"
                                        : "gfx: HBIL bounce OFF (numpad 8)");
    }
    prevHbilKey = hbilKey;

    // Numpad 6 toggles SSR - the owner's A/B switch for judging whether the
    // wet-ground look earns its keep. Same contract: runtime only, never
    // writes settings, logged per flip.
    static bool prevSsrKey = false;
    bool ssrKey = (GetAsyncKeyState(VK_NUMPAD6) & 0x8000) != 0;
    if (ssrKey && !prevSsrKey) {
        g_params.ssrEnable = (g_params.ssrEnable > 0.5f) ? 0.0f : 1.0f;
        LogC(g_params.ssrEnable > 0.5f ? "gfx: SSR ON (numpad 6)"
                                       : "gfx: SSR OFF (numpad 6)");
    }
    prevSsrKey = ssrKey;

    if (!g_enabled) return;   // do nothing - game renders as normal

    // When the early scene-FBO pass is active the frame has already been
    // processed before the UI was drawn, so running again here would apply the
    // whole stack twice (and would re-introduce the UI shading we moved to
    // avoid).
    if (g_params.earlyPass > 0.5f) {
        // The early pass must not do this itself (big readbacks inside the FBO
        // hook are unsafe), so keep the depth-texture choice fresh here, at
        // swap, where a stall is harmless.
        if (g_needDepthPick) PickDepthProtected();
        g_firstEnabledFrame = false;
        return;
    }

    // Use the TRUE window client size, not the game's current glViewport
    // (which can be a sub-region during 3D/menu rendering).
    HWND hwnd = WindowFromDC(hdc);
    RECT rc;
    if (!hwnd || !GetClientRect(hwnd, &rc)) return;
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    if (!RenderProtected(w, h)) {
        g_enabled = false;    // fault -> turn off so the game keeps running
        g_ready = false;
    }
    g_firstEnabledFrame = false;
}

// ---- console control surface ---------------------------------------------
void SWSE_GfxSetEnabled(int on) {
    g_enabled = on != 0;
    if (g_enabled && SWSE_SceneDepthTex() == 0) g_needDepthPick = true;
}
int  SWSE_GfxIsEnabled() { return g_enabled ? 1 : 0; }

// Truth probe for the mask chain: the live param values and the uniform
// locations, so 'ssrmask' can show exactly which link is dead.
void SWSE_GfxMaskDebugInfo(int* stampP, int* dbgP, int* locDbg, int* locFbo,
                           int* locHas, int* haveFbo) {
    if (stampP)  *stampP  = (int)(g_params.ssrMaskStamp * 100.0f);
    if (dbgP)    *dbgP    = (int)(g_params.debugSSRMask * 100.0f);
    if (locDbg)  *locDbg  = (int)u_debugSSRMask;
    if (locFbo)  *locFbo  = (int)u_sceneFBO;
    if (locHas)  *locHas  = (int)u_hasFBOMask;
    if (haveFbo) *haveFbo = g_haveFBOMask ? 1 : 0;
}

// Alpha histogram of the last RGBA capture - ground truth for the material
// mask, read from the CPU copy the capture already made. No GL calls, safe
// from any thread. Returns 0 when the capture is RGB (stamping off).
int SWSE_GfxMaskAlphaStats(int* mn, int* mx, int* mean, int* pctNonzero) {
    if (!g_texHasA || !g_cpu || g_texW <= 0 || g_texH <= 0) return 0;
    unsigned char lo = 255, hi = 0;
    unsigned long long sum = 0; long long nz = 0;
    long long n = (long long)g_texW * g_texH;
    for (long long i = 0; i < n; i++) {
        unsigned char a = g_cpu[i * 4 + 3];
        if (a < lo) lo = a;
        if (a > hi) hi = a;
        sum += a;
        if (a) nz++;
    }
    if (mn) *mn = lo;
    if (mx) *mx = hi;
    if (mean) *mean = (int)(sum / (unsigned long long)n);
    if (pctNonzero) *pctNonzero = (int)(nz * 100 / n);
    return 1;
}
void SWSE_GfxReloadSettings() { LoadSettings(); }

// Rewrite one "key value" line in settings.txt (replace or append), then
// reload live. Lets the console do `set bloom_intensity 0.6` etc.
// Two bugs lived here and between them they destroyed the user's settings.txt:
//
//  1. CR ACCUMULATION. The file uses CRLF, but the splitter only looks for
//     '\n', so every line kept a trailing '\r' and was then written back as
//     "%s\r\n" -> "...\r\r\n". Each `set` added one more CR to EVERY line, so
//     after ~25 calls the file had ~25 blank lines between each real one.
//  2. UNBOUNDED WRITE. `o += wsprintfA(out + o, ...)` never checked the buffer,
//     so once the CRs inflated the text past 9216 bytes it smashed the stack.
//     The 8192-byte read cap also silently truncated the tail of the file.
//
// Now: CR is stripped when splitting, every append is bounds-checked, and the
// buffers are large enough for a real config.
#define SET_BUF 65536
int SWSE_GfxSetSetting(const char* key, const char* value) {
    // Check before touching the file (RT_QA_STATE_AUDIT 2.1a): only a key
    // ApplySetting reads, and only a number - LoadSettings reads every value
    // as a float, so `set rtao_enable on` would have sat in the file doing
    // nothing. A decimal comma is taken as a point, as the prefs editor does.
    if (!SWSE_GfxKnownKey(key)) return 0;
    char num[64];
    if (lstrlenA(value) >= (int)sizeof(num)) return -2;
    lstrcpynA(num, value, sizeof(num));
    for (char* c = num; *c; c++) if (*c == ',') *c = '.';
    char* stop = nullptr;
    double d = strtod(num, &stop);
    if (stop == num || *stop || !(d >= -1.0e30 && d <= 1.0e30)) return -2;
    value = num;

    char path[MAX_PATH]; SettingsPath(path);

    static char buf[SET_BUF];
    DWORD n = 0;
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        ReadFile(h, buf, sizeof(buf) - 1, &n, NULL); CloseHandle(h);
    }
    buf[n] = 0;

    static char out[SET_BUF];
    int o = 0; bool replaced = false;

    // Append with a hard bound; refuse to write a truncated file rather than
    // silently losing the user's settings.
    // (_snprintf_s, not wsprintfA: a line over 511 characters would have run
    // past _t. Such a line refuses the write too.)
    #define SET_EMIT(fmt, a, b)                                          \
        do {                                                             \
            char _t[512];                                                \
            int _k = _snprintf_s(_t, sizeof(_t), _TRUNCATE, fmt, a, b);  \
            if (_k < 0 || o + _k >= (int)sizeof(out) - 1) return -1;     \
            memcpy(out + o, _t, _k); o += _k;                            \
        } while (0)

    char* p = buf;
    while (*p) {
        char* eol = p;
        while (*eol && *eol != '\n') eol++;
        char save = *eol;
        *eol = 0;
        // Strip the CR that CRLF leaves behind -- not doing this is what made
        // the blank lines multiply on every write.
        char* end = eol;
        while (end > p && (end[-1] == '\r')) { end[-1] = 0; end--; }

        char firstTok[64] = {0};
        sscanf(p, "%63s", firstTok);
        if (!replaced && firstTok[0] && lstrcmpiA(firstTok, key) == 0) {
            SET_EMIT("%s %s\r\n", key, value);
            replaced = true;
        } else if (p[0]) {
            SET_EMIT("%s%s\r\n", p, "");
        } else {
            SET_EMIT("%s%s\r\n", "", "");   // keep intentional blank lines
        }
        *eol = save;
        p = (save ? eol + 1 : eol);
    }
    if (!replaced) SET_EMIT("%s %s\r\n", key, value);
    #undef SET_EMIT

    HANDLE hw = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, NULL);
    if (hw == INVALID_HANDLE_VALUE) return -1;
    DWORD wr; WriteFile(hw, out, o, &wr, NULL); CloseHandle(hw);
    LoadSettings();
    return 1;
}
