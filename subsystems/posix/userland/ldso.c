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
#define ET_EXEC 2
#define EM_X86_64 62

/* La base que elige el kernel para una imagen ET_DYN, y el piso del escaneo que
 * busca la cabecera del ejecutable. Son el mismo numero por construccion: una
 * ET_DYN se carga en kUserBase, y el recorrido hacia atras no puede seguir
 * bajando de ahi. */
#define kExecutableBase 0x0000000000400000UL
#define kScanFloor kExecutableBase

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

#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_GLOB_DAT 6
/* El valor guardado ya es una direccion de enlace, sin simbolo: hay que
 * sumarle el bias de la imagen. Es lo que hace que un ejecutable PIE con
 * punteros a datos -- una tabla de vtables, una cadena constante, un GOT
 * inicial -- arranque con las direcciones donde el codigo las busca. Una
 * imagen reubicada sin esto tiene punteros al valor cero. */
#define R_X86_64_RELATIVE 8

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
    /* Nombre con el que se cargo, para reconocerlo si otro DT_NEEDED lo pide de
     * nuevo en la misma cadena. Es el basename: DT_NEEDED lleva "libmath.so.0.4"
     * aunque la ruta sea /disk/lib/libmath.so.0.4. */
    char soname[96];
    /* Los program headers se leen una vez y se guardan aca: despues el
     * descriptor sigue abierto pero no se vuelve a leer el archivo. */
    Elf64_Phdr headers[64];
    /* Direccion final de cada segmento, en el mismo orden que los program
     * headers. Los indices son los de los headers, asi que un -1 es "este
     * segmento no se pudo mapear". */
    void* placed[64];
} Library;

/* Cuantas librerias conviven. Ocho alcanzan para un interprete con su cadena de
 * dependencias; excederlo es un fallo explicito en load_one, no un
 * desbordamiento. */
#define kMaxLibraries 8

static Library g_libs[kMaxLibraries];
static int g_lib_count;
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

/* Registra el ejecutable en g_libs para que sus simbolos entren en el ambito.
 *
 * El cargador no mapeo esta imagen: la mapeo el kernel. Asi que no hay un
 * descriptor ni una seccion que abrir, y la base hay que deducirla. Se toma la
 * direccion de una funcion de este mismo archivo --que por definicion esta en la
 * imagen del ejecutable-- y se retrocede pagina a pagina buscando la cabecera
 * ELF. Es la misma cuenta que hace un interprete cuando no tiene a donde
 * preguntarle al nucleo.
 *
 * Devuelve 0 si no encuentra una cabecera valida, y el ambito queda como
 * estaba: resolver solo entre librerias. Un ejecutable sin .dynsym (una ET_EXEC
 * no PIE) es un caso normal, no un fallo. */
