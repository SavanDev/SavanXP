#pragma once

/*
 * Neutral wire contract between the kernel and every userland layer.
 *
 * Everything here crosses a boundary: it is data the kernel produces or
 * consumes on behalf of a process, never behaviour. The POSIX surface
 * (syscall numbers, open flags, mode bits, seek, fcntl, poll events,
 * signals, sockets) lives in the SDK's savanxp/syscall.h, which includes
 * this file. A kernel source must never include that one: the kernel
 * speaks this contract and is indifferent to which userland, if any, is
 * built on it.
 *
 * See docs/kernel/KERNEL_NATIVE_API.md for the isolation campaign.
 */

#pragma once

#include <stdint.h>

#define SAVANXP_SDK_VERSION 1
#define SAVANXP_SDK_VERSION_MAJOR 1
#define SAVANXP_SDK_VERSION_MINOR 2

#define SAVANXP_STDIN_FILENO 0
#define SAVANXP_STDOUT_FILENO 1
#define SAVANXP_STDERR_FILENO 2

#define SAVANXP_WAIT_ANY (-1)
#define SAVANXP_MAX_ARGC 15
#define SAVANXP_MAX_ARG_LENGTH 63
#define SAVANXP_PROC_NAME_CAPACITY 32



/*
 * The kernel's own status space (isolation slice E).
 *
 * The kernel returns one of these and nothing else. The values are identical
 * to errno because changing them would change every userland-visible error for
 * no gain, and a migration nobody can observe is not a migration -- the point
 * here is ownership and direction, not arithmetic.
 *
 * The POSIX names below are what the SDK sees. POSIX is downstream: remove the
 * userland and these numbers are still the kernel's, because the kernel no
 * longer names them `savanxp_*` or calls them errno. A userland that wants a
 * different error space maps these in its own translation (docs/
 * KERNEL_NATIVE_API.md, slice E).
 */
enum sx_status {
    SX_EIO = 5,
    SX_ENOEXEC = 8,
    SX_EACCES = 13,
    SX_EAGAIN = 11,
    SX_EINVAL = 22,
    SX_EBADF = 9,
    SX_ENOENT = 2,
    SX_ENOMEM = 12,
    SX_EBUSY = 16,
    SX_EEXIST = 17,
    SX_ENODEV = 19,
    SX_ENOTDIR = 20,
    SX_EISDIR = 21,
    SX_ENOSPC = 28,
    SX_EPIPE = 32,
    SX_ENOTTY = 25,
    SX_ENOSYS = 38,
    SX_ENOTEMPTY = 39,
    SX_ECHILD = 10,
    SX_E2BIG = 7,
    SX_ECONNRESET = 104,
    SX_ETIMEDOUT = 110,
};

// El nombre de POSIX para los mismos numeros, para userland. El kernel nunca
// escribe esto.
typedef enum sx_status savanxp_error_code;

#define SAVANXP_EIO SX_EIO
#define SAVANXP_ENOEXEC SX_ENOEXEC
#define SAVANXP_EACCES SX_EACCES
#define SAVANXP_EAGAIN SX_EAGAIN
#define SAVANXP_EINVAL SX_EINVAL
#define SAVANXP_EBADF SX_EBADF
#define SAVANXP_ENOENT SX_ENOENT
#define SAVANXP_ENOMEM SX_ENOMEM
#define SAVANXP_EBUSY SX_EBUSY
#define SAVANXP_EEXIST SX_EEXIST
#define SAVANXP_ENODEV SX_ENODEV
#define SAVANXP_ENOTDIR SX_ENOTDIR
#define SAVANXP_EISDIR SX_EISDIR
#define SAVANXP_ENOSPC SX_ENOSPC
#define SAVANXP_EPIPE SX_EPIPE
#define SAVANXP_ENOTTY SX_ENOTTY
#define SAVANXP_ENOSYS SX_ENOSYS
#define SAVANXP_ENOTEMPTY SX_ENOTEMPTY
#define SAVANXP_ECHILD SX_ECHILD
#define SAVANXP_E2BIG SX_E2BIG
#define SAVANXP_ECONNRESET SX_ECONNRESET
#define SAVANXP_ETIMEDOUT SX_ETIMEDOUT


struct savanxp_stat {
    uint32_t st_mode;
    uint32_t st_size;
};



/* El proceso ocioso. Existe siempre, corre cuando no hay nada listo y es el
 * que vuelve calculable el uso de CPU del sistema: lo que el idle NO se llevo
 * es lo que se llevaron los demas. */
#define SAVANXP_PROC_FLAG_IDLE (1u << 0)

struct savanxp_process_info {
    uint32_t pid;
    uint32_t parent_pid;
    int32_t exit_code;
    uint32_t state;
    char name[32];

    /* Contadores vivos. `cpu_ticks` son ticks del timer consumidos por este
     * proceso desde que arranco: se lee por diferencia contra
     * savanxp_system_info.cpu_ticks_total, nunca solo. `memory_bytes` son las
     * paginas de usuario mapeadas en este instante. */
    uint64_t cpu_ticks;
    uint64_t memory_bytes;
    uint32_t handle_count;
    uint32_t flags;          /* SAVANXP_PROC_FLAG_* */
};



struct savanxp_pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
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
    /* Pide que las paginas de la vista se puedan ejecutar. Sin este bit salen
     * con NX, como antes. Combinado con WRITE se rechaza: el kernel no hace
     * jamas una vista que sea escribible y ejecutable a la vez. */
    SAVANXP_SECTION_EXEC = 1u << 2,
};

enum savanxp_view_flags {
    SAVANXP_VIEW_PRIVATE = 1u << 8,
};

enum savanxp_timer_backend {
    SAVANXP_TIMER_NONE = 0,
    SAVANXP_TIMER_LOCAL_APIC = 1,
    SAVANXP_TIMER_PIT = 2,
};

