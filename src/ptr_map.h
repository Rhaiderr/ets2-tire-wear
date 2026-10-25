#pragma once

// Pointer -> int map for the D3D hooks: lock-free lookups (thousands per
// frame, on the render thread), writes under a lock (texture and view
// creation / destruction only).
//
// Open addressing with linear probing. Erased entries become tombstones and
// the table is rebuilt into its spare copy before tombstones or entries can
// fill it, so a lookup always ends at an empty slot within a short chain.
// The first version had no erase: in a long session the game's streamed
// textures filled it, and every miss then walked the whole table at each
// draw (FPS 100 -> 30 after an hour, vega 159).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

template <size_t kSlots>  // power of two
class PtrMap {
public:
    // 0 when the key is absent.
    int Get(const void* key) const {
        if (!key || key == Tombstone()) return 0;
        const Table& t = tables_[active_.load(std::memory_order_acquire)];
        size_t i = Hash(key);
        for (size_t n = 0; n < kSlots; ++n, i = (i + 1) & (kSlots - 1)) {
            const void* cur = t.keys[i].load(std::memory_order_acquire);
            if (cur == key) return t.values[i].load(std::memory_order_relaxed);
            if (!cur) return 0;
        }
        return 0;
    }

    // value 0 erases. False when the table has no room (the entry is not kept).
    bool Set(const void* key, int value) {
        if (!key || key == Tombstone()) return false;
        std::lock_guard<std::mutex> lock(write_);
        if (!value) {
            EraseLocked(key);
            return true;
        }
        Table& t = tables_[active_.load(std::memory_order_relaxed)];
        size_t i = Hash(key), free_slot = kSlots;
        for (size_t n = 0; n < kSlots; ++n, i = (i + 1) & (kSlots - 1)) {
            const void* cur = t.keys[i].load(std::memory_order_relaxed);
            if (cur == key) {
                t.values[i].store(value, std::memory_order_release);
                return true;
            }
            if (cur == Tombstone() && free_slot == kSlots) free_slot = i;
            if (!cur) {
                if (free_slot == kSlots) free_slot = i;
                break;
            }
        }
        if (count_.load(std::memory_order_relaxed) + 1 > kMaxLive || free_slot == kSlots) {
            full_.store(true, std::memory_order_relaxed);
            return false;
        }
        const bool reused_tombstone = t.keys[free_slot].load(std::memory_order_relaxed) == Tombstone();
        t.values[free_slot].store(value, std::memory_order_relaxed);
        t.keys[free_slot].store(key, std::memory_order_release);  // value is visible before the key
        count_.fetch_add(1, std::memory_order_relaxed);
        if (reused_tombstone) --tombstones_;
        if (count_.load(std::memory_order_relaxed) + tombstones_ > kMaxUsed) RebuildLocked();
        return true;
    }

    void Erase(const void* key) { Set(key, 0); }

    int Count() const { return static_cast<int>(count_.load(std::memory_order_relaxed)); }
    int Rebuilds() const { return rebuilds_.load(std::memory_order_relaxed); }
    bool Full() const { return full_.load(std::memory_order_relaxed); }  // an insert was refused

private:
    static constexpr size_t kMaxLive = kSlots / 2;      // live entries allowed
    static constexpr size_t kMaxUsed = kSlots * 3 / 4;  // live + tombstones before a rebuild

    struct Table {
        std::atomic<const void*> keys[kSlots] = {};
        std::atomic<int> values[kSlots] = {};
    };

    static const void* Tombstone() { return reinterpret_cast<const void*>(uintptr_t(1)); }
    static size_t Hash(const void* p) {
        return static_cast<size_t>((reinterpret_cast<uintptr_t>(p) >> 4) * 0x9E3779B97F4A7C15ull) & (kSlots - 1);
    }

    void EraseLocked(const void* key) {
        Table& t = tables_[active_.load(std::memory_order_relaxed)];
        size_t i = Hash(key);
        for (size_t n = 0; n < kSlots; ++n, i = (i + 1) & (kSlots - 1)) {
            const void* cur = t.keys[i].load(std::memory_order_relaxed);
            if (!cur) return;
            if (cur == key) {
                t.values[i].store(0, std::memory_order_release);
                t.keys[i].store(Tombstone(), std::memory_order_release);
                count_.fetch_sub(1, std::memory_order_relaxed);
                ++tombstones_;
                if (count_.load(std::memory_order_relaxed) + tombstones_ > kMaxUsed) RebuildLocked();
                return;
            }
        }
    }

    // Copies the live entries into the spare table, then switches to it. A
    // reader still in the old table finds what it held a moment ago.
    void RebuildLocked() {
        const int from = active_.load(std::memory_order_relaxed), to = 1 - from;
        Table& src = tables_[from];
        Table& dst = tables_[to];
        for (size_t i = 0; i < kSlots; ++i) {
            dst.keys[i].store(nullptr, std::memory_order_relaxed);
            dst.values[i].store(0, std::memory_order_relaxed);
        }
        for (size_t s = 0; s < kSlots; ++s) {
            const void* key = src.keys[s].load(std::memory_order_relaxed);
            if (!key || key == Tombstone()) continue;
            size_t i = Hash(key);
            while (dst.keys[i].load(std::memory_order_relaxed)) i = (i + 1) & (kSlots - 1);
            dst.values[i].store(src.values[s].load(std::memory_order_relaxed), std::memory_order_relaxed);
            dst.keys[i].store(key, std::memory_order_relaxed);
        }
        active_.store(to, std::memory_order_release);
        tombstones_ = 0;
        rebuilds_.fetch_add(1, std::memory_order_relaxed);
    }

    // On the heap: static tables would sit in the DLL's initialised data.
    std::unique_ptr<Table[]> tables_{new Table[2]};
    std::atomic<int> active_{0};
    std::mutex write_;
    std::atomic<size_t> count_{0};
    size_t tombstones_ = 0;  // under write_
    std::atomic<int> rebuilds_{0};
    std::atomic<bool> full_{false};
};
