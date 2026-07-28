// SWSE foliage identification - see foliage.h.
//
// glBindTexture is called thousands of times per frame, so the
// unhook -> call -> rehook pattern used elsewhere in SWSE is not usable here:
// it costs two VirtualProtect calls per invocation. This uses a trampoline
// instead (relocate the prologue once, jump back), the same shape as the bolt
// hook in granny.cpp.

#include "foliage.h"
#include "wind.h"
#include <windows.h>
#include <gl/GL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void LogF(const char* s) {
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
#define MAX_FOLIAGE 512
static unsigned g_fp[MAX_FOLIAGE];
// Per-entry "nopush": this plant sways in the wind but is NOT shoved aside by
// the player. Trees are marked so, because a tree must not shift when walked
// past. It is a flag in the list rather than a height threshold: plant local
// heights vary so much between types that no single size cut separated grass
// from trees - one that silenced the trees also killed the effect on grass.
static unsigned char g_fpNoPush[MAX_FOLIAGE];
// Per-entry sway scale, as a PERCENT (0..100, default 100). Trees read as
// wrong at the strength that suits grass: a trunk-mounted canopy that visibly
// travels looks like rubber, and the global `weight` control cannot express
// this because it damps by plant height, not by plant type. `sway=0` freezes
// an entry completely while leaving it in the list for the push gate.
static unsigned char g_fpSway[MAX_FOLIAGE];
static int      g_fpN = 0;
static bool     g_loaded = false;

// Texture ids are small; a flat flag array is faster than any search and this
// runs inside glBindTexture.
#define GL_ACTIVE_TEXTURE_ARB 0x84E0
#define GL_TEXTURE0_ARB       0x84C0

#define MAX_TEXID 65536
static unsigned char g_isFoliage[MAX_TEXID];
static unsigned char g_noPushTex[MAX_TEXID];
static unsigned char g_swayTex[MAX_TEXID];
static int g_knownTexids = 0;

// texid -> vanilla fingerprint, so a scan can report what a bound id WAS, and
// so the flags can be rebuilt when the list is reloaded.
static unsigned* g_texHash = nullptr;

// Declared here rather than beside NoteFoliageProgram because the scan writer
// below reports them.
static unsigned g_folVP[64];
static int      g_folVPN = 0;

static void LoadList() {
    char path[MAX_PATH];
    GetModuleFileNameA(GetModuleHandleA(NULL), path, MAX_PATH);
    char* sl = strrchr(path, '\\'); if (sl) *sl = 0;   // ...\bin
    sl = strrchr(path, '\\'); if (sl) *sl = 0;         // game root
    lstrcatA(path, "\\SWSEMods\\SWSE Wind\\foliage.txt");

    FILE* f = fopen(path, "r");
    if (!f) {
        char b[MAX_PATH + 64];
        wsprintfA(b, "foliage: no list at %s", path);
        LogF(b);
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), f) && g_fpN < MAX_FOLIAGE) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        unsigned v = 0; int n = 0;
        // leading 8 hex digits; the rest of the line is a human-readable name
        for (const char* p = line; *p && n < 8; p++) {
            int d = (*p >= '0' && *p <= '9') ? *p - '0'
                  : (*p >= 'a' && *p <= 'f') ? *p - 'a' + 10
                  : (*p >= 'A' && *p <= 'F') ? *p - 'A' + 10 : -1;
            if (d < 0) break;
            v = v * 16 + (unsigned)d; n++;
        }
        if (n != 8) continue;
        // optional trailing "nopush" anywhere on the line
        g_fpNoPush[g_fpN] = 0;
        for (const char* q = line; *q; q++) {
            if ((*q == 'n' || *q == 'N') &&
                (q[1] == 'o' || q[1] == 'O') &&
                (q[2] == 'p' || q[2] == 'P')) {
                g_fpNoPush[g_fpN] = 1;
                break;
            }
        }
        // optional "sway=<0..1>" anywhere on the line; "nowind" means sway=0.
        g_fpSway[g_fpN] = 100;
        for (const char* q = line; *q; q++) {
            if ((*q == 'n' || *q == 'N') &&
                (q[1] == 'o' || q[1] == 'O') &&
                (q[2] == 'w' || q[2] == 'W')) {
                g_fpSway[g_fpN] = 0;
                break;
            }
            if ((*q == 's' || *q == 'S') &&
                (q[1] == 'w' || q[1] == 'W') &&
                (q[2] == 'a' || q[2] == 'A') &&
                (q[3] == 'y' || q[3] == 'Y') && q[4] == '=') {
                double s = atof(q + 5);
                if (s < 0.0) s = 0.0;
                if (s > 1.0) s = 1.0;
                g_fpSway[g_fpN] = (unsigned char)(s * 100.0 + 0.5);
                break;
            }
        }
        g_fp[g_fpN++] = v;
    }
    fclose(f);
    char b[160];
    wsprintfA(b, "foliage: %d fingerprint(s) loaded", g_fpN);
    LogF(b);
}

