// Step R5: wear transform on BC3 / BC5 tire textures. See wear_transform.h.

#include "wear_transform.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace wear {
namespace {

int Clamp255(float v) { return v < 0.f ? 0 : v > 255.f ? 255 : static_cast<int>(v + 0.5f); }

void DecodeBc4(const uint8_t* b, int* v) {
    const int e0 = b[0], e1 = b[1];
    int pal[8] = {e0, e1};
    if (e0 > e1) {
        for (int i = 2; i < 8; ++i) pal[i] = ((8 - i) * e0 + (i - 1) * e1) / 7;
    } else {
        for (int i = 2; i < 6; ++i) pal[i] = ((6 - i) * e0 + (i - 1) * e1) / 5;
        pal[6] = 0;
        pal[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= uint64_t(b[2 + i]) << (8 * i);
    for (int p = 0; p < 16; ++p) v[p] = pal[(bits >> (3 * p)) & 7];
}

// 8-value mode encoding of 16 values (e0 = max, e1 = min).
void EncodeBc4(const int* v, uint8_t* b) {
    int mn = 255, mx = 0;
    for (int p = 0; p < 16; ++p) {
        mn = std::min(mn, v[p]);
        mx = std::max(mx, v[p]);
    }
    b[0] = static_cast<uint8_t>(mx);
    b[1] = static_cast<uint8_t>(mn);
    uint64_t out = 0;
    if (mx > mn) {
        int pal[8] = {mx, mn};
        for (int i = 2; i < 8; ++i) pal[i] = ((8 - i) * mx + (i - 1) * mn) / 7;
        for (int p = 0; p < 16; ++p) {
            int best = 0, err = 1 << 30;
            for (int i = 0; i < 8; ++i) {
                const int e = (pal[i] - v[p]) * (pal[i] - v[p]);
                if (e < err) err = e, best = i;
            }
            out |= uint64_t(best) << (3 * p);
        }
    }
    for (int i = 0; i < 6; ++i) b[2 + i] = static_cast<uint8_t>(out >> (8 * i));
}

void Unpack565(const uint8_t* b, float* rgb) {
    const uint16_t c = uint16_t(b[0] | b[1] << 8);
    rgb[0] = ((c >> 11) & 31) * 255.f / 31.f;
    rgb[1] = ((c >> 5) & 63) * 255.f / 63.f;
    rgb[2] = (c & 31) * 255.f / 31.f;
}

float Luma(const float* c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

// Colour error: a change of hue weighs 4x a change of brightness. Rounding
// each channel on its own turns greys green or magenta (G has 6 bits, R and
// B 5) [verified: user, previews of 2026-10-20].
float ColorError(const float* a, const float* b) {
    const float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    const float dl = Luma(d);
    return dl * dl + 4.f * ((d[0] - dl) * (d[0] - dl) + (d[1] - dl) * (d[1] - dl) + (d[2] - dl) * (d[2] - dl));
}

void Unpack565(uint16_t c, float* rgb) {
    rgb[0] = ((c >> 11) & 31) * 255.f / 31.f;
    rgb[1] = ((c >> 5) & 63) * 255.f / 63.f;
    rgb[2] = (c & 31) * 255.f / 31.f;
}

// The best 565 colours (by ColorError) just below and just above rgb in
// brightness. True greys only exist every ~8/255, so a block quantised to
// one of them alone jumps by that much from its neighbour: the dark
// "pixelated" squares seen in the game [verified: R5c, trail70 mip 1].
// Taking one of each lets the block's palette (thirds between the two) land
// in between.
void Bracket565(const float* rgb, uint16_t* below, uint16_t* above) {
    const int r0 = static_cast<int>(std::floor(rgb[0] * 31.f / 255.f)),
              g0 = static_cast<int>(std::floor(rgb[1] * 63.f / 255.f)),
              b0 = static_cast<int>(std::floor(rgb[2] * 31.f / 255.f));
    const float l = Luma(rgb);
    float eb = 1e30f, ea = 1e30f;
    *below = *above = 0;
    for (int r = r0 - 1; r <= r0 + 2; ++r)
        for (int g = g0 - 2; g <= g0 + 3; ++g)
            for (int bl = b0 - 1; bl <= b0 + 2; ++bl) {
                if (r < 0 || r > 31 || g < 0 || g > 63 || bl < 0 || bl > 31) continue;
                const uint16_t c = static_cast<uint16_t>(r << 11 | g << 5 | bl);
                float q[3];
                Unpack565(c, q);
                const float e = ColorError(q, rgb);
                if (Luma(q) <= l + 0.01f && e < eb) eb = e, *below = c;
                if (Luma(q) >= l - 0.01f && e < ea) ea = e, *above = c;
            }
    if (eb == 1e30f) *below = *above;
    if (ea == 1e30f) *above = *below;
}

// BC1-style colour half of a BC3 block (always 4-colour mode).
void DecodeColor(const uint8_t* b, float (*px)[3]) {
    float pal[4][3];
    Unpack565(uint16_t(b[0] | b[1] << 8), pal[0]);
    Unpack565(uint16_t(b[2] | b[3] << 8), pal[1]);
    for (int k = 0; k < 3; ++k) {
        pal[2][k] = (2.f * pal[0][k] + pal[1][k]) / 3.f;
        pal[3][k] = (pal[0][k] + 2.f * pal[1][k]) / 3.f;
    }
    const uint32_t idx = uint32_t(b[4] | b[5] << 8 | b[6] << 16 | uint32_t(b[7]) << 24);
    for (int p = 0; p < 16; ++p) std::copy(pal[(idx >> (2 * p)) & 3], pal[(idx >> (2 * p)) & 3] + 3, px[p]);
}

// Encodes 16 target colours: endpoints near the brightest and darkest
// pixel, each bracketed (below / above); the pair whose palette fits the
// pixels best wins.
void EncodeColor(const float (*px)[3], uint8_t* b) {
    int hi = 0, lo = 0;
    for (int p = 1; p < 16; ++p) {
        if (Luma(px[p]) > Luma(px[hi])) hi = p;
        if (Luma(px[p]) < Luma(px[lo])) lo = p;
    }
    uint16_t h[2], l[2];
    Bracket565(px[hi], &h[0], &h[1]);
    Bracket565(px[lo], &l[0], &l[1]);
    float best = 1e30f;
    uint16_t best0 = h[1], best1 = l[0];
    uint32_t best_idx = 0;
    for (uint16_t c0 : h)
        for (uint16_t c1 : l) {
            float pal[4][3];
            Unpack565(c0, pal[0]);
            Unpack565(c1, pal[1]);
            for (int k = 0; k < 3; ++k) {
                pal[2][k] = (2.f * pal[0][k] + pal[1][k]) / 3.f;
                pal[3][k] = (pal[0][k] + 2.f * pal[1][k]) / 3.f;
            }
            float err = 0;
            uint32_t idx = 0;
            for (int p = 0; p < 16; ++p) {
                int bi = 0;
                float be = ColorError(pal[0], px[p]);
                for (int i = 1; i < 4; ++i) {
                    const float e = ColorError(pal[i], px[p]);
                    if (e < be) be = e, bi = i;
                }
                err += be;
                idx |= uint32_t(bi) << (2 * p);
            }
            if (err < best) best = err, best0 = c0, best1 = c1, best_idx = idx;
        }
    b[0] = uint8_t(best0 & 0xFF), b[1] = uint8_t(best0 >> 8);
    b[2] = uint8_t(best1 & 0xFF), b[3] = uint8_t(best1 >> 8);
    for (int i = 0; i < 4; ++i) b[4 + i] = uint8_t(best_idx >> (8 * i));
}

// Wear weight of the block whose centre is (u, v): 1 deep inside the tread
// rectangle, fading to 0 across the feathers at its edges, 0 outside. The
// top/bottom edges are where the tile repeats around the tire, so they get
// no fade by default (R5c).
float Weight(float u, float v, const Rect& r, float feather_u, float feather_v) {
    if (u <= r.u0 || u >= r.u1 || v <= r.v0 || v >= r.v1) return 0.f;
    float w = 1.f;
    if (feather_u > 0.f) w = std::min(w, std::min(u - r.u0, r.u1 - u) / feather_u);
    if (feather_v > 0.f) w = std::min(w, std::min(v - r.v0, r.v1 - v) / feather_v);
    return w;
}

float Chroma(const float* c) { return std::max({c[0], c[1], c[2]}) - std::min({c[0], c[1], c[2]}); }

// Smooth value noise in -1..1 over u (cells per unit of u), the same for
// every v: scuffs run around the tire, so they continue across the tile seam.
float Noise1(float u, float cells, uint32_t seed) {
    auto h = [seed](int i) {
        uint32_t x = uint32_t(i) * 2654435761u ^ seed;
        x ^= x >> 15, x *= 2246822519u, x ^= x >> 13;
        return (x & 0xFFFF) / 32767.5f - 1.f;
    };
    const float x = u * cells;
    const int i = static_cast<int>(std::floor(x));
    const float t = x - i, s = t * t * (3.f - 2.f * t);
    return h(i) + (h(i + 1) - h(i)) * s;
}

// Scuffs: a few broad bands plus fine lines.
float Scuff(float u) { return 0.6f * Noise1(u, 23.f, 0x5c0ffu) + 0.4f * Noise1(u, 97.f, 0xa11cefu); }

// R5d helpers. vt = position along the tread tile (0..1, around the tire).

float Hash01(uint32_t i, uint32_t seed) {
    uint32_t x = i * 2654435761u ^ seed;
    x ^= x >> 15, x *= 2246822519u, x ^= x >> 13, x *= 3266489917u, x ^= x >> 16;
    return (x & 0xFFFFFF) / 16777215.f;
}

// Smooth 2D value noise in -1..1: cu cells per unit of u, cv cells per tile,
// periodic in vt so it continues across the tile seam.
float Noise2(float u, float vt, float cu, int cv, uint32_t seed) {
    auto h = [seed](int i, int j) {
        uint32_t x = uint32_t(i) * 2654435761u ^ uint32_t(j) * 2246822519u ^ seed;
        x ^= x >> 15, x *= 2246822519u, x ^= x >> 13;
        return (x & 0xFFFF) / 32767.5f - 1.f;
    };
    const float x = u * cu, y = vt * cv;
    const int i = static_cast<int>(std::floor(x)), j = static_cast<int>(std::floor(y));
    const float tx = x - i, ty = y - j;
    const float sx = tx * tx * (3.f - 2.f * tx), sy = ty * ty * (3.f - 2.f * ty);
    const int j0 = ((j % cv) + cv) % cv, j1 = (j0 + 1) % cv;
    const float a = h(i, j0) + (h(i + 1, j0) - h(i, j0)) * sx;
    const float b = h(i, j1) + (h(i + 1, j1) - h(i, j1)) * sx;
    return a + (b - a) * sy;
}

// Detail whose size is `px` pixels: full from 2 px, gone below half a pixel
// (finer detail would only alias on the small mips).
float Fade(float px) { return std::clamp((px - 0.5f) / 1.5f, 0.f, 1.f); }

// The per-pixel wear detail of one mip (R5d). Colour and normal map call the
// same functions with the same UV, so the two always line up.
struct Detail {
    const Rect& r;
    const Params& p;
    float width, height;
    uint32_t seed;  // per texture pair (from its tread rectangle), so models do not share chips

    float Vt(float v) const { return (v - r.v0) / (r.v1 - r.v0); }
    // 0 at the tread's centre line, 1 at the shoulders.
    float Side(float u) const { return std::fabs(2.f * (u - r.u0) / (r.u1 - r.u0) - 1.f); }

    // 2: wear at this point of the width: the centre runs ahead of the
    // shoulders, with a little difference from rib to rib.
    float LocalWear(float u, float w) const {
        const float s = Side(u);
        const float shape = 1.f + p.width_profile * (0.35f - s * s) + p.rib_vary * Noise1(u, 9.f, 0x71b5u);
        return std::clamp(w * shape, 0.f, 1.f);
    }
    // 1: abrasion grain: fine, stretched around the tire (14 and 40 cells per tile).
    float Grain(float u, float vt) const {
        return Fade(width / 260.f) * 0.65f * Noise2(u, vt, 260.f, 14, 0x9a17u) +
               Fade(width / 520.f) * 0.35f * Noise2(u, vt, 520.f, 40, 0x3c5eu);
    }
    // 6: cupping: two slow waves per tile, the phase drifting across the width.
    float CupPhase(float u, float vt) const { return 6.2831853f * (2.f * vt + 0.15f * Noise1(u, 5.f, 0xc0bu)); }
    float CupWeight(float u) const { return 0.5f + 0.5f * Side(u); }
    // 5: chips: an irregular torn-out spot (lobed outline, random size,
    // stretch and angle): depth 0..1 of its dark core, its light torn rim
    // (rim, 0..1) and the core's slope (for the normal map).
    float Chip(float u, float vt, float w, float* gu, float* gv, float* rim) const {
        *gu = *gv = *rim = 0.f;
        const float count = p.chips * w * std::sqrt(w);
        float depth = 0.f;
        const float tile_h = r.v1 - r.v0;
        for (int i = 0; i < int(std::ceil(count)); ++i) {
            const float ru = 0.0015f + 0.003f * Hash01(i, seed ^ 0xc41u);  // UV across
            const float fade = Fade(ru * width);
            if (fade <= 0.f) continue;
            const float cu = r.u0 + (r.u1 - r.u0) * (0.04f + 0.92f * Hash01(i, seed ^ 0xc43u));
            float dv = vt - Hash01(i, seed ^ 0xc44u);
            dv -= std::floor(dv + 0.5f);  // around the tile seam
            // To pixels (isotropic), then rotate and stretch.
            const float px = (u - cu) * width, py = dv * tile_h * height, rp = ru * width;
            const float a = 6.2831853f * Hash01(i, seed ^ 0xc46u), ca = std::cos(a), sa = std::sin(a);
            const float stretch = 1.f + 1.5f * Hash01(i, seed ^ 0xc42u);
            const float x = (px * ca + py * sa) / (rp * stretch), y = (-px * sa + py * ca) / rp;
            const float d2 = x * x + y * y;
            if (d2 >= 2.f) continue;
            const float th = std::atan2(y, x), ph = 6.2831853f * Hash01(i, seed ^ 0xc47u);
            const float lobes = 1.f + 0.3f * std::sin(3.f * th + ph) + 0.18f * std::sin(5.f * th + 2.f * ph);
            const float d = std::sqrt(d2) / lobes;
            const float strength = std::min(1.f, count - i) * fade * (0.6f + 0.4f * Hash01(i, seed ^ 0xc45u));
            if (d < 1.f) {
                const float core = (1.f - d * d) * strength;
                if (core > depth) depth = core, *gu = -x * strength, *gv = -y * strength;
            } else if (d < 1.35f) {
                *rim = std::max(*rim, (1.f - (d - 1.f) / 0.35f) * strength);
            }
        }
        return depth;
    }
};

// New-tire stripe: thin white or coloured line painted around the tread
// (some Michelin textures) [verified: r_315-70_michelin_xlined2,
// 315-70/315-80_michelin_xze]. An endpoint belongs to it when it is far
// brighter than the tread or clearly coloured (rubber is near grey).
constexpr float kStripeChroma = 48.f;
constexpr float kStripeRows = 0.3f;         // share of a block column's rows holding it
constexpr float kStripeRowsProfile = 0.7f;  // the same, for a column found by its profile (small mips)
constexpr float kStripeExcess = 10.f;  // brightness the line's column must stand out by (small mips)
constexpr int kMinAnalysisBlocks = 128;
constexpr float kTwiLength = 0.025f;  // wear bar length around the tire, share of the tile [estimativa]
constexpr float kMainGroove = 0.6f;   // share of a pixel column's rows in groove for a main groove
// cut: brightness above which a colour of a stripe column is the line.
bool IsStripe(const float* c, float cut) { return Luma(c) > cut || Chroma(c) > kStripeChroma; }

}  // namespace

ColorAnalysis AnalyzeColor(uint32_t format, const uint8_t* blocks, uint32_t width, uint32_t height, const Rect& tread,
                           const Params& p, std::vector<float>* stripe) {
    ColorAnalysis an;
    if (!IsColor(format)) return an;
    const uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4;
    auto block = [&](uint32_t bx, uint32_t by) { return blocks + (size_t(by) * bw + bx) * 16; };
    auto weight = [&](uint32_t bx, uint32_t by) {
        return Weight((bx * 4 + 2.f) / width, (by * 4 + 2.f) / height, tread, p.feather_u, p.feather_v);
    };
    // Mean colour and specular, and every endpoint's brightness.
    double sum_rgb[3] = {}, sum_a = 0;
    std::vector<float> ls;
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx) {
            if (weight(bx, by) < 1.f) continue;
            const uint8_t* b = block(bx, by);
            for (int e = 0; e < 2; ++e) {
                float c[3];
                Unpack565(b + 8 + e * 2, c);
                for (int k = 0; k < 3; ++k) sum_rgb[k] += c[k] * 0.5;
                ls.push_back(Luma(c));
            }
            sum_a += (b[0] + b[1]) * 0.5;
            ++an.blocks;
        }
    if (an.blocks == 0) return an;
    for (int k = 0; k < 3; ++k) an.mean_rgb[k] = float(sum_rgb[k] / an.blocks);
    an.mean_a = float(sum_a / an.blocks);
    // Outliers (a stripe, glints) above the upper fence p75 + 1.5 IQR are left
    // out: a white stripe alone pulled the threshold above the whole tread.
    std::vector<float> sorted = ls;
    std::sort(sorted.begin(), sorted.end());
    const float p25 = sorted[sorted.size() / 4], p75 = sorted[sorted.size() * 3 / 4];
    an.fence = p75 + 1.5f * (p75 - p25) + 4.f;  // + 4: about one 565 step, for very flat treads
    const auto inner_end = std::upper_bound(sorted.begin(), sorted.end(), an.fence);
    an.groove_t = GrooveThreshold(sorted.data(), size_t(inner_end - sorted.begin()));
    const auto top_begin = std::lower_bound(sorted.begin(), inner_end, an.groove_t);
    an.top_l = top_begin < inner_end ? *(top_begin + (inner_end - top_begin) / 2) : an.groove_t;
    // Small mips (seen from afar): too few blocks for a stable threshold
    // (trail70 64 px: 19.5 against ~48 above), and grooves are blurred into
    // the top anyway, so everything below the top counts as groove.
    if (an.blocks < kMinAnalysisBlocks) an.groove_t = an.top_l;
    // Stripe columns. Only columns clear of the edge fade: the edge column
    // takes in the bright shoulder [verified: runner70, goodyear kmax,
    // michelin xde]. Profile = per column, the median over its rows of the
    // block's brighter endpoint. On small mips the line is blurred into its
    // block and no longer passes the fence [verified: michelin xlinef mip 2],
    // so a column also counts when its profile stands out from its
    // neighbours' (+-3 columns) and from the top surface; its cut is then
    // halfway between the neighbours and the line.
    std::vector<float> profile(bw, -1.f);
    for (uint32_t bx = 0; bx < bw; ++bx) {
        std::vector<float> col;
        for (uint32_t by = 0; by < bh; ++by) {
            if (weight(bx, by) < 1.f) continue;
            float c0[3], c1[3];
            Unpack565(block(bx, by) + 8, c0);
            Unpack565(block(bx, by) + 10, c1);
            col.push_back(std::max(Luma(c0), Luma(c1)));
        }
        if (col.empty()) continue;
        std::nth_element(col.begin(), col.begin() + col.size() / 2, col.end());
        profile[bx] = col[col.size() / 2];
    }
    std::vector<float> cuts(bw, kNoStripe);
    for (uint32_t bx = 0; bx < bw; ++bx) {
        if (profile[bx] < 0.f) continue;
        float cut = an.fence;
        // Each side's level: the brighter of its columns 2 and 3 away (the
        // line may spill into the next column). A line is darker-flanked on
        // both sides; a bright rib is not.
        auto side = [&](int dir) {
            float s = -1.f;
            for (int d = 2; d <= 3; ++d) {
                const int x = int(bx) + dir * d;
                if (x >= 0 && x < int(bw)) s = std::max(s, profile[x]);
            }
            return s;
        };
        // The line runs on a rib: the top surface on both sides, at about the
        // same level. A groove's bright lip has the groove on one side
        // [verified: goodyear fmaxs2/kmaxs2, michelin xline_en_z/f].
        const float left = side(-1), right = side(+1);
        if (left >= an.top_l - kStripeExcess && right >= an.top_l - kStripeExcess &&
            std::fabs(left - right) < kStripeExcess) {
            const float base = std::max(left, right);
            if (profile[bx] - base > kStripeExcess && profile[bx] > an.top_l + kStripeExcess)
                cut = std::min(cut, std::max(an.top_l + 4.f, (base + profile[bx]) * 0.5f));
        }
        int rows = 0, hits = 0;
        for (uint32_t by = 0; by < bh; ++by) {
            if (weight(bx, by) < 1.f) continue;
            ++rows;
            for (int e = 0; e < 2; ++e) {
                float c[3];
                Unpack565(block(bx, by) + 8 + e * 2, c);
                if (IsStripe(c, cut)) {
                    ++hits;
                    break;
                }
            }
        }
        // A cut below the fence (profile rule) needs a near continuous line:
        // bright ribs and sipes break up along the tread.
        const float need = cut < an.fence ? kStripeRowsProfile : kStripeRows;
        if (rows > 0 && hits >= need * rows) {
            cuts[bx] = cut;
            ++an.stripe_columns;
        }
    }
    // The line may spill into the next column (blurred, or off the block
    // grid) [verified: michelin xlinef mips 1-2]: same cut there.
    std::vector<float> spread = cuts;
    for (uint32_t bx = 0; bx < bw; ++bx)
        if (cuts[bx] != kNoStripe)
            for (int d : {-1, 1}) {
                const int x = int(bx) + d;
                if (x >= 0 && x < int(bw)) spread[x] = std::min(spread[x], cuts[bx]);
            }
    if (stripe) *stripe = std::move(spread);
    return an;
}

float GrooveThreshold(const float* luma, size_t n) {
    double hist[256] = {}, sum = 0;
    for (size_t i = 0; i < n; ++i) hist[std::clamp(static_cast<int>(luma[i]), 0, 255)] += 1;
    for (int i = 0; i < 256; ++i) sum += i * hist[i];
    double wb = 0, sb = 0, best = -1;
    int t = 0;
    for (int i = 0; i < 256; ++i) {
        wb += hist[i];
        sb += i * hist[i];
        if (wb == 0 || wb == double(n)) continue;
        const double mb = sb / wb, mf = (sum - sb) / (n - wb), v = wb * (n - wb) * (mb - mf) * (mb - mf);
        if (v > best) best = v, t = i;
    }
    return t + 0.5f;
}

void AffineBc4(uint8_t* b, float a, float c) {
    int v[16];
    DecodeBc4(b, v);
    for (int& x : v) x = Clamp255(a * x + c);
    EncodeBc4(v, b);
}

void Apply(uint32_t format, uint8_t* blocks, uint32_t width, uint32_t height, const Rect& tread, float wear,
           const Params& p) {
    if (wear <= 0.f || (!IsNormal(format) && !IsColor(format))) return;
    wear = std::min(wear, 1.f);
    const uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4;
    auto block = [&](uint32_t bx, uint32_t by) { return blocks + (size_t(by) * bw + bx) * 16; };
    auto weight = [&](uint32_t bx, uint32_t by) {
        return Weight((bx * 4 + 2.f) / width, (by * 4 + 2.f) / height, tread, p.feather_u, p.feather_v);
    };
    // Scuffs fade out on small mips, where one block spans several of them.
    const float scuff_scale = std::min(1.f, width / 512.f);
    auto scuff = [&](uint32_t bx) { return Scuff((bx * 4 + 2.f) / width) * scuff_scale; };
    auto pixel_scuff = [&](uint32_t x) { return Scuff((x + 0.5f) / width) * scuff_scale; };

    const uint32_t seed = uint32_t(tread.u0 * 1e4f) * 73856093u ^ uint32_t(tread.v0 * 1e4f) * 19349663u ^
                          uint32_t(tread.u1 * 1e4f) * 83492791u ^ uint32_t(tread.v1 * 1e4f);
    const Detail det{tread, p, float(width), float(height), seed};
    if (IsNormal(format)) {
        for (uint32_t by = 0; by < bh; ++by)
            for (uint32_t bx = 0; bx < bw; ++bx) {
                const float w = wear * weight(bx, by);
                if (w <= 0.f) continue;
                // Scuff ridges: X (across the tread) tilts with the scuffs'
                // slope, so they catch the light like fine grooves.
                const float slope = (scuff(bx + 1) - scuff(bx > 0 ? bx - 1 : bx)) * 0.5f;
                const float ridge = std::clamp(slope * 4.f, -1.f, 1.f) * p.streak_relief;
                uint8_t* b = block(bx, by);
                int nx[16], ny[16];
                DecodeBc4(b, nx);
                DecodeBc4(b + 8, ny);
                for (int i = 0; i < 16; ++i) {
                    const float u = (bx * 4 + i % 4 + 0.5f) / width, vt = det.Vt((by * 4 + i / 4 + 0.5f) / height);
                    const float wl = det.LocalWear(u, w);
                    const float k = 1.f - (1.f - p.relief_left) * wl;  // relief kept
                    float gu, gv, rim;
                    const float chip = det.Chip(u, vt, wl, &gu, &gv, &rim);
                    const float cupw = p.cup_relief * wl * det.CupWeight(u);
                    float x = 128.f + (nx[i] - 128.f) * k + ridge * wl + p.grain_relief * wl * det.Grain(u + 0.37f, vt);
                    float y = 128.f + (ny[i] - 128.f) * k + cupw * std::cos(det.CupPhase(u, vt));
                    if (chip > 0.f) x += p.chip_relief * gu, y += p.chip_relief * gv;
                    nx[i] = Clamp255(x), ny[i] = Clamp255(y);
                }
                EncodeBc4(nx, b);
                EncodeBc4(ny, b + 8);
            }
        return;
    }

    std::vector<float> stripe;
    const ColorAnalysis an = AnalyzeColor(format, blocks, width, height, tread, p, &stripe);
    if (an.blocks == 0) return;
    const float* mean_rgb = an.mean_rgb;
    const float mean_a = an.mean_a, mean_l = Luma(mean_rgb), groove_t = an.groove_t, top_l = an.top_l;
    // The new-tire stripe takes the top surface's colour (with the tread's
    // mean tint) from the first level on, in full: a painted line wears off
    // at once [user, 2026-10-20].
    float stripe_rgb[3];
    for (int k = 0; k < 3; ++k) stripe_rgb[k] = top_l + (mean_rgb[k] - mean_l);

    // 4: main (circumferential) grooves: pixel columns that are groove in
    // most of the tile. Needs a reliable threshold, so not on small mips.
    std::vector<uint8_t> main_groove(width, 0);
    const float bar_fade = Fade((tread.v1 - tread.v0) * height * kTwiLength);
    if (an.blocks >= kMinAnalysisBlocks && bar_fade > 0.f && wear >= p.twi_from * 0.8f) {
        std::vector<int> groove(width, 0), rows(width, 0);
        for (uint32_t by = 0; by < bh; ++by)
            for (uint32_t bx = 0; bx < bw; ++bx) {
                if (weight(bx, by) < 1.f) continue;
                float px[16][3];
                DecodeColor(block(bx, by) + 8, px);
                for (int i = 0; i < 16; ++i) {
                    const uint32_t x = bx * 4 + i % 4;
                    if (x >= width) continue;
                    ++rows[x];
                    groove[x] += Luma(px[i]) < groove_t;
                }
            }
        for (uint32_t x = 0; x < width; ++x) main_groove[x] = rows[x] > 0 && groove[x] >= kMainGroove * rows[x];
    }
    // 3: dust colour (light grey-brown).
    const float dirt_tint[3] = {3.f, 1.f, -3.f};  // was 6/1/-6: read as rust

    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx) {
            const float w = wear * weight(bx, by);
            if (w <= 0.f) continue;
            uint8_t* b = block(bx, by);
            // Specular (alpha block): less contrast; R5c: duller (matte rubber).
            const float s = 1.f - p.spec_flatten * w, g = 1.f + p.spec_gain * w;
            AffineBc4(b, s * g, mean_a * (1.f - s) * g);
            // Colour, pixel by pixel, then the block is encoded again (R5c):
            // grooves (below the threshold) rise towards the top's brightness
            // as they get shallower; the darker half of the top surface
            // follows less and less (no step at the threshold), the rest
            // keeps its tone, losing a little of its fine contrast; then a
            // touch of dust, the scuffs and a slightly darker overall tone;
            // colour towards neutral grey.
            // R5d, per pixel: the wear varies across the width (2); then
            // dust in the remaining grooves (3), the wear indicator bars
            // (4), grain (1) and cupping (6) in the brightness, and chips (5).
            float px[16][3];
            DecodeColor(b + 8, px);
            for (int i = 0; i < 16; ++i) {
                float* c = px[i];
                const uint32_t x = bx * 4 + i % 4;
                const float u = (x + 0.5f) / width, vt = det.Vt((by * 4 + i / 4 + 0.5f) / height);
                const float wl = det.LocalWear(u, w);
                const float fill = p.groove_fill * wl, top_keep = 1.f - p.top_contrast * wl;
                const float keep = 1.f - p.color_mix * wl, chroma = 1.f - p.desaturate * wl;
                const float dust = p.lift * 255.f * wl, tone = 1.f - p.darken * wl;
                if (stripe[bx] != kNoStripe && IsStripe(c, stripe[bx])) std::copy(stripe_rgb, stripe_rgb + 3, c);
                const float l = Luma(c);
                float follow = l < groove_t ? 1.f : (top_l - l) / std::max(1.f, top_l - groove_t);
                follow *= follow;
                float nl = l < top_l ? l + (top_l - l) * fill * follow : top_l + (l - top_l) * top_keep;
                nl = mean_l + (nl - mean_l) * keep;  // R5b look only (color_mix 0 in R5c)
                const float swing = 1.f + p.streak * wl * pixel_scuff(x);
                nl = (nl + dust) * swing * tone;
                for (int k = 0; k < 3; ++k) c[k] = nl + (c[k] - l) * chroma;
                // 3: dust settles in what is left of the grooves, patchy,
                // most at mid wear (shallow grooves hold the most, gone when smooth).
                const float groove_mask = l < top_l ? follow : 0.f;
                const float dirt = p.dirt * std::sin(3.1415927f * wl) * groove_mask *
                                   (0.55f + 0.45f * Noise2(u, vt, 40.f, 5, 0xd127u));
                if (dirt > 0.f) {
                    const float dl = std::max(nl, top_l * 1.08f * tone);
                    for (int k = 0; k < 3; ++k) c[k] += (dl + dirt_tint[k] - c[k]) * dirt;
                }
                // 4: a short bar across each main groove, once per tile,
                // flush with the tread near the limit (top surface colour).
                if (main_groove[x] && l < groove_t && std::fabs(vt - 0.5f) < kTwiLength * 0.5f) {
                    const float a = bar_fade * std::clamp((wl - p.twi_from) / (p.twi_full - p.twi_from), 0.f, 1.f);
                    const float tl = (top_l + dust) * tone;
                    for (int k = 0; k < 3; ++k) c[k] += (tl + (mean_rgb[k] - mean_l) * chroma - c[k]) * a;
                }
                // 1 + 6: grain and cupping.
                const float shade = 1.f + p.grain * wl * det.Grain(u, vt) +
                                    p.cup * wl * det.CupWeight(u) * std::sin(det.CupPhase(u, vt));
                // 5: chips: darker cores, lighter torn rims.
                float gu, gv, rim;
                const float chip = det.Chip(u, vt, wl, &gu, &gv, &rim);
                const float f = shade * (1.f - p.chip_dark * chip + p.chip_rim * rim);
                for (int k = 0; k < 3; ++k) c[k] = std::clamp(c[k] * f, 0.f, 255.f);
            }
            EncodeColor(px, b + 8);
        }
}

namespace {

// E4b: where the tread is torn off. Colour and normal map evaluate the same
// fields at the same UV, so they always line up.
struct Burst {
    const Rect& r;
    const BurstParams& p;
    float width, height;
    uint32_t seed;
    float thr = 0.f;              // tear field level above which the tread is gone
    float period = 1.f;           // cord spacing in pixels
    float ca = 1.f, sa = 0.f;     // cord direction (cos, sin of the lean)

