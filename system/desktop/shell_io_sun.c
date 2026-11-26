#include "shell/shell_io.h"

#include "sxn/sxn.h"
#include "sxn/sxnprofile.h"

#include <string.h>

/* The SUN side of the shell's I/O (shell/shell_io.h): what the desktop links.
 * It answers as shell_io_posix.c does for the same file, so the modules in
 * system/shell/ behave the same in the desktop as in Program Manager and
 * Files (docs/desktop/DESKTOP_ARCHITECTURE.md, "The shell's data and its
 * I/O"). */

long shell_io_read_file(const char *path, void *buffer, size_t capacity)
{
    long handle = sxn_open_file(path, SXN_FILE_READ);
    size_t total = 0;

    if (handle < 0)
    {
        return handle;
    }
    while (total < capacity)
    {
        long got = sxn_read(handle, (char *)buffer + total, capacity - total);

        if (got < 0)
        {
            (void)sxn_close(handle);
            return got;
        }
        if (got == 0)
        {
            break;
        }
        total += (size_t)got;
    }
    (void)sxn_close(handle);
    return (long)total;
}

long shell_io_write_file(const char *path, const void *data, size_t size)
{
    long handle = sxn_create_file(path, SXN_CREATE_ALWAYS);
    long written = 0;

    if (handle < 0)
    {
        return handle;
    }
    if (size != 0)
    {
        written = sxn_write(handle, data, size);
    }
    (void)sxn_close(handle);
    if (written < 0)
    {
        return written;
    }
    return written == (long)size ? 0 : -SAVANXP_EIO;
}

long shell_io_query(const char *path, struct shell_io_info *out)
{
    struct sxn_file_info info;
    long result;

    memset(&info, 0, sizeof(info));
    result = sxn_query_path(path, &info);
    if (result < 0)
    {
        return result;
    }
    switch (info.type)
    {
        case SXN_FILE_TYPE_FILE:
            out->type = SHELL_IO_FILE;
            break;
        case SXN_FILE_TYPE_DIRECTORY:
            out->type = SHELL_IO_DIRECTORY;
            break;
        default:
            out->type = SHELL_IO_OTHER;
            break;
    }
    out->size = info.size;
    out->mtime = info.mtime;
    return 0;
}

long shell_io_list(const char *directory, shell_io_entry_fn each, void *context)
{
    /* Static: an entry carries a 256-byte name, and the listing never nests
     * (the callbacks read files, they do not list). */
    static struct sxn_directory_entry entry;
    long handle = sxn_open_directory(directory);

    if (handle < 0)
    {
        return handle;
    }
    while (sxn_read_directory(handle, &entry) > 0)
    {
        entry.name[sizeof(entry.name) - 1u] = '\0';
        each(entry.name, context);
    }
    (void)sxn_close(handle);
    return 0;
}

long shell_io_make_directory(const char *path)
{
    return sxn_make_directory(path);
}

long shell_io_delete_file(const char *path)
{
    return sxn_delete_path(path);
}

long shell_io_delete_directory(const char *path)
{
    return sxn_delete_directory(path);
}

long shell_io_rename(const char *old_path, const char *new_path)
{
    return sxn_rename_path(old_path, new_path);
}

const char *shell_io_profile_directory(void)
{
    return sxn_profile_directory();
}

long shell_io_settings_path(const char *name, int for_write, char *out, size_t capacity)
{
    return sxn_settings_path(name, for_write, out, capacity);
}

/* The programs this process started and has not reaped yet. One that does
 * not fit goes untracked and is reaped when the desktop exits, as an orphan
 * is; the POSIX runtime keeps the same table (gfx_desktop_launch_arg). */
#define SHELL_IO_LAUNCHED_CAPACITY 32
static long g_launched[SHELL_IO_LAUNCHED_CAPACITY];
static int g_launched_count = 0;

/* Reaps the ones that ended, so they do not stay zombies under the desktop. */
static void reap_launched(void)
{
    int index = 0;

    while (index < g_launched_count)
    {
        struct sxn_process_query query;

        memset(&query, 0, sizeof(query));
        if (sxn_process_query(g_launched[index], &query) == 0 && query.state == SXN_PROCESS_RUNNING)
        {
            ++index;
            continue;
        }
        if (query.state == SXN_PROCESS_ENDED)
        {
            int status = 0;

            (void)sxn_waitpid(g_launched[index], &status);
        }
        g_launched[index] = g_launched[--g_launched_count];
    }
}

long shell_io_launch(const char *program, const char *argument, const char *workdir)
{
    struct sxn_spawn_request request;
    const char *directories[3];
    char *argv[3];
    int argc = 1;
    int attempt;
    long pid = -SAVANXP_ENOENT;

    if (program == 0 || program[0] != '/' || strlen(program) >= SAVANXP_DESKTOP_LAUNCH_PATH_CAPACITY ||
        (argument != 0 && strlen(argument) >= SAVANXP_DESKTOP_LAUNCH_ARG_CAPACITY))
    {
        return -SAVANXP_EINVAL;
    }
    argv[0] = (char *)program;
    argv[1] = 0;
    argv[2] = 0;
    if (argument != 0 && argument[0] != '\0')
    {
        argv[1] = (char *)argument;
        argc = 2;
    }
    /* A SUN caller has no current directory to pass on. The program begins
     * in the shortcut's working directory; without one, or when that one
     * cannot be entered, in the person's profile, where the POSIX desktop
     * stood, so a file it saves under a bare name lands there and not at the
     * root, which the person may not write; and if even that fails, at the
     * root. Only a POSIX program keeps the directory (slice 32). */
    directories[0] = workdir;
    directories[1] = sxn_profile_directory();
    directories[2] = 0;

    reap_launched();
    for (attempt = 0; attempt < 3; ++attempt)
    {
        const char *directory = directories[attempt];

        if (attempt < 2 && (directory == 0 || directory[0] == '\0'))
        {
            continue;
        }
        sxn_spawn_request_init(&request, program, argv, argc);
        pid = directory != 0 ? sxn_spawn_ex_in(&request, directory) : sxn_spawn_ex(&request);
        /* Only a directory that would not do is worth another attempt. The
         * same errors from a missing program fail all three alike. */
        if (pid != -SAVANXP_ENOENT && pid != -SAVANXP_ENOTDIR && pid != -SAVANXP_EACCES)
        {
            break;
        }
    }
    if (pid > 0 && g_launched_count < SHELL_IO_LAUNCHED_CAPACITY)
    {
        g_launched[g_launched_count++] = pid;
    }
    return pid;
}
