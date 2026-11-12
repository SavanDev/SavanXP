/*
 * Log Viewer, a SUN program (docs/userland/LOG_VIEWER.md).
 *
 * The system log on screen: every line the kernel and the programs sent to
 * the serial port, which on real hardware nobody can read without a cable.
 * It reads the kernel's ring with sxn_log_read (docs/kernel/SYSTEM_LOG.md)
 * and keeps its own copy, so it remembers more than the ring holds while it
 * stays open.
 *
 * CATEGORIES. A kernel line names its subsystem the way every driver writes
 * it, "xhci: ...", and that prefix is its category. A program's line is
 * filed under the program's name. "Problems" gathers the lines that read
 * like an error or a warning, by their words: the log has no levels.
 *
 * File > Save Log... writes the lines shown to a file chosen in the common
 * Save As dialog (include/sxn/sxnfiledlg.h), in --print's format.
 *
 * Without a window, `logview --print [category]` writes the log to stdout,
 * for the text console; `--selftest` checks the reading, the filing and the
 * saving.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "abi/savanxp_abi.h"
#include "sxgfx/gfx2d.h"
#include "sxgfx/pixels.h"
#include "sxgfx/sxgui.h"
#include "sxn/sxn.h"
#include "sxn/sxnfiledlg.h"
#include "sxn/sxngui.h"

#include "logview.sunres.h"

#define LV_MAX_ENTRIES 4096
#define LV_DROP_WHEN_FULL 512
#define LV_MAX_CATEGORIES 128
#define LV_TAG_MAX 24
#define LV_ROW_CAPACITY 288
#define LV_READ_BATCH 64
#define LV_DETAIL_LINES 8
/* Where Save Log... starts, and the file the selftest writes. */
#define LV_SAVE_FOLDER "/users/user"
#define LV_SELFTEST_FILE "/users/logview-selftest.txt"

#define LV_DEFAULT_WIDTH 760
#define LV_DEFAULT_HEIGHT 520
#define LV_CATEGORY_WIDTH 190
#define LV_TIME_WIDTH 76
#define LV_SOURCE_WIDTH 110

enum lv_severity {
    LV_NORMAL = 0,
    LV_WARNING = 1,
    LV_ERROR = 2,
};

/* What a category row selects. */
enum lv_category_kind {
    LV_CAT_ALL = 0,
    LV_CAT_PROBLEMS,
    LV_CAT_KERNEL,  /* every kernel line */
    LV_CAT_TAG,     /* kernel lines with one prefix */
    LV_CAT_PROGRAMS,
    LV_CAT_PROGRAM, /* one program's lines */
};

struct lv_entry {
    struct sxn_log_record record;
    int category;  /* index into g_categories: a TAG or a PROGRAM */
    int severity;
};

struct lv_category {
    int kind;
    char name[LV_TAG_MAX];
    int count;
};

/* ---- the model ------------------------------------------------------------ */

static struct lv_entry g_entries[LV_MAX_ENTRIES];
static int g_entry_count;
static uint64_t g_next_sequence = 1;
static struct sxn_log_record g_batch[LV_READ_BATCH];

/* The fixed rows come first, in this order; tags and programs are added as
 * they appear and shown sorted under their group. */
static struct lv_category g_categories[LV_MAX_CATEGORIES];
static int g_category_count;

static void category_reset(void) {
    static const int fixed[] = {LV_CAT_ALL, LV_CAT_PROBLEMS, LV_CAT_KERNEL, LV_CAT_PROGRAMS};
    int index;
    g_category_count = 0;
    for (index = 0; index < (int)(sizeof(fixed) / sizeof(fixed[0])); ++index) {
        memset(&g_categories[g_category_count], 0, sizeof(g_categories[0]));
        g_categories[g_category_count].kind = fixed[index];
        g_category_count += 1;
    }
}

static int category_find_or_add(int kind, const char* name) {
    int index;
    for (index = 0; index < g_category_count; ++index) {
        if (g_categories[index].kind == kind && strcmp(g_categories[index].name, name) == 0) {
            return index;
        }
    }
    if (g_category_count >= LV_MAX_CATEGORIES) {
        /* Out of rows: the line still shows under All and its group. */
        return kind == LV_CAT_TAG ? 2 : 3;
    }
    memset(&g_categories[g_category_count], 0, sizeof(g_categories[0]));
    g_categories[g_category_count].kind = kind;
    (void)snprintf(g_categories[g_category_count].name, LV_TAG_MAX, "%s", name);
    return g_category_count++;
}

/* "xhci: port 3 ..." -> "xhci". A prefix of letters, digits, '-', '_' and
 * '.' that ends in ':' and a space. Lines without one are "(other)". */
static void kernel_tag(const char* text, char* tag) {
    int length = 0;
    while (length < LV_TAG_MAX - 1) {
        char c = text[length];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
              c == '_' || c == '.')) {
            break;
        }
        length += 1;
    }
    if (length > 0 && text[length] == ':' && (text[length + 1] == ' ' || text[length + 1] == '\0')) {
        memcpy(tag, text, (size_t)length);
        tag[length] = '\0';
        return;
    }
    strcpy(tag, "(other)");
}

