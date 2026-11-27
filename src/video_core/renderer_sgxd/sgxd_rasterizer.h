// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <atomic>
#include <array>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "common/mutex.h"
#include "video_core/pica/regs_texturing.h"
#include "video_core/rasterizer_accelerated.h"
#include "video_core/renderer_sgxd/guest_gpu_memory.h"
#include "video_core/renderer_sgxd/sgxd_shader_disk_cache.h"
#include "video_core/renderer_sgxd/sgxd_shader_tier.h"
#include "video_core/renderer_sgxd/key_index.h"
#include "video_core/renderer_sgxd/sgxd_backend.h"
#include "video_core/renderer_sgxd/state_regs.h"
#include "video_core/renderer_sgxd/usse/usse_program.h"
#include "video_core/renderer_sgxd/usse/vs_usse_gen.h"
#include "video_core/shader/generator/pica_fs_config.h"
#include "video_core/shader/generator/profile.h"

namespace Pica::Shader::Generator {
struct PicaVSConfig;
}

namespace Pica {
class ShaderEngine;
}

namespace Pica {
class PicaCore;
}

namespace Memory {
class MemorySystem;
}

namespace SgxdRenderer {

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
    ~RasterizerSgxd() override;

    void AddTriangle(const Pica::OutputVertex& v0, const Pica::OutputVertex& v1,
                     const Pica::OutputVertex& v2) override;
    void DrawTriangles() override;
    void FlushAll() override;
    /// After guest memory was loaded whole: the pictures and screen pictures go.
    void ForgetPictures();
    void ClearAll(bool flush) override;
    bool AccelerateDisplayTransfer(const Pica::DisplayTransferConfig& config) override;
    /// The screen picture of the LCD buffer at lcd: the copy the last display transfer into it made of
    /// its source's picture, at the source's scale, LINEAR U8888 width x height, the output's transpose.
    /// lcd's first row is picture column row * quarters / 4; its `rows` rows are rows * quarters / 4
    /// picture columns and its 240 columns 60 * quarters picture rows. `margin`: widescreen rows each
    /// side; `flip`: the rows run the other way; `format`: the output's Pica::PixelFormat (an 8-bit
    /// one's picture is U8888, a 16-bit one's its own). False when guest memory holds the screen. On
    /// the render thread.
    static bool ScreenPictureOf(PAddr lcd, u32 rows, u32& gpu, u32& width, u32& height, u32& row,
                                u32& quarters, u32& margin, bool& flip, u32& format);
    /// The LCD buffer [lcd, lcd + bytes) into guest memory if a screen picture owns it, waited for.
    static void ScreenToGuest(PAddr lcd, u32 bytes);
    /// The internal resolution in quarters (4 native, 5 1.25x, 6 1.5x), read at each pass on a buffer.
    /// TopScale answers 4 while TopMsaa is on. Any thread.
    static void SetTopScale(u32 quarters);
    [[nodiscard]] static u32 TopScale();
    /// Widescreen: the top screen's colour buffers drawn margin rows wider each side (a multiple of 4,
    /// 0 for none). Any thread.
    static void SetWidescreen(u32 margin);
    [[nodiscard]] static u32 Widescreen();
    /// 2x MSAA on the top screen (every other buffer at 1x while it is on). Any thread.
    static void SetTopMsaa(bool on);
    /// The HUD scale option, in quarters (4: off; 5 or 6): for a title with TuningHudScale, the
    /// top screen's 2D layer is drawn at this scale over its 3D picture, which stays at the
    /// internal resolution (PictureShapeFor). Only above the internal resolution, and not with
    /// widescreen margins or MSAA. Any thread.
    static void SetHudScale(u32 quarters);
#if defined(VITA_DEBUG_TOOLS) && !defined(VITA_DIAGNOSTICS)
    /// Tools builds' cpu line, since the start: geometry draws 0 queued with the pass, 1 sent
    /// alone for want of a triangle bound, 2 sent alone with a bound past the room.
    [[nodiscard]] static u32 ToolsGeometryDraws(int which);
    /// Tools builds' cpu line, since the start: geometry draws (even `which`) and their invocations
    /// (odd) by how pass A1 ran them: 0-1 in parallel, 2-3 in order (VsRequest::gs_serial), 4-5 in
    /// parallel although the program reads a temporary first (the Parallel geometry option).
    [[nodiscard]] static u32 ToolsGeometryOrder(int which);
    /// Tools builds' cpu line, since the start: the render thread's us in GeometryStage (0) and in
    /// its programs' lookups and makes (1).
    [[nodiscard]] static u64 ToolsGeometryUs(int which);
    /// Tools builds' cpu line, since the start: Draw's us by stage (0-5) and its count (6).
    [[nodiscard]] static u64 ToolsDrawUs(int which);
    /// Tools builds' cpu line: how geometry draws could be merged (tools_merge).
    [[nodiscard]] static u64 ToolsMerge(int which);
    /// Tools builds' cpu line: the repeat draws' time by stage (as ToolsDrawUs), 6 their count.
    [[nodiscard]] static u64 ToolsRepeatUs(int which);
    /// Tools builds' cpu line: the repeat draws by register 0x200 + i of theirs that differs from
    /// the draw before's (i < 64), 64 the indexed ones, 65-68 by topology.
    [[nodiscard]] static u32 ToolsRepeatRegs(int which);
    /// Tools builds' cpu line, since the start: GeometryStage's us by part (0-4).
    [[nodiscard]] static u64 ToolsGeometryPartUs(int which);
    struct ToolsDrawSections {
        ToolsDrawSections();
        ~ToolsDrawSections();
        void Mark(int stage);
        u64 last;
        int next = 0;
        /// This draw's time by stage, and whether it repeats the draw before (ToolsMergeNote).
        std::array<u64, 6> spent{};
        bool repeat = false;
    };
    /// Tools builds' slow-frame recorder (RenderThread): from Start to End (a present to the
    /// next), every pass rendered on its own and timed (SgxdBackend::tools_timing) and each
    /// draw's fragment program counted with its pass; End logs the passes by GPU time and the
    /// programs of the heaviest. `gap_ms`: the present gap that asked for it.
    void ToolsTimingStart(u32 gap_ms);
    void ToolsTimingEnd();
    struct ToolsDrawTally {
        u32 pass;
        u64 key;
        u32 draws, translucent, vertices;
    };
    std::vector<ToolsDrawTally> tools_tally;
    u32 tools_trigger_gap{};
    u32 tools_timed_frames{};
#endif
    /// The Fast shadows option: for a title with TuningFastShadows, a Shadow2D texture is read once
    /// through a linear filter and compared once (hard edges) rather than as four compared taps.
    /// Any thread; draws after it changes take it.
    static void SetFastShadows(bool on);
    /// Whether draws read shadows the fast way now (the option and the title's bit, or the
    /// diagnostics flag shadowfast).
    [[nodiscard]] static bool FastShadows();
    /// The Per-vertex effect lighting option: for a title with TuningPvEffectLighting, a blended
    /// draw whose lighting Usse::PvEffectEligible lets through is lit at its vertices (the vertex
    /// program's VsPvEffect form), not at every pixel; the fragment program reads the colour and
    /// N.V for its Fresnel lookup. Any thread; draws after it changes take it.
    static void SetPvEffectLighting(bool on);
    /// Whether eligible blended draws are lit at their vertices now (the option and the title's
    /// bit, or the diagnostics flag pveffect).
    [[nodiscard]] static bool PvEffectLighting();
    /// The Parallel geometry option: for a title with TuningParallelGeometry, a geometry program
    /// that reads a temporary before it writes it (Program::gs_temps_read_first) still runs its
    /// invocations side by side, each starting those temporaries at (0, 0, 0, 1) as the parallel
    /// form starts them, rather than in order in one instance with the temporaries carried from
    /// each invocation to the next (VsRequest::gs_serial). Any thread; draws after it changes
    /// take it.
    static void SetParallelGeometry(bool on);
    /// Whether such geometry programs run side by side now (the option and the title's bit, or the
    /// diagnostics flag parallelgs).
    [[nodiscard]] static bool ParallelGeometry();
    /// What applies now: SetHudScale's for a title with the tuning, else 4.
    [[nodiscard]] static u32 HudScale();
    /// Whether the top screen's 2D layer is drawn at the HUD scale now: one above the internal
    /// resolution, without MSAA. Its 3D picture is at HudBase.
    [[nodiscard]] static bool HudLayered();
    /// The scale of the top screen's 3D picture under the HUD scale: the internal resolution.
    [[nodiscard]] static u32 HudBase();
    [[nodiscard]] static bool TopMsaa();
    /// Whether the top screen's pictures are other than 1x ones: upscaled, widescreen or 2x.
    [[nodiscard]] static bool TopPictures();
    /// A guest frame has ended (each VBlank, its present shown or dropped): the draw logs count frames.
    void EndFrame() {
        frames++;
    }
    /// The guest wrote a watched page, or the HLE wrote [address, address + size): the textures over it
    /// are untiled again, and a depth store whose buffer's whole pages it reaches is no longer newer.
    void GuestWrote(PAddr address, u32 size);
    /// Whether a picture owns guest rows of [lo, hi), or copies them back still (any thread): a guest
    /// or HLE access there asks for the copy first.
    [[nodiscard]] bool PictureOwns(PAddr lo, PAddr hi) const;
    /// Whether a picture owns guest rows of [lo, hi) that no copy is taking back yet (any thread).
    [[nodiscard]] bool PictureOwnsRows(PAddr lo, PAddr hi) const;
    /// The rows a copy of [lo, hi) for an access may write back: the pictures' strips that meet it,
    /// which may reach past it ({0, 0}: none). Any thread.
    [[nodiscard]] std::pair<PAddr, PAddr> PictureCopyExtent(PAddr lo, PAddr hi) const;
    /// The render side of a guest or HLE access to [lo, hi): the rows pictures own there copied into
    /// guest memory, waited for unless `drain` is false; with write, the pictures and textures over the
    /// range marked written. `beside`: the guest keeps accessing a page of the range beside the rows.
    void ServeGuestAccess(PAddr lo, PAddr hi, bool write, bool drain = true, bool beside = false);
    /// The render side: the copies whose GPU work is done no longer count for PictureOwns.
    void RetirePictureCopies();
    bool AccelerateTextureCopy(const Pica::DisplayTransferConfig& config) override;
    bool AccelerateFill(const Pica::MemoryFillConfig& config) override;
    bool AccelerateDma(PAddr src, PAddr dst, u32 size) override;
    /// The GPU's MMU onto the guest's slots of `pages`, in order with the work queued (each pte[k]
    /// stored at pte_va[k]). At most SGXD_REMAP_MAX. `direct`: queued work read or wrote a page's old
    /// slot other than as a texture.
    void RemapPages(const u32* pages, const u32* pte_va, const u32* pte, u32 count, bool direct);
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
    /// The next draw's ranges in cached FCRAM that were cleaned out of the CPU's cache when the
    /// draw was queued (PageHolds::CleanGeometry): bit n the attribute loader n's vertices, bit
    /// CleanedIndices its 16-bit index list. They are read as they are, and stay cached.
    static constexpr u32 CleanedIndices = 12;
    void GeometryCleaned(u16 ranges) {
        cleaned_geometry = ranges;
    }
    /// The next draw's indices as the guest had them at the draw (a geometry draw's).
    void GsIndices(const u8* indices, bool index_u16) {
        const u32 bytes = regs.pipeline.num_vertices * (index_u16 ? 2u : 1u);
        gs_indices.assign(indices, indices + bytes);
        gs_indices_set = true;
    }
    /// Strip runs (RenderThread::DrawStripRun): indexed strips whose state is the run's first
    /// draw's but the index list (0x227) and count (0x228). While a run is armed, Draw keeps a
    /// draw's finished submission when another draw of the run may share it (shared_strip), and
    /// DrawSharedStrip submits a later draw from it: the same setup, its own index list and count,
    /// after cleaning its own index list and vertex range.
    /// NotKept: no draw kept (or the kept one not shareable); ProgramsChanged: programs replaced
    /// or a second tier ready; Pending: a DMA's retire or the code heap's flush due first;
    /// SecondaryCopied: the full path would place another copy of a secondary PDS program.
    enum class StripRefusal : u8 {
        None,
        NotKept,
        ProgramsChanged,
        Pending,
        SecondaryCopied,
        PassChanged,
        NoRoom,
        IndexRange,
        Count_
    };
    void BeginStripRun();
    void EndStripRun();
    /// The run's next draw from the kept submission, its registers set as the guest had them;
    /// `scanned` and `cleaned` as its record has them (ScannedIndices, GeometryCleaned). Refused:
    /// nothing submitted, the draw takes the full path, and the draw that does keeps its own.
    /// `submit` false builds the submission without drawing (the tools check, LastShared).
    StripRefusal DrawSharedStrip(u32 count, PAddr index_address,
                                 std::optional<std::pair<u32, u32>> scanned, u16 cleaned,
                                 bool submit);
#if defined(VITA_DEBUG_TOOLS) && !defined(VITA_DIAGNOSTICS)
    /// Tools builds' strip-run check: what DrawSharedStrip built last, and the backend's last
    /// draw (SgxdBackend::LastDraw) to compare it with.
    struct SharedSubmission {
        SgxdRecipeDraw draw;
        SgxdRecipeDrawPrograms p;
    };
    [[nodiscard]] const SharedSubmission& LastShared() const {
        return last_shared;
    }
    [[nodiscard]] const SgxdBackend& ToolsBackend() const {
        return backend;
    }
#endif
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
    [[nodiscard]] bool Replaying() const {
        return replay_limit != -2;
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
    [[nodiscard]] bool InterruptReached(u64 fence);
    void InterruptWait(u64 fence) override;
    void InterruptKick(u64 fence);
    /// The render thread's sleep until the fence's work (SgxdBackend::InterruptArm).
    [[nodiscard]] bool InterruptArm(u64 fence);
    void InterruptDisarm();
    void InterruptWaker(s32 flag, u32 bits);
    /// The render side took a fog LUT: FogTableWords makes its table.
    void FogLutChanged() {
        fog_table_dirty = true;
    }
    /// The render side took procedural texture tables: ProcTexTableWords makes its table.
    void ProcTexTablesChanged() {
        proctex_table_dirty = true;
    }
    [[nodiscard]] bool FlipArm(u32 fence) {
        return backend.FlipArm(fence);
    }
    /// The title's program id, which names the disk cache (opened at the first program needed).
    void SetTitle(u64 program_id) {
        title_id = program_id;
    }
    /// Parse thread, at a draw whose fragment registers changed: its program queued for the first
    /// tier's worker.
    void PrefetchFragment(const Pica::RegsInternal& parse_regs);
    /// Parse thread, at a draw without a geometry shader: its vertex program queued for the first
    /// tier's workers.
    void PrefetchVertex(const Pica::RegsInternal& parse_regs, Pica::ShaderSetup& parse_setup);
    /// The bytes of an 8-bit index list as the queued draws read it. Unset: guest memory.
    std::function<const u8*(PAddr, u32)> guest_indices;
    void SetFlipHook(std::function<bool(u32*)> pending, std::function<s64()> due_in,
                     std::function<void()> flip) {
        backend.SetFlipHook(std::move(pending), std::move(due_in), std::move(flip));
    }
#ifdef VITA_DIAGNOSTICS
    u64 GpuWaitUs() const override;
#endif
    std::vector<std::pair<PAddr, u32>> TakeCachedReads() override;

private:
    /// A vertex or index range a draw cleaned: its pages move to CDRAM at vblank (TakeCachedReads).
    void CachedRead(PAddr addr, u32 size);
    Common::Mutex cached_reads_mutex;
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
        u32 checked_seq;  // write_seq at its untile, or when TextureWritten last found none
        bool alpha_one = false; // a surface without alpha, read as a texture (swizzle bit 2)
        u32 last_used = 0;      // the pass count when a draw last took it (eviction order)
        SgxdBackend::Data alpha{}; // a split ETC1A4's A8 chain (alpha_bytes 0: none)
        u32 alpha_bytes = 0;
        Texture* bucket_next = nullptr; // the next texture of its bucket (texture_buckets)
        /// A non-power-of-two 2D texture's rows this many texels apart (a multiple of 32); 0 for a
        /// power-of-two chain.
        u32 stride = 0;
        /// Untiled into its own memory again when written (a cube, an ETC1A4 chain), not into a new copy.
        bool in_place = false;
        /// A byte copy's U8 swizzle: 6 for A8 and A4 (alpha), 5 for I8 and I4 (intensity); 0 for
        /// A8B8G8R8.
        u8 byte_swizzle = 0;
        /// A copy at an upscaled picture's or depth store's own scale: the GPU texture's sides; 0 for the
        /// PICA's. The coordinates are normalised, so a draw reads it as the 1x one.
        u32 gpu_width = 0;
        u32 gpu_height = 0;
#ifdef VITA_DIAGNOSTICS
#endif
    };

