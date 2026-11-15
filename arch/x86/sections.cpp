#include "sections.hpp"
#include "io.hpp"
#include "paging.hpp"
#include "kernel/string.hpp"
#include "kernel/vfs.hpp"

// Port of v0.4.0 process.cpp and vmm.cpp: handles and views independently
// retain the same section object. The page tables only borrow its pages.
namespace i386::sections {
namespace {
uint32_t g_failure;
uint32_t access_for(uint32_t flags, uint32_t defaults) {
    uint32_t access = flags & (object::access_read | object::access_write | object::access_execute);
    return access ? access : defaults;
}
int install(process::Process& task, object::SectionObject* section, uint32_t access) {
    if (!section) return -SAVANXP_ENOMEM;
    const int result = io::install_object(task, &section->header,
        access | object::access_query | object::access_synchronize);
    if (result < 0 && !section->header.refcount) {
        object::Header* header = &section->header;
        object::release(header);
    }
    return result;
}
}
uint32_t last_failure() { return g_failure; }

int create(process::Process& task, uint32_t size, uint32_t flags) {
    if (!size) return -SAVANXP_EINVAL;
    const uint32_t access = access_for(flags, object::access_read | object::access_write);
    return install(task, object::create_section(size, access), access);
}
int open(process::Process& task, uint32_t fd, uint32_t offset, uint32_t length, uint32_t flags) {
    if (fd >= io::kMaxHandles || !(task.handles[fd].granted_access & object::access_read))
        return -SAVANXP_EBADF;
    auto* file = object::as_io(task.handles[fd].object);
    if (!file || !file->node) return -SAVANXP_EBADF;
    if (file->node->type != vfs::NodeType::file || (flags & SAVANXP_SECTION_WRITE))
        return -SAVANXP_EINVAL;
    const uint32_t access = access_for(flags, object::access_read) | object::access_read;
    return install(task, object::acquire_file_range_section(*file->node, offset, length, access), access);
}
long map(process::Process& task, uint32_t fd, uint32_t base, uint32_t flags) {
    g_failure = 1;
    if (fd >= io::kMaxHandles) return -SAVANXP_EBADF;
    auto* section = object::as_section(task.handles[fd].object);
    if (!section) return -SAVANXP_EBADF;
    const uint32_t access = access_for(flags, section->access_mask);
    if ((access & task.handles[fd].granted_access) != access ||
        (access & section->access_mask) != access) { g_failure = 2; return -SAVANXP_EACCES; }
    if ((access & object::access_write) && (access & object::access_execute)) return -SAVANXP_EINVAL;
    if (base & 4095) return -SAVANXP_EINVAL;
    const uint32_t size = static_cast<uint32_t>(section->size_bytes);
    if (!base) {
        uint32_t candidate = task.next_view_base ? task.next_view_base : paging::kSectionViewBase;
        bool found = false;
        for (unsigned pass = 0; pass < 2 && !found; ++pass) {
            const uint32_t end = pass ? candidate : paging::kUserWindowEnd;
            for (uint32_t page = pass ? paging::kSectionViewBase : candidate;
                 size <= end && page <= end - size; page += paging::kPageBytes) {
                if (paging::range_free(task.directory, page, size)) { base = page; found = true; break; }
            }
        }
        if (!found) { g_failure = 3; return -SAVANXP_ENOMEM; }
    } else if (!paging::range_free(task.directory, base, size)) {
        g_failure = 4; return -SAVANXP_ENOMEM;
    }
    process::Process::SectionView* view = nullptr;
    for (auto& entry : task.views) if (!entry.section) { view = &entry; break; }
    if (!view) { g_failure = 5; return -SAVANXP_ENOMEM; }
    for (uint32_t offset = 0; offset < size; offset += paging::kPageBytes) {
        if (!paging::map_borrowed_page(task.directory, base + offset,
            static_cast<uint32_t>(section->physical_pages[offset / paging::kPageBytes]),
            (access & object::access_write) != 0)) {
            // Include the failed page to reclaim an empty, newly allocated table.
            paging::unmap_borrowed_range(task.directory, base, offset + paging::kPageBytes);
            g_failure = 6; return -SAVANXP_ENOMEM;
        }
    }
    object::retain(&section->header);
    *view = {base, size, access, section, (flags & SAVANXP_VIEW_PRIVATE) == 0};
    if (task.next_view_base < paging::kSectionViewBase) task.next_view_base = paging::kSectionViewBase;
    if (base + size > task.next_view_base) task.next_view_base = base + size;
    g_failure = 0;
    return base;
}
int unmap(process::Process& task, uint32_t base) {
    for (auto& view : task.views) {
        if (!view.section || view.base != base) continue;
        paging::unmap_borrowed_range(task.directory, base, view.size);
        object::Header* header = &view.section->header;
        object::release(header);
        memset(&view, 0, sizeof(view));
        return 0;
    }
    return -SAVANXP_EINVAL;
}
bool clone_views(const process::Process& source, process::Process& target) {
    for (const auto& view : source.views) {
        if (!view.section) continue;
        auto* section = view.share_on_fork ? view.section : object::clone_section(*view.section);
        const int fd = install(target, section, view.access);
        if (fd < 0) return false;
        const long result = map(target, fd, view.base, view.access |
            (view.share_on_fork ? 0 : SAVANXP_VIEW_PRIVATE));
        io::close(target, fd);
        if (result < 0) return false;
    }
    target.next_view_base = source.next_view_base;
    return true;
}

void release_views(process::Process& task) {
    for (auto& view : task.views) if (view.section) unmap(task, view.base);
    task.next_view_base = 0;
}
}
