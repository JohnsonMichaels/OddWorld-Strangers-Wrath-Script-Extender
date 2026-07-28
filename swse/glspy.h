// SWSE GL reconnaissance: hook wglGetProcAddress to observe which GL extension
// functions the game resolves (reveals its render-to-texture / depth pipeline).
#pragma once
#include <windows.h>

// Install the wglGetProcAddress hook. Call once opengl32.dll is loaded.
void SWSE_InstallGLSpy();

// The GL texture id of the scene depth attachment the game renders into
// (detected live by watching glBindFramebufferEXT). 0 until found.
unsigned int SWSE_SceneDepthTex();

// The game's real camera, read from its projection matrix. Returns false until
// a perspective frustum has been seen. near/far are what depth linearisation
// needs; guessing them is what made depth-based GI useless.
bool SWSE_SceneProjection(float* nearZ, float* farZ, float* fovY);

// Find the engine's camera frustum struct in memory by value signature
// (zNear/zFar/fovRad, cross-checked against tanHalfFov). Returns the number of
// candidates written.
int  SWSE_ScanCameraFrustum(unsigned* addrs, float* nears, float* fars,
                            float* fovs, int maxOut);

// Scan and adopt the widest frustum as the scene camera.
bool SWSE_AdoptScannedCamera();

// Enumerate every depth texture the game has attached to an FBO, and pin one.
// They differ in what geometry they contain (notably the first-person weapon),
// so which one is sampled changes the result. Pass 0 to un-pin.
int  SWSE_DepthTexList(unsigned* out, int maxOut);
void SWSE_ForceSceneDepthTex(unsigned id);

// Pick the depth texture that actually contains scene geometry, rejecting the
// empty (all-far-plane) buffers the "last bound wins" rule used to select.
// Requires a current GL context; call from the frame hook.
bool SWSE_AutoPickDepthTex();

// Log the FBO bind order for the next N frames, to locate the point where the
// scene is finished but the UI has not been composited yet.
void SWSE_TraceFBO(int frames);
void SWSE_TraceFBOFrameMark();

// How many HD replacement textures are installed, and how many have actually
// been substituted so far. The second number is what proves the pipeline runs:
// files on disk that never load look identical to a broken hook.
//
// `failed` counts .oft files the game asked for and we could NOT load - a
// truncated or malformed replacement. Those fall back to the vanilla texture,
// so they are invisible in play; without a counter a bad batch ships silently.
void SWSE_HdStats(int* available, int* loaded, int* failed);

// Fingerprints of the replacements that failed to load, so a bad file can be
// named and re-packed. Returns how many were written into `out`.
int SWSE_HdFailures(unsigned* out, int max);

// The game's finished scene-colour texture and its size, tracked live. This is
// the render target the pre-UI pass processes: the pass immediately before
// fbo=0 is depth-only, so the bound framebuffer there has no colour at all.
unsigned SWSE_SceneColorTex();
int      SWSE_SceneColorW();
int      SWSE_SceneColorH();
