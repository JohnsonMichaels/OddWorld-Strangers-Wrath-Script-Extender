// SWSE - playnpc: play as another character resident in the level.
//
// The player OBJECT is never swapped - Stranger's controller, camera,
// collision, physics, inventory and form (Stranger/Steef) stay as they are.
// What changes is what the player's actor draws (the character's geometry)
// and which animation set drives it (the character's MotionAnimConfig, with
// the gaits it lacks filled from its OWN clips), plus the rules that keep a
// non-Stranger body believable: its own walk/run cycle whatever the player's
// motion state, no double jump, no first person (the engine's camera lock).
// `playnpc <name>` spins into the character with Stranger's own
// transformation clip; `nospin` swaps instantly. A character that is not in
// memory is streamed in first, and the character's data blocks are held in
// memory (the engine's own per-frame "needed" count) while it is played, so
// walking away does not turn the player back.
//
// Default OFF: nothing is armed until `playnpc <name>` is typed. Refused on a
// game build SWSE's addresses were not measured on.
// Owner of this module: the playnpc work (see research/PLAYNPC.md).
#pragma once

// The whole console command (registered directly in the console's table).
//   playnpc                         list characters in this level (ready / load / far)
//   playnpc <name|hash> [nospin]    become that character (loading it first if needed)
//   playnpc off [nospin]            back to Stranger (cancels a pending load)
//   playnpc status | states         diagnostics
void SWSE_PlayNpcCmd(int argc, char** argv);

// Per frame, from the frame hook (the game thread, between frames). Must
// return immediately while inactive (no body swapped and no load pending).
void SWSE_PlayNpcTick();

// True while the player is played as something other than Stranger/Steef
// (including the second or so of a spin into a character).
int SWSE_PlayNpcActive();

// Name of the character currently being played, or nullptr.
const char* SWSE_PlayNpcCurrent();

// Bump the held data blocks now (a no-op unless playing, spinning into or
// loading a character). The engine's teleport runs its own scheduler ticks
// and a paging wait inside one call; a bump just before it keeps the
// character's blocks requested throughout (research/TELEPORT.md).
void SWSE_PlayNpcHoldNow();
