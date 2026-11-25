/*
 * memory.exe -- WP4's fixture for memory and atoms (WIN32_WORDPAD_ROADMAP.md):
 * private heaps, the process heap, moveable global memory shared for DDE as
 * the clipboard asks for it, and local and global atoms, failure paths
 * included.
 *
 * No address or atom number is printed, only how they relate, so it writes
 * the same on Windows XP, in the harness and on SavanXP. Global atoms are
 * the whole system's on Windows XP: the names are unusual enough that no
 * other program has them.
 */
#include "print.h"

typedef WORD ATOM_T;

DECLSPEC_IMPORT HANDLE WINAPI GetProcessHeap(void);
DECLSPEC_IMPORT HANDLE WINAPI HeapCreate(DWORD options, SIZE_T initial, SIZE_T maximum);
DECLSPEC_IMPORT BOOL WINAPI HeapDestroy(HANDLE heap);
DECLSPEC_IMPORT LPVOID WINAPI HeapAlloc(HANDLE heap, DWORD flags, SIZE_T bytes);
DECLSPEC_IMPORT LPVOID WINAPI HeapReAlloc(HANDLE heap, DWORD flags, LPVOID block, SIZE_T bytes);
DECLSPEC_IMPORT BOOL WINAPI HeapFree(HANDLE heap, DWORD flags, LPVOID block);
DECLSPEC_IMPORT SIZE_T WINAPI HeapSize(HANDLE heap, DWORD flags, LPCVOID block);
DECLSPEC_IMPORT ATOM_T WINAPI AddAtomW(LPCWSTR name);
DECLSPEC_IMPORT ATOM_T WINAPI FindAtomW(LPCWSTR name);
DECLSPEC_IMPORT ATOM_T WINAPI DeleteAtom(ATOM_T atom);
DECLSPEC_IMPORT UINT WINAPI GetAtomNameW(ATOM_T atom, LPWSTR buffer, int size);
DECLSPEC_IMPORT ATOM_T WINAPI GlobalAddAtomW(LPCWSTR name);
DECLSPEC_IMPORT ATOM_T WINAPI GlobalFindAtomW(LPCWSTR name);
DECLSPEC_IMPORT ATOM_T WINAPI GlobalDeleteAtom(ATOM_T atom);
DECLSPEC_IMPORT UINT WINAPI GlobalGetAtomNameW(ATOM_T atom, LPWSTR buffer, int size);

#define HEAP_NO_SERIALIZE 0x1u
#define HEAP_ZERO_MEMORY 0x8u
#define HEAP_REALLOC_IN_PLACE_ONLY 0x10u
#define MAKEINTATOM(i) ((LPCWSTR)(ULONG_PTR)(WORD)(i))

static void say_pointer(const void *p) {
    say(p != NULL ? "block" : "null");
    if (p == NULL) {
        say_error();
    }
}

