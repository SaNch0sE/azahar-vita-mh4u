
#include <psp2kern/ctrl.h>
#include <psp2kern/kernel/syscon.h>
#include <psp2kern/kernel/threadmgr/event_flags.h>
#include <psp2kern/registrymgr.h>

#define SHELL_APPMGR_NID 0x1C9879D6u
#define SHELL_FEED 0x9B04u           // the system time into the watchdog's word
#define SHELL_FEED_CODE 0x14u
#define SHELL_FEED_PRINT 0xB2883646u
#define SHELL_EVENT_NUM 0x14A50u     // sceAppMgrReceiveShellEventNum's body
#define SHELL_EVENT_NUM_CODE 0x7Cu
#define SHELL_EVENT_NUM_PRINT 0x03E0F663u
#define SHELL_MUTEX 0x4A4u           // data: the mutex's UID, then its holder's thread ID
#define SHELL_FEED_US 5000000u

#define SHELL_AVCONFIG_NID 0x55A6E312u
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

static int shell_module(const char *name, uint32_t nid, uintptr_t *text, uintptr_t *data) {
    tai_module_info_t tai = {.size = sizeof(tai)};
    if (taiGetModuleInfoForKernel(AZAHAR_KERNEL_PID, name, &tai) < 0 ||
        module_get_offset(AZAHAR_KERNEL_PID, tai.modid, 0, 0, text) < 0 ||
        module_get_offset(AZAHAR_KERNEL_PID, tai.modid, 1, 0, data) < 0) {
        SGXD_LOG("shell: %s not found\n", name);
        return 0;
    }
    if (tai.module_nid != nid) {
        SGXD_LOG("shell: %s module nid %08x, want %08x\n", name, tai.module_nid, nid);
        return 0;
    }
    return 1;
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
    if (shell_module("SceAppMgr", SHELL_APPMGR_NID, &text, &data) &&
        shell_code(text, SHELL_FEED, SHELL_FEED_CODE, SHELL_FEED_PRINT) &&
        shell_code(text, SHELL_EVENT_NUM, SHELL_EVENT_NUM_CODE, SHELL_EVENT_NUM_PRINT)) {
        shell_feed = (shell_feed_fn)(text + SHELL_FEED + 1);
        shell_mutex = (volatile SceUID *)(data + SHELL_MUTEX);
    }
    if (shell_module("SceAVConfig", SHELL_AVCONFIG_NID, &text, &data) &&
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
