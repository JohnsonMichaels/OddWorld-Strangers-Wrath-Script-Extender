// SWSE foliage wind.
//
// Grass and plants in this game are completely static. This bends them by
// rewriting the vertex programs that draw them.
//
// WHY REWRITING IS SAFE HERE. The engine's programs are ARB assembly, and ARB
// programs can be read back (glGetProgramStringARB) and re-uploaded
// (glProgramStringARB) in place. So no function needs hooking: the program is
// fetched, edited, and put back. Originals are kept so it is reversible.
//
// WHERE THE DISPLACEMENT GOES. Every foliage program starts by dequantising a
// packed position:
//
//     MUL R0, vertex.attrib[0], c[5];    # * scale
//     ADD R1, R0, c[8];                  # + bias
//
// and programs 3/4 then multiply R1 by a matrix held in VERTEX ATTRIBUTES -
// per-plant instancing. R1 is therefore that plant's OWN local space, so
// displacing R1.x/R1.z in proportion to R1.y bends each plant about its own
// base. Displacement is inserted immediately after the bias add, before the
// position is consumed.
//
// GATING. The wind vector lives in program.env[0], which the game never uses
// (measured: zero references across all 231 programs). It is set to the live
// wind only while a foliage texture is bound and to zero otherwise, so the
// injected instructions are inert for anything else that shares a program.
#pragma once

// Turn wind on/off. Injects (or restores) the foliage vertex programs.
// Must run with a GL context current, so it is serviced from the frame hook.
int  SWSE_WindSet(int on, char* msg, int msgLen);
// Writes the live source of every foliage program to swse_wind_dump.txt, so a
// patch that compiles but misbehaves can actually be read.
int  SWSE_WindDump(char* msg, int msgLen);

// Strength = peak bend in local units per unit of height. Speed scales the
// oscillation rate. Defaults are deliberately gentle.
void SWSE_WindParams(float strength, float speed);
void SWSE_WindGetParams(float* strength, float* speed);

// Stiffness. The tallest plant height that still gains movement: displacement
// is sway * height, so without a cap a tree bends in proportion to its size
// and whips. A uniform, so it tunes live without re-injecting.
void  SWSE_WindWeight(float w);
float SWSE_WindGetWeight();

// Player push: foliage bends away from the player, as if they were the wind.
// amount is how far (local units), radius how close before anything happens.
// Applied in WORLD space, after each plant's instance transform, because
// "away from the player" cannot be expressed in the plant's local axes
// without inverting that transform.
// maxHeight gates the effect by plant size: only plants shorter than it are
// pushed, so grass and undergrowth part around the player while trees stay
// put. Pass <= 0 for any argument to leave it unchanged.
void SWSE_WindPush(float amount, float radius, float maxHeight);
void SWSE_WindGetPush(float* amount, float* radius, float* maxHeight);

// Serviced once per frame from the frame hook: applies pending injection and
// advances the wind oscillation.
void SWSE_WindFrame();

// Called by the foliage bind tracker as the bound texture changes between
// foliage and non-foliage. Only acts on a CHANGE, so this costs nothing per
// draw even though binds are frequent.
// noPush marks a plant that sways but must not be shoved aside by the player
// (trees), flagged per entry in foliage.txt.
// Called per texture bind. `swayPct` is the entry's sway scale (0..100) from
// foliage.txt, so a tree can be near-still while grass moves normally.
void SWSE_WindGate(int isFoliage, int noPush, int swayPct);

// Sprint-aware push readout: `norm` is 0..1 (how sprinting he is right now),
// `seen` is the fastest horizontal speed observed this session, `ref` is the
// speed treated as a full sprint. Tune `sprintspeed` in wind.txt from `seen`.
void SWSE_WindSpeedInfo(float* norm, float* seen, float* ref, float* boost,
                        float* baseRadius);

// injected  = programs successfully rewritten
// failed    = programs whose expected pattern was not found (left untouched)
// on        = wind enabled
void SWSE_WindStats(int* injected, int* failed, int* on, float* curX, float* curZ);
// How many programs were refused as skinned character programs. Reported by
// `wind` because a rejection that is never surfaced is indistinguishable from a
// rejection that never happened.
int  SWSE_WindRejectedSkinned();

// Stall attribution. Program upload makes the driver compile, and the periodic
// revert-check reads a program back; either can cost hundreds of ms. Reported
// so an intermittent freeze can be pinned on a subsystem rather than guessed.
void SWSE_WindPerf(double* injectMs, double* injectMax,
                   double* checkMs, double* checkMax, int* stalls);

// Exaggerate the effect for a visual test: makes the bend obvious so the
// direction and pivot can be confirmed by eye before tuning it down.
void SWSE_WindTest(int on);

// Per-draw gating. Required in practice: the foliage vertex programs are
// shared with other instanced geometry, so ungated wind sways tyres too.
// Exposed mainly so the shared-program behaviour can be re-checked later.
void SWSE_WindGateEnable(int on);
int  SWSE_WindGateEnabled();

// Which local axis is up (0 = Y, 1 = Z), and whether to seed the oscillation
// phase per vertex. Both regenerate the injected code. They exist as switches
// because neither is derivable from the shader source: the up axis is a
// property of the models, and the per-vertex seed made plants twist on
// themselves, so it is off until a genuinely per-plant value is found.
void SWSE_WindAxis(int axis);
int  SWSE_WindGetAxis();
void SWSE_WindSeed(int mode);
int  SWSE_WindGetSeed();

// Settings live in SWSEMods\SWSE Wind\wind.txt. Load runs once at
// startup and can enable the effect (installing the foliage tracker it needs);
// save writes whatever is currently tuned. Every value was arrived at by
// looking at the result in game, so they must not be lost on restart.
void SWSE_WindLoadSettings();
void SWSE_WindSaveSettings();
