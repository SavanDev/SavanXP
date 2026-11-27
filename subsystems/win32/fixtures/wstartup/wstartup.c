/*
 * wstartup.exe -- The WP1 fixture (WIN32_WORDPAD_ROADMAP.md). It starts the
 * way wordpad.exe does, as a Unicode program over msvcrt.dll (argv from
 * __wgetmainargs, exit functions from _onexit), then checks what WordPad's
 * and MFC's start-up need, failure paths included: moveable memory, the C
 * heap, thread-local storage, critical sections, the version, strings and
 * code pages, the wide printf family, number conversions, resources, module
 * paths, minimal OLE, rectangles, registered clipboard formats and the
 * missing printer.
 *
 * Everything it writes is the same on Windows XP: no address, handle, path,
 * TLS index, locale or screen size. Run it as:
 *   wstartup.exe alpha "two words" 3
 */
#include <stdarg.h>
#include <windows.h>

typedef void(__cdecl *_PVFV)(void);
typedef struct {
    int newmode;
} _startupinfo;
typedef long HRESULT;

#define IMPORT __declspec(dllimport)

/* What a Visual C++ object using floating point refers to; a program links
 * it from the static runtime, which this fixture does without. */
int _fltused = 0x9875;

/* msvcrt.dll */
IMPORT int __cdecl __wgetmainargs(int *argc, WCHAR ***argv, WCHAR ***envp, int wildcards, _startupinfo *info);
IMPORT extern WCHAR *_wcmdln;
IMPORT int *__cdecl __p___argc(void);
IMPORT WCHAR ***__cdecl __p___wargv(void);
IMPORT _PVFV __cdecl _onexit(_PVFV function);
IMPORT _PVFV __cdecl __dllonexit(_PVFV function, _PVFV **begin, _PVFV **end);
IMPORT __declspec(noreturn) void __cdecl exit(int status);
IMPORT char *__cdecl setlocale(int category, const char *locale);
IMPORT void *__cdecl malloc(unsigned int size);
IMPORT void *__cdecl calloc(unsigned int count, unsigned int size);
IMPORT void *__cdecl realloc(void *block, unsigned int size);
IMPORT void *__cdecl _expand(void *block, unsigned int size);
IMPORT unsigned int __cdecl _msize(void *block);
IMPORT void __cdecl free(void *block);
IMPORT void *__cdecl memset(void *d, int v, unsigned int n);
IMPORT void *__cdecl memmove(void *d, const void *s, unsigned int n);
IMPORT int __cdecl memcmp(const void *a, const void *b, unsigned int n);
IMPORT unsigned int __cdecl wcslen(const WCHAR *s);
IMPORT int __cdecl wcscmp(const WCHAR *a, const WCHAR *b);
IMPORT int __cdecl wcsncmp(const WCHAR *a, const WCHAR *b, unsigned int n);
IMPORT int __cdecl _wcsicmp(const WCHAR *a, const WCHAR *b);
IMPORT int __cdecl _wcsnicmp(const WCHAR *a, const WCHAR *b, unsigned int n);
IMPORT WCHAR *__cdecl wcsncpy(WCHAR *d, const WCHAR *s, unsigned int n);
IMPORT WCHAR *__cdecl wcschr(const WCHAR *s, WCHAR c);
IMPORT WCHAR *__cdecl wcsrchr(const WCHAR *s, WCHAR c);
IMPORT WCHAR *__cdecl wcsstr(const WCHAR *s, const WCHAR *t);
IMPORT WCHAR *__cdecl wcspbrk(const WCHAR *s, const WCHAR *set);
IMPORT unsigned int __cdecl wcsspn(const WCHAR *s, const WCHAR *set);
IMPORT unsigned int __cdecl wcscspn(const WCHAR *s, const WCHAR *set);
IMPORT WCHAR *__cdecl _wcsdup(const WCHAR *s);
IMPORT WCHAR *__cdecl _wcslwr(WCHAR *s);
IMPORT WCHAR *__cdecl _wcsupr(WCHAR *s);
IMPORT WCHAR *__cdecl _wcsrev(WCHAR *s);
IMPORT long __cdecl wcstol(const WCHAR *s, WCHAR **end, int base);
IMPORT unsigned long __cdecl wcstoul(const WCHAR *s, WCHAR **end, int base);
IMPORT double __cdecl wcstod(const WCHAR *s, WCHAR **end);
IMPORT int __cdecl _wtoi(const WCHAR *s);
IMPORT WCHAR *__cdecl _itow(int v, WCHAR *out, int radix);
IMPORT WCHAR *__cdecl _ltow(long v, WCHAR *out, int radix);
IMPORT WCHAR *__cdecl _ultow(unsigned long v, WCHAR *out, int radix);
IMPORT int __cdecl iswspace(unsigned short c);
IMPORT int __cdecl iswdigit(unsigned short c);
IMPORT int __cdecl _vsnwprintf(WCHAR *b, unsigned int n, const WCHAR *f, va_list a);
IMPORT int __cdecl swprintf(WCHAR *b, const WCHAR *f, ...);
IMPORT double __cdecl floor(double v);
IMPORT double __cdecl ceil(double v);
IMPORT double __cdecl modf(double v, double *i);
IMPORT int __cdecl abs(int v);

