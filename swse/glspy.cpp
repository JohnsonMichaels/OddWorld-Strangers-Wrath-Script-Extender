// SWSE GL reconnaissance. Inline-hooks wglGetProcAddress so we can log every GL
// extension function the game resolves. This reveals whether the game uses FBOs
// (glBindFramebufferEXT/ARB), the depth-related calls, etc. - the map we need to
// find the scene depth buffer for true SSAO/RTGI.

#include "features.h"
#include "modregistry.h"
#include "glspy.h"
#include "gfx.h"
#include "foliage.h"
#include <gl/GL.h>
#include <string>
#include <fstream>
#include <cstring>
#include <cmath>

typedef PROC(WINAPI* wglGPA_t)(LPCSTR);
static wglGPA_t g_realGPA = nullptr;
static BYTE g_orig[5];

// FBO-inspection helpers (resolved in SWSE_InstallGLSpy)
typedef void (APIENTRY* glBindFBO_t)(GLenum, GLuint);
typedef void (APIENTRY* glGetFBAttParam_t)(GLenum, GLenum, GLenum, GLint*);
typedef void (APIENTRY* glGetRBParam_t)(GLenum, GLenum, GLint*);
typedef void (APIENTRY* glBindRB_t)(GLenum, GLuint);
static glBindFBO_t       r_bindFBO = nullptr;
static glGetFBAttParam_t r_getAtt  = nullptr;
static glGetRBParam_t    r_getRB   = nullptr;
static glBindRB_t        r_bindRB  = nullptr;
static int g_fboLogged = 0;

static void LogS(const char* s) {
    char path[MAX_PATH]; GetModuleFileNameA(GetModuleHandleA(NULL), path, MAX_PATH);
    char* sl = strrchr(path, '\\'); if (sl) *sl = 0;
    char full[MAX_PATH]; wsprintfA(full, "%s\\swse_glspy.txt", path);
    HANDLE h = CreateFileA(full, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wr; char line[300]; int n = wsprintfA(line, "%s\r\n", s);
        SetFilePointer(h, 0, NULL, FILE_END); WriteFile(h, line, n, &wr, NULL); CloseHandle(h);
    }
}

// remember names we've already logged (small fixed table, no allocation churn)
static char g_seen[512][64];
static int  g_seenN = 0;
static bool Seen(const char* name) {
    for (int i = 0; i < g_seenN; i++) if (!lstrcmpA(g_seen[i], name)) return true;
    if (g_seenN < 512) { lstrcpynA(g_seen[g_seenN], name, 64); g_seenN++; }
    return false;
}

static void WriteJump(void* target, void* dest) {
    DWORD old; VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old);
    BYTE* t = (BYTE*)target; t[0] = 0xE9; *(DWORD*)(t + 1) = (DWORD)((BYTE*)dest - (t + 5));
    VirtualProtect(target, 5, old, &old);
}
static void Restore(void* t) { DWORD o; VirtualProtect(t, 5, PAGE_EXECUTE_READWRITE, &o); memcpy(t, g_orig, 5); VirtualProtect(t, 5, o, &o); }

// (wglGetProcAddress hook kept only for the name log; FBO capture is done via
//  an inline patch on glBindFramebufferEXT itself - see below.)
static PROC WINAPI HookedGPA(LPCSTR name) {
    Restore((void*)g_realGPA);
    PROC r = g_realGPA(name);
    WriteJump((void*)g_realGPA, (void*)&HookedGPA);
    return r;
}

#define GL_FRAMEBUFFER_EXT              0x8D40
#define GL_DEPTH_ATTACHMENT_EXT         0x8D00
#define GL_COLOR_ATTACHMENT0_EXT        0x8CE0
#define GL_FB_ATT_OBJ_TYPE             0x8CD0
#define GL_FB_ATT_OBJ_NAME             0x8CD1
#define GL_RENDERBUFFER_EXT            0x8D41
#define GL_RENDERBUFFER_WIDTH_EXT      0x8D42
#define GL_RENDERBUFFER_HEIGHT_EXT     0x8D43
#define GL_RENDERBUFFER_INTERNAL_FMT   0x8D44
#define GL_TEXTURE_ATT_TYPE            0x1702  /* GL_TEXTURE */

// The scene depth texture id, detected live. Updated whenever an FBO with a
// texture depth attachment at (near) screen size is bound.
static volatile unsigned int g_sceneDepthTex = 0;
unsigned int SWSE_SceneDepthTex() { return g_sceneDepthTex; }

// Every distinct depth texture the game attaches to an FBO. They are NOT
// interchangeable -- see the note in the bind hook.
static unsigned g_depthTexIds[16];
static int      g_depthTexN = 0;
static bool     g_depthForced = false;

int SWSE_DepthTexList(unsigned* out, int maxOut) {
    int n = (g_depthTexN < maxOut) ? g_depthTexN : maxOut;
    for (int i = 0; i < n; i++) out[i] = g_depthTexIds[i];
    return n;
}
// id 0 = go back to tracking whichever was bound last.
void SWSE_ForceSceneDepthTex(unsigned id) {
    g_depthForced = (id != 0);
    if (id) g_sceneDepthTex = id;
}

