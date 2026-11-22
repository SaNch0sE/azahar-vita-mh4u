// Copyright 2015 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <atomic>
#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif
#ifdef VITA_DIAGNOSTICS
#include <vector>
#endif
#include "common/assert.h"
#include "common/color.h"
#include "common/common_types.h"
#include "common/microprofile.h"
#include "common/vector_math.h"
#include "core/core.h"
#include "core/hle/service/cam/y2r_u.h"
#include "core/hw/y2r.h"
#include "core/memory.h"

namespace HW::Y2R {

using namespace Service::Y2R;

static const std::size_t MAX_TILES = 1024 / 8;
static const std::size_t TILE_SIZE = 8 * 8;
using ImageTile = std::array<u32, TILE_SIZE>;

/// Converts a image strip from the source YUV format into individual 8x8 RGB32 tiles.
template <InputFormat input_format>
static void ConvertYUVToRGB(const u8* input_Y, const u8* input_U, const u8* input_V,
                            ImageTile output[], unsigned int width, unsigned int height,
                            const CoefficientSet& coefficients) {

    for (unsigned int y = 0; y < height; ++y) {
        for (unsigned int x = 0; x < width; ++x) {
            s32 Y;
            s32 U;
            s32 V;
            if constexpr (input_format == InputFormat::YUV422_Indiv8 ||
                          input_format == InputFormat::YUV422_Indiv16) {
                Y = input_Y[y * width + x];
                U = input_U[(y * width + x) / 2];
                V = input_V[(y * width + x) / 2];
            } else if constexpr (input_format == InputFormat::YUV420_Indiv8 ||
                                 input_format == InputFormat::YUV420_Indiv16) {
                Y = input_Y[y * width + x];
                U = input_U[((y / 2) * width + x) / 2];
                V = input_V[((y / 2) * width + x) / 2];
            } else if constexpr (input_format == InputFormat::YUYV422_Interleaved) {
                Y = input_Y[(y * width + x) * 2];
                U = input_Y[(y * width + (x / 2) * 2) * 2 + 1];
                V = input_Y[(y * width + (x / 2) * 2) * 2 + 3];
            } else {
                UNREACHABLE_MSG("Unknown Y2R input format {}", input_format);
                return;
            }

            // This conversion process is bit-exact with hardware, as far as could be tested.
            auto& c = coefficients;
            s32 cY = c[0] * Y;

            s32 r = cY + c[1] * V;
            s32 g = cY - c[2] * V - c[3] * U;
            s32 b = cY + c[4] * U;

            const s32 rounding_offset = 0x18;
            r = (r >> 3) + c[5] + rounding_offset;
            g = (g >> 3) + c[6] + rounding_offset;
            b = (b >> 3) + c[7] + rounding_offset;

            unsigned int tile = x / 8;
            unsigned int tile_x = x % 8;
            u32* out = &output[tile][y * 8 + tile_x];
            *out = ((u32)std::clamp(r >> 5, 0, 0xFF) << 24) |
                   ((u32)std::clamp(g >> 5, 0, 0xFF) << 16) |
                   ((u32)std::clamp(b >> 5, 0, 0xFF) << 8);
        }
    }
}

/// Simulates an incoming CDMA transfer. The N parameter is used to automatically convert 16-bit
/// formats to 8-bit.
template <std::size_t N>
static void ReceiveData(const HostBuffers& memory, u8* output, ConversionBuffer& buf,
                        std::size_t amount_of_data) {
    const u8* input = memory.GetPointer(buf.address);

    std::size_t output_unit = buf.transfer_unit / N;
    ASSERT(amount_of_data % output_unit == 0);

    while (amount_of_data > 0) {
        if constexpr (N == 1) {
            std::memcpy(output, input, output_unit);
        } else {
            for (std::size_t i = 0; i < output_unit; ++i) {
                output[i] = input[i * N];
            }
        }

        output += output_unit;
        input += buf.transfer_unit + buf.gap;

        buf.address += buf.transfer_unit + buf.gap;
        buf.image_size -= buf.transfer_unit;
        amount_of_data -= output_unit;
    }
}

/// Convert intermediate RGB32 format to the final output format while simulating an outgoing CDMA
/// transfer.
template <OutputFormat output_format>
static void SendData(const HostBuffers& memory, const u32* input, ConversionBuffer& buf,
                     int amount_of_data, u8 alpha) {

    u8* output = memory.GetPointer(buf.address);

    while (amount_of_data > 0) {
        u8* unit_end = output + buf.transfer_unit;
        while (output < unit_end) {
            u32 color = *input++;
            Common::Vec4<u8> col_vec{(u8)(color >> 24), (u8)(color >> 16), (u8)(color >> 8), alpha};

            if constexpr (output_format == OutputFormat::RGBA8) {
                Common::Color::EncodeRGBA8(col_vec, output);
                output += 4;
            } else if constexpr (output_format == OutputFormat::RGB8) {
                Common::Color::EncodeRGB8(col_vec, output);
                output += 3;
            } else if constexpr (output_format == OutputFormat::RGB5A1) {
                Common::Color::EncodeRGB5A1(col_vec, output);
                output += 2;
            } else if constexpr (output_format == OutputFormat::RGB565) {
                Common::Color::EncodeRGB565(col_vec, output);
                output += 2;
            } else {
                UNREACHABLE_MSG("Unknown Y2R output format {}", output_format);
            }

            amount_of_data -= 1;
        }

        output += buf.gap;
        buf.address += buf.transfer_unit + buf.gap;
        buf.image_size -= buf.transfer_unit;
    }
}

static const u8 linear_lut[TILE_SIZE] = {
    // clang-format off
     0,  1,  2,  3,  4,  5,  6,  7,
     8,  9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23,
    24, 25, 26, 27, 28, 29, 30, 31,
    32, 33, 34, 35, 36, 37, 38, 39,
    40, 41, 42, 43, 44, 45, 46, 47,
    48, 49, 50, 51, 52, 53, 54, 55,
    56, 57, 58, 59, 60, 61, 62, 63,
    // clang-format on
};

static const u8 morton_lut[TILE_SIZE] = {
    // clang-format off
     0,  1,  4,  5, 16, 17, 20, 21,
     2,  3,  6,  7, 18, 19, 22, 23,
     8,  9, 12, 13, 24, 25, 28, 29,
    10, 11, 14, 15, 26, 27, 30, 31,
    32, 33, 36, 37, 48, 49, 52, 53,
    34, 35, 38, 39, 50, 51, 54, 55,
    40, 41, 44, 45, 56, 57, 60, 61,
    42, 43, 46, 47, 58, 59, 62, 63,
    // clang-format on
};

static void RotateTile0(const ImageTile& input, ImageTile& output, int height,
                        const u8 out_map[64]) {
    for (int i = 0; i < height * 8; ++i) {
        output[out_map[i]] = input[i];
    }
}

static void RotateTile90(const ImageTile& input, ImageTile& output, int height,
                         const u8 out_map[64]) {
    int out_i = 0;
    for (int x = 0; x < 8; ++x) {
        for (int y = height - 1; y >= 0; --y) {
            output[out_map[out_i++]] = input[y * 8 + x];
        }
    }
}

static void RotateTile180(const ImageTile& input, ImageTile& output, int height,
                          const u8 out_map[64]) {
    int out_i = 0;
    for (int i = height * 8 - 1; i >= 0; --i) {
        output[out_map[out_i++]] = input[i];
    }
}

static void RotateTile270(const ImageTile& input, ImageTile& output, int height,
                          const u8 out_map[64]) {
    int out_i = 0;
    for (int x = 8 - 1; x >= 0; --x) {
        for (int y = 0; y < height; ++y) {
            output[out_map[out_i++]] = input[y * 8 + x];
        }
    }
}

static void WriteTileToOutput(u32* output, const ImageTile& tile, int height, int line_stride) {
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < 8; ++x) {
            output[y * line_stride + x] = tile[y * 8 + x];
        }
    }
}

