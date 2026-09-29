// SWSE UI callback spy - see uispy.h.

#include "uispy.h"
#include "gamebuild.h"
#include "hookreg.h"    // every patch reported to the one list (`hooks`)
#include <windows.h>
#include <stdio.h>
#include <string.h>

static void LogU(const char* s) {
    char path[MAX_PATH];
    GetModuleFileNameA(GetModuleHandleA(NULL), path, MAX_PATH);
    char* sl = strrchr(path, '\\'); if (sl) *sl = 0;
    char full[MAX_PATH];
    wsprintfA(full, "%s\\swse_log.txt", path);
    FILE* f = fopen(full, "a");
    if (!f) return;
    fprintf(f, "%s\n", s);
    fclose(f);
}

// Menu callback vtables, recovered from RTTI (`.?AVMyCallback@<Screen>@@`).
// Addresses are as-linked at base 0x400000; stranger.exe is ASLR'd, so every
// one is rebased against the running module.
//
// Each MyCallback has two vtables. The PRIMARY one (the SWFCallbackInterface
// part) has the handler in slot 0:
//   SWFCallback(SWFMovie* m, const char* cmd, const char* arg), thiscall, ret 0xC
// which SWFMovie::FSCommandCallback calls for every fscommand the screen's
// movie sends. Before 1.1 this table held the OTHER vtable, 8 bytes higher (the
// RefCounted part at +4), whose slot 0 is one destructor thunk shared by every
// screen - so "callback invoked" was really an object being destroyed
// (RE_UI_AND_ICONS.md 1.2, PROVEN). `handlerVA` is the proven handler: a slot
// that holds anything else is left alone.
struct UiScreen { const char* name; unsigned vtableVA; unsigned handlerVA; };
static const UiScreen kScreens[] = {
    { "Attract",      0x785294, 0x5DD620 }, { "Collectables", 0x785370, 0x5DD920 },
    { "Controls",     0x785554, 0x5DDF40 }, { "Credits",      0x78559C, 0x5DE670 },
    { "Difficulty",   0x7855DC, 0x5DE8C0 }, { "EnterName",    0x785638, 0x5DEC20 },
    { "Extras",       0x785698, 0x5DEFF0 }, { "LevelSelect",  0x785D08, 0x5E35B0 },
    { "LoadGame",     0x785EC4, 0x5E3FC0 }, { "MainMenu",     0x785F34, 0x5E4BD0 },
    { "MovieView",    0x7860B0, 0x5E5420 }, { "NewGame",      0x7860FC, 0x5E5F30 },
    { "Options",      0x786158, 0x5E62F0 }, { "Pause",        0x7861D8, 0x5E66C0 },
    { "SaveGame",     0x786D10, 0x5ED530 }, { "Sound",        0x786E0C, 0x5EE0F0 },
    { "movie",        0x786F30, 0x5F40F0 },   // a paused Bink movie's SKIP / PLAY
    { "Store",        0x770824, 0x487880 },   // in game
    { "BountyPost",   0x76DE90, 0x471CF0 },   // in game
};
#define NSCREEN (int)(sizeof(kScreens) / sizeof(kScreens[0]))

static void*         g_orig[NSCREEN];
static unsigned*     g_slot[NSCREEN];
static volatile LONG g_hits[NSCREEN];
static bool          g_on = false;
static DWORD         g_lastAt[NSCREEN];
static char          g_lastCmd[NSCREEN][32];

// A string the game passed in, copied defensively: printable ASCII only, and a
// fault reads as "?" rather than taking the menu down.
static void CopyArg(const char* s, char* out, int n) {
    out[0] = 0;
    if (!s) return;
    __try {
        int i = 0;
        for (; i < n - 1 && s[i]; i++) out[i] = (s[i] >= 32 && s[i] < 127) ? s[i] : '?';
        out[i] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { lstrcpynA(out, "?", n); }
}

// Called from each thunk with the handler's own arguments. Kept small: this
// runs on the game thread inside the menu's command dispatch. A command that
// repeats on one screen within 250 ms (text fields ask for strings every
// frame) is counted but not logged again.
static void __cdecl UiNote(int i, const char* cmd, const char* arg) {
    if (i < 0 || i >= NSCREEN) return;
    LONG n = InterlockedIncrement(&g_hits[i]);
    char c[32], a[64];
    CopyArg(cmd, c, sizeof(c));
    CopyArg(arg, a, sizeof(a));
    DWORD now = GetTickCount();
    if (!lstrcmpA(c, g_lastCmd[i]) && now - g_lastAt[i] < 250) { g_lastAt[i] = now; return; }
    lstrcpynA(g_lastCmd[i], c, sizeof(g_lastCmd[i]));
    g_lastAt[i] = now;
    char b[200];
    wsprintfA(b, "UISPY: %s <- fscommand(\"%s\", \"%s\") (#%d)", kScreens[i].name, c, a, (int)n);
    LogU(b);
}

// One naked thunk per screen. pushad/popad hand every register and the stack
// to the original untouched. After pushad (32 bytes) the handler's stack is
// [esp+32] return, +36 SWFMovie*, +40 cmd, +44 arg; the first push reads arg,
// which moves cmd to +44 for the second.
#define UITHUNK(i)                                   \
    static __declspec(naked) void Thunk##i() {       \
        __asm { pushad }                             \
        __asm { push dword ptr [esp + 44] }          \
        __asm { push dword ptr [esp + 44] }          \
        __asm { push i }                             \
        __asm { call UiNote }                        \
        __asm { add  esp, 12 }                       \
        __asm { popad }                              \
        __asm { jmp  dword ptr [g_orig + i * 4] }    \
    }

