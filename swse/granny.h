// SWSE â€” Granny 3D animation layer (additive hit reactions).
//
// See granny.cpp for the full rationale. In short: the game uses RAD's Granny,
// whose local-pose stage is the natural place to add a small decaying offset to
// the bone nearest an impact â€” additive feedback rather than a canned reaction
// animation, so it layers over walking/aiming/idling.
#pragma once

// Find candidate granny_local_pose arrays by value signature (unit quaternion +
// near-identity scale-shear). Read-only: nothing is hooked or patched, which is
// deliberate â€” we have no disassembler to place a safe inline hook on Granny.
// Returns the number written; counts[] receives each run's bone count.
int  SWSE_GrannyScan(unsigned* addrs, int* counts, int maxOut, int minBones,
                     int wide, int strictScale, int mode);

// Dump the first few bones of a candidate pose to bin\swse_granny.txt, so the
// stride and layout can be confirmed against real data before anything writes.
void SWSE_GrannyDumpBones(unsigned poseAddr, int count, int stride, char* msg, int msgLen);

// Restrict the next scan to a window around 'base' (0 = full range).
void SWSE_GrannySetNear(unsigned base, unsigned radius);

// ---- additive hit reactions ------------------------------------------------
// Hooks granny's BuildWorldPose and adds a small decaying rotation to the bone
// that was hit, just before the hierarchy is composed. Perturbing the LOCAL
// pose means the change propagates down the limb automatically.
//
// The hook verifies the prologue bytes before patching and refuses if they do
// not match what was measured, so a different build cannot get its code
// corrupted.
// Concurrent reactions. One projectile queues one, so this also caps how many
// separate impacts can be decaying at once.
#define SWSE_MAX_REACTS 32
// Longest chain a single reaction may spread over (chest -> neck -> head ...).
#define SWSE_MAX_CHAIN  8

// Settings live in SWSEMods\SWSE Combat\hitreact.txt. Load runs once
// at startup and switches the feature on (installing the pose hook, the bolt
// hook and the health watcher). With no file the compiled defaults - the
// values signed off in play - are used and the feature still comes up ON.
void SWSE_HitReactLoadSettings();
void SWSE_HitReactSaveSettings();

int  SWSE_HitReactInstall(char* msg, int msgLen);
void SWSE_HitReactRemove();
void SWSE_HitReactEnable(int on);
int  SWSE_HitReactEnabled();
void SWSE_HitReactTune(float strength, int ms);
// Hook instrumentation: how many times the patched function ran, and how many
// bone orientations a reaction actually rewrote.
void SWSE_HitReactStats(unsigned* calls, unsigned* writes,
                        unsigned* lastSkel, int* lastBones);

// Identity probe. Captures each distinct skeleton the hook composes together
// with the translation of the a4 offset matrix, so the "match an impact to a
// character by proximity" idea can be verified against known NPC positions
// before any damage code depends on it.
void SWSE_HitReactProbe(int on);
int  SWSE_HitReactProbeGet(int i, unsigned* skel, int* bones, unsigned* a4,
                           float* pos, int* posValid);
// The trailing arguments plus the first dwords at a5, to identify the result
// world_pose and read its layout off live data.
int  SWSE_HitReactProbeArgs(int i, unsigned* a5, unsigned* a6, unsigned* head,
                            int* headValid);

// Read a granny_skeleton's header and auto-detect its granny_bone stride. Self
// verifying: the BoneCount stored at +4 must match the count the hook already
// receives, and every bone must yield a printable name and a sane parent index.
// Returns the bone count, or 0 with the reason in msg.
int  SWSE_GrannySkeletonInfo(unsigned skel, int expectBones, int* strideOut,
                             unsigned* bonesOut, char* msg, int msgLen);
int  SWSE_GrannyBoneInfo(unsigned bones, int stride, int index,
                         char* name, int nameLen, int* parent);

