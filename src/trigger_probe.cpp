#include "trigger_probe.h"

#include <MinHook.h>

#include <chrono>
#include <cstdint>
#include <mutex>

#include "pattern.h"

namespace trigger_probe {
namespace {

// The dispatcher's start: frame setup, "cmp dword [rcx+14h],0", the -1 and
// the "type <= 31" check before the jump table (T3-B1).
const char* kRunPattern =
    "40 55 41 54 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 83 79 14 00 4C 8B E1 0F 84 ?? ?? ?? ?? "
    "48 81 C1 ?? ?? ?? ?? E8 ?? ?? ?? ?? 41 8B 44 24 14 FF C8 83 F8 1E";
constexpr size_t kTypeOffset = 0x14;
constexpr size_t kMaxQueued = 256;

// The return value and any further arguments are unknown: pass four integer
// registers through and hand back rax untouched.
using RunFn = uintptr_t(__fastcall*)(uint8_t* action, uintptr_t a2, uintptr_t a3, uintptr_t a4);
RunFn g_original = nullptr;
void* g_target = nullptr;

std::mutex g_mutex;
std::vector<Action> g_queue;

double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

uintptr_t __fastcall RunDetour(uint8_t* action, uintptr_t a2, uintptr_t a3, uintptr_t a4) {
    const int type = action ? *reinterpret_cast<const int32_t*>(action + kTypeOffset) : -1;
    const uintptr_t result = g_original(action, a2, a3, a4);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_queue.size() < kMaxQueued) g_queue.push_back({type, Now()});
    return result;
}

}  // namespace

std::string Start() {
    if (g_target) return "already on";
    const pattern::Range text = pattern::GameText();
    const auto run = pattern::Find(text, kRunPattern);
    if (run.size() != 1) return "off: dispatcher pattern found " + pattern::Count(run.size()) + " time(s) (need 1)";
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) return std::string("off: MH_Initialize ") + MH_StatusToString(st);
    void* target = const_cast<uint8_t*>(run[0]);
    st = MH_CreateHook(target, reinterpret_cast<void*>(&RunDetour), reinterpret_cast<void**>(&g_original));
    if (st == MH_OK) st = MH_EnableHook(target);
    if (st != MH_OK) {
        MH_RemoveHook(target);
        return std::string("off: hook failed: ") + MH_StatusToString(st);
    }
    g_target = target;
    return "on, dispatcher at exe+" + pattern::Hex(run[0] - text.base);
}

void Stop() {
    if (!g_target) return;
    MH_DisableHook(g_target);
    MH_RemoveHook(g_target);
    g_target = nullptr;
}

std::vector<Action> Drain() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<Action> out;
    out.swap(g_queue);
    return out;
}

}  // namespace trigger_probe
