# Animation, blending, and projectile impacts

Groundwork for impact reactions: shoot an NPC and have the hit body part react.

Established earlier and still true: **this engine has no ragdoll.** No ragdoll or
physique class appears in 432 recovered RTTI classes, and there is no blood or
decal system. So reactions have to be built from animation, not physics.

The good news is that the engine's animation system is layered, which is enough.

## The animation API (script verbs, callable)

Addresses below are absolute against the preferred base `0x400000`; SWSE works
in RVAs, so subtract `0x400000`.

| verb | address | signature |
|---|---|---|
| `PlayAnim` | 0x56CE80 | **ShortGoal**, Object |
| `PlayAnimBlend` | 0x56D0D0 | **ShortGoal**, Object, **float** |
| `PlayAnimBlendHold` | 0x56D320 | **ShortGoal**, Object, **float** |
| `StartCineTorsoAnim` | 0x56ABD0 | Object, int |
| `EndCineTorsoAnim` | 0x56AE20 | Object |
| `StopAnimation` | 0x55A680 | Object |
| `PutCurrentAnimationAtEnd` | 0x55A720 | Object |
| `TriggerAnimationReverse` | 0x55A620 | Object |

Two things stand out:

1. **`PlayAnimBlend` takes a float.** That is a blend weight - the engine
   supports weighted blending between animations, not just switching.
2. **`StartCineTorsoAnim` is torso-only.** An upper-body animation that leaves
   the legs alone is exactly the shape of a flinch, and proves the engine does
   partial-body layering rather than whole-body replacement.

Supporting classes: `AnimationLayer`, `AnimationLayerConfig` (`m_sources`,
`m_which`), `AnimationControl` (`m_xLo/m_xHi/m_yLo/m_yHi` - a **2D blend
space**), `SkeletonResource`, `BonePositionProxy`, `MotionImpl`,
`ShortGoalPlayAnim`.

## Bones are addressable BY NAME

This is the key to "which body part was hit":

| verb | address | signature |
|---|---|---|
| `SpawnEffectAttachedToBone` | 0x55ADF0 | EffectPref, **String** |
| `SpawnEffectAttachedToBonePosition` | 0x55B030 | EffectPref, **String** |
| `PlaySoundOnBone` | 0x556B50 | SoundHandle |
| `SpawnTendrilFromBoneToBone` | 0x55BDE0 | Effect, EffectPref, Object |
| `SpawnTendrilFromRandomBoneToObject` | 0x55C130 | Effect, EffectPref, Object |

A `String` bone name means we can attach an effect to a named bone without
knowing the skeleton layout in advance - and `BonePositionProxy` suggests bone
world positions are readable, which is what maps an impact point to a body part.

## Projectiles

| verb | address | signature |
|---|---|---|
| `FireBolt` | 0x553850 | NPCWeaponPrefs, String |
| `FireBoltMiss` | 0x5538B0 | NPCWeaponPrefs, String |
| `TakeDamage` | 0x555320 | Object |
| `ShakeOffDamage` | - | - |
| `ClearActiveBolts` | 0x5612F0 | - |
| `DestroyAttachedBolts` | 0x54D640 | Object |

Classes: `Bolt`, `BoltPrefs`, `BoltDamagePrefs`, `BoltEffectPrefs`,
`BoltStuckData`, `BoltSurfaceFXPrefs`, `BoltSurfaceSndPrefs`, `BoltSpreadPrefs`,
`BoltComplexData`, **`MoveBoltRayReq`**, `DamageCollisionComponent`.

`MoveBoltRayReq` is the one to chase for hit detection - a "move bolt ray
request" is almost certainly where a projectile's path is traced against the
world each tick, so it should carry both the impact point and what was hit.

`BoltStuckData` implies bolts can attach to what they hit, which means an
impact position is already being computed and stored somewhere reachable.

## Probe built: `anim`

```
anim <torso|endtorso|stop> [n] [tag]
```

Acts on the NPC nearest the player. The int argument's VM type tag was unknown,
so it is a parameter to sweep rather than a guess.

## Result so far: calls succeed, nothing visible

