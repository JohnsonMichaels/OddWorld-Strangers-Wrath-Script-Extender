// SWSE - DirectInput8 proxy with synthetic key injection.
//
// dllmain.cpp used to forward DirectInput8Create straight to the real DLL with
// a linker pragma, so SWSE never saw a single input call. Here we implement the
// export ourselves: load the real dinput8, call through, then patch the vtables
// of the returned COM interfaces so every device read passes through us.
//
// Why vtable patching and not wrapper objects: the game keeps raw interface
// pointers and passes them around, so a wrapper would have to be perfectly
// transparent for the lifetime of the process. Patching the vtable in place
// leaves the game's pointers valid and hooks every instance of that class at
// once, which is what we want anyway.
//
// We deliberately declare the DirectInput structs by hand rather than including
// <dinput.h>: it drags in a lib dependency for the GUID symbols and the build
// is a bare `cl` invocation with no DirectX SDK on the include path.

#include <windows.h>
#include <stdio.h>
#include "input.h"
#include "hookreg.h"    // every patch reported to the one list (`hooks`)

// Local C-string logger, same shape as the one in gfx.cpp: no C++ objects, so
// it stays safe to call from inside a hook on the game's input thread.
static void SWSE_Log(const char* s) {
    char path[MAX_PATH];
    GetModuleFileNameA(GetModuleHandleA(NULL), path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) *slash = 0;
    lstrcatA(path, "\\swse_log.txt");
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           0, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    SetFilePointer(h, 0, 0, FILE_END);
    WriteFile(h, s, lstrlenA(s), &w, 0);
    WriteFile(h, "\r\n", 2, &w, 0);
    CloseHandle(h);
}

// ---- minimal DirectInput ABI --------------------------------------------
// DIDEVICEOBJECTDATA as of DIRECTINPUT_VERSION 0x0800 (20 bytes on x86).
typedef struct {
    DWORD     dwOfs;
    DWORD     dwData;
    DWORD     dwTimeStamp;
    DWORD     dwSequence;
    UINT_PTR  uAppData;
} SWSE_DIDATA;

#define DIGDD_PEEK 0x00000001

// vtable slots. IDirectInput8: 0-2 IUnknown, 3 CreateDevice.
// IDirectInputDevice8: 7 Acquire, 9 GetDeviceState, 10 GetDeviceData.
#define VT_DI_CREATEDEVICE   3
#define VT_DEV_GETSTATE      9
#define VT_DEV_GETDATA      10

static const GUID kSysKeyboard =
    { 0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00 } };
static const GUID kSysMouse =
    { 0x6F1D2B60, 0xD5A0, 0x11CF, { 0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00 } };

typedef HRESULT (WINAPI *PFN_Create)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
typedef HRESULT (WINAPI *PFN_CreateDevice)(void*, REFGUID, void**, LPUNKNOWN);
typedef HRESULT (WINAPI *PFN_GetState)(void*, DWORD, void*);
typedef HRESULT (WINAPI *PFN_GetData)(void*, DWORD, SWSE_DIDATA*, DWORD*, DWORD);

static PFN_CreateDevice o_CreateDevice = 0;
static PFN_GetState     o_GetState     = 0;
static PFN_GetData      o_GetData      = 0;

// ---- what the game is actually doing (diagnostics) -----------------------
static volatile LONG g_nKeyboards = 0, g_nMice = 0, g_nOther = 0;
static volatile LONG g_stateCalls = 0, g_dataCalls = 0;
static volatile LONG g_lastStateCb = 0;

// Devices created with GUID_SysKeyboard. The mouse and keyboard may or may not
// share a vtable, so identify the keyboard by interface pointer rather than
// trusting that only one class got patched.
static void*  g_kbd[8];
static int    g_kbdCount = 0;
static bool IsKeyboard(void* self) {
    for (int i = 0; i < g_kbdCount; i++) if (g_kbd[i] == self) return true;
    return false;
}

// ---- scheduled key presses ----------------------------------------------
// A press occupies a time window [downAt, upAt). Windows let a caller lay out
// "Down now, Down in 400ms, Enter in 800ms" in one call, with no blocking and
// no per-frame tick - the read hook just asks which windows are open right now.
//
// A window alone is not enough when frames are slow. Behind other windows (the
// background mode agents use) the driver throttles the game to ~11 fps, so a
// 90 ms press could open and close between two frames and never be posted -
// which is why `continue` did nothing on a minimized game. So presses are
// ADVANCED only by the per-frame tick, the channel the game actually reads
// (its message queue): a press starts at the first tick at or after its time,
// is held for its full duration from THAT tick, and is released at a later
// tick. A second press of the same key waits until a tick has posted the first
// one's release, so Down, Down never merges into one long Down. Every other
// reader (DirectInput, GetAsyncKeyState) sees the same state, read-only.
#define MAX_PRESSES 64
struct Press { DWORD downAt, upAt; BYTE scan; BYTE live; BYTE seen; };
static Press g_press[MAX_PRESSES];
static LONG  g_tickN = 0;                  // SWSE_InputTick advances
static LONG  g_upTick[256];                // per key: the tick that released it last
static CRITICAL_SECTION g_lock;
static bool g_lockReady = false;

static void LockInit() {
    if (!g_lockReady) { InitializeCriticalSection(&g_lock); g_lockReady = true; }
}

void SWSE_QueueKey(int scan, int delayMs, int holdMs) {
    if (scan <= 0 || scan > 255) return;
    if (holdMs < 16) holdMs = 16;          // must survive at least one frame
    LockInit();
    EnterCriticalSection(&g_lock);
    DWORD now = GetTickCount();
    for (int i = 0; i < MAX_PRESSES; i++) {
        if (g_press[i].live) continue;                            // still in use
        g_press[i].scan   = (BYTE)scan;
        g_press[i].downAt = now + (DWORD)delayMs;
        g_press[i].upAt   = now + (DWORD)delayMs + (DWORD)holdMs;
        g_press[i].live   = 1;
        g_press[i].seen   = 0;
        break;
    }
    LeaveCriticalSection(&g_lock);
}

// Fill a 256-byte DIK map with the keys we are currently holding down.
// advance = true only from SWSE_InputTick (once per frame): it starts and
// releases presses. Everyone else reads the state it left, unchanged.
static void SynthState(BYTE* out, bool advance = false) {
    memset(out, 0, 256);
    if (!g_lockReady) return;
    EnterCriticalSection(&g_lock);
    DWORD now = GetTickCount();
    LONG tick = advance ? ++g_tickN : g_tickN;
    for (int i = 0; i < MAX_PRESSES; i++) {
        Press& p = g_press[i];
        if (!p.live) continue;
        if (advance) {
            if (!p.seen) {
                if (now < p.downAt) continue;                 // not yet
                if (g_upTick[p.scan] >= tick) continue;       // release not posted yet
                p.seen = 1;
                p.upAt = now + (p.upAt - p.downAt);           // hold from THIS tick
            } else if (now >= p.upAt) {
                p.live = 0;
                g_upTick[p.scan] = tick;                      // released at this tick
                continue;
            }
        }
        if (p.seen) out[p.scan] = 0x80;
    }
    LeaveCriticalSection(&g_lock);
}

