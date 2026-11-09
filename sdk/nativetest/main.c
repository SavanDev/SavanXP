/* Standalone native ABI validation (no POSIX).
 *
 * Built as an external app and stamped EI_OSABI=0x53:
 *
 *     ./tools/build-user.sh --source sdk/nativetest --name nativetest --native
 *
 * Runs headless from init:
 *
 *     ./build.sh smoke native-test
 *
 * Speaks through include/sxn/sxn.h, the header-only native userland
 * surface, with one raw trap kept as a wire-format cross-check. Linking
 * the POSIX runtime objects is fine (dead code never runs), but calling
 * into them would prove nothing about the native dispatcher. Reporting
 * goes through sxn_log (kernel log) and the verdict through sxn_exit
 * (waitpid status). Neither path ever returns: on return crt0 would
 * invoke the POSIX EXIT, which is ENOSYS for a native process.
 *
 * Covers: INFO handshake, LOG, ENOSYS for POSIX numbers and for unknown
 * native numbers, self-spawn plus WAITPID, cross-process event plus
 * shared section (parent writes a marker, child answers, parent waits on
 * the event, then reset plus a zero-timeout poll), and EXIT with the
 * failure count as status. Then the phase 3 calls: CLOCK, DUPLICATE and
 * CLOSE on an event, READ refusing a non-stream handle, OPEN_DEVICE refusing
 * paths and unknown names, DEVICE_CONTROL on gpu0 and WRITE to serial; and a
 * port: a second child ("portchild") connects by name, exchanges a message
 * each way and signals an event handed over its reply pipe; WAIT_MANY on
 * events and streams; and TERMINATE:
 * a third child ("sleeper") waits forever until the parent ends it, and
 * waitpid reports the status the parent chose; PROCESS_QUERY sees it running,
 * then ended with that status, then gone. And the heap
 * (docs/userland/NATIVE_HEAP.md): several MiB across arenas from the native
 * page source. And threads (docs/roadmaps/done/THREADS_ROADMAP.md): workers that
 * share memory and end with their own codes, joined through their handles; a
 * process killed with threads blocked and running; the last thread ending the
 * process; SXN_SYS_EXIT from a second thread ending them all. The first
 * child runs this same binary with
 * argv = ["nativetest", "child", ev, sec] and exits 42.
 *
 * And SUN sound (sxn/sxnaudio.h): with no daemon a stream writes to
 * /dev/audio0 directly; then the real audiod is started and a stream reaches
 * it through its native port, names itself, and finds that name in the
 * daemon's census.
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#include "abi/savanxp_native_abi.h"
#include "savanxp/syscall.h"
#include "sxn/sxn.h"
#include "sxn/sxnwm.h"
#include "sxn/sxnaudio.h"
#include "sxn/sxncapture.h"
#include "sxgfx/virgl_protocol.h"

#define SXN_CHILD_STATUS 42
#define SXN_PARENT_MARKER 0x5EED1u
#define SXN_CHILD_MARKER 0x5EED2u
#define SXN_RING_ROUNDS 50
#define SXN_RING_WAIT_MS 5000

static long raw_info(struct sxn_native_info* out) {
    long result;

    __asm__ volatile("int $0x80"
                     : "=a"(result)
                     : "a"((unsigned long)SXN_SYS_INFO), "D"((unsigned long)out), "S"(0ul), "d"(0ul)
                     : "rcx", "r11", "cc", "memory");
    return result;
}

static int g_failures = 0;

/* A failed check logs its line, so a FAIL says where. */
static void check_at(int ok, int line) {
    char text[40] = "nativetest: check failed at line ";
    char digits[12];
    int count = 0;
    int length = 33;

    if (ok) {
        return;
    }
    g_failures += 1;
    do {
        digits[count++] = (char)('0' + line % 10);
        line /= 10;
    } while (line > 0 && count < 11);
    while (count > 0 && length < (int)sizeof(text) - 1) {
        text[length++] = digits[--count];
    }
    text[length] = '\0';
    (void)sxn_log(text);
}

#define check(ok) check_at((ok), __LINE__)

/* 3D through virgl (docs/roadmaps/open/VIRGL_ROADMAP.md): on a device with
 * virgl, clear a render target on the host and read it back; without it, the
 * 3D ioctls must say there is no 3D instead of pretending. */
static void check_gpu_3d(long gpu, const struct savanxp_gpu_info* info)
{
    enum { size = 64, bytes = size * size * 4 };
    struct savanxp_gpu_3d_capset capset = {0};
    struct savanxp_gpu_3d_context context = {0};
    struct savanxp_gpu_3d_resource target = {0};
    struct savanxp_gpu_3d_resource_ref ref = {0};
    struct savanxp_gpu_3d_submit submit = {0};
    struct savanxp_gpu_3d_transfer transfer = {0};
    uint32_t caps[16];
    uint32_t commands[32];
    unsigned count = 0;
    union { float f; uint32_t u; } red = {1.0f}, green = {0.5f}, blue = {0.0f}, alpha = {1.0f};
    const uint32_t* pixels;
    long section;
    long view;
    int matches = 1;

    capset.data = (uint64_t)(uintptr_t)caps;
    capset.size = sizeof(caps);
    if ((info->flags & SAVANXP_GPU_INFO_FLAG_3D) == 0) {
        check(sxn_device_control(gpu, GPU_IOC_3D_GET_CAPSET, (unsigned long)&capset) == -SAVANXP_ENODEV);
        check(sxn_device_control(gpu, GPU_IOC_3D_CREATE_CONTEXT, (unsigned long)&context) == -SAVANXP_ENODEV);
        (void)sxn_log("nativetest: gpu 3d absent");
        return;
    }

    check(sxn_device_control(gpu, GPU_IOC_3D_GET_CAPSET, (unsigned long)&capset) == 0);
    check(capset.capset_id == 1u || capset.capset_id == 2u);
    check(capset.size > sizeof(caps));
    /* Another process's context id is not ours to use. */
    check(sxn_device_control(gpu, GPU_IOC_3D_DESTROY_CONTEXT, 16) == -SAVANXP_EINVAL);

    context.debug_name[0] = 'n';
    context.debug_name[1] = 't';
    check(sxn_device_control(gpu, GPU_IOC_3D_CREATE_CONTEXT, (unsigned long)&context) == 0);
    section = sxn_section_create(bytes, SXN_SECTION_READ | SXN_SECTION_WRITE);
    view = section >= 0 ? sxn_map_view(section, SXN_SECTION_READ | SXN_SECTION_WRITE) : -1;
    check(context.context_id != 0u && section >= 0 && view > 0);
    if (context.context_id == 0u || view <= 0) {
        return;
    }

    target.context_id = context.context_id;
    target.target = PIPE_TEXTURE_2D;
    target.format = VIRGL_FORMAT_B8G8R8A8_UNORM;
    target.bind = VIRGL_BIND_RENDER_TARGET;
    target.width = size;
    target.height = size;
    target.depth = 1u;
    target.array_size = 1u;
    target.section_handle = (int32_t)section;
    target.backing_size = bytes;
    check(sxn_device_control(gpu, GPU_IOC_3D_CREATE_RESOURCE, (unsigned long)&target) == 0);

    /* A surface on the resource, bound as colour buffer 0, cleared to orange. */
    commands[count++] = VIRGL_CMD_HEADER(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, VIRGL_OBJ_SURFACE_SIZE);
    commands[count++] = 1u;
    commands[count++] = target.resource_id;
    commands[count++] = VIRGL_FORMAT_B8G8R8A8_UNORM;
    commands[count++] = 0u;
    commands[count++] = 0u;
    commands[count++] = VIRGL_CMD_HEADER(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, VIRGL_SET_FRAMEBUFFER_STATE_SIZE(1u));
    commands[count++] = 1u;
    commands[count++] = 0u;
    commands[count++] = 1u;
    commands[count++] = VIRGL_CMD_HEADER(VIRGL_CCMD_CLEAR, 0, VIRGL_OBJ_CLEAR_SIZE);
    commands[count++] = PIPE_CLEAR_COLOR0;
    commands[count++] = red.u;
    commands[count++] = green.u;
    commands[count++] = blue.u;
    commands[count++] = alpha.u;
    commands[count++] = 0u;
    commands[count++] = 0u;
    commands[count++] = 0u;
    submit.context_id = context.context_id;
    submit.size = count * 4u;
    submit.commands = (uint64_t)(uintptr_t)commands;
    check(sxn_device_control(gpu, GPU_IOC_3D_SUBMIT, (unsigned long)&submit) == 0);

    transfer.context_id = context.context_id;
    transfer.resource_id = target.resource_id;
    transfer.direction = SAVANXP_GPU_3D_TRANSFER_FROM_HOST;
    transfer.width = size;
    transfer.height = size;
    transfer.depth = 1u;
    transfer.stride = size * 4u;
    check(sxn_device_control(gpu, GPU_IOC_3D_TRANSFER, (unsigned long)&transfer) == 0);

    /* B8G8R8A8 in memory is 0xAARRGGBB as a little-endian word; 0.5 rounds to
     * 0x80 or 0x7f depending on the host's GL. */
    pixels = (const uint32_t*)(uintptr_t)view;
    for (unsigned index = 0; index < size * size; ++index) {
        const uint32_t pixel = pixels[index];
        if ((pixel & 0xffff00ffu) != 0xffff0000u || ((pixel >> 8) & 0xffu) < 0x7fu ||
            ((pixel >> 8) & 0xffu) > 0x80u) {
            matches = 0;
            break;
        }
    }
    check(matches);

    ref.context_id = context.context_id;
    ref.resource_id = target.resource_id;
    check(sxn_device_control(gpu, GPU_IOC_3D_DESTROY_RESOURCE, (unsigned long)&ref) == 0);
    check(sxn_device_control(gpu, GPU_IOC_3D_DESTROY_RESOURCE, (unsigned long)&ref) == -SAVANXP_EINVAL);
    check(sxn_device_control(gpu, GPU_IOC_3D_DESTROY_CONTEXT, context.context_id) == 0);
    check(sxn_unmap_view((uint64_t)view) == 0);
    check(sxn_close(section) == 0);
    (void)sxn_log(matches ? "nativetest: gpu 3d clear read back" : "nativetest: gpu 3d clear mismatch");
}

static int streq(const char* a, const char* b) {
    while (*a != '\0' && *a == *b) {
        a += 1;
        b += 1;
    }
    return *a == *b;
}

static void itoau(long value, char* buffer) {
    char reversed[24];
    int length = 0;

    if (value == 0) {
        buffer[0] = '0';
        buffer[1] = '\0';
        return;
    }
    while (value > 0 && length < 23) {
        reversed[length] = (char)('0' + (value % 10));
        value /= 10;
        length += 1;
    }
    for (int index = 0; index < length; index += 1) {
        buffer[index] = reversed[length - 1 - index];
    }
    buffer[length] = '\0';
}

static long atou(const char* text) {
    long value = 0;

    while (*text >= '0' && *text <= '9') {
        value = value * 10 + (*text - '0');
        text += 1;
    }
    return value;
}

static void run_child(int argc, char** argv) {
    struct sxn_native_info info;
    long event_handle;
    long section_handle;
    long ev_req;
    long base;

    check(argc == 5);
    check(sxn_info(&info) == 0);
    check(info.abi_version == SXN_ABI_VERSION);
    check(info.subsystem_id == 1u);
    check(info.pid != 0u);
    check(sxn_log("nativetest: child alive") == 0);

    event_handle = atou(argv[2]);
    section_handle = atou(argv[3]);
    ev_req = atou(argv[4]);
    base = sxn_map_view(section_handle, SXN_SECTION_READ | SXN_SECTION_WRITE);
    check(base > 0);
    if (base > 0) {
        volatile uint32_t* cell = (volatile uint32_t*)base;
        check(*cell == SXN_PARENT_MARKER);
        *cell = SXN_CHILD_MARKER;
    }
    check(sxn_log("nativetest: child mapped") == 0);
    check(sxn_event_set(event_handle) == 0);

    /* Ring ping-pong: strict alternation over the shared section, the same
     * shape as a WM submit/retire channel. Each side resets what it
     * consumed; any failure aborts the loop so a broken channel fails in
     * seconds, not at the smoke timeout. */
    if (base > 0) {
        volatile uint32_t* ring = (volatile uint32_t*)base + 16;
        int round;

        for (round = 1; round <= SXN_RING_ROUNDS; round += 1) {
            int round_failures = g_failures;

            check(sxn_wait_one(ev_req, SXN_RING_WAIT_MS) == 0);
            check(ring[0] == (uint32_t)round);
            check(ring[1] == (uint32_t)(round * 3 + 1));
            ring[2] = (uint32_t)(round * 5 + 2);
            ring[3] = (uint32_t)round;
            check(sxn_event_reset(ev_req) == 0);
            check(sxn_event_set(event_handle) == 0);
            if (g_failures != round_failures) {
                break;
            }
        }
        check(sxn_log("nativetest: child ring done") == 0);
    }

    sxn_exit(g_failures == 0 ? SXN_CHILD_STATUS : 1);
    __builtin_unreachable();
}

#define SXN_PORT_CHILD_STATUS 7
#define SXN_TERMINATE_STATUS 99

/* Sleeper child: blocks on an event nobody sets, so only TERMINATE ends it. */
static void run_sleeper(void) {
    long event = sxn_event_create(SXN_EVENT_MANUAL_RESET);

    for (;;) {
        (void)sxn_wait_one(event, 60000);
    }
}

/* TERMINATE as the user (uid 1000), which the parent started it as:
 * argv[2] is a sleeper of the system's that is not this child's descendant,
 * argv[3] one of the user's that is not either. kill's rule: the first is
 * -EPERM, the second, same owner, ends. Exits 0 when both held. */
static void run_user_terminate_child(int argc, char** argv) {
    long system_sleeper;
    long user_sleeper;

    if (argc < 4) {
        sxn_exit(2);
    }
    system_sleeper = atou(argv[2]);
    user_sleeper = atou(argv[3]);
    if (sxn_terminate(system_sleeper, 1) != -SAVANXP_EPERM) {
        sxn_exit(3);
    }
    if (sxn_terminate(user_sleeper, 1) != 0) {
        sxn_exit(4);
    }
    sxn_exit(0);
}

/* Storage child, run as the user: listing is open, sectors and rescans are
 * the system's. argv[2] is the id of the device mounted at "/". */
static void run_storage_user_child(int argc, char** argv) {
    static uint8_t sector[512];
    struct sxn_storage_device device;
    uint32_t id;

    if (argc < 3) {
        sxn_exit(2);
    }
    id = (uint32_t)atou(argv[2]);
    if (sxn_storage_snapshot(0, &device) != 1) {
        sxn_exit(3);
    }
    if (sxn_storage_read(id, 0, 1, sector) != -SAVANXP_EPERM) {
        sxn_exit(4);
    }
    if (sxn_storage_write(id, 0, 1, sector) != -SAVANXP_EPERM) {
        sxn_exit(5);
    }
    if (sxn_storage_rescan(id) != -SAVANXP_EPERM) {
        sxn_exit(6);
    }
    if (sxn_storage_mount(id, "/mnt/user") != -SAVANXP_EPERM || sxn_storage_unmount("/") != -SAVANXP_EPERM) {
        sxn_exit(7);
    }
    sxn_exit(0);
}

/* Port child: connects to the parent's port by name, says ping, reads pong,
 * takes the event the parent handed over and signals it. Native calls only. */
static void run_port_child(void) {
    int32_t handles[2] = {-1, -1};
    char reply[4] = {0};
    long event = -1;

    if (sxn_port_connect("nativetest", handles) != 0 ||
        sxn_write(handles[0], "ping", 4) != 4 ||
        sxn_read(handles[1], reply, 4) != 4 || reply[0] != 'p' || reply[1] != 'o' ||
        (event = sxn_pipe_receive_handle(handles[1])) < 0 ||
        sxn_event_set(event) != 0) {
        sxn_exit(1);
    }
    sxn_exit(SXN_PORT_CHILD_STATUS);
}

/* SPAWN_EX child: owns exactly 0, 1, 2 and 3. Echoes the line read on 0 to
 * 1, says "err" on 2 and "x3" on 3, so the parent can tell every handle
 * landed where the request put it. */
static void run_stdio_child(void) {
    struct sxn_native_info self_info;
    struct sxn_process_query self;
    char line[16] = {0};
    long count = 0;

    if (sxn_info(&self_info) != 0 || sxn_process_query(self_info.pid, &self) != 0 ||
        self.handle_count != 4u) {
        sxn_exit(2);
    }
    count = sxn_read(0, line, sizeof(line));
    if (count <= 0 || sxn_write(1, line, (unsigned long)count) != count ||
        sxn_write(2, "err", 3) != 3 || sxn_write(3, "x3", 2) != 2) {
        sxn_exit(3);
    }
    sxn_exit(0);
}

/* TERMINATE across a generation: starts a sleeper, tells the parent its pid
 * on 1, and exits with whatever status the sleeper ends with. */
static void run_middle_child(void) {
    char* sleeper_argv[2];
    long sleeper = -1;
    int32_t pid_value = 0;
    int status = -1;

    sleeper_argv[0] = "/bin/nativetest.sxe";
    sleeper_argv[1] = "sleeper";
    sleeper = sxn_spawn_stdio("/bin/nativetest.sxe", sleeper_argv, 2);
    if (sleeper <= 0) {
        sxn_exit(2);
    }
    pid_value = (int32_t)sleeper;
    if (sxn_write(1, &pid_value, sizeof(pid_value)) != (long)sizeof(pid_value) ||
        sxn_waitpid(sleeper, &status) != sleeper) {
        sxn_exit(3);
    }
    sxn_exit(status);
}

/* --- threads (docs/roadmaps/done/THREADS_ROADMAP.md, phase 1) ------------------ */

#define SXN_THREAD_STACK_BYTES (64ul * 1024ul)
#define SXN_THREAD_WORKERS 3
#define SXN_THREAD_KILL_STATUS 77
#define SXN_THREAD_LAST_STATUS 55
#define SXN_THREAD_EXITALL_STATUS 66
#define SXN_SHOOTDOWN_STALE_STATUS 44
/* What a #PF ends a process with: 128 + the vector. */
#define SXN_PAGE_FAULT_STATUS (128 + 14)
#define SXN_SHOOTDOWN_WORD 0x5eedu

