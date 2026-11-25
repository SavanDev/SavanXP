/*
 * atom.c -- kernel32's atoms: the local table and the global one.
 *
 * An atom names a string, compared without case, with a reference count;
 * string atoms are 0xC000 and up. "#123" and MAKEINTATOM(123) are integer
 * atoms, 1 to 0xBFFF, which are their own number and are never stored. As
 * on Windows XP, a name is 1 to 255 characters, GetAtomName answers an
 * integer atom as "#123", and the first spelling added is the one kept.
 *
 * Windows' global table is the whole session's. SavanXP runs no other
 * Windows program beside this one to share it with (nothing uses DDE), so
 * here it is this process's second table.
 */
#define SXW_BUILDING_KERNEL32
#include <windows.h>

typedef unsigned int u32;
typedef WORD atom_t;

__declspec(dllimport) void *WINAPI SxwHeapAllocate(u32 size, u32 zero);
__declspec(dllimport) int WINAPI SxwHeapFree(void *pointer);

#define MAXIMUM_ATOMS 1024u
#define FIRST_STRING_ATOM 0xc000u
#define LONGEST_NAME 255u

typedef struct atom_entry {
    WCHAR *name; /* 0 when the slot is free */
    u32 references;
} atom_entry;

typedef struct atom_table {
    atom_entry entries[MAXIMUM_ATOMS];
} atom_table;

static atom_table g_local;
static atom_table g_global;

static WCHAR fold(WCHAR c) {
    if ((c >= 'a' && c <= 'z') || (c >= 0xe0u && c <= 0xfeu && c != 0xf7u)) {
        return (WCHAR)(c - 0x20u);
    }
    return c;
}

static int same_name(const WCHAR *a, const WCHAR *b) {
    while (*a != 0 && fold(*a) == fold(*b)) {
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

/* What a name is: 1 with *number for an integer atom ("#123" or
 * MAKEINTATOM), 0 for a string, -1 when it is neither (the error is set). */
static int integer_atom(LPCWSTR name, atom_t *number) {
    if ((ULONG_PTR)name < 0x10000u) {
        const u32 n = (u32)(ULONG_PTR)name;
        if (n == 0u || n >= FIRST_STRING_ATOM) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return -1;
        }
        *number = (atom_t)n;
        return 1;
    }
    if (name[0] != '#' || name[1] == 0) {
        return 0;
    }
    u32 n = 0;
    for (const WCHAR *c = name + 1; *c != 0; ++c) {
        if (*c < '0' || *c > '9') {
            return 0;
        }
        n = n * 10u + (u32)(*c - '0');
        if (n > 0xffffu) {
            n = 0x10000u;
        }
    }
    if (n == 0u || n >= FIRST_STRING_ATOM) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }
    *number = (atom_t)n;
    return 1;
}

/* A string name's length, or 0 when it is not a valid one (the error is
 * set). */
static u32 name_length(LPCWSTR name) {
    u32 n = 0;
    while (name[n] != 0) {
        if (++n > LONGEST_NAME) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return 0;
        }
    }
    if (n == 0u) {
        SetLastError(ERROR_INVALID_NAME);
    }
    return n;
}

static atom_entry *entry_of(atom_table *table, atom_t atom) {
    if (atom < FIRST_STRING_ATOM || atom - FIRST_STRING_ATOM >= MAXIMUM_ATOMS) {
        return 0;
    }
    atom_entry *entry = &table->entries[atom - FIRST_STRING_ATOM];
    return entry->name != 0 ? entry : 0;
}

static atom_t find(atom_table *table, LPCWSTR name) {
    for (u32 i = 0; i < MAXIMUM_ATOMS; ++i) {
        if (table->entries[i].name != 0 && same_name(table->entries[i].name, name)) {
            return (atom_t)(FIRST_STRING_ATOM + i);
        }
    }
    return 0;
}

