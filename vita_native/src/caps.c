// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// Core 3 for the app: SceKernelThreadMgr refuses a game's threads on the system's core
// (isIllegalAffinity); this allows any mask while the module is loaded, as CapUnlocker does. A
// CapUnlocker loaded already keeps its own hook.

// isIllegalAffinity, text+0x114C: the same code (its first 16 bytes) in 3.60, 3.65,
// 3.68 and 3.73.
#define CAPS_AFFINITY_CHECK 0x114Cu
static const uint8_t caps_affinity_head[] = {
    0x10, 0xB5, 0xA0, 0xB1, 0x53, 0x6F, 0x5B, 0x69, 0xB3, 0xF1, 0x00, 0x7F, 0x20, 0xD0, 0x0F, 0xD8,
};
static SceUID caps_hook_uid = -1;
static tai_hook_ref_t caps_hook_ref;

// No mask is illegal.
static int caps_any_affinity(void) {
    return 0;
}

int azaharUnlockCores(void) {
    uint32_t state;
    ENTER_SYSCALL(state);
    int ret = AZAHAR_CORES_OWN;
    tai_module_info_t info;
    info.size = sizeof(info);
    if (caps_hook_uid >= 0) {
        goto out;
    }
    if (taiGetModuleInfoForKernel(AZAHAR_KERNEL_PID, "CapUnlocker", &info) >= 0) {
        ret = AZAHAR_CORES_CAPUNLOCKER;
        goto out;
    }
    info.size = sizeof(info);
    uintptr_t text = 0;
    if (taiGetModuleInfoForKernel(AZAHAR_KERNEL_PID, "SceKernelThreadMgr", &info) < 0 ||
        module_get_offset(AZAHAR_KERNEL_PID, info.modid, 0, 0, &text) < 0 || !text ||
        memcmp((const void *)((text & ~1u) + CAPS_AFFINITY_CHECK), caps_affinity_head,
               sizeof(caps_affinity_head)) != 0) {
        ret = AZAHAR_ERR_RESOLVE;
        goto out;
    }
    const SceUID uid = taiHookFunctionOffsetForKernel(AZAHAR_KERNEL_PID, &caps_hook_ref,
                                                      info.modid, 0, CAPS_AFFINITY_CHECK, 1,
                                                      caps_any_affinity);
    if (uid < 0) {
        ret = uid;
        goto out;
    }
    caps_hook_uid = uid;
out:
    ksceKernelPrintf("[azaharnative] cores: %s (0x%08x)\n",
                     ret == AZAHAR_CORES_CAPUNLOCKER ? "CapUnlocker's"
                     : ret == AZAHAR_CORES_OWN       ? "the module's own unlock"
                                                     : "not unlocked",
                     (uint32_t)ret);
    EXIT_SYSCALL(state);
    return ret;
}

// The module's stop: the check as it was (or CapUnlocker's, which never was ours).
static void caps_release(void) {
    if (caps_hook_uid >= 0) {
        taiHookReleaseForKernel(caps_hook_uid, caps_hook_ref);
        caps_hook_uid = -1;
    }
}