// ---- hooks ---------------------------------------------------------------
// Immediate mode: OR our held keys into the state the real device returned.
static HRESULT WINAPI My_GetState(void* self, DWORD cb, void* data) {
    HRESULT hr = o_GetState(self, cb, data);
    InterlockedIncrement(&g_stateCalls);
    g_lastStateCb = (LONG)cb;
    // DirectInput drops device acquisition when the window loses focus, so this
    // returns DIERR_INPUTLOST/NOTACQUIRED and the injection below never runs -
    // which is why menu navigation stopped working under AgentDebugMode while
    // the in-game console still did. Substitute an empty state so synthetic
    // keys still reach the game; the real device is genuinely gone, and its
    // keys belong to whatever app the user is actually in.
    if (FAILED(hr) && SWSE_AgentDebugModeOn() && !SWSE_InputReallyFocused() &&
        data && cb == 256 && IsKeyboard(self)) {
        memset(data, 0, 256);
        hr = S_OK;
    }
    // A keyboard's immediate state is a 256-byte DIK array. Check both the
    // size and the device identity so we never scribble on a joystick struct.
    if (SUCCEEDED(hr) && data && cb == 256 && IsKeyboard(self)) {
        BYTE synth[256]; SynthState(synth);
        BYTE* buf = (BYTE*)data;
        for (int i = 0; i < 256; i++) if (synth[i]) buf[i] |= 0x80;
    }
    return hr;
}

// Buffered mode: emit press/release events on the edges of our synthetic state.
// Menus commonly read buffered data so a tap registers exactly once.
static BYTE g_prevSynth[256];
static HRESULT WINAPI My_GetData(void* self, DWORD cb, SWSE_DIDATA* rgdod,
                                 DWORD* pdwInOut, DWORD flags) {
    // Capacity is an in/out parameter: on return it holds the count actually
    // written, so the buffer size has to be saved before calling through.
    DWORD cap = (pdwInOut ? *pdwInOut : 0);
    HRESULT hr = o_GetData(self, cb, rgdod, pdwInOut, flags);
    InterlockedIncrement(&g_dataCalls);
    // Same acquisition problem as the immediate path: unfocused, the real read
    // fails and buffered menu input would never see our keys.
    if (FAILED(hr) && SWSE_AgentDebugModeOn() && !SWSE_InputReallyFocused() &&
        IsKeyboard(self) && pdwInOut) {
        *pdwInOut = 0;
        hr = S_OK;
    }
    if (FAILED(hr) || !IsKeyboard(self) || !pdwInOut) return hr;

    BYTE cur[256]; SynthState(cur);
    // A NULL buffer means the caller is only counting pending events.
    if (!rgdod) {
        for (int i = 0; i < 256; i++) if (cur[i] != g_prevSynth[i]) (*pdwInOut)++;
        return hr;
    }

    DWORD have = *pdwInOut;
    for (int i = 0; i < 256 && have < cap; i++) {
        if (cur[i] == g_prevSynth[i]) continue;
        // Write through the caller's stride, not sizeof(our struct).
        SWSE_DIDATA* slot = (SWSE_DIDATA*)((BYTE*)rgdod + (size_t)have * cb);
        memset(slot, 0, cb);
        slot->dwOfs       = (DWORD)i;
        slot->dwData      = cur[i] ? 0x80 : 0;
        slot->dwTimeStamp = GetTickCount();
        slot->dwSequence  = 0;
        have++;
    }
    *pdwInOut = have;
    // PEEK means "look but don't consume", so only commit the edge on a real read.
    if (!(flags & DIGDD_PEEK)) memcpy(g_prevSynth, cur, 256);
    return hr;
}

