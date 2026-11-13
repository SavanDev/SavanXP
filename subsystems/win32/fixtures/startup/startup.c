/*
 * startup.exe -- The W2 fixture. It starts the way sol.exe does: Visual
 * C++ 7's WinMainCRTStartup over msvcrt.dll, written out (the same runtime
 * imports, a structured-exception frame at fs:[0] whose handler is
 * _except_handler3, C and C++ initializers through _initterm, argv from
 * __getmainargs, the command tail from _acmdln, a WinMain). Then it checks
 * each non-GUI function of the W0 matrix, failure paths included, and writes
 * what it saw. Everything it writes is the same on Windows XP; it prints no
 * address, handle, path or time.
 *
 * Run it as: startup.exe alpha "two words" 3   (twice: the registry counter
 * shows the first run's value read back). beta.dll must sit next to it.
 */
#include <windows.h>

typedef void(__cdecl *_PVFV)(void);
typedef struct {
    int newmode;
} _startupinfo;

__declspec(dllimport) void __cdecl __set_app_type(int type);
__declspec(dllimport) int *__cdecl __p__fmode(void);
__declspec(dllimport) int *__cdecl __p__commode(void);
__declspec(dllimport) extern int _adjust_fdiv;
__declspec(dllimport) void __cdecl __setusermatherr(int(__cdecl *handler)(void *));
__declspec(dllimport) void __cdecl _initterm(_PVFV *begin, _PVFV *end);
__declspec(dllimport) int __cdecl __getmainargs(int *argc, char ***argv, char ***envp, int wildcards,
                                                 _startupinfo *info);
__declspec(dllimport) extern char *_acmdln;
__declspec(dllimport) unsigned int __cdecl _controlfp(unsigned int value, unsigned int mask);
__declspec(dllimport) int __cdecl _except_handler3(void *, void *, void *, void *);
__declspec(dllimport) int __cdecl _XcptFilter(unsigned long code, void *pointers);
__declspec(dllimport) __declspec(noreturn) void __cdecl exit(int status);
__declspec(dllimport) __declspec(noreturn) void __cdecl _exit(int status);
__declspec(dllimport) void __cdecl _cexit(void);
__declspec(dllimport) void __cdecl _c_exit(void);
__declspec(dllimport) int __cdecl rand(void);
__declspec(dllimport) void __cdecl srand(unsigned int seed);
__declspec(dllimport) long __cdecl time(long *out);

/* Imported, as sol.exe imports them, and never called (sol.exe never calls
 * them either). */
void *volatile g_never_called[2] = {(void *)_XcptFilter, (void *)_exit};

/* ---- Output -------------------------------------------------------------------- */

static void say(const char *text) {
    DWORD length = 0;
    while (text[length] != 0) {
        ++length;
    }
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, length, &written, NULL);
}