    float Vt(float v) const { return (v - r.v0) / (r.v1 - r.v0); }
    // Tear field: big patches (6 across the tread, 3 per tile) with a ragged
    // edge from finer noise; periodic around the tire.
    float Field(float u, float vt) const {
        const float tw = r.u1 - r.u0;
        const float fine = Fade(width * tw / 40.f);
        return 0.7f * Noise2(u, vt, 6.f / tw, 3, seed ^ 0xb1a5u) + 0.3f * fine * Noise2(u, vt, 40.f / tw, 14, seed ^ 0x7e4du);
    }
    float Torn(float u, float vt) const {
        const float t = std::clamp((Field(u, vt) - thr + p.edge) / (2.f * p.edge), 0.f, 1.f);
        // Not right at the shoulders' edge, where the tile meets the sidewall.
        return t * t * (3.f - 2.f * t) * Weight(u, r.v0 + (r.v1 - r.v0) * 0.5f, r, 0.03f, 0.f);
    }
    // Torn rubber rim just outside a tear, 0..1.
    float Rim(float u, float vt) const {
        const float d = thr - p.edge - Field(u, vt);
        return d > 0.f ? std::max(0.f, 1.f - d / (2.f * p.edge)) : 0.f;
    }
    // Steel cords: thin bright lines, leaning from the circumference; the
    // second belt (deeper, where the tear is deepest) leans the other way.
    // Returns 0..1 and the slope across the lines (for the normal map).
    float Cord(float u, float vt, bool second, float* slope) const {
        const float x = (u - r.u0) * width, y = vt * (r.v1 - r.v0) * height;
        // A little waviness, and patches where rubber left on the ply hides the cords.
        const float wave = 0.25f * Noise2(u, vt, 30.f / (r.u1 - r.u0), 6, seed ^ 0x3a1u);
        const float s = (x * ca + (second ? -y : y) * sa) / period + wave;
        const float c = 0.5f + 0.5f * std::cos(6.2831853f * s);
        const float c2 = c * c, c4 = c2 * c2, c8 = c4 * c4, pulse = c8 * c2;  // c^10: thin lines
        const float bare = std::clamp(0.55f + 0.9f * Noise2(u, vt, 16.f / (r.u1 - r.u0), 8, seed ^ 0x9c3u), 0.f, 1.f);
        const float fade = Fade(period * 0.5f);
        *slope = fade * bare * -10.f * c8 * c * 0.5f * std::sin(6.2831853f * s);
        return bare * (fade * pulse + (1.f - fade) * 0.18f);  // mean of c^10 when too fine to show
    }
};

Burst MakeBurst(const Rect& r, const BurstParams& p, uint32_t width, uint32_t height) {
    const uint32_t seed = uint32_t(r.u0 * 1e4f) * 73856093u ^ uint32_t(r.v0 * 1e4f) * 19349663u ^
                          uint32_t(r.u1 * 1e4f) * 83492791u ^ uint32_t(r.v1 * 1e4f);
    Burst b{r, p, float(width), float(height), seed};
    // Threshold: the field's quantile that leaves `torn` of the tread above it.
    std::vector<float> f;
    for (int j = 0; j < 64; ++j)
        for (int i = 0; i < 64; ++i) f.push_back(b.Field(r.u0 + (r.u1 - r.u0) * (i + 0.5f) / 64.f, (j + 0.5f) / 64.f));
    std::sort(f.begin(), f.end());
    b.thr = f[std::clamp(size_t((1.f - p.torn) * f.size()), size_t(0), f.size() - 1)];
    // Cords: spacing from the tread width; the lean is rounded so the lines
    // meet themselves across the tile seam.
    b.period = std::max(0.5f, (r.u1 - r.u0) * width / std::max(1.f, p.cords));
    const float tile_px = (r.v1 - r.v0) * height;
    const float k = std::round(tile_px * std::sin(p.cord_angle) / b.period);
    b.sa = std::clamp(k * b.period / std::max(1.f, tile_px), -1.f, 1.f);
    b.ca = std::sqrt(1.f - b.sa * b.sa);
    return b;
}

}  // namespace

namespace {

// The sidewall gash: a ragged lens along the circumference, open to the dark
// inside of the tire, with the radial ply cords showing at its edges and a
// lighter, raised rim of torn rubber. Only the blocks in its bounding box
// are touched, so it costs little.
struct Gash {
    const BurstParams& p;
    float width, height;
    uint32_t seed;
    float angle, half_len, half_w;  // radians, radians, UV

