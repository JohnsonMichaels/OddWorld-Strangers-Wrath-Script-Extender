// SWSE self-test: is every feature actually operational?
//
// WHY THIS EXISTS. A change to the actor scan left hit reactions completely
// dead while the feature still reported itself ON - the hook was installed,
// the flag was true, and it detected nothing because its actor list was empty.
// It was committed on the strength of a passing performance measurement. The
// timing was verified; the FEATURE was not.
//
// So every check here asserts on evidence of WORK DONE, not on a flag:
// actors found, polls taken, programs injected, textures substituted, a matrix
// that projects a known point to where it really is. A subsystem that is
// enabled but idle must read as a failure, because that is exactly the state
// that went unnoticed.
//
// Runs automatically once a level is up, and on demand via `selftest`.
#pragma once

// Run every check now. Writes a report to the console and the log.
// Returns the number of FAILED checks (0 = everything operational).
int SWSE_SelfTestRun();

// Per-frame driver: fires the test once, shortly after a level becomes ready.
void SWSE_SelfTestTick();

// Results of the last run, for a quick status line.
void SWSE_SelfTestStats(int* passed, int* warned, int* failed, int* ranAt);
