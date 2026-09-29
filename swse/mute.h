// SWSE audio mute - the game's own Windows audio session (the per-app switch
// in Volume Mixer), not the game's volume settings.
//
// Background mode (SWSE_AGENTDEBUG=1) mutes at the first frame: agents drive
// the game while the owner works in other apps, and its sound effects playing
// behind everything was the first complaint. `mute on|off` is the console
// command; unmuting there, or in Volume Mixer, undoes it.
#pragma once

// Mute (1) or unmute (0) this process's audio on every active playback
// device. Runs on a short-lived thread of its own, so COM never meets the
// game's threads. wait = milliseconds to wait for the result (0 = don't).
// Returns sessions changed, 0 if none (or not waited for), -1 on failure.
int SWSE_SetGameMute(int mute, unsigned waitMs);

// 1 if SWSE last muted the game, 0 if it unmuted or never touched it.
int SWSE_GameMuted();
