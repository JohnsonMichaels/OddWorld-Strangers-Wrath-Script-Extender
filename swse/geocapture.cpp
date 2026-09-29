// SWSE geometry capture probe - see geocapture.h.
//
// Reads the vertex/index data behind a draw call the way a BVH builder would
// have to, then proves the coordinate space by projecting the result with the
// camera matrix: if captured vertices land inside the screen's clip volume,
// the data is world space and a tracer can consume the draw stream directly.

#include "geocapture.h"
#include "wind.h"
#include "foliage.h"   // SWSE_FoliageCurrentIsFoliage - leaf cards are not solid
#include <stdio.h>
#include <string.h>
#include <math.h>

#pragma comment(lib, "opengl32.lib")

#define GL_ARRAY_BUFFER_          0x8892
#define GL_ELEMENT_ARRAY_BUFFER_  0x8893
#define GL_ARRAY_BUFFER_BINDING_  0x8894
#define GL_ELEM_ARRAY_BINDING_    0x8895
#define GL_VAA_ENABLED            0x8622
#define GL_VAA_SIZE               0x8623
#define GL_VAA_STRIDE             0x8624
#define GL_VAA_TYPE               0x8625
#define GL_VAA_POINTER            0x8645
#define GL_VAA_BUFFER_BINDING     0x889F
#define GL_TRIANGLES_             0x0004
#define GL_TRIANGLE_STRIP_        0x0005
#define GL_TRIANGLE_FAN_          0x0006

typedef ptrdiff_t GLintptr_;
typedef ptrdiff_t GLsizeiptr_;
typedef void (APIENTRY* pfnBindBuffer)(GLenum, GLuint);
typedef void (APIENTRY* pfnGetBufferSubData)(GLenum, GLintptr_, GLsizeiptr_, void*);
typedef void (APIENTRY* pfnGetVertexAttribiv)(GLuint, GLenum, GLint*);
typedef void (APIENTRY* pfnGetVertexAttribPointerv)(GLuint, GLenum, void**);
typedef void (APIENTRY* pfnGetProgramLocalParameterfv)(GLenum, GLuint, GLfloat*);
typedef void (APIENTRY* pfnGetProgramivARB)(GLenum, GLenum, GLint*);
typedef void (APIENTRY* pfnGetProgramStringARB)(GLenum, GLenum, void*);
typedef void (APIENTRY* pfnGetVertexAttribfv)(GLuint, GLenum, GLfloat*);

static pfnBindBuffer              p_BindBuffer;
static pfnGetBufferSubData        p_GetBufferSubData;
static pfnGetVertexAttribiv       p_GetVertexAttribiv;
static pfnGetVertexAttribPointerv p_GetVertexAttribPointerv;
static pfnGetProgramLocalParameterfv p_GetProgLocal;
static pfnGetProgramivARB      p_GetProgramivARB;
typedef void (APIENTRY* pfnBindProgramARBGeo)(GLenum, GLuint);
static pfnBindProgramARBGeo    p_BindProgramARBGeo;
static pfnGetProgramStringARB  p_GetProgramString;
static pfnGetVertexAttribfv    p_GetVertexAttribfv;

static void* GlProc(const char* name) {
    HMODULE gl = GetModuleHandleA("opengl32.dll");
    if (!gl) return 0;
    typedef PROC (WINAPI* wglGPA_t)(LPCSTR);
    static wglGPA_t wglGPA = 0;
    if (!wglGPA) wglGPA = (wglGPA_t)GetProcAddress(gl, "wglGetProcAddress");
    if (!wglGPA) return 0;
    void* p = (void*)wglGPA(name);
    if (!p) p = (void*)GetProcAddress(gl, name);
    return p;
}

// ---- report buffer ---------------------------------------------------------
#define MAX_LINES 80
#define LINE_LEN  200
static char g_lines[MAX_LINES][LINE_LEN];
static int  g_nLines = 0;
static char g_tmp[LINE_LEN];

// Crash breadcrumbs. Written and CLOSED per call so the last line survives a
// hard crash - a buffered logger loses exactly the line that matters.
static void GeoTrace(const char* s) {
    char path[MAX_PATH];
    GetModuleFileNameA(GetModuleHandleA(NULL), path, MAX_PATH);
    char* slash = 0;
    for (char* p = path; *p; p++) if (*p == '\\' || *p == '/') slash = p;
    if (!slash) return;
    lstrcpyA(slash + 1, "swse_geotrace.txt");
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, 0,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    SetFilePointer(h, 0, 0, FILE_END);
    WriteFile(h, s, lstrlenA(s), &w, 0);
    WriteFile(h, "\r\n", 2, &w, 0);
    FlushFileBuffers(h);
    CloseHandle(h);
}

static void Line(const char* s) {
    if (g_nLines >= MAX_LINES) return;
    lstrcpynA(g_lines[g_nLines++], s, LINE_LEN);
}

// wsprintfA cannot do floats; render as sign + whole.milli.
// Every caller passes a `char[24]`. Huge and non-finite values MUST be caught
// here: (int)1e30f is undefined and lands on INT_MIN, which "%d" prints as 11
// characters - and this format emits the integer part AND the fraction, so a
// negative sentinel produced "-2147483648.-2147483648", 24 chars plus a NUL,
// one byte past the buffer. That smashed the stack and killed the game on the
// report's RETURN: every line printed, "report EXIT ok" was written, and the
// process died on the way out. The 1e30 rejection sentinel reaches this.
static void F3(char* out, float v) {
    float a = v < 0 ? -v : v;
    if (!(a < 1e9f)) {                        // also true for NaN
        lstrcpyA(out, (v != v) ? "NaN" : (v < 0 ? "-HUGE" : "HUGE"));
        return;
    }
    int w = (int)a;
    int m = (int)((a - (float)w) * 1000.0f);
    if (m < 0) m = 0;
    wsprintfA(out, "%s%d.%03d", v < 0 ? "-" : "", w, m);
}

// Stage 1 lives at the bottom of this file but is driven from the capture
// path above it.
static void HarvestRecord(GLenum mode, GLsizei count, GLenum type, const void* indices);
static void HarvestRun(void);
// Returns 1 if the harvest re-armed because the camera was not ready yet.
static int  HarvestRetryIfNoCamera(void);
// 1 = continuous mode handled this frame; the state it touches is defined
// further down, so this is a function rather than a pile of forward decls.
static int  HarvestContinuousTick(void);
static void RefreshCamera(void);
// 1 = more frames still wanted; refreshes the camera for the next one.
static int  HarvestMoreFrames(void);

static int g_readErrors = 0, g_readErrLogged = 0;
static unsigned g_lastReadError = 0;
typedef void (APIENTRY* pfnGetBufferParameteriv)(GLenum, GLenum, GLint*);
static pfnGetBufferParameteriv p_GetBufferParameteriv;

// ---- state -----------------------------------------------------------------
static int g_armed = 0;
static int g_want = 0;
static int g_got = 0;
static int g_frameTris = 0;      // census across ALL world draws this frame
static int g_frameDraws = 0;
static int g_vboDraws = 0, g_clientDraws = 0;
static int g_onScreen = 0, g_offScreen = 0;
// Matrix-free space test: object-space vertices cluster near each mesh's own
// origin, so captures from different draws would all be small and similar.
// World-space vertices sit at the position the object occupies in the level,
// so captures spread across hundreds of units. The spread IS the answer, and
// unlike the projection test it needs no assumption about which program local
// holds the camera (locals 1..4 is a foliage-program convention, not a law -
// assuming it universally is what produced a false "not world space" verdict).
static float g_lo[3] = { 1e30f, 1e30f, 1e30f };
static float g_hi[3] = { -1e30f, -1e30f, -1e30f };
static int   g_nVerts = 0;

int SWSE_GeoArmed() { return g_armed; }

void SWSE_GeoArm(int nDraws) {
    if (!p_BindBuffer) {
        p_BindBuffer              = (pfnBindBuffer)GlProc("glBindBuffer");
        p_GetBufferSubData        = (pfnGetBufferSubData)GlProc("glGetBufferSubData");
        p_GetVertexAttribiv       = (pfnGetVertexAttribiv)GlProc("glGetVertexAttribivARB");
        if (!p_GetVertexAttribiv)
            p_GetVertexAttribiv   = (pfnGetVertexAttribiv)GlProc("glGetVertexAttribiv");
        p_GetVertexAttribPointerv = (pfnGetVertexAttribPointerv)GlProc("glGetVertexAttribPointervARB");
        if (!p_GetVertexAttribPointerv)
            p_GetVertexAttribPointerv = (pfnGetVertexAttribPointerv)GlProc("glGetVertexAttribPointerv");
        p_GetProgLocal = (pfnGetProgramLocalParameterfv)GlProc("glGetProgramLocalParameterfvARB");
        p_GetBufferParameteriv = (pfnGetBufferParameteriv)GlProc("glGetBufferParameteriv");
        p_GetProgramivARB = (pfnGetProgramivARB)GlProc("glGetProgramivARB");
        p_BindProgramARBGeo = (pfnBindProgramARBGeo)GlProc("glBindProgramARB");
        p_GetProgramString = (pfnGetProgramStringARB)GlProc("glGetProgramStringARB");
        p_GetVertexAttribfv = (pfnGetVertexAttribfv)GlProc("glGetVertexAttribfvARB");
        if (!p_GetVertexAttribfv)
            p_GetVertexAttribfv = (pfnGetVertexAttribfv)GlProc("glGetVertexAttribfv");
    }
    g_nLines = 0; g_got = 0; g_want = nDraws;
    g_frameTris = 0; g_frameDraws = 0;
    g_vboDraws = 0; g_clientDraws = 0;
    g_onScreen = 0; g_offScreen = 0;
    g_nVerts = 0;
    for (int i = 0; i < 3; i++) { g_lo[i] = 1e30f; g_hi[i] = -1e30f; }
    g_armed = 1;
}

// Pull `bytes` from a buffer object, or from client memory when id == 0.
static int ReadFrom(GLenum target, GLuint id, const void* basePtr,
                    GLintptr_ offset, GLsizeiptr_ bytes, void* out) {
    if (bytes <= 0) return 0;
    if (id != 0) {
        if (!p_BindBuffer || !p_GetBufferSubData) return 0;
        GLint prev = 0;
        glGetIntegerv(target == GL_ARRAY_BUFFER_ ? GL_ARRAY_BUFFER_BINDING_
                                                 : GL_ELEM_ARRAY_BINDING_, &prev);
        p_BindBuffer(target, id);
        while (glGetError() != GL_NO_ERROR) { }        // clear stale errors
        p_GetBufferSubData(target, offset, bytes, out);
        // A failed glGetBufferSubData writes NOTHING and leaves the caller's
        // buffer holding the previous read - which is exactly the symptom:
        // byte-identical "vertex data" across three different VBOs.
        GLenum e = glGetError();
        p_BindBuffer(target, (GLuint)prev);
        if (e != GL_NO_ERROR) {
            g_readErrors++;
            g_lastReadError = (unsigned)e;
            // How far past the end are we? GL_INVALID_VALUE from
            // glGetBufferSubData means offset+size exceeds the buffer.
            if (g_readErrLogged < 5 && p_GetBufferParameteriv) {
                g_readErrLogged++;
                GLint bufSize = -1;
                p_BindBuffer(target, id);
                p_GetBufferParameteriv(target, 0x8764 /*GL_BUFFER_SIZE*/, &bufSize);
                p_BindBuffer(target, (GLuint)prev);
                wsprintfA(g_tmp, "readfail: %s buf %u  want offset %d + %d bytes  BUFFER IS %d bytes",
                          (target == GL_ARRAY_BUFFER_) ? "vertex" : "index",
                          id, (int)offset, (int)bytes, bufSize);
                Line(g_tmp);
            }
            return 0;
        }
        return 1;
    }
    // client-side array: the game handed the driver a live CPU pointer
    if (!basePtr) return 0;
    memcpy(out, (const unsigned char*)basePtr + offset, (size_t)bytes);
    return 1;
}

