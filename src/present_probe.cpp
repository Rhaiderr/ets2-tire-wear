// Present hook plan, tests A and B.
//
// The game creates its swap chain before SDK plugins load, and a dxgi.dll
// wrapper (SnowyMoon) sits in the game folder. To learn where the system
// IDXGISwapChain::Present lives we create a minimal 1x1 device + swap chain
// through the factory of the *system* dxgi.dll (the copy the wrapper itself
// loaded), read the vtable and release everything.
//
// Test A (probe = 1): log the addresses and who already jumps out of them.
// Test B (hook = 1): also detour Present / Present1 / ResizeBuffers with
// MinHook. The detours only count calls and log the frame rate; nothing is
// drawn. Once installed the detours are never removed: on shutdown they turn
// into pass-through and the DLL is pinned, so no jump ever points at freed
// memory even if another hook in the chain goes away first.

#include "present_probe.h"

#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>

#include "MinHook.h"
#include "overlay.h"
#include "tire_probe.h"

namespace present_probe {
namespace {

std::mutex g_mutex;  // guards g_messages and g_file
std::vector<std::string> g_messages;
std::thread g_thread;
FILE* g_file = nullptr;
bool g_hook_enabled = false;
bool g_overlay_enabled = false;
bool g_tires_enabled = false;

void Emit(const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(g_file, "%02d:%02d:%02d.%03d %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                msg.c_str());
        fflush(g_file);
    }
    g_messages.push_back("present probe: " + msg);
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string Hex(uintptr_t v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(v));
    return buf;
}

std::string ModulePath(HMODULE m) {
    wchar_t path[MAX_PATH] = {};
    if (!m || !GetModuleFileNameW(m, path, MAX_PATH)) return "?";
    return Narrow(path);
}

bool Readable(const void* addr, size_t n) {
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(addr, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    const auto end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return reinterpret_cast<uintptr_t>(addr) + n <= end;
}

// Destination of a jump at addr (jmp rel32, jmp [rip+x], mov rax,imm64; jmp rax), or null.
const void* JumpTarget(const void* addr) {
    if (!Readable(addr, 16)) return nullptr;
    const auto* p = static_cast<const unsigned char*>(addr);
    if (p[0] == 0xE9) return p + 5 + *reinterpret_cast<const int32_t*>(p + 1);
    if (p[0] == 0xFF && p[1] == 0x25) {
        const auto* slot = p + 6 + *reinterpret_cast<const int32_t*>(p + 2);
        if (!Readable(slot, 8)) return nullptr;
        return *reinterpret_cast<const void* const*>(slot);
    }
    if (p[0] == 0x48 && p[1] == 0xB8 && p[10] == 0xFF && p[11] == 0xE0)
        return *reinterpret_cast<const void* const*>(p + 2);
    return nullptr;
}

std::string Where(const void* addr) {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(addr), &m);
    if (!m) return Hex(reinterpret_cast<uintptr_t>(addr)) + " (no module: relay/heap)";
    return Hex(reinterpret_cast<uintptr_t>(addr)) + " = " + ModulePath(m) + "+" +
           Hex(reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(m));
}

// Address, first bytes, and the chain of jumps that starts there (who hooked it).
std::string Describe(const void* addr) {
    std::string s = Where(addr) + ", bytes:";
    const auto* p = static_cast<const unsigned char*>(addr);
    char b[4];
    for (int i = 0; i < 16; ++i) {
        snprintf(b, sizeof(b), " %02X", p[i]);
        s += b;
    }
    const void* cur = addr;
    for (int hop = 0; hop < 4; ++hop) {
        const void* next = JumpTarget(cur);
        if (!next) break;
        s += "\n    -> " + Where(next);
        cur = next;
    }
    return s;
}

void LogModule(const wchar_t* name) {
    HMODULE m = GetModuleHandleW(name);
    Emit(Narrow(name) + ": " + (m ? ModulePath(m) : std::string("not loaded")));
}

struct Found {
    void* present = nullptr;
    void* present1 = nullptr;
    void* resize_buffers = nullptr;
    tire_probe::Targets d3d;  // device, immediate and deferred context functions
};

// Reads the tire probe's context slots from a context's vtable(s).
void ReadContextSlots(ID3D11DeviceContext* ctx, void** out) {
    void** vt = *reinterpret_cast<void***>(ctx);
    for (int fn = 0; fn < tire_probe::kCtxFnCount; ++fn)
        if (fn != tire_probe::kUpdateSubresource1) out[fn] = vt[tire_probe::CtxFnSlot(fn)];
    ID3D11DeviceContext1* ctx1 = nullptr;
    if (SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1)))) {
        out[tire_probe::kUpdateSubresource1] =
            (*reinterpret_cast<void***>(ctx1))[tire_probe::CtxFnSlot(tire_probe::kUpdateSubresource1)];
        ctx1->Release();
    }
}

