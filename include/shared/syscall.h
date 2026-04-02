#pragma once

#include <stdint.h>

enum savanxp_syscall_number {
    SAVANXP_SYS_READ = 0,
    SAVANXP_SYS_WRITE = 1,
    SAVANXP_SYS_OPEN = 2,
    SAVANXP_SYS_CLOSE = 3,
    SAVANXP_SYS_READDIR = 4,
    SAVANXP_SYS_SPAWN = 5,
    SAVANXP_SYS_WAITPID = 6,
    SAVANXP_SYS_EXIT = 7,
    SAVANXP_SYS_YIELD = 8,
    SAVANXP_SYS_UPTIME_MS = 9,
    SAVANXP_SYS_CLEAR = 10,
    SAVANXP_SYS_SLEEP_MS = 11,
    SAVANXP_SYS_PIPE = 12,
    SAVANXP_SYS_SPAWN_FD = 13,
    SAVANXP_SYS_DUP = 14,
    SAVANXP_SYS_DUP2 = 15,
    SAVANXP_SYS_PROC_INFO = 16,
    SAVANXP_SYS_SEEK = 17,
    SAVANXP_SYS_UNLINK = 18,
    SAVANXP_SYS_EXEC = 19,
    SAVANXP_SYS_MKDIR = 20,
    SAVANXP_SYS_RMDIR = 21,
    SAVANXP_SYS_TRUNCATE = 22,
    SAVANXP_SYS_RENAME = 23,
    SAVANXP_SYS_IOCTL = 24,
    SAVANXP_SYS_SOCKET = 25,
    SAVANXP_SYS_BIND = 26,
    SAVANXP_SYS_SENDTO = 27,
    SAVANXP_SYS_RECVFROM = 28,
    SAVANXP_SYS_CONNECT = 29,
    SAVANXP_SYS_GETPID = 30,
    SAVANXP_SYS_STAT = 31,
    SAVANXP_SYS_FSTAT = 32,
    SAVANXP_SYS_CHDIR = 33,
    SAVANXP_SYS_GETCWD = 34,
    SAVANXP_SYS_SYSTEM_INFO = 35,
    SAVANXP_SYS_SYNC = 36,
    SAVANXP_SYS_REALTIME = 37,
    SAVANXP_SYS_FCNTL = 38,
    SAVANXP_SYS_POLL = 39,
    SAVANXP_SYS_FORK = 40,
    SAVANXP_SYS_KILL = 41,
    SAVANXP_SYS_EVENT_CREATE = 42,
    SAVANXP_SYS_EVENT_SET = 43,
    SAVANXP_SYS_EVENT_RESET = 44,
    SAVANXP_SYS_WAIT_ONE = 45,
    SAVANXP_SYS_WAIT_MANY = 46,
    SAVANXP_SYS_TIMER_CREATE = 47,
    SAVANXP_SYS_TIMER_SET = 48,
    SAVANXP_SYS_TIMER_CANCEL = 49,
    SAVANXP_SYS_SECTION_CREATE = 50,
    SAVANXP_SYS_MAP_VIEW = 51,
    SAVANXP_SYS_UNMAP_VIEW = 52,
};

enum savanxp_open_flags {
    SAVANXP_OPEN_READ = 1u << 0,
    SAVANXP_OPEN_WRITE = 1u << 1,
    SAVANXP_OPEN_CREATE = 1u << 2,
    SAVANXP_OPEN_TRUNCATE = 1u << 3,
    SAVANXP_OPEN_APPEND = 1u << 4,
    SAVANXP_OPEN_NONBLOCK = 1u << 5,
};

enum savanxp_error_code {
    SAVANXP_EIO = 5,
    SAVANXP_EACCES = 13,
    SAVANXP_EAGAIN = 11,
    SAVANXP_EINVAL = 22,
    SAVANXP_EBADF = 9,
    SAVANXP_ENOENT = 2,
    SAVANXP_ENOMEM = 12,
    SAVANXP_EBUSY = 16,
    SAVANXP_EEXIST = 17,
    SAVANXP_ENODEV = 19,
    SAVANXP_EISDIR = 21,
    SAVANXP_ENOTDIR = 20,
    SAVANXP_ENOSPC = 28,
    SAVANXP_EPIPE = 32,
    SAVANXP_ENOTTY = 25,
    SAVANXP_ENOSYS = 38,
    SAVANXP_ENOTEMPTY = 39,
    SAVANXP_ECHILD = 10,
    SAVANXP_ETIMEDOUT = 110,
};

