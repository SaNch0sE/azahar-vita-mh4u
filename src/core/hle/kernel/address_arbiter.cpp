// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <atomic>
#include <boost/serialization/base_object.hpp>
#include <boost/serialization/shared_ptr.hpp>
#include <boost/serialization/string.hpp>
#include <boost/serialization/vector.hpp>
#include "common/archives.h"
#include "common/common_types.h"
#include "common/logging/log.h"
#include "core/hle/kernel/address_arbiter.h"
#include "core/hle/kernel/errors.h"
#include "core/hle/kernel/host_link.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/resource_limit.h"
#include "core/hle/kernel/thread.h"
#include "core/memory.h"

SERIALIZE_EXPORT_IMPL(Kernel::AddressArbiter)
SERIALIZE_EXPORT_IMPL(Kernel::AddressArbiter::Callback)

namespace Kernel {

void AddressArbiter::WaitThread(std::shared_ptr<Thread> thread, VAddr wait_address) {
    // Counted already (waiter_count), before the address was read.
    thread->wait_address = wait_address;
    thread->status = ThreadStatus::WaitArb;
    thread->wait_host = static_cast<u8>(Core::ThisHost());
    thread->wait_arbiter = this;
    HostWaiters().emplace_back(std::move(thread));
}

void AddressArbiter::ResumeSignaled(Thread& thread) {
    // The timeout types answered the waiter ResultTimeout when it went to sleep (ArbitrateAddress,
    // below); a signal before the timeout is a success. Without this libctru's
    // LightEvent_WaitTimeout, which loops until the timeout result, takes every signal for a
    // timeout.
    thread.SetWaitSynchronizationResult(ResultSuccess);
    thread.ResumeFromWait();
}

u64 AddressArbiter::ResumeHostWaiters(VAddr address, s32 count) {
    // The threads waiting on this address, taken in the list's order where it is kept. No
    // std::stable_partition: its temporary buffer was an allocation a call.
    auto& list = HostWaiters();
    const auto waits_here = [address](const auto& thread) {
        ASSERT_MSG(thread->status == ThreadStatus::WaitArb, "Inconsistent AddressArbiter state");
        return thread->wait_address == address;
    };
    u64 woken = 0;
    if (count < 0) {
        // Negative count means resume all threads
        auto kept = list.begin();
        for (auto it = list.begin(); it != list.end(); ++it) {
            if (waits_here(*it)) {
                ResumeSignaled(**it);
                woken++;
            } else {
                if (kept != it) {
                    *kept = std::move(*it);
                }
                ++kept;
            }
        }
        list.erase(kept, list.end());
    } else {
        // The highest priority one each time. Note: The real kernel will pick the first thread in
        // the list if more than one have the same highest priority value. Lower priority values
        // mean higher priority.
        for (; woken < static_cast<u64>(count); woken++) {
            auto itr = list.end();
            for (auto it = list.begin(); it != list.end(); ++it) {
                if (waits_here(*it) &&
                    (itr == list.end() || (*it)->current_priority < (*itr)->current_priority)) {
                    itr = it;
                }
            }
            if (itr == list.end()) {
                break;
            }
            ResumeSignaled(**itr);
            list.erase(itr);
        }
    }
    waiter_count[Core::ThisHost()].fetch_sub(static_cast<u32>(woken), std::memory_order_relaxed);
    return woken;
}

u64 AddressArbiter::ResumeAllThreads(VAddr address) {
    return ResumeHostWaiters(address, -1);
}

bool AddressArbiter::ResumeHighestPriorityThread(VAddr address) {
    return ResumeHostWaiters(address, 1) != 0;
}

void AddressArbiter::ReceiveSignal(VAddr address, s32 count) {
    ResumeHostWaiters(address, count);
}

void AddressArbiter::MoveWaiterToFirstHost(Thread* thread) {
    const auto it = std::find_if(host1_waiting.begin(), host1_waiting.end(),
                                 [thread](const auto& t) { return t.get() == thread; });
    if (it == host1_waiting.end()) {
        return;
    }
    waiting_threads.push_back(std::move(*it));
    host1_waiting.erase(it);
    waiter_count[1].fetch_sub(1, std::memory_order_relaxed);
    waiter_count[0].fetch_add(1, std::memory_order_relaxed);
    thread->wait_host = 0;
}

AddressArbiter::AddressArbiter(KernelSystem& kernel)
    : Object(kernel), kernel(kernel), timeout_callback(std::make_shared<Callback>(*this)) {}

AddressArbiter::~AddressArbiter() {
    if (resource_limit) {
        resource_limit->Release(ResourceLimitType::AddressArbiter, 1);
    }
}

std::shared_ptr<AddressArbiter> KernelSystem::CreateAddressArbiter(std::string name) {
    auto address_arbiter = std::make_shared<AddressArbiter>(*this);
    address_arbiter->name = std::move(name);
    return address_arbiter;
}

class AddressArbiter::Callback : public WakeupCallback {
public:
    explicit Callback(AddressArbiter& _parent) : parent(_parent) {}
    AddressArbiter& parent;

