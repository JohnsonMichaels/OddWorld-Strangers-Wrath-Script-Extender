// SWSE - freecam: detach the view and fly the game's own FlyCamera.
// See freecam.h; the design and the evidence for every address are in
// swse/research/FREECAM.md (PROVEN / SUSPECTED tags there).
//
// What the engine gives us (all static RE on the Steam HD exe, beta PDB for
// the names):
//  - FlyCamera is whole in HD: constructor, Init, GetInput (schemes 0 and 1,
//    reading the logical input the player reads), UpdateView.
//  - The cheat-camera slot is alive: CameraContext+0x18. UpdateCameras
//    (0x40CD90) updates a camera there every frame and stops updating the
//    normal ones; UpdateRenderContext (0x40DAC0) renders from it.
//  - What HD lost is the code that PUT a camera there (the beta's
//    HandleInput_CheatCams on debug buttons) and the player-input skip that
//    went with it. freecam does what CreateCheatCamera + SetCheatCam did, and
//    takes the tick's existing skip branch while it is on.

#include "freecam.h"
#include "console.h"
#include "scriptvm.h"
#include "levelwatch.h"
#include "gamebuild.h"
#include "input.h"
#include "hookreg.h"     // the gate byte is listed by `hooks` while it is flipped
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

// ---------------------------------------------------------------------------
//  addresses (RVAs; the exe is ASLR'd, VA = image base + RVA)
// ---------------------------------------------------------------------------

// globals
#define RVA_CAMCTX          0x6362A0    // CameraMgr::s_state.m_context - set only inside the engine's
                                        // context guard; null between frames (FREECAM.md 3.1)
#define RVA_CAMCTX_DATA     0x636294    // CameraContext** - every context
#define RVA_CAMCTX_COUNT    0x63629C    // and how many
#define RVA_HANDLE_PTR      0x5D55F0    // handle table {object*, u16 generation}, 6 bytes each
#define RVA_HANDLE_GEN      0x5D55F4
#define RVA_CINEMATIC       0x5D5377    // byte: an in-engine cutscene runs (vm:IsCinematicRunning)
#define RVA_FRAME_DT        0x5D5554    // float: the frame delta FlyCamera::UpdateView integrates with
                                        // (0x41611E; MOVEMENT_BUG.md: the delta's copy)
#define RVA_MOUSE_MODE      0x3FE260    // int: 1 while the game runs on keyboard and mouse - the look
                                        // stick then carries one frame's mouse motion, and the player's
                                        // cameras turn by it directly (set by the WndProc, 0x5F8208/21)
#define RVA_MOUSE_SENS      0x3FE264    // float: the game's mouse factor, GamePrefs+0xEC + 0.1, set every
                                        // frame by the main loop (0x5F843F); raw counts x 0.002 x this
                                        // is what its WndProc (0x5F8018) adds up for the camera

// functions
#define RVA_CAM_CREATE      0x00AF90    // Camera::Create_Internal, cdecl (SPtr* out, type, generator, subject, context)
#define RVA_CAM_TELEPORT    0x00BDF0    // Camera::TeleportCam, stdcall (cam, const Frame3*, zone, portal side)
#define RVA_SPTR_RELEASE    0x1A0BF0    // SPtr<T> release: ESI = SPtr*; dec ref, at 0 bump generation + delete
#define RVA_FLY_INIT        0x015F90    // FlyCamera::Init(const Viewer&)    - vtable slot 7
#define RVA_CAM_SETFRAME    0x00B710    // Camera::SetFrame(const Frame3&)   - vtable slot 17
#define RVA_MAT_PITCHYAW    0x015ED0    // SetMatrixFromPitchAndYaw, cdecl (float out[9], pitch, yaw)
#define RVA_INPUT_GET       0x25BA10    // InputState singleton getter
#define RVA_INPUT_2D        0x25C6C0    // InputState: const Vec2* (channel, control); channel -1 = all

#define RVA_VT_FLYCAMERA    0x366064
#define VTS_INIT            (7 * 4)
#define VTS_SETFRAME        (17 * 4)

// The player-input branch in PlayerImpl's tick, right after
// HandleInput_Cinematic (0x455BB0) reports whether to skip input:
//   0044FA70  cmp byte ptr [esp+0x16], 0
//   0044FA75  jne 0x44FADF   ; skip HandleInput_Camera, HandleInput, Weapon::HandleInput
// freecam makes the jne a jmp (0x75 -> 0xEB) while it is on.
#define RVA_GATE_SITE       0x04FA70
#define RVA_GATE_JCC        0x04FA75
static const BYTE kGateSite[7] = { 0x80, 0x7C, 0x24, 0x16, 0x00, 0x75, 0x68 };
#define GATE_OFF            0x75        // jne
#define GATE_ON             0xEB        // jmp

// object header (RefCounted / CoreObject)
#define OBJ_REF             0x04        // u16 reference count
#define OBJ_HANDLE          0x06        // u16 handle index

