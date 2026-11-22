// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// azahar-native: SceShell while sgxd holds the GPU (shell_take to shell_give). Its main thread
// sleeps at SceGpu's gate, and SceAppMgr's watchdog reboots once SceShell has not called
// sceAppMgrReceiveShellEventNum for 30 s. So:
// - a thread of ours makes that call's write every 5 s, the same function under the same mutex;
// - the volume keys are out of SceShell's pad data (it would sleep at its volume bar); our
//   thread reads them from Syscon every 20 ms for azaharVolumeKeys;
// - the app steps the system volume (azaharSystemVolume) as SceShell's handler does, past
//   sceAVConfigSetSystemVol's system-program check: the route, the level under SceAVConfig's
//   spinlock, event 2. The built-in output's level goes into the registry at the release.
// Both modules are pinned by their module NIDs and the code of each function used, for 3.60, 3.65,
// 3.68 and 3.73; with another build the watchdog is left alone and the volume calls fail.

#include <psp2kern/ctrl.h>
#include <psp2kern/kernel/syscon.h>
#include <psp2kern/kernel/threadmgr/event_flags.h>
#include <psp2kern/registrymgr.h>

// SceAppMgr's builds: the module NID, then where its two functions are and their code prints:
// the feed writes the system time into the watchdog's word; the other is
// sceAppMgrReceiveShellEventNum's body.
typedef struct {
    uint32_t nid;
    uint32_t feed, feed_print;
    uint32_t event_num, event_num_print;
} ShellAppMgrBuild;
static const ShellAppMgrBuild shell_appmgr_builds[] = {
    {0x1C9879D6u, 0x9B04u, 0xB2883646u, 0x14A50u, 0x03E0F663u}, // 3.65
    {0xDBB29DB7u, 0x9B04u, 0x94EE7168u, 0x14A50u, 0x74A57E41u}, // 3.60
    {0xC3C538DEu, 0x9B10u, 0xB2883646u, 0x14A5Cu, 0x391A55DBu}, // 3.68
    {0xB299D195u, 0x9B18u, 0x1985AA76u, 0x14A84u, 0xB21FD26Bu}, // 3.73
};
#define SHELL_FEED_CODE 0x14u
#define SHELL_EVENT_NUM_CODE 0x7Cu
#define SHELL_MUTEX 0x4A4u           // data: the mutex's UID, then its holder's thread ID
#define SHELL_FEED_US 5000000u

// SceAVConfig's builds (3.65, 3.60, 3.68, 3.73): one code and data, the module NID apart.
static const uint32_t shell_avconfig_nids[] = {0x55A6E312u, 0x222DDEB1u, 0xA1F08F46u,
                                               0x136D0561u};
#define SHELL_SET_VOL 0x1F60u        // sceAVConfigSetSystemVol, whose steps these are
#define SHELL_SET_VOL_CODE 0xA8u
#define SHELL_SET_VOL_PRINT 0x5AA7379Bu
#define SHELL_ROUTE 0x1504u          // the output route (1 built-in, 2 Bluetooth)
#define SHELL_ROUTE_CODE 0x5Cu
#define SHELL_ROUTE_PRINT 0x80275388u
#define SHELL_EVENT 0x14ECu          // SceAVConfig's event flag, these bits set
#define SHELL_EVENT_CODE 0x18u
#define SHELL_EVENT_PRINT 0xA50A629Cu
#define SHELL_AV_LOCK 0x2ACu         // data: SceAVConfig's spinlock
#define SHELL_AV_VOLUME 0x270u       // data: the built-in output's level
#define SHELL_AV_BT_VOLUME 0x278u    // data: the Bluetooth output's level
#define SHELL_AV_AVLS 0x2E8u         // data: AVLS on (/CONFIG/SOUND/avls)
#define SHELL_AV_VOLUME_CHANGED 2u
#define SHELL_VOLUME_MAX 30
#define SHELL_VOLUME_AVLS 21

