// SWSE foliage wind - see wind.h.

#include "wind.h"
#include "foliage.h"
#include "scriptvm.h"       // SWSE_PosGet - the player's world position
#include <windows.h>
#include <gl/GL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define GL_VERTEX_PROGRAM_ARB       0x8620
#define GL_PROGRAM_LENGTH_ARB       0x8627
#define GL_PROGRAM_STRING_ARB       0x8628
#define GL_PROGRAM_BINDING_ARB      0x8677
#define GL_PROGRAM_FORMAT_ASCII_ARB 0x8875
#define GL_PROGRAM_ERROR_POSITION_ARB 0x864B

typedef char GLcharARB;
typedef void (APIENTRY* PFN_BINDPROGRAMARB)(GLenum, GLuint);
typedef void (APIENTRY* PFN_PROGRAMSTRINGARB)(GLenum, GLenum, GLsizei, const void*);
typedef void (APIENTRY* PFN_GETPROGRAMIVARB)(GLenum, GLenum, GLint*);
typedef void (APIENTRY* PFN_GETPROGRAMSTRINGARB)(GLenum, GLenum, void*);
typedef void (APIENTRY* PFN_PROGRAMENVPARAM4FARB)(GLenum, GLuint, GLfloat, GLfloat, GLfloat, GLfloat);

static PFN_BINDPROGRAMARB       p_BindProgramARB;
static PFN_PROGRAMSTRINGARB     p_ProgramStringARB;
static PFN_GETPROGRAMIVARB      p_GetProgramivARB;
static PFN_GETPROGRAMSTRINGARB  p_GetProgramStringARB;
static PFN_PROGRAMENVPARAM4FARB p_ProgramEnvParameter4fARB;
static bool g_resolved = false;

// ---- stall instrumentation -------------------------------------------------
// Uploading a program makes the driver COMPILE it, which is a classic
// hundred-millisecond stall, and this injects programs during play as new
// plant types appear. Reading a program back can force a sync too. Both are
// timed so a hitch can be attributed instead of guessed at.
static double g_msInject = 0, g_msInjectMax = 0;
static double g_msCheck  = 0, g_msCheckMax  = 0;
static int    g_stalls   = 0;

static double NowMs() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static void LogW(const char* s) {
    char path[MAX_PATH];
    GetModuleFileNameA(GetModuleHandleA(NULL), path, MAX_PATH);
    char* sl = strrchr(path, '\\');
    if (sl) *(sl + 1) = 0;
    lstrcatA(path, "swse_log.txt");
    FILE* f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "%s\n", s);
    fclose(f);
}

static bool Resolve() {
    if (g_resolved) return p_ProgramStringARB != nullptr;
    g_resolved = true;
    HMODULE gl = GetModuleHandleA("opengl32.dll");
    if (!gl) return false;
    typedef PROC (WINAPI* wglGPA_t)(LPCSTR);
    wglGPA_t gpa = (wglGPA_t)GetProcAddress(gl, "wglGetProcAddress");
    if (!gpa) return false;
    p_BindProgramARB           = (PFN_BINDPROGRAMARB)gpa("glBindProgramARB");
    p_ProgramStringARB         = (PFN_PROGRAMSTRINGARB)gpa("glProgramStringARB");
    p_GetProgramivARB          = (PFN_GETPROGRAMIVARB)gpa("glGetProgramivARB");
    p_GetProgramStringARB      = (PFN_GETPROGRAMSTRINGARB)gpa("glGetProgramStringARB");
    p_ProgramEnvParameter4fARB = (PFN_PROGRAMENVPARAM4FARB)gpa("glProgramEnvParameter4fARB");
    return p_ProgramStringARB && p_GetProgramStringARB && p_BindProgramARB
        && p_GetProgramivARB && p_ProgramEnvParameter4fARB;
}

// ---- state ----------------------------------------------------------------
#define MAX_PROGS 64
#define SRC_MAX   (64 * 1024)

struct SavedProg {
    unsigned id;
    char*    orig;     // original source, for restore
    int      origLen;
};
static SavedProg g_saved[MAX_PROGS];
static int  g_savedN = 0;
static int  g_injected = 0, g_failed = 0;
static bool g_on = false;
static bool g_testMode = false;
static int  g_pending = 0;          // 1 = inject, -1 = restore, 0 = idle

