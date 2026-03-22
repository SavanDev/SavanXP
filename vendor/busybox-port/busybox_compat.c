#include "libbb.h"

const char* applet_name = "busybox";
int optind = 1;
char* optarg = NULL;
const char bb_msg_write_error[] = "write error";
const char bb_msg_memory_exhausted[] = "out of memory";
const char bb_msg_requires_arg[] = "%s requires an argument";
const char bb_default_path[] = "/disk/bin:/bin";
const char bb_PATH_root_path[] = "PATH=/disk/bin:/bin";
const char bb_busybox_exec_path[] = "/bin/busybox";
const char bb_banner[] = "BusyBox 1.37.0";
char bb_common_bufsiz1[4096];
volatile sig_atomic_t bb_got_signal = 0;

static const char* bb_error_name(int code) {
    switch (code) {
        case 0: return "ok";
        case ENOENT: return "no such file or directory";
        case EIO: return "i/o error";
        case EBADF: return "bad file descriptor";
        case ECHILD: return "no child processes";
        case EAGAIN: return "resource temporarily unavailable";
        case ENOMEM: return "out of memory";
        case EACCES: return "permission denied";
        case EBUSY: return "device or resource busy";
        case EEXIST: return "file exists";
        case ENODEV: return "no such device";
        case ENOTDIR: return "not a directory";
        case EISDIR: return "is a directory";
        case EINVAL: return "invalid argument";
        case ENOTTY: return "inappropriate ioctl";
        case ENOSPC: return "no space left on device";
        case EPIPE: return "broken pipe";
        case ENOSYS: return "function not implemented";
        case ENOTEMPTY: return "directory not empty";
        case ETIMEDOUT: return "timed out";
        default: return "error";
    }
}

static void bb_vmessage(FILE* stream, const char* suffix, const char* format, va_list args) {
    if (applet_name != NULL && applet_name[0] != '\0') {
        fprintf(stream, "%s: ", applet_name);
    }
    vfprintf(stream, format, args);
    if (suffix != NULL) {
        fprintf(stream, ": %s", suffix);
    }
    fprintf(stream, "\n");
}

static int next_option_bit(const char* optstring, char option) {
    int bit = 0;
    for (size_t index = 0; optstring[index] != '\0'; ++index) {
        const char current = optstring[index];
        if (current == '^') {
            continue;
        }
        if (current == ':') {
            continue;
        }
        if (current == option) {
            return bit;
        }
        ++bit;
        if (optstring[index + 1] == ':') {
            continue;
        }
    }
    return -1;
}

static int option_requires_argument(const char* optstring, char option) {
    for (size_t index = 0; optstring[index] != '\0'; ++index) {
        if (optstring[index] == option) {
            return optstring[index + 1] == ':';
        }
    }
    return 0;
}

char* bb_basename(const char* path) {
    const char* name = path != NULL ? path : "";
    for (const char* cursor = name; *cursor != '\0'; ++cursor) {
        if (*cursor == '/') {
            name = cursor + 1;
        }
    }
    return (char*)name;
}

char* bb_stpcpy(char* destination, const char* source) {
    while (*source != '\0') {
        *destination++ = *source++;
    }
    *destination = '\0';
    return destination;
}

void* xmalloc(size_t size) {
    void* pointer = malloc(size);
    if (pointer == NULL) {
        bb_simple_error_msg_and_die("out of memory");
    }
    return pointer;
}

void* xzalloc(size_t size) {
    void* pointer = calloc(1, size);
    if (pointer == NULL) {
        bb_simple_error_msg_and_die(bb_msg_memory_exhausted);
    }
    return pointer;
}

void* xrealloc(void* pointer, size_t size) {
    void* resized = realloc(pointer, size);
    if (resized == NULL) {
        bb_simple_error_msg_and_die(bb_msg_memory_exhausted);
    }
    return resized;
}

void* xmemdup(const void* source, size_t size) {
    void* copy = xmalloc(size);
    if (size != 0) {
        memcpy(copy, source, size);
    }
    return copy;
}

char* xstrdup(const char* text) {
    char* copy = strdup(text != NULL ? text : "");
    if (copy == NULL) {
        bb_simple_error_msg_and_die("out of memory");
    }
    return copy;
}

ssize_t full_write(int fd, const void* buffer, size_t count) {
    const unsigned char* bytes = (const unsigned char*)buffer;
    size_t written = 0;

    while (written < count) {
        const ssize_t step = write(fd, bytes + written, count - written);
        if (step < 0) {
            return -1;
        }
        if (step == 0) {
            break;
        }
        written += (size_t)step;
    }

    return (ssize_t)written;
}

