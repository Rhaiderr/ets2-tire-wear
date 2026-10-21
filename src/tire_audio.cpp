#include "tire_audio.h"

#ifndef NOMINMAX
#define NOMINMAX  // the tools build this file without the project's definitions
#endif
#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>

namespace tire_audio {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr float kFlapRps = 5.f;  // turns per second where the flapping is at full loudness (~57 km/h)

void Put16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(v & 0xff), b.push_back(v >> 8); }
void Put32(std::vector<uint8_t>& b, uint32_t v) { Put16(b, v & 0xffff), Put16(b, v >> 16); }

}  // namespace

// ---- the bang ----

std::vector<float> BangSamples() {
    const int n = static_cast<int>(kRate * 0.9);
    std::vector<float> s(n);
    uint32_t rng = 0x9e3779b9u;
    auto noise = [&] {
        rng ^= rng << 13, rng ^= rng >> 17, rng ^= rng << 5;
        return static_cast<float>(rng) / 2147483648.f - 1.f;
    };
    float low = 0.f, hiss_lp = 0.f, prev = 0.f;
    double phase = 0.0;
    for (int i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kRate;
        const float white = noise();
        // Crack: bright noise, 1 ms attack, ~25 ms decay (white minus its previous sample = high-passed).
        const float attack = static_cast<float>(std::min(1.0, t / 0.001));
        const float crack = (white - prev) * 0.5f * attack * static_cast<float>(std::exp(-t / 0.025));
        prev = white;
        // Boom: a sine falling from 110 to 45 Hz, ~140 ms decay, plus low-passed noise for body.
        const double f = 45.0 + 65.0 * std::exp(-t / 0.06);
        phase += 2.0 * kPi * f / kRate;
        low += 0.02f * (white - low);
        const float boom = (0.8f * static_cast<float>(std::sin(phase)) + 2.5f * low) * attack *
                           static_cast<float>(std::exp(-t / 0.14));
        // Hiss: the air going out, swelling in after the bang and fading.
        hiss_lp += 0.25f * (white - hiss_lp);
        const float hiss = (white - hiss_lp) * 0.12f * static_cast<float>(std::min(1.0, t / 0.05)) *
                           static_cast<float>(std::exp(-t / 0.3));
        s[i] = crack + boom + hiss;
    }
    // Normalise, with a soft clip so the crack stays punchy.
    float peak = 0.f;
    for (float v : s) peak = std::max(peak, std::fabs(v));
    const float gain = peak > 0.f ? 1.f / peak : 1.f;
    for (float& v : s) v = std::tanh(1.5f * v * gain) / std::tanh(1.5f);
    return s;
}

std::vector<uint8_t> MakeWav(const std::vector<float>& samples) {
    const uint32_t n = static_cast<uint32_t>(samples.size());
    std::vector<uint8_t> wav;
    wav.reserve(44 + n * 2);
    const char* riff = "RIFF";
    wav.insert(wav.end(), riff, riff + 4);
    Put32(wav, 36 + n * 2);
    const char* head = "WAVEfmt ";
    wav.insert(wav.end(), head, head + 8);
    Put32(wav, 16), Put16(wav, 1), Put16(wav, 1), Put32(wav, kRate), Put32(wav, kRate * 2), Put16(wav, 2),
        Put16(wav, 16);
    const char* data = "data";
    wav.insert(wav.end(), data, data + 4);
    Put32(wav, n * 2);
    for (float v : samples)
        Put16(wav, static_cast<uint16_t>(static_cast<int16_t>(std::lround(std::clamp(v, -1.f, 1.f) * 32000.f))));
    return wav;
}

// ---- the synth ----

float Synth::NextNoise() {
    rng_ ^= rng_ << 13, rng_ ^= rng_ >> 17, rng_ ^= rng_ << 5;
    return static_cast<float>(rng_) / 2147483648.f - 1.f;
}

void Synth::Bang(float volume) {
    static const std::vector<float> bang = BangSamples();
    bang_ = bang;
    bang_pos_ = 0;
    bang_amp_ = std::clamp(volume, 0.f, 1.f);
}

void Synth::SetWheel(bool active, double rotation, double rps, float volume) {
    if (active && (!active_ || !locked_)) {
        // (Re)start in step with the wheel.
        phase_ = rotation;
        rps_ = rps;
        locked_ = true;
    }
    active_ = active;
    target_rot_ = rotation;
    target_rps_ = rps;
    since_update_ = 0;
    volume_ = std::clamp(volume, 0.f, 1.f);
}

void Synth::Flap(float amp) {
    if (amp <= 0.001f) return;
    Voice& v = voices_[next_voice_];
    next_voice_ = (next_voice_ + 1) % 3;
    v.on = true, v.t = 0.0, v.amp = amp;
}

