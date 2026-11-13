// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <atomic>
#include <thread>

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
#include "common/emu_prof.h"
#include "common/pipeline_stats.h"
#include "common/settings.h"
#include "common/thread.h"
#include "common/vita_tuning.h"
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
    HLE::SharedMemory& ReadRegion();
    HLE::SharedMemory& WriteRegion();

    StereoFrame16 GenerateCurrentFrame();
    bool Tick();
    void AudioTickCallback(s64 cycles_late);

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

    std::unique_ptr<HLE::DecoderBase> aac_decoder{};

    std::function<void(Service::DSP::InterruptType type, DspPipe pipe)> interrupt_handler{};

    // The odd sources' Tick on a second thread (Common::dsp_parallel): sources are
    // independent - each reads its own configuration and buffers and writes its own status -
    // so half of them run on an idle core while the emulation thread runs the other half; the
    // mixing stays on this thread, in source order, so the output is the same.
    void SourceWorkerLoop();
    // Pipelined ticks (Common::dsp_pipelined): the tick hands the frame to a worker and an event
    // half an audio frame later takes the result, outputs it and raises the interrupt. The game
    // runs on in between and writes only the other shared-memory region (it flips regions after
    // the interrupt, which has not come yet). Every other entry point joins the worker first.
    void PipeWorkerLoop();
    void JoinPipe() const;
    void CollectCallback();
    std::thread pipe_worker;
    Common::Event pipe_start;
    std::atomic<bool> pipe_busy{false};
    std::atomic<bool> pipe_quit{false};
    StereoFrame16 pipe_frame{};
    bool pipe_pending = false;
    Core::TimingEventType* collect_event{};
    void TickSources(HLE::SharedMemory& read, HLE::SharedMemory& write, std::size_t first);
    std::thread source_worker;
    Common::Event source_start;
    std::atomic<bool> source_done{false};
    std::atomic<bool> source_quit{false};
    HLE::SharedMemory* job_read{};
    HLE::SharedMemory* job_write{};

    template <class Archive>
    void serialize(Archive& ar, const unsigned int) {
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
    core_timing.ScheduleEvent(audio_frame_ticks, tick_event);
    collect_event = core_timing.RegisterEvent(
        "AudioCore::DspHle::collect_event", [this](u64, s64) { this->CollectCallback(); });
}

