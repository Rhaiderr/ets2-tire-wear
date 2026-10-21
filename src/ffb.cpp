// F1/F2: force feedback on the player's wheel (see ffb.h).

#include "ffb.h"

#include <windows.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>

#include "MinHook.h"

namespace ffb {
namespace {

// IDirectInputDevice8 vtable slots (dinput.h declaration order).
constexpr int kSlotGetDeviceState = 9;
constexpr int kSlotGetDeviceData = 10;
constexpr int kSlotPoll = 25;
constexpr int kMaxSeen = 12;
constexpr DWORD kMinSendMs = 15;    // at most ~60 updates a second to the wheel
constexpr DWORD kRefreshMs = 250;   // resend a steady force now and then
constexpr int kMaxFailures = 200;   // consecutive failed updates before giving up

using GetDeviceStateFn = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, LPVOID);
using GetDeviceDataFn = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
using PollFn = HRESULT(STDMETHODCALLTYPE*)(void*);

// One detour per distinct function: the A and W interfaces may or may not
// share their implementation.
struct Detoured {
    void* target = nullptr;
    void* original = nullptr;
};
Detoured g_state[2], g_data[2], g_poll[2];  // [0] = W, [1] = A

std::atomic<bool> g_passthrough{true};
std::atomic<void*> g_wheel_fast{nullptr};  // the wheel, for the per-read fast path
std::atomic<bool> g_active{false};
std::atomic<bool> g_failed{false};
std::atomic<float> g_force{0.f};
bool g_invert = false;
std::string g_device_filter;  // lower case; empty = any wheel

std::mutex g_mutex;  // the fields below
std::vector<std::string> g_messages;
void* g_seen[kMaxSeen] = {};
int g_seen_count = 0;
IDirectInputDevice8W* g_wheel = nullptr;  // AddRef'd while we hold it (may be an A object)
bool g_wheel_driving = false;
DWORD g_axis = 0;                          // object id of the force feedback axis
IDirectInputEffect* g_effect = nullptr;
float g_sent = 0.f;
DWORD g_sent_tick = 0;
int g_failures = 0;
std::string g_wheel_name;

void Message(const std::string& s) { g_messages.push_back("ffb: " + s); }  // g_mutex held

void MessageLocked(const std::string& s) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Message(s);
}

std::string Narrow(const wchar_t* w) {
    char b[MAX_PATH] = {};
    WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof(b), nullptr, nullptr);
    return b;
}

std::string Lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

struct Info {
    DWORD flags = 0;     // DIDEVCAPS
    DWORD dev_type = 0;  // DIDEVICEINSTANCE
    bool wide = true;    // a W object (else A)
    wchar_t name_w[MAX_PATH] = {};
    char name_a[MAX_PATH] = {};
};

