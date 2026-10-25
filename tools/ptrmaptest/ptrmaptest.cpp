// Offline test of src/ptr_map.h (vega 159): the map must stay correct and
// fast under the game's pattern of textures created and destroyed for hours.
//
// Build (x64 Native Tools prompt):
//   cl /nologo /O2 /EHsc /std:c++17 /I..\..\src ptrmaptest.cpp

#include <chrono>
#include <cstdio>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

#include "ptr_map.h"

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        ++g_failures;
        if (g_failures <= 10) printf("FAIL: %s\n", what);
    }
}

// Pointer-like keys: 16-byte aligned, from a pool so addresses get reused.
const void* Key(uint64_t n) { return reinterpret_cast<const void*>(uintptr_t(0x10000000) + n * 16); }
int ValueFor(const void* k) { return static_cast<int>((reinterpret_cast<uintptr_t>(k) >> 4) & 0x7FFFFFFF) | 1; }

double MissNs(const PtrMap<8192>& m, int rounds) {
    const auto t0 = std::chrono::steady_clock::now();
    int sink = 0;
    for (int i = 0; i < rounds; ++i) sink += m.Get(Key(10'000'000 + i));  // never inserted
    const auto t1 = std::chrono::steady_clock::now();
    if (sink) printf("(unexpected hits %d)\n", sink);
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / rounds;
}

}  // namespace

int main() {
    static PtrMap<8192> m;  // the size of g_srv_map
    std::unordered_map<const void*, int> ref;
    std::mt19937_64 rng(1234);

    const double fresh = MissNs(m, 1'000'000);

    // 1. Churn: ~3,000 live entries, 5 million creates/destroys over a pool
    //    of 200,000 addresses (reused, as the D3D runtime does).
    std::vector<const void*> live;
    for (int op = 0; op < 5'000'000; ++op) {
        const bool create = live.size() < 2000 || (live.size() < 4000 && (rng() & 1));
        if (create) {
            const void* k = Key(rng() % 200'000);
            if (ref.count(k)) continue;
            const int v = ValueFor(k);
            Check(m.Set(k, v), "insert refused below the limit");
            ref[k] = v;
            live.push_back(k);
        } else {
            const size_t i = rng() % live.size();
            m.Erase(live[i]);
            ref.erase(live[i]);
            live[i] = live.back();
            live.pop_back();
        }
    }
    Check(m.Count() == static_cast<int>(ref.size()), "count matches the reference");
    for (auto& [k, v] : ref) Check(m.Get(k) == v, "live key found with its value");
    for (int i = 0; i < 200'000; ++i) {
        const void* k = Key(i);
        Check(m.Get(k) == (ref.count(k) ? ref[k] : 0), "every pool key agrees with the reference");
    }
    const double churned = MissNs(m, 1'000'000);
    printf("churn: %d live, %d rebuilds, miss %.1f ns fresh / %.1f ns after churn\n", m.Count(), m.Rebuilds(), fresh,
           churned);
    Check(churned < fresh * 10 + 50, "a miss after churn stays as cheap as on a fresh table");

    // 2. Full: inserts past the limit are refused, lookups still end fast.
    static PtrMap<8192> full;
    int accepted = 0;
    for (int i = 0; i < 8192; ++i) accepted += full.Set(Key(i), ValueFor(Key(i)));
    printf("full: %d of 8192 accepted, Full() %d, miss %.1f ns\n", accepted, full.Full(), MissNs(full, 1'000'000));
    Check(accepted == 4096 && full.Full(), "half the slots at most");
    for (int i = 0; i < accepted; ++i) Check(full.Get(Key(i)) == ValueFor(Key(i)), "accepted keys kept");

    // 3. Readers during writes: a lookup returns the key's own value or 0,
    //    never another key's value.
    static PtrMap<8192> shared;
    std::atomic<bool> stop{false};
    std::atomic<long long> reads{0}, wrong{0};
    std::thread reader([&] {
        std::mt19937_64 r(99);
        while (!stop.load()) {
            const void* k = Key(r() % 50'000);
            const int v = shared.Get(k);
            if (v && v != ValueFor(k)) wrong.fetch_add(1);
            reads.fetch_add(1);
        }
    });
    std::vector<const void*> w_live;
    for (int op = 0; op < 3'000'000; ++op) {
        if (w_live.size() < 3000 && (rng() % 3)) {
            const void* k = Key(rng() % 50'000);
            if (shared.Get(k)) continue;
            shared.Set(k, ValueFor(k));
            w_live.push_back(k);
        } else if (!w_live.empty()) {
            const size_t i = rng() % w_live.size();
            shared.Erase(w_live[i]);
            w_live[i] = w_live.back();
            w_live.pop_back();
        }
    }
    stop = true;
    reader.join();
    printf("threads: %lld reads during writes, %lld wrong, %d rebuilds\n", reads.load(), wrong.load(),
           shared.Rebuilds());
    Check(wrong.load() == 0, "no reader saw another key's value");

    printf(g_failures ? "%d FAILURE(S)\n" : "all checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
