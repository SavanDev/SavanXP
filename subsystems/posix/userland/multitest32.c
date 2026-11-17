#include "libc.h"

extern int context_probe32(unsigned int marker, unsigned int deadline);
static volatile unsigned int g_private_marker;
static unsigned char g_pages[8192] __attribute__((aligned(4096)));
static const char* const kPath = "/disk/bin/multitest32";

static void number(char* text, unsigned int value) {
    char reversed[12];
    unsigned int count = 0;
    do {
        reversed[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);
    unsigned int index = 0;
    while (count != 0) text[index++] = reversed[--count];
    text[index] = 0;
}

static void append(char* destination, const char* text) {
    strcpy(destination + strlen(destination), text);
}

static unsigned int parse(const char* text) {
    unsigned int value = 0;
    while (*text >= '0' && *text <= '9') value = value * 10 + (unsigned int)(*text++ - '0');
    return value;
}

static int fail(const char* reason) {
    puts_out("MULTITASK FAIL ");
    puts_out(reason);
    puts_out("\n");
    return 1;
}

static long child(const char* mode) {
    const char* arguments[] = {kPath, mode};
    return spawn(kPath, arguments, 2);
}

static int collect(long pid, int expected) {
    int status = -1;
    return pid > 0 && savanxp_waitpid((int)pid, &status) == pid && status == expected;
}

static int worker(int argc, char** argv) {
    if (strcmp(argv[1], "cpu") == 0 && argc == 4) {
        unsigned int id = parse(argv[2]);
        unsigned int deadline = parse(argv[3]);
        volatile unsigned int stack_marker = 0xfeed0000 + id;
        g_private_marker = id;
        unsigned int start = (unsigned int)uptime_ms();
        int result = context_probe32(id, deadline);
        char line[160], id_text[12], pid_text[12], start_text[12], end_text[12], result_text[12];
        number(id_text, id);
        number(pid_text, (unsigned int)savanxp_getpid());
        number(start_text, start);
        number(end_text, (unsigned int)uptime_ms());
        number(result_text, (unsigned int)result);
        strcpy(line, "CPU worker "); append(line, id_text);
        append(line, " pid "); append(line, pid_text);
        append(line, " start "); append(line, start_text);
        append(line, " end "); append(line, end_text);
        append(line, " result "); append(line, result_text); append(line, "\n");
        puts_out(line);
        if (result != 0 || g_private_marker != id || stack_marker != 0xfeed0000 + id) return 100 + result;
        return 17 + (int)id;
    }
    if (strcmp(argv[1], "sleep") == 0) {
        sleep_ms(200);
        return 42;
    }
    if (strcmp(argv[1], "orphan") == 0) {
        return child("sleep") > 0 ? 24 : 96;
    }
    if (strcmp(argv[1], "zombie-orphan") == 0) {
        if (child("short") <= 0) return 97;
        sleep_ms(50);
        return 25;
    }
    if (strcmp(argv[1], "fault-ud") == 0 || strcmp(argv[1], "fault") == 0) __asm__ volatile("ud2");
    if (strcmp(argv[1], "fault-div") == 0)
        __asm__ volatile("xorl %%edx, %%edx; movl $1, %%eax; xorl %%ecx, %%ecx; divl %%ecx"
                         : : : "eax", "ecx", "edx", "cc");
    if (strcmp(argv[1], "fault-null") == 0)
        __asm__ volatile("movl 0, %%eax" : : : "eax");
    if (strcmp(argv[1], "fault-hole") == 0)
        __asm__ volatile("movl 0x08800000, %%eax" : : : "eax");
    if (strcmp(argv[1], "fault-kernel") == 0)
        __asm__ volatile("movl 0x00100000, %%eax" : : : "eax");
    if (strcmp(argv[1], "fault-alias") == 0)
        __asm__ volatile("movl 0x40000000, %%eax" : : : "eax");
    if (strcmp(argv[1], "fault-text") == 0)
        __asm__ volatile("movb $0, 0x08000000" : : : "memory");
    if (strcmp(argv[1], "fault-guard") == 0)
        __asm__ volatile("movb $0, 0x0eeff000" : : : "memory");
    if (strcmp(argv[1], "fault-gp") == 0) __asm__ volatile("cli");
    if (strcmp(argv[1], "fault-io") == 0) {
        // Port 0x80 used to be permitted by the malformed TSS I/O-map offset.
        // Correct ring-3 permissions must fault before issuing this harmless IN.
        __asm__ volatile("inb $0x80, %%al" ::: "eax");
        puts_out("MEMORY ring-3 port I/O unexpectedly permitted\n");
        return 99;
    }
    if (strcmp(argv[1], "fault-segments") == 0)
        __asm__ volatile("xorw %%ax, %%ax; movw %%ax, %%ds; movw %%ax, %%es; "
                         "movw %%ax, %%fs; movw %%ax, %%gs; std; ud2" : : : "eax", "cc");
    if (strcmp(argv[1], "fault-gpu") == 0) {
        long fd = gpu_open();
        if (fd != 3 || gpu_acquire((int)fd) != 0) return 88;
        __asm__ volatile("ud2");
    }
    if (strcmp(argv[1], "short") == 0) return 23;
    if (strcmp(argv[1], "args") == 0) {
        if (argc != SAVANXP_MAX_ARGC) return 90;
        for (int index = 2; index < argc; ++index) {
            if (strlen(argv[index]) != SAVANXP_MAX_ARG_LENGTH) return 91;
            for (int letter = 0; letter < SAVANXP_MAX_ARG_LENGTH; ++letter) {
                if (argv[index][letter] != 'a' + index) return 92;
            }
        }
        return 0;
    }
    if (strcmp(argv[1], "gpu-busy") == 0) {
        long fd;
        struct savanxp_gpu_info info;
        return gpu_get_info(3, &info) == -SAVANXP_EBADF && (fd = gpu_open()) >= 0 &&
            gpu_acquire((int)fd) == -SAVANXP_EBUSY && savanxp_close((int)fd) == 0 ? 0 : 93;
    }
    if (strcmp(argv[1], "gpu-exit") == 0) {
        long fd = gpu_open();
        return fd == 3 && gpu_acquire((int)fd) == 0 ? 0 : 94;
    }
    return 95;
}

static int memory_checks(void) {
    struct savanxp_system_info before, after;
    if (system_info(&before) < 0 || before.memory_free_bytes == 0) return fail("memory accounting");
    // Kernel destinations must be writable; bad pointers must return EFAULT,
    // including present read-only text and ranges crossing into an absent page.
    const char* readonly = "read only";
    if (system_info((struct savanxp_system_info*)readonly) != -SAVANXP_EFAULT ||
        savanxp_waitpid(-1, (int*)readonly) != -SAVANXP_EFAULT ||
        savanxp_read(0, (void*)readonly, sizeof(struct savanxp_input_event)) != -SAVANXP_EFAULT ||
        savanxp_write(1, (void*)0x08800000, 1) != -SAVANXP_EFAULT ||
        savanxp_write(1, (void*)0x0efffffe, 4) != -SAVANXP_EFAULT ||
        savanxp_write(1, (void*)0xfffffffe, 4) != -SAVANXP_EFAULT ||
        spawn((const char*)0x08800000, 0, 0) != -SAVANXP_EFAULT)
        return fail("page-aware syscall validation");
    long fd = gpu_open();
    if (fd != 3 || gpu_get_info((int)fd, (struct savanxp_gpu_info*)readonly) != -SAVANXP_EINVAL ||
        savanxp_close((int)fd) != 0) return fail("read-only ioctl buffer");
    // Both immediate and blocking wait results straddle distinct physical pages.
    int* status = (int*)(g_pages + 4094);
    long pid = child("short");
    sleep_ms(50);
    if (pid < 0 || savanxp_waitpid((int)pid, status) != pid || *status != 23)
        return fail("cross-page zombie status");
    pid = child("sleep");
    if (pid < 0 || savanxp_waitpid((int)pid, status) != pid || *status != 42)
        return fail("cross-page blocked status");
    const char* modes[] = {"fault-ud", "fault-div", "fault-null", "fault-hole", "fault-kernel",
                          "fault-alias", "fault-text", "fault-guard", "fault-gp", "fault-segments",
                          "fault-gpu", "fault-io"};
    const int vectors[] = {6, 0, 14, 14, 14, 14, 14, 14, 13, 6, 6, 13};
    long sibling = child("sleep");
    if (sibling < 0) return fail("fault sibling");
    for (unsigned int index = 0; index < sizeof(vectors) / sizeof(vectors[0]); ++index) {
        if (!collect(child(modes[index]), 128 + vectors[index])) return fail(modes[index]);
    }
    if (!collect(sibling, 42) || g_private_marker != 0xabcdef01) return fail("fault sibling isolation");
    if (!collect(child("gpu-exit"), 0)) return fail("GPU cleanup after fault");
    if (system_info(&after) < 0 || after.memory_free_bytes != before.memory_free_bytes)
        return fail("fault page leak");
    puts_out("MEMORY faults and protection PASS\n");

    const char* cross_args[] = {"/disk/bin/memhog32", "crosspage"};
    if (!collect(spawn(cross_args[0], cross_args, 2), 43)) return fail("fragmented result copying");
    if (system_info(&after) < 0 || after.memory_free_bytes != before.memory_free_bytes)
        return fail("fragmented result page leak");
    puts_out("MEMORY fragmented copy PASS\n");

    // Keep successfully loaded children blocked while retrying a failed load.
    // Low RAM forces failure in a partially populated image, not just a full PID table.
    const char* hog = "/disk/bin/memhog32";
    const char* args[] = {hog};
    for (int round = 0; round < 2; ++round) {
        long children[64];
        int count = 0;
        long error = 0;
        for (; count < 64; ++count) {
            long result = spawn(hog, args, 1);
            if (result < 0) { error = result; break; }
            children[count] = result;
        }
        if (count == 0 || (error != -SAVANXP_ENOMEM && error != -SAVANXP_EAGAIN))
            return fail("memory capacity");
        if (system_info(&after) < 0) return fail("memory sample");
        unsigned long long remaining = after.memory_free_bytes;
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (spawn(hog, args, 1) != error || system_info(&after) < 0 ||
                after.memory_free_bytes != remaining) return fail("failed load rollback");
        }
        if (error == -SAVANXP_ENOMEM) puts_out("MEMORY ENOMEM rollback PASS\n");
        for (int index = 0; index < count; ++index)
            if (!collect(children[index], 43)) return fail("memory pressure survivor");
        if (system_info(&after) < 0 || after.memory_free_bytes != before.memory_free_bytes)
            return fail("memory pressure leak");
    }
    puts_out("MEMORY reclamation PASS\n");
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 0) return 31;
    if (argc > 1) return worker(argc, argv);
    struct savanxp_system_info baseline, after;
    if (system_info(&baseline) < 0 || baseline.process_count < 3) return fail("process count");
    g_private_marker = 0xabcdef01;
    const unsigned int began = (unsigned int)uptime_ms();
    char deadline[12]; number(deadline, began + 600);
    const char* first_args[] = {kPath, "cpu", "1", deadline};
    const char* second_args[] = {kPath, "cpu", "2", deadline};
    long first = spawn(kPath, first_args, 4);
    long second = spawn(kPath, second_args, 4);
    if (first <= 0 || second <= 0 || first == second || first == savanxp_getpid()) return fail("PID allocation");
    if (!collect(second, 19)) return fail("specific blocking wait / second context");
    int status = -1;
    if (savanxp_waitpid(SAVANXP_WAIT_ANY, &status) != first || status != 18) return fail("wait-any / first context");
    if (g_private_marker != 0xabcdef01) return fail("private user memory");
    if (savanxp_waitpid((int)first, 0) != -SAVANXP_ECHILD ||
        savanxp_waitpid(1, 0) != -SAVANXP_ECHILD ||
        savanxp_waitpid(0, 0) != -SAVANXP_EINVAL ||
        savanxp_waitpid(-1, (int*)1) != -SAVANXP_EFAULT) return fail("wait validation");
    puts_out("MULTITASK contexts and waits PASS\n");

    const char* args[] = {kPath};
    char too_long[65]; memset(too_long, 'x', sizeof(too_long));
    const char* bad_args[] = {too_long};
    const char* invalid_args[] = {(const char*)1};
    if (spawn((const char*)1, 0, 0) != -SAVANXP_EFAULT ||
        spawn("/disk/bin/missing32", 0, 0) != -SAVANXP_ENOENT ||
        spawn("/disk/bin", 0, 0) != -SAVANXP_ENOEXEC ||
        spawn(kPath, args, SAVANXP_MAX_ARGC + 1) != -SAVANXP_E2BIG ||
        spawn(kPath, (const char* const*)1, 1) != -SAVANXP_EFAULT ||
        spawn(kPath, invalid_args, 1) != -SAVANXP_EFAULT ||
        spawn(kPath, bad_args, 1) != -SAVANXP_E2BIG) return fail("spawn validation");
    char strings[SAVANXP_MAX_ARGC][SAVANXP_MAX_ARG_LENGTH + 1];
    const char* many_args[SAVANXP_MAX_ARGC];
    many_args[0] = kPath; many_args[1] = "args";
    for (int index = 2; index < SAVANXP_MAX_ARGC; ++index) {
        memset(strings[index], 'a' + index, SAVANXP_MAX_ARG_LENGTH);
        strings[index][SAVANXP_MAX_ARG_LENGTH] = 0;
        many_args[index] = strings[index];
    }
    if (!collect(spawn(kPath, many_args, SAVANXP_MAX_ARGC), 0)) return fail("argument copying");
    if (!collect(spawn(kPath, 0, 0), 31)) return fail("empty argv");
    puts_out("MULTITASK arguments PASS\n");

    // Fill every remaining slot with sleeping children, then reap and reuse it.
    long children[64]; int count = 0;
    for (; count < 64; ++count) {
        long pid = child("sleep");
        if (pid == -SAVANXP_EAGAIN) break;
        if (pid < 0) return fail("capacity spawn");
        children[count] = pid;
    }
    if (count == 0 || count == 64) return fail("bounded table");
    unsigned long before_sleep = uptime_ms();
    sleep_ms(50);
    if (uptime_ms() - before_sleep < 50) return fail("sleep wakeup");
    for (int index = 0; index < count; ++index) {
        if (!collect(children[index], 42)) return fail("sleeping children");
    }
    long previous_pid = 0;
    for (int index = 0; index < 12; ++index) {
        long pid = child("short");
        if (pid <= previous_pid || !collect(pid, 23)) return fail("reap and reuse");
        previous_pid = pid;
    }
    if (system_info(&after) < 0 || after.process_count != baseline.process_count) return fail("process leak");
    if (!collect(child("orphan"), 24)) return fail("live orphan parent");
    sleep_ms(250);
    if (!collect(child("zombie-orphan"), 25)) return fail("zombie orphan parent");
    if (system_info(&after) < 0 || after.process_count != baseline.process_count) return fail("orphan leak");
    long null_status_child = child("short");
    if (null_status_child < 0 || savanxp_waitpid((int)null_status_child, 0) != null_status_child ||
        yield() != 0) return fail("null status / yield");
    puts_out("MULTITASK sleep and reclamation PASS\n");

    long fd = gpu_open();
    if (fd != 3 || gpu_acquire((int)fd) != 0 || !collect(child("gpu-busy"), 0) || savanxp_close((int)fd) < 0 ||
        !collect(child("gpu-exit"), 0) || !collect(child("gpu-exit"), 0)) return fail("GPU ownership on exit");
    puts_out("MULTITASK GPU ownership PASS\n");
    if (memory_checks() != 0) return 1;
    puts_out("MULTITASK PASS\n");
    return 0;
}
