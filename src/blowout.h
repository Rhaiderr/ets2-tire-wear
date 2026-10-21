// Stage 3 (v2): tire blowouts. The game has no blowout mechanic, so this
// creates one: each unit (truck, each trailer) gets a random chance per km
// that grows with its average wheel wear; when it happens one of its wheels
// is blown until the player repairs the tires (the unit's wear drops).
// Pure logic, no game calls: tools\blowoutsim drives it outside the game.
#pragma once

#include <cstdint>
#include <map>
#include <random>
#include <string>

namespace blowout {

struct Params {
    bool enabled = true;
    float start_wear = 0.60f;  // below this wear a tire never blows
    float km_at_100 = 300.f;   // mean km until a blowout at 100% wear
    float curve = 3.f;         // how fast the risk grows between start_wear and 100%
    int test_after_s = 0;      // test only: > 0 blows a tire after this many s of driving
    bool test_trailer = false; // test only: blow a tire of the first trailer instead of the truck
    int test_wheel = -1;       // test only: telemetry wheel index (-1 = random)
};

// Blowouts per km at `wear` (0..1): km_at_100^-1 * ((w - start) / (1 - start))^curve.
double RatePerKm(double wear, const Params& p);
// Mean km until a blowout at a constant wear (infinite below start_wear).
double MeanKm(double wear, const Params& p);

struct Burst {
    bool active = false;
    int wheel = -1;     // telemetry wheel index of the unit
    float wear = 0.f;   // unit wear when it blew (a lower wear later = repaired)
    bool test = false;  // forced by the test mode: this session only, never saved
    // E4b: the sidewall gash, drawn at the blowout.
    float side_angle = 0.f;  // where around the sidewall (turns, 0..1)
    float side_size = 0.5f;  // 0 = small slit ... 1 = big gash
};

enum class Change { kNone, kBlown, kRepaired };

// Keeps the blowout state per unit key (model + licence plate, so a trailer
// keeps its blown tire when it is dropped and picked up again).
class Engine {
public:
    explicit Engine(uint64_t seed);
    void SetParams(const Params& p) { p_ = p; }
    const Params& params() const { return p_; }

    // One step for one unit: `km` driven since the last step at `wear`.
    // `allow` = false (multiplayer, disabled) only checks for repairs.
    Change Step(const std::string& key, float wear, int wheel_count, double km, bool allow);
    // Forced blowout of `wheel` (-1 = a random wheel); `test` = not saved.
    Change Force(const std::string& key, float wear, int wheel_count, int wheel = -1, bool test = false);
    Burst Get(const std::string& key) const;

    // Text file: one "key<TAB>wheel<TAB>wear" line per blown unit.
    bool Load(const std::wstring& file);
    bool Save(const std::wstring& file) const;

private:
    Params p_;
    std::mt19937_64 rng_;
    std::map<std::string, Burst> state_;
};

// Where a wheel is on its unit, from the unit's wheel positions: axles front
// to back by z (SCS vehicle space, z grows to the rear) and x < 0 = left
// [estimate: the side convention is to be checked in the game].
struct WheelPos {
    int axle = 0, axles = 0;  // 1 = front axle; 0 = unknown
    bool left = false;
};
WheelPos Locate(const float (*wheels)[3], int count, int wheel);

}  // namespace blowout
