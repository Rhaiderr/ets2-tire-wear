// Test C: HUD drawn from the Present detour with Dear ImGui (DX11 backend
// only: no input backend, the HUD never takes mouse or keyboard).
//
// Everything here runs on the thread that presents. Any D3D failure or SEH
// exception turns the HUD off for the rest of the session; the detour keeps
// passing frames through untouched.

#include "overlay.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <atomic>
#include <cstdio>
#include <mutex>

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "notice.h"

namespace overlay {
namespace {

constexpr int kMaxTrailers = 10;
constexpr int kFontResource = 101;  // RCDATA in resources.rc: Rubik Regular (OFL)

Settings g_settings;
void (*g_log)(const std::string&) = nullptr;

std::atomic<float> g_truck{-1.f};
std::atomic<float> g_trailer[kMaxTrailers];
std::atomic<bool> g_connected[kMaxTrailers];

std::mutex g_mutex;  // Present and ResizeBuffers may come from different threads
std::mutex g_alert_mutex;
std::string g_alert;  // stage 3: blown tires, one per line
bool g_failed = false;
IDXGISwapChain* g_sc = nullptr;  // not owned; only compared
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_dc = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
UINT g_width = 0, g_height = 0;
ImGuiContext* g_imgui = nullptr;
bool g_imgui_dx11 = false;
ImFont* g_notice_font = nullptr;  // owned by the ImGui atlas
LARGE_INTEGER g_qpf = {}, g_last = {};

void Log(const std::string& s) {
    if (g_log) g_log("overlay: " + s);
}

template <typename T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

void ReleaseLocked() {
    SafeRelease(g_rtv);
    if (g_imgui) {
        ImGui::SetCurrentContext(g_imgui);
        if (g_imgui_dx11) ImGui_ImplDX11_Shutdown();
        ImGui::DestroyContext(g_imgui);
        g_imgui = nullptr;
        g_imgui_dx11 = false;
        g_notice_font = nullptr;
    }
    SafeRelease(g_dc);
    SafeRelease(g_device);
    g_sc = nullptr;
}

std::string Hr(HRESULT hr) {
    char b[16];
    snprintf(b, sizeof(b), "0x%08lX", static_cast<unsigned long>(hr));
    return b;
}

// The notice font, built into this DLL (read-only, lives as long as the DLL).
ImFont* AddNoticeFont(ImGuiIO& io) {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&AddNoticeFont), &mod))
        return nullptr;
    HRSRC res = FindResourceW(mod, MAKEINTRESOURCEW(kFontResource), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    HGLOBAL data = res ? LoadResource(mod, res) : nullptr;
    void* ttf = data ? LockResource(data) : nullptr;
    if (!ttf) return nullptr;
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    return io.Fonts->AddFontFromMemoryTTF(ttf, static_cast<int>(SizeofResource(mod, res)), 24.f, &cfg);
}

// Returns nullptr on success, or a reason to turn the HUD off.
const char* Init(IDXGISwapChain* sc, std::string* detail) {
    HRESULT hr = sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_device));
    if (FAILED(hr)) {
        *detail = Hr(hr);
        return "GetDevice";
    }
    g_device->GetImmediateContext(&g_dc);
    g_imgui = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_imgui);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange | ImGuiConfigFlags_NoKeyboard;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().FontScaleMain = g_settings.scale;
    ImGui::GetStyle().WindowRounding = 4.f;
    // The default font first: it stays the HUD's font.
    io.Fonts->AddFontDefault();
    if (g_settings.notice) {
        g_notice_font = AddNoticeFont(io);
        if (!g_notice_font) Log("notice font not found in the DLL; using the default font");
    }
    if (!ImGui_ImplDX11_Init(g_device, g_dc)) return "ImGui_ImplDX11_Init";
    g_imgui_dx11 = true;
    QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_last);
    g_sc = sc;
    Log("ImGui " IMGUI_VERSION " ready on the game device");
    return nullptr;
}

const char* CreateTarget(IDXGISwapChain* sc, std::string* detail) {
    ID3D11Texture2D* bb = nullptr;
    HRESULT hr = sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb));
    if (FAILED(hr)) {
        *detail = Hr(hr);
        return "GetBuffer";
    }
    D3D11_TEXTURE2D_DESC td = {};
    bb->GetDesc(&td);
    // View in the buffer's own format (B8G8R8A8_UNORM here): ImGui colours are
    // already display (sRGB) values, so no sRGB conversion on write.
    hr = g_device->CreateRenderTargetView(bb, nullptr, &g_rtv);
    bb->Release();
    if (FAILED(hr)) {
        *detail = Hr(hr);
        return "CreateRenderTargetView";
    }
    g_width = td.Width;
    g_height = td.Height;
    Log("render target " + std::to_string(g_width) + "x" + std::to_string(g_height) + ", format " +
        std::to_string(td.Format));
    return nullptr;
}

