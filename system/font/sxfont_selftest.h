/* libsxfont's self-test (docs/desktop/FONT_LIBRARY.md, "Verification"):
 * the same checks on the build host (sxfont-test) and on SavanXP (fonttest,
 * the font-test scenario), over the Liberation faces the system ships. */
#pragma once

#include <stddef.h>

/* The bytes of one of the twelve Liberation files, by file name
 * ("LiberationSans-Regular.ttf"), or NULL. They stay valid until the test
 * returns. */
typedef const void *(*sxfont_selftest_load)(void *user, const char *name, size_t *size);
/* One failed check, as a line of English. */
typedef void (*sxfont_selftest_report)(void *user, const char *line);

/* Runs every check; returns the number that failed. */
int sxfont_selftest(sxfont_selftest_load load, sxfont_selftest_report report, void *user);