/* kernel32.dll, beyond windows.h */
IMPORT DWORD WINAPI GetVersion(void);
IMPORT BOOL WINAPI GetVersionExA(LPOSVERSIONINFOA info);
IMPORT DWORD WINAPI GetProcessVersion(DWORD id);
IMPORT DWORD WINAPI TlsAlloc(void);
IMPORT BOOL WINAPI TlsFree(DWORD index);
IMPORT LPVOID WINAPI TlsGetValue(DWORD index);
IMPORT BOOL WINAPI TlsSetValue(DWORD index, LPVOID value);
IMPORT void WINAPI InitializeCriticalSection(LPCRITICAL_SECTION cs);
IMPORT void WINAPI EnterCriticalSection(LPCRITICAL_SECTION cs);
IMPORT void WINAPI LeaveCriticalSection(LPCRITICAL_SECTION cs);
IMPORT void WINAPI DeleteCriticalSection(LPCRITICAL_SECTION cs);
IMPORT DWORD WINAPI GetCurrentThreadId(void);
IMPORT LONG WINAPI InterlockedIncrement(LPLONG v);
IMPORT LONG WINAPI InterlockedDecrement(LPLONG v);
IMPORT LONG WINAPI InterlockedExchange(LPLONG v, LONG n);
IMPORT LPWSTR WINAPI lstrcpynW(LPWSTR d, LPCWSTR s, int n);
IMPORT LPWSTR WINAPI lstrcatW(LPWSTR d, LPCWSTR s);
IMPORT int WINAPI lstrcmpW(LPCWSTR a, LPCWSTR b);
IMPORT int WINAPI lstrcmpiW(LPCWSTR a, LPCWSTR b);
IMPORT int WINAPI lstrlenA(LPCSTR s);
IMPORT HMODULE WINAPI GetModuleHandleW(LPCWSTR name);
IMPORT HMODULE WINAPI LoadLibraryW(LPCWSTR name);
IMPORT BOOL WINAPI FreeLibrary(HMODULE module);
IMPORT DWORD WINAPI GetModuleFileNameW(HMODULE module, LPWSTR out, DWORD size);
IMPORT UINT WINAPI GetSystemDirectoryW(LPWSTR out, UINT size);
IMPORT HRSRC WINAPI FindResourceW(HMODULE module, LPCWSTR name, LPCWSTR type);
IMPORT HGLOBAL WINAPI LoadResource(HMODULE module, HRSRC resource);
IMPORT LPVOID WINAPI LockResource(HGLOBAL data);
IMPORT DWORD WINAPI SizeofResource(HMODULE module, HRSRC resource);
IMPORT UINT WINAPI SetErrorMode(UINT mode);

/* advapi32.dll, user32.dll, gdi32.dll, shlwapi.dll, ole32.dll, comdlg32.dll */
IMPORT LONG WINAPI RegOpenKeyExW(HKEY key, LPCWSTR subkey, DWORD options, REGSAM sam, PHKEY out);
IMPORT LONG WINAPI RegDeleteKeyW(HKEY key, LPCWSTR subkey);
IMPORT UINT WINAPI RegisterClipboardFormatW(LPCWSTR name);
IMPORT BOOL WINAPI SetRect(LPRECT r, int l, int t, int ri, int b);
IMPORT BOOL WINAPI SetRectEmpty(LPRECT r);
IMPORT BOOL WINAPI IsRectEmpty(const RECT *r);
IMPORT BOOL WINAPI EqualRect(const RECT *a, const RECT *b);
IMPORT BOOL WINAPI OffsetRect(LPRECT r, int dx, int dy);
IMPORT BOOL WINAPI UnionRect(LPRECT d, const RECT *a, const RECT *b);
IMPORT HWND WINAPI FindWindowW(LPCWSTR cls, LPCWSTR title);
IMPORT int WINAPI GetSystemMetrics(int index);
IMPORT HFONT WINAPI CreateFontIndirectW(const LOGFONTW *lf);
IMPORT int WINAPI GetObjectW(HANDLE h, int c, LPVOID out);
IMPORT int __cdecl wsprintfW(LPWSTR b, LPCWSTR f, ...);
IMPORT LPWSTR WINAPI PathFindFileNameW(LPCWSTR path);
IMPORT HRESULT WINAPI OleInitialize(LPVOID reserved);
IMPORT void WINAPI OleUninitialize(void);
IMPORT LPVOID WINAPI CoTaskMemAlloc(SIZE_T size);
IMPORT void WINAPI CoTaskMemFree(LPVOID p);
IMPORT BOOL WINAPI PrintDlgW(void *pd);
IMPORT DWORD WINAPI CommDlgExtendedError(void);

