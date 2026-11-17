// Architecture boundaries for the unchanged v0.4.0 framebuffer GPU backend.
#include "fb32.hpp"
#include "paging.hpp"
#include "process.hpp"
#include "ps2_32.hpp"
#include "serial.hpp"
#include "kernel/console.hpp"
#include "kernel/display.hpp"
#include "kernel/process.hpp"
#include "kernel/ui.hpp"
#include "kernel/vmm.hpp"

namespace process {
Process* current() { return i386::process::current(); }
uint32_t current_pid() { return i386::process::current_pid(); }
bool validate_user_range(uint64_t address, size_t size, bool writable) {
    return address <= UINT32_MAX && address + size <= UINT32_MAX &&
        i386::process::user_range(static_cast<uint32_t>(address), size, writable);
}
bool copy_to_user(uint64_t address, const void* source, size_t size) {
    return validate_user_range(address, size, true) &&
        i386::paging::copy_to_user(current()->directory, address, source, size);
}
bool copy_from_user(void* destination, uint64_t address, size_t size) {
    return validate_user_range(address, size, false) &&
        i386::paging::copy_from_user(current()->directory, destination, address, size);
}
}
namespace vm {
bool map_kernel_pages(const uint64_t* pages, uint64_t count, uint64_t flags, void** base) {
    return count <= UINT32_MAX && i386::paging::map_kernel_pages(pages, count, flags, base);
}
bool unmap_kernel_pages(void* base, uint64_t count) {
    return count <= UINT32_MAX && i386::paging::unmap_kernel_pages(base, count);
}
// GRUB's device framebuffer uses an identity mapping. RAM has a separate
// supervisor direct map; these offsets must never be interchanged.
uint64_t hhdm_offset() { return 0; }
bool write_combining_page_flags(uint64_t&) { return false; }
bool kernel_page_cache_flags(uint64_t, uint64_t& flags) { flags = 0; return true; }
bool map_kernel_device_memory(uint64_t physical, size_t size, uint64_t, void** base) {
    // The backend remaps its aperture during boot, before user CR3s exist.
    if (!i386::paging::map_identity(physical, size)) return false;
    *base = reinterpret_cast<void*>(static_cast<uintptr_t>(physical));
    return true;
}
}
namespace ui {
namespace { uint32_t g_owner; }
bool graphics_active() { return g_owner != 0; }
bool owns_graphics_session(uint32_t pid) { return pid && g_owner == pid; }
bool acquire_graphics_session(uint32_t pid) {
    if (!pid || (g_owner && g_owner != pid)) return false;
    if (g_owner == pid) return true;
    g_owner = pid;
    i386::ps2_32::clear_queue();
    i386::fb32::console_set_enabled(false);
    return true;
}
void release_graphics_session(uint32_t pid) {
    if (!owns_graphics_session(pid)) return;
    g_owner = 0;
    i386::ps2_32::clear_queue();
    i386::fb32::console_set_enabled(true);
}
void sync_framebuffer_geometry() {
    i386::fb32::set_external(display::framebuffer_address(), display::framebuffer_info());
}
}
namespace console {
void set_external_framebuffer(void* address, const savanxp_fb_info& info) {
    i386::fb32::set_external(address, info);
}
void printf(const char* format, ...) {
    // The backend's diagnostics use strings and unsigned decimal fields only.
    i386::serial::SerialOnly serial_only;
    va_list args;
    va_start(args, format);
    for (const char* p = format; *p; ++p) {
        if (*p != '%') { i386::serial::write_char(*p); continue; }
        ++p;
        if (*p == 'u') i386::serial::write_dec(va_arg(args, unsigned));
        else if (*p == 's') i386::serial::write(va_arg(args, const char*));
        else if (*p == '%') i386::serial::write_char('%');
    }
    va_end(args);
}
}