// Choose the depth texture that actually holds the scene.
//
// "Keep whichever FBO was bound last" is not good enough: measured live, that
// rule selected a 6720x4200 buffer whose nearest surface was 999.83 units --
// i.e. entirely at the far plane, completely empty -- while the real scene
// depth sat in a different texture. Sampling an empty depth buffer silently
// produces meaningless AO/GI.
//
// So score the candidates: a texture is only usable if it contains real
// geometry (something well in front of the far plane), and among those we take
// the largest, which is the main scene buffer rather than a shadow map.
// Must run with the GL context current (i.e. from the frame hook).
bool SWSE_AutoPickDepthTex() {
    if (g_depthForced) return true;
    DWORD t0 = GetTickCount();
    int   nRead = 0;
    GLint prevTex = 0;
    glGetIntegerv(0x8069 /*GL_TEXTURE_BINDING_2D*/, &prevTex);

    unsigned best = 0;
    long long bestPixels = 0;
    float bestNear = 2.0f;      // smaller = contains geometry closer to the camera
    for (int i = 0; i < g_depthTexN; i++) {
        glBindTexture(GL_TEXTURE_2D, (GLuint)g_depthTexIds[i]);
        GLint tw = 0, th = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
        if (tw <= 0 || th <= 0) continue;
        long long pixels = (long long)tw * th;
        // Shadow maps are small and square; the scene buffer is screen-shaped.
        if (pixels < 200000) continue;
        // Cap the readback. glGetTexImage has to pull the WHOLE texture across
        // the bus: a 6720x4200 depth buffer is 113 MB of floats, and testing
        // several of those is what froze the game for ~20s every time the
        // effect was switched on. The real scene depth is the backbuffer-sized
        // one (3360x2100 = 28 MB here), so anything vastly larger is a
        // supersampled buffer we do not want anyway.
        if (pixels > 12000000) continue;
        float* buf = (float*)malloc((size_t)pixels * sizeof(float));
        if (!buf) continue;                     // too big to check: skip, don't guess
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        DWORD tr = GetTickCount();
        glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, buf);
        nRead++;
        {   // per-readback timing: depth readback on some drivers is glacial
            char tb[140];
            wsprintfA(tb, "PICKDEPTH: read tex %u %dx%d took %u ms",
                      g_depthTexIds[i], tw, th, GetTickCount() - tr);
            LogS(tb);
        }
        float mn = 1.0f;
        for (long long k = 0; k < pixels; k += 11) {
            float v = buf[k];
            if (v > 0.0f && v < mn) mn = v;
        }
        free(buf);
        if (mn > 0.99f) continue;               // nothing but far plane => empty
        // Bigger is better (scene buffer over shadow map), but on a tie prefer
        // whichever holds the CLOSEST geometry: several buffers share the same
        // dimensions and only the most complete one contains near objects such
        // as the first-person weapon.
        bool better = (pixels > bestPixels) ||
                      (pixels == bestPixels && mn < bestNear);
        if (better) { bestPixels = pixels; bestNear = mn; best = g_depthTexIds[i]; }
    }
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);

    if (best) {
        char b[160];
        // wsprintfA has no %lld - it printed a literal "ld" before.
        wsprintfA(b, "PICKDEPTH: texture %u (%d kpx, nearest %d/1000) - %d readback(s), %u ms total",
                  best, (int)(bestPixels / 1000), (int)(bestNear * 1000.0f),
                  nRead, GetTickCount() - t0);
        LogS(b);
        g_sceneDepthTex = best;
        return true;
    }
    return false;
}

// ---- SWSE HD: RUNTIME TEXTURE REPLACEMENT (TexMod-style) --------------
// Every compressed texture the engine uploads passes through this hook. We
// fingerprint the vanilla data (FNV-1a of the first 4KB ^ dims); if a
// replacement file exists in SWSEMods\SWSE HD\textures\<HASH>.oft,
// we upload OUR data (any resolution!) instead and swallow the engine's
// vanilla mip uploads. The archives stay 100%% vanilla - nothing can crash.
//
// .oft layout: 'OFT1' u32 | glfmt u32 | w u32 | h u32 | levels u32 |
//              then per level: size u32, data[size]
typedef void (APIENTRY* glCompTex2D_t)(GLenum, GLint, GLenum, GLsizei, GLsizei,
                                       GLint, GLsizei, const void*);
typedef void (APIENTRY* glTexParami_t)(GLenum, GLenum, GLint);
static glCompTex2D_t r_compTex = nullptr;
static BYTE g_ctOrig[5];
static int  g_ctLogged = 0;

static void RestoreCT() { DWORD o; VirtualProtect((void*)r_compTex, 5, PAGE_EXECUTE_READWRITE, &o); memcpy((void*)r_compTex, g_ctOrig, 5); VirtualProtect((void*)r_compTex, 5, o, &o); }

