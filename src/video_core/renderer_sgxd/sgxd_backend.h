// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <functional>
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
    static constexpr u32 PassDataBytes = 2u << 20;

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

    /// Draws a pass takes at most, each about 200 bytes of its recipe's layout
    /// (RasterizerSgxd::Draw ends a full pass).
    static constexpr u32 MaxDraws = 1024;

    /// USSE code: its offset from code base cb (0 vertex, 1 pixel).
    u32 PlaceCode(unsigned cb, std::span<const u64> words);
    /// PlaceCode for a program of several phases: each phase's opening PHAS but the last's
    /// (an immediate PHAS at phase_starts[k]) gets the next phase's execution address, the
    /// DOUTU0 EXE field's instruction index from the code base, and the temps in fours.
    u32 PlaceCodePhased(unsigned cb, std::vector<u64> words, std::span<const u32> phase_starts,
                        u32 temps);
    /// A PDS program's GPU address, the same for the same words while it lasts: fixed, for
    /// good (a program whose address is kept); otherwise until the flipping half it is in is
    /// reused, at a pass start (OpenPass).
    u32 PlacePds(const SgxdPdsProgram& program, bool fixed = false);

    struct Data {
        u8* cpu;
        u32 gpu;
    };
    Data AllocData(u32 bytes, u32 align = 16);
    [[nodiscard]] u32 DataLeft() const {
        const u32 at = (data_used + 15) & ~15u;
        return at < data_end ? data_end - at : 0;
    }

    /// A host surface: width x height A8B8G8R8 pixels, rows width apart; the first fit of what
    /// FreeSurface gave back.
    Data AllocSurface(u32 width, u32 height);
    /// A surface no longer used (a colour buffer the guest reshaped), after its last read.
    void FreeSurface(const Data& surface, u32 width, u32 height);
    /// The LUT texture's rows, in the shared block: Pica::Shader::LUT_LF_ROWS rows of 256 F32F32
    /// texels, the rasterizer's to manage.
    [[nodiscard]] Data LutRows() const;
    /// The proctex LUT textures' rows, after the LF rows: Pica::Shader::LUT_RG_ROWS rows of 256
    /// F32F32 texels (value, delta) and Pica::Shader::LUT_RGBA_ROWS rows of 256 F16F16F16F16.
    [[nodiscard]] Data LutRgRows() const;
    [[nodiscard]] Data LutRgbaRows() const;

    /// Opens a pass on a surface, under the recipe for its size. The background is clear
    /// (A8B8G8R8) at depth, what a fill left pending. before: as BeginPassLoaded's.
    void BeginPass(const Data& target, u32 width, u32 height, u32 clear, float depth,
                   const std::function<void()>& before = {});
    [[nodiscard]] bool InPass() const {
        return in_pass;
    }

    /// PTLA ops complete through sgxd's interrupt (sgxd_ptla_events). PtlaFence is the last
    /// op's completion point.
    [[nodiscard]] u32 PtlaFence() const;
    [[nodiscard]] bool PtlaReached(u32 fence) const;
    /// Renders run as one async list each (sgxd_session_render_overlap): EndPass returns once
    /// the TA is kicked; RenderTaReached when its TA has read the vertices. A pass's TA runs
    /// while the pass before's 3D does: its list ends at its own 3D kick, and the next list, or
    /// RenderFinish's, waits for that 3D. RenderFence counts that list; RenderTaFence the
    /// pass's own.
#ifdef VITA_DIAGNOSTICS
    /// vprof's replay: every pass synchronous, its (pass, TA us, 3D us) in replay_times.
    bool replay_sync{};
    std::vector<std::array<u32, 3>> replay_times;
    /// sgxd_flags syncside: the side renders (untiles, gathers) run whole too, (count, TA us,
    /// 3D us) summed here; a replay lists them after the frame's passes as one more.
    std::array<u32, 3> side_times{};