/* A thread's stack: a private section, mapped, its handle closed (the view
 * keeps it). Returns the top, or 0. */
static void* thread_stack(void) {
    long section = sxn_section_create(SXN_THREAD_STACK_BYTES, SXN_SECTION_READ | SXN_SECTION_WRITE);
    long base;

    if (section < 0) {
        return 0;
    }
    base = sxn_map_view(section, SXN_SECTION_READ | SXN_SECTION_WRITE);
    (void)sxn_close(section);
    return base > 0 ? (void*)(base + (long)SXN_THREAD_STACK_BYTES) : 0;
}

static volatile uint32_t g_thread_marks[SXN_THREAD_WORKERS];
static long g_thread_go = -1;

/* Waits for the go event, leaves its mark in memory the whole process sees,
 * and ends with a code of its own. */
static void thread_worker(void* argument) {
    long index = (long)argument;

    if (sxn_wait_one(g_thread_go, 5000) != 0) {
        sxn_thread_exit(1);
    }
    g_thread_marks[index] = 0x7700u + (uint32_t)index;
    sxn_thread_exit(100 + (int)index);
}

/* Blocked forever: only the end of its process ends it. */
static void thread_blocked(void* argument) {
    long event = (long)argument;

    for (;;) {
        (void)sxn_wait_one(event, 60000);
    }
}

/* Never enters the kernel: ready or running whenever it is looked at. */
static void thread_spinning(void* argument) {
    volatile unsigned long* counter = (volatile unsigned long*)argument;

    for (;;) {
        *counter += 1;
    }
}

/* Outlives the main thread a little, then ends the process as its last
 * thread. */
static void thread_last(void* argument) {
    long event = (long)argument;

    (void)sxn_wait_one(event, 50);
    sxn_thread_exit(SXN_THREAD_LAST_STATUS);
}

/* SXN_SYS_EXIT from a thread that is not the first ends every thread. */
static void thread_exit_all(void* argument) {
    (void)argument;
    sxn_exit(SXN_THREAD_EXITALL_STATUS);
}

/* Reads a view in a loop that never enters the kernel. Unmapping the view
 * under it has to fault it, even when it runs on another core, whose TLB
 * still holds the translation until the kernel makes it flush. */
static volatile unsigned long g_shootdown_reads = 0;

static void thread_reader(void* argument) {
    volatile uint32_t* word = (volatile uint32_t*)argument;

    for (;;) {
        if (*word != SXN_SHOOTDOWN_WORD) {
            sxn_exit(4);
        }
        g_shootdown_reads += 1;
    }
}

/* "shootdown" child: the main thread unmaps the view a reader is reading,
 * while the reader runs (on another core, when there is one). The view holds
 * the section's last reference, so its pages are freed too -- which must not
 * happen while that core can still reach them. The reader faulting ends the
 * process with SXN_PAGE_FAULT_STATUS; a reader that keeps going on a stale
 * translation leaves the main thread to end it with
 * SXN_SHOOTDOWN_STALE_STATUS. */
static void run_shootdown_child(void) {
    long section = sxn_section_create(4096, SXN_SECTION_READ | SXN_SECTION_WRITE);
    long event = sxn_event_create(SXN_EVENT_MANUAL_RESET);
    void* stack = thread_stack();
    long base;
    unsigned long seen;

    if (section < 0 || event < 0 || stack == 0) {
        sxn_exit(2);
    }
    base = sxn_map_view(section, SXN_SECTION_READ | SXN_SECTION_WRITE);
    (void)sxn_close(section);
    if (base <= 0) {
        sxn_exit(2);
    }
    *(volatile uint32_t*)base = SXN_SHOOTDOWN_WORD;
    if (sxn_thread_create(thread_reader, (void*)base, stack, 0) < 0) {
        sxn_exit(2);
    }
    while (g_shootdown_reads < 1000u) {
        (void)sxn_wait_one(event, 1);
    }
    /* Running right now: the count moves while this thread stays in user
     * mode. */
    seen = g_shootdown_reads;
    while (g_shootdown_reads == seen) {
    }
    if (sxn_unmap_view((uint64_t)base) != 0) {
        sxn_exit(3);
    }
    (void)sxn_wait_one(event, 2000);
    sxn_exit(SXN_SHOOTDOWN_STALE_STATUS);
}

/* "threads" child: one thread blocked, two spinning (on two cores, when
 * there are), and the main thread blocked too, once it told the parent (on 1)
 * they all exist. */
static void run_threads_child(void) {
    static volatile unsigned long spins[2] = {0, 0};
    long event = sxn_event_create(SXN_EVENT_MANUAL_RESET);
    void* blocked_stack = thread_stack();
    void* spinning_stack = thread_stack();
    void* second_spinning_stack = thread_stack();
    char ready = 'T';

    if (event < 0 || blocked_stack == 0 || spinning_stack == 0 || second_spinning_stack == 0 ||
        sxn_thread_create(thread_blocked, (void*)event, blocked_stack, 0) < 0 ||
        sxn_thread_create(thread_spinning, (void*)&spins[0], spinning_stack, 0) < 0 ||
        sxn_thread_create(thread_spinning, (void*)&spins[1], second_spinning_stack, 0) < 0) {
        sxn_exit(2);
    }
    while (spins[0] == 0 || spins[1] == 0) {
        (void)sxn_wait_one(event, 1);
    }
    if (sxn_write(1, &ready, 1) != 1) {
        sxn_exit(3);
    }
    for (;;) {
        (void)sxn_wait_one(event, 60000);
    }
}

/* "lastexit" child: the main thread ends first; the other one ends the
 * process. */
static void run_last_exit_child(void) {
    long event = sxn_event_create(SXN_EVENT_MANUAL_RESET);
    void* stack = thread_stack();

    if (event < 0 || stack == 0 || sxn_thread_create(thread_last, (void*)event, stack, 0) < 0) {
        sxn_exit(2);
    }
    sxn_thread_exit(1);
}

/* "exitall" child: a second thread calls SXN_SYS_EXIT while the main thread
 * spins, on another core when there is one: the exit has to wait for that
 * core to stop it. */
static void run_exit_all_child(void) {
    static volatile unsigned long spins = 0;
    void* stack = thread_stack();

    if (stack == 0 || sxn_thread_create(thread_exit_all, 0, stack, 0) < 0) {
        sxn_exit(2);
    }
    for (;;) {
        spins += 1;
    }
}

/* Thread-local storage through the segment bases. Each thread points %fs and
 * %gs at blocks of its own and reads them back many times, giving its core
 * away now and then: whichever core it resumes on, and whoever ran there in
 * between, it must see its own values. */
#define SXN_TLS_THREADS 3
#define SXN_TLS_ROUNDS 2000

struct tls_block {
    struct tls_block* self;
    uint64_t value;
};

static struct tls_block g_tls_blocks[SXN_TLS_THREADS + 1][2];
static long g_tls_pause = -1;

static uint64_t fs_value(void) {
    uint64_t value;
    __asm__ volatile("movq %%fs:8, %0" : "=r"(value));
    return value;
}

static uint64_t gs_value(void) {
    uint64_t value;
    __asm__ volatile("movq %%gs:8, %0" : "=r"(value));
    return value;
}

static int tls_round_trips(long index) {
    struct tls_block* fs = &g_tls_blocks[index][0];
    struct tls_block* gs = &g_tls_blocks[index][1];
    long round;

    if (sxn_set_thread_base(SXN_THREAD_BASE_FS, fs) != 0 ||
        sxn_set_thread_base(SXN_THREAD_BASE_GS, gs) != 0) {
        return 0;
    }
    for (round = 0; round < SXN_TLS_ROUNDS; ++round) {
        if (fs_value() != fs->value || gs_value() != gs->value) {
            return 0;
        }
        if ((round % 64) == 0) {
            (void)sxn_wait_one(g_tls_pause, 1);
        }
    }
    return 1;
}

static void thread_tls(void* argument) {
    sxn_thread_exit(tls_round_trips((long)argument) ? 0 : 1);
}

static void test_thread_bases(void) {
    int32_t handles[SXN_TLS_THREADS];
    int32_t code = -1;
    long index;

    for (index = 0; index <= SXN_TLS_THREADS; ++index) {
        g_tls_blocks[index][0].self = &g_tls_blocks[index][0];
        g_tls_blocks[index][0].value = 0xf5000000u + (uint64_t)index;
        g_tls_blocks[index][1].self = &g_tls_blocks[index][1];
        g_tls_blocks[index][1].value = 0x65000000u + (uint64_t)index;
    }
    g_tls_pause = sxn_event_create(SXN_EVENT_MANUAL_RESET);
    check(g_tls_pause >= 0);

    for (index = 0; index < SXN_TLS_THREADS; ++index) {
        void* stack = thread_stack();

        check(stack != 0);
        handles[index] = (int32_t)sxn_thread_create(thread_tls, (void*)index, stack, 0);
        check(handles[index] >= 0);
    }
    /* The main thread takes part too, with the last pair of blocks. */
    check(tls_round_trips(SXN_TLS_THREADS));
    for (index = 0; index < SXN_TLS_THREADS; ++index) {
        check(sxn_wait_one(handles[index], 10000) == 0);
        check(sxn_thread_exit_code(handles[index], &code) == 0 && code == 0);
        (void)sxn_close(handles[index]);
    }

    /* Refusals: an unknown base, a non-canonical address, a kernel one. */
    check(sxn_set_thread_base(0, &g_tls_blocks[0][0]) == -SAVANXP_EINVAL);
    check(sxn_set_thread_base(SXN_THREAD_BASE_FS, (void*)0x0000800000000000ul) == -SAVANXP_EINVAL);
    check(sxn_set_thread_base(SXN_THREAD_BASE_GS, (void*)0xffff800000000000ul) == -SAVANXP_EINVAL);
    /* Still its own after the refusals. */
    check(fs_value() == g_tls_blocks[SXN_TLS_THREADS][0].value);
    check(sxn_set_thread_base(SXN_THREAD_BASE_FS, 0) == 0 && sxn_set_thread_base(SXN_THREAD_BASE_GS, 0) == 0);
    (void)sxn_close(g_tls_pause);
    check(sxn_log("nativetest: thread bases done") == 0);
}

/* Wait on an address (threads phase 4). Waiters parked on a word are
 * released by count, an address wait that times out or finds the word changed
 * comes straight back, and a three-state mutex built on the pair (the classic
 * futex mutex) keeps a counter exact while four threads hammer it. */
#define SXN_GATE_THREADS 3
#define SXN_LOCK_THREADS 4
#define SXN_LOCK_ROUNDS 5000

static volatile uint32_t g_gate;
static volatile uint32_t g_gate_ready;
static volatile long g_gate_results[SXN_GATE_THREADS];
static volatile uint32_t g_lock;
static volatile uint64_t g_locked_counter;
static volatile uint32_t g_lock_parks;
static volatile uint64_t g_wide = 0x1122334455667788ull;
static long g_address_pause = -1;

static void address_pause(long ms) {
    (void)sxn_wait_one(g_address_pause, ms);
}

static void gate_waiter(void* argument) {
    long index = (long)argument;

    __atomic_fetch_add(&g_gate_ready, 1u, __ATOMIC_SEQ_CST);
    g_gate_results[index] = sxn_wait_address(&g_gate, 0, sizeof(g_gate), 10000);
    sxn_thread_exit(0);
}