`StartCineTorsoAnim` was called on an NPC 5 units away with int values 1 and 3
and every type tag 0..6. **No call faulted**, and no visible pose change
(frame diff: 0.6% of pixels, mean 0.21/255 - noise).

### The likely reason, and the next move

Look at the signatures: `PlayAnim` and `PlayAnimBlend` take a **ShortGoal** as
their first argument, and there is a class called **`ShortGoalPlayAnim`**.

These verbs are not standalone commands - they are **goal constructors**. The AI
runs on a goal stack (we already use `ShortGoal` types elsewhere: `SGFireWeapon`,
`ShortGoalWaitForAIMotion`, `ShortGoalPlayAnim`), and playing an animation means
pushing a goal onto an NPC rather than calling a function at it.

That also explains the silence: calling the verb without a goal context builds
nothing and returns harmlessly.

**Next step:** find how a ShortGoal is constructed and pushed onto an NPC. We
already know the NPC goal machinery exists - `NPC_MIND 0xB4` -> MindBasic, and
`RVA_CombatGoto 0x16C540` demonstrably makes an NPC move, so goals *can* be
driven from outside. `CombatGoto` is the working example to imitate: whatever it
does to enqueue movement is what an animation goal needs.

## THE BIG FINDING: the game uses Granny 3D

38 functions reference `granny_*.cpp` sources:

| module | fns | why it matters |
|---|---|---|
| `local_pose` | 9 | per-bone LOCAL transforms - the insertion point |
| `world_pose` | 1 | composes local + hierarchy into world matrices (0x6AD060) |
| `track_mask` | 4 | **per-bone blend weights** |
| `controlled_animation` | 5 | animation instances |
| `animation_binding` | 2 | binds animation to skeleton |
| `fixed_allocator` | 6 | Granny's own allocator - NOT the game heap |

This is RAD's Granny middleware, not a bespoke system, so the API is documented
and stable. Granny's pipeline has exactly the seam additive hit reactions need:

```
SampleModelAnimations -> granny_local_pose    <- perturb HERE
BuildWorldPose        -> granny_world_pose
                      -> skinning / render
```

Perturbing a bone in the LOCAL pose means `BuildWorldPose` composes the
hierarchy for us: nudge an upper arm and the forearm and hand follow for free.
That is the whole feature, at the cost of one quaternion multiply plus a decay.

`track_mask` is a bonus - per-bone weights are how the effect can taper down a
limb (shoulder strong, hand barely).

## Why we scan instead of hooking

The obvious move is to inline-hook `BuildWorldPose` and take the pose from its
arguments. A 5-byte JMP has to land on whole instruction boundaries and we have
no disassembler for that address; guessing is exactly what crashed the early
graphics pass. So the pose is located **read-only, by value signature** - the
same technique that found the camera frustum.

## Probe built: `granny`

```
granny [minBones] [wide] [strict]   find candidate bone arrays
granny dump <addr> [n]              dump transforms to bin\swse_granny.txt
```

`wide` opens the scan beyond the game-heap window, which matters because Granny
allocates through its own `fixed_allocator`. `strict` re-enables a
near-identity scale-shear test that proved too aggressive.

## Result: found bone data, but the stride assumption is WRONG

The narrow scan finds **64+ candidate arrays** (hits the cap) with runs of
12-16 elements. But dumping one shows a telltale pattern:

```
bone 0  pos(1.905, 1.935, 1.966)  quat(0,0,0,999)
bone 1  pos(0.000, 0.999, 0.000)  quat(-5,0,999,0)
bone 2  pos(0.999, 0.000, 0.000)  quat(0,999,0,-15)
bone 3  pos(0.000, 0.000, 0.000)  quat(999,0,-23,-1)
bone 4  pos(0.000, 0.000, 0.999)  quat(0,-31,-2,999)
```

The `0.999` **migrates through the slots** and the quaternion components rotate
with it. That is an array read at the WRONG STRIDE, drifting a fixed amount per
element.

