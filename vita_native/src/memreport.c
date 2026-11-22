// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// azahar-native azaharMemReport: who holds the console's CDRAM, printed once to the kernel
// console: the calling process's physical memory partitions and every memblock of 64 KiB or
// more. A block's type names its partition class: 0x09 user CDRAM, 0x40 kernel CDRAM, 0x11 the
// GPU's game memory (memtype.h). SceSysmemForKernel's NIDs change every firmware, so they are
// resolved at runtime. Read-only.

#define MEMREPORT_SYSMEM_KERNEL 0x02451F0F // SceSysmemForKernel, 3.63-3.73
#define MEMREPORT_VECTOR_SIZE 0x10000      // the memblock UIDs' vector, a block for the call
#define MEMREPORT_MAX_UIDS (MEMREPORT_VECTOR_SIZE / sizeof(SceUID))

typedef SceClass *(*memreport_class_fn)(void);
typedef int (*memreport_vector_fn)(SceClass *cls, int vis_level, SceUID *vector, SceSize num,
                                   SceSize *ret_num);
typedef int (*memreport_entry_fn)(SceUID guid, void **entry);
typedef const char *(*memreport_entry_name_fn)(void *entry);
typedef SceUID (*memreport_pid_fn)(SceUID guid);
typedef int (*memreport_as_part_fn)(SceUInt32 index, void **part);
typedef int (*memreport_part_core_fn)(void *part, void *info);
typedef int (*memreport_show_part_fn)(void *part);

static uintptr_t memreport_resolve(uint32_t nid) {
    uintptr_t fn = 0;
    if (module_get_export_func(AZAHAR_KERNEL_PID, "SceSysmem", MEMREPORT_SYSMEM_KERNEL, nid,
                               &fn) < 0) {
        return 0;
    }
    return fn;
}

int azaharMemReport(void) {
    uint32_t state;
    ENTER_SYSCALL(state);
    int ret = 0;
    const memreport_class_fn memblock_class = (memreport_class_fn)memreport_resolve(0x86681B64);
    const memreport_vector_fn uid_vector = (memreport_vector_fn)memreport_resolve(0x52137FA3);
    const memreport_entry_fn guid_entry = (memreport_entry_fn)memreport_resolve(0xFF464A49);
    const memreport_entry_name_fn entry_name =
        (memreport_entry_name_fn)memreport_resolve(0x5FD15C6E);
    const memreport_pid_fn guid_pid = (memreport_pid_fn)memreport_resolve(0xBD42A0FC);
    const memreport_as_part_fn as_part = (memreport_as_part_fn)memreport_resolve(0x47C55DCC);
    const memreport_part_core_fn part_core = (memreport_part_core_fn)memreport_resolve(0xB9B69700);
    const memreport_show_part_fn show_part = (memreport_show_part_fn)memreport_resolve(0x8F0B5536);
    ksceKernelPrintf("[azaharnative] memreport: class %p vector %p entry %p name %p pid %p "
                     "as-part %p part-core %p show-part %p\n",
                     memblock_class, uid_vector, guid_entry, entry_name, guid_pid, as_part,
                     part_core, show_part);

    // The partitions behind the calling process's address space, by index.
    if (as_part) {
        for (SceUInt32 index = 0; index < 16; index++) {
            void *part = NULL;
            if (as_part(index, &part) < 0 || !part) {
                continue;
            }
            uint32_t core[16] = {0};
            const int got = part_core ? part_core(part, core) : -1;
            ksceKernelPrintf("[azaharnative] memreport: part %u %p \"%s\" core %08x: %08x %08x "
                             "%08x %08x %08x %08x %08x %08x\n",
                             index, part, (const char *)part + 0x8c, got, core[0], core[1],
                             core[2], core[3], core[4], core[5], core[6], core[7]);
            if (show_part) {
                show_part(part);
            }
        }
    }

    // Every memblock: the first visibility level that answers (3 does for the process class).
    if (!memblock_class || !uid_vector) {
        ret = AZAHAR_ERR_RESOLVE;
        goto out;
    }
    const SceUID vector_uid = ksceKernelAllocMemBlock("AzaharMemReport", SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW,
                                                      MEMREPORT_VECTOR_SIZE, NULL);
    SceUID *memreport_uids = NULL;
    if (vector_uid < 0 || ksceKernelGetMemBlockBase(vector_uid, (void **)&memreport_uids) < 0) {
        ret = AZAHAR_ERR_ALLOC;
        goto out;
    }
    SceClass *const cls = memblock_class();
    SceSize count = 0;
    static const int levels[] = {3, 0, 1, 2, -1};
    for (unsigned v = 0; v < sizeof(levels) / sizeof(levels[0]); v++) {
        count = 0;
        const int got = uid_vector(cls, levels[v], memreport_uids, MEMREPORT_MAX_UIDS, &count);
        ksceKernelPrintf("[azaharnative] memreport: memblocks at vis_level %d: %08x, %u\n",
                         levels[v], got, (unsigned)count);
        if (got >= 0 && count > 0) {
            break;
        }
    }
    if (count > MEMREPORT_MAX_UIDS) {
        count = MEMREPORT_MAX_UIDS;
    }
    uint32_t listed = 0;
    for (SceSize i = 0; i < count; i++) {
        const SceUID uid = memreport_uids[i];
        SceObjectBase *obj = NULL;
        if (ksceGUIDReferObject(uid, &obj) < 0 || !obj) {
            continue;
        }
        const uint32_t *const w = (const uint32_t *)obj;
        const uint32_t type = w[0x10 / 4], base = w[0x18 / 4], size = w[0x1c / 4];
        ksceGUIDReleaseObject(uid);
        if (size < (64u << 10)) {
            continue;
        }
        void *entry = NULL;
        const char *name = guid_entry && entry_name && guid_entry(uid, &entry) >= 0 && entry
                               ? entry_name(entry)
                               : NULL;
        const SceUID pid = guid_pid ? guid_pid(uid) : -1;
        uintptr_t pa = 0;
        if (pid == AZAHAR_KERNEL_PID) {
            ksceKernelVAtoPA((const void *)(uintptr_t)base, &pa);
        }
        ksceKernelPrintf("[azaharnative] memreport: pid %08x %08x type %08x %08x+%uK pa %08x "
                         "\"%s\"\n",
                         pid, uid, type, base, size >> 10, (uint32_t)pa, name ? name : "?");
        listed++;
    }
    ksceKernelPrintf("[azaharnative] memreport: %u memblocks of 64 KiB or more listed of %u\n",
                     listed, (unsigned)count);
    ksceKernelFreeMemBlock(vector_uid);
out:
    EXIT_SYSCALL(state);
    return ret;
}
