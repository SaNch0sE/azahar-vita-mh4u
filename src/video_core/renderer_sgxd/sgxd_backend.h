// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/common_types.h"
#include "sgxd_program.h"
#include "sgxd_ptla.h"
#include "sgxd_recipe.h"

namespace SgxdRenderer {

/**
 * The rasterizer's GPU memory and passes through sgxd (docs/sgxd/README.md). Each
 * target size has its own recipe, made at its first pass, in a block of its own that also holds
 * a copy of the code heap:
 *
 * - the code heap: USSE programs, append-only, reached from the recipe's code bases (0 at CV for
 *   vertex programs, 1 at CF for pixel programs). Every recipe's copy sits at the same distance
 *   from its CV and CF, so a program's DOUTU is the same under every recipe; PlaceCode writes
 *   all the copies. The kicks invalidate the USSE code caches, so new code needs nothing more
 *   (docs/sgxd/pds.md).
 *
 * One shared block holds what is addressed absolutely, the same for every recipe:
 *
 * - the PDS arena: PDS programs, shared by content, in two halves used in turn. A PDS program
 *   that has run is never rewritten at the address it ran at (docs/sgxd/pds.md): each use of a
 *   half maps it at a fresh window (FlipPdsHalf, PdsWindows in the .cpp).
 * - the pass data: SA blocks, uniform buffers and immediate-mode vertices, rewritten every pass.
 *   They are DOUTD and LD sources, which the kicks' DCU invalidate and the SLC invalidate before
 *   the render cover.
 * - the LUT rows: the lighting and fog LUTs as one 256-wide F32F32 texture, a ring of rows the
 *   rasterizer hands out (TCU invalidated by every kick).
 *
 * Host surfaces (render targets) come from a second block. Nothing the GPU runs is waited
 * for when it is sent: passes, side renders, fills and copies are lists in sgxd's queue, run in
 * order. Each CPU-written resource (a recipe instance, a pass data slot, a PDS half, the LUT
 * halves) carries the fence of the last list that reads it, waited for only when the CPU
 * writes it again. The guest's interrupts go at submission (VideoCore::GPU::Signal), and the
 * guest pages the work uses are held until its fence is reached (PageHolds).
 */
class SgxdBackend {
public:
    SgxdBackend();
    ~SgxdBackend();

    SgxdBackend(const SgxdBackend&) = delete;
    SgxdBackend& operator=(const SgxdBackend&) = delete;

    /// Diagnostics: called by a fault's post-mortem after the backend's own dumps (the
    /// frontend's present writes its recipe block and the parameter buffer).
    static inline void (*fault_hook)() = nullptr;
    /// Diagnostics: called by a fault's post-mortem before fault_hook; the rasterizer logs the
    /// geometry draw's stream-out render it is in, if any, and dumps its PICA programs.
    static inline void (*stream_hook)() = nullptr;
    /// The command channel's `hangtest`: the next pass with draws gets a pixel program that never
    /// ends (DrawHang), to test hang recovery.
    static inline std::atomic<bool> hang_test{false};

    /// Draws a pass takes at most.
    static constexpr u32 MaxDraws = 256;

    /// USSE code: its offset from code base cb (0 vertex, 1 pixel).
    u32 PlaceCode(unsigned cb, std::span<const u64> words);
    /// A PDS program's GPU address, the same for the same words.
    u32 PlacePds(const SgxdPdsProgram& program);

    struct Data {
        u8* cpu;
        u32 gpu;
    };
    Data AllocData(u32 bytes, u32 align = 16);

    /// A host surface: width x height A8B8G8R8 pixels, rows width apart.
    Data AllocSurface(u32 width, u32 height);
    /// The LUT texture's rows, in the shared block: Pica::Shader::LUT_LF_ROWS rows of 256 F32F32
    /// texels, the rasterizer's to manage.
    [[nodiscard]] Data LutRows() const;
    /// The proctex LUT textures' rows, after the LF rows: Pica::Shader::LUT_RG_ROWS rows of 256
    /// F32F32 texels (value, delta) and Pica::Shader::LUT_RGBA_ROWS rows of 256 F16F16F16F16.
    [[nodiscard]] Data LutRgRows() const;
    [[nodiscard]] Data LutRgbaRows() const;

    /// Opens a pass on a surface, under the recipe for its size. The background is clear
    /// (A8B8G8R8) at depth, what a fill left pending.
    void BeginPass(const Data& target, u32 width, u32 height, u32 clear, float depth);
    [[nodiscard]] bool InPass() const {
        return in_pass;
    }

