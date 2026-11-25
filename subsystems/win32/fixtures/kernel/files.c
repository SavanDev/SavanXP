/*
 * files.exe -- WP4's fixture for files (WIN32_WORDPAD_ROADMAP.md): wide
 * names, create, read, write, seek, size and times, the share modes,
 * directories and searches, full paths, the current directory, the volume,
 * and the file-time conversions, failure paths included.
 *
 * It works in its own folder and leaves nothing behind. Everything it
 * writes is the same on Windows XP (run from a folder on C:), in the harness
 * and on SavanXP: no handle, time, drive letter or serial number, and the
 * time zone only through the conversions' agreement with each other.
 */
#include "print.h"

DECLSPEC_IMPORT DWORD WINAPI GetModuleFileNameW(HMODULE module, LPWSTR out, DWORD size);
DECLSPEC_IMPORT int WINAPI lstrcmpiW(LPCWSTR a, LPCWSTR b);
DECLSPEC_IMPORT int WINAPI lstrcmpW(LPCWSTR a, LPCWSTR b);
DECLSPEC_IMPORT LPWSTR WINAPI lstrcpyW(LPWSTR d, LPCWSTR s);
DECLSPEC_IMPORT LPWSTR WINAPI lstrcatW(LPWSTR d, LPCWSTR s);

#define RW (GENERIC_READ | GENERIC_WRITE)

static void say_handle(HANDLE h) {
    if (h == INVALID_HANDLE_VALUE) {
        say("invalid");
        say_error();
    } else {
        say("valid");
    }
}

static void say_size(HANDLE h) {
    DWORD high = 7;
    const DWORD low = GetFileSize(h, &high);
    say_int((long)low);
    say(" high ");
    say_int((long)high);
}

static void say_bytes(const char *bytes, DWORD count) {
    say("\"");
    for (DWORD i = 0; i < count; ++i) {
        char one[2] = {bytes[i] >= 0x20 && bytes[i] < 0x7f ? bytes[i] : '.', 0};
        say(one);
    }
    say("\"");
}

static void say_attributes(DWORD attributes) {
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        say("invalid");
        say_error();
    } else {
        say_hex(attributes);
    }
}

static void clean(void) {
    static const WCHAR *const files[] = {L"wp4file.txt", L"wp4moved.txt", L"wp4dir\\A.txt", L"wp4dir\\b.rtf",
                                         L"wp4dir\\c.TXT", L"wp4dir\\sub\\deep.txt"};
    for (unsigned i = 0; i < sizeof files / sizeof files[0]; ++i) {
        DeleteFileW(files[i]);
    }
    RemoveDirectoryW(L"wp4dir\\sub");
    RemoveDirectoryW(L"wp4dir");
}

