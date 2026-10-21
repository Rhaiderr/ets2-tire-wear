// ETS2 tire wear - step 1: read-only telemetry logger.
//
// Logs truck/trailer wheel wear (per-unit average, as exposed by the SCS
// Telemetry SDK) plus speed and odometer to a CSV, so we can check scale,
// whether the value changes while driving, and how many trailers appear.
// Optionally (ets2_tire_wear.ini, [present] probe / hook / overlay) runs tests A-C of the
// Present hook plan: locate the system DXGI Present, detour it and draw a wear HUD.

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include "scssdk_input.h"
#include "scssdk_telemetry.h"
#include "amtrucks/scssdk_ats.h"
#include "eurotrucks2/scssdk_eut2.h"
#include "eurotrucks2/scssdk_telemetry_eut2.h"
#include "common/scssdk_telemetry_common_channels.h"
#include "common/scssdk_telemetry_common_configs.h"
#include "common/scssdk_telemetry_truck_common_channels.h"
#include "common/scssdk_telemetry_trailer_common_channels.h"

#include "blowout.h"
#include "effect.h"
#include "ffb.h"
#include "notice.h"
#include "overlay.h"
#include "player_tracker.h"
#include "present_probe.h"
#include "tire_audio.h"
#include "tire_probe.h"

namespace {

constexpr int kMaxTrailers = SCS_TELEMETRY_trailers_count;
constexpr double kLogIntervalSec = 1.0;

struct State {
    scs_log_t log = nullptr;
    bool ats = false;  // American Truck Simulator (game_id "ats"); otherwise ETS2
    bool lang_pt = false;  // T5: the game is in Portuguese (notices); otherwise English
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
    // Stage 3: blowouts.
    std::string unit_key[tracker::kMaxUnits];  // "truck|<model>|<plate>", "trailer|<model>|<plate>"; "" = unknown
    double last_odometer = -1.0;
    int32_t mp_offset = 0;    // multiplayer.time.offset: non-zero in a Convoy session
    bool truckersmp = false;  // TruckersMP client module in the process
    bool mp_logged = false;
    std::chrono::steady_clock::time_point last_mp_check{};
    std::chrono::steady_clock::time_point last_frame{};
    double test_drive_s = 0.0;
    bool test_done = false;
    // E2: the effect of a blown tire, fed back as driver input.
    bool steerable[tracker::kMaxUnits][tracker::kMaxWheels] = {};
    float input_steering = 0.f, effective_steering = 0.f, input_throttle = 0.f, effective_throttle = 0.f,
          effective_brake = 0.f;
    bool truck_blew = false, trailer_blew = false;  // a tire blew this frame
    bool light_brake = false;
    std::chrono::steady_clock::time_point effect_last{}, effect_last_log{};
    bool effect_stopped = false;  // Ctrl+Shift+B, for the rest of the session
    bool stop_key_down = false;
    bool effect_logged_idle = true;
    // E4a: the bang and the flapping tire, locked to the blown wheel's rotation.
    bool sound = true;
    float sound_volume = 0.8f, flap_volume = 0.6f;
    bool visual = true;  // E4b: draw the blown wheel shredded
    bool ffb = true;     // F1/F2: a force of our own on the player's force feedback wheel
    bool ffb_invert = false;
    std::string ffb_device;  // part of the wheel's name; empty = the first wheel with force feedback
    float wheel_rotation[tracker::kMaxUnits][tracker::kMaxWheels] = {};  // turns, 0 ... 1
    float wheel_rps[tracker::kMaxUnits][tracker::kMaxWheels] = {};       // turns per second
};

State g;
blowout::Engine g_blowout(static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
effect::Shaper g_effect;

// Values for the input device: written at the end of a telemetry frame, read
// by the game's input callback (both on the main thread [estimate], atomic anyway).
enum { kInSteering, kInThrottle, kInBrake, kInTrailerBrake, kInCount };
std::atomic<float> g_input[kInCount];
scs_log_t g_input_log = nullptr;  // the input API has its own init; may run before telemetry

void Log(scs_log_type_t type, const std::string& msg) {
    if (scs_log_t log = g.log ? g.log : g_input_log) log(type, ("[ets2_tire_wear] " + msg).c_str());
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

// T5: the game's language from its config.cfg (`uset g_lang "pt_br"`, or
// `g_lang_init` [verified: vega 122, ETS2 and ATS]). Notices are in Portuguese
// for pt_br/pt_pt and in English for any other language.
bool GameLanguagePt() {
    std::wstring path = TireWearDir();
    path = path.substr(0, path.rfind(L'\\')) + L"\\config.cfg";
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    std::string lang, lang_init;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char name[64] = {}, value[64] = {};
        if (sscanf(line, " uset %63s \"%63[^\"]\"", name, value) != 2) continue;
        if (strcmp(name, "g_lang") == 0) lang = value;
        else if (strcmp(name, "g_lang_init") == 0) lang_init = value;
    }
    fclose(f);
    if (lang.empty()) lang = lang_init;
    return lang.compare(0, 2, "pt") == 0;
}

void FlushProbeMessages() {
    for (const auto& m : present_probe::TakeMessages()) Log(SCS_LOG_TYPE_message, m);
    for (const auto& m : ffb::TakeMessages()) Log(SCS_LOG_TYPE_message, m);
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
    bool steerable[tracker::kMaxWheels] = {};
    std::string model, plate;
    for (const scs_named_value_t* a = cfg->attributes; a && a->name; ++a) {
        if (a->value.type == SCS_VALUE_TYPE_string && a->value.value_string.value) {
            if (!strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_id)) model = a->value.value_string.value;
            if (!strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_license_plate)) plate = a->value.value_string.value;
        }
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
        if (!strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_wheel_steerable) && a->index < tracker::kMaxWheels &&
            a->value.type == SCS_VALUE_TYPE_bool)
            steerable[a->index] = a->value.value_bool.value != 0;
    }
    g.configs[id] = text;
    if (unit >= 0 && unit < tracker::kMaxUnits) {
        g.unit_wheels[unit] = wheels;  // empty config = unit gone
        std::copy(std::begin(steerable), std::end(steerable), g.steerable[unit]);
        // Stage 3: the blowout state follows the vehicle (model + plate), so a
        // dropped trailer keeps its blown tire when picked up again.
        for (char& c : plate)
            if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        g.unit_key[unit] = model.empty() ? "" : (unit == 0 ? "truck|" : "trailer|") + model + "|" + plate;
    }
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

