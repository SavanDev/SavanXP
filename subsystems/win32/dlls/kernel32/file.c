/*
 * file.c -- kernel32's files, directories, searches and paths, over the
 * host's handle services (sxw_abi.h, SXW_SVC_CLOSE to SXW_SVC_FIND_NEXT).
 *
 * Every path is made full here, as GetFullPathNameW makes it, before the host
 * sees it: the current directory, "." and "..", '/' as '\\', and the dots and
 * blanks Windows drops from the end of a name. The current directory starts
 * as the executable's folder, which is what Explorer gives a program it
 * starts. C: is the host's drive_c folder and Z: the host's root; there are
 * no others. See docs/userland/WIN32.md, "Files".
 */
#define SXW_BUILDING_KERNEL32
#include <windows.h>

#include "sxw_abi.h"

typedef unsigned int u32;
typedef unsigned long long u64;

__declspec(dllimport) u32 WINAPI SxwService(u32 service, const u32 *arguments);

#define SXW_HANDLE_STDIN 3u
#define SXW_HANDLE_STDOUT 7u
#define SXW_HANDLE_STDERR 11u
/* 1601 to 1970 in 100-ns intervals. */
#define EPOCH_1970 116444736000000000ull

static void set_last_error(DWORD error) {
    __asm__ volatile("movl %0, %%fs:%c1" : : "r"(error), "i"(SXW_TEB_LAST_ERROR) : "memory");
}

static u32 peb(void) {
    u32 value;
    __asm__ volatile("movl %%fs:%c1, %0" : "=r"(value) : "i"(SXW_TEB_PEB));
    return value;
}

/* A service that returns a Win32 error: TRUE, or FALSE with the error set. */
static BOOL done(u32 error) {
    if (error != 0u) {
        set_last_error(error);
        return FALSE;
    }
    return TRUE;
}

static int is_standard(HANDLE h) {
    const u32 v = (u32)h;
    return v == SXW_HANDLE_STDIN || v == SXW_HANDLE_STDOUT || v == SXW_HANDLE_STDERR;
}

static u32 wide_length(const WCHAR *s) {
    u32 n = 0;
    while (s[n] != 0) {
        ++n;
    }
    return n;
}

static int is_slash(WCHAR c) {
    return c == '\\' || c == '/';
}

static WCHAR upper(WCHAR c) {
    return c >= 'a' && c <= 'z' ? (WCHAR)(c - 0x20) : c;
}

/* ---- The current directory -------------------------------------------------------- */

static WCHAR g_cwd[MAX_PATH];

/* The executable's folder, the first time it is asked for. */
static const WCHAR *current_directory(void) {
    if (g_cwd[0] == 0) {
        const u32 params = *(u32 *)(peb() + SXW_PEB_PROCESS_PARAMETERS);
        const WCHAR *image = (const WCHAR *)*(u32 *)(params + SXW_PARAMS_IMAGE_PATH + 4u);
        u32 end = 0;
        for (u32 i = 0; image[i] != 0; ++i) {
            if (image[i] == '\\') {
                end = i;
            }
        }
        if (end < 3u) {
            end = 3u; /* "C:\" keeps its backslash */
        }
        for (u32 i = 0; i < end && i + 1u < MAX_PATH; ++i) {
            g_cwd[i] = image[i];
        }
        g_cwd[end < MAX_PATH ? end : MAX_PATH - 1u] = 0;
    }
    return g_cwd;
}

/* ---- Full paths ------------------------------------------------------------------- */

#define PATH_ROOM 1024u

/* The full form of path into out (room units), as GetFullPathNameW makes
 * it; its length, or 0 when it cannot be made (the error is set). */
