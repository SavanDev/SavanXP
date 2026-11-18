// Architecture boundaries for the unchanged v0.4.0 framebuffer GPU backend.
#include "fb32.hpp"
#include "paging.hpp"
#include "process.hpp"
#include "ps2_32.hpp"
#include "serial.hpp"
#include "io.hpp"
#include "waits.hpp"
#include "kernel/boot_screen.hpp"
#include "kernel/console.hpp"
#include "kernel/display.hpp"
#include "kernel/process.hpp"
#include "kernel/ui.hpp"
#include "kernel/virtio_input.hpp"
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
int export_handle(object::Header* header, uint32_t access, uint32_t flags) {
    if (!current() || !header) return -SAVANXP_EBADF;
    const int fd = i386::io::install_object(*current(), header, access);
    if (fd >= 0) current()->handles[fd].flags = flags;
    return fd;
}
void notify_object_signal(object::Header*) { i386::waits::service(); }
}
namespace vm {
bool map_kernel_pages(const uint64_t* pages, uint64_t count, uint64_t flags, void** base) {
    return count <= UINT32_MAX && i386::paging::map_kernel_pages(pages, count, flags, base);
}
bool unmap_kernel_pages(void* base, uint64_t count) {
    return count <= UINT32_MAX && i386::paging::unmap_kernel_pages(base, count);
}
bool map_kernel_mmio(uint64_t physical, size_t size, uint64_t, void** base) {
    return i386::paging::map_kernel_mmio(physical, size, base);
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
    virtio_input::begin_graphics_session();
    i386::fb32::console_set_enabled(false);
    boot_screen::finish();
    return true;
}
void release_graphics_session(uint32_t pid) {
    if (!owns_graphics_session(pid)) return;
    g_owner = 0;
    virtio_input::end_graphics_session();
    i386::ps2_32::clear_queue();
    i386::fb32::console_set_enabled(true);
}
void sync_framebuffer_geometry() {
    i386::fb32::set_external(display::framebuffer_address(), display::framebuffer_info());
    virtio_input::set_framebuffer_extent(display::framebuffer_info().width, display::framebuffer_info().height);
}
}
namespace console {
void write(const char* text) { i386::serial::SerialOnly serial_only; i386::serial::write(text); }
void serial_write(const char* bytes, size_t count) {
    i386::serial::SerialOnly serial_only;
    for (size_t i = 0; i < count; ++i) i386::serial::write_char(bytes[i]);
}
void write_line(const char* text) { write(text); write("\n"); }
void set_external_framebuffer(void* address, const savanxp_fb_info& info) {
    i386::fb32::set_external(address, info);
}
namespace {
void number(uint64_t value, unsigned base) {
    char buffer[32]; unsigned count = 0;
    do { buffer[count++] = "0123456789abcdef"[value % base]; value /= base; } while (value);
    while (count) i386::serial::write_char(buffer[--count]);
}
}
void vprintf(const char* format, va_list args) {
    i386::serial::SerialOnly serial_only;
    for (const char* p = format; *p; ++p) {
        if (*p != '%') { i386::serial::write_char(*p); continue; }
        ++p;
        unsigned width = 0;
        while (*p == 'l') { ++width; ++p; }
        if (*p == 'u' || *p == 'x') {
            const uint64_t value = width > 1 ? va_arg(args, uint64_t) :
                width ? va_arg(args, unsigned long) : va_arg(args, unsigned);
            number(value, *p == 'x' ? 16 : 10);
        } else if (*p == 'd' || *p == 'i') {
            const int value = va_arg(args, int);
            if (value < 0) i386::serial::write_char('-');
            number(value < 0 ? 0u - static_cast<unsigned>(value) : value, 10);
        } else if (*p == 's') {
            const char* value = va_arg(args, const char*);
            i386::serial::write(value ? value : "(null)");
        } else if (*p == 'c') i386::serial::write_char(static_cast<char>(va_arg(args, int)));
        else if (*p == 'p') { i386::serial::write("0x"); number(reinterpret_cast<uintptr_t>(va_arg(args, void*)), 16); }
        else if (*p == '%') i386::serial::write_char('%');
        else if (!*p) break;
    }
}
void printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
}
}
