/* Web Viewer's engine libraries on the host (docs/desktop/WEBVIEWER_ENGINE.md):
 * the shared self-test, then every page of the corpus parsed into a DOM and
 * every element styled. The corpus directory is the first argument. */

#include "webengine.h"
#include "webengine_selftest.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void report(void *user, const char *line) {
    (void)user;
    fprintf(stderr, "FAIL: %s\n", line);
}

static int check_corpus(const char *directory) {
    DIR *dir = opendir(directory);
    if (dir == NULL) {
        fprintf(stderr, "FAIL: cannot open the corpus %s\n", directory);
        return 1;
    }
    int failures = 0;
    unsigned pages = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t length = strlen(entry->d_name);
        if (length < 5 || strcmp(entry->d_name + length - 5, ".html") != 0) {
            continue;
        }
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", directory, entry->d_name);
        FILE *file = fopen(path, "rb");
        if (file == NULL) {
            fprintf(stderr, "FAIL: cannot read %s\n", path);
            failures++;
            continue;
        }
        fseek(file, 0, SEEK_END);
        long size = ftell(file);
        fseek(file, 0, SEEK_SET);
        unsigned char *data = malloc(size > 0 ? (size_t)size : 1);
        size_t got = data != NULL ? fread(data, 1, (size_t)size, file) : 0;
        fclose(file);
        unsigned elements = 0;
        int result = got == (size_t)size ? webengine_check_page(data, got, path, &elements) : -1;
        free(data);
        if (result != 0 || elements == 0) {
            fprintf(stderr, "FAIL: %s: %d (%u elements)\n", entry->d_name, result, elements);
            failures++;
        } else {
            printf("%s: %u elements styled\n", entry->d_name, elements);
        }
        pages++;
    }
    closedir(dir);
    if (pages == 0) {
        fprintf(stderr, "FAIL: no page in %s\n", directory);
        failures++;
    }
    return failures;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: webengine-test <corpus directory>\n");
        return 2;
    }
    int failures = webengine_selftest(report, NULL);
    if (webengine_init() != 0) {
        fprintf(stderr, "FAIL: webengine_init\n");
        return 1;
    }
    failures += check_corpus(argv[1]);
    webengine_fini();
    if (failures != 0) {
        fprintf(stderr, "webengine-test: %d failed\n", failures);
        return 1;
    }
    printf("webengine-test: pass\n");
    return 0;
}
