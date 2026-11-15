#include "file_copy.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* See file_copy.h. The walk keeps one open directory per level and the two
 * paths it is at, `from` and `to`, which grow by "/<name>" for the entry being
 * worked on and shrink back to the level's own length when it is done. */

int file_copy_join(char *out, size_t capacity, const char *directory, const char *name)
{
    int written;

    if (out == 0 || capacity == 0 || directory == 0 || name == 0)
    {
        return -1;
    }
    written = strcmp(directory, "/") == 0 ? snprintf(out, capacity, "/%s", name)
                                          : snprintf(out, capacity, "%s/%s", directory, name);
    return written >= 0 && (size_t)written < capacity ? 0 : -1;
}

int file_copy_running(const struct file_copy *op)
{
    return op != 0 && (op->state == FILE_COPY_COUNTING || op->state == FILE_COPY_COPYING ||
                       op->state == FILE_COPY_REMOVING);
}

static void file_copy_set_path(char *field, const char *path)
{
    snprintf(field, FILE_COPY_PATH_CAPACITY, "%s", path);
}

/* Both paths back to the directory the walk is in (the root's parent when the
 * walk is at the root). */
static void file_copy_trim(struct file_copy *op)
{
    if (op->depth > 0)
    {
        op->from[op->levels[op->depth - 1].from_length] = '\0';
        op->to[op->levels[op->depth - 1].to_length] = '\0';
    }
}

static void file_copy_close_files(struct file_copy *op, int remove_partial)
{
    if (op->in_fd >= 0)
    {
        close(op->in_fd);
        op->in_fd = -1;
    }
    if (op->out_fd >= 0)
    {
        close(op->out_fd);
        op->out_fd = -1;
        if (remove_partial)
        {
            (void)unlink(op->to);
        }
    }
}

static void file_copy_close_levels(struct file_copy *op)
{
    while (op->depth > 0)
    {
        op->depth -= 1;
        if (op->levels[op->depth].directory != 0)
        {
            closedir(op->levels[op->depth].directory);
            op->levels[op->depth].directory = 0;
        }
    }
}

/* Opens the directory at `from` as a new level. */
static int file_copy_push(struct file_copy *op)
{
    DIR *directory;

    if (op->depth >= FILE_COPY_MAX_DEPTH)
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    directory = opendir(op->from);
    if (directory == 0)
    {
        return -1;
    }
    op->levels[op->depth].directory = directory;
    op->levels[op->depth].from_length = strlen(op->from);
    op->levels[op->depth].to_length = strlen(op->to);
    op->depth += 1;
    return 0;
}

static void file_copy_pop(struct file_copy *op)
{
    if (op->depth <= 0)
    {
        return;
    }
    op->depth -= 1;
    closedir(op->levels[op->depth].directory);
    op->levels[op->depth].directory = 0;
    file_copy_trim(op);
}

/* Appends "/<name>" to `path`, which is `length` long. */
static int file_copy_append(char *path, size_t length, const char *name)
{
    int written = length == 1 && path[0] == '/' ? snprintf(path + length, FILE_COPY_PATH_CAPACITY - length, "%s", name)
                                                : snprintf(path + length, FILE_COPY_PATH_CAPACITY - length, "/%s", name);
    return written >= 0 && (size_t)written < FILE_COPY_PATH_CAPACITY - length ? 0 : -1;
}

/* The next entry of the deepest level, with `from` and `to` extended to it.
 * 1 with an entry, 0 when the level is exhausted, -1 when the names do not
 * fit (errno ENAMETOOLONG; the paths then name the level itself). */
static int file_copy_next(struct file_copy *op)
{
    struct file_copy_level *level = &op->levels[op->depth - 1];
    struct dirent *entry;

    for (;;)
    {
        entry = readdir(level->directory);
        if (entry == 0)
        {
            return 0;
        }
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
        {
            break;
        }
    }
    if (file_copy_append(op->from, level->from_length, entry->d_name) != 0 ||
        file_copy_append(op->to, level->to_length, entry->d_name) != 0)
    {
        file_copy_trim(op);
        errno = ENAMETOOLONG;
        return -1;
    }
    return 1;
}

