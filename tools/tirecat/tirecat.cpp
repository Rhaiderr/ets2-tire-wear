// tirecat: builds the tire texture catalog (step R1).
//
// Usage: tirecat <out.csv> <label>=<extracted root> [<label>=<root> ...]
// Walks each root for .dds files under a "wheel\tire" folder and writes one
// CSV row per texture: size, DXGI format, mip count and a 64-bit FNV-1a hash
// of every mip level. The plugin will compare these hashes with the data the
// game passes to ID3D11Device::CreateTexture2D to recognise tire textures.

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

#pragma pack(push, 1)
struct DdsPixelFormat {
    uint32_t size, flags, fourcc, rgb_bits, r_mask, g_mask, b_mask, a_mask;
};
struct DdsHeader {
    uint32_t size, flags, height, width, pitch, depth, mip_count, reserved1[11];
    DdsPixelFormat pf;
    uint32_t caps, caps2, caps3, caps4, reserved2;
};
struct DdsHeaderDx10 {
    uint32_t dxgi_format, dimension, misc_flag, array_size, misc_flags2;
};
#pragma pack(pop)

constexpr uint32_t FourCC(char a, char b, char c, char d) {
    return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 | uint32_t(uint8_t(d)) << 24;
}

// Bytes per 4x4 block for block-compressed formats, 0 if not BC.
uint32_t BlockBytes(uint32_t f) {
    switch (f) {
        case 70: case 71: case 72:     // BC1
        case 79: case 80: case 81:     // BC4
            return 8;
        case 73: case 74: case 75:     // BC2
        case 76: case 77: case 78:     // BC3
        case 82: case 83: case 84:     // BC5
        case 94: case 95: case 96:     // BC6H
        case 97: case 98: case 99:     // BC7
            return 16;
        default:
            return 0;
    }
}

// Bytes per pixel for the few uncompressed formats we might meet.
uint32_t PixelBytes(uint32_t f) {
    switch (f) {
        case 28: case 29: case 87: case 88: case 91: return 4;  // RGBA8 / BGRA8 variants
        case 61: return 1;                                     // R8
        case 49: return 2;                                     // R8G8
        default: return 0;
    }
}

uint32_t LegacyToDxgi(const DdsPixelFormat& pf) {
    if (pf.flags & 0x4) {  // DDPF_FOURCC
        switch (pf.fourcc) {
            case FourCC('D', 'X', 'T', '1'): return 71;
            case FourCC('D', 'X', 'T', '3'): return 74;
            case FourCC('D', 'X', 'T', '5'): return 77;
            case FourCC('A', 'T', 'I', '2'): case FourCC('B', 'C', '5', 'U'): return 83;
            case FourCC('A', 'T', 'I', '1'): case FourCC('B', 'C', '4', 'U'): return 80;
        }
        return 0;
    }
    if (pf.rgb_bits == 32) return pf.r_mask == 0x00ff0000 ? 87 : 28;
    return 0;
}

uint64_t Fnv1a(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

std::string HexHash(uint64_t h) {
    char b[20];
    snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(h));
    return b;
}

