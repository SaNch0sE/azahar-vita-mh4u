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
#include <span>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/common_types.h"
#include "common/named_thread.h"
#include "common/process_clock.h"
#include "common/thread.h"
#include "video_core/pica/pica_core.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_sgxd/page_holds.h"

namespace VideoCore {
class RightEyeDisabler;
}

namespace Memory {
class MemorySystem;
}

namespace SgxdRenderer {

class RasterizerSgxd;
class RendererSgxd;

/**
 * The render thread (Vita core 0) owns everything behind the rasterizer interface: the sgxd
 * rasterizer, sgxd, the guest's GPU memory map, the window and the presents (docs/threads.md).
 *
 * Each list is split between PicaCores (Pica::PicaCore::BindSplit). The emulation side scans it at
 * submission (late skip, page holds, index ranges, interrupt requests) and queues it as a record;
 * the parse thread parses it ahead of this thread into a second ring, which this thread runs at the
 * list's record (RunParsed). Until the parse has copied the list, its pages are held against the
 * guest's writes (PageHolds::HoldParse).
 *
 * Tickets: each GPU interrupt and vblank takes one (InterruptFence) for the work queued before it;
 * the interrupt goes to the guest at once. The pages that work touches stay held until the ticket
 * is reached. While tickets wait the thread sleeps until sgxd wakes it for the oldest one's fence.
 *
 * Every producer holds the GPU's lock (VideoCore::GPU::Lock): the ring has one writer at a time.
 */
class RenderThread final : public VideoCore::RasterizerInterface {
public:
    RenderThread(Pica::PicaCore& pica, Pica::PicaCore& parse_pica, Pica::PicaCore& render_pica,
                 RendererSgxd& renderer,
                 RasterizerSgxd& rasterizer, Memory::MemorySystem& memory,
                 const VideoCore::RightEyeDisabler& right_eye);
    ~RenderThread() override;

    /// The live render thread's state for the stall watch's report, into `out` without allocating (a
    /// frozen thread may hold the heap's lock). Read without locks: a snapshot.
    static void DescribeLive(char* out, std::size_t size);
    static s32 LiveThreadId();
    /// The late skip's cap: how many frames in a row it may drop one screen's draws while the GPU lags
    /// (0 none, NoScreenSkipLimit any number). Read at each frame's end; any thread.
    static void SetMaxScreenSkips(u32 frames);
    static void SetReadbackHolds(bool on);
    [[nodiscard]] static u32 MaxScreenSkips();
    static constexpr u32 NoScreenSkipLimit = 0xFFFFFFFF;

    RenderThread(const RenderThread&) = delete;
    RenderThread& operator=(const RenderThread&) = delete;

    struct GuestRange {
        PAddr addr;
        u32 bytes;
    };
    using DrawTextures = std::array<GuestRange, 8>;

    /// A command list the guest submitted, its interrupts' delays into `irqs`. may_leave_out: a list
    /// whose first draw is a top-screen picture is the right eye's, left out with its P3D alone.
    /// Returns whether it was.
    bool SubmitCmdList(PAddr addr, u32 size, bool may_leave_out, std::vector<u64>& irqs);
    struct ScanFirstDraw {
        bool drew;
        u32 vp_height;
        PAddr paddr;
    };
    const ScanFirstDraw& FirstDraw() const {
        return scan_first;
    }
    /// A PICA register the guest wrote outside a list, in order with the lists. Only the parse thread
    /// reads it, so the next list's publish takes it along.
    void RegWrite(u32 id, u32 value);
    void TakeWholeState();
    void GiveWholeState();
    /// A resource file the game's watchers named: the textures, indices and vertices a draw reads inside
    /// it are loaded data the game does not write again, and hold no page.
    void WatchResource(PAddr addr, u32 size);
    /// A buffer of the game's per-frame command lists and vertices: the GPU's reads inside it hold no
    /// page, and the guest waits at its turn for the work that read it last.
    void WatchFrameBuffer(PAddr addr, u32 size);
    [[nodiscard]] std::vector<u32> ResourceRanges() const;
    void SetResourceRanges(std::span<const u32> words);