// ---- stage 3: blowouts ----

SCSAPI_VOID OnS32(const scs_string_t, const scs_u32_t, const scs_value_t* value, const scs_context_t ctx) {
    if (value && value->type == SCS_VALUE_TYPE_s32) *static_cast<int32_t*>(ctx) = value->value_s32.value;
}

void BlowoutLog(const std::string& msg) {
    Log(SCS_LOG_TYPE_message, "blowout: " + msg);
    if (FILE* f = _wfopen((TireWearDir() + L"\\blowout.log").c_str(), L"ab")) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d %s\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                msg.c_str());
        fclose(f);
    }
}

void LoadBlowoutParams() {
    const std::wstring ini = TireWearDir() + L"\\ets2_tire_wear.ini";
    const wchar_t* file = ini.c_str();
    auto number = [&](const wchar_t* key, float fallback) {
        wchar_t buf[32] = {};
        GetPrivateProfileStringW(L"blowout", key, L"", buf, 32, file);
        return buf[0] ? wcstof(buf, nullptr) : fallback;
    };
    blowout::Params p;
    p.enabled = GetPrivateProfileIntW(L"blowout", L"enabled", 1, file) == 1;  // on also for INIs from older versions
    p.start_wear = std::clamp(number(L"start_wear", p.start_wear), 0.f, 0.99f);
    p.km_at_100 = std::max(1.f, number(L"km_at_100", p.km_at_100));
    p.curve = std::clamp(number(L"curve", p.curve), 0.1f, 10.f);
    p.test_after_s = static_cast<int>(GetPrivateProfileIntW(L"blowout", L"test_after_s", 0, file));
    wchar_t unit[16] = {};
    GetPrivateProfileStringW(L"blowout", L"test_unit", L"truck", unit, 16, file);
    p.test_trailer = _wcsicmp(unit, L"trailer") == 0;
    p.test_wheel = static_cast<int>(GetPrivateProfileIntW(L"blowout", L"test_wheel", -1, file));
    g_blowout.SetParams(p);
    char msg[640];
    snprintf(msg, sizeof(msg), "%s, start_wear %.2f, km_at_100 %.0f, curve %.1f", p.enabled ? "on" : "off", p.start_wear,
             p.km_at_100, p.curve);
    std::string text = msg;
    if (p.test_after_s > 0) {
        snprintf(msg, sizeof(msg), ", test mode (%s, wheel %d, after %d s)", p.test_trailer ? "trailer" : "truck",
                 p.test_wheel, p.test_after_s);
        text += msg;
    }
    Log(SCS_LOG_TYPE_message, "blowout: " + text);

    auto effect_number = [&](const wchar_t* key, float fallback) {
        wchar_t buf[32] = {};
        GetPrivateProfileStringW(L"effect", key, L"", buf, 32, file);
        return buf[0] ? wcstof(buf, nullptr) : fallback;
    };
    effect::Params e;
    e.enabled = GetPrivateProfileIntW(L"effect", L"enabled", 1, file) == 1;
    e.pull = std::clamp(effect_number(L"pull", e.pull), 0.f, 1.f);
    e.pull_rear = std::clamp(effect_number(L"pull_rear", e.pull_rear), 0.f, 1.f);
    e.kick = std::clamp(effect_number(L"kick", e.kick), 1.f, 5.f);
    e.kick_s = std::clamp(effect_number(L"kick_s", e.kick_s), 0.f, 5.f);
    e.kick_brake = std::clamp(effect_number(L"kick_brake", e.kick_brake), 0.f, 1.f);
    e.kick_brake_s = std::clamp(effect_number(L"kick_brake_s", e.kick_brake_s), 0.f, 10.f);
    e.throttle_cut = std::clamp(effect_number(L"throttle_cut", e.throttle_cut), 0.f, 1.f);
    e.trailer_throttle_cut = std::clamp(effect_number(L"trailer_throttle_cut", e.trailer_throttle_cut), 0.f, 1.f);
    e.trailer_hold = std::clamp(effect_number(L"trailer_hold", e.trailer_hold), 0.f, 1.f);
    e.truck_trailer_hold = std::clamp(effect_number(L"truck_trailer_hold", e.truck_trailer_hold), 0.f, 1.f);
    e.trailer_pedal = std::clamp(effect_number(L"trailer_pedal", e.trailer_pedal), 0.f, 1.f);
    e.limit_truck_kmh = std::max(10.f, effect_number(L"limit_truck_kmh", e.limit_truck_kmh));
    e.limit_trailer_kmh = std::max(10.f, effect_number(L"limit_trailer_kmh", e.limit_trailer_kmh));
    e.governor_kmh = std::clamp(effect_number(L"governor_kmh", e.governor_kmh), 0.f, 30.f);
    e.limit_brake_after_kmh = std::clamp(effect_number(L"limit_brake_after_kmh", e.limit_brake_after_kmh), 0.f, 50.f);
    e.limit_band_kmh = std::max(1.f, effect_number(L"limit_band_kmh", e.limit_band_kmh));
    e.limit_brake = std::clamp(effect_number(L"limit_brake", e.limit_brake), 0.f, 1.f);
    e.wobble = std::clamp(effect_number(L"wobble", e.wobble), 0.f, 0.2f);
    e.min_speed_kmh = std::max(0.f, effect_number(L"min_speed_kmh", e.min_speed_kmh));
    e.full_speed_kmh = std::max(1.f, effect_number(L"full_speed_kmh", e.full_speed_kmh));
    e.steer_invert = GetPrivateProfileIntW(L"effect", L"steer_invert", 0, file) == 1;
    e.ffb_pull = std::clamp(effect_number(L"ffb_pull", e.ffb_pull), 0.f, 1.f);
    e.ffb_kick = std::clamp(effect_number(L"ffb_kick", e.ffb_kick), 0.f, 1.f);
    e.ffb_wobble = std::clamp(effect_number(L"ffb_wobble", e.ffb_wobble), 0.f, 0.5f);
    g_effect.SetParams(e);
    g.sound = GetPrivateProfileIntW(L"effect", L"sound", 1, file) == 1;
    g.sound_volume = std::clamp(effect_number(L"sound_volume", g.sound_volume), 0.f, 1.f);
    g.flap_volume = std::clamp(effect_number(L"flap_volume", g.flap_volume), 0.f, 1.f);
    g.visual = GetPrivateProfileIntW(L"effect", L"visual", 1, file) == 1;
    g.ffb = GetPrivateProfileIntW(L"effect", L"ffb", 1, file) == 1;
    g.ffb_invert = GetPrivateProfileIntW(L"effect", L"ffb_invert", 0, file) == 1;
    wchar_t device[128] = {};
    GetPrivateProfileStringW(L"effect", L"ffb_device", L"", device, 128, file);
    char device_utf8[384] = {};
    WideCharToMultiByte(CP_UTF8, 0, device, -1, device_utf8, sizeof(device_utf8), nullptr, nullptr);
    g.ffb_device = device_utf8;
    char sound[32] = "off";
    if (g.sound) snprintf(sound, sizeof(sound), "%.2f (flap %.2f)", g.sound_volume, g.flap_volume);
    snprintf(msg, sizeof(msg),
             "%s, pull %.2f (rear x%.2f, kick x%.1f for %.1f s), brake tap %.2f for %.1f s, throttle cut %.2f "
             "(trailer %.2f), trailer_hold %.2f (truck tire %.2f) + pedal %.2f, limit %.0f/%.0f km/h "
             "(governor %.0f km/h, brake %.2f from +%.0f over %.0f km/h), wobble %.3f, sound %s, "
             "ffb %s (pull %.2f, kick %.2f, wobble %.2f%s)%s",
             e.enabled ? "on" : "off", e.pull, e.pull_rear, e.kick, e.kick_s, e.kick_brake, e.kick_brake_s,
             e.throttle_cut, e.trailer_throttle_cut, e.trailer_hold, e.truck_trailer_hold, e.trailer_pedal,
             e.limit_truck_kmh, e.limit_trailer_kmh, e.governor_kmh, e.limit_brake, e.limit_brake_after_kmh,
             e.limit_band_kmh, e.wobble, sound, g.ffb ? "on" : "off", e.ffb_pull, e.ffb_kick, e.ffb_wobble,
             g.ffb_invert ? ", inverted" : "",
             e.steer_invert ? ", steer inverted" : "");
    Log(SCS_LOG_TYPE_message, std::string("effect: ") + msg);
}