The drift is 4 bytes per element, which identifies the real layout: these are
**4x4 matrices (64 bytes)** being read at 68. And that also explains why the
detector fired at all - a **row of a rotation matrix is unit length**, so the
"unit quaternion" test matches rotation matrices too.

**Conclusion: the scan is finding WORLD-POSE matrices, not `granny_transform`s.**

That is still progress: bone data is present, reachable and abundant. The
detector just needs to distinguish the two layouts.

### Next steps

1. Re-run the dump at a **64-byte stride** and confirm the matrices stabilise
   into sane rotation+translation. That verifies the world-pose layout.
2. Tighten the signature to separate `granny_transform` (68B, quaternion at
   +0x10) from a 4x4 matrix (64B, bottom row 0,0,0,1) - the bottom row is the
   cheapest discriminator.
3. Local poses are what we actually want to write. If they cannot be found by
   signature, follow the moolah precedent: walk the object graph from a known
   NPC instead of scanning blind, and find which object points at a pose.

## CONFIRMED: a live character's bone data is readable

The `Flags <= 7` discriminator was the unlock. `granny_transform` begins with a
small bitfield (HasPosition/HasOrientation/HasScaleShear), while a 4x4 matrix
begins with a rotation component - a huge value read as an integer. Adding that
one test cut 64 false positives to a single true skeleton.

```
granny 30 mat   ->  19D30870   63 bones
```

Dumped, it is unmistakably a real skeleton:

```
bone 0  T(-510.45, -782.87, 95.52)  R0(289,957,0)    R1(-957,289,0)
bone 3  T(-510.38, -782.91, 96.66)  R0(144,989,0)    R1(-938,136,317)
bone 7  T(-510.24, -782.53, 97.67)  R0(-67,510,-857) R1(-461,745,480)
```

1. **63 bones** - a textbook humanoid count.
2. **Translations sit at (-510, -782, 96)** - exactly where `bring` had just
   teleported the NPCs. These are world-space bone positions of a character
   standing in front of the player.
3. **Rotations diverge progressively** down the array, as a hierarchy does.

Contrast the 238-element run the looser scan also found: identity quaternions
throughout and tightly clustered positions - scattered props or particles, not
a skeleton. Worth remembering as the shape of a false positive here.

**This already delivers impact -> bone**, which is half the hit-reaction system
and *all* of what locational damage needs: given an impact point, the nearest of
these 63 positions names the body part. That part needs no write access at all.

## The local pose is transient - writing needs to be in-frame

Searching outward from the world pose found no `granny_transform` run at 256 KB,
2 MB or 16 MB (only the particle-like 238 array).

The likely reason is ordinary Granny usage: applications allocate **one** local
pose sized to the largest skeleton and reuse it as scratch for every character.
World poses persist because rendering needs them; local poses do not exist
between frames. So there is nothing stable to find from outside.

Consequence: **perturbing the local pose has to happen during the animation
update**, not from a per-frame tick at swap time.

## The safe hook: hardware breakpoints, not code patching

SWSE already has the machinery - `watchexec`, DR0-DR7, a vectored exception
handler. That traps execution with **no code modification at all**, which side-
steps the instruction-boundary problem that made an inline JMP too risky.

Armed on the Granny function and it **fired immediately and repeatedly**:

```
watchexec 2AD060
WATCH HIT: code at 00D6D060 (module+0x2AD060)
```

That confirms two things: the address is right, and it is reachable safely.

**But it also hung the game.** The function runs for every character every
frame, so a breakpoint that logs on every hit floods the handler faster than the
console can answer, and the game had to be restarted to clear it. The watch
state is in-memory only, so a restart is a complete reset.

### What a usable version needs

- **Fire once, then disarm** - arm, capture the arguments from the trapped
  CONTEXT, immediately clear DR7, and process afterwards. Never free-run.
- Or gate on a specific character so the handler runs a few times per frame
  instead of hundreds.

The payoff for one clean capture is large: the trapped CONTEXT gives ESP, so
the arguments to `BuildWorldPose` can be read directly, which yields the local
pose pointer, the bone count, and the skeleton - everything the write path needs.
The known world-pose address (`19D30870`) is a ready-made check: if it appears
among the arguments, the layout is confirmed rather than assumed.