/* ---- Output ------------------------------------------------------------------------ */

static void say(const char *text) {
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, (DWORD)lstrlenA(text), &written, NULL);
}

static void say_int(long value) {
    char digits[16];
    int n = 15;
    unsigned long magnitude = value < 0 ? 0ul - (unsigned long)value : (unsigned long)value;
    digits[n] = 0;
    do {
        digits[--n] = (char)('0' + magnitude % 10u);
        magnitude /= 10u;
    } while (magnitude != 0u);
    if (value < 0) {
        digits[--n] = '-';
    }
    say(digits + n);
}

static void say_hex(unsigned long value) {
    char digits[11] = "0x";
    for (int i = 0; i < 8; ++i) {
        unsigned nibble = (unsigned)(value >> (28 - 4 * i)) & 0xfu;
        digits[2 + i] = (char)(nibble < 10u ? '0' + nibble : 'a' + nibble - 10u);
    }
    digits[10] = 0;
    say(digits);
}

/* A wide string in Windows-1252, in brackets, with any character 1252 lacks
 * shown as '?'. */
static void say_w(const WCHAR *text) {
    char narrow[512];
    int n = WideCharToMultiByte(1252, 0, text, -1, narrow, sizeof narrow, NULL, NULL);
    say("[");
    say(n > 0 ? narrow : "?conversion failed?");
    say("]");
}

static void say_yes(int yes) {
    say(yes ? "yes" : "no");
}

static void line(const char *label) {
    say("\r\n");
    say(label);
    say(": ");
}

static void say_error(void) {
    say(" error ");
    say_int((long)GetLastError());
}

/* ---- Exit functions -------------------------------------------------------------- */

static void __cdecl first_at_exit(void) {
    say("\r\nexit: the first registered runs last");
}

static void __cdecl second_at_exit(void) {
    say("\r\nexit: the second registered runs first");
}

static int g_dll_calls;
static void __cdecl dll_exit_function(void) {
    ++g_dll_calls;
}

/* ---- The checks -------------------------------------------------------------------- */

static void check_arguments(void) {
    int argc = 0;
    WCHAR **argv = 0;
    WCHAR **envp = 0;
    _startupinfo info = {0};
    int status = __wgetmainargs(&argc, &argv, &envp, 0, &info);
    line("__wgetmainargs");
    say_int(status);
    say(", argc ");
    say_int(argc);
    for (int i = 1; i < argc; ++i) {
        say(" ");
        say_w(argv[i]);
    }
    line("__p___argc and __p___wargv agree");
    say_yes(*__p___argc() == argc && *__p___wargv() == argv);
    line("_wcmdln ends with the arguments");
    const WCHAR *tail = L"alpha \"two words\" 3";
    unsigned int n = wcslen(_wcmdln);
    unsigned int t = wcslen(tail);
    say_yes(n >= t && wcscmp(_wcmdln + n - t, tail) == 0);
    line("setlocale query");
    say(setlocale(0, NULL));
}

static void check_moveable(void) {
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, 10);
    line("GlobalAlloc(GHND, 10) size, flags");
    say_int((long)GlobalSize(h));
    say(", ");
    say_hex(GlobalFlags(h));
    unsigned char *p = GlobalLock(h);
    unsigned char *q = GlobalLock(h);
    line("two locks: same address, zeroed, *(void **)handle is it, flags");
    say_yes(p != NULL && p == q && p[0] == 0 && p[9] == 0);
    say(", ");
    say_yes(*(void **)h == p);
    say(", ");
    say_hex(GlobalFlags(h));
    line("GlobalHandle(address) is the handle");
    say_yes(GlobalHandle(p) == h);
    for (int i = 0; i < 10; ++i) {
        p[i] = (unsigned char)(i + 1);
    }
    BOOL first = GlobalUnlock(h);
    SetLastError(1234);
    BOOL second = GlobalUnlock(h);
    line("unlock, unlock");
    say_int(first);
    say(", ");
    say_int(second);
    say_error();
    SetLastError(1234);
    BOOL third = GlobalUnlock(h);
    line("a third unlock");
    say_int(third);
    say_error();
    HGLOBAL r = GlobalReAlloc(h, 100, GMEM_MOVEABLE | GMEM_ZEROINIT);
    p = GlobalLock(h);
    line("GlobalReAlloc to 100: same handle, size, kept, zero tail");
    say_yes(r == h);
    say(", ");
    say_int((long)GlobalSize(h));
    say(", ");
    say_yes(p != NULL && p[0] == 1 && p[9] == 10);
    say(", ");
    say_yes(p != NULL && p[10] == 0 && p[99] == 0);
    GlobalUnlock(h);
    r = GlobalReAlloc(h, 0, GMEM_MOVEABLE);
    line("GlobalReAlloc to 0: same handle, flags, lock");
    say_yes(r == h);
    say(", ");
    say_hex(GlobalFlags(h));
    say(", ");
    say_yes(GlobalLock(h) == NULL);
    line("GlobalFree");
    say_yes(GlobalFree(h) == NULL);

    HGLOBAL empty = GlobalAlloc(GMEM_MOVEABLE, 0);
    line("GlobalAlloc(GMEM_MOVEABLE, 0): flags, lock, size");
    say_hex(GlobalFlags(empty));
    say(", ");
    say_yes(GlobalLock(empty) == NULL);
    say(", ");
    say_int((long)GlobalSize(empty));
    GlobalFree(empty);

    void *fixed = GlobalAlloc(GMEM_FIXED, 10);
    line("GlobalAlloc(GMEM_FIXED, 10): size, flags, handle, lock");
    say_int((long)GlobalSize(fixed));
    say(", ");
    say_hex(GlobalFlags(fixed));
    say(", ");
    say_yes(GlobalHandle(fixed) == fixed);
    say(", ");
    say_yes(GlobalLock(fixed) == fixed);
    void *grown = GlobalReAlloc(fixed, 4000, 0);
    line("GlobalReAlloc of a fixed block without GMEM_MOVEABLE, to 4000");
    say_yes(grown == NULL || grown == fixed);
    GlobalFree(grown != NULL ? grown : fixed);

    HLOCAL local = LocalAlloc(LMEM_MOVEABLE | LMEM_ZEROINIT, 6);
    unsigned char *l = LocalLock(local);
    line("LocalAlloc(LHND, 6): lock gives zeroed memory, unlock");
    say_yes(l != NULL && l[0] == 0 && l[5] == 0);
    say(", ");
    say_int(LocalUnlock(local));
    LocalFree(local);
}