/* "nightwood.sxe" -> "nightwood": the name a person calls it. */
static void program_name(const char* source, char* name) {
    size_t length;
    (void)snprintf(name, LV_TAG_MAX, "%s", source[0] != '\0' ? source : "(unnamed)");
    length = strlen(name);
    if (length > 4 && strcmp(name + length - 4, ".sxe") == 0) {
        name[length - 4] = '\0';
    }
}

/* `word` at the start of a word ("failed", "errors", "timeout" all count),
 * but not as a counter's name: "fault=0" in a stats line is a number, not a
 * fault. */
static int has_word(const char* lower, const char* word) {
    size_t length = strlen(word);
    const char* at = lower;
    while ((at = strstr(at, word)) != 0) {
        const char* end = at + length;
        int starts_word = at == lower || !((at[-1] >= 'a' && at[-1] <= 'z') || (at[-1] >= '0' && at[-1] <= '9'));
        while ((*end >= 'a' && *end <= 'z') || *end == '_') {
            end += 1;
        }
        if (starts_word && *end != '=') {
            return 1;
        }
        at += 1;
    }
    return 0;
}

static int classify(const char* text) {
    char lower[SXN_LOG_TEXT_MAX];
    size_t index;
    for (index = 0; index + 1 < sizeof(lower) && text[index] != '\0'; ++index) {
        char c = text[index];
        lower[index] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    lower[index] = '\0';
    if (has_word(lower, "panic") || has_word(lower, "fatal") || has_word(lower, "error") ||
        has_word(lower, "fail") || has_word(lower, "fault") || has_word(lower, "corrupt")) {
        return LV_ERROR;
    }
    if (has_word(lower, "warn") || has_word(lower, "timeout") || has_word(lower, "timed out") ||
        has_word(lower, "unsupported") || has_word(lower, "retry") || has_word(lower, "retrying") ||
        has_word(lower, "lost")) {
        return LV_WARNING;
    }
    return LV_NORMAL;
}

/* A continued line belongs where its first part went. */
static int continued_category(uint32_t pid) {
    int index;
    for (index = g_entry_count - 1; index >= 0; --index) {
        if (g_entries[index].record.pid == pid) {
            return g_entries[index].category;
        }
    }
    return -1;
}

static void count_entry(const struct lv_entry* entry, int delta) {
    g_categories[0].count += delta;
    if (entry->severity != LV_NORMAL) {
        g_categories[1].count += delta;
    }
    g_categories[entry->record.pid == 0u ? 2 : 3].count += delta;
    if (entry->category > 3) {
        g_categories[entry->category].count += delta;
    }
}

static void add_record(const struct sxn_log_record* record) {
    struct lv_entry* entry;
    char name[LV_TAG_MAX];
    int category = -1;

    if (g_entry_count == LV_MAX_ENTRIES) {
        /* Full: forget the oldest block at once rather than one line per
         * line read. */
        int index;
        for (index = 0; index < LV_DROP_WHEN_FULL; ++index) {
            count_entry(&g_entries[index], -1);
        }
        memmove(&g_entries[0], &g_entries[LV_DROP_WHEN_FULL],
            sizeof(g_entries[0]) * (size_t)(LV_MAX_ENTRIES - LV_DROP_WHEN_FULL));
        g_entry_count -= LV_DROP_WHEN_FULL;
    }

    if ((record->flags & SXN_LOG_CONTINUED) != 0u) {
        category = continued_category(record->pid);
    }
    if (category < 0) {
        if (record->pid == 0u) {
            kernel_tag(record->text, name);
            category = category_find_or_add(LV_CAT_TAG, name);
        } else {
            program_name(record->source, name);
            category = category_find_or_add(LV_CAT_PROGRAM, name);
        }
    }

    entry = &g_entries[g_entry_count++];
    entry->record = *record;
    entry->category = category;
    entry->severity = classify(record->text);
    count_entry(entry, 1);
}

/* Reads whatever the kernel logged since the last call. Returns how many
 * lines arrived. */
static int read_new(void) {
    int total = 0;
    long count;
    while ((count = sxn_log_read(g_next_sequence, g_batch, LV_READ_BATCH)) > 0) {
        long index;
        for (index = 0; index < count; ++index) {
            add_record(&g_batch[index]);
            g_next_sequence = g_batch[index].sequence + 1u;
        }
        total += (int)count;
    }
    return total;
}

static void clear_entries(void) {
    g_entry_count = 0;
    category_reset();
}

static int entry_in_category(const struct lv_entry* entry, int category) {
    switch (g_categories[category].kind) {
    case LV_CAT_ALL:
        return 1;
    case LV_CAT_PROBLEMS:
        return entry->severity != LV_NORMAL;
    case LV_CAT_KERNEL:
        return entry->record.pid == 0u;
    case LV_CAT_PROGRAMS:
        return entry->record.pid != 0u;
    default:
        return entry->category == category;
    }
}

/* Case-insensitive, for the Find box. */
static int contains_folded(const char* text, const char* needle) {
    size_t length = strlen(needle);
    if (length == 0) {
        return 1;
    }
    for (; *text != '\0'; ++text) {
        size_t index;
        for (index = 0; index < length; ++index) {
            char a = text[index];
            char b = needle[index];
            if (a == '\0') {
                return 0;
            }
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) {
                break;
            }
        }
        if (index == length) {
            return 1;
        }
    }
    return 0;
}

