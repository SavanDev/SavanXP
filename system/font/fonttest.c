/* libsxfont in a SUN program (docs/desktop/FONT_LIBRARY.md, "Verification"):
 * maps libsxfont.so.0.4, reads the twelve Liberation faces from
 * /system/fonts with SUN's file calls, and runs the self-test the host test
 * runs. "FONT TEST PASS" or "FONT TEST FAIL" ends it; the font-test scenario
 * waits for one of them. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxn/sxn.h"
#include "sxn/sxngui.h"
#include "sxgfx/pixels.h"
#include "sxfont_selftest.h"

#define FONT_DIRECTORY "/system/fonts"

struct loaded {
    char name[64];
    void *data;
    size_t size;
};

struct state {
    struct loaded files[12];
    unsigned count;
    int failures;
};

static void say(const char *text) {
    (void)sxn_log(text);
}

static void report(void *user, const char *line) {
    struct state *state = user;
    char message[240];
    snprintf(message, sizeof message, "fonttest: FAILED %s", line);
    say(message);
    state->failures++;
}

/* The whole file, or NULL. */
static void *read_file(const char *path, size_t *size) {
    long handle = sxn_open_file(path, SXN_FILE_READ);
    if (handle < 0) return NULL;
    struct sxn_file_info info;
    void *data = NULL;
    if (sxn_query_file(handle, &info) == 0 && info.size > 0 && info.size < 64u * 1024u * 1024u) {
        data = malloc((size_t)info.size);
        size_t done = 0;
        while (data != NULL && done < info.size) {
            long got = sxn_read(handle, (char *)data + done, (unsigned long)(info.size - done));
            if (got <= 0) break;
            done += (size_t)got;
        }
        if (data != NULL && done != info.size) {
            free(data);
            data = NULL;
        }
        *size = done;
    }
    (void)sxn_close(handle);
    return data;
}

static const void *load(void *user, const char *name, size_t *size) {
    struct state *state = user;
    for (unsigned i = 0; i < state->count; i++) {
        if (strcmp(state->files[i].name, name) == 0) {
            *size = state->files[i].size;
            return state->files[i].data;
        }
    }
    if (state->count == 12) return NULL;
    char path[128];
    snprintf(path, sizeof path, "%s/%s", FONT_DIRECTORY, name);
    struct loaded *file = &state->files[state->count];
    file->data = read_file(path, &file->size);
    if (file->data == NULL) return NULL;
    snprintf(file->name, sizeof file->name, "%s", name);
    state->count++;
    *size = file->size;
    return file->data;
}

/* The directory holds the twelve faces, as the build staged them. */
static unsigned count_faces(void) {
    long directory = sxn_open_directory(FONT_DIRECTORY);
    if (directory < 0) return 0;
    unsigned faces = 0;
    struct sxn_directory_entry entry;
    while (sxn_read_directory(directory, &entry) > 0) {
        size_t length = strlen(entry.name);
        if (length > 4 && strcmp(entry.name + length - 4, ".ttf") == 0 && strncmp(entry.name, "Liberation", 10) == 0)
            faces++;
    }
    (void)sxn_close(directory);
    return faces;
}

/* The interface faces as SUN's frame loads them (docs/desktop/UI_FONTS.md,
 * "Interface faces from files"): the body and caption slots from
 * /system/fonts, the monospace slot baked, and the same widths as the tables
 * they replace. */
static void check_interface_faces(struct state *state) {
    static const char sample[] = "SavanXP: WordPad, ni\xc3\xb1o 0123";
    int body_width = gfx_text_width(sample), title_width = gfx_text_width_title(sample);
    int loaded = sxngui_load_interface_faces();
    char line[160];
    snprintf(line, sizeof line, "fonttest: %d interface faces from " FONT_DIRECTORY, loaded);
    say(line);
    if (loaded != 2) report(state, "two interface faces load from " FONT_DIRECTORY);
    if (!gfx_face_installed(GFX_FACE_UI) || !gfx_face_installed(GFX_FACE_TITLE))
        report(state, "the body and caption slots hold the faces from files");
    if (gfx_face_installed(GFX_FACE_MONO)) report(state, "the monospace slot keeps its baked face");
    if (gfx_text_height() != 13 || gfx_text_height_title() != 17)
        report(state, "the files' lines are the tables' (13 and 17)");
    if (gfx_text_width(sample) != body_width || gfx_text_width_title(sample) != title_width)
        report(state, "the files measure the sample as the tables do");
    if (sxngui_load_interface_faces() != loaded) report(state, "a second load answers as the first");
}

int main(void) {
    static struct state state;
    unsigned faces = count_faces();
    char line[120];
    snprintf(line, sizeof line, "fonttest: %u Liberation faces in " FONT_DIRECTORY, faces);
    say(line);
    if (faces != 12) report(&state, "twelve Liberation faces in " FONT_DIRECTORY);
    int failed = sxfont_selftest(load, report, &state);
    check_interface_faces(&state);
    snprintf(line, sizeof line, "fonttest: %d self-test checks failed", failed);
    say(line);
    for (unsigned i = 0; i < state.count; i++) free(state.files[i].data);
    say(state.failures == 0 ? "FONT TEST PASS" : "FONT TEST FAIL");
    return state.failures == 0 ? 0 : 1;
}