## Validation idea: Steef is a centaur

Stranger is bipedal (63 bones). Steef is a **centaur** - four legs, so a
materially different skeleton and bone count. Switching between them is a free
correctness test for the scanner: the bone count should change substantially. A
scanner that reports 63 for both is matching something other than the player.

## SOLVED: the call signature, captured from a real call

The free-running breakpoint wedged the game, so `watchexec` gained a **one-shot**
mode: the handler captures once and disarms itself from inside the trapped
CONTEXT (writing `Dr0`/`Dr7` there is applied by the OS on resume). With that,
the capture is safe and the game survives.

```
WATCH HIT: code at 00D6D060 (module+0x2AD060)
new value = 83EC8B55
stack: ret=00CC56BC a0=13F9DF30 a1=00000000 a2=00000005 a3=5B1DA058
       a4=3431F810 a5=00000005 a6=13B84070 a7=40400001 a8=00000005
```

Mapped onto Granny's documented signature:

```c
GrannyBuildWorldPose(Skeleton, FirstBone, BoneCount, LocalPose, Offset4x4, Result)
                     a0        a1=0       a2=5       a3         a4=stack
```

| arg | value | reading |
|---|---|---|
| a0 | 13F9DF30 | Skeleton (game heap) |
| a1 | 0 | FirstBone - exactly as expected |
| a2 | 5 | BoneCount |
| a3 | **5B1DA058** | **LocalPose** |
| a4 | 3431F810 | Offset4x4 - a stack address, i.e. a temporary matrix |

`FirstBone = 0` plus a stack-resident `Offset4x4` is a strong signature; this is
`BuildWorldPose` (or its direct equivalent) and the stack layout is real.

## PROVEN: the local pose is transient

Dumping `5B1DA058` immediately afterwards **faulted** - the address is not
readable between frames. That settles it: the local pose is scratch memory,
valid only *during* the animation update, which is why no amount of scanning
found one and why it sits at `0x5B…`, outside the game heap entirely.

**Consequence: the perturbation must be applied inside the call.** There is no
"write it from a per-frame tick" option.

## The inline hook is now SAFE (we have the bytes)

The handler logs raw bytes at EIP:

```
bytes around EIP:  C3 CC CC CC CC CC CC CC | 55 8B EC 83
```

`C3` ends the previous function, seven `CC` are alignment padding, and the
function begins at a clean boundary:

```
55        push ebp         -> boundary at 1
8B EC     mov ebp, esp     -> boundary at 3
83 EC XX  sub esp, imm8    -> boundary at 6
```

A 5-byte JMP at offset 0 would split `83 EC XX` (bytes 3-5). The safe patch is
therefore: **relocate the first 6 bytes to a trampoline, write JMP + one NOP,
and have the trampoline jump back to entry+6.**

This is exactly the risk that made an inline hook unacceptable earlier. It is
resolved now - not by being braver, but because the hardware breakpoint handed
us the actual bytes at zero risk. Measure, then patch.

## Design for the implementation

```
HookedBuildWorldPose(Skeleton, FirstBone, BoneCount, LocalPose, Offset, Result)
    if (reactions enabled && this instance has an active hit)
        for each active hit on this character:
            bone  = hit.boneIndex
            t     = elapsed / duration            // 0 -> 1
            amt   = hit.strength * (1 - t) * (1 - t)   // ease-out decay
            apply amt as a small quaternion rotation to LocalPose[bone].Orientation
    call the original
```

Because it perturbs the LOCAL pose, `BuildWorldPose` propagates the change down
the hierarchy for free: nudging an upper arm carries the forearm and hand.

Still to resolve:

1. **Which character is this call for?** `BoneCount = 5` on the captured call is
   small, so the pose builder runs for sub-skeletons and props too. The
   Skeleton pointer (a0) is the natural identity key - map NPC -> skeleton once,
   then match.
2. **Impact -> bone index.** The world pose (found earlier, 63 bones) gives bone
   world positions; nearest-to-impact names the bone. Note the world pose is the
   *output*, so it is one frame behind - fine for this purpose.