// Camera (HD = beta - 0x10)
#define CAM_SUBJ_IDX        0x10        // m_subject weak handle: u16 index
#define CAM_SUBJ_GEN        0x12        //                        u16 generation
#define CAM_1PFOV           0x14        // m_1PFov, radians (UpdateFovRad derives the viewer FOV from it)
#define CAM_TYPE            0x18        // m_type (Camera::EType)
#define CAM_CONTEXT         0x1C        // m_context
#define CAM_TRANSITION      0x20        // m_transitionState (4 = eFinished)
#define CAM_ZONE            0x50        // m_zoneCode
#define CAM_SIDE            0x54        // m_lastValidatedPortalSide
#define CAM_VIEWER          0x64        // m_viewer (0x70 bytes)
#define CAM_FRAME           0x70        // the viewer's frame: 3x3 rotation, then the position
#define CAM_POS             0x94        // position (frame + 0x24) - what GetPosition returns

// FlyCamera (HD = beta - 0x10)
#define FC_VELOCITY         0xD8        // Vec3
#define FC_MOVESCALE        0xE4        // m_movementScale: full stick = 200 x this, units/s
#define FC_PITCH            0xEC        // m_pitch, radians, positive looks DOWN
#define FC_YAW              0xF0        // m_yaw, radians
#define FC_FIRSTUPDATE      0xF8        // m_firstUpdate
#define FC_MOVECTRL         0x100       // m_moveControl, m_moveControlVel (Vec3 each),
#define FC_PITCHCTRL        0x118       // then m_pitchControl, m_pitchVel, m_yawControl, m_yawVel
#define FC_CTRL_END         0x128       // (the end of those)

// CameraContext (HD = beta - 8)
#define CTX_CURRENT         0x0C        // m_currentCam
#define CTX_CHEAT           0x18        // m_cheatCam
#define CTX_ENABLED         0x58        // m_enabled

#define CAMTYPE_FLY         (-1)        // Camera::eFlyCamera
#define TRANSITION_FINISHED 4           // what CameraContext::SetCheatCam gives a cheat camera
#define SPEED_UNITS         200.0f      // UpdateView: velocity = m_moveControl * m_movementScale * 200
#define DEFAULT_SPEED       40.0f       // the constructor's m_movementScale 0.2
#define STICK_IDLE2         0.0625f     // |stick|^2 below this counts as released (|stick| < 0.25)
#define MAX_CONTEXTS        16

// Turning (section 5.1 of FREECAM.md): the mouse, and the arrow keys as a
// fallback, turn the camera directly - the camera's own look-stick turning
// still runs and adds to it (a gamepad's right stick).
#define DEFAULT_SENS        8.0f        // degrees per 100 mouse counts, before the game's own factor
#define KEY_YAW_DPS         90.0f       // arrow keys: degrees per second
#define KEY_PITCH_DPS       60.0f
#define PITCH_LIMIT_DEG     89.0f

#define PI_D                3.14159265358979323846
#define DEG                 (180.0 / PI_D)

typedef void* (__cdecl*    CamCreate_t)(unsigned* out, int type, void* generator, void* subject, int context);
typedef void  (__stdcall*  CamTeleport_t)(void* cam, const void* frame, int zone, int side);
typedef void  (__thiscall* FlyInit_t)(void* self, const void* viewer);
typedef void  (__thiscall* SetFrame_t)(void* self, const void* frame);
typedef void  (__cdecl*    MatPitchYaw_t)(float* out9, float pitch, float yaw);
typedef void* (__cdecl*    InputGet_t)();
typedef const float* (__thiscall* Input2D_t)(void* self, int channel, int control);

// ---------------------------------------------------------------------------
//  small helpers
// ---------------------------------------------------------------------------

static unsigned Base() { return (unsigned)(uintptr_t)GetModuleHandleA(NULL); }

#define ADDR_LO 0x10000u
#define ADDR_HI 0xFFFE0000u
static bool IsHeap(unsigned p) { return p >= ADDR_LO && p < ADDR_HI && !(p & 3); }