static void* PatchSlot(void* iface, int index, void* hook, const char* label) {
    void** vt = *(void***)iface;
    DWORD old;
    if (!VirtualProtect(&vt[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return 0;
    void* orig = vt[index];
    if (orig == hook) { VirtualProtect(&vt[index], sizeof(void*), old, &old); return 0; }
    vt[index] = hook;
    VirtualProtect(&vt[index], sizeof(void*), old, &old);
    SWSE_HookNote(&vt[index], sizeof(void*), "input", SWSE_HOOK_VTABLE, label);
    return orig;
}

static HRESULT WINAPI My_CreateDevice(void* self, REFGUID rguid, void** out, LPUNKNOWN outer) {
    HRESULT hr = o_CreateDevice(self, rguid, out, outer);
    if (FAILED(hr) || !out || !*out) return hr;

    bool isKbd = (memcmp(&rguid, &kSysKeyboard, sizeof(GUID)) == 0);
    bool isMouse = (memcmp(&rguid, &kSysMouse, sizeof(GUID)) == 0);
    if (isKbd) {
        InterlockedIncrement(&g_nKeyboards);
        if (g_kbdCount < 8) g_kbd[g_kbdCount++] = *out;
    } else if (isMouse) InterlockedIncrement(&g_nMice);
    else InterlockedIncrement(&g_nOther);

    // Patch once per vtable; a second device of the same class reuses it.
    void* p = PatchSlot(*out, VT_DEV_GETSTATE, (void*)My_GetState, "IDirectInputDevice8::GetDeviceState");
    if (p) o_GetState = (PFN_GetState)p;
    p = PatchSlot(*out, VT_DEV_GETDATA, (void*)My_GetData, "IDirectInputDevice8::GetDeviceData");
    if (p) o_GetData = (PFN_GetData)p;

    char msg[160];
    wsprintfA(msg, "input: device created (%s) - read hooks %s",
              isKbd ? "keyboard" : isMouse ? "mouse" : "other",
              (o_GetState || o_GetData) ? "installed" : "FAILED");
    SWSE_Log(msg);
    return hr;
}

// ---- finding the keyboard path the game actually uses --------------------
// Measured: the game calls DirectInput8Create but creates ZERO devices, so it
// does not read the keyboard through DirectInput at all (DI is its gamepad
// path). The remaining candidates are the Win32 polling APIs and the window
// message loop. Hook the exe's import table for each so we can both COUNT the
// calls (which tells us the real path) and answer them with our synthetic
// state (which makes injection work on whichever path wins).
static volatile LONG g_asyncCalls = 0, g_keyStateCalls = 0, g_kbStateCalls = 0;

typedef SHORT (WINAPI *PFN_GetAsyncKeyState)(int);
typedef SHORT (WINAPI *PFN_GetKeyState)(int);
typedef BOOL  (WINAPI *PFN_GetKeyboardState)(PBYTE);
static PFN_GetAsyncKeyState o_GetAsyncKeyState = 0;
static PFN_GetKeyState      o_GetKeyState      = 0;
static PFN_GetKeyboardState o_GetKeyboardState = 0;

// Is the virtual key vk currently held by one of our scheduled presses?
static bool SynthHasVK(int vk) {
    BYTE s[256];
    SynthState(s);
    for (int scan = 1; scan < 256; scan++) {
        if (!s[scan]) continue;
        // DIK scancodes above 0x80 are the extended (E0-prefixed) keys.
        UINT mapped = MapVirtualKeyA(scan & 0x7F, 1 /*MAPVK_VSC_TO_VK*/);
        if (scan & 0x80) {
            switch (scan) {
                case 0xC8: mapped = VK_UP;    break;
                case 0xD0: mapped = VK_DOWN;  break;
                case 0xCB: mapped = VK_LEFT;  break;
                case 0xCD: mapped = VK_RIGHT; break;
                case 0x9C: mapped = VK_RETURN;break;
                case 0xC7: mapped = VK_HOME;  break;
                case 0xCF: mapped = VK_END;   break;
                case 0xC9: mapped = VK_PRIOR; break;
                case 0xD1: mapped = VK_NEXT;  break;
                case 0xD2: mapped = VK_INSERT;break;
                case 0xD3: mapped = VK_DELETE;break;
            }
        }
        if ((int)mapped == vk) return true;
    }
    return false;
}

// While genuinely unfocused, real key state belongs to whatever app the user is
// actually working in - typing in a modeller must not drive the game. Synthetic
// (SWSE-injected) keys still pass, so unattended testing keeps working.
int SWSE_InputReallyFocused();

// Real key state is suppressed ONLY while AgentDebugMode is on AND the game is
// genuinely unfocused. The first version omitted the AgentDebugMode test, so a
// wrong focus reading could swallow the user's input even with the feature
// switched off - which defeats the whole point of having a toggle. With it off,
// these must behave exactly as the unhooked functions do.
// DEBOUNCED. Focus is tracked from window messages, and Windows delivers
// transient WM_KILLFOCUS / WM_ACTIVATE(WA_INACTIVE) for things that are not
// really the user leaving: a notification, a tooltip, another window blipping
// to the foreground. Suppressing input the instant one arrives made the player
// stop dead mid-movement and then recover a moment later on his own - reported
// as "sometimes he just stops, then the keys work again".
//
// A genuine alt-tab lasts far longer than this window, so waiting a moment
// before believing a focus loss costs nothing and removes the false ones. The
// foreground window is also cross-checked, because that is authoritative where
// a stray message is not.
#define FOCUS_LOSS_GRACE_MS 400

// Declared later in the file; needed here.
static HWND g_wnd;
typedef HWND (WINAPI* PFN_GetWnd)(void);
static PFN_GetWnd o_GetForegroundWindow;
extern volatile LONG g_focusLostAt;

static bool SuppressRealInput() {
    if (!SWSE_AgentDebugModeOn()) return false;
    if (SWSE_InputReallyFocused()) return false;
    // Still the foreground window? Then the message lied; focus is not lost.
    if (g_wnd && o_GetForegroundWindow && o_GetForegroundWindow() == g_wnd)
        return false;
    DWORD lost = (DWORD)InterlockedCompareExchange(&g_focusLostAt, 0, 0);
    if (!lost) return false;
    return (GetTickCount() - lost) >= FOCUS_LOSS_GRACE_MS;
}

static SHORT WINAPI My_GetAsyncKeyState(int vk) {
    InterlockedIncrement(&g_asyncCalls);
    SHORT r = (!SuppressRealInput() && o_GetAsyncKeyState)
              ? o_GetAsyncKeyState(vk) : 0;
    if (SynthHasVK(vk)) r = (SHORT)(r | 0x8000);
    return r;
}
static SHORT WINAPI My_GetKeyState(int vk) {
    InterlockedIncrement(&g_keyStateCalls);
    SHORT r = (!SuppressRealInput() && o_GetKeyState) ? o_GetKeyState(vk) : 0;
    if (SynthHasVK(vk)) r = (SHORT)(r | 0x8000);
    return r;
}
static BOOL WINAPI My_GetKeyboardState(PBYTE st) {
    InterlockedIncrement(&g_kbStateCalls);
    BOOL r = FALSE;
    if (!SuppressRealInput() && o_GetKeyboardState) r = o_GetKeyboardState(st);
    else if (st) { for (int i = 0; i < 256; i++) st[i] = 0; r = TRUE; }
    if (r && st) for (int vk = 0; vk < 256; vk++) if (SynthHasVK(vk)) st[vk] |= 0x80;
    return r;
}

// ---- the console owns the keyboard and mouse while it is open ---------------
// The game reads the keyboard and mouse through Raw Input (RegisterRawInputDevices,
// then GetRawInputData in its WM_INPUT handler) and also takes WM_KEYDOWN /
// WM_CHAR from its queue - it creates no DirectInput devices. So typing in
// SWSE's console drove the game too: WASD walked Stranger, the mouse turned the
// camera, Escape paused. Now, for input typed while the console is open:
//   * a raw key PRESS becomes "no key" (VKey 0xFF); raw mouse motion, button
//     presses and the wheel become nothing. Releases pass, so a key held when
//     the console opened is not left stuck down.
//   * WM_KEYDOWN / WM_CHAR and mouse-button presses become WM_NULL. SWSE's own
//     injected keys (the synthetic lParam bit) still pass.
// "While open" includes input read just after it closed but typed before - the
// Escape or Enter that closed it. SWSE's console reads the keyboard through its
// own GetAsyncKeyState, which none of this touches.
#define SWSE_SYNTH_BIT_EARLY (1 << 25)          // == SWSE_SYNTH_LPARAM_BIT, below
bool SWSE_ConsoleOpen();
void SWSE_ConsoleOpenWindow(DWORD* openedAt, DWORD* closedAt);
static bool TypedIntoConsole(DWORD t) {
    if (SWSE_ConsoleOpen()) return true;
    DWORD o = 0, c = 0;
    SWSE_ConsoleOpenWindow(&o, &c);
    if (!c) return false;
    return (LONG)(t - o) >= 0 && (LONG)(c - t) >= 0;
}
static volatile LONG g_consoleEaten = 0;

// ---- freecam's mouse ---------------------------------------------------------
// The developers' fly camera turns at a RATE set by the look stick, and on PC
// the mouse reaches that stick only as one frame's motion (the game's WndProc
// adds raw counts x 0.002 x sensitivity into 0x9D5510/14; the key-binding pass
// hands at most 1.0 per poll to the virtual pad's right stick). The player's
// cameras know that ("mouse mode", 0x7FE260) and turn by it directly; the fly
// camera, older than the PC port, barely moves (research/FREECAM.md 5.1). So
// while freecam asks, the relative motion the game receives is also added up
// here and freecam turns the camera by it. The game's own records are left
// exactly as they were. Written on the window's thread, taken on the render
// thread: interlocked both ways.
static volatile LONG g_mouseCapture = 0;
static volatile LONG g_mouseDX = 0, g_mouseDY = 0;
void SWSE_InputMouseCapture(bool on) {
    InterlockedExchange(&g_mouseCapture, on ? 1 : 0);
    InterlockedExchange(&g_mouseDX, 0);
    InterlockedExchange(&g_mouseDY, 0);
}
void SWSE_InputTakeMouseDelta(int* dx, int* dy) {
    LONG x = InterlockedExchange(&g_mouseDX, 0);
    LONG y = InterlockedExchange(&g_mouseDY, 0);
    if (dx) *dx = (int)x;
    if (dy) *dy = (int)y;
}

typedef UINT (WINAPI *PFN_GetRawInputData)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
static PFN_GetRawInputData o_GetRawInputData = nullptr;
static UINT WINAPI My_GetRawInputData(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT hdr) {
    UINT r = o_GetRawInputData ? o_GetRawInputData(h, cmd, data, size, hdr) : (UINT)-1;
    if (r == (UINT)-1 || cmd != RID_INPUT || !data || r < sizeof(RAWINPUTHEADER)) return r;
    bool console = TypedIntoConsole((DWORD)GetMessageTime());
    if (!console && g_mouseCapture && r >= sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE)) {
        const RAWINPUT* rm = (const RAWINPUT*)data;
        if (rm->header.dwType == RIM_TYPEMOUSE && !(rm->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) &&
            SWSE_InputReallyFocused() && !SuppressRealInput()) {
            InterlockedExchangeAdd(&g_mouseDX, rm->data.mouse.lLastX);
            InterlockedExchangeAdd(&g_mouseDY, rm->data.mouse.lLastY);
        }
    }
    if (!console) return r;
    RAWINPUT* ri = (RAWINPUT*)data;
    if (ri->header.dwType == RIM_TYPEKEYBOARD && r >= sizeof(RAWINPUTHEADER) + sizeof(RAWKEYBOARD)) {
        if (!(ri->data.keyboard.Flags & RI_KEY_BREAK)) {
            ri->data.keyboard.VKey = 0xFF;
            ri->data.keyboard.MakeCode = 0;
            InterlockedIncrement(&g_consoleEaten);
        }
    } else if (ri->header.dwType == RIM_TYPEMOUSE && r >= sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE)) {
        RAWMOUSE& m = ri->data.mouse;
        if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE)) { m.lLastX = 0; m.lLastY = 0; }
        const USHORT presses = RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_DOWN |
                               RI_MOUSE_MIDDLE_BUTTON_DOWN | RI_MOUSE_BUTTON_4_DOWN |
                               RI_MOUSE_BUTTON_5_DOWN | RI_MOUSE_WHEEL;
        m.usButtonFlags &= (USHORT)~presses;
        if (!(m.usButtonFlags & ~presses)) m.usButtonData = 0;
    }
    return r;
}

