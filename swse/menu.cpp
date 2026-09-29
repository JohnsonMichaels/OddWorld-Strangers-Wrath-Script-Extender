// SWSE menu actions without keys - see menu.h.

#include "menu.h"
#include "gamebuild.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

// Link-time VA -> address in the running (ASLR'd) exe.
static unsigned P(unsigned va) { return (unsigned)(uintptr_t)GetModuleHandleA(NULL) + (va - 0x400000u); }

// Each screen's MyCallback PRIMARY vtable (the SWFCallbackInterface part,
// slot 0 = the handler) - RE_UI_AND_ICONS.md table 1.2, PROVEN. "movie" is
// the MovieCancelCallback a paused Bink movie subscribes.
struct Screen { const char* name; unsigned vt; };
static const Screen kScreens[] = {
    { "MainMenu", 0x785F34 },   { "LoadGame", 0x785EC4 },   { "Pause", 0x7861D8 },
    { "movie", 0x786F30 },      { "SaveGame", 0x786D10 },   { "NewGame", 0x7860FC },
    { "Difficulty", 0x7855DC }, { "Options", 0x786158 },    { "Extras", 0x785698 },
    { "Controls", 0x785554 },   { "Sound", 0x786E0C },      { "LevelSelect", 0x785D08 },
    { "EnterName", 0x785638 },  { "MovieView", 0x7860B0 },  { "Collectables", 0x785370 },
};
#define NSCREEN (int)(sizeof(kScreens) / sizeof(kScreens[0]))

#define VA_MOVIES        0xA3D1C8   // SWF::s_movies data pointer
#define VA_MOVIES_N      0xA3D1D0   // ...and count
#define VA_HANDLES       0x9D55F0   // weak-handle table: {u32 RefCounted*; u16 gen}, stride 6
#define VA_FSCOMMAND     0x60F410   // cdecl FSCommandCallback(movie_interface*, cmd, arg)
#define MOV_INTERFACE    0x0C       // SWFMovie: its gameswf movie_interface
#define MOV_SUBS         0x10       // SWFMovie: subscriber handles (u32*)
#define MOV_SUBS_N       0x18       // SWFMovie: subscriber count

// The complete object behind subscriber handle h, or 0. The handle table
// stores the RefCounted sub-object; the RTTI locator's offset walks back to
// the object's start, whose first dword is its primary vtable.
static unsigned SubscriberObject(unsigned h) {
    unsigned ent = P(VA_HANDLES) + 6 * (h & 0xFFFF);
    if (*(unsigned short*)(ent + 4) != (unsigned short)(h >> 16)) return 0;
    unsigned rc = *(unsigned*)ent;
    if (!rc) return 0;
    unsigned col = *(unsigned*)(*(unsigned*)rc - 4);
    return rc - *(unsigned*)(col + 4);
}