    // What the two sides of a split list call (Pica::PicaCore::BindSplit): the scan on the emulation
    // side, the parse on this thread.
    /// Scan: a list, or a buffer it jumps to, as it is submitted: its words.
    const u8* ScanList(PAddr addr, u32 size);
    void ScanDraw(bool is_indexed);
    void ScanImmediateBatch();
    void ScanImmediateFlush();
    /// Scan: an immediate-mode vertex the geometry unit takes (drawn on the CPU).
    void ScanImmediateVertex();
    /// Scan: an interrupt request, raised once the list is handed over.
    void ScanIrq(u64 delay_ns);
    const u8* RenderList(PAddr addr, u32 size);
    /// Render: whether to draw the next of the scan's draws, in the scan's order.
    bool RenderDraw();
    void RenderFogLut();

    // The rasterizer interface, for the emulation side. Each call queues; the Accelerate ones answer
    // true (RasterizerSgxd stops the emulator on what it cannot do). The draws are the lists'.
    void AddTriangle(const Pica::OutputVertex& v0, const Pica::OutputVertex& v1,
                     const Pica::OutputVertex& v2) override;
    void DrawTriangles() override;
    // The surfaces reach guest memory through the operations that write them there, the pictures
    // through the reads of their bytes (the page holds copy a picture's rows first).
    void FlushAll() override {}
    /// A savestate's save, on this thread: the pictures guest memory is older than go there first.
    void ClearAll(bool flush) override;
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
    /// The guest's view gets the holds the work queued since made, at the end of each GPU operation and
    /// vblank.
    void ApplyPageHolds();
    /// The tickets reached so far to the holds (their pages let go of at the guest's next fault on
    /// one). Between slices.
    void ReleasePageHolds();
    /// A guest access aborted on the page of `host`: whether a hold refused it. If so, the work it
    /// waited for is done and the page open, and the guest runs the access again.
    bool PageFault(const u8* host, bool write, u32 guest_pc, u32 guest_va);
    /// A GPU-side read of guest memory (a list's parse, an index scan): waits for the queued GPU writes
    /// over those bytes, whatever the guest's view.
    void GpuRead(const u8* host, u32 size);
    /// The holds' changes sent, then whether the record lets this access through: an abort that came
    /// before its page's change reached the descriptor (host 1's) may run again.
    bool AccessOpenAfterFlush(const u8* host, bool write);
    bool OpensDisplayPage(const u8* host, bool write, u32 reach);
    /// Whether an aborted guest access to [host, host + size), on one page, can be made at once through
    /// the app's view: the page open to it, or the access clear of the bytes its holds protect.
    bool AccessMisses(const u8* host, u32 size, bool write);
    /// A guest access on a held page the guest keeps accessing past its holds opens it without a wait
    /// if it can: a store by a version of a moved FCRAM page, or the page its own effects open. A page
    /// a picture's rows close has them written back and marked outdated; the access itself goes past
    /// the holds (false).
    bool OpensHeldPage(const u8* host, u32 size, bool write);
    bool ReadMissesWrites(const u8* host, u32 size);
    /// A guest store of [host, host + size) on a held page, made by the caller through the app's view
    /// when true: past the holds, or onto the page opened.
    bool StoreMissesHolds(const u8* host, u32 size);
    /// The HLE reads or writes [host, host + size) through its own view: it waits for the queued GPU
    /// work the guest would wait for.
    void HostAccess(const u8* host, u32 size, bool write);
    /// The HLE writes these pages later through its own pointers (Y2R's worker); `done` waits until it
    /// has. A version of one of them runs it first. One writer at a time.
    void HostWritesLater(const std::vector<const u8*>& pages, std::function<void()> done);
    /// A guest window was mapped over [host, host + size): its descriptors take the holds.
    void HostRemapped(const u8* host, u32 size);
#ifdef VITA_DIAGNOSTICS
    std::pair<u32, u32> TakeTextureWrites();
    /// No page held less than the module still holds it (`where`).
    void CheckHoldsSent(const char* where);
    std::string HoldState(const u8* host);
    std::string VersionList() const;
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
        /// Writes to a handed-over list's page: copies taken for the parse, and waits for a parse.
        u32 parse_copies, parse_waits;
        u64 parse_wait_us;
        /// Writes onto pages this thread was still to read on the CPU: the copies they took.
        u32 copy_faults;
        u32 copy_bytes;
        u64 fault_wait_us;
        u32 host_waits;
        u64 host_wait_us;
        u32 host_past, host_versions;
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
    /// Before a move of [addr, addr + bytes)'s FCRAM pages to CDRAM that no hold announces: their
    /// versions of the cached pool given up (PageHolds::LeaveVersions). From the emulation thread,
    /// outside an operation's holds.
    void LeaveVersions(PAddr addr, u32 bytes);
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

