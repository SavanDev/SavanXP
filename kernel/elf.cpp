#include "kernel/elf.hpp"

#include "kernel/physical_memory.hpp"
#include "kernel/string.hpp"

namespace {

constexpr uint32_t kElfMagic = 0x464c457fU;
constexpr uint32_t kElfClass64 = 2;
constexpr uint32_t kElfDataLittle = 1;
constexpr uint16_t kElfTypeExec = 2;
constexpr uint16_t kElfTypeShared = 3;
constexpr uint16_t kElfMachineX86_64 = 62;
constexpr uint32_t kProgramLoad = 1;
constexpr uint32_t kProgramInterp = 3;
// Tope de la ruta del interprete. Es una ruta de sistema, no entrada de usuario:
// un valor absurdo es un ELF roto, no algo que haya que truncar en silencio.
constexpr uint64_t kMaxInterpreterPath = 512;
constexpr uint32_t kProgramExecutable = 1u << 0;
constexpr uint32_t kProgramWritable = 1u << 1;
constexpr uint16_t kMaxLoadSegments = 64;

struct [[gnu::packed]] ElfHeader {
    uint32_t magic;
    uint8_t elf_class;
    uint8_t data_encoding;
    uint8_t version;
    uint8_t os_abi;
    uint8_t abi_version;
    uint8_t padding[7];
    uint16_t type;
    uint16_t machine;
    uint32_t version2;
    uint64_t entry;
    uint64_t program_header_offset;
    uint64_t section_header_offset;
    uint32_t flags;
    uint16_t header_size;
    uint16_t program_header_entry_size;
    uint16_t program_header_count;
    uint16_t section_header_entry_size;
    uint16_t section_header_count;
    uint16_t section_name_index;
};

struct [[gnu::packed]] ProgramHeader {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t virtual_address;
    uint64_t physical_address;
    uint64_t file_size;
    uint64_t memory_size;
    uint64_t alignment;
};

uint64_t align_down(uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1);
}

uint64_t align_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

bool range_within(uint64_t offset, uint64_t size, uint64_t limit) {
    return size <= limit && offset <= limit - size;
}

bool validate_header(const ElfHeader& header, size_t size) {
    const uint64_t table_size = static_cast<uint64_t>(header.program_header_count) * sizeof(ProgramHeader);
    return header.magic == kElfMagic &&
        header.elf_class == kElfClass64 &&
        header.data_encoding == kElfDataLittle &&
        header.version == 1 &&
        header.version2 == 1 &&
        (header.type == kElfTypeExec || header.type == kElfTypeShared) &&
        header.machine == kElfMachineX86_64 &&
        header.header_size == sizeof(ElfHeader) &&
        header.program_header_count != 0 &&
        header.program_header_count <= kMaxLoadSegments &&
        header.program_header_entry_size == sizeof(ProgramHeader) &&
        range_within(header.program_header_offset, table_size, size);
}

bool page_ranges_overlap(uint64_t left_start, uint64_t left_end, uint64_t right_start, uint64_t right_end) {
    return left_start < right_end && right_start < left_end;
}

uint8_t* segment_page_pointer(vm::VmSpace& space, uint64_t virtual_page);

// Puntero de kernel al respaldo fisico de una pagina ya mapeada del espacio
// destino. El espacio todavia no es el activo, asi que no se puede escribir por
// su direccion virtual de usuario.
uint8_t* segment_page_pointer(vm::VmSpace& space, uint64_t virtual_page) {
    uint64_t* pml4 = space.pml4_virtual;
    const uint64_t pml4e = pml4[(virtual_page >> 39) & 0x1ff];
    if ((pml4e & vm::kPagePresent) == 0) {
        return nullptr;
    }
    uint64_t* pdpt = vm::physical_to_virtual(pml4e & 0x000ffffffffff000ULL);
    const uint64_t pdpte = pdpt[(virtual_page >> 30) & 0x1ff];
    if ((pdpte & vm::kPagePresent) == 0) {
        return nullptr;
    }
    uint64_t* pd = vm::physical_to_virtual(pdpte & 0x000ffffffffff000ULL);
    const uint64_t pde = pd[(virtual_page >> 21) & 0x1ff];
    if ((pde & vm::kPagePresent) == 0) {
        return nullptr;
    }
    uint64_t* pt = vm::physical_to_virtual(pde & 0x000ffffffffff000ULL);
    const uint64_t pte = pt[(virtual_page >> 12) & 0x1ff];
    if ((pte & vm::kPagePresent) == 0) {
        return nullptr;
    }
    return reinterpret_cast<uint8_t*>(vm::hhdm_offset() + (pte & 0x000ffffffffff000ULL));
}

