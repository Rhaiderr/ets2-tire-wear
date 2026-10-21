// Present hook plan, tests A-C: locate the system DXGI Present/Present1/
// ResizeBuffers functions, log who already jumps out of them, detour them
// (hook = 1) and draw the wear HUD from the detour (overlay = 1).
#pragma once

#include <string>
#include <vector>

namespace present_probe {

struct Config {
    bool probe = false;    // test A: locate and log
    bool hook = false;     // test B: counting detours (needs probe)
    bool overlay = false;  // test C: test HUD, wear per unit (needs hook)
    bool notice = false;   // T4: blown tire notice at the top of the screen (needs hook)
    float notice_s = 8.f;
    float notice_top = 60.f;  // UI units (1/900 of the screen height)
    float notice_scale = 1.f;
    int delay_sec = 10;
    float overlay_scale = 1.6f;
    int overlay_x = 20;
    int overlay_y = 20;
    bool tires_log = false;      // R2: hook at plugin load, log tire textures/draws (needs hook)
    bool tires_capture = false;  // R2: one-frame capture of tire draws
    int capture_delay_sec = 30;
    int capture2_delay_sec = 0;  // R4a: second capture (0 = none), e.g. while driving
    int capture_frames = 1;      // R4b-diag: consecutive frames per capture
    bool wear_levels = false;    // R5: draw tires with the worn copy of the current level (needs tires_log)
    int levels = 8;              // R5: number of wear levels, level 0 = the game's texture
    float wear_override = -1.f;  // R5: >= 0 forces this wear (0..1) on every tire, for testing
    bool tint_test = false;      // R3 test: worn colour copies painted magenta
    bool separate = false;       // R4b: only the player's tires, each with its unit's wear (needs wear_levels)
};

// Reads the settings file `ini` (creating it with the worn tires on if missing).
Config LoadConfig(const std::wstring& ini);

// Starts the probe on a background thread (one shot).
void Start(const std::wstring& tirewear_dir, const Config& config);

// Watchdog; call once per game frame from the game thread.
void Tick();

// Waits for the probe thread and turns the detours into pass-through;
// call from scs_telemetry_shutdown.
void Stop();

// Messages produced by the probe, to be forwarded to the game log from the
// game thread.
std::vector<std::string> TakeMessages();

}  // namespace present_probe