static void check_c_heap(void) {
    unsigned char *m = malloc(10);
    line("malloc(10): _msize");
    say_int((long)_msize(m));
    for (int i = 0; i < 10; ++i) {
        m[i] = (unsigned char)(100 + i);
    }
    m = realloc(m, 3000);
    line("realloc to 3000: _msize, kept");
    say_int((long)_msize(m));
    say(", ");
    say_yes(m[0] == 100 && m[9] == 109);
    void *same = _expand(m, 20);
    line("_expand to 20: in place, _msize");
    say_yes(same == m);
    say(", ");
    say_int((long)_msize(m));
    free(m);
    int *z = calloc(4, sizeof(int));
    line("calloc(4, 4): zeroed, _msize");
    say_yes(z[0] == 0 && z[3] == 0);
    say(", ");
    say_int((long)_msize(z));
    free(z);
    char buffer[8] = "abcdefg";
    memmove(buffer + 1, buffer, 5);
    line("memmove overlapping, memcmp, memset");
    say(buffer);
    say(", ");
    say_int(memcmp("abc", "abd", 3));
    say(", ");
    memset(buffer, 'x', 3);
    say(buffer);
}

static void check_tls_and_locks(void) {
    DWORD index = TlsAlloc();
    SetLastError(1234);
    LPVOID value = TlsGetValue(index);
    line("TlsAlloc: a new slot reads NULL with no error");
    say_yes(index != 0xffffffffu && value == NULL);
    say_error();
    TlsSetValue(index, (LPVOID)0x1234);
    line("TlsSetValue then TlsGetValue");
    say_hex((unsigned long)TlsGetValue(index));
    line("TlsFree twice");
    say_int(TlsFree(index));
    say(", ");
    SetLastError(0);
    say_int(TlsFree(index));
    say_error();
    SetLastError(0);
    line("TlsGetValue(1100)");
    say_yes(TlsGetValue(1100) == NULL);
    say_error();

    CRITICAL_SECTION cs;
    InitializeCriticalSection(&cs);
    line("critical section: initial lock count, recursion");
    say_int(cs.LockCount);
    say(", ");
    say_int(cs.RecursionCount);
    EnterCriticalSection(&cs);
    EnterCriticalSection(&cs);
    line("entered twice: recursion, owner is this thread");
    say_int(cs.RecursionCount);
    say(", ");
    say_yes((DWORD)cs.OwningThread == GetCurrentThreadId());
    LeaveCriticalSection(&cs);
    LeaveCriticalSection(&cs);
    line("left twice: lock count, recursion, owner");
    say_int(cs.LockCount);
    say(", ");
    say_int(cs.RecursionCount);
    say(", ");
    say_yes(cs.OwningThread == NULL);
    DeleteCriticalSection(&cs);

    LONG counter = 5;
    line("InterlockedIncrement, Decrement, Exchange");
    say_int(InterlockedIncrement(&counter));
    say(", ");
    say_int(InterlockedDecrement(&counter));
    say(", ");
    say_int(InterlockedExchange(&counter, 42));
    say(" then ");
    say_int(counter);
}