    [[nodiscard]] u32 ShownFrames() const {
        return shown_frames.load(std::memory_order_relaxed);
    }

    RasterizerSgxd& Rasterizer() {
        return rasterizer;
    }

    enum EmuWait : u32 { EmuTicket, EmuSpace, EmuSync, EmuWaits };

#ifdef VITA_DIAGNOSTICS
    /// perflog's GPU sampler cells: each microkernel stream's state (no job, in a job's code, or at a
    /// wait of sgxd_mkq's kinds), cell 3D * GpuAtStates + TA.
    static constexpr u32 GpuAtStates = 9;
    static constexpr u32 GpuAtCells = GpuAtStates * GpuAtStates;
    static constexpr u32 GpuLabels = 64;

    struct Stats {
        /// State: the lists' decisions and the register writes; Draw: the lists.
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
        u32 right_eye_draws;     ///< draws of the right eye left out
        u32 right_eye_transfers; ///< transfers into a right-eye LCD buffer left out
        u32 late_partial;
        u32 gpu_samples;
        u32 gpu_busy;
        u32 gpu_held;
        u64 parse_busy_us;
        u32 parsed_waits;
        u64 parsed_wait_us;
        u32 lists;
        u64 list_bytes;
        u64 list_parsed_bytes;
        u32 list_ranges;
        u32 list_parsed_max;
        u32 list_draws;
        u64 list_kept_bytes;
        u64 list_vram_bytes;
        u64 scan_us;
        u32 index_scans;
        u64 index_scan_indices;
        u64 index_scan_us;
        u32 index_memo_hits;
        u32 resource_named;
        u32 resource_gone;
        u32 resources;
        u64 resource_bytes;
        std::array<u32, 3> resource_skips;
        u32 frame_buffer_reads;
        u32 frame_buffer_waits;
        u64 frame_buffer_wait_us;
        u32 publishes;
        u32 render_wakes;
        u32 parse_wakes;
        u64 wake_us;
        std::array<u32, GpuAtCells> gpu_at;
        std::array<u32, GpuLabels> gpu_3d_by;
        std::array<u32, GpuLabels> gpu_ta_by;
        std::array<u32, GpuLabels> jobs_by;
        std::array<u32, 4> present_gaps;
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

