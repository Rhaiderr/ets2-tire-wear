// wearpreview (step R5a): renders the wear levels of a tire texture pair to
// images, using the same wear::Apply as the plugin.
//
// Usage: wearpreview <colspec.dds> <norm.dds> <out_prefix> [u0 v0 u1 v1]
// Without a rectangle the tread is detected from the normal map's relief.
// Writes BMPs (converted to PNG by the caller):
//   <prefix>_rect.bmp    full texture, shaded, tread rectangle outlined
//   <prefix>_levels.bmp  tread crops at every wear level (wear::kDefaultLevels), side by side
//   <prefix>_full100.bmp full texture at 100% (sidewall must look unchanged)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/wear_transform.h"

namespace {

struct Dds {
    int w = 0, h = 0;
    uint32_t format = 0;
    std::vector<uint8_t> mip0;  // packed blocks
};

// Mip `mip` of a DDS file (DX10 header, BC formats: 16-byte blocks).
bool LoadDdsMip(const char* path, int mip, Dds* d) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    std::vector<uint8_t> buf;
    uint8_t tmp[65536];
    size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0) buf.insert(buf.end(), tmp, tmp + n);
    fclose(f);
    if (buf.size() < 148 || memcmp(buf.data(), "DDS ", 4) != 0) return false;
    int w, h;
    memcpy(&h, buf.data() + 12, 4);
    memcpy(&w, buf.data() + 16, 4);
    memcpy(&d->format, buf.data() + 128, 4);
    size_t off = 148;
    for (int m = 0; m < mip; ++m) {
        off += size_t((w + 3) / 4) * ((h + 3) / 4) * 16;
        w = std::max(1, w / 2), h = std::max(1, h / 2);
    }
    const size_t bytes = size_t((w + 3) / 4) * ((h + 3) / 4) * 16;
    if (buf.size() < off + bytes) return false;
    d->w = w, d->h = h;
    d->mip0.assign(buf.begin() + off, buf.begin() + off + bytes);
    return true;
}

bool LoadDds(const char* path, Dds* d) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    std::vector<uint8_t> buf;
    uint8_t tmp[65536];
    size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0) buf.insert(buf.end(), tmp, tmp + n);
    fclose(f);
    if (buf.size() < 148 || memcmp(buf.data(), "DDS ", 4) != 0) return false;
    memcpy(&d->h, buf.data() + 12, 4);
    memcpy(&d->w, buf.data() + 16, 4);
    memcpy(&d->format, buf.data() + 128, 4);  // DX10 header (all tire textures use it)
    const size_t bytes = size_t((d->w + 3) / 4) * ((d->h + 3) / 4) * 16;
    if (buf.size() < 148 + bytes) return false;
    d->mip0.assign(buf.begin() + 148, buf.begin() + 148 + bytes);
    return true;
}

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

void DecodeColor4(const uint8_t* b, int* rgb48) {  // BC3 colour: always 4-colour mode
    const uint16_t c0 = uint16_t(b[0] | b[1] << 8), c1 = uint16_t(b[2] | b[3] << 8);
    int c[4][3];
    auto un = [](uint16_t v, int* o) {
        o[0] = ((v >> 11) & 31) * 255 / 31;
        o[1] = ((v >> 5) & 63) * 255 / 63;
        o[2] = (v & 31) * 255 / 31;
    };
    un(c0, c[0]);
    un(c1, c[1]);
    for (int k = 0; k < 3; ++k) {
        c[2][k] = (2 * c[0][k] + c[1][k]) / 3;
        c[3][k] = (c[0][k] + 2 * c[1][k]) / 3;
    }
    const uint32_t idx = uint32_t(b[4] | b[5] << 8 | b[6] << 16 | uint32_t(b[7]) << 24);
    for (int p = 0; p < 16; ++p)
        for (int k = 0; k < 3; ++k) rgb48[p * 3 + k] = c[(idx >> (2 * p)) & 3][k];
}

struct Image {
    int w = 0, h = 0;
    std::vector<float> px;  // rgb 0..1
    float* at(int x, int y) { return &px[(size_t(y) * w + x) * 3]; }
};

