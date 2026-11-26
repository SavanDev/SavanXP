#include "shell/file_assoc.h"

#include "savanxp/sxe.h"
#include "shell/shell_io.h"

#include <stdio.h>
#include <string.h>

static struct file_assoc_entry g_entries[FILE_ASSOC_MAX_ENTRIES];
static int g_entry_count = 0;
/* How many executables the last scan opened. The smoke reports it: it is the
 * number to look at before deciding whether a cache is needed. */
static int g_last_scan_examined = 0;

/* --- helpers -------------------------------------------------------------- */

static char lower_char(char value)
{
    if (value >= 'A' && value <= 'Z')
    {
        return (char)(value - 'A' + 'a');
    }
    return value;
}

static void copy_field(char *destination, size_t capacity, const char *source)
{
    size_t index = 0;

    if (destination == 0 || capacity == 0)
    {
        return;
    }
    if (source != 0)
    {
        while (source[index] != '\0' && index + 1u < capacity)
        {
            destination[index] = source[index];
            index += 1u;
        }
    }
    destination[index] = '\0';
}

static int is_space_char(char value)
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

static char *trim(char *text)
{
    size_t length;

    if (text == 0)
    {
        return text;
    }
    while (*text != '\0' && is_space_char(*text))
    {
        text += 1;
    }
    length = strlen(text);
    while (length != 0 && is_space_char(text[length - 1u]))
    {
        text[length - 1u] = '\0';
        length -= 1u;
    }
    return text;
}

/*
 * Normalises an extension to a lower-case ".ext". It is accepted with or
 * without the dot: an assoc.ini written by hand is likely to have both forms,
 * and refusing one of them would be a trap with no benefit.
 */
static int normalize_extension(const char *source, char *out, size_t capacity)
{
    size_t index = 0;

    if (source == 0 || out == 0 || capacity < 3u)
    {
        return 0;
    }
    if (source[0] == '\0' || (source[0] == '.' && source[1] == '\0'))
    {
        return 0;
    }

    out[index] = '.';
    index += 1u;
    if (source[0] == '.')
    {
        source += 1;
    }
    while (source[0] != '\0' && index + 1u < capacity)
    {
        out[index] = lower_char(source[0]);
        index += 1u;
        source += 1;
    }
    out[index] = '\0';
    return 1;
}

static int find_entry(const char *extension)
{
    int index;

    for (index = 0; index < g_entry_count; ++index)
    {
        if (strcmp(g_entries[index].extension, extension) == 0)
        {
            return index;
        }
    }
    return -1;
}

/* Add or replace one action. Explicit user policy wins over shipped defaults
 * and scanned capabilities, regardless of which extension has the other action. */
static int set_entry(const char *extension, const char *program, int from_policy, int edit)
{
    char normalized[FILE_ASSOC_EXT_CAPACITY];
    int index;

    if (!normalize_extension(extension, normalized, sizeof(normalized)))
    {
        return 0;
    }
    if (program == 0 || program[0] != '/')
    {
        /* A program without an absolute path cannot be started: it is dropped
         * instead of being kept broken. */
        return 0;
    }

    index = find_entry(normalized);
    if (index < 0)
    {
        if (g_entry_count >= FILE_ASSOC_MAX_ENTRIES) return 0;
        index = g_entry_count++;
        memset(&g_entries[index], 0, sizeof(g_entries[index]));
        copy_field(g_entries[index].extension, sizeof(g_entries[index].extension), normalized);
    }
    {
        char *target = edit ? g_entries[index].editor : g_entries[index].program;
        int *policy = edit ? &g_entries[index].edit_from_policy : &g_entries[index].from_policy;
        /* User choices replace defaults; defaults precede capability scanning.
         * Open and Edit have independent precedence for the same extension. */
        if (target[0] != '\0' && !from_policy) return 0;
        copy_field(target, FILE_ASSOC_PATH_CAPACITY, program);
        *policy = from_policy;
    }
    return 1;
}

