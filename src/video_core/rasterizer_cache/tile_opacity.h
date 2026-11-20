// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <algorithm>
#include <cstring>
#include <span>
#include "common/mh4u_cover.h"
#include "video_core/rasterizer_cache/surface_base.h"

// Texture opacity for MH4U's cover proof (docs/plans/vita-mh4u-menu-world-skip.md): which 8x8
// tiles of a texture have alpha 255 in every texel. A PICA texture is stored as 8x8 tiles in
// memory order, so one bit per tile is read straight from the guest bytes an upload decodes -
// no geometry, no decoded copy. The panels of the quest counter are an opaque region of an
// ETC1A4 texture: only such a map can prove them opaque. Costs 2 bits per 64 texels (4 KiB for
// a 1024x1024 texture), only for textures with alpha; kept under `covertrap` and for the
// opaque 2D sprites (Common::Mh4uCover::opacity_maps).
namespace VideoCore {

/// Bytes of one 8x8 tile of `format`, 0 for formats without alpha (always opaque) or that
/// this map does not cover.
constexpr u32 OpacityTileBytes(PixelFormat format) {
    switch (format) {
    case PixelFormat::RGBA8:
        return 256;
    case PixelFormat::RGB5A1:
    case PixelFormat::RGBA4:
    case PixelFormat::IA8:
        return 128;
    case PixelFormat::A8:
    case PixelFormat::IA4:
    case PixelFormat::ETC1A4:
        return 64;
    case PixelFormat::A4:
        return 32;
    default:
        return 0;
    }
}

/// Every texel of the tile at `t` (guest layout) has alpha 255.
inline bool TileOpaque(PixelFormat format, const u8* t) {
    const auto all = [&](u32 stride, u32 offset, u8 mask) {
        for (u32 i = offset; i < OpacityTileBytes(format); i += stride) {
            if ((t[i] & mask) != mask) {
                return false;
            }
        }
        return true;
    };
    switch (format) {
    case PixelFormat::RGBA8:
        return all(4, 0, 0xFF); // bytes A, B, G, R
    case PixelFormat::RGB5A1:
        return all(2, 0, 0x01); // bit 0 of the little-endian word
    case PixelFormat::RGBA4:
        return all(2, 0, 0x0F); // low nibble
    case PixelFormat::IA8:
        return all(2, 0, 0xFF); // byte 0
    case PixelFormat::A8:
    case PixelFormat::A4:
        return all(1, 0, 0xFF); // every byte (A4: two texels a byte)
    case PixelFormat::IA4:
        return all(1, 0, 0x0F); // low nibble
    case PixelFormat::ETC1A4:
        // Four 4x4 blocks of 16 bytes, each starting with 64 bits of 4-bit alphas.
        for (u32 b = 0; b < 64; b += 16) {
            u64 alpha;
            std::memcpy(&alpha, t + b, sizeof(alpha));
            if (alpha != ~0ull) {
                return false;
            }
        }
        return true;
    default:
        return false;
    }
}

/// coverlog: the lowest alpha (0-255) in the tile at `t` and how many texels are below 255.
inline u32 TileAlphaStats(PixelFormat format, const u8* t, u32* low_texels) {
    u32 min = 255, low = 0;
    const auto note = [&](u32 a) {
        min = std::min(min, a);
        low += a < 255 ? 1 : 0;
    };
    switch (format) {
    case PixelFormat::RGBA8:
        for (u32 i = 0; i < 64; i++) {
            note(t[i * 4]);
        }
        break;
    case PixelFormat::RGB5A1:
        for (u32 i = 0; i < 64; i++) {
            note((t[i * 2] & 1) ? 255 : 0);
        }
        break;
    case PixelFormat::RGBA4:
    case PixelFormat::IA4:
        for (u32 i = 0; i < 64; i++) {
            const u8 nib = format == PixelFormat::RGBA4 ? t[i * 2] & 0xF : t[i] & 0xF;
            note(nib * 17);
        }
        break;
    case PixelFormat::IA8:
        for (u32 i = 0; i < 64; i++) {
            note(t[i * 2]);
        }
        break;
    case PixelFormat::A8:
        for (u32 i = 0; i < 64; i++) {
            note(t[i]);
        }
        break;
    case PixelFormat::A4:
        for (u32 i = 0; i < 32; i++) {
            note((t[i] & 0xF) * 17);
            note((t[i] >> 4) * 17);
        }
        break;
    case PixelFormat::ETC1A4:
        for (u32 b = 0; b < 64; b += 16) {
            for (u32 i = 0; i < 8; i++) {
                note((t[b + i] & 0xF) * 17);
                note((t[b + i] >> 4) * 17);
            }
        }
        break;
    default:
        break;
    }
    if (low_texels != nullptr) {
        *low_texels = low;
    }
    return min;
}

/// Level 0's tile count, 0 when the surface keeps no map.
inline u32 OpacityTiles(const SurfaceBase& s) {
    const u32 tile_bytes = OpacityTileBytes(s.pixel_format);
    // Textures, and colour surfaces too: a page the CPU writes may be cached as one. Any GPU
    // write moves the tick and leaves the map stale, so only memory content is ever described.
    if (tile_bytes == 0 || (s.type != SurfaceType::Texture && s.type != SurfaceType::Color)) {
        return 0;
    }
    return s.width * s.height / 64;
}

/// An upload wrote the guest bytes [addr, addr + data.size()) into `s`: note the opacity of
/// every whole level-0 tile among them. A map that went stale (the tick moved since it was
/// last made valid) starts over with every tile unknown.
inline void NoteUploadOpacity(SurfaceBase& s, PAddr addr, std::span<const u8> data) {
    if (!Common::Mh4uCover::opacity_maps) {
        return;
    }
    const u32 tiles = OpacityTiles(s);
    if (tiles == 0) {
        return;
    }
    const u32 tile_bytes = OpacityTileBytes(s.pixel_format);
    const std::size_t words = (tiles + 63) / 64;
    if ((!s.opacity_pending && s.opacity_tick != s.modification_tick) ||
        s.known_tiles.size() != words) {
        s.known_tiles.assign(words, 0);
        s.opaque_tiles.assign(words, 0);
    }
    // From here the map describes the surface as this validation leaves it, even when this
    // upload is of another mip level and sets no tile.
    s.opacity_pending = true;
    const PAddr level0_end = s.addr + tiles * tile_bytes;
    const PAddr from = std::max<PAddr>(addr, s.addr);
    const PAddr to = std::min<PAddr>(addr + static_cast<u32>(data.size()), level0_end);
    if (from >= to) {
        return;
    }
    const u32 first = (from - s.addr + tile_bytes - 1) / tile_bytes;
    const u32 last = (to - s.addr) / tile_bytes; // exclusive
    for (u32 i = first; i < last; i++) {
        const u8* t = data.data() + (s.addr + i * tile_bytes - addr);
        const u64 bit = 1ull << (i % 64);
        s.known_tiles[i / 64] |= bit;
        if (TileOpaque(s.pixel_format, t)) {
            s.opaque_tiles[i / 64] |= bit;
        } else {
            s.opaque_tiles[i / 64] &= ~bit;
        }
    }
}

/// After a validation: the tiles an upload just set are what the surface holds now.
inline void SyncOpacityTick(SurfaceBase& s) {
    if (s.opacity_pending) {
        s.opacity_pending = false;
        s.opacity_tick = s.modification_tick;
        s.opacity_hash = s.upload_hash; // a whole upload's hash, 0 after a partial one
    }
}

/// The whole surface was just found to match guest bytes hashing to `upload_hash`: a valid
/// map now describes those bytes.
inline void NoteOpacityHash(SurfaceBase& s) {
    if (s.opacity_tick == s.modification_tick) {
        s.opacity_hash = s.upload_hash;
    }
}

/// After a validation that changed nothing (the guest's bytes hashed the same as the upload's):
/// a map that was valid before, or that was made from bytes with this hash, is valid again -
/// the guest rewrote a static texture with what it held (MH4U does that every frame), which
/// marked the surface invalid in between.
inline void KeepOpacityTick(SurfaceBase& s, u64 tick_before) {
    if (s.opacity_tick == tick_before ||
        (s.opacity_hash != 0 && s.opacity_hash == s.upload_hash && !s.known_tiles.empty() &&
         !s.gpu_content)) {
        s.opacity_tick = s.modification_tick;
    }
}

/// Makes the whole level-0 map from `bytes`, the guest bytes the surface holds now (the caller
/// checked they hash to its upload hash): for a surface whose map was lost or never made.
inline void RebuildOpacity(SurfaceBase& s, std::span<const u8> bytes) {
    const u32 tiles = OpacityTiles(s);
    const u32 tile_bytes = OpacityTileBytes(s.pixel_format);
    if (tiles == 0 || bytes.size() < static_cast<std::size_t>(tiles) * tile_bytes) {
        return;
    }
    const std::size_t words = (tiles + 63) / 64;
    s.known_tiles.assign(words, 0);
    s.opaque_tiles.assign(words, 0);
    for (u32 i = 0; i < tiles; i++) {
        const u64 bit = 1ull << (i % 64);
        s.known_tiles[i / 64] |= bit;
        if (TileOpaque(s.pixel_format, bytes.data() + static_cast<std::size_t>(i) * tile_bytes)) {
            s.opaque_tiles[i / 64] |= bit;
        }
    }
    s.opacity_pending = false;
    s.opacity_tick = s.modification_tick;
    s.opacity_hash = s.upload_hash;
}

/// Why RegionOpaque said no (coverlog).
enum class OpacityWhy : u32 {
    Opaque = 0,
    NoMap = 1,       ///< a format or surface type without a map
    Stale = 2,       ///< the surface changed since the map was made (or never had one)
    Misaligned = 3,  ///< the texture does not start on a tile of the surface
    OutOfRange = 4,  ///< a tile past the surface's level 0
    Unknown = 5,     ///< a tile no upload described
    Transparent = 6, ///< a tile with a texel below alpha 255
};

/**
 * Whether the texels [s0, s1] x [m0, m1] of a texture at `tex_addr`, `tex_width` texels wide,
 * all have alpha 255, by the map of `s` (the cached surface holding it, of the same format).
 * Rows `m` count in memory order (the PICA's t flipped: m = height - 1 - t). Anything unknown
 * is a no; `bad_tile` gets the tile that decided it.
 */
inline OpacityWhy RegionOpacity(const SurfaceBase& s, PAddr tex_addr, u32 tex_width, u32 s0,
                                u32 s1, u32 m0, u32 m1, u32* bad_tile = nullptr) {
    const u32 tiles = OpacityTiles(s);
    if (tiles == 0) {
        return OpacityWhy::NoMap;
    }
    if (s.opacity_tick != s.modification_tick || s.known_tiles.empty()) {
        return OpacityWhy::Stale;
    }
    const u32 tile_bytes = OpacityTileBytes(s.pixel_format);
    if (tex_addr < s.addr || (tex_addr - s.addr) % tile_bytes != 0) {
        return OpacityWhy::Misaligned;
    }
    const u32 base = (tex_addr - s.addr) / tile_bytes;
    const u32 row_tiles = tex_width / 8;
    for (u32 ty = m0 / 8; ty <= m1 / 8; ty++) {
        for (u32 tx = s0 / 8; tx <= s1 / 8; tx++) {
            const u32 i = base + ty * row_tiles + tx;
            if (bad_tile != nullptr) {
                *bad_tile = i;
            }
            if (i >= tiles) {
                return OpacityWhy::OutOfRange;
            }
            const u64 bit = 1ull << (i % 64);
            if ((s.known_tiles[i / 64] & bit) == 0) {
                return OpacityWhy::Unknown;
            }
            if ((s.opaque_tiles[i / 64] & bit) == 0) {
                return OpacityWhy::Transparent;
            }
        }
    }
    return OpacityWhy::Opaque;
}

/// coverlog: how many tiles of the window are known transparent, of all.
inline void RegionTileCounts(const SurfaceBase& s, PAddr tex_addr, u32 tex_width, u32 s0, u32 s1,
                             u32 m0, u32 m1, u32& transparent, u32& total) {
    transparent = total = 0;
    const u32 tiles = OpacityTiles(s);
    const u32 tile_bytes = OpacityTileBytes(s.pixel_format);
    if (tiles == 0 || s.known_tiles.empty() || tex_addr < s.addr ||
        (tex_addr - s.addr) % tile_bytes != 0) {
        return;
    }
    const u32 base = (tex_addr - s.addr) / tile_bytes;
    for (u32 ty = m0 / 8; ty <= m1 / 8; ty++) {
        for (u32 tx = s0 / 8; tx <= s1 / 8; tx++) {
            const u32 i = base + ty * (tex_width / 8) + tx;
            total++;
            if (i < tiles && (s.known_tiles[i / 64] >> (i % 64) & 1) != 0 &&
                (s.opaque_tiles[i / 64] >> (i % 64) & 1) == 0) {
                transparent++;
            }
        }
    }
}

inline bool RegionOpaque(const SurfaceBase& s, PAddr tex_addr, u32 tex_width, u32 s0, u32 s1,
                         u32 m0, u32 m1) {
    return RegionOpacity(s, tex_addr, tex_width, s0, s1, m0, m1) == OpacityWhy::Opaque;
}

} // namespace VideoCore
