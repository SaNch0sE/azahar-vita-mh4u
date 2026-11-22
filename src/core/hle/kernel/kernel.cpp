// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <boost/serialization/shared_ptr.hpp>
#include <boost/serialization/unordered_map.hpp>
#include <boost/serialization/vector.hpp>
#include "common/archives.h"
#include "common/serialization/atomic.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/hle/kernel/client_port.h"
#include "core/hle/kernel/config_mem.h"
#include "core/hle/kernel/handle_table.h"
#include "core/hle/kernel/ipc_debugger/recorder.h"
#include "core/hle/kernel/address_arbiter.h"
#include "core/hle/kernel/event.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/memory.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/resource_limit.h"
#include "core/hle/kernel/shared_page.h"
#include "core/hle/kernel/thread.h"
#include "core/hle/kernel/timer.h"

SERIALIZE_EXPORT_IMPL(Kernel::New3dsHwCapabilities)

namespace Kernel {

/// Initialize the kernel
KernelSystem::KernelSystem(Memory::MemorySystem& memory, Core::Timing& timing,
                           std::function<void()> prepare_reschedule_callback,
                           MemoryMode memory_mode, u32 num_cores, u64 override_init_time)
    : memory(memory), timing(timing),
      prepare_reschedule_callback(std::move(prepare_reschedule_callback)),
      memory_mode(memory_mode) {
    std::generate(memory_regions.begin(), memory_regions.end(),
                  [] { return std::make_shared<MemoryRegionInfo>(); });
    MemoryInit(memory_mode, override_init_time);

    resource_limits = std::make_unique<ResourceLimitList>(*this);
    for (u32 core_id = 0; core_id < num_cores; ++core_id) {
        thread_managers.push_back(std::make_unique<ThreadManager>(*this, core_id));
    }
    timer_manager = std::make_unique<TimerManager>(timing);
    ipc_recorder = std::make_unique<IPCDebugger::Recorder>();
    stored_processes.assign(num_cores, nullptr);
    for (auto& inbox : host_inboxes) {
        inbox = std::make_unique<HostInbox>();
    }

    next_thread_id = 1;
}

/// Shutdown the kernel
KernelSystem::~KernelSystem() {
    ResetThreadIDs();
};

ResourceLimitList& KernelSystem::ResourceLimit() {
    return *resource_limits;
}

const ResourceLimitList& KernelSystem::ResourceLimit() const {
    return *resource_limits;
}

u32 KernelSystem::GenerateObjectID() {
    return next_object_id++;
}

const std::shared_ptr<Process>& KernelSystem::GetCurrentProcess() const {
    return current_process[Core::ThisHost()];
}

void KernelSystem::SetCurrentProcess(const std::shared_ptr<Process>& process) {
    // This runs on every core switch, almost always with the process (and therefore the page
    // table) already in force; the reference-count and page-table churn was measurable, so
    // skip the redundant work.
    auto& current = current_process[Core::ThisHost()];
    if (current != process) {
        current = process;
    }
    SetCurrentMemoryPageTable(process->vm_manager.page_table);
}

void KernelSystem::SetCurrentProcessForCPU(const std::shared_ptr<Process>& process, u32 core_id) {
    const u32 host = Core::ThisHost();
    if (current_cpu[host]->GetID() == core_id) {
        if (current_process[host] != process) {
            current_process[host] = process;
        }
        SetCurrentMemoryPageTable(process->vm_manager.page_table);
    } else {
        if (stored_processes[core_id] != process) {
            stored_processes[core_id] = process;
        }
        thread_managers[core_id]->cpu->SetPageTable(process->vm_manager.page_table);
    }
}

void KernelSystem::SetCurrentMemoryPageTable(const std::shared_ptr<Memory::PageTable>& page_table) {
    memory.SetCurrentPageTable(page_table);
    if (Core::ARM_Interface* const cpu = CurrentCPU(); cpu != nullptr) {
        cpu->SetPageTable(page_table);
    }
}

void KernelSystem::SetCPUs(std::vector<std::shared_ptr<Core::ARM_Interface>> cpus) {
    ASSERT(cpus.size() == thread_managers.size());
    for (u32 i = 0; i < cpus.size(); i++) {
        thread_managers[i]->SetCPU(*cpus[i]);
    }
}

void KernelSystem::SetRunningCPU(Core::ARM_Interface* cpu) {
    const u32 host = Core::ThisHost();
    Core::ARM_Interface*& current = current_cpu[host];
    if (current == cpu) {
        timing.SetCurrentTimer(cpu->GetID());
        return;
    }
    const auto& process = current_process[host];
    if (current != nullptr && process && stored_processes[current->GetID()] != process) {
        stored_processes[current->GetID()] = process;
    }
    current = cpu;
    timing.SetCurrentTimer(cpu->GetID());
    if (stored_processes[cpu->GetID()]) {
        SetCurrentProcess(stored_processes[cpu->GetID()]);
    }
}

void KernelSystem::SendToHost(u32 host, HostMessage&& message) {
    host_inboxes[host]->Push(std::move(message));
}

void KernelSystem::ProcessHostMessages() {
    ProcessHostMessages(Core::ThisHost());
}

void KernelSystem::ProcessHostMessages(u32 host) {
    DEBUG_ASSERT(host == Core::ThisHost() || !Core::multi_host.load(std::memory_order_relaxed));
    HostInbox& inbox = *host_inboxes[host];
    if (inbox.Empty()) {
        return;
    }
    std::unique_lock<Common::RecursiveMutex> lock{hle_lock, std::defer_lock};
    if (Core::ThisHost() == 0) {
        lock.lock();
    }
    HostMessage message;
    while (inbox.Pop(message)) {
        switch (message.kind) {
        case HostMessage::Kind::EventSignal:
            static_cast<Event&>(*message.object).ReceiveSignal(message.seq, message.token);
            break;
        case HostMessage::Kind::Handoff:
            static_cast<Thread&>(*message.object).ResumeHandedOff();
            break;
        case HostMessage::Kind::ArbiterSignal:
            static_cast<AddressArbiter&>(*message.object)
                .ReceiveSignal(message.seq, message.count);
            break;
        case HostMessage::Kind::NewThread: {
            auto thread = std::static_pointer_cast<Thread>(message.object);
            GetThreadManager(thread->core_id).AdoptThread(std::move(thread));
            break;
        }
        case HostMessage::Kind::Requeue:
            static_cast<Thread&>(*message.object).Requeue();
            PrepareReschedule();
            break;
        case HostMessage::Kind::Core1Limit:
            if (message.token) {
                GetThreadManager(1).SetScheduleMode(static_cast<Core1ScheduleMode>(message.seq));
            } else {
                GetThreadManager(1).UpdateAppCpuLimit();
            }
            break;
        }
        message.object.reset();
    }
}

ThreadManager& KernelSystem::GetThreadManager(u32 core_id) {
    return *thread_managers[core_id];
}

const ThreadManager& KernelSystem::GetThreadManager(u32 core_id) const {
    return *thread_managers[core_id];
}

ThreadManager& KernelSystem::GetCurrentThreadManager() {
    return *thread_managers[CurrentCPU()->GetID()];
}

const ThreadManager& KernelSystem::GetCurrentThreadManager() const {
    return *thread_managers[CurrentCPU()->GetID()];
}

TimerManager& KernelSystem::GetTimerManager() {
    return *timer_manager;
}

const TimerManager& KernelSystem::GetTimerManager() const {
    return *timer_manager;
}

SharedPage::Handler& KernelSystem::GetSharedPageHandler() {
    return *shared_page_handler;
}

const SharedPage::Handler& KernelSystem::GetSharedPageHandler() const {
    return *shared_page_handler;
}

ConfigMem::Handler& KernelSystem::GetConfigMemHandler() {
    return *config_mem_handler;
}

IPCDebugger::Recorder& KernelSystem::GetIPCRecorder() {
    return *ipc_recorder;
}

const IPCDebugger::Recorder& KernelSystem::GetIPCRecorder() const {
    return *ipc_recorder;
}

std::unique_ptr<IPCDebugger::Recorder> KernelSystem::BackupIPCRecorder() {
    return std::move(ipc_recorder);
}

void KernelSystem::RestoreIPCRecorder(std::unique_ptr<IPCDebugger::Recorder> recorder) {
    ipc_recorder = std::move(recorder);
}

void KernelSystem::AddNamedPort(std::string name, std::shared_ptr<ClientPort> port) {
    named_ports.emplace(std::move(name), std::move(port));
}

u32 KernelSystem::NewThreadId() {
    return next_thread_id++;
}

void KernelSystem::ResetThreadIDs() {
    next_thread_id = 0;
}

void KernelSystem::UpdateCPUAndMemoryState(u64 title_id, MemoryMode memory_mode,
                                           New3dsHwCapabilities n3ds_hw_cap) {
    u32 tid_high = static_cast<u32>(title_id >> 32);

    constexpr u32 TID_HIGH_APPLET = 0x00040030;
    constexpr u32 TID_HIGH_SYSMODULE = 0x00040130;

    // PM only updates the reported memory for normal applications.
    // TODO(PabloMK7): Using the title ID is not correct, but close enough.
    if (tid_high != TID_HIGH_APPLET && tid_high != TID_HIGH_SYSMODULE) {
        UpdateReportedMemory(memory_mode, n3ds_hw_cap.memory_mode);
    }
    // A New 3DS title's 804 MHz clock, as APT reports it (GetApplicationRunningMode); on the Vita
    // the guest clock counts three A9 cycles a tick then (GuestTickHz).
    SetRunning804MHz(Core::System::GetInstance().IsNew3ds() && n3ds_hw_cap.enable_804MHz_cpu);
}

void KernelSystem::RestoreMemoryState(u64 title_id) {
    u32 tid_high = static_cast<u32>(title_id >> 32);

    constexpr u32 TID_HIGH_APPLET = 0x00040030;
    constexpr u32 TID_HIGH_SYSMODULE = 0x00040130;

    // PM only updates the reported memory for normal applications.
    // TODO(PabloMK7): Using the title ID is not correct, but close enough.
    if (tid_high != TID_HIGH_APPLET && tid_high != TID_HIGH_SYSMODULE) {
        RestoreReportedMemory();
    }
}

// Core 1's limiter (its tick on core 1's timer, the threads it put to sleep) is the host's that
// runs core 1: a change made on the other host (a service's, on the first emulation thread) goes
// there as a message. Made here, its unschedule left the tick on core 1's timer and scheduled a
// second one, and its wakeups went through the list the other host was adding to.
void KernelSystem::SetCore1ScheduleMode(Core1ScheduleMode mode) {
    if (Core::HostOfCore(1) != Core::ThisHost()) {
        SendToHost(Core::HostOfCore(1), HostMessage{HostMessage::Kind::Core1Limit, true,
                                                    static_cast<u32>(mode), nullptr});
        return;
    }
    GetThreadManager(1).SetScheduleMode(mode);
}

void KernelSystem::UpdateCore1AppCpuLimit() {
    if (Core::HostOfCore(1) != Core::ThisHost()) {
        SendToHost(Core::HostOfCore(1),
                   HostMessage{HostMessage::Kind::Core1Limit, false, 0, nullptr});
        return;
    }
    GetThreadManager(1).UpdateAppCpuLimit();
}

template <class Archive>
void KernelSystem::serialize(Archive& ar, const unsigned int) {
    ar & memory_regions;
    ar & named_ports;
    // current_cpu set externally
    // NB: subsystem references and prepare_reschedule_callback are constant
    ar&* resource_limits.get();
    ar & next_object_id;
    ar&* timer_manager.get();
    ar & next_process_id;
    ar & process_list;
    ar & current_process[0];
    // NB: core count checked in 'core'
    for (auto& thread_manager : thread_managers) {
        ar&* thread_manager.get();
    }
    ar & config_mem_handler;
    ar & shared_page_handler;
    ar & stored_processes;
    ar & next_thread_id;
    ar & memory_mode;
    ar & running_804MHz;
    ar & main_thread_extended_sleep;
    // Deliberately don't include debugger info to allow debugging through loads

    if (Archive::is_loading::value) {
        for (auto& memory_region : memory_regions) {
            memory_region->Unlock();
        }
        for (auto& process : process_list) {
            process->vm_manager.Unlock();
        }
    }
}
SERIALIZE_IMPL(KernelSystem)

template <class Archive>
void New3dsHwCapabilities::serialize(Archive& ar, const unsigned int) {
    ar & enable_l2_cache;
    ar & enable_804MHz_cpu;
    ar & memory_mode;
}
SERIALIZE_IMPL(New3dsHwCapabilities)

template <class Archive>
void Core1CpuTime::serialize(Archive& ar, const unsigned int) {
    ar & raw;
}
SERIALIZE_IMPL(Core1CpuTime)

} // namespace Kernel
