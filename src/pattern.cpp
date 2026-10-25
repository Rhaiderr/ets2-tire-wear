#include "pattern.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pattern {

Range GameText() {
    Range r;
    r.base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(r.base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(r.base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
        if (memcmp(sec->Name, ".text", 6) == 0) {
            r.begin = r.base + sec->VirtualAddress;
            r.size = sec->Misc.VirtualSize;
        }
    return r;
}

std::vector<const uint8_t*> Find(const Range& r, const char* text) {
    std::vector<int> bytes;  // -1 = any byte
    for (const char* s = text; *s;) {
        while (*s == ' ') ++s;
        if (!*s) break;
        if (s[0] == '?') {
            bytes.push_back(-1);
        } else {
            const char pair[3] = {s[0], s[1], 0};
            bytes.push_back(static_cast<int>(strtoul(pair, nullptr, 16)));
        }
        s += 2;
    }
    std::vector<const uint8_t*> hits;
    const size_t n = bytes.size();
    for (size_t i = 0; i + n <= r.size && hits.size() < 2; ++i) {
        size_t k = 0;
        while (k < n && (bytes[k] < 0 || r.begin[i + k] == bytes[k])) ++k;
        if (k == n) hits.push_back(r.begin + i);
    }
    return hits;
}

const uint8_t* Rel32(const uint8_t* at, const uint8_t* next) {
    int32_t v;
    memcpy(&v, at, sizeof(v));
    return next + v;
}

std::string Hex(uintptr_t v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(v));
    return buf;
}

std::string Count(size_t n) { return n >= 2 ? std::string("2+") : std::to_string(n); }

}  // namespace pattern
