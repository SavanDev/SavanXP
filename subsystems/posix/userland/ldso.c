/* Ver ldso.h. Todo el trabajo es aritmetica de punteros sobre una imagen ya
 * mapeada, mas la copia de los segmentos escribibles. */

#include "ldso.h"

#include "libc.h"

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
#define EM_X86_64 62

#define PT_LOAD 1
#define PT_DYNAMIC 2

#define PF_X 1
#define PF_W 2
#define PF_R 4

#define DT_NULL 0
#define DT_PLTRELSZ 2
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_STRTAB 5
#define DT_STRSZ 10
#define DT_SYMTAB 6
#define DT_JMPREL 23
#define DT_PLTREL 20

#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_GLOB_DAT 6

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

/* El estado de la unica libreria cargada. Se podria hacer una tabla, pero el
 * alcance es una sola y una tabla seria mas codigo sin un caso que la use. */
static struct {
    int loaded;
    int file_section;
    int file_fd;
    /* Desplazamiento de carga. Un ET_DYN tiene p_vaddr cerca de cero, y cero no
     * es una direccion de usuario valida: el kernel no mapea por debajo de
     * kUserBase. Asi que el primer segmento -- el que tiene p_vaddr 0 -- se pide
     * sin base para que el kernel elija una buena, y esa direccion menos su
     * p_vaddr es el bias contra el que va todo lo demas. */
    unsigned long bias;
    int bias_ready;
    Elf64_Ehdr header;
    Elf64_Half header_count;
    const unsigned char* dynsym;
    const char* dynstr;
    size_t dynstr_size;
    /* Los program headers se leen una vez y se guardan aca: despues el
     * descriptor sigue abierto pero no se vuelve a leer el archivo. */
    Elf64_Phdr headers[64];
    /* Direccion final de cada segmento, en el mismo orden que los program
     * headers. Los indices son los de los headers, asi que un -1 es "este
     * segmento no se pudo mapear". */
    void* placed[64];
} g_lib;

/* Ultimo segmento que no se pudo colocar, y el errno que devolvio. El codigo de
 * retorno de ldso_load solo dice "el paso"; con estos dos se sabe cual. */
int g_lib_fail_index;
long g_lib_fail_errno;
unsigned long g_lib_bias;
int g_lib_reloc_step;

int ldso_loaded(void) {
    return g_lib.loaded;
}

/* Los nombres de los simbolos y los PT_LOAD se leen del archivo a traves de la
 * vista que se mapeo para eso. Se mantiene aparte del mapa final porque al final
 * el texto vive en la direccion que pidio el ELF, no en la que el kernel eligio. */
static void copy_bytes(const void* from, void* to, size_t count);
/* El orden del archivo es el del razonamiento --validar, ubicar, colocar,
 * reubicar-- y no el de las dependencias entre funciones. */
static int read_header(const unsigned char* bytes, Elf64_Ehdr* out);
static void* place_segment(const Elf64_Phdr* ph);
static void* at_vaddr(Elf64_Addr vaddr);
static int find_dynamic(Elf64_Sword wanted, void* out);
static int apply_table(unsigned long elf_table, unsigned long elf_size);
static void* translate(unsigned long elf_address);
static int relocate(void);
static Elf64_Addr resolve(const char* name);

static int read_header(const unsigned char* bytes, Elf64_Ehdr* out) {
    if (bytes[0] != 0x7f || bytes[1] != 'E' || bytes[2] != 'L' || bytes[3] != 'F') {
        return 0;
    }
    copy_bytes(bytes, out, sizeof(*out));
    if (out->e_type != ET_DYN || out->e_machine != EM_X86_64) {
        return 0;
    }
    if (out->e_phentsize != sizeof(Elf64_Phdr) || out->e_phnum == 0 || out->e_phnum > 64) {
        return 0;
    }
    return 1;
}

static void copy_bytes(const void* from, void* to, size_t count) {
    const unsigned char* src = (const unsigned char*)from;
    unsigned char* dst = (unsigned char*)to;
    for (size_t index = 0; index < count; ++index) {
        dst[index] = src[index];
    }
}

