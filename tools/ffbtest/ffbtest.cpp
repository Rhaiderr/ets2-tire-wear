// F1/F2 check outside the game: ffb::Start() detours the DirectInput reads,
// then this program reads every game controller the way the game does
// (DirectInput 8) and prints what the plugin saw.
//
// ffbtest          read all controllers, no force
// ffbtest force [name]  also take the wheel (exclusive, like the game) and ask the
//                  plugin for -0.3 then +0.3 for 1.5 s each: the wheel turns,
//                  and the printed X axis tells which way each sign pushes
//                  (the plugin's -1 = left).

#include <windows.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "../../src/ffb.h"

#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "user32.lib")

namespace {

struct Found {
    GUID guid;
    DWORD type;
};
std::vector<Found> g_found;

BOOL CALLBACK Add(LPCDIDEVICEINSTANCEW inst, LPVOID) {
    g_found.push_back({inst->guidInstance, inst->dwDevType});
    return DIENUM_CONTINUE;
}

void Print() {
    for (const auto& m : ffb::TakeMessages()) printf("%s\n", m.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    const bool force = argc > 1 && strcmp(argv[1], "force") == 0;
    ffb::Start(false, argc > 2 ? argv[2] : "");
    IDirectInput8W* di = nullptr;
    if (FAILED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                                  reinterpret_cast<void**>(&di), nullptr)))
        return 1;
    di->EnumDevices(DI8DEVCLASS_GAMECTRL, Add, nullptr, DIEDFL_ATTACHEDONLY);
    printf("%zu game controller(s)\n", g_found.size());
    // Exclusive mode needs a top-level window of this process (the console's
    // belongs to conhost): a small one of our own.
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ffbtest";
    RegisterClassW(&wc);
    const HWND hwnd = CreateWindowW(L"ffbtest", L"ffbtest", WS_OVERLAPPEDWINDOW, 0, 0, 200, 100, nullptr, nullptr,
                                    wc.hInstance, nullptr);
    IDirectInputDevice8W* wheel = nullptr;
    for (const Found& f : g_found) {
        IDirectInputDevice8W* dev = nullptr;
        if (FAILED(di->CreateDevice(f.guid, &dev, nullptr))) continue;
        DIDEVCAPS caps = {};
        caps.dwSize = sizeof(caps);
        dev->GetCapabilities(&caps);
        const bool is_wheel = GET_DIDEVICE_TYPE(f.type) == DI8DEVTYPE_DRIVING && (caps.dwFlags & DIDC_FORCEFEEDBACK);
        dev->SetDataFormat(&c_dfDIJoystick2);
        const bool take = force && is_wheel && !wheel;
        const HRESULT coop = dev->SetCooperativeLevel(hwnd, DISCL_BACKGROUND | (take ? DISCL_EXCLUSIVE : DISCL_NONEXCLUSIVE));
        const HRESULT acq = dev->Acquire();
        if (take)
            printf("wheel: SetCooperativeLevel 0x%08lX, Acquire 0x%08lX\n", static_cast<unsigned long>(coop),
                   static_cast<unsigned long>(acq));
        DIJOYSTATE2 st = {};
        const HRESULT poll = dev->Poll();
        const HRESULT read = dev->GetDeviceState(sizeof(st), &st);
        DIDEVICEINSTANCEW info = {};
        info.dwSize = sizeof(info);
        dev->GetDeviceInfo(&info);
        printf("  %ls: type 0x%02X, ffb %d, take %d, coop 0x%08lX, acquire 0x%08lX, poll 0x%08lX, read 0x%08lX\n",
               info.tszProductName, GET_DIDEVICE_TYPE(f.type), (caps.dwFlags & DIDC_FORCEFEEDBACK) ? 1 : 0,
               take ? 1 : 0, static_cast<unsigned long>(coop), static_cast<unsigned long>(acq),
               static_cast<unsigned long>(poll), static_cast<unsigned long>(read));
        if (take) {
            wheel = dev;
            continue;
        }
        dev->Unacquire();
        dev->Release();
    }
    Print();
    printf("active (wheel in use): %s\n", ffb::Active() ? "yes" : "no");
    if (wheel) {
        const float steps[3] = {-0.3f, 0.3f, 0.f};
        for (float s : steps) {
            ffb::SetForce(s);
            DIJOYSTATE2 st = {};
            LONG first = 0;
            for (int i = 0; i < 150; ++i) {
                wheel->Poll();
                wheel->GetDeviceState(sizeof(st), &st);
                if (i == 0) first = st.lX;
                Sleep(10);
            }
            printf("force %+.1f: X axis %ld -> %ld (%s)\n", s, first, st.lX,
                   st.lX < first ? "turned left" : st.lX > first ? "turned right" : "did not move");
        }
        Print();
        wheel->Unacquire();
        wheel->Release();
    }
    di->Release();
    ffb::Stop();
    return 0;
}