    /// The parse thread (core 3): lists parsed ahead of the render side. It reads the ring behind the
    /// emulation side's writes, applies each list's decisions and register writes to parse_pica, and
    /// writes the state changes and draws as records of its own (`parsed`), which the render side runs
    /// at the list's Op::CmdList.
    void ParseLoop();
    void ParseRecord(const Head& head, const u8* payload);
    bool ParseSleep(std::chrono::microseconds timeout);
    void ParseWake();
    /// Waits until the parse thread has read every record published (a whole-state copy).
    void WaitParseIdle();
    u8* ParsedBegin(Op op, u32 bytes, u16 extra = 0);
    void ParsedCommit();
    void PublishParsed();
    template <typename T>
    void ParsedPut(Op op, const T& payload, u16 extra = 0);
    void ParsedPut(Op op);
    void SyncParsedState();
    void PrefetchFragment();
    bool parse_fragment_dirty = true;
    void PrefetchVertex();
    /// Render side: list `seq`'s parsed records run as they come, to its end.
    void RunParsed(u32 seq);
    class ParsedOutput;
    std::unique_ptr<ParsedOutput> parsed_output;
    /// The parse thread's place in the ring: the producer's room ends here or at `read`.
    std::atomic<u32> parse_read{};
    s32 parse_wake_flag{-1};
    std::atomic<bool> parse_sleeping{};
    /// A list's record committed since the parse thread was woken: only a list needs it awake (each
    /// wake a syscall and an IPI).
    bool parse_wake_due{};
    std::atomic<bool> parse_stopping{};
    /// The parsed ring; positions wrap at 2^32.
    static constexpr u32 ParsedSize = 1024 * 1024;
    std::unique_ptr<u8[]> parsed;
    u32 parsed_write{};
    u32 parsed_open{};
    std::atomic<u32> parsed_published{};
    std::atomic<u32> parsed_read{};
    std::atomic<bool> parsed_space_waiting{};
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
    Pica::PicaCore& parse_pica;
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
    std::atomic<u32> loop_passes{};
    std::atomic<u32> pending_size{};
    std::atomic<u32> pending_front_ticket{};
    std::atomic<u64> pending_front_fence{};
    static constexpr u32 NoRecord = 0xFFFFFFFF;
    std::atomic<u32> running_op{NoRecord};
    std::atomic<s32> render_tid{-1};
    /// RunAvailable is running: a Call's function that queues returns to it.
    bool running_available = false;
    u32 tickets{};                  ///< the last ticket handed out (GPU's lock)
    std::atomic<u32> reached{};     ///< the last ticket whose work is done

