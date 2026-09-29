// SWSE GPU compute probe.
//
// Answers one question with evidence rather than version strings: can this
// game's ancient GL context actually run modern compute shaders, and how
// much ray-triangle math per second does the GPU deliver through it?
//
// That number decides whether an in-engine ray tracer is feasible at all.
// The probe is read-only with respect to the game: it creates its own
// objects, restores the shader state it found, and deletes everything.
#pragma once

// Where each report line goes (the console, which relays to the mailbox).
typedef void (*SWSE_GpuEmit)(const char* line);

// Run the full escalation: version -> entry points -> limits -> trivial
// dispatch -> ray-triangle correctness -> throughput. Safe to call any
// number of times; must be called with a GL context current (the console
// runs at swap time, so command handlers qualify).
void SWSE_GpuProbe(SWSE_GpuEmit emit);
