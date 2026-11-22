// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <list>
#include <map>
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
#include "video_core/renderer_gxm/usse/usse_program.h"
#include "video_core/renderer_gxm/usse/vs_usse_gen.h"
#include "video_core/shader/generator/profile.h"

namespace Pica {
class ShaderEngine;
}

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
 * Draws render in passes: a pass is one colour buffer from its first draw until P3D (the draws'
 * vertices are the guest's again after it), something reads the buffer (a transfer, a DMA, a
 * texture) or another buffer is drawn to. A buffer a render takes whole (ScalesWhole) is drawn
 * into a picture of its own (Upscaled), at the internal resolution (TopScale, 1x by default) and
 * in the guest's format (PictureFormatFor):
 * its colour stays there until something else reads or writes the buffer's bytes, which have it
 * copied into guest memory in the PICA's tiling first (WriteColourBack); a display transfer reads
 * the picture itself, and a texture over it is copied from it. Any other buffer (narrower than
 * 32, or of a size no render takes) is drawn straight into guest memory, in the guest's format
 * and the PICA's tiling (SgxdBackend::PicaTarget, route 1). A pass starts from what a memory fill
 * left in its buffers (the clear), from nothing when its first draw covers the target, or else
 * from its picture or the buffer's own bytes, loaded by its first draws.
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
    /// A guest frame has been presented (RendererGxm::PresentFrame, each VBlank, on the render
    /// thread): the next draws start a new frame, whose first use of an FCRAM texture untiles
    /// it again (TextureFor).
    void EndFrame() {
        frames++;
#ifdef VITA_DIAGNOSTICS
        ProbeEotMix();
#endif
    }
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
    /// The next draw's 16-bit indices span lo..hi, as the emulation side scanned them.
    void ScannedIndices(u32 lo, u32 hi) {
        scanned_indices = {lo, hi};
    }
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
#ifdef VITA_DIAGNOSTICS
    /// vprof's replay: the frame's draws counted from here, those after `limit` left out (-1: none).
    void ReplayBegin(int limit) {
        replay_limit = limit;
        replay_draw_index = 0;
    }
    void ReplayStop() {
        replay_limit = -2;
    }
    [[nodiscard]] u32 ReplayDraws() const {
        return replay_draw_index;
    }
    void ReplayEndPass();
    /// The colour buffer at `addr` as a pass last drew it, as A8B8G8R8 rows; false when none did.
    bool ReplayReadSurface(PAddr addr, std::vector<u8>& out, u32& width, u32& height);
    SgxdBackend& Backend() {
        return backend;
    }
#endif
#ifdef VITA_DIAGNOSTICS
    /// vprof: what a pass can start from as the recorded frame began: the pending clears and the depth
    /// stores (pointers into the capture).
    struct VprofState {
        struct Clear {
            PAddr start, end;
            u32 value, bytes, order;
        };
        struct Colour {
            PAddr addr;
            u32 width, height, format, bpp, passes, generation;
            bool synced, guest_newer;
            const u8* pixels; // width x height A8B8G8R8
        };
        struct Depth {
            PAddr addr;
            u32 width, height, format;
            bool valid;
            const u8* samples; // the store's bytes (NewDepthStore's layout)
            u32 bytes;
        };
        u32 clear_order{};
        std::vector<Clear> clears;
        u32 generations{};
        std::vector<Colour> surfaces;
        std::vector<Depth> depths;
    };
    /// vprof: the recorded frame starts: the open pass ended, the GPU idle, VprofState recorded.
    void VprofFrameStart();
    /// vprof's replay: `state` in place of the depth stores and clears before each run, and with
    /// `retile` every texture untiled again.
    void ReplayRestore(const VprofState& state, bool retile);
    /// vprof's replay wrote guest memory: the textures there are untiled again.
    void ReplayMemoryWritten(PAddr start, PAddr end) {
        TexturesWritten(start, end);
    }