    /// A vertex program's SA slot that VertexSa fills each draw, one per uniform but the float bank.
    struct VertexSaSlot {
        Usse::UniformKind kind;
        u8 components;
        u8 index;    ///< DefaultInput's input register
        u32 sa;      ///< the slot's first float in the SA image
        u32 array_size;
        u32 uniform; ///< its index in usse.uniforms, for messages
    };
    struct VertexProgram {
        Usse::Program usse;
        u32 input_regs;
        u32 primary_doutu0;
        u32 secondary_doutu0;
        std::array<u32, 4> code; ///< the primary's and secondary's offset and bytes (FreeCode)
        u32 draws;         ///< draws with the first tier's program, up to Tier2Draws
        bool tier2_queued; ///< sent to the second tier (or never to be)
        std::vector<VertexSaSlot> sa_slots;
        /// The SA image's words (the secondary PDS program DMAs them all), and its fixed part.
        u32 sa_words;
        std::vector<float> sa_image;
        /// VertexSa's image, put together in cached memory before its one copy into the pass data; every
        /// draw writes the same words of it again.
        mutable std::vector<float> sa_scratch;
        /// The last image VertexSa put in the pass data, its pass and where: a draw with the same image
        /// takes that copy and its secondary PDS program.
        mutable std::vector<float> sa_last;
        mutable u32 sa_last_pass = ~0u;
        mutable SgxdBackend::Data sa_last_data{};
        mutable std::array<u32, 3> vdm_words{};
        mutable u32 vdm_sa = ~0u;
        /// uniforms_f, the whole float bank, in the buffer the SA's pointer names.
        bool has_bank;
        u32 bank_offset; ///< its first float in the buffer
        u32 bank_rows;
        /// The Input attribute's first primary attribute register by input register; ~0u: none.
        std::array<u32, 16> attr_resource;
        /// The program lights the draw at its vertices (VsPvEffect: it has a PvAmbient uniform), so
        /// the fragment program takes the pv_effect form. A program asked for it may lack it where
        /// its emission refused it (VertexProgramFor).
        bool pv_effect;
    };
    static void ResolveVertexProgram(VertexProgram& v);
    /// The buffer a program's SA pointer names (zeros and the float bank), built once and named again
    /// while its shape, the float uniforms' version and the pass stay the same.
    struct BankUpload {
        u32 pass;
        const Pica::ShaderSetup* setup; ///< null: no bank, the buffer all zeros
        u32 version;
        u32 floats, offset, rows;
        u32 gpu;
    };
    std::array<BankUpload, 8> bank_uploads{};
    u32 bank_upload_next = 0;
    [[nodiscard]] u32 BankBuffer(const VertexProgram& vp, const Pica::ShaderSetup& setup);

