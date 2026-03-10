#pragma once

#include <stddef.h>
#include <stdint.h>

struct sx_FILE;

int sx_parse_int(const char *text, int *result);
int sx_make_dirs(const char *path);
void sx_register_shutdown(void (*callback)(void));
int *sx_errno_location(void);
