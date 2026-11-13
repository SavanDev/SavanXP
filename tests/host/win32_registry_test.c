/*
 * win32_registry_test.c -- Host test of the Win32 subsystem's registry store
 * (subsystems/win32/loader/registry.c): what RegCreateKeyEx, RegOpenKeyEx,
 * RegQueryValueEx, RegSetValueEx and RegCloseKey answer, every error they
 * return, the text file round trip, and compaction of replaced data.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "registry.h"
#include "sxw_abi.h"

static int g_failures;
static int g_checks;

static void check(int ok, const char *what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        printf("  FAIL %s\n", what);
    }
}

static sxw_registry g_reg;
static sxw_registry g_copy;
static char g_text[SXW_REG_DATA_BYTES * 4];

static void test_keys(void) {
    printf("keys\n");
    sxw_reg_init(&g_reg);
    uint32_t key = 0;
    uint32_t disposition = 0;
    check(sxw_reg_open(&g_reg, SXW_HKEY_CURRENT_USER, "Software\\Microsoft\\Solitaire", 0, &key, 0) ==
              SXW_ERROR_FILE_NOT_FOUND,
          "opening a missing key");
    check(sxw_reg_open(&g_reg, SXW_HKEY_CURRENT_USER, "Software\\Microsoft\\Solitaire", 1, &key, &disposition) ==
              SXW_ERROR_SUCCESS && disposition == SXW_REG_CREATED_NEW_KEY,
          "creating it, with every key on the way");
    uint32_t parent = 0;
    check(sxw_reg_open(&g_reg, SXW_HKEY_CURRENT_USER, "SOFTWARE\\microsoft", 0, &parent, 0) == SXW_ERROR_SUCCESS,
          "an intermediate key exists, found in any case");
    uint32_t again = 0;
    check(sxw_reg_open(&g_reg, parent, "\\Solitaire\\", 1, &again, &disposition) == SXW_ERROR_SUCCESS &&
              disposition == SXW_REG_OPENED_EXISTING_KEY,
          "created again under its parent: opened");
    check(sxw_reg_open(&g_reg, SXW_HKEY_CLASSES_ROOT, "CLSID\\{x}", 0, &again, 0) == SXW_ERROR_FILE_NOT_FOUND,
          "HKEY_CLASSES_ROOT holds nothing");
    check(sxw_reg_open(&g_reg, SXW_HKEY_LOCAL_MACHINE, "Software", 1, &again, 0) == SXW_ERROR_ACCESS_DENIED,
          "nothing is created under HKEY_LOCAL_MACHINE");
    check(sxw_reg_open(&g_reg, 0x1234u, "x", 0, &again, 0) == SXW_ERROR_INVALID_HANDLE, "a bogus handle");
    check(sxw_reg_close(&g_reg, key) == SXW_ERROR_SUCCESS && sxw_reg_close(&g_reg, key) == SXW_ERROR_INVALID_HANDLE,
          "a handle closes once");
    check(sxw_reg_close(&g_reg, SXW_HKEY_CURRENT_USER) == SXW_ERROR_SUCCESS, "closing a predefined key");
    uint32_t handles[SXW_REG_MAX_HANDLES + 1];
    uint32_t error = 0;
    unsigned opened = 0;
    for (unsigned i = 0; i <= SXW_REG_MAX_HANDLES && error == SXW_ERROR_SUCCESS; ++i) {
        error = sxw_reg_open(&g_reg, SXW_HKEY_CURRENT_USER, "Software", 0, &handles[i], 0);
        opened += error == SXW_ERROR_SUCCESS;
    }
    check(error == SXW_ERROR_NOT_ENOUGH_MEMORY, "running out of handles is refused cleanly");
    for (unsigned i = 0; i < opened; ++i) {
        sxw_reg_close(&g_reg, handles[i]);
    }
}

static void test_values(void) {
    printf("values\n");
    sxw_reg_init(&g_reg);
    uint32_t key = 0;
    sxw_reg_open(&g_reg, SXW_HKEY_CURRENT_USER, "Software\\Microsoft\\Solitaire", 1, &key, 0);
    const unsigned char options[4] = {0x35, 0, 0, 0};
    check(sxw_reg_set(&g_reg, key, "Opciones", SXW_REG_DWORD, options, 4) == SXW_ERROR_SUCCESS, "set a DWORD");
    uint32_t type = 0;
    uint32_t size = 0;
    unsigned char data[64];
    check(sxw_reg_query(&g_reg, key, "OPCIONES", &type, 0, 0, &size) == SXW_ERROR_SUCCESS && type == 4 &&
              size == 4,
          "a size query, any case");
    check(sxw_reg_query(&g_reg, key, "Opciones", &type, data, 3, &size) == SXW_ERROR_MORE_DATA && size == 4,
          "a short buffer: more data, with the size");
    check(sxw_reg_query(&g_reg, key, "Opciones", &type, data, sizeof(data), &size) == SXW_ERROR_SUCCESS &&
              data[0] == 0x35,
          "the value back");
    check(sxw_reg_query(&g_reg, key, "Atr\xc3\xa1s", &type, data, sizeof(data), &size) == SXW_ERROR_FILE_NOT_FOUND,
          "a missing value");
    const unsigned char longer[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    check(sxw_reg_set(&g_reg, key, "Opciones", SXW_REG_BINARY, longer, 8) == SXW_ERROR_SUCCESS &&
              sxw_reg_query(&g_reg, key, "Opciones", &type, data, sizeof(data), &size) == SXW_ERROR_SUCCESS &&
              type == SXW_REG_BINARY && size == 8 && data[7] == 8,
          "replaced by a longer value of another type");
    check(sxw_reg_set(&g_reg, key, "", SXW_REG_SZ, (const unsigned char *)"d\0\0", 4) == SXW_ERROR_SUCCESS &&
              sxw_reg_query(&g_reg, key, "", &type, data, sizeof(data), &size) == SXW_ERROR_SUCCESS && size == 4,
          "the default value");
    check(sxw_reg_set(&g_reg, SXW_HKEY_CLASSES_ROOT, "x", SXW_REG_DWORD, options, 4) == SXW_ERROR_ACCESS_DENIED,
          "no value under HKEY_CLASSES_ROOT");
    check(sxw_reg_query(&g_reg, SXW_HKEY_CLASSES_ROOT, "x", &type, data, 4, &size) == SXW_ERROR_FILE_NOT_FOUND,
          "and none found there");

    /* Fill the data store with values replaced again and again: compaction
     * keeps it usable, and a value that cannot fit changes nothing. */
    static unsigned char block[1024];
    int stored = 1;
    for (int round = 0; round < 200 && stored; ++round) {
        memset(block, round, sizeof(block));
        char name[16];
        snprintf(name, sizeof(name), "v%d", round % 8);
        stored = sxw_reg_set(&g_reg, key, name, SXW_REG_BINARY, block, (uint32_t)(512 + round % 512)) ==
                 SXW_ERROR_SUCCESS;
    }
    check(stored, "replacing values never runs out while they fit");
    static unsigned char huge[SXW_REG_DATA_BYTES];
    check(sxw_reg_set(&g_reg, key, "big", SXW_REG_BINARY, huge, sizeof(huge)) == SXW_ERROR_NOT_ENOUGH_MEMORY &&
              sxw_reg_query(&g_reg, key, "big", &type, 0, 0, &size) == SXW_ERROR_FILE_NOT_FOUND,
          "a value that cannot fit is refused and not created");
    check(sxw_reg_query(&g_reg, key, "v7", &type, data, sizeof(data), &size) == SXW_ERROR_MORE_DATA &&
              size == 512 + 199 % 512,
          "the last value written survived");
}