const char *file_assoc_extension_of(const char *file_path, char *out, size_t capacity)
{
    const char *dot = 0;
    const char *cursor = file_path;

    if (file_path == 0 || out == 0 || capacity == 0)
    {
        return 0;
    }
    /* Only the last dot of the LAST component: "/v1.2/README" has no
     * extension, even though there is a dot on the way. */
    while (cursor[0] != '\0')
    {
        if (cursor[0] == '/')
        {
            dot = 0;
        }
        else if (cursor[0] == '.')
        {
            dot = cursor;
        }
        cursor += 1;
    }
    if (dot == 0 || dot[1] == '\0')
    {
        out[0] = '\0';
        return 0;
    }
    if (!normalize_extension(dot, out, capacity))
    {
        out[0] = '\0';
        return 0;
    }
    return out;
}

/* --- user policy --------------------------------------------------------- */

static int parse_entries(const char *text, size_t length, int from_policy)
{
    static char buffer[FILE_ASSOC_POLICY_MAX_BYTES];
    size_t copy_length = 0;
    size_t offset = 0;
    int added = 0;

    if (text == 0 || length == 0)
    {
        return 0;
    }

    copy_length = length < (sizeof(buffer) - 1u) ? length : (sizeof(buffer) - 1u);
    memcpy(buffer, text, copy_length);
    buffer[copy_length] = '\0';

    while (offset < copy_length)
    {
        size_t start = offset;
        char *line = 0;
        char *separator = 0;
        size_t scan = 0;

        while (offset < copy_length && buffer[offset] != '\n')
        {
            offset += 1u;
        }
        if (offset < copy_length)
        {
            buffer[offset] = '\0';
            offset += 1u;
        }
        line = trim(&buffer[start]);
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';')
        {
            continue;
        }

        for (scan = 0; line[scan] != '\0'; ++scan)
        {
            if (line[scan] == '=')
            {
                separator = &line[scan];
                break;
            }
        }
        if (separator == 0)
        {
            continue;
        }
        *separator = '\0';
        line = trim(line);
        {
            int edit = strncmp(line, "edit:", 5) == 0;
            if (edit || strncmp(line, "open:", 5) == 0) line += 5;
            if (strchr(line, ':') != 0) continue;
            if (set_entry(trim(line), trim(separator + 1), from_policy, edit))
            {
                added += 1;
            }
        }
    }
    return added;
}

int file_assoc_parse_policy(const char *text, size_t length)
{
    g_entry_count = 0;
    return parse_entries(text, length, 1);
}

static int load_policy_file(const char *path, int from_policy)
{
    static char file_buffer[FILE_ASSOC_POLICY_MAX_BYTES];
    /* One byte short of the buffer, as parse_entries keeps one for the NUL. */
    long total = shell_io_read_file(path, file_buffer, sizeof(file_buffer) - 1u);

    /* Missing policy is normal: lower-priority sources still apply. */
    if (total <= 0)
    {
        return 0;
    }
    return parse_entries(file_buffer, (size_t)total, from_policy);
}

int file_assoc_load_policy(void)
{
    char path[256];
    g_entry_count = 0;
    if (shell_io_settings_path(FILE_ASSOC_POLICY_NAME, 0, path, sizeof(path)) != 0) return 0;
    return load_policy_file(path, 1);
}

/* --- capability scan ----------------------------------------------------- */

static int default_path_exists(const char *path)
{
    struct shell_io_info info;

    return shell_io_query(path, &info) == 0;
}

/* Reads the extensions a binary declares and registers them. */
static int scan_program(const char *path, void *meta_buffer, size_t meta_capacity)
{
    struct sxe_meta meta;
    char extension[FILE_ASSOC_EXT_CAPACITY];
    int added = 0;
    int index = 0;
    int count = 0;

    if (sxe_load_meta(path, meta_buffer, meta_capacity, &meta) != SXE_OK)
    {
        return 0;
    }
    count = sxe_meta_list_count(&meta, SXE_TAG_EXT_OPEN);
    for (index = 0; index < count; ++index)
    {
        if (sxe_meta_list_at(&meta, SXE_TAG_EXT_OPEN, index, extension, sizeof(extension)) == 0u)
        {
            continue;
        }
        if (set_entry(extension, path, 0, 0))
        {
            added += 1;
        }
    }
    return added;
}