    /// Where a fragment uniform's value is in FSUniformData: `count` elements of `components` values,
    /// `stride` bytes apart, into the SA block at `sa` (elements four floats apart).
    struct UniformSource {
        u32 sa;
        u16 offset;
        u8 components;
        u8 count;
        u8 stride;
        bool is_int;
        bool normalize; ///< the vec3 normalised as it is copied (UniformKind::LightDirection)
        bool u8;        ///< packed to a word of bytes an element (Usse::Uniform::u8)
        bool f16;       ///< packed to a pair of four F16 an element (Usse::Uniform::f16)
        s8 lane;        ///< u8: every byte this component (Usse::Uniform::lane), or -1
        /// A LUT offset as its row's centre: 1 alone, 2 and 3 after a pair's x; 0 none.
        ::u8 lut_row;
        /// The distance attenuation's scale (1) or bias (2), in the LUT read's units (the bias plus half a
        /// texel). (::u8: the member u8 above hides the type.)
        ::u8 dist_atten;
        /// A Shadow2D border colour's (-depth, density) (UniformKind::ShadowBorderDepth).
        bool shadow_border;
        /// A constant colour's two Dot3 terms (UniformKind::Dot3Const).
        bool dot3_const;
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
        u32 pixel_pa;
        /// The pixel PDS's iterators (DOUTI words and sizes); the draw places the program in the PDS arena.
        std::array<u32, 12> iterators;
        std::array<u32, 12> iterator_words;
        u32 iterator_count;
        Epilogue epilogue; ///< what its code ends with
        std::array<u32, 4> code; ///< the primary's and secondary's offset and bytes (FreeCode)
        u32 draws;         ///< draws with the first tier's program, up to Tier2Draws
        bool tier2_queued; ///< sent to the second tier (or never to be)
        mutable std::vector<u32> sa_last;
        mutable u32 sa_last_pass = ~0u;
        mutable SgxdBackend::Data sa_last_data{};
        mutable std::vector<u32> buf_last; ///< its memory buffer's, if it has one
        mutable SgxdBackend::Data buf_last_data{};
        mutable u32 pixel_size = 0;
        mutable u32 pixel_size_sa = ~0u;
        /// Usse::SampledTextureUnits of its config (the key holds the config), 0xff until a
        /// strip run's keep asks (Draw): emitting to find them costs as much as a draw's setup.
        mutable u8 sampled_units = 0xff;
    };
    /// Draws after which a program is emitted again with the optimising passes.
    static constexpr u32 Tier2Draws = 100;
    /// A first-tier program of this many words goes to the second tier at once.
    static constexpr u32 Tier2Words = 256;