/* 0 free, 1 taken, 2 taken with waiters. */
static void futex_lock(volatile uint32_t* word) {
    uint32_t seen = 0;

    if (__atomic_compare_exchange_n(word, &seen, 1u, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        return;
    }
    if (seen != 2u) {
        seen = __atomic_exchange_n(word, 2u, __ATOMIC_SEQ_CST);
    }
    while (seen != 0u) {
        __atomic_fetch_add(&g_lock_parks, 1u, __ATOMIC_RELAXED);
        (void)sxn_wait_address(word, 2u, sizeof(*word), -1);
        seen = __atomic_exchange_n(word, 2u, __ATOMIC_SEQ_CST);
    }
}

static void futex_unlock(volatile uint32_t* word) {
    if (__atomic_fetch_sub(word, 1u, __ATOMIC_SEQ_CST) != 1u) {
        __atomic_store_n(word, 0u, __ATOMIC_SEQ_CST);
        (void)sxn_wake_address(word, 1);
    }
}

static void lock_worker(void* argument) {
    long round;
    int spin;

    (void)argument;
    for (round = 0; round < SXN_LOCK_ROUNDS; ++round) {
        uint64_t value;

        futex_lock(&g_lock);
        /* A read-modify-write wide enough to lose updates without the lock. */
        value = g_locked_counter;
        for (spin = 0; spin < 16; ++spin) {
            __asm__ volatile("pause");
        }
        g_locked_counter = value + 1u;
        /* Now and then sleep holding it, so the others park even on one core. */
        if ((round % 500) == 250) {
            address_pause(1);
        }
        futex_unlock(&g_lock);
    }
    sxn_thread_exit(0);
}

static void test_address_waits(void) {
    int32_t handles[SXN_LOCK_THREADS];
    volatile uint8_t* wide_bytes = (volatile uint8_t*)&g_wide;
    int32_t code = -1;
    uint64_t started;
    long woken = 0;
    long index;
    int tries;

    g_address_pause = sxn_event_create(SXN_EVENT_MANUAL_RESET);
    check(g_address_pause >= 0);

    /* Straight back: the word differs, a zero timeout, a timeout that runs
     * out, the low `size` bytes compared and nothing else, bad requests. */
    g_gate = 5u;
    check(sxn_wait_address(&g_gate, 0, 4, -1) == -SAVANXP_EAGAIN);
    check(sxn_wait_address(&g_gate, 5, 4, 0) == -SAVANXP_ETIMEDOUT);
    started = sxn_clock_ns();
    check(sxn_wait_address(&g_gate, 5, 4, 30) == -SAVANXP_ETIMEDOUT);
    check(sxn_clock_ns() - started >= 25000000ull);
    check(sxn_wait_address(&g_wide, 0x1122334455667788ull, 8, 0) == -SAVANXP_ETIMEDOUT);
    check(sxn_wait_address(&g_wide, 0x1122334400000000ull, 8, 0) == -SAVANXP_EAGAIN);
    check(sxn_wait_address(wide_bytes, 0xff88u, 1, 0) == -SAVANXP_ETIMEDOUT);
    check(sxn_wait_address(wide_bytes, 0x0011u, 2, 0) == -SAVANXP_EAGAIN);
    check(sxn_wait_address(&g_gate, 5, 3, 0) == -SAVANXP_EINVAL);
    check(sxn_wait_address(wide_bytes + 1, 0, 4, 0) == -SAVANXP_EINVAL);
    check(sxn_wait_address((void*)0, 0, 4, 0) == -SAVANXP_EINVAL);
    check(sxn_wait_address((void*)0xffff800000000000ul, 0, 4, 0) == -SAVANXP_EINVAL);
    check(sxn_wake_address(&g_gate, SXN_WAKE_ALL) == 0);

    /* Three waiters on one word: one released alone, then the other two. The
     * word never changes, so only a wake lets a waiter go; a wake that finds
     * fewer parked than expected is retried after a pause. */
    g_gate = 0u;
    g_gate_ready = 0u;
    for (index = 0; index < SXN_GATE_THREADS; ++index) {
        void* stack = thread_stack();

        g_gate_results[index] = 1;
        check(stack != 0);
        handles[index] = (int32_t)sxn_thread_create(gate_waiter, (void*)index, stack, 0);
        check(handles[index] >= 0);
    }
    for (tries = 0; tries < 2000 && g_gate_ready != SXN_GATE_THREADS; ++tries) {
        address_pause(1);
    }
    check(g_gate_ready == SXN_GATE_THREADS);
    address_pause(20);
    for (tries = 0; tries < 500 && woken < 1; ++tries) {
        long step = sxn_wake_address(&g_gate, 1);

        check(step == 0 || step == 1);
        woken += step;
        if (step == 0) {
            address_pause(2);
        }
    }
    check(woken == 1);
    for (tries = 0; tries < 500 && woken < SXN_GATE_THREADS; ++tries) {
        long step = sxn_wake_address(&g_gate, SXN_WAKE_ALL);

        check(step >= 0 && step <= SXN_GATE_THREADS - woken);
        woken += step;
        if (woken < SXN_GATE_THREADS) {
            address_pause(2);
        }
    }
    check(woken == SXN_GATE_THREADS);
    for (index = 0; index < SXN_GATE_THREADS; ++index) {
        check(sxn_wait_one(handles[index], 10000) == 0);
        check(g_gate_results[index] == 0);
        (void)sxn_close(handles[index]);
    }

    /* The futex mutex. */
    g_lock = 0u;
    g_locked_counter = 0u;
    g_lock_parks = 0u;
    for (index = 0; index < SXN_LOCK_THREADS; ++index) {
        void* stack = thread_stack();

        check(stack != 0);
        handles[index] = (int32_t)sxn_thread_create(lock_worker, 0, stack, 0);
        check(handles[index] >= 0);
    }
    for (index = 0; index < SXN_LOCK_THREADS; ++index) {
        check(sxn_wait_one(handles[index], 60000) == 0);
        check(sxn_thread_exit_code(handles[index], &code) == 0 && code == 0);
        (void)sxn_close(handles[index]);
    }
    check(g_locked_counter == (uint64_t)SXN_LOCK_THREADS * SXN_LOCK_ROUNDS);
    check(g_lock == 0u);
    check(g_lock_parks != 0u);
    (void)sxn_close(g_address_pause);
    {
        /* How many times a locker parked: contention reached the kernel. */
        static const char prefix[] = "nativetest: address waits done, parks=";
        char line[sizeof(prefix) + 12];
        char digits[12];
        unsigned parks = g_lock_parks;
        unsigned count = 0;
        unsigned at;

        for (at = 0; at < sizeof(prefix) - 1u; ++at) {
            line[at] = prefix[at];
        }
        do {
            digits[count++] = (char)('0' + parks % 10u);
            parks /= 10u;
        } while (parks != 0u);
        while (count != 0u) {
            line[at++] = digits[--count];
        }
        line[at] = '\0';
        check(sxn_log(line) == 0);
    }
}

static void test_threads(const struct sxn_native_info* info) {
    int32_t handles[SXN_THREAD_WORKERS] = {-1, -1, -1};
    uint32_t tids[SXN_THREAD_WORKERS] = {0, 0, 0};
    struct sxn_process_query before;
    struct sxn_process_query after;
    int32_t code = 0;
    long index;

    check(sxn_process_query(info->pid, &before) == 0);
    g_thread_go = sxn_event_create(SXN_EVENT_MANUAL_RESET);
    check(g_thread_go >= 0);

    for (index = 0; index < SXN_THREAD_WORKERS; ++index) {
        void* stack = thread_stack();

        check(stack != 0);
        handles[index] = (int32_t)sxn_thread_create(thread_worker, (void*)index, stack, &tids[index]);
        check(handles[index] >= 0);
        check(tids[index] != 0u && tids[index] != info->pid);
    }
    check(tids[0] != tids[1] && tids[1] != tids[2] && tids[0] != tids[2]);

    /* Nothing ended yet: no exit code, and a zero-timeout wait times out. */
    check(sxn_thread_exit_code(handles[0], &code) == -SAVANXP_EAGAIN);
    check(sxn_wait_many(handles, SXN_THREAD_WORKERS, 0) == -SAVANXP_ETIMEDOUT);

    check(sxn_event_set(g_thread_go) == 0);
    for (index = 0; index < SXN_THREAD_WORKERS; ++index) {
        check(sxn_wait_one(handles[index], 5000) == 0);
        check(sxn_thread_exit_code(handles[index], &code) == 0 && code == 100 + (int32_t)index);
        check(g_thread_marks[index] == 0x7700u + (uint32_t)index);
        /* Manual reset: an ended thread stays signaled. */
        check(sxn_wait_one(handles[index], 0) == 0);
    }

    /* Refusals: no entry, no stack, a handle that is not a thread. */
    check(sxn_thread_create(0, 0, thread_stack(), 0) == -SAVANXP_EINVAL);
    check(sxn_thread_create(thread_worker, 0, 0, 0) == -SAVANXP_EINVAL);
    check(sxn_thread_exit_code(g_thread_go, &code) == -SAVANXP_EBADF);

    for (index = 0; index < SXN_THREAD_WORKERS; ++index) {
        (void)sxn_close(handles[index]);
    }
    (void)sxn_close(g_thread_go);
    check(sxn_process_query(info->pid, &after) == 0 && after.handle_count == before.handle_count);

    /* A process killed with a thread blocked, one spinning and the main one
     * blocked: TERMINATE ends them all and waitpid sees the status. */
    {
        char* child_argv[2];
        struct sxn_spawn_request request;
        int32_t report[2] = {-1, -1};
        char ready = 0;
        long child = -1;
        int status = -1;

        child_argv[0] = "/bin/nativetest.sxe";
        child_argv[1] = "threads";
        check(sxn_pipe_create(report) == 0);
        sxn_spawn_request_init(&request, "/bin/nativetest.sxe", child_argv, 2);
        request.std_handles[1] = report[1];
        child = sxn_spawn_ex(&request);
        check(child > 0);
        (void)sxn_close(report[1]);
        if (child > 0) {
            check(sxn_read(report[0], &ready, 1) == 1 && ready == 'T');
            check(sxn_terminate(child, SXN_THREAD_KILL_STATUS) == 0);
            check(sxn_waitpid(child, &status) == child && status == SXN_THREAD_KILL_STATUS);
        }
        (void)sxn_close(report[0]);
    }

    /* The last live thread ends the process, with its own code. */
    {
        char* child_argv[2];
        long child;
        int status = -1;

        child_argv[0] = "/bin/nativetest.sxe";
        child_argv[1] = "lastexit";
        child = sxn_spawn_stdio("/bin/nativetest.sxe", child_argv, 2);
        check(child > 0);
        check(child <= 0 || (sxn_waitpid(child, &status) == child && status == SXN_THREAD_LAST_STATUS));
    }

    /* SXN_SYS_EXIT from any thread ends them all. */
    {
        char* child_argv[2];
        long child;
        int status = -1;

        child_argv[0] = "/bin/nativetest.sxe";
        child_argv[1] = "exitall";
        child = sxn_spawn_stdio("/bin/nativetest.sxe", child_argv, 2);
        check(child > 0);
        check(child <= 0 || (sxn_waitpid(child, &status) == child && status == SXN_THREAD_EXITALL_STATUS));
    }

    /* A view unmapped while a sibling reads it on another core: the reader
     * faults instead of reading on through a stale translation. */
    {
        char* child_argv[2];
        long child;
        int status = -1;

        child_argv[0] = "/bin/nativetest.sxe";
        child_argv[1] = "shootdown";
        child = sxn_spawn_stdio("/bin/nativetest.sxe", child_argv, 2);
        check(child > 0);
        check(child <= 0 || (sxn_waitpid(child, &status) == child && status == SXN_PAGE_FAULT_STATUS));
    }

    check(sxn_log("nativetest: threads done") == 0);
}

/* SUN sound (sxn/sxnaudio.h, docs/media/AUDIO_DAEMON.md#sun-clients). The
 * smoke boots without a session, so there is no daemon at first: the stream
 * must go straight to the device. Then the real /bin/audiod is started and a
 * second stream must reach it through the native port, under the name it
 * declared. Without an audio device the whole test is skipped: the machine
 * cannot say anything about sound. */
static void test_audio(void) {
    static int16_t frames[480 * 2]; /* more than one record's worth */
    struct sxnaudio_stream stream;
    char* const audiod_argv[] = {(char*)"/bin/audiod", (char*)"--no-preferences", 0};
    long result = 0;
    long pid = 0;
    long pause = sxn_event_create(0);
    int attempt = 0;
    int found = 0;
    int status = 0;
    uint32_t index = 0;

    for (index = 0; index < 480u; index += 1) {
        int16_t value = (index / 40u) % 2u == 0 ? 1200 : -1200;
        frames[index * 2u] = value;
        frames[index * 2u + 1u] = value;
    }

    result = sxnaudio_open(&stream, "nativetest");
    if (result == -SAVANXP_ENOENT) {
        (void)sxn_log("nativetest: audio: no device, skipped");
        (void)sxn_close(pause);
        return;
    }
    check(result == 0);
    check(stream.mode == SXNAUDIO_MODE_DIRECT);
    {
        struct savanxp_audio_endpoint first, again;
        struct savanxp_audio_endpoint_selection selection;
        struct savanxp_audio_endpoint_query invalid = {0};
        long readonly = sxn_open_device("audio0",SXN_DEVICE_READ);
        check(sxnaudio_endpoint_get(stream.device,0,&first) == 0);
        check(sxnaudio_endpoint_get(stream.device,0,&again) == 0 && first.id == again.id &&
              first.device_id == again.device_id && first.name[0] && first.device_name[0]);
        check((first.flags & (SAVANXP_AUDIO_PLAYBACK|SAVANXP_AUDIO_AVAILABLE|SAVANXP_AUDIO_SELECTED)) ==
              (SAVANXP_AUDIO_PLAYBACK|SAVANXP_AUDIO_AVAILABLE|SAVANXP_AUDIO_SELECTED));
        check(sxnaudio_endpoint_get(stream.device,SAVANXP_AUDIO_MAX_ENDPOINTS,&again) == -SAVANXP_ENOENT);
        for (index = 0; index < SAVANXP_AUDIO_MAX_ENDPOINTS; ++index) {
            if (sxnaudio_endpoint_get(stream.device,index,&again) < 0) break;
            check(!(again.flags & SAVANXP_AUDIO_CONNECTED) ||
                  ((again.flags & (SAVANXP_AUDIO_PRESENCE_VALID|SAVANXP_AUDIO_PRESENCE_CAPABLE)) ==
                   (SAVANXP_AUDIO_PRESENCE_VALID|SAVANXP_AUDIO_PRESENCE_CAPABLE)));
            check(!(again.flags & SAVANXP_AUDIO_PRESENCE_VALID) ||
                  (again.flags & SAVANXP_AUDIO_PRESENCE_CAPABLE));
            check(!(again.flags & SAVANXP_AUDIO_PRESENCE_CAPABLE) || (again.flags & SAVANXP_AUDIO_JACK));
        }
        selection.id = first.id; selection.direction = SAVANXP_AUDIO_PLAYBACK; selection.reserved = 0;
        check(readonly >= 0 && sxn_device_control(readonly,AUDIO_IOC_SELECT_ENDPOINT,
              (unsigned long)(uintptr_t)&selection) == -SAVANXP_EACCES);
        if (readonly >= 0) (void)sxn_close(readonly);
        invalid.reserved = 1;
        check(sxn_device_control(stream.device,AUDIO_IOC_GET_ENDPOINT,
              (unsigned long)(uintptr_t)&invalid) == -SAVANXP_EINVAL);
        selection.direction = 3;
        check(sxn_device_control(stream.device,AUDIO_IOC_SELECT_ENDPOINT,
              (unsigned long)(uintptr_t)&selection) == -SAVANXP_EINVAL);
    }
    check(stream.rate_hz >= 8000u);
    check(sxnaudio_frames_due(&stream, 50) > 0);
    check(sxnaudio_write(&stream, frames, 480u) == 480);
    sxnaudio_close(&stream);
    check(stream.mode == SXNAUDIO_MODE_CLOSED);

    pid = sxn_spawn("/bin/audiod", audiod_argv, 2);
    check(pid > 0);
    if (pid <= 0) {
        (void)sxn_close(pause);
        return;
    }
    /* The port appears once the daemon owns the device. */
    for (attempt = 0; attempt < 100; ++attempt) {
        int32_t probe[2] = {-1, -1};
        if (sxn_port_connect(SAVANXP_AUDIOD_PORT_NAME, probe) == 0) {
            (void)sxn_close(probe[0]);
            (void)sxn_close(probe[1]);
            break;
        }
        (void)sxn_wait_one(pause, 50);
    }
    check(attempt < 100);
    /* The native name enforces singleton startup even without UDP/NIC. */
    {
        long duplicate = sxn_spawn("/bin/audiod", audiod_argv, 2);
        int duplicate_status = -1;
        check(duplicate > 0);
        if (duplicate > 0)
            check(sxn_waitpid(duplicate, &duplicate_status) == duplicate && duplicate_status == 0);
    }

    result = sxnaudio_open(&stream, "nativetest");
    check(result == 0 && stream.mode == SXNAUDIO_MODE_DAEMON);
    if (result == 0 && stream.mode == SXNAUDIO_MODE_DAEMON) {
        struct savanxp_audiod_list_query query;

        /* Four records: with the declaration and the census request, six of
         * the eight a pipe holds. */
        for (index = 0; index < 4u; index += 1) {
            check(sxnaudio_write(&stream, frames, SAVANXP_AUDIOD_RECORD_MAX_FRAMES) ==
                (long)SAVANXP_AUDIOD_RECORD_MAX_FRAMES);
        }
        query.magic = SAVANXP_AUDIOD_CONTROL_MAGIC;
        query.version = SAVANXP_AUDIOD_CONTROL_VERSION;
        query.kind = SAVANXP_AUDIOD_LIST;
        check(sxnaudio_send_record(&stream, &query, (uint32_t)sizeof(query)) == 0);
        for (attempt = 0; attempt < 40 && !found; ++attempt) {
            struct savanxp_audiod_record record;
            const struct savanxp_audiod_list_reply* reply =
                (const struct savanxp_audiod_list_reply*)record.payload;

            (void)sxn_wait_one(stream.from_daemon, 100);
            while (sxn_read(stream.from_daemon, &record, sizeof(record)) == (long)sizeof(record)) {
                if (record.magic == SAVANXP_AUDIOD_RECORD_MAGIC && record.length >= sizeof(*reply) &&
                    reply->kind == SAVANXP_AUDIOD_REPLY && streq(reply->name, "nativetest") &&
                    reply->port >= SAVANXP_AUDIOD_NATIVE_FIRST_PORT &&
                    reply->port < SAVANXP_AUDIOD_NATIVE_FIRST_PORT + SAVANXP_AUDIOD_NATIVE_SLOTS) {
                    found = 1;
                }
            }
        }
        check(found);
        /* SUN control is a separate, non-playing connection. It returns one
         * atomic census and changes a generation-identified stream only. */
        {
            struct sxnaudio_control control = {-1, -1};
            struct savanxp_audiod_snapshot snapshot;
            struct savanxp_audiod_stream_state target;
            uint32_t old_volume = 0, old_muted = 0;
            int have_target = 0;
            check(sxnaudio_master_get(stream.device, &old_volume, &old_muted) == 0);
            check(sxnaudio_master_volume(stream.device, 50) == 0);
            check(sxnaudio_master_muted(stream.device, 1) == 0);
            {
                uint32_t volume = 0, muted = 0;
                check(sxnaudio_master_get(stream.device, &volume, &muted) == 0 && volume == 50 && muted == 1);
            }
            check(sxnaudio_master_volume(stream.device, old_volume) == 0);
            check(sxnaudio_master_muted(stream.device, old_muted) == 0);
            check(sxnaudio_control_open(&control) == 0);
            {
                struct savanxp_audio_endpoint output;
                struct savanxp_audio_endpoint_selection direct = {0};
                uint32_t case_index;
                check(sxnaudio_endpoint_get(stream.device,0,&output) == 0);
                direct.id = output.id; direct.direction = SAVANXP_AUDIO_PLAYBACK;
                check(sxn_device_control(stream.device,AUDIO_IOC_SELECT_ENDPOINT,
                    (unsigned long)(uintptr_t)&direct) == -SAVANXP_EBUSY);
                for (case_index = 0; case_index < 2; ++case_index) {
                    struct savanxp_audiod_endpoint_reply reply;
                    int acknowledged = 0;
                    check(sxnaudio_endpoint_select(&control,500+case_index,
                        case_index ? UINT64_MAX : output.id,SAVANXP_AUDIO_PLAYBACK) == 0);
                    for (attempt = 0; attempt < 50 && !acknowledged; ++attempt) {
                        long received;
                        (void)sxn_wait_one(pause,20);
                        received = sxnaudio_control_receive(&control,&snapshot,&reply);
                        if (received == 2 && reply.request_id == 500+case_index) {
                            check(reply.result == (case_index ? -SAVANXP_ENOENT : 0));
                            check(reply.endpoint_id == output.id && reply.direction == SAVANXP_AUDIO_PLAYBACK);
                            acknowledged = 1;
                        }
                    }
                    check(acknowledged);
                }
                check(sxn_log("nativetest: stable endpoints, ownership and acknowledged selection") == 0);
            }
            {
                struct savanxp_audiod_jack_policy_reply policy;
                struct savanxp_audiod_endpoint_reply route_reply;
                uint32_t operation;
                int supported = 0;
                check(sxnaudio_jack_policy_set(&control,599,2) == -SAVANXP_EINVAL);
                for (operation = 0; operation < 4; ++operation) {
                    int acknowledged = 0;
                    long sent = operation == 0 || operation == 3 ?
                        sxnaudio_jack_policy_query(&control,600+operation) :
                        sxnaudio_jack_policy_set(&control,600+operation,operation == 1);
                    check(sent == 0);
                    for (attempt = 0; attempt < 50 && !acknowledged; ++attempt) {
                        long received;
                        (void)sxn_wait_one(pause,20);
                        received = sxnaudio_control_receive_ex(&control,&snapshot,&route_reply,&policy);
                        if (received == 3 && policy.request_id == 600+operation) {
                            if (!operation) supported = (int)policy.supported;
                            check(policy.supported == (uint32_t)supported);
                            check(policy.result == (operation == 1 && !supported ? -SAVANXP_ENODEV : 0));
                            check(policy.enabled == (uint32_t)(operation == 1 && supported));
                            acknowledged = 1;
                        }
                    }
                    check(acknowledged);
                }
                check(sxn_log("nativetest: jack policy query, enable support and explicit manual mode") == 0);
            }

            for (attempt = 0; attempt < 30 && !have_target; ++attempt) {
                uint32_t row;
                (void)sxnaudio_write(&stream, frames, 480);
                check(sxnaudio_control_query(&control, (uint32_t)attempt + 1) == 0);
                (void)sxn_wait_one(pause, 20);
                while (sxnaudio_control_read(&control, &snapshot) == 1) {
                    for (row = 0; row < snapshot.count; ++row) {
                        if (streq(snapshot.streams[row].name, "nativetest") &&
                            snapshot.streams[row].peak_left > 0) {
                            target = snapshot.streams[row]; have_target = 1;
                        }
                    }
                    check(snapshot.count == 1); /* the monitor is not a playback client */
                }
            }
            check(have_target);
            if (have_target) {
                int changed = 0;
                check(sxnaudio_control_change(&control, &target, SAVANXP_AUDIOD_STREAM_VOLUME, 25) == 0);
                check(sxnaudio_control_change(&control, &target, SAVANXP_AUDIOD_STREAM_MUTED, 1) == 0);
                for (attempt = 0; attempt < 20 && !changed; ++attempt) {
                    uint32_t row;
                    (void)sxnaudio_write(&stream, frames, 480);
                    check(sxnaudio_control_query(&control, 100 + attempt) == 0);
                    (void)sxn_wait_one(pause, 20);
                    while (sxnaudio_control_read(&control, &snapshot) == 1) {
                        for (row = 0; row < snapshot.count; ++row) {
                            const struct savanxp_audiod_stream_state* state = &snapshot.streams[row];
                            if (state->id == target.id && state->volume == 25 && state->muted == 1 &&
                                state->peak_left == 0 && state->peak_right == 0) changed = 1;
                        }
                    }
                }
                check(changed);
                check(sxnaudio_control_change(&control, &target, SAVANXP_AUDIOD_STREAM_VOLUME, 100) == 0);
                check(sxnaudio_control_change(&control, &target, SAVANXP_AUDIOD_STREAM_MUTED, 0) == 0);
            }
            sxnaudio_control_close(&control);
            check(sxn_log("nativetest: audio control snapshot, gain, mute and meters") == 0);
        }
        /* A full pipe ends a write early instead of blocking or dropping. */
        for (index = 0; index < 16u; index += 1) {
            result = sxnaudio_write(&stream, frames, 480u);
            check(result >= 0 && result <= 480);
        }
    }
    sxnaudio_close(&stream);
    check(sxn_terminate(pid, 0) == 0);
    check(sxn_waitpid(pid, &status) == pid);
    (void)sxn_close(pause);
    check(sxn_log("nativetest: audio: direct and through audiod's port") == 0);
}

static struct sxnaudio_stream* g_capture_test_output;
static struct sxnaudio_capture* g_capture_test_secondary;
static uint64_t g_capture_test_secondary_frames;
static uint32_t g_capture_test_phase;
static uint64_t g_capture_test_last_pump, g_capture_test_max_gap;
static void capture_test_pump(void) {
    int16_t pcm[480*2];
    uint32_t i, due;
    long written;
    if (!g_capture_test_output) return;
    {
        uint64_t now = sxn_clock_ns();
        if (g_capture_test_last_pump && now-g_capture_test_last_pump > g_capture_test_max_gap)
            g_capture_test_max_gap = now-g_capture_test_last_pump;
        g_capture_test_last_pump = now;
    }
    due = sxnaudio_frames_due(g_capture_test_output,30);
    if (due > 480) due = 480;
    for (i = 0; i < due; ++i) pcm[i*2] = pcm[i*2+1] =
        (g_capture_test_phase+i)%109 < 54 ? 3000 : -3000;
    written = sxnaudio_write(g_capture_test_output,pcm,due);
    if (written >= 0) g_capture_test_phase += (uint32_t)written;
    else check(0);
}
static int capture_test_samples(struct sxnaudio_capture* stream, int left, int right, int verify) {
    int16_t pcm[480*2];
    uint32_t total = 0, crossings_left = 0, crossings_right = 0;
    int max_left = 0, max_right = 0;
    int previous_left = 0, previous_right = 0, have_previous = 0, valid = 1;
    uint64_t deadline = sxn_clock_ns()+2000000000ull;
    long pause = sxn_event_create(0);
    const uint32_t target = verify ? 4800 : 9600;
    while (total < target && sxn_clock_ns() < deadline) {
        uint32_t want = target-total, i;
        long got;
        capture_test_pump();
        if (g_capture_test_secondary) {
            int16_t other[2048];
            long extra;
            while ((extra = sxnaudio_capture_read(g_capture_test_secondary,other,1024)) > 0) {
                uint32_t j;
                g_capture_test_secondary_frames += (uint32_t)extra;
                for (j = 0; verify && j < (uint32_t)extra; ++j) {
                    int l = other[j*2], r = other[j*2+1];
                    int al = l < 0 ? -l : l, ar = r < 0 ? -r : r;
                    if (al < left-2 || al > left+2 || ar < right-2 || ar > right+2) valid = 0;
                }
            }
            if (extra < 0) { check(0); (void)sxn_close(pause); return 0; }
        }
        if (want > 137) want = 137; /* exercises buffered partial-period reads */
        got = sxnaudio_capture_read(stream,pcm,want);
        if (got < 0) { check(0); (void)sxn_close(pause); return 0; }
        if (!got) { (void)sxn_wait_one(pause,2); continue; }
        for (i = 0; i < (uint32_t)got; ++i) {
            int l = pcm[i*2], r = pcm[i*2+1];
            int al = l < 0 ? -l : l, ar = r < 0 ? -r : r;
            if (al > max_left) max_left = al;
            if (ar > max_right) max_right = ar;
            if (verify && (al < left-2 || al > left+2 || ar < right-2 || ar > right+2)) valid = 0;
            if (have_previous && previous_left <= 0 && l > 0) ++crossings_left;
            if (have_previous && previous_right <= 0 && r > 0) ++crossings_right;
            previous_left = l; previous_right = r; have_previous = 1;
        }
        total += (uint32_t)got;
    }
    (void)sxn_close(pause);
    check(total == target);
    if (verify) {
        char diagnostic[160];
        (void)snprintf(diagnostic,sizeof(diagnostic),"nativetest: capture expected=%d/%d peak=%d/%d crossings=%u/%u frames=%u",
            left,right,max_left,max_right,crossings_left,crossings_right,total);
        (void)sxn_log(diagnostic);
        check(valid);
        if (left && right) check(crossings_left >= 39 && crossings_left <= 41 &&
                                 crossings_right >= 59 && crossings_right <= 61);
        else check(!crossings_left && !crossings_right);
    }
    return total == target && valid;
}
static void audio_busy_child(int capture) {
    long fd = sxn_open_device("audio0",capture ? SXN_DEVICE_READ : SXN_DEVICE_WRITE);
    int16_t pcm[2] = {1000,1000};
    long result = fd < 0 ? fd : capture ? sxn_device_control(fd,AUDIO_IOC_CAPTURE_START,0) : sxn_write(fd,pcm,sizeof(pcm));
    if (fd >= 0) (void)sxn_close(fd);
    sxn_exit(result == -SAVANXP_EBUSY ? 0 : 1);
}
static void capture_test_contender(int capture) {
    const char* argv[] = {"/bin/nativetest.sxe",capture ? "audio-capture-busy" : "audio-writer-busy",0};
    int status = -1;
    long pid = sxn_spawn(argv[0],(char* const*)argv,2);
    check(pid > 0);
    if (pid > 0) check(sxn_waitpid(pid,&status) == pid && status == 0);
}
static void capture_test_protocol(void) {
    int32_t pipe[2];
    struct sxnaudio_capture stream = {0};
    struct savanxp_audiod_record record = {0};
    struct savanxp_audiod_capture_pcm* header = (struct savanxp_audiod_capture_pcm*)record.payload;
    int16_t* data = (int16_t*)(record.payload+sizeof(*header));
    int16_t out[6];
    check(sxn_pipe_create(pipe) == 0);
    check(sxn_set_nonblocking(pipe[0],1) == 0);
    stream.mode = SXNAUDIO_MODE_DAEMON; stream.from_daemon = pipe[0];
    record.magic = SAVANXP_AUDIOD_RECORD_MAGIC; record.length = sizeof(*header)+8;
    header->magic = SAVANXP_AUDIOD_CONTROL_MAGIC; header->version = SAVANXP_AUDIOD_CONTROL_VERSION;
    header->kind = SAVANXP_AUDIOD_CAPTURE_PCM; header->frames = 2;
    data[0] = 100; data[1] = -200; data[2] = 300; data[3] = -400;
    check(sxn_write(pipe[1],&record,sizeof(record)) == sizeof(record));
    check(sxnaudio_capture_read(&stream,out,1) == 1 && out[0] == 100 && out[1] == -200);
    check(sxnaudio_capture_read(&stream,out,1) == 1 && out[0] == 300 && out[1] == -400);
    header->frame_index = 5;
    check(sxn_write(pipe[1],&record,sizeof(record)) == sizeof(record));
    check(sxnaudio_capture_read(&stream,out,2) == 2 && stream.dropped_frames == 3);
    header->frame_index = 1;
    check(sxn_write(pipe[1],&record,sizeof(record)) == sizeof(record));
    check(sxnaudio_capture_read(&stream,out,2) == -SAVANXP_EIO);
    stream.error = 0; /* Each malformed record must be parsed independently. */
    header->frame_index = UINT64_MAX;
    check(sxn_write(pipe[1],&record,sizeof(record)) == sizeof(record));
    check(sxnaudio_capture_read(&stream,out,2) == -SAVANXP_EIO);
    stream.error = 0;
    header->frame_index = 7; record.length = sizeof(*header);
    check(sxn_write(pipe[1],&record,sizeof(record)) == sizeof(record));
    check(sxnaudio_capture_read(&stream,out,2) == -SAVANXP_EIO);
    stream.error = 0;
    header->frame_index = 7; record.length = sizeof(*header)+8;
    check(sxn_write(pipe[1],&record,sizeof(record)) == sizeof(record));
    {
        struct savanxp_audiod_capture_reply* error = (struct savanxp_audiod_capture_reply*)record.payload;
        header->kind = SAVANXP_AUDIOD_CAPTURE_REPLY;
        record.length = sizeof(*error); error->request_id = 0;
        error->result = -SAVANXP_EAGAIN; error->reserved = 0;
        check(sxn_write(pipe[1],&record,sizeof(record)) == sizeof(record));
        check(sxnaudio_capture_read(&stream,out,3) == 2);
        check(sxnaudio_capture_read(&stream,out,1) == -SAVANXP_EAGAIN);
        check(sxnaudio_capture_read(&stream,out,1) == -SAVANXP_EAGAIN);
    }
    stream.error = 0;
    (void)sxn_close(pipe[1]);
    check(sxnaudio_capture_read(&stream,out,2) == -SAVANXP_EIO);
    (void)sxn_close(pipe[0]);
    check(sxn_log("nativetest: capture framing, partial reads, loss indices and malformed/EOF replies") == 0);
}

static void test_capture(void) {
    struct sxnaudio_capture input;
    struct sxnaudio_stream output;
    struct savanxp_audio_capture_state state;
    long control = sxn_open_device("audio0",SXN_DEVICE_READ|SXN_DEVICE_WRITE);
    long result;
    capture_test_protocol();
    check(control >= 0);
    if (control < 0) return;
    result = sxnaudio_capture_open(&input);
    check(result == 0); /* The fixture scenario must fail, never skip capture. */
    if (result < 0) { (void)sxn_close(control); return; }
    check(input.mode == SXNAUDIO_MODE_DIRECT && input.rate_hz == 48000);
    check(sxnaudio_capture_volume(input.device,50) == -SAVANXP_EACCES);
    check(sxnaudio_capture_volume(control,101) == -SAVANXP_EINVAL);
    check(sxnaudio_capture_volume(control,100) == 0 && sxnaudio_capture_muted(control,0) == 0);
    check(sxnaudio_master_volume(control,100) == 0 && sxnaudio_master_muted(control,0) == 0);
    check(sxnaudio_open(&output,"Duplex test") == 0 && output.mode == SXNAUDIO_MODE_DIRECT);
    g_capture_test_output = &output; g_capture_test_phase = 0;
    capture_test_pump();
    capture_test_contender(1);
    check(capture_test_samples(&input,0,0,0));
    check(capture_test_samples(&input,12000,6000,1));
    check(sxnaudio_capture_state(control,&state) == 0 && state.active && !state.muted && state.volume == 100 &&
          state.peak_left >= 11998 && state.peak_left <= 12002 && state.peak_right >= 5998 && state.peak_right <= 6002);
    check(sxnaudio_capture_volume(control,50) == 0);
    check(capture_test_samples(&input,0,0,0));
    check(capture_test_samples(&input,6000,3000,1));
    check(sxnaudio_capture_muted(control,1) == 0);
    check(capture_test_samples(&input,0,0,0));
    check(capture_test_samples(&input,0,0,1));
    check(sxnaudio_capture_state(control,&state) == 0 && state.muted && state.volume == 50 && !state.peak_left && !state.peak_right);
    check(sxnaudio_capture_muted(control,0) == 0);
    check(capture_test_samples(&input,0,0,0));
    check(capture_test_samples(&input,6000,3000,1));
    check(sxnaudio_capture_volume(control,100) == 0);
    {
        char diagnostic[160];
        (void)snprintf(diagnostic,sizeof(diagnostic),"nativetest: duplex sent=%llu source=%u elapsed=%llu max-pump-gap-us=%llu",
            (unsigned long long)output.frames_sent,g_capture_test_phase,
            (unsigned long long)((sxn_clock_ns()-output.started_ns)*48000/1000000000),
            (unsigned long long)(g_capture_test_max_gap/1000));
        (void)sxn_log(diagnostic);
    }
    g_capture_test_output = 0;
    sxnaudio_close(&output);
    check(sxnaudio_capture_state(control,&state) == 0 && state.active);
    check(capture_test_samples(&input,0,0,0));
    check(capture_test_samples(&input,12000,6000,1));
    check(sxnaudio_open(&output,"Duplex test") == 0);
    { int16_t pcm[2] = {1000,1000}; check(sxnaudio_write(&output,pcm,1) == 1); }
    sxnaudio_capture_close(&input);
    capture_test_contender(0);
    sxnaudio_close(&output);
    check(sxnaudio_capture_state(control,&state) == 0 && !state.active);
    {
        struct sxnaudio_capture second;
        char* const argv[] = {(char*)"/bin/audiod",(char*)"--no-preferences",0};
        long daemon = sxn_spawn(argv[0],argv,2);
        long pause = sxn_event_create(0);
        int attempt, status = -1;
        check(daemon > 0);
        for (attempt = 0; attempt < 100; ++attempt) {
            int32_t handles[2];
            if (sxn_port_connect(SAVANXP_AUDIOD_PORT_NAME,handles) == 0) {
                (void)sxn_close(handles[0]); (void)sxn_close(handles[1]); break;
            }
            (void)sxn_wait_one(pause,10);
        }
        check(attempt < 100);
        check(sxnaudio_capture_open(&input) == 0 && input.mode == SXNAUDIO_MODE_DAEMON);
        check(sxnaudio_capture_open(&second) == 0 && second.mode == SXNAUDIO_MODE_DAEMON);
        check(sxnaudio_open(&output,"Duplex test") == 0 && output.mode == SXNAUDIO_MODE_DAEMON);
        check(sxnaudio_capture_volume(control,100) == 0 && sxnaudio_capture_muted(control,0) == 0);
        g_capture_test_output = &output; g_capture_test_phase = 0;
        g_capture_test_last_pump = g_capture_test_max_gap = 0;
        // A slow subscriber must neither stall the input nor conceal its loss.
        g_capture_test_secondary = 0;
        check(capture_test_samples(&input,0,0,0));
        g_capture_test_secondary = &second; g_capture_test_secondary_frames = 0;
        check(capture_test_samples(&input,0,0,0));
        check(capture_test_samples(&input,12000,6000,1));
        check(!input.pending && !input.dropped_frames && second.dropped_frames > 0 && g_capture_test_secondary_frames > 0);
        capture_test_contender(1);
        check(sxnaudio_capture_volume(control,50) == 0);
        check(capture_test_samples(&input,0,0,0)); check(capture_test_samples(&input,6000,3000,1));
        check(sxnaudio_capture_muted(control,1) == 0);
        check(capture_test_samples(&input,0,0,0)); check(capture_test_samples(&input,0,0,1));
        check(sxnaudio_capture_muted(control,0) == 0);
        check(capture_test_samples(&input,0,0,0)); check(capture_test_samples(&input,6000,3000,1));
        check(!input.dropped_frames);
        g_capture_test_secondary = 0;
        sxnaudio_capture_close(&input);
        check(capture_test_samples(&second,0,0,0)); check(capture_test_samples(&second,6000,3000,1));
        check(sxnaudio_capture_state(control,&state) == 0 && state.active);
        sxnaudio_capture_close(&second);
        for (attempt = 0; attempt < 100; ++attempt) {
            capture_test_pump();
            if (sxnaudio_capture_state(control,&state) == 0 && !state.active) break;
            (void)sxn_wait_one(pause,10);
        }
        check(attempt < 100);
        g_capture_test_output = 0; sxnaudio_close(&output);
        check(sxnaudio_capture_volume(control,100) == 0);
        check(sxn_terminate(daemon,0) == 0);
        check(sxn_waitpid(daemon,&status) == daemon && status == 0);
        (void)sxn_close(pause);
        check(sxn_log("nativetest: daemon capture fanout, counted loss, independent readers and duplex") == 0);
    }
    (void)sxn_close(control);
    check(sxn_log("nativetest: exact capture PCM, independent input gain/mute, duplex and close ownership") == 0);
}

static uint64_t audio_selected(long control, uint32_t direction) {
    struct savanxp_audio_endpoint e;
    uint64_t id = 0;
    uint32_t i;
    for (i = 0; sxnaudio_endpoint_get(control,i,&e) == 0; ++i) {
        if ((e.flags&(direction|SAVANXP_AUDIO_SELECTED)) != (direction|SAVANXP_AUDIO_SELECTED)) continue;
        check(!id); id = e.id;
    }
    return id;
}
static long audio_select_direct(long control, uint64_t id, uint32_t direction) {
    struct savanxp_audio_endpoint_selection selection = {id,direction,0};
    return sxn_device_control(control,AUDIO_IOC_SELECT_ENDPOINT,(unsigned long)(uintptr_t)&selection);
}
static int audio_select_remote(struct sxnaudio_control* control, uint64_t id, uint32_t direction, uint32_t request) {
    struct savanxp_audiod_snapshot snapshot;
    struct savanxp_audiod_endpoint_reply reply;
    struct savanxp_audiod_jack_policy_reply policy;
    long pause = sxn_event_create(0);
    uint64_t deadline = sxn_clock_ns()+1000000000ull;
    int done = 0;
    check(sxnaudio_endpoint_select(control,request,id,direction) == 0);
    while (!done && sxn_clock_ns() < deadline) {
        long result;
        capture_test_pump();
        result = sxnaudio_control_receive_ex(control,&snapshot,&reply,&policy);
        if (result == 2 && reply.request_id == request) {
            check(reply.result == 0 && reply.endpoint_id == id && reply.direction == direction); done = 1;
        } else if (result < 0) { check(0); break; }
        (void)sxn_wait_one(pause,2);
    }
    (void)sxn_close(pause); check(done); return done;
}
static void audio_device_samples(struct sxnaudio_capture* input, int left, int right, unsigned left_hz, unsigned right_hz) {
    int16_t samples[274];
    uint32_t total = 0, crossings[2] = {0,0};
    int previous[2] = {0,0}, valid = 1;
    uint64_t deadline = sxn_clock_ns()+4000000000ull;
    long pause = sxn_event_create(0);
    while (total < 96000 && sxn_clock_ns() < deadline) {
        uint32_t want = 96000-total, i;
        long got;
        capture_test_pump();
        if (want > 137) want = 137;
        got = sxnaudio_capture_read(input,samples,want);
        if (got < 0) { check(0); break; }
        if (!got) { (void)sxn_wait_one(pause,2); continue; }
        for (i = 0; i < (uint32_t)got; ++i) {
            unsigned channel;
            for (channel = 0; channel < 2; ++channel) {
                int sample = samples[i*2+channel], magnitude = sample < 0 ? -sample : sample;
                int expected = channel ? right : left;
                if (magnitude < expected-2 || magnitude > expected+2) valid = 0;
                if (total+i && previous[channel] <= 0 && sample > 0) ++crossings[channel];
                previous[channel] = sample;
            }
        }
        total += (uint32_t)got;
    }
    (void)sxn_close(pause);
    {
        char diagnostic[160];
        (void)snprintf(diagnostic,sizeof(diagnostic),"nativetest: device input expected=%d/%d %u/%uHz observed=%u/%u frames=%u valid=%d",
            left,right,left_hz,right_hz,crossings[0],crossings[1],total,valid);
        (void)sxn_log(diagnostic);
    }
    check(total == 96000 && valid);
    check(crossings[0]+1 >= left_hz*2 && crossings[0] <= left_hz*2+1 &&
          crossings[1]+1 >= right_hz*2 && crossings[1] <= right_hz*2+1);
}
static void test_audio_devices(void) {
    struct savanxp_audio_endpoint e;
    uint64_t outputs[3] = {0}, inputs[3] = {0}, devices[3] = {0};
    unsigned count = 0, i, j, have_ac97 = 0, have_virtio = 0;
    long control = sxn_open_device("audio0",SXN_DEVICE_READ|SXN_DEVICE_WRITE);
    struct sxnaudio_stream output;
    struct sxnaudio_capture input;
    check(control >= 0);
    if (control < 0) return;
    for (i = 0; sxnaudio_endpoint_get(control,i,&e) == 0; ++i) {
        if (e.backend == SAVANXP_AUDIO_BACKEND_AC97) ++have_ac97;
        if (e.backend == SAVANXP_AUDIO_BACKEND_VIRTIO) ++have_virtio;
        if (e.backend != SAVANXP_AUDIO_BACKEND_HDA) continue;
        for (j = 0; j < count && devices[j] != e.device_id; ++j) {}
        if (j == count) { check(count < 3); if (count == 3) break; devices[count++] = e.device_id; }
        if (e.flags&SAVANXP_AUDIO_PLAYBACK) outputs[j] = e.id;
        if (e.flags&SAVANXP_AUDIO_CAPTURE) inputs[j] = e.id;
    }
    check(count == 3 && have_ac97 == 1 && have_virtio == 2);
    if (count != 3) { (void)sxn_close(control); return; }
    check((devices[0]>>16) == (devices[1]>>16) && ((devices[1]>>8)&255) == 2 && (devices[0]>>16) != (devices[2]>>16));
    for (i = 0; i < count; ++i) check(outputs[i] && inputs[i]);
    check(audio_select_direct(control,outputs[0],SAVANXP_AUDIO_PLAYBACK) == 0);
    check(audio_select_direct(control,inputs[1],SAVANXP_AUDIO_CAPTURE) == 0);
    check(sxnaudio_master_volume(control,100) == 0 && sxnaudio_master_muted(control,0) == 0);
    check(sxnaudio_capture_volume(control,100) == 0 && sxnaudio_capture_muted(control,0) == 0);
    check(sxnaudio_open(&output,"Device isolation test") == 0 && output.mode == SXNAUDIO_MODE_DIRECT);
    check(sxnaudio_capture_open(&input) == 0 && input.mode == SXNAUDIO_MODE_DIRECT);
    g_capture_test_output = &output; g_capture_test_phase = 0;
    check(capture_test_samples(&input,0,0,0));
    audio_device_samples(&input,8000,4000,500,1000);
    check(audio_selected(control,SAVANXP_AUDIO_PLAYBACK) == outputs[0] && audio_selected(control,SAVANXP_AUDIO_CAPTURE) == inputs[1]);
    check(audio_select_direct(control,outputs[1],SAVANXP_AUDIO_PLAYBACK) == 0);
    audio_device_samples(&input,8000,4000,500,1000);
    check(audio_select_direct(control,outputs[2],SAVANXP_AUDIO_PLAYBACK) == 0);
    audio_device_samples(&input,8000,4000,500,1000);
    check(audio_select_direct(control,inputs[2],SAVANXP_AUDIO_CAPTURE) == 0);
    sxnaudio_capture_close(&input);
    check(sxnaudio_capture_open(&input) == 0);
    check(capture_test_samples(&input,0,0,0));
    audio_device_samples(&input,10000,2000,300,750);
    check(audio_select_direct(control,0xdeadbeef,SAVANXP_AUDIO_CAPTURE) == -SAVANXP_ENOENT);
    check(audio_selected(control,SAVANXP_AUDIO_CAPTURE) == inputs[2] && audio_selected(control,SAVANXP_AUDIO_PLAYBACK) == outputs[2]);
    audio_device_samples(&input,10000,2000,300,750);
    check(audio_select_direct(control,inputs[0],SAVANXP_AUDIO_CAPTURE) == 0);
    sxnaudio_capture_close(&input);
    check(sxnaudio_capture_open(&input) == 0);
    check(capture_test_samples(&input,0,0,0));
    audio_device_samples(&input,12000,6000,400,600);
    sxnaudio_capture_close(&input);
    g_capture_test_output = 0; sxnaudio_close(&output);
    {
        char* const argv[] = {(char*)"/bin/audiod",(char*)"--no-preferences",0};
        long daemon = sxn_spawn(argv[0],argv,2), pause = sxn_event_create(0);
        struct sxnaudio_control remote = {-1,-1};
        int attempt, status = -1;
        long result = 0;
        check(daemon > 0);
        for (attempt = 0; attempt < 100 && sxnaudio_control_open(&remote) < 0; ++attempt)
            (void)sxn_wait_one(pause,10);
        check(attempt < 100);
        check(sxnaudio_open(&output,"Device isolation test") == 0 && output.mode == SXNAUDIO_MODE_DAEMON);
        check(sxnaudio_capture_open(&input) == 0 && input.mode == SXNAUDIO_MODE_DAEMON);
        g_capture_test_output = &output; g_capture_test_phase = 0;
        check(capture_test_samples(&input,0,0,0));
        audio_device_samples(&input,12000,6000,400,600);
        (void)audio_select_remote(&remote,inputs[1],SAVANXP_AUDIO_CAPTURE,2000);
        for (attempt = 0; attempt < 500 && result >= 0; ++attempt) {
            int16_t samples[274];
            capture_test_pump(); result = sxnaudio_capture_read(&input,samples,137);
            if (!result) (void)sxn_wait_one(pause,2);
        }
        check(result == -SAVANXP_EAGAIN);
        sxnaudio_capture_close(&input);
        check(sxnaudio_capture_open(&input) == 0);
        check(capture_test_samples(&input,0,0,0));
        audio_device_samples(&input,8000,4000,500,1000);
        check(audio_selected(control,SAVANXP_AUDIO_PLAYBACK) == outputs[2]);
        (void)audio_select_remote(&remote,outputs[0],SAVANXP_AUDIO_PLAYBACK,2001);
        audio_device_samples(&input,8000,4000,500,1000);
        check(!input.dropped_frames && audio_selected(control,SAVANXP_AUDIO_CAPTURE) == inputs[1]);
        sxnaudio_capture_close(&input);
        g_capture_test_output = 0; sxnaudio_close(&output); sxnaudio_control_close(&remote);
        check(sxn_terminate(daemon,0) == 0 && sxn_waitpid(daemon,&status) == daemon && status == 0);
        (void)sxn_close(pause);
        check(sxn_log("nativetest: acknowledged default changes, explicit old-source error, reopened native capture and uninterrupted opposite direction") == 0);
    }
    (void)sxn_close(control);
    check(sxn_log("nativetest: three analog codecs on two controllers, AC97/virtio inventory and independent live defaults") == 0);
}

static long preferences_start_daemon(struct sxnaudio_control* control) {
    char* const argv[] = {(char*)"/bin/audiod",0};
    long pid = sxn_spawn(argv[0],argv,1), pause = sxn_event_create(0);
    unsigned attempt;
    check(pid > 0);
    for (attempt = 0; attempt < 200 && sxnaudio_control_open(control) < 0; ++attempt)
        (void)sxn_wait_one(pause,10);
    check(attempt < 200); (void)sxn_close(pause); return pid;
}
static struct savanxp_audiod_stream_state preferences_stream(struct sxnaudio_control* control, uint32_t id, const char* name) {
    struct savanxp_audiod_stream_state state = {0};
    struct savanxp_audiod_snapshot snapshot;
    struct savanxp_audiod_endpoint_reply endpoint;
    struct savanxp_audiod_jack_policy_reply policy;
    struct savanxp_audiod_preference_reply preferences;
    long pause = sxn_event_create(0);
    uint64_t deadline = sxn_clock_ns()+1000000000ull;
    check(sxnaudio_control_query(control,id) == 0);
    while (!state.id && sxn_clock_ns() < deadline) {
        long result;
        uint32_t i;
        capture_test_pump();
        result = sxnaudio_control_receive_full(control,&snapshot,&endpoint,&policy,&preferences);
        if (result == 1 && snapshot.request_id == id) {
            for (i = 0; i < snapshot.count; ++i) if (streq(snapshot.streams[i].name,name)) state = snapshot.streams[i];
            if (state.id) break;
            // DECLARE arrives on another connection. Wait for registration
            // rather than imposing ordering between the two client pipes.
            check(sxnaudio_control_query(control,id) == 0);
        }
        if (result < 0) { check(0); break; }
        (void)sxn_wait_one(pause,2);
    }
    check(state.id != 0); (void)sxn_close(pause); return state;
}
static void preferences_flush(struct sxnaudio_control* control, uint32_t id) {
    struct savanxp_audiod_snapshot snapshot;
    struct savanxp_audiod_endpoint_reply endpoint;
    struct savanxp_audiod_jack_policy_reply policy;
    struct savanxp_audiod_preference_reply preferences;
    long pause = sxn_event_create(0);
    uint64_t deadline = sxn_clock_ns()+5000000000ull;
    int done = 0;
    check(sxnaudio_preferences_flush(control,id) == 0);
    while (!done && sxn_clock_ns() < deadline) {
        long result;
        capture_test_pump();
        result = sxnaudio_control_receive_full(control,&snapshot,&endpoint,&policy,&preferences);
        if (result == 4 && preferences.request_id == id) {
            char log[128];
            (void)snprintf(log,sizeof(log),"nativetest: preference flush result=%d flags=%u generation=%llu saved=%llu error=%d",
                preferences.result,preferences.flags,(unsigned long long)preferences.generation,
                (unsigned long long)preferences.saved_generation,preferences.last_error);
            (void)sxn_log(log);
            check(!preferences.result && !preferences.last_error &&
                  preferences.flags == SAVANXP_AUDIOD_PREFERENCE_ENABLED && preferences.generation == preferences.saved_generation);
            done = 1;
        } else if (result < 0) { check(0); break; }
        (void)sxn_wait_one(pause,2);
    }
    check(done); (void)sxn_close(pause);
}
static void test_audio_preferences(void) {
    const char* role = "Preference restore test";
    char* const source_argv[] = {(char*)"/bin/nativetest.sxe",(char*)"audio-source",(char*)"Preference restore test",0};
    char* const posix_argv[] = {(char*)"/bin/audiotest",(char*)"--stream",0};
    struct sxnaudio_control remote = {-1,-1};
    struct sxnaudio_stream dummy[2];
    struct sxnaudio_capture capture;
    struct savanxp_audiod_stream_state before, after;
    struct savanxp_audiod_stream_state posix_before, posix_after;
    struct savanxp_audio_capture_state input;
    struct savanxp_audio_endpoint endpoint;
    uint64_t outputs[3] = {0}, inputs[3] = {0}, initial_output = 0, initial_input = 0;
    uint32_t i, out = 0, in = 0;
    int status = -1;
    long control = sxn_open_device("audio0",SXN_DEVICE_READ|SXN_DEVICE_WRITE), daemon, source, posix_source;
    check(control >= 0); if (control < 0) return;
    for (i = 0; sxnaudio_endpoint_get(control,i,&endpoint) == 0; ++i) {
        if (endpoint.flags&SAVANXP_AUDIO_SELECTED) {
            if (endpoint.flags&SAVANXP_AUDIO_PLAYBACK) initial_output = endpoint.id;
            if (endpoint.flags&SAVANXP_AUDIO_CAPTURE) initial_input = endpoint.id;
        }
        if (endpoint.backend != SAVANXP_AUDIO_BACKEND_HDA) continue;
        if ((endpoint.flags&SAVANXP_AUDIO_PLAYBACK) && out < 3) outputs[out++] = endpoint.id;
        if ((endpoint.flags&SAVANXP_AUDIO_CAPTURE) && in < 3) inputs[in++] = endpoint.id;
    }
    check(out == 3 && in == 3 && initial_output && initial_input);
    if (out != 3 || in != 3) { (void)sxn_close(control); return; }
    daemon = preferences_start_daemon(&remote);
    source = sxn_spawn(source_argv[0],source_argv,3); check(source > 0);
    before = preferences_stream(&remote,3000,role);
    posix_source = sxn_spawn(posix_argv[0],posix_argv,2); check(posix_source > 0);
    posix_before = preferences_stream(&remote,3010,"audiotest");
    check(posix_before.port >= SAVANXP_AUDIOD_NATIVE_FIRST_PORT &&
          posix_before.port < SAVANXP_AUDIOD_NATIVE_FIRST_PORT+SAVANXP_AUDIOD_NATIVE_SLOTS);
    check(sxnaudio_control_change(&remote,&posix_before,SAVANXP_AUDIOD_STREAM_VOLUME,37) == 0);
    check(sxnaudio_control_change(&remote,&posix_before,SAVANXP_AUDIOD_STREAM_MUTED,1) == 0);
    check(before.volume == 100 && !before.muted);
    check(sxnaudio_control_change(&remote,&before,SAVANXP_AUDIOD_STREAM_VOLUME,43) == 0);
    check(sxnaudio_control_change(&remote,&before,SAVANXP_AUDIOD_STREAM_MUTED,1) == 0);
    (void)audio_select_remote(&remote,outputs[1],SAVANXP_AUDIO_PLAYBACK,3001);
    (void)audio_select_remote(&remote,inputs[2],SAVANXP_AUDIO_CAPTURE,3002);
    check(sxnaudio_master_volume(control,62) == 0 && sxnaudio_master_muted(control,1) == 0);
    check(sxnaudio_capture_volume(control,50) == 0 && sxnaudio_capture_muted(control,1) == 0);
    preferences_flush(&remote,3003);
    check(sxn_terminate(posix_source,0) == 0 && sxn_waitpid(posix_source,&status) == posix_source && !status);
    check(sxn_terminate(source,0) == 0 && sxn_waitpid(source,&status) == source && !status);
    sxnaudio_control_close(&remote);
    check(sxn_terminate(daemon,0) == 0 && sxn_waitpid(daemon,&status) == daemon && !status);
    check(sxnaudio_master_volume(control,100) == 0 && sxnaudio_master_muted(control,0) == 0);
    check(sxnaudio_capture_volume(control,100) == 0 && sxnaudio_capture_muted(control,0) == 0);
    check(audio_select_direct(control,initial_output,SAVANXP_AUDIO_PLAYBACK) == 0);
    check(audio_select_direct(control,initial_input,SAVANXP_AUDIO_CAPTURE) == 0);
    daemon = preferences_start_daemon(&remote);
    // Occupy different ports/registration ids before reconnecting the same role.
    sxnaudio_control_close(&remote);
    for (i = 0; i < 2; ++i) {
        int16_t silence[2] = {0,0};
        check(sxnaudio_open(&dummy[i],i ? "Preference warmup B" : "Preference warmup A") == 0);
        check(sxnaudio_write(&dummy[i],silence,1) == 1);
    }
    check(sxnaudio_control_open(&remote) == 0);
    // A dedicated source keeps producing while this process changes controls
    // and waits for recording/flush acknowledgements.
    source = sxn_spawn(source_argv[0],source_argv,3); check(source > 0);
    after = preferences_stream(&remote,3004,role);
    check(after.id != before.id && after.port != before.port && after.volume == 43 && after.muted == 1);
    posix_source = sxn_spawn(posix_argv[0],posix_argv,2); check(posix_source > 0);
    posix_after = preferences_stream(&remote,3011,"audiotest");
    check(posix_after.id != posix_before.id && posix_after.port != posix_before.port &&
          posix_after.volume == 37 && posix_after.muted == 1 &&
          posix_after.port < SAVANXP_AUDIOD_NATIVE_FIRST_PORT+SAVANXP_AUDIOD_NATIVE_SLOTS);
    check(sxn_terminate(posix_source,0) == 0 && sxn_waitpid(posix_source,&status) == posix_source && !status);
    {
        uint32_t volume, muted;
        check(sxnaudio_master_get(control,&volume,&muted) == 0 && volume == 62 && muted == 1);
    }
    check(sxnaudio_capture_state(control,&input) == 0 && input.volume == 50 && input.muted == 1);
    check(audio_selected(control,SAVANXP_AUDIO_PLAYBACK) == outputs[1] && audio_selected(control,SAVANXP_AUDIO_CAPTURE) == inputs[2]);
    for (i = 0; i < 2; ++i) sxnaudio_close(&dummy[i]);
    check(sxnaudio_capture_open(&capture) == 0 && capture.mode == SXNAUDIO_MODE_DAEMON);
    // Complete the capture/DMA startup while output is still muted. The
    // measured continuous interval then covers live capture and autosaves,
    // without including queued silence from process loading/stream setup.
    check(capture_test_samples(&capture,0,0,0));
    check(sxnaudio_control_change(&remote,&after,SAVANXP_AUDIOD_STREAM_MUTED,0) == 0);
    check(sxnaudio_master_volume(control,100) == 0 && sxnaudio_master_muted(control,0) == 0);
    check(capture_test_samples(&capture,0,0,1));
    check(sxnaudio_capture_muted(control,0) == 0);
    check(capture_test_samples(&capture,0,0,0));
    audio_device_samples(&capture,5000,1000,300,750);
    check(!capture.dropped_frames);
    sxnaudio_capture_close(&capture);
    check(sxn_terminate(source,0) == 0 && sxn_waitpid(source,&status) == source && !status);
    preferences_flush(&remote,3005); // A second save must replace existing files.
    sxnaudio_control_close(&remote);
    check(sxn_terminate(daemon,0) == 0 && sxn_waitpid(daemon,&status) == daemon && !status);
    (void)sxn_close(control);
    check(sxn_log("nativetest: durable defaults/master/input/application controls restored with different stream ids and ports") == 0);
}

/* Sustained SUN source for the Sound panel's visual regression. Runs only
 * when explicitly requested; normal nativetest never enters this mode. */
static void audio_source(const char* name) {
    struct sxnaudio_stream stream;
    int16_t frames[2048 * 2];
    uint32_t phase = 0;
    uint64_t started;
    long pause = sxn_event_create(0);
    int amplitude = streq(name,"ToneB") ? 4000 : 12000;
    const uint32_t period = streq(name,"Preference restore test") ? 109 : 120;
    if (period == 109) amplitude = 3000;
    if (sxnaudio_open(&stream,name) < 0) sxn_exit(1);
    started = sxn_clock_ns();
    while (sxn_clock_ns() - started < 180000000000ull) {
        uint32_t due = sxnaudio_frames_due(&stream,30), i;
        long written;
        // Catch up the requested 30 ms lead in one turn after scheduling
        // delays; limiting each turn to 10 ms can starve a correctly paced
        // mixer while this source keeps sleeping between partial catch-ups.
        if (due > 2048) due = 2048;
        for (i = 0; i < due; ++i) {
            int16_t value = (phase + i) % period < period/2 ? amplitude : -amplitude;
            frames[i*2] = frames[i*2+1] = value;
        }
        written = sxnaudio_write(&stream,frames,due);
        if (written < 0) break;
        phase += (uint32_t)written;
        (void)sxn_wait_one(pause,5);
    }
    sxnaudio_close(&stream); (void)sxn_close(pause); sxn_exit(0);
}

static int file_bytes_equal(const char *a, const char *b, unsigned count) {
    unsigned i;
    for (i = 0; i < count; ++i) if (a[i] != b[i]) return 0;
    return 1;
}

static void test_files(void) {
    char bytes[64];
    long handle = sxn_open_file("/system/native-file-test.txt", SXN_FILE_READ);
    long duplicate;
    check(handle >= 0);
    if (handle >= 0) {
        check(sxn_read(handle, bytes, 4) == 4 && file_bytes_equal(bytes, "SUN ", 4));
        duplicate = sxn_duplicate(handle);
        check(duplicate >= 0);
        if (duplicate >= 0) {
            check(sxn_read(duplicate, bytes, 5) == 5 && file_bytes_equal(bytes, "file ", 5));
            check(sxn_close(duplicate) == 0);
        }
        check(sxn_read(handle, (void*)1, 1) == -SAVANXP_EINVAL);
        check(sxn_read(handle, bytes, sizeof(bytes)) == 13 && file_bytes_equal(bytes, "read fixture\n", 13));
        check(sxn_read(handle, bytes, 1) == 0);
        check(sxn_write(handle, bytes, 1) == -SAVANXP_EBADF);
        check(sxn_close(handle) == 0);
        check(sxn_read(handle, bytes, 1) == -SAVANXP_EBADF);
    }
    handle = sxn_open_file("/system/native-file-test.txt", 0);
    check(handle >= 0);
    if (handle >= 0) {
        check(sxn_read(handle, bytes, 1) == -SAVANXP_EBADF);
        check(sxn_close(handle) == 0);
    }
    check(sxn_open_file("/system", SXN_FILE_READ) == -SAVANXP_EISDIR);
    check(sxn_open_file("/dev/serial", 0) == -SAVANXP_EINVAL);
    check(sxn_open_file("/system/no-such-native-fixture", SXN_FILE_READ) == -SAVANXP_ENOENT);
    check(sxn_open_file("relative", SXN_FILE_READ) == -SAVANXP_EINVAL);
    check(sxn_open_file((const char*)1, SXN_FILE_READ) == -SAVANXP_EINVAL);
    check(sxn_open_file("/system/native-file-test.txt", 2) == -SAVANXP_EINVAL);
    {
        struct sxn_identity user = {1001u, 1001u, 100u, 0u};
        struct sxn_spawn_request request;
        char *args[] = {"/bin/nativetest.sxe", "file-user"};
        int status = -1;
        long child;
        sxn_spawn_request_init(&request, args[0], args, 2);
        child = sxn_spawn_ex_as(&request, &user);
        check(child > 0);
        if (child > 0) check(sxn_waitpid(child, &status) == child && status == 0);
    }
    check(sxn_write_file("relative", "x", 1) == -SAVANXP_EINVAL);
    check(sxn_write_file("/dev/serial", "x", 1) == -SAVANXP_EINVAL);
    check(sxn_write_file("/system", "x", 1) == -SAVANXP_EISDIR);
    check(sxn_write_file("/users/native-file-write-test", "long contents", 13) == 0);
    check(sxn_write_file("/users/native-file-write-test", (const void*)1, 1) == -SAVANXP_EINVAL);
    check(sxn_write_file("/users/native-file-write-test", "x", SXN_WRITE_FILE_MAX+1) == -SAVANXP_EINVAL);
    handle = sxn_open_file("/users/native-file-write-test", SXN_FILE_READ);
    check(handle >= 0);
    if (handle >= 0) {
        check(sxn_read(handle, bytes, sizeof(bytes)) == 13 && file_bytes_equal(bytes, "long contents", 13));
        (void)sxn_close(handle);
    }
    check(sxn_write_file("/users/native-file-write-test", "new", 3) == 0);
    handle = sxn_open_file("/users/native-file-write-test", SXN_FILE_READ);
    check(handle >= 0);
    if (handle >= 0) {
        check(sxn_read(handle, bytes, sizeof(bytes)) == 3 && file_bytes_equal(bytes, "new", 3));
        (void)sxn_close(handle);
    }
    check(sxn_write_file("/users/native-file-write-test", 0, 0) == 0);
    handle = sxn_open_file("/users/native-file-write-test", SXN_FILE_READ);
    check(handle >= 0);
    if (handle >= 0) { check(sxn_read(handle, bytes, 1) == 0); (void)sxn_close(handle); }
    check(sxn_log("nativetest: files done") == 0);
}

struct socket_cancel_test { long socket, cancel, delay; };
static unsigned char g_socket_cancel_stack[32768] __attribute__((aligned(16)));
static void socket_cancel_worker(void* argument) {
    struct socket_cancel_test* test = (struct socket_cancel_test*)argument;
    (void)sxn_wait_one(test->delay, 20);
    if (sxn_close(test->socket) != 0 || sxn_event_set(test->cancel) != 0) sxn_thread_exit(1);
    sxn_thread_exit(0);
}

/* Socket checks use SUN traps exclusively, including cancellation waits. */
static void test_sockets(void) {
    long rx = sxn_socket_create(SXN_SOCKET_UDP);
    long tx = sxn_socket_create(SXN_SOCKET_UDP);
    long tcp = sxn_socket_create(SXN_SOCKET_TCP);
    long cancel = sxn_event_create(SXN_EVENT_MANUAL_RESET);
    struct sxn_ipv4_endpoint local = {0x0a00020fu, 49123, 0}, source = {0};
    struct sxn_io_interest waits[2] = {{(int32_t)rx, SXN_IO_READ}, {(int32_t)cancel, 0}};
    char bytes[8] = {0};
    long duplicate;
    check(rx >= 0 && tx >= 0 && tcp >= 0 && cancel >= 0);
    check(sxn_socket_create(99) == -SAVANXP_EINVAL);
    check(sxn_socket_status(cancel) == -SAVANXP_EBADF);
    check(sxn_socket_connect(tcp, 0) == -SAVANXP_EINVAL);
    check(sxn_socket_status(tcp) == -SAVANXP_EINVAL);
    check(sxn_socket_bind(rx, &local) == 0);
    check(sxn_wait_io(waits, 2, 0) == -SAVANXP_ETIMEDOUT);
    check(sxn_socket_recvfrom(rx, bytes, sizeof(bytes), &source) == -SAVANXP_EAGAIN);
    check(sxn_socket_sendto(tx, "SUN", 3, &local) == 3);
    check(sxn_wait_io(waits, 2, 1000) == 0);
    check(sxn_socket_recvfrom(rx, bytes, sizeof(bytes), &source) == 3);
    check(bytes[0] == 'S' && bytes[1] == 'U' && bytes[2] == 'N');
    check(source.ipv4 == local.ipv4 && source.port != 0);
    check(sxn_event_set(cancel) == 0);
    check(sxn_wait_io(waits, 2, 1000) == 1);
    check(sxn_event_reset(cancel) == 0);
    check(sxn_wait_io(waits, 2, 10) == -SAVANXP_ETIMEDOUT);
    waits[0].interests = SXN_IO_WRITE;
    check(sxn_wait_io(waits, 2, 0) == 0);
    waits[0].interests = 8;
    check(sxn_wait_io(waits, 2, 0) == -SAVANXP_EINVAL);
    duplicate = sxn_duplicate(rx);
    check(duplicate >= 0 && sxn_close(rx) == 0);
    check(sxn_socket_sendto(tx, "x", 1, &local) == 1);
    check(sxn_socket_recvfrom(duplicate, bytes, sizeof(bytes), 0) == 1);
    check(sxn_close(duplicate) == 0);
    {
        struct socket_cancel_test test = {sxn_socket_create(SXN_SOCKET_UDP), cancel,
                                         sxn_event_create(0)};
        long worker;
        int32_t status = -1;
        check(sxn_event_reset(cancel) == 0);
        waits[0].handle = (int32_t)test.socket;
        waits[0].interests = SXN_IO_READ;
        worker = sxn_thread_create(socket_cancel_worker, &test,
                                   g_socket_cancel_stack + sizeof(g_socket_cancel_stack), 0);
        check(worker >= 0);
        if (worker >= 0) {
            check(sxn_wait_io(waits, 2, 1000) == 1);
            check(sxn_wait_one(worker, 1000) == 0);
            check(sxn_thread_exit_code(worker, &status) == 0 && status == 0);
            check(sxn_socket_status(test.socket) == -SAVANXP_EBADF);
            check(sxn_close(worker) == 0);
        } else (void)sxn_close(test.socket);
        check(sxn_close(test.delay) == 0);
    }
    check(sxn_close(tx) == 0 && sxn_close(tcp) == 0 && sxn_close(cancel) == 0);
    check(sxn_log("nativetest: native IPv4 sockets and cancellable readiness") == 0);
}

/* Readiness reports completion, not success. Status is retained across waits. */
static void check_connect_failure(long socket, long expected) {
    struct sxn_io_interest interest = {(int32_t)socket, SXN_IO_WRITE};
    char byte;
    check(sxn_wait_io(&interest, 1, 25000) == 0);
    check(sxn_socket_status(socket) == expected);
    check(sxn_socket_status(socket) == expected);
    interest.interests = SXN_IO_ERROR;
    check(sxn_wait_io(&interest, 1, 0) == 0);
    check(sxn_socket_status(socket) == expected);
    check(sxn_read(socket, &byte, 1) == expected);
    check(sxn_write(socket, "x", 1) == expected);
}

static void test_native_connect_failures(unsigned port) {
    long control = sxn_socket_create(SXN_SOCKET_TCP);
    long refused = sxn_socket_create(SXN_SOCKET_TCP);
    long stalled = sxn_socket_create(SXN_SOCKET_TCP);
    long device = sxn_open_device("net0", SXN_DEVICE_READ | SXN_DEVICE_WRITE);
    struct sxn_ipv4_endpoint peer = {0x0a000202u, (uint16_t)port, 0};
    struct sxn_io_interest interest = {(int32_t)control, SXN_IO_WRITE};
    struct savanxp_net_tcp_fault fault = {0};
    struct savanxp_net_tcp_stats before = {0}, after = {0};
    unsigned refused_port = 0, digits = 0;
    char byte = 0;
    long result;
    uint64_t started;
    check(control >= 0 && refused >= 0 && stalled >= 0 && device >= 0);
    if (control < 0 || refused < 0 || stalled < 0 || device < 0) goto cleanup;
    result = sxn_socket_connect(control, &peer);
    check(result == 0 || result == -SAVANXP_EAGAIN);
    check(sxn_wait_io(&interest, 1, 5000) == 0);
    check(sxn_socket_status(control) == 0);
    check(sxn_write(control, "REFUSE\n", 7) == 7);
    interest.interests = SXN_IO_READ;
    while (digits < 6) {
        if (sxn_wait_io(&interest, 1, 5000) != 0 || sxn_read(control, &byte, 1) != 1) break;
        if (byte == '\n') break;
        if (byte < '0' || byte > '9') break;
        refused_port = refused_port * 10u + (unsigned)(byte - '0');
        ++digits;
    }
    check(byte == '\n' && digits > 0 && refused_port > 0 && refused_port <= 65535);
    if (byte != '\n' || !refused_port || refused_port > 65535) goto cleanup;
    peer.port = (uint16_t)refused_port;
    result = sxn_socket_connect(refused, &peer);
    check(result == -SAVANXP_EAGAIN || result == -SAVANXP_ECONNREFUSED);
    check_connect_failure(refused, -SAVANXP_ECONNREFUSED);
    sxn_log("nativetest: pending connect: refusal retained after readiness");

    // Drop every outgoing SYN using the existing fault injector. No remote
    // blackhole or host firewall is needed, and the normal retry budget runs.
    check(sxn_device_control(device, NET_IOC_GET_TCP_STATS, (unsigned long)&before) == 0);
    fault.drop_tx_every = 1;
    if (sxn_device_control(device, NET_IOC_SET_TCP_FAULT, (unsigned long)&fault) != 0) {
        check(0);
        goto cleanup;
    }
    peer.port = (uint16_t)port;
    started = sxn_clock_ns();
    check(sxn_socket_connect(stalled, &peer) == -SAVANXP_EAGAIN);
    check(sxn_socket_status(stalled) == -SAVANXP_EAGAIN);
    check(sxn_socket_connect(stalled, &peer) == -SAVANXP_EBUSY);
    interest.handle = (int32_t)stalled;
    interest.interests = SXN_IO_WRITE | SXN_IO_ERROR;
    check(sxn_wait_io(&interest, 1, 0) == -SAVANXP_ETIMEDOUT);
    check(sxn_wait_io(&interest, 1, 20) == -SAVANXP_ETIMEDOUT);
    check(sxn_socket_status(stalled) == -SAVANXP_EAGAIN);
    check_connect_failure(stalled, -SAVANXP_ETIMEDOUT);
    check(sxn_clock_ns() - started < 25000000000ull);
    check(sxn_device_control(device, NET_IOC_GET_TCP_STATS, (unsigned long)&after) == 0);
    check(after.retransmits > before.retransmits && after.aborts > before.aborts);
    sxn_log("nativetest: pending connect: wait expiry preserves pending; retries expire as timeout");
    fault.drop_tx_every = 0;
    check(sxn_device_control(device, NET_IOC_SET_TCP_FAULT, (unsigned long)&fault) == 0);

    // A deliberate new connection replaces the retained failure.
    peer.port = (uint16_t)refused_port;
    result = sxn_socket_connect(stalled, &peer);
    check(result == -SAVANXP_EAGAIN || result == -SAVANXP_ECONNREFUSED);
    check_connect_failure(stalled, -SAVANXP_ECONNREFUSED);
cleanup:
    fault.drop_tx_every = 0;
    if (device >= 0) {
        check(sxn_device_control(device, NET_IOC_SET_TCP_FAULT, (unsigned long)&fault) == 0);
        check(sxn_close(device) == 0);
    }
    if (stalled >= 0) check(sxn_close(stalled) == 0);
    if (refused >= 0) check(sxn_close(refused) == 0);
    if (control >= 0) check(sxn_close(control) == 0);
}

static void test_native_tcp(unsigned port) {
    long socket = sxn_socket_create(SXN_SOCKET_TCP);
    struct sxn_ipv4_endpoint peer = {0x0a000202u, (uint16_t)port, 0};
    struct sxn_io_interest interest = {(int32_t)socket, SXN_IO_WRITE | SXN_IO_ERROR};
    const char request[] = "BULK 2048\nQUIT\n";
    unsigned char bytes[257];
    unsigned total = 0;
    long result;
    check(socket >= 0);
    result = sxn_socket_connect(socket, &peer);
    check(result == 0 || result == -SAVANXP_EAGAIN);
    check(sxn_wait_io(&interest, 1, 5000) == 0);
    check(sxn_socket_status(socket) == 0);
    check(sxn_socket_status(socket) == 0);
    check(sxn_socket_connect(socket, &peer) == -SAVANXP_EBUSY);
    interest.interests = SXN_IO_ERROR;
    check(sxn_wait_io(&interest, 1, 0) == -SAVANXP_ETIMEDOUT);
    check(sxn_write(socket, request, sizeof(request) - 1) == sizeof(request) - 1);
    interest.interests = SXN_IO_READ | SXN_IO_ERROR;
    while (total < 2048) {
        unsigned i;
        if (sxn_wait_io(&interest, 1, 5000) != 0) { check(0); break; }
        result = sxn_read(socket, bytes, sizeof(bytes));
        if (result == -SAVANXP_EAGAIN) continue;
        if (result <= 0) { check(0); break; }
        for (i = 0; i < (unsigned)result; ++i)
            check(bytes[i] == (unsigned char)(((total + i) * 31u + 7u) & 255u));
        total += (unsigned)result;
    }
    check(total == 2048);
    check(sxn_wait_io(&interest, 1, 5000) == 0);
    check(sxn_read(socket, bytes, sizeof(bytes)) == 0);
    check(sxn_close(socket) == 0);
}

int main(int argc, char** argv) {
    struct sxn_native_info info;
    struct sxn_native_info raw;
    long event_handle;
    long section_handle;
    long base;

    if (argc > 1 && streq(argv[1], "network")) {
        test_sockets();
        if (argc > 2) {
            unsigned port = 0;
            const char* digit = argv[2];
            while (*digit >= '0' && *digit <= '9') port = port * 10u + (unsigned)(*digit++ - '0');
            check(port > 0 && port <= 65535 && *digit == 0);
            if (port && port <= 65535 && *digit == 0) {
                test_native_tcp(port);
                test_native_connect_failures(port);
            }
        }
        sxn_log(g_failures ? "NATIVE NETWORK FAIL" : "NATIVE NETWORK PASS");
        sxn_exit(g_failures ? 1 : 0);
    }
    if (argc > 1 && streq(argv[1], "file-user")) {
        char byte;
        long handle;
        if (sxn_open_file("/users/native-file-private.txt", SXN_FILE_READ) != -SAVANXP_EACCES) sxn_exit(1);
        handle = sxn_open_file("/users/native-file-private.txt", 0);
        if (handle < 0 || sxn_read(handle, &byte, 1) != -SAVANXP_EBADF) sxn_exit(2);
        (void)sxn_close(handle);
        if (sxn_open_file("/users/native-file-private/readable.txt", 0) != -SAVANXP_EACCES) sxn_exit(3);
        if (sxn_write_file("/users/native-file-private.txt", "x", 1) != -SAVANXP_EACCES) sxn_exit(4);
        if (sxn_write_file("/system/native-file-test.txt", "x", 1) != -SAVANXP_EACCES) sxn_exit(5);
        sxn_exit(0);
    }
    if (argc > 1 && streq(argv[1], "child")) {
        run_child(argc, argv);
    }
    if (argc > 1 && streq(argv[1],"audio-writer-busy")) audio_busy_child(0);
    if (argc > 1 && streq(argv[1],"audio-capture-busy")) audio_busy_child(1);
    if (argc > 1 && streq(argv[1],"audio-preferences")) {
        test_audio_preferences();
        (void)sxn_log(g_failures ? "NATIVE PREFERENCES FAIL" : "NATIVE PREFERENCES PASS");
        sxn_exit(g_failures ? 1 : 0);
    }
    if (argc > 1 && streq(argv[1],"audio-devices")) {
        test_audio_devices();
        (void)sxn_log(g_failures ? "NATIVE DEVICES FAIL" : "NATIVE DEVICES PASS");
        sxn_exit(g_failures ? 1 : 0);
    }
    if (argc > 1 && streq(argv[1],"audio-capture")) {
        test_capture();
        (void)sxn_log(g_failures ? "NATIVE CAPTURE FAIL" : "NATIVE CAPTURE PASS");
        sxn_exit(g_failures ? 1 : 0);
    }
    if (argc > 2 && streq(argv[1], "audio-source")) audio_source(argv[2]);
    if (argc > 1 && streq(argv[1], "portchild")) {
        run_port_child();
    }
    if (argc > 1 && streq(argv[1], "sleeper")) {
        run_sleeper();
    }
    if (argc > 1 && streq(argv[1], "stdiochild")) {
        run_stdio_child();
    }
    if (argc > 1 && streq(argv[1], "middle")) {
        run_middle_child();
    }
    if (argc > 1 && streq(argv[1], "threads")) {
        run_threads_child();
    }
    if (argc > 1 && streq(argv[1], "lastexit")) {
        run_last_exit_child();
    }
    if (argc > 1 && streq(argv[1], "exitall")) {
        run_exit_all_child();
    }
    if (argc > 1 && streq(argv[1], "shootdown")) {
        run_shootdown_child();
    }
    if (argc > 1 && streq(argv[1], "userterm")) {
        run_user_terminate_child(argc, argv);
    }
    if (argc > 1 && streq(argv[1], "storageuser")) {
        run_storage_user_child(argc, argv);
    }

    /* Fullscreen launch policy rejects bad arguments before contacting windowd. */
    {
        struct sxnwm_client window = {0};
        check(sxnwm_connect_with_launch_flags(0, SAVANXP_DESKTOP_LAUNCH_FLAG_FULLSCREEN) == -SAVANXP_EINVAL);
        check(sxnwm_connect_with_launch_flags(&window, ~SAVANXP_DESKTOP_LAUNCH_FLAG_FULLSCREEN) == -SAVANXP_EINVAL);
    }
    check(sxn_info(&info) == 0);
    check(info.abi_version == SXN_ABI_VERSION);
    check(info.subsystem_id == 1u);
    check(info.pid != 0u);

    /* Wire-format cross-check: the header must match the trap. */
    check(raw_info(&raw) == 0);
    check(raw.pid == info.pid);

    check(sxn_log("nativetest: standalone SXN alive") == 0);

    /* POSIX numbers from a native process: ENOSYS, no delegation. */
    check(sxn_trap(SAVANXP_SYS_WRITE, 1, (unsigned long)"x", 1) == -SAVANXP_ENOSYS);
    check(sxn_trap(SAVANXP_SYS_GETPID, 0, 0, 0) == -SAVANXP_ENOSYS);

    /* Unknown native number: ENOSYS as well. */
    check(sxn_trap(0x1FFF, 0, 0, 0) == -SAVANXP_ENOSYS);

    /* Waiting on an unrelated pid is ECHILD, like the POSIX path. */
    {
        int status = 0;
        check(sxn_waitpid(0x7fffffffL, &status) == -SAVANXP_ECHILD);
    }

    /* Shared event plus section with the child (handles are inherited). */
    event_handle = sxn_event_create(SXN_EVENT_MANUAL_RESET);
    check(event_handle >= 0);
    section_handle = sxn_section_create(4096, SXN_SECTION_READ | SXN_SECTION_WRITE);
    check(section_handle >= 0);
    base = sxn_map_view(section_handle, SXN_SECTION_READ | SXN_SECTION_WRITE);
    check(base > 0);
    check(sxn_log("nativetest: parent mapped") == 0);
    if (base > 0) {
        *(volatile uint32_t*)base = SXN_PARENT_MARKER;
    }

    /* Self-spawn: the child maps the same section, answers, signals. */
    if (event_handle >= 0 && section_handle >= 0 && base > 0) {
        char event_text[24];
        char section_text[24];
        char request_text[24];
        char* child_argv[5];
        int status = -1;
        long ev_req = -1;
        long child;
        long reaped;
        int round;

        itoau(event_handle, event_text);
        itoau(section_handle, section_text);
        ev_req = sxn_event_create(SXN_EVENT_MANUAL_RESET);
        check(ev_req >= 0);
        itoau(ev_req, request_text);
        child_argv[0] = "/bin/nativetest.sxe";
        child_argv[1] = "child";
        child_argv[2] = event_text;
        child_argv[3] = section_text;
        child_argv[4] = request_text;
        child = sxn_spawn("/bin/nativetest.sxe", child_argv, 5);
        check(child > 0);
        if (child > 0) {
            volatile uint32_t* ring = (volatile uint32_t*)base + 16;

            check(sxn_wait_one(event_handle, 5000) == 0);
            check(sxn_log("nativetest: parent woke") == 0);
            check(*(volatile uint32_t*)base == SXN_CHILD_MARKER);
            /* The marker handshake leaves ev signaled; clear it so round 1
             * really waits for the child's first reply. */
            check(sxn_event_reset(event_handle) == 0);
            for (round = 1; round <= SXN_RING_ROUNDS; round += 1) {
                int round_failures = g_failures;

                ring[0] = (uint32_t)round;
                ring[1] = (uint32_t)(round * 3 + 1);
                check(sxn_event_set(ev_req) == 0);
                check(sxn_wait_one(event_handle, SXN_RING_WAIT_MS) == 0);
                check(ring[2] == (uint32_t)(round * 5 + 2));
                check(ring[3] == (uint32_t)round);
                check(sxn_event_reset(event_handle) == 0);
                if (g_failures != round_failures) {
                    break;
                }
            }
            check(sxn_log("nativetest: parent ring done") == 0);
            check(sxn_wait_one(event_handle, 0) == -SAVANXP_ETIMEDOUT);
            reaped = sxn_waitpid(child, &status);
            check(reaped == child);
            check(status == SXN_CHILD_STATUS);
        }
    } else {
        check(0);
    }

    if (base > 0) {
        check(sxn_unmap_view((uint64_t)base) == 0);
    }

    /* Handles, devices and the clock (phase 3 of
     * docs/desktop/DESKTOP_ARCHITECTURE.md). */
    {
        uint64_t first = sxn_clock_ns();
        uint64_t second = sxn_clock_ns();
        long duplicate = -1;
        long gpu = -1;
        long serial = -1;
        struct savanxp_gpu_info gpu_info;
        static const char k_serial_line[] = "nativetest: serial device\n";

        check(first != 0u && second >= first);

        duplicate = sxn_duplicate(event_handle);
        check(duplicate >= 0 && duplicate != event_handle);
        check(sxn_event_set(duplicate) == 0);
        check(sxn_wait_one(event_handle, 0) == 0);
        check(sxn_close(duplicate) == 0);
        check(sxn_close(duplicate) == -SAVANXP_EBADF);
        check(sxn_event_set(duplicate) == -SAVANXP_EBADF);
        /* An event is not a stream. */
        check(sxn_read(event_handle, &gpu_info, sizeof(gpu_info)) == -SAVANXP_EBADF);

        check(sxn_open_device("../disk", SXN_DEVICE_READ) == -SAVANXP_EINVAL);
        check(sxn_open_device("gpu0", 0) == -SAVANXP_EINVAL);
        check(sxn_open_device("nosuchdevice", SXN_DEVICE_READ) == -SAVANXP_ENOENT);
        gpu = sxn_open_device("gpu0", SXN_DEVICE_READ | SXN_DEVICE_WRITE);
        check(gpu >= 0);
        if (gpu >= 0) {
            gpu_info.width = 0;
            check(sxn_device_control(gpu, GPU_IOC_GET_INFO, (unsigned long)&gpu_info) == 0);
            check(gpu_info.width != 0u && gpu_info.height != 0u);
            check_gpu_3d(gpu, &gpu_info);
            check(sxn_close(gpu) == 0);
        }
        serial = sxn_open_device("serial", SXN_DEVICE_WRITE);
        check(serial >= 0);
        if (serial >= 0) {
            check(sxn_write(serial, k_serial_line, sizeof(k_serial_line) - 1u) ==
                (long)(sizeof(k_serial_line) - 1u));
            check(sxn_close(serial) == 0);
        }
        check(sxn_log("nativetest: handles, devices and clock done") == 0);
    }

    /* Ports: a service found by name instead of an inherited channel. */
    {
        struct sxn_port_connection connection;
        char* child_argv[2];
        char request[4] = {0};
        long port = sxn_port_create("nativetest");
        long event = sxn_event_create(SXN_EVENT_MANUAL_RESET);
        long child = -1;
        int status = -1;

        check(port >= 0 && event >= 0);
        check(sxn_port_create("nativetest") == -SAVANXP_EEXIST);
        child_argv[0] = "/bin/nativetest.sxe";
        child_argv[1] = "portchild";
        child = sxn_spawn("/bin/nativetest.sxe", child_argv, 2);
        check(child > 0);
        if (port >= 0 && event >= 0 && child > 0) {
            check(sxn_wait_one(port, 5000) == 0);
            check(sxn_port_accept(port, &connection) == 0);
            check(connection.client_pid == (uint32_t)child);
            /* WAIT_MANY on an event and a stream: the child's "ping" makes the
             * pipe readable, which is index 1 (and may still be on its way:
             * this also exercises waking a parked wait from a pipe write). */
            {
                int32_t targets[2];

                targets[0] = (int32_t)event;
                targets[1] = connection.read_handle;
                check(sxn_wait_many(targets, 2, 5000) == 1);
            }
            check(sxn_read(connection.read_handle, request, 4) == 4 && request[0] == 'p' && request[1] == 'i');
            /* A port does not travel: queued on a pipe of its own pending
             * connection it would keep itself alive forever. */
            check(sxn_pipe_send_handle(connection.write_handle, port) == -SAVANXP_EINVAL);
            check(sxn_pipe_send_handle(connection.write_handle, event) == 0);
            check(sxn_write(connection.write_handle, "pong", 4) == 4);
            check(sxn_wait_one(event, 5000) == 0);
            check(sxn_waitpid(child, &status) == child && status == SXN_PORT_CHILD_STATUS);
            check(sxn_close(connection.read_handle) == 0 && sxn_close(connection.write_handle) == 0);
            check(sxn_close(port) == 0);
            {
                int32_t none[2] = {-1, -1};
                check(sxn_port_connect("nativetest", none) == -SAVANXP_ENOENT);
            }
        }
        check(sxn_log("nativetest: ports done") == 0);
    }

    /* WAIT_MANY without a second process: the index of what is signaled, a
     * timeout when nothing is, and a stream that is readable or at its end.
     * The port connects to itself to get a pipe. */
    {
        struct sxn_port_connection self;
        int32_t pipes[2] = {-1, -1};
        int32_t targets[3];
        long port = sxn_port_create("nativetest-wait");
        long quiet = sxn_event_create(SXN_EVENT_MANUAL_RESET);
        long loud = sxn_event_create(SXN_EVENT_MANUAL_RESET);

        check(port >= 0 && quiet >= 0 && loud >= 0);
        check(sxn_port_connect("nativetest-wait", pipes) == 0);
        check(sxn_port_accept(port, &self) == 0);
        targets[0] = (int32_t)quiet;
        targets[1] = (int32_t)loud;
        targets[2] = self.read_handle;
        check(sxn_wait_many(targets, 3, 0) == -SAVANXP_ETIMEDOUT);
        check(sxn_wait_many(targets, 3, 20) == -SAVANXP_ETIMEDOUT);
        check(sxn_event_set(loud) == 0);
        check(sxn_wait_many(targets, 3, 0) == 1);
        check(sxn_event_reset(loud) == 0);
        check(sxn_write(pipes[0], "x", 1) == 1);
        check(sxn_wait_many(targets, 3, 0) == 2);
        /* Level-triggered: still readable until read. */
        check(sxn_wait_many(targets, 3, 0) == 2);
        {
            char byte = 0;
            check(sxn_read(self.read_handle, &byte, 1) == 1 && byte == 'x');
        }
        check(sxn_wait_many(targets, 3, 0) == -SAVANXP_ETIMEDOUT);
        /* The writer gone: end of stream is readable too. */
        check(sxn_close(pipes[0]) == 0);
        check(sxn_wait_many(targets, 3, 0) == 2);
        /* A write end cannot be waited on; neither can nothing. */
        targets[0] = self.write_handle;
        check(sxn_wait_many(targets, 1, 0) == -SAVANXP_EINVAL);
        check(sxn_wait_many(targets, 0, 0) == -SAVANXP_EINVAL);
        /* SET_NONBLOCKING: an empty read and a write with no room say EAGAIN
         * instead of parking the only thread of this process. */
        {
            static char fill[8192];
            char byte = 0;

            /* The reply direction: its writer is alive, so empty is EAGAIN
             * (the request direction already reads as ended). */
            check(sxn_set_nonblocking(pipes[1], 1) == 0);
            check(sxn_read(pipes[1], &byte, 1) == -SAVANXP_EAGAIN);
            check(sxn_set_nonblocking(self.write_handle, 1) == 0);
            check(sxn_write(self.write_handle, fill, sizeof(fill)) == (long)sizeof(fill));
            check(sxn_write(self.write_handle, fill, 1) == -SAVANXP_EAGAIN);
            check(sxn_set_nonblocking(quiet, 1) == -SAVANXP_EBADF);
        }
        check(sxn_close(pipes[1]) == 0 && sxn_close(self.read_handle) == 0 && sxn_close(self.write_handle) == 0);
        check(sxn_close(quiet) == 0 && sxn_close(loud) == 0 && sxn_close(port) == 0);
        check(sxn_log("nativetest: wait many done") == 0);
    }

    /* SPAWN_STDIO: the child gets stdio and nothing else this process holds;
     * SPAWN clones the whole table. */
    {
        char* child_argv[2];
        struct sxn_process_query whole;
        struct sxn_process_query bare;
        long cloned = -1;
        long isolated = -1;
        int status = 0;

        child_argv[0] = "/bin/nativetest.sxe";
        child_argv[1] = "sleeper";
        cloned = sxn_spawn("/bin/nativetest.sxe", child_argv, 2);
        isolated = sxn_spawn_stdio("/bin/nativetest.sxe", child_argv, 2);
        check(cloned > 0 && isolated > 0);
        if (cloned > 0 && isolated > 0) {
            check(sxn_process_query(cloned, &whole) == 0 && sxn_process_query(isolated, &bare) == 0);
            /* stdio, plus the event a sleeper creates as soon as it runs: on
             * several cores it may already be running here. */
            check(bare.handle_count <= 4u && whole.handle_count > bare.handle_count);
            check(sxn_terminate(cloned, 0) == 0 && sxn_waitpid(cloned, &status) == cloned);
            check(sxn_terminate(isolated, 0) == 0 && sxn_waitpid(isolated, &status) == isolated);
        }
        check(sxn_log("nativetest: spawn stdio done") == 0);
    }

    /* TERMINATE: a parent ends its child; anybody else's process is refused. */
    {
        char* child_argv[2];
        long child = -1;
        int status = -1;

        child_argv[0] = "/bin/nativetest.sxe";
        child_argv[1] = "sleeper";
        child = sxn_spawn("/bin/nativetest.sxe", child_argv, 2);
        check(child > 0);
        if (child > 0) {
            struct sxn_process_query query;

            check(sxn_process_query(child, &query) == 0);
            check(query.pid == (uint32_t)child && query.parent_pid == info.pid &&
                query.state == SXN_PROCESS_RUNNING && streq(query.name, "nativetest.sxe"));
            check(sxn_terminate(child, SXN_TERMINATE_STATUS) == 0);
            /* Ended but not reaped: still there, with its status. */
            check(sxn_process_query(child, &query) == 0);
            check(query.state == SXN_PROCESS_ENDED && query.exit_code == SXN_TERMINATE_STATUS);
            check(sxn_waitpid(child, &status) == child && status == SXN_TERMINATE_STATUS);
            /* Reaped: the pid names nothing any more. */
            check(sxn_terminate(child, 1) == -SAVANXP_ENOENT);
            check(sxn_process_query(child, &query) == -SAVANXP_ENOENT);
        }
        {
            struct sxn_process_query self;

            check(sxn_process_query(info.pid, &self) == 0);
            check(self.pid == info.pid && self.state == SXN_PROCESS_RUNNING && self.handle_count >= 3u);
            check(sxn_process_query(0, &self) == -SAVANXP_ENOENT);
        }
        /* pid 1 is the idle task, which nobody ends; the caller ends itself
         * with EXIT, not TERMINATE. */
        check(sxn_terminate(1, 1) == -SAVANXP_EACCES);
        check(sxn_terminate(info.pid, 1) == -SAVANXP_EINVAL);
        check(sxn_terminate(0, 1) == -SAVANXP_ENOENT);
        check(sxn_log("nativetest: terminate and query done") == 0);
    }

    /* TERMINATE outside the tree: kill's rule (USERS_AND_PERMISSIONS.md). Two
     * sleepers, one the system's and one the user's, and a child started as
     * the user that is the parent of neither. */
    {
        struct sxn_identity user = {1000u, 1000u, 100u, 0u};
        struct sxn_spawn_request request;
        char* sleeper_argv[2];
        char* child_argv[4];
        char system_text[16];
        char user_text[16];
        long system_sleeper;
        long user_sleeper = -1;
        long child = -1;
        int status = -1;

        sleeper_argv[0] = "/bin/nativetest.sxe";
        sleeper_argv[1] = "sleeper";
        system_sleeper = sxn_spawn_stdio("/bin/nativetest.sxe", sleeper_argv, 2);
        sxn_spawn_request_init(&request, "/bin/nativetest.sxe", sleeper_argv, 2);
        user_sleeper = sxn_spawn_ex_as(&request, &user);
        check(system_sleeper > 0 && user_sleeper > 0);
        if (system_sleeper > 0 && user_sleeper > 0) {
            itoau(system_sleeper, system_text);
            itoau(user_sleeper, user_text);
            child_argv[0] = "/bin/nativetest.sxe";
            child_argv[1] = "userterm";
            child_argv[2] = system_text;
            child_argv[3] = user_text;
            sxn_spawn_request_init(&request, "/bin/nativetest.sxe", child_argv, 4);
            child = sxn_spawn_ex_as(&request, &user);
            check(child > 0);
            check(child > 0 && sxn_waitpid(child, &status) == child && status == 0);
            /* The user's sleeper was ended by the child; reap it. */
            check(sxn_waitpid(user_sleeper, &status) == user_sleeper && status == 1);
        }
        if (system_sleeper > 0) {
            (void)sxn_terminate(system_sleeper, 0);
            (void)sxn_waitpid(system_sleeper, &status);
        }
        check(sxn_log("nativetest: terminate by owner done") == 0);
    }

    /* PROCESS_SNAPSHOT and SYSTEM_INFO: the task list's two reads. */
    {
        struct savanxp_process_info entry;
        struct savanxp_system_info system;
        unsigned long index = 0;
        int found_self = 0;
        int found_idle = 0;
        long result;

        while ((result = sxn_process_snapshot(index, &entry)) == 1) {
            if (entry.pid == info.pid) {
                found_self = streq(entry.name, "nativetest.sxe") && entry.thread_count >= 1u &&
                    entry.uid == 0u && (entry.flags & SAVANXP_PROC_FLAG_SYSTEM) != 0u &&
                    entry.handle_count >= 3u && entry.kernel_ticks <= entry.cpu_ticks;
            }
            if ((entry.flags & SAVANXP_PROC_FLAG_IDLE) != 0u) {
                found_idle = 1;
            }
            index += 1;
        }
        check(result == 0 && found_self && found_idle);
        check(sxn_trap(SXN_SYS_PROCESS_SNAPSHOT, 0, (unsigned long)&entry, SAVANXP_PROCESS_INFO_V1_SIZE - 1u) ==
            -SAVANXP_EINVAL);
        check(sxn_system_info(&system) == 0);
        check(system.cpu_online >= 1u && system.memory_free_bytes > 0u && system.cpu_ticks_total > 0u &&
            system.process_count >= 1u);
        check(sxn_log("nativetest: process snapshot and system info done") == 0);
    }

    /* STORAGE_SNAPSHOT (slice 28): the devices Disk Management lists. The
     * scenario boots from a disk, so something is mounted at "/" and holds
     * SxFS, and every partition's parent is a listed disk. */
    {
        struct sxn_storage_device device;
        struct sxn_storage_device devices[32];
        unsigned long index = 0;
        int root = -1;
        long result;
        unsigned long count;
        unsigned long other;

        while (index < 32ul && (result = sxn_storage_snapshot(index, &devices[index])) == 1) {
            if (streq(devices[index].mount_point, "/")) {
                root = (int)index;
            }
            index += 1;
        }
        count = index;
        check(count >= 1ul && (count == 32ul || result == 0));
        check(root >= 0);
        if (root >= 0) {
            check(streq(devices[root].fs_name, "sxfs"));
            check((devices[root].flags & (SXN_STORAGE_MOUNTED | SXN_STORAGE_SYSTEM)) ==
                (SXN_STORAGE_MOUNTED | SXN_STORAGE_SYSTEM));
            check(devices[root].total_bytes > 0u &&
                devices[root].used_bytes + devices[root].free_bytes == devices[root].total_bytes);
        }
        for (index = 0; index < count; ++index) {
            int parent_found = devices[index].kind != SXN_STORAGE_PARTITION;

            check(devices[index].id != 0u && devices[index].generation == devices[0].generation);
            for (other = 0; other < count; ++other) {
                if (other != index) {
                    check(devices[other].id != devices[index].id);
                }
                if (devices[index].kind == SXN_STORAGE_PARTITION && devices[other].id == devices[index].parent_id &&
                    devices[other].kind != SXN_STORAGE_PARTITION) {
                    parent_found = 1;
                }
            }
            check(parent_found);
        }
        check(sxn_storage_snapshot(count, &device) == 0);
        check(sxn_trap(SXN_SYS_STORAGE_SNAPSHOT, 0, (unsigned long)&device, SXN_STORAGE_DEVICE_V1_SIZE - 1u) ==
            -SAVANXP_EINVAL);
        check(sxn_trap(SXN_SYS_STORAGE_SNAPSHOT, 0, 16ul, sizeof(device)) == -SAVANXP_EINVAL);
        check(sxn_log("nativetest: storage snapshot done") == 0);

        /* STORAGE_READ, WRITE and RESCAN. The root's first sector is its SxFS
         * superblock; writing it back is refused while it is mounted, so the
         * image is never touched even if the check were broken: the bytes are
         * the ones just read. */
        if (root >= 0) {
            static uint8_t sector[512];
            struct sxn_identity user = {1000u, 1000u, 100u, 0u};
            struct sxn_spawn_request request;
            char* child_argv[3];
            char id_text[16];
            uint32_t root_id = devices[root].id;
            uint32_t disk_id = devices[root].kind == SXN_STORAGE_PARTITION ? devices[root].parent_id : root_id;
            int status = -1;
            long child;

            check(sxn_storage_read(root_id, 0, 1, sector) == 0);
            check(sector[0] == 'S' && sector[1] == 'X' && sector[2] == 'F' && sector[3] == 'S');
            check(sxn_storage_write(root_id, 0, 1, sector) == -SAVANXP_EBUSY);
            check(sxn_storage_write(disk_id, 0, 1, sector) == -SAVANXP_EBUSY || disk_id == root_id);
            check(sxn_storage_rescan(disk_id) == -SAVANXP_EBUSY);
            check(sxn_storage_read(root_id, 0, 0, sector) == -SAVANXP_EINVAL);
            check(sxn_storage_read(root_id, 0, SXN_STORAGE_MAX_TRANSFER + 1u, sector) == -SAVANXP_EINVAL);
            check(sxn_storage_read(root_id, devices[root].sector_count, 1, sector) == -SAVANXP_EINVAL);
            check(sxn_storage_read(0xfffffff0u, 0, 1, sector) == -SAVANXP_ENODEV);
            for (index = 0; index < count; ++index) {
                if (devices[index].kind == SXN_STORAGE_PARTITION) {
                    check(sxn_storage_rescan(devices[index].id) == -SAVANXP_EINVAL);
                    break;
                }
            }

            /* MOUNT and UNMOUNT: what they refuse. The root is mounted, so
             * mounting it again is busy; mount points live under /mnt only. */
            check(sxn_storage_mount(root_id, "/mnt/again") == -SAVANXP_EBUSY);
            check(sxn_storage_mount(root_id, "/tmp/x") == -SAVANXP_EINVAL);
            check(sxn_storage_mount(root_id, "/mnt/") == -SAVANXP_EINVAL);
            check(sxn_storage_mount(root_id, "/mnt/a/b") == -SAVANXP_EINVAL);
            check(sxn_storage_mount(root_id, "/mnt/..") == -SAVANXP_EINVAL);
            check(sxn_trap(SXN_SYS_STORAGE_MOUNT, root_id, (unsigned long)"/mnt/x", 1) == -SAVANXP_EINVAL);
            check(sxn_storage_mount(0xfffffff0u, "/mnt/x") == -SAVANXP_ENODEV);
            check(sxn_storage_unmount("/") == -SAVANXP_EINVAL);
            check(sxn_storage_unmount("/mnt/nothing") == -SAVANXP_EINVAL);
            /* The boot volume: the development boot drive's FAT partition,
             * mounted at /boot by the kernel and never unmounted. */
            {
                int boot = -1;

                for (index = 0; index < count; ++index) {
                    if ((devices[index].flags & SXN_STORAGE_MOUNTED) != 0u && streq(devices[index].mount_point, "/boot")) {
                        boot = (int)index;
                    }
                }
                check(boot >= 0 && streq(devices[boot].fs_name, "fat"));
                check(boot >= 0 && devices[boot].kind == SXN_STORAGE_PARTITION);
                check(sxn_storage_unmount("/boot") == -SAVANXP_EINVAL);
            }
            for (index = 0; index < count; ++index) {
                if (devices[index].kind == SXN_STORAGE_DISK && devices[index].scheme != SXN_STORAGE_SCHEME_NONE &&
                    (devices[index].flags & (SXN_STORAGE_MOUNTED | SXN_STORAGE_CHILD_MOUNTED)) == 0u) {
                    /* A partitioned disk's first sector is its table. */
                    check(sxn_storage_mount(devices[index].id, "/mnt/table") == -SAVANXP_EINVAL);
                    break;
                }
            }

            itoau((long)root_id, id_text);
            child_argv[0] = "/bin/nativetest.sxe";
            child_argv[1] = "storageuser";
            child_argv[2] = id_text;
            sxn_spawn_request_init(&request, "/bin/nativetest.sxe", child_argv, 3);
            child = sxn_spawn_ex_as(&request, &user);
            check(child > 0 && sxn_waitpid(child, &status) == child && status == 0);
            check(sxn_log("nativetest: storage read, write, rescan, mount and unmount done") == 0);
        }
    }

    /* PIPE_CREATE: a pipe made by a native process, both ends in its table.
     * Data crosses, the read end is a wait target, and closing the write end
     * is end of stream. */
    {
        int32_t pipe_handles[2] = {-1, -1};
        char buffer[8] = {0};

        check(sxn_pipe_create(pipe_handles) == 0);
        check(pipe_handles[0] >= 0 && pipe_handles[1] >= 0 && pipe_handles[0] != pipe_handles[1]);
        check(sxn_wait_one(pipe_handles[0], 0) == -SAVANXP_ETIMEDOUT);
        check(sxn_write(pipe_handles[1], "abc", 3) == 3);
        check(sxn_wait_many(&pipe_handles[0], 1, 100) == 0);
        check(sxn_read(pipe_handles[0], buffer, sizeof(buffer)) == 3 && buffer[0] == 'a' && buffer[2] == 'c');
        check(sxn_write(pipe_handles[0], "z", 1) < 0);
        check(sxn_close(pipe_handles[1]) == 0);
        check(sxn_read(pipe_handles[0], buffer, sizeof(buffer)) == 0);
        check(sxn_close(pipe_handles[0]) == 0);
        check(sxn_pipe_create(0) == -SAVANXP_EINVAL);
        check(sxn_log("nativetest: pipe create done") == 0);
    }

    /* SPAWN_EX: the child's 0, 1, 2 and 3 are exactly the handles named, and
     * it holds nothing else. Two pipes for stdio (stderr shares stdout, as a
     * console host does) and a third as handle 3. */
    {
        char* child_argv[2];
        struct sxn_spawn_request request;
        int32_t input[2] = {-1, -1};
        int32_t output[2] = {-1, -1};
        int32_t extra[2] = {-1, -1};
        char buffer[16] = {0};
        long child = -1;
        long total = 0;
        int status = -1;

        child_argv[0] = "/bin/nativetest.sxe";
        child_argv[1] = "stdiochild";
        check(sxn_pipe_create(input) == 0 && sxn_pipe_create(output) == 0 && sxn_pipe_create(extra) == 0);
        sxn_spawn_request_init(&request, "/bin/nativetest.sxe", child_argv, 2);
        request.std_handles[0] = input[0];
        request.std_handles[1] = output[1];
        request.std_handles[2] = output[1];
        request.handles[0] = extra[1];
        request.handle_count = 1;
        child = sxn_spawn_ex(&request);
        check(child > 0);
        /* The child holds its own references: the parent's copies of the ends
         * it gave away go, so end of stream reaches the parent. */
        (void)sxn_close(input[0]);
        (void)sxn_close(output[1]);
        (void)sxn_close(extra[1]);
        if (child > 0) {
            check(sxn_write(input[1], "hi\n", 3) == 3);
            for (;;) {
                long count = sxn_read(output[0], buffer + total, sizeof(buffer) - 1u - (unsigned long)total);
                if (count <= 0) {
                    break;
                }
                total += count;
            }
            buffer[total] = '\0';
            check(streq(buffer, "hi\nerr"));
            check(sxn_read(extra[0], buffer, sizeof(buffer)) == 2 && buffer[0] == 'x' && buffer[1] == '3');
            check(sxn_waitpid(child, &status) == child && status == 0);
        }
        (void)sxn_close(input[1]);
        (void)sxn_close(output[0]);
        (void)sxn_close(extra[0]);

        /* Refusals: a handle that is not open, a flag that does not exist,
         * more extra handles than fit. */
        sxn_spawn_request_init(&request, "/bin/nativetest.sxe", child_argv, 2);
        request.std_handles[1] = 60;
        check(sxn_spawn_ex(&request) == -SAVANXP_EBADF);
        sxn_spawn_request_init(&request, "/bin/nativetest.sxe", child_argv, 2);
        request.flags = 1;
        check(sxn_spawn_ex(&request) == -SAVANXP_EINVAL);
        sxn_spawn_request_init(&request, "/bin/nativetest.sxe", child_argv, 2);
        request.handle_count = SXN_SPAWN_MAX_HANDLES + 1u;
        check(sxn_spawn_ex(&request) == -SAVANXP_EINVAL);
        check(sxn_log("nativetest: spawn ex done") == 0);
    }

    /* TERMINATE reaches a grandchild: the middle child reports its sleeper's
     * pid, this process ends the sleeper, and the middle child exits with the
     * status its waitpid saw. */
    {
        char* child_argv[2];
        struct sxn_spawn_request request;
        int32_t report[2] = {-1, -1};
        int32_t grandchild = 0;
        long middle = -1;
        int status = -1;

        child_argv[0] = "/bin/nativetest.sxe";
        child_argv[1] = "middle";
        check(sxn_pipe_create(report) == 0);
        sxn_spawn_request_init(&request, "/bin/nativetest.sxe", child_argv, 2);
        request.std_handles[1] = report[1];
        middle = sxn_spawn_ex(&request);
        check(middle > 0);
        (void)sxn_close(report[1]);
        if (middle > 0) {
            check(sxn_read(report[0], &grandchild, sizeof(grandchild)) == (long)sizeof(grandchild));
            check(grandchild > 0 && grandchild != (int32_t)middle);
            if (grandchild > 0) {
                struct sxn_process_query query;

                check(sxn_process_query(grandchild, &query) == 0 && query.parent_pid == (uint32_t)middle);
                check(sxn_terminate(grandchild, SXN_TERMINATE_STATUS) == 0);
            }
            check(sxn_waitpid(middle, &status) == middle && status == SXN_TERMINATE_STATUS);
        }
        (void)sxn_close(report[0]);
        check(sxn_log("nativetest: terminate descendant done") == 0);
    }

    test_files();
    test_threads(&info);
    test_thread_bases();
    test_address_waits();
    test_audio();

    /* The heap (docs/userland/NATIVE_HEAP.md). The bootstrap arena is BSS and
     * needs no system call; the arenas past it come from the native page
     * source (system/runtime/sxn_runtime.c): sections, mapped and their
     * handles closed. Several MiB held at once, each block its own pattern so
     * an overlap shows; no descriptor left behind; then allocate-and-free
     * round after round, where every free empties an arena and gives it back
     * (sx_arena_release) and the next allocation maps one again. A refused
     * 1 MiB here would mean the runtime's weak POSIX page source is the one
     * linked: ENOSYS. */
    {
        enum { HEAP_BLOCKS = 4 };
        static const unsigned long sizes[HEAP_BLOCKS] = {
            64ul * 1024ul, 1024ul * 1024ul, 3ul * 1024ul * 1024ul, 6ul * 1024ul * 1024ul};
        unsigned char* blocks[HEAP_BLOCKS] = {0, 0, 0, 0};
        struct sxn_process_query before;
        struct sxn_process_query after;
        unsigned long index;
        unsigned long offset;
        int round;
        int intact = 1;

        check(sxn_process_query(info.pid, &before) == 0);
        for (index = 0; index < HEAP_BLOCKS; ++index) {
            blocks[index] = (unsigned char*)malloc(sizes[index]);
            check(blocks[index] != 0);
        }
        for (index = 0; index < HEAP_BLOCKS; ++index) {
            if (blocks[index] == 0) {
                continue;
            }
            for (offset = 0; offset < sizes[index]; offset += 4096ul) {
                blocks[index][offset] = (unsigned char)(index * 37u + (offset >> 12));
            }
            blocks[index][sizes[index] - 1u] = (unsigned char)(0xA0u + index);
        }
        for (index = 0; index < HEAP_BLOCKS; ++index) {
            if (blocks[index] == 0) {
                continue;
            }
            for (offset = 0; offset < sizes[index]; offset += 4096ul) {
                if (blocks[index][offset] != (unsigned char)(index * 37u + (offset >> 12))) {
                    intact = 0;
                }
            }
            if (blocks[index][sizes[index] - 1u] != (unsigned char)(0xA0u + index)) {
                intact = 0;
            }
        }
        check(intact);
        for (index = 0; index < HEAP_BLOCKS; ++index) {
            free(blocks[index]);
        }
        check(sxn_process_query(info.pid, &after) == 0 && after.handle_count == before.handle_count);

        for (round = 0; round < 24; ++round) {
            unsigned long size = 2ul * 1024ul * 1024ul + (unsigned long)round * 4096ul;
            unsigned char* block = (unsigned char*)malloc(size);

            check(block != 0);
            if (block == 0) {
                break;
            }
            block[0] = (unsigned char)round;
            block[size - 1u] = (unsigned char)~round;
            check(block[0] == (unsigned char)round && block[size - 1u] == (unsigned char)~round);
            free(block);
        }
        check(sxn_log("nativetest: heap: arenas from the native page source") == 0);
    }

    sxn_exit(g_failures == 0 ? 0 : 1);
    __builtin_unreachable();
}
