// Test C: a small HUD drawn with Dear ImGui from the Present detour, showing
// the per-unit average wheel wear read from the telemetry SDK.
#pragma once

#include <string>

struct IDXGISwapChain;

namespace overlay {

struct Settings {
    float scale = 1.6f;  // ImGui font scale (default font is 13 px)
    int x = 20;          // top-left corner, in back buffer pixels
    int y = 20;
};

void Configure(const Settings& s, void (*log)(const std::string&));

// Game thread: latest telemetry values (-1 = no value yet).
void SetWear(float truck, const float* trailer_wear, const bool* trailer_connected, int count);
// Game thread: blown tires to show, one per line (empty = none).
void SetAlert(const std::string& text);

// Render thread, game swap chain only.
void OnPresent(IDXGISwapChain* sc);
void OnBeforeResize(IDXGISwapChain* sc);
// Render thread: drop every D3D/ImGui resource we hold (pass-through mode).
void ReleaseAll();
bool HasResources();

}  // namespace overlay
