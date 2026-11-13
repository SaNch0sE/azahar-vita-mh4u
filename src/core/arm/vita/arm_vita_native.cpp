// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "core/arm/vita/arm_vita_native.h"
#include "core/arm/dyncom/arm_dyncom.h"
#include "core/arm/dyncom/arm_dyncom_trans.h"
#include "core/arm/skyeye_common/armstate.h"
#include "video_core/renderer_gxm/gxm_flags.h"
#include "core/arm/vita/native_stats.h"
#include "core/arm/vita/mh4u_vector_hle.h"
#include "common/emu_prof.h"

#include <algorithm>
#include <array>
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/core.h"
#include "video_core/gpu.h"
#include "core/core_timing.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/vm_manager.h"
#include "core/hle/kernel/thread.h"
#include "core/memory.h"

namespace Core {

namespace {

/// Never hand the guest a slice longer than this, so a runaway guest cannot stall the frontend.
constexpr u64 MaxQuantumNs = 16'000'000;
/// Nor one so short that the timer costs more than the work it bounds.
constexpr u64 MinQuantumNs = 250'000;

u64 TicksToNanoseconds(s64 ticks) {
    if (ticks <= 0) {
        return 0;
    }
    return static_cast<u64>(ticks) * 1'000'000'000ull / BASE_CLOCK_RATE_ARM11;
}

u64 NanosecondsToTicks(u64 nanoseconds) {
    return nanoseconds * BASE_CLOCK_RATE_ARM11 / 1'000'000'000ull;
}

/// Every guest page is mapped read, write and execute. The emulated process's own permissions
/// are not reported through the mapping observer; W^X per section is a later refinement.
constexpr u32 GuestPerm = AZAHAR_PERM_R | AZAHAR_PERM_W | AZAHAR_PERM_X;

/// The core the module takes. The emulation thread must not be pinned there, or the detach
/// leaves it with nowhere to run: it moves to core 0, where the rasterizer's first worker lives
/// and where it mostly waits inside azaharRun anyway.
constexpr int TakenCore = 2;

const char* ReasonName(u32 reason) {
    switch (reason) {
    case AZAHAR_EXIT_SVC:
        return "svc";
    case AZAHAR_EXIT_UND:
        return "undefined instruction";
    case AZAHAR_EXIT_PABT:
        return "prefetch abort";
    case AZAHAR_EXIT_DABT:
        return "data abort";
    case AZAHAR_EXIT_TIMER:
        return "preemption";
    default:
        return "unknown exit";
    }
}

} // Anonymous namespace

// ---------------------------------------------------------------------------- VitaGuestMap

VitaGuestMap::VitaGuestMap(Memory::MemorySystem& memory_) : memory{memory_} {}

VitaGuestMap::~VitaGuestMap() {
    if (installed) {
        azaharRelease();
    }
}

void VitaGuestMap::ProtectHostRange(const u8* host, std::size_t size, bool read_only) {
    if (!installed) {
        return;
    }
    const u32 perm = read_only ? (AZAHAR_PERM_R | AZAHAR_PERM_X) : GuestPerm;
    for (const auto& [base, mapping] : live_mappings) {
        const u8* lo = std::max<const u8*>(mapping.host, host);
        const u8* hi = std::min<const u8*>(mapping.host + mapping.size, host + size);
        if (lo >= hi) {
            continue;
        }
        Edit(AZAHAR_EDIT_PROTECT, 0, base + static_cast<u32>(lo - mapping.host),
             static_cast<u32>(hi - lo), perm);
    }
}

void VitaGuestMap::ProtectGuestPage(VAddr guest_page, bool read_only) {
    Edit(AZAHAR_EDIT_PROTECT, 0, guest_page, 4096, read_only ? (AZAHAR_PERM_R | AZAHAR_PERM_X) : GuestPerm);
}

const u8* VitaGuestMap::HostPageOf(VAddr guest) const {
    auto it = live_mappings.upper_bound(guest);
    if (it == live_mappings.begin()) {
        return nullptr;
    }
    --it;
    if (guest >= it->first + it->second.size) {
        return nullptr;
    }
    return it->second.host + ((guest - it->first) & ~VAddr{4095});
}

bool VitaGuestMap::Edit(u32 op, u32 user_va, u32 guest_va, u32 size, u32 perm) const {
    AzaharEditRequest req{};
    req.op = op;
    req.range = AzaharRange{user_va, guest_va, size, perm};
    const int ret = azaharEdit(&req);
    if (ret < 0) {
        LOG_ERROR(Core_ARM11, "azaharEdit op {} guest {:#010x}+{:#x} failed: {}", op, guest_va, size,
                  ret);
        return false;
    }
    return true;
}

void VitaGuestMap::OnMapped(VAddr base, u32 size, MemoryRef target) {
    if (base + size > GuestEnd || base + size < base) {
        return;
    }
    u8* host = target.GetPtr();
    if (host == nullptr) {
        OnUnmapped(base, size);
        return;
    }
    if ((reinterpret_cast<uintptr_t>(host) & 0xFFFu) != 0) {
        // The module maps whole pages; backing that does not start on one cannot be described.
        LOG_WARNING(Core_ARM11, "Guest {:#010x}+{:#x}: backing {} is not page aligned", base, size,
                    fmt::ptr(host));
        OnUnmapped(base, size);
        return;
    }

    const Mapping wanted{size, host};
    const auto existing = live_mappings.find(base);
    if (existing != live_mappings.end() && existing->second == wanted) {
        return;
    }

    // Whatever was there is replaced, on the guest side too when the table is live.
    ForgetRange(base, size, installed);
    live_mappings.emplace(base, wanted);
    if (installed) {
        Edit(AZAHAR_EDIT_MAP, reinterpret_cast<u32>(host), base, size, GuestPerm);
    }
}

void VitaGuestMap::OnUnmapped(VAddr base, u32 size) {
    if (base + size > GuestEnd || base + size < base) {
        return;
    }
    ForgetRange(base, size, installed);
}

void VitaGuestMap::ForgetRange(VAddr base, u32 size, bool unmap) {
    if (size == 0) {
        return;
    }
    const VAddr end = base + size;

    auto it = live_mappings.lower_bound(base);
    if (it != live_mappings.begin()) {
        const auto prev = std::prev(it);
        if (prev->first + prev->second.size > base) {
            it = prev;
        }
    }

    while (it != live_mappings.end() && it->first < end) {
        const VAddr entry_base = it->first;
        const Mapping entry = it->second;
        const VAddr entry_end = entry_base + entry.size;
        const VAddr overlap_base = std::max(entry_base, base);
        const VAddr overlap_end = std::min(entry_end, end);

        it = live_mappings.erase(it);

        if (unmap) {
            Edit(AZAHAR_EDIT_UNMAP, 0, overlap_base, overlap_end - overlap_base, 0);
        }

        if (entry_base < overlap_base) {
            live_mappings.emplace(entry_base, Mapping{overlap_base - entry_base, entry.host});
        }
        if (overlap_end < entry_end) {
            live_mappings.emplace(overlap_end, Mapping{entry_end - overlap_end,
                                                       entry.host + (overlap_end - entry_base)});
        }
    }
}

void VitaGuestMap::OnPageTableChanged() {
    mirrored = nullptr;
    if (installed) {
        for (const auto& [base, mapping] : live_mappings) {
            Edit(AZAHAR_EDIT_UNMAP, 0, base, mapping.size, 0);
        }
    }
    live_mappings.clear();
}

bool VitaGuestMap::Install() {
    if (installed) {
        return true;
    }
    AzaharMapRequest req{};
    req.count = 0;
    const int ret = azaharMap(&req);
    if (ret < 0) {
        LOG_ERROR(Core_ARM11, "azaharMap failed: {}", ret);
        return false;
    }
    installed = true;
    // Guest write tracking (core/guest_write_tracker.h) over AZAHAR_EDIT_PROTECT, armed between
    // slices. `nowritetrack` in gxm_flags.txt turns it off.
    {
        auto& tracker = memory.WriteTracker();
        tracker.SetProtect([this](const u8* host, std::size_t size, bool read_only) {
            ProtectHostRange(host, size, read_only);
        });
        tracker.SetDeferred(true);
        tracker.SetHotPages(!GxmRenderer::GxmFlag("nohotpages"));
        tracker.SetEnabled(!GxmRenderer::GxmFlag("nowritetrack"));
    }
    unsigned added = 0;
    for (const auto& [base, mapping] : live_mappings) {
        if (Edit(AZAHAR_EDIT_MAP, reinterpret_cast<u32>(mapping.host), base, mapping.size,
                 GuestPerm)) {
            added++;
        }
    }
    LOG_INFO(Core_ARM11, "Guest table installed on the taken core with {} range(s)", added);
    return true;
}

void VitaGuestMap::SyncCode(VAddr base, u32 size) {
    if (!installed || size == 0) {
        return;
    }
    const VAddr start = base & ~0xFFFu;
    const VAddr end = (base + size + 0xFFFu) & ~0xFFFu;
    auto it = live_mappings.lower_bound(start);
    if (it != live_mappings.begin()) {
        const auto prev = std::prev(it);
        if (prev->first + prev->second.size > start) {
            it = prev;
        }
    }
    for (; it != live_mappings.end() && it->first < end; ++it) {
        const VAddr lo = std::max(it->first, start);
        const VAddr hi = std::min(it->first + it->second.size, end);
        if (lo < hi) {
            Edit(AZAHAR_EDIT_SYNC_CODE, reinterpret_cast<u32>(it->second.host + (lo - it->first)),
                 lo, hi - lo, 0);
        }
    }
}

void VitaGuestMap::SyncAllCode() {
    if (!installed) {
        return;
    }
    for (const auto& [base, mapping] : live_mappings) {
        Edit(AZAHAR_EDIT_SYNC_CODE, reinterpret_cast<u32>(mapping.host), base, mapping.size, 0);
    }
}

// ---------------------------------------------------------------------------- ARM_VitaNative

std::shared_ptr<VitaGuestMap> ARM_VitaNative::CreateMap(Memory::MemorySystem& memory) {
    // Under the Vita3K emulator there is no kernel to take a core from, and an unresolved import
    // there returns zero rather than an error - which azaharTakeCore cannot tell from success. So
    // that target says so with a file instead, and the caller falls back to the interpreter.
    if (SceUID fd = sceIoOpen("ux0:data/azahar/cpu_interpreter", SCE_O_RDONLY, 0); fd >= 0) {
        sceIoClose(fd);
        LOG_WARNING(Core_ARM11, "ux0:data/azahar/cpu_interpreter present: not taking a core");
        return nullptr;
    }

    // The module holds a core only in its resident design, and a thread pinned to a held core
    // never runs again - so this thread steps aside before the take, and steps back if the
    // module answers that it folded the guest into its caller instead. Folded, the partition's
    // choice is the one that matters: the slice runs on this thread's core, so the emulation
    // thread belongs on the core the frontend gave it and the guest goes there with it.
    const SceUID self = sceKernelGetThreadId();
    const int affinity = sceKernelGetThreadCpuAffinityMask(self);
    if (affinity == (SCE_KERNEL_CPU_MASK_USER_0 << TakenCore) || affinity == 0) {
        sceKernelChangeThreadCpuAffinityMask(self, SCE_KERNEL_CPU_MASK_USER_0);
    }

    sceClibPrintf("[azahar] CreateMap: calling azaharTakeCore\n");
    const int ret = azaharTakeCore();
    sceClibPrintf("[azahar] CreateMap: azaharTakeCore -> %d\n", ret);
    if (ret < 0) {
        LOG_WARNING(Core_ARM11, "azaharTakeCore failed ({}): native execution unavailable", ret);
        return nullptr;
    }
    if (ret == AZAHAR_TAKE_FOLDED && affinity != 0) {
        const int back = sceKernelChangeThreadCpuAffinityMask(self, affinity);
        LOG_INFO(Core_ARM11, "folded native execution: emulation thread back on affinity {:#x} "
                             "({:#010x})", affinity, back);
    }
    auto map = std::make_shared<VitaGuestMap>(memory);
    memory.SetMappingObserver(map.get());
    return map;
}

ARM_VitaNative::ARM_VitaNative(Core::System& system_, Memory::MemorySystem& memory_, u32 id,
                               std::shared_ptr<Core::Timing::Timer> timer_,
                               std::shared_ptr<VitaGuestMap> map_)
    : ARM_Interface(id, timer_), system{system_}, memory{memory_}, svc_context{system_},
      map{std::move(map_)} {
    usable = map != nullptr;
    context.cpsr = 0x10;
}

ARM_VitaNative::~ARM_VitaNative() {
    if (map != nullptr && map.use_count() == 1) {
        memory.SetMappingObserver(nullptr);
    }
}

void ARM_VitaNative::Run() {
    if (break_flag) [[unlikely]] {
        return;
    }
    // The scheduler asks for a stop immediately before handing out a new slice; that request is
    // about the slice that just ended.
    stop_requested = false;

    const u64 quantum = std::clamp(TicksToNanoseconds(timer->GetDowncount()), MinQuantumNs,
                                   MaxQuantumNs);
    while (RunSlice(quantum)) {
        if (timer->GetDowncount() <= 0 || break_flag || stop_requested) {
            break;
        }
    }
}

void ARM_VitaNative::Step() {
    if (break_flag) [[unlikely]] {
        return;
    }
    stop_requested = false;
    RunSlice(MinQuantumNs);
}

bool ARM_VitaNative::RunSlice(u64 quantum_ns) {
    if (!usable) [[unlikely]] {
        system.SetStatus(System::ResultStatus::ErrorCoreExceptionRaised,
                         "Vita native backend is not usable");
        return false;
    }
    if (!map->IsInstalled() && !map->Install()) {
        system.SetStatus(System::ResultStatus::ErrorCoreExceptionRaised,
                         "Vita native backend could not install the guest table");
        usable = false;
        return false;
    }

    // A command list finished on the GX thread since the last slice: post its interrupts now.
    system.GPU().GxPoll();
    const SceUInt64 prologue_start = Common::EmuProf::enabled ? sceKernelGetProcessTimeWide() : 0;
    // Pages the surface cache wants armed since the last slice: protected now, while the
    // guest is stopped, so the claim and the protection take effect together at this entry.
    memory.WriteTracker().ApplyPending();
    if (Common::EmuProf::enabled) {
        Common::EmuProf::Add(" .slice apply pending", sceKernelGetProcessTimeWide() - prologue_start);
    }

    AzaharRunRequest run{};
    run.ctx = context;
    run.ctx.tpidruro = cp15[CP15_THREAD_URO];
    run.ctx.tpidrurw = cp15[CP15_THREAD_UPRW];
    run.quantum_us = static_cast<u32>(quantum_ns / 1000);
    if (fast_svc < 0) {
        std::FILE* off = std::fopen("ux0:data/azahar/nofastsvc", "rb");
        if (off != nullptr) {
            std::fclose(off);
        }
        fast_svc = (azaharVersion() >= 2 && off == nullptr) ? 1 : 0;
        LOG_INFO(Core_ARM11, "fast system calls in the plugin: {} (plugin version {})",
                 fast_svc ? "on" : "off", azaharVersion());
    }
    const double clock_scale = timer->GetClockScale();
    if (fast_svc > 0) {
        // svcGetThreadId: the running thread, and the other handles the guest has asked about,
        // resolved now - a handle closed and reused since is simply looked up again.
        Kernel::KernelSystem& kernel = system.Kernel();
        if (const auto thread = kernel.GetCurrentThreadManager().GetCurrentThread()) {
            run.fast_flags |= AZAHAR_FAST_THREAD_ID;
            run.cur_thread_id = thread->GetThreadId();
            if (const auto process = kernel.GetCurrentProcess()) {
                for (std::size_t i = 0; i < fast_handles.size(); i++) {
                    if (fast_handles[i] == 0) {
                        continue;
                    }
                    if (const auto t = process->handle_table.Get<Kernel::Thread>(fast_handles[i])) {
                        run.handle_ids[i][0] = fast_handles[i];
                        run.handle_ids[i][1] = t->GetThreadId();
                    }
                }
            }
        }
        // svcGetSystemTick: the plugin adds the time spent in this slice, at the rate CreditTime
        // will credit it, to the ticks at entry. Needs the timer's clock, known after one run.
        if (fast_timer_hz != 0 && run.quantum_us != 0) {
            run.fast_flags |= AZAHAR_FAST_SYSTEM_TICK;
            run.tick_ratio_q16 = static_cast<u32>(
                static_cast<double>(BASE_CLOCK_RATE_ARM11) * clock_scale * 65536.0 /
                fast_timer_hz);
            run.tick_base = timer->GetTicks();
            run.last_tick = fast_last_tick;
        }
        // The cycles the HLE charges these calls (svc.cpp's table), in guest ticks.
        run.charge_37 = static_cast<u32>(677 * clock_scale);
        run.charge_28 = static_cast<u32>(340 * clock_scale);
    }

    const SceUInt64 vanrun_start = sceKernelGetProcessTimeWide();
    if (Common::EmuProf::enabled) {
        Common::EmuProf::Add(" .slice prologue", vanrun_start - prologue_start);
    }
    const int ret = azaharRun(&run);
    const SceUInt64 vanrun_end = sceKernelGetProcessTimeWide();
    NativeStats::vanrun_us.fetch_add(vanrun_end - vanrun_start, std::memory_order_relaxed);
    if (Common::EmuProf::enabled) {
        Common::EmuProf::Add(" .slice guest (azaharRun)", vanrun_end - vanrun_start);
    }
    if (ret < 0) {
        system.SetStatus(System::ResultStatus::ErrorCoreExceptionRaised,
                         fmt::format("azaharRun failed: {}", ret).c_str());
        usable = false;
        return false;
    }

    context = run.ctx;
    cp15[CP15_THREAD_UPRW] = run.ctx.tpidrurw;
    NativeStats::slices.fetch_add(1, std::memory_order_relaxed);
    if (run.reason == AZAHAR_EXIT_SVC) {
        NativeStats::svcs.fetch_add(1, std::memory_order_relaxed);
    }
    CreditTime(run, quantum_ns);
    if (fast_svc > 0) {
        fast_timer_hz = run.timer_hz;
        if (run.fast37_count + run.fast28_count != 0) {
            NativeStats::fast_svcs.fetch_add(run.fast37_count + run.fast28_count,
                                             std::memory_order_relaxed);
            timer->AddTicks(u64{run.fast37_count} * 677 + u64{run.fast28_count} * 340);
        }
        if (run.fast28_count != 0) {
            fast_last_tick = run.last_tick;
            // Guest time never goes back: the slice's credit is rounded differently from the
            // plugin's estimate, so catch up to the last tick it handed out.
            const u64 now = timer->GetTicks();
            if (now < fast_last_tick) {
                timer->AddTicks(static_cast<u64>((fast_last_tick - now) / clock_scale) + 1);
            }
        }
        if (run.reason == AZAHAR_EXIT_SVC && run.svc_number == 0x37 &&
            run.ctx.r[1] != 0xFFFF8000u) {
            // A handle the plugin could not answer: remember it for the next slices.
            const u32 handle = run.ctx.r[1];
            if (std::find(fast_handles.begin(), fast_handles.end(), handle) ==
                fast_handles.end()) {
                std::move_backward(fast_handles.begin(), fast_handles.end() - 1,
                                   fast_handles.end());
                fast_handles[0] = handle;
            }
        }
    }

    if (Common::EmuProf::enabled) {
        Common::EmuProf::Add(" .slice epilogue", sceKernelGetProcessTimeWide() - vanrun_end);
    }
    switch (run.reason) {
    case AZAHAR_EXIT_SVC: {
        const SceUInt64 svc_start = sceKernelGetProcessTimeWide();
        std::string prof_key;
        // gxoverlap (with emuprof): how much of the GX command processing could overlap other
        // work if it ran on its own thread. For each TriggerCmdReqQueue: its duration, and the
        // wall time until the same guest thread next waits (WaitSynchronization1/N) - what the
        // emulation thread does in between could run while the GX work proceeds elsewhere.
        static const bool gx_overlap = GxmRenderer::GxmFlag("gxoverlap");
        u32 svc_thread_id = 0;
        bool is_gx_trigger = false;
        if (gx_overlap && Common::EmuProf::enabled) {
            if (const auto thread = system.Kernel().GetCurrentThreadManager().GetCurrentThread()) {
                svc_thread_id = thread->GetThreadId();
            }
            if (run.svc_number == 0x24 || run.svc_number == 0x25) {
                const auto it = gx_pending.find(svc_thread_id);
                if (it != gx_pending.end()) {
                    const u64 gap = svc_start - it->second.end_us;
                    Common::EmuProf::Add("gxoverlap: trigger time", it->second.dur_us);
                    Common::EmuProf::Add("gxoverlap: gap to wait", gap);
                    Common::EmuProf::Add("gxoverlap: hideable", std::min<u64>(gap, it->second.dur_us));
                    gx_pending.erase(it);
                }
            }
        }
        if (Common::EmuProf::enabled) {
            // IPC requests by service and command; everything else by number.
            prof_key = fmt::format("svc {:02X}", run.svc_number);
            if (run.svc_number == 0x32) {
                if (const auto process = system.Kernel().GetCurrentProcess()) {
                    if (const auto obj = process->handle_table.GetGeneric(run.ctx.r[0])) {
                        const u32 header = memory.Read32(cp15[CP15_THREAD_URO] + 0x80);
                        prof_key = fmt::format("ipc {} {:04X}", obj->GetName(), header >> 16);
                        is_gx_trigger = (header >> 16) == 0x000C &&
                                        obj->GetName().starts_with("gsp::Gpu");
                    }
                }
            }
        }
        svc_context.CallSVC(run.svc_number);
        const SceUInt64 svc_end = sceKernelGetProcessTimeWide();
        NativeStats::svc_us.fetch_add(svc_end - svc_start, std::memory_order_relaxed);
        if (!prof_key.empty()) {
            Common::EmuProf::Add(prof_key, svc_end - svc_start);
        }
        if (gx_overlap && is_gx_trigger) {
            auto& pending = gx_pending[svc_thread_id];
            if (pending.end_us != 0) {
                // Triggered again before waiting: what the thread did between the two (building
                // the next list) could have overlapped the earlier one's processing.
                const u64 gap = svc_start - pending.end_us;
                Common::EmuProf::Add("gxoverlap: trigger time", pending.dur_us);
                Common::EmuProf::Add("gxoverlap: gap to next trigger", gap);
                Common::EmuProf::Add("gxoverlap: hideable", std::min<u64>(gap, pending.dur_us));
            }
            pending = {svc_end, svc_end - svc_start};
        }
    }
        // An svc can reschedule, which replaces the register file wholesale.
        return !break_flag && !stop_requested;

    case AZAHAR_EXIT_TIMER:
        return false;

    case AZAHAR_EXIT_DABT: {
        // A store into a page the surface cache armed: mark it, make it writable again and
        // resume at the store (r15 is the faulting instruction). A fault on any other page
        // is the guest's own.
        auto& tracker = memory.WriteTracker();
        const u8* host = tracker.Enabled() ? map->HostPageOf(run.far) : nullptr;
        if (host != nullptr) {
            struct Unprotect {
                VitaGuestMap* map;
                u32 guest_page;
            } ctx{map.get(), run.far & ~u32{4095}};
            const bool ours = tracker.OnFault(
                host,
                [](void* raw) {
                    auto* u = static_cast<Unprotect*>(raw);
                    u->map->ProtectGuestPage(u->guest_page, false);
                },
                &ctx);
            if (ours) {
                NativeStats::write_faults.fetch_add(1, std::memory_order_relaxed);
                return !break_flag && !stop_requested;
            }
        }
        ReportFault(run);
        return false;
    }
    case AZAHAR_EXIT_UND: {
        const SceUInt64 und_start = Common::EmuProf::enabled ? sceKernelGetProcessTimeWide() : 0;
        const bool handled = InterpretShortVectors();
        if (Common::EmuProf::enabled) {
            Common::EmuProf::Add(" .slice VFP short vectors (interpreted)",
                                 sceKernelGetProcessTimeWide() - und_start);
        }
        if (handled) {
            return !break_flag && !stop_requested;
        }
    }
        ReportFault(run);
        return false;
    case AZAHAR_EXIT_PABT:
    default:
        ReportFault(run);
        return false;
    }
}

void ARM_VitaNative::CreditTime(const AzaharRunRequest& run, u64 quantum_ns) {
    // The core's private timer counted the slice down; what it has left is what the guest did not
    // use. Native execution has no instruction count, so guest time is real time.
    u64 elapsed_ns = quantum_ns;
    if (run.timer_hz != 0) {
        const u64 quantum_ticks = quantum_ns / 1000 * run.timer_hz / 1'000'000ull;
        const u64 spent = quantum_ticks > run.timer_left ? quantum_ticks - run.timer_left : 0;
        elapsed_ns = spent * 1'000'000'000ull / run.timer_hz;
    }
    NativeStats::credited_ns.fetch_add(elapsed_ns, std::memory_order_relaxed);
    const u64 ticks = NanosecondsToTicks(elapsed_ns);
    if (ticks > 0) {
        timer->AddTicks(ticks);
    }
}

void ARM_VitaNative::ReportFault(const AzaharRunRequest& run) {
    std::string registers;
    for (int i = 0; i < 16; i++) {
        registers += fmt::format("r{:02d} = {:08X}\n", i, context.r[i]);
    }
    const std::string message =
        fmt::format("{}Guest {} at PC {:08X} (cpsr {:08X}), fsr {:08X} far {:08X}", registers,
                    ReasonName(run.reason), context.r[15], context.cpsr, run.fsr, run.far);
    LOG_CRITICAL(Core_ARM11, "{}", message);
    std::string code;
    for (u32 a = context.r[15] - 16; a <= context.r[15] + 12; a += 4) {
        code += fmt::format("{}{:08X}", a == context.r[15] ? " *" : " ", memory.Read32(a));
    }
    const std::string writes = system.GPU().DescribeRecentWrites(std::span<const u32>(context.r),
                                                                 run.far);
    LOG_CRITICAL(Core_ARM11, "code around PC:{}\nrender-thread writes:\n{}", code, writes);
    // The console log is a ring that a burst of errors just before a fault can overrun, taking
    // the registers with it; the same report goes to a file that nothing else writes.
    if (FILE* f = std::fopen("ux0:data/azahar/fault.txt", "a")) {
        std::fprintf(f, "%s\nfpscr %08X tpidruro %08X tpidrurw %08X reason %u\ncode around PC:%s\n",
                     message.c_str(), context.fpscr, context.tpidruro, context.tpidrurw,
                     static_cast<unsigned>(run.reason), code.c_str());
        for (int i = 0; i < 32; i++) {
            std::fprintf(f, "d%02d = %08X%08X%s", i, context.fpregs[2 * i + 1],
                         context.fpregs[2 * i], (i % 4 == 3) ? "\n" : "  ");
        }
        std::fprintf(f, "render-thread writes:\n%s\n----\n", writes.c_str());
        std::fclose(f);
    }
    system.SetStatus(System::ResultStatus::ErrorCoreExceptionRaised, message.c_str());
}

bool ARM_VitaNative::InterpretShortVectors() {
    // FPSCR.LEN (bits 18:16) and STRIDE (bits 21:20). The ARM11's VFPv2 runs data-processing
    // instructions over register banks while either is set; the Cortex-A9 has no short vectors and
    // takes VFP instructions as undefined instead (Monster Hunter 4 Ultimate sets LEN=3 for its
    // matrix code when gameplay starts). The guest normally clears them again at the end of the
    // block, so the interpreter runs exactly that block and native execution resumes after it.
    constexpr u32 ShortVectorMask = 0x00370000;
    if ((context.fpscr & ShortVectorMask) == 0) {
        return false;
    }
    // MH4U's audio filter, the one routine that runs in vector mode (mh4u_vector_hle.h): native.
    // `nosvhle` interprets it as before; `svcheck` runs both and logs any difference.
    static const bool sv_hle = !GxmRenderer::GxmFlag("nosvhle");
    static const bool sv_check = GxmRenderer::GxmFlag("svcheck");
    const bool hle_here = sv_hle && context.r[15] == VitaHle::Mh4uFilterEntry &&
                          (context.fpscr & ShortVectorMask) == 0x00030000;
    if (hle_here && !sv_check) {
        struct Direct {
            Memory::MemorySystem& m;
            u32 Read(u32 a) {
                return m.Read32(a);
            }
            void Write(u32 a, u32 v) {
                m.Write32(a, v);
            }
            u8* Span(u32 a, u32 size) {
                if ((a & 3) != 0 || (a & 0xfff) + size > 0x1000) {
                    return nullptr;
                }
                return m.GetPointer(a);
            }
        } direct{memory};
        VitaHle::RunMh4uFilter({context.r, context.fpregs, &context.cpsr, &context.fpscr}, direct);
        static u32 hle_calls = 0;
        if (++hle_calls == 1) {
            LOG_INFO(Core_ARM11, "MH4U vector routine at {:08X}: native (HLE)", VitaHle::Mh4uFilterEntry);
        }
        return true;
    }
    // svcheck: the HLE on copies, writes into a shadow; compared after the interpreter below.
    struct Shadow {
        Memory::MemorySystem& m;
        std::vector<std::pair<u32, u32>> writes;
        u32 Read(u32 a) {
            for (auto it = writes.rbegin(); it != writes.rend(); ++it) {
                if (it->first == a) {
                    return it->second;
                }
            }
            return m.Read32(a);
        }
        void Write(u32 a, u32 v) {
            writes.emplace_back(a, v);
        }
        u8* Span(u32, u32) {
            return nullptr;
        }
    } shadow{memory, {}};
    std::array<u32, 16> chk_r{};
    std::array<u32, 64> chk_s{};
    u32 chk_cpsr = 0, chk_fpscr = 0;
    if (hle_here) {
        std::copy(std::begin(context.r), std::end(context.r), chk_r.begin());
        std::copy(std::begin(context.fpregs), std::end(context.fpregs), chk_s.begin());
        chk_cpsr = context.cpsr;
        chk_fpscr = context.fpscr;
        VitaHle::RunMh4uFilter({chk_r.data(), chk_s.data(), &chk_cpsr, &chk_fpscr}, shadow);
    }
    if (!vector_interpreter) {
        // The interpreter's translation cache is 32 MB by default, enough to push a title that
        // already holds most of the heap (Monster Hunter 4 Ultimate, ~210 MB of 240) into
        // std::bad_alloc. A short-vector routine needs a few KB of it.
        if (trans_cache_buf == nullptr) {
            trans_cache_size = 4 * 1024 * 1024;
        }
        // No timer: the native slice this block belongs to has already credited the time.
        vector_interpreter = std::make_unique<ARM_DynCom>(system, memory, USER32MODE, GetID(), nullptr);
        LOG_INFO(Core_ARM11, "VFP short vectors at PC {:08X} (fpscr {:08X}): interpreting such blocks",
                 context.r[15], context.fpscr);
    }
    auto& interp = *vector_interpreter;
    // A full cache asserts, and it may only be cleared between blocks; half is the margin for
    // the one about to run.
    if (trans_cache_buf_top > trans_cache_size / 2) {
        interp.ClearInstructionCache();
    }
    // SetPageTable empties the interpreter's translation cache: once per table, not per block.
    // Per block, Monster Hunter 4 Ultimate's 1525-instruction routine was decoded afresh every
    // time it ran (~60 times a second), ~1.8 us an instruction.
    if (vector_interpreter_table != current_page_table.get()) {
        interp.SetPageTable(current_page_table);
        vector_interpreter_table = current_page_table.get();
    }
    ThreadContext ctx{};
    SaveContext(ctx);
    interp.LoadContext(ctx);
    interp.SetCP15Register(CP15_THREAD_URO, cp15[CP15_THREAD_URO]);
    interp.SetCP15Register(CP15_THREAD_UPRW, cp15[CP15_THREAD_UPRW]);

    // Stop before an svc, which must be serviced by this core as usual: the interpreter would take
    // it against whichever core the kernel thinks is running. A block left with LEN still set
    // simply traps again at its next VFP instruction.
    // In chunks: the interpreter stops by itself before an svc (RunUntilSvc), and running a
    // few instructions past the end of the vector block is still correct, only not native -
    // cheaper than one Step() per instruction with an svc check reading memory before each.
    constexpr u32 MaxSteps = 1'000'000;
    constexpr u32 Chunk = 128;
    u32 steps = 0;
    if (hle_here) {
        // Check mode: one instruction at a time up to the routine's exit, then compare.
        while (steps < MaxSteps && interp.GetPC() != VitaHle::Mh4uFilterExit) {
            bool at_svc = false;
            steps += interp.RunUntilSvc(1, at_svc);
            if (at_svc) {
                break;
            }
        }
        u32 bad = 0;
        std::string what;
        for (u32 i = 0; i < 16; i++) {
            if (interp.GetReg(i) != chk_r[i]) {
                bad++;
                what += fmt::format(" r{} {:08X}/{:08X}", i, interp.GetReg(i), chk_r[i]);
            }
        }
        for (u32 i = 0; i < 32; i++) {
            const u32 iv = interp.GetVFPReg(i);
            if (iv != chk_s[i]) {
                bad++;
                what += fmt::format(" s{} {:08X}/{:08X}", i, iv, chk_s[i]);
            }
        }
        if ((interp.GetCPSR() & 0xf0000000u) != (chk_cpsr & 0xf0000000u)) {
            bad++;
            what += fmt::format(" nzcv {:08X}/{:08X}", interp.GetCPSR(), chk_cpsr);
        }
        if (interp.GetVFPSystemReg(VFP_FPSCR) != chk_fpscr) {
            bad++;
            what += fmt::format(" fpscr {:08X}/{:08X}", interp.GetVFPSystemReg(VFP_FPSCR), chk_fpscr);
        }
        u32 bad_mem = 0;
        for (const auto& [a, v] : shadow.writes) {
            if (memory.Read32(a) != v && shadow.Read(a) == v) {
                bad_mem++;
            }
        }
        static u32 checks = 0, failures = 0;
        checks++;
        if (bad != 0 || bad_mem != 0) {
            failures++;
            if (failures <= 8) {
                LOG_ERROR(Core_ARM11, "svcheck: HLE differs ({} regs, {} of {} words):{}", bad,
                          bad_mem, shadow.writes.size(), what.substr(0, 400));
            }
        }
        if ((checks & (checks - 1)) == 0) {
            LOG_INFO(Core_ARM11, "svcheck: {} calls compared, {} differed ({} steps, {} writes)",
                     checks, failures, steps, shadow.writes.size());
        }
    }
    while (steps < MaxSteps && (interp.GetVFPSystemReg(VFP_FPSCR) & ShortVectorMask) != 0) {
        bool at_svc = false;
        steps += interp.RunUntilSvc(Chunk, at_svc);
        if (at_svc) {
            break;
        }
    }
    if (steps == 0) {
        return false;
    }
    interp.SaveContext(ctx);
    LoadContext(ctx);
    cp15[CP15_THREAD_UPRW] = interp.GetCP15Register(CP15_THREAD_UPRW);

    static u32 blocks = 0;
    static u64 total_steps = 0;
    blocks++;
    total_steps += steps;
    if (blocks <= 4 || (blocks & (blocks - 1)) == 0) {
        LOG_INFO(Core_ARM11, "short-vector block {}: {} instructions, now at {:08X} (fpscr {:08X}); "
                             "{} instructions interpreted in all",
                 blocks, steps, context.r[15], context.fpscr, total_steps);
    }
    return true;
}

void ARM_VitaNative::ClearInstructionCache() {
    if (vector_interpreter) {
        vector_interpreter->ClearInstructionCache();
    }
    if (map) {
        map->SyncAllCode();
    }
}

void ARM_VitaNative::InvalidateCacheRange(u32 start_address, std::size_t length) {
    if (vector_interpreter) {
        vector_interpreter->InvalidateCacheRange(start_address, length);
    }
    if (!map) {
        return;
    }
    // svcInvalidateInstructionCacheRange, which some titles issue every frame over a page
    // of their data. A sync is a kernel round trip that cleans the page and drops core 2's
    // icache, and it is only owed where something can execute: the process's own view of
    // the range says whether it can.
    if (const auto process = system.Kernel().GetCurrentProcess()) {
        const auto& vm = process->vm_manager;
        bool executable = false;
        const VAddr end = start_address + static_cast<VAddr>(length);
        for (VAddr at = start_address; at < end;) {
            const auto vma = vm.FindVMA(at);
            if (vma == vm.vma_map.end()) {
                break;
            }
            if ((static_cast<u8>(vma->second.permissions) &
                 static_cast<u8>(Kernel::VMAPermission::Execute)) != 0) {
                executable = true;
                break;
            }
            at = vma->second.base + vma->second.size;
        }
        if (!executable) {
            static u32 shown = 0;
            if (shown++ < 4) {
                LOG_INFO(Core_ARM11, "icache invalidate of {:08X}+{:X} skipped: nothing executes there",
                         start_address, length);
            }
            return;
        }
    }
    map->SyncCode(start_address, static_cast<u32>(length));
}

void ARM_VitaNative::ClearExclusiveState() {
    // Nothing to do here, but not for the reason this used to give. Whether an exception return
    // clears the local monitor is IMPLEMENTATION DEFINED on ARMv7-A, so entry.S now executes
    // CLREX explicitly before every guest entry - which covers this and every other path back
    // into the guest, including the ones that never call through here.
}

void ARM_VitaNative::SetPageTable(const std::shared_ptr<Memory::PageTable>& page_table) {
    current_page_table = page_table;
    // The one call that reaches every core: the kernel updates the memory system's current
    // table only for the core it considers running, so a table given to any other core would
    // never reach the mapping observer at all - and the guest table would install empty.
    if (map && page_table && map->MirroredTable() != page_table.get()) {
        memory.SyncMappingObserver(*page_table);
        map->SetMirroredTable(page_table.get());
    }
}

std::shared_ptr<Memory::PageTable> ARM_VitaNative::GetPageTable() const {
    return current_page_table;
}

void ARM_VitaNative::SetPC(u32 pc) {
    context.r[15] = pc & ~1u;
}

u32 ARM_VitaNative::GetPC() const {
    return context.r[15] & ~1u;
}

u32 ARM_VitaNative::GetReg(int index) const {
    ASSERT(index >= 0 && index < 16);
    return index == 15 ? GetPC() : context.r[index];
}

void ARM_VitaNative::SetReg(int index, u32 value) {
    ASSERT(index >= 0 && index < 16);
    if (index == 15) {
        SetPC(value);
    } else {
        context.r[index] = value;
    }
}

u32 ARM_VitaNative::GetVFPReg(int index) const {
    ASSERT(index >= 0 && index < 64);
    return context.fpregs[index];
}

void ARM_VitaNative::SetVFPReg(int index, u32 value) {
    ASSERT(index >= 0 && index < 64);
    context.fpregs[index] = value;
}

u32 ARM_VitaNative::GetVFPSystemReg(VFPSystemRegister reg) const {
    switch (reg) {
    case VFP_FPSCR:
        return context.fpscr;
    case VFP_FPEXC:
        return fpexc;
    default:
        UNREACHABLE_MSG("Unknown VFP system register: {}", reg);
    }
}

void ARM_VitaNative::SetVFPSystemReg(VFPSystemRegister reg, u32 value) {
    switch (reg) {
    case VFP_FPSCR:
        context.fpscr = value;
        return;
    case VFP_FPEXC:
        fpexc = value;
        return;
    default:
        UNREACHABLE_MSG("Unknown VFP system register: {}", reg);
    }
}

u32 ARM_VitaNative::GetCPSR() const {
    return context.cpsr;
}

void ARM_VitaNative::SetCPSR(u32 cpsr) {
    context.cpsr = cpsr;
}

u32 ARM_VitaNative::GetCP15Register(CP15Register reg) const {
    return cp15[reg];
}

void ARM_VitaNative::SetCP15Register(CP15Register reg, u32 value) {
    cp15[reg] = value;
}

void ARM_VitaNative::SaveContext(ThreadContext& ctx) {
    std::memcpy(ctx.cpu_registers.data(), context.r, sizeof(context.r));
    ctx.cpu_registers[15] = GetPC();
    ctx.cpsr = context.cpsr;
    std::memcpy(ctx.fpu_registers.data(), context.fpregs, sizeof(context.fpregs));
    ctx.fpscr = context.fpscr;
    ctx.fpexc = fpexc;
}

void ARM_VitaNative::LoadContext(const ThreadContext& ctx) {
    std::memcpy(context.r, ctx.cpu_registers.data(), sizeof(context.r));
    context.r[15] = ctx.cpu_registers[15] & ~1u;
    context.cpsr = ctx.cpsr;
    std::memcpy(context.fpregs, ctx.fpu_registers.data(), sizeof(context.fpregs));
    context.fpscr = ctx.fpscr;
    fpexc = ctx.fpexc;
}

void ARM_VitaNative::PrepareReschedule() {
    // Called from the HLE side while the guest is stopped at an exit, or from another host thread
    // while it runs. The former ends the slice loop; the latter is bounded by the quantum.
    stop_requested = true;
}

} // namespace Core