// Queue a reaction. skeleton=0 matches any character (useful for testing);
// dir is the impact direction, used as the rotation axis.
int  SWSE_HitReactTrigger(unsigned skeleton, int bone, float dirX, float dirY, float dirZ);

// Trigger on whichever character stands at (x,y,z) - the form the damage path
// uses, since it knows actor positions rather than Granny skeletons.
// bone < 0 = let the hook pick that skeleton's torso.
int  SWSE_HitReactTriggerAt(float x, float y, float z, float radius, int bone,
                            float dirX, float dirY, float dirZ, float strength);

// Impact-driven reaction. Give it WHERE the projectile landed and the hook
// resolves the nearest bone itself (root and weapon/prop attachment points
// excluded), so a shot to the arm moves the arm. This is the entry point the
// OnProjectileHit event feeds; other listeners (blood decals, custom projectile
// behaviour) can hang off the same event without touching this code.
int  SWSE_HitReactImpact(float x, float y, float z,
                         float dirX, float dirY, float dirZ, float strength);

// Projectile tracking. A health drop says who was hit, never where; a bolt's
// position in flight supplies the missing half, and the hook resolves it to a
// bone. This is the groundwork for an OnProjectileHit event that other features
// (blood decals, custom projectile behaviour) can share.
int  SWSE_BoltHookInstall(char* msg, int msgLen);
void SWSE_NoteBoltPosition(float x, float y, float z);
void SWSE_BoltStats(unsigned* seen);
int  SWSE_BoltRecent(int i, float* pos, unsigned* ageMs);
void SWSE_HitReactImpactStats(unsigned* resolved, unsigned* failed,
                              int* lastBone, float* lastDist);

// Watch live NPC health and fire a reaction whenever it drops. This is what
// turns the hook into a feature: shoot someone and they flinch.
// The cached live-actor list the damage watch maintains. Exposed so other
// systems can use it without paying for the heap scan - notably to verify the
// camera matrix by projecting every NPC and checking the marks land on them,
// which tests the whole screen rather than one centred point.
int  SWSE_WatchActors(unsigned* out, int maxOut);

void SWSE_HitReactWatch(int on);
int  SWSE_HitReactWatching();
void SWSE_HitReactTick();       // called once per frame
// Per-phase cost of that tick, in milliseconds (last and worst seen).
void SWSE_HitReactTickStats(double* scan, double* poll, double* player,
                            double* scanMax, double* pollMax, double* playerMax);
void SWSE_HitReactWatchStats(unsigned* polled, unsigned* hitsSeen);

// Skeletons with fewer bones than this are props/ammo, not characters, and are
// never perturbed. The crossbow's live ammo rides at the player's position, so
// without this a reaction on the player throws the zapfly around the screen.
// Pose record offsets, switchable at runtime so the two candidate layouts can
// be A/B tested against what actually renders.
void SWSE_HitReactSetOffsets(int base, int stride, int pos, int orient);
void SWSE_HitReactGetOffsets(int* base, int* stride, int* pos, int* orient);

// A flinch spread along the chain toward the root rather than applied to one
// rigid bone. links=1 restores single-bone behaviour.
// Whether a frame writes the CHANGE in rotation (correct if the pose buffer
// persists) or the full rotation (correct if animation rebuilds it each frame).
// Does the pose buffer survive to the next frame, or does animation rebuild it?
// same = it persisted, diff = it was rebuilt. Decides delta vs absolute.
void SWSE_HitReactPoseStats(unsigned* same, unsigned* diff);
// Times the hook body faulted. Non-zero means a bad assumption was caught and
// reactions were switched off rather than taking the game down with them.
void SWSE_HitReactFaults(unsigned* faults);

void SWSE_HitReactApplyMode(int useDelta);
int  SWSE_HitReactGetApplyMode();

