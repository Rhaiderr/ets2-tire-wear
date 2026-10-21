#pragma once

// Stage 3, E4a: the sounds of a blown tire, synthesised (no third-party
// audio): the bang when it blows, then the torn tire slapping the road once
// per wheel turn, locked to the wheel's rotation from the telemetry.

#include <cstdint>
#include <vector>

namespace tire_audio {

constexpr int kRate = 44100;

// Mixes the bang and the flapping tire. The same code plays in the game and
// renders the preview (tools/tiresound).
class Synth {
public:
    void Bang(float volume);
    // The blown wheel as the game sees it now: rotation in turns (any range,
    // only the fraction counts), angular velocity in turns per second.
    // active = false fades the flapping out.
    void SetWheel(bool active, double rotation, double rps, float volume);
    void Render(float* out, int n);

private:
    struct Voice {
        bool on = false;
        double t = 0.0;
        float amp = 0.f;
    };
    void Flap(float amp);

    // Bang
    std::vector<float> bang_;
    size_t bang_pos_ = 0;
    float bang_amp_ = 0.f;
    // Wheel: the game's rotation and the synth's own phase, pulled towards it.
    bool active_ = false;
    double target_rot_ = 0.0, target_rps_ = 0.0;
    long long since_update_ = 0;  // samples since SetWheel
    bool locked_ = false;
    double phase_ = 0.0, rps_ = 0.0;
    float volume_ = 0.f, level_ = 0.f;
    Voice voices_[3];
    int next_voice_ = 0;
    // Noise
    uint32_t rng_ = 0x2545f491u;
    float rumble_lp_ = 0.f, rumble_lp2_ = 0.f, thump_lp_ = 0.f, prev_white_ = 0.f;
    float NextNoise();
};

// The bang alone (normalised, -1 ... 1).
std::vector<float> BangSamples();
// A WAV file (PCM 16-bit mono) from samples.
std::vector<uint8_t> MakeWav(const std::vector<float>& samples);

// In the game: an audio device and its thread, started on first use.
void Bang(float volume);
void SetWheel(bool active, double rotation, double rps, float volume);
void Stop();

}  // namespace tire_audio
