// SWSE geometry capture probe.
//
// The second feasibility question for an in-engine ray tracer: at draw time,
// can we READ the geometry the engine is about to rasterize - vertex
// positions and triangle indices - and is that data already in world space?
//
// World space matters enormously. If vertices arrive pre-transformed, a BVH
// can be built straight from the draw stream with no per-object matrix
// hunting. The probe proves it end to end by transforming captured vertices
// with the canonical camera matrix and checking they land on screen.
#pragma once

#include <windows.h>
#include <gl/GL.h>

typedef void (*SWSE_GeoEmit)(const char* line);

void SWSE_GeoArm(int nDraws);      // capture the next frame's first N world draws
void SWSE_GeoReport(SWSE_GeoEmit emit);
// Print one live program's position-building instructions, by GL id.
void SWSE_GeoDumpProgram(unsigned progId, SWSE_GeoEmit emit);
int  SWSE_GeoArmed();

// Called from the draw hook for candidate world draws (ARB vp bound, not
// skinned). Cheap no-op unless armed.
void SWSE_GeoCaptureDraw(GLenum mode, GLsizei count, GLenum type, const void* indices);
// Per-frame triangle census - runs whenever armed, over every world draw.
void SWSE_GeoFrameEnd();

// ---- STAGE 1: geometry harvest --------------------------------------------
// Collect one frame's entire visible world as a flat world-space triangle
// soup - the input a BVH builder consumes. Strips are expanded, degenerate
// stitching triangles dropped. Read back once; the data is static per level.
void SWSE_GeoHarvestArm();
// How many frames to wait for the camera before giving up (it appears once
// foliage has drawn, which can lag a level load).
void SWSE_GeoHarvestArmRetries(int n);
// Accumulate over this many frames, skipping meshes already collected. One
// frame draws only what it draws; several frames make a complete world.
void SWSE_GeoHarvestFrames(int n);
int  SWSE_GeoHarvestBusy();
void SWSE_GeoHarvestReport(SWSE_GeoEmit emit);
int  SWSE_GeoHarvestDumpObj(char* pathOut, int pathLen, int maxTris);
// The harvested soup, for the BVH builder: 3 vertices (9 floats) per triangle.
const float* SWSE_GeoHarvestTris(int* nTris);
// Shared with the tracer: the same inverse that recovered world space.
int SWSE_GeoInvert4x4(const float* m, float* out);
// Per-triangle: 1 = real world geometry, 0 = sky/water/fog billboard.
const unsigned char* SWSE_GeoHarvestPackedFlags();
// Per triangle: 1 = the draw wrote depth, i.e. a solid world surface. Sky,
// fog and water overlays are blended passes that do not write depth, so this
// separates world geometry from camera-attached planes by what the draw DOES
// rather than by how its vertices are packed.
const unsigned char* SWSE_GeoHarvestWorldFlags();
// Drop draws using a foliage vertex program from the solid set. OFF: those
// programs are shared with other instanced geometry, so this over-excludes
// badly (332 of 371 draws). Kept as a switch for the in-progress work on
// combining program id with the bound-texture test, as the wind gate does.
void SWSE_GeoExcludeFoliage(int on);
// Keep harvesting as the player moves, accumulating into one soup. Affordable
// because the cross-frame dedup returns before the GPU readback: a mesh
// already collected costs a hash lookup, and only first sight pays for
// glGetBufferSubData. Unique mesh count so far, for deciding when to rebuild:
void SWSE_GeoHarvestContinuous(int on);
int  SWSE_GeoHarvestMeshCount();
// Every distinct vertex program seen drawing world geometry. The camera
// consensus needs this pool: its own candidate list is foliage-derived and in
// a sparse area held only two programs, neither of which was the scene camera.
int  SWSE_GeoWorldPrograms(unsigned* out, int maxOut);
// Facts stored per triangle so the BVH can apply card policy at BUILD time
// without a re-harvest: was it drawn by a wind-injected program, and how big
// is it. A leaf card is both; terrain is big but not foliage; grass is foliage
// but not big.
const unsigned char* SWSE_GeoHarvestFoliageFlags();
const float*         SWSE_GeoHarvestAreas();
