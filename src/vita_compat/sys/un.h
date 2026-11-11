/* sys/un.h for the PS Vita.
 *
 * The console's newlib defines AF_UNIX in <sys/socket.h> but ships no <sys/un.h>, and the console
 * has no local sockets. Boost.Asio and cpp-httplib include it for their local-socket endpoints,
 * which this build never opens; they need only the address structure to compile. It is laid out
 * as BSD's, like the console's struct sockaddr: a length byte before the family.
 */

#pragma once

#include <sys/socket.h>

struct sockaddr_un {
    uint8_t sun_len;
    sa_family_t sun_family;
    char sun_path[104];
};