static void format_time(uint64_t ns, char* out, size_t capacity) {
    unsigned long long ms = (unsigned long long)(ns / 1000000u);
    (void)snprintf(out, capacity, "%llu.%03llu", ms / 1000u, ms % 1000u);
}

static void format_source(const struct sxn_log_record* record, char* out, size_t capacity) {
    char name[LV_TAG_MAX];
    if (record->pid == 0u) {
        (void)snprintf(out, capacity, "kernel");
        return;
    }
    program_name(record->source, name);
    (void)snprintf(out, capacity, "%s [%u]", name, (unsigned)record->pid);
}

/* One line as --print and Save Log... write it. */
static void format_line(const struct lv_entry* entry, char* out, size_t capacity) {
    char time[24];
    char source[48];
    format_time(entry->record.time_ns, time, sizeof(time));
    format_source(&entry->record, source, sizeof(source));
    (void)snprintf(out, capacity, "[%10s] %-16s %s\n", time, source, entry->record.text);
}

/* ---- saving --------------------------------------------------------------------- */

static char g_save_buffer[8192];

/* Writes the entries at `indices` (all of them when NULL) to `path`,
 * replacing what it held, and makes it durable. The line count, or -errno. */
static long save_entries(const char* path, const int* indices, int count) {
    char line[LV_ROW_CAPACITY + 64];
    size_t used = 0;
    long handle = sxn_create_file(path, SXN_CREATE_ALWAYS);
    long result = 0;
    int index;

    if (handle < 0) {
        return handle;
    }
    for (index = 0; index < count && result >= 0; ++index) {
        size_t length;
        format_line(&g_entries[indices != 0 ? indices[index] : index], line, sizeof(line));
        length = strlen(line);
        if (used + length > sizeof(g_save_buffer)) {
            result = sxn_write(handle, g_save_buffer, used);
            if (result >= 0 && (size_t)result != used) result = -SAVANXP_EIO;
            used = 0;
        }
        memcpy(g_save_buffer + used, line, length);
        used += length;
    }
    if (result >= 0 && used > 0) {
        result = sxn_write(handle, g_save_buffer, used);
        if (result >= 0 && (size_t)result != used) result = -SAVANXP_EIO;
    }
    if (result >= 0) {
        result = sxn_flush_file(handle);
    }
    (void)sxn_close(handle);
    return result < 0 ? result : count;
}

/* ---- --print ---------------------------------------------------------------- */

static void write_out(const char* text) {
    (void)sxn_write(1, text, strlen(text));
}

static int category_named(const char* name) {
    int index;
    if (name == 0 || strcmp(name, "all") == 0) return 0;
    if (strcmp(name, "problems") == 0) return 1;
    if (strcmp(name, "kernel") == 0) return 2;
    if (strcmp(name, "programs") == 0) return 3;
    for (index = 4; index < g_category_count; ++index) {
        if (strcmp(g_categories[index].name, name) == 0) return index;
    }
    return -1;
}

static int run_print(const char* name) {
    char line[LV_ROW_CAPACITY + 64];
    int category;
    int index;

    category_reset();
    (void)read_new();
    category = category_named(name);
    if (category < 0) {
        (void)snprintf(line, sizeof(line), "logview: no category '%s'\n", name);
        write_out(line);
        return 1;
    }
    for (index = 0; index < g_entry_count; ++index) {
        const struct lv_entry* entry = &g_entries[index];
        if (!entry_in_category(entry, category)) continue;
        format_line(entry, line, sizeof(line));
        write_out(line);
    }
    return 0;
}

/* ---- --selftest --------------------------------------------------------------- */

