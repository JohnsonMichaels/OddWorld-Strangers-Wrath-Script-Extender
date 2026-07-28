// SWSE â€” DirectInput8 proxy with synthetic key injection.
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
// no per-frame tick â€” the read hook just asks which windows are open right now.
#define MAX_PRESSES 64
struct Press { DWORD downAt, upAt; BYTE scan; BYTE live; };
static Press g_press[MAX_PRESSES];
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
        if (g_press[i].live && g_press[i].upAt > now) continue;   // still in use
        g_press[i].scan   = (BYTE)scan;
        g_press[i].downAt = now + (DWORD)delayMs;
        g_press[i].upAt   = now + (DWORD)delayMs + (DWORD)holdMs;
        g_press[i].live   = 1;
        break;
    }
    LeaveCriticalSection(&g_lock);
}

// Fill a 256-byte DIK map with the keys we are currently holding down.
static void SynthState(BYTE* out) {
    memset(out, 0, 256);
    if (!g_lockReady) return;
    EnterCriticalSection(&g_lock);
    DWORD now = GetTickCount();
    for (int i = 0; i < MAX_PRESSES; i++) {
        if (!g_press[i].live) continue;
        if (now >= g_press[i].upAt) { g_press[i].live = 0; continue; }
        if (now >= g_press[i].downAt) out[g_press[i].scan] = 0x80;
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

static void* PatchSlot(void* iface, int index, void* hook) {
    void** vt = *(void***)iface;
    DWORD old;
    if (!VirtualProtect(&vt[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return 0;
    void* orig = vt[index];
    if (orig == hook) { VirtualProtect(&vt[index], sizeof(void*), old, &old); return 0; }
    vt[index] = hook;
    VirtualProtect(&vt[index], sizeof(void*), old, &old);
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
    void* p = PatchSlot(*out, VT_DEV_GETSTATE, (void*)My_GetState);
    if (p) o_GetState = (PFN_GetState)p;
    p = PatchSlot(*out, VT_DEV_GETDATA, (void*)My_GetData);
    if (p) o_GetData = (PFN_GetData)p;

    char msg[160];
    wsprintfA(msg, "input: device created (%s) â€” read hooks %s",
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

int SWSE_InputReallyFocused() { return g_reallyFocused ? 1 : 0; }

static LRESULT CALLBACK BgWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    // Never call through a pointer we are not sure of. Activation messages
    // arrive exactly when the user clicks the window, and a stale o_wndProc -
    // or one captured from a previous load - turns that click into a crash.
    if (!o_wndProc) return DefWindowProcA(h, m, w, l);
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
static int WINAPI My_ShowCursor(BOOL show) {
    if (g_agentDebug && !g_reallyFocused && !show) return 0;   // stay visible
    return o_ShowCursor ? o_ShowCursor(show) : 0;
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

int SWSE_AgentDebugMode(int on, char* msg, int msgLen) {
    if (!g_wnd) EnumWindows(FindWnd, 0);
    if (!g_wnd) { lstrcpynA(msg, "no game window found", msgLen); return 0; }
    if (on && !o_wndProc) {
        // Refuse to subclass a window that is ALREADY subclassed by us from a
        // previous DLL load: the old BgWndProc address points into unmapped
        // memory, and chaining to it crashes on the next activation - which is
        // the moment the user clicks the window.
        WNDPROC cur = (WNDPROC)GetWindowLongPtrA(g_wnd, GWLP_WNDPROC);
        if (cur == BgWndProc) {
            g_agentDebug = (on != 0);
            lstrcpynA(msg, "already subclassed (reusing existing hook)", msgLen);
            return 1;
        }
        o_wndProc = (WNDPROC)SetWindowLongPtrA(g_wnd, GWLP_WNDPROC,
                                               (LONG_PTR)BgWndProc);
        if (!o_wndProc) { lstrcpynA(msg, "could not subclass the window", msgLen); return 0; }
    }
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

    // Cursor ownership. Without these the game clips and recentres the pointer
    // every frame and the desktop becomes unusable while background mode is on.
    p = HookImport("user32.dll", "ClipCursor", (void*)My_ClipCursor);
    if (p) o_ClipCursor = (PFN_ClipCursor)p;
    p = HookImport("user32.dll", "SetCursorPos", (void*)My_SetCursorPos);
    if (p) o_SetCursorPos = (PFN_SetCursorPos)p;
    p = HookImport("user32.dll", "ShowCursor", (void*)My_ShowCursor);
    if (p) o_ShowCursor = (PFN_ShowCursor)p;

    EnumWindows(FindWnd, 0);

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
    wsprintfA(msg, "input: probes installed â€” GetAsyncKeyState=%s GetKeyState=%s "
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
        InterlockedExchange(&g_reallyFocused, focused);
    }

    // Safety net. If the game calls ClipCursor/SetCursorPos through a pointer
    // rather than the import table the hooks above never fire, and the pointer
    // stays trapped. Forcing the clip open every frame while genuinely
    // unfocused cannot be defeated that way. Cheap, and it is the difference
    // between a usable desktop and a locked one.
    if (g_agentDebug && !g_reallyFocused) ClipCursor(nullptr);
    BYTE cur[256];
    SynthState(cur);
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
    wsprintfA(out,
        "dinput: %dkbd/%dmouse/%dother reads=%d/%d | win32: async=%d keystate=%d "
        "kbstate=%d | hwnd=%p",
        (int)g_nKeyboards, (int)g_nMice, (int)g_nOther,
        (int)g_stateCalls, (int)g_dataCalls,
        (int)g_asyncCalls, (int)g_keyStateCalls, (int)g_kbStateCalls, (void*)g_wnd);
    (void)outLen;
}

// ---- the export ----------------------------------------------------------
// Undecorated name for a __stdcall export on x86.
#pragma comment(linker, "/export:DirectInput8Create=_DirectInput8Create@20")

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD ver, REFIID riid,
                                             LPVOID* out, LPUNKNOWN outer) {
    static PFN_Create real = 0;
    if (!real) {
        // dinput8_real.dll is already loaded â€” the other exports in dllmain.cpp
        // still forward to it â€” but resolve by path so we never bind to
        // ourselves if the loader search order ever changes.
        HMODULE m = GetModuleHandleA("dinput8_real.dll");
        if (!m) {
            char path[MAX_PATH];
            GetModuleFileNameA(GetModuleHandleA("dinput8.dll"), path, MAX_PATH);
            char* slash = strrchr(path, '\\');
            if (slash) { lstrcpyA(slash + 1, "dinput8_real.dll"); m = LoadLibraryA(path); }
            if (!m) m = LoadLibraryA("dinput8_real.dll");
        }
        if (!m) return E_FAIL;
        real = (PFN_Create)GetProcAddress(m, "DirectInput8Create");
        if (!real) return E_FAIL;
    }

    HRESULT hr = real(hinst, ver, riid, out, outer);
    if (SUCCEEDED(hr) && out && *out) {
        void* p = PatchSlot(*out, VT_DI_CREATEDEVICE, (void*)My_CreateDevice);
        if (p) {
            o_CreateDevice = (PFN_CreateDevice)p;
            SWSE_Log("input: DirectInput8 proxied â€” CreateDevice hooked");
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
