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
