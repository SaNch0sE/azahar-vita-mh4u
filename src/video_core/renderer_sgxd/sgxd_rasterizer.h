// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "video_core/pica/regs_texturing.h"
#include "video_core/rasterizer_accelerated.h"
#include "video_core/renderer_gxm/guest_gpu_memory.h"
#include "video_core/renderer_gxm/gxm_shader_tier.h"
#include "video_core/renderer_gxm/sgxd_backend.h"
#include "video_core/renderer_gxm/usse/gxp_writer.h"
#include "video_core/renderer_gxm/usse/vs_usse_gen.h"
#include "video_core/shader/generator/profile.h"

namespace Pica {
class PicaCore;
}

namespace Memory {
class MemorySystem;
}

namespace GxmRenderer {

/// The fixed palettes that expand a 4-bit format's index for the untile (P4 and P8).
enum class TexturePalette : u32 { None, A4, I4, IA4 };

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
 *
 * Immediate-mode vertices reach the GPU too: the PICA hands over each vertex's raw attributes
 * (AccelerateImmediateVertex) and the batch is drawn with the USSE vertex program at the next
 * register write that is not immediate data, or before any other operation.
 */
class RasterizerSgxd : public VideoCore::RasterizerAccelerated {
public:
    explicit RasterizerSgxd(Pica::PicaCore& pica, Memory::MemorySystem& memory,
                           GuestGpuMemory& guest_memory);
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
    /// The HLE wrote a texture page (RenderThread's GuestWrote, Y2R's picture): the textures
    /// over it are untiled again at their next use.
    void GuestWrote(PAddr page) {
        TexturesWritten(page, page + 0x1000);
    }
    bool AccelerateTextureCopy(const Pica::DisplayTransferConfig& config) override;
    bool AccelerateFill(const Pica::MemoryFillConfig& config) override;
    bool AccelerateDma(PAddr src, PAddr dst, u32 size) override;
    /// The GPU's MMU onto the pages a page move or return put under the CPU's views. Returns the fence
    /// the work queued with it is done by.
    u32 RemapMoved(const u32* pte_va, const u32* pte, u32 count);
    /// Before a page move: a DMA into cached memory still queued retires first.
    void MoveWaits();
    [[nodiscard]] bool MovedFenceReached(u32 fence) const {
        return backend.RenderReached(fence);
    }
    void MovedFenceWait(u32 fence) {
        backend.RenderWait(fence, "a moved page's pool page");
    }
    bool AccelerateDrawBatch(bool is_indexed) override;
    /// The next draw's indices as the guest had them at the draw (a geometry draw's).
    void GsIndices(const u8* indices, bool index_u16) {
        const u32 bytes = regs.pipeline.num_vertices * (index_u16 ? 2u : 1u);
        gs_indices.assign(indices, indices + bytes);
        gs_indices_set = true;
    }
    bool AcceleratesGeometryShaders() const override {
        return true;
    }
    bool AccelerateImmediateVertex(const Pica::AttributeBuffer& input) override;
    void FlushImmediate() override;
    void NotifyP3D() override;
    void GpuIdle() override;
    u64 GpuWaitUs() const override;
#endif
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

    /// A guest texture's host copy, untiled on the GPU into A8B8G8R8, rows in the PICA's order (row 0
    /// is t = 1): a LINEAR chain of the levels, each level's rows its width apart.
    struct Texture {
        PAddr addr;
        u32 width;
        u32 height;
        Pica::TexturingRegs::TextureFormat format;
        SgxdBackend::Data memory;
        u32 bytes;        // of memory
        u32 levels;       // the chain's, 1 to the PICA's max_level + 1
        u32 parts;        // 1, or a cube's 6 faces (SWIZZLED squares, one level)
        std::array<PAddr, 6> part_addr; // the guest's, in the host copy's order
        u32 part_bytes;   // a part's guest bytes: the levels back to back, or a face
        u32 untiled_pass; // the pass that last untiled it (cached FCRAM: each that samples it)
        bool dirty;       // a fill, DMA or transfer wrote over the guest bytes
        /// A non-power-of-two 2D texture's rows this many texels apart (a multiple of 32); 0 for a
        /// power-of-two chain.
        u32 stride = 0;
        /// A byte copy's U8 swizzle: 6 for A8 and A4 (alpha), 5 for I8 and I4 (intensity); 0 for
        /// A8B8G8R8.
        u8 byte_swizzle = 0;
#ifdef VITA_DIAGNOSTICS
#endif
    };

    /// A vertex program placed in the code heap, for one PICA program and draw shape.
    struct VertexProgram {
        Usse::GxpProgram gxp;
        u32 input_regs;
        u32 primary_doutu0;
        u32 secondary_doutu0;
        u32 draws;         ///< draws with the first tier's program, up to Tier2Draws
        bool tier2_queued; ///< sent to the second tier (or never to be)
    };

