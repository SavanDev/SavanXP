/* See ldso.h. All the work is pointer arithmetic over an image already mapped,
 * plus copying the writable segments.
 *
 * The loader speaks SXN, not POSIX (docs/kernel/SHARED_LIBRARIES.md, "One
 * loader over SXN"): it opens, reads and maps files with the native object
 * calls, which every subsystem reaches, and calls no C library. What it needs
 * from the program's runtime is two things: sx_image_base, which crt0 stores,
 * and sx_loader_report, through which each runtime says in its own terms why a
 * library did not load. The loader never ends the process; it returns a step
 * and the program goes on. <dlfcn.h>, at the end, is the POSIX layer over it. */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <savanxp/ldso.h>
#include <dlfcn.h>

#include "sxn/sxn.h"

/* From the runtime (savanxp/libc.h). Declared here rather than by including
 * the C library's header, so a call into it cannot slip back in unnoticed. */
extern unsigned long sx_image_base;
void sx_loader_report(const char* text, unsigned long length);
long savanxp_getcwd(char* buffer, size_t count);

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
/* Indice de seccion reservado: marca un simbolo que esta declarado pero no
 * definido aqui. */
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
/* Constructors. Only dlopen runs them; see run_initializers. */
#define DT_INIT 12
#define DT_INIT_ARRAY 25
#define DT_INIT_ARRAYSZ 27

#define STT_OBJECT 1
#define STT_FUNC 2

#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_GLOB_DAT 6
/* El valor guardado ya es una direccion de enlace, sin simbolo: hay que
 * sumarle el bias de la imagen. Es lo que hace que un ejecutable PIE con
 * punteros a datos -- una tabla de vtables, una cadena constante, un GOT
 * inicial -- arranque con las direcciones donde el codigo las busca. Una
 * imagen reubicada sin esto tiene punteros al valor cero. */
#define R_X86_64_RELATIVE 8
/* Direccion absoluta de 64 bits de un simbolo IMPORTADO, guardada en datos. Es lo
 * mismo que un GLOB_DAT --resolver por nombre y escribir el valor-- y aparece
 * cuando el codigo hace &simbolo y lo guarda en una tabla. */
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

/* Cuantos program headers se aceptan por imagen, y por lo tanto cuantos caben en el
 * struct de abajo. Es el techo del hueco: cada libreria guarda sus headers en un
 * array de este tamano, asi que subir kMaxLibraries sin bajar esto multiplica la
 * memoria de todos los procesos por nada.
 *
 * Medido sobre todo lo que produce este arbol, el maximo e_phnum es 15. Veinticuatro
 * deja margen para un segmento mas sin convertir un rechazo raro en algo normal. Y el
 * limite se comprueba ANTES de copiar, asi que una imagen con mas headers se rechaza
 * con un motivo en vez de desbordar el array en silencio.
 *
 * Bajarlo de 64 es lo que hace barato el numero de huecos de mas abajo: el hueco pasa
 * de 4300 B a unos 1750, y treinta y dos huecos de 137 KB se vuelven 56 KB. */
#define kMaxProgramHeaders 24

/* El estado de UNA libreria cargada.
 *
 * Antes habia una sola, y por eso esto era un unico objeto global con un
 * nombre. Con una cadena de DT_NEEDED deja de alcanzar: cargar una dependencia
 * pisa el estado de la que se estaba cargando, asi que hace falta un lugar por
 * libreria y un puntero a la que esta en curso. */
typedef struct {
    int loaded;
    /* The file's handle and its whole-file section, or -1. They live only
     * while the image is being loaded; 0 is a handle like any other -- a
     * native process may have no standard input, so its first open gets 0. */
    long file_section;
    long file_fd;
    /* Whether the two above are still open, so closing does not repeat. */
    int handles_open;
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
    /* Cuantos DT_NEEDED ya se trajeron. Es el cursor de la cadena: con un
     * diamante, D se carga desde A y desde C, y el segundo next_needed tiene
     * que saltar el que ya se proceso en vez de volver a cargarlo. */
    unsigned needed_done;
    /* The name it was loaded under, to recognise it if another DT_NEEDED asks
     * for it again in the same chain. It is the basename: DT_NEEDED carries
     * "libmath.so.0.4" even though the path is /lib/libmath.so.0.4. */
    char soname[96];
    /* Los program headers se leen una vez y se guardan aca: despues el
     * descriptor sigue abierto pero no se vuelve a leer el archivo. */
    Elf64_Phdr headers[kMaxProgramHeaders];
    /* Direccion final de cada segmento, en el mismo orden que los program
     * headers. Los indices son los de los headers, asi que un -1 es "este
     * segmento no se pudo mapear". */
    void* placed[kMaxProgramHeaders];
    /* Whether this image answers lookups from images outside its own dependency
     * tree. Everything loaded at startup or through ldso_load is global, which is
     * what the loader always did; dlopen without RTLD_GLOBAL is not. */
    int global;
    /* dlopen references not yet dropped by dlclose. Images brought in at startup
     * or as a dependency stay at zero: nothing can close them. */
    unsigned refcount;
    /* The slots of this image's direct DT_NEEDED, one bit each. Its lookup scope
     * is itself plus the closure of these. */
    uint64_t deps;
    /* Entries in .dynsym, from DT_HASH or DT_GNU_HASH, or 0 when the image has
     * neither and the end of the table has to be guessed. */
    unsigned long symbol_count;
    /* The path dlopen was given or DT_NEEDED resolved to, so a second dlopen of
     * the same file finds this image, and dladdr has a name to report. */
    char path[128];
} Library;

