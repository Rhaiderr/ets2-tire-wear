// Drawing with Dear ImGui from the Present detour: the test HUD (test C: the
// per-unit average wheel wear and the blown tires, off by default) and the
// blown tire notice (T4, see notice.h).
#pragma once

#include <string>

struct IDXGISwapChain;

namespace overlay {

struct Settings {
    float scale = 1.6f;  // ImGui font scale (default font is 13 px)
    int x = 20;          // top-left corner, in back buffer pixels
    int y = 20;
    bool hud = false;     // test HUD in the corner
    bool notice = false;  // blown tire notice
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
