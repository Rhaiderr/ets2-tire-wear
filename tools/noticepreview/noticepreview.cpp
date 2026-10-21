// T4 preview: draws the blown tire notice with the plugin's own code
// (src/notice.cpp + ImGui DX11) on an offscreen D3D11 target and saves PNGs,
// so the look can be approved before going into the game.
//
// noticepreview <font.ttf> <out_dir> [background.png]
// Writes notice_<lang>_<w>x<h>.png (top part of the screen) for pt/en at
// 3440x1440 and 1920x1080, and notice_fade.png (fading in).

#include <windows.h>
#include <d3d11.h>
#include <wincodec.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "../../src/notice.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace {

ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_dc = nullptr;
IWICImagingFactory* g_wic = nullptr;

bool Fail(const char* what, HRESULT hr) {
    fprintf(stderr, "%s failed: 0x%08lX\n", what, static_cast<unsigned long>(hr));
    return false;
}

// Background scaled to w x h (BGRA), or a grass-like gradient without one.
std::vector<unsigned char> Background(const std::wstring& path, UINT w, UINT h) {
    std::vector<unsigned char> px(static_cast<size_t>(w) * h * 4);
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapScaler* sc = nullptr;
    IWICFormatConverter* cv = nullptr;
    bool ok = !path.empty() &&
              SUCCEEDED(g_wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                         WICDecodeMetadataCacheOnLoad, &dec)) &&
              SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(g_wic->CreateBitmapScaler(&sc)) &&
              SUCCEEDED(sc->Initialize(frame, w, h, WICBitmapInterpolationModeFant)) &&
              SUCCEEDED(g_wic->CreateFormatConverter(&cv)) &&
              SUCCEEDED(cv->Initialize(sc, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                       WICBitmapPaletteTypeCustom)) &&
              SUCCEEDED(cv->CopyPixels(nullptr, w * 4, static_cast<UINT>(px.size()), px.data()));
    if (cv) cv->Release();
    if (sc) sc->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    if (!ok) {
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x) {
                unsigned char* p = &px[(static_cast<size_t>(y) * w + x) * 4];
                const float t = static_cast<float>(y) / h;
                p[0] = static_cast<unsigned char>(70 + 40 * t);
                p[1] = static_cast<unsigned char>(130 - 30 * t + (x % 7));
                p[2] = static_cast<unsigned char>(100 - 20 * t);
                p[3] = 255;
            }
    }
    for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    return px;
}

bool SavePng(const std::wstring& path, const unsigned char* bgra, UINT w, UINT h, UINT pitch) {
    IWICStream* st = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fr = nullptr;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    bool ok = SUCCEEDED(g_wic->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(g_wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) &&
              SUCCEEDED(fr->Initialize(nullptr)) && SUCCEEDED(fr->SetSize(w, h)) &&
              SUCCEEDED(fr->SetPixelFormat(&fmt)) &&
              SUCCEEDED(fr->WritePixels(h, pitch, pitch * h, const_cast<BYTE*>(bgra))) && SUCCEEDED(fr->Commit()) &&
              SUCCEEDED(enc->Commit());
    if (fr) fr->Release();
    if (enc) enc->Release();
    if (st) st->Release();
    return ok;
}

// One frame of w x h with the notice at `age` seconds; saves the top `crop_h` rows.
bool Render(ImFont* font, UINT w, UINT h, const std::wstring& bg, double age, UINT crop_h, const std::wstring& out) {
    std::vector<unsigned char> px = Background(bg, w, h);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;  // as the game's back buffer [verified: overlay log]
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    D3D11_SUBRESOURCE_DATA init = {px.data(), w * 4, 0};
    ID3D11Texture2D* tex = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    HRESULT hr = g_dev->CreateTexture2D(&td, &init, &tex);
    if (FAILED(hr)) return Fail("CreateTexture2D", hr);
    hr = g_dev->CreateRenderTargetView(tex, nullptr, &rtv);
    if (FAILED(hr)) return Fail("CreateRenderTargetView", hr);

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(w), static_cast<float>(h));
    io.DeltaTime = 1.f / 60.f;
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    notice::Draw(ImGui::GetForegroundDrawList(), io.DisplaySize, font, notice::Now() + age);
    ImGui::Render();
    g_dc->OMSetRenderTargets(1, &rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    td.BindFlags = 0;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    hr = g_dev->CreateTexture2D(&td, nullptr, &staging);
    if (FAILED(hr)) return Fail("CreateTexture2D staging", hr);
    g_dc->CopyResource(staging, tex);
    D3D11_MAPPED_SUBRESOURCE m = {};
    hr = g_dc->Map(staging, 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr)) return Fail("Map", hr);
    std::vector<unsigned char> rows(static_cast<size_t>(w) * 4 * crop_h);
    for (UINT y = 0; y < crop_h; ++y) {
        memcpy(&rows[static_cast<size_t>(y) * w * 4], static_cast<unsigned char*>(m.pData) + y * m.RowPitch, w * 4);
        for (UINT x = 0; x < w; ++x) rows[(static_cast<size_t>(y) * w + x) * 4 + 3] = 255;
    }
    g_dc->Unmap(staging, 0);
    staging->Release();
    rtv->Release();
    tex->Release();
    const bool ok = SavePng(out, rows.data(), w, crop_h, w * 4);
    printf("%ls: %s\n", out.c_str(), ok ? "ok" : "FAILED");
    return ok;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: noticepreview <font.ttf> <out_dir> [background.png]\n");
        return 2;
    }
    const std::wstring dir = argv[2];
    const std::wstring bg = argc > 3 ? argv[3] : L"";
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<char> ttf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (ttf.empty()) {
        fprintf(stderr, "cannot read the font\n");
        return 1;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic));
    if (FAILED(hr)) return Fail("WIC", hr) ? 0 : 1;
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &g_dev, nullptr,
                           &g_dc);
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &g_dev, nullptr,
                               &g_dc);
    if (FAILED(hr)) return Fail("D3D11CreateDevice", hr) ? 0 : 1;

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    ImFont* font = io.Fonts->AddFontFromMemoryTTF(ttf.data(), static_cast<int>(ttf.size()), 24.f, &cfg);
    if (!font || !ImGui_ImplDX11_Init(g_dev, g_dc)) {
        fprintf(stderr, "ImGui setup failed\n");
        return 1;
    }

    const char* pt_title = "Pneu estourado!";
    const char* pt_body = "Caminh\xC3\xA3o: eixo 1, lado esquerdo.\nTroque o pneu na oficina.";
    const char* en_title = "Tire blown!";
    const char* en_body = "Trailer: axle 2, right side.\nReplace the tire at a service station.";

    bool ok = true;
    notice::Show(pt_title, pt_body);
    ok &= Render(font, 3440, 1440, bg, 1.0, 300, dir + L"\\notice_pt_3440x1440.png");
    ok &= Render(font, 1920, 1080, bg, 1.0, 220, dir + L"\\notice_pt_1920x1080.png");
    ok &= Render(font, 3440, 1440, bg, 0.12, 300, dir + L"\\notice_fade.png");
    notice::Show(en_title, en_body);
    ok &= Render(font, 3440, 1440, bg, 1.0, 300, dir + L"\\notice_en_3440x1440.png");

    ImGui_ImplDX11_Shutdown();
    ImGui::DestroyContext();
    g_dc->Release();
    g_dev->Release();
    g_wic->Release();
    return ok ? 0 : 1;
}
