// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <atomic>
#include "common/common_types.h"

// MH4U menu world skip (docs/plans/vita-mh4u-menu-world-skip.md): what the cover trap on the
// emulation thread and the cover proof on the render thread tell each other. Each value has
// one writer; nothing here takes a lock.
namespace Common::Mh4uCover {

/// The render thread proves covers and the texture cache keeps opacity bitmaps: set once at
/// start-up from the `covertrap` switch, before any texture is uploaded.
inline bool render_enabled = false;

/// Cover traps taken so far (emulation thread). The frame tag: read when a frame's display
/// transfer is queued and carried with it to the render thread, so a proof names the trap
/// whose frame it judged (give or take the game's own lag, which the decision allows for).
inline std::atomic<u32> trap_seq{0};

/// The emulation thread's layout walk found a plausible cover key (large opaque panes). The
/// render thread reads it once a frame and does no proof work at all while it is false.
inline std::atomic<bool> key_present{false};

/// The skip is armed (emulation thread), for `covermagenta` on the render thread.
inline std::atomic<bool> armed{false};

/// One frame's verdict: every pixel of the picture ended under an opaque 2D sprite drawn after
/// anything world-dependent there (`covered`), and how many world-dependent draws (depth,
/// perspective, a rendered surface) came after the first cover (`late3d`, for the logs).
struct Proof {
    u32 tag = 0;
    bool covered = false;
    u32 late3d = 0;
};

/// A small ring the render thread appends to and the trap drains: a proof is never torn and
/// a slow reader loses only the oldest ones.
constexpr u32 ProofRingSize = 16;
inline std::array<std::atomic<u64>, ProofRingSize> proof_ring{};
inline std::atomic<u32> proof_head{0};

inline void PublishProof(u32 tag, bool covered, u32 late3d) {
    const u32 head = proof_head.load(std::memory_order_relaxed);
    const u64 late = late3d < 0x7FFF ? late3d : 0x7FFF;
    proof_ring[head % ProofRingSize].store(
        u64{tag} | (u64{covered ? 1u : 0u} << 32) | (late << 33), std::memory_order_relaxed);
    proof_head.store(head + 1, std::memory_order_release);
}

/// The proof at ring position `index` (any value below proof_head, at most ProofRingSize
/// behind it).
inline Proof ReadProof(u32 index) {
    const u64 v = proof_ring[index % ProofRingSize].load(std::memory_order_relaxed);
    return Proof{static_cast<u32>(v), ((v >> 32) & 1) != 0, static_cast<u32>((v >> 33) & 0x7FFF)};
}

} // namespace Common::Mh4uCover