void WearLine(const char* label, float v) {
    if (v < 0.f) ImGui::Text("%s: --", label);
    else ImGui::Text("%s: %.2f %%", label, v * 100.f);
}

void DrawHud() {
    ImGui::SetNextWindowPos(ImVec2(static_cast<float>(g_settings.x), static_cast<float>(g_settings.y)));
    ImGui::SetNextWindowBgAlpha(0.55f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    ImGui::Begin("##ets2_tire_wear", nullptr, flags);
    ImGui::TextUnformatted("Desgaste dos pneus (media)");
    ImGui::Separator();
    WearLine("Caminhao", g_truck.load(std::memory_order_relaxed));
    char label[32];
    for (int i = 0; i < kMaxTrailers; ++i) {
        if (!g_connected[i].load(std::memory_order_relaxed)) continue;
        snprintf(label, sizeof(label), "Carreta %d", i);
        WearLine(label, g_trailer[i].load(std::memory_order_relaxed));
    }
    {
        std::lock_guard<std::mutex> lock(g_alert_mutex);
        if (!g_alert.empty()) {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.f, 0.35f, 0.25f, 1.f), "%s", g_alert.c_str());
        }
    }
    ImGui::End();
}

const char* Draw(IDXGISwapChain* sc, std::string* detail) {
    if (sc != g_sc) {
        // First frame, or the game made a new swap chain: start clean.
        ReleaseLocked();
        if (const char* e = Init(sc, detail)) return e;
    }
    if (!g_rtv)
        if (const char* e = CreateTarget(sc, detail)) return e;

    // Without the HUD, frames with no notice cost nothing more.
    const double now_s = notice::Now();
    const bool show_notice = g_settings.notice && notice::Active(now_s);
    if (!g_settings.hud && !show_notice) return nullptr;

    ImGui::SetCurrentContext(g_imgui);
    ImGuiIO& io = ImGui::GetIO();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    io.DeltaTime = static_cast<float>(now.QuadPart - g_last.QuadPart) / static_cast<float>(g_qpf.QuadPart);
    if (io.DeltaTime <= 0.f) io.DeltaTime = 1.f / 1000.f;
    g_last = now;
    io.DisplaySize = ImVec2(static_cast<float>(g_width), static_cast<float>(g_height));

    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    if (show_notice)
        notice::Draw(ImGui::GetForegroundDrawList(), io.DisplaySize, g_notice_font ? g_notice_font : ImGui::GetFont(),
                     now_s);
    if (g_settings.hud) DrawHud();
    ImGui::Render();

    // The DX11 backend restores most pipeline state but not the render
    // targets, so keep the game's bindings around our draw.
    ID3D11RenderTargetView* old_rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* old_dsv = nullptr;
    g_dc->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, old_rtv, &old_dsv);
    g_dc->OMSetRenderTargets(1, &g_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_dc->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, old_rtv, old_dsv);
    for (auto*& r : old_rtv) SafeRelease(r);
    SafeRelease(old_dsv);
    return nullptr;
}

// No C++ objects with destructors in this frame, so __try is allowed.
const char* GuardedDraw(IDXGISwapChain* sc, std::string* detail, DWORD* seh_code) {
    __try {
        return Draw(sc, detail);
    } __except (*seh_code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return "exception";
    }
}

}  // namespace

void Configure(const Settings& s, void (*log)(const std::string&)) {
    g_settings = s;
    g_log = log;
}

void SetWear(float truck, const float* trailer_wear, const bool* trailer_connected, int count) {
    g_truck.store(truck, std::memory_order_relaxed);
    for (int i = 0; i < count && i < kMaxTrailers; ++i) {
        g_trailer[i].store(trailer_wear[i], std::memory_order_relaxed);
        g_connected[i].store(trailer_connected[i], std::memory_order_relaxed);
    }
}

void SetAlert(const std::string& text) {
    std::lock_guard<std::mutex> lock(g_alert_mutex);
    g_alert = text;
}

void OnPresent(IDXGISwapChain* sc) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_failed) return;
    std::string detail;
    DWORD seh_code = 0;
    if (const char* err = GuardedDraw(sc, &detail, &seh_code)) {
        g_failed = true;
        char code[16];
        snprintf(code, sizeof(code), "0x%08lX", seh_code);
        Log(std::string("off: ") + err + (detail.empty() ? "" : " " + detail) +
            (seh_code ? std::string(" code ") + code : "") + "; frames pass through untouched");
        // After an exception the state is unknown: leak rather than touch it.
        if (!seh_code) ReleaseLocked();
    }
}

void OnBeforeResize(IDXGISwapChain* sc) {
    std::lock_guard<std::mutex> lock(g_mutex);
    // The swap chain cannot resize while we hold a view of its buffer.
    if (sc == g_sc && g_rtv) {
        SafeRelease(g_rtv);
        Log("render target released for ResizeBuffers");
    }
}

void ReleaseAll() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_failed) ReleaseLocked();
}

bool HasResources() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_device != nullptr && !g_failed;
}

}  // namespace overlay
