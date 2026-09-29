// SWSE materials - the SSR reflectivity mask.
//
// materials.txt (per mod, accumulating in load order) lists texture upload
// fingerprints graded reflective by the owner: M metal, G glass, C ceramic,
// A water. At draw time, binds of a listed texture enable alpha writes and
// every other bind disables them, so reflective surfaces stamp their own
// per-pixel mask into destination alpha as the scene renders. The post pass
// captures RGBA and the SSR shader reads the mask from uScene.a.
#pragma once

void SWSE_MaterialsInit();                 // load materials.txt (once)
int  SWSE_MaterialsReload();               // re-read + rebuild flags; returns entries
void SWSE_MaterialsNoteUpload(unsigned hash, unsigned texid);
void SWSE_MaterialsOnBind(unsigned texid); // glBindTexture hook tap
void SWSE_MaterialsFrameMark();            // swap-time: re-arm, roll counters
void SWSE_MaterialsPassGuard(int inPass);  // 1 = our post pass: stop stamping
void SWSE_MaterialsSetStamp(int on);       // fed from gfx params each frame
void SWSE_MaterialsSetDepthTest(int on);   // redraw depth test (diagnostic off)

int  SWSE_MaterialsListN();                // fingerprints loaded
int  SWSE_MaterialsKnownTexids();          // live texids flagged reflective
int  SWSE_MaterialsBindsLastFrame();       // reflective binds seen last frame
int  SWSE_MaterialsStampState();           // stamping active right now?
int  SWSE_MaterialsMarks();                // FrameMark heartbeat counter
int  SWSE_MaterialsSuspended();            // suspension state at query time
unsigned SWSE_MaterialsMaskTex();          // the mask FBO's color texture (0 = none)
int  SWSE_MaterialsRedrawsLastFrame();     // tagged draws re-issued last frame
int  SWSE_MaterialsDrawsSeenLastFrame();   // total glDrawElements seen last frame
int  SWSE_MaterialsDrawHooked();           // draw trampoline installed?

// ---- skinned-draw census (Oddview model-format work) ----------------------
// The geometry probe captures only class-1 static world draws; characters are
// class 2 (ARB program with >64 locals, i.e. bone matrices) and are skipped
// there. This records one frame of CHARACTER draws instead - index count,
// attribute stride and program - so those counts can be compared against the
// record counts in the .smb mesh blobs.
typedef void (*SWSE_MatEmit)(const char* line);
// Capture the DECODED character geometry the engine feeds the GPU, to
// bin\swse_meshes.odv, for the standalone viewer. The .smb mesh blobs are
// encoded in a way not yet cracked; the engine decodes them every frame, so
// this reads the result rather than fighting the container.
int  SWSE_MeshDumpStart(char* pathOut, int pathLen);
int  SWSE_MeshDumpFinish();
// One dump frame, so every draw carries a frame number and the captured poses
// replay in order. This is what makes the capture an ANIMATION rather than a
// pile of meshes.
void SWSE_MeshDumpFrameMark();
int  SWSE_MeshDumpFrames();

// ---- hidden character parts ----
// Characters are drawn as several skinned pieces; dropping one draw removes
// that piece. Keyed on index count, which is fixed per mesh and already listed
// by 'skin show'. General on purpose - noponcho/nohat are named wrappers.
int  SWSE_MaterialsHidePart(unsigned indexCount, int on);   // returns list size
void SWSE_MaterialsHideClear();
int  SWSE_MaterialsHideList(unsigned* out, int maxOut);
int  SWSE_MaterialsHidden(unsigned indexCount);
void SWSE_SkinCensusArm();
void SWSE_SkinCensusReport(SWSE_MatEmit emit);
int  SWSE_MaterialsGlslSkipsLastFrame();   // tagged draws skipped (GLSL program)

// ---- the normal G-buffer (true vertex normals via the draw hook) ----------
void SWSE_MaterialsGBuf(int on, int w, int h);  // fed from gfx each frame
unsigned SWSE_MaterialsGBufTex();               // RGBA8 normals, a=coverage
int  SWSE_MaterialsGBufDrawsLastFrame();        // world draws captured
int  SWSE_MaterialsSkinSkipsLastFrame();        // skinned draws excluded