static void check_version(void) {
    line("GetVersion");
    say_hex(GetVersion());
    OSVERSIONINFOA info;
    info.dwOSVersionInfoSize = sizeof info;
    BOOL ok = GetVersionExA(&info);
    line("GetVersionExA");
    say_int(ok);
    say(" ");
    say_int((long)info.dwMajorVersion);
    say(".");
    say_int((long)info.dwMinorVersion);
    say(".");
    say_int((long)info.dwBuildNumber);
    say(" platform ");
    say_int((long)info.dwPlatformId);
    say(" ");
    say(info.szCSDVersion);
    info.dwOSVersionInfoSize = 12;
    SetLastError(0);
    line("GetVersionExA with a short size");
    say_int(GetVersionExA(&info));
    say_error();
    line("GetProcessVersion(0)");
    say_hex(GetProcessVersion(0));
    line("SetErrorMode returns the previous mode");
    SetErrorMode(1);
    say_int((long)SetErrorMode(0));
}

static void check_strings(void) {
    WCHAR buffer[32];
    lstrcpynW(buffer, L"abcdef", 4);
    line("lstrcpynW(4)");
    say_w(buffer);
    lstrcatW(buffer, L"XYZ");
    say(" lstrcatW ");
    say_w(buffer);
    line("lstrcmpW abc/abd, abc/abc, lstrcmpiW HELLO/hello");
    say_int(lstrcmpW(L"abc", L"abd"));
    say(", ");
    say_int(lstrcmpW(L"abc", L"abc"));
    say(", ");
    say_int(lstrcmpiW(L"HELLO", L"hello"));

    const char cp1252[] = "a\xf1\xe9\x80z";
    WCHAR wide[16];
    int n = MultiByteToWideChar(CP_ACP, 0, cp1252, -1, wide, 16);
    line("MultiByteToWideChar(ACP) count and units");
    say_int(n);
    for (int i = 0; i < n; ++i) {
        say(" ");
        say_hex(wide[i]);
    }
    line("MultiByteToWideChar size query");
    say_int(MultiByteToWideChar(CP_ACP, 0, cp1252, -1, NULL, 0));
    SetLastError(0);
    line("MultiByteToWideChar into 2 units");
    say_int(MultiByteToWideChar(CP_ACP, 0, cp1252, -1, wide, 2));
    say_error();
    const char utf8[] = "\xc3\xb1\xe2\x82\xac\xf0\x9f\x98\x80";
    n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, 16);
    line("MultiByteToWideChar(UTF8) count and units");
    say_int(n);
    for (int i = 0; i < n; ++i) {
        say(" ");
        say_hex(wide[i]);
    }
    char back[32];
    n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, back, sizeof back, NULL, NULL);
    line("WideCharToMultiByte(UTF8) round trip");
    say_int(n);
    say(" ");
    say_yes(memcmp(back, utf8, sizeof utf8) == 0);
    BOOL used = FALSE;
    n = WideCharToMultiByte(CP_ACP, 0, L"a\x0416z", -1, back, sizeof back, "#", &used);
    line("WideCharToMultiByte(ACP) of a missing character with a default");
    say_int(n);
    say(" ");
    say(back);
    say(" used ");
    say_int(used);
}

static void check_c_strings(void) {
    WCHAR buffer[64];
    line("_wcsicmp ABC/abc, Ñ/ñ; _wcsnicmp; wcsncmp");
    say_int(_wcsicmp(L"ABC", L"abc"));
    say(", ");
    say_int(_wcsicmp(L"\x00d1", L"\x00f1") != 0);
    say(", ");
    say_int(_wcsnicmp(L"ABCx", L"abcy", 3));
    say(", ");
    say_int(wcsncmp(L"abcx", L"abcy", 4));
    WCHAR *copy = _wcsdup(L"Mixed Case \x00d1");
    line("_wcslwr, _wcsupr, _wcsrev");
    say_w(_wcslwr(copy));
    say(" ");
    say_w(_wcsupr(copy));
    say(" ");
    say_w(_wcsrev(copy));
    free(copy);
    const WCHAR *text = L"one,two;three";
    line("wcschr, wcsrchr, wcsstr, wcspbrk, wcsspn, wcscspn");
    say_int(wcschr(text, ',') - text);
    say(", ");
    say_int(wcsrchr(text, 'e') - text);
    say(", ");
    say_int(wcsstr(text, L"two") - text);
    say(", ");
    say_int(wcspbrk(text, L";,") - text);
    say(", ");
    say_int((long)wcsspn(L"aabbc", L"ab"));
    say(", ");
    say_int((long)wcscspn(L"abc;d", L";"));
    wcsncpy(buffer, L"ab", 5);
    line("wcsncpy pads with zeros");
    say_yes(buffer[2] == 0 && buffer[4] == 0);
    line("iswspace space, nbsp, x; iswdigit 7, a");
    say_yes(iswspace(' '));
    say(", ");
    say_yes(iswspace(0xa0));
    say(", ");
    say_yes(iswspace('x'));
    say(", ");
    say_yes(iswdigit('7'));
    say(", ");
    say_yes(iswdigit('a'));
}

