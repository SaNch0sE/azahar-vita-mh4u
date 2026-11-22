// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "azahar_native.h"
#include "core/arm/arm_interface.h"
#include "core/hle/kernel/svc.h"
#include "core/memory.h"

namespace Core {

class System;

/**
 * Keeps the guest's translation table matching the emulated process's page table.
 *
 * The PS Vita build runs guest code on the emulation threads' own cores, one slice at a time,
 * under a translation table the kernel module authors (vita_native/src/native.c). The guest
 * therefore sees memory at its own addresses, but those addresses are not this process's: what
 * this process reaches through a MemoryRef pointer, the guest reaches through a descriptor the
 * module writes for the same physical pages. This observer turns each mapping change of the
 * emulated process into a azaharMap / azaharEdit call.
 *
 * Until the table is installed (the first Run), mappings are only recorded; Install() then
 * installs an empty table and adds them one by one.
 */
class VitaGuestMap final : public Memory::MappingObserver {
public:
    explicit VitaGuestMap(Memory::MemorySystem& memory);
    ~VitaGuestMap() override;

    void OnMapped(VAddr base, u32 size, MemoryRef target) override;
    void OnUnmapped(VAddr base, u32 size) override;
    void OnPageTableChanged() override;

    /// Installs the table on the taken core with everything recorded so far. Idempotent.
    bool Install();

    /// Which PageTable the recorded mappings mirror. Cleared on OnPageTableChanged; the backend
    /// compares against it to re-sync only when the kernel actually hands a core a new table.
    const void* MirroredTable() const {
        return mirrored;
    }
    void SetMirroredTable(const void* table) {
        mirrored = table;
    }
    bool IsInstalled() const {
        return installed;
    }

    /// The app wrote instructions into [base, base+size) of guest memory through its own
    /// pointers: clean them out to memory and have the taken core drop its instruction cache.
    void SyncCode(VAddr base, u32 size);
    void SyncAllCode();

    /// Highest guest address the module's table can hold, exclusive.
    static constexpr u32 GuestEnd = 0x40000000;

    std::string Describe(VAddr addr) const;
    const u8* HostAt(VAddr addr) const;

    /// App memory mapped into the guest's table read and execute beside the process's mappings (the
    /// thunks, below the process image). Kept across page table changes.
    void MapExtra(VAddr base, u32 size, u8* host);

private:
    struct Mapping {
        u32 size;
        u8* host;
        bool operator==(const Mapping&) const = default;
    };

    /// Drops [base, base+size) from the bookkeeping, trimming or splitting entries that reach
    /// into it, so the map only ever describes disjoint ranges. With unmap set the guest side is
    /// cleared too.
    void ForgetRange(VAddr base, u32 size, bool unmap);
    bool Edit(u32 op, u32 user_va, u32 guest_va, u32 size, u32 perm) const;
    /// The permission a mapping of host is made with: guest VRAM, uncached in CDRAM, takes the app's
    /// own memory attributes.
    u32 MapPerm(const u8* host);

    Memory::MemorySystem& memory;
    bool installed{};
    const void* mirrored{};
    /// Guest ranges currently mapped (or recorded for the install), keyed by guest base, disjoint.
    std::map<VAddr, Mapping> live_mappings;
    VAddr extra_base{};
    Mapping extra{};
};

/**
 * Runs the guest's ARM code natively on a PS Vita core taken away from the Vita's kernel.
 *
 * The counterpart of ARM_Native (the Linux backend) with every host mechanism replaced by a call
 * into the azahar-native kernel module: the guest is entered with azaharRun, which returns when the guest
 * takes an svc, an undefined instruction, a prefetch or data abort, or when its quantum on the
 * core's private timer expires. There are no signals and no address-space mirror; the module
 * owns the core's translation table (VitaGuestMap keeps it in step) and the guest runs at PL0
 * under vectors the module installed. See DEVELOPMENT.md.
 *
 * The svcs the module can answer from what each slice hands it - GetSystemTick, the cache
 * svcs on the current process, the current thread's and process's IDs (AZAHAR_FAST_*) - never
 * leave the guest: its vector answers them and the slice runs on.
 *
 * The 3DS's core 0 runs on the emulation thread, its other cores on the second emulation thread
 * (Core::System::SecondHostMain), each in the guest's world on a Vita core of its own.
 */
#ifdef VITA_DIAGNOSTICS
/// The native core's slices since the last TakeVitaNativeCensus (sgxd_flags `perflog`).
struct VitaNativeCensus {
    u32 slices{};
    std::array<u32, AZAHAR_EXIT_TIMER + 1> reasons{}; ///< By AZAHAR_EXIT_*.
    std::array<u32, 256> svcs{};                      ///< The svc exits, by immediate.
    std::array<u32, AZAHAR_FAST_KINDS> fast{};        ///< By AZAHAR_FAST_*.
    u64 guest_ticks{}; ///< The slices' time in the guest, on the guest clock.
    u64 kernel_us{};   ///< azaharRun's own time, the guest's included.
    /// VFP instructions run under short vectors, the exits that ran them, and the sites patched to a
    /// thunk or to the module's svc.
    u32 vfp_insns{}, vfp_exits{}, vfp_thunks{}, vfp_svcs{};
    u32 svc_thunks{};
    u32 upcall_sites{}, upcall_runs{}, upcall_preempted{};
    /// Upcalls that asked for a reschedule, and those that answered busy (none since the generic
    /// upcall).
    u32 upcall_reschedules{}, upcall_busy{};
    u32 upcall_syscalls{};
    /// In-kernel scheduling: the slices that carried the thread table, and the module's
    /// switches, SleepThreads answered and idle stretches skipped in them.
    u32 sched_slices{}, sched_switches{}, sched_sleeps{}, sched_idle{};
    /// ArbitrateAddresses the module answered (azaharnative 36).
    u32 sched_arbitrations{};
    /// With `perflog`: the last slices in which the module answered one, each table entry's
    /// state, address, order, wake and r0 before and after (the module's transitions).
    std::vector<std::string> arb_traces;
};
VitaNativeCensus TakeVitaNativeCensus();
#endif

/// In-kernel scheduling (azaharnative 34), on by default: the module switches the guest's
/// threads on a SleepThread itself.
void SetVitaInKernelScheduling(bool on);

class ARM_VitaNative final : public ARM_Interface {
public:
    explicit ARM_VitaNative(Core::System& system, Memory::MemorySystem& memory, u32 id,
                            std::shared_ptr<Core::Timing::Timer> timer,
                            std::shared_ptr<VitaGuestMap> map);
    ~ARM_VitaNative() override;

