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

void CoreCondition::Wait() {
    const int r = sceKernelWaitLwCond(LwCond(lw_cond), nullptr);
    ASSERT_MSG(r == 0, "CoreCondition: wait {:#010x}", static_cast<u32>(r));
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

void CoreCondition::Wait() {
    std::unique_lock lk{mutex, std::adopt_lock};
    condvar.wait(lk);
    lk.release();
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
 * The guest executes on the emulation thread's core: azaharRun turns the calling core into the
 * guest's for one slice and back (docs/native-execution.md), so the emulation thread and the guest
 * share core 2, and nothing else of ours goes there. The second
 * emulation thread (the emulated cores other than core 0) has core 1 the same way, with the HLE
 * DSP's thread beside it (and on core 3 when core 1 is taken); the render thread (SgxdRenderer::RenderThread: the rasterizer, sgxd and
 * the presents) has core 0 with Y2R. Everything else - the command lists' parse, the
 * tier-2 shader compiler and the miscellaneous threads - goes to core 3 with whatever the system
 * keeps there.
 */
constexpr int RoleCores(ThreadRole role, std::size_t index) {
    (void)index;
    switch (role) {
    case ThreadRole::Emulation:
        // Folded native execution: the guest runs on the emulation thread's own core, one
        // slice at a time inside azaharRun (the folded design), so this core is the guest's
        // too and nothing else of ours goes there.
        return 1 << 2;
    case ThreadRole::SecondEmulation:
        // The emulated cores other than core 0, the guest on this core as on core 2.
        return 1 << 1;
    case ThreadRole::Dsp:
        // The DSP's frames take a fifth of the emulation thread's time in a game's race. Beside the
        // second emulation thread, which sleeps most of the time (a game's ~870 ms a second)
        // and leaves the core to it between its slices (PriorityCore), or on core 3: on core 1
        // alone, the DSP ran ahead of core 2's job for half of the main thread's wait for it
        // (a game).
        return 1 << 1 | 1 << 3;
    case ThreadRole::Parse:
        // The command lists' parse, ahead of the render thread, on a core of its own (on core 0 the
        // render thread waited for it). Core 3, with the mostly idle rest.
        return 1 << 3;
    case ThreadRole::Y2r:
        // A video's frames, converted while the guest decodes the next one: core 0 again.
        return 1 << 0;
    case ThreadRole::Render:
        // The PICA stays on the emulation thread; what it draws is built and sent here. Core 0:
        // the second emulation thread has core 1.
        return 1 << 0;
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
    case ThreadRole::SecondEmulation:
        return SCE_KERNEL_PROCESS_PRIORITY_USER_HIGH;
    case ThreadRole::Dsp:
        // Behind the second emulation thread: with core 1 taken the DSP's frames go to core 3
        // (RoleCores), and the emulated cores' jobs the main thread waits for go on (a game's
        // wait for core 2's job: the DSP on core 1 for 46% of it at the same priority).
        return SCE_KERNEL_PROCESS_PRIORITY_USER_HIGH + 1;
    case ThreadRole::ShaderCompiler:
        // On core 3 with the miscellaneous threads, before them (CapUnlocker gives
        // applications core 3).
        return SCE_KERNEL_PROCESS_PRIORITY_USER_HIGH + 1;
    case ThreadRole::Parse:
    case ThreadRole::Y2r:
        // Behind the render thread on core 0 (Y2R), before the rest on core 3 (the parse).
        return SCE_KERNEL_PROCESS_PRIORITY_USER_HIGH + 1;
    case ThreadRole::Other:
        return SCE_KERNEL_PROCESS_PRIORITY_USER_DEFAULT;
    }
    return 0;
}

namespace {
/// The threads SetCurrentThreadRole placed, for RoleThreads: the role plus one above the kernel ID
/// in one word (0: none), a slot a thread, given back as its thread ends (ForgetCurrentThreadRole).
std::array<std::atomic<u64>, 64> placed{};

void NotePlaced(ThreadRole role, int thread_id) {
    const u64 word = u64{static_cast<u32>(role) + 1} << 32 | static_cast<u32>(thread_id);
    // A thread placed again keeps its slot. No other thread is looked up: sceKernelGetThreadInfo
    // of another thread's ID is a critical error that debug-suspends the caller
    // (~15 ms).
    for (auto& p : placed) {
        if (static_cast<u32>(p.load(std::memory_order_relaxed)) == static_cast<u32>(thread_id)) {
            p.store(word, std::memory_order_relaxed);
            return;
        }
    }
    for (auto& p : placed) {
        u64 expected = 0;
        if (p.compare_exchange_strong(expected, word, std::memory_order_relaxed)) {
            return;
        }
    }
    ASSERT_MSG(false, "SetCurrentThreadRole: more than {} threads placed at once", placed.size());
}
} // namespace

void ForgetCurrentThreadRole() {
    const u32 self = static_cast<u32>(sceKernelGetThreadId());
    for (auto& p : placed) {
        u64 seen = p.load(std::memory_order_relaxed);
        if (seen != 0 && static_cast<u32>(seen) == self) {
            p.compare_exchange_strong(seen, 0, std::memory_order_relaxed);
            return;
        }
    }
}

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

int RoleCoreMask(ThreadRole role) {
    return RoleCores(role, 0);
}

void SetCurrentThreadRole(ThreadRole role, std::size_t index) {
    const int cores = RoleCores(role, index);
    // Affinity is one bit per core from bit 16. The top of the four, SCE_KERNEL_CPU_MASK_SYSTEM,
    // is the core the kernel keeps for itself; it is reachable from homebrew loaded through
    // taiHEN, which is how this build runs. Miscellaneous threads are mostly idle, so they may
    // float between the emulation core and the system core rather than crowding either.
    const int mask = SCE_KERNEL_CPU_MASK_USER_0 * cores;
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