    void RecordClear(const ClearedRange& range);
    void ForgetClears(PAddr start, PAddr end);
    bool TakeClear(PAddr start, PAddr end, ClearedRange& out);
    /// The PBE's form of a colour buffer (route 1 takes widths of 32 to 1024 in 16s and heights of 8
    /// to 1024 in 8s).
    SgxdBackend::PicaTarget PicaTargetFor(const ColourBuffer& c, const char* what);
    /// Whether [start, end) overlaps the open or parked pass's colour buffer, or its depth buffer when
    /// a draw uses it.
    bool PassOverlaps(PAddr start, PAddr end) const;
    /// Whether [start, end) overlaps the open or the parked pass's colour buffer.
    bool PassColourOverlaps(PAddr start, PAddr end) const;
    void EndPass(const char* why);
    void EndOnePass(const char* why);
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
        std::array<SgxdVertexFetch, SGXD_PROGRAM_MAX_FETCH> fetch;
        u32 fetch_count{};
        u32 indices{};    // a 16-bit index list's GPU address, 0 for the identity list
        u32 count{};      // the indices the draw takes
        u32 vertex_min{}; // the PICA vertices it reads
        u32 vertex_max{};
        const Pica::ShaderSetup* setup{}; // the unit whose uniforms the program reads
        bool fresh{}; // a fetch reads pass data allocated for the draw (SgxdBackend::VertexPds)
    };
    struct VertexFeed {
        u32 reg;
        PAddr address;
        u32 stride;
        u32 words;
        u32 loader;
    };
    struct IndexedReads {
        bool mapped; // false: the indices are outside the guest's memory, the draw draws nothing
        bool scan;   // the vertex range comes from the index list (else every reachable vertex)
        u32 indices_gpu;
        u32 vertex_min;
        u32 vertex_max;
    };
    /// An indexed draw's index list: whether it is mapped, whether to scan it, the vertex range, and
    /// the list's GPU address (cleaned or widened as needed).
    IndexedReads ReadIndexedList(std::span<const VertexFeed> feeds, u16 input_regs,
                                 PAddr index_address, bool index_u16, u32 count, Usse::VsMode mode,
                                 const Describe& describe);
    /// Cleans the cached guest vertices a draw's feeds read, over the range `r` found.
    void CleanVertexReads(std::span<const VertexFeed> feeds, u16 input_regs, u32 first,
                          const IndexedReads& r);
    /// The loaders' attributes into the PICA's vertex program. With StoreOutputs the fetch covers
    /// vertex_min to vertex_max densely and the caller adds the stores and the index list.
    /// `pv`: per-vertex effect lighting's lights (PvEffectRequest), 0 for none.
    VertexStage VertexStageFor(bool is_indexed, const float* immediate, u32 count,
                               Usse::VsMode mode, const Describe& describe, u32 pv = 0);
    /// A draw with a geometry shader: the vertex program's outputs and the geometry program's triangles
    /// through two stream-out renders, then the triangles as the draw's vertices (the SGX has no
    /// geometry stage, so the geometry program runs as a vertex program that stores what it emits).
    VertexStage GeometryStage(bool is_indexed, const Describe& describe);
    /// `lines` lines of `line` bytes, src_stride apart, to dst_stride apart through the PTLA.
    bool CopyLines(PAddr src, u32 src_stride, PAddr dst, u32 dst_stride, u32 line, u32 lines);
    /// The part of a pass's target a draw's viewport covers, in the target's pixels (x in 8s, y0 in
    /// 16s); x1 0 for all of it.
    struct StorePart {
        u32 x0{}, x1{}, y0{}, y1{};
        bool operator==(const StorePart&) const = default;
    };
    /// Whether a draw writes every pixel of a width x height target (or of `part`) whatever was there:
    /// two triangles covering it whole, not culled or clipped, and an output merger that takes nothing
    /// from the target. A pass it opens loads nothing. `depth`: every pixel's depth instead, the depth
    /// test ALWAYS or off and the stencil written whole or not tested.
    bool CoversTarget(bool is_indexed, const float* immediate, u32 count, u32 width, u32 height,
                      const StorePart& part, bool depth = false);
    /// A draw that would open a pass only to write one colour (one depth, 0 or 1, and one
    /// stencil, or none) over the part of target it stores (CoversTarget): done as fills of the
    /// guest's buffer and the depth store through the PTLA, beside the 3D, where the pass would
    /// store every tile of the part. Only for a buffer 32 wide or more drawn at 1x: the fill
    /// takes the rows a 1x picture owns there (ColourWritesDue); a scaled picture's quad is
    /// drawn. stencil: the draw's stencil test is on. False when it is not such a draw, and
    /// nothing was done; with act false, only whether it is one.
    bool FillsClearQuad(bool is_indexed, const float* immediate, u32 count,
                        const ColourBuffer& target, const StorePart& part, PAddr depth,
                        bool dwrite, bool stencil, bool act);
    u32 quad_fills = 0;
    /// Where clear quads were drawn (a buffer and its passes' part), and whether the last pass one
    /// opened held nothing else. A quad is filled only there: one the pass's later draws join costs
    /// next to nothing drawn, where a fill adds a PTLA op the 3D waits for.
    struct ClearQuadSite {
        PAddr addr{};
        StorePart part{};
        bool alone{};
    };
    std::array<ClearQuadSite, 16> clear_quad_sites{};
    u32 clear_quad_next = 0;  ///< the site replaced next
    s32 pass_clear_site = -1; ///< the open pass's first draw was a clear quad of this site
    s32 filled_site = -1;     ///< a site filled since the last P3D, with no draw after it yet
    u32 pass_guest_draws = 0; ///< the guest's draws in the open pass
    /// A draw of `count` vertices (4 or 6) run through the vertex program on the CPU; false when
    /// its index list is not in memory. Once a draw.
    bool QuadVertices(bool is_indexed, const float* immediate, u32 count,
                      std::array<Pica::OutputVertex, 6>& out);
    bool RunVertices(bool is_indexed, const float* immediate, u32 count,
                     std::span<Pica::OutputVertex> out);
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
    /// A draw's viewport on the TA's axes: words 12-15 (x centre, x scale, y centre, y scale), each
    /// scale widened by `guard`, which the vertex program divides x and y by: the TA's guard band is
    /// `guard` times wider (DrawViewport).
    struct DrawView {
        std::array<float, 4> words;
        std::array<float, 2> guard;
    };
    DrawView DrawViewport() const;
    /// The draw's target's rows, which DrawViewport's corner counts from.
    u32 draw_rows{};
    DrawView draw_view{};
    /// The draw's clip z orientation (Usse::UniformKind::ClipZSign) from its position rows and
    /// uniforms; `near_w` the least w the PICA's planes leave of z = alpha w + beta, or NaN.
    float ClipZSign(const VertexProgram& vp, const Pica::ShaderSetup& setup, float& near_w);
    /// The draw's ClipZSign, for VertexSa's ClipZSign slot, and its near_w.
    float draw_clip_z_sign = -1.0f;
    float draw_near_w = 0.0f;
    /// The W-buffered draws' TA depth 1 - K / w (K / w where the PICA's depth falls with w): K starts
    /// at 1 and only comes down, to the power of two at or below a W-buffered draw's near plane, so no
    /// depth lies below 0, where the ISP clamps it. A smaller K costs depth resolution far away.
    float w_depth_k = 1.0f;
    SgxdBackend::Data VertexSa(const VertexProgram& vp, const Pica::ShaderSetup& setup,
                               u32& vsec_sa, const Describe& describe);
    /// For each VS input register, the attribute whose default value it reads, -1 for none; made again
    /// only when the attribute mask or the input map changed.
    const std::array<s8, 16>& DefaultInputAttributes();
    std::array<s8, 16> default_input_attr{};
    std::array<u32, 3> default_input_key{};
    bool default_input_valid = false;
    /// The vertex side of a draw's programs: the fetch and secondary PDS programs, the VDM words, the
    /// output selects.
    void VertexPrograms(const VertexStage& stage, const SgxdBackend::Data& vsa, u32 vsec_sa,
                        SgxdRecipeDrawPrograms& p, const Describe& describe);

    /// A vertex program's inputs past its PicaVSConfig: the layout's registers, the mode and the
    /// bools its flow control reads.
    struct VertexInputs {
        u16 default_regs, u8_regs, s8_regs, s16_regs;
        std::array<u8, 16> reg_components;
        Usse::VsMode mode;
        u16 bools, used_bools;
        /// Per-vertex effect lighting's lights (PvEffectRequest's encoding), 0 for none: in the key
        /// only when set, so the other programs keep theirs.
        u32 pv = 0;
    };
    static u64 VertexKeyOf(const Pica::Shader::Generator::PicaVSConfig& config,
                           const VertexInputs& in);
    static Usse::VsRequest VertexRequestOf(const Pica::Shader::Generator::PicaVSConfig& config,
                                           const Pica::ShaderSetup& setup, const VertexInputs& in);
    /// `u8_regs`: the input registers an integer loader feeds.
    const VertexProgram& VertexProgramFor(u16 default_regs,
                                          const std::array<u8, 16>& reg_components, u16 u8_regs,
                                          u16 s8_regs, u16 s16_regs,
                                          Usse::VsMode mode = Usse::VsMode::Draw, u32 pv = 0);
    VertexProgram PlaceVertexProgram(Usse::Program usse, const char* what, u64 key,
                                     u64 emit_us = 0);
    /// The geometry program for `rows` input rows and at most `max_triangles` triangles an invocation;
    /// `input_rows`: the fixed-primitive mode's registers that read rows of their own.
    /// `forced_parallel`: a program that reads a temporary first, run side by side anyway (the
    /// Parallel geometry option; `serial` false). Its code is the parallel form's, but its key is
    /// its own, so the variant the option asked for is never taken for another.
    const VertexProgram& GeometryProgramFor(u32 rows, bool rows_uniform, bool rows_table,
                                            u32 first_uniform, u16 input_rows, u32 max_triangles,
                                            bool serial, bool forced_parallel,
                                            const Describe& describe);
    /// A geometry program's key, and the bool registers it reads with their values (setup's).
    struct GeometryKey {
        u64 key;
        u16 bools;
        u16 used;
        bool uniform_table; ///< Usse::VsRequest::gs_uniform_table
    };
    /// GeometryStage's choice for the draw's A1 programs, read by GeometryKeyFor and
    /// GeometryRequest: its invocations zero their slots (Usse::VsRequest::gs_zero_slots).
    bool gs_zero_slots{};
    /// `forced_parallel` sets a key bit of its own, only when true: the other keys are unchanged.
    GeometryKey GeometryKeyFor(u32 rows, bool rows_uniform, bool rows_table, u32 first_uniform,
                               u16 input_rows, u32 max_triangles, bool serial,
                               bool forced_parallel);
    Usse::VsRequest GeometryRequest(const GeometryKey& k, u32 rows, bool rows_uniform,
                                    bool rows_table, u32 first_uniform, u16 input_rows,
                                    u32 max_triangles, bool serial,
                                    bool forced_parallel) const;
    /// A new geometry draw's second program (b15 true) on the first tier's worker while this thread
    /// emits the first, when neither is placed or cached and they differ.
    void QueueGeometryPair(u32 rows, bool rows_uniform, bool rows_table, u32 first_uniform,
                           u16 input_rows, u32 max_triangles, bool serial,
                           bool forced_parallel, bool b15);
    const VertexProgram& MakeGeometryProgram(const GeometryKey& k, u32 rows, bool rows_uniform,
                                             bool rows_table, u32 first_uniform, u16 input_rows,
                                             u32 max_triangles, bool serial,
                                             bool forced_parallel, const Describe& describe);
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
    /// The host copy of texture unit `unit`'s texture for the open pass: untiled when new or written
    /// over (queued, SgxdBackend::QueueUntile). A unit whose register words match a memo takes its
    /// texture without TextureFromRegs.
    const Texture& TextureFor(u32 unit);
    /// TextureFor from the unit's registers. `written`: the memo found a write over it.
    Texture& TextureFromRegs(u32 unit, bool written);
    /// A vertex-side program's code into the heap: its DOUTU words and code ranges.
    void PlaceVertexCode(VertexProgram& v);
    void FreeProgramCode(const std::array<u32, 4>& code);
    /// Every program's code back to the heap and the programs forgotten (with the states and layouts
    /// that point at them). Run before a draw looks a program up, with the heap half free.
    void FlushPrograms();
    u32 program_flushes = 0;
    /// The first tier's emission of a vertex (0), geometry (1) or fragment (2) program, for perflog.
    void NoteEmit([[maybe_unused]] int kind, [[maybe_unused]] u64 us) {
#ifdef VITA_DIAGNOSTICS
        emit_stats.programs[kind]++;
        emit_stats.us[kind] += us;
#endif
    }
    /// A program of that kind taken from the first tier's workers, emitted or read from the disk.
    void NoteTier1([[maybe_unused]] int kind, [[maybe_unused]] const ShaderTier1::Taken& t) {
#ifdef VITA_DIAGNOSTICS
        if (t.from_disk) {
            emit_stats.from_disk[kind]++;
            emit_stats.disk_us += t.took_us;
        } else {
            emit_stats.taken[kind]++;
            emit_stats.taken_us += t.took_us;
        }
        emit_stats.waited_us += t.waited_us;
        emit_stats.helped += t.helped ? 1 : 0;
#endif
    }
    u32 flush_mark = ~0u; ///< the backend's PassMark at the last flush
    /// Where a texture unit's texels are in guest memory: a 2D chain or six cube faces, `bytes` each.
    struct TextureExtent {
        std::array<PAddr, 6> part_addr{};
        u32 parts = 1;
        u32 bytes = 0;
        u32 levels = 1;
        bool cube = false;
    };
    [[nodiscard]] TextureExtent TextureExtentFor(u32 unit) const;
    /// The guest bytes a unit may read in this draw: its parts, or for a quad's one-level 2D texture
    /// the tile rows its coordinates reach.
    struct UnitRead {
        u32 parts = 0;
        std::array<PAddr, 6> lo{};
        std::array<PAddr, 6> hi{};
    };
    void TextureReads(bool is_indexed, const float* immediate, u32 count);
    [[nodiscard]] bool UnitReadsDrawn(u32 unit) const;
    /// The colour buffer tile rows the draw can write, as guest bytes; empty when none.
    [[nodiscard]] std::pair<PAddr, PAddr> DrawnRows() const;
    /// Whether an enabled texture unit the program samples reads bytes the open pass drew.
    [[nodiscard]] bool TexturesReadPassColour() const;
    /// Whether the unit holds an ETC1A4 texture split into compressed colour and an A8 alpha texture.
    bool SplitAlpha(u32 unit) const;
    static bool SplitAlphaIn(const Pica::RegsInternal& regs, u32 unit);
    /// Whether the unit's texture is stored as four bytes in channel order, R lowest (U8U8U8U8,
    /// swizzle 0): the U8 combiners read its stored word.
    static bool RawBytesIn(const Pica::RegsInternal& regs, u32 unit);
    /// The alpha texture's words of a split unit from its colour's, over its A8 chain (swizzle R000).
    static void AlphaTextureWords(const Texture& tex, const u32 colour[4], u32 words[4]);
    /// The GPU address of a fixed palette for a 4-bit format (0 for none), written at the first use.
    u32 PaletteFor(TexturePalette palette);
    /// The sampler's four state words for the unit's texture, which it returns (NullTexture when off).
    const Texture& TextureWords(u32 unit, u32 words[4]);
    const Texture& NullTexture(u32 unit);
    /// A GPU write over [start, end): its pages stamped, so the textures over them are untiled again.
    void TexturesWritten(PAddr start, PAddr end);
    bool TextureWritten(Texture& texture);
    /// The lighting and fog LUTs the guest changed into fresh rows, fs_data's offsets pointed at them.
    void SyncLutLf();
    /// The same for the proctex colour map, alpha map, colour and colour-difference tables.
    void SyncLutProcTex();
    /// The fog factor table's texture words for the fog LUT and `flip`, made again when either changed.
    void FogTableWords(bool flip, u32 words[4]);
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
    /// Each guest page's last GPU write as a write_seq stamp: a texture reads its own pages' stamps at
    /// its next use.
    std::vector<u32> page_writes;
    u32 write_seq{};
    /// How many textures' parts cover each guest page: a write over pages none covers stamps nothing.
    std::vector<u16> page_textures;
    /// TextureFor's last result for each unit: the five register words, their XOR, the texture and
    /// texture_drops then. A unit whose words match any of the three takes its texture at once; a cube
    /// is never memoised.
    struct TextureMemo {
        std::array<u32, 5> words{};
        u32 hash = 0;
        Texture* texture = nullptr;
        u32 drops = 0;
    };
    std::array<TextureMemo, 3> texture_memos{};
    u32 texture_drops{}; ///< DropTexture's count: a memo from before one is stale
    static u32 TextureBucket(PAddr addr);
    /**
     * The bytes of [addr, addr + bytes) that guest memory holds. A texture may run past VRAM's
     * end: only its pages up to there are tracked, and its untile reads on into the filler mapped
     * past VRAM (GuestGpuMemory::PastVramBytes), the texels past the end undefined. Past the
     * filler or FCRAM's end is not built.
     */
    static u32 ReadableBytes(PAddr addr, u32 bytes);
    /// The first and last page (TextureBucket) of the texture's part k, up to ReadableBytes.
    static std::pair<u32, u32> PartPages(const Texture& texture, u32 k);
    Texture* FindTexture(PAddr addr) const;
    void IndexTexture(Texture& texture);
    void DropTexture(Texture& texture);
    SgxdBackend::Data palettes{};
    /// The proctex noise NoiseRand2D(x, y) = h for x, y < 256 as F16x4 texels h * (x odd),
    /// h * (y odd), h, 0.
    SgxdBackend::Data noise_table{};
    u32 NoiseTable();
    KeyIndex<VertexProgram> vertex_programs;
    /**
     * VertexStageFor's decoded vertex formats, by the raw registers that describe one: the
     * attribute formats, each loader's component list, stride and component count, the VS
     * input count and input map (StateRegs::Layout). Not the addresses: the base and the
     * loaders' offsets change mesh by mesh and are added per draw. Each also keeps the vertex program it resolved to, by the program's own inputs
     * (VertexProgramKey), once that program has drawn the draws before its second tier.
     */
    struct VertexLayout {
        static constexpr u32 ProgramKeyWords = 4 + 4 + 7 + 2;
        struct Attribute {
            u8 reg, loader;
            u32 offset; ///< from the loader's start of a vertex
            u32 stride, words;
        };
        std::array<Attribute, 16> attributes{};
        u32 count = 0;
        u16 enabled_regs = 0, u8_regs = 0, s8_regs = 0, s16_regs = 0;
        std::array<u8, 16> reg_components{};
        std::array<u32, ProgramKeyWords> program_key{};
        const VertexProgram* program = nullptr;
    };
    /// The vertex formats met, by their registers' hash, hashed again only after one changed.
    KeyIndex<VertexLayout> vertex_layouts;
    static constexpr u32 MaxVertexLayouts = 1024; ///< past which vertex_layouts starts again
    VertexLayout* last_layout = nullptr;
    u64 last_layout_regs = 0;
    VertexLayout& VertexLayoutFor();
    static VertexLayout DecodeVertexLayout(const Pica::RegsInternal& regs);
    /// The raw words VertexProgramFor's program depends on past the layout.
    static std::array<u32, VertexLayout::ProgramKeyWords> VertexProgramKey(
        const Pica::RegsInternal& regs, Pica::ShaderSetup& setup, Usse::VsMode mode);
    /// What a pass on target stores for the draw (its viewport's part, or all of it off the grid).
    StorePart StorePartFor(const ColourBuffer& target) const;
    StorePart ViewportPart(const ColourBuffer& target) const;
    /// Whether the open pass takes a draw whose viewport lies inside the region its draws are kept to:
    /// the ISP's mask is cleared outside it and the pass goes on.
    bool KeepPassForViewport(const ColourBuffer& target);
    /// Whether a part's edges lie where the end of tile drops strips (x0, x1, y1 in 8s, y0 in 16s).
    static bool OnStoreGrid(const StorePart& p);
    KeyIndex<u16> used_bools;
    /// The second tier by fragment config (ShaderTier1::ConfigKey): the program is the config's
    /// under every epilogue, so one job a config. The programs waiting for it, and once done the
    /// program, for the epilogues that come later.
    struct Tier2Config {
        std::optional<Usse::Program> program;
        std::vector<u64> waiting;
    };
    KeyIndex<Tier2Config> tier2_configs;
    /// The program of `key` to the config's second tier: queued once a config, or taken now.
    void QueueFragmentTier2(u64 key, const Pica::Shader::FSConfig& config);
    void InstallFragmentTier2(u64 key, const Usse::Program& program);
    /// PrefetchVertex's, the parse thread's: the last draw's words and layout, the bools each
    /// program reads, and the code copy its jobs share.
    std::array<u32, VertexLayout::ProgramKeyWords> parse_vertex_words{};
    u64 parse_layout_hash = 0;
    KeyIndex<u16> parse_used_bools;
    std::shared_ptr<const ShaderTier1::VertexCode> parse_vs_code;
    u64 parse_vs_code_hash = 0, parse_vs_swizzle_hash = 0;
    /// What each geometry program's code says, by its code's hash and entry: its
    /// Usse::GsTriangleBound (0: none), and Usse::GsIndexesUniforms.
    struct GsCodeFacts {
        u32 bound;
        bool indexes_uniforms;
    };
    KeyIndex<GsCodeFacts> gs_code_facts;
    GsCodeFacts GsFacts();
    /// The most a geometry draw's triangle slots take when each invocation's room is its program's
    /// bound (GeometryStage): half a pass's data.
    static constexpr u32 GsBoundRoomBytes = 1u << 20;
    /// Whether the geometry program set now gets its table of uniforms in this mode
    /// (Usse::VsRequest::gs_uniform_table).
    bool GsUniformTable(bool rows_uniform, bool rows_table) {
        return rows_uniform && !rows_table && GsFacts().indexes_uniforms;
    }
    KeyIndex<FragmentProgram> fragment_programs;
    /**
     * FragmentProgramFor's programs by the hash of the registers FSConfig is built from
     * (StateRegs::Fragment, hashed again only after one of them changed) and the epilogue's
     * key: FSConfig, 272 bytes
     * built from those registers and hashed with XXH3's long path, only for a state not met yet.
     * A draw whose registers and epilogue are the last draw's keeps its program.
     */
    struct FragmentState {
        FragmentProgram* program;
        u64 key; ///< the program's in fragment_programs
    };
    KeyIndex<FragmentState> fragment_states;
    /// States, past which fragment_states starts again (the texture sizes are among its words).
    static constexpr u32 MaxFragmentStates = 1u << 14;
    FragmentProgram* last_fragment = nullptr;
    u64 last_fragment_key = 0;
    u64 last_fragment_regs = 0;
    u64 last_fragment_epilogue = 0;
    u32 last_fragment_own = 0;
    bool last_fragment_kills = false;
    bool last_fragment_depth_stencil = false;
    /// The open pass's colour buffer bytes its draws covered so far, whole tile rows.
    PAddr pass_drawn_lo = 0;
    PAddr pass_drawn_hi = 0;
    std::array<UnitRead, 3> unit_reads{};
    u32 draw_own_pixel = 0;
    /// The draw keeps its fragments inside its viewport by the scissor's kill: it writes depth or
    /// stencil in a pass that loaded guest memory's depth and stores part of its target.
    bool draw_kills_viewport = false;
    /// The draw writes depth or tests the stencil: the alpha test stays a kill.
    bool draw_depth_stencil = false;
    /// The draw is lit at its vertices: its vertex program has the VsPvEffect form, so its fragment
    /// program takes the pv_effect one (FragmentConfigFor).
    bool draw_pv_effect = false;
    bool last_fragment_pv = false;
    /// Per-vertex effect lighting for this draw (Draw): 0 for none, else the lights the vertex
    /// program is asked to light, encoded (bits 0-1 the count, 2-3 two-sided, 4-6 and 7-9 the
    /// lights' numbers). What the registers allow is kept by their hash (StateRegs::Fragment), as
    /// fragment_states keeps the programs: an FSConfig built once a state.
    u32 PvEffectRequest(bool blend);
    KeyIndex<u32> pv_requests;
    u64 pv_regs_hash = 0;
    bool pv_regs_known = false;
    u32 pv_regs_request = 0;
    /// FSConfig from the registers, with the sgxd_flags' changes and the units' alpha split.
    Pica::Shader::FSConfig FragmentConfig() const;
    /// FragmentConfig of any registers (the parse thread's too). `pv_effect`: the draw's vertex
    /// program lights it (draw_pv_effect), so an eligible configuration takes the pv_effect form.
    static Pica::Shader::FSConfig FragmentConfigFor(const Pica::RegsInternal& regs, u32 own_pixel,
                                                    bool kills_viewport, bool pv_effect = false);
    /// The hashes of StateRegs' sets, and whether a set's registers changed since.
    u64 fragment_regs_hash = 0;
    u64 layout_regs_hash = 0;
    bool fragment_regs_dirty = true;
    bool layout_regs_dirty = true;
    std::optional<std::pair<u32, u32>> scanned_indices;
    u16 cleaned_geometry = 0;
    /// A strip run's kept submission (DrawSharedStrip): `ok` when another draw of the run may
    /// share it. The feeds and input registers are its vertex stage's (VertexStageFor), the pass
    /// mark, data opens, PDS generation, colour buffer and programs' generation those it drew
    /// under, `fragment` its fragment program.
    struct SharedStrip {
        bool ok;
        SgxdRecipeDraw draw;
        SgxdRecipeDrawPrograms p;
        std::array<VertexFeed, 16> feeds;
        u32 feed_count;
        u16 input_regs;
        u32 pass_mark;
        u32 data_opens;
        u32 pds_generation;
        PAddr colour;
        u32 programs_generation;
        /// The kept draw's programs: the values of fragment_programs and vertex_programs, which
        /// stay where they are put (KeyIndex's deque; a second tier replaces one in place) until
        /// FlushPrograms clears them, which steps programs_generation (refused from then on).
        FragmentProgram* fragment;
        u64 fragment_key;
        VertexProgram* vertex;
        /// VertexProgramFor's key and inputs for `vertex`, for its second tier's step (set
        /// whenever VertexProgramFor ran for the draw, which it does while the program's second
        /// tier is not queued).
        u64 vertex_key;
        VertexInputs vertex_in;
        bool blend; ///< for PerflogDraw
        bool keeps_alpha; ///< a translucent alpha-keeping draw (keep_alpha_translucents)
        /// The vertex and pixel secondaries' SA images, sizes and programs (SecondaryHeld).
        u32 vsa, vsa_words, vsa_doutu0;
        u32 fsa, fsa_words, fsa_doutu0;
    };
    SharedStrip shared_strip{};
    bool strip_run_armed = false;
    /// Steps where the programs a kept submission points at may be replaced (InstallTier2,
    /// FlushPrograms).
    u32 programs_generation = 0;
