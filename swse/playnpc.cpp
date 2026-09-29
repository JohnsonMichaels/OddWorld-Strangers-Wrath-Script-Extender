// SWSE playnpc - play as another character resident in the level.
// See playnpc.h for the interface and research/PLAYNPC.md for the RE behind
// every address in this file (all disassembled from the HD exe; offsets that
// the beta PDB names are given their beta name).
//
// WHAT CHANGES, AND WHAT DOES NOT
//   The player OBJECT, its controller, camera, collision, physics, weapons,
//   inventory and FORM (Stranger / Steef) are never swapped. Three things are:
//     1. the geometry the player's actor draws: a fresh instance of the NPC's
//        GeometryHierarchyDef, installed with the engine's own SetGeometry;
//     2. the AnimationLayer of the player's CURRENT MotionImpl: rebuilt from
//        the NPC's MotionAnimConfig, bound to the new instance, and pointed at
//        a PRIVATE copy of that config whose state lookup fills the gaits the
//        NPC lacks (run, trot, canter, turnarounds, skids, bursts, landings)
//        from the NPC's OWN clips - never a Stranger/Steef clip;
//     3. four player rules while an NPC: no double jump, no first person
//        (the engine's own camera lock), no manual disguise toggle (the
//        engine's own lock), and no foot IK (Stranger's IK chains index
//        Stranger's bones - on another skeleton they read out of bounds).
//   `playnpc off` puts each back; nothing else is touched.
//
// NO LONG-LIVED REFERENCES. The NPC instance is owned by the player (via
// SetGeometry) and by its animation layer; the layer is owned by the motion.
// Stranger's own instance stays owned by the player's mode data. SWSE holds
// only pointers, validated every frame, so a level change, death or reload
// that tears the objects down leaves nothing dangling behind us.
//
// KEEPING THE CHARACTER LOADED. A character's body and animations live in
// streamed data blocks that the engine keeps loaded only while something
// needs them. playnpc says "needed" the way a live NPC of the type does -
// one per-frame counter per block - so walking away no longer unloads the
// body, and a character that is not streamed in is loaded on demand. The
// engine zeroes those counters every frame, so there is nothing to undo.
//
// Default OFF. Nothing is armed until `playnpc <name>` is typed. Every code
// patch is temporary: a one-slot PlayerImpl vtable swap for the length of a
// spin (about a second); and, while a character is on, one call in the walk
// anims redirected through the gait guard, the weapon tick's call redirected,
// the buck button's jump flipped, and four of the player's attack slots
// swapped (see "the gait guard" and "Stranger's attacks"). Each is checked
// byte for byte, reported to `hooks`, and put back when its reason ends. Every
// write is refused on a game build SWSE's addresses were not measured on.

#include "playnpc.h"
#include "console.h"
#include "scriptvm.h"
#include "prefsedit.h"
#include "levelwatch.h"
#include "gamebuild.h"
#include "hookreg.h"    // every patch reported to the one list (`hooks`)
#include "input.h"      // SWSE_QueueKey: `playnpc runtest` drives a real run
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

// ---------------------------------------------------------------------------
//  addresses (RVAs; the exe is ASLR'd, VA = image base + RVA)
// ---------------------------------------------------------------------------

// vtables (RTTI-named)
#define RVA_VT_PLAYERIMPL     0x36ACE4
#define RVA_VT_MOTIONIMPL     0x37A924
#define RVA_VT_GHI            0x388A94    // GeometryHierarchyInst
#define RVA_VT_GHD            0x389DBC    // GeometryHierarchyDef
#define RVA_VT_ANIMLAYER      0x379E2C
#define RVA_VT_ANIMCFG        0x379E8C    // AnimationLayerConfig
#define RVA_VT_NPCPREFS       0x367E1C

// functions
#define RVA_CREATE_ANIMLAYER  0x11FEF0    // cdecl (SPtr* out, GHI*, const char* path, info*)
#define RVA_MOTION_ANIMINFO   0x400574    // the MotionAnimationListInfo SetAnimConfig passes
#define RVA_SET_TICKFLAGS     0x2057E0    // eax=IK array; stack (inst, flags, count); ret 0xC
#define RVA_APPLY_ATTACH      0x032200    // cdecl (GameObject*, attachment vector*, bool)
#define RVA_FINISHDISGUISE    0x047D50    // PlayerImpl vfunc152 - flags the form swap
#define RVA_HANDLE_GEN        0x5D55F4    // u16 generation, 6-byte stride, per handle index
#define RVA_RESMGR            0x5D55A8    // ResourceManager*; registry vector at +0x30
#define RVA_ANIM_NAMES        0x363C20    // const char*[150] "ANIM_*" / "TORSOANIM_*"

// PlayerImpl
#define PL_GEOM               0x014       // GameObject::m_geometry (current body)
#define PL_MOTION             0x0B0       // Actor::m_motion (current MotionImpl)
#define PL_MODEDATA           0x188       // SteefModeData[2]: [0] Steef, [1] Stranger
#define MD_STRIDE             0x1C
#define MD_MOTION             0x00
#define MD_GEO                0x04
#define MD_IKCOUNT            0x10        // m_IKChains {ptr +8, cap +0xC, count +0x10}
#define PL_DISGUISED          0x1E9       // 1 = Stranger
#define PL_DISGUISELOCK       0x1EA
#define PL_CAMLOCK            0x494       // Player::ELockCamera: 0 off, 1 1st, 2 3rd
#define PLV_SETGEOMETRY       0x088
#define PLV_SETCAMLOCK        0x1B0
#define PLV_FINISHDISGUISE    0x260       // slot 152

// MotionImpl
#define MI_OWNER              0x08
#define MI_PREFS              0x14        // MotionPrefs*
#define MI_LAYER              0x18        // SPtr<AnimationLayer>
#define MI_MOTCURRENT         0x1C
#define MI_PHYSICS            0x34        // 0 land, 1 water, 2 air, 3 boat, 4 pipe
#define MI_JUMPCANDOUBLE      0x1F0       // bool, set by StartJump (0x53BB30)
#define MIV_REQCHANGE         0x14C       // RequestChange(EChange) -> ChangeEnter
#define MIV_REQCHANGE2        0x150       // -> ChangeLeave
#define MOT_CHANGEENTER       26
#define MOT_CHANGELEAVE       27

// prefs
#define MP_ANIMCFG            0x4C        // MotionPrefs::m_animationConfigFile
#define NP_ANIMCFG            0xD8        // NPCPrefs m_spMotionPrefs.m_animationConfigFile
#define NP_GEOMETRY           0x438
#define NP_SCALEMIN           0x440
#define NP_SCALEMAX           0x444
#define NP_ATTACH             0x4D0       // m_attachments: NPC::Init applies it (flag 0) - empty on every retail character (AT3)
#define NP_DEFATTACH          0x4DC       // m_defaultAttachments: what a spawn that names none wears (flag 1) - hats, weapons

// Resource header
#define RES_REF               0x04        // u16 refcount
#define RES_IDX               0x06        // u16 handle index
#define RES_BLOCK             0x08        // owning SharedMemoryBlock* | bit0 resident | bit1 override
                                          // (bit1: the block is mid-transition, e.g. waiting for release)
#define RES_HASH              0x0C

// GeometryHierarchyInst / Def
#define GHI_FRAME             0x30
#define GHI_SCALE             0x60
#define GHI_DEF               0x64
#define GHIV_RECOMPOSE        0x50
#define GHDV_CREATEINST       0x48

// AnimationLayer / AnimationLayerConfig
#define AL_GHI                0x48
#define AL_CONFIG             0x4C
#define AC_SIZE               0x2C
#define AC_LOOKUP             0x20        // {ptr, cap, count} of AnimationLayerAnimConfig*
#define AC_LOOKUPCAP          0x24
#define AC_LOOKUPN            0x28

// AnimationLayerConfig's animation list (what Motion::AddAnimationBlocks walks)
#define AC_ANIMS              0x14        // {AnimationLayerAnimConfig (0x10 each), cap, count}
#define AC_ANIMSN             0x1C
#define AAC_STRIDE            0x10
#define AAC_SRCS              0x08        // {source (0x14 each), count}
#define AAC_SRCSN             0x0C
#define SRC_STRIDE            0x14
#define SRC_ANIMHASH          0x08        // the Animation resource's name hash
#define ATT_STRIDE            0x3C        // NP_ATTACH element (0x4323C4 add edi,0x3C)
#define ATT_GEOTOKEN          0x04        // its geometry's name hash

// Streaming: data blocks (SharedMemoryBlock, one .smb bundle each)
#define RVA_VT_BLOCK          0x388FF0    // SharedMemoryBlock
#define RVA_VT_ANIM           0x3890F4    // Animation
#define RVA_NPCBLOCKS         0x63BA4C    // the scheduler's npc block list {block** data, cap, count}
#define RVA_BLOCKQUEUE        0x63BAE4    // the block manager's requests {{block, 0 load/1 release}*, cap, count}
#define BLK_LAZYNEED          0x14        // m_lazyNeedRefCount (beta +0x1C)
#define BLK_FILENAME          0x1C        // StringBuffer "\data\bundles\region_02\lm_level_02\npc_8.smb"
#define BLK_RESIDENCY         0x58        // m_residency: 0 not, 1 resident, 2 waiting for release
#define MAX_DEPS              16          // body, animations, attachments, and what their materials draw from
#define LOAD_TIMEOUT_MS       20000
#define LAND_WAIT_MS          15000

#define NSTATES               150         // 0..133 ANIM_*, 134..149 TORSOANIM_*
#define HASH_NULL             0x2DFD1072u // hash("") - the engine's empty token
#define SCALE_MAX_SAFE        1.9f        // GIANTSLEG.md: bone palette wraps at 2.0
#define SCALE_MIN_SAFE        0.3f
#define PIN_REFCOUNT          0x4000

// ---------------------------------------------------------------------------
//  small helpers
// ---------------------------------------------------------------------------

static unsigned Base() { return (unsigned)(uintptr_t)GetModuleHandleA(NULL); }

// The install may carry the 4GB (large-address-aware) patch, so live objects
// can sit above 2 GB. Anything in user space is a candidate; every access is
// SEH-guarded, and every object is identified by its vtable before use.
#define ADDR_LO 0x10000u
#define ADDR_HI 0xFFFE0000u
static bool IsHeap(unsigned p) { return p >= ADDR_LO && p < ADDR_HI && !(p & 3); }