MICROPROFILE_DEFINE(Y2R_PerformConversion, "Y2R", "PerformConversion", MP_RGB(185, 66, 245));

/**
 * Performs a Y2R colorspace conversion.
 *
 * The Y2R hardware implements hardware-accelerated YUV to RGB colorspace conversions. It is most
 * commonly used for video playback or to display camera input to the screen.
 *
 * The conversion process is quite configurable, and can be divided in distinct steps. From
 * observation, it appears that the hardware buffers a single 8-pixel tall strip of image data
 * internally and converts it in one go before writing to the output and loading the next strip.
 *
 * The steps taken to convert one strip of image data are:
 *
 * - The hardware receives data via CDMA (http://3dbrew.org/wiki/Corelink_DMA_Engines), which is
 *   presumably stored in one or more internal buffers. This process can be done in several separate
 *   transfers, as long as they don't exceed the size of the internal image buffer. This allows
 *   flexibility in input strides.
 * - The input data is decoded into a YUV tuple. Several formats are suported, see the `InputFormat`
 *   enum.
 * - The YUV tuple is converted, using fixed point calculations, to RGB. This step can be configured
 *   using a set of coefficients to support different colorspace standards. See `CoefficientSet`.
 * - The strip can be optionally rotated 90, 180 or 270 degrees. Since each strip is processed
 *   independently, this notably rotates each *strip*, not the entire image. This means that for 90
 *   or 270 degree rotations, the output will be in terms of several 8 x height images, and for any
 *   non-zero rotation the strips will have to be re-arranged so that the parts of the image will
 *   not be shuffled together. This limitation makes this a feature of somewhat dubious utility. 90
 *   or 270 degree rotations in images with non-even height don't seem to work properly.
 * - The data is converted to the output RGB format. See the `OutputFormat` enum.
 * - The data can be output either linearly line-by-line or in the swizzled 8x8 tile format used by
 *   the PICA. This is decided by the `BlockAlignment` enum. If 8x8 alignment is used, then the
 *   image must have a height divisible by 8. The image width must always be divisible by 8.
 * - The final data is then CDMAed out to main memory and the next image strip is processed. This
 *   offers the same flexibility as the input stage.
 *
 * In this implementation, to avoid the combinatorial explosion of parameter combinations, common
 * intermediate formats are used and where possible tables or parameters are used instead of
 * diverging code paths to keep the amount of branches in check. Some steps are also merged to
 * increase efficiency.
 *
 * Output for all valid settings combinations matches hardware, however output in some edge-cases
 * differs:
 *
 * - `Block8x8` alignment with non-mod8 height produces different garbage patterns on the last
 *   strip, especially when combined with rotation.
 * - Hardware, when using `Linear` alignment with a non-even height and 90 or 270 degree rotation
 *   produces misaligned output on the last strip. This implmentation produces output with the
 *   correct "expected" alignment.
 *
 * Hardware behaves strangely (doesn't fire the completion interrupt, for example) in these cases,
 * so they are believed to be invalid configurations anyway.
 */