// Blowouts are a single-player feature: off in a Convoy session (official
// multiplayer, seen in the telemetry) and with TruckersMP (its client module
// in the game process [estimate: module names to be confirmed]).
bool InMultiplayer() {
    const auto now = std::chrono::steady_clock::now();
    if (now - g.last_mp_check > std::chrono::seconds(5)) {
        g.last_mp_check = now;
        g.truckersmp = GetModuleHandleW(L"core_ets2mp.dll") != nullptr || GetModuleHandleW(L"core_atsmp.dll") != nullptr;
    }
    return g.mp_offset != 0 || g.truckersmp;
}

std::string UnitName(int unit, bool pt) {
    if (unit == 0) return pt ? "Caminhao" : "truck";
    return (pt ? "Carreta " : "trailer ") + std::to_string(unit - 1);
}

std::string WheelText(const blowout::WheelPos& w, bool pt) {
    if (!w.axle) return pt ? "roda ?" : "wheel ?";
    char b[64];
    snprintf(b, sizeof(b), pt ? "eixo %d de %d, %s" : "axle %d of %d, %s", w.axle, w.axles,
             pt ? (w.left ? "esquerda" : "direita") : (w.left ? "left" : "right"));
    return b;
}

// T4: the notice for a tire that just blew, in the game's language (UTF-8).
void ShowBlowoutNotice(int unit, const blowout::WheelPos& w, bool test) {
    const bool pt = g.lang_pt;
    std::string who = unit == 0 ? (pt ? "Caminh\xC3\xA3o" : "Truck") : (pt ? "Carreta" : "Trailer");
    if (unit > 1) who += " " + std::to_string(unit);
    char where[96];
    if (w.axle)
        snprintf(where, sizeof(where), pt ? "%s: eixo %d, lado %s." : "%s: axle %d, %s side.", who.c_str(), w.axle,
                 pt ? (w.left ? "esquerdo" : "direito") : (w.left ? "left" : "right"));
    else
        snprintf(where, sizeof(where), "%s.", who.c_str());
    std::string title = pt ? "Pneu estourado!" : "Tire blown!";
    if (test) title += pt ? " (teste)" : " (test)";
    notice::Show(title, std::string(where) + "\n" +
                            (pt ? "Troque o pneu na oficina." : "Replace the tire at a service station."));
}