static u32 full_path(const WCHAR *path, WCHAR *out, u32 room) {
    static WCHAR raw[PATH_ROOM];
    u32 n = 0;
    const WCHAR *cwd = current_directory();
    if (path == NULL || path[0] == 0) {
        set_last_error(ERROR_INVALID_NAME);
        return 0;
    }
    const u32 length = wide_length(path);
    const u32 cwd_length = wide_length(cwd);
    if (length + cwd_length + 4u >= PATH_ROOM) {
        set_last_error(ERROR_FILENAME_EXCED_RANGE);
        return 0;
    }
    if (((path[0] >= 'a' && path[0] <= 'z') || (path[0] >= 'A' && path[0] <= 'Z')) && path[1] == ':') {
        if (is_slash(path[2])) {
            for (u32 i = 0; i < length; ++i) {
                raw[n++] = path[i];
            }
        } else if (upper(path[0]) == upper(cwd[0])) {
            for (u32 i = 0; i < cwd_length; ++i) {
                raw[n++] = cwd[i];
            }
            raw[n++] = '\\';
            for (u32 i = 2; i < length; ++i) {
                raw[n++] = path[i];
            }
        } else {
            raw[n++] = path[0];
            raw[n++] = ':';
            raw[n++] = '\\';
            for (u32 i = 2; i < length; ++i) {
                raw[n++] = path[i];
            }
        }
    } else if (is_slash(path[0])) {
        if (is_slash(path[1])) {
            /* A UNC name: there is no network. */
            set_last_error(ERROR_PATH_NOT_FOUND);
            return 0;
        }
        raw[n++] = cwd[0];
        raw[n++] = ':';
        for (u32 i = 0; i < length; ++i) {
            raw[n++] = path[i];
        }
    } else {
        for (u32 i = 0; i < cwd_length; ++i) {
            raw[n++] = cwd[i];
        }
        raw[n++] = '\\';
        for (u32 i = 0; i < length; ++i) {
            raw[n++] = path[i];
        }
    }
    raw[n] = 0;
    const int trailing = n > 3u && is_slash(raw[n - 1u]);

    /* The drive as it was written, then each component: "." dropped, ".."
     * back one, and the dots and blanks at the end of the last name dropped,
     * as Windows XP does (not those of a folder before it). */
    u32 at = 0;
    if (room < 4u) {
        set_last_error(ERROR_FILENAME_EXCED_RANGE);
        return 0;
    }
    out[at++] = raw[0];
    out[at++] = ':';
    out[at++] = '\\';
    u32 i = 2;
    while (i < n) {
        while (i < n && is_slash(raw[i])) {
            ++i;
        }
        const u32 start = i;
        while (i < n && !is_slash(raw[i])) {
            ++i;
        }
        u32 end = i;
        if (end == start) {
            continue;
        }
        if (end - start == 1u && raw[start] == '.') {
            continue;
        }
        if (end - start == 2u && raw[start] == '.' && raw[start + 1u] == '.') {
            if (at > 3u) {
                --at; /* the backslash after the last component */
                while (at > 3u && out[at - 1u] != '\\') {
                    --at;
                }
            }
            continue;
        }
        u32 rest = i;
        while (rest < n && is_slash(raw[rest])) {
            ++rest;
        }
        while (rest == n && end > start && (raw[end - 1u] == '.' || raw[end - 1u] == ' ')) {
            --end;
        }
        if (end == start) {
            continue;
        }
        if (at + (end - start) + 2u >= room) {
            set_last_error(ERROR_FILENAME_EXCED_RANGE);
            return 0;
        }
        for (u32 k = start; k < end; ++k) {
            out[at++] = raw[k];
        }
        out[at++] = '\\';
    }
    if (at > 3u && !trailing) {
        --at;
    }
    out[at] = 0;
    return at;
}

/* A full path for the host: no trailing backslash but a root's, and no
 * character a file name cannot have. */
static u32 host_path(const WCHAR *path, WCHAR *out) {
    u32 n = full_path(path, out, MAX_PATH);
    if (n == 0u) {
        return 0;
    }
    for (u32 i = 3; i < n; ++i) {
        const WCHAR c = out[i];
        if (c < 0x20u || c == '<' || c == '>' || c == ':' || c == '"' || c == '|' || c == '?' || c == '*') {
            set_last_error(ERROR_INVALID_NAME);
            return 0;
        }
    }
    if (n > 3u && out[n - 1u] == '\\') {
        out[--n] = 0;
    }
    return n;
}

DWORD WINAPI GetFullPathNameW(LPCWSTR lpFileName, DWORD nBufferLength, LPWSTR lpBuffer, LPWSTR *lpFilePart) {
    static WCHAR full[PATH_ROOM];
    const u32 n = full_path(lpFileName, full, PATH_ROOM);
    if (n == 0u) {
        return 0;
    }
    if (nBufferLength <= n || lpBuffer == NULL) {
        return n + 1u;
    }
    for (u32 i = 0; i <= n; ++i) {
        lpBuffer[i] = full[i];
    }
    if (lpFilePart != NULL) {
        u32 last = 0;
        for (u32 i = 0; i < n; ++i) {
            if (full[i] == '\\') {
                last = i + 1u;
            }
        }
        *lpFilePart = last < n ? lpBuffer + last : NULL;
    }
    return n;
}