static void PerformConversionScalar(const HostBuffers& memory, ConversionConfiguration& cvt) {
    ASSERT(cvt.input_line_width % 8 == 0);
    ASSERT(cvt.block_alignment != BlockAlignment::Block8x8 || cvt.input_lines % 8 == 0);
    // Tiles per row
    std::size_t num_tiles = cvt.input_line_width / 8;
    ASSERT(num_tiles <= MAX_TILES);

    // Buffer used as a CDMA source/target.
    std::unique_ptr<u8[]> data_buffer(new u8[cvt.input_line_width * 8 * 4]);
    // Intermediate storage for decoded 8x8 image tiles. Always stored as RGB32.
    std::unique_ptr<ImageTile[]> tiles(new ImageTile[num_tiles]);
    ImageTile tmp_tile;

    // LUT used to remap writes to a tile. Used to allow linear or swizzled output without
    // requiring two different code paths.
    const u8* tile_remap = nullptr;
    switch (cvt.block_alignment) {
    case BlockAlignment::Linear:
        tile_remap = linear_lut;
        break;
    case BlockAlignment::Block8x8:
        tile_remap = morton_lut;
        break;
    }

    for (unsigned int y = 0; y < cvt.input_lines; y += 8) {
        unsigned int row_height = std::min(cvt.input_lines - y, 8u);

        // Total size in pixels of incoming data required for this strip.
        const std::size_t row_data_size = row_height * cvt.input_line_width;

        u8* input_Y = data_buffer.get();
        u8* input_U = input_Y + 8 * cvt.input_line_width;
        u8* input_V = input_U + 8 * cvt.input_line_width / 2;

        switch (cvt.input_format) {
        case InputFormat::YUV422_Indiv8:
            ReceiveData<1>(memory, input_Y, cvt.src_Y, row_data_size);
            ReceiveData<1>(memory, input_U, cvt.src_U, row_data_size / 2);
            ReceiveData<1>(memory, input_V, cvt.src_V, row_data_size / 2);
            ConvertYUVToRGB<InputFormat::YUV422_Indiv8>(input_Y, input_U, input_V, tiles.get(),
                                                        cvt.input_line_width, row_height,
                                                        cvt.coefficients);
            break;
        case InputFormat::YUV420_Indiv8:
            ReceiveData<1>(memory, input_Y, cvt.src_Y, row_data_size);
            ReceiveData<1>(memory, input_U, cvt.src_U, row_data_size / 4);
            ReceiveData<1>(memory, input_V, cvt.src_V, row_data_size / 4);
            ConvertYUVToRGB<InputFormat::YUV420_Indiv8>(input_Y, input_U, input_V, tiles.get(),
                                                        cvt.input_line_width, row_height,
                                                        cvt.coefficients);
            break;
        case InputFormat::YUV422_Indiv16:
            ReceiveData<2>(memory, input_Y, cvt.src_Y, row_data_size);
            ReceiveData<2>(memory, input_U, cvt.src_U, row_data_size / 2);
            ReceiveData<2>(memory, input_V, cvt.src_V, row_data_size / 2);
            ConvertYUVToRGB<InputFormat::YUV422_Indiv16>(input_Y, input_U, input_V, tiles.get(),
                                                         cvt.input_line_width, row_height,
                                                         cvt.coefficients);
            break;
        case InputFormat::YUV420_Indiv16:
            ReceiveData<2>(memory, input_Y, cvt.src_Y, row_data_size);
            ReceiveData<2>(memory, input_U, cvt.src_U, row_data_size / 4);
            ReceiveData<2>(memory, input_V, cvt.src_V, row_data_size / 4);
            ConvertYUVToRGB<InputFormat::YUV420_Indiv16>(input_Y, input_U, input_V, tiles.get(),
                                                         cvt.input_line_width, row_height,
                                                         cvt.coefficients);
            break;
        case InputFormat::YUYV422_Interleaved:
            input_U = nullptr;
            input_V = nullptr;
            ReceiveData<1>(memory, input_Y, cvt.src_YUYV, row_data_size * 2);
            ConvertYUVToRGB<InputFormat::YUYV422_Interleaved>(input_Y, input_U, input_V,
                                                              tiles.get(), cvt.input_line_width,
                                                              row_height, cvt.coefficients);
            break;
        default:
            UNREACHABLE_MSG("Unknown Y2R input format {}", cvt.input_format);
            return;
        }

        u32* output_buffer = reinterpret_cast<u32*>(data_buffer.get());

        for (std::size_t i = 0; i < num_tiles; ++i) {
            int image_strip_width = 0;
            int output_stride = 0;

            switch (cvt.rotation) {
            case Rotation::None:
                RotateTile0(tiles[i], tmp_tile, row_height, tile_remap);
                image_strip_width = cvt.input_line_width;
                output_stride = 8;
                break;
            case Rotation::Clockwise_90:
                RotateTile90(tiles[i], tmp_tile, row_height, tile_remap);
                image_strip_width = 8;
                output_stride = 8 * row_height;
                break;
            case Rotation::Clockwise_180:
                // For 180 and 270 degree rotations we also invert the order of tiles in the strip,
                // since the rotates are done individually on each tile.
                RotateTile180(tiles[num_tiles - i - 1], tmp_tile, row_height, tile_remap);
                image_strip_width = cvt.input_line_width;
                output_stride = 8;
                break;
            case Rotation::Clockwise_270:
                RotateTile270(tiles[num_tiles - i - 1], tmp_tile, row_height, tile_remap);
                image_strip_width = 8;
                output_stride = 8 * row_height;
                break;
            }

            switch (cvt.block_alignment) {
            case BlockAlignment::Linear:
                WriteTileToOutput(output_buffer, tmp_tile, row_height, image_strip_width);
                output_buffer += output_stride;
                break;
            case BlockAlignment::Block8x8:
                WriteTileToOutput(output_buffer, tmp_tile, 8, 8);
                output_buffer += TILE_SIZE;
                break;
            }
        }

        switch (cvt.output_format) {
        case OutputFormat::RGBA8:
            SendData<OutputFormat::RGBA8>(memory, reinterpret_cast<u32*>(data_buffer.get()),
                                          cvt.dst, static_cast<int>(row_data_size),
                                          static_cast<u8>(cvt.alpha));
            break;
        case OutputFormat::RGB8:
            SendData<OutputFormat::RGB8>(memory, reinterpret_cast<u32*>(data_buffer.get()), cvt.dst,
                                         static_cast<int>(row_data_size),
                                         static_cast<u8>(cvt.alpha));
            break;
        case OutputFormat::RGB5A1:
            SendData<OutputFormat::RGB5A1>(memory, reinterpret_cast<u32*>(data_buffer.get()),
                                           cvt.dst, static_cast<int>(row_data_size),
                                           static_cast<u8>(cvt.alpha));
            break;
        case OutputFormat::RGB565:
            SendData<OutputFormat::RGB565>(memory, reinterpret_cast<u32*>(data_buffer.get()),
                                           cvt.dst, static_cast<int>(row_data_size),
                                           static_cast<u8>(cvt.alpha));
            break;
        default:
            UNREACHABLE_MSG("Unknown Y2R output format {}", cvt.output_format);
            return;
        }
    }
}