// Bend in local units per unit of height: 0.060 moves the tip of a plant by
// 6% of its own height. 0.010 was measured as working but literally
// invisible; the brief is "gives life to the foliage", which still has to be
// perceptible.
static float g_strength = 0.060f;
static float g_speed    = 1.0f;
// Stiffness: the greatest plant height that still gains movement. Beyond it a
// plant bends no further, so tall trees stop whipping. It is a UNIFORM, so it
// tunes live with no re-injection.
static float g_weight   = 0.5f;
// The shader needs 1/cap: ARB has RCP but no divide, and doing it once here
// beats doing it per vertex.
static float g_invWeight = 2.0f;
static float g_curX = 0.0f, g_curZ = 0.0f, g_phase = 0.0f;
// Player push: foliage bends away from the player, as if they were the wind.
static float g_push       = 0.35f;   // how far, in local units
static float g_pushRadius = 3.0f;    // how close before anything happens
// Plants taller than this are not pushed at all: grass and undergrowth part
// around the player, trees stay put.
static float g_pushMax    = 1.0f;
static float g_px = 0.0f, g_py = 0.0f, g_pz = 0.0f;
static int   g_havePlayer = 0;
static bool  g_gateOpen = false;    // is the wind param currently non-zero
static bool  g_gateNoPush = false;  // current draw is a plant that must not be pushed
static int   g_gateSway = 100;      // current draw's sway scale, percent
// Per-draw gating is REQUIRED, and this was measured rather than assumed.
// Programs 3/4/127 were observed drawing foliage, but running ungated made the
// TIRES sway - they are generic instanced-geometry programs shared by plants,
// tyres, and anything else built the same way. So the wind parameter is only
// non-zero while a foliage texture is bound.
static bool  g_useGate = true;
// Z is up in these models. MEASURED, not assumed: with Y the plants sheared
// and appeared to twist; with Z they hinge at the base and lean over, which is
// what wind looks like. Nothing in the shader source reveals this - the vertex
// programs only show the projection convention - so it was settled by trying
// both and looking.
static int   g_upAxis  = 1;         // 0 = Y is up, 1 = Z is up
// Per-plant phase seeding, DEFAULT OFF.
//
// The first attempt seeded phase from vertex.attrib[1].w, on the reading that
// attrib[1] was an instance-matrix row. It is not: the program header declares
// ATTR1 as input.Weight, so it varies PER VERTEX. Different vertices of one
// plant therefore got different phases and the plants visibly twisted on
// themselves. With this off every plant shares one phase - they move in sync,
// which is far less wrong than twisting.
// Default ON: on the correct (Z) axis the seed desyncs plants cleanly. The
// twisting that caused it to be disabled was the WRONG AXIS, not the seed -
// attrib[1] does hold each plant's instance transform, replicated across that
// plant's vertices, so it is constant within a plant after all.
static int   g_seedMode = 1;        // 0 = one global phase, 1 = per-plant attr
static bool  g_reinject = false;

// ---- program rewriting ----------------------------------------------------
//
// The edit is anchored on the dequantise pair that every foliage program
// begins with, rather than on a line number or a program id:
//
//     MUL R0, vertex.attrib[0], c[N];
//     ADD <dst>, R0, c[M];
//
// <dst> holds the local-space position. Anchoring on the instruction shape
// means the same code works for all three programs and would keep working if
// the ids changed between runs (they are driver-assigned).
static const char* FindLine(const char* src, const char* needle) {
    return strstr(src, needle);
}