static void heaps(void) {
    line("GetProcessHeap");
    HANDLE process = GetProcessHeap();
    say_yes(process != NULL);
    line("HeapAlloc(process, 0)");
    unsigned char *empty = HeapAlloc(process, 0, 0);
    say_pointer(empty);
    say(" size ");
    say_int((long)HeapSize(process, 0, empty));
    say(" free ");
    say_result(HeapFree(process, 0, empty));

    line("HeapCreate(0, 0, 0)");
    HANDLE heap = HeapCreate(0, 0, 0);
    say_yes(heap != NULL && heap != process);
    line("HeapAlloc(10)");
    SetLastError(0);
    unsigned char *p = HeapAlloc(heap, 0, 10);
    say_pointer(p);
    say(" size ");
    say_int((long)HeapSize(heap, 0, p));
    say(" aligned ");
    say_yes(((ULONG_PTR)p & 7u) == 0u);
    for (int i = 0; i < 10; ++i) {
        p[i] = (unsigned char)('a' + i);
    }
    line("HeapAlloc(HEAP_ZERO_MEMORY, 300)");
    unsigned char *z = HeapAlloc(heap, HEAP_ZERO_MEMORY, 300);
    int zero = z != NULL;
    for (int i = 0; zero && i < 300; ++i) {
        zero = z[i] == 0;
    }
    say_yes(zero);
    line("HeapReAlloc(10 -> 5000) keeps the bytes");
    unsigned char *q = HeapReAlloc(heap, 0, p, 5000);
    int kept = q != NULL;
    for (int i = 0; kept && i < 10; ++i) {
        kept = q[i] == (unsigned char)('a' + i);
    }
    say_yes(kept);
    say(" size ");
    say_int((long)HeapSize(heap, 0, q));
    line("HeapReAlloc(HEAP_ZERO_MEMORY, 5000 -> 6000) zeroes the rest");
    unsigned char *r = HeapReAlloc(heap, HEAP_ZERO_MEMORY, q, 6000);
    zero = r != NULL;
    for (int i = 5000; zero && i < 6000; ++i) {
        zero = r[i] == 0;
    }
    say_yes(zero && r[3] == 'd');
    line("HeapReAlloc(IN_PLACE_ONLY, 6000 -> 3000)");
    unsigned char *smaller = HeapReAlloc(heap, HEAP_REALLOC_IN_PLACE_ONLY, r, 3000);
    say_yes(smaller == r);
    say(" size ");
    say_int((long)HeapSize(heap, 0, r));
    line("HeapReAlloc(IN_PLACE_ONLY, 3000 -> 4 MiB)");
    SetLastError(0);
    say_pointer(HeapReAlloc(heap, HEAP_REALLOC_IN_PLACE_ONLY, r, 4u << 20));
    say(" still ");
    say_int((long)HeapSize(heap, 0, r));
    line("HeapFree(NULL)");
    say_result(HeapFree(heap, 0, NULL));
    line("HeapFree");
    say_result(HeapFree(heap, 0, r));
    line("HeapFree(zeroed)");
    say_result(HeapFree(heap, 0, z));
    line("many blocks in a no-serialize heap");
    HANDLE quick = HeapCreate(HEAP_NO_SERIALIZE, 4096, 0);
    void *blocks[200];
    int all = quick != NULL;
    for (int i = 0; i < 200 && all; ++i) {
        blocks[i] = HeapAlloc(quick, 0, 100u + 37u * (unsigned)i);
        all = blocks[i] != NULL && HeapSize(quick, 0, blocks[i]) == 100u + 37u * (unsigned)i;
    }
    for (int i = 0; i < 200 && all; i += 2) {
        all = HeapFree(quick, 0, blocks[i]);
    }
    say_yes(all);
    line("HeapDestroy with blocks in it");
    say_result(HeapDestroy(quick));
    line("HeapDestroy");
    say_result(HeapDestroy(heap));

    line("HeapCreate(0, 4 KiB, 64 KiB)");
    HANDLE fixed = HeapCreate(0, 4096, 65536);
    say_yes(fixed != NULL);
    line("HeapAlloc(32 KiB) in it");
    void *half = HeapAlloc(fixed, 0, 32768);
    say_pointer(half);
    line("HeapAlloc(128 KiB) in it");
    SetLastError(0);
    say_pointer(HeapAlloc(fixed, 0, 131072));
    HeapFree(fixed, 0, half);
    HeapDestroy(fixed);
}

static void moveable(void) {
    line("GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, 16)");
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, 16);
    say_yes(h != NULL);
    say(" flags ");
    say_hex(GlobalFlags(h));
    line("GlobalLock");
    char *p = GlobalLock(h);
    say_yes(p != NULL);
    say(" flags ");
    say_hex(GlobalFlags(h));
    p[0] = 'x';
    p[15] = 'y';
    line("GlobalHandle(data)");
    say_yes(GlobalHandle(p) == h);
    line("GlobalUnlock");
    say_int(GlobalUnlock(h));
    say(" flags ");
    say_hex(GlobalFlags(h));
    line("GlobalReAlloc(4000, GMEM_MOVEABLE) keeps the handle");
    HGLOBAL grown = GlobalReAlloc(h, 4000, GMEM_MOVEABLE);
    say_yes(grown == h);
    say(" size ");
    say_int((long)GlobalSize(h));
    p = GlobalLock(h);
    say(" bytes ");
    say_yes(p[0] == 'x' && p[15] == 'y');
    GlobalUnlock(h);
    line("GlobalAlloc(GHND | GMEM_DDESHARE, 64) zeroed");
    HGLOBAL second = GlobalAlloc(GHND | GMEM_DDESHARE, 64);
    p = GlobalLock(second);
    int zero = p != NULL;
    for (int i = 0; zero && i < 64; ++i) {
        zero = p[i] == 0;
    }
    say_yes(zero);
    GlobalUnlock(second);
    line("GlobalFree");
    say_yes(GlobalFree(h) == NULL);
    say(" ");
    say_yes(GlobalFree(second) == NULL);
}