DWORD WINAPI GetCurrentDirectoryW(DWORD nBufferLength, LPWSTR lpBuffer) {
    const WCHAR *cwd = current_directory();
    const u32 n = wide_length(cwd);
    if (nBufferLength <= n || lpBuffer == NULL) {
        return n + 1u;
    }
    for (u32 i = 0; i <= n; ++i) {
        lpBuffer[i] = cwd[i];
    }
    return n;
}

BOOL WINAPI SetCurrentDirectoryW(LPCWSTR lpPathName) {
    WCHAR path[MAX_PATH];
    const u32 n = host_path(lpPathName, path);
    if (n == 0u) {
        return FALSE;
    }
    sxw_file_entry entry;
    u32 arguments[2] = {(u32)path, (u32)&entry};
    const u32 error = SxwService(SXW_SVC_PATH_INFO, arguments);
    if (error != 0u) {
        return done(error);
    }
    if ((entry.attributes & FILE_ATTRIBUTE_DIRECTORY) == 0u) {
        return done(ERROR_DIRECTORY);
    }
    for (u32 i = 0; i <= n; ++i) {
        g_cwd[i] = path[i];
    }
    return TRUE;
}

/* ---- Files ----------------------------------------------------------------------- */

HANDLE WINAPI CreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                          LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
                          DWORD dwFlagsAndAttributes, HANDLE hTemplateFile) {
    (void)lpSecurityAttributes;
    (void)dwFlagsAndAttributes;
    (void)hTemplateFile;
    WCHAR path[MAX_PATH];
    if (host_path(lpFileName, path) == 0u) {
        return INVALID_HANDLE_VALUE;
    }
    u32 out[2] = {0, 0};
    u32 arguments[5] = {(u32)path, dwDesiredAccess, dwShareMode, dwCreationDisposition, (u32)out};
    const u32 error = SxwService(SXW_SVC_FILE_OPEN, arguments);
    if (error != 0u) {
        set_last_error(error);
        return INVALID_HANDLE_VALUE;
    }
    /* Windows says the file was there for the dispositions that take it. */
    set_last_error(out[1] != 0u && (dwCreationDisposition == CREATE_ALWAYS || dwCreationDisposition == OPEN_ALWAYS)
                       ? ERROR_ALREADY_EXISTS
                       : 0u);
    return (HANDLE)out[0];
}

BOOL WINAPI ReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead, LPDWORD lpNumberOfBytesRead,
                     LPOVERLAPPED lpOverlapped) {
    if (lpNumberOfBytesRead != NULL) {
        *lpNumberOfBytesRead = 0;
    }
    if (lpOverlapped != NULL) {
        return done(ERROR_INVALID_PARAMETER);
    }
    if (is_standard(hFile)) {
        return done(ERROR_INVALID_HANDLE); /* nothing comes in on standard input */
    }
    u32 count = 0;
    u32 arguments[4] = {(u32)hFile, (u32)lpBuffer, nNumberOfBytesToRead, (u32)&count};
    const u32 error = SxwService(SXW_SVC_FILE_READ, arguments);
    if (lpNumberOfBytesRead != NULL) {
        *lpNumberOfBytesRead = count;
    }
    return done(error);
}

/* A file's write; WriteFile (kernel32.c) sends the standard handles to the
 * host's streams. */
BOOL sxw_write_file(HANDLE hFile, LPCVOID lpBuffer, DWORD count, LPDWORD written) {
    u32 wrote = 0;
    u32 arguments[4] = {(u32)hFile, (u32)lpBuffer, count, (u32)&wrote};
    const u32 error = SxwService(SXW_SVC_FILE_WRITE, arguments);
    if (written != NULL) {
        *written = wrote;
    }
    return done(error);
}

DWORD WINAPI SetFilePointer(HANDLE hFile, LONG lDistanceToMove, LONG *lpDistanceToMoveHigh, DWORD dwMoveMethod) {
    const u32 high = lpDistanceToMoveHigh != NULL ? (u32)*lpDistanceToMoveHigh
                     : lDistanceToMove < 0        ? 0xffffffffu
                                                  : 0u;
    u32 position[2] = {0, 0};
    u32 arguments[5] = {(u32)hFile, (u32)lDistanceToMove, high, dwMoveMethod, (u32)position};
    const u32 error = SxwService(SXW_SVC_FILE_SEEK, arguments);
    if (error != 0u) {
        set_last_error(error);
        return INVALID_SET_FILE_POINTER;
    }
    if (lpDistanceToMoveHigh != NULL) {
        *lpDistanceToMoveHigh = (LONG)position[1];
    }
    set_last_error(0);
    return position[0];
}

