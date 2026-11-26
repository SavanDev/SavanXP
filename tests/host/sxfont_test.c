/* libsxfont on the build host (docs/desktop/FONT_LIBRARY.md, "Verification"):
 * the self-test fonttest runs on SavanXP, over the same Liberation files,
 * read from assets/fonts/liberation. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxfont_selftest.h"

struct loaded {
    char name[64];
    void *data;
    size_t size;
};

static struct loaded g_files[12];
static unsigned g_count;

static const void *load(void *user, const char *name, size_t *size) {
    (void)user;
    for (unsigned i = 0; i < g_count; i++) {
        if (strcmp(g_files[i].name, name) == 0) {
            *size = g_files[i].size;
            return g_files[i].data;
        }
    }
    if (g_count == 12) return NULL;
    char path[512];
    snprintf(path, sizeof path, "%s/%s", SAVANXP_FONT_DIR, name);
    FILE *file = fopen(path, "rb");
    if (file == NULL) return NULL;
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    void *data = length > 0 ? malloc((size_t)length) : NULL;
    if (data == NULL || fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    struct loaded *entry = &g_files[g_count++];
    snprintf(entry->name, sizeof entry->name, "%s", name);
    entry->data = data;
    entry->size = (size_t)length;
    *size = entry->size;
    return data;
}

static void report(void *user, const char *line) {
    (void)user;
    fprintf(stderr, "sxfont-test: FAILED %s\n", line);
}

int main(void) {
    int failed = sxfont_selftest(load, report, NULL);
    for (unsigned i = 0; i < g_count; i++) free(g_files[i].data);
    if (failed != 0) {
        fprintf(stderr, "sxfont-test: %d checks failed\n", failed);
        return 1;
    }
    printf("sxfont-test: PASS\n");
    return 0;
}
