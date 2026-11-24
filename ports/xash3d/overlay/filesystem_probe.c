#include "build.h"
#include "filesystem.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MODULE_PATH "/games/xash3d/filesystem_stdio.so"

/* The fixture is a little-endian PAK with one four-byte entry. */
static const struct {
    char magic[4];
    uint32_t directory_offset;
    uint32_t directory_size;
    char data[4];
    char name[56];
    uint32_t offset;
    uint32_t size;
} fixture_pak = {"PACK", 16, 64, "PAK!", "sound/test.txt", 12, 4};

static int write_fixture(const char *path, const void *data, size_t size)
{
    FILE *file = fopen(path, "wb");
    if (!file)
        return -1;
    size_t written = fwrite(data, 1, size, file);
    int closed = fclose(file);
    return written == size && closed == 0 ? 0 : -1;
}

static int read_asset(const fs_api_t *api, const char *path, const char *expected)
{
    file_t *file = api->Open(path, "rb", true);
    if (!file) {
        fprintf(stderr, "xash3d-fsprobe: cannot open %s\n", path);
        return -1;
    }

    unsigned char buffer[4096];
    fs_offset_t length = api->FileLength(file);
    fs_offset_t size = 0;
    uint32_t hash = 2166136261u;
    fs_offset_t count;
    while ((count = api->Read(file, buffer, sizeof(buffer))) > 0) {
        if (expected && (size != 0 || count != (fs_offset_t)strlen(expected)
                         || memcmp(buffer, expected, count) != 0)) {
            api->Close(file);
            fprintf(stderr, "xash3d-fsprobe: wrong contents for %s\n", path);
            return -1;
        }
        for (fs_offset_t i = 0; i < count; ++i)
            hash = (hash ^ buffer[i]) * 16777619u;
        size += count;
    }
    int closed = api->Close(file);
    if (closed != 0 || count < 0 || size != length || (expected && size == 0)) {
        fprintf(stderr, "xash3d-fsprobe: cannot read %s\n", path);
        return -1;
    }
    printf("%s: %lld bytes, FNV-1a %08x\n", path, (long long)size, hash);
    return 0;
}

static int run_probe(const char *root, int count, char **paths, int selftest)
{
    void *module = dlopen(MODULE_PATH, RTLD_NOW | RTLD_LOCAL);
    if (!module) {
        fprintf(stderr, "xash3d-fsprobe: %s\n", dlerror());
        return 1;
    }

    FSAPI get_api = (FSAPI)dlsym(module, GET_FS_API);
    void *(*create_interface)(const char *, int *) = dlsym(module, "CreateInterface");
    fs_api_t api;
    fs_globals_t *globals = NULL;
    fs_interface_t callbacks = {0};
    if (!get_api || !create_interface ||
        get_api(FS_API_VERSION, &api, &globals, &callbacks) != FS_API_VERSION ||
        !create_interface(FILESYSTEM_INTERFACE_VERSION, NULL)) {
        fprintf(stderr, "xash3d-fsprobe: filesystem interface unavailable\n");
        dlclose(module);
        return 1;
    }

    if (chdir(root) != 0 || !api.InitStdio(true, root, "valve", "valve", "")) {
        fprintf(stderr, "xash3d-fsprobe: cannot initialize %s\n", root);
        dlclose(module);
        return 1;
    }
    api.LoadGameInfo(0, NULL);

    int result = 0;
    if (selftest) {
        if (!globals->GameInfo ||
            strcmp(globals->GameInfo->title, "SavanXP filesystem smoke") != 0 ||
            read_asset(&api, "LOOSE.cfg", "loose\n") != 0 ||
            read_asset(&api, "sound/test.txt", "PAK!") != 0)
            result = 1;
    } else {
        for (int i = 0; i < count; ++i)
            if (read_asset(&api, paths[i], NULL) != 0)
                result = 1;
    }
    api.ShutdownStdio();
    return dlclose(module) != 0 || result;
}

static int run_selftest(void)
{
    static const char gameinfo[] = "title \"SavanXP filesystem smoke\"\n";
    char root[64];
    snprintf(root, sizeof(root), "/games/xash3d/.fsprobe-%ld", (long)getpid());
    if (mkdir(root, 0700) != 0 || chdir(root) != 0) {
        perror("xash3d-fsprobe: create fixture directory");
        return 1;
    }

    int result = 1;
    if (mkdir("valve", 0700) == 0
        && write_fixture("valve/gameinfo.txt", gameinfo, sizeof(gameinfo) - 1) == 0
        && write_fixture("valve/loose.cfg", "loose\n", 6) == 0
        && write_fixture("valve/pak0.pak", &fixture_pak, sizeof(fixture_pak)) == 0) {
        result = run_probe(root, 0, NULL, 1);
    } else {
        perror("xash3d-fsprobe: write fixtures");
    }
    unlink("valve/pak0.pak");
    unlink("valve/loose.cfg");
    unlink("valve/gameinfo.txt");
    rmdir("valve");
    chdir("/");
    rmdir(root);
    return result;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--selftest") == 0)
        return run_selftest();
    if (argc >= 3 && argv[1][0] == '/')
        return run_probe(argv[1], argc - 2, argv + 2, 0);

    fprintf(stderr, "Usage: xash3d-fsprobe --selftest | /absolute/game/root relative/file ...\n");
    return 2;
}
