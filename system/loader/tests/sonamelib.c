/* One library of the DT_NEEDED resolution test (sonametest.c), built once per
 * version: SONAME_FUNCTION returns SONAME_VERSION, the version in the file's
 * name (the minor, or major and minor run together: 13 for 1.3, 0 for a file
 * with none), so the program can tell which file the loader chose. */

int SONAME_FUNCTION(void);

int SONAME_FUNCTION(void) {
    return SONAME_VERSION;
}