#if defined(VITA_DEBUG_TOOLS) && !defined(VITA_DIAGNOSTICS)
    SharedSubmission last_shared{};
#endif
    std::vector<u8> gs_indices;
    bool gs_indices_set = false;
    /// The second tier's programs of earlier runs. Before the tiers, whose workers read and
    /// store into it.
    ShaderDiskCache disk_cache;
    u64 title_id = 0;
    /// The first tier's worker. Before tier2, whose workers emit its jobs too.
    std::unique_ptr<ShaderTier1> tier1;
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
    /// The lighting LUT rows of the current half by their table's contents: a table this half holds
    /// takes that row again. Cleared when the ring moves to its other half.
    KeyIndex<u32> lut_rows_by_contents;
    /// The fog factor tables: a ring of slots in two halves, kept as the LUT rows, the current half's
    /// by contents and flip.
    SgxdBackend::Data fog_tables{};
    u32 fog_table_next{}, fog_table_half{};
    std::array<u32, 2> fog_table_half_mark{};
    std::array<u32, 16> fog_table_pass{};
    std::array<u32, 4> fog_table_words{};
    bool fog_table_dirty{true};
    bool fog_table_flip{};
    KeyIndex<u32> fog_tables_by_contents;
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

    /// Draws with no blend that keep only the alpha: opaque, or translucent (perflog).
    u32 keep_alpha_opaques = 0, keep_alpha_translucents = 0;
    /// The flush objects a pass may take for them (SgxdBackend::OpaqueKeepingAlpha).
    static constexpr u32 KeepAlphaFlushes = 4;

    /// A colour buffer's picture: scaled, every buffer a render takes whole is drawn into one at the
    /// internal resolution (the top screen's with its margins and MSAA), in the buffer's own format. The
    /// guest's buffer gets it at 1x once something other than its passes reads or writes the bytes
    /// (WriteColourBack); a transfer reads the picture itself, a texture over it is copied from it, and
    /// a transfer to the LCD copies it for the present (ScreenPicture).
    struct Upscaled {
        PAddr addr;
        u32 width, height, bytes; ///< the guest buffer's
        u32 quarters;              ///< its scale (TopScale when it was made)
        u32 margin;                ///< its widescreen margin (Widescreen when it was made)
        u32 samples;               ///< its passes' samples a pixel (2 with TopMsaa, at 1x)
        SgxdBackend::Data picture; ///< height x width scaled (the pass's transpose), LINEAR
        u32 stride;                ///< its rows apart, in pixels (a multiple of 8)
        /// The rows where the picture is older than guest memory or another picture: whole strips, sorted,
        /// apart. A pass that loads the buffer loads those from guest memory and stores all of it.
        std::vector<std::pair<PAddr, PAddr>> outdated;
        /// The guest bytes whose newest colour is this picture's: whole strips, sorted, apart. A byte has
        /// one owner at most (guest memory none).
        std::vector<std::pair<PAddr, PAddr>> owned;
        SgxdBackend::PicaTarget copy_target{};
        Pica::FramebufferRegs::ColorFormat format{}; ///< the guest buffer's
        SgxdBackend::PictureFormat picture_format{};
        u32 last_pass{}; ///< `passes` when a pass last drew it (UpscaledFor's eviction)
    };
    /// The rows pictures own and their copies still on the GPU, for PictureOwns on any thread (built
    /// again whenever either changes, PublishOwned).
    mutable std::mutex owned_lock;
    std::vector<std::pair<PAddr, PAddr>> owned_published;
    std::vector<std::pair<PAddr, PAddr>> rows_published;
    /// The pictures' buffers with them: [lo, hi) and a strip's bytes (0: a screen picture).
    std::vector<std::array<u32, 3>> footprint_published;
    std::atomic<bool> owned_any{false};
    struct CopyInFlight {
        PAddr lo, hi;
        u64 fence;
    };
    std::vector<CopyInFlight> copies_in_flight;
    void PublishOwned();
    /// The pictures' bytes at most (UpscaledFor), in the textures' pool.
    static constexpr u32 PicturesBytes = 16u << 20;
    /// Whether a buffer is drawn into a picture (only one that scales whole), its scale, margin and
    /// samples a pixel.
    struct PictureShape {
        bool drawn;
        bool scaled;
        u32 quarters;
        u32 margin;
        u32 samples;
    };
    /// `depth`: the pass's depth buffer, 0 for none. A pass with a depth buffer a top-screen pass drew
    /// takes the top screen's shape too (they share the depth store).
    [[nodiscard]] PictureShape PictureShapeFor(const ColourBuffer& target, PAddr depth) const;
    /// Whether opening a pass on target copies picture rows first. Such a pass is not opened beside a
    /// parked one.
    [[nodiscard]] bool PassOpenCopies(const ColourBuffer& target, PAddr depth, PAddr depth_end,
                                      bool uses_depth) const;
    /// The picture a texture is copied from instead of untiled: one level of its whole rows from
    /// first_row, none outdated, in its buffer's format and width; null for none.
    [[nodiscard]] const Upscaled* PictureForTexture(PAddr addr, u32 width, u32 height, u32 format,
                                                    u32 levels, bool cube, u32 parts,
                                                    u32& first_row) const;
    std::vector<PAddr> top_sources; ///< the colour buffers transferred to the top LCD lately
    /// The depth buffers passes drew lately with the top screen's margins or MSAA.
    std::vector<PAddr> top_depths;
    void NoteTopDepth(PAddr depth);
    /// A list: a pass's picture stays in place while the pass beside makes or drops another.
    std::list<Upscaled> upscaled;
    /// The HUD scale's frame: a pass with depth has drawn a top screen buffer's 3D picture
    /// (hud_world_done), a pass at the HUD's scale has opened since (hud_layer_open); both until
    /// the next display transfer. hud_holdoff: transfers to go before the layer is scaled again.
    bool hud_world_done = false;
    bool hud_layer_open = false;
    u32 hud_holdoff = 0;
    static constexpr u32 HudHoldoffFrames = 60;
    u32 hud_opens = 0;           ///< perflog: layer passes opened from the 1x picture
    u32 hud_depth_fallbacks = 0; ///< perflog: depth draws that took the layer back to 1x
    Upscaled* pass_upscaled = nullptr;           ///< the open pass's buffer, drawn scaled
    SgxdBackend::PicaTarget pass_scaled_target{}; ///< its target, for DownscalePicture after it
    /// Its target's part before a load of outdated rows made it all of the buffer.
    SgxdBackend::PicaTarget pass_drawn_target{};
    u32 pass_quarters = 4;                       ///< the pass's pixels to the guest's, in quarters
    static constexpr u32 TopScreenRows = 400;
    u32 pass_margin = 0;  ///< its widescreen margin, in the buffer's rows (Upscaled::margin)
    u32 pass_samples = 1; ///< its samples a pixel (Upscaled::samples)
    bool pass_widened{};  ///< its part as wide as the screen: the margins beside it drawn too
    /// A guest pixel edge in the pass's pixels: the first pixel whose centre is at or past it.
    [[nodiscard]] s32 PassEdge(s32 v) const {
        return (2 * v * static_cast<s32>(pass_quarters) + 3 + (v < 0 ? -7 : 0)) / 8;
    }
    /// The same for the target's x (the buffer's rows), past the widescreen margin; `widen` takes the
    /// margins beside a part.
    [[nodiscard]] s32 PassX(s32 v, s32 widen = 0) const {
        return PassEdge(v + static_cast<s32>(pass_margin) + widen * static_cast<s32>(pass_margin));
    }
    /// The pass stores all of its depth (its draws kept off the margin by the ISP's mask or a kill):
    /// guest memory's depth untiled into the store. A scaled pass keeps the scratch and the copy
    /// instead (with the mask, a scaled pass's 3D hangs).
    [[nodiscard]] bool PassStoresWholeDepth() const {
        return pass_depth_guest;
    }
    /// What a draw has to cover for nothing of its target to be loaded: the part it stores, or all of
    /// the target when scaled, widened or narrow.
    [[nodiscard]] StorePart PartToCover() const {
        return pass_quarters == 4 && pass_margin == 0 && !pass_narrow ? pass_part : StorePart{};
    }
