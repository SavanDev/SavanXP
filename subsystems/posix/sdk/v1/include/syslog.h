/* syslog(3): a line in the system log (docs/kernel/SYSTEM_LOG.md), the one Log
 * Viewer shows and the serial port carries. Built on SXN_SYS_LOG, the native
 * call a SUN program reaches as sxn_log; the kernel prefixes the line with
 * "posix[pid]: " and records the program's name as its source.
 *
 * The system log has no facilities and no priorities: the facility is
 * ignored, and the priority shows only as a word before a warning or worse. A
 * line is cut at 255 bytes. */
#ifndef SAVANXP_SYSLOG_H
#define SAVANXP_SYSLOG_H

#define LOG_EMERG 0
#define LOG_ALERT 1
#define LOG_CRIT 2
#define LOG_ERR 3
#define LOG_WARNING 4
#define LOG_NOTICE 5
#define LOG_INFO 6
#define LOG_DEBUG 7
#define LOG_PRIMASK 0x07
#define LOG_PRI(priority) ((priority) & LOG_PRIMASK)

#define LOG_KERN (0 << 3)
#define LOG_USER (1 << 3)
#define LOG_DAEMON (3 << 3)
#define LOG_LOCAL0 (16 << 3)

#define LOG_PID 0x01
#define LOG_CONS 0x02
#define LOG_ODELAY 0x04
#define LOG_NDELAY 0x08
#define LOG_NOWAIT 0x10
#define LOG_PERROR 0x20

#define LOG_MASK(priority) (1 << (priority))
#define LOG_UPTO(priority) ((1 << ((priority) + 1)) - 1)

#include <stdarg.h>

void openlog(const char* ident, int option, int facility);
void syslog(int priority, const char* format, ...);
void vsyslog(int priority, const char* format, va_list args);
void closelog(void);
int setlogmask(int mask);

#endif
