// Copyright 2013 Dolphin Emulator Project / 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>
#include "common/common_types.h"
#include "common/polyfill_thread.h"

namespace Common {

class Event {
public:
    void Set() {
        std::scoped_lock lk{mutex};
        if (!is_set) {
            is_set = true;
            condvar.notify_one();
        }
    }

    void Wait() {
        std::unique_lock lk{mutex};
        condvar.wait(lk, [&] { return is_set.load(); });
        is_set = false;
    }

    template <class Duration>
    bool WaitFor(const std::chrono::duration<Duration>& time) {
        std::unique_lock lk{mutex};
        if (!condvar.wait_for(lk, time, [this] { return is_set.load(); }))
            return false;
        is_set = false;
        return true;
    }

    template <class Clock, class Duration>
    bool WaitUntil(const std::chrono::time_point<Clock, Duration>& time) {
        std::unique_lock lk{mutex};
        if (!condvar.wait_until(lk, time, [this] { return is_set.load(); }))
            return false;
        is_set = false;
        return true;
    }

    void Reset() {
        std::unique_lock lk{mutex};
        // no other action required, since wait loops on the predicate and any lingering signal will
        // get cleared on the first iteration
        is_set = false;
    }

    [[nodiscard]] bool IsSet() {
        return is_set;
    }

private:
    std::condition_variable condvar;
    std::mutex mutex;
    std::atomic_bool is_set{false};
};

/**
 * A mutex and a condition variable on it, for waits two threads make and answer at a high rate
 * (the HLE DSP's frames). On the Vita they are the kernel's lightweight mutex and condition
 * variable (SceKernelLwMutex, SceKernelLwCond): newlib's pthread-embedded builds
 * std::condition_variable from semaphores, and at thousands of waits a second it leaked counts
 * until every sceKernelSignalSema failed with 0x80028103. Elsewhere
 * std::mutex and std::condition_variable. Meets BasicLockable, for std::scoped_lock.
 */
class CoreCondition {
public:
    CoreCondition();
    ~CoreCondition();
    CoreCondition(const CoreCondition&) = delete;
    CoreCondition& operator=(const CoreCondition&) = delete;

    void lock();
    void unlock();
    /// With the lock held: releases it, waits for a NotifyAll or the timeout, holds it again.
    /// False on timeout.
    bool WaitFor(std::chrono::microseconds timeout);
    /// With the lock held: wakes every waiter.
    void NotifyAll();

private:
#ifdef __vita__
    alignas(8) std::array<u8, 0x20> lw_mutex{}; ///< SceKernelLwMutexWork
    alignas(8) std::array<u8, 0x10> lw_cond{};  ///< SceKernelLwCondWork
#else
    std::mutex mutex;
    std::condition_variable condvar;
#endif
};

class Barrier {
public:
    explicit Barrier(std::size_t count_) : count(count_) {}

    /// Blocks until all "count" threads have called Sync()
    bool Sync(std::stop_token token = {}) {
        std::unique_lock lk{mutex};
        const std::size_t current_generation = generation;

        if (++waiting == count) {
            generation++;
            waiting = 0;
            condvar.notify_all();
            return true;
        } else {
            CondvarWait(condvar, lk, token,
                        [this, current_generation] { return current_generation != generation; });
            return !token.stop_requested();
        }
    }

    std::size_t Generation() {
        std::unique_lock lk{mutex};
        return generation;
    }

private:
    std::condition_variable_any condvar;
    std::mutex mutex;
    std::size_t count;
    std::size_t waiting = 0;
    std::size_t generation = 0; // Incremented once each time the barrier is used
};

enum class ThreadPriority : u32 {
    Low = 0,
    Normal = 1,
    High = 2,
    VeryHigh = 3,
    Critical = 4,
};

void SetCurrentThreadPriority(ThreadPriority new_priority);

void SetCurrentThreadName(const char* name);

/**
 * Which part of the emulator a thread belongs to.
 *
 * On a fixed four-core handheld there is no scheduler decision worth making: the emulation and
 * render threads each want a core to themselves for a whole frame, and the two rasterizer
 * workers want the rest. The roles name that partition so the policy lives in one place instead
 * of being spread across the thread creation sites.
 */
enum class ThreadRole {
    Other,
    Emulation,
    Render,
    RasterWorker,
    ShaderCompiler,
    AudioSink,
    /// The HLE DSP's frames (AudioCore::DspHle), beside the emulation thread as the 3DS's DSP
    /// runs beside its ARM11.
    Dsp,
};

/**
 * Pins the calling thread to the core its role owns, where the platform has a fixed partition
 * (the Vita port, and Linux hosts with at least four cores, so the same partition can be tested
 * off-console). `index` distinguishes threads sharing a role, currently the rasterizer workers.
 * A no-op everywhere else - a desktop scheduler does better than a hardcoded map.
 */
void SetCurrentThreadRole(ThreadRole role, std::size_t index = 0);

} // namespace Common
