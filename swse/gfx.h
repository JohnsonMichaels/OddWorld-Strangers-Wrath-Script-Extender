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
void SWSE_GfxReloadSettings();

// Capture the framebuffer to an uncompressed TGA on the next frame. Works with
// the game unfocused or occluded, unlike any outside-the-process capture -
// which is what makes unattended work possible under AgentDebugMode.
void SWSE_GfxRequestSnapshot(const char* path);     // re-read settings.txt live
// Set one settings.txt key to a value (writes the file + reloads). Returns 1 ok.
int  SWSE_GfxSetSetting(const char* key, const char* value);

// Post-process the scene while its FBO is still bound, before the game
// composites and draws UI. SEH-guarded; disables itself on fault.
void SWSE_GfxProcessSceneFBOProtected();
