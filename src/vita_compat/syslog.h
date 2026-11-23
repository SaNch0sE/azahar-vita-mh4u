/* syslog.h for the PS Vita.
 *
 * There is no system log on the console and its newlib ships no <syslog.h>. libressl's compat
 * header includes it unconditionally; its one caller, OPENSSL_showfatal, is patched to write to
 * stderr on this platform (patches/vita/libressl-vita.patch). Only the names are given here.
 */

#pragma once

#define LOG_EMERG 0
#define LOG_ALERT 1
#define LOG_CRIT 2
#define LOG_ERR 3
#define LOG_WARNING 4
#define LOG_NOTICE 5
#define LOG_INFO 6
#define LOG_DEBUG 7

#define LOG_USER (1 << 3)
#define LOG_LOCAL2 (18 << 3)