static void check_numbers(void) {
    WCHAR *end = 0;
    line("wcstol");
    say_int(wcstol(L"  -123abc", &end, 10));
    say(" rest ");
    say_w(end);
    say(", ");
    say_int(wcstol(L"0x1F", NULL, 0));
    say(", ");
    say_int(wcstol(L"017", NULL, 0));
    say(", ");
    say_int(wcstol(L"99999999999", NULL, 10));
    say(", ");
    say_int(wcstol(L"zz", &end, 36));
    say(", none ");
    say_int(wcstol(L"x1", &end, 10));
    say(" rest ");
    say_w(end);
    line("wcstoul, _wtoi");
    say_hex(wcstoul(L"4294967295", NULL, 10));
    say(", ");
    say_hex(wcstoul(L"-1", NULL, 10));
    say(", ");
    say_int(_wtoi(L"42x"));
    double d = wcstod(L" 3.25e2xyz", &end);
    line("wcstod 3.25e2");
    say_int((long)d);
    say(" rest ");
    say_w(end);
    d = wcstod(L"1.5e", &end);
    say(", 1.5e: ");
    say_int((long)(d * 10.0));
    say(" rest ");
    say_w(end);
    WCHAR out[40];
    line("_itow -42 base 10, 255 base 16, -1 base 2; _ultow");
    say_w(_itow(-42, out, 10));
    say(" ");
    say_w(_ltow(255, out, 16));
    say(" ");
    say_w(_itow(-1, out, 2));
    say(" ");
    say_w(_ultow(4000000000ul, out, 10));
    double whole = 0;
    double fraction = modf(-2.75, &whole);
    line("floor, ceil, modf(-2.75), abs");
    say_int((long)floor(-2.5));
    say(" ");
    say_int((long)ceil(-2.5));
    say(" ");
    say_int((long)floor(2.5));
    say(" ");
    say_int((long)ceil(2.5));
    say(" ");
    say_int((long)whole);
    say(" ");
    say_int((long)(fraction * 100.0));
    say(" ");
    say_int(abs(-7));
}

static int format(WCHAR *buffer, unsigned int count, const WCHAR *f, ...) {
    va_list args;
    va_start(args, f);
    int n = _vsnwprintf(buffer, count, f, args);
    va_end(args);
    return n;
}

static void check_printf(void) {
    WCHAR b[128];
    int n = format(b, 128, L"%d|%5d|%-5d|%05d|%+d|%x|%X|%#x|%o|%u|%%", 42, -7, 3, 42, 5, 255, 255, 255, 8,
                   4000000000u);
    line("integers");
    say_int(n);
    say(" ");
    say_w(b);
    n = format(b, 128, L"%s|%10s|%-6s|%.2s|%S|%c|%C|%ls|%hs", L"wide", L"right", L"left", L"cut", "narrow", L'w',
               'n', L"l", "h");
    line("strings and characters");
    say_int(n);
    say(" ");
    say_w(b);
    n = format(b, 128, L"%f|%.2f|%8.3f|%-8.1f|%e|%E|%g|%g|%g|%g|%.3g|%.0f", 3.14159, 2.675, -1.5, 0.05,
               12345.678, 0.000123, 0.0001, 1e-5, 123456789.0, 100.0, 3.14159, 2.5);
    line("floating point");
    say_int(n);
    say(" ");
    say_w(b);
    n = format(b, 128, L"%.2f|%.2f|%.1f|%g|%e|%f|%g|%.3e|%G|%#.0f|%+.1e|%d", 0.125, 9.995, -0.0, 0.0, 0.0, 1e-320,
               1e-320, 6.0221e23, 1e-10, 3.0, -2.5e-7, (int)0x80000000u);
    line("floating point edges");
    say_int(n);
    say(" ");
    say_w(b);
    WCHAR big[400];
    n = format(big, 400, L"%.0f|%.3g", 1e300, 1e300);
    line("1e300 with %.0f: length, first and last digits");
    say_int(n);
    say(" ");
    big[12] = 0;
    say_w(big);
    say_w(big + n - 12);
    n = format(b, 128, L"%I64d|%I64x|%lld|%hd|%p", (long long)-5000000000ll, (unsigned long long)0x123456789ull,
               (long long)7, 70000, (void *)0x1234);
    line("lengths and %p");
    say_int(n);
    say(" ");
    say_w(b);
    for (int i = 0; i < 12; ++i) {
        b[i] = '#';
    }
    n = format(b, 5, L"%s", L"abcdefgh");
    line("_vsnwprintf over 5: result, no terminator");
    say_int(n);
    say(" ");
    say_yes(b[4] == 'e' && b[5] == '#');
    n = format(b, 5, L"%s", L"abcde");
    line("_vsnwprintf exactly 5");
    say_int(n);
    say(" ");
    say_yes(b[5] == '#');
    n = swprintf(b, L"%d-%s", 9, L"nine");
    line("swprintf");
    say_int(n);
    say(" ");
    say_w(b);
    n = wsprintfW(b, L"%d|%-4s|%S|%05x|%c|%lu", -3, L"ab", "nar", 255, L'z', 4000000000u);
    line("wsprintfW");
    say_int(n);
    say(" ");
    say_w(b);
    static WCHAR long_text[1200];
    static WCHAR long_out[1200];
    for (int i = 0; i < 1100; ++i) {
        long_text[i] = 'a' + i % 26;
        long_out[i] = '#';
    }
    n = wsprintfW(long_out, L"%s", long_text);
    line("wsprintfW of 1100 units: result, then the units at 1022 to 1025");
    say_int(n);
    for (int i = 1022; i < 1026; ++i) {
        say(" ");
        say_int(long_out[i]);
    }
}

