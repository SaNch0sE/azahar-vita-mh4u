// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// The POSIX pieces LLVM's profile runtime links against that the Vita's newlib does not have
// (getpagesize is azahar's own, src/vita_compat).
//
// The runtime maps memory only in its continuous mode (counters mapped onto the profile file) and
// to merge into an existing file; neither works without file mappings, so azahar writes the
// profile on demand instead (`pgo dump`, __llvm_profile_write_file). Anonymous mappings come from
// the kernel's memory blocks, file mappings are refused.

#include <errno.h>
#include <stddef.h>
#include <sys/types.h>
#include <psp2/kernel/sysmem.h>

#define MAP_FAILED_ ((void *)-1)
#define MAP_ANONYMOUS_ 0x20

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    (void)addr;
    (void)prot;
    (void)offset;
    if (!(flags & MAP_ANONYMOUS_) || fd != -1 || length == 0) {
        errno = ENOTSUP;
        return MAP_FAILED_;
    }
    const size_t size = (length + 0xfffu) & ~(size_t)0xfffu;
    const SceUID uid = sceKernelAllocMemBlock("azahar-pgo", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, size, NULL);
    void *base = NULL;
    if (uid < 0 || sceKernelGetMemBlockBase(uid, &base) < 0) {
        errno = ENOMEM;
        return MAP_FAILED_;
    }
    return base;
}

int munmap(void *addr, size_t length) {
    (void)length;
    const SceUID uid = sceKernelFindMemBlockByAddr(addr, 0);
    if (uid < 0 || sceKernelFreeMemBlock(uid) < 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int madvise(void *addr, size_t length, int advice) {
    (void)addr;
    (void)length;
    (void)advice;
    return 0;
}

// %h in a profile file name: no host name here.
int lprofGetHostName(char *name, int len) {
    (void)name;
    (void)len;
    return -1;
}

// What instrumented code references so that the runtime is linked in; compiler-rt defines it in
// InstrProfilingRuntime.cpp, whose initializer would write the profile at exit (an app the system
// ends never exits). Here it is the symbol alone.
int __llvm_profile_runtime;