// Once per frame: distance since the last frame, a draw per present unit,
// repairs, the test blowout, the HUD alert and the saved state.
void BlowoutStep(const float* wear, const tracker::Unit* units) {
    const blowout::Params& p = g_blowout.params();
    const auto now = std::chrono::steady_clock::now();
    const double dt = g.last_frame.time_since_epoch().count() ? std::chrono::duration<double>(now - g.last_frame).count() : 0.0;
    g.last_frame = now;
    double km = 0.0;
    if (!g.paused && g.odometer_km > 0.f) {
        // More than 1 km in one frame is a jump (ferry, train, profile change), not driving.
        if (g.last_odometer >= 0.0) km = g.odometer_km - g.last_odometer;
        if (km < 0.0 || km > 1.0) km = 0.0;
        g.last_odometer = g.odometer_km;
    }
    const bool mp = InMultiplayer();
    if (mp && !g.mp_logged) {
        g.mp_logged = true;
        BlowoutLog(std::string("multiplayer (") + (g.truckersmp ? "TruckersMP" : "Convoy") + "): no blowouts in this session");
    }
    bool changed = false;
    std::string alert;
    g.truck_blew = g.trailer_blew = false;
    for (int u = 0; u < tracker::kMaxUnits; ++u) {
        if (units[u].wheel_count <= 0 || g.unit_key[u].empty()) continue;
        const std::string& key = g.unit_key[u];
        blowout::Change ch = g_blowout.Step(key, wear[u], units[u].wheel_count, km, !mp);
        const bool test_unit = u == (p.test_trailer ? 1 : 0);
        bool test = false;
        if (test_unit && p.test_after_s > 0 && !g.test_done && !mp && !g.paused && g.speed_ms > 2.f) {
            g.test_drive_s += dt;
            if (g.test_drive_s >= p.test_after_s) {
                g.test_done = true;
                if (!g_blowout.Get(key).active) {
                    // A test blowout lasts for this session only: at ~0% wear it could
                    // never be seen as repaired [verified: E2 test, blew at 0.0%].
                    ch = g_blowout.Force(key, wear[u], units[u].wheel_count, p.test_wheel, true);
                    test = true;
                }
            }
        }
        const blowout::Burst b = g_blowout.Get(key);
        const blowout::WheelPos where = blowout::Locate(units[u].wheel, units[u].wheel_count, b.wheel);
        if (ch == blowout::Change::kBlown) {
            changed = true;
            (u == 0 ? g.truck_blew : g.trailer_blew) = true;
            if (g.sound && !g.paused && g_effect.params().enabled) tire_audio::Bang(g.sound_volume);
            if (!mp) ShowBlowoutNotice(u, where, test);
            char msg[256];
            snprintf(msg, sizeof(msg), "%s tire blew: wheel %d (%s) at %.1f%% wear, odometer %.1f km%s [%s]",
                     UnitName(u, false).c_str(), b.wheel, WheelText(where, false).c_str(), wear[u] * 100.f,
                     g.odometer_km, test ? ", test (this session only)" : "", key.c_str());
            BlowoutLog(msg);
        } else if (ch == blowout::Change::kRepaired) {
            changed = true;
            char msg[160];
            snprintf(msg, sizeof(msg), "%s repaired (wear now %.1f%%) [%s]", UnitName(u, false).c_str(), wear[u] * 100.f,
                     key.c_str());
            BlowoutLog(msg);
        }
        if (b.active && !mp) alert += "PNEU ESTOURADO: " + UnitName(u, true) + ", " + WheelText(where, true) + "\n";
    }
    if (changed) g_blowout.Save(TireWearDir() + L"\\blowout_state.txt");
    if (!alert.empty()) alert.pop_back();
    if (!alert.empty() && g.effect_stopped) alert += "\n(efeito parado: Ctrl+Shift+B)";
    overlay::SetAlert(alert);
}