static BOOL file_info(HANDLE h, sxw_file_entry *entry) {
    u32 arguments[2] = {(u32)h, (u32)entry};
    return done(SxwService(SXW_SVC_FILE_INFO, arguments));
}

DWORD WINAPI GetFileSize(HANDLE hFile, LPDWORD lpFileSizeHigh) {
    sxw_file_entry entry;
    if (!file_info(hFile, &entry)) {
        return INVALID_FILE_SIZE;
    }
    if (lpFileSizeHigh != NULL) {
        *lpFileSizeHigh = entry.size_high;
    }
    set_last_error(0);
    return entry.size_low;
}

static void file_time(u32 seconds, LPFILETIME out) {
    if (out != NULL) {
        const u64 t = (u64)seconds * 10000000ull + EPOCH_1970;
        out->dwLowDateTime = (DWORD)t;
        out->dwHighDateTime = (DWORD)(t >> 32);
    }
}

/* The hosts keep one time for a file, its last write: it answers for the
 * creation and the last access too. */
BOOL WINAPI GetFileTime(HANDLE hFile, LPFILETIME lpCreationTime, LPFILETIME lpLastAccessTime,
                        LPFILETIME lpLastWriteTime) {
    sxw_file_entry entry;
    if (!file_info(hFile, &entry)) {
        return FALSE;
    }
    file_time(entry.mtime, lpCreationTime);
    file_time(entry.mtime, lpLastAccessTime);
    file_time(entry.mtime, lpLastWriteTime);
    return TRUE;
}

BOOL WINAPI SetEndOfFile(HANDLE hFile) {
    u32 arguments[1] = {(u32)hFile};
    return done(SxwService(SXW_SVC_FILE_SET_END, arguments));
}

BOOL WINAPI FlushFileBuffers(HANDLE hFile) {
    if (is_standard(hFile)) {
        return TRUE;
    }
    u32 arguments[1] = {(u32)hFile};
    return done(SxwService(SXW_SVC_FILE_FLUSH, arguments));
}

BOOL WINAPI CloseHandle(HANDLE hObject) {
    if (is_standard(hObject)) {
        return TRUE;
    }
    u32 arguments[1] = {(u32)hObject};
    return done(SxwService(SXW_SVC_CLOSE, arguments));
}

/* ---- Paths ----------------------------------------------------------------------- */

DWORD WINAPI GetFileAttributesW(LPCWSTR lpFileName) {
    WCHAR path[MAX_PATH];
    if (host_path(lpFileName, path) == 0u) {
        return INVALID_FILE_ATTRIBUTES;
    }
    sxw_file_entry entry;
    u32 arguments[2] = {(u32)path, (u32)&entry};
    const u32 error = SxwService(SXW_SVC_PATH_INFO, arguments);
    if (error != 0u) {
        set_last_error(error);
        return INVALID_FILE_ATTRIBUTES;
    }
    return entry.attributes;
}

static BOOL path_action(u32 action, LPCWSTR first, LPCWSTR second) {
    WCHAR path[MAX_PATH];
    WCHAR other[MAX_PATH];
    if (host_path(first, path) == 0u || (second != NULL && host_path(second, other) == 0u)) {
        return FALSE;
    }
    u32 arguments[3] = {action, (u32)path, second != NULL ? (u32)other : 0u};
    return done(SxwService(SXW_SVC_PATH_ACTION, arguments));
}

BOOL WINAPI DeleteFileW(LPCWSTR lpFileName) {
    return path_action(SXW_PATH_DELETE_FILE, lpFileName, NULL);
}

BOOL WINAPI CreateDirectoryW(LPCWSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes) {
    (void)lpSecurityAttributes;
    return path_action(SXW_PATH_MAKE_DIRECTORY, lpPathName, NULL);
}

BOOL WINAPI RemoveDirectoryW(LPCWSTR lpPathName) {
    return path_action(SXW_PATH_REMOVE_DIRECTORY, lpPathName, NULL);
}

BOOL WINAPI MoveFileW(LPCWSTR lpExistingFileName, LPCWSTR lpNewFileName) {
    return path_action(SXW_PATH_MOVE, lpExistingFileName, lpNewFileName);
}

/* ---- Searches ---------------------------------------------------------------------- */

static void find_data(const sxw_file_entry *entry, LPWIN32_FIND_DATAW out) {
    unsigned char *p = (unsigned char *)out;
    for (u32 i = 0; i < sizeof(*out); ++i) {
        p[i] = 0;
    }
    out->dwFileAttributes = entry->attributes;
    file_time(entry->mtime, &out->ftCreationTime);
    file_time(entry->mtime, &out->ftLastAccessTime);
    file_time(entry->mtime, &out->ftLastWriteTime);
    out->nFileSizeHigh = entry->size_high;
    out->nFileSizeLow = entry->size_low;
    for (u32 i = 0; i < MAX_PATH && entry->name[i] != 0; ++i) {
        out->cFileName[i] = entry->name[i];
    }
}

