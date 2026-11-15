#pragma once

/*
 * Copying and moving a file or a folder tree a piece at a time
 * (docs/desktop/FILES.md, "Cut, Copy and Paste").
 *
 * A program with a window cannot copy a big folder in one call: its loop
 * would stop for as long as the copy takes. file_copy_step does one small
 * unit of work -- one directory entry, or one buffer of a file -- and
 * returns, so the caller interleaves the steps with its own loop and shows
 * the progress the struct keeps. A program without a window (cp) just calls
 * it until it is done.
 *
 * A copy first walks the source to count what it holds, so the progress has
 * a total, then walks it again copying. A move is a rename when both ends
 * are on one volume; across volumes (EXDEV) it is a copy, and the source is
 * removed only after all of it has been copied, so a failure or a cancel
 * never loses the only copy of anything.
 */

#include <dirent.h>
#include <stddef.h>

#define FILE_COPY_PATH_CAPACITY 256
/* Folders deeper than this below the source are refused (ENAMETOOLONG long
 * before, in practice: a path is at most FILE_COPY_PATH_CAPACITY). */
#define FILE_COPY_MAX_DEPTH 32
#define FILE_COPY_BUFFER_SIZE 16384

enum file_copy_flags {
    /* Remove the source once copied; try a rename first. */
    FILE_COPY_MOVE = 1u << 0,
    /* `to` may exist: a folder is filled, a file is overwritten (cp's
     * behaviour). Without it an existing `to` is EEXIST. */
    FILE_COPY_MERGE = 1u << 1,
    /* Skip the counting walk: the totals stay 0 (cp needs no progress). */
    FILE_COPY_NO_COUNT = 1u << 2,
    /* A failure on one entry is reported and skipped instead of ending the
     * whole copy (cp's behaviour). */
    FILE_COPY_KEEP_GOING = 1u << 3,
};

enum file_copy_state {
    FILE_COPY_IDLE = 0,
    FILE_COPY_COUNTING,
    FILE_COPY_COPYING,
    FILE_COPY_REMOVING,
    FILE_COPY_DONE,
    FILE_COPY_FAILED,
    FILE_COPY_CANCELLED,
};

struct file_copy_level {
    DIR *directory;
    size_t from_length;
    size_t to_length;
};

struct file_copy {
    /* What the caller reads. */
    int state;
    unsigned int flags;
    unsigned long long total_bytes;
    unsigned long long done_bytes;
    unsigned int total_items; /* files and folders, the source included */
    unsigned int done_items;
    unsigned int failures;    /* entries skipped under FILE_COPY_KEEP_GOING */
    int renamed;              /* a move that was a single rename */
    char current[FILE_COPY_PATH_CAPACITY]; /* the source path being worked on */
    char failed_path[FILE_COPY_PATH_CAPACITY];
    int error;                /* errno of the failure */

    /* Optional: called for every failure, before the copy stops or, under
     * FILE_COPY_KEEP_GOING, skips the entry. `what` is "open", "create",
     * "read", "write", "create folder", "read folder" or "remove". */
    void (*report)(const char *path, const char *what, int error, void *user);
    void *user;

    /* The engine's. */
    char root_from[FILE_COPY_PATH_CAPACITY];
    char root_to[FILE_COPY_PATH_CAPACITY];
    char from[FILE_COPY_PATH_CAPACITY];
    char to[FILE_COPY_PATH_CAPACITY];
    struct file_copy_level levels[FILE_COPY_MAX_DEPTH];
    int depth;
    int in_fd;
    int out_fd;
    int root_is_directory;
    int root_started;
    unsigned char buffer[FILE_COPY_BUFFER_SIZE];
};

/* Prepares copying (or moving) `from` to the path `to`. Fails without
 * touching anything when `from` does not exist, `to` exists and
 * FILE_COPY_MERGE was not given (EEXIST), or `to` is `from` or inside it
 * (EINVAL). A move that a rename completes is already FILE_COPY_DONE here.
 * Returns 0 or -errno. */
int file_copy_begin(struct file_copy *op, const char *from, const char *to, unsigned int flags);

/* One unit of work. Returns 1 while there is more, 0 once FILE_COPY_DONE,
 * and -errno once FILE_COPY_FAILED (failed_path and error say where). */
int file_copy_step(struct file_copy *op);

/* Stops a copy that is not finished: closes everything, removes the file
 * being written (it would be half a file), and leaves what was already
 * copied. A move's source is untouched unless removing had begun, which only
 * happens after the whole copy succeeded. */
void file_copy_cancel(struct file_copy *op);

/* 1 while begun and not finished. */
int file_copy_running(const struct file_copy *op);

/* The name a paste of `name` into `directory` gets: `name` while it is free,
 * else "Copy of <name>", then "Copy (2) of <name>" and on, as the Explorer
 * names it. Returns 0, or -1 when no free name fits. */
int file_copy_free_name(const char *directory, const char *name, char *out, size_t capacity);

/* "<directory>/<name>", for "/" as well. 0, or -1 when it does not fit. */
int file_copy_join(char *out, size_t capacity, const char *directory, const char *name);