/* A failure on the entry at `from`: reported, then either skipped
 * (FILE_COPY_KEEP_GOING, below the root) or the end of the whole copy. */
static int file_copy_fail(struct file_copy *op, const char *what)
{
    int error = errno != 0 ? errno : EIO;

    if (op->report != 0)
    {
        op->report(op->from, what, error, op->user);
    }
    file_copy_close_files(op, 1);
    if ((op->flags & FILE_COPY_KEEP_GOING) != 0 && op->depth > 0 && op->state != FILE_COPY_REMOVING)
    {
        op->failures += 1;
        file_copy_trim(op);
        return 1;
    }
    file_copy_set_path(op->failed_path, op->from);
    op->error = error;
    file_copy_close_levels(op);
    op->state = FILE_COPY_FAILED;
    return -error;
}

int file_copy_begin(struct file_copy *op, const char *from, const char *to, unsigned int flags)
{
    void (*report)(const char *, const char *, int, void *);
    void *user;
    struct stat info;
    size_t from_length;

    if (op == 0 || from == 0 || to == 0 || from[0] == '\0' || to[0] == '\0' ||
        strlen(from) >= FILE_COPY_PATH_CAPACITY || strlen(to) >= FILE_COPY_PATH_CAPACITY)
    {
        return -EINVAL;
    }
    report = op->report;
    user = op->user;
    memset(op, 0, sizeof(*op));
    op->report = report;
    op->user = user;
    op->flags = flags;
    op->in_fd = -1;
    op->out_fd = -1;

    if (lstat(from, &info) != 0)
    {
        return -errno;
    }
    /* A folder copied into itself would find its own copy in the walk and
     * copy it again, without end. */
    from_length = strlen(from);
    if (strcmp(from, to) == 0 || strcmp(from, "/") == 0 ||
        (strncmp(to, from, from_length) == 0 && to[from_length] == '/'))
    {
        return -EINVAL;
    }
    if ((flags & FILE_COPY_MERGE) == 0)
    {
        struct stat existing;

        if (lstat(to, &existing) == 0)
        {
            return -EEXIST;
        }
    }

    file_copy_set_path(op->root_from, from);
    file_copy_set_path(op->root_to, to);
    file_copy_set_path(op->from, from);
    file_copy_set_path(op->to, to);
    file_copy_set_path(op->current, from);
    op->root_is_directory = S_ISDIR(info.st_mode);

    if ((flags & FILE_COPY_MOVE) != 0)
    {
        if (rename(from, to) == 0)
        {
            op->renamed = 1;
            op->total_items = 1;
            op->done_items = 1;
            op->state = FILE_COPY_DONE;
            return 0;
        }
        if (errno != EXDEV)
        {
            return -errno;
        }
    }
    op->state = (flags & FILE_COPY_NO_COUNT) != 0 ? FILE_COPY_COPYING : FILE_COPY_COUNTING;
    return 0;
}

/* ---- counting --------------------------------------------------------------- */

static void file_copy_start_copying(struct file_copy *op)
{
    file_copy_close_levels(op);
    file_copy_set_path(op->from, op->root_from);
    file_copy_set_path(op->to, op->root_to);
    op->root_started = 0;
    op->state = FILE_COPY_COPYING;
}

/* What cannot be read here is not counted; the copying walk meets it again
 * and reports it. */
static int file_copy_count_step(struct file_copy *op)
{
    struct stat info;
    int next;

    if (!op->root_started)
    {
        op->root_started = 1;
        op->total_items = 1;
        if (lstat(op->from, &info) == 0 && !S_ISDIR(info.st_mode))
        {
            op->total_bytes = (unsigned long long)info.st_size;
        }
        if (!op->root_is_directory || file_copy_push(op) != 0)
        {
            file_copy_start_copying(op);
        }
        return 1;
    }
    if (op->depth == 0)
    {
        file_copy_start_copying(op);
        return 1;
    }
    next = file_copy_next(op);
    if (next == 0)
    {
        file_copy_pop(op);
        if (op->depth == 0)
        {
            file_copy_start_copying(op);
        }
        return 1;
    }
    if (next < 0)
    {
        return 1;
    }
    op->total_items += 1;
    if (lstat(op->from, &info) == 0)
    {
        if (S_ISDIR(info.st_mode))
        {
            if (file_copy_push(op) == 0)
            {
                return 1;
            }
        }
        else
        {
            op->total_bytes += (unsigned long long)info.st_size;
        }
    }
    file_copy_trim(op);
    return 1;
}

