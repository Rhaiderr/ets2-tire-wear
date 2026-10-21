// blowoutsim: the blowout curve of src/blowout.cpp, in numbers.
//
// Usage: blowoutsim [start_wear km_at_100 curve km_per_wear_point]
//   defaults: 0.60 300 3 500 (the game wears ~1% per ~500 km [estimate])
// Prints the mean km to a blowout at constant wear, then Monte Carlo runs of
// the Engine itself: (a) at constant 100% wear (checks the mean), (b) a tire
// that starts at start_wear and keeps wearing 1% per km_per_wear_point km.
//
// Build: cl /nologo /O2 /EHsc /W4 /std:c++17 /D_CRT_SECURE_NO_WARNINGS blowoutsim.cpp ..\..\src\blowout.cpp ..\..\src\effect.cpp

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "../../src/blowout.h"
#include "../../src/effect.h"

namespace {

struct Stats {
    double mean, p10, p50, p90;
};

Stats Summarise(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    double s = 0;
    for (double x : v) s += x;
    auto at = [&](double q) { return v[static_cast<size_t>(q * (v.size() - 1))]; };
    return {s / v.size(), at(0.1), at(0.5), at(0.9)};
}

}  // namespace

int main(int argc, char** argv) {
    blowout::Params p;
    double km_per_point = 500.0;
    if (argc > 1) p.start_wear = static_cast<float>(atof(argv[1]));
    if (argc > 2) p.km_at_100 = static_cast<float>(atof(argv[2]));
    if (argc > 3) p.curve = static_cast<float>(atof(argv[3]));
    if (argc > 4) km_per_point = atof(argv[4]);
    printf("start_wear %.2f  km_at_100 %.0f  curve %.1f  (tire wears 1%% per %.0f km)\n\n", p.start_wear,
           p.km_at_100, p.curve, km_per_point);

    printf("constant wear -> mean km until a blowout, chance within 100 km / 1000 km\n");
    for (int w = 55; w <= 100; w += 5) {
        const double r = blowout::RatePerKm(w / 100.0, p);
        if (r <= 0) {
            printf("  %3d%%   never\n", w);
            continue;
        }
        printf("  %3d%%   %9.0f km   %5.1f%%   %5.1f%%\n", w, 1.0 / r, 100.0 * (1.0 - std::exp(-r * 100)),
               100.0 * (1.0 - std::exp(-r * 1000)));
    }

    const int kRuns = 4000;
    const double kStep = 0.5;  // km per Engine step, like a few seconds of driving

    // (a) constant 100%: the Engine's mean must be ~km_at_100.
    {
        blowout::Engine e(12345);
        e.SetParams(p);
        std::vector<double> km;
        for (int r = 0; r < kRuns; ++r) {
            const std::string key = "run" + std::to_string(r);
            double d = 0;
            while (e.Step(key, 1.f, 6, kStep, true) != blowout::Change::kBlown && d < 1e7) d += kStep;
            km.push_back(d);
        }
        const Stats s = Summarise(km);
        printf("\n(a) Engine at constant 100%%, %d runs: mean %.0f km (expected %.0f), median %.0f, 10%% %.0f, 90%% %.0f\n",
               kRuns, s.mean, p.km_at_100, s.p50, s.p10, s.p90);
    }

    // (b) a tire from start_wear that keeps wearing.
    {
        blowout::Engine e(777);
        e.SetParams(p);
        std::vector<double> km, wear_at;
        for (int r = 0; r < kRuns; ++r) {
            const std::string key = "run" + std::to_string(r);
            double d = 0, w = p.start_wear;
            while (true) {
                w = std::min(1.0, p.start_wear + d / km_per_point / 100.0);
                if (e.Step(key, static_cast<float>(w), 6, kStep, true) == blowout::Change::kBlown || d > 1e7) break;
                d += kStep;
            }
            km.push_back(d);
            wear_at.push_back(w * 100.0);
        }
        const Stats s = Summarise(km), sw = Summarise(wear_at);
        printf("(b) from %.0f%% wearing 1%% per %.0f km, %d runs:\n", p.start_wear * 100, km_per_point, kRuns);
        printf("    km driven after %.0f%% until the blowout: mean %.0f, median %.0f, 10%% %.0f, 90%% %.0f\n",
               p.start_wear * 100, s.mean, s.p50, s.p10, s.p90);
        printf("    wear when it blows: mean %.1f%%, median %.1f%%, 10%% %.1f%%, 90%% %.1f%%\n", sw.mean, sw.p50,
               sw.p10, sw.p90);
    }

    // (c) behaviour checks: repair, persistence, multiplayer, wheel names.
    {
        int failed = 0;
        auto check = [&](bool ok, const char* what) {
            printf("    [%s] %s\n", ok ? "ok" : "FAIL", what);
            failed += !ok;
        };
        printf("(c) checks\n");
        blowout::Engine e(1);
        e.SetParams(p);
        const std::string key = "trailer|vehicle.lodeking.bril_flat|NK - 3346";
        check(e.Step(key, 0.50f, 4, 1e6, true) == blowout::Change::kNone, "never blows below start_wear");
        check(e.Step(key, 0.95f, 4, 1e6, false) == blowout::Change::kNone, "never blows when not allowed (multiplayer)");
        check(e.Force(key, 0.95f, 4) == blowout::Change::kBlown && e.Get(key).active, "forced blowout is kept");
        check(e.Step(key, 0.97f, 4, 1e6, true) == blowout::Change::kNone && e.Get(key).active, "stays blown while wear grows");
        const std::wstring file = L"blowoutsim_state.txt";
        check(e.Save(file), "state saved");
        blowout::Engine e2(2);
        e2.SetParams(p);
        check(e2.Load(file) && e2.Get(key).active && e2.Get(key).wheel == e.Get(key).wheel, "state loaded (same wheel)");
        check(e2.Step(key, 0.93f, 4, 0.5, false) == blowout::Change::kNone, "a small wear drop is not a repair");
        check(e2.Step(key, 0.01f, 4, 0.5, false) == blowout::Change::kRepaired && !e2.Get(key).active,
              "repair (wear drops) clears it, also in multiplayer");
        _wremove(file.c_str());
        const std::string truck = "truck|vehicle.scania.r_2016|FX62 RMK";
        e2.Force(truck, 0.011f, 6);  // the E1 test blowout, at 1.1% wear
        check(e2.Step(truck, 0.011f, 6, 0.5, true) == blowout::Change::kNone, "a tire blown at 1.1% stays blown");
        check(e2.Step(truck, 0.0f, 6, 0.5, true) == blowout::Change::kRepaired, "and is repaired when the wear goes to 0");
        // Lode King Brilliance flatbed wheel layout from the telemetry [verified: tire capture, 2026-10-20].
        const float wheels[4][3] = {{-0.95f, 0.52f, 3.374f}, {0.95f, 0.52f, 3.374f}, {-0.95f, 0.52f, 6.472f},
                                    {0.95f, 0.52f, 6.472f}};
        const blowout::WheelPos w0 = blowout::Locate(wheels, 4, 0), w3 = blowout::Locate(wheels, 4, 3);
        check(w0.axle == 1 && w0.axles == 2 && w0.left, "wheel 0 = axle 1 of 2, left");
        check(w3.axle == 2 && w3.axles == 2 && !w3.left, "wheel 3 = axle 2 of 2, right");

        // (d) E2 effect: what goes to the game's input.
        printf("(d) effect checks\n");
        effect::Shaper s;
        const effect::Params& ep = s.params();
        auto run = [&](const effect::Input& in, double seconds) {
            effect::Output o;
            for (double t = 0; t < seconds; t += 0.01) o = s.Update(in, 0.01);
            return o;
        };
        auto blow = [&](effect::Input in) {  // the frame the tire blows
            in.truck_blew = in.truck, in.trailer_blew = in.trailer;
            s.Update(in, 0.01);
        };
        effect::Input in;
        in.allow = true, in.speed_kmh = 60.f, in.trailer = true;
        blow(in);
        effect::Output o = run(in, 0.3);
        check(o.brake > 0.14f, "trailer tire: brake tap at the blowout");
        o = run(in, 2.0);
        check(o.steering == 0.f && o.trailer_brake > 0.29f && o.brake == 0.f && o.throttle < -0.09f,
              "then trailer brake + less throttle, no pull, no brake pedal (no lights)");
        s = effect::Shaper{};
        in = effect::Input{};
        in.allow = true, in.speed_kmh = 60.f, in.truck = true, in.truck_steered = true, in.truck_left = true;
        blow(in);
        o = run(in, 0.2);
        check(o.steering < -ep.pull * 1.5f && o.brake > 0.14f, "front-left tire: kick to the left + brake tap");
        o = run(in, 2.0);
        check(std::fabs(o.steering + ep.pull) < ep.wobble + 1e-3f && o.brake == 0.f && o.throttle < -0.19f && o.trailer_brake == 0.f,
              "then a steady pull left + less throttle, no brake (solo truck)");
        in.trailer_attached = true;
        o = run(in, 1.0);
        check(o.trailer_brake > 0.14f && o.brake == 0.f, "with a trailer attached: also the trailer brake");
        in.trailer_attached = false;
        in.truck_left = false, in.truck_steered = false;
        o = run(in, 2.0);
        check(std::fabs(o.steering - ep.pull * ep.pull_rear) < ep.wobble + 1e-3f, "rear-right tire: weaker pull right");
        // E3: the steering shakes around the pull while driving.
        float lo = 1.f, hi = -1.f;
        for (int i = 0; i < 100; ++i) {
            o = s.Update(in, 0.01);
            lo = std::min(lo, o.steering), hi = std::max(hi, o.steering);
        }
        check(hi - lo > ep.wobble && hi - lo < 2.5f * ep.wobble, "the steering shakes (wobble) around the pull");
        // E3b: speed limit, 60 km/h with a truck tire: governor first, brake last.
        in.speed_kmh = 50.f;
        o = run(in, 1.0);
        check(o.brake == 0.f && std::fabs(o.throttle + ep.throttle_cut) < 1e-3f, "well under the limit: only the usual throttle cut");
        in.speed_kmh = 57.5f;
        o = run(in, 1.0);
        check(o.brake == 0.f && std::fabs(o.throttle + 0.5f) < 1e-3f, "2.5 km/h under the limit: governor takes half the throttle");
        in.speed_kmh = 64.f;
        o = run(in, 1.0);
        check(o.brake == 0.f && o.throttle < -0.99f, "a little over the limit: no throttle at all, still no brake (no lights)");
        in.speed_kmh = 73.f;
        o = run(in, 1.0);
        check(std::fabs(o.brake - ep.limit_brake * 0.5f) < 1e-3f, "13 km/h over (keyboard): last-resort brake, half");
        effect::Input tr;
        tr.allow = true, tr.trailer = true, tr.speed_kmh = 65.f;
        effect::Shaper s2;
        o = s2.Update(tr, 0.01);
        for (int i = 0; i < 200; ++i) o = s2.Update(tr, 0.01);
        check(o.brake == 0.f && o.steering == 0.f, "only a trailer tire: 65 km/h is under its 70 km/h limit, no shake");
        in.speed_kmh = 3.f;
        o = run(in, 2.0);
        check(o.throttle == 0.f && std::fabs(o.steering) < ep.pull * ep.pull_rear * 0.15f, "standing: no drag, almost no pull");
        s = effect::Shaper{};
        in.speed_kmh = 60.f;
        o = run(in, 0.5);
        check(o.brake == 0.f && o.steering != 0.f, "blowout loaded from the save: pull, but no kick / brake tap");
        in.allow = false;
        o = s.Update(in, 0.01);
        check(o.steering == 0.f && o.throttle == 0.f && o.brake == 0.f && o.trailer_brake == 0.f,
              "paused / multiplayer / Ctrl+Shift+B: lets go at once");
        in.allow = true, in.truck = false;
        o = run(in, 1.0);
        check(o.steering == 0.f && o.throttle == 0.f && o.brake == 0.f, "repaired: back to nothing");
        blowout::Engine e3(3);
        e3.SetParams(p);
        e3.Force(truck, 0.f, 6, 0, true);
        check(e3.Get(truck).active && e3.Get(truck).wheel == 0 && e3.Save(file), "test blowout of wheel 0 is active");
        blowout::Engine e4(4);
        check(e4.Load(file) && !e4.Get(truck).active, "but is not saved (this session only)");
        _wremove(file.c_str());
        printf("    %s\n", failed ? "SOME CHECKS FAILED" : "all checks passed");
        return failed ? 1 : 0;
    }
}