static bool Inject(GLuint id, char* work, int workMax) {
    GLint len = 0;
    p_BindProgramARB(GL_VERTEX_PROGRAM_ARB, id);
    p_GetProgramivARB(GL_VERTEX_PROGRAM_ARB, GL_PROGRAM_LENGTH_ARB, &len);
    if (len <= 0 || len >= SRC_MAX) return false;

    char* src = (char*)malloc(len + 1);
    if (!src) return false;
    memset(src, 0, len + 1);
    p_GetProgramStringARB(GL_VERTEX_PROGRAM_ARB, GL_PROGRAM_STRING_ARB, src);
    src[len] = 0;

    if (strstr(src, "swsew")) { free(src); return true; }   // already injected

    // Find where the local-space position is produced.
    //
    // The first version anchored on the literal "MUL R0, vertex.attrib[0], c["
    // and only 3 of 213 programs matched. Cg's -O3 output is not one fixed
    // shape: the scale and bias are sometimes two instructions (MUL then ADD)
    // and sometimes fused into a single MAD, and the temp is not always R0.
    // So anchor on the first instruction that READS vertex.attrib[0], take its
    // destination register, and only chase a following ADD when the first
    // instruction was a bare MUL.
    const char* a0 = strstr(src, "vertex.attrib[0]");
    if (!a0) { free(src); return false; }        // no position input: not ours
    const char* ls = a0;
    while (ls > src && *(ls - 1) != '\n') ls--;  // rewind to the line start
    while (*ls == ' ' || *ls == '\t') ls++;

    char op[8] = {0};
    for (int i = 0; i < 7 && ls[i] && ls[i] != ' '; i++) op[i] = ls[i];

    char dst[24] = {0};
    {
        const char* p = ls;
        while (*p && *p != ' ') p++;             // past the opcode
        while (*p == ' ') p++;
        int di = 0;
        for (; *p && *p != ',' && *p != ' ' && *p != '.' && di < 23; p++)
            dst[di++] = *p;                      // register name, swizzle dropped
    }
    if (!dst[0] || dst[0] == 'r') { free(src); return false; }

    const char* insertAfter = strchr(ls, '\n');
    if (!insertAfter) { free(src); return false; }
    insertAfter++;

    if (!strcmp(op, "MUL")) {
        // the bias is a separate ADD; the position is not final until after it
        bool found = false;
        const char* scan = insertAfter;
        for (int guard = 0; scan && *scan && guard < 8; guard++) {
            const char* s = scan;
            while (*s == ' ' || *s == '\t') s++;
            const char* eol = strchr(s, '\n');
            if (!eol) break;
            if (!strncmp(s, "ADD ", 4)) {
                char seg[256];
                int n = (int)(eol - s); if (n > 255) n = 255;
                memcpy(seg, s, n); seg[n] = 0;
                if (strstr(seg, dst)) {
                    char nd[24] = {0};
                    const char* p = s + 4;
                    while (*p == ' ') p++;
                    int di = 0;
                    for (; *p && *p != ',' && *p != ' ' && *p != '.' && di < 23; p++)
                        nd[di++] = *p;
                    if (nd[0]) lstrcpynA(dst, nd, 24);
                    insertAfter = eol + 1;
                    found = true;
                    break;
                }
            }
            scan = eol + 1;
        }
        if (!found) { free(src); return false; }
    }

    // Which local axis is UP. Not derivable from the shader - the vertex
    // programs only reveal the projection convention, not the model's own
    // orientation - so it is a switch, set by looking at the result.
    // Declared here because the player push needs it too, for the size gate.
    const char* up = (g_upAxis == 0) ? "y" : "z";
    const char* h1 = "x";
    const char* h2 = (g_upAxis == 0) ? "z" : "y";

    // ---- second injection point: WORLD space -----------------------------
    //
    // The wind bend goes in local space, but "away from the player" is a
    // world-space direction, and rotating one into the other would need the
    // inverse of each plant's instance transform. So the player push is
    // applied AFTER the instance transform instead, where both the plant
    // position and the direction are in world space and the maths is exact.
    //
    // The instance transform is the run of DP4s that read vertex attributes;
    // the last of them completes the world position. Programs without that run
    // (static geometry) are already in world space at the first point.
    char wdst[24] = {0};
    const char* worldAfter = nullptr;
    {
        const char* scan = insertAfter;
        for (int guard = 0; scan && *scan && guard < 24; guard++) {
            const char* s = scan;
            while (*s == ' ' || *s == '\t') s++;
            const char* eol = strchr(s, '\n');
            if (!eol) break;
            if (!strncmp(s, "DP4 ", 4)) {
                char seg[256];
                int n = (int)(eol - s); if (n > 255) n = 255;
                memcpy(seg, s, n); seg[n] = 0;
                if (strstr(seg, "vertex.attrib[")) {
                    const char* p = s + 4;
                    while (*p == ' ') p++;
                    int di = 0; char nd[24] = {0};
                    for (; *p && *p != ',' && *p != ' ' && *p != '.' && di < 23; p++)
                        nd[di++] = *p;
                    if (nd[0]) { lstrcpynA(wdst, nd, 24); worldAfter = eol + 1; }
                }
            }
            scan = eol + 1;
        }
    }
    if (!worldAfter) { lstrcpynA(wdst, dst, 24); worldAfter = insertAfter; }

    // The push READS the world position back, and in ARB assembly `result.*`
    // registers are WRITE-ONLY - reading one is GL_INVALID_OPERATION, which is
    // exactly how programs 115 and 153 failed. Some programs transform
    // straight into result.position rather than through a temp, so when that
    // happens the push is dropped for that program and only the wind is
    // injected. Losing the push on a few programs beats losing both.
    bool canPush = strncmp(wdst, "result", 6) != 0;

    // World up is Y for this engine (the `up` command raises axis 1), so the
    // horizontal plane the player pushes through is XZ.
    char push[768];
    push[0] = 0;
    if (!canPush) {
        worldAfter = insertAfter;     // nothing to insert, keep the segments valid
    } else
    wsprintfA(push,
              "SUB swseU.x, %s.x, swsep.x;\n"            // dx from player
              "SUB swseU.z, %s.z, swsep.z;\n"            // dz from player
              "MUL swseU.y, swseU.x, swseU.x;\n"
              "MAD swseU.y, swseU.z, swseU.z, swseU.y;\n"   // dist^2
              "ADD swseU.y, swseU.y, swsec.w;\n"            // avoid RSQ(0)
              "RSQ swseU.w, swseU.y;\n"                     // 1/dist
              "MAD swseU.y, -swseU.y, swsep.w, swsec.y;\n"  // 1 - d2/r2
              "MAX swseU.y, swseU.y, swsec.x;\n"            // clamp at 0
              "MUL swseU.y, swseU.y, swseq.x;\n"            // * push strength
              // SIZE GATE: only grass and small plants part around the player;
              // a tree must not shift when walked past. This uses the RAW
              // local height, not the saturated one - saturation deliberately
              // flattens everything above the cap, so a bush and a redwood
              // read identically there and could not be told apart.
              "MAX swseT.x, %s.%s, swsec.x;\n"              // raw height >= 0
              "MAD swseT.x, swseT.x, -swseq.y, swsec.y;\n"  // 1 - h/pushMax
              "MAX swseT.x, swseT.x, swsec.x;\n"            // clamp at 0
              "MUL swseU.y, swseU.y, swseT.x;\n"
              "MUL swseU.y, swseU.y, swseT.w;\n"            // keep the base planted
              "MUL swseU.x, swseU.x, swseU.w;\n"            // normalise
              "MUL swseU.z, swseU.z, swseU.w;\n"
              "MAD %s.x, swseU.x, swseU.y, %s.x;\n"
              "MAD %s.z, swseU.z, swseU.y, %s.z;\n",
              wdst, wdst, dst, up, wdst, wdst, wdst, wdst);

    // Declarations go straight after the "!!ARBvp1.0" header rather than after
    // an existing PARAM line - not every program has one, and declaration
    // order among themselves does not matter.
    const char* hdrEnd = strchr(src, '\n');
    if (!hdrEnd) { free(src); return false; }
    hdrEnd++;
    if (hdrEnd > insertAfter) { free(src); return false; }
    const char* paramEnd = hdrEnd;
    const char* addEnd   = insertAfter;

    // Per-plant phase seed. Programs 3/4 carry each plant's world transform in
    // VERTEX ATTRIBUTES, and with the position's w = 1 the translation lands
    // in attrib[1].w - i.e. that plant's world X. Using it as a phase offset
    // is what stops every plant swaying in lockstep.
    //
    // Programs WITHOUT an instance matrix (static geometry) get a constant
    // seed instead. Seeding those from vertex position would give every vertex
    // of one mesh a different phase and tear the mesh apart.
    bool instanced = strstr(src, "vertex.attrib[1]") != nullptr;
    const char* seed = (g_seedMode && instanced) ? "vertex.attrib[1].w" : "swsek.w";

    char inj[512];
    // A TRIANGLE wave, because ARBvp1.0 has no SIN: fract() gives a sawtooth,
    // and |2u-1| folds it into a triangle. Displacement is one sway scalar
    // along a FIXED direction (swsew.xz) scaled by local height - swaying
    // back and forth. Driving x and z from separate oscillators made the tops
    // trace an ellipse, which read as the plants rotating rather than blowing.
    wsprintfA(inj,
              "MAD swseT.x, %s, swsek.x, swsew.w;\n"   // phase = seed*k + time
              "FRC swseT.x, swseT.x;\n"                 // 0..1 sawtooth
              "MAD swseT.y, swseT.x, swsek.y, swsek.z;\n"  // 2u-1  -> -1..1
              "ABS swseT.y, swseT.y;\n"                 // |2u-1| -> 1..0..1
              "MAD swseT.y, swseT.y, -swsek.y, swsek.w;\n" // 1-2t -> -1..1..-1
              // WEIGHT, as SOFT saturation: h / (1 + h/cap).
              //
              // Displacement is sway * height, so it grows linearly with plant
              // size - grass barely moves while a tree swings absurdly. The
              // first fix was a hard MIN against the cap, but that damps
              // EVERYTHING taller than the cap equally, so mid-sized plants
              // lost their motion along with the trees.
              //
              // This curve leaves small plants almost untouched (h << cap
              // gives back ~h) while tall ones asymptote towards the cap, so
              // trees calm down and undergrowth keeps moving. swsew.y carries
              // 1/cap, since ARB has RCP but no divide.
              "MAX swseT.w, %s.%s, swsec.x;\n"          // height >= 0
              "MUL swseT.x, swseT.w, swsew.y;\n"        // h / cap
              "ADD swseT.x, swseT.x, swsec.y;\n"        // 1 + h/cap
              "RCP swseT.x, swseT.x;\n"
              "MUL swseT.w, swseT.w, swseT.x;\n"        // h / (1 + h/cap)
              "MUL swseT.z, swseT.y, swseT.w;\n"
              "MAD %s.%s, swseT.z, swsew.x, %s.%s;\n"
              "MAD %s.%s, swseT.z, swsew.z, %s.%s;\n",
              seed, dst, up, dst, h1, dst, h1, dst, h2, dst, h2);

    const char* decl = "PARAM swsew = program.env[0];\n"
                       "PARAM swsep = program.env[1];\n"   // player xyz, w = 1/radius^2
                       "PARAM swseq = program.env[2];\n"   // x = push strength
                       "PARAM swsek = { 0.137, 2.0, -1.0, 1.0 };\n"
                       "PARAM swsec = { 0.0, 1.0, 0.5, 0.0001 };\n"
                       "TEMP swseT;\n"
                       "TEMP swseU;\n";
    int need = len + (int)strlen(decl) + (int)strlen(inj) + (int)strlen(push) + 16;
    if (need >= workMax) { free(src); return false; }

    // rebuild:
    //   [..paramEnd) decl [paramEnd..addEnd) inj [addEnd..worldAfter) push [..]
    // When the program has no instance transform, worldAfter == addEnd and the
    // push simply follows the wind block.
    int o = 0;
    int n1 = (int)(paramEnd - src);
    memcpy(work + o, src, n1); o += n1;
    int nd = (int)strlen(decl); memcpy(work + o, decl, nd); o += nd;
    int n2 = (int)(addEnd - paramEnd);
    memcpy(work + o, paramEnd, n2); o += n2;
    int ni = (int)strlen(inj); memcpy(work + o, inj, ni); o += ni;
    int n3 = (int)(worldAfter - addEnd);
    memcpy(work + o, addEnd, n3); o += n3;
    int np = (int)strlen(push); memcpy(work + o, push, np); o += np;
    int n4 = len - (int)(worldAfter - src);
    memcpy(work + o, worldAfter, n4); o += n4;
    work[o] = 0;

    // stash the original before replacing it
    if (g_savedN < MAX_PROGS) {
        SavedProg* s = &g_saved[g_savedN];
        s->orig = (char*)malloc(len + 1);
        if (s->orig) {
            memcpy(s->orig, src, len + 1);
            s->id = id; s->origLen = len;
            g_savedN++;
        }
    }
    free(src);

    while (glGetError() != GL_NO_ERROR) {}
    p_ProgramStringARB(GL_VERTEX_PROGRAM_ARB, GL_PROGRAM_FORMAT_ASCII_ARB, o, work);
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        GLint pos = -1;
        glGetIntegerv(GL_PROGRAM_ERROR_POSITION_ARB, &pos);
        char b[200];
        wsprintfA(b, "wind: program %u REJECTED (err 0x%X at %d) - restoring", id, err, pos);
        LogW(b);
        // Write the generated source out. A byte offset into a program we
        // cannot see is not diagnosable; the file is.
        char rp[MAX_PATH];
        GetModuleFileNameA(GetModuleHandleA(NULL), rp, MAX_PATH);
        char* rs = strrchr(rp, '\\'); if (rs) *(rs + 1) = 0;
        wsprintfA(rp + lstrlenA(rp), "swse_wind_reject_%u.txt", id);
        FILE* rf = fopen(rp, "w");
        if (rf) { fprintf(rf, "%s", work); fclose(rf); }
        // put the original back immediately; a half-patched program would
        // render the plant as garbage rather than simply not moving.
        if (g_savedN > 0 && g_saved[g_savedN - 1].id == id) {
            SavedProg* s = &g_saved[--g_savedN];
            p_ProgramStringARB(GL_VERTEX_PROGRAM_ARB, GL_PROGRAM_FORMAT_ASCII_ARB,
                               s->origLen, s->orig);
            free(s->orig); s->orig = nullptr;
            while (glGetError() != GL_NO_ERROR) {}
        }
        return false;
    }
    return true;
}

