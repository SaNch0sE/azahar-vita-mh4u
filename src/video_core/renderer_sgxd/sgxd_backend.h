// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <deque>
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
#include "video_core/renderer_sgxd/untile_geometry.h"

namespace SgxdRenderer {

/// Prints the kernel's printf text (sgxd's, azaharnative's, Sony's modules') to the app's TTY. The
/// first call starts the capture; no-op off the Vita.
void SgxdDrainKernelLog();

enum class CacheSite : u32 {
    Vertices,       ///< guest vertices in FCRAM, cleaned before the TA fetches them
    Screens,        ///< a guest framebuffer in FCRAM, cleaned before the present samples it
    DmaSource,      ///< a DMA's source in FCRAM, cleaned before the PTLA reads it
    DmaDest,        ///< a DMA's destination in FCRAM, before and after the PTLA writes it
    Textures,       ///< a guest texture in FCRAM, cleaned before the untile reads it
    Other,
    Count,
};

/// sgxdCacheRange, timed for `perflog`; every CPU cache operation of the renderer goes here.
int SgxdCacheRange(const void* cpu, u32 bytes, u32 op, CacheSite site);
/// An operation not needed because the range is uncached for the CPU (guest VRAM, or FCRAM
/// pages moved to CDRAM): counted for `perflog`, next to the ones done.
void SgxdCacheSkipped(CacheSite site, u32 bytes);
/// Sleeps until the GPU's interrupt brings the async chain to `fence` (the live backend's
/// RenderWait, on the render thread).
void SgxdWaitChain(u32 fence);
#ifdef VITA_DIAGNOSTICS
struct SgxdCacheTimes {
    u64 us;
    u64 bytes;
    u32 calls;
    struct Site {
        u64 us;
        u64 bytes;
        u32 calls;
        u64 skipped_bytes;
        u32 skipped_calls;
    };
    Site sites[static_cast<u32>(CacheSite::Count)];
};
SgxdCacheTimes SgxdTakeCacheTimes();
#endif

/// How a picture holds its pixels: stored through the PBE's pack, bpp bytes a pixel, read back
/// through the texture unit's code and swizzle, converted or raw. The default is U8888 read raw
/// (an RGBA8 buffer's); other formats keep the buffer's own (RasterizerSgxd::PictureFormatFor).
struct SgxdPictureFormat {
    u32 pack = SGXD_RECIPE_PACK_U8888;
    u32 code = 0x0c;
    u32 swizzle = 0;
    u32 bpp = 4;
    bool converted = false;
};

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
 * Passes render into a colour buffer's picture (PicaTarget::picture), or into guest memory
 * itself where the buffer has none (PicaTarget); the depth stores they keep come from a second
 * block. Nothing the GPU runs is waited
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

    /// The USE code base a placed program's DOUTU names (PlaceCode's offsets are from it):
    /// 2 for the vertex data master (cb 0), 3 for the pixel (cb 1). Both are the shared block's
    /// code heap (sgxd_recipe_code_heap); 0 and 1 stay the recipe's own CV and CF.
    static constexpr unsigned HeapCodeBase(unsigned cb) {
        return cb + 2;
    }

    /// The code heap's size: the shared block's CDRAM holds its first megabyte or more, uncached
    /// main memory mapped after it the rest (DOUTU reaches 8 MiB from the code base).
    static constexpr u32 CodeBytes = 4u << 20;
    /// USSE code: its offset from code base HeapCodeBase(cb) (cb 0 vertex, 1 pixel).
    u32 PlaceCode(unsigned cb, std::span<const u64> words);
    /// The code heap's bytes free now: its free ranges and its unused end, once the code given
    /// back whose passes are done is free again.
    u32 CodeFree();
    /// Code no draw will run again (a program its second tier replaced): its bytes are PlaceCode's
    /// again once every pass begun so far has finished; the kicks invalidate the code caches.
    void FreeCode(u32 offset, u32 bytes);
    /// PlaceCode for a program of several phases: each phase's opening PHAS but the last's (an
    /// immediate PHAS at phase_starts[k]) gets the next phase's execution address and temps.
    u32 PlaceCodePhased(unsigned cb, std::vector<u64> words, std::span<const u32> phase_starts,
                        u32 temps);
    /// A per-draw PDS program placed in the flipping half (until that half is reused, at a pass
    /// start: OpenPass), found by its generator's inputs: built only when no program with the
    /// same inputs is placed there yet. Each takes sgxd_program's generator's arguments.
    struct PdsRef {
        u32 va;
        u32 units;
    };
    /// fresh: a fetch reads pass data allocated for this draw, so no program placed before has
    /// these addresses (the geometry draws' stages): no lookup by them.
    PdsRef VertexPds(const SgxdVertexFetch* fetch, u32 count, u32 doutu0, bool per_instance,
                     bool fresh = false);
    /// sa_address: pass data allocated for the draw (never looked up), or with sa_words 0 any.
    PdsRef SecondaryPds(u32 sa_address, u32 sa_words, u32 doutu0);
    PdsRef BlitPixelPds(u32 doutu0, const u32 tex[4]);
    PdsRef ScreenPixelPds(u32 doutu0, const u32 tex[4]);
    PdsRef MergePixelPds(u32 doutu0, const u32 tex0[4], const u32 tex1[4]);
    /// sgxd_program_pixel_pds: a PICA fragment program's iterators, no texture reads issued.
    PdsRef PixelPds(u32 doutu0, const u32* iterators, const u32* words, u32 count);
    /// sgxd_program_pica_pixel_pds: a PICA fragment program's iterators and the texture reads
    /// its PDS issues.
    PdsRef PicaPixelPds(u32 doutu0, const u32* iterators, const u32* words, u32 count,
                        const SgxdTextureIssue* issues, u32 issue_count);

    struct Data {
        u8* cpu;
        u32 gpu;
    };
    Data AllocData(u32 bytes, u32 align = 16);
    [[nodiscard]] u32 DataLeft() const {
        const u32 at = (data_used + 15) & ~15u;
        return at < data_end ? data_end - at : 0;
    }
    /// Counts the passes opened: an AllocData block holds while it stays the same. A pass
    /// parked and the one beside it share it: either takes the other's blocks, and both rooms
    /// are sent with the later of the two fences (CloseData).
    [[nodiscard]] u32 DataOpens() const {
        return data_opens;
    }

