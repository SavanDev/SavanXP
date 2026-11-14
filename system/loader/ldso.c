/* The shared-library loader. See <savanxp/ldso.h> for what a program sees of it
 * and <loader/loader.h> for what a subsystem builds its run-time loading on.
 * All the work is pointer arithmetic over an image already mapped, plus
 * copying the writable segments.
 *
 * It is platform code, not a subsystem's (docs/kernel/SHARED_LIBRARIES.md, "One
 * loader over SXN"): it opens, reads and maps files with the native object
 * calls, which every subsystem reaches, and calls no C library, so the same
 * file serves a SUN program and a POSIX one. What it needs from the program's
 * runtime is two things: sx_image_base, which crt0 stores, and
 * sx_loader_report, through which each runtime says in its own terms why a
 * library did not load. It never ends the process; it returns a step and the
 * program goes on. tools/check_shared_libs.py fails the build if a compiled
 * copy imports anything else. */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <savanxp/ldso.h>

#include "loader/loader.h"
#include "sxn/sxn.h"

/* From the runtime (savanxp/libc.h). Declared here rather than by including
 * the C library's header, so a call into it cannot slip back in unnoticed. */
extern unsigned long sx_image_base;
void sx_loader_report(const char* text, unsigned long length);

typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef int32_t Elf64_Sword;
typedef uint64_t Elf64_Xword;

#define EI_NIDENT 16
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define ET_DYN 3
#define ET_EXEC 2
#define EM_X86_64 62

#define PT_LOAD 1
#define PT_DYNAMIC 2

#define PF_X 1
#define PF_W 2
#define PF_R 4

#define DT_NULL 0
/* Reserved section index: marks a symbol declared but not defined here. */
#define SHN_UNDEF 0
#define DT_NEEDED 1
#define DT_PLTRELSZ 2
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_STRTAB 5
#define DT_STRSZ 10
#define DT_SYMTAB 6
#define DT_JMPREL 23
#define DT_PLTREL 20
/* The two hash tables are read only for the symbol count they imply; lookup
 * itself stays a linear scan (see "Symbol lookup was tried and rejected"). */
#define DT_HASH 4
#define DT_GNU_HASH 0x6ffffef5
/* Constructors. Only sx_loader_open runs them; see run_initializers. */
#define DT_INIT 12
#define DT_INIT_ARRAY 25
#define DT_INIT_ARRAYSZ 27

#define STT_OBJECT 1
#define STT_FUNC 2

#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_GLOB_DAT 6
/* The stored value is already a link-time address, with no symbol: the image's
 * bias has to be added. It is what lets a PIE executable with pointers to data
 * -- a table of vtables, a constant string, an initial GOT -- start with the
 * addresses where the code looks for them. An image relocated without it has
 * pointers to the value zero. */
#define R_X86_64_RELATIVE 8
/* The absolute 64-bit address of an IMPORTED symbol, stored in data. It is the
 * same as a GLOB_DAT -- resolve by name and write the value -- and appears when
 * code takes &symbol and keeps it in a table. */
#define R_X86_64_64 1

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    Elf64_Half e_type;
    Elf64_Half e_machine;
    Elf64_Word e_version;
    Elf64_Addr e_entry;
    Elf64_Off e_phoff;
    Elf64_Off e_shoff;
    Elf64_Word e_flags;
    Elf64_Half e_ehsize;
    Elf64_Half e_phentsize;
    Elf64_Half e_phnum;
    Elf64_Half e_shentsize;
    Elf64_Half e_shnum;
    Elf64_Half e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    Elf64_Word p_type;
    Elf64_Word p_flags;
    Elf64_Off p_offset;
    Elf64_Addr p_vaddr;
    Elf64_Addr p_paddr;
    Elf64_Xword p_filesz;
    Elf64_Xword p_memsz;
    Elf64_Xword p_align;
} Elf64_Phdr;

typedef struct {
    Elf64_Word st_name;
    unsigned char st_info;
    unsigned char st_other;
    Elf64_Half st_shndx;
    Elf64_Addr st_value;
    Elf64_Xword st_size;
} Elf64_Sym;

typedef struct {
    Elf64_Addr r_offset;
    Elf64_Xword r_info;
    Elf64_Sword r_addend;
} Elf64_Rela;

/* How many program headers an image may have, and so how many fit in the
 * struct below. It is the ceiling of the slot: each library keeps its headers
 * in an array of this size, so raising kMaxLibraries without lowering this
 * multiplies every process's memory for nothing.
 *
 * Measured over everything this tree produces, the largest e_phnum is 15.
 * Twenty-four leaves room for one more segment without turning a rare refusal
 * into a normal one. And the limit is checked BEFORE copying, so an image with
 * more headers is refused with a reason instead of silently overflowing the
 * array.
 *
 * Bringing it down from 64 is what makes the slot count below cheap: a slot
 * goes from 4300 B to about 1750, and thirty-two slots of 137 KB become 56 KB. */
#define kMaxProgramHeaders 24

/* The state of ONE loaded image.
 *
 * There used to be a single library, which is why this was one global object
 * with a name. A DT_NEEDED chain outgrows that: loading a dependency overwrites
 * the state of the one being loaded, so there has to be a place per library and
 * a pointer to the one in progress. */
typedef struct {
    int loaded;
    /* The file's handle and its whole-file section, or -1. They live only
     * while the image is being loaded; 0 is a handle like any other -- a
     * native process may have no standard input, so its first open gets 0. */
    long file_section;
    long file_fd;
    /* Whether the two above are still open, so closing does not repeat. */
    int handles_open;
    /* Load bias. An ET_DYN has p_vaddr near zero, and zero is not a valid user
     * address: the kernel maps nothing below kUserBase. So the first segment --
     * the one with p_vaddr 0 -- is asked for without a base so the kernel picks
     * a good one, and that address minus its p_vaddr is the bias everything
     * else goes against. */
    unsigned long bias;
    int bias_ready;
    Elf64_Ehdr header;
    Elf64_Half header_count;
    const unsigned char* dynsym;
    const char* dynstr;
    size_t dynstr_size;
    /* How many DT_NEEDED have been brought in already. It is the chain's
     * cursor: with a diamond, D is loaded from A and from C, and the second
     * next_needed has to skip the one already processed instead of loading it
     * again. */
    unsigned needed_done;
    /* The name it was loaded under, to recognise it if another DT_NEEDED asks
     * for it again in the same chain. It is the basename: DT_NEEDED carries
     * "libmath.so.0.4" even though the path is /lib/libmath.so.0.4. */
    char soname[96];
    /* The program headers are read once and kept here: the file is not read
     * again after that. */
    Elf64_Phdr headers[kMaxProgramHeaders];
    /* The final page address of each segment, in program-header order; 0 is
     * "this segment is not mapped". */
    void* placed[kMaxProgramHeaders];
    /* Whether this image answers lookups from images outside its own dependency
     * tree. Everything loaded at startup or through ldso_load is global, which is
     * what the loader always did; sx_loader_open without `global` is not. */
    int global;
    /* References from sx_loader_open and _retain not yet dropped by _release.
     * Images brought in at startup or as a dependency stay at zero: nothing can
     * close them. */
    unsigned refcount;
    /* The slots of this image's direct DT_NEEDED, one bit each. Its lookup scope
     * is itself plus the closure of these. */
    uint64_t deps;
    /* Entries in .dynsym, from DT_HASH or DT_GNU_HASH, or 0 when the image has
     * neither and the end of the table has to be guessed. */
    unsigned long symbol_count;
    /* The path sx_loader_open was given or DT_NEEDED resolved to, so a second
     * open of the same file finds this image, and sx_loader_address has a name
     * to report. */
    char path[128];
} Library;

/* How many images live together. The executable takes slot 0.
 *
 * The longest realistic chain in the tree is the media player's: the
 * executable, its two interface libraries and, once FFmpeg stopped being
 * static, libavcodec + libavutil + the scaling and resampling ones. That is
 * seven, and with eight we were one slot from the wall -- a wall the FFmpeg
 * integration would have found, not us. Thirty-two leaves room for a second
 * codec; beyond that nothing justifies the number.
 *
 * Raising it is cheap only because the slot got cheaper: at 4300 B a slot,
 * thirty-two would cost 137 KB resident in every process, an ls that loads
 * nothing included. With headers[] of 24 a slot is ~1750 B and the total drops
 * to 56 KB. The two changes go together for that reason. */
#ifndef SAVANXP_LD_MAX_LIBRARIES
#define SAVANXP_LD_MAX_LIBRARIES 32
#endif
#define kMaxLibraries SAVANXP_LD_MAX_LIBRARIES
/* Library.deps is a 64-bit mask indexed by slot. */
_Static_assert(kMaxLibraries <= 64, "Library.deps holds one bit per slot");

static Library g_libs[kMaxLibraries];
static int g_lib_count;
/* What a new image gets as Library.global. sx_loader_open lowers it for the
 * length of one call; everything else loads global. */