#ifdef VITA_DIAGNOSTICS
    const char* pass_opened = ""; ///< passlog: clear, covered, load
#endif
    const Upscaled* upscaled_writing = nullptr; ///< its pass end's own TexturesWritten
    /// The buffer's picture in PictureShapeFor's shape; null for a buffer not drawn into one.
    Upscaled* UpscaledFor(const ColourBuffer& target, PAddr depth);
    /// A scaled pass's side at most: the size a recipe's block holds.
    static constexpr u32 ScaledSideMax = 1024;
    /// Whether target can be drawn at quarters / 4 with margin rows each side (UpscaledFor).
    [[nodiscard]] static bool ScalesWhole(const ColourBuffer& target, u32 quarters, u32 margin);
    /// The part of buffer a draw onto it draws: all of it, or with the top screen upscaled and the
    /// buffer too large to scale whole, the rows from a top-screen transfer's first to its end.
    [[nodiscard]] ColourBuffer TopWindowFor(const ColourBuffer& buffer) const;
    struct ScreenPicture {
        PAddr output;  ///< the transfer's output: the LCD buffer, or the start of the one the
                       ///< LCD points into
        u32 bytes, row_bytes;
        SgxdBackend::Data picture;
        u32 width, height; ///< the output's rows by its columns scaled, width up to 8s (its rows
                           ///< apart)
        u32 quarters;
        u32 margin; ///< the picture's widescreen margin, in the screen's rows
        bool flip;  ///< the transfer's flip_vertically: the output's rows the other way round
        bool valid; ///< it holds the output's bytes: no write since but its transfer's
        /// The output's guest bytes are older than the picture: they go there when something reads them.
        bool owned = false;
        /// Owned, and the output's guest bytes hold it too (a GPU read had them copied): still the screen's
        /// picture, its output closed to the guest, whose first access lets it go.
        bool copied = false;
        u32 pack = 0;                   ///< the output's format, as the PBE packs it
        u32 format = 0;                 ///< the output's format (a Pica::PixelFormat)
        /// The picture's texels: an 8-bit output's U8888, a 16-bit one's in the output's format.
        SgxdBackend::PictureFormat picture_format{};
        u32 out_width = 0, out_height = 0; ///< the output's columns and rows
        u32 last_used = 0;              ///< display_transfers at its last transfer (eviction)
    };
    std::vector<ScreenPicture> screen_pictures;
    /// Display outputs whose first or last page holds other data the guest keeps accessing: a screen
    /// picture owning the output would close that page from each transfer, and nearly every access
    /// would abort. A transfer into one from a picture of the guest's own size goes into guest memory.
    std::vector<PAddr> outputs_beside;
    static constexpr u32 OutputsBesideMax = 8;
    /// The screen pictures at most: past that the oldest is copied into guest memory if it owns its
    /// bytes, and let go.
    static constexpr u32 ScreenPicturesMax = 8;
    u32 display_transfers{};
    /// A display transfer of input into a top-screen-sized output: the input is the top screen's source.
    void NoteTopSource(PAddr input);
    /// A display transfer of out_width x out_height into output from u's picture (from its memory row
    /// first_row, scale_x x scale_y averaged): the output's
    /// screen picture takes it at u's scale, owns the output's bytes, and is what the present shows.
    void ScreenTransfer(const Upscaled& u, u32 first_row, PAddr output, u32 out_width,
                        u32 out_height, u32 bpp, u32 pack, u32 format,
                        const SgxdBackend::PictureFormat& picture_format, u32 scale_x,
                        u32 scale_y, bool flip);
    static RasterizerSgxd* upscale_instance;

    ColourBuffer pass_colour{}; // while the backend is in a pass
    StorePart pass_part{};      // what its end of tile stores (its draws' viewport)
    StorePart pass_mask{};      // its draws' viewport off the store's grid, masked (MaskOutside)
    bool pass_margin_masked{};  // masked to the part it stores from its first draw on
