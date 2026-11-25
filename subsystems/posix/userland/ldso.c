/* Ver ldso.h. Todo el trabajo es aritmetica de punteros sobre una imagen ya
 * mapeada, mas la copia de los segmentos escribibles. */

#include <savanxp/ldso.h>

#include "libc.h"

// ELF32 REL entries store their addend at the relocation destination.
typedef uint16_t Elf_Half;
typedef uint32_t Elf_Word;
typedef int32_t Elf_Sword;
typedef uint32_t Elf_Addr;
typedef uint32_t Elf_Off;
typedef uint32_t Elf_Xword;
#define ELF_CLASS 1
#define ELF_MACHINE 3
#define DT_RELOC 17
#define DT_RELOCSZ 18
#define ELF_R_TYPE(info) ((info) & 255u)
#define ELF_R_SYM(info) ((info) >> 8)
#define EI_NIDENT 16
#define ET_DYN 3
#define ET_EXEC 2
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PF_X 1
#define PF_W 2
#define PF_R 4
#define DT_NULL 0
#define SHN_UNDEF 0
#define DT_NEEDED 1
#define DT_PLTRELSZ 2
#define DT_STRTAB 5
#define DT_STRSZ 10
#define DT_SYMTAB 6
#define DT_JMPREL 23
#define DT_PLTREL 20
#define R_JUMP_SLOT 7
#define R_GLOB_DAT 6
#define R_RELATIVE 8
#define R_ABSOLUTE 1
#define R_PC32 2

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    Elf_Half e_type, e_machine;
    Elf_Word e_version;
    Elf_Addr e_entry;
    Elf_Off e_phoff, e_shoff;
    Elf_Word e_flags;
    Elf_Half e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Elf_Ehdr;

typedef struct {
    Elf_Word p_type;
    Elf_Off p_offset;
    Elf_Addr p_vaddr, p_paddr;
    Elf_Xword p_filesz, p_memsz;
    Elf_Word p_flags;
    Elf_Xword p_align;
} Elf_Phdr;

typedef struct {
    Elf_Word st_name;
    Elf_Addr st_value;
    Elf_Xword st_size;
    unsigned char st_info, st_other;
    Elf_Half st_shndx;
} Elf_Sym;

typedef struct {
    Elf_Addr r_offset;
    Elf_Xword r_info;
} Elf_Reloc;

// Each image accepts at most 24 program headers, matching the reference budget.
#define kMaxProgramHeaders 24

/* El estado de UNA libreria cargada.
 *
 * Antes habia una sola, y por eso esto era un unico objeto global con un
 * nombre. Con una cadena de DT_NEEDED deja de alcanzar: cargar una dependencia
 * pisa el estado de la que se estaba cargando, asi que hace falta un lugar por
 * libreria y un puntero a la que esta en curso. */