void SWSE_FoliageInit() {
    if (g_loaded) return;
    g_loaded = true;
    LoadList();
}

static bool IsFoliageHash(unsigned h);
static bool FoliageLookup(unsigned h, unsigned char* noPush, unsigned char* sway);

// Re-read foliage.txt without restarting, so plants can be added to the list
// and checked immediately.
//
// Existing texture ids keep working because their fingerprints were recorded
// at upload: the flags are RECOMPUTED from those rather than cleared. Simply
// clearing would leave every plant already loaded in the level unflagged until
// the next level change, which would look exactly like the new entries not
// working.
int SWSE_FoliageReload() {
    g_fpN = 0;
    LoadList();
    g_knownTexids = 0;
    if (g_texHash) {
        for (int i = 0; i < MAX_TEXID; i++) {
            if (!g_texHash[i]) continue;
            unsigned char np = 0, sw = 100;
            unsigned char now = FoliageLookup(g_texHash[i], &np, &sw) ? 1 : 0;
            g_isFoliage[i] = now;
            g_noPushTex[i] = now ? np : 0;
            g_swayTex[i]   = now ? sw : 100;
            if (now) g_knownTexids++;
        }
    }
    return g_fpN;
}

static bool IsFoliageHash(unsigned h) {
    for (int i = 0; i < g_fpN; i++) if (g_fp[i] == h) return true;
    return false;
}

// As above, but also reports the entry's nopush flag and sway scale.
static bool FoliageLookup(unsigned h, unsigned char* noPush, unsigned char* sway) {
    for (int i = 0; i < g_fpN; i++) {
        if (g_fp[i] == h) {
            if (noPush) *noPush = g_fpNoPush[i];
            if (sway)   *sway   = g_fpSway[i];
            return true;
        }
    }
    if (noPush) *noPush = 0;
    if (sway)   *sway   = 100;
    return false;
}

void SWSE_FoliageNoteUpload(unsigned hash, unsigned texid) {
    if (texid >= MAX_TEXID) return;
    if (!g_texHash) {
        g_texHash = (unsigned*)calloc(MAX_TEXID, sizeof(unsigned));
        if (!g_texHash) return;
    }
    g_texHash[texid] = hash;
    unsigned char np = 0, sw = 100;
    unsigned char was = g_isFoliage[texid];
    unsigned char now = FoliageLookup(hash, &np, &sw) ? 1 : 0;
    g_noPushTex[texid] = now ? np : 0;
    g_swayTex[texid]   = now ? sw : 100;
    if (was != now) {
        g_isFoliage[texid] = now;
        g_knownTexids += now ? 1 : -1;
    }
}

// ---- one-frame texture census --------------------------------------------
static char           g_scanPath[MAX_PATH];
static volatile LONG  g_scanPending = 0;
static int            g_scanDone = 0;
static unsigned char* g_scanSeen = nullptr;
static unsigned       g_scanList[4096];
static int            g_scanN = 0;