    /// Memory for width x height samples of bpp bytes (32-bit: a depth store), rows width apart;
    /// the first fit of what FreeSurface gave back. `upscaled`: a picture's, from the textures
    /// pool (AllocTexture), where a surface the block has no room left for comes from as well.
    Data AllocSurface(u32 width, u32 height, bool upscaled = false, u32 bpp = 4);
    /// Memory AllocSurface gave, no longer used (a depth buffer the guest reshaped), after its
    /// last read: the same sizes.
    void FreeSurface(const Data& surface, u32 width, u32 height, u32 bpp = 4);
    /// The LUT texture's rows, in the shared block: Pica::Shader::LUT_LF_ROWS rows of 256 F32F32
    /// texels, the rasterizer's to manage.
    [[nodiscard]] Data LutRows() const;
    /// The proctex LUT textures' rows, after the LF rows: Pica::Shader::LUT_RG_ROWS rows of 256
    /// F32F32 texels (value, delta) and Pica::Shader::LUT_RGBA_ROWS rows of 256 F16F16F16F16.
    [[nodiscard]] Data LutRgRows() const;
    [[nodiscard]] Data LutRgbaRows() const;
    /// The LF rows' lighting values as F16, Pica::Shader::LUT_LF_ROWS rows of 256, row for row
    /// (the lighting's filtered LUT reads).
    [[nodiscard]] Data LutLfHalfRows() const;

    using PictureFormat = SgxdPictureFormat;

    /**
     * A guest colour buffer a pass renders into, where it is (docs/sgxd/pbe.md): width x height
     * pixels of bpp bytes at its PICA address, in the PICA's 8x8 tiles, the format as the PBE's
     * pack (SGXD_RECIPE_PACK_*). The pass draws its transpose, height x width under the recipe for
     * that size: x is the guest's row, y its column. Its end of tile (sgxd_recipe_pica_eot) stores
     * every tile it renders into the guest buffer as the PICA lays it out, or with a picture
     * (below) into the picture; a tile no draw touches keeps the bytes it had.
     */
    struct PicaTarget {
        u32 gpu;
        u32 width, height;
        u32 pack;
        u32 bpp;
        // The part of the transpose stored (SgxdPicaStore's x0..x1, y0..y1); x1 0 for all of it.
        u32 x0{}, x1{}, y0{}, y1{};
        // Drawn into a picture at picture_quarters / 4 (4 to 8): the pass is height x width
        // scaled, each tile stored whole into picture as LINEAR rows of picture_format,
        // picture_stride pixels apart; the guest's buffer is left to DownscalePicture. 0 for none.
        // Widescreen: the pass and picture are picture_margin rows wider each side; picture_widen
        // grows the part's columns by the margin too.
        u32 picture{}, picture_stride{}, picture_quarters{}, picture_margin{};
        bool picture_widen{};
        // With a picture: its pass's samples a pixel, 1 or 2 (2x MSAA, sgxd_recipe_samples): a
        // 2x pass's tiles store each pixel's samples averaged into the picture.
        u32 picture_samples{1};
        PictureFormat picture_format{};
    };
    /// Opens a pass on target. The background is clear (A8B8G8R8) at depth, what a fill left
    /// pending in guest memory. before: as BeginPassLoaded's.
    void BeginPass(const PicaTarget& target, u32 clear, float depth,
                   const std::function<void()>& before = {});
    [[nodiscard]] bool InPass() const {
        return in_pass;
    }
    /// A pass parked beside the open one: its recipe instance, data room and draws kept unsent
    /// while the open pass draws, so draws switching between two targets make a pass a target,
    /// not a pass a switch. ParkPass parks the open pass, SwapParked trades the two, and
    /// ResumeParked reopens the parked one for EndPass to send after the one it sent. The
    /// rasterizer ends both together.
    [[nodiscard]] bool HasParked() const {
        return parked_open;
    }
    /// Whether the open pass may be parked or traded for the parked one: the PDS arena's half
    /// not past its middle, where the next OpenPass flips it (no pass may span a flip: the
    /// programs of the half left are dropped).
    [[nodiscard]] bool CanPark() const;
    void ParkPass();
    void SwapParked();
    void ResumeParked();

