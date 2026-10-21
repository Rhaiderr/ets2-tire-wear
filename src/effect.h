#pragma once

// Stage 3, E2: what a blown tire does to the truck, as extra driver input fed
// to the game through the SCS Input API (semantical device). The game adds
// these to the player's own wheel and pedals [verified: controls.sii mixes
// "steering = wheel - semantical.steering", "aforward = pedal +
// semantical.aforward", "abackward = pedal + semantical.abackward",
// "trailerbrake = semantical.trailerbrake"].
//
// Truck tire: pull towards the blown side (strong on a steered axle). Any
// blown tire: a short brake tap at the blowout, then drag by taking throttle
// away (no brake lights) [verified: E2 test, brake pedal input lights them].
// A trailer tire also pulls on the trailer brake, never on the steering.

namespace effect {

struct Params {
    bool enabled = true;
    float pull = 0.12f;          // steering offset for a steered-axle tire (1 = full lock)
    float pull_rear = 0.4f;      // other truck axles: this fraction of pull
    float kick = 2.f;            // pull multiplier right after the blowout ...
    float kick_s = 0.4f;         // ... for this many seconds
    float kick_brake = 0.15f;    // brake pedal tap at the blowout (brake lights only then) ...
    float kick_brake_s = 1.f;    // ... for this many seconds
    float throttle_cut = 0.2f;   // throttle taken away while a truck tire is blown
    float trailer_throttle_cut = 0.1f;  // ... while only a trailer tire is blown
    // Trailer brake for a trailer tire / for a truck tire with a trailer
    // attached. 0.35 holds without lighting the brake lamps [verified: E2b
    // test, light.brake stayed off; a full manual trailer brake lights them].
    float trailer_hold = 0.35f;
    float truck_trailer_hold = 0.35f;
    float trailer_pedal = 0.f;   // plus this much brake pedal all the time (lights on)
    // E3: speed limit while a tire is blown. A throttle governor takes the
    // throttle away from governor_kmh under the limit (all of it at the
    // limit); only limit_brake_after_kmh over the limit the service brake
    // (lights on) grows to limit_brake over limit_band_kmh.
    float limit_truck_kmh = 60.f;    // a truck tire is blown
    float limit_trailer_kmh = 70.f;  // only a trailer tire is blown
    float governor_kmh = 5.f;
    float limit_brake_after_kmh = 8.f;
    float limit_band_kmh = 10.f;
    float limit_brake = 0.3f;
    // F1: the limit brake only starts once the speed has not been falling
    // faster than limit_brake_falling (km/h per second, ~0.5 s average) for
    // limit_brake_wait_s (counted from the blowout), so the governor gets to
    // slow the truck first; once on it holds until back under the limit +
    // limit_brake_after_kmh, so a downhill does not flash the lights.
    float limit_brake_falling = 0.5f;
    float limit_brake_wait_s = 1.5f;
    // E3: a flapping truck tire shakes the steering: +-wobble, about once per
    // wheel turn (~5 Hz at 60 km/h [estimate: ~1 m tire]).
    float wobble = 0.015f;
    float min_speed_kmh = 5.f;   // no drag below this (keeps an automatic out of reverse)
    float full_speed_kmh = 40.f; // the pull grows from 0 (standing) to full at this speed
    bool steer_invert = false;   // flips the pull side if the game's sign turns out opposite
    // F2: with a force feedback wheel the pull is a force on the real wheel
    // instead of a steering offset (fractions of the wheel's full force).
    float ffb_pull = 0.25f;      // steady force for a steered-axle tire (other axles: x pull_rear)
    float ffb_kick = 0.6f;       // force right after the blowout, for kick_s
    float ffb_wobble = 0.08f;    // +- shake, about once per wheel turn
};

struct Input {
    bool allow = false;           // not paused, not multiplayer, not stopped by the player
    bool truck = false;           // a truck tire is blown ...
    bool truck_steered = false;   // ... on a steered axle
    bool truck_left = false;      // ... on the left side
    bool trailer = false;         // a trailer tire is blown
    bool truck_blew = false;      // the truck tire blew this frame (not a blowout loaded from the save)
    bool trailer_blew = false;    // a trailer tire blew this frame
    bool trailer_attached = false;  // there is a trailer brake to drag with
    bool wheel_ffb = false;       // F2: a force feedback wheel takes the pull as a force
    float speed_kmh = 0.f;        // signed, negative when reversing
};

struct Output {
    float steering = 0.f;      // semantical.steering (positive = right [verified: E2 test])
    float force = 0.f;         // F2: force on the wheel, -1 (left) ... 1 (right)
    float throttle = 0.f;      // semantical.aforward (negative = less throttle)
    float brake = 0.f;         // semantical.abackward
    float trailer_brake = 0.f; // semantical.trailerbrake
};

// Turns the blowout state into input values, frame by frame: a kick when a
// tire blows, then a steady pull and drag; quick ramps in and out.
class Shaper {
public:
    void SetParams(const Params& p) { p_ = p; }
    const Params& params() const { return p_; }
    Output Update(const Input& in, double dt);
    const Output& last() const { return out_; }

private:
    Params p_;
    Output out_;
    double since_truck_ = 1e9, since_burst_ = 1e9;  // seconds since the truck tire / any tire blew
    double wobble_phase_ = 0.0;                     // radians
    float last_speed_ = -1.f, speed_rate_ = 0.f;    // km/h (-1 = none yet), km/h per second (smoothed)
    double not_falling_s_ = 0.0;                    // since the speed stopped falling (or the blowout)
    bool limit_braking_ = false;
};

}  // namespace effect