// The device's capabilities, type and name. The object may be an A or a W
// device; only GetDeviceInfo differs between them. No C++ objects with
// destructors here, so __try is allowed.
bool Inspect(void* dev, Info* info) {
    __try {
        auto* d = static_cast<IDirectInputDevice8W*>(dev);
        DIDEVCAPS caps = {};
        caps.dwSize = sizeof(caps);
        if (FAILED(d->GetCapabilities(&caps))) return false;
        info->flags = caps.dwFlags;
        DIDEVICEINSTANCEW iw = {};
        iw.dwSize = sizeof(iw);
        if (SUCCEEDED(d->GetDeviceInfo(&iw))) {
            info->dev_type = iw.dwDevType;
            wcsncpy_s(info->name_w, MAX_PATH, iw.tszProductName, _TRUNCATE);
            return true;
        }
        DIDEVICEINSTANCEA ia = {};
        ia.dwSize = sizeof(ia);
        if (SUCCEEDED(reinterpret_cast<IDirectInputDevice8A*>(dev)->GetDeviceInfo(&ia))) {
            info->wide = false;
            info->dev_type = ia.dwDevType;
            strncpy_s(info->name_a, MAX_PATH, ia.tszProductName, _TRUNCATE);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

BOOL CALLBACK AxisW(LPCDIDEVICEOBJECTINSTANCEW o, LPVOID ctx) {
    if (!(o->dwFlags & DIDOI_FFACTUATOR)) return DIENUM_CONTINUE;
    *static_cast<DWORD*>(ctx) = o->dwType;
    return DIENUM_STOP;
}
BOOL CALLBACK AxisA(LPCDIDEVICEOBJECTINSTANCEA o, LPVOID ctx) {
    if (!(o->dwFlags & DIDOI_FFACTUATOR)) return DIENUM_CONTINUE;
    *static_cast<DWORD*>(ctx) = o->dwType;
    return DIENUM_STOP;
}

// The first axis that can be driven (the steering axis on a wheel), by object id.
DWORD FindAxis(void* dev, bool wide) {
    DWORD axis = 0;
    __try {
        if (wide)
            static_cast<IDirectInputDevice8W*>(dev)->EnumObjects(AxisW, &axis, DIDFT_AXIS);
        else
            static_cast<IDirectInputDevice8A*>(dev)->EnumObjects(AxisA, &axis, DIDFT_AXIS);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        axis = 0;
    }
    return axis;
}

void DropEffectLocked() {
    if (!g_effect) return;
    __try {
        g_effect->Stop();
        g_effect->Release();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    g_effect = nullptr;
    g_sent = 0.f;
}

// A constant force on the wheel's axis, -1 (left) ... 1 (right). DirectInput
// takes the opposite sign here [verified: ffbtest force on the G923, -0.3
// turned it right and +0.3 left].
HRESULT SendLocked(float force) {
    DICONSTANTFORCE cf = {static_cast<LONG>(std::lround(-force * DI_FFNOMINALMAX))};
    DWORD axes[1] = {g_axis};
    LONG dir[1] = {1};
    DIEFFECT e = {};
    e.dwSize = sizeof(e);
    e.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTIDS;
    e.dwDuration = INFINITE;
    e.dwGain = DI_FFNOMINALMAX;
    e.dwTriggerButton = DIEB_NOTRIGGER;
    e.cAxes = 1;
    e.rgdwAxes = axes;
    e.rglDirection = dir;
    e.cbTypeSpecificParams = sizeof(cf);
    e.lpvTypeSpecificParams = &cf;
    HRESULT hr = E_FAIL;
    __try {
        if (!g_effect) {
            hr = g_wheel->CreateEffect(GUID_ConstantForce, &e, &g_effect, nullptr);
            if (SUCCEEDED(hr)) hr = g_effect->Start(1, 0);
        } else {
            hr = g_effect->SetParameters(&e, DIEP_TYPESPECIFICPARAMS);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        hr = E_UNEXPECTED;
    }
    return hr;
}

// Right after a read of the wheel by the game (its input thread): bring the
// force up to date.
void Drive() {
    if (g_failed.load(std::memory_order_relaxed)) return;
    float target = g_force.load(std::memory_order_relaxed);
    if (g_invert) target = -target;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_wheel) return;
    const DWORD now = GetTickCount();
    if (!g_effect && target == 0.f) return;  // nothing to do until the first force
    const bool changed = std::fabs(target - g_sent) > 0.005f;
    if (!changed && now - g_sent_tick < kRefreshMs) return;
    if (now - g_sent_tick < kMinSendMs) return;
    const bool creating = g_effect == nullptr;
    const HRESULT hr = SendLocked(target);
    g_sent_tick = now;
    if (SUCCEEDED(hr)) {
        if (creating) Message("force on '" + g_wheel_name + "'");
        g_sent = target;
        g_failures = 0;
        return;
    }
    if (hr == DIERR_NOTEXCLUSIVEACQUIRED || hr == DIERR_NOTACQUIRED || hr == DIERR_INPUTLOST) {
        // The game let go of the wheel for now (alt-tab, menus): start the
        // force again once it has it back.
        DropEffectLocked();
        return;
    }
    if (creating || hr == DIERR_INVALIDPARAM) DropEffectLocked();  // try again from scratch
    if (++g_failures == 1) {
        char b[96];
        snprintf(b, sizeof(b), "force update failed 0x%08lX (retrying)", static_cast<unsigned long>(hr));
        Message(b);
    }
    if (g_failures >= kMaxFailures) {
        DropEffectLocked();
        g_failed.store(true);
        g_active.store(false);
        Message("off for this session: the wheel keeps refusing the force; back to the steering offset");
    }
}

// Every device is looked at once: among those with force feedback, a driving
// device (a wheel) wins over any other, e.g. a virtual joystick. With
// ffb_device set, only a device with that in its name is used, whatever its type.
void Observe(void* dev, const char* via) {
    if (g_passthrough.load(std::memory_order_relaxed)) return;
    if (dev == g_wheel_fast.load(std::memory_order_relaxed)) {
        Drive();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (int i = 0; i < g_seen_count; ++i)
            if (g_seen[i] == dev) return;
        if (g_seen_count == kMaxSeen) return;
        g_seen[g_seen_count++] = dev;
    }
    Info info;
    if (!Inspect(dev, &info)) {
        MessageLocked(std::string("a device read via ") + via + " could not be inspected");
        return;
    }
    const std::string name = info.name_w[0] ? Narrow(info.name_w) : std::string(info.name_a);
    if (!(info.flags & DIDC_FORCEFEEDBACK)) {
        MessageLocked("seen '" + name + "' (no force feedback), read via " + via);
        return;
    }
    const bool chosen = !g_device_filter.empty();
    if (chosen && Lower(name).find(g_device_filter) == std::string::npos) {
        MessageLocked("force feedback, not used: '" + name + "' (ffb_device = " + g_device_filter + ")");
        return;
    }
    // A device picked by name counts as the wheel.
    const bool driving = chosen || GET_DIDEVICE_TYPE(info.dev_type) == DI8DEVTYPE_DRIVING;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_passthrough.load() || g_failed.load()) return;
    if (g_wheel && (g_wheel_driving || !driving)) {
        Message("also force feedback, not used: '" + name + "'" + (driving ? " (wheel)" : ""));
        return;
    }
    const DWORD axis = FindAxis(dev, info.wide);
    if (!axis) {
        Message("'" + name + "' has force feedback but no axis to drive; not used");
        return;
    }
    if (g_wheel) {
        Message("'" + g_wheel_name + "' let go for a wheel");
        DropEffectLocked();
        g_wheel->Release();
    }
    g_wheel = static_cast<IDirectInputDevice8W*>(dev);
    g_wheel->AddRef();
    g_wheel_driving = driving;
    g_wheel_name = name;
    g_axis = axis;
    g_failures = 0;
    g_wheel_fast.store(dev);
    g_active.store(driving);
    Message("captured '" + name + "'" +
            (chosen    ? " (ffb_device)"
             : driving ? " (wheel)"
                       : " (not a wheel: no force, steering offset kept)") +
            ", read by the game via " + via);
}

HRESULT STDMETHODCALLTYPE StateW(void* dev, DWORD size, LPVOID data) {
    const HRESULT hr = reinterpret_cast<GetDeviceStateFn>(g_state[0].original)(dev, size, data);
    Observe(dev, "GetDeviceState");
    return hr;
}
HRESULT STDMETHODCALLTYPE StateA(void* dev, DWORD size, LPVOID data) {
    const HRESULT hr = reinterpret_cast<GetDeviceStateFn>(g_state[1].original)(dev, size, data);
    Observe(dev, "GetDeviceState");
    return hr;
}
HRESULT STDMETHODCALLTYPE DataW(void* dev, DWORD size, LPDIDEVICEOBJECTDATA od, LPDWORD n, DWORD fl) {
    const HRESULT hr = reinterpret_cast<GetDeviceDataFn>(g_data[0].original)(dev, size, od, n, fl);
    Observe(dev, "GetDeviceData");
    return hr;
}
HRESULT STDMETHODCALLTYPE DataA(void* dev, DWORD size, LPDIDEVICEOBJECTDATA od, LPDWORD n, DWORD fl) {
    const HRESULT hr = reinterpret_cast<GetDeviceDataFn>(g_data[1].original)(dev, size, od, n, fl);
    Observe(dev, "GetDeviceData");
    return hr;
}
HRESULT STDMETHODCALLTYPE PollW(void* dev) {
    const HRESULT hr = reinterpret_cast<PollFn>(g_poll[0].original)(dev);
    Observe(dev, "Poll");
    return hr;
}
HRESULT STDMETHODCALLTYPE PollA(void* dev) {
    const HRESULT hr = reinterpret_cast<PollFn>(g_poll[1].original)(dev);
    Observe(dev, "Poll");
    return hr;
}

BOOL CALLBACK FirstController(LPCDIDEVICEINSTANCEW inst, LPVOID ctx) {
    *static_cast<GUID*>(ctx) = inst->guidInstance;
    return DIENUM_STOP;
}

void* Slot(void* com_object, int slot) { return (*static_cast<void***>(com_object))[slot]; }

bool Detour(Detoured& d, void* target, void* detour, const char* name) {
    // The W and A interfaces may share a function: one detour serves both.
    for (Detoured* other : {&g_state[0], &g_state[1], &g_data[0], &g_data[1], &g_poll[0], &g_poll[1]})
        if (other != &d && other->target == target) return true;
    MH_STATUS st = MH_CreateHook(target, detour, &d.original);
    if (st == MH_OK) st = MH_EnableHook(target);
    if (st != MH_OK) {
        MessageLocked(std::string("off: detour of ") + name + " failed: " + MH_StatusToString(st));
        return false;
    }
    d.target = target;
    return true;
}

}  // namespace

void Start(bool invert, const std::string& device) {
    static bool started = false;
    if (started) return;
    started = true;
    g_invert = invert;
    g_device_filter = Lower(device);

    // The real dinput8.dll of Windows, under any proxy of the same name in the game folder.
    wchar_t path[MAX_PATH] = {};
    GetSystemDirectoryW(path, MAX_PATH);
    wcscat_s(path, L"\\dinput8.dll");
    HMODULE di = LoadLibraryW(path);
    using CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    auto create = di ? reinterpret_cast<CreateFn>(GetProcAddress(di, "DirectInput8Create")) : nullptr;
    if (!create) {
        MessageLocked("off: system dinput8.dll not available");
        return;
    }
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&Start), &self)) {
        MessageLocked("off: could not pin the DLL");
        return;
    }
    IDirectInput8W* dw = nullptr;
    IDirectInput8A* da = nullptr;
    if (FAILED(create(self, DIRECTINPUT_VERSION, IID_IDirectInput8W, reinterpret_cast<void**>(&dw), nullptr)) ||
        FAILED(create(self, DIRECTINPUT_VERSION, IID_IDirectInput8A, reinterpret_cast<void**>(&da), nullptr))) {
        if (dw) dw->Release();
        MessageLocked("off: DirectInput8Create failed");
        return;
    }
    GUID guid = {};
    dw->EnumDevices(DI8DEVCLASS_GAMECTRL, FirstController, &guid, DIEDFL_ATTACHEDONLY);
    IDirectInputDevice8W* devw = nullptr;
    IDirectInputDevice8A* deva = nullptr;
    const bool any = guid != GUID{};
    if (any) {
        dw->CreateDevice(guid, &devw, nullptr);
        da->CreateDevice(guid, &deva, nullptr);
    }
    if (!devw || !deva) {
        if (devw) devw->Release();
        if (deva) deva->Release();
        dw->Release();
        da->Release();
        MessageLocked(any ? "off: could not open the game controller" : "off: no game controller attached");
        return;
    }
    // Our own device objects are only used to find the functions, then released.
    void* targets[2][3] = {{Slot(devw, kSlotGetDeviceState), Slot(devw, kSlotGetDeviceData), Slot(devw, kSlotPoll)},
                           {Slot(deva, kSlotGetDeviceState), Slot(deva, kSlotGetDeviceData), Slot(deva, kSlotPoll)}};
    devw->Release();
    deva->Release();
    dw->Release();
    da->Release();

    const MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        MessageLocked(std::string("off: MH_Initialize ") + MH_StatusToString(st));
        return;
    }
    g_passthrough.store(false);
    bool ok = Detour(g_state[0], targets[0][0], reinterpret_cast<void*>(&StateW), "GetDeviceState") &&
              Detour(g_state[1], targets[1][0], reinterpret_cast<void*>(&StateA), "GetDeviceState (A)") &&
              Detour(g_data[0], targets[0][1], reinterpret_cast<void*>(&DataW), "GetDeviceData") &&
              Detour(g_data[1], targets[1][1], reinterpret_cast<void*>(&DataA), "GetDeviceData (A)") &&
              Detour(g_poll[0], targets[0][2], reinterpret_cast<void*>(&PollW), "Poll") &&
              Detour(g_poll[1], targets[1][2], reinterpret_cast<void*>(&PollA), "Poll (A)");
    if (!ok) {
        g_passthrough.store(true);
        return;
    }
    wchar_t mod[MAX_PATH] = {};
    GetModuleFileNameW(di, mod, MAX_PATH);
    MessageLocked("watching the game's DirectInput reads (" + Narrow(mod) + ", DLL pinned" +
                  (g_invert ? ", force inverted" : "") +
                  (g_device_filter.empty() ? "" : ", ffb_device = " + g_device_filter) + ")");
}

void Stop() {
    g_passthrough.store(true);
    g_active.store(false);
    g_wheel_fast.store(nullptr);
    g_force.store(0.f);
    std::lock_guard<std::mutex> lock(g_mutex);
    DropEffectLocked();
    if (g_wheel) {
        g_wheel->Release();
        g_wheel = nullptr;
    }
}

bool Active() { return g_active.load(std::memory_order_relaxed) && !g_failed.load(std::memory_order_relaxed); }

void SetForce(float force) { g_force.store(force < -1.f ? -1.f : force > 1.f ? 1.f : force, std::memory_order_relaxed); }

std::vector<std::string> TakeMessages() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<std::string> out;
    out.swap(g_messages);
    return out;
}

}  // namespace ffb
