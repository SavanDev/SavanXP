#include "user32.hpp"

#include "boot_state.hpp"
#include "cpu.hpp"
#include "kernel/string.hpp"
#include "kernel/vfs.hpp"
#include "paging.hpp"
#include "process.hpp"
#include "savanxp/syscall.h"
#include "serial.hpp"

namespace i386::user32 {
namespace {
constexpr uint32_t kStackTop = paging::kUserWindowEnd;
constexpr uint32_t kStackSize = 64u * 1024u;
constexpr uint32_t kMaxSegments = 16;
struct Elf32Header {
    uint8_t identification[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint32_t entry;
    uint32_t program_offset;
    uint32_t section_offset;
    uint32_t flags;
    // Keep the complete ELF32 header layout, including unused section fields.
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


bool in_image(uint32_t address, uint32_t size) {
    return address >= paging::kUserWindowBase && address < kStackTop - kStackSize &&
        static_cast<uint64_t>(address) + size <= kStackTop - kStackSize;
}

bool read_at(vfs::Vnode& node, uint32_t offset, void* buffer, uint32_t count) {
    return vfs::read(node, offset, buffer, count) == count;
}

} // namespace

int load(const char* path, uint8_t* memory, ContextFrame& frame, uint32_t argc,
         const char* const* argv) {
    vfs::Vnode* node = vfs::open(path, SAVANXP_OPEN_READ);
    if (node == nullptr) {
        return SAVANXP_ENOENT;
    }
    Elf32Header header = {};
    if (!read_at(*node, 0, &header, sizeof(header)) ||
        memcmp(header.identification, "\x7f" "ELF", 4) != 0 ||
        header.identification[4] != kClass32 || header.identification[5] != kDataLittleEndian ||
        header.type != kTypeExecutable || header.machine != kMachineIntel386 ||
        header.program_entry_size != sizeof(Elf32Segment) || header.program_count == 0 ||
        header.program_count > kMaxSegments) {
        return SAVANXP_ENOEXEC;
    }
    Elf32Segment segments[kMaxSegments] = {};
    if (!read_at(*node, header.program_offset, segments,
                 header.program_count * sizeof(Elf32Segment))) {
        return SAVANXP_ENOEXEC;
    }
    uint32_t image_base = 0;
    bool executable_entry = false;
    // Validate the whole image before modifying its private backing memory.
    for (uint32_t index = 0; index < header.program_count; ++index) {
        const auto& segment = segments[index];
        if (segment.type != kSegmentLoad) {
            continue;
        }
        if (segment.file_size > segment.memory_size ||
            !in_image(segment.virtual_address, segment.memory_size) ||
            segment.alignment == 0 || (segment.alignment & (segment.alignment - 1)) != 0 ||
            static_cast<uint64_t>(segment.offset) + segment.file_size > node->size) {
            return SAVANXP_ENOEXEC;
        }
        if (image_base == 0 || segment.virtual_address < image_base) {
            image_base = segment.virtual_address;
        }
        if ((segment.flags & kSegmentExecute) != 0 && header.entry >= segment.virtual_address &&
            header.entry < static_cast<uint64_t>(segment.virtual_address) + segment.memory_size) {
            executable_entry = true;
        }
    }
    if (!executable_entry || argc > SAVANXP_MAX_ARGC) {
        return SAVANXP_ENOEXEC;
    }
    memset(memory, 0, paging::kUserWindowEnd - paging::kUserWindowBase);
    for (uint32_t index = 0; index < header.program_count; ++index) {
        const auto& segment = segments[index];
        if (segment.type == kSegmentLoad && segment.file_size != 0 &&
            !read_at(*node, segment.offset, memory + segment.virtual_address - paging::kUserWindowBase,
                     segment.file_size)) {
            return SAVANXP_EIO;
        }
    }
    uint32_t pointers[SAVANXP_MAX_ARGC + 1] = {};
    uint32_t stack = kStackTop;
    for (uint32_t index = 0; index < argc; ++index) {
        const uint32_t length = static_cast<uint32_t>(strlen(argv[index])) + 1;
        if (length > SAVANXP_MAX_ARG_LENGTH + 1) {
            return SAVANXP_E2BIG;
        }
        stack = (stack - length) & ~3u;
        pointers[index] = stack;
        memcpy(memory + stack - paging::kUserWindowBase, argv[index], length);
    }
    stack = (stack - (argc + 1) * sizeof(uint32_t)) & ~15u;
    memcpy(memory + stack - paging::kUserWindowBase, pointers, (argc + 1) * sizeof(uint32_t));
    initialize_user_frame(frame, header.entry, stack, image_base, argc, stack);
    serial::write("user: ");
    serial::write(path);
    serial::write(" loaded at ");
    serial::write_hex(image_base);
    serial::write(", entry ");
    serial::write_hex(header.entry);
    serial::write(", stack ");
    serial::write_hex(stack);
    serial::write("\n");
    return 0;
}

[[noreturn]] void run(const char* path) {
    process::initialize();
    if (boot_state::state().sxfs_mounted != 0) {
        const char* arguments[] = {path};
        const long pid = process::spawn(path, arguments, 1);
        if (pid < 0) {
            serial::write("process: cannot load boot program\n");
            cpu::halt_forever();
        }
    }
    process::start();
}

} // namespace i386::user32
