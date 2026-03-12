#pragma once

int sx_parse_int(const char *text, int *result);
int sx_make_dirs(const char *path);
void sx_register_shutdown(void (*callback)(void));
void sx_shutdown_exit(int code) __attribute__((noreturn));
