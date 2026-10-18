#include "user32.hpp"

#include <stdint.h>

#include "cpu.hpp"
#include "kernel/string.hpp"
#include "kernel/vfs.hpp"
#include "paging.hpp"
#include "savanxp/syscall.h"
#include "serial.hpp"

namespace i386 {

namespace user32 {

namespace {

/* Tope del stack del proceso: cae en la ventana de usuario (PDE 33) y deja
 * 64 KiB debajo. De momento hay un proceso y no hay planificador, asi que una
 * pila fija alcanza; cuando haya varios, esto pasa a ser una pila por
 * proceso como en arch/x86_64. */
constexpr uint32_t kStackTop = 0x08400000u;
constexpr uint32_t kStackSize = 64u * 1024u;

/* Pila de ring 0 para las entradas desde ring 3: el processor carga
 * TSS.esp0/ss0 en cada int $0x80. Con un solo proceso una pila propia del
 * cargador es suficiente. */
alignas(16) uint8_t g_kernel_stack[8 * 1024];

/* El registro inicial que crt0 espera en su rama i386 (edi = argc, esi =
 * argv, edx = canario de pila, ecx = ruta del interprete y ebp = cabecera
 * ELF). Los immediatos del iret de abajo son esos valores: un proceso por
 * ahora, sin interprete y con canary fijo. */

constexpr size_t kMaxSegments = 16;

/* Registro inicial que crt0 espera en su rama i386: edi = argc, esi = argv,
 * edx = canario de pila, ecx = ruta del interprete y ebp = cabecera ELF. Un
 * proceso por ahora, sin interprete y con canary fijo. */
constexpr uint32_t kStackGuard = 0x00C0FFEEu;
constexpr uint32_t kUserEflags = 0x202; /* IF=1, IOPL de supervisor */

struct Elf32Header {
    uint8_t identification[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint32_t entry;
    uint32_t program_offset;
    uint32_t section_offset;
    uint32_t flags;
    /* e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx: el
     * orden importa, los dos ultimos campos son de la tabla de secciones y el
     * cargador no la usa, pero sin ellos los siguientes se leen corridos. */
    uint16_t header_size;
    uint16_t program_entry_size;
    uint16_t program_count;
    uint16_t section_entry_size;
    uint16_t section_count;
    uint16_t section_name_index;
};

struct Elf32Segment {
    uint32_t type;
    uint32_t offset;
    uint32_t virtual_address;
    uint32_t physical_address;
    uint32_t file_size;
    uint32_t memory_size;
    uint32_t flags;
    uint32_t alignment;
};

constexpr uint8_t kClass32 = 1;
constexpr uint8_t kDataLittleEndian = 1;
constexpr uint16_t kTypeExecutable = 2;
constexpr uint16_t kMachineIntel386 = 3;
constexpr uint32_t kSegmentLoad = 1;
constexpr uint32_t kSegmentExecute = 1;

bool in_user_window(uint32_t address, uint32_t size) {
    if (address < paging::kUserWindowBase || address >= paging::kUserWindowEnd) {
        return false;
    }
    return static_cast<uint64_t>(address) + size <= paging::kUserWindowEnd;
}

bool is_power_of_two(uint32_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

[[noreturn]] void fail(const char* reason) {
    serial::write("user: ");
    serial::write(reason);
    serial::write("\n");
    cpu::halt_forever();
}

bool read_at(vfs::Vnode& node, uint32_t offset, void* buffer, uint32_t count) {
    return vfs::read(node, offset, buffer, count) == count;
}

/* Lo que el kernel carga no es necesariamente cero: la ventana de usuario la
 * reservo GRUB2 para su cuenta, asi que el .bss del programa y el stack se
 * limpian antes de usarse. */
void fill_zero(uint32_t address, uint32_t count) {
    unsigned char* bytes = reinterpret_cast<unsigned char*>(static_cast<uintptr_t>(address));
    for (uint32_t index = 0; index < count; ++index) {
        bytes[index] = 0;
    }
}

/* Pila inicial: la ruta del programa arriba y debajo el arreglo de punteros
 * con su entrada y el terminador. En i386 cada ranura son 4 bytes (en
 * kernel/elf.cpp para x86_64 son 8: ese era el unico agujero LP64 del ABI). */
uint32_t build_initial_stack(const char* path) {
    fill_zero(kStackTop - kStackSize, kStackSize);

    const uint32_t length = static_cast<uint32_t>(strlen(path)) + 1u;
    const uint32_t string_address = (kStackTop - length) & ~3u;
    char* destination = reinterpret_cast<char*>(string_address);
    for (uint32_t index = 0; index < length; ++index) {
        destination[index] = path[index];
    }

    const uint32_t table_address = (string_address - 2u * 4u) & ~3u;
    uint32_t* table = reinterpret_cast<uint32_t*>(table_address);
    table[0] = string_address;
    table[1] = 0;
    return table_address;
}

/* El iret de entrada: se cambia la pila por la del proceso, se cargan los
 * registros de entrada y se empuja el marco que un iret pops (eip, cs,
 * eflags, esp, ss, de menor a mayor direccion). No hay vuelta: la primera
 * instruccion del programa ya corre en ring 3. */
[[noreturn]] void enter_user_mode(uint32_t user_esp, uint32_t image_base, uint32_t entry) {
    /* Todo por memoria, sin reparto de registros: clang decide como asignar un
     * operando "r" y si un valor inmediato se queda en el registro que hace
     * falta como destino, el iret salta al sitio equivocado. Pasó dos veces:
     * una con "movl $canario, %edx" encima del eip, otra con "%ecx" a cero
     * encima de la base de imagen. Con operandos de memoria no hay registros
     * que repartir y el destino de cada mov es fijo. Los valores viven en la
     * pila del kernel, por debajo de la pila del proceso, asi que siguen
     * siendo legibles cuando esp ya es la del proceso. */
    struct {
        uint32_t eip;
        uint32_t cs;
        uint32_t eflags;
        uint32_t esp;
        uint32_t ss;
    } frame = {entry, cpu::kUserCodeSelector, kUserEflags, user_esp, cpu::kUserDataSelector};

    const uint32_t argc = 1;
    const uint32_t argv = user_esp;
    const uint32_t guard = kStackGuard;
    const uint32_t interpreter = 0;

    asm volatile("cld\n\tmovl %[frame], %%esp\n\t"
                 :
                 : [frame] "r"(&frame)
                 : "memory");
    asm volatile("movl %[argc], %%edi\n\t"
                 "movl %[argv], %%esi\n\t"
                 "movl %[guard], %%edx\n\t"
                 "movl %[interpreter], %%ecx\n\t"
                 "movl %[base], %%ebp\n\t"
                 "iret"
                 :
                 : [argc] "m"(argc), [argv] "m"(argv), [guard] "m"(guard),
                   [interpreter] "m"(interpreter), [base] "m"(image_base)
                 : "memory");
    __builtin_unreachable();
}

}

void run(const char* path) {
    vfs::Vnode* node = vfs::open(path, SAVANXP_OPEN_READ);
    if (node == nullptr) {
        fail("el programa no esta en el volumen");
    }

    Elf32Header header = {};
    if (!read_at(*node, 0, &header, static_cast<uint32_t>(sizeof(header)))) {
        fail("no se pudo leer la cabecera ELF");
    }
    if (header.identification[0] != 0x7F || header.identification[1] != 'E' ||
        header.identification[2] != 'L' || header.identification[3] != 'F') {
        fail("la imagen no es ELF");
    }
    if (header.identification[4] != kClass32 || header.identification[5] != kDataLittleEndian) {
        fail("solo ELF32 little endian");
    }
    if (header.type != kTypeExecutable) {
        fail("solo imagenes ET_EXEC: el PIE vendra con su propio cargador");
    }
    if (header.machine != kMachineIntel386) {
        fail("la imagen no es de i386");
    }

    /* Una linea por imagen: los numeros de la tabla de segmentos son lo que
     * decide el resto del camino y conviene verlos en el log cuando algo se
     * rechaza. */
    serial::write("user: elf32 phentsize ");
    serial::write_dec(header.program_entry_size);
    serial::write(" phnum ");
    serial::write_dec(header.program_count);
    serial::write(" phoff ");
    serial::write_hex(header.program_offset);
    serial::write(" entry ");
    serial::write_hex(header.entry);
    serial::write("\n");
    if (header.program_entry_size != sizeof(Elf32Segment) || header.program_count == 0 ||
        header.program_count > kMaxSegments) {
        fail("tabla de segmentos invalida");
    }

    Elf32Segment segments[kMaxSegments] = {};
    if (!read_at(*node, header.program_offset, segments,
                 static_cast<uint32_t>(header.program_count) * static_cast<uint32_t>(sizeof(Elf32Segment)))) {
        fail("no se pudo leer la tabla de segmentos");
    }

    uint32_t image_base = 0;
    uint32_t entry_address = 0;
    for (uint16_t index = 0; index < header.program_count; ++index) {
        const Elf32Segment& segment = segments[index];
        if (segment.type != kSegmentLoad) {
            continue;
        }
        if (segment.file_size > segment.memory_size) {
            fail("segmento truncado");
        }
        if (!in_user_window(segment.virtual_address, segment.memory_size)) {
            fail("segmento fuera de la ventana de usuario");
        }
        if (!is_power_of_two(segment.alignment)) {
            fail("alineamiento de segmento no valido");
        }
        if (segment.file_size != 0 &&
            !read_at(*node, segment.offset,
                     reinterpret_cast<void*>(static_cast<uintptr_t>(segment.virtual_address)),
                     segment.file_size)) {
            fail("no se pudo copiar un segmento a la ventana de usuario");
        }
        if (segment.file_size < segment.memory_size) {
            fill_zero(segment.virtual_address + segment.file_size,
                      segment.memory_size - segment.file_size);
        }
        if (image_base == 0 || segment.virtual_address < image_base) {
            /* Ningun segmento cubre el offset 0 del fichero (la cabecera ELF
             * vive en su propia pagina, fuera de PT_LOAD), asi que la base de
             * la imagen es la direccion virtual mas baja: es lo que espera
             * crt0 como sx_image_base para comprobar simbolos debiles. */
            image_base = segment.virtual_address;
        }
        if ((segment.flags & kSegmentExecute) != 0 && header.entry >= segment.virtual_address &&
            header.entry < segment.virtual_address + segment.memory_size) {
            entry_address = header.entry;
        }
    }
    if (entry_address == 0) {
        fail("el punto de entrada no cae en un segmento ejecutable");
    }

    const uint32_t user_esp = build_initial_stack(path);

    serial::write("user: ");
    serial::write(path);
    serial::write(" cargado en ");
    serial::write_hex(image_base);
    serial::write(", entrada ");
    serial::write_hex(entry_address);
    serial::write(", pila ");
    serial::write_hex(user_esp);
    serial::write("\n");

    /* El userland se compila con SSE/SSE2, y las llamadas al sistema bajan a
     * la pila que aponta el TSS. */
    cpu::enable_fpu();
    cpu::set_ring0_stack(
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_kernel_stack[sizeof(g_kernel_stack)])));
    /* Los registros de datos los carga el proceso desde ring 0 justo antes
     * del iret; el iret en si solo cambia cs y ss. */
    cpu::load_user_segments();

    enter_user_mode(user_esp, image_base, entry_address);
}

} // namespace user32

} // namespace i386