/* Informe de consistencia del volumen SxFS, el equivalente dentro del sistema de
 * lo que `sxfs-cli check` hace en el host. Es de SOLO LECTURA: no repara nada y
 * no puede empeorar un volumen ya roto, asi que se puede correr con `/disk`
 * montado. Devuelve 0 si el informe se lleno, o un error negativo.
 *
 * Todos los contadores `*_blocks` y `*_inodes` estan en cero en un volumen sano.
 * Los que no son cero describen el defecto:
 *
 *   leaked_blocks       sector ocupado en el bitmap que ningun inodo reclama
 *   lost_blocks         sector reclamado por un inodo y libre en el bitmap
 *   double_claimed      dos inodos sobre el mismo sector
 *   metadata_unmarked   sector de [0, data_lba) que el bitmap da por libre
 *   orphan_inodes       inodo asignado que ningun directorio alcanza
 *   alias_inodes        dos entradas de directorio apuntan al mismo inodo
 *   duplicate_names     un nombre repetido dentro de un mismo directorio
 *   bad_dir_entries     entrada de directorio que no valida
 *   unreadable_dirs     directorio ilegible o mas profundo que el limite
 *
 * `journal_pending` distinto de cero significa que una transaccion quedo a
 * medias; el montaje la haya reintentado, asi que finding informed.
 */
struct savanxp_fscheck_report {
    uint32_t total_sectors;
    uint32_t data_lba;
    uint32_t sequence;
    uint32_t journal_pending;
    uint8_t clean_shutdown;
    uint8_t journal_valid;
    uint16_t reserved0;
    uint32_t inodes_allocated;
    uint32_t files;
    uint32_t directories;
    uint32_t data_sectors;
    uint32_t data_used_sectors;
    uint32_t data_claimed_sectors;
    uint32_t leaked_blocks;
    uint32_t lost_blocks;
    uint32_t double_claimed;
    uint32_t metadata_unmarked;
    uint32_t orphan_inodes;
    uint32_t alias_inodes;
    uint32_t duplicate_names;
    uint32_t bad_dir_entries;
    uint32_t unreadable_dirs;

    /* Costo de metadata desde el arranque. El journal copia toda la metadata en
     * cada commit y la reescribe, asi que `metadata_bytes_written` crece con el
     * tamano de la metadata y no con el del volumen: es el numero que decide si
     * conviene un journal de delta. */
    uint32_t metadata_commits;
    uint32_t reserved1;
    uint64_t metadata_bytes_written;
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
    uint8_t sxfs_mounted;
    uint16_t reserved0;
    uint32_t timer_backend;
    uint32_t timer_frequency_hz;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint32_t framebuffer_bpp;
    uint32_t pci_device_count;
    uint32_t sxfs_file_count;
    /* Secciones vivas en la tabla global, y cuantas de ellas estan respaldadas
     * por un archivo. Las respaldadas se comparten entre procesos por inode, asi
     * que dos procesos con la misma libreria deberian sumar 1 entre los dos. */
    uint32_t sections_live;
    uint32_t file_sections_live;
    /* Motivo del ultimo fallo de map_view/map_view_at. */
    uint32_t last_view_failure;
    uint64_t memory_usable_bytes;
    uint64_t memory_reclaimable_bytes;
    uint64_t memory_total_pages;
    uint64_t sxfs_total_bytes;
    uint64_t sxfs_used_bytes;
    uint64_t sxfs_free_bytes;
    uint64_t initramfs_size;
    uint64_t uptime_ms;

    /* ---- estado vivo y identidad del procesador --------------------------
     *
     * Todo lo de arriba se responde con lo que el boot dejo anotado. Esto se
     * mide en el momento de preguntar, y es lo que hace posible un
     * administrador de tareas: la memoria que queda libre AHORA y el contador
     * de ticks contra el que se comparan los `cpu_ticks` de cada proceso.
     *
     * `cpu_ticks_total` y `savanxp_process_info.cpu_ticks` solo significan algo
     * juntos y del MISMO par de muestras: el uso de CPU es el cociente entre
     * los dos incrementos, no un valor que se pueda leer de una sola lectura.
     */
    uint64_t memory_free_bytes;
    uint64_t cpu_ticks_total;
    uint32_t process_count;
    uint32_t cpu_count;      /* cores que reporto el bootloader */
    uint32_t cpu_online;     /* cores que corren procesos (uno ocioso por cada uno) */
    uint32_t cpu_khz;        /* reloj del procesador; 0 si no se calibro */
    uint32_t cpu_features;   /* enum savanxp_cpu_feature */

    /* Hardware de red que EXISTE en el bus, que no es lo mismo que
     * `net_present`: ese dice que un driver reclamo el adaptador, y este que
     * hay un dispositivo PCI de clase red enchufado. Los dos juntos son los que
     * distinguen "esta maquina no tiene placa de red" de "tiene una que este
     * sistema todavia no sabe manejar", que es lo que pasa en VirtualBox, donde
     * la placa por default es una Intel PRO/1000. Decirle "sin adaptador" a la
     * segunda es mentir sobre la maquina. */
    uint8_t net_hardware;
    uint8_t reserved1;
    uint16_t net_hardware_vendor;
    uint16_t net_hardware_device;
    uint16_t reserved2;
    char cpu_vendor[16];     /* "GenuineIntel", "AuthenticAMD", ... */
    char cpu_brand[52];      /* marca declarada por el CPU; vacio si no la da */
};