enum savanxp_mode_bits : uint32_t {
    SAVANXP_S_IFMT = 0170000u,
    SAVANXP_S_IFREG = 0100000u,
    SAVANXP_S_IFDIR = 0040000u,
    SAVANXP_S_IFCHR = 0020000u,
    SAVANXP_S_IFIFO = 0010000u,
    SAVANXP_S_IFSOCK = 0140000u,
};

struct savanxp_stat {
    uint32_t st_mode;
    uint32_t st_size;
};

enum savanxp_seek_whence {
    SAVANXP_SEEK_SET = 0,
    SAVANXP_SEEK_CUR = 1,
    SAVANXP_SEEK_END = 2,
};

enum savanxp_process_state {
    SAVANXP_PROC_UNUSED = 0,
    SAVANXP_PROC_READY = 1,
    SAVANXP_PROC_RUNNING = 2,
    SAVANXP_PROC_BLOCKED_READ = 3,
    SAVANXP_PROC_BLOCKED_WRITE = 4,
    SAVANXP_PROC_BLOCKED_WAIT = 5,
    SAVANXP_PROC_SLEEPING = 6,
    SAVANXP_PROC_ZOMBIE = 7,
};

struct savanxp_process_info {
    uint32_t pid;
    uint32_t parent_pid;
    int32_t exit_code;
    uint32_t state;
    char name[32];
};

enum savanxp_fcntl_command {
    SAVANXP_F_GETFL = 1,
    SAVANXP_F_SETFL = 2,
};

enum savanxp_poll_events {
    SAVANXP_POLLIN = 0x0001,
    SAVANXP_POLLOUT = 0x0004,
    SAVANXP_POLLERR = 0x0008,
    SAVANXP_POLLHUP = 0x0010,
    SAVANXP_POLLNVAL = 0x0020,
};

struct savanxp_pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};

enum savanxp_signal_number {
    SAVANXP_SIGINT = 2,
    SAVANXP_SIGKILL = 9,
    SAVANXP_SIGPIPE = 13,
    SAVANXP_SIGTERM = 15,
    SAVANXP_SIGCHLD = 17,
};

enum savanxp_event_flags {
    SAVANXP_EVENT_AUTO_RESET = 0,
    SAVANXP_EVENT_MANUAL_RESET = 1u << 0,
    SAVANXP_EVENT_INITIAL_SET = 1u << 1,
};

enum savanxp_wait_flags {
    SAVANXP_WAIT_FLAG_ANY = 0,
    SAVANXP_WAIT_FLAG_ALL = 1u << 0,
};

enum savanxp_timer_flags {
    SAVANXP_TIMER_AUTO_RESET = 0,
    SAVANXP_TIMER_MANUAL_RESET = 1u << 0,
};

enum savanxp_section_flags {
    SAVANXP_SECTION_READ = 1u << 0,
    SAVANXP_SECTION_WRITE = 1u << 1,
};

enum savanxp_view_flags {
    SAVANXP_VIEW_PRIVATE = 1u << 8,
};

enum savanxp_timer_backend {
    SAVANXP_TIMER_NONE = 0,
    SAVANXP_TIMER_LOCAL_APIC = 1,
};

struct savanxp_system_info {
    char bootloader_name[32];
    char bootloader_version[32];
    char firmware[16];
    uint8_t input_ready;
    uint8_t framebuffer_ready;
    uint8_t net_present;
    uint8_t speaker_ready;
    uint8_t block_ready;
    uint8_t svfs_mounted;
    uint16_t reserved0;
    uint32_t timer_backend;
    uint32_t timer_frequency_hz;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint32_t framebuffer_bpp;
    uint32_t pci_device_count;
    uint32_t svfs_file_count;
    uint64_t memory_usable_bytes;
    uint64_t memory_reclaimable_bytes;
    uint64_t memory_total_pages;
    uint64_t svfs_total_bytes;
    uint64_t svfs_used_bytes;
    uint64_t svfs_free_bytes;
    uint64_t initramfs_size;
    uint64_t uptime_ms;
};

struct savanxp_realtime {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t valid;
};

#define SAVANXP_IOCTL(group, number) ((((group) & 0xffffu) << 16) | ((number) & 0xffffu))

enum savanxp_ioctl_group {
    SAVANXP_IOCTL_GROUP_FB = 0x1001,
    SAVANXP_IOCTL_GROUP_NET = 0x1002,
    SAVANXP_IOCTL_GROUP_PCSPK = 0x1003,
    SAVANXP_IOCTL_GROUP_GPU = 0x1004,
    SAVANXP_IOCTL_GROUP_AUDIO = 0x1005,
};