HANDLE WINAPI FindFirstFileW(LPCWSTR lpFileName, LPWIN32_FIND_DATAW lpFindFileData) {
    WCHAR directory[MAX_PATH];
    const u32 n = full_path(lpFileName, directory, MAX_PATH);
    if (n == 0u) {
        return INVALID_HANDLE_VALUE;
    }
    /* The folder, and the mask after its last backslash. */
    u32 split = 2;
    for (u32 i = 0; i < n; ++i) {
        if (directory[i] == '\\') {
            split = i;
        }
    }
    WCHAR mask[MAX_PATH];
    u32 m = 0;
    for (u32 i = split + 1u; i < n; ++i) {
        mask[m++] = directory[i];
    }
    mask[m] = 0;
    if (m == 0u) {
        set_last_error(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    directory[split > 2u ? split : 3u] = 0;
    static sxw_file_entry entry;
    u32 handle = 0;
    u32 arguments[4] = {(u32)directory, (u32)mask, (u32)&handle, (u32)&entry};
    const u32 error = SxwService(SXW_SVC_FIND_FIRST, arguments);
    if (error != 0u) {
        set_last_error(error);
        return INVALID_HANDLE_VALUE;
    }
    find_data(&entry, lpFindFileData);
    return (HANDLE)handle;
}

BOOL WINAPI FindNextFileW(HANDLE hFindFile, LPWIN32_FIND_DATAW lpFindFileData) {
    static sxw_file_entry entry;
    u32 arguments[2] = {(u32)hFindFile, (u32)&entry};
    const u32 error = SxwService(SXW_SVC_FIND_NEXT, arguments);
    if (error != 0u) {
        return done(error);
    }
    find_data(&entry, lpFindFileData);
    return TRUE;
}

BOOL WINAPI FindClose(HANDLE hFindFile) {
    u32 arguments[1] = {(u32)hFindFile};
    return done(SxwService(SXW_SVC_CLOSE, arguments));
}

/* ---- Volumes ------------------------------------------------------------------------ */

/* C: and Z: answer alike: no label, a fixed serial, names of up to 255
 * characters whose case is kept, and SavanXP's file system. */
BOOL WINAPI GetVolumeInformationW(LPCWSTR lpRootPathName, LPWSTR lpVolumeNameBuffer, DWORD nVolumeNameSize,
                                  LPDWORD lpVolumeSerialNumber, LPDWORD lpMaximumComponentLength,
                                  LPDWORD lpFileSystemFlags, LPWSTR lpFileSystemNameBuffer,
                                  DWORD nFileSystemNameSize) {
    WCHAR drive = 0;
    if (lpRootPathName == NULL) {
        drive = upper(current_directory()[0]);
    } else if (lpRootPathName[0] != 0 && lpRootPathName[1] == ':' && is_slash(lpRootPathName[2]) &&
               lpRootPathName[3] == 0) {
        drive = upper(lpRootPathName[0]);
    } else {
        return done(ERROR_INVALID_NAME);
    }
    if (drive != 'C' && drive != 'Z') {
        return done(ERROR_PATH_NOT_FOUND);
    }
    if (lpVolumeNameBuffer != NULL) {
        if (nVolumeNameSize == 0u) {
            return done(ERROR_INSUFFICIENT_BUFFER);
        }
        lpVolumeNameBuffer[0] = 0;
    }
    if (lpVolumeSerialNumber != NULL) {
        *lpVolumeSerialNumber = drive == 'C' ? 0x5358c000u : 0x5358f000u;
    }
    if (lpMaximumComponentLength != NULL) {
        *lpMaximumComponentLength = 255;
    }
    if (lpFileSystemFlags != NULL) {
        *lpFileSystemFlags = FS_CASE_IS_PRESERVED | FS_UNICODE_STORED_ON_DISK;
    }
    if (lpFileSystemNameBuffer != NULL) {
        static const WCHAR name[] = {'S', 'x', 'F', 'S', 0};
        if (nFileSystemNameSize < 5u) {
            return done(ERROR_INSUFFICIENT_BUFFER);
        }
        for (u32 i = 0; i < 5u; ++i) {
            lpFileSystemNameBuffer[i] = name[i];
        }
    }
    return TRUE;
}