static void check_modules(void) {
    WCHAR path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    line("GetModuleFileNameW(NULL): length matches, file name");
    say_yes(n == wcslen(path));
    say(" ");
    say_w(_wcslwr(PathFindFileNameW(path)));
    HMODULE kernel = GetModuleHandleW(L"KERNEL32.DLL");
    n = GetModuleFileNameW(kernel, path, MAX_PATH);
    line("GetModuleFileNameW(kernel32): file name");
    say_w(_wcslwr(PathFindFileNameW(path)));
    for (int i = 0; i < 8; ++i) {
        path[i] = '#';
    }
    SetLastError(0);
    n = GetModuleFileNameW(kernel, path, 4);
    line("GetModuleFileNameW into 4 units: result, terminated");
    say_int((long)n);
    say_error();
    say(" ");
    say_yes(path[3] == 0);
    say(" untouched after ");
    say_yes(path[4] == '#');
    line("LoadLibraryW(kernel32) is the loaded module, FreeLibrary");
    HMODULE again = LoadLibraryW(L"kernel32");
    say_yes(again == kernel);
    say(", ");
    say_int(FreeLibrary(again));
    SetLastError(0);
    line("GetModuleHandleW(nosuch.dll)");
    say_yes(GetModuleHandleW(L"nosuch.dll") == NULL);
    say_error();
    WCHAR system[MAX_PATH];
    UINT length = GetSystemDirectoryW(system, MAX_PATH);
    line("GetSystemDirectoryW");
    say_int((long)length);
    say(" ");
    say_w(system);
    say(", into 3: ");
    say_int((long)GetSystemDirectoryW(system, 3));
    line("PathFindFileNameW");
    say_w(PathFindFileNameW(L"C:\\dir\\file.txt"));
    say(" ");
    say_w(PathFindFileNameW(L"C:\\dir\\"));
    say(" ");
    say_w(PathFindFileNameW(L"plain"));
    say(" ");
    say_w(PathFindFileNameW(L"C:x.txt"));
}

static void check_resources(void) {
    HRSRC r = FindResourceW(NULL, MAKEINTRESOURCEW(100), MAKEINTRESOURCEW(10));
    line("FindResourceW(100, RT_RCDATA): size, text");
    if (r == NULL) {
        say("not found");
    } else {
        say_int((long)SizeofResource(NULL, r));
        say(" ");
        const char *text = LockResource(LoadResource(NULL, r));
        char copy[32];
        unsigned int size = SizeofResource(NULL, r);
        for (unsigned int i = 0; i < size && i < 31; ++i) {
            copy[i] = text[i];
        }
        copy[size < 31 ? size : 31] = 0;
        say(copy);
    }
    line("FindResourceW(\"#100\") and by name DATA");
    say_yes(FindResourceW(NULL, L"#100", MAKEINTRESOURCEW(10)) == r);
    say(" ");
    say_yes(FindResourceW(NULL, L"data", MAKEINTRESOURCEW(10)) != NULL);
    SetLastError(0);
    line("FindResourceW(101)");
    say_yes(FindResourceW(NULL, MAKEINTRESOURCEW(101), MAKEINTRESOURCEW(10)) == NULL);
    say_error();
}

