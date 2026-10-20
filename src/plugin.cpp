// ETS2 tire wear - step 1: read-only telemetry logger.
//
// Logs truck/trailer wheel wear (per-unit average, as exposed by the SCS
// Telemetry SDK) plus speed and odometer to a CSV, so we can check scale,
// whether the value changes while driving, and how many trailers appear.
// Optionally (ets2_tire_wear.ini, [present] probe / hook / overlay) runs tests A-C of the
// Present hook plan: locate the system DXGI Present, detour it and draw a wear HUD.

#include <windows.h>
#include <shlobj.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include "scssdk_telemetry.h"
#include "amtrucks/scssdk_ats.h"
#include "eurotrucks2/scssdk_eut2.h"
#include "eurotrucks2/scssdk_telemetry_eut2.h"
#include "common/scssdk_telemetry_common_configs.h"
#include "common/scssdk_telemetry_truck_common_channels.h"
#include "common/scssdk_telemetry_trailer_common_channels.h"

#include "overlay.h"
#include "player_tracker.h"
#include "present_probe.h"
#include "tire_probe.h"

namespace {

constexpr int kMaxTrailers = SCS_TELEMETRY_trailers_count;
constexpr double kLogIntervalSec = 1.0;

struct State {
    scs_log_t log = nullptr;
    bool ats = false;  // American Truck Simulator (game_id "ats"); otherwise ETS2
    FILE* csv = nullptr;
    bool paused = true;
    float speed_ms = 0.f;
    float odometer_km = 0.f;
    float truck_wheels_wear = -1.f;                          // -1 = no value yet
    std::array<float, kMaxTrailers> trailer_wheels_wear;     // -1 = no value yet
    std::array<bool, kMaxTrailers> trailer_connected{};
    std::chrono::steady_clock::time_point last_row{};
    std::chrono::steady_clock::time_point t0{};
    // Present probe (tests A-C): armed from the INI, fired once after the delay.
    present_probe::Config probe_cfg;
    bool probe_started = false;
    bool probe_seen_unpaused = false;
    std::chrono::steady_clock::time_point probe_first_unpaused{};
    int captures_requested = 0;
    // R4a: where the player's units are, for the tire capture.
    scs_value_dplacement_t truck_placement{};
    std::array<scs_value_dplacement_t, kMaxTrailers> trailer_placement{};
    std::map<std::string, std::string> configs;  // config id ("truck", "trailer.0" ...) -> attributes as text
    tracker::Unit unit_wheels[tracker::kMaxUnits];  // R4b: wheel layout per unit (0 = truck, 1 + i = trailer i)
};

State g;

void Log(scs_log_type_t type, const std::string& msg) {
    if (g.log) g.log(type, ("[ets2_tire_wear] " + msg).c_str());
}

std::wstring DocumentsDir() {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    return out;
}

// Each game keeps its own settings and logs, next to its own profiles.
std::wstring TireWearDir() {
    std::wstring dir = DocumentsDir() + (g.ats ? L"\\American Truck Simulator" : L"\\Euro Truck Simulator 2") +
                       L"\\TireWear";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

void FlushProbeMessages() {
    for (const auto& m : present_probe::TakeMessages()) Log(SCS_LOG_TYPE_message, m);
}

bool OpenCsv() {
    std::wstring path = TireWearDir() + L"\\tire_wear_log.csv";
    // Append so successive sessions accumulate; header only for a new/empty file.
    g.csv = _wfopen(path.c_str(), L"ab");
    if (!g.csv) return false;
    fseek(g.csv, 0, SEEK_END);
    if (ftell(g.csv) == 0) {
        fprintf(g.csv, "t_sec,paused,speed_kmh,odometer_km,truck_wheels_wear");
        for (int i = 0; i < kMaxTrailers; ++i)
            fprintf(g.csv, ",trailer%d_connected,trailer%d_wheels_wear", i, i);
        fprintf(g.csv, "\n");
    }
    return true;
}

void WriteRow() {
    if (!g.csv) return;
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g.t0).count();
    fprintf(g.csv, "%.1f,%d,%.2f,%.3f,%.6f", t, g.paused ? 1 : 0, g.speed_ms * 3.6f,
            g.odometer_km, g.truck_wheels_wear);
    for (int i = 0; i < kMaxTrailers; ++i)
        fprintf(g.csv, ",%d,%.6f", g.trailer_connected[i] ? 1 : 0, g.trailer_wheels_wear[i]);
    fprintf(g.csv, "\n");
    fflush(g.csv);
}

// ---- channel callbacks ----

SCSAPI_VOID OnFloat(const scs_string_t, const scs_u32_t, const scs_value_t* value, const scs_context_t ctx) {
    if (value && value->type == SCS_VALUE_TYPE_float) *static_cast<float*>(ctx) = value->value_float.value;
}

SCSAPI_VOID OnBool(const scs_string_t, const scs_u32_t, const scs_value_t* value, const scs_context_t ctx) {
    if (value && value->type == SCS_VALUE_TYPE_bool) *static_cast<bool*>(ctx) = value->value_bool.value != 0;
}

SCSAPI_VOID OnDPlacement(const scs_string_t, const scs_u32_t, const scs_value_t* value, const scs_context_t ctx) {
    if (value && value->type == SCS_VALUE_TYPE_dplacement)
        *static_cast<scs_value_dplacement_t*>(ctx) = value->value_dplacement;
}

// ---- R4a: telemetry snapshot for the tire capture ----

std::string ValueText(const scs_value_t& v) {
    char b[160];
    switch (v.type) {
        case SCS_VALUE_TYPE_bool: return v.value_bool.value ? "true" : "false";
        case SCS_VALUE_TYPE_s32: return std::to_string(v.value_s32.value);
        case SCS_VALUE_TYPE_u32: return std::to_string(v.value_u32.value);
        case SCS_VALUE_TYPE_s64: return std::to_string(v.value_s64.value);
        case SCS_VALUE_TYPE_u64: return std::to_string(v.value_u64.value);
        case SCS_VALUE_TYPE_float: snprintf(b, sizeof(b), "%g", v.value_float.value); return b;
        case SCS_VALUE_TYPE_double: snprintf(b, sizeof(b), "%.6f", v.value_double.value); return b;
        case SCS_VALUE_TYPE_fvector:
            snprintf(b, sizeof(b), "(%g %g %g)", v.value_fvector.x, v.value_fvector.y, v.value_fvector.z);
            return b;
        case SCS_VALUE_TYPE_dvector:
            snprintf(b, sizeof(b), "(%.4f %.4f %.4f)", v.value_dvector.x, v.value_dvector.y, v.value_dvector.z);
            return b;
        case SCS_VALUE_TYPE_euler:
            snprintf(b, sizeof(b), "(h %g p %g r %g)", v.value_euler.heading, v.value_euler.pitch, v.value_euler.roll);
            return b;
        case SCS_VALUE_TYPE_fplacement: {
            const auto& p = v.value_fplacement;
            snprintf(b, sizeof(b), "pos (%g %g %g) h %g p %g r %g", p.position.x, p.position.y, p.position.z,
                     p.orientation.heading, p.orientation.pitch, p.orientation.roll);
            return b;
        }
        case SCS_VALUE_TYPE_dplacement: {
            const auto& p = v.value_dplacement;
            snprintf(b, sizeof(b), "pos (%.4f %.4f %.4f) h %g p %g r %g", p.position.x, p.position.y, p.position.z,
                     p.orientation.heading, p.orientation.pitch, p.orientation.roll);
            return b;
        }
        case SCS_VALUE_TYPE_string: return v.value_string.value ? v.value_string.value : "";
        default: return "?";
    }
}

// Keeps the truck and trailer configurations (wheel positions, hook...).
SCSAPI_VOID OnConfig(const scs_event_t, const void* info, const scs_context_t) {
    const auto* cfg = static_cast<const scs_telemetry_configuration_t*>(info);
    if (!cfg || !cfg->id) return;
    const std::string id = cfg->id;
    if (id != SCS_TELEMETRY_CONFIG_truck && id.rfind(SCS_TELEMETRY_CONFIG_trailer, 0) != 0) return;
    std::string text;
    // R4b: wheel positions of the unit, for player_tracker. "truck" -> unit 0,
    // "trailer.<i>" -> unit 1 + i (the plain "trailer" id repeats trailer 0).
    int unit = -1;
    if (id == SCS_TELEMETRY_CONFIG_truck) unit = 0;
    else if (id.size() > 8 && id.compare(0, 8, "trailer.") == 0) unit = 1 + atoi(id.c_str() + 8);
    tracker::Unit wheels;
    for (const scs_named_value_t* a = cfg->attributes; a && a->name; ++a) {
        text += std::string("  ") + a->name;
        if (a->index != SCS_U32_NIL) text += "[" + std::to_string(a->index) + "]";
        text += " = " + ValueText(a->value) + "\n";
        if (!strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_wheel_position) && a->index < tracker::kMaxWheels &&
            a->value.type == SCS_VALUE_TYPE_fvector) {
            wheels.wheel[a->index][0] = a->value.value_fvector.x;
            wheels.wheel[a->index][1] = a->value.value_fvector.y;
            wheels.wheel[a->index][2] = a->value.value_fvector.z;
            if (static_cast<int>(a->index) + 1 > wheels.wheel_count) wheels.wheel_count = a->index + 1;
        }
    }
    g.configs[id] = text;
    if (unit >= 0 && unit < tracker::kMaxUnits) g.unit_wheels[unit] = wheels;  // empty config = unit gone
}

