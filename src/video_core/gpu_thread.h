// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include "common/common_types.h"
#include "common/wake_flag.h"

namespace VideoCore {

/**
 * The render thread. It owns the PICA's internal
 * registers, the rasterizer and the GPU the renderer drives, and runs the GPU's operations in
 * the order the emulation thread queued them: one op per GSP command or register trigger, never
 * one per draw.
 *
 * Presents have no guest-visible effect, so a present with a newer present queued behind it is
 * dropped; nothing else ever is.
 */
class GpuThread {
public:
    enum class Kind {
        Work,
        Present,
    };

    /// Starts the thread, which runs on_start before its first op.
    explicit GpuThread(std::function<void()> on_start);

    /// From the next GpuThread on: no thread; every op runs in place on the thread that
    /// queues it (the emulation thread), and a present at most once per display frame
    /// (diagnostics: gxm_flags `emurender`).
    static void RunInline(bool on);
    /// Runs every queued op, then ends the thread.
    ~GpuThread();

    GpuThread(const GpuThread&) = delete;
    GpuThread& operator=(const GpuThread&) = delete;

    void Push(std::function<void()> op, Kind kind = Kind::Work);
    /// Runs fn on the render thread after every op queued before it, and waits for it. Runs fn
    /// in place when called from the render thread itself.
    void RunSync(const std::function<void()>& fn);
    [[nodiscard]] bool OnThread() const;

    /// Time spent in ops since the last call, by kind (diagnostics: `perflog`).
    struct Stats {
        u64 work_us;
        u32 work_ops;
        u64 present_us;
        u32 presents;
        u32 presents_dropped;
        /// Empty-queue waits a spin ended with an op, and those that went on to sleep.
        u32 spin_hits;
        u32 spin_misses;
    };
    Stats TakeStats();

private:
    struct Op {
        std::function<void()> fn;
        Kind kind;
    };

    void Loop(std::function<void()> on_start);
    /// Inline mode: on_start once, then op, timed as the thread's ops are.
    void RunHere(Op op);

    bool inline_ops{};
    std::function<void()> inline_start;
    std::thread::id inline_owner;
    std::chrono::steady_clock::time_point inline_last_present{};

    std::mutex mutex;
    /// Not a condition variable: the Vita's is pthread-embedded's, a 500 us poll on a
    /// semaphore that has overflowed and lost a wakeup (common/wake_flag.h).
    Common::WakeFlag wake;
    /// The queue's length, for the spin to read without the lock.
    std::atomic<u32> queued{};
    /// The render thread is (about to be) waiting on wake. Under mutex.
    bool sleeping{};
    std::deque<Op> ops;
    bool stopping{};
    std::atomic<u64> busy_us[2]{};
    std::atomic<u32> op_count[2]{};
    std::atomic<u32> presents_dropped{};
    std::atomic<u32> spin_hits{};
    std::atomic<u32> spin_misses{};
    std::thread thread;
};

} // namespace VideoCore
