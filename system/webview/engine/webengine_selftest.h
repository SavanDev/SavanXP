#pragma once

/* The engine's self-test (webengine_selftest.c), shared by the host test and
 * the SUN program enginetest. */

#include <stddef.h>
#include <stdint.h>

/* One line per failed check. */
typedef void (*webengine_report)(void *user, const char *line);

/* Runs every check, between webengine_init and webengine_fini of its own;
 * the number that failed. */
int webengine_selftest(webengine_report report, void *user);

/* Parse one page, style it with its own <style> sheets, and check that every
 * element of the DOM was styled; `elements` gets how many. Needs
 * webengine_init. 0, -1 when the libraries failed, -2 when the walk and the
 * DOM disagree. */
int webengine_check_page(const uint8_t *data, size_t length, const char *url, unsigned *elements);