// Programs that have been ATTEMPTED, successfully or not. Without this the
// per-frame catch-up retried every failure every frame, which is why the
// failure count ran to 210 - it was 3 programs counted 70 times.
static unsigned g_tried[MAX_PROGS];
static int      g_triedN = 0;

static bool AlreadyTried(unsigned id) {
    for (int i = 0; i < g_triedN; i++) if (g_tried[i] == id) return true;
    return false;
}

// Inject every known foliage program that is not already patched. Safe to call
// repeatedly: it is how newly discovered plant types get picked up mid-play.
static void InjectAll() {
    unsigned progs[MAX_PROGS];
    int n = SWSE_FoliagePrograms(progs, MAX_PROGS);
    if (n <= 0) {
        LogW("wind: no foliage vertex programs known yet - walk near some plants");
        return;
    }
    double t0 = NowMs();
    GLint prev = 0;
    p_GetProgramivARB(GL_VERTEX_PROGRAM_ARB, GL_PROGRAM_BINDING_ARB, &prev);
    char* work = (char*)malloc(SRC_MAX);
    if (!work) return;
    for (int i = 0; i < n; i++) {
        if (AlreadyTried(progs[i])) continue;
        if (g_triedN < MAX_PROGS) g_tried[g_triedN++] = progs[i];
        if (Inject(progs[i], work, SRC_MAX)) {
            g_injected++;
        } else {
            g_failed++;
            char b[120];
            wsprintfA(b, "wind: program %u did not match the position pattern", progs[i]);
            LogW(b);
        }
    }
    free(work);
    p_BindProgramARB(GL_VERTEX_PROGRAM_ARB, (GLuint)prev);
    while (glGetError() != GL_NO_ERROR) {}

    g_msInject = NowMs() - t0;
    if (g_msInject > g_msInjectMax) g_msInjectMax = g_msInject;
    if (g_msInject > 30.0) {
        g_stalls++;
        char b[160];
        wsprintfA(b, "WINDSTALL: injecting programs took %d ms (driver compile)",
                  (int)g_msInject);
        LogW(b);
    }
}