#if defined(__ARM_NEON)
namespace {

/// A strip's input from the CDMA, as PerformConversionScalar receives it: Y, then U and V (half
/// or quarter planes), or the YUYV words in Y's place.
void ReceiveStrip(const HostBuffers& memory, ConversionConfiguration& cvt, u8* input_Y,
                  u8* input_U, u8* input_V, std::size_t row_data_size) {
    switch (cvt.input_format) {
    case InputFormat::YUV422_Indiv8:
        ReceiveData<1>(memory, input_Y, cvt.src_Y, row_data_size);
        ReceiveData<1>(memory, input_U, cvt.src_U, row_data_size / 2);
        ReceiveData<1>(memory, input_V, cvt.src_V, row_data_size / 2);
        break;
    case InputFormat::YUV420_Indiv8:
        ReceiveData<1>(memory, input_Y, cvt.src_Y, row_data_size);
        ReceiveData<1>(memory, input_U, cvt.src_U, row_data_size / 4);
        ReceiveData<1>(memory, input_V, cvt.src_V, row_data_size / 4);
        break;
    case InputFormat::YUV422_Indiv16:
        ReceiveData<2>(memory, input_Y, cvt.src_Y, row_data_size);
        ReceiveData<2>(memory, input_U, cvt.src_U, row_data_size / 2);
        ReceiveData<2>(memory, input_V, cvt.src_V, row_data_size / 2);
        break;
    case InputFormat::YUV420_Indiv16:
        ReceiveData<2>(memory, input_Y, cvt.src_Y, row_data_size);
        ReceiveData<2>(memory, input_U, cvt.src_U, row_data_size / 4);
        ReceiveData<2>(memory, input_V, cvt.src_V, row_data_size / 4);
        break;
    case InputFormat::YUYV422_Interleaved:
        ReceiveData<1>(memory, input_Y, cvt.src_YUYV, row_data_size * 2);
        break;
    default:
        UNREACHABLE_MSG("Unknown Y2R input format {}", cvt.input_format);
    }
}

constexpr u32 OutputBytes(OutputFormat format) {
    switch (format) {
    case OutputFormat::RGBA8:
        return 4;
    case OutputFormat::RGB8:
        return 3;
    default:
        return 2;
    }
}

/// ConvertYUVToRGB's coefficients for eight pixels at once. With k = c[5..7] + 0x18,
/// ((v >> 3) + k) >> 5 is (v + 8k) >> 8 for arithmetic shifts, so k goes in first, and the
/// saturating narrows clamp to 0-255 as std::clamp does.
struct NeonCoefficients {
    s16 y, r_v, g_v, g_u, b_u;
    int32x4_t r_offset, g_offset, b_offset;
};

NeonCoefficients MakeNeonCoefficients(const CoefficientSet& c) {
    return {c[0],
            c[1],
            c[2],
            c[3],
            c[4],
            vdupq_n_s32((c[5] + 0x18) * 8),
            vdupq_n_s32((c[6] + 0x18) * 8),
            vdupq_n_s32((c[7] + 0x18) * 8)};
}

struct Rgb8 {
    uint8x8_t r, g, b;
};

uint8x8_t NarrowChannel(int32x4_t low, int32x4_t high) {
    return vqmovn_u16(vcombine_u16(vqshrun_n_s32(low, 8), vqshrun_n_s32(high, 8)));
}

Rgb8 ConvertEight(const NeonCoefficients& c, uint8x8_t y8, uint8x8_t u8, uint8x8_t v8) {
    const int16x8_t y = vreinterpretq_s16_u16(vmovl_u8(y8));
    const int16x8_t u = vreinterpretq_s16_u16(vmovl_u8(u8));
    const int16x8_t v = vreinterpretq_s16_u16(vmovl_u8(v8));
    const int32x4_t y_low = vmull_n_s16(vget_low_s16(y), c.y);
    const int32x4_t y_high = vmull_n_s16(vget_high_s16(y), c.y);
    const int32x4_t r_low = vmlal_n_s16(vaddq_s32(y_low, c.r_offset), vget_low_s16(v), c.r_v);
    const int32x4_t r_high = vmlal_n_s16(vaddq_s32(y_high, c.r_offset), vget_high_s16(v), c.r_v);
    const int32x4_t g_low = vmlsl_n_s16(
        vmlsl_n_s16(vaddq_s32(y_low, c.g_offset), vget_low_s16(v), c.g_v), vget_low_s16(u), c.g_u);
    const int32x4_t g_high =
        vmlsl_n_s16(vmlsl_n_s16(vaddq_s32(y_high, c.g_offset), vget_high_s16(v), c.g_v),
                    vget_high_s16(u), c.g_u);
    const int32x4_t b_low = vmlal_n_s16(vaddq_s32(y_low, c.b_offset), vget_low_s16(u), c.b_u);
    const int32x4_t b_high = vmlal_n_s16(vaddq_s32(y_high, c.b_offset), vget_high_s16(u), c.b_u);
    return {NarrowChannel(r_low, r_high), NarrowChannel(g_low, g_high),
            NarrowChannel(b_low, b_high)};
}

/// Pixels x..x+7 of line `line` of the strip: Y, and U and V repeated for each pixel pair.
template <InputFormat input_format>
void LoadEight(const u8* input_Y, const u8* input_U, const u8* input_V, u32 width, u32 line, u32 x,
               uint8x8_t& y, uint8x8_t& u, uint8x8_t& v) {
    if constexpr (input_format == InputFormat::YUYV422_Interleaved) {
        // Y0 U Y1 V for each pair: the even bytes are Y, the odd ones U and V by turns.
        static constexpr u8 u_lanes[8] = {0, 0, 2, 2, 4, 4, 6, 6};
        static constexpr u8 v_lanes[8] = {1, 1, 3, 3, 5, 5, 7, 7};
        const uint8x8x2_t words = vld2_u8(input_Y + (line * width + x) * 2);
        y = words.val[0];
        u = vtbl1_u8(words.val[1], vld1_u8(u_lanes));
        v = vtbl1_u8(words.val[1], vld1_u8(v_lanes));
    } else {
        constexpr bool half_lines = input_format == InputFormat::YUV420_Indiv8 ||
                                    input_format == InputFormat::YUV420_Indiv16;
        // Four U and four V bytes are used; the eight read stay inside the input buffer.
        const u32 chroma = ((half_lines ? line / 2 : line) * width + x) / 2;
        y = vld1_u8(input_Y + line * width + x);
        const uint8x8_t u4 = vld1_u8(input_U + chroma);
        const uint8x8_t v4 = vld1_u8(input_V + chroma);
        u = vzip_u8(u4, u4).val[0];
        v = vzip_u8(v4, v4).val[0];
    }
}

/// Eight converted pixels in the output format, as SendData's encoders write them.
template <OutputFormat output_format>
void StoreEight(u8* out, const Rgb8& p, uint8x8_t alpha) {
    if constexpr (output_format == OutputFormat::RGBA8) {
        const uint8x8x4_t bytes{{alpha, p.b, p.g, p.r}};
        vst4_u8(out, bytes);
    } else if constexpr (output_format == OutputFormat::RGB8) {
        const uint8x8x3_t bytes{{p.b, p.g, p.r}};
        vst3_u8(out, bytes);
    } else if constexpr (output_format == OutputFormat::RGB5A1) {
        uint16x8_t px = vshll_n_u8(p.r, 8);
        px = vsriq_n_u16(px, vshll_n_u8(p.g, 8), 5);
        px = vsriq_n_u16(px, vshll_n_u8(p.b, 8), 10);
        px = vsriq_n_u16(px, vshll_n_u8(alpha, 8), 15);
        vst1q_u8(out, vreinterpretq_u8_u16(px));
    } else {
        uint16x8_t px = vshll_n_u8(p.r, 8);
        px = vsriq_n_u16(px, vshll_n_u8(p.g, 8), 5);
        px = vsriq_n_u16(px, vshll_n_u8(p.b, 8), 11);
        vst1q_u8(out, vreinterpretq_u8_u16(px));
    }
}

/// The pixel pairs of two lines' eight pixels a and b, by turns: a0 a1 b0 b1 a2 a3 b2 b3 (`low`),
/// a4 a5 b4 b5 a6 a7 b6 b7 (`high`).
void ZipPairs(uint8x8_t a, uint8x8_t b, uint8x8_t& low, uint8x8_t& high) {
    const uint16x4x2_t z = vzip_u16(vreinterpret_u16_u8(a), vreinterpret_u16_u8(b));
    low = vreinterpret_u8_u16(z.val[0]);
    high = vreinterpret_u8_u16(z.val[1]);
}

/// A strip converted into the order SendData sends: its lines one after another, or with Block8x8
/// its 8x8 tiles, each in the PICA's Morton order. There an even line and the next hold pixels 0-7
/// of the tile's Morton positions 8k-8k+7 and 8k+16-8k+23 (k = 0, 1, 4, 5 for lines 0, 2, 4, 6) by
/// pixel pairs, so the two lines are converted together and their pairs zipped in registers, each
/// half stored at once: storing a line apart and copying its pairs into place read the NEON store
/// back with ARM loads, which on the Cortex-A9 wait for the NEON queue to drain.
template <InputFormat input_format, OutputFormat output_format>
void ConvertStrip(const u8* input_Y, const u8* input_U, const u8* input_V, u8* out, u32 width,
                  u32 lines, bool tiled, const NeonCoefficients& c, uint8x8_t alpha) {
    constexpr u32 bpp = OutputBytes(output_format);
    if (!tiled) {
        for (u32 line = 0; line < lines; line++) {
            for (u32 x = 0; x < width; x += 8) {
                uint8x8_t y, u, v;
                LoadEight<input_format>(input_Y, input_U, input_V, width, line, x, y, u, v);
                StoreEight<output_format>(out + (line * width + x) * bpp,
                                          ConvertEight(c, y, u, v), alpha);
            }
        }
        return;
    }
    DEBUG_ASSERT_MSG(lines % 2 == 0, "y2r: a tiled strip of {} lines", lines);
    for (u32 line = 0; line < lines; line += 2) {
        for (u32 x = 0; x < width; x += 8) {
            uint8x8_t y, u, v;
            LoadEight<input_format>(input_Y, input_U, input_V, width, line, x, y, u, v);
            const Rgb8 a = ConvertEight(c, y, u, v);
            LoadEight<input_format>(input_Y, input_U, input_V, width, line + 1, x, y, u, v);
            const Rgb8 b = ConvertEight(c, y, u, v);
            Rgb8 low, high;
            ZipPairs(a.r, b.r, low.r, high.r);
            ZipPairs(a.g, b.g, low.g, high.g);
            ZipPairs(a.b, b.b, low.b, high.b);
            u8* const tile = out + ((x / 8) * TILE_SIZE + morton_lut[line * 8]) * bpp;
            StoreEight<output_format>(tile, low, alpha);
            StoreEight<output_format>(tile + 16 * bpp, high, alpha);
        }
    }
}

template <InputFormat input_format>
void ConvertStripTo(OutputFormat output_format, const u8* input_Y, const u8* input_U,
                    const u8* input_V, u8* out, u32 width, u32 lines, bool tiled,
                    const NeonCoefficients& c, uint8x8_t alpha) {
    switch (output_format) {
    case OutputFormat::RGBA8:
        return ConvertStrip<input_format, OutputFormat::RGBA8>(input_Y, input_U, input_V, out,
                                                               width, lines, tiled, c, alpha);
    case OutputFormat::RGB8:
        return ConvertStrip<input_format, OutputFormat::RGB8>(input_Y, input_U, input_V, out, width,
                                                              lines, tiled, c, alpha);
    case OutputFormat::RGB5A1:
        return ConvertStrip<input_format, OutputFormat::RGB5A1>(input_Y, input_U, input_V, out,
                                                                width, lines, tiled, c, alpha);
    case OutputFormat::RGB565:
        return ConvertStrip<input_format, OutputFormat::RGB565>(input_Y, input_U, input_V, out,
                                                                width, lines, tiled, c, alpha);
    default:
        UNREACHABLE_MSG("Unknown Y2R output format {}", output_format);
    }
}

/// Whether PerformConversionNeon gives PerformConversionScalar's bytes: no rotation, and each
/// strip's output whole transfer units of whole pixels (SendData otherwise sends past the strip).
bool NeonFits(const ConversionConfiguration& cvt) {
    const u32 bpp = OutputBytes(cvt.output_format);
    const u32 unit = cvt.dst.transfer_unit;
    const u32 full = cvt.input_line_width * 8 * bpp;
    const u32 last = cvt.input_line_width * (cvt.input_lines % 8) * bpp;
    return cvt.rotation == Rotation::None && cvt.input_line_width % 8 == 0 && unit != 0 &&
           unit % bpp == 0 && full % unit == 0 && last % unit == 0;
}

/// A buffer's DMA position once `bytes` of its data are taken: whole transfer units, each followed
/// by the gap.
ConversionBuffer Advanced(ConversionBuffer buf, u32 bytes) {
    buf.address += bytes / buf.transfer_unit * (buf.transfer_unit + buf.gap) +
                   bytes % buf.transfer_unit;
    buf.image_size -= bytes;
    return buf;
}

/// Strip s of a conversion NeonFits takes (lines 8s on, eight or what is left), as
/// PerformConversionScalar converts it: its input received from where the DMA reads it (in place
/// when its buffers have no gap), eight pixels at a time straight into the output format, sent a
/// transfer unit at a time (or written in place, saving the copies). cvt is the conversion as it
/// starts: each strip's buffers are found from it, so the strips are independent and any thread
/// may take any of them. `scratch` holds width * 64 bytes.
void ConvertNeonStrip(const HostBuffers& memory, const ConversionConfiguration& cvt, u32 s,
                      u8* scratch) {
    const u32 width = cvt.input_line_width;
    const u32 bpp = OutputBytes(cvt.output_format);
    const bool tiled = cvt.block_alignment == BlockAlignment::Block8x8;
    const NeonCoefficients c = MakeNeonCoefficients(cvt.coefficients);
    const uint8x8_t alpha = vdup_n_u8(static_cast<u8>(cvt.alpha));
    const bool planar8 = cvt.input_format == InputFormat::YUV422_Indiv8 ||
                         cvt.input_format == InputFormat::YUV420_Indiv8;
    const bool sixteen = cvt.input_format == InputFormat::YUV422_Indiv16 ||
                         cvt.input_format == InputFormat::YUV420_Indiv16;
    const u32 chroma_div = cvt.input_format == InputFormat::YUV420_Indiv8 ||
                                   cvt.input_format == InputFormat::YUV420_Indiv16
                               ? 4
                               : 2;
    const u32 sample = sixteen ? 2 : 1;
    const u32 lines = std::min(cvt.input_lines - s * 8, 8u);
    const u32 strip = lines * width;
    const u32 before = s * 8 * width;
    // The strip's buffers, as the DMA has them after the strips before.
    ConversionConfiguration at = cvt;
    at.src_Y = Advanced(cvt.src_Y, before * sample);
    at.src_U = Advanced(cvt.src_U, before / chroma_div * sample);
    at.src_V = Advanced(cvt.src_V, before / chroma_div * sample);
    at.src_YUYV = Advanced(cvt.src_YUYV, before * 2);
    at.dst = Advanced(cvt.dst, before * bpp);
    u8* input_Y = scratch;
    u8* input_U = input_Y + 8 * width;
    u8* input_V = input_U + 8 * width / 2;
    u8* const output = scratch + 32 * width;
    if (planar8 && at.src_Y.gap == 0 && at.src_U.gap == 0 && at.src_V.gap == 0) {
        input_Y = memory.GetPointer(at.src_Y.address);
        input_U = memory.GetPointer(at.src_U.address);
        input_V = memory.GetPointer(at.src_V.address);
    } else if (cvt.input_format == InputFormat::YUYV422_Interleaved && at.src_YUYV.gap == 0) {
        input_Y = memory.GetPointer(at.src_YUYV.address);
    } else {
        ReceiveStrip(memory, at, input_Y, input_U, input_V, strip);
    }
    const bool direct_out = at.dst.gap == 0;
    u8* const out = direct_out ? memory.GetPointer(at.dst.address) : output;
    switch (cvt.input_format) {
    case InputFormat::YUV422_Indiv8:
        ConvertStripTo<InputFormat::YUV422_Indiv8>(cvt.output_format, input_Y, input_U, input_V,
                                                   out, width, lines, tiled, c, alpha);
        break;
    case InputFormat::YUV420_Indiv8:
        ConvertStripTo<InputFormat::YUV420_Indiv8>(cvt.output_format, input_Y, input_U, input_V,
                                                   out, width, lines, tiled, c, alpha);
        break;
    case InputFormat::YUV422_Indiv16:
        ConvertStripTo<InputFormat::YUV422_Indiv16>(cvt.output_format, input_Y, input_U, input_V,
                                                    out, width, lines, tiled, c, alpha);
        break;
    case InputFormat::YUV420_Indiv16:
        ConvertStripTo<InputFormat::YUV420_Indiv16>(cvt.output_format, input_Y, input_U, input_V,
                                                    out, width, lines, tiled, c, alpha);
        break;
    case InputFormat::YUYV422_Interleaved:
        ConvertStripTo<InputFormat::YUYV422_Interleaved>(
            cvt.output_format, input_Y, input_U, input_V, out, width, lines, tiled, c, alpha);
        break;
    default:
        UNREACHABLE_MSG("Unknown Y2R input format {}", cvt.input_format);
    }
    if (direct_out) {
        return;
    }
    // SendData's units: transfer_unit bytes, then the gap.
    u8* dst = memory.GetPointer(at.dst.address);
    const u32 unit = at.dst.transfer_unit;
    for (u32 sent = 0; sent < strip * bpp; sent += unit) {
        std::memcpy(dst, out + sent, unit);
        dst += unit + at.dst.gap;
    }
}

u32 NeonStrips(const ConversionConfiguration& cvt) {
    return (cvt.input_lines + 7) / 8;
}

/// Read and written in place, the output must not run over the input other strips have yet to
/// read (the copies hid that; no title seen does it).
[[maybe_unused]] bool OutputApart(const ConversionConfiguration& cvt) {
    const u32 pixels = u32{cvt.input_line_width} * cvt.input_lines;
    const u32 out_bytes = pixels * OutputBytes(cvt.output_format);
    const auto apart = [&](const ConversionBuffer& buf, u32 bytes) {
        return buf.gap != 0 || cvt.dst.gap != 0 || buf.address + bytes <= cvt.dst.address ||
               cvt.dst.address + out_bytes <= buf.address;
    };
    const u32 div = cvt.input_format == InputFormat::YUV420_Indiv8 ? 4 : 2;
    switch (cvt.input_format) {
    case InputFormat::YUV422_Indiv8:
    case InputFormat::YUV420_Indiv8:
        return apart(cvt.src_Y, pixels) && apart(cvt.src_U, pixels / div) &&
               apart(cvt.src_V, pixels / div);
    case InputFormat::YUYV422_Interleaved:
        return apart(cvt.src_YUYV, pixels * 2);
    default:
        return true;
    }
}

/// PerformConversionScalar for the conversions NeonFits takes, strip after strip.
void PerformConversionNeon(const HostBuffers& memory, const ConversionConfiguration& cvt) {
    ASSERT(cvt.input_line_width / 8 <= MAX_TILES);
    ASSERT(cvt.block_alignment != BlockAlignment::Block8x8 || cvt.input_lines % 8 == 0);
    DEBUG_ASSERT_MSG(OutputApart(cvt), "y2r: the output at {:#010x} overlaps the input",
                     cvt.dst.address);
    std::unique_ptr<u8[]> scratch(new u8[cvt.input_line_width * 64]);
    for (u32 s = 0; s < NeonStrips(cvt); s++) {
        ConvertNeonStrip(memory, cvt, s, scratch.get());
    }
}

#ifdef VITA_DIAGNOSTICS
/// The configurations compared with no byte different (RGB565 and RGB8 from games'
/// videos): diagnostics builds check every other one at each conversion.
bool NeonVerified(const ConversionConfiguration& cvt) {
    return cvt.input_format == InputFormat::YUV420_Indiv8 &&
           (cvt.output_format == OutputFormat::RGB565 || cvt.output_format == OutputFormat::RGB8) &&
           cvt.block_alignment == BlockAlignment::Block8x8;
}

/// A configuration NeonVerified does not list: the conversion run both ways, the guest left with
/// the per-pixel path's bytes; a byte the NEON path wrote differently stops the run.
void CheckNeon(const HostBuffers& memory, const ConversionConfiguration& cvt) {
    static u32 checked, differed;
    const u32 bpp = OutputBytes(cvt.output_format);
    const u32 units = cvt.input_line_width * cvt.input_lines * bpp / cvt.dst.transfer_unit;
    const u32 bytes = units * (cvt.dst.transfer_unit + cvt.dst.gap);
    ConversionConfiguration neon = cvt, scalar = cvt;
    PerformConversionNeon(memory, neon);
    const u8* const dst = memory.GetPointer(cvt.dst.address);
    const std::vector<u8> neon_bytes(dst, dst + bytes);
    PerformConversionScalar(memory, scalar);
    u32 differ = 0, first = 0;
    for (u32 i = 0; i < bytes; i++) {
        if (neon_bytes[i] != dst[i] && differ++ == 0) {
            first = i;
        }
    }
    checked++;
    differed += differ != 0 ? 1 : 0;
    if (differ != 0 || checked <= 3 || checked % 256 == 0) {
        LOG_INFO(Service_Y2R,
                 "y2r check: {}x{} in {} out {} {} rotation {}: {} of {} bytes differ (first at "
                 "{}); {} of {} conversions differed",
                 cvt.input_line_width, cvt.input_lines, cvt.input_format, cvt.output_format,
                 cvt.block_alignment == BlockAlignment::Block8x8 ? "8x8" : "linear", cvt.rotation,
                 differ, bytes, first, differed, checked);
    }
    DEBUG_ASSERT_MSG(
        differ == 0, "y2r: the NEON conversion of {}x{} in {} out {} differs at byte {} of {}",
        cvt.input_line_width, cvt.input_lines, cvt.input_format, cvt.output_format, first, bytes);
}
#endif

} // namespace
#endif

