// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <vector>
#include <boost/icl/interval_set.hpp>
#include "video_core/rasterizer_cache/surface_params.h"
#include "video_core/rasterizer_cache/utils.h"

namespace VideoCore {

using SurfaceRegions = boost::icl::interval_set<PAddr, std::less, SurfaceInterval>;

struct Material;

enum class SurfaceFlagBits : u32 {
    Registered = 1 << 0,   ///< Surface is registed in the rasterizer cache.
    Picked = 1 << 1,       ///< Surface has been picked when searching for a match.
    Tracked = 1 << 2,      ///< Surface is part of a texture cube and should be tracked.
    Custom = 1 << 3,       ///< Surface texture has been replaced with a custom texture.
    ShadowSource = 1 << 4, ///< Surface is used as a shadow source.
    RenderTarget = 1 << 5, ///< Surface was a render target.
    /// The guest flushed its CPU cache over the whole surface: its pixels may have changed,
    /// and a hash of guest memory decides at the next use whether they did.
    GuestFlushed = 1 << 6,
};
DECLARE_ENUM_FLAG_OPERATORS(SurfaceFlagBits);

class SurfaceBase : public SurfaceParams {
public:
    /// Readbacks answered from what the last one left in guest memory rather than by draining
    /// the GPU, and whether a real one has ever run. See RasterizerCache::DownloadSurface.
    u8 download_skips{};
    bool ever_downloaded{};

public:
    SurfaceBase(const SurfaceParams& params, const SurfaceFlagBits& initial_flag_bits);
    ~SurfaceBase();

    /// Returns true when this surface can be used to fill the fill_interval of dest_surface
    bool CanFill(const SurfaceParams& dest_surface, SurfaceInterval fill_interval) const;

    /// Returns true when surface can validate copy_interval of dest_surface
    bool CanCopy(const SurfaceParams& dest_surface, SurfaceInterval copy_interval) const;

    /// Returns the region of the biggest valid rectange within interval
    SurfaceInterval GetCopyableInterval(const SurfaceParams& params) const;

    /// Returns the clear value used to validate another surface from this fill surface
    ClearValue MakeClearValue(PAddr copy_addr, PixelFormat dst_format);

    /// Returns the internal surface extent.
    Extent RealExtent(bool scaled = true) const;

    /// Returns true if the surface contains a custom material with a normal map.
    bool HasNormalMap() const noexcept;

    bool Overlaps(PAddr overlap_addr, std::size_t overlap_size) const noexcept {
        const PAddr overlap_end = overlap_addr + static_cast<PAddr>(overlap_size);
        return addr < overlap_end && overlap_addr < end;
    }

    u64 ModificationTick() const noexcept {
        return modification_tick;
    }

    bool IsCustom() const noexcept {
        return True(flags & SurfaceFlagBits::Custom) && custom_format != CustomPixelFormat::Invalid;
    }

    bool IsRegionValid(SurfaceInterval interval) const {
        return invalid_regions.find(interval) == invalid_regions.end();
    }

    void MarkValid(SurfaceInterval interval) {
        invalid_regions.erase(interval);
        modification_tick++;
    }

    void MarkInvalid(SurfaceInterval interval) {
        invalid_regions.insert(interval);
        modification_tick++;
    }

    bool IsFullyInvalid() const {
        auto interval = GetInterval();
        return *invalid_regions.equal_range(interval).first == interval;
    }

private:
    /// Returns the fill buffer value starting from copy_addr
    std::array<u8, 4> MakeFillBuffer(PAddr copy_addr);

public:
    SurfaceFlagBits flags{};
    const Material* material = nullptr;
    SurfaceRegions invalid_regions;
    u32 fill_size = 0;
    std::array<u8, 4> fill_data{};
    u64 modification_tick = 1;
    /// Runtime resource tick of the last frame that validated (so used) this surface; the
    /// least recently used surfaces are what a trim gives up first.
    u64 last_use_tick = 0;
    /// Resource tick of the last guest-flush hash check (see InvalidateRegionUnlessDirty).
    u64 last_hash_tick = 0;
    /// Hash of the guest bytes the whole surface was last uploaded from, 0 when it never was
    /// uploaded whole. A guest cache flush over the surface compares against it instead of
    /// decoding and uploading again: titles flush texture memory every frame whether or not
    /// they wrote it.
    u64 upload_hash = 0;
    /// The same bytes hashed per 4 KiB guest page (page i covers the surface's part of page
    /// (addr >> 12) + i), valid only while `page_hashes_of == upload_hash`: whatever replaces
    /// or clears the whole hash leaves these stale by construction. A guest flush that touched
    /// two pages of a 64 KiB texture then hashes those two pages, not the texture.
    std::vector<u64> page_hashes;
    u64 page_hashes_of = 0;

    /// MH4U cover proof (tile_opacity.h): per 8x8 tile of level 0, in guest memory order,
    /// whether every texel has alpha 255 (`opaque_tiles`) and whether that is known
    /// (`known_tiles`) - set from the guest bytes of each upload. Valid only while
    /// `opacity_tick == modification_tick`: anything else that changes the pixels (a copy
    /// from another surface, a fill, a render) moves the tick and leaves every tile unknown.
    std::vector<u64> opaque_tiles;
    std::vector<u64> known_tiles;
    u64 opacity_tick = 0;
    bool opacity_pending = false; ///< an upload set tiles; the validation's tick follows
    /// The whole-surface upload hash the valid map describes (0: none). A guest write that the
    /// hash check later finds changed nothing moves the tick twice (invalid, then valid): the
    /// map stays good when the bytes hash the same as the ones it was made from.
    u64 opacity_hash = 0;
    u64 opacity_rebuilt_tick = 0; ///< the tick a lazy rebuild from guest memory was tried at
    /// Some of the pixels came from the GPU (a draw, blit or fill into it, or a copy from
    /// another surface) rather than from guest memory; cleared when the whole surface is
    /// uploaded again. A surface the CPU writes and the cache uploads (a page of text, say)
    /// keeps it false whatever its type.
    bool gpu_content = false;
    u32 upload_count = 0; ///< uploads into it (coverlog)

    /// Records the whole hash of `bytes` (the surface's own guest bytes, `size` long) and its
    /// per-page hashes.
    void SetUploadHash(u64 whole, const u8* bytes);
};

} // namespace VideoCore
