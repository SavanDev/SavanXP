#include "kernel/object.hpp"
#include "kernel/heap.hpp"
#include "kernel/string.hpp"
#include "kernel/sxfs.hpp"
#include "kernel/vfs.hpp"

// Extracted from v0.4.0 object.cpp; section identity, allocation and ownership
// remain shared between architectures. Physical aliases come from the allocator.
namespace object {
namespace {
SectionObject g_section_objects[kMaxSectionObjects] = {};
constexpr uint64_t kMaxSectionBytes = 64ULL << 20;
uint64_t align_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}
} // namespace

void destroy_section(Header* header) {
    SectionObject* section_object = as_section(header);
    if (section_object == nullptr) {
        return;
    }

    if (section_object->physical_pages != nullptr) {
        for (uint64_t page_index = 0; page_index < section_object->page_count; ++page_index) {
            if (section_object->physical_pages[page_index] != 0) {
                (void)memory::free_pages(section_object->physical_pages[page_index], 1);
            }
        }
        heap::free(section_object->physical_pages);
    }

    memset(section_object, 0, sizeof(*section_object));
}

SectionObject* as_section(Header* object) {
    if (object == nullptr || object->type != Type::section) {
        return nullptr;
    }
    return reinterpret_cast<SectionObject*>(object);
}

const SectionObject* as_section(const Header* object) {
    if (object == nullptr || object->type != Type::section) {
        return nullptr;
    }
    return reinterpret_cast<const SectionObject*>(object);
}

// Reserve a section slot and zeroed physical pages with the requested grant.

static SectionObject* allocate_section(uint64_t aligned_size, uint64_t page_count, uint32_t access_mask) {
    for (SectionObject& section_object : g_section_objects) {
        if (section_object.in_use) {
            continue;
        }

        memset(&section_object, 0, sizeof(section_object));
        section_object.physical_pages = static_cast<uint64_t*>(
            heap::allocate(static_cast<size_t>(page_count * sizeof(uint64_t)), alignof(uint64_t)));
        if (section_object.physical_pages == nullptr) {
            return nullptr;
        }
        memset(section_object.physical_pages, 0, static_cast<size_t>(page_count * sizeof(uint64_t)));

        for (uint64_t page_index = 0; page_index < page_count; ++page_index) {
            memory::PageAllocation page = {};
            if (!memory::allocate_page(page)) {
                for (uint64_t rollback = 0; rollback < page_index; ++rollback) {
                    if (section_object.physical_pages[rollback] != 0) {
                        (void)memory::free_pages(section_object.physical_pages[rollback], 1);
                    }
                }
                heap::free(section_object.physical_pages);
                memset(&section_object, 0, sizeof(section_object));
                return nullptr;
            }

            memset(page.virtual_address, 0, memory::kPageSize);
            section_object.physical_pages[page_index] = page.physical_address;
        }

        section_object.header.type = Type::section;
        section_object.header.destroy = destroy_section;
        section_object.in_use = true;
        section_object.access_mask = access_mask;
        section_object.size_bytes = aligned_size;
        section_object.page_count = page_count;
        section_object.allocation = {
            .physical_address = 0,
            .virtual_address = nullptr,
            .page_count = page_count,
        };
        return &section_object;
    }

    return nullptr;
}

SectionObject* create_section(uint64_t size_bytes, uint32_t access_mask) {
    if (size_bytes == 0 || size_bytes > kMaxSectionBytes) {
        return nullptr;
    }

    const uint64_t aligned_size = align_up(size_bytes, memory::kPageSize);
    if (aligned_size < size_bytes) {
        return nullptr;
    }
    const uint64_t page_count = aligned_size / memory::kPageSize;
    if (page_count == 0 || page_count > SIZE_MAX / sizeof(uint64_t)) {
        return nullptr;
    }
    return allocate_section(aligned_size, page_count, access_mask);
}

// Live global objects make physical sharing observable across processes.

void section_live_counts(uint32_t& live, uint32_t& file_backed) {
    live = 0;
    file_backed = 0;
    for (const SectionObject& section_object : g_section_objects) {
        if (!section_object.in_use) {
            continue;
        }
        live++;
        if (section_object.file_backed != 0) {
            file_backed++;
        }
    }
}

