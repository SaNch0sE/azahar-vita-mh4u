// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cstring>

#include <fmt/format.h>
#include <psp2/audioout.h>

#include "citra_vita/banner_sound.h"
#include "common/logging/log.h"
#include "common/thread.h"

namespace VitaFrontend::Banner {

namespace {

constexpr u32 OutputRate = 48000;
/// Frames a sceAudioOutOutput, as the emulator's sink.
constexpr u32 OutputFrames = 1024;
/// The longest jingle played, in output frames.
constexpr u64 MaxJingleFrames = 10 * OutputRate;

/// The CWAV's little-endian values; a read past its end reads 0 and clears `ok`.
struct Cwav {
    std::span<const u8> d;
    bool ok = true;

    template <typename T>
    T Read(std::size_t at) {
        if (at > d.size() || sizeof(T) > d.size() - at) {
            ok = false;
            return T{};
        }
        T v;
        std::memcpy(&v, d.data() + at, sizeof(T));
        return v;
    }
};

} // Anonymous namespace

std::vector<s16> DecodeJingle(std::span<const u8> cwav) {
    if (cwav.size() < 0x20 || std::memcmp(cwav.data(), "CWAV", 4) != 0) {
        return {};
    }
    // A jingle this does not decode is not played.
    const auto skip = [](const auto& why) {
        LOG_WARNING(Frontend, "banner: the jingle is not played: {}", why);
        return std::vector<s16>{};
    };
    Cwav w{cwav};
    // The blocks' references: INFO (0x7000) and DATA (0x7001).
    u32 info = 0, data = 0;
    const u16 blocks = w.Read<u16>(0x10);
    for (u32 i = 0; i < blocks; i++) {
        const u16 id = w.Read<u16>(0x14 + 12 * i);
        const u32 offset = w.Read<u32>(0x14 + 12 * i + 4);
        if (id == 0x7000) {
            info = offset;
        } else if (id == 0x7001) {
            data = offset;
        }
    }
    if (info == 0 || data == 0 || w.Read<u32>(info) != 0x4f464e49 || // "INFO"
        w.Read<u32>(data) != 0x41544144) {                           // "DATA"
        return skip("no INFO or DATA");
    }
    const u8 encoding = w.Read<u8>(info + 8);
    const u32 rate = w.Read<u32>(info + 0xc);
    const u32 count = w.Read<u32>(info + 0x14); // the loop's end: every sample
    const u32 table = info + 0x1c;
    const u32 channels = w.Read<u32>(table);
    if (!w.ok || rate < 1000 || rate > 96000 || channels < 1 || channels > 8 || encoding > 2 ||
        u64{count} * OutputRate / rate > MaxJingleFrames) {
        return skip(fmt::format("encoding {}, {} channels, {} samples at {} Hz", encoding,
                                channels, count, rate));
    }

    std::vector<std::vector<s16>> decoded;
    for (u32 c = 0; c < std::min(channels, 2u); c++) {
        const u32 ci = table + w.Read<u32>(table + 4 + 8 * c + 4);
        const u32 samples = data + 8 + w.Read<u32>(ci + 4);
        std::vector<s16> out(count);
        switch (encoding) {
        case 2: { // DSP-ADPCM: frames of a header byte and 14 four-bit samples
            const u32 adpcm = ci + w.Read<u32>(ci + 0xc);
            std::array<s16, 16> coefs;
            for (u32 k = 0; k < 16; k++) {
                coefs[k] = w.Read<s16>(adpcm + 2 * k);
            }
            s32 h1 = w.Read<s16>(adpcm + 0x22), h2 = w.Read<s16>(adpcm + 0x24);
            u32 p = samples;
            for (u32 i = 0; i < count && w.ok;) {
                const u8 ps = w.Read<u8>(p++);
                if (ps >> 4 >= 8) {
                    return skip(fmt::format("a frame of coefficients {}", ps >> 4));
                }
                const s32 scale = 1 << (ps & 15);
                const s32 c1 = coefs[(ps >> 4) * 2], c2 = coefs[(ps >> 4) * 2 + 1];
                for (u32 k = 0; k < 14 && i < count; k++) {
                    const u8 byte = w.Read<u8>(p + k / 2);
                    s32 nibble = k % 2 == 0 ? byte >> 4 : byte & 15;
                    if (nibble >= 8) {
                        nibble -= 16;
                    }
                    const s32 s = std::clamp(
                        ((nibble * scale) << 11) + c1 * h1 + c2 * h2 + 1024 >> 11, -32768, 32767);
                    out[i++] = static_cast<s16>(s);
                    h2 = h1;
                    h1 = s;
                }
                p += 7;
            }
            break;
        }
        case 1: // PCM16
            for (u32 i = 0; i < count && w.ok; i++) {
                out[i] = w.Read<s16>(samples + 2 * i);
            }
            break;
        case 0: // PCM8
            for (u32 i = 0; i < count && w.ok; i++) {
                out[i] = static_cast<s16>(w.Read<s8>(samples + i) * 256);
            }
            break;
        }
        if (!w.ok) {
            return skip("its samples run past its end");
        }
        decoded.push_back(std::move(out));
    }

    // To the port's rate, linearly; one channel to both sides.
    const s16* const left = decoded[0].data();
    const s16* const right = decoded.size() > 1 ? decoded[1].data() : left;
    const u64 frames = u64{count} * OutputRate / rate;
    std::vector<s16> pcm(frames * 2);
    for (u64 f = 0; f < frames; f++) {
        const double at = static_cast<double>(f) * rate / OutputRate;
        const u32 i = static_cast<u32>(at);
        const u32 j = std::min(i + 1, count - 1);
        const float t = static_cast<float>(at - i);
        pcm[f * 2] = static_cast<s16>(left[i] + (left[j] - left[i]) * t);
        pcm[f * 2 + 1] = static_cast<s16>(right[i] + (right[j] - right[i]) * t);
    }
    return pcm;
}

JinglePlayer::~JinglePlayer() {
    StopAndWait();
}

void JinglePlayer::Play(std::vector<s16> frames) {
    // The one before, told to stop a banner's load ago at the latest, has ended by now.
    StopAndWait();
    if (frames.empty()) {
        return;
    }
    pcm = std::move(frames);
    thread = Common::NamedThread{
        Common::ThreadCfg{"banner jingle", 64 * 1024}, [this](std::stop_token stop) {
            Common::SetCurrentThreadRole(Common::ThreadRole::AudioSink);
            const int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, OutputFrames,
                                                 OutputRate, SCE_AUDIO_OUT_MODE_STEREO);
            if (port < 0) {
                LOG_ERROR(Frontend, "banner: no audio port for the jingle ({:#x})",
                          static_cast<u32>(port));
                return;
            }
            int volume[2] = {SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB};
            sceAudioOutSetVolume(port,
                                 static_cast<SceAudioOutChannelFlag>(SCE_AUDIO_VOLUME_FLAG_L_CH |
                                                                     SCE_AUDIO_VOLUME_FLAG_R_CH),
                                 volume);
            std::array<s16, OutputFrames * 2> buffer;
            for (std::size_t at = 0; at < pcm.size() && !stop.stop_requested();
                 at += buffer.size()) {
                const std::size_t n = std::min(buffer.size(), pcm.size() - at);
                std::copy_n(pcm.begin() + at, n, buffer.begin());
                std::fill(buffer.begin() + n, buffer.end(), s16{0});
                sceAudioOutOutput(port, buffer.data());
            }
            sceAudioOutOutput(port, nullptr);
            sceAudioOutReleasePort(port);
        }};
}

void JinglePlayer::Stop() {
    if (thread.joinable()) {
        thread.request_stop();
    }
}

void JinglePlayer::StopAndWait() {
    if (thread.joinable()) {
        thread.request_stop();
        thread.join();
    }
}

} // namespace VitaFrontend::Banner
