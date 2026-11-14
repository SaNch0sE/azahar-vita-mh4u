// Copyright 2015 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <vector>
#include "common/common_types.h"

namespace Memory {
class MemorySystem;
}

namespace Service::Y2R {
struct ConversionConfiguration;
} // namespace Service::Y2R

namespace HW::Y2R {
void PerformConversion(Memory::MemorySystem& memory, Service::Y2R::ConversionConfiguration cvt);
/// The fast path's conversion (YUV420 Indiv8 -> RGB8, 8x8 blocks, no rotation) with every row
/// already resolved to host memory: its 8-line strips can be converted on any thread.
struct FastJob {
    u32 width = 0;
    u32 strips = 0;
    s32 c0 = 0, c1 = 0, c2 = 0, c3 = 0, c4 = 0, k5 = 0, k6 = 0, k7 = 0;
    std::vector<const u8*> y, u, v; // per luma row, per chroma row
    std::vector<u8*> dst;           // per strip
};
/// False (and nothing written) when the configuration is not the fast path's or a row does not
/// resolve; ignores Common::y2r_fast.
bool PrepareFastJob(Memory::MemorySystem& memory, const Service::Y2R::ConversionConfiguration& cvt,
                    FastJob& job);
void ConvertFastStrip(const FastJob& job, u32 strip);
/// The general (any format) conversion, without the fast path.
void PerformConversionGeneral(Memory::MemorySystem& memory, Service::Y2R::ConversionConfiguration cvt);
} // namespace HW::Y2R