// E2: what the blown tires do to the truck, handed to the input device. A
// truck tire pulls to its side and drags; a trailer tire only drags.
void EffectStep(const tracker::Unit* units) {
    const auto now = std::chrono::steady_clock::now();
    const double dt =
        g.effect_last.time_since_epoch().count() ? std::chrono::duration<double>(now - g.effect_last).count() : 0.0;
    g.effect_last = now;
    // Ctrl+Shift+B stops the effect for the rest of the session (a way out while testing).
    const bool key = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
                     (GetAsyncKeyState('B') & 0x8000);
    if (key && !g.stop_key_down && !g.effect_stopped) {
        g.effect_stopped = true;
        BlowoutLog("effect stopped with Ctrl+Shift+B for this session");
    }
    g.stop_key_down = key;

    effect::Input in;
    in.allow = !g.paused && !InMultiplayer() && !g.effect_stopped;
    in.speed_kmh = g.speed_ms * 3.6f;
    in.truck_blew = g.truck_blew;
    in.trailer_blew = g.trailer_blew;
    in.trailer_attached = g.trailer_connected[0];
    int heard_unit = -1, heard_wheel = -1;  // the blown wheel we hear (the truck's first)
    const bool show = g.visual && !InMultiplayer();
    for (int u = 0; u < tracker::kMaxUnits; ++u) {
        blowout::Burst b;
        if (units[u].wheel_count > 0 && !g.unit_key[u].empty()) b = g_blowout.Get(g.unit_key[u]);
        const bool blown = b.active && b.wheel >= 0 && b.wheel < units[u].wheel_count;
        // E4b: that wheel is drawn shredded, with its sidewall gash.
        tire_probe::SetBlown(u, blown && show ? b.wheel : -1, b.side_angle, b.side_size);
        if (!blown) continue;
        if (heard_unit < 0) heard_unit = u, heard_wheel = b.wheel;
        if (u == 0) {
            in.truck = true;
            in.truck_steered = g.steerable[0][b.wheel];
            in.truck_left = units[0].wheel[b.wheel][0] < 0.f;  // x < 0 = left [estimate]
        } else {
            in.trailer = true;
        }
    }
    in.wheel_ffb = ffb::Active();
    const effect::Output out = g_effect.Update(in, dt);
    ffb::SetForce(out.force);
    // The torn tire slapping the road, in step with that wheel's rotation.
    const bool flapping = heard_unit >= 0 && in.allow && g.sound && g_effect.params().enabled;
    tire_audio::SetWheel(flapping, flapping ? g.wheel_rotation[heard_unit][heard_wheel] : 0.0,
                         flapping ? g.wheel_rps[heard_unit][heard_wheel] : 0.0, g.flap_volume);
    g_input[kInSteering] = out.steering;
    g_input[kInThrottle] = out.throttle;
    g_input[kInBrake] = out.brake;
    g_input[kInTrailerBrake] = out.trailer_brake;

    // Twice a second while anything is going on: what we send and what the
    // game made of it (sum with the wheel, pull side, brake lights).
    const bool busy = in.truck || in.trailer || out.steering != 0.f || out.throttle != 0.f || out.brake != 0.f ||
                      out.trailer_brake != 0.f || out.force != 0.f;
    if (!busy) {
        if (!g.effect_logged_idle) {
            g.effect_logged_idle = true;
            if (FILE* f = _wfopen((TireWearDir() + L"\\effect.log").c_str(), L"ab")) {
                fprintf(f, "idle\n");
                fclose(f);
            }
        }
        return;
    }
    if (now - g.effect_last_log < std::chrono::milliseconds(500)) return;
    g.effect_last_log = now;
    if (FILE* f = _wfopen((TireWearDir() + L"\\effect.log").c_str(), L"ab")) {
        if (g.effect_logged_idle)
            fprintf(f, "time,speed_kmh,source,allow,send_steering,send_throttle,send_brake,send_trailer_brake,"
                       "input_steering,effective_steering,input_throttle,effective_throttle,effective_brake,light_brake,"
                       "wheel_ffb,send_force\n");
        g.effect_logged_idle = false;
        SYSTEMTIME st;
        GetLocalTime(&st);
        const char* source = in.truck ? (in.truck_steered ? (in.truck_left ? "truck_steered_left" : "truck_steered_right")
                                                          : (in.truck_left ? "truck_left" : "truck_right"))
                                      : in.trailer ? "trailer" : "-";
        fprintf(f, "%02d:%02d:%02d.%03d,%.1f,%s%s,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d,%.3f\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, in.speed_kmh, source,
                in.truck && in.trailer ? "+trailer" : "", in.allow ? 1 : 0, out.steering, out.throttle, out.brake,
                out.trailer_brake, g.input_steering, g.effective_steering, g.input_throttle, g.effective_throttle,
                g.effective_brake, g.light_brake ? 1 : 0, in.wheel_ffb ? 1 : 0, out.force);
        fclose(f);
    }
}