void SWSE_GeoCaptureDraw(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    if (!g_armed) return;
    if (mode != GL_TRIANGLES_ && mode != GL_TRIANGLE_STRIP_ && mode != GL_TRIANGLE_FAN_) return;
    if (count < 3) return;
    if (!p_GetVertexAttribiv || !p_GetVertexAttribPointerv) return;

    // frame census, every world draw
    g_frameDraws++;
    g_frameTris += (mode == GL_TRIANGLES_) ? (count / 3) : (count - 2);

    // Stage 1: record state only. Reading buffers here would stall the
    // pipeline once per draw; the bulk read happens at frame end instead.
    if (SWSE_GeoHarvestBusy()) HarvestRecord(mode, count, type, indices);

    // attribute 0 = position (measured: the engine binds only generic attribs)
    GLint enabled = 0, size = 0, stride = 0, atype = 0, vbo = 0;
    p_GetVertexAttribiv(0, GL_VAA_ENABLED, &enabled);
    if (!enabled) return;
    p_GetVertexAttribiv(0, GL_VAA_SIZE, &size);
    p_GetVertexAttribiv(0, GL_VAA_STRIDE, &stride);
    p_GetVertexAttribiv(0, GL_VAA_TYPE, &atype);
    p_GetVertexAttribiv(0, GL_VAA_BUFFER_BINDING, &vbo);
    void* attrPtr = 0;
    p_GetVertexAttribPointerv(0, GL_VAA_POINTER, &attrPtr);
    if (vbo) g_vboDraws++; else g_clientDraws++;

    if (g_got >= g_want) return;          // census continues; detail capture done

    GLint ebo = 0;
    glGetIntegerv(GL_ELEM_ARRAY_BINDING_, &ebo);
    if (atype != GL_FLOAT) return;        // positions should be float3
    if (stride == 0) stride = size * 4;

    // --- read the first few indices ---
    unsigned idx[6] = { 0, 0, 0, 0, 0, 0 };
    int nIdx = (count < 6) ? count : 6;
    int idxBytes = (type == GL_UNSIGNED_INT) ? 4 : (type == GL_UNSIGNED_SHORT) ? 2 : 1;
    unsigned char raw[24];
    if (!ReadFrom(GL_ELEMENT_ARRAY_BUFFER_, (GLuint)ebo, indices, 0,
                  (GLsizeiptr_)(nIdx * idxBytes), raw)) return;
    for (int i = 0; i < nIdx; i++) {
        if (idxBytes == 4)      idx[i] = ((unsigned*)raw)[i];
        else if (idxBytes == 2) idx[i] = ((unsigned short*)raw)[i];
        else                    idx[i] = raw[i];
    }

    // --- read the vertices those indices point at ---
    float v[3][3];
    int okAll = 1;
    for (int i = 0; i < 3; i++) {
        float tmp[4] = { 0, 0, 0, 0 };
        GLintptr_ off = (GLintptr_)((size_t)attrPtr) + (GLintptr_)idx[i] * stride;
        GLintptr_ base = vbo ? off : 0;
        const void* cbase = vbo ? 0 : (const void*)((const unsigned char*)attrPtr + (size_t)idx[i] * stride);
        if (!ReadFrom(GL_ARRAY_BUFFER_, (GLuint)vbo, cbase, vbo ? base : 0,
                      (GLsizeiptr_)(3 * sizeof(float)), tmp)) { okAll = 0; break; }
        v[i][0] = tmp[0]; v[i][1] = tmp[1]; v[i][2] = tmp[2];
        for (int k = 0; k < 3; k++) {
            if (tmp[k] < g_lo[k]) g_lo[k] = tmp[k];
            if (tmp[k] > g_hi[k]) g_hi[k] = tmp[k];
        }
        g_nVerts++;
    }
    if (!okAll) return;

    g_got++;
    wsprintfA(g_tmp, "geo: draw %d - mode 0x%X, %d indices (%d-bit), tris %d",
              g_got, mode, count, idxBytes * 8,
              (mode == GL_TRIANGLES_) ? count / 3 : count - 2);
    Line(g_tmp);
    wsprintfA(g_tmp, "geo:   vertex source: %s%d, stride %d, size %d, float",
              vbo ? "VBO " : "client ptr ", vbo ? vbo : (int)(size_t)attrPtr, stride, size);
    Line(g_tmp);
    wsprintfA(g_tmp, "geo:   index source : %s", ebo ? "EBO" : "client ptr");
    Line(g_tmp);

    // --- the vertex format, attribute by attribute ---
    // Settles what a tracer can consume per vertex. Tonight's G-buffer found
    // attrib 2 (the normal slot) empty; this reports whether it is even bound.
    {
        char fmt[LINE_LEN]; fmt[0] = 0;
        for (GLuint a = 0; a < 8; a++) {
            GLint en = 0, sz = 0, ty = 0, st = 0;
            p_GetVertexAttribiv(a, GL_VAA_ENABLED, &en);
            if (!en) continue;
            p_GetVertexAttribiv(a, GL_VAA_SIZE, &sz);
            p_GetVertexAttribiv(a, GL_VAA_TYPE, &ty);
            p_GetVertexAttribiv(a, GL_VAA_STRIDE, &st);
            char one[48];
            wsprintfA(one, " a%d[%dx0x%X st%d]", a, sz, ty, st);
            lstrcatA(fmt, one);
        }
        wsprintfA(g_tmp, "geo:   attribs:%s", fmt);
        Line(g_tmp);
    }

    // --- the proof: transform by the matrix THIS DRAW is using ------------
    // Preferred source is the bound program's local[1..4] - the rows the
    // engine's own vertex program dots with the position, and the exact
    // convention our G-buffer program rasterized correctly tonight. The
    // wind system's cached camera is the fallback (it needs foliage on
    // screen first, which is why the first run printed no proof lines).
    float m[16];
    int haveM = 0;
    if (p_GetProgLocal) {
        for (int r = 0; r < 4; r++) p_GetProgLocal(0x8620, (GLuint)(r + 1), &m[r * 4]);
        float sum = 0.0f;
        for (int i = 0; i < 16; i++) sum += (m[i] < 0 ? -m[i] : m[i]);
        haveM = (sum > 0.001f);
        if (haveM) Line("geo:   matrix source: this draw's program.local[1..4]");
    }
    if (!haveM && SWSE_WindClipVPLast(m)) {
        haveM = 1;
        Line("geo:   matrix source: wind system's cached camera");
    }
    if (haveM) {
        for (int i = 0; i < 3; i++) {
            float x = v[i][0], y = v[i][1], z = v[i][2];
            float cx = m[0]*x + m[1]*y + m[2]*z  + m[3];
            float cy = m[4]*x + m[5]*y + m[6]*z  + m[7];
            float cw = m[12]*x + m[13]*y + m[14]*z + m[15];
            char sx[24], sy[24], sz[24], nx[24], ny[24];
            F3(sx, x); F3(sy, y); F3(sz, z);
            if (cw > 0.0001f) {
                float ndx = cx / cw, ndy = -cy / cw;   // engine negates clip y
                F3(nx, ndx); F3(ny, ndy);
                int on = (ndx > -1.2f && ndx < 1.2f && ndy > -1.2f && ndy < 1.2f);
                if (on) g_onScreen++; else g_offScreen++;
                wsprintfA(g_tmp, "geo:   v%d world(%s, %s, %s) -> screen(%s, %s) %s",
                          i, sx, sy, sz, nx, ny, on ? "ON-SCREEN" : "off");
            } else {
                g_offScreen++;
                wsprintfA(g_tmp, "geo:   v%d world(%s, %s, %s) -> behind camera", i, sx, sy, sz);
            }
            Line(g_tmp);
        }
    }
}

void SWSE_GeoFrameEnd() {
    // CONTINUOUS MODE. A one-shot harvest cannot serve a game you walk around
    // in: the BVH only ever holds what was on screen during those frames, so
    // everywhere else has no geometry to occlude against. Staying armed is
    // affordable because the dedup check returns BEFORE the GPU readback - a
    // mesh already collected costs one hash and a table lookup, and the
    // expensive glGetBufferSubData only happens the first time a mesh is seen.
    // Measured on a normal harvest: 63576 dup skips against 448 unique meshes.
    if (HarvestContinuousTick()) { g_armed = 1; return; }
    if (!g_armed) return;
    g_armed = 0;                       // one frame only
    if (SWSE_GeoHarvestBusy()) {
        // No camera means every position stayed object-local and the whole
        // harvest is worthless (measured: the BVH came out as one 56-unit box
        // at the origin). The wind system captures the camera from foliage
        // draws, so right after a level load it is not ready at arm time -
        // retry on later frames rather than silently banking bad data.
        if (HarvestRetryIfNoCamera()) { g_armed = 1; return; }
        // Keep collecting across frames; dedup means each mesh is taken once.
        if (HarvestMoreFrames()) { GeoTrace("trace: frame done, more to go"); g_armed = 1; return; }
        GeoTrace("trace: last frame done, entering HarvestRun");
        HarvestRun();                      // clears the armed flag itself
        GeoTrace("trace: HarvestRun returned OK");
    }
    Line("geo: ---- frame census ----");
    wsprintfA(g_tmp, "geo: world draws %d, triangles %d", g_frameDraws, g_frameTris);
    Line(g_tmp);
    wsprintfA(g_tmp, "geo: vertex data in VBOs: %d draws, client memory: %d draws",
              g_vboDraws, g_clientDraws);
    Line(g_tmp);
    int total = g_onScreen + g_offScreen;
    if (total > 0) {
        wsprintfA(g_tmp, "geo: projection check (locals 1..4 assumed): %d of %d on screen",
                  g_onScreen, total);
        Line(g_tmp);
    }
    // The verdict rests on coordinate spread, not on the projection above:
    // which local holds the camera varies per program, so a failed projection
    // proves nothing about the vertex data itself.
    if (g_nVerts > 0) {
        char a[24], b[24], c[24], d[24], e[24], f[24];
        F3(a, g_lo[0]); F3(b, g_hi[0]);
        F3(c, g_lo[1]); F3(d, g_hi[1]);
        F3(e, g_lo[2]); F3(f, g_hi[2]);
        wsprintfA(g_tmp, "geo: captured vertex range x[%s..%s] y[%s..%s] z[%s..%s]",
                  a, b, c, d, e, f);
        Line(g_tmp);
        float spread = (g_hi[0] - g_lo[0]);
        if ((g_hi[1] - g_lo[1]) > spread) spread = (g_hi[1] - g_lo[1]);
        char s[24]; F3(s, spread);
        wsprintfA(g_tmp, "geo: widest spread across draws: %s units", s);
        Line(g_tmp);
        if (spread > 20.0f) {
            Line("geo: VERDICT - WORLD SPACE. Vertices from different draws sit far");
            Line("geo: apart, at level coordinates (compare the 'pos' command).");
            Line("geo: A tracer can consume the draw stream directly - no model matrices.");
        } else {
            Line("geo: VERDICT - vertices cluster near an origin: object space, and a");
            Line("geo: per-draw model matrix would have to be recovered first.");
        }
    }
}

// ============================================================================
// STAGE 1: geometry harvest
// ============================================================================
// One frame's world draws are recorded as they happen (cheap - just state),
// then read back in bulk at frame end. Reading during the draw would stall the
// pipeline 800 times; doing it once at the end costs a single hitchy frame,
// and the result is static for the whole level.

typedef struct {
    GLuint   vbo, ebo;
    unsigned idxOffset;      // byte offset into the EBO (or client pointer)
    const void* idxPtr;      // client pointer when ebo == 0
    const void* attrPtr;     // attrib-0 base (offset when vbo != 0)
    int      count;
    GLenum   type;           // index type
    GLenum   mode;           // GL_TRIANGLES / STRIP / FAN
    int      stride;
    GLenum   atype;          // attrib-0 component type (FLOAT or SHORT)
    int      asize;          // components per position
    int      normalized;
    int      hasUnpack;      // scale/bias present
    float    scale[4], bias[4];
    int      hasModel;
    float    model[4][4];    // rows dotted with the unpacked position
    int      hasMvp;
    int      vpId;           // the program that drew it
    float    mvp[16];        // this draw's matrix, in true clip convention
    int      mtxBase[4];     // WHICH locals hold it - per program, not assumed
    int      mtxYNeg;        // does this program negate result.position.y
} DrawRec;

#define MAX_RECS  8192
#define MAX_TRIS  1200000        // 1.2M tris * 36 bytes = 43 MB
static DrawRec g_recs[MAX_RECS];
static int     g_nRecs = 0;
static int     g_recsDropped = 0;

