// SWSE - Granny 3D animation layer: additive hit reactions.
//
// THE GOAL
//   When a projectile hits an NPC, nudge the bone nearest the impact by a small
//   amount and let it decay over ~200ms. No "hurt left arm" animation has to
//   exist: the offset is ADDITIVE on top of whatever the character is already
//   doing, so it works while they walk, aim or idle. It is feedback, not a
//   reaction animation.
//
// WHY THIS IS POSSIBLE
//   The game uses RAD's Granny 3D (38 functions reference granny_*.cpp sources:
//   local_pose, world_pose, track_mask, controlled_animation, animation_binding).
//   Granny's pipeline has exactly the seam we need:
//
//       SampleModelAnimations -> granny_local_pose   (per-bone local transforms)
//                                   ^^^ we insert here
//       BuildWorldPose        -> granny_world_pose   (composed world matrices)
//                             -> skinning / render
//
//   Perturbing a bone in the LOCAL pose means BuildWorldPose composes the
//   hierarchy for us, so nudging an upper arm carries the forearm and hand with
//   it automatically. That is the whole feature.
//
// WHY THIS FILE SCANS INSTEAD OF HOOKING
//   The obvious move is to inline-hook granny_world_pose (0x6AD060) and grab the
//   local pose from its arguments. But a 5-byte JMP patch has to land on whole
//   instruction boundaries, and we have no disassembler for that address -
//   guessing is precisely what crashed the early graphics pass earlier today.
//
//   granny_transform has a very checkable shape instead, so we can FIND poses
//   read-only and never patch a byte:
//
//       uint32 Flags;              +0x00
//       float  Position[3];        +0x04
//       float  Orientation[4];     +0x10   <- UNIT QUATERNION: x2+y2+z2+w2 == 1
//       float  ScaleShear[3][3];   +0x20   <- normally the identity matrix
//                                  = 0x44 (68 bytes)
//
//   A run of these, with normalised quaternions and identity scale-shear, is an
//   extremely distinctive pattern - the same value-signature approach that
//   located the camera frustum.

#include <windows.h>
#include <cmath>
#include <stdio.h>      // settings file I/O
#include <string.h>
#include "granny.h"
#include "scriptvm.h"   // SWSE_FindNpcs / SWSE_PosGet, for the damage watch

// MEASURED, not taken from Granny's documented granny_transform (0x44 with the
// quaternion at +0x10). Those constants were wrong for this build, and because
// writing at a wrong offset still moves *a* bone, the mistake looked like
// success for a long time - it is why reactions never read as a clean flinch
// and why bone-position composition produced NaN.
//
// 'hitreact layout' scans a live pose for unit quaternions. Their spacing
// alternates 0x1C / 0x14, i.e. a real quaternion once per 0x30 record plus one
// false positive straddling the boundary. Confirmed against raw memory: with
// these constants record 0 reads position (0,0,0) and orientation (0,0,0,1) -
// an exact identity root, which is what a root bone must be.
//
//   pose + 0x10 + b*0x30 + 0x0C   position   (3 floats)
//   pose + 0x10 + b*0x30 + 0x18   orientation (x,y,z,w)
// Runtime-switchable, because there is a contradiction to settle: the ORIGINAL
// (wrong) constants 0x00/0x44/0x10 produced dramatic visible deformation, while
// these measured ones produce none. Both cannot drive the mesh. The measured
// layout is certainly a real structure - identity root, unit quaternions at a
// regular stride - but "this structure is real" is not "this structure is what
// gets drawn". Being able to flip between them live is the only way to tell.
// SETTLED BY EXPERIMENT. Rotating every bone by 2 rad under each candidate and
// photographing the result: the 0x44 layout stretches the character across the
// screen, the 0x30 layout leaves it untouched. The mesh is driven by
//
//     base 0x00, stride 0x44, position +0x04, orientation +0x10
//
// which is Granny's documented granny_transform after all. The 0x30-stride
// structure IS present in the same buffer and is internally consistent - an
// identity root and unit quaternions at a regular stride - which is precisely
// why it was so convincing. Being a real structure is not the same as being the
// structure that gets drawn, and only a render-visible A/B could tell them
// apart. Offsets were never the bug here; compounding writes were.
// CORRECTED DEFAULTS. These were 0x00/0x44/0x04/0x10 - Granny's documented
// granny_transform - and that is wrong for this build in a way that looked
// right: with base 0 and stride 0x44, bone 0's "orientation" lands at +0x10,
// which is INSIDE the pose header, on top of the pointer to the transform
// array. It was corrupting the structure, not rotating a bone, and corruption
// is far more visible than a correct subtle rotation - which is exactly why it
// won a render-visible A/B test and why arms kept mangling.
//
// The real layout, confirmed by bone 0 reading position (0,0,0) and orientation
// (0,0,0,1) - an exact identity root, the one value that could not be a
// coincidence:
//     header 0x10 bytes, then transforms every 0x30
//     position +0x0C, orientation +0x18 (x,y,z,w)
static int g_gtBase   = 0x10;
static int g_gtSize   = 0x30;
static int g_gtPos    = 0x0C;
static int g_gtOrient = 0x18;

#define GT_BASE      g_gtBase
#define GT_SIZE      g_gtSize
#define GT_POSITION  g_gtPos
#define GT_ORIENT    g_gtOrient
#define GT_SCALE     0x28      // single scale float, not a 3x3 scale-shear

// Upper bound on bones in one pose, for the fixed buffers used when walking a
// skeleton (largest seen so far is 64).
#define WPOS_MAX 128

void SWSE_HitReactSetOffsets(int base, int stride, int pos, int orient) {
    if (base   >= 0) g_gtBase   = base;
    if (stride >  0) g_gtSize   = stride;
    if (pos    >= 0) g_gtPos    = pos;
    if (orient >= 0) g_gtOrient = orient;
}
void SWSE_HitReactGetOffsets(int* base, int* stride, int* pos, int* orient) {
    if (base)   *base   = g_gtBase;
    if (stride) *stride = g_gtSize;
    if (pos)    *pos    = g_gtPos;
    if (orient) *orient = g_gtOrient;
}

static void LogG(const char* s) {
    char path[MAX_PATH];
    GetModuleFileNameA(GetModuleHandleA(NULL), path, MAX_PATH);
    char* sl = strrchr(path, '\\');
    if (sl) *sl = 0;
    lstrcatA(path, "\\swse_granny.txt");
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           0, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    SetFilePointer(h, 0, 0, FILE_END);
    WriteFile(h, s, lstrlenA(s), &w, 0);
    WriteFile(h, "\r\n", 2, &w, 0);
    CloseHandle(h);
}

static bool g_strictScale = false;   // relaxed: see LooksLikeTransform
static bool g_wideScan    = false;   // Granny has its own allocator
static unsigned g_nearBase   = 0;    // targeted scan window (0 = whole range)
static unsigned g_nearRadius = 0;

static inline bool Finite(float f) {
    // NaN fails every comparison, so this is written to reject rather than admit.
    return (f == f) && (f < 3.0e38f) && (f > -3.0e38f);
}

// Does this address look like a 4x4 matrix (64 bytes, bottom row 0,0,0,1)?
//
// This exists because the first scan was fooled: a ROW OF A ROTATION MATRIX IS
// ALSO UNIT LENGTH, so a "unit quaternion" test matches matrices too. The dump
// gave it away - the value 0.999 migrated one slot per element, which is an
// array read at 68 bytes when its real stride is 64.
static bool LooksLikeMatrix4x4(const BYTE* p) {
    const float* m = (const float*)p;
    for (int i = 0; i < 16; i++) if (!Finite(m[i])) return false;
    // bottom row of an affine 4x4
    if (m[3] < -0.001f || m[3] > 0.001f) return false;
    if (m[7] < -0.001f || m[7] > 0.001f) return false;
    if (m[11] < -0.001f || m[11] > 0.001f) return false;
    if (m[15] < 0.999f || m[15] > 1.001f) return false;
    // the upper-left 3x3 should be a rotation: rows unit length
    for (int r = 0; r < 3; r++) {
        const float* row = m + r*4;
        float n = row[0]*row[0] + row[1]*row[1] + row[2]*row[2];
        if (!(n > 0.9f && n < 1.1f)) return false;
    }
    return true;
}

// Does this address look like a granny_transform?
static bool LooksLikeTransform(const BYTE* p) {
    // Flags is a small bitfield (HasPosition/HasOrientation/HasScaleShear), so
    // 0..7. This is the discriminator that separates a real transform from a
    // 4x4 matrix, whose first word is a rotation component and therefore a huge
    // integer when read as one. Without it the scan returns matrices.
    unsigned flags = *(const unsigned*)p;
    if (flags > 7) return false;

    const float* q = (const float*)(p + GT_ORIENT);
    for (int i = 0; i < 4; i++) if (!Finite(q[i])) return false;
    float n = q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3];
    if (!(n > 0.97f && n < 1.03f)) return false;        // unit quaternion

    const float* pos = (const float*)(p + GT_POSITION);
    for (int i = 0; i < 3; i++) {
        if (!Finite(pos[i])) return false;
        if (pos[i] > 10000.0f || pos[i] < -10000.0f) return false;
    }

    // ScaleShear check is OPTIONAL. Requiring a near-identity 3x3 found zero
    // candidates on the first run, and it is the weakest of the assumptions
    // here: the game may use non-uniform scale, or a Granny build with a
    // compressed transform. The unit quaternion is the strong signal, so the
    // scale-shear only has to be finite.
    // The measured 0x30 record has no 9-float scale-shear block; it carries a
    // single scale at +0x28. Check that instead.
    if (g_strictScale) {
        const float* sc = (const float*)(p + GT_SCALE);
        if (!Finite(sc[0])) return false;
        if (!(sc[0] > 0.2f && sc[0] < 5.0f)) return false;
    }
    return true;
}

// Which structure are we hunting this run?
static int g_mode = 0;               // 0 = granny_transform (68B), 1 = 4x4 (64B)

static inline int ModeStride() { return g_mode ? 64 : GT_SIZE; }
static inline bool ModeMatches(const BYTE* p) {
    return g_mode ? LooksLikeMatrix4x4(p) : LooksLikeTransform(p);
}

// How many consecutive elements start at p?
static int RunLength(const BYTE* p, const BYTE* limit) {
    int n = 0;
    int stride = ModeStride();
    while (p + stride <= limit && n < 512) {
        if (!ModeMatches(p)) break;
        p += stride;
        n++;
    }
    return n;
}

