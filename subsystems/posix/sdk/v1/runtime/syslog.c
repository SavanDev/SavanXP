/* syslog(3) over SXN_SYS_LOG (see <syslog.h>). The kernel adds the pid and the
 * program's name, so LOG_PID adds nothing; the ident, when openlog gave one,
 * starts the line as on other systems. LOG_PERROR also writes the line to
 * standard error. */

#include <stdarg.h>
#include <stdio.h>
#include <syslog.h>

#include "savanxp/libc.h"
#include "sxn/sxn.h"

static const char* g_ident = 0;
static int g_option = 0;
static int g_mask = 0xff;

void openlog(const char* ident, int option, int facility) {
    (void)facility;
    g_ident = ident;
    g_option = option;
}

void closelog(void) {
    g_ident = 0;
    g_option = 0;
}

int setlogmask(int mask) {
    const int previous = g_mask;
    if (mask != 0) {
        g_mask = mask;
    }
    return previous;
}

void vsyslog(int priority, const char* format, va_list args) {
    static const char* const kLevel[] = {"emergency: ", "alert: ", "critical: ", "error: ", "warning: "};
    const int level = LOG_PRI(priority);
    char line[256];
    int used = 0;

    if ((g_mask & LOG_MASK(level)) == 0) {
        return;
    }
    if (g_ident != 0) {
        used = snprintf(line, sizeof(line), "%s: ", g_ident);
    }
    if (level <= LOG_WARNING && used >= 0 && used < (int)sizeof(line)) {
        used += snprintf(line + used, sizeof(line) - (size_t)used, "%s", kLevel[level]);
    }
    if (used < 0 || used >= (int)sizeof(line)) {
        used = 0;
    }
    (void)vsnprintf(line + used, sizeof(line) - (size_t)used, format, args);
    /* One line: the system log breaks at a line break, and a trailing one
     * would leave an empty record. */
    for (char* ch = line; *ch != '\0'; ++ch) {
        if (*ch == '\n') {
            *ch = (ch[1] == '\0') ? '\0' : ' ';
            if (*ch == '\0') {
                break;
            }
        }
    }
    (void)sxn_log(line);
    if ((g_option & LOG_PERROR) != 0) {
        eprintf("%s\n", line);
    }
}

void syslog(int priority, const char* format, ...) {
    va_list args;
    va_start(args, format);
    vsyslog(priority, format, args);
    va_end(args);
}
