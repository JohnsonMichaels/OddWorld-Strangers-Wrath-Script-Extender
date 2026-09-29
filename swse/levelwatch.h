// SWSE level watcher - "a level is up" as a signal of its own.
//
// WHY THIS EXISTS. 1.0.x told AI tuning and the self-test that a level had
// loaded by watching the hit-reaction system's actor list go from empty to
// populated. That list is only maintained while hit reactions are ON, so with
// hit reactions off - the 1.1 default - `aiprefs.txt active = <profile>`
// would silently never apply. Nothing that is not about hit reactions should
// depend on hit reactions.
//
// THE SIGNAL. The player object plus its two FORM motion objects - Steef's
// and Stranger's, at player+0x188 and +0x1A4 (vtable-checked). A level load
// rebuilds the form motions (on a warp the player object itself can come back
// at the same address), so a new player or a changed form motion is a new
// level - an "epoch". The CURRENT motion (+0xB0) is deliberately not used: it
// flips between the two forms on every Stranger/Steef change, which the first
// 1.1 builds counted as a new level (QA Q32). The body must also be missing
// for 1.5 s before the level counts as gone, so a single failed poll is not a
// load either (Q7). Everything that re-applies per level (AI tuning, player
// tuning, prefs edits, triggers, the self-test) keys off the epoch and waits a
// settle delay after it, so the level's cast has finished building first.
//
// Cheap: one getter call and a few reads, four times a second.
#pragma once

// Call once per frame (from the frame hook).
void SWSE_LevelTick();

// Increments every time a new player body appears. 0 = never seen one.
unsigned SWSE_LevelEpoch();

// Milliseconds the current body has existed; 0 when there is no level up.
unsigned SWSE_LevelAgeMs();

// True while a player body exists (in a level, not at the menu or loading).
bool SWSE_LevelUp();

// Convenience for per-level appliers: true once per epoch, the first time the
// level has been up for at least settleMs. `lastEpoch` is the caller's own
// memory of the epoch it last acted on.
bool SWSE_LevelDue(unsigned* lastEpoch, unsigned settleMs);

// The body that defines the current epoch: the player object and the form
// motion object the watcher keys on. Both 0 while no level is up. (For the
// plugin API's LEVEL_UP event.)
void SWSE_LevelBody(unsigned* player, unsigned* motion);