// Decodes both textures and lights the result with one side light, so the
// relief of the grooves is visible to the eye.
Image Shade(const Dds& col, const Dds& nrm) {
    Image img;
    img.w = col.w;
    img.h = col.h;
    img.px.assign(size_t(img.w) * img.h * 3, 0.f);
    std::vector<int> rgb(size_t(col.w) * col.h * 3), alpha(size_t(col.w) * col.h), nx(alpha.size()), ny(alpha.size());
    const int bw = col.w / 4;
    for (int by = 0; by < col.h / 4; ++by)
        for (int bx = 0; bx < bw; ++bx) {
            const uint8_t* cb = &col.mip0[(size_t(by) * bw + bx) * 16];
            const uint8_t* nb = &nrm.mip0[(size_t(by) * bw + bx) * 16];
            int a[16], c[48], x[16], y[16];
            DecodeBc4(cb, a);
            DecodeColor4(cb + 8, c);
            DecodeBc4(nb, x);
            DecodeBc4(nb + 8, y);
            for (int p = 0; p < 16; ++p) {
                const size_t i = size_t(by * 4 + p / 4) * col.w + bx * 4 + p % 4;
                alpha[i] = a[p];
                nx[i] = x[p];
                ny[i] = y[p];
                for (int k = 0; k < 3; ++k) rgb[i * 3 + k] = c[p * 3 + k];
            }
        }
    const float L[3] = {-0.45f, -0.55f, 0.70f};
    const float ll = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
    const float l[3] = {L[0] / ll, L[1] / ll, L[2] / ll};
    float hv[3] = {l[0], l[1], l[2] + 1.f};
    const float hl = std::sqrt(hv[0] * hv[0] + hv[1] * hv[1] + hv[2] * hv[2]);
    for (float& v : hv) v /= hl;
    for (size_t i = 0; i < alpha.size(); ++i) {
        const float x = nx[i] / 255.f * 2.f - 1.f, y = ny[i] / 255.f * 2.f - 1.f;
        const float z = std::sqrt(std::max(0.f, 1.f - x * x - y * y));
        const float diff = std::max(0.f, x * l[0] + y * l[1] + z * l[2]);
        const float spec = std::pow(std::max(0.f, x * hv[0] + y * hv[1] + z * hv[2]), 24.f) * alpha[i] / 255.f;
        for (int k = 0; k < 3; ++k) {
            const float lin = std::pow(rgb[i * 3 + k] / 255.f, 2.2f);
            const float out = (lin * (0.15f + 1.6f * diff) + 0.35f * spec) * 3.0f;  // exposure up: tires are dark
            img.px[i * 3 + k] = std::pow(std::min(1.f, out), 1.f / 2.2f);
        }
    }
    return img;
}

bool WriteBmp(const std::string& path, Image& img) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    const int row = (img.w * 3 + 3) & ~3;
    const uint32_t size = 54 + row * img.h, off = 54, dib = 40, planes_bpp = 1 | (24 << 16), data = row * img.h;
    uint8_t hdr[54] = {'B', 'M'};
    memcpy(hdr + 2, &size, 4);
    memcpy(hdr + 10, &off, 4);
    memcpy(hdr + 14, &dib, 4);
    memcpy(hdr + 18, &img.w, 4);
    memcpy(hdr + 22, &img.h, 4);
    memcpy(hdr + 26, &planes_bpp, 4);
    memcpy(hdr + 34, &data, 4);
    fwrite(hdr, 1, 54, f);
    std::vector<uint8_t> line(row, 0);
    for (int y = img.h - 1; y >= 0; --y) {
        for (int x = 0; x < img.w; ++x) {
            const float* p = img.at(x, y);
            for (int k = 0; k < 3; ++k) line[x * 3 + (2 - k)] = static_cast<uint8_t>(std::clamp(p[k], 0.f, 1.f) * 255.f);
        }
        fwrite(line.data(), 1, row, f);
    }
    fclose(f);
    return true;
}

// Tread = the rectangle of strong relief in the middle of the normal map
// (the sidewall ring around it has relief too, so search outward from the
// centre and stop at the first gap).
wear::Rect DetectTread(const Dds& nrm) {
    const int w = nrm.w, h = nrm.h, bw = w / 4;
    std::vector<int> dev(size_t(w) * h);
    for (int by = 0; by < h / 4; ++by)
        for (int bx = 0; bx < bw; ++bx) {
            int x[16], y[16];
            const uint8_t* nb = &nrm.mip0[(size_t(by) * bw + bx) * 16];
            DecodeBc4(nb, x);
            DecodeBc4(nb + 8, y);
            for (int p = 0; p < 16; ++p)
                dev[size_t(by * 4 + p / 4) * w + bx * 4 + p % 4] = std::abs(x[p] - 128) + std::abs(y[p] - 128);
        }
    auto col_frac = [&](int x, int y0, int y1) {
        int c = 0;
        for (int y = y0; y < y1; ++y) c += dev[size_t(y) * w + x] > 20;
        return float(c) / (y1 - y0);
    };
    auto row_frac = [&](int y, int x0, int x1) {
        int c = 0;
        for (int x = x0; x < x1; ++x) c += dev[size_t(y) * w + x] > 20;
        return float(c) / (x1 - x0);
    };
    // Scan outward from the centre; a run of `max_gap` empty lines ends the
    // tread. Grooves can be far apart across the tread (straight-rib tires),
    // so columns allow a wide gap; rows are continuous along the tread and the
    // sidewall ring is close above and below it, so rows allow a small one.
    const int y0 = int(h * 0.40f), y1 = int(h * 0.60f);
    auto scan = [](int from, int to, int step, int max_gap, auto frac) {
        int last = from, gap = 0;
        for (int i = from; i != to; i += step) {
            if (frac(i) > 0.08f) {
                last = i;
                gap = 0;
            } else if (++gap >= max_gap) {
                break;
            }
        }
        return last;
    };
    const int col_gap = w / 16, row_gap = std::max(6, h / 85);
    const int xl = scan(w / 2, 0, -1, col_gap, [&](int x) { return col_frac(x, y0, y1); });
    const int xr = scan(w / 2, w, +1, col_gap, [&](int x) { return col_frac(x, y0, y1); });
    const int yt = scan(h / 2, 0, -1, row_gap, [&](int y) { return row_frac(y, xl, xr + 1); });
    const int yb = scan(h / 2, h, +1, row_gap, [&](int y) { return row_frac(y, xl, xr + 1); });
    return wear::Rect{float(xl) / w, float(yt) / h, float(xr + 1) / w, float(yb + 1) / h};
}

