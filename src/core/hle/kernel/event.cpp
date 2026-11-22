// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <boost/serialization/base_object.hpp>
#include <boost/serialization/shared_ptr.hpp>
#include <boost/serialization/string.hpp>
#include <boost/serialization/vector.hpp>
#include "common/archives.h"
#include "common/assert.h"
#include "core/hle/kernel/event.h"
#include "core/hle/kernel/host_link.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/resource_limit.h"
#include "core/hle/kernel/thread.h"

SERIALIZE_EXPORT_IMPL(Kernel::Event)

namespace Kernel {

Event::Event(KernelSystem& kernel) : WaitObject(kernel), kernel(kernel) {}

Event::~Event() {
    if (resource_limit) {
        resource_limit->Release(ResourceLimitType::Event, 1);
    }
}

std::shared_ptr<Event> KernelSystem::CreateEvent(ResetType reset_type, std::string name) {
    auto event = std::make_shared<Event>(*this);

    event->reset_type = reset_type;
    event->name = std::move(name);

    return event;
}

bool Event::ShouldWait(const Thread* thread) const {
    return (state.load(std::memory_order_acquire) & 1u) == 0;
}

bool Event::TryAcquire(Thread* thread) {
    u32 s = state.load(std::memory_order_acquire);
    if (reset_type != ResetType::OneShot) {
        return (s & 1u) != 0;
    }
    while ((s & 1u) != 0) {
        if (state.compare_exchange_weak(s, s & ~1u, std::memory_order_acq_rel,
                                        std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

void Event::Acquire(Thread* thread) {
    // A one-shot event the other emulation thread took between the caller's ShouldWait and here:
    // nothing has shown it yet (a wait for all of several objects).
    const bool acquired = TryAcquire(thread);
    ASSERT_MSG(acquired, "event {} taken by the other emulation thread", name);
}

void Event::AddWaitingThread(std::shared_ptr<Thread> thread) {
    const u32 host = Core::ThisHost();
    auto& list = Waiters(host);
    if (std::find(list.begin(), list.end(), thread) != list.end()) {
        return;
    }
    thread->wait_host = static_cast<u8>(host);
    const u32 before = state.load(std::memory_order_relaxed);
    list.push_back(std::move(thread));
    marks[host].push_back(before >> 1);
    // A signal from the other host between the caller's ShouldWait and the count below did not
    // see this waiter: it comes to this host's inbox as one of the other's would. Each side
    // writes before it reads the other's word, so at least one of them sees the other.
    waiter_count[host].fetch_add(1, std::memory_order_seq_cst);
    if (!Core::multi_host.load(std::memory_order_relaxed)) {
        return;
    }
    const u32 after = state.load(std::memory_order_seq_cst);
    if ((after & 1u) != 0) {
        kernel.SendToHost(host, HostMessage{HostMessage::Kind::EventSignal, false, ~0u,
                                            SharedFrom(this)});
    } else if ((after >> 1) != (before >> 1)) {
        kernel.SendToHost(host, HostMessage{HostMessage::Kind::EventSignal, false, after >> 1,
                                            SharedFrom(this)});
    }
}

void Event::RemoveWaitingThread(Thread* thread) {
    const u32 host = thread->wait_host;
    DEBUG_ASSERT_MSG(host == Core::ThisHost(),
                     "event {}: thread {} waits on host {}'s list, removed from host {}", name,
                     thread->GetThreadId(), host, Core::ThisHost());
    auto& list = Waiters(host);
    const auto it = std::find_if(list.begin(), list.end(),
                                 [thread](const auto& p) { return p.get() == thread; });
    // If a thread passed multiple handles to the same object, the kernel might attempt to remove
    // the thread from the object's waiting threads list multiple times.
    if (it == list.end()) {
        return;
    }
    marks[host].erase(marks[host].begin() + (it - list.begin()));
    list.erase(it);
    waiter_count[host].fetch_sub(1, std::memory_order_relaxed);
}

void Event::WakeHostWaiters(u32 host, u32 seq, bool token) {
    auto& list = Waiters(host);
    for (;;) {
        Thread* best = nullptr;
        u32 best_priority = ThreadPrioLowest + 1;
        for (std::size_t i = 0; i < list.size(); i++) {
            Thread* const t = list[i].get();
            ASSERT_MSG(t->status == ThreadStatus::WaitSynchAny ||
                           t->status == ThreadStatus::WaitSynchAll ||
                           t->status == ThreadStatus::WaitHleEvent,
                       "Inconsistent thread statuses in waiting_threads");
            if (t->current_priority >= best_priority) {
                continue;
            }
            // The other host's signal `seq` wakes the threads that waited before it came.
            if (seq != ~0u && static_cast<s32>(marks[host][i] - seq) >= 0) {
                continue;
            }
            if (t->status == ThreadStatus::WaitSynchAll &&
                std::any_of(t->wait_objects.begin(), t->wait_objects.end(),
                            [this, t](const std::shared_ptr<WaitObject>& object) {
                                return object.get() != this && object->ShouldWait(t);
                            })) {
                continue;
            }
            best = t;
            best_priority = t->current_priority;
        }
        if (best == nullptr) {
            break;
        }
        if (reset_type == ResetType::OneShot) {
            if (token) {
                token = false;
            } else if (!TryAcquire(best)) {
                break;
            }
        }
        const std::shared_ptr<Thread> thread = SharedFrom(best);
        if (thread->IsSleepingOnWaitAll()) {
            for (auto& object : thread->wait_objects) {
                if (object.get() != this) {
                    object->Acquire(thread.get());
                }
            }
        }
        // Invoke the wakeup callback before clearing the wait objects
        if (thread->wakeup_callback) {
            thread->wakeup_callback->WakeUp(ThreadWakeupReason::Signal, thread, SharedFrom(this));
        }
        for (auto& object : thread->wait_objects) {
            object->RemoveWaitingThread(thread.get());
        }
        thread->wait_objects.clear();
        thread->ResumeFromWait();
    }
    // A one-shot signal handed over for a waiter that left meanwhile (its timeout): nothing has
    // shown it yet.
    ASSERT_MSG(!token, "event {}: the other host's signal found no waiter on host {}", name,
               host);
}

void Event::Signal() {
    u32 s = state.load(std::memory_order_relaxed);
    u32 next;
    do {
        next = (s | 1u) + 2u;
    } while (!state.compare_exchange_weak(s, next, std::memory_order_seq_cst,
                                          std::memory_order_relaxed));
    const u32 host = Core::ThisHost();
    WakeHostWaiters(host, ~0u, false);
    if (Core::multi_host.load(std::memory_order_relaxed)) {
        const u32 other = host ^ 1u;
        // The DSP's semaphore event: its notifier is host 0's (the HLE DSP).
        const bool notifier = other == 0 && hle_notifier;
        if (notifier || waiter_count[other].load(std::memory_order_seq_cst) != 0) {
            if (reset_type == ResetType::OneShot && !notifier) {
                // This host's waiters left the signal: it goes to the other's with the message.
                u32 cur = state.load(std::memory_order_relaxed);
                while ((cur & 1u) != 0 &&
                       !state.compare_exchange_weak(cur, cur & ~1u, std::memory_order_seq_cst,
                                                    std::memory_order_relaxed)) {
                }
                if ((cur & 1u) != 0) {
                    kernel.SendToHost(other, HostMessage{HostMessage::Kind::EventSignal, true,
                                                         next >> 1, SharedFrom(this)});
                }
            } else {
                kernel.SendToHost(other, HostMessage{HostMessage::Kind::EventSignal, false,
                                                     next >> 1, SharedFrom(this)});
            }
        }
    }
    if (reset_type == ResetType::Pulse) {
        state.fetch_and(~1u, std::memory_order_seq_cst);
    }
    if (host == 0 && hle_notifier) {
        hle_notifier();
    }
}

void Event::ReceiveSignal(u32 seq, bool token) {
    const u32 host = Core::ThisHost();
    WakeHostWaiters(host, reset_type == ResetType::OneShot ? ~0u : seq, token);
    if (host == 0 && hle_notifier) {
        hle_notifier();
    }
}

void Event::Clear() {
    state.fetch_and(~1u, std::memory_order_seq_cst);
}

void Event::WakeupAllWaitingThreads() {
    WakeHostWaiters(Core::ThisHost(), ~0u, false);
}

template <class Archive>
void Event::serialize(Archive& ar, const unsigned int file_version) {
    ar& boost::serialization::base_object<WaitObject>(*this); // host 0's waiters
    ar & reset_type;
    bool signaled = (state.load(std::memory_order_relaxed) & 1u) != 0;
    ar & signaled;
    ar & name;
    ar & resource_limit;
    // Version 1: the signal count, each waiter's mark and host 1's waiters (a savestate is made
    // with host 1 stopped).
    u32 count = state.load(std::memory_order_relaxed) >> 1;
    if (file_version >= 1) {
        ar & count;
        ar & marks[0];
        ar & host1_waiting;
        ar & marks[1];
    }
    if (Archive::is_loading::value) {
        state.store(count << 1 | (signaled ? 1u : 0u), std::memory_order_relaxed);
        if (file_version < 1) {
            marks[0].assign(waiting_threads.size(), count);
            marks[1].clear();
            host1_waiting.clear();
        }
        for (const auto& t : host1_waiting) {
            t->wait_host = 1;
        }
        waiter_count[0].store(static_cast<u32>(waiting_threads.size()), std::memory_order_relaxed);
        waiter_count[1].store(static_cast<u32>(host1_waiting.size()), std::memory_order_relaxed);
    }
}
SERIALIZE_IMPL(Event)

} // namespace Kernel
