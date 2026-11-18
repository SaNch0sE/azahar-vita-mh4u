// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

// BigArena: one fixed block for large, short-lived buffers (GPU command list snapshots, decoded
// audio, file read buffers), so they stop cutting holes in the main heap.
//
// On the Vita the C++ heap is a fixed 240 MiB block. In long sessions ~20 GiB an hour of
// 32-700 KiB buffers went through it, each freed moments later; long-lived small objects
// settled in the holes, and after 1-2 hours ~20 MiB was free in pieces under 200 KiB and a
// 258 KiB request failed (std::bad_alloc). Here those buffers come and go inside their own
// block, which the rest of the heap never sees.
//
// The block is cut into 4 KiB granules. Runs are tracked out of band (a length per run's first
// and last granule), allocation is best fit, and a free coalesces with both neighbours; a few
// dozen runs are live at a time, so the scans are short. One mutex guards it.
//
// Requests under MinBytes, or that do not fit, go to std::malloc; Free tells the two apart by
// address. Without Init (desktop builds, or the nobigarena switch) everything goes to malloc.

#include <cstddef>
#include <memory>
#include <new>

namespace Common::BigArena {

/// Smaller requests are not worth the arena: they go to malloc.
constexpr std::size_t MinBytes = 32 * 1024;

/// Takes `bytes` (rounded down to whole granules) from malloc for the arena. Call once, before
/// any other thread uses it. Returns false if that allocation failed (then all calls pass
/// through to malloc).
bool Init(std::size_t bytes);

/// A block of at least `bytes`, from the arena when it is large enough and fits, else from
/// std::malloc. nullptr only when both are out of memory.
void* Alloc(std::size_t bytes) noexcept;

/// Frees a block from Alloc (either source). nullptr is ignored.
void Free(void* p) noexcept;

struct Stats {
    std::size_t capacity = 0; ///< bytes in the arena (0: not initialised)
    std::size_t used = 0;     ///< bytes in live arena blocks (whole granules)
    std::size_t peak = 0;     ///< highest `used` so far
    std::size_t largest = 0;  ///< largest free run
    std::size_t allocs = 0;   ///< blocks served by the arena so far
    std::size_t fallbacks = 0; ///< requests of MinBytes or more that did not fit (went to malloc)
};
Stats GetStats();

/// Stateless allocator: MinBytes and up through the arena, smaller through operator new.
template <class T>
struct Allocator {
    using value_type = T;
    Allocator() noexcept = default;
    template <class U>
    Allocator(const Allocator<U>&) noexcept {}

    T* allocate(std::size_t n) {
        const std::size_t bytes = n * sizeof(T);
        if (bytes >= MinBytes) {
            if (void* p = Alloc(bytes)) {
                return static_cast<T*>(p);
            }
            throw std::bad_alloc();
        }
        return static_cast<T*>(::operator new(bytes));
    }
    void deallocate(T* p, std::size_t n) noexcept {
        if (n * sizeof(T) >= MinBytes) {
            Free(p);
        } else {
            ::operator delete(p);
        }
    }
    template <class U>
    bool operator==(const Allocator<U>&) const noexcept {
        return true;
    }
    template <class U>
    bool operator!=(const Allocator<U>&) const noexcept {
        return false;
    }
};

struct Deleter {
    void operator()(void* p) const noexcept {
        Free(p);
    }
};
/// A raw byte buffer from Alloc, freed with Free.
using Buffer = std::unique_ptr<unsigned char[], Deleter>;
/// nullptr only when out of memory.
inline Buffer MakeBuffer(std::size_t bytes) noexcept {
    return Buffer(static_cast<unsigned char*>(Alloc(bytes != 0 ? bytes : 1)));
}

} // namespace Common::BigArena
