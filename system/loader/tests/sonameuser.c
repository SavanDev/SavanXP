/* A second image that needs libsoname.so.0.4, so the test sees whether a
 * library that stood in for it is mapped once for both. */

int soname_version(void);
int soname_user_version(void);

int soname_user_version(void) {
    return soname_version();
}