// Cancel this fraction of the body's rotation at the arm roots, so a hit does
// not swing the held weapon. 0 = arms ride the torso, 1 = arms hold station.
// Envelope shape: how long the rotation takes to reach peak, and how far it
// swings past rest on the way back. The original snapped to full rotation on
// frame one, which popped rather than reading as an impact.
void SWSE_HitReactEase(float attack, float overshoot);
void SWSE_HitReactEaseGet(float* attack, float* overshoot);

// How much skeleton must hang below the bone a reaction rotates. A hit that
// lands on a fingertip is geometrically right and visually nothing, so the
// reaction climbs to a parent that carries real limb mass.
void SWSE_HitReactLimbMass(int n);
int  SWSE_HitReactGetLimbMass();

void  SWSE_HitReactArmDamp(float d);
float SWSE_HitReactGetArmDamp();

void SWSE_HitReactChain(int links, float falloff);
void SWSE_HitReactChainGet(int* links, float* falloff);
// Which bones the chain would actually rotate, for the last skeleton posed.
int  SWSE_HitReactChainPath(int* out, int maxOut, unsigned* skel, int* boneCount);

// Flatten every bone to bind pose. With the animation gone, any movement at all
// must be a hit reaction - the cleanest possible proof the system works.
void SWSE_HitReactTPose(int on);
int  SWSE_HitReactTPoseOn();

// Hold the animation at its current pose. Unlike the T-pose this keeps a
// natural stance, and it stops the game's own hurt animation from masking or
// imitating our offset - so what moves is unambiguously ours.
void SWSE_HitReactFreeze(int on);
int  SWSE_HitReactFreezeOn();

void SWSE_HitReactMinBones(int n);
int  SWSE_HitReactGetMinBones();

// World-space bone positions, composed inside the hook (the local pose is
// transient scratch and meaningless outside the call). This is the geometry
// that lets an impact point choose the bone it actually struck.
void SWSE_HitReactWposRequest(int minBones);
int  SWSE_HitReactWposGet(int i, float* pos, unsigned* skel, int* count);
// Raw composition inputs (bone 0 local transform + the a4 matrix), so garbage
// output can be attributed to the math or to the pointers.
int  SWSE_HitReactWposRaw(float* pos, float* quat, float* a4);
// The head of the pose buffer, to check for a granny_local_pose header ahead of
// the transform array.
int  SWSE_HitReactWposHead(unsigned* head, unsigned* addr);

// Measure the pose record layout instead of assuming Granny's documented
// granny_transform: scan a live pose buffer for unit quaternions and read the
// stride off the spacing between them.
void SWSE_HitReactLayoutRequest();
int  SWSE_HitReactLayoutGet(int i, unsigned* off, unsigned* base, int* count,
                            int* bones);

// Damage-to-strength curve: strength = base * clamp(floor + dmg*perDmg, 0, max).
// Weak weapons must read as weak, so the floor sits near zero.
// Feed the curve damage as a PERCENTAGE of the victim's max health instead of
// a raw number, so one curve covers every character. Absolute damage does not
// compare across characters: the same bolt is 1.4 off a Wolvark and 10 off the
// player, which made enemy reactions invisible while the player's looked right.
void SWSE_HitReactDamageRatio(int on);
int  SWSE_HitReactDamageRatioOn();

void SWSE_HitReactCurve(float floorV, float perDmg, float maxV);
void SWSE_HitReactCurveGet(float* floorV, float* perDmg, float* maxV);
// Recent hits, newest first - the measured damage each weapon actually deals,
// which is what the curve has to be tuned against.
// Recent hits, newest first, each annotated with the bone it actually rotated.
int  SWSE_HitReactHitLog(int i, float* dmg, float* scale, unsigned* ageMs,
                         int* bone, int* bones, unsigned* skel);
void SWSE_HitReactSetLogSlot(int slot);
// Where pending reactions get dropped, so a low bone-write count says WHY.
void SWSE_HitReactMatchStats(unsigned* match, unsigned* reject, unsigned* noMatrix,
                             unsigned* torsoFail, unsigned* boneRange);