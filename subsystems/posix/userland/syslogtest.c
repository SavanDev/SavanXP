/* syslog(3) for a POSIX program (docs/kernel/SYSTEM_LOG.md): a line reaches the
 * system log through SXN_SYS_LOG even with no standard output or error to
 * carry it.
 *
 * Both are closed first, so the only way "SYSLOG TEST PASS" can reach the
 * serial port -- which the scenario reads -- is the system log. The kernel
 * prefixes it "posix[pid]: ", the ident and the priority word follow. */

#include <syslog.h>

#include "libc.h"

int main(void) {
    (void)savanxp_close(1);
    (void)savanxp_close(2);
    openlog("syslogtest", 0, LOG_USER);
    /* Masked out: must not appear. */
    (void)setlogmask(LOG_UPTO(LOG_WARNING));
    syslog(LOG_INFO, "SYSLOG TEST FAIL: a masked priority was logged");
    syslog(LOG_WARNING, "SYSLOG TEST %s", "PASS");
    closelog();
    return 0;
}
