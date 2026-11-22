// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <atomic>
#include <cstdio>

#include <boost/serialization/array.hpp>
#include <boost/serialization/base_object.hpp>
#include <boost/serialization/binary_object.hpp>
#include <boost/serialization/shared_ptr.hpp>
#include <boost/serialization/vector.hpp>
#include <boost/serialization/weak_ptr.hpp>
#include "audio_core/audio_types.h"
#include "audio_core/hle/aac_decoder.h"
#include "audio_core/hle/common.h"
#include "audio_core/hle/decoder.h"
#include "audio_core/hle/hle.h"
#include "audio_core/hle/mixers.h"
#include "audio_core/hle/shared_memory.h"
#include "audio_core/hle/source.h"
#include "audio_core/sink.h"
#include "common/archives.h"
#include "common/assert.h"
#include "common/common_types.h"
#include "common/hash.h"
#include "common/logging/log.h"
#include "common/named_thread.h"
#include "common/process_clock.h"
#include "common/settings.h"
#include "common/heap_check.h"
#include "common/thread.h"
#include "core/core.h"
#include "core/core_timing.h"

using InterruptType = Service::DSP::InterruptType;

namespace AudioCore {

DspHle::DspHle(Core::System& system) : DspHle(system, system.Memory(), system.CoreTiming()) {}

template <class Archive>
void DspHle::serialize(Archive& ar, const unsigned int) {
    ar& boost::serialization::base_object<DspInterface>(*this);
    ar&* impl.get();
}
SERIALIZE_IMPL(DspHle)

// The value below is the "perfect" mathematical ratio of ARM11 cycles per audio frame, samples per
// frame * teaklite cycles per sample * 2 ARM11 cycles/teaklite cycle
// (160 * 4096 * 2) = (1310720)
//
// This value has been verified against a rough hardware test with hardware and LLE
static constexpr u64 audio_frame_ticks = samples_per_frame * 4096 * 2ull; ///< Units: ARM11 cycles

struct DspHle::Impl final {
public:
    explicit Impl(DspHle& parent, Memory::MemorySystem& memory, Core::Timing& timing);
    ~Impl();

    DspState GetDspState() const;

    u16 RecvData(u32 register_number);
    bool RecvDataIsReady(u32 register_number) const;
    std::vector<u8> PipeRead(DspPipe pipe_number, std::size_t length);
    std::size_t GetPipeReadableSize(DspPipe pipe_number) const;
    void PipeWrite(DspPipe pipe_number, std::span<const u8> buffer);

    void SetInterruptHandler(
        std::function<void(Service::DSP::InterruptType type, DspPipe pipe)> handler);

private:
    void Initialize();
    void Sleep();
    void Wakeup();

    void ResetPipes();
    void WriteU16(DspPipe pipe_number, u16 value);
    void AudioPipeWriteStructAddresses();

    std::size_t CurrentRegionIndex() const;

    /// A frame from the region the application wrote last (`read`), its statuses and output
    /// into the other one.
    StereoFrame16 GenerateFrame(std::size_t read);
    void AudioTickCallback(s64 cycles_late);