std::string PlacementText(const scs_value_dplacement_t& p) {
    scs_value_t v{};
    v.type = SCS_VALUE_TYPE_dplacement;
    v.value_dplacement = p;
    return ValueText(v);
}

std::string TelemetrySnapshot() {
    std::string s = "# telemetry at the capture request\n";
    char b[128];
    snprintf(b, sizeof(b), "speed_kmh = %.2f\npaused = %d\n", g.speed_ms * 3.6f, g.paused ? 1 : 0);
    s += b;
    s += "truck.world.placement = " + PlacementText(g.truck_placement) + "\n";
    for (int i = 0; i < kMaxTrailers; ++i) {
        if (!g.trailer_connected[i]) continue;
        s += "trailer." + std::to_string(i) + ".world.placement = " + PlacementText(g.trailer_placement[i]) + "\n";
    }
    for (const auto& [id, text] : g.configs) s += "config " + id + ":\n" + text;
    return s;
}

// ---- event callbacks ----

SCSAPI_VOID OnFrameEnd(const scs_event_t, const void*, const scs_context_t) {
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(now - g.last_row).count() >= kLogIntervalSec) {
        g.last_row = now;
        WriteRow();
    }
    if (!g.paused && !g.probe_seen_unpaused) {
        g.probe_seen_unpaused = true;
        g.probe_first_unpaused = now;
    }
    const double unpaused_for =
        g.probe_seen_unpaused ? std::chrono::duration<double>(now - g.probe_first_unpaused).count() : -1.0;
    if (g.probe_cfg.probe && !g.probe_started && unpaused_for >= g.probe_cfg.delay_sec) {
        g.probe_started = true;
        present_probe::Start(TireWearDir(), g.probe_cfg);
    }
    // Up to two captures (R4a: one parked, one driving), each at its delay.
    if (g.probe_cfg.tires_capture && g.captures_requested < 2) {
        const int delay = g.captures_requested == 0 ? g.probe_cfg.capture_delay_sec : g.probe_cfg.capture2_delay_sec;
        if (delay > 0 && unpaused_for >= delay && tire_probe::RequestCapture(TelemetrySnapshot())) {
            ++g.captures_requested;
            Log(SCS_LOG_TYPE_message, "tire capture " + std::to_string(g.captures_requested) +
                                          " requested for the next frame");
        }
    }
    overlay::SetWear(g.truck_wheels_wear, g.trailer_wheels_wear.data(), g.trailer_connected.data(), kMaxTrailers);
    // Per unit (0 = truck, 1 + i = trailer i): wear and, for the tracker, the
    // wheel layout of the units actually attached (a dropped trailer is not
    // the player's any more, so its tires go back to the game's).
    float wear[tracker::kMaxUnits] = {g.truck_wheels_wear};
    tracker::Unit units[tracker::kMaxUnits];
    // R4c: and where each unit is in the world, for predicting one unit's
    // wheels from another's pose.
    const auto place = [](tracker::Unit& u, const scs_value_dplacement_t& p) {
        u.placed = true;
        u.pos[0] = p.position.x, u.pos[1] = p.position.y, u.pos[2] = p.position.z;
        u.heading = p.orientation.heading, u.pitch = p.orientation.pitch, u.roll = p.orientation.roll;
    };
    units[0] = g.unit_wheels[0];
    place(units[0], g.truck_placement);
    for (int i = 0; i < kMaxTrailers && 1 + i < tracker::kMaxUnits; ++i) {
        wear[1 + i] = g.trailer_wheels_wear[i];
        if (g.trailer_connected[i]) {
            units[1 + i] = g.unit_wheels[1 + i];
            place(units[1 + i], g.trailer_placement[i]);
        }
    }
    tire_probe::SetUnitWear(wear, tracker::kMaxUnits);
    tracker::SetUnits(units, tracker::kMaxUnits);
    present_probe::Tick();
    FlushProbeMessages();
}

