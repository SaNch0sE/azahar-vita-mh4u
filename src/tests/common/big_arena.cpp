// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstring>
#include <random>
#include <thread>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "common/big_arena.h"

using namespace Common::BigArena;

// The arena is process-wide and initialised once, so one test case covers it all, in order.
TEST_CASE("BigArena", "[common]") {
    // Before Init everything passes through to malloc.
    void* q = Alloc(100000);
    REQUIRE(q != nullptr);
    Free(q);
    REQUIRE(GetStats().capacity == 0);

    REQUIRE(Init(8 << 20));
    REQUIRE_FALSE(Init(8 << 20));

    // Random alloc/free with fill patterns: no block overlaps another.
    std::mt19937 rng(1);
    struct Block {
        unsigned char* p;
        std::size_t n;
        unsigned char v;
    };
    std::vector<Block> live;
    for (int it = 0; it < 50000; it++) {
        if (live.size() < 40 && (rng() % 2 || live.empty())) {
            const std::size_t n = 1 + rng() % (700 * 1024);
            auto* p = static_cast<unsigned char*>(Alloc(n));
            REQUIRE(p != nullptr);
            const auto v = static_cast<unsigned char>(rng());
            std::memset(p, v, n);
            live.push_back({p, n, v});
        } else {
            const std::size_t i = rng() % live.size();
            const Block b = live[i];
            REQUIRE(b.p[0] == b.v);
            REQUIRE(b.p[b.n / 2] == b.v);
            REQUIRE(b.p[b.n - 1] == b.v);
            Free(b.p);
            live[i] = live.back();
            live.pop_back();
        }
    }
    for (const Block& b : live) {
        REQUIRE(b.p[b.n - 1] == b.v);
        Free(b.p);
    }
    // Everything coalesces back into one free run.
    Stats s = GetStats();
    REQUIRE(s.used == 0);
    REQUIRE(s.largest == s.capacity);

    // Full: requests fall back to malloc and are counted.
    std::vector<void*> blocks;
    for (int i = 0; i < 300; i++) {
        blocks.push_back(Alloc(64 * 1024));
    }
    REQUIRE(GetStats().fallbacks > s.fallbacks);
    for (void* p : blocks) {
        Free(p);
    }
    REQUIRE(GetStats().largest == GetStats().capacity);

    // Several threads.
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; t++) {
        threads.emplace_back([t] {
            std::mt19937 r(t);
            std::vector<void*> mine;
            for (int i = 0; i < 20000; i++) {
                if (mine.size() < 8 && r() % 2) {
                    mine.push_back(Alloc(32768 + r() % 300000));
                } else if (!mine.empty()) {
                    Free(mine.back());
                    mine.pop_back();
                }
            }
            for (void* p : mine) {
                Free(p);
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    REQUIRE(GetStats().used == 0);
    REQUIRE(GetStats().largest == GetStats().capacity);

    // The allocator, small and large.
    std::vector<int, Allocator<int>> large(100000, 7), small(10, 3);
    REQUIRE(large[99999] == 7);
    REQUIRE(small[9] == 3);
}