// Mapea una pagina nueva, en cero, dentro del espacio destino. No pide memoria
// contigua: el stack no la necesita, y pedirla lo hacia fallar por
// fragmentacion justo cuando el sistema ya venia cargado.
bool map_fresh_user_page(vm::VmSpace& address_space, uint64_t virtual_address, uint8_t** out_backing) {
    memory::PageAllocation page = {};
    if (!memory::allocate_page(page)) {
        return false;
    }
    memset(page.virtual_address, 0, memory::kPageSize);
    if (!vm::map_page(address_space, virtual_address, page.physical_address,
                      vm::kPageUser | vm::kPageWrite | vm::kPageNoExecute)) {
        (void)memory::free_allocation(page);
        return false;
    }
    if (out_backing != nullptr) {
        *out_backing = static_cast<uint8_t*>(page.virtual_address);
    }
    return true;
}

// Topes del armado inicial. Existen para que un argv gigante no se coma el
// stack antes de que el programa llegue a main; lo que sobra se descarta y el
// proceso arranca con el argc que de verdad quedo escrito.
constexpr uint64_t kMaxArgumentPages = 16;
constexpr uint64_t kMaxArguments = 128;
constexpr uint64_t kInitialStackPages = kMaxArgumentPages > vm::kUserStackInitialPages
                                            ? kMaxArgumentPages
                                            : vm::kUserStackInitialPages;

uint64_t argument_pages_needed(int argc, const char* const* argv) {
    uint64_t bytes = sizeof(uint64_t) * (static_cast<uint64_t>(argc) + 1);
    for (int index = 0; index < argc; ++index) {
        bytes += strlen(argv[index]) + 1 + 8; // +8 por el alineado de cada cadena
    }
    bytes += 64; // margen para el alineado a 16 del rsp final
    const uint64_t pages = (bytes + memory::kPageSize - 1) / memory::kPageSize;
    return pages < kMaxArgumentPages ? pages : kMaxArgumentPages;
}

// Traduce una direccion de usuario adentro de lo recien mapeado al puntero de
// kernel que la respalda: el espacio destino todavia no es el activo.
uint8_t* stack_kernel_pointer(uint8_t* const* backing, uint64_t mapped_bottom, uint64_t user_address) {
    const uint64_t offset = user_address - mapped_bottom;
    return backing[offset / memory::kPageSize] + (offset % memory::kPageSize);
}