void DspHle::Impl::JoinPipe() const {
    while (pipe_busy.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

void DspHle::Impl::PipeWorkerLoop() {
    Common::SetCurrentThreadRole(Common::ThreadRole::ShaderCompiler);
    Common::SetCurrentThreadName("DspTick");
    Common::EmuProf::thread_disabled = true;
    for (;;) {
        pipe_start.Wait();
        if (pipe_quit.load(std::memory_order_acquire)) {
            return;
        }
        pipe_frame = GenerateCurrentFrame();
        pipe_busy.store(false, std::memory_order_release);
    }
}

void DspHle::Impl::CollectCallback() {
    JoinPipe();
    if (!pipe_pending) {
        return;
    }
    pipe_pending = false;
    parent.OutputFrame(std::move(pipe_frame));
    interrupt_handler(InterruptType::Pipe, DspPipe::Audio);
}

DspHle::Impl::~Impl() {
    core_timing.UnscheduleEvent(tick_event, 0);
    core_timing.UnscheduleEvent(collect_event, 0);
    if (pipe_worker.joinable()) {
        JoinPipe();
        pipe_quit.store(true, std::memory_order_release);
        pipe_start.Set();
        pipe_worker.join();
    }
    if (source_worker.joinable()) {
        source_quit.store(true, std::memory_order_release);
        source_start.Set();
        source_worker.join();
    }
}

void DspHle::Impl::TickSources(HLE::SharedMemory& read, HLE::SharedMemory& write,
                               std::size_t first) {
    for (std::size_t i = first; i < HLE::num_sources; i += 2) {
        write.source_statuses.status[i] =
            sources[i].Tick(read.source_configurations.config[i], read.adpcm_coefficients.coeff[i]);
    }
}

void DspHle::Impl::SourceWorkerLoop() {
    // The shader compiler's core: idle while a title runs (libgxm's display queue thread is the
    // only other tenant), where the emulation thread's core carries the guest too.
    Common::SetCurrentThreadRole(Common::ThreadRole::ShaderCompiler);
    Common::SetCurrentThreadName("DspSources");
    for (;;) {
        source_start.Wait();
        if (source_quit.load(std::memory_order_acquire)) {
            return;
        }
        TickSources(*job_read, *job_write, 1);
        source_done.store(true, std::memory_order_release);
    }
}

DspState DspHle::Impl::GetDspState() const {
    JoinPipe();
    return dsp_state;
}

u16 DspHle::Impl::RecvData(u32 register_number) {
    JoinPipe();
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
    JoinPipe();
    ASSERT_MSG(register_number == 0, "Unknown register_number {}", register_number);
    return true;
}

std::vector<u8> DspHle::Impl::PipeRead(DspPipe pipe_number, std::size_t length) {
    JoinPipe();
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
    JoinPipe();
    const std::size_t pipe_index = static_cast<std::size_t>(pipe_number);

    if (pipe_index >= num_dsp_pipe) {
        LOG_ERROR(Audio_DSP, "pipe_number = {} invalid", pipe_index);
        return 0;
    }

    return pipe_data[pipe_index].size();
}

void DspHle::Impl::PipeWrite(DspPipe pipe_number, std::span<const u8> buffer) {
    JoinPipe();
    switch (pipe_number) {
    case DspPipe::Audio: {
        if (buffer.size() != 4) {
            LOG_ERROR(Audio_DSP, "DspPipe::Audio: Unexpected buffer length {} was written",
                      buffer.size());
            return;
        }

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
    JoinPipe();
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
    JoinPipe();
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
    JoinPipe();
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

HLE::SharedMemory& DspHle::Impl::ReadRegion() {
    return CurrentRegionIndex() == 0 ? dsp_memory->region_0 : dsp_memory->region_1;
}

HLE::SharedMemory& DspHle::Impl::WriteRegion() {
    return CurrentRegionIndex() != 0 ? dsp_memory->region_0 : dsp_memory->region_1;
}

StereoFrame16 DspHle::Impl::GenerateCurrentFrame() {
    HLE::SharedMemory& read = ReadRegion();
    HLE::SharedMemory& write = WriteRegion();

    std::array<QuadFrame32, 3> intermediate_mixes = {};

    namespace EP = Common::EmuProf;
    u64 prof_t = EP::enabled ? Common::PipelineStats::NowUs() : 0;
    const auto prof = [&](const char* key) {
        if (EP::enabled) {
            const u64 now = Common::PipelineStats::NowUs();
            EP::Add(key, now - prof_t);
            prof_t = now;
        }
    };
    // Generate intermediate mixes
    if (Common::dsp_parallel && !Common::dsp_pipelined) {
        if (!source_worker.joinable()) {
            source_worker = std::thread([this] { SourceWorkerLoop(); });
        }
        job_read = &read;
        job_write = &write;
        source_done.store(false, std::memory_order_relaxed);
        source_start.Set();
        TickSources(read, write, 0);
        while (!source_done.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    } else {
        TickSources(read, write, 0);
        TickSources(read, write, 1);
    }
    prof(" .dsp sources");
    for (std::size_t i = 0; i < HLE::num_sources; i++) {
        for (std::size_t mix = 0; mix < 3; mix++) {
            sources[i].MixInto(intermediate_mixes[mix], mix);
        }
    }
    prof(" .dsp mixinto");

    // Generate final mix
    write.dsp_status = mixers.Tick(read.dsp_configuration, read.intermediate_mix_samples,
                                   write.intermediate_mix_samples, intermediate_mixes);

    prof(" .dsp mixers");
    StereoFrame16 output_frame = mixers.GetOutput();

    // Write current output frame to the shared memory region
    for (std::size_t samplei = 0; samplei < output_frame.size(); samplei++) {
        for (std::size_t channeli = 0; channeli < output_frame[0].size(); channeli++) {
            write.final_samples.pcm16[samplei][channeli] = s16_le(output_frame[samplei][channeli]);
        }
    }

    return output_frame;
}

bool DspHle::Impl::Tick() {
    bool is_on = GetDspState() == DspState::On;

    if (is_on) {
        StereoFrame16 current_frame = {};

        // TODO: Check dsp::DSP semaphore (which indicates emulated application has finished writing
        // to shared memory region)
        current_frame = GenerateCurrentFrame();

        const u64 out_t0 = Common::EmuProf::enabled ? Common::PipelineStats::NowUs() : 0;
        parent.OutputFrame(std::move(current_frame));
        if (Common::EmuProf::enabled) {
            Common::EmuProf::Add(" .dsp output", Common::PipelineStats::NowUs() - out_t0);
        }
    }

    return is_on;
}

void DspHle::Impl::AudioTickCallback(s64 cycles_late) {
    if (Common::dsp_pipelined) {
        JoinPipe();
        if (pipe_pending) {
            // The previous frame's collect has not run (it is always scheduled before this
            // tick); deliver it now so no frame is lost.
            CollectCallback();
        }
        if (GetDspState() == DspState::On) {
            if (!pipe_worker.joinable()) {
                pipe_worker = std::thread([this] { PipeWorkerLoop(); });
            }
            pipe_pending = true;
            pipe_busy.store(true, std::memory_order_release);
            pipe_start.Set();
            core_timing.ScheduleEvent(static_cast<s64>(audio_frame_ticks / 2), collect_event);
        }
    } else if (Tick()) {
        // TODO(merry): Signal all the other interrupts as appropriate.
        interrupt_handler(InterruptType::Pipe, DspPipe::Audio);
    }

    // Reschedule recurrent event
    const double time_scale =
        Settings::values.enable_realtime_audio
            ? std::max(0.01, // Arbitrary small value to prevent time_scale from approaching zero
                       Core::System::GetInstance().GetStableFrameTimeScale())
            : 1.0;
    s64 adjusted_ticks = static_cast<s64>(audio_frame_ticks / time_scale - cycles_late);
    core_timing.ScheduleEvent(adjusted_ticks, tick_event);
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
    // Do nothing in HLE
}

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