// SCS tire textures all put the tread in a centred rectangle of about the
// same size. A detection that leaked into the sidewall ring or the metal hub
// drawn around the tread (newer Michelin textures) is replaced by the median
// rectangle of the good detections [verified: contact sheet, 2026-10-19].
const wear::Rect kDefaultTread{0.36f, 0.28f, 0.64f, 0.72f};

bool Plausible(const wear::Rect& r) {
    const float w = r.u1 - r.u0, h = r.v1 - r.v0, cu = (r.u0 + r.u1) / 2, cv = (r.v0 + r.v1) / 2;
    return w > 0.2f && w < 0.36f && h > 0.35f && h < 0.52f && std::fabs(cu - 0.5f) < 0.03f &&
           std::fabs(cv - 0.5f) < 0.03f;
}

// R5c: the tread is a tile pasted into the texture with sharp edges. Moves
// each side of a rough rectangle (within +-0.06 UV) to the line with the
// strongest brightness step along it, measured over the middle half of that
// side, on the shaded image. Used for the `fallback` rectangles, whose
// default leaves the shoulders out and takes in a bit of the hub on the
// newer Michelin textures [verified: R5c preview f_385-55_michelin_xmult_f].
wear::Rect SnapRect(const Image& img, wear::Rect r) {
    const int w = img.w, h = img.h;
    auto lum = [&](int x, int y) {
        const float* p = const_cast<Image&>(img).at(std::clamp(x, 0, w - 1), std::clamp(y, 0, h - 1));
        return 0.3f * p[0] + 0.59f * p[1] + 0.11f * p[2];
    };
    const int ya = int((r.v0 + (r.v1 - r.v0) * 0.25f) * h), yb = int((r.v0 + (r.v1 - r.v0) * 0.75f) * h);
    const int xa = int((r.u0 + (r.u1 - r.u0) * 0.25f) * w), xb = int((r.u0 + (r.u1 - r.u0) * 0.75f) * w);
    auto col_step = [&](int x) {
        float s = 0;
        for (int y = ya; y < yb; ++y) s += std::fabs(lum(x, y) - lum(x - 1, y));
        return s / (yb - ya);
    };
    auto row_step = [&](int y) {
        float s = 0;
        for (int x = xa; x < xb; ++x) s += std::fabs(lum(x, y) - lum(x, y - 1));
        return s / (xb - xa);
    };
    auto best = [](float centre, int size, auto step) {
        const int lo = std::max(1, int((centre - 0.06f) * size)), hi = std::min(size - 1, int((centre + 0.06f) * size));
        int bi = int(centre * size);
        float bv = -1;
        for (int i = lo; i <= hi; ++i)
            if (step(i) > bv) bv = step(i), bi = i;
        return float(bi) / size;
    };
    wear::Rect s;
    s.u0 = best(r.u0, w, col_step);
    s.u1 = best(r.u1, w, col_step);
    s.v0 = best(r.v0, h, row_step);
    s.v1 = best(r.v1, h, row_step);
    return s;
}

// Unshaded tread brightness (0..255 luma of the colour texture), pixel by pixel.
std::vector<float> TreadLuma(const Dds& col, const wear::Rect& r) {
    std::vector<float> out;
    const int bw = col.w / 4;
    for (int by = 0; by < col.h / 4; ++by)
        for (int bx = 0; bx < bw; ++bx) {
            const float u = (bx * 4 + 2.f) / col.w, v = (by * 4 + 2.f) / col.h;
            if (u <= r.u0 || u >= r.u1 || v <= r.v0 || v >= r.v1) continue;
            int c[48];
            DecodeColor4(&col.mip0[(size_t(by) * bw + bx) * 16] + 8, c);
            for (int p = 0; p < 16; ++p) out.push_back(0.2126f * c[p * 3] + 0.7152f * c[p * 3 + 1] + 0.0722f * c[p * 3 + 2]);
        }
    return out;
}

}  // namespace