enum savanxp_net_ioctl {
    NET_IOC_GET_INFO = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_NET, 1),
    NET_IOC_UP = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_NET, 2),
    NET_IOC_PING = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_NET, 3),
};

enum savanxp_net_status {
    SAVANXP_NET_STATUS_UNKNOWN = 0,
    SAVANXP_NET_STATUS_IDLE = 1,
    SAVANXP_NET_STATUS_READY = 2,
    SAVANXP_NET_STATUS_NO_DEVICE = 3,
    SAVANXP_NET_STATUS_BRING_UP_FAILED = 4,
    SAVANXP_NET_STATUS_TX_FAILED = 5,
    SAVANXP_NET_STATUS_TX_TIMEOUT = 6,
    SAVANXP_NET_STATUS_RX_INVALID = 7,
    SAVANXP_NET_STATUS_ARP_RESOLVING = 8,
    SAVANXP_NET_STATUS_ARP_RESOLVED = 9,
    SAVANXP_NET_STATUS_ARP_TIMEOUT = 10,
    SAVANXP_NET_STATUS_ICMP_SENT = 11,
    SAVANXP_NET_STATUS_ICMP_REPLY = 12,
    SAVANXP_NET_STATUS_ICMP_TIMEOUT = 13,
    SAVANXP_NET_STATUS_TCP_SYN_SENT = 14,
    SAVANXP_NET_STATUS_TCP_ESTABLISHED = 15,
    SAVANXP_NET_STATUS_TCP_FIN = 16,
    SAVANXP_NET_STATUS_TCP_TIMEOUT = 17,
};

enum savanxp_pcspk_ioctl {
    PCSPK_IOC_BEEP = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_PCSPK, 1),
    PCSPK_IOC_STOP = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_PCSPK, 2),
};

enum savanxp_gpu_ioctl {
    GPU_IOC_GET_INFO = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_GPU, 1),
    GPU_IOC_ACQUIRE = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_GPU, 2),
    GPU_IOC_RELEASE = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_GPU, 3),
    GPU_IOC_PRESENT = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_GPU, 4),
    GPU_IOC_PRESENT_REGION = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_GPU, 5),
    GPU_IOC_SET_MODE = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_GPU, 6),
    GPU_IOC_IMPORT_SECTION = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_GPU, 7),
    GPU_IOC_RELEASE_SURFACE = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_GPU, 8),
    GPU_IOC_PRESENT_SURFACE_REGION = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_GPU, 9),
    GPU_IOC_WAIT_IDLE = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_GPU, 10),
    GPU_IOC_GET_STATS = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_GPU, 11),
    GPU_IOC_GET_SCANOUTS = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_GPU, 12),
    GPU_IOC_REFRESH_SCANOUTS = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_GPU, 13),
    GPU_IOC_SET_CURSOR = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_GPU, 14),
    GPU_IOC_MOVE_CURSOR = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_GPU, 15),
    GPU_IOC_GET_PRESENT_TIMELINE = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_GPU, 16),
    GPU_IOC_WAIT_PRESENT = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_GPU, 17),
};

enum savanxp_audio_ioctl {
    AUDIO_IOC_GET_INFO = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_AUDIO, 1),
};

enum savanxp_socket_domain {
    SAVANXP_AF_INET = 1,
};

enum savanxp_socket_type {
    SAVANXP_SOCK_DGRAM = 1,
    SAVANXP_SOCK_STREAM = 2,
};

enum savanxp_socket_protocol {
    SAVANXP_IPPROTO_UDP = 17,
    SAVANXP_IPPROTO_TCP = 6,
};

struct savanxp_fb_info {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t buffer_size;
};

struct savanxp_fb_present_region {
    uint64_t pixels;
    uint32_t source_pitch;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};

enum savanxp_gpu_backend {
    SAVANXP_GPU_BACKEND_NONE = 0,
    SAVANXP_GPU_BACKEND_VIRTIO = 1,
};

enum savanxp_gpu_info_flags {
    SAVANXP_GPU_INFO_FLAG_IRQ_DRIVEN = 1u << 0,
    SAVANXP_GPU_INFO_FLAG_CURSOR_PLANE = 1u << 1,
    SAVANXP_GPU_INFO_FLAG_SCANOUT_ENUMERATION = 1u << 2,
    SAVANXP_GPU_INFO_FLAG_HOTPLUG_REFRESH = 1u << 3,
};