    // Signed distance (UV) from the gash's edge: < 0 inside. Also the polar
    // position, for the cords.
    float Dist(float u, float v, float* phi_out, float* r_out) const {
        const float du = u - 0.5f, dv = v - 0.5f, r = std::sqrt(du * du + dv * dv);
        float d_phi = std::atan2(dv, du) - angle;
        d_phi -= 6.2831853f * std::floor(d_phi / 6.2831853f + 0.5f);
        *phi_out = d_phi, *r_out = r;
        if (std::fabs(d_phi) >= half_len) return 1.f;
        const float t = d_phi / half_len;
        // Ragged: the width and the centre line wander along the gash.
        const float rag = 1.f + 0.35f * Noise1(t + 3.f, 9.f, seed ^ 0x6a5u) + 0.15f * Noise1(t + 7.f, 31.f, seed ^ 0x2b7u);
        const float w = half_w * std::sqrt(1.f - t * t) * rag;
        const float rc = p.side_r + 0.25f * half_w * Noise1(t + 11.f, 4.f, seed ^ 0x1c9u);
        return std::fabs(r - rc) - w;
    }
};

void ApplyGash(uint32_t format, uint8_t* blocks, uint32_t width, uint32_t height, const Rect& tread,
               const BurstParams& p, uint32_t seed) {
    const float size = std::clamp(p.side_size, 0.f, 1.f);
    const Gash g{p, float(width), float(height), seed, 6.2831853f * p.side_angle,
                 0.5f * (0.3f + 1.0f * size), 0.012f + 0.026f * size};
    const float rim_w = 0.006f;  // torn rubber rim width (UV)
    // Bounding box of the gash and its rim.
    float u_min = 1, u_max = 0, v_min = 1, v_max = 0;
    for (int i = 0; i <= 16; ++i) {
        const float a = g.angle - g.half_len + 2.f * g.half_len * i / 16.f;
        for (float r : {p.side_r - 2.f * g.half_w - rim_w, p.side_r + 2.f * g.half_w + rim_w}) {
            const float u = 0.5f + r * std::cos(a), v = 0.5f + r * std::sin(a);
            u_min = std::min(u_min, u), u_max = std::max(u_max, u), v_min = std::min(v_min, v), v_max = std::max(v_max, v);
        }
    }
    const uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4;
    const uint32_t bx0 = uint32_t(std::max(0.f, u_min * width / 4.f)), bx1 = std::min(bw, uint32_t(u_max * width / 4.f) + 1);
    const uint32_t by0 = uint32_t(std::max(0.f, v_min * height / 4.f)), by1 = std::min(bh, uint32_t(v_max * height / 4.f) + 1);
    const float px_uv = 1.f / width;
    // Radial cords: ~2.5 px apart on mip 0, gone when finer than a pixel.
    const float cord_period = 2.5f * 1024.f / std::max(256.f, float(width)) * px_uv;
    const float cord_fade = Fade(cord_period * width);
    auto inside_ring = [&](float u, float v) {
        if (u > tread.u0 && u < tread.u1 && v > tread.v0 && v < tread.v1) return false;
        const float du = u - 0.5f, dv = v - 0.5f, r = std::sqrt(du * du + dv * dv);
        return r > p.side_ring_in && r < p.side_ring_out;
    };
    for (uint32_t by = by0; by < by1; ++by)
        for (uint32_t bx = bx0; bx < bx1; ++bx) {
            uint8_t* b = blocks + (size_t(by) * bw + bx) * 16;
            if (IsNormal(format)) {
                int nx[16], ny[16];
                DecodeBc4(b, nx);
                DecodeBc4(b + 8, ny);
                bool changed = false;
                for (int i = 0; i < 16; ++i) {
                    const float u = (bx * 4 + i % 4 + 0.5f) / width, v = (by * 4 + i / 4 + 0.5f) / height;
                    if (!inside_ring(u, v)) continue;
                    float phi, r;
                    const float d = g.Dist(u, v, &phi, &r);
                    if (d > rim_w * 1.5f) continue;
                    // Slope across the edge (radial): a deep step into the hole,
                    // a raised lip of torn rubber just outside.
                    float phi2, r2;
                    const float dr = g.Dist(u + 0.5f * px_uv * (u - 0.5f) / std::max(r, 1e-4f),
                                            v + 0.5f * px_uv * (v - 0.5f) / std::max(r, 1e-4f), &phi2, &r2) - d;
                    const float edge = std::clamp(1.f - std::fabs(d) / (2.f * px_uv + 0.3f * rim_w), 0.f, 1.f);
                    const float slope = std::clamp(dr / (0.5f * px_uv), -1.f, 1.f) * edge * 90.f;
                    const float cu = (u - 0.5f) / std::max(r, 1e-4f), cv = (v - 0.5f) / std::max(r, 1e-4f);
                    const float flat = d < 0.f ? 0.f : 1.f;  // the hole has no surface detail
                    nx[i] = Clamp255(128.f + (nx[i] - 128.f) * flat + slope * cu);
                    ny[i] = Clamp255(128.f + (ny[i] - 128.f) * flat + slope * cv);
                    changed = true;
                }
                if (changed) EncodeBc4(nx, b), EncodeBc4(ny, b + 8);
                continue;
            }
            int spec[16];
            DecodeBc4(b, spec);
            float px[16][3];
            DecodeColor(b + 8, px);
            bool changed = false;
            for (int i = 0; i < 16; ++i) {
                const float u = (bx * 4 + i % 4 + 0.5f) / width, v = (by * 4 + i / 4 + 0.5f) / height;
                if (!inside_ring(u, v)) continue;
                float phi, r;
                const float d = g.Dist(u, v, &phi, &r);
                if (d > rim_w) continue;
                float* c = px[i];
                if (d >= 0.f) {
                    // Torn rubber lip: lighter, scuffed.
                    const float lip = 1.f - d / rim_w;
                    for (int k = 0; k < 3; ++k) c[k] = std::clamp(c[k] * (1.f + 0.7f * lip * lip), 0.f, 255.f);
                    changed = true;
                    continue;
                }
                // Hole: the dark inside of the tire; near the edge the cut ply
                // with its radial cords (steel-grey lines across the gash).
                const float depth = std::clamp(-d / (0.2f * g.half_w + 2.f * px_uv), 0.f, 1.f);
                const float s = phi * r / cord_period;
                const float cc = 0.5f + 0.5f * std::cos(6.2831853f * s);
                const float cord = cord_fade * cc * cc * cc * cc * (1.f - depth);
                const float l = 4.f + 10.f * (1.f - depth) + 70.f * cord;
                for (int k = 0; k < 3; ++k) c[k] = l + (k == 0 ? 2.f : k == 2 ? -1.f : 0.f) * (1.f - cord);
                spec[i] = Clamp255(10.f + 120.f * cord);
                changed = true;
            }
            if (changed) EncodeBc4(spec, b), EncodeColor(px, b + 8);
        }
}

}  // namespace

void ApplyBurst(uint32_t format, uint8_t* blocks, uint32_t width, uint32_t height, const Rect& tread,
                const BurstParams& p) {
    if (!IsNormal(format) && !IsColor(format)) return;
    const uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4;
    const Burst bu = MakeBurst(tread, p, width, height);
    if (p.side) ApplyGash(format, blocks, width, height, tread, p, bu.seed ^ uint32_t(p.side_angle * 1e6f));
    const float du = 1.f / width, dvt = 1.f / ((tread.v1 - tread.v0) * height);
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx) {
            if (Weight((bx * 4 + 2.f) / width, (by * 4 + 2.f) / height, tread, 0.f, 0.f) <= 0.f) continue;
            uint8_t* b = blocks + (size_t(by) * bw + bx) * 16;
            if (IsNormal(format)) {
                int nx[16], ny[16];
                DecodeBc4(b, nx);
                DecodeBc4(b + 8, ny);
                for (int i = 0; i < 16; ++i) {
                    const float u = (bx * 4 + i % 4 + 0.5f) / width, vt = bu.Vt((by * 4 + i / 4 + 0.5f) / height);
                    const float m = bu.Torn(u, vt);
                    // The torn edge is a step down into the carcass.
                    const float gx = bu.Torn(u + du, vt) - bu.Torn(u - du, vt);
                    const float gy = bu.Torn(u, vt + dvt) - bu.Torn(u, vt - dvt);
                    float slope = 0.f;
                    if (m > 0.f) bu.Cord(u, vt, false, &slope);
                    float x = 128.f + (nx[i] - 128.f) * (1.f - m) + p.relief * 0.5f * gx + p.cord_relief * m * slope * bu.ca;
                    float y = 128.f + (ny[i] - 128.f) * (1.f - m) + p.relief * 0.5f * gy + p.cord_relief * m * slope * bu.sa;
                    nx[i] = Clamp255(x), ny[i] = Clamp255(y);
                }
                EncodeBc4(nx, b);
                EncodeBc4(ny, b + 8);
                continue;
            }
            int spec[16];
            DecodeBc4(b, spec);
            float px[16][3];
            DecodeColor(b + 8, px);
            for (int i = 0; i < 16; ++i) {
                const float u = (bx * 4 + i % 4 + 0.5f) / width, vt = bu.Vt((by * 4 + i / 4 + 0.5f) / height);
                const float m = bu.Torn(u, vt), rim = bu.Rim(u, vt);
                float* c = px[i];
                // Torn rubber: lighter, rougher at the rim.
                const float lift = 1.f + p.rim_light * rim;
                if (m <= 0.f) {
                    for (int k = 0; k < 3; ++k) c[k] = std::clamp(c[k] * lift, 0.f, 255.f);
                    continue;
                }
                // Carcass: dark brown-grey ply, uneven, with the steel cords
                // (the second belt where the tear goes deepest).
                float slope;
                float cord = bu.Cord(u, vt, false, &slope);
                const float into = bu.Field(u, vt) - bu.thr;  // how far inside the tear
                const float deep = std::clamp((into - 0.35f) / 0.1f, 0.f, 1.f);
                cord = std::max(cord, deep * bu.Cord(u, vt, true, &slope));
                // In the shadow of the torn wall near the edge.
                const float shadow = 0.55f + 0.45f * std::clamp((into - p.edge) / (4.f * p.edge), 0.f, 1.f);
                const float ply = p.carcass_l * (1.f + 0.2f * Noise2(u, vt, 120.f, 20, bu.seed ^ 0x51u));
                const float l = (ply + (p.cord_l - ply) * cord) * shadow;
                const float tint[3] = {4.f * (1.f - cord) - 2.f * cord, 1.f, -3.f * (1.f - cord) + 3.f * cord};
                for (int k = 0; k < 3; ++k) c[k] = std::clamp(c[k] * lift * (1.f - m) + (l + tint[k]) * m, 0.f, 255.f);
                spec[i] = Clamp255(spec[i] * (1.f - m) + (p.spec_carcass + (p.spec_cord - p.spec_carcass) * cord) * m);
            }
            EncodeBc4(spec, b);
            EncodeColor(px, b + 8);
        }
}

}  // namespace wear