3. **Where the hit is detected.** `MoveBoltRayReq` remains the target, or hook
   `TakeDamage` (0x555320) which we already call successfully.

## Toggle

Per the request, this ships behind a switch like the RTGI and NPC tuning:
`hitreact 0|1`, plus strength and duration, live-tunable and persisted.

## Open questions

- Does `StartCineTorsoAnim`'s int select an animation index, or a mode?
- Can `BonePositionProxy` give bone world positions from outside the engine?
  That is what turns an impact point into "left arm".
- Does `MoveBoltRayReq` expose the impact point and the struck object?
- `NPCPrefs +0x484 m_hurtReaction` is already tunable via `npchurt` - worth
  checking whether it selects between existing reaction animations, which would
  be a much cheaper route to hit reactions than building one.

---

## Additive hit reactions - built and working

The hook is installed, writes bones, and is driven by real damage. What follows
is measured, not inferred.

### The prologue check earned its keep immediately

The breakpoint dump showed only FOUR bytes of `BuildWorldPose`: `55 8B EC 83`.
The natural completion is `83 EC xx` (`sub esp, imm8`) and that is what the
installer originally verified. The real bytes are:

```
55        push ebp                boundary @1
8B EC     mov  ebp, esp           boundary @3
83 E4 F0  and  esp, 0FFFFFFF0h    boundary @6   <- ALIGNMENT, not sub
```

The installer refused to patch and the game survived at 58 fps. Both encodings
are three bytes, so `PROLOGUE_LEN 6` was right either way - but the check is the
only reason a wrong guess did not corrupt live code. **Verify prologue bytes
against the running image before every patch; never complete an instruction from
memory.**

### Instrumentation beats screenshots

A pixel diff cannot separate "we moved a bone" from "the NPC took a step" - the
first attempt showed 1.8% pixels changed and proved nothing. Counters settled it
in one run:

| state | hook calls | bones written |
|---|---|---|
| enabled, nothing queued | 7,273 | 0 |
| after `hitreact test 5`  | 13,214 | 1,673 |

`hitreact test all` at 1.2 rad across 32 bones produced a violently contorted
character on screen - an absurd-strength sanity check that proved the whole
chain end to end: hook → local pose → quaternion → visible geometry. It also
confirmed the layout guesses (`GT_ORIENT` +0x10, quaternion order x,y,z,w) and
that perturbing the LOCAL pose propagates down the limb for free.

### granny_skeleton layout in this build

```
granny_skeleton:  +0x00 char* Name (NULL - names are STRIPPED)
                  +0x04 int   BoneCount      <- self-verifying against the hook
                  +0x08 granny_bone* Bones
granny_bone:      stride 0x70 (NOT Granny's documented 152)
                  +0x68 int ParentIndex
```

Names being stripped is what broke the first scanner - it required a readable
name per bone and therefore rejected every candidate. Parent indices are what
the code actually needs, so the scanner now searches `(stride, parentOffset)`
jointly and requires a valid hierarchy over all N bones: index 0 is the root
with parent −1, every other parent strictly precedes its child. For the 51-bone
character **exactly one** pair satisfied that, so it is not a coincidental fit.

The recovered hierarchy is plainly a real skeleton: bone 10 is a hub with 18
direct children (head, arms, prop attachments), and bones 27→28→29, 31→32→33,
34→35→36, 37→38→39 are four clean 3-link limb chains off bone 4.

### Picking a bone without names

The chest is what reads as a flinch, and rotating it carries the head and arms
along. With names stripped, the chest is found structurally: **the non-root bone
with the most direct children**. On the 51-bone character that is bone 10 with
18, against 1-2 for ordinary limb links - not a close call. Cached per skeleton;
walking the bone array every frame is far too expensive. `torsoFail` has stayed
0 across tens of thousands of evaluations.

### Joining damage to skeletons without a lookup table

The damage side knows an actor pointer; the hook knows a Granny skeleton, and
nothing maps between them. The fifth argument `a4` is the composition offset
matrix, and its translation (elements 12,13,14) is the character's world
position - verified exactly:

