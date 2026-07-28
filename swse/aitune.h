// SWSE AI tuning - editable difficulty modifiers.
//
// The game's own difficulty menu sends exactly one integer (see
// research/AI_SYSTEMS.md: EASY/MEDIUM/HARD are `new_game` 0/1/2) and touches
// no AI parameter. Everything that decides how hard an encounter actually
// plays - how far NPCs see, how fast they shoot, how often they think - lives
// in per-character prefs objects that difficulty never reads.
//
// This applies multipliers to those objects, driven by a text file, so a mod
// author can build their own difficulty without recompiling anything.
//
// MULTIPLIERS, NOT ABSOLUTES. Characters differ enormously - one outlaw sees
// 50 units, another 115 - so a flat "see 80" would flatten the hand-authored
// spread that makes encounters feel different. A multiplier preserves it.
//
// EVERY APPLY IS COMPUTED FROM A STORED BASELINE, never from the current
// value. Applying twice must not compound, and turning it off must land back
// on exactly the shipped numbers.
#pragma once

// Load SWSEMods\SWSE Console\aiprefs.txt. Returns profiles parsed.
int SWSE_AiTuneLoad(char* msg, int msgLen);

// Apply a named profile ("special"), or "off" to restore the baseline.
// Returns objects modified, or -1 if the profile is unknown.
int SWSE_AiTuneApply(const char* profile, char* msg, int msgLen);

// Current profile name ("" when off), and how many objects are tuned.
const char* SWSE_AiTuneActive();
int SWSE_AiTuneCount();

// Called every frame. If aiprefs.txt names an `active` profile, this applies
// it automatically whenever a level comes up - the scan runs on a worker so
// it never stalls a frame.
void SWSE_AiTuneTick();

// Re-apply the active profile. A level load builds new prefs objects, so the
// tuning has to be re-established or it silently lapses.
void SWSE_AiTuneRefresh();
