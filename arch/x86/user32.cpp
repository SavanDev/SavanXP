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
constexpr uint32_t kStackTop = paging::kUserStackTop;
constexpr uint32_t kStackSize = kStackTop - paging::kUserStackBottom;
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
constexpr uint32_t kSegmentWrite = 2;


bool in_image(uint32_t address, uint32_t size) {
    return address >= paging::kUserWindowBase && address < kStackTop - kStackSize - paging::kPageBytes &&
        static_cast<uint64_t>(address) + size <= kStackTop - kStackSize - paging::kPageBytes;
}

bool read_at(vfs::Vnode& node, uint32_t offset, void* buffer, uint32_t count) {
    return vfs::read(node, offset, buffer, count) == count;
}

} // namespace

int load(const char* path, uint32_t* directory, ContextFrame& frame, uint32_t argc,
         const char* const* argv, bool* native_subsystem) {
    vfs::Vnode* node = vfs::open(path, SAVANXP_OPEN_READ);
    if (node == nullptr) {
        return SAVANXP_ENOENT;
    }
    Elf32Header header = {};
    if (!read_at(*node, 0, &header, sizeof(header)) ||
        memcmp(header.identification, "\x7f" "ELF", 4) != 0 ||
        header.identification[4] != kClass32 || header.identification[5] != kDataLittleEndian || header.identification[6] != 1 ||
        header.version != 1 || header.header_size != sizeof(Elf32Header) ||
        (header.type != kTypeExecutable && header.type != 3) || header.machine != kMachineIntel386 ||
        header.program_entry_size != sizeof(Elf32Segment) || header.program_count == 0 ||
        header.program_count > kMaxSegments) {
        return SAVANXP_ENOEXEC;
    }
    Elf32Segment segments[kMaxSegments] = {};
    if (!read_at(*node, header.program_offset, segments,
                 header.program_count * sizeof(Elf32Segment))) {
        return SAVANXP_ENOEXEC;
    }
    const uint32_t bias = header.type == 3 ? paging::kUserWindowBase : 0;
    if (header.entry > UINT32_MAX - bias) return SAVANXP_ENOEXEC;
    header.entry += bias;
    uint32_t interpreter_offset = 0, interpreter_size = 0;
    for (uint32_t index = 0; index < header.program_count; ++index) {
        auto& segment = segments[index];
        if (segment.type == 3) {
            char path[256];
            if (segment.file_size < 2 || segment.file_size > sizeof(path) ||
                static_cast<uint64_t>(segment.offset) + segment.file_size > node->size ||
                !read_at(*node, segment.offset, path, segment.file_size) ||
                path[0] != '/' || path[segment.file_size - 1] != 0) return SAVANXP_ENOEXEC;
            interpreter_offset = segment.offset; interpreter_size = segment.file_size;
        }
        if (segment.type != kSegmentLoad) continue;
        if (segment.virtual_address > UINT32_MAX - bias) return SAVANXP_ENOEXEC;
        segment.virtual_address += bias;
    }
    uint32_t image_base = 0, lowest_base = UINT32_MAX;
    bool executable_entry = false;
    // Validate the whole image before modifying its private backing memory.
    for (uint32_t index = 0; index < header.program_count; ++index) {
        const auto& segment = segments[index];
        if (segment.type != kSegmentLoad) {
            continue;
        }
        if (!segment.memory_size) continue;
        if (segment.file_size > segment.memory_size ||
            !in_image(segment.virtual_address, segment.memory_size) ||
            (segment.alignment > 1 && ((segment.alignment & (segment.alignment - 1)) != 0 ||
                segment.offset % segment.alignment != segment.virtual_address % segment.alignment)) ||
            (segment.flags & (kSegmentWrite | kSegmentExecute)) == (kSegmentWrite | kSegmentExecute) ||
            static_cast<uint64_t>(segment.offset) + segment.file_size > node->size) {
            return SAVANXP_ENOEXEC;
        }
        if (segment.virtual_address < lowest_base) lowest_base = segment.virtual_address & ~4095u;
        if (segment.offset == 0) image_base = segment.virtual_address & ~4095u;
        for (uint32_t previous = 0; previous < index; ++previous) {
            const auto& other = segments[previous];
            if (other.type != kSegmentLoad || !other.memory_size) continue;
            const uint64_t end = (static_cast<uint64_t>(segment.virtual_address) + segment.memory_size + 4095) & ~4095ull;
            const uint64_t other_end = (static_cast<uint64_t>(other.virtual_address) + other.memory_size + 4095) & ~4095ull;
            if ((segment.virtual_address & ~4095u) < other_end && (other.virtual_address & ~4095u) < end &&
                ((segment.flags | other.flags) & 3) == 3) return SAVANXP_ENOEXEC;
        }
        if ((segment.flags & kSegmentExecute) != 0 && header.entry >= segment.virtual_address &&
            header.entry < static_cast<uint64_t>(segment.virtual_address) + segment.file_size) {
            executable_entry = true;
        }
    }
    if (header.type == 3 && !image_base) return SAVANXP_ENOEXEC;
    if (!image_base) image_base = lowest_base;
    if (!executable_entry || argc > SAVANXP_MAX_ARGC) {
        return SAVANXP_ENOEXEC;
    }
    // Reference VM design: allocate zeroed physical pages and union PT_LOAD
    // permissions when segments share a page; holes and the stack guard stay absent.
    for (uint32_t index = 0; index < header.program_count; ++index) {
        const auto& segment = segments[index];
        if (segment.type != kSegmentLoad || segment.memory_size == 0) continue;
        if (!paging::map_user_range(directory, segment.virtual_address, segment.memory_size,
                                    (segment.flags & kSegmentWrite) != 0)) return SAVANXP_ENOMEM;
        uint32_t copied = 0;
        while (copied < segment.file_size) {
            const uint32_t address = segment.virtual_address + copied;
            uint32_t chunk = paging::kPageBytes - (address & (paging::kPageBytes - 1));
            if (chunk > segment.file_size - copied) chunk = segment.file_size - copied;
            if (!read_at(*node, segment.offset + copied, paging::user_pointer(directory, address),
                         chunk)) return SAVANXP_EIO;
            copied += chunk;
        }
    }
    if (!paging::map_user_range(directory, kStackTop - 8 * paging::kPageBytes, 8 * paging::kPageBytes, true))
        return SAVANXP_ENOMEM;
    uint32_t pointers[SAVANXP_MAX_ARGC + 1] = {};
    uint32_t stack = kStackTop;
    for (uint32_t index = 0; index < argc; ++index) {
        const uint32_t length = static_cast<uint32_t>(strlen(argv[index])) + 1;
        if (length > SAVANXP_MAX_ARG_LENGTH + 1) {
            return SAVANXP_E2BIG;
        }
        stack = (stack - length) & ~3u;
        pointers[index] = stack;
        paging::copy_to_user(directory, stack, argv[index], length);
    }
    uint32_t interpreter_address = 0;
    if (interpreter_size) {
        stack = (stack - interpreter_size) & ~3u;
        interpreter_address = stack;
        char interpreter[256];
        if (!read_at(*node, interpreter_offset, interpreter, interpreter_size) ||
            !paging::copy_to_user(directory, stack, interpreter, interpreter_size)) return SAVANXP_EIO;
    }
    stack = (stack - (argc + 1) * sizeof(uint32_t)) & ~15u;
    paging::copy_to_user(directory, stack, pointers, (argc + 1) * sizeof(uint32_t));
    initialize_user_frame(frame, header.entry, stack, image_base, argc, stack);
    frame.ecx = interpreter_address;
    if (native_subsystem) *native_subsystem = header.identification[7] == 0x53;
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