static bool Rd(unsigned addr, unsigned* out) {
    if (addr < ADDR_LO || addr >= ADDR_HI) return false;
    __try { *out = *(unsigned*)addr; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static unsigned R32(unsigned addr) { unsigned v = 0; Rd(addr, &v); return v; }
static bool Rd16(unsigned addr, unsigned short* out) {
    if (addr < ADDR_LO || addr >= ADDR_HI) return false;
    __try { *out = *(unsigned short*)addr; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
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
static bool WrF(unsigned addr, float f) { return Wr(addr, *(unsigned*)&f); }

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
    fprintf(f, "freecam: %s\n", buf);
    fclose(f);
}

static bool Finite(float v) { return v > -1.0e6f && v < 1.0e6f; }

// ---------------------------------------------------------------------------
//  state
// ---------------------------------------------------------------------------

static bool           g_on = false;
static unsigned       g_cam = 0;         // the FlyCamera; the context's slot holds its one reference
static unsigned short g_camIdx = 0;      // its handle: alive while the table still maps idx -> g_cam
static unsigned short g_camGen = 0;      //   with this generation (the engine bumps it on delete)
static unsigned       g_player = 0;
static unsigned       g_epoch = 0;
static bool           g_gate = false;    // our jmp is in
static bool           g_waitSaid = false;
static float          g_speed = 0.0f;    // units/s asked for; 0 = the camera's own default
static DWORD          g_since = 0;
static float          g_sensX = DEFAULT_SENS;   // degrees per 100 counts, sideways
static float          g_sensY = DEFAULT_SENS;   //   and up/down (negative inverts)
static double         g_mouseCountsX = 0.0, g_mouseCountsY = 0.0;  // this session, for the log
static double         g_keyTurnDeg = 0.0;

int SWSE_FreecamActive() { return g_on ? 1 : 0; }

// The engine's weak-pointer test (Camera::GetSubject, 0x467640): the handle
// table still maps the index to this object, with the same generation.
static bool HandleAlive(unsigned obj, unsigned short idx, unsigned short gen) {
    unsigned b = Base(), p = 0;
    unsigned short g = 0;
    if (!Rd(b + RVA_HANDLE_PTR + idx * 6u, &p)) return false;
    if (!Rd16(b + RVA_HANDLE_GEN + idx * 6u, &g)) return false;
    return p == obj && g == gen;
}
static bool CamAlive() { return g_cam && HandleAlive(g_cam, g_camIdx, g_camGen); }

// A camera's subject, resolved the way Camera::GetSubject does. 0 if none.
static unsigned SubjectOf(unsigned cam) {
    unsigned short idx = 0, gen = 0, tg = 0;
    if (!Rd16(cam + CAM_SUBJ_IDX, &idx) || !Rd16(cam + CAM_SUBJ_GEN, &gen)) return 0;
    if (!Rd16(Base() + RVA_HANDLE_GEN + idx * 6u, &tg) || tg != gen) return 0;
    unsigned p = R32(Base() + RVA_HANDLE_PTR + idx * 6u);
    return IsHeap(p) ? p : 0;
}

// The address of the context slot that holds `cam` as its cheat camera: the
// current context first, then every context. 0 if none does. Only live engine
// state is read (the globals, never a context we remembered).
static unsigned CheatSlotOf(unsigned cam) {
    if (!cam) return 0;
    unsigned b = Base();
    unsigned ctx = R32(b + RVA_CAMCTX);
    if (IsHeap(ctx) && R32(ctx + CTX_CHEAT) == cam) return ctx + CTX_CHEAT;
    unsigned data = R32(b + RVA_CAMCTX_DATA);
    int n = (int)R32(b + RVA_CAMCTX_COUNT);
    if (!IsHeap(data) || n <= 0 || n > MAX_CONTEXTS) return 0;
    for (int i = 0; i < n; i++) {
        unsigned c = R32(data + i * 4u);
        if (IsHeap(c) && R32(c + CTX_CHEAT) == cam) return c + CTX_CHEAT;
    }
    return 0;
}

// Drop one reference exactly as the engine's SPtr does (0x5A0BF0, ESI = the
// SPtr): at zero it bumps the handle generation and deletes the object.
static void EngineRelease(unsigned* holder) {
    unsigned fn = Base() + RVA_SPTR_RELEASE;
    __asm {
        mov  esi, holder
        call fn
    }
}
static bool EngineReleaseGuarded(unsigned* sp) {
    __try { EngineRelease(sp); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool Cinematic() {
    BYTE c = 0;
    return RdB(Base() + RVA_CINEMATIC, &c) && c != 0;
}

// The move stick, on every input channel (eStick_Left 0x1C, which the player
// reads in third person, and eStick_LeftHaloDZ 0x1E, first person and the fly
// camera). Released = both near zero.
static bool SticksIdle() {
    unsigned b = Base();
    __try {
        void* is = ((InputGet_t)(b + RVA_INPUT_GET))();
        if (!is) return true;
        static const int kIds[2] = { 0x1C, 0x1E };
        for (int i = 0; i < 2; i++) {
            const float* v = ((Input2D_t)(b + RVA_INPUT_2D))(is, -1, kIds[i]);
            if (v && v[0] * v[0] + v[1] * v[1] > STICK_IDLE2) return false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
    return true;
}

// ---------------------------------------------------------------------------
//  the player-input gate: one byte, only while freecam is on
// ---------------------------------------------------------------------------

static bool GateSiteOk() {
    unsigned a = Base() + RVA_GATE_SITE;
    for (int i = 0; i < 7; i++) {
        BYTE v = 0;
        if (!RdB(a + i, &v)) return false;
        if (i == 5) { if (v != GATE_OFF && v != GATE_ON) return false; continue; }
        if (v != kGateSite[i]) return false;
    }
    return true;
}

static bool GateSet(bool on) {
    unsigned a = Base() + RVA_GATE_JCC;
    BYTE cur = 0;
    if (!RdB(a, &cur)) return false;
    BYTE want = on ? GATE_ON : GATE_OFF, other = on ? GATE_OFF : GATE_ON;
    if (cur == want) { g_gate = on; return true; }
    if (cur != other) {
        Log("input gate at %08X reads %02X (expected %02X or %02X) - left alone", a, cur, GATE_OFF, GATE_ON);
        return false;
    }
    DWORD old = 0, tmp = 0;
    if (!VirtualProtect((void*)(uintptr_t)a, 1, PAGE_EXECUTE_READWRITE, &old)) {
        Log("input gate: VirtualProtect failed (%lu)", GetLastError());
        return false;
    }
    bool ok = true;
    __try { *(volatile BYTE*)(uintptr_t)a = want; }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    VirtualProtect((void*)(uintptr_t)a, 1, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), (void*)(uintptr_t)a, 1);
    if (!ok) return false;
    if (on) SWSE_HookNote((void*)(uintptr_t)a, 1, "freecam", SWSE_HOOK_BYTES, "player input gate (a jump flipped)");
    else    SWSE_HookForget((void*)(uintptr_t)a);
    g_gate = on;
    Log(on ? "Stranger's input cut (the tick skips HandleInput_Camera, HandleInput, Weapon::HandleInput)"
           : "Stranger's input restored");
    return true;
}

// ---------------------------------------------------------------------------
//  pose: the fly camera's own angles, applied through the engine's own math
// ---------------------------------------------------------------------------
//
// Angles on the console use SWSE's facing convention (`yaw`, `writepos`): a
// heading h looks along (sin h, -cos h), and pitch is degrees above the
// horizon. The camera's own angles (research/FREECAM.md, "Angles"): it looks
// along (sin y cos p, cos y cos p, -sin p), so y = 180 - h and p = -pitch.

static float HeadingFromYaw(float yawRad) {
    double h = 180.0 - (double)yawRad * DEG;
    h = fmod(h, 360.0);
    if (h < 0.0) h += 360.0;
    return (float)h;
}
static float YawFromHeading(float hDeg) { return (float)((180.0 - (double)hDeg) / DEG); }

// Zero every rate so a placed camera stays exactly where it was put.
static void StopMotion() {
    for (unsigned o = FC_VELOCITY; o < FC_VELOCITY + 12; o += 4) Wr(g_cam + o, 0);
    for (unsigned o = FC_MOVECTRL; o < FC_CTRL_END; o += 4) Wr(g_cam + o, 0);
    WrB(g_cam + FC_FIRSTUPDATE, 0);
}

// Put the camera at pos with the camera-space angles (radians), now: the
// rotation from SetMatrixFromPitchAndYaw and the frame through SetFrame,
// exactly as FlyCamera::UpdateView builds them - so it holds even while the
// camera is not ticking (a paused game).
static bool ApplyPose(const float pos[3], float pitch, float yaw) {
    unsigned b = Base();
    if (!CamAlive()) return false;
    unsigned vt = R32(g_cam);
    if (!IsHeap(vt) || R32(vt + VTS_SETFRAME) != b + RVA_CAM_SETFRAME) {
        Log("pose: SetFrame slot reads %08X, expected %08X - not applied", R32(vt + VTS_SETFRAME), b + RVA_CAM_SETFRAME);
        return false;
    }
    if (pitch > 1.5707963f) pitch = 1.5707963f;
    if (pitch < -1.5707963f) pitch = -1.5707963f;
    WrF(g_cam + FC_PITCH, pitch);
    WrF(g_cam + FC_YAW, yaw);
    StopMotion();
    float frame[12] = { 0 };
    __try {
        ((MatPitchYaw_t)(b + RVA_MAT_PITCHYAW))(frame, pitch, yaw);
        frame[9] = pos[0]; frame[10] = pos[1]; frame[11] = pos[2];
        ((SetFrame_t)(b + RVA_CAM_SETFRAME))((void*)(uintptr_t)g_cam, frame);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("pose: faulted applying the frame");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
//  turning by the mouse and the arrow keys
// ---------------------------------------------------------------------------
//
// The camera's own GetInput reads the look stick as a turning RATE, through a
// damped drive (up to 84 degrees a second sideways). On PC the mouse reaches
// that stick only as one frame's motion - the player's cameras turn by it
// directly in "mouse mode" (0x7FE260) - so the fly camera barely turned
// (owner, 2026-09-28: "it only moves but not my camera angle"). SWSE adds the
// relative mouse counts the game receives (input.cpp) and turns the camera by
// them here, writing the same two angles `freecam look` writes. Rates, speed
// and position are left alone, so flying and turning mix; the camera's own
// UpdateView builds the frame from the angles on the next tick.

// The game's own mouse factor (its sensitivity setting + 0.1), so freecam
// follows the owner's in-game sensitivity. Clamped against a bad read.
static float GameMouseFactor() {
    float f = RF(Base() + RVA_MOUSE_SENS);
    if (!(f >= 0.1f && f <= 10.0f)) f = 1.0f;
    return f;
}

// The game's frame delta, 0 while it is not ticking (SUSPECTED for the pause
// menu), so nothing turns behind a menu.
static float FrameDt() {
    float dt = RF(Base() + RVA_FRAME_DT);
    if (!(dt > 0.0f && dt < 0.25f)) return 0.0f;
    return dt;
}

// Turn by degrees: right is positive, up is positive.
static void TurnBy(double rightDeg, double upDeg) {
    float yaw = RF(g_cam + FC_YAW), pitch = RF(g_cam + FC_PITCH);
    double y = (double)yaw + rightDeg / DEG;            // the camera's yaw grows turning right
    double p = (double)pitch - upDeg / DEG;             // its pitch grows looking down
    const double lim = PITCH_LIMIT_DEG / DEG;
    if (p > lim) p = lim;
    if (p < -lim) p = -lim;
    y = fmod(y, 2.0 * PI_D);
    WrF(g_cam + FC_YAW, (float)y);
    WrF(g_cam + FC_PITCH, (float)p);
}

// Once per frame while on: the mouse since the last frame, and the arrow keys.
static void TurnFrame() {
    int dx = 0, dy = 0;
    SWSE_InputTakeMouseDelta(&dx, &dy);   // always taken, so nothing piles up
    // In mouse mode the camera's own look stick is the same mouse motion,
    // read as a rate: a small extra turn that carries on after the mouse stops.
    // Its damped turning state is cleared each frame, so the mouse turns the
    // camera exactly once. A gamepad (mouse mode off) keeps its right stick.
    unsigned mm = 0;
    if (Rd(Base() + RVA_MOUSE_MODE, &mm) && mm)
        for (unsigned o = FC_PITCHCTRL; o < FC_CTRL_END; o += 4) Wr(g_cam + o, 0);
    float dt = FrameDt();
    if (dt <= 0.0f) return;
    double right = 0.0, up = 0.0;
    if (dx || dy) {
        float gf = GameMouseFactor();
        right += (double)dx * g_sensX * gf / 100.0;
        up    -= (double)dy * g_sensY * gf / 100.0;   // raw +y is the mouse coming toward you
        g_mouseCountsX += dx;
        g_mouseCountsY += dy;
    }
    // The arrow keys, only while the owner is in the game and the console is shut.
    if (SWSE_InputReallyFocused() && !SWSE_ConsoleOpen()) {
        double k = 0.0;
        if (GetAsyncKeyState(VK_LEFT)  & 0x8000) { right -= KEY_YAW_DPS * dt;   k += KEY_YAW_DPS * dt; }
        if (GetAsyncKeyState(VK_RIGHT) & 0x8000) { right += KEY_YAW_DPS * dt;   k += KEY_YAW_DPS * dt; }
        if (GetAsyncKeyState(VK_UP)    & 0x8000) { up    += KEY_PITCH_DPS * dt; k += KEY_PITCH_DPS * dt; }
        if (GetAsyncKeyState(VK_DOWN)  & 0x8000) { up    -= KEY_PITCH_DPS * dt; k += KEY_PITCH_DPS * dt; }
        g_keyTurnDeg += k;
    }
    if (right != 0.0 || up != 0.0) TurnBy(right, up);
}

// ---------------------------------------------------------------------------
//  on / off
// ---------------------------------------------------------------------------

// reason: why it switched itself off, or nullptr when it was asked to.
static void Stop(const char* reason) {
    if (!g_on) return;
    SWSE_InputMouseCapture(false);
    Log("turning this session: mouse %.0f counts sideways, %.0f up/down; arrow keys %.1f degrees",
        g_mouseCountsX, g_mouseCountsY, g_keyTurnDeg);
    bool gateOk = GateSet(false);
    const char* how = "it was already gone";
    if (CamAlive()) {
        unsigned slot = CheatSlotOf(g_cam);
        if (slot) {
            // Empty the slot first, then drop the reference it held - the
            // same net effect as the beta's SetCheatCam(NULL).
            Wr(slot, 0);
            unsigned local = g_cam;
            how = EngineReleaseGuarded(&local) ? "released" : "released (faulted - left to the engine)";
        } else {
            how = "not in the camera slot any more - left to its holder";
        }
    }
    Log("off after %lu ms (%s); camera %08X %s; gate %s", GetTickCount() - g_since,
        reason ? reason : "asked", g_cam, how, gateOk ? "restored" : "NOT restored");
    if (reason) Printf("freecam: off - %s", reason);
    else        Printf("freecam: off");
    if (!gateOk) Printf("freecam: WARNING - could not put Stranger's input branch back (see swse_log.txt)");
    g_on = false;
    g_cam = 0;
    g_camIdx = g_camGen = 0;
    g_player = 0;
    g_waitSaid = false;
}

// The context the game renders with. CameraMgr::s_state.m_context is only set
// while the camera manager is working on it: MEASURED 2026-09-28 in
// lm_level_01, it read null in 156 of 200 samples between frames and the one
// live context in the rest - and SWSE's frame hook runs in the null part, so
// `freecam on` answered "no camera context". When it is null, the list of every
// context is used instead (a level has one), taking the first that is enabled.
static unsigned MainContext() {
    unsigned b = Base();
    unsigned ctx = R32(b + RVA_CAMCTX);
    if (IsHeap(ctx)) return ctx;
    unsigned n = R32(b + RVA_CAMCTX_COUNT), data = R32(b + RVA_CAMCTX_DATA);
    if (!IsHeap(data) || n == 0 || n > 16) return 0;
    for (unsigned i = 0; i < n; i++) {
        unsigned c = R32(data + 4 * i);
        BYTE en = 0;
        if (IsHeap(c) && RdB(c + CTX_ENABLED, &en) && en) return c;
    }
    return 0;
}

static bool Start() {
    unsigned b = Base();
    if (!SWSE_LevelUp()) { Printf("freecam: no level is up"); return false; }
    if (Cinematic()) { Printf("freecam: not during a cutscene"); return false; }
    unsigned player = 0;
    if (SWSE_PlayerBody(&player, nullptr) != 1 || !player) { Printf("freecam: no player"); return false; }
    float hp = 1.0f;
    if (SWSE_PlayerHealth(&hp, nullptr, nullptr) == 1 && hp <= 0.0f) { Printf("freecam: not while Stranger is dead"); return false; }
    unsigned ctx = MainContext();
    if (!IsHeap(ctx)) { Printf("freecam: no camera context"); return false; }
    BYTE en = 0;
    if (!RdB(ctx + CTX_ENABLED, &en) || !en) { Printf("freecam: the camera is not running (loading?)"); return false; }
    unsigned other = R32(ctx + CTX_CHEAT);
    if (other) { Printf("freecam: another cheat camera is already up (%08X)", other); return false; }
    unsigned src = R32(ctx + CTX_CURRENT);
    if (!IsHeap(src)) { Printf("freecam: no current camera"); return false; }
    if (!GateSiteOk()) {
        Printf("freecam: Stranger's input branch is not what SWSE measured - refused");
        Log("gate site at %08X does not match; not starting", b + RVA_GATE_SITE);
        return false;
    }
    unsigned subject = SubjectOf(src);
    if (!subject) subject = player;
    int context = (int)R32(src + CAM_CONTEXT);

    // Camera::CreateCheatCamera, as the beta did it: create the type, Init it
    // from the current camera's viewer, teleport it into that camera's zone.
    unsigned sp = 0;
    __try {
        ((CamCreate_t)(b + RVA_CAM_CREATE))(&sp, CAMTYPE_FLY, nullptr, (void*)(uintptr_t)subject, context);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("Camera::Create_Internal faulted");
        Printf("freecam: the game could not create the fly camera");
        return false;
    }
    if (!VtIs(sp, RVA_VT_FLYCAMERA)) {
        Log("Camera::Create_Internal returned %08X (vtable %08X), not a FlyCamera", sp, R32(sp));
        if (sp) EngineReleaseGuarded(&sp);
        Printf("freecam: the game did not make a fly camera");
        return false;
    }
    unsigned vt = R32(sp);
    if (R32(vt + VTS_INIT) != b + RVA_FLY_INIT) {
        Log("FlyCamera vtable slot 7 reads %08X, expected %08X", R32(vt + VTS_INIT), b + RVA_FLY_INIT);
        EngineReleaseGuarded(&sp);
        Printf("freecam: the fly camera is not the one SWSE measured - refused");
        return false;
    }
    bool ok = true;
    __try {
        ((FlyInit_t)(b + RVA_FLY_INIT))((void*)(uintptr_t)sp, (const void*)(uintptr_t)(src + CAM_VIEWER));
        ((CamTeleport_t)(b + RVA_CAM_TELEPORT))((void*)(uintptr_t)sp, (const void*)(uintptr_t)(src + CAM_FRAME),
                                                (int)R32(src + CAM_ZONE), (int)R32(src + CAM_SIDE));
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    if (!ok) {
        Log("FlyCamera::Init / TeleportCam faulted");
        EngineReleaseGuarded(&sp);
        Printf("freecam: the fly camera could not take the current view");
        return false;
    }
    Wr(sp + CAM_1PFOV, R32(src + CAM_1PFOV));           // the same field of view as the view it leaves
    Wr(sp + CAM_TRANSITION, TRANSITION_FINISHED);       // what SetCheatCam does
    WrB(sp + FC_FIRSTUPDATE, 0);                        // no snap-behind-the-subject on its first update
    if (g_speed > 0.0f) WrF(sp + FC_MOVESCALE, g_speed / SPEED_UNITS);

    unsigned short idx = 0, gen = 0;
    if (!Rd16(sp + OBJ_HANDLE, &idx) || !Rd16(b + RVA_HANDLE_GEN + idx * 6u, &gen) ||
        !HandleAlive(sp, idx, gen)) {
        Log("the new camera's handle does not check out (idx %u)", idx);
        EngineReleaseGuarded(&sp);
        Printf("freecam: could not track the fly camera safely - refused");
        return false;
    }
    // CameraContext::SetCheatCam: the slot takes over our one reference.
    if (!Wr(ctx + CTX_CHEAT, sp)) {
        EngineReleaseGuarded(&sp);
        Printf("freecam: could not install the fly camera");
        return false;
    }
    g_cam = sp; g_camIdx = idx; g_camGen = gen;
    g_player = player;
    g_epoch = SWSE_LevelEpoch();
    g_on = true;
    g_gate = false;
    g_waitSaid = false;
    g_since = GetTickCount();
    unsigned short rc = 0; Rd16(sp + OBJ_REF, &rc);
    Log("on: FlyCamera %08X (handle %u gen %u, refs %u) from camera %08X (type %d, context %d, zone %d), subject %08X, context object %08X",
        sp, idx, gen, rc, src, (int)R32(src + CAM_TYPE), context, (int)R32(src + CAM_ZONE), subject, ctx);
    if (SticksIdle()) GateSet(true);
    g_mouseCountsX = g_mouseCountsY = g_keyTurnDeg = 0.0;
    SWSE_InputMouseCapture(true);
    Log("mouse turning on: %.1f / %.1f degrees per 100 counts x the game's factor %.2f",
        g_sensX, g_sensY, GameMouseFactor());
    return true;
}

// ---------------------------------------------------------------------------
//  per frame
// ---------------------------------------------------------------------------

void SWSE_FreecamTick() {
    if (!g_on) return;
    const char* why = nullptr;
    unsigned p = 0;
    float hp = 1.0f;
    if (!CamAlive())                                        why = "the game let go of the camera - a load or a level change";
    else if (!CheatSlotOf(g_cam))                           why = "the fly camera is no longer the view";
    else if (Cinematic())                                   why = "a cutscene started";
    else if (!SWSE_LevelUp() || SWSE_LevelEpoch() != g_epoch) why = "the level changed";
    else if (SWSE_PlayerBody(&p, nullptr) != 1 || p != g_player) why = "the player changed";
    else if (SWSE_PlayerHealth(&hp, nullptr, nullptr) == 1 && hp <= 0.0f) why = "Stranger died";
    if (why) { Stop(why); return; }

    // Cut Stranger's input once the move stick is released: his last
    // HandleInput then left him standing, and skipping it keeps him so.
    if (!g_gate) {
        if (SticksIdle()) {
            // The site was checked at start; if the byte cannot be set now,
            // Stranger would keep his controls - so the view comes back.
            if (!GateSet(true)) { Stop("Stranger's input could not be held (see swse_log.txt)"); return; }
            if (g_waitSaid) Printf("freecam: movement released - Stranger stays put now");
        } else if (!g_waitSaid) {
            g_waitSaid = true;
            Printf("freecam: Stranger keeps his controls until the movement keys are released");
        }
    }

    TurnFrame();
}

// ---------------------------------------------------------------------------
//  the command
// ---------------------------------------------------------------------------

static void Status() {
    if (!g_on) {
        Printf("freecam: off (speed %.1f units/s when on)", g_speed > 0.0f ? g_speed : DEFAULT_SPEED);
        return;
    }
    float x = RF(g_cam + CAM_POS), y = RF(g_cam + CAM_POS + 4), z = RF(g_cam + CAM_POS + 8);
    float yaw = RF(g_cam + FC_YAW), pitch = RF(g_cam + FC_PITCH);
    float speed = RF(g_cam + FC_MOVESCALE) * SPEED_UNITS;
    Printf("freecam: on - pos %.2f %.2f %.2f  yaw %.1f  pitch %.1f  speed %.1f units/s  (Stranger's input %s)",
           x, y, z, HeadingFromYaw(yaw), (float)(-(double)pitch * DEG), speed,
           g_gate ? "cut" : "live until the movement keys are released");
}

static void SensStatus() {
    float gf = GameMouseFactor();
    Printf("freecam: mouse %.1f / %.1f degrees per 100 counts (sideways / up-down) x the game's sensitivity %.2f = %.2f / %.2f; arrow keys %.0f / %.0f degrees a second",
           g_sensX, g_sensY, gf, g_sensX * gf, g_sensY * gf, KEY_YAW_DPS, KEY_PITCH_DPS);
}

static bool EnsureOn() {
    if (g_on) return true;
    if (!Start()) return false;
    Printf("freecam: on");
    return true;
}

void SWSE_FreecamCmd(int argc, char** argv) {
    // The engine may have freed the camera since the last frame's check (a
    // load): never touch it without the handle test.
    if (g_on && !CamAlive()) Stop("the game let go of the camera - a load or a level change");
    if (argc < 2 || !lstrcmpiA(argv[1], "status")) { Status(); return; }
    const char* a = argv[1];

    if (!lstrcmpiA(a, "off")) {
        if (!g_on) { Printf("freecam: already off"); return; }
        Stop(nullptr);
        return;
    }
    // Everything below writes into game objects or code at addresses measured
    // on the Steam build.
    if (!SWSE_GameBuildKnown()) {
        char m[300];
        SWSE_GameBuildRefusal("freecam", m, sizeof(m));
        Printf("freecam: %s", m);
        return;
    }
    if (!lstrcmpiA(a, "toggle")) {
        if (g_on) Stop(nullptr);
        else if (EnsureOn()) Status();
        return;
    }
    if (!lstrcmpiA(a, "on")) {
        if (g_on) { Status(); return; }
        if (EnsureOn()) Status();
        return;
    }
    if (!lstrcmpiA(a, "speed")) {
        if (argc < 3) {
            float s = g_on ? RF(g_cam + FC_MOVESCALE) * SPEED_UNITS : (g_speed > 0.0f ? g_speed : DEFAULT_SPEED);
            Printf("freecam: speed %.1f units/s (full stick)", s);
            return;
        }
        float s = (float)atof(argv[2]);
        if (!(s >= 0.5f && s <= 100000.0f)) { Printf("freecam: speed must be 0.5 to 100000 units/s"); return; }
        g_speed = s;
        if (g_on) WrF(g_cam + FC_MOVESCALE, s / SPEED_UNITS);
        Log("speed %.2f units/s", s);
        Printf("freecam: speed %.1f units/s%s", s, g_on ? "" : " (from the next freecam on)");
        return;
    }
    if (!lstrcmpiA(a, "sens")) {
        if (argc < 3) { SensStatus(); return; }
        float sx = (float)atof(argv[2]);
        float sy = argc > 3 ? (float)atof(argv[3]) : sx;
        if (!(sx >= 0.1f && sx <= 200.0f) || !(fabsf(sy) >= 0.1f && fabsf(sy) <= 200.0f)) {
            Printf("freecam: sens is 0.1 to 200 degrees per 100 counts (a negative up/down value inverts it)");
            return;
        }
        g_sensX = sx;
        g_sensY = sy;
        Log("sens %.2f / %.2f", sx, sy);
        SensStatus();
        return;
    }
    if (!lstrcmpiA(a, "pos")) {
        if (argc < 5) { Printf("usage: freecam pos <x> <y> <z>"); return; }
        float v[3] = { (float)atof(argv[2]), (float)atof(argv[3]), (float)atof(argv[4]) };
        if (!Finite(v[0]) || !Finite(v[1]) || !Finite(v[2])) { Printf("freecam: position out of range"); return; }
        if (!EnsureOn()) return;
        if (!ApplyPose(v, RF(g_cam + FC_PITCH), RF(g_cam + FC_YAW))) { Printf("freecam: could not place the camera"); return; }
        Log("pos %.3f %.3f %.3f", v[0], v[1], v[2]);
        Status();
        return;
    }
    if (!lstrcmpiA(a, "look")) {
        if (argc < 3) { Printf("usage: freecam look <yaw> [pitch]   (degrees; yaw as `yaw` reports it, pitch up positive)"); return; }
        float h = (float)atof(argv[2]);
        if (!(h > -1.0e6f && h < 1.0e6f)) { Printf("freecam: yaw out of range"); return; }
        float up = argc > 3 ? (float)atof(argv[3]) : 0.0f;
        if (!(up > -1.0e6f && up < 1.0e6f)) { Printf("freecam: pitch out of range"); return; }
        if (!EnsureOn()) return;
        if (argc <= 3) up = (float)(-(double)RF(g_cam + FC_PITCH) * DEG);    // keep the current pitch
        if (up > 89.9f) up = 89.9f;
        if (up < -89.9f) up = -89.9f;
        float pos[3] = { RF(g_cam + CAM_POS), RF(g_cam + CAM_POS + 4), RF(g_cam + CAM_POS + 8) };
        if (!ApplyPose(pos, (float)(-(double)up / DEG), YawFromHeading(h))) { Printf("freecam: could not aim the camera"); return; }
        Log("look yaw %.2f pitch %.2f", h, up);
        Status();
        return;
    }
    Printf("usage: freecam [on|off|toggle|status] | pos <x> <y> <z> | look <yaw> [pitch] | speed [n] | sens [n] [up/down n]");
}