ssize_t safe_write(int fd, const void* buffer, size_t count) {
    return write(fd, buffer, count);
}

int bb_ask_y_confirmation(void) {
    char line[8] = {};
    if (fgets(line, sizeof(line), stdin) == NULL) {
        return 0;
    }
    return line[0] == 'y' || line[0] == 'Y';
}

void bb_show_usage(void) {
    fprintf(stderr, "%s: invalid usage\n", applet_name != NULL ? applet_name : "busybox");
    exit(1);
}

void fflush_all(void) {
    fflush(stdout);
    fflush(stderr);
}

void bb_simple_error_msg(const char* text) {
    if (applet_name != NULL && applet_name[0] != '\0') {
        fprintf(stderr, "%s: ", applet_name);
    }
    fprintf(stderr, "%s\n", text != NULL ? text : "");
}

void bb_simple_error_msg_and_die(const char* text) {
    bb_simple_error_msg(text);
    exit(1);
}

void bb_simple_perror_msg(const char* text) {
    if (applet_name != NULL && applet_name[0] != '\0') {
        fprintf(stderr, "%s: ", applet_name);
    }
    fprintf(stderr, "%s: %s\n", text != NULL ? text : "", bb_error_name(errno));
}

void bb_error_msg(const char* format, ...) {
    va_list args;
    va_start(args, format);
    bb_vmessage(stderr, NULL, format, args);
    va_end(args);
}

void bb_error_msg_and_die(const char* format, ...) {
    va_list args;
    va_start(args, format);
    bb_vmessage(stderr, NULL, format, args);
    va_end(args);
    exit(1);
}

void bb_perror_msg(const char* format, ...) {
    va_list args;
    va_start(args, format);
    bb_vmessage(stderr, bb_error_name(errno), format, args);
    va_end(args);
}

void bb_perror_msg_and_die(const char* format, ...) {
    va_list args;
    va_start(args, format);
    bb_vmessage(stderr, bb_error_name(errno), format, args);
    va_end(args);
    exit(1);
}

unsigned getopt32(char** argv, const char* optstring, ...) {
    const char* short_opts = optstring != NULL ? optstring : "";
    unsigned flags = 0;
    va_list args;
    char** arg_outputs[16] = {};
    size_t arg_output_count = 0;

    if (short_opts[0] == '^') {
        short_opts += 1;
    }
    while (*short_opts != '\0') {
        if (short_opts[0] != ':' && short_opts[1] == ':') {
            if (arg_output_count >= ARRAY_SIZE(arg_outputs)) {
                bb_simple_error_msg_and_die("too many option arguments");
            }
            arg_outputs[arg_output_count++] = NULL;
        }
        short_opts += 1;
        if (*short_opts == ':') {
            short_opts += 1;
        }
    }

    va_start(args, optstring);
    for (size_t index = 0; index < arg_output_count; ++index) {
        arg_outputs[index] = va_arg(args, char**);
        if (arg_outputs[index] != NULL) {
            *arg_outputs[index] = NULL;
        }
    }
    va_end(args);

    optind = 1;
    optarg = NULL;
    while (argv[optind] != NULL) {
        char* current = argv[optind];
        if (current[0] != '-' || current[1] == '\0') {
            break;
        }
        if (strcmp(current, "--") == 0) {
            optind += 1;
            break;
        }

        for (size_t char_index = 1; current[char_index] != '\0'; ++char_index) {
            const char option = current[char_index];
            const int bit = next_option_bit(optstring != NULL && optstring[0] == '^' ? optstring + 1 : optstring, option);
            if (bit < 0) {
                bb_show_usage();
            }
            flags |= 1u << bit;

            if (option_requires_argument(optstring != NULL && optstring[0] == '^' ? optstring + 1 : optstring, option)) {
                char* value = NULL;
                size_t sink_index = 0;
                const char* cursor = optstring != NULL && optstring[0] == '^' ? optstring + 1 : optstring;
                for (size_t scan = 0; cursor[scan] != '\0'; ++scan) {
                    if (cursor[scan] == ':' || cursor[scan] == '^') {
                        continue;
                    }
                    if (cursor[scan] == option) {
                        break;
                    }
                    if (cursor[scan + 1] == ':') {
                        sink_index += 1;
                    }
                }

                if (current[char_index + 1] != '\0') {
                    value = &current[char_index + 1];
                    char_index = strlen(current) - 1;
                } else {
                    optind += 1;
                    if (argv[optind] == NULL) {
                        bb_show_usage();
                    }
                    value = argv[optind];
                }

                optarg = value;
                if (sink_index < arg_output_count && arg_outputs[sink_index] != NULL) {
                    *arg_outputs[sink_index] = value;
                }
                break;
            }
        }
        optind += 1;
    }

    return flags;
}