static void write_named(const WCHAR *name, const char *text) {
    HANDLE h = CreateFileW(name, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written = 0;
    WriteFile(h, text, (DWORD)lstrlenA(text), &written, NULL);
    CloseHandle(h);
}

/* The current directory is the executable's folder for the rest. */
static void current_directory(void) {
    WCHAR exe[MAX_PATH];
    WCHAR folder[MAX_PATH];
    const DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    DWORD cut = 0;
    for (DWORD i = 0; i < n; ++i) {
        if (exe[i] == '\\') {
            cut = i;
        }
    }
    exe[cut] = 0;
    line("SetCurrentDirectoryW(own folder)");
    say_result(SetCurrentDirectoryW(exe));
    line("GetCurrentDirectoryW is it");
    const DWORD got = GetCurrentDirectoryW(MAX_PATH, folder);
    say_yes(got == (DWORD)lstrlenW(exe) && lstrcmpiW(folder, exe) == 0);
    line("GetCurrentDirectoryW(2) needs");
    say_yes(GetCurrentDirectoryW(2, folder) == (DWORD)lstrlenW(exe) + 1u);
    line("SetCurrentDirectoryW(missing)");
    say_result(SetCurrentDirectoryW(L"wp4-no-such-folder"));
    line("current directory unchanged");
    GetCurrentDirectoryW(MAX_PATH, folder);
    say_yes(lstrcmpiW(folder, exe) == 0);
}

static void full_paths(void) {
    static const WCHAR *const cases[] = {
        L"C:\\a\\b\\..\\c\\.\\d.txt", L"C:/a/b/c",       L"C:\\a\\b\\",      L"C:\\..\\..\\x",
        L"C:\\x\\y. \\z..",           L"c:\\lower\\case", L"C:\\",            L"C:\\a\\.",
        L"C:\\a\\..",                 L"C:\\a\\*.txt",
    };
    WCHAR out[MAX_PATH];
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        LPWSTR part = (LPWSTR)1;
        line("GetFullPathNameW");
        say_w(cases[i]);
        say(" -> ");
        const DWORD n = GetFullPathNameW(cases[i], MAX_PATH, out, &part);
        say_int((long)n);
        say(" ");
        say_w(out);
        say(" part ");
        if (part == NULL) {
            say("none");
        } else {
            say_w(part);
        }
    }
    line("GetFullPathNameW(small) needs");
    say_int((long)GetFullPathNameW(L"C:\\a\\b", 3, out, NULL));
    WCHAR cwd[MAX_PATH];
    WCHAR want[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, cwd);
    lstrcpyW(want, cwd);
    lstrcatW(want, L"\\rel\\file.txt");
    line("GetFullPathNameW(rel\\file.txt) is the current directory's");
    GetFullPathNameW(L"rel\\.\\file.txt", MAX_PATH, out, NULL);
    say_yes(lstrcmpW(out, want) == 0);
    line("GetFullPathNameW(\\rooted.txt) after the drive");
    GetFullPathNameW(L"\\rooted.txt", MAX_PATH, out, NULL);
    say_yes(out[1] == ':' && out[0] == cwd[0]);
    say(" ");
    say_w(out + 2);
}

