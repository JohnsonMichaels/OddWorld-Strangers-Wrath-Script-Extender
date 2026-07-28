// SWSE - synthetic input injection through our own dinput8 proxy.
//
// The game reads the keyboard through DirectInput, and we ARE dinput8.dll, so
// we can hand it key presses that are indistinguishable from hardware. This is
// the only injection method that works at the main menu: keybd_event and
// SendKeys both depend on the game window holding foreground focus, and a
// fullscreen window frequently does not (see reboot.ps1's -Menu notes, where
// every OS-level method failed). We overlay our state AFTER the real call
// returns, so focus is irrelevant.
#pragma once

// Queue a key press. delayMs schedules it into the future so a caller can lay
// out a whole menu sequence in one go without blocking the render thread.
// scan is a DirectInput scancode (DIK_*), not a virtual key.
void SWSE_QueueKey(int scan, int delayMs, int holdMs);

// Map a friendly name ("down", "enter", "esc", "w") to a DIK scancode.
// Returns -1 if unknown. Accepts "0x1C" / "28" for a raw scancode too.
int  SWSE_ScanForName(const char* name);

// One-line report of what the game's input actually looks like: which devices
// it created, and which read path it uses. This is the diagnostic that tells us
// whether injection can work at all.
void SWSE_InputStatus(char* out, int outLen);

// True once the game has created a keyboard device through our proxy.
bool SWSE_InputReady();

// Install the Win32 keyboard-API probes and locate the game window. Safe to
// call every frame; it only does work once. Must run after the exe's imports
// are resolved, so it is driven from the frame hook rather than DllMain.
void SWSE_InputInstallProbes();

// Call once per frame from the frame hook: installs probes and pushes queued
// key presses out on the window-message channel.
void SWSE_InputTick();

// Keep the game simulating while it is alt-tabbed. The engine pauses on focus
// loss, which blocks working on anything else during a test and quietly
// invalidates unattended measurements. Rewrites the deactivation messages and
// answers focus queries with the game's own window.
int  SWSE_AgentDebugMode(int on, char* msg, int msgLen);
int  SWSE_AgentDebugModeOn();
// TRUE focus, as opposed to what the engine is told. Everything that decides
// whether the game may own the cursor or read the keyboard keys off this.
int  SWSE_InputReallyFocused();

// Deliver a key straight to the game's window queue. This is a separate
// channel from the DirectInput overlay and from the Win32 polling hooks -
// between the three we cover every way this engine could read a key.
void SWSE_PostKeyMessage(int scan, bool down);