static int run_selftest(void) {
    struct sxn_native_info self;
    char tag[LV_TAG_MAX];
    int failures = 0;
    int found_own = -1;
    int kernel_tags = 0;
    int index;

#define LV_CHECK(ok)                                                  \
    do {                                                              \
        if (!(ok)) {                                                  \
            char message[96];                                         \
            (void)snprintf(message, sizeof(message), "logview: selftest check failed at line %d", __LINE__); \
            (void)sxn_log(message);                                   \
            failures += 1;                                            \
        }                                                             \
    } while (0)

    kernel_tag("xhci: port 3 connected", tag);
    LV_CHECK(strcmp(tag, "xhci") == 0);
    kernel_tag("virtio-gpu: ready", tag);
    LV_CHECK(strcmp(tag, "virtio-gpu") == 0);
    kernel_tag("SavanXP boot", tag);
    LV_CHECK(strcmp(tag, "(other)") == 0);
    LV_CHECK(classify("ahci: port 1 FAILED to start") == LV_ERROR);
    LV_CHECK(classify("usb: reset timed out, retrying") == LV_WARNING);
    LV_CHECK(classify("pci: 12 devices") == LV_NORMAL);
    LV_CHECK(classify("smp-stats: fault=0 errors=0") == LV_NORMAL);
    LV_CHECK(classify("xhci: Error on port 2") == LV_ERROR);

    LV_CHECK(sxn_info(&self) == 0);
    (void)sxn_log("logview: selftest marker");
    category_reset();
    LV_CHECK(read_new() > 0);
    for (index = 0; index < g_entry_count; ++index) {
        const struct lv_entry* entry = &g_entries[index];
        if (entry->record.pid == self.pid && strstr(entry->record.text, "logview: selftest marker") != 0) {
            found_own = entry->category;
        }
    }
    LV_CHECK(found_own >= 0 && g_categories[found_own].kind == LV_CAT_PROGRAM &&
        strcmp(g_categories[found_own].name, "logview") == 0);
    for (index = 4; index < g_category_count; ++index) {
        if (g_categories[index].kind == LV_CAT_TAG && g_categories[index].count > 0) {
            kernel_tags += 1;
        }
    }
    LV_CHECK(kernel_tags >= 2 && g_categories[2].count > 0 && g_categories[0].count == g_entry_count);
    /* Nothing new means nothing read twice. */
    index = g_entry_count;
    LV_CHECK(read_new() == 0 && g_entry_count == index);

    /* Save Log...: the whole log, larger than one small save, and read back. */
    {
        struct sxn_file_info info;
        char first[LV_ROW_CAPACITY + 64];
        char back[64];
        long handle;
        uint64_t expected = 0;

        for (index = 0; index < g_entry_count; ++index) {
            format_line(&g_entries[index], first, sizeof(first));
            expected += strlen(first);
        }
        LV_CHECK(save_entries(LV_SELFTEST_FILE, 0, g_entry_count) == g_entry_count);
        LV_CHECK(sxn_query_path(LV_SELFTEST_FILE, &info) == 0 && info.size == expected && expected > SXN_WRITE_FILE_MAX);
        format_line(&g_entries[0], first, sizeof(first));
        handle = sxn_open_file(LV_SELFTEST_FILE, SXN_FILE_READ);
        LV_CHECK(handle >= 0);
        if (handle >= 0) {
            size_t want = strlen(first) < sizeof(back) ? strlen(first) : sizeof(back);
            LV_CHECK(sxn_read(handle, back, want) == (long)want && memcmp(back, first, want) == 0);
            (void)sxn_close(handle);
        }
        /* A second save replaces the file, it does not append. */
        LV_CHECK(save_entries(LV_SELFTEST_FILE, 0, 1) == 1);
        LV_CHECK(sxn_query_path(LV_SELFTEST_FILE, &info) == 0 && info.size == strlen(first));
        LV_CHECK(save_entries("/no-such-folder/log.txt", 0, 1) == -SAVANXP_ENOENT);
    }
    LV_CHECK(sxn_file_dialog_selftest() == 0);
#undef LV_CHECK

    (void)sxn_log(failures == 0 ? "logview: selftest passed" : "logview: selftest failed");
    return failures == 0 ? 0 : 1;
}

/* ---- the window --------------------------------------------------------------- */

enum {
    LV_FIND_LABEL,
    LV_FIND,
    LV_FOLLOW,
    LV_COPY,
    LV_CLEAR,
    LV_CATEGORIES,
    LV_LIST,
    LV_DETAIL,
    LV_STATUS,
    LV_WIDGET_COUNT
};

enum {
    LV_CMD_EXIT = 1,
    LV_CMD_SAVE,
    LV_CMD_COPY,
    LV_CMD_CLEAR,
    LV_CMD_FOLLOW,
    LV_CMD_REFRESH,
};

static struct sxngui_app g_app;
static struct sxgui_widget g_widgets[LV_WIDGET_COUNT];
static struct sxgui_menubar g_menubar;

static struct sxgui_menu_item g_file_items[] = {
    {"Save Log...\tCtrl+S", LV_CMD_SAVE, 0},
    {0, 0, 0},
    {"Exit", LV_CMD_EXIT, 0},
};
static struct sxgui_menu_item g_edit_items[] = {
    {"Copy Line\tCtrl+C", LV_CMD_COPY, 0},
};
static struct sxgui_menu_item g_view_items[] = {
    {"Follow New Lines", LV_CMD_FOLLOW, SXGUI_MENU_CHECKED},
    {"Refresh\tF5", LV_CMD_REFRESH, 0},
    {0, 0, 0},
    {"Clear", LV_CMD_CLEAR, 0},
};
static const struct sxgui_menu g_menus[] = {
    {"File", g_file_items, 3},
    {"Edit", g_edit_items, 1},
    {"View", g_view_items, 4},
};

static char g_find[64];
static char g_status[128];
/* A message the status bar keeps over the line count until then. */
static unsigned long g_notice_until_ms;

static struct sxgui_column g_columns[] = {
    {"Time", LV_TIME_WIDTH, SXGUI_COLUMN_RIGHT},
    {"Source", LV_SOURCE_WIDTH, 0},
    {"Message", 400, 0},
};

/* The category list: one row per entry of g_category_order. */
static int g_category_order[LV_MAX_CATEGORIES];
static char g_category_rows[LV_MAX_CATEGORIES][LV_TAG_MAX + 24];
static const char* g_category_row_ptrs[LV_MAX_CATEGORIES];
static int g_selected_category; /* index into g_categories */

/* The visible lines: one row per entry that passes the category and Find. */
static int g_row_entry[LV_MAX_ENTRIES];
static char g_rows[LV_MAX_ENTRIES][LV_ROW_CAPACITY];
static const char* g_row_ptrs[LV_MAX_ENTRIES];
static const struct sx_bitmap* g_row_icons[LV_MAX_ENTRIES];
static int g_row_count;
static uint64_t g_selected_sequence; /* 0 = none */

static char g_detail_lines[LV_DETAIL_LINES][SXN_LOG_TEXT_MAX + 64];
static const char* g_detail_ptrs[LV_DETAIL_LINES];

