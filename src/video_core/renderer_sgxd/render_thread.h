// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include "common/common_types.h"
#include "common/named_thread.h"
#include "common/process_clock.h"
#include "common/thread.h"
#include "video_core/pica/pica_core.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_sgxd/page_holds.h"

namespace Memory {
class MemorySystem;
}

namespace SgxdRenderer {

class RasterizerSgxd;
class RendererSgxd;

/**
 * The render thread, on Vita core 1. The
 * emulation thread owns the PICA: it parses the command lists, keeps the registers, the shader
 * setups and the LUTs, and raises the interrupts. This thread owns everything behind the rasterizer
 * interface: the GXM rasterizer, sgxd, the guest's GPU memory map and the presents.
 *
 * To the emulation side's PicaCore and GPU this class is the rasterizer. Each call it gets is
 * a record in a ring, behind a record of the PICA state that changed since the last one
 * (registers by the dirty table, uniforms, program and swizzle words, LUTs, default
 * attributes). The thread applies the state to its own PicaCore, which the GXM rasterizer was
 * made with, and makes the call there, in order.
 *
 * Tickets: each GPU interrupt and vblank takes one (InterruptFence) for the work queued before it;
 * the interrupt goes to the guest at once. The pages that work touches stay held until the ticket
 * is reached. While tickets wait the thread sleeps until sgxd wakes it for the oldest one's fence.
 *
 * Every producer holds the GPU's lock (VideoCore::GPU::Lock): the state records read the
 * emulation side's PICA, and the ring has one writer at a time.
 *
 * The window is this thread's too: each VBlank's present record polls its input and presents,
 * and with no present for WindowPollInterval the thread polls it on its own. The PS button's
 * pause menu runs inside such a poll (EmuWindowVita::PollEvents), on this thread, while the
 * emulation waits for it.
 *
 * Held: the work that must run off this thread (FCRAM page moves, which need the guest stopped;
 * vprof's capture and replay; savestates; settings) parks the thread with the ring drained
 * (Hold). Until Release, the holder's thread is the render side: each record runs as it is
 * written, and the interrupts' fences are polled there.
 */
class RenderThread final : public VideoCore::RasterizerInterface {
public:
    RenderThread(Pica::PicaCore& pica, Pica::PicaCore& render_pica, RendererGxm& renderer,
                 RasterizerGxm& rasterizer, Memory::MemorySystem& memory);
    ~RenderThread() override;

    RenderThread(const RenderThread&) = delete;
    RenderThread& operator=(const RenderThread&) = delete;

    struct GuestRange {
        PAddr addr;
        u32 bytes;
    };
    using DrawTextures = std::array<GuestRange, 8>;

    // The rasterizer interface, for the emulation side. Each call queues; the Accelerate ones
    // answer true, as RasterizerSgxd's always do (it stops the emulator on what it cannot do).
    void AddTriangle(const Pica::OutputVertex& v0, const Pica::OutputVertex& v1,
                     const Pica::OutputVertex& v2) override;
    void DrawTriangles() override;
    // RasterizerSgxd's are empty: its surfaces reach guest memory only through the operations
    // that write them there. An invalidate is the HLE about to write the range (Y2R's conversion,
    // a DMA): T12's textures over it are untiled again (InvalidateWritten), as for a write the
    // page holds catch.
    void FlushAll() override {}
    void FlushRegion(PAddr, u32) override {}
    void InvalidateRegion(PAddr addr, u32 size) override {
        InvalidateWritten(addr, size);
    }
    void FlushAndInvalidateRegion(PAddr addr, u32 size) override {
        InvalidateWritten(addr, size);
    }
    void ClearAll(bool) override {}
    bool AccelerateDisplayTransfer(const Pica::DisplayTransferConfig& config) override;
    bool AccelerateTextureCopy(const Pica::DisplayTransferConfig& config) override;
    bool AccelerateFill(const Pica::MemoryFillConfig& config) override;
    bool AccelerateDma(PAddr src, PAddr dst, u32 size) override;
    bool AccelerateDrawBatch(bool is_indexed) override;
    bool AcceleratesGeometryShaders() const override {
        return true;
    }
    bool AccelerateImmediateVertex(const Pica::AttributeBuffer& input) override;
    void FlushImmediate() override;
    void NotifyP3D() override;
    void GpuIdle() override;
    /// A ticket for the work queued so far (the page holds made since the last one are its).
    u64 InterruptFence() override;
    /// Whether the ticket's work is done. Any thread, without a lock.
    bool InterruptPoll(u64 ticket) override;
    /// Waits until it is; without the GPU's lock unless this thread is the render side.
    void InterruptWait(u64 ticket) override;
    void WaitForGpuWrites() override;

