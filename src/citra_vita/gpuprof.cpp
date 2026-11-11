// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "citra_vita/gpuprof.h"

#include <atomic>
#include <cstdio>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>

extern "C" {
#include <psp2_gpuprof.h>
#include <psp2_gpuprof_capture.h>
#include <psp2_gpuprof_capture_diagnostic.h>
#include <psp2_gpuprof_diagnostic.h>
#include <psp2_gpuprof_gxm_observer.h>
#include <psp2_gpuprof_gxm_state.h>
}

namespace VitaFrontend::GpuProf {
namespace {

constexpr const char* TriggerPath = "ux0:data/azahar/gpuprof_now";
constexpr const char* OutputPath = "ux0:data/azahar/gpuprof.csv";
constexpr unsigned CaptureSeconds = 2;
// Static rather than heap: the newlib heap is what the emulator runs out of first, and this
// is ~11 MB the executable's own data segment takes from the free user memory instead.
constexpr size_t EventCapacity = 49152;
Psp2GpuProfCaptureEvent events[EventCapacity];
Psp2GpuProfCapture capture;
SceUID capture_mutex = -1;

// Every live context and shader program, tracked from the first GXM call so a capture started
// mid-game can open with a snapshot of them (psp2GpuProfGxmStateAttach); without it no draw can
// be tied to its shaders. Serialized by count_lock, so the cache takes no lock of its own.
constexpr size_t ObjectCapacity = 2048;
Psp2GpuProfGxmObject objects[ObjectCapacity];
Psp2GpuProfCaptureEvent snapshot_scratch[ObjectCapacity + 1];
Psp2GpuProfGxmState gxm_state;

// The identity model's counters, kept from the first GXM call: presents, and scenes ended
// since the last present. Guarded by count_lock; the observer may run on several threads.
SceUID count_lock = -1;
uint32_t frames = 0;
uint32_t scenes = 0;
bool scene_open = false;

enum State : int { Counting, Armed, Recording, Stopping, Done };
std::atomic<int> state{Counting};
std::atomic<uint64_t> record_start_us{0};

void Lock(void*) {
    sceKernelLockMutex(capture_mutex, 1, nullptr);
}
void Unlock(void*) {
    sceKernelUnlockMutex(capture_mutex, 1);
}

void Observer(void*, const Psp2GpuProfGxmCall* call) {
    // Every observed call is serialized here: the counters, the state cache and the switches
    // between not recording and recording all happen with no other GXM call in between.
    sceKernelLockMutex(count_lock, 1, nullptr);
    const bool ended = call->result == 0 && call->kind == PSP2_GPUPROF_GXM_END;
    if (call->result == 0) {
        switch (call->kind) {
        case PSP2_GPUPROF_GXM_BEGIN:
            scene_open = true;
            break;
        case PSP2_GPUPROF_GXM_END:
            scene_open = false;
            scenes++;
            break;
        case PSP2_GPUPROF_GXM_PRESENT:
            frames++;
            scenes = 0;
            break;
        default:
            break;
        }
    }
    // Records the call into the capture once one is attached.
    psp2GpuProfGxmStateObserve(&gxm_state, call);
    if (ended && state.load() == Armed) {
        // No scene is open: seed the identity with the exact counts, then the snapshot of
        // every live object, and from the next call on everything is recorded.
        const uint64_t now = sceKernelGetSystemTimeWide();
        if (psp2GpuProfCaptureStart(&capture) == 0 &&
            psp2GpuProfCaptureGxmIdentitySeed365(&capture, sceKernelGetProcessId(), frames,
                                                 scenes) == 0 &&
            psp2GpuProfGxmStateAttach(&gxm_state, &capture, snapshot_scratch,
                                      ObjectCapacity + 1, now) == 0) {
            record_start_us = now;
            state = Recording;
        } else {
            state = Done;
        }
    } else if (ended && state.load() == Stopping) {
        // The scene this END closed was recorded whole; nothing after it is.
        psp2GpuProfGxmStateDetach(&gxm_state);
        state = Done;
    }
    sceKernelUnlockMutex(count_lock, 1);
}

int SamplerThread(SceSize, void*) {
    static const unsigned groups[] = {2,  4,  43,  55,  71,  72,  75,  76,
                                      77, 78, 103, 104, 107, 108, 109, 110};
    Psp2GpuProfInfo info{};
    info.size = sizeof(info);
    info.abi = PSP2_GPUPROF_ABI;
    const int info_rc = psp2GpuProfGetInfo(&info);
    sceClibPrintf("[gpuprof] GetInfo %d status %d capabilities 0x%x\n", info_rc,
                  static_cast<int>(info.status), static_cast<unsigned>(info.capabilities));
    const bool diagnostic =
        info_rc == 0 && info.status == 0 && (info.capabilities & PSP2_GPUPROF_CAP_DIAGNOSTIC);
    for (;;) {
        // Wait for the trigger file while counting continues in the observer.
        while (state.load() == Counting) {
            SceIoStat st{};
            if (sceIoGetstat(TriggerPath, &st) >= 0) {
                sceIoRemove(TriggerPath);
                const Psp2GpuProfCaptureConfig config{events, EventCapacity, Lock, Unlock,
                                                      nullptr};
                if (psp2GpuProfCaptureInit(&capture, &config) == 0) {
                    sceClibPrintf("[gpuprof] armed\n");
                    state = Armed;
                } else {
                    sceClibPrintf("[gpuprof] capture init failed\n");
                }
            } else {
                sceKernelDelayThread(500 * 1000);
            }
        }
        while (state.load() == Armed) {
            sceKernelDelayThread(1000);
        }
        if (state.load() != Recording) {
            sceClibPrintf("[gpuprof] could not start (seed rejected)\n");
            return 0;
        }
        sceClibPrintf("[gpuprof] recording from frame %u scene %u\n", frames, scenes);
        for (uint64_t id = 0;
             sceKernelGetSystemTimeWide() - record_start_us.load() < CaptureSeconds * 1000000ull;
             ++id) {
            if (!diagnostic) {
                sceKernelDelayThread(10 * 1000);
                continue;
            }
            const unsigned group = groups[id % 16];
            Psp2GpuProfDiagnosticConfig cfg{};
            cfg.size = sizeof(cfg);
            cfg.abi = PSP2_GPUPROF_ABI;
            cfg.group = group;
            cfg.tag_group = group >= 103 ? 102 : 70;
            Psp2GpuProfDiagnostic data{};
            Psp2GpuProfDiagnosticObservation o{};
            o.sampler_thread = static_cast<uint32_t>(sceKernelGetThreadId());
            o.sample_id = id;
            o.group = group;
            o.tag_group = cfg.tag_group;
            o.process_id = static_cast<uint32_t>(sceKernelGetProcessId());
            o.driver_fingerprint = info.driver_fingerprint;
            int mhz = scePowerGetGpuClockFrequency();
            o.clock_before_mhz = mhz > 0 ? static_cast<uint32_t>(mhz) : 0;
            o.before_us = sceKernelGetSystemTimeWide();
            o.result = psp2GpuProfReadDiagnostic(&cfg, &data);
            o.after_us = sceKernelGetSystemTimeWide();
            mhz = scePowerGetGpuClockFrequency();
            o.clock_after_mhz = mhz > 0 ? static_cast<uint32_t>(mhz) : 0;
            psp2GpuProfCaptureDiagnostic(&capture, &o, o.result ? nullptr : &data);
            sceKernelDelayThread(500);
        }
        // Stop at the next scene end, so no scene is cut in half (the observer detaches).
        state = Stopping;
        for (unsigned i = 0; i < 2000 && state.load() != Done; i++) {
            sceKernelDelayThread(1000);
        }
        if (state.load() != Done) {
            sceKernelLockMutex(count_lock, 1, nullptr);
            psp2GpuProfGxmStateDetach(&gxm_state);
            state = Done;
            sceKernelUnlockMutex(count_lock, 1);
        }
        const int stop_rc = psp2GpuProfCaptureStop(&capture);
        int write_rc = -1;
        if (FILE* f = std::fopen(OutputPath, "w")) {
            write_rc = psp2GpuProfCaptureWriteCsv(&capture, f);
            std::fclose(f);
        }
        sceClibPrintf("[gpuprof] wrote %s: stop %d write %d, %u events, %llu dropped\n",
                      OutputPath, stop_rc, write_rc, static_cast<unsigned>(capture.count),
                      static_cast<unsigned long long>(capture.dropped));
        return 0;
    }
}

} // namespace

void Install() {
    capture_mutex = sceKernelCreateMutex("gpuprof-capture", 0, 0, nullptr);
    count_lock = sceKernelCreateMutex("gpuprof-count", 0, 0, nullptr);
    if (capture_mutex < 0 || count_lock < 0) {
        sceClibPrintf("[gpuprof] mutex creation failed\n");
        return;
    }
    if (psp2GpuProfGxmStateInit(&gxm_state, objects, ObjectCapacity, nullptr, nullptr,
                                nullptr) != 0) {
        sceClibPrintf("[gpuprof] state cache init failed\n");
        return;
    }
    psp2GpuProfGxmSetObserver(Observer, nullptr);
    const SceUID thread =
        sceKernelCreateThread("gpuprof-sampler", SamplerThread, 0x10000100, 0x10000, 0, 0, nullptr);
    if (thread >= 0) {
        sceKernelStartThread(thread, 0, nullptr);
    }
    sceClibPrintf("[gpuprof] installed; create %s to capture %u s to %s\n", TriggerPath,
                  CaptureSeconds, OutputPath);
}

} // namespace VitaFrontend::GpuProf