// Has the engine replaced our patched programs behind our back?
//
// Loading a level destroys and recreates the ARB programs. The ids come back
// reused but carrying the ORIGINAL source, so every injected program silently
// reverts - and since the catch-up skips anything already attempted, the wind
// would stay dead for the rest of the session after one level change.
//
// Detected rather than assumed: read a patched program back and look for our
// marker. When it has gone, the saved "originals" belong to destroyed programs
// and must be DROPPED, never written back.
static unsigned g_nextCheck = 0;

static bool InjectionStillLive() {
    if (g_savedN <= 0) return true;
    GLint len = 0, prev = 0;
    p_GetProgramivARB(GL_VERTEX_PROGRAM_ARB, GL_PROGRAM_BINDING_ARB, &prev);
    p_BindProgramARB(GL_VERTEX_PROGRAM_ARB, g_saved[0].id);
    struct Rebind {          // put the engine's binding back on every path
        PFN_BINDPROGRAMARB f; GLuint id;
        ~Rebind() { if (f) f(GL_VERTEX_PROGRAM_ARB, id); }
    } rebind = { p_BindProgramARB, (GLuint)prev };
    p_GetProgramivARB(GL_VERTEX_PROGRAM_ARB, GL_PROGRAM_LENGTH_ARB, &len);
    if (len <= 0 || len >= SRC_MAX) return false;
    char* buf = (char*)malloc(len + 1);
    if (!buf) return true;                       // cannot tell; assume fine
    memset(buf, 0, len + 1);
    p_GetProgramStringARB(GL_VERTEX_PROGRAM_ARB, GL_PROGRAM_STRING_ARB, buf);
    buf[len] = 0;
    bool live = strstr(buf, "swsew") != nullptr;
    free(buf);
    return live;
}