#endif
    [[nodiscard]] u32 RenderFence() const;
    [[nodiscard]] u32 RenderTaFence() const;
    [[nodiscard]] bool RenderReached(u32 fence) const;
    [[nodiscard]] bool RenderTaReached(u32 fence) const;
    /// Waits for the last async render, 3D included, and stops the process with the
    /// post-mortem if it failed.
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
    /// Hands a pending 3D its finishing list without waiting for it, if something waits on it
    /// (RenderFence): after each GPU op (RasterizerInterface::GpuIdle). Otherwise the next
    /// pass's list finishes it.
    void RenderFinish();
    /// A PTLA fill or copy is about to touch GPU addresses [lo, hi): the pending 3D's finish
    /// goes in the queue first if it writes or samples any of them. Nothing waits.
    void RenderBefore(u32 lo, u32 hi, const char* why);
    /// A guest interrupt's GPU work (VideoCore::GPU::Signal): every list and PTLA op sent so
    /// far, the pending 3D included. Unlike RenderFence it leaves the pending 3D unmarked, so
    /// no op waits for the list before it; InterruptPoll sends its finish once that list ends.
    struct InterruptFence {
        u32 render;
        u32 ptla;
    };
    [[nodiscard]] InterruptFence InterruptFenceNow() const;
    /// Whether the fence's work has completed, without waiting. A pending 3D the fence takes
    /// gets its finishing list here once the list before it has ended.
    [[nodiscard]] bool InterruptPoll(const InterruptFence& fence);
    /// Waits for the fence's work; a failed render stops the process as RenderIdle does.
    void InterruptWait(const InterruptFence& fence);
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
    /// What AllocTexture calls while the pool has no room: frees a texture and returns true, or
    /// returns false when none can go (the rasterizer's cached textures, least recently used).
    void SetTextureEvictor(std::function<bool()> evictor) {
        texture_evictor = std::move(evictor);
    }
    /// What OpenPass calls as a pass takes the room [begin, end) of AllocData's ring (GPU
    /// addresses), the passes that wrote there done, before the pass writes over it: the
    /// rasterizer keeps what a later draw still reads from there (RasterizerSgxd::KeepGsRows).
    void SetRoomTaken(std::function<void(u32 begin, u32 end)> taken) {
        room_taken = std::move(taken);
    }
    [[nodiscard]] const u8* SharedAt(u32 gpu) const;
    u32 TexturePoolLive() const;
    /// Memory from the textures pool that is never freed (vertex arrays built once, palettes,
    /// the noise table): from the pool's end down, apart from the textures it would split.
    Data AllocTextureFixed(u32 bytes);
    [[nodiscard]] bool TextureFits(u32 bytes) const;
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
    /// Opens a pass on a surface that keeps its picture instead of a clear: the pass's first
    /// draw blits the surface onto itself, or with guest, the guest's tiled picture (untiled
    /// into the pass's data first), texel for pixel. before runs once the pass is open, ahead
    /// of the load: side renders (TileBack) that must land in guest memory first.
    void BeginPassLoaded(const Data& target, u32 width, u32 height, float depth,
                         const UntileSource* guest, const std::function<void()>& before = {});
    /// Opens a pass whose first draw writes every pixel: no clear and no load.
    void BeginPassOver(const Data& target, u32 width, u32 height, float depth,
                       const std::function<void()>& before = {});
    /// The open pass's background stencil, a D24S8 clear's top byte (ISP_BGOBJ): 0 at each
    /// OpenPass.
    void SetBackgroundStencil(u32 stencil) {
        sgxd_recipe_set_background_stencil(&current->recipe, static_cast<uint8_t>(stencil));
    }
    /// A depth buffer kept across passes: width x height F32 samples (whole 32x32 tiles, rows
    /// width apart), then a stencil byte a sample in the same layout when stencil.
    struct DepthStore {
        Data memory;
        u32 width, height;
        bool stencil;
    };
    /// Where a pass's depth starts from: a clear, the store's F32 depth and stencil bytes, or
    /// a guest D24S8 (D24) buffer's words untiled into the store (ZLS I24ZI8S).
    enum class DepthLoad { None, Store, Guest };
    /// The open pass's depth (and stencil) loaded from store at its start as load says, and
    /// stored there at its end (ZLS, sgxd_recipe_set_zls). OpenPass turns it off.
    void SetDepthStore(const DepthStore& store, DepthLoad load);
    /// A quad over the open pass's whole target that the ISP rejects (DCMP NEVER): every tile
    /// holds an object, so none stays EMPTY and skips its ZLS load and store.
    void TouchAllTiles();
    /// store's samples as a clear leaves them, depth z and stencil, through the PTLA: a tile no
    /// primitive touches neither loads nor stores.
    void FillDepthStore(const DepthStore& store, float z, u32 stencil);
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

    /// The geometry shader's stream-out stages (RasterizerSgxd::GeometryStage): A0 stores
    /// the vertex program's outputs, A1 runs the geometry program over them.
    enum class StreamStage : u32 { A0, A1 };

    /**
     * A draw whose vertex program stores its results (Usse::VsMode StoreOutputs or Geometry)
     * and covers no pixel, queued for one render of its stage. `programs` brings the vertex
     * side (vspds, vsec, the VDM words, the output selects, the index list of `count`
     * indices, a multiple of 3); the pixel side is the blit's, never run (DCMP never). The
     * vertex ranges ReadsCached named are cleaned now (the pass's own draws read them later,
     * from memory). The queue goes out at the pass's end, before an op that changes guest
     * memory (RenderBefore, a remap), or when a stage holds StreamMaxDraws draws, as two
     * renders under the side recipe: every queued A0
     * draw, then every A1 draw, so an A1 reads its A0's stores from memory, and the pass's TA
     * after both reads A1's. With `wait`, an A1 sends the queue and the open pass waits for
     * it, so the CPU can read what it stored.
     */
    void StreamOut(const SgxdRecipeDrawPrograms& programs, u32 count, StreamStage stage, bool wait);
    void FlushStreamOut(bool wait);

    /// mirror: the texel is read converted with its lanes mirrored (A, B, G, R) and packed
    /// again, for a pack that stores the channels in the other order (A4R4G4B4 as RGBA4).
    /// source_stride: the source's rows apart in texels, 0 for width.
    /// scale_x, scale_y (1 or 2): each output pixel is the average of scale_x x scale_y
    /// source texels (the display transfer's ScaleX and ScaleXY), a bilinear read between
    /// them; the source then has width * scale_x columns and height * scale_y rows.
    SgxdRecipeDrawPrograms BlitPrograms(const Data& source, u32 width, u32 height,
                                        bool mirror = false, u32 source_stride = 0,
                                        u32 scale_x = 1, u32 scale_y = 1);
    void Blit(const Data& source, u32 width, u32 height, const Store& dest, u32* ta_us = nullptr,
              u32* d3_us = nullptr);
    /// The same into a PICA-tiled picture: one pass on target as a PICA colour buffer (as
    /// BeginPassLoaded opens it), the source read as its load reads, scaled.
    void TransferTiledFromGuest(const PicaTarget& target, const UntileSource& source, u32 scale_x,
                                u32 scale_y, u32* ta_us = nullptr, u32* d3_us = nullptr);
    /**
     * The untile's inverse: a pass's width x height surface (A8B8G8R8, rows width apart) written
     * into dest as the PICA stores a colour buffer, in 8x8 tiles row after row with Morton
     * order inside, x first, each pixel `bytes` wide in pack (an SGXD_RECIPE_PACK_*; mirror as
     * Blit's). The PBE's TWIDDLED layout is Morton order y first: a PICA tile is an 8x8 block of
     * it transposed, and the tiles in memory order fill a square twiddled target's blocks in its
     * own order. So the picture goes out in square targets (a side render each, to completion),
     * one quad a tile. Inside a pass or not; width and height multiples of 8. The PBE writes
     * whole 32x32 tiles: the tiles past the last multiple of 16 render into a square of their
     * own, which the CPU copies to dest.cpu.
     */
    void TileBack(const Data& surface, u32 width, u32 height, const Data& dest, u32 pack, bool mirror,
                  u32 bytes);
    /// Memory for a CPU copy of what a side render wrote (TileBack): one buffer in the textures
    /// pool, grown as needed, valid until the next call. Side renders run to completion, so
    /// the last copy's reads are done by then. Not the pass data: a pass opening writes back
    /// several surfaces (sm3dl's passed 2 MiB).
    Data Scratch(u32 bytes);

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
        u32 instance{};   // up to MaxInstances of each size, the pending pass's among them
        u32 fence{};      // the list that completes its last render's 3D
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
    /// A render of its own under the side recipe, sent ahead of the open pass's: the quads in
    /// vertices into a width x height target of pack at dest_gpu, rows dest_stride pixels apart.
    /// merge: PutMergeQuad's quads, a coordinate for each texture. twiddled: the PBE's TWIDDLED
    /// layout.
    void SideRender(const Data& vertices, std::span<const SideDraw> list, u32 dest_gpu,
                    u32 dest_stride, u32 width, u32 height, bool merge = false,
                    bool twiddled = false, u32 pack = SGXD_RECIPE_PACK_U8888);
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
    /// AllocTextureFixed's lowest byte: its blocks run down from the pool's end.
    u32 textures_fixed{};
    int textures_block{-1};
    struct FreeRange {
        u32 offset, bytes;
        /// The textures' pools: FenceAll when the range was given back, the work that may still
        /// sample or write it (AllocTexture's `ready`); merged ranges keep the newer.
        u32 fence = 0;
    };
    std::vector<FreeRange> textures_free;
    std::vector<FreeRange> surfaces_free;
    /// TileBack's quads by surface size (width << 16 | height), from the textures' block: the
    /// same every time, so built once.
    std::unordered_map<u32, Data> tileback_vertices;
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
    u32 pds_fixed_used{}, pds_half{};
    u64 pds_flips{};
    /// Each half's last reader: FenceAll when the arena flipped away from it (its programs
    /// are not reused after that).
    std::array<u32, 2> pds_fence{};
    /// Resets every core's data mover, shader units and caches together, the GPU idle: the PDS
    /// forgets the programs it ran (docs/sgxd/pds.md). why: for the stop's message.
    void ResetCores(const char* why);
    void FlipPdsHalf();
    u32 data_begin{}, data_used{}, data_end{};
    u32 data_peak{}; ///< perflog: the largest pass data logged
    Data scratch{};    ///< Scratch's buffer
    struct DataSite {
        uintptr_t caller;
        u32 bytes;
        u32 count;
    };
    std::array<DataSite, 16> data_sites{}; ///< the open pass's AllocData by caller
    u32 scratch_bytes{};
    /// The pass data's slots, in turn, each with its last pass's fence. The guest waits for
    /// P3D before its next command list, so only passes of one list follow each other unwaited.
    static constexpr u32 DataSlots = 2;
    u32 data_slot{};
    std::array<u32, DataSlots> data_fence{};
    u32 lut_begin{};
    /// The last passes' fences by number (passes % PassFences): WaitPassMark's.
    static constexpr u32 PassFences = 64;
    std::array<u32, PassFences> pass_fences{};
    std::vector<std::pair<u32, std::function<void()>>> retire;
    void RetireReached();
    SgxdEvents events{};
    // The 3D kicked at its list's end and not known to be done is sgxd_session_pending's (ours or
    // the present's). What the open pass's 3D writes and samples outside its recipe and the pass
    // data (GPU addresses).
    struct Footprint {
        u32 lo, hi;
    };
    Footprint pass_target{}, pass_source{}, pass_zls{};
    Data tileback_tail{}; ///< TileBack's square for the tiles past its last whole one
    std::function<bool()> texture_evictor;
    std::function<void(u32, u32)> room_taken;
    /// Our recipe whose 3D is pending, or null (none, or the present's).
    [[nodiscard]] Recipe* Pending() const;
    /// Why a pending 3D is finished by a list of its own rather than the next render list's
    /// (perflog counts them: the next pass's TA then overlaps no 3D).
    enum class FinishWhy : u8 {
        Idle,
        Wait,
        Remap,
        Fenced,
        Touch,
        Poll,
        Kick,
        PdsFlip,
        FreeSurface,
        FreeTexture,
        Count,
    };
    /// Submits the pending 3D's finishing list once the list before has ended.
    void FinishPending(FinishWhy why);
    /// gxm_flags tracelog: the ended lists' GPU stamps as events.
    void TraceStamps();
    void Fault(const Recipe& r, const char* failed);