static void check_windows_side(void) {
    RECT a;
    RECT b;
    RECT c;
    SetRect(&a, 1, 2, 11, 12);
    SetRect(&b, 5, 0, 20, 8);
    UnionRect(&c, &a, &b);
    line("UnionRect");
    say_int(c.left);
    say(",");
    say_int(c.top);
    say(",");
    say_int(c.right);
    say(",");
    say_int(c.bottom);
    OffsetRect(&a, -1, 3);
    line("OffsetRect, EqualRect, IsRectEmpty");
    say_int(a.left);
    say(",");
    say_int(a.top);
    say(" ");
    say_int(EqualRect(&a, &a));
    say(" ");
    say_int(EqualRect(&a, &b));
    SetRectEmpty(&c);
    say(" ");
    say_int(IsRectEmpty(&c));
    say(" ");
    say_int(IsRectEmpty(&a));
    UINT f1 = RegisterClipboardFormatW(L"Rich Text Format");
    UINT f2 = RegisterClipboardFormatW(L"rich text FORMAT");
    UINT m = RegisterWindowMessageW(L"Rich Text Format");
    line("RegisterClipboardFormatW: registered range, any case, the same as the message");
    say_yes(f1 >= 0xc000u && f1 <= 0xffffu);
    say(" ");
    say_yes(f1 == f2);
    say(" ");
    say_yes(f1 == m);
    line("FindWindowW of no such class");
    say_yes(FindWindowW(L"WStartupNoSuchClass", NULL) == NULL);
    line("GetSystemMetrics icon, small icon, border, edge, scroll, menu check");
    say_int(GetSystemMetrics(11));
    say(" ");
    say_int(GetSystemMetrics(49));
    say(" ");
    say_int(GetSystemMetrics(5));
    say(" ");
    say_int(GetSystemMetrics(45));
    say(" ");
    say_int(GetSystemMetrics(2));
    say(" ");
    say_int(GetSystemMetrics(71));

    LOGFONTW lf;
    memset(&lf, 0, sizeof lf);
    lf.lfHeight = -13;
    lf.lfWeight = 700;
    lf.lfItalic = 1;
    lf.lfCharSet = 0;
    const WCHAR face[] = L"Arial";
    for (int i = 0; i < 6; ++i) {
        lf.lfFaceName[i] = face[i];
    }
    HFONT font = CreateFontIndirectW(&lf);
    LOGFONTW out;
    memset(&out, 0x55, sizeof out);
    int got = GetObjectW(font, sizeof out, &out);
    line("CreateFontIndirectW then GetObjectW");
    say_int(got);
    say(" ");
    say_int(out.lfHeight);
    say(" ");
    say_int(out.lfWeight);
    say(" ");
    say_int(out.lfItalic);
    say(" ");
    say_w(out.lfFaceName);
    say(" size query ");
    say_int(GetObjectW(font, 0, NULL));
    DeleteObject(font);

    HKEY key = NULL;
    HKEY opened = NULL;
    DWORD disposition = 0;
    LONG created = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\SavanXP fixtures\\wstartup", 0, NULL, 0,
                                   0xf003f, NULL, &key, &disposition);
    LONG open = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\SavanXP fixtures\\wstartup", 0, 0x20019, &opened);
    LONG missing = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\SavanXP fixtures\\nosuch", 0, 0x20019, &opened);
    line("RegCreateKeyExW, RegOpenKeyExW, a missing key");
    say_int(created);
    say(" ");
    say_int(open);
    say(" ");
    say_int(missing);
    RegCloseKey(key);

    line("OleInitialize twice, then uninitialize twice");
    say_hex((unsigned long)OleInitialize(NULL));
    say(" ");
    say_hex((unsigned long)OleInitialize(NULL));
    OleUninitialize();
    OleUninitialize();
    void *task = CoTaskMemAlloc(16);
    line("CoTaskMemAlloc");
    say_yes(task != NULL);
    CoTaskMemFree(task);
}

/* No printer is installed: PD_RETURNDEFAULT finds no default one. The last
 * line is Windows XP RTM's, with no printer; the SP3 machine the rest of the
 * output comes from has one, and there it succeeds. */
static void check_printer(void) {
    unsigned char pd[66] = {0};
    line("CommDlgExtendedError before any dialog");
    say_hex(CommDlgExtendedError());
    line("PrintDlgW with a wrong size: result, error");
    say_int(PrintDlgW(pd));
    say(" ");
    say_hex(CommDlgExtendedError());
    pd[0] = 66;
    pd[21] = 0x04; /* Flags: PD_RETURNDEFAULT (0x400) */
    line("PrintDlgW for the default printer: result, error");
    say_int(PrintDlgW(pd));
    say(" ");
    say_hex(CommDlgExtendedError());
}

static void check_dll_onexit(void) {
    _PVFV *begin = (_PVFV *)malloc(sizeof(_PVFV));
    _PVFV *end = begin;
    __dllonexit(dll_exit_function, &begin, &end);
    __dllonexit(dll_exit_function, &begin, &end);
    line("__dllonexit: entries, then run by hand");
    say_int((long)(end - begin));
    for (_PVFV *p = end; p > begin;) {
        (*--p)();
    }
    say(" ");
    say_int(g_dll_calls);
    free(begin);
}

void __cdecl start(void) {
    say("wstartup: begin");
    _onexit(first_at_exit);
    _onexit(second_at_exit);
    check_arguments();
    check_moveable();
    check_c_heap();
    check_tls_and_locks();
    check_version();
    check_strings();
    check_c_strings();
    check_numbers();
    check_printf();
    check_modules();
    check_resources();
    check_windows_side();
    check_printer();
    check_dll_onexit();
    say("\r\nwstartup: exit 5");
    exit(5);
}