    PageHolds page_holds;
    /// A guest or HLE access to [addr, addr + bytes) where a picture owns rows, from any thread: the
    /// render side copies them into guest memory before this returns.
    void AskPictureCopy(PAddr addr, u32 bytes, bool write, bool wait = true, bool beside = false);
    /// A request for the render side, served between its records; waited for unless AccessNoWait.
    static constexpr u32 AccessWrite = 1;
    static constexpr u32 AccessNoWait = 2;
    static constexpr u32 AccessBeside = 8;
    void Ask(PAddr addr, u32 bytes, u32 flags);
    void ServeGuestAccesses();
    /// After a copy for a read of [lo, hi): the rows it may have written back watched for the first
    /// write.
    void WatchCopiedRows(PAddr lo, PAddr hi);
    std::mutex access_lock;
    std::vector<std::array<u32, 3>> access_requests; ///< address, bytes, Access flags
    std::atomic<u32> access_requested{0}; ///< requests made (under access_lock)
    std::atomic<u32> access_served{0};    ///< requests served
#ifdef VITA_DIAGNOSTICS
    std::atomic<u32> picture_faults{0};        ///< guest aborts that copied a picture's rows
    std::atomic<u32> picture_host_accesses{0}; ///< HLE accesses that did
#endif
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
    /// On this thread, inside a RunSync call: `ticket`'s work done, the fences driven here.
    void WaitTicketHere(u32 ticket);
    void WaitGpuWritesHere(PAddr addr, u32 size);
    [[nodiscard]] u32 HoldTicket() const {
        return tickets + 1;
    }
    /// Whether the range's pages are held (not one of the game's frame buffers, held by its turns).
    bool HoldRead(PAddr addr, u32 bytes, PageHolds::Kind kind);
    void HoldWrite(PAddr addr, u32 bytes, PageHolds::Kind kind);
    void HoldMoves(PAddr addr, u32 bytes);
    void HoldDraw(const DrawTextures& textures, u32 count, bool vertices, bool is_indexed);
    void HoldVertices(bool is_indexed);
    /// A resource file's range and its first 16 bytes as named: a file the game freed goes at the next
    /// read inside it.
    struct ResourceRange {
        PAddr lo;
        PAddr hi;
        std::array<u32, 4> header;
        u32 checked;
        const u8* host;
    };
    /// By address, none overlapping another.
    std::vector<ResourceRange> resources;
    /// The file the last read found inside: a draw's reads are mostly one model's.
    std::size_t last_resource{};
    struct FrameBuffer {
        PAddr lo;
        PAddr hi;
        u32 read_ticket;
    };
    std::vector<FrameBuffer> frame_buffers;
    [[nodiscard]] FrameBuffer* InFrameBuffer(PAddr addr, u32 bytes);
    [[nodiscard]] bool InResource(PAddr addr, u32 bytes);
#ifdef VITA_DIAGNOSTICS
    std::atomic<u32> resource_named{};
    std::atomic<u32> resource_gone{};
    std::array<std::atomic<u32>, 3> resource_skips{};
    std::atomic<u32> frame_buffer_reads{};
    std::atomic<u32> frame_buffer_waits{};
    std::atomic<u64> frame_buffer_wait_us{};
#endif
    /// The scan's memo of 16-bit index lists' ranges, valid while their pages' write stamps stand.
    struct IndexMemo {
        PAddr addr;
        u32 count;
        u32 stamp;
        u16 lo, hi;
    };
    std::array<IndexMemo, 4096> index_memo{};
    /// A draw's 16-bit indices' range as HoldVertices scanned it (scanned 0: not), sent with the draw
    /// so the render thread does not scan them again; and the ranges of the draw whose pages the holds
    /// cleaned out of the CPU's cache (PageHolds::CleanGeometry), which the render thread reads as
    /// they are: bit n the attribute loader n's, bit CleanedIndices the 16-bit index list's.
    struct DrawIndexRange {
        u16 lo;
        u16 hi;
        u16 scanned;
        u16 cleaned;
    };
    static constexpr u32 CleanedIndices = 12;
    DrawIndexRange draw_indices{};
    DrawIndexRange parse_indices{};
    const u8* parse_gs{};
    u32 parse_gs_bytes{};
    bool parse_gs_u16{};
    /// Geometry draws the parse holds back: point-mode draws with no indices whose state is the run's
    /// first one's but its first vertex, sent as one MergedDraw of their vertices' numbers.
    struct HeldRun {
        u32 draws = 0;
        DrawIndexRange first_indices{}; ///< sent as its own Draw when the run is that one
        u16 cleaned{};                  ///< the loaders' ranges every draw of the run had cleaned
        std::vector<u16> vertices;
        decltype(Pica::RegsInternal::reg_array) regs{}; ///< as the run's first draw left them
    };
    static constexpr u32 MaxRunVertices = 256;
    HeldRun held;
    bool held_flushing{};
    bool RunsDraw(bool is_indexed) const;
    bool MergeDraw(bool is_indexed);
    void FlushRun();
    void DrawMerged(const u16* vertices, u32 count, u16 cleaned);