int ldso_load(const char* path) {
    /* El codigo de retorno dice EN QUE PASO fallo, para que un fallo no sea un
     * -1 opaco: los pasos van en orden y el ultimo que se alcanzo a hacer es el
     * que importa. */
    if (g_lib.loaded) {
        return -1;
    }
    const long fd = savanxp_open(path);
    if (fd < 0) {
        return -2;
    }
    const long section = section_open((int)fd, SAVANXP_SECTION_READ | SAVANXP_SECTION_EXEC);
    if (section < 0) {
        savanxp_close((int)fd);
        return -3;
    }
    g_lib.file_section = (int)section;
    g_lib.file_fd = (int)fd;
    g_lib.bias = 0;
    g_lib.bias_ready = 0;

    /* Mapa de trabajo: el archivo entero en una direccion que eligio el kernel,
     * solo para leerle la cabecera y los program headers. */
    /* La cabecera se lee por el descriptor, no mapeando el archivo entero: son
     * 64 bytes de Elf64_Ehdr mas 56 por program header, y mapear 40 KiB para
     * leer 3.6 KiB deja una vista viva justo en el rango donde despues se
     * colocan los segmentos. */
    unsigned char probe[sizeof(Elf64_Ehdr) + (sizeof(Elf64_Phdr) * 64)];
    unsigned long filled = 0;
    const unsigned long want_bytes = sizeof(Elf64_Ehdr) + (sizeof(Elf64_Phdr) * 64);
    while (filled < want_bytes) {
        const long got = savanxp_read((int)fd, probe + filled, want_bytes - filled);
        if (got <= 0) {
            break;
        }
        filled += (unsigned long)got;
    }
    if (filled < sizeof(Elf64_Ehdr)) {
        return -4;
    }
    Elf64_Ehdr header;
    if (!read_header(probe, &header)) {
        return -5;
    }
    g_lib.header = header;
    g_lib.header_count = header.e_phnum;
    for (Elf64_Half index = 0; index < header.e_phnum; ++index) {
        copy_bytes(probe + header.e_phoff + (index * header.e_phentsize), &g_lib.headers[index], sizeof(Elf64_Phdr));
        g_lib.placed[index] = 0;
    }

    for (Elf64_Half index = 0; index < header.e_phnum; ++index) {
        if (g_lib.headers[index].p_type != PT_LOAD || g_lib.headers[index].p_memsz == 0) {
            continue;
        }
        g_lib.placed[index] = place_segment(&g_lib.headers[index]);
        if (g_lib.placed[index] == 0) {
            g_lib_fail_index = (int)index;
            return -6;
        }
    }

    if (!relocate()) {
        return -7;
    }
    g_lib.loaded = 1;
    return 0;
}

/* Mapea un segmento en la direccion que pide el ELF.
 *
 * Un segmento escribible no puede salir de la seccion del archivo: el respaldo
 * es de solo lectura justamente para que dos procesos que cargan la misma
 * libreria no se pisen, y el GOT de un .so vive en un segmento escribible. Asi
 * que se crea una seccion anonima en esa direccion y se copia el contenido del
 * archivo. Cada proceso paga su copia, que es lo mismo que paga cualquier
 * biblioteca con datos reubicados. */
