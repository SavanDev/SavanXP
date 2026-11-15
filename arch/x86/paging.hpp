#pragma once
#include <stdint.h>

namespace i386::paging {
constexpr uint32_t kUserWindowBase = 0x08000000u;
constexpr uint32_t kUserWindowEnd = 0x40000000u;
constexpr uint32_t kPageBytes = 4096;
constexpr uint32_t kUserStackTop = 0x09000000u;
constexpr uint32_t kUserStackBottom = kUserStackTop - 1024u * 1024u;
constexpr uint32_t kUserStackGuard = kUserStackBottom - kPageBytes;
constexpr uint32_t kSectionViewBase = 0x10000000u;
// Supervisor aliases of physical 0..1 GiB, independent of user CR3 mappings.
constexpr uint32_t kDirectMapBase = 0x40000000u;
constexpr uint32_t kDirectMapBytes = 0x40000000u;

bool cpu_has_pse();
void enable_identity_1gib();
bool map_identity(uint64_t address, uint32_t bytes);
void initialize_process_directory(uint32_t* directory);
void destroy_process_directory(uint32_t* directory);
bool map_user_range(uint32_t* directory, uint32_t address, uint32_t bytes, bool writable);
bool user_range(const uint32_t* directory, uint32_t address, uint32_t bytes, bool writable);
void* user_pointer(const uint32_t* directory, uint32_t address);
// Copy through physical aliases, including ranges crossing noncontiguous pages.
bool copy_to_user(uint32_t* directory, uint32_t address, const void* source, uint32_t bytes);
bool copy_from_user(const uint32_t* directory, void* destination, uint32_t address, uint32_t bytes);
uint32_t resident_bytes(const uint32_t* directory);
bool clone_private_pages(const uint32_t* source, uint32_t* destination);
bool range_free(const uint32_t* directory, uint32_t address, uint32_t bytes);
bool map_borrowed_page(uint32_t* directory, uint32_t address, uint32_t physical, bool writable);
void unmap_borrowed_range(uint32_t* directory, uint32_t address, uint32_t bytes);
void activate(const uint32_t* directory);
void activate_kernel();
}