static void say_atom_name(UINT(WINAPI *get)(ATOM_T, LPWSTR, int), ATOM_T atom, int size) {
    WCHAR name[300];
    name[0] = '#';
    name[1] = 0;
    const UINT n = get(atom, name, size);
    say_int((long)n);
    if (n == 0u) {
        say_error();
    } else {
        say(" ");
        say_w(name);
    }
}

static void atoms(ATOM_T(WINAPI *add)(LPCWSTR), ATOM_T(WINAPI *find)(LPCWSTR), ATOM_T(WINAPI *remove)(ATOM_T),
                  UINT(WINAPI *get)(ATOM_T, LPWSTR, int), const WCHAR *mixed, const WCHAR *upper,
                  const WCHAR *other, int small_buffer) {
    line("add");
    const ATOM_T a = add(mixed);
    say_yes(a >= 0xc000u);
    line("add again, other case");
    say_yes(add(upper) == a);
    line("find");
    say_yes(find(upper) == a);
    line("name, first spelling");
    say_atom_name(get, a, 64);
    /* Windows XP's GlobalGetAtomNameW writes the whole name past a short
     * buffer: only the local one is asked. */
    if (small_buffer) {
        line("name in 5 units");
        say_atom_name(get, a, 5);
    }
    line("name in 0 units");
    say_atom_name(get, a, 0);
    line("a second name");
    const ATOM_T b = add(other);
    say_yes(b >= 0xc000u && b != a);
    line("delete once");
    say_int(remove(a));
    line("still there");
    say_yes(find(mixed) == a);
    line("delete twice");
    say_int(remove(a));
    line("gone");
    SetLastError(0);
    say_int(find(mixed));
    say_error();
    line("name of a deleted atom");
    say_atom_name(get, a, 64);
    line("delete a deleted atom");
    say_int(remove(a));
    say_error();
    remove(b);
    line("add #123");
    say_int(add(L"#123"));
    line("find #123");
    say_int(find(L"#123"));
    line("name of 123");
    say_atom_name(get, 123, 64);
    line("delete 123");
    say_int(remove(123));
    line("add MAKEINTATOM(456)");
    say_int(add(MAKEINTATOM(456)));
    line("add #0");
    say_int(add(L"#0"));
    say_error();
    line("add #49152");
    say_int(add(L"#49152"));
    say_error();
    line("add #49151");
    say_int(add(L"#49151"));
    line("add #12ab");
    const ATOM_T odd = add(L"#12ab");
    say_yes(odd >= 0xc000u);
    remove(odd);
    line("add empty");
    SetLastError(0);
    say_int(add(L""));
    say_error();
    static WCHAR longest[300];
    for (int i = 0; i < 255; ++i) {
        longest[i] = (WCHAR)('a' + i % 26);
    }
    longest[255] = 0;
    line("add 255 characters");
    const ATOM_T wide = add(longest);
    say_yes(wide >= 0xc000u);
    remove(wide);
    longest[255] = 'z';
    longest[256] = 0;
    line("add 256 characters");
    SetLastError(0);
    say_int(add(longest));
    say_error();
}

void __stdcall start(void) {
    say("memory.exe");
    heaps();
    moveable();
    say("\r\nlocal atoms:");
    atoms(AddAtomW, FindAtomW, DeleteAtom, GetAtomNameW, L"SxwFixtureAlpha", L"SXWFIXTUREALPHA",
          L"SxwFixtureBeta", 1);
    say("\r\nglobal atoms:");
    atoms(GlobalAddAtomW, GlobalFindAtomW, GlobalDeleteAtom, GlobalGetAtomNameW, L"SxwGlobalAlpha",
          L"SXWGLOBALALPHA", L"SxwGlobalBeta", 0);
    line("a local name is not global");
    const ATOM_T local = AddAtomW(L"SxwOnlyLocal");
    SetLastError(0);
    say_int(GlobalFindAtomW(L"SxwOnlyLocal"));
    say_error();
    DeleteAtom(local);
    say("\r\n");
    ExitProcess(0);
}