static int adopt_executable(void) {
    if (g_lib_count >= kMaxLibraries) {
        return 0;
    }
    const void* anchor = (const void*)adopt_executable;
    unsigned long cursor = (unsigned long)anchor & ~4095UL;

    Elf64_Ehdr header;
    int found = 0;
    /* El techo son 16 MiB y el suelo, la primera pagina mapeable de usuario. Un
     * ejecutable mas grande que el techo no existe aca, y los dos limites
     * evitan que una cabecera ilegible se convierta en un recorrido infinito
     * hasta toparse con algo. */
    for (unsigned long steps = 0; steps < (16UL << 20) / 4096UL; ++steps) {
        const unsigned char* bytes = (const unsigned char*)cursor;
        if (bytes[0] == 0x7f && bytes[1] == 'E' && bytes[2] == 'L' && bytes[3] == 'F') {
            copy_bytes(bytes, &header, sizeof(header));
            /* ET_DYN o ET_EXEC: cualquier otra cosa no es una imagen de
             * programa. Y e_phnum tiene que caber, porque de ahi salen los
             * segmentos que se usan para traducir direcciones. */
            if ((header.e_type == ET_DYN || header.e_type == ET_EXEC) &&
                header.e_machine == EM_X86_64 && header.e_phentsize == sizeof(Elf64_Phdr) &&
                header.e_phnum != 0 && header.e_phnum <= 64) {
                found = 1;
                break;
            }
        }
        if (cursor <= kScanFloor) {
            return 0;
        }
        cursor -= 4096UL;
    }
    if (!found) {
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
    for (Elf64_Half index = 0; index < header.e_phnum; ++index) {
        copy_bytes((const unsigned char*)cursor + header.e_phoff + (index * header.e_phentsize),
                   &exe->headers[index], sizeof(Elf64_Phdr));
        exe->placed[index] = 0;
    }
    /* Los segmentos ya estan mapeados: el kernel los puso, y el cargador no
     * tiene descriptor ni seccion con que mapearlos otra vez. placed[] se deduce
     * de la geometria de la imagen.
     *
     * El bias es el mismo que eligio el kernel: kUserBase para una ET_DYN, cero
     * para una ET_EXEC. Es la misma regla de kernel/elf.cpp, y tiene que
     * coincidir con ella o cada direccion del ejecutable caeria en otra pagina. */
    const unsigned long bias = (header.e_type == ET_DYN) ? kExecutableBase : 0UL;
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
    return g_lib;
}

static void end_load(int ok) {
    if (ok) {
        g_lib->loaded = 1;
        g_lib_count++;
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
static int apply_table_in(int slot, unsigned long elf_table, unsigned long elf_size) {
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
        if (type != R_X86_64_JUMP_SLOT && type != R_X86_64_GLOB_DAT) {
            return 0;
        }
        const unsigned name_index = (unsigned)(rela.r_info >> 32);
        Elf64_Sym symbol;
        copy_bytes(g_libs[slot].dynsym + (name_index * sizeof(Elf64_Sym)), &symbol, sizeof(symbol));

        /* Por nombre, no por st_value. Un st_value es relativo a la libreria que
         * DEFINIO el simbolo, y el que necesita la llamada puede ser otra: sin
         * la busqueda, una llamada a una dependencia se resolveria con la
         * direccion del simbolo dentro de la imagen que la invoca. */
        Elf64_Addr target = 0;
        const int owner = resolve(g_libs[slot].dynstr + symbol.st_name, &target);
        if (owner < 0) {
            return 0;
        }
        const Elf64_Addr value = (Elf64_Addr)(unsigned long)bias_in(owner, target);
        if (value == 0) {
            return 0;
        }
        Elf64_Addr* where = (Elf64_Addr*)map_of(slot, rela.r_offset);
        if (where == 0) {
            return 0;
        }
        *where = value;
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
    return 1;
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
    if (load_needed_chain(0) != 0) {
        return -8;
    }
    if (!apply_relocs_in(0)) {
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
        return -1;
    }
    const long fd = savanxp_open(path);
    if (fd < 0) {
        end_load(0);
        return -2;
    }
    const long section = section_open((int)fd, SAVANXP_SECTION_READ | SAVANXP_SECTION_EXEC);
    if (section < 0) {
        savanxp_close((int)fd);
        end_load(0);
        return -3;
    }
    g_lib->file_section = (int)section;
    g_lib->file_fd = (int)fd;
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
        end_load(0);
        return -4;
    }
    Elf64_Ehdr header;
    if (!read_header(probe, &header)) {
        end_load(0);
        return -5;
    }
    g_lib->header = header;
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
    if (chain_failed != 0) {
        g_lib = 0;
        return -8;
    }

    if (!apply_relocs_in(slot)) {
        g_lib = 0;
        return -9;
    }
    g_lib = 0;
    return 0;
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
                return -1;
            }
        }
    }
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
    unsigned long want = at;
    if (g_lib->bias_ready) {
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

    void* view = map_view_at((int)segment_section, want, flags);
    if (view == 0 || result_is_error((long)view)) {
        g_lib_fail_errno = (long)view;
        if (!wants_exec) {
            (void)savanxp_close((int)segment_section);
        }
        return 0;
    }
    if (!g_lib->bias_ready) {
        /* El kernel eligio; ese es el bias. Los segmentos siguientes se piden
         * explicitamente para que no puedan caer en otra parte. */
        g_lib->bias = (unsigned long)view - at;
        g_lib->bias_ready = 1;
        g_lib_bias = g_lib->bias;
    }
    if (!wants_exec) {
        /* Los bytes salen de una seccion del rango EXACTO del archivo, mapeada un
         * momento y liberada. No se puede savanxp_read: no hay seek en la API, el
         * descriptor sigue leyendo desde donde quedo la vez anterior, y el
         * segmento recibiria bytes del offset equivocado. */
        const long source = section_open_range(g_lib->file_fd, start_off, span, SAVANXP_SECTION_READ);
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
        copy_bytes(bytes, view, (size_t)span);
        (void)unmap_view((void*)bytes);
        (void)savanxp_close((int)source);
        /* Los bytes mas alla de p_filesz son .bss: section_create ya entrego la
         * pagina en cero. */
        (void)savanxp_close((int)segment_section);
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
    for (unsigned index = 0;; ++index) {
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
        if (strcmp(library->dynstr + symbol.st_name, name) == 0) {
            *value_out = symbol.st_value;
            return slot;
        }
    }
}

static int resolve(const char* name, Elf64_Addr* value_out) {
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
