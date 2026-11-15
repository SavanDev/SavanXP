#pragma once
#include "process.hpp"

namespace i386::io {
// Same bounds, handle entries and shared I/O objects as the reference kernel.
constexpr uint32_t kMaxHandles = 64;
constexpr int kBlocked = -0x70000000;
void initialize();
int install_object(process::Process& task, object::Header* header, uint32_t access, int fd = -1);
bool initialize_handles(process::Process& child, const process::Process* parent,
                        int stdin_fd = 0, int stdout_fd = 1);
void release_handles(process::Process& task);
void service();
int open(process::Process& task, const char* path, uint32_t flags);
int send_handle(process::Process& task, uint32_t pipe_fd, uint32_t fd);
int receive_handle(process::Process& task, uint32_t pipe_fd);
int close(process::Process& task, uint32_t fd);
int duplicate(process::Process& task, uint32_t oldfd, int newfd = -1);
int read(process::Process& task, uint32_t fd, uint32_t buffer, uint32_t count);
int write(process::Process& task, uint32_t fd, uint32_t buffer, uint32_t count);
int readdir(process::Process& task, uint32_t fd, uint32_t buffer, uint32_t capacity);
int seek(process::Process& task, uint32_t fd, int32_t offset, uint32_t whence);
int pipe(process::Process& task, uint32_t array);
int fcntl(process::Process& task, uint32_t fd, uint32_t command, uint32_t value);
int poll(process::Process& task, uint32_t array, uint32_t count, int32_t timeout);
int stat(process::Process& task, const char* path, uint32_t buffer);
int fstat(process::Process& task, uint32_t fd, uint32_t buffer);
int mutate(const char* path, uint32_t operation, uint32_t value = 0, const char* new_path = nullptr);
bool input_descriptor(process::Process& task, uint32_t fd, bool writable = false);
bool gpu_descriptor(process::Process& task, uint32_t fd);
void clear_console();
uint32_t handle_count(const process::Process& task);
}