bool build_initial_stack(
    elf::ImageReader read_image,
    void* read_context,
    vm::VmSpace& address_space,
    int argc,
    const char* const* argv,
    int& accepted_argc,
    uint64_t& stack_pointer,
    uint64_t interpreter_offset,
    uint64_t interpreter_length,
    uint64_t& interpreter_address
) {
    uint8_t* backing[kInitialStackPages] = {};
    uint64_t argv_values[kMaxArguments] = {};
    uint64_t user_sp = vm::kUserStackTop;
    int stored_argc = 0;

    if (argc < 0) {
        argc = 0;
    }
    if (static_cast<uint64_t>(argc) > kMaxArguments) {
        argc = static_cast<int>(kMaxArguments);
    }

    // Solo se mapea la punta de la region reservada; el resto aparece por
    // demanda en process::grow_user_stack cuando el programa lo toca.
    {
        const uint64_t needed = argument_pages_needed(argc, argv);
        const uint64_t initial_pages =
            needed > vm::kUserStackInitialPages ? needed : vm::kUserStackInitialPages;
        const uint64_t mapped_bottom = vm::kUserStackTop - (initial_pages * memory::kPageSize);

        for (uint64_t page = 0; page < initial_pages; ++page) {
            if (!map_fresh_user_page(address_space, mapped_bottom + (page * memory::kPageSize),
                                     &backing[page])) {
                return false;
            }
        }

        for (int index = 0; index < argc; ++index) {
            const size_t length = strlen(argv[index]) + 1;
            uint64_t candidate = user_sp - length;
            candidate &= ~static_cast<uint64_t>(0x7);

            // Tiene que quedar lugar tambien para el arreglo de punteros, que
            // se escribe despues y crece con cada argumento aceptado.
            const uint64_t pointers = sizeof(uint64_t) * (static_cast<uint64_t>(index) + 2);
            if (candidate < mapped_bottom || candidate - mapped_bottom < pointers) {
                break;
            }

            user_sp = candidate;
            memcpy(stack_kernel_pointer(backing, mapped_bottom, user_sp), argv[index], length);
            argv_values[index] = user_sp;
            stored_argc = index + 1;
        }

        user_sp &= ~static_cast<uint64_t>(0xf);

        /* La ruta del intérprete se reserva ENTRE las cadenas de argv y el
         * arreglo de punteros, nunca arriba del arreglo.
         *
         * Arriba del arreglo no hay espacio libre: las cadenas quedan justo
         * encima de él, porque el kernel las copia desde la punta hacia abajo y
         * recién después reserva el arreglo. Escribir arriba pisa la cadena del
         * último argumento, y el síntoma es desconcertante porque los punteros
         * de argv quedan correctos: con `calc --selftest` el arreglo quedaba en
         * 0x6fffffffc8, la cadena de "--selftest" en 0x6fffffffe0, y una ruta de
         * intérprete de 28 bytes arrancaba exactamente en 0x6fffffffe0. El
         * programa leía argv[1] = "/lib64/ld-linux-x86-64.so.2".
         *
         * Reservarlo acá, bajando user_sp ANTES de armar el arreglo, deja la
         * pila contigua: cadenas, intérprete, arreglo. Y no mueve rsp: rsp queda
         * en el arreglo, que es lo que recibe el proceso.
         *
         * Se reserva un multiplo de 8 y se escriben p_filesz bytes. El
         * terminador lo aporta el redondeo hacia arriba del espacio reservado,
         * no el contenido de la pagina: depender de que la pagina venga en cero
         * no es una precondicion que se pueda dar por cierta. */
        if (interpreter_length != 0 && interpreter_offset != 0) {
            const uint64_t reserved = (interpreter_length + 7u) & ~static_cast<uint64_t>(7u);
            const uint64_t slot = user_sp - reserved;
            if (slot < mapped_bottom) {
                return false;
            }
            if (read_image(read_context, interpreter_offset,
                           stack_kernel_pointer(backing, mapped_bottom, slot),
                           static_cast<size_t>(interpreter_length))) {
                user_sp = slot;
                interpreter_address = slot;
            }
        }

        user_sp -= static_cast<uint64_t>((stored_argc + 1) * sizeof(uint64_t));
        if (user_sp < mapped_bottom) {
            return false;
        }
        for (int index = 0; index < stored_argc; ++index) {
            const uint64_t slot = user_sp + (static_cast<uint64_t>(index) * sizeof(uint64_t));
            memcpy(stack_kernel_pointer(backing, mapped_bottom, slot), &argv_values[index],
                   sizeof(uint64_t));
        }
        {
            const uint64_t terminator = 0;
            const uint64_t slot = user_sp + (static_cast<uint64_t>(stored_argc) * sizeof(uint64_t));
            memcpy(stack_kernel_pointer(backing, mapped_bottom, slot), &terminator,
                   sizeof(terminator));
        }

        }

    // El argc que recibe el proceso es el que REALMENTE quedo en el arreglo.
    // Antes se copiaban 15 argumentos como maximo pero se pasaba el argc
    // original, asi que un argv mas largo hacia que el programa leyera
    // punteros que nunca se escribieron.
    accepted_argc = stored_argc;
    stack_pointer = user_sp;
    return true;
}
} // namespace

