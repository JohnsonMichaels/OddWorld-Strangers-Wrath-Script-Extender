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
#include "glspy.h"
#include <gl/GL.h>
#include <string>
#include <fstream>
#include <cstdio>
#include <cstring>

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
    "  if (uAOEnable > 0.5 && hasD && !sky) {\n"
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
    "  if (uSSGIEnable > 0.5 && hasD && !sky) {\n"
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
    "  gl_FragColor = vec4(clamp(c,0.0,1.0), 1.0);\n"
    "}\n";

// ---- live-tunable parameters (read from settings.txt) --------------------
struct GfxParams {
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
static GLint u_debugGI=-1, u_fov=-1, u_aspect=-1;
static GLint u_depthInvert=-1, u_depthFlipV=-1;
static GLint u_sharpenEnable=-1, u_sharpenStrength=-1;
static GLint u_gradeEnable=-1, u_exposure=-1, u_tonemap=-1;
static GLint u_saturation=-1, u_contrast=-1, u_brightness=-1, u_temperature=-1, u_vignette=-1;

// Settings file: <game>\SWSEMods\SWSE Graphics\settings.txt
// Format: one "key value" per line (e.g. "saturation 1.8"). '#' = comment.
static void SettingsPath(char* out) {
    char exe[MAX_PATH]; GetModuleFileNameA(GetModuleHandleA(NULL), exe, MAX_PATH);
    char* sl = strrchr(exe, '\\'); if (sl) *sl = 0;      // ...\bin
    sl = strrchr(exe, '\\'); if (sl) *sl = 0;            // ...\Stranger's Wrath
    wsprintfA(out, "%s\\SWSEMods\\SWSE Graphics\\settings.txt", exe);
}

static void LoadSettings() {
    char path[MAX_PATH]; SettingsPath(path);
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { LogC("gfx: no settings.txt (using defaults)"); return; }
    char buf[4096]; DWORD n = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &n, NULL); CloseHandle(h);
    buf[n] = 0;
    char key[64]; float val;
    char* line = strtok(buf, "\r\n");
    while (line) {
        if (line[0] != '#' && sscanf(line, "%63s %f", key, &val) == 2) {
            if      (!lstrcmpiA(key, "intensity"))       g_params.intensity      = val;
            else if (!lstrcmpiA(key, "depth_invert"))    g_params.depthInvert    = val;
    else if (!lstrcmpiA(key, "depth_flipv"))     g_params.depthFlipV     = val;
    else if (!lstrcmpiA(key, "depth_near"))      g_params.depthNear      = val;
            else if (!lstrcmpiA(key, "depth_far"))       g_params.depthFar       = val;
            else if (!lstrcmpiA(key, "ao_enable"))       g_params.aoEnable       = val;
            else if (!lstrcmpiA(key, "ao_intensity"))    g_params.aoIntensity    = val;
            else if (!lstrcmpiA(key, "ao_radius"))       g_params.aoRadius       = val;
    else if (!lstrcmpiA(key, "ao_samples"))      g_params.aoSamples      = val;
    else if (!lstrcmpiA(key, "near_cutoff"))     g_params.nearCutoff     = val;
            else if (!lstrcmpiA(key, "bloom_enable"))    g_params.bloomEnable    = val;
            else if (!lstrcmpiA(key, "bloom_threshold")) g_params.bloomThreshold = val;
            else if (!lstrcmpiA(key, "bloom_intensity")) g_params.bloomIntensity = val;
            else if (!lstrcmpiA(key, "bloom_radius"))    g_params.bloomRadius    = val;
            else if (!lstrcmpiA(key, "ssgi_enable"))     g_params.ssgiEnable     = val;
            else if (!lstrcmpiA(key, "ssgi_intensity"))  g_params.ssgiIntensity  = val;
            else if (!lstrcmpiA(key, "ssgi_radius"))     g_params.ssgiRadius     = val;
    else if (!lstrcmpiA(key, "ssgi_thickness"))  g_params.ssgiThickness  = val;
    else if (!lstrcmpiA(key, "ssgi_maxscreen"))  g_params.ssgiMaxScreen  = val;
    // early_pass is DISABLED IN CODE, not merely defaulted off. It crashes the
    // driver and the value persists into settings.txt the moment anyone runs
    // `set early_pass 1`, so a plain default would come back and crash the game
    // on the next launch -- which is exactly what happened. Re-enable this line
    // only once the pass is attached to the correct render target (see
    // research/GRAPHICS_RTGI.md: the scene colour is a window-sized texture
    // bound during earlier passes, not at the fbo=0 transition).
    else if (!lstrcmpiA(key, "early_pass"))      g_params.earlyPass      = val;
    else if (!lstrcmpiA(key, "dof_enable"))      g_params.dofEnable      = val;
    else if (!lstrcmpiA(key, "dof_start"))       g_params.dofStart       = val;
    else if (!lstrcmpiA(key, "dof_end"))         g_params.dofEnd         = val;
    else if (!lstrcmpiA(key, "dof_strength"))    g_params.dofStrength    = val;
            else if (!lstrcmpiA(key, "ssgi_samples"))    g_params.ssgiSamples    = (int)val;
            else if (!lstrcmpiA(key, "debug_gi"))        g_params.debugGI        = val;
            else if (!lstrcmpiA(key, "fov"))             g_params.fov            = val;
            else if (!lstrcmpiA(key, "sharpen_enable"))  g_params.sharpenEnable  = val;
            else if (!lstrcmpiA(key, "sharpen_strength"))g_params.sharpenStrength= val;
            else if (!lstrcmpiA(key, "grade_enable"))    g_params.gradeEnable    = val;
            else if (!lstrcmpiA(key, "exposure"))        g_params.exposure       = val;
            else if (!lstrcmpiA(key, "tonemap"))         g_params.tonemap        = val;
            else if (!lstrcmpiA(key, "saturation"))      g_params.saturation     = val;
            else if (!lstrcmpiA(key, "contrast"))        g_params.contrast       = val;
            else if (!lstrcmpiA(key, "brightness"))      g_params.brightness     = val;
            else if (!lstrcmpiA(key, "temperature"))     g_params.temperature    = val;
            else if (!lstrcmpiA(key, "vignette"))        g_params.vignette       = val;
        }
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
    int nbytes = w * h * 3;
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
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, g_cpu);
    if (w != g_texW || h != g_texH) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, g_cpu);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        g_texW = w; g_texH = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, g_cpu);
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

static void ServiceSnapshot(HDC hdc) {
    if (!g_snapPending) return;
    HWND hwnd = WindowFromDC(hdc);
    RECT rc;
    if (!hwnd || !GetClientRect(hwnd, &rc)) { InterlockedExchange(&g_snapPending, 0); return; }
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
    #define SET_EMIT(fmt, a, b)                                          \
        do {                                                             \
            char _t[512];                                                \
            int _k = wsprintfA(_t, fmt, a, b);                           \
            if (o + _k >= (int)sizeof(out) - 1) return 0;                \
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
    if (hw == INVALID_HANDLE_VALUE) return 0;
    DWORD wr; WriteFile(hw, out, o, &wr, NULL); CloseHandle(hw);
    LoadSettings();
    return 1;
}