static void DropSaved() {
    for (int i = 0; i < g_savedN; i++) {
        if (g_saved[i].orig) free(g_saved[i].orig);
        g_saved[i].orig = nullptr;
    }
    g_savedN = 0;
    g_triedN = 0;
    g_injected = 0;
    g_failed = 0;
}

static void RestoreAll() {
    if (!Resolve()) return;
    for (int i = 0; i < g_savedN; i++) {
        SavedProg* s = &g_saved[i];
        if (!s->orig) continue;
        p_BindProgramARB(GL_VERTEX_PROGRAM_ARB, s->id);
        p_ProgramStringARB(GL_VERTEX_PROGRAM_ARB, GL_PROGRAM_FORMAT_ASCII_ARB,
                           s->origLen, s->orig);
        free(s->orig); s->orig = nullptr;
    }
    while (glGetError() != GL_NO_ERROR) {}
    g_savedN = 0;
    g_injected = 0;
    g_failed = 0;
    g_triedN = 0;      // a regenerate must re-attempt everything
}

// ---- settings persistence -------------------------------------------------
// Kept beside the foliage list in SWSEMods\SWSE Wind rather than in
// bin\, so a user's tuning lives with the mod and survives reinstalling the
// DLL. Every value here was arrived at by looking at the result in game, so
// losing them on restart means redoing that work each session.
static void SettingsPath(char* out) {
    GetModuleFileNameA(GetModuleHandleA(NULL), out, MAX_PATH);
    char* sl = strrchr(out, '\\'); if (sl) *sl = 0;    // ...\bin
    sl = strrchr(out, '\\'); if (sl) *sl = 0;          // game root
    lstrcatA(out, "\\SWSEMods\\SWSE Wind\\wind.txt");
}

void SWSE_WindSaveSettings() {
    char path[MAX_PATH];
    SettingsPath(path);
    FILE* f = fopen(path, "w");
    if (!f) { LogW("wind: could not write wind.txt"); return; }
    fprintf(f, "# SWSE foliage wind settings\n");
    fprintf(f, "# enabled  0/1   turn the effect on at launch\n");
    fprintf(f, "# strength       bend per unit of plant height (0.06 = 6%%)\n");
    fprintf(f, "# speed          oscillation rate multiplier\n");
    fprintf(f, "# axis     y/z   which local axis is up (measured: z)\n");
    fprintf(f, "# seed     0/1   per-plant phase, so plants do not sway in step\n");
    fprintf(f, "# gate     0/1   restrict wind to foliage draws (0 sways tyres too)\n");
    fprintf(f, "enabled %d\n", g_on ? 1 : 0);
    fprintf(f, "strength %.4f\n", g_strength);
    fprintf(f, "speed %.4f\n", g_speed);
    fprintf(f, "weight %.4f\n", g_weight);
    fprintf(f, "push %.4f\n", g_push);
    fprintf(f, "pushradius %.4f\n", g_pushRadius);
    fprintf(f, "pushmax %.4f\n", g_pushMax);
    fprintf(f, "axis %s\n", g_upAxis ? "z" : "y");
    fprintf(f, "seed %d\n", g_seedMode);
    fprintf(f, "gate %d\n", g_useGate ? 1 : 0);
    fclose(f);
    char b[MAX_PATH + 32];
    wsprintfA(b, "wind: settings saved to %s", path);
    LogW(b);
}

void SWSE_WindLoadSettings() {
    char path[MAX_PATH];
    SettingsPath(path);
    FILE* f = fopen(path, "r");
    if (!f) return;                       // no file: built-in defaults stand
    char line[256];
    int enable = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        char key[64] = {0}, val[64] = {0};
        if (sscanf(line, "%63s %63s", key, val) != 2) continue;
        if      (!lstrcmpiA(key, "enabled"))  enable      = atoi(val);
        else if (!lstrcmpiA(key, "strength")) g_strength  = (float)atof(val);
        else if (!lstrcmpiA(key, "speed"))    g_speed     = (float)atof(val);
        else if (!lstrcmpiA(key, "weight"))   SWSE_WindWeight((float)atof(val));
        else if (!lstrcmpiA(key, "push"))     g_push       = (float)atof(val);
        else if (!lstrcmpiA(key, "pushradius")) g_pushRadius = (float)atof(val);
        else if (!lstrcmpiA(key, "pushmax"))   g_pushMax    = (float)atof(val);
        // set directly, not through SWSE_WindAxis/Seed: those request a
        // REGENERATE, which is meaningless before anything is injected
        else if (!lstrcmpiA(key, "axis"))     g_upAxis    = (val[0] == 'z' || val[0] == 'Z') ? 1 : 0;
        else if (!lstrcmpiA(key, "seed"))     g_seedMode  = atoi(val) ? 1 : 0;
        else if (!lstrcmpiA(key, "gate"))     g_useGate   = atoi(val) != 0;
    }
    fclose(f);

    if (enable) {
        // Wind needs the foliage bind tracker: it supplies both the per-draw
        // gate and the program discovery that injection depends on.
        char msg[160] = {0};
        SWSE_FoliageTrack(1, msg, sizeof(msg));
        g_pending = 1;
        LogW("wind: enabled from wind.txt");
    }
}