    /// The page holds, emulation side, under the GPU's lock. On from the guest table's install.
    void EnablePageHolds();
    /// Waits for the GPU writes queued to the FCRAM pages of [addr, addr + size) (a page move's
    /// copy, GuestGpuMemory::MoveHooks).
    void WaitGpuWrites(PAddr addr, u32 size);
    /// The guest's view gets the holds the work queued since made, at the end of each GPU operation and
    /// vblank.
    void ApplyPageHolds();
    /// Lets go of the holds whose work is done. Between slices.
    void ReleasePageHolds();
    /// A guest access aborted on the page of `host`: whether a hold refused it. If so, the work it
    /// waited for is done and the page open, and the guest runs the access again.
    bool PageFault(const u8* host, bool write, u32 guest_pc, u32 guest_va);
    /// The HLE reads or writes [host, host + size) of guest memory through its own view: it
    /// waits for the queued GPU work the guest would wait for (Memory's host guard).
    void HostAccess(const u8* host, u32 size, bool write);
    /// T12: [addr, addr + size) written by the HLE through a pointer the page holds do not see:
    /// each watched page's textures untiled again at their next draw, the page unwatched.
    void InvalidateWritten(PAddr addr, u32 size);
    /// A guest window was mapped over [host, host + size): its descriptors take the holds.
    void HostRemapped(const u8* host, u32 size);
#ifdef VITA_DIAGNOSTICS
    /// Since the last call (`perflog`): T12's texture pages written by the guest (aborts) and
    /// the texture pages the HLE wrote (InvalidateWritten).
    std::pair<u32, u32> TakeTextureWrites();
#endif
    /// A vblank: a frame starts for the page versions (changed pages remapped, versions armed again).
    void StartFramePageHolds();
    /// A byte a page of FCRAM then VRAM, nonzero where the guest's view is held.
    [[nodiscard]] const u8* PageHeldMap() const {
        return page_holds.HeldMap();
    }
#ifdef VITA_DIAGNOSTICS
    struct FaultSite {
        u32 count;
        u64 us;
        PAddr page;
    };
    struct HoldStats {
        PageHolds::Stats holds;
        u32 faults, fault_waits;
        u32 versioned_faults; ///< of the aborts, those a page version took
        u32 geometry_moves;   ///< geometry pages moved to CDRAM after a write waited on them
        u64 fault_wait_us;
        u32 host_waits;
        u64 host_wait_us;
        std::array<u32, PageHolds::Kinds> fault_kinds;
        std::array<u64, PageHolds::Kinds> fault_kind_us;
        struct Site {
            u32 pc;
            PageHolds::Kind kind;
            FaultSite at;
        };
        std::array<Site, 6> sites;
        u32 site_count;
    };
    HoldStats TakeHoldStats();
#endif
    std::vector<std::pair<PAddr, u32>> TakeCachedReads() override;
    void LoadDefaultDiskResources(const std::atomic_bool& stop_loading,
                                  const VideoCore::DiskResourceLoadCallback& callback) override;
    void SwitchDiskResources(u64 title_id) override;