static void test_file(void) {
    printf("file\n");
    sxw_reg_init(&g_reg);
    uint32_t key = 0;
    sxw_reg_open(&g_reg, SXW_HKEY_CURRENT_USER, "Software\\SavanXP\\W2", 1, &key, 0);
    const unsigned char count[4] = {2, 0, 0, 0};
    sxw_reg_set(&g_reg, key, "Contador", SXW_REG_DWORD, count, 4);
    sxw_reg_set(&g_reg, key, "Atr\xc3\xa1s", SXW_REG_DWORD, count, 4);
    sxw_reg_set(&g_reg, key, "with \"quotes\" and \\", SXW_REG_SZ, (const unsigned char *)"a\0", 2);
    sxw_reg_set(&g_reg, key, "empty", SXW_REG_BINARY, 0, 0);
    size_t length = 0;
    check(sxw_reg_serialize(&g_reg, g_text, sizeof(g_text), &length) == 0, "serialize");
    check(strstr(g_text, "\"Contador\"=dword:00000002\n") != 0 && strstr(g_text, "[Software\\SavanXP\\W2]\n") != 0,
          "the file reads as documented");
    check(sxw_reg_parse(&g_copy, g_text, length) == 0, "parse it back");
    uint32_t again = 0;
    uint32_t type = 0;
    uint32_t size = 0;
    unsigned char data[8];
    check(sxw_reg_open(&g_copy, SXW_HKEY_CURRENT_USER, "software\\savanxp\\w2", 0, &again, 0) == 0 &&
              sxw_reg_query(&g_copy, again, "with \"quotes\" and \\", &type, data, 8, &size) == 0 && size == 2 &&
              sxw_reg_query(&g_copy, again, "Atr\xc3\xa1s", &type, data, 8, &size) == 0 && data[0] == 2 &&
              sxw_reg_query(&g_copy, again, "empty", &type, data, 8, &size) == 0 && size == 0,
          "every value survives the round trip");
    char again_text[sizeof(g_text)];
    size_t again_length = 0;
    sxw_reg_serialize(&g_copy, again_text, sizeof(again_text), &again_length);
    check(again_length == length && memcmp(again_text, g_text, length) == 0, "and writes the same file");
    check(sxw_reg_serialize(&g_reg, g_text, 10, &length) == -1, "a short buffer is refused");

    static const char *bad[] = {
        "not a registry\n",
        "SavanXP registry 1\n\"orphan\"=dword:00000001\n",
        "SavanXP registry 1\n[k]\n\"x\"=dword:1\n",
        "SavanXP registry 1\n[k]\n\"x\"=hex(4):0g\n",
        "SavanXP registry 1\n[k\n",
        "SavanXP registry 1\n[k]\n\"x=dword:00000001\n",
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        char what[64];
        snprintf(what, sizeof(what), "malformed file %u is refused and leaves it empty", i);
        check(sxw_reg_parse(&g_copy, bad[i], strlen(bad[i])) == -1 &&
                  sxw_reg_open(&g_copy, SXW_HKEY_CURRENT_USER, "k", 0, &again, 0) == SXW_ERROR_FILE_NOT_FOUND,
              what);
    }
    const char *crlf = "SavanXP registry 1\r\n[k]\r\n@=hex(1):41,00\r\n";
    check(sxw_reg_parse(&g_copy, crlf, strlen(crlf)) == 0 &&
              sxw_reg_open(&g_copy, SXW_HKEY_CURRENT_USER, "k", 0, &again, 0) == 0 &&
              sxw_reg_query(&g_copy, again, "", &type, data, 8, &size) == 0 && size == 2 && type == 1,
          "CRLF line ends and the default value");
}

int main(void) {
    test_keys();
    test_values();
    test_file();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
