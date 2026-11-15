#pragma once
#include "process.hpp"

namespace i386::sections {
int create(process::Process& task, uint32_t size, uint32_t flags);
int open(process::Process& task, uint32_t fd, uint32_t offset, uint32_t length, uint32_t flags);
long map(process::Process& task, uint32_t fd, uint32_t base, uint32_t flags);
int unmap(process::Process& task, uint32_t base);
bool clone_views(const process::Process& source, process::Process& target);
void release_views(process::Process& task);
uint32_t last_failure();
}