unsigned getopt32long(char** argv, const char* optstring, const char* longopts, ...) {
    (void)longopts;

    const char* forwarded = optstring;
    va_list args;
    char** outputs[16] = {};
    size_t output_count = 0;

    if (forwarded != NULL && forwarded[0] == '^') {
        forwarded += 1;
    }
    for (size_t index = 0; forwarded != NULL && forwarded[index] != '\0'; ++index) {
        if (forwarded[index] != ':' && forwarded[index + 1] == ':') {
            output_count += 1;
        }
    }

    va_start(args, longopts);
    for (size_t index = 0; index < output_count && index < ARRAY_SIZE(outputs); ++index) {
        outputs[index] = va_arg(args, char**);
    }
    va_end(args);

    va_start(args, longopts);
    unsigned result = getopt32(argv, optstring,
        output_count > 0 ? outputs[0] : NULL,
        output_count > 1 ? outputs[1] : NULL,
        output_count > 2 ? outputs[2] : NULL,
        output_count > 3 ? outputs[3] : NULL,
        output_count > 4 ? outputs[4] : NULL,
        output_count > 5 ? outputs[5] : NULL,
        output_count > 6 ? outputs[6] : NULL,
        output_count > 7 ? outputs[7] : NULL,
        output_count > 8 ? outputs[8] : NULL,
        output_count > 9 ? outputs[9] : NULL);
    va_end(args);
    return result;
}

char* safe_strncpy(char* destination, const char* source, size_t size) {
    size_t index = 0;
    if (size == 0) {
        return destination;
    }
    while (index + 1 < size && source[index] != '\0') {
        destination[index] = source[index];
        index += 1;
    }
    destination[index] = '\0';
    return destination;
}

char* is_prefixed_with(const char* string, const char* prefix) {
    while (*prefix != '\0') {
        if (*string != *prefix) {
            return 0;
        }
        ++string;
        ++prefix;
    }
    return (char*)string;
}

char* skip_whitespace(const char* text) {
    const unsigned char* cursor = (const unsigned char*)text;
    while (cursor != NULL && (*cursor == ' ' || *cursor == '\t' || *cursor == '\n'
        || *cursor == '\r' || *cursor == '\f' || *cursor == '\v')) {
        cursor += 1;
    }
    return (char*)cursor;
}

char* skip_non_whitespace(const char* text) {
    const unsigned char* cursor = (const unsigned char*)text;
    while (cursor != NULL && *cursor != '\0' && *cursor != ' ' && *cursor != '\t'
        && *cursor != '\n' && *cursor != '\r' && *cursor != '\f' && *cursor != '\v') {
        cursor += 1;
    }
    return (char*)cursor;
}

const char* endofname(const char* text) {
    const unsigned char* cursor = (const unsigned char*)text;
    if (cursor == NULL) {
        return text;
    }
    if (!(((*cursor >= 'a' && *cursor <= 'z') || (*cursor >= 'A' && *cursor <= 'Z')) || *cursor == '_')) {
        return text;
    }
    cursor += 1;
    while (((*cursor >= 'a' && *cursor <= 'z')
        || (*cursor >= 'A' && *cursor <= 'Z')
        || (*cursor >= '0' && *cursor <= '9')
        || *cursor == '_')) {
        cursor += 1;
    }
    return (const char*)cursor;
}

unsigned bb_strtou(const char* text, char** endptr, int base) {
    unsigned long value = strtoul(text, endptr, base);
    if (value > ~0u) {
        errno = ERANGE;
        return ~0u;
    }
    return (unsigned)value;
}

unsigned long bb_strtoul(const char* text, char** endptr, int base) {
    return strtoul(text, endptr, base);
}

unsigned long long bb_strtoull(const char* text, char** endptr, int base) {
    return (unsigned long long)strtoul(text, endptr, base);
}

unsigned long long monotonic_us(void) {
    struct timespec value = {};
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return 0;
    }
    return (unsigned long long)value.tv_sec * 1000000ull + (unsigned long long)(value.tv_nsec / 1000);
}