static SectionObject* find_file_section(uint16_t volume, uint32_t inode_id, uint64_t offset, uint64_t length) {
    if (inode_id == 0) {
        return nullptr;
    }
    for (SectionObject& section_object : g_section_objects) {
        if (!section_object.in_use || section_object.file_backed == 0) {
            continue;
        }
        // The backing identity includes the byte range, not just the inode.

        if (section_object.source_volume != volume || section_object.source_inode_id != inode_id ||
            section_object.source_offset != offset ||
            section_object.source_length != length) {
            continue;
        }
        return &section_object;
    }
    return nullptr;
}

SectionObject* acquire_file_range_section(
    vfs::Vnode& node,
    uint64_t offset,
    uint64_t length,
    uint32_t access_mask) {
    uint64_t size_bytes = length != 0 ? length : node.size;
    if (offset > node.size || size_bytes > node.size - offset) {
        return nullptr;
    }
    if (size_bytes == 0 || size_bytes > kMaxSectionBytes) {
        return nullptr;
    }

    const uint64_t aligned_size = align_up(size_bytes, memory::kPageSize);
    if (aligned_size < size_bytes) {
        return nullptr;
    }
    const uint64_t page_count = aligned_size / memory::kPageSize;
    if (page_count == 0 || page_count > SIZE_MAX / sizeof(uint64_t)) {
        return nullptr;
    }

    // Initramfs has no stable SxFS inode identity and is deliberately not cached.

    uint32_t inode_id = 0;
    if (node.backend == vfs::Backend::sxfs) {
        sxfs::FileRecord* record = sxfs::file_from_vnode(node);
        if (record == nullptr) {
            return nullptr;
        }
        inode_id = record->inode_id;
        if (inode_id == 0) {
            return nullptr;
        }
        if (SectionObject* existing = find_file_section(record->volume, inode_id, offset, size_bytes)) {
            // The installing handle takes the reference, just as for anonymous sections.

            // The immutable backing may acquire an execute grant later. Existing
            // handle grants remain unchanged; writable file views stay forbidden.
            existing->access_mask |= access_mask;
            return existing;
        }
    }

    SectionObject* section_object = allocate_section(aligned_size, page_count, access_mask);
    if (section_object == nullptr) {
        return nullptr;
    }

    section_object->file_backed = 1;
    section_object->source_inode_id = inode_id;
    if (inode_id) section_object->source_volume = sxfs::file_from_vnode(node)->volume;
    section_object->source_offset = offset;
    section_object->source_length = size_bytes;

    // Read into zeroed pages; a short read rolls back the whole section.

    uint64_t filled = 0;
    while (filled < size_bytes) {
        const uint64_t chunk = (size_bytes - filled) > memory::kPageSize
            ? static_cast<uint64_t>(memory::kPageSize)
            : (size_bytes - filled);
        void* buffer = memory::physical_to_virtual(section_object->physical_pages[filled / memory::kPageSize]);
        const size_t got = vfs::read(node, static_cast<size_t>(offset + filled), buffer, static_cast<size_t>(chunk));
        if (got != chunk) {
            object::Header* header = &section_object->header;
            object::release(header);
            return nullptr;
        }
        filled += got;
    }

    return section_object;
}

void invalidate_file_sections(vfs::Vnode& node) {
    if (node.backend != vfs::Backend::sxfs) return;
    const auto* record = sxfs::file_from_vnode(node);
    if (!record) return;
    for (auto& section : g_section_objects) {
        // Retain old snapshots for existing views, but remove their cache key.
        // A later open loads the new bytes even if size and inode are unchanged.
        if (section.in_use && section.file_backed && section.source_volume == record->volume && section.source_inode_id == record->inode_id)
            section.source_inode_id = 0;
    }
}

SectionObject* clone_section(const SectionObject& source) {
    if (!source.in_use) {
        return nullptr;
    }

    SectionObject* section_object = create_section(source.size_bytes, source.access_mask);
    if (section_object == nullptr) {
        return nullptr;
    }

    uint64_t remaining = source.size_bytes;
    for (uint64_t page_index = 0; page_index < source.page_count; ++page_index) {
        const size_t page_bytes = static_cast<size_t>(remaining > memory::kPageSize ? memory::kPageSize : remaining);
        memcpy(
            memory::physical_to_virtual(section_object->physical_pages[page_index]),
            memory::physical_to_virtual(source.physical_pages[page_index]),
            page_bytes);
        if (remaining <= memory::kPageSize) {
            break;
        }
        remaining -= memory::kPageSize;
    }
    return section_object;
}


} // namespace object