    /// PTLA ops complete through sgxd's interrupt (sgxd_ptla_events). PtlaFence is the last
    /// op's completion point.
    [[nodiscard]] u32 PtlaFence() const;
    [[nodiscard]] bool PtlaReached(u32 fence) const;
    /// Renders run as one async list each (sgxd_session_render_async), unless gxm_flags
    /// renderpoll or ptlapoll: EndPass returns once the TA is kicked. RenderFence is the last
    /// pass's, 0 when synchronous; RenderTaReached when its TA has read the vertices.
    [[nodiscard]] bool RenderAsync() const {
        return render_async;
    }
    [[nodiscard]] u32 RenderFence() const;
    [[nodiscard]] bool RenderReached(u32 fence) const;
    [[nodiscard]] bool RenderTaReached(u32 fence) const;
    /// Waits for the last async render, and stops the process with the post-mortem if it
    /// failed.
    void RenderIdle(const char* why);
    /// Every list sent so far, and the pending 3D's finish (sent by RenderWait if needed).
    [[nodiscard]] u32 FenceAll() const;
    /// Waits for a fence (RenderFence's, FenceAll's, or a list's count), sending the pending
    /// 3D's finish if the fence takes it; a failed list stops the process as RenderIdle does.
    void RenderWait(u32 fence, const char* why);
    /// A mark covering every pass so far, the open one included once it ends; WaitPassMark
    /// waits for its passes' 3Ds (the LUT rows they sample). Marks of passes long gone wait
    /// for the oldest pass still known, which ended later.
    [[nodiscard]] u32 PassMark() const {
        return passes + (in_pass ? 1 : 0);
    }
    void WaitPassMark(u32 mark, const char* why);
    /// CPU work on what the GPU writes (a DMA's cached destination): run once every list sent
    /// so far has completed, before the guest's interrupt for it (InterruptPoll, InterruptWait)
    /// or a wait for everything. RetireAll waits for and runs every one.
    void AfterGpu(std::function<void()> action);
    void RetireAll(const char* why);
    [[nodiscard]] bool RetirePending() const {
        return !retire.empty();
    }
    [[nodiscard]] const SgxdEvents& Events() const {
        return events;
    }
    [[nodiscard]] u32 PassDraws() const {
        return draw_count;
    }
#ifdef VITA_DIAGNOSTICS
    /// passlog: the open pass's primitives as the VDM counts them (a strip's or fan's count - 2),
    /// for the TE split's runs of 1000.
    [[nodiscard]] u32 PassPrimitives() const {
        u32 n = 0;
        for (u32 d = 0; d < draw_count; d++) {
            const u32 count = draws[d].count;
            const u32 primitive = draws[d].programs ? draws[d].programs->primitive : 0;
            n += primitive == 0 ? count / 3 : count > 2 ? count - 2 : 0;
        }
        return n;
    }
    void NotePass(std::string_view note) {
        if (pass_note.size() < 600) {
            pass_note += note;
        }
    }
#endif
    /// Adds a draw to the open pass; its programs are copied.
    void Draw(const SgxdRecipeDraw& draw, const SgxdRecipeDrawPrograms& programs);
    /// A range of cached guest memory the pass reads, cleaned from the CPU's cache at EndPass.
    void ReadsCached(const u8* cpu, u32 bytes);
    /// Renders the pass: waits for it when synchronous, returns once the TA is kicked when
    /// async. Returns the TA's and the 3D's microseconds, the render before's when async.
    void EndPass(u32* ta_us = nullptr, u32* d3_us = nullptr);

    /// A store into guest memory: gpu holds width x height pixels of pack (an
    /// SGXD_RECIPE_PACK_*), rows width apart, cpu its cached view of bytes bytes (null and 0
    /// for uncached memory).
    struct Store {
        u32 gpu;
        u8* cpu;
        u32 bytes;
        u32 pack;
    };
    /// Guest textures' host copies (RasterizerSgxd's texture cache), from a block of their own:
    /// bytes at a 4 KiB multiple, the first fit of what FreeTexture gave back.
    Data AllocTexture(u32 bytes);
    void FreeTexture(const Data& texture, u32 bytes);
    /// poolcheck (diagnostics): each page of the textures pool in use as the GPU reads it (a
    /// PTLA copy to a probe page) against the CPU's view; logs the pages that differ.
    void CheckTexturePool();