// The key MESSAGES are filtered in SWSE's window procedure (BgWndProc), not by
// hooking PeekMessageA. The game's loop (0x5F7E81) loads PeekMessageA,
// TranslateMessage and DispatchMessageA into ESI/EBX/EDI ONCE, before SWSE is
// running, and calls through the registers for the life of the game - so an
// import hook on PeekMessageA is never called. That is why the first console
// filter never ate a key (inputst: 0) while the owner's typing walked Stranger.
// Every message still reaches the window procedure through DispatchMessageA.

// Redirect one imported function in stranger.exe's IAT. Returns the original.
static void* HookImport(const char* dll, const char* fn, void* hook) {
    BYTE* b = (BYTE*)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)b;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(b + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    DWORD rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return 0;
    for (IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(b + rva); imp->Name; imp++) {
        if (lstrcmpiA((const char*)(b + imp->Name), dll)) continue;
        // Bound imports leave OriginalFirstThunk null; names then live in FirstThunk.
        IMAGE_THUNK_DATA* oft = (IMAGE_THUNK_DATA*)(b + (imp->OriginalFirstThunk
                                                         ? imp->OriginalFirstThunk
                                                         : imp->FirstThunk));
        IMAGE_THUNK_DATA* ft = (IMAGE_THUNK_DATA*)(b + imp->FirstThunk);
        for (; oft->u1.AddressOfData && ft->u1.Function; oft++, ft++) {
            if (oft->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(b + oft->u1.AddressOfData);
            if (lstrcmpA((const char*)ibn->Name, fn)) continue;
            DWORD old;
            if (!VirtualProtect(&ft->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return 0;
            void* orig = (void*)ft->u1.Function;
            ft->u1.Function = (DWORD_PTR)hook;
            VirtualProtect(&ft->u1.Function, sizeof(void*), old, &old);
            SWSE_HookNote(&ft->u1.Function, sizeof(void*), "input", SWSE_HOOK_IAT, fn);
            return orig;
        }
    }
    return 0;
}

// The game's top-level window, for the message-loop delivery channel.
// g_wnd is declared near SuppressRealInput

static BOOL CALLBACK FindWnd(HWND h, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    if (!IsWindowVisible(h)) return TRUE;
    if (GetWindow(h, GW_OWNER)) return TRUE;      // skip owned dialogs
    g_wnd = h;
    return FALSE;
}

// ---- keep running while alt-tabbed -----------------------------------------
// The game stops advancing the moment it loses focus. That is a real obstacle
// to working on it: the user cannot do anything else while a test runs, and it
// silently invalidated several unattended measurements here - a reaction queued
// and then slept on expires by wall clock without the game ever running a
// frame, which looks exactly like "the effect did not fire".
//
// The engine only knows it lost focus because Windows tells it, so intercept
// the telling: rewrite the deactivation messages as activations, swallow
// WM_KILLFOCUS, and answer the focus queries with the game's own window.
static WNDPROC o_wndProc = nullptr;
static bool    g_agentDebug  = false;

// FIRST ATTEMPT TRAPPED THE MOUSE. Telling the engine it still has focus also
// left it believing it still owns the cursor, and this game clips the cursor to
// its window and recentres it every frame - so the pointer could not leave the
// game's rectangle. "Keep simulating" and "keep owning input" are separate
// things and must be handled separately:
//
//   reported focus  -> always active, so the engine keeps ticking
//   REAL focus      -> tracked here; when false, cursor capture and input
//                      delivery are suppressed so the desktop stays usable
static volatile LONG g_reallyFocused = 1;
// When focus was last lost, for the debounce in SuppressRealInput. 0 = focused.
volatile LONG g_focusLostAt = 0;

// Every place that clears focus routes through here so the timestamp cannot be
// forgotten at one of them.
static void NoteFocus(int focused) {
    InterlockedExchange(&g_reallyFocused, focused ? 1 : 0);
    InterlockedExchange(&g_focusLostAt, focused ? 0 : (LONG)GetTickCount());
}

// Marks a keyboard message as SWSE-injected. Bits 25-28 of a keyboard lParam
// are reserved and are zero for genuine input, so this never collides.
#define SWSE_SYNTH_LPARAM_BIT (1 << 25)
static_assert(SWSE_SYNTH_LPARAM_BIT == SWSE_SYNTH_BIT_EARLY, "the console filter's copy of the synthetic-key bit");

int SWSE_InputReallyFocused() { return g_reallyFocused ? 1 : 0; }

// ---- keeping the keyboard where the owner is ------------------------------------
// Agents launch the game from inside the app the owner types in, and Windows
// lets a process started by the foreground app take the foreground itself - so
// the game came to the front after loads while the owner was typing, and their
// keys walked Stranger around (2026-09-28). The calls behind it were not the
// game's own imports (the hooks below never fired), so the activation is
// answered where it lands instead: in background mode, when the game becomes
// the active app and the user did NOT click it (WM_ACTIVATE says WA_ACTIVE, not
// WA_CLICKACTIVE) and is not on the taskbar, the foreground goes straight back
// to the window the user was in. A click on the game, or on its taskbar
// button, still hands it over.
#define WM_SWSE_HANDBACK (WM_APP + 0x5E1)
static HWND          g_lastOtherFg = nullptr;   // the user's window, polled each frame
static volatile LONG g_appActivatedAt = 0;      // tick of the last WM_ACTIVATEAPP(TRUE)
static volatile LONG g_handedBack = 0;

static bool OtherProcessWindow(HWND h) {
    if (!h || !IsWindow(h)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    return pid && pid != GetCurrentProcessId();
}
static bool CursorOnTaskbar() {
    POINT pt;
    if (!GetCursorPos(&pt)) return false;
    HWND root = GetAncestor(WindowFromPoint(pt), GA_ROOT);
    char cls[64] = "";
    if (!root || !GetClassNameA(root, cls, sizeof(cls))) return false;
    return !lstrcmpA(cls, "Shell_TrayWnd") || !lstrcmpA(cls, "Shell_SecondaryTrayWnd");
}
static void PidExe(DWORD pid, char* out, int outLen) {
    lstrcpynA(out, "?", outLen);
    HANDLE p = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
    if (!p) return;
    char path[MAX_PATH]; DWORD n = MAX_PATH;
    if (QueryFullProcessImageNameA(p, 0, path, &n)) {
        const char* base = strrchr(path, '\\');
        lstrcpynA(out, base ? base + 1 : path, outLen);
    }
    CloseHandle(p);
}
static void WindowExe(HWND h, char* out, int outLen) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    PidExe(pid, out, outLen);
}
// Is h part of the Windows shell: the taskbar, the Alt+Tab switcher, Task
// View, the Start menu, the desktop? Activation that comes FROM one of those is
// the user switching to the game, so the game keeps it.
static bool ShellWindow(HWND h) {
    if (!h || !IsWindow(h)) return false;
    char exe[MAX_PATH];
    WindowExe(h, exe, sizeof(exe));
    return !lstrcmpiA(exe, "explorer.exe") || !lstrcmpiA(exe, "ShellExperienceHost.exe") ||
           !lstrcmpiA(exe, "StartMenuExperienceHost.exe") || !lstrcmpiA(exe, "SearchHost.exe");
}

// Every activation message in agent mode, as it arrives, so the next grab that
// gets past the hand-back names the path it took: the time, the message and
// what its wParam says, the exe on the other side (WM_ACTIVATE's lParam is a
// window, WM_ACTIVATEAPP's a thread), the taskbar test, and the decision. The
// first 200, then every power of two.
static volatile LONG g_actLogged = 0;
static void LogActivation(UINT m, WPARAM w, LPARAM l, bool onBar, const char* decided) {
    LONG n = InterlockedIncrement(&g_actLogged);
    if (n > 200 && (n & (n - 1))) return;
    SYSTEMTIME t; GetLocalTime(&t);
    char other[MAX_PATH] = "none";
    char says[48];
    if (m == WM_ACTIVATEAPP) {
        lstrcpynA(says, w ? "TRUE" : "FALSE", sizeof(says));
        HANDLE th = l ? OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)l) : nullptr;
        if (th) { PidExe(GetProcessIdOfThread(th), other, sizeof(other)); CloseHandle(th); }
    } else {
        const char* s = LOWORD(w) == WA_INACTIVE ? "WA_INACTIVE" : LOWORD(w) == WA_CLICKACTIVE ? "WA_CLICKACTIVE" : "WA_ACTIVE";
        _snprintf_s(says, sizeof(says), _TRUNCATE, "%s%s", s, HIWORD(w) ? ", minimized" : "");
        if (l) WindowExe((HWND)l, other, sizeof(other));
    }
    char b[MAX_PATH + 200];
    _snprintf_s(b, sizeof(b), _TRUNCATE,
                "background: %02d:%02d:%02d.%03d %s(%s), other side %s, cursor %son the taskbar - %s",
                t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                m == WM_ACTIVATEAPP ? "WM_ACTIVATEAPP" : "WM_ACTIVATE", says, other,
                onBar ? "" : "not ", decided);
    SWSE_Log(b);
}
// Runs on the window's own thread, after the activation has finished. The game
// is the foreground process at this point, so it may give the foreground away.
// SetForegroundWindow here is SWSE's own import, not the game's hooked one.
static void HandBack(HWND self, HWND to) {
    if (!OtherProcessWindow(to) || GetForegroundWindow() != self) return;
    BOOL ok = SetForegroundWindow(to);
    LONG n = InterlockedIncrement(&g_handedBack);
    if (n > 5 && (n & (n - 1))) return;
    char exe[MAX_PATH], b[MAX_PATH + 160];
    WindowExe(to, exe, sizeof(exe));
    wsprintfA(b, "background: the game came to the front without a click - keyboard handed back to %s (%s, #%ld)",
              exe, ok ? "done" : "Windows refused", n);
    SWSE_Log(b);
}

static LRESULT CALLBACK BgWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    // Never call through a pointer we are not sure of. Activation messages
    // arrive exactly when the user clicks the window, and a stale o_wndProc -
    // or one captured from a previous load - turns that click into a crash.
    if (!o_wndProc) return DefWindowProcA(h, m, w, l);
    if (m == WM_SWSE_HANDBACK) { HandBack(h, (HWND)l); return 0; }
    // What is typed into the console stays in the console: key presses,
    // characters and mouse presses typed while it was open never reach the
    // game (the raw feed is handled in My_GetRawInputData). Releases pass, so
    // nothing sticks; SWSE's own injected keys carry the synthetic bit and pass.
    switch (m) {
    case WM_KEYDOWN: case WM_CHAR: case WM_DEADCHAR:
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
    case WM_MOUSEWHEEL:
        if (!(l & SWSE_SYNTH_BIT_EARLY) && TypedIntoConsole((DWORD)GetMessageTime())) {
            InterlockedIncrement(&g_consoleEaten);
            return 0;
        }
        break;
    }
    if (g_agentDebug && (m == WM_ACTIVATEAPP || m == WM_ACTIVATE)) {
        bool onBar = CursorOnTaskbar();
        const char* decided;
        // WM_ACTIVATEAPP(TRUE) is only sent when another app was active before,
        // so it marks a transition from the background; WM_ACTIVATE follows.
        if (m == WM_ACTIVATEAPP) {
            if (w) InterlockedExchange(&g_appActivatedAt, (LONG)GetTickCount());
            decided = w ? "noted: the game's app is active now" : "noted: another app is active now";
        } else if (LOWORD(w) == WA_ACTIVE && GetTickCount() - (DWORD)g_appActivatedAt < 1000 && !onBar) {
            HWND to = OtherProcessWindow((HWND)l) ? (HWND)l : g_lastOtherFg;
            if (ShellWindow(to)) {
                // Alt+Tab, Task View or a taskbar button: the switcher is the
                // shell's window, so this is the user choosing the game. The
                // first version handed the keyboard to explorer.exe instead,
                // three times while the owner was trying to get in (2026-09-28).
                decided = "switched to from the shell (Alt+Tab / taskbar) - the game keeps it";
            } else if (OtherProcessWindow(to)) {
                PostMessageA(h, WM_SWSE_HANDBACK, 0, (LPARAM)to);
                decided = "no click: handing the keyboard back";
            } else {
                decided = "no click, but no window of the user's to hand back to - left";
            }
        } else if (LOWORD(w) == WA_INACTIVE) {
            decided = "deactivated - nothing to do";
        } else if (LOWORD(w) == WA_CLICKACTIVE) {
            decided = "a click - the game keeps it";
        } else if (onBar) {
            decided = "cursor on the taskbar - the game keeps it";
        } else {
            decided = "no app switch in the last second - left";
        }
        LogActivation(m, w, l, onBar, decided);
    }
    // Record the TRUE state before rewriting anything.
    switch (m) {
    case WM_ACTIVATE:
        NoteFocus((LOWORD(w) == WA_INACTIVE) ? 0 : 1);
        break;
    case WM_ACTIVATEAPP:
        NoteFocus(w ? 1 : 0);
        break;
    case WM_KILLFOCUS:
        NoteFocus(0);
        break;
    case WM_SETFOCUS:
        NoteFocus(1);
        break;
    }
    if (g_agentDebug) {
        switch (m) {
        case WM_ACTIVATE:
            if (LOWORD(w) == WA_INACTIVE) w = MAKEWPARAM(WA_ACTIVE, 0);
            break;
        case WM_ACTIVATEAPP:
            if (!w) w = TRUE;
            break;
        case WM_NCACTIVATE:
            if (!w) w = TRUE;
            break;
        case WM_KILLFOCUS:
            return 0;                       // never tell it focus was lost
        // Mouse input that arrives while we are genuinely unfocused is the
        // user working in another app; do not let the game act on it.
        case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MOUSEWHEEL:
        case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR:
            // Real keystrokes while unfocused belong to whatever app the user
            // is in. SWSE's own injected keys must still get through, or the
            // console can no longer drive the game unattended - which is the
            // entire point of this mode. Bits 25-28 of a keyboard lParam are
            // reserved and always zero for genuine input, so one of them makes
            // a marker that costs nothing.
            if (!g_reallyFocused && !(l & SWSE_SYNTH_LPARAM_BIT)) return 0;
            break;
        }
    }
    return CallWindowProcA(o_wndProc, h, m, w, l);
}

