// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <mutex>
#include <psp2/audioin.h>
#include "audio_core/vita_input.h"
#include "common/logging/log.h"
#include "common/mutex.h"
#include "common/named_thread.h"
#include "common/thread.h"

namespace AudioCore {

struct VitaInput::Impl {
    static constexpr int PortRate = 16000;
    /// Samples a sceAudioInInput returns: 16 ms.
    static constexpr int Grain = 256;
    /// What the thread keeps for Read at most: half a second, the newest.
    static constexpr std::size_t MaxKept = PortRate / 2;

    int port = -1;
    bool sampling = false;
    Common::NamedThread thread;

    Common::Mutex mutex;
    /// Samples the thread took since the last Read, under mutex.
    std::vector<s16> captured;

    // Read's resampler: the samples not used up yet, and the fractional read position in them.
    std::vector<s16> pending;
    double pos = 0.0;

    void Loop(std::stop_token stop);
};

VitaInput::VitaInput() : impl(std::make_unique<Impl>()) {}

VitaInput::~VitaInput() {
    StopSampling();
}

void VitaInput::Impl::Loop(std::stop_token stop) {
    Common::SetCurrentThreadRole(Common::ThreadRole::Other);
    std::array<s16, Grain> block{};
    while (!stop.stop_requested()) {
        const int r = sceAudioInInput(port, block.data());
        if (r < 0) {
            LOG_CRITICAL(Audio, "sceAudioInInput failed: {:#x}; the microphone stops",
                         static_cast<u32>(r));
            return;
        }
        std::scoped_lock lock{mutex};
        captured.insert(captured.end(), block.begin(), block.end());
        if (captured.size() > MaxKept) {
            captured.erase(captured.begin(),
                           captured.begin() + static_cast<std::ptrdiff_t>(captured.size() - MaxKept));
        }
    }
}

void VitaInput::StartSampling(const InputParameters& params) {
    if (IsSampling()) {
        return;
    }
    parameters = params;
    impl->sampling = true;
    impl->pending.clear();
    impl->pos = 0.0;
    {
        std::scoped_lock lock{impl->mutex};
        impl->captured.clear();
    }
    // The raw port: the voice port's processing is for speech, and games listen for blowing too.
    impl->port = sceAudioInOpenPort(SCE_AUDIO_IN_PORT_TYPE_RAW, Impl::Grain, Impl::PortRate,
                                    SCE_AUDIO_IN_PARAM_FORMAT_S16_MONO);
    if (impl->port < 0) {
        LOG_WARNING(Audio, "sceAudioInOpenPort failed: {:#x}; the game hears silence",
                    static_cast<u32>(impl->port));
        return;
    }
    impl->thread = Common::NamedThread{Common::ThreadCfg{"microphone", 64 * 1024},
                                       [this](std::stop_token stop) { impl->Loop(stop); }};
}

void VitaInput::StopSampling() {
    impl->sampling = false;
    if (impl->thread.joinable()) {
        impl->thread.request_stop();
        impl->thread.join();
    }
    if (impl->port >= 0) {
        sceAudioInReleasePort(impl->port);
        impl->port = -1;
    }
}

bool VitaInput::IsSampling() {
    return impl->sampling;
}

void VitaInput::AdjustSampleRate(u32 sample_rate) {
    parameters.sample_rate = sample_rate;
}

Samples VitaInput::Read() {
    if (!impl->sampling || impl->port < 0 || parameters.sample_rate == 0) {
        return {};
    }
    {
        std::scoped_lock lock{impl->mutex};
        impl->pending.insert(impl->pending.end(), impl->captured.begin(), impl->captured.end());
        impl->captured.clear();
    }
    // Linear interpolation from the port's 16 kHz to the game's rate (the 3DS's 8180 to 32730
    // Hz), then the game's sample size and sign.
    const double step = static_cast<double>(Impl::PortRate) / parameters.sample_rate;
    const std::size_t bytes = parameters.sample_size / 8;
    const bool is_signed = parameters.sign == Signedness::Signed;
    const std::vector<s16>& in = impl->pending;
    Samples out;
    while (static_cast<std::size_t>(impl->pos) + 1 < in.size() &&
           out.size() + bytes <= parameters.buffer_size) {
        const std::size_t base = static_cast<std::size_t>(impl->pos);
        const float frac = static_cast<float>(impl->pos - static_cast<double>(base));
        const s16 v = static_cast<s16>(in[base] + (in[base + 1] - in[base]) * frac);
        if (bytes == 2) {
            const u16 w = is_signed ? static_cast<u16>(v) : static_cast<u16>(v + 0x8000);
            out.push_back(static_cast<u8>(w));
            out.push_back(static_cast<u8>(w >> 8));
        } else {
            const s8 b = static_cast<s8>(v >> 8);
            out.push_back(is_signed ? static_cast<u8>(b) : static_cast<u8>(b + 0x80));
        }
        impl->pos += step;
    }
    const std::size_t used = std::min(static_cast<std::size_t>(impl->pos), in.size());
    impl->pending.erase(impl->pending.begin(),
                        impl->pending.begin() + static_cast<std::ptrdiff_t>(used));
    impl->pos -= static_cast<double>(used);
    return out;
}

std::vector<std::string> ListVitaInputDevices() {
    return {"Vita"};
}

} // namespace AudioCore
