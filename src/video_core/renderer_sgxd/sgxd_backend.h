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
    };
    /// A size's instances at most: a render's recipe is the GPU's until its 3D completes, and
    /// the CPU writes the next render's into another.
    static constexpr u32 MaxInstances = 4;

    /// The recipe for a width x height target, made at its first use.
    Recipe& RecipeFor(u32 width, u32 height);
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

    // The blit's programs (DOUTU word 0s), placed at its first use.
    bool blit_ready{};
    u32 blit_vertex{}, blit_fragment{};
};

} // namespace SgxdRenderer