#endif
    void GpuIdle() override;
    u64 InterruptFence() override;
    bool InterruptPoll(u64 fence) override;
    void InterruptWait(u64 fence) override;
    void InterruptKick(u64 fence);
    /// The render thread's sleep until the fence's work (SgxdBackend::InterruptArm).
    [[nodiscard]] bool InterruptArm(u64 fence);
    void InterruptDisarm();
    void InterruptWaker(s32 flag, u32 bits);
#ifdef VITA_DIAGNOSTICS
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

    struct ColourBuffer {
        PAddr addr{};
        u32 width{};
        u32 height{};
        Pica::FramebufferRegs::ColorFormat format = Pica::FramebufferRegs::ColorFormat::RGBA8;
        u32 bpp = 4; ///< the guest's bytes a pixel
        [[nodiscard]] PAddr End() const {
            return addr + width * height * bpp;
        }
    };

    /// A guest texture's host copy, untiled on the GPU into A8B8G8R8, rows in the PICA's order (row 0
    /// is t = 1): a LINEAR chain of the levels, each level's rows its width apart.
    struct Texture {
        PAddr addr;
        u32 width;
        u32 height;
        Pica::TexturingRegs::TextureFormat format;
        SgxdBackend::Data memory;
        u32 bytes;        // of memory (0 before a queued untile gives it one)
        u32 levels;       // the chain's, 1 to the PICA's max_level + 1
        u32 parts;        // 1, or a cube's 6 faces (SWIZZLED squares, one level)
        std::array<PAddr, 6> part_addr; // the guest's, in the host copy's order
        u32 part_bytes;   // a part's guest bytes: the levels back to back, or a face
        bool dirty;       // a fill, DMA or transfer wrote over the guest bytes
        bool alpha_one = false; // a surface without alpha, read as a texture (swizzle bit 2)
        u32 last_used = 0;      // the pass count when a draw last took it (eviction order)
        SgxdBackend::Data alpha{}; // a split ETC1A4's A8 chain (alpha_bytes 0: none)
        u32 alpha_bytes = 0;
        Texture* bucket_next = nullptr; // the next texture of its bucket (texture_buckets)
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
        Usse::Program usse;
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

    /// What follows a fragment program's own code: the pack to the output, or the pack and the blend.
    struct Epilogue {
        u64 blend = 0;      ///< the blend SOP2, 0 for none
        /// The bytes of o0 written (bit 0 R): other than 0xf with a blend only for R, G and B masked apart.
        u8 mask = 0xf;
        bool constant = false; ///< the blend's source is `colour` (LIMM into pa0), not the fragment's
        u32 colour = 0;     ///< the PICA's blend constant, R in the low byte
        /// The split blend's alpha word (BlendSplitWords), 0 for none: blend is then its colour SOP3.
        u64 split_alpha = 0;
        u64 Key() const {
            return (blend ^ split_alpha * 0xc2b2ae3d27d4eb4full) ^
                   (u64{mask} << 8 | u64{constant} << 12 | u64{colour} << 16) *
                       0x9e3779b97f4a7c15ull;
        }
    };
    struct FragmentProgram {
        std::vector<UniformSource> uniforms;
        Usse::Program usse;
        u32 primary_doutu0;
        u32 secondary_doutu0;
        u32 fprim;
        u32 fprim_units;
        u32 pixel_pa;
        Epilogue epilogue; ///< what its code ends with
        u32 draws;         ///< draws with the first tier's program, up to Tier2Draws
        bool tier2_queued; ///< sent to the second tier (or never to be)
        mutable u32 pixel_size = 0;
        mutable u32 pixel_size_sa = ~0u;
    };
    /// Draws after which a program is emitted again with the optimising passes.
    static constexpr u32 Tier2Draws = 100;

    void RecordClear(const ClearedRange& range);
    void ForgetClears(PAddr start, PAddr end);
    bool TakeClear(PAddr start, PAddr end, ClearedRange& out);
    /// The PBE's form of a colour buffer (route 1 takes widths of 32 to 1024 in 16s and heights of 8
    /// to 1024 in 8s).
    SgxdBackend::PicaTarget PicaTargetFor(const ColourBuffer& c, const char* what);
    /// Whether [start, end) overlaps the open pass's colour buffer, or its depth buffer when a
    /// draw uses it.
    bool PassOverlaps(PAddr start, PAddr end) const;
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
        bool fresh{}; // a fetch reads pass data allocated for the draw (SgxdBackend::VertexPds)
    };
    /// The loaders' attributes into the PICA's vertex program. With StoreOutputs the fetch covers
    /// vertex_min to vertex_max densely and the caller adds the stores and the index list.
    VertexStage VertexStageFor(bool is_indexed, const float* immediate, u32 count,
                               Usse::VsMode mode, const Describe& describe);
    /// A draw with a geometry shader: the vertex program's outputs and the geometry program's triangles
    /// through two stream-out renders, then the triangles as the draw's vertices (the SGX has no
    /// geometry stage, so the geometry program runs as a vertex program that stores what it emits).
    VertexStage GeometryStage(bool is_indexed, const Describe& describe);
    /// Whether a draw writes every pixel of a width x height target whatever was there: two
    /// triangles (4 or 6 vertices, positions from upstream's interpreter) covering it whole,
    /// neither culled nor clipped, and an output merger that takes nothing from the target
    /// (full colour mask, no blend, alpha, depth or stencil test, no scissor inside it). A
    /// pass it opens starts from nothing: no load, and no write-backs for one.
    /// `lines` lines of `line` bytes, src_stride apart, to dst_stride apart through the PTLA.
    bool CopyLines(PAddr src, u32 src_stride, PAddr dst, u32 dst_stride, u32 line, u32 lines);
    /// `depth`: whether the draw writes every pixel's depth instead, whatever it does to the
    /// colour, its depth test ALWAYS or off.
    bool CoversTarget(bool is_indexed, const float* immediate, u32 count, u32 width, u32 height,
                      bool depth = false);
    /// A draw of `count` vertices (4 or 6) run through the vertex program on the CPU; false when
    /// its index list is not in memory. Once a draw.
    bool QuadVertices(bool is_indexed, const float* immediate, u32 count,
                      std::array<Pica::OutputVertex, 6>& out);
    bool RunQuadVertices(bool is_indexed, const float* immediate, u32 count,
                         std::array<Pica::OutputVertex, 6>& out);
