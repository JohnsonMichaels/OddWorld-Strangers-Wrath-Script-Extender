// SWSE - freecam: detach the view and fly the game's own FlyCamera.
//
// `freecam on` builds the game's own developer fly camera (FlyCamera, left in
// the HD build with its input code intact) from the current view and installs
// it in the camera context's cheat-camera slot, the way the beta's debug
// buttons did (Camera::CreateCheatCamera + CameraContext::SetCheatCam). The
// engine then renders from it and updates it every frame with its own input
// code: the move stick flies, the look stick turns, the triggers go up and
// down, and the stick clicks change speed. The player's camera is left as it
// was, so `freecam off` returns the view to it exactly.
//
// Stranger must not move while the view flies. The beta skipped the player's
// input handlers while a cheat camera was up (PlayerImpl::HandleInput_CheatCams
// set the flag PlayerImpl::Tick tests); HD removed that function but kept the
// test, which its cutscene gate still uses. freecam takes that same branch by
// changing one byte, and only while it is on.
//
// Console control needs no real input (agents, background mode, RT QA): `pos`,
// `look` and `speed` write the camera directly.
//
// Default OFF: nothing is created or patched until `freecam on`. Switches
// itself off on a level change, a death and a cutscene. Refused on a game build
// SWSE's addresses were not measured on. Design and evidence:
// swse/research/FREECAM.md.
#pragma once

// The console command (registered directly in the console's table).
//   freecam                      status: on/off, position, angles, speed
//   freecam on | off
//   freecam pos <x> <y> <z>      place the camera (turns freecam on)
//   freecam look <yaw> [pitch]   aim it, in degrees (turns freecam on)
//   freecam speed [n]            flying speed, world units per second
//   freecam sens [n] [updown]    mouse turning, degrees per 100 counts
//                                (times the game's own sensitivity)
//
// Turning: the mouse and the arrow keys turn the camera directly (SWSE reads
// the raw mouse counts the game receives, see input.h); the camera's own
// look-stick turning, a gamepad's right stick, still adds to it.
void SWSE_FreecamCmd(int argc, char** argv);

// Per frame, from the frame hook (the game thread, between frames). Returns
// at once while freecam is off.
void SWSE_FreecamTick();

// 1 while the fly camera is installed.
int SWSE_FreecamActive();