static int g_load_global = 1;
/* Set while sx_loader_open runs. An unresolvable symbol then fails the load
 * instead of being skipped: the tolerant mode exists so a program can start and
 * report a missing dependency, and a run-time caller gets an error for that. */
static int g_strict = 0;
/* The dependency whose DT_NEEDED could not be brought in. See ldso_missing(). */
static const char* g_missing_library = 0;
/* The DISTINCT dependencies that could not be brought in, by name.
 *
 * Distinct, and not a count of attempts: removing libsxgfx.so.0.4 fails the
 * load of libffmpeg, of libgfx2d and of libsxgui, because all three need it, and
 * each one retries. With a count of attempts the player came to say "6 libraries
 * missing" when two were, and a number that lies is worse than none. The first
 * one's name is kept apart because it is the one shown. */
static char g_missing_names[kMaxLibraries][64];
static unsigned g_missing_count = 0;
/* The executable takes slot 0 and libraries start at 1.
 *
 * resolve walks the slots from newest to oldest, so putting the executable
 * first consults it LAST. That order is the symbol scope's: a library wins over
 * the one that loaded it, and the executable is the background, which is where a
 * symbol the program exports is meant to land.
 *
 * The library being loaded. Everything written while an image is placed goes
 * here; the rest of the tree reads it by index. */
static Library* g_lib;

/* The last segment that could not be placed, and the error it returned.
 * ldso_load's return code only says "the step"; these two say which. */
int g_lib_fail_index;
long g_lib_fail_errno;
unsigned long g_lib_bias;
int g_lib_reloc_step;
/* Which symbol of which library failed. The step says where; this says what.
 * The pointer points into the library's .dynstr, which stays mapped, so there
 * is no copy to make and none to go stale. */
const char* g_lib_fail_symbol;
const char* g_lib_fail_library;

/* The dependency that could not be loaded, by name, or 0 if none failed.
 *
 * It exists so a program can SAY what it lacks instead of dying. The loader is
 * not fatal: sx_start_dynamic reports the failure and returns, and crt0 calls
 * main anyway, so a program with an unresolvable DT_NEEDED starts. What it could
 * not do was find out: ldso_loaded() returns g_lib_count, which is at least 1
 * because the executable takes slot 0, so it answered "yes" even with a
 * dependency missing. That was an accident, not an interface.
 *
 * Mind what it means: "this dependency could not be loaded", not "it is
 * missing". A file that is present and broken leaves its name here too, and
 * saying it is missing would be a lie. The precise reason is in the message the
 * loader already reported, with its step and its reason.
 *
 * It is never cleared. A failure at startup is permanent, and the program
 * reports it when it opens its window, not before. */
const char* ldso_missing(void) {
    return g_missing_library;
}

unsigned ldso_missing_count(void) {
    return g_missing_count;
}

/* How many images are mapped, the executable included. It exists for the
 * tests: without it, the only way to check that a shared dependency was loaded
 * ONCE -- the diamond case -- would be to count files.
 *
 * The executable counts because it takes a slot, and because the limit includes
 * its slot: with kMaxLibraries 32 there are 31 slots for libraries. */
int ldso_count(void) {
    return g_lib_count;
}

int ldso_loaded(void) {
    return g_lib_count;
}

/* The order of this file is the order of the reasoning -- validate, locate,
 * place, relocate -- and not the order of the dependencies between functions. */
static void copy_bytes(const void* from, void* to, size_t count);
static int read_header(const unsigned char* bytes, Elf64_Ehdr* out);
static void* place_segment(const Elf64_Phdr* ph);
static void* map_of(int slot, Elf64_Addr vaddr);
static int read_dynamic_in(int slot);
static int find_dynamic_in(int slot, Elf64_Sword wanted, void* out);
static void* bias_in(int slot, Elf64_Addr value);
static int resolve(const char* name, Elf64_Addr* value_out);
static int resolve_in(const char* name, int slot, Elf64_Addr* value_out);
static int resolve_for(int slot, const char* name, Elf64_Addr* value_out);
static void count_symbols_in(int slot);
static const char* next_needed(int slot);
static int load_needed_chain(int parent_slot);

static int read_header(const unsigned char* bytes, Elf64_Ehdr* out) {
    if (bytes[0] != 0x7f || bytes[1] != 'E' || bytes[2] != 'L' || bytes[3] != 'F') {
        return 0;
    }
    copy_bytes(bytes, out, sizeof(*out));
    if (out->e_type != ET_DYN || out->e_machine != EM_X86_64) {
        return 0;
    }
    if (out->e_phentsize != sizeof(Elf64_Phdr) || out->e_phnum == 0 ||
        out->e_phnum > kMaxProgramHeaders) {
        return 0;
    }
    return 1;
}

/* no_builtin: the compiler recognises this loop as memcpy and would call the C
 * library's, which the loader does not use. */
__attribute__((no_builtin("memcpy"))) static void copy_bytes(const void* from, void* to, size_t count) {
    const unsigned char* src = (const unsigned char*)from;
    unsigned char* dst = (unsigned char*)to;
    for (size_t index = 0; index < count; ++index) {
        dst[index] = src[index];
    }
}

