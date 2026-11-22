// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <chrono>
#include <utility>
#include "common/thread.h"
#include "video_core/gpu_thread.h"

namespace VideoCore {

namespace {
// How long the render thread spins on an empty queue before it sleeps: the GPU's ops come in
// bursts (a fill, the guest's answer to PSC0, a command list, a transfer), and a burst's next op
// then costs no wakeup. Unmeasured: perflog counts the ops a spin caught and the sleeps.
constexpr u32 SpinIterations = 4096;

void SpinPause() {
#if defined(__arm__) || defined(__aarch64__)
    __builtin_arm_yield();
#endif
}

// GpuThread::RunInline's.
std::atomic<bool> inline_requested{false};
} // namespace

void GpuThread::RunInline(bool on) {
    inline_requested.store(on, std::memory_order_relaxed);
}

GpuThread::GpuThread(std::function<void()> on_start)
    : inline_ops{inline_requested.load(std::memory_order_relaxed)} {
    if (inline_ops) {
        inline_start = std::move(on_start);
        inline_owner = std::this_thread::get_id();
        return;
    }
    thread = std::thread{
        [this, on_start = std::move(on_start)]() mutable { Loop(std::move(on_start)); }};
}

void GpuThread::RunHere(Op op) {
    if (inline_start) {
        std::exchange(inline_start, nullptr)();
    }
    if (op.kind == Kind::Present) {
        // A present has no guest-visible effect: well past the display's rate it is dropped,
        // as a queued one is behind a newer one. 12 ms, not a frame's 16.7: at 100% the guest's
        // VBlanks jitter around the frame, and a frame's cap dropped a quarter of them.
        const auto now = std::chrono::steady_clock::now();
        if (now - inline_last_present < std::chrono::microseconds(12000)) {
            presents_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        inline_last_present = now;
    }
#ifdef VITA_DIAGNOSTICS
    const auto start = std::chrono::steady_clock::now();
    op.fn();
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    const auto k = static_cast<std::size_t>(op.kind);
    busy_us[k].fetch_add(static_cast<u64>(us), std::memory_order_relaxed);
    op_count[k].fetch_add(1, std::memory_order_relaxed);
#else
    op.fn();
#endif
}

GpuThread::~GpuThread() {
    if (inline_ops) {
        return;
    }
    {
        std::scoped_lock lock{mutex};
        stopping = true;
    }
    wake.Raise();
    thread.join();
}

void GpuThread::Push(std::function<void()> op, Kind kind) {
    if (inline_ops) {
        RunHere(Op{std::move(op), kind});
        return;
    }
    bool raise;
    {
        std::scoped_lock lock{mutex};
        if (kind == Kind::Present) {
            // A newer present drops the queued one and goes behind the work queued since: it
            // shows the screens of a later VBlank.
            const auto queued = std::find_if(ops.begin(), ops.end(), [](const Op& queued_op) {
                return queued_op.kind == Kind::Present;
            });
            if (queued != ops.end()) {
                ops.erase(queued);
                presents_dropped.fetch_add(1, std::memory_order_relaxed);
            }
        }
        ops.push_back(Op{std::move(op), kind});
        queued.store(static_cast<u32>(ops.size()), std::memory_order_release);
        // Only a sleeping render thread needs the flag: a busy or spinning one finds the op.
        raise = std::exchange(sleeping, false);
    }
    if (raise) {
        wake.Raise();
    }
}

void GpuThread::RunSync(const std::function<void()>& fn) {
    if (inline_ops) {
        RunHere(Op{fn, Kind::Work});
        return;
    }
    if (OnThread()) {
        fn();
        return;
    }
    // Raised once, after done: the flag starts down, so the wait returns only then.
    Common::WakeFlag done_flag;
    std::atomic<bool> done{false};
    Push([&] {
        fn();
        done.store(true, std::memory_order_release);
        done_flag.Raise();
    });
    while (!done.load(std::memory_order_acquire)) {
        done_flag.Wait();
    }
}

bool GpuThread::OnThread() const {
    if (inline_ops) {
        return std::this_thread::get_id() == inline_owner;
    }
    return std::this_thread::get_id() == thread.get_id();
}

void GpuThread::Loop(std::function<void()> on_start) {
    Common::SetCurrentThreadName("GpuThread");
    Common::SetCurrentThreadRole(Common::ThreadRole::Render);
    if (on_start) {
        on_start();
    }
    for (;;) {
        Op op;
        {
            std::unique_lock lock{mutex};
            if (ops.empty() && !stopping) {
                lock.unlock();
                u32 spins = 0;
                while (spins < SpinIterations && queued.load(std::memory_order_acquire) == 0) {
                    SpinPause();
                    spins++;
                }
                lock.lock();
#ifdef VITA_DIAGNOSTICS
                (ops.empty() ? spin_misses : spin_hits).fetch_add(1, std::memory_order_relaxed);
#endif
            }
            while (ops.empty() && !stopping) {
                // Marked and lowered under the lock, having seen nothing to do: a Push
                // publishes under the same lock and raises after, so it cannot be missed.
                sleeping = true;
                wake.Lower();
                lock.unlock();
                wake.Wait();
                lock.lock();
            }
            if (ops.empty()) {
                return;
            }
            op = std::move(ops.front());
            ops.pop_front();
            queued.store(static_cast<u32>(ops.size()), std::memory_order_relaxed);
        }
#ifdef VITA_DIAGNOSTICS
        // The time is `perflog`'s, in the diagnostics build only.
        const auto start = std::chrono::steady_clock::now();
        op.fn();
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();
        const auto k = static_cast<std::size_t>(op.kind);
        busy_us[k].fetch_add(static_cast<u64>(us), std::memory_order_relaxed);
        op_count[k].fetch_add(1, std::memory_order_relaxed);
#else
        op.fn();
#endif
    }
}

GpuThread::Stats GpuThread::TakeStats() {
    return Stats{busy_us[0].exchange(0),  op_count[0].exchange(0),      busy_us[1].exchange(0),
                 op_count[1].exchange(0), presents_dropped.exchange(0), spin_hits.exchange(0),
                 spin_misses.exchange(0)};
}

} // namespace VideoCore