static float*  g_tris = 0;       // 9 floats per triangle
// 1 = from a packed (16-bit) draw, i.e. real world geometry; 0 = from a float
// draw, which measurement showed are sky/water/fog billboards. The tracer must
// exclude those: they are camera-attached planes and every ray hits them at
// distance zero (measured - the first traced frame came back solid white).
static unsigned char* g_triPacked = 0;
static int     g_curPacked = 0;
static int     g_curDepthWrite = 1;      // this draw writes depth = solid world
static unsigned char* g_triWorld = 0;    // per triangle, same indexing as g_tris
static int     g_curAlphaMask = 0;       // alpha-tested/blended = not solid
static int     g_excludeFoliage = 0;     // see the note at the use site
static int     g_continuous = 0;         // keep harvesting as the player moves
static unsigned g_curMeshKey = 0;        // pending until the mesh converts
static int     g_worldProg[64], g_nWorldProg = 0;   // programs drawing the world
static int     g_bigProg[32], g_bigCount[32], g_nBigProg = 0;  // big-card census
static int     g_curFoliageProg = 0;   // this draw uses a wind-injected program
static int     g_curTriCard = 0;       // this triangle is a leaf card
static int     g_cardTris = 0;
static float   g_curTriArea = 0.0f;
static unsigned char* g_triFol = 0;   // per triangle: drawn by a foliage program
static float*  g_triArea = 0;         // per triangle: world-space area
static int     g_depthWriteDraws = 0, g_overlayDraws = 0, g_alphaDraws = 0;
static int     g_nTris = 0;
static int     g_harvestArmed = 0;
static int     g_harvestDone = 0;
static int     g_harvestRetries = 0;
// Harvesting one frame collected wildly different amounts run to run (172k
// then 53k triangles from the same spot) because a frame only draws what it
// draws. So accumulate over several frames and skip meshes already taken -
// a mesh is identified by the buffers and index range the draw references.
static int     g_harvestFrames = 0;
static int     g_harvestMulti = 0;
#define SEEN_CAP 32768
static unsigned      g_seenKey[SEEN_CAP];
static unsigned char g_seenUsed[SEEN_CAP];
static int     g_dupSkipped = 0;

// SPLIT from the old combined test-and-insert. A mesh must only be marked seen
// once it has actually CONVERTED. In continuous mode the first seconds run
// before the wind system has published a camera (measured: 55750 verts
// rejected for having none) and before it has discovered all the foliage
// programs (2 known at boot versus 11 warm). Inserting on the test meant those
// meshes were banked as "collected" while contributing nothing, and were never
// looked at again - a permanent hole in the world for anything drawn early.
static int SeenHas(unsigned key) {          // 1 = already collected
    unsigned h = key & (SEEN_CAP - 1);
    for (int probe = 0; probe < 64; probe++) {
        unsigned i = (h + (unsigned)probe) & (SEEN_CAP - 1);
        if (!g_seenUsed[i]) return 0;
        if (g_seenKey[i] == key) return 1;
    }
    return 0;
}
static void SeenMark(unsigned key) {
    unsigned h = key & (SEEN_CAP - 1);
    for (int probe = 0; probe < 64; probe++) {
        unsigned i = (h + (unsigned)probe) & (SEEN_CAP - 1);
        if (!g_seenUsed[i]) { g_seenUsed[i] = 1; g_seenKey[i] = key; return; }
        if (g_seenKey[i] == key) return;
    }
    // Table saturated: nothing to do. The caller still emitted the geometry,
    // so the cost is re-reading it, not losing it.
}
static int     g_hDraws = 0, g_hDegenerate = 0, g_hFailed = 0;
// Why draws get skipped, and what attribute layouts exist. The first run
// recorded 322 of ~800 world draws and produced impossible bounds, so both
// questions need instruments rather than theories.
static int     g_skipNoAttr0 = 0, g_skipNotFloat = 0, g_outOfRange = 0;
static unsigned g_typeVal[8]; static int g_typeCount[8]; static int g_nTypes = 0;
static int g_noDecode = 0, g_packed = 0, g_modelled = 0, g_zeroMvp = 0;
static int g_unpackLogged = 0, g_rawLogged = 0;
static int g_collapsedDraws = 0, g_goodDraws = 0, g_collapseLogged = 0, g_goodLogged = 0;
static int g_slivers = 0, g_eyeFans = 0, g_giants = 0, g_shadowPass = 0, g_traceLogged = 0;
static int g_collFloat = 0, g_collPacked = 0, g_collNoUnpack = 0, g_collNoMvp = 0;
static float g_harvestEye[3] = { 0, 0, 0 };
static int   g_haveHarvestEye = 0;
static unsigned g_mtxHash[32]; static int g_mtxCount[32];
static int g_nMtx = 0, g_mtxOverflow = 0;
static unsigned g_idxBuf[65536];
static float    g_vtxBuf[65536 * 3];
static unsigned g_layouts[16];      // enabled-attrib bitmask -> count
static int     g_nLayouts = 0;
static unsigned g_layoutMask[16];
static double  g_hMs = 0.0;
static float   g_hLo[3], g_hHi[3];
static LARGE_INTEGER g_hStart;

static void ClipRows(const float* rows, float* out16, int yNeg);
static int  Invert4x4(const float* m, float* inv);
static float g_invVP[16];
static int   g_haveInvVP = 0;
static int   g_camFrames = 0;    // harvest frames that published a fresh camera
static int   g_noCamVerts = 0;   // vertices rejected for having no camera at all
static int   g_preRejected = 0;  // tris carrying a vertex already marked 1e30
static int   g_clipWRej = 0;     // verts at/behind the camera plane (clip w ~ 0)
static int   g_oorLogged = 0;    // sample of genuinely wild coordinates logged
static int   g_curVpId = 0;      // program of the draw currently being converted
static int   g_collProgLogged = 0;  // sample of collapsing programs named

int SWSE_GeoInvert4x4(const float* m, float* out) { return Invert4x4(m, out); }
const unsigned char* SWSE_GeoHarvestPackedFlags() { return g_triPacked; }
const unsigned char* SWSE_GeoHarvestWorldFlags()  { return g_triWorld; }
void SWSE_GeoExcludeFoliage(int on) { g_excludeFoliage = on ? 1 : 0; }
static int HarvestContinuousTick(void) {
    if (!g_continuous) return 0;
    RefreshCamera();              // world recovery needs THIS frame's camera
    g_harvestArmed = 1;
    g_harvestDone  = 1;           // the soup is always usable
    g_harvestMulti = 1;           // dedup across frames, never within one
    return 1;
}

int  SWSE_GeoHarvestMeshCount() { return g_nRecs; }

// Every distinct vertex program seen drawing world geometry. The camera
// consensus was drawing candidates only from wind-saved and foliage-tracker
// programs, which in a sparse area was TWO - and measured, neither was the
// scene camera ("tilt: 0 of 2 candidates agree"). This is the full set the
// engine actually renders the world with, which is where the real camera is.
int SWSE_GeoWorldPrograms(unsigned* out, int maxOut) {
    int n = (g_nWorldProg < maxOut) ? g_nWorldProg : maxOut;
    for (int i = 0; i < n; i++) out[i] = (unsigned)g_worldProg[i];
    return n;
}
void SWSE_GeoHarvestContinuous(int on) {
    g_continuous = on ? 1 : 0;
    if (!g_continuous) return;
    // Start from whatever is already collected rather than clearing: an
    // existing harvest is still valid geometry, and re-reading it would cost
    // the expensive first-sight readback all over again.
    if (!g_tris)      g_tris      = (float*)malloc((size_t)MAX_TRIS * 9 * sizeof(float));
    if (!g_triPacked) g_triPacked = (unsigned char*)malloc((size_t)MAX_TRIS);
    if (!g_triWorld)  g_triWorld  = (unsigned char*)malloc((size_t)MAX_TRIS);
    if (!g_triFol)    g_triFol    = (unsigned char*)malloc((size_t)MAX_TRIS);
    if (!g_triArea)   g_triArea   = (float*)malloc((size_t)MAX_TRIS * sizeof(float));
    g_harvestMulti = 1;
    g_harvestArmed = 1;
    g_harvestDone  = 1;
    g_armed        = 1;
    SWSE_GeoArm(0);               // resolve entry points if not already done
    g_harvestArmed = 1;           // SWSE_GeoArm does not set this
}
const unsigned char* SWSE_GeoHarvestFoliageFlags() { return g_triFol; }
const float*         SWSE_GeoHarvestAreas()        { return g_triArea; }

int SWSE_GeoHarvestBusy() { return g_harvestArmed; }
const float* SWSE_GeoHarvestTris(int* nTris) {
    if (nTris) *nTris = g_nTris;
    return g_tris;
}

// The camera, inverted: it turns every draw's MVP back into a world
// transform. Without it the packed geometry stays object-local.
// clip (x,y,z,1) -> world using the cached inverse camera.
static void UnprojInv(float x, float y, float z, float* out) {
    const float* m = g_invVP;
    float w = m[12]*x + m[13]*y + m[14]*z + m[15];
    if (w > -1e-9f && w < 1e-9f) w = 1e-9f;
    out[0] = (m[0]*x + m[1]*y + m[2]*z  + m[3])  / w;
    out[1] = (m[4]*x + m[5]*y + m[6]*z  + m[7])  / w;
    out[2] = (m[8]*x + m[9]*y + m[10]*z + m[11]) / w;
}

static void RefreshCamera(void) {
    // Do NOT clear a good camera here. The wind system only publishes a VP on
    // frames where foliage actually drew, so a mid-harvest frame can come back
    // empty while the view has barely moved. Clearing on those frames sent the
    // whole frame down the `else` branch in HarvestOne, which leaves w0..w2 as
    // RAW OBJECT-LOCAL coordinates and writes them into the soup as if they
    // were world space. Keeping the previous frame's camera is far closer to
    // the truth than emitting object-local debris.
    // ASK for the camera; do not just read the cache. SWSE_WindClipVPLast only
    // returns the matrix that the LAST call to SWSE_WindClipCamera published,
    // and nothing on the harvest path was calling it - so a fresh session had
    // no camera until some unrelated command (`proj`) happened to derive one.
    // Measured: harvest right after a load reported "NO CAMERA, 89629 verts
    // rejected"; running `proj` first and re-harvesting gave "camera OK on 8
    // frame(s)" in the same spot with the same view.
    float vp[16], clip[16];
    { float n, f, fv, asp; unsigned pr; int cv, gp;
      SWSE_WindClipCamera(&n, &f, &fv, &asp, &pr, &cv, &gp); }
    if (SWSE_WindClipVPLast(vp)) {
        ClipRows(vp, clip, 1);   // the foliage family this came from negates y
        float inv[16];
        if (Invert4x4(clip, inv)) {
            for (int i = 0; i < 16; i++) g_invVP[i] = inv[i];
            g_haveInvVP = 1;
            g_camFrames++;
        }
    }
    g_haveHarvestEye = 0;
    if (!g_haveInvVP) return;
    // The eye, by intersecting two unprojected screen rays - the same method
    // the tracer uses. Needed here to recognise fan-from-eye decode errors.
    float A[3], B[3], C[3], D[3];
    UnprojInv(-0.5f, -0.5f, 0.0f, A);
    UnprojInv(-0.5f, -0.5f, 0.8f, B);
    UnprojInv( 0.5f,  0.5f, 0.0f, C);
    UnprojInv( 0.5f,  0.5f, 0.8f, D);
    float d1[3], d2[3], r[3];
    for (int i = 0; i < 3; i++) { d1[i]=B[i]-A[i]; d2[i]=D[i]-C[i]; r[i]=A[i]-C[i]; }
    float aa = d1[0]*d1[0]+d1[1]*d1[1]+d1[2]*d1[2];
    float bb = d1[0]*d2[0]+d1[1]*d2[1]+d1[2]*d2[2];
    float cc = d2[0]*d2[0]+d2[1]*d2[1]+d2[2]*d2[2];
    float dd = d1[0]*r[0] +d1[1]*r[1] +d1[2]*r[2];
    float ee = d2[0]*r[0] +d2[1]*r[1] +d2[2]*r[2];
    float den = aa*cc - bb*bb;
    if (den > -1e-9f && den < 1e-9f) return;
    float t = (bb*ee - cc*dd) / den, s = (aa*ee - bb*dd) / den;
    for (int i = 0; i < 3; i++)
        g_harvestEye[i] = 0.5f * ((A[i] + t*d1[i]) + (C[i] + s*d2[i]));
    g_haveHarvestEye = 1;
}

void SWSE_GeoHarvestArmRetries(int n) { g_harvestRetries = n; }
void SWSE_GeoHarvestFrames(int n) {
    g_harvestFrames = (n > 0) ? n : 1;
    g_harvestMulti  = (g_harvestFrames > 1) ? 1 : 0;
}

static int HarvestMoreFrames(void) {
    if (g_harvestFrames <= 1) return 0;
    g_harvestFrames--;
    RefreshCamera();        // the view moves between frames; each uses its own
    return 1;
}

static int HarvestRetryIfNoCamera(void) {
    if (g_haveInvVP || g_harvestRetries <= 0) return 0;
    g_harvestRetries--;
    int keep = g_harvestRetries;
    SWSE_GeoHarvestArm();          // re-arms and re-reads the camera
    g_harvestRetries = keep;       // arm resets nothing else we need
    return 1;
}