static void say_int(int value) {
    char digits[12];
    int n = 11;
    unsigned magnitude = value < 0 ? 0u - (unsigned)value : (unsigned)value;
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

static void say_hex(unsigned value) {
    char digits[11] = "0x";
    for (int i = 0; i < 8; ++i) {
        unsigned nibble = (value >> (28 - 4 * i)) & 0xfu;
        digits[2 + i] = (char)(nibble < 10u ? '0' + nibble : 'a' + nibble - 10u);
    }
    digits[10] = 0;
    say(digits);
}

static void say_yes(int ok) {
    say(ok ? "yes" : "NO");
}

static DWORD last_error(void) {
    DWORD value;
    __asm__ volatile("movl %%fs:0x34, %0" : "=r"(value));
    return value;
}

/* ---- Initializers, as the C runtime's sections hold them --------------------- */

#pragma section(".CRT$XIA", read)
#pragma section(".CRT$XIU", read)
#pragma section(".CRT$XIZ", read)
#pragma section(".CRT$XCA", read)
#pragma section(".CRT$XCU", read)
#pragma section(".CRT$XCZ", read)

static int g_c_initializers;
static int g_cpp_initializers;

static void __cdecl c_initializer(void) {
    ++g_c_initializers;
}

static void __cdecl cpp_initializer(void) {
    ++g_cpp_initializers;
}

__declspec(allocate(".CRT$XIA")) _PVFV __xi_a[] = {0};
__declspec(allocate(".CRT$XIU")) _PVFV g_xi[] = {c_initializer};
__declspec(allocate(".CRT$XIZ")) _PVFV __xi_z[] = {0};
__declspec(allocate(".CRT$XCA")) _PVFV __xc_a[] = {0};
__declspec(allocate(".CRT$XCU")) _PVFV g_xc[] = {cpp_initializer, cpp_initializer};
__declspec(allocate(".CRT$XCZ")) _PVFV __xc_z[] = {0};

static int __cdecl user_matherr(void *exception) {
    (void)exception;
    return 0;
}

/* ---- WinMain -------------------------------------------------------------------- */

static void check_random(void) {
    static const unsigned seeds[2] = {1u, 12345u};
    for (int s = 0; s < 2; ++s) {
        srand(seeds[s]);
        say("winmain: srand(");
        say_int((int)seeds[s]);
        say("):");
        for (int i = 0; i < 6; ++i) {
            say(" ");
            say_int(rand());
        }
        say("\r\n");
    }
    long first = time(NULL);
    long second = 0;
    long returned = time(&second);
    say("winmain: time plausible: ");
    say_yes(first > 1000000000L && returned == second && second >= first && second - first < 5);
    say("\r\n");
}

static void check_arithmetic(void) {
    static const int cases[][3] = {
        {10, 3, 4},   {-10, 3, 4},  {10, -3, 4}, {10, 3, -4}, {7, 2, 4},   {-7, 2, 4},
        {1, 1, 3},    {2, 1, 3},    {300, 96, 72}, {5, 5, 0}, {0x7fffffff, 2, 1},
        {-0x7fffffff, 1, 1}, {0x7fffffff, 0x7fffffff, 0x7fffffff}, {-5, -5, 2},
    };
    say("winmain: MulDiv:");
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        say(" ");
        say_int(MulDiv(cases[i][0], cases[i][1], cases[i][2]));
    }
    say("\r\n");
    say("winmain: lstrlenW: ");
    say_int(lstrlenW(L"Solitario"));
    say(" ");
    say_int(lstrlenW(NULL));
    say("\r\n");
}

/* The command line past the program name: quoted or not, then blanks. */
static void check_command_line(void) {
    const WCHAR *line = GetCommandLineW();
    if (*line == '"') {
        ++line;
        while (*line != 0 && *line != '"') {
            ++line;
        }
        if (*line == '"') {
            ++line;
        }
    } else {
        while (*line > ' ') {
            ++line;
        }
    }
    while (*line == ' ' || *line == '\t') {
        ++line;
    }
    char narrow[128];
    int n = 0;
    while (line[n] != 0 && n < 127) {
        narrow[n] = line[n] < 0x80u ? (char)line[n] : '?';
        ++n;
    }
    narrow[n] = 0;
    say("winmain: GetCommandLineW tail: [");
    say(narrow);
    say("]\r\n");
}

static void check_modules(HINSTANCE instance) {
    HMODULE kernel32 = GetModuleHandleA("KERNEL32");
    say("winmain: GetModuleHandleA(NULL) is the instance: ");
    say_yes(GetModuleHandleA(NULL) == (HMODULE)instance);
    say(", kernel32 by two names: ");
    say_yes(kernel32 != NULL && GetModuleHandleA("kernel32.dll") == kernel32);
    say("\r\n");
    say("winmain: GetModuleHandleA(nosuch): ");
    say_yes(GetModuleHandleA("nosuch") == NULL);
    say(", error ");
    say_int((int)last_error());
    say("\r\n");

    say("winmain: LoadLibraryA(kernel32) is the loaded one: ");
    say_yes(LoadLibraryA("kernel32.dll") == kernel32);
    say("\r\n");
    say("winmain: LoadLibraryA(nosuch.dll): ");
    say_yes(LoadLibraryA("nosuch.dll") == NULL);
    say(", error ");
    say_int((int)last_error());
    say("\r\n");

    say("winmain: GetProcAddress(kernel32, MulDiv) is the import: ");
    say_yes((void *)GetProcAddress(kernel32, "MulDiv") == (void *)MulDiv);
    say("\r\n");

    HMODULE beta = LoadLibraryA("beta.dll");
    say("winmain: LoadLibraryA(beta.dll): ");
    say_yes(beta != NULL);
    say("\r\n");
    if (beta != NULL) {
        typedef unsigned(WINAPI * value_fn)(void);
        value_fn by_name = (value_fn)GetProcAddress(beta, "BetaValue");
        value_fn by_ordinal = (value_fn)GetProcAddress(beta, (LPCSTR)1);
        say("winmain: BetaValue by name says ");
        say_int(by_name != NULL ? (int)by_name() : -1);
        say(", by ordinal the same: ");
        say_yes(by_ordinal != NULL && by_ordinal == by_name);
        say("\r\n");
        say("winmain: GetProcAddress(beta, Nope): ");
        say_yes(GetProcAddress(beta, "Nope") == NULL);
        say(", error ");
        say_int((int)last_error());
        say("\r\n");
    }
}

