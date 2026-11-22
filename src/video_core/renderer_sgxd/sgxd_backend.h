// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

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
 * The rasterizer's GPU memory and passes through sgxd. One
 * block holds a recipe laid out for one target size, then three areas the recipe does not know:
 *
 * - the code heap: USSE programs, append-only, reached from the recipe's code bases (0 at CV for
 *   vertex programs, 1 at CF for pixel programs). The kicks invalidate the USSE code caches, so
 *   new code needs nothing more (4.7).
 * - the PDS arena: PDS programs, shared by content, in two halves used in turn. A PDS program
 *   that has run is never rewritten at the address it ran at (docs/sgxd/pds.md): each use of a
 *   half maps it at a fresh window (FlipPdsHalf, PdsWindows in the .cpp).
 * - the pass data: SA blocks and uniform buffers, rewritten every pass. They are DOUTD and LD
 *   sources, which the kicks' DCU invalidate and the SLC invalidate before the render cover.
 *
 * Host surfaces (render targets) come from a second block. Every pass is synchronous: EndPass
 * returns with the picture in memory.
 *
 * The host build has no GPU: the memory is the process's own, every word is built as on the
 * Vita, and EndPass renders nothing.
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
    /// The proctex LUT textures' rows, after the LF rows: Pica::Shader::LUT_RG_ROWS rows of 256
    /// F32F32 texels (value, delta) and Pica::Shader::LUT_RGBA_ROWS rows of 256 F16F16F16F16.
    [[nodiscard]] Data LutRgRows() const;
    [[nodiscard]] Data LutRgbaRows() const;

    /// Opens a pass on a surface of the recipe's size (the first pass fixes it). The
    /// background is clear (A8B8G8R8) at depth, what a fill left pending.
    void BeginPass(const Data& target, u32 width, u32 height, u32 clear, float depth);
    [[nodiscard]] bool InPass() const {
        return in_pass;
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
    /// Renders the pass and waits for it. Returns the TA's and the 3D's microseconds.
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
    void CreateRecipe(u32 width, u32 height);
    /// The recipe's target, its size checked, and the pass state reset.
    void OpenPass(u32 target_gpu, u32 width, u32 height, u32 pack, u32 clear, float depth);

    u8* memory{};      // the block: recipe, code heap, PDS arena, pass data
    u32 size{};
    u8* surfaces{};    // the host surfaces' block
    u32 surfaces_size{};
    u32 surfaces_spilled{}; ///< surfaces the block had no room for (AllocSurface), for its log
    u32 surfaces_used{};
    int block{-1}, surfaces_block{-1};

    SgxdRecipe recipe{};
    bool recipe_ready{};
    u32 bg_offset{};   // the background colour word and its SA secondary, after the layout
    std::vector<u16> identity;
    std::vector<SgxdRecipeDraw> init_draws;

    u32 code_begin{}, code_used{}, code_end{};
    u32 pds_begin{}, pds_used{}, pds_end{};
    u32 data_begin{}, data_used{}, data_end{};
    std::unordered_map<std::string, u32> pds_by_words;

    bool in_pass{};
    u32 target_width{}, target_height{};
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