// Plain C-style body so it can run under __try in the caller.
HRESULT FindSwapChainFunctions(Found* out, std::string* step) {
    wchar_t sysdir[MAX_PATH] = {};
    GetSystemDirectoryW(sysdir, MAX_PATH);
    const std::wstring sys_dxgi_path = std::wstring(sysdir) + L"\\dxgi.dll";

    // Never load anything new: both modules must already be in the process.
    *step = "system dxgi.dll";
    HMODULE sys_dxgi = GetModuleHandleW(sys_dxgi_path.c_str());
    if (!sys_dxgi) return E_FAIL;
    *step = "d3d11.dll";
    HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
    if (!d3d11) return E_FAIL;

    using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
    auto create_factory = reinterpret_cast<CreateFactoryFn>(GetProcAddress(sys_dxgi, "CreateDXGIFactory1"));
    auto create_device = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(d3d11, "D3D11CreateDevice"));
    *step = "exports";
    if (!create_factory || !create_device) return E_FAIL;

    HRESULT hr;
    IDXGIFactory1* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    ID3D11Device* device = nullptr;
    IDXGISwapChain* swap = nullptr;
    IDXGISwapChain1* swap1 = nullptr;
    ID3D11DeviceContext* imm = nullptr;
    ID3D11DeviceContext* def = nullptr;
    HWND wnd = nullptr;

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ets2_tire_wear_probe";
    RegisterClassExW(&wc);

    do {
        *step = "CreateDXGIFactory1";
        if (FAILED(hr = create_factory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) break;
        *step = "EnumAdapters1";
        if (FAILED(hr = factory->EnumAdapters1(0, &adapter))) break;
        *step = "D3D11CreateDevice";
        const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        if (FAILED(hr = create_device(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &fl, 1, D3D11_SDK_VERSION,
                                      &device, nullptr, nullptr)))
            break;
        *step = "CreateWindow";
        wnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance,
                              nullptr);
        if (!wnd) {
            hr = E_FAIL;
            break;
        }
        DXGI_SWAP_CHAIN_DESC sd = {};
        sd.BufferDesc.Width = 1;
        sd.BufferDesc.Height = 1;
        sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 1;
        sd.OutputWindow = wnd;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        *step = "CreateSwapChain";
        if (FAILED(hr = factory->CreateSwapChain(device, &sd, &swap))) break;

        void** vt = *reinterpret_cast<void***>(swap);
        out->present = vt[8];          // IDXGISwapChain::Present
        out->resize_buffers = vt[13];  // IDXGISwapChain::ResizeBuffers
        if (SUCCEEDED(swap->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&swap1))))
            out->present1 = (*reinterpret_cast<void***>(swap1))[22];  // IDXGISwapChain1::Present1

        void** dvt = *reinterpret_cast<void***>(device);
        out->d3d.create_texture2d = dvt[5];  // ID3D11Device::CreateTexture2D
        out->d3d.create_srv = dvt[7];        // ID3D11Device::CreateShaderResourceView
        *step = "GetImmediateContext";
        device->GetImmediateContext(&imm);
        if (imm) ReadContextSlots(imm, out->d3d.ctx[0]);
        *step = "CreateDeferredContext";
        if (SUCCEEDED(device->CreateDeferredContext(0, &def))) ReadContextSlots(def, out->d3d.ctx[1]);
        *step = "done";
    } while (false);

    if (def) def->Release();
    if (imm) imm->Release();
    if (swap1) swap1->Release();
    if (swap) swap->Release();
    if (device) device->Release();
    if (adapter) adapter->Release();
    if (factory) factory->Release();
    if (wnd) DestroyWindow(wnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return hr;
}

HRESULT GuardedFind(Found* out, std::string* step, DWORD* seh_code) {
    __try {
        return FindSwapChainFunctions(out, step);
    } __except (*seh_code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return E_UNEXPECTED;
    }
}

// ---- test B: counting detours ----

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

