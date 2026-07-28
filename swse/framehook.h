// SWSE frame hook - installs an inline hook on wglSwapBuffers so SWSE runs
// once per rendered frame, right before the back buffer is presented.
#pragma once

// Frame-stall attribution: how long SWSE's own per-frame work took versus the
// whole frame. A long frame with tiny SWSE work is the game or the driver.
void SWSE_FramePerf(double* lastOurs, double* worstOurs,
                    double* lastFrame, double* worstFrame, int* stalls);
#include <windows.h>

// Starts a background thread that waits for opengl32.dll, then hooks the frame.
// Safe to call from DllMain (does no GL work itself).
void SWSE_StartFrameHook();
