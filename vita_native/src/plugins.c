// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// azahar-native plugins.c: the kernel modules the console runs besides its firmware, for the
// app's crash report (azaharKernelPlugins). SceModulemgrForKernel's NIDs changed at 3.63, so
// both sets are tried. Modules from os0:, vs0: or bootfs: are the firmware's and left out.

#define PLUGINS_LIB_360 0xC445FA63u // SceModulemgrForKernel, 3.60
#define PLUGINS_LIB_363 0x92C9FFC2u // SceModulemgrForKernel, 3.63-3.73
#define PLUGINS_MAX_MODULES 256

typedef int (*plugins_list_fn)(SceUID pid, int flags1, int flags2, SceUID *modids, SceSize *num);
typedef int (*plugins_info_fn)(SceUID pid, SceUID modid, SceKernelModuleInfo *info);

static SceUID plugins_ids[PLUGINS_MAX_MODULES];
static SceKernelModuleInfo plugins_info;
static char plugins_text[2048];
static int plugins_busy;

static uintptr_t plugins_resolve(uint32_t nid_360, uint32_t nid_363) {
    uintptr_t fn = 0;
    if (module_get_export_func(AZAHAR_KERNEL_PID, "SceKernelModulemgr", PLUGINS_LIB_360, nid_360,
                               &fn) < 0 &&
        module_get_export_func(AZAHAR_KERNEL_PID, "SceKernelModulemgr", PLUGINS_LIB_363, nid_363,
                               &fn) < 0) {
        return 0;
    }
    return fn;
}

static int plugins_firmware_path(const char *path) {
    return (((path[0] == 'o' || path[0] == 'v') && path[1] == 's' && path[2] == '0' &&
             path[3] == ':') ||
            __builtin_memcmp(path, "bootfs:", 7) == 0);
}

int azaharKernelPlugins(char *user_out, uint32_t size) {
    uint32_t state;
    ENTER_SYSCALL(state);
    int ret = AZAHAR_ERR_ARG;
    if (user_out == NULL || size == 0) {
        goto out;
    }
    ret = AZAHAR_ERR_STATE;
    if (__atomic_exchange_n(&plugins_busy, 1, __ATOMIC_ACQUIRE)) {
        goto out;
    }
    ret = AZAHAR_ERR_RESOLVE;
    const plugins_list_fn list = (plugins_list_fn)plugins_resolve(0x97CF7B4E, 0xB72C75A4);
    const plugins_info_fn info = (plugins_info_fn)plugins_resolve(0xD269F915, 0xDAA90093);
    if (list && info) {
        SceSize num = PLUGINS_MAX_MODULES;
        ret = list(AZAHAR_KERNEL_PID, 0x7FFFFFFF, 1, plugins_ids, &num);
        if (ret >= 0) {
            // "name path" a line, as many as fit; the count listed.
            uint32_t at = 0;
            ret = 0;
            for (SceSize i = 0; i < num && i < PLUGINS_MAX_MODULES; i++) {
                __builtin_memset(&plugins_info, 0, sizeof(plugins_info));
                plugins_info.size = sizeof(plugins_info);
                if (info(AZAHAR_KERNEL_PID, plugins_ids[i], &plugins_info) < 0) {
                    continue;
                }
                plugins_info.module_name[sizeof(plugins_info.module_name) - 1] = '\0';
                plugins_info.path[sizeof(plugins_info.path) - 1] = '\0';
                if (plugins_firmware_path(plugins_info.path)) {
                    continue;
                }
                const int n = snprintf(plugins_text + at, sizeof(plugins_text) - at, "%s %s\n",
                                       plugins_info.module_name, plugins_info.path);
                if (n < 0 || at + (uint32_t)n >= sizeof(plugins_text)) {
                    break;
                }
                at += (uint32_t)n;
                ret++;
            }
            plugins_text[at] = '\0';
            const uint32_t copy = at + 1 < size ? at + 1 : size;
            if (copy > 1) {
                ksceKernelCopyToUser(user_out, plugins_text, copy - 1);
            }
            ksceKernelCopyToUser(user_out + copy - 1, "", 1);
        }
    }
    __atomic_store_n(&plugins_busy, 0, __ATOMIC_RELEASE);
out:
    EXIT_SYSCALL(state);
    return ret;
}
