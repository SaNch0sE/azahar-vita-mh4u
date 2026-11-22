// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <cstring>
#include "common/common_types.h"

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace Pica {

/// Whether count words at a and b differ. The uploads that compare a command list's words with what
/// they replace (lighting LUTs, shader code and swizzles) find them equal nearly always. With NEON
/// the differences are ORed eight words at a time and moved to the core registers once at the end,
/// since on the Cortex-A9 each NEON-to-core move stalls the pipeline; newlib's memcmp compares a
/// word at a time.
[[nodiscard]] inline bool WordsDiffer(const u32* a, const u32* b, u32 count) {
#if defined(__ARM_NEON)
    uint32x4_t acc = vdupq_n_u32(0);
    u32 i = 0;
    for (; i + 8 <= count; i += 8) {
        acc = vorrq_u32(acc, veorq_u32(vld1q_u32(a + i), vld1q_u32(b + i)));
        acc = vorrq_u32(acc, veorq_u32(vld1q_u32(a + i + 4), vld1q_u32(b + i + 4)));
    }
    const uint32x2_t half = vorr_u32(vget_low_u32(acc), vget_high_u32(acc));
    u32 diff = vget_lane_u32(vpmax_u32(half, half), 0);
    for (; i < count; ++i) {
        diff |= a[i] ^ b[i];
    }
    return diff != 0;
#else
    return std::memcmp(a, b, count * sizeof(u32)) != 0;
#endif
}

} // namespace Pica
