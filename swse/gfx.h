// SWSE graphics pipeline (M3): capture the frame, run a shader, draw it back.
#pragma once
#include <windows.h>

// Called once the frame hook is live and a GL context is current.
// Returns false if GL2.0 shader entry points aren't available.
bool SWSE_GfxInit();

// Run the post-process for this frame. Takes the game's HDC so it can use the
// true window size (the game's current glViewport may be a sub-region during
// 3D/menu rendering, which does not match gl_FragCoord's full-window space).
void SWSE_GfxFrame(HDC hdc);

// True once a shader program is compiled and ready.
bool SWSE_GfxReady();

// ---- console control surface ----
void SWSE_GfxSetEnabled(int on);   // 1 = post-process ON, 0 = OFF
int  SWSE_GfxIsEnabled();          // current on/off state
// Alpha histogram of the last RGBA capture (material mask ground truth).
// Returns 0 when the capture is RGB (stamping off).
int  SWSE_GfxMaskAlphaStats(int* mn, int* mx, int* mean, int* pctNonzero);
// Live mask params (x100) + uniform locations - the ssrmask truth probe.
void SWSE_GfxMaskDebugInfo(int* stampP, int* dbgP, int* locDbg, int* locFbo,
                           int* locHas, int* haveFbo);
void SWSE_GfxReloadSettings();

// Capture the framebuffer to an uncompressed TGA on the next frame. Works with
// the game unfocused or occluded, unlike any outside-the-process capture -
// which is what makes unattended work possible under AgentDebugMode.
void SWSE_GfxRequestSnapshot(const char* path);
// Service a pending snapshot now. The frame hook calls it every frame the
// pipeline is not running, so `snap` works with graphics off.
void SWSE_GfxServiceSnapshot(HDC hdc);
// (SWSE_GfxReloadSettings re-reads graphics.txt live.)
// Set one graphics.txt key to a value (writes the file + reloads). Returns 1
// ok; 0 for a key nothing reads and -2 for a value that is not a number, in
// both cases writing nothing; -1 if the file could not be written.
int  SWSE_GfxSetSetting(const char* key, const char* value);
// 1 if `key` is a graphics.txt setting SWSE reads.
int  SWSE_GfxKnownKey(const char* key);

// Post-process the scene while its FBO is still bound, before the game
// composites and draws UI. SEH-guarded; disables itself on fault.
void SWSE_GfxProcessSceneFBOProtected();
