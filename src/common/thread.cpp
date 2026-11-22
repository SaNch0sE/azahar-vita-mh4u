// Copyright 2013 Dolphin Emulator Project / 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <string>

#include <algorithm>
#include "common/assert.h"
#include "common/error.h"
#include "common/logging/log.h"
#include "common/thread.h"
#ifdef __APPLE__
#include <mach/mach.h>
#elif defined(_WIN32)
#include <windows.h>
#include "common/string_util.h"
#else
#if defined(__Bitrig__) || defined(__DragonFly__) || defined(__FreeBSD__) || defined(__OpenBSD__)
#include <pthread_np.h>
#else
#include <pthread.h>
#endif
#include <sched.h>
#endif
#ifndef _WIN32
#include <unistd.h>
#endif

#ifdef __FreeBSD__
#define cpu_set_t cpuset_t
#endif

#ifdef __vita__
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#endif

namespace Common {

#ifdef __vita__

namespace {
SceKernelLwMutexWork* LwMutex(std::array<u8, 0x20>& work) {
    return reinterpret_cast<SceKernelLwMutexWork*>(work.data());
}
SceKernelLwCondWork* LwCond(std::array<u8, 0x10>& work) {
    return reinterpret_cast<SceKernelLwCondWork*>(work.data());
}
} // namespace

CoreCondition::CoreCondition() {
    const int m = sceKernelCreateLwMutex(LwMutex(lw_mutex), "CoreCondition", 0, 0, nullptr);
    const int c =
        sceKernelCreateLwCond(LwCond(lw_cond), "CoreCondition", 0, LwMutex(lw_mutex), nullptr);
    ASSERT_MSG(m == 0 && c == 0, "CoreCondition: LwMutex {:#010x}, LwCond {:#010x}",
               static_cast<u32>(m), static_cast<u32>(c));
}

CoreCondition::~CoreCondition() {
    sceKernelDeleteLwCond(LwCond(lw_cond));
    sceKernelDeleteLwMutex(LwMutex(lw_mutex));
}

void CoreCondition::lock() {
    const int r = sceKernelLockLwMutex(LwMutex(lw_mutex), 1, nullptr);
    ASSERT_MSG(r == 0, "CoreCondition: lock {:#010x}", static_cast<u32>(r));
}

void CoreCondition::unlock() {
    sceKernelUnlockLwMutex(LwMutex(lw_mutex), 1);
}

bool CoreCondition::WaitFor(std::chrono::microseconds timeout) {
    SceUInt us = static_cast<SceUInt>(std::max<s64>(timeout.count(), 1));
    const int r = sceKernelWaitLwCond(LwCond(lw_cond), &us);
    ASSERT_MSG(r == 0 || r == static_cast<int>(0x80028005), "CoreCondition: wait {:#010x}",
               static_cast<u32>(r));
    return r == 0;
}

void CoreCondition::NotifyAll() {
    sceKernelSignalLwCondAll(LwCond(lw_cond));
}

#else

CoreCondition::CoreCondition() = default;
CoreCondition::~CoreCondition() = default;

void CoreCondition::lock() {
    mutex.lock();
}

void CoreCondition::unlock() {
    mutex.unlock();
}

bool CoreCondition::WaitFor(std::chrono::microseconds timeout) {
    std::unique_lock lk{mutex, std::adopt_lock};
    const bool notified = condvar.wait_for(lk, timeout) == std::cv_status::no_timeout;
    lk.release();
    return notified;
}

void CoreCondition::NotifyAll() {
    condvar.notify_all();
}

#endif

#ifdef _WIN32

void SetCurrentThreadPriority(ThreadPriority new_priority) {
    auto handle = GetCurrentThread();
    int windows_priority = 0;
    switch (new_priority) {
    case ThreadPriority::Low:
        windows_priority = THREAD_PRIORITY_BELOW_NORMAL;
        break;
    case ThreadPriority::Normal:
        windows_priority = THREAD_PRIORITY_NORMAL;
        break;
    case ThreadPriority::High:
        windows_priority = THREAD_PRIORITY_ABOVE_NORMAL;
        break;
    case ThreadPriority::VeryHigh:
        windows_priority = THREAD_PRIORITY_HIGHEST;
        break;
    case ThreadPriority::Critical:
        windows_priority = THREAD_PRIORITY_TIME_CRITICAL;
        break;
    default:
        windows_priority = THREAD_PRIORITY_NORMAL;
        break;
    }
    SetThreadPriority(handle, windows_priority);
}

#else

void SetCurrentThreadPriority(ThreadPriority new_priority) {
    pthread_t this_thread = pthread_self();

    const auto scheduling_type = SCHED_OTHER;
    s32 max_prio = sched_get_priority_max(scheduling_type);
    s32 min_prio = sched_get_priority_min(scheduling_type);
    u32 level = std::max(static_cast<u32>(new_priority) + 1, 4U);

    struct sched_param params;
    if (max_prio > min_prio) {
        params.sched_priority = min_prio + ((max_prio - min_prio) * level) / 4;
    } else {
        params.sched_priority = min_prio - ((min_prio - max_prio) * level) / 4;
    }

    pthread_setschedparam(this_thread, scheduling_type, &params);
}

#endif

#ifdef _MSC_VER

// Sets the debugger-visible name of the current thread.
void SetCurrentThreadName(const char* name) {
    SetThreadDescription(GetCurrentThread(), UTF8ToUTF16W(name).data());
}

#else // !MSVC_VER, so must be POSIX threads

// MinGW with the POSIX threading model does not support pthread_setname_np
#if !defined(_WIN32) || defined(_MSC_VER)
void SetCurrentThreadName(const char* name) {
#ifdef __vita__
    // The console names threads at creation and has no way to rename one afterwards; the
    // thread's identity here is its role, and roles are pinned rather than named.
    (void)name;
#elif defined(__APPLE__)
    pthread_setname_np(name);
#elif defined(__Bitrig__) || defined(__DragonFly__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    pthread_set_name_np(pthread_self(), name);
#elif defined(__NetBSD__)
    pthread_setname_np(pthread_self(), "%s", (void*)name);
#elif defined(__linux__)
    // Linux limits thread names to 15 characters and will outright reject any
    // attempt to set a longer name with ERANGE.
    std::string truncated(name, std::min(strlen(name), static_cast<std::size_t>(15)));
    if (int e = pthread_setname_np(pthread_self(), truncated.c_str())) {
        errno = e;
        LOG_ERROR(Common, "Failed to set thread name to '{}': {}", truncated, GetLastErrorMsg());
    }
#else
    pthread_setname_np(pthread_self(), name);
#endif
}
#endif

#if defined(_WIN32)
void SetCurrentThreadName(const char*) {
    // Do Nothing on MingW
}
#endif

#endif

/**
 * The cores each role may run on (one bit per core), indexed by ThreadRole.
 *
 * Core 2 is not in this table at all: the native backend detaches it from the system scheduler
 * (vanTakeCore) and the guest's ARM11 code runs on it directly. No host thread may be pinned
 * there - a thread whose affinity mask names a detached core is a thread the scheduler can no
 * longer place. The emulation thread is the host-side driver of that core: it spends its slices
 * inside vanRun waiting for the guest to exit, and that wait spins, so it keeps its own core
 * busy and deserves one to itself.
 *
 * Off-console core 2 is left empty rather than used: the Linux tier exists to rehearse this
 * design, and a partition that differs from the console's would measure something else. There
 * the guest executes on the emulation thread itself, so its core carries both halves.
 *
 * On the console the guest also executes on the emulation thread's core since vaprobe's
 * folded mode (2026-09-09): vanRun turns the calling core into the guest's for one slice and
 * back, so the emulation thread and the guest share core 2 and no core is held. The render
 * thread (SgxdRenderer::RenderThread: the rasterizer, sgxd and the presents) has core 1 and the
 * HLE DSP's thread core 0. Everything else - the tier-2 shader compiler, the software rasterizer's worker,
 * which the GXM build does not run at all, and the miscellaneous threads - goes to core 3 with
 * whatever the system keeps there.
 */
constexpr int RoleCores(ThreadRole role, std::size_t index) {
    (void)index;
    switch (role) {
    case ThreadRole::Emulation:
        // Folded native execution: the guest runs on the emulation thread's own core, one
        // slice at a time inside azaharRun (the folded design), so this core is the guest's
        // too and nothing else of ours goes there.
        return 1 << 2;
    case ThreadRole::Dsp:
        // The DSP's frames take a fifth of the emulation thread's time in a game's race.
        return 0;
    case ThreadRole::Render:
        // The PICA stays on the emulation thread; what it draws is built and sent here. Core 1
        // is otherwise idle.
        return 1;
    case ThreadRole::RasterWorker:
    case ThreadRole::ShaderCompiler:
    case ThreadRole::AudioSink:
    case ThreadRole::Other:
        return 1 << 3;
    }
    return 1 << 0;
}

#if defined(__vita__)

constexpr int PriorityCore(ThreadRole role) {
    switch (role) {
    case ThreadRole::Emulation:
    case ThreadRole::Render:
    case ThreadRole::RasterWorker:
    case ThreadRole::AudioSink:
    case ThreadRole::Dsp:
        return SCE_KERNEL_PROCESS_PRIORITY_USER_HIGH;
    case ThreadRole::SecondEmulation:
        // Behind the DSP's frames on core 1: its slices cannot be preempted, so the DSP's thread
        // takes the core whenever the second emulation thread is between them or asleep.
        return SCE_KERNEL_PROCESS_PRIORITY_USER_HIGH + 1;
    case ThreadRole::ShaderCompiler:
        // On core 3 with the miscellaneous threads, before them (CapUnlocker gives
        // applications core 3).
        return SCE_KERNEL_PROCESS_PRIORITY_USER_HIGH + 1;
    case ThreadRole::Other:
        return SCE_KERNEL_PROCESS_PRIORITY_USER_DEFAULT;
    }
    return 0;
}

namespace {
/// The threads SetCurrentThreadRole placed, for RoleThreads: the role plus one above the kernel ID
/// in one word (0: none), a slot a thread, taken again once its thread is gone.
std::array<std::atomic<u64>, 32> placed{};

void NotePlaced(ThreadRole role, int thread_id) {
    const u64 word = u64{static_cast<u32>(role) + 1} << 32 | static_cast<u32>(thread_id);
    for (int pass = 0; pass < 2; pass++) {
        for (auto& p : placed) {
            u64 seen = p.load(std::memory_order_relaxed);
            if (seen != 0 && pass == 1) {
                // Full: a slot whose thread is gone.
                SceKernelThreadInfo info{};
                info.size = sizeof(info);
                if (sceKernelGetThreadInfo(static_cast<SceUID>(static_cast<u32>(seen)), &info) >=
                    0) {
                    continue;
                }
            } else if (seen != 0) {
                continue;
            }
            if (p.compare_exchange_strong(seen, word, std::memory_order_relaxed)) {
                return;
            }
        }
    }
}
} // namespace

std::vector<RoleThread> RoleThreads() {
    std::vector<RoleThread> out;
    for (const auto& p : placed) {
        const u64 word = p.load(std::memory_order_relaxed);
        if (word != 0) {
            const auto role = static_cast<ThreadRole>((word >> 32) - 1);
            out.push_back({role, static_cast<int>(static_cast<u32>(word)), RoleCores(role, 0)});
        }
    }
    return out;
}

void SetCurrentThreadRole(ThreadRole role, std::size_t index) {
    const int cores = RoleCores(role, index);
    // Affinity is one bit per core from bit 16. The top of the four, SCE_KERNEL_CPU_MASK_SYSTEM,
    // is the core the kernel keeps for itself; it is reachable from homebrew loaded through
    // taiHEN, which is how this build runs. Miscellaneous threads are mostly idle, so they may
    // float between the emulation core and the system core rather than crowding either.
    const int mask = SCE_KERNEL_CPU_MASK_USER_0 << core;
    const int thread_id = sceKernelGetThreadId();
    const int result = sceKernelChangeThreadCpuAffinityMask(thread_id, mask);
    if (result < 0) {
        LOG_WARNING(Common, "Could not pin thread to cores {:#x}: {:#010x}", cores, result);
    }
    sceKernelChangeThreadPriority(thread_id, PriorityCore(role));
    NotePlaced(role, thread_id);
}

#elif defined(__linux__)

void SetCurrentThreadRole(ThreadRole role, std::size_t index) {
    // Off-console the partition is a test aid, not a policy: it only applies where the host has
    // the four cores it was written for, so a desktop keeps its own scheduler.
    const long online = sysconf(_SC_NPROCESSORS_ONLN);
    const int cores = RoleCores(role, index);
    if (online < 4) {
        return;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int core = 0; core < 4; core++) {
        if (cores & (1 << core)) {
            CPU_SET(core, &set);
        }
    }
    const int result = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    if (result != 0) {
        LOG_WARNING(Common, "Could not pin thread to cores {:#x}: {}", cores, result);
    }
}

#else

void SetCurrentThreadRole(ThreadRole, std::size_t) {}

#endif

} // namespace Common
