// SWSE frame hook - installs an inline hook on wglSwapBuffers so SWSE runs
// once per rendered frame, right before the back buffer is presented.
#pragma once

// Frame-stall attribution: how long SWSE's own per-frame work took versus the
// whole frame. A long frame with tiny SWSE work is the game or the driver.
void SWSE_FramePerf(double* lastOurs, double* worstOurs,
                    double* lastFrame, double* worstFrame, int* stalls);
#include <windows.h>
#include "features.h"

// Starts a background thread that waits for opengl32.dll, then hooks the frame.
// Safe to call from DllMain (does no GL work itself).
void SWSE_StartFrameHook();

// Bring one SWSE system up or down while the game runs - what launch does for
// every feature features.txt switches on, and what `features <name> on|off`
// does live. Render thread only (the frame hook and console commands are).
// Does not touch the on/off flag or features.txt; the caller does that.
bool SWSE_FeatureStart(SwseFeature f, char* msg, int msgLen);
bool SWSE_FeatureStop(SwseFeature f, char* msg, int msgLen);

// For a switch set to `auto` (features.h): ask the system's own file whether
// it wants to run, and start or stop it to match. Returns the resulting state.
bool SWSE_FeatureAutoEvaluate(SwseFeature f, char* msg, int msgLen);

// The built-in switches safe mode refuses (an unknown game build,
// gamebuild.h), named the way the refusal names them; null for a switch that
// runs there. Whether or not the build is unknown - callers check that.
const char* SWSE_FeatureSafeModeRefusal(SwseFeature f);