UITHUNK(0)  UITHUNK(1)  UITHUNK(2)  UITHUNK(3)
UITHUNK(4)  UITHUNK(5)  UITHUNK(6)  UITHUNK(7)
UITHUNK(8)  UITHUNK(9)  UITHUNK(10) UITHUNK(11)
UITHUNK(12) UITHUNK(13) UITHUNK(14) UITHUNK(15)
UITHUNK(16) UITHUNK(17) UITHUNK(18)

static void* const kThunk[NSCREEN] = {
    (void*)Thunk0,  (void*)Thunk1,  (void*)Thunk2,  (void*)Thunk3,
    (void*)Thunk4,  (void*)Thunk5,  (void*)Thunk6,  (void*)Thunk7,
    (void*)Thunk8,  (void*)Thunk9,  (void*)Thunk10, (void*)Thunk11,
    (void*)Thunk12, (void*)Thunk13, (void*)Thunk14, (void*)Thunk15,
    (void*)Thunk16, (void*)Thunk17, (void*)Thunk18,
};

int SWSE_UiSpyOn() { return g_on ? 1 : 0; }

void SWSE_UiSpyReset() {
    for (int i = 0; i < NSCREEN; i++) InterlockedExchange(&g_hits[i], 0);
}

int SWSE_UiSpyStats(const char** names, int* counts, int max) {
    int n = 0;
    for (int i = 0; i < NSCREEN && n < max; i++, n++) {
        names[n]  = kScreens[i].name;
        counts[n] = (int)InterlockedCompareExchange(&g_hits[i], 0, 0);
    }
    return n;
}

int SWSE_UiSpy(int on, char* msg, int msgLen) {
    unsigned base = (unsigned)(uintptr_t)GetModuleHandleA(NULL);
    char tmp[200];
    // Swaps vtable slots at addresses measured on the Steam build.
    if (on && !SWSE_GameBuildKnown()) {
        SWSE_GameBuildRefusal("uispy", msg, msgLen);
        SWSE_HookRefused("uispy", "menu screens' MyCallback");
        return 0;
    }

    if (!on) {
        if (!g_on) { lstrcpynA(msg, "uispy already off", msgLen); return 0; }
        int n = 0;
        for (int i = 0; i < NSCREEN; i++) {
            if (!g_slot[i]) continue;
            DWORD old;
            if (VirtualProtect(g_slot[i], 4, PAGE_READWRITE, &old)) {
                *g_slot[i] = (unsigned)(uintptr_t)g_orig[i];
                VirtualProtect(g_slot[i], 4, old, &old);
                SWSE_HookForget(g_slot[i]);
                n++;
            }
            g_slot[i] = nullptr;
        }
        g_on = false;
        wsprintfA(tmp, "uispy OFF (%d vtable(s) restored)", n);
        lstrcpynA(msg, tmp, msgLen);
        return n;
    }

    if (g_on) { lstrcpynA(msg, "uispy already on", msgLen); return 0; }

    int n = 0, bad = 0;
    for (int i = 0; i < NSCREEN; i++) {
        unsigned* slot = (unsigned*)(uintptr_t)(base + (kScreens[i].vtableVA - 0x400000));
        unsigned cur = 0;
        // The slot must hold exactly the proven handler before it is written.
        // A wrong table or rebase would otherwise corrupt whatever lives there,
        // and the menus would fail in a way that looks nothing like a bad hook.
        __try { cur = *slot; }
        __except (EXCEPTION_EXECUTE_HANDLER) { bad++; continue; }
        if (cur != base + (kScreens[i].handlerVA - 0x400000)) { bad++; continue; }

        DWORD old;
        if (!VirtualProtect(slot, 4, PAGE_READWRITE, &old)) { bad++; continue; }
        g_orig[i] = (void*)(uintptr_t)cur;
        g_slot[i] = slot;
        *slot = (unsigned)(uintptr_t)kThunk[i];
        VirtualProtect(slot, 4, old, &old);
        char lbl[48];
        _snprintf_s(lbl, sizeof(lbl), _TRUNCATE, "%s::MyCallback", kScreens[i].name);
        SWSE_HookNote(slot, 4, "uispy", SWSE_HOOK_VTABLE, lbl);
        n++;
    }
    g_on = (n > 0);
    SWSE_UiSpyReset();
    if (bad) wsprintfA(tmp, "uispy ON - %d menu callback(s) hooked, %d left alone (slot did not hold the proven handler)", n, bad);
    else     wsprintfA(tmp, "uispy ON - %d menu callback(s) hooked", n);
    lstrcpynA(msg, tmp, msgLen);
    LogU(tmp);
    return n;
}