    void SetRendererAccurateMul(bool accurate_mul);
    /// Runs fn on the render side after the work queued before it: Run without waiting, RunSync
    /// waiting (at once on the render side itself).
    void Run(std::function<void()> fn);
    void RunSync(const std::function<void()>& fn);
    /// A present of the guest's screens as the VBlank found them. One with two newer ones queued is
    /// dropped (one newer is the normal lag), and one within 12 ms of the last.
    void Present(const std::array<Pica::FramebufferConfig, 2>& fb,
                 const std::array<Pica::ColorFill, 2>& fill);

    /// A guest frame's end, each vblank, presented or not. The late skip decides here whether the next
    /// frame is late.
    void EndFrame();
    void FrameTicket(u64 ticket);

    /// Parks the thread once it has run everything queued; nests. Under the GPU's lock, which
    /// the caller may let go of and take again while the hold lasts.
    void Hold();
    void Release();
    /// Whether a hold is on (the GPU's lock held). The holder may then use the rasterizer, the
    /// renderer and sgxd directly.
    [[nodiscard]] bool Held() const {
        return hold_depth != 0;
    }
    /// Under a hold, on the holder's thread: polls the fences, which nothing else does then.
    /// Otherwise nothing.
    void PollHeld();

    RasterizerSgxd& Rasterizer() {
        return rasterizer;
    }

    /// The emulation side's waits for the thread, by kind: an interrupt ticket's work, ring
    /// space, a RunSync (a hold's park is apart: Stats::holds).
    enum EmuWait : u32 { EmuTicket, EmuSpace, EmuSync, EmuWaits };

    /// perflog's GPU sampler cells (Stats::gpu_at): sgxd_mkq's job kinds and wait kinds
    /// (SGXD_MKQ_KINDS, SGXD_MKQ_WAITS), each twice (a 3D's end latched or not), after idle and
    /// a held 3D running or done.
    static constexpr u32 GpuAtKinds = 5;
    static constexpr u32 GpuAtWaits = 5;
    static constexpr u32 GpuAtIdle = 0;
    static constexpr u32 GpuAtHeldRunning = 1;
    static constexpr u32 GpuAtHeldDone = 2;
    static constexpr u32 GpuAtBusy = 3;
    static constexpr u32 GpuAtCells = GpuAtBusy + GpuAtKinds * GpuAtWaits * 2;

    /// Since the last call (`perflog`; diagnostics builds only, as every counter behind it):
    /// the records the thread ran by kind, its time running them and waiting for work (us),
    /// sgxd's wakes for a ticket's GPU work and the sleeps with tickets pending that timed out
    /// instead, the presents, and the holds with their waits for the thread to drain and park
    /// (us). Timed by batch, not by record. The emulation side's waits by EmuWait: the waits
    /// that did not return at once, their time (us), and their sleeps on `done` with those that
    /// ended at the 1 ms timeout rather than a wake.
    struct Stats {
        enum Kind : u32 { State, Draw, Fill, Transfer, Dma, Present, Other, Count };
        std::array<u32, Count> records;
        u64 busy_us;
        u64 idle_us;
        u32 gpu_wakes;
        u32 timeouts;
        u32 presents;
        u32 presents_behind;
        u32 presents_soon;
        u32 late_frames;
        u32 late_waits;
        u64 late_wait_us;
        u64 lead_frames_sum;
        u32 lead_samples;
        u32 lead_frames_max;
        u32 lead_bytes_max;
        u32 late_draws;
        u32 late_kept;
        u32 merged_draws; ///< the parse's merged runs' vertices (MergeDraw), and the runs
        u32 merged_runs;
        u32 late_partial;
        u32 gpu_samples;
        u32 gpu_busy;
        u32 gpu_held;
        std::array<u32, GpuAtCells> gpu_at;
        std::array<u32, GpuLabels> gpu_3d_by;
        std::array<u32, GpuLabels> gpu_ta_by;
        std::array<u32, GpuLabels> jobs_by;
        std::array<u32, 4> present_gaps;
        u32 holds;
        u64 hold_us;
        std::array<u32, EmuWaits> emu_waits;
        std::array<u64, EmuWaits> emu_wait_us;
        std::array<u32, EmuWaits> emu_sleeps;
        std::array<u32, EmuWaits> emu_timeouts;
    };
    Stats TakeStats();
#endif

private:
    enum class Op : u16;
    struct Head;
    struct Pending {
        u32 ticket;
        u64 fence;
    };

