// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/mutex.h"

#ifdef __vita__

#include <psp2/kernel/threadmgr.h>
#include "common/assert.h"

namespace Common {

namespace {

constexpr int LwMutexRecursive = 0x02;                    // SCE_KERNEL_MUTEX_ATTR_RECURSIVE
constexpr int WaitTimeout = static_cast<int>(0x80028005); // SCE_KERNEL_ERROR_WAIT_TIMEOUT

SceKernelLwMutexWork* LwMutex(std::array<u8, 0x20>& work) {
    return reinterpret_cast<SceKernelLwMutexWork*>(work.data());
}
SceKernelLwCondWork* LwCond(std::array<u8, 0x10>& work) {
    return reinterpret_cast<SceKernelLwCondWork*>(work.data());
}

void Create(std::array<u8, 0x20>& work, const char* name, int attr) {
    const int r = sceKernelCreateLwMutex(LwMutex(work), name, attr, 0, nullptr);
    ASSERT_MSG(r == 0, "{}: sceKernelCreateLwMutex {:#010x}", name, static_cast<u32>(r));
}

void Lock(std::array<u8, 0x20>& work) {
    const int r = sceKernelLockLwMutex(LwMutex(work), 1, nullptr);
    ASSERT_MSG(r == 0, "sceKernelLockLwMutex {:#010x}", static_cast<u32>(r));
}

void Unlock(std::array<u8, 0x20>& work) {
    const int r = sceKernelUnlockLwMutex(LwMutex(work), 1);
    DEBUG_ASSERT_MSG(r == 0, "sceKernelUnlockLwMutex {:#010x}", static_cast<u32>(r));
}

} // Anonymous namespace

Mutex::Mutex() {
    Create(work, "Common::Mutex", 0);
}

Mutex::~Mutex() {
    sceKernelDeleteLwMutex(LwMutex(work));
}

void Mutex::lock() {
    Lock(work);
}

void Mutex::unlock() {
    Unlock(work);
}

bool Mutex::try_lock() {
    return sceKernelTryLockLwMutex(LwMutex(work), 1) == 0;
}

RecursiveMutex::RecursiveMutex() {
    Create(work, "Common::RecursiveMutex", LwMutexRecursive);
}

RecursiveMutex::~RecursiveMutex() {
    sceKernelDeleteLwMutex(LwMutex(work));
}

void RecursiveMutex::lock() {
    Lock(work);
}

void RecursiveMutex::unlock() {
    Unlock(work);
}

bool RecursiveMutex::try_lock() {
    return sceKernelTryLockLwMutex(LwMutex(work), 1) == 0;
}

ConditionVariable::ConditionVariable() {
    Create(lw_mutex, "Common::ConditionVariable", 0);
    const int c = sceKernelCreateLwCond(LwCond(lw_cond), "Common::ConditionVariable", 0,
                                        LwMutex(lw_mutex), nullptr);
    ASSERT_MSG(c == 0, "Common::ConditionVariable: sceKernelCreateLwCond {:#010x}",
               static_cast<u32>(c));
}

ConditionVariable::~ConditionVariable() {
    sceKernelDeleteLwCond(LwCond(lw_cond));
    sceKernelDeleteLwMutex(LwMutex(lw_mutex));
}

void ConditionVariable::notify_one() noexcept {
    Enter();
    if (waiters != 0) {
        sceKernelSignalLwCond(LwCond(lw_cond));
    }
    Leave();
}

void ConditionVariable::notify_all() noexcept {
    Enter();
    if (waiters != 0) {
        sceKernelSignalLwCondAll(LwCond(lw_cond));
    }
    Leave();
}

void ConditionVariable::Enter() {
    Lock(lw_mutex);
}

void ConditionVariable::Leave() {
    Unlock(lw_mutex);
}

void ConditionVariable::Block(u32* timeout_us) {
    waiters++;
    const int r = sceKernelWaitLwCond(LwCond(lw_cond), timeout_us);
    // A timed-out wait holds the mutex again, as a woken one does: the kernel's wait takes it
    // back on both (its timeout, 0x80028005, joins the wake's path).
    ASSERT_MSG(r == 0 || (timeout_us != nullptr && r == WaitTimeout),
               "Common::ConditionVariable: sceKernelWaitLwCond {:#010x}", static_cast<u32>(r));
    waiters--;
    Leave();
}

} // namespace Common

#endif