/* What sol.exe allocates: LPTR blocks of at most 860 bytes, 115 of them. */
static void check_memory(void) {
    unsigned char *blocks[115];
    int zeroed = 1;
    int allocated = 1;
    for (int i = 0; i < 115; ++i) {
        unsigned size = (unsigned)(i * 37 % 860) + 1u;
        blocks[i] = LocalAlloc(LPTR, size);
        if (blocks[i] == NULL) {
            allocated = 0;
            continue;
        }
        for (unsigned b = 0; b < size; ++b) {
            zeroed &= blocks[i][b] == 0;
            blocks[i][b] = (unsigned char)(i + 1);
        }
    }
    int intact = 1;
    for (int i = 0; i < 115; ++i) {
        unsigned size = (unsigned)(i * 37 % 860) + 1u;
        for (unsigned b = 0; blocks[i] != NULL && b < size; ++b) {
            intact &= blocks[i][b] == (unsigned char)(i + 1);
        }
    }
    int freed = 1;
    for (int i = 0; i < 115; ++i) {
        freed &= LocalFree(blocks[i]) == NULL;
    }
    void *empty = LocalAlloc(LPTR, 0);
    say("winmain: LocalAlloc 115 LPTR blocks: ");
    say_yes(allocated && zeroed && intact);
    say(", LocalFree: ");
    say_yes(freed);
    say(", zero bytes: ");
    say_yes(empty != NULL && LocalFree(empty) == NULL);
    say("\r\n");
}

static void check_registry(void) {
    HKEY key = NULL;
    DWORD disposition = 0;
    LONG error = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\SavanXP\\W2Fixture", 0, NULL,
                                 REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, NULL, &key, &disposition);
    say("winmain: RegCreateKeyExW: ");
    say_int(error);
    say(", disposition ");
    say_int((int)disposition);
    say("\r\n");
    if (error != ERROR_SUCCESS) {
        return;
    }

    DWORD count = 0;
    DWORD type = 0;
    DWORD size = sizeof(count);
    error = RegQueryValueExW(key, L"Contador", NULL, &type, (LPBYTE)&count, &size);
    say("winmain: counter before: ");
    say_int(error == ERROR_SUCCESS ? (int)count : -(int)error);
    count = error == ERROR_SUCCESS ? count + 1u : 1u;
    say(", set: ");
    say_int(RegSetValueExW(key, L"Contador", 0, REG_DWORD, (const BYTE *)&count, sizeof(count)));
    count = 0;
    size = sizeof(count);
    error = RegQueryValueExW(key, L"Contador", NULL, &type, (LPBYTE)&count, &size);
    say(", read back: ");
    say_int((int)count);
    say(" type ");
    say_int((int)type);
    say(" size ");
    say_int((int)size);
    say("\r\n");

    static const WCHAR name[] = L"Solitario";
    say("winmain: REG_SZ set: ");
    say_int(RegSetValueExW(key, L"Nombre", 0, REG_SZ, (const BYTE *)name, sizeof(name)));
    WCHAR small[2];
    size = sizeof(small);
    error = RegQueryValueExW(key, L"Nombre", NULL, &type, (LPBYTE)small, &size);
    say(", small buffer: ");
    say_int(error);
    say(" needs ");
    say_int((int)size);
    size = 0;
    error = RegQueryValueExW(key, L"Nombre", NULL, NULL, NULL, &size);
    say(", size query: ");
    say_int(error);
    say(" ");
    say_int((int)size);
    say("\r\n");

    char ansi[32];
    size = sizeof(ansi);
    error = RegQueryValueExA(key, "Nombre", NULL, &type, (LPBYTE)ansi, &size);
    say("winmain: RegQueryValueExA: ");
    say_int(error);
    say(" [");
    say(error == ERROR_SUCCESS ? ansi : "");
    say("] size ");
    say_int((int)size);
    say("\r\n");

    /* "Atrás", one of sol.exe's own value names: written in UTF-16, read
     * through the ANSI name. */
    DWORD back = 53;
    say("winmain: Atr\xe1s: set ");
    say_int(RegSetValueExW(key, L"Atr\x00e1s", 0, REG_DWORD, (const BYTE *)&back, sizeof(back)));
    back = 0;
    size = sizeof(back);
    error = RegQueryValueExA(key, "Atr\xe1s", NULL, &type, (LPBYTE)&back, &size);
    say(", read through ANSI: ");
    say_int(error);
    say(" ");
    say_int((int)back);
    say("\r\n");

    size = sizeof(count);
    say("winmain: missing value: ");
    say_int(RegQueryValueExW(key, L"Nada", NULL, &type, (LPBYTE)&count, &size));
    HKEY other = NULL;
    say(", missing key: ");
    say_int(RegOpenKeyExA(HKEY_CLASSES_ROOT, "CLSID\\{00000000-5341-5658-0000-000000000002}", 0, KEY_READ,
                          &other));
    say(", reopen: ");
    error = RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\SavanXP\\W2Fixture", 0, KEY_READ, &other);
    say_int(error);
    if (error == ERROR_SUCCESS) {
        say(" close ");
        say_int(RegCloseKey(other));
    }
    say(", close: ");
    say_int(RegCloseKey(key));
    say("\r\n");
}