/* Cuantas librerias conviven. El ejecutable ocupa el hueco 0.
 *
 * La cadena mas larga realista del arbol es la de mediaplayer: el ejecutable, sus
 * dos librerias de interfaz y, cuando FFmpeg deje de ser estatico, libavcodec +
 * libavutil + las de escala y remuestreo. Eso son siete, y con ocho estabamos a un
 * hueco del muro -- un muro queributed descubriria la integracion de FFmpeg y no
 * nosotros. Treinta y dos deja sitio para un segundo codec; mas alla ya no hay nada
 * que justifique el numero.
 *
 * Subirlo solo es barato porque el hueco se abarata: a 4300 B por hueco, treinta y
 * dos costarian 137 KB residentES en cada proceso, incluido un ls que no carga
 * ninguna. Con headers[] de 24 son ~1750 B y el total baja a 56 KB. Los dos cambios
 * van juntos por eso. */
#ifndef SAVANXP_LD_MAX_LIBRARIES
#define SAVANXP_LD_MAX_LIBRARIES 32
#endif
#define kMaxLibraries SAVANXP_LD_MAX_LIBRARIES
/* Library.deps is a 64-bit mask indexed by slot. */
_Static_assert(kMaxLibraries <= 64, "Library.deps holds one bit per slot");

static Library g_libs[kMaxLibraries];
static int g_lib_count;
/* What a new image gets as Library.global. dlopen lowers it for the length of
 * one call; everything else loads global. */
static int g_load_global = 1;
/* Set while dlopen runs. An unresolvable symbol then fails the load instead of
 * being skipped: the tolerant mode exists so a program can start and report a
 * missing dependency, and a dlopen caller has dlerror for that. */
static int g_strict = 0;
/* La dependencia cuyo DT_NEEDED no se pudo traer. Ver ldso_missing(). */
static const char* g_missing_library = 0;
/* Las dependencias DISTINTAS que no se pudieron traer, con su nombre.
 *
 * Distintas, y no un contador de intentos: quitar libsxgfx.so.0.4 hace fallar la
 * carga de libffmpeg, de libgfx2d y de libsxgui, porque las tres la necesitan, y
 * cada una reintenta. Con un contador de intentos el reproductor llegaba a decir
 * "faltan 6 librerias" cuando faltaban dos, y un numero que miente es peor que
 * ninguno. El nombre de la primera va aparte porque es lo que se enseña. */
static char g_missing_names[kMaxLibraries][64];
static unsigned g_missing_count = 0;
/* El ejecutable ocupa el slot 0 y las librerias empiezan en el 1.
 *
 * resolve recorre los slots de la mas nueva a la mas vieja, asi que poner el
 * ejecutable primero lo consulta AL ULTIMO. Ese orden es el del ambito de
 * simbolos: una libreria gana sobre la que la cargo, y el ejecutable queda de
 * fondo, que es donde un simbolo exportado por el programa va a caer. */
/* La libreria que se esta cargando. Todo lo que se escribe mientras se coloca
 * una imagen va aca; el resto del arbol lo lee por indice. */
static Library* g_lib;

/* Ultimo segmento que no se pudo colocar, y el errno que devolvio. El codigo de
 * retorno de ldso_load solo dice "el paso"; con estos dos se sabe cual. */
int g_lib_fail_index;
long g_lib_fail_errno;
unsigned long g_lib_bias;
int g_lib_reloc_step;
/* Que simbolo y de que libreria fallaron. El paso dice donde; esto dice que.
 * El puntero apunta a la .dynstr de la libreria, que queda mapeada, asi que no
 * hay copia que hacer ni que se pueda invalidar. */
const char* g_lib_fail_symbol;
const char* g_lib_fail_library;