    /// Writes the PICA state that changed since the last call as records. Before every call
    /// record that may read it.
    void SyncState();
    /// gxm_flags `rendercheck`: before each draw the emulation side's whole PICA state, which the
    /// render side compares with its copy, logging each register, uniform and table that
    /// differs once.
    void PutCheck();
    void RunCheck(const u8* payload);
    bool check{};
    /// A record of `bytes` payload bytes; the returned pointer is where they go.
    u8* Begin(Op op, u32 bytes, u16 extra = 0);
    void Commit();
    /// The records closed so far to the render thread: a seq-cst store of `published`, then the wake.
    void Publish();
    /// A draw's records published together when it goes out of scope: one seq-cst store and sleep
    /// check a draw. A wait for the render thread inside publishes first.
    struct PublishTogether {
        explicit PublishTogether(RenderThread& t) : thread{t} {
            thread.publish_together = true;
        }
        ~PublishTogether() {
            thread.publish_together = false;
            if (thread.published.load(std::memory_order_relaxed) != thread.write) {
                thread.Publish();
            }
        }
        RenderThread& thread;
    };
    bool publish_together{};
    template <typename T>
    void Put(Op op, const T& payload, u16 extra = 0);
    /// A record with no payload. A flag goes through Begin (an integer here would be the payload).
    void Put(Op op);

    void RunAvailable();
    void RunRecord(const Head& head, const u8* payload);
#ifdef VITA_DIAGNOSTICS
    static Stats::Kind KindOf(Op op);
#endif
    void PollFences();
    void Loop();

    void NotifyDone();
    /// The thread's sleep on `wake_flag`, at most `timeout`: false on the timeout. Wake sets it.
    bool Sleep(std::chrono::microseconds timeout);
    void Wake();

    Pica::PicaCore& pica;
    Pica::PicaCore& render_pica;
    RendererSgxd& renderer;
    RasterizerSgxd& rasterizer;
    Memory::MemorySystem& memory;

    static constexpr u32 RingSize = 2 * 1024 * 1024;
    std::unique_ptr<u8[]> ring;
    /// Byte positions, wrapping at 2^32. write: the producers' (under the GPU's lock), published as
    /// `published`; read: the render side's.
    u32 write{};
    u32 open_record{}; ///< Begin's record, until Commit
    std::atomic<u32> published{};
    std::atomic<u32> read{};

    std::deque<Pending> pending;
    u32 tickets{};                  ///< the last ticket handed out (GPU's lock)
    std::atomic<u32> reached{};     ///< the last ticket whose work is done