#ifdef VITA_DIAGNOSTICS
    /// sgxd_flags dumptex: each level's guest bytes and host copy to host0, named by its guest address
    /// (sgxd_dump.cpp). `bits`: the guest format's bits a texel.
    void DumpTexture(const Texture& tex, u32 bits, bool split);
    /// sgxd_flags `picdump=N`: after each of the first two display transfers into a screen picture
    /// from N s on, every picture, screen picture and their guest bytes to host0 (sgxd_dump.cpp).
    void DumpPictures();
    /// sgxd_flags `texdump=N`: each texture a draw takes during the frame, as the GPU wrote it, to host0
    /// (sgxd_dump.cpp).
    void DumpFrameTexture(u32 unit, const Texture& tex);
    std::vector<u64> tex_dumped;
    u32 tex_dump_transfers = 0;
    bool tex_dumping = false;
    /// sgxd_flags wbufmix: a Z-buffered draw that tests depth against the TA's W depths (a
    /// W-buffered draw of the pass wrote them, or the pass loaded a store holding them) is
    /// logged with its PICA depth range: there the PICA compared W depths, sgxd 1 - 1/w.
    /// `w_ta`: this draw is W-buffered through WBUFFEN.
    void LogWbufMix(bool w_ta, bool dwrite, u32 dcmp, bool is_indexed, const float* immediate,
                    u32 count);
    bool pass_ta_w{}; ///< the pass's depth holds the TA's W depths (wbufmix)
