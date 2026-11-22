// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <string>
#include <boost/serialization/export.hpp>
#include "common/common_types.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/resource_limit.h"
#include "core/hle/kernel/wait_object.h"
#include "core/hle/result.h"

namespace Kernel {

class Thread;

class Mutex final : public WaitObject {
public:
    explicit Mutex(KernelSystem& kernel);
    ~Mutex() override;

    std::string GetTypeName() const override {
        return "Mutex";
    }
    std::string GetName() const override {
        return name;
    }

    static constexpr HandleType HANDLE_TYPE = HandleType::Mutex;
    HandleType GetHandleType() const override {
        return HANDLE_TYPE;
    }

    std::shared_ptr<ResourceLimit> resource_limit;
    int lock_count;   ///< Number of times the mutex has been acquired
    u32 priority;     ///< The priority of the mutex, used for priority inheritance.
    std::string name; ///< Name of mutex (optional)
    std::shared_ptr<Thread> holding_thread; ///< Thread that has acquired the mutex

    /**
     * Elevate the mutex priority to the best priority
     * among the priorities of all its waiting threads.
     */
    void UpdatePriority();

    bool ShouldWait(const Thread* thread) const override;
    void Acquire(Thread* thread) override;

    void AddWaitingThread(std::shared_ptr<Thread> thread) override;
    void RemoveWaitingThread(Thread* thread) override;

    /**
     * Attempts to release the mutex from the specified thread.
     * @param thread Thread that wants to release the mutex.
     * @returns The result code of the operation.
     */
    Result Release(Thread* thread);

    /// A lease (the Vita native core's): while no thread waits on the mutex, the guest's own
    /// code locks and unlocks it through this word, which the core's svc thunks share: free
    /// (LeaseFree), held by a thread (its ID << 8 | its lock count, 1-255), or the kernel's
    /// (LeaseKernel: lock_count and holding_thread are the truth). Every kernel use of the mutex
    /// takes the state back first (Reclaim), and it is lent again free, held, or held with
    /// waiters (Relet).
    static constexpr u32 LeaseFree = 0;
    static constexpr u32 LeaseKernel = 0xFFFFFFFF;
    /// With a holder's word: threads wait on it (the kernel has them, and the holder's boost).
    /// The holder's nested locks and unlocks change the count in the word; its last unlock comes
    /// to the kernel.
    static constexpr u32 LeaseWaiters = 0x80000000;
    u32* lease = nullptr;
    void Reclaim() const;
    void Relet();

private:
    KernelSystem& kernel;

    friend class boost::serialization::access;
    template <class Archive>
    void serialize(Archive& ar, const unsigned int);
};

/**
 * Releases all the mutexes held by the specified thread
 * @param thread Thread that is holding the mutexes
 */
void ReleaseThreadMutexes(Thread* thread);

/// The Vita native core's side of the leases (Mutex::lease): a leased mutex's handle closed, a
/// leased mutex gone, and a thread that ends taking back the mutexes it holds by lease.
struct MutexLeaseHooks {
    void (*closed)(Mutex& mutex, Handle handle) = nullptr;
    void (*freed)(Mutex& mutex) = nullptr;
    void (*reclaim_held)(u32 thread_id) = nullptr;
};
extern MutexLeaseHooks mutex_lease_hooks;

} // namespace Kernel

BOOST_CLASS_EXPORT_KEY(Kernel::Mutex)
CONSTRUCT_KERNEL_OBJECT(Kernel::Mutex)