    PageHolds page_holds;
    bool page_holds_on{};
    u32 last_write_ticket{}; ///< the last ticket whose work writes guest memory
    /// A draw's colour write and depth read holds, made once a ticket and frame (the draws of a list
    /// hold the same buffers).
    struct BufferHold {
        PAddr addr;
        u32 bytes;
        u32 ticket;
    };
    BufferHold held_colour{};
    BufferHold held_depth{};
    [[nodiscard]] u32 HoldTicket() const {
        return tickets + 1;
    }
    void HoldRead(PAddr addr, u32 bytes, PageHolds::Kind kind);
    void HoldWrite(PAddr addr, u32 bytes, PageHolds::Kind kind);
    void HoldDraw(const DrawTextures& textures, u32 count, bool vertices, bool is_indexed);
    /// A geometry draw's indices into a record of their own (Op::GsIndices), before its Draw.
    void PutGsIndices();
    void HoldVertices(bool is_indexed);
    /// The scan's memo of 16-bit index lists' ranges, valid while their pages' write stamps stand.
    struct IndexMemo {
        PAddr addr;
        u32 count;
        u32 stamp;
        u16 lo, hi;
    };
    std::array<IndexMemo, 4096> index_memo{};
    /// A draw's 16-bit indices' range as HoldVertices scanned it (scanned 0: not), sent with the draw
    /// so the render thread does not scan them again.
    struct DrawIndexRange {
        u16 lo;
        u16 hi;
        u32 scanned;
    };
    DrawIndexRange draw_indices{};
    /// The GPU remaps the last holds asked for, as records before the work's.
    void PutPageRemaps();
    /// Waits for the work a guest access to the page waits for, the open holds' ticket fenced first.
    void WaitHeldPage(u32 page, bool write);
    /// A texture page the HLE wrote: the render side marks the textures over it written
    /// (RasterizerSgxd::TexturesWritten), in order with the work.
    void TextureWritten(u32 page);
#ifdef VITA_DIAGNOSTICS
    u32 texture_write_faults{}, texture_write_hle{};
#endif
#ifdef VITA_DIAGNOSTICS
    u32 faults{}, fault_waits{}, host_waits{}, versioned_faults{};
    u32 geometry_moves{};
    u64 fault_wait_us{}, host_wait_us{};
    std::array<u32, PageHolds::Kinds> fault_kinds{};
    std::array<u64, PageHolds::Kinds> fault_kind_us{};
    std::unordered_map<u64, FaultSite> fault_sites; ///< by the guest's PC << 8 | the kind
#endif