static int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command_tail, int show) {
    say("winmain: previous instance: ");
    say_yes(previous == NULL);
    say(", tail [");
    say(command_tail);
    say("], show ");
    say_int(show);
    say("\r\n");
    check_random();
    check_arithmetic();
    check_command_line();
    check_modules(instance);
    check_memory();
    check_registry();
    return 5;
}

/* ---- WinMainCRTStartup ---------------------------------------------------------- */

typedef struct registration {
    struct registration *next;
    void *handler;
} registration;

void WINAPI start(void) {
    /* The frame the C runtime's start-up registers first: a write to fs:[0]. */
    registration frame;
    __asm__ volatile("movl %%fs:0, %0" : "=r"(frame.next));
    frame.handler = (void *)_except_handler3;
    __asm__ volatile("movl %0, %%fs:0" : : "r"(&frame) : "memory");

    __set_app_type(1); /* _CONSOLE_APP: this fixture is a console program */
    *__p__fmode() = 0;
    *__p__commode() = 0;
    say("startup: fmode ");
    say_int(*__p__fmode());
    say(", commode ");
    say_int(*__p__commode());
    say(", adjust_fdiv ");
    say_int(_adjust_fdiv);
    say("\r\n");

    say("startup: control word ");
    say_hex(_controlfp(0, 0));
    say(", after _PC_53 ");
    say_hex(_controlfp(0x10000u, 0x30000u));
    say(", chopping ");
    say_hex(_controlfp(0x300u, 0x300u));
    say(", back to nearest ");
    say_hex(_controlfp(0u, 0x300u));
    say("\r\n");
    __setusermatherr(user_matherr);

    int argc = 0;
    char **argv = NULL;
    char **envp = NULL;
    _startupinfo info = {0};
    _initterm(__xi_a, __xi_z);
    __getmainargs(&argc, &argv, &envp, 0, &info);
    _initterm(__xc_a, __xc_z);
    say("startup: initializers: C ");
    say_int(g_c_initializers);
    say(", C++ ");
    say_int(g_cpp_initializers);
    say("\r\n");
    say("startup: argc ");
    say_int(argc);
    for (int i = 1; i < argc; ++i) {
        say(", [");
        say(argv[i]);
        say("]");
    }
    say("\r\n");

    /* lpCmdLine: _acmdln past the program name, as WinMainCRTStartup finds
     * it. */
    unsigned char *tail = (unsigned char *)_acmdln;
    if (*tail == '"') {
        while (*++tail != 0 && *tail != '"') {
        }
        if (*tail == '"') {
            ++tail;
        }
    } else {
        while (*tail > ' ') {
            ++tail;
        }
    }
    while (*tail != 0 && *tail <= ' ') {
        ++tail;
    }

    STARTUPINFOA startup;
    startup.dwFlags = 0;
    GetStartupInfoA(&startup);
    say("startup: STARTUPINFOA size ");
    say_int((int)startup.cb);
    say("\r\n");
    int show = (startup.dwFlags & STARTF_USESHOWWINDOW) != 0u ? startup.wShowWindow : SW_SHOWDEFAULT;

    int status = WinMain((HINSTANCE)GetModuleHandleA(NULL), NULL, (LPSTR)tail, show);
    __asm__ volatile("movl %0, %%fs:0" : : "r"(frame.next) : "memory");
    _cexit();
    _c_exit();
    exit(status);
}