static void one_file(void) {
    char buffer[32];
    DWORD count = 0;
    line("CreateFileW(wp4file.txt, CREATE_ALWAYS) new");
    SetLastError(1234);
    HANDLE h = CreateFileW(L"wp4file.txt", RW, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    say_handle(h);
    say(" last error ");
    say_int((long)GetLastError());
    line("WriteFile(hello world)");
    say_result(WriteFile(h, "hello world", 11, &count, NULL));
    say(" wrote ");
    say_int((long)count);
    line("SetFilePointer(0, FILE_CURRENT)");
    say_int((long)SetFilePointer(h, 0, NULL, FILE_CURRENT));
    line("SetFilePointer(6, FILE_BEGIN)");
    say_int((long)SetFilePointer(h, 6, NULL, FILE_BEGIN));
    line("ReadFile(5)");
    say_result(ReadFile(h, buffer, 5, &count, NULL));
    say(" read ");
    say_int((long)count);
    say(" ");
    say_bytes(buffer, count);
    line("ReadFile at the end");
    say_result(ReadFile(h, buffer, 5, &count, NULL));
    say(" read ");
    say_int((long)count);
    line("SetFilePointer(-2, FILE_END)");
    say_int((long)SetFilePointer(h, -2, NULL, FILE_END));
    line("SetFilePointer(-20, FILE_CURRENT)");
    const DWORD bad = SetFilePointer(h, -20, NULL, FILE_CURRENT);
    say_hex(bad);
    say_error();
    line("SetFilePointer with a high part");
    LONG high = 0;
    say_int((long)SetFilePointer(h, 3, &high, FILE_BEGIN));
    say(" high ");
    say_int(high);
    line("GetFileSize");
    say_size(h);
    line("past the end, then a byte");
    SetFilePointer(h, 20, NULL, FILE_BEGIN);
    WriteFile(h, "!", 1, &count, NULL);
    say_size(h);
    line("the gap");
    SetFilePointer(h, 9, NULL, FILE_BEGIN);
    ReadFile(h, buffer, 12, &count, NULL);
    say_int((long)count);
    say(" ");
    say_bytes(buffer, count);
    line("SetEndOfFile at 100");
    SetFilePointer(h, 100, NULL, FILE_BEGIN);
    say_result(SetEndOfFile(h));
    say(" size ");
    say_size(h);
    line("SetEndOfFile at 5");
    SetFilePointer(h, 5, NULL, FILE_BEGIN);
    say_result(SetEndOfFile(h));
    say(" size ");
    say_size(h);
    line("FlushFileBuffers");
    say_result(FlushFileBuffers(h));
    line("GetFileTime is about now");
    FILETIME created;
    FILETIME accessed;
    FILETIME written;
    FILETIME now;
    say_result(GetFileTime(h, &created, &accessed, &written));
    GetSystemTimeAsFileTime(&now);
    const long long distance = (long long)((((unsigned long long)now.dwHighDateTime << 32) | now.dwLowDateTime) -
                                           (((unsigned long long)written.dwHighDateTime << 32) |
                                            written.dwLowDateTime));
    say(" ");
    say_yes(distance > -30ll * 10000000ll && distance < 30ll * 10000000ll);
    line("CloseHandle");
    say_result(CloseHandle(h));
    line("CloseHandle again");
    say_result(CloseHandle(h));

    line("CreateFileW(CREATE_NEW) over it");
    say_handle(CreateFileW(L"wp4file.txt", RW, 0, NULL, CREATE_NEW, 0, NULL));
    line("CreateFileW(OPEN_ALWAYS) existing");
    SetLastError(1234);
    h = CreateFileW(L"WP4FILE.TXT", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    say_handle(h);
    say(" last error ");
    say_int((long)GetLastError());
    line("ReadFile through the other spelling");
    ReadFile(h, buffer, sizeof buffer, &count, NULL);
    say_bytes(buffer, count);
    line("WriteFile on a read handle");
    say_result(WriteFile(h, "x", 1, &count, NULL));
    line("SetEndOfFile on a read handle");
    say_result(SetEndOfFile(h));
    line("a second reader, sharing reading");
    HANDLE second = CreateFileW(L"wp4file.txt", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    say_handle(second);
    CloseHandle(second);
    line("a writer, the reader not sharing writing");
    say_handle(CreateFileW(L"wp4file.txt", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0,
                           NULL));
    line("a reader not sharing reading");
    say_handle(CreateFileW(L"wp4file.txt", GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL));
    line("DeleteFileW while open");
    say_result(DeleteFileW(L"wp4file.txt"));
    CloseHandle(h);

    line("CreateFileW(TRUNCATE_EXISTING)");
    h = CreateFileW(L"wp4file.txt", GENERIC_WRITE, 0, NULL, TRUNCATE_EXISTING, 0, NULL);
    say_handle(h);
    say(" size ");
    say_size(h);
    line("ReadFile on a write handle");
    say_result(ReadFile(h, buffer, 1, &count, NULL));
    CloseHandle(h);
    line("CreateFileW(TRUNCATE_EXISTING) to read");
    say_handle(CreateFileW(L"wp4file.txt", GENERIC_READ, 0, NULL, TRUNCATE_EXISTING, 0, NULL));
    line("CreateFileW(OPEN_EXISTING) missing");
    say_handle(CreateFileW(L"wp4-missing.txt", GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL));
    line("CreateFileW in a missing folder");
    say_handle(CreateFileW(L"wp4-missing\\x.txt", RW, 0, NULL, CREATE_ALWAYS, 0, NULL));
    line("CreateFileW with a wildcard");
    say_handle(CreateFileW(L"wp4*.txt", GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL));
    line("CreateFileW(OPEN_ALWAYS) new");
    SetLastError(1234);
    h = CreateFileW(L"wp4moved.txt", RW, 0, NULL, OPEN_ALWAYS, 0, NULL);
    say_handle(h);
    say(" last error ");
    say_int((long)GetLastError());
    CloseHandle(h);
    line("ReadFile on a closed handle");
    say_result(ReadFile(h, buffer, 1, &count, NULL));
    line("GetFileSize on a closed handle");
    say_hex(GetFileSize(h, NULL));
    say_error();
}

static void directories(void) {
    line("CreateDirectoryW(wp4dir)");
    say_result(CreateDirectoryW(L"wp4dir", NULL));
    line("CreateDirectoryW(wp4dir) again");
    say_result(CreateDirectoryW(L"wp4dir", NULL));
    line("CreateDirectoryW(wp4dir\\sub)");
    say_result(CreateDirectoryW(L"wp4dir\\sub", NULL));
    line("CreateDirectoryW in a missing folder");
    say_result(CreateDirectoryW(L"wp4-missing\\sub", NULL));
    write_named(L"wp4dir\\b.rtf", "{\\rtf1}");
    write_named(L"wp4dir\\A.txt", "a");
    write_named(L"wp4dir\\c.TXT", "cc");
    write_named(L"wp4dir\\sub\\deep.txt", "deep");
    line("GetFileAttributesW(wp4dir)");
    say_attributes(GetFileAttributesW(L"wp4dir"));
    line("GetFileAttributesW(wp4dir\\a.TXT)");
    say_attributes(GetFileAttributesW(L"wp4dir\\a.TXT"));
    line("GetFileAttributesW(missing)");
    say_attributes(GetFileAttributesW(L"wp4dir\\missing"));
    line("GetFileAttributesW(missing folder)");
    say_attributes(GetFileAttributesW(L"wp4-missing\\x"));
    line("CreateFileW on a folder");
    say_handle(CreateFileW(L"wp4dir", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL));

    static const WCHAR *const searches[] = {L"wp4dir\\*",      L"wp4dir\\*.*",  L"wp4dir\\*.txt",
                                            L"wp4dir\\?.rtf",  L"wp4dir\\SUB",  L"wp4dir\\a.txt",
                                            L"wp4dir\\*.none", L"wp4-missing\\*", L"wp4dir\\sub\\*"};
    for (unsigned i = 0; i < sizeof searches / sizeof searches[0]; ++i) {
        WIN32_FIND_DATAW data;
        line("FindFirstFileW");
        say_w(searches[i]);
        say(":");
        HANDLE find = FindFirstFileW(searches[i], &data);
        if (find == INVALID_HANDLE_VALUE) {
            say(" none");
            say_error();
            continue;
        }
        do {
            say(" ");
            say_w(data.cFileName);
            say(" ");
            say_hex(data.dwFileAttributes);
            say(" ");
            say_int((long)data.nFileSizeLow);
        } while (FindNextFileW(find, &data));
        say(" end");
        say_error();
        say(" close ");
        say_result(FindClose(find));
    }

    line("DeleteFileW(folder)");
    say_result(DeleteFileW(L"wp4dir\\sub"));
    line("RemoveDirectoryW(not empty)");
    say_result(RemoveDirectoryW(L"wp4dir\\sub"));
    line("RemoveDirectoryW(a file)");
    say_result(RemoveDirectoryW(L"wp4dir\\A.txt"));
    line("RemoveDirectoryW(missing)");
    say_result(RemoveDirectoryW(L"wp4dir\\missing"));
    line("MoveFileW(onto a file)");
    say_result(MoveFileW(L"wp4dir\\A.txt", L"wp4dir\\b.rtf"));
    line("MoveFileW(missing)");
    say_result(MoveFileW(L"wp4dir\\missing", L"wp4dir\\x"));
    line("MoveFileW(A.txt to the parent)");
    DeleteFileW(L"wp4moved.txt");
    say_result(MoveFileW(L"wp4dir\\A.txt", L"wp4moved.txt"));
    line("the moved file");
    say_attributes(GetFileAttributesW(L"wp4moved.txt"));
    say(" ");
    say_attributes(GetFileAttributesW(L"wp4dir\\A.txt"));
    line("DeleteFileW");
    say_result(DeleteFileW(L"wp4moved.txt"));
    line("DeleteFileW again");
    say_result(DeleteFileW(L"wp4moved.txt"));
    DeleteFileW(L"wp4dir\\sub\\deep.txt");
    line("RemoveDirectoryW(empty)");
    say_result(RemoveDirectoryW(L"wp4dir\\sub"));
}

static void volume(void) {
    WCHAR root[4] = {0, ':', '\\', 0};
    WCHAR cwd[MAX_PATH];
    WCHAR name[32];
    WCHAR system[32];
    DWORD serial = 0;
    DWORD longest = 0;
    DWORD flags = 0;
    GetCurrentDirectoryW(MAX_PATH, cwd);
    root[0] = cwd[0];
    line("GetVolumeInformationW(own drive)");
    say_result(GetVolumeInformationW(root, name, 32, &serial, &longest, &flags, system, 32));
    say(" longest ");
    say_int((long)longest);
    say(" case kept, Unicode ");
    say_hex(flags & (FS_CASE_IS_PRESERVED | FS_UNICODE_STORED_ON_DISK));
    line("GetVolumeInformationW(not a root)");
    say_result(GetVolumeInformationW(L"C:\\windows", name, 32, &serial, &longest, &flags, system, 32));
}

static void say_system_time(const SYSTEMTIME *t) {
    say_int(t->wYear);
    say("-");
    say_int(t->wMonth);
    say("-");
    say_int(t->wDay);
    say(" ");
    say_int(t->wHour);
    say(":");
    say_int(t->wMinute);
    say(":");
    say_int(t->wSecond);
    say(".");
    say_int(t->wMilliseconds);
    say(" day ");
    say_int(t->wDayOfWeek);
}

static unsigned long long quad(const FILETIME *f) {
    return ((unsigned long long)f->dwHighDateTime << 32) | f->dwLowDateTime;
}

static void times(void) {
    static const unsigned long long instants[] = {
        0ull,
        1234567ull,
        125458461600000000ull, /* 2001-08-24 02:56:00 */
        126000000000000000ull,
        0x7fffffffffffffffull,
        0x8000000000000000ull,
    };
    for (unsigned i = 0; i < sizeof instants / sizeof instants[0]; ++i) {
        FILETIME f = {(DWORD)instants[i], (DWORD)(instants[i] >> 32)};
        SYSTEMTIME t;
        line("FileTimeToSystemTime");
        say_hex(f.dwHighDateTime);
        say(" ");
        say_hex(f.dwLowDateTime);
        say(" -> ");
        if (FileTimeToSystemTime(&f, &t)) {
            say_system_time(&t);
            FILETIME back;
            say(" back ");
            say_result(SystemTimeToFileTime(&t, &back));
            say(" ");
            say_yes(quad(&back) == instants[i] - instants[i] % 10000ull);
        } else {
            say("failed");
            say_error();
        }
    }
    static const SYSTEMTIME dates[] = {
        {2000, 2, 0, 29, 12, 0, 0, 0},  {2001, 2, 0, 29, 12, 0, 0, 0},  {2001, 13, 0, 1, 0, 0, 0, 0},
        {1600, 12, 0, 31, 0, 0, 0, 0},  {30827, 12, 0, 31, 23, 59, 59, 999}, {30828, 1, 0, 1, 0, 0, 0, 0},
        {2001, 8, 6, 24, 24, 0, 0, 0},  {2001, 8, 6, 24, 23, 60, 0, 0}, {2001, 8, 6, 24, 23, 59, 59, 1000},
        {2100, 3, 5, 1, 0, 0, 0, 0},
    };
    for (unsigned i = 0; i < sizeof dates / sizeof dates[0]; ++i) {
        FILETIME f;
        line("SystemTimeToFileTime");
        say_system_time(&dates[i]);
        say(" -> ");
        if (SystemTimeToFileTime(&dates[i], &f)) {
            say_hex(f.dwHighDateTime);
            say(" ");
            say_hex(f.dwLowDateTime);
        } else {
            say("failed");
            say_error();
        }
    }
    FILETIME now;
    FILETIME local;
    FILETIME back;
    GetSystemTimeAsFileTime(&now);
    line("FileTimeToLocalFileTime and back");
    say_result(FileTimeToLocalFileTime(&now, &local));
    say(" ");
    say_result(LocalFileTimeToFileTime(&local, &back));
    say(" ");
    say_yes(quad(&back) == quad(&now));
    /* GetLocalTime against GetSystemTime: the same bias as the conversion. */
    SYSTEMTIME utc;
    SYSTEMTIME here;
    GetSystemTime(&utc);
    GetLocalTime(&here);
    FILETIME utc_file;
    FILETIME here_file;
    SystemTimeToFileTime(&utc, &utc_file);
    SystemTimeToFileTime(&here, &here_file);
    const long long bias = (long long)(quad(&local) - quad(&now));
    const long long seen = (long long)(quad(&here_file) - quad(&utc_file));
    line("GetLocalTime agrees with FileTimeToLocalFileTime");
    say_yes(seen - bias > -20000000ll && seen - bias < 20000000ll);
}

void __stdcall start(void) {
    say("files.exe");
    current_directory();
    clean();
    full_paths();
    one_file();
    directories();
    volume();
    times();
    clean();
    line("left behind");
    say_attributes(GetFileAttributesW(L"wp4dir"));
    say("\r\n");
    ExitProcess(0);
}