void SWSE_GeoHarvestArm() {
    GeoTrace("trace: ARM");
    SWSE_GeoArm(0);                 // reuses entry-point resolution + census
    g_nRecs = 0; g_recsDropped = 0;
    g_nTris = 0; g_hDraws = 0; g_hDegenerate = 0; g_hFailed = 0;
    g_skipNoAttr0 = 0; g_skipNotFloat = 0; g_outOfRange = 0;
    g_camFrames = 0; g_noCamVerts = 0;
    g_preRejected = 0; g_clipWRej = 0; g_oorLogged = 0; g_collProgLogged = 0;
    g_depthWriteDraws = 0; g_overlayDraws = 0; g_alphaDraws = 0;
    g_nBigProg = 0; g_cardTris = 0; g_curTriCard = 0;
    g_nLayouts = 0; g_nTypes = 0;
    g_noDecode = 0; g_packed = 0; g_modelled = 0; g_zeroMvp = 0; g_unpackLogged = 0; g_rawLogged = 0;
    g_collapsedDraws = 0; g_goodDraws = 0; g_collapseLogged = 0; g_goodLogged = 0;
    g_eyeFans = 0; g_giants = 0; g_shadowPass = 0; g_traceLogged = 0; g_collFloat = 0; g_collPacked = 0; g_collNoUnpack = 0; g_collNoMvp = 0;
    g_readErrors = 0; g_lastReadError = 0; g_readErrLogged = 0; g_slivers = 0;
    g_nMtx = 0; g_mtxOverflow = 0;
    for (int i = 0; i < 3; i++) { g_hLo[i] = 1e30f; g_hHi[i] = -1e30f; }
    RefreshCamera();
    g_dupSkipped = 0;
    for (int i = 0; i < SEEN_CAP; i++) g_seenUsed[i] = 0;
    QueryPerformanceCounter(&g_hStart);
    for (int i = 0; i < 16; i++) { g_layouts[i] = 0; g_layoutMask[i] = 0; }
    g_harvestDone = 0;
    g_harvestArmed = 1;
    if (!g_tris) g_tris = (float*)malloc((size_t)MAX_TRIS * 9 * sizeof(float));
    if (!g_triPacked) g_triPacked = (unsigned char*)malloc((size_t)MAX_TRIS);
    if (!g_triWorld)  g_triWorld  = (unsigned char*)malloc((size_t)MAX_TRIS);
    if (!g_triFol)    g_triFol    = (unsigned char*)malloc((size_t)MAX_TRIS);
    if (!g_triArea)   g_triArea   = (float*)malloc((size_t)MAX_TRIS * sizeof(float));
}

// ---- per-program position decode -------------------------------------------
// The engine packs positions and unpacks them differently per program family,
// so the decode is READ FROM THE PROGRAM'S OWN INSTRUCTIONS rather than
// assumed. Verbatim from a live program:
//     MUL R0, vertex.attrib[0], c[5];    <- unpack scale, local 5
//     ADD R1, R0, c[8];                  <- unpack bias,  local 8
//     DP4 R0.x, R1, vertex.attrib[1];    <- model matrix rows, carried as
//     DP4 R0.y, R1, vertex.attrib[10];      generic attributes (usually
//     DP4 R0.z, R1, vertex.attrib[11];      DISABLED arrays, i.e. constants
//     DP4 R0.w, R1, vertex.attrib[12];      set per draw)
//     DP4 result.position.x, R0, c[1];   <- then the camera
// Everything on the right of those arrows is queryable at draw time.
typedef struct {
    int parsed;
    int scaleLocal;        // -1 = positions need no unpacking
    int biasLocal;         // -1 = no bias
    int modelAttrib[4];    // rows x,y,z,w; -1 = no model transform
    // WHICH locals hold the position matrix. NOT always c[1..4]: program 116
    // uses c[2..5], and reading a fixed location shifted every row by one and
    // fed an unrelated constant in as the x row. -1 = not found, fall back.
    int mtxLocal[4];       // rows x,y,z,w
    int mtxYNeg;           // 1 = program writes result.position.y negated
} ProgDecode;
static ProgDecode g_decode[65536];
static char g_src[16384];

static void FirstDst(const char* line, char* out, int cap) {
    const char* p = line;
    while (*p == ' ' || *p == '\t') p++;
    while (*p && *p != ' ') p++;           // skip the opcode
    while (*p == ' ') p++;
    int i = 0;
    while (*p && *p != ',' && *p != '.' && *p != ' ' && i < cap - 1) out[i++] = *p++;
    out[i] = 0;
}

// Index inside the first "c[N]" appearing after `after`, or -1.
static int LocalAfter(const char* line, const char* after) {
    const char* p = strstr(line, after);
    if (!p) return -1;
    p = strstr(p, "c[");
    if (!p) return -1;
    return atoi(p + 2);
}

static void ParseDecode(int progId) {
    ProgDecode* d = &g_decode[progId];
    d->parsed = 1;
    d->scaleLocal = -1; d->biasLocal = -1;
    d->mtxYNeg = 0;
    for (int i = 0; i < 4; i++) { d->modelAttrib[i] = -1; d->mtxLocal[i] = -1; }
    if (!p_GetProgramivARB || !p_GetProgramString) return;

    GLint len = 0;
    p_GetProgramivARB(0x8620, 0x8627 /*PROGRAM_LENGTH_ARB*/, &len);
    if (len <= 0 || len >= (GLint)sizeof(g_src)) return;
    p_GetProgramString(0x8620, 0x8628 /*PROGRAM_STRING_ARB*/, g_src);
    g_src[len] = 0;

    char scaleReg[16] = { 0 }, posReg[16] = { 0 }, yTmp[16] = { 0 };
    // Temps written by a DP4 against a program local, in source order, so a
    // `MOV result.position.y, -Rn.x` can be resolved backwards to its row.
    char tmpReg[16][16]; int tmpLocal[16]; int nTmp = 0;
    char posSrcReg[32] = { 0 };   // what the result.position rows dot against
    char line[256];
    const char* p = g_src;
    while (*p) {
        int n = 0;
        while (*p && *p != '\n' && n < (int)sizeof(line) - 1) line[n++] = *p++;
        line[n] = 0;
        if (*p) p++;
        if (n == 0 || line[0] == '#') continue;

        if (!scaleReg[0] && !strncmp(line, "MUL", 3) && strstr(line, "vertex.attrib[0]")) {
            int L = LocalAfter(line, "vertex.attrib[0]");
            if (L >= 0) {
                d->scaleLocal = L;
                FirstDst(line, scaleReg, sizeof(scaleReg));
                lstrcpynA(posReg, scaleReg, sizeof(posReg));
            }
            continue;
        }
        if (scaleReg[0] && d->biasLocal < 0 && !strncmp(line, "ADD", 3) && strstr(line, scaleReg)) {
            int L = LocalAfter(line, scaleReg);
            if (L >= 0) {
                d->biasLocal = L;
                FirstDst(line, posReg, sizeof(posReg));
            }
            continue;
        }
        // POSITION MATRIX ROWS, read from the program rather than assumed.
        //   DP4 result.position.<c>, <reg>, c[N]      -> row <c> = N
        //   MOV result.position.y, -<tmp>.x           -> remember the temp,
        //   DP4 <tmp>.x, <reg>, c[M]                  -> row y = M
        if (!strncmp(line, "DP4", 3) && strstr(line, "result.position")) {
            const char* rp = strstr(line, "result.position.");
            const char* cp = strstr(line, "c[");
            if (rp && cp) {
                char comp = rp[16];
                int slot = (comp=='x')?0 : (comp=='y')?1 : (comp=='z')?2 : (comp=='w')?3 : -1;
                if (slot >= 0) d->mtxLocal[slot] = atoi(cp + 2);
            }
            // WHAT do the position rows actually read? A program can contain a
            // `MUL Rn, vertex.attrib[0], c[S]` that has nothing to do with
            // unpacking - program 115 has one feeding texcoords while its
            // position rows take vertex.attrib[0] RAW. Matching that MUL as an
            // unpack scaled every vertex by a texcoord matrix row and collapsed
            // the mesh. The source register settles it.
            if (!posSrcReg[0]) {
                const char* c1 = strchr(line, ',');
                if (c1) {
                    const char* q = c1 + 1;
                    while (*q == ' ') q++;
                    int i2 = 0;
                    while (*q && *q != ',' && *q != ' ' && *q != ';' && i2 < 31)
                        posSrcReg[i2++] = *q++;
                    posSrcReg[i2] = 0;
                }
            }
            continue;
        }
        if (!strncmp(line, "MOV", 3) && strstr(line, "result.position.y")) {
            // The y row is negated through a temp. Resolve it from the table
            // of temp writes seen SO FAR - program 151 emits
            //   DP4 R0.x, vertex.attrib[0], c[9];
            //   MOV result.position.y, -R0.x;
            // i.e. the DP4 comes BEFORE the MOV. Scanning forward only, as
            // this did at first, left the y row unresolved and it fell back
            // to c[2] while x/z/w correctly read c[8]/c[10]/c[11]. Three good
            // rows and one wrong one is a SHEAR: measured, world z climbed to
            // 4787 while x and y stayed sane, and 37614 triangles were thrown
            // out as "out of range" when the geometry was fine.
            const char* mn = strchr(line, '-');
            if (mn) {
                d->mtxYNeg = 1;
                int i = 0;
                const char* q = mn + 1;
                while (*q && *q != '.' && *q != ';' && *q != ' ' && i < 15) yTmp[i++] = *q++;
                yTmp[i] = 0;
                // LAST write wins. Registers are reused, so scanning forward
                // and taking the first match picks a stale value from earlier
                // in the program - measured: collapsed draws jumped from
                // 37/343 to 169/428 when this scanned forwards.
                if (yTmp[0] && d->mtxLocal[1] < 0) {
                    for (int i2 = nTmp - 1; i2 >= 0; i2--)
                        if (!lstrcmpA(tmpReg[i2], yTmp)) { d->mtxLocal[1] = tmpLocal[i2]; break; }
                }
            }
            continue;
        }
        // Any DP4 writing a temp from a constant: remember it, so a later MOV
        // into result.position can name it. Also covers the forward order.
        if (!strncmp(line, "DP4", 3) && strstr(line, "c[") && nTmp < 16 &&
            // Only DP4s that read the POSITION. A program dots plenty of other
            // things against constants (texgen, lighting, fog); matching those
            // would resolve the y row to an unrelated local.
            (strstr(line, "vertex.attrib[0]") || (posReg[0] && strstr(line, posReg)))) {
            char dst[16];
            FirstDst(line, dst, sizeof(dst));
            if (dst[0] == 'R' || dst[0] == 'r') {
                const char* cp = strstr(line, "c[");
                lstrcpynA(tmpReg[nTmp], dst, sizeof(tmpReg[0]));
                tmpLocal[nTmp++] = atoi(cp + 2);
                if (yTmp[0] && d->mtxLocal[1] < 0 && !lstrcmpA(dst, yTmp))
                    d->mtxLocal[1] = atoi(cp + 2);
            }
            continue;
        }

        // model rows: DP4 <dst>.<c>, <posReg>, vertex.attrib[K]
        if (!strncmp(line, "DP4", 3) && strstr(line, "vertex.attrib[") &&
            posReg[0] && strstr(line, posReg)) {
            const char* dot = strchr(line + 4, '.');
            const char* at = strstr(line, "vertex.attrib[");
            if (dot && at && dot < at) {
                char comp = dot[1];
                int K = atoi(at + 14);
                int slot = (comp == 'x') ? 0 : (comp == 'y') ? 1 : (comp == 'z') ? 2 : (comp == 'w') ? 3 : -1;
                if (slot >= 0 && K != 0) d->modelAttrib[slot] = K;
            }
        }
    }
    // If the position rows dot vertex.attrib[0] DIRECTLY, the program does no
    // unpacking, whatever MUL/ADD pairs appear elsewhere in it. Measured on
    // program 115: it has `MUL R0.y, vertex.attrib[0], c[11]` feeding TEXCOORDS
    // while its position rows read the raw attribute. Treating c[11] as an
    // unpack scale multiplied every position by a texcoord matrix row and
    // collapsed the mesh to a dot - it was the only program still collapsing.
    if (posSrcReg[0] && !lstrcmpA(posSrcReg, "vertex.attrib[0]")) {
        d->scaleLocal = -1;
        d->biasLocal  = -1;
    }
}

// ---- recovering world space from per-object matrices ------------------------
// Measured: c[1..4] differs per draw (32+ distinct matrices, most used once),
// so it is a model-view-projection, not a shared camera. Unpacked positions
// are therefore object-local. But clip = MVP * p_object and clip = VP *
// p_world describe the same point, so p_world = inv(VP) * MVP * p_object.
// VP comes from the wind system's consensus camera, which the temporal
// reprojection already validated.
//
// Clip convention, read straight off the shaders:
//   clip.x = c1 . p    clip.y = -(c2 . p)    clip.z = c3 . p    clip.w = c4 . p
// The engine's y flip, applied only when the program actually performs it.
// Every family seen so far writes `MOV result.position.y, -<tmp>.x`, but that
// is a fact we now read off each program rather than assume for all of them.
static void ClipRows(const float* rows, float* out16, int yNeg) {
    for (int i = 0; i < 16; i++) out16[i] = rows[i];
    if (yNeg) for (int i = 4; i < 8; i++) out16[i] = -out16[i];
}