struct scan_context
{
    const char *directory;
    file_assoc_path_exists_fn exists;
    void *meta_buffer;
    size_t meta_capacity;
    int added;
};

static void scan_entry(const char *name, void *opaque)
{
    struct scan_context *context = (struct scan_context *)opaque;
    char full_path[FILE_ASSOC_PATH_CAPACITY];
    int written;

    if (name[0] == '.')
    {
        return;
    }
    written = snprintf(full_path, sizeof(full_path), "%s/%s", context->directory, name);
    /* A truncated path would point at ANOTHER file, not the one meant: it is
     * dropped instead of opened. */
    if (written < 0 || (size_t)written >= sizeof(full_path))
    {
        return;
    }
    if (context->exists != 0 && !context->exists(full_path))
    {
        return;
    }
    g_last_scan_examined += 1;
    context->added += scan_program(full_path, context->meta_buffer, context->meta_capacity);
}

static int scan_directory(const char *directory, file_assoc_path_exists_fn exists, void *meta_buffer, size_t meta_capacity)
{
    struct scan_context context = {directory, exists, meta_buffer, meta_capacity, 0};

    (void)shell_io_list(directory, scan_entry, &context);
    return context.added;
}

int file_assoc_scan_programs(file_assoc_path_exists_fn exists)
{
    /* Static and not on the stack, like the rest of userland's scratch
     * buffers: 4 KiB reused for every binary of the scan. */
    _Alignas(4) static uint8_t meta_buffer[SXE_META_MAX_BYTES];
    int added = 0;

    g_last_scan_examined = 0;
    if (exists == 0)
    {
        exists = default_path_exists;
    }
    added += scan_directory(FILE_ASSOC_SCAN_DIR, exists, meta_buffer, sizeof(meta_buffer));
    added += scan_directory("/system", exists, meta_buffer, sizeof(meta_buffer));
    return added;
}

int file_assoc_scan_examined(void)
{
    return g_last_scan_examined;
}

int file_assoc_load(void)
{
    (void)file_assoc_load_policy();
    (void)load_policy_file(FILE_ASSOC_DEFAULTS_PATH, 0);
    (void)file_assoc_scan_programs(0);
    return g_entry_count;
}

/* --- lookup -------------------------------------------------------------- */

const char *file_assoc_program_for_file(const char *file_path)
{
    char extension[FILE_ASSOC_EXT_CAPACITY];
    int index;

    if (file_assoc_extension_of(file_path, extension, sizeof(extension)) == 0)
    {
        return 0;
    }
    index = find_entry(extension);
    if (index < 0)
    {
        return 0;
    }
    return g_entries[index].program[0] ? g_entries[index].program : 0;
}

const char *file_assoc_editor_for_file(const char *file_path)
{
    char extension[FILE_ASSOC_EXT_CAPACITY];
    int index;
    if (!file_assoc_extension_of(file_path, extension, sizeof(extension))) return 0;
    index = find_entry(extension);
    return index >= 0 && g_entries[index].editor[0] ? g_entries[index].editor : 0;
}

int file_assoc_count(void)
{
    return g_entry_count;
}

const struct file_assoc_entry *file_assoc_at(int index)
{
    if (index < 0 || index >= g_entry_count)
    {
        return 0;
    }
    return &g_entries[index];
}

/* --- selftest ------------------------------------------------------------- */

static int g_selftest_failures = 0;

static void expect(int condition, const char *label)
{
    if (!condition)
    {
        printf("FILESAPP SMOKE FAIL %s\n", label);
        g_selftest_failures += 1;
    }
}