std::string Utf8(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

struct Row {
    std::string source, path, error;
    uint32_t width = 0, height = 0, format = 0, mips = 0;
    uint64_t file_bytes = 0, data_bytes = 0, expected_bytes = 0;
    std::vector<uint64_t> mip_hashes;
};

Row ReadDds(const fs::path& file) {
    Row r;
    std::ifstream in(file, std::ios::binary);
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    r.file_bytes = buf.size();
    if (buf.size() < 4 + sizeof(DdsHeader) || memcmp(buf.data(), "DDS ", 4) != 0) {
        r.error = "not a DDS";
        return r;
    }
    const auto* h = reinterpret_cast<const DdsHeader*>(buf.data() + 4);
    size_t offset = 4 + sizeof(DdsHeader);
    r.width = h->width;
    r.height = h->height;
    r.mips = h->mip_count ? h->mip_count : 1;
    if (h->pf.fourcc == FourCC('D', 'X', '1', '0')) {
        if (buf.size() < offset + sizeof(DdsHeaderDx10)) {
            r.error = "short DX10 header";
            return r;
        }
        r.format = reinterpret_cast<const DdsHeaderDx10*>(buf.data() + offset)->dxgi_format;
        offset += sizeof(DdsHeaderDx10);
    } else {
        r.format = LegacyToDxgi(h->pf);
    }
    r.data_bytes = buf.size() - offset;

    const uint32_t bb = BlockBytes(r.format), pb = PixelBytes(r.format);
    if (!bb && !pb) {
        r.error = "unhandled format";
        return r;
    }
    uint32_t w = r.width, ht = r.height;
    for (uint32_t m = 0; m < r.mips; ++m) {
        const uint64_t size = bb ? uint64_t((w + 3) / 4) * ((ht + 3) / 4) * bb : uint64_t(w) * ht * pb;
        r.expected_bytes += size;
        if (offset + size > buf.size()) {
            r.error = "truncated at mip " + std::to_string(m);
            break;
        }
        r.mip_hashes.push_back(Fnv1a(buf.data() + offset, size));
        offset += size;
        w = w > 1 ? w / 2 : 1;
        ht = ht > 1 ? ht / 2 : 1;
    }
    return r;
}

bool IsTireFolder(const fs::path& p) {
    std::wstring s = p.generic_wstring();
    for (auto& c : s) c = towlower(c);
    return s.find(L"/wheel/tire/") != std::wstring::npos;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        fwprintf(stderr, L"usage: tirecat <out.csv> <label>=<root> [...]\n");
        return 2;
    }
    std::vector<Row> rows;
    for (int i = 2; i < argc; ++i) {
        std::wstring arg = argv[i];
        const size_t eq = arg.find(L'=');
        if (eq == std::wstring::npos) continue;
        const std::wstring label = arg.substr(0, eq);
        const fs::path root = arg.substr(eq + 1);
        for (const auto& e : fs::recursive_directory_iterator(root)) {
            if (!e.is_regular_file() || _wcsicmp(e.path().extension().c_str(), L".dds") != 0) continue;
            if (!IsTireFolder(e.path())) continue;
            Row r = ReadDds(e.path());
            r.source = Utf8(label);
            r.path = Utf8(L"/" + fs::relative(e.path(), root).generic_wstring());
            rows.push_back(std::move(r));
        }
    }

    FILE* out = _wfopen(argv[1], L"wb");
    if (!out) {
        fwprintf(stderr, L"cannot write %s\n", argv[1]);
        return 1;
    }
    fprintf(out, "source,path,width,height,dxgi_format,mips,file_bytes,data_bytes,expected_bytes,hash_mip0,hash_all_mips,error\n");
    std::map<uint64_t, std::vector<std::string>> by_hash;
    int ok = 0;
    for (const auto& r : rows) {
        std::string all;
        for (size_t m = 0; m < r.mip_hashes.size(); ++m) all += (m ? ";" : "") + HexHash(r.mip_hashes[m]);
        fprintf(out, "%s,%s,%u,%u,%u,%u,%llu,%llu,%llu,%s,%s,%s\n", r.source.c_str(), r.path.c_str(), r.width, r.height,
                r.format, r.mips, static_cast<unsigned long long>(r.file_bytes),
                static_cast<unsigned long long>(r.data_bytes), static_cast<unsigned long long>(r.expected_bytes),
                r.mip_hashes.empty() ? "" : HexHash(r.mip_hashes[0]).c_str(), all.c_str(), r.error.c_str());
        if (!r.mip_hashes.empty()) {
            by_hash[r.mip_hashes[0]].push_back(r.source + ":" + r.path);
            ++ok;
        }
    }
    fclose(out);

    printf("textures: %zu, hashed: %d, unique mip0 hashes: %zu\n", rows.size(), ok, by_hash.size());
    for (const auto& [h, names] : by_hash) {
        if (names.size() < 2) continue;
        printf("same mip0 %s:", HexHash(h).c_str());
        for (const auto& n : names) printf(" %s", n.c_str());
        printf("\n");
    }
    for (const auto& r : rows)
        if (!r.error.empty()) printf("error %s:%s: %s\n", r.source.c_str(), r.path.c_str(), r.error.c_str());
    return 0;
}
