# Blood decals - ATTEMPTED AND REMOVED

**Status: removed from the build.** The code is gone (`blood.cpp`,
`bloodgfx.cpp`, `camera.cpp` and their console commands); this document is kept
because the findings below are real, were expensive to obtain, and are worth
having if anyone tries again.

**Why it was removed.** Everything except one transform worked. The final
blocker was a systematic ~4.6 unit error in reconstructing a pixel's world
position, which with a squared falloff rendered decals at fractions of a
percent opacity - drawing, but invisible. Six explanations were proposed for
that one symptom over a long session; two were real (see below) and the rest
were chasing it downstream. It was not converging, so it was cut rather than
left half-working in the build.

**What would be needed to finish it.** The projection's Z row is the only
unknown. `ndcZ = -A + B*(1/w)` is linear in `1/w`, so two clean
(known world point, measured depth) pairs at different distances determine it.
The difficulty is CLEAN pairs: NPC positions are contaminated by occlusion -
the depth at an NPC's pixel often belongs to whatever stands in front of him,
and a fit over contaminated samples produced a confident wrong answer
(`B = +0.53` against a theoretical `-0.30`). The player's own body in third
person cannot be occluded from his own camera and is the one reliable
reference; a second at a different distance is what is missing.

---

## Original notes

Goal: when a character is shot they already emit a blood particle, but nothing
is left behind. Blood should mark the surfaces around the hit.

## The engine has no decal system

Established in [NPC_TUNING.md](NPC_TUNING.md): *"There is also no blood or decal
system - nothing matching decal, blood, splat, stain or gore anywhere in the
reflected fields or RTTI."* There is nothing to drive, so it has to be drawn by
SWSE.

The approach is screen-space decals: keep world-space splats, and in a
fullscreen pass reconstruct each pixel's world position from the scene depth
buffer, colouring pixels that fall inside a splat. That reuses the depth
texture selection and post-process pass already built for RTGI.

## The blocker was the camera matrix - SOLVED

Reconstructing a pixel's world position needs the inverse view-projection
matrix. SWSE only had near/far/fov (`SWSE_SceneProjection`), which linearises
depth but cannot place a pixel in the world.

The engine is ARB assembly and passes its matrix in `program.local`
parameters. Every scene vertex program transforms the same way:

```
DP4 result.position.x, R0, c[1];     clip.x =  dot(p, local[1])
MOV result.position.y, -R1.x;        clip.y = -dot(p, local[2])   <- NEGATED
DP4 result.position.z, R0, c[3];     clip.z =  dot(p, local[3])
...                        c[4]      clip.w =  dot(p, local[4])
```

so `glGetProgramLocalParameterfvARB` on a bound scene program yields the rows.
Captured in `camera.cpp`, verified with `camera mark on`, which draws a
crosshair where the player's world position projects to - it lands on his feet
and stays there as the camera moves.

### Three ways this went wrong first

**Reading whatever program was bound.** The capture originally ran inside the
texture-bind hook. The engine binds textures BEFORE it binds the program and
sets its locals, so it read the *previous* object's values and projected the
player off-screen. Local parameters persist per program, so binding a known
scene program at end of frame gives the values its last draw used.

**A degenerate matrix that looked perfect.** Slot base 0 appeared to work: the
marker tracked the player convincingly. Its X row is **all zeros**, forcing
`clip.x = 0`, which pins every point to the exact horizontal centre of the
screen - and a third-person player is horizontally centred. The marker was
stuck to the middle of the screen, not following anything. Printing the matrix
exposed it; the picture never would have. Capture now rejects zero X/W rows.

**Capturing from a non-instanced program.** Programs 3/4 apply each plant's
transform from vertex attributes first, so what they feed to `c[1..4]` is
already world space and `c[1..4]` is world->clip. Program 127 has no such step
and feeds object space, so its `c[1..4]` is that object's MVP - a perfectly
plausible-looking matrix that projects world points wrongly. Capture now reads
the program source and only accepts one containing `vertex.attrib[1]`.

## Still to do

* **`OnProjectileHit` as a shared event.** Hit reactions already resolve an
  impact to a world point and a bone at 100% resolve rate. Blood should
  subscribe to that rather than re-deriving it - the library the user asked
  for, with blood as its second consumer.
* **Splat storage.** A ring buffer of (world position, radius, age), capped for
  performance, fading over time.
* **The decal pass.** Reconstruct world position per pixel from scene depth
  using the inverse of the captured matrix, and blend blood inside each splat.
  Needs the matrix INVERTED, which is not yet implemented.
* **Verify impact points the same way.** The marker instrument works for any
  world point; pointing it at a recorded impact before rendering anything is
  the cheap way to confirm blood would land where the shot did.

## Tooling added

* `camera` - print the captured world->clip matrix and project a point
* `camera probe` - sweep local-parameter slots, report where each candidate
  puts the player
* `camera base <n>` - pin a slot
* `camera mark on` - draw a crosshair at the projected player position