/* ---- copying ------------------------------------------------------------------ */

static int file_copy_make_directory(struct file_copy *op, const struct stat *info)
{
    struct stat existing;

    /* The owner must be able to fill it, whatever the source's mode. */
    if (mkdir(op->to, (mode_t)((info->st_mode & 0777) | 0700)) == 0)
    {
        return 0;
    }
    if ((op->flags & FILE_COPY_MERGE) != 0 && errno == EEXIST && stat(op->to, &existing) == 0 &&
        S_ISDIR(existing.st_mode))
    {
        return 0;
    }
    return -1;
}

static int file_copy_open_files(struct file_copy *op, const struct stat *info)
{
    int create = O_WRONLY | O_CREAT | O_TRUNC | ((op->flags & FILE_COPY_MERGE) != 0 ? 0 : O_EXCL);

    file_copy_set_path(op->current, op->from);
    op->in_fd = open(op->from, O_RDONLY);
    if (op->in_fd < 0)
    {
        return file_copy_fail(op, "open");
    }
    op->out_fd = open(op->to, create, (mode_t)(info->st_mode & 0777));
    if (op->out_fd < 0)
    {
        /* Nothing of ours to remove: the create failed. */
        int error = errno;
        close(op->in_fd);
        op->in_fd = -1;
        errno = error;
        return file_copy_fail(op, "create");
    }
    return 1;
}

/* One buffer of the open file; at its end, the file counts as done. */
static int file_copy_pump(struct file_copy *op)
{
    long count = read(op->in_fd, op->buffer, sizeof(op->buffer));
    long written = 0;

    if (count < 0)
    {
        return file_copy_fail(op, "read");
    }
    if (count == 0)
    {
        int closed;

        close(op->in_fd);
        op->in_fd = -1;
        closed = close(op->out_fd);
        op->out_fd = -1;
        if (closed != 0)
        {
            /* The data may not have reached the disk: the file is not a
             * copy. */
            int error = errno;
            (void)unlink(op->to);
            errno = error;
            return file_copy_fail(op, "write");
        }
        op->done_items += 1;
        file_copy_trim(op);
        return 1;
    }
    while (written < count)
    {
        long result = write(op->out_fd, op->buffer + written, (size_t)(count - written));

        if (result <= 0)
        {
            if (result == 0)
            {
                errno = EIO;
            }
            return file_copy_fail(op, "write");
        }
        written += result;
    }
    op->done_bytes += (unsigned long long)count;
    return 1;
}

static int file_copy_enter(struct file_copy *op, const struct stat *info)
{
    file_copy_set_path(op->current, op->from);
    if (file_copy_make_directory(op, info) != 0)
    {
        return file_copy_fail(op, "create folder");
    }
    op->done_items += 1;
    if (file_copy_push(op) != 0)
    {
        return file_copy_fail(op, "read folder");
    }
    return 1;
}

static int file_copy_copy_step(struct file_copy *op)
{
    struct stat info;
    int next;

    if (op->in_fd >= 0)
    {
        return file_copy_pump(op);
    }
    if (!op->root_started)
    {
        op->root_started = 1;
        if (lstat(op->from, &info) != 0)
        {
            return file_copy_fail(op, "open");
        }
        return S_ISDIR(info.st_mode) ? file_copy_enter(op, &info) : file_copy_open_files(op, &info);
    }
    if (op->depth == 0)
    {
        if ((op->flags & FILE_COPY_MOVE) != 0)
        {
            file_copy_set_path(op->from, op->root_from);
            op->root_started = 0;
            op->state = FILE_COPY_REMOVING;
            return 1;
        }
        op->state = FILE_COPY_DONE;
        return 0;
    }
    next = file_copy_next(op);
    if (next == 0)
    {
        file_copy_pop(op);
        return 1;
    }
    if (next < 0)
    {
        return file_copy_fail(op, "open");
    }
    if (lstat(op->from, &info) != 0)
    {
        return file_copy_fail(op, "open");
    }
    return S_ISDIR(info.st_mode) ? file_copy_enter(op, &info) : file_copy_open_files(op, &info);
}