// "mips" mode: like the game, every mip is transformed on its own; shows the
// tread of mips 0-3 at wear 0 and 100%, each enlarged (nearest) to mip 0's
// size, so artefacts of the small mips are visible.
int MipsMode(const char* cpath, const char* npath, const std::string& pre, const wear::Rect& r) {
    const int kMips = 4;
    Dds c0;
    if (!LoadDdsMip(cpath, 0, &c0)) return 1;
    const int rx0 = int(r.u0 * c0.w), ry0 = int(r.v0 * c0.h), rx1 = int(r.u1 * c0.w), ry1 = int(r.v1 * c0.h);
    const int tw = rx1 - rx0, th = ry1 - ry0, gap = 8;
    Image out;
    out.w = tw * kMips + gap * (kMips - 1);
    out.h = th * 2 + gap;
    out.px.assign(size_t(out.w) * out.h * 3, 1.f);
    for (int row = 0; row < 2; ++row)
        for (int m = 0; m < kMips; ++m) {
            Dds c, n;
            if (!LoadDdsMip(cpath, m, &c) || !LoadDdsMip(npath, m, &n)) return 1;
            const wear::Params params;
            if (row == 0) {
                const wear::ColorAnalysis an = wear::AnalyzeColor(c.format, c.mip0.data(), c.w, c.h, r, params);
                printf("MIP %d %dpx blocks %d groove_t %.1f top %.1f fence %.1f stripes %d mean %.1f %.1f %.1f\n", m,
                       c.w, an.blocks, an.groove_t, an.top_l, an.fence, an.stripe_columns, an.mean_rgb[0],
                       an.mean_rgb[1], an.mean_rgb[2]);
            }
            // WP_ONLY=c / n: transform only the colour / only the normal map (diagnosis).
            const char* only = getenv("WP_ONLY");
            if (!only || *only != 'n') wear::Apply(c.format, c.mip0.data(), c.w, c.h, r, row ? 1.f : 0.f, params);
            if (!only || *only != 'c') wear::Apply(n.format, n.mip0.data(), n.w, n.h, r, row ? 1.f : 0.f, params);
            if (row == 1 && m == 1 && getenv("WP_DUMP")) {
                // Diagnosis: blocks of the worn mip whose darkest pixel is still well below the top.
                Dds orig;
                LoadDdsMip(cpath, m, &orig);
                const wear::ColorAnalysis an = wear::AnalyzeColor(orig.format, orig.mip0.data(), orig.w, orig.h, r, params);
                int shown = 0;
                for (int by = 0; by < c.h / 4 && shown < 25; ++by)
                    for (int bx = 0; bx < c.w / 4 && shown < 25; ++bx) {
                        const float u = (bx * 4 + 2.f) / c.w, v = (by * 4 + 2.f) / c.h;
                        if (u <= r.u0 + 0.02f || u >= r.u1 - 0.02f || v <= r.v0 || v >= r.v1) continue;
                        const uint8_t* nb = &c.mip0[(size_t(by) * (c.w / 4) + bx) * 16];
                        const uint8_t* ob = &orig.mip0[(size_t(by) * (c.w / 4) + bx) * 16];
                        int px[48], po[48];
                        DecodeColor4(nb + 8, px);
                        DecodeColor4(ob + 8, po);
                        float mn = 255, omn = 255;
                        for (int p = 0; p < 16; ++p) {
                            mn = std::min(mn, 0.2126f * px[p * 3] + 0.7152f * px[p * 3 + 1] + 0.0722f * px[p * 3 + 2]);
                            omn = std::min(omn, 0.2126f * po[p * 3] + 0.7152f * po[p * 3 + 1] + 0.0722f * po[p * 3 + 2]);
                        }
                        if (mn > an.top_l * 0.75f) continue;
                        auto ep = [](const uint8_t* b, int e) {
                            const uint16_t v = uint16_t(b[8 + e * 2] | b[9 + e * 2] << 8);
                            return std::to_string((v >> 11) & 31) + "/" + std::to_string((v >> 5) & 63) + "/" +
                                   std::to_string(v & 31);
                        };
                        printf("DARK block %d,%d min %.0f (orig %.0f) endpoints orig %s %s new %s %s idx %02x%02x%02x%02x\n",
                               bx, by, mn, omn, ep(ob, 0).c_str(), ep(ob, 1).c_str(), ep(nb, 0).c_str(),
                               ep(nb, 1).c_str(), nb[12], nb[13], nb[14], nb[15]);
                        ++shown;
                    }
            }
            Image img = Shade(c, n);
            // Bilinear, as the game's texture filtering: a nearest-pixel
            // zoom made the small mips look blocky, which they are not on
            // screen [user, 2026-10-20].
            const float sc = 1.f / float(1 << m);
            for (int y = 0; y < th; ++y)
                for (int x = 0; x < tw; ++x) {
                    const float fx = std::max(0.f, (rx0 + x + 0.5f) * sc - 0.5f), fy = std::max(0.f, (ry0 + y + 0.5f) * sc - 0.5f);
                    const int x0 = std::min(img.w - 1, int(fx)), y0 = std::min(img.h - 1, int(fy));
                    const int x1 = std::min(img.w - 1, x0 + 1), y1 = std::min(img.h - 1, y0 + 1);
                    const float ax = fx - x0, ay = fy - y0;
                    float* o = out.at(m * (tw + gap) + x, row * (th + gap) + y);
                    for (int k = 0; k < 3; ++k)
                        o[k] = (img.at(x0, y0)[k] * (1 - ax) + img.at(x1, y0)[k] * ax) * (1 - ay) +
                               (img.at(x0, y1)[k] * (1 - ax) + img.at(x1, y1)[k] * ax) * ay;
                }
        }
    WriteBmp(pre + "_mips.bmp", out);

    // The same tread at 100% wear at its true size on screen for each mip
    // (mip 1 = half size = about twice as far away, ...), not enlarged:
    // how the small mips are actually seen [user, 2026-10-20].
    {
        Image dist;
        dist.w = tw + tw / 2 + tw / 4 + tw / 8 + gap * 3;
        dist.h = th;
        dist.px.assign(size_t(dist.w) * dist.h * 3, 1.f);
        int ox = 0;
        for (int m = 0; m < kMips; ++m) {
            Dds c, n;
            if (!LoadDdsMip(cpath, m, &c) || !LoadDdsMip(npath, m, &n)) return 1;
            const wear::Params params;
            wear::Apply(c.format, c.mip0.data(), c.w, c.h, r, 1.f, params);
            wear::Apply(n.format, n.mip0.data(), n.w, n.h, r, 1.f, params);
            Image img = Shade(c, n);
            const int w = tw >> m, h = th >> m, x0 = rx0 >> m, y0 = ry0 >> m;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    memcpy(dist.at(ox + x, y), img.at(std::min(img.w - 1, x0 + x), std::min(img.h - 1, y0 + y)), 12);
            ox += w + gap;
        }
        WriteBmp(pre + "_distance.bmp", dist);
    }
    printf("wrote %s_mips\n", pre.c_str());
    return 0;
}

