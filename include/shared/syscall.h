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
};

enum savanxp_open_flags {
    SAVANXP_OPEN_READ = 1u << 0,
    SAVANXP_OPEN_WRITE = 1u << 1,
    SAVANXP_OPEN_CREATE = 1u << 2,
    SAVANXP_OPEN_TRUNCATE = 1u << 3,
    SAVANXP_OPEN_APPEND = 1u << 4,
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

#define SAVANXP_IOCTL(group, number) ((((group) & 0xffffu) << 16) | ((number) & 0xffffu))

enum savanxp_ioctl_group {
    SAVANXP_IOCTL_GROUP_FB = 0x1001,
    SAVANXP_IOCTL_GROUP_NET = 0x1002,
    SAVANXP_IOCTL_GROUP_PCSPK = 0x1003,
};

enum savanxp_fb_ioctl {
    FB_IOC_GET_INFO = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_FB, 1),
    FB_IOC_ACQUIRE = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_FB, 2),
    FB_IOC_RELEASE = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_FB, 3),
    FB_IOC_PRESENT = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_FB, 4),
    FB_IOC_PRESENT_REGION = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_FB, 5),
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

enum savanxp_input_event_type {
    SAVANXP_INPUT_EVENT_KEY_DOWN = 1,
    SAVANXP_INPUT_EVENT_KEY_UP = 2,
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
