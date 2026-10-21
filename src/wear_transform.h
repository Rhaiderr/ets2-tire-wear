// Step R5: wear transform applied directly to block-compressed tire textures.
//
// Shared by the plugin (on the game's own upload) and tools\wearpreview (on
// the extracted .dds files), so the preview shows exactly what the game gets.
// Only blocks inside the tread rectangle change; the sidewall and its
// lettering are left alone. Every operation is affine per channel, applied to
// the block's palette, so no image-wide recompression is needed.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wear {

// Tread area of the texture, in UV (0..1, origin top-left).
struct Rect {
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
};

// Defaults: 100% is a practically smooth tread (user, 2026-10-19); the old
// "nearly bald" look now sits around 86%.
// R5c look, from photos of bald tires [verified: Wikimedia Commons "bald
// tire"]: the worn tread is a matte, dusty mid-dark grey (not glossy black),
// keeps a faint ghost of the pattern (grooves risen towards the top surface
// as they get shallower), and shows fine scuffs running around
// the tire.
struct Params {
    float relief_left = 0.02f;   // fraction of the groove relief left at 100% wear
    // R5c (user, 2026-10-20): the exposed top rubber keeps its tone and the
    // darker grooves rise towards it as they get shallower (not a grey wash
    // over everything). Grooves = below the Otsu threshold of the tread's
    // brightness; "top" = the median above it. Second pass (same day): the
    // whole tire brightened too much (top +34% at 100%), so the top now
    // only gets the dust.
    float groove_fill = 0.9f;    // at 100%: how far groove brightness rises to the top surface's
    float top_contrast = 0.15f;  // at 100%: contrast lost within the top surface (fine detail fades a bit)
    float color_mix = 0.f;       // at 100%: pull of everything towards the mean (R5b look; off in R5c)
    float desaturate = 0.9f;     // at 100%: how far colour moves to neutral grey (R5c: was the brown mean)
    float lift = 0.005f;         // at 100%: tread brightness added, x 255 (dust, abraded rubber); was 0.03
    float streak = 0.12f;        // at 100%: brightness swing of the scuffs running around the tire
    float streak_relief = 6.f;   // at 100%: scuff ridges in the normal map (BC5 units)
    // R5c in game (user, 2026-10-20): with the relief flattened every texel
    // catches the light alike, so the specular mask showed as dark blotches
    // (worst on mips 1+) and the sheen read as a light grey: uniform and
    // nearly matte now.
    float spec_flatten = 1.f;    // at 100%: how far tread specular moves to its mean (was 0.7)
    float spec_gain = -0.75f;    // at 100%: specular x (1 + gain); was -0.35 (R5b: +0.3)
    float darken = 0.12f;        // at 100%: tread brightness x (1 - darken) (worn rubber is charcoal, not light grey)
    float feather_u = 0.015f;    // UV width of the fade at the shoulders (left/right edges); R5c: was 0.03
    float feather_v = 0.f;       // top/bottom edges are seams of the repeated tile: no fade (R5c)

    // R5d: per-pixel detail (user, 2026-10-20: "fotorrealismo por pixel").
    // Everything along v repeats exactly once per tread tile, so the tile
    // seam stays invisible; detail finer than a pixel fades out on small mips.
    float grain = 0.14f;          // 1: abrasion grain, brightness swing at 100% (fine streaks around the tire)
    float grain_relief = 16.f;    //    and in the normal map (BC5 units)
    float width_profile = 0.25f;  // 2: uneven wear across the width: centre ahead of the shoulders [estimativa]
    float rib_vary = 0.06f;       //    and a little from rib to rib
    float dirt = 0.6f;            // 3: light grey-brown dust in the remaining grooves, strongest at mid wear
    float twi_from = 0.7f;        // 4: tread wear indicator bars show in the main grooves from this wear…
    float twi_full = 0.85f;       //    …in full from this one
    float chips = 18.f;           // 5: cuts / chips per tile at 100% (dark, small, sparse)
    float chip_dark = 0.45f;      //    how much darker a chip's core is
    float chip_rim = 0.18f;       //    how much lighter its torn rim is
    float chip_relief = 22.f;     //    dent in the normal map (BC5 units)
    float cup = 0.06f;            // 6: cupping: slow brightness wave around the tire, at 100%, more on the shoulders
    float cup_relief = 6.f;       //    and in the normal map (BC5 units)

    // R5c2 look (no per-pixel detail), for before/after previews.
    static Params R5c2() {
        Params p;
        p.grain = 0.f, p.grain_relief = 0.f, p.width_profile = 0.f, p.rib_vary = 0.f, p.dirt = 0.f;
        p.twi_from = 2.f, p.twi_full = 3.f, p.chips = 0.f, p.cup = 0.f, p.cup_relief = 0.f;
        return p;
    }
    // The R5b look, for before/after previews.
    static Params R5b() {
        Params p = R5c2();
        p.color_mix = 0.95f, p.groove_fill = 0.f, p.top_contrast = 0.f;
        p.desaturate = 0.f, p.lift = 0.f, p.streak = 0.f, p.streak_relief = 0.f;
        p.spec_gain = 0.3f, p.feather_v = 0.03f, p.darken = 0.f, p.spec_flatten = 0.7f;
        return p;
    }
};