// ---- E2: input device (SCS Input API, semantical) ----

const scs_input_device_input_t kInputs[kInCount] = {
    {"steering", "Steering", SCS_VALUE_TYPE_float},
    {"aforward", "Throttle", SCS_VALUE_TYPE_float},
    {"abackward", "Brake", SCS_VALUE_TYPE_float},
    {"trailerbrake", "Trailer brake", SCS_VALUE_TYPE_float},
};

SCSAPI_VOID OnInputActive(const scs_u8_t active, const scs_context_t) {
    Log(SCS_LOG_TYPE_message, std::string("effect input device ") + (active ? "active" : "inactive"));
}

// Called repeatedly each frame until it says "no more": one event per input
// whose value changed (all of them again after the device is re-activated).
SCSAPI_RESULT OnInputEvent(scs_input_event_t* const event_info, const scs_u32_t flags, const scs_context_t) {
    static float sent[kInCount] = {NAN, NAN, NAN, NAN};
    if (flags & SCS_INPUT_EVENT_CALLBACK_FLAG_first_after_activation) std::fill(std::begin(sent), std::end(sent), NAN);
    for (scs_u32_t i = 0; i < kInCount; ++i) {
        const float v = g_input[i].load();
        if (v == sent[i]) continue;  // NaN never equals, so a reset resends
        sent[i] = v;
        event_info->input_index = i;
        event_info->value_float.value = v;
        return SCS_RESULT_ok;
    }
    return SCS_RESULT_not_found;
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
    BlowoutStep(wear, units);
    EffectStep(units);
    present_probe::Tick();
    FlushProbeMessages();
}

