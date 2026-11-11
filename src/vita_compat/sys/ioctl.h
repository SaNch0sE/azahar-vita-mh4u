/* sys/ioctl.h for the PS Vita.
 *
 * The console's newlib ships no <sys/ioctl.h> and no ioctl. Boost.Asio and libressl's BIO reach
 * for it to switch a socket between blocking and non-blocking (FIONBIO), which newlib does offer
 * as fcntl(F_SETFL, O_NONBLOCK) - mapped onto the socket's SO_NONBLOCK - so that request is
 * answered through it. FIONREAD, the bytes-ready query, has no equivalent here; it and every other
 * request fail with ENOSYS rather than pretend to have worked.
 */

#pragma once

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>

#ifndef FIONBIO
#define FIONBIO 0x8004667e
#endif
#ifndef FIONREAD
#define FIONREAD 0x4004667f
#endif

#ifdef __cplusplus
extern "C" {
#endif

static inline int ioctl(int fd, unsigned long request, ...) {
    if (request == FIONBIO) {
        va_list ap;
        va_start(ap, request);
        const int* on = va_arg(ap, const int*);
        va_end(ap);
        const int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0)
            return -1;
        return fcntl(fd, F_SETFL, (on && *on) ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
    }
    errno = ENOSYS;
    return -1;
}

#ifdef __cplusplus
}
#endif
