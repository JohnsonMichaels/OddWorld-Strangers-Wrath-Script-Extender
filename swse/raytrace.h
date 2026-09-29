// SWSE ray tracer - stage 1b of GRAPHICS_ROADMAP.md.
//
// Takes the harvested world-space triangle soup, builds a BVH over it, uploads
// both to the GPU, and traces rays in a compute shader. The acceptance test is
// visual and unambiguous: trace one camera ray per pixel, write hit distance to
// a texture, and see the level appear.
#pragma once

typedef void (*SWSE_RtEmit)(const char* line);

// Build the BVH from whatever the harvest last collected, then upload.
// Safe to call repeatedly; rebuilds from scratch.
void SWSE_RtBuild(SWSE_RtEmit emit);
// Dispatch the camera trace into the output texture (no-op until built).
void SWSE_RtTrace(int w, int h);
// The traced image, for the composite's debug view. 0 = nothing traced yet.
unsigned SWSE_RtTex();
void SWSE_RtStatus(SWSE_RtEmit emit);
void SWSE_RtSetNearZ(float z);     // -1 = GL convention, 0 = D3D/console
void SWSE_RtSetMaxDist(float d);   // trace range, also the shading scale
// 1 = put float-attribute draws in the BVH too. The packed-only rule was a
// proxy for "decodes correctly" and now excludes real geometry, the ground
// included; the cost of including them is that sky/fog planes come back.
void SWSE_RtSetUseAll(int on);
void SWSE_RtSetMode(float m);      // 0 depth, 1 ray direction, 2 ray origin
void SWSE_RtSetFlipY(float f);     // vertical orientation of the traced image

// ---- Stage 2: ray-traced ambient occlusion ---------------------------------
// Fires short occlusion rays from each pixel's world position into the BVH.
// Sees geometry behind surfaces and off screen - what screen space cannot.
void SWSE_RtAo(int w, int h, unsigned depthTex, float depthInvert,
               float depthFlipV, float nearZ, float farZ);
unsigned SWSE_RtAoTex();                                  // half-res AO, 0 = none
void SWSE_RtAoParams(float radius, float rays, float strength);
void SWSE_RtAoBlend(float b);
// Bound how far accepted history may pull the current estimate (V2's safety
// net, which RTAO dropped). 0 = unbounded, the old behaviour.
void  SWSE_RtAoClamp(float c);
float SWSE_RtAoGetClamp(void);   // weight of the new frame; low = smoother
void SWSE_RtAoGeoNormal(float on); // 1 = normals from the hit triangle
// Separable cross-bilateral denoise over the AO. Temporal accumulation alone
// still ghosts on fast camera motion: history is correctly rejected there and
// what is left is the raw few-ray estimate. Pass 0 for any value to keep it.
void SWSE_RtAoDenoise(float on, float radius, float depthTol, float normalTol);
// Whether the temporal reprojection flips V when fetching history. Wrong
// either way it drags AO across the world as the camera turns, so it is a
// toggle to be measured rather than a constant to be argued about.
void SWSE_RtAoHistFlip(float on);
// 1 = a pixel whose primary ray finds no BVH surface reports UNOCCLUDED
// instead of falling back to a depth-derived position and normal. The
// fallback casts rays from a surface the BVH does not contain and produces
// dancing false-occlusion specks.
void SWSE_RtAoRequireHit(float on);
// Log the derived camera for N frames (program id, consensus size, fov, eye)
// so frame-to-frame INSTABILITY is a number rather than a guess.
void SWSE_RtCamLog(int frames);
// Leaf-card policy, applied at BVH build so it can be swept with `rt build`
// alone. 0 disables. cardAny drops big triangles regardless of which program
// drew them, for trees whose texture was never fingerprinted.
void SWSE_RtCardSize(float area);
void SWSE_RtCardAny(int on);
// How solid a leaf card is to occlusion rays, 0..1. Cards stay in the BVH and
// most rays pass through, which is what a mostly-transparent leaf texture
// does. 1 restores the old fully-opaque behaviour.
void SWSE_RtCardOpacity(float o);
// Hash cells per world unit on a leaf card. Coarse cells make a card shade
// uniformly and read as a hard-edged grey polygon; fine cells make it noise
// that the spatial denoiser resolves into dappled shade.
void SWSE_RtCardCell(float c);
// Effective FOV of the inverse used to unproject depth. Scaling NDC by k
// scales tan(fov/2) by k. Sweeping k and minimising |Pdepth - hp| MEASURES the
// true FOV, rather than trusting the heap scan or the draw-derived value.
void SWSE_RtFovScale(float k);
void SWSE_RtFovSweep();
// Rebuild the BVH automatically once `threshold` new meshes have been
// collected. Paired with the continuous harvest so the acceleration structure
// follows the player instead of being pinned to wherever `harvest` was typed.
void SWSE_RtAutoBuild(int on, int threshold);
void SWSE_RtAutoBuildTick();
// The engine's clip-y negation, switchable so the convention can be measured
// against |Pdepth - hp| (debug rtao_strength -6) instead of assumed.
void SWSE_RtClipYNeg(int on);
// Read back one frame of the -6 debug view and report the MEAN camera error
// to swse_log.txt. Requires `set rtao_strength -6`.
void SWSE_RtErrProbe();
// 1 = unproject the depth buffer with the PREVIOUS frame's camera. The depth
// copy is a frame older than the program locals the camera is read from;
// pairing them removes the motion-only reconstruction error (2.1 -> spike to
// 6.3 during pitch with the current matrix).
void SWSE_RtPairPrev(int on);
