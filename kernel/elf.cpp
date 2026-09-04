#include "kernel/elf.hpp"

#include "kernel/physical_memory.hpp"
#include "kernel/string.hpp"

namespace {

constexpr uint32_t kElfMagic = 0x464c457fU;
constexpr uint32_t kElfClass64 = 2;
constexpr uint32_t kElfDataLittle = 1;
constexpr uint16_t kElfTypeExec = 2;
constexpr uint16_t kElfMachineX86_64 = 62;
constexpr uint32_t kProgramLoad = 1;
constexpr uint32_t kProgramWritable = 1u << 1;

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

bool validate_header(const ElfHeader& header, size_t size) {
    return header.magic == kElfMagic &&
        header.elf_class == kElfClass64 &&
        header.data_encoding == kElfDataLittle &&
        header.type == kElfTypeExec &&
        header.machine == kElfMachineX86_64 &&
        header.program_header_offset < size &&
        header.program_header_entry_size == sizeof(ProgramHeader) &&
        (header.program_header_offset + (header.program_header_count * sizeof(ProgramHeader))) <= size;
}

bool map_segment_pages(vm::VmSpace& space, uint64_t start, uint64_t end, uint64_t flags) {
    for (uint64_t page = start; page < end; page += memory::kPageSize) {
        memory::PageAllocation page_allocation = {};
        if (!memory::allocate_page(page_allocation)) {
            return false;
        }

        memset(page_allocation.virtual_address, 0, memory::kPageSize);
        if (!vm::map_page(space, page, page_allocation.physical_address, flags)) {
            // Sin mapear, destroy_address_space no la ve: hay que devolverla aca
            // o cada carga fallida se lleva una pagina puesta.
            (void)memory::free_allocation(page_allocation);
            return false;
        }
    }
    return true;
}

void copy_segment_page(
    uint64_t virtual_page,
    uint64_t page_offset,
    const uint8_t* source,
    size_t size,
    vm::VmSpace& space
) {
    uint64_t* pml4 = space.pml4_virtual;
    const uint64_t pml4e = pml4[(virtual_page >> 39) & 0x1ff];
    uint64_t* pdpt = vm::physical_to_virtual(pml4e & 0x000ffffffffff000ULL);
    const uint64_t pdpte = pdpt[(virtual_page >> 30) & 0x1ff];
    uint64_t* pd = vm::physical_to_virtual(pdpte & 0x000ffffffffff000ULL);
    const uint64_t pde = pd[(virtual_page >> 21) & 0x1ff];
    uint64_t* pt = vm::physical_to_virtual(pde & 0x000ffffffffff000ULL);
    const uint64_t pte = pt[(virtual_page >> 12) & 0x1ff];
    auto* target = reinterpret_cast<uint8_t*>(vm::hhdm_offset() + (pte & 0x000ffffffffff000ULL));
    memcpy(target + page_offset, source, size);
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
                      vm::kPageUser | vm::kPageWrite)) {
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
    vm::VmSpace& address_space,
    int argc,
    const char* const* argv,
    int& accepted_argc,
    uint64_t& stack_pointer
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
        case LoadFailure::truncated:
            return "truncated segment";
        case LoadFailure::out_of_memory:
            return "out of memory";
    }
    return "unknown";
}

bool load_user_image(
    const void* image,
    size_t size,
    vm::VmSpace& address_space,
    int argc,
    const char* const* argv,
    LoadResult& result,
    LoadFailure& failure
) {
    failure = LoadFailure::none;

    if (image == nullptr || size < sizeof(ElfHeader)) {
        failure = LoadFailure::bad_header;
        return false;
    }

    const auto& header = *static_cast<const ElfHeader*>(image);
    if (!validate_header(header, size)) {
        failure = LoadFailure::bad_header;
        return false;
    }

    const auto* program_headers = reinterpret_cast<const ProgramHeader*>(
        static_cast<const uint8_t*>(image) + header.program_header_offset
    );

    for (uint16_t index = 0; index < header.program_header_count; ++index) {
        const ProgramHeader& program = program_headers[index];
        if (program.type != kProgramLoad || program.memory_size == 0) {
            continue;
        }

        if ((program.offset + program.file_size) > size) {
            failure = LoadFailure::truncated;
            return false;
        }

        const uint64_t mapped_start = align_down(program.virtual_address, memory::kPageSize);
        const uint64_t mapped_end = align_up(program.virtual_address + program.memory_size, memory::kPageSize);
        const uint64_t flags = vm::kPageUser | ((program.flags & kProgramWritable) != 0 ? vm::kPageWrite : 0);

        if (!map_segment_pages(address_space, mapped_start, mapped_end, flags)) {
            failure = LoadFailure::out_of_memory;
            return false;
        }

        const auto* source = static_cast<const uint8_t*>(image) + program.offset;
        uint64_t remaining = program.file_size;
        uint64_t source_offset = 0;
        while (remaining != 0) {
            const uint64_t page = align_down(program.virtual_address + source_offset, memory::kPageSize);
            const uint64_t page_offset = (program.virtual_address + source_offset) & (memory::kPageSize - 1);
            const uint64_t chunk = (memory::kPageSize - page_offset) < remaining
                ? (memory::kPageSize - page_offset)
                : remaining;
            copy_segment_page(page, page_offset, source + source_offset, static_cast<size_t>(chunk), address_space);
            remaining -= chunk;
            source_offset += chunk;
        }
    }

    if (!build_initial_stack(address_space, argc, argv, result.accepted_argc, result.stack_pointer)) {
        failure = LoadFailure::out_of_memory;
        return false;
    }

    result.entry_point = header.entry;
    result.os_abi = header.os_abi;
    return true;
}

} // namespace elf
