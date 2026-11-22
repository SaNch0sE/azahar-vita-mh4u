// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <shared_mutex>
#include "common/common_types.h"
#ifdef __vita__
#include <stop_token>
#endif

namespace Common {

#ifdef __vita__

/**
 * Mutexes and a condition variable on the Vita kernel's lightweight ones (SceKernelLwMutex,
 * SceKernelLwCond) in place of std::mutex and friends, which newlib's pthread-embedded builds:
 * each pthread_mutex_lock and unlock there looks its owner up with pthread_self(), a walk of the
 * thread's TLS key list (pte_osTlsGetValue), a large share of the emulation core with every svc
 * an upcall (the HLE lock in CallSVCForUpcall and UpcallSvc). A lightweight mutex locks
 * and unlocks uncontended in user space, the owner from TPIDRURO (the upcall body has the emulation
 * thread's in it). Elsewhere these are the std types. Each meets Lockable, for std::scoped_lock,
 * std::unique_lock and std::lock_guard.
 */
class Mutex {
public:
    Mutex();
    ~Mutex();
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    void lock();
    void unlock();
    bool try_lock();

private:
    alignas(8) std::array<u8, 0x20> work{}; ///< SceKernelLwMutexWork
};

/// Mutex, but its owner may lock it again (a count; as std::recursive_mutex).
class RecursiveMutex {
public:
    RecursiveMutex();
    ~RecursiveMutex();
    RecursiveMutex(const RecursiveMutex&) = delete;
    RecursiveMutex& operator=(const RecursiveMutex&) = delete;

    void lock();
    void unlock();
    bool try_lock();

private:
    alignas(8) std::array<u8, 0x20> work{}; ///< SceKernelLwMutexWork
};

/// std::shared_mutex's interface on one Mutex: readers take it exclusively too. Its users hold it
/// briefly and seldom at once (cheats, the IPC recorder's callbacks, Artic caches).
class SharedMutex {
public:
    void lock() {
        mutex.lock();
    }
    void unlock() {
        mutex.unlock();
    }
    bool try_lock() {
        return mutex.try_lock();
    }
    void lock_shared() {
        mutex.lock();
    }
    void unlock_shared() {
        mutex.unlock();
    }
    bool try_lock_shared() {
        return mutex.try_lock();
    }

private:
    Mutex mutex;
};

/**
 * std::condition_variable_any on a lightweight condition variable: waits with any lock (a
 * std::unique_lock of Mutex as a rule), with stop tokens too. It has a lightweight mutex of its
 * own: a wait releases the caller's lock while holding it, and a notify takes it, so no notify
 * falls between the caller's check and its wait, and a stop callback that notifies never takes
 * the caller's lock. A notify with no waiter is no syscall (a count under that mutex): signalling
 * the kernel's condition always is one, and inside an upcall each one is an exit.
 */
class ConditionVariable {
public:
    ConditionVariable();
    ~ConditionVariable();
    ConditionVariable(const ConditionVariable&) = delete;
    ConditionVariable& operator=(const ConditionVariable&) = delete;

    void notify_one() noexcept;
    void notify_all() noexcept;

    template <typename Lock>
    void wait(Lock& lock) {
        Wait(lock, nullptr, nullptr);
    }

    template <typename Lock, typename Pred>
    void wait(Lock& lock, Pred pred) {
        while (!pred()) {
            Wait(lock, nullptr, nullptr);
        }
    }

    template <typename Lock, typename Pred>
    bool wait(Lock& lock, std::stop_token token, Pred pred) {
        std::stop_callback callback(token, [this] { notify_all(); });
        while (!pred()) {
            if (!Wait(lock, &token, nullptr)) {
                return pred();
            }
        }
        return true;
    }

    template <typename Lock, typename Clock, typename Duration>
    std::cv_status wait_until(Lock& lock, const std::chrono::time_point<Clock, Duration>& when) {
        const auto now = Clock::now();
        if (now >= when) {
            return std::cv_status::timeout;
        }
        u32 us = TimeoutMicroseconds(std::chrono::ceil<std::chrono::microseconds>(when - now));
        Wait(lock, nullptr, &us);
        return Clock::now() >= when ? std::cv_status::timeout : std::cv_status::no_timeout;
    }

    template <typename Lock, typename Clock, typename Duration, typename Pred>
    bool wait_until(Lock& lock, const std::chrono::time_point<Clock, Duration>& when, Pred pred) {
        while (!pred()) {
            if (wait_until(lock, when) == std::cv_status::timeout) {
                return pred();
            }
        }
        return true;
    }

    template <typename Lock, typename Clock, typename Duration, typename Pred>
    bool wait_until(Lock& lock, std::stop_token token,
                    const std::chrono::time_point<Clock, Duration>& when, Pred pred) {
        std::stop_callback callback(token, [this] { notify_all(); });
        while (!pred()) {
            const auto now = Clock::now();
            if (now >= when) {
                return pred();
            }
            u32 us = TimeoutMicroseconds(std::chrono::ceil<std::chrono::microseconds>(when - now));
            if (!Wait(lock, &token, &us)) {
                return pred();
            }
        }
        return true;
    }

    template <typename Lock, typename Rep, typename Period>
    std::cv_status wait_for(Lock& lock, const std::chrono::duration<Rep, Period>& rel) {
        return wait_until(lock, std::chrono::steady_clock::now() + rel);
    }

    template <typename Lock, typename Rep, typename Period, typename Pred>
    bool wait_for(Lock& lock, const std::chrono::duration<Rep, Period>& rel, Pred pred) {
        return wait_until(lock, std::chrono::steady_clock::now() + rel, std::move(pred));
    }

    template <typename Lock, typename Rep, typename Period, typename Pred>
    bool wait_for(Lock& lock, std::stop_token token, const std::chrono::duration<Rep, Period>& rel,
                  Pred pred) {
        return wait_until(lock, std::move(token), std::chrono::steady_clock::now() + rel,
                          std::move(pred));
    }

private:
    static u32 TimeoutMicroseconds(std::chrono::microseconds us) {
        return static_cast<u32>(std::clamp<s64>(us.count(), 1, 0xFFFFFFFF));
    }

    /// One wait: false at once, without waiting, if `token` is stopped (checked under the own
    /// mutex, after which its stop callback's notify can only come once this waits).
    template <typename Lock>
    bool Wait(Lock& lock, const std::stop_token* token, u32* timeout_us) {
        Enter();
        if (token != nullptr && token->stop_requested()) {
            Leave();
            return false;
        }
        lock.unlock();
        Block(timeout_us);
        lock.lock();
        return true;
    }

    void Enter();
    void Leave();
    /// With the own mutex held: waits as one more waiter (until a notify, or `timeout_us` if
    /// given), then releases it.
    void Block(u32* timeout_us);

    alignas(8) std::array<u8, 0x20> lw_mutex{}; ///< SceKernelLwMutexWork
    alignas(8) std::array<u8, 0x10> lw_cond{};  ///< SceKernelLwCondWork
    u32 waiters{};                              ///< under lw_mutex
};

#else

using Mutex = std::mutex;
using RecursiveMutex = std::recursive_mutex;
using SharedMutex = std::shared_mutex;
using ConditionVariable = std::condition_variable_any;

#endif

} // namespace Common