    // The frames run on their own thread, as the 3DS's DSP runs beside the ARM11 and the
    // application keeps to the shared memory's protocol with it: the application writes a
    // region and signals the semaphore; at its audio tick the DSP reads the region with the
    // newer frame counter, writes its statuses and output into the other one, and interrupts;
    // the application touches the regions again only after that interrupt.
    //
    // Here the frame starts at the application's semaphore signal, its answer to the last
    // interrupt: the region it wrote is the one the tick would read, and it leaves both alone
    // from there on. The DSP thread has the rest of the frame's time for it, and the tick, on
    // the emulation thread, finds it done and fires the interrupt: answer, frame, tick and
    // interrupt follow each other, the emulation thread waits for nothing, and no lock is
    // needed. The application sees each frame done at its tick, as on the 3DS.
    //
    // The application's sound thread may run on the other emulation thread, beside this one, and
    // come late: its wake-up crosses the threads, and core 1 gives it a share of its time. A frame
    // made at a tick while it still read the last frame's statuses or wrote its next frame came
    // from the old region and put statuses into the region in use: buffer changes the application
    // never read, or read for one voice of a pair only. So a tick with no answer yet waits for it,
    // in steps of guest time. An application that stops answering (TickWaitSteps steps, one frame)
    // is not waited for again until it signals: its frames (and the first one, before any answer)
    // start at the tick, which waits for the DSP thread and then interrupts.
    // Whatever else touches the DSP's state (the pipe's state changes, a savestate) waits for the
    // thread first (Finish).
public:
    void Finish();
    /// The application's semaphore signal: its answer to the last interrupt, and the next
    /// frame's start.
    void Answered();
    /// DspHle::DescribeWorker's text.
    void DescribeWorker(char* out, std::size_t size) const;
#ifdef VITA_DIAGNOSTICS
    void NoteSemaphore();
    DspHle::Stats TakeStats();
#endif

private:
    void Worker();
    void Post(std::size_t read, bool late);
    /// Posts the frame of the region the application wrote last. `at_tick`: the tick starts
    /// it, not the application's answer.
    void StartFrame(bool at_tick);

    DspState dsp_state = DspState::Off;
    std::array<std::vector<u8>, num_dsp_pipe> pipe_data{};

public:
    HLE::DspMemory* dsp_memory;

private:
    std::array<HLE::Source, HLE::num_sources> sources{{
        HLE::Source(0),  HLE::Source(1),  HLE::Source(2),  HLE::Source(3),  HLE::Source(4),
        HLE::Source(5),  HLE::Source(6),  HLE::Source(7),  HLE::Source(8),  HLE::Source(9),
        HLE::Source(10), HLE::Source(11), HLE::Source(12), HLE::Source(13), HLE::Source(14),
        HLE::Source(15), HLE::Source(16), HLE::Source(17), HLE::Source(18), HLE::Source(19),
        HLE::Source(20), HLE::Source(21), HLE::Source(22), HLE::Source(23),
    }};
    HLE::Mixers mixers{};

    HLE::DspMemory backup_dsp_memory;

    DspHle& parent;
    Core::Timing& core_timing;
    Core::TimingEventType* tick_event{};
    /// The emulation thread's: a frame is posted whose interrupt has not fired (the coming
    /// tick's), the last interrupt has no answer (the application's semaphore signal) yet, the
    /// application answers its interrupts (it has, and no wait for it ran out since), and the
    /// steps the coming tick has waited for the answer. None is in a savestate: a restored
    /// one starts as an application that has not answered yet.
    bool frame_pending{};
    bool awaiting_answer{};
    bool answers{};
    u32 tick_waits{};
    static constexpr u32 TickWaitSteps = 16;

    /// Under frame_cond: the frames posted and generated, the posted frame's read region, the
    /// thread's end, and the counters.
    Common::CoreCondition frame_cond;
    u32 frames_posted{};
    u32 frames_done{};
    std::size_t frame_read{};
    bool worker_stop{};
#ifdef VITA_DIAGNOSTICS
    DspHle::Stats stats{};
    /// The posted frame read the region and counter of the frame before (the application had
    /// written no new one), the last tick's region and counter, and the last interrupt's time.
    bool frame_late{};
    u32 last_tick_key{~0u};
    Common::ProcessClock::time_point interrupt_at{};
    /// When the frame in flight was posted.
    Common::ProcessClock::time_point posted_at{};
#endif
    /// Where the worker's frame is, for DescribeWorker: a source's index, MixersAt, OutputAt,
    /// or IdleAt between frames.
    static constexpr u32 MixersAt = 100;
    static constexpr u32 OutputAt = 200;
    static constexpr u32 IdleAt = 255;
    std::atomic<u32> worker_at{IdleAt};
    Common::NamedThread worker;

    std::unique_ptr<HLE::DecoderBase> aac_decoder{};

    std::function<void(Service::DSP::InterruptType type, DspPipe pipe)> interrupt_handler{};

