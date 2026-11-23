/* sys/mman.h for the PS Vita.
 *
 * The console's newlib ships no <sys/mman.h> and has no file mapping. The only caller in this
 * build is cpp-httplib's mmap class, which serves static files from an HTTP server the emulator
 * never runs. It is given the declarations it compiles against and an mmap that fails with
 * ENOSYS, which httplib already treats as "could not open".
 */

#pragma once

#include <errno.h>
#include <stddef.h>
#include <sys/types.h>

#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4

#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS

#define MAP_FAILED ((void*)-1)

#ifdef __cplusplus
extern "C" {
#endif

static inline void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
    (void)addr;
    (void)length;
    (void)prot;
    (void)flags;
    (void)fd;
    (void)offset;
    errno = ENOSYS;
    return MAP_FAILED;
}

static inline int munmap(void* addr, size_t length) {
    (void)addr;
    (void)length;
    errno = ENOSYS;
    return -1;
}

#ifdef __cplusplus
}
#endif