/* La dependencia que no se pudo cargar, por nombre, o 0 si no fallo ninguna.
 *
 * Existe para que un programa pueda DECIR que le falta en vez de morirse. El
 * cargador no es fatal: sx_start_dynamic imprime el fallo y vuelve, y crt0 llama a
 * main igual, asi que un programa con un DT_NEEDED irresoluble arranca. Lo que no
 * podia era enterarse: ldso_loaded() devuelve g_lib_count, que es al menos 1 porque
 * el ejecutable ocupa el hueco 0, asi que responde "si" aunque falte una
 * dependencia. Eso era un accidente, no una interfaz.
 *
 * Ojo con lo que significa: "esta dependencia no se pudo cargar", no "falta". Un
 * archivo presente y roto tambien deja el nombre aqui, y en ese caso decir que
 * falta seria mentira. El motivo fino esta en el mensaje que el cargador ya
 * imprimio, con su paso y su razon.
 *
 * No se limpia nunca. Un fallo en el arranque es permanente, y el programa lo
 * reporta cuando abre su ventana, no antes. */
const char* ldso_missing(void) {
    return g_missing_library;
}

unsigned ldso_missing_count(void) {
    return g_missing_count;
}

/* Cuantas imagenes hay mapeadas, el ejecutable incluido. Existe para las pruebas:
 * sin el, la unica forma de comprobar que una dependencia compartida se cargo UNA
 * vez -- el caso del diamante-- seria contar ficheros.
 *
 * El ejecutable cuenta porque ocupa un hueco, y porque el limite incluye su
 * hueco: con kMaxLibraries 32 hay 31 huecos para librerias. */
int ldso_count(void) {
    return g_lib_count;
}

int ldso_loaded(void) {
    return g_lib_count;
}

/* Los nombres de los simbolos y los PT_LOAD se leen del archivo a traves de la
 * vista que se mapeo para eso. Se mantiene aparte del mapa final porque al final
 * el texto vive en la direccion que pidio el ELF, no en la que el kernel eligio. */
static void copy_bytes(const void* from, void* to, size_t count);
/* El orden del archivo es el del razonamiento --validar, ubicar, colocar,
 * reubicar-- y no el de las dependencias entre funciones. */
static int read_header(const unsigned char* bytes, Elf64_Ehdr* out);
static void* place_segment(const Elf64_Phdr* ph);
static void* map_of(int slot, Elf64_Addr vaddr);
static int read_dynamic_in(int slot);
static int find_dynamic_in(int slot, Elf64_Sword wanted, void* out);
static void* bias_in(int slot, Elf64_Addr value);
static int resolve(const char* name, Elf64_Addr* value_out);
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

/* Registra el ejecutable en g_libs para que sus simbolos entren en el ambito.
 *
 * El cargador no mapeo esta imagen: la mapeo el kernel. Asi que no hay un
 * descriptor ni una seccion que abrir, y la base la tiene que decir el kernel.
 * Llega en r8 y crt0 la guarda en sx_image_base.
 *
 * ANTES se adivinaba. Se tomaba la direccion de una funcion de este mismo
 * archivo --que por definicion esta en la imagen-- y se retrocedia pagina a
 * pagina buscando la magia de ELF. El problema no es que la cuenta este mal: es
 * que supone que TODA pagina del camino esta mapeada, y un enlazador puede dejar
 * un hueco entre dos PT_LOAD. seltest lo tiene: su primer segmento termina en
 * 0xaff3 y el texto arranca en 0xc000, asi que la pagina 0xb000 no existe. El
 * escaneo la leia y el proceso moria con cr2 = 0x40b000 antes de llegar a main.
 *
 * Un escaneo que puede hacer fault no es un escaneo. Con el dato del kernel no
 * hay recorrido, y no hay forma de que la cuenta falle.
 *
 * Devuelve 0 si no hay cabecera valida, y el ambito queda como estaba: resolver
 * solo entre librerias. Un ejecutable sin .dynsym (una ET_EXEC no PIE) es un caso
 * normal, no un fallo. */