#endif
    u32 draw_serial = 0; ///< counts Draw's calls: QuadVertices' result holds for one
    struct QuadRun {
        u32 serial = ~0u;
        bool indexed{};
        const float* immediate{};
        u32 count{};
        bool ok{};
        std::array<Pica::OutputVertex, 6> out{};
    } quad_run;
    /// The texture units whose texel at each pixel is that pixel's own colour in the buffer the draw
    /// writes (the buffer itself, point sampled, read by the combiners alone, window coordinates on a
    /// quad of one w): the program reads the tile's colour for them, and the pass need not end.
    u32 OwnPixelUnits(bool is_indexed, const float* immediate, u32 count);
    std::unique_ptr<Pica::ShaderEngine> cover_engine;
    /// The vertex program's SA block: its uniforms from `setup`, its literals and buffer
    /// pointer; `vsec_sa` its words.
    SgxdBackend::Data VertexSa(const Usse::GxpProgram& vg, const Pica::ShaderSetup& setup,
                               u32& vsec_sa, const Describe& describe);
    /// The vertex side of a draw's programs: the fetch and secondary PDS programs, the VDM words, the
    /// output selects.
    void VertexPrograms(const VertexStage& stage, const SgxdBackend::Data& vsa, u32 vsec_sa,
                        SgxdRecipeDrawPrograms& p, const Describe& describe);

    /// `u8_regs`: the input registers an integer loader feeds.
    const VertexProgram& VertexProgramFor(u16 default_regs,
                                          const std::array<u8, 16>& reg_components, u16 u8_regs,
                                          u16 s8_regs, u16 s16_regs,
                                          Usse::VsMode mode = Usse::VsMode::Draw);
    VertexProgram PlaceVertexProgram(Usse::Program usse, const char* what, u64 key,
                                     u64 emit_us = 0);
    /// The geometry program for `rows` input rows and at most `max_triangles` triangles an invocation;
    /// `input_rows`: the fixed-primitive mode's registers that read rows of their own.
    const VertexProgram& GeometryProgramFor(u32 rows, bool rows_uniform, bool rows_table,
                                            u32 first_uniform, u16 input_rows, u32 max_triangles,
                                            const Describe& describe);
    const VertexProgram& MakeGeometryProgram(u64 key, u16 bools, u16 used, u32 rows,
                                             bool rows_uniform, bool rows_table, u32 first_uniform,
                                             u16 input_rows, u32 max_triangles,
                                             const Describe& describe);
    /// The vertex program that draws a geometry program's stored triangles: input k is output k.
    const VertexProgram& PassthroughProgramFor(u32 outputs);
    const VertexProgram& PassthroughProgramLookup(u32 outputs, u32 output_mask);
    const VertexProgram* last_passthrough = nullptr;
    std::array<u32, 9> last_passthrough_regs{};
    const FragmentProgram& FragmentProgramFor(const Epilogue& epilogue);
    /// Places a program: its code (with the blend's tail), secondary and PDS. `emit_us` for the log.
    FragmentProgram PlaceFragmentProgram(Usse::Program usse, const Epilogue& epilogue, u64 hash,
                                         u64 emit_us = 0);
    void InstallTier2();
    void InstallVertexTier2(ShaderTier2::Result& result);
    /// The host copy of texture unit `unit`'s texture for the open pass: untiled when it is
    /// new, in every pass that samples it for FCRAM (no hash: the guest CPU's writes leave no
    /// mark), and for VRAM when a GPU operation wrote over it. ETC1 stays compressed. Its untile
    /// is queued (SgxdBackend::QueueUntile): the copy moves at each one, and the pass's
    /// untiles of a kind are one side render, sent before the pass's own.
    const Texture& TextureFor(u32 unit);
    /// Whether the unit holds an ETC1A4 texture split into compressed colour and an A8 alpha texture.
    bool SplitAlpha(u32 unit) const;
    /// The alpha texture's words of a split unit from its colour's, over its A8 chain (swizzle R000).
    static void AlphaTextureWords(const Texture& tex, const u32 colour[4], u32 words[4]);
    /// Frees the least recently used texture no open pass reads; false when there is none.
    bool EvictTexture();
    /// The GPU address of a fixed palette for a 4-bit format (0 for none), written at the first use.
    u32 PaletteFor(TexturePalette palette);
    /// The sampler's four state words for the unit's texture, which it returns (NullTexture when off).
    const Texture& TextureWords(u32 unit, u32 words[4]);
    const Texture& NullTexture(u32 unit);
    /// Marks the textures over [start, end) to be untiled again.
    void TexturesWritten(PAddr start, PAddr end);
    /// The lighting and fog LUTs the guest changed into fresh rows, fs_data's offsets pointed at them.
    void SyncLutLf();
    /// The same for the proctex colour map, alpha map, colour and colour-difference tables.
    void SyncLutProcTex();
    /// The procedural texture's table (Usse::TextureIssue::proctex) for the tables and registers
    /// now, made again when either changed.
    void ProcTexTableWords(u32 words[4]);

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
#ifdef VITA_DIAGNOSTICS
    std::map<PAddr, ColourBuffer> drawn_buffers;
