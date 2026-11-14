/* For the loader's tests: how many lines of the system log contain a text.
 * A SUN program reads the log with sxn_log_read (docs/kernel/SYSTEM_LOG.md). */
#ifndef SAVANXP_LOADER_TESTS_LOGLINES_H
#define SAVANXP_LOADER_TESTS_LOGLINES_H

#include <stdint.h>

#include "sxn/sxn.h"

static int log_count(const char* text) {
    static struct sxn_log_record records[32];
    uint64_t next = 1;
    int found = 0;
    for (;;) {
        const long count = sxn_log_read(next, records, sizeof(records) / sizeof(records[0]));
        if (count <= 0) {
            return found;
        }
        for (long index = 0; index < count; ++index) {
            for (const char* at = records[index].text; *at != '\0'; ++at) {
                const char* a = at;
                const char* b = text;
                while (*b != '\0' && *a == *b) {
                    a++;
                    b++;
                }
                if (*b == '\0') {
                    found++;
                    break;
                }
            }
            next = records[index].sequence + 1;
        }
    }
}

#endif