    template <class Archive>
    void serialize(Archive& ar, const unsigned int) {
        // The DSP thread's frame done before its state is saved or replaced.
        Finish();
        if (Archive::is_loading::value) {
            // A frame the saved state had made and not reported is made again at the first
            // tick, from the same region (its changes were taken the first time).
            frame_pending = false;
            awaiting_answer = false;
            answers = false;
            tick_waits = 0;
        }
        ar& boost::serialization::make_binary_object(backup_dsp_memory.raw_memory.data(),
                                                     backup_dsp_memory.raw_memory.size());
        ar & dsp_state;
        ar & pipe_data;
        ar & sources;
        ar & mixers;
        // interrupt_handler is reregistered when loading state from DSP_DSP
    }
    friend class boost::serialization::access;
};

namespace {
/// The DSP running, for DescribeWorker (the stall watch's thread reads it).
std::atomic<void*> live_dsp{};
} // namespace

DspHle::Impl::Impl(DspHle& parent_, Memory::MemorySystem& memory, Core::Timing& timing)
    : parent(parent_), core_timing(timing) {
    dsp_memory = reinterpret_cast<HLE::DspMemory*>(memory.GetDspMemory(0));
    dsp_memory->raw_memory.fill(0);

    for (auto& source : sources) {
        source.SetMemory(memory);
    }

    aac_decoder = std::make_unique<HLE::AACDecoder>(memory);
    tick_event =
        core_timing.RegisterEvent("AudioCore::DspHle::tick_event", [this](u64, s64 cycles_late) {
            this->AudioTickCallback(cycles_late);
        });
    // The interrupt had its own event, half a frame after the tick, while the frames started
    // at the tick: a state saved then with it scheduled restores it, and it does nothing (the
    // first tick reports the frame).
    core_timing.RegisterEvent("AudioCore::DspHle::interrupt_event", [](u64, s64) {});
    core_timing.ScheduleEvent(audio_frame_ticks, tick_event);
    worker = Common::NamedThread{Common::ThreadCfg{"dsp", 256 * 1024}, [this] { Worker(); }};
    live_dsp.store(this, std::memory_order_release);
}

DspHle::Impl::~Impl() {
    live_dsp.store(nullptr, std::memory_order_release);
    core_timing.UnscheduleEvent(tick_event, 0);
    {
        std::scoped_lock lk{frame_cond};
        worker_stop = true;
        frame_cond.NotifyAll();
    }
    worker.join();
}

void DspHle::Impl::Worker() {
    Common::SetCurrentThreadRole(Common::ThreadRole::Dsp);
    std::scoped_lock lk{frame_cond};
    while (true) {
        while (frames_done == frames_posted && !worker_stop) {
            frame_cond.WaitFor(std::chrono::seconds(1));
        }
        if (worker_stop) {
            return;
        }
        const std::size_t read = frame_read;
#ifdef VITA_DIAGNOSTICS
        const bool late = frame_late;
        const auto start = Common::ProcessClock::now();
        stats.start_us += static_cast<u64>((start - posted_at).count());
#endif
        frame_cond.unlock();
        Common::HeapCheck::Check("the DSP frame's start");
        const StereoFrame16 frame = GenerateFrame(read);
#ifdef VITA_DIAGNOSTICS
        // A late frame's statuses went into the region the application is working in: the
        // buffer changes they report are the ones it will not read.
        u32 late_changes = 0;
        if (late) {
            const HLE::SharedMemory& write =
                read != 0 ? dsp_memory->region_0 : dsp_memory->region_1;
            std::string ids;
            for (std::size_t i = 0; i < HLE::num_sources; i++) {
                const auto& status = write.source_statuses.status[i];
                if (status.current_buffer_id_dirty) {
                    late_changes++;
                    fmt::format_to(std::back_inserter(ids), " {}:{}", i,
                                   static_cast<u16>(status.current_buffer_id));
                }
            }
            if (late_changes != 0) {
                LOG_INFO(Audio_DSP, "dsp: a late frame's buffer changes (source:buffer):{}", ids);
            }
        }
#endif
        Common::HeapCheck::Check("the DSP frame's sources and mixes");
        worker_at.store(OutputAt, std::memory_order_relaxed);
        parent.OutputFrame(frame);
        Common::HeapCheck::Check("the DSP frame's output");
        worker_at.store(IdleAt, std::memory_order_relaxed);
#ifdef VITA_DIAGNOSTICS
        const auto us = (Common::ProcessClock::now() - start).count();
#endif
        frame_cond.lock();
        frames_done++;
#ifdef VITA_DIAGNOSTICS
        stats.frames++;
        stats.work_us += static_cast<u64>(us);
        stats.late_buffer_changes += late_changes;
#endif
        frame_cond.NotifyAll();
    }
}

void DspHle::Impl::Post(std::size_t read, [[maybe_unused]] bool late) {
    std::scoped_lock lk{frame_cond};
    DEBUG_ASSERT(frames_done == frames_posted);
    frame_read = read;
#ifdef VITA_DIAGNOSTICS
    frame_late = late;
    stats.late_ticks += late ? 1 : 0;
    posted_at = Common::ProcessClock::now();
#endif
    frames_posted++;
    frame_cond.NotifyAll();
}

void DspHle::Impl::Finish() {
    std::scoped_lock lk{frame_cond};
    if (frames_done == frames_posted) {
        return;
    }
#ifdef VITA_DIAGNOSTICS
    const auto start = Common::ProcessClock::now();
#endif
    while (frames_done != frames_posted) {
        frame_cond.WaitFor(std::chrono::seconds(1));
    }
#ifdef VITA_DIAGNOSTICS
    stats.waits++;
    stats.wait_us += static_cast<u64>((Common::ProcessClock::now() - start).count());
    stats.wait_lead_us += static_cast<u64>((start - posted_at).count());
#endif
}

#ifdef VITA_DIAGNOSTICS
void DspHle::Impl::NoteSemaphore() {
    std::scoped_lock lk{frame_cond};
    stats.semaphores++;
    const u32 us = static_cast<u32>((Common::ProcessClock::now() - interrupt_at).count());
    stats.answer_us += us;
    stats.answer_longest_us = std::max(stats.answer_longest_us, us);
}

DspHle::Stats DspHle::Impl::TakeStats() {
    std::scoped_lock lk{frame_cond};
    return std::exchange(stats, {});
}
#endif

void DspHle::Impl::StartFrame([[maybe_unused]] bool at_tick) {
    // The region with the newer frame counter is the one the application finished: it bumps
    // the counter when it is done writing, and signals the semaphore (SetSemaphore).
    const std::size_t read = CurrentRegionIndex();
    bool late = false;
#ifdef VITA_DIAGNOSTICS
    const u32 key = static_cast<u32>(read) << 16 |
                    static_cast<u16>(read == 0 ? dsp_memory->region_0.frame_counter
                                               : dsp_memory->region_1.frame_counter);
    late = key == last_tick_key;
    last_tick_key = key;
#endif
    Post(read, late);
#ifdef VITA_DIAGNOSTICS
    if (at_tick) {
        std::scoped_lock lk{frame_cond};
        stats.tick_frames++;
    }
#endif
    frame_pending = true;
}

void DspHle::Impl::Answered() {
    answers = true;
    awaiting_answer = false;
    // One frame an interrupt: a second signal before the tick starts none.
    if (GetDspState() == DspState::On && !frame_pending) {
        StartFrame(false);
    }
}

DspState DspHle::Impl::GetDspState() const {
    return dsp_state;
}

u16 DspHle::Impl::RecvData(u32 register_number) {
    ASSERT_MSG(register_number == 0, "Unknown register_number {}", register_number);

    // Application reads this after requesting DSP shutdown, to verify the DSP has indeed shutdown
    // or slept.

    switch (GetDspState()) {
    case AudioCore::DspState::On:
        return 0;
    case AudioCore::DspState::Off:
    case AudioCore::DspState::Sleeping:
        return 1;
    default:
        UNREACHABLE();
        break;
    }
}

bool DspHle::Impl::RecvDataIsReady(u32 register_number) const {
    ASSERT_MSG(register_number == 0, "Unknown register_number {}", register_number);
    return true;
}

std::vector<u8> DspHle::Impl::PipeRead(DspPipe pipe_number, std::size_t length) {
    const std::size_t pipe_index = static_cast<std::size_t>(pipe_number);

    if (pipe_index >= num_dsp_pipe) {
        LOG_ERROR(Audio_DSP, "pipe_number = {} invalid", pipe_index);
        return {};
    }

    if (length > UINT16_MAX) { // Can only read at most UINT16_MAX from the pipe
        LOG_ERROR(Audio_DSP, "length of {} greater than max of {}", length, UINT16_MAX);
        return {};
    }

    std::vector<u8>& data = pipe_data[pipe_index];

    if (length > data.size()) {
        LOG_WARNING(
            Audio_DSP,
            "pipe_number = {} is out of data, application requested read of {} but {} remain",
            pipe_index, length, data.size());
        length = static_cast<u32>(data.size());
    }

    if (length == 0)
        return {};

    std::vector<u8> ret(data.begin(), data.begin() + length);
    data.erase(data.begin(), data.begin() + length);
    return ret;
}

size_t DspHle::Impl::GetPipeReadableSize(DspPipe pipe_number) const {
    const std::size_t pipe_index = static_cast<std::size_t>(pipe_number);

    if (pipe_index >= num_dsp_pipe) {
        LOG_ERROR(Audio_DSP, "pipe_number = {} invalid", pipe_index);
        return 0;
    }

    return pipe_data[pipe_index].size();
}

void DspHle::Impl::PipeWrite(DspPipe pipe_number, std::span<const u8> buffer) {
    // The DSP thread's frame is done before anything else works on the DSP's state.
    Finish();
    switch (pipe_number) {
    case DspPipe::Audio: {
        if (buffer.size() != 4) {
            LOG_ERROR(Audio_DSP, "DspPipe::Audio: Unexpected buffer length {} was written",
                      buffer.size());
            return;
        }
        // A state change resets or saves what the frames use, and starts the exchange with
        // the application over: a frame made and not reported is dropped with the state it
        // was made in (the change came before its tick).
        frame_pending = false;
        awaiting_answer = false;
        answers = false;
        tick_waits = 0;

        enum class StateChange {
            Initialize = 0,
            Shutdown = 1,
            Wakeup = 2,
            Sleep = 3,
        };

        // Waking up from sleep garbles some of the structs in the memory region. (TODO:
        // Implement this.) Applications store away the state of these structs before
        // sleeping and reset it back after wakeup on behalf of the DSP.

        switch (static_cast<StateChange>(buffer[0])) {
        case StateChange::Initialize:
            LOG_INFO(Audio_DSP, "Application has requested initialization of DSP hardware");
            Initialize();
            AudioPipeWriteStructAddresses();
            dsp_state = DspState::On;
            break;
        case StateChange::Shutdown:
            LOG_INFO(Audio_DSP, "Application has requested shutdown of DSP hardware");
            dsp_state = DspState::Off;
            break;
        case StateChange::Wakeup:
            LOG_INFO(Audio_DSP, "Application has requested wakeup of DSP hardware");
            Wakeup();
            AudioPipeWriteStructAddresses();
            dsp_state = DspState::On;
            break;
        case StateChange::Sleep:
            LOG_INFO(Audio_DSP, "Application has requested sleep of DSP hardware");
            Sleep();
            AudioPipeWriteStructAddresses();
            dsp_state = DspState::Sleeping;
            break;
        default:
            LOG_ERROR(Audio_DSP,
                      "Application has requested unknown state transition of DSP hardware {}",
                      buffer[0]);
            dsp_state = DspState::Off;
            break;
        }

        return;
    }
    case DspPipe::Binary: {
        // TODO(B3N30): Make this async, and signal the interrupt
        HLE::BinaryMessage request{};
        if (sizeof(request) != buffer.size()) {
            LOG_CRITICAL(Audio_DSP, "got binary pipe with wrong size {}", buffer.size());
            UNIMPLEMENTED();
            return;
        }
        std::memcpy(&request, buffer.data(), buffer.size());
        if (request.header.codec != HLE::DecoderCodec::DecodeAAC) {
            LOG_CRITICAL(Audio_DSP, "got unknown codec {}", static_cast<u16>(request.header.codec));
            UNIMPLEMENTED();
            return;
        }
        const HLE::BinaryMessage response = aac_decoder->ProcessRequest(request);
        pipe_data[static_cast<u32>(pipe_number)].resize(sizeof(response));
        std::memcpy(pipe_data[static_cast<u32>(pipe_number)].data(), &response, sizeof(response));

        interrupt_handler(InterruptType::Pipe, DspPipe::Binary);
        break;
    }
    default:
        LOG_CRITICAL(Audio_DSP, "pipe_number = {} unimplemented",
                     static_cast<std::size_t>(pipe_number));
        UNIMPLEMENTED();
        return;
    }
}

void DspHle::Impl::SetInterruptHandler(
    std::function<void(Service::DSP::InterruptType type, DspPipe pipe)> handler) {
    interrupt_handler = handler;
}

void DspHle::Impl::Initialize() {
    // TODO(PabloMK7): This is NOT the right way to do this,
    // but it is close enough. This makes sure the DSP state
    // is clean and consistent every time the HW is initialized,
    // but what is exactly reset needs to be figured out.
    dsp_memory->raw_memory.fill(0);
    mixers.Reset();
    for (auto& s : sources) {
        s.Reset();
    }
    aac_decoder->Reset();
    ResetPipes();
}

void DspHle::Impl::Sleep() {
    // TODO(PabloMK7): This is NOT the right way to do this,
    // but it is close enough. What state is saved on
    // real hardware still not figured out.
    backup_dsp_memory.raw_memory = dsp_memory->raw_memory;
    mixers.Sleep();
    for (auto& s : sources) {
        s.Sleep();
    }
    // TODO(PabloMK7): Figure out if we need to save the state
    // of the AAC decoder, probably not.
}

void DspHle::Impl::Wakeup() {
    // TODO(PabloMK7): This is NOT the right way to do this,
    // but it is close enough. What state is restored on
    // real hardware still not figured out.
    dsp_memory->raw_memory = backup_dsp_memory.raw_memory;
    backup_dsp_memory.raw_memory.fill(0);
    mixers.Wakeup();
    for (auto& s : sources) {
        s.Wakeup();
    }
    aac_decoder->Reset();
    ResetPipes();
}

void DspHle::Impl::ResetPipes() {
    for (auto& data : pipe_data) {
        data.clear();
    }
}

void DspHle::Impl::WriteU16(DspPipe pipe_number, u16 value) {
    const std::size_t pipe_index = static_cast<std::size_t>(pipe_number);

    std::vector<u8>& data = pipe_data.at(pipe_index);
    // Little endian
    data.emplace_back(value & 0xFF);
    data.emplace_back(value >> 8);
}

void DspHle::Impl::AudioPipeWriteStructAddresses() {
    // These struct addresses are DSP dram addresses.
    // See also: DSP_DSP::ConvertProcessAddressFromDspDram
    static const std::array<u16, 15> struct_addresses = {
        0x8000 + offsetof(HLE::SharedMemory, frame_counter) / 2,
        0x8000 + offsetof(HLE::SharedMemory, source_configurations) / 2,
        0x8000 + offsetof(HLE::SharedMemory, source_statuses) / 2,
        0x8000 + offsetof(HLE::SharedMemory, adpcm_coefficients) / 2,
        0x8000 + offsetof(HLE::SharedMemory, dsp_configuration) / 2,
        0x8000 + offsetof(HLE::SharedMemory, dsp_status) / 2,
        0x8000 + offsetof(HLE::SharedMemory, final_samples) / 2,
        0x8000 + offsetof(HLE::SharedMemory, intermediate_mix_samples) / 2,
        0x8000 + offsetof(HLE::SharedMemory, compressor) / 2,
        0x8000 + offsetof(HLE::SharedMemory, dsp_debug) / 2,
        0x8000 + offsetof(HLE::SharedMemory, unknown10) / 2,
        0x8000 + offsetof(HLE::SharedMemory, unknown11) / 2,
        0x8000 + offsetof(HLE::SharedMemory, unknown12) / 2,
        0x8000 + offsetof(HLE::SharedMemory, unknown13) / 2,
        0x8000 + offsetof(HLE::SharedMemory, unknown14) / 2,
    };

    // Begin with a u16 denoting the number of structs.
    WriteU16(DspPipe::Audio, static_cast<u16>(struct_addresses.size()));
    // Then write the struct addresses.
    for (u16 addr : struct_addresses) {
        WriteU16(DspPipe::Audio, addr);
    }
    // Signal that we have data on this pipe.
    interrupt_handler(InterruptType::Pipe, DspPipe::Audio);
}

size_t DspHle::Impl::CurrentRegionIndex() const {
    // The region with the higher frame counter is chosen unless there is wraparound.
    // This function only returns a 0 or 1.
    const u16 frame_counter_0 = dsp_memory->region_0.frame_counter;
    const u16 frame_counter_1 = dsp_memory->region_1.frame_counter;

    if (frame_counter_0 == 0xFFFFu && frame_counter_1 != 0xFFFEu) {
        // Wraparound has occurred.
        return 1;
    }

    if (frame_counter_1 == 0xFFFFu && frame_counter_0 != 0xFFFEu) {
        // Wraparound has occurred.
        return 0;
    }

    return (frame_counter_0 > frame_counter_1) ? 0 : 1;
}

StereoFrame16 DspHle::Impl::GenerateFrame(std::size_t read_index) {
    HLE::SharedMemory& read = read_index == 0 ? dsp_memory->region_0 : dsp_memory->region_1;
    HLE::SharedMemory& write = read_index != 0 ? dsp_memory->region_0 : dsp_memory->region_1;

    std::array<QuadFrame32, 3> intermediate_mixes = {};

    // Generate intermediate mixes
    for (std::size_t i = 0; i < HLE::num_sources; i++) {
        worker_at.store(static_cast<u32>(i), std::memory_order_relaxed);
        write.source_statuses.status[i] =
            sources[i].Tick(read.source_configurations.config[i], read.adpcm_coefficients.coeff[i]);
        for (std::size_t mix = 0; mix < 3; mix++) {
            sources[i].MixInto(intermediate_mixes[mix], mix);
        }
    }

    // Generate final mix
    worker_at.store(MixersAt, std::memory_order_relaxed);
    write.dsp_status = mixers.Tick(read.dsp_configuration, read.intermediate_mix_samples,
                                   write.intermediate_mix_samples, intermediate_mixes);

    StereoFrame16 output_frame = mixers.GetOutput();

    // Write current output frame to the shared memory region
    for (std::size_t samplei = 0; samplei < output_frame.size(); samplei++) {
        for (std::size_t channeli = 0; channeli < output_frame[0].size(); channeli++) {
            write.final_samples.pcm16[samplei][channeli] = s16_le(output_frame[samplei][channeli]);
        }
    }

    return output_frame;
}

void DspHle::Impl::AudioTickCallback(s64 cycles_late) {
    const double time_scale =
        Settings::values.enable_realtime_audio
            ? std::max(0.01, // Arbitrary small value to prevent time_scale from approaching zero
                       Core::System::GetInstance().GetStableFrameTimeScale())
            : 1.0;
    const s64 frame_ticks = static_cast<s64>(audio_frame_ticks / time_scale);
    if (GetDspState() == DspState::On) {
        if (!frame_pending) {
            // The application is still on the last frame: the tick waits for its answer.
            if (answers && awaiting_answer) {
                if (tick_waits < TickWaitSteps) {
                    tick_waits++;
#ifdef VITA_DIAGNOSTICS
                    {
                        std::scoped_lock lk{frame_cond};
                        stats.tick_waits++;
                    }
#endif
                    core_timing.ScheduleEvent(
                        std::max<s64>(0, frame_ticks / TickWaitSteps - cycles_late), tick_event);
                    return;
                }
                answers = false;
#ifdef VITA_DIAGNOSTICS
                std::scoped_lock lk{frame_cond};
                stats.unanswered++;
#endif
            }
            // No answer started the frame (the first one, a restored state, an application
            // that does not answer): it starts here.
            StartFrame(true);
        }
        Finish();
        frame_pending = false;
#ifdef VITA_DIAGNOSTICS
        {
            std::scoped_lock lk{frame_cond};
            interrupt_at = Common::ProcessClock::now();
        }
#endif
        awaiting_answer = true;
        // TODO(merry): Signal all the other interrupts as appropriate.
        interrupt_handler(InterruptType::Pipe, DspPipe::Audio);
    }
    tick_waits = 0;

    // Reschedule recurrent event
    core_timing.ScheduleEvent(frame_ticks - cycles_late, tick_event);
}

void DspHle::DescribeWorker(char* out, std::size_t size) {
    const auto* const d = static_cast<const Impl*>(live_dsp.load(std::memory_order_acquire));
    if (d == nullptr) {
        std::snprintf(out, size, "no DSP thread");
        return;
    }
    d->DescribeWorker(out, size);
}

void DspHle::Impl::DescribeWorker(char* out, std::size_t size) const {
    const u32 at = worker_at.load(std::memory_order_relaxed);
    const int used = std::snprintf(
        out, size, "frames posted %u, done %u; at %s %u; source loop passes %u", frames_posted,
        frames_done,
        at < HLE::num_sources ? "source"
        : at == MixersAt      ? "mixers"
        : at == OutputAt      ? "output"
                              : "idle",
        at, HLE::source_loop_passes.load(std::memory_order_relaxed));
    if (at < HLE::num_sources && used > 0 && static_cast<std::size_t>(used) + 3 < size) {
        std::snprintf(out + used, size - used, "; ");
        sources[at].Describe(out + used + 2, size - used - 2);
    }
}

DspHle::DspHle(Core::System& system, Memory::MemorySystem& memory, Core::Timing& timing)
    : DspInterface(system), impl(std::make_unique<Impl>(*this, memory, timing)) {}
DspHle::~DspHle() = default;

u16 DspHle::RecvData(u32 register_number) {
    return impl->RecvData(register_number);
}

bool DspHle::RecvDataIsReady(u32 register_number) const {
    return impl->RecvDataIsReady(register_number);
}

void DspHle::SetSemaphore(u16 semaphore_value) {
    // The application's "shared memory written": the frame counters say which region is
    // newer, and the signal that it is done with the regions until the next interrupt (the
    // next frame starts here).
    impl->Answered();
#ifdef VITA_DIAGNOSTICS
    impl->NoteSemaphore();
#endif
}

#ifdef VITA_DIAGNOSTICS
DspHle::Stats DspHle::TakeStats() {
    return impl->TakeStats();
}
#endif

std::vector<u8> DspHle::PipeRead(DspPipe pipe_number, std::size_t length) {
    return impl->PipeRead(pipe_number, length);
}

size_t DspHle::GetPipeReadableSize(DspPipe pipe_number) const {
    return impl->GetPipeReadableSize(pipe_number);
}

void DspHle::PipeWrite(DspPipe pipe_number, std::span<const u8> buffer) {
    impl->PipeWrite(pipe_number, buffer);
}

void DspHle::SetInterruptHandler(
    std::function<void(Service::DSP::InterruptType type, DspPipe pipe)> handler) {
    impl->SetInterruptHandler(handler);
};

void DspHle::LoadComponent(std::span<const u8> component_data) {
    // HLE doesn't need DSP program. Only log some info here
    LOG_INFO(Service_DSP, "Firmware hash: {:#018x}",
             Common::ComputeHash64<Common::HashAlgo64::CityHash>(component_data.data(),
                                                                 component_data.size()));
    // Some versions of the firmware have the location of DSP structures listed here.
    if (component_data.size() > 0x37C) {
        LOG_INFO(
            Service_DSP, "Structures hash: {:#018x}",
            Common::ComputeHash64<Common::HashAlgo64::CityHash>(component_data.data() + 0x340, 60));
    }
}

void DspHle::UnloadComponent() {
    // Do nothing
}

} // namespace AudioCore