    /// A PICA texture as the untile reads it: width x height texels (powers of two, 8 to
    /// UntileSide) of bits each at gpu, in 8x8 tiles with Morton order inside, read through
    /// the texture unit's format code and swizzle (docs/sgxd/tsp.md), and for P4
    /// and P8 the palette's GPU address (64-byte aligned).
    struct UntileSource {
        u32 gpu;
        u32 width, height;
        u32 bits;
        u32 code;
        u32 swizzle;
        u32 palette;
        /// The copy a byte a texel, the texel's A (EXTMODE1 U8): QueueUntile's and UntileChain's
        /// LINEAR chains of a one-channel format, read as U8 (RasterizerSgxd::TextureWords).
        bool byte_copy = false;
    };
    static constexpr u32 UntileSide = 1024;
    /// UntileSource::code for ETC1 (the texture unit's ETC1, whose blocks the untile reorders
    /// first).
    static constexpr u32 UntileEtc1 = 0x84;
    /// UntileSource::code for ETC1A4, which the texture unit has no format for: ETC1 blocks
    /// each after a 4-bit alpha block, joined with the alpha by a merge render.
    static constexpr u32 UntileEtc1A4 = 0x184;
    /**
     * Untiles source into dest, a linear A8B8G8R8 image (R the lowest byte) with rows
     * dest_stride texels apart, in the PICA's row order: row 0 is the texture's t = 1, and the
     * fragment programs read at (s, 1 - t), since an ETC1 block cannot be flipped by moving
     * it. A render of its own under a recipe no pass uses, queued ahead of the open pass's
     * render, whose draws are not kicked yet.
     *
     * The tiled bytes read as a SWIZZLED texture 8 texels wide are the PICA's tiles stacked,
     * each transposed (SWIZZLED is Morton with y in bit 0): SGX texel (sx, sy) is tile sy / 8,
     * PICA x = sy % 8, y = sx. One quad per tile row samples it point for pixel through the
     * screen program (the texel converted to half4, packed to U8888), in bands of at most 4096
     * SWIZZLED rows, the texture unit's largest side.
     *
     * dest_stride is a multiple of 32, or the width when that is under 32 (a LINEAR mip
     * level's rows, max(w, 8) texels apart: docs/sgxd/tsp.md), where 32 / w rows share
     * each 32-pixel PBE row. The PBE writes every tile a quad touches whole: dest takes
     * max(rows, 32) of max(dest_stride, 32) texels, rows the image's or, packed, h * w / 32.
     * Past the image that is scratch a later untile may write over.
     *
     * swizzled: dest is a square SWIZZLED texture instead (a cube face), written through the
     * PBE's TWIDDLED layout; dest_stride is unused. The whole tiles take max(w, 32)^2 texels.
     */
    void Untile(const UntileSource& source, u32 dest_gpu, u32 dest_stride, bool swizzled = false);

    SgxdRecipeDrawPrograms BlitPrograms(const Data& source, u32 width, u32 height);
    void Blit(const Data& source, u32 width, u32 height, const Store& dest, u32* ta_us = nullptr,
              u32* d3_us = nullptr);

private:
    /// One target size's recipe and its block: the layout, the background colour word, then
    /// the code heap's copy.
    struct Recipe {
        SgxdRecipe recipe{};
        u8* memory{};
        u32 va{};
        u32 size{};
        int block{-1};
        u32 width{}, height{};
        u32 bg_offset{};  // the background colour word and its SA secondary, after the layout
        u32 code_begin{}; // the code heap's copy, from the block's start
        bool side{};      // the untile's, which no pass shares
    };
    /// A size's instances at most: a render's recipe is the GPU's until its 3D completes, and
    /// the CPU writes the next render's into another.
    static constexpr u32 MaxInstances = 4;