    /// The thread's state (a hold, the end) is under `work`. It sleeps on `wake_flag`, an event
    /// flag set for records (Commit sees `sleeping`), a hold or the end, and by sgxd when the GPU
    /// has done the oldest pending ticket's work (sgxdWaker, from IRQ 52): no timed polls. A
    /// hold's park waits on `work`; everyone else waits on `done`.
    Common::CoreCondition work;
    s32 wake_flag{-1};
    static constexpr u32 WakeBit = 1;
    Common::CoreCondition done;
    std::atomic<bool> sleeping{};
    std::atomic<u32> done_waiters{};
    std::atomic<bool> space_waiting{};
    bool park_requested{};      ///< under `work`
    std::atomic<bool> parked{}; ///< set under `work`, waited for on `done`
    bool stopping{};            ///< under `work`
    u32 hold_depth{};      ///< the GPU's lock
    /// The holder's thread while a hold is on, else -1: only it may run the render side then.
    /// HLE threads that wait for a ticket meanwhile (a page the GPU holds) sleep until it is
    /// reached instead: the backend's waits take one waiter (sgxd's event flag).
    std::atomic<s32> holder{-1};
    /// The thread that writes the ring (the emulation thread): HLE threads may wait, never queue.
    const s32 producer;
    Common::ProcessClock::time_point last_present{};
    /// Presents at most this often: the display shows one picture each 16.7 ms.
    static constexpr std::chrono::microseconds PresentInterval{12000};
    /// A present two newer ones would replace is shown anyway past this long without one: a GPU-bound
    /// scene keeps the thread that far behind.
    static constexpr std::chrono::microseconds PresentStarved{33000};
    /// A present within PresentInterval of the last, or while the last one is still on the GPU, shown
    /// once both allow unless a newer one takes its place (presents come in bursts).
    bool present_pending{};
    std::array<Pica::FramebufferConfig, 2> pending_fb{};
    std::array<Pica::ColorFill, 2> pending_fill{};
    void ShowDuePresent();
    void ShowPresent(const std::array<Pica::FramebufferConfig, 2>& fb,
                     const std::array<Pica::ColorFill, 2>& fill,
                     Common::ProcessClock::time_point now);
    /// The window's input at least this often while the thread runs, so the PS button answers.
    static constexpr std::chrono::milliseconds WindowPollInterval{16};
    Common::ProcessClock::time_point last_window_poll{};
    std::atomic<u32> presents_queued{};
    /// The late skip (emulation side): the vblank never waits for the GPU, so when the GPU is
    /// TicketFramesBack frames behind, the next frame's busier screen's draws are not recorded and the
    /// game keeps its CPU speed. The screen a late frame keeps (or -1), and an open batch's decision.
    bool late_skip{};
    int late_keep{-1};
    /// Each screen's frames in a row the late skip dropped; past MaxScreenSkips() the frame is drawn
    /// whole, with the vblank waiting for the frame LeadFramesBack before.
    std::array<u32, 2> screen_skips{};
    /// The vblank tickets of the last frames: the next frame is late when the one TicketFramesBack
    /// before the newest is not reached.
    static constexpr u32 TicketFramesBack = 3;
    /// The frame the vblank's wait waits for, one older than the late decision's, so the GPU keeps that
    /// frame's work queued while the guest makes the next.
    static constexpr u32 LeadFramesBack = TicketFramesBack + 1;
    std::array<u64, LeadFramesBack> frame_tickets{};
    u32 frame_ticket_at{};
    bool late_batch_open{};
    bool late_batch_skip{};
    bool late_triangles_open{};
    bool late_triangles_skip{};
    /// The colour buffers drawn lately (the newest 16), each with its segments: a segment is
    /// the draws into it up to a transfer from it. a game draws both screens into one colour
    /// buffer (skiplog: the top screen's draws, its transfer, the bottom screen's draws, its
    /// transfer). Per buffer: this frame's segment, whether a draw of it was dropped, whether
    /// it is open (drawn since its last transfer), and each segment's screen as the transfer
    /// that ended it last found it (-1: none, or no LCD). A segment open at a frame's end goes
    /// on into the next frame with its number and its dropped draws: MH Generations' frames
    /// sometimes draw the bottom screen and the top screen's first draws before a vblank and
    /// the rest of the top screen after it.
    struct SegmentTarget {
        PAddr addr;
        u32 bytes;
        u32 segment;
        bool dropped;
        bool open;
        std::array<s8, 4> screens;
        u8 readers;
        /// segment_draws at its last draw: of the buffers holding a transfer's source, the newest drawn.
        u32 drawn;
#ifdef VITA_DIAGNOSTICS
        u32 dropped_frame; // screenlog_frame of its last dropped draw (latelog)
        u32 kept_frame;    // screenlog_frame of its last kept draw (latelog)
#endif
    };
    std::vector<SegmentTarget> segment_targets;
    /// This frame's and the last frame's outputs of dropped transfers and copies, until a kept write
    /// covers them: a picture never made. Not an LCD buffer's. A transfer or copy that reads one and no
    /// drawn colour buffer is dropped too; one into a drawn colour buffer counts as a dropped draw.
    struct DroppedOutput {
        PAddr lo, hi;
        bool earlier = false; ///< dropped in the last frame
    };
    std::vector<DroppedOutput> dropped_outputs;
    /// This frame's LCD buffers with a dropped write: its later writes into one are dropped too, so it
    /// keeps its last whole picture and stamp.
    std::vector<PAddr> dropped_lcd;
    [[nodiscard]] bool ReadsDroppedOutput(PAddr lo, PAddr hi) const;
    void ForgetDroppedOutputs(PAddr lo, PAddr hi);
    u32 segment_draws{};
    SegmentTarget& SegmentTargetFor(PAddr addr, u32 bytes);
    struct LateTargetCache {
        PAddr addr = 0;
        u32 bytes = 0;
        u32 generation = ~0u;
        int lcd_screen = -1;
        PAddr lcd = 0;
        u32 target = 0;
    };
    LateTargetCache late_cache;
    u32 late_generation{};
#ifdef VITA_DIAGNOSTICS
    struct ScreenlogTarget {
        PAddr addr;
        u32 bytes;
        int screen;
        u32 draws, dropped;
    };
    u32 screenlog_frame{};
    bool screenlog_on{};
    std::vector<ScreenlogTarget> screenlog_targets;
    std::string screenlog_transfers;
#endif
    std::array<Pica::FramebufferConfig, 2> lcd_fb{};
    /// Each screen's LCD buffers (the newest four a present named) with their pictures' stamps; a write
    /// the late skip dropped leaves the stamp. A present shows a screen's selected buffer unless its
    /// picture is older than the one last shown, and then the oldest no older than that.
    std::array<std::vector<LcdBuffer>, 2> lcd_buffers;
    u32 lcd_writes{};
    std::array<u32, 2> shown_stamp{};
    void NoteLcdWrite(int screen, PAddr buffer, bool dropped);
    /// Each screen's GPU cost, in vertices: this frame's, and a running sum (8 x the frame's mean).
    /// A draw adds its vertices and LateDrawCost.
    std::array<u32, 2> frame_cost{};
    std::array<u32, 2> screen_cost{};
    /// A draw's own TA cost in vertices (measured: ~2.2 us a draw, 72 ns a vertex).
    static constexpr u32 LateDrawCost = 32;
    [[nodiscard]] int LcdScreenOf(PAddr addr, u32 bytes, PAddr* buffer = nullptr) const;
    /// A draw (a batch's first vertex, the first CPU-path triangle): its screen (its colour
    /// buffer's segment's) counted, and whether the late skip drops it. Emulation side, before
    /// the draw is recorded.
    [[nodiscard]] bool LateDraw();
    /// A display transfer or texture copy: the segment it ends gets its screen, and whether the late
    /// skip drops it. Emulation side.
    [[nodiscard]] bool LateTransfer(const Pica::DisplayTransferConfig& config, bool display);

