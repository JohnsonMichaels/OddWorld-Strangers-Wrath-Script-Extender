// SWSE audio mute - see mute.h.

#include "mute.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#pragma comment(lib, "ole32.lib")

static volatile LONG g_muted = 0;

// The process's default session on each playback device - the one the game's
// DirectSound output lands in. GetSimpleAudioVolume(NULL, FALSE) returns it,
// creating it if the game has not played anything on that device yet, so a
// mute set at the first frame is already in place when the sound starts.
static DWORD WINAPI MuteThread(LPVOID arg) {
    BOOL mute = arg ? TRUE : FALSE;
    HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    LONG changed = -1;
    IMMDeviceEnumerator* en = NULL;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), (void**)&en))) {
        changed = 0;
        IMMDeviceCollection* devs = NULL;
        if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devs))) {
            UINT n = 0;
            devs->GetCount(&n);
            for (UINT i = 0; i < n; i++) {
                IMMDevice* dev = NULL;
                if (FAILED(devs->Item(i, &dev))) continue;
                IAudioSessionManager* mgr = NULL;
                if (SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager), CLSCTX_ALL,
                                            NULL, (void**)&mgr))) {
                    ISimpleAudioVolume* vol = NULL;
                    if (SUCCEEDED(mgr->GetSimpleAudioVolume(NULL, FALSE, &vol))) {
                        if (SUCCEEDED(vol->SetMute(mute, NULL))) changed++;
                        vol->Release();
                    }
                    mgr->Release();
                }
                dev->Release();
            }
            devs->Release();
        }
        en->Release();
    }
    if (SUCCEEDED(co)) CoUninitialize();
    if (changed > 0) InterlockedExchange(&g_muted, mute ? 1 : 0);
    return (DWORD)changed;
}

int SWSE_SetGameMute(int mute, unsigned waitMs) {
    HANDLE h = CreateThread(NULL, 0, MuteThread, (LPVOID)(INT_PTR)(mute ? 1 : 0), 0, NULL);
    if (!h) return -1;
    int r = 0;
    if (waitMs && WaitForSingleObject(h, waitMs) == WAIT_OBJECT_0) {
        DWORD code = 0;
        if (GetExitCodeThread(h, &code)) r = (int)(LONG)code;
    }
    CloseHandle(h);
    return r;
}

int SWSE_GameMuted() { return g_muted ? 1 : 0; }
