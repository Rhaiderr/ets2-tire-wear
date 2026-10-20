// Steps R2/R2b: logging-only D3D11 hooks that recognise tire textures by
// hashing their pixel data (compared with the catalog built by
// tools\tirecat), track which shader resource views point at them, and
// count / capture the draws that use them. Nothing the game draws changes.
//
// R2 showed the game creates its textures empty and uploads the data later,
// so R2b also watches the upload paths: UpdateSubresource(1), Map/Unmap of
// staging textures and CopyResource / CopySubresourceRegion.
#pragma once

#include <string>

struct IDXGISwapChain;

namespace tire_probe {

// Context functions are hooked for the immediate context ([0]) and, when the
// deferred context class uses different code, for deferred contexts ([1]).
enum CtxFn {
    kPSSetShaderResources,  // slot 8
    kDrawIndexed,           // slot 12
    kDrawIndexedInstanced,  // slot 20
    kMap,                   // slot 14
    kUnmap,                 // slot 15
    kCopySubresourceRegion, // slot 46
    kCopyResource,          // slot 47
    kUpdateSubresource,     // slot 48
    kUpdateSubresource1,    // ID3D11DeviceContext1 slot 116
    kCtxFnCount
};

struct Targets {
    void* create_texture2d = nullptr;  // ID3D11Device slot 5
    void* create_srv = nullptr;        // ID3D11Device slot 7
    void* ctx[2][kCtxFnCount] = {};    // [immediate, deferred]
};

const char* CtxFnName(int fn);
int CtxFnSlot(int fn);

// Hook descriptor handed to the installer (MinHook lives in present_probe).
struct Hook {
    const char* name;
    void* target;
    void* detour;
    void** original;
};
constexpr int kMaxHooks = 2 + 2 * kCtxFnCount;

// Picks the built-in catalog (catalog/ets2 or catalog/ats). Call before LoadCatalog.
enum class Game { kEts2, kAts };
void SetGame(Game game);

// Loads TireWear\tire_textures.csv; returns the number of textures or -1.
int LoadCatalog(const std::wstring& csv_path, std::string* error);

void SetLog(void (*log)(const std::string&));
void SetCaptureFile(const std::wstring& path);

// Fills hooks for the targets that are present (deferred ones only when they
// differ from the immediate ones); returns the count.
int MakeHooks(const Targets& t, Hook* out);

void SetActive(bool active);  // false = every detour only forwards

// R5: tread rectangles (catalog\tire_regions.csv); returns textures matched or -1.
int LoadRegions(const std::wstring& csv_path, std::string* error);

// R5: draw tires with worn copies of their textures, `levels` levels (level 0
// = the game's texture). tint paints worn colour clones magenta (test);
// wear_override >= 0 forces that wear (0..1) on every tire (test).
// Call before the hooks go live.
void SetWearLevels(bool enabled, int levels, bool tint, float wear_override);

// Game thread: average wheel wear (0..1) per unit, index as in player_tracker
// (0 = truck, 1 + i = trailer i).
void SetUnitWear(const float* wear, int count);

// R4b: give each tire draw the wear of the unit it belongs to (player_tracker)
// and leave traffic alone. Without it, or when it fails, the texture folder
// picks the unit (/trailer_owned/ = trailer 0, else truck). Call before the
// hooks go live.
void SetSeparation(bool enabled);

// R4b-diag: each capture covers this many consecutive frames (1 = as before);
// with more than one, only cb0 rows 0-2 are copied and every real tire draw
// also logs what the separation read and decided. Call before the hooks go live.
void SetCaptureFrames(int frames);

// Game thread: ask for a one-frame capture of tire draws; `telemetry` (where
// the player's units are) is written at the top of the file. False while a
// capture is still pending.
bool RequestCapture(const std::string& telemetry);
// Render thread, once per game Present (game swap chain): frame boundary for
// stats and the capture, which is read back through this swap chain's device.
void OnPresent(IDXGISwapChain* swap_chain);
// Summary lines for the periodic stats.
std::string Stats();

}  // namespace tire_probe