    /// The emulation side waits for waited_ticket: the loop kicks the pending 3D up to it.
    std::atomic<bool> ticket_wait{false};
    std::atomic<u32> waited_ticket{};
#ifdef VITA_DIAGNOSTICS
    void SampleGpu(std::stop_token stop);
    std::atomic<u32> gpu_samples{};
    std::atomic<u32> gpu_busy{};
    std::atomic<u32> gpu_held{};
    std::array<std::atomic<u32>, GpuAtCells> gpu_at{};
    std::array<std::atomic<u32>, GpuLabels> gpu_3d_by{};
    std::array<std::atomic<u32>, GpuLabels> gpu_ta_by{};
    Common::NamedThread gpu_sampler;
#endif

    void SleepDone(EmuWait kind);
    /// Whether this thread holds (Hold): the render side's while the hold lasts.
    [[nodiscard]] bool Holding() const;
#ifdef VITA_DIAGNOSTICS
    std::array<std::atomic<u32>, Stats::Count> records{};
    std::atomic<u64> busy_us{};
    std::atomic<u64> idle_us{};
    std::atomic<u32> timeouts{};
    u32 gpu_wakes_mark{};
    std::atomic<u32> presents{};
    std::atomic<u32> presents_behind{};
    std::atomic<u32> presents_soon{};
    std::atomic<u32> late_frames{};
    std::atomic<u32> late_waits{};
    PAddr trace_top{}; ///< tracelog: the top screen's buffer the last present showed
    std::atomic<u64> late_wait_us{};
    u32 frames_put{};
    std::atomic<u32> frames_done{};
    std::atomic<u64> lead_frames_sum{};
    std::atomic<u32> lead_samples{};
    std::atomic<u32> lead_frames_max{};
    std::atomic<u32> lead_bytes_max{};
    std::atomic<u32> late_draws{};
    std::atomic<u32> late_kept{};
    std::atomic<u32> merged_draws{};
    std::atomic<u32> merged_runs{};
    std::atomic<u32> late_partial{};
    std::array<std::atomic<u32>, 4> present_gaps{};
    std::atomic<u32> holds{};
    std::atomic<u64> hold_us{};
    struct EmuWaitCounters {
        std::atomic<u32> waits{};
        std::atomic<u64> us{};
        std::atomic<u32> sleeps{};
        std::atomic<u32> timeouts{};
    };
    std::array<EmuWaitCounters, EmuWaits> emu_wait_counters{};
    void CountEmuWait(EmuWait kind, Common::ProcessClock::time_point start);
#endif

    Common::NamedThread thread;
};

} // namespace SgxdRenderer