PresentFn g_orig_present = nullptr;
Present1Fn g_orig_present1 = nullptr;
ResizeBuffersFn g_orig_resize = nullptr;

// Statics survive a plugin reload because the DLL is pinned once hooked.
std::atomic<bool> g_installed{false};
std::atomic<bool> g_passthrough{true};
std::atomic<uint64_t> g_game_presents{0};
std::atomic<uint64_t> g_present1_calls{0};
std::atomic<uint64_t> g_other_presents{0};
std::atomic<void*> g_cached_swap{nullptr};
std::atomic<bool> g_cached_is_game{false};
std::atomic<HWND> g_game_wnd{nullptr};
uint64_t g_stats_t0 = 0;            // render thread only
uint64_t g_stats_presents0 = 0;     // render thread only

// The game's swap chain is the one whose window is a visible top-level
// window of this process (the dummy window from the probe is gone by then).
bool IsGameSwapChain(IDXGISwapChain* sc) {
    if (sc == g_cached_swap.load(std::memory_order_relaxed)) return g_cached_is_game.load(std::memory_order_relaxed);
    DXGI_SWAP_CHAIN_DESC d = {};
    bool is_game = false;
    if (SUCCEEDED(sc->GetDesc(&d)) && d.OutputWindow) {
        DWORD pid = 0;
        GetWindowThreadProcessId(d.OutputWindow, &pid);
        is_game = pid == GetCurrentProcessId() && GetAncestor(d.OutputWindow, GA_ROOT) == d.OutputWindow &&
                  IsWindowVisible(d.OutputWindow);
        if (is_game && g_game_wnd.exchange(d.OutputWindow) != d.OutputWindow)
            Emit("game swap chain " + Hex(reinterpret_cast<uintptr_t>(sc)) + ", window " +
                 Hex(reinterpret_cast<uintptr_t>(d.OutputWindow)) + ", " + std::to_string(d.BufferDesc.Width) +
                 "x" + std::to_string(d.BufferDesc.Height) + ", format " + std::to_string(d.BufferDesc.Format) +
                 ", buffers " + std::to_string(d.BufferCount) + ", swap effect " + std::to_string(d.SwapEffect));
    }
    g_cached_is_game.store(is_game, std::memory_order_relaxed);
    g_cached_swap.store(sc, std::memory_order_relaxed);
    return is_game;
}

// Returns true when this is a real frame of the game's swap chain.
bool CountPresent(IDXGISwapChain* sc, UINT flags) {
    if (flags & DXGI_PRESENT_TEST) return false;
    if (!IsGameSwapChain(sc)) {
        g_other_presents.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const uint64_t n = g_game_presents.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint64_t now = GetTickCount64();
    if (g_stats_t0 == 0) {
        g_stats_t0 = now;
        g_stats_presents0 = n;
        Emit("first game Present reached");
    } else if (now - g_stats_t0 >= 60000) {
        const double fps = (n - g_stats_presents0) * 1000.0 / static_cast<double>(now - g_stats_t0);
        char buf[160];
        snprintf(buf, sizeof(buf), "stats: %.1f fps over %.0f s (game presents %llu, via Present1 %llu, other %llu)",
                 fps, (now - g_stats_t0) / 1000.0, static_cast<unsigned long long>(n),
                 static_cast<unsigned long long>(g_present1_calls.load()),
                 static_cast<unsigned long long>(g_other_presents.load()));
        Emit(buf);
        if (g_tires_enabled) Emit(tire_probe::Stats());
        g_stats_t0 = now;
        g_stats_presents0 = n;
    }
    return true;
}

// Shared by Present and Present1: count, mark the frame for the tire probe,
// then draw the HUD on game frames. In pass-through mode the HUD drops its
// resources on this (presenting) thread.
void BeforePresent(IDXGISwapChain* sc, UINT flags) {
    if (g_passthrough.load(std::memory_order_relaxed)) {
        if (g_overlay_enabled && overlay::HasResources()) overlay::ReleaseAll();
        return;
    }
    if (!CountPresent(sc, flags)) return;
    if (g_tires_enabled) tire_probe::OnPresent(sc);
    if (g_overlay_enabled) overlay::OnPresent(sc);
}

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    BeforePresent(sc, flags);
    return g_orig_present(sc, sync, flags);
}

HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                       const DXGI_PRESENT_PARAMETERS* params) {
    if (!g_passthrough.load(std::memory_order_relaxed)) g_present1_calls.fetch_add(1, std::memory_order_relaxed);
    BeforePresent(sc, flags);
    return g_orig_present1(sc, sync, flags, params);
}

HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt,
                                            UINT flags) {
    if (g_overlay_enabled) overlay::OnBeforeResize(sc);
    const HRESULT hr = g_orig_resize(sc, count, w, h, fmt, flags);
    if (!g_passthrough.load(std::memory_order_relaxed))
        Emit("ResizeBuffers " + std::to_string(w) + "x" + std::to_string(h) + " -> hr " +
             Hex(static_cast<uint32_t>(hr)) + (sc == g_cached_swap.load() ? " (game swap chain)" : ""));
    return hr;
}

void InstallHooks(const Found& f) {
    if (g_installed.load()) {
        Emit("hooks already installed earlier in this process: counting again");
        g_passthrough.store(false);
        return;
    }
    // Pin first: once a jump to us exists this DLL must never unload.
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&InstallHooks), &self)) {
        Emit("hook off: could not pin the DLL");
        return;
    }
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        Emit(std::string("hook off: MH_Initialize ") + MH_StatusToString(st));
        return;
    }
    std::vector<tire_probe::Hook> items = {
        {"Present", f.present, reinterpret_cast<void*>(&HookPresent), reinterpret_cast<void**>(&g_orig_present)},
        {"ResizeBuffers", f.resize_buffers, reinterpret_cast<void*>(&HookResizeBuffers),
         reinterpret_cast<void**>(&g_orig_resize)},
        {"Present1", f.present1, reinterpret_cast<void*>(&HookPresent1), reinterpret_cast<void**>(&g_orig_present1)},
    };
    if (g_tires_enabled) {
        tire_probe::Hook extra[tire_probe::kMaxHooks];
        const int n = tire_probe::MakeHooks(f.d3d, extra);
        items.insert(items.end(), extra, extra + n);
    }
    // All or nothing: a partial set would leave the probe half blind.
    for (auto& it : items) {
        if (!it.target) continue;
        st = MH_CreateHook(it.target, it.detour, it.original);
        if (st != MH_OK) {
            Emit(std::string("hook off: MH_CreateHook ") + it.name + " " + MH_StatusToString(st));
            MH_RemoveHook(MH_ALL_HOOKS);
            return;
        }
    }
    g_passthrough.store(false);
    if (g_tires_enabled) tire_probe::SetActive(true);
    st = MH_EnableHook(MH_ALL_HOOKS);
    if (st != MH_OK) {
        g_passthrough.store(true);
        tire_probe::SetActive(false);
        MH_DisableHook(MH_ALL_HOOKS);
        Emit(std::string("hook off: MH_EnableHook ") + MH_StatusToString(st));
        return;
    }
    g_installed.store(true);
    Emit("hooks installed (" + std::to_string(items.size()) + " functions; logging only, the game's drawing is not "
         "changed); DLL pinned");
    for (auto& it : items)
        if (it.target) Emit(std::string("after hook ") + it.name + " " + Describe(it.target));
}

void Run() {
    Emit(std::string("start (") + (g_hook_enabled ? "detours" : "locate only, no hook") +
         (g_tires_enabled ? " + tire probe R5d" : "") + (g_overlay_enabled ? " + HUD" : "") + ")");
    LogModule(L"dxgi.dll");
    LogModule(L"dinput8.dll");
    LogModule(L"d3d11.dll");
    LogModule(L"NvPresent64.dll");
    LogModule(L"GameOverlayRenderer64.dll");
    LogModule(L"DiscordHook64.dll");
    LogModule(L"TMRealWalk.dll");
    LogModule(L"ets2_brake_fade.dll");

    Found f;
    std::string step;
    DWORD seh_code = 0;
    const auto t0 = GetTickCount64();
    const HRESULT hr = GuardedFind(&f, &step, &seh_code);
    const auto ms = GetTickCount64() - t0;

    if (seh_code) {
        Emit("off: exception " + Hex(seh_code) + " at step " + step);
        return;
    }
    if (FAILED(hr)) {
        Emit("off: failed at step " + step + ", hr=" + Hex(static_cast<uint32_t>(hr)));
        return;
    }
    Emit("dummy device + swap chain created and released in " + std::to_string(ms) + " ms");
    Emit("Present       " + Describe(f.present));
    Emit("ResizeBuffers " + Describe(f.resize_buffers));
    if (f.present1) Emit("Present1      " + Describe(f.present1));
    else Emit("Present1      not available");
    if (g_tires_enabled) {
        Emit("CreateTexture2D          " + Describe(f.d3d.create_texture2d));
        Emit("CreateShaderResourceView " + Describe(f.d3d.create_srv));
        for (int fn = 0; fn < tire_probe::kCtxFnCount; ++fn) {
            const void* imm = f.d3d.ctx[0][fn];
            const void* def = f.d3d.ctx[1][fn];
            Emit(std::string(tire_probe::CtxFnName(fn)) + " " + (imm ? Describe(imm) : std::string("not found")) +
                 (def == imm ? " (deferred: same)" : def ? "\n    deferred: " + Describe(def) : " (deferred: none)"));
        }
    }

    if (g_hook_enabled) InstallHooks(f);
    else Emit("done; nothing was hooked");
}

