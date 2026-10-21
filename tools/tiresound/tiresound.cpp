// tiresound: renders the blown-tire sounds of src/tire_audio.cpp to a WAV
// file, to listen to them outside the game: the bang at 45 km/h, the torn
// tire speeding up to 60 km/h, holding, then slowing down to a stop. The
// wheel is fed to the synth like the game does (60 updates a second, the
// rotation as a fraction of a turn).
//
// Usage: tiresound [out.wav] [bang_volume] [flap_volume]   (defaults: tire.wav 0.8 0.6)
// Build: cl /nologo /O2 /EHsc /W4 /std:c++17 /D_CRT_SECURE_NO_WARNINGS tiresound.cpp ..\..\src\tire_audio.cpp winmm.lib

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "../../src/tire_audio.h"

int main(int argc, char** argv) {
    const char* out = argc > 1 ? argv[1] : "tire.wav";
    const float bang = argc > 2 ? static_cast<float>(atof(argv[2])) : 0.8f;
    const float flap = argc > 3 ? static_cast<float>(atof(argv[3])) : 0.6f;
    const double kTurnM = 3.2;  // ~1 m tire [estimate]
    auto speed_kmh = [](double t) {
        if (t < 1.0) return 45.0;
        if (t < 5.0) return 45.0 + 15.0 * (t - 1.0) / 4.0;
        if (t < 8.0) return 60.0;
        if (t < 11.0) return 60.0 * (11.0 - t) / 3.0;
        return 0.0;
    };
    tire_audio::Synth synth;
    synth.Bang(bang);
    const int frame = tire_audio::kRate / 60;
    const double seconds = 12.0;
    std::vector<float> samples(static_cast<size_t>(seconds * tire_audio::kRate));
    double rotation = 0.0;
    for (size_t i = 0; i < samples.size(); i += frame) {
        const double t = static_cast<double>(i) / tire_audio::kRate;
        const double rps = speed_kmh(t) / 3.6 / kTurnM;
        synth.SetWheel(true, rotation - std::floor(rotation), rps, flap);
        const int n = static_cast<int>(std::min<size_t>(frame, samples.size() - i));
        synth.Render(&samples[i], n);
        rotation += rps * n / tire_audio::kRate;
    }
    const auto wav = tire_audio::MakeWav(samples);
    FILE* f = fopen(out, "wb");
    if (!f) return 1;
    fwrite(wav.data(), 1, wav.size(), f);
    fclose(f);
    printf("%s: %.0f s, bang %.2f, flap %.2f, %zu bytes\n", out, seconds, bang, flap, wav.size());
    return 0;
}