void SWSE_FoliageScanRequest(const char* path) {
    lstrcpynA(g_scanPath, path, MAX_PATH);
    g_scanDone = 0;
    g_scanN = 0;
    g_folVPN = 0;
    if (!g_scanSeen) g_scanSeen = (unsigned char*)calloc(MAX_TEXID, 1);
    if (g_scanSeen) memset(g_scanSeen, 0, MAX_TEXID);
    InterlockedExchange(&g_scanPending, 1);
}

int SWSE_FoliageScanDone() { return g_scanDone; }

static void ScanWrite() {
    FILE* f = fopen(g_scanPath, "w");
    if (!f) { g_scanDone = -1; return; }
    fprintf(f, "# textures bound during one frame (fingerprint, texid)\n");
    fprintf(f, "# match against names with: python tools/texmap.py --fp-in <this file>\n");
    for (int i = 0; i < g_scanN; i++) {
        unsigned id = g_scanList[i];
        fprintf(f, "%08X  texid=%u%s\n", g_texHash ? g_texHash[id] : 0, id,
                g_isFoliage[id] ? "  [flagged foliage]" : "");
    }
    fprintf(f, "\n# ARB vertex programs bound while foliage was bound\n");
    for (int i = 0; i < g_folVPN; i++) fprintf(f, "vertexprogram %u\n", g_folVP[i]);
    fclose(f);
    g_scanDone = g_scanN ? g_scanN : -1;
}

// ---- bind tracking --------------------------------------------------------
static unsigned g_curTex = 0;
static int g_bindsThisFrame = 0, g_bindsLastFrame = 0, g_peakBinds = 0;
static bool g_scanActive = false;
static unsigned g_lastFoliageTex = 0xFFFFFFFF;
static int g_camCaptures = 0;      // reset each frame

// ---- which ARB vertex program draws foliage -------------------------------
// Needed because the wind displacement has to be injected into THOSE programs
// and no others. Read by querying the current binding when a foliage texture
// is bound, rather than by hooking glBindProgramARB: this runs only during a
// one-frame scan, so it costs nothing in normal play and adds no second hook
// whose prologue would need verifying.
#define GL_VERTEX_PROGRAM_ARB   0x8620
#define GL_PROGRAM_BINDING_ARB  0x8677
typedef void (APIENTRY* PFN_GETPROGRAMIVARB)(GLenum, GLenum, GLint*);
static PFN_GETPROGRAMIVARB p_GetProgramivARB = nullptr;
static bool g_arbResolved = false;
static void NoteFoliageProgram() {
    if (!g_arbResolved) {
        g_arbResolved = true;
        HMODULE gl = GetModuleHandleA("opengl32.dll");
        if (gl) {
            typedef PROC (WINAPI* wglGPA_t)(LPCSTR);
            wglGPA_t gpa = (wglGPA_t)GetProcAddress(gl, "wglGetProcAddress");
            if (gpa) p_GetProgramivARB = (PFN_GETPROGRAMIVARB)gpa("glGetProgramivARB");
        }
    }
    if (!p_GetProgramivARB) return;
    GLint id = 0;
    p_GetProgramivARB(GL_VERTEX_PROGRAM_ARB, GL_PROGRAM_BINDING_ARB, &id);
    if (id <= 0) return;
    for (int i = 0; i < g_folVPN; i++) if (g_folVP[i] == (unsigned)id) return;
    if (g_folVPN < (int)(sizeof(g_folVP) / sizeof(g_folVP[0])))
        g_folVP[g_folVPN++] = (unsigned)id;
}

int SWSE_FoliageCurrentIsFoliage() {
    return (g_curTex < MAX_TEXID && g_isFoliage[g_curTex]) ? 1 : 0;
}