    /**
     * Takes the core and registers the mapping observer, or returns nullptr if the module is
     * absent or the core cannot be taken — the caller then falls back to interpretation. Once
     * per process: the core stays taken until the console reboots.
     */
    static std::shared_ptr<VitaGuestMap> CreateMap(Memory::MemorySystem& memory);

    void Run() override;
    void Step() override;

    void ClearInstructionCache() override;
    void InvalidateCacheRange(u32 start_address, std::size_t length) override;
    void ClearExclusiveState() override;
    void SetPageTable(const std::shared_ptr<Memory::PageTable>& page_table) override;

    void SetPC(u32 pc) override;
    u32 GetPC() const override;
    u32 GetReg(int index) const override;
    void SetReg(int index, u32 value) override;
    u32 GetVFPReg(int index) const override;
    void SetVFPReg(int index, u32 value) override;
    u32 GetVFPSystemReg(VFPSystemRegister reg) const override;
    void SetVFPSystemReg(VFPSystemRegister reg, u32 value) override;
    u32 GetCPSR() const override;
    void SetCPSR(u32 cpsr) override;
    u32 GetCP15Register(CP15Register reg) const override;
    void SetCP15Register(CP15Register reg, u32 value) override;

    void SaveContext(ThreadContext& ctx) override;
    void LoadContext(const ThreadContext& ctx) override;
    void PrepareReschedule() override;
    /// Puts back every instruction PatchVfp and PatchSvc replaced, after every thread left its
    /// thunk: the state holds the guest's own code.
    void PrepareSaveState() override;

    /// An upcall: svc `svc` answered from the guest's world by the HLE handler with r0-r7 from `in`.
    /// Returns the result (r0) and the thunk's UpcallStatus.
    static u64 UpcallSvc(u32 svc, const std::array<u32, 8>& in);

    bool HasSingleInstructionBreakAccuracy() override {
        return false;
    }

protected:
    std::shared_ptr<Memory::PageTable> GetPageTable() const override;

private:
    /// One trip into the guest, or more while it stands inside an upcall. False if the core should
    /// stop for now.
    bool RunSlice(u64 quantum_ns);
    bool RunSliceOnce(u64 quantum_ns);
    void ReportFault();
    /// Patches a VFP instruction that trapped under the guest's LEN and STRIDE into a branch to a
    /// thunk, or out of reach into the module's svc.
    void PatchVfp(u32 pc, u32 insn, bool thumb);
    bool PatchVfpThunk(u32 pc, u32 insn, bool thumb);
    /// `size` bytes free in the thunk area, mapped into this guest's table.
    std::optional<u32> ThunkRoom(u32 size);
    /// Patches an ARM svc the module answered in the vector into a branch to a thunk answering it
    /// from the module's user pages.
    void PatchSvc(u32 pc);
    /// The upcall experiment: the ARM svc at `pc` (gxm_flags `upcall=N`'s) into a branch to a
    /// thunk that calls the emulator's C++ for it in the guest's world (AZAHAR_SEAL=0 only).
    void PatchUpcall(u32 pc);
    u32 FetchCode(u32 pc, bool thumb) const;

    Core::System& system;
    Memory::MemorySystem& memory;
    Kernel::SVCContext svc_context;

    /// The slices' request, which the module maps and works on in place. Its PC is kept without the
    /// Thumb bit (CPSR.T says the instruction set).
    AzaharRunRequest run{};
    std::shared_ptr<VitaGuestMap> map;
    std::shared_ptr<Memory::PageTable> current_page_table;
    std::array<u32, CP15_REGISTER_COUNT> cp15{};
    u32 fpexc{};
    std::atomic<bool> stop_requested{};
    /// The slice ended inside an upcall: RunSlice runs the thread on.
    bool upcall_again{};
    bool usable{};
};

} // namespace Core