#ifdef VITA_DIAGNOSTICS
    u32 viewport_masks{}; // perflog: draws whose narrower viewport kept the pass (masked)
#endif
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
    u32 pass_open_draws = 0;    // the backend's draws when Draw opened the pass (its loads)
    u16 gs_regs_loaded = 0;     // the GS input registers point-mode draws have loaded
    /// Each GS input register's row as the last draw that loaded it left it.
    std::array<u32, 16> gs_last_rows{};
    /// The rows KeepGsRows copied out of a room a pass took, a 16-byte row a register.
    SgxdBackend::Data gs_kept_rows{};
    /// The geometry unit's temporaries and emit buffer between draws run in order, zero at first:
    /// two blocks, gs_unit_current's read. A draw of trails (VsRequest::gs_trails) stores into the
    /// other, so no instance that ends early stores over what the draw's first one still loads.
    std::array<SgxdBackend::Data, 2> gs_unit_temps{};
    u32 gs_unit_current = 0;
    /// A geometry draw's row no attribute fills: zeros, kept for good.
    SgxdBackend::Data gs_zero_row{};
    std::vector<u16> gs_list;
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
    bool pass_depth_written{};  // a draw wrote depth or stencil
    bool pass_depth_load{};     // it starts from the depth an earlier pass stored
    bool pass_depth_covered{};  // or a draw wrote every pixel's depth before any read it
    bool pass_depth_part{};     // ... of the part it stores, the rest kept in its store
    bool pass_depth_guest{};    // or from guest memory's, untiled into the store
    u32 guest_depth_loads{};
    u32 scaled_depth_loads{}; ///< of guest_depth_loads, the magnified ones (UntileDepthScaled)
    u32 window_misses{}; ///< draws reaching before the top screen's window (TopWindowFor), logged
    /// A W-buffered draw's depth through WBUFFEN is c0 + c1 / w: 1 - 1/w where the PICA's W depth
    /// grows with w, else 1/w. That depth is alpha * w + beta for a perspective projection, fitted
    /// from a draw's vertices (FitWDepth), for MapZOnW.
    struct WDepthMap {
        bool fitted = false;
        bool grows = true;
        float alpha = 0.0f, beta = 0.0f;
        /// The least w the PICA's planes leave of the fitted projection (w_depth_k), or NaN.
        float near_w = std::numeric_limits<float>::quiet_NaN();
    };
    bool pass_ta_w{};        ///< the pass's depth holds the TA's W depths
    WDepthMap pass_w_map{};  ///< ... and their map, once a W-buffered draw was fitted
    /// The parked pass's pass_ members, traded by TradePassState.
    struct ParkedPassState {
        ColourBuffer colour{};
        StorePart part{}, mask{};
        bool margin_masked{};
        const NarrowScratch* narrow = nullptr;
        u32 open_draws = 0;
        PAddr depth{};
        bool depth_cleared{};
        u32 depth_bytes{4};
        u32 depth_format{};
        float bg_depth{1.0f};
        u32 bg_stencil{};
        bool depth_used{}, depth_written{}, depth_load{}, depth_covered{}, depth_part{},
            depth_guest{};
        bool ta_w{};
        WDepthMap w_map{};
        Upscaled* upscaled = nullptr;
        SgxdBackend::PicaTarget scaled_target{};
        SgxdBackend::PicaTarget drawn_target{};
        u32 quarters = 4, margin = 0, samples = 1;
        bool widened{};
        PAddr drawn_lo = 0, drawn_hi = 0;
        s32 clear_site = -1;
        u32 guest_draws = 0;
#ifdef VITA_DIAGNOSTICS
        const char* opened = "";
#endif
    };
    ParkedPassState parked_pass{};
    void TradePassState();
    /// A draw onto other buffers than the open pass's: the open pass parked and the draw's opened
    /// beside it, or traded for the parked one when that is the draw's, else both ended.
    void SwitchPass(const ColourBuffer& target, PAddr depth, u32 depth_bytes, bool uses_depth,
                    bool geometry);
    /// Whether a pass on target may be parked or drawn beside a parked one: at 1x, one sample, no
    /// margin, into the buffer itself.
    [[nodiscard]] bool ParksAt1x(const ColourBuffer& target, PAddr depth) const;
#ifdef VITA_DIAGNOSTICS
    u32 pass_parks{}; ///< perflog: passes parked beside another
    u32 pass_trades{}; ///< perflog: draws that traded the open pass for the parked one