| probe sample | bones | a4 translation | ground truth |
|---|---|---|---|
| 4  | 51 | `170 −239 44` | player at `170.9 −239.6 41.7` |
| 13,14 | 24,15 | `172 −240 43` | 3 NPCs `bring`-ed to `170 −239 41` |

So an impact is matched to a character **by proximity**. Note `a5` is the bone
count repeated (0x33 = 51), not a pointer - the result buffer is `a6`, a C++
object with three vtables identical across all 15 skeletons.

Match against a **cylinder, not a sphere**: the pose origin sits ~2.5 units
above the actor origin, so a 3-unit sphere only barely contains the character
and drops out as they animate. Tight horizontally, loose in z.

### Damage detection is a poll, not a second hook

Hooking the damage routine would give the exact impact point, but it is also a
way to destabilise combat code for a cosmetic effect. Health is already readable
per NPC (`actor+0x78` is the current/max/base triple) and a drop is an
unambiguous "this one was hit". Cost: a drop says *who*, not *where* - direction
is taken from the player, which is where the shot came from and carries most of
the feel.

**`SWSE_FindNpcs` walks the heap and must never run per frame - measured 5.90
fps against 59, a 10x collapse.** The actor list is refreshed on a 2 s timer
while health is polled every frame from cached pointers. Back to 55.8 fps with
the whole feature live.

### Testing unattended: the game does not advance while unfocused

A reaction queued and then slept on shows exactly "1 match, 13 rejects" - one
frame of evaluation - because wall-clock age expires while the game is paused.
Poll in short intervals that let the game tick instead; the same reaction then
accumulates hundreds of matches. This is a testing artifact, not a bug, and it
invalidates any unattended measurement that sleeps through the interesting part.

### Commands

```
hitreact on | off              install + enable + watch for damage
hitreact                       status and where reactions are being dropped
hitreact <strength> <ms>       tune (default 0.12 rad / 220 ms)
hitreact test [bone|all]       fire on every character, ignoring damage
hitreact here [radius]         position-matched reaction at the player
hitreact probe [show]          skeletons being composed, with world positions
hitreact bones [n]             bone hierarchy of a probed skeleton
hitreact watch on|off          damage polling only
peek <hexaddr> [dwords]        general hex/float/ascii memory view
```

### Still open

- Impact POINT. A health drop gives the victim, not the wound location. That
  needs the projectile path (`MoveBoltRayReq`), and it is also the prerequisite
  for locational damage.
- Bone-world positions for nearest-bone selection. The hierarchy is now known,
  so composing local transforms into world space is the remaining step; the
  torso heuristic is a stand-in until then.

---

## The pose record layout was wrong (and why it looked right) - RETRACTED, see below

Every bone write before this point landed at the wrong offset. The constants
came from Granny's documented `granny_transform` - 0x44 stride, quaternion at
+0x10 - and those are simply not what this build uses.

**The trap:** writing a quaternion at a wrong offset inside a pose buffer still
rotates *a* bone. Characters visibly moved, so the layout looked confirmed. It
explained nothing about why the motion never read as a clean flinch, why held
offsets stretched characters into spikes, or why world-position composition
returned NaN - those were all treated as separate problems for hours.

**Measuring instead of assuming.** `hitreact layout` scans a live pose buffer
for unit quaternions: four floats in [-1,1] with norm 1.0 within 0.001 is a very
strong signature. The *spacing* between hits gives the stride directly. Result
on a 31-bone character:

```
+0x08  (header false positive)
+0x28  stride 0x20
+0x3C  stride 0x14
+0x58  stride 0x1C
+0x6C  stride 0x14
+0x88  stride 0x1C   ... repeating 0x1C / 0x14
```

Alternating 0x1C/0x14 means the real period is **0x30**, with one genuine
quaternion per record plus one false positive straddling the record boundary.

**The measured layout:**

