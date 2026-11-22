// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// untile_geometry_test — host test of untile_geometry.h: the chain untile's and the ETC1 batch's
// quads rasterised in software (pixel centres, point sampling, the SWIZZLED views and the
// PBE's TWIDDLED layout as docs/sgxd/tsp.md and docs/sgxd/pbe.md give them),
// against each layout computed directly from the PICA's: every texel or block of the chain
// written once, from the right source, and nothing else in the target. Build and run from
// src/video_core/renderer_sgxd:
//
//     c++ -std=c++20 -Wall -Wextra -Werror -I ../.. -I ../../sgxd untile_geometry_test.cpp
//         -o untile_geometry_test && ./untile_geometry_test

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "untile_geometry.h"

using namespace SgxdRenderer::UntileGeometry;

namespace {

int failures;
std::mt19937 rng(12345);

// Morton with y in bit 0 over a size x size square (SGX SWIZZLED, PBE TWIDDLED).
u32 MortonYFirst(u32 x, u32 y) {
    u32 i = 0;
    for (u32 b = 0; b < 16; b++) {
        i |= (y >> b & 1u) << (2 * b) | (x >> b & 1u) << (2 * b + 1);
    }
    return i;
}

// The SWIZZLED index of texel (x, y) of a w x h texture: Morton over the smaller side, the
// larger side's remaining bits above (docs/sgxd/tsp.md).
u32 Swizzled(u32 x, u32 y, u32 w, u32 h) {
    const u32 s = std::min(w, h);
    return MortonYFirst(x % s, y % s) + (w >= h ? x / s : y / s) * s * s;
}

// The PICA's 8x8 tile index: Morton with x in bit 0.
u32 MortonXFirst(u32 x, u32 y) {
    return MortonYFirst(y, x);
}

// One quad of the vertices at q: its pixel rectangle and the coordinates at its corners.
struct Quad {
    float x0, y0, x1, y1;
    float uv[4][2];
};

Quad QuadAt(const float* v, u32 width, u32 height) {
    constexpr u32 N = SGXD_PROGRAM_BLIT_VERTEX_WORDS;
    // PutQuad's triangles take corners 0, 1, 2 and 1, 3, 2: vertices 0, 1, 2 and 4 are
    // corners 00, 10, 01, 11.
    static constexpr u32 vertex_of[4] = {0, 1, 2, 4};
    Quad q{};
    for (u32 c = 0; c < 4; c++) {
        const float* w = v + vertex_of[c] * N;
        const float x = (w[0] + 1) / 2 * width, y = (w[1] + 1) / 2 * height;
        if (c == 0) {
            q.x0 = x, q.y0 = y;
        }
        if (c == 3) {
            q.x1 = x, q.y1 = y;
        }
        q.uv[c][0] = w[8];
        q.uv[c][1] = w[9];
    }
    return q;
}

// A pixel the side render's region clip drops (SgxdBackend::SideRender, LastTile): the TA
// leaves the tiles past the clip out, so nothing a quad draws there is stored.
bool Clipped(u32 px, u32 py, u32 width, u32 height) {
    const auto [last_x, last_y] = LastTile(width, height);
    return px / 32 > last_x || py / 32 > last_y;
}

// Each pixel centre inside the quad, with the coordinate interpolated there (the quads'
// coordinates are affine, so the triangles' interpolation is the bilinear one).
template <typename F>
void Rasterise(const Quad& q, F&& pixel) {
    for (u32 c = 0; c < 2; c++) {
        const float d = q.uv[3][c] - q.uv[1][c] - q.uv[2][c] + q.uv[0][c];
        if (std::fabs(d) > 1e-6f) {
            printf("a quad's coordinates are not affine\n");
            failures++;
        }
    }
    for (u32 py = static_cast<u32>(std::ceil(q.y0 - 0.5f)); py + 0.5f < q.y1; py++) {
        for (u32 px = static_cast<u32>(std::ceil(q.x0 - 0.5f)); px + 0.5f < q.x1; px++) {
            const float fx = (px + 0.5f - q.x0) / (q.x1 - q.x0);
            const float fy = (py + 0.5f - q.y0) / (q.y1 - q.y0);
            float uv[2];
            for (u32 c = 0; c < 2; c++) {
                const float top = q.uv[0][c] + (q.uv[1][c] - q.uv[0][c]) * fx;
                const float bottom = q.uv[2][c] + (q.uv[3][c] - q.uv[2][c]) * fx;
                uv[c] = top + (bottom - top) * fy;
            }
            pixel(px, py, uv[0], uv[1]);
        }
    }
}

// A texel of `bits` at index i of data (4-bit texels low nibble first).
u32 Fetch(const std::vector<u8>& data, u64 byte, u32 bits, u32 i, bool& out_of_range) {
    const u64 at = byte + u64{i} * bits / 8;
    if (at + std::max(bits / 8, 1u) > data.size()) {
        out_of_range = true;
        return 0;
    }
    u32 value = 0;
    for (u32 n = 0; n < std::max(bits / 8, 1u); n++) {
        value |= u32{data[at + n]} << (8 * n);
    }
    return bits == 4 ? value >> (4 * (i & 1)) & 15 : value;
}

// The chain's renders: UntileChain's (ChainRenders), or with `batch` its place in a batch,
// one render at ChainBatchStride (ChainAt), when it fits.
void CheckChain(u32 width, u32 height, u32 levels, u32 bits, bool batch = false) {
    const std::vector<ChainRender> renders =
        batch ? std::vector{ChainAt(width, height, levels, ChainBatchStride(width))}
              : ChainRenders(width, height, levels);
    if (batch && renders[0].rows > UntileSide) {
        return;
    }
    u32 guest_bytes = 0, texels = 0;
    for (u32 l = 0; l < levels; l++) {
        guest_bytes += (width >> l) * (height >> l) * bits / 8;
        texels += (width >> l) * (height >> l);
    }
    std::vector<u8> guest(guest_bytes);
    for (u8& b : guest) {
        b = static_cast<u8>(rng());
    }
    // Room for any render's rows past the chain's end.
    const u32 host_texels = texels + UntileSide * UntileSide;
    std::vector<u32> host(host_texels, 0);
    std::vector<u8> writes(host_texels, 0);
    u32 max_written = 0;
    bool out_of_range = false;
    for (const ChainRender& cr : renders) {
        if (cr.rows > UntileSide || cr.stride > UntileSide || cr.stride % 32) {
            printf("chain %ux%u/%u: a render of %u x %u\n", width, height, levels, cr.stride,
                   cr.rows);
            failures++;
        }
        std::vector<GeometryDraw> draws;
        const u32 count = ChainVertices(cr, width, height, bits, nullptr, draws);
        std::vector<float> v(count * SGXD_PROGRAM_BLIT_VERTEX_WORDS, NAN);
        std::vector<GeometryDraw> filled;
        ChainVertices(cr, width, height, bits, v.data(), filled);
        for (const GeometryDraw& d : draws) {
            if (d.first % 8 || d.count % 6 || d.view_rows > 4096) {
                printf("chain %ux%u/%u: a draw at %u of %u, view %u rows\n", width, height,
                       levels, d.first, d.count, d.view_rows);
                failures++;
            }
            for (u32 k = d.first; k < d.first + d.count; k += 6) {
                const Quad q = QuadAt(&v[k * SGXD_PROGRAM_BLIT_VERTEX_WORDS], cr.stride, cr.rows);
                Rasterise(q, [&](u32 px, u32 py, float u, float vv) {
                    if (px >= cr.stride || py >= cr.rows || Clipped(px, py, cr.stride, cr.rows)) {
                        printf("chain %ux%u/%u: pixel (%u, %u) outside the %ux%u target or "
                               "its clip\n",
                               width, height, levels, px, py, cr.stride, cr.rows);
                        failures++;
                        return;
                    }
                    const u32 sx = static_cast<u32>(std::floor(u * 8));
                    const u32 sy = static_cast<u32>(std::floor(vv * d.view_rows));
                    const u32 texel =
                        Fetch(guest, d.source, bits, Swizzled(sx, sy, 8, d.view_rows), out_of_range);
                    const u32 at = cr.offset + py * cr.stride + px;
                    host[at] = texel;
                    writes[at]++;
                    max_written = std::max(max_written, at + 1);
                });
            }
        }
    }
    if (out_of_range) {
        printf("chain %ux%u/%u: a sample past the guest's bytes\n", width, height, levels);
        failures++;
    }
    u32 wrong = 0, twice = 0;
    for (u32 l = 0, guest_at = 0, host_at = 0; l < levels; l++) {
        const u32 w = width >> l, h = height >> l;
        for (u32 y = 0; y < h; y++) {
            for (u32 x = 0; x < w; x++) {
                const u32 index = ((y / 8) * (w / 8) + x / 8) * 64 + MortonXFirst(x % 8, y % 8);
                bool unused = false;
                const u32 want = Fetch(guest, guest_at, bits, index, unused);
                const u32 at = host_at + y * w + x;
                wrong += host[at] != want || writes[at] == 0;
                twice += writes[at] > 1;
            }
        }
        guest_at += w * h * bits / 8;
        host_at += w * h;
    }
    if (wrong || twice || max_written > texels) {
        printf("chain %ux%u/%u bits %u: %u texels wrong, %u written twice, %u written past the "
               "chain's %u\n",
               width, height, levels, bits, wrong, twice, max_written > texels ? max_written : 0,
               texels);
        failures++;
    }
    printf("chain %ux%u, %u levels, %u bits: %zu renders of %u texels a row%s, ok\n", width,
           height, levels, bits, renders.size(), renders[0].stride, batch ? " (batch)" : "");
}

// An ETC1 chain's render over `guest` (an ETC1 chain), into out (a block a pixel, by PBE index)
// with writes counting each pixel's stores. Returns false on a geometry fault (reported).
bool RunEtc1(u32 width, u32 height, u32 levels, const std::vector<u8>& guest,
             std::vector<u64>& out, std::vector<u8>& writes, u32& count) {
    const Etc1Target target = Etc1TargetFor(width, height, levels);
    if (target.width > UntileSide || target.height > UntileSide || target.width < target.height ||
        !std::has_single_bit(target.width) || !std::has_single_bit(target.height)) {
        printf("etc1 %ux%u/%u: target %ux%u\n", width, height, levels, target.width,
               target.height);
        failures++;
        return false;
    }
    const u32 pixels = target.width * target.height;
    out.assign(pixels, 0);
    writes.assign(pixels, 0);
    std::vector<GeometryDraw> draws, filled;
    count = Etc1Vertices(width, height, levels, nullptr, draws);
    std::vector<float> v(count * SGXD_PROGRAM_BLIT_VERTEX_WORDS, NAN);
    Etc1Vertices(width, height, levels, v.data(), filled);
    bool out_of_range = false;
    for (const GeometryDraw& d : draws) {
        if (d.first % 8 || d.count % 6 || d.view_rows > 4096) {
            printf("etc1 %ux%u/%u: a draw at %u of %u, view %u rows\n", width, height, levels,
                   d.first, d.count, d.view_rows);
            failures++;
        }
        for (u32 k = d.first; k < d.first + d.count; k += 6) {
            const Quad q = QuadAt(&v[k * SGXD_PROGRAM_BLIT_VERTEX_WORDS], target.width,
                                  target.height);
            Rasterise(q, [&](u32 px, u32 py, float u, float vv) {
                if (px >= target.width || py >= target.height ||
                    Clipped(px, py, target.width, target.height)) {
                    printf("etc1 %ux%u/%u: pixel (%u, %u) outside the %ux%u target or its "
                           "clip\n",
                           width, height, levels, px, py, target.width, target.height);
                    failures++;
                    return;
                }
                const u32 sx = static_cast<u32>(std::floor(u * 2));
                const u32 sy = static_cast<u32>(std::floor(vv * d.view_rows));
                const u32 i = Swizzled(sx, sy, 2, d.view_rows);
                const u64 at = d.source + u64{i} * 8;
                if (at + 8 > guest.size()) {
                    out_of_range = true;
                    return;
                }
                u64 texel;
                std::memcpy(&texel, &guest[at], 8);
                // PBE TWIDDLED, the smaller side the height; F32F32 SWIZ 1 stores word 1 first.
                const u32 s = target.height;
                const u32 index = MortonYFirst(px % s, py) + px / s * s * s;
                out[index] = texel >> 32 | texel << 32;
                writes[index]++;
            });
        }
    }
    if (out_of_range) {
        printf("etc1 %ux%u/%u: a sample past the guest's bytes\n", width, height, levels);
        failures++;
    }
    return true;
}

void CheckEtc1(u32 width, u32 height, u32 levels) {
    u32 blocks = 0;
    for (u32 l = 0; l < levels; l++) {
        blocks += (width >> l) / 4 * ((height >> l) / 4);
    }
    std::vector<u8> guest(blocks * 8);
    for (u8& b : guest) {
        b = static_cast<u8>(rng());
    }
    std::vector<u64> out;
    std::vector<u8> writes;
    u32 count = 0;
    if (!RunEtc1(width, height, levels, guest, out, writes, count)) {
        return;
    }
    const Etc1Target target = Etc1TargetFor(width, height, levels);
    const u32 pixels = target.width * target.height;
    std::vector<GeometryDraw> draws;
    Etc1Vertices(width, height, levels, nullptr, draws);
    u32 wrong = 0, twice = 0, written = 0;
    for (u32 n = 0; n < pixels; n++) {
        written += writes[n] != 0;
        twice += writes[n] > 1;
    }
    for (u32 l = 0, first = 0; l < levels; l++) {
        const u32 bw = (width >> l) / 4, bh = (height >> l) / 4;
        for (u32 by = 0; by < bh; by++) {
            for (u32 bx = 0; bx < bw; bx++) {
                const u32 tile = (by / 2) * (bw / 2) + bx / 2;
                const u32 block = tile * 4 + (bx & 1) + 2 * (by & 1);
                u64 want;
                std::memcpy(&want, &guest[(first + block) * 8], 8);
                want = want >> 32 | want << 32;
                const u32 index = first + Swizzled(bx, by, bw, bh);
                wrong += out[index] != want || writes[index] == 0;
            }
        }
        first += bw * bh;
    }
    // A batch moves the chain by whole squares: nothing past its slot.
    const u32 slot = Etc1Squares(width, height, levels) * target.height * target.height;
    u32 past_slot = 0;
    for (u32 n = slot; n < pixels; n++) {
        past_slot += writes[n] != 0;
    }
    if (wrong || twice || written != blocks || past_slot) {
        printf("etc1 %ux%u/%u: %u blocks wrong, %u written twice, %u written of %u, %u past its "
               "%u-block slot\n",
               width, height, levels, wrong, twice, written, blocks, past_slot, slot);
        failures++;
    }
    printf("etc1 %ux%u, %u levels: target %ux%u blocks (%u bytes for %u), %zu draws, %u quads, "
           "ok\n",
           width, height, levels, target.width, target.height, pixels * 8, blocks * 8,
           draws.size(), count / 6);
}

// Each quad of `draws` (vertices v of a width x height target) rasterised: pixel(px, py, u, v,
// draw). False with a report when a pixel falls outside the target.
template <typename F>
bool RasteriseDraws(const char* what, const std::vector<float>& v,
                    const std::vector<GeometryDraw>& draws, u32 width, u32 height, F&& pixel) {
    bool ok = true;
    for (const GeometryDraw& d : draws) {
        if (d.first % 8 || d.count % 6) {
            printf("%s: a draw at %u of %u\n", what, d.first, d.count);
            failures++;
        }
        for (u32 k = d.first; k < d.first + d.count; k += 6) {
            const Quad q = QuadAt(&v[k * SGXD_PROGRAM_BLIT_VERTEX_WORDS], width, height);
            Rasterise(q, [&](u32 px, u32 py, float u, float vv) {
                if (px >= width || py >= height || Clipped(px, py, width, height)) {
                    if (ok) {
                        printf("%s: pixel (%u, %u) outside the %ux%u target or its clip\n",
                               what, px, py, width, height);
                    }
                    ok = false;
                    return;
                }
                pixel(px, py, u, vv, d);
            });
        }
    }
    if (!ok) {
        failures++;
    }
    return ok;
}

// A split ETC1A4's three renders on a random ETC1A4 chain: the gather (raw words through LINEAR
// U8888 views), the ETC1 render over the gathered colour, and the alpha render (a LINEAR P4 view
// through the 17n palette, its A stored as a byte), against the guest's colour blocks and
// alpha nibbles.
void CheckEtc1A4(u32 width, u32 height, u32 levels) {
    char what[64];
    snprintf(what, sizeof(what), "etc1a4 %ux%u/%u", width, height, levels);
    u32 tiles = 0;
    for (u32 l = 0; l < levels; l++) {
        tiles += (width >> l) / 8 * ((height >> l) / 8);
    }
    std::vector<u8> guest(tiles * 64);
    for (u8& b : guest) {
        b = static_cast<u8>(rng());
    }
    // The gather.
    const Etc1A4Gather g = Etc1A4GatherFor(width, height, levels);
    if (g.rows > UntileSide || g.stride > UntileSide || 8 * g.stride > 4096) {
        printf("%s: gather target %u x %u\n", what, g.stride, g.rows);
        failures++;
        return;
    }
    std::vector<u32> scratch(g.stride * g.rows + 16, 0xdeadbeefu);
    std::vector<GeometryDraw> draws, filled;
    u32 count = Etc1A4GatherVertices(width, height, levels, nullptr, draws);
    std::vector<float> v(count * SGXD_PROGRAM_BLIT_VERTEX_WORDS, NAN);
    Etc1A4GatherVertices(width, height, levels, v.data(), filled);
    const auto word = [&](u64 byte) -> u32 {
        u32 x = 0;
        if (byte + 4 <= guest.size()) {
            std::memcpy(&x, &guest[byte], 4);
        }
        return x;
    };
    RasteriseDraws(what, v, draws, g.stride, g.rows, [&](u32 px, u32 py, float u, float vv,
                                                         const GeometryDraw& d) {
        const u32 col = static_cast<u32>(std::floor(u * d.view_width));
        const u32 row = static_cast<u32>(std::floor(vv * d.view_rows));
        scratch[py * g.stride + px] = word(d.source + (u64{row} * d.view_width + col) * 4);
    });
    // The ETC1 render over the colour words.
    std::vector<u8> colour(tiles * 32);
    std::memcpy(colour.data(), scratch.data(), colour.size());
    std::vector<u64> out;
    std::vector<u8> writes;
    if (!RunEtc1(width, height, levels, colour, out, writes, count)) {
        return;
    }
    u32 wrong_colour = 0;
    for (u32 l = 0, first = 0, at = 0; l < levels; l++) {
        const u32 bw = (width >> l) / 4, bh = (height >> l) / 4;
        for (u32 by = 0; by < bh; by++) {
            for (u32 bx = 0; bx < bw; bx++) {
                const u32 tile = (by / 2) * (bw / 2) + bx / 2;
                const u32 block = tile * 4 + (bx & 1) + 2 * (by & 1);
                u64 want;
                std::memcpy(&want, &guest[at + block * 16 + 8], 8);
                want = want >> 32 | want << 32;
                const u32 index = first + Swizzled(bx, by, bw, bh);
                wrong_colour += out[index] != want || writes[index] != 1;
            }
        }
        first += bw * bh;
        at += bw * bh * 16;
    }
    // The alpha render, into an A8 chain at the batch's stride.
    u32 texels = 0;
    for (u32 l = 0; l < levels; l++) {
        texels += (width >> l) * (height >> l);
    }
    std::vector<u8> alpha(texels + UntileSide * UntileSide, 0);
    std::vector<u8> alpha_writes(alpha.size(), 0);
    const u8* const nibbles = reinterpret_cast<const u8*>(scratch.data());
    const ChainRender alpha_render = ChainAt(width, height, levels, AlphaBatchStride);
    if (alpha_render.rows > UntileSide) {
        printf("%s: alpha target %u x %u\n", what, alpha_render.stride, alpha_render.rows);
        failures++;
        return;
    }
    for (const ChainRender& cr : {alpha_render}) {
        count = Etc1A4AlphaVertices(cr, width, height, levels, nullptr, draws);
        v.assign(count * SGXD_PROGRAM_BLIT_VERTEX_WORDS, NAN);
        Etc1A4AlphaVertices(cr, width, height, levels, v.data(), filled);
        RasteriseDraws(what, v, draws, cr.stride, cr.rows, [&](u32 px, u32 py, float u, float vv,
                                                               const GeometryDraw& d) {
            const u32 col = static_cast<u32>(std::floor(u * d.view_width));
            const u32 row = static_cast<u32>(std::floor(vv * d.view_rows));
            const u64 n = u64{row} * d.view_width + col;
            const u8 byte = nibbles[d.source + n / 2];
            const u32 at = cr.offset + py * cr.stride + px;
            alpha[at] = static_cast<u8>(((n & 1) ? byte >> 4 : byte & 15) * 17);
            alpha_writes[at]++;
        });
    }
    u32 wrong_alpha = 0;
    for (u32 l = 0, at = 0, host = 0; l < levels; l++) {
        const u32 w = width >> l, h = height >> l;
        for (u32 y = 0; y < h; y++) {
            for (u32 x = 0; x < w; x++) {
                const u32 tile = (y / 8) * (w / 8) + x / 8;
                const u32 block = tile * 4 + (x % 8) / 4 + 2 * ((y % 8) / 4);
                u64 a;
                std::memcpy(&a, &guest[at + block * 16], 8);
                const u32 want = static_cast<u32>(a >> (4 * (4 * (x % 4) + y % 4)) & 15) * 17;
                wrong_alpha += alpha[host + y * w + x] != want || alpha_writes[host + y * w + x] != 1;
            }
        }
        at += w * h;
        host += w * h;
    }
    if (wrong_colour || wrong_alpha) {
        printf("%s: %u colour blocks wrong, %u alpha texels wrong\n", what, wrong_colour,
               wrong_alpha);
        failures++;
    }
    printf("%s: gather %u x %u words, ok\n", what, g.stride, g.rows);
}

u32 MaxLevels(u32 w, u32 h) {
    u32 levels = 1;
    while (w >> levels >= 8 && h >> levels >= 8) {
        levels++;
    }
    return levels;
}

} // namespace

int main() {
    static const u32 sizes[][2] = {
        {8, 8},     {16, 16},   {32, 32},  {64, 32},   {32, 64},   {128, 16},  {16, 128},
        {256, 256}, {512, 64},  {64, 512}, {1024, 8},  {8, 1024},  {32, 1024}, {1024, 32},
        {512, 256}, {256, 512}, {1024, 1024},
    };
    for (const auto& s : sizes) {
        const u32 max = MaxLevels(s[0], s[1]);
        for (const u32 bits : {32u, 16u, 8u, 4u}) {
            CheckChain(s[0], s[1], max, bits);
            CheckChain(s[0], s[1], max, bits, true);
        }
        CheckChain(s[0], s[1], 1, 32, true);
        if (max > 2) {
            CheckChain(s[0], s[1], 2, 24);
        }
        CheckEtc1(s[0], s[1], max);
        if (max > 1) {
            CheckEtc1(s[0], s[1], 1);
        }
        if (s[0] <= Etc1A4MaxWidth) {
            CheckEtc1A4(s[0], s[1], max);
            if (max > 1) {
                CheckEtc1A4(s[0], s[1], 1);
            }
        }
    }
    printf("untile_geometry_test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
