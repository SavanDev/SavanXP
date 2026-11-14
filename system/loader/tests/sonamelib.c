/* One library of the DT_NEEDED resolution test (sonametest.c), built once per
 * version: SONAME_FUNCTION returns SONAME_VERSION, the minor in the file's
 * name, so the program can tell which file the loader chose. */

int SONAME_FUNCTION(void);

int SONAME_FUNCTION(void) {
    return SONAME_VERSION;
}
