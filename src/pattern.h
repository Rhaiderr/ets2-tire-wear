#pragma once

// Byte patterns in the game's executable (read only): "48 8B ?? 05", where
// ?? is any byte. Used to find game functions that have no export (T3).

#include <cstdint>
#include <string>
#include <vector>

namespace pattern {

// The game's .text section in memory; size 0 when not found.
struct Range {
    const uint8_t* base = nullptr;   // module base (exe+0)
    const uint8_t* begin = nullptr;  // .text start
    size_t size = 0;
};
Range GameText();

// Start of every match, stopping at 2 (only "exactly one" matters).
std::vector<const uint8_t*> Find(const Range& r, const char* text);

// rel32 at `at`, resolved against the next instruction at `next`.
const uint8_t* Rel32(const uint8_t* at, const uint8_t* next);

std::string Hex(uintptr_t v);
std::string Count(size_t n);  // "0", "1" or "2+"

}  // namespace pattern