#define SHELL_VOLUME_KEYS (SCE_CTRL_VOLUP | SCE_CTRL_VOLDOWN)
#define SHELL_SYSCON_VOLUP (1u << 16)   // Syscon's controls word: low while pressed
#define SHELL_SYSCON_VOLDOWN (1u << 17)
#define SHELL_KEYS_US 20000u            // the volume keys' poll
#define SHELL_WAKE_STOP 1u

typedef void (*shell_feed_fn)(void);
typedef void (*shell_route_fn)(int *route, int *spare);
typedef void (*shell_event_fn)(uint32_t bits);

static int shell_identified;
static shell_feed_fn shell_feed;
static volatile SceUID *shell_mutex;   // SceAppMgr's mutex UID, then its holder
static shell_route_fn shell_route;
static shell_event_fn shell_av_event;
static uintptr_t shell_av_data;

static SceUID shell_thread = -1;
static SceUID shell_wake = -1;
static uint32_t shell_keys_all, shell_keys_non_shell;
static int shell_keys_taken;
static volatile uint32_t shell_keys_held; // the volume keys held, SCE_CTRL_* bits (shell_main)
static int shell_volume_start = -1; // the built-in output's level at the take

// The module's segments, if its module NID is one of `nids`: *build is its index there.
static int shell_module(const char *name, const uint32_t *nids, unsigned count, uintptr_t *text,
                        uintptr_t *data, unsigned *build) {
    tai_module_info_t tai = {.size = sizeof(tai)};
    if (taiGetModuleInfoForKernel(AZAHAR_KERNEL_PID, name, &tai) < 0 ||
        module_get_offset(AZAHAR_KERNEL_PID, tai.modid, 0, 0, text) < 0 ||
        module_get_offset(AZAHAR_KERNEL_PID, tai.modid, 1, 0, data) < 0) {
        SGXD_LOG("shell: %s not found\n", name);
        return 0;
    }
    for (unsigned i = 0; i < count; i++) {
        if (tai.module_nid == nids[i]) {
            *build = i;
            return 1;
        }
    }
    SGXD_LOG("shell: %s module nid %08x is no build known (3.60, 3.65, 3.68, 3.73)\n", name,
             tai.module_nid);
    return 0;
}

static int shell_code(uintptr_t text, uint32_t offset, uint32_t size, uint32_t print) {
    const uint32_t got = sgxd_code_fingerprint((const uint16_t *)(text + offset), size);
    if (got != print) {
        SGXD_LOG("shell: code at %08x is %08x, want %08x\n", (uint32_t)(text + offset), got,
                 print);
    }
    return got == print;
}

// Once per boot: neither module is ever unloaded.
static void shell_identify(void) {
    if (shell_identified) {
        return;
    }
    shell_identified = 1;
    uintptr_t text = 0, data = 0;
    uint32_t appmgr_nids[sizeof(shell_appmgr_builds) / sizeof(shell_appmgr_builds[0])];
    for (unsigned i = 0; i < sizeof(appmgr_nids) / sizeof(appmgr_nids[0]); i++) {
        appmgr_nids[i] = shell_appmgr_builds[i].nid;
    }
    unsigned build = 0;
    if (shell_module("SceAppMgr", appmgr_nids, sizeof(appmgr_nids) / sizeof(appmgr_nids[0]),
                     &text, &data, &build)) {
        const ShellAppMgrBuild *b = &shell_appmgr_builds[build];
        if (shell_code(text, b->feed, SHELL_FEED_CODE, b->feed_print) &&
            shell_code(text, b->event_num, SHELL_EVENT_NUM_CODE, b->event_num_print)) {
            shell_feed = (shell_feed_fn)(text + b->feed + 1);
            shell_mutex = (volatile SceUID *)(data + SHELL_MUTEX);
        }
    }
    if (shell_module("SceAVConfig", shell_avconfig_nids,
                     sizeof(shell_avconfig_nids) / sizeof(shell_avconfig_nids[0]), &text, &data,
                     &build) &&
        shell_code(text, SHELL_SET_VOL, SHELL_SET_VOL_CODE, SHELL_SET_VOL_PRINT) &&
        shell_code(text, SHELL_ROUTE, SHELL_ROUTE_CODE, SHELL_ROUTE_PRINT) &&
        shell_code(text, SHELL_EVENT, SHELL_EVENT_CODE, SHELL_EVENT_PRINT)) {
        shell_route = (shell_route_fn)(text + SHELL_ROUTE + 1);
        shell_av_event = (shell_event_fn)(text + SHELL_EVENT + 1);
        shell_av_data = data;
    }
}

