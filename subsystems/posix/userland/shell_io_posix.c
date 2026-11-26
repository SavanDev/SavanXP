#include "shell/shell_io.h"

#include "libc.h"

#include <dirent.h>
#include <string.h>
#include <unistd.h>

/* The POSIX side of the shell's I/O (shell/shell_io.h): what Program Manager
 * and Files link. The calls are the ones the shared modules made themselves
 * before they lost their system calls, so they behave as they did. */

long shell_io_read_file(const char *path, void *buffer, size_t capacity)
{
    long fd = savanxp_open_mode(path, SAVANXP_OPEN_READ);
    size_t total = 0;

    if (fd < 0)
    {
        return fd;
    }
    while (total < capacity)
    {
        long got = savanxp_read((int)fd, (char *)buffer + total, capacity - total);

        if (got < 0)
        {
            (void)savanxp_close((int)fd);
            return got;
        }
        if (got == 0)
        {
            break;
        }
        total += (size_t)got;
    }
    (void)savanxp_close((int)fd);
    return (long)total;
}

long shell_io_write_file(const char *path, const void *data, size_t size)
{
    long fd = savanxp_open_mode(path, SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE | SAVANXP_OPEN_TRUNCATE);
    long written;

    if (fd < 0)
    {
        return fd;
    }
    written = size != 0 ? savanxp_write((int)fd, data, size) : 0;
    (void)savanxp_close((int)fd);
    if (written < 0)
    {
        return written;
    }
    return written == (long)size ? 0 : -SAVANXP_EIO;
}

long shell_io_query(const char *path, struct shell_io_info *out)
{
    struct savanxp_stat_ex info;
    long result;

    memset(&info, 0, sizeof(info));
    result = savanxp_stat_ex(path, &info);
    if (result < 0)
    {
        return result;
    }
    switch (info.st_mode & SAVANXP_S_IFMT)
    {
        case SAVANXP_S_IFREG:
            out->type = SHELL_IO_FILE;
            break;
        case SAVANXP_S_IFDIR:
            out->type = SHELL_IO_DIRECTORY;
            break;
        default:
            out->type = SHELL_IO_OTHER;
            break;
    }
    out->size = info.st_size;
    out->mtime = info.st_mtime;
    return 0;
}

long shell_io_list(const char *directory, shell_io_entry_fn each, void *context)
{
    DIR *handle = opendir(directory);
    struct dirent *entry;

    if (handle == 0)
    {
        return -SAVANXP_ENOENT;
    }
    while ((entry = readdir(handle)) != 0)
    {
        each(entry->d_name, context);
    }
    closedir(handle);
    return 0;
}

long shell_io_make_directory(const char *path)
{
    return savanxp_mkdir(path);
}

long shell_io_delete_file(const char *path)
{
    return savanxp_unlink(path);
}

long shell_io_delete_directory(const char *path)
{
    return savanxp_rmdir(path);
}

long shell_io_rename(const char *old_path, const char *new_path)
{
    return savanxp_rename(old_path, new_path);
}

const char *shell_io_profile_directory(void)
{
    return savanxp_profile_dir();
}

long shell_io_settings_path(const char *name, int for_write, char *out, size_t capacity)
{
    return savanxp_settings_path(name, for_write, out, capacity);
}

long shell_io_launch(const char *program, const char *argument, const char *workdir)
{
    char saved_directory[SAVANXP_DESKTOP_LAUNCH_PATH_CAPACITY];
    int changed_directory = 0;
    long pid;

    /* The program inherits the working directory at spawn, so the caller's
     * is swapped for the one asked for around the launch. */
    if (workdir != 0 && workdir[0] != '\0' && getcwd(saved_directory, sizeof(saved_directory)) != 0 &&
        chdir(workdir) == 0)
    {
        changed_directory = 1;
    }
    pid = gfx_desktop_launch_arg(0, program, argument, SAVANXP_DESKTOP_LAUNCH_FLAG_NONE);
    if (changed_directory)
    {
        (void)chdir(saved_directory);
    }
    return pid;
}
