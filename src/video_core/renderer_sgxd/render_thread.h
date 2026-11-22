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
#include "common/common_types.h"
#include "common/named_thread.h"
#include "common/thread.h"
#include "video_core/pica/pica_core.h"
#include "video_core/rasterizer_interface.h"

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
 * Interrupts: a GPU interrupt is a ticket (InterruptFence). The thread takes the rasterizer's
 * fence for it when it reaches the ticket's record, polls it, and publishes the tickets whose
 * work is done (InterruptPoll reads that, any thread). A GPU that completes nothing for
 * sgxd_chain_stall_us while a ticket waits gets the backend's blocking wait, which stops the
 * emulator with its post-mortem, as sgxd's own waits do.
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
    /// A ticket for the work queued so far.
    u64 InterruptFence() override;
    /// Whether the ticket's work is done. Any thread, without a lock.
    bool InterruptPoll(u64 ticket) override;
    /// Waits until it is; without the GPU's lock unless this thread is the render side.
    void InterruptWait(u64 ticket) override;
    std::vector<std::pair<PAddr, u32>> TakeCachedReads() override;
    void LoadDefaultDiskResources(const std::atomic_bool& stop_loading,
                                  const VideoCore::DiskResourceLoadCallback& callback) override;
    void SwitchDiskResources(u64 title_id) override;

    void SetRendererAccurateMul(bool accurate_mul);
    /// Runs fn on the render side after the work queued before it: Run without waiting, RunSync
    /// waiting (at once on the render side itself).
    void Run(std::function<void()> fn);
    void RunSync(const std::function<void()>& fn);
    /// A present of the guest's screens, as the VBlank found them. One with a newer one queued
    /// behind it is dropped, and one within 12 ms of the last: at 100% the guest's VBlanks
    /// jitter around the display's frame.
    void Present(const std::array<Pica::FramebufferConfig, 2>& fb,
                 const std::array<Pica::ColorFill, 2>& fill);

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

    /// Since the last call (`perflog`): the records the thread ran by kind, its time running
    /// them and waiting for work (us), the waits a fence poll ended, the presents, and the
    /// holds with their waits for the thread to drain and park (us). Timed by batch, not by
    /// record: a clock read is a syscall.
    struct Stats {
        enum Kind : u32 { State, Draw, Fill, Transfer, Dma, Present, Other, Count };
        std::array<u32, Count> records;
        u64 busy_us;
        u64 idle_us;
        u32 poll_wakes;
        u32 presents;
        u32 presents_dropped;
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

    /// The thread waits on `work` for records, a hold or the end; everyone else on `done`.
    Common::CoreCondition work;
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
    /// How long the thread sleeps between fence polls, with tickets pending and no records:
    /// the longest an interrupt waits past its work.
    static constexpr std::chrono::microseconds poll_interval{100};

    std::chrono::steady_clock::time_point last_present{};
    /// The window's input at least this often while the thread runs, so the PS button answers.
    static constexpr std::chrono::milliseconds WindowPollInterval{16};
    std::chrono::steady_clock::time_point last_window_poll{};
    /// The newest present record's position: one with a newer one queued behind it is dropped.
    std::atomic<u32> newest_present{};

    /// The emulation side waits for waited_ticket: the loop kicks the pending 3D up to it.
    std::atomic<bool> ticket_wait{false};
    std::atomic<u32> waited_ticket{};

    void SleepDone(EmuWait kind);
    /// Whether this thread holds (Hold): the render side's while the hold lasts.
    [[nodiscard]] bool Holding() const;
#ifdef VITA_DIAGNOSTICS
    std::array<std::atomic<u32>, Stats::Count> records{};
    std::atomic<u64> busy_us{};
    std::atomic<u64> idle_us{};
    std::atomic<u32> poll_wakes{};
    std::atomic<u32> presents{};
    std::atomic<u32> presents_dropped{};
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
