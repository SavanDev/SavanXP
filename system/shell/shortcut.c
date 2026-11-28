#include "shell/shortcut.h"

#include "shell/file_assoc.h"
#include "shell/shell_io.h"
#include "savanxp/sxe.h"

#include <stdio.h>
#include <string.h>

static char lower_char(char value)
{
    return value >= 'A' && value <= 'Z' ? (char)(value - 'A' + 'a') : value;
}

static int ends_with_nocase(const char *text, const char *suffix)
{
    size_t text_length = text != 0 ? strlen(text) : 0;
    size_t suffix_length = strlen(suffix);
    size_t index;

    if (text_length < suffix_length)
    {
        return 0;
    }
    for (index = 0; index < suffix_length; ++index)
    {
        if (lower_char(text[text_length - suffix_length + index]) != suffix[index])
        {
            return 0;
        }
    }
    return 1;
}

static void copy_field(char *destination, size_t capacity, const char *source, size_t length)
{
    if (capacity == 0)
    {
        return;
    }
    if (length >= capacity)
    {
        length = capacity - 1;
    }
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static int is_space(char value)
{
    return value == ' ' || value == '\t' || value == '\r';
}

int shortcut_is_shortcut_name(const char *name)
{
    return ends_with_nocase(name, SHORTCUT_EXTENSION);
}

/* A desktop file in the explicit Linux guest tree selects SSL. ELF alone
 * cannot distinguish Linux from POSIX, so files elsewhere keep their normal
 * association. Reject path traversal before selecting this subsystem. */
int shortcut_is_linux_program(const char *path)
{
    if (!path || strncmp(path, "/system/linux/", 14) != 0) return 0;
    const char *part = path + 14;
    while (*part) {
        const char *end = part;
        while (*end && *end != '/') ++end;
        const size_t count = (size_t)(end - part);
        if (!count || (count == 1 && part[0] == '.') ||
            (count == 2 && part[0] == '.' && part[1] == '.')) return 0;
        part = *end ? end + 1 : end;
    }
    unsigned char header[64];
    const long count = shell_io_read_file(path, header, sizeof(header));
    return count == (long)sizeof(header) && !memcmp(header, "\177ELF\2\1\1", 7) &&
        (header[16] == 2 || header[16] == 3) && !header[17] && header[18] == 62 && !header[19];
}

int shortcut_is_program_path(const char *path)
{
    return path != 0 &&
        (shortcut_is_linux_program(path) || strncmp(path, "/bin/", 5) == 0 || strncmp(path, "/disk/bin/", 10) == 0 || ends_with_nocase(path, ".sxe"));
}

int shortcut_parse(const char *text, size_t length, struct shortcut *out)
{
    size_t position = 0;

    if (out == 0)
    {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (text == 0)
    {
        return -1;
    }
    while (position < length)
    {
        size_t line_start = position;
        size_t line_end;
        size_t key_start;
        size_t key_end;
        size_t value_start;
        size_t value_end;
        size_t equals;

        while (position < length && text[position] != '\n')
        {
            ++position;
        }
        line_end = position;
        if (position < length)
        {
            ++position;
        }

        key_start = line_start;
        while (key_start < line_end && is_space(text[key_start]))
        {
            ++key_start;
        }
        if (key_start >= line_end || text[key_start] == '#' || text[key_start] == ';')
        {
            continue;
        }
        equals = key_start;
        while (equals < line_end && text[equals] != '=')
        {
            ++equals;
        }
        if (equals >= line_end)
        {
            continue;
        }
        key_end = equals;
        while (key_end > key_start && is_space(text[key_end - 1]))
        {
            --key_end;
        }
        value_start = equals + 1;
        while (value_start < line_end && is_space(text[value_start]))
        {
            ++value_start;
        }
        value_end = line_end;
        while (value_end > value_start && is_space(text[value_end - 1]))
        {
            --value_end;
        }

#define SHORTCUT_KEY_IS(name) \
    (key_end - key_start == sizeof(name) - 1 && memcmp(text + key_start, name, sizeof(name) - 1) == 0)
        if (SHORTCUT_KEY_IS("target"))
        {
            copy_field(out->target, sizeof(out->target), text + value_start, value_end - value_start);
        }
        else if (SHORTCUT_KEY_IS("args"))
        {
            copy_field(out->args, sizeof(out->args), text + value_start, value_end - value_start);
        }
        else if (SHORTCUT_KEY_IS("workdir"))
        {
            copy_field(out->workdir, sizeof(out->workdir), text + value_start, value_end - value_start);
        }
        else if (SHORTCUT_KEY_IS("icon"))
        {
            copy_field(out->icon, sizeof(out->icon), text + value_start, value_end - value_start);
        }
#undef SHORTCUT_KEY_IS
    }
    return out->target[0] != '\0' ? 0 : -1;
}

int shortcut_format(const struct shortcut *shortcut, char *out, size_t capacity)
{
    static const char *const k_keys[4] = {"target", "args", "workdir", "icon"};
    const char *values[4];
    size_t used = 0;
    int index;

    if (shortcut == 0 || out == 0 || capacity == 0 || shortcut->target[0] == '\0')
    {
        return -1;
    }
    values[0] = shortcut->target;
    values[1] = shortcut->args;
    values[2] = shortcut->workdir;
    values[3] = shortcut->icon;
    out[0] = '\0';
    for (index = 0; index < 4; ++index)
    {
        int written;

        if (values[index][0] == '\0')
        {
            continue;
        }
        written = snprintf(out + used, capacity - used, "%s=%s\n", k_keys[index], values[index]);
        if (written < 0 || (size_t)written >= capacity - used)
        {
            return -1;
        }
        used += (size_t)written;
    }
    return (int)used;
}

int shortcut_load(const char *path, struct shortcut *out)
{
    char text[SHORTCUT_MAX_BYTES];
    long total = shell_io_read_file(path, text, sizeof(text));

    if (total < 0)
    {
        if (out != 0)
        {
            memset(out, 0, sizeof(*out));
        }
        return -1;
    }
    return shortcut_parse(text, (size_t)total, out);
}

int shortcut_save(const char *path, const struct shortcut *shortcut)
{
    char text[SHORTCUT_MAX_BYTES];
    int length = shortcut_format(shortcut, text, sizeof(text));

    if (length < 0)
    {
        return -1;
    }
    return shell_io_write_file(path, text, (size_t)length) == 0 ? 0 : -1;
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash != 0 ? slash + 1 : path;
}

void shortcut_for_target(const char *target, struct shortcut *out, char *name, size_t name_capacity)
{
    char title[64];
    const char *base;
    size_t base_length;

    memset(out, 0, sizeof(*out));
    copy_field(out->target, sizeof(out->target), target, strlen(target));
    if (name == 0 || name_capacity == 0)
    {
        return;
    }

    /* A program is named as it calls itself (its .sxmeta), a file by its
     * own name. */
    title[0] = '\0';
    if (shortcut_is_program_path(target))
    {
        static _Alignas(8) uint8_t meta_buffer[SXE_META_MAX_BYTES];
        struct sxe_meta meta;

        if (sxe_load_meta(target, meta_buffer, sizeof(meta_buffer), &meta) == SXE_OK)
        {
            (void)sxe_meta_string(&meta, SXE_TAG_NAME, title, sizeof(title));
        }
    }
    base = base_name(target);
    base_length = strlen(base);
    if (title[0] == '\0')
    {
        if (shortcut_is_program_path(target) && ends_with_nocase(base, ".sxe"))
        {
            base_length -= 4;
        }
        copy_field(title, sizeof(title), base, base_length);
    }
    /* A name a file can have: no slashes from a program's title. */
    {
        char *cursor;
        for (cursor = title; *cursor != '\0'; ++cursor)
        {
            if (*cursor == '/' || *cursor == '\\')
            {
                *cursor = '-';
            }
        }
    }
    snprintf(name, name_capacity, "%s%s", title, SHORTCUT_EXTENSION);
}

int shortcut_send_to_desktop(const char *target)
{
    char folder[SHORTCUT_PATH_CAPACITY];
    char name[160];
    char path[SHORTCUT_PATH_CAPACITY];
    struct shortcut link;
    struct shell_io_info info;
    int written;

    if (target == 0 || target[0] == '\0')
    {
        return -1;
    }
    written = snprintf(folder, sizeof(folder), "%s/Desktop", shell_io_profile_directory());
    if (written < 0 || (size_t)written >= sizeof(folder))
    {
        return -1;
    }
    if (shell_io_query(folder, &info) != 0)
    {
        if (shell_io_make_directory(folder) < 0)
        {
            return -1;
        }
    }
    else if (info.type != SHELL_IO_DIRECTORY)
    {
        return -1;
    }
    shortcut_for_target(target, &link, name, sizeof(name));
    written = snprintf(path, sizeof(path), "%s/%s", folder, name);
    if (written < 0 || (size_t)written >= sizeof(path))
    {
        return -1;
    }
    for (int attempt = 2; shell_io_query(path, &info) == 0; ++attempt)
    {
        size_t stem = strlen(name) - (sizeof(SHORTCUT_EXTENSION) - 1u);

        if (attempt >= 100)
        {
            return -1;
        }
        snprintf(path, sizeof(path), "%s/%.*s (%d)%s", folder, (int)stem, name, attempt, SHORTCUT_EXTENSION);
    }
    return shortcut_save(path, &link);
}

static int is_directory(const char *path)
{
    struct shell_io_info info;
    return shell_io_query(path, &info) == 0 && info.type == SHELL_IO_DIRECTORY;
}

static long launch(const char *workdir, const char *program, const char *argument)
{
    return shell_io_launch(program, argument, workdir);
}

/* Files and the desktop keep one association snapshot for both actions. */
static void ensure_associations(void)
{
    static int loaded;
    if (!loaded) { (void)file_assoc_load(); loaded = 1; }
}

/* A document: its Open association, or Notepad when nobody claims it. */
static enum shortcut_open_kind open_document(const char *workdir, const char *path)
{
    const char *program;
    ensure_associations();
    program = file_assoc_program_for_file(path);
    if (program != 0)
    {
        return launch(workdir, program, path) >= 0 ? SHORTCUT_OPEN_ASSOCIATED : SHORTCUT_OPEN_FAILED;
    }
    return launch(workdir, SHORTCUT_EDITOR_PATH, path) >= 0 ? SHORTCUT_OPEN_EDITOR : SHORTCUT_OPEN_FAILED;
}

/* What a path opens as, without following shortcuts; whatever it starts
 * starts in `workdir` when there is one. */
static enum shortcut_open_kind open_plain(const char *workdir, const char *path, const char *args, int edit)
{
    if (edit)
    {
        const char *editor;
        if (is_directory(path) || shortcut_is_program_path(path) || shortcut_is_linux_program(path)) return SHORTCUT_OPEN_FAILED;
        ensure_associations();
        editor = file_assoc_editor_for_file(path);
        return editor && launch(workdir, editor, path) >= 0 ? SHORTCUT_OPEN_ASSOCIATED : SHORTCUT_OPEN_FAILED;
    }
    if (is_directory(path))
    {
        return launch(workdir, SHORTCUT_FILES_PATH, path) >= 0 ? SHORTCUT_OPEN_FOLDER : SHORTCUT_OPEN_FAILED;
    }
    if (shortcut_is_linux_program(path))
    {
        /* The desktop API passes one argument. The physical guest path is
         * the launcher's desktop mode; Linux sees the stripped guest path. */
        return launch(workdir, "/system/linux.sxe", path) >= 0 ? SHORTCUT_OPEN_PROGRAM : SHORTCUT_OPEN_FAILED;
    }
    if (shortcut_is_program_path(path))
    {
        return launch(workdir, path, args) >= 0 ? SHORTCUT_OPEN_PROGRAM : SHORTCUT_OPEN_FAILED;
    }
    return open_document(workdir, path);
}

static enum shortcut_open_kind act_on_path(const char *path, int edit)
{
    struct shortcut shortcut;

    if (path == 0 || path[0] == '\0')
    {
        return SHORTCUT_OPEN_FAILED;
    }
    if (!shortcut_is_shortcut_name(path) || is_directory(path))
    {
        return open_plain(0, path, 0, edit);
    }
    if (shortcut_load(path, &shortcut) != 0)
    {
        return SHORTCUT_OPEN_FAILED;
    }
    /* A shortcut to a shortcut is not followed: one level, as Windows does. */
    if (shortcut_is_shortcut_name(shortcut.target) && !is_directory(shortcut.target))
    {
        return SHORTCUT_OPEN_FAILED;
    }
    /* What it starts, starts in the shortcut's working directory. */
    return open_plain(shortcut.workdir, shortcut.target, shortcut.args, edit);
}

enum shortcut_open_kind shortcut_open_path(const char *path)
{
    return act_on_path(path, 0);
}

enum shortcut_open_kind shortcut_edit_path(const char *path)
{
    return act_on_path(path, 1);
}

int shortcut_can_edit_path(const char *path)
{
    struct shortcut shortcut;
    struct shell_io_info info;
    if (!path || !path[0]) return 0;
    if (shortcut_is_shortcut_name(path))
    {
        if (shortcut_load(path, &shortcut) != 0 || shortcut_is_shortcut_name(shortcut.target)) return 0;
        path = shortcut.target;
    }
    if (shell_io_query(path, &info) != 0 || info.type != SHELL_IO_FILE || shortcut_is_program_path(path) ||
        shortcut_is_linux_program(path)) return 0;
    ensure_associations();
    return file_assoc_editor_for_file(path) != 0;
}

/* ---- selftest ----------------------------------------------------------- */

static int g_failures = 0;

static void expect(int condition, const char *label)
{
    if (!condition)
    {
        printf("SHORTCUT SELFTEST FAIL %s\n", label);
        ++g_failures;
    }
}

int shortcut_selftest(void)
{
    static const char k_text[] =
        "# a comment\n"
        "; another\n"
        "  target = /bin/notepad.sxe  \n"
        "args=/users/user/notes.txt\r\n"
        "unknown=ignored\n"
        "workdir=/users/user\n"
        "icon=text-x-generic";
    struct shortcut shortcut;
    struct shortcut again;
    char text[SHORTCUT_MAX_BYTES];
    char name[96];
    int length;

    g_failures = 0;
    expect(shortcut_parse(k_text, sizeof(k_text) - 1, &shortcut) == 0, "parse: a target");
    expect(strcmp(shortcut.target, "/bin/notepad.sxe") == 0, "parse: spaces around the target");
    expect(strcmp(shortcut.args, "/users/user/notes.txt") == 0, "parse: CRLF");
    expect(strcmp(shortcut.workdir, "/users/user") == 0, "parse: workdir");
    expect(strcmp(shortcut.icon, "text-x-generic") == 0, "parse: last line without newline");
    expect(shortcut_parse("args=x\n", 7, &again) != 0, "parse: no target is refused");

    length = shortcut_format(&shortcut, text, sizeof(text));
    expect(length > 0 && shortcut_parse(text, (size_t)length, &again) == 0 &&
               memcmp(&shortcut, &again, sizeof(shortcut)) == 0,
        "format: round trip");
    memset(&again, 0, sizeof(again));
    strcpy(again.target, "/bin/calc");
    length = shortcut_format(&again, text, sizeof(text));
    expect(length >= 0 && strcmp(text, "target=/bin/calc\n") == 0, "format: empty fields left out");

    expect(shortcut_is_shortcut_name("Notepad.SXLNK"), "name: any case");
    expect(!shortcut_is_shortcut_name("sxlnk"), "name: an extension, not a name");
    expect(shortcut_is_program_path("/bin/calc") && shortcut_is_program_path("/users/user/game.sxe") &&
               !shortcut_is_program_path("/users/user/notes.txt"),
        "program paths");
    expect(!shortcut_is_linux_program("/bin/calc") && !shortcut_is_linux_program("/system/linux.sxe") &&
               !shortcut_is_linux_program("/system/linux/../linux.sxe") &&
               !shortcut_is_linux_program("/system/linux/./fixtures/hello"),
        "linux programs: only the guest tree, no dot components");
    {
        /* Present only in test images; when it is there it must be a Linux program. */
        struct shell_io_info fixture;

        if (shell_io_query("/system/linux/fixtures/hello", &fixture) == 0)
        {
            expect(shortcut_is_linux_program("/system/linux/fixtures/hello"), "linux programs: a guest ELF");
        }
    }

    shortcut_for_target("/users/user/notes.txt", &again, name, sizeof(name));
    expect(strcmp(name, "notes.txt.sxlnk") == 0 && strcmp(again.target, "/users/user/notes.txt") == 0,
        "naming: a document keeps its name");
    shortcut_for_target("/no/such/dir/tool.sxe", &again, name, sizeof(name));
    expect(strcmp(name, "tool.sxlnk") == 0, "naming: a program without metadata drops .sxe");
    return g_failures;
}
