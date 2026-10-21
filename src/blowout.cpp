#include "blowout.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

namespace blowout {

double RatePerKm(double wear, const Params& p) {
    if (!(p.km_at_100 > 0.f) || wear <= p.start_wear || p.start_wear >= 1.f) return 0.0;
    const double x = std::min(1.0, (wear - p.start_wear) / (1.0 - p.start_wear));
    return std::pow(x, p.curve) / p.km_at_100;
}

double MeanKm(double wear, const Params& p) {
    const double r = RatePerKm(wear, p);
    return r > 0.0 ? 1.0 / r : std::numeric_limits<double>::infinity();
}

Engine::Engine(uint64_t seed) : rng_(seed) {}

Change Engine::Step(const std::string& key, float wear, int wheel_count, double km, bool allow) {
    if (key.empty() || wear < 0.f) return Change::kNone;  // no telemetry yet
    auto it = state_.find(key);
    if (it != state_.end() && it->second.active) {
        // Repaired or replaced in the game: the unit's wear went down (wear
        // never drops while driving). The drop needed is 5 points, or half
        // the wear for a tire that blew almost new (test blowouts) [verified:
        // E1 test, blew at 1.1%].
        if (wear < it->second.wear - std::min(0.05f, it->second.wear * 0.5f)) {
            state_.erase(it);
            return Change::kRepaired;
        }
        return Change::kNone;  // one blown tire per unit at a time
    }
    if (!allow || !p_.enabled || km <= 0.0 || wheel_count <= 0) return Change::kNone;
    const double rate = RatePerKm(wear, p_);
    if (rate <= 0.0) return Change::kNone;
    std::uniform_real_distribution<double> u(0.0, 1.0);
    if (u(rng_) >= 1.0 - std::exp(-rate * km)) return Change::kNone;
    return Force(key, wear, wheel_count);
}

Change Engine::Force(const std::string& key, float wear, int wheel_count, int wheel, bool test) {
    if (key.empty() || wheel_count <= 0) return Change::kNone;
    std::uniform_int_distribution<int> pick(0, wheel_count - 1);
    Burst& b = state_[key];
    b.active = true;
    b.test = test;
    b.wheel = wheel >= 0 && wheel < wheel_count ? wheel : pick(rng_);
    b.wear = wear < 0.f ? 0.f : wear;
    return Change::kBlown;
}

Burst Engine::Get(const std::string& key) const {
    auto it = state_.find(key);
    return it == state_.end() ? Burst{} : it->second;
}

bool Engine::Load(const std::wstring& file) {
    FILE* f = _wfopen(file.c_str(), L"rb");
    if (!f) return false;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        const size_t t1 = s.find('\t'), t2 = t1 == std::string::npos ? t1 : s.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        Burst b;
        b.active = true;
        b.wheel = atoi(s.c_str() + t1 + 1);
        b.wear = static_cast<float>(atof(s.c_str() + t2 + 1));
        state_[s.substr(0, t1)] = b;
    }
    fclose(f);
    return true;
}

bool Engine::Save(const std::wstring& file) const {
    FILE* f = _wfopen(file.c_str(), L"wb");
    if (!f) return false;
    for (const auto& [key, b] : state_)
        if (b.active && !b.test) fprintf(f, "%s\t%d\t%.4f\n", key.c_str(), b.wheel, b.wear);
    fclose(f);
    return true;
}

WheelPos Locate(const float (*wheels)[3], int count, int wheel) {
    WheelPos pos;
    if (wheel < 0 || wheel >= count) return pos;
    // Axles: distinct z positions (within 10 cm), front (smallest z) first.
    std::vector<float> axles;
    for (int i = 0; i < count; ++i) {
        bool known = false;
        for (float z : axles) known |= std::fabs(z - wheels[i][2]) < 0.1f;
        if (!known) axles.push_back(wheels[i][2]);
    }
    std::sort(axles.begin(), axles.end());
    for (size_t a = 0; a < axles.size(); ++a)
        if (std::fabs(axles[a] - wheels[wheel][2]) < 0.1f) pos.axle = static_cast<int>(a) + 1;
    pos.axles = static_cast<int>(axles.size());
    pos.left = wheels[wheel][0] < 0.f;
    return pos;
}

}  // namespace blowout