    /// The recipe for a width x height target, made at its first use; side: the untile's.
    Recipe& RecipeFor(u32 width, u32 height, bool side = false);
    void RenderSync(Recipe& r, u32* ta_us, u32* d3_us);
    void RenderAsync(Recipe& r, const u32 touches[6]);
    void PlaceFixedPrograms();
    /// One draw of a side render: its texture, whether the texel is converted (the screen
    /// program) or copied raw (the blit's), and its vertices. A merge render's draws read
    /// tex and tex1 raw and write their AND.
    struct SideDraw {
        u32 tex[4];
        bool converted;
        u32 first, count;
        u32 tex1[4];
    };
    /// A render of its own under the side recipe, sent ahead of the open pass's:
    /// the blit's quads in vertices (PutQuad) into a width x height U8888 target at dest_gpu,
    /// rows dest_stride pixels apart, clipped to its tiles. With merge, the quads are
    /// PutMergeQuad's, with a coordinate for each texture. With twiddled, the target is
    /// stored in the PBE's TWIDDLED layout.
    void SideRender(const Data& vertices, std::span<const SideDraw> list, u32 dest_gpu,
                    u32 dest_stride, u32 width, u32 height, bool merge = false,
                    bool twiddled = false);
    void UntileEtc1Blocks(const UntileSource& source, u32 dest_gpu, u32 dest_stride,
                          bool swizzled);
    void UntileEtc1A4Merge(const UntileSource& source, u32 colour_gpu, u32 colour_stride,
                           u32 dest_gpu, u32 dest_stride, bool swizzled);
    /// The recipe's target, and the pass state reset.
    void OpenPass(u32 target_gpu, u32 width, u32 height, u32 pack, u32 clear, float depth);

    std::vector<std::unique_ptr<Recipe>> recipes;
    Recipe* current{}; // the open pass's
    u32 next_va{};     // where the next recipe's block goes

    u8* shared{};      // the shared block: PDS arena, pass data
    u32 shared_size{};
    int shared_block{-1};
    u8* surfaces{};    // the host surfaces' block
    u32 surfaces_size{};
    u32 surfaces_spilled{}; ///< surfaces the block had no room for (AllocSurface), for its log
    u32 surfaces_used{};
    int surfaces_block{-1};
    u8* textures{};    // the guest textures' host copies
    u32 textures_used{};
    int textures_block{-1};
    struct FreeRange {
        u32 offset, bytes;
        /// The textures' pools: FenceAll when the range was given back, the work that may still
        /// sample or write it (AllocTexture's `ready`); merged ranges keep the newer.
        u32 fence = 0;
    };
    std::vector<FreeRange> textures_free;
    /// An untile's vertices by its geometry (GeometryKey): a texture of a size seen before reuses
    /// them. In the textures pool: ETC1 uploads inside a pass filled the pass data.
    std::unordered_map<u64, Data> geometry_vertices;
    /// The cached vertices for key, or `bytes` of new ones with fresh set for the caller to
    /// fill.
    Data GeometryVertices(u64 key, u32 bytes, bool& fresh);

    std::vector<u16> identity;
    std::vector<SgxdRecipeDraw> init_draws;

    u32 code_used{};   // the code heap's bytes, the same in every copy
    /// Diagnostics: every program PlaceCode placed (bank, offset from the bank's base as the
    /// DOUTU takes it, bytes, place in the heap), written with the heap at a fault.
    struct PlacedCode {
        u32 cb, offset, bytes, at;
    };
    std::vector<PlacedCode> placed_code;
    u32 pds_begin{}, pds_used{}, pds_end{};
    u32 data_begin{}, data_used{}, data_end{};
    u32 lut_begin{};
    /// The last passes' fences by number (passes % PassFences): WaitPassMark's.
    static constexpr u32 PassFences = 64;
    std::array<u32, PassFences> pass_fences{};
    std::vector<std::pair<u32, std::function<void()>>> retire;
    void RetireReached();
    SgxdEvents events{};
    bool ptla_async{};
    bool render_async{};
    void Fault(const Recipe& r, const char* failed);
    std::unordered_map<std::string, u32> pds_by_words;

    bool in_pass{};
    std::vector<SgxdRecipeDraw> draws;
    std::vector<SgxdRecipeDrawPrograms> programs;
    u32 draw_count{};
    struct CachedRange {
        const u8* cpu;
        u32 bytes;
    };
    std::vector<CachedRange> cached;
    std::vector<Store> stores;   // guest memory the open pass writes
    u32 passes{};

    bool blit_ready{};
    u32 blit_vertex{}, blit_fragment{}, untile_fragment{}, merge_vertex{}, merge_fragment{};
    std::vector<SgxdRecipeDraw> untile_draws;
    std::vector<SgxdRecipeDrawPrograms> untile_programs;
    // ETC1A4's alpha palette for the merge: entry n is 17n in alpha over white (the AND keeps
    // the colour), from the texture block at the first ETC1A4.
    Data merge_palette{};
};

} // namespace SgxdRenderer