static void selftest_extension_of(void)
{
    char extension[FILE_ASSOC_EXT_CAPACITY];

    expect(file_assoc_extension_of("/notas.txt", extension, sizeof(extension)) != 0 &&
        strcmp(extension, ".txt") == 0, "simple extension");
    /* Upper case normalised: a .TXT has to resolve as a .txt. */
    expect(file_assoc_extension_of("/NOTAS.TXT", extension, sizeof(extension)) != 0 &&
        strcmp(extension, ".txt") == 0, "upper-case extension");
    /* Only the last component counts. */
    expect(file_assoc_extension_of("/v1.2/README", extension, sizeof(extension)) == 0,
        "a dot in a directory is no extension");
    expect(file_assoc_extension_of("/bin/progman", extension, sizeof(extension)) == 0,
        "binary without an extension");
    expect(file_assoc_extension_of("/archivo.", extension, sizeof(extension)) == 0,
        "trailing dot is no extension");
    expect(file_assoc_extension_of("/.oculto", extension, sizeof(extension)) != 0 &&
        strcmp(extension, ".oculto") == 0, "file starting with a dot");
    expect(file_assoc_extension_of(0, extension, sizeof(extension)) == 0, "null path");
    /* An extension longer than the capacity is truncated, not overflowed. */
    expect(file_assoc_extension_of("/x.extensionmuylargaquenoentra", extension, sizeof(extension)) != 0,
        "long extension truncated");
    expect(strlen(extension) == sizeof(extension) - 1u, "truncated to the capacity");
}

static void selftest_policy(void)
{
    static const char kText[] =
        "# comment\n"
        "; another comment\n"
        ".txt=/bin/notepad\n"
        "  .md  =  /bin/notepad  \n"
        "cfg=/bin/notepad\n"          /* no dot: accepted all the same */
        ".BAT=/system/console.sxe\n"     /* upper case: normalised */
        "no_equals\n"                 /* invalid line: ignored */
        ".rel=relative/program\n"     /* relative path: dropped */
        ".empty=\n";                  /* no program: dropped */

    expect(file_assoc_parse_policy(kText, sizeof(kText) - 1) == 4, "policy: 4 valid entries");
    expect(file_assoc_count() == 4, "policy: count");
    expect(file_assoc_program_for_file("/a.txt") != 0 &&
        strcmp(file_assoc_program_for_file("/a.txt"), "/bin/notepad") == 0, "policy: .txt");
    expect(file_assoc_program_for_file("/a.md") != 0 &&
        strcmp(file_assoc_program_for_file("/a.md"), "/bin/notepad") == 0, "policy: spaces trimmed");
    expect(file_assoc_program_for_file("/a.cfg") != 0, "policy: extension without a dot");
    expect(file_assoc_program_for_file("/a.bat") != 0 &&
        strcmp(file_assoc_program_for_file("/a.bat"), "/system/console.sxe") == 0, "policy: upper case is normalised");
    expect(file_assoc_program_for_file("/a.rel") == 0, "policy: drops a relative path");
    expect(file_assoc_program_for_file("/a.empty") == 0, "policy: drops an empty program");
    expect(file_assoc_program_for_file("/a.desconocida") == 0, "policy: undeclared extension");
    expect(file_assoc_program_for_file("/bin/progman") == 0, "policy: file without an extension");

    /* Out of range returns 0 instead of garbage. */
    expect(file_assoc_at(-1) == 0 && file_assoc_at(file_assoc_count()) == 0, "policy: bounds");

    /* Empty text leaves the registry empty, not broken. */
    expect(file_assoc_parse_policy("", 0) == 0 && file_assoc_count() == 0, "empty policy");
}