// The game re-clips and re-centres the cursor every frame while it believes it
// is active, so a one-off ClipCursor(NULL) is not enough - its own calls have
// to be neutralised for as long as we are really unfocused.
typedef BOOL (WINAPI *PFN_ClipCursor)(const RECT*);
typedef BOOL (WINAPI *PFN_SetCursorPos)(int, int);
typedef int  (WINAPI *PFN_ShowCursor)(BOOL);
static PFN_ClipCursor   o_ClipCursor   = nullptr;
static PFN_SetCursorPos o_SetCursorPos = nullptr;
static PFN_ShowCursor   o_ShowCursor   = nullptr;

static BOOL WINAPI My_ClipCursor(const RECT* r) {
    if (g_agentDebug && !g_reallyFocused) {
        if (o_ClipCursor) o_ClipCursor(nullptr);      // keep the desktop free
        return TRUE;
    }
    return o_ClipCursor ? o_ClipCursor(r) : TRUE;
}
static BOOL WINAPI My_SetCursorPos(int x, int y) {
    // Recentring is what actually pins the pointer inside the window.
    if (g_agentDebug && !g_reallyFocused) return TRUE;
    return o_SetCursorPos ? o_SetCursorPos(x, y) : TRUE;
}
// The game hides its cursor with `while (ShowCursor(FALSE) >= 0);` (at
// 0x5F7806 - it decrements Windows' display counter until it goes negative).
// Background mode keeps the desktop cursor visible by not forwarding the
// hide, and used to answer 0 - "still visible" - so that loop never ended:
// the game's window thread spun at 100% forever, pumped no messages, and read
// as hung (IsHungAppWindow) - which is why `continue` could not load a save in
// the background. Instead the game gets a VIRTUAL counter that moves the way
// the real one would; the real cursor stays shown, and the virtual state is
// applied to the real counter when the window really gets focus back.
static int  g_virtCursor = 0;          // display count the game believes in
static bool g_virtActive = false;      // a background hide/show is pending
static int WINAPI My_ShowCursor(BOOL show) {
    if (g_agentDebug && !g_reallyFocused) {
        g_virtActive = true;
        g_virtCursor += show ? 1 : -1;
        return g_virtCursor;
    }
    int r = o_ShowCursor ? o_ShowCursor(show) : 0;
    g_virtCursor = r;
    return r;
}