    /// The scan's decision on a list's draw, in the scan's order: whether it is dropped (late skip,
    /// right eye), its indices' range, and a geometry draw's indices as the guest had them at the
    /// scan (gs_bytes after it: the guest may have rewritten them since).
    struct alignas(8) ListDraw {
        u8 dropped;
        u8 gs_u16;
        u8 pad[2];
        DrawIndexRange indices;
        u32 gs_bytes;
    };
    std::vector<u8> scan_draws;
    u32 scan_draw_count{};
    ScanFirstDraw scan_first{};
    bool scan_may_leave_out{};
    bool scan_left_out{};
    bool FirstDrawLeavesOut();
    std::vector<GuestRange> scan_lists;
    /// Each range's words in scan_words, or NotCopied. A range in guest VRAM (uncached, slow to read a
    /// word at a time) is copied as the scan meets it and the record carries the copy to the parse; a
    /// range in FCRAM is read in place and held until the parse has copied it.
    std::vector<u32> scan_offsets;
    std::unique_ptr<u8[]> scan_words;
    u32 scan_words_size{};
    u32 scan_words_cap{};
    static constexpr u32 NotCopied = 0xFFFFFFFF;
    /// The lists' words in guest VRAM as the scan last read them, in cached memory, a bit a word kept.
    /// A kept word is VRAM's as long as nothing wrote it, and every write is seen: a kept page is
    /// closed to the guest's and HLE's writes, and a DMA or GPU operation takes a write hold over its
    /// bytes. A page whose words a write dropped keeps none for ListQuietFrames frames.
    struct ListKeep {
        static constexpr u32 ChunkBytes = 64 << 10;
        static constexpr u32 VramBytes = 0x00600000; ///< Memory::VRAM_SIZE
        std::array<std::unique_ptr<u8[]>, VramBytes / ChunkBytes> chunks;
        std::vector<u32> kept;
        [[nodiscard]] bool Has(PAddr addr, u32 bytes) const;
        void Read(u8* out, PAddr addr, u32 bytes) const;
        void Store(PAddr addr, const u8* words, u32 bytes);
        bool Drop(PAddr lo, PAddr hi);
        void Clear();
    };
    ListKeep list_keep;
    static constexpr u32 ListQuietFrames = 30;
    u32 list_frame{};
    std::vector<u32> list_quiet = std::vector<u32>(ListKeep::VramBytes >> 12);
    void DropListWrites();
    std::vector<u64>* scan_irqs{};
    void PutScanDraw(const ListDraw& d, const u8* gs);
    void FlushScanDraws();
    std::vector<u8> parse_draws;
    std::size_t parse_draw_at{};
    std::vector<u8> parse_words;
    std::vector<std::array<u32, 3>> parse_ranges;
    std::size_t parse_range_at{};
    void CopyListWords(u32 seq, const GuestRange* ranges, const u32* offsets, const u8* words,
                       u32 count);

