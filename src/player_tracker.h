// Step R4b: tells the player's tire draws apart from traffic.
//
// Each tire draw carries its wheel's position relative to the camera
// [verified: R4a]. The player's units (truck, trailers) are rigid sets of
// wheels whose positions in the unit are known from the telemetry config, so
// their pairwise distances are known too. Per camera (viewport, see ViewportKey):
//  - tracking: a draw within kTrackRadius of a wheel position seen in the
//    previous frame belongs to that unit (the camera moves little relative to
//    the vehicle between two frames);
//  - search: when a unit is not tracked, the frame's unmatched draws are
//    searched at frame end for a subset whose pairwise distances match the
//    unit's wheel layout (>= kMinWheels wheels). Until then nothing is
//    claimed, so a miss leaves the original tire (safe). The search also
//    runs when tracking finds fewer wheels than the unit had (R4b3);
//  - completion: an unclaimed draw whose distances to >= kMinWheels claimed
//    wheels fit one of the unit's missing wheels joins the unit (R4b3);
//  - velocity (R4c): tracking expects each wheel moved as in the last frame;
//  - pose prediction (R4c): a unit seen with >= kMinWheels wheels gives its
//    pose in the camera; with the telemetry world placements that predicts
//    the other units' wheels (the truck in a mirror showing the trailer);
//  - pair rule (R4c, R4c2): in a mirror camera, two draws near the camera at
//    the distance of two wheels of a unit not seen there, steady for a few
//    frames, are that unit's: an axle of the truck within 3.5 m (front wheels
//    in the small mirror), or two wheels on one side within 6 m whose axles
//    are square to the line between them (the side mirrors).
// Pure geometry, no D3D: also used offline on the captures.
#pragma once

#include <string>

namespace tracker {

constexpr int kMaxUnits = 11;   // 0 = truck, 1 + i = trailer i
constexpr int kMaxWheels = 16;

struct Unit {
    int wheel_count = 0;  // 0 = unit absent (no config / trailer not connected)
    float wheel[kMaxWheels][3] = {};
    // R4c: world placement (telemetry), angles in turns.
    bool placed = false;
    double pos[3] = {};
    double heading = 0, pitch = 0, roll = 0;
};

// Game thread: current units; tracks are reset when the wheel layouts change.
void SetUnits(const Unit* units, int count);

// Camera key from a viewport (x, y, width, height): render targets change
// every frame [verified: R4b-diag], the viewport does not.
// run: which run of that viewport in the frame (R4c2: two mirrors share one).
const void* ViewportKey(const float vp[4], int run = 0);

// Render thread, per tire draw: the wheel position relative to the camera.
// Returns the unit index, or -1 when the draw is not (yet) known as the player's.
// obs_index (diagnostics): the draw's index among this camera's draws this frame.
// viewport_area: width x height, tells mirrors from the main camera (pair rule).
// axle: the wheel's axle direction in the camera (column 0 of its matrix), or null.
int Classify(const void* camera_key, const float pos[3], int* obs_index = nullptr, float viewport_area = 0,
             const float* axle = nullptr);

// Render thread: area of the largest viewport drawn this frame (the main
// camera, with or without tires in it); mirrors are much smaller (R4c2).
void SetMainViewportArea(float area);

// Render thread, once per frame: update tracks and search for lost units.
// debug (diagnostics): receives one text block per camera with draws.
void EndFrame(std::string* debug = nullptr);

struct Stats {
    long long claimed = 0, unclaimed = 0, searches = 0, found = 0, completed = 0, predicted = 0, paired = 0;
};
Stats GetStats();

}  // namespace tracker