#endif
    /// The first W-buffered draw of a pass that writes depth (and the next, until one fits): up to six
    /// vertices on the CPU, z against w, into pass_w_map.
    void FitWDepth(bool is_indexed, const float* immediate, u32 count, bool grows);
    /// A Z-buffered draw testing depth against the TA's W depths: its depth for the ISP is the TA's
    /// depth of the w where the two are equal, set in `viewport` (state words 16, 17). Built for a quad
    /// of one depth and for layers behind the scene; false, asserted, for anything else.
    bool MapZOnW(bool is_indexed, const float* immediate, u32 count, float* viewport);
    std::vector<Pica::OutputVertex> zw_vertices; ///< MapZOnW's draw on the CPU
    /// Depth buffers kept across passes (EndPass stores a pass's depth, the next pass without a clear
    /// loads it).
    struct DepthStore {
        PAddr addr;
        u32 width, height, format; // the guest buffer's
        u32 quarters;              // the store's samples a pixel side, in quarters: 4 at 1x
        u32 margin;                // the widescreen margin it is wider by, the buffer's rows
        u32 samples;               // samples a pixel: 2 for a 2x pass's (its rows twice over)
        SgxdBackend::DepthStore store;
        bool valid; // the last pass on the buffer stored it, and nothing wrote there since
        // A pass storing part of its target stores its depth here, then that part into store.
        SgxdBackend::DepthStore scratch{};
        /// A clear quad's depth and stencil over part of the store, not written yet: the PTLA fills it once
        /// a pass uses the depth (SettleDepthClear), and a clear of the same part replaces it unwritten.
        struct PendingClear {
            StorePart part;
            float z;
            u32 stencil;
            bool has_z, has_stencil;
        };
        std::optional<PendingClear> pending{};
        bool ta_w = false;
        WDepthMap ta_map{};
        float ta_k = 1.0f;
        /// A pass stored it since guest memory last took its depth: the guest's bytes are older.
        bool guest_stale = false;
        /// With guest_stale, the buffer's 8x8 tiles a part's write-back gave their depth since, a bit each.
        std::vector<u64> fresh{};
        /// words holds the store's depth as it is now, at its scale: a texture over the buffer is copied
        /// from it. A pass storing the depth, a pending clear, or any write over the bytes clears it.
        bool words_current = false;
        /// What PublishOwned last published: bit 0 valid, bit 1 newer than guest memory (its bytes closed
        /// to the guest and the HLE).
        u8 published = 0;
        [[nodiscard]] u8 GuardState() const {
            return static_cast<u8>((valid ? 1 : 0) | (valid && guest_stale ? 2 : 0));
        }
        void SetGuestStale(bool stale) {
            guest_stale = stale;
            fresh.clear();
            if (stale) {
                words_current = false;
            }
        }
        /// The guest's bytes at the buffer's two ends on the FCRAM pages it shares with other data, as the
        /// last pass found them: other bytes there before a write-back are the guest's writes since. Empty
        /// for whole pages, outside FCRAM, or while a GPU write to the bytes may be on its way.
        std::vector<u8> edges{};
        /// The GPU was sent a write to the buffer's bytes since the last pass on it; the edges are read
        /// once the GPU is past it.
        bool gpu_wrote = false;
        std::optional<u32> gpu_write{};
        /// WriteDepthBack's I24ZI8S words (D16's I16ZI16Z in their first half), twice the size when
        /// scaled or 2x.
        SgxdBackend::Data words{};
        [[nodiscard]] bool Scaled() const {
            return quarters != 4 || margin != 0;
        }
    };
    /// A W-depth buffer's write-back: the guest bytes the words left, 1 - K / w (or K / w), turned
    /// into the PICA's depths on the CPU.
    void WDepthToGuest(const DepthStore& d);
    /// Twice the store's rows: the words' renders put their pixels on the second half.
    [[nodiscard]] static u32 WordsRows(const DepthStore& d) {
        return d.store.height * 2;
    }
    std::vector<DepthStore> depth_stores;
    /// The D24S8 store an RGBA8 texture is copied from at the store's scale instead of untiled: one
    /// level of whole rows from first_row; null for none.
    [[nodiscard]] const DepthStore* DepthWordsForTexture(PAddr addr, u32 width, u32 height,
                                                         u32 levels, bool cube, u32 parts,
                                                         u32& first_row) const;
    [[nodiscard]] DepthStore* DepthWordsForUnit(u32 unit);
    /// A sampled unit's read of a store's words: made from the store if older, guest memory left as
    /// it is (closed to the guest). Whether the words serve the unit: else the caller writes back.
    bool WriteDepthWords(u32 unit, const char* why);
    std::vector<u8> w_depth_scratch; ///< WDepthToGuest's cached copy of the buffer
    DepthStore& DepthStoreFor(PAddr addr, u32 width, u32 height, u32 format, u32 quarters = 4,
                              u32 margin = 0, u32 samples = 1);
    /// Whether a store of that shape holds the buffer's depth (a pass may load it).
    bool DepthStoreValid(PAddr addr, u32 width, u32 height, u32 format, u32 quarters,
                         u32 margin, u32 samples = 1) const;
    DepthStore* DepthStoreOf(PAddr addr, u32 width, u32 height, u32 format);
    void SettleDepthClear(DepthStore& d);
    SgxdBackend::DepthStore NewDepthStore(u32 width, u32 height, u32 format,
                                          bool upscaled = false, u32 samples = 1);
    void DepthStoresWritten(PAddr start, PAddr end);
    /// The bytes [addr, end) has on its first page before a page boundary and on its last after one.
    [[nodiscard]] static std::pair<u32, u32> DepthEdges(PAddr addr, PAddr end);
    /// A pass stored the buffer's depth: its edges as guest memory holds them now.
    void NoteDepthEdges(DepthStore& d);
    /// Before a write-back over [start, end): a store whose edges changed since its last pass is
    /// stale (the guest wrote them).
    void DepthEdgesWritten(PAddr start, PAddr end);
    /// A GPU read of [start, end) (a transfer's input, a texture): a depth store over it newer than
    /// guest memory goes into the guest's buffer first, as the PICA's depth buffer would hold it.
    void WriteDepthBack(PAddr start, PAddr end, const char* why);
    /// The depth stores newer than guest memory over [lo, hi), as their whole buffers (a guest access
    /// writes them back whole).
    [[nodiscard]] std::vector<std::pair<PAddr, PAddr>> GuardedDepths(PAddr lo, PAddr hi) const;
    /// PublishOwned again when a store's guard state is not the one published: a store that became
    /// newer must be closed before the pass's holds let its pages go.
    void DepthGuardsChanged();
    [[nodiscard]] bool DepthBackDue(PAddr start, PAddr end) const;
    /// A GPU read of [start, end): the rows pictures own there go into the guest's buffer first, at 1x
    /// (but except's). Any pass open ends once a copy is due. A screen picture's output is copied once
    /// and stays its; for_guest, the access is the guest's or the HLE's and the picture lets it go.
    void WriteColourBack(PAddr start, PAddr end, const char* why,
                         const Upscaled* except = nullptr, bool for_guest = false);
    [[nodiscard]] static std::pair<PAddr, PAddr> StoredBytes(const Upscaled& u,
                                                             const SgxdBackend::PicaTarget& t);
    /// A GPU write into [start, end) other than a pass on the buffer: a stale picture it covers whole
    /// is guest memory's again, one it covers in part goes into the guest's buffer first.
    void ColourWritesDue(PAddr start, PAddr end, const char* why);
    u32 colour_writebacks{};      ///< WriteColourBack's copies, for the log's first few
    u32 colour_writebacks_mark{}; ///< colour_writebacks at perflog's last second
    u32 picture_transfers{};      ///< display transfers read from a picture (TransferFromPicture)
    u32 picture_transfers_mark{}; ///< picture_transfers at perflog's last second
    u32 picture_textures{};       ///< textures copied from a picture (TextureFromPicture)
    u32 scaled_wraps{}; ///< diagnostics: scaled copies read with repeat or mirror (TextureWords)
    u32 depth_word_renders{}; ///< WriteDepthWords' renders, for the log's first few
    u32 picture_textures_mark{};  ///< picture_textures at perflog's last second
    u32 depth_writebacks{};      ///< WriteDepthBack's, for the log's first few
    u32 depth_edge_writes{};     ///< DepthEdgesWritten's finds, for the log's first few
    u32 depth_writeback_kinds{}; ///< the kinds logged (D16, 2x, scaled: a bit each combination)
    u32 depth_writebacks_mark{}; ///< depth_writebacks at perflog's last second
    u32 depth_writeback_parts{}; ///< of them a part's (perflog's line, the second's)
    /// The part of d's buffer holding start..end, in the store grid's steps; x1 0 for most of it.
    [[nodiscard]] static StorePart DepthPartFor(const DepthStore& d, PAddr start, PAddr end);
    /// Whether every tile of d's buffer under start..end was written back since its last pass.
    [[nodiscard]] static bool DepthTilesFresh(const DepthStore& d, PAddr start, PAddr end);
    void MarkDepthPartFresh(DepthStore& d, const StorePart& part);

    u32 fills{};
    u32 dmas{};
    u32 draws{};
    u32 gs_draws{};
    u32 passes{};
    u32 frames{}; ///< guest frames ended (EndFrame)
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
        u32 lookups{};        ///< TextureFor's calls
        u32 memo_hits{};      ///< ... answered by a memo
        u32 writes{};         ///< TexturesWritten's writes
        u32 stamped{};        ///< ... over a texture's pages, which stamped them
        u64 write_pages{};    ///< the pages they stamped
        u32 checks{};         ///< lookups that read their pages' stamps (a write since their last)
        u64 check_pages{};    ///< the pages read
        u32 written{};        ///< ... that found a page written since
        u32 shared{};         ///< ... by writes none of which overlapped the texture's bytes
        u32 untold{};         ///< ... by writes older than write_ring: not told apart
        u32 null_samples{};   ///< samplers of a unit that is off (NullTexture)
        u32 tiny{};           ///< textures with a side under 8, decoded on the CPU
        u64 tiny_texels{};    ///< ... their texels
        u64 tiny_us{};        ///< ... the render thread's time in them
        u32 tiny_logged{};    ///< the first ones logged (carried across lines)
        u64 start_us{};
        u32 passes_mark{};    ///< passes at the second's start: the textures draws took since
        u64 evictions_mark{}; ///< evictions at the second's start
        std::vector<std::pair<const char*, u32>> pass_ends;
        std::vector<std::pair<const char*, u32>> writeback_reasons; ///< WriteColourBack's copies by why
    } texture_stats;
    struct EmitStats {
        std::array<u32, 3> programs{};
        std::array<u64, 3> us{};
        u32 flushes_mark{};
        std::array<u32, 3> taken{}; ///< from the first tier's workers (NoteTier1)
        u64 taken_us{};
        std::array<u32, 3> from_disk{}; ///< read from the disk by them
        u64 disk_us{};
        u64 waited_us{};
        u32 helped{};
    } emit_stats;
    /// The last writes' ranges: whether a texture a check found written was, or only shares a page.
    std::array<std::pair<PAddr, PAddr>, 256> write_ring{};
    void LogTextureStats();
    /// perflog: draws with the PICA's user clip plane on (sgxd gives the TA none).
    u32 clip_plane_draws{}, clip_plane_all{};
    std::array<float, 4> clip_plane_last{};
    /// perflog: draws whose clip z ClipZSign flipped, left as they were (oblique rows), or had no rows.
    u32 clip_z_reversed{}, clip_z_oblique{}, clip_z_unknown{}, clip_z_all{};
#endif
};

} // namespace SgxdRenderer