static int Invert4x4(const float* m, float* inv) {
    float a[16];
    a[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    a[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    a[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    a[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    a[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    a[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    a[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    a[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    a[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
    a[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
    a[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
    a[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
    a[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
    a[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
    a[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
    a[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];
    float det = m[0]*a[0] + m[1]*a[4] + m[2]*a[8] + m[3]*a[12];
    if (det > -1e-12f && det < 1e-12f) return 0;
    det = 1.0f / det;
    for (int i = 0; i < 16; i++) inv[i] = a[i] * det;
    return 1;
}

static void HarvestOne(const DrawRec* r, unsigned* idxBuf, int idxCap,
                       float* vtxBuf, int vtxCap);

// Read a draw's geometry IMMEDIATELY, while it happens.
//
// The first version recorded state and read the buffers at frame end. That
// produced 5% usable triangles: the engine refills its vertex buffers between
// draws, so by frame end nearly every recorded handle pointed at whatever was
// written last. Reading in place stalls the pipeline once per draw, which for
// a one-shot harvest is a few hundred milliseconds - a fine price for data
// that is actually correct.
//
// CORRECTED 2026-09-28 (RT_1_3_PLAN section 7.5): the reason given above is
// wrong. GRAPHICS_RTGI.md records it as a corrected negative:
//   "Immediate in-draw reads produced byte-identical results".
// The bad data came from our own read overrunning each buffer, fixed by
//   span = (nv-1)*stride + attribute bytes.
// The engine does not refill these buffers. Each mesh has its own static VBO
// and EBO, uploaded when its data block becomes resident (0x64D7C0,
// 0x67C100). The in-place read is kept because it is the proven path, not
// because the buffers change.
static void HarvestRecord(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    if (g_nRecs >= MAX_RECS) { g_recsDropped++; return; }
    // SHADOW-MAP PASSES MUST NOT BE HARVESTED. The engine re-renders the same
    // objects from a LIGHT's viewpoint, so those draws carry the light's
    // matrix in c[1..4]. We transform everything by inv(cameraVP), so that
    // geometry lands somewhere arbitrary that moves with the camera - a slab
    // of ordinary triangles, invisible in the real frame, occluding
    // everything in the BVH. The owner spotted it: "nothing there in vanilla,
    // maybe linked to the third person camera".
    // Shadow maps render square; the scene does not.
    {
        GLint vp4[4] = { 0, 0, 0, 0 };
        glGetIntegerv(GL_VIEWPORT, vp4);
        int vw = vp4[2], vh = vp4[3];
        if (vw > 0 && vh > 0) {
            int d = vw - vh; if (d < 0) d = -d;
            if (d * 8 < vw) { g_shadowPass++; return; }   // near-square = shadow map
        }
    }

    GLint stride = 0, atype = 0, vbo = 0, enabled = 0, ebo = 0, asize = 0;
    // Record the attribute layout of every draw, so a skip is explained
    // rather than silent.
    unsigned mask = 0;
    for (GLuint a = 0; a < 16; a++) {
        GLint en = 0;
        p_GetVertexAttribiv(a, GL_VAA_ENABLED, &en);
        if (en) mask |= (1u << a);
    }
    int seen = 0;
    for (int i = 0; i < g_nLayouts; i++)
        if (g_layoutMask[i] == mask) { g_layouts[i]++; seen = 1; break; }
    if (!seen && g_nLayouts < 16) { g_layoutMask[g_nLayouts] = mask; g_layouts[g_nLayouts++] = 1; }

    p_GetVertexAttribiv(0, GL_VAA_ENABLED, &enabled);
    if (!enabled) { g_skipNoAttr0++; return; }
    p_GetVertexAttribiv(0, GL_VAA_TYPE, &atype);
    if (atype != GL_FLOAT && atype != 0x1402 /*GL_SHORT*/) {
        g_skipNotFloat++;
        for (int i = 0; i < g_nTypes; i++)
            if (g_typeVal[i] == (unsigned)atype) { g_typeCount[i]++; return; }
        if (g_nTypes < 8) { g_typeVal[g_nTypes] = (unsigned)atype; g_typeCount[g_nTypes++] = 1; }
        return;
    }
    p_GetVertexAttribiv(0, GL_VAA_SIZE, &asize);
    p_GetVertexAttribiv(0, GL_VAA_STRIDE, &stride);
    p_GetVertexAttribiv(0, GL_VAA_BUFFER_BINDING, &vbo);
    glGetIntegerv(GL_ELEM_ARRAY_BINDING_, &ebo);
    void* attrPtr = 0;
    p_GetVertexAttribPointerv(0, GL_VAA_POINTER, &attrPtr);
    // Tightly packed means size * component-size, NOT a hardcoded 12: a
    // float4 or short3 stream would be misread on every single vertex.
    int compBytes = (atype == GL_FLOAT) ? 4 : 2;
    if (stride == 0) stride = (asize > 0 ? asize : 3) * compBytes;

    DrawRec rec;
    rec.atype = (GLenum)atype;
    rec.asize = (asize > 0) ? asize : 3;
    rec.hasUnpack = 0; rec.hasModel = 0; rec.vpId = 0;
    {
        GLint norm = 0;
        p_GetVertexAttribiv(0, 0x886A /*ARRAY_NORMALIZED*/, &norm);
        rec.normalized = norm ? 1 : 0;
    }
    // Learn this program's unpacking, once, from its own instructions.
    GLint vpId = 0;
    // Defaults for programs we cannot look up: the historical c[1..4] guess and
    // the y flip every family seen so far performs.
    for (int r2 = 0; r2 < 4; r2++) rec.mtxBase[r2] = r2 + 1;
    rec.mtxYNeg = 1;
    if (p_GetProgramivARB) p_GetProgramivARB(0x8620, 0x8677 /*PROGRAM_BINDING*/, &vpId);
    if (vpId > 0 && vpId < 65536) {
        rec.vpId = (int)vpId;
        // Remember it as a world-drawing program: this is the pool the camera
        // consensus should be choosing from.
        {
            int seen = 0;
            for (int i = 0; i < g_nWorldProg; i++)
                if (g_worldProg[i] == (int)vpId) { seen = 1; break; }
            if (!seen && g_nWorldProg < 64) g_worldProg[g_nWorldProg++] = (int)vpId;
        }
        ProgDecode* d = &g_decode[vpId];
        if (!d->parsed) ParseDecode(vpId);
        // Which locals this program's matrix actually lives in. Program 116
        // uses c[2..5]; reading a fixed c[1..4] shifted every row by one and
        // fed an unrelated per-program constant in as the x row.
        for (int r2 = 0; r2 < 4; r2++)
            if (d->mtxLocal[r2] >= 0) rec.mtxBase[r2] = d->mtxLocal[r2];
        if (d->mtxLocal[0] >= 0) rec.mtxYNeg = d->mtxYNeg;
        if (d->scaleLocal >= 0 && p_GetProgLocal) {
            p_GetProgLocal(0x8620, (GLuint)d->scaleLocal, rec.scale);
            if (d->biasLocal >= 0) p_GetProgLocal(0x8620, (GLuint)d->biasLocal, rec.bias);
            else { rec.bias[0] = rec.bias[1] = rec.bias[2] = rec.bias[3] = 0.0f; }
            rec.hasUnpack = 1;
            // The suspect: a near-zero scale shrinks a whole mesh to a dot,
            // which the per-object matrix then places faithfully in the
            // world - reproducing the shell exactly. Log the first few so
            // the parsed local INDEX and its read-back VALUE can both be
            // checked against the program's source.
            if (g_unpackLogged < 6) {
                g_unpackLogged++;
                char s0[24], s1[24], s2[24], b0[24], b1[24], b2[24];
                F3(s0, rec.scale[0]); F3(s1, rec.scale[1]); F3(s2, rec.scale[2]);
                F3(b0, rec.bias[0]);  F3(b1, rec.bias[1]);  F3(b2, rec.bias[2]);
                wsprintfA(g_tmp, "unpack: prog %d  scale c[%d]=(%s,%s,%s)  bias c[%d]=(%s,%s,%s)",
                          (int)vpId, d->scaleLocal, s0, s1, s2,
                          d->biasLocal, b0, b1, b2);
                Line(g_tmp);
                wsprintfA(g_tmp, "unpack: prog %d  matrix rows x=c[%d] y=c[%d] z=c[%d] w=c[%d]%s",
                          (int)vpId, d->mtxLocal[0], d->mtxLocal[1],
                          d->mtxLocal[2], d->mtxLocal[3],
                          d->mtxYNeg ? "  (y negated)" : "");
                Line(g_tmp);
            }
        }
        if (d->modelAttrib[0] >= 0 && p_GetVertexAttribfv) {
            rec.hasModel = 1;
            for (int r2 = 0; r2 < 4; r2++) {
                int K = d->modelAttrib[r2];
                if (K < 0) { rec.model[r2][0] = rec.model[r2][1] = rec.model[r2][2] = 0.0f;
                             rec.model[r2][3] = (r2 == 3) ? 1.0f : 0.0f; continue; }
                // These rows are almost always DISABLED arrays, i.e. constants
                // set per draw - the engine's way of passing a matrix without
                // uniforms. GL_CURRENT_VERTEX_ATTRIB reads exactly that value.
                p_GetVertexAttribfv((GLuint)K, 0x8626 /*CURRENT_VERTEX_ATTRIB*/, rec.model[r2]);
            }
        }
    }
    rec.vbo = (GLuint)vbo;
    rec.ebo = (GLuint)ebo;
    rec.idxOffset = (unsigned)(size_t)indices;
    rec.idxPtr = indices;
    rec.attrPtr = attrPtr;
    rec.count = count;
    rec.type = type;
    rec.mode = mode;
    rec.stride = stride;

    // Already harvested this mesh on an earlier frame? Skip it, so several
    // frames accumulate into one complete world instead of overwriting.
    {
        // The vertex-pointer offset is part of a mesh's identity: this engine
        // packs many meshes into one buffer and distinguishes them by where
        // the attribute stream starts, not by index offset. Leaving it out
        // collapsed 884 draws/frame down to 339 "unique" meshes.
        unsigned key = 2166136261u;
        unsigned parts[6] = { (unsigned)vbo, (unsigned)ebo, (unsigned)(size_t)indices,
                              (unsigned)(size_t)attrPtr, (unsigned)count, (unsigned)mode };
        const unsigned char* pb = (const unsigned char*)parts;
        for (int i = 0; i < (int)sizeof(parts); i++) { key ^= pb[i]; key *= 16777619u; }
        // Only meaningful across frames; within a frame this wrongly merges
        // instances of the same mesh drawn at different world positions.
        if (g_harvestMulti && SeenHas(key)) { g_dupSkipped++; return; }
        g_curMeshKey = key;      // marked seen only after it converts
    }

    // Is c[1..4] one shared camera, or a per-object MVP? If the world were
    // pre-transformed, every draw would carry the SAME matrix. Counting
    // distinct values answers it without any theory.
    rec.hasMvp = 0;
    // NOT rec.vpId = 0 here: this block runs AFTER the decode block that sets
    // it, so zeroing here made every diagnostic print "prog 0" and left the
    // traces impossible to match against ofse_shaders.txt.
    if (p_GetProgLocal) {
        float m[16];
        // WHICH locals, read from the program - not assumed. Program 116 builds
        // result.position from c[2],c[4],c[5] (+ y through a temp), so reading a
        // fixed c[1..4] shifted every row by one and fed c[1] - an unrelated
        // per-program constant - in as the x row. That was the collapse, and it
        // is why row0's x,y came back IDENTICAL across two different draws of
        // the same program: a constant cannot vary per object.
        for (int r2 = 0; r2 < 4; r2++) p_GetProgLocal(0x8620, (GLuint)rec.mtxBase[r2], &m[r2 * 4]);
        // An all-zero matrix means this program keeps its transform somewhere
        // we still have not found. Multiplying by it sends every vertex to clip
        // origin, which unprojects to a point ON the camera - measured: 102,320
        // of 172,131 triangles collapsed to (134.3, -456.1, 40.6), median area
        // 0.000, and that debris is what every traced ray was hitting.
        float mag = 0.0f;
        for (int k = 0; k < 16; k++) mag += (m[k] < 0 ? -m[k] : m[k]);
        if (mag < 1e-6f) { g_zeroMvp++; return; }
        ClipRows(m, rec.mvp, rec.mtxYNeg);
        rec.hasMvp = 1;
        unsigned h = 2166136261u;
        const unsigned char* mb = (const unsigned char*)m;
        for (int i = 0; i < 64; i++) { h ^= mb[i]; h *= 16777619u; }
        int found = 0;
        for (int i = 0; i < g_nMtx; i++)
            if (g_mtxHash[i] == h) { g_mtxCount[i]++; found = 1; break; }
        if (!found && g_nMtx < 32) { g_mtxHash[g_nMtx] = h; g_mtxCount[g_nMtx++] = 1; }
        if (!found && g_nMtx >= 32) g_mtxOverflow++;
    }

    g_nRecs++;
    g_curPacked = (rec.atype == 0x1402) ? 1 : 0;
    // IS THIS DRAW WORLD GEOMETRY? Ask what the draw DOES, not how its
    // vertices happen to be packed. Solid world surfaces write depth; the
    // sky, fog and water overlay planes are blended passes that do not.
    // The packed/float split was only ever a proxy for "decodes correctly",
    // and using it as a world test either loses the terrain (`rt all 0`) or
    // readmits camera-attached planes whose occlusion slides with the view
    // (`rt all 1`). Both were measured; see GRAPHICS_RTGI.md.
    {
        GLboolean dw = GL_TRUE;
        glGetBooleanv(GL_DEPTH_WRITEMASK, &dw);
        int dt = glIsEnabled(GL_DEPTH_TEST) ? 1 : 0;
        int solid = (dw && dt) ? 1 : 0;
        // FOLIAGE CARDS ARE NOT SOLID OCCLUDERS. Tree leaves are drawn as large
        // flat quads carrying a mostly-transparent texture. The tracer has no
        // alpha, so each card becomes a fully opaque polygon and every tree
        // turns into a giant black star that swings through the AO radius as
        // the camera moves - measured directly in the AO buffer, and the cause
        // of the owner's "shadows bounce around everywhere".
        //
        // NOT via GL_ALPHA_TEST/GL_BLEND: measured, this engine leaves those
        // enabled for essentially every draw (SOLID 0, alpha-masked 370 of
        // 371), so the state says nothing. NOT via the bound-texture tracker
        // either: it keys off the texture live at bind time and only caught 2
        // of 371 here, leaving every tree in the BVH.
        //
        // The reliable answer is the draw's VERTEX PROGRAM. The wind system
        // discovered and injected the foliage programs by scan and enumerates
        // them, so a draw using one IS a leaf card by construction.
        // OFF by default. Program id alone OVER-excludes: the foliage programs
        // are shared with other instanced geometry (wind.h says so explicitly),
        // and using it dropped 332 of 371 draws, shrinking the BVH to 24608
        // tris and the root box to a third of the level. The bound-texture
        // tracker alone UNDER-excludes: it caught 2. The wind gate uses both
        // together; matching that here is the open work.
        int alpha = 0;
        // Is this draw's program one the wind injected? On its own that
        // over-excludes (332 of 371 draws) because those programs are shared,
        // so it is combined per TRIANGLE with size in AddTri: large AND
        // foliage-program = leaf card. Measured, the big triangles cluster
        // almost entirely in foliage programs (151: 16906, 115: 6528,
        // 116: 957, 127: 260) while non-foliage programs contribute 190, 19,
        // 16, 4 - so the pair separates cards from terrain cleanly.
        g_curFoliageProg = 0;
        if (vpId > 0) {
            unsigned fv[64];
            int fn = SWSE_FoliagePrograms(fv, 64);
            for (int i = 0; i < fn; i++)
                if (fv[i] == (unsigned)vpId) { g_curFoliageProg = 1; break; }
        }
        if (g_excludeFoliage && vpId > 0) {
            // Cheap: at most 64 ids, and only over the harvest's few hundred
            // draws. Re-read each time so a program discovered mid-session is
            // picked up without any invalidation dance.
            unsigned folVP[64];
            int folN = SWSE_FoliagePrograms(folVP, 64);
            for (int i = 0; i < folN; i++)
                if (folVP[i] == (unsigned)vpId) { alpha = 1; break; }
        }
        g_curAlphaMask = alpha;
        g_curDepthWrite = (solid && !alpha) ? 1 : 0;
        if (g_curDepthWrite) g_depthWriteDraws++;
        else if (alpha)      g_alphaDraws++;
        else                 g_overlayDraws++;
    }
    g_curVpId = rec.vpId;
    if (rec.atype == 0x1402) g_packed++;
    if (rec.hasModel) g_modelled++;
    HarvestOne(&rec, g_idxBuf, 65536, g_vtxBuf, 65536);
}

static void AddTri(const float* a, const float* b, const float* c) {
    if (g_nTris >= MAX_TRIS || !g_tris) return;
    // Sanity gate. Level coordinates are hundreds of units (`pos` reads ~135,
    // -450, 37); anything beyond 100k is a misread layout, not geometry, and
    // one such vertex poisons the whole bounding box - which is exactly what
    // the first harvest reported.
    // The level spans about +-2600 (measured from the harvest bounds), so a
    // 100k limit let plenty of misdecoded geometry through - close enough to
    // the camera to form a shell that every ray hit within ~25 units.
    const float LIMIT = 4000.0f;
    // Vertices ALREADY rejected upstream are marked 1e30 and arrive here too,
    // so this counter was conflating "we could not unproject it" with "the
    // decode produced a wild coordinate" - two completely different failures.
    // Count them apart before drawing any conclusion about the gate.
    if (a[0] >= 1e29f || b[0] >= 1e29f || c[0] >= 1e29f) { g_preRejected++; return; }
    for (int k = 0; k < 3; k++) {
        if (!(a[k] > -LIMIT && a[k] < LIMIT) ||
            !(b[k] > -LIMIT && b[k] < LIMIT) ||
            !(c[k] > -LIMIT && c[k] < LIMIT)) {
            g_outOfRange++;
            // What do the genuinely wild ones look like? Log a few verbatim
            // rather than guessing whether the 4000-unit gate is too tight.
            if (g_oorLogged < 24) {
                g_oorLogged++;
                char s0[24], s1[24], s2[24];
                F3(s0, a[0]); F3(s1, a[1]); F3(s2, a[2]);
                wsprintfA(g_tmp, "oor: prog %d %s (%s, %s, %s)", g_curVpId,
                          g_curPacked ? "packed" : "float", s0, s1, s2);
                GeoTrace(g_tmp);
            }
            return;
        }
    }
    // Degenerate stitching triangles are how strips jump between pieces;
    // they carry no surface and would poison a BVH with zero-area leaves.
    if ((a[0] == b[0] && a[1] == b[1] && a[2] == b[2]) ||
        (b[0] == c[0] && b[1] == c[1] && b[2] == c[2]) ||
        (a[0] == c[0] && a[1] == c[1] && a[2] == c[2])) { g_hDegenerate++; return; }
    // Reject FANS FROM THE EYE. The owner spotted sheets radiating from where
    // the third-person camera sits - the signature of vertices whose depth
    // component decoded wrong, landing strung out along view rays. A real
    // surface triangle has its three vertices at similar distance from the
    // camera; a fan triangle spans a huge depth range. Uses the HARVEST
    // camera, since that is the transform that produced these coordinates.
    if (g_haveHarvestEye) {
        float dmin = 1e30f, dmax = 0.0f;
        const float* vv[3] = { a, b, c };
        for (int i = 0; i < 3; i++) {
            float dx = vv[i][0] - g_harvestEye[0];
            float dy = vv[i][1] - g_harvestEye[1];
            float dz = vv[i][2] - g_harvestEye[2];
            float d = dx*dx + dy*dy + dz*dz;
            if (d < dmin) dmin = d;
            if (d > dmax) dmax = d;
        }
        // squared distances, so 64 == a 8x span in real distance
        if (dmin > 1e-4f && dmax > dmin * 64.0f) { g_eyeFans++; return; }
    }

    // Reject BRIDGE triangles. A strip draw often concatenates several
    // disconnected pieces; where the engine stitches them we weld the end of
    // one piece to the start of the next and manufacture an enormous needle
    // spanning the level. Those phantom sheets are invisible in-game but
    // occlude everything in the BVH (owner: "vertices coming from the centre
    // which are not appearing in the vanilla game").
    // Real surfaces are roughly compact; a bridge is a sliver. Compare the
    // area against the longest edge squared - equilateral is ~0.43, a needle
    // tends to zero - and reject the needles.
    {
        float e1[3], e2[3], e3[3];
        for (int k = 0; k < 3; k++) {
            e1[k] = b[k] - a[k]; e2[k] = c[k] - a[k]; e3[k] = c[k] - b[k];
        }
        float l1 = e1[0]*e1[0] + e1[1]*e1[1] + e1[2]*e1[2];
        float l2 = e2[0]*e2[0] + e2[1]*e2[1] + e2[2]*e2[2];
        float l3 = e3[0]*e3[0] + e3[1]*e3[1] + e3[2]*e3[2];
        float lmax = (l1 > l2) ? l1 : l2; if (l3 > lmax) lmax = l3;
        float cx = e1[1]*e2[2] - e1[2]*e2[1];
        float cy = e1[2]*e2[0] - e1[0]*e2[2];
        float cz = e1[0]*e2[1] - e1[1]*e2[0];
        float area2 = cx*cx + cy*cy + cz*cz;          // (2*area)^2
        // area / lmax^2 < 0.005  ->  a needle, not a surface
        if (lmax > 1e-6f && area2 < (0.0001f * lmax * lmax)) { g_slivers++; return; }

        // Reject GIANT SHEETS. The sliver test only catches needles; a
        // mis-decoded mesh can also come out as a broad, well-formed polygon
        // hundreds of units across. One of those has been parked in front of
        // the camera the whole session - it is why the very first traced
        // frames came back uniformly white at every range and orientation.
        // Real geometry here has a median triangle area of ~0.2 units^2;
        // this level's own bounds are only ~550 units wide.
        // area2 is (2*area)^2, so compare against (2*MAXA)^2.
        const float MAXA = 2000.0f;                 // ~45x45 units
        if (area2 > (4.0f * MAXA * MAXA)) { g_giants++; return; }
    }

    // WHICH PROGRAMS DRAW BIG FLAT CARDS? Leaf cards are metres across while
    // real world geometry here averages ~0.2 units^2 per triangle. Counting
    // large triangles per program NAMES the card programs instead of guessing
    // at render state - the same method that cracked every decode bug.
    {
        float e1[3] = { b[0]-a[0], b[1]-a[1], b[2]-a[2] };
        float e2[3] = { c[0]-a[0], c[1]-a[1], c[2]-a[2] };
        float cx = e1[1]*e2[2] - e1[2]*e2[1];
        float cy = e1[2]*e2[0] - e1[0]*e2[2];
        float cz = e1[0]*e2[1] - e1[1]*e2[0];
        float area2 = cx*cx + cy*cy + cz*cz;      // (2*area)^2
        if (area2 > 64.0f) {                      // area > 4 units^2
            int f = 0;
            for (int i = 0; i < g_nBigProg; i++)
                if (g_bigProg[i] == g_curVpId) { g_bigCount[i]++; f = 1; break; }
            if (!f && g_nBigProg < 32) { g_bigProg[g_nBigProg] = g_curVpId; g_bigCount[g_nBigProg++] = 1; }
            // A LEAF CARD: big, and drawn by a program the wind injected. The
            // tracer has no alpha, so left in the BVH this is a solid slab
            // metres across, and every tree becomes a black star that swings
            // through the AO radius as the camera turns.
            if (g_curFoliageProg) { g_curTriCard = 1; g_cardTris++; }
        }
        g_curTriArea = (area2 > 0.0f) ? (0.5f * (float)sqrt((double)area2)) : 0.0f;
    }
    if (g_triPacked) g_triPacked[g_nTris] = (unsigned char)g_curPacked;
    // Store FACTS here, apply POLICY at BVH build. Baking the card decision in
    // at harvest meant every threshold experiment cost a full re-harvest and a
    // game restart; keeping area and provenance lets `rt build` re-filter
    // instantly.
    if (g_triWorld)  g_triWorld[g_nTris]  = (unsigned char)g_curDepthWrite;
    if (g_triFol)    g_triFol[g_nTris]    = (unsigned char)g_curFoliageProg;
    if (g_triArea)   g_triArea[g_nTris]   = g_curTriArea;
    g_curTriCard = 0;
    float* t = g_tris + (size_t)g_nTris * 9;
    t[0]=a[0]; t[1]=a[1]; t[2]=a[2];
    t[3]=b[0]; t[4]=b[1]; t[5]=b[2];
    t[6]=c[0]; t[7]=c[1]; t[8]=c[2];
    for (int k = 0; k < 3; k++) {
        if (a[k] < g_hLo[k]) g_hLo[k] = a[k];
        if (a[k] > g_hHi[k]) g_hHi[k] = a[k];
    }
    g_nTris++;
}

// Pull one recorded draw's geometry and expand it into world-space triangles.
static void HarvestOne(const DrawRec* r, unsigned* idxBuf, int idxCap,
                       float* vtxBuf, int vtxCap) {
    // Packed positions with no scale/bias found cannot be decoded: normalising
    // to -1..1 without scaling back collapses the whole mesh to a dot.
    // Measured: 110 such draws, every one of them 'collapsed'. Emitting them
    // puts debris in the BVH; skipping them is honest.
    if (r->atype == 0x1402 && !r->hasUnpack) { g_noDecode++; return; }
    int isz = (r->type == GL_UNSIGNED_INT) ? 4 : (r->type == GL_UNSIGNED_SHORT) ? 2 : 1;
    int n = r->count;
    if (n < 3 || n > idxCap) { g_hFailed++; return; }

    static unsigned char raw[65536 * 4];
    if ((size_t)n * isz > sizeof(raw)) { g_hFailed++; return; }
    if (!ReadFrom(GL_ELEMENT_ARRAY_BUFFER_, r->ebo, r->idxPtr,
                  r->ebo ? (GLintptr_)r->idxOffset : 0,
                  (GLsizeiptr_)(n * isz), raw)) { g_hFailed++; return; }
    unsigned lo = 0xFFFFFFFF, hi = 0;
    for (int i = 0; i < n; i++) {
        unsigned v = (isz == 4) ? ((unsigned*)raw)[i]
                   : (isz == 2) ? ((unsigned short*)raw)[i] : raw[i];
        idxBuf[i] = v;
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    int nv = (int)(hi - lo + 1);
    if (nv <= 0 || nv > vtxCap) { g_hFailed++; return; }

    // One read for the whole vertex span, then stride-walk it on the CPU.
    static unsigned char vraw[1 << 22];             // 4 MB span ceiling
    // The LAST vertex needs only its attribute, not a whole stride. Asking
    // for nv*stride starting at a non-zero attribute offset overruns the
    // buffer by exactly that offset - measured: "want offset 4 + 80 bytes,
    // BUFFER IS 80 bytes", failing 882 of 884 draws with GL_INVALID_VALUE
    // while every attrOff==0 draw succeeded.
    int compBytes = (r->atype == GL_FLOAT) ? 4 : 2;
    size_t attrBytes = (size_t)r->asize * compBytes;
    size_t span = (size_t)(nv - 1) * r->stride + attrBytes;
    if (span > sizeof(vraw)) { g_hFailed++; return; }
    GLintptr_ base = (GLintptr_)(size_t)r->attrPtr + (GLintptr_)lo * r->stride;
    const void* cbase = r->vbo ? 0 : (const void*)((const unsigned char*)r->attrPtr + (size_t)lo * r->stride);
    if (!ReadFrom(GL_ARRAY_BUFFER_, r->vbo, cbase, r->vbo ? base : 0,
                  (GLsizeiptr_)span, vraw)) { g_hFailed++; return; }
    for (int i = 0; i < nv; i++) {
        const unsigned char* src = vraw + (size_t)i * r->stride;
        float p4[4] = { 0, 0, 0, 1 };
        if (r->atype == GL_FLOAT) {
            const float* f = (const float*)src;
            for (int k = 0; k < r->asize && k < 4; k++) p4[k] = f[k];
        } else {
            // Packed 16-bit positions - how this engine stores its world.
            // ALWAYS normalize when an unpack scale/bias pair is present.
            // Measured: the scale is the object's bounding-box size (18.8 x
            // 4.8 x 19.1 for one mesh, 2.2 x 4.1 x 3.0 for another), which
            // only makes sense multiplying a -1..1 domain. Trusting GL's
            // normalized flag gave raw integers for some draws - positions
            // exploded (85,185 out-of-range rejects) - and the shell on the
            // camera is the same bug from the other side.
            const short* s = (const short*)src;
            int norm = r->hasUnpack ? 1 : r->normalized;
            for (int k = 0; k < r->asize && k < 4; k++)
                p4[k] = norm ? (float)s[k] / 32767.0f : (float)s[k];
        }
        if (r->asize < 4) p4[3] = 1.0f;
        // The assumption everything rests on: that attrib 0 is the position
        // stream. Print the RAW shorts for a few vertices - if they barely
        // vary, or look like normals/UVs, the assumption is wrong and every
        // downstream fix is moot.
        if (r->atype == 0x1402 && g_rawLogged < 9 && i < 3) {
            g_rawLogged++;
            const short* s = (const short*)src;
            wsprintfA(g_tmp, "raw: v%d shorts [%d %d %d %d] stride %d size %d -> unpacked (%d.%03d, %d.%03d, %d.%03d)",
                      i, (int)s[0], (int)s[1], (int)s[2],
                      (r->asize > 3) ? (int)s[3] : 0, r->stride, r->asize,
                      (int)p4[0], (int)((p4[0] < 0 ? -p4[0] : p4[0]) * 1000) % 1000,
                      (int)p4[1], (int)((p4[1] < 0 ? -p4[1] : p4[1]) * 1000) % 1000,
                      (int)p4[2], (int)((p4[2] < 0 ? -p4[2] : p4[2]) * 1000) % 1000);
            Line(g_tmp);
        }
        if (r->hasUnpack) {                       // MUL by scale, ADD bias
            for (int k = 0; k < 4; k++) p4[k] = p4[k] * r->scale[k] + r->bias[k];
        }
        float w0 = p4[0], w1 = p4[1], w2 = p4[2];
        if (r->hasModel) {                        // DP4 against the model rows
            w0 = p4[0]*r->model[0][0] + p4[1]*r->model[0][1] + p4[2]*r->model[0][2] + p4[3]*r->model[0][3];
            w1 = p4[0]*r->model[1][0] + p4[1]*r->model[1][1] + p4[2]*r->model[1][2] + p4[3]*r->model[1][3];
            w2 = p4[0]*r->model[2][0] + p4[1]*r->model[2][1] + p4[2]*r->model[2][2] + p4[3]*r->model[2][3];
        } else if (r->hasMvp && !g_haveInvVP) {
            // No camera at all: object-local is NOT world space. Reject rather
            // than pretend - silently letting these through is how object-local
            // debris got into the BVH looking like real geometry.
            w0 = w1 = w2 = 1e30f;
            g_noCamVerts++;
        } else if (r->hasMvp && g_haveInvVP) {
            // Object space -> clip (this draw's MVP) -> world (inverse camera).
            float c0 = r->mvp[0]*p4[0] + r->mvp[1]*p4[1] + r->mvp[2]*p4[2]  + r->mvp[3]*p4[3];
            float c1 = r->mvp[4]*p4[0] + r->mvp[5]*p4[1] + r->mvp[6]*p4[2]  + r->mvp[7]*p4[3];
            float c2 = r->mvp[8]*p4[0] + r->mvp[9]*p4[1] + r->mvp[10]*p4[2] + r->mvp[11]*p4[3];
            float c3 = r->mvp[12]*p4[0]+ r->mvp[13]*p4[1]+ r->mvp[14]*p4[2] + r->mvp[15]*p4[3];
            float x = g_invVP[0]*c0 + g_invVP[1]*c1 + g_invVP[2]*c2  + g_invVP[3]*c3;
            float y = g_invVP[4]*c0 + g_invVP[5]*c1 + g_invVP[6]*c2  + g_invVP[7]*c3;
            float z = g_invVP[8]*c0 + g_invVP[9]*c1 + g_invVP[10]*c2 + g_invVP[11]*c3;
            float w = g_invVP[12]*c0+ g_invVP[13]*c1+ g_invVP[14]*c2 + g_invVP[15]*c3;
            // c3 is the vertex's clip w. At or behind the camera plane it
            // vanishes, and dividing by it lands the vertex ON the camera -
            // measured: 102,320 of 172,131 triangles became zero-area specks
            // 0.3 units from the eye, which is what every traced ray hit.
            // Such vertices cannot be unprojected; mark them so the range
            // filter in AddTri drops the whole triangle.
            float scale3 = (c3 < 0) ? -c3 : c3;
            if (scale3 < 1e-3f || !(w > 1e-6f || w < -1e-6f)) {
                g_clipWRej++;
                w0 = w1 = w2 = 1e30f;      // rejected downstream, and counted
            } else {
                w0 = x/w; w1 = y/w; w2 = z/w;
            }
        }
        vtxBuf[i * 3 + 0] = w0;
        vtxBuf[i * 3 + 1] = w1;
        vtxBuf[i * 3 + 2] = w2;
    }

    // Which draws collapse? Measure this draw's own output extent. A real
    // mesh spans units; a collapsed one is a dot. Naming the guilty programs
    // beats guessing at the arithmetic.
    {
        float lo3[3] = { 1e30f, 1e30f, 1e30f }, hi3[3] = { -1e30f, -1e30f, -1e30f };
        for (int i = 0; i < nv; i++)
            for (int k = 0; k < 3; k++) {
                float c = vtxBuf[i * 3 + k];
                if (c < lo3[k]) lo3[k] = c;
                if (c > hi3[k]) hi3[k] = c;
            }
        float ext = 0.0f;
        for (int k = 0; k < 3; k++) { float e = hi3[k] - lo3[k]; if (e > ext) ext = e; }
        // 0.05, not 0.5: grass tufts and pebbles are legitimately tiny, and
        // the looser threshold was reporting healthy small props as broken.
        if (ext < 0.05f) {
            g_collapsedDraws++;
            // WHICH PROGRAM, and which locals did we resolve for it? Program
            // 116 wanted c[2..5] and 151 wanted c[8..11]; naming the offender
            // is what cracked both, so name it here instead of counting.
            if (g_collProgLogged < 12) {
                g_collProgLogged++;
                wsprintfA(g_tmp, "collapse: prog %d rows x=c[%d] y=c[%d] z=c[%d] w=c[%d] unpack=%d",
                          r->vpId, r->mtxBase[0], r->mtxBase[1],
                          r->mtxBase[2], r->mtxBase[3], r->hasUnpack);
                GeoTrace(g_tmp);
            }
            // WHICH draws collapse? Answering this beats filtering symptoms.
            if (r->atype == GL_FLOAT) g_collFloat++; else g_collPacked++;
            if (!r->hasUnpack) g_collNoUnpack++;
            if (!r->hasMvp)    g_collNoMvp++;
            // THE END-TO-END TRACE: for the first collapsed PACKED draw,
            // print every stage of the decode for one vertex so the broken
            // step is visible rather than inferred. This is the method that
            // cracked the buffer overrun and the shadow passes.
            if (r->atype == 0x1402 && g_traceLogged < 2) {
                g_traceLogged++;
                const short* s0 = (const short*)(vraw + 0);
                char sc[3][24], bi[3][24], wp[3][24];
                for (int k = 0; k < 3; k++) {
                    F3(sc[k], r->scale[k]); F3(bi[k], r->bias[k]);
                    F3(wp[k], vtxBuf[k]);
                }
                wsprintfA(g_tmp, "TRACE prog %d: shorts[%d %d %d] stride %d size %d norm %d",
                          r->vpId, (int)s0[0], (int)s0[1], (int)s0[2],
                          r->stride, r->asize, r->normalized);
                Line(g_tmp);
                wsprintfA(g_tmp, "TRACE   scale(%s,%s,%s) bias(%s,%s,%s) unpack %d mvp %d",
                          sc[0], sc[1], sc[2], bi[0], bi[1], bi[2],
                          r->hasUnpack, r->hasMvp);
                Line(g_tmp);
                wsprintfA(g_tmp, "TRACE   -> world(%s, %s, %s)   [extent %d.%03d]",
                          wp[0], wp[1], wp[2], (int)ext, (int)(ext*1000)%1000);
                Line(g_tmp);
                // The matrix itself. A rank-deficient or tiny-scale MVP is
                // obvious by eye - rows near-parallel, or a row of near-zeros.
                for (int rr = 0; rr < 4; rr++) {
                    char m0[24], m1[24], m2[24], m3[24];
                    F3(m0, r->mvp[rr*4+0]); F3(m1, r->mvp[rr*4+1]);
                    F3(m2, r->mvp[rr*4+2]); F3(m3, r->mvp[rr*4+3]);
                    wsprintfA(g_tmp, "TRACE   mvp row%d [%s %s %s %s]", rr, m0, m1, m2, m3);
                    Line(g_tmp);
                }
            }
            if (g_collapseLogged < 4) {
                g_collapseLogged++;
                // WHERE did this draw read from? The read location is the
                // suspect now, so print it: buffer, attribute offset, the
                // index span, and the byte offset actually used.
                wsprintfA(g_tmp, "collapse: vbo %u ebo %u attrOff %u idx[%u..%u] stride %d -> byte %u, %d verts, extent %d.%03d",
                          r->vbo, r->ebo, (unsigned)(size_t)r->attrPtr,
                          lo, hi, r->stride,
                          (unsigned)((size_t)r->attrPtr + (size_t)lo * r->stride),
                          nv, (int)ext, (int)(ext * 1000) % 1000);
                Line(g_tmp);
            }
        } else {
            g_goodDraws++;
            // CONVERTED. Only now is this mesh genuinely collected, so only
            // now does it go into the seen table. A mesh that produced nothing
            // (no camera yet, collapsed decode) stays unmarked and gets
            // another chance on a later frame once the camera and the foliage
            // program list are warm.
            if (g_curMeshKey) { SeenMark(g_curMeshKey); g_curMeshKey = 0; }
            // One healthy draw for comparison - the difference between these
            // two lines is the whole bug.
            if (g_goodLogged < 2) {
                g_goodLogged++;
                wsprintfA(g_tmp, "healthy:  vbo %u ebo %u attrOff %u idx[%u..%u] stride %d -> byte %u, %d verts, extent %d.%03d",
                          r->vbo, r->ebo, (unsigned)(size_t)r->attrPtr,
                          lo, hi, r->stride,
                          (unsigned)((size_t)r->attrPtr + (size_t)lo * r->stride),
                          nv, (int)ext, (int)(ext * 1000) % 1000);
                Line(g_tmp);
            }
        }
    }

    #define VTX(k) (vtxBuf + (size_t)(idxBuf[k] - lo) * 3)
    if (r->mode == GL_TRIANGLES_) {
        for (int i = 0; i + 2 < n; i += 3) AddTri(VTX(i), VTX(i + 1), VTX(i + 2));
    } else if (r->mode == GL_TRIANGLE_STRIP_) {
        // Winding alternates every triangle; keep it consistent so a tracer
        // can trust face orientation later.
        for (int i = 0; i + 2 < n; i++) {
            if (i & 1) AddTri(VTX(i + 1), VTX(i), VTX(i + 2));
            else       AddTri(VTX(i), VTX(i + 1), VTX(i + 2));
        }
    } else if (r->mode == GL_TRIANGLE_FAN_) {
        for (int i = 1; i + 1 < n; i++) AddTri(VTX(0), VTX(i), VTX(i + 1));
    }
    #undef VTX
    g_hDraws++;
}

// The reads already happened, in place, during the draws. This only closes
// the books on the frame.
static void HarvestRun(void) {
    LARGE_INTEGER f, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t1);
    g_hMs = (double)(t1.QuadPart - g_hStart.QuadPart) * 1000.0 / (double)f.QuadPart;
    g_harvestDone = 1;
    g_harvestArmed = 0;
}

void SWSE_GeoHarvestReport(SWSE_GeoEmit emit) {
    GeoTrace("trace: report ENTER");
    if (!emit) return;
    if (!g_harvestDone) { emit("harvest: nothing yet - run 'harvest' then 'harvest show'"); return; }
    char b[LINE_LEN], a1[24], a2[24], a3[24], a4[24], a5[24], a6[24];
    wsprintfA(b, "harvest: %d unique meshes (%d dropped), %d converted, %d failed, %d dup skips",
              g_nRecs, g_recsDropped, g_hDraws, g_hFailed, g_dupSkipped);
    emit(b);
    wsprintfA(b, "harvest: %d triangles, %d degenerate stitches, %d SLIVER BRIDGES dropped",
              g_nTris, g_hDegenerate, g_slivers);
    emit(b);
    F3(a1, g_hLo[0]); F3(a2, g_hHi[0]);
    F3(a3, g_hLo[1]); F3(a4, g_hHi[1]);
    F3(a5, g_hLo[2]); F3(a6, g_hHi[2]);
    wsprintfA(b, "harvest: bounds x[%s..%s] y[%s..%s] z[%s..%s]", a1, a2, a3, a4, a5, a6);
    emit(b);
    F3(a1, (float)g_hMs);
    wsprintfA(b, "harvest: %s ms, %d KB of triangle data", a1,
              (int)((size_t)g_nTris * 9 * sizeof(float) / 1024));
    emit(b);
    wsprintfA(b, "harvest: skipped - no attrib0 %d, unknown type %d; out-of-range tris %d",
              g_skipNoAttr0, g_skipNotFloat, g_outOfRange);
    emit(b);
    // These two used to be lumped into "out-of-range", which made a decode
    // problem and an unprojectable-vertex problem look like one number.
    wsprintfA(b, "harvest: pre-rejected tris %d (verts: clip-w ~0 %d, no-camera %d)",
              g_preRejected, g_clipWRej, g_noCamVerts);
    emit(b);
    wsprintfA(b, "harvest: SOLID draws %d; foliage cards %d; non-depth overlay %d (sky/fog/water)",
              g_depthWriteDraws, g_alphaDraws, g_overlayDraws);
    emit(b);
    // Big-triangle census by program: the card programs, named.
    {
        unsigned folVP[64];
        int folN = SWSE_FoliagePrograms(folVP, 64);
        for (int i = 0; i < g_nBigProg && i < 10; i++) {
            int isFol = 0;
            for (int k = 0; k < folN; k++)
                if (folVP[k] == (unsigned)g_bigProg[i]) { isFol = 1; break; }
            wsprintfA(b, "harvest: BIG-CARD prog %d -> %d tris over 4 units^2  %s",
                      g_bigProg[i], g_bigCount[i], isFol ? "<< FOLIAGE PROGRAM" : "");
            emit(b);
        }
        wsprintfA(b, "harvest: wind knows %d foliage programs; LEAF-CARD tris excluded %d of %d",
                  folN, g_cardTris, g_nTris);
        emit(b);
    }
    wsprintfA(b, "harvest: packed(16-bit) draws %d, model-transformed %d, undecodable %d, ZERO-MATRIX skipped %d",
              g_packed, g_modelled, g_noDecode, g_zeroMvp);
    emit(b);
    wsprintfA(b, "harvest: draws producing real extent %d, COLLAPSED to a dot %d",
              g_goodDraws, g_collapsedDraws);
    emit(b);
    wsprintfA(b, "harvest: of the collapsed - float-attrib %d, packed %d, no-unpack %d, no-mvp %d",
              g_collFloat, g_collPacked, g_collNoUnpack, g_collNoMvp);
    emit(b);
    wsprintfA(b, "harvest: eye-fan tris %d, giant sheets %d, SHADOW-PASS DRAWS SKIPPED %d",
              g_eyeFans, g_giants, g_shadowPass);
    emit(b);
    wsprintfA(b, "harvest: BUFFER READ ERRORS %d (last GL error 0x%X)",
              g_readErrors, g_lastReadError);
    emit(b);
    wsprintfA(b, "harvest: distinct c[1..4] matrices across draws: %d%s (top counts %d, %d, %d)",
              g_nMtx, g_mtxOverflow ? "+ (capped)" : "",
              g_nMtx > 0 ? g_mtxCount[0] : 0,
              g_nMtx > 1 ? g_mtxCount[1] : 0,
              g_nMtx > 2 ? g_mtxCount[2] : 0);
    emit(b);
    emit(g_nMtx <= 2 ? "harvest: -> ONE shared matrix: c[1..4] is the camera; positions are world space"
                     : "harvest: -> MANY matrices: c[1..4] is a per-object MVP; world = inv(VP) * MVP * p");
    // Report what the camera did across the WHOLE harvest, not just whatever
    // the last frame happened to see - the old one-shot flag reported "NO
    // CAMERA" on a harvest whose bounds were perfectly centred on the player.
    if (g_haveInvVP && g_noCamVerts == 0) {
        wsprintfA(g_tmp, "harvest: camera OK on %d frame(s) - object space recovered to world", g_camFrames);
    } else if (g_haveInvVP) {
        wsprintfA(g_tmp, "harvest: camera OK on %d frame(s), but %d verts had none and were REJECTED",
                  g_camFrames, g_noCamVerts);
    } else {
        wsprintfA(g_tmp, "harvest: NO CAMERA (wind published none) - %d verts rejected, nothing usable", g_noCamVerts);
    }
    emit(g_tmp);
    for (int i = 0; i < g_nTypes; i++) {
        const char* nm = (g_typeVal[i] == 0x1402) ? "SHORT"
                       : (g_typeVal[i] == 0x1403) ? "UNSIGNED_SHORT"
                       : (g_typeVal[i] == 0x140B) ? "HALF_FLOAT"
                       : (g_typeVal[i] == 0x1401) ? "UNSIGNED_BYTE"
                       : (g_typeVal[i] == 0x1400) ? "BYTE" : "?";
        wsprintfA(b, "harvest: skipped attrib0 type 0x%X (%s) x%d draws",
                  g_typeVal[i], nm, g_typeCount[i]);
        emit(b);
    }
    for (int i = 0; i < g_nLayouts; i++) {
        char bits[64]; bits[0] = 0;
        for (int a = 0; a < 16; a++) {
            if (g_layoutMask[i] & (1u << a)) {
                char one[8]; wsprintfA(one, "%d ", a);
                lstrcatA(bits, one);
            }
        }
        wsprintfA(b, "harvest: layout [%s] used by %d draws", bits[0] ? bits : "(none)", g_layouts[i]);
        emit(b);
    }
    if (g_nTris >= MAX_TRIS) emit("harvest: HIT THE TRIANGLE CAP - level is larger than the buffer");
    GeoTrace("trace: report EXIT ok");
}

// Write the soup as a Wavefront OBJ so the harvest can be inspected (and
// path-traced) outside the game. Verification the owner can open and rotate.
int SWSE_GeoHarvestDumpObj(char* pathOut, int pathLen, int maxTris) {
    if (!g_harvestDone || g_nTris == 0) return 0;
    char path[MAX_PATH];
    GetModuleFileNameA(GetModuleHandleA(NULL), path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) *(slash + 1) = 0;
    lstrcatA(path, "swse_harvest.obj");
    FILE* fp = fopen(path, "w");
    if (!fp) return 0;
    int n = (maxTris > 0 && maxTris < g_nTris) ? maxTris : g_nTris;
    fprintf(fp, "# SWSE geometry harvest: %d triangles (world space)\n", n);
    for (int t = 0; t < n; t++) {
        const float* v = g_tris + (size_t)t * 9;
        for (int k = 0; k < 3; k++)
            fprintf(fp, "v %.4f %.4f %.4f\n", v[k*3+0], v[k*3+1], v[k*3+2]);
    }
    for (int t = 0; t < n; t++)
        fprintf(fp, "f %d %d %d\n", t*3+1, t*3+2, t*3+3);
    fclose(fp);
    lstrcpynA(pathOut, path, pathLen);
    return n;
}

// Fetch ONE program's source by its live GL id and print the instructions
// that build result.position. The shader dump numbers sequentially, not by
// GL id, so matching a traced program to the dump is guesswork - this is not.
void SWSE_GeoDumpProgram(unsigned progId, SWSE_GeoEmit emit) {
    if (!emit) return;
    if (!p_GetProgramivARB || !p_GetProgramString || !p_BindProgramARBGeo) {
        emit("progsrc: ARB program entry points missing"); return;
    }
    GLint prev = 0;
    p_GetProgramivARB(0x8620, 0x8677 /*PROGRAM_BINDING*/, &prev);
    p_BindProgramARBGeo(0x8620, progId);
    GLint len = 0;
    p_GetProgramivARB(0x8620, 0x8627 /*PROGRAM_LENGTH*/, &len);
    if (len <= 0 || len >= (GLint)sizeof(g_src)) {
        p_BindProgramARBGeo(0x8620, (GLuint)prev);
        wsprintfA(g_tmp, "progsrc: program %u has length %d - not readable", progId, len);
        emit(g_tmp);
        return;
    }
    p_GetProgramString(0x8620, 0x8628 /*PROGRAM_STRING*/, g_src);
    g_src[len] = 0;
    p_BindProgramARBGeo(0x8620, (GLuint)prev);

    wsprintfA(g_tmp, "progsrc: program %u, %d bytes - lines touching attrib[0] or result.position:",
              progId, len);
    emit(g_tmp);
    char line[220];
    const char* p = g_src;
    int shown = 0;
    while (*p && shown < 24) {
        int n = 0;
        while (*p && *p != '\n' && n < (int)sizeof(line) - 1) line[n++] = *p++;
        line[n] = 0;
        if (*p) p++;
        if (n == 0 || line[0] == '#') continue;
        if (strstr(line, "vertex.attrib[0]") || strstr(line, "result.position")) {
            emit(line);
            shown++;
        }
    }
}

void SWSE_GeoReport(SWSE_GeoEmit emit) {
    if (!emit) return;
    if (g_nLines == 0) { emit("geo: nothing captured yet - run 'geo' first, then 'geo show'"); return; }
    for (int i = 0; i < g_nLines; i++) emit(g_lines[i]);
}