SCSAPI_VOID OnPause(const scs_event_t ev, const void*, const scs_context_t) {
    g.paused = (ev == SCS_TELEMETRY_EVENT_paused);
    if (g.paused) {
        // The input device may still be asked for values while paused: let go now.
        for (auto& v : g_input) v = 0.f;
    }
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

    RegisterChannel(reg, SCS_TELEMETRY_CHANNEL_multiplayer_time_offset, SCS_VALUE_TYPE_s32, OnS32, &g.mp_offset);
    RegisterChannel(reg, SCS_TELEMETRY_TRUCK_CHANNEL_input_steering, SCS_VALUE_TYPE_float, OnFloat, &g.input_steering);
    RegisterChannel(reg, SCS_TELEMETRY_TRUCK_CHANNEL_effective_steering, SCS_VALUE_TYPE_float, OnFloat,
                    &g.effective_steering);
    // E4a: every wheel's rotation, for the sound of a blown one.
    for (scs_u32_t w = 0; w < tracker::kMaxWheels; ++w) {
        reg(SCS_TELEMETRY_TRUCK_CHANNEL_wheel_rotation, w, SCS_VALUE_TYPE_float, SCS_TELEMETRY_CHANNEL_FLAG_none, OnFloat,
            &g.wheel_rotation[0][w]);
        reg(SCS_TELEMETRY_TRUCK_CHANNEL_wheel_velocity, w, SCS_VALUE_TYPE_float, SCS_TELEMETRY_CHANNEL_FLAG_none, OnFloat,
            &g.wheel_rps[0][w]);
        for (int i = 0; i < kMaxTrailers && 1 + i < tracker::kMaxUnits; ++i) {
            const std::string base = "trailer." + std::to_string(i) + ".wheel.";
            reg((base + "rotation").c_str(), w, SCS_VALUE_TYPE_float, SCS_TELEMETRY_CHANNEL_FLAG_none, OnFloat,
                &g.wheel_rotation[1 + i][w]);
            reg((base + "angular_velocity").c_str(), w, SCS_VALUE_TYPE_float, SCS_TELEMETRY_CHANNEL_FLAG_none, OnFloat,
                &g.wheel_rps[1 + i][w]);
        }
    }
    RegisterChannel(reg, SCS_TELEMETRY_TRUCK_CHANNEL_input_throttle, SCS_VALUE_TYPE_float, OnFloat, &g.input_throttle);
    RegisterChannel(reg, SCS_TELEMETRY_TRUCK_CHANNEL_effective_throttle, SCS_VALUE_TYPE_float, OnFloat,
                    &g.effective_throttle);
    RegisterChannel(reg, SCS_TELEMETRY_TRUCK_CHANNEL_effective_brake, SCS_VALUE_TYPE_float, OnFloat, &g.effective_brake);
    RegisterChannel(reg, SCS_TELEMETRY_TRUCK_CHANNEL_light_brake, SCS_VALUE_TYPE_bool, OnBool, &g.light_brake);

    g.probe_cfg = present_probe::LoadConfig(TireWearDir());  // also creates the INI when missing
    g.lang_pt = GameLanguagePt();
    Log(SCS_LOG_TYPE_message, std::string("notice language: ") + (g.lang_pt ? "Portuguese" : "English"));
    LoadBlowoutParams();
    if (g.ffb) ffb::Start(g.ffb_invert, g.ffb_device);
    FlushProbeMessages();
    if (g_blowout.Load(TireWearDir() + L"\\blowout_state.txt")) Log(SCS_LOG_TYPE_message, "blowout: state loaded");
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
    tire_audio::Stop();
    ffb::Stop();
    present_probe::Stop();
    FlushProbeMessages();
    if (g.csv) {
        WriteRow();
        fclose(g.csv);
        g.csv = nullptr;
    }
    for (auto& v : g_input) v = 0.f;
    Log(SCS_LOG_TYPE_message, "shutdown");
    g.log = nullptr;
}