enum savanxp_cpu_feature {
    SAVANXP_CPU_FEATURE_SSE2 = 1u << 0,
    SAVANXP_CPU_FEATURE_PAE = 1u << 1,
    SAVANXP_CPU_FEATURE_NX = 1u << 2,
    SAVANXP_CPU_FEATURE_LONG_MODE = 1u << 3,
    SAVANXP_CPU_FEATURE_X2APIC = 1u << 4,
    SAVANXP_CPU_FEATURE_HYPERVISOR = 1u << 5,
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
    SAVANXP_IOCTL_GROUP_POWER = 0x1006,
    SAVANXP_IOCTL_GROUP_CLIPBOARD = 0x1007,
    SAVANXP_IOCTL_GROUP_INPUT = 0x1008,
};

enum savanxp_keyboard_layout {
    SAVANXP_KEYBOARD_LAYOUT_ES = 0,
    SAVANXP_KEYBOARD_LAYOUT_EN = 1,
};

/* SET_LAYOUT manda el valor de savanxp_keyboard_layout directo como argument
 * (no como puntero: es un escalar de un bit de informacion). GET_LAYOUT
 * devuelve el layout activo como el propio valor de retorno no-negativo del
 * ioctl, mismo convenio que usan los fds (negativo = -errno). */
enum savanxp_input_ioctl {
    INPUT_IOC_SET_LAYOUT = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_INPUT, 1),
    INPUT_IOC_GET_LAYOUT = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_INPUT, 2),
};

enum savanxp_net_ioctl {
    NET_IOC_GET_INFO = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_NET, 1),
    NET_IOC_UP = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_NET, 2),
    NET_IOC_PING = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_NET, 3),
    NET_IOC_GET_TCP_STATS = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_NET, 4),
    NET_IOC_SET_TCP_FAULT = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_NET, 5),
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
    SAVANXP_NET_STATUS_TCP_RESET = 18,
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
    GPU_IOC_GET_CONNECTOR_PROPERTIES = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_GPU, 18),
    GPU_IOC_CREATE_PRESENT_EVENT = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_GPU, 19),
    GPU_IOC_CREATE_SCANOUT_EVENT = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_GPU, 20),
    GPU_IOC_PRESENT_SURFACE_BATCH = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_GPU, 21),
};

enum savanxp_audio_ioctl {
    AUDIO_IOC_GET_INFO = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_AUDIO, 1),
    /* Volumen maestro de reproduccion, 0..100. SET lleva el valor directo
     * como argument (no como puntero: es un escalar, igual que
     * INPUT_IOC_SET_LAYOUT) y exige acceso de escritura; GET lo devuelve
     * como el propio valor de retorno no-negativo del ioctl. Se aplica en
     * el kernel a todo lo que llegue a /dev/audio0, escriba quien escriba,
     * asi que un control en la barra y un juego hablan del mismo numero.
     * 100 es identidad: el periodo viaja sin tocarse. */
    AUDIO_IOC_SET_VOLUME = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_AUDIO, 2),
    AUDIO_IOC_GET_VOLUME = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_AUDIO, 3),
    /* Silencio maestro, 0 (suena) o 1 (mudo). Mismo convenio escalar que el
     * volumen. Mudo equivale a ganancia cero pero conserva el volumen para
     * al quitarlo volver al nivel anterior. */
    AUDIO_IOC_SET_MUTED = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_AUDIO, 4),
    AUDIO_IOC_GET_MUTED = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_AUDIO, 5),
};

enum savanxp_power_ioctl {
    POWER_IOC_SHUTDOWN = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_POWER, 1),
    POWER_IOC_REBOOT = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_POWER, 2),
};

/* Portapapeles del sistema, expuesto como `/dev/clipboard`.
 *
 * Vive en el kernel y no en el window manager por una razon concreta: cada
 * canal del WM se paga con descriptores POR CLIENTE contra un limite de 64 por
 * proceso (process::kMaxFileHandles), asi que un par de canales mas por cliente
 * le bajaria el techo de ventanas simultaneas. Como device node el cliente lo
 * abre cuando copia o pega y lo cierra enseguida, no cuesta un fd permanente,
 * y ademas lo pueden usar procesos sin ventana.
 *
 * Semantica de VALOR, no de stream: cada `write` reemplaza el contenido
 * entero, y cada `read` lo devuelve desde el principio, sin cursor -- leer dos
 * veces da lo mismo. Un `write` mas grande que la capacidad se rechaza con
 * ENOSPC en vez de truncarse: pegar medio texto en silencio es peor que no
 * pegar nada. */
#define SAVANXP_CLIPBOARD_CAPACITY 8192u

enum savanxp_clipboard_format {
    SAVANXP_CLIPBOARD_FORMAT_EMPTY = 0,
    SAVANXP_CLIPBOARD_FORMAT_TEXT = 1,
};

enum savanxp_clipboard_ioctl {
    CLIP_IOC_GET_INFO = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_CLIPBOARD, 1),
    CLIP_IOC_CLEAR = SAVANXP_IOCTL(SAVANXP_IOCTL_GROUP_CLIPBOARD, 2),
};

struct savanxp_clipboard_info {
    uint32_t length;
    uint32_t capacity;
    uint32_t format; /* enum savanxp_clipboard_format */
    uint32_t reserved0;
    /* Arranca en 0 ("nunca se escribio") y sube con cada cambio, incluido el
     * CLIP_IOC_CLEAR. Un cliente que la cachea sabe si el portapapeles cambio
     * sin releer el contenido -- por ejemplo para habilitar o deshabilitar
     * Pegar en un menu sin copiar 8 KiB en cada repintado. */
    uint64_t sequence;
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
    SAVANXP_GPU_BACKEND_FRAMEBUFFER = 2,
};

