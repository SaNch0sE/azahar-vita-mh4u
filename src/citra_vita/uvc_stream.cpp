// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <type_traits>
#include <vector>
#include <arm_neon.h>
#include "azahar_native.h"
#include "citra_vita/i18n.h"
#include "citra_vita/uvc_stream.h"
#include "common/logging/log.h"
#include "common/named_thread.h"
#include "common/thread.h"

namespace VitaFrontend::UvcStream {
namespace {

constexpr u32 ScreenRows = 240;

std::atomic<int> streamed{0};
/// 0 not started, 1 started, -1 refused (the port could not be had): once a session.
std::atomic<int> started{0};
/// Problem()'s text (English, which the menus translate), set once a start fails.
std::atomic<const char*> problem{nullptr};
std::atomic<bool> takeover{false};

constexpr const char* TakenText =
    I18n::Translatable("Not started: another plugin is using the USB port. Turn USB video off and "
                       "on again to take it over.");

/// The latest frame offered, for the thread (under the condition's lock).
struct Offered {
    const u8* pixels = nullptr;
    u32 stride = 0;
    u32 columns = 0;
    u32 format = 0;
    bool fresh = false;
};

struct Stream {
    Common::CoreCondition gate;
    Offered offered;
    bool stop = false;
    std::vector<u8> copy;
    std::vector<u8> yuy2;
    Common::NamedThread thread;
};

Stream& TheStream() {
    static Stream stream;
    return stream;
}

/// Eight pixels, a byte a channel.
struct Rgb8 {
    uint8x8_t r, g, b;
};

/// Eight pixels of a PICA framebuffer format (Pica::PixelFormat: RGBA8, RGB8, RGB565, RGB5A1,
/// RGBA4), as the GPU stores them, widened to 8 bits a channel (the low bits repeating the high).
template <u32 Format>
Rgb8 Load8(const u8* p) {
    if constexpr (Format == 0) {
        const uint8x8x4_t v = vld4_u8(p);
        return {v.val[3], v.val[2], v.val[1]};
    } else if constexpr (Format == 1) {
        const uint8x8x3_t v = vld3_u8(p);
        return {v.val[2], v.val[1], v.val[0]};
    } else {
        const uint16x8_t v = vld1q_u16(reinterpret_cast<const u16*>(p));
        const auto widen = [](uint16x8_t x, int bits) {
            // 5 and 6 bits: the high bits again below; 4 bits: the nibble twice.
            const uint16x8_t up = bits == 6   ? vorrq_u16(vshlq_n_u16(x, 2), vshrq_n_u16(x, 4))
                                  : bits == 5 ? vorrq_u16(vshlq_n_u16(x, 3), vshrq_n_u16(x, 2))
                                              : vorrq_u16(vshlq_n_u16(x, 4), x);
            return vmovn_u16(up);
        };
        if constexpr (Format == 2) {
            return {widen(vshrq_n_u16(v, 11), 5),
                    widen(vandq_u16(vshrq_n_u16(v, 5), vdupq_n_u16(0x3f)), 6),
                    widen(vandq_u16(v, vdupq_n_u16(0x1f)), 5)};
        } else if constexpr (Format == 3) {
            return {widen(vshrq_n_u16(v, 11), 5),
                    widen(vandq_u16(vshrq_n_u16(v, 6), vdupq_n_u16(0x1f)), 5),
                    widen(vandq_u16(vshrq_n_u16(v, 1), vdupq_n_u16(0x1f)), 5)};
        } else {
            return {widen(vshrq_n_u16(v, 12), 4),
                    widen(vandq_u16(vshrq_n_u16(v, 8), vdupq_n_u16(0xf)), 4),
                    widen(vandq_u16(vshrq_n_u16(v, 4), vdupq_n_u16(0xf)), 4)};
        }
    }
}

// BT.601 in studio range, the colour matching uvc.c announces.
uint8x8_t Luma(const Rgb8& c) {
    uint16x8_t y = vmull_u8(c.r, vdup_n_u8(66));
    y = vmlal_u8(y, c.g, vdup_n_u8(129));
    y = vmlal_u8(y, c.b, vdup_n_u8(25));
    return vadd_u8(vshrn_n_u16(vaddq_u16(y, vdupq_n_u16(128)), 8), vdup_n_u8(16));
}

/// One chroma of BT.601's: ((a*r + b*g + c*b + 128) >> 8) + 128, its weights signed.
uint8x8_t Chroma(int16x8_t r, int16x8_t g, int16x8_t b, s16 wr, s16 wg, s16 wb) {
    int16x8_t v = vmulq_n_s16(r, wr);
    v = vmlaq_n_s16(v, g, wg);
    v = vmlaq_n_s16(v, b, wb);
    const int16x8_t bias = vdupq_n_s16(128);
    return vqmovun_s16(vaddq_s16(vshrq_n_s16(vaddq_s16(v, bias), 8), bias));
}

/// The frame upright: columns of the screen become the picture's x, each column's 240 pixels its y
/// from the bottom up, two columns at a time for YUY2's shared chroma, eight pixels of each at a
/// time (a scalar conversion took more than the USB thread's time). The bottom screen sits in the
/// middle of the 400 columns, black either side.
template <u32 Format>
void ConvertColumns(const Offered& f, const u8* src, u8* out) {
    constexpr u32 Bpp = Format == 0 ? 4 : Format == 1 ? 3 : 2;
    constexpr u32 Row = Width * 2;
    const u32 columns = std::min(f.columns, Width);
    const u32 left = (Width - columns) / 2 & ~1u;
    // The sides past the screen, black: Y 16, U and V 128.
    for (u32 y = 0; y < Height; y++) {
        u32* const row = reinterpret_cast<u32*>(out + y * Row);
        std::fill(row, row + left / 2, 0x80108010u);
        std::fill(row + (left + columns) / 2, row + Width / 2, 0x80108010u);
    }
    for (u32 c = 0; c + 1 < columns; c += 2) {
        const u8* const a = src + c * f.stride;
        const u8* const b = a + f.stride;
        u8* const column = out + (left + c) * 2;
        for (u32 i = 0; i < ScreenRows; i += 8) {
            const Rgb8 p0 = Load8<Format>(a + i * Bpp);
            const Rgb8 p1 = Load8<Format>(b + i * Bpp);
            const auto mean = [](uint8x8_t x, uint8x8_t y) {
                return vreinterpretq_s16_u16(vmovl_u8(vhadd_u8(x, y)));
            };
            const int16x8_t r = mean(p0.r, p1.r), g = mean(p0.g, p1.g), bl = mean(p0.b, p1.b);
            const uint8x8x2_t yu = vzip_u8(Luma(p0), Chroma(r, g, bl, -38, -74, 112));
            const uint8x8x2_t yv = vzip_u8(Luma(p1), Chroma(r, g, bl, 112, -94, -18));
            // Y0 U Y1 V a pixel pair: word k for the column pair's pixel i + k.
            const uint16x4x2_t lo =
                vzip_u16(vreinterpret_u16_u8(yu.val[0]), vreinterpret_u16_u8(yv.val[0]));
            const uint16x4x2_t hi =
                vzip_u16(vreinterpret_u16_u8(yu.val[1]), vreinterpret_u16_u8(yv.val[1]));
            const uint32x2_t w0 = vreinterpret_u32_u16(lo.val[0]);
            const uint32x2_t w1 = vreinterpret_u32_u16(lo.val[1]);
            const uint32x2_t w2 = vreinterpret_u32_u16(hi.val[0]);
            const uint32x2_t w3 = vreinterpret_u32_u16(hi.val[1]);
            // Pixel i + k on row 239 - i - k, the rows upwards.
            u8* at = column + (ScreenRows - 1 - i) * Row;
            const auto put = [&](uint32x2_t w, auto lane) {
                vst1_lane_u32(reinterpret_cast<u32*>(at), w, decltype(lane)::value);
                at -= Row;
            };
            put(w0, std::integral_constant<int, 0>{});
            put(w0, std::integral_constant<int, 1>{});
            put(w1, std::integral_constant<int, 0>{});
            put(w1, std::integral_constant<int, 1>{});
            put(w2, std::integral_constant<int, 0>{});
            put(w2, std::integral_constant<int, 1>{});
            put(w3, std::integral_constant<int, 0>{});
            put(w3, std::integral_constant<int, 1>{});
        }
    }
}

void Convert(const Offered& f, const u8* src, u8* out) {
    switch (f.format) {
    case 0:
        return ConvertColumns<0>(f, src, out);
    case 1:
        return ConvertColumns<1>(f, src, out);
    case 2:
        return ConvertColumns<2>(f, src, out);
    case 3:
        return ConvertColumns<3>(f, src, out);
    default:
        return ConvertColumns<4>(f, src, out);
    }
}

void Loop(std::stop_token stop) {
    Common::SetCurrentThreadRole(Common::ThreadRole::UsbVideo);
    Stream& s = TheStream();
    for (;;) {
        Offered f;
        {
            std::scoped_lock lock{s.gate};
            while (!s.offered.fresh && !s.stop && !stop.stop_requested()) {
                s.gate.Wait();
            }
            if (s.stop || stop.stop_requested()) {
                return;
            }
            f = s.offered;
            s.offered.fresh = false;
        }
        // Out of guest memory in one pass (often uncached: column by column it would cost a
        // bus read a pixel), then converted from the copy.
        const std::size_t bytes = static_cast<std::size_t>(f.stride) * f.columns;
        s.copy.resize(bytes);
        std::memcpy(s.copy.data(), f.pixels, bytes);
        Convert(f, s.copy.data(), s.yuy2.data());
        const int sent = azaharUvcFrame(s.yuy2.data(), static_cast<u32>(s.yuy2.size()));
        static bool first = true;
        if (first) {
            first = false;
            LOG_INFO(Frontend, "usb video: the first frame ({} columns, format {}) handed over: {}",
                     f.columns, f.format, sent);
        }
        if (sent < 0) {
            LOG_ERROR(Frontend, "usb video: a frame was not taken: {:#010x}",
                      static_cast<u32>(sent));
        }
    }
}

/// The device up, at the first frame a title draws with a screen chosen (the settings load
/// before azaharnative does).
bool Start(int screen) {
    int expected = 0;
    if (!started.compare_exchange_strong(expected, -1, std::memory_order_acq_rel)) {
        return expected == 1;
    }
    const int r =
        azaharUvcStart(Width, Height, takeover.load(std::memory_order_acquire) ? 1 : 0);
    LOG_INFO(Frontend, "usb video: {} screen, start {:#010x}", screen == 1 ? "top" : "bottom",
             static_cast<u32>(r));
    if (r < 0) {
        problem.store(
            r == AZAHAR_ERR_TAKEN
                ? TakenText
                : I18n::Translatable(
                      "Not started: the USB port could not be set up. Reboot and try again."),
            std::memory_order_release);
        return false;
    }
    Stream& s = TheStream();
    s.yuy2.resize(Width * Height * 2);
    s.thread = Common::NamedThread{Common::ThreadCfg{"usb video", 64 * 1024},
                                   [](std::stop_token stop) { Loop(stop); }};
    started.store(1, std::memory_order_release);
    return true;
}

} // Anonymous namespace

void SetScreen(int screen) {
    streamed.store(screen, std::memory_order_relaxed);
}

int Screen() {
    return streamed.load(std::memory_order_relaxed);
}

bool PortTaken() {
    return azaharUvcPortTaken() == 1;
}

void AllowTakeover() {
    takeover.store(true, std::memory_order_release);
    // A start held back for the port goes again at the next frame.
    const char* held = TakenText;
    if (problem.compare_exchange_strong(held, nullptr, std::memory_order_acq_rel)) {
        started.store(0, std::memory_order_release);
    }
}

bool TakeoverAllowed() {
    return takeover.load(std::memory_order_acquire);
}

const char* Problem() {
    return problem.load(std::memory_order_acquire);
}

bool Streams(u32 index) {
    const int screen = streamed.load(std::memory_order_relaxed);
    return screen != 0 && static_cast<u32>(screen - 1) == index && azaharUvcStreaming();
}

void Offer(u32 index, const u8* pixels, u32 stride, u32 columns, u32 format) {
    const int screen = streamed.load(std::memory_order_relaxed);
    if (screen == 0 || static_cast<u32>(screen - 1) != index || pixels == nullptr || format > 4 ||
        !Start(screen) || !azaharUvcStreaming()) {
        return;
    }
    Stream& s = TheStream();
    std::scoped_lock lock{s.gate};
    s.offered = {pixels, stride, columns, format, true};
    s.gate.NotifyAll();
}

} // namespace VitaFrontend::UvcStream