// E2: the same DLL is also an input plugin. A semantical device needs no
// binding: its inputs go straight into the game's mixes of the same name.
SCSAPI_RESULT scs_input_init(const scs_u32_t version, const scs_input_init_params_t* const params) {
    if (version != SCS_INPUT_VERSION_1_00) return SCS_RESULT_unsupported;
    const auto* p = static_cast<const scs_input_init_params_v100_t*>(params);
    g_input_log = p->common.log;
    for (auto& v : g_input) v = 0.f;
    scs_input_device_t device{};
    device.name = "tire_wear_effect";
    device.display_name = "Tire Wear effect";
    device.type = SCS_INPUT_DEVICE_TYPE_semantical;
    device.input_count = kInCount;
    device.inputs = kInputs;
    device.input_active_callback = OnInputActive;
    device.input_event_callback = OnInputEvent;
    const scs_result_t r = p->register_device(&device);
    Log(r == SCS_RESULT_ok ? SCS_LOG_TYPE_message : SCS_LOG_TYPE_error,
        "effect input device " + std::string(r == SCS_RESULT_ok ? "registered" : "NOT registered, result ") +
            (r == SCS_RESULT_ok ? "" : std::to_string(r)));
    return r == SCS_RESULT_ok ? SCS_RESULT_ok : SCS_RESULT_generic_error;
}

SCSAPI_VOID scs_input_shutdown(void) {
    for (auto& v : g_input) v = 0.f;
    g_input_log = nullptr;
}