typedef struct {
    int loaded;
    int file_section;
    int file_fd;
    /* Los dos de arriba solo viven durante la carga. Este dice si siguen
     * abiertos, para que el cierre no los repita. */
    int handles_open;
    /* Desplazamiento de carga. Un ET_DYN tiene p_vaddr cerca de cero, y cero no
     * es una direccion de usuario valida: el kernel no mapea por debajo de
     * kUserBase. Asi que el primer segmento -- el que tiene p_vaddr 0 -- se pide
     * sin base para que el kernel elija una buena, y esa direccion menos su
     * p_vaddr es el bias contra el que va todo lo demas. */
    unsigned long bias;
    int bias_ready;
    Elf_Ehdr header;
    Elf_Half header_count;
    const unsigned char* dynsym;
    const char* dynstr;
    size_t dynstr_size;
    size_t dynsym_count;
    /* Cuantos DT_NEEDED ya se trajeron. Es el cursor de la cadena: con un
     * diamante, D se carga desde A y desde C, y el segundo next_needed tiene
     * que saltar el que ya se proceso en vez de volver a cargarlo. */
    unsigned needed_done;
    /* Nombre con el que se cargo, para reconocerlo si otro DT_NEEDED lo pide de
     * nuevo en la misma cadena. Es el basename: DT_NEEDED lleva "libmath.so.0.4"
     * aunque la ruta sea /disk/lib/libmath.so.0.4. */
    char soname[96];
    /* Los program headers se leen una vez y se guardan aca: despues el
     * descriptor sigue abierto pero no se vuelve a leer el archivo. */
    Elf_Phdr headers[kMaxProgramHeaders];
    /* Direccion final de cada segmento, en el mismo orden que los program
     * headers. Los indices son los de los headers, asi que un -1 es "este
     * segmento no se pudo mapear". */
    void* placed[kMaxProgramHeaders];
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

static Library g_libs[kMaxLibraries];
static int g_lib_count;
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
static int read_header(const unsigned char* bytes, Elf_Ehdr* out);
static void* place_segment(const Elf_Phdr* ph);
static void* map_of(int slot, Elf_Addr vaddr);
static int read_dynamic_in(int slot);
static int find_dynamic_in(int slot, Elf_Sword wanted, void* out);
static int mapped_range(int slot, Elf_Addr address, size_t size, int writable);
static void* bias_in(int slot, Elf_Addr value);
static int resolve(const char* name, Elf_Addr* value_out);
static const char* next_needed(int slot);
static int load_needed_chain(int parent_slot);

static int read_header(const unsigned char* bytes, Elf_Ehdr* out) {
    if (bytes[0] != 0x7f || bytes[1] != 'E' || bytes[2] != 'L' || bytes[3] != 'F') {
        return 0;
    }
    copy_bytes(bytes, out, sizeof(*out));
    if (out->e_ident[4] != ELF_CLASS || out->e_ident[5] != 1 ||
        out->e_ident[6] != 1 || out->e_version != 1 || out->e_ehsize != sizeof(Elf_Ehdr) ||
        out->e_type != ET_DYN || out->e_machine != ELF_MACHINE) {
        return 0;
    }
    if (out->e_phentsize != sizeof(Elf_Phdr) || out->e_phnum == 0 ||
        out->e_phnum > kMaxProgramHeaders) {
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

    Elf_Ehdr header;
    const unsigned char* bytes = (const unsigned char*)cursor;
    if (bytes[0] != 0x7f || bytes[1] != 'E' || bytes[2] != 'L' || bytes[3] != 'F') {
        return 0;
    }
    copy_bytes(bytes, &header, sizeof(header));
    /* ET_DYN o ET_EXEC: cualquier otra cosa no es una imagen de programa. Y
     * e_phnum tiene que caber, porque de ahi salen los segmentos que se usan
     * para traducir direcciones. */
    if ((header.e_type != ET_DYN && header.e_type != ET_EXEC) ||
        header.e_machine != ELF_MACHINE || header.e_phentsize != sizeof(Elf_Phdr) ||
        header.e_phnum == 0 || header.e_phnum > kMaxProgramHeaders) {
        return 0;
    }

    Library* exe = &g_libs[g_lib_count];
    exe->soname[0] = '\0';
    exe->header = header;
    exe->header_count = header.e_phnum;
    exe->dynsym = 0;
    exe->dynstr = 0;
    exe->loaded = 0;
    exe->bias_ready = 0;
    exe->needed_done = 0;
    for (Elf_Half index = 0; index < header.e_phnum; ++index) {
        copy_bytes((const unsigned char*)cursor + header.e_phoff + (index * header.e_phentsize),
                   &exe->headers[index], sizeof(Elf_Phdr));
        exe->placed[index] = 0;
    }
    /* The kernel already mapped the executable. Compute its bias from the
     * mapped header and the first loadable segment's page-aligned address. */
    Elf_Addr first_vaddr = 0;
    int found_first = 0;
    for (Elf_Half index = 0; index < header.e_phnum; ++index) {
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
    const unsigned long bias = cursor - (unsigned long)(first_vaddr & ~(Elf_Addr)4095);
    exe->bias = bias;
    exe->bias_ready = 1;
    for (Elf_Half index = 0; index < header.e_phnum; ++index) {
        if (exe->headers[index].p_type != PT_LOAD || exe->headers[index].p_memsz == 0) {
            continue;
        }
        exe->placed[index] = (void*)(bias + (exe->headers[index].p_vaddr & ~(Elf_Addr)4095));
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
    g_lib->header_count = 0;
    g_lib->loaded = 0;
    g_lib->bias_ready = 0;
    g_lib->dynsym = 0;
    g_lib->dynstr = 0;
    g_lib->needed_done = 0;
    g_lib->soname[0] = '\0';
    /* Los handles del archivo tambien se ponen a cero. Este slot pudo ser el de
     * una libreria cargada antes --cargarla ocupa el hueco y lo deja--, asi que
     * un savanxp_open fallido dejaba los handles de la anterior puestos aca, y un
     * cierre de los propios habria cerrado los de aquella. */
    g_lib->file_section = -1;
    g_lib->file_fd = -1;
    g_lib->handles_open = 0;
    return g_lib;
}

/* Se declara antes de end_load, que la usa: end_load suelta el puntero a la
 * libreria en curso, y con el puntero no hay forma de cerrarle nada. */
static void close_file_handles(void);

static void end_load(int ok) {
    if (ok) {
        g_lib->loaded = 1;
        g_lib_count++;
    } else {
        /* Un fallo a mitad de la carga dejo handles abiertos. Cerrarlos aca y no
         * en el llamador es lo que los cierra en todos los caminos de error. */
        close_file_handles();
        // A rejected, unpublished image must not consume views or physical
        // sections. Published dependency failures retain the reference model's
        // diagnostic state until process exit.
        for (unsigned index = 0; index < g_lib->header_count; ++index) {
            if (g_lib->placed[index]) {
                (void)unmap_view(g_lib->placed[index]);
                g_lib->placed[index] = 0;
            }
        }
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

// Apply both relocation tables eagerly before a PLT call can reach them.
/* Los tres por que una reubicacion con simbolo puede fallar, en palabras.
 *
 * La funcion que las llama devuelve cero y quien lo ve solo tiene un numero de
 * paso. El numero dice DONDE, no QUE: "paso 5" son todas las reubicaciones de la
 * tabla, con toda la tabla de simbolos por delante. Un numero de paso ahorra
 * trabajo; un nombre ahorra la tarde.
 *
 * Se imprimen desde aca y no desde arriba porque aca esta el nombre, y quien llama
 * ya lo solto: el retorno es un int. */
static void report_unresolved(const char* library, const char* symbol)
{
    /* Se guarda ademas de imprimirse. Imprimir es lo que ayuda a una persona; el
     * global es lo que permite a una PRUEBA afirmar el nombre, porque una prueba
     * no puede leer la salida estandar. */
    g_lib_fail_library = library;
    g_lib_fail_symbol = symbol;
    eprintf("loader: %s necesita \"%s\" y no esta en ninguna imagen cargada\n",
            library, symbol);
}

static void report_unplaced(const char* symbol, int owner)
{
    eprintf("loader: \"%s\" se encontro en %s, pero su direccion no cae en ningun "
            "segmento mapeado\n",
            symbol, g_libs[owner].soname);
}

static void report_unmapped(const char* library, const char* symbol, Elf_Addr offset)
{
    eprintf("loader: %s escribe en 0x%lx para \"%s\", que no esta en ningun segmento "
            "mapeado de la imagen\n",
            library, (unsigned long)offset, symbol);
}

/* Como se llama una imagen en los mensajes.
 *
 * El ejecutable no tiene DT_SONAME --ese campo es solo para .so--, asi que su
 * nombre queda vacio y el mensaje sale como "39 entradas de  se quedaron". El
 * nombre de la imagen es lo primero que se lee de un diagnostico, y que este en
 * blanco lo hace inutil. */
static const char* image_name(int slot) {
    return g_libs[slot].soname[0] != '\0' ? g_libs[slot].soname : "el ejecutable";
}

static int apply_table_in(int slot, unsigned long elf_table, unsigned long elf_size) {
    // Preserve the reference tolerant failure mode: relocate the rest of an image
    // after a missing dependency, and count the unresolved entries for diagnostics.
    const int tolerant = g_missing_library != 0;
    unsigned int skipped = 0;
    const unsigned char* table = (const unsigned char*)map_of(slot, elf_table);
    if (table == 0 || elf_size % sizeof(Elf_Reloc) ||
        !mapped_range(slot, elf_table, elf_size, 0)) return 0;
    for (unsigned long offset = 0; offset + sizeof(Elf_Reloc) <= elf_size;
         offset += sizeof(Elf_Reloc)) {
        Elf_Reloc relocation;
        copy_bytes(table + offset, &relocation, sizeof(relocation));
        const unsigned type = ELF_R_TYPE(relocation.r_info);
        if (type == 0) continue;
        Elf_Addr* destination = (Elf_Addr*)map_of(slot, relocation.r_offset);
        if (!destination || !mapped_range(slot, relocation.r_offset, sizeof(Elf_Addr), 1)) return 0;
        const Elf_Addr addend = *destination;
        if (type == R_RELATIVE) {
            extern unsigned int sx_bootstrap_relocated;
            if (slot == 0 && sx_bootstrap_relocated) continue;
            // RELATIVE adds the image bias to the word already in the image.
            Elf_Addr* where = (Elf_Addr*)map_of(slot, relocation.r_offset);
            if (where == 0) {
                return 0;
            }
            *where = (Elf_Addr)((unsigned long)addend + g_libs[slot].bias);
            continue;
        }
        if (type != R_JUMP_SLOT && type != R_GLOB_DAT &&
            type != R_ABSOLUTE && type != R_PC32) {
            // Unsupported relocation types fail explicitly rather than leaving a broken pointer.
            eprintf("loader: %s has unsupported relocation type %u; "
                    "supported: JUMP_SLOT, GLOB_DAT, absolute, PC32 (i386), RELATIVE\n",
                    image_name(slot), type);
            return 0;
        }
        const unsigned name_index = ELF_R_SYM(relocation.r_info);
        if (name_index >= g_libs[slot].dynsym_count) return 0;
        Elf_Sym symbol;
        copy_bytes(g_libs[slot].dynsym + (name_index * sizeof(Elf_Sym)), &symbol, sizeof(symbol));
        if (symbol.st_name >= g_libs[slot].dynstr_size) return 0;

        /* Por nombre, no por st_value. Un st_value es relativo a la libreria que
         * DEFINIO el simbolo, y el que necesita la llamada puede ser otra: sin
         * la busqueda, una llamada a una dependencia se resolveria con la
         * direccion del simbolo dentro de la imagen que la invoca. */
        Elf_Addr target = 0;
        const int owner = resolve(g_libs[slot].dynstr + symbol.st_name, &target);
        const int unresolved_weak = owner < 0 && (symbol.st_info >> 4) == 2;
        if (owner < 0 && !unresolved_weak) {
            if (tolerant) {
                skipped++;
                continue;
            }
            /* El nombre es lo unico que hay. Sin el, el sintoma es "paso 5" y la
             * unica manera de sacar algo es un volcado de la .dynsym. */
            report_unresolved(image_name(slot), g_libs[slot].dynstr + symbol.st_name);
            return 0;
        }
        const Elf_Addr value = unresolved_weak ? 0 : (Elf_Addr)(unsigned long)bias_in(owner, target);
        if (value == 0 && !unresolved_weak) {
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
        Elf_Addr* where = (Elf_Addr*)map_of(slot, relocation.r_offset);
        if (where == 0) {
            /* El GOT de esta entrada no esta en ningun segmento mapeado. Un
             * error de la imagen, no del enlazado. */
            if (tolerant) {
                skipped++;
                continue;
            }
            report_unmapped(image_name(slot), g_libs[slot].dynstr + symbol.st_name, relocation.r_offset);
            return 0;
        }
        // Absolute relocations add the original word. GOT/PLT entries take
        // the resolved symbol address alone.
        if (type == R_PC32) {
            *where = value + addend - (Elf_Addr)(uintptr_t)where;
        } else {
            *where = value + (type == R_ABSOLUTE ? addend : 0);
        }
    }
    if (skipped != 0) {
        /* Una linea, no una por simbolo: el nombre de la libreria ya lo enseanyo
         * el mensaje de la carga, y lo que aporta aqui es el RECUENTO. */
        eprintf("loader: %u entradas de %s se quedaron sin reubicar porque %s no cargo\n",
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
    if (!strsz || !mapped_range(slot, strtab, strsz, 0) || strings[strsz - 1] != 0) return 0;
    library->dynstr_size = strsz;
    unsigned long hash = 0;
    unsigned long count = strtab > symtab ? (strtab - symtab) / sizeof(Elf_Sym) : 0;
    if (find_dynamic_in(slot, 4 /* DT_HASH */, &hash) && mapped_range(slot, hash, 8, 0)) {
        const unsigned char* bytes = map_of(slot, hash);
        uint32_t nchain;
        copy_bytes(bytes + 4, &nchain, sizeof(nchain));
        count = nchain;
    }
    if (!count || count > 65536 || !mapped_range(slot, symtab, count * sizeof(Elf_Sym), 0)) return 0;
    library->dynsym_count = count;
    library->dynsym = symbols;
    library->loaded = 1;
    return 1;
}

/* Both tables are optional. A library with no imported calls can omit .rel.plt. */
static int apply_relocs_in(int slot) {
    unsigned long jmprel = 0;
    unsigned long pltrelsz = 0;
    unsigned long rel = 0;
    unsigned long relsz = 0;
    unsigned long plt_format = 0;
    if (find_dynamic_in(slot, DT_RELOC, &rel) && find_dynamic_in(slot, DT_RELOCSZ, &relsz) &&
        relsz != 0) {
        if (!apply_table_in(slot, rel, relsz)) { g_lib_reloc_step = 5; return 0; }
    }
    if (find_dynamic_in(slot, DT_JMPREL, &jmprel) &&
        find_dynamic_in(slot, DT_PLTRELSZ, &pltrelsz) && pltrelsz != 0) {
        if (!find_dynamic_in(slot, DT_PLTREL, &plt_format) || plt_format != DT_RELOC) return 0;
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
    // Startup is idempotent: reapplying i386 REL would add the bias twice.
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
        /* No es "no se pudo cargar": es que ya hay 32 y no cabe una mas. El
         * numero de paso no distingue los dos, y la accion es distinta: una
         * son 32 es una cadena demasiado larga, la otra es que el archivo no
         * esta. */
        eprintf("loader: no cabe una libreria mas, el limite es %d y ya hay %d\n",
                kMaxLibraries, g_lib_count);
        return -1;
    }
    const long fd = savanxp_open(path);
    if (fd < 0) {
        /* El caso mas comun de todos y el que mas cuesta adivinar: una
         * dependencia que no esta en el volumen. El nombre va en el mensaje
         * porque el que se imprime arriba es solo un numero de paso. */
        eprintf("loader: no se pudo abrir %s (paso 2)\n", path);
        end_load(0);
        return -2;
    }
    const long section = section_open((int)fd, SAVANXP_SECTION_READ | SAVANXP_SECTION_EXEC);
    if (section < 0) {
        eprintf("loader: no se pudo mapear %s como seccion ejecutable (paso 3)\n", path);
        savanxp_close((int)fd);
        end_load(0);
        return -3;
    }
    g_lib->file_section = (int)section;
    g_lib->file_fd = (int)fd;
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

    /* Mapa de trabajo: el archivo entero en una direccion que eligio el kernel,
     * solo para leerle la cabecera y los program headers. */
    // Read only the native header and bounded program-header table, not a whole-file view.
    unsigned char probe[sizeof(Elf_Ehdr) + (sizeof(Elf_Phdr) * kMaxProgramHeaders)];
    unsigned long filled = 0;
    const unsigned long want_bytes =
        sizeof(Elf_Ehdr) + (sizeof(Elf_Phdr) * kMaxProgramHeaders);
    while (filled < want_bytes) {
        const long got = savanxp_read((int)fd, probe + filled, want_bytes - filled);
        if (got <= 0) {
            break;
        }
        filled += (unsigned long)got;
    }
    if (filled < sizeof(Elf_Ehdr)) {
        end_load(0);
        return -4;
    }
    Elf_Ehdr header;
    if (!read_header(probe, &header)) {
        /* read_header no dice cual de sus seis condiciones fallo, y aqui hay un
         * solo mensaje para las seis. Se distingue el caso comun --no es una
         * imagen ELF-- del resto, porque un archivo que no es una imagen y una
         * imagen con un e_phnum imposible se rompen distinto. */
        if (probe[0] != 0x7f || probe[1] != 'E' || probe[2] != 'L' || probe[3] != 'F')
        {
            eprintf("loader: %s no empieza con la magia de ELF\n", path);
        }
        else
        {
            eprintf("loader: %s es un ELF que no sirve de libreria (clase, tipo, "
                    "maquina o numero de program headers)\n",
                    path);
        }
        end_load(0);
        return -5;
    }
    if (header.e_phoff > filled || header.e_phnum > (filled - header.e_phoff) / sizeof(Elf_Phdr)) {
        end_load(0);
        return -5;
    }
    g_lib->header = header;
    g_lib->header_count = header.e_phnum;
    for (Elf_Half index = 0; index < header.e_phnum; ++index) {
        copy_bytes(probe + header.e_phoff + (index * header.e_phentsize), &g_lib->headers[index], sizeof(Elf_Phdr));
        g_lib->placed[index] = 0;
    }

    for (Elf_Half index = 0; index < header.e_phnum; ++index) {
        if (g_lib->headers[index].p_type != PT_LOAD || g_lib->headers[index].p_memsz == 0) {
            continue;
        }
        const Elf_Phdr* ph = &g_lib->headers[index];
        struct savanxp_stat metadata;
        if (savanxp_fstat(g_lib->file_fd, &metadata) < 0 || ph->p_filesz > ph->p_memsz ||
            ph->p_offset > metadata.st_size || ph->p_filesz > metadata.st_size - ph->p_offset ||
            ph->p_vaddr > (Elf_Addr)-1 - ph->p_memsz ||
            ph->p_memsz > 64u * 1024u * 1024u ||
            (ph->p_offset & 4095) != (ph->p_vaddr & 4095) || (ph->p_flags & (PF_W | PF_X)) == (PF_W | PF_X)) {
            end_load(0);
            return -5;
        }
        g_lib->placed[index] = place_segment(ph);
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

/* Cierra el descriptor y la seccion del archivo de la libreria en curso.
 *
 * Los dos handles solo hacen falta mientras se leen los program headers y se
 * mapean los segmentos, que ya termino. Dejarlos abiertos costaba DOS
 * descriptores por libreria para siempre, y se nota: windowd lanza sus clientes y
 * les hereda la tabla, asi que cargar una tercera libreria --libgfx2d-- empujó la
 * sesion de 20 a 21 descriptores y windowd-smoke fallo por eso. Con tres
 * libreras eran seis descriptores quemados en cada proceso, sin usar.
 *
 * Antes el codigo decia, en el comentario del struct, que el descriptor "sigue
 * abierto pero no se vuelve a leer el archivo": o sea, un handle guardado para no
 * hacer nada. */
static void close_file_handles(void) {
    if (g_lib == 0 || !g_lib->handles_open) {
        return;
    }
    if (g_lib->file_section >= 0) {
        (void)savanxp_close(g_lib->file_section);
        g_lib->file_section = -1;
    }
    if (g_lib->file_fd >= 0) {
        (void)savanxp_close(g_lib->file_fd);
        g_lib->file_fd = -1;
    }
    g_lib->handles_open = 0;
}

/* ¿Ya esta cargada alguna libreria con este basename? */
static int already_loaded(const char* soname) {
    for (int slot = 0; slot < g_lib_count; ++slot) {
        int i = 0;
        for (;; ++i) {
            if (g_libs[slot].soname[i] != soname[i]) {
                break;
            }
            if (soname[i] == '\0') {
                return 1;
            }
        }
    }
    return 0;
}

/* Recorre la cadena de DT_NEEDED de una libreria. Carga lo que falte desde
 * /disk/lib/ y avanza el cursor de la libreria procesando, asi no carga dos
 * veces lo mismo.
 *
 * Es iterativo y no recursivo adrede: cargar una dependencia cambia g_lib (la
 * libreria en curso), asi que llevarlo por recursiones obligaria a guardar y
 * restaurar el contexto completo en cada nivel. Con un loop basta: se procesan
 * las librerias en el orden en que se completaron, y cada nueva carga apunta al
 * slot de esta en curso, no al de la que se estaba recorriendo. */
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
            if (already_loaded(need)) {
                continue;
            }
            /* El nombre de DT_NEEDED es un basename, no una ruta: se busca en
             * /disk/lib/. Una ruta que ya empieza con / se usa tal cual. */
            char full[128];
            if (need[0] == '/') {
                unsigned i = 0;
                while (need[i] != '\0' && i < sizeof(full) - 1) {
                    full[i] = need[i];
                    i++;
                }
                full[i] = '\0';
            } else {
                const char* prefix = "/disk/lib/";
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
            if (ldso_load(full) != 0) {
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
                    if (strcmp(g_missing_names[seen], need) == 0) {
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

/* Mapea un segmento en la direccion que pide el ELF.
 *
 * Un segmento escribible no puede salir de la seccion del archivo: el respaldo
 * es de solo lectura justamente para que dos procesos que cargan la misma
 * libreria no se pisen, y el GOT de un .so vive en un segmento escribible. Asi
 * que se crea una seccion anonima en esa direccion y se copia el contenido del
 * archivo. Cada proceso paga su copia, que es lo mismo que paga cualquier
 * biblioteca con datos reubicados. */
static void* place_segment(const Elf_Phdr* ph) {
    const int wants_exec = (ph->p_flags & PF_X) != 0;
    const unsigned long flags =
        wants_exec ? (SAVANXP_SECTION_READ | SAVANXP_SECTION_EXEC) : (SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    const unsigned long at = (unsigned long)(ph->p_vaddr & ~(Elf_Addr)4095);
    /* Los bytes del segmento empiezan a offset p_vaddr - at dentro de la pagina,
     * NO al principio del mapping.
     *
     * map_view_at exige una direccion alineada a pagina y pega el primer byte de
     * la seccion en esa direccion. Por eso el rango se pide desde el principio
     * de la pagina del segmento y no desde p_offset: asi el primer byte caiga
     * donde tiene que caer y TODA la imagen comparta un solo bias. Sin eso cada
     * segmento tiene un ancla distinta, y las referencias entre segmentos -- un
     * PLT en el codigo saltando a un GOT en los datos -- apuntan a la imagen
     * equivocada sin dar ningun error visible.
     *
     * El rango arranca en p_offset - off_in_page, que es el comienzo de esa
     * pagina en el archivo: los ELF guardan p_offset y p_vaddr con el mismo
     * resto modulo la pagina, asi que nunca es negativo. */
    const unsigned long off_in_page = (unsigned long)(ph->p_vaddr - at);
    const unsigned long start_off = (unsigned long)ph->p_offset - off_in_page;
    const unsigned long span = off_in_page + (unsigned long)ph->p_filesz;
    unsigned long want = 0;
    if (g_lib->bias_ready) {
        if (at > (unsigned long)-1 - g_lib->bias) return 0;
        want = g_lib->bias + at;
    }

    long segment_section;
    if (wants_exec) {
        /* Solo el tramo del archivo que corresponde a ESTE segmento. Mapear la
         * seccion del archivo entero pondria el resto del ELF en direcciones que
         * no le tocan: el primer LOAD pide p_vaddr 0 para el offset 0 y el
         * siguiente pide 0x2560 para el offset 0x1560. */
        segment_section = section_open_range(g_lib->file_fd, start_off, span, flags);
    } else {
        /* La region privada tiene queoze el hueco de la pagina antes del
         * segmento, o los bytes copiados no entran. */
        segment_section = section_create(off_in_page + (unsigned long)ph->p_memsz, flags);
    }
    if (segment_section < 0) {
        g_lib_fail_errno = segment_section;
        return 0;
    }

    void* view = map_view_at((int)segment_section, want, flags | (wants_exec ? 0 : SAVANXP_VIEW_PRIVATE));
    if (view == 0 || result_is_error((long)view)) {
        g_lib_fail_errno = (long)view;
        (void)savanxp_close((int)segment_section);
        return 0;
    }
    // The view owns its reference; temporary section descriptors must not
    // survive either successful loads or any later failure.
    (void)savanxp_close((int)segment_section);
    if (!g_lib->bias_ready) {
        /* El kernel eligio; ese es el bias. Los segmentos siguientes se piden
         * explicitamente para que no puedan caer en otra parte. */
        g_lib->bias = (unsigned long)view - at;
        g_lib->bias_ready = 1;
        g_lib_bias = g_lib->bias;
    }
    if (!wants_exec && ph->p_filesz != 0) {
        /* Los bytes salen de una seccion del rango EXACTO del archivo, mapeada un
         * momento y liberada. No se puede savanxp_read: no hay seek en la API, el
         * descriptor sigue leyendo desde donde quedo la vez anterior, y el
         * segmento recibiria bytes del offset equivocado. */
        const long source = section_open_range(g_lib->file_fd, start_off, span, SAVANXP_SECTION_READ);
        if (source < 0) {
            (void)unmap_view(view);
            return 0;
        }
        const unsigned char* bytes = (const unsigned char*)map_view_at((int)source, 0, SAVANXP_SECTION_READ);
        if (bytes == 0 || result_is_error((long)bytes)) {
            (void)savanxp_close((int)source);
            (void)unmap_view(view);
            return 0;
        }
        copy_bytes(bytes, view, (size_t)span);
        (void)unmap_view((void*)bytes);
        (void)savanxp_close((int)source);
        /* Los bytes mas alla de p_filesz son .bss: section_create ya entrego la
         * pagina en cero. */
    }
    return view;
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
// Bound all table reads and relocation writes by an actually mapped PT_LOAD.
static int mapped_range(int slot, Elf_Addr address, size_t size, int writable) {
    const Library* library = &g_libs[slot];
    for (unsigned index = 0; index < library->header_count; ++index) {
        const Elf_Phdr* ph = &library->headers[index];
        if (ph->p_type != PT_LOAD || !library->placed[index] ||
            (writable && !(ph->p_flags & PF_W)) || address < ph->p_vaddr) continue;
        const Elf_Addr offset = address - ph->p_vaddr;
        if (offset <= ph->p_memsz && size <= ph->p_memsz - offset) return 1;
    }
    return 0;
}

static void* bias_in(int slot, Elf_Addr value) {
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
static void* map_of(int slot, Elf_Addr vaddr) {
    const Library* library = &g_libs[slot];
    for (Elf_Half index = 0; index < library->header_count; ++index) {
        const Elf_Phdr* ph = &library->headers[index];
        if (ph->p_type != PT_LOAD || library->placed[index] == 0) {
            continue;
        }
        if (vaddr < ph->p_vaddr || vaddr >= ph->p_vaddr + ph->p_memsz) {
            continue;
        }
        const Elf_Addr page = ph->p_vaddr & ~(Elf_Addr)4095;
        return (void*)(vaddr - page + (Elf_Addr)(unsigned long)library->placed[index]);
    }
    return 0;
}

typedef struct {
    Elf_Sword tag;
    union {
        Elf_Addr pointer;
        Elf_Word value;
    } u;
} Elf_Dyn;

/* Recorre la tabla dinamica y deja el puntero a la entrada pedida. Las entradas
 * tienen tamaño fijo, asi que el indice se multiplica por sizeof(Elf_Dyn). */
static int find_dynamic_in(int slot, Elf_Sword wanted, void* out) {
    const Library* library = &g_libs[slot];
    for (Elf_Half index = 0; index < library->header_count; ++index) {
        const Elf_Phdr* ph = &library->headers[index];
        if (ph->p_type != PT_DYNAMIC) {
            continue;
        }
        const unsigned char* table = (const unsigned char*)map_of(slot, ph->p_vaddr);
        if (table == 0 || !mapped_range(slot, ph->p_vaddr, ph->p_memsz, 0)) {
            return 0;
        }
        for (Elf_Addr offset = 0; offset + sizeof(Elf_Dyn) <= ph->p_memsz;
             offset += sizeof(Elf_Dyn)) {
            Elf_Dyn entry;
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
    for (Elf_Half index = 0; index < library->header_count; ++index) {
        const Elf_Phdr* ph = &library->headers[index];
        if (ph->p_type != PT_DYNAMIC) {
            continue;
        }
        /* El PT_DYNAMIC esta dentro de un PT_LOAD; se traduce con los segmentos
         * ya colocados de ESTA libreria, no con los de la que este en curso. */
        const unsigned char* table = (const unsigned char*)map_of(slot, ph->p_vaddr);
        if (table == 0 || !mapped_range(slot, ph->p_vaddr, ph->p_memsz, 0)) {
            return 0;
        }
        if (table == 0) {
            return 0;
        }
        for (Elf_Addr offset = 0; offset + sizeof(Elf_Dyn) <= ph->p_memsz;
             offset += sizeof(Elf_Dyn)) {
            Elf_Dyn entry;
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
            if (entry.u.pointer >= library->dynstr_size) return 0;
            return library->dynstr + entry.u.pointer;
        }
        return 0;
    }
    return 0;
}

static int resolve_in(const char* name, int slot, Elf_Addr* value_out) {
    const Library* library = &g_libs[slot];
    if (library->dynsym == 0 || library->dynstr == 0) {
        return -1;
    }
    const size_t limit = library->dynstr_size;
    for (unsigned index = 0; index < library->dynsym_count; ++index) {
        Elf_Sym symbol;
        copy_bytes(library->dynsym + (index * sizeof(Elf_Sym)), &symbol, sizeof(symbol));
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
        if (strcmp(library->dynstr + symbol.st_name, name) == 0) {
            *value_out = symbol.st_value;
            return slot;
        }
    }
    return -1;
}

static int resolve(const char* name, Elf_Addr* value_out) {
    for (int slot = g_lib_count - 1; slot >= 0; --slot) {
        if (resolve_in(name, slot, value_out) >= 0) {
            return slot;
        }
    }
    return -1;
}

void* ldso_lookup(const char* name) {
    if (g_lib_count == 0) {
        return 0;
    }
    Elf_Addr value = 0;
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
    Elf_Addr value = 0;
    return resolve(name, &value) > 0;
}
