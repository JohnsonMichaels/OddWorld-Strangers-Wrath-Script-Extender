// SWSE Console - an in-game dev console (SKSE-style) drawn by SWSE.
// Toggle with the ` / ~ key. Type commands; output scrolls in an overlay.
#pragma once
#include <windows.h>

// Called once per frame from the swap hook (GL context current). Handles the
// toggle key, text input, command execution, and overlay rendering.
void SWSE_ConsoleFrame(HDC hdc);

// True while the console is open (so other systems can ignore game hotkeys).
bool SWSE_ConsoleOpen();

// Push a line into the console log from anywhere in SWSE.
void SWSE_ConsolePrint(const char* text);

// Run a console command line programmatically. This is what makes triggers
// small: a trigger's actions are console commands, so it inherits the whole
// command set and every command added later, with no new plumbing.
void SWSE_ConsoleExec(const char* line);

// For the plugin API (plugins.cpp):
// Is `name` one of the console's built-in commands? Those names can never be
// taken by a plugin command.
bool SWSE_ConsoleIsBuiltin(const char* name);
// Run a line as a nested command - counted into the same depth as scripts,
// aliases, `exec` and `repeat`, so a plugin command that runs itself stops at
// 8 levels instead of running the stack out. 0 = ran; -11 (SWSE_E_NESTING)
// = refused, nested too deep.
int  SWSE_ConsoleExecNested(const char* line);
