// SWSE UI callback spy - see uispy.h.

#include "uispy.h"
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
// one is rebased against the running module. Each screen has two vtables - the
// callback object and its base - and slot 0 of the first is the invoke handler.
struct UiScreen { const char* name; unsigned vtableVA; };
static const UiScreen kScreens[] = {
    { "Attract",      0x78529C }, { "Collectables", 0x785378 },
    { "Controls",     0x78555C }, { "Credits",      0x7855A4 },
    { "Difficulty",   0x7855E4 }, { "EnterName",    0x785640 },
    { "Extras",       0x7856A0 }, { "LevelSelect",  0x785D10 },
    { "LoadGame",     0x785ECC }, { "MainMenu",     0x785F3C },
    { "MovieView",    0x7860B8 }, { "NewGame",      0x786104 },
    { "Options",      0x786160 }, { "Pause",        0x7861E0 },
    { "SaveGame",     0x786D18 }, { "Sound",        0x786E14 },
};
#define NSCREEN (int)(sizeof(kScreens) / sizeof(kScreens[0]))

static void*         g_orig[NSCREEN];
static unsigned*     g_slot[NSCREEN];
static volatile LONG g_hits[NSCREEN];
static bool          g_on = false;

// Called from each thunk. Kept tiny: this runs on the game's UI thread with
// every register already saved, and anything slow here would be felt as menu
// lag rather than seen as a bug.
static void __cdecl UiNote(int i) {
    if (i < 0 || i >= NSCREEN) return;
    LONG n = InterlockedIncrement(&g_hits[i]);
    char b[160];
    wsprintfA(b, "UISPY: %s callback invoked (#%d)", kScreens[i].name, (int)n);
    LogU(b);
}

// One naked thunk per screen. Naked + pushad/popad means the real signature of
// the callback never has to be known: every register and the stack are handed
// to the original untouched, and we only observe that it happened.
#define UITHUNK(i)                                   \
    static __declspec(naked) void Thunk##i() {       \
        __asm { pushad }                             \
        __asm { push i }                             \
        __asm { call UiNote }                        \
        __asm { add  esp, 4 }                        \
        __asm { popad }                              \
        __asm { jmp  dword ptr [g_orig + i * 4] }    \
    }

UITHUNK(0)  UITHUNK(1)  UITHUNK(2)  UITHUNK(3)
UITHUNK(4)  UITHUNK(5)  UITHUNK(6)  UITHUNK(7)
UITHUNK(8)  UITHUNK(9)  UITHUNK(10) UITHUNK(11)
UITHUNK(12) UITHUNK(13) UITHUNK(14) UITHUNK(15)

static void* const kThunk[NSCREEN] = {
    (void*)Thunk0,  (void*)Thunk1,  (void*)Thunk2,  (void*)Thunk3,
    (void*)Thunk4,  (void*)Thunk5,  (void*)Thunk6,  (void*)Thunk7,
    (void*)Thunk8,  (void*)Thunk9,  (void*)Thunk10, (void*)Thunk11,
    (void*)Thunk12, (void*)Thunk13, (void*)Thunk14, (void*)Thunk15,
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

    if (!on) {
        if (!g_on) { lstrcpynA(msg, "uispy already off", msgLen); return 0; }
        int n = 0;
        for (int i = 0; i < NSCREEN; i++) {
            if (!g_slot[i]) continue;
            DWORD old;
            if (VirtualProtect(g_slot[i], 4, PAGE_READWRITE, &old)) {
                *g_slot[i] = (unsigned)(uintptr_t)g_orig[i];
                VirtualProtect(g_slot[i], 4, old, &old);
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
        // Verify the slot holds a plausible code pointer into this module
        // before writing it. A wrong rebase would otherwise corrupt whatever
        // happens to live there, and the menus would fail in a way that looks
        // nothing like a bad hook.
        __try { cur = *slot; }
        __except (EXCEPTION_EXECUTE_HANDLER) { bad++; continue; }
        if (cur < base + 0x1000 || cur > base + 0x350000) { bad++; continue; }

        DWORD old;
        if (!VirtualProtect(slot, 4, PAGE_READWRITE, &old)) { bad++; continue; }
        g_orig[i] = (void*)(uintptr_t)cur;
        g_slot[i] = slot;
        *slot = (unsigned)(uintptr_t)kThunk[i];
        VirtualProtect(slot, 4, old, &old);
        n++;
    }
    g_on = (n > 0);
    SWSE_UiSpyReset();
    wsprintfA(tmp, "uispy ON - %d menu callback(s) hooked%s", n,
              bad ? " (some slots rejected)" : "");
    lstrcpynA(msg, tmp, msgLen);
    LogU(tmp);
    return n;
}