// Watchdog state (game thread only).
uint64_t g_wd_last_count = 0;
uint64_t g_wd_last_change = 0;
bool g_wd_stalled = false;

}  // namespace

Config LoadConfig(const std::wstring& tirewear_dir) {
    const std::wstring ini = tirewear_dir + L"\\ets2_tire_wear.ini";
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (FILE* f = _wfopen(ini.c_str(), L"wb")) {
            fputs("; ets2_tire_wear settings (created by the plugin: worn tires on, tests off)\r\n"
                  "[present]\r\n"
                  "; 1 = after the world loads, locate the system DXGI Present and log it\r\n"
                  "probe = 1\r\n"
                  "; 1 = also detour Present (count frames); needs probe = 1\r\n"
                  "hook = 1\r\n"
                  "; 1 = draw the wear HUD from the Present detour; needs hook = 1\r\n"
                  "overlay = 0\r\n"
                  "; HUD font scale and top-left position in pixels\r\n"
                  "overlay_scale = 1.6\r\n"
                  "overlay_x = 20\r\n"
                  "overlay_y = 20\r\n"
                  "; seconds to wait after the first unpaused frame\r\n"
                  "delay_s = 10\r\n"
                  "[tires]\r\n"
                  "; 1 = hook early (at plugin load) and recognise the tire textures; needs hook = 1\r\n"
                  "log = 1\r\n"
                  "; 1 = write a capture of tire draws, capture_delay_s after the first unpaused frame (diagnosis)\r\n"
                  "capture = 0\r\n"
                  "capture_delay_s = 30\r\n"
                  "; second capture (0 = none), e.g. while driving\r\n"
                  "capture2_delay_s = 0\r\n"
                  "; consecutive frames per capture (1-60); more than 1 also logs what the separation saw\r\n"
                  "capture_frames = 1\r\n"
                  "; 1 = draw tires worn according to the telemetry wear; needs log = 1\r\n"
                  "wear_levels = 1\r\n"
                  "; number of wear levels (level 0 = the game's own texture)\r\n"
                  "levels = 8\r\n"
                  "; test only: 0.0 ... 1.0 forces this wear on every tire; -1 = use the telemetry\r\n"
                  "wear_override = -1\r\n"
                  "; test only: 1 = worn tire colour painted magenta\r\n"
                  "tint_test = 0\r\n"
                  "; 1 = only the player's truck and trailers wear (traffic keeps the game's tires)\r\n"
                  "separate = 1\r\n"
                  "[blowout]\r\n"
                  "; 1 = worn tires can blow out (single player only; off by itself in multiplayer)\r\n"
                  "enabled = 1\r\n"
                  "; below this wear (0.0 ... 1.0) a tire never blows\r\n"
                  "start_wear = 0.60\r\n"
                  "; mean km until a blowout at 100% wear (the risk grows from start_wear to 100%)\r\n"
                  "km_at_100 = 300\r\n"
                  "; how steeply the risk grows between start_wear and 100% (1 = linear)\r\n"
                  "curve = 3\r\n"
                  "; test only: > 0 blows a tire after this many seconds of driving\r\n"
                  "test_after_s = 0\r\n"
                  "; test only: truck or trailer (the first trailer)\r\n"
                  "test_unit = truck\r\n"
                  "; test only: telemetry wheel index to blow (-1 = random)\r\n"
                  "test_wheel = -1\r\n"
                  "[effect]\r\n"
                  "; 1 = a blown tire acts on the truck: a truck tire pulls to its side, any blown tire\r\n"
                  "; drags (less throttle, no brake lights); stop it for the session with Ctrl+Shift+B\r\n"
                  "enabled = 1\r\n"
                  "; steering pull of a tire on a steered axle (1 = full lock)\r\n"
                  "pull = 0.12\r\n"
                  "; other truck axles: this fraction of pull\r\n"
                  "pull_rear = 0.4\r\n"
                  "; right after the blowout the pull is kick times stronger for kick_s seconds\r\n"
                  "kick = 2\r\n"
                  "kick_s = 0.4\r\n"
                  "; brake pedal tap at the blowout (the brake lights come on only then)\r\n"
                  "kick_brake = 0.15\r\n"
                  "kick_brake_s = 1\r\n"
                  "; throttle taken away while a truck tire / only a trailer tire is blown (0 ... 1)\r\n"
                  "throttle_cut = 0.2\r\n"
                  "trailer_throttle_cut = 0.1\r\n"
                  "; trailer brake while a trailer tire / a truck tire is blown (with a trailer attached);\r\n"
                  "; 0.35 holds without lighting the brake lamps\r\n"
                  "trailer_hold = 0.35\r\n"
                  "truck_trailer_hold = 0.35\r\n"
                  "; brake pedal all the time while a trailer tire is blown (lights on)\r\n"
                  "trailer_pedal = 0\r\n"
                  "; speed limit while a truck tire / only a trailer tire is blown\r\n"
                  "limit_truck_kmh = 60\r\n"
                  "limit_trailer_kmh = 70\r\n"
                  "; governor: from this many km/h under the limit the throttle is taken away\r\n"
                  "; (all of it at the limit; analog pedal only, not the keyboard)\r\n"
                  "governor_kmh = 5\r\n"
                  "; last resort: from this many km/h over the limit the brake (lights on)\r\n"
                  "; grows to limit_brake over limit_band_kmh\r\n"
                  "limit_brake_after_kmh = 8\r\n"
                  "limit_band_kmh = 10\r\n"
                  "limit_brake = 0.3\r\n"
                  "; steering shake of a blown truck tire (0 = none)\r\n"
                  "wobble = 0.015\r\n"
                  "; 1 = a bang when a tire blows, then the torn tire slapping the road in step with\r\n"
                  "; the wheel; volumes 0 ... 1 (not tied to the game's volume)\r\n"
                  "sound = 1\r\n"
                  "sound_volume = 0.8\r\n"
                  "flap_volume = 0.6\r\n"
                  "; 1 = the blown wheel is drawn shredded (steel belts showing, a gash in the sidewall);\r\n"
                  "; needs [tires] wear_levels = 1 and separate = 1\r\n"
                  "visual = 1\r\n"
                  "; no drag below this speed; the pull reaches full strength at full_speed_kmh\r\n"
                  "min_speed_kmh = 5\r\n"
                  "full_speed_kmh = 40\r\n"
                  "; 1 = pull to the other side (if the game's sign is the opposite)\r\n"
                  "steer_invert = 0\r\n",
                  f);
            fclose(f);
        }
    }
    const wchar_t* p = ini.c_str();
    Config c;
    c.probe = GetPrivateProfileIntW(L"present", L"probe", 0, p) == 1;
    c.hook = c.probe && GetPrivateProfileIntW(L"present", L"hook", 0, p) == 1;
    c.overlay = c.hook && GetPrivateProfileIntW(L"present", L"overlay", 0, p) == 1;
    c.delay_sec = static_cast<int>(GetPrivateProfileIntW(L"present", L"delay_s", 10, p));
    wchar_t buf[32] = {};
    GetPrivateProfileStringW(L"present", L"overlay_scale", L"1.6", buf, 32, p);
    c.overlay_scale = wcstof(buf, nullptr);
    if (!(c.overlay_scale >= 0.5f && c.overlay_scale <= 5.f)) c.overlay_scale = 1.6f;
    c.overlay_x = static_cast<int>(GetPrivateProfileIntW(L"present", L"overlay_x", 20, p));
    c.overlay_y = static_cast<int>(GetPrivateProfileIntW(L"present", L"overlay_y", 20, p));
    c.tires_log = c.hook && GetPrivateProfileIntW(L"tires", L"log", 0, p) == 1;
    c.tires_capture = c.tires_log && GetPrivateProfileIntW(L"tires", L"capture", 0, p) == 1;
    c.capture_delay_sec = static_cast<int>(GetPrivateProfileIntW(L"tires", L"capture_delay_s", 30, p));
    c.capture2_delay_sec = static_cast<int>(GetPrivateProfileIntW(L"tires", L"capture2_delay_s", 0, p));
    c.capture_frames = static_cast<int>(GetPrivateProfileIntW(L"tires", L"capture_frames", 1, p));
    c.wear_levels = c.tires_log && GetPrivateProfileIntW(L"tires", L"wear_levels", 0, p) == 1;
    c.levels = static_cast<int>(GetPrivateProfileIntW(L"tires", L"levels", 8, p));
    GetPrivateProfileStringW(L"tires", L"wear_override", L"-1", buf, 32, p);
    c.wear_override = wcstof(buf, nullptr);
    c.tint_test = c.wear_levels && GetPrivateProfileIntW(L"tires", L"tint_test", 0, p) == 1;
    c.separate = c.wear_levels && GetPrivateProfileIntW(L"tires", L"separate", 0, p) == 1;
    return c;
}