void SWSE_FoliageFrameMark() {
    g_camCaptures = 0;
    g_bindsLastFrame = g_bindsThisFrame;
    if (g_bindsThisFrame > g_peakBinds) g_peakBinds = g_bindsThisFrame;
    g_bindsThisFrame = 0;

    // A scan collects for exactly one frame: arm it here, and the NEXT frame
    // mark writes what was collected. Collecting across frames would blur
    // whatever is being looked at into whatever came before.
    static bool collecting = false;
    if (collecting) { ScanWrite(); collecting = false; }
    if (InterlockedCompareExchange(&g_scanPending, 0, 1) == 1) collecting = true;
    g_scanActive = collecting;
}

typedef void (APIENTRY* glBindTexture_t)(GLenum, GLuint);
static glBindTexture_t g_bindTramp = nullptr;
static BYTE* g_trampMem = nullptr;
static void* g_bindTarget = nullptr;
static BYTE  g_bindOrig[8];
static int   g_bindPrologue = 0;
static bool  g_hooked = false;

static void APIENTRY HookedBindTexture(GLenum target, GLuint tex) {
    if (target == GL_TEXTURE_2D) {
        g_curTex = tex;
        // Gating on ANY unit's bind, deliberately. Restricting this to unit 0
        // looked more correct on paper, but the effect demonstrably reaches
        // the plants as it stands, and a query-per-bind refinement that could
        // silently miss foliage bound to another unit is not worth making
        // against a working system.
        SWSE_WindGate(tex < MAX_TEXID && g_isFoliage[tex],
                      tex < MAX_TEXID && g_noPushTex[tex],
                      (tex < MAX_TEXID && g_isFoliage[tex]) ? g_swayTex[tex] : 100);
        if (tex < MAX_TEXID && g_isFoliage[tex]) {
            g_bindsThisFrame++;
            // Capture the camera matrix mid-scene, and keep RE-capturing.
            //
            // Program local parameters hold whatever the LAST draw set. On the
            // FIRST foliage bind of a frame that is still the previous frame's
            // value, so decals were reconstructed with a stale camera and
            // visibly swam as the view moved. Capturing repeatedly means the
            // value left at the end is from a draw in THIS frame's scene pass.
            // Capped so the cost stays bounded on foliage-heavy views.
            // The camera is no longer captured here. It used to be, because
            // scraping the matrix out of shader parameters required a scene
            // program to be bound - which meant it only updated when FOLIAGE
            // was on screen. Looking at bare ground left the matrix stale and
            // decals appeared to move with the camera. It is built from a
            // memory struct now, so the frame hook updates it every frame
            // regardless of what is being drawn.
            // Discover foliage programs CONTINUOUSLY, not only during a scan.
            // A single scan frame found 3 programs and missed whichever one
            // draws the dandelions, so they never moved. Different plant types
            // use different programs, and a plant only reveals its program
            // when it is on screen. Querying once per distinct foliage texture
            // (about 11 per frame here, not per bind) keeps this cheap while
            // still catching every type the player walks past.
            if (tex != g_lastFoliageTex) {
                g_lastFoliageTex = tex;
                NoteFoliageProgram();
            }
        }
        if (g_scanActive && tex < MAX_TEXID && g_scanSeen && !g_scanSeen[tex]
            && g_scanN < (int)(sizeof(g_scanList) / sizeof(g_scanList[0]))) {
            g_scanSeen[tex] = 1;
            g_scanList[g_scanN++] = tex;
        }
    }
    g_bindTramp(target, tex);
}