/* The two severity marks, drawn once at start. */
static uint32_t g_icon_pixels[2][16 * 16];
static struct sx_bitmap g_icons[2];

static void build_icons(void) {
    static const struct savanxp_fb_info info = {16, 16, 16 * 4, 32, 16 * 16 * 4};
    int y;
    int x;
    for (y = 0; y < 16; ++y) {
        for (x = 0; x < 16; ++x) {
            /* Error: a red disc with a white bar. */
            int dx = 2 * x - 15;
            int dy = 2 * y - 15;
            int inside = dx * dx + dy * dy <= 13 * 13;
            uint32_t pixel = 0;
            if (inside) {
                pixel = (y >= 7 && y <= 8 && x >= 4 && x <= 11) ? 0xffffffffu : 0xffc81e1eu;
            } else if (dx * dx + dy * dy <= 15 * 15) {
                pixel = 0xff7a0e0eu;
            }
            g_icon_pixels[0][y * 16 + x] = pixel;

            /* Warning: a yellow triangle with a black stroke. */
            pixel = 0;
            if (y >= 2 && y <= 14) {
                int half = (y - 2) / 2 + 1;
                if (x >= 8 - half && x < 8 + half) {
                    int edge = x == 8 - half || x == 8 + half - 1 || y == 14;
                    pixel = edge ? 0xff404000u : 0xfff0c800u;
                    if ((x == 7 || x == 8) && ((y >= 6 && y <= 10) || y == 12)) {
                        pixel = 0xff000000u;
                    }
                }
            }
            g_icon_pixels[1][y * 16 + x] = pixel;
        }
    }
    sx_bitmap_wrap(&g_icons[0], g_icon_pixels[0], &info, SX_PIXEL_FORMAT_BGRA8888);
    sx_bitmap_wrap(&g_icons[1], g_icon_pixels[1], &info, SX_PIXEL_FORMAT_BGRA8888);
}

static int compare_categories(int left, int right) {
    return strcmp(g_categories[left].name, g_categories[right].name);
}

static void append_sorted_group(int kind, int* rows) {
    int start = *rows;
    int index;
    for (index = 4; index < g_category_count; ++index) {
        int at;
        if (g_categories[index].kind != kind || g_categories[index].count == 0) continue;
        /* Insertion sort: a few dozen names at most. */
        at = *rows;
        while (at > start && compare_categories(g_category_order[at - 1], index) > 0) {
            g_category_order[at] = g_category_order[at - 1];
            at -= 1;
        }
        g_category_order[at] = index;
        *rows += 1;
    }
}

static void rebuild_categories(void) {
    static const char* const titles[] = {"All lines", "Problems", "Kernel", "Programs"};
    struct sxgui_widget* list = &g_widgets[LV_CATEGORIES];
    int rows = 0;
    int index;

    /* Programs before the kernel: a few names against dozens of
     * subsystems, which would push them out of view. */
    g_category_order[rows++] = 0;
    g_category_order[rows++] = 1;
    g_category_order[rows++] = 3;
    append_sorted_group(LV_CAT_PROGRAM, &rows);
    g_category_order[rows++] = 2;
    append_sorted_group(LV_CAT_TAG, &rows);

    list->value = 0;
    for (index = 0; index < rows; ++index) {
        const struct lv_category* category = &g_categories[g_category_order[index]];
        if (category->kind == LV_CAT_TAG || category->kind == LV_CAT_PROGRAM) {
            (void)snprintf(g_category_rows[index], sizeof(g_category_rows[index]), "    %s (%d)", category->name,
                category->count);
        } else {
            (void)snprintf(g_category_rows[index], sizeof(g_category_rows[index]), "%s (%d)",
                titles[category->kind == LV_CAT_ALL ? 0 : category->kind == LV_CAT_PROBLEMS ? 1
                    : category->kind == LV_CAT_KERNEL ? 2 : 3],
                category->count);
        }
        g_category_row_ptrs[index] = g_category_rows[index];
        if (g_category_order[index] == g_selected_category) {
            list->value = index;
        }
    }
    list->item_count = rows;
    /* A category that emptied (Clear) is no longer listed: back to All. */
    g_selected_category = g_category_order[list->value];
}

static int list_visible_rows(const struct sxgui_widget* list) {
    int header = gfx_text_height() + SXGUI_BORDER_RAISED * 2 + 2;
    int rows = (list->rect.height - SXGUI_BORDER_SUNKEN * 2 - header) / (gfx_text_height() + 4);
    return rows > 0 ? rows : 1;
}

static void update_status(void) {
    if (sxngui_uptime_ms() < g_notice_until_ms) {
        return;
    }
    (void)snprintf(g_status, sizeof(g_status), "%d of %d lines shown", g_row_count, g_entry_count);
}