static atom_t add_atom(atom_table *table, LPCWSTR name) {
    atom_t number;
    const int integer = integer_atom(name, &number);
    if (integer != 0) {
        return integer > 0 ? number : 0;
    }
    const u32 length = name_length(name);
    if (length == 0u) {
        return 0;
    }
    const atom_t existing = find(table, name);
    if (existing != 0) {
        ++table->entries[existing - FIRST_STRING_ATOM].references;
        return existing;
    }
    for (u32 i = 0; i < MAXIMUM_ATOMS; ++i) {
        atom_entry *entry = &table->entries[i];
        if (entry->name == 0) {
            entry->name = SxwHeapAllocate((length + 1u) * sizeof(WCHAR), 0);
            if (entry->name == 0) {
                SetLastError(ERROR_NOT_ENOUGH_MEMORY);
                return 0;
            }
            for (u32 k = 0; k <= length; ++k) {
                entry->name[k] = name[k];
            }
            entry->references = 1;
            return (atom_t)(FIRST_STRING_ATOM + i);
        }
    }
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return 0;
}

static atom_t find_atom(atom_table *table, LPCWSTR name) {
    atom_t number;
    const int integer = integer_atom(name, &number);
    if (integer != 0) {
        return integer > 0 ? number : 0;
    }
    if (name_length(name) == 0u) {
        return 0;
    }
    const atom_t atom = find(table, name);
    if (atom == 0) {
        SetLastError(ERROR_FILE_NOT_FOUND);
    }
    return atom;
}

/* 0 on success, as Windows answers; a string atom that is not there leaves
 * ERROR_INVALID_HANDLE and is answered 0 too, as Windows XP did. */
static atom_t delete_atom(atom_table *table, atom_t atom) {
    if (atom < FIRST_STRING_ATOM) {
        return 0;
    }
    atom_entry *entry = entry_of(table, atom);
    if (entry == 0) {
        SetLastError(ERROR_INVALID_HANDLE);
        return 0;
    }
    if (--entry->references == 0u) {
        SxwHeapFree(entry->name);
        entry->name = 0;
    }
    return 0;
}

/* The name into buffer[size], cut to fit with its terminator; the
 * characters written without it. */
static UINT atom_name(atom_table *table, atom_t atom, LPWSTR buffer, int size) {
    WCHAR integer[8];
    const WCHAR *name = 0;
    if (atom != 0 && atom < FIRST_STRING_ATOM) {
        u32 n = atom;
        u32 at = 7;
        integer[at] = 0;
        do {
            integer[--at] = (WCHAR)('0' + n % 10u);
            n /= 10u;
        } while (n != 0u);
        integer[--at] = '#';
        name = integer + at;
    } else {
        const atom_entry *entry = entry_of(table, atom);
        if (entry == 0) {
            SetLastError(ERROR_INVALID_HANDLE);
            return 0;
        }
        name = entry->name;
    }
    if (size <= 0 || buffer == NULL) {
        SetLastError(ERROR_MORE_DATA);
        return 0;
    }
    UINT n = 0;
    while (name[n] != 0 && n + 1u < (UINT)size) {
        buffer[n] = name[n];
        ++n;
    }
    buffer[n] = 0;
    return n;
}

ATOM WINAPI AddAtomW(LPCWSTR lpString) {
    return add_atom(&g_local, lpString);
}

ATOM WINAPI FindAtomW(LPCWSTR lpString) {
    return find_atom(&g_local, lpString);
}

ATOM WINAPI DeleteAtom(ATOM nAtom) {
    return delete_atom(&g_local, nAtom);
}

UINT WINAPI GetAtomNameW(ATOM nAtom, LPWSTR lpBuffer, int nSize) {
    return atom_name(&g_local, nAtom, lpBuffer, nSize);
}

ATOM WINAPI GlobalAddAtomW(LPCWSTR lpString) {
    return add_atom(&g_global, lpString);
}

ATOM WINAPI GlobalFindAtomW(LPCWSTR lpString) {
    return find_atom(&g_global, lpString);
}

ATOM WINAPI GlobalDeleteAtom(ATOM nAtom) {
    return delete_atom(&g_global, nAtom);
}

UINT WINAPI GlobalGetAtomNameW(ATOM nAtom, LPWSTR lpBuffer, int nSize) {
    return atom_name(&g_global, nAtom, lpBuffer, nSize);
}
