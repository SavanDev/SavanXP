#pragma once

/*
 * What the shell's data asks of the system (docs/desktop/DESKTOP_ARCHITECTURE.md,
 * "The shell's data and its I/O").
 *
 * The Program Manager registry, shortcuts, the Start menu's pins and
 * shortcuts and the file associations (include/shell/, system/shell/) make
 * no system call. Everything they read, write, list or start goes through
 * these functions, and each subsystem links its own: POSIX programs (Program
 * Manager, Files) link subsystems/posix/userland/shell_io_posix.c, the SUN
 * desktop system/desktop/shell_io_sun.c. The two behave the same; that is
 * what lets one copy of the logic serve both.
 *
 * Paths are absolute. Errors are -errno, as everywhere else.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum shell_io_type
{
    SHELL_IO_FILE = 1,
    SHELL_IO_DIRECTORY = 2,
    SHELL_IO_OTHER = 3, /* a device, or anything else that is not a file */
};

struct shell_io_info
{
    uint32_t type;  /* enum shell_io_type */
    uint64_t size;  /* bytes; 0 for a directory */
    uint64_t mtime; /* seconds since 1970 UTC; 0 when none was recorded */
};

/* Reads `path` from its start into `buffer`, up to `capacity` bytes. How many
 * were read (less than capacity at the end of the file), or -errno. */
long shell_io_read_file(const char *path, void *buffer, size_t capacity);

/* Creates or replaces `path` with `size` bytes. 0, or -errno. */
long shell_io_write_file(const char *path, const void *data, size_t size);

/* What is at `path`. 0, or -errno (-ENOENT when nothing is). */
long shell_io_query(const char *path, struct shell_io_info *out);

/* Calls `each` with the name of every entry of `directory`, "." and ".."
 * included when the system lists them; the caller filters. `each` may read
 * other files. 0, or -errno when the directory does not open. */
typedef void (*shell_io_entry_fn)(const char *name, void *context);
long shell_io_list(const char *directory, shell_io_entry_fn each, void *context);

/* Makes a directory of the person's. 0, or -errno (-EEXIST included). */
long shell_io_make_directory(const char *path);

/* Deletes a file, not a directory. 0, or -errno. */
long shell_io_delete_file(const char *path);

/* Deletes an empty directory. 0, or -errno (-ENOTEMPTY included). */
long shell_io_delete_directory(const char *path);

/* Renames within a volume; `new_path` must not exist (the caller checks). 0,
 * or -errno. */
long shell_io_rename(const char *old_path, const char *new_path);

/* The person's profile ("/users/user"), and the path of the setting `name`
 * in it, as savanxp_settings_path and sxn_settings_path find it: `for_write`
 * 0 to read (falling back to a loose or root copy), 1 to write. */
const char *shell_io_profile_directory(void);
long shell_io_settings_path(const char *name, int for_write, char *out, size_t capacity);

/* Starts `program` with `argument` as argv[1] when it is not 0 or empty, in
 * `workdir` when it is not 0 or empty (a shortcut's working directory) and in
 * the caller's otherwise -- for a SUN caller, which has none, the person's
 * profile, where the POSIX desktop stood. The new process gets only standard
 * input, output and error, and the shell reaps it. Its pid, or -errno. */
long shell_io_launch(const char *program, const char *argument, const char *workdir);

#ifdef __cplusplus
}
#endif
