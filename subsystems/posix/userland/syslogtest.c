/* The system log from a POSIX program (docs/kernel/SYSTEM_LOG.md), with no
 * standard output or error to carry anything: both are closed first, so a
 * line the scenario reads from the serial port can only have come through the
 * system log, under its "posix[pid]: " prefix.
 *
 * With no argument: syslog(3), with a masked priority that must not appear.
 * With --loader: the shared loader's notice. dlopen of libsonamelatest.so.0.1,
 * which is not in /lib, falls back to the newest minor, 0.7, and the loader
 * says so; with standard error gone, the POSIX runtime has to send that line
 * to the system log (sx_loader_report). */

#include <dlfcn.h>
#include <syslog.h>

#include "libc.h"

typedef int (*version_fn)(void);

int main(int argc, char** argv) {
    (void)savanxp_close(1);
    (void)savanxp_close(2);
    openlog("syslogtest", 0, LOG_USER);
    if (argc > 1 && strcmp(argv[1], "--loader") == 0) {
        void* library = dlopen("libsonamelatest.so.0.1", RTLD_NOW);
        version_fn version = library != 0 ? (version_fn)dlsym(library, "soname_latest_version") : 0;
        if (version == 0 || version() != 7) {
            syslog(LOG_ERR, "SYSLOG TEST FAIL: libsonamelatest.so.0.1 did not fall back to 0.7");
            return 1;
        }
        return 0;
    }
    /* Masked out: must not appear. */
    (void)setlogmask(LOG_UPTO(LOG_WARNING));
    syslog(LOG_INFO, "SYSLOG TEST FAIL: a masked priority was logged");
    syslog(LOG_WARNING, "SYSLOG TEST %s", "PASS");
    closelog();
    return 0;
}