namespace elf {

const char* load_failure_string(LoadFailure failure) {
    switch (failure) {
        case LoadFailure::none:
            return "ok";
        case LoadFailure::bad_header:
            return "bad elf header";
        case LoadFailure::bad_segment:
            return "bad elf segment";
        case LoadFailure::truncated:
            return "truncated segment";
        case LoadFailure::out_of_memory:
            return "out of memory";
    }
    return "unknown";
}

bool load_user_image(
    ImageReader read_image,
    void* context,
    size_t size,
    vm::VmSpace& address_space,
    int argc,
    const char* const* argv,
    LoadResult& result,
    LoadFailure& failure
) {
    ElfHeader header = {};

    failure = LoadFailure::none;

    if (read_image == nullptr || size < sizeof(ElfHeader)) {
        failure = LoadFailure::bad_header;
        return false;
    }
    if (!read_image(context, 0, &header, sizeof(header))) {
        failure = LoadFailure::truncated;
        return false;
    }
    if (!validate_header(header, size)) {
        failure = LoadFailure::bad_header;
        return false;
    }

    ProgramHeader segments[kMaxLoadSegments] = {};
    uint64_t segment_starts[kMaxLoadSegments] = {};
    uint64_t segment_ends[kMaxLoadSegments] = {};
    uint64_t interpreter_offset = 0;
    /* Desplazamiento de carga. Un ET_EXEC tiene direcciones absolutas y se carga
     * donde dice: sesgo cero, exactamente igual que antes. Un ET_DYN (PIE) las
     * tiene relativas y hay que(sumarlas) a una base.
     *
     * La base arranca en kUserBase, que es la misma que usa el linker script, de
     * modo que una imagen PIE ocupa el mismo lugar que una ET_EXEC. Eso es lo
     * unico que cambia aca: elegir una base por proceso con entropia es el
     * trabajo siguiente, y es tambien lo que le da ASLR al ejecutable. */
    const uint64_t load_bias = header.type == kElfTypeShared ? vm::kUserBase : 0;
    uint64_t interpreter_path_length = 0;
    size_t segment_count = 0;
    bool entry_is_executable = false;

    for (uint16_t index = 0; index < header.program_header_count; ++index) {
        ProgramHeader program = {};
        const uint64_t header_offset =
            header.program_header_offset + (static_cast<uint64_t>(index) * sizeof(ProgramHeader));

        if (!read_image(context, header_offset, &program, sizeof(program))) {
            failure = LoadFailure::truncated;
            return false;
        }
        if (program.type == kProgramInterp) {
            /* La ruta del intérprete no se mapea: es una cadena, y copiarla al
             * stack inicial alcanza. Solo se acepta si es una cadena NUL
             * terminada dentro del archivo y de largo sensato; el resto de
             * entradas PT_INTERP se ignoran y la imagen arranca como estatica,
             * que es lo que hacen las imagenes que no declaran uno. */
            if (interpreter_path_length == 0 && program.file_size > 0 &&
                program.file_size <= kMaxInterpreterPath &&
                program.offset + program.file_size <= size) {
                char candidate[kMaxInterpreterPath];
                if (read_image(context, program.offset, candidate, static_cast<size_t>(program.file_size))) {
                    const uint64_t limit = program.file_size;
                    bool terminated = false;
                    for (uint64_t index = 0; index < limit; ++index) {
                        if (candidate[index] == '\0') {
                            terminated = true;
                            break;
                        }
                    }
                    if (terminated && candidate[0] == '/') {
                        interpreter_path_length = limit;
                        interpreter_offset = program.offset;
                    }
                }
            }
            continue;
        }

        if (program.type != kProgramLoad) {
            continue;
        }
        if (segment_count >= kMaxLoadSegments) {
            failure = LoadFailure::bad_segment;
            result.fail_index = index;
            return false;
        }

        const bool executable = (program.flags & kProgramExecutable) != 0;
        const bool writable = (program.flags & kProgramWritable) != 0;
        const uint64_t vaddr = program.virtual_address + load_bias;
        const bool valid_alignment = program.alignment == 0 || program.alignment == 1 ||
            ((program.alignment & (program.alignment - 1)) == 0 &&
             (program.offset % program.alignment) == (vaddr % program.alignment));
        const uint64_t image_end = vaddr + program.memory_size;
        const bool address_range_valid = program.memory_size != 0 && image_end >= program.virtual_address &&
            vaddr >= vm::kUserBase && image_end <= vm::kUserStackGuardBottom;
        const bool mapped_range_valid = address_range_valid &&
            align_up(image_end, memory::kPageSize) >= image_end;
        const bool file_range_valid = program.file_size <= program.memory_size &&
            range_within(program.offset, program.file_size, size);

        if (!valid_alignment) {
            failure = LoadFailure::bad_segment;
            result.fail_index = index;
            return false;
        }
        if (!mapped_range_valid || !file_range_valid) {
            failure = LoadFailure::bad_segment;
            result.fail_index = index;
            return false;
        }
        if (executable && writable) {
            failure = LoadFailure::bad_segment;
            result.fail_index = index;
            return false;
        }

        const uint64_t mapped_start = align_down(vaddr, memory::kPageSize);
        const uint64_t mapped_end = align_up(image_end, memory::kPageSize);
        for (size_t previous = 0; previous < segment_count; ++previous) {
            if (!page_ranges_overlap(mapped_start, mapped_end, segment_starts[previous], segment_ends[previous])) {
                continue;
            }
            /* Compartir pagina entre dos PT_LOAD es un ELF valido y lo produce
             * cualquier binario enlazado con un interprete: el segmento de
             * texto no termina en un borde de pagina y el de datos empieza en esa
             * misma pagina. map_segment_pages ya lo contemplaba --la pagina queda
             * con la union de permisos--, pero esta validacion lo rechazaba, y
             * una imagen legitima no cargaba.
             *
             * Lo que no se admite es que la union sea escribible Y ejecutable:
             * eso seria una pagina W^X que hoy ningun segmento produce por su
             * cuenta. */
            const uint32_t union_flags = program.flags | segments[previous].flags;
            const bool union_writable = (union_flags & kProgramWritable) != 0;
            const bool union_executable = (union_flags & kProgramExecutable) != 0;
            if (union_writable && union_executable) {
                failure = LoadFailure::bad_segment;
                result.fail_index = index;
                return false;
            }
        }

        if (executable && header.entry + load_bias >= vaddr &&
            header.entry + load_bias < vaddr + program.file_size) {
            entry_is_executable = true;
        }
        segments[segment_count] = program;
        segment_starts[segment_count] = mapped_start;
        segment_ends[segment_count] = mapped_end;
        ++segment_count;
    }

    if (segment_count == 0 || !entry_is_executable) {
        failure = LoadFailure::bad_segment;
        result.fail_index = 0xffff;
        return false;
    }

    for (size_t index = 0; index < segment_count; ++index) {
        const ProgramHeader& program = segments[index];
        /* La pagina compartida por dos segmentos necesita la union de los
         * permisos de los dos, no los de este. Se calcula antes de mapear porque
         * despues es tarde: si el ejecutable va primero y el de solo lectura
         * segundo, un OR le devuelve NX al texto y la pagina deja de ser
         * ejecutable. */
        uint64_t page = segment_starts[index];
        while (page < segment_ends[index]) {
            uint32_t union_program_flags = program.flags;
            for (size_t other = 0; other < segment_count; ++other) {
                if (page >= segment_starts[other] && page < segment_ends[other]) {
                    union_program_flags |= segments[other].flags;
                }
            }
            const uint64_t page_flags = vm::kPageUser |
                ((union_program_flags & kProgramWritable) != 0 ? vm::kPageWrite : 0) |
                ((union_program_flags & kProgramExecutable) == 0 ? vm::kPageNoExecute : 0);

            memory::PageAllocation page_allocation = {};
            if (!vm::add_user_page_flags(address_space, page, page_flags)) {
                if (!memory::allocate_page(page_allocation)) {
                    failure = LoadFailure::out_of_memory;
                    return false;
                }
                memset(page_allocation.virtual_address, 0, memory::kPageSize);
                if (!vm::map_page(address_space, page, page_allocation.physical_address, page_flags)) {
                    (void)memory::free_allocation(page_allocation);
                    failure = LoadFailure::out_of_memory;
                    return false;
                }
            }
            page += memory::kPageSize;
        }

        // El contenido del segmento se lee sobre las paginas del proceso, de a
        // una: no hay copia intermedia de la imagen. Lo que el segmento pide de
        // mas que el archivo (el .bss) ya quedo en cero al mapear.
        uint64_t remaining = program.file_size;
        uint64_t written = 0;
        while (remaining != 0) {
            const uint64_t address = program.virtual_address + load_bias + written;
            const uint64_t page = align_down(address, memory::kPageSize);
            const uint64_t page_offset = address & (memory::kPageSize - 1);
            const uint64_t room = memory::kPageSize - page_offset;
            const uint64_t chunk = room < remaining ? room : remaining;
            uint8_t* destination = segment_page_pointer(address_space, page);

            if (destination == nullptr) {
                failure = LoadFailure::out_of_memory;
                return false;
            }
            if (!read_image(context, program.offset + written, destination + page_offset,
                            static_cast<size_t>(chunk))) {
                failure = LoadFailure::truncated;
                return false;
            }
            remaining -= chunk;
            written += chunk;
        }
    }

    if (!build_initial_stack(
            read_image, context, address_space, argc, argv, result.accepted_argc, result.stack_pointer,
            interpreter_offset, interpreter_path_length, result.interpreter_address)) {
        failure = LoadFailure::out_of_memory;
        return false;
    }

    result.entry_point = header.entry + load_bias;
    result.os_abi = header.os_abi;
    return true;
}

} // namespace elf