static inline volatile int *shell_av_word(uint32_t offset) {
    return (volatile int *)(shell_av_data + offset);
}

// What sceAppMgrReceiveShellEventNum does for SceShell: the watchdog's time, under the mutex,
// with the holder's word as it keeps it.
static void shell_feed_watchdog(void) {
    if (!shell_feed || ksceKernelLockMutex(shell_mutex[0], 1, NULL) < 0) {
        return;
    }
    shell_mutex[1] = ksceKernelGetThreadId();
    shell_feed();
    shell_mutex[1] = -1;
    ksceKernelUnlockMutex(shell_mutex[0], 1);
}

// The volume keys held, from Syscon's controls word.
static void shell_read_keys(void) {
    SceUInt32 controls = 0;
    if (ksceSysconGetControlsInfo(&controls) < 0) {
        return;
    }
    shell_keys_held = (controls & SHELL_SYSCON_VOLUP ? 0 : SCE_CTRL_VOLUP) |
                      (controls & SHELL_SYSCON_VOLDOWN ? 0 : SCE_CTRL_VOLDOWN);
}

static int shell_main(SceSize args, void *argp) {
    (void)args; (void)argp;
    SceUInt64 fed = ksceKernelGetSystemTimeWide();
    for (;;) {
        unsigned int bits = 0;
        SceUInt timeout = SHELL_KEYS_US;
        ksceKernelWaitEventFlag(shell_wake, SHELL_WAKE_STOP, SCE_EVENT_WAITOR, &bits, &timeout);
        if (bits & SHELL_WAKE_STOP) {
            break;
        }
        shell_read_keys();
        const SceUInt64 now = ksceKernelGetSystemTimeWide();
        if (now - fed >= SHELL_FEED_US) {
            shell_feed_watchdog();
            fed = now;
        }
    }
    shell_keys_held = 0;
    return 0;
}

// sgxd's take: the volume keys the app's, the thread started.
static void shell_take(void) {
    shell_identify();
    if (!shell_keys_taken) {
        uint32_t all = 0, non_shell = 0;
        ksceCtrlGetMaskForAll(&all);
        ksceCtrlGetMaskForNonShell(&non_shell);
        shell_keys_all = all & SHELL_VOLUME_KEYS;
        shell_keys_non_shell = non_shell & SHELL_VOLUME_KEYS;
        ksceCtrlUpdateMaskForAll(SHELL_VOLUME_KEYS, 0);
        ksceCtrlUpdateMaskForNonShell(0, SHELL_VOLUME_KEYS);
        shell_keys_taken = 1;
    }
    if (shell_volume_start < 0 && shell_av_data) {
        shell_volume_start = *shell_av_word(SHELL_AV_VOLUME);
    }
    if (shell_thread >= 0) {
        return;
    }
    shell_wake = ksceKernelCreateEventFlag("SceAzaharShellWake", 0, 0, NULL);
    // On core 0, beside the app's helper threads: the guest's core is left alone.
    shell_thread = ksceKernelCreateThread("SceAzaharShell", shell_main, 0x10000100, 0x1000, 0,
                                          CPU_AFFINITY(0), NULL);
    if (shell_wake < 0 || shell_thread < 0 || ksceKernelStartThread(shell_thread, 0, NULL) < 0) {
        SGXD_LOG("shell: no thread (0x%08x, 0x%08x)\n", (uint32_t)shell_wake,
                 (uint32_t)shell_thread);
        if (shell_thread >= 0) {
            ksceKernelDeleteThread(shell_thread);
        }
        if (shell_wake >= 0) {
            ksceKernelDeleteEventFlag(shell_wake);
        }
        shell_thread = shell_wake = -1;
    }
}