    /// Where a fragment uniform's value is in FSUniformData: `count` elements of `components` values,
    /// `stride` bytes apart, into the SA block at `sa` (elements four floats apart).
    struct UniformSource {
        u32 sa;
        u16 offset;
        u8 components;
        u8 count;
        u8 stride;
        bool is_int;
    };

    /// A fragment program with its blend, placed, and its pixel primary PDS (which holds no
    /// address but the code's, so one serves every draw).
    struct FragmentProgram {
        std::vector<UniformSource> uniforms;
        Usse::GxpProgram gxp;
        u32 primary_doutu0;
        u32 secondary_doutu0;
        u32 fprim;
        u32 fprim_units;
        u32 pixel_pa;
        u64 blend;         ///< the blend word its code ends with
        u32 draws;         ///< draws with the first tier's program, up to Tier2Draws
        bool tier2_queued; ///< sent to the second tier (or never to be)
    };
    /// Draws after which a program is emitted again with the optimising passes.
    static constexpr u32 Tier2Draws = 100;

    void RecordClear(const ClearedRange& range);
    void ForgetClears(PAddr start, PAddr end);
    bool TakeClear(PAddr start, PAddr end, ClearedRange& out);
    Surface& SurfaceFor(PAddr addr, u32 width, u32 height);
    void EndPass(const char* why);
    /// A draw of the loaders' vertices, or of the immediate-mode batch (`count` vertices of
    /// `immediate_attributes` x 4 floats).
    bool Draw(bool is_indexed, const float* immediate, u32 count);

    /// A draw's description for its assert messages, formatted only when one fires.
    class Describe {
    public:
        template <typename F>
        Describe(const F& f)
            : ctx{&f}, fn{[](const void* c) { return (*static_cast<const F*>(c))(); }} {}
        std::string operator()() const {
            return fn(ctx);
        }

    private:
        const void* ctx;
        std::string (*fn)(const void*);
    };

    struct VertexStage {
        const VertexProgram* vp{};
        std::array<SgxdVertexFetch, SGXD_PROGRAM_MAX_FETCH> fetch{};
        u32 fetch_count{};
        u32 indices{};    // a 16-bit index list's GPU address, 0 for the identity list
        u32 count{};      // the indices the draw takes
        u32 vertex_min{}; // the PICA vertices it reads
        u32 vertex_max{};
        const Pica::ShaderSetup* setup{}; // the unit whose uniforms the program reads
    };
    /// The loaders' attributes into the PICA's vertex program. With StoreOutputs the fetch covers
    /// vertex_min to vertex_max densely and the caller adds the stores and the index list.
    VertexStage VertexStageFor(bool is_indexed, const float* immediate, u32 count,
                               Usse::VsMode mode, const Describe& describe);
    /// A draw with a geometry shader: the vertex program's outputs and the geometry program's triangles
    /// through two stream-out renders, then the triangles as the draw's vertices (the SGX has no
    /// geometry stage, so the geometry program runs as a vertex program that stores what it emits).
    VertexStage GeometryStage(bool is_indexed, const Describe& describe);
    /// The vertex program's SA block: its uniforms from `setup`, its literals and buffer
    /// pointer; `vsec_sa` its words.
    SgxdBackend::Data VertexSa(const Usse::GxpProgram& vg, const Pica::ShaderSetup& setup,
                               u32& vsec_sa, const Describe& describe);
    /// The vertex side of a draw's programs: the fetch and secondary PDS programs, the VDM words, the
    /// output selects.
    void VertexPrograms(const VertexStage& stage, const SgxdBackend::Data& vsa, u32 vsec_sa,
                        SgxdRecipeDrawPrograms& p, const Describe& describe);

