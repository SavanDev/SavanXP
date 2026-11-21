/*
 * shlwapi.dll -- The path helpers WordPad's start-up uses. The rest of what
 * programs import is declared (declared.txt).
 */
#include <windows.h>

/* The file name part of a path: after the last '\\', '/' or ':'. */
LPWSTR WINAPI PathFindFileNameW(LPCWSTR pszPath) {
    if (pszPath == NULL) {
        return NULL;
    }
    LPCWSTR name = pszPath;
    for (LPCWSTR p = pszPath; *p != 0; ++p) {
        if ((*p == '\\' || *p == '/' || *p == ':') && p[1] != 0 && p[1] != '\\' && p[1] != '/') {
            name = p + 1;
        }
    }
    return (LPWSTR)name;
}
