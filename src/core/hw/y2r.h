// Copyright 2015 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <memory>
#include "common/common_types.h"

namespace Memory {
class MemorySystem;
}

namespace Service::Y2R {
struct ConversionConfiguration;
} // namespace Service::Y2R

namespace HW::Y2R {

/// The guest buffers a conversion reads and writes as host memory: each one's guest range and
/// where it starts in host memory. The conversion runs on a thread of its own
/// (Service::Y2R::Y2R_U's worker), where the current page table may not be the guest's.
struct HostBuffers {
    struct Range {
        VAddr start;
        u32 bytes;
        u8* host;
    };
    std::array<Range, 5> ranges{};
    u32 count{};
    /// The host address of guest address `vaddr`, inside one of the ranges.
    [[nodiscard]] u8* GetPointer(VAddr vaddr) const;
};

/// cvt's buffers through the current page table, each the stretch cvt's DMA reads or writes:
/// one stretch of host memory each (the CDMA's buffers are physically contiguous).
[[nodiscard]] HostBuffers ResolveBuffers(Memory::MemorySystem& memory,
                                         const Service::Y2R::ConversionConfiguration& cvt);

void PerformConversion(const HostBuffers& memory, Service::Y2R::ConversionConfiguration cvt);

/// One conversion, its strips of eight lines taken in turn by every thread that runs it: the
/// worker (Run), and a thread that would otherwise wait for it (Help: the guest's, in
/// Service::Y2R::Y2R_U::WaitConversion). A conversion the NEON path does not take, or one a
/// diagnostics build checks against the per-pixel path, runs whole in Run, and Help takes none.
class Conversion {
public:
    Conversion(const HostBuffers& memory, const Service::Y2R::ConversionConfiguration& cvt);
    ~Conversion();
    Conversion(const Conversion&) = delete;
    Conversion& operator=(const Conversion&) = delete;
    /// Converts strips until none is left to take (or the whole conversion).
    void Run();
    /// Takes strips as Run does; false for a conversion that runs whole.
    bool Help();
    /// Whether every strip is written (the last of them may still be in another thread's hands
    /// after this thread's Run or Help returned).
    [[nodiscard]] bool Finished() const;

private:
    struct State;
    void TakeStrips();
    std::unique_ptr<State> state;
};

} // namespace HW::Y2R