int SWSE_GrannyScan(unsigned* addrs, int* counts, int maxOut, int minBones,
                    int wide, int strictScale, int mode) {
    g_wideScan = (wide != 0);
    g_strictScale = (strictScale != 0);
    g_mode = mode;
    if (minBones < 4) minBones = 4;
    int found = 0;
    MEMORY_BASIC_INFORMATION mbi;
    // Default is the heap window every other scan uses. Bounded deliberately:
    // an unbounded walk of the address space is what froze the game for 131
    // seconds when the camera scan ran from the render path.
    //
    // But Granny allocates through granny_fixed_allocator, which need not put
    // poses in that window at all - so 'wide' opens it up when the narrow scan
    // comes back empty.
    BYTE* addr = g_wideScan ? (BYTE*)0x00010000 : (BYTE*)0x10000000;
    BYTE* end  = g_wideScan ? (BYTE*)0x7FFF0000 : (BYTE*)0x40000000;
    // Targeted window: the LOCAL pose for a character should sit near its WORLD
    // pose - same character, same allocator, usually the same allocation block.
    // Searching a few KB around a known world pose is far cheaper and far less
    // ambiguous than another full sweep.
    if (g_nearBase) {
        unsigned lo = (g_nearBase > g_nearRadius) ? (g_nearBase - g_nearRadius) : 0x10000;
        addr = (BYTE*)lo;
        end  = (BYTE*)(g_nearBase + g_nearRadius);
    }
    while (addr < end && found < maxOut) {
        if (!VirtualQuery(addr, &mbi, sizeof(mbi))) break;
        BYTE* base = (BYTE*)mbi.BaseAddress;
        SIZE_T size = mbi.RegionSize;
        bool usable = (mbi.State == MEM_COMMIT) &&
                      !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                      (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                                      PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
        if (usable && size >= (SIZE_T)ModeStride() * (SIZE_T)minBones) {
            __try {
                BYTE* stop = base + size - ModeStride();
                for (BYTE* p = base; p < stop; p += 4) {
                    if (!ModeMatches(p)) continue;
                    int run = RunLength(p, base + size);
                    if (run >= minBones) {
                        if (addrs)  addrs[found]  = (unsigned)(uintptr_t)p;
                        if (counts) counts[found] = run;
                        found++;
                        if (found >= maxOut) break;
                        p += (SIZE_T)run * ModeStride();   // skip past this run
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) { }
        }
        BYTE* next = base + size;
        if (next <= addr) break;
        addr = next;
    }
    return found;
}

void SWSE_GrannyDumpBones(unsigned poseAddr, int count, int stride, char* msg, int msgLen) {
    if (count > 8) count = 8;
    char line[300];
    wsprintfA(line, "=== pose %08X, first %d bone(s) ===", poseAddr, count);
    LogG(line);
    // A 4x4 keeps its translation in the bottom row (m[12..14]), not at +0x04,
    // so dumping a matrix run with transform offsets prints nonsense. Split it.
    if (stride == 64) {
        __try {
            for (int i = 0; i < count; i++) {
                const float* m = (const float*)((const BYTE*)(uintptr_t)poseAddr + (SIZE_T)i * 64);
                wsprintfA(line,
                    "bone %2d  T(%d.%02d, %d.%02d, %d.%02d)  R0(%d,%d,%d)/1000  R1(%d,%d,%d)/1000",
                    i,
                    (int)m[12], (int)((m[12]<0?-m[12]:m[12])*100)%100,
                    (int)m[13], (int)((m[13]<0?-m[13]:m[13])*100)%100,
                    (int)m[14], (int)((m[14]<0?-m[14]:m[14])*100)%100,
                    (int)(m[0]*1000), (int)(m[1]*1000), (int)(m[2]*1000),
                    (int)(m[4]*1000), (int)(m[5]*1000), (int)(m[6]*1000));
                LogG(line);
            }
            lstrcpynA(msg, "matrix dump written to bin\\swse_granny.txt", msgLen);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            lstrcpynA(msg, "FAULTED reading that address", msgLen);
        }
        return;
    }
    __try {
        for (int i = 0; i < count; i++) {
            const BYTE* p = (const BYTE*)(uintptr_t)poseAddr + (SIZE_T)i * stride;
            const float* pos = (const float*)(p + GT_POSITION);
            const float* q   = (const float*)(p + GT_ORIENT);
            float n = q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3];
            wsprintfA(line,
                "bone %2d  pos(%d.%03d, %d.%03d, %d.%03d)  quat(%d/1000,%d/1000,%d/1000,%d/1000) |q|2=%d/1000",
                i,
                (int)pos[0], (int)(pos[0]<0?-pos[0]:pos[0]*1000)%1000,
                (int)pos[1], (int)(pos[1]<0?-pos[1]:pos[1]*1000)%1000,
                (int)pos[2], (int)(pos[2]<0?-pos[2]:pos[2]*1000)%1000,
                (int)(q[0]*1000), (int)(q[1]*1000), (int)(q[2]*1000), (int)(q[3]*1000),
                (int)(n*1000));
            LogG(line);
        }
        lstrcpynA(msg, "bone dump written to bin\\swse_granny.txt", msgLen);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        lstrcpynA(msg, "FAULTED reading that address", msgLen);
    }
}

// Restrict the next scan to a window around 'base'. Used to find a character's
// LOCAL pose given its WORLD pose: the two live close together in memory.
void SWSE_GrannySetNear(unsigned base, unsigned radius) {
    g_nearBase = base;
    g_nearRadius = radius;
}

// ===========================================================================
//  ADDITIVE HIT REACTIONS
// ===========================================================================
// Hook granny's BuildWorldPose and, just before it composes the hierarchy, add
// a small decaying rotation to the bone that was hit. Because the LOCAL pose is
// perturbed, BuildWorldPose carries the change down the chain for free: nudge an
// upper arm and the forearm and hand follow.
//
// The call signature was captured from a REAL call with a one-shot hardware
// breakpoint (see ANIMATION.md), not guessed:
//
//   a0 = Skeleton     141C4390     <- identity: which character
//   a1 = FirstBone    0
//   a2 = BoneCount    0x3F = 63    <- matched the scanned skeleton exactly
//   a3 = LocalPose    514FD2F8     <- the write target
//   a4 = Offset4x4    (stack)
//
// The prologue was read the same way. NOTE the third instruction: the
// breakpoint dump only showed four bytes (55 8B EC 83) and it is tempting to
// finish it as 'sub esp, imm8'. Verified against the live image it is:
//   55        push ebp                  boundary @1
//   8B EC     mov ebp, esp              boundary @3
//   83 E4 F0  and esp, 0FFFFFFF0h       boundary @6
// i.e. a stack ALIGNMENT (SSE), not a frame allocation. Both encodings are
// three bytes so the boundary is @6 either way, but the install-time check
// verifies the real bytes instead of trusting the guess. A 5-byte JMP would
// split that instruction, so the trampoline relocates SIX bytes and jumps
// back to entry+6.

#define RVA_BUILD_WORLD_POSE 0x2AD060
#define PROLOGUE_LEN 6            // 55 | 8B EC | 83 E4 F0  -- whole instructions

static bool     g_reactOn      = false;
// Tuned against real play and signed off as "noticeable for sure". Defaults
// matter here: every restart resets these, and testing with the old values
// silently produced a system that appeared not to work at all.
static float    g_reactStrength = 1.2f;    // radians at full strength
static int      g_reactMs      = 360;      // decay time

// Vertical slack when matching an impact to a character. The pose origin and
// the actor origin do not coincide, and a character is tall, so height is a
// poor discriminator - horizontal distance does the real work.
#define REACT_Z_TOLERANCE 12.0f
// Impact points sit on the body itself, up to a full character height above the
// origin (measured extent is ~22 units), so head shots need more slack.
#define REACT_Z_IMPACT    28.0f

// Minimum bones for something to count as a character rather than a prop or a
// piece of ammo. See the note in ApplyReactions - this is what stops the
// crossbow's critters being treated as bodies.
// How far a reaction spreads down the chain toward the root, and how fast it
// weakens. 1 link reproduces the old single-bone behaviour.
static int   g_chainLinks   = 3;
static float g_chainFalloff = 0.6f;

// How much of the chest's rotation is cancelled at the arm roots. The arms hang
// off the chest, so without this a hit swings whatever the character is holding
// - in first person the crossbow visibly cants over, which reads as losing
// control of your aim rather than as taking a hit. 0 = arms ride the body,
// 1 = arms hold station.
// Envelope shape. attack = fraction of the duration spent rising to peak;
// overshoot = how far past rest the body swings on the way back.
static float g_easeAttack    = 0.18f;
static float g_easeOvershoot = 0.12f;
void SWSE_HitReactEase(float attack, float overshoot) {
    if (attack >= 0.0f)    g_easeAttack    = (attack > 0.8f) ? 0.8f : attack;
    if (overshoot >= 0.0f) g_easeOvershoot = (overshoot > 0.6f) ? 0.6f : overshoot;
}
void SWSE_HitReactEaseGet(float* attack, float* overshoot) {
    if (attack)    *attack    = g_easeAttack;
    if (overshoot) *overshoot = g_easeOvershoot;
}

// Minimum number of bones that must hang below the bone a reaction rotates.
// Below this the rotation is real but invisible - the whole reason hits on
// outlaws read as "barely noticeable" while the numbers looked healthy.
static int g_minLimbMass = 14;   // torso-weighted: the feel signed off in play
void SWSE_HitReactLimbMass(int n) { if (n >= 1) g_minLimbMass = n; }
int  SWSE_HitReactGetLimbMass()   { return g_minLimbMass; }

static float g_armDamp = 0.0f;   // arms ride the torso: the motion the user saw was vanilla
void SWSE_HitReactArmDamp(float d) {
    if (d < 0.0f) d = 0.0f;
    if (d > 1.0f) d = 1.0f;
    g_armDamp = d;
}
float SWSE_HitReactGetArmDamp() { return g_armDamp; }

// DOES THE POSE BUFFER PERSIST BETWEEN FRAMES?
// Everything about delta-vs-absolute hinges on this, and it has been guessed
// at twice. Measure it: remember the exact quaternion written last frame, and
// next frame compare what is there BEFORE writing again.
//   same as we wrote  -> the buffer persisted; a full write would compound
//   different         -> animation rebuilt it; a delta would drift forever
static float         g_probeLastWrite[4];
static unsigned      g_probeBone = 0xFFFFFFFF;
static volatile LONG g_poseSame = 0, g_poseDiff = 0;

void SWSE_HitReactPoseStats(unsigned* same, unsigned* diff) {
    if (same) *same = (unsigned)g_poseSame;
    if (diff) *diff = (unsigned)g_poseDiff;
}

// See the note at the delta/absolute branch in ApplyReactions.
static bool g_applyDelta = true;
void SWSE_HitReactApplyMode(int useDelta) { g_applyDelta = (useDelta != 0); }
int  SWSE_HitReactGetApplyMode()          { return g_applyDelta ? 1 : 0; }
void SWSE_HitReactChain(int links, float falloff) {
    if (links > 0)     g_chainLinks   = (links > SWSE_MAX_CHAIN) ? SWSE_MAX_CHAIN : links;
    if (falloff >= 0)  g_chainFalloff = falloff;
}
void SWSE_HitReactChainGet(int* links, float* falloff) {
    if (links)   *links   = g_chainLinks;
    if (falloff) *falloff = g_chainFalloff;
}


// Flatten the animation to bind pose, so a reaction is the only possible
// source of movement. Purely a diagnostic.
static bool g_tpose = false;
void SWSE_HitReactTPose(int on) { g_tpose = (on != 0); }
int  SWSE_HitReactTPoseOn()     { return g_tpose ? 1 : 0; }

// FREEZE: hold the animation at whatever pose it was in, and let reactions play
// on top. Better than a T-pose for judging the effect - the character keeps a
// natural stance, and the game's own hurt animation cannot mask or mimic our
// offset because no animation is advancing at all.
#define FREEZE_SKELS 16
struct FreezeCache {
    unsigned skel;
    int      bones;
    float    q[WPOS_MAX][4];
    bool     filled;
};
static FreezeCache   g_freeze[FREEZE_SKELS];
static int           g_freezeCount = 0;
static bool          g_freezeOn    = false;

void SWSE_HitReactFreeze(int on) {
    g_freezeOn = (on != 0);
    if (!on) { g_freezeCount = 0; }        // forget, so re-freezing recaptures
}
int SWSE_HitReactFreezeOn() { return g_freezeOn ? 1 : 0; }

// Capture on first sight, replay every frame after.
static void ApplyFreeze(unsigned skel, int boneCount, BYTE* localPose) {
    if (boneCount > WPOS_MAX) boneCount = WPOS_MAX;
    FreezeCache* fc = nullptr;
    for (int i = 0; i < g_freezeCount; i++)
        if (g_freeze[i].skel == skel) { fc = &g_freeze[i]; break; }
    if (!fc) {
        if (g_freezeCount >= FREEZE_SKELS) return;
        fc = &g_freeze[g_freezeCount++];
        fc->skel = skel; fc->bones = boneCount; fc->filled = false;
    }
    if (fc->bones != boneCount) return;                 // pose changed shape
    if (!fc->filled) {
        for (int b = 0; b < boneCount; b++) {
            const float* q = (const float*)(localPose + GT_BASE +
                                            (SIZE_T)b * GT_SIZE + GT_ORIENT);
            for (int c = 0; c < 4; c++) fc->q[b][c] = q[c];
        }
        fc->filled = true;
        return;                                          // first frame: as-is
    }
    for (int b = 0; b < boneCount; b++) {
        float* q = (float*)(localPose + GT_BASE + (SIZE_T)b * GT_SIZE + GT_ORIENT);
        for (int c = 0; c < 4; c++) q[c] = fc->q[b][c];
    }
}

// A small skeleton this close to the player is something they are carrying.
#define ATTACHED_PROP_RADIUS 2.5f

static int g_minReactBones = 12;
void SWSE_HitReactMinBones(int n) { if (n >= 0) g_minReactBones = n; }
int  SWSE_HitReactGetMinBones()   { return g_minReactBones; }

struct HitReact {
    unsigned skeleton;     // which character (a0), 0 = any
    // Characters are matched by WORLD POSITION rather than by identity. The
    // damage path knows an actor pointer; the hook knows a Granny skeleton.
    // Nothing maps between them - but the hook can read the character's world
    // position out of the a4 offset matrix (verified: it reproduced the
    // player's reported position exactly), and the damage path knows the
    // actor's position. Matching on proximity joins the two without needing a
    // table. usePos=0 keeps the old skeleton/any behaviour for testing.
    int      usePos;
    float    pos[3];
    float    radius;
    // A world-space impact point. When set, the bone is chosen by proximity to
    // this point instead of falling back to the torso - "shot in the arm moves
    // the arm". Supplied by whatever knows where the projectile landed.
    int      useImpact;
    float    impact[3];
    int      bone;         // bone index within the pose, <0 = auto (torso)
    DWORD    startedAt;
    float    strength;
    float    axis[3];      // rotation axis, from the impact direction
    bool     live;
    // The local pose buffer PERSISTS between frames - it is not rebuilt from
    // animation every frame as first assumed. Writing the full rotation each
    // frame therefore COMPOUNDS: a 0.15 rad flinch over ~15 frames of decay
    // accumulated toward 2 rad, which is why nothing ever looked subtle and
    // why held test offsets stretched characters into spikes.
    // Fix: apply only the change since last frame, and unwind on expiry so the
    // net rotation returns to exactly zero.
    float    lastK;
    bool     applied;
    // THE POSE BUFFER IS NOT CONSISTENT. Measured 5917 persisted vs 6994
    // rebuilt - roughly half and half - so a fixed policy is wrong half the
    // time: 'absolute' compounds on the frames that persisted, 'delta' drifts
    // on the frames that were rebuilt. Both produced the deformed arm.
    //
    // So detect it per bone, per frame. Remember exactly what was written and
    // what the animation's value was underneath it:
    //   current == what we wrote  -> nothing overwrote us; rebase on the saved
    //                                animation value so the result is never
    //                                compounded
    //   current != what we wrote  -> animation rebuilt the pose; that value IS
    //                                the new base
    // Either way the bone ends at exactly (base * rotation(k)), which is the
    // definition of an additive offset.
    float    baseQ[SWSE_MAX_CHAIN][4];   // animation value under our write
    float    wroteQ[SWSE_MAX_CHAIN][4];  // what we last wrote
    bool     haveBase[SWSE_MAX_CHAIN];
    unsigned appliedSkel;  // bind to one character, so the unwind lands on the
    int      appliedBone;  // same bones even if it moved out of range
    int      logSlot;      // hit-log entry to annotate once the bone is known

};
#define MAX_REACTS SWSE_MAX_REACTS
static HitReact g_reacts[MAX_REACTS];

// Quaternion multiply: q = a * b, layout (x,y,z,w) as granny stores it.
static void QuatMul(const float* a, const float* b, float* out) {
    out[0] = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
    out[1] = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
    out[2] = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
    out[3] = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
}

// Live instrumentation. A pixel diff cannot tell "the hook moved a bone" apart
// from "the NPC was walking", so the hook counts what it actually did and
// 'hitreact' reports it. g_hookCalls proves the patch is on the executed path;
// g_bonesWritten proves a reaction reached a quaternion.
static volatile LONG g_hookCalls   = 0;
static volatile LONG g_bonesWritten = 0;
// Where a pending reaction gets dropped. Without this the only symptom is
// "bones written is lower than expected", which does not say why.
static volatile LONG g_posMatch  = 0;   // position test passed
static volatile LONG g_posReject = 0;   // character was elsewhere
static volatile LONG g_noMatrix  = 0;   // a4 offset matrix unreadable
static volatile LONG g_torsoFail = 0;   // could not identify a torso bone
static volatile LONG g_boneRange = 0;   // bone outside this pose's range
static volatile LONG g_faulted        = 0;  // times the hook body faulted
static volatile LONG g_impactResolved = 0;  // impact point -> bone succeeded
static volatile LONG g_impactFail     = 0;  // could not compose a bone cloud
static int           g_lastImpactBone = -1;
static float         g_lastImpactDist = 0.0f;
static unsigned      g_lastSkeleton = 0;
static int           g_lastBoneCount = 0;

// ---- identity probe --------------------------------------------------------
// To react to the character that was actually shot, the hook has to know WHICH
// character it is composing. The skeleton pointer (a0) is a stable identity but
// it is a Granny object - nothing in the damage path hands us one.
//
// a4 is the composition offset matrix. If it is the character's world
// transform, its translation gives a world position, and an impact can be
// matched to a character by proximity - no NPC-to-skeleton table needed. This
// probe captures distinct skeletons with that translation so it can be checked
// against known NPC positions from 'npcs' before anything relies on it.
// a5 is captured too: Granny's BuildWorldPose signature ends in a result
// world_pose, and world-space bone transforms would let an impact point pick
// its nearest bone geometrically - no parent hierarchy needed. head[] is the
// first few dwords at a5 so the struct layout can be read off real data.
struct PoseSample {
    unsigned skeleton;
    int      bones;
    unsigned a4;
    float    pos[3];
    int      posValid;
    unsigned a5, a6;
    unsigned head[8];
    int      headValid;
};
#define MAX_SAMPLES 24
static PoseSample   g_samples[MAX_SAMPLES];
static volatile LONG g_sampleCount = 0;
static bool          g_probing     = false;

static bool ReadableFloats(const void* p, int n) {
    // The offset matrix may be passed by value or as a null; only dereference
    // what is actually mapped and writable-or-readable.
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    SIZE_T avail = (SIZE_T)((BYTE*)mbi.BaseAddress + mbi.RegionSize - (BYTE*)p);
    return avail >= (SIZE_T)n * sizeof(float);
}

static void CaptureSample(unsigned* args) {
    unsigned skel = args[0];
    for (LONG i = 0; i < g_sampleCount; i++)
        if (g_samples[i].skeleton == skel) return;      // already have this one
    LONG idx = InterlockedIncrement(&g_sampleCount) - 1;
    if (idx >= MAX_SAMPLES) { g_sampleCount = MAX_SAMPLES; g_probing = false; return; }
    PoseSample* s = &g_samples[idx];
    s->skeleton = skel;
    s->bones    = (int)args[2];
    s->a4       = args[4];
    s->posValid = 0;
    const float* m = (const float*)(uintptr_t)args[4];
    // Row-major 4x4: translation is elements 12,13,14.
    if (m && ReadableFloats(m, 16)) {
        s->pos[0] = m[12]; s->pos[1] = m[13]; s->pos[2] = m[14];
        s->posValid = 1;
    }
    s->a5 = args[5];
    s->a6 = args[6];
    s->headValid = 0;
    // a5 turned out to be the bone count repeated, not a pointer, so a6 is the
    // candidate result buffer. Dump its head to identify the struct.
    const unsigned* h = (const unsigned*)(uintptr_t)args[6];
    if (h && ReadableFloats(h, 8)) {
        for (int k = 0; k < 8; k++) s->head[k] = h[k];
        s->headValid = 1;
    }
}

// ---- skeleton introspection ------------------------------------------------
// Guessing a world-pose stride from hex is a dead end - the buffers are small
// and adjacent, so a wrong stride silently reads the next object. Granny's
// skeleton carries the answer directly:
//
//   granny_skeleton { char* Name; int BoneCount; granny_bone* Bones; }
//   granny_bone     { char* Name; int ParentIndex; granny_transform Local;
//                     granny_matrix_4x4 InverseWorld4x4; float LODError;
//                     granny_variant ExtendedData; }
//
// BoneCount at +4 must equal the count the hook already receives, which makes
// this self-verifying: if it does not match, the layout is wrong and we stop.
// Bone NAMES are what this is really after - they turn "bone 17" into
// "L_UpperArm" and make locational reactions (and later locational damage)
// possible without any hardcoded per-character index tables.

static bool ReadableBytes(const void* p, SIZE_T n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    SIZE_T avail = (SIZE_T)((BYTE*)mbi.BaseAddress + mbi.RegionSize - (BYTE*)p);
    return avail >= n;
}

// Copy a printable ASCII name, or return false. Bone names are short, so a
// long run of non-printables means the pointer is not a name.
static bool ReadName(unsigned addr, char* out, int outLen) {
    const char* p = (const char*)(uintptr_t)addr;
    if (!ReadableBytes(p, 2)) return false;
    int i = 0;
    for (; i < outLen - 1; i++) {
        if (!ReadableBytes(p + i, 1)) return false;
        char ch = p[i];
        if (ch == 0) break;
        if ((unsigned char)ch < 0x20 || (unsigned char)ch > 0x7E) return false;
        out[i] = ch;
    }
    out[i] = 0;
    return i > 0;
}

// This build has bone names stripped (skeleton->Name is NULL), so a scorer that
// requires readable names rejects everything - which is exactly what the first
// attempt did. Names are a convenience; the PARENT INDICES are what the code
// actually needs, because composing local transforms into world space is what
// lets an impact point pick its nearest bone.
//
// Rather than trusting Granny's documented 152-byte granny_bone (this build
// uses a different size), search for the (stride, parentOffset) pair that
// yields a valid hierarchy over all N bones. The constraints are strong enough
// to be self-verifying: index 0 is the root with parent -1, and every other
// bone's parent strictly precedes it (Granny stores bones parent-first).
static int g_boneParentOff = 4;   // set by SWSE_GrannySkeletonInfo
static int SWSE_GrannySkeletonInfoUncached(unsigned skel, int expectBones,
                                           int* strideOut, unsigned* bonesOut,
                                           char* msg, int msgLen);

// One guarded block for the whole sequence instead of a VirtualQuery per read.
// The per-read version made this ~200,000 syscalls across the full candidate
// search, which froze the game for about three seconds the first time a given
// skeleton was hit.
static bool ValidParentSeq(unsigned bones, int boneCount, int stride, int poff) {
    __try {
        for (int b = 0; b < boneCount; b++) {
            int parent = *(const int*)(uintptr_t)(bones + (unsigned)(b * stride) + poff);
            if (b == 0) { if (parent != -1) return false; }
            else        { if (parent < 0 || parent >= b) return false; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

// Result cache. The search below is a nested scan over stride and offset
// candidates with a VirtualQuery per probe - fine once per skeleton, ruinous
// per frame. Uncached it froze the game for seconds after every hit, because a
// live reaction re-ran it every frame for the whole of its life.
struct SkelInfoCache {
    unsigned skel;
    int      bones;      // expected bone count this was validated against
    int      stride;
    unsigned bonesPtr;
    int      parentOff;
    bool     ok;
};
#define MAX_SKELINFO 64
static SkelInfoCache g_skelInfo[MAX_SKELINFO];
static int           g_skelInfoCount = 0;

int SWSE_GrannySkeletonInfo(unsigned skel, int expectBones, int* strideOut,
                            unsigned* bonesOut, char* msg, int msgLen) {
    for (int i = 0; i < g_skelInfoCount; i++) {
        SkelInfoCache* e = &g_skelInfo[i];
        if (e->skel != skel || e->bones != expectBones) continue;
        if (!e->ok) { lstrcpynA(msg, "cached: layout not resolvable", msgLen); return 0; }
        if (strideOut) *strideOut = e->stride;
        if (bonesOut)  *bonesOut  = e->bonesPtr;
        g_boneParentOff = e->parentOff;
        lstrcpynA(msg, "cached skeleton layout", msgLen);
        return expectBones;
    }
    int rc = SWSE_GrannySkeletonInfoUncached(skel, expectBones, strideOut,
                                            bonesOut, msg, msgLen);
    if (g_skelInfoCount < MAX_SKELINFO) {
        SkelInfoCache* e = &g_skelInfo[g_skelInfoCount++];
        e->skel      = skel;
        e->bones     = expectBones;
        e->ok        = (rc == expectBones);
        e->stride    = (strideOut && e->ok) ? *strideOut : 0;
        e->bonesPtr  = (bonesOut  && e->ok) ? *bonesOut  : 0;
        e->parentOff = g_boneParentOff;
    }
    return rc;
}

static int SWSE_GrannySkeletonInfoUncached(unsigned skel, int expectBones,
                                           int* strideOut, unsigned* bonesOut,
                                           char* msg, int msgLen) {
    if (strideOut) *strideOut = 0;
    if (bonesOut)  *bonesOut  = 0;
    if (!ReadableBytes((const void*)(uintptr_t)skel, 12)) {
        lstrcpynA(msg, "skeleton pointer not readable", msgLen);
        return 0;
    }
    unsigned namePtr  = *(unsigned*)(uintptr_t)skel;
    int      count    = *(int*)(uintptr_t)(skel + 4);
    unsigned bones    = *(unsigned*)(uintptr_t)(skel + 8);
    if (count != expectBones) {
        wsprintfA(msg, "BoneCount at +4 is %d, hook reported %d - layout wrong",
                  count, expectBones);
        return 0;
    }
    // Search (stride, parentOffset) jointly. A single bone would match many
    // pairs by luck; requiring a consistent hierarchy across all N makes a
    // false positive very unlikely, and ambiguity is reported rather than
    // silently picking the first hit.
    // Try the layout every skeleton in this game has used first. The full
    // search is a last resort: it is thousands of candidate pairs, and paying
    // for it on the first hit of each new character is what made the game
    // stutter. A hit here costs one pass over the bones.
    int foundStride = 0, foundPoff = 0, matches = 0;
    if (ValidParentSeq(bones, count, 0x70, 0x68)) {
        foundStride = 0x70; foundPoff = 0x68; matches = 1;
    } else {
        for (int stride = 0x40; stride <= 0x100; stride += 4) {
            for (int poff = 0; poff <= stride - 4; poff += 4) {
                if (!ValidParentSeq(bones, count, stride, poff)) continue;
                matches++;
                if (!foundStride) { foundStride = stride; foundPoff = poff; }
            }
        }
    }
    if (!foundStride) {
        wsprintfA(msg, "no (stride,parentOffset) produced a valid %d-bone hierarchy",
                  count);
        return 0;
    }
    if (strideOut) *strideOut = foundStride;
    if (bonesOut)  *bonesOut  = bones;
    g_boneParentOff = foundPoff;

    char skelName[64];
    if (!ReadName(namePtr, skelName, sizeof(skelName))) lstrcpyA(skelName, "(unnamed)");
    wsprintfA(msg, "%s: %d bones, stride 0x%X, parent at +0x%X (%d layout%s matched)",
              skelName, count, foundStride, foundPoff, matches, matches == 1 ? "" : "s");
    return count;
}

int SWSE_GrannyBoneInfo(unsigned bones, int stride, int index,
                        char* name, int nameLen, int* parent) {
    unsigned rec = bones + (unsigned)(index * stride);
    if (!ReadableBytes((const void*)(uintptr_t)rec, 8)) return 0;
    if (parent) {
        const void* pp = (const void*)(uintptr_t)(rec + g_boneParentOff);
        *parent = ReadableBytes(pp, 4) ? *(const int*)pp : -2;
    }
    // Names are stripped in this build; keep the lookup so a build that ships
    // them (or a debug asset) still benefits.
    if (name) {
        unsigned namePtr = *(unsigned*)(uintptr_t)rec;
        if (!ReadName(namePtr, name, nameLen)) lstrcpynA(name, "-", nameLen);
    }
    return 1;
}

// ---- torso bone, without bone names ----------------------------------------
// A reaction has to perturb something the player can SEE. Bone 5 might be a
// finger; the chest is what reads as a flinch, and rotating it carries the
// head and arms with it because children inherit the parent's local change.
//
// Names would make this trivial ("Spine2"), but this build strips them. The
// hierarchy alone is enough: the chest is the bone that fans out into the most
// direct children (head, both arms, and every prop attachment hang off it). On
// the 51-bone character that is bone 10 with 18 children, against 1-2 for
// ordinary limb links - not a close call.
//
// Cached per skeleton: this walks the whole bone array, which is far too much
// work to repeat for every character every frame.
// Per-skeleton cache. Also holds the parent array, because a flinch should not
// be one rigid bone: rotating only the chest moves the whole upper body as a
// block, which reads as a mannequin pivoting. Distributing the rotation along
// the chain toward the root - each step weaker - looks like a body absorbing an
// impact. Walking parents per frame needs them resident, not re-read.
struct TorsoCache {
    unsigned skel;
    int      bone;
    int      boneCount;
    int      parent[WPOS_MAX];
    // For each bone, the child carrying the biggest subtree - i.e. the spine
    // continuation. Chaining toward the ROOT was wrong: from the chest that
    // walks into the pelvis, and rotating the pelvis drags the legs and blows
    // the hips out. A flinch travels the other way, up through neck and head.
    int      spineChild[WPOS_MAX];
    // Precomputed once: a childless bone hanging off the chest hub is a weapon
    // or prop mount, not anatomy. Deciding this per frame cost an O(n^2) scan
    // with a VirtualQuery per probe - about 4000 syscalls per resolve, which is
    // what made the game lurch every time a shot landed.
    bool     isAttach[WPOS_MAX];
    // How much skeleton hangs below each bone. Rotating a fingertip moves a
    // fingertip; rotating the upper arm swings the whole limb. Nearest-bone
    // search happily picks terminal bones - measured on a 50-bone rig, hits
    // resolved to bones 3, 10, 12, 13, 18, 21, 26, 27, 31, 33 and the reaction
    // was "barely noticeable" despite a healthy 19.5 degrees of rotation.
    int      subtree[WPOS_MAX];
    // Character height, from the composed bone cloud. The same rotation reads
    // very differently on a big Wolvark and a smaller outlaw - it sweeps more
    // distance and more pixels on the larger body - and physically a lighter
    // body should move MORE for the same impact, not the same amount. Scaling
    // by size fixes both at once.
    float    height;
    bool     haveHeight;
    // Children of the chest that are NOT the spine continuation - i.e. the arm
    // roots. Rotating the chest necessarily carries the arms and whatever they
    // are holding, so a hit swings the crossbow off aim. Counter-rotating these
    // lets the torso absorb the impact while the weapon stays roughly put,
    // which is the same trick the Fallout NV mod uses when it damps the
    // clavicles for an armed character.
    int      armRoot[6];
    int      armRootCount;
    bool     haveParents;
};
#define MAX_TORSO 64
static TorsoCache g_torso[MAX_TORSO];
static int        g_torsoCount = 0;

// The spine continuation of a bone (its largest-subtree child), from the cache.
// -1 = none / unknown.
// Cached parent, so hot paths never re-read skeleton memory.
static int CachedParentOf(unsigned skel, int bone) {
    for (int i = 0; i < g_torsoCount; i++) {
        if (g_torso[i].skel != skel) continue;
        if (!g_torso[i].haveParents) return -2;
        if (bone < 0 || bone >= g_torso[i].boneCount) return -2;
        return g_torso[i].parent[bone];
    }
    return -2;                                  // unknown: caller falls back
}

// Arm roots for a skeleton, so the hot path never rescans the hierarchy.
static int CachedArmRoots(unsigned skel, const int** out) {
    for (int i = 0; i < g_torsoCount; i++) {
        if (g_torso[i].skel != skel) continue;
        if (!g_torso[i].haveParents) return 0;
        if (out) *out = g_torso[i].armRoot;
        return g_torso[i].armRootCount;
    }
    return 0;
}

// How many bones hang below this one (itself included). 1 = a leaf.
static int CachedSubtree(unsigned skel, int bone) {
    for (int i = 0; i < g_torsoCount; i++) {
        if (g_torso[i].skel != skel) continue;
        if (!g_torso[i].haveParents) return 0;
        if (bone < 0 || bone >= g_torso[i].boneCount) return 0;
        return g_torso[i].subtree[bone];
    }
    return 0;
}

// Walk up from a struck bone until the rotation will actually move something.
// A hit that resolves to a fingertip is geometrically correct and visually
// nothing; its parent carries the hand, its grandparent the forearm. Climbing
// until at least g_minLimbMass bones hang below keeps the reaction on the limb
// that was hit rather than on its smallest tip.
static int PromoteToVisibleBone(unsigned skel, int bone, int minMass) {
    for (int guard = 0; guard < 8; guard++) {
        int mass = CachedSubtree(skel, bone);
        if (mass <= 0) return bone;                 // unknown - leave it alone
        if (mass >= minMass) return bone;
        int par = CachedParentOf(skel, bone);
        if (par < 0 || par >= bone) return bone;    // reached the root
        bone = par;
    }
    return bone;
}

static bool CachedIsAttachment(unsigned skel, int bone) {
    for (int i = 0; i < g_torsoCount; i++) {
        if (g_torso[i].skel != skel) continue;
        if (!g_torso[i].haveParents) return false;
        if (bone < 0 || bone >= g_torso[i].boneCount) return false;
        return g_torso[i].isAttach[bone];
    }
    return false;
}

static int CachedSpineChild(unsigned skel, int bone) {
    for (int i = 0; i < g_torsoCount; i++) {
        if (g_torso[i].skel != skel) continue;
        if (!g_torso[i].haveParents) return -1;
        if (bone < 0 || bone >= g_torso[i].boneCount) return -1;
        return g_torso[i].spineChild[bone];
    }
    return -1;
}

static int FindTorsoBone(unsigned skel, int boneCount) {
    for (int i = 0; i < g_torsoCount; i++)
        if (g_torso[i].skel == skel) return g_torso[i].bone;

    int result = -1;
    int stride = 0; unsigned bones = 0; char msg[200];
    if (SWSE_GrannySkeletonInfo(skel, boneCount, &stride, &bones, msg, sizeof(msg))
        == boneCount && stride > 0 && bones) {

        // MEASURED FAILURE of the previous heuristic: "the bone with the most
        // direct children" picked bone 13 of 64 on the player, every hit, and
        // what visibly moved was an ARM. Arms fan out into hands and fingers,
        // so they win a child count easily. The chest does not.
        //
        // The chest is better characterised by its SUBTREE: everything above
        // the waist - head, neck, both arms - hangs below it. So walk down from
        // the root and take the DEEPEST bone that still carries most of the
        // skeleton. That is the last point where rotating still moves the whole
        // upper body, which is exactly what a flinch is.
        static int parent[WPOS_MAX], subtree[WPOS_MAX], depth[WPOS_MAX];
        if (boneCount > WPOS_MAX) boneCount = WPOS_MAX;
        bool ok = true;
        for (int b = 0; b < boneCount && ok; b++) {
            const void* p = (const void*)(uintptr_t)
                            (bones + (unsigned)(b * stride) + g_boneParentOff);
            if (!ReadableBytes(p, 4)) { ok = false; break; }
            parent[b]  = *(const int*)p;
            subtree[b] = 1;
            depth[b]   = 0;
        }
        if (ok) {
            // Bones are stored parent-first, so a single reverse pass
            // accumulates subtree sizes and a forward pass gives depth.
            for (int b = boneCount - 1; b > 0; b--)
                if (parent[b] >= 0 && parent[b] < b) subtree[parent[b]] += subtree[b];
            for (int b = 1; b < boneCount; b++)
                if (parent[b] >= 0 && parent[b] < b) depth[b] = depth[parent[b]] + 1;

            // "Most of the skeleton" = 40%. Above that and it selects the
            // pelvis; below it and it slides up into the neck.
            int need = (boneCount * 2) / 5;
            int bestBone = -1, bestDepth = -1;
            for (int b = 1; b < boneCount; b++) {
                if (subtree[b] < need) continue;
                if (depth[b] > bestDepth ||
                    (depth[b] == bestDepth && bestBone >= 0 && subtree[b] > subtree[bestBone])) {
                    bestDepth = depth[b]; bestBone = b;
                }
            }
            result = bestBone;
        }
    }
    if (g_torsoCount < MAX_TORSO) {
        TorsoCache* tc = &g_torso[g_torsoCount];
        tc->skel        = skel;
        tc->bone        = result;
        tc->boneCount   = boneCount;
        tc->haveParents = false;
        if (stride > 0 && bones && boneCount <= WPOS_MAX) {
            bool ok = true;
            for (int b = 0; b < boneCount; b++) {
                const void* p = (const void*)(uintptr_t)
                                (bones + (unsigned)(b * stride) + g_boneParentOff);
                if (!ReadableBytes(p, 4)) { ok = false; break; }
                tc->parent[b]     = *(const int*)p;
                tc->spineChild[b] = -1;
            }
            if (ok) {
                // Subtree sizes, then for each bone the child with the biggest
                // one. From the chest that path is neck -> head, not an arm,
                // because the head branch outweighs a single arm link.
                static int sub[WPOS_MAX];
                for (int b = 0; b < boneCount; b++) sub[b] = 1;
                for (int b = boneCount - 1; b > 0; b--)
                    if (tc->parent[b] >= 0 && tc->parent[b] < b)
                        sub[tc->parent[b]] += sub[b];
                for (int b = 1; b < boneCount; b++) {
                    int par = tc->parent[b];
                    if (par < 0 || par >= b) continue;
                    int cur = tc->spineChild[par];
                    if (cur < 0 || sub[b] > sub[cur]) tc->spineChild[par] = b;
                }
                for (int b = 0; b < boneCount; b++) tc->subtree[b] = sub[b];
                // Attachment flags, computed here once instead of per frame.
                // sub[b] == 1 means the bone has no descendants at all.
                for (int b = 0; b < boneCount; b++)
                    tc->isAttach[b] = (b > 0 && sub[b] == 1 && result > 0 &&
                                       tc->parent[b] == result);
                // Arm roots: children of the chest other than the spine
                // continuation. On the 64-bone rig the chest (5) has children
                // 6, 8 (the two arms) and 10 (neck), so this picks out 6 and 8.
                tc->armRootCount = 0;
                if (result > 0) {
                    int spine = tc->spineChild[result];
                    for (int b = 1; b < boneCount && tc->armRootCount < 6; b++)
                        if (tc->parent[b] == result && b != spine && sub[b] > 1)
                            tc->armRoot[tc->armRootCount++] = b;
                }
                // ONLY a biped has arms to damp. Measured on a 20-bone creature
                // rig: its body bone carries FOUR 3-link chains - legs and
                // wings - and three of them looked like "arms" by this test, so
                // the damping was cancelling rotation on a chicken's legs. A
                // character that holds a weapon has exactly two branches off
                // the chest besides the spine; anything else is not an arm pair
                // and must be left alone.
                if (tc->armRootCount != 2) tc->armRootCount = 0;
            }
            tc->haveParents = ok;
        }
        g_torsoCount++;
    }
    return result;
}

// ---- pose record layout, measured rather than assumed ----------------------
// The transform stride and the orientation offset were taken from Granny's
// documented granny_transform (0x44, quat at +0x10). That was wrong for this
// build, and because writing at the wrong offset still MOVES bones, the error
// looked like success for a long time.
//
// This finds the layout instead of assuming it. A unit quaternion is a very
// strong signature - four floats in [-1,1] whose norm is 1 within a tight
// tolerance - so scanning the buffer for them and looking at the SPACING
// between hits gives the stride directly, and the first hit gives the offset.
#define LAYOUT_MAX 64
static unsigned     g_layoutOff[LAYOUT_MAX];
static int          g_layoutCount = 0;
static unsigned     g_layoutBase  = 0;
static int          g_layoutBones = 0;
static volatile LONG g_layoutRequest = 0;

static bool IsUnitQuat(const float* q) {
    for (int i = 0; i < 4; i++) {
        if (q[i] != q[i]) return false;               // NaN
        if (q[i] < -1.001f || q[i] > 1.001f) return false;
    }
    float n = q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3];
    if (n < 0.999f || n > 1.001f) return false;
    // An all-zero-plus-one vector is identity and legitimate, but a run of
    // exact 1.0/0.0 also matches padding; keep them, the spacing still tells.
    return true;
}

static void CaptureLayout(unsigned poseAddr, int boneCount) {
    g_layoutCount = 0;
    g_layoutBase  = poseAddr;
    g_layoutBones = boneCount;
    // Generous window: even a 0x60 stride for boneCount bones plus a header.
    SIZE_T span = (SIZE_T)boneCount * 0x60 + 0x40;
    for (SIZE_T o = 0; o + 16 <= span && g_layoutCount < LAYOUT_MAX; o += 4) {
        const float* q = (const float*)(uintptr_t)(poseAddr + o);
        if (!ReadableBytes(q, 16)) break;
        if (IsUnitQuat(q)) {
            g_layoutOff[g_layoutCount++] = (unsigned)o;
            o += 12;          // skip past this quat so we do not match overlaps
        }
    }
}

void SWSE_HitReactLayoutRequest() { InterlockedExchange(&g_layoutRequest, 1); }
int  SWSE_HitReactLayoutGet(int i, unsigned* off, unsigned* base, int* count,
                            int* bones) {
    if (base)  *base  = g_layoutBase;
    if (count) *count = g_layoutCount;
    if (bones) *bones = g_layoutBones;
    if (i < 0 || i >= g_layoutCount) return 0;
    if (off) *off = g_layoutOff[i];
    return 1;
}

// ---- world-space bone positions --------------------------------------------
// Everything locational needs this: "which bone did the projectile hit" is a
// geometry question, and the local pose only holds parent-relative transforms.
// Composing them needs the parent array, which is why decoding the skeleton
// layout mattered.
//
//   world_R[b] = local_R[b] * world_R[parent]
//   world_t[b] = local_t[b] * world_R[parent] + world_t[parent]
//
// with the a4 offset matrix acting as the root's parent. Row-vector convention
// (v' = v*M) to match the engine's matrices; if the convention were wrong the
// composed cloud would be scrambled rather than character-shaped, which is
// exactly what 'hitreact wpos' checks.
struct BoneXform { float R[9]; float t[3]; };

static void QuatToMat(const float* q, float* R) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    R[0] = 1-2*(y*y+z*z); R[1] = 2*(x*y+w*z);   R[2] = 2*(x*z-w*y);
    R[3] = 2*(x*y-w*z);   R[4] = 1-2*(x*x+z*z); R[5] = 2*(y*z+w*x);
    R[6] = 2*(x*z+w*y);   R[7] = 2*(y*z-w*x);   R[8] = 1-2*(x*x+y*y);
}
// out = a * b  (row-vector: apply a first, then b)
static void Mat3Mul(const float* a, const float* b, float* out) {
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            out[r*3+c] = a[r*3+0]*b[0*3+c] + a[r*3+1]*b[1*3+c] + a[r*3+2]*b[2*3+c];
}
static void Vec3Mat3(const float* v, const float* M, float* out) {
    out[0] = v[0]*M[0] + v[1]*M[3] + v[2]*M[6];
    out[1] = v[0]*M[1] + v[1]*M[4] + v[2]*M[7];
    out[2] = v[0]*M[2] + v[1]*M[5] + v[2]*M[8];
}

// Composes into out[]. Returns bones written, 0 on failure.
static int ComposeWorldBones(unsigned skel, int boneCount, const BYTE* localPose,
                             const float* a4, BoneXform* out, int maxOut) {
    if (boneCount <= 0 || boneCount > maxOut || !localPose || !a4) return 0;
    int stride = 0; unsigned bones = 0; char msg[200];
    if (SWSE_GrannySkeletonInfo(skel, boneCount, &stride, &bones, msg, sizeof(msg))
        != boneCount || !stride || !bones) return 0;

    // Root's parent transform: the a4 offset matrix (row-major 4x4).
    float rootR[9] = { a4[0], a4[1], a4[2],
                       a4[4], a4[5], a4[6],
                       a4[8], a4[9], a4[10] };
    float rootT[3] = { a4[12], a4[13], a4[14] };

    for (int b = 0; b < boneCount; b++) {
        const BYTE*  rec = localPose + GT_BASE + (SIZE_T)b * GT_SIZE;
        const float* lp = (const float*)(rec + GT_POSITION);
        const float* lq = (const float*)(rec + GT_ORIENT);
        float lR[9]; QuatToMat(lq, lR);

        const float* pR; const float* pT;
        // Cached parent: re-reading skeleton memory here meant a VirtualQuery
        // per bone per compose, on the render thread.
        int parent = CachedParentOf(skel, b);
        if (parent == -2) {
            const void* prec = (const void*)(uintptr_t)
                               (bones + (unsigned)(b * stride) + g_boneParentOff);
            if (!ReadableBytes(prec, 4)) return 0;
            parent = *(const int*)prec;
        }
        if (parent < 0 || parent >= b) { pR = rootR; pT = rootT; }
        else                           { pR = out[parent].R; pT = out[parent].t; }

        Mat3Mul(lR, pR, out[b].R);
        float rotated[3];
        Vec3Mat3(lp, pR, rotated);
        out[b].t[0] = rotated[0] + pT[0];
        out[b].t[1] = rotated[1] + pT[1];
        out[b].t[2] = rotated[2] + pT[2];
    }
    return boneCount;
}

// ---- nearest bone to an impact point ---------------------------------------
// "A shot to the arm should move the arm." With world-space bone positions
// available this is just a nearest-point search, with two exclusions:
//
//   * the ROOT, because rotating it swings the whole character from the feet
//     rather than reading as a local flinch;
//   * ATTACHMENT bones. The 51-bone rig hangs 8 childless bones off its chest
//     hub - weapon and prop mount points. They sit out at holster distance, so
//     a shot that passes near a slung rifle would otherwise "hit" the rifle
//     mount and rotate the prop instead of the body.
//
// Returns the bone index, or -1.
static int NearestBoneToPoint(unsigned skel, int boneCount, const BYTE* localPose,
                              const float* a4, const float* point,
                              float* outDist) {
    static BoneXform xf[WPOS_MAX];
    int n = ComposeWorldBones(skel, boneCount, localPose, a4, xf, WPOS_MAX);
    if (n <= 0) return -1;

    // Populates the cache (parents, spine chain, attachment flags) on first use.
    FindTorsoBone(skel, boneCount);

    int best = -1; float bestD2 = 1e30f;
    for (int b = 1; b < n; b++) {              // b = 0 is the root, skipped
        if (CachedIsAttachment(skel, b)) continue;   // weapon/prop mount
        float dx = xf[b].t[0] - point[0];
        float dy = xf[b].t[1] - point[1];
        float dz = xf[b].t[2] - point[2];
        float d2 = dx*dx + dy*dy + dz*dz;
        if (d2 < bestD2) { bestD2 = d2; best = b; }
    }
    if (outDist) *outDist = (best >= 0) ? (float)sqrt((double)bestD2) : 0.0f;
    return best;
}

// Debug capture: the local pose is only valid inside the hook, so the console
// asks for a snapshot and the hook fills it on the next matching character.
static BoneXform    g_wpos[WPOS_MAX];
static int          g_wposCount   = 0;
static unsigned     g_wposSkel    = 0;
static volatile LONG g_wposRequest = 0;
static int          g_wposMinBones = 0;

// Raw inputs to the composition, captured alongside it. Garbage output has two
// possible causes - bad math or bad pointers - and dumping bone 0's local
// transform plus the a4 matrix separates them immediately.
static float g_wposRawPos[3], g_wposRawQuat[4], g_wposRawA4[16];
static int   g_wposRawValid = 0;
// The head of the pose buffer itself. If granny_local_pose has a header before
// the transform array then every offset used so far is shifted, which would
// explain a NaN position and a quaternion that only makes sense as (w,x,y,z).
static unsigned g_wposRawHead[24];
static unsigned g_wposPoseAddr = 0;

int SWSE_HitReactWposHead(unsigned* head, unsigned* addr) {
    if (!g_wposRawValid) return 0;
    if (addr) *addr = g_wposPoseAddr;
    if (head) for (int i = 0; i < 24; i++) head[i] = g_wposRawHead[i];
    return 1;
}

int SWSE_HitReactWposRaw(float* pos, float* quat, float* a4) {
    if (!g_wposRawValid) return 0;
    if (pos)  for (int i = 0; i < 3; i++)  pos[i]  = g_wposRawPos[i];
    if (quat) for (int i = 0; i < 4; i++)  quat[i] = g_wposRawQuat[i];
    if (a4)   for (int i = 0; i < 16; i++) a4[i]   = g_wposRawA4[i];
    return 1;
}

void SWSE_HitReactWposRequest(int minBones) {
    g_wposMinBones = minBones;
    g_wposCount = 0;
    InterlockedExchange(&g_wposRequest, 1);
}
int SWSE_HitReactWposGet(int i, float* pos, unsigned* skel, int* count) {
    if (skel)  *skel  = g_wposSkel;
    if (count) *count = g_wposCount;
    if (i < 0 || i >= g_wposCount) return 0;
    if (pos) { pos[0] = g_wpos[i].t[0]; pos[1] = g_wpos[i].t[1]; pos[2] = g_wpos[i].t[2]; }
    return 1;
}

void SWSE_HitReactProbe(int on) {
    if (on) { g_sampleCount = 0; g_probing = true; }
    else      g_probing = false;
}

int SWSE_HitReactProbeGet(int i, unsigned* skel, int* bones, unsigned* a4,
                          float* pos, int* posValid) {
    if (i < 0 || i >= g_sampleCount || i >= MAX_SAMPLES) return 0;
    PoseSample* s = &g_samples[i];
    if (skel)     *skel     = s->skeleton;
    if (bones)    *bones    = s->bones;
    if (a4)       *a4       = s->a4;
    if (posValid) *posValid = s->posValid;
    if (pos) { pos[0] = s->pos[0]; pos[1] = s->pos[1]; pos[2] = s->pos[2]; }
    return 1;
}

int SWSE_HitReactProbeArgs(int i, unsigned* a5, unsigned* a6, unsigned* head,
                           int* headValid) {
    if (i < 0 || i >= g_sampleCount || i >= MAX_SAMPLES) return 0;
    PoseSample* s = &g_samples[i];
    if (a5)        *a5        = s->a5;
    if (a6)        *a6        = s->a6;
    if (headValid) *headValid = s->headValid;
    if (head) for (int k = 0; k < 8; k++) head[k] = s->head[k];
    return 1;
}

static void NoteHitBone(int slot, int bone, int boneCount, unsigned skel);
static void ApplyReactionsBody(unsigned* args);

// Called from the hook with a pointer to the stack arguments.
//
// FAIL SAFE. This runs on the render thread thousands of times a second, on
// pointers derived from a reverse-engineered layout, for every character in the
// level. A single bad assumption anywhere in here takes the whole game down -
// which it has done. A fault now disables the feature and lets the game carry
// on instead of killing it: the hook stays patched (unpatching live is its own
// crash) but returns immediately from then on.
static void __stdcall ApplyReactions(unsigned* args) {
    __try {
        ApplyReactionsBody(args);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_reactOn = false;
        InterlockedIncrement(&g_faulted);
        LogG("HITREACT: FAULTED - reactions disabled, game left running");
    }
}

static void ApplyReactionsBody(unsigned* args) {
    // Counted before the enabled check so 'hitreact off' still shows the hook
    // is alive and being called.
    InterlockedIncrement(&g_hookCalls);
    g_lastSkeleton  = args[0];
    g_lastBoneCount = (int)args[2];
    if (g_probing) CaptureSample(args);

    // T-POSE: force every bone to identity before reactions are applied.
    // With the animation flattened, the ONLY thing that can move the character
    // is our own offset - so any movement at all is proof the system works,
    // with no need to distinguish it from a walk cycle or a hurt animation.
    if (g_tpose && (int)args[2] >= g_minReactBones) {
        __try {
            BYTE* lp = (BYTE*)(uintptr_t)args[3];
            int   nb = (int)args[2];
            int   cap = *(const int*)lp;
            if (cap > 0 && cap < 512 && nb > cap) nb = cap;
            for (int b = 0; b < nb; b++) {
                float* q = (float*)(lp + GT_BASE + (SIZE_T)b * GT_SIZE + GT_ORIENT);
                q[0] = 0.0f; q[1] = 0.0f; q[2] = 0.0f; q[3] = 1.0f;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // Hold the animation still, so the game's own hurt reaction cannot move the
    // character and only our offset can.
    if (g_freezeOn && (int)args[2] >= g_minReactBones) {
        __try { ApplyFreeze(args[0], (int)args[2], (BYTE*)(uintptr_t)args[3]); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    if (!g_reactOn) return;
    // Not everything with a skeleton is a character. The crossbow's live ammo
    // is a 3-6 bone skeleton attached to the weapon, so it sits at the PLAYER's
    // position and gets caught by any reaction centred there - which sent the
    // zapfly flying around the screen. Measured bone counts separate cleanly:
    // characters 15-51, props/ammo/critters 3-7.
    // Small skeletons are BOTH critters and the crossbow's live ammo, so a bone
    // count alone cannot tell them apart - which is why the threshold that
    // stopped the zapfly flying off the weapon was also silently excluding
    // every chicken in the game.
    //
    // Position separates them: ammo and held props ride at the player's own
    // position, a critter stands somewhere else. So only reject a small
    // skeleton when it is sitting on top of the player.
    if ((int)args[2] < g_minReactBones) {
        const float* m = (const float*)(uintptr_t)args[4];
        float pp[3];
        bool  havePlayer = (SWSE_PlayerGet(0x24, &pp[0], &pp[1], &pp[2]) == 1);
        if (!m || !havePlayer) return;               // cannot tell - stay safe
        __try {
            float dx = m[12] - pp[0], dy = m[13] - pp[1], dz = m[14] - pp[2];
            if (dx*dx + dy*dy + dz*dz < ATTACHED_PROP_RADIUS * ATTACHED_PROP_RADIUS)
                return;                              // held prop / ammo
        } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    }

    // Layout discovery, on a real pose buffer of a real character.
    if (g_layoutRequest && (int)args[2] >= 30) {
        CaptureLayout(args[3], (int)args[2]);
        InterlockedExchange(&g_layoutRequest, 0);
    }

    // Fill a world-bone snapshot if one was asked for. Only inside the hook is
    // the local pose valid.
    if (g_wposRequest && (int)args[2] >= g_wposMinBones) {
        const float* m = (const float*)(uintptr_t)args[4];
        if (m && ReadableFloats(m, 16)) {
            const BYTE* lp0 = (const BYTE*)(uintptr_t)args[3];
            for (int k = 0; k < 3; k++)
                g_wposRawPos[k]  = ((const float*)(lp0 + GT_BASE + GT_POSITION))[k];
            for (int k = 0; k < 4; k++)
                g_wposRawQuat[k] = ((const float*)(lp0 + GT_BASE + GT_ORIENT))[k];
            for (int k = 0; k < 16; k++) g_wposRawA4[k] = m[k];
            g_wposPoseAddr = args[3];
            for (int k = 0; k < 24; k++) g_wposRawHead[k] = ((const unsigned*)lp0)[k];
            g_wposRawValid = 1;
            int n = ComposeWorldBones(args[0], (int)args[2],
                                      (const BYTE*)(uintptr_t)args[3], m,
                                      g_wpos, WPOS_MAX);
            if (n > 0) {
                g_wposCount = n;
                g_wposSkel  = args[0];
                InterlockedExchange(&g_wposRequest, 0);
            }
        }
    }
    unsigned skeleton  = args[0];
    int      firstBone = (int)args[1];
    int      boneCount = (int)args[2];
    BYTE*    localPose = (BYTE*)(uintptr_t)args[3];
    if (!localPose || boneCount <= 0 || boneCount > 512) return;

    // The pose header's first dword is a capacity (0x64 = 100 observed on every
    // pose dumped). Writing past it corrupts the heap, and a heap corruption
    // surfaces as a crash somewhere else entirely - exactly the kind of failure
    // that is impossible to attribute after the fact. Trust the smaller of the
    // two counts.
    __try {
        int cap = *(const int*)localPose;
        if (cap > 0 && cap < 512 && boneCount > cap) boneCount = cap;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }

    // The character's world position, read from the a4 offset matrix. Computed
    // once per call and only when something is actually pending.
    float cx = 0, cy = 0, cz = 0; bool haveCharPos = false;

    // ROOT CAUSE OF THE DEFORMED ARM. Reactions overlap - sustained fire keeps
    // several alive at once - and each one used to read the bone, rotate, and
    // write. The second reaction therefore read the FIRST one's output as its
    // base and multiplied on top of it. With a stream of hits the rotations
    // stack without bound, which is why isolated tests looked correct and real
    // combat tore the character apart.
    //
    // Every bone is now written exactly once per call: contributions are
    // accumulated first, then applied to the animation's own value.
    struct BoneAccum { int bone; float q[4]; bool used; };
    BoneAccum acc[SWSE_MAX_CHAIN * 4];
    int accCount = 0;
    int liveThisCall = 0;      // reactions contributing to THIS character

    DWORD now = GetTickCount();
    for (int i = 0; i < MAX_REACTS; i++) {
        HitReact* r = &g_reacts[i];
        if (!r->live) continue;
        DWORD age = now - r->startedAt;
        // Expiry does NOT just drop the reaction: whatever rotation is still
        // applied has to be unwound, or it stays in the persistent pose buffer
        // forever. An expiring reaction runs one last time with k = 0.
        bool expiring = ((int)age >= g_reactMs);
        if (expiring && !r->applied) { r->live = false; continue; }

        // Once a reaction has touched a character it stays bound to it, so the
        // unwind reaches the same bones even if that character has since moved
        // out of the match radius.
        if (r->applied) {
            if (r->appliedSkel != skeleton) continue;
        } else if (r->usePos) {
            if (!haveCharPos) {
                const float* m = (const float*)(uintptr_t)args[4];
                if (!m || !ReadableFloats(m, 16)) { InterlockedIncrement(&g_noMatrix); continue; }
                cx = m[12]; cy = m[13]; cz = m[14];
                haveCharPos = true;
            }
            // Match against a CYLINDER, not a sphere. The pose origin sits a
            // couple of units above the actor origin (measured: the player's
            // 51-bone pose reported z=44 while the actor reported z=41.7), so
            // a 3-unit sphere only barely contains the character and drops out
            // as they animate - which is exactly what the first test showed.
            // Characters are tall and thin: tight horizontally, loose in z.
            float dx = cx - r->pos[0], dy = cy - r->pos[1], dz = cz - r->pos[2];
            if (dz < 0) dz = -dz;
            // An impact point sits ON the body, so it can legitimately be a
            // full body-height above the character's origin - a head shot is
            // ~20 units up. Measured character extent is ~22 units tall, so the
            // ordinary 12-unit tolerance rejects head and shoulder hits.
            float zTol = r->useImpact ? REACT_Z_IMPACT : REACT_Z_TOLERANCE;
            if (dx*dx + dy*dy > r->radius * r->radius || dz > zTol) {
                InterlockedIncrement(&g_posReject);
                continue;
            }
            InterlockedIncrement(&g_posMatch);
        } else if (r->skeleton && r->skeleton != skeleton) {
            continue;                                   // different character
        }

        // Resolve the bone once, then keep using it, so the unwind cannot land
        // on a different bone than the one that was rotated.
        int wanted;
        if (r->applied) {
            wanted = r->appliedBone;
        } else if (r->useImpact) {
            // The bone the projectile actually struck.
            const float* m = (const float*)(uintptr_t)args[4];
            float dist = 0.0f;
            wanted = m ? NearestBoneToPoint(skeleton, boneCount, localPose, m,
                                            r->impact, &dist) : -1;
            if (wanted < 0) {           // composition failed - do not guess
                InterlockedIncrement(&g_impactFail);
                continue;
            }
            // Climb to a bone that actually carries some body with it - but
            // scale the requirement to the rig. A fixed 14 is right for a
            // 50-bone humanoid and impossible on an 8-bone chicken, where
            // nothing qualifies and the climb reaches the root, spinning the
            // whole animal rigidly. A third of the skeleton is a sensible
            // ceiling: enough mass to see, never the entire creature.
            int mass = g_minLimbMass;
            int cap  = boneCount / 3;
            if (cap < 2)    cap = 2;
            if (mass > cap) mass = cap;
            wanted = PromoteToVisibleBone(skeleton, wanted, mass);
            g_lastImpactBone = wanted;
            g_lastImpactDist = dist;
            InterlockedIncrement(&g_impactResolved);
        } else {
            wanted = r->bone;
            if (wanted < 0) {
                wanted = FindTorsoBone(skeleton, boneCount);
                if (wanted < 0) { InterlockedIncrement(&g_torsoFail); continue; }
            }
        }
        int b = wanted - firstBone;
        if (b < 0 || b >= boneCount) { InterlockedIncrement(&g_boneRange); continue; }

        // Ease-out: strongest at impact, gone by g_reactMs. Squared so it snaps
        // back quickly rather than drifting - this is meant to read as feedback,
        // not as an animation. An expiring reaction targets k = 0 so the delta
        // below unwinds it completely.
        // Envelope. The original was (1-t)^2: full rotation on the very first
        // frame, then a smooth glide back - so it POPPED on and eased off,
        // which reads as a glitch rather than a hit.
        //
        // A real impact has a fast but finite onset, overshoots slightly, and
        // settles. Three phases:
        //   attack  - rise to peak over g_easeAttack of the duration, smoothed
        //             with 3u^2-2u^3 so there is no corner at either end
        //   settle  - fall past zero into a small counter-swing
        //   return  - ease back to rest
        // g_easeAttack 0 restores the original instant onset.
        float k = 0.0f;
        if (!expiring) {
            float t = (float)age / (float)g_reactMs;
            if (t < g_easeAttack && g_easeAttack > 0.0001f) {
                float u = t / g_easeAttack;
                k = (3.0f*u*u - 2.0f*u*u*u) * r->strength;      // smoothstep in
            } else {
                float u = (g_easeAttack < 0.9999f)
                          ? (t - g_easeAttack) / (1.0f - g_easeAttack) : t;
                if (u > 1.0f) u = 1.0f;
                // Decay with a small overshoot past rest, so the body settles
                // instead of stopping dead.
                float decay = (1.0f - u) * (1.0f - u);
                float swing = g_easeOvershoot * (float)sin((double)(u * 3.14159265));
                k = (decay - swing) * r->strength;
            }
        }

        // THE FIX: rotate by the CHANGE since last frame, not by the full angle.
        // The pose buffer persists between frames, so writing the full rotation
        // every frame compounds it. Because every step turns about the same
        // axis, the increments telescope: the net rotation is always exactly k,
        // and the final k = 0 step returns the bone to its animated pose.
        // DELTA vs ABSOLUTE, and it depends on something only an experiment can
        // settle: whether the animation rebuilds this pose every frame.
        //   persists  -> writing the full rotation compounds, so send the delta
        //   rebuilt   -> the delta never gets removed, so it drifts; send the
        //                full rotation, which the fresh pose overwrites anyway
        // The evidence for "persists" was gathered with the wrong offsets, so it
        // proves nothing about the buffer that actually renders.
        // The delta/absolute choice is gone: rebasing per bone is correct
        // whether or not the buffer survived, so there is nothing left to pick.
        r->lastK = k;
        {
            // Spread the rotation UP the spine from the hit bone, each link
            // taking less: chest full, neck less, head least. Chaining the
            // other way (toward the root) reached the pelvis and blew the hips
            // out, because rotating a bone that low drags the legs with it.
            // Weights must be FIXED per link, or the unwind would not cancel.
            int   link = b;
            float w    = 1.0f;
            for (int step = 0; step < g_chainLinks && link >= 0; step++) {
                if (link < boneCount && step < SWSE_MAX_CHAIN && !expiring) {
                    // Accumulate only - the write happens once, after every
                    // reaction has had its say.
                    float ang  = k * w;
                    float half = ang * 0.5f;
                    float s = (float)sin((double)half);
                    float dq[4] = { r->axis[0]*s, r->axis[1]*s, r->axis[2]*s,
                                    (float)cos((double)half) };
                    int slot = -1;
                    for (int a = 0; a < accCount; a++)
                        if (acc[a].bone == link) { slot = a; break; }
                    if (slot < 0 && accCount < (int)(sizeof(acc)/sizeof(acc[0]))) {
                        slot = accCount++;
                        acc[slot].bone = link;
                        acc[slot].q[0] = 0; acc[slot].q[1] = 0;
                        acc[slot].q[2] = 0; acc[slot].q[3] = 1;   // identity
                        acc[slot].used = true;
                    }
                    if (slot >= 0) {
                        float res[4];
                        QuatMul(dq, acc[slot].q, res);
                        for (int c = 0; c < 4; c++) acc[slot].q[c] = res[c];
                    }

                    // Counter-rotate the arms by a fraction of what we just did
                    // to their parent, so the torso absorbs the hit while the
                    // hands - and the crossbow they are holding - stay roughly
                    // where they were pointing. g_armDamp 1.0 would hold them
                    // perfectly still; 0 lets them ride the body completely.
                    if (g_armDamp > 0.001f) {
                        const int* roots = nullptr;
                        int nRoots = CachedArmRoots(skeleton, &roots);
                        for (int ri = 0; ri < nRoots; ri++) {
                            int ab = roots[ri];
                            if (ab <= 0 || ab >= boneCount) continue;
                            // Same axis, opposite sign, scaled by the damping.
                            float ah = -ang * g_armDamp * 0.5f;
                            float as = (float)sin((double)ah);
                            float aq[4] = { r->axis[0]*as, r->axis[1]*as,
                                            r->axis[2]*as, (float)cos((double)ah) };
                            int aslot = -1;
                            for (int a2 = 0; a2 < accCount; a2++)
                                if (acc[a2].bone == ab) { aslot = a2; break; }
                            if (aslot < 0 && accCount < (int)(sizeof(acc)/sizeof(acc[0]))) {
                                aslot = accCount++;
                                acc[aslot].bone = ab;
                                acc[aslot].q[0] = 0; acc[aslot].q[1] = 0;
                                acc[aslot].q[2] = 0; acc[aslot].q[3] = 1;
                                acc[aslot].used = true;
                            }
                            if (aslot >= 0) {
                                float ares[4];
                                QuatMul(aq, acc[aslot].q, ares);
                                for (int c = 0; c < 4; c++) acc[aslot].q[c] = ares[c];
                            }
                        }
                    }
                }
                int nxt = CachedSpineChild(skeleton, link);
                if (nxt < 0) break;                     // end of the spine
                link = nxt;
                w   *= g_chainFalloff;
            }
        }

        if (!expiring) liveThisCall++;
        if (!r->applied) NoteHitBone(r->logSlot, wanted, boneCount, skeleton);
        r->applied     = true;
        r->appliedSkel = skeleton;
        r->appliedBone = wanted;
        // Nothing to unwind: the pose is rebuilt from animation and we simply
        // stop contributing, so an expiring reaction just drops.
        if (expiring) r->live = false;
    }

    // Concurrency limit, taken from the Fallout NV additive-hit-reaction mod:
    // it divides the blend weight by the number of hit animations already
    // playing and caps them at two. Accumulating instead of multiplying stops
    // reactions compounding, but nothing stopped ten simultaneous hits SUMMING
    // to an absurd angle - a burst of fire would still tear the pose apart.
    // Scaling the total back when several land at once keeps a burst reading as
    // one firm shove rather than a seizure.
    if (liveThisCall > 1) {
        float scale = 1.0f / (float)liveThisCall;
        // Do not vanish entirely under heavy fire - a floor keeps the last
        // hits readable.
        if (scale < 0.45f) scale = 0.45f;
        for (int a = 0; a < accCount; a++) {
            // Scale a quaternion's ANGLE, not its components: shrink the
            // rotation about the same axis.
            float w = acc[a].q[3];
            if (w > 1.0f) w = 1.0f;
            if (w < -1.0f) w = -1.0f;
            float ang = 2.0f * (float)acos((double)w);
            float sn  = (float)sqrt((double)(1.0 - (double)w * w));
            if (sn < 1e-5f) continue;                 // already identity
            float axis[3] = { acc[a].q[0]/sn, acc[a].q[1]/sn, acc[a].q[2]/sn };
            float half = ang * scale * 0.5f;
            float s2 = (float)sin((double)half);
            acc[a].q[0] = axis[0]*s2; acc[a].q[1] = axis[1]*s2;
            acc[a].q[2] = axis[2]*s2; acc[a].q[3] = (float)cos((double)half);
        }
    }

    // ONE write per bone, onto the animation's own value. Because nothing was
    // written during the loop above, whatever is here is the engine's pose -
    // so reactions can never read each other's output and stack.
    for (int a = 0; a < accCount; a++) {
        int b = acc[a].bone;
        if (b < 0 || b >= boneCount) continue;
        float* q = (float*)(localPose + GT_BASE + (SIZE_T)b * GT_SIZE + GT_ORIENT);
        float base[4] = { q[0], q[1], q[2], q[3] };
        float res[4];
        QuatMul(acc[a].q, base, res);
        float n = (float)sqrt((double)(res[0]*res[0]+res[1]*res[1]+
                                       res[2]*res[2]+res[3]*res[3]));
        if (n > 0.0001f) {
            q[0] = res[0]/n; q[1] = res[1]/n; q[2] = res[2]/n; q[3] = res[3]/n;
            InterlockedIncrement(&g_bonesWritten);
        }
    }
}

// Counter snapshot for the console. Reading is enough to distinguish
// "never called" from "called but nothing matched" from "bones moved".
void SWSE_HitReactStats(unsigned* calls, unsigned* writes,
                        unsigned* lastSkel, int* lastBones) {
    if (calls)     *calls     = (unsigned)g_hookCalls;
    if (writes)    *writes    = (unsigned)g_bonesWritten;
    if (lastSkel)  *lastSkel  = g_lastSkeleton;
    if (lastBones) *lastBones = g_lastBoneCount;
}

// ---- bolt hook -------------------------------------------------------------
// Bolt::vfunc17 is a method on a live bolt, so ecx is the bolt and the position
// sits at +0x24 (measured from a trapped instance: (-1.15,-1.95,-37.08) while
// the player stood at (-1.79,-7.93,-41.81) - a bolt in flight a few units
// away). Recording it every call gives a trail of where bolts actually are.
#define RVA_BOLT_UPDATE 0x91180
#define BOLT_POS_OFF    0x24

static BYTE* g_boltTramp  = nullptr;
static void* g_boltTarget = nullptr;
static BYTE  g_boltOrig[PROLOGUE_LEN];
static bool  g_boltHooked = false;

static void __stdcall NoteBolt(unsigned self) {
    if (self < 0x10000 || self >= 0x7F000000) return;
    __try {
        const float* p = (const float*)(self + BOLT_POS_OFF);
        // Reject anything that is not a plausible world position, so a layout
        // surprise records noise instead of steering reactions.
        for (int i = 0; i < 3; i++)
            if (!(p[i] > -100000.0f && p[i] < 100000.0f)) return;
        SWSE_NoteBoltPosition(p[0], p[1], p[2]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static __declspec(naked) void HookedBoltUpdate() {
    __asm {
        pushad
        pushfd
        push ecx                 // thiscall: ecx is the bolt
        call NoteBolt
        popfd
        popad
        jmp  [g_boltTramp]
    }
}

int SWSE_BoltHookInstall(char* msg, int msgLen) {
    if (g_boltHooked) { lstrcpynA(msg, "bolt hook already installed", msgLen); return 1; }
    BYTE* t = (BYTE*)((BYTE*)GetModuleHandleA(NULL) + RVA_BOLT_UPDATE);
    // Same prologue discipline as the pose hook: verify before patching.
    if (!(t[0] == 0x55 && t[1] == 0x8B && t[2] == 0xEC && t[3] == 0x83)) {
        wsprintfA(msg, "bolt prologue mismatch: %02X %02X %02X %02X - refusing",
                  t[0], t[1], t[2], t[3]);
        return 0;
    }
    g_boltTarget = t;
    g_boltTramp  = (BYTE*)VirtualAlloc(0, 32, MEM_COMMIT | MEM_RESERVE,
                                       PAGE_EXECUTE_READWRITE);
    if (!g_boltTramp) { lstrcpynA(msg, "trampoline alloc failed", msgLen); return 0; }
    memcpy(g_boltOrig, t, PROLOGUE_LEN);
    memcpy(g_boltTramp, t, PROLOGUE_LEN);
    g_boltTramp[PROLOGUE_LEN] = 0xE9;
    *(DWORD*)(g_boltTramp + PROLOGUE_LEN + 1) =
        (DWORD)((t + PROLOGUE_LEN) - (g_boltTramp + PROLOGUE_LEN + 5));
    DWORD old;
    VirtualProtect(t, PROLOGUE_LEN, PAGE_EXECUTE_READWRITE, &old);
    t[0] = 0xE9;
    *(DWORD*)(t + 1) = (DWORD)((BYTE*)HookedBoltUpdate - (t + 5));
    t[5] = 0x90;
    VirtualProtect(t, PROLOGUE_LEN, old, &old);
    g_boltHooked = true;
    lstrcpynA(msg, "bolt hook installed - tracking projectile positions", msgLen);
    return 1;
}

// ---- the hook --------------------------------------------------------------
static BYTE* g_trampoline = nullptr;
static void* g_bwpTarget  = nullptr;
static BYTE  g_bwpOrig[PROLOGUE_LEN];
static bool  g_bwpHooked  = false;

static __declspec(naked) void HookedBuildWorldPose() {
    __asm {
        pushad
        pushfd
        // pushad(32) + pushfd(4) + return address(4) = 0x28 to the first argument
        lea  eax, [esp + 0x28]
        push eax
        call ApplyReactions
        popfd
        popad
        jmp  [g_trampoline]
    }
}

int SWSE_HitReactInstall(char* msg, int msgLen) {
    if (g_bwpHooked) { lstrcpynA(msg, "already installed", msgLen); return 1; }
    g_bwpTarget = (BYTE*)GetModuleHandleA(NULL) + RVA_BUILD_WORLD_POSE;

    // Verify the prologue before patching. This check already paid for itself:
    // the breakpoint log only showed FOUR bytes (55 8B EC 83) and the fifth was
    // assumed to be EC (sub esp, imm8). The real bytes are
    //
    //     55        push ebp
    //     8B EC     mov ebp, esp
    //     83 E4 F0  and esp, 0FFFFFFF0h     <- stack ALIGNMENT, not sub
    //
    // Both forms are 3 bytes, so the 6-byte trampoline length was right either
    // way, but patching on an unverified guess is exactly how code gets
    // corrupted. Accept either ALU-with-imm8 form; both end at offset 6.
    BYTE* t = (BYTE*)g_bwpTarget;
    bool okPrologue = (t[0] == 0x55 && t[1] == 0x8B && t[2] == 0xEC && t[3] == 0x83 &&
                       (t[4] == 0xEC || t[4] == 0xE4));
    if (!okPrologue) {
        char b[160];
        wsprintfA(b, "prologue mismatch: %02X %02X %02X %02X %02X %02X - refusing to hook",
                  t[0], t[1], t[2], t[3], t[4], t[5]);
        lstrcpynA(msg, b, msgLen);
        LogG(b);
        return 0;
    }

    g_trampoline = (BYTE*)VirtualAlloc(0, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_trampoline) { lstrcpynA(msg, "trampoline alloc failed", msgLen); return 0; }

    memcpy(g_bwpOrig, t, PROLOGUE_LEN);
    memcpy(g_trampoline, t, PROLOGUE_LEN);              // relocated instructions
    g_trampoline[PROLOGUE_LEN] = 0xE9;                  // jmp back to entry+6
    *(DWORD*)(g_trampoline + PROLOGUE_LEN + 1) =
        (DWORD)((t + PROLOGUE_LEN) - (g_trampoline + PROLOGUE_LEN + 5));

    DWORD old;
    VirtualProtect(t, PROLOGUE_LEN, PAGE_EXECUTE_READWRITE, &old);
    t[0] = 0xE9;
    *(DWORD*)(t + 1) = (DWORD)((BYTE*)HookedBuildWorldPose - (t + 5));
    t[5] = 0x90;                                        // NOP the split byte
    VirtualProtect(t, PROLOGUE_LEN, old, &old);

    g_bwpHooked = true;
    LogG("HITREACT: hook installed on BuildWorldPose");
    lstrcpynA(msg, "hit reactions: hook installed", msgLen);
    return 1;
}

void SWSE_HitReactRemove() {
    if (!g_bwpHooked) return;
    // DO NOT UNPATCH. Restoring the prologue while another thread is executing
    // inside the hook (or inside the trampoline) crashes the process, and
    // BuildWorldPose runs thousands of times a second on the render thread, so
    // the window where that is unsafe is essentially always. Observed: the game
    // crashed on the next window activation immediately after a 'hitreact off'.
    //
    // Disabling is enough. ApplyReactions returns immediately when g_reactOn is
    // false, which costs a predictable-branch and nothing else - the hook was
    // measured at 53 fps against a 60 fps baseline with reactions doing real
    // work, and near zero when idle.
    g_reactOn = false;
    LogG("HITREACT: disabled (hook left in place - unpatching is not thread safe)");
}

// Damage-to-strength curve. Tunable at runtime because the right numbers
// depend on what the game's weapons actually deal, which is measured rather
// than assumed - see the hit log below. Declared here rather than beside its
// setters because the settings loader below reads it.
static float g_curveFloor  = 0.25f;   // tuned in play and signed off
static float g_curvePerDmg = 0.04f;   // input is % of the victim's health
static float g_curveMax    = 0.50f;
// Feed the curve damage as a percentage of the victim's max health rather than
// an absolute number, so one curve fits every character. Off = raw damage.
static bool  g_useDamageRatio = true;

// ---- settings persistence --------------------------------------------------
// The tuned values are already the compiled defaults, but the FEATURE started
// off and needed `hitreact on` every launch. That is the part worth
// persisting: a mod that has to be switched on by hand each session is not
// really installed.
//
// Defined here, below the curve and envelope globals, because it reads all of
// them and C++ has no tentative declarations for statics.
static void HitReactSettingsPath(char* out) {
    GetModuleFileNameA(GetModuleHandleA(NULL), out, MAX_PATH);
    char* sl = strrchr(out, '\\'); if (sl) *sl = 0;    // ...\bin
    sl = strrchr(out, '\\'); if (sl) *sl = 0;          // game root
    lstrcatA(out, "\\SWSEMods\\SWSE Combat\\hitreact.txt");
}

void SWSE_HitReactSaveSettings() {
    char path[MAX_PATH];
    HitReactSettingsPath(path);
    FILE* f = fopen(path, "w");
    if (!f) { LogG("hitreact: could not write hitreact.txt"); return; }
    fprintf(f, "# SWSE additive hit reactions\n");
    fprintf(f, "# enabled   0/1  switch on at launch\n");
    fprintf(f, "# strength       peak rotation in radians at full scale\n");
    fprintf(f, "# ms             how long a reaction lasts\n");
    fprintf(f, "# curve     floor perdmg max   damage -> scale\n");
    fprintf(f, "# ratio     0/1  feed damage as %% of max health\n");
    fprintf(f, "# ease      attack overshoot   envelope shape\n");
    fprintf(f, "# chain     links falloff      spread along the spine\n");
    fprintf(f, "# limbmass       min descendants before a bone is used\n");
    fprintf(f, "# armdamp        how much the arms resist the torso\n");
    fprintf(f, "enabled %d\n", SWSE_HitReactEnabled() ? 1 : 0);
    fprintf(f, "strength %.4f\n", g_reactStrength);
    fprintf(f, "ms %d\n", g_reactMs);
    fprintf(f, "curve %.4f %.4f %.4f\n", g_curveFloor, g_curvePerDmg, g_curveMax);
    fprintf(f, "ratio %d\n", g_useDamageRatio ? 1 : 0);
    fprintf(f, "ease %.4f %.4f\n", g_easeAttack, g_easeOvershoot);
    fprintf(f, "chain %d %.4f\n", g_chainLinks, g_chainFalloff);
    fprintf(f, "limbmass %d\n", g_minLimbMass);
    fprintf(f, "armdamp %.4f\n", g_armDamp);
    fclose(f);
    LogG("hitreact: settings saved");
}

void SWSE_HitReactLoadSettings() {
    char path[MAX_PATH];
    HitReactSettingsPath(path);
    FILE* f = fopen(path, "r");
    // No file: the compiled defaults ARE the signed-off values, so the feature
    // still comes up on. The file only exists to keep later tuning.
    int enable = 1;
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
            char key[64] = {0};
            float a = 0, b = 0, c = 0;
            if (sscanf(line, "%63s %f %f %f", key, &a, &b, &c) < 2) continue;
            if      (!lstrcmpiA(key, "enabled"))  enable = (int)a;
            else if (!lstrcmpiA(key, "strength")) g_reactStrength = a;
            else if (!lstrcmpiA(key, "ms"))       g_reactMs = (int)a;
            else if (!lstrcmpiA(key, "curve"))    SWSE_HitReactCurve(a, b, c);
            else if (!lstrcmpiA(key, "ratio"))    g_useDamageRatio = (a != 0);
            else if (!lstrcmpiA(key, "ease"))     SWSE_HitReactEase(a, b);
            else if (!lstrcmpiA(key, "chain"))    SWSE_HitReactChain((int)a, b);
            else if (!lstrcmpiA(key, "limbmass")) g_minLimbMass = (int)a;
            else if (!lstrcmpiA(key, "armdamp"))  g_armDamp = a;
        }
        fclose(f);
    }
    if (!enable) return;

    char msg[200] = {0};
    if (!SWSE_HitReactInstall(msg, sizeof(msg))) { LogG(msg); return; }
    SWSE_HitReactEnable(1);
    char bmsg[160] = {0};
    SWSE_BoltHookInstall(bmsg, sizeof(bmsg));   // impact points, not torso hits
    SWSE_HitReactWatch(1);                      // watch health drops
    LogG("hitreact: enabled at startup");
}

void SWSE_HitReactEnable(int on) { g_reactOn = (on != 0); }
int  SWSE_HitReactEnabled()      { return (g_reactOn && g_bwpHooked) ? 1 : 0; }
void SWSE_HitReactTune(float strength, int ms) {
    if (strength >= 0.0f) g_reactStrength = strength;
    if (ms > 0)           g_reactMs = ms;
}

static int QueueReactEx(unsigned skeleton, int usePos, const float* pos, float radius,
                        int bone, float dirX, float dirY, float dirZ, float strength,
                        int useImpact, const float* impact);

// Impact-driven reaction: the caller knows WHERE the projectile landed, and the
// hook resolves that to a bone. This is the entry point OnProjectileHit feeds,
// and the reason the pose layout and bone composition had to be correct first.
int SWSE_HitReactImpact(float x, float y, float z,
                        float dirX, float dirY, float dirZ, float strength) {
    float p[3] = { x, y, z };
    if (strength <= 0.0f) strength = g_reactStrength;


    // The impact point is inside the body, so the character-match cylinder can
    // be generous; picking the wrong character is prevented by the bone search
    // itself, which will find a much nearer bone on the right one.
    int slot = QueueReactEx(0, 1, p, 6.0f, -1, dirX, dirY, dirZ, strength, 1, p);
    return slot;
}

void SWSE_HitReactImpactStats(unsigned* resolved, unsigned* failed,
                              int* lastBone, float* lastDist) {
    if (resolved) *resolved = (unsigned)g_impactResolved;
    if (failed)   *failed   = (unsigned)g_impactFail;
    if (lastBone) *lastBone = g_lastImpactBone;
    if (lastDist) *lastDist = g_lastImpactDist;
}

// The hit-log slot the next queued reaction should annotate. Set by the damage
// watch immediately before queuing, so the hook can record which bone that
// specific hit ended up rotating.
static int g_pendingLogSlot = -1;
void SWSE_HitReactSetLogSlot(int slot) { g_pendingLogSlot = slot; }

static int QueueReact(unsigned skeleton, int usePos, const float* pos, float radius,
                      int bone, float dirX, float dirY, float dirZ, float strength) {
    return QueueReactEx(skeleton, usePos, pos, radius, bone, dirX, dirY, dirZ,
                        strength, 0, nullptr);
}

static int QueueReactEx(unsigned skeleton, int usePos, const float* pos, float radius,
                        int bone, float dirX, float dirY, float dirZ, float strength,
                        int useImpact, const float* impact) {
    float n = (float)sqrt((double)(dirX*dirX + dirY*dirY + dirZ*dirZ));
    if (n < 0.0001f) { dirX = 0; dirY = 0; dirZ = 1; n = 1; }
    for (int i = 0; i < MAX_REACTS; i++) {
        if (g_reacts[i].live) continue;
        g_reacts[i].skeleton  = skeleton;
        g_reacts[i].usePos    = usePos;
        g_reacts[i].radius    = radius;
        if (pos) { g_reacts[i].pos[0] = pos[0]; g_reacts[i].pos[1] = pos[1];
                   g_reacts[i].pos[2] = pos[2]; }
        g_reacts[i].bone      = bone;
        g_reacts[i].useImpact = useImpact;
        if (impact) { g_reacts[i].impact[0] = impact[0];
                      g_reacts[i].impact[1] = impact[1];
                      g_reacts[i].impact[2] = impact[2]; }
        g_reacts[i].startedAt = GetTickCount();
        g_reacts[i].strength  = strength;
        g_reacts[i].axis[0]   = dirX / n;
        g_reacts[i].axis[1]   = dirY / n;
        g_reacts[i].axis[2]   = dirZ / n;
        g_reacts[i].lastK       = 0.0f;
        g_reacts[i].logSlot     = g_pendingLogSlot;
        g_reacts[i].applied     = false;
        for (int cIdx = 0; cIdx < SWSE_MAX_CHAIN; cIdx++)
            g_reacts[i].haveBase[cIdx] = false;   // no stale base from a past use
        g_reacts[i].appliedSkel = 0;
        g_reacts[i].appliedBone = -1;
        g_reacts[i].live      = true;
        return 1;
    }
    return 0;   // all slots busy
}

int SWSE_HitReactTrigger(unsigned skeleton, int bone, float dirX, float dirY, float dirZ) {
    return QueueReact(skeleton, 0, nullptr, 0.0f, bone, dirX, dirY, dirZ, g_reactStrength);
}

// Trigger on whichever character is standing at (x,y,z). This is the form the
// damage path uses: it knows an actor's position, not a Granny skeleton.
// bone < 0 asks the hook to pick that skeleton's torso.
int SWSE_HitReactTriggerAt(float x, float y, float z, float radius, int bone,
                           float dirX, float dirY, float dirZ, float strength) {
    float p[3] = { x, y, z };
    if (strength <= 0.0f) strength = g_reactStrength;
    if (radius   <= 0.0f) radius   = 3.0f;
    return QueueReact(0, 1, p, radius, bone, dirX, dirY, dirZ, strength);
}

// ---- bolt tracking ---------------------------------------------------------
// A health drop says WHO was hit and never WHERE. The engine knows where -
// bolts stick to characters and follow their limbs - but the places that
// knowledge is exposed all came up empty: MoveBoltRayReq handed back a pool
// allocator, TakeDamage is an AI notification that never fires on the damage
// path, and SpawnEffectAttachedToBone / AttachGeometry are script entry points
// the engine's own impact code does not use. Scanning for Bolt objects works
// but they live for a few frames, so polling from the console misses them.
//
// What does work: a bolt in flight is a live object being updated every frame.
// Record where bolts are as they fly, and when damage lands, the most recent
// bolt position near the victim is the impact point. That is all the bone
// search needs - it never needed the engine's own bone index.
#define BOLT_TRACK 24
struct BoltPos { float p[3]; DWORD when; };
static BoltPos       g_boltPos[BOLT_TRACK];
static volatile LONG g_boltNext = 0;
static volatile LONG g_boltSeen = 0;

void SWSE_NoteBoltPosition(float x, float y, float z) {
    LONG i = InterlockedIncrement(&g_boltNext) - 1;
    BoltPos* b = &g_boltPos[i % BOLT_TRACK];
    b->p[0] = x; b->p[1] = y; b->p[2] = z;
    b->when = GetTickCount();
    InterlockedIncrement(&g_boltSeen);
}

// Most recent bolt within 'radius' of (x,y,z) and no older than maxAgeMs.
static bool NearestBolt(float x, float y, float z, float radius, int maxAgeMs,
                        float* out) {
    DWORD now = GetTickCount();
    float best = radius * radius;
    bool  got  = false;
    for (int i = 0; i < BOLT_TRACK; i++) {
        BoltPos* b = &g_boltPos[i];
        if (!b->when) continue;
        if ((int)(now - b->when) > maxAgeMs) continue;
        float dx = b->p[0] - x, dy = b->p[1] - y, dz = b->p[2] - z;
        float d2 = dx*dx + dy*dy + dz*dz;
        if (d2 < best) { best = d2; out[0] = b->p[0]; out[1] = b->p[1]; out[2] = b->p[2]; got = true; }
    }
    return got;
}

void SWSE_BoltStats(unsigned* seen) { if (seen) *seen = (unsigned)g_boltSeen; }

// Recent bolt positions, newest first. 597 recorded but zero matched a victim,
// so the recorded values need checking against a known position before the
// offset can be trusted.
int SWSE_BoltRecent(int i, float* pos, unsigned* ageMs) {
    if (i < 0 || i >= BOLT_TRACK) return 0;
    LONG total = g_boltNext;
    LONG idx = (total - 1 - i) % BOLT_TRACK;
    if (idx < 0) idx += BOLT_TRACK;
    BoltPos* b = &g_boltPos[idx];
    if (!b->when) return 0;
    if (pos) { pos[0] = b->p[0]; pos[1] = b->p[1]; pos[2] = b->p[2]; }
    if (ageMs) *ageMs = GetTickCount() - b->when;
    return 1;
}

// ---- damage watch ----------------------------------------------------------
// What actually fires a reaction. Hooking the damage routine would give the
// exact impact point, but it is also a way to destabilise combat code for a
// cosmetic effect. Health is already readable per NPC (actor+0x78 is the
// current/max/base triple) and a drop is an unambiguous "this one was hit",
// so the trigger is a poll rather than a second code patch. The cost is the
// impact POINT: a drop says who, not where. Direction is taken from the
// player, which is where the shot came from, and that carries most of the
// feel; an exact impact point can be layered on later without changing this.
#define WATCH_MAX 512
struct HealthSnap { unsigned actor; float hp; };
static HealthSnap  g_snap[WATCH_MAX];
static int         g_snapCount = 0;
static bool        g_watchOn   = false;
static float       g_playerHp      = 0.0f;
static bool        g_playerHpKnown = false;
static volatile LONG g_watchPolled = 0;
static volatile LONG g_watchHits   = 0;

void SWSE_HitReactWatch(int on) {
    g_watchOn = (on != 0);
    // Forget the baselines, or re-enabling fires a reaction on everything that
    // lost health while the watch was off.
    if (!on) { g_snapCount = 0; g_playerHpKnown = false; }
}
int  SWSE_HitReactWatching() { return g_watchOn ? 1 : 0; }
void SWSE_HitReactWatchStats(unsigned* polled, unsigned* hits) {
    if (polled) *polled = (unsigned)g_watchPolled;
    if (hits)   *hits   = (unsigned)g_watchHits;
}

void SWSE_HitReactMatchStats(unsigned* match, unsigned* reject, unsigned* noMatrix,
                             unsigned* torsoFail, unsigned* boneRange) {
    if (match)     *match     = (unsigned)g_posMatch;
    if (reject)    *reject    = (unsigned)g_posReject;
    if (noMatrix)  *noMatrix  = (unsigned)g_noMatrix;
    if (torsoFail) *torsoFail = (unsigned)g_torsoFail;
    if (boneRange) *boneRange = (unsigned)g_boneRange;
}

void SWSE_HitReactDamageRatio(int on) { g_useDamageRatio = (on != 0); }
int  SWSE_HitReactDamageRatioOn()     { return g_useDamageRatio ? 1 : 0; }

void SWSE_HitReactCurve(float floorV, float perDmg, float maxV) {
    if (floorV >= 0.0f) g_curveFloor  = floorV;
    if (perDmg >= 0.0f) g_curvePerDmg = perDmg;
    if (maxV   >  0.0f) g_curveMax    = maxV;
}
void SWSE_HitReactCurveGet(float* floorV, float* perDmg, float* maxV) {
    if (floorV) *floorV = g_curveFloor;
    if (perDmg) *perDmg = g_curvePerDmg;
    if (maxV)   *maxV   = g_curveMax;
}

// Log of recent hits. Tuning "weak weapons should feel weak" needs the actual
// damage numbers - a zapfly tick and a crossbow bolt were being guessed at.
// Each hit records the bone it ended up rotating. Without this the only way to
// judge bone selection is to watch the character and guess, and "the arm is
// deforming, not the torso" is exactly the kind of thing that needs a number
// rather than an impression.
struct HitLog { float dmg, scale; DWORD when; int bone; int bones; unsigned skel; };
#define HITLOG_MAX 24
static HitLog        g_hitLog[HITLOG_MAX];
static volatile LONG g_hitLogNext = 0;

static int RecordHit(float dmg, float scale) {
    LONG i = InterlockedIncrement(&g_hitLogNext) - 1;
    int slot = (int)(i % HITLOG_MAX);
    HitLog* h = &g_hitLog[slot];
    h->dmg = dmg; h->scale = scale; h->when = GetTickCount();
    h->bone = -1; h->bones = 0; h->skel = 0;   // filled in by the hook
    return slot;
}

// Called from the hook once a reaction has resolved its bone.
static void NoteHitBone(int slot, int bone, int boneCount, unsigned skel) {
    if (slot < 0 || slot >= HITLOG_MAX) return;
    g_hitLog[slot].bone  = bone;
    g_hitLog[slot].bones = boneCount;
    g_hitLog[slot].skel  = skel;
}

int SWSE_HitReactHitLog(int i, float* dmg, float* scale, unsigned* ageMs,
                        int* bone, int* bones, unsigned* skel) {
    LONG total = g_hitLogNext;
    int have = (total < HITLOG_MAX) ? (int)total : HITLOG_MAX;
    if (i < 0 || i >= have) return 0;
    // Newest first.
    LONG idx = (total - 1 - i) % HITLOG_MAX;
    if (idx < 0) idx += HITLOG_MAX;
    HitLog* h = &g_hitLog[idx];
    if (dmg)   *dmg   = h->dmg;
    if (scale) *scale = h->scale;
    if (ageMs) *ageMs = GetTickCount() - h->when;
    if (bone)  *bone  = h->bone;
    if (bones) *bones = h->bones;
    if (skel)  *skel  = h->skel;
    return 1;
}

// Health parallel to g_watchActors, so looking up "what was this actor's health
// last frame" is an array index rather than a search. The previous version
// searched the whole snapshot per actor - 316 actors x 316 entries every frame.
static float g_watchHp[WATCH_MAX];
static bool  g_watchHpKnown[WATCH_MAX];

// SWSE_FindNpcs walks the heap. Calling it every frame measured 5.90 fps
// against 59 - a 10x collapse - so the actor LIST is refreshed on a timer while
// health is polled every frame from the cached pointers, which is just a read
// per NPC. Actors can be freed between refreshes, hence the guarded reads and a
// short refresh interval.
// A 2 s refresh measured 55 fps in a 206-NPC level and 7 fps in a 316-NPC one:
// the heap scan grows with the level and starts dominating whole frames. Two
// changes: refresh far less often, and never rescan on the "empty" path every
// frame - a scan that returns nothing used to re-run immediately, turning one
// slow scan into one per frame.
#define WATCH_REFRESH_MS 15000
#define WATCH_EMPTY_BACKOFF_MS 3000
static unsigned g_watchActors[WATCH_MAX];
static int      g_watchActorCount = 0;
static DWORD    g_watchRefreshed  = 0;

int SWSE_WatchActors(unsigned* out, int maxOut) {
    int n = (g_watchActorCount < maxOut) ? g_watchActorCount : maxOut;
    for (int i = 0; i < n; i++) out[i] = g_watchActors[i];
    return n;
}

// Per-phase timing. Two rounds of optimisation guessed at the cost and both
// guesses were wrong (55->14 fps unchanged), so measure which phase is actually
// expensive instead of reasoning about it.
static double g_tickMsScan = 0.0, g_tickMsPoll = 0.0, g_tickMsPlayer = 0.0;
static double g_tickMsScanMax = 0.0, g_tickMsPollMax = 0.0, g_tickMsPlayerMax = 0.0;

static double NowMs() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

void SWSE_HitReactTickStats(double* scan, double* poll, double* player,
                            double* scanMax, double* pollMax, double* playerMax) {
    if (scan)      *scan      = g_tickMsScan;
    if (poll)      *poll      = g_tickMsPoll;
    if (player)    *player    = g_tickMsPlayer;
    if (scanMax)   *scanMax   = g_tickMsScanMax;
    if (pollMax)   *pollMax   = g_tickMsPollMax;
    if (playerMax) *playerMax = g_tickMsPlayerMax;
}

void SWSE_HitReactTick() {
    if (!g_watchOn || !g_reactOn) return;

    double t0 = NowMs();
    DWORD now = GetTickCount();
    // Refresh INCREMENTALLY. Doing the whole heap walk in one frame cost
    // 150-270 ms and was the source of the intermittent freezes during play -
    // confirmed by frame attribution, not guessed ("frame 190 ms, SWSE work
    // 156 ms"). A ~1.5 ms slice per frame spreads the same work over a second
    // or two of play and never shows up as a hitch. The previous list stays
    // in use until a pass completes, so polling never sees a partial set.
    static bool scanning = false;
    static int  refreshes = 0;
    DWORD due = (g_watchActorCount == 0) ? WATCH_EMPTY_BACKOFF_MS : WATCH_REFRESH_MS;
    if (!scanning && (now - g_watchRefreshed) > due) {
        // Cheap path first: re-check the pointers we already hold instead of
        // rediscovering the same actors. Only every 4th refresh (~60 s) pays
        // for a full walk, as a backstop against anything the validation
        // cannot see.
        bool full = (g_watchActorCount == 0) || ((++refreshes % 4) == 0);
        if (!full) {
            int before = g_watchActorCount;
            int v = SWSE_ValidateNpcs(g_watchActors, before);
            // Losing a quarter of the list means the world changed under us -
            // a level load - so rebuild rather than limp on with survivors.
            if (v < 0 || v < (before * 3) / 4) {
                full = true;
            } else {
                g_watchActorCount = v;
                g_watchRefreshed  = now;
                for (int i = 0; i < WATCH_MAX; i++) g_watchHpKnown[i] = false;
            }
        }
        if (full) scanning = true;
    }
    if (scanning) {
        int complete = 0;
        unsigned fresh[WATCH_MAX];
        // Two speeds, because the two situations are different.
        //
        // With NO list at all the feature does nothing, so finishing fast
        // matters more than smoothness - and this case only arises just after
        // a level load, where the frame rate is already disrupted. With a list
        // in hand the old one stays valid while the next pass runs, so it can
        // afford to be slow and invisible.
        //
        // Measured throughput is only ~85 MB/s (the scan touches committed but
        // paged-out memory, so it is page-fault bound, not bandwidth bound), so
        // a 3 ms slice covers ~256 KB and a 768 MB pass takes ~25 s. That is
        // fine for a refresh and useless for a cold start.
        double budget = (g_watchActorCount == 0) ? 15.0 : 3.0;
        int r = SWSE_FindNpcsStep(fresh, WATCH_MAX, budget, &complete);
        if (complete) {
            scanning = false;
            {   // Did a pass finish, and with what? "0 polls" could mean the
                // scan never completes OR completes empty; these are different
                // bugs and the counter alone cannot tell them apart.
                char b[120];
                wsprintfA(b, "watch: scan pass complete, %d actor(s)", r < 0 ? 0 : r);
                LogG(b);
            }
            g_watchRefreshed = now;            // set even on an empty result,
            g_watchActorCount = (r < 0) ? 0 : r;   // so it backs off instead
            for (int i = 0; i < g_watchActorCount; i++) g_watchActors[i] = fresh[i];
            // The list changed, so last frame's health no longer lines up.
            for (int i = 0; i < WATCH_MAX; i++) g_watchHpKnown[i] = false;
        }
    }
    g_tickMsScan = NowMs() - t0;
    if (g_tickMsScan > g_tickMsScanMax) g_tickMsScanMax = g_tickMsScan;

    unsigned* scan = g_watchActors;
    int n = g_watchActorCount;
    if (n <= 0) return;
    InterlockedIncrement(&g_watchPolled);

    double t1 = NowMs();
    // Read the position off the PLAYER OBJECT, not the script context.
    // SWSE_PosGet depends on a ScriptContext that is not always primed - it
    // returned 0,0,0 for a whole session while damage detection worked fine,
    // so every reaction was aimed at the origin and matched nobody
    // (posReject 19493, zero matches). Health already comes from the player
    // object; the position sits at +0x24 of that same object, so taking both
    // from one place removes the dependency entirely.
    float  playerPos[3] = { 0, 0, 0 };
    bool   havePlayer = (SWSE_PlayerGet(0x24, &playerPos[0], &playerPos[1],
                                        &playerPos[2]) == 1);
    if (havePlayer && playerPos[0] == 0.0f && playerPos[1] == 0.0f &&
        playerPos[2] == 0.0f) {
        // All-zero means the object was not really resolved; fall back rather
        // than aim every reaction at the world origin.
        havePlayer = (SWSE_PosGet(playerPos) == 1);
    }
    g_tickMsPlayer = NowMs() - t1;
    if (g_tickMsPlayer > g_tickMsPlayerMax) g_tickMsPlayerMax = g_tickMsPlayer;
    double t2 = NowMs();

    // The player is not in the NPC list, so without this you can be shot to
    // pieces and never flinch - which is the one reaction you are guaranteed to
    // be looking at in third person.
    if (havePlayer) {
        float cur = 0, mx = 0, base = 0;
        if (SWSE_PlayerHealth(&cur, &mx, &base) && cur > 0.0f) {
            if (g_playerHpKnown && cur < g_playerHp - 0.01f) {
                InterlockedIncrement(&g_watchHits);
                float dmg = g_playerHp - cur;
                float scale = g_curveFloor + dmg * g_curvePerDmg;
                if (scale > g_curveMax) scale = g_curveMax;
                SWSE_HitReactSetLogSlot(RecordHit(dmg, scale));
                // Direction from the projectile that just arrived. A fixed axis
                // was arbitrary: depending on which way the character faced it
                // twisted the torso sideways and threw the arms around instead
                // of bending the body away from the shot. The bolt trail gives
                // the real incoming vector.
                float ax = 1.0f, ay = 0.0f, az = 0.0f;
                float imp[3];
                bool haveImpact = NearestBolt(playerPos[0], playerPos[1], playerPos[2],
                                              15.0f, 400, imp);
                if (haveImpact) {
                    float dx = playerPos[0] - imp[0];
                    float dy = playerPos[1] - imp[1];
                    // Rotate ABOUT the horizontal perpendicular, so the body
                    // bends along the bolt's path rather than spinning on it.
                    ax = -dy; ay = dx; az = 0.0f;
                    if (ax*ax + ay*ay < 1e-6f) { ax = 1.0f; ay = 0.0f; }
                }
                if (haveImpact) {
                    // Impact point known: let the hook pick the struck bone.
                    SWSE_HitReactImpact(imp[0], imp[1], imp[2],
                                        ax, ay, az, g_reactStrength * scale);
                } else {
                    SWSE_HitReactTriggerAt(playerPos[0], playerPos[1], playerPos[2],
                                           3.0f, -1, ax, ay, az,
                                           g_reactStrength * scale);
                }
            }
            g_playerHp      = cur;
            g_playerHpKnown = true;
        } else {
            g_playerHpKnown = false;
        }
    }

    // One SEH frame for the whole sweep rather than one per actor. Entering and
    // leaving a __try 316 times a frame is a real cost, and a fault here means
    // the actor list is stale - which is handled by dropping the whole sweep and
    // forcing a refresh, not by skipping one entry.
    static float hp[WATCH_MAX], hpMax[WATCH_MAX];
    static float px[WATCH_MAX], py[WATCH_MAX], pz[WATCH_MAX];
    __try {
        for (int i = 0; i < n; i++) {
            unsigned a = scan[i];
            const float* h = (const float*)(a + 0x78);    // current / max / base
            hp[i]    = h[0];
            hpMax[i] = h[1];
            const float* ap = (const float*)(a + 0x24);   // actor position copy
            px[i] = ap[0]; py[i] = ap[1]; pz[i] = ap[2];
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_watchActorCount = 0;                 // stale list; refresh after backoff
        g_watchRefreshed  = now;
        return;
    }

    for (int i = 0; i < n; i++) {
        bool  known = g_watchHpKnown[i];
        float was   = g_watchHp[i];

        // A KILLING BLOW IS STILL A HIT. Skipping actors already at zero health
        // is right, but skipping them BEFORE comparing to last frame threw away
        // the full-health-to-dead transition - so anything that dies in one shot
        // never reacted at all. Chickens die in one hit, which is why they
        // looked immune. Let the drop through once, then stop tracking.
        if (hp[i] <= 0.0f) {
            if (known && was > 0.01f) {
                hp[i] = 0.0f;                 // the drop is 'was' -> 0
            } else {
                g_watchHpKnown[i] = false;
                continue;
            }
        }
        // A drop, not a heal, and not the first sighting (which would fire on
        // every NPC the moment the watch is switched on).
        if (known && hp[i] < was - 0.01f) {
            InterlockedIncrement(&g_watchHits);
            // Push away from the player: that is where the bolt came from.
            float dx = 0, dy = 0;
            if (havePlayer) { dx = px[i] - playerPos[0]; dy = py[i] - playerPos[1]; }
            // The rotation axis is perpendicular to the push, so the torso
            // rotates AROUND the impact rather than spinning about it.
            float ax = -dy, ay = dx, az = 0.0f;
            if (ax*ax + ay*ay < 1e-6f) { ax = 1; ay = 0; }
            // Weapon feedback scales with damage dealt. The first curve was
            // 0.6 + dmg*0.02, which gave a 1-damage zapfly tick 62% of full
            // strength and capped at 200% - a 3x spread across every weapon in
            // the game, so nothing felt different from anything else. The floor
            // has to be near zero for weak weapons to read as weak.
            float dmg = was - hp[i];
            // Reject nonsense. The actor list is refreshed on a timer, so a
            // pointer can be freed and its memory reused between refreshes -
            // the health read then returns garbage and the log fills with
            // entries like "-2147483648 dmg", every one of them slamming the
            // curve to its cap and firing a maximum-strength reaction.
            if (!(dmg > 0.0f) || !(dmg < 100000.0f)) { g_watchHpKnown[i] = false; continue; }
            // Scale by damage as a FRACTION of this character's health, not by
            // the raw number. A crossbow bolt takes 1.4 off a Wolvark and 10
            // off the player - identical shots, wildly different numbers -
            // so an absolute curve made enemy reactions invisible while the
            // player's read fine. A ratio means "a scratch" and "nearly killed
            // me" mean the same thing on any character, including rigs we have
            // never seen.
            float dmgUnit = dmg;
            if (g_useDamageRatio && hpMax[i] > 1.0f) dmgUnit = 100.0f * dmg / hpMax[i];
            float scale = g_curveFloor + dmgUnit * g_curvePerDmg;
            if (scale > g_curveMax) scale = g_curveMax;
            if (scale < 0.0f)       scale = 0.0f;
            SWSE_HitReactSetLogSlot(RecordHit(dmg, scale));
            // If a bolt was near this character in the last moment, that is
            // where it was hit - use it and let the hook resolve the bone.
            // Otherwise fall back to the torso.
            float imp[3];
            if (NearestBolt(px[i], py[i], pz[i], 12.0f, 300, imp)) {
                SWSE_HitReactImpact(imp[0], imp[1], imp[2],
                                    ax, ay, az, g_reactStrength * scale);
            } else {
                SWSE_HitReactTriggerAt(px[i], py[i], pz[i], 3.0f, -1,
                                       ax, ay, az, g_reactStrength * scale);
            }
        }
        g_watchHp[i]      = hp[i];
        // Stop tracking the dead: their health stays at zero, so leaving them
        // 'known' would just re-test a drop that already fired.
        g_watchHpKnown[i] = (hp[i] > 0.0f);
    }
    g_tickMsPoll = NowMs() - t2;
    if (g_tickMsPoll > g_tickMsPollMax) g_tickMsPollMax = g_tickMsPoll;
}

// The bones a reaction would actually touch on the last skeleton posed. "The
// arm is deforming" needs checking against which bones are written, not
// inferred from the picture - the same instrument-first approach that caught
// the head/chest mix-up.
int SWSE_HitReactChainPath(int* out, int maxOut, unsigned* skelOut, int* boneCount) {
    // Caller supplies the skeleton via *skelOut/*boneCount when it knows which
    // one it wants; the last-posed skeleton is usually a prop, not a character.
    unsigned skel = (skelOut && *skelOut) ? *skelOut : 0;
    int      n    = (boneCount && *boneCount > 0) ? *boneCount : 0;
    if (!skel || n <= 0) SWSE_HitReactStats(nullptr, nullptr, &skel, &n);
    if (skelOut)   *skelOut   = skel;
    if (boneCount) *boneCount = n;
    if (!skel || n <= 0) return 0;
    int bone = FindTorsoBone(skel, n);
    if (bone < 0) return 0;
    int links = 0; float fall = 0;
    SWSE_HitReactChainGet(&links, &fall);
    int count = 0, link = bone;
    for (int step = 0; step < links && link >= 0 && count < maxOut; step++) {
        out[count++] = link;
        int nxt = CachedSpineChild(skel, link);
        if (nxt < 0) break;
        link = nxt;
    }
    return count;
}

// Fault count, defined at the end so it sits after the counter it reads.
void SWSE_HitReactFaults(unsigned* faults) { if (faults) *faults = (unsigned)g_faulted; }