    /// The lists handed over, the one this thread copies or last copied, and the last copied whole (its
    /// holds can go).
    u32 list_seq{};
    std::atomic<u32> parsing_seq{};
    std::atomic<u32> copied_seq{};
    struct ParseHold {
        u32 seq;
        std::vector<GuestRange> ranges;
        std::vector<u32> released;
    };
    std::deque<ParseHold> parse_holds;
    void ReleaseParsed();
    /// A write to a page a handed-over list's parse reads: each such list not copied yet reads a copy
    /// of the page, its hold let go; one being copied now ends its copy first.
    bool ParseFault(u32 page);
    /// The copies, by list and page, and how many (none, as a rule). Under snapshot_mutex, but the count.
    struct Snapshot {
        u32 seq;
        u32 page;
        std::shared_ptr<std::array<u8, 0x1000>> bytes;
    };
    std::mutex snapshot_mutex;
    std::vector<Snapshot> snapshots;
    std::atomic<u32> snapshot_count{};
    std::atomic<bool> parse_waiting{};
    /// The GPU remaps the last holds asked for, as records before the work's.
    void PutPageRemaps();
    /// Waits for the work a guest access to the page waits for, the open holds' ticket fenced first.
    void WaitHeldPage(u32 page, bool write);
    /// A write onto a held page, by the guest (PageFault) or the HLE (HostAccess): the one way past the
    /// holds. [host, host + bytes) is what it writes (bytes 0: not known). First its own effects (stale
    /// index memos, parse copies, textures untiled again), then the first that applies: the page open
    /// (Open); a known range clear of the queued work's bytes, the page still held (PastHolds); a
    /// version of a moved FCRAM page (Open); else the caller waits (Wait). `closed`: a picture's rows
    /// or watched rows on the page: it stays closed and takes no version.
    enum class HeldWrite { Open, PastHolds, Wait };
    std::vector<u32> later_pages;
    std::function<void()> later_done;
    HeldWrite TakeHeldWrite(u32 page, const u8* host, u32 bytes, bool guest, bool closed,
                            bool past = true);
    bool host_range = false;
    std::vector<std::pair<PAddr, PAddr>> host_textures;
    /// Waits until the GPU has reached `ticket` (0: nothing to wait for), the open holds'
    /// ticket fenced first.
    void WaitTicket(u32 ticket);
    /// The last ticket whose records this thread has run: what the work before it read on the CPU is
    /// read.
    std::atomic<u32> processed{};
    /// Holds [addr, addr + bytes) for a read this thread makes on the CPU when it runs the work.
    void HoldCopyRead(PAddr addr, u32 bytes, PageHolds::Kind kind);
    /// A write onto a page a live CopyRead holds: the held bytes are copied for this thread, which reads
    /// the copy, and the CopyRead lets go of the page. Whether the page was so held.
    bool CopyFault(u32 page);
    /// The copies CopyFault took: page, tickets served, bytes [lo, hi) and the bytes. Under copy_mutex,
    /// but the count; dropped once their work has run.
    struct CopiedBytes {
        u32 page, from, to;
        std::array<u16, 2> lo, hi;
        std::vector<u8> bytes;
    };
    std::mutex copy_mutex;
    std::vector<CopiedBytes> copied;
    std::atomic<u32> copied_count{};
    /// This thread's read of [addr, addr + bytes) for the records it runs now: guest memory, or an
    /// assembled copy (valid until the next call).
    const u8* IndexBytes(PAddr addr, u32 bytes);
    std::vector<u8> index_bytes;
    /// Guest FCRAM written (a watched texture page, or the HLE's write): the render side marks the
    /// textures over it written, in order with the work.
    void TexturesWritten(PAddr address, u32 size);
#ifdef VITA_DIAGNOSTICS
    u32 texture_write_faults{}, texture_write_hle{};
#endif
#ifdef VITA_DIAGNOSTICS
    u32 faults{}, fault_waits{}, host_waits{}, versioned_faults{};
    u32 host_past{}, host_versions{};
    u32 parse_copies{}, parse_waits{};
    u64 parse_wait_us{};
    u32 copy_faults{};
    u32 copy_bytes{};
    u64 fault_wait_us{}, host_wait_us{};
    std::array<u32, PageHolds::Kinds> fault_kinds{};
    std::array<u64, PageHolds::Kinds> fault_kind_us{};
    std::unordered_map<u64, FaultSite> fault_sites; ///< by the guest's PC << 8 | the kind
#endif