static int adopt_executable(void) {
    if (g_lib_count >= kMaxLibraries) {
        return 0;
    }
    const unsigned long cursor = sx_image_base & ~4095UL;
    if (cursor == 0) {
        /* Sin base no hay nada que registrar. Preferible decirlo a leer
         * direcciones que pueden no existir. */
        return 0;
    }

    Elf64_Ehdr header;
    const unsigned char* bytes = (const unsigned char*)cursor;
    if (bytes[0] != 0x7f || bytes[1] != 'E' || bytes[2] != 'L' || bytes[3] != 'F') {
        return 0;
    }
    copy_bytes(bytes, &header, sizeof(header));
    /* ET_DYN o ET_EXEC: cualquier otra cosa no es una imagen de programa. Y
     * e_phnum tiene que caber, porque de ahi salen los segmentos que se usan
     * para traducir direcciones. */
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
    /* Los segmentos ya estan mapeados: el kernel los puso, y el cargador no
     * tiene descriptor ni seccion con que mapearlos otra vez. placed[] se deduce
     * de la geometria de la imagen.
     *
     * El bias sale de la base que dio el kernel y del primer PT_LOAD: lo que
     * hay entre donde quedo la cabecera y donde la imagen dice que empieza su
     * primer segmento. Para una ET_DYN, que carga en kUserBase, da kUserBase;
     * para una ET_EXEC, que carga en sus propias direcciones de enlace, da cero.
     *
     * Antes estaba escrito como una constante --kUserBase para una ET_DYN-- con
     * el comentario de que tiene que coincidir con kernel/elf.cpp. Coincidia, y
     * por eso el bug no se veia. Con los dos numeros a la vista la cuenta es
     * explicita y no puede desincronizarse sola. */
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

/* El ejecutable ocupa el slot 0 desde el arranque y las librerias empiezan en 1,
 * para que resolve lo consulte al ultimo. */
static void adopt_executable_once(void) {
    static int done = 0;
    if (done) {
        return;
    }
    done = 1;
    (void)adopt_executable();
}

/* Reserva un slot y lo deja como la libreria en curso. El contenido del slot no
 * se limpia: cada carga escribe todos los campos que usa, y limpiarlo seria
 * otra pasada sobre lo mismo. */
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

/* Se declara antes de end_load, que la usa: end_load suelta el puntero a la
 * libreria en curso, y con el puntero no hay forma de cerrarle nada. */
static void close_file_handles(void);

/* Unmaps every segment an image placed. For a load that failed before it was
 * published, and for dlopen's rollback of one that failed after. */
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

/* Hace visible la libreria en curso sin soltar el puntero a ella.
 *
 * end_load no sirve para el punto donde hace falta: reubicar una libreria
 * depende de que sus DT_NEEDED ya estan cargadas, asi que esta tiene que estar
 * publicada antes de que se recorra la cadena. Publicar y seguir guardando el
 * puntero es lo que permite que, al volver de la cadena, las reubicaciones
 * siganmaginiendo sobre esta y no sobre la ultima dependencia traida. */
static void publish(void) {
    g_lib->loaded = 1;
    g_lib_count++;
}

/* Aplica una tabla RELA: .rela.dyn y .rela.plt. Las dos son obligatorias antes
 * de llamar a nada, asi que no hay version perezosa todavia. Solo estan
 * soportados JUMP_SLOT y GLOB_DAT, que son los que emite lld para este
 * objetivo. */
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

static int apply_table_in(int slot, unsigned long elf_table, unsigned long elf_size) {
    /* Con una dependencia ausente, la imagen se reubica IGUAL Y LA ENTERA.
     *
     * Antes se abortaba en el primer simbolo que no se podia resolver, y como la
     * cadena se recorre ANTES de reubicar, un solo DT_NEEDED ausente dejaba el
     * ejecutable entero sin reubicar: main() corria con stdout --un puntero global
     * reubicado por R_X86_64_RELATIVE-- apuntando al valor de enlace. El programa no
     * podia ni imprimir por que estaba roto, que es justo lo que un fallo
     * diagnosticable tiene que poder hacer.
     *
     * Asi que en modo tolerante las entradas irresolubles se saltan y se cuentan. El
     * nombre de lo que falta ya lo da ldso_missing(), y repetirlo una vez por cada
     * av_* del programa seria ruido. */
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
            /* Sin simbolo que buscar. Lo que hay que escribir es
             * r_addend + bias, y NO sumar el bias a lo que ya esta en memoria.
             *
             * La diferencia no es academica y cuesta un bug entero: lld deja la
             * casilla EN CERO y pone el valor de enlace en r_addend. Sumarle el
             * bias a un cero deja base + 0, que es un puntero a la propia base
             * de la imagen. Con `stdout` eso daba 0x400000 y la primera
             * escritura a pantalla moria con cr2 = 0x40000c.
             *
             * Leer la memoria en vez de la adenda solo funciona con enlaces que
             * escriben tambien el valor, y SavanXP se enlaza con lld, que no. */
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
         * scope, so a library dlopened without RTLD_GLOBAL cannot capture the
         * calls of one loaded after it. */
        Elf64_Addr target = 0;
        const int owner = resolve_for(slot, g_libs[slot].dynstr + symbol.st_name, &target);
        if (owner < 0) {
            if (tolerant) {
                skipped++;
                continue;
            }
            /* El nombre es lo unico que hay. Sin el, el sintoma es "paso 5" y la
             * unica manera de sacar algo es un volcado de la .dynsym. */
            report_unresolved(image_name(slot), g_libs[slot].dynstr + symbol.st_name);
            return 0;
        }
        const Elf64_Addr value = (Elf64_Addr)(unsigned long)bias_in(owner, target);
        if (value == 0) {
            /* Se encontro la imagen pero la direccion no se puede traducir: un
             * st_value que cae en un segmento que no quedo mapeado. Distinto del
             * caso de arriba porque el nombre SI existe, y por eso el mensaje
             * tiene que decir las dos cosas. */
            if (tolerant) {
                skipped++;
                continue;
            }
            report_unplaced(g_libs[slot].dynstr + symbol.st_name, owner);
            return 0;
        }
        Elf64_Addr* where = (Elf64_Addr*)map_of(slot, rela.r_offset);
        if (where == 0) {
            /* El GOT de esta entrada no esta en ningun segmento mapeado. Un
             * error de la imagen, no del enlazado. */
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

/* Fase uno: leer donde estan la tabla de simbolos y la de nombres.
 *
 * Va separada de la reubicacion porque leer DT_NEEDED tambien necesita
 * dynstr, y DT_NEEDED hay que recorrer antes de reubicar: una llamada a la
 * dependencia se resuelve en un GOT que todavia esta en cero. Si se reubicara
 * primero, la cadena se resolveria contra un mundo incompleto. */
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
 * table usually looks like that -- a dlsym that misses (and Xash probes for
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

/* Las dos tablas son OPCIONALES. Una libreria que no llama a nada de fuera no
 * tiene .rela.plt, y exigirla la hacia fallar al cargar: el fallo no era de la
 * reubicacion sino de una tabla que no existe. */
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

/* Prepara el ejecutable para llamar a una libreria.
 *
 * El kernel mapeo la imagen principal pero NO toco su GOT: deja el proceso
 * corriendo con un ejecutable que declara DT_NEEDED y tiene entradas sin
 * rellenar. Por eso hace falta este paso, y por eso el programa tiene que
 * llamarlo ANTES de usar cualquier simbolo de libreria.
 *
 * El orden es el mismo que para una libreria: primero la cadena de DT_NEEDED del
 * ejecutable, y despues sus propias reubicaciones. Al reves, una llamada a una
 * dependencia se resolveria contra una libreria todavia no mapeada.
 *
 * Devuelve 0 si el ejecutable quedo operativo, o un numero negativo dizendo en
 * que paso fallo, con la misma convencion que ldso_load. Un ejecutable sin
 * .dynsym no tiene nada que reubicar y devuelve 0. */
int ldso_start(void) {
    /* Idempotente a proposito: crt0 lo llama antes de main y un programa puede
     * llamarlo otra vez. Reaplicar las reubicaciones del ejecutable escribiria
     * los mismos valores, pero R_X86_64_RELATIVE SUMA el bias, y hacerlo dos
     * veces dejaria los punteros de la imagen corridos por kUserBase. */
    static int done = 0;
    if (done) {
        return 0;
    }
    done = 1;
    adopt_executable_once();
    if (g_lib_count == 0) {
        /* Sin la imagen no hay nada que reubicar, pero una imagen ET_DYN SI
         * necesita que la reubiquen. Devolver 0 aqui seria mentir: el programa
         * arranca con punteros sin sumar el bias y falla mas tarde, en un sitio
         * que no dice nada de esto. Un programa que de verdad no depende de
         * nada no llega aca, porque no llega a enlazar el cargador. */
        return -10;
    }
    if (g_libs[0].dynsym == 0) {
        return -11;
    }
    /* La cadena se recorre antes que la reubicacion porque un GOT que apunta a una
     * dependencia tiene que llenarse cuando esa dependencia ya esta mapeada. Pero si
     * la cadena falla, la reubicacion SE HACE IGUAL y el fallo se devuelve despues.
     *
     * Volver aqui no deja la imagen como estaba: deja el ejecutable entero sin
     * reubicar, con stdout y el resto de punteros globales apuntando a sus valores
     * de enlace. El programa arranca y no puede ni imprimir el motivo. Que el fallo
     * sea no mortal no significa que la imagen pueda quedar a medias. */
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

/* El hook que crt0 llama antes de main.
 *
 * El nombre sigue el del campo que ya existia, sx_interpreter_path, y el modelo
 * es el que el kernel ya impone: la imagen principal la mapea el kernel y el
 * programa hace el trabajo de enlace. Asi que "correr el interprete" aca es
 * dejar operativo al ejecutable: cargar lo que declara y rellenar su GOT. */
int sx_run_interpreter(void) {
    return ldso_start();
}

int ldso_load(const char* path) {
    /* El codigo de retorno dice EN QUE PASO fallo, para que un fallo no sea un
     * -1 opaco: los pasos van en orden y el ultimo que se alcanzo a hacer es el
     * que importa. */
    if (begin_load() == 0) {
        /* Not "could not load": there are already 32 and one more does not fit.
         * The step number does not tell the two apart, and the cure differs:
         * one is a chain too long, the other a file that is not there. */
        report("loader: no room for another library, the limit is %d and there are %d\n",
               kMaxLibraries, g_lib_count);
        return -1;
    }
    /* The path is absolute: the native call knows no working directory, and
     * every path the loader builds starts at /lib. dlopen makes a relative
     * one absolute before it gets here. */
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
    /* El nombre se queda con el basename del path, para casar con DT_NEEDED. */
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

    /* Publicar, recorrer la cadena, y recien ahi reubicar. El orden no es
     * estetico: un GOT que apunta a una dependencia tiene que llenarse cuando
     * la dependencia ya esta mapeada, no antes. Al reubicar antes, la
     * dependencia todavia no existe y la carga falla.
     *
     * Cada carga de la cadena pisa g_lib, asi que se guarda esta libreria y se
     * restaura al volver. Sin eso, las reubicaciones de aqui se aplicarian
     * sobre la ultima dependencia traida. */
    const int slot = (int)(g_lib - g_libs);
    Library* self = g_lib;
    publish();
    const int chain_failed = load_needed_chain(g_lib_count - 1);
    g_lib = self;
    /* El cierre va ANTES de soltar el puntero: close_file_handles trabaja sobre
     * g_lib, asi que ponerlo a cero primero lo dejaria sin hacer nada. */
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
                /* Se guarda el NOMBRE, no solo el codigo de paso. El codigo dice
                 * donde fallo --y donde fallo es "en algun punto de la cadena"--
                 * y el nombre dice que falta. Sin el, un programa no puede ni
                 * distinguir "no esta instalado" de "esta roto" ni decir cual.
                 *
                 * Queda el fallo MAS PROFUNDO, porque la recursion llega aqui desde
                 * la libreria que fallo y la sobreescribe: si top -> left -> leaf y
                 * falta leaf, el nombre guardado es el de leaf, que es el que de
                 * verdad falta. Con varias ausentes el nombre guardado es el de la
                 * ULTIMA que fallo, que es arbitrario; el recuento de lo que quedo
                 * sin reubicar dice cuanto falta, no cual.
                 *
                 * `need` es un basename --DT_NEEDED lo es-- y el registro es el
                 * mismo buffer, asi que no hay copia que hacer. */
                /* El PRIMERO que falla, no el último.
                 *
                 * Con el último, el nombre dependía del orden del DT_NEEDED y de qué
                 * recursion hubo, así que dos imágenes con las mismas tres librerías ausentes
                 * podían nombrar tres librerías distintas. Con el primero, el nombre
                 * es "la primera dependencia que no se pudo traer", que es una
                 * definición y no un accidente del recorrido.
                 *
                 * Y se cuentan todos, que es lo que permite al programa decir que
                 * faltan tres en vez de insinuar que falta una. */
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
    /* Una dependencia que falta NO puede terminar el recorrido.
     *
     * El DT_NEEDED del ejecutable es una lista, no una cadena: si libffmpeg no esta,
     * eso dice algo de libffmpeg y nada de libsxgui, libgfx2d o libsxgfx, que son
     * las siguientes de la lista y las necesita igual. Volver aqui las dejaba sin
     * cargar a TODAS, y con la reubicacion tolerante --que salta lo que no resuelve
     * en vez de abortar-- el programa arranco con las cuatro ausentes y se mato en la
     * primera llamada al toolkit, en un salto a la casilla sin reubicar de
     * sx_rect_make@plt. Un fallo mudo y sin relacion con su causa: lo peor de los dos
     * mundos.
     *
     * Se sigue informando del fallo, y por eso se devuelve != 0 igual, pero despues
     * de haber cargado todo lo que si se puede. La diferencia entre esto y lo de
     * antes es la que hay entre "no esta libffmpeg" y "no esta nada". */
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


/* Traduce una direccion del ELF a la direccion real. Busca primero en la
 * libreria en curso y despues en las ya cargadas, porque una direccion de una
 * .so solo tiene sentido dentro de la libreria que la declaro: dos imagenes
 * tendrian el mismo vaddr para direcciones distintas.
 *
 * Con una sola libreria esto era un recorrido de sus segmentos y nada mas. Con
 * una cadena, es la pregunta que no tenia respuesta antes: cual libreria
 * posee esta direccion. */

/* El mismo recorrido pero solo dentro de la libreria en curso, que es lo que
 * necesitan place_segment y translate mientras se arma una imagen. */
/* Traduce un st_value con los segmentos de una libreria concreta.
 *
 * Existe separada de at_vaddr_here porque un st_value pertenece a la libreria
 * que DEFINIO el simbolo, no a la que esta usandolo. Sesgarlo con el mapa de
 * otra imagen produce una direccion que cae dentro de la imagen equivocada y no
 * falla de forma visible: cae en codigo ajeno. */
static void* bias_in(int slot, Elf64_Addr value) {
    return map_of(slot, value);
}

/* Traduce una direccion del ELF a la direccion real, dentro de una libreria
 * concreta.
 *
 * placed[] guarda la DIRECCION DE PAGINA donde se mapéo el segmento, o sea
 * bias + (p_vaddr & ~4095). Restar p_vaddr sin alinear correria la direccion
 * justo lo que el segmento seSpecified antes de la pagina, y el error cae
 * dentro de la propia imagen: un GOT escrito en el lugar equivocado, una
 * llamada que salta a otra cosa y un fallo que no parece de este sitio. */
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

/* Recorre la tabla dinamica y deja el puntero a la entrada pedida. Las entradas
 * tienen tamaño fijo, asi que el indice se multiplica por sizeof(Elf64_Dyn). */
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


/* El nombre del primer DT_NEEDED que aun no se cargo, o 0 si no queda ninguno.
 *
 * El indice de recorrido es lo que permite no cargar dos veces la misma
 * dependencia en una cadena con diamantes: si A necesita B y C, y ambos
 * necesitan D, D se carga una sola vez y el segundo la encuentra.
 *
 * El nombre sale de esa misma libreria, traducido con SU mapa, no con el de
 * la que este en curso: con dos imagenes mapeadas, el mismo vaddr significa dos
 * cosas distintas.
 *
 * slot < 0 es la libreria en curso. */
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
        /* El PT_DYNAMIC esta dentro de un PT_LOAD; se traduce con los segmentos
         * ya colocados de ESTA libreria, no con los de la que este en curso. */
        const unsigned char* table = (const unsigned char*)map_of(slot, ph->p_vaddr);
        if (table == 0) {
            return 0;
        }
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
        /* Una entrada INDEFINIDA con el nombre buscado no es una respuesta: es
         * la pregunta misma. Si se aceptara, st_value = 0 se traduciria a la
         * base de la imagen, y una llamada terminaria en el encabezado ELF.
         * Solo contesta quien DEFINIO el simbolo. */
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
 * and the program is the fallback -- and dlsym(RTLD_DEFAULT) keeps it. */
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
 * own dependency tree, in the same newest-first order. Before dlopen every image
 * was global, so this is the same walk as resolve() for everything loaded at
 * startup. The difference is for a library dlopened RTLD_LOCAL: it can no
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
    /* El valor es relativo a la libreria donde se encontro, asi que se traduce
     * con SU mapa y no con el de la ultima cargada: con dos imagenes mapeadas,
     * un vaddr igual en las dos son direcciones distintas. */
    return map_of(slot, value);
}

/* El ejecutable es el slot 0, asi que "vino de una libreria compartida" es "el
 * slot donde se encontro no es el cero".
 *
 * La pregunta es mas util de lo que parece. Sin ella, un programa que todavia
 * trae su propia copia del simbolo en el binario pasa la misma comprobacion,
 * porque en los dos casos la direccion que reporta el resolvedor coincide con la
 * que tiene el GOT. */
int ldso_symbol_is_shared(const char* name) {
    Elf64_Addr value = 0;
    return resolve(name, &value) > 0;
}

/* ---- <dlfcn.h> ------------------------------------------------------------
 *
 * Built on the same slots as everything above. A handle is a pointer to the
 * image's slot; the executable's slot stands for the global scope. What is
 * deliberately missing and why is in SHARED_LIBRARIES.md#dlopen. */

/* The last failure, until dlerror() hands it out once. */
static char g_dl_error[256];
static int g_dl_error_pending = 0;

static void dl_error_append(size_t* used, const char* text) {
    while (text != 0 && *text != '\0' && *used < sizeof(g_dl_error) - 1) {
        g_dl_error[(*used)++] = *text++;
    }
    g_dl_error[*used] = '\0';
}

static void dl_fail(const char* first, const char* second, const char* third) {
    size_t used = 0;
    dl_error_append(&used, first);
    dl_error_append(&used, second);
    dl_error_append(&used, third);
    g_dl_error_pending = 1;
}

char* dlerror(void) {
    if (!g_dl_error_pending) {
        return 0;
    }
    g_dl_error_pending = 0;
    return g_dl_error;
}

/* The slot a handle names, or -1 when it is not one this loader gave out. */
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

/* RTLD_GLOBAL applies to the image and its whole tree, as on Linux: a symbol
 * reachable through the library becomes reachable by everyone. */
static void make_global(int slot) {
    const uint64_t scope = scope_of(slot);
    for (int index = 0; index < g_lib_count; ++index) {
        if ((scope & ((uint64_t)1 << index)) != 0) {
            g_libs[index].global = 1;
        }
    }
}

/* Puts the slot table back to how it was before a dlopen that failed.
 *
 * ldso_load publishes an image before walking its DT_NEEDED and relocating it,
 * so a failure at either step leaves it -- and any dependency it brought -- in
 * the table, mapped and visible to every later lookup. That is acceptable at
 * startup, where the program reports the failure and goes on; for dlopen it
 * would mean a library that "failed to load" still answers dlsym(RTLD_DEFAULT).
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
 * Only dlopen calls this. Nothing at startup runs constructors -- not crt0 for
 * the program, not the loader for its DT_NEEDED -- because nothing in this tree
 * has any; a C++ module opened at run time, like Xash3D's client.so with its
 * global CHud, is the first thing that does. */
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

static const char* dl_step_reason(int step) {
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

void* dlopen(const char* path, int mode) {
    adopt_executable_once();
    if (path == 0) {
        if (g_lib_count == 0) {
            dl_fail("dlopen: the program image is not registered", 0, 0);
            return 0;
        }
        g_libs[0].refcount++;
        return &g_libs[0];
    }

    /* A bare name is looked up where DT_NEEDED is; anything with a '/' is a
     * path, relative ones included. The loader opens absolute paths only (the
     * native call knows no working directory), so a relative one is made
     * absolute here against the POSIX one: this is the POSIX layer. */
    char full[sizeof(g_libs[0].path)];
    char cwd[sizeof(g_libs[0].path)];
    size_t used = 0;
    int has_slash = 0;
    for (const char* ch = path; *ch != '\0'; ++ch) {
        if (*ch == '/') {
            has_slash = 1;
            break;
        }
    }
    const char* parts[3] = {has_slash ? "" : "/lib/", "", path};
    if (has_slash && path[0] != '/') {
        if (savanxp_getcwd(cwd, sizeof(cwd)) < 0 || cwd[0] != '/') {
            dl_fail("dlopen: no working directory for ", path, 0);
            return 0;
        }
        size_t length = 0;
        while (cwd[length] != '\0') {
            length++;
        }
        parts[0] = cwd;
        parts[1] = cwd[length - 1] == '/' ? "" : "/";
    }
    for (int part = 0; part < 3; ++part) {
        for (const char* ch = parts[part]; *ch != '\0'; ++ch) {
            if (used >= sizeof(full) - 1) {
                dl_fail("dlopen: path too long: ", path, 0);
                return 0;
            }
            full[used++] = *ch;
        }
    }
    full[used] = '\0';

    int slot = loaded_slot_at(full);
    if (slot >= 0) {
        if ((mode & RTLD_GLOBAL) != 0) {
            make_global(slot);
        }
        g_libs[slot].refcount++;
        return &g_libs[slot];
    }
    if ((mode & RTLD_NOLOAD) != 0) {
        return 0;
    }

    /* ldso_missing() describes the program's startup and must not change
     * because of a dlopen, so its state is put back however this ends. */
    const int start = g_lib_count;
    const char* missing_before = g_missing_library;
    const unsigned missing_count_before = g_missing_count;
    g_lib_fail_symbol = 0;
    g_lib_fail_library = 0;
    g_load_global = (mode & RTLD_GLOBAL) != 0;
    g_strict = 1;
    const int step = ldso_load(full);
    g_strict = 0;
    g_load_global = 1;

    if (step != 0) {
        if (step == -8 && g_missing_count > missing_count_before) {
            dl_fail(full, ": a dependency failed to load: ", g_missing_names[missing_count_before]);
        } else if (step == -9 && g_lib_fail_symbol != 0) {
            size_t length = 0;
            dl_error_append(&length, full);
            dl_error_append(&length, ": unresolved symbol \"");
            dl_error_append(&length, g_lib_fail_symbol);
            dl_error_append(&length, "\" needed by ");
            dl_error_append(&length, g_lib_fail_library);
            g_dl_error_pending = 1;
        } else {
            dl_fail(full, ": ", dl_step_reason(step));
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
    slot = start;
    if ((mode & RTLD_GLOBAL) != 0) {
        /* Dependencies that were already loaded RTLD_LOCAL become global too. */
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

void* dlsym(void* handle, const char* name) {
    if (name == 0) {
        dl_fail("dlsym: no symbol name", 0, 0);
        return 0;
    }
    Elf64_Addr value = 0;
    int owner = -1;
    if (handle == RTLD_DEFAULT || handle == &g_libs[0]) {
        owner = resolve(name, &value);
    } else {
        const int slot = slot_of_handle(handle);
        if (slot < 0) {
            dl_fail("dlsym: invalid handle", 0, 0);
            return 0;
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
    void* address = owner >= 0 ? map_of(owner, value) : 0;
    if (address == 0) {
        dl_fail("dlsym: symbol not found: ", name, 0);
        return 0;
    }
    return address;
}

int dlclose(void* handle) {
    const int slot = slot_of_handle(handle);
    if (slot < 0 || g_libs[slot].refcount == 0) {
        dl_fail("dlclose: handle is not open", 0, 0);
        return -1;
    }
    /* The image stays mapped: see the header. */
    g_libs[slot].refcount--;
    return 0;
}

/* Which image holds an address, and the nearest symbol at or below it.
 *
 * Xash3D's save/restore turns function pointers into names this way. A symbol
 * matches when the address falls inside it; one with no size matches only its
 * own address. Of the matches, the highest start wins. */
int dladdr(const void* address, Dl_info* info) {
    if (info == 0) {
        return 0;
    }
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
        info->dli_fname = library->path;
        info->dli_fbase = (void*)(library->bias + (first & ~(Elf64_Addr)4095));
        info->dli_sname = 0;
        info->dli_saddr = 0;
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
            if (!covers || (info->dli_sname != 0 && symbol.st_value < best)) {
                continue;
            }
            best = symbol.st_value;
            info->dli_sname = library->dynstr + symbol.st_name;
            info->dli_saddr = map_of(slot, symbol.st_value);
        }
        return 1;
    }
    return 0;
}
