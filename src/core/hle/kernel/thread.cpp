// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <climits>
#include <boost/serialization/string.hpp>
#include <boost/serialization/unordered_map.hpp>
#include <boost/serialization/vector.hpp>
#include <boost/serialization/weak_ptr.hpp>
#include "common/archives.h"
#include "common/assert.h"
#include "common/common_types.h"
#include "common/logging/log.h"
#include "common/serialization/boost_flat_set.h"
#include "common/settings.h"
#include "core/arm/arm_interface.h"
#include "core/arm/skyeye_common/armstate.h"
#include "core/core.h"
#ifdef ENABLE_GDBSTUB
#include "core/gdbstub/gdbstub.h"
#endif
#include "core/hle/kernel/address_arbiter.h"
#include "core/hle/kernel/errors.h"
#include "core/hle/kernel/host_link.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/mutex.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/resource_limit.h"
#include "core/hle/kernel/thread.h"
#include "core/hle/result.h"
#include "core/memory.h"

SERIALIZE_EXPORT_IMPL(Kernel::Thread)
SERIALIZE_EXPORT_IMPL(Kernel::WakeupCallback)

namespace Kernel {

template <class Archive>
void ThreadManager::serialize(Archive& ar, const unsigned int) {
    ar & current_thread;
    ar & ready_queue;
    ar & wakeup_callback_table;
    ar & thread_list;
    ar & current_schedule_mode;
    ar & single_time_limiter;
    ar & multi_time_limiter;
}
SERIALIZE_IMPL(ThreadManager)

template <class Archive>
void Thread::serialize(Archive& ar, const unsigned int file_version) {
    ar& boost::serialization::base_object<WaitObject>(*this);
    ar & context;
    ar & thread_id;
    ar & status;
    ar & entry_point;
    ar & stack_top;
    ar & nominal_priority;
    ar & current_priority;
    ar & last_running_ticks;
    ar & processor_id;
    ar & tls_address;
    ar & held_mutexes;
    ar & pending_mutexes;
    ar & owner_process;
    ar & resource_limit_category;
    ar & wait_objects;
    ar & wait_address;
    ar & name;
    if (Archive::is_loading::value) {
        bool serialize_blocked;
        ar & serialize_blocked;
        if (!serialize_blocked) {
            ar & wakeup_callback;
        }
    } else {
        bool serialize_blocked = wakeup_callback.get() && !wakeup_callback->SupportsSerialization();
        ar & serialize_blocked;
        if (!serialize_blocked) {
            ar & wakeup_callback;
        }
    }
    ar & wakeup_callback;
    ar & unschedule_mode;
    if (Archive::is_loading::value) {
        wakeup_queued = true; // the event queue came back with whatever it held
    }
}
SERIALIZE_IMPL(Thread)

template <class Archive>
void WakeupCallback::serialize(Archive& ar, const unsigned int) {}
SERIALIZE_IMPL(WakeupCallback)

bool Thread::ShouldWait(const Thread* thread) const {
    return status != ThreadStatus::Dead;
}

void Thread::Acquire(Thread* thread) {
    ASSERT_MSG(!ShouldWait(thread), "object unavailable!");
}

Thread::Thread(KernelSystem& kernel, u32 core_id)
    : WaitObject(kernel), core_id(core_id), thread_manager(kernel.GetThreadManager(core_id)) {}

Thread::~Thread() = default;

Thread* ThreadManager::GetCurrentThread() const {
    return current_thread.get();
}

void Thread::Stop() {
    // The callback that would have woken the thread goes first. An HLE async section's waits for
    // its section to end (which then schedules this thread's wakeup, cancelled below), and holds
    // what the request captured: a mapped buffer holds its process, whose handle table holds this
    // thread. Kept, the thread, the process and its handle table held each other past the
    // kernel's end, a title's whole process left on the heap with its 5 MiB page table (a quit
    // with a request in flight).
    wakeup_callback = nullptr;

    // Cancel any outstanding wakeup events for this thread
    CancelWakeup();
    thread_manager.wakeup_callback_table.erase(thread_id);

    // Clean up thread from ready queue
    // This is only needed when the thread is termintated forcefully (SVC TerminateProcess)
    if (status == ThreadStatus::Ready) {
        thread_manager.ready_queue.remove(current_priority, this);
    }

    status = ThreadStatus::Dead;

    WakeupAllWaitingThreads();

    // Clean up any dangling references in objects that this thread was waiting for
    for (auto& wait_object : wait_objects) {
        wait_object->RemoveWaitingThread(this);
    }
    wait_objects.clear();

    // Release all the mutexes that this thread holds
    ReleaseThreadMutexes(this);

    // Mark the TLS slot in the thread's page as free.
    u32 tls_page = (tls_address - Memory::TLS_AREA_VADDR) / Memory::CITRA_PAGE_SIZE;
    u32 tls_slot =
        ((tls_address - Memory::TLS_AREA_VADDR) % Memory::CITRA_PAGE_SIZE) / Memory::TLS_ENTRY_SIZE;
    if (auto process = owner_process.lock()) {
        process->tls_slots[tls_page].reset(tls_slot);
        process->resource_limit->Release(ResourceLimitType::Thread, 1);
    }

#ifdef ENABLE_GDBSTUB
    GDBStub::OnThreadExit(thread_id);
#endif
}

void ThreadManager::SwitchContext(Thread* new_thread) {
    Thread* previous_thread = GetCurrentThread();
    std::shared_ptr<Process> previous_process = nullptr;

    Core::Timing& timing = kernel.timing;

    // Save context for previous thread
    if (previous_thread) {
        previous_process = previous_thread->owner_process.lock();
        previous_thread->last_running_ticks = cpu->GetTimer().GetTicks();
        cpu->SaveContext(previous_thread->context);

        if (previous_thread->status == ThreadStatus::Running) {
            // This is only the case when a reschedule is triggered without the current thread
            // yielding execution (i.e. an event triggered, system core time-sliced, etc)
            ready_queue.push_front(previous_thread->current_priority, previous_thread);
            previous_thread->status = ThreadStatus::Ready;
        }
    }

    // Load context of new thread
    if (new_thread) {
        ASSERT_MSG(new_thread->status == ThreadStatus::Ready,
                   "Thread must be ready to become running.");

        // Cancel any outstanding wakeup events for this thread
        new_thread->CancelWakeup();

        current_thread = SharedFrom(new_thread);

        ready_queue.remove(new_thread->current_priority, new_thread);
        new_thread->status = ThreadStatus::Running;

        ASSERT(current_thread->owner_process.lock());
        if (previous_process != current_thread->owner_process.lock()) {
            kernel.SetCurrentProcessForCPU(current_thread->owner_process.lock(), cpu->GetID());
        }

        cpu->LoadContext(new_thread->context);
        cpu->SetCP15Register(CP15_THREAD_URO, new_thread->GetTLSAddress());
    } else {
        current_thread = nullptr;
        // Note: We do not reset the current process and current page table when idling because
        // technically we haven't changed processes, our threads are just paused.
    }
}

Thread* ThreadManager::PopNextReadyThread() {
    Thread* next;
    Thread* thread = GetCurrentThread();

    while (true) {
        std::vector<std::pair<u32, Thread*>> skipped;
        u32 next_priority{};
        next = nullptr;

        if (thread && thread->status == ThreadStatus::Running && thread->CanSchedule()) {
            do {
                // We have to do better than the current thread.
                // This call returns null when that's not possible.
                std::tie(next_priority, next) =
                    ready_queue.pop_first_better(thread->current_priority);
                if (!next) {
                    // Otherwise just keep going with the current thread
                    next = thread;
                    break;
                } else if (!next->CanSchedule()) {
                    skipped.push_back({next_priority, next});
                }

            } while (!next->CanSchedule());
        } else {
            do {
                std::tie(next_priority, next) = ready_queue.pop_first();
                if (next && !next->CanSchedule()) {
                    skipped.push_back({next_priority, next});
                }
            } while (next && !next->CanSchedule());
        }

        for (auto it = skipped.rbegin(); it != skipped.rend(); it++) {
            ready_queue.push_front(it->first, it->second);
        }

        // Try to time limit the selected thread on core 1
        if (core_id == 1 && next && GetCpuLimiter()->DoTimeLimit(next)) {
            // If the thread is time limited, select the next one
            continue;
        }

        break;
    }

    return next;
}

void ThreadManager::WaitCurrentThread_Sleep() {
    Thread* thread = GetCurrentThread();
    thread->status = ThreadStatus::WaitSleep;
}

void ThreadManager::ExitCurrentThread() {
    current_thread->Stop();
    std::erase(thread_list, current_thread);
    kernel.PrepareReschedule();
}

void ThreadManager::TerminateProcessThreads(std::shared_ptr<Process> process) {
    auto iter = thread_list.begin();
    while (iter != thread_list.end()) {
        auto& thread = *iter;
        if (thread == current_thread || thread->owner_process.lock() != process) {
            iter++;
            continue;
        }

        if (thread->status != ThreadStatus::WaitSynchAny &&
            thread->status != ThreadStatus::WaitSynchAll) {
            // TODO: How does the real kernel handle non-waiting threads?
            LOG_WARNING(Kernel, "Terminating non-waiting thread {}", thread->thread_id);
        }

        thread->Stop();
        iter = thread_list.erase(iter);
    }

    // Kill the current thread last, if applicable.
    if (current_thread != nullptr && current_thread->owner_process.lock() == process) {
        ExitCurrentThread();
    }
}

void ThreadManager::ThreadWakeupCallback(u64 thread_id, s64 cycles_late) {
    std::shared_ptr<Thread> thread = SharedFrom(wakeup_callback_table.at(thread_id));
    if (thread == nullptr) {
        LOG_CRITICAL(Kernel, "Callback fired for invalid thread {:08X}", thread_id);
        return;
    }
    thread->wakeup_queued = false;

    if (thread->status == ThreadStatus::WaitSynchAny ||
        thread->status == ThreadStatus::WaitSynchAll || thread->status == ThreadStatus::WaitArb ||
        thread->status == ThreadStatus::WaitHleEvent) {

        // Invoke the wakeup callback before clearing the wait objects
        if (thread->wakeup_callback)
            thread->wakeup_callback->WakeUp(ThreadWakeupReason::Timeout, thread, nullptr);

        // Remove the thread from each of its waiting objects' waitlists
        for (auto& object : thread->wait_objects)
            object->RemoveWaitingThread(thread.get());
        thread->wait_objects.clear();
    }

    thread->ResumeFromWait();
}

void Thread::WakeAfterDelay(s64 nanoseconds, bool thread_safe_mode) {
    // Don't schedule a wakeup if the thread wants to wait forever
    if (nanoseconds == -1)
        return;
    // From another host thread (an async section's end): on the timer of the emulation thread
    // whose lists the wait is on, core 0's for a wait host 0 made for another host's thread.
    std::size_t core = std::numeric_limits<std::size_t>::max();
    if (thread_safe_mode) {
        core = wait_host == Core::HostOfCore(core_id) ? core_id : 0;
    }

    thread_manager.kernel.timing.ScheduleEvent(nsToCycles(nanoseconds),
                                               thread_manager.ThreadWakeupEventType, thread_id,
                                               core, thread_safe_mode);
    wakeup_queued = true;
}

void Thread::CancelWakeup() {
    if (!wakeup_queued) {
        DEBUG_ASSERT_MSG(thread_manager.cpu == nullptr ||
                             Core::HostOfCore(core_id) != Core::ThisHost() ||
                             thread_manager.cpu->GetTimer().EventTime(
                                 thread_manager.ThreadWakeupEventType, thread_id) < 0,
                         "thread {} has a wakeup queued but no mark of it", thread_id);
        return;
    }
    thread_manager.kernel.timing.UnscheduleEvent(thread_manager.ThreadWakeupEventType, thread_id);
    wakeup_queued = false;
}

void Thread::ResumeFromWait() {
    ASSERT_MSG(wait_objects.empty(), "Thread is waking up while waiting for objects");

    // Woken already, on its way to the emulation thread that runs its core.
    if (handoff_pending) {
        return;
    }

    switch (status) {
    case ThreadStatus::WaitSynchAll:
    case ThreadStatus::WaitSynchAny:
    case ThreadStatus::WaitHleEvent:
    case ThreadStatus::WaitArb:
    case ThreadStatus::WaitSleep:
    case ThreadStatus::WaitIPC:
    case ThreadStatus::Dormant:
        break;

    case ThreadStatus::Ready:
        // The thread's wakeup callback must have already been cleared when the thread was first
        // awoken.
        ASSERT(wakeup_callback == nullptr);
        // If the thread is waiting on multiple wait objects, it might be awoken more than once
        // before actually resuming. We can ignore subsequent wakeups if the thread status has
        // already been set to ThreadStatus::Ready.
        return;

    case ThreadStatus::Running:
        DEBUG_ASSERT_MSG(false, "Thread with object id {} has already resumed.", GetObjectId());
        return;
    case ThreadStatus::Dead:
        // This should never happen, as threads must complete before being stopped.
        DEBUG_ASSERT_MSG(false, "Thread with object id {} cannot be resumed because it's DEAD.",
                         GetObjectId());
        return;
    }

    wakeup_callback = nullptr;

    // A thread of the other emulation thread's cores: its scheduler is that thread's to change.
    // Its wakeup is this host's to cancel (a wait this host made for it times out here).
    if (const u32 owner = Core::HostOfCore(core_id); owner != Core::ThisHost()) {
        CancelWakeup();
        handoff_pending = true;
        thread_manager.kernel.SendToHost(
            owner, HostMessage{HostMessage::Kind::Handoff, false, 0, SharedFrom(this)});
        return;
    }

    thread_manager.ready_queue.push_back(current_priority, this);
    status = ThreadStatus::Ready;
    thread_manager.kernel.PrepareReschedule();
}

void Thread::ResumeHandedOff() {
    ASSERT(handoff_pending && Core::HostOfCore(core_id) == Core::ThisHost());
    handoff_pending = false;
    thread_manager.ready_queue.push_back(current_priority, this);
    status = ThreadStatus::Ready;
    thread_manager.kernel.PrepareReschedule();
}

void ThreadManager::DebugThreadQueue() {
    Thread* thread = GetCurrentThread();
    if (!thread) {
        LOG_DEBUG(Kernel, "Current: NO CURRENT THREAD");
    } else {
        LOG_DEBUG(Kernel, "0x{:02X} {} (current)", thread->current_priority,
                  GetCurrentThread()->GetObjectId());
    }

    for (auto& t : thread_list) {
        u32 priority = ready_queue.contains(t.get());
        if (priority != UINT_MAX) {
            LOG_DEBUG(Kernel, "0x{:02X} {}", priority, t->GetObjectId());
        }
    }
}

/**
 * Resets a thread context, making it ready to be scheduled and run by the CPU
 * @param context Thread context to reset
 * @param stack_top Address of the top of the stack
 * @param entry_point Address of entry point for execution
 * @param arg User argument for thread
 */
static void ResetThreadContext(Core::ARM_Interface::ThreadContext& context, u32 stack_top,
                               u32 entry_point, u32 arg) {
    context.cpu_registers[0] = arg;
    context.SetProgramCounter(entry_point);
    context.SetStackPointer(stack_top);
    context.cpsr = USER32MODE | ((entry_point & 1) << 5); // Usermode and THUMB mode
}

ResultVal<std::shared_ptr<Thread>> KernelSystem::CreateThread(
    std::string name, VAddr entry_point, u32 priority, u32 arg, s32 processor_id, VAddr stack_top,
    std::shared_ptr<Process> owner_process, bool make_ready) {
    // Check if priority is in ranged. Lowest priority -> highest priority id.
    if (priority > ThreadPrioLowest) {
        LOG_ERROR(Kernel_SVC, "Invalid thread priority: {}", priority);
        return ResultOutOfRange;
    }

    if (processor_id > ThreadProcessorIdMax) {
        LOG_ERROR(Kernel_SVC, "Invalid processor id: {}", processor_id);
        return ResultOutOfRangeKernel;
    }
    // Indexes thread_managers below: one per emulated core, two on an Old 3DS.
    if (processor_id < 0 || static_cast<std::size_t>(processor_id) >= thread_managers.size()) {
        LOG_ERROR(Kernel_SVC, "Processor id {} past the {} cores", processor_id,
                  thread_managers.size());
        return ResultOutOfRangeKernel;
    }

    // TODO(yuriks): Other checks, returning 0xD9001BEA
    if (!memory.IsValidVirtualAddress(*owner_process, entry_point)) {
        LOG_ERROR(Kernel_SVC, "(name={}): invalid entry {:08x}", name, entry_point);
        // TODO: Verify error
        return Result(ErrorDescription::InvalidAddress, ErrorModule::Kernel,
                      ErrorSummary::InvalidArgument, ErrorLevel::Permanent);
    }

    auto thread = std::make_shared<Thread>(*this, processor_id);

    thread_managers[processor_id]->thread_list.push_back(thread);
    thread_managers[processor_id]->ready_queue.prepare(priority);

    thread->thread_id = NewThreadId();
    thread->status = ThreadStatus::Dormant;
    thread->entry_point = entry_point;
    thread->stack_top = stack_top;
    thread->nominal_priority = thread->current_priority = priority;
    thread->last_running_ticks = timing.GetTimer(processor_id)->GetTicks();
    thread->processor_id = processor_id;
    thread->wait_objects.clear();
    thread->wait_address = 0;
    thread->name = std::move(name);
    thread_managers[processor_id]->wakeup_callback_table[thread->thread_id] = thread.get();
    thread->owner_process = owner_process;
    thread->resource_limit_category = owner_process->resource_limit->GetCategory();
    CASCADE_RESULT(thread->tls_address, owner_process->AllocateThreadLocalStorage());

    // TODO(peachum): move to ScheduleThread() when scheduler is added so selected core is used
    // to initialize the context
    ResetThreadContext(thread->context, stack_top, entry_point, arg);

    if (make_ready) {
        thread_managers[processor_id]->ready_queue.push_back(thread->current_priority,
                                                             thread.get());
        thread->status = ThreadStatus::Ready;
    }

    return thread;
}

void Thread::SetPriority(u32 priority) {
    ASSERT_MSG(priority <= ThreadPrioLowest && priority >= ThreadPrioHighest,
               "Invalid priority value.");
    // If thread was ready, adjust queues
    if (status == ThreadStatus::Ready)
        thread_manager.ready_queue.move(this, current_priority, priority);
    else
        thread_manager.ready_queue.prepare(priority);

    nominal_priority = current_priority = priority;
}

void Thread::UpdatePriority() {
    u32 best_priority = nominal_priority;
    for (auto& mutex : held_mutexes) {
        if (mutex->priority < best_priority)
            best_priority = mutex->priority;
    }
    BoostPriority(best_priority);
}

void Thread::BoostPriority(u32 priority) {
    // If thread was ready, adjust queues
    if (status == ThreadStatus::Ready)
        thread_manager.ready_queue.move(this, current_priority, priority);
    else
        thread_manager.ready_queue.prepare(priority);
    current_priority = priority;
}

std::shared_ptr<Thread> SetupMainThread(KernelSystem& kernel, u32 entry_point, u32 priority,
                                        std::shared_ptr<Process> owner_process) {

    constexpr s64 sleep_app_thread_ns = 2'600'000'000LL;
    constexpr u32 system_module_tid_high = 0x00040130;

    const bool is_lle_service =
        static_cast<u32>(owner_process->codeset->program_id >> 32) == system_module_tid_high;

    s64 sleep_time_ns = 0;
    if (!is_lle_service && kernel.GetAppMainThreadExtendedSleep()) {
        if (Settings::values.delay_start_for_lle_modules) {
            sleep_time_ns = sleep_app_thread_ns;
        }
        kernel.SetAppMainThreadExtendedSleep(false);
    }

    // Initialize new "main" thread
    auto thread_res = kernel.CreateThread(
        fmt::format("{}-main", owner_process->codeset->name), entry_point, priority, 0,
        owner_process->ideal_processor, Memory::HEAP_VADDR_END, owner_process, sleep_time_ns == 0);

    std::shared_ptr<Thread> thread = std::move(thread_res).Unwrap();

    thread->context.fpscr =
        FPSCR_DEFAULT_NAN | FPSCR_FLUSH_TO_ZERO | FPSCR_ROUND_TOZERO | FPSCR_IXC; // 0x03C00010

    if (sleep_time_ns != 0) {
        thread->status = ThreadStatus::WaitSleep;
        thread->WakeAfterDelay(sleep_time_ns);
    }

    // Note: The newly created thread will be run when the scheduler fires.
    return thread;
}

int ThreadManager::ExportNativeSchedule(std::vector<NativeSchedEntry>& out, std::size_t max,
                                        const AddressArbiter* arbiter) {
    out.clear();
    Thread* const running = GetCurrentThread();
    if (running == nullptr || running->status != ThreadStatus::Running || !running->CanSchedule()) {
        return -1;
    }
    const auto process = running->owner_process.lock();
    out.push_back({running, 3, 0, 0});
    bool takeable = true;
    std::array<s32, ThreadPrioLowest + 1> next{};
    ready_queue.for_each([&](u32 priority, Thread* t) {
        if (t->owner_process.lock() != process || !t->CanSchedule()) {
            takeable = false;
            return;
        }
        out.push_back({t, 1, next[priority]++, 0});
    });
    if (!takeable) {
        return -1;
    }
    Core::Timing::Timer& timer = cpu->GetTimer();
    timer.MoveEvents();
    for (const auto& t : thread_list) {
        if (t->status != ThreadStatus::WaitSleep || t->owner_process.lock() != process ||
            !t->CanSchedule()) {
            continue;
        }
        // Asleep for good (no wakeup event) stays the app's.
        const s64 wake = timer.EventTime(ThreadWakeupEventType, t->thread_id);
        if (wake >= 0) {
            out.push_back({t.get(), 2, 0, static_cast<u64>(wake)});
        }
    }
    // The arbiter's waiters, every one of them: a signal in the module must see them all. The
    // arbiter is the process's, and another emulated core's threads wait on it too: those are
    // woken there but never run here (state 5), their timeouts on their own core's timer.
    if (arbiter != nullptr) {
        s32 index = 0;
        for (const auto& t : arbiter->GetWaitingThreads()) {
            if (t->status != ThreadStatus::WaitArb || t->owner_process.lock() != process) {
                return -1;
            }
            if (t->core_id != core_id) {
                out.push_back({t.get(), 5, index++, ~0ull, t->wait_address});
                continue;
            }
            if (!t->CanSchedule()) {
                return -1;
            }
            const s64 wake = timer.EventTime(ThreadWakeupEventType, t->thread_id);
            out.push_back(
                {t.get(), 4, index++, wake >= 0 ? static_cast<u64>(wake) : ~0ull, t->wait_address});
        }
    }
    return out.size() <= max ? 0 : -1;
}

void ThreadManager::ImportNativeSchedule(const std::vector<NativeSchedEntry>& before,
                                         const std::vector<NativeSchedEntry>& after, int current,
                                         u64 now, AddressArbiter* arbiter) {
    Core::Timing& timing = kernel.timing;
    std::vector<const NativeSchedEntry*> ready;
    std::vector<const NativeSchedEntry*> waits;
    for (std::size_t i = 0; i < after.size(); i++) {
        const NativeSchedEntry& was = before[i];
        const NativeSchedEntry& is = after[i];
        Thread* const t = is.thread;
        if (was.state == 1) {
            ready_queue.remove(t->current_priority, t);
        }
        const bool same_sleep = was.state == 2 && is.state == 2 && was.wake == is.wake;
        if (was.state == 2 && !same_sleep) {
            t->CancelWakeup();
        }
        // A wait at the arbiter the module left alone keeps its place (a new wait takes the
        // list's next order); any other leaves the list and its timeout.
        const bool same_wait = was.state == 4 && is.state == 4 && was.order == is.order;
        if (was.state == 4 && !same_wait) {
            ASSERT(arbiter != nullptr);
            arbiter->RemoveNativeWaiter(t);
            if (was.wake != ~0ull) {
                t->CancelWakeup();
            }
            t->wakeup_callback = nullptr;
        }
        switch (is.state) {
        case 1:
            t->status = ThreadStatus::Ready;
            ready.push_back(&is);
            break;
        case 2:
            t->status = ThreadStatus::WaitSleep;
            if (!same_sleep) {
                timing.ScheduleEvent(is.wake > now ? static_cast<s64>(is.wake - now) : 0,
                                     ThreadWakeupEventType, t->thread_id);
                t->wakeup_queued = true;
            }
            break;
        case 4:
            if (!same_wait) {
                waits.push_back(&is);
            }
            break;
        case 5:
            break; // another core's waiter the slice left waiting
        case 6:
            // Another core's waiter a signal here woke: ready on its own core, as
            // AddressArbiter::ResumeSignaled leaves it (its context has the module's success).
            ASSERT(arbiter != nullptr && was.state == 5);
            arbiter->RemoveNativeWaiter(t);
            t->ResumeFromWait();
            break;
        default:
            t->status = ThreadStatus::Running;
            break;
        }
        if (was.state == 3 && is.state != 3) {
            t->last_running_ticks = now;
        }
    }
    // The module's ready order: its pushes to the front and back of each level.
    std::sort(ready.begin(), ready.end(), [](const NativeSchedEntry* a, const NativeSchedEntry* b) {
        return a->order < b->order;
    });
    for (const NativeSchedEntry* e : ready) {
        ready_queue.push_back(e->thread->current_priority, e->thread);
    }
    // The waits the module began, at the arbiter list's back in the order it began them.
    std::sort(waits.begin(), waits.end(), [](const NativeSchedEntry* a, const NativeSchedEntry* b) {
        return a->order < b->order;
    });
    for (const NativeSchedEntry* e : waits) {
        ASSERT(arbiter != nullptr);
        const bool timed = e->wake != ~0ull;
        arbiter->AddNativeWaiter(SharedFrom(e->thread), e->address, timed);
        if (timed) {
            timing.ScheduleEvent(e->wake > now ? static_cast<s64>(e->wake - now) : 0,
                                 ThreadWakeupEventType, e->thread->thread_id);
            e->thread->wakeup_queued = true;
        }
    }
    current_thread = current >= 0 ? SharedFrom(after[current].thread) : nullptr;
}

Thread* ThreadManager::SwitchForNative() {
    Thread* const cur = GetCurrentThread();
    Thread* const next = PopNextReadyThread();
    // The running thread keeps going. One that stopped and is ready again (woken while the
    // upcall idled the guest: popped off the ready queue) becomes the running one below.
    if (next == nullptr || (next == cur && cur->status == ThreadStatus::Running)) {
        return next;
    }
    if (cur == nullptr || next->owner_process.lock() != cur->owner_process.lock()) {
        ready_queue.push_front(next->current_priority, next);
        return nullptr;
    }
    // SwitchContext's, but for SaveContext and LoadContext.
    cur->last_running_ticks = cpu->GetTimer().GetTicks();
    if (cur->status == ThreadStatus::Running) {
        ready_queue.push_front(cur->current_priority, cur);
        cur->status = ThreadStatus::Ready;
    }
    ASSERT_MSG(next->status == ThreadStatus::Ready, "Thread must be ready to become running.");
    next->CancelWakeup();
    current_thread = SharedFrom(next);
    ready_queue.remove(next->current_priority, next);
    next->status = ThreadStatus::Running;
    return next;
}

bool ThreadManager::HaveReadyThreads() {
    return ready_queue.get_first() != nullptr;
}

void ThreadManager::Reschedule() {
    Thread* cur = GetCurrentThread();
    Thread* next = PopNextReadyThread();

    if (cur && next) {
        LOG_TRACE(Kernel, "context switch {} -> {}", cur->GetObjectId(), next->GetObjectId());
    } else if (cur) {
        LOG_TRACE(Kernel, "context switch {} -> idle", cur->GetObjectId());
    } else if (next) {
        LOG_TRACE(Kernel, "context switch idle -> {}", next->GetObjectId());
    } else {
        LOG_TRACE(Kernel, "context switch idle -> idle, do nothing");
        return;
    }

    SwitchContext(next);
}

void Thread::SetWaitSynchronizationResult(Result result) {
    context.cpu_registers[0] = result.raw;
}

void Thread::SetWaitSynchronizationOutput(s32 output) {
    context.cpu_registers[1] = output;
}

s32 Thread::GetWaitObjectIndex(const WaitObject* object) const {
    ASSERT_MSG(!wait_objects.empty(), "Thread is not waiting for anything");
    const auto match = std::find_if(wait_objects.rbegin(), wait_objects.rend(),
                                    [object](const auto& p) { return p.get() == object; });
    return static_cast<s32>(std::distance(match, wait_objects.rend()) - 1);
}

VAddr Thread::GetCommandBufferAddress() const {
    // Offset from the start of TLS at which the IPC command buffer begins.
    constexpr u32 command_header_offset = 0x80;
    return GetTLSAddress() + command_header_offset;
}

bool Thread::SetUnscheduleMode(UnscheduleMode mode) {
    UnscheduleMode old = unschedule_mode;

    unschedule_mode |= mode;

    return unschedule_mode != old;
}

bool Thread::ClearUnscheduleMode(UnscheduleMode mode) {
    UnscheduleMode old = unschedule_mode;

    unschedule_mode &= ~mode;

    return unschedule_mode != old;
}

CpuLimiter::~CpuLimiter() {}

CpuLimiterMulti::CpuLimiterMulti(Kernel::KernelSystem& _kernel) : kernel(_kernel) {}

void CpuLimiterMulti::Initialize(bool is_single) {
    // TODO(PabloMK7): The is_single variable is needed to prevent
    // registering an event twice with the same name. Once CpuLimiterSingle
    // is implemented we can remove it.
    tick_event = kernel.timing.RegisterEvent(
        fmt::format("Kernel::{}::tick_event", is_single ? "CpuLimiterSingle" : "CpuLimiterMulti"),
        [this](u64, s64 cycles_late) { this->OnTick(cycles_late); });
}

void CpuLimiterMulti::Start() {
    if (ready) {
        return;
    }
    ready = true;
    active = false;
    curr_state = SchedState::APP; // So that ChangeState starts with SYS
    app_cpu_time = Core1CpuTime::PREEMPTION_DISABLED;
}

void CpuLimiterMulti::End() {
    if (!ready) {
        return;
    }
    ready = false;
    active = false;
    kernel.timing.UnscheduleEvent(tick_event, 0);
    WakeupSleepingThreads();
}

void CpuLimiterMulti::UpdateAppCpuLimit() {
    if (!ready) {
        return;
    }

    app_cpu_time = static_cast<u32>(kernel.ResourceLimit()
                                        .GetForCategory(Kernel::ResourceLimitCategory::Application)
                                        ->GetCurrentValue(Kernel::ResourceLimitType::CpuTime));
    if (app_cpu_time == Core1CpuTime::PREEMPTION_DISABLED) {
        // No preemption, disable event
        active = false;
        kernel.timing.UnscheduleEvent(tick_event, 0);
        WakeupSleepingThreads();
    } else {
        // Start preempting, enable event
        if (active) {
            // If we were active already, unschedule first
            // so that the event is not scheduled twice.
            // We could just not call ChangeState instead,
            // but this allows adjusting the timing of the
            // event sooner.
            kernel.timing.UnscheduleEvent(tick_event, 0);
        }
        active = true;
        ChangeState(0);
    }
}

bool CpuLimiterMulti::DoTimeLimit(Thread* thread) {
    if (!ready || !active) {
        // Preemption is not active, don't do anything.
        return false;
    }
    if (kernel.ResourceLimit()
            .GetForCategory(thread->resource_limit_category)
            ->GetLimitValue(ResourceLimitType::CpuTime) == Core1CpuTime::PREEMPTION_EXCEMPTED) {
        // The thread is excempted from preemption
        return false;
    }

    // On real hardware, the kernel uses a KPreemptionTimer to determine if a
    // thread needs to be time limited. This properly uses the resource limit
    // value to check if it is a sysmodule or not. We can do this instead and
    // it should be good enough. TODO(PabloMK7): fix?
    if (thread->resource_limit_category == ResourceLimitCategory::Application &&
            curr_state == SchedState::SYS ||
        thread->resource_limit_category == ResourceLimitCategory::Other &&
            curr_state == SchedState::APP) {
        // Block thread as not in the current mode
        thread->status = ThreadStatus::WaitSleep;
        sleeping_thread_ids.push(thread->thread_id);
        return true;
    }
    return false;
}

void CpuLimiterMulti::OnTick(s64 cycles_late) {
    WakeupSleepingThreads();
    ChangeState(cycles_late);
}

void CpuLimiterMulti::ChangeState(s64 cycles_late) {
    curr_state = (curr_state == SchedState::SYS) ? SchedState::APP : SchedState::SYS;

    s64 next_timer = base_tick_interval * (app_cpu_time / 100.f);
    if (curr_state == SchedState::SYS) {
        next_timer = base_tick_interval - next_timer;
    }
    if (next_timer > cycles_late) {
        next_timer -= cycles_late;
    }
    kernel.timing.ScheduleEvent(next_timer, tick_event, 0, 1);
}

void CpuLimiterMulti::WakeupSleepingThreads() {
    while (!sleeping_thread_ids.empty()) {
        u32 curr_id = sleeping_thread_ids.front();

        auto thread = kernel.GetThreadManager(1).GetThreadByID(curr_id);
        if (thread && thread->status == ThreadStatus::WaitSleep) {
            thread->ResumeFromWait();
        }

        sleeping_thread_ids.pop();
    }
}

std::string CpuLimiterMulti::DebugState() const {
    std::string ids;
    for (std::queue<u32> q = sleeping_thread_ids; !q.empty(); q.pop()) {
        fmt::format_to(std::back_inserter(ids), " {}", q.front());
    }
    return fmt::format("ready {} active {} app time {} state {} tick events queued {}, asleep:{}",
                       ready, active, static_cast<u32>(app_cpu_time),
                       curr_state == SchedState::APP ? "APP" : "SYS",
                       kernel.timing.GetTimer(1)->EventCount(tick_event), ids);
}

template <class Archive>
void CpuLimiterMulti::serialize(Archive& ar, const unsigned int) {
    ar & ready;
    ar & active;
    ar & app_cpu_time;
    ar & curr_state;
    std::vector<u32> v;
    if (Archive::is_loading::value) {
        ar & v;
        for (auto it : v) {
            sleeping_thread_ids.push(it);
        }
    } else {
        std::queue<u32> temp = sleeping_thread_ids;
        while (!temp.empty()) {
            v.push_back(temp.front());
            temp.pop();
        }
        ar & v;
    }
}

ThreadManager::ThreadManager(Kernel::KernelSystem& kernel, u32 core_id)
    : kernel(kernel), core_id(core_id), current_schedule_mode(Core1ScheduleMode::Multi),
      single_time_limiter(kernel), multi_time_limiter(kernel) {
    ThreadWakeupEventType = kernel.timing.RegisterEvent(
        "ThreadWakeupCallback_" + std::to_string(core_id),
        [this](u64 thread_id, s64 cycle_late) { ThreadWakeupCallback(thread_id, cycle_late); });
    if (core_id == 1) {
        single_time_limiter.Initialize(true);
        multi_time_limiter.Initialize(false);
    }
}

ThreadManager::~ThreadManager() {
    for (auto& t : thread_list) {
        t->Stop();
    }
}

std::span<const std::shared_ptr<Thread>> ThreadManager::GetThreadList() const {
    return thread_list;
}

std::shared_ptr<Thread> ThreadManager::GetThreadByID(u32 thread_id) const {
    for (auto& thread : thread_list) {
        if (thread->thread_id == thread_id) {
            return thread;
        }
    }
    return nullptr;
}

void ThreadManager::SetScheduleMode(Core1ScheduleMode mode) {
    GetCpuLimiter()->End();
    current_schedule_mode = mode;
    if (mode == Core1ScheduleMode::Single) {
        LOG_WARNING(Kernel, "Unimplemented \"Single\" schedule mode.");
    }
    GetCpuLimiter()->Start();
}

void ThreadManager::UpdateAppCpuLimit() {
    GetCpuLimiter()->UpdateAppCpuLimit();
}

std::shared_ptr<Thread> KernelSystem::GetThreadByID(u32 thread_id) const {
    for (u32 core_id = 0; core_id < Core::System::GetInstance().GetNumCores(); core_id++) {
        auto ret = GetThreadManager(core_id).GetThreadByID(thread_id);
        if (ret) {
            return ret;
        }
    }
    return nullptr;
}

} // namespace Kernel