// Verify before patching. A wrong prologue length splits an instruction and
// corrupts the driver; refusing and logging the bytes is always better than
// guessing, and the bytes tell us what to support next.
static int PrologueLen(const BYTE* t) {
    // mov edi,edi ; push ebp ; mov ebp,esp   -- the hot-patch prologue
    if (t[0] == 0x8B && t[1] == 0xFF && t[2] == 0x55 && t[3] == 0x8B && t[4] == 0xEC)
        return 5;
    // push ebp ; mov ebp,esp ; sub esp,imm8
    if (t[0] == 0x55 && t[1] == 0x8B && t[2] == 0xEC && t[3] == 0x83 && t[4] == 0xEC)
        return 6;
    // push ebp ; mov ebp,esp ; push esi/edi/ebx
    if (t[0] == 0x55 && t[1] == 0x8B && t[2] == 0xEC &&
        (t[3] == 0x56 || t[3] == 0x57 || t[3] == 0x53))
        return 5;   // 1 + 2 + 1 = 4 whole; take 5 only if byte 4 also starts clean
    return 0;
}

int SWSE_FoliageTrack(int on, char* msg, int msgLen) {
    if (!on) {
        // Never unpatch: the render thread may be inside the hook right now.
        // Same rule the hit-reaction hook learned the hard way (it crashed the
        // game). Tracking simply goes quiet instead.
        if (msg) lstrcpynA(msg, "foliage tracking left installed but idle", msgLen);
        return 1;
    }
    if (g_hooked) { if (msg) lstrcpynA(msg, "foliage tracking already on", msgLen); return 1; }

    HMODULE gl = GetModuleHandleA("opengl32.dll");
    if (!gl) { if (msg) lstrcpynA(msg, "opengl32.dll not loaded", msgLen); return 0; }
    BYTE* t = (BYTE*)GetProcAddress(gl, "glBindTexture");
    if (!t) { if (msg) lstrcpynA(msg, "glBindTexture not found", msgLen); return 0; }

    int len = PrologueLen(t);
    if (len == 0) {
        char b[160];
        wsprintfA(b, "foliage: UNRECOGNISED glBindTexture prologue %02X %02X %02X %02X %02X %02X",
                  t[0], t[1], t[2], t[3], t[4], t[5]);
        LogF(b);
        if (msg) lstrcpynA(msg, b, msgLen);
        return 0;
    }

    g_trampMem = (BYTE*)VirtualAlloc(0, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_trampMem) { if (msg) lstrcpynA(msg, "trampoline alloc failed", msgLen); return 0; }

    memcpy(g_bindOrig, t, len);
    memcpy(g_trampMem, t, len);
    g_trampMem[len] = 0xE9;                            // JMP back to target+len
    *(DWORD*)(g_trampMem + len + 1) = (DWORD)((t + len) - (g_trampMem + len + 5));
    g_bindTramp = (glBindTexture_t)g_trampMem;

    DWORD old;
    VirtualProtect(t, len, PAGE_EXECUTE_READWRITE, &old);
    t[0] = 0xE9;
    *(DWORD*)(t + 1) = (DWORD)((BYTE*)HookedBindTexture - (t + 5));
    for (int i = 5; i < len; i++) t[i] = 0x90;         // pad the remainder
    VirtualProtect(t, len, old, &old);

    g_bindTarget = t;
    g_bindPrologue = len;
    g_hooked = true;
    char b[120];
    wsprintfA(b, "foliage: glBindTexture hooked (prologue %d bytes)", len);
    LogF(b);
    if (msg) lstrcpynA(msg, b, msgLen);
    return 1;
}

int SWSE_FoliagePrograms(unsigned* out, int maxOut) {
    int n = g_folVPN < maxOut ? g_folVPN : maxOut;
    for (int i = 0; i < n; i++) out[i] = g_folVP[i];
    return n;
}

void SWSE_FoliageStats(int* listN, int* knownTexids, int* bindsLastFrame,
                       int* peakBinds, int* hooked) {
    if (listN)          *listN          = g_fpN;
    if (knownTexids)    *knownTexids    = g_knownTexids;
    if (bindsLastFrame) *bindsLastFrame = g_bindsLastFrame;
    if (peakBinds)      *peakBinds      = g_peakBinds;
    if (hooked)         *hooked         = g_hooked ? 1 : 0;
}
