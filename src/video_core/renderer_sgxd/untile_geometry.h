// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

// The untiles' geometry, apart from the GPU: the blit quads that read a PICA texture through
// SWIZZLED views and where they land in the side render's target. SgxdBackend draws them, and
// untile_geometry_test.cpp rasterises them on the host against the layouts they produce.

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <utility>
#include <vector>
#include "common/common_types.h"
#include "sgxd_program.h"

namespace SgxdRenderer::UntileGeometry {

/// The side recipe's largest target side, and so a chain render's rows.
constexpr u32 UntileSide = 1024;

constexpr u32 QuadVertexWords = 6 * SGXD_PROGRAM_BLIT_VERTEX_WORDS;

/// The last tile column and row a width x height side render's region clip keeps: every tile
/// the target covers, the partial last ones too (a chain's target has any number of rows).
/// Rounding down would lose a 128x36 target's rows 32-35.
inline std::pair<u32, u32> LastTile(u32 width, u32 height) {
    return {(std::max(width, 1u) + 31) / 32 - 1, (std::max(height, 1u) + 31) / 32 - 1};
}

/// One quad as the blit's two triangles into v: target pixels [x0, x1) x [y0, y1) of a
/// width x height viewport, and the texture coordinate at each corner (00 at x0, y0; 10 at
/// x1, y0; 01 at x0, y1; 11 at x1, y1).
inline void PutQuad(float* v, u32 width, u32 height, float x0, float y0, float x1, float y1,
                    const float (&uv)[4][2]) {
    constexpr u32 N = SGXD_PROGRAM_BLIT_VERTEX_WORDS;
    const float corner[4][2] = {{x0, y0}, {x1, y0}, {x0, y1}, {x1, y1}};
    static constexpr u32 order[6] = {0, 1, 2, 1, 3, 2};
    for (u32 n = 0; n < 6; n++) {
        const u32 c = order[n];
        const float words[N] = {2 * corner[c][0] / width - 1,
                                2 * corner[c][1] / height - 1,
                                0.5f,
                                1.0f,
                                1.0f,
                                1.0f,
                                1.0f,
                                1.0f,
                                uv[c][0],
                                uv[c][1]};
        std::memcpy(v + n * N, words, sizeof(words));
    }
}

/// PutQuad for image rows [y0, y1) of an image w texels wide whose texel 0 is `offset` texels
/// into a width x height target, row_texels texels a target row: one quad when the image's
/// rows are the target's, else a quad per row (row_texels / w of them side by side on each
/// target row) with the coordinates interpolated down the block. Returns the quads written,
/// each QuadVertexWords.
inline u32 PutRowsAt(float* v, u32 width, u32 height, u32 row_texels, u32 w, u32 offset, u32 y0,
                     u32 y1, const float (&uv)[4][2]) {
    if (w == row_texels) {
        const u32 top = offset / row_texels;
        PutQuad(v, width, height, 0, static_cast<float>(top + y0), static_cast<float>(w),
                static_cast<float>(top + y1), uv);
        return 1;
    }
    const auto lerp = [](const float (&a)[2], const float (&b)[2], float f, u32 i) {
        return a[i] + (b[i] - a[i]) * f;
    };
    for (u32 y = y0; y < y1; y++) {
        const float f0 = static_cast<float>(y - y0) / (y1 - y0);
        const float f1 = static_cast<float>(y + 1 - y0) / (y1 - y0);
        const float row[4][2] = {
            {lerp(uv[0], uv[2], f0, 0), lerp(uv[0], uv[2], f0, 1)},
            {lerp(uv[1], uv[3], f0, 0), lerp(uv[1], uv[3], f0, 1)},
            {lerp(uv[0], uv[2], f1, 0), lerp(uv[0], uv[2], f1, 1)},
            {lerp(uv[1], uv[3], f1, 0), lerp(uv[1], uv[3], f1, 1)},
        };
        const u32 at = offset + y * w;
        const float x0 = static_cast<float>(at % row_texels);
        const float ty = static_cast<float>(at / row_texels);
        PutQuad(v + (y - y0) * QuadVertexWords, width, height, x0, ty, x0 + w, ty + 1, row);
    }
    return y1 - y0;
}

/// A PICA-tiled picture's bands of tile rows (Untile, DrawTiled): a band's tiles are a SWIZZLED
/// texture 8 texels wide and rows * w tall, at most 4096 (the texture unit's largest side), its
/// height rounded up to a power of two; the quads read only the band's own rows. Each band's
/// first vertex is a multiple of 8 (the recipe's rule), after unused vertices where the band
/// before ends short of it. An ETC1 chain's views are two blocks wide and w blocks a tile row.
struct Band {
    u32 first_row, rows, first_vertex;
};

inline std::vector<Band> TileBands(u32 w, u32 tile_rows, u32 quads_a_row, u32& vertex_count) {
    const u32 max_band = std::min(tile_rows, 4096 / w);
    std::vector<Band> bands;
    vertex_count = 0;
    for (u32 ty = 0; ty < tile_rows; ty += max_band) {
        const u32 first = (vertex_count + 7) & ~7u;
        const u32 rows = std::min(max_band, tile_rows - ty);
        bands.push_back({ty, rows, first});
        vertex_count = first + rows * quads_a_row * 6;
    }
    return bands;
}

/// One draw of an untile's side render: vertices [first, first + count), which read a view
/// view_rows tall at byte `source` from the source's start: a band's SWIZZLED view (a chain,
/// an ETC1 chain), or a LINEAR one view_width texels wide (a split ETC1A4's gather and alpha).
struct GeometryDraw {
    u32 first, count, view_rows, source;
    u32 view_width = 0;
};

/// A chain's side renders (UntileChain's, or its place in a batch, ChainAt): levels
/// [first, end) into a target `stride` texels wide and `rows` tall that starts at texel
/// `offset` of the chain.
struct ChainRender {
    u32 first, end, offset, stride, rows;
};

/// A width x height chain of `levels` levels in renders: the target's rows are max(w, 32)
/// texels of the render's first level, and a render ends before a level that would take it
/// past UntileSide rows.
inline std::vector<ChainRender> ChainRenders(u32 width, u32 height, u32 levels) {
    std::vector<ChainRender> renders;
    for (u32 l = 0, offset = 0; l < levels;) {
        ChainRender r{l, l, offset, std::max(width >> l, 32u), 0};
        u32 texels = 0;
        while (r.end < levels) {
            const u32 n = (width >> r.end) * (height >> r.end);
            if (r.end > l && (texels + n + r.stride - 1) / r.stride > UntileSide) {
                break;
            }
            texels += n;
            r.end++;
        }
        r.rows = (texels + r.stride - 1) / r.stride;
        renders.push_back(r);
        offset += texels;
        l = r.end;
    }
    return renders;
}

/// A batched chain's target stride (SgxdBackend::QueueUntile): every chain in a batch is laid in
/// rows of the batch's stride, 256 texels for a level 0 up to 256 wide, else its own width. A level
/// narrower than the stride is a quad a texel row (PutRowsAt): a 512-wide level in UntileSide rows
/// would be 512 quads one pixel tall; at its width a tile row is one quad.
inline u32 ChainBatchStride(u32 width) {
    return width <= 256 ? 256 : width <= 512 ? 512 : UntileSide;
}

/// A chain of `levels` levels laid in rows of `stride` texels (each level's width divides it):
/// the one render that holds all its levels, its rows rounded up. More than UntileSide rows is
/// more than a render holds.
inline ChainRender ChainAt(u32 width, u32 height, u32 levels, u32 stride) {
    u32 texels = 0;
    for (u32 l = 0; l < levels; l++) {
        texels += (width >> l) * (height >> l);
    }
    return {0, levels, 0, stride, (texels + stride - 1) / stride};
}

/// Render cr's draws (a band of a level each) of a width x height chain of `bits` texels, and
/// with v its vertices: Untile's quads, a tile row a quad, or a row a quad for a level
/// narrower than the target, at the level's place. Returns the vertices' count.
inline u32 ChainVertices(const ChainRender& cr, u32 width, u32 height, u32 bits, float* v,
                         std::vector<GeometryDraw>& draws) {
    draws.clear();
    u32 guest = 0;
    for (u32 l = 0; l < cr.first; l++) {
        guest += (width >> l) * (height >> l) * bits / 8;
    }
    u32 vertex_count = 0;
    for (u32 l = cr.first, offset = 0; l < cr.end; l++) {
        const u32 w = width >> l, h = height >> l;
        const u32 quads = w == cr.stride ? 1 : 8; // a tile row's (PutRowsAt)
        u32 count = 0;
        const std::vector<Band> bands = TileBands(w, h / 8, quads, count);
        // A multiple of 8, the recipe's rule for a draw's first vertex.
        const u32 base = (vertex_count + 7) & ~7u;
        vertex_count = base + count;
        for (const Band& band : bands) {
            // Tile row ty holds image rows 8ty..8ty+7. Along a quad, image x is the band's
            // SWIZZLED row (ty's local row) * w + x, image y the column: pixel centres land on
            // texel centres.
            const u32 view_rows = std::bit_ceil(band.rows * w);
            float* const band_v =
                v ? v + (base + band.first_vertex) * SGXD_PROGRAM_BLIT_VERTEX_WORDS : nullptr;
            for (u32 row = 0; row < band.rows && v; row++) {
                const u32 ty = band.first_row + row;
                const float v0 = static_cast<float>(row * w) / view_rows;
                const float v1 = static_cast<float>((row + 1) * w) / view_rows;
                const float uv[4][2] = {{0, v0}, {0, v1}, {1, v0}, {1, v1}};
                PutRowsAt(band_v + row * quads * QuadVertexWords, cr.stride, cr.rows, cr.stride, w,
                          offset, 8 * ty, 8 * (ty + 1), uv);
            }
            draws.push_back({base + band.first_vertex, band.rows * quads * 6, view_rows,
                             guest + band.first_row * 8 * w * bits / 8, 8});
        }
        offset += w * h;
        guest += w * h * bits / 8;
    }
    return vertex_count;
}

/// An ETC1 chain's own target in blocks, whose height its batch takes (SgxdBackend::QueueUntile):
/// its height the PBE's smaller side, at least 32 and level 0's smaller side, so that each of the
/// chain's square runs is one of its squares; its width the power of two from there that holds the
/// chain. 32 keeps every tile's store inside the twiddled sides, as the TWIDDLED stores measured
/// so far are (whether word 5 clips a TWIDDLED store inside a tile is unknown).
struct Etc1Target {
    u32 width, height;
};

inline Etc1Target Etc1TargetFor(u32 width, u32 height, u32 levels) {
    u32 blocks = 0;
    for (u32 l = 0; l < levels; l++) {
        blocks += (width >> l) / 4 * ((height >> l) / 4);
    }
    const u32 side = std::max(32u, std::min(width, height) / 4);
    return {std::max(side, std::bit_ceil((blocks + side - 1) / side)), side};
}

/// The TWIDDLED squares (Etc1TargetFor's height a side) an ETC1 chain's blocks take: its slot
/// in a batch of that height, where its quads move along by whole squares.
inline u32 Etc1Squares(u32 width, u32 height, u32 levels) {
    u32 blocks = 0;
    for (u32 l = 0; l < levels; l++) {
        blocks += (width >> l) / 4 * ((height >> l) / 4);
    }
    const u32 side = Etc1TargetFor(width, height, levels).height;
    return (blocks + side * side - 1) / (side * side);
}

/// The pixel at index i of the PBE's TWIDDLED layout for a target whose smaller side is its
/// height, `side`: Morton over side x side squares with y in bit 0, the squares along x
/// (the PICA tile store's 16-row strips have the column's bits above the square's).
inline std::pair<u32, u32> TwiddledPixel(u32 i, u32 side) {
    const u32 square = side * side, low = i % square;
    u32 x = 0, y = 0;
    for (u32 b = 0; (1u << 2 * b) < square; b++) {
        y |= (low >> 2 * b & 1u) << b;
        x |= (low >> (2 * b + 1) & 1u) << b;
    }
    return {x + i / square * side, y};
}

/// The draws (a band of a level each) for a width x height ETC1 chain of `levels`
/// levels, and with v their vertices. Returns the vertices' count.
///
/// A PICA ETC1 level is 8x8 tiles row after row, each four blocks (x first), each block one
/// u64. Read as a SWIZZLED texture of 64-bit texels two wide (Morton over the 2-wide grid:
/// index (sy & 1) | sx << 1 | (sy >> 1) << 2), a tile's blocks are four texels: texel (sx, sy)
/// is block (2 tx + (sy & 1), 2 ty + sx) of tile sy / 2. Over a tile row ty of a level bw
/// blocks wide, block (bx, by) is therefore texel (by & 1, bw (ty - band's first) + bx) of the
/// band's view: x along the view's rows, the block row's parity across them.
///
/// A level of bw x bh blocks is SWIZZLED over its smaller side s: square runs of s x s blocks,
/// Morton inside, along the larger side. Run k starts at block index first + k s^2 of the
/// chain, and the target holds that index range as an aligned s x s square in the same order
/// (s is at most its height), so a run moves whole to TwiddledPixel(first + k s^2): one quad
/// for each run and tile row. A wide or square level's tile row crosses all bw / s runs; a
/// tall one's lies in one (bw / s is 1).
inline u32 Etc1Vertices(u32 width, u32 height, u32 levels, float* v,
                        std::vector<GeometryDraw>& draws) {
    draws.clear();
    const Etc1Target target = Etc1TargetFor(width, height, levels);
    u32 vertex_count = 0;
    for (u32 l = 0, blocks = 0, guest = 0; l < levels; l++) {
        const u32 bw = (width >> l) / 4, bh = (height >> l) / 4, side = std::min(bw, bh);
        const u32 quads = bw / side;
        const bool wide = bw >= bh;
        // A band's tile rows read through one view: bw view rows a tile row, 4096 at most.
        u32 count = 0;
        const std::vector<Band> bands = TileBands(bw, bh / 2, quads, count);
        const u32 base = (vertex_count + 7) & ~7u;
        vertex_count = base + count;
        for (const Band& band : bands) {
            const u32 view_rows = std::bit_ceil(band.rows * bw);
            float* const band_v =
                v ? v + (base + band.first_vertex) * SGXD_PROGRAM_BLIT_VERTEX_WORDS : nullptr;
            for (u32 row = 0; row < band.rows && v; row++) {
                const u32 ty = band.first_row + row;
                for (u32 q = 0; q < quads; q++) {
                    // The run's first block: (q s, 0) in a wide or square level, (0, k s) with
                    // k = 2 ty / s in a tall one.
                    const u32 k = wide ? q : 2 * ty / side;
                    const u32 run_x = wide ? q * side : 0, run_y = wide ? 0 : k * side;
                    const auto [px, py] = TwiddledPixel(blocks + k * side * side, target.height);
                    const u32 y0 = py + 2 * ty - run_y;
                    const float va = static_cast<float>(row * bw + run_x) / view_rows;
                    const float vb = static_cast<float>(row * bw + run_x + side) / view_rows;
                    const float uv[4][2] = {{0, va}, {0, vb}, {1, va}, {1, vb}};
                    PutQuad(band_v + (row * quads + q) * QuadVertexWords, target.width,
                            target.height, static_cast<float>(px), static_cast<float>(y0),
                            static_cast<float>(px + side), static_cast<float>(y0 + 2), uv);
                }
            }
            // A tile row is bw / 2 tiles of 32 bytes.
            draws.push_back({base + band.first_vertex, band.rows * quads * 6, view_rows,
                             guest + band.first_row * 16 * bw, 2});
        }
        blocks += bw * bh;
        guest += bw * bh * 8;
    }
    return vertex_count;
}

/// A split ETC1A4's gather (SgxdBackend::QueueUntile): raw word copies from the guest's ETC1A4
/// chain (8x8 tiles of four blocks, x first, each block its alpha u64 and then its colour u64)
/// into rows `stride` words wide. Rows [0, colour_rows): every colour word of the chain in
/// ETC1's layout, eight words a tile in the guest's block order and word order, colour_words
/// of them, which the ETC1 batch then moves. From row alpha_row[l]: level l's alpha words, a
/// target row a tile row, block row 0's in x order and then block row 1's (UntileEtc1A4Merge's
/// gather, rows `stride` apart).
struct Etc1A4Gather {
    u32 stride, colour_rows, rows, colour_words;
    std::array<u32, 16> alpha_row;
};

/// The widest level 0 the gather takes: the alpha render reads a level's tile rows as a LINEAR
/// P4 view 8 * stride nibbles wide, and the texture unit's widest is 4096.
constexpr u32 Etc1A4MaxWidth = 512;
/// Every gather's stride, so that a batch's gathers share one target: 4096 / 8 nibbles.
constexpr u32 Etc1A4GatherStride = 512;
/// A batched A8 alpha chain's target stride in bytes, which takes a level 0 up to Etc1A4MaxWidth
/// wide and UntileSide tall in one render (ChainAt).
constexpr u32 AlphaBatchStride = UntileSide;

inline Etc1A4Gather Etc1A4GatherFor(u32 width, u32 height, u32 levels) {
    Etc1A4Gather g{};
    g.stride = Etc1A4GatherStride;
    u32 tiles = 0;
    for (u32 l = 0; l < levels; l++) {
        tiles += (width >> l) / 8 * ((height >> l) / 8);
    }
    g.colour_words = tiles * 8;
    g.colour_rows = (g.colour_words + g.stride - 1) / g.stride;
    u32 row = g.colour_rows;
    for (u32 l = 0; l < levels; l++) {
        g.alpha_row[l] = row;
        row += (height >> l) / 8;
    }
    g.rows = row;
    return g;
}

/// The gather's draws, and with v its vertices: one draw for the colour words, whose LINEAR
/// view holds stride / 8 tiles a row (2 * stride words), and one a level for its alpha words,
/// whose view is a tile row a row (2 w words). Each target column is one quad over its rows,
/// reading one view column; the colour's only as many columns as it has words. Returns the
/// vertices' count.
inline u32 Etc1A4GatherVertices(u32 width, u32 height, u32 levels, float* v,
                                std::vector<GeometryDraw>& draws) {
    draws.clear();
    const Etc1A4Gather g = Etc1A4GatherFor(width, height, levels);
    constexpr u32 N = SGXD_PROGRAM_BLIT_VERTEX_WORDS;
    // Colour: target word x of a row is word x % 8 of tile x / 8 there, block (x % 8) / 2's
    // colour word x % 2: view column 16 (x / 8) + 4 ((x % 8) / 2) + 2 + x % 2.
    const u32 colour_view = 2 * g.stride;
    const u32 columns = std::min(g.stride, g.colour_words);
    for (u32 x = 0; x < columns && v; x++) {
        const u32 c = 16 * (x / 8) + 4 * (x % 8 / 2) + 2 + x % 2;
        const float u = (c + 0.5f) / colour_view;
        const float uv[4][2] = {{u, 0}, {u, 0}, {u, 1}, {u, 1}};
        PutQuad(v + x * QuadVertexWords, g.stride, g.rows, static_cast<float>(x), 0,
                static_cast<float>(x + 1), static_cast<float>(g.colour_rows), uv);
    }
    draws.push_back({0, columns * 6, g.colour_rows, 0, colour_view});
    u32 vertex_count = columns * 6;
    // Alpha: word d of a level's target row holds block (bx, p)'s alpha word i1, d = (w / 2) p +
    // 2 bx + i1; the view's row is the tile row, 16 words a tile, the block's alpha words at
    // 4 ((bx & 1) + 2 p) + i1 of tile bx >> 1.
    for (u32 l = 0, guest = 0; l < levels; l++) {
        const u32 w = width >> l, h = height >> l, tile_rows = h / 8;
        const u32 base = (vertex_count + 7) & ~7u;
        for (u32 d = 0; d < w && v; d++) {
            const u32 p = d / (w / 2), bx = d % (w / 2) / 2, i1 = d % 2;
            const u32 c = 16 * (bx >> 1) + 4 * (bx & 1) + 8 * p + i1;
            const float u = (c + 0.5f) / (2 * w);
            const float uv[4][2] = {{u, 0}, {u, 0}, {u, 1}, {u, 1}};
            PutQuad(v + (base * N) + d * QuadVertexWords, g.stride, g.rows, static_cast<float>(d),
                    static_cast<float>(g.alpha_row[l]), static_cast<float>(d + 1),
                    static_cast<float>(g.alpha_row[l] + tile_rows), uv);
        }
        draws.push_back({base, w * 6, tile_rows, guest, 2 * w});
        vertex_count = base + w * 6;
        guest += w * h; // a byte a texel
    }
    return vertex_count;
}

/// A split ETC1A4's alpha render cr (ChainAt over an A8 chain at AlphaBatchStride: a byte a
/// texel, rows of the level's width): each level's texels from its gathered alpha words, read
/// as a LINEAR P4 view 8 * stride nibbles wide with a tile row a row, through a palette whose
/// entry n is the alpha 17n. Texel (x, y)'s nibble there is 4w ((y >> 2) & 1) + 4x + (y & 3):
/// affine along a row, so a quad a row. Returns the vertices' count.
inline u32 Etc1A4AlphaVertices(const ChainRender& cr, u32 width, u32 height, u32 levels,
                               float* v, std::vector<GeometryDraw>& draws) {
    draws.clear();
    const Etc1A4Gather g = Etc1A4GatherFor(width, height, levels);
    const u32 p4_width = 8 * g.stride;
    u32 vertex_count = 0;
    for (u32 l = cr.first, offset = 0; l < cr.end; l++) {
        const u32 w = width >> l, h = height >> l, tile_rows = h / 8;
        const u32 base = (vertex_count + 7) & ~7u;
        for (u32 y = 0; y < h && v; y++) {
            const u32 k = 4 * w * (y >> 2 & 1) + (y & 3);
            // Nibble 4x + k's centre at pixel x's centre.
            const float ua = (k - 1.5f) / p4_width, ub = (4.0f * w + k - 1.5f) / p4_width;
            const float row = (static_cast<float>(y >> 3) + 0.5f) / tile_rows;
            const float uv[4][2] = {{ua, row}, {ub, row}, {ua, row}, {ub, row}};
            const u32 at = offset + y * w;
            const float x0 = static_cast<float>(at % cr.stride);
            const float ty = static_cast<float>(at / cr.stride);
            PutQuad(v + (base + y * 6) * SGXD_PROGRAM_BLIT_VERTEX_WORDS, cr.stride, cr.rows, x0, ty,
                    x0 + w, ty + 1, uv);
        }
        draws.push_back({base, h * 6, tile_rows, g.alpha_row[l] * g.stride * 4, p4_width});
        vertex_count = base + h * 6;
        offset += w * h;
    }
    return vertex_count;
}

/// A compressed cube's face stride in bytes (SgxdBackend::UntileCube), `bits` a texel: 4 for its
/// ETC1 colour, 8 for its A8 alpha. Without mipmaps the faces touch. With them a face is its whole
/// chain to 1x1, a block format's levels under 4x4 a block each, in a slot rounded up to 2 KiB
/// when the side is over 16: libgxm's rule for 8-bit and block formats, whose 2 KiB slots the
/// U8888 cube shows (docs/sgxd/tsp.md).
inline u32 CompressedCubeFaceStride(u32 width, bool mipmapped, u32 bits) {
    if (!mipmapped) {
        return width * width * bits / 8;
    }
    const u32 block = bits == 4 ? 4 : 1;
    u32 chain = 0;
    for (u32 side = width; side >= 1; side >>= 1) {
        const u32 s = std::max(side, block);
        chain += s * s * bits / 8;
    }
    const u32 align = width > 16 ? 2048 : 4;
    return (chain + align - 1) / align * align;
}

/// Whether a cube `width` wide is kept compressed: from 32 a side, where its colour's and its
/// alpha's face slots are multiples of 128 bytes whatever its levels (512 bytes and up, or 2 KiB
/// slots): TWIDDLED face stores at 184 and 344 bytes apart came out wrong or stopped the 3D. By
/// the side alone, so that the split alpha (RasterizerSgxd::SplitAlphaIn) follows the fragment
/// registers' size word. A narrower cube stays RGBA8 (12 KiB at most).
inline bool CompressedCubeFits(u32 width) {
    return width >= 32;
}

/// The bytes a compressed cube takes (six faces CompressedCubeFaceStride apart, `levels` of each
/// rendered): the last face's render stores the whole 32x32 TWIDDLED tiles its chain touches,
/// aligned runs of 1024 pixels from the face (8 bytes a pixel for the colour's blocks, one for
/// the alpha's texels).
inline u32 CompressedCubeBytes(u32 width, u32 levels, u32 bits) {
    const u32 block = bits == 4 ? 4 : 1;
    u32 pixels = 0;
    for (u32 l = 0; l < levels; l++) {
        const u32 side = (width >> l) / block;
        pixels += side * side;
    }
    const u32 stride = CompressedCubeFaceStride(width, levels > 1, bits);
    const u32 stored = (pixels + 1023) / 1024 * 1024 * (bits == 4 ? 8 : 1);
    return 5 * stride + std::max(stride, stored);
}

/// A split ETC1A4 cube face's A8 chain target (CubeAlphaVertices): TWIDDLED, a byte a texel, its
/// height max(32, width) so that level 0 is one of its squares.
struct CubeAlphaTarget {
    u32 width, height;
};

inline CubeAlphaTarget CubeAlphaTargetFor(u32 width, u32 levels) {
    u32 texels = 0;
    for (u32 l = 0; l < levels; l++) {
        texels += (width >> l) * (width >> l);
    }
    const u32 side = std::max(32u, width);
    return {std::bit_ceil((texels + side * side - 1) / (side * side)) * side, side};
}

/// A split ETC1A4 cube face's alpha render: the face's SWIZZLED A8 levels back to back, level l
/// the square of its side at TwiddledPixel of its first texel in CubeAlphaTargetFor's target,
/// where texel (x, y) lands at the square's (x, y). Each level's texels come from its gathered
/// alpha words as Etc1A4AlphaVertices reads them, a quad a texel row. Returns the vertices'
/// count.
inline u32 CubeAlphaVertices(u32 width, u32 levels, float* v, std::vector<GeometryDraw>& draws) {
    draws.clear();
    const Etc1A4Gather g = Etc1A4GatherFor(width, width, levels);
    const CubeAlphaTarget target = CubeAlphaTargetFor(width, levels);
    const u32 p4_width = 8 * g.stride;
    u32 vertex_count = 0;
    for (u32 l = 0, offset = 0; l < levels; l++) {
        const u32 w = width >> l, tile_rows = w / 8;
        const auto [px, py] = TwiddledPixel(offset, target.height);
        const u32 base = (vertex_count + 7) & ~7u;
        for (u32 y = 0; y < w && v; y++) {
            const u32 k = 4 * w * (y >> 2 & 1) + (y & 3);
            // Nibble 4x + k's centre at pixel x's centre.
            const float ua = (k - 1.5f) / p4_width, ub = (4.0f * w + k - 1.5f) / p4_width;
            const float row = (static_cast<float>(y >> 3) + 0.5f) / tile_rows;
            const float uv[4][2] = {{ua, row}, {ub, row}, {ua, row}, {ub, row}};
            PutQuad(v + (base + y * 6) * SGXD_PROGRAM_BLIT_VERTEX_WORDS, target.width,
                    target.height, static_cast<float>(px), static_cast<float>(py + y),
                    static_cast<float>(px + w), static_cast<float>(py + y + 1), uv);
        }
        draws.push_back({base, w * 6, tile_rows, g.alpha_row[l] * g.stride * 4, p4_width});
        vertex_count = base + w * 6;
        offset += w * w;
    }
    return vertex_count;
}

} // namespace SgxdRenderer::UntileGeometry