struct savanxp_gpu_info {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t buffer_size;
    uint32_t backend;
    uint32_t flags;
};

struct savanxp_gpu_present_region {
    uint64_t pixels;
    uint32_t source_pitch;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};

enum savanxp_gpu_surface_flags {
    SAVANXP_GPU_SURFACE_FLAG_NONE = 0,
    SAVANXP_GPU_SURFACE_FLAG_SCANOUT = 1u << 0,
};

struct savanxp_gpu_mode {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t buffer_size;
};

struct savanxp_gpu_surface_import {
    int32_t section_handle;
    int32_t surface_id;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t buffer_size;
    uint32_t flags;
};

struct savanxp_gpu_surface_present {
    uint32_t surface_id;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};

enum savanxp_gpu_scanout_flags {
    SAVANXP_GPU_SCANOUT_FLAG_ENABLED = 1u << 0,
    SAVANXP_GPU_SCANOUT_FLAG_ACTIVE = 1u << 1,
    SAVANXP_GPU_SCANOUT_FLAG_PRIMARY = 1u << 2,
    SAVANXP_GPU_SCANOUT_FLAG_PREFERRED = 1u << 3,
};

struct savanxp_gpu_scanout_info {
    uint32_t scanout_id;
    uint32_t flags;
    uint32_t native_width;
    uint32_t native_height;
    uint32_t preferred_width;
    uint32_t preferred_height;
    uint32_t active_width;
    uint32_t active_height;
};

struct savanxp_gpu_scanout_state {
    uint32_t count;
    uint32_t active_scanout_id;
    struct savanxp_gpu_scanout_info scanouts[16];
};

struct savanxp_gpu_cursor_image {
    uint64_t pixels;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t hotspot_x;
    uint32_t hotspot_y;
};

struct savanxp_gpu_cursor_position {
    uint32_t x;
    uint32_t y;
    uint32_t visible;
    uint32_t reserved0;
};

enum savanxp_gpu_present_timeline_flags {
    SAVANXP_GPU_PRESENT_TIMELINE_FLAG_DEGRADED = 1u << 0,
    SAVANXP_GPU_PRESENT_TIMELINE_FLAG_TARGET_FAILED = 1u << 1,
};

struct savanxp_gpu_present_timeline {
    uint64_t submitted_sequence;
    uint64_t retired_sequence;
    uint32_t pending_count;
    uint32_t flags;
};

struct savanxp_gpu_present_wait {
    uint64_t target_sequence;
    uint64_t retired_sequence;
    uint32_t pending_count;
    uint32_t flags;
};

struct savanxp_gpu_stats {
    uint64_t present_enqueued;
    uint64_t present_completed;
    uint64_t present_coalesced;
    uint64_t pending_depth_max;
    uint64_t transfer_stage_submitted;
    uint64_t transfer_stage_ticks;
    uint64_t flush_stage_submitted;
    uint64_t flush_stage_ticks;
    uint64_t scanout_stage_submitted;
    uint64_t scanout_stage_ticks;
    uint64_t sync_command_submitted;
    uint64_t sync_command_ticks;
    uint64_t wait_command_polls;
    uint64_t wait_command_ticks;
    uint64_t wait_surface_polls;
    uint64_t wait_surface_ticks;
    uint64_t wait_idle_polls;
    uint64_t wait_idle_ticks;
    uint64_t wait_pending_slot_polls;
    uint64_t wait_pending_slot_ticks;
    uint64_t command_timeouts;
    uint64_t surface_timeouts;
    uint64_t idle_timeouts;
    uint64_t pending_slot_timeouts;
    uint64_t resource_timeouts;
    uint64_t irq_notifications;
    uint64_t irq_config_events;
    uint64_t command_completions;
    uint64_t recovery_attempts;
    uint64_t recovery_successes;
    uint64_t degraded_entries;
    uint64_t scanout_refreshes;
    uint64_t cursor_updates;
    uint64_t cursor_moves;
    uint64_t wait_present_polls;
    uint64_t wait_present_ticks;
    uint64_t present_wait_timeouts;
    uint64_t present_end_to_end_ticks;
    uint64_t present_end_to_end_samples;
    uint64_t present_end_to_end_max_ticks;
};

#define SAVANXP_GPU_CLIENT_SURFACE_MAGIC 0x53584746u

struct savanxp_gpu_client_surface_header {
    uint32_t magic;
    uint32_t pixels_offset;
    struct savanxp_fb_info info;
};