// Wear levels: level i of n covers wear i/(n-1). 8 levels = 0, 14, 29, 43,
// 57, 71, 86, 100 % (user, 2026-10-19: one extra almost-smooth level and two
// more intermediate ones over the first proposal of 5).
constexpr int kDefaultLevels = 8;
inline float LevelWear(int level, int levels) { return levels > 1 ? float(level) / float(levels - 1) : 0.f; }

// DXGI formats handled (numbers, so this file does not need the D3D headers).
constexpr uint32_t kBc3Unorm = 77, kBc3Srgb = 78, kBc5Unorm = 83;
inline bool IsNormal(uint32_t f) { return f == kBc5Unorm || f == 82; }
inline bool IsColor(uint32_t f) { return f == kBc3Unorm || f == kBc3Srgb || f == 76; }

// Transforms one mip (tightly packed rows of 16-byte blocks) in place.
// width/height are the mip's pixel size; wear is 0..1.
void Apply(uint32_t format, uint8_t* blocks, uint32_t width, uint32_t height, const Rect& tread, float wear,
           const Params& p);

// Brightness (0..255 luma) splitting the tread into grooves (darker) and the
// exposed top surface: Otsu threshold of the sample.
float GrooveThreshold(const float* luma, size_t n);

// What the colour transform measures on one BC3 mip's tread (from the block
// endpoints). Exposed for previews.
struct ColorAnalysis {
    int blocks = 0;               // blocks fully inside the tread (0: nothing to do)
    float mean_rgb[3] = {}, mean_a = 0;
    float fence = 255;            // brightness above which an endpoint is an outlier (stripe, glint)
    float groove_t = 0;           // grooves below, top surface above (outliers left out)
    float top_l = 0;              // the top surface's brightness (median between groove_t and fence)
    int stripe_columns = 0;       // block columns holding a new-tire stripe
};
// stripe (optional) gets, per block column, the brightness above which a
// colour is the new-tire line, or kNoStripe.
constexpr float kNoStripe = 1e9f;
ColorAnalysis AnalyzeColor(uint32_t format, const uint8_t* blocks, uint32_t width, uint32_t height, const Rect& tread,
                           const Params& p, std::vector<float>* stripe = nullptr);

// Stage 3, E4b: a blown, shredded tire. Applied on top of the wear (the
// tread tile repeats around the tire, so the damage repeats with it, as on a
// tire that shed its tread all round). Patches of tread are torn off down to
// the carcass, whose steel belt cords show as fine slanted lines; the torn
// rubber edges are ragged and lighter.
struct BurstParams {
    float torn = 0.45f;          // share of the tread torn off
    float edge = 0.06f;          // softness of the torn edge (noise units)
    float rim_light = 0.35f;     // torn rubber edge: brightness x (1 + rim_light)
    float carcass_l = 22.f;      // exposed carcass brightness (0..255), dark brown-grey
    float cord_l = 70.f;         // steel cord brightness (0..255)
    float cords = 45.f;          // cords across the tread width
    float cord_angle = 0.35f;    // radians the cords lean from the circumference [estimativa: belts ~20 deg]
    float relief = 110.f;        // step at the torn edge in the normal map (BC5 units)
    float cord_relief = 20.f;    // cord ridges in the normal map (BC5 units)
    float spec_carcass = 30.f;   // carcass specular (0..255), matte
    float spec_cord = 150.f;     // steel cord specular (0..255)

    // A gash in the sidewall, drawn once on the sidewall disc around the
    // texture's centre [verified: SCS tire textures, sidewall ring centred
    // at UV 0.5, 0.5]. Where and how big is drawn at the blowout.
    bool side = true;
    float side_angle = 0.f;      // where around the sidewall (turns, 0..1)
    float side_size = 0.5f;      // 0 = small slit (~15 deg), 1 = big gash (~75 deg)
    float side_r = 0.40f;        // radius of its centre line (UV) [estimativa: ring ~0.30..0.49]
    float side_ring_in = 0.31f;  // keep inside the sidewall ring (UV radii) [estimativa]
    float side_ring_out = 0.47f;
};
void ApplyBurst(uint32_t format, uint8_t* blocks, uint32_t width, uint32_t height, const Rect& tread,
                const BurstParams& p = BurstParams());

// Low level, exposed for tests: v' = a*v + b on a BC4 block (re-encoded).
void AffineBc4(uint8_t* block8, float a, float b);

}  // namespace wear