/* Wraps the selected line into the detail panel. */
static void update_detail(void) {
    struct sxgui_widget* detail = &g_widgets[LV_DETAIL];
    const struct lv_entry* entry = 0;
    int width = detail->rect.width - SXGUI_BORDER_SUNKEN * 2 - SXGUI_TEXT_PAD * 2 - SXGUI_SCROLLBAR_THICKNESS;
    int lines = 0;
    int index;

    for (index = 0; index < g_entry_count; ++index) {
        if (g_entries[index].record.sequence == g_selected_sequence) {
            entry = &g_entries[index];
            break;
        }
    }
    if (entry == 0) {
        (void)snprintf(g_detail_lines[0], sizeof(g_detail_lines[0]), "Select a line to read it in full.");
        lines = 1;
    } else {
        char time[24];
        char source[48];
        const char* text = entry->record.text;
        format_time(entry->record.time_ns, time, sizeof(time));
        format_source(&entry->record, source, sizeof(source));
        (void)snprintf(g_detail_lines[0], sizeof(g_detail_lines[0]), "%s s    %s    line %llu%s", time, source,
            (unsigned long long)entry->record.sequence,
            entry->severity == LV_ERROR ? "    error" : entry->severity == LV_WARNING ? "    warning" : "");
        lines = 1;
        while (*text != '\0' && lines < LV_DETAIL_LINES) {
            char* out = g_detail_lines[lines];
            size_t length = 0;
            size_t last_space = 0;
            while (text[length] != '\0' && length + 1 < sizeof(g_detail_lines[0])) {
                out[length] = text[length];
                out[length + 1] = '\0';
                if (width > 0 && gfx_text_width(out) > width && length > 0) {
                    break;
                }
                if (text[length] == ' ') last_space = length;
                length += 1;
            }
            if (text[length] != '\0' && last_space > 0) {
                length = last_space + 1;
            }
            out[length] = '\0';
            text += length;
            lines += 1;
        }
    }
    for (index = 0; index < LV_DETAIL_LINES; ++index) {
        g_detail_ptrs[index] = g_detail_lines[index];
    }
    detail->item_count = lines;
    detail->scroll = 0;
}

/* Rebuilds the visible rows; keeps the selected line selected and, while
 * following, the newest one in view. */
static void rebuild_rows(int follow) {
    struct sxgui_widget* list = &g_widgets[LV_LIST];
    int index;
    int selected_row = -1;

    g_row_count = 0;
    for (index = 0; index < g_entry_count; ++index) {
        const struct lv_entry* entry = &g_entries[index];
        char time[24];
        char source[48];
        if (!entry_in_category(entry, g_selected_category)) continue;
        if (g_find[0] != '\0' && !contains_folded(entry->record.text, g_find) &&
            !contains_folded(entry->record.source, g_find)) {
            continue;
        }
        format_time(entry->record.time_ns, time, sizeof(time));
        format_source(&entry->record, source, sizeof(source));
        (void)snprintf(g_rows[g_row_count], LV_ROW_CAPACITY, "%s\t%s\t%s", time, source, entry->record.text);
        g_row_ptrs[g_row_count] = g_rows[g_row_count];
        g_row_icons[g_row_count] = entry->severity == LV_ERROR ? &g_icons[0]
            : entry->severity == LV_WARNING ? &g_icons[1] : 0;
        g_row_entry[g_row_count] = index;
        if (entry->record.sequence == g_selected_sequence) {
            selected_row = g_row_count;
        }
        g_row_count += 1;
    }
    list->item_count = g_row_count;
    list->value = selected_row;
    if (selected_row < 0) {
        g_selected_sequence = 0;
    }
    if (follow) {
        int top = g_row_count - list_visible_rows(list);
        list->scroll = top > 0 ? top : 0;
    } else if (list->scroll > g_row_count) {
        list->scroll = 0;
    }
    update_status();
    update_detail();
}

static int following(void) {
    return g_widgets[LV_FOLLOW].value != 0;
}

static void refresh_all(int follow) {
    rebuild_categories();
    rebuild_rows(follow);
    sxngui_app_request_repaint(&g_app);
}

static void copy_selected(void) {
    const struct sxgui_host* host = sxngui_host();
    char text[LV_ROW_CAPACITY + 64];
    char time[24];
    char source[48];
    int index;

    for (index = 0; index < g_entry_count; ++index) {
        const struct lv_entry* entry = &g_entries[index];
        if (entry->record.sequence != g_selected_sequence) continue;
        format_time(entry->record.time_ns, time, sizeof(time));
        format_source(&entry->record, source, sizeof(source));
        (void)snprintf(text, sizeof(text), "[%s] %s: %s", time, source, entry->record.text);
        if (host != 0 && host->clipboard_set_text != 0) {
            (void)host->clipboard_set_text(text);
            (void)snprintf(g_status, sizeof(g_status), "Copied line %llu to the clipboard.",
                (unsigned long long)entry->record.sequence);
            g_notice_until_ms = sxngui_uptime_ms() + 3000u;
            sxngui_app_request_repaint(&g_app);
        }
        return;
    }
}

static void set_follow(int on) {
    g_widgets[LV_FOLLOW].value = on ? 1 : 0;
    g_view_items[0].flags = on ? SXGUI_MENU_CHECKED : 0u;
    if (on) {
        rebuild_rows(1);
    }
    sxngui_app_request_repaint(&g_app);
}

static void clear_view(void) {
    clear_entries();
    g_selected_category = 0;
    g_selected_sequence = 0;
    refresh_all(1);
}

static void show_notice(void) {
    g_notice_until_ms = sxngui_uptime_ms() + 5000u;
    sxngui_app_request_repaint(&g_app);
}