SCSAPI_VOID OnPause(const scs_event_t ev, const void*, const scs_context_t) {
    g.paused = (ev == SCS_TELEMETRY_EVENT_paused);
}

template <typename T>
bool RegisterChannel(scs_telemetry_register_for_channel_t reg, const char* name, scs_value_type_t type,
                     scs_telemetry_channel_callback_t cb, T* target, scs_u32_t index = SCS_U32_NIL) {
    const scs_result_t r = reg(name, index, type, SCS_TELEMETRY_CHANNEL_FLAG_none, cb, target);
    if (r != SCS_RESULT_ok) Log(SCS_LOG_TYPE_warning, std::string("channel not registered: ") + name);
    return r == SCS_RESULT_ok;
}

}  // namespace

SCSAPI_RESULT scs_telemetry_init(const scs_u32_t version, const scs_telemetry_init_params_t* const params) {
    // Accept 1.00 and 1.01 (the game offers the newest it supports first).
    if (version != SCS_TELEMETRY_VERSION_1_01 && version != SCS_TELEMETRY_VERSION_1_00)
        return SCS_RESULT_unsupported;

    const auto* p = static_cast<const scs_telemetry_init_params_v101_t*>(params);
    g.log = p->common.log;
    g.trailer_wheels_wear.fill(-1.f);
    g.t0 = g.last_row = std::chrono::steady_clock::now();

    Log(SCS_LOG_TYPE_message, std::string("init: game=") + p->common.game_id + " game_version=" +
        std::to_string(SCS_GET_MAJOR_VERSION(p->common.game_version)) + "." +
        std::to_string(SCS_GET_MINOR_VERSION(p->common.game_version)) + " telemetry_api=" +
        std::to_string(SCS_GET_MAJOR_VERSION(version)) + "." + std::to_string(SCS_GET_MINOR_VERSION(version)));

    g.ats = strcmp(p->common.game_id, SCS_GAME_ID_ATS) == 0;
    if (!g.ats && strcmp(p->common.game_id, SCS_GAME_ID_EUT2) != 0)
        Log(SCS_LOG_TYPE_warning, "unknown game; using the ETS2 folder and catalog");
    tire_probe::SetGame(g.ats ? tire_probe::Game::kAts : tire_probe::Game::kEts2);

    if (!OpenCsv()) {
        Log(SCS_LOG_TYPE_error, std::string("cannot open CSV in Documents\\") +
                                    (g.ats ? "American Truck Simulator" : "Euro Truck Simulator 2") + "\\TireWear");
        return SCS_RESULT_generic_error;
    }

    p->register_for_event(SCS_TELEMETRY_EVENT_frame_end, OnFrameEnd, nullptr);
    p->register_for_event(SCS_TELEMETRY_EVENT_paused, OnPause, nullptr);
    p->register_for_event(SCS_TELEMETRY_EVENT_started, OnPause, nullptr);
    p->register_for_event(SCS_TELEMETRY_EVENT_configuration, OnConfig, nullptr);

    auto reg = p->register_for_channel;
    RegisterChannel(reg, SCS_TELEMETRY_TRUCK_CHANNEL_speed, SCS_VALUE_TYPE_float, OnFloat, &g.speed_ms);
    RegisterChannel(reg, SCS_TELEMETRY_TRUCK_CHANNEL_odometer, SCS_VALUE_TYPE_float, OnFloat, &g.odometer_km);
    RegisterChannel(reg, SCS_TELEMETRY_TRUCK_CHANNEL_wear_wheels, SCS_VALUE_TYPE_float, OnFloat,
                    &g.truck_wheels_wear);
    RegisterChannel(reg, SCS_TELEMETRY_TRUCK_CHANNEL_world_placement, SCS_VALUE_TYPE_dplacement, OnDPlacement,
                    &g.truck_placement);

    // Per the SDK header notes, trailer i uses "trailer.<i>." instead of "trailer.".
    for (int i = 0; i < kMaxTrailers; ++i) {
        const std::string base = "trailer." + std::to_string(i) + ".";
        RegisterChannel(reg, (base + "connected").c_str(), SCS_VALUE_TYPE_bool, OnBool, &g.trailer_connected[i]);
        RegisterChannel(reg, (base + "wear.wheels").c_str(), SCS_VALUE_TYPE_float, OnFloat,
                        &g.trailer_wheels_wear[i]);
        RegisterChannel(reg, (base + "world.placement").c_str(), SCS_VALUE_TYPE_dplacement, OnDPlacement,
                        &g.trailer_placement[i]);
    }

    g.probe_cfg = present_probe::LoadConfig(TireWearDir());
    if (g.probe_cfg.tires_log) {
        // R2 must see the world's textures being created, so hook now
        // (before the profile / world loads) instead of after the delay.
        Log(SCS_LOG_TYPE_message, std::string("present probe: on at load (tire probe R2") +
                                      (g.probe_cfg.overlay ? " + HUD" : "") + ")" +
                                      (g.probe_cfg.tires_capture ? ", capture " +
                                                                       std::to_string(g.probe_cfg.capture_delay_sec) +
                                                                       " s after the first unpaused frame"
                                                                 : std::string()));
        g.probe_started = true;
        present_probe::Start(TireWearDir(), g.probe_cfg);
    } else if (g.probe_cfg.probe) {
        const char* what = g.probe_cfg.overlay ? "hook + HUD" : g.probe_cfg.hook ? "hook" : "locate only";
        Log(SCS_LOG_TYPE_message, std::string("present probe (") + what + "): on, delay " +
                                      std::to_string(g.probe_cfg.delay_sec) + " s after the first unpaused frame");
    } else {
        Log(SCS_LOG_TYPE_message, "present probe: off (ini)");
    }

    Log(SCS_LOG_TYPE_message, "ready, logging every 1 s");
    return SCS_RESULT_ok;
}

SCSAPI_VOID scs_telemetry_shutdown(void) {
    present_probe::Stop();
    FlushProbeMessages();
    if (g.csv) {
        WriteRow();
        fclose(g.csv);
        g.csv = nullptr;
    }
    Log(SCS_LOG_TYPE_message, "shutdown");
    g.log = nullptr;
}