```
granny_local_pose:
  +0x00  int    capacity (0x64 seen)
  +0x04  ptr -> transform array, always pose+0x10
  +0x08  float  0.010
  +0x0C  int    tag/marker, repeats once per record

transform record, stride 0x30, first at pose+0x10:
  +0x00  float  1.0
  +0x04  int
  +0x08  int
  +0x0C  float[3]  position
  +0x18  float[4]  orientation (x, y, z, w)
  +0x28  float     scale        (NOT a 9-float scale-shear)
  +0x2C  int       marker
```

**Self-verifying:** with these constants bone 0 reads position `(0,0,0)` and
orientation `(0,0,0,1)` - an exact identity root, which is what a root bone must
be. The old constants read a NaN position and a quaternion that only made sense
as (w,x,y,z). Quaternion ORDER was the one thing that was right.

Composition immediately produced a character-shaped cloud: 4 units wide, 22
tall, rooted at the character's feet, versus `-2147483648` before.

### Lesson

Vendor documentation is a hypothesis about a shipped binary, not a fact about
it. Where a wrong value still produces *some* visible effect, "it moved" is not
confirmation - the layout has to be verified against a value that could only be
right (an identity root, a unit quaternion, a self-consistent hierarchy).

## Accumulation: the local pose persists between frames

The pose buffer is NOT rebuilt from animation each frame. Writing the full
rotation every frame therefore COMPOUNDS: a 0.15 rad flinch decaying over ~15
frames accumulated toward 2 rad. Every strength value tuned before this fix was
doing something roughly 15x larger than the number said, which is why nothing
ever looked subtle and why a held test offset deformed characters permanently.

Fix: apply only the CHANGE since last frame. Because each step turns about the
same axis the increments telescope, so net rotation is always exactly k, and an
expiring reaction runs one final step with k = 0 to unwind itself completely. A
reaction also binds to the first character it touches, so the unwind reaches the
same bones even if that character has moved out of match range.

## Performance note

`SWSE_FindNpcs` walks the heap. Refreshing the watch list every 2 s produces a
~150 ms hitch on a 2-second beat - invisible in an averaged fps number (55.8)
but very visible while playing. Averaged frame rate is the wrong metric for
anything that runs on a timer; measure the hitch, not the mean.

---

## RETRACTION: the 0x30 layout is not the rendered pose

The section above concluded the pose records were stride 0x30 with the
orientation at +0x18, and called it "measured, not assumed". That conclusion is
wrong, and the way it was reached is worth keeping as a warning.

**The experiment that settled it.** Rotate every bone by 2 rad under each
candidate layout and photograph the character:

| layout | on screen |
|---|---|
| base 0x00, stride 0x44, orient +0x10 | character stretched across the screen |
| base 0x10, stride 0x30, orient +0x18 | **completely unaffected** |

So the mesh is driven by `0x00 / 0x44 / +0x04 / +0x10` - Granny's documented
`granny_transform`, which is what the code used originally.

**Why the wrong answer was so convincing.** The 0x30 structure is really there.
Scanning for unit quaternions found them at a regular stride; reading the first
record with those offsets gave position `(0,0,0)` and orientation `(0,0,0,1)`,
an exact identity root. Every internal check passed. It is a genuine structure
sharing the same allocation - plausibly a bind pose or a blend source - and none
of those checks could distinguish it from the pose that gets composed.

**The actual error in reasoning.** "These values are self-consistent" was
treated as "these values are the ones in use". Structural self-consistency
proves a structure exists; only an effect-visible test proves which structure
the engine reads. The check that mattered was the one nobody can fake: change it
and see whether the picture changes.

Both hypotheses also had a render-visible prediction available the whole time,
and testing it took one A/B once the offsets were made switchable at runtime.
That should have come first, before any documentation was written.

**What was actually broken** was accumulation (see the section below): the pose
buffer persists between frames, so writing a full rotation each frame compounds
it. The offsets were correct from the start.

### Consequence for bone composition

`ComposeWorldBones` produces a character-shaped cloud under the 0x30 offsets and
garbage under 0x44, which is the reverse of the rendering result. So the local
transform used for rendering does NOT carry usable positions at +0x04, and
world-space bone positions still have no verified source. Impact-to-bone
selection is therefore still unsolved - the machinery works, but it needs bone
positions that come from the same data the renderer uses.