static void on_save_chosen(struct sxngui_app* app, const char* path, void* user) {
    long saved;
    (void)app;
    (void)user;
    if (path == 0) {
        return;
    }
    saved = save_entries(path, g_row_entry, g_row_count);
    if (saved < 0) {
        (void)snprintf(g_status, sizeof(g_status), "Could not save %.80s (error %ld).", path, -saved);
    } else {
        (void)snprintf(g_status, sizeof(g_status), "Saved %ld lines to %.80s.", saved, path);
    }
    show_notice();
}

/* Saves the lines shown: the category and Find chosen are what is saved. */
static void begin_save(void) {
    struct sxn_file_dialog_request request;
    memset(&request, 0, sizeof(request));
    request.mode = SXN_FILE_DIALOG_SAVE;
    request.title = "Save Log As";
    request.initial_directory = LV_SAVE_FOLDER;
    request.file_name = "system-log.txt";
    request.filter_description = "Text files (*.txt; *.log)";
    request.filter_patterns = "*.txt;*.log";
    request.default_extension = "txt";
    request.on_done = on_save_chosen;
    if (sxn_file_dialog_begin(&g_app, &request) != 0) {
        (void)snprintf(g_status, sizeof(g_status), "Not enough memory for the Save As dialog.");
        show_notice();
    }
}

static void on_menu_command(int id, void* user) {
    (void)user;
    switch (id) {
    case LV_CMD_EXIT:
        sxngui_app_quit(&g_app, 0);
        break;
    case LV_CMD_SAVE:
        begin_save();
        break;
    case LV_CMD_COPY:
        copy_selected();
        break;
    case LV_CMD_CLEAR:
        clear_view();
        break;
    case LV_CMD_FOLLOW:
        set_follow(!following());
        break;
    case LV_CMD_REFRESH:
        (void)read_new();
        refresh_all(following());
        break;
    default:
        break;
    }
}

static void on_widget(struct sxgui_widget* widget, void* user) {
    (void)user;
    if (widget == &g_widgets[LV_CATEGORIES]) {
        if (widget->value >= 0 && widget->value < widget->item_count) {
            g_selected_category = g_category_order[widget->value];
            g_widgets[LV_LIST].scroll = 0;
            rebuild_rows(following());
        }
    } else if (widget == &g_widgets[LV_LIST]) {
        if (widget->value >= 0 && widget->value < g_row_count) {
            g_selected_sequence = g_entries[g_row_entry[widget->value]].record.sequence;
            /* Picking an older line stops the list from jumping away. */
            if (widget->value < g_row_count - 1 && following()) {
                set_follow(0);
            }
            update_detail();
        }
    } else if (widget == &g_widgets[LV_FIND]) {
        rebuild_rows(following());
    } else if (widget == &g_widgets[LV_FOLLOW]) {
        set_follow(widget->value);
    } else if (widget == &g_widgets[LV_COPY]) {
        copy_selected();
    } else if (widget == &g_widgets[LV_CLEAR]) {
        clear_view();
    }
    sxngui_app_request_repaint(&g_app);
}

static int on_key(struct sxngui_app* app, const struct savanxp_input_event* event) {
    (void)app;
    if (event->type != SAVANXP_INPUT_EVENT_KEY_DOWN) {
        return 0;
    }
    if (event->key == SAVANXP_KEY_F5) {
        on_menu_command(LV_CMD_REFRESH, 0);
        return 1;
    }
    if ((event->modifiers & SAVANXP_KEY_MOD_CTRL) != 0 && (event->ascii == 's' || event->ascii == 'S' ||
        event->ascii == 19) && !sxgui_dialog_active(&g_app.ui)) {
        begin_save();
        return 1;
    }
    /* Ctrl+C in the Find box copies its text, as anywhere else. */
    if ((event->modifiers & SAVANXP_KEY_MOD_CTRL) != 0 && (event->ascii == 'c' || event->ascii == 'C' ||
        event->ascii == 3) && !g_widgets[LV_FIND].focused) {
        copy_selected();
        return 1;
    }
    return 0;
}

static void on_tick(struct sxngui_app* app) {
    (void)app;
    if (read_new() > 0) {
        refresh_all(following());
    }
}