// Focus is back: make the real counter match what the game asked for while
// it was in the background (hidden if its virtual count is negative).
static void SyncCursorAfterBackground() {
    if (!g_virtActive || !o_ShowCursor) return;
    g_virtActive = false;
    bool wantHidden = g_virtCursor < 0;
    int real = o_ShowCursor(TRUE);                     // read by nudging...
    real = o_ShowCursor(FALSE);                        // ...and restoring
    for (int i = 0; i < 64 && wantHidden && real >= 0; i++) real = o_ShowCursor(FALSE);
    for (int i = 0; i < 64 && !wantHidden && real < 0; i++) real = o_ShowCursor(TRUE);
    g_virtCursor = real;
}

// PFN_GetWnd / o_GetForegroundWindow are declared up by SuppressRealInput,
// which needs them for the focus-loss debounce.
static PFN_GetWnd o_GetActiveWindow     = nullptr;
static PFN_GetWnd o_GetFocus            = nullptr;

static HWND WINAPI My_GetForegroundWindow(void) {
    if (g_agentDebug && g_wnd) return g_wnd;
    return o_GetForegroundWindow ? o_GetForegroundWindow() : (HWND)0;
}
static HWND WINAPI My_GetActiveWindow(void) {
    if (g_agentDebug && g_wnd) return g_wnd;
    return o_GetActiveWindow ? o_GetActiveWindow() : (HWND)0;
}
static HWND WINAPI My_GetFocus(void) {
    if (g_agentDebug && g_wnd) return g_wnd;
    return o_GetFocus ? o_GetFocus() : (HWND)0;
}

// The game brings itself to the front on its own - after a level load, for
// one - and once the user has been idle a little while Windows lets it. In
// background mode that put the owner's keystrokes into the parked game while
// they typed elsewhere (2026-09-28). So while background mode is on and the
// window does not really have focus, the game's OWN activation calls are
// neutralised. A click on its window still activates it, because Windows does
// that, not the game; from then on these calls pass through untouched.
typedef BOOL (WINAPI *PFN_SetForegroundWindow)(HWND);
typedef HWND (WINAPI *PFN_SetFocus)(HWND);
typedef BOOL (WINAPI *PFN_ShowWindow)(HWND, int);
typedef BOOL (WINAPI *PFN_SetWindowPos)(HWND, HWND, int, int, int, int, UINT);
static PFN_SetForegroundWindow o_SetForegroundWindow = nullptr;
static PFN_SetFocus            o_SetFocus            = nullptr;
static PFN_ShowWindow          o_ShowWindow          = nullptr;
static PFN_SetWindowPos        o_SetWindowPos        = nullptr;
static volatile LONG           g_keptBack[4];

static bool KeepInBackground(HWND h) {
    return g_agentDebug && !g_reallyFocused && g_wnd && (!h || h == g_wnd);
}
// The first three of each kind, then powers of two, so the log names the call
// the game uses without filling up.
static void NoteKeptBack(int i, const char* fn) {
    LONG n = InterlockedIncrement(&g_keptBack[i]);
    if (n > 3 && (n & (n - 1))) return;
    char b[160];
    wsprintfA(b, "background: kept the game from taking the foreground (%s, #%ld)", fn, n);
    SWSE_Log(b);
}
static BOOL WINAPI My_SetForegroundWindow(HWND h) {
    if (KeepInBackground(h)) { NoteKeptBack(0, "SetForegroundWindow"); return TRUE; }
    return o_SetForegroundWindow ? o_SetForegroundWindow(h) : FALSE;
}
static HWND WINAPI My_SetFocus(HWND h) {
    if (KeepInBackground(h)) { NoteKeptBack(1, "SetFocus"); return g_wnd; }
    return o_SetFocus ? o_SetFocus(h) : (HWND)0;
}
static BOOL WINAPI My_ShowWindow(HWND h, int cmd) {
    if (h && KeepInBackground(h)) {
        int keep = cmd;
        if (cmd == SW_SHOWNORMAL || cmd == SW_RESTORE || cmd == SW_SHOWDEFAULT) keep = SW_SHOWNOACTIVATE;
        else if (cmd == SW_SHOW) keep = SW_SHOWNA;
        if (keep != cmd) { NoteKeptBack(2, "ShowWindow"); cmd = keep; }
    }
    return o_ShowWindow ? o_ShowWindow(h, cmd) : FALSE;
}
static BOOL WINAPI My_SetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT flags) {
    if (h && KeepInBackground(h)) {
        UINT keep = flags | SWP_NOACTIVATE;
        // Raising it to the top would still cover the windows the owner is in.
        if (after == HWND_TOP || after == HWND_TOPMOST) keep |= SWP_NOZORDER;
        if (keep != flags) { NoteKeptBack(3, "SetWindowPos"); flags = keep; }
    }
    return o_SetWindowPos ? o_SetWindowPos(h, after, x, y, cx, cy, flags) : FALSE;
}

// SWSE's window procedure in front of the game's. It used to go in only with
// background mode. The console filter lives there too now (the game's loop
// bypasses import hooks - see the note after My_GetRawInputData), so it goes
// in at the first frame, always; everything background mode does in it stays
// behind g_agentDebug.
static bool EnsureSubclass(char* msg, int msgLen) {
    if (!g_wnd) EnumWindows(FindWnd, 0);
    if (!g_wnd) { if (msg) lstrcpynA(msg, "no game window found", msgLen); return false; }
    if (o_wndProc) return true;
    // Refuse to subclass a window that is ALREADY subclassed by us from a
    // previous DLL load: the old BgWndProc address points into unmapped
    // memory, and chaining to it crashes on the next activation - which is
    // the moment the user clicks the window.
    WNDPROC cur = (WNDPROC)GetWindowLongPtrA(g_wnd, GWLP_WNDPROC);
    if (cur == BgWndProc) {
        if (msg) lstrcpynA(msg, "already subclassed (reusing existing hook)", msgLen);
        return true;
    }
    // The game's thread may dispatch the moment the pointer is swapped, so the
    // original is known BEFORE BgWndProc can run (it passes messages to
    // DefWindowProc while o_wndProc is null).
    o_wndProc = cur;
    WNDPROC prev = (WNDPROC)SetWindowLongPtrA(g_wnd, GWLP_WNDPROC, (LONG_PTR)BgWndProc);
    if (!prev) { o_wndProc = nullptr; if (msg) lstrcpynA(msg, "could not subclass the window", msgLen); return false; }
    if (prev != cur) o_wndProc = prev;
    return true;
}