// "bench" mode: the plugin's work for one texture pair: levels 1..7 of
// every mip of colour and normal map; prints the time.
int BenchMode(const char* cpath, const char* npath, const wear::Rect& r) {
    std::vector<Dds> cm, nm;
    for (int m = 0;; ++m) {
        Dds c, n;
        if (!LoadDdsMip(cpath, m, &c) || !LoadDdsMip(npath, m, &n) || c.w < 4) break;
        cm.push_back(c), nm.push_back(n);
        if (c.w == 4) break;
    }
    const auto t0 = std::chrono::steady_clock::now();
    double col_ms = 0;
    for (int level = 1; level < wear::kDefaultLevels; ++level)
        for (size_t m = 0; m < cm.size(); ++m) {
            Dds c = cm[m], n = nm[m];
            const auto a = std::chrono::steady_clock::now();
            wear::Apply(c.format, c.mip0.data(), c.w, c.h, r, wear::LevelWear(level, wear::kDefaultLevels), wear::Params());
            col_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
            wear::Apply(n.format, n.mip0.data(), n.w, n.h, r, wear::LevelWear(level, wear::kDefaultLevels), wear::Params());
        }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    printf("BENCH %dpx %zu mips, 7 levels: %.0f ms (colour %.0f ms, normal %.0f ms)\n", cm[0].w, cm.size(), ms, col_ms,
           ms - col_ms);
    return 0;
}

