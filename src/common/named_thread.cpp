// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#ifdef __vita__

#include <cstdlib>
#include <string>

#include <pthread.h>
#include <reent.h>
#include <psp2/kernel/threadmgr.h>
#include <vitasdk/utils.h>

#include "common/assert.h"
#include "common/named_thread.h"

extern "C" {
// newlib's own thread-exit helpers (lib_a-threading.o): clean up the thread's lazily
// created reentrancy state, then sceKernelExitThread / sceKernelExitDeleteThread. Not
// declared in any public header.
[[noreturn]] void vita_exit_thread(int status);
[[noreturn]] void vita_exit_delete_thread(int status);
// pte's per-thread setup for the calling thread (pthread-embedded platform/vita): builds
// the pspThreadData record {entryPoint, argv, cancel event flag} that every cancellable
// wait dereferences. Returns PTE_OS_OK (0) on success.
int pte_osInit(void);
// pte's end of a thread pthread_create did not make (pthread-embedded pte_detach.c): the
// thread's key destructors, then its implicit pthread record back for reuse and its OS side
// deleted (pte_osThreadDelete: the record above, the key list, the reentrancy state).
int pte_thread_detach_np(void);
// newlib's reentrancy state of the calling thread (sys/vita/threading.c). Not `_REENT`: newlib is
// built with __DYNAMIC_REENT__ but its installed headers do not define it, so outside newlib
// `_REENT` is the global state (_impure_ptr), no thread's.
struct _reent* __getreent(void);
}

namespace Common {

namespace {

enum : std::uint32_t {
    StateRunning = 0,
    StateDetached = 1,
    StateFinished = 2,
};

std::atomic<std::size_t> running_threads{0};

/// The calling thread's dtoa cache of powers of five, freed. newlib builds it in the thread's
/// reentrancy state the first time the thread prints a float (pow5mult), and reclaiming that state
/// when another thread takes its slot frees the state's Bigint lists but not this chain: one a
/// title would be left on the heap.
void FreeDtoaPowersOfFive() {
    struct _reent* const reent = __getreent();
#ifdef _REENT_SMALL
    if (reent->_mp == nullptr) {
        return;
    }
#endif
    for (struct _Bigint* p = _REENT_MP_P5S(reent); p != nullptr;) {
        struct _Bigint* const next = p->_next;
        std::free(p);
        p = next;
    }
    _REENT_MP_P5S(reent) = nullptr;
}

} // namespace

struct NamedThread::Ctrl {
    std::function<void()> run;
    // Two-party handoff between the owner and the thread: whoever moves second - the
    // thread finishing, or the owner detaching - learns the other's decision and takes the
    // cleanup that falls to it.
    std::atomic<std::uint32_t> state{StateRunning};
};

int NamedThread::Trampoline(unsigned int args, void* argp) {
    auto* ctrl = *static_cast<NamedThread::Ctrl**>(argp);

    // Attach to newlib's pthread layer: pte's cancellable waits (under every std::mutex
    // and condition variable) dereference the pspThreadData record unconditionally, and
    // only pthread_create-made threads get one for free - a bare SCE thread crashes in
    // pte_osSemaphoreCancellablePend without it. pte_osInit builds the record
    // for the calling thread, exactly as pte_osThreadCreate does on its first-ever call.
    void** slot = static_cast<void**>(vitasdk_get_pthread_data(0));
    bool attached = false;
    if (slot != nullptr && *slot == nullptr) {
        attached = pte_osInit() == 0;
    }

    ctrl->run();

    FreeDtoaPowersOfFive();
    if (attached) {
        // The thread's pthread side ends with it, as a pthread_create thread's does: the first
        // pthread call on a foreign thread gives it an implicit record (with two mutexes, each
        // a kernel object), its keys' values (the C++ thread_locals' emulated storage) are
        // freed only by their destructors, and its key list hangs off the reentrancy state.
        // The render thread, the DSP's and the services' workers come and go with each title,
        // and would leave all of it on the heap (17 chunks a title). pthread_self makes the
        // implicit record if nothing did, so that detaching always deletes the record attached
        // above (it frees it; the slot goes with the reentrancy state).
        static_cast<void>(pthread_self());
        pte_thread_detach_np();
    }

    ForgetCurrentThreadRole();
    running_threads--;
    const bool detached = ctrl->state.exchange(StateFinished) == StateDetached;
    if (detached) {
        delete ctrl; // the owner is gone; this side frees and self-deletes
        vita_exit_delete_thread(0);
    }
    vita_exit_thread(0); // the owner joins, deletes the thread, and frees ctrl
}

void NamedThread::Launch(const ThreadCfg& cfg, std::function<void()> f) {
    auto* c = new Ctrl{std::move(f)};
    uid = sceKernelCreateThread(cfg.name, &Trampoline, 0x10000100,
                                static_cast<SceSize>(cfg.stack_size), 0, 0, nullptr);
    // A thread that never runs leaves whoever waits on it waiting for good. The stack comes out
    // of the game
    // partition, which a launch can find too short.
    ASSERT_MSG(uid >= 0, "thread \"{}\": sceKernelCreateThread {:#010x} for a {} KiB stack",
               cfg.name, static_cast<u32>(uid), cfg.stack_size >> 10);
    ctrl = c;
    running_threads++;
    sceKernelStartThread(uid, sizeof(c), &c);
}

void NamedThread::join() {
    if (uid < 0) {
        return;
    }
    // Under Vita3K a worker joined right after its stop request (the AM scan worker at a
    // savestate load's shutdown) never returns from this wait although the
    // thread's callable has returned and it has entered newlib's exit; the same join on a
    // thread that exited earlier returns at once. Not reproduced on the console.
    sceKernelWaitThreadEnd(uid, nullptr, nullptr);
    sceKernelDeleteThread(uid);
    delete ctrl;
    uid = -1;
    ctrl = nullptr;
}

void NamedThread::detach() {
    if (uid < 0) {
        return;
    }
    if (ctrl->state.exchange(StateDetached) == StateFinished) {
        // The thread already finished as joinable; the cleanup falls here.
        join();
        return;
    }
    uid = -1;
    ctrl = nullptr;
}

bool NamedThread::IsCurrentThread() const noexcept {
    return uid >= 0 && sceKernelGetThreadId() == uid;
}

std::size_t RunningNamedThreads() {
    return running_threads.load();
}

std::string DescribeWaiters(std::int32_t uid) {
    const auto name_of = [](SceUID id) {
        SceKernelThreadInfo info{};
        info.size = sizeof(info);
        if (sceKernelGetThreadInfo(id, &info) < 0) {
            return std::string("?");
        }
        info.name[sizeof(info.name) - 1] = '\0';
        return std::string(info.name);
    };
    const SceUID self = sceKernelGetThreadId();
    std::string out = "this thread " + name_of(self) + "; the named threads waiting on it:";
    std::int32_t ids[64];
    const std::size_t n = RunningNamedThreadIds(ids, std::size(ids));
    bool any = false;
    for (std::size_t i = 0; i < n; i++) {
        SceKernelThreadInfo info{};
        info.size = sizeof(info);
        if (ids[i] != self && sceKernelGetThreadInfo(ids[i], &info) >= 0 && info.waitId == uid) {
            info.name[sizeof(info.name) - 1] = '\0';
            out += std::string(any ? ", " : " ") + info.name;
            any = true;
        }
    }
    return any ? out : out + " none";
}

} // namespace Common

#endif // __vita__
