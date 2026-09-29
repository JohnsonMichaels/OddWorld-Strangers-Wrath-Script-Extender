// SWSE menu actions without keys.
//
// Every button in the game's Flash menus ends in one call: the movie runs
// ActionScript `fscommand("<cmd>", "<arg>")`, and gameswf hands it to the
// global sink FSCommandCallback (0x60F410), which passes it to each object
// subscribed to that movie - the screen's own MyCallback among them
// (research/RE_UI_AND_ICONS.md section 1). SWSE sends the same call, so a
// menu can be driven with no input at all: CONTINUE is `goto_continue` to the
// live MainMenu movie, skipping a paused movie is `skip`.
//
// Why not keys: injected key presses need the game window to be pumping
// messages and the frame rate to catch a short press - neither holds when the
// game runs behind other windows (background mode). A direct call does not
// care where the window is.
//
// Runs on the game thread (the frame hook / console). Every address is
// measured on the Steam build, so it refuses on any other (gamebuild.h).
#pragma once

// Is a screen's movie live right now? `screen` as in menu.cpp's table
// (MainMenu, LoadGame, Pause, SaveGame, movie, ...), case-insensitive.
bool SWSE_MenuScreenLive(const char* screen);

// Send fscommand(cmd, arg) to the live movie of `screen`. Returns 1 when
// sent, 0 when that screen is not up, -1 when refused (unknown build, fault);
// msg says which.
int  SWSE_MenuFs(const char* screen, const char* cmd, const char* arg, char* msg, int msgLen);

// Every live movie and what is subscribed to it, for `menu list`.
void SWSE_MenuList(void (*emit)(const char*));