static void selftest_precedence(void)
{
    static const char kText[] = ".txt=/system/console.sxe\n";
    const struct file_assoc_entry *entry = 0;

    /*
     * Policy wins over what the binaries declare. notepad declares .txt in
     * its manifest, so without this order the scan would take it.
     */
    expect(file_assoc_parse_policy(kText, sizeof(kText) - 1) == 1, "precedence: fixture");
    (void)file_assoc_scan_programs(0);
    expect(file_assoc_program_for_file("/a.txt") != 0 &&
        strcmp(file_assoc_program_for_file("/a.txt"), "/system/console.sxe") == 0,
        "precedence: policy beats the binary");

    entry = file_assoc_at(0);
    expect(entry != 0 && entry->from_policy == 1, "precedence: marked as policy");
}

static int deny_all(const char *path)
{
    (void)path;
    return 0;
}

static void selftest_scan(void)
{
    const char *program = 0;

    /*
     * A scan of the image's REAL binaries. notepad declares
     * ext_open=.txt,.ini,.cfg,.md in its manifest: if the stamping or the
     * reader broke, this fails. It is the only check tying the .sxres
     * declaration to the live resolution.
     */
    expect(file_assoc_parse_policy("", 0) == 0, "scan: no earlier policy");
    expect(file_assoc_scan_programs(0) > 0, "scan: some extension declared");

    program = file_assoc_program_for_file("/notes.txt");
    expect(program != 0 && strcmp(program, "/bin/notepad") == 0, "scan: notepad takes .txt");
    program = file_assoc_program_for_file("/config.ini");
    expect(program != 0 && strcmp(program, "/bin/notepad") == 0, "scan: notepad takes .ini");
    program = file_assoc_program_for_file("/notes.md");
    expect(program != 0 && strcmp(program, "/bin/notepad") == 0, "scan: notepad takes .md");

    /* Media: the association has to point at the LAUNCHER, not the backend.
     * A mediaplayer-ffmpeg belongs to the port and may be absent;
     * /bin/mediaplayer is always there and is the one that delegates or
     * explains. It is what makes opening an .avi on an image without FFmpeg
     * show the notice instead of failing, and the only thing tying that to
     * reality is this line: the capabilities come from the stamped .sxres, so
     * if the stamping breaks, this fails. */
    program = file_assoc_program_for_file("/song.mp3");
    expect(program != 0 && strcmp(program, "/bin/mediaplayer") == 0, "scan: the launcher takes .mp3");
    program = file_assoc_program_for_file("/movie.avi");
    expect(program != 0 && strcmp(program, "/bin/mediaplayer") == 0, "scan: the launcher takes .avi");
    program = file_assoc_program_for_file("/audio.flac");
    expect(program != 0 && strcmp(program, "/bin/mediaplayer") == 0, "scan: the launcher takes .flac");

    /* An extension nobody declares stays without an owner. */
    expect(file_assoc_program_for_file("/thing.xyz") == 0, "scan: extension nobody owns");

    /* The scan has to be idempotent: running it twice can neither duplicate
     * entries nor change who resolves. */
    {
        int before = file_assoc_count();

        (void)file_assoc_scan_programs(0);
        expect(file_assoc_count() == before, "scan: idempotent");
        program = file_assoc_program_for_file("/notes.txt");
        expect(program != 0 && strcmp(program, "/bin/notepad") == 0, "scan: idempotent keeps the owner");
    }

    /* With a predicate that denies everything, no binary is opened and no
     * association is left: it proves the injection really cuts the scan. */
    expect(file_assoc_parse_policy("", 0) == 0, "scan: reset");
    expect(file_assoc_scan_programs(deny_all) == 0, "scan: predicate that denies everything");
    expect(file_assoc_count() == 0, "scan: no entries with a false predicate");
    expect(file_assoc_scan_examined() == 0, "scan: opened no binary");
}