int SWSE_AgentDebugMode(int on, char* msg, int msgLen) {
    if (!EnsureSubclass(msg, msgLen)) return 0;
    g_agentDebug = (on != 0);
    // Never leave the pointer trapped: releasing on both transitions means an
    // 'off' is always a way out, even if a hook failed to install.
    ClipCursor(nullptr);
    wsprintfA(msg, "AgentDebugMode %s (hwnd %p)%s", g_agentDebug ? "ON" : "off",
              (void*)g_wnd,
              g_agentDebug ? " - cursor freed while alt-tabbed" : "");
    return 1;
}
int SWSE_AgentDebugModeOn() { return g_agentDebug ? 1 : 0; }

static bool g_probed = false;
void SWSE_InputInstallProbes() {
    if (g_probed) return;
    g_probed = true;
    void* p;
    p = HookImport("user32.dll", "GetAsyncKeyState", (void*)My_GetAsyncKeyState);
    if (p) o_GetAsyncKeyState = (PFN_GetAsyncKeyState)p;
    p = HookImport("user32.dll", "GetKeyState", (void*)My_GetKeyState);
    if (p) o_GetKeyState = (PFN_GetKeyState)p;
    p = HookImport("user32.dll", "GetKeyboardState", (void*)My_GetKeyboardState);
    if (p) o_GetKeyboardState = (PFN_GetKeyboardState)p;

    // Focus queries, for background mode. Hooked unconditionally; they only
    // change behaviour once background mode is switched on.
    p = HookImport("user32.dll", "GetForegroundWindow", (void*)My_GetForegroundWindow);
    if (p) o_GetForegroundWindow = (PFN_GetWnd)p;
    p = HookImport("user32.dll", "GetActiveWindow", (void*)My_GetActiveWindow);
    if (p) o_GetActiveWindow = (PFN_GetWnd)p;
    p = HookImport("user32.dll", "GetFocus", (void*)My_GetFocus);
    if (p) o_GetFocus = (PFN_GetWnd)p;
    // What the console types stays in the console (see My_GetRawInputData).
    p = HookImport("user32.dll", "GetRawInputData", (void*)My_GetRawInputData);
    if (p) o_GetRawInputData = (PFN_GetRawInputData)p;
    // ...and the game's own attempts to take the foreground (see above).
    p = HookImport("user32.dll", "SetForegroundWindow", (void*)My_SetForegroundWindow);
    if (p) o_SetForegroundWindow = (PFN_SetForegroundWindow)p;
    p = HookImport("user32.dll", "SetFocus", (void*)My_SetFocus);
    if (p) o_SetFocus = (PFN_SetFocus)p;
    p = HookImport("user32.dll", "ShowWindow", (void*)My_ShowWindow);
    if (p) o_ShowWindow = (PFN_ShowWindow)p;
    p = HookImport("user32.dll", "SetWindowPos", (void*)My_SetWindowPos);
    if (p) o_SetWindowPos = (PFN_SetWindowPos)p;

    // Cursor ownership. Without these the game clips and recentres the pointer
    // every frame and the desktop becomes unusable while background mode is on.
    p = HookImport("user32.dll", "ClipCursor", (void*)My_ClipCursor);
    if (p) o_ClipCursor = (PFN_ClipCursor)p;
    p = HookImport("user32.dll", "SetCursorPos", (void*)My_SetCursorPos);
    if (p) o_SetCursorPos = (PFN_SetCursorPos)p;
    p = HookImport("user32.dll", "ShowCursor", (void*)My_ShowCursor);
    if (p) o_ShowCursor = (PFN_ShowCursor)p;

    EnumWindows(FindWnd, 0);
    {
        char sm[160] = "";
        bool ok = EnsureSubclass(sm, sizeof(sm));
        char lb[220];
        wsprintfA(lb, "input: window procedure %s%s%s", ok ? "installed (console input filter)" : "NOT installed",
                  sm[0] ? " - " : "", sm);
        SWSE_Log(lb);
    }

    // AgentDebugMode on by default. This is a development build and the whole
    // point is that the game can be driven and observed while the user works in
    // another application - having to enable it by hand after every restart
    // means the game sits paused whenever they alt-tab, which stalls both of
    // us. 'agentdebug off' restores stock focus behaviour at any time.
    // NOT auto-enabled. This trapped the user's mouse twice: the game re-clips
    // the cursor every frame while it believes it is active, so a bad focus
    // decision costs them their desktop, not just a failed test. Opt in with
    // 'agentdebug on' once the focus polling has proven itself.

    char msg[220];
    wsprintfA(msg, "input: probes installed - GetAsyncKeyState=%s GetKeyState=%s "
                   "GetKeyboardState=%s hwnd=%p",
              o_GetAsyncKeyState ? "yes" : "not-imported",
              o_GetKeyState      ? "yes" : "not-imported",
              o_GetKeyboardState ? "yes" : "not-imported", (void*)g_wnd);
    SWSE_Log(msg);
}

// Message-loop delivery. Posting straight to the window bypasses focus
// entirely, unlike SendKeys, which only reaches the foreground window.
void SWSE_PostKeyMessage(int scan, bool down) {
    if (!g_wnd) EnumWindows(FindWnd, 0);
    if (!g_wnd) return;
    UINT vk = MapVirtualKeyA(scan & 0x7F, 1 /*MAPVK_VSC_TO_VK*/);
    switch (scan) {                      // extended keys
        case 0xC8: vk = VK_UP;    break;
        case 0xD0: vk = VK_DOWN;  break;
        case 0xCB: vk = VK_LEFT;  break;
        case 0xCD: vk = VK_RIGHT; break;
        case 0x9C: vk = VK_RETURN;break;
    }
    if (!vk) return;
    LPARAM lp = (LPARAM)(1 | ((scan & 0x7F) << 16));
    if (scan & 0x80) lp |= (1 << 24);                       // extended-key flag
    lp |= SWSE_SYNTH_LPARAM_BIT;         // ours: survives the unfocused filter
    if (down) PostMessageA(g_wnd, WM_KEYDOWN, vk, lp);
    else      PostMessageA(g_wnd, WM_KEYUP, vk, lp | (1 << 30) | (1 << 31));
}