static unsigned Fnv1a(const unsigned char* p, int n) {
    unsigned h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

// loaded replacement cache
struct HdTex {
    unsigned hash;          // vanilla fingerprint
    int      state;         // 0=unknown, 1=loaded, 2=no file (negative cache)
    unsigned glfmt, w, h, levels;
    unsigned char* blob;    // level data, packed [size u32][data]...
};
// Replacement cache. This used to be 160 entries consumed by EVERY texture the
// game uploaded, including the hundreds with no replacement file (kept as a
// negative cache). The game uploads far more than 160 distinct textures, so the
// table filled with misses and HdLookup then returned null for everything after
// -- meaning a replacement whose texture happened to upload late could NEVER be
// applied. Measured: of 21 installed .oft files only 2 ever loaded, and the
// first-person crossbow (uploaded 17 times, correct hash, correct format) was
// silently never swapped.
//
// Fixed two ways: the folder is enumerated once so misses cost no slot at all,
// and the table is big enough for a full-game texture pack.
static HdTex g_hd[4096];
static int   g_hdN = 0;

// Hashes we actually have a .oft for, read from the folder once. A texture not
// in this set is rejected immediately: no table slot, no disk hit.
static unsigned* g_hdHave = nullptr;
static int       g_hdHaveN = 0;
static bool      g_hdScanned = false;

// Fingerprint -> the folder its .oft lives in. Storing the owner is what lets
// ANY mod ship textures: a texture pack is a folder with a textures\ subfolder
// and nothing else, and no one has to drop files into the shipped HD mod. On a
// collision the last enabled mod wins, which is what load order promises.
static char*     g_hdOwner = nullptr;         // g_hdCap entries of MAX_PATH
static int       g_hdCap = 0;

static bool HdGrow() {
    int cap = g_hdCap ? g_hdCap * 2 : 1024;
    unsigned* h2 = (unsigned*)realloc(g_hdHave, cap * sizeof(unsigned));
    if (!h2) return false;
    g_hdHave = h2;
    char* o2 = (char*)realloc(g_hdOwner, (size_t)cap * MAX_PATH);
    if (!o2) return false;
    g_hdOwner = o2;
    g_hdCap = cap;
    return true;
}

static void HdScanOneMod(const char* texDir, const char* modName, void*) {
    char glob[MAX_PATH];
    wsprintfA(glob, "%s\\*.oft", texDir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(glob, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    int added = 0, overrode = 0;
    do {
        // filename is the 8-hex-digit fingerprint
        unsigned v = 0; int n = 0; bool ok = true;
        for (const char* p = fd.cFileName; *p && *p != '.'; p++, n++) {
            int d = (*p >= '0' && *p <= '9') ? *p - '0'
                  : (*p >= 'a' && *p <= 'f') ? *p - 'a' + 10
                  : (*p >= 'A' && *p <= 'F') ? *p - 'A' + 10 : -1;
            if (d < 0) { ok = false; break; }
            v = v * 16 + (unsigned)d;
        }
        if (!ok || n != 8) continue;

        int at = -1;
        for (int i = 0; i < g_hdHaveN; i++) if (g_hdHave[i] == v) { at = i; break; }
        if (at < 0) {
            if (g_hdHaveN >= g_hdCap && !HdGrow()) break;
            at = g_hdHaveN++;
            g_hdHave[at] = v;
            added++;
        } else {
            overrode++;                        // a later mod replaces it
        }
        lstrcpynA(g_hdOwner + (size_t)at * MAX_PATH, texDir, MAX_PATH);
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    char b[300];
    wsprintfA(b, "HD: [%s] +%d texture(s)%s", modName, added,
              overrode ? " (some override earlier mods)" : "");
    LogS(b);
}

static void HdBuildFolderIndex() {
    if (g_hdScanned) return;
    g_hdScanned = true;
    SWSE_ForEachModFile("textures", HdScanOneMod, nullptr);
    char b[160];
    if (g_hdHaveN == 0)
        LogS("HD: no mod provides a textures\\ folder (replacement inactive)");
    else {
        wsprintfA(b, "HD: %d replacement texture(s) from %d mod(s)",
                  g_hdHaveN, SWSE_CountModFile("textures"));
        LogS(b);
    }
}

// Where a given fingerprint's .oft lives. Empty if we do not have it.
static const char* HdOwnerDir(unsigned hash) {
    for (int i = 0; i < g_hdHaveN; i++)
        if (g_hdHave[i] == hash) return g_hdOwner + (size_t)i * MAX_PATH;
    return nullptr;
}

static bool HdHaveFile(unsigned hash) {
    for (int i = 0; i < g_hdHaveN; i++) if (g_hdHave[i] == hash) return true;
    return false;
}
// The texid whose vanilla mip uploads we must swallow.
//
// THE BLACK-TEXTURE BUG. This used to be a permanent 256-entry LIST of every
// texid we had ever substituted. OpenGL recycles texture names: when a level
// unloads the game calls glDeleteTextures and those ids go back to the pool,
// then the next level's glGenTextures hands the SAME numbers to completely
// unrelated textures. Our stale list then matched a new texture, so we swallowed
// its mip uploads -- leaving it with mip 0, no mip chain, and a stale
// GL_TEXTURE_MAX_LEVEL. An incomplete texture renders SOLID BLACK, which is
// exactly what appeared on shelves, floors and walls after warping between
// levels, and why it got worse the more textures had been loaded.
//
// The engine uploads a texture's levels consecutively (0, then 1..N), so only
// the MOST RECENT substitution can legitimately still be receiving mips. Track
// one id instead of a list, and clear it whenever a level-0 upload arrives for
// a texture we did not substitute. A recycled id therefore clears the mark
// before it can do any harm.
static GLuint g_subbedTex = 0;
static bool   g_subbedActive = false;

static bool IsSubbed(GLuint id) {
    return g_subbedActive && id == g_subbedTex;
}
static void MarkSubbed(GLuint id) {
    g_subbedTex = id;
    g_subbedActive = true;
}
static void ClearSubbed() {
    g_subbedActive = false;
}

void SWSE_HdStats(int* available, int* loaded, int* failed) {
    if (available) *available = g_hdHaveN;
    int n = 0, bad = 0;
    for (int i = 0; i < g_hdN; i++) {
        if (g_hd[i].state == 1) n++;
        else if (g_hd[i].state == 2) bad++;
    }
    if (loaded) *loaded = n;
    if (failed) *failed = bad;
}

int SWSE_HdFailures(unsigned* out, int max) {
    int n = 0;
    for (int i = 0; i < g_hdN && n < max; i++)
        if (g_hd[i].state == 2) out[n++] = g_hd[i].hash;
    return n;
}

static HdTex* HdLookup(unsigned hash) {
    for (int i = 0; i < g_hdN; i++) if (g_hd[i].hash == hash) return &g_hd[i];
    // Reject unknown hashes BEFORE spending a table slot. This is the whole
    // point of the folder index: the game uploads far more textures than we
    // replace, and letting misses consume slots is what broke this.
    HdBuildFolderIndex();
    if (!HdHaveFile(hash)) return nullptr;
    if (g_hdN >= (int)(sizeof(g_hd)/sizeof(g_hd[0]))) return nullptr;
    HdTex* t = &g_hd[g_hdN++];
    t->hash = hash; t->state = 0; t->blob = nullptr;
    // Load from whichever mod folder claimed this fingerprint during the scan,
    // rather than from a fixed folder.
    const char* dir = HdOwnerDir(hash);
    if (!dir) { t->state = 2; return t; }
    char full[MAX_PATH];
    wsprintfA(full, "%s\\%08X.oft", dir, hash);
    HANDLE f = CreateFileA(full, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { t->state = 2; return t; }
    DWORD sz = GetFileSize(f, NULL), rd = 0;
    unsigned char* buf = (unsigned char*)malloc(sz);
    if (!buf || !ReadFile(f, buf, sz, &rd, NULL) || rd != sz || sz < 20
        || memcmp(buf, "OFT1", 4) != 0) {
        CloseHandle(f); free(buf); t->state = 2; return t;
    }
    CloseHandle(f);
    t->glfmt  = *(unsigned*)(buf + 4);
    t->w      = *(unsigned*)(buf + 8);
    t->h      = *(unsigned*)(buf + 12);
    t->levels = *(unsigned*)(buf + 16);
    t->blob   = buf;                        // keep whole file; levels at +20
    t->state  = 1;
    char b[160];
    wsprintfA(b, "HD: loaded %08X.oft -> %ux%u levels=%u", hash, t->w, t->h, t->levels);
    LogS(b);
    return t;
}

static void APIENTRY HookedCompTex2D(GLenum target, GLint level, GLenum ifmt,
                                     GLsizei w, GLsizei h, GLint border,
                                     GLsizei size, const void* data) {
    RestoreCT();
    GLint bound = 0; glGetIntegerv(0x8069 /*GL_TEXTURE_BINDING_2D*/, &bound);

    bool handled = false;
    if (level == 0 && data && size >= 64 && w >= 64) {
        unsigned hash = Fnv1a((const unsigned char*)data, size < 4096 ? size : 4096)
                        ^ ((unsigned)w * 73856093u) ^ ((unsigned)h * 19349663u);
        if (g_ctLogged < 200) {   // keep the diagnostic log (now with hash)
            char b[160];
            wsprintfA(b, "TEXSPY: %dx%d fmt=0x%X size=%d texid=%d hash=%08X",
                      w, h, ifmt, size, bound, hash);
            LogS(b); g_ctLogged++;
        }
        // Tell the foliage tracker which id this data landed in. Done for
        // EVERY level-0 upload, not just foliage, so a recycled id gets its
        // stale flag cleared rather than inherited.
        SWSE_FoliageNoteUpload(hash, (unsigned)bound);

        // With HD replacement disabled the lookup is skipped entirely, so the
        // .oft folder is never even indexed - the game uploads its own texture
        // exactly as it always did.
        HdTex* hd = SWSE_Feature(FEAT_HDTEXTURES) ? HdLookup(hash) : nullptr;
        if (hd && hd->state == 1) {
            // upload OUR texture instead - every level, any resolution
            unsigned char* p = hd->blob + 20;
            for (unsigned lv = 0; lv < hd->levels; lv++) {
                unsigned lsz = *(unsigned*)p; p += 4;
                r_compTex(target, (GLint)lv, (GLenum)hd->glfmt,
                          (GLsizei)(hd->w >> lv ? hd->w >> lv : 1),
                          (GLsizei)(hd->h >> lv ? hd->h >> lv : 1),
                          0, (GLsizei)lsz, p);
                p += lsz;
            }
            glTexParameteri(GL_TEXTURE_2D, 0x813D /*GL_TEXTURE_MAX_LEVEL*/,
                            (GLint)hd->levels - 1);
            MarkSubbed((GLuint)bound);
            handled = true;
        } else {
            // A level-0 upload we are NOT substituting ends any previous
            // substitution. Without this, a recycled texture id kept matching
            // the old mark and had its mip chain swallowed -> black texture.
            ClearSubbed();
        }
    } else if (level > 0 && IsSubbed((GLuint)bound)) {
        handled = true;   // swallow the engine's vanilla mips - ours are in place
    }

    if (!handled)
        r_compTex(target, level, ifmt, w, h, border, size, data);
    WriteJump((void*)r_compTex, (void*)&HookedCompTex2D);
}

// ---- camera introspection -------------------------------------------------
// MEASURED: the scene depth texture spans only ~0.96..0.99 of the depth range,
// so linearising it with guessed constants (the old depth_near=1/depth_far=120
// from settings.txt) turns the tiny per-pixel deltas into noise -- and every
// normal reconstructed by differencing those depths comes out garbage. That,
// not "the game has no normals", is why depth-based GI looked flat.
//
// So read the camera for real. Exact near/far/FOV is broadly useful to mods
// (FOV changes, culling, any screen-space effect), not just to the shader.
#define GL_PROJECTION_MATRIX_ 0x0BA7
static float g_projNear = 0, g_projFar = 0, g_projFovY = 0;
static bool  g_projFound = false;

// A GL perspective projection (column-major) has m[11] == -1 and m[15] == 0.
// From m[10] = -(f+n)/(f-n) and m[14] = -2fn/(f-n):
//     n = m14/(m10-1),  f = m14/(m10+1)
// Diagnostic: log the distinct matrices we actually see, so a failure to find
// a perspective frustum can be told apart from "we never looked at the right
// moment". Logs only when the matrix changes, capped.
static int   g_pmLogged = 0;
static float g_pmLast[16];
static void LogMatrixOnce(const float* m) {
    if (g_pmLogged >= 10) return;
    if (memcmp(m, g_pmLast, sizeof(g_pmLast)) == 0) return;
    memcpy(g_pmLast, m, sizeof(g_pmLast));
    g_pmLogged++;
    char b[300];
    wsprintfA(b, "PROJMAT: m5=%d/1000 m10=%d/1000 m11=%d/1000 m14=%d/1000 m15=%d/1000",
              (int)(m[5]*1000), (int)(m[10]*1000), (int)(m[11]*1000),
              (int)(m[14]*1000), (int)(m[15]*1000));
    LogS(b);
}

static void ConsiderProjection(const float* m) {
    if (!m) return;
    LogMatrixOnce(m);
    if (m[15] != 0.0f) return;                       // ortho / modelview
    if (m[11] > -0.99f || m[11] < -1.01f) return;    // not a perspective frustum
    float dn = m[10] - 1.0f, df = m[10] + 1.0f;
    if (dn == 0.0f || df == 0.0f) return;
    float n = m[14] / dn, f = m[14] / df;
    if (!(n > 0.0f) || !(f > n)) return;             // also rejects NaN
    // Shadow and reflection passes use their own small frusta. The main scene
    // camera is the one that sees furthest, so keep the largest far plane.
    if (g_projFound && f <= g_projFar) return;
    g_projNear = n; g_projFar = f;
    g_projFovY = (m[5] != 0.0f) ? (2.0f * (float)atan(1.0 / m[5])) * 57.2957795f : 0.0f;
    if (!g_projFound) {
        char b[220];
        wsprintfA(b, "PROJ: near=%d.%03d far=%d.%03d fovY=%d.%02d deg",
                  (int)n, (int)((n < 0 ? -n : n) * 1000) % 1000,
                  (int)f, (int)((f < 0 ? -f : f) * 1000) % 1000,
                  (int)g_projFovY, (int)(g_projFovY * 100) % 100);
        LogS(b);
    }
    g_projFound = true;
}

// ---- finding the camera frustum in memory ---------------------------------
// MEASURED: the game never uses the fixed-function projection stack (the only
// GL_PROJECTION_MATRIX we ever see is the identity), so glGetFloatv cannot
// give us the camera. But the engine reflects a frustum struct laid out as
//     +0x00 m_zNear   +0x04 m_zFar   +0x08 m_fovRad   ...   +0x6C m_tanHalfFov
// which we can find by value signature. The tanHalfFov field is the reason
// this is reliable rather than a guess: it must agree with tan(fovRad/2), and
// that cross-check rejects essentially every coincidental float triple.
#define FRUSTUM_ZNEAR   0x00
#define FRUSTUM_ZFAR    0x04
#define FRUSTUM_FOVRAD  0x08
#define FRUSTUM_TANHALF 0x6C

static bool PlausibleFrustum(const BYTE* p) {
    float n = *(const float*)(p + FRUSTUM_ZNEAR);
    float f = *(const float*)(p + FRUSTUM_ZFAR);
    float fov = *(const float*)(p + FRUSTUM_FOVRAD);
    float t = *(const float*)(p + FRUSTUM_TANHALF);
    // Ranges written so NaN fails every test rather than sneaking through.
    // MEASURED: the real camera is near=0.5 far=1000 fov=80deg. Requiring a far
    // plane of at least 50 units drops the coincidental triples the loose
    // version matched (far=0.204, 0.887, 0.929 -- nonsense for a scene camera)
    // which had filled the result cap with junk.
    if (!(n > 0.05f && n < 100.0f))      return false;
    if (!(f > 50.0f && f < 1000000.0f))  return false;
    if (!(f > n * 10.0f))                return false;
    if (!(fov > 0.2f && fov < 3.0f))     return false;   // radians
    if (!(t > 0.05f && t < 10.0f))       return false;
    // The clincher: tanHalfFov must match the fov it sits next to. Note every
    // cheap range test above runs FIRST -- tan() is comparatively expensive and
    // this predicate is evaluated on every 4-byte offset of the scan.
    float want = (float)tan(fov * 0.5);
    float alt  = (float)tan(fov);                        // in case fov is a half-angle
    float d1 = t - want, d2 = t - alt;
    if (d1 < 0) d1 = -d1;
    if (d2 < 0) d2 = -d2;
    return (d1 < 0.02f || d2 < 0.02f);
}

// Scan committed, writable, non-image memory for the frustum. Returns how many
// candidates were found and fills the caller's arrays.
int SWSE_ScanCameraFrustum(unsigned* addrs, float* nears, float* fars,
                           float* fovs, int maxOut) {
    int found = 0;
    MEMORY_BASIC_INFORMATION mbi;
    // MEASURED: this scan was the F10 freeze. Walking the whole address space
    // four bytes at a time, calling tan() on every candidate, took **131
    // SECONDS** -- and SetAllUniforms calls SWSE_SceneProjection twice per
    // frame, so it ran from the render path. Widening the end bound to 0xFFFF0000
    // for the 4GB patch had made it twice as slow again.
    //
    // The frustum is a heap object, so scan the heap, exactly like every other
    // scan in scriptvm.cpp does (HEAP_LO/HEAP_HI). That is ~768MB instead of
    // ~4GB and skips the module images, stacks and reserved ranges entirely.
    BYTE* addr = (BYTE*)0x10000000;
    BYTE* end  = (BYTE*)0x40000000;
    while (addr < end && found < maxOut) {
        if (!VirtualQuery(addr, &mbi, sizeof(mbi))) break;
        BYTE* base = (BYTE*)mbi.BaseAddress;
        SIZE_T size = mbi.RegionSize;
        bool usable = (mbi.State == MEM_COMMIT) &&
                      !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                      (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                                      PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
        if (usable && size >= 0x80) {
            __try {
                BYTE* stop = base + size - 0x80;
                for (BYTE* p = base; p < stop; p += 4) {
                    if (!PlausibleFrustum(p)) continue;
                    if (addrs) addrs[found] = (unsigned)(uintptr_t)p;
                    if (nears) nears[found] = *(float*)(p + FRUSTUM_ZNEAR);
                    if (fars)  fars[found]  = *(float*)(p + FRUSTUM_ZFAR);
                    if (fovs)  fovs[found]  = *(float*)(p + FRUSTUM_FOVRAD);
                    if (++found >= maxOut) break;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) { }
        }
        BYTE* next = base + size;
        if (next <= addr) break;
        addr = next;
    }
    return found;
}

// Adopt a scanned frustum as the camera when the GL path found nothing.
bool SWSE_AdoptScannedCamera() {
    enum { SCAN_N = 256 };
    static unsigned a[SCAN_N]; static float n[SCAN_N], f[SCAN_N], v[SCAN_N];
    int c = SWSE_ScanCameraFrustum(a, n, f, v, SCAN_N);
    if (c <= 0) return false;
    // Pick the MODE, not the maximum. "Widest frustum" looked reasonable but
    // picked a lone outlier (near=1.009 far=1449) over the real camera: the
    // engine keeps many copies of the active frustum for its various passes,
    // so the true camera is the value that repeats, while junk matches are
    // one-offs. Ties go to the wider frustum.
    int best = 0, bestCount = 0;
    for (int i = 0; i < c; i++) {
        int count = 0;
        for (int j = 0; j < c; j++) {
            float dn = n[j] - n[i], df = f[j] - f[i];
            if (dn < 0) dn = -dn;
            if (df < 0) df = -df;
            if (dn < 0.001f && df < 0.5f) count++;
        }
        if (count > bestCount || (count == bestCount && f[i] > f[best])) {
            bestCount = count; best = i;
        }
    }
    g_projNear = n[best]; g_projFar = f[best];
    g_projFovY = v[best] * 57.2957795f;
    g_projFound = true;
    char b[220];
    wsprintfA(b, "PROJ(scan): near=%d.%03d far=%d.%03d fovY=%d.%02d deg "
                 "(%d of %d candidates agree)",
              (int)g_projNear, (int)(g_projNear * 1000) % 1000,
              (int)g_projFar,  (int)(g_projFar * 1000) % 1000,
              (int)g_projFovY, (int)(g_projFovY * 100) % 100, bestCount, c);
    LogS(b);
    return true;
}

bool SWSE_SceneProjection(float* nearZ, float* farZ, float* fovY) {
    // Auto-acquire: the GL path never succeeds on this engine, so fall back to
    // the memory scan on demand. Throttled because the scan walks the address
    // space -- callers run per frame, and the camera only changes per level.
    if (!g_projFound) {
        static DWORD s_nextTry = 0;
        DWORD now = GetTickCount();
        if (now >= s_nextTry) {
            s_nextTry = now + 5000;
            SWSE_AdoptScannedCamera();
        }
    }
    if (!g_projFound) return false;
    if (nearZ) *nearZ = g_projNear;
    if (farZ)  *farZ  = g_projFar;
    if (fovY)  *fovY  = g_projFovY;
    return true;
}

// ---- frame structure tracing ---------------------------------------------
// UI and the first-person weapon are drawn AFTER the depth pass, so at those
// pixels the depth buffer still holds the world behind them and our effects
// shade them with the wrong geometry (visible as scenery ghosting through the
// inventory poster). The fix is to run the post-process before the UI is
// composited -- which means knowing where in the frame that happens. This logs
// the FBO bind order for a few frames so the structure is measured, not guessed.
static volatile int g_fboTraceFrames = 0;
static int g_fboTraceSeq = 0;
void SWSE_TraceFBO(int frames) { g_fboTraceSeq = 0; g_fboTraceFrames = frames; }
void SWSE_TraceFBOFrameMark() {
    if (g_fboTraceFrames > 0) {
        g_fboTraceFrames--;
        g_fboTraceSeq = 0;
        LogS("FBOSEQ: ---------------- SWAP ----------------");
    }
}

// inline-hook state for glBindFramebufferEXT
static BYTE g_fboOrig[5];
static void RestoreFBO() { DWORD o; VirtualProtect((void*)r_bindFBO, 5, PAGE_EXECUTE_READWRITE, &o); memcpy((void*)r_bindFBO, g_fboOrig, 5); VirtualProtect((void*)r_bindFBO, 5, o, &o); }

// Our hook for glBindFramebufferEXT: when the game binds a real FBO, inspect its
// depth attachment and log id/size/format. Logs each distinct FBO once.
// Uses unhook -> call real -> rehook (single render thread, like the swap hook).
static GLuint g_lastFbo = 0;
static bool   g_inEarlyPass = false;   // hard re-entry guard for the early pass

// ---- the finished scene colour -------------------------------------------
// MEASURED in game with fbotrace (26 passes per frame):
//
//   bind 25: ending-vp=3840x2160  colorType=GL_TEXTURE  colorName=7   <- scene
//   bind 26: ending-vp=7680x4320  colorType=0x0 colorName=0  -> fbo=0 <- depth only
//
// Texture 7 at 3840x2160 (2x the 1920x1080 window) is the finished scene image.
// The pass immediately before fbo=0 has NO colour attachment at all, which is
// why the first attempt at an early pass crashed: it copied colour from a
// framebuffer that had none.
//
// So remember the last real colour attachment instead, and process THAT.
static GLuint g_sceneColorTex = 0;
static GLint  g_sceneColorW = 0, g_sceneColorH = 0;

unsigned SWSE_SceneColorTex() { return g_sceneColorTex; }
int      SWSE_SceneColorW()   { return g_sceneColorW; }
int      SWSE_SceneColorH()   { return g_sceneColorH; }

static void APIENTRY HookedBindFBO(GLenum target, GLuint fbo) {
    RestoreFBO();
    // THE EARLY-PASS TRIGGER. Measured frame structure: N binds of the scene
    // FBO, then exactly one bind of fbo=0, after which the game composites and
    // draws UI. At this point the real bind has NOT happened yet, so the scene
    // FBO is still current and holds the finished frame with no UI on it -- the
    // one moment in the frame where the scene can be processed alone.
    //
    // Order matters, and getting it wrong crashed the game: this must run AFTER
    // RestoreFBO() so glBindFramebufferEXT is unpatched while our code runs. It
    // used to run with the patch still installed, so any re-entry landed back in
    // this hook and recursed until the stack died. g_lastFbo is likewise updated
    // BEFORE the call, so even a re-entry cannot re-trigger the pass.
    // Remember the last real colour attachment before we leave for fbo=0.
    // Only accept a screen-shaped target: the shadow/dof passes attach 512x512
    // and other odd sizes, and those are not the scene.
    if (fbo != 0 && r_getAtt) {
        GLint ct = 0, cn = 0;
        r_getAtt(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_FB_ATT_OBJ_TYPE, &ct);
        r_getAtt(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_FB_ATT_OBJ_NAME, &cn);
        if (ct == GL_TEXTURE_ATT_TYPE && cn > 0) {
            GLint vp[4] = {0,0,0,0};
            glGetIntegerv(GL_VIEWPORT, vp);
            if (vp[2] >= 1024 && vp[3] >= 512) {   // screen-shaped, not a shadow map
                g_sceneColorTex = (GLuint)cn;
                g_sceneColorW = vp[2];
                g_sceneColorH = vp[3];
            }
        }
    }

    GLuint prevFbo = g_lastFbo;
    g_lastFbo = fbo;
    if (fbo == 0 && prevFbo != 0 && !g_inEarlyPass) {
        // The gate is now "do we know which texture holds the finished scene",
        // not "does the currently bound framebuffer have colour". Measured: the
        // pass immediately before fbo=0 is DEPTH-ONLY (colorType=0x0), which is
        // why the first attempt crashed reading colour from it. gfx.cpp attaches
        // g_sceneColorTex to an FBO of its own instead.
        if (g_sceneColorTex != 0) {
            g_inEarlyPass = true;
            SWSE_GfxProcessSceneFBOProtected();
            g_inEarlyPass = false;
        }
    }
    r_bindFBO(target, fbo);   // real bind (bytes restored)
    WriteJump((void*)r_bindFBO, (void*)&HookedBindFBO);
    if (g_fboTraceFrames > 0 && g_fboTraceSeq < 40) {
        // Log the viewport and colour attachment of the pass that is ENDING
        // (the currently bound FBO), not the one being bound. The game reuses a
        // single FBO id and swaps attachments per pass, so the sizes are the
        // only way to tell the full-resolution scene pass from bloom /
        // downsample / shadow passes.
        GLint vp[4] = {0,0,0,0};
        glGetIntegerv(GL_VIEWPORT, vp);
        GLint ctype = 0, cname = 0;
        if (r_getAtt) {
            r_getAtt(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_FB_ATT_OBJ_TYPE, &ctype);
            r_getAtt(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_FB_ATT_OBJ_NAME, &cname);
        }
        char tb[200];
        wsprintfA(tb, "FBOSEQ: %2d  ending-vp=%dx%d colorType=0x%X colorName=%d  -> bind fbo=%u",
                  g_fboTraceSeq++, vp[2], vp[3], ctype, cname, fbo);
        LogS(tb);
    }
    if (fbo != 0) {
        // Sample the current projection here rather than hooking glLoadMatrixf:
        // matrix loads happen thousands of times a frame and the unhook/rehook
        // dance would cost far more than it is worth. FBO binds are a few per
        // frame, and the scene camera is current for the scene's own passes.
        float pm[16];
        glGetFloatv(GL_PROJECTION_MATRIX_, pm);
        ConsiderProjection(pm);
    }
    if (fbo != 0 && r_getAtt) {
        GLint dtype = 0, dname = 0;
        r_getAtt(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_FB_ATT_OBJ_TYPE, &dtype);
        r_getAtt(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_FB_ATT_OBJ_NAME, &dname);
        // depth as a TEXTURE => we can sample it directly. Remember its id.
        if (dtype == GL_TEXTURE_ATT_TYPE && dname > 0) {
            // Record every distinct depth texture. We used to blindly keep the
            // last one bound, but they are not equivalent: the one we settled on
            // does not contain the first-person weapon (measured -- the nearest
            // depth on screen is ~4.8 units while the crossbow sits under 2), so
            // weapon pixels get shaded using the wall behind them. Being able to
            // enumerate and pick is what makes that fixable.
            bool known = false;
            for (int i = 0; i < g_depthTexN; i++)
                if (g_depthTexIds[i] == (unsigned)dname) { known = true; break; }
            if (!known && g_depthTexN < 16) g_depthTexIds[g_depthTexN++] = (unsigned)dname;
            if (!g_depthForced) g_sceneDepthTex = (unsigned int)dname;
        }
        if (g_fboLogged < 12) {
            char b[220];
            wsprintfA(b, "FBO bind: id=%u depthAttType=0x%X depthName=%d", fbo, dtype, dname);
            LogS(b); g_fboLogged++;
        }
    }
}

void SWSE_InstallGLSpy() {
    static bool installed = false;
    if (installed) return;          // guard: two GL contexts => called twice
    installed = true;
    HMODULE gl = GetModuleHandleA("opengl32.dll");
    if (!gl) { installed = false; return; }
    g_realGPA = (wglGPA_t)GetProcAddress(gl, "wglGetProcAddress");
    if (!g_realGPA) { LogS("glspy: wglGetProcAddress not found"); return; }

    // resolve the FBO-inspection helpers we need (via the real GPA)
    r_getAtt = (glGetFBAttParam_t)g_realGPA("glGetFramebufferAttachmentParameterivEXT");
    r_getRB  = (glGetRBParam_t)   g_realGPA("glGetRenderbufferParameterivEXT");
    r_bindRB = (glBindRB_t)       g_realGPA("glBindRenderbufferEXT");
    r_bindFBO= (glBindFBO_t)      g_realGPA("glBindFramebufferEXT");

    memcpy(g_orig, (void*)g_realGPA, 5);
    WriteJump((void*)g_realGPA, (void*)&HookedGPA);

    // INLINE-hook glBindFramebufferEXT directly (catches the game's cached
    // pointer regardless of when it resolved the function - beats the race).
    if (r_bindFBO) {
        memcpy(g_fboOrig, (void*)r_bindFBO, 5);
        WriteJump((void*)r_bindFBO, (void*)&HookedBindFBO);
        LogS("==== SWSE GL spy installed - glBindFramebufferEXT inline-hooked ====");
    } else {
        LogS("==== SWSE GL spy: glBindFramebufferEXT not resolvable ====");
    }

    // TEXSPY: inline-hook glCompressedTexImage2D(ARB) - logs engine texture
    // creation dims so we can see whether it obeys the archive records.
    r_compTex = (glCompTex2D_t)g_realGPA("glCompressedTexImage2D");
    if (!r_compTex)
        r_compTex = (glCompTex2D_t)g_realGPA("glCompressedTexImage2DARB");
    if (r_compTex) {
        memcpy(g_ctOrig, (void*)r_compTex, 5);
        WriteJump((void*)r_compTex, (void*)&HookedCompTex2D);
        LogS("==== TEXSPY armed: logging compressed texture creation ====");
    } else {
        LogS("==== TEXSPY: glCompressedTexImage2D not resolvable ====");
    }
}