void Synth::Render(float* out, int n) {
    for (int i = 0; i < n; ++i) {
        float x = 0.f;
        if (bang_pos_ < bang_.size()) x += bang_[bang_pos_++] * bang_amp_;

        const float white = NextNoise();
        rumble_lp_ += 0.017f * (white - rumble_lp_);  // ~120 Hz
        rumble_lp2_ += 0.017f * (rumble_lp_ - rumble_lp2_);
        thump_lp_ += 0.05f * (white - thump_lp_);      // ~350 Hz
        const float bright = white - prev_white_;      // high-passed
        prev_white_ = white;

        if (locked_) {
            ++since_update_;
            // Where the game's wheel is now, extrapolated from its last update;
            // the synth's phase follows its speed and is pulled towards it.
            const double target = target_rot_ + target_rps_ * static_cast<double>(since_update_) / kRate;
            rps_ += (target_rps_ - rps_) * 0.001;
            const double prev = phase_;
            phase_ += rps_ / kRate;
            double err = target - phase_;
            err -= std::floor(err + 0.5);  // nearest turn
            phase_ += err * 0.002;         // ~10 ms pull
            // The torn piece slaps once per turn; a loose flap at half a turn, softer.
            const float level_target = active_ ? std::clamp(static_cast<float>(std::fabs(rps_)) / kFlapRps, 0.f, 1.f) : 0.f;
            level_ += (level_target - level_) * 0.0005f;  // ~45 ms
            if (std::floor(phase_) != std::floor(prev)) Flap(level_ * volume_);
            if (std::floor(phase_ + 0.5) != std::floor(prev + 0.5)) Flap(0.35f * level_ * volume_);
            // Rumble of the flat tire on the road, louder where the tear touches it.
            const float near_tear = 1.f + 0.6f * static_cast<float>(std::cos(2.0 * kPi * phase_));
            x += rumble_lp2_ * 6.f * 0.3f * level_ * volume_ * near_tear;
            if (!active_ && level_ < 1e-4f) locked_ = false;
        }

        for (Voice& v : voices_) {
            if (!v.on) continue;
            const double t = v.t;
            const float thump = static_cast<float>(std::sin(2.0 * kPi * 70.0 * t) * std::exp(-t / 0.03)) +
                                3.f * thump_lp_ * static_cast<float>(std::exp(-t / 0.015));
            const float slap = 0.4f * bright * static_cast<float>(std::exp(-t / 0.006));
            x += v.amp * (thump + slap);
            v.t += 1.0 / kRate;
            if (v.t > 0.2) v.on = false;
        }
        out[i] = std::clamp(x, -1.f, 1.f);
    }
}

// ---- the audio device (game) ----

namespace {

constexpr int kBuffers = 4;
constexpr int kBufferSamples = 768;
constexpr double kLatency = static_cast<double>(kBuffers * kBufferSamples) / kRate;  // ~70 ms

struct Device {
    std::mutex start_m;
    bool started = false, failed = false;
    std::mutex synth_m;
    Synth synth;
    std::thread thread;
    std::atomic<bool> run{false};
    HWAVEOUT out = nullptr;
    HANDLE event = nullptr;
    WAVEHDR hdr[kBuffers] = {};
    int16_t buf[kBuffers][kBufferSamples] = {};
};

Device& Dev() {
    static Device d;
    return d;
}

void Fill(Device& d, int b) {
    float tmp[kBufferSamples];
    {
        std::lock_guard<std::mutex> lock(d.synth_m);
        d.synth.Render(tmp, kBufferSamples);
    }
    for (int i = 0; i < kBufferSamples; ++i) d.buf[b][i] = static_cast<int16_t>(std::lround(tmp[i] * 32000.f));
    waveOutWrite(d.out, &d.hdr[b], sizeof(WAVEHDR));
}

void Loop(Device* d) {
    for (int b = 0; b < kBuffers; ++b) Fill(*d, b);
    while (d->run) {
        WaitForSingleObject(d->event, 20);
        for (int b = 0; b < kBuffers && d->run; ++b)
            if (d->hdr[b].dwFlags & WHDR_DONE) Fill(*d, b);
    }
}

bool EnsureStarted() {
    Device& d = Dev();
    std::lock_guard<std::mutex> lock(d.start_m);
    if (d.started || d.failed) return d.started;
    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 1;
    fmt.nSamplesPerSec = kRate;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 2;
    fmt.nAvgBytesPerSec = kRate * 2;
    d.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!d.event || waveOutOpen(&d.out, WAVE_MAPPER, &fmt, reinterpret_cast<DWORD_PTR>(d.event), 0, CALLBACK_EVENT) !=
                        MMSYSERR_NOERROR) {
        if (d.event) CloseHandle(d.event), d.event = nullptr;
        d.failed = true;
        return false;
    }
    for (int b = 0; b < kBuffers; ++b) {
        d.hdr[b].lpData = reinterpret_cast<LPSTR>(d.buf[b]);
        d.hdr[b].dwBufferLength = sizeof(d.buf[b]);
        waveOutPrepareHeader(d.out, &d.hdr[b], sizeof(WAVEHDR));
    }
    d.run = true;
    d.thread = std::thread(Loop, &d);
    d.started = true;
    return true;
}

}  // namespace

void Bang(float volume) {
    if (!EnsureStarted()) return;
    Device& d = Dev();
    std::lock_guard<std::mutex> lock(d.synth_m);
    d.synth.Bang(volume);
}

void SetWheel(bool active, double rotation, double rps, float volume) {
    Device& d = Dev();
    if (!d.started && (!active || !EnsureStarted())) return;  // no device until a tire is blown
    std::lock_guard<std::mutex> lock(d.synth_m);
    // What we hear is ~kLatency behind what we render: aim at where the wheel will be then.
    d.synth.SetWheel(active, rotation + rps * kLatency, rps, volume);
}

void Stop() {
    Device& d = Dev();
    std::lock_guard<std::mutex> lock(d.start_m);
    if (!d.started) return;
    d.run = false;
    SetEvent(d.event);
    if (d.thread.joinable()) d.thread.join();
    waveOutReset(d.out);
    for (int b = 0; b < kBuffers; ++b) waveOutUnprepareHeader(d.out, &d.hdr[b], sizeof(WAVEHDR));
    waveOutClose(d.out);
    CloseHandle(d.event);
    d.out = nullptr, d.event = nullptr;
    d.started = false;
}

}  // namespace tire_audio