u8* HostBuffers::GetPointer(VAddr vaddr) const {
    for (u32 i = 0; i < count; i++) {
        const Range& r = ranges[i];
        if (vaddr - r.start < r.bytes) {
            return r.host + (vaddr - r.start);
        }
    }
    UNREACHABLE_MSG("Y2R: {:#010x} is in none of the conversion's buffers", vaddr);
}

HostBuffers ResolveBuffers(Memory::MemorySystem& memory, const ConversionConfiguration& cvt) {
    HostBuffers buffers;
    // A buffer's stretch: the transfer units its DMA takes for `bytes`, with the gaps between.
    const auto add = [&](const ConversionBuffer& buf, u32 bytes, const char* name) {
        ASSERT_MSG(buf.transfer_unit != 0, "Y2R: {}'s transfer unit is 0", name);
        const u32 units = (bytes + buf.transfer_unit - 1) / buf.transfer_unit;
        const u32 span = (units - 1) * (buf.transfer_unit + buf.gap) + buf.transfer_unit;
        u8* const host = memory.GetPointer(buf.address);
        ASSERT_MSG(host != nullptr && memory.GetPointer(buf.address + span - 1) == host + span - 1,
                   "Y2R: {} at {:#010x}+{:#x} is not one stretch of host memory", name,
                   buf.address, span);
        buffers.ranges[buffers.count++] = {buf.address, span, host};
    };
    const u32 pixels = u32{cvt.input_line_width} * cvt.input_lines;
    const u32 sample = cvt.input_format == InputFormat::YUV422_Indiv16 ||
                               cvt.input_format == InputFormat::YUV420_Indiv16
                           ? 2
                           : 1;
    switch (cvt.input_format) {
    case InputFormat::YUV422_Indiv8:
    case InputFormat::YUV422_Indiv16:
        add(cvt.src_Y, pixels * sample, "Y");
        add(cvt.src_U, pixels / 2 * sample, "U");
        add(cvt.src_V, pixels / 2 * sample, "V");
        break;
    case InputFormat::YUV420_Indiv8:
    case InputFormat::YUV420_Indiv16:
        add(cvt.src_Y, pixels * sample, "Y");
        add(cvt.src_U, pixels / 4 * sample, "U");
        add(cvt.src_V, pixels / 4 * sample, "V");
        break;
    case InputFormat::YUYV422_Interleaved:
        add(cvt.src_YUYV, pixels * 2, "YUYV");
        break;
    default:
        UNREACHABLE_MSG("Unknown Y2R input format {}", cvt.input_format);
    }
    const u32 bpp = cvt.output_format == OutputFormat::RGBA8  ? 4
                    : cvt.output_format == OutputFormat::RGB8 ? 3
                                                              : 2;
    add(cvt.dst, pixels * bpp, "the output");
    return buffers;
}

