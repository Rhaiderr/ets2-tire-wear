// ddsview: decodes mip 0 of a BC3 / BC5 DDS to 24-bit BMP files (read-only).
//
// Usage: ddsview <in.dds> <out_prefix>
//   BC3 -> <prefix>_rgb.bmp (colour) and <prefix>_a.bmp (alpha as grey)
//   BC5 -> <prefix>_rgb.bmp (R = X, G = Y, B = 255) and <prefix>_x.bmp / _y.bmp (grey)

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

void DecodeBc4(const uint8_t* b, uint8_t* out16) {
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
    for (int p = 0; p < 16; ++p) out16[p] = static_cast<uint8_t>(pal[(bits >> (3 * p)) & 7]);
}

void DecodeBc1Color(const uint8_t* b, uint8_t* rgb48) {
    const uint16_t c0 = uint16_t(b[0] | b[1] << 8), c1 = uint16_t(b[2] | b[3] << 8);
    int c[4][3];
    auto unpack = [](uint16_t v, int* o) {
        o[0] = ((v >> 11) & 31) * 255 / 31;
        o[1] = ((v >> 5) & 63) * 255 / 63;
        o[2] = (v & 31) * 255 / 31;
    };
    unpack(c0, c[0]);
    unpack(c1, c[1]);
    for (int k = 0; k < 3; ++k) {
        if (c0 > c1) {
            c[2][k] = (2 * c[0][k] + c[1][k]) / 3;
            c[3][k] = (c[0][k] + 2 * c[1][k]) / 3;
        } else {
            c[2][k] = (c[0][k] + c[1][k]) / 2;
            c[3][k] = 0;
        }
    }
    const uint32_t idx = uint32_t(b[4] | b[5] << 8 | b[6] << 16 | uint32_t(b[7]) << 24);
    for (int p = 0; p < 16; ++p)
        for (int k = 0; k < 3; ++k) rgb48[p * 3 + k] = static_cast<uint8_t>(c[(idx >> (2 * p)) & 3][k]);
}

bool WriteBmp(const std::string& path, int w, int h, const std::vector<uint8_t>& rgb) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    const int row = (w * 3 + 3) & ~3;
    const uint32_t size = 54 + row * h;
    uint8_t hdr[54] = {'B', 'M'};
    memcpy(hdr + 2, &size, 4);
    const uint32_t off = 54, dib = 40, planes_bpp = 1 | (24 << 16), img = row * h;
    memcpy(hdr + 10, &off, 4);
    memcpy(hdr + 14, &dib, 4);
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    memcpy(hdr + 26, &planes_bpp, 4);
    memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, 54, f);
    std::vector<uint8_t> line(row, 0);
    for (int y = h - 1; y >= 0; --y) {  // bottom-up
        for (int x = 0; x < w; ++x) {
            const uint8_t* p = &rgb[(size_t(y) * w + x) * 3];
            line[x * 3 + 0] = p[2];
            line[x * 3 + 1] = p[1];
            line[x * 3 + 2] = p[0];
        }
        fwrite(line.data(), 1, row, f);
    }
    fclose(f);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: ddsview <in.dds> <out_prefix>\n");
        return 2;
    }
    FILE* f = fopen(argv[1], "rb");
    if (!f) return 1;
    std::vector<uint8_t> d;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) d.insert(d.end(), buf, buf + n);
    fclose(f);
    if (d.size() < 148 || memcmp(d.data(), "DDS ", 4) != 0) return 1;
    int h, w;
    memcpy(&h, d.data() + 12, 4);
    memcpy(&w, d.data() + 16, 4);
    uint32_t fourcc, fmt = 0;
    memcpy(&fourcc, d.data() + 84, 4);
    size_t off = 128;
    if (fourcc == 0x30315844) {  // "DX10"
        memcpy(&fmt, d.data() + 128, 4);
        off = 148;
    }
    const bool bc5 = fmt == 82 || fmt == 83 || fmt == 84;
    const bool bc3 = fmt == 76 || fmt == 77 || fmt == 78;
    if (!bc3 && !bc5) {
        fprintf(stderr, "format %u not handled\n", fmt);
        return 1;
    }
    std::vector<uint8_t> rgb(size_t(w) * h * 3), a(size_t(w) * h * 3), b2(size_t(w) * h * 3);
    const int bw = (w + 3) / 4, bh = (h + 3) / 4;
    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
            const uint8_t* blk = d.data() + off + (size_t(by) * bw + bx) * 16;
            uint8_t c0[16], c1[16], col[48];
            if (bc3) {
                DecodeBc4(blk, c0);
                DecodeBc1Color(blk + 8, col);
            } else {
                DecodeBc4(blk, c0);
                DecodeBc4(blk + 8, c1);
            }
            for (int p = 0; p < 16; ++p) {
                const int x = bx * 4 + p % 4, y = by * 4 + p / 4;
                if (x >= w || y >= h) continue;
                uint8_t* o = &rgb[(size_t(y) * w + x) * 3];
                uint8_t* oa = &a[(size_t(y) * w + x) * 3];
                uint8_t* ob = &b2[(size_t(y) * w + x) * 3];
                if (bc3) {
                    memcpy(o, col + p * 3, 3);
                    oa[0] = oa[1] = oa[2] = c0[p];
                } else {
                    o[0] = c0[p];
                    o[1] = c1[p];
                    o[2] = 255;
                    oa[0] = oa[1] = oa[2] = c0[p];
                    ob[0] = ob[1] = ob[2] = c1[p];
                }
            }
        }
    }
    const std::string pre = argv[2];
    WriteBmp(pre + "_rgb.bmp", w, h, rgb);
    if (bc3) {
        WriteBmp(pre + "_a.bmp", w, h, a);
    } else {
        WriteBmp(pre + "_x.bmp", w, h, a);
        WriteBmp(pre + "_y.bmp", w, h, b2);
    }
    printf("%s: %dx%d format %u -> %s_*.bmp\n", argv[1], w, h, fmt, pre.c_str());
    return 0;
}
