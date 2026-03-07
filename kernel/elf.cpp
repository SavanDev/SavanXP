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

bool build_initial_stack(
    vm::VmSpace& address_space,
    int argc,
    const char* const* argv,
    uint64_t& stack_pointer
) {
    memory::PageAllocation stack_allocation = {};
    if (!memory::allocate_contiguous_pages(vm::kUserStackPages, stack_allocation)) {
        return false;
    }

    auto* stack_base = static_cast<uint8_t*>(stack_allocation.virtual_address);
    memset(stack_base, 0, vm::kUserStackPages * memory::kPageSize);

    const uint64_t stack_bottom = vm::kUserStackTop - (vm::kUserStackPages * memory::kPageSize);
    for (uint64_t page = 0; page < vm::kUserStackPages; ++page) {
        if (!vm::map_page(
                address_space,
                stack_bottom + (page * memory::kPageSize),
                stack_allocation.physical_address + (page * memory::kPageSize),
                vm::kPageUser | vm::kPageWrite)) {
            return false;
        }
    }

    uint64_t user_sp = vm::kUserStackTop;
    uint64_t argv_values[16] = {};
    const int limited_argc = argc < 16 ? argc : 15;

    for (int index = limited_argc - 1; index >= 0; --index) {
        const size_t length = strlen(argv[index]) + 1;
        user_sp -= length;
        user_sp &= ~static_cast<uint64_t>(0x7);

        const uint64_t offset = user_sp - stack_bottom;
        memcpy(stack_base + offset, argv[index], length);
        argv_values[index] = user_sp;
    }

    user_sp &= ~static_cast<uint64_t>(0xf);
    user_sp -= static_cast<uint64_t>((limited_argc + 1) * sizeof(uint64_t));
    const uint64_t argv_offset = user_sp - stack_bottom;
    auto* user_argv = reinterpret_cast<uint64_t*>(stack_base + argv_offset);
    for (int index = 0; index < limited_argc; ++index) {
        user_argv[index] = argv_values[index];
    }
    user_argv[limited_argc] = 0;

    stack_pointer = user_sp;
    return true;
}

} // namespace

namespace elf {

bool load_user_image(
    const void* image,
    size_t size,
    vm::VmSpace& address_space,
    int argc,
    const char* const* argv,
    LoadResult& result
) {
    if (image == nullptr || size < sizeof(ElfHeader)) {
        return false;
    }

    const auto& header = *static_cast<const ElfHeader*>(image);
    if (!validate_header(header, size)) {
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
            return false;
        }

        const uint64_t mapped_start = align_down(program.virtual_address, memory::kPageSize);
        const uint64_t mapped_end = align_up(program.virtual_address + program.memory_size, memory::kPageSize);
        const uint64_t flags = vm::kPageUser | ((program.flags & kProgramWritable) != 0 ? vm::kPageWrite : 0);

        if (!map_segment_pages(address_space, mapped_start, mapped_end, flags)) {
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

    if (!build_initial_stack(address_space, argc, argv, result.stack_pointer)) {
        return false;
    }

    result.entry_point = header.entry;
    return true;
}

} // namespace elf