// "burst" mode (E4b): the blown, shredded tire next to the new one: tread
// crops (two tiles high, so the seam shows) of the new tire, blown at 0% and
// at 57% wear, then mip 2 of the blown one at its true size (seen from afar).
int BurstMode(const char* cpath, const char* npath, const std::string& pre, const wear::Rect& r) {
    Dds col, nrm;
    if (!LoadDds(cpath, &col) || !LoadDds(npath, &nrm)) return 1;
    const int rx0 = int(r.u0 * col.w), ry0 = int(r.v0 * col.h), tw = int(r.u1 * col.w) - rx0,
              th = int(r.v1 * col.h) - ry0, gap = 12;
    Image out;
    out.w = tw * 3 + tw / 4 + gap * 3;
    out.h = th * 2;
    out.px.assign(size_t(out.w) * out.h * 3, 1.f);
    const float wears[3] = {0.f, 0.f, wear::LevelWear(4, wear::kDefaultLevels)};
    for (int i = 0; i < 3; ++i) {
        Dds c = col, n = nrm;
        wear::Apply(c.format, c.mip0.data(), c.w, c.h, r, wears[i], wear::Params());
        wear::Apply(n.format, n.mip0.data(), n.w, n.h, r, wears[i], wear::Params());
        if (i > 0) {
            wear::ApplyBurst(c.format, c.mip0.data(), c.w, c.h, r);
            wear::ApplyBurst(n.format, n.mip0.data(), n.w, n.h, r);
        }
        Image img = Shade(c, n);
        for (int rep = 0; rep < 2; ++rep)
            for (int y = 0; y < th; ++y)
                for (int x = 0; x < tw; ++x) memcpy(out.at(i * (tw + gap) + x, rep * th + y), img.at(rx0 + x, ry0 + y), 12);
    }
    // Mip 2, transformed on its own like the game does, at its own size.
    Dds c, n;
    if (LoadDdsMip(cpath, 2, &c) && LoadDdsMip(npath, 2, &n)) {
        wear::ApplyBurst(c.format, c.mip0.data(), c.w, c.h, r);
        wear::ApplyBurst(n.format, n.mip0.data(), n.w, n.h, r);
        Image img = Shade(c, n);
        const int w = tw / 4, h = th / 4, x0 = rx0 / 4, y0 = ry0 / 4;
        for (int rep = 0; rep < 2; ++rep)
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    memcpy(out.at(3 * (tw + gap) + x, rep * h + y),
                           img.at(std::min(img.w - 1, x0 + x), std::min(img.h - 1, y0 + y)), 12);
    }
    WriteBmp(pre + "_burst.bmp", out);

    // The whole texture (half size) with a small, a mid and a big sidewall
    // gash at different places, as drawn at three blowouts.
    {
        const float sizes[3] = {0.1f, 0.5f, 1.f}, angles[3] = {0.62f, 0.9f, 0.3f};
        Image side;
        side.w = (col.w / 2) * 3 + gap * 2;
        side.h = col.h / 2;
        side.px.assign(size_t(side.w) * side.h * 3, 1.f);
        for (int i = 0; i < 3; ++i) {
            Dds c2 = col, n2 = nrm;
            wear::BurstParams bp;
            bp.side_size = sizes[i], bp.side_angle = angles[i];
            wear::ApplyBurst(c2.format, c2.mip0.data(), c2.w, c2.h, r, bp);
            wear::ApplyBurst(n2.format, n2.mip0.data(), n2.w, n2.h, r, bp);
            Image img = Shade(c2, n2);
            if (getenv("WP_RAW")) {  // diagnosis: the colour texture alone, unlit
                const int bw = c2.w / 4;
                for (int by = 0; by < c2.h / 4; ++by)
                    for (int bx = 0; bx < bw; ++bx) {
                        int cc[48];
                        DecodeColor4(&c2.mip0[(size_t(by) * bw + bx) * 16] + 8, cc);
                        for (int q = 0; q < 16; ++q)
                            for (int k = 0; k < 3; ++k) img.at(bx * 4 + q % 4, by * 4 + q / 4)[k] = cc[q * 3 + k] / 255.f;
                    }
            }
            for (int y = 0; y < side.h; ++y)
                for (int x = 0; x < col.w / 2; ++x) {
                    float* o = side.at(i * (col.w / 2 + gap) + x, y);
                    for (int k = 0; k < 3; ++k)
                        o[k] = 0.25f * (img.at(2 * x, 2 * y)[k] + img.at(2 * x + 1, 2 * y)[k] + img.at(2 * x, 2 * y + 1)[k] +
                                        img.at(2 * x + 1, 2 * y + 1)[k]);
                }
        }
        WriteBmp(pre + "_side.bmp", side);
    }

    // Cost: what the plugin does once per blowout for this pair: every mip of
    // colour and normal map.
    {
        std::vector<Dds> cm, nm;
        for (int m = 0;; ++m) {
            Dds c3, n3;
            if (!LoadDdsMip(cpath, m, &c3) || !LoadDdsMip(npath, m, &n3) || c3.w < 4) break;
            cm.push_back(c3), nm.push_back(n3);
            if (c3.w == 4) break;
        }
        const auto t0 = std::chrono::steady_clock::now();
        for (size_t m = 0; m < cm.size(); ++m) {
            wear::ApplyBurst(cm[m].format, cm[m].mip0.data(), cm[m].w, cm[m].h, r);
            wear::ApplyBurst(nm[m].format, nm[m].mip0.data(), nm[m].w, nm[m].h, r);
        }
        printf("BURST COST %dpx %zu mips: %.0f ms\n", col.w, cm.size(),
               std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    printf("wrote %s_burst / _side\n", pre.c_str());
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 9 && !strcmp(argv[8], "burst"))
        return BurstMode(argv[1], argv[2], argv[3],
                         wear::Rect{float(atof(argv[4])), float(atof(argv[5])), float(atof(argv[6])), float(atof(argv[7]))});
    if (argc == 9 && !strcmp(argv[8], "bench"))
        return BenchMode(argv[1], argv[2],
                         wear::Rect{float(atof(argv[4])), float(atof(argv[5])), float(atof(argv[6])), float(atof(argv[7]))});
    if (argc == 9 && !strcmp(argv[8], "mips"))
        return MipsMode(argv[1], argv[2], argv[3],
                        wear::Rect{float(atof(argv[4])), float(atof(argv[5])), float(atof(argv[6])), float(atof(argv[7]))});
    // "snap" after the rectangle: move its sides to the tile's edges (R5c).
    const bool snap = argc == 9 && !strcmp(argv[8], "snap");
    if (argc != 4 && argc != 8 && !snap) {
        fprintf(stderr, "usage: wearpreview <colspec.dds> <norm.dds> <out_prefix> [u0 v0 u1 v1 [snap]]\n");
        return 2;
    }
    Dds col, nrm;
    if (!LoadDds(argv[1], &col) || !LoadDds(argv[2], &nrm) || col.w != nrm.w || col.h != nrm.h) {
        fprintf(stderr, "cannot load the pair\n");
        return 1;
    }
    const std::string pre = argv[3];
    const char* how = "manual";
    wear::Rect r;
    if (argc >= 8) {
        r = wear::Rect{float(atof(argv[4])), float(atof(argv[5])), float(atof(argv[6])), float(atof(argv[7]))};
        if (snap) {
            r = SnapRect(Shade(col, nrm), r);
            how = "snapped";
        }
    } else {
        r = DetectTread(nrm);
        how = "auto";
        if (!Plausible(r)) {
            printf("detected %.3f %.3f %.3f %.3f is implausible, using the default\n", r.u0, r.v0, r.u1, r.v1);
            r = kDefaultTread;
            how = "fallback";
        }
    }
    printf("RECT %s %.3f %.3f %.3f %.3f px %d,%d-%d,%d %s size %d\n", argv[1], r.u0, r.v0, r.u1, r.v1, int(r.u0 * col.w),
           int(r.v0 * col.h), int(r.u1 * col.w), int(r.v1 * col.h), how, col.w);

    // Outline of the rectangle on the untouched texture.
    Image base = Shade(col, nrm);
    const int rx0 = int(r.u0 * base.w), ry0 = int(r.v0 * base.h), rx1 = int(r.u1 * base.w) - 1, ry1 = int(r.v1 * base.h) - 1;
    for (int x = rx0; x <= rx1; ++x)
        for (int y : {ry0, ry1}) {
            float* p = base.at(x, y);
            p[0] = 0.f, p[1] = 1.f, p[2] = 0.f;
        }
    for (int y = ry0; y <= ry1; ++y)
        for (int x : {rx0, rx1}) {
            float* p = base.at(x, y);
            p[0] = 0.f, p[1] = 1.f, p[2] = 0.f;
        }
    // Detected new-tire stripe columns (R5c): red ticks above and below the rectangle.
    {
        std::vector<float> stripe;
        wear::AnalyzeColor(col.format, col.mip0.data(), col.w, col.h, r, wear::Params(), &stripe);
        for (size_t bx = 0; bx < stripe.size(); ++bx)
            if (stripe[bx] != wear::kNoStripe)
                for (int x = int(bx) * 4; x < int(bx) * 4 + 4; ++x)
                    for (int d = 2; d < 14; ++d)
                        for (int y : {ry0 - d, ry1 + d}) {
                            if (y < 0 || y >= base.h) continue;
                            float* q = base.at(x, y);
                            q[0] = 1.f, q[1] = 0.f, q[2] = 0.f;
                        }
    }
    WriteBmp(pre + "_rect.bmp", base);

    // Tread crops (with a margin) for each level, side by side.
    const wear::Params params;
    const int levels = wear::kDefaultLevels;
    const int m = col.w / 32;
    const int cx0 = std::max(0, rx0 - m), cy0 = std::max(0, ry0 - m), cx1 = std::min(col.w, rx1 + 1 + m),
              cy1 = std::min(col.h, ry1 + 1 + m);
    const int cw = cx1 - cx0, ch = cy1 - cy0, gap = 8;
    Image strip;
    strip.w = cw * levels + gap * (levels - 1);
    strip.h = ch;
    strip.px.assign(size_t(strip.w) * strip.h * 3, 1.f);
    for (int li = 0; li < levels; ++li) {
        const float w = wear::LevelWear(li, levels);
        Dds c = col, n = nrm;
        wear::Apply(c.format, c.mip0.data(), c.w, c.h, r, w, params);
        wear::Apply(n.format, n.mip0.data(), n.w, n.h, r, w, params);
        Image img = Shade(c, n);
        for (int y = 0; y < ch; ++y)
            for (int x = 0; x < cw; ++x) memcpy(strip.at(li * (cw + gap) + x, y), img.at(cx0 + x, cy0 + y), 12);
        if (li == levels - 1) WriteBmp(pre + "_full100.bmp", img);
    }
    WriteBmp(pre + "_levels.bmp", strip);

    // R5d: R5c2 look (top row) x current look (bottom row) at 0, 29, 57, 86 and
    // 100%; each cell is the tread tile stacked twice, so the seam between two
    // repetitions (where the tile meets itself around the tire) is in the
    // middle of the cell.
    {
        const int tw = rx1 + 1 - rx0, th = ry1 + 1 - ry0, cg = 12;
        const int pick[5] = {0, 2, 4, 6, 7};
        Image cmp;
        cmp.w = tw * 5 + cg * 4;
        cmp.h = th * 4 + cg;
        cmp.px.assign(size_t(cmp.w) * cmp.h * 3, 1.f);
        for (int row = 0; row < 2; ++row) {
            const wear::Params pr = row == 0 ? wear::Params::R5c2() : wear::Params();
            for (int i = 0; i < 5; ++i) {
                const float w = wear::LevelWear(pick[i], levels);
                Dds c = col, n = nrm;
                wear::Apply(c.format, c.mip0.data(), c.w, c.h, r, w, pr);
                wear::Apply(n.format, n.mip0.data(), n.w, n.h, r, w, pr);
                Image img = Shade(c, n);
                for (int rep = 0; rep < 2; ++rep)
                    for (int y = 0; y < th; ++y)
                        for (int x = 0; x < tw; ++x)
                            memcpy(cmp.at(i * (tw + cg) + x, row * (2 * th + cg) + rep * th + y), img.at(rx0 + x, ry0 + y),
                                   12);
            }
        }
        WriteBmp(pre + "_compare.bmp", cmp);
    }
    // R5c: groove / top brightness per level (unshaded), split by the same
    // threshold the transform uses on the untouched tread.
    {
        const std::vector<float> l0 = TreadLuma(col, r);
        const wear::ColorAnalysis an = wear::AnalyzeColor(col.format, col.mip0.data(), col.w, col.h, r, params);
        const float t = an.groove_t;
        std::vector<float> sorted = l0;
        std::sort(sorted.begin(), sorted.end());
        printf("ANALYSIS fence %.1f top %.1f stripe_columns %d\n", an.fence, an.top_l, an.stripe_columns);
        printf("STATS t %.1f p10 %.1f p50 %.1f p75 %.1f p90 %.1f groove %.0f%%\n", t, sorted[sorted.size() / 10],
               sorted[sorted.size() / 2], sorted[sorted.size() * 3 / 4], sorted[sorted.size() * 9 / 10],
               100.0 * (std::lower_bound(sorted.begin(), sorted.end(), t) - sorted.begin()) / sorted.size());
        for (int li = 0; li < levels; ++li) {
            Dds c = col, n = nrm;
            wear::Apply(c.format, c.mip0.data(), c.w, c.h, r, wear::LevelWear(li, levels), params);
            wear::Apply(n.format, n.mip0.data(), n.w, n.h, r, wear::LevelWear(li, levels), params);
            const std::vector<float> l = TreadLuma(c, r);
            // Same pixels on the shaded image (light + relief), x 255.
            Image img = Shade(c, n);
            std::vector<float> sl;
            for (int by = 0; by < c.h / 4; ++by)
                for (int bx = 0; bx < c.w / 4; ++bx) {
                    const float u = (bx * 4 + 2.f) / c.w, v = (by * 4 + 2.f) / c.h;
                    if (u <= r.u0 || u >= r.u1 || v <= r.v0 || v >= r.v1) continue;
                    for (int p = 0; p < 16; ++p) {
                        const float* q = img.at(bx * 4 + p % 4, by * 4 + p / 4);
                        sl.push_back(255.f * (0.2126f * q[0] + 0.7152f * q[1] + 0.0722f * q[2]));
                    }
                }
            double sg = 0, st = 0, ng = 0, nt = 0, hg = 0, ht = 0;
            for (size_t i = 0; i < l.size(); ++i)
                if (l0[i] < t)
                    sg += l[i], hg += sl[i], ng += 1;
                else if (l0[i] <= an.fence)
                    st += l[i], ht += sl[i], nt += 1;
            printf("LEVEL %d groove %.1f top %.1f | shaded groove %.1f top %.1f\n", li, ng ? sg / ng : 0,
                   nt ? st / nt : 0, ng ? hg / ng : 0, nt ? ht / nt : 0);
        }
    }
    printf("wrote %s_rect / _levels / _full100 / _compare\n", pre.c_str());
    return 0;
}
