// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// azahar-native coredump.c: the calling process's core dump, on request (azaharCoredump), so a
// crash the app catches ends in its game list, not the system's error dialog.
// ksceCoredumpCreateDump suspends the process, so it runs on a kernel thread of the module's
// while the caller waits in its syscall; the dump's worker resumes the process partway, so its
// threads run again before the call returns.

typedef int (*fn_coredump_create)(SceUID pid, const char *titleid, SceSize titleid_len,
                                  const char *app_name, SceSize app_name_len, int unk,
                                  char *path, SceSize path_size);

#define COREDUMP_NID_CREATE 0x0C10313Fu  // SceCoredumpForDriver, every firmware
#define COREDUMP_ANY_LIBRARY 0xFFFFFFFFu

static fn_coredump_create coredump_create;
static SceUID coredump_pid;
static char coredump_path[256];

static int coredump_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    // 100 as the system's own crash path passes it (SceCoredumpTriggerParam's word at 0x24).
    return coredump_create(coredump_pid, "AZAHARPSV", 9, "Azahar", 6, 100, coredump_path,
                           sizeof(coredump_path));
}

int azaharCoredump(char *user_path, uint32_t path_size) {
    uint32_t state;
    ENTER_SYSCALL(state);
    int ret = AZAHAR_ERR_RESOLVE;
    if (!coredump_create) {
        uintptr_t create = 0;
        if (module_get_export_func(AZAHAR_KERNEL_PID, "SceCoredump", COREDUMP_ANY_LIBRARY,
                                   COREDUMP_NID_CREATE, &create) < 0) {
            goto out;
        }
        coredump_create = (fn_coredump_create)create;
    }
    coredump_pid = ksceKernelGetProcessId();
    coredump_path[0] = '\0';
    const SceUID thread = ksceKernelCreateThread("SceAzaharCoredump", coredump_thread, 0x10000100,
                                                 0x4000, 0, 0, NULL);
    if (thread < 0) {
        ret = AZAHAR_ERR_ALLOC;
        goto out;
    }
    int created = AZAHAR_ERR_TIMEOUT;
    if (ksceKernelStartThread(thread, 0, NULL) >= 0) {
        ksceKernelWaitThreadEnd(thread, &created, NULL);
    }
    ksceKernelDeleteThread(thread);
    coredump_path[sizeof(coredump_path) - 1] = '\0';
    ksceKernelPrintf("[azaharnative] coredump: create %08x, %s\n", created, coredump_path);
    if (user_path != NULL && path_size != 0) {
        char out[sizeof(coredump_path)];
        SceSize n = 0;
        for (; n + 1 < path_size && n + 1 < sizeof(out) && coredump_path[n] != '\0'; n++) {
            out[n] = coredump_path[n];
        }
        out[n] = '\0';
        ksceKernelCopyToUser(user_path, out, n + 1);
    }
    ret = created;
out:
    EXIT_SYSCALL(state);
    return ret;
}