// Per-frame driver: installs the probes once, then turns our scheduled press
// windows into WM_KEYDOWN/WM_KEYUP edges. The DirectInput overlay and the
// Win32 polling hooks answer on demand and need no tick; only the message
// channel has to be pushed.
static BYTE g_prevPost[256];
void SWSE_InputTick() {
    SWSE_InputInstallProbes();
    // Which thread is which. MEASURED: SWSE's frame hook runs on the render
    // thread, and the game window belongs to a DIFFERENT thread (the game's
    // main thread) - so nothing here can pump the window's messages. That
    // main thread stops pumping while the window is minimized (IsHungAppWindow
    // true, WM_NULL unanswered), which is why background mode parks the
    // window behind other windows instead of minimizing it.
    static bool s_threadsLogged = false;
    if (g_wnd && !s_threadsLogged) {
        s_threadsLogged = true;
        char b[160];
        wsprintfA(b, "input: frame hook runs on thread %u; the game window belongs to thread %u",
                  (unsigned)GetCurrentThreadId(), (unsigned)GetWindowThreadProcessId(g_wnd, nullptr));
        SWSE_Log(b);
    }
    // Authoritative focus, polled every frame.
    //
    // Deriving focus from window messages alone was wrong twice: a message
    // missed (or one that arrived before the subclass was installed) leaves the
    // flag stuck at "not focused", and the game then ignores the user's own
    // keyboard and mouse while they are looking straight at it. Asking the OS
    // cannot go stale. Note this must call the ORIGINAL GetForegroundWindow -
    // ours lies by design when AgentDebugMode is on.
    if (g_wnd) {
        HWND fg = o_GetForegroundWindow ? o_GetForegroundWindow()
                                        : GetForegroundWindow();
        LONG focused = (fg == g_wnd) ? 1 : 0;
        LONG was = InterlockedExchange(&g_reallyFocused, focused);
        if (focused && !was) SyncCursorAfterBackground();
        // The window the user is in, for a hand-back (see BgWndProc).
        if (!focused && OtherProcessWindow(fg)) g_lastOtherFg = fg;
    }

    // Safety net. If the game calls ClipCursor/SetCursorPos through a pointer
    // rather than the import table the hooks above never fire, and the pointer
    // stays trapped. Forcing the clip open every frame while genuinely
    // unfocused cannot be defeated that way. Cheap, and it is the difference
    // between a usable desktop and a locked one.
    if (g_agentDebug && !g_reallyFocused) ClipCursor(nullptr);
    BYTE cur[256];
    SynthState(cur, true);                  // the one caller that advances presses
    for (int i = 1; i < 256; i++) {
        if (cur[i] == g_prevPost[i]) continue;
        SWSE_PostKeyMessage(i, cur[i] != 0);
    }
    memcpy(g_prevPost, cur, 256);
}

// MEASURED: this game reads keys ONLY from its window message queue. It calls
// DirectInput8Create but creates zero devices (that path is for gamepads), and
// it imports none of GetAsyncKeyState/GetKeyState/GetKeyboardState. So
// readiness means "we found the window", not "we saw a DI device". The DI
// proxy and Win32 hooks below are kept because they are what proved this, and
// they will light up immediately if a patch or a gamepad changes the picture.
bool SWSE_InputReady() { return g_wnd != 0; }

void SWSE_InputStatus(char* out, int outLen) {
    // Whichever counter is climbing is the path the game really reads.
    _snprintf_s(out, outLen, _TRUNCATE,
        "dinput: %dkbd/%dmouse/%dother reads=%d/%d | win32: async=%d keystate=%d "
        "kbstate=%d | hwnd=%p",
        (int)g_nKeyboards, (int)g_nMice, (int)g_nOther,
        (int)g_stateCalls, (int)g_dataCalls,
        (int)g_asyncCalls, (int)g_keyStateCalls, (int)g_kbStateCalls, (void*)g_wnd);
}

// The two guards: what the console kept from the game, and the keyboard focus
// handed back to the user's window in background mode.
void SWSE_InputGuardStatus(char* out, int outLen) {
    LONG kept = 0;
    for (int i = 0; i < 4; i++) kept += g_keptBack[i];
    _snprintf_s(out, outLen, _TRUNCATE,
        "guards: %d input(s) kept from the game while the console was open; "
        "focus handed back %d time(s); %d activation call(s) neutralised",
        (int)g_consoleEaten, (int)g_handedBack, (int)kept);
}

extern "C" HMODULE SWSE_RealDInput8();   // dllmain.cpp

// ---- the export ----------------------------------------------------------
// Undecorated name for a __stdcall export on x86.
#pragma comment(linker, "/export:DirectInput8Create=_DirectInput8Create@20")

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD ver, REFIID riid,
                                             LPVOID* out, LPUNKNOWN outer) {
    static PFN_Create real = 0;
    if (!real) {
        // Shared resolver (dllmain.cpp): dinput8_real.dll when present,
        // otherwise the real system dinput8.dll by absolute path. THIS is the
        // call the game makes during startup, so a missing dinput8_real.dll
        // used to kill the process right here - no window, no error, only a
        // swse_log.txt that made it look like SWSE had loaded fine.
        HMODULE m = SWSE_RealDInput8();
        if (!m) return E_FAIL;
        real = (PFN_Create)GetProcAddress(m, "DirectInput8Create");
        if (!real) return E_FAIL;
    }

    HRESULT hr = real(hinst, ver, riid, out, outer);
    if (SUCCEEDED(hr) && out && *out) {
        void* p = PatchSlot(*out, VT_DI_CREATEDEVICE, (void*)My_CreateDevice, "IDirectInput8::CreateDevice");
        if (p) {
            o_CreateDevice = (PFN_CreateDevice)p;
            SWSE_Log("input: DirectInput8 proxied - CreateDevice hooked");
        }
    }
    return hr;
}

// ---- name -> DIK scancode ------------------------------------------------
struct KeyName { const char* name; int scan; };
static const KeyName kKeys[] = {
    {"esc",0x01},{"escape",0x01},{"1",0x02},{"2",0x03},{"3",0x04},{"4",0x05},
    {"5",0x06},{"6",0x07},{"7",0x08},{"8",0x09},{"9",0x0A},{"0",0x0B},
    {"back",0x0E},{"backspace",0x0E},{"tab",0x0F},
    {"q",0x10},{"w",0x11},{"e",0x12},{"r",0x13},{"t",0x14},{"y",0x15},
    {"u",0x16},{"i",0x17},{"o",0x18},{"p",0x19},
    {"enter",0x1C},{"return",0x1C},{"ctrl",0x1D},
    {"a",0x1E},{"s",0x1F},{"d",0x20},{"f",0x21},{"g",0x22},{"h",0x23},
    {"j",0x24},{"k",0x25},{"l",0x26},
    {"shift",0x2A},{"z",0x2C},{"x",0x2D},{"c",0x2E},{"v",0x2F},{"b",0x30},
    {"n",0x31},{"m",0x32},{"alt",0x38},{"space",0x39},
    {"f1",0x3B},{"f2",0x3C},{"f3",0x3D},{"f4",0x3E},{"f5",0x3F},{"f6",0x40},
    {"f7",0x41},{"f8",0x42},{"f9",0x43},{"f10",0x44},{"f11",0x57},{"f12",0x58},
    {"numenter",0x9C},{"up",0xC8},{"left",0xCB},{"right",0xCD},{"down",0xD0},
    {"ins",0xD2},{"del",0xD3},{"home",0xC7},{"end",0xCF},
    {"pgup",0xC9},{"pgdn",0xD1},
};

int SWSE_ScanForName(const char* name) {
    if (!name || !*name) return -1;
    // raw scancode: "0x1C" or a bare number
    if (name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
        int v = 0;
        for (const char* p = name + 2; *p; p++) {
            int d = (*p >= '0' && *p <= '9') ? *p - '0'
                  : (*p >= 'a' && *p <= 'f') ? *p - 'a' + 10
                  : (*p >= 'A' && *p <= 'F') ? *p - 'A' + 10 : -1;
            if (d < 0) return -1;
            v = v * 16 + d;
        }
        return (v > 0 && v < 256) ? v : -1;
    }
    for (int i = 0; i < (int)(sizeof(kKeys)/sizeof(kKeys[0])); i++)
        if (!lstrcmpiA(name, kKeys[i].name)) return kKeys[i].scan;
    // a bare number, but only after the name table so "1"/"0" stay as digit keys
    if (name[0] >= '0' && name[0] <= '9') {
        int v = 0;
        for (const char* p = name; *p; p++) {
            if (*p < '0' || *p > '9') return -1;
            v = v * 10 + (*p - '0');
        }
        return (v > 0 && v < 256) ? v : -1;
    }
    return -1;
}