unsigned monotonic_ms(void) {
    return (unsigned)(monotonic_us() / 1000ull);
}

unsigned bb_clk_tck(void) {
    return 1000u;
}

char* utoa(unsigned value) {
    enum { SLOT_COUNT = 8, SLOT_CAPACITY = 16 };
    static char buffers[SLOT_COUNT][SLOT_CAPACITY];
    static unsigned next_slot = 0;
    char* slot = buffers[next_slot++ % SLOT_COUNT];
    char scratch[SLOT_CAPACITY];
    size_t length = 0;

    if (value == 0) {
        slot[0] = '0';
        slot[1] = '\0';
        return slot;
    }

    while (value != 0 && length + 1 < sizeof(scratch)) {
        scratch[length++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    for (size_t index = 0; index < length; ++index) {
        slot[index] = scratch[length - 1 - index];
    }
    slot[length] = '\0';
    return slot;
}

void xwrite(int fd, const void* buffer, size_t count) {
    if (full_write(fd, buffer, count) != (ssize_t)count) {
        bb_simple_perror_msg(bb_msg_write_error);
        exit(1);
    }
}

ssize_t nonblock_immune_read(int fd, void* buffer, size_t count) {
    return read(fd, buffer, count);
}

int fdprintf(int fd, const char* format, ...) {
    char buffer[1024];
    int written = 0;
    va_list args;
    va_start(args, format);
    written = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (written < 0) {
        return written;
    }
    if (full_write(fd, buffer, (size_t)((written < (int)sizeof(buffer)) ? written : (int)(sizeof(buffer) - 1))) < 0) {
        return -1;
    }
    return written;
}

void close_on_exec_on(int fd) {
    (void)fd;
}

void _exit_SUCCESS(void) {
    exit(0);
}

int sigprocmask_allsigs(int how) {
    sigset_t set = 0;
    sigfillset(&set);
    return sigprocmask(how, &set, NULL);
}

int sigprocmask2(int how, sigset_t* set) {
    return sigprocmask(how, set, set);
}

int sigaction_set(int signal_number, const struct sigaction* action) {
    return sigaction(signal_number, action, NULL);
}

void xfunc_die(void) {
    exit(1);
}

const char* get_signame(int signal_number) {
    switch (signal_number) {
        case SIGHUP: return "HUP";
        case SIGINT: return "INT";
        case SIGQUIT: return "QUIT";
        case SIGKILL: return "KILL";
        case SIGPIPE: return "PIPE";
        case SIGALRM: return "ALRM";
        case SIGTERM: return "TERM";
        case SIGCHLD: return "CHLD";
        case SIGTSTP: return "TSTP";
        case SIGTTIN: return "TTIN";
        case SIGTTOU: return "TTOU";
        default: return NULL;
    }
}

int get_signum(const char* name) {
    if (name == NULL || *name == '\0') {
        return -1;
    }
    if (strcmp(name, "HUP") == 0 || strcmp(name, "SIGHUP") == 0) return SIGHUP;
    if (strcmp(name, "INT") == 0 || strcmp(name, "SIGINT") == 0) return SIGINT;
    if (strcmp(name, "QUIT") == 0 || strcmp(name, "SIGQUIT") == 0) return SIGQUIT;
    if (strcmp(name, "KILL") == 0 || strcmp(name, "SIGKILL") == 0) return SIGKILL;
    if (strcmp(name, "PIPE") == 0 || strcmp(name, "SIGPIPE") == 0) return SIGPIPE;
    if (strcmp(name, "ALRM") == 0 || strcmp(name, "SIGALRM") == 0) return SIGALRM;
    if (strcmp(name, "TERM") == 0 || strcmp(name, "SIGTERM") == 0) return SIGTERM;
    if (strcmp(name, "CHLD") == 0 || strcmp(name, "SIGCHLD") == 0) return SIGCHLD;
    if (strcmp(name, "TSTP") == 0 || strcmp(name, "SIGTSTP") == 0) return SIGTSTP;
    if (strcmp(name, "TTIN") == 0 || strcmp(name, "SIGTTIN") == 0) return SIGTTIN;
    if (strcmp(name, "TTOU") == 0 || strcmp(name, "SIGTTOU") == 0) return SIGTTOU;
    return -1;
}

int bb_cat(char** argv) {
    char buffer[512];
    int status = 0;

    do {
        int fd = STDIN_FILENO;
        if (*argv != NULL && strcmp(*argv, "-") != 0) {
            fd = open(*argv, O_RDONLY);
            if (fd < 0) {
                bb_perror_msg("can't open '%s'", *argv);
                status = 1;
                continue;
            }
        }

        for (;;) {
            const ssize_t count = read(fd, buffer, sizeof(buffer));
            if (count < 0) {
                bb_perror_msg("can't read '%s'", *argv != NULL ? *argv : "-");
                status = 1;
                break;
            }
            if (count == 0) {
                break;
            }
            if (full_write(STDOUT_FILENO, buffer, (size_t)count) != count) {
                bb_simple_perror_msg(bb_msg_write_error);
                if (fd != STDIN_FILENO) {
                    close(fd);
                }
                return 1;
            }
        }

        if (fd != STDIN_FILENO) {
            close(fd);
        }
    } while (*++argv != NULL);

    return status;
}

mode_t bb_parse_mode(const char* text, mode_t base_mode) {
    char* end = NULL;
    unsigned long value = strtoul(text, &end, 8);
    (void)base_mode;
    if (text == NULL || *text == '\0' || end == NULL || *end != '\0') {
        return (mode_t)-1;
    }
    return (mode_t)value;
}

char* bb_get_last_path_component_strip(char* path) {
    size_t length = strlen(path);
    while (length > 1 && path[length - 1] == '/') {
        path[--length] = '\0';
    }

    char* last = path;
    for (char* cursor = path; *cursor != '\0'; ++cursor) {
        if (*cursor == '/') {
            last = cursor + 1;
        }
    }
    return last;
}

char* concat_path_file(const char* path, const char* filename) {
    const size_t left = strlen(path);
    const size_t right = strlen(filename);
    const int need_slash = (left != 0 && path[left - 1] != '/') ? 1 : 0;
    char* joined = (char*)xmalloc(left + need_slash + right + 1);
    memcpy(joined, path, left);
    if (need_slash) {
        joined[left] = '/';
    }
    memcpy(joined + left + need_slash, filename, right);
    joined[left + need_slash + right] = '\0';
    return joined;
}

static int mkdir_one(const char* path) {
    if (mkdir(path) == 0 || errno == EEXIST) {
        return 0;
    }
    return -1;
}

int bb_make_directory(char* path, long mode, int flags) {
    (void)mode;
    if ((flags & FILEUTILS_RECUR) == 0) {
        if (mkdir_one(path) == 0) {
            return 0;
        }
        bb_perror_msg("can't create directory '%s'", path);
        return -1;
    }

    char scratch[256] = {};
    size_t write = 0;
    for (size_t index = 0; path[index] != '\0' && write + 1 < sizeof(scratch); ++index) {
        scratch[write++] = path[index];
        scratch[write] = '\0';
        if (path[index] == '/' && write > 1) {
            if (mkdir_one(scratch) < 0 && errno != EEXIST) {
                bb_perror_msg("can't create directory '%s'", scratch);
                return -1;
            }
        }
    }
    if (mkdir_one(scratch) < 0 && errno != EEXIST) {
        bb_perror_msg("can't create directory '%s'", scratch);
        return -1;
    }
    return 0;
}

static int copy_regular_file(const char* source, const char* dest) {
    char buffer[512];
    int src = open(source, O_RDONLY);
    if (src < 0) {
        bb_perror_msg("can't open '%s'", source);
        return -1;
    }

    int dst = open(dest, O_WRONLY | O_CREAT | O_TRUNC);
    if (dst < 0) {
        bb_perror_msg("can't create '%s'", dest);
        close(src);
        return -1;
    }

    for (;;) {
        const ssize_t count = read(src, buffer, sizeof(buffer));
        if (count < 0) {
            bb_perror_msg("can't read '%s'", source);
            close(src);
            close(dst);
            return -1;
        }
        if (count == 0) {
            break;
        }
        if (full_write(dst, buffer, (size_t)count) != count) {
            bb_perror_msg("error writing to '%s'", dest);
            close(src);
            close(dst);
            return -1;
        }
    }

    close(src);
    close(dst);
    return 0;
}

static int copy_directory_recursive(const char* source, const char* dest, int flags) {
    DIR* directory = opendir(source);
    if (directory == NULL) {
        bb_perror_msg("can't open directory '%s'", source);
        return -1;
    }

    char mutable_dest[256] = {};
    strncpy(mutable_dest, dest, sizeof(mutable_dest) - 1);
    if (bb_make_directory(mutable_dest, -1, FILEUTILS_RECUR) != 0) {
        closedir(directory);
        return -1;
    }

    struct dirent* entry = NULL;
    while ((entry = readdir(directory)) != NULL) {
        if (DOT_OR_DOTDOT(entry->d_name)) {
            continue;
        }

        char* child_source = concat_path_file(source, entry->d_name);
        char* child_dest = concat_path_file(dest, entry->d_name);
        if (copy_file(child_source, child_dest, flags) != 0) {
            free(child_source);
            free(child_dest);
            closedir(directory);
            return -1;
        }
        free(child_source);
        free(child_dest);
    }

    closedir(directory);
    return 0;
}

int copy_file(const char* source, const char* dest, int flags) {
    struct stat source_info = {};
    struct stat dest_info = {};
    const int source_state = cp_mv_stat(source, &source_info);
    const int dest_state = cp_mv_stat(dest, &dest_info);
    const int destination_is_dir = (dest_state & 2) != 0;
    const char* actual_dest = dest;
    char* allocated_dest = NULL;

    if (source_state <= 0) {
        return -1;
    }

    if (destination_is_dir) {
        char source_copy[256] = {};
        strncpy(source_copy, source, sizeof(source_copy) - 1);
        allocated_dest = concat_path_file(dest, bb_get_last_path_component_strip(source_copy));
        actual_dest = allocated_dest;
    }

    if ((flags & FILEUTILS_NO_OVERWRITE) != 0 && cp_mv_stat(actual_dest, &dest_info) != 0) {
        free(allocated_dest);
        return 0;
    }

    int result = 0;
    if (S_ISDIR(source_info.st_mode)) {
        if ((flags & FILEUTILS_RECUR) == 0) {
            bb_error_msg("omitting directory '%s'", source);
            result = -1;
        } else {
            result = copy_directory_recursive(source, actual_dest, flags);
        }
    } else {
        result = copy_regular_file(source, actual_dest);
    }

    free(allocated_dest);
    return result;
}

static int remove_directory_recursive(const char* path, int flags) {
    DIR* directory = opendir(path);
    if (directory == NULL) {
        bb_perror_msg("can't open directory '%s'", path);
        return -1;
    }

    struct dirent* entry = NULL;
    while ((entry = readdir(directory)) != NULL) {
        if (DOT_OR_DOTDOT(entry->d_name)) {
            continue;
        }

        char* child = concat_path_file(path, entry->d_name);
        if (remove_file(child, flags) != 0) {
            free(child);
            closedir(directory);
            return -1;
        }
        free(child);
    }

    closedir(directory);
    if (rmdir(path) != 0) {
        bb_perror_msg("can't remove '%s'", path);
        return -1;
    }
    return 0;
}

int remove_file(const char* path, int flags) {
    struct stat info = {};
    if (lstat(path, &info) != 0) {
        if ((flags & FILEUTILS_FORCE) != 0 && errno == ENOENT) {
            return 0;
        }
        bb_perror_msg("can't stat '%s'", path);
        return -1;
    }

    if (S_ISDIR(info.st_mode)) {
        if ((flags & FILEUTILS_RECUR) == 0) {
            bb_perror_msg("can't remove '%s'", path);
            return -1;
        }
        return remove_directory_recursive(path, flags);
    }

    if (unlink(path) != 0) {
        bb_perror_msg("can't remove '%s'", path);
        return -1;
    }
    return 0;
}

int cp_mv_stat2(const char* path, struct stat* info, stat_func stat_fn) {
    if (stat_fn(path, info) != 0) {
        if (errno != ENOENT) {
            bb_perror_msg("can't stat '%s'", path);
            return -1;
        }
        return 0;
    }
    return S_ISDIR(info->st_mode) ? 3 : 1;
}

int cp_mv_stat(const char* path, struct stat* info) {
    return cp_mv_stat2(path, info, stat);
}

char* dirname(char* path) {
    size_t length = strlen(path);
    if (length == 0) {
        return path;
    }

    while (length > 1 && path[length - 1] == '/') {
        path[--length] = '\0';
    }

    while (length > 0 && path[length - 1] != '/') {
        path[--length] = '\0';
    }

    if (length == 0) {
        strcpy(path, ".");
        return path;
    }
    if (length == 1) {
        path[1] = '\0';
        return path;
    }

    path[length - 1] = '\0';
    return path;
}

int bb_process_escape_sequence(const char** text) {
    const char* cursor = *text;
    const int value = *cursor;
    if (*cursor != '\0') {
        cursor += 1;
    }
    *text = cursor;
    return value;
}