enum savanxp_gpu_info_flags {
    SAVANXP_GPU_INFO_FLAG_IRQ_DRIVEN = 1u << 0,
    SAVANXP_GPU_INFO_FLAG_CURSOR_PLANE = 1u << 1,
    SAVANXP_GPU_INFO_FLAG_SCANOUT_ENUMERATION = 1u << 2,
    SAVANXP_GPU_INFO_FLAG_HOTPLUG_REFRESH = 1u << 3,
    SAVANXP_GPU_INFO_FLAG_ASYNC_PRESENT_EVENTS = 1u << 4,
};

enum savanxp_gpu_surface_format {
    SAVANXP_GPU_SURFACE_FORMAT_INVALID = 0,
    SAVANXP_GPU_SURFACE_FORMAT_BGRX8888 = 1,
    SAVANXP_GPU_SURFACE_FORMAT_BGRA8888 = 2,
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
    /* Page-aligned byte offset of the pixel region within the section. 0 means
     * the section is pixels-only (the display surface). Non-zero lets a client
     * surface whose pixels follow a header be imported as a scanout resource. */
    uint32_t pixels_offset;
    uint32_t reserved0;
};

struct savanxp_gpu_surface_present {
    uint32_t surface_id;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};

enum savanxp_gpu_connector_flags {
    SAVANXP_GPU_CONNECTOR_FLAG_MUTABLE_MODE_SETTING = 1u << 0,
    SAVANXP_GPU_CONNECTOR_FLAG_PARTIAL_PRESENT = 1u << 1,
    SAVANXP_GPU_CONNECTOR_FLAG_CURSOR_PLANE = 1u << 2,
    SAVANXP_GPU_CONNECTOR_FLAG_SCANOUT_ENUMERATION = 1u << 3,
    SAVANXP_GPU_CONNECTOR_FLAG_HOTPLUG_REFRESH = 1u << 4,
    SAVANXP_GPU_CONNECTOR_FLAG_ASYNC_PRESENT_EVENTS = 1u << 5,
    SAVANXP_GPU_CONNECTOR_FLAG_SAFE_MODE = 1u << 6,
};

struct savanxp_gpu_connector_properties {
    uint32_t flags;
    uint32_t active_scanout_id;
    uint32_t preferred_width;
    uint32_t preferred_height;
    uint32_t batch_capacity;
    uint32_t max_dirty_rects;
};

struct savanxp_gpu_dirty_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};

enum savanxp_gpu_surface_present_batch_flags {
    SAVANXP_GPU_SURFACE_PRESENT_BATCH_FLAG_NONE = 0,
    SAVANXP_GPU_SURFACE_PRESENT_BATCH_FLAG_FULL_SURFACE = 1u << 0,
};

#define SAVANXP_GPU_SURFACE_PRESENT_BATCH_MAX_RECTS 32u