#ifdef VITA_DIAGNOSTICS
    void WriteCode();
#endif
    std::unordered_map<std::string, u32> pds_by_words, pds_fixed_by_words;

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
    /// gxm_flags perflog: each target's passes, draws and GPU time over a second. A list's TA is
    /// known when the next pass ends, the 3D it waited for one pass before that.
    struct TargetTime {
        u32 passes, draws;
        u64 ta_us, d3_us;
    };
    std::unordered_map<u64, TargetTime> target_times;
    std::array<u64, 2> ended_targets{};
    std::string pass_note;
    int eot_mix_variant = -1; ///< ProbeMixedEot's, for OpenPass
public:
    /// sgxd_flags eotmixreal=V+1: the 2x passes end their tiles with sgxd_recipe_pica_eot_mix's
    /// variant V (the guest's 1x copy stored by the same end of tile), -1 for none.
    int eot_mix_real = -1;
private:
    std::array<std::string, 2> ended_notes;
    u32 slow_passes{};
    std::array<u32, static_cast<std::size_t>(FinishWhy::Count)> finishes{};
    u64 target_times_start{};
    void TargetTimes(u32 width, u32 height, u32 ta_us, u32 d3_us);
    /// gxm_flags perflog: the CPU's spins on the GPU by the site that waited (string literals),
    /// from sgxd_wait_us_total's growth across each; what no site took is "elsewhere".
    std::unordered_map<const char*, std::pair<u32, u64>> wait_sites;
    u64 wait_sites_mark{};
#endif
    void WaitSite(const char* site, u64 before);

    bool blit_ready{};
    u32 blit_vertex{}, blit_fragment{}, untile_fragment{}, merge_vertex{}, merge_fragment{};
    std::vector<SgxdRecipeDraw> untile_draws;
    std::vector<SgxdRecipeDrawPrograms> untile_programs;
    // StreamOut's target: one tile, which the render never writes.
    Data stream_target{};
    // StreamOut's queue, at most StreamMaxDraws a render: a render of 247 A0 draws stopped its TA;
    // up to 64 ran.
    static constexpr u32 StreamMaxDraws = 64;
    struct StreamDraw {
        SgxdRecipeDrawPrograms programs;
        u32 count;
    };
    std::array<std::vector<StreamDraw>, 2> stream_draws;
    void StreamRender(std::vector<StreamDraw>& queued, bool wait);
    // ETC1A4's alpha palette for the merge: entry n is 17n in alpha over white (the AND keeps
    // the colour), from the texture block at the first ETC1A4.
    Data merge_palette{};
};

} // namespace SgxdRenderer
