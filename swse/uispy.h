// SWSE UI callback spy.
//
// Every Scaleform menu screen in the game owns a `MyCallback` class whose
// vtable the SWF calls into when a button is pressed - MainMenu, NewGame,
// Difficulty, Options, Pause and eleven more. Recovered from RTTI, they all
// have the same shape, so one hook covers the whole menu system.
//
// This exists to answer a specific question - what does choosing a difficulty
// actually do? - without guessing from static analysis. Hook the callback,
// press the button, read what ran.
//
// Hooking is done by swapping the function pointer INSIDE the vtable rather
// than patching code. The menu vtables live in .rdata and are only ever read,
// so a pointer swap is reversible and cannot race a thread mid-instruction the
// way an inline patch can.
#pragma once

// Install/remove the hooks. Returns the number of vtables hooked.
int SWSE_UiSpy(int on, char* msg, int msgLen);
int SWSE_UiSpyOn();

// Per-screen call counts, so a press can be attributed to a screen.
// Returns the number of screens; writes up to `max` names/counts.
int SWSE_UiSpyStats(const char** names, int* counts, int max);

// Reset the counters (call before pressing a button to isolate it).
void SWSE_UiSpyReset();