struct savanxp_gpu_surface_present_batch {
    uint32_t surface_id;
    uint32_t rect_count;
    uint32_t flags;
    uint32_t reserved0;
    uint64_t present_cookie;
    struct savanxp_gpu_dirty_rect rects[SAVANXP_GPU_SURFACE_PRESENT_BATCH_MAX_RECTS];
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

enum savanxp_cursor_shape {
    SAVANXP_CURSOR_ARROW = 0,
    SAVANXP_CURSOR_WAIT,
    SAVANXP_CURSOR_TEXT,
    SAVANXP_CURSOR_MOVE,
    SAVANXP_CURSOR_RESIZE_H,
    SAVANXP_CURSOR_RESIZE_V,
    SAVANXP_CURSOR_UNAVAILABLE,
    SAVANXP_CURSOR_LINK,
    SAVANXP_CURSOR_SHAPE_COUNT,
};

struct savanxp_gpu_cursor_position {
    uint32_t x;
    uint32_t y;
    uint32_t visible;
    uint32_t shape; /* enum savanxp_cursor_shape */
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
#define SAVANXP_GPU_CLIENT_SURFACE_VERSION_1 1u
#define SAVANXP_GPU_CLIENT_SURFACE_VERSION_2 2u
#define SAVANXP_GPU_CLIENT_SURFACE_VERSION_3 3u
/* v4: los pedidos del cliente al WM (cursor, size hint, launch) y el pedido de
 * cierre viajan en el header en vez de por descriptores propios. Ver
 * savanxp/wm_protocol.h. */
#define SAVANXP_GPU_CLIENT_SURFACE_VERSION_4 4u
/* v5: the launch queue left the header -- a client starts programs itself
 * (protocol v5, savanxp/wm_protocol.h) -- so the requests that follow it moved. */
#define SAVANXP_GPU_CLIENT_SURFACE_VERSION_5 5u
#define SAVANXP_GPU_CLIENT_BATCH_CAPACITY 8u
#define SAVANXP_GPU_CLIENT_BATCH_MAX_RECTS 32u

struct savanxp_gpu_dirty_rect_batch {
    uint64_t submit_sequence;
    uint32_t rect_count;
    uint32_t flags;
    struct savanxp_gpu_dirty_rect rects[SAVANXP_GPU_CLIENT_BATCH_MAX_RECTS];
};

enum savanxp_audio_backend {
    SAVANXP_AUDIO_BACKEND_NONE = 0,
    SAVANXP_AUDIO_BACKEND_VIRTIO = 1,
    SAVANXP_AUDIO_BACKEND_AC97 = 2,
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
    SAVANXP_INPUT_EVENT_RESIZED = 3,
};

#define SAVANXP_DESKTOP_LAUNCH_PATH_CAPACITY 192u

/* Launch flags: what a program declares about itself in its .sxe
 * (SXE_TAG_LAUNCH_FLAGS), sent to the WM in the hello of protocol v5
 * (savanxp_wm_hello), so the WM does not have to know a catalog of
 * applications. They describe the program, never a piece of the shell: the
 * shell's own windows are window roles. */
#define SAVANXP_DESKTOP_LAUNCH_FLAG_NONE 0x00000000u
/* The program renders at a low resolution and uses composited fullscreen
 * (F11): the WM sizes its surface for that when it connects. */
#define SAVANXP_DESKTOP_LAUNCH_FLAG_FULLSCREEN 0x00000001u

/* Estilo de ventana: propiedades de la VENTANA de un programa, no de su
 * lanzamiento. El WM las lee del .sxe del binario al crear la ventana, por el
 * mismo camino que el titulo y el accent (SXE_TAG_WINDOW_FLAGS en
 * include/sxe/sxe_format.h), asi que valen sin importar quien lo lanzo -- el
 * launcher, la shell o una asociacion de archivo. */
#define SAVANXP_WM_WINDOW_STYLE_NONE 0x00000000u

/*
 * La ventana tiene TAMANO FIJO: el usuario no la redimensiona. Los bordes no
 * agarran y el boton de maximizar se dibuja deshabilitado.
 *
 * Es para el programa cuyo layout no tiene como estirarse -- un tablero de
 * celdas, un teclado de calculadora --, que en una ventana mas grande no gana
 * nada y deja un margen vacio alrededor del contenido.
 *
 * NO congela el tamano: lo que prohibe es que lo cambie el USUARIO. El programa
 * sigue pidiendo el suyo con gfx_request_content_size, y en una ventana fija el
 * WM le hace caso TODAS las veces y no solo la primera -- la razon por la que
 * el size hint es de una sola vez es no pelearle la geometria al usuario, y
 * aca el usuario no tiene ninguna que defender. Es lo que permite que el
 * buscaminas cambie de nivel y la ventana lo siga.
 */
#define SAVANXP_WM_WINDOW_STYLE_FIXED_SIZE 0x00000001u

/* The single optional argument gfx_desktop_launch_arg passes as argv[1]:
 * enough for "open this file". */
#define SAVANXP_DESKTOP_LAUNCH_ARG_CAPACITY 192u

/* Pedido de cierre del WM al cliente (bit de savanxp_gpu_client_surface_header
 * .flags). El WM lo prende y despues senala el evento de wake. */
#define SAVANXP_GPU_CLIENT_SURFACE_FLAG_SHUTDOWN 0x00000001u

/*
 * Client requests to the WM, inside the surface header (since v4). They used
 * to be three pipes per window; as a section they cost the WM no descriptor.
 * The client writes and then sets the submit event, the same one it uses for
 * frames. The WM treats all of it as hostile memory: copy, validate, and only
 * then use.
 *
 * cursor_shape: the last shape asked for (enum savanxp_cursor_shape). State,
 * not a queue: 0 is the arrow, which is also "no request".
 *
 * size_hint_*: the client-area size (the "content") the program asks for its
 * window. A SUGGESTION: the WM clamps it to the surface capacity and ignores
 * it once the window has left its launch geometry (maximized, fullscreen or
 * resized by hand), because from then on the user decides. It exists because
 * the program knows the natural size of its window -- it is its layout --,
 * not the WM. Seqlock: the client makes size_hint_sequence odd, writes width
 * and height and makes it even again; the WM serves each new even value once,
 * and only the latest.
 *
 * Launching is not a request: since protocol v5 a client starts programs
 * itself and they connect on their own.
 */
/*
 * Ventanas con dueno (docs/desktop/OWNED_WINDOWS.md). Un proceso tiene UNA conexion con
 * el WM -- estos canales -- y puede tener varias ventanas: la principal (id 0) y
 * hasta SAVANXP_WM_MAX_OWNED_WINDOWS con dueno, cuyo id elige el cliente. Crear
 * una es un pedido por el header de la PRINCIPAL; su superficie llega como
 * handle de seccion por la cola de handles del pipe de eventos
 * (SAVANXP_SYS_PIPE_SEND_HANDLE), y sus eventos por el mismo pipe con su id en
 * savanxp_wm_event.window_id.
 */
#define SAVANXP_WM_MAX_OWNED_WINDOWS 4u
#define SAVANXP_WM_WINDOW_TITLE_CAPACITY 64u

enum savanxp_wm_window_action {
    SAVANXP_WM_WINDOW_ACTION_NONE = 0,
    /* Crear la ventana `window_id`. Respuesta 0: el handle de la seccion quedo
     * en la cola del pipe de eventos y hay que recibirlo. */
    SAVANXP_WM_WINDOW_ACTION_OPEN = 1,
    /* Destruirla. Cerrar una que no existe no es error. */
    SAVANXP_WM_WINDOW_ACTION_CLOSE = 2,
};

/*
 * Window roles (docs/desktop/DESKTOP_ARCHITECTURE.md#window-roles). A role is a
 * property of the window, asked for when it is created. Only the shell client
 * may ask for anything but NORMAL; for any other client the WM refuses the
 * request. The shell's main window is the background, which is granted when
 * the WM starts the shell, never requested.
 */
enum savanxp_wm_window_role {
    /* An owned window (a dialog): framed, above its owner. */
    SAVANXP_WM_WINDOW_ROLE_NORMAL = 0,
    /* Anchored to a screen edge, full width, above normal windows and below a
     * fullscreen app. Its strip is reserved: maximized windows stay out of it.
     * `height` is the strip's thickness; `width`, `x` and `y` are ignored. */
    SAVANXP_WM_WINDOW_ROLE_PANEL = 1,
    /* Frameless, above panels, at `x`,`y` in screen coordinates (clamped to the
     * display). Never takes keyboard focus. */
    SAVANXP_WM_WINDOW_ROLE_POPUP = 2,
};

/* role_flags for SAVANXP_WM_WINDOW_ROLE_PANEL: the edge it sits on. */
#define SAVANXP_WM_PANEL_EDGE_BOTTOM 0x00000000u
#define SAVANXP_WM_PANEL_EDGE_TOP 0x00000001u

/* role_flags for SAVANXP_WM_WINDOW_ROLE_POPUP. A press outside every window of
 * the popup's process asks the process to close it, through the same
 * SAVANXP_GPU_CLIENT_SURFACE_FLAG_SHUTDOWN a dialog's close button uses. The
 * press still reaches whatever it landed on. A press on another window of the
 * same process does not dismiss: that process decides what it means. */
#define SAVANXP_WM_POPUP_DISMISS_ON_OUTSIDE 0x00000001u

struct savanxp_wm_window_request {
    uint32_t action; /* enum savanxp_wm_window_action */
    uint32_t window_id;
    uint32_t width;  /* client area, like the size hint */
    uint32_t height;
    char title[SAVANXP_WM_WINDOW_TITLE_CAPACITY];
    /* Appended after v4 shipped. The header only grows at its end and clients
     * read every offset from it, so a client built before this still maps the
     * surface; it never writes these fields, which stay 0 = NORMAL. */
    uint32_t role; /* enum savanxp_wm_window_role */
    uint32_t role_flags;
    int32_t x;
    int32_t y;
};

/*
 * Presentation: the title, icon, accent and style a window declares about
 * itself (docs/desktop/DESKTOP_ARCHITECTURE.md#presentation-comes-from-the-
 * client). The client runtime fills it from the program's own .sxe before the
 * first frame, under a seqlock like the size hint: `sequence` odd while it is
 * being written. The WM copies each new even value once and never reads
 * files. A client that publishes nothing gets its file name as the title and
 * the generic icon.
 */
#define SAVANXP_WM_PRESENTATION_TITLE_CAPACITY 32u
#define SAVANXP_WM_PRESENTATION_ICON_EXTENT 16u
#define SAVANXP_WM_PRESENTATION_HAS_ACCENT 0x00000001u

struct savanxp_wm_presentation {
    uint32_t sequence;     /* written only by the client */
    uint32_t flags;        /* SAVANXP_WM_PRESENTATION_* */
    uint32_t accent;       /* 0x00RRGGBB, valid with _HAS_ACCENT */
    uint32_t window_style; /* SAVANXP_WM_WINDOW_STYLE_* */
    uint32_t icon_extent;  /* 0 = no icon, else SAVANXP_WM_PRESENTATION_ICON_EXTENT */
    uint32_t reserved;
    char title[SAVANXP_WM_PRESENTATION_TITLE_CAPACITY];
    uint32_t icon_pixels[SAVANXP_WM_PRESENTATION_ICON_EXTENT * SAVANXP_WM_PRESENTATION_ICON_EXTENT]; /* BGRA8888 */
};

struct savanxp_wm_client_requests {
    uint32_t cursor_shape;
    uint32_t size_hint_width;
    uint32_t size_hint_height;
    uint32_t size_hint_sequence;
    /* Un pedido de ventana a la vez, con el mismo seqlock que el size hint: el
     * cliente pone window_sequence en impar, escribe `window` y lo vuelve a par.
     * El WM atiende cada valor par nuevo una vez, escribe el resultado en
     * window_reply_status (0 o -errno), copia la secuencia a
     * window_reply_sequence y senala el wake. Solo el header de la ventana
     * PRINCIPAL se lee; en el de una ventana con dueno se ignora. */
    uint32_t window_sequence;      /* lo escribe solo el cliente */
    uint32_t window_reply_sequence; /* lo escribe solo el WM */
    int32_t window_reply_status;    /* lo escribe solo el WM */
    uint32_t window_reserved;
    struct savanxp_wm_window_request window;
    /* What the window says about itself, published by the client (appended
     * after v4 shipped, like the window roles). */
    struct savanxp_wm_presentation presentation;
};

struct savanxp_gpu_client_surface_header {
    uint32_t magic;
    uint32_t pixels_offset;
    struct savanxp_fb_info info;
    uint32_t version;
    uint32_t flags; /* SAVANXP_GPU_CLIENT_SURFACE_FLAG_*, los escribe el WM */
    uint32_t pixel_format;
    uint32_t reserved0;
    uint32_t command_offset;
    uint32_t batch_capacity;
    uint32_t rect_capacity;
    uint32_t reserved1;
    uint64_t submit_sequence;
    uint64_t retired_sequence;
    uint64_t composed_sequence;
    struct savanxp_wm_client_requests requests;
};

enum savanxp_mouse_button {
    SAVANXP_MOUSE_BUTTON_LEFT = 1u << 0,
    SAVANXP_MOUSE_BUTTON_RIGHT = 1u << 1,
    SAVANXP_MOUSE_BUTTON_MIDDLE = 1u << 2,
};

/* Flags de savanxp_mouse_event.flags.
 *
 * SAVANXP_MOUSE_FLAG_ABSOLUTE: absolute_x/absolute_y traen la posicion
 * verdadera del puntero y hay que asignarlas, no sumarles el delta. Solo lo
 * pone un dispositivo absoluto (virtio-tablet). */
enum savanxp_mouse_event_flag {
    SAVANXP_MOUSE_FLAG_ABSOLUTE = 1u << 0,
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

/* Estado de los modificadores en el instante del evento. El kernel ya lo
 * calculaba para su input::KeyEvent y lo perdia al cruzar a userland, asi que
 * cada consumidor terminaba haciendo lo que hacia el WM: seguir Ctrl a mano
 * por los KEY_DOWN/KEY_UP de SAVANXP_KEY_CTRL. Ese seguimiento a mano se
 * desincroniza en cuanto se pierde un KEY_UP -- soltar Ctrl con el foco en
 * otra ventana deja al modificador trabado --, y el estado real lo tiene el
 * driver. include/kernel/input.hpp deriva sus ModifierFlags de estos valores,
 * asi que no pueden divergir. */
enum savanxp_key_modifier {
    SAVANXP_KEY_MOD_SHIFT = 1u << 0,
    SAVANXP_KEY_MOD_CTRL = 1u << 1,
    SAVANXP_KEY_MOD_ALT = 1u << 2,
    SAVANXP_KEY_MOD_ALT_GR = 1u << 3,
    SAVANXP_KEY_MOD_CAPS_LOCK = 1u << 4,
    SAVANXP_KEY_MOD_NUM_LOCK = 1u << 5,
    SAVANXP_KEY_MOD_SCROLL_LOCK = 1u << 6,
};

struct savanxp_input_event {
    uint32_t type;
    uint32_t key;
    int32_t ascii;
    uint32_t modifiers; /* enum savanxp_key_modifier */
};

/* Ticks de rueda acumulados desde el evento anterior. Positivo = lejos del
 * usuario (el contenido sube, el indice de una lista baja), misma convencion
 * que REL_WHEEL de evdev. Cero en un mouse sin rueda, y cero en la mayoria de
 * los eventos de un mouse con rueda: es un campo esporadico, no un estado.
 *
 * Va como int32_t y no como un flag porque la magnitud importa -- un tick es
 * un tick, pero perderlos no se recupera, mientras que un delta de movimiento
 * perdido se reubica solo: absolute_x/absolute_y traen la posicion verdadera
 * del puntero y el consumidor la asigna. Por eso la rueda se SUMA en todo el
 * camino (cola del kernel, coalescencia del WM) en vez de pisarse. */
struct savanxp_mouse_event {
    int32_t delta_x;
    int32_t delta_y;
    int32_t wheel;
    uint32_t buttons;
    /* Posicion del puntero en la pantalla, en pixeles, presente solo si flags
     * trae SAVANXP_MOUSE_FLAG_ABSOLUTE. Sin el flag, x/y no se tocan.
     *
     * Un puntero absoluto (virtio-tablet) es la verdad: el host la manda y no
     * depende de lo que el invitado haya acumulado. Un delta de movimiento
     * perdido -- cola llena, evento descartado -- es posicion que no vuelve,
     * porque despues de la frontera del kernel el consumidor solo suma. Por eso
     * la posicion viaja entera: el que aplica un evento con el flag pone
     * absolute_x/y en vez de sumarle el delta, y un delta perdido se corrige en
     * el proximo evento en vez decorrerse para siempre. Un puntero relativo
     * (PS/2) no tiene posicion que mandar y no pone el flag. */
    int32_t absolute_x;
    int32_t absolute_y;
    uint32_t flags;
};

/* Pointer event delivered by the compositor to a windowed client over its
 * event pipe (savanxp/wm_protocol.h). x,y are relative to the top-left of the
 * client's own surface (already accounting for window frame and position), so
 * apps can hit-test in their local coordinate space and stay aligned with the
 * system cursor the compositor draws. These are already absolute, so this one
 * needs no savanxp_mouse_event_flag: it is built by the compositor from the
 * cursor position it already resolved, not read off a device. */
struct savanxp_gui_pointer_event {
    int32_t x;
    int32_t y;
    int32_t wheel; /* mismo significado y signo que savanxp_mouse_event.wheel */
    uint32_t buttons;
};

enum savanxp_wm_event_kind {
    SAVANXP_WM_EVENT_KEY = 1,
    SAVANXP_WM_EVENT_POINTER = 2,
    /* The first record a client reads after connecting (protocol v5): the
     * WM's answer to its hello. payload.welcome. */
    SAVANXP_WM_EVENT_WELCOME = 3,
};

/*
 * Connecting to the WM, protocol v5 (savanxp/wm_protocol.h). A client opens the
 * WM's port (SAVANXP_WM_PORT_NAME) and writes this hello on the request pipe;
 * the WM answers on the event pipe with a WELCOME record. With status 0, the
 * handles are already queued on the event pipe, in this order: the surface
 * section, the wake event, the session's submit event, then the input ring
 * (if SAVANXP_WM_HELLO_INPUT_RING was asked) or the window list section (for
 * the shell), then the request event (if SAVANXP_WM_HELLO_REQUEST_EVENT was
 * asked). 32 bytes, like an event record.
 */
#define SAVANXP_WM_HELLO_MAGIC 0x4f4c4548u /* "HELO" */
#define SAVANXP_WM_PROTOCOL_VERSION 5u

/* The client wants its input on a shared ring instead of the event pipe
 * (what a native client without a POSIX read used to need). */
#define SAVANXP_WM_HELLO_INPUT_RING 0x00000001u
/* The client asks for the shell role (savanxp/wm_shell_protocol.h). Granted
 * only to a process the session manager started -- its parent is the WM's
 * parent, or the WM itself -- and to one at a time; otherwise the welcome
 * carries -EACCES. See docs/desktop/DESKTOP_ARCHITECTURE.md#who-is-the-shell-
 * decided-by-parentage. */
#define SAVANXP_WM_HELLO_SHELL 0x00000002u
/* The client sets a request event of its own after a request in its header,
 * and the submit event only after a frame: frames go to compositord, requests
 * to the WM (docs/desktop/DESKTOP_ARCHITECTURE.md#frames). A client that does
 * not ask has its requests seen on the WM's next tick. */
#define SAVANXP_WM_HELLO_REQUEST_EVENT 0x00000004u

struct savanxp_wm_hello {
    uint32_t magic;        /* SAVANXP_WM_HELLO_MAGIC */
    uint32_t version;      /* SAVANXP_WM_PROTOCOL_VERSION */
    uint32_t flags;        /* SAVANXP_WM_HELLO_* */
    uint32_t launch_flags; /* SAVANXP_DESKTOP_LAUNCH_FLAG_*, what the program declares about itself */
    uint32_t reserved[4];
};

/* The welcome carries the outcome; the rest of the window (size, style) is in
 * the surface header as always. */
#define SAVANXP_WM_WELCOME_SHELL 0x00000001u /* the connection has the shell role */
#define SAVANXP_WM_WELCOME_REQUEST_EVENT 0x00000002u /* the request event is the last handle */

struct savanxp_wm_welcome {
    int32_t status;        /* 0, or -errno and no handles */
    uint32_t handle_count; /* handles queued before this record */
    uint32_t flags;        /* SAVANXP_WM_WELCOME_* */
    uint32_t reserved[3];
};

/* Registro del canal de eventos WM -> cliente (savanxp/wm_protocol.h). Teclado
 * y puntero comparten el pipe, asi que cada registro dice que trae. Mide 32
 * bytes a proposito: el pipe del kernel tiene 8 KiB, un multiplo exacto, y
 * mientras todas las escrituras y lecturas sean de registros enteros el
 * espacio libre tambien lo es -- una escritura nunca queda por la mitad. */
struct savanxp_wm_event {
    uint32_t kind; /* enum savanxp_wm_event_kind */
    /* Ventana del proceso a la que va el evento: 0 la principal, 1..N una con
     * dueno (SAVANXP_WM_MAX_OWNED_WINDOWS). El pipe es uno por proceso. */
    uint32_t window_id;
    union {
        struct savanxp_input_event key;
        struct savanxp_gui_pointer_event pointer;
        struct savanxp_wm_welcome welcome;
        uint8_t bytes[24];
    } payload;
};

/* Anillo de entrada WM -> cliente para procesos nativos, que no pueden leer
 * el pipe (savanxp/wm_protocol.h). Mismo registro de 32 bytes, misma
 * disciplina SPSC que la cola de launches: el WM escribe el registro y
 * DESPUES publica head; el cliente drena hasta head y despues publica tail.
 * Los dos contadores solo crecen (indice = secuencia % capacidad). Lleno es
 * head - tail == capacidad y se descarta como el pipe lleno: preferimos
 * perder input de un cliente que no drena antes que congelar la sesion. */
#define SAVANXP_WM_INPUT_RING_CAPACITY 64u
#define SAVANXP_WM_INPUT_RING_MAGIC 0x574D4952u
#define SAVANXP_WM_INPUT_RING_VERSION_1 1u

struct savanxp_wm_input_ring {
    uint32_t magic;
    uint32_t version;
    uint64_t head; /* lo escribe solo el WM */
    uint64_t tail; /* lo escribe solo el cliente */
    struct savanxp_wm_event records[SAVANXP_WM_INPUT_RING_CAPACITY];
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

/* Contadores del camino TCP. Son la prueba observable de que la retransmision
 * y el reensamblado corrieron de verdad: un stack que solo funcione contra una
 * red perfecta los deja en cero. */
struct savanxp_net_tcp_stats {
    uint32_t segments_sent;
    uint32_t segments_received;
    uint32_t retransmits;
    uint32_t rx_out_of_order;
    uint32_t rx_duplicates;
    uint32_t rx_out_of_window;
    uint32_t window_updates;
    uint32_t aborts;
};

/* Inyeccion de fallas del camino TCP, para los tests. Cada campo es un
 * periodo: 0 desactiva, N > 0 afecta a uno de cada N segmentos. Sin esto no hay
 * forma de ejercitar la retransmision, porque ni slirp ni una LAN de
 * laboratorio pierden paquetes cuando uno los necesita.
 *
 * drop_tx_every cuenta solo los segmentos que consumen espacio de secuencia
 * (datos, SYN, FIN) y reorder_rx_every solo los que traen datos: son los que
 * ejercitan la retransmision propia y el reensamblado. Contar tambien los ACK
 * puros, que son la mayoria del trafico, deja el periodo gastado en ellos. */
struct savanxp_net_tcp_fault {
    uint32_t drop_tx_every;
    uint32_t drop_rx_every;
    uint32_t reorder_rx_every;
    uint32_t reserved0;
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

/*
 * Yield request used by the idle trampoline (kernel/process.cpp). It is still
 * delivered as a trap because the idle "process" is a real ring-3 context with
 * no other way to re-enter the scheduler. The number belongs here, not to the
 * POSIX surface: the kernel does not route it through the POSIX dispatcher.
 */
#define SAVANXP_SYS_YIELD 8

enum savanxp_open_flags {
    SAVANXP_OPEN_READ = 1u << 0,
    SAVANXP_OPEN_WRITE = 1u << 1,
    SAVANXP_OPEN_CREATE = 1u << 2,
    SAVANXP_OPEN_TRUNCATE = 1u << 3,
    SAVANXP_OPEN_APPEND = 1u << 4,
    SAVANXP_OPEN_NONBLOCK = 1u << 5,
};

enum savanxp_poll_events {
    SAVANXP_POLLIN = 0x0001,
    SAVANXP_POLLOUT = 0x0004,
    SAVANXP_POLLERR = 0x0008,
    SAVANXP_POLLHUP = 0x0010,
    SAVANXP_POLLNVAL = 0x0020,
};

enum savanxp_signal_number {
    SAVANXP_SIGINT = 2,
    SAVANXP_SIGKILL = 9,
    SAVANXP_SIGPIPE = 13,
    SAVANXP_SIGTERM = 15,
    SAVANXP_SIGCHLD = 17,
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

/* Opciones de SAVANXP_SYS_SETSOCKOPT. Los valores coinciden con los SO_* de
 * <sys/socket.h> a proposito, para que la capa POSIX no tenga que traducir.
 * El plazo va en milisegundos y 0 significa esperar indefinidamente, igual que
 * en POSIX. */
enum savanxp_socket_option {
    SAVANXP_SO_RCVTIMEO = 20,
    SAVANXP_SO_SNDTIMEO = 21,
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
