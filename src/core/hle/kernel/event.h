// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <atomic>
#include <vector>
#include <boost/serialization/export.hpp>
#include "core/emu_host.h"
#include "core/hle/kernel/object.h"
#include "core/hle/kernel/resource_limit.h"
#include "core/hle/kernel/wait_object.h"

namespace Kernel {

/**
 * An event, signalled and waited on from either emulation thread (Core::ThisHost) with no lock:
 * its state is one atomic word, and each host keeps its own list of the threads that wait on it.
 * A signal wakes the signalling host's waiters itself and hands the other host's theirs as a
 * message (HostMessage::EventSignal): every signal at least the threads that waited when it
 * came, by the signal count each waiter noted when it began; a one-shot event's signal goes to
 * the other host's waiters only when none of the signalling host's took it, and travels with the
 * message (the token), so no Clear in between takes it back.
 */
class Event final : public WaitObject {
public:
    explicit Event(KernelSystem& kernel);
    ~Event() override;

    std::string GetTypeName() const override {
        return "Event";
    }
    std::string GetName() const override {
        return name;
    }
    void SetName(const std::string& name_) {
        name = name_;
    }

    static constexpr HandleType HANDLE_TYPE = HandleType::Event;
    HandleType GetHandleType() const override {
        return HANDLE_TYPE;
    }

    ResetType GetResetType() const {
        return reset_type;
    }

    bool ShouldWait(const Thread* thread) const override;
    void Acquire(Thread* thread) override;
    bool TryAcquire(Thread* thread) override;

    void AddWaitingThread(std::shared_ptr<Thread> thread) override;
    void RemoveWaitingThread(Thread* thread) override;
    void WakeupAllWaitingThreads() override;

    void Signal();
    void Clear();

    /// The other host's signal number `seq` (HostMessage::EventSignal), on this host's waiters:
    /// those that waited before it; for a one-shot event with `token`, the signal itself.
    void ReceiveSignal(u32 seq, bool token);

    /// Thread `thread`'s wait from host `from`'s list to host `to`'s (one emulation thread owning
    /// both: the second one stopped).
    void MoveWaiter(Thread* thread, u32 from, u32 to);

    /// The state word and each host's waiters, for a thread dump.
    [[nodiscard]] std::array<u32, 3> DebugState() const {
        return {state.load(std::memory_order_relaxed),
                waiter_count[0].load(std::memory_order_relaxed),
                waiter_count[1].load(std::memory_order_relaxed)};
    }

    std::shared_ptr<ResourceLimit> resource_limit;

private:
    /// Host `host`'s waiters: host 0's are WaitObject's list (the savestate's), host 1's its own.
    std::vector<std::shared_ptr<Thread>>& Waiters(u32 host) {
        return host == 0 ? waiting_threads : host1_waiting;
    }
    /// Wakes this host's waiters: with `seq` only those that began before signal `seq`; a
    /// one-shot event's with the signal in hand (`token`) or taken from the state.
    void WakeHostWaiters(u32 host, u32 seq, bool token);

    ResetType reset_type; ///< Current ResetType

    /// Bit 0: signalled. Bits 1-31: the signals so far.
    std::atomic<u32> state{};
    std::vector<std::shared_ptr<Thread>> host1_waiting;
    /// Each host's waiters' signal counts when they began to wait, in their list's order.
    std::array<std::vector<u32>, Core::NumHosts> marks;
    /// Each host's waiters, for the other host's signals.
    std::array<std::atomic<u32>, Core::NumHosts> waiter_count{};
    std::string name; ///< Name of event (optional)
    KernelSystem& kernel;

    friend class KernelSystem;

    friend class boost::serialization::access;
    template <class Archive>
    void serialize(Archive& ar, const unsigned int);
};

} // namespace Kernel

BOOST_CLASS_EXPORT_KEY(Kernel::Event)
BOOST_CLASS_VERSION(Kernel::Event, 1)
CONSTRUCT_KERNEL_OBJECT(Kernel::Event)
