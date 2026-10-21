#include "effect.h"

#include <algorithm>
#include <cmath>

namespace effect {
namespace {

float Approach(float cur, float target, float max_step) {
    if (cur < target) return std::min(target, cur + max_step);
    return std::max(target, cur - max_step);
}

float Clamp01(float x) { return std::clamp(x, 0.f, 1.f); }

}  // namespace

Output Shaper::Update(const Input& in, double dt) {
    dt = std::clamp(dt, 0.0, 0.25);
    // Only a blowout happening now kicks; one loaded from the save does not.
    if (in.truck_blew) since_truck_ = 0.0;
    if (in.truck_blew || in.trailer_blew) since_burst_ = 0.0;
    since_truck_ += dt;
    since_burst_ += dt;
    if (!in.allow || !p_.enabled) {
        // Pause, multiplayer or the stop key: let go at once.
        out_ = Output{};
        return out_;
    }

    const float speed = std::fabs(in.speed_kmh);
    const float pull_factor = p_.full_speed_kmh > 0.f ? Clamp01(speed / p_.full_speed_kmh) : 1.f;
    // Drag fades in over 5 km/h above min_speed_kmh, so it never stops the truck dead.
    const float drag_factor = Clamp01((speed - p_.min_speed_kmh) / 5.f);

    Output target;
    if (in.truck) {
        float pull = p_.pull * (in.truck_steered ? 1.f : p_.pull_rear);
        if (since_truck_ < p_.kick_s) pull *= p_.kick;
        // Positive semantical steering turns right [verified: E2 test, sent
        // -0.087 and truck.input.steering read +0.087, counterclockwise]; a
        // left tire pulls left.
        float side = in.truck_left ? -1.f : 1.f;
        if (p_.steer_invert) side = -side;
        target.steering = std::clamp(side * pull * pull_factor, -1.f, 1.f);
        target.throttle = -Clamp01(p_.throttle_cut) * drag_factor;
        if (in.trailer_attached) target.trailer_brake = Clamp01(p_.truck_trailer_hold) * drag_factor;
    }
    if (in.trailer) {
        target.trailer_brake = std::max(target.trailer_brake, Clamp01(p_.trailer_hold) * drag_factor);
        target.throttle = std::min(target.throttle, -Clamp01(p_.trailer_throttle_cut) * drag_factor);
        target.brake = Clamp01(p_.trailer_pedal) * drag_factor;
    }
    if ((in.truck || in.trailer) && since_burst_ < p_.kick_brake_s)
        target.brake = std::max(target.brake, Clamp01(p_.kick_brake) * drag_factor);

    // A blowout is sudden: steering reaches the kick within ~0.05 s; brakes,
    // throttle and the release are a little softer.
    const float f = static_cast<float>(dt);
    out_.steering = Approach(out_.steering, target.steering, 6.f * f);
    out_.throttle = Approach(out_.throttle, target.throttle, 2.f * f);
    out_.brake = Approach(out_.brake, target.brake, 2.f * f);
    out_.trailer_brake = Approach(out_.trailer_brake, target.trailer_brake, 2.f * f);
    return out_;
}

}  // namespace effect