/* ---- removing a moved source -------------------------------------------------- */

/* Children first, then their folder, listing each folder once: deleting an
 * entry right after reading it is safe, the kernel moves the listing past an
 * entry removed under it (as rm relies on). */
static int file_copy_remove_step(struct file_copy *op)
{
    struct stat info;
    int next;

    if (!op->root_started)
    {
        op->root_started = 1;
        if (!op->root_is_directory)
        {
            if (unlink(op->from) != 0)
            {
                return file_copy_fail(op, "remove");
            }
            op->state = FILE_COPY_DONE;
            return 0;
        }
        if (file_copy_push(op) != 0)
        {
            return file_copy_fail(op, "remove");
        }
        return 1;
    }
    if (op->depth == 0)
    {
        op->state = FILE_COPY_DONE;
        return 0;
    }
    next = file_copy_next(op);
    if (next == 0)
    {
        struct file_copy_level *level = &op->levels[op->depth - 1];

        closedir(level->directory);
        level->directory = 0;
        op->from[level->from_length] = '\0';
        op->depth -= 1;
        if (rmdir(op->from) != 0)
        {
            return file_copy_fail(op, "remove");
        }
        file_copy_trim(op);
        return 1;
    }
    if (next < 0)
    {
        return file_copy_fail(op, "remove");
    }
    if (lstat(op->from, &info) == 0 && S_ISDIR(info.st_mode))
    {
        if (file_copy_push(op) != 0)
        {
            return file_copy_fail(op, "remove");
        }
        return 1;
    }
    if (unlink(op->from) != 0)
    {
        return file_copy_fail(op, "remove");
    }
    file_copy_trim(op);
    return 1;
}

int file_copy_step(struct file_copy *op)
{
    if (op == 0)
    {
        return -EINVAL;
    }
    errno = 0;
    switch (op->state)
    {
    case FILE_COPY_COUNTING:
        return file_copy_count_step(op);
    case FILE_COPY_COPYING:
        return file_copy_copy_step(op);
    case FILE_COPY_REMOVING:
        return file_copy_remove_step(op);
    case FILE_COPY_DONE:
        return 0;
    case FILE_COPY_FAILED:
        return -op->error;
    default:
        return -EINVAL;
    }
}

void file_copy_cancel(struct file_copy *op)
{
    if (!file_copy_running(op))
    {
        return;
    }
    file_copy_close_files(op, 1);
    file_copy_close_levels(op);
    op->state = FILE_COPY_CANCELLED;
}

int file_copy_free_name(const char *directory, const char *name, char *out, size_t capacity)
{
    char candidate[FILE_COPY_PATH_CAPACITY];
    char path[FILE_COPY_PATH_CAPACITY];
    struct stat info;
    int attempt;

    if (directory == 0 || name == 0 || out == 0 || name[0] == '\0')
    {
        return -1;
    }
    for (attempt = 0; attempt < 100; ++attempt)
    {
        int written = attempt == 0   ? snprintf(candidate, sizeof(candidate), "%s", name)
                      : attempt == 1 ? snprintf(candidate, sizeof(candidate), "Copy of %s", name)
                                     : snprintf(candidate, sizeof(candidate), "Copy (%d) of %s", attempt, name);

        if (written < 0 || (size_t)written >= sizeof(candidate) || (size_t)written >= capacity ||
            file_copy_join(path, sizeof(path), directory, candidate) != 0)
        {
            return -1;
        }
        if (lstat(path, &info) != 0)
        {
            snprintf(out, capacity, "%s", candidate);
            return 0;
        }
    }
    return -1;
}
