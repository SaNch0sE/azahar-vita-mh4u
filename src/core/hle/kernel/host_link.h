// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include "common/common_types.h"
#include "common/thread.h"

namespace Kernel {

class Object;

/**
 * What one emulation thread (Core::ThisHost) hands the other, with no lock on either side: an
 * event's signal for the waiters on the receiving host's list, or a thread the sender woke whose
 * core the receiver runs.
 */
struct HostMessage {
    enum class Kind : u8 {
        EventSignal, ///< Event::ReceiveSignal(seq, token)
        Handoff,     ///< Thread::ResumeHandedOff
        ArbiterSignal, ///< AddressArbiter::ReceiveSignal(seq, count)
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

    /// The owning host, with nothing to run: returns at once if `has_work()` (checked with the
    /// sleeper published, so no Push or Wake is lost), else after the next Push or Wake.
    template <typename HasWork>
    void Sleep(HasWork&& has_work) {
        std::scoped_lock lock{cond};
        sleeping.store(true, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (Empty() && !has_work()) {
            cond.Wait();
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
    alignas(64) u32 head{};
    std::atomic<bool> sleeping{};
    Common::CoreCondition cond;
};

} // namespace Kernel
