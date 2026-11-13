#include "start_pins.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* See start_pins.h. */

static int ends_with_nocase(const char *text, const char *suffix)
{
    size_t length = strlen(text);
    size_t suffix_length = strlen(suffix);

    if (length < suffix_length)
    {
        return 0;
    }
    for (size_t i = 0; i < suffix_length; ++i)
    {
        char c = text[length - suffix_length + i];

        if ((c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c) != suffix[i])
        {
            return 0;
        }
    }
    return 1;
}

static int join(char *out, size_t capacity, const char *folder, const char *name)
{
    int written = snprintf(out, capacity, "%s/%s", folder, name);

    return written > 0 && (size_t)written < capacity;
}

/* A copy of each default shortcut, into the new folder. */
static void seed(const char *folder)
{
    DIR *handle = opendir(START_PINS_DEFAULTS_DIR);
    struct dirent *entry;

    if (handle == 0)
    {
        return;
    }
    while ((entry = readdir(handle)) != 0)
    {
        char from[SHORTCUT_PATH_CAPACITY];
        char to[SHORTCUT_PATH_CAPACITY];
        struct shortcut link;

        if (entry->d_name[0] != '.' && shortcut_is_shortcut_name(entry->d_name) &&
            join(from, sizeof(from), START_PINS_DEFAULTS_DIR, entry->d_name) &&
            join(to, sizeof(to), folder, entry->d_name) && shortcut_load(from, &link) == 0)
        {
            (void)shortcut_save(to, &link);
        }
    }
    closedir(handle);
}

int start_pins_folder(char *out, size_t capacity)
{
    struct stat info;

    if (!join(out, capacity, savanxp_profile_dir(), START_PINS_FOLDER))
    {
        return -1;
    }
    if (stat(out, &info) == 0)
    {
        return S_ISDIR(info.st_mode) ? 0 : -1;
    }
    if (savanxp_mkdir(out) < 0)
    {
        return -1;
    }
    seed(out);
    return 0;
}

int start_pins_can_pin(const char *path)
{
    return path != 0 &&
           (shortcut_is_program_path(path) || ends_with_nocase(path, ".exe") || shortcut_is_shortcut_name(path));
}

/* How many pins there are, and whether one already opens `link`'s target
 * with its argument. */
static int survey(const char *folder, const struct shortcut *link, int *already)
{
    DIR *handle = opendir(folder);
    struct dirent *entry;
    int count = 0;

    *already = 0;
    if (handle == 0)
    {
        return 0;
    }
    while ((entry = readdir(handle)) != 0)
    {
        char path[SHORTCUT_PATH_CAPACITY];
        struct shortcut pin;

        if (entry->d_name[0] == '.' || !shortcut_is_shortcut_name(entry->d_name))
        {
            continue;
        }
        count += 1;
        if (join(path, sizeof(path), folder, entry->d_name) && shortcut_load(path, &pin) == 0 &&
            strcmp(pin.target, link->target) == 0 && strcmp(pin.args, link->args) == 0)
        {
            *already = 1;
        }
    }
    closedir(handle);
    return count;
}

enum start_pin_result start_pins_pin(const char *path)
{
    char folder[SHORTCUT_PATH_CAPACITY];
    char name[160];
    char pin[SHORTCUT_PATH_CAPACITY];
    struct shortcut link;
    struct stat info;
    int already;

    if (!start_pins_can_pin(path) || start_pins_folder(folder, sizeof(folder)) != 0)
    {
        return START_PIN_FAILED;
    }
    if (shortcut_is_shortcut_name(path))
    {
        /* The shortcut itself is pinned, under its own name. */
        const char *base = strrchr(path, '/');

        if (shortcut_load(path, &link) != 0)
        {
            return START_PIN_FAILED;
        }
        snprintf(name, sizeof(name), "%s", base != 0 ? base + 1 : path);
    }
    else
    {
        shortcut_for_target(path, &link, name, sizeof(name));
        if (ends_with_nocase(name, ".exe" SHORTCUT_EXTENSION))
        {
            /* "sol", not "sol.exe", and started where the program is, as
             * Windows fills a new shortcut's Start in. */
            size_t length = strlen(name) - (sizeof(SHORTCUT_EXTENSION) - 1u) - 4u;
            const char *slash = strrchr(path, '/');

            name[length] = '\0';
            strncat(name, SHORTCUT_EXTENSION, sizeof(name) - length - 1u);
            if (slash != 0 && slash != path && (size_t)(slash - path) < sizeof(link.workdir))
            {
                memcpy(link.workdir, path, (size_t)(slash - path));
                link.workdir[slash - path] = '\0';
            }
        }
    }
    if (survey(folder, &link, &already) >= START_PINS_MAX)
    {
        return already ? START_PIN_ALREADY : START_PIN_FULL;
    }
    if (already)
    {
        return START_PIN_ALREADY;
    }
    if (!join(pin, sizeof(pin), folder, name))
    {
        return START_PIN_FAILED;
    }
    /* A name already taken by another target gets " (2)" and on. */
    for (int attempt = 2; stat(pin, &info) == 0 && attempt < 100; ++attempt)
    {
        size_t stem = strlen(name) - (sizeof(SHORTCUT_EXTENSION) - 1u);

        snprintf(pin, sizeof(pin), "%s/%.*s (%d)%s", folder, (int)stem, name, attempt, SHORTCUT_EXTENSION);
    }
    return shortcut_save(pin, &link) == 0 ? START_PIN_DONE : START_PIN_FAILED;
}

int start_pins_unpin(const char *shortcut_path)
{
    char folder[SHORTCUT_PATH_CAPACITY];
    size_t length;

    if (shortcut_path == 0 || !shortcut_is_shortcut_name(shortcut_path) ||
        !join(folder, sizeof(folder), savanxp_profile_dir(), START_PINS_FOLDER))
    {
        return -1;
    }
    /* Only a pin: a file directly in the folder. */
    length = strlen(folder);
    if (strncmp(shortcut_path, folder, length) != 0 || shortcut_path[length] != '/' ||
        strchr(shortcut_path + length + 1u, '/') != 0)
    {
        return -1;
    }
    return unlink(shortcut_path) == 0 ? 0 : -1;
}