static const char* ClassName(unsigned obj, char* out, int outLen) {
    out[0] = 0;
    __try {
        unsigned col = *(unsigned*)(*(unsigned*)obj - 4);
        const char* n = (const char*)(*(unsigned*)(col + 12) + 8);   // ".?AVName@@"
        if (n[0] == '.' && n[1] == '?' && n[2] == 'A') n += 4;
        int i = 0;
        for (; n[i] && i < outLen - 1; i++) out[i] = n[i];
        out[i] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { lstrcpynA(out, "?", outLen); }
    return out;
}

// The live movie with a subscriber whose primary vtable is `vtVA`, or 0.
static unsigned FindMovie(unsigned vtVA) {
    unsigned want = P(vtVA);
    __try {
        unsigned n = *(unsigned*)P(VA_MOVIES_N);
        unsigned* movies = *(unsigned**)P(VA_MOVIES);
        if (!movies || n > 256) return 0;
        for (unsigned i = 0; i < n; i++) {
            unsigned m = movies[i];
            if (!m) continue;
            unsigned ns = *(unsigned*)(m + MOV_SUBS_N);
            unsigned* subs = *(unsigned**)(m + MOV_SUBS);
            if (!subs || ns > 64) continue;
            for (unsigned j = 0; j < ns; j++) {
                unsigned obj = SubscriberObject(subs[j]);
                if (obj && *(unsigned*)obj == want) return m;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}

static const Screen* FindScreen(const char* name) {
    for (int i = 0; i < NSCREEN; i++)
        if (!lstrcmpiA(name, kScreens[i].name)) return &kScreens[i];
    return nullptr;
}

bool SWSE_MenuScreenLive(const char* screen) {
    if (!SWSE_GameBuildKnown()) return false;
    const Screen* s = FindScreen(screen);
    return s && FindMovie(s->vt) != 0;
}

static int Dispatch(unsigned movie, const char* cmd, const char* arg) {
    typedef void (__cdecl *FsFn)(void*, const char*, const char*);
    __try {
        void* mi = *(void**)(movie + MOV_INTERFACE);
        ((FsFn)P(VA_FSCOMMAND))(mi, cmd, arg ? arg : "");
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    return 1;
}

int SWSE_MenuFs(const char* screen, const char* cmd, const char* arg, char* msg, int msgLen) {
    if (!SWSE_GameBuildKnown()) {
        SWSE_GameBuildRefusal("menu", msg, msgLen);
        return -1;
    }
    const Screen* s = FindScreen(screen);
    if (!s) { _snprintf_s(msg, msgLen, _TRUNCATE, "no screen called '%s' (`menu list` shows the live ones)", screen); return -1; }
    unsigned m = FindMovie(s->vt);
    if (!m) { _snprintf_s(msg, msgLen, _TRUNCATE, "%s is not on screen right now", s->name); return 0; }
    int r = Dispatch(m, cmd, arg);
    if (r < 0) { _snprintf_s(msg, msgLen, _TRUNCATE, "%s: fscommand '%s' faulted", s->name, cmd); return -1; }
    _snprintf_s(msg, msgLen, _TRUNCATE, "%s <- fscommand(\"%s\", \"%s\")", s->name, cmd, arg ? arg : "");
    return 1;
}

void SWSE_MenuList(void (*emit)(const char*)) {
    char b[300];
    if (!SWSE_GameBuildKnown()) { SWSE_GameBuildRefusal("menu", b, sizeof(b)); emit(b); return; }
    __try {
        unsigned n = *(unsigned*)P(VA_MOVIES_N);
        unsigned* movies = *(unsigned**)P(VA_MOVIES);
        _snprintf_s(b, sizeof(b), _TRUNCATE, "%u live Flash movie(s):", n);
        emit(b);
        for (unsigned i = 0; movies && i < n && i < 64; i++) {
            unsigned m = movies[i];
            if (!m) continue;
            unsigned ns = *(unsigned*)(m + MOV_SUBS_N);
            unsigned* subs = *(unsigned**)(m + MOV_SUBS);
            char line[300];
            int used = _snprintf_s(line, sizeof(line), _TRUNCATE, "  %u: movie %08X  ", i, m);
            for (unsigned j = 0; subs && j < ns && j < 8; j++) {
                unsigned obj = SubscriberObject(subs[j]);
                if (!obj) continue;
                char cn[64];
                const char* screen = nullptr;
                for (int k = 0; k < NSCREEN; k++)
                    if (*(unsigned*)obj == P(kScreens[k].vt)) screen = kScreens[k].name;
                int w = _snprintf_s(line + used, sizeof(line) - used, _TRUNCATE, "%s%s ",
                                    screen ? screen : ClassName(obj, cn, sizeof(cn)),
                                    screen ? "*" : "");
                if (w > 0) used += w;
            }
            emit(line);
        }
        emit("  (* = a screen `menu fs <screen> <cmd> [arg]` can drive)");
    } __except (EXCEPTION_EXECUTE_HANDLER) { emit("menu list: faulted reading the movie list"); }
}
