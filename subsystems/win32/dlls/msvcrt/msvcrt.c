/*
 * msvcrt.dll -- The C runtime functions the W0 matrix names: what a Visual
 * C++ 7 program's start-up code calls (sol.exe's is the classic
 * WinMainCRTStartup over msvcrt.dll), its exit, and rand, srand and time.
 * Nothing else of the C runtime is here.
 */
#include <windows.h>

#include "codepage.h"
#include "sxw_abi.h"

typedef unsigned int u32;
typedef void(__cdecl *_PVFV)(void);
typedef struct {
    int newmode;
} _startupinfo;

__declspec(dllimport) void *WINAPI SxwHeapAllocate(u32 size, u32 zero);
__declspec(dllimport) u32 WINAPI SxwTime(void);

/* ---- Data -------------------------------------------------------------------- */

/* The Pentium FDIV flaw: no processor this runs on has it. */
int _adjust_fdiv = 0;
/* The command line, in the ANSI code page, set when the DLL attaches. */
char *_acmdln = 0;

/* Kept for what a later C runtime function may need to know: console or
 * GUI, and the program's _matherr. */
static volatile int g_app_type;
static int g_fmode;   /* _O_TEXT */
static int g_commode; /* no _commode flags */
static int(__cdecl *volatile g_matherr)(void *);
static u32 g_seed = 1;

/* ---- Start-up ------------------------------------------------------------------ */

void __cdecl __set_app_type(int type) {
    g_app_type = type;
}

int *__cdecl __p__fmode(void) {
    return &g_fmode;
}

int *__cdecl __p__commode(void) {
    return &g_commode;
}

/* Kept, never called: nothing here computes a math function that reports an
 * error through _matherr. */
void __cdecl __setusermatherr(int(__cdecl *handler)(void *)) {
    g_matherr = handler;
}

void __cdecl _initterm(_PVFV *begin, _PVFV *end) {
    for (_PVFV *p = begin; p < end; ++p) {
        if (*p != 0) {
            (*p)();
        }
    }
}

/* Splits a command line the way msvcrt.dll does: argv[0] up to the first
 * space, or between quotes with no escapes; then arguments separated by
 * spaces and tabs, where 2n backslashes before a quote give n and toggle
 * quoting, 2n+1 give n and a literal quote, and "" inside quotes is a quote.
 * With out NULL it only counts. Returns the argument count and *chars, the
 * bytes the strings need with their NULs. */
static int split(const char *line, char **argv, char *out, u32 *chars) {
    const char *s = line;
    int argc = 0;
    u32 n = 0;
    /* argv[0] */
    if (argv != 0) {
        argv[argc] = out + n;
    }
    if (*s == '"') {
        ++s;
        while (*s != 0 && *s != '"') {
            if (out != 0) {
                out[n] = *s;
            }
            ++n;
            ++s;
        }
        if (*s == '"') {
            ++s;
        }
    } else {
        while (*s != 0 && *s != ' ' && *s != '\t') {
            if (out != 0) {
                out[n] = *s;
            }
            ++n;
            ++s;
        }
    }
    if (out != 0) {
        out[n] = 0;
    }
    ++n;
    ++argc;
    for (;;) {
        while (*s == ' ' || *s == '\t') {
            ++s;
        }
        if (*s == 0) {
            break;
        }
        if (argv != 0) {
            argv[argc] = out + n;
        }
        int quoted = 0;
        while (*s != 0 && (quoted || (*s != ' ' && *s != '\t'))) {
            u32 slashes = 0;
            while (*s == '\\') {
                ++slashes;
                ++s;
            }
            if (*s == '"') {
                for (u32 i = 0; i < slashes / 2u; ++i) {
                    if (out != 0) {
                        out[n] = '\\';
                    }
                    ++n;
                }
                if ((slashes & 1u) != 0u) {
                    if (out != 0) {
                        out[n] = '"';
                    }
                    ++n;
                    ++s;
                } else if (quoted && s[1] == '"') {
                    if (out != 0) {
                        out[n] = '"';
                    }
                    ++n;
                    s += 2;
                } else {
                    quoted = !quoted;
                    ++s;
                }
            } else {
                for (u32 i = 0; i < slashes; ++i) {
                    if (out != 0) {
                        out[n] = '\\';
                    }
                    ++n;
                }
                if (*s != 0 && (quoted || (*s != ' ' && *s != '\t'))) {
                    if (out != 0) {
                        out[n] = *s;
                    }
                    ++n;
                    ++s;
                }
            }
        }
        if (out != 0) {
            out[n] = 0;
        }
        ++n;
        ++argc;
    }
    *chars = n;
    return argc;
}

/* argc and argv from _acmdln; envp is empty, since there is no environment
 * yet. Wildcards are not expanded: the profile does not ask for it. */
int __cdecl __getmainargs(int *argc, char ***argv, char ***envp, int expand_wildcards,
                          _startupinfo *startup) {
    (void)expand_wildcards;
    (void)startup;
    static char *empty_environment[1] = {0};
    const char *line = _acmdln != 0 ? _acmdln : "";
    u32 chars = 0;
    int count = split(line, 0, 0, &chars);
    char **vector = SxwHeapAllocate((u32)(count + 1) * sizeof(char *) + chars, 0);
    if (vector == 0) {
        *argc = 0;
        *argv = empty_environment;
        *envp = empty_environment;
        return -1;
    }
    char *strings = (char *)(vector + count + 1);
    split(line, vector, strings, &chars);
    vector[count] = 0;
    *argc = count;
    *argv = vector;
    *envp = empty_environment;
    return 0;
}