static void* place_segment(const Elf64_Phdr* ph) {
    const int wants_exec = (ph->p_flags & PF_X) != 0;
    const unsigned long flags =
        wants_exec ? (SAVANXP_SECTION_READ | SAVANXP_SECTION_EXEC) : (SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    const unsigned long at = (unsigned long)(ph->p_vaddr & ~(Elf64_Addr)4095);
    unsigned long want = at;
    if (g_lib.bias_ready) {
        want = g_lib.bias + at;
    }

    long segment_section;
    if (wants_exec) {
        /* Solo el tramo del archivo que corresponde a ESTE segmento. Mapear la
         * seccion del archivo entero pondria el resto del ELF en direcciones que
         * no le tocan: el primer LOAD pide p_vaddr 0 para el offset 0 y el
         * siguiente pide 0x2560 para el offset 0x1560. */
        segment_section = section_open_range(
            g_lib.file_fd, (unsigned long)ph->p_offset, (unsigned long)ph->p_filesz, flags);
    } else {
        segment_section = section_create((unsigned long)ph->p_memsz, flags);
    }
    if (segment_section < 0) {
        g_lib_fail_errno = segment_section;
        return 0;
    }

    void* view = map_view_at((int)segment_section, want, flags);
    if (view == 0 || result_is_error((long)view)) {
        g_lib_fail_errno = (long)view;
        if (!wants_exec) {
            (void)savanxp_close((int)segment_section);
        }
        return 0;
    }
    if (!g_lib.bias_ready) {
        /* El kernel eligio; ese es el bias. Los segmentos siguientes se piden
         * explicitamente para que no puedan caer en otra parte. */
        g_lib.bias = (unsigned long)view - at;
        g_lib.bias_ready = 1;
        g_lib_bias = g_lib.bias;
    }
    if (!wants_exec) {
        /* Los bytes salen de una seccion del rango EXACTO del archivo, mapeada un
         * momento y liberada. No se puede savanxp_read: no hay seek en la API, el
         * descriptor sigue leyendo desde donde quedo la vez anterior, y el
         * segmento recibiria bytes del offset equivocado. */
        const long source = section_open_range(
            g_lib.file_fd, (unsigned long)ph->p_offset, (unsigned long)ph->p_filesz, SAVANXP_SECTION_READ);
        if (source < 0) {
            (void)savanxp_close((int)segment_section);
            return 0;
        }
        const unsigned char* bytes = (const unsigned char*)map_view_at((int)source, 0, SAVANXP_SECTION_READ);
        if (bytes == 0 || result_is_error((long)bytes)) {
            (void)savanxp_close((int)source);
            (void)savanxp_close((int)segment_section);
            return 0;
        }
        copy_bytes(bytes, view, (size_t)ph->p_filesz);
        (void)unmap_view((void*)bytes);
        (void)savanxp_close((int)source);
        /* Los bytes mas alla de p_filesz son .bss: section_create ya entrego la
         * pagina en cero. */
        (void)savanxp_close((int)segment_section);
    }
    return view;
}


/* Traduce una direccion del ELF a la direccion real, usando el segmento que la
 * contiene. Los indices de headers y de placed son los mismos. */
static void* at_vaddr(Elf64_Addr vaddr) {
    for (Elf64_Half index = 0; index < g_lib.header_count; ++index) {
        const Elf64_Phdr* ph = &g_lib.headers[index];
        if (ph->p_type != PT_LOAD || g_lib.placed[index] == 0) {
            continue;
        }
        if (vaddr < ph->p_vaddr || vaddr >= ph->p_vaddr + ph->p_memsz) {
            continue;
        }
        return (void*)(vaddr - ph->p_vaddr + (Elf64_Addr)(unsigned long)g_lib.placed[index]);
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

/* Recorre la tabla dinamica y deja el puntero a la entrada pedida. Las entradas
 * tienen tamaño fijo, asi que el indice se multiplica por sizeof(Elf64_Dyn). */
static int find_dynamic(Elf64_Sword wanted, void* out) {
    for (Elf64_Half index = 0; index < g_lib.header_count; ++index) {
        const Elf64_Phdr* ph = &g_lib.headers[index];
        if (ph->p_type != PT_DYNAMIC) {
            continue;
        }
        const unsigned char* table = (const unsigned char*)at_vaddr(ph->p_vaddr);
        if (table == 0) {
            return 0;
        }
        for (Elf64_Addr offset = 0; offset + sizeof(Elf64_Dyn) <= ph->p_memsz; offset += sizeof(Elf64_Dyn)) {
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

/* Direccion del simbolo pedido, o 0 si no esta definido aca. La tabla de
 * simbolos se recorre entero: son ~94 entradas y un hash seria mas codigo del
 * que el caso necesita. */
static Elf64_Addr resolve(const char* name) {
    if (g_lib.dynsym == 0 || g_lib.dynstr == 0) {
        return 0;
    }
    const size_t limit = g_lib.dynstr_size;
    for (unsigned index = 0;; ++index) {
        Elf64_Sym symbol;
        copy_bytes(g_lib.dynsym + (index * sizeof(Elf64_Sym)), &symbol, sizeof(symbol));
        if (symbol.st_name >= limit) {
            return 0;
        }
        const char* candidate = g_lib.dynstr + symbol.st_name;
        if (strcmp(candidate, name) == 0) {
            return symbol.st_value;
        }
    }
}

/* Aplica una tabla RELA: .rela.dyn y .rela.plt. Las dos son obligatorias antes
 * de llamar a nada, asi que no hay version perezosa todavia. Solo estan
 * soportados JUMP_SLOT y GLOB_DAT, que son los que emite lld para este
 * objetivo; un tipo mas seria un error explicito y no un salto silencioso a la
 * direccion equivocada. */
static int apply_table(unsigned long elf_table, unsigned long elf_size) {
    const unsigned char* table = (const unsigned char*)translate(elf_table);
    if (table == 0) {
        return 0;
    }
    for (unsigned long offset = 0; offset + sizeof(Elf64_Rela) <= elf_size; offset += sizeof(Elf64_Rela)) {
        Elf64_Rela rela;
        copy_bytes(table + offset, &rela, sizeof(rela));
        const unsigned type = (unsigned)(rela.r_info & 0xffffffffu);
        if (type != R_X86_64_JUMP_SLOT && type != R_X86_64_GLOB_DAT) {
            return 0;
        }
        const unsigned name_index = (unsigned)(rela.r_info >> 32);
        Elf64_Sym symbol;
        copy_bytes(g_lib.dynsym + (name_index * sizeof(Elf64_Sym)), &symbol, sizeof(symbol));
        const Elf64_Addr value = (Elf64_Addr)(unsigned long)at_vaddr(symbol.st_value);
        if (value == 0) {
            return 0;
        }
        /* El slot es una direccion de la imagen YA MOVIDA: va por at_vaddr, no
         * por el vaddr crudo del ELF. */
        Elf64_Addr* slot = (Elf64_Addr*)at_vaddr(rela.r_offset);
        if (slot == 0) {
            return 0;
        }
        *slot = value;
    }
    return 1;
}

/* Los d_ptr de la tabla dinamica son direcciones DEL ELF todavia sin mover.
 * Todo lo que salga de ahi pasa por at_vaddr antes de usarse como puntero. */
static void* translate(unsigned long elf_address) {
    return at_vaddr((Elf64_Addr)elf_address);
}

static int relocate(void) {
    unsigned long strtab = 0;
    unsigned long symtab = 0;
    unsigned long jmprel = 0;
    unsigned long pltrelsz = 0;
    unsigned long rel = 0;
    unsigned long relasz = 0;
    unsigned long strsz = 0;

    if (!find_dynamic(DT_STRTAB, &strtab)) { g_lib_reloc_step = 1; return 0; }
    if (!find_dynamic(DT_SYMTAB, &symtab)) { g_lib_reloc_step = 2; return 0; }
    if (!find_dynamic(DT_JMPREL, &jmprel)) { g_lib_reloc_step = 3; return 0; }
    if (!find_dynamic(DT_PLTRELSZ, &pltrelsz)) { g_lib_reloc_step = 4; return 0; }
    (void)find_dynamic(DT_STRSZ, &strsz);

    const unsigned char* strings = (const unsigned char*)translate(strtab);
    const unsigned char* symbols = (const unsigned char*)translate(symtab);
    if (strings == 0) { g_lib_reloc_step = 5; return 0; }
    if (symbols == 0) { g_lib_reloc_step = 6; return 0; }
    g_lib.dynstr = (const char*)strings;
    g_lib.dynstr_size = strsz != 0 ? strsz : 4096;
    g_lib.dynsym = symbols;

    if (find_dynamic(DT_RELA, &rel) && find_dynamic(DT_RELASZ, &relasz)) {
        if (!apply_table(rel, relasz)) { g_lib_reloc_step = 7; return 0; }
    }
    if (!apply_table(jmprel, pltrelsz)) { g_lib_reloc_step = 8; return 0; }
    return 1;
}

void* ldso_lookup(const char* name) {
    if (!g_lib.loaded) {
        return 0;
    }
    const Elf64_Addr value = resolve(name);
    if (value == 0) {
        return 0;
    }
    void* at = at_vaddr(value);
    if (at == 0) {
        return 0;
    }
    return at;
}