// ---- public ---------------------------------------------------------------
int SWSE_WindSet(int on, char* msg, int msgLen) {
    if (!!on == g_on) {
        if (msg) lstrcpynA(msg, on ? "wind already on" : "wind already off", msgLen);
        return 1;
    }
    g_pending = on ? 1 : -1;
    if (msg) lstrcpynA(msg, on ? "wind enabling on next frame" : "wind restoring on next frame", msgLen);
    return 1;
}

void SWSE_WindParams(float strength, float speed) {
    if (strength >= 0.0f) g_strength = strength;
    if (speed    >  0.0f) g_speed    = speed;
}
void SWSE_WindWeight(float w) {
    if (w > 0.0f) { g_weight = w; g_invWeight = 1.0f / w; }
}
void SWSE_WindPush(float amount, float radius, float maxHeight) {
    if (amount    >= 0.0f) g_push       = amount;
    if (radius    >  0.0f) g_pushRadius = radius;
    if (maxHeight >  0.0f) g_pushMax    = maxHeight;
}
void SWSE_WindGetPush(float* amount, float* radius, float* maxHeight) {
    if (amount)    *amount    = g_push;
    if (radius)    *radius    = g_pushRadius;
    if (maxHeight) *maxHeight = g_pushMax;
}
float SWSE_WindGetWeight()    { return g_weight; }
void SWSE_WindGetParams(float* strength, float* speed) {
    if (strength) *strength = g_strength;
    if (speed)    *speed    = g_speed;
}
void SWSE_WindTest(int on) { g_testMode = on != 0; }

// Changing how the code is generated means the programs must be rewritten.
// Restoring first keeps the originals authoritative - patching a patched
// program would compound the edits.
void SWSE_WindAxis(int axis) {
    if (axis == g_upAxis) return;
    g_upAxis = axis ? 1 : 0;
    if (g_on) g_reinject = true;
}
int SWSE_WindGetAxis() { return g_upAxis; }

void SWSE_WindSeed(int mode) {
    if (mode == g_seedMode) return;
    g_seedMode = mode ? 1 : 0;
    if (g_on) g_reinject = true;
}
int SWSE_WindGetSeed() { return g_seedMode; }

void SWSE_WindGateEnable(int on) {
    g_useGate = on != 0;
    if (!g_useGate) g_gateOpen = false;
}
int SWSE_WindGateEnabled() { return g_useGate ? 1 : 0; }

void SWSE_WindPerf(double* injectMs, double* injectMax,
                   double* checkMs, double* checkMax, int* stalls) {
    if (injectMs)  *injectMs  = g_msInject;
    if (injectMax) *injectMax = g_msInjectMax;
    if (checkMs)   *checkMs   = g_msCheck;
    if (checkMax)  *checkMax  = g_msCheckMax;
    if (stalls)    *stalls    = g_stalls;
}

void SWSE_WindStats(int* injected, int* failed, int* on, float* curX, float* curZ) {
    if (injected) *injected = g_injected;
    if (failed)   *failed   = g_failed;
    if (on)       *on       = g_on ? 1 : 0;
    if (curX)     *curX     = g_curX;
    if (curZ)     *curZ     = g_curZ;
}

void SWSE_WindGate(int isFoliage, int noPush, int swayPct) {
    if (!g_on || !g_useGate || !p_ProgramEnvParameter4fARB) return;
    bool want = isFoliage != 0;
    bool np   = noPush != 0;
    if (swayPct < 0)   swayPct = 0;
    if (swayPct > 100) swayPct = 100;
    // The sway scale joins the change test: without it, a tree drawn after a
    // grass clump would keep the grass's amplitude, because the gate state
    // would look unchanged.
    if (want == g_gateOpen && np == g_gateNoPush && swayPct == g_gateSway) return;
    g_gateOpen = want;
    g_gateNoPush = np;
    g_gateSway = swayPct;
    const float sway = (float)swayPct * 0.01f;
    // Push strength is per-draw: a plant flagged nopush still sways, it is
    // just not shoved aside by the player.
    p_ProgramEnvParameter4fARB(GL_VERTEX_PROGRAM_ARB, 2,
                               (want && !np && g_havePlayer) ? g_push : 0.0f,
                               (g_pushMax > 0.001f) ? 1.0f / g_pushMax : 1.0f,
                               0.0f, 0.0f);
    // The phase is carried in .w even when closed: only the xz amplitude is
    // zeroed, so nothing else moves, and plants do not jump when the gate
    // reopens on a different phase.
    if (want) p_ProgramEnvParameter4fARB(GL_VERTEX_PROGRAM_ARB, 0,
                                         g_curX * sway, g_invWeight, g_curZ * sway, g_phase);
    else      p_ProgramEnvParameter4fARB(GL_VERTEX_PROGRAM_ARB, 0, 0.0f, g_invWeight, 0.0f, g_phase);
}