    /// `u8_regs`: the input registers an UNSIGNED_BYTE loader feeds.
    const VertexProgram& VertexProgramFor(u16 default_regs,
                                          const std::array<u8, 16>& reg_components, u16 u8_regs,
                                          Usse::VsMode mode = Usse::VsMode::Draw);
    VertexProgram PlaceVertexProgram(Usse::GxpProgram gxp, const char* what, u64 key);
    /// The geometry program for `rows` input rows (uniform: from c<first_uniform>) and at most
    /// `max_triangles` triangles an invocation.
    const VertexProgram& GeometryProgramFor(u32 rows, bool rows_uniform, bool rows_table,
                                            u32 first_uniform, u32 max_triangles,
                                            const Describe& describe);
    /// The vertex program that draws a geometry program's stored triangles: input k is output k.
    const VertexProgram& PassthroughProgramFor(u32 outputs);
    const FragmentProgram& FragmentProgramFor(u64 blend);
    /// The program placed for sgxd: its code (with the blend's tail), secondary and PDS.
    FragmentProgram PlaceFragmentProgram(Usse::GxpProgram gxp, u64 blend, u64 hash);
    void InstallTier2();
    void InstallVertexTier2(ShaderTier2::Result& result);
    /// The host copy of texture unit `unit`'s texture for the open pass, untiled when it is
    /// new or the guest's bytes changed: cached FCRAM by its hash, compared once a pass;
    /// uncached memory (VRAM, FCRAM moved to CDRAM) when a GPU operation wrote over it.
    const Texture& TextureFor(u32 unit);
    /// The GPU address of a fixed palette for a 4-bit format (0 for none), written at the first use.
    u32 PaletteFor(TexturePalette palette);
    /// The sampler's four state words for unit `unit`'s texture.
    void TextureWords(u32 unit, u32 words[4]);
    /// Marks the textures over [start, end) to be untiled again.
    void TexturesWritten(PAddr start, PAddr end);
    /// The lighting and fog LUTs the guest changed into fresh rows, fs_data's offsets pointed at them.
    void SyncLutLf();
    /// The same for the proctex colour map, alpha map, colour and colour-difference tables.
    void SyncLutProcTex();

    GuestGpuMemory& guest_memory;
    /// [addr, addr + size) in FCRAM moves to CDRAM where it is not yet, for the work about to use it:
    /// its page holds keep the guest off it through the copy.
    void KeepUncached(PAddr addr, u32 size);
    SgxdBackend backend;
    /// The pending clears, disjoint. A fill into a full table replaces the oldest.
    static constexpr u32 MaxClears = 8;
    std::array<ClearedRange, MaxClears> cleared{};
    u32 clear_count{};
    u32 clear_order{};
    std::vector<Surface> surfaces;
    std::vector<Texture> textures;
    SgxdBackend::Data palettes{};
    std::unordered_map<u64, VertexProgram> vertex_programs;
    std::unordered_map<u64, u16> used_bools;
    std::unordered_map<u64, FragmentProgram> fragment_programs;
    std::vector<u8> gs_indices;
    bool gs_indices_set = false;
    std::unique_ptr<ShaderTier2> tier2;

    // Immediate-mode vertices not drawn yet, four floats an attribute. One ImmDraw block is one draw.
    std::vector<float> immediate_vertices;
    u32 immediate_attributes{};

    // The LUT rows: a ring, each row stamped with the pass that wrote it (a pass samples at its end,
    // so a row is never rewritten in its own pass).
    u32 lut_row{};
    std::array<u32, Pica::Shader::LUT_LF_ROWS> lut_row_pass{};
    bool lut_rows_ready{};
    // The proctex rows (LutRgRows, LutRgbaRows), two rings kept the same way.
    u32 lut_rg_row{};
    u32 lut_rgba_row{};
    std::array<u32, Pica::Shader::LUT_RG_ROWS> lut_rg_row_pass{};
    std::array<u32, Pica::Shader::LUT_RGBA_ROWS> lut_rgba_row_pass{};
    /// The LUT rings' halves in use, and each half's pass mark when it was left.
    u32 lut_half{}, proctex_half{};
    std::array<u32, 2> lut_half_mark{}, proctex_half_mark{};

    std::size_t pass_surface{}; // into surfaces, while the backend is in a pass
    /// The rows KeepGsRows copied out of a room a pass took, a 16-byte row a register.
    SgxdBackend::Data gs_kept_rows{};
    u16 gs_rows_kept = 0;
    /// The geometry draw's outputs, triangles and tables are past a pass's data and take memory of
    /// their own, given back as the pass is sent.
    bool gs_own_data = false;
    u16 gs_rows_own = 0;
    /// SgxdBackend::SetRoomTaken: each GS input register's last row in [begin, end) copied out.
    void KeepGsRows(u32 begin, u32 end);
    PAddr pass_depth{};
    bool pass_depth_cleared{};
    u32 pass_depth_bytes{4}; // 2, 3 or 4, the depth format's

    u32 fills{};
    u32 dmas{};
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
    u32 untiles{};
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
        u32 untiles{};        ///< textures untiled: new, written by a GPU op, or cached FCRAM
        u64 untiled_bytes{};  ///< their guest bytes
        u32 null_samples{};   ///< samplers of a unit that is off (NullTexture)
        u32 tiny{};           ///< untiles of a texture with a side under 8 (UntileTiny)
        u64 tiny_texels{};    ///< ... their texels
        u64 tiny_us{};        ///< ... the render thread's time in them
        u32 tiny_logged{};    ///< the first ones logged (carried across lines)
        u64 start_us{};
        u32 passes_mark{};    ///< passes at the second's start: the textures draws took since
        u64 evictions_mark{}; ///< evictions at the second's start
        std::vector<std::pair<const char*, u32>> pass_ends;
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