void Start(const std::wstring& tirewear_dir, const Config& c) {
    if (g_thread.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_file = _wfopen((tirewear_dir + L"\\present_probe.log").c_str(), L"ab");
    }
    g_hook_enabled = c.hook;
    g_overlay_enabled = c.overlay;
    if (c.tires_log) {
        tire_probe::SetLog(&Emit);
        std::string err;
        const int n = tire_probe::LoadCatalog(tirewear_dir + L"\\tire_textures.csv", &err);
        if (n < 0) {
            Emit("tires: off (" + err + " TireWear\\tire_textures.csv)");
        } else {
            g_tires_enabled = true;
            SYSTEMTIME st;
            GetLocalTime(&st);
            wchar_t name[64];
            swprintf(name, 64, L"\\tire_capture_%04d%02d%02d_%02d%02d%02d.txt", st.wYear, st.wMonth, st.wDay,
                     st.wHour, st.wMinute, st.wSecond);
            tire_probe::SetCaptureFile(tirewear_dir + name);
            int regions = 0;
            bool wear = c.wear_levels;
            if (wear) {
                regions = tire_probe::LoadRegions(tirewear_dir + L"\\tire_regions.csv", &err);
                if (regions <= 0) {
                    Emit("tires: wear levels off (" + (regions < 0 ? err : std::string("no regions")) +
                         " TireWear\\tire_regions.csv)");
                    wear = false;
                }
            }
            tire_probe::SetWearLevels(wear, c.levels, c.tint_test, c.wear_override);
            tire_probe::SetSeparation(wear && c.separate);
            tire_probe::SetCaptureFrames(c.capture_frames);
            char text[160];
            snprintf(text, sizeof(text),
                     "wear levels %s (%d levels, %d textures with a tread rectangle, override %.2f%s%s)",
                     wear ? "on" : "off", c.levels, regions, c.wear_override, c.tint_test ? ", tint test" : "",
                     wear && c.separate ? ", player separation" : "");
            Emit("tires: catalog loaded, " + std::to_string(n) + " textures; " + text);
        }
    }
    overlay::Settings s;
    s.scale = c.overlay_scale;
    s.x = c.overlay_x;
    s.y = c.overlay_y;
    overlay::Configure(s, &Emit);
    g_thread = std::thread(Run);
}

void Tick() {
    if (!g_installed.load() || g_passthrough.load()) return;
    const uint64_t now = GetTickCount64();
    const uint64_t n = g_game_presents.load(std::memory_order_relaxed);
    if (n != g_wd_last_count) {
        if (g_wd_stalled) Emit("watchdog: game Present reached again");
        g_wd_stalled = false;
        g_wd_last_count = n;
        g_wd_last_change = now;
    } else if (!g_wd_stalled && g_wd_last_change && now - g_wd_last_change > 5000) {
        // The game keeps running frames but our detour is not reached:
        // someone rewrote the jump or the game presents another way.
        g_wd_stalled = true;
        Emit("watchdog: game Present not reached for 5 s while the game runs frames");
    }
}

void Stop() {
    if (g_thread.joinable()) g_thread.join();
    if (g_installed.load()) {
        g_passthrough.store(true);
        tire_probe::SetActive(false);
        Emit("shutdown: detours left in place as pass-through (DLL pinned, nothing unhooked)");
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
}

std::vector<std::string> TakeMessages() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<std::string> out;
    out.swap(g_messages);
    return out;
}

}  // namespace present_probe
