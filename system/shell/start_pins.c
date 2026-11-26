#include "shell/start_pins.h"

#include "shell/shell_io.h"
#include "sxgfx/pe_version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* The largest Windows program whose version is read. */
#define START_PINS_PE_MAX (1024u * 1024u)

/* A Windows program's FileDescription, as Windows XP names a pinned program
 * ("Juego Solitario" for sol.exe), made a file name: no slashes, no blanks
 * at the ends. 0 when it has none. */
static int exe_description(const char *path, char *out, size_t capacity)
{
    uint8_t *file = malloc(START_PINS_PE_MAX);
    long total;
    int found = 0;

    if (file == 0)
    {
        return 0;
    }
    total = shell_io_read_file(path, file, START_PINS_PE_MAX);
    if (total >= 0)
    {
        found = pe_version_string(file, (size_t)total, "FileDescription", out, capacity);
    }
    free(file);
    if (found)
    {
        size_t start = 0;
        size_t end = strlen(out);

        for (char *c = out; *c != '\0'; ++c)
        {
            if (*c == '/' || *c == '\\')
            {
                *c = '-';
            }
        }
        while (out[start] == ' ')
        {
            ++start;
        }
        while (end > start && out[end - 1u] == ' ')
        {
            --end;
        }
        memmove(out, out + start, end - start);
        out[end - start] = '\0';
        found = out[0] != '\0' && strcmp(out, ".") != 0 && strcmp(out, "..") != 0;
    }
    return found;
}

/* One default shortcut, copied into the new folder. */
static void seed_one(const char *name, void *context)
{
    const char *folder = (const char *)context;
    char from[SHORTCUT_PATH_CAPACITY];
    char to[SHORTCUT_PATH_CAPACITY];
    struct shortcut link;

    if (name[0] != '.' && shortcut_is_shortcut_name(name) &&
        join(from, sizeof(from), START_PINS_DEFAULTS_DIR, name) &&
        join(to, sizeof(to), folder, name) && shortcut_load(from, &link) == 0)
    {
        (void)shortcut_save(to, &link);
    }
}

int start_pins_folder(char *out, size_t capacity)
{
    struct shell_io_info info;

    if (!join(out, capacity, shell_io_profile_directory(), START_PINS_FOLDER))
    {
        return -1;
    }
    if (shell_io_query(out, &info) == 0)
    {
        return info.type == SHELL_IO_DIRECTORY ? 0 : -1;
    }
    if (shell_io_make_directory(out) < 0)
    {
        return -1;
    }
    /* A copy of each default shortcut, into the new folder. */
    (void)shell_io_list(START_PINS_DEFAULTS_DIR, seed_one, out);
    return 0;
}

int start_pins_can_pin(const char *path)
{
    return path != 0 &&
           (shortcut_is_program_path(path) || ends_with_nocase(path, ".exe") || shortcut_is_shortcut_name(path));
}

struct survey_state
{
    const char *folder;
    const struct shortcut *link;
    int count;
    int already;
};

static void survey_one(const char *name, void *context)
{
    struct survey_state *state = (struct survey_state *)context;
    char path[SHORTCUT_PATH_CAPACITY];
    struct shortcut pin;

    if (name[0] == '.' || !shortcut_is_shortcut_name(name))
    {
        return;
    }
    state->count += 1;
    if (join(path, sizeof(path), state->folder, name) && shortcut_load(path, &pin) == 0 &&
        strcmp(pin.target, state->link->target) == 0 && strcmp(pin.args, state->link->args) == 0)
    {
        state->already = 1;
    }
}

/* How many pins there are, and whether one already opens `link`'s target
 * with its argument. */
static int survey(const char *folder, const struct shortcut *link, int *already)
{
    struct survey_state state = {folder, link, 0, 0};

    (void)shell_io_list(folder, survey_one, &state);
    *already = state.already;
    return state.count;
}

enum start_pin_result start_pins_pin(const char *path)
{
    char folder[SHORTCUT_PATH_CAPACITY];
    char name[160];
    char pin[SHORTCUT_PATH_CAPACITY];
    struct shortcut link;
    struct shell_io_info info;
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
            /* Named by its description ("Juego Solitario"), or else "sol",
             * not "sol.exe"; started where the program is, as Windows fills
             * a new shortcut's Start in. */
            size_t length = strlen(name) - (sizeof(SHORTCUT_EXTENSION) - 1u) - 4u;
            const char *slash = strrchr(path, '/');
            char description[64];

            name[length] = '\0';
            if (exe_description(path, description, sizeof(description)))
            {
                snprintf(name, sizeof(name), "%s", description);
                length = strlen(name);
            }
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
    for (int attempt = 2; shell_io_query(pin, &info) == 0 && attempt < 100; ++attempt)
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
        !join(folder, sizeof(folder), shell_io_profile_directory(), START_PINS_FOLDER))
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
    return shell_io_delete_file(shortcut_path) == 0 ? 0 : -1;
}
