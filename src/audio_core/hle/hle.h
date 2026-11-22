// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <memory>
#include <vector>
#include <boost/serialization/export.hpp>
#include "audio_core/audio_types.h"
#include "audio_core/dsp_interface.h"
#include "common/common_types.h"
#include "core/hle/service/dsp/dsp_dsp.h"
#include "core/memory.h"

namespace Core {
class Timing;
}

namespace Memory {
class MemorySystem;
}

namespace AudioCore {

class DspHle final : public DspInterface {
public:
    explicit DspHle(Core::System& system);
    explicit DspHle(Core::System& system, Memory::MemorySystem& memory, Core::Timing& timing);
    ~DspHle();

    u16 RecvData(u32 register_number) override;
    bool RecvDataIsReady(u32 register_number) const override;
    void SetSemaphore(u16 semaphore_value) override;
    std::vector<u8> PipeRead(DspPipe pipe_number, std::size_t length) override;
    std::size_t GetPipeReadableSize(DspPipe pipe_number) const override;
    void PipeWrite(DspPipe pipe_number, std::span<const u8> buffer) override;

#ifdef VITA_DIAGNOSTICS
    /// The DSP thread's counters since the last call (perflog; diagnostics builds only): frames
    /// generated, the thread's time on them, the emulation thread's waits for a frame at its
    /// interrupt, the application's semaphore signals (its "shared memory written") and how late
    /// they came.
    struct Stats {
        u32 frames{};
        /// Frames the tick started itself (no answer from the application had).
        u32 tick_frames{};
        u64 work_us{};
        u32 waits{};
        u64 wait_us{};
        /// From a frame's post to its start on the DSP thread (all frames), and from the post to
        /// the emulation thread's wait for it (the frames waited for).
        u64 start_us{};
        u64 wait_lead_us{};
        u32 semaphores{};
        /// Ticks that found no new frame from the application (the region and counter of the
        /// tick before), and the buffer changes such frames reported: statuses written into
        /// the region the application is still working in, which it reads no more.
        u32 late_ticks{};
        u32 late_buffer_changes{};
        /// The steps ticks waited for the application's answer, and the waits given up.
        u32 tick_waits{};
        u32 unanswered{};
        /// From the interrupt to the application's semaphore signal: total and longest.
        u64 answer_us{};
        u32 answer_longest_us{};
    };
    Stats TakeStats();
#endif

    void SetInterruptHandler(
        std::function<void(Service::DSP::InterruptType type, DspPipe pipe)> handler) override;

    void LoadComponent(std::span<const u8> buffer) override;
    void UnloadComponent() override;

    /// The live DSP thread's place for the stall watch's report, into `out` without allocating:
    /// the frames posted and done, where the frame being made is (a source, the mixers, the
    /// output) with that source's buffer state, and the sources' loop passes. Read without the
    /// lock: a snapshot. "no DSP thread" when none runs.
    static void DescribeWorker(char* out, std::size_t size);

private:
    struct Impl;
    friend struct Impl;
    std::unique_ptr<Impl> impl;

    template <class Archive>
    void serialize(Archive& ar, const unsigned int);
    friend class boost::serialization::access;
};

} // namespace AudioCore