---

## WORKING - final configuration and what actually mattered

Additive hit reactions are working on the player, enemies and creatures: a
projectile produces a locational, decaying rotation layered over whatever
animation is playing, propagating down the limb into any held weapon.

These are now the CODE DEFAULTS, not commands to retype. Settings resetting on
restart repeatedly made the feature look broken - at one point it appeared not
to work on outlaws at all because a restart had silently disabled it.

```
strength   1.2 rad / 360 ms
curve      0.25 + dmg * 0.04, max 0.5   (dmg = % of the victim's max health)
chain      3 links, 60% falloff          (chest -> neck -> neck)
limbmass   14                            (climb until 14 bones hang below)
ease       attack 18%, overshoot 12%
armdamp    0                             (arms ride the torso - vanilla does too)
minbones   12                            (props/ammo excluded)
```

Cost: ~2 fps against a 60 fps baseline. 100% impact-to-bone resolve rate, with
the chosen bone typically within 0.1-0.7 units of the impact point.

### The bug that cost the most: scaling by absolute damage

Reactions looked correct on the player and invisible on Wolvarks. Same code,
same hook, same bones resolving - the difference was the INPUT. A crossbow bolt
takes ~10 health off the player and ~1.4 off a Wolvark, so an absolute damage
curve produced 5.4 degrees on an enemy against 17+ on the player. Not a bug in
any mechanism; a units error.

Fix: feed the curve damage as a PERCENTAGE of the victim's max health (the
health triple at actor+0x78 is current/max/base). One curve then fits every
character, including rigs that do not exist yet - a Scrab or a Sea Rex needs no
per-creature tuning. The Fallout NV additive-hit-reactions mod does the same
thing (fDmgRatio), which is a good sign it is the right shape.

### The diagnostic that cracked it: T-pose

"It does not work on Wolvarks" has at least three causes - writes not reaching
them, bones not resolving, or the effect being too small - and no amount of
staring at combat separates them. Flattening every bone to identity does:

  * Wolvark goes rigid  -> our writes DO control their final pose
  * Wolvark animates    -> our writes never reach it

One screenshot of a T-posed Wolvark killed an entire wrong theory (that
BuildWorldPose runs per animation layer and a later blend was overwriting us)
and pointed straight at magnitude. Cranking to 3.0 rad then confirmed it -
everything ragdolled, so the path was fine all along.

Generalisable: when an effect is invisible, first prove the WRITE lands by
making it absurd, then tune. Debugging a subtle effect and an unproven
mechanism at the same time is how hours disappear.

### Other fixes worth keeping

* Reactions overlapped and each read the previous one's OUTPUT as its base, so
  sustained fire multiplied rotations without bound. Contributions are now
  accumulated and each bone is written exactly ONCE per call, onto the engine's
  own pose. Concurrency also scales the total back when several land together.
* Envelope: the original (1-t)^2 jumped to full rotation on frame one, which
  popped. Now smoothstep attack (~18% of duration), decay, and a small
  counter-swing past rest before settling.
* Arm damping (cancelling the body's rotation at the arm roots so a held weapon
  keeps its aim) was built to solve a phantom - the arm movement the user saw
  was vanilla. Left in at 0 by default. It also has to be biped-only: on a
  20-bone creature rig it treated three of four limb chains as "arms" and was
  cancelling rotation on a chicken's legs.
* minbones excluded critters along with the crossbow's live ammo, since both are
  3-8 bone skeletons. Position separates them: ammo rides at the player's own
  position, a critter does not.
* Stale actor pointers between list refreshes produced garbage damage values
  (-2147483648) that slammed the curve to its cap and fired maximum-strength
  reactions. Rejected explicitly.
* Layout searching per frame cost ~200,000 VirtualQuery calls and froze the game
  for seconds on the first hit of each new skeleton. Cached per skeleton, the
  known-good layout tried first, and one guarded block instead of a syscall per
  read.
* Hook removal is not thread safe: unpatching while the render thread is inside
  the hook crashed the game on the next window activation. 'off' now disables
  without unpatching.