/* ---- Floating point ------------------------------------------------------------ */

#define MCW_EM 0x0008001fu
#define MCW_IC 0x00040000u
#define MCW_RC 0x00000300u
#define MCW_PC 0x00030000u

/* x87 control word <-> _controlfp's word. Exception masks map bit for bit
 * (invalid, denormal, zero divide, overflow, underflow, inexact); rounding
 * keeps its order; precision 24/53/64 is _PC_24/_PC_53/_PC_64; infinity
 * control is _IC_AFFINE. */
static u32 from_x87(unsigned short cw) {
    u32 v = 0;
    if (cw & 0x01) v |= 0x10u;      /* _EM_INVALID */
    if (cw & 0x02) v |= 0x80000u;   /* _EM_DENORMAL */
    if (cw & 0x04) v |= 0x08u;      /* _EM_ZERODIVIDE */
    if (cw & 0x08) v |= 0x04u;      /* _EM_OVERFLOW */
    if (cw & 0x10) v |= 0x02u;      /* _EM_UNDERFLOW */
    if (cw & 0x20) v |= 0x01u;      /* _EM_INEXACT */
    v |= (u32)((cw >> 10) & 3u) << 8; /* _RC_* */
    switch ((cw >> 8) & 3u) {
    case 0: v |= 0x20000u; break;   /* _PC_24 */
    case 2: v |= 0x10000u; break;   /* _PC_53 */
    default: break;                 /* _PC_64 */
    }
    if (cw & 0x1000) v |= 0x40000u; /* _IC_AFFINE */
    return v;
}

static unsigned short to_x87(u32 v, unsigned short cw) {
    cw &= (unsigned short)~0x1f3fu;
    if (v & 0x10u) cw |= 0x01;
    if (v & 0x80000u) cw |= 0x02;
    if (v & 0x08u) cw |= 0x04;
    if (v & 0x04u) cw |= 0x08;
    if (v & 0x02u) cw |= 0x10;
    if (v & 0x01u) cw |= 0x20;
    cw |= (unsigned short)(((v >> 8) & 3u) << 10);
    switch (v & MCW_PC) {
    case 0x20000u: break;                /* 24 */
    case 0x10000u: cw |= 0x200; break;   /* 53 */
    default: cw |= 0x300; break;         /* 64 */
    }
    if (v & 0x40000u) cw |= 0x1000;
    return cw;
}

unsigned int __cdecl _controlfp(unsigned int new_value, unsigned int mask) {
    unsigned short cw;
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    mask &= MCW_EM | MCW_IC | MCW_RC | MCW_PC;
    if (mask != 0u) {
        u32 current = from_x87(cw);
        current = (current & ~mask) | (new_value & mask);
        cw = to_x87(current, cw);
        __asm__ volatile("fldcw %0" : : "m"(cw));
    }
    return from_x87(cw);
}

/* ---- Exceptions ------------------------------------------------------------------- */
/*
 * The start-up code registers _except_handler3 as its frame's handler, and
 * _XcptFilter is its filter. Nothing delivers an exception to a frame until
 * the S3 slice (docs/roadmaps/open/WIN32_SUBSYSTEM_ROADMAP.md): a fault ends
 * the process in the kernel first. Both answer "keep searching", which is
 * what a frame with no matching scope does.
 */

int __cdecl _except_handler3(void *record, void *frame, void *context, void *dispatcher) {
    (void)record;
    (void)frame;
    (void)context;
    (void)dispatcher;
    return 1; /* ExceptionContinueSearch */
}

int __cdecl _XcptFilter(unsigned long code, void *pointers) {
    (void)code;
    (void)pointers;
    return 0; /* EXCEPTION_CONTINUE_SEARCH */
}

/* ---- Exit ----------------------------------------------------------------------- */
/*
 * Nothing registers an exit function (atexit and _onexit are not in the
 * matrix) and there is no stdio to flush, so the cleanups have nothing to
 * run and exit is ExitProcess.
 */

void __cdecl _cexit(void) {
}

void __cdecl _c_exit(void) {
}

__declspec(noreturn) void __cdecl exit(int status) {
    ExitProcess((unsigned int)status);
}

__declspec(noreturn) void __cdecl _exit(int status) {
    ExitProcess((unsigned int)status);
}

/* ---- rand, srand, time ---------------------------------------------------------- */

/* Microsoft's generator, so a seed deals the same cards as on Windows. */
int __cdecl rand(void) {
    g_seed = g_seed * 214013u + 2531011u;
    return (int)((g_seed >> 16) & 0x7fffu);
}

void __cdecl srand(unsigned int seed) {
    g_seed = seed;
}

/* time_t is 32 bits in msvcrt.dll. */
long __cdecl time(long *out) {
    long now = (long)SxwTime();
    if (out != 0) {
        *out = now;
    }
    return now;
}

/* ---- Attach ---------------------------------------------------------------------- */

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)instance;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        const WCHAR *wide = GetCommandLineW();
        int n = lstrlenW(wide);
        _acmdln = SxwHeapAllocate((u32)n + 1u, 0);
        if (_acmdln == 0) {
            return FALSE;
        }
        for (int i = 0; i < n; ++i) {
            _acmdln[i] = (char)sxw_wide_to_ansi(wide[i]);
        }
        _acmdln[n] = 0;
    }
    return TRUE;
}