#endif
    /// A list: eviction during a texture's untile keeps the others in place.
    std::list<Texture> textures;
    /// The zeros of a unit that is off, 2D and cube (NullTexture), outside the eviction.
    std::array<Texture, 2> null_textures{};
    /// Textures by address: a bucket for each 4 KiB guest page (TextureBucket), each a chain of the
    /// textures starting there (Texture::bucket_next).
    std::vector<Texture*> texture_buckets;
    static u32 TextureBucket(PAddr addr);
    Texture* FindTexture(PAddr addr) const;
    void IndexTexture(Texture& texture);
    void DropTexture(Texture& texture);
    SgxdBackend::Data palettes{};
    /// The proctex noise NoiseRand2D(x, y) = h for x, y < 256 as F16x4 texels h * (x odd),
    /// h * (y odd), h, 0.
    SgxdBackend::Data noise_table{};
    u32 NoiseTable();
    std::unordered_map<u64, VertexProgram> vertex_programs;
    /**
     * VertexStageFor's decoded vertex formats, by the raw registers that describe one: the
     * attribute formats, each loader's component list, stride and component count, the VS
     * input count and input map (VertexLayoutKey). Not the addresses: the base and the
     * loaders' offsets change mesh by mesh and are added per draw. Most recently used first.
     * Each also keeps the vertex program it resolved to, by the program's own inputs
     * (VertexProgramKey), once that program has drawn the draws before its second tier.
     */
    struct VertexLayout {
        static constexpr u32 ProgramKeyWords = 4 + 4 + 7 + 2;
        struct Attribute {
            u8 reg, loader;
            u32 offset; ///< from the loader's start of a vertex
            u32 stride, words;
        };
        std::array<u32, KeyWords> key{};
        std::array<Attribute, 16> attributes{};
        u32 count = 0;
        u16 enabled_regs = 0, u8_regs = 0, s8_regs = 0, s16_regs = 0;
        std::array<u8, 16> reg_components{};
        std::array<u32, ProgramKeyWords> program_key{};
        const VertexProgram* program = nullptr;
    };
    std::array<VertexLayout, 8> vertex_layouts{};
    u32 vertex_layout_count = 0;
    /// The open draw's vertex format, decoded or found, at vertex_layouts[0].
    VertexLayout& VertexLayoutFor();
    /// The raw words VertexProgramFor's program depends on past the layout.
    std::array<u32, VertexLayout::ProgramKeyWords> VertexProgramKey(Usse::VsMode mode);
    std::unordered_map<u64, u16> used_bools;
    std::unordered_map<u64, FragmentProgram> fragment_programs;
    std::optional<std::pair<u32, u32>> scanned_indices;
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
    /// The procedural textures' tables, kept as the fog tables: the current half's by the guest
    /// tables' contents and the registers the table is made from (proctex_table_regs).
    SgxdBackend::Data proctex_tables{};
    u32 proctex_table_next{}, proctex_table_half{};
    std::array<u32, 2> proctex_table_half_mark{};
    std::array<u32, 16> proctex_table_pass{};
    std::array<u32, 4> proctex_table_words{};
    std::array<u32, 3> proctex_table_regs{};
    bool proctex_table_dirty{true};
    KeyIndex<u32> proctex_tables_by_contents;

    ColourBuffer pass_colour{}; // while the backend is in a pass
    /// A colour buffer of a width no pass draws (under 32, or not in 16s): the pass draws into a
    /// scratch of the next width in 16s (32 at least) and 32 rows at least, copied in before and out
    /// after.
    struct NarrowScratch {
        PAddr addr;
        u32 width, height, bpp;
        u32 scratch_width;
        SgxdBackend::Data surface;
    };
    /// The width of c's scratch; 0 for a width a pass draws itself (32 or more, in 16s).
    [[nodiscard]] static u32 NarrowWidth(const ColourBuffer& c) {
        return c.width >= 32 && c.width % 16 == 0 ? 0 : std::max((c.width + 15) / 16 * 16, 32u);
    }
    std::vector<NarrowScratch> narrow_scratch;
    const NarrowScratch* pass_narrow = nullptr; // the open pass's
    const NarrowScratch& NarrowScratchFor(const ColourBuffer& c);
    /// `lines` lines of `line_bytes` from src to dst, strides apart, queued on the PTLA.
    void CopyLines(u32 src, u32 dst, u32 line_bytes, u32 lines, u32 src_stride, u32 dst_stride,
                   const char* what);
    u16 gs_regs_loaded = 0;     // the GS input registers point-mode draws have loaded
    /// Each GS input register's row as the last draw that loaded it left it.
    std::array<u32, 16> gs_last_rows{};
    /// The rows KeepGsRows copied out of a room a pass took, a 16-byte row a register.
    SgxdBackend::Data gs_kept_rows{};
    /// Draw's fragment SA image and its memory buffer, made here and copied out whole.
    std::vector<u32> fsa_scratch;
    std::vector<u32> fbuf_scratch;
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
    u32 pass_depth_format{};
    float pass_bg_depth{1.0f};  // the depth clear the pass took
    u32 pass_bg_stencil{};
    bool pass_depth_used{};     // a draw tested or wrote depth or stencil
    bool pass_depth_load{};     // it starts from the depth an earlier pass stored
    bool pass_depth_covered{};  // or a draw wrote every pixel's depth before any read it
    bool pass_depth_guest{};    // or from guest memory's, untiled into the store
    u32 guest_depth_loads{};
    /// Depth buffers kept across passes (EndPass stores a pass's depth, the next pass without a clear
    /// loads it).
    struct DepthStore {
        PAddr addr;
        u32 width, height, format;
        SgxdBackend::DepthStore store;
        bool valid; // the last pass on the buffer stored it, and nothing wrote there since
    };
    std::vector<DepthStore> depth_stores;
    DepthStore& DepthStoreFor(PAddr addr, u32 width, u32 height, u32 format);
    SgxdBackend::DepthStore NewDepthStore(u32 width, u32 height, u32 format);
    void DepthStoresWritten(PAddr start, PAddr end);

    u32 fills{};
    u32 dmas{};
    u32 draws{};
    u32 gs_draws{};
    u32 passes{};
    u32 frames{}; ///< frames presented (EndFrame)
#ifdef VITA_DIAGNOSTICS
    bool vprof_frame_start = false; // VprofFrameStart's own EndPass is not a frame's result
    int replay_limit = -2; // -2: no replay; -1: a replay of every draw
    u32 replay_draw_index = 0;
    u32 vprof_draws = 0;
    bool vprof_draw_noted = false;
    void VprofDraw(bool is_indexed, bool immediate, u32 count);
    void VprofPassResult(const ColourBuffer& c);
    void VprofTransferResult(PAddr output, u32 width, u32 height, u32 format, const u8* out,
                             u32 bytes);
    bool ReplaySkipsDraw();
    void PerflogTransfer(PAddr input, u32 in_width, u32 in_height, PAddr output, u32 width,
                         u32 height, u32 scale_x, u32 scale_y, u32 ta_us, u32 d3_us);
    void PerflogDraw(const Describe& describe, u32 isp, bool blend,
                     const SgxdRecipeDrawPrograms& p);
#endif
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
    u64 evictions{};     ///< textures freed for room (TextureFor)
    u32 loads{}; ///< passes that loaded their colour buffer from guest memory
    u64 covered_passes{}; ///< passes whose first draw covers the target (CoversTarget)
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
