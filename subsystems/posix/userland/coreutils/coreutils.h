#pragma once

/*
 * Shared helpers for the small file utilities in /bin (cat, cp, echo, ls,
 * mkdir, mv, rm, ...). They replaced the BusyBox port; each one stays a
 * separate program so a broken utility only breaks itself.
 */

#include <stddef.h>

/* Prints "<program>: <what> '<path>': <strerror(errno)>" to stderr. */
void cu_fail(const char* program, const char* what, const char* path);

/* The last path component, ignoring trailing slashes ("a/b/" -> "b"). Returns
 * a static buffer, valid until the next call. */
const char* cu_basename(const char* path);

/* Writes "<directory>/<name>" into out. 0 on success, -1 if it does not fit. */
int cu_join(char* out, size_t capacity, const char* directory, const char* name);

/* 1 if path exists and is a directory. */
int cu_is_directory(const char* path);

/* Writes all of buffer to fd, retrying short writes. 0 on success. */
int cu_write_all(int fd, const void* buffer, size_t length);

/* Copies the whole of in_fd to out_fd. 0 on success. */
int cu_copy_fd(int in_fd, int out_fd);