    /// The thread's state is under `work`. It sleeps on `wake_flag`, set for records, the end, and by
    /// sgxd when the GPU has done the oldest pending ticket's work: no timed polls. Others wait on `done`.
    Common::CoreCondition work;
    s32 wake_flag{-1};
    static constexpr u32 WakeBit = 1;
    Common::CoreCondition done;
    std::atomic<bool> sleeping{};
    std::atomic<u32> done_waiters{};
    std::atomic<bool> space_waiting{};
    bool stopping{}; ///< under `work`
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
    std::atomic<u32> shown_frames{};
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
    /// The colour buffers drawn lately (the newest 16), each with its segments: a segment is the draws
    /// into it up to a transfer from it. Per buffer: this frame's segment, a dropped draw, open (drawn
    /// since its last transfer), and each segment's screen as the transfer that ended it last found it.
    /// A segment open at a frame's end goes on into the next. Segments from the last slot on share it.
    static constexpr u32 SegmentSlots = 16;
    struct SegmentTarget {
        PAddr addr;
        u32 bytes;
        u32 segment;
        bool dropped;
        bool open;
        std::array<s8, SegmentSlots> screens;
        u8 readers;
        /// segment_draws at its last draw: of the buffers holding a transfer's source, the newest drawn.
        u32 drawn;
        /// Every transfer from it into an LCD buffer in the last frame that had one went into a right-eye
        /// buffer: every segment is the right eye's. One that also went to a screen keeps its segments'
        /// screens. to_right_eye and to_screen: this frame's so far.
        bool right_eye;
        bool to_right_eye;
        bool to_screen;
        /// A kept draw into another buffer sampled a copy or transfer of it this frame: its picture goes on
        /// into that draw's, so it is not the right eye's.
        bool feeds_reader;
        /// A buffer of both eyes: the bytes the right-eye transfers of the last frame read, when no
        /// transfer to a screen read any of them; a draw within them is the right eye's. Empty when
        /// lo >= hi; the to_ pairs are this frame's reach so far.
        u32 right_lo, right_hi;
        u32 to_right_lo, to_right_hi, to_screen_lo, to_screen_hi;
        /// This frame's reach of its transfers of a right eye no screen shows, beside a screen's bytes alone
        /// and while no kept draw samples its copies.
        u32 to_hidden_lo, to_hidden_hi;
#ifdef VITA_DIAGNOSTICS
        u32 dropped_frame; // screenlog_frame of its last dropped draw (latelog)
        u32 kept_frame;    // screenlog_frame of its last kept draw (latelog)
#endif
    };
    std::vector<SegmentTarget> segment_targets;
    struct SegmentCopy {
        PAddr lo, hi;
        PAddr source;
        u32 source_bytes;
    };
    std::vector<SegmentCopy> segment_copies;
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
    /// A segment's screen when its transfer went into a right-eye LCD buffer of the top screen: its
    /// draws and that transfer are left out from then on (the Vita shows the left eye only).
    static constexpr s8 RightEyeScreen = 2;
    const VideoCore::RightEyeDisabler& right_eye_disabler;
    /// An LCD buffer a present named: its picture's stamp (LCD writes so far, 0: none seen) and the
    /// present that last named it.
    struct LcdBuffer {
        PAddr addr;
        u32 stamp;
        u32 named;
        /// The GPU has written it. One it never has holds what the CPU or a service wrote.
        bool gpu;
    };
    u32 presents_named{};
    /// The top screen's right-eye LCD buffers a present named apart from its left ones (newest four).
    std::vector<LcdBuffer> right_eye_buffers;
    [[nodiscard]] bool RightEyeOutput(PAddr addr, u32 bytes) const;
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
    /// A draw of `vertices` (0 in immediate mode): its screen's cost counted, and whether the late
    /// skip drops it. Emulation side.
    bool LateDraw(ListDraw& d, u32 vertices);
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
#ifdef VITA_DIAGNOSTICS
    std::array<std::atomic<u32>, Stats::Count> records{};
    std::atomic<u64> busy_us{};
    std::atomic<u64> idle_us{};
    std::atomic<u32> timeouts{};
    std::atomic<u64> parse_busy_us{};
    std::atomic<u32> parsed_waits{};
    std::atomic<u64> parsed_wait_us{};
    std::atomic<u32> lists{};
    std::atomic<u64> list_bytes{};
    std::atomic<u64> list_parsed_bytes{};
    std::atomic<u32> list_ranges{};
    std::atomic<u32> list_parsed_max{};
    std::atomic<u32> list_draws{};
    std::atomic<u64> list_kept_bytes{};
    std::atomic<u64> list_vram_bytes{};
    std::atomic<u64> scan_us{};
    std::atomic<u32> index_scans{};
    std::atomic<u64> index_scan_indices{};
    std::atomic<u64> index_scan_us{};
    std::atomic<u32> index_memo_hits{};
    std::atomic<u32> publishes{};
    std::atomic<u32> render_wakes{};
    std::atomic<u32> parse_wakes{};
    std::atomic<u64> wake_us{};
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
    std::atomic<u32> right_eye_draws{};
    std::atomic<u32> right_eye_transfers{};
    std::atomic<u32> late_partial{};
    std::array<std::atomic<u32>, 4> present_gaps{};
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
    Common::NamedThread parse_thread;
};

} // namespace SgxdRenderer
