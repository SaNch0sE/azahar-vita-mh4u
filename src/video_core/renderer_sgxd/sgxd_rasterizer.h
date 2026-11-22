// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "video_core/rasterizer_accelerated.h"
#include "video_core/renderer_gxm/sgxd_backend.h"
#include "video_core/renderer_gxm/usse/gxp_writer.h"

namespace Pica {
class PicaCore;
}

namespace Memory {
class MemorySystem;
}

namespace GxmRenderer {

/**
 * The PICA on the SGX through sgxd. There is no CPU fallback: every operation either runs on
 * the GPU or stops the emulator with a message naming what is unsupported, so the Accelerate*
 * hooks return true or crash, and the CPU vertex path, which ends in AddTriangle, always
 * crashes.
 *
 * Draws render into host surfaces (linear A8B8G8R8, SgxdBackend's second block) in passes: a
 * pass is one colour target from its first draw until P3D (the draws' vertices are the guest's
 * again after it), something needs the picture (the display transfer) or another target is
 * drawn to. A pass starts from what a memory fill left pending on
 * its colour and depth buffers; loading a buffer's earlier picture is not built.
 */
class RasterizerSgxd : public VideoCore::RasterizerAccelerated {
public:
    explicit RasterizerGxm(Pica::PicaCore& pica, Memory::MemorySystem& memory);
    ~RasterizerGxm() override;

    void AddTriangle(const Pica::OutputVertex& v0, const Pica::OutputVertex& v1,
                     const Pica::OutputVertex& v2) override;
    void DrawTriangles() override;
    void FlushAll() override;
    void FlushRegion(PAddr addr, u32 size) override;
    void InvalidateRegion(PAddr addr, u32 size) override;
    void FlushAndInvalidateRegion(PAddr addr, u32 size) override;
    void ClearAll(bool flush) override;
    bool AccelerateDisplayTransfer(const Pica::DisplayTransferConfig& config) override;
    bool AccelerateTextureCopy(const Pica::DisplayTransferConfig& config) override;
    bool AccelerateFill(const Pica::MemoryFillConfig& config) override;
    bool AccelerateDrawBatch(bool is_indexed) override;
    void NotifyP3D() override;
    std::vector<std::pair<PAddr, u32>> TakeCachedReads() override;

private:
    /// A vertex or index range a draw cleaned: its pages move to CDRAM at vblank (TakeCachedReads).
    void CachedRead(PAddr addr, u32 size);
    std::mutex cached_reads_mutex;
    std::vector<std::pair<PAddr, u32>> cached_reads;

    /// A range a fill wrote, with its value: a pass over it starts from the clear value with nothing
    /// loaded. Only the newest fill of a range counts, and a pass takes the clear it starts from.
    struct ClearedRange {
        PAddr start;
        PAddr end;
        u32 value;
        u32 bytes; // 2, 3 or 4, the fill's width
        u32 order; // when it was recorded, for replacing the oldest
    };

    /// A colour buffer's host surface.
    struct Surface {
        PAddr addr;
        u32 width;
        u32 height;
        SgxdBackend::Data memory;
        u32 passes;
    };

    /// A vertex program placed in the code heap, for one PICA program and draw shape.
    struct VertexProgram {
        Usse::GxpProgram gxp;
        u32 input_regs;
        u32 primary_doutu0;
        u32 secondary_doutu0;
    };

    /// A fragment program with its blend, placed, and its pixel primary PDS (which holds no
    /// address but the code's, so one serves every draw).
    struct FragmentProgram {
        Usse::GxpProgram gxp;
        u32 primary_doutu0;
        u32 secondary_doutu0;
        u32 fprim;
        u32 fprim_units;
        u32 pixel_pa;
    };

    void RecordClear(const ClearedRange& range);
    void ForgetClears(PAddr start, PAddr end);
    bool TakeClear(PAddr start, PAddr end, ClearedRange& out);
    Surface& SurfaceFor(PAddr addr, u32 width, u32 height);
    void EndPass(const char* why);

    /// `u8_regs`: the input registers an UNSIGNED_BYTE loader feeds.
    const VertexProgram& VertexProgramFor(u16 default_regs,
                                          const std::array<u8, 16>& reg_components, u16 u8_regs);
    const FragmentProgram& FragmentProgramFor(u64 blend);
    /// The same for the proctex colour map, alpha map, colour and colour-difference tables.
    void SyncLutProcTex();