static void layout(struct sxngui_app* app) {
    int width = (int)app->info.width;
    int height = (int)app->info.height;
    int top = sxgui_menubar_height() + SXGUI_MARGIN;
    int x = SXGUI_MARGIN;
    int detail_height = gfx_text_height() * 5 + SXGUI_BORDER_SUNKEN * 2 + 8;
    int status_y = height - SXGUI_STATUS_HEIGHT - 2;
    int body_top = top + SXGUI_FIELD_HEIGHT + SXGUI_GAP;
    int detail_y = status_y - SXGUI_GAP - detail_height;
    int list_x = x + LV_CATEGORY_WIDTH + SXGUI_GAP;
    int list_width = width - list_x - SXGUI_MARGIN;
    int message;
    int find_width;

    g_widgets[LV_FIND_LABEL].rect = sx_rect_make(x, top + 4, 36, SXGUI_FIELD_HEIGHT - 4);
    g_widgets[LV_CLEAR].rect = sx_rect_make(width - SXGUI_MARGIN - 72, top - 1, 72, SXGUI_FIELD_HEIGHT + 2);
    g_widgets[LV_COPY].rect = sx_rect_make(width - SXGUI_MARGIN - 72 * 2 - SXGUI_GAP, top - 1, 72,
        SXGUI_FIELD_HEIGHT + 2);
    g_widgets[LV_FOLLOW].rect = sx_rect_make(g_widgets[LV_COPY].rect.x - SXGUI_GAP - 140, top + 2, 140,
        SXGUI_FIELD_HEIGHT - 4);
    find_width = g_widgets[LV_FOLLOW].rect.x - SXGUI_GAP - (x + 40);
    if (find_width > 280) find_width = 280;
    if (find_width < 60) find_width = 60;
    g_widgets[LV_FIND].rect = sx_rect_make(x + 40, top, find_width, SXGUI_FIELD_HEIGHT);
    g_widgets[LV_CATEGORIES].rect = sx_rect_make(x, body_top, LV_CATEGORY_WIDTH, detail_y - SXGUI_GAP - body_top);
    g_widgets[LV_LIST].rect = sx_rect_make(list_x, body_top, list_width, detail_y - SXGUI_GAP - body_top);
    g_widgets[LV_DETAIL].rect = sx_rect_make(x, detail_y, width - SXGUI_MARGIN * 2, detail_height);
    g_widgets[LV_STATUS].rect = sx_rect_make(0, status_y, width, SXGUI_STATUS_HEIGHT);

    message = list_width - SXGUI_BORDER_SUNKEN * 2 - SXGUI_SCROLLBAR_THICKNESS - LV_TIME_WIDTH - LV_SOURCE_WIDTH -
        SXGUI_LISTBOX_ICON_SIZE - SXGUI_LISTBOX_ICON_GAP;
    g_columns[2].width = message > 80 ? message : 80;
    rebuild_rows(following());
    sxngui_app_request_repaint(app);
}

static int run_window(void) {
    int result;

    build_icons();
    category_reset();
    (void)read_new();

    g_widgets[LV_FIND_LABEL] = sxgui_label(sx_rect_make(0, 0, 0, 0), "Find:");
    g_widgets[LV_FIND] = sxgui_textfield(sx_rect_make(0, 0, 0, 0), g_find, (int)sizeof(g_find));
    g_widgets[LV_FOLLOW] = sxgui_checkbox(sx_rect_make(0, 0, 0, 0), "Follow new lines", 1);
    g_widgets[LV_COPY] = sxgui_button(sx_rect_make(0, 0, 0, 0), "Copy", on_widget, 0);
    g_widgets[LV_CLEAR] = sxgui_button(sx_rect_make(0, 0, 0, 0), "Clear", on_widget, 0);
    g_widgets[LV_CATEGORIES] = sxgui_listbox(sx_rect_make(0, 0, 0, 0), g_category_row_ptrs, 0);
    g_widgets[LV_LIST] = sxgui_listbox(sx_rect_make(0, 0, 0, 0), g_row_ptrs, 0);
    g_widgets[LV_LIST].columns = g_columns;
    g_widgets[LV_LIST].column_count = (int)(sizeof(g_columns) / sizeof(g_columns[0]));
    g_widgets[LV_LIST].item_icons = g_row_icons;
    g_widgets[LV_LIST].flags |= SXGUI_FLAG_EMPTY_SELECTION;
    g_widgets[LV_LIST].value = -1;
    g_widgets[LV_DETAIL] = sxgui_textview(sx_rect_make(0, 0, 0, 0), g_detail_ptrs, 0);
    g_widgets[LV_STATUS] = sxgui_label(sx_rect_make(0, 0, 0, 0), g_status);
    g_widgets[LV_STATUS].flags |= SXGUI_FLAG_SUNKEN;
    g_widgets[LV_FIND].on_action = on_widget;
    g_widgets[LV_FOLLOW].on_action = on_widget;
    g_widgets[LV_CATEGORIES].on_action = on_widget;
    g_widgets[LV_LIST].on_action = on_widget;

    result = sxngui_app_init(&g_app, &sun_resources, g_widgets, LV_WIDGET_COUNT);
    if (result < 0) {
        (void)sxn_log("logview: no window from windowd; try logview --print");
        return 1;
    }
    g_menubar.menus = g_menus;
    g_menubar.menu_count = (int)(sizeof(g_menus) / sizeof(g_menus[0]));
    g_menubar.on_command = on_menu_command;
    sxgui_set_menubar(&g_app.ui, &g_menubar);

    g_app.on_resize = layout;
    g_app.on_key = on_key;
    g_app.on_tick = on_tick;
    g_app.tick_interval_ms = 500;
    (void)sxngui_app_set_content_size(&g_app, LV_DEFAULT_WIDTH, LV_DEFAULT_HEIGHT);
    rebuild_categories();
    layout(&g_app);
    sxgui_focus(&g_app.ui, LV_LIST);
    return sxngui_app_run(&g_app);
}

int main(int argc, char** argv) {
    /* A SUN program never returns from main: the stock crt0 would fall
     * through to a POSIX exit, which is ENOSYS here. */
    if (argc > 1 && argv[1] != 0 && strcmp(argv[1], "--selftest") == 0) {
        sxn_exit(run_selftest());
    }
    if (argc > 1 && argv[1] != 0 && strcmp(argv[1], "--print") == 0) {
        sxn_exit(run_print(argc > 2 ? argv[2] : 0));
    }
    if (argc > 1 && argv[1] != 0 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        write_out("usage: logview [--print [all|problems|kernel|programs|<category>]] [--selftest]\n");
        sxn_exit(0);
    }
    sxn_exit(run_window());
    return 0;
}