    void WakeUp(ThreadWakeupReason reason, std::shared_ptr<Thread> thread,
                std::shared_ptr<WaitObject> object) override {
        parent.WakeUp(reason, std::move(thread), std::move(object));
    }

private:
    template <class Archive>
    void serialize(Archive& ar, const unsigned int) {
        ar& boost::serialization::base_object<WakeupCallback>(*this);
    }
    friend class boost::serialization::access;
};

void AddressArbiter::WakeUp(ThreadWakeupReason reason, std::shared_ptr<Thread> thread,
                            std::shared_ptr<WaitObject> object) {
    ASSERT(reason == ThreadWakeupReason::Timeout);
    // Remove the newly-awakened thread from the Arbiter's waiting list (its host's: a timeout runs
    // on the host that made the wait).
    DEBUG_ASSERT(thread->wait_host == Core::ThisHost());
    const auto removed = std::erase(HostWaiters(), thread);
    waiter_count[Core::ThisHost()].fetch_sub(static_cast<u32>(removed), std::memory_order_relaxed);
};

Result AddressArbiter::ArbitrateAddress(std::shared_ptr<Thread> thread, ArbitrationType type,
                                        VAddr address, s32 value, u64 nanoseconds) {
    switch (type) {

    // Signal thread(s) waiting for arbitrate address...
    case ArbitrationType::Signal: {
        // The guest stored to the address before this svc; the other emulation thread's waiter
        // counts itself before it reads the address (below): one of the two sees the other.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        // Negative value means resume all threads; this host's waiters, then the rest of the
        // signal to the other emulation thread's.
        const u64 num_threads = ResumeHostWaiters(address, value < 0 ? -1 : value);
        const u32 other = Core::ThisHost() ^ 1u;
        if (Core::multi_host.load(std::memory_order_relaxed) &&
            (value < 0 || num_threads < static_cast<u64>(value)) &&
            waiter_count[other].load(std::memory_order_seq_cst) != 0) {
            HostMessage message{HostMessage::Kind::ArbiterSignal, false, address,
                                SharedFrom(this)};
            message.count = value < 0 ? -1 : value - static_cast<s32>(num_threads);
            kernel.SendToHost(other, std::move(message));
        }

        // Prevents lag from low priority threads that spam svcArbitrateAddress and wake no threads
        // The tick count is taken directly from official HOS kernel. The priority value is one less
        // than official kernel as the affected FMV threads dont meet the priority threshold of 50.
        // TODO: Revisit this when scheduler is rewritten and adjust if there isn't a problem there.
        if (num_threads == 0 && thread->current_priority >= 49) {
            kernel.current_cpu->GetTimer().AddTicks(1614u);
        }
        break;
    }
    // Wait current thread (acquire the arbiter)...
    case ArbitrationType::WaitIfLessThan:
    case ArbitrationType::WaitIfLessThanWithTimeout:
    case ArbitrationType::DecrementAndWaitIfLessThan:
    case ArbitrationType::DecrementAndWaitIfLessThanWithTimeout: {
        // Counted before the address is read: the other emulation thread's signal either sees
        // the count and sends the rest of its signal here, or this read sees the store it
        // signalled for.
        const u32 host = Core::ThisHost();
        waiter_count[host].fetch_add(1, std::memory_order_seq_cst);
        u8* const word = kernel.memory.GetPointer(address);
        ASSERT_MSG(word != nullptr, "arbitration on unmapped address {:08X}", address);
        std::atomic_ref<u32> memory_value{*reinterpret_cast<u32*>(word)};
        const bool decrement = type == ArbitrationType::DecrementAndWaitIfLessThan ||
                               type == ArbitrationType::DecrementAndWaitIfLessThanWithTimeout;
        u32 current = memory_value.load(std::memory_order_seq_cst);
        bool wait = static_cast<s32>(current) < value;
        // Only change the memory value if the thread should wait, in one step: the other core's
        // guest may change it meanwhile.
        while (wait && decrement &&
               !memory_value.compare_exchange_weak(current, current - 1,
                                                   std::memory_order_seq_cst)) {
            wait = static_cast<s32>(current) < value;
        }
        if (!wait) {
            waiter_count[host].fetch_sub(1, std::memory_order_relaxed);
            break;
        }
        if (type == ArbitrationType::WaitIfLessThanWithTimeout ||
            type == ArbitrationType::DecrementAndWaitIfLessThanWithTimeout) {
            thread->wakeup_callback = timeout_callback;
            thread->WakeAfterDelay(nanoseconds);
        }
        WaitThread(std::move(thread), address);
        break;
    }

    default:
        LOG_ERROR(Kernel, "unknown type={}", type);
        return ResultInvalidEnumValueFnd;
    }

    // The calls that use a timeout seem to always return a Timeout error even if they did not put
    // the thread to sleep
    if (type == ArbitrationType::WaitIfLessThanWithTimeout ||
        type == ArbitrationType::DecrementAndWaitIfLessThanWithTimeout) {
        return ResultTimeout;
    }
    return ResultSuccess;
}

template <class Archive>
void AddressArbiter::serialize(Archive& ar, const unsigned int) {
    ar& boost::serialization::base_object<Object>(*this);
    ar & name;
    ar & waiting_threads; // host 0's: a savestate is made with the second thread stopped
    ar & timeout_callback;
    ar & resource_limit;
    if (Archive::is_loading::value) {
        waiter_count[0].store(static_cast<u32>(waiting_threads.size()), std::memory_order_relaxed);
    }
}
SERIALIZE_IMPL(AddressArbiter)

} // namespace Kernel

namespace boost::serialization {

template <class Archive>
void save_construct_data(Archive& ar, const Kernel::AddressArbiter::Callback* t,
                         const unsigned int) {
    ar << Kernel::SharedFrom(&t->parent);
}

template <class Archive>
void load_construct_data(Archive& ar, Kernel::AddressArbiter::Callback* t, const unsigned int) {
    std::shared_ptr<Kernel::AddressArbiter> parent;
    ar >> parent;
    ::new (t) Kernel::AddressArbiter::Callback(*parent);
}

} // namespace boost::serialization