    SgxdBackend backend;
    /// The pending clears, disjoint. A fill into a full table replaces the oldest.
    static constexpr u32 MaxClears = 8;
    std::array<ClearedRange, MaxClears> cleared{};
    u32 clear_count{};
    u32 clear_order{};
    std::vector<Surface> surfaces;
    std::unordered_map<u64, VertexProgram> vertex_programs;
    std::unordered_map<u64, u16> used_bools;
    std::unordered_map<u64, FragmentProgram> fragment_programs;

    std::size_t pass_surface{}; // into surfaces, while the backend is in a pass
    PAddr pass_depth{};
    bool pass_depth_cleared{};

    u32 fills{};
    u32 draws{};
    u32 gs_draws{};
    u32 passes{};
#ifdef VITA_DIAGNOSTICS
    struct StreamOutNote {
        const char* stage = nullptr; // "A0" (vertex outputs), "A1" (geometry)
        u32 pass, draw, mode, invocations, rows, cap, count, doutu, temps;
    };
    static StreamOutNote stream_note;
    static RasterizerSgxd* stream_self;
    static void PrintStreamOut(); ///< sgxd_dump.cpp
    void NoteStreamOut(const char* stage, u32 mode, u32 invocations, u32 rows, u32 cap, u32 count,
                       const VertexProgram& program);
#endif
#ifdef VITA_DIAGNOSTICS
    u32 transfers{}; ///< perflog's progress lines and vprof's transfer numbers
#endif
    u32 p3ds{};
#ifdef VITA_DIAGNOSTICS
    /// perflog's `perf: textures` line (LogTextureStats), and the 8-bit index lists widened since the
    /// last line, their bytes by where the CPU read them, and the cycles.
    struct WidenStats {
        u32 lists{};
        std::array<u64, 3> bytes{};
        u64 cycles{};
    } widen_stats;
    struct TextureStats {
        u32 checks{};         ///< hashes of a cached FCRAM texture's guest bytes
        u32 changed{};        ///< checks of a known texture that found new bytes
        u64 hashed_bytes{};
        u64 hash_us{};
        u32 untiles{};        ///< textures untiled: new, written by a GPU op, or changed
        u64 untiled_bytes{};  ///< their guest bytes
        u32 null_samples{};   ///< samplers of a unit that is off (NullTexture)
        u32 tiny{};           ///< untiles of a texture with a side under 8 (UntileTiny)
        u64 tiny_texels{};    ///< ... their texels
        u64 tiny_us{};        ///< ... the render thread's time in them
        u32 tiny_logged{};    ///< the first ones logged (carried across lines)
        u64 start_us{};
        u32 passes_mark{};    ///< passes at the second's start: the textures draws took since
        u64 evictions_mark{}; ///< evictions at the second's start
    } texture_stats;
    /// gxm_flags perflog + bankstats: the `perf: banks` and `perf: LUT rows` lines, with the
    /// textures' (NoteBankBuild, NoteLutTable; the draw and lookup counts are kept always).
    struct BankLutStats {
        u32 draws{};
        u32 bank_lookups{};   ///< BankBuffer calls for a program with a uniform bank
        u32 bank_builds{};    ///< ... that built a new bank
        u32 zero_builds{};    ///< buffers built for a program without one (zeros)
        u64 bank_rows{};      ///< the rows the builds copied
        /// Rows changed since the bank built 1, 8 and 32 builds before: what a ring of that
        /// many banks, each patched with its changed rows, would copy (one history for the VS
        /// and GS setups: their versions mix if both have banks).
        std::array<u64, 3> changed_since{};
        std::array<u32, 32> build_versions{}; ///< the f_version of the last builds, a ring
        u32 build_count{};
        u32 lut_tables{};      ///< lighting tables converted
        u32 lut_ring_moves{};  ///< ... of them because the ring moved to its other half
        u64 lut_entries_changed{}; ///< entries differing from the table's previous contents
        u32 lut_seen_64{}, lut_seen_256{}; ///< contents converted within the last 64 / 256
        u32 fog_tables{};
        std::vector<std::array<u32, 256>> lut_shadow; ///< the 24 tables as last converted
        std::array<u64, 256> lut_hashes{};             ///< the last conversions' contents
        u32 lut_hash_count{};
    } bank_lut_stats;
    void LogTextureStats();
    /// perflog: draws with the PICA's user clip plane on (sgxd gives the TA none).
    u32 clip_plane_draws{}, clip_plane_all{};
    std::array<float, 4> clip_plane_last{};
#endif
};

} // namespace GxmRenderer