// sgxd's release: the thread ended, the keys back as they were at the take, and the built-in
// output's level, if the app's steps changed it, where SceShell keeps it.
static void shell_give(void) {
    if (shell_thread >= 0) {
        ksceKernelSetEventFlag(shell_wake, SHELL_WAKE_STOP);
        ksceKernelWaitThreadEnd(shell_thread, NULL, NULL);
        ksceKernelDeleteThread(shell_thread);
        ksceKernelDeleteEventFlag(shell_wake);
        shell_thread = shell_wake = -1;
    }
    if (shell_keys_taken) {
        ksceCtrlUpdateMaskForAll(SHELL_VOLUME_KEYS, shell_keys_all);
        ksceCtrlUpdateMaskForNonShell(SHELL_VOLUME_KEYS, shell_keys_non_shell);
        shell_keys_taken = 0;
    }
    if (shell_volume_start >= 0) {
        const int level = *shell_av_word(SHELL_AV_VOLUME);
        if (level != shell_volume_start) {
            ksceRegMgrSetKeyInt("/CONFIG/SOUND", "main_volume", level);
        }
        shell_volume_start = -1;
    }
}

int azaharSystemVolume(int step) {
    uint32_t state;
    ENTER_SYSCALL(state);
    int ret = AZAHAR_ERR_STATE;
    if (sgxd_mutex < 0 || ksceKernelLockMutex(sgxd_mutex, 1, NULL) < 0) {
        goto out;
    }
    if (sgxd_state == SGXD_OWNED && sgxd_owner == ksceKernelGetProcessId() && shell_av_data) {
        int route = 0, spare = 0;
        shell_route(&route, &spare);
        static int logged_route = -1;
        if (route != logged_route) {
            SGXD_LOG("shell: volume step on output route %d\n", route);
            logged_route = route;
        }
        if (route != 1 && route != 2) {
            // HDMI, USB and no output: SceShell's handler steps nothing either.
            ret = AZAHAR_ERR_ROUTE;
        } else {
            SceKernelSpinlock *lock = (SceKernelSpinlock *)shell_av_word(SHELL_AV_LOCK);
            volatile int *word = shell_av_word(route == 1 ? SHELL_AV_VOLUME : SHELL_AV_BT_VOLUME);
            // AVLS holds the built-in output at 21 as SceShell does; a level past it steps down.
            const int max =
                route == 1 && *shell_av_word(SHELL_AV_AVLS) ? SHELL_VOLUME_AVLS : SHELL_VOLUME_MAX;
            const SceKernelIntrStatus st = ksceKernelSpinlockLowLockCpuSuspendIntr(lock);
            int level = *word;
            if (step > 0 && level < max) {
                level++;
            } else if (step < 0 && level > 0) {
                level--;
            }
            const int changed = level != *word;
            *word = level;
            ksceKernelSpinlockLowUnlockCpuResumeIntr(lock, st);
            if (changed) {
                shell_av_event(SHELL_AV_VOLUME_CHANGED);
            }
            ret = level;
        }
    }
    ksceKernelUnlockMutex(sgxd_mutex, 1);
out:
    EXIT_SYSCALL(state);
    return ret;
}

int azaharVolumeKeys(void) {
    uint32_t state;
    ENTER_SYSCALL(state);
    int ret = AZAHAR_ERR_STATE;
    if (sgxd_mutex >= 0 && ksceKernelLockMutex(sgxd_mutex, 1, NULL) >= 0) {
        if (sgxd_state == SGXD_OWNED && sgxd_owner == ksceKernelGetProcessId() &&
            shell_thread >= 0) {
            ret = (int)shell_keys_held;
        }
        ksceKernelUnlockMutex(sgxd_mutex, 1);
    }
    EXIT_SYSCALL(state);
    return ret;
}