    /// PTLA ops complete through sgxd's microkernel (sgxd_ptla_events). PtlaFence is the last
    /// op's completion point.
    [[nodiscard]] u32 PtlaFence() const;
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
    // A send that failed (AZAHAR_ERR_STATE: a fault, or a flip that found the GPU busy): the
    // microkernel's post-mortem and the dumps, whether or not the lists sent have finished;
    // Fault's assert ends it.
    void Stopped(const char* why);
    /// Every list sent so far, and the pending 3D's finish (sent by RenderWait if needed).
    [[nodiscard]] u32 FenceAll() const;
    /// Waits for a fence (RenderFence's, FenceAll's, or a list's count), sending the pending
    /// 3D's finish if the fence takes it; a failed list stops the process as RenderIdle does.
    /// It sleeps on the waker's flag until the GPU's interrupt (sgxd_chain_sleep): the render
    /// thread's own, or the emulation thread's under a hold, while the render thread is parked.
    void RenderWait(u32 fence, const char* why);
    /// A mark covering every pass so far, the open one (and the parked one) included once it
    /// ends; WaitPassMark waits for its passes' 3Ds (the LUT rows they sample). Marks of passes
    /// long gone wait for the oldest pass still known, which ended later.
    [[nodiscard]] u32 PassMark() const {
        return passes + (in_pass ? 1 : 0) + (parked_open ? 1 : 0);
    }
    void WaitPassMark(u32 mark, const char* why);
    /// CPU work on what the GPU writes (a DMA's cached destination): run once every list sent
    /// so far has completed, before the guest's interrupt for it (InterruptPoll, InterruptWait)
    /// or a wait for everything. RetireAll waits for and runs every one.
    void AfterGpu(std::function<void()> action);
    /// A remap list (sgxd_remap_submit): the GPU stores each pte[k] at pte_va[k] in its queue's
    /// order, after the queued untiles and the pending 3D, then invalidates its MMU. At most
    /// SGXD_REMAP_MAX.
    void RemapPages(const u32* pte_va, const u32* pte, u32 count);
    void RetireAll(const char* why);
    [[nodiscard]] bool RetirePending() const {
        return !retire.empty();
    }
    /// Hands a pending 3D its finishing list without waiting for it, if something waits on it
    /// (RenderFence): after each GPU op (RasterizerInterface::GpuIdle). Otherwise the next
    /// pass's list finishes it.
    void RenderFinish();
    /// A PTLA fill or copy is about to touch GPU addresses [lo, hi): the queued untiles
    /// (QueueUntile) are sent, and the pending 3D's finish goes in the queue first if it writes
    /// or samples any of them. Nothing waits.
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
    /// As InterruptPoll, without sending a pending 3D: whether the fence's work is done (the
    /// render thread inside a list, where the pass may still go on).
    [[nodiscard]] bool InterruptReached(const InterruptFence& fence);
    /// Waits for the fence's work; a failed render stops the process as RenderIdle does.
    void InterruptWait(const InterruptFence& fence);
    /// The guest waits for the fence's work: a pending 3D it takes gets its finishing list now,
    /// behind the lists queued, as InterruptWait sends it, without waiting.
    void InterruptKick(const InterruptFence& fence);
    /// The render thread's sleep until the fence's work is done (sgxd_wake_arm): false when it
    /// is done already. With a 3D held back (the fence one past the lists sent), the wake comes
    /// once the lists before it are done, for InterruptPoll to send it. Disarm after the sleep.
    [[nodiscard]] bool InterruptArm(const InterruptFence& fence);
    void InterruptDisarm();
    /// sgxdWaker: the render thread's event flag, whose bits sgxd sets when an armed sleep's
    /// work is done; a negative flag stops it.
    void InterruptWaker(s32 flag, u32 bits);
    /// The render thread's sleep until the window's last present is drawn (its chain fence):
    /// false when it is drawn already. Disarm after the sleep.
    [[nodiscard]] bool FlipArm(u32 fence);
    /// The window's present waiting for its 3D (pending: its chain fence), drawn and held for
    /// its paced flip (due_in, us), and its flip. A RenderWait for later work sleeps until that
    /// 3D or that time first and flips it.
    void SetFlipHook(std::function<bool(u32*)> pending, std::function<s64()> due_in,
                     std::function<void()> flip) {
        flip_pending = std::move(pending);
        flip_due_in = std::move(due_in);
        flip_done = std::move(flip);
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
    /// Renders the pass as an async list: returns once the TA is kicked, with the TA's and the
    /// 3D's microseconds of the render before.
    void EndPass(u32* ta_us = nullptr, u32* d3_us = nullptr);

    /// Guest textures' host copies (RasterizerSgxd's texture cache): 4 KiB multiples, the smallest
    /// free range that fits; MakeRoom drops textures for room, else the overflow. GPU writes to
    /// the range follow its earlier users in queue order; a CPU write waits for `ready` first (the
    /// fence of everything sent before the range was given back, 0 for none).
    Data AllocTexture(u32 bytes, u32* ready = nullptr);
    /// A host copy the texture cache may drop to make room: its range, and the key that names
    /// its texture to the drop (a texture with an alpha copy lists both ranges under one key).
    struct Droppable {
        u32 gpu, bytes;
        const void* key;
    };
    /// The rasterizer's cache for MakeRoom: `list` gives the copies it may drop (none the open
    /// pass reads), `drop` drops a texture by its key and gives its ranges back at once.
    void SetTextureDropper(std::function<void(std::vector<Droppable>&)> list,
                           std::function<void(const void*)> drop) {
        droppable_list = std::move(list);
        droppable_drop = std::move(drop);
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
    /// FreeTexture for a host copy the open pass may sample: the range is given back once the
    /// pass's render is sent (EndPass), so whatever takes it next is written behind that 3D.
    /// Outside a pass, FreeTexture.
    void FreeTextureAfterPass(const Data& texture, u32 bytes);

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
    /// Opens a pass on target that keeps its picture instead of a clear: its first draws read
    /// guest (the target's own bytes) texel for pixel, a band of tile rows a draw (DrawTiled).
    /// before runs once the pass is open, ahead of the load.
    void BeginPassLoaded(const PicaTarget& target, float depth, const UntileSource& guest,
                         const std::function<void()>& before = {});
    /// The same for a target drawn scaled (PicaTarget::picture): its first draws read the
    /// scaled picture texel for pixel, or with magnified the guest's 1x bytes, each texel
    /// magnified to the scale (nearest).
    void BeginPassLoadedScaled(const PicaTarget& target, float depth, const UntileSource& guest,
                               bool magnified, const std::function<void()>& before = {});
    /// A target drawn scaled whose first draw reads another picture of the same buffer (`source`,
    /// rows of source_stride texels, source_rows of them, at a smaller scale) over all of it,
    /// through the bilinear filter: the buffer's colour as that picture holds it, magnified.
    void BeginPassFromPicture(const PicaTarget& target, float depth, u32 source, u32 source_stride,
                              u32 source_rows, const PictureFormat& source_format);
    /// In a pass BeginPassLoadedScaled opened from its picture: rows (the guest's whole strips
    /// of the buffer from its row first_row on) drawn over the load from guest memory, each
    /// texel magnified to the scale (nearest): the picture's rows older than guest memory. The
    /// pass's footprint is the guest's bytes then (the first call's, the later ones' joined).
    void LoadGuestRowsScaled(const PicaTarget& target, const UntileSource& rows, u32 first_row,
                             bool first);
    /// One pass storing a scaled pass's picture into target's guest buffer (its part, or all) at
    /// 1x: at 2x each pixel averages its 2x2 block, at other scales a bilinear read at the pixel's
    /// centre; nearest: the texel the centre falls in (depth words). quad_to_part: the quad covers
    /// the part alone, the other tiles stay EMPTY. rows: picture rows a pixel row (2 for a 2x
    /// store's words). Texels read as target.picture_format says.
    void DownscalePicture(const PicaTarget& target, bool nearest = false,
                          bool quad_to_part = false, u32 rows = 1);
    /// One pass copying width x height pixels of a LINEAR picture in format (rows stride
    /// apart), from its column source_x on, into dest, LINEAR rows width apart stored through
    /// dest_pack: a screen picture kept for the present. scale_x, scale_y (1 or 2): each pixel
    /// the average of that many of the source's along and across its rows (BlitPrograms').
    void CopyPicture(u32 dest_gpu, u32 dest_pack, u32 source_gpu, u32 width, u32 height,
                     u32 stride, const PictureFormat& format, u32 source_x = 0, u32 scale_x = 1,
                     u32 scale_y = 1);
    /// Opens a pass whose first draw writes every pixel: no clear and no load.
    void BeginPassOver(const PicaTarget& target, float depth,
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
    /// stored there at its end, or into store_to, of store's shape (ZLS, sgxd_recipe_set_zls_to).
    /// OpenPass turns it off.
    void SetDepthStore(const DepthStore& store, DepthLoad load,
                       const DepthStore* store_to = nullptr);
    /// from's samples x0 <= x < x1, y0 <= y < y1 (x in 8s) into to, of the same shape, depth and
    /// stencil, through the PTLA, behind every 3D sent before (sgxd_ptla's queued copies).
    void CopyDepthPart(const DepthStore& from, const DepthStore& to, u32 x0, u32 x1, u32 y0,
                       u32 y1);
    /// A store's depth and stencil into the guest's buffer as the PICA lays it out, in two renders
    /// (no pass open): the ZLS stores the samples as I24ZI8S words (D16: I16ZI16Z) into words,
    /// then a pass copies the words into target as U8888 (D16: U16_RAW).
    /// At 1x (quarters 4, no margin) words are the store's size and the copy is texel for pixel;
    /// scaled or widescreen, words are twice that, the first render on their second half, and each
    /// pixel takes the word its centre falls in. A 2x store (pixel_samples 2) converts as a store
    /// of its samples, each pixel taking one sample's word.
    /// A target with a part (x1 non-zero) writes that part alone; the other tiles stay EMPTY.
    /// `d16`: the guest's buffer is D16, else D24S8 or D24 (target's pack U8888, or U888 for D24).
    void DepthToGuest(const DepthStore& store, const Data& words, const PicaTarget& target,
                      bool d16, u32 quarters, u32 margin, u32 pixel_samples);
    /// DepthToGuest's first render alone: the store's I24ZI8S words (D24S8's) into words, every
    /// tile, the pass's pixels on the words' second half; nothing into guest memory. A texture
    /// copied from the words takes them (RasterizerSgxd::WriteDepthWords).
    void DepthToWords(const DepthStore& store, const Data& words);
    struct PassRect {
        u32 x0, x1, y0, y1;
    };
    /// A quad over the open pass's whole target that the ISP rejects (DCMP NEVER): every tile
    /// holds an object, so none stays EMPTY and skips its ZLS load and store.
    void TouchAllTiles() {
        TouchTiles(nullptr);
    }
    void TouchTiles(const PassRect* rect);
    /// Whether an opaque draw that keeps what is under it (colour mask without alpha, no blend)
    /// can go in as opaque: the ISP removes hidden opaque objects before shading, and a hidden
    /// object that writes the pixel would not write the bytes this draw keeps. True with no such
    /// object pending, or after a flush object (a translucent full-target quad that keeps the
    /// pixel, so every tile shades what is pending), at most `budget` a pass.
    [[nodiscard]] bool OpaqueKeepingAlpha(u32 budget);
    /// hang_test's draw: a translucent quad over the whole target whose pixel program branches
    /// to itself, so the pass's 3D never ends and the microkernel's wait for it runs out.
    void DrawHang();
    /// Draw for such a draw: an object that writes no alpha, so it leaves the pending state as
    /// it was.
    void DrawKeepingAlpha(const SgxdRecipeDraw& draw, const SgxdRecipeDrawPrograms& program);
    [[nodiscard]] u32 TakeAlphaFlushes() {
        return std::exchange(alpha_flushes, 0);
    }
    /// The open pass's target outside [x0, x1) x [y0, y1) masked from here on: mask updates
    /// clear the mask there (SGXD_RECIPE_ISP_MASK_UPDATE), so the ISP rejects every later draw's
    /// samples there before any shading, and they keep what the pass loaded or cleared.
    void MaskOutside(u32 x0, u32 x1, u32 y0, u32 y1, bool part_tiles = false);
    /// store's samples as a clear leaves them, depth z and stencil, through the PTLA: a tile no
    /// primitive touches neither loads nor stores.
    void FillDepthStore(const DepthStore& store, float z, u32 stencil);
    /// The same over the store's samples x0..x1 (in 4s) by y0..y1: the depth with z, the
    /// stencil with stencil, each kept when null.
    void FillDepthStorePart(const DepthStore& store, u32 x0, u32 x1, u32 y0, u32 y1,
                            const float* z, const u32* stencil);
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
     *
     * transposed: dest is the image's transpose, h texels a row (dest_stride apart) and w rows,
     * as a pass draws a PICA target (PicaTarget): a guest depth buffer into its depth store.
     * Not with ETC1 or swizzled.
     */
    void Untile(const UntileSource& source, u32 dest_gpu, u32 dest_stride, bool swizzled = false,
                bool transposed = false);
    /// A guest depth buffer's words (D24S8's or D24's, w x h) untiled into the depth store of a
    /// scaled, widescreen or 2x pass, for DepthLoad::Guest: Untile's transpose magnified,
    /// quarters / 4 a side, margin columns of the far plane either side. Each sample takes the
    /// word of the pixel its centre falls in, so DepthToGuest writes back each pixel's own word.
    void UntileDepthScaled(const UntileSource& source, const DepthStore& store, u32 quarters,
                           u32 margin, u32 samples);
    /**
     * A 2D texture's levels untiled as the texture unit's LINEAR chain at dest_gpu: level l is
     * w_l x h_l A8B8G8R8 texels in rows of w_l, the levels back to back from level 0
     * (docs/sgxd/tsp.md), each in Untile's row order. source is level 0, and the
     * guest's levels follow it; every level's sides are 8 or more. Not ETC1 or ETC1A4.
     *
     * The levels share one target, rows max(w, 32) texels apart, where a level narrower than
     * that puts that many texels of its rows on each target row: one side render for the whole
     * chain, or a second for the levels after the first where the target would pass
     * UntileSide rows (a 1024x1024 chain, or a 32-wide one 1024 tall).
     */
    void UntileChain(const UntileSource& source, u32 levels, u32 dest_gpu);
    /// The bytes UntileChain writes from dest_gpu: the chain, and the PBE's whole tiles past it,
    /// at `bytes` a texel (4, or 1 for a byte copy).
    static u32 ChainBytes(u32 width, u32 height, u32 levels, u32 bytes);
    /// A queued untile's host copy (QueueUntile): its chain, and a split ETC1A4's A8 alpha
    /// chain (alpha_bytes 0 for none), each a range of the texture pool for FreeTexture.
    struct UntileCopy {
        Data colour;
        u32 colour_bytes;
        Data alpha;
        u32 alpha_bytes;
    };
    /**
     * A 2D texture's levels for the texture unit (source is level 0, the guest's levels follow
     * it, every level's sides 8 or more), queued: returns where the copy will be, which the
     * open pass's draws may sample at once. FlushUntiles sends the queue's side renders, before
     * the open pass's own (EndPass) and before a PTLA op (RenderBefore); out of a pass, the
     * caller flushes at once. By format:
     * - ETC1: the texture unit's SWIZZLED ETC1 (0x84) chain, still compressed: each block moved
     *   to its SWIZZLED place with its two words swapped (docs/sgxd/tsp.md), rendered into a
     *   64-bit TWIDDLED target (Etc1TargetFor).
     * - ETC1A4 up to Etc1A4MaxWidth wide: split into an ETC1 colour chain and an A8 LINEAR
     *   alpha chain the draws join, through a gather of raw words (Etc1A4GatherVertices).
     * - Any other: the LINEAR A8B8G8R8 chain UntileChain writes. A chain past one render's
     *   UntileSide rows is untiled at once into a copy of its own.
     * Block rows and texel rows stay in the guest's order: the draws read at (s, 1 - t).
     * The queue's untiles of a kind share one side render (untile_geometry.h), each copy a slot
     * of its target; a copy that does not fit flushes the queue first.
     */
    UntileCopy QueueUntile(const UntileSource& source, u32 levels);
    /// Sends the queued untiles' side renders, the gather first (the ETC1 and alpha renders
    /// read it): one render for each batch the queue holds.
    void FlushUntiles();
    /**
     * An ETC1 cube's faces (faces: each face's guest address, its chain of `levels` after it) as
     * the texture unit's ETC1 CUBE at colour_gpu, still compressed: each face's chain moved as
     * QueueUntile moves a 2D square chain, into its slot CompressedCubeFaceStride apart. With
     * `alpha` the faces are ETC1A4, up to Etc1A4MaxWidth wide: the colour words gathered first,
     * and the alpha an A8 CUBE at alpha_gpu, each face's SWIZZLED chain rendered TWIDDLED
     * (CubeAlphaVertices). Side renders a face at a time, in address order: a face's whole tiles
     * past its chain land where the next face's render writes. Not queued.
     */
    void UntileCube(const std::array<u32, 6>& faces, u32 width, u32 levels, bool alpha,
                    u32 colour_gpu, u32 alpha_gpu);

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

    /// The blit's draw programs: one quad sampling source (A8B8G8R8, width x height) texel for
    /// pixel into a target of that size, point sampled (TouchAllTiles).
    /// source_stride: the source's rows apart in texels, 0 for width.
    /// scale_x, scale_y (1 to 2): the source is width * scale_x x height * scale_y, read bilinear
    /// at each pixel's centre (the display transfer's ScaleX and ScaleXY); nearest: the texel the
    /// centre falls in.
    /// rect: the triangles over that part of the target alone.
    /// format: the source's texels (PictureFormat): U8888 or 0x09 U16 raw, or converted by the
    /// screen program.
    SgxdRecipeDrawPrograms BlitPrograms(const Data& source, u32 width, u32 height,
                                        u32 source_stride = 0, float scale_x = 1,
                                        float scale_y = 1, u32 source_x = 0,
                                        bool nearest = false, const PassRect* rect = nullptr,
                                        const PictureFormat& format = {});
    /// A display transfer from a PICA-tiled picture in guest memory: one pass whose draws read
    /// it a band of tile rows at a time (DrawTiled), into dest_gpu as width x height pixels of
    /// pack (an SGXD_RECIPE_PACK_*), rows width apart, the source's top-left width * scale_x x
    /// height * scale_y, averaged as BlitPrograms' scale. flip: the output's rows in the other
    /// order (the transfer's flip_vertically).
    void TransferFromGuest(const UntileSource& source, u32 dest_gpu, u32 width, u32 height,
                           u32 pack, u32* ta_us = nullptr, u32* d3_us = nullptr, u32 scale_x = 1,
                           u32 scale_y = 1, bool flip = false);
    /// A display transfer from a pass's picture (PicaTarget::picture: the buffer's transpose at
    /// quarters / 4, LINEAR rows in format stride pixels apart, margin rows past each side) instead
    /// of the guest's bytes: as TransferFromGuest, from the buffer's row first_row on. Each output
    /// pixel reads the picture at its centre (one texel at 1x, bilinear scaled).
    void TransferFromPicture(u32 picture, u32 stride, u32 rows, u32 quarters, u32 margin,
                             u32 first_row, u32 dest_gpu, u32 width, u32 height, u32 pack,
                             u32 scale_x, u32 scale_y, bool flip, const PictureFormat& format,
                             u32* ta_us = nullptr, u32* d3_us = nullptr);
    /// A texture's copy from a picture (TransferFromPicture's source): a side render of the
    /// buffer's width x height texels from row first_row on, at the picture's scale, into LINEAR
    /// U8888 rows dest_stride texels apart at dest_gpu, point sampled: a scaled pass's colour stays
    /// at its scale, and depth words do not average.
    void TextureFromPicture(u32 picture, u32 stride, u32 rows, u32 quarters, u32 margin,
                            u32 first_row, u32 dest_gpu, u32 dest_stride, u32 width, u32 height,
                            const PictureFormat& format);
    /// The same into a PICA-tiled picture: one pass on target as a PICA colour buffer (as
    /// BeginPassLoaded opens it), the source read as its load reads, scaled.
    void TransferTiledFromGuest(const PicaTarget& target, const UntileSource& source, u32 scale_x,
                                u32 scale_y, u32* ta_us = nullptr, u32* d3_us = nullptr);
    /// A display transfer from a linear picture into a PICA-tiled one (input_linear): one pass
    /// on target as a PICA colour buffer, one quad reading source as a LINEAR texture of its
    /// format (rows source.width texels apart) texel for pixel, converted by the screen program.
    /// flip (the transfer's flip_vertically): the source's first row is the target's last.
    void TransferLinearToTiled(const PicaTarget& target, const UntileSource& source, bool flip,
                               u32* ta_us = nullptr, u32* d3_us = nullptr);

private:
    enum class Kind : u8 { Linear, Pica, Side };
    struct Recipe {
        SgxdRecipe recipe{};
        u8* memory{};
        u32 va{};
        u32 size{};
        int block{-1};
        int shell_slot{-1}; // its block's slot in the shell's CDRAM, or -1: its own block
        u32 width{}, height{};
        u32 bg_offset{}; // the background colour word and its SA secondary, after the layout
        Kind kind{};
        u32 samples{1}; // samples a pixel (sgxd_recipe_samples)
        u32 instance{};   // up to MaxInstances of each size, the pending pass's among them
        u32 fence{};      // the list that completes its last render's 3D
    };
    /// A PICA recipe's end-of-tile program (sgxd_recipe_pica_eot), from bg_offset: rewritten at
    /// each pass on the recipe, before its 3D kick's USE code-cache invalidate.
    static constexpr u32 PicaEotOffset = 0x400;
    /// A size's instances at most: a render's recipe is the GPU's until its 3D completes, and the
    /// CPU writes the next render's into another.
    static constexpr u32 MaxInstances = 16;
    /// A recipe's block at most (layout and background page; the 1024x1024 side recipe's layout
    /// is 430 KiB), and the CDRAM left free beside one before RecipeFor makes another instance:
    /// the kernel's free figure is not one block's worth.
    static constexpr u32 RecipeBlockSize = 512u << 10;
    static constexpr u32 RecipeCdramMargin = 1u << 20;
    /// SceShell's 16 MiB of CDRAM, the app's while sgxd holds the GPU (azaharShellCdram): the
    /// recipes' blocks in slots of RecipeBlockSize, before the game's 112 MiB.
    static constexpr u32 ShellCdramSize = 16u << 20;
    static constexpr u32 ShellSlots = ShellCdramSize / RecipeBlockSize;
    u8* shell_cdram{};
    u64 shell_slots_used{}; ///< a bit a slot
    /// The slots' GPU addresses, a megabyte apart from here, mapped once at the start: a recipe
    /// made in a slot maps nothing, so the GPU never idles for one.
    u32 shell_slots_va{};
    [[nodiscard]] u32 ShellSlotVa(u32 slot) const {
        return shell_slots_va + slot * (1u << 20);
    }
    void FreeRecipeBlock(const Recipe& r);

    /// The recipe for a width x height target of kind, made at its first use. A PICA recipe's
    /// pixel event program DOUTAs IR0 to its end of tile (sgxd_recipe_pica_pixev).
    Recipe& RecipeFor(u32 width, u32 height, Kind kind, u32 samples = 1);
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
        bool raw64 = false; ///< a 64-bit texel copied raw (sgxd_program_raw64_*), not converted
        u32 vertices = 0;   ///< the draw's own vertex array (GPU address); 0: the render's
        /// A batch member's quads, put for a view_width x view_height target whose (0, 0) is
        /// this render's pixel (view_x, view_y); view_width 0: put for this render's target.
        u32 view_x = 0, view_y = 0, view_width = 0, view_height = 0;
    };
    /// A render of its own under the side recipe, sent ahead of the open pass's: the quads in
    /// vertices into a width x height target of pack at dest_gpu, rows dest_stride pixels apart.
    /// merge: PutMergeQuad's quads, a coordinate for each texture. twiddled: the PBE's TWIDDLED
    /// layout.
    void SideRender(const Data& vertices, std::span<const SideDraw> list, u32 dest_gpu,
                    u32 dest_stride, u32 width, u32 height, bool merge = false,
                    bool twiddled = false, u32 pack = SGXD_RECIPE_PACK_U8888,
                    bool double_pixels = false);
    PdsRef TermPds(const SgxdRecipe& recipe);
    /// QueueUntile's batches, each one side render's target: LINEAR U8888 chains in rows of
    /// 256, 512 or UntileSide texels (ChainBatchStride), the byte copies' U8 chains the same in
    /// bytes, TWIDDLED ETC1 chains in squares of 32, 64, 128 or 256 blocks a side, A8 alpha
    /// chains in rows of AlphaBatchStride bytes. A batch's block is taken from the texture pool
    /// at its first member, a slot (in units: rows, or squares) at a time.
    enum UntileBatchKind : u32 {
        BatchChain256,
        BatchChain512,
        BatchChainWide,
        BatchBytes256,
        BatchBytes512,
        BatchBytesWide,
        BatchEtc1, // + log2(height / 32)
        BatchAlpha = BatchEtc1 + 4,
        UntileBatchKinds,
    };
    struct UntileBatch {
        Data block{};
        u32 capacity = 0; ///< its units
        u32 used = 0;     ///< the units its members' slots take
        u32 draws = 0;
    };
    /// A queued untile: its source, its slot's first unit in its chain's batch, and a split
    /// ETC1A4's first row in the A8 batch and in the gather.
    struct Geometry;
    struct UntileMember {
        UntileSource source;
        u32 levels;
        u32 batch;
        u32 at;
        u32 alpha_at;
        u32 gather_row;
        const Geometry* geometry = nullptr;
        const Geometry* gather = nullptr;
        const Geometry* alpha = nullptr;
    };
    std::array<UntileBatch, UntileBatchKinds> untile_batches{};
    std::vector<UntileMember> untile_members;
    u32 gather_rows = 0;  ///< the queued gathers' rows
    u32 gather_draws = 0; ///< and draws
    bool flushing_untiles = false;
    std::vector<std::pair<Data, u32>> pass_frees;
    /// A unit's bytes, a batch's most units (UntileSide rows, the target's width in squares),
    /// and the units a slot rounds up to (a 4 KiB multiple).
    static u32 UnitBytes(u32 kind);
    static u32 MaxUnits(u32 kind);
    static u32 SlotUnits(u32 kind);
    /// A texture pool range back on the free list, with no finish of the pending 3D: for
    /// ranges only side renders write next, whose lists finish that 3D before their own 3D
    /// kick (sgxd_session_render_overlap).
    void GiveTexture(const Data& texture, u32 bytes);
    /// The textures pool past its block: CDRAM while the game's has room, then uncached main
    /// memory. Chunks of OverflowChunkSize or more, mapped one after another on first need and
    /// kept until the backend's end; each is shared out on its own (their CPU sides are apart).
    Data AllocOverflow(u32 bytes, u32* ready);
    /// The ETC1 render's pixel primary: the texel's two words raw into pa0 and pa1.
    PdsRef Raw64PixelPds(u32 doutu0, const u32 tex[4]);
    void UntileEtc1Blocks(const UntileSource& source, u32 dest_gpu, u32 dest_stride,
                          bool swizzled);
    void UntileEtc1A4Merge(const UntileSource& source, u32 colour_gpu, u32 colour_stride,
                           u32 dest_gpu, u32 dest_stride, bool swizzled);
    /// The recipe's target, and the pass state reset: a linear target at target_gpu (rows
    /// width apart), or with pica that guest buffer (width and height its transpose's).
    void OpenPass(u32 target_gpu, u32 width, u32 height, u32 pack, u32 clear, float depth,
                  const PicaTarget* pica = nullptr);
    /**
     * Draws into the open pass that read src, a PICA-tiled picture, texel for pixel (scaled:
     * each target pixel the average of scale_x x scale_y texels, bilinear between them), from
     * the target's top-left. Untile's bands: a band of tile rows is an 8-texel-wide SWIZZLED
     * texture whose texel (sx, sy) is PICA pixel (sy % w, 8 (band row + sy / w) + sx), one
     * draw a band, a quad a tile row. transposed: target x is the picture's row and y its
     * column (a PICA target's load), which is the texture's own orientation; otherwise x is the
     * column (a display transfer's output). Opaque, DCMP ALWAYS, no depth write. magnify
     * (quarters, 5 to 8; unscaled only): each texel magnify / 4 pixels a side, nearest (a
     * scaled pass's load of the guest's 1x bytes). clip: the tiles drawn (SGXD_RECIPE_CLIP), 0
     * for all of the target's. flip (not transposed): the target's rows in the other order.
     */
    void DrawTiled(const UntileSource& src, bool transposed, u32 scale_x = 1, u32 scale_y = 1,
                   u32 magnify = 4, u32 offset_x = 0, u32 clip = 0, bool flip = false);

    s32 waker_flag = -1;
    u32 waker_bits = 0;
    std::function<bool(u32*)> flip_pending;
    std::function<s64()> flip_due_in;
    std::function<void()> flip_done;

    std::vector<std::unique_ptr<Recipe>> recipes;
    Recipe* current{}; // the open pass's
    u32 next_va{};     // where the next recipe's block goes

    u8* shared{};      // the shared block: PDS arena, pass data
    u32 shared_size{};
    int shared_block{-1};
    u8* surfaces{};    // the depth stores' block
    u32 surfaces_size{};
    u32 surfaces_spilled{}; ///< surfaces the block had no room for (AllocSurface), for its log
    u32 surfaces_used{};
    int surfaces_block{-1};
    u8* textures{};    // the guest textures' host copies
    u32 textures_used{};
    /// The most `textures_used` has been: below it a range may have held a texture.
    u32 textures_peak{};
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
    u32 textures_top_fence{};
    struct OverflowChunk {
        u8* cpu;
        u32 gpu;
        u32 size;
        int uid;
        u32 used; ///< its room handed out from the front, as textures_used
        std::vector<FreeRange> free;
        u32 top_fence = 0; ///< as textures_top_fence, above `used`
    };
    std::vector<OverflowChunk> overflow;
    u32 overflow_mapped{}; ///< the chunks' bytes, from the overflow's GPU address
    u32 overflow_logs{};
    u32 make_room_logs{};
    std::vector<FreeRange> surfaces_free;
    /// An untile's vertices by its geometry (GeometryKey): a texture of a size seen before reuses
    /// them. In the textures pool: ETC1 uploads inside a pass filled the pass data.
    std::unordered_map<u64, Data> geometry_vertices;
    /// The cached vertices for key, or `bytes` of new ones with fresh set for the caller to
    /// fill.
    Data GeometryVertices(u64 key, u32 bytes, bool& fresh);
    /// A shape's whole geometry, made once (GeometryFor): its vertices in the textures pool and
    /// the draws that read them (an untile's) or its bands (a tiled picture's). An untile queued
    /// or a picture drawn again builds no list.
    struct Geometry {
        Data vertices;
        std::vector<UntileGeometry::GeometryDraw> draws;
        std::vector<UntileGeometry::Band> bands;
    };
    std::unordered_map<u64, Geometry> geometries;
    /// The geometry under key, made the first time by make(v, geometry), which fills the draws
    /// or the bands and returns the vertex count: with v null for the count, then into the new
    /// vertices, vertex_bytes each. The map is node-based: a geometry's address stays.
    template <typename Make>
    const Geometry& GeometryFor(u64 key, u32 vertex_bytes, Make&& make);

    std::vector<u16> identity;
    std::vector<SgxdRecipeDraw> init_draws;

    u32 code_begin{}; // the code heap in the shared block (code bases 2 and 3)
    u32 code_cdram{}; // its bytes there, to the block's end
    u8* code_main{};  // the rest, in main memory, at GPU BlockVa + shared_size
    int code_main_block{-1};
    u32 code_used{};  // its bytes
    /// Copies code to heap offset `at`, across the CDRAM part's end into main memory.
    void CopyCode(u32 at, const void* words, u32 bytes);
    /// Code FreeCode gave back: waiting for the passes begun before (mark, PassMark's), and
    /// free (by offset, neighbours merged), which AllocateCode takes first.
    struct CodeRange {
        u32 offset, bytes, mark;
    };
    std::vector<CodeRange> code_pending;
    std::vector<CodeRange> code_free;
    /// Where `bytes` of code go: the first free range they fit, else the heap's end; a heap
    /// with neither waits for the code given back longest ago.
    u32 AllocateCode(u32 bytes);
    void ReleaseCode();
    /// Diagnostics: every program PlaceCode placed (bank, offset from the bank's base as the
    /// DOUTU takes it, bytes, place in the heap), written with the heap at a fault.
    struct PlacedCode {
        u32 cb, offset, bytes, at;
    };
    std::vector<PlacedCode> placed_code;
    u32 pds_begin{}, pds_used{}, pds_end{};
    u32 pds_half{};
    u32 pds_window{}; ///< the GPU window the current half's programs are placed under
    u64 pds_flips{};
    /// Each half's last reader: FenceAll when the arena flipped away from it (its programs
    /// are not reused after that).
    std::array<u32, 2> pds_fence{};
    /// Resets every core's data mover, shader units and caches together, the GPU idle: the PDS
    /// forgets the programs it ran (docs/sgxd/pds.md). why: for the stop's message.
    void ResetCores(const char* why);
    void FlipPdsHalf();
    u32 data_begin{}, data_used{}, data_end{};
    u32 data_placed{}; ///< the open pass's room's place among the rooms (data_opens at its open)
    u32 data_peak{}; ///< perflog: the largest pass data logged
    struct DataSite {
        uintptr_t caller;
        u32 bytes;
        u32 count;
    };
    std::array<DataSite, 32> data_sites{};
    u32 data_opens{};
    /// The pass data: a ring of DataSlots x DataSize bytes. A pass's data starts where the last
    /// one's ended (at the ring's start when DataSize does not fit before its end) and may take
    /// DataSize; opening it waits only for the sent passes whose data that room overlaps, and on a
    /// wrap for those in the tail it skips.
    static constexpr u32 DataSlots = 4;
    struct DataSpan {
        u32 begin, end, fence;
    };
    std::deque<DataSpan> data_spans; ///< the sent passes' data, oldest first
    u32 data_next{};                 ///< where the next pass's data starts
    void CloseData(u32 fence);
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
    std::function<void(std::vector<Droppable>&)> droppable_list;
    std::function<void(const void*)> droppable_drop;
    /// With no free range of `bytes`: of the pool's runs of free ranges and droppable copies, the
    /// stretch that holds `bytes` dropping the fewest bytes, its copies dropped (every copy can be
    /// untiled again). False when no run is that long (the open pass's copies and the fixed blocks
    /// are in the way).
    bool MakeRoom(u32 bytes);
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
    void Fault(const Recipe& r, const char* failed);
#ifdef VITA_DIAGNOSTICS
    void WriteCode();
#endif
    /// The flipping half's programs by their generator's inputs (a tag, then the arguments):
    /// open addressing over a power-of-two table. Emptied when the arena flips.
    class PdsIndex {
    public:
        explicit PdsIndex(u32 first_slots = 4096) : slots(first_slots) {}
        /// MurmurHash3's 32-bit mixing, whole or a word at a time: HashStart, HashStep, HashEnd.
        static u32 Hash(const u32* key, u32 words);
        static u32 HashStart(u32 words) {
            return 0x9e3779b9u ^ words;
        }
        static u32 HashStep(u32 h, u32 word) {
            const u32 k = std::rotl(word * 0xcc9e2d51u, 15) * 0x1b873593u;
            return std::rotl(h ^ k, 13) * 5 + 0xe6546b64u;
        }
        static u32 HashEnd(u32 h);
        const PdsRef* Find(const u32* key, u32 words, u32 hash) const;
        void Insert(const u32* key, u32 words, u32 hash, PdsRef ref);
        void Clear();

    private:
        struct Slot {
            u32 hash;
            u32 key_at;
            u32 key_words; ///< 0: empty
            PdsRef ref;
        };
        void Grow();
        std::vector<Slot> slots;
        std::vector<u32> keys;
        u32 used = 0;
    };
    PdsIndex pds_index;
    u32 pds_index_programs{}; ///< programs placed in the flipping half since its flip
    template <typename Build>
    PdsRef KeyedPds(const u32* key, u32 words, Build&& build);
    template <typename Build>
    PdsRef KeyedPds(const u32* key, u32 words, u32 hash, Build&& build);
    u32 AppendPds(const SgxdPdsProgram& program);
    /// A vertex or secondary PDS program as its generator built it once, and where its addresses
    /// are in its words: the program for the same inputs at other addresses is these words with
    /// those changed. Kept for good; the flips do not touch them.
    struct PdsTemplate {
        SgxdPdsProgram program;
        std::array<unsigned, SGXD_PROGRAM_MAX_FETCH> at;
    };
    std::deque<PdsTemplate> pds_templates;
    PdsIndex pds_shapes{256};
    /// The secondary programs placed for recent SA images, a slot by the image's address
    /// (SecondaryPds); emptied when the arena flips.
    struct SecondaryKept {
        u32 address;
        u32 words; ///< 0: none
        u32 doutu0;
        PdsRef ref;
    };
    std::array<SecondaryKept, 16> secondary_kept{};
    /// The template for a key without addresses, built by `build` on a miss; null if it fails.
    template <typename Build>
    PdsTemplate* TemplatePds(const u32* key, u32 words, Build&& build);

    bool in_pass{};
    std::vector<SgxdRecipeDraw> draws;
    std::vector<SgxdRecipeDrawPrograms> programs;
    u32 draw_count{};
    struct CachedRange {
        const u8* cpu;
        u32 bytes;
    };
    std::vector<CachedRange> cached;
    u32 passes{};
    /// The parked pass (ParkPass): the open pass's members above, traded by TradeParked.
    struct ParkedPass {
        Recipe* recipe{};
        Footprint target{}, source{}, zls{};
        u32 data_begin{}, data_used{}, data_end{}, data_placed{};
        std::array<DataSite, 32> data_sites{};
        std::vector<SgxdRecipeDraw> draws;
        std::vector<SgxdRecipeDrawPrograms> programs;
        u32 draw_count{};
        std::vector<CachedRange> cached;
        bool alpha_pending{};
        u32 flushes{};
#ifdef VITA_DIAGNOSTICS
        std::string note;
#endif
    };
    ParkedPass parked;
    bool parked_open{};
    void TradeParked();
    /// The first of two passes ended together: its data's room, which CloseData closes with
    /// the second's fence (either pass's draws may take the other's copies: the caches by
    /// DataOpens).
    struct HeldData {
        u32 begin, end, placed;
    };
    HeldData held_data{};
    bool data_held{};
#ifdef VITA_DIAGNOSTICS
    struct TargetTime {
        u32 passes, draws;
        u64 ta_us, d3_us;
    };
    std::unordered_map<u64, TargetTime> target_times;
    std::string pass_note;
    u32 slow_passes{};
    std::array<u32, static_cast<std::size_t>(FinishWhy::Count)> finishes{};
    u64 target_times_start{};
    void TargetTimes(u32 width, u32 height, u32 ta_us, u32 d3_us);
    std::unordered_map<const char*, std::pair<u32, u64>> wait_sites;
    u64 wait_sites_mark{};
#endif
    void WaitSite(const char* site, u64 before);

    bool blit_ready{};
    u32 blit_vertex{}, blit_fragment{}, untile_fragment{}, merge_vertex{}, merge_fragment{};
    u32 raw64_fragment{}, keep_fragment{};
    /// The open pass holds an opaque object that writes the pixel (alpha included) and that
    /// no flush object has had shaded yet (OpaqueKeepingAlpha), the background object from the
    /// pass's start; the flushes made in the pass.
    bool pass_alpha_pending{};
    u32 pass_flushes{};
    u32 alpha_flushes{};
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