struct savanxp_gpu_client_present_packet {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};

enum savanxp_audio_backend {
    SAVANXP_AUDIO_BACKEND_NONE = 0,
    SAVANXP_AUDIO_BACKEND_VIRTIO = 1,
};

struct savanxp_audio_info {
    uint32_t sample_rate_hz;
    uint32_t channels;
    uint32_t bits_per_sample;
    uint32_t frame_bytes;
    uint32_t period_bytes;
    uint32_t buffer_bytes;
    uint32_t backend;
    uint32_t flags;
};

enum savanxp_input_event_type {
    SAVANXP_INPUT_EVENT_KEY_DOWN = 1,
    SAVANXP_INPUT_EVENT_KEY_UP = 2,
};

enum savanxp_mouse_button {
    SAVANXP_MOUSE_BUTTON_LEFT = 1u << 0,
    SAVANXP_MOUSE_BUTTON_RIGHT = 1u << 1,
    SAVANXP_MOUSE_BUTTON_MIDDLE = 1u << 2,
};

enum savanxp_key_code {
    SAVANXP_KEY_NONE = 0,
    SAVANXP_KEY_BACKSPACE = 8,
    SAVANXP_KEY_TAB = 9,
    SAVANXP_KEY_ENTER = 13,
    SAVANXP_KEY_ESC = 27,
    SAVANXP_KEY_UP = 256,
    SAVANXP_KEY_DOWN = 257,
    SAVANXP_KEY_LEFT = 258,
    SAVANXP_KEY_RIGHT = 259,
    SAVANXP_KEY_SHIFT = 260,
    SAVANXP_KEY_CTRL = 261,
    SAVANXP_KEY_ALT = 262,
    SAVANXP_KEY_CAPSLOCK = 263,
    SAVANXP_KEY_HOME = 264,
    SAVANXP_KEY_END = 265,
    SAVANXP_KEY_PAGE_UP = 266,
    SAVANXP_KEY_PAGE_DOWN = 267,
    SAVANXP_KEY_INSERT = 268,
    SAVANXP_KEY_DELETE = 269,
    SAVANXP_KEY_F1 = 270,
    SAVANXP_KEY_F2 = 271,
    SAVANXP_KEY_F3 = 272,
    SAVANXP_KEY_F4 = 273,
    SAVANXP_KEY_F5 = 274,
    SAVANXP_KEY_F6 = 275,
    SAVANXP_KEY_F7 = 276,
    SAVANXP_KEY_F8 = 277,
    SAVANXP_KEY_F9 = 278,
    SAVANXP_KEY_F10 = 279,
    SAVANXP_KEY_F11 = 280,
    SAVANXP_KEY_F12 = 281,
    SAVANXP_KEY_PRINT_SCREEN = 282,
    SAVANXP_KEY_PAUSE = 283,
    SAVANXP_KEY_NUMLOCK = 284,
    SAVANXP_KEY_SCROLLLOCK = 285,
    SAVANXP_KEY_SUPER = 286,
    SAVANXP_KEY_MENU = 287,
    SAVANXP_KEY_ALT_GR = 288,
};

struct savanxp_input_event {
    uint32_t type;
    uint32_t key;
    int32_t ascii;
};

struct savanxp_mouse_event {
    int32_t delta_x;
    int32_t delta_y;
    uint32_t buttons;
};

struct savanxp_net_info {
    uint8_t present;
    uint8_t up;
    uint8_t link;
    uint8_t reserved0;
    uint8_t mac[6];
    uint16_t reserved1;
    uint32_t ipv4;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t last_status;
    uint32_t tx_frames;
    uint32_t rx_frames;
    uint32_t tx_errors;
    uint32_t rx_errors;
    uint32_t arp_requests;
    uint32_t arp_timeouts;
    uint32_t ping_requests;
    uint32_t ping_timeouts;
};

struct savanxp_net_ping_result {
    uint32_t ipv4;
    uint32_t reply_ipv4;
    uint32_t elapsed_ms;
    uint8_t ttl;
    uint8_t reserved[3];
};

struct savanxp_net_ping_request {
    uint32_t ipv4;
    uint32_t timeout_ms;
    uint16_t sequence;
    uint16_t payload_size;
    uint64_t result_ptr;
};

struct savanxp_pcspk_beep {
    uint32_t frequency_hz;
    uint32_t duration_ms;
};

struct savanxp_sockaddr_in {
    uint32_t ipv4;
    uint16_t port;
    uint16_t reserved0;
};