static void selftest_actions(void)
{
    static const char *const extensions[] = {"/image.BMP", "/image.PNG", "/image.JPG", "/image.JpEg"};
    static const char choices[] = ".png=/bin/notepad\nedit:.bmp=/bin/notepad\n";
    unsigned i;

    (void)file_assoc_parse_policy("", 0);
    expect(load_policy_file(FILE_ASSOC_DEFAULTS_PATH, 0) == 8, "shipped defaults define eight actions");
    expect(file_assoc_count() == 4, "Open and Edit share one extension slot");
    for (i = 0; i < sizeof(extensions) / sizeof(extensions[0]); ++i)
    {
        const char *open = file_assoc_program_for_file(extensions[i]);
        const char *edit = file_assoc_editor_for_file(extensions[i]);
        expect(open && !strcmp(open, "/system/imgview.sxe"), "images default to Image Viewer");
        expect(edit && !strcmp(edit, "/bin/paint"), "image Edit defaults to Paint, including JPEG");
    }
    (void)file_assoc_scan_programs(0);
    expect(!strcmp(file_assoc_program_for_file("/image.png"), "/system/imgview.sxe"), "Paint capability cannot replace viewer default");
    expect(file_assoc_program_for_file("/page.html") &&
        !strcmp(file_assoc_program_for_file("/page.html"), "/system/webview.sxe"), "image defaults preserve HTML association");
    expect(file_assoc_program_for_file("/page.htm") &&
        !strcmp(file_assoc_program_for_file("/page.htm"), "/system/webview.sxe"), "image defaults preserve HTM association");
    expect(file_assoc_editor_for_file("/notes.txt") == 0, "Open capability does not imply Edit");
    expect(file_assoc_editor_for_file("/unknown") == 0, "Edit has no fallback without an extension");

    /* Choices after defaults override only their own action. */
    expect(parse_entries(choices, sizeof(choices) - 1, 1) == 2, "independent user choices parsed");
    expect(!strcmp(file_assoc_program_for_file("/image.png"), "/bin/notepad"), "legacy user Open overrides default");
    expect(!strcmp(file_assoc_editor_for_file("/image.png"), "/bin/paint"), "user Open preserves default Edit");
    expect(!strcmp(file_assoc_editor_for_file("/image.bmp"), "/bin/notepad"), "user Edit overrides default");
    expect(!strcmp(file_assoc_program_for_file("/image.bmp"), "/system/imgview.sxe"), "user Edit preserves default Open");

    /* Production load order: choices, defaults, capabilities. */
    (void)file_assoc_parse_policy(choices, sizeof(choices) - 1);
    (void)load_policy_file(FILE_ASSOC_DEFAULTS_PATH, 0);
    (void)file_assoc_scan_programs(0);
    expect(!strcmp(file_assoc_program_for_file("/image.png"), "/bin/notepad"), "earlier user Open survives defaults and scan");
    expect(!strcmp(file_assoc_editor_for_file("/image.bmp"), "/bin/notepad"), "earlier user Edit survives defaults and scan");
    expect(!strcmp(file_assoc_program_for_file("/image.bmp"), "/system/imgview.sxe"), "Edit-only entry accepts default Open");

    (void)file_assoc_parse_policy("edit:.txt=/bin/paint\n", sizeof("edit:.txt=/bin/paint\n") - 1);
    expect(file_assoc_program_for_file("/notes.txt") == 0, "Edit-only entry has no Open yet");
    (void)file_assoc_scan_programs(0);
    expect(!strcmp(file_assoc_program_for_file("/notes.txt"), "/bin/notepad"), "capability fills Open beside user Edit");
    expect(!strcmp(file_assoc_editor_for_file("/notes.txt"), "/bin/paint"), "scan preserves Edit-only choice");
    (void)file_assoc_parse_policy("open:.png=/bin/notepad\nprint:.png=/bin/paint\n", sizeof("open:.png=/bin/notepad\nprint:.png=/bin/paint\n") - 1);
    expect(file_assoc_count() == 1 && file_assoc_program_for_file("/image.png") &&
        !strcmp(file_assoc_program_for_file("/image.png"), "/bin/notepad"), "explicit Open prefix and unknown verb rejection");
    (void)file_assoc_load();
}

int file_assoc_selftest(void)
{
    g_selftest_failures = 0;

    selftest_extension_of();
    selftest_policy();
    selftest_scan();
    selftest_precedence();
    selftest_actions();

    return g_selftest_failures;
}
