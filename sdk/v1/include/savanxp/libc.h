#pragma once

#include <stddef.h>
#include <stdint.h>

#include "savanxp/syscall.h"

long read(int fd, void* buffer, size_t count);
long write(int fd, const void* buffer, size_t count);
long open(const char* path);
long open_mode(const char* path, unsigned long flags);
long close(int fd);
long readdir(int fd, char* buffer, size_t count);
long spawn(const char* path, const char* const* argv, int argc);
long spawn_fd(const char* path, const char* const* argv, int argc, int stdin_fd, int stdout_fd);
long spawn_fds(const char* path, const char* const* argv, int argc, int stdin_fd, int stdout_fd, int stderr_fd);
long exec(const char* path, const char* const* argv, int argc);
long pipe(int fds[2]);
long dup(int fd);
long dup2(int oldfd, int newfd);
long seek(int fd, long offset, int whence);
long unlink(const char* path);
long mkdir(const char* path);
long rmdir(const char* path);
long truncate(const char* path, unsigned long size);
long rename(const char* old_path, const char* new_path);
long fcntl(int fd, unsigned long command, unsigned long value);
long ioctl(int fd, unsigned long request, unsigned long arg);
long poll(struct savanxp_pollfd* fds, unsigned long count, long timeout_ms);
long socket(unsigned long domain, unsigned long type, unsigned long protocol);
long bind(int fd, const struct savanxp_sockaddr_in* address);
long sendto(int fd, const void* buffer, size_t count, const struct savanxp_sockaddr_in* address);
long recvfrom(int fd, void* buffer, size_t count, struct savanxp_sockaddr_in* address, unsigned long timeout_ms);
long connect(int fd, const struct savanxp_sockaddr_in* address, unsigned long timeout_ms);
long waitpid(int pid, int* status);
long fork(void);
long kill(int pid, int signal_number);
long event_create(unsigned long flags);
long event_set(int handle);
long event_reset(int handle);
long wait_one(int handle, long timeout_ms);
long wait_many(const int* handles, unsigned long count, unsigned long flags, long timeout_ms);
long timer_create(unsigned long flags);
long timer_set(int handle, unsigned long due_ms, unsigned long period_ms);
long timer_cancel(int handle);
long yield(void);
long sleep_ms(unsigned long milliseconds);
unsigned long uptime_ms(void);
long clear_screen(void);
long proc_info(unsigned long index, struct savanxp_process_info* info);
long system_info(struct savanxp_system_info* info);
long realtime(struct savanxp_realtime* value);
long sync(void);
long mouse_open(void);
int mouse_poll_event(int fd, struct savanxp_mouse_event* event);
long audio_open(void);
long audio_get_info(int fd, struct savanxp_audio_info* info);
long gpu_open(void);
long gpu_get_info(int fd, struct savanxp_gpu_info* info);
long gpu_acquire(int fd);
long gpu_release(int fd);
long gpu_present(int fd, const uint32_t* pixels);
long gpu_present_region(int fd, const uint32_t* pixels, uint32_t source_pitch, uint32_t x, uint32_t y, uint32_t width, uint32_t height);
long savanxp_getpid(void);
long savanxp_stat(const char* path, struct savanxp_stat* info);
long savanxp_fstat(int fd, struct savanxp_stat* info);
long savanxp_chdir(const char* path);
long savanxp_getcwd(char* buffer, size_t count);
void exit(int code) __attribute__((noreturn));

struct savanxp_gfx_context {
    int fb_fd;
    int input_fd;
    struct savanxp_fb_info info;
};

long gfx_open(struct savanxp_gfx_context* context);
long gfx_close(struct savanxp_gfx_context* context);
long gfx_acquire(struct savanxp_gfx_context* context);
long gfx_release(struct savanxp_gfx_context* context);
long gfx_present(const struct savanxp_gfx_context* context, const uint32_t* pixels);
long gfx_present_region(const struct savanxp_gfx_context* context, const uint32_t* pixels, uint32_t x, uint32_t y, uint32_t width, uint32_t height);
int gfx_poll_event(const struct savanxp_gfx_context* context, struct savanxp_input_event* event);
uint32_t gfx_rgb(uint8_t red, uint8_t green, uint8_t blue);
uint32_t gfx_stride_pixels(const struct savanxp_fb_info* info);
size_t gfx_buffer_pixels(const struct savanxp_fb_info* info);
size_t gfx_buffer_bytes(const struct savanxp_fb_info* info);
void gfx_clear(uint32_t* pixels, const struct savanxp_fb_info* info, uint32_t colour);
void gfx_pixel(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, uint32_t colour);
void gfx_hline(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, int width, uint32_t colour);
void gfx_vline(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, int height, uint32_t colour);
void gfx_rect(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, int width, int height, uint32_t colour);
void gfx_frame(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, int width, int height, uint32_t colour);
int gfx_text_width(const char* text);
int gfx_text_height(void);
void gfx_blit_text(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, const char* text, uint32_t colour);

int result_is_error(long result);
int result_error_code(long result);
const char* error_string(int error_code);
const char* result_error_string(long result);
const char* process_state_string(unsigned long state);
const char* net_status_string(unsigned long status);

size_t strlen(const char* text);
int strcmp(const char* left, const char* right);
int strncmp(const char* left, const char* right, size_t count);
char* strcpy(char* destination, const char* source);
void* memcpy(void* destination, const void* source, size_t count);
void* memset(void* destination, int value, size_t count);

void putchar(int fd, char character);
void puts_fd(int fd, const char* text);
void puts_err(const char* text);
void puts(const char* text);
void printf_fd(int fd, const char* format, ...);
void eprintf(const char* format, ...);
void printf(const char* format, ...);
