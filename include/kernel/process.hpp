#pragma once

#include "../../arch/x86/process.hpp"

namespace process {
using Process = i386::process::Process;
constexpr size_t kMaxFileHandles = 64;
Process* current();
uint32_t current_pid();
bool validate_user_range(uint64_t address, size_t size, bool writable);
bool copy_to_user(uint64_t address, const void* source, size_t size);
bool copy_from_user(void* destination, uint64_t address, size_t size);
int export_handle(object::Header* header, uint32_t access, uint32_t flags);
void notify_object_signal(object::Header* header);
} // namespace process