void PerformConversion(const HostBuffers& memory, ConversionConfiguration cvt) {
    MICROPROFILE_SCOPE(Y2R_PerformConversion);
#if defined(__ARM_NEON)
    if (NeonFits(cvt)) {
#ifdef VITA_DIAGNOSTICS
        if (!NeonVerified(cvt)) {
            CheckNeon(memory, cvt);
            return;
        }
#endif
        PerformConversionNeon(memory, cvt);
        return;
    }
#endif
    PerformConversionScalar(memory, cvt);
}

struct Conversion::State {
    HostBuffers memory;
    ConversionConfiguration cvt;
    bool shared = false;
    u32 strips = 1;
    std::atomic<u32> next{0};
    std::atomic<u32> done{0};
};

Conversion::Conversion(const HostBuffers& memory, const ConversionConfiguration& cvt)
    : state(std::make_unique<State>()) {
    state->memory = memory;
    state->cvt = cvt;
#if defined(__ARM_NEON)
    state->shared = NeonFits(cvt);
#ifdef VITA_DIAGNOSTICS
    state->shared = state->shared && NeonVerified(cvt);
#endif
    if (state->shared) {
        ASSERT(cvt.input_line_width / 8 <= MAX_TILES);
        ASSERT(cvt.block_alignment != BlockAlignment::Block8x8 || cvt.input_lines % 8 == 0);
        DEBUG_ASSERT_MSG(OutputApart(cvt), "y2r: the output at {:#010x} overlaps the input",
                         cvt.dst.address);
        state->strips = NeonStrips(cvt);
    }
#endif
}

Conversion::~Conversion() = default;

void Conversion::Run() {
    if (!state->shared) {
        PerformConversion(state->memory, state->cvt);
        state->done.store(1, std::memory_order_release);
        return;
    }
    TakeStrips();
}

bool Conversion::Help() {
    if (!state->shared) {
        return false;
    }
    TakeStrips();
    return true;
}

bool Conversion::Finished() const {
    return state->done.load(std::memory_order_acquire) == state->strips;
}

void Conversion::TakeStrips() {
#if defined(__ARM_NEON)
    std::unique_ptr<u8[]> scratch(new u8[state->cvt.input_line_width * 64]);
    for (;;) {
        const u32 s = state->next.fetch_add(1, std::memory_order_relaxed);
        if (s >= state->strips) {
            return;
        }
        ConvertNeonStrip(state->memory, state->cvt, s, scratch.get());
        state->done.fetch_add(1, std::memory_order_release);
    }
#endif
}
} // namespace HW::Y2R