static bool Rd(unsigned addr, unsigned* out) {
    if (addr < ADDR_LO || addr >= ADDR_HI) return false;
    __try { *out = *(unsigned*)addr; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static unsigned R32(unsigned addr) { unsigned v = 0; Rd(addr, &v); return v; }
static bool RdB(unsigned addr, BYTE* out) {
    if (addr < ADDR_LO || addr >= ADDR_HI) return false;
    __try { *out = *(BYTE*)addr; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool Wr(unsigned addr, unsigned v) {
    if (addr < ADDR_LO || addr >= ADDR_HI) return false;
    __try { *(unsigned*)addr = v; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool WrB(unsigned addr, BYTE v) {
    if (addr < ADDR_LO || addr >= ADDR_HI) return false;
    __try { *(BYTE*)addr = v; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static float RF(unsigned addr) { unsigned v = R32(addr); return *(float*)&v; }

// Is obj an object whose vtable is exactly RVA `vt`?
static bool VtIs(unsigned obj, unsigned vtRva) {
    unsigned v = 0;
    return IsHeap(obj) && Rd(obj, &v) && v == Base() + vtRva;
}

static void Printf(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    SWSE_ConsolePrint(buf);
}

static void Log(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    char path[MAX_PATH];
    GetModuleFileNameA(GetModuleHandleA(NULL), path, MAX_PATH);
    char* sl = strrchr(path, '\\'); if (sl) *sl = 0;
    char full[MAX_PATH];
    _snprintf_s(full, sizeof(full), _TRUNCATE, "%s\\swse_log.txt", path);
    FILE* f = fopen(full, "a");
    if (!f) return;
    fprintf(f, "playnpc: %s\n", buf);
    fclose(f);
}

// A char* out of a StringBuffer field (StringBuffer -> data -> char*).
static const char* StrField(unsigned addr) {
    __try {
        unsigned sb = *(unsigned*)addr;
        if (!IsHeap(sb)) return nullptr;
        const char* s = *(const char**)sb;
        if ((unsigned)(uintptr_t)s < 0x10000) return nullptr;
        if (!s[0]) return nullptr;
        return s;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// ---------------------------------------------------------------------------
//  reference counting, exactly as the engine does it
// ---------------------------------------------------------------------------
//   inc word [obj+4]
//   dec word [obj+4]; if 0: inc word [0x9D55F4 + idx*6]; obj->vtable[0](1)

static bool RefAdd(unsigned obj) {
    if (!IsHeap(obj)) return false;
    __try { (*(unsigned short*)(obj + RES_REF))++; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool RefRelease(unsigned obj) {
    if (!IsHeap(obj)) return false;
    __try {
        unsigned short* rc = (unsigned short*)(obj + RES_REF);
        if (*rc == 0) return false;               // never underflow a count
        if (--*rc == 0) {
            unsigned short idx = *(unsigned short*)(obj + RES_IDX);
            (*(unsigned short*)(Base() + RVA_HANDLE_GEN + idx * 6))++;
            typedef void (__thiscall* dtor_t)(void*, int);
            dtor_t d = (dtor_t)(*(unsigned*)(*(unsigned*)obj));
            d((void*)obj, 1);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---------------------------------------------------------------------------
//  engine calls (each SEH-guarded; 0 / false on any fault)
// ---------------------------------------------------------------------------

typedef void* (__thiscall* vfPtr1_t)(void* self, void* a);
typedef void  (__thiscall* vf0_t)(void* self);
typedef void  (__thiscall* vfInt1_t)(void* self, int a);
typedef unsigned char (__thiscall* vfBool1_t)(void* self, int a);
typedef void* (__cdecl* createLayer_t)(void** out, void* ghi, const char* path, void* info);
typedef void  (__cdecl* applyAttach_t)(void* go, void* vec, int flag);

static unsigned VFn(unsigned obj, unsigned slot) {
    unsigned vt = R32(obj);
    return vt ? R32(vt + slot) : 0;
}

// GeometryHierarchyDef::CreateInstance -> a new GHI holding one reference.
static unsigned CreateInstance(unsigned def) {
    unsigned fn = VFn(def, GHDV_CREATEINST);
    if (!fn) return 0;
    void* out = nullptr;
    __try { ((vfPtr1_t)fn)((void*)def, &out); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    unsigned inst = (unsigned)(uintptr_t)out;
    return VtIs(inst, RVA_VT_GHI) ? inst : 0;
}

static bool Recompose(unsigned inst) {
    unsigned fn = VFn(inst, GHIV_RECOMPOSE);
    if (!fn) return false;
    __try { ((vf0_t)fn)((void*)inst); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// GameObject/Actor::SetGeometry. It does the whole install: removes the old
// body from the spatial index and makes it non-resident, copies the old
// frame AND SCALE onto the new one, sets its owner back-pointer, makes it
// resident, and adds it to the spatial index. SetGeometry takes its own
// reference; the caller keeps whatever it held.
static bool SetGeometry(unsigned player, unsigned inst) {
    unsigned fn = VFn(player, PLV_SETGEOMETRY);
    if (!fn) return false;
    void* sp = (void*)inst;
    __try { ((vfPtr1_t)fn)((void*)player, &sp); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return R32(player + PL_GEOM) == inst;
}

// The player tick does this for its current body every frame (with its IK
// chains); done once here so the pose buffers exist before the first render.
// flags 5 = local pose + world pose. No IK chains.
static bool SetTickFlags(unsigned inst, int flags) {
    unsigned fn = Base() + RVA_SET_TICKFLAGS;
    int ok = 1;
    __try {
        __asm {
            push 0
            push flags
            push inst
            xor  eax, eax
            call fn
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    return ok != 0;
}

static unsigned CreateAnimLayer(unsigned ghi, const char* path) {
    if (!path || !VtIs(ghi, RVA_VT_GHI)) return 0;
    void* out = nullptr;
    __try {
        ((createLayer_t)(Base() + RVA_CREATE_ANIMLAYER))(&out, (void*)ghi, path,
                                                         (void*)(Base() + RVA_MOTION_ANIMINFO));
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    unsigned layer = (unsigned)(uintptr_t)out;
    return VtIs(layer, RVA_VT_ANIMLAYER) ? layer : 0;
}

// Replace motion->m_spAnimationLayer with `layer`, taking over the one
// reference the caller holds. Same order as MotionImpl::SetAnimConfig
// (0x527E80): ref the new, release the old, store, drop the caller's.
static bool InstallLayer(unsigned motion, unsigned layer) {
    if (!VtIs(motion, RVA_VT_MOTIONIMPL) || !VtIs(layer, RVA_VT_ANIMLAYER)) return false;
    unsigned old = R32(motion + MI_LAYER);
    RefAdd(layer);
    if (old) RefRelease(old);
    if (!Wr(motion + MI_LAYER, layer)) return false;
    RefRelease(layer);
    return true;
}

static bool SetCameraLock(unsigned player, int mode) {
    unsigned fn = VFn(player, PLV_SETCAMLOCK);
    if (!fn) return false;
    __try { ((vfInt1_t)fn)((void*)player, mode); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static int MotionRequest(unsigned motion, unsigned slot, int arg) {
    unsigned fn = VFn(motion, slot);
    if (!fn) return -1;
    __try { return ((vfBool1_t)fn)((void*)motion, arg) ? 1 : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// What an NPC of the type wears, put on the player's current body (the
// character's instance) the way a spawn puts it on: m_attachments with flag 0
// (NPC::Init, 0x426429), then m_defaultAttachments with flag 1 (a spawn whose
// tag names none; flag 1 skips a bone that already carries one). 0x432200
// binds each mesh to its bone on the body instance itself (GHI +0x84, stride
// 0x70), so it goes when the instance goes - the revert's SetGeometry back to
// Stranger's body takes it off. Retail keeps every worn mesh (the outlaw
// mortar's mortar, hats, quivers) in m_defaultAttachments; m_attachments is
// empty on all 77 characters (AT3 char_fields.py, "THE ATTACHMENT LIST").
static bool ApplyAttachments(unsigned player, unsigned prefs) {
    __try {
        ((applyAttach_t)(Base() + RVA_APPLY_ATTACH))((void*)player, (void*)(prefs + NP_ATTACH), 0);
        ((applyAttach_t)(Base() + RVA_APPLY_ATTACH))((void*)player, (void*)(prefs + NP_DEFATTACH), 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// "attachments: hd=1A2B3C4D, weapnode_ranged_wpn1=..." for the log.
static void DescribeAttachments(unsigned prefs, char* out, int outLen) {
    int len = 0; out[0] = 0;
    const unsigned lists[2] = { prefs + NP_ATTACH, prefs + NP_DEFATTACH };
    for (int l = 0; l < 2; l++) {
        unsigned data = R32(lists[l]), n = R32(lists[l] + 8);
        if (!IsHeap(data) || n > 32) continue;
        for (unsigned i = 0; i < n && len < outLen - 48; i++) {
            unsigned e = data + i * ATT_STRIDE;
            unsigned tok = R32(e + ATT_GEOTOKEN);
            const char* bone = nullptr;
            unsigned sb = R32(e);
            if (IsHeap(sb)) bone = (const char*)(uintptr_t)R32(sb);
            char b[40] = "?";
            __try { if (bone && IsHeap((unsigned)(uintptr_t)bone)) lstrcpynA(b, bone, sizeof(b)); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
            len += _snprintf_s(out + len, outLen - len, _TRUNCATE, "%s%s=%08X", len ? ", " : "", b, tok);
        }
    }
    if (!out[0]) lstrcpynA(out, "none", outLen);
}

// ---------------------------------------------------------------------------
//  the animation state table
// ---------------------------------------------------------------------------

static const char* StateName(int i) {
    if (i < 0 || i >= NSTATES) return "?";
    unsigned s = R32(Base() + RVA_ANIM_NAMES + i * 4);
    return s ? (const char*)(uintptr_t)s : "?";
}

// State indices, verified against the exe's own name table at first use.
enum {
    A_STAND = 0, A_STANDTURN_L = 1, A_STANDTURN_R = 2,
    A_WALK = 3, A_WALKTURN_L = 4, A_WALKTURN_R = 5,
    A_TROT = 6, A_TROTTURN_L = 7, A_TROTTURN_R = 8,
    A_CANTER = 9, A_CANTERTURN_L = 10, A_CANTERTURN_R = 11,
    A_RUN = 12, A_RUNTURN_L = 13, A_RUNTURN_R = 14,
    A_WALK_BACK = 15, A_TROT_BACK = 16,
    A_WALK_STRAFE_L = 17, A_WALK_STRAFE_R = 18, A_TROT_STRAFE_L = 19, A_TROT_STRAFE_R = 20,
    A_TROT_TURNAROUND = 27, A_CANTER_TURNAROUND = 28, A_RUN_TURNAROUND = 29,
    A_CANTER_SKIDTOSTOP = 30, A_RUN_SKIDTOSTOP = 31, A_CANTER_TO_RUN = 32,
    A_JUMP_STAND = 33, A_JUMP_WALK = 34, A_JUMP_TROT = 35, A_JUMP_CANTER = 36, A_JUMP_RUN = 37,
    A_DJUMP_STAND = 38, A_DJUMP_WALK = 39, A_DJUMP_TROT = 40, A_DJUMP_CANTER = 41, A_DJUMP_RUN = 42,
    A_FALL_NEAR = 44, A_FALL_FAR = 45,
    A_KNOCK_AIR_QUAD = 51, A_KNOCK_GROUND_QUAD = 52, A_GET_UP_QUAD = 53,
    A_STUMBLE = 63,
    A_BUCK_ATTACK = 64, A_BUCK_EXHAUSTED = 65,
    A_BURST_TO_RUN = 66, A_BURST_EXHAUSTED = 67, A_BURST_AIR = 68,
    A_PRONE_QUAD = 69,
    A_LAND_STAND = 70, A_LAND_WALK = 71, A_LAND_TROT = 72, A_LAND_CANTER = 73, A_LAND_RUN = 74,
    A_IDLE1 = 78, A_IDLE2 = 79, A_SLIDE = 85,
    A_TEETER_START = 86, A_TEETER_IDLE = 87,
    A_DODGE_QUAD = 111,
    T_WEAPON_READY = 134, T_WEAPON_FIRE = 135,
    T_READY_UP = 146, T_READY_DOWN = 147, T_FIRE_UP = 148, T_FIRE_DOWN = 149,
};

static bool g_stateTableOk = false;
static bool CheckStateTable() {
    if (g_stateTableOk) return true;
    struct { int i; const char* n; } k[] = {
        { A_STAND, "ANIM_STAND" }, { A_RUN, "ANIM_RUN" }, { A_JUMP_STAND, "ANIM_JUMP_STAND" },
        { A_DJUMP_RUN, "ANIM_DOUBLEJUMP_RUN" }, { A_BURST_TO_RUN, "ANIM_BURST_TO_RUN" },
        { A_BUCK_ATTACK, "ANIM_BUCK_ATTACK" }, { A_DODGE_QUAD, "ANIM_DODGE_QUAD" },
        { A_LAND_RUN, "ANIM_LAND_RUN" }, { T_FIRE_DOWN, "TORSOANIM_WEAPON_FIRE_DOWN" },
        { 61, "ANIM_DISGUISECHANGE_ENTER" },
    };
    for (int j = 0; j < (int)(sizeof(k) / sizeof(k[0])); j++) {
        const char* s = StateName(k[j].i);
        __try { if (lstrcmpA(s, k[j].n) != 0) return false; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    g_stateTableOk = true;
    return true;
}

// ---------------------------------------------------------------------------
//  names
// ---------------------------------------------------------------------------
//
// A character type is its NPCPrefs path hash. Two name sources:
//   - the owner's own table (research/CHARACTER_HASHES.tsv) - the names he
//     uses, keyed by hash;
//   - the 2004 beta's loose prefs filenames, hashed in process with the
//     game's own hasher: "/data/prefs/characters/<name>prefs.txt".
// Anything left unnamed is still reachable by its hash or its animation
// config's file name ("outlawShooter"), which every character carries.

struct OwnerName { unsigned hash; const char* name; };
static const OwnerName kOwnerNames[] = {
    { 0x65DB9849, "outlawcutteralthat" }, { 0x072A64D6, "outlawcutter" },
    { 0x2BEC04A1, "outlawshooteralt" },   { 0xDDD3C8B8, "outlawcutterfan" },
    { 0x5CEE67FD, "townsfolk_clakkerz" }, { 0xE2B247BD, "blisterz" },
    { 0xFFFC00CB, "outlawshooter" },      { 0xBAF35C16, "clerk" },
    { 0x44E4794D, "jailedblisterz" },     { 0x9419779C, "jailedlootenduke" },
    { 0xC387D977, "vykkerdoc" },          { 0x0E997260, "femaleclakker" },
    { 0x155A0299, "boilzbooty" },         { 0x6CA190AC, "overallsclakker" },
    { 0xFF61B694, "filthyhandsfloydd" },  { 0x6265E0D3, "outlawmortar" },
    { 0x4312CE47, "lootenduke" },         { 0x6A6A4558, "outlawsniper" },
    { 0xFF0100ED, "outlawmortarbridge" }, { 0x606283C9, "farmerbeaks" },
    { 0x4D82B2A2, "tiny" },               { 0xB28A48AA, "meaglymcgraw" },
    { 0x2F4A8582, "femaleclakker2" },     { 0xAB444625, "sewerworker" },
    { 0xC766CCA2, "eugeneius" },          { 0x7C6717E1, "slog" },
    { 0xB6FE5A75, "grubb_male" },         { 0x2F1D4FF5, "sewerslog" },
    { 0x5629AD35, "outlawsuicidebomber" },{ 0x2D8CF05F, "outlawsemiauto" },
    { 0xB5A32E92, "outlawflamethrower" }, { 0x2B6A3743, "patrackpalooka" },
    { 0xFF32E523, "outlaw_nailer" },      { 0x35179A52, "jomama" },
    { 0x3670CD9C, "clakker_sleghunter" }, { 0xECF8307A, "skycart_joe" },
    { 0x3E4F6569, "bargekeeper" },        { 0xE974A99C, "outlawnailer_infrared" },
    { 0x4E169515, "lefty_lugnutz" },      { 0x57009F0C, "fattymcboomboom" },
    { 0xE0A31269, "anomaly_outlawshooter" },{ 0xF4DC66D8, "minecartxplosives" },
    { 0x0BB34EB7, "xplosivesmcgee" },     { 0xB53065FF, "outlawboss_elbowz" },
    { 0x018AD12D, "giantsleg" },          { 0xC9F81B7E, "dcasteraider" },
    { 0xFDBD2F9C, "wolvarkshooter" },     { 0xF0315D54, "wolvarkgrenadier" },
    { 0x3EE6BB58, "grubb_female" },       { 0xE7EBC893, "grubb_leader" },
    { 0xF98DFA43, "grubb_armored" },      { 0x84AB1672, "wolvarkmachinegunner" },
    { 0x20F622E5, "wolvarksloghandler" }, { 0x214F50C8, "grubb_injured" },
    { 0x36ED7834, "grubb_winter" },       { 0x31D1280E, "wolvarksniper" },
    { 0x450E9598, "gloktigi" },           { 0x151F4B4E, "shocktank" },
    { 0xB03890D0, "wolvarksniper6" },     { 0xB42B865B, "grubbsoldier" },
    { 0xE33BE973, "wolvarkgunner" },      { 0x879B644E, "anomaly_region6" },
    { 0x300596C6, "armoredgloktigi" },    { 0xCAB666AF, "sektosdeathray" },
    { 0x97C44232, "sektoboss" },
};
static const int kOwnerNameCount = sizeof(kOwnerNames) / sizeof(kOwnerNames[0]);

static const char* kBetaNames[] = {
    "ArtilleryCannon_Far", "Castaraider", "ElbowsFreely", "FarmerFoster",
    "FarmersDaughters", "GiantSleg", "Gloktigi", "Gloktigi_minion", "GutLips",
    "NativeCannon", "NativeCatapulter", "NativeCatapulter_Close",
    "NativeCatapulter_Far", "NativeFemale", "NativeLeader", "Native",
    "NativeRebelHurt", "NativeRebel", "NativeSnow", "NoBS_Townsfolk",
    "OuthouseBandit", "OutlawBomber", "OutlawBossPyro", "OutlawBoss_BadMortar",
    "OutlawBoss_ElbowsFreely", "OutlawBoss_FattyMcBoomBoom",
    "OutlawBoss_FilthyHandsFloyd", "OutlawBoss_JoMomma",
    "OutlawBoss_LeftyLugnutz", "OutlawBoss_LootenDuke",
    "OutlawBoss_MeaglyMcGraw", "OutlawBoss_MovieBoss",
    "OutlawBoss_SplosivesMcgree", "OutlawCart", "OutlawCutter", "OutlawGutlips",
    "OutlawHunter", "OutlawIgnited", "OutlawMortar", "OutlawNailer",
    "OutlawShooter", "OutlawSniper", "OutlawTurret", "ScubaToadNative",
    "ScubaToad", "Sekto1", "Sekto2", "Sekto2a", "Sekto3", "Sekto3a",
    "SewerSleg", "Sleg", "SlogHandlerSpawned", "Slog", "TownsfolkDusty",
    "TownsfolkFemale", "TownsfolkKid", "Townsfolk", "TownsfolkSewerworker",
    "TownsfolkStorekeeper", "Townsfolk_SkycartJoe", "Ugenius", "VykkerDoc",
    "WolvarkCannon", "WolvarkGrenadier", "WolvarkLeader", "WolvarkShooter",
    "WolvarkSloghandler", "WolvarkSniper", "WolvarkSuperSniper", "shocktank",
};
static const int kBetaNameCount = sizeof(kBetaNames) / sizeof(kBetaNames[0]);
static unsigned g_betaHash[sizeof(kBetaNames) / sizeof(kBetaNames[0])];
static bool     g_betaHashed = false;

static void HashBetaNames() {
    if (g_betaHashed) return;
    char path[256];
    for (int i = 0; i < kBetaNameCount; i++) {
        _snprintf_s(path, sizeof(path), _TRUNCATE, "/data/prefs/characters/%sprefs.txt", kBetaNames[i]);
        CharLowerA(path);
        g_betaHash[i] = SWSE_HashPath(path);
    }
    g_betaHashed = true;
}

// The stem of an animation config path: "\data\prefs\MotionAnimConfig\
// outlawShooter.txt" -> "outlawShooter".
static void CfgStem(const char* path, char* out, int outLen) {
    out[0] = 0;
    if (!path) return;
    __try {
        const char* s = path;
        for (const char* p = path; *p; p++) if (*p == '\\' || *p == '/') s = p + 1;
        int i = 0;
        for (; s[i] && s[i] != '.' && i < outLen - 1; i++) out[i] = s[i];
        out[i] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; }
}

// ---------------------------------------------------------------------------
//  what is resident: every NPCPrefs in the game's resource registry
// ---------------------------------------------------------------------------

struct NpcType {
    unsigned prefs, hash, def, cfg;
    const char* cfgPath;
    char  name[48];     // best display name
    char  stem[48];     // animation config stem
    float scale;        // what we would draw it at (clamped)
    float authored;     // m_geoScaleMax as authored
    int   ready;        // 1 = geometry device data present (streamed in)
};

#define MAX_TYPES 96
static NpcType g_types[MAX_TYPES];
static int     g_typeCount = 0;

static int DefFlags(unsigned def) { return (int)(R32(def + RES_BLOCK) & 3); }

static void NameType(NpcType* t) {
    HashBetaNames();
    t->name[0] = 0;
    for (int i = 0; i < kOwnerNameCount; i++)
        if (kOwnerNames[i].hash == t->hash) { lstrcpynA(t->name, kOwnerNames[i].name, sizeof(t->name)); return; }
    for (int i = 0; i < kBetaNameCount; i++)
        if (g_betaHash[i] == t->hash) { lstrcpynA(t->name, kBetaNames[i], sizeof(t->name)); return; }
}

static float TypeScale(unsigned prefs, float* authored) {
    float lo = RF(prefs + NP_SCALEMIN), hi = RF(prefs + NP_SCALEMAX);
    if (!(hi > 0.05f && hi < 100.0f)) hi = 1.0f;
    if (!(lo > 0.05f && lo <= hi)) lo = hi;
    if (authored) *authored = hi;
    float s = 0.5f * (lo + hi);             // a typical member of the type
    if (s > SCALE_MAX_SAFE) s = SCALE_MAX_SAFE;
    if (s < SCALE_MIN_SAFE) s = SCALE_MIN_SAFE;
    return s;
}

static bool FillType(unsigned prefs, NpcType* t) {
    memset(t, 0, sizeof(*t));
    t->prefs = prefs;
    t->hash = R32(prefs + RES_HASH);
    if (!t->hash || t->hash == HASH_NULL) return false;
    unsigned tok = R32(prefs + NP_GEOMETRY);
    unsigned def = SWSE_ResourceLookup(tok);
    if (VtIs(def, RVA_VT_GHD)) t->def = def;
    t->cfgPath = StrField(prefs + NP_ANIMCFG);
    if (t->cfgPath) {
        unsigned cfg = SWSE_ResourceLookup(SWSE_HashPath(t->cfgPath));
        if (VtIs(cfg, RVA_VT_ANIMCFG)) t->cfg = cfg;
    }
    CfgStem(t->cfgPath, t->stem, sizeof(t->stem));
    t->scale = TypeScale(prefs, &t->authored);
    t->ready = t->def && DefFlags(t->def) == 1;
    NameType(t);
    if (!t->name[0]) {
        if (t->stem[0]) _snprintf_s(t->name, sizeof(t->name), _TRUNCATE, "%s_%08X", t->stem, t->hash);
        else _snprintf_s(t->name, sizeof(t->name), _TRUNCATE, "%08X", t->hash);
    }
    return true;
}

// Walk the registry: the sorted Resource* vector at ResourceManager+0x30
// (measured live by prefsedit). Read-only; no call into the game.
static int ScanTypes() {
    g_typeCount = 0;
    unsigned vtNpc = Base() + RVA_VT_NPCPREFS;
    __try {
        unsigned mgr = *(unsigned*)(Base() + RVA_RESMGR);
        if (!IsHeap(mgr)) return 0;
        unsigned vec = *(unsigned*)(mgr + 0x30);
        if (!IsHeap(vec)) return 0;
        unsigned data = *(unsigned*)vec, cap = *(unsigned*)(vec + 4), cnt = *(unsigned*)(vec + 8);
        if (!IsHeap(data) || !cnt || cnt > cap || cnt > 1000000) return 0;
        for (unsigned i = 0; i < cnt && g_typeCount < MAX_TYPES; i++) {
            unsigned r = *(unsigned*)(data + i * 4);
            if (!IsHeap(r) || *(unsigned*)r != vtNpc) continue;
            if (FillType(r, &g_types[g_typeCount])) g_typeCount++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return g_typeCount;
}

// Resolve what the user typed. Exact name, config stem or hash first, then an
// unambiguous prefix. Returns the index into g_types, -1 none, -2 ambiguous.
static int FindType(const char* want) {
    if (!want || !want[0]) return -1;
    ScanTypes();
    size_t n = strlen(want);
    if (n == 8 && strspn(want, "0123456789abcdefABCDEF") == 8) {
        unsigned h = strtoul(want, nullptr, 16);
        for (int i = 0; i < g_typeCount; i++) if (g_types[i].hash == h) return i;
        return -1;
    }
    HashBetaNames();
    for (int i = 0; i < g_typeCount; i++) {
        if (!lstrcmpiA(g_types[i].name, want)) return i;
    }
    // any known name of the type (owner's and beta's both), and the stem
    for (int i = 0; i < g_typeCount; i++) {
        for (int k = 0; k < kOwnerNameCount; k++)
            if (kOwnerNames[k].hash == g_types[i].hash && !lstrcmpiA(kOwnerNames[k].name, want)) return i;
        for (int k = 0; k < kBetaNameCount; k++)
            if (g_betaHash[k] == g_types[i].hash && !lstrcmpiA(kBetaNames[k], want)) return i;
    }
    int stemHit = -1, stemHits = 0;
    for (int i = 0; i < g_typeCount; i++)
        if (g_types[i].stem[0] && !lstrcmpiA(g_types[i].stem, want)) { stemHit = i; stemHits++; }
    if (stemHits == 1) return stemHit;
    int hit = -1, hits = 0;
    for (int i = 0; i < g_typeCount; i++)
        if (_strnicmp(g_types[i].name, want, n) == 0) { hit = i; hits++; }
    if (hits == 1) return hit;
    if (hits > 1 || stemHits > 1) return -2;
    return -1;
}

// ---------------------------------------------------------------------------
//  residency: what a character needs loaded, and keeping it loaded
// ---------------------------------------------------------------------------
//
// A level's data is split into SharedMemoryBlocks (.smb bundles) that the
// streamer loads and unloads. MEASURED in lm_level_02 (45 npc blocks): a
// character's BODY - GeometryHierarchyDef, GeometryDefs, textures, skeleton -
// is one "npc_N.smb"; its ANIMATIONS are in one to three shared npc blocks
// (every townsfolk variant animates from npc_41); its NPCPrefs and
// MotionAnimConfig are in the level's "_tgl.smb", which never unloads.
//
// The scheduler keeps an npc block loaded while something needs it, and the
// signal is a per-frame count. Every live NPC bumps its blocks' lazy-need
// count as it ticks (beta NPC::IncBlockRefCounts; HD 0x42E220 over NPC+0x22C).
// At the top of every frame the scheduler (beta Scheduler::TickNPCBlocks; HD
// 0x5F2970(1), called from 0x402677) requests residency for every npc block
// with a non-zero count, release for the rest, and zeroes the counts. That is
// why the body "streamed out" when the player walked away: that type's NPCs
// went to sleep and stopped bumping the count.
//
// So playnpc does what a live NPC of the type does: once per frame it bumps
// the count of every block the character needs - the set the engine's own
// NPC::FindDependentBlocks + Motion::AddAnimationBlocks gather: the body's
// block and each animation's block. There is nothing to undo: the engine
// zeroes the counts every frame, so a hold ends the first frame playnpc stops
// bumping (revert, off, another character, level change). And a block is only
// ever written while it is in the scheduler's live list that frame; a level
// unload empties the list (0x5F1EC0) before the blocks go.
//
// Load on demand is the same bump on a block that is not in yet: the next
// frame's scheduler tick requests it (front of the queue), the block manager
// streams it in, and playnpc swaps once every needed resource is resident.
//
// NOT used: Resource::SetBlockResidencyOverride. It is not a pin - the block
// sets it on its own resources while moving or releasing them (bit1 of
// Resource+8), to hide them mid-transition.

struct Deps {
    int      n;
    unsigned block[MAX_DEPS];      // npc blocks the character needs
    unsigned rep[MAX_DEPS];        // a resource we use from each; its resident bit is the test
    char     file[MAX_DEPS][24];   // "npc_8.smb"
    DWORD    inAt[MAX_DEPS];       // during a load: ms after the request it arrived (0 = not yet)
    int      anims, unresolved, overflow;
    unsigned farBlk;               // a needed block that is not an npc block and not resident
    char     farFile[24];
    // What the bodies' materials draw from (see "textures" below).
    bool     refsPending;          // a body is not in yet: its references are read once it is
    int      refs, refBlocks;      // resources referenced; npc bundles added for them
    int      refUnreg, refOut;     // not registered in this level; in a level bundle that is not in
    char     refOutFile[24];
};

static bool NpcBlockList(unsigned* data, unsigned* cnt) {
    unsigned v = Base() + RVA_NPCBLOCKS;
    unsigned d = R32(v), cap = R32(v + 4), n = R32(v + 8);
    if (!IsHeap(d) || n == 0 || n > cap || cap > 4096) return false;
    *data = d; *cnt = n;
    return true;
}

static int NpcBlockIndex(unsigned blk) {
    unsigned d = 0, n = 0;
    if (!blk || !NpcBlockList(&d, &n)) return -1;
    for (unsigned k = 0; k < n; k++) if (R32(d + k * 4) == blk) return (int)k;
    return -1;
}

static void BlockFile(unsigned blk, char* out, int outLen) {
    const char* s = StrField(blk + BLK_FILENAME);
    if (s) {
        __try {
            const char* f = s;
            for (const char* p = s; *p; p++) if (*p == '\\' || *p == '/') f = p + 1;
            lstrcpynA(out, f, outLen);
            return;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    _snprintf_s(out, outLen, _TRUNCATE, "block %08X", blk);
}

static unsigned BlockOf(unsigned res) { return R32(res + RES_BLOCK) & ~3u; }
static bool ResResident(unsigned res) { return (R32(res + RES_BLOCK) & 3) == 1; }
static bool DepIn(const Deps* d, int i) {
    return R32(d->block[i] + BLK_RESIDENCY) == 1 && ResResident(d->rep[i]);
}

// soft: a resource a material draws from. One in a level bundle that is not in
// is counted, not "far" - the character is not refused for it; that part
// draws with the engine's white fallback until its area loads.
static void AddDep(Deps* d, unsigned res, bool soft = false) {
    unsigned b = BlockOf(res);
    if (!b) return;                                   // not block-backed: nothing to hold
    for (int i = 0; i < d->n; i++) if (d->block[i] == b) return;
    if (!VtIs(b, RVA_VT_BLOCK)) return;
    if (NpcBlockIndex(b) < 0) {
        // A level block (the _tgl bundle, a zone): not the npc scheduler's to
        // load. Fine while resident; otherwise it only comes with its area.
        if (R32(b + BLK_RESIDENCY) != 1 || !ResResident(res)) {
            if (soft) {
                if (!d->refOut++) BlockFile(b, d->refOutFile, sizeof(d->refOutFile));
            } else if (!d->farBlk) {
                d->farBlk = b;
                BlockFile(b, d->farFile, sizeof(d->farFile));
            }
        }
        return;
    }
    if (d->n >= MAX_DEPS) { d->overflow++; return; }
    d->block[d->n] = b;
    d->rep[d->n] = res;
    BlockFile(b, d->file[d->n], sizeof(d->file[0]));
    d->n++;
    if (soft) d->refBlocks++;
}

// ---------------------------------------------------------------------------
//  textures: what a body's materials draw from
// ---------------------------------------------------------------------------
//
// OWNER REPORT (1.1): boilzbooty's shotgun, and filthyhandsfloydd's, drew pure
// white - the mesh there, no texture. Both guns are part of the body mesh.
//
// The engine's own NPC holds more than its body's bundle. NPC::FindDependentBlocks
// (beta 0x467B07) asks the body for every resource it references and holds
// each one's bundle: GeometryHierarchyDef::GetReferencedResources (HD vtable
// +0x3C, 0x630AE0) gives its own token, the skeleton's (+0x5C) and, through
// GeometryDef (0x6386B0), each material's (materials at +0x40, count +0x44):
// the glare/distortion/envmap tokens every MaterialDef has (+0x1C/+0x20/+0x24,
// 0x649CA0) and a textured material's own (+0x34). A texture can sit in a
// different npc bundle than the body - a gun several outlaws share. playnpc
// held only the body's bundle and the animations', so that bundle could stay
// out, and a material whose texture is missing draws with the engine's
// fallback: MaterialDef::PreCacheTexture (0x649EE0) caches
// Texture::GetWhiteTexture (0x6116D0, the Texture* kept at 0xA54004) when the
// lookup finds nothing, and never looks again while its cache is set
// (SubclassPreCacheResources 0x67F280 tests it first).
//
// So: the deps take the bodies' referenced resources as well (the attachments'
// too), a load waits for their bundles, and the hold keeps them in. A
// resource in a level bundle that is not in is logged, not waited for. And a
// material that already cached the white fallback while its texture was out
// is bound to the real texture once that is in (the same SPtr assignment the
// engine's own precache does, 0x50D480).
#define GHDV_GETREFS          0x3C        // GeometryHierarchyDef::GetReferencedResources(vecsorted<Token>&)
#define RVA_GHD_GETREFS       0x230AE0
#define RVA_VT_GEODEF         0x38A25C    // GeometryDef: a plain mesh - what the hats and weapons are
#define RVA_GD_GETREFS        0x2386B0    // GeometryDef::GetReferencedResources, the same slot
#define GD_MATS               0x40        // GeometryDef materials: MaterialDef** +0x40, count +0x44
#define GD_MATSN              0x44
#define RVA_WHITE_TEX         0x654004    // Texture::GetWhiteTexture's Texture*
#define MAX_REFS              512
#define MAX_MATS              64          // ~5 tokens a material: the token buffer never has to grow

struct MatKind { unsigned vt; const char* name; int n; unsigned tok[2], cache[2]; };
// HD vtables (RTTI) and each class's SubclassPreCacheResources offsets.
static const MatKind kMatKinds[] = {
    { 0x39A91C, "DynamicVertexLit", 1, { 0x34, 0 },    { 0x38, 0 } },     // 0x67F280
    { 0x39A724, "SimpleTextured",   1, { 0x34, 0 },    { 0x38, 0 } },     // 0x67F280
    { 0x39A424, "MatteTextured",    1, { 0x34, 0 },    { 0x38, 0 } },     // 0x67F280
    { 0x39A7C4, "Transparent",      1, { 0x34, 0 },    { 0x48, 0 } },     // 0x67F850
    { 0x39A574, "NormalMapped",     2, { 0x34, 0x38 }, { 0x3C, 0x40 } },  // 0x67D840
};

static const MatKind* MatKindOf(unsigned mat) {
    unsigned vt = R32(mat);
    for (int i = 0; i < (int)(sizeof(kMatKinds) / sizeof(kMatKinds[0])); i++)
        if (vt == Base() + kMatKinds[i].vt) return &kMatKinds[i];
    return nullptr;
}

static bool IsGeoDef(unsigned def) { return VtIs(def, RVA_VT_GHD) || VtIs(def, RVA_VT_GEODEF); }

static bool DefIn(unsigned def) {
    if (!IsGeoDef(def) || !ResResident(def)) return false;
    unsigned b = BlockOf(def);
    return !b || R32(b + BLK_RESIDENCY) == 1;
}

typedef void (__thiscall* getRefs_t)(void* self, void* vec);
static unsigned g_refBuf[MAX_REFS];

// The engine's own list of what a body references, into a buffer of ours big
// enough that the sorted insert never grows it (growing would free our buffer
// on the engine's heap); a call that grew it or faulted is discarded. Only
// for a body that is in: an unloaded one's material list is not there.
static int RefTokens(unsigned def, unsigned* out, int max) {
    if (!SWSE_GameBuildKnown() || !DefIn(def)) return -1;
    unsigned fn = VFn(def, GHDV_GETREFS);
    if (fn != Base() + (VtIs(def, RVA_VT_GHD) ? RVA_GHD_GETREFS : RVA_GD_GETREFS)) return -1;
    if (R32(def + GD_MATSN) > MAX_MATS) return -1;
    struct { unsigned data, cap, count; } v = { (unsigned)(uintptr_t)g_refBuf, MAX_REFS, 0 };
    __try { ((getRefs_t)fn)((void*)def, &v); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    if (v.data != (unsigned)(uintptr_t)g_refBuf || v.count > MAX_REFS) return -1;
    int n = (int)v.count < max ? (int)v.count : max;
    for (int i = 0; i < n; i++) out[i] = g_refBuf[i];
    return n;
}

static void AddRefDeps(Deps* d, unsigned def) {
    static unsigned toks[MAX_REFS];
    int n = RefTokens(def, toks, MAX_REFS);
    if (n < 0) { d->refsPending = true; return; }
    for (int i = 0; i < n; i++) {
        unsigned h = toks[i];
        if (!h || h == HASH_NULL) continue;
        d->refs++;
        unsigned r = SWSE_ResourceLookup(h);
        if (!r) { d->refUnreg++; continue; }
        if (r != def) AddDep(d, r, true);
    }
}

// A material that cached the white fallback while its texture was out is
// bound to the texture now that it is in - what the engine's precache would
// have cached. Only a texture that is registered, resident and in a resident
// bundle; the white texture keeps its own static reference.
static int RebindWhite(unsigned def, const char* who) {
    if (!SWSE_GameBuildKnown() || !DefIn(def)) return 0;
    unsigned white = R32(Base() + RVA_WHITE_TEX);
    if (!IsHeap(white)) return 0;
    unsigned mats = R32(def + GD_MATS), nm = R32(def + GD_MATSN);
    if (!IsHeap(mats) || nm > MAX_MATS) return 0;
    int fixed = 0;
    for (unsigned i = 0; i < nm; i++) {
        unsigned m = R32(mats + i * 4);
        const MatKind* k = IsHeap(m) ? MatKindOf(m) : nullptr;
        if (!k) continue;
        for (int j = 0; j < k->n; j++) {
            unsigned slot = m + k->cache[j], h = R32(m + k->tok[j]);
            if (R32(slot) != white || !h || h == HASH_NULL) continue;
            unsigned tex = SWSE_ResourceLookup(h);
            char nm2[48] = "";
            if (!tex || tex == white || !SWSE_RttiName(tex, nm2, sizeof(nm2)) || lstrcmpA(nm2, "Texture")) continue;
            unsigned b = BlockOf(tex);
            if (!ResResident(tex) || (b && R32(b + BLK_RESIDENCY) != 1)) continue;
            bool done = false;
            __try {
                volatile WORD* wr = (volatile WORD*)(uintptr_t)(white + RES_REF);
                volatile WORD* tr = (volatile WORD*)(uintptr_t)(tex + RES_REF);
                if (*wr >= 2 && *tr != 0xFFFF) {         // never the white texture's last reference
                    ++*tr;
                    --*wr;
                    *(volatile unsigned*)(uintptr_t)slot = tex;
                    done = true;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            if (done) {
                fixed++;
                Log("texture: %s material %u (%s) had the white fallback cached - bound %08X", who, i, k->name, h);
            }
        }
    }
    return fixed;
}

// `playnpc textures [name]`: each material of the body (and its attachments):
// its class, texture token, where that texture lives and whether it is in,
// and what the material has cached (the white fallback marked).
static void DescribeTextures(unsigned def, const char* label) {
    unsigned white = R32(Base() + RVA_WHITE_TEX);
    unsigned mats = R32(def + GD_MATS), nm = R32(def + GD_MATSN);
    if (!DefIn(def)) { Printf("  %s: not in memory - its materials cannot be read", label); return; }
    if (!IsHeap(mats) || nm > MAX_MATS) { Printf("  %s: material list unreadable (%u)", label, nm); return; }
    Printf("  %s: %u material(s)", label, nm);
    for (unsigned i = 0; i < nm; i++) {
        unsigned m = R32(mats + i * 4);
        const MatKind* k = IsHeap(m) ? MatKindOf(m) : nullptr;
        if (!k) {
            char rn[48] = "?";
            if (IsHeap(m)) SWSE_RttiName(m, rn, sizeof(rn));
            Printf("    %2u %s (not read)", i, rn);
            continue;
        }
        for (int j = 0; j < k->n; j++) {
            unsigned h = R32(m + k->tok[j]), c = R32(m + k->cache[j]);
            unsigned r = (h && h != HASH_NULL) ? SWSE_ResourceLookup(h) : 0;
            char where[64] = "not registered here";
            if (r) {
                unsigned b = BlockOf(r);
                char f[24] = "no bundle";
                if (b) BlockFile(b, f, sizeof(f));
                _snprintf_s(where, sizeof(where), _TRUNCATE, "%s %s%s", f,
                            b ? (R32(b + BLK_RESIDENCY) == 1 ? "in" : "OUT") : "",
                            ResResident(r) ? "" : ", texture not resident");
            }
            const char* cached = !c ? "nothing cached yet" : c == white ? "WHITE FALLBACK" : c == r ? "the texture" : "another texture";
            Log("texture: %s mat %u %s%s token %08X -> %s; cached: %s", label, i, k->name, j ? " (normal map)" : "", h, where, cached);
            Printf("    %2u %-16s %08X  %-34s %s", i, j ? "  normal map" : k->name, h, where, cached);
        }
    }
}

enum { DEP_OK = 0, DEP_NOBODY = 1, DEP_FAR = 2 };

// The blocks a character needs: its body's, each animation's in its config,
// each default attachment's, and those of what the bodies' materials draw
// from (read once a body is in; until then refsPending).
static int ComputeDeps(const NpcType& t, Deps* d) {
    memset(d, 0, sizeof(*d));
    if (!t.def) return DEP_NOBODY;
    AddDep(d, t.def);
    AddRefDeps(d, t.def);
    if (t.cfg) {
        unsigned arr = R32(t.cfg + AC_ANIMS), n = R32(t.cfg + AC_ANIMSN);
        if (IsHeap(arr) && n <= 1024) {
            for (unsigned i = 0; i < n; i++) {
                unsigned e = arr + i * AAC_STRIDE;
                unsigned src = R32(e + AAC_SRCS), sn = R32(e + AAC_SRCSN);
                if (!IsHeap(src) || sn > 64) continue;
                for (unsigned j = 0; j < sn; j++) {
                    unsigned h = R32(src + j * SRC_STRIDE + SRC_ANIMHASH);
                    if (!h || h == HASH_NULL) continue;
                    unsigned a = SWSE_ResourceLookup(h);
                    if (!VtIs(a, RVA_VT_ANIM)) { d->unresolved++; continue; }
                    d->anims++;
                    AddDep(d, a);
                }
            }
        }
    }
    const unsigned lists[2] = { t.prefs + NP_ATTACH, t.prefs + NP_DEFATTACH };
    for (int l = 0; l < 2; l++) {
        unsigned adata = R32(lists[l]), an = R32(lists[l] + 8);
        if (!IsHeap(adata) || an > 32) continue;
        for (unsigned i = 0; i < an; i++) {
            unsigned tok = R32(adata + i * ATT_STRIDE + ATT_GEOTOKEN);
            if (!tok || tok == HASH_NULL) continue;
            unsigned g = SWSE_ResourceLookup(tok);
            // A hat or a weapon is a plain GeometryDef: held too (the engine's
            // own NPC holds its attachments' bundles), softly - one in a level
            // bundle that is not in never refuses the character.
            if (VtIs(g, RVA_VT_GHD)) { AddDep(d, g); AddRefDeps(d, g); }
            else if (VtIs(g, RVA_VT_GEODEF)) { AddDep(d, g, true); AddRefDeps(d, g); }
        }
    }
    return d->farBlk ? DEP_FAR : DEP_OK;
}

// Every held block resident, and the resource we use from it flagged resident
// (bit0 set, no transition bit).
static bool DepsResident(const Deps* d, int* firstMissing) {
    for (int i = 0; i < d->n; i++) {
        if (!DepIn(d, i)) { if (firstMissing) *firstMissing = i; return false; }
    }
    if (firstMissing) *firstMissing = -1;
    return true;
}

// THE HOLD: what a live NPC of the type does every tick. Only a block in the
// scheduler's list THIS frame is written.
static int HoldDeps(const Deps* d) {
    if (!d || d->n <= 0 || !SWSE_GameBuildKnown()) return 0;
    unsigned data = 0, cnt = 0;
    if (!NpcBlockList(&data, &cnt)) return 0;
    int held = 0;
    for (int i = 0; i < d->n; i++) {
        unsigned b = d->block[i];
        bool listed = false;
        for (unsigned k = 0; k < cnt && !listed; k++) listed = R32(data + k * 4) == b;
        if (!listed || !VtIs(b, RVA_VT_BLOCK)) continue;
        __try { ++*(volatile LONG*)(uintptr_t)(b + BLK_LAZYNEED); held++; }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return held;
}

// Where a block stands in the block manager's request queue; -1 = not queued.
static int QueuePos(unsigned blk, int* type, int* depth) {
    unsigned v = Base() + RVA_BLOCKQUEUE;
    unsigned d = R32(v), n = R32(v + 8);
    if (depth) *depth = (int)n;
    if (type) *type = -1;
    if (!IsHeap(d) || n > 4096) return -1;
    for (unsigned k = 0; k < n; k++) {
        if (R32(d + k * 8) == blk) { if (type) *type = (int)R32(d + k * 8 + 4); return (int)k; }
    }
    return -1;
}

static void DepsFiles(const Deps* d, char* out, int outLen, bool missingOnly) {
    out[0] = 0;
    for (int i = 0; i < d->n; i++) {
        if (missingOnly && DepIn(d, i)) continue;
        if (out[0]) strncat_s(out, outLen, ", ", _TRUNCATE);
        strncat_s(out, outLen, d->file[i], _TRUNCATE);
    }
}

// ---------------------------------------------------------------------------
//  the private animation config
// ---------------------------------------------------------------------------
//
// AnimationLayerConfig (0x2C bytes) is a shared, loaded resource: every NPC of
// the type animates from the same object. Its lookup[state] -> entry table is
// what every accessor reads (0x520B00 / 0x520D60 / 0x520DE0 / 0x520550 all go
// layer+0x4C -> +0x20 / +0x28). So the player gets a COPY of the header with
// its own 150-entry lookup, and the NPC layer is pointed at the copy. Entries
// stay the NPC's own, shared, read-only AnimationLayerAnimConfig records - so
// whatever plays is by construction a clip this character authored.
//
// The copy's refcount is pinned so the engine can never destroy it (it was
// never the engine's allocation); a layer releasing it just decrements.
// BuildStateLookup (0x523DF0) returns at once for a lookup that exists, so
// nothing rebuilds the copy, and the original's lookup is never written.

struct GaitRule { short state; short donors[4]; };
// Order matters: gaits first, so later rules can draw on filled gaits.
static const GaitRule kRules[] = {
    { A_RUN,               { A_CANTER, A_TROT, A_WALK, -1 } },
    { A_CANTER,            { A_RUN, A_TROT, A_WALK, -1 } },
    { A_TROT,              { A_RUN, A_CANTER, A_WALK, -1 } },
    { A_WALK,              { A_TROT, A_RUN, A_CANTER, -1 } },
    { A_WALKTURN_L,        { A_TROTTURN_L, A_RUNTURN_L, A_WALK, -1 } },
    { A_WALKTURN_R,        { A_TROTTURN_R, A_RUNTURN_R, A_WALK, -1 } },
    { A_TROTTURN_L,        { A_RUNTURN_L, A_CANTERTURN_L, A_WALKTURN_L, A_TROT } },
    { A_TROTTURN_R,        { A_RUNTURN_R, A_CANTERTURN_R, A_WALKTURN_R, A_TROT } },
    { A_CANTERTURN_L,      { A_RUNTURN_L, A_TROTTURN_L, A_CANTER, -1 } },
    { A_CANTERTURN_R,      { A_RUNTURN_R, A_TROTTURN_R, A_CANTER, -1 } },
    { A_RUNTURN_L,         { A_CANTERTURN_L, A_TROTTURN_L, A_RUN, -1 } },
    { A_RUNTURN_R,         { A_CANTERTURN_R, A_TROTTURN_R, A_RUN, -1 } },
    { A_STANDTURN_L,       { A_WALKTURN_L, A_STAND, -1, -1 } },
    { A_STANDTURN_R,       { A_WALKTURN_R, A_STAND, -1, -1 } },
    { A_WALK_BACK,         { A_WALK, -1, -1, -1 } },
    { A_TROT_BACK,         { A_TROT, A_WALK, -1, -1 } },
    { A_WALK_STRAFE_L,     { A_WALK, -1, -1, -1 } },
    { A_WALK_STRAFE_R,     { A_WALK, -1, -1, -1 } },
    { A_TROT_STRAFE_L,     { A_TROT, A_WALK_STRAFE_L, -1, -1 } },
    { A_TROT_STRAFE_R,     { A_TROT, A_WALK_STRAFE_R, -1, -1 } },
    // Transitional one-shots. A missing state falls back to lookup[0] - the
    // idle, often several seconds long - and a one-shot motion lasts as long
    // as its clip. That is the "running is messed up" freeze. The NPC's own
    // gait loop is short and looks like what the body is doing.
    { A_TROT_TURNAROUND,   { A_TROT, A_RUN, A_WALK, -1 } },
    { A_CANTER_TURNAROUND, { A_CANTER, A_RUN, A_TROT, -1 } },
    { A_RUN_TURNAROUND,    { A_RUN, A_CANTER, A_TROT, -1 } },
    { A_CANTER_SKIDTOSTOP, { A_WALK, A_TROT, -1, -1 } },
    { A_RUN_SKIDTOSTOP,    { A_WALK, A_TROT, -1, -1 } },
    { A_CANTER_TO_RUN,     { A_RUN, A_CANTER, -1, -1 } },
    // The sprint and the charge. BurstToRun (motion 32, enter 0x5405C0) plays
    // BURST_TO_RUN - or BURST_TO_RUN_EXHAUSTED without stamina - once, then
    // hands over to RUN; BuckAttack (the other mouse button) plays BUCK_ATTACK
    // or BUCK_ATTACK_EXHAUSTED (0x533B58, 0x54029F). All are Stranger's own
    // four-legged clips: the NPC's fastest run stands in for each.
    { A_BURST_TO_RUN,      { A_RUN, A_CANTER, A_TROT, A_WALK } },
    { A_BURST_EXHAUSTED,   { A_RUN, A_CANTER, A_TROT, A_WALK } },
    { A_BURST_AIR,         { A_RUN, A_CANTER, A_TROT, A_WALK } },
    { A_BUCK_ATTACK,       { A_RUN, A_CANTER, A_TROT, A_WALK } },
    { A_BUCK_EXHAUSTED,    { A_RUN, A_CANTER, A_TROT, A_WALK } },
    { A_STUMBLE,           { A_WALK, -1, -1, -1 } },
    // Jumps: the NPC's own jump clips where it has any; none -> no clip, the
    // jump still happens (the owner's rule).
    { A_JUMP_STAND,        { A_JUMP_RUN, -1, -1, -1 } },
    { A_JUMP_RUN,          { A_JUMP_STAND, -1, -1, -1 } },
    { A_JUMP_WALK,         { A_JUMP_STAND, A_JUMP_RUN, -1, -1 } },
    { A_JUMP_TROT,         { A_JUMP_RUN, A_JUMP_STAND, -1, -1 } },
    { A_JUMP_CANTER,       { A_JUMP_RUN, A_JUMP_STAND, -1, -1 } },
    // The double jump itself is blocked, which leaves m_jumpCanDouble false
    // after every jump - and the air tick (0x53A1BC) then animates the rest of
    // the arc from the DOUBLEJUMP states. So they carry the NPC's jump clip.
    { A_DJUMP_STAND,       { A_JUMP_STAND, -1, -1, -1 } },
    { A_DJUMP_WALK,        { A_JUMP_WALK, -1, -1, -1 } },
    { A_DJUMP_TROT,        { A_JUMP_TROT, -1, -1, -1 } },
    { A_DJUMP_CANTER,      { A_JUMP_CANTER, -1, -1, -1 } },
    { A_DJUMP_RUN,         { A_JUMP_RUN, -1, -1, -1 } },
    { A_LAND_WALK,         { A_WALK, -1, -1, -1 } },
    { A_LAND_TROT,         { A_TROT, -1, -1, -1 } },
    { A_LAND_CANTER,       { A_CANTER, -1, -1, -1 } },
    { A_LAND_RUN,          { A_RUN, -1, -1, -1 } },
    { A_TEETER_START,      { A_STAND, -1, -1, -1 } },
    { A_TEETER_IDLE,       { A_STAND, -1, -1, -1 } },
    // Upper body: Stranger's aim-up/down blend onto the NPC's own weapon pose.
    { T_READY_UP,          { T_WEAPON_READY, -1, -1, -1 } },
    { T_READY_DOWN,        { T_WEAPON_READY, -1, -1, -1 } },
    { T_FIRE_UP,           { T_WEAPON_FIRE, T_WEAPON_READY, -1, -1 } },
    { T_FIRE_DOWN,         { T_WEAPON_FIRE, T_WEAPON_READY, -1, -1 } },
};
static const int kRuleCount = sizeof(kRules) / sizeof(kRules[0]);

struct CfgCopy {
    unsigned orig, hash, anims, stand;
    BYTE*     obj;            // our copy of the 0x2C-byte header
    unsigned* lookup;         // NSTATES entries
    int authored;             // non-null states in the original
    int filled;               // states this copy added
    short from[NSTATES];      // for `playnpc states`: donor of a filled state, else -1
};
#define MAX_COPIES 32
static CfgCopy g_copies[MAX_COPIES];
static int     g_copyNext = 0;
static CfgCopy* g_activeCopy = nullptr;   // never recycled while in use

// A copy is only good while the config it was taken from is the same loaded
// object: same address, same path hash, same entries array, same STAND entry.
// A level load frees every config, and a reload can put a new one at the old
// address - the entry check catches that, because its records move too.
// Abandoned copies are never freed (a few hundred bytes each): a layer the
// engine has not released yet may still point at one.
static bool CopyValid(const CfgCopy* c, unsigned orig) {
    if (!c->obj || c->orig != orig) return false;
    if (R32(orig + RES_HASH) != c->hash || R32(orig + 0x14) != c->anims) return false;
    unsigned lk = R32(orig + AC_LOOKUP);
    return IsHeap(lk) && R32(lk) == c->stand;
}

static CfgCopy* MakeCopy(unsigned orig, char* why, int whyLen) {
    for (int i = 0; i < MAX_COPIES; i++)
        if (CopyValid(&g_copies[i], orig)) return &g_copies[i];
    if (!VtIs(orig, RVA_VT_ANIMCFG)) { lstrcpynA(why, "not an AnimationLayerConfig", whyLen); return nullptr; }
    unsigned lk = R32(orig + AC_LOOKUP), n = R32(orig + AC_LOOKUPN);
    if (!IsHeap(lk) || n == 0 || n > NSTATES) {
        _snprintf_s(why, whyLen, _TRUNCATE, "config lookup unusable (ptr %08X count %u)", lk, n);
        return nullptr;
    }
    if (!R32(lk)) { lstrcpynA(why, "config has no ANIM_STAND - every fallback needs it", whyLen); return nullptr; }

    CfgCopy* c = &g_copies[g_copyNext];
    if (c == g_activeCopy) { g_copyNext = (g_copyNext + 1) % MAX_COPIES; c = &g_copies[g_copyNext]; }
    g_copyNext = (g_copyNext + 1) % MAX_COPIES;
    memset(c, 0, sizeof(*c));
    HANDLE heap = GetProcessHeap();
    c->obj = (BYTE*)HeapAlloc(heap, HEAP_ZERO_MEMORY, 0x40);
    c->lookup = (unsigned*)HeapAlloc(heap, HEAP_ZERO_MEMORY, NSTATES * 4);
    if (!c->obj || !c->lookup) { lstrcpynA(why, "out of memory", whyLen); return nullptr; }
    __try {
        memcpy(c->obj, (void*)orig, AC_SIZE);
        for (unsigned s = 0; s < n; s++) c->lookup[s] = ((unsigned*)lk)[s];
    } __except (EXCEPTION_EXECUTE_HANDLER) { lstrcpynA(why, "reading the config faulted", whyLen); return nullptr; }
    for (int s = 0; s < NSTATES; s++) { c->from[s] = -1; if (c->lookup[s]) c->authored++; }

    for (int r = 0; r < kRuleCount; r++) {
        int dst = kRules[r].state;
        if (c->lookup[dst]) continue;                       // the NPC authors it
        for (int d = 0; d < 4; d++) {
            int src = kRules[r].donors[d];
            if (src < 0) break;
            if (!c->lookup[src]) continue;
            c->lookup[dst] = c->lookup[src];
            c->from[dst] = (short)src;
            c->filled++;
            break;
        }
    }
    *(unsigned short*)(c->obj + RES_REF) = PIN_REFCOUNT;
    *(unsigned*)(c->obj + AC_LOOKUP) = (unsigned)(uintptr_t)c->lookup;
    *(unsigned*)(c->obj + AC_LOOKUPCAP) = NSTATES;
    *(unsigned*)(c->obj + AC_LOOKUPN) = NSTATES;
    c->orig = orig;
    c->hash = R32(orig + RES_HASH);
    c->anims = R32(orig + 0x14);
    c->stand = R32(lk);
    return c;
}

// What a character is missing goes in the log when its body goes on: every
// filled state with its donor, then each state the player's motion can ask for
// while moving that still has no clip (0x5207E0 plays the character's STAND
// for those). The list is every state the moving motions request by constant
// (a scan of the call sites of 0x5207E0 / 0x520DE0 / 0x520550 / 0x5374A0 /
// 0x537550), boats, pipes, cinematics and first person left out.
static const short kMovingStates[] = {
    A_STAND, A_STANDTURN_L, A_STANDTURN_R, A_WALK, A_WALKTURN_L, A_WALKTURN_R,
    A_TROT, A_TROTTURN_L, A_TROTTURN_R, A_CANTER, A_CANTERTURN_L, A_CANTERTURN_R,
    A_RUN, A_RUNTURN_L, A_RUNTURN_R, A_WALK_BACK, A_TROT_BACK,
    A_WALK_STRAFE_L, A_WALK_STRAFE_R, A_TROT_STRAFE_L, A_TROT_STRAFE_R,
    A_TROT_TURNAROUND, A_CANTER_TURNAROUND, A_RUN_TURNAROUND,
    A_CANTER_SKIDTOSTOP, A_RUN_SKIDTOSTOP, A_CANTER_TO_RUN,
    A_JUMP_STAND, A_JUMP_WALK, A_JUMP_TROT, A_JUMP_CANTER, A_JUMP_RUN,
    A_DJUMP_STAND, A_DJUMP_WALK, A_DJUMP_TROT, A_DJUMP_CANTER, A_DJUMP_RUN,
    A_FALL_NEAR, A_FALL_FAR, A_KNOCK_AIR_QUAD, A_KNOCK_GROUND_QUAD, A_GET_UP_QUAD,
    A_STUMBLE, A_BUCK_ATTACK, A_BUCK_EXHAUSTED, A_BURST_TO_RUN, A_BURST_EXHAUSTED, A_BURST_AIR,
    A_PRONE_QUAD, A_LAND_STAND, A_LAND_WALK, A_LAND_TROT, A_LAND_CANTER, A_LAND_RUN,
    A_IDLE1, A_IDLE2, A_SLIDE, A_TEETER_START, A_TEETER_IDLE, A_DODGE_QUAD,
};

static const char* ShortName(int s) {
    const char* n = StateName(s);
    __try { if (n[0] == 'A' && !strncmp(n, "ANIM_", 5)) return n + 5; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return "?"; }
    return n;
}

static void LogFill(const char* who, const CfgCopy* c) {
    char line[1800]; int len;
    len = _snprintf_s(line, sizeof(line), _TRUNCATE, "fill %s: authors %d states, %d filled:",
                      who, c->authored, c->filled);
    for (int s = 0; s < NSTATES && len < (int)sizeof(line) - 64; s++) {
        if (c->from[s] < 0) continue;
        len += _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE, " %s<-%s", ShortName(s), ShortName(c->from[s]));
    }
    Log("%s", line);
    len = _snprintf_s(line, sizeof(line), _TRUNCATE, "fill %s: no clip while moving (STAND plays):", who);
    int none = 0;
    for (int i = 0; i < (int)(sizeof(kMovingStates) / sizeof(kMovingStates[0])); i++) {
        int s = kMovingStates[i];
        if (c->lookup[s]) continue;
        none++;
        if (len < (int)sizeof(line) - 40) len += _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE, " %s", ShortName(s));
    }
    if (!none) _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE, " none");
    Log("%s", line);
    // Death: the player's Die motion (33, enter 0x5448B0) plays ANIM_DIE once;
    // falls, water and hurts have their own states.
    static const short kDeath[] = { 109, 75, 107, 108, 100, 101, 52, 69 };   // DIE FALL_FAR_SPLAT DROWN DEAD_IN_WATER HURT HURT_PRONE_QUAD KNOCK_GROUND_QUAD PRONE_QUAD
    len = _snprintf_s(line, sizeof(line), _TRUNCATE, "fill %s: death and hurt -", who);
    for (int i = 0; i < (int)(sizeof(kDeath) / sizeof(kDeath[0])) && len < (int)sizeof(line) - 48; i++) {
        int s = kDeath[i];
        const char* how = !c->lookup[s] ? "none" : c->from[s] >= 0 ? "filled" : "own";
        len += _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE, " %s %s", ShortName(s), how);
    }
    Log("%s", line);
}

// Point a freshly built layer at the copy. The layer held one reference on
// the original; it now holds one on the copy instead.
static bool UseCopy(unsigned layer, CfgCopy* c) {
    unsigned orig = R32(layer + AL_CONFIG);
    if (orig != c->orig) return false;
    unsigned copy = (unsigned)(uintptr_t)c->obj;
    RefAdd(copy);
    if (!Wr(layer + AL_CONFIG, copy)) return false;
    unsigned short rc = 0;
    __try { rc = *(unsigned short*)(orig + RES_REF); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (rc > 1) RefRelease(orig);          // never the last one - that is the engine's
    else Log("config %08X refcount %u - left one reference in place", orig, rc);
    return true;
}

// ---------------------------------------------------------------------------
//  the player
// ---------------------------------------------------------------------------

struct PlayerView {
    unsigned player, motion, geo, layer;
    int form;               // 0 Steef, 1 Stranger (index into the mode data)
};

// Everything checked by vtable, nothing assumed.
static bool ViewPlayer(PlayerView* v, char* why, int whyLen) {
    memset(v, 0, sizeof(*v));
    unsigned p = 0, m = 0;
    if (SWSE_PlayerBody(&p, &m) != 1 || !VtIs(p, RVA_VT_PLAYERIMPL)) {
        lstrcpynA(why, "no player (load a level first)", whyLen);
        return false;
    }
    v->player = p;
    v->motion = R32(p + PL_MOTION);
    v->geo = R32(p + PL_GEOM);
    if (!VtIs(v->motion, RVA_VT_MOTIONIMPL)) { lstrcpynA(why, "player motion is not a MotionImpl", whyLen); return false; }
    if (!VtIs(v->geo, RVA_VT_GHI)) { lstrcpynA(why, "player body is not a GeometryHierarchyInst", whyLen); return false; }
    v->layer = R32(v->motion + MI_LAYER);
    if (R32(p + PL_MODEDATA + MD_MOTION) == v->motion) v->form = 0;
    else if (R32(p + PL_MODEDATA + MD_STRIDE + MD_MOTION) == v->motion) v->form = 1;
    else { lstrcpynA(why, "current motion is in neither form's mode data", whyLen); return false; }
    return true;
}

static unsigned ModeData(unsigned player, int form) { return player + PL_MODEDATA + form * MD_STRIDE; }

// ---------------------------------------------------------------------------
//  state
// ---------------------------------------------------------------------------

enum { S_OFF, S_SPIN_IN, S_ACTIVE };
static int g_state = S_OFF;

struct Session {
    unsigned player, motion, oldGeo, inst, layer, def, prefs, typeHash;
    int      form;
    float    oldScale, scale;
    CfgCopy* copy;
    unsigned ikSaved;   bool ikTouched;
    unsigned camSaved;  bool camTouched;
    BYTE     dlockSaved; bool dlockTouched;
    char     name[48];
    Deps     deps;            // held every frame while active
};
static Session g_s;
static void LogAttacks(const char* who);   // the attack gates (below)
static char    g_current[48] = "";

struct SpinReq {
    unsigned player, motion, typeHash;
    float    scaleOverride;
    bool     attach;
    bool     cancel;          // `playnpc off` during the spin: swallow the midpoint, stay yourself
    DWORD    started, loadedMs;
    volatile LONG midpoint;
    Deps     deps;            // held through the spin, handed to the session at the midpoint
};
static SpinReq g_spin;
static DWORD   g_missSince = 0;   // first frame the player body went missing

// One pending load at a time; a new request replaces it. Independent of the
// body state: NPC -> not-yet-loaded NPC keeps the current body until it is in.
struct LoadReq {
    bool     on;
    unsigned typeHash, def, epoch;
    char     name[48];
    Deps     deps;
    DWORD    started, loadedAt;
    bool     spin, attach, waitSaid;
    float    scaleOverride;
};
static LoadReq g_load;

int SWSE_PlayNpcActive() { return g_state != S_OFF; }
const char* SWSE_PlayNpcCurrent() { return g_state == S_ACTIVE && g_current[0] ? g_current : nullptr; }

// ---------------------------------------------------------------------------
//  the spin: one PlayerImpl vtable slot, only while a spin is in flight
// ---------------------------------------------------------------------------
//
// The engine's own transformation: MotionImpl::RequestChange(eDisguise)
// enters ChangeEnter (motion 26), which plays ANIM_DISGUISECHANGE_ENTER on
// the current body. When it ends, ChangeEnter's leave handler (0x53FF30)
// calls player->vfunc152 (0x447D50), which sets PlayerImpl+0x278; the next
// player tick then runs FinishDisguiseChange (0x447D80) and swaps to the
// OTHER form's cached body - the "spin, then back to Stranger or Steef" the
// owner saw. vfunc152 has exactly one caller (0x53FF44), so while our spin is
// in flight that slot records the midpoint instead, and our tick puts the NPC
// on at that moment. The form never changes.

static unsigned g_slotAddr = 0;
static bool     g_hookArmed = false;

static void __fastcall FinishDisguiseHook(void* self, void* /*edx*/) {
    if (g_state == S_SPIN_IN && (unsigned)(uintptr_t)self == g_spin.player) {
        InterlockedExchange(&g_spin.midpoint, 1);
        return;                         // no form swap
    }
    ((vf0_t)(Base() + RVA_FINISHDISGUISE))(self);
}

static bool SlotWrite(unsigned value) {
    if (!g_slotAddr) g_slotAddr = Base() + RVA_VT_PLAYERIMPL + PLV_FINISHDISGUISE;
    DWORD old = 0;
    if (!VirtualProtect((void*)(uintptr_t)g_slotAddr, 4, PAGE_READWRITE, &old)) return false;
    bool ok = true;
    __try { *(volatile unsigned*)g_slotAddr = value; }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    DWORD tmp = 0;
    VirtualProtect((void*)(uintptr_t)g_slotAddr, 4, old, &tmp);
    return ok;
}

static bool ArmSpinHook() {
    if (g_hookArmed) return true;
    if (!g_slotAddr) g_slotAddr = Base() + RVA_VT_PLAYERIMPL + PLV_FINISHDISGUISE;
    if (R32(g_slotAddr) != Base() + RVA_FINISHDISGUISE) {
        Log("spin hook NOT armed: vtable slot reads %08X, expected %08X",
            R32(g_slotAddr), Base() + RVA_FINISHDISGUISE);
        return false;
    }
    if (!SlotWrite((unsigned)(uintptr_t)&FinishDisguiseHook)) return false;
    g_hookArmed = true;
    SWSE_HookNote((void*)(uintptr_t)g_slotAddr, 4, "playnpc", SWSE_HOOK_VTABLE, "PlayerImpl::FinishDisguise");
    return true;
}

static void DisarmSpinHook() {
    if (!g_hookArmed) return;
    if (SlotWrite(Base() + RVA_FINISHDISGUISE)) {
        g_hookArmed = false;
        SWSE_HookForget((void*)(uintptr_t)g_slotAddr);
    }
    else Log("could not restore the vtable slot - it stays hooked but inert");
}

// ---------------------------------------------------------------------------
//  become / revert
// ---------------------------------------------------------------------------

static void RestorePlayerRules(unsigned player) {
    if (!VtIs(player, RVA_VT_PLAYERIMPL)) return;
    if (g_s.ikTouched) {
        unsigned md = ModeData(player, g_s.form);
        if (R32(md + MD_MOTION) == g_s.motion && R32(md + MD_IKCOUNT) == 0)
            Wr(md + MD_IKCOUNT, g_s.ikSaved);
        g_s.ikTouched = false;
    }
    if (g_s.camTouched) {
        if (R32(player + PL_CAMLOCK) == 2) SetCameraLock(player, (int)g_s.camSaved);
        g_s.camTouched = false;
    }
    if (g_s.dlockTouched) {
        BYTE b = 0;
        if (RdB(player + PL_DISGUISELOCK, &b) && b == 1) WrB(player + PL_DISGUISELOCK, g_s.dlockSaved);
        g_s.dlockTouched = false;
    }
}

// Forget the session. The body, layer and motion are never touched here -
// they are gone (level change, reload) or no longer ours. `livePlayer` is the
// player object when it is known to be the same live object; then only its
// own plain fields are put back (camera lock, disguise lock, IK count), and
// only where they still hold the value we wrote.
// ---------------------------------------------------------------------------
//  health and stamina
// ---------------------------------------------------------------------------
//
// The player's health and stamina are (current, max, base) triples on the
// player object at +0x78 and +0x8C (scriptvm PF_HEALTH / PF_STAMINA; the HUD,
// `hp`, god mode and playertune use the same). A character's own are its
// NPCPrefs m_health (+0x448) and m_stamina (+0x44C): what a spawned NPC of the
// type starts with (NPC_TUNING.md as corrected, REFLECT_FIELDS.tsv).
//
// Save state: the player's IO (vtable +0x14, 0x445720) and the persistent
// state (0x4455C0) serialise neither triple - the game sets them up again at
// every level start - so a checkpoint taken as a character stores nothing of
// it.
//
// Rules (the owner's): the character's max on the way in, full for a fresh
// start; on a switch A -> B, B's max at A's fraction; on the way back,
// Stranger's own max at the character's fraction, so playnpc is never a free
// heal. A level change or reload builds a new player: nothing to restore
// then, unless the game kept the same object, when Stranger's max comes back.
#define PL_HEALTH    0x78
#define PL_STAMINA   0x8C
#define NP_HEALTH    0x448
#define NP_STAMINA   0x44C
#define PROTECTED_VITAL 99999.0f   // the game's number for its 22 unkillable characters is 100000

static struct {
    bool     have;            // Stranger's triples are captured
    unsigned player;          // ... on this player object
    float    h[3], s[3];      // Stranger's, as captured on the way in
    float    carryH, carryS;  // a switch's fractions for the next character (< 0: none)
    float    npcH, npcS;      // the max the character was given (0: left alone)
    DWORD    nextCheck;
    bool     switching;       // Revert is part of an A -> B switch
} g_v = { false, 0, {0}, {0}, -1.0f, -1.0f, 0.0f, 0.0f, 0, false };

static bool ReadTri(unsigned p, int off, float* t) {
    __try { for (int i = 0; i < 3; i++) t[i] = *(float*)(uintptr_t)(p + off + 4 * i); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    for (int i = 0; i < 3; i++) if (!(t[i] > -1.0e7f && t[i] < 1.0e7f)) return false;
    return true;
}

static bool WriteTri(unsigned p, int off, float a, float b, float c) {
    if (!SWSE_GameBuildKnown()) return false;
    __try {
        float* f = (float*)(uintptr_t)(p + off);
        f[0] = a; f[1] = b; f[2] = c;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

// A usable character value: finite and positive.
static float CharVital(unsigned prefs, int off) {
    float v = RF(prefs + off);
    return (v > 0.5f && v < 1.0e7f) ? v : 0.0f;
}

static float Frac(const float* t) {
    if (!(t[1] > 0.0f)) return 0.0f;        // no max: dead or zeroed - never a free heal
    float f = t[0] / t[1];
    return !(f == f) ? 1.0f : f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
}

static void VitalsOnBecome(unsigned player, unsigned prefs, const char* name) {
    float fh = 1.0f, fs = 1.0f;
    if (!g_v.have || g_v.player != player) {
        float h[3], s[3];
        if (!ReadTri(player, PL_HEALTH, h) || !ReadTri(player, PL_STAMINA, s)) {
            Log("vitals: could not read Stranger's health/stamina - left as they are");
            g_v.have = false;
            return;
        }
        memcpy(g_v.h, h, sizeof(h)); memcpy(g_v.s, s, sizeof(s));
        g_v.have = true; g_v.player = player;
    } else {
        if (g_v.carryH >= 0.0f) fh = g_v.carryH;
        if (g_v.carryS >= 0.0f) fs = g_v.carryS;
    }
    g_v.carryH = g_v.carryS = -1.0f;
    float hm = CharVital(prefs, NP_HEALTH), sm = CharVital(prefs, NP_STAMINA);
    // The owner: "cap it for natives and townsfolk". The 22 characters the
    // game never lets die carry 100000 (NPC_TUNING.md); as one of them the
    // player gets Stranger's own max instead, at the same fraction.
    bool capH = hm >= PROTECTED_VITAL && g_v.h[1] > 0.0f, capS = sm >= PROTECTED_VITAL && g_v.s[1] > 0.0f;
    if (capH) hm = g_v.h[1];
    if (capS) sm = g_v.s[1];
    if (capH) Log("health: capped at Stranger's max (%.0f) - protected character", hm);
    if (capS) Log("stamina: capped at Stranger's max (%.0f) - protected character", sm);
    g_v.npcH = (hm > 0.0f && WriteTri(player, PL_HEALTH, hm * fh, hm, hm)) ? hm : 0.0f;
    g_v.npcS = (sm > 0.0f && WriteTri(player, PL_STAMINA, sm * fs, sm, sm)) ? sm : 0.0f;
    g_v.nextCheck = GetTickCount() + 500;
    char hs[48], ss[48];
    if (g_v.npcH > 0.0f) _snprintf_s(hs, sizeof(hs), _TRUNCATE, "%.0f/%.0f", hm * fh, hm);
    else lstrcpynA(hs, "Stranger's (it has none)", sizeof(hs));
    if (g_v.npcS > 0.0f) _snprintf_s(ss, sizeof(ss), _TRUNCATE, "%.0f/%.0f", sm * fs, sm);
    else lstrcpynA(ss, "Stranger's (it has none)", sizeof(ss));
    Log("vitals: %s - health %s, stamina %s (Stranger's %.0f/%.0f and %.0f/%.0f held for the way back)",
        name, hs, ss, g_v.h[0], g_v.h[1], g_v.s[0], g_v.s[1]);
    Printf("playnpc: %s's health %s, stamina %s%s", name, hs, ss,
           (capH || capS) ? " - capped at Stranger's own (a character the game never lets die)" : "");
}

// Called from Revert (in place). A switch keeps the fractions for the next
// character; the way back gives Stranger his own max at the same fraction.
static void VitalsOnRevert(unsigned player, const char* name) {
    if (!g_v.have || g_v.player != player) return;
    float h[3], s[3];
    bool okh = ReadTri(player, PL_HEALTH, h), oks = ReadTri(player, PL_STAMINA, s);
    float fh = okh ? Frac(h) : 1.0f, fs = oks ? Frac(s) : 1.0f;
    if (g_v.switching) { g_v.carryH = fh; g_v.carryS = fs; return; }
    if (g_v.npcH > 0.0f || !okh) WriteTri(player, PL_HEALTH, g_v.h[1] * fh, g_v.h[1], g_v.h[2]);
    if (g_v.npcS > 0.0f || !oks) WriteTri(player, PL_STAMINA, g_v.s[1] * fs, g_v.s[1], g_v.s[2]);
    Log("vitals: back from %s at %.0f%% health, %.0f%% stamina -> Stranger %.0f/%.0f, %.0f/%.0f",
        name, fh * 100.0f, fs * 100.0f, g_v.h[1] * fh, g_v.h[1], g_v.s[1] * fs, g_v.s[1]);
    Printf("playnpc: health %.0f%% -> %.0f/%.0f, stamina %.0f%% -> %.0f/%.0f",
           fh * 100.0f, g_v.h[1] * fh, g_v.h[1], fs * 100.0f, g_v.s[1] * fs, g_v.s[1]);
    g_v.have = false;
    g_v.npcH = g_v.npcS = 0.0f;
}

// The session was dropped (level change, reload, death). A new player object
// has the game's own numbers already; the same object gets Stranger's max
// back, at the fraction it has.
static void VitalsOnDrop(unsigned livePlayer) {
    if (g_v.have && livePlayer && livePlayer == g_v.player) {
        float h[3], s[3];
        if (g_v.npcH > 0.0f && ReadTri(livePlayer, PL_HEALTH, h) && (h[1] < g_v.h[1] - 0.5f || h[1] > g_v.h[1] + 0.5f))
            WriteTri(livePlayer, PL_HEALTH, g_v.h[1] * Frac(h), g_v.h[1], g_v.h[2]);
        if (g_v.npcS > 0.0f && ReadTri(livePlayer, PL_STAMINA, s) && (s[1] < g_v.s[1] - 0.5f || s[1] > g_v.s[1] + 0.5f))
            WriteTri(livePlayer, PL_STAMINA, g_v.s[1] * Frac(s), g_v.s[1], g_v.s[2]);
        Log("vitals: session dropped on the same player object - Stranger's max restored");
    }
    g_v.have = false;
    g_v.npcH = g_v.npcS = 0.0f;
    g_v.carryH = g_v.carryS = -1.0f;
}

// While a character is on: if the game puts its own max back (a pickup, an
// upgrade, its difficulty set-up), the character's comes back at the fraction.
static void VitalsTick(unsigned player) {
    if (!g_v.have || g_v.player != player) return;
    DWORD now = GetTickCount();
    if ((int)(now - g_v.nextCheck) < 0) return;
    g_v.nextCheck = now + 500;
    float t[3];
    // A death takes current health to 0 (and `hp 0` takes max with it): the
    // Die motion and the checkpoint reload must run, so nothing is put back
    // while either is 0.
    if (g_v.npcH > 0.0f && ReadTri(player, PL_HEALTH, t) && t[0] > 0.0f && t[1] > 0.5f &&
        (t[1] < g_v.npcH - 0.5f || t[1] > g_v.npcH + 0.5f)) {
        WriteTri(player, PL_HEALTH, g_v.npcH * Frac(t), g_v.npcH, g_v.npcH);
        Log("vitals: the game set max health to %.0f - %s's %.0f put back", t[1], g_s.name, g_v.npcH);
    }
    float hh[3];
    bool dead = !ReadTri(player, PL_HEALTH, hh) || !(hh[0] > 0.0f);
    if (!dead && g_v.npcS > 0.0f && ReadTri(player, PL_STAMINA, t) && t[1] > 0.5f &&
        (t[1] < g_v.npcS - 0.5f || t[1] > g_v.npcS + 0.5f)) {
        WriteTri(player, PL_STAMINA, g_v.npcS * Frac(t), g_v.npcS, g_v.npcS);
        Log("vitals: the game set max stamina to %.0f - %s's %.0f put back", t[1], g_s.name, g_v.npcS);
    }
}

static void Drop(const char* why, unsigned livePlayer = 0) {
    Log("dropped (%s)", why);
    DisarmSpinHook();
    if (livePlayer && livePlayer == g_s.player) RestorePlayerRules(livePlayer);
    VitalsOnDrop(livePlayer == g_s.player ? livePlayer : 0);
    memset(&g_s, 0, sizeof(g_s));
    g_current[0] = 0;
    g_activeCopy = nullptr;
    g_missSince = 0;
    g_state = S_OFF;
}

// Give the motion a layer built from its own form's config, bound to `ghi`.
static bool RestoreFormLayer(unsigned motion, unsigned ghi, char* why, int whyLen) {
    unsigned mp = R32(motion + MI_PREFS);
    const char* path = IsHeap(mp) ? StrField(mp + MP_ANIMCFG) : nullptr;
    if (!path) { lstrcpynA(why, "the form's animation config path is unreadable", whyLen); return false; }
    unsigned layer = CreateAnimLayer(ghi, path);
    if (!layer) { lstrcpynA(why, "rebuilding the form's animation layer failed", whyLen); return false; }
    if (!InstallLayer(motion, layer)) { RefRelease(layer); lstrcpynA(why, "installing the layer failed", whyLen); return false; }
    return true;
}

// Full revert: Stranger's (or Steef's) own body and layer, exactly as found.
static bool Revert(bool quiet, char* why, int whyLen) {
    if (g_state != S_ACTIVE) { lstrcpynA(why, "not playing an NPC", whyLen); return false; }
    unsigned p = g_s.player, m = g_s.motion;
    if (!VtIs(p, RVA_VT_PLAYERIMPL) || !VtIs(m, RVA_VT_MOTIONIMPL) ||
        R32(ModeData(p, g_s.form) + MD_MOTION) != m) {
        Drop("player or motion no longer ours at revert");
        lstrcpynA(why, "the player was rebuilt by the game - nothing to restore", whyLen);
        return false;
    }
    unsigned target = g_s.oldGeo;
    if (!VtIs(target, RVA_VT_GHI)) {
        // Stranger's instance was replaced under us (armor change); use what
        // the mode data holds for this form now.
        target = R32(ModeData(p, g_s.form) + MD_GEO);
        if (!VtIs(target, RVA_VT_GHI)) { Drop("no body to restore"); lstrcpynA(why, "no body to restore", whyLen); return false; }
    }
    bool geoOk = true;
    if (R32(p + PL_GEOM) == g_s.inst) {
        geoOk = SetGeometry(p, target);
        if (geoOk) {
            if (target == g_s.oldGeo) {
                __try { *(float*)(target + GHI_SCALE) = g_s.oldScale; } __except (EXCEPTION_EXECUTE_HANDLER) {}
                Recompose(target);
            }
        } else Log("revert: SetGeometry back to %08X failed", target);
    }
    // The body the motion should animate: the current one if this form is
    // still current, else this form's own body.
    unsigned bindTo = (R32(p + PL_MOTION) == m) ? R32(p + PL_GEOM) : R32(ModeData(p, g_s.form) + MD_GEO);
    char w2[128] = "";
    bool layerOk = false;
    if (R32(m + MI_LAYER) == g_s.layer || R32(R32(m + MI_LAYER) + AL_GHI) != bindTo)
        layerOk = RestoreFormLayer(m, bindTo, w2, sizeof(w2));
    else layerOk = true;                 // the engine already rebuilt it for us
    RestorePlayerRules(p);
    if (!layerOk) Log("revert: %s", w2);
    if (!quiet) Printf("playnpc: back to %s%s", R32(p + PL_DISGUISED) & 0xFF ? "Stranger" : "Steef",
                       (geoOk && layerOk) ? "" : " (partial - see swse_log.txt)");
    Log("reverted %s: body %s, layer %s", g_s.name, geoOk ? "ok" : "FAILED", layerOk ? "ok" : "FAILED");
    VitalsOnRevert(p, g_s.name);
    memset(&g_s, 0, sizeof(g_s));
    g_current[0] = 0;
    g_activeCopy = nullptr;
    g_missSince = 0;
    g_state = S_OFF;
    if (!(geoOk && layerOk)) lstrcpynA(why, w2[0] ? w2 : "partial restore", whyLen);
    return geoOk && layerOk;
}

// Put the NPC on. Instant; the spin wrapper calls this at the midpoint. `deps`
// is the block set that was held to get here; the session keeps holding it.
static bool BecomeNow(int ti, float scaleOverride, bool attach, const Deps* deps, char* why, int whyLen) {
    if (!CheckStateTable()) { lstrcpynA(why, "the exe's animation state table is not the one this was built against", whyLen); return false; }
    NpcType t = g_types[ti];
    if (!t.def) { lstrcpynA(why, "it has no body in this level", whyLen); return false; }
    int fl = DefFlags(t.def), miss = -1;
    if (fl != 1 || !deps || !DepsResident(deps, &miss)) {
        _snprintf_s(why, whyLen, _TRUNCATE, "its data is not in memory (%s)",
                    deps && miss >= 0 ? deps->file[miss] : "its body");
        return false;
    }
    if (!t.cfg || !t.cfgPath) { lstrcpynA(why, "its animation config is not loaded in this level", whyLen); return false; }

    PlayerView v;
    if (!ViewPlayer(&v, why, whyLen)) return false;
    unsigned phys = R32(v.motion + MI_PHYSICS), mot = R32(v.motion + MI_MOTCURRENT);
    if (phys != 0) { lstrcpynA(why, "stand on solid ground first (not in the air, water, a boat or on a pipe)", whyLen); return false; }
    if (mot == MOT_CHANGEENTER || mot == MOT_CHANGELEAVE) { lstrcpynA(why, "a Stranger/Steef transformation is playing", whyLen); return false; }
    unsigned cam = R32(v.player + PL_CAMLOCK);
    if (cam == 1) { lstrcpynA(why, "a scripted first-person section is active", whyLen); return false; }

    CfgCopy* copy = MakeCopy(t.cfg, why, whyLen);
    if (!copy) return false;

    float scale = scaleOverride > 0.0f ? scaleOverride : t.scale;
    if (scale > SCALE_MAX_SAFE) scale = SCALE_MAX_SAFE;
    if (scale < SCALE_MIN_SAFE) scale = SCALE_MIN_SAFE;

    // Everything its materials draw from is in (the deps waited for it): a
    // material that cached the white fallback earlier gets its texture now.
    int rebound = RebindWhite(t.def, t.name);
    if (attach) {
        const unsigned lists[2] = { t.prefs + NP_ATTACH, t.prefs + NP_DEFATTACH };
        for (int l = 0; l < 2; l++) {
            unsigned adata = R32(lists[l]), an = R32(lists[l] + 8);
            if (!IsHeap(adata) || an > 32) continue;
            for (unsigned i = 0; i < an; i++) {
                unsigned tok = R32(adata + i * ATT_STRIDE + ATT_GEOTOKEN);
                unsigned g = (tok && tok != HASH_NULL) ? SWSE_ResourceLookup(tok) : 0;
                if (IsGeoDef(g)) rebound += RebindWhite(g, t.name);
            }
        }
    }

    unsigned inst = CreateInstance(t.def);
    if (!inst) { lstrcpynA(why, "creating its body failed", whyLen); return false; }

    float oldScale = RF(v.geo + GHI_SCALE);
    if (!(oldScale > 0.01f && oldScale < 100.0f)) oldScale = 1.0f;

    if (!SetGeometry(v.player, inst)) {
        RefRelease(inst);
        lstrcpynA(why, "installing its body failed", whyLen);
        return false;
    }
    __try { *(float*)(inst + GHI_SCALE) = scale; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    Recompose(inst);
    SetTickFlags(inst, 5);
    if (attach) {
        ApplyAttachments(v.player, t.prefs);
        char worn[400];
        DescribeAttachments(t.prefs, worn, sizeof(worn));
        Log("attachments: %s wears %s", t.name, worn);
    } else Log("attachments: %s bare (none put on)", t.name);

    unsigned layer = CreateAnimLayer(inst, t.cfgPath);
    if (!layer || !UseCopy(layer, copy) || !InstallLayer(v.motion, layer)) {
        if (layer) RefRelease(layer);
        // roll the body back; the motion still has its own layer
        SetGeometry(v.player, v.geo);
        __try { *(float*)(v.geo + GHI_SCALE) = oldScale; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        Recompose(v.geo);
        RefRelease(inst);
        lstrcpynA(why, "building its animation layer failed - Stranger restored", whyLen);
        return false;
    }
    RefRelease(inst);          // the player and the layer own it now

    memset(&g_s, 0, sizeof(g_s));
    g_s.player = v.player; g_s.motion = v.motion; g_s.oldGeo = v.geo;
    g_s.inst = inst; g_s.layer = layer; g_s.def = t.def; g_s.prefs = t.prefs;
    g_s.typeHash = t.hash; g_s.form = v.form; g_s.oldScale = oldScale; g_s.scale = scale;
    g_s.copy = copy;
    g_s.deps = *deps;
    lstrcpynA(g_s.name, t.name, sizeof(g_s.name));

    // Stranger's foot IK indexes Stranger's bones; the player tick feeds it to
    // whatever body is current (0x44FC2C -> 0x6057E0 -> 0x636630, unchecked).
    unsigned md = ModeData(v.player, v.form);
    g_s.ikSaved = R32(md + MD_IKCOUNT);
    if (g_s.ikSaved && Wr(md + MD_IKCOUNT, 0)) g_s.ikTouched = true;
    // No first person: the engine's own lock (SetCamMode and the FP entry
    // both refuse while it is set). Leaves FP first if the player is in it.
    g_s.camSaved = cam;
    if (cam == 0 && SetCameraLock(v.player, 2)) g_s.camTouched = true;
    // No manual Stranger/Steef toggle while an NPC (ToggleDisguise honours it).
    BYTE dl = 0;
    if (RdB(v.player + PL_DISGUISELOCK, &dl)) {
        g_s.dlockSaved = dl;
        if (dl == 0 && WrB(v.player + PL_DISGUISELOCK, 1)) g_s.dlockTouched = true;
    }

    lstrcpynA(g_current, t.name, sizeof(g_current));
    g_activeCopy = copy;
    g_missSince = 0;
    g_state = S_ACTIVE;
    Log("became %s (%08X): inst %08X layer %08X copy %08X (+%d states) scale %.2f form %d",
        t.name, t.hash, inst, layer, (unsigned)(uintptr_t)copy->obj, copy->filled, scale, v.form);
    LogFill(t.name, copy);
    VitalsOnBecome(v.player, t.prefs, t.name);
    LogAttacks(t.name);
    char files[200];
    DepsFiles(&g_s.deps, files, sizeof(files), false);
    Log("holding %d block(s) resident: %s (%d animations, %d unresolved%s)", g_s.deps.n,
        files[0] ? files : "none", g_s.deps.anims, g_s.deps.unresolved,
        g_s.deps.overflow ? ", SOME NOT HELD - more than 16 blocks" : "");
    Log("textures: %s - its materials reference %d resource(s): %d more bundle(s) held for them, "
        "%d not registered in this level, %d in a level bundle that is not in%s%s%s; %d white fallback(s) rebound",
        t.name, g_s.deps.refs, g_s.deps.refBlocks, g_s.deps.refUnreg, g_s.deps.refOut,
        g_s.deps.refOut ? " (" : "", g_s.deps.refOut ? g_s.deps.refOutFile : "", g_s.deps.refOut ? " - white until its area loads)" : "",
        rebound);
    return true;
}

// ---------------------------------------------------------------------------
//  the gait guard (Act 1)
// ---------------------------------------------------------------------------
//
// MEASURED (lm_level_01, townsfolk_clakkerz, `playnpc runtest`): past about
// 10 u/s the requested weights of TROT, CANTER, RUN and all their turns (6-14)
// went negative and kept falling at the layer's blend rate, 5/s: -1 at 0.8 s,
// -12 at 3 s. Negative blend weights draw the bind pose - the owner's T-pose
// "at max speed", "only in act 1".
//
// WHY ACT 1. The walk anims (0x530170, once per frame per walking actor) take
// a separate path for the player while PlayerImpl::IsDisguised() (vtable
// +0x1D8, the byte at player+0x1E9 - the beta's m_disguised, PL_DISGUISED) is
// true: the player is Stranger, the upright disguised form, not Steef. In Act 1
// the disguise is locked on (m_disguiseLocked), so Act 1 always takes it. On
// that path CANTER is the top gait (past its clip velocity the canter clips
// are sped up, 0x53143B)
// and a CANTER/RUN switch by midpoint follows (0x531764). The turn weights
// divide by each turn clip's rotation rate (0x520F40: anim+0x2C x speed
// adjustment x scale) and the gait lerp (0x40DCD0) by the difference of two
// gait velocities. The townsperson authors ONE straight run clip for TROT,
// CANTER, RUN and every turn: zero rotation, equal velocities. The targets
// that path hands the layer (0x521210) are then not finite, and the layer's
// integrator walks the weights down at its maximum rate for as long as the
// player keeps running. Later acts do not take the disguised path.
//
// THE GUARD. m_disguised is story state that checkpoints save, so it is never
// touched. Instead, while playnpc is active, the walk anims' one call to the
// layer's blend (0x531A04, `call 0x521210`) goes through GaitBlendGuard, which
// looks at the TARGETS before the engine blends toward them - for the player's
// NPC layer only. Targets that are all finite and in [0,1] pass untouched, so
// Stranger, every NPC and every later-act case behave byte for byte as before.
// When a gait or turn target is not (NaN, infinite, below 0 or above 1), the
// gait part is rebuilt from the same inputs the engine uses:
//   * gait velocities as the engine computes them (anim+0x28 x |speed
//     adjustment| x the body's scale), WALK / TROT / CANTER / RUN;
//   * gaits sharing a velocity (one clip on several gaits) count once, the
//     slowest of them standing for the group;
//   * the moving weight (1 - the stand/idle targets) blends linearly between
//     the two gaits around the player's ground speed; at or above the fastest
//     gait, that gait takes it all;
//   * turn variants get 0 - no turn blend while the guard is rebuilding.
// Writes go through the engine's own AllAnimWeights::Set (0x51FB90), so the
// map stays the engine's (a zero erases, as the engine's own zeros do).
// Playback rates are left to the engine: at the character's top speed they
// measured 1.00-1.25x, the feet already keeping up.
#define RVA_WALK_BLENDCALL   0x131A04   // in the walk anims 0x530170: E8 -> 0x521210
#define RVA_LAYER_BLEND      0x121210   // stdcall (layer, AllAnimWeights* targets, float time), ret 0xC
#define RVA_ANIM_FOR_STATE   0x120D60   // edi = layer, [esp+4] = state -> Animation* or 0, ret 4 (reads only)
#define RVA_SOURCE_FOR_STATE 0x120550   // ecx = layer, edi = state -> source record (lookup[0] fallback), ret
#define RVA_WEIGHTS_SET      0x11FB90   // esi = map, edi = key, [esp+4] = float; 0 erases; ret 4
#define ANIM_ROOTSPEED       0x28       // Animation: root-motion speed (the walk anims read it)
#define SRC_SPEEDADJ         0x04       // source record: speed adjustment
#define GHI_SCALE            0x60       // GeometryHierarchyInst: the draw scale the walk anims apply
#define MI_VELOCITY          0x54       // MotionImpl: velocity Vec3 (the walk anims read it)

static bool     g_guardOn = false;          // the call is redirected
static unsigned g_guardRel = 0;             // the original rel32, for the way back
static int      g_guardFires = 0, g_guardFiresLogged = 0;
static DWORD    g_guardLastLog = 0;
static bool     g_guardRawLogged = false;

static unsigned AnimForState(unsigned layer, int state) {
    unsigned fn = Base() + RVA_ANIM_FOR_STATE, r = 0;
    __try {
        __asm {
            push edi
            mov edi, layer
            push state
            call fn
            mov r, eax
            pop edi
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { r = 0; }
    return r;
}

static unsigned SourceForState(unsigned layer, int state) {
    unsigned fn = Base() + RVA_SOURCE_FOR_STATE, r = 0;
    __try {
        __asm {
            push edi
            mov ecx, layer
            mov edi, state
            call fn
            mov r, eax
            pop edi
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { r = 0; }
    return r;
}

static void WeightsSet(unsigned map, int key, float value) {
    unsigned fn = Base() + RVA_WEIGHTS_SET;
    __try {
        __asm {
            push esi
            push edi
            mov esi, map
            mov edi, key
            push value
            call fn
            pop edi
            pop esi
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static float GaitVelocity(unsigned layer, int state) {
    unsigned anim = AnimForState(layer, state);
    if (!IsHeap(anim)) return 0.0f;
    unsigned src = SourceForState(layer, state);
    if (!IsHeap(src)) return 0.0f;
    float v = RF(anim + ANIM_ROOTSPEED) * (float)fabs(RF(src + SRC_SPEEDADJ)) * RF(R32(layer + AL_GHI) + GHI_SCALE);
    return (v == v && v > 0.0f && v < 1.0e6f) ? v : 0.0f;
}

static bool TargetBad(float w) { return !(w == w) || w < -0.001f || w > 1.001f; }
static float Clamp01(float w) { return !(w == w) ? 0.0f : w < 0.0f ? 0.0f : w > 1.0f ? 1.0f : w; }

static void GuardTargets(unsigned layer, unsigned map) {
    unsigned root = R32(map + 4), count = R32(map + 0x10);
    if (!count || count > 200 || root == map || !IsHeap(root)) return;
    int keys[64]; float vals[64]; int n = 0;
    unsigned st[64]; int sp = 0, visits = 0;
    st[sp++] = root;
    while (sp > 0 && n < 64 && visits++ < 256) {
        unsigned nd = st[--sp];
        if (nd == map || !IsHeap(nd)) continue;
        keys[n] = (int)R32(nd + 0x10); vals[n] = RF(nd + 0x14); n++;
        if (sp < 62) { st[sp++] = R32(nd + 0x0C); st[sp++] = R32(nd + 0x08); }
    }
    bool bad = false;
    for (int i = 0; i < n; i++) if (keys[i] >= A_WALK && keys[i] <= A_RUNTURN_R && TargetBad(vals[i])) bad = true;
    if (!bad) return;

    // The stand/idle part stays the engine's; the rest of the weight moves.
    float stand = 0.0f;
    for (int i = 0; i < n; i++) {
        int k = keys[i];
        if (k == A_STAND || k == A_STANDTURN_L || k == A_STANDTURN_R || k == A_IDLE1 || k == A_IDLE2)
            stand += Clamp01(vals[i]);
    }
    float move = Clamp01(1.0f - stand);
    float vel[3] = { RF(g_s.motion + MI_VELOCITY), RF(g_s.motion + MI_VELOCITY + 4), 0.0f };
    float speed = (float)sqrt(vel[0] * vel[0] + vel[1] * vel[1]);
    if (!(speed == speed)) speed = 0.0f;

    static const int kGaits[4] = { A_WALK, A_TROT, A_CANTER, A_RUN };
    float gv[4]; int kept[4]; float kv[4]; int nk = 0;
    for (int g = 0; g < 4; g++) {
        gv[g] = GaitVelocity(layer, kGaits[g]);
        if (gv[g] <= 0.01f) continue;
        if (nk && gv[g] <= kv[nk - 1] + 0.01f) continue;      // same clip / velocity: counted once
        kept[nk] = kGaits[g]; kv[nk] = gv[g]; nk++;
    }
    float want[NSTATES + 1];
    for (int s = A_WALK; s <= A_RUNTURN_R; s++) want[s] = 0.0f;
    if (!nk) want[A_WALK] = move;
    else if (speed <= kv[0]) want[kept[0]] = move;
    else if (speed >= kv[nk - 1]) want[kept[nk - 1]] = move;
    else {
        for (int i = 0; i + 1 < nk; i++) {
            if (speed > kv[i + 1]) continue;
            float t = (speed - kv[i]) / (kv[i + 1] - kv[i]);    // kv strictly increasing: no zero divide
            want[kept[i]] = move * (1.0f - t);
            want[kept[i + 1]] = move * t;
            break;
        }
    }
    if (!g_guardRawLogged) {
        g_guardRawLogged = true;
        char line[1200]; int len;
        len = _snprintf_s(line, sizeof(line), _TRUNCATE,
                          "gait guard: %s at %.2f u/s - the walk anims asked for", g_s.name, speed);
        for (int i = 0; i < n && len < (int)sizeof(line) - 40; i++) {
            if (vals[i] != vals[i]) len += _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE, " %d=NaN", keys[i]);
            else len += _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE, " %d=%g", keys[i], vals[i]);
        }
        Log("%s", line);
        Log("gait guard: gait velocities WALK %.2f TROT %.2f CANTER %.2f RUN %.2f; rebuilt: WALK %.2f TROT %.2f CANTER %.2f RUN %.2f, turns 0",
            gv[0], gv[1], gv[2], gv[3], want[A_WALK], want[A_TROT], want[A_CANTER], want[A_RUN]);
    }
    for (int s = A_WALK; s <= A_RUNTURN_R; s++) WeightsSet(map, s, want[s]);
    g_guardFires++;
}

typedef void (__stdcall *LayerBlendFn)(unsigned layer, unsigned targets, float time);

static void __stdcall GaitBlendGuard(unsigned layer, unsigned targets, float time) {
    if (g_state == S_ACTIVE && layer == g_s.layer && layer) {
        __try { GuardTargets(layer, targets); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    ((LayerBlendFn)(uintptr_t)(Base() + RVA_LAYER_BLEND))(layer, targets, time);
}

// The call site is checked byte for byte before and after: E8 and a rel32 that
// lands on the blend. Anything else, or another patch on those bytes, and the
// guard stays off (logged).
static bool GuardInstall() {
    if (g_guardOn) return true;
    if (!SWSE_GameBuildKnown()) return false;
    unsigned site = Base() + RVA_WALK_BLENDCALL;
    BYTE op = 0;
    if (!RdB(site, &op) || op != 0xE8) { Log("gait guard NOT installed: %08X is not a call", site); return false; }
    unsigned rel = R32(site + 1);
    if (site + 5 + rel != Base() + RVA_LAYER_BLEND) {
        Log("gait guard NOT installed: the call at %08X goes to %08X, not the layer blend", site, site + 5 + rel);
        return false;
    }
    char who[96];
    if (SWSE_HookOwner(site, 5, who, sizeof(who))) { Log("gait guard NOT installed: %s already patched it", who); return false; }
    unsigned nrel = (unsigned)(uintptr_t)&GaitBlendGuard - (site + 5);
    DWORD old;
    if (!VirtualProtect((void*)(uintptr_t)(site + 1), 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    *(volatile unsigned*)(uintptr_t)(site + 1) = nrel;
    VirtualProtect((void*)(uintptr_t)(site + 1), 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)(uintptr_t)site, 5);
    g_guardRel = rel;
    g_guardOn = true;
    g_guardRawLogged = false;
    g_guardFires = g_guardFiresLogged = 0;
    g_guardLastLog = GetTickCount();
    SWSE_HookNote((void*)(uintptr_t)(site + 1), 4, "playnpc", SWSE_HOOK_BYTES, "walk anims -> gait blend guard");
    return true;
}

static void GuardRemove() {
    if (!g_guardOn) return;
    unsigned site = Base() + RVA_WALK_BLENDCALL;
    DWORD old;
    if (!VirtualProtect((void*)(uintptr_t)(site + 1), 4, PAGE_EXECUTE_READWRITE, &old)) {
        Log("gait guard: could not restore the call at %08X - it stays redirected, and passes everything through", site);
        return;
    }
    *(volatile unsigned*)(uintptr_t)(site + 1) = g_guardRel;
    VirtualProtect((void*)(uintptr_t)(site + 1), 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)(uintptr_t)site, 5);
    g_guardOn = false;
    SWSE_HookForget((void*)(uintptr_t)(site + 1));
    if (g_guardFires) Log("gait guard: rebuilt the gait targets on %d frame(s) this session", g_guardFires);
}

// Once per frame: the redirect exists exactly while a character is on.
static void GuardReconcile() {
    if (g_state == S_ACTIVE) {
        if (!g_guardOn) GuardInstall();
        DWORD now = GetTickCount();
        if (g_guardOn && g_guardFires != g_guardFiresLogged && now - g_guardLastLog > 10000) {
            Log("gait guard: fired on %d frame(s) so far (%s)", g_guardFires, g_s.name);
            g_guardFiresLogged = g_guardFires;
            g_guardLastLog = now;
        }
    } else if (g_guardOn) GuardRemove();
}

// ---------------------------------------------------------------------------
//  Stranger's attacks, off while a character is played
// ---------------------------------------------------------------------------
//
// The owner: "no headbutt no crossbow" - for every character in 1.1, and it
// must hold whatever the camera does. The sprint stays. While a character is
// on, three kinds of gate:
//
// * G1, the weapon tick. The player tick calls PlayerImpl::HandleInput_Weapon
//   (0x458710, EDI = the player; the beta's 0x490765) once, at 0x44FADA. That
//   function is the whole crossbow: weapon cycling and selection
//   (Weapon::HandleCycleInput 0x4B7270), the punch-target refresh and the
//   auto-punch pose (UpdatePunchTarget 0x4509B0, WAImpl::RequestPunchReady
//   0x4BD090), the aim focus, and the fire input (Weapon::HandleInput 0x4B3070,
//   whose only caller is 0x458830, -> WAImpl::HandleInput 0x4BE490: fire,
//   reload, and the crossbow punch on Q, buttons 10/11 at 0x4BE4F5). The call
//   goes through WeaponTickGate, which returns at once while a character is on.
//   Fire, reload and the punch also need first person, which playnpc's camera
//   lock refuses; G1 holds even if a script forces first person. Freecam's gate
//   (the jne at 0x44FA75, 1 byte) skips the same call from further up: the two
//   touch different bytes and work in either order.
// * D1, the buck button. PlayerImpl::HandleInput enters its button-19 branch
//   (MOUSE1: RequestBuckAttack, and below the stamina cost the exhausted buck,
//   RequestPlayAnimStand(65) through MotionImpl +0x128) past `je 0x45777B` at
//   0x457675. While a character is on it is `nop; jmp 0x45777B` (0F 84 -> 90 E9,
//   the same rel32), so the whole branch is skipped. The sprint, button 20,
//   starts at 0x45777B and runs as before.
// * The hits themselves. Motion code the buttons do not guard reaches the
//   player's attack actions, so those refuse for the played player (PlayerImpl
//   vtable slot swaps; the PlayerImpl vtable is the player's alone):
//     DoBuckAttack +0x2A8 (0x451470)  the buck motion (31)'s hit - also the
//                                     ram: a collision at speed puts the player
//                                     in motion 31 - and the jump attack's
//     DoHeadButt   +0x2B0 (0x451A10)  the burst's damage keys (0x540070, text
//                                     keys 0x25/0x26), the jump attack's landing
//                                     (0x52BE93) and repeats (0x540780)
//     DoRamStart   +0x2B4 (0x451DF0)  the burst's ram effect (GenericStartAV)
//     DoRam        +0x2B8 (0x451E10)  ram and slam damage on what the player hits
//                                     (0x52D889 knock/slam, 0x52DC54 wall ram)
//   A collision rams when MotionImpl::CanRam (0x52CDD0) says so: never in
//   first person; disguised (Act 1), only faster than midway between the
//   canter and run clips, which a character's top speed can reach; undisguised,
//   always.
#define RVA_WEAPONTICK_SITE   0x04FAD8   // 8B FD mov edi,ebp ; E8 31 8C 00 00 call 0x458710
#define RVA_WEAPONTICK_CALL   0x04FADA
#define RVA_WEAPONTICK        0x058710   // PlayerImpl::HandleInput_Weapon, EDI = player, plain ret
#define RVA_BUCKBRANCH_JCC    0x057675   // 0F 84 00 01 00 00  je 0x45777B
#define PLV_DOBUCK            0x2A8
#define PLV_DOHEADBUTT        0x2B0
#define PLV_DORAMSTART        0x2B4
#define PLV_DORAM             0x2B8
#define RVA_DOBUCK            0x051470
#define RVA_DOHEADBUTT        0x051A10
#define RVA_DORAMSTART        0x051DF0
#define RVA_DORAM             0x051E10
static const BYTE kWeaponTickSite[7] = { 0x8B, 0xFD, 0xE8, 0x31, 0x8C, 0x00, 0x00 };
static const BYTE kBuckBranch[6]     = { 0x0F, 0x84, 0x00, 0x01, 0x00, 0x00 };

static volatile LONG g_blockWeapons = 0;        // 1: skip the player's weapon tick
static unsigned g_weaponTickFn = 0;             // 0x458710, absolute
static unsigned g_blockPlayer = 0;              // the played player, while its hits are refused
static unsigned g_origDoBuck = 0, g_origDoHeadButt = 0, g_origDoRamStart = 0, g_origDoRam = 0;
static bool     g_g1On = false, g_d1On = false;
static unsigned g_g1Rel = 0;
static bool     g_slotOn[4] = { false, false, false, false };

__declspec(naked) static void WeaponTickGate() {
    __asm {
        cmp g_blockWeapons, 0
        jne skip
        jmp dword ptr [g_weaponTickFn]      // EDI (the player) untouched
    skip:
        ret
    }
}
// thiscall, no stack arguments; the callers ignore the result.
__declspec(naked) static void DoBuckGate() {
    __asm {
        mov eax, g_blockPlayer
        test eax, eax
        je pass
        cmp ecx, eax
        jne pass
        xor eax, eax
        ret
    pass:
        jmp dword ptr [g_origDoBuck]
    }
}
__declspec(naked) static void DoHeadButtGate() {
    __asm {
        mov eax, g_blockPlayer
        test eax, eax
        je pass
        cmp ecx, eax
        jne pass
        xor eax, eax
        ret
    pass:
        jmp dword ptr [g_origDoHeadButt]
    }
}
__declspec(naked) static void DoRamStartGate() {
    __asm {
        mov eax, g_blockPlayer
        test eax, eax
        je pass
        cmp ecx, eax
        jne pass
        xor eax, eax
        ret
    pass:
        jmp dword ptr [g_origDoRamStart]
    }
}
// DoRam: thiscall, five stack arguments (ret 0x14), returns al - 0: no damage.
__declspec(naked) static void DoRamGate() {
    __asm {
        mov eax, g_blockPlayer
        test eax, eax
        je pass
        cmp ecx, eax
        jne pass
        xor eax, eax
        ret 0x14
    pass:
        jmp dword ptr [g_origDoRam]
    }
}

static bool CodeWrite(unsigned addr, const BYTE* bytes, unsigned n) {
    DWORD old = 0, tmp = 0;
    if (!VirtualProtect((void*)(uintptr_t)addr, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    bool ok = true;
    __try { for (unsigned i = 0; i < n; i++) *(volatile BYTE*)(uintptr_t)(addr + i) = bytes[i]; }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    VirtualProtect((void*)(uintptr_t)addr, n, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), (void*)(uintptr_t)addr, n);
    return ok;
}

static bool BytesAre(unsigned addr, const BYTE* want, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        BYTE b = 0;
        if (!RdB(addr + i, &b) || b != want[i]) return false;
    }
    return true;
}

static bool VtWrite(unsigned slot, unsigned value) {
    DWORD old = 0, tmp = 0;
    if (!VirtualProtect((void*)(uintptr_t)slot, 4, PAGE_READWRITE, &old)) return false;
    bool ok = true;
    __try { *(volatile unsigned*)(uintptr_t)slot = value; }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    VirtualProtect((void*)(uintptr_t)slot, 4, old, &tmp);
    return ok;
}

struct HitSlot { unsigned plv, rva; void* stub; unsigned* orig; const char* label; };
static const HitSlot kHitSlots[4] = {
    { PLV_DOBUCK,     RVA_DOBUCK,     (void*)&DoBuckGate,     &g_origDoBuck,     "PlayerImpl::DoBuckAttack" },
    { PLV_DOHEADBUTT, RVA_DOHEADBUTT, (void*)&DoHeadButtGate, &g_origDoHeadButt, "PlayerImpl::DoHeadButt" },
    { PLV_DORAMSTART, RVA_DORAMSTART, (void*)&DoRamStartGate, &g_origDoRamStart, "PlayerImpl::DoRamStart" },
    { PLV_DORAM,      RVA_DORAM,      (void*)&DoRamGate,      &g_origDoRam,      "PlayerImpl::DoRam" },
};

static void AttackGatesInstall() {
    if (!SWSE_GameBuildKnown()) return;
    char who[96];
    // G1: the one call to the weapon tick.
    if (!g_g1On) {
        unsigned site = Base() + RVA_WEAPONTICK_SITE, call = Base() + RVA_WEAPONTICK_CALL;
        g_weaponTickFn = Base() + RVA_WEAPONTICK;
        if (!BytesAre(site, kWeaponTickSite, sizeof(kWeaponTickSite)))
            Log("attack gate G1 NOT installed: the weapon tick call at %08X is not 8B FD E8 31 8C 00 00", site);
        else if (SWSE_HookOwner(call + 1, 4, who, sizeof(who)))
            Log("attack gate G1 NOT installed: %s already patched %08X", who, call + 1);
        else {
            unsigned rel = R32(call + 1), nrel = (unsigned)(uintptr_t)&WeaponTickGate - (call + 5);
            if (CodeWrite(call + 1, (const BYTE*)&nrel, 4)) {
                g_g1Rel = rel; g_g1On = true;
                SWSE_HookNote((void*)(uintptr_t)(call + 1), 4, "playnpc", SWSE_HOOK_BYTES, "weapon tick -> playnpc attack gate (G1)");
            }
        }
    }
    // D1: the button-19 branch.
    if (!g_d1On) {
        unsigned jcc = Base() + RVA_BUCKBRANCH_JCC;
        static const BYTE skip[2] = { 0x90, 0xE9 };
        if (!BytesAre(jcc, kBuckBranch, sizeof(kBuckBranch)))
            Log("attack gate D1 NOT installed: the buck branch at %08X is not 0F 84 00 01 00 00", jcc);
        else if (SWSE_HookOwner(jcc, 2, who, sizeof(who)))
            Log("attack gate D1 NOT installed: %s already patched %08X", who, jcc);
        else if (CodeWrite(jcc, skip, 2)) {
            g_d1On = true;
            SWSE_HookNote((void*)(uintptr_t)jcc, 2, "playnpc", SWSE_HOOK_BYTES, "buck/headbutt button branch skipped (D1)");
        }
    }
    // The hits.
    for (int i = 0; i < 4; i++) {
        if (g_slotOn[i]) continue;
        const HitSlot& h = kHitSlots[i];
        unsigned slot = Base() + RVA_VT_PLAYERIMPL + h.plv;
        if (R32(slot) != Base() + h.rva) {
            Log("attack gate: %s's slot reads %08X, expected %08X - left alone", h.label, R32(slot), Base() + h.rva);
            continue;
        }
        if (SWSE_HookOwner(slot, 4, who, sizeof(who))) { Log("attack gate: %s already patched by %s", h.label, who); continue; }
        *h.orig = R32(slot);
        if (VtWrite(slot, (unsigned)(uintptr_t)h.stub)) {
            g_slotOn[i] = true;
            SWSE_HookNote((void*)(uintptr_t)slot, 4, "playnpc", SWSE_HOOK_VTABLE, h.label);
        }
    }
}

static void AttackGatesRemove() {
    InterlockedExchange(&g_blockWeapons, 0);
    g_blockPlayer = 0;
    if (g_g1On) {
        unsigned call = Base() + RVA_WEAPONTICK_CALL;
        if (CodeWrite(call + 1, (const BYTE*)&g_g1Rel, 4)) {
            g_g1On = false;
            SWSE_HookForget((void*)(uintptr_t)(call + 1));
        } else Log("attack gate G1: could not restore %08X - it stays, passing everything through", call);
    }
    if (g_d1On) {
        unsigned jcc = Base() + RVA_BUCKBRANCH_JCC;
        if (CodeWrite(jcc, kBuckBranch, 2)) {
            g_d1On = false;
            SWSE_HookForget((void*)(uintptr_t)jcc);
        } else Log("attack gate D1: could not restore %08X - the buck stays off", jcc);
    }
    for (int i = 0; i < 4; i++) {
        if (!g_slotOn[i]) continue;
        const HitSlot& h = kHitSlots[i];
        unsigned slot = Base() + RVA_VT_PLAYERIMPL + h.plv;
        if (VtWrite(slot, *h.orig)) {
            g_slotOn[i] = false;
            SWSE_HookForget((void*)(uintptr_t)slot);
        } else Log("attack gate: could not restore %s - it stays, passing everything through", h.label);
    }
}

static bool AnyAttackGate() {
    return g_g1On || g_d1On || g_slotOn[0] || g_slotOn[1] || g_slotOn[2] || g_slotOn[3] ||
           g_blockWeapons || g_blockPlayer;
}

// Once per frame: the gates exist exactly while a character is on.
static void AttackGatesReconcile() {
    if (g_state == S_ACTIVE) {
        if (!g_g1On || !g_d1On || !g_slotOn[0] || !g_slotOn[1] || !g_slotOn[2] || !g_slotOn[3])
            AttackGatesInstall();
        InterlockedExchange(&g_blockWeapons, 1);
        g_blockPlayer = g_s.player;
    } else if (AnyAttackGate()) {
        AttackGatesRemove();
    }
}

static void LogAttacks(const char* who) {
    Log("attack: none (blocked) - %s: no crossbow (the weapon tick is skipped), no buck, headbutt or ram "
        "(the button branch is skipped and the player's hits refuse); the sprint stays", who);
}

// ---------------------------------------------------------------------------
//  diagnostics (QA): what the animation layer plays, and a scripted run
// ---------------------------------------------------------------------------
//
// `playnpc anims` logs one sample; `playnpc runtest [ms] [sprint]` holds W
// (and, with `sprint`, the right mouse button) and logs a sample every 250 ms.
// Works as Stranger/Steef too, so the player's own states at full run can be
// compared with a character's. The samples only read; the run's input is the
// one write (below).
//
// AnimationLayer (HD): active animations {data +0x0C, cap +0x10, count +0x14},
// 0xC each {BaseControl* +0, activeSource +4, state +8} (0x5207E0). The
// requested weights (AllAnimWeights, an STLport map<int,float>) keep their
// header node INSIDE the layer: +0x50 {colour, root +0x54, leftmost +0x58,
// rightmost +0x5C}, count +0x60 (MEASURED live: one node, root = leftmost =
// rightmost, count 1). A node: left +0x08, right +0x0C, key +0x10, value +0x14.
//
// BaseControl +0x08 -> the Granny control. Its fields, from AnimationControl's
// own getters (vtable 0x789F0C: GetSpeed 0x62FA40 reads +0x40, GetWeight
// 0x62FA50 reads +0x54; Stop 0x63D5A0 zeroes +0x38/+0x3C): local clock +0x3C,
// playback speed +0x40, local duration +0x44, weight +0x54.
#define AL_ACTIVE      0x0C
#define AL_ACTIVEN     0x14
#define AL_WHDR        0x50        // the weights map's header node
#define AL_WCOUNT      0x60
#define BC_GRANNY      0x08        // BaseControl -> granny_control (beta +0x10)
#define BC_ANIM        0x10        // BaseControl -> Animation (beta m_spAnimation +0x18)
#define GC_CLOCK       0x3C
#define GC_SPEED       0x40
#define GC_DURATION    0x44
#define GC_WEIGHT      0x54

static int ReadWeights(unsigned layer, int* keys, float* vals, int max) {
    unsigned hdr = layer + AL_WHDR, root = R32(hdr + 4), count = R32(layer + AL_WCOUNT);
    if (!count || count > 200 || root == hdr || !IsHeap(root)) return 0;
    unsigned st[64]; int sp = 0, n = 0, visits = 0;
    st[sp++] = root;
    while (sp > 0 && n < max && visits++ < 256) {
        unsigned nd = st[--sp];
        if (nd == hdr || !IsHeap(nd)) continue;
        int k = (int)R32(nd + 0x10);
        if (k >= 0 && k <= NSTATES) { keys[n] = k; vals[n] = RF(nd + 0x14); n++; }
        if (sp < 62) { st[sp++] = R32(nd + 0x0C); st[sp++] = R32(nd + 0x08); }
    }
    for (int i = 1; i < n; i++)                      // by state, for reading
        for (int j = i; j > 0 && keys[j - 1] > keys[j]; j--) {
            int tk = keys[j]; keys[j] = keys[j - 1]; keys[j - 1] = tk;
            float tv = vals[j]; vals[j] = vals[j - 1]; vals[j - 1] = tv;
        }
    return n;
}

// One line: motion, physics, ground speed, the requested weights (NaN shown),
// then every active animation as state[weight xSpeed clock/duration], marked
// NOANIM when its control has no Animation and UNLOADED when the Animation's
// data is not resident - either draws the bind pose.
static void DumpAnims(const char* tag, float speed) {
    PlayerView v; char why[128];
    if (!ViewPlayer(&v, why, sizeof(why))) { Log("anims %s: %s", tag, why); return; }
    unsigned L = v.layer;
    char line[1600]; int len = 0;
    len += _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE,
                       "anims %s: %s mot %u phys %u speed %.2f | weights",
                       tag, g_state == S_ACTIVE ? g_s.name : (v.form ? "Stranger" : "Steef"),
                       R32(v.motion + MI_MOTCURRENT), R32(v.motion + MI_PHYSICS), speed);
    int keys[64]; float vals[64];
    int nw = ReadWeights(L, keys, vals, 64);
    for (int i = 0; i < nw && len < (int)sizeof(line) - 40; i++) {
        float w = vals[i];
        if (w != w) len += _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE, " %d=NaN", keys[i]);
        else if (w > 0.0005f || w < -0.0005f)
            len += _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE, " %d=%.3f", keys[i], w);
    }
    unsigned data = R32(L + AL_ACTIVE), n = R32(L + AL_ACTIVEN);
    len += _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE, " | active %u:", n);
    for (unsigned i = 0; i < n && i < 24 && IsHeap(data) && len < (int)sizeof(line) - 60; i++) {
        unsigned e = data + i * 0xC, ctl = R32(e), gc = IsHeap(ctl) ? R32(ctl + BC_GRANNY) : 0;
        int st = (int)R32(e + 8);
        if (!IsHeap(gc)) { len += _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE, " %d[?]", st); continue; }
        unsigned an = R32(ctl + BC_ANIM);
        const char* bad = !IsHeap(an) ? " NOANIM" : !ResResident(an) ? " UNLOADED" : "";
        len += _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE, " %d[w%.2f x%.2f %.2f/%.2f%s]",
                           st, RF(gc + GC_WEIGHT), RF(gc + GC_SPEED), RF(gc + GC_CLOCK), RF(gc + GC_DURATION), bad);
    }
    Log("%s", line);
}

// The sprint is not on the keyboard. BurstToRun (MotionImpl vtable +0x168)
// answers game button 20 in the HD numbering (the beta's eBurstToRun, 21: the
// HD dropped ePunch), which the button map 0x762CF8 puts on pad button 16 -
// "fireRight", bound by config.txt's fire2 = MOUSE2. The game reads mouse
// buttons only from raw input: its WM_INPUT handler (0x5F7F70) keeps them as
// bits at 0x9D5520 (bit 0 left, bit 1 right). SWSE's key injection posts
// window key messages and cannot reach them, so a sprint run sets bit 1 for
// the length of the run - what holding the button does - and clears it on
// every way out: the run's end, `playnpc off`, a new runtest.
#define RVA_MOUSEBUTTONS  0x5D5520
#define MOUSE_LEFT        1u         // fire1: the buck (HD button 19) - `runtest ... buck`
#define MOUSE_RIGHT       2u

static struct {
    bool on, sprint, rmb, buck, lmb;
    DWORD start, holdEnd, end, next, lastT;
    float last[3]; bool haveLast; int n; float top;
} g_rt;

static void RtMouseBit(unsigned bit, bool down) {
    if (!SWSE_GameBuildKnown()) return;
    volatile unsigned* bits = (volatile unsigned*)(Base() + RVA_MOUSEBUTTONS);
    __try {
        if (down) *bits |= bit;
        else      *bits &= ~bit;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static void RtMouse(bool down)     { RtMouseBit(MOUSE_RIGHT, down); g_rt.rmb = down; }
static void RtMouseLeft(bool down) { RtMouseBit(MOUSE_LEFT, down);  g_rt.lmb = down; }

static void RunTestStop(const char* why) {
    if (g_rt.rmb) RtMouse(false);
    if (g_rt.lmb) RtMouseLeft(false);
    if (g_rt.on) Log("runtest %s (%d samples, top speed %.2f; gait guard %s, fired on %d frame(s) this session)",
                     why, g_rt.n, g_rt.top, g_guardOn ? "installed" : "not installed", g_guardFires);
    g_rt.on = false;
}

static void RunTestTick() {
    DWORD now = GetTickCount();
    // The button: down from 400 ms (the run under way) to the end of the hold.
    bool want = g_rt.sprint && (int)(now - (g_rt.start + 400)) >= 0 && (int)(now - g_rt.holdEnd) < 0;
    if (want) RtMouse(true);                        // every frame: a real release would clear it
    else if (g_rt.rmb) RtMouse(false);
    // The buck: the left button, pressed at 800 ms for 300 ms (a click).
    bool wantL = g_rt.buck && (int)(now - (g_rt.start + 800)) >= 0 && (int)(now - (g_rt.start + 1100)) < 0;
    if (wantL) RtMouseLeft(true);
    else if (g_rt.lmb) RtMouseLeft(false);
    if ((int)(now - g_rt.next) < 0) return;
    g_rt.next = now + 250;
    float p[3], speed = 0.0f;
    if (SWSE_PosGet(p) == 1) {
        if (g_rt.haveLast && now != g_rt.lastT) {
            float dx = p[0] - g_rt.last[0], dy = p[1] - g_rt.last[1];
            speed = (float)sqrt(dx * dx + dy * dy) * 1000.0f / (float)(now - g_rt.lastT);
        }
        memcpy(g_rt.last, p, sizeof(p)); g_rt.haveLast = true; g_rt.lastT = now;
    }
    if (speed > g_rt.top) g_rt.top = speed;
    char tag[24];
    _snprintf_s(tag, sizeof(tag), _TRUNCATE, "t+%lu", now - g_rt.start);
    DumpAnims(tag, speed);
    g_rt.n++;
    if ((int)(now - g_rt.end) >= 0) RunTestStop("done");
}

static void RunTest(int ms, bool sprint, bool buck) {
    if (ms < 500) ms = 500;
    if (ms > 15000) ms = 15000;
    RunTestStop("replaced by a new one");
    memset(&g_rt, 0, sizeof(g_rt));
    SWSE_QueueKey(0x11, 0, ms);                     // W, forward
    g_rt.on = true;
    g_rt.sprint = sprint;
    g_rt.buck = buck;
    g_rt.start = GetTickCount();
    g_rt.next = g_rt.start;
    g_rt.holdEnd = g_rt.start + (DWORD)ms;
    g_rt.end = g_rt.start + (DWORD)ms + 750;
    Log("runtest: holding W%s%s for %d ms", sprint ? " + the right mouse button (sprint)" : "",
        buck ? " + a left click at 0.8 s (the buck)" : "", ms);
    Printf("playnpc: runtest - holding W%s%s for %d ms; samples every 250 ms in swse_log.txt",
           sprint ? " + the right mouse button (sprint)" : "", buck ? " + a left click (buck)" : "", ms);
}

// ---------------------------------------------------------------------------
//  per frame
// ---------------------------------------------------------------------------

// While the engine's ChangeEnter is still playing, the hook must stay: if it
// were taken off now, the transformation's own end would swap the form. So
// the spin only ends at its midpoint - or, if ChangeEnter never started or
// has already gone (it cannot leave without calling the slot), after a grace.
static void SpinTick() {
    unsigned p = 0, m = 0;
    if (SWSE_PlayerBody(&p, &m) != 1) {
        // Wait out a blip: taking the hook off while ChangeEnter still plays
        // would let its end swap the form.
        DWORD now = GetTickCount();
        if (!g_missSince) g_missSince = now ? now : 1;
        if (now - g_missSince <= 1500) return;
    }
    g_missSince = 0;
    if (!p || p != g_spin.player || R32(p + PL_MOTION) != g_spin.motion) {
        DisarmSpinHook();       // the transformation went with the old player
        g_state = S_OFF;
        SWSE_ConsolePrint("playnpc: the player changed during the spin - cancelled");
        return;
    }
    bool mid = InterlockedCompareExchange(&g_spin.midpoint, 0, 0) != 0;
    DWORD age = GetTickCount() - g_spin.started;
    bool inChange = R32(g_spin.motion + MI_MOTCURRENT) == MOT_CHANGEENTER;
    bool gone = !mid && !inChange && age > 750;     // it never started, or ended oddly
    bool stuck = !mid && age > 20000;               // give up; leave the slot hooked-inert
    if (!mid && !gone && !stuck) return;
    if (stuck && inChange) {
        Log("spin: ChangeEnter still running after 20 s - leaving the slot armed (inert) until it ends");
        g_spin.cancel = true;
        return;
    }
    DisarmSpinHook();
    g_state = S_OFF;
    if (g_spin.cancel) { SWSE_ConsolePrint("playnpc: spin cancelled - still yourself"); return; }
    ScanTypes();
    int ti = -1;
    for (int i = 0; i < g_typeCount; i++) if (g_types[i].hash == g_spin.typeHash) ti = i;
    char why[200] = "";
    if (ti < 0) { SWSE_ConsolePrint("playnpc: the character left the level during the spin"); return; }
    if (BecomeNow(ti, g_spin.scaleOverride, g_spin.attach, &g_spin.deps, why, sizeof(why))) {
        char loaded[40] = "";
        if (g_spin.loadedMs) _snprintf_s(loaded, sizeof(loaded), _TRUNCATE, " (loaded in %lu ms)", g_spin.loadedMs);
        Printf("playnpc: you are %s%s%s", g_types[ti].name, loaded, gone ? " (no transformation played)" : "");
    } else {
        Printf("playnpc: cannot become %s - %s", g_types[ti].name, why);
    }
}

static void StartBecome(int ti, const Deps* d, bool spin, bool attach, float scaleOverride, DWORD loadedMs);

static void CancelLoad(const char* why, bool say) {
    if (!g_load.on) return;
    Log("load %s cancelled (%s) after %lu ms", g_load.name, why, GetTickCount() - g_load.started);
    if (say) Printf("playnpc: stopped loading %s (%s)", g_load.name, why);
    g_load.on = false;
}

// The transient reasons a body cannot go on right now ("waiting until ...").
static bool PlayerCanChange(char* why, int whyLen) {
    PlayerView v;
    if (!ViewPlayer(&v, why, whyLen)) return false;
    if (R32(v.motion + MI_PHYSICS) != 0) { lstrcpynA(why, "you stand on solid ground", whyLen); return false; }
    unsigned mot = R32(v.motion + MI_MOTCURRENT);
    if (mot == MOT_CHANGEENTER || mot == MOT_CHANGELEAVE) { lstrcpynA(why, "the transformation finishes", whyLen); return false; }
    if (R32(v.player + PL_CAMLOCK) == 1) { lstrcpynA(why, "the scripted first-person section ends", whyLen); return false; }
    return true;
}

// A pending load: hold its blocks, log each as it arrives, swap when all are
// in (and the player can take a new body), give up after LOAD_TIMEOUT_MS.
static void LoadTick() {
    DWORD now = GetTickCount(), age = now - g_load.started;
    if (!SWSE_LevelUp() || SWSE_LevelEpoch() != g_load.epoch) { CancelLoad("the level changed", true); return; }
    HoldDeps(&g_load.deps);
    for (int i = 0; i < g_load.deps.n; i++) {
        if (g_load.deps.inAt[i] || !DepIn(&g_load.deps, i)) continue;
        g_load.deps.inAt[i] = age ? age : 1;
        Log("load %s: %s in after %lu ms", g_load.name, g_load.deps.file[i], age);
    }
    int miss = -1;
    bool in = DepsResident(&g_load.deps, &miss) && VtIs(g_load.def, RVA_VT_GHD) && DefFlags(g_load.def) == 1;
    if (in && g_load.deps.refsPending) {
        // The bodies are in: what their materials draw from can be read now,
        // and asked for the same way. Once - a body that still cannot be read
        // does not hold the load up.
        ScanTypes();
        int ti = -1;
        for (int i = 0; i < g_typeCount; i++) if (g_types[i].hash == g_load.typeHash) ti = i;
        if (ti >= 0 && g_types[ti].def == g_load.def) {
            Deps nd;
            ComputeDeps(g_types[ti], &nd);
            for (int i = 0; i < nd.n; i++)
                for (int j = 0; j < g_load.deps.n; j++)
                    if (nd.block[i] == g_load.deps.block[j]) nd.inAt[i] = g_load.deps.inAt[j];
            nd.refsPending = false;
            g_load.deps = nd;
            HoldDeps(&g_load.deps);
            char more[200];
            DepsFiles(&g_load.deps, more, sizeof(more), true);
            Log("load %s: body in; its materials reference %d resource(s)%s%s", g_load.name, nd.refs,
                more[0] ? " - still needs " : " - all in", more);
            in = DepsResident(&g_load.deps, &miss);
        } else g_load.deps.refsPending = false;
    }
    if (!in) {
        if (age <= LOAD_TIMEOUT_MS) return;
        int type = -1, depth = 0, pos = -1, res = -1;
        char f[24] = "its body";
        if (miss >= 0) {
            unsigned b = g_load.deps.block[miss];
            lstrcpynA(f, g_load.deps.file[miss], sizeof(f));
            pos = QueuePos(b, &type, &depth);
            res = (int)R32(b + BLK_RESIDENCY);
        }
        for (int i = 0; i < g_load.deps.n; i++) {
            if (DepIn(&g_load.deps, i)) continue;
            int t2 = -1, d2 = 0, p2 = QueuePos(g_load.deps.block[i], &t2, &d2);
            Log("load %s: %s still not in after %lu ms (residency %u, resource flags %d, lazy %d, queue %d/%d type %d)",
                g_load.name, g_load.deps.file[i], age, R32(g_load.deps.block[i] + BLK_RESIDENCY),
                (int)(R32(g_load.deps.rep[i] + RES_BLOCK) & 3), (int)R32(g_load.deps.block[i] + BLK_LAZYNEED),
                p2, d2, t2);
        }
        const char* state = pos >= 0 ? (type == 1 ? "queued for release" : "queued, not started")
                          : res == 2 ? "being released" : res == 1 ? "in, but not marked usable" : "never requested";
        Printf("playnpc: %s did not load in %d s - still waiting on %s (%s; %d block request(s) pending). Try again, or go nearer one.",
               g_load.name, LOAD_TIMEOUT_MS / 1000, f, state, depth);
        g_load.on = false;
        return;
    }
    if (!g_load.loadedAt) {
        g_load.loadedAt = now ? now : 1;
        Log("load %s: complete after %lu ms", g_load.name, age);
    }
    if (g_state == S_SPIN_IN) return;
    char why[160] = "";
    if (!PlayerCanChange(why, sizeof(why))) {
        if (now - g_load.loadedAt > LAND_WAIT_MS) {
            Printf("playnpc: %s is loaded, but you could not take a new body for %d s (waiting until %s) - cancelled",
                   g_load.name, LAND_WAIT_MS / 1000, why);
            g_load.on = false;
        } else if (!g_load.waitSaid) {
            Printf("playnpc: %s is loaded - waiting until %s", g_load.name, why);
            g_load.waitSaid = true;
        }
        return;
    }
    LoadReq L = g_load;
    g_load.on = false;
    ScanTypes();
    int ti = -1;
    for (int i = 0; i < g_typeCount; i++) if (g_types[i].hash == L.typeHash) ti = i;
    if (ti < 0 || g_types[ti].def != L.def) { Printf("playnpc: %s left the level while it loaded", L.name); return; }
    StartBecome(ti, &L.deps, L.spin, L.attach, L.scaleOverride, age ? age : 1);
}

void SWSE_PlayNpcHoldNow() {
    if (g_load.on) HoldDeps(&g_load.deps);
    if (g_state == S_SPIN_IN) HoldDeps(&g_spin.deps);
    else if (g_state == S_ACTIVE) HoldDeps(&g_s.deps);
}

// A fault inside a run must not leave the mouse button down.
static void RunTestTickSafe() {
    __try { RunTestTick(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        RtMouse(false);
        RtMouseLeft(false);
        g_rt.on = false;
        Log("runtest: fault in the sampler - stopped, the mouse buttons released");
    }
}

void SWSE_PlayNpcTick() {
    GuardReconcile();                       // the gait guard exists exactly while a character is on
    AttackGatesReconcile();                 // and so do the attack gates
    if (g_rt.on || g_rt.rmb || g_rt.lmb) RunTestTickSafe();
    if (g_state == S_OFF && !g_load.on) return;
    if (g_load.on) LoadTick();              // may put the body on (changes g_state)
    if (g_state == S_OFF) return;
    if (g_state == S_SPIN_IN) {
        if (SWSE_LevelUp()) HoldDeps(&g_spin.deps);
        SpinTick();
        return;
    }
    // Hold first, before anything below can return: a frame without the bump
    // lets the next scheduler tick release the blocks.
    if (SWSE_LevelUp()) HoldDeps(&g_s.deps);

    unsigned p = 0, m = 0;
    int body = SWSE_PlayerBody(&p, &m);
    if (body != 1) {
        // A pointer caught mid-update is one frame; a load is seconds. Only
        // a real absence means the objects are gone.
        DWORD now = GetTickCount();
        if (!g_missSince) g_missSince = now ? now : 1;
        if (now - g_missSince > 1500) Drop("player gone (level change, load or death)");
        return;
    }
    g_missSince = 0;
    if (p != g_s.player || !VtIs(p, RVA_VT_PLAYERIMPL)) {
        Drop("a different player object (level change or reload)");
        return;
    }
    unsigned md = ModeData(p, g_s.form);
    if (R32(md + MD_MOTION) != g_s.motion || !VtIs(g_s.motion, RVA_VT_MOTIONIMPL)) {
        Drop("the form's motion object was replaced", p);
        return;
    }
    char why[160] = "", name[48];
    lstrcpynA(name, g_s.name[0] ? g_s.name : "the character", sizeof(name));
    // The engine put another body on (a scripted Stranger/Steef change, an
    // armor change) or rebuilt the motion's layer: undo our part cleanly.
    if (R32(p + PL_GEOM) != g_s.inst || R32(g_s.motion + MI_LAYER) != g_s.layer) {
        Log("engine changed the body/layer under us (geom %08X layer %08X) - reverting",
            R32(p + PL_GEOM), R32(g_s.motion + MI_LAYER));
        Revert(true, why, sizeof(why));
        Printf("playnpc: the game changed your body - no longer %s", name);
        return;
    }
    // The fallback: the hold should keep every block in, but if the streamer
    // unloads one anyway the body stops drawing and its animation data goes -
    // put Stranger back before anything reads it again.
    int miss = -1;
    if (!VtIs(g_s.def, RVA_VT_GHD) || DefFlags(g_s.def) != 1 || !DepsResident(&g_s.deps, &miss)) {
        char f[24] = "its body";
        unsigned b = miss >= 0 ? g_s.deps.block[miss] : BlockOf(g_s.def);
        if (miss >= 0) lstrcpynA(f, g_s.deps.file[miss], sizeof(f));
        else if (b) BlockFile(b, f, sizeof(f));
        int type = -1, depth = 0, pos = b ? QueuePos(b, &type, &depth) : -1;
        Log("%s: %s lost residency (block residency %u, resource flags %d, lazy %d, queue %d/%d type %d, def flags %d) - reverting",
            name, f, b ? R32(b + BLK_RESIDENCY) : 0u,
            miss >= 0 ? (int)(R32(g_s.deps.rep[miss] + RES_BLOCK) & 3) : DefFlags(g_s.def),
            b ? (int)R32(b + BLK_LAZYNEED) : 0, pos, depth, type, DefFlags(g_s.def));
        Revert(true, why, sizeof(why));
        Printf("playnpc: the streamer unloaded %s's data (%s) anyway - back to normal", name, f);
        return;
    }
    // No double jump: StartJump sets this after every first jump; RequestJump
    // double-jumps only while it is set (0x53AA95).
    if (R32(p + PL_MOTION) == g_s.motion) WrB(g_s.motion + MI_JUMPCANDOUBLE, 0);
    VitalsTick(p);
}

// ---------------------------------------------------------------------------
//  commands
// ---------------------------------------------------------------------------

// ready = everything it needs is in memory; load = playnpc streams it in
// first; far = part of it is in a level block that only comes with its area;
// no body = its geometry is not in this level at all.
static const char* TypeKind(const NpcType& t, Deps* d) {
    int r = ComputeDeps(t, d);
    if (r == DEP_NOBODY) return "nobody";
    if (r == DEP_FAR) return "far";
    bool in = DefFlags(t.def) == 1 && DepsResident(d, nullptr);
    if (!in && d->n == 0) return "far";    // not in memory, and nothing we can ask for
    return in ? "ready" : "load";
}

static void ListTypes() {
    int n = ScanTypes();
    if (n <= 0) {
        SWSE_ConsolePrint("playnpc: no characters found - load a level (continue / warp) first.");
        return;
    }
    const char* kind[MAX_TYPES];
    int ready = 0, load = 0;
    for (int i = 0; i < n; i++) {
        Deps d;
        kind[i] = TypeKind(g_types[i], &d);
        if (kind[i][0] == 'r') ready++;
        else if (kind[i][0] == 'l') load++;
    }
    Printf("playnpc: %d character type(s) in this level - %d in memory now, %d loadable on demand:", n, ready, load);
    for (int i = 0; i < n; i++) {
        const NpcType& t = g_types[i];
        char sc[24] = "";
        if (t.authored > SCALE_MAX_SAFE) _snprintf_s(sc, sizeof(sc), _TRUNCATE, " %.1fx->%.1fx", t.authored, t.scale);
        Printf("  %-26s %-6s %-18s%s", t.name, kind[i], t.stem[0] ? t.stem : "-", sc);
    }
    SWSE_ConsolePrint("  playnpc <name> [nospin] [bare] [scale <x>] | off [nospin] | status | states");
    SWSE_ConsolePrint("  QA: playnpc anims | runtest [ms] [sprint] [buck] | runtest stop | hurt <n> [stamina] | textures [name]  (log: swse_log.txt)");
    SWSE_ConsolePrint("  ready = in memory now; load = streamed in first (a moment); far = only near its own area.");
}

static void PrintDeps(const Deps* d, bool loading) {
    for (int i = 0; i < d->n; i++) {
        unsigned b = d->block[i];
        int type = -1, depth = 0, pos = QueuePos(b, &type, &depth);
        char q[40] = "";
        if (pos >= 0) _snprintf_s(q, sizeof(q), _TRUNCATE, "  queued %d/%d (%s)", pos, depth, type == 1 ? "release" : "load");
        char at[32] = "";
        if (loading && d->inAt[i]) _snprintf_s(at, sizeof(at), _TRUNCATE, "  in at %lu ms", d->inAt[i]);
        Printf("    %-14s residency %u  data %s  need %d%s%s", d->file[i], R32(b + BLK_RESIDENCY),
               ResResident(d->rep[i]) ? "in " : "out", (int)R32(b + BLK_LAZYNEED), q, at);
    }
    if (d->overflow) Printf("    (+%d more block(s) not held - over the limit of %d)", d->overflow, MAX_DEPS);
}

static void Status() {
    if (g_load.on) {
        Printf("playnpc: loading %s for %lu ms%s:", g_load.name, GetTickCount() - g_load.started,
               g_load.loadedAt ? " - loaded, waiting for the player" : "");
        PrintDeps(&g_load.deps, true);
    }
    if (g_state == S_OFF) {
        if (g_load.on) return;
        PlayerView v; char why[128];
        if (!ViewPlayer(&v, why, sizeof(why))) { Printf("playnpc: off (%s)", why); return; }
        Printf("playnpc: off. player %08X form %s motion %08X body %08X layer %08X camlock %u",
               v.player, v.form ? "Stranger" : "Steef", v.motion, v.geo, v.layer, R32(v.player + PL_CAMLOCK));
        return;
    }
    if (g_state == S_SPIN_IN) {
        Printf("playnpc: spinning into %08X (%lu ms, midpoint %ld, hook %s)", g_spin.typeHash,
               GetTickCount() - g_spin.started, g_spin.midpoint, g_hookArmed ? "armed" : "off");
        return;
    }
    Printf("playnpc: you are %s (%08X) at %.2fx, form %s", g_s.name, g_s.typeHash, g_s.scale,
           g_s.form ? "Stranger" : "Steef");
    Printf("  body %08X (Stranger's %08X)  layer %08X  config copy %08X", g_s.inst, g_s.oldGeo,
           g_s.layer, g_s.copy ? (unsigned)(uintptr_t)g_s.copy->obj : 0);
    Printf("  geometry flags %d (1 = drawn)  camlock %u  disguise lock %u  IK %u -> %u",
           DefFlags(g_s.def), R32(g_s.player + PL_CAMLOCK), R32(g_s.player + PL_DISGUISELOCK) & 0xFF,
           g_s.ikSaved, R32(ModeData(g_s.player, g_s.form) + MD_IKCOUNT));
    float h[3], s[3];
    if (ReadTri(g_s.player, PL_HEALTH, h) && ReadTri(g_s.player, PL_STAMINA, s))
        Printf("  health %.0f/%.0f, stamina %.0f/%.0f%s", h[0], h[1], s[0], s[1],
               g_v.have ? "" : " (Stranger's - not captured)");
    if (g_v.have)
        Printf("  Stranger's own, for the way back: health max %.0f, stamina max %.0f (at the fraction you leave with)",
               g_v.h[1], g_v.s[1]);
    Printf("  holding %d block(s) in memory (%d animations):", g_s.deps.n, g_s.deps.anims);
    PrintDeps(&g_s.deps, false);
}

static void States(int limit) {
    if (g_state != S_ACTIVE || !g_s.copy) { SWSE_ConsolePrint("playnpc states: not playing an NPC"); return; }
    CfgCopy* c = g_s.copy;
    Printf("playnpc states: %s authors %d of %d states; %d filled from its own clips:",
           g_s.name, c->authored, NSTATES, c->filled);
    int shown = 0;
    for (int s = 0; s < NSTATES && shown < limit; s++) {
        if (c->from[s] < 0) continue;
        Printf("  %-30s <- %s", StateName(s), StateName(c->from[s]));
        shown++;
    }
    int missing = 0;
    for (int s = 0; s < NSTATES; s++) if (!c->lookup[s]) missing++;
    Printf("  %d states have no clip at all (the action still happens; the engine", missing);
    SWSE_ConsolePrint("  falls back to the character's idle for those).");
}

// Everything the character needs is in memory: put it on (spin or instant).
static void StartBecome(int ti, const Deps* d, bool spin, bool attach, float scaleOverride, DWORD loadedMs) {
    char why[200] = "";
    const NpcType t = g_types[ti];
    if (g_state == S_ACTIVE) {
        // NPC to NPC: straight swap, no spin (an NPC has no transformation clip).
        // The health/stamina fractions carry over to the next character.
        g_v.switching = true;
        bool left = Revert(true, why, sizeof(why));
        g_v.switching = false;
        if (!left) {
            g_v.carryH = g_v.carryS = -1.0f;
            Printf("playnpc: could not leave %s first - %s", g_current, why);
            return;
        }
        spin = false;
    }
    if (t.authored > SCALE_MAX_SAFE && scaleOverride <= 0.0f)
        Printf("playnpc: %s is authored at %.1fx; drawn at %.1fx - above 2x the PC port's bone packing wraps (the pancake).",
               t.name, t.authored, t.scale);
    if (spin) {
        PlayerView v;
        if (ViewPlayer(&v, why, sizeof(why)) && R32(v.motion + MI_PHYSICS) == 0 &&
            R32(v.motion + MI_MOTCURRENT) != MOT_CHANGEENTER && R32(v.motion + MI_MOTCURRENT) != MOT_CHANGELEAVE &&
            ArmSpinHook()) {
            memset(&g_spin, 0, sizeof(g_spin));
            g_spin.player = v.player; g_spin.motion = v.motion;
            g_spin.typeHash = t.hash; g_spin.scaleOverride = scaleOverride; g_spin.attach = attach;
            g_spin.started = GetTickCount();
            g_spin.loadedMs = loadedMs;
            g_spin.deps = *d;
            g_state = S_SPIN_IN;
            int r = MotionRequest(v.motion, MIV_REQCHANGE, 0);
            if (r == 1) {
                Printf("playnpc: transforming into %s...", t.name);
                return;
            }
            g_state = S_OFF;
            DisarmSpinHook();
            Log("RequestChange refused (%d) - swapping without the spin", r);
        }
    }
    char loaded[40] = "";
    if (loadedMs) _snprintf_s(loaded, sizeof(loaded), _TRUNCATE, "loaded in %lu ms, ", loadedMs);
    if (BecomeNow(ti, scaleOverride, attach, d, why, sizeof(why)))
        Printf("playnpc: you are %s (%s%.2fx, %d gaps filled from its own clips)", t.name, loaded,
               g_s.scale, g_s.copy ? g_s.copy->filled : 0);
    else
        Printf("playnpc: cannot become %s - %s", t.name, why);
}

static void Become(const char* who, bool spin, bool attach, float scaleOverride) {
    if (g_state == S_SPIN_IN) { SWSE_ConsolePrint("playnpc: a spin is already in progress"); return; }
    int ti = FindType(who);
    if (ti == -2) { Printf("playnpc: '%s' matches more than one character - be more specific", who); return; }
    if (ti < 0) {
        Printf("playnpc: no character called '%s' in this level.", who);
        SWSE_ConsolePrint("  'playnpc' lists what this level has.");
        return;
    }
    const NpcType t = g_types[ti];
    if (!t.def) {
        Printf("playnpc: %s has no body in this level's archives (its geometry is not registered here), so it cannot be loaded.",
               t.name);
        return;
    }
    if (!t.cfg || !t.cfgPath) { Printf("playnpc: %s has no animation config in this level.", t.name); return; }
    Deps d;
    if (ComputeDeps(t, &d) == DEP_FAR) {
        Printf("playnpc: part of %s is in %s, which is not a character bundle - it only streams in with its own area; go there first.",
               t.name, d.farFile);
        return;
    }
    // One pending load at a time: a new request replaces it.
    if (g_load.on) {
        if (g_load.typeHash == t.hash && !(DefFlags(t.def) == 1 && DepsResident(&d, nullptr))) {
            Printf("playnpc: still loading %s (%lu ms so far)", t.name, GetTickCount() - g_load.started);
            return;
        }
        CancelLoad(g_load.typeHash == t.hash ? "it is in" : "another character was asked for", g_load.typeHash != t.hash);
    }
    if (DefFlags(t.def) == 1 && DepsResident(&d, nullptr)) {
        StartBecome(ti, &d, spin, attach, scaleOverride, 0);
        return;
    }
    if (d.n == 0) {
        Printf("playnpc: %s's body is not in memory and is not in a character bundle playnpc can load - it only streams in with its own area.",
               t.name);
        return;
    }
    // Not in memory: stream it in first. The hold IS the request - the next
    // frame's scheduler tick asks the block manager for every held block.
    memset(&g_load, 0, sizeof(g_load));
    g_load.on = true;
    g_load.typeHash = t.hash;
    g_load.def = t.def;
    g_load.epoch = SWSE_LevelEpoch();
    lstrcpynA(g_load.name, t.name, sizeof(g_load.name));
    g_load.deps = d;
    g_load.started = GetTickCount();
    g_load.spin = spin; g_load.attach = attach; g_load.scaleOverride = scaleOverride;
    for (int i = 0; i < d.n; i++) if (DepIn(&d, i)) g_load.deps.inAt[i] = 1;
    HoldDeps(&g_load.deps);
    char all[200], missing[200];
    DepsFiles(&d, all, sizeof(all), false);
    DepsFiles(&d, missing, sizeof(missing), true);
    Log("load %s (%08X): needs %s; not in memory: %s", t.name, t.hash, all[0] ? all : "-",
        missing[0] ? missing : "its body");
    Printf("playnpc: loading %s (%s)...%s", t.name, missing[0] ? missing : "its body",
           g_state == S_ACTIVE ? " - you stay as you are until it is in" : "");
}

static void Off(bool spin) {
    bool wasLoading = g_load.on;
    if (g_load.on) CancelLoad("playnpc off", true);
    if (g_state == S_SPIN_IN) {
        // Not disarmed here: ChangeEnter is still playing and its end would
        // swap the form. The midpoint is swallowed and nothing is put on.
        g_spin.cancel = true;
        SWSE_ConsolePrint("playnpc: cancelling - the transformation finishes as yourself");
        return;
    }
    if (g_state != S_ACTIVE) { if (!wasLoading) SWSE_ConsolePrint("playnpc: already yourself"); return; }
    unsigned m = g_s.motion;
    char why[200] = "";
    if (!Revert(false, why, sizeof(why))) {
        if (why[0]) Printf("playnpc: %s", why);
        return;
    }
    // The second half of the engine's transformation on the body that is back:
    // ChangeLeave plays ANIM_DISGUISECHANGE_LEAVE and leaves no form swap.
    if (spin && VtIs(m, RVA_VT_MOTIONIMPL)) {
        int r = MotionRequest(m, MIV_REQCHANGE2, 0);
        if (r != 1) Log("RequestChange2 refused (%d)", r);
    }
}

void SWSE_PlayNpcCmd(int argc, char** argv) {
    if (argc < 2 || !lstrcmpiA(argv[1], "list")) { ListTypes(); return; }
    if (!lstrcmpiA(argv[1], "status")) { Status(); return; }
    if (!lstrcmpiA(argv[1], "states")) { States(argc > 2 ? atoi(argv[2]) : 60); return; }
    if (!lstrcmpiA(argv[1], "anims")) {
        if (!SWSE_GameBuildKnown()) { SWSE_ConsolePrint("playnpc anims: unknown game build"); return; }
        DumpAnims("now", 0.0f);
        SWSE_ConsolePrint("playnpc anims: one sample written to swse_log.txt");
        return;
    }
    // QA: take n off the current health (or stamina) only - the max stays -
    // to check the fraction rule without a fight. Never below 1.
    if (!lstrcmpiA(argv[1], "hurt")) {
        if (!SWSE_GameBuildKnown()) { SWSE_ConsolePrint("playnpc hurt: unknown game build"); return; }
        float n = argc > 2 ? (float)atof(argv[2]) : 0.0f;
        bool stam = argc > 3 && !lstrcmpiA(argv[3], "stamina");
        PlayerView v; char w[128];
        float t[3];
        int off = stam ? PL_STAMINA : PL_HEALTH;
        if (!(n > 0.0f) || !ViewPlayer(&v, w, sizeof(w)) || !ReadTri(v.player, off, t)) {
            SWSE_ConsolePrint("playnpc hurt <n> [stamina] - take n off the current value (QA)");
            return;
        }
        float c = t[0] - n;
        if (c < 1.0f) c = 1.0f;
        WriteTri(v.player, off, c, t[1], t[2]);
        Printf("playnpc hurt: %s %.0f -> %.0f of %.0f", stam ? "stamina" : "health", t[0], c, t[1]);
        return;
    }
    // QA: each material of a body (and its attachments), where its texture
    // lives, whether that is in, and what the material has cached.
    if (!lstrcmpiA(argv[1], "textures")) {
        if (!SWSE_GameBuildKnown()) { SWSE_ConsolePrint("playnpc textures: unknown game build"); return; }
        int ti = -1;
        if (argc > 2) {
            ti = FindType(argv[2]);
            if (ti < 0) { Printf("playnpc textures: no single character '%s' in this level", argv[2]); return; }
        } else if (g_state == S_ACTIVE) {
            char hx[12];
            _snprintf_s(hx, sizeof(hx), _TRUNCATE, "%08X", g_s.typeHash);
            ti = FindType(hx);
        }
        if (ti < 0) { SWSE_ConsolePrint("playnpc textures [name] - its materials and where their textures are (QA; log too)"); return; }
        const NpcType t = g_types[ti];
        if (!t.def) { Printf("playnpc textures: %s has no body in this level", t.name); return; }
        Printf("playnpc textures: %s (the white fallback is %08X)", t.name, R32(Base() + RVA_WHITE_TEX));
        DescribeTextures(t.def, t.name);
        const unsigned lists[2] = { t.prefs + NP_ATTACH, t.prefs + NP_DEFATTACH };
        for (int l = 0; l < 2; l++) {
            unsigned adata = R32(lists[l]), an = R32(lists[l] + 8);
            if (!IsHeap(adata) || an > 32) continue;
            for (unsigned i = 0; i < an; i++) {
                unsigned tok = R32(adata + i * ATT_STRIDE + ATT_GEOTOKEN);
                unsigned g = (tok && tok != HASH_NULL) ? SWSE_ResourceLookup(tok) : 0;
                char lab[40];
                _snprintf_s(lab, sizeof(lab), _TRUNCATE, "attachment %08X", tok);
                if (IsGeoDef(g)) DescribeTextures(g, lab);
            }
        }
        Deps d;
        ComputeDeps(t, &d);
        char files[200];
        DepsFiles(&d, files, sizeof(files), false);
        Printf("  needs %d bundle(s): %s", d.n, files[0] ? files : "-");
        Printf("  its materials reference %d resource(s): %d more bundle(s) for them, %d not registered here, %d in a level bundle not in%s%s%s%s",
               d.refs, d.refBlocks, d.refUnreg, d.refOut, d.refOut ? " (" : "", d.refOut ? d.refOutFile : "",
               d.refOut ? ")" : "", d.refsPending ? " - a body is not in, so not all were read" : "");
        return;
    }
    if (!lstrcmpiA(argv[1], "runtest")) {
        if (argc > 2 && !lstrcmpiA(argv[2], "stop")) {
            bool was = g_rt.on;
            RunTestStop("stopped");
            SWSE_ConsolePrint(was ? "playnpc: runtest stopped (W is released when its hold ends)"
                                  : "playnpc: no runtest running");
            return;
        }
        if (!SWSE_GameBuildKnown()) { SWSE_ConsolePrint("playnpc runtest: unknown game build"); return; }
        int ms = 4000; bool sprint = false, buck = false;
        for (int i = 2; i < argc; i++) {
            if (!lstrcmpiA(argv[i], "sprint")) sprint = true;
            else if (!lstrcmpiA(argv[i], "buck")) buck = true;
            else if (atoi(argv[i]) > 0) ms = atoi(argv[i]);
            else { Printf("playnpc: runtest [ms] [sprint] [buck] | runtest stop - unknown '%s'", argv[i]); return; }
        }
        RunTest(ms, sprint, buck);
        return;
    }
    bool spin = true, attach = true;
    float scale = 0.0f;
    for (int i = 2; i < argc; i++) {
        if (!lstrcmpiA(argv[i], "nospin")) spin = false;
        else if (!lstrcmpiA(argv[i], "spin")) spin = true;
        else if (!lstrcmpiA(argv[i], "bare")) attach = false;
        else if (!lstrcmpiA(argv[i], "scale") && i + 1 < argc) scale = (float)atof(argv[++i]);
    }
    if (!lstrcmpiA(argv[1], "off")) { RunTestStop("stopped by playnpc off"); Off(spin); return; }
    // Becoming writes into game objects, a vtable slot and the streamer's
    // counters at addresses measured on the Steam build.
    if (!SWSE_GameBuildKnown()) {
        char m[300];
        SWSE_GameBuildRefusal("playnpc", m, sizeof(m));
        Printf("playnpc: %s", m);
        return;
    }
    Become(argv[1], spin, attach, scale);
}