void SWSE_WindFrame() {
    if (g_pending == -1) {
        RestoreAll();
        g_on = false;
        g_pending = 0;
        LogW("wind: OFF (programs restored)");
        return;
    }
    if (g_pending == 1) {
        g_pending = 0;
        if (!Resolve()) { LogW("wind: ARB program entry points unavailable"); return; }
        // Turn on regardless of how many programs exist RIGHT NOW.
        //
        // Enabling from wind.txt happens on the first frame, before any plant
        // has been drawn, so nothing is discoverable yet and this first
        // InjectAll always finds zero. Making g_on depend on that left wind
        // permanently off at launch - and because the per-frame catch-up sits
        // behind `if (!g_on) return`, it could never recover. ON is the user's
        // intent; injection then happens as each plant type appears.
        g_on = true;
        InjectAll();
    }

    if (!g_on) return;

    // Regenerate after an axis/seed change.
    if (g_reinject) {
        g_reinject = false;
        RestoreAll();
        InjectAll();
    }

    // Foliage programs are discovered as the player walks past new plant
    // types, so the set GROWS during play. Without this, whichever plants were
    // not on screen when wind was switched on would stay frozen for the rest
    // of the session - which is exactly why the dandelions and ferns did not
    // move.
    unsigned probe[MAX_PROGS];
    if (SWSE_FoliagePrograms(probe, MAX_PROGS) > g_triedN) InjectAll();

    // Loading a level recreates the ARB programs, silently reverting every
    // injection. Checked rather than hooked: a marker read-back every couple
    // of seconds costs nothing and needs no knowledge of how the engine
    // signals a level change.
    unsigned now = GetTickCount();
    if (now >= g_nextCheck) {
        g_nextCheck = now + 2000;
        double c0 = NowMs();
        bool live = InjectionStillLive();
        g_msCheck = NowMs() - c0;
        if (g_msCheck > g_msCheckMax) g_msCheckMax = g_msCheck;
        if (g_msCheck > 30.0) {
            g_stalls++;
            char b[160];
            wsprintfA(b, "WINDSTALL: the 2s revert-check took %d ms (GL sync)",
                      (int)g_msCheck);
            LogW(b);
        }
        if (!live) {
            LogW("wind: programs reverted (level load) - re-injecting");
            DropSaved();        // originals belong to destroyed programs
            InjectAll();
        }
    }

    // One wind DIRECTION, with the oscillation done per-plant in the shader.
    // The CPU supplies direction*amplitude plus a running phase; the shader
    // adds each plant's own offset to that phase. A slow gust keeps the
    // strength from being metronomic.
    float t = (float)GetTickCount() * 0.001f;
    g_phase = t * 0.25f * g_speed;                  // in cycles; FRC wraps it
    float gust = 0.78f + 0.22f * sinf(t * 0.23f);
    float amp = (g_testMode ? 0.25f : g_strength) * gust;
    g_curX = amp * 0.88f;                           // fixed prevailing direction
    g_curZ = amp * 0.47f;

    // Where the player is, for the push. Read every frame - it has to track
    // movement, and this is one call, not per draw.
    float p[3] = { 0, 0, 0 };
    if (SWSE_PosGet(p) == 1) {
        g_px = p[0]; g_py = p[1]; g_pz = p[2];
        g_havePlayer = 1;
    }
    if (p_ProgramEnvParameter4fARB) {
        float invR2 = (g_pushRadius > 0.01f)
                    ? 1.0f / (g_pushRadius * g_pushRadius) : 1.0f;
        p_ProgramEnvParameter4fARB(GL_VERTEX_PROGRAM_ARB, 1, g_px, g_py, g_pz, invR2);
        // Push off entirely until the player position is known, so plants are
        // never shoved away from the world origin.
        float invPushMax = (g_pushMax > 0.001f) ? 1.0f / g_pushMax : 1.0f;
        // Honour the current gate: refreshing this unconditionally would push
        // whatever was drawn last, including a tree.
        bool allow = g_havePlayer && !(g_useGate && g_gateNoPush);
        p_ProgramEnvParameter4fARB(GL_VERTEX_PROGRAM_ARB, 2,
                                   allow ? g_push : 0.0f, invPushMax, 0.0f, 0.0f);
    }

    // Ungated: the parameter simply carries the live wind every frame, and
    // only the injected programs read it. Gated: the value still has to be
    // refreshed while the gate is open, or plants freeze at whatever the wind
    // was when their texture was bound.
    //
    // The per-draw sway scale must be reapplied here. Refreshing at full
    // amplitude would undo it for whichever plant was bound last, so a tree
    // left on screen at frame end would swing at grass strength.
    if ((!g_useGate || g_gateOpen) && p_ProgramEnvParameter4fARB) {
        float sway = g_useGate ? (float)g_gateSway * 0.01f : 1.0f;
        p_ProgramEnvParameter4fARB(GL_VERTEX_PROGRAM_ARB, 0,
                                   g_curX * sway, g_invWeight, g_curZ * sway, g_phase);
    }
}
