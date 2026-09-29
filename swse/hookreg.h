// SWSE hook registry (1.1) - every place SWSE has patched code or a pointer
// table, in one list, so the question "who patched this byte?" has an answer.
//
// WHY. SWSE patches a dozen functions and tables, each system in its own way
// (PLUGIN_SYSTEM.md appendix A), and until now nothing recorded them in one
// place. Two patches on one function break each other silently: the GL hooks
// restore the original bytes on every call, erasing anything laid over them.
// With native plugins in the process that matters more, so every install site
// reports here, `hooks` lists what is live, and the plugin API's Memory.Write
// refuses a range someone has patched (SWSE_E_TAKEN).
//
// Bookkeeping only: the registry never patches anything itself, and an entry
// changes nothing about how its hook behaves. A site calls SWSE_HookNote once
// its patch is in place, and SWSE_HookForget if it ever puts the original
// back (the reversible table swaps and byte flips do; trampolines stay).
// Any thread: the frame hook is installed from its own thread at startup.
#pragma once

// Kinds, for `hooks`.
#define SWSE_HOOK_REHOOK     "unhook-rehook"   // 5-byte JMP, original restored per call
#define SWSE_HOOK_TRAMPOLINE "trampoline"      // prologue relocated, JMP at entry
#define SWSE_HOOK_VTABLE     "vtable slot"     // one pointer in a vtable swapped
#define SWSE_HOOK_IAT        "import slot"     // one pointer in an import table swapped
#define SWSE_HOOK_BYTES      "code bytes"      // instruction bytes changed in place (a jump flipped)

// Record (or refresh) a patch of `len` bytes at `addr`. owner = the SWSE
// system ("framehook", "glspy", "materials"...) or a plugin's name; label =
// what was patched ("wglSwapBuffers", "GiveAmmo").
void SWSE_HookNote(const void* addr, unsigned len, const char* owner,
                   const char* kind, const char* label);
// The patch at `addr` was put back: drop it from the list.
void SWSE_HookForget(const void* addr);
// Does any registered patch overlap [addr, addr+len)? 1 and "owner: label"
// in out (may be null), else 0.
int  SWSE_HookOwner(unsigned addr, unsigned len, char* out, int outLen);
// One line per live patch, for the `hooks` command. In safe mode (an unknown
// game build, gamebuild.h) it says so, and lists the patches refused.
void SWSE_HooksList(void (*emit)(const char* line));
int  SWSE_HookCount();
// A patch into game code that was refused because the game build is unknown
// (safe mode): `hooks` lists it, so the gated state is visible. Recorded once
// per owner and label. Any thread.
void SWSE_HookRefused(const char* owner, const char* label);
