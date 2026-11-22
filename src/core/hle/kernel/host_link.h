// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <atomic>
#include <memory>
#include "common/common_types.h"
#include "common/wake_flag.h"

namespace Kernel {

class Object;

/**
 * What one emulation thread (Core::ThisHost) hands the other, with no lock on either side: an
 * event's signal for the waiters on the receiving host's list, a thread the sender woke,
 * created or changed the priority of whose core the receiver runs, or a change of core 1's CPU
 * limit, whose limiter is that core's host's.
 */
struct HostMessage {
    enum class Kind : u8 {
        EventSignal, ///< Event::ReceiveSignal(seq, token)
        Handoff,     ///< Thread::ResumeHandedOff
        NewThread,   ///< ThreadManager::AdoptThread
        ArbiterSignal, ///< AddressArbiter::ReceiveSignal(seq, count)
        Requeue,       ///< Thread::Requeue: its priority changed on the sending host
        Core1Limit,    ///< core 1's CPU limit changed: its schedule mode (token, seq) or its share
    };
    Kind kind{};
    /// EventSignal of a one-shot event: the sender took its signal for this host's waiters.
    bool token{};
    /// EventSignal: the event's signal count with this signal. ArbiterSignal: the address.
    u32 seq{};
    std::shared_ptr<Object> object;
    /// ArbiterSignal: the waiters left to wake (-1: all).
    s32 count{};
};

/**
 * A host's messages: a bounded ring any thread pushes to with one compare-exchange (Vyukov's
 * bounded queue) and only its host pops. The host sleeps on it when it has nothing to run.
 */
class HostInbox {
public:
    static constexpr u32 Size = 4096;

    HostInbox();

    /// Any thread. A full ring stops the run: it means the host stopped taking its messages.
    void Push(HostMessage&& message);
    /// The owning host only.
    bool Pop(HostMessage& out);
    [[nodiscard]] bool Empty() const;
    /// Any thread: messages sent that the owning host has not taken yet.
    [[nodiscard]] bool Pending() const;

    /// The owning host, with nothing to run: returns at once if `has_work()`, else after the
    /// next Push or Wake. No lock: the flag goes down before the sleeper is published and the
    /// work checked, so a Push or Wake after the check raises it again and the wait ends.
    template <typename HasWork>
    void Sleep(HasWork&& has_work) {
        SleepUntil([&] { return !Empty() || has_work(); });
    }
    /// Sleep that leaves the messages where they are: returns once `done()`, a Push or a Wake
    /// also ending a wait the caller then makes again.
    template <typename Done>
    void SleepUntil(Done&& done) {
        flag.Lower();
        sleeping.store(true, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (!done()) {
            flag.Wait();
        }
        sleeping.store(false, std::memory_order_relaxed);
    }
    /// Any thread: ends a Sleep (the caller changed what `has_work` reads first).
    void Wake();

private:
    struct Cell {
        std::atomic<u32> seq;
        HostMessage message;
    };
    std::array<Cell, Size> cells;
    alignas(64) std::atomic<u32> tail{};
    alignas(64) std::atomic<u32> head{};
    std::atomic<bool> sleeping{};
    /// The Vita kernel's event flag (WakeFlag): the sleeper's wait and the senders' wake.
    Common::WakeFlag flag;
};

} // namespace Kernel