/* strcmp's answer to "are these equal", the only question the loader asks. */
static int same_string(const char* a, const char* b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* Appends text to a NUL-terminated buffer of `size` bytes, cutting it short. */
static void append_text(char* buffer, unsigned long size, unsigned long* used, const char* text) {
    if (buffer == 0 || size == 0) {
        return;
    }
    while (text != 0 && *text != '\0' && *used + 1 < size) {
        buffer[(*used)++] = *text++;
    }
    buffer[*used] = '\0';
}

/* One diagnostic line, handed to the runtime's sx_loader_report. The format
 * knows what the messages use and nothing else: %s, %d, %u and %lx. A line
 * longer than the buffer is cut, and keeps its line break. */
static void report(const char* format, ...) {
    char line[256];
    const size_t room = sizeof(line) - 1;
    size_t used = 0;
    va_list args;
    va_start(args, format);
    for (const char* ch = format; *ch != '\0' && used < room; ++ch) {
        if (*ch != '%') {
            line[used++] = *ch;
            continue;
        }
        ++ch;
        char digits[20];
        int count = 0;
        if (*ch == 's') {
            const char* text = va_arg(args, const char*);
            if (text == 0) {
                text = "(null)";
            }
            while (*text != '\0' && used < room) {
                line[used++] = *text++;
            }
            continue;
        }
        if (*ch == 'd' || *ch == 'u') {
            unsigned long value;
            if (*ch == 'd') {
                const int signed_value = va_arg(args, int);
                if (signed_value < 0 && used < room) {
                    line[used++] = '-';
                }
                value = signed_value < 0 ? (unsigned long)(-(long)signed_value) : (unsigned long)signed_value;
            } else {
                value = va_arg(args, unsigned int);
            }
            do {
                digits[count++] = (char)('0' + value % 10);
                value /= 10;
            } while (value != 0);
        } else if (ch[0] == 'l' && ch[1] == 'x') {
            ++ch;
            unsigned long value = va_arg(args, unsigned long);
            do {
                digits[count++] = "0123456789abcdef"[value & 0xf];
                value >>= 4;
            } while (value != 0);
        } else {
            /* Not a conversion this formatter knows: say so in the line rather
             * than read an argument of the wrong size. */
            if (*ch == '\0') {
                break;
            }
            digits[count++] = '?';
        }
        while (count > 0 && used < room) {
            line[used++] = digits[--count];
        }
    }
    va_end(args);
    if (used == room && line[used - 1] != '\n') {
        line[used - 1] = '\n';
    }
    sx_loader_report(line, (unsigned long)used);
}

/* Registers the executable in g_libs so its symbols enter the scope.
 *
 * The loader did not map this image: the kernel did. So there is no handle and
 * no section to open, and the base has to come from the kernel. It arrives in
 * r8 and crt0 stores it in sx_image_base.
 *
 * It used to be GUESSED. The address of a function of this very file -- by
 * definition inside the image -- was taken, and the walk went back a page at a
 * time looking for the ELF magic. The problem is not that the arithmetic is
 * wrong: it is that it assumes EVERY page on the way is mapped, and a linker
 * can leave a gap between two PT_LOADs. seltest has one: its first segment ends
 * at 0xaff3 and its text starts at 0xc000, so page 0xb000 does not exist. The
 * scan read it and the process died with cr2 = 0x40b000 before reaching main.
 *
 * A scan that can fault is not a scan. With the kernel's figure there is no
 * walk, and no way for the arithmetic to fail.
 *
 * Returns 0 when there is no valid header, and the scope stays as it was:
 * resolving among libraries only. An executable without .dynsym (a non-PIE
 * ET_EXEC) is a normal case, not a failure. */
static int adopt_executable(void) {
    if (g_lib_count >= kMaxLibraries) {
        return 0;
    }
    const unsigned long cursor = sx_image_base & ~4095UL;
    if (cursor == 0) {
        /* Without a base there is nothing to register. Better to say so than to
         * read addresses that may not exist. */
        return 0;
    }

    Elf64_Ehdr header;
    const unsigned char* bytes = (const unsigned char*)cursor;
    if (bytes[0] != 0x7f || bytes[1] != 'E' || bytes[2] != 'L' || bytes[3] != 'F') {
        return 0;
    }
    copy_bytes(bytes, &header, sizeof(header));
    /* ET_DYN or ET_EXEC: anything else is not a program image. And e_phnum has
     * to fit, because the segments used to translate addresses come from it. */
    if ((header.e_type != ET_DYN && header.e_type != ET_EXEC) ||
        header.e_machine != EM_X86_64 || header.e_phentsize != sizeof(Elf64_Phdr) ||
        header.e_phnum == 0 || header.e_phnum > kMaxProgramHeaders) {
        return 0;
    }

    Library* exe = &g_libs[g_lib_count];
    exe->soname[0] = '\0';
    exe->path[0] = '\0';
    exe->global = 1;
    exe->refcount = 0;
    exe->deps = 0;
    exe->symbol_count = 0;
    copy_bytes(&header, &exe->header, sizeof(header));
    exe->header_count = header.e_phnum;
    exe->dynsym = 0;
    exe->dynstr = 0;
    exe->loaded = 0;
    exe->bias_ready = 0;
    exe->needed_done = 0;
    for (Elf64_Half index = 0; index < header.e_phnum; ++index) {
        copy_bytes((const unsigned char*)cursor + header.e_phoff + (index * header.e_phentsize),
                   &exe->headers[index], sizeof(Elf64_Phdr));
        exe->placed[index] = 0;
    }
    /* The segments are mapped already: the kernel put them there, and the
     * loader has neither handle nor section to map them again with. placed[] is
     * derived from the image's geometry.
     *
     * The bias comes from the base the kernel gave and the first PT_LOAD: what
     * lies between where the header landed and where the image says its first
     * segment starts. For an ET_DYN, which loads at kUserBase, it is kUserBase;
     * for an ET_EXEC, which loads at its own link addresses, it is zero.
     *
     * It used to be written as a constant -- kUserBase for an ET_DYN -- with a
     * comment saying it had to match kernel/elf.cpp. It matched, which is why
     * the bug did not show. With both numbers in view the arithmetic is
     * explicit and cannot drift on its own. */
    Elf64_Addr first_vaddr = 0;
    int found_first = 0;
    for (Elf64_Half index = 0; index < header.e_phnum; ++index) {
        if (exe->headers[index].p_type != PT_LOAD || exe->headers[index].p_memsz == 0) {
            continue;
        }
        first_vaddr = exe->headers[index].p_vaddr;
        found_first = 1;
        break;
    }
    if (!found_first) {
        return 0;
    }
    const unsigned long bias = cursor - (unsigned long)(first_vaddr & ~(Elf64_Addr)4095);
    exe->bias = bias;
    exe->bias_ready = 1;
    for (Elf64_Half index = 0; index < header.e_phnum; ++index) {
        if (exe->headers[index].p_type != PT_LOAD || exe->headers[index].p_memsz == 0) {
            continue;
        }
        exe->placed[index] = (void*)(bias + (exe->headers[index].p_vaddr & ~(Elf64_Addr)4095));
    }
    g_lib_count++;
    (void)read_dynamic_in(g_lib_count - 1);
    return 1;
}

/* The executable has slot 0 from startup and libraries start at 1, so that
 * resolve consults it last. */
static void adopt_executable_once(void) {
    static int done = 0;
    if (done) {
        return;
    }
    done = 1;
    (void)adopt_executable();
}

/* Reserves a slot and makes it the library in progress. The slot is not
 * cleared: each load writes every field it uses, and clearing it would be
 * another pass over the same thing. */
static Library* begin_load(void) {
    adopt_executable_once();
    if (g_lib_count >= kMaxLibraries) {
        return 0;
    }
    g_lib = &g_libs[g_lib_count];
    g_lib->loaded = 0;
    g_lib->bias_ready = 0;
    g_lib->dynsym = 0;
    g_lib->dynstr = 0;
    g_lib->needed_done = 0;
    g_lib->soname[0] = '\0';
    g_lib->path[0] = '\0';
    g_lib->global = g_load_global;
    g_lib->refcount = 0;
    g_lib->deps = 0;
    g_lib->symbol_count = 0;
    /* placed[] is what a failed load unmaps, so it must not hold the views of
     * whatever image had this slot before a rollback freed it. */
    g_lib->header_count = 0;
    for (int index = 0; index < kMaxProgramHeaders; ++index) {
        g_lib->placed[index] = 0;
    }
    /* The file's handles are reset too. This slot may have held a library
     * loaded earlier -- loading one takes the slot and leaves it -- so a failed
     * open left that library's handles here, and closing our own would have
     * closed those. */
    g_lib->file_section = -1;
    g_lib->file_fd = -1;
    g_lib->handles_open = 0;
    return g_lib;
}

/* Declared before end_load, which uses it: end_load lets go of the pointer to
 * the library in progress, and without the pointer nothing can be closed. */
static void close_file_handles(void);

/* Unmaps every segment an image placed. For a load that failed before it was
 * published, and for sx_loader_open's rollback of one that failed after. */
static void unmap_placed(Library* library) {
    for (int index = 0; index < kMaxProgramHeaders; ++index) {
        if (library->placed[index] != 0) {
            (void)sxn_unmap_view((uint64_t)(uintptr_t)library->placed[index]);
            library->placed[index] = 0;
        }
    }
}

static void end_load(int ok) {
    if (ok) {
        g_lib->loaded = 1;
        g_lib_count++;
    } else {
        /* A load that failed halfway left handles open, and from step -6 on,
         * segments mapped. Releasing them here and not in the caller is what
         * releases them on every error path. */
        close_file_handles();
        unmap_placed(g_lib);
    }
    g_lib = 0;
}

/* Makes the library in progress visible without letting go of the pointer to
 * it.
 *
 * end_load does not fit the place where this is needed: relocating a library
 * depends on its DT_NEEDED being loaded already, so it has to be published
 * before the chain is walked. Publishing and still keeping the pointer is what
 * lets the relocations, back from the chain, keep applying to this library and
 * not to the last dependency brought in. */
static void publish(void) {
    g_lib->loaded = 1;
    g_lib_count++;
}

/* The three reasons a relocation with a symbol can fail, in words.
 *
 * The function that calls these returns zero, and whoever sees it only has a
 * step number. The number says WHERE, not WHAT: "step 5" is every relocation in
 * the table, with the whole symbol table ahead. A step number saves work; a name
 * saves the afternoon.
 *
 * They are reported from here and not from above because the name is here, and
 * the caller has already let it go: its return is an int. */
static void report_unresolved(const char* library, const char* symbol)
{
    /* Kept as well as reported. The report is what helps a person; the global
     * is what lets a TEST assert the name, since a test cannot read standard
     * error. */
    g_lib_fail_library = library;
    g_lib_fail_symbol = symbol;
    report("loader: %s needs \"%s\" and no loaded image defines it\n", library, symbol);
}

static void report_unplaced(const char* symbol, int owner)
{
    report("loader: \"%s\" was found in %s, but its address falls in no mapped segment\n",
           symbol, g_libs[owner].soname);
}

static void report_unmapped(const char* library, const char* symbol, Elf64_Addr offset)
{
    report("loader: %s writes at 0x%lx for \"%s\", which is in no mapped segment of the image\n",
           library, (unsigned long)offset, symbol);
}

/* What an image is called in the messages.
 *
 * The executable has no DT_SONAME -- that field is for shared objects only -- so
 * its name is empty and the message would read "39 entries of  were left". The
 * image's name is the first thing read in a diagnostic, and a blank one makes
 * it useless. */
static const char* image_name(int slot) {
    return g_libs[slot].soname[0] != '\0' ? g_libs[slot].soname : "the executable";
}

/* Applies one RELA table: .rela.dyn or .rela.plt. Both are needed before
 * anything is called, so there is no lazy version; JUMP_SLOT, GLOB_DAT,
 * R_X86_64_64 and RELATIVE are what lld emits for this target. */
static int apply_table_in(int slot, unsigned long elf_table, unsigned long elf_size) {
    /* With a dependency missing, the image is relocated ANYWAY, and WHOLE.
     *
     * Loading used to stop at the first symbol that could not be resolved, and
     * since the chain is walked BEFORE relocating, one missing DT_NEEDED left
     * the whole executable unrelocated: main() ran with stdout -- a global
     * pointer relocated by R_X86_64_RELATIVE -- pointing at its link value. The
     * program could not even print why it was broken, which is exactly what a
     * diagnosable failure has to be able to do.
     *
     * So in tolerant mode the unresolvable entries are skipped and counted.
     * ldso_missing() already gives the name of what is missing, and repeating
     * it once for every av_* in the program would be noise. */
    const int tolerant = !g_strict && g_missing_library != 0;
    unsigned int skipped = 0;
    const unsigned char* table = (const unsigned char*)map_of(slot, elf_table);
    if (table == 0) {
        return 0;
    }
    for (unsigned long offset = 0; offset + sizeof(Elf64_Rela) <= elf_size;
         offset += sizeof(Elf64_Rela)) {
        Elf64_Rela rela;
        copy_bytes(table + offset, &rela, sizeof(rela));
        const unsigned type = (unsigned)(rela.r_info & 0xffffffffu);
        if (type == R_X86_64_RELATIVE) {
            /* No symbol to look up. What has to be written is r_addend + bias,
             * NOT the bias added to what is already in memory.
             *
             * The difference is not academic and cost a whole bug: lld leaves
             * the slot AT ZERO and puts the link value in r_addend. Adding the
             * bias to a zero gives base + 0, a pointer to the image's own base.
             * With `stdout` that gave 0x400000, and the first write to the
             * screen died with cr2 = 0x40000c.
             *
             * Reading memory instead of the addend only works with links that
             * also write the value, and SavanXP links with lld, which does not. */
            Elf64_Addr* where = (Elf64_Addr*)map_of(slot, rela.r_offset);
            if (where == 0) {
                return 0;
            }
            *where = (Elf64_Addr)((unsigned long)(Elf64_Xword)rela.r_addend + g_libs[slot].bias);
            continue;
        }
        if (type != R_X86_64_JUMP_SLOT && type != R_X86_64_GLOB_DAT &&
            type != R_X86_64_64) {
            /* The loader does not implement this type. Failing is right --
             * going on and leaving the slot as it was would be a broken
             * reference that shows only at the call -- but without naming the
             * type, "step 5" leads nowhere.
             *
             * This linker cannot produce it: lld emits only these for a
             * well-built shared object. It stays for one linked by hand. */
            report("loader: %s has a relocation of type %u; only JUMP_SLOT, GLOB_DAT, "
                   "R_X86_64_64 and RELATIVE are implemented\n",
                   image_name(slot), type);
            return 0;
        }
        const unsigned name_index = (unsigned)(rela.r_info >> 32);
        Elf64_Sym symbol;
        copy_bytes(g_libs[slot].dynsym + (name_index * sizeof(Elf64_Sym)), &symbol, sizeof(symbol));

        /* By name, not by st_value: an st_value is relative to the image that
         * DEFINED the symbol, which may be another one. And within this image's
         * scope, so a library opened without `global` cannot capture the calls
         * of one loaded after it. */
        Elf64_Addr target = 0;
        const int owner = resolve_for(slot, g_libs[slot].dynstr + symbol.st_name, &target);
        if (owner < 0) {
            if (tolerant) {
                skipped++;
                continue;
            }
            /* The name is all there is. Without it, the symptom is "step 5" and
             * the only way to get anything out is a dump of .dynsym. */
            report_unresolved(image_name(slot), g_libs[slot].dynstr + symbol.st_name);
            return 0;
        }
        const Elf64_Addr value = (Elf64_Addr)(unsigned long)bias_in(owner, target);
        if (value == 0) {
            /* The image was found but the address cannot be translated: an
             * st_value in a segment that was not mapped. Different from the case
             * above because the name DOES exist, so the message has to say
             * both things. */
            if (tolerant) {
                skipped++;
                continue;
            }
            report_unplaced(g_libs[slot].dynstr + symbol.st_name, owner);
            return 0;
        }
        Elf64_Addr* where = (Elf64_Addr*)map_of(slot, rela.r_offset);
        if (where == 0) {
            /* This entry's GOT slot is in no mapped segment. A fault of the
             * image, not of the linking. */
            if (tolerant) {
                skipped++;
                continue;
            }
            report_unmapped(image_name(slot), g_libs[slot].dynstr + symbol.st_name, rela.r_offset);
            return 0;
        }
        *where = value;
    }
    if (skipped != 0) {
        /* One line, not one per symbol: the load's message already named the
         * library, and what this adds is the COUNT. */
        report("loader: %u entries of %s were left unrelocated because %s did not load\n",
               skipped, image_name(slot), g_missing_library);
    }
    return 1;
}

/* Phase one: find where the symbol table and the string table are.
 *
 * It is kept apart from relocation because reading DT_NEEDED needs dynstr as
 * well, and DT_NEEDED has to be walked before relocating: a call into the
 * dependency resolves into a GOT that is still zero. Relocating first would
 * resolve the chain against an incomplete world. */
static int read_dynamic_in(int slot) {
    Library* library = &g_libs[slot];
    unsigned long strtab = 0;
    unsigned long symtab = 0;
    unsigned long strsz = 0;
    if (!find_dynamic_in(slot, DT_STRTAB, &strtab)) { g_lib_reloc_step = 1; return 0; }
    if (!find_dynamic_in(slot, DT_SYMTAB, &symtab)) { g_lib_reloc_step = 2; return 0; }
    (void)find_dynamic_in(slot, DT_STRSZ, &strsz);
    const unsigned char* strings = (const unsigned char*)map_of(slot, strtab);
    const unsigned char* symbols = (const unsigned char*)map_of(slot, symtab);
    if (strings == 0) { g_lib_reloc_step = 3; return 0; }
    if (symbols == 0) { g_lib_reloc_step = 4; return 0; }
    library->dynstr = (const char*)strings;
    library->dynstr_size = strsz != 0 ? strsz : 4096;
    library->dynsym = symbols;
    library->loaded = 1;
    count_symbols_in(slot);
    return 1;
}

/* How many entries .dynsym has. ELF does not say it directly; the hash table
 * does. Without the count the scan in resolve_in has to stop at the first entry
 * whose name offset falls outside .dynstr, which works because what follows the
 * table usually looks like that -- a lookup that misses (and Xash probes for
 * optional exports on every load) walks into whatever comes next.
 *
 * DT_HASH holds the count as its second word. DT_GNU_HASH does not: the highest
 * symbol index any bucket starts at, followed along its chain to the entry with
 * the end bit set, is the last one. Symbols below symoffset are not hashed but
 * still count. */
static void count_symbols_in(int slot) {
    Library* library = &g_libs[slot];
    library->symbol_count = 0;
    unsigned long table = 0;
    if (find_dynamic_in(slot, DT_HASH, &table)) {
        const uint32_t* words = (const uint32_t*)map_of(slot, table);
        if (words != 0) {
            library->symbol_count = words[1];
        }
        return;
    }
    if (!find_dynamic_in(slot, DT_GNU_HASH, &table)) {
        return;
    }
    const uint32_t* words = (const uint32_t*)map_of(slot, table);
    if (words == 0) {
        return;
    }
    const uint32_t bucket_count = words[0];
    const uint32_t symoffset = words[1];
    const uint32_t bloom_words = words[2];
    /* The bloom filter is ELFCLASS-sized words: 64-bit here. */
    const uint32_t* buckets = words + 4 + (bloom_words * 2);
    const uint32_t* chain = buckets + bucket_count;
    uint32_t last = 0;
    for (uint32_t index = 0; index < bucket_count; ++index) {
        if (buckets[index] > last) {
            last = buckets[index];
        }
    }
    if (last < symoffset) {
        library->symbol_count = symoffset;
        return;
    }
    while ((chain[last - symoffset] & 1u) == 0) {
        last++;
    }
    library->symbol_count = (unsigned long)last + 1;
}

/* Both tables are OPTIONAL. A library that calls nothing outside has no
 * .rela.plt, and requiring it made such a library fail to load: the failure was
 * not the relocation's but a table that does not exist. */
static int apply_relocs_in(int slot) {
    unsigned long jmprel = 0;
    unsigned long pltrelsz = 0;
    unsigned long rel = 0;
    unsigned long relasz = 0;
    if (find_dynamic_in(slot, DT_RELA, &rel) && find_dynamic_in(slot, DT_RELASZ, &relasz) &&
        relasz != 0) {
        if (!apply_table_in(slot, rel, relasz)) { g_lib_reloc_step = 5; return 0; }
    }
    if (find_dynamic_in(slot, DT_JMPREL, &jmprel) &&
        find_dynamic_in(slot, DT_PLTRELSZ, &pltrelsz) && pltrelsz != 0) {
        if (!apply_table_in(slot, jmprel, pltrelsz)) { g_lib_reloc_step = 6; return 0; }
    }
    return 1;
}

/* Gets the executable ready to call into a library.
 *
 * The kernel mapped the main image but did NOT touch its GOT: it leaves the
 * process running with an executable that declares DT_NEEDED and has entries
 * left empty. That is why this step exists, and why it has to run BEFORE any
 * library symbol is used.
 *
 * The order is the same as for a library: first the executable's DT_NEEDED
 * chain, then its own relocations. The other way round, a call into a
 * dependency would resolve against a library not mapped yet.
 *
 * Returns 0 if the executable is ready, or a negative number saying which step
 * failed, with the same convention as ldso_load. An executable without .dynsym
 * has nothing to relocate and returns 0. */
int ldso_start(void) {
    /* Idempotent on purpose: crt0 calls it before main and a program may call
     * it again. Reapplying the executable's relocations would write the same
     * values, but R_X86_64_RELATIVE ADDS the bias, and doing it twice would
     * leave the image's pointers shifted by kUserBase. */
    static int done = 0;
    if (done) {
        return 0;
    }
    done = 1;
    adopt_executable_once();
    if (g_lib_count == 0) {
        /* Without the image there is nothing to relocate, but an ET_DYN image
         * DOES need relocating. Returning 0 here would be a lie: the program
         * starts with pointers missing their bias and fails later, somewhere
         * that says nothing of this. A program that really depends on nothing
         * does not get here, because it does not link the loader. */
        return -10;
    }
    if (g_libs[0].dynsym == 0) {
        return -11;
    }
    /* The chain is walked before relocating because a GOT that points into a
     * dependency has to be filled when that dependency is already mapped. But
     * if the chain fails, relocation HAPPENS ANYWAY and the failure is returned
     * afterwards.
     *
     * Returning here would not leave the image as it was: it would leave the
     * whole executable unrelocated, with stdout and the other global pointers
     * at their link values. The program starts and cannot even print the
     * reason. A failure being non-fatal does not mean the image may be left
     * half done. */
    const int chain_failed = load_needed_chain(0) != 0;
    const int relocated = apply_relocs_in(0);
    if (chain_failed) {
        return -8;
    }
    if (!relocated) {
        return -9;
    }
    return 0;
}

/* The hook crt0 calls before main.
 *
 * The name follows the field that already existed, sx_interpreter_path, and the
 * model is the one the kernel already imposes: the kernel maps the main image
 * and the program does the linking. So "running the interpreter" here is
 * getting the executable ready: loading what it declares and filling its GOT. */
int sx_run_interpreter(void) {
    return ldso_start();
}

int ldso_load(const char* path) {
    /* The return code says WHICH STEP failed, so a failure is not an opaque -1:
     * the steps go in order and the last one reached is the one that matters. */
    if (begin_load() == 0) {
        /* Not "could not load": there are already 32 and one more does not fit.
         * The step number does not tell the two apart, and the cure differs:
         * one is a chain too long, the other a file that is not there. */
        report("loader: no room for another library, the limit is %d and there are %d\n",
               kMaxLibraries, g_lib_count);
        return -1;
    }
    /* The path is absolute: the native call knows no working directory, and
     * every path the loader builds starts at /lib. A subsystem's run-time
     * layer makes a relative one absolute before it gets here. */
    const long fd = sxn_open_file(path, SXN_FILE_READ);
    if (fd < 0) {
        /* The commonest case of all and the hardest to guess: a dependency
         * that is not on the volume. The name goes in the message because the
         * one printed above is only a step number. */
        report("loader: cannot open %s (step 2)\n", path);
        end_load(0);
        return -2;
    }
    const long section = sxn_section_open_range(fd, 0, 0, SXN_SECTION_READ | SXN_SECTION_EXEC);
    if (section < 0) {
        report("loader: cannot map %s as an executable section (step 3)\n", path);
        (void)sxn_close(fd);
        end_load(0);
        return -3;
    }
    g_lib->file_section = section;
    g_lib->file_fd = fd;
    g_lib->handles_open = 1;
    g_lib->bias = 0;
    g_lib->bias_ready = 0;
    /* The name keeps the path's basename, to match DT_NEEDED. */
    const char* slash = path;
    for (const char* ch = path; *ch != '\0'; ++ch) {
        if (*ch == '/') {
            slash = ch + 1;
        }
    }
    unsigned i = 0;
    while (slash[i] != '\0' && i < sizeof(g_lib->soname) - 1) {
        g_lib->soname[i] = slash[i];
        i++;
    }
    g_lib->soname[i] = '\0';
    i = 0;
    while (path[i] != '\0' && i < sizeof(g_lib->path) - 1) {
        g_lib->path[i] = path[i];
        i++;
    }
    g_lib->path[i] = '\0';

    /* The header is read through the handle, not by mapping the whole file:
     * it is 64 bytes of Elf64_Ehdr plus 56 per program header, and mapping
     * 40 KiB to read 3.6 KiB leaves a live view right in the range where the
     * segments are placed next. */
    unsigned char probe[sizeof(Elf64_Ehdr) + (sizeof(Elf64_Phdr) * kMaxProgramHeaders)];
    unsigned long filled = 0;
    const unsigned long want_bytes =
        sizeof(Elf64_Ehdr) + (sizeof(Elf64_Phdr) * kMaxProgramHeaders);
    while (filled < want_bytes) {
        const long got = sxn_read(fd, probe + filled, want_bytes - filled);
        if (got <= 0) {
            break;
        }
        filled += (unsigned long)got;
    }
    if (filled < sizeof(Elf64_Ehdr)) {
        end_load(0);
        return -4;
    }
    Elf64_Ehdr header;
    if (!read_header(probe, &header)) {
        /* read_header does not say which of its six conditions failed, and
         * here there is one message for all six. The common case -- not an ELF
         * image -- is told apart from the rest, because a file that is not an
         * image and an image with an impossible e_phnum break differently. */
        if (probe[0] != 0x7f || probe[1] != 'E' || probe[2] != 'L' || probe[3] != 'F')
        {
            report("loader: %s does not start with the ELF magic\n", path);
        }
        else
        {
            report("loader: %s is an ELF that cannot be a library (class, type, "
                   "machine or number of program headers)\n",
                   path);
        }
        end_load(0);
        return -5;
    }
    copy_bytes(&header, &g_lib->header, sizeof(header));
    g_lib->header_count = header.e_phnum;
    for (Elf64_Half index = 0; index < header.e_phnum; ++index) {
        copy_bytes(probe + header.e_phoff + (index * header.e_phentsize), &g_lib->headers[index], sizeof(Elf64_Phdr));
        g_lib->placed[index] = 0;
    }

    for (Elf64_Half index = 0; index < header.e_phnum; ++index) {
        if (g_lib->headers[index].p_type != PT_LOAD || g_lib->headers[index].p_memsz == 0) {
            continue;
        }
        g_lib->placed[index] = place_segment(&g_lib->headers[index]);
        if (g_lib->placed[index] == 0) {
            g_lib_fail_index = (int)index;
            end_load(0);
            return -6;
        }
    }

    if (!read_dynamic_in(g_lib - g_libs)) {
        end_load(0);
        return -7;
    }

    /* Publish, walk the chain, and only then relocate. The order is not
     * cosmetic: a GOT that points into a dependency has to be filled when the
     * dependency is already mapped, not before. Relocating earlier, the
     * dependency does not exist yet and the load fails.
     *
     * Each load in the chain overwrites g_lib, so this library is saved and
     * restored on the way back. Without that, the relocations here would be
     * applied to the last dependency brought in. */
    const int slot = (int)(g_lib - g_libs);
    Library* self = g_lib;
    publish();
    const int chain_failed = load_needed_chain(g_lib_count - 1);
    g_lib = self;
    /* Closing goes BEFORE letting go of the pointer: close_file_handles works
     * on g_lib, so clearing it first would leave it doing nothing. */
    if (chain_failed != 0) {
        close_file_handles();
        g_lib = 0;
        return -8;
    }

    if (!apply_relocs_in(slot)) {
        close_file_handles();
        g_lib = 0;
        return -9;
    }
    close_file_handles();
    g_lib = 0;
    return 0;
}

/* Closes the file handle and the file's section of the library in progress.
 *
 * Both handles are needed only while the program headers are read and the
 * segments mapped, which is over. Leaving them open cost TWO descriptors per
 * library for good, and it showed: windowd launches its clients and they
 * inherit its table, so loading a third library -- libgfx2d -- pushed the
 * session from 20 to 21 descriptors and windowd-smoke failed on it. With three
 * libraries that was six descriptors burnt in every process, unused.
 *
 * The struct's comment used to say the descriptor "stays open but the file is
 * not read again": a handle kept to do nothing. */
static void close_file_handles(void) {
    if (g_lib == 0 || !g_lib->handles_open) {
        return;
    }
    if (g_lib->file_section >= 0) {
        (void)sxn_close(g_lib->file_section);
        g_lib->file_section = -1;
    }
    if (g_lib->file_fd >= 0) {
        (void)sxn_close(g_lib->file_fd);
        g_lib->file_fd = -1;
    }
    g_lib->handles_open = 0;
}

/* The slot of a loaded image with this basename, or -1. The caller needs the
 * slot and not just a yes: it is what goes into the dependent's deps mask. */
static int loaded_slot_named(const char* soname) {
    for (int slot = 0; slot < g_lib_count; ++slot) {
        int i = 0;
        for (;; ++i) {
            if (g_libs[slot].soname[i] != soname[i]) {
                break;
            }
            if (soname[i] == '\0') {
                return slot;
            }
        }
    }
    return -1;
}

/* Walks a library's DT_NEEDED chain. Loads whatever is missing from /lib/ and
 * advances the cursor of the library being processed, so it never loads the
 * same thing twice.
 *
 * Iterative and not recursive on purpose: loading a dependency changes g_lib
 * (the library in progress), so carrying it through recursion would mean
 * saving and restoring the whole context at every level. A loop is enough:
 * libraries are processed in the order they were completed, and each new load
 * points at the slot of the one in progress, not of the one being walked. */
static int load_needed_chain(int parent_slot) {
    int failed = 0;
    for (int work = parent_slot; work < g_lib_count; ++work) {
        Library* lib = &g_libs[work];
        for (;;) {
            const char* need = next_needed(work);
            if (need == 0) {
                break;
            }
            lib->needed_done++;
            const int existing = loaded_slot_named(need);
            if (existing >= 0) {
                lib->deps |= (uint64_t)1 << existing;
                continue;
            }
            /* A DT_NEEDED name is a basename, not a path: it is looked up in
             * /lib/. A path that already starts with / is used as it is. */
            char full[128];
            if (need[0] == '/') {
                unsigned i = 0;
                while (need[i] != '\0' && i < sizeof(full) - 1) {
                    full[i] = need[i];
                    i++;
                }
                full[i] = '\0';
            } else {
                const char* prefix = "/lib/";
                unsigned i = 0;
                while (*prefix != '\0' && i < sizeof(full) - 1) {
                    full[i++] = *prefix++;
                }
                unsigned j = 0;
                while (need[j] != '\0' && i < sizeof(full) - 1) {
                    full[i++] = need[j++];
                }
                full[i] = '\0';
            }
            /* begin_load takes the next free slot, so this is where the
             * dependency will land if it loads. */
            const int dep_slot = g_lib_count;
            if (ldso_load(full) == 0) {
                lib->deps |= (uint64_t)1 << dep_slot;
            } else {
                /* The NAME is kept, not only the step code. The code says where
                 * it failed -- and where is "somewhere in the chain" -- and the
                 * name says what is missing. Without it a program cannot even
                 * tell "not installed" from "broken", nor say which.
                 *
                 * The FIRST one to fail is named, not the last. With the last,
                 * the name depended on the order of DT_NEEDED and on the walk,
                 * so two images missing the same three libraries could name
                 * three different ones. With the first, the name is "the first
                 * dependency that could not be brought in", which is a
                 * definition and not an accident of the walk.
                 *
                 * And every one is counted, which is what lets the program say
                 * three are missing instead of implying one. `need` is a
                 * basename -- DT_NEEDED is one -- and stays mapped, so the
                 * first one's pointer needs no copy. */
                unsigned seen;
                for (seen = 0; seen < g_missing_count; ++seen) {
                    if (same_string(g_missing_names[seen], need)) {
                        break;
                    }
                }
                if (seen == g_missing_count && g_missing_count < kMaxLibraries) {
                    copy_bytes(need, g_missing_names[seen], sizeof(g_missing_names[0]));
                    g_missing_count++;
                }
                if (g_missing_count == 1) {
                    g_missing_library = need;
                }
                failed = 1;
            }
        }
    }
    /* A missing dependency must NOT end the walk.
     *
     * The executable's DT_NEEDED is a list, not a chain: if libffmpeg is not
     * there, that says something about libffmpeg and nothing about libsxgui,
     * libgfx2d or libsxgfx, which come next in the list and are needed all the
     * same. Returning here left ALL of them unloaded, and with tolerant
     * relocation -- which skips what does not resolve instead of aborting -- the
     * program started with all four absent and died at the first toolkit call,
     * jumping through the unrelocated slot of sx_rect_make@plt. A silent failure
     * unrelated to its cause: the worst of both worlds.
     *
     * The failure is still reported, which is why != 0 is returned anyway, but
     * after loading everything that can be. The difference between this and
     * before is the one between "libffmpeg is missing" and "nothing is there". */
    return failed;
}

/* Maps a segment at the address the ELF asks for.
 *
 * A writable segment cannot come from the file's section: the backing is
 * read-only precisely so that two processes loading the same library do not
 * step on each other, and a shared object's GOT lives in a writable segment. So
 * an anonymous section is created at that address and the file's bytes are
 * copied in. Each process pays for its copy, which is what any library with
 * relocated data pays. */
static void* place_segment(const Elf64_Phdr* ph) {
    const int wants_exec = (ph->p_flags & PF_X) != 0;
    const unsigned flags =
        wants_exec ? (SXN_SECTION_READ | SXN_SECTION_EXEC) : (SXN_SECTION_READ | SXN_SECTION_WRITE);
    const unsigned long at = (unsigned long)(ph->p_vaddr & ~(Elf64_Addr)4095);
    /* The segment's bytes start at offset p_vaddr - at inside the page, NOT at
     * the start of the mapping.
     *
     * map_view_at wants a page-aligned address and puts the section's first
     * byte there. So the range is asked from the start of the segment's page,
     * not from p_offset: that way the first byte lands where it must and the
     * WHOLE image shares one bias. Without it each segment has its own anchor,
     * and references between segments -- a PLT in the code jumping to a GOT in
     * the data -- point into the wrong image without any visible error.
     *
     * The range starts at p_offset - off_in_page, the start of that page in the
     * file: ELF keeps p_offset and p_vaddr congruent modulo the page, so it is
     * never negative. */
    const unsigned long off_in_page = (unsigned long)(ph->p_vaddr - at);
    const unsigned long start_off = (unsigned long)ph->p_offset - off_in_page;
    const unsigned long span = off_in_page + (unsigned long)ph->p_filesz;
    unsigned long want = at;
    if (g_lib->bias_ready) {
        want = g_lib->bias + at;
    }

    long segment_section;
    if (wants_exec) {
        /* Only the stretch of the file that belongs to THIS segment. Mapping the
         * whole file's section would put the rest of the ELF at addresses that
         * are not its own: the first LOAD asks p_vaddr 0 for offset 0 and the
         * next asks 0x2560 for offset 0x1560. */
        segment_section = sxn_section_open_range(g_lib->file_fd, start_off, span, flags);
    } else {
        /* The private region has to include the gap of the page before the
         * segment, or the copied bytes do not fit. */
        segment_section = sxn_section_create(off_in_page + (unsigned long)ph->p_memsz, flags);
    }
    if (segment_section < 0) {
        g_lib_fail_errno = segment_section;
        return 0;
    }

    /* A base, or -errno: no user address is that high. */
    const long view = sxn_map_view_at(segment_section, want, flags);
    if (view <= 0) {
        g_lib_fail_errno = view;
        (void)sxn_close(segment_section);
        return 0;
    }
    if (wants_exec) {
        /* The view holds its own reference to the section (SECTIONS.md, "The
         * budget"), so the handle is done. It used to stay open: one descriptor
         * per library for the life of the process, out of 64. */
        (void)sxn_close(segment_section);
    }
    if (!g_lib->bias_ready) {
        /* The kernel chose; that is the bias. The following segments are asked
         * for explicitly so they cannot land anywhere else. */
        g_lib->bias = (unsigned long)view - at;
        g_lib->bias_ready = 1;
        g_lib_bias = g_lib->bias;
    }
    if (!wants_exec) {
        /* The bytes come from a section over the EXACT range of the file, mapped
         * for a moment and released. Reading the handle would not do: there is
         * no seek, the handle reads on from where it stopped, and the segment
         * would get the bytes of the wrong offset. */
        const long source = sxn_section_open_range(g_lib->file_fd, start_off, span, SXN_SECTION_READ);
        if (source < 0) {
            (void)sxn_close(segment_section);
            (void)sxn_unmap_view((uint64_t)view);
            return 0;
        }
        const long bytes = sxn_map_view_at(source, 0, SXN_SECTION_READ);
        if (bytes <= 0) {
            (void)sxn_close(source);
            (void)sxn_close(segment_section);
            (void)sxn_unmap_view((uint64_t)view);
            return 0;
        }
        copy_bytes((const void*)(uintptr_t)bytes, (void*)(uintptr_t)view, (size_t)span);
        (void)sxn_unmap_view((uint64_t)bytes);
        (void)sxn_close(source);
        /* The bytes past p_filesz are .bss: the section came zeroed. */
        (void)sxn_close(segment_section);
    }
    return (void*)(uintptr_t)view;
}

/* Translates an st_value with the segments of one particular image.
 *
 * It is separate from translating an address of the image being loaded because
 * an st_value belongs to the image that DEFINED the symbol, not to the one using
 * it. Biasing it with another image's map gives an address inside the wrong
 * image, and nothing visibly fails: it lands in someone else's code. */
static void* bias_in(int slot, Elf64_Addr value) {
    return map_of(slot, value);
}

/* Translates an ELF address to the real one, within one particular image: an
 * address of a shared object only means something inside the library that
 * declared it, since two images can have the same vaddr for different things.
 *
 * placed[] keeps the PAGE ADDRESS where the segment was mapped, that is
 * bias + (p_vaddr & ~4095). Subtracting an unaligned p_vaddr would shift the
 * address by exactly how far the segment starts into its page, and the error
 * lands inside the image itself: a GOT written in the wrong place, a call that
 * jumps somewhere else, and a fault that does not look like it comes from here. */
static void* map_of(int slot, Elf64_Addr vaddr) {
    const Library* library = &g_libs[slot];
    for (Elf64_Half index = 0; index < library->header_count; ++index) {
        const Elf64_Phdr* ph = &library->headers[index];
        if (ph->p_type != PT_LOAD || library->placed[index] == 0) {
            continue;
        }
        if (vaddr < ph->p_vaddr || vaddr >= ph->p_vaddr + ph->p_memsz) {
            continue;
        }
        const Elf64_Addr page = ph->p_vaddr & ~(Elf64_Addr)4095;
        return (void*)(vaddr - page + (Elf64_Addr)(unsigned long)library->placed[index]);
    }
    return 0;
}

typedef struct {
    Elf64_Sword tag;
    union {
        Elf64_Addr pointer;
        Elf64_Word value;
    } u;
} Elf64_Dyn;

/* Walks the dynamic table and stores the value of the entry asked for. Entries
 * have a fixed size, so the offset steps by sizeof(Elf64_Dyn). */
static int find_dynamic_in(int slot, Elf64_Sword wanted, void* out) {
    const Library* library = &g_libs[slot];
    for (Elf64_Half index = 0; index < library->header_count; ++index) {
        const Elf64_Phdr* ph = &library->headers[index];
        if (ph->p_type != PT_DYNAMIC) {
            continue;
        }
        const unsigned char* table = (const unsigned char*)map_of(slot, ph->p_vaddr);
        if (table == 0) {
            return 0;
        }
        for (Elf64_Addr offset = 0; offset + sizeof(Elf64_Dyn) <= ph->p_memsz;
             offset += sizeof(Elf64_Dyn)) {
            Elf64_Dyn entry;
            copy_bytes(table + offset, &entry, sizeof(entry));
            if (entry.tag == DT_NULL) {
                return 0;
            }
            if (entry.tag == wanted) {
                *(unsigned long*)out = (unsigned long)entry.u.pointer;
                return 1;
            }
        }
        return 0;
    }
    return 0;
}

/* The name of the first DT_NEEDED not loaded yet, or 0 when none is left.
 *
 * The walk's index is what avoids loading the same dependency twice in a chain
 * with diamonds: if A needs B and C, and both need D, D is loaded once and the
 * second finds it.
 *
 * The name comes from that same library, translated with ITS map, not with the
 * one in progress: with two images mapped, the same vaddr means two different
 * things.
 *
 * slot < 0 is the library in progress. */
static const char* next_needed(int slot) {
    Library* library = slot < 0 ? g_lib : &g_libs[slot];
    if (library == 0 || library->dynstr == 0) {
        return 0;
    }
    unsigned seen = 0;
    for (Elf64_Half index = 0; index < library->header_count; ++index) {
        const Elf64_Phdr* ph = &library->headers[index];
        if (ph->p_type != PT_DYNAMIC) {
            continue;
        }
        /* PT_DYNAMIC lies inside a PT_LOAD; it is translated with the segments
         * THIS library already placed, not with the ones in progress. */
        const unsigned char* table = (const unsigned char*)map_of(slot, ph->p_vaddr);
        if (table == 0) {
            return 0;
        }
        for (Elf64_Addr offset = 0; offset + sizeof(Elf64_Dyn) <= ph->p_memsz;
             offset += sizeof(Elf64_Dyn)) {
            Elf64_Dyn entry;
            copy_bytes(table + offset, &entry, sizeof(entry));
            if (entry.tag == DT_NULL) {
                return 0;
            }
            if (entry.tag != DT_NEEDED) {
                continue;
            }
            if (seen < library->needed_done) {
                seen++;
                continue;
            }
            return library->dynstr + entry.u.pointer;
        }
        return 0;
    }
    return 0;
}

static int resolve_in(const char* name, int slot, Elf64_Addr* value_out) {
    const Library* library = &g_libs[slot];
    if (library->dynsym == 0 || library->dynstr == 0) {
        return -1;
    }
    const size_t limit = library->dynstr_size;
    const unsigned long count = library->symbol_count;
    for (unsigned index = 0;; ++index) {
        if (count != 0 && index >= count) {
            return -1;
        }
        Elf64_Sym symbol;
        copy_bytes(library->dynsym + (index * sizeof(Elf64_Sym)), &symbol, sizeof(symbol));
        if (symbol.st_name >= limit) {
            return -1;
        }
        /* An UNDEFINED entry with the name sought is not an answer: it is the
         * question itself. Accepting it, st_value = 0 would translate to the
         * image's base, and a call would end in the ELF header. Only the image
         * that DEFINED the symbol answers. */
        if (symbol.st_shndx == SHN_UNDEF) {
            continue;
        }
        if (same_string(library->dynstr + symbol.st_name, name)) {
            *value_out = symbol.st_value;
            return slot;
        }
    }
}

/* An image and everything it depends on, transitively, as a slot mask. */
static uint64_t scope_of(int slot) {
    uint64_t scope = (uint64_t)1 << slot;
    uint64_t grown = scope;
    do {
        scope = grown;
        for (int index = 0; index < g_lib_count; ++index) {
            if ((scope & ((uint64_t)1 << index)) != 0) {
                grown |= g_libs[index].deps;
            }
        }
    } while (grown != scope);
    return scope;
}

/* The global scope: newest image first, the executable last. That order is the
 * one this loader has always had -- a library wins over the one that loaded it
 * and the program is the fallback -- and a global lookup keeps it. */
static int resolve(const char* name, Elf64_Addr* value_out) {
    for (int slot = g_lib_count - 1; slot >= 0; --slot) {
        if (!g_libs[slot].global) {
            continue;
        }
        if (resolve_in(name, slot, value_out) >= 0) {
            return slot;
        }
    }
    return -1;
}

/* What a relocation of `slot` may bind to: every global image plus the image's
 * own dependency tree, in the same newest-first order. Before run-time loading
 * every image was global, so this is the same walk as resolve() for everything
 * loaded at startup. The difference is for a library opened local: it can no
 * longer answer for an image outside its tree. That is what keeps client.so and
 * hl.so, which both define PM_Move, from calling into each other. */
static int resolve_for(int slot, const char* name, Elf64_Addr* value_out) {
    const uint64_t scope = scope_of(slot);
    for (int index = g_lib_count - 1; index >= 0; --index) {
        if (!g_libs[index].global && (scope & ((uint64_t)1 << index)) == 0) {
            continue;
        }
        if (resolve_in(name, index, value_out) >= 0) {
            return index;
        }
    }
    return -1;
}

void* ldso_lookup(const char* name) {
    if (g_lib_count == 0) {
        return 0;
    }
    Elf64_Addr value = 0;
    const int slot = resolve(name, &value);
    if (slot < 0 || value == 0) {
        return 0;
    }
    /* The value is relative to the library where it was found, so it is
     * translated with ITS map and not with the last one loaded: with two images
     * mapped, the same vaddr in both is two different addresses. */
    return map_of(slot, value);
}

/* The executable is slot 0, so "came from a shared library" is "the slot where
 * it was found is not zero".
 *
 * The question is more useful than it looks. Without it, a program that still
 * carries its own copy of the symbol in the binary passes the same check,
 * because in both cases the address the resolver reports matches the one in the
 * GOT. */
int ldso_symbol_is_shared(const char* name) {
    Elf64_Addr value = 0;
    return resolve(name, &value) > 0;
}

/* ---- Run-time loading (loader/loader.h) ------------------------------------
 *
 * Built on the same slots as everything above. An image handle is a pointer to
 * its slot; the executable's slot stands for the global scope. Each subsystem
 * builds its own interface on this: POSIX's <dlfcn.h> is
 * subsystems/posix/userland/dlfcn.c. What is deliberately missing and why is in
 * SHARED_LIBRARIES.md#dlopen. */

/* The slot an image handle names, or -1 when it is not one this loader gave
 * out. */
static int slot_of_handle(void* handle) {
    Library* library = (Library*)handle;
    if (library < &g_libs[0] || library >= &g_libs[g_lib_count]) {
        return -1;
    }
    const int slot = (int)(library - g_libs);
    if (&g_libs[slot] != library || !g_libs[slot].loaded) {
        return -1;
    }
    return slot;
}

static int loaded_slot_at(const char* path) {
    for (int slot = 1; slot < g_lib_count; ++slot) {
        if (same_string(g_libs[slot].path, path)) {
            return slot;
        }
    }
    return -1;
}

/* Global applies to the image and its whole tree, as RTLD_GLOBAL does on
 * Linux: a symbol reachable through the library becomes reachable by everyone. */
static void make_global(int slot) {
    const uint64_t scope = scope_of(slot);
    for (int index = 0; index < g_lib_count; ++index) {
        if ((scope & ((uint64_t)1 << index)) != 0) {
            g_libs[index].global = 1;
        }
    }
}

/* Puts the slot table back to how it was before an open that failed.
 *
 * ldso_load publishes an image before walking its DT_NEEDED and relocating it,
 * so a failure at either step leaves it -- and any dependency it brought -- in
 * the table, mapped and visible to every later lookup. That is acceptable at
 * startup, where the program reports the failure and goes on; at run time it
 * would mean a library that "failed to load" still answers a global lookup.
 * Nothing else runs between, so everything from `start` up is this call's. */
static void roll_back_to(int start) {
    for (int slot = g_lib_count - 1; slot >= start; --slot) {
        unmap_placed(&g_libs[slot]);
        g_libs[slot].loaded = 0;
    }
    g_lib_count = start;
}

/* Runs an image's constructors: DT_INIT, then each DT_INIT_ARRAY entry in order,
 * the order glibc uses. The array was filled by R_X86_64_RELATIVE, so its entries
 * are already final addresses; 0 and -1 are the conventional empty markers.
 *
 * Only sx_loader_open calls this. Nothing at startup runs constructors -- not
 * crt0 for the program, not the loader for its DT_NEEDED -- because nothing in
 * this tree has any; a C++ module opened at run time, like Xash3D's client.so
 * with its global CHud, is the first thing that does. */
typedef void (*initializer_fn)(void);

static void run_initializers(int slot) {
    unsigned long init = 0;
    if (find_dynamic_in(slot, DT_INIT, &init) && init != 0) {
        initializer_fn function = (initializer_fn)map_of(slot, init);
        if (function != 0) {
            function();
        }
    }
    unsigned long array = 0;
    unsigned long size = 0;
    if (!find_dynamic_in(slot, DT_INIT_ARRAY, &array) ||
        !find_dynamic_in(slot, DT_INIT_ARRAYSZ, &size)) {
        return;
    }
    const unsigned long* entries = (const unsigned long*)map_of(slot, array);
    if (entries == 0) {
        return;
    }
    for (unsigned long index = 0; index < size / sizeof(unsigned long); ++index) {
        if (entries[index] != 0 && entries[index] != ~0UL) {
            ((initializer_fn)entries[index])();
        }
    }
}

static const char* step_reason(int step) {
    switch (step) {
    case -1: return "no free library slot";
    case -2: return "cannot open file";
    case -3: return "cannot map file as an executable section";
    case -4: return "file too short for an ELF header";
    case -5: return "not an x86-64 ELF shared object";
    case -6: return "cannot map a segment";
    case -7: return "no dynamic symbol table";
    case -8: return "a dependency failed to load";
    case -9: return "relocation failed";
    default: return "load failed";
    }
}

void* sx_loader_program(void) {
    adopt_executable_once();
    return g_lib_count != 0 ? &g_libs[0] : 0;
}

void* sx_loader_find(const char* path) {
    adopt_executable_once();
    const int slot = loaded_slot_at(path);
    return slot >= 0 ? &g_libs[slot] : 0;
}

void* sx_loader_open(const char* path, int global, char* error, unsigned long error_size) {
    adopt_executable_once();
    unsigned long used = 0;
    if (error != 0 && error_size != 0) {
        error[0] = '\0';
    }
    /* ldso_missing() describes the program's startup and must not change
     * because of a run-time open, so its state is put back however this ends. */
    const int start = g_lib_count;
    const char* missing_before = g_missing_library;
    const unsigned missing_count_before = g_missing_count;
    g_lib_fail_symbol = 0;
    g_lib_fail_library = 0;
    g_load_global = global != 0;
    g_strict = 1;
    const int step = ldso_load(path);
    g_strict = 0;
    g_load_global = 1;

    if (step != 0) {
        append_text(error, error_size, &used, path);
        if (step == -8 && g_missing_count > missing_count_before) {
            append_text(error, error_size, &used, ": a dependency failed to load: ");
            append_text(error, error_size, &used, g_missing_names[missing_count_before]);
        } else if (step == -9 && g_lib_fail_symbol != 0) {
            append_text(error, error_size, &used, ": unresolved symbol \"");
            append_text(error, error_size, &used, g_lib_fail_symbol);
            append_text(error, error_size, &used, "\" needed by ");
            append_text(error, error_size, &used, g_lib_fail_library);
        } else {
            append_text(error, error_size, &used, ": ");
            append_text(error, error_size, &used, step_reason(step));
        }
        roll_back_to(start);
        g_missing_library = missing_before;
        g_missing_count = missing_count_before;
        return 0;
    }
    g_missing_library = missing_before;
    g_missing_count = missing_count_before;

    /* begin_load gave the requested image the first free slot; its
     * dependencies came after it. */
    const int slot = start;
    if (global) {
        /* Dependencies that were already loaded local become global too. */
        make_global(slot);
    }
    g_libs[slot].refcount = 1;
    /* Dependencies before dependents. A call's images occupy start.. upwards,
     * each library ahead of the dependencies it brought, so walking down from
     * the newest initializes every dependency before whatever needs it. */
    const int newest = g_lib_count - 1;
    for (int index = newest; index >= start; --index) {
        run_initializers(index);
    }
    return &g_libs[slot];
}

void sx_loader_make_global(void* image) {
    const int slot = slot_of_handle(image);
    if (slot >= 0) {
        make_global(slot);
    }
}

void sx_loader_retain(void* image) {
    if (image != 0) {
        ((Library*)image)->refcount++;
    }
}

int sx_loader_release(void* image) {
    const int slot = slot_of_handle(image);
    if (slot < 0 || g_libs[slot].refcount == 0) {
        return -1;
    }
    /* The image stays mapped: nothing is ever unloaded. */
    g_libs[slot].refcount--;
    return 0;
}

int sx_loader_symbol(void* image, const char* name, void** address) {
    Elf64_Addr value = 0;
    int owner = -1;
    *address = 0;
    if (image == 0 || image == &g_libs[0]) {
        owner = resolve(name, &value);
    } else {
        const int slot = slot_of_handle(image);
        if (slot < 0) {
            return -1;
        }
        /* The image itself first, then its dependency tree in load order. */
        if (resolve_in(name, slot, &value) >= 0) {
            owner = slot;
        } else {
            const uint64_t scope = scope_of(slot);
            for (int index = 0; index < g_lib_count; ++index) {
                if (index == slot || (scope & ((uint64_t)1 << index)) == 0) {
                    continue;
                }
                if (resolve_in(name, index, &value) >= 0) {
                    owner = index;
                    break;
                }
            }
        }
    }
    *address = owner >= 0 ? map_of(owner, value) : 0;
    return *address != 0 ? 0 : -2;
}

/* Xash3D's save/restore turns function pointers into names this way. A symbol
 * matches when the address falls inside it; one with no size matches only its
 * own address. Of the matches, the highest start wins. */
int sx_loader_address(const void* address, const char** path, void** base,
                      const char** symbol_name, void** symbol_address) {
    const unsigned long target = (unsigned long)address;
    for (int slot = 0; slot < g_lib_count; ++slot) {
        const Library* library = &g_libs[slot];
        if (!library->loaded && slot != 0) {
            continue;
        }
        if (!library->bias_ready || target < library->bias) {
            continue;
        }
        const Elf64_Addr vaddr = (Elf64_Addr)(target - library->bias);
        int inside = 0;
        Elf64_Addr first = ~(Elf64_Addr)0;
        for (Elf64_Half index = 0; index < library->header_count; ++index) {
            const Elf64_Phdr* ph = &library->headers[index];
            if (ph->p_type != PT_LOAD || library->placed[index] == 0) {
                continue;
            }
            if (ph->p_vaddr < first) {
                first = ph->p_vaddr;
            }
            if (vaddr >= ph->p_vaddr && vaddr < ph->p_vaddr + ph->p_memsz) {
                inside = 1;
            }
        }
        if (!inside) {
            continue;
        }
        *path = library->path;
        *base = (void*)(library->bias + (first & ~(Elf64_Addr)4095));
        *symbol_name = 0;
        *symbol_address = 0;
        /* Without a count there is no safe end to the table, so no name. */
        Elf64_Addr best = 0;
        for (unsigned long index = 1; index < library->symbol_count; ++index) {
            Elf64_Sym symbol;
            copy_bytes(library->dynsym + (index * sizeof(Elf64_Sym)), &symbol, sizeof(symbol));
            const unsigned type = symbol.st_info & 0xfu;
            if (symbol.st_shndx == SHN_UNDEF || (type != STT_FUNC && type != STT_OBJECT)) {
                continue;
            }
            if (symbol.st_value > vaddr || symbol.st_name >= library->dynstr_size) {
                continue;
            }
            const int covers = symbol.st_size != 0 ? vaddr < symbol.st_value + symbol.st_size
                                                   : vaddr == symbol.st_value;
            if (!covers || (*symbol_name != 0 && symbol.st_value < best)) {
                continue;
            }
            best = symbol.st_value;
            *symbol_name = library->dynstr + symbol.st_name;
            *symbol_address = map_of(slot, symbol.st_value);
        }
        return 1;
    }
    return 0;
}
