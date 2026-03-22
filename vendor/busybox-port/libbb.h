#pragma once

#include <savanxp/libc.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define FAST_FUNC
#define ALWAYS_INLINE inline __attribute__((always_inline))
#define NOINLINE __attribute__((noinline))
#define NORETURN __attribute__((noreturn))
#define MAIN_EXTERNALLY_VISIBLE
#define EXTERNALLY_VISIBLE
#define UNUSED_PARAM __attribute__((unused))
#define ALIGN1 __attribute__((aligned(1)))
#define ALIGN2 __attribute__((aligned(2)))
#define ALIGN_PTR __attribute__((aligned(sizeof(void*))))
#define RETURNS_MALLOC
#define PUSH_AND_SET_FUNCTION_VISIBILITY_TO_HIDDEN
#define POP_SAVED_FUNCTION_VISIBILITY
#define BB_GLOBAL_CONST

#define ENABLE_DESKTOP 0
#define ENABLE_FTPD 0
#define ENABLE_LONG_OPTS 0
#define ENABLE_LOCALE_SUPPORT 0
#define ENABLE_SELINUX 0
#define ENABLE_FEATURE_CLEAN_UP 0
#define ENABLE_FEATURE_CP_LONG_OPTIONS 0
#define ENABLE_FEATURE_CP_REFLINK 0
#define ENABLE_FEATURE_FANCY_ECHO 0
#define ENABLE_FEATURE_CATN 0
#define ENABLE_FEATURE_CATV 0
#define ENABLE_FEATURE_VERBOSE 0
#define ENABLE_FEATURE_LS_FILETYPES 0
#define ENABLE_FEATURE_LS_RECURSIVE 0
#define ENABLE_FEATURE_LS_TIMESTAMPS 0
#define ENABLE_FEATURE_LS_SORTFILES 0
#define ENABLE_FEATURE_LS_FOLLOWLINKS 0
#define ENABLE_FEATURE_HUMAN_READABLE 0
#define ENABLE_FEATURE_LS_WIDTH 0
#define ENABLE_FEATURE_LS_COLOR 0
#define ENABLE_FEATURE_LS_COLOR_IS_DEFAULT 0
#define ENABLE_FEATURE_LS_USERNAME 0
#define ENABLE_FEATURE_SHOW_THREADS 0
#define ENABLE_FEATURE_PS_TIME 0
#define ENABLE_FEATURE_PS_LONG 0
#define ENABLE_FEATURE_PS_WIDE 0
#define ENABLE_FEATURE_PS_ADDITIONAL_COLUMNS 0
#define ENABLE_UNICODE_SUPPORT 0
#define ENABLE_UNICODE_USING_LOCALE 0
#define ENABLE_FEATURE_CHECK_UNICODE_IN_ENV 0
#define ENABLE_ASH 1
#define ENABLE_SH_IS_ASH 1
#define ENABLE_BASH_IS_ASH 0
#define ENABLE_ASH_OPTIMIZE_FOR_SIZE 1
#define ENABLE_ASH_INTERNAL_GLOB 1
#define ENABLE_ASH_BASH_COMPAT 0
#define ENABLE_ASH_BASH_SOURCE_CURDIR 0
#define ENABLE_ASH_BASH_NOT_FOUND_HOOK 0
#define ENABLE_ASH_JOB_CONTROL 0
#define ENABLE_ASH_ALIAS 0
#define ENABLE_ASH_RANDOM_SUPPORT 0
#define ENABLE_ASH_EXPAND_PRMT 0
#define ENABLE_ASH_IDLE_TIMEOUT 0
#define ENABLE_ASH_MAIL 0
#define ENABLE_ASH_ECHO 1
#define ENABLE_ASH_PRINTF 0
#define ENABLE_ASH_TEST 0
#define ENABLE_ASH_HELP 0
#define ENABLE_ASH_GETOPTS 0
#define ENABLE_ASH_CMDCMD 1
#define ENABLE_ASH_SLEEP 0
#define ENABLE_FEATURE_EDITING 0
#define CONFIG_FEATURE_EDITING_MAX_LEN 1024
#define ENABLE_FEATURE_EDITING_FANCY_PROMPT 0
#define ENABLE_FEATURE_EDITING_SAVEHISTORY 0
#define ENABLE_FEATURE_EDITING_SAVE_ON_EXIT 0
#define ENABLE_FEATURE_EDITING_VI 0
#define ENABLE_FEATURE_TAB_COMPLETION 0
#define ENABLE_FEATURE_SH_MATH 0
#define ENABLE_FEATURE_SH_MATH_64 0
#define ENABLE_FEATURE_SH_MATH_BASE 0
#define ENABLE_FEATURE_SH_STANDALONE 0
#define ENABLE_FEATURE_SH_NOFORK 0
#define ENABLE_FEATURE_SH_READ_FRAC 0
#define ENABLE_FEATURE_SH_HISTFILESIZE 0
#define ENABLE_FEATURE_SH_EMBEDDED_SCRIPTS 0
#define ENABLE_FEATURE_SH_EXTRA_QUIET 1
#define HAVE_DEV_FD 0
#define BB_MMU 1

#define IF_SELINUX(...)
#define IF_FEATURE_VERBOSE(...)
#define IF_FEATURE_CATN(...)
#define IF_FEATURE_CATV(...)
#define IF_LONG_OPTS(...)
#define IF_ASH(...) __VA_ARGS__
#define IF_SH_IS_ASH(...) __VA_ARGS__
#define IF_BASH_IS_ASH(...)
#define IF_ASH_BASH_COMPAT(...)
#define IF_ASH_EXPAND_PRMT(...)
#define IF_ASH_HELP(...)
#define IF_FEATURE_SH_MATH(...)
#define IF_FEATURE_SH_STANDALONE(...)
#define IF_NOT_FEATURE_SH_STANDALONE(...) __VA_ARGS__
#define IF_NOT_ASH_OPTIMIZE_FOR_SIZE(...)

#define BUILD_BUG_ON(condition) typedef char bb_build_bug_on_##__LINE__[(condition) ? -1 : 1]
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define barrier() asm volatile ("" ::: "memory")
#define STRERROR_FMT "%s"
#define STRERROR_ERRNO , strerror(errno)

#define No_argument "\x00"
#define Required_argument "\x01"
#define Optional_argument "\x02"

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#ifndef EXDEV
#define EXDEV 18
#endif

#define FILEUTILS_PRESERVE_STATUS (1 << 0)
#define FILEUTILS_DEREFERENCE (1 << 1)
#define FILEUTILS_RECUR (1 << 2)
#define FILEUTILS_FORCE (1 << 3)
#define FILEUTILS_INTERACTIVE (1 << 4)
#define FILEUTILS_NO_OVERWRITE (1 << 5)
#define FILEUTILS_MAKE_HARDLINK (1 << 6)
#define FILEUTILS_MAKE_SOFTLINK (1 << 7)
#define FILEUTILS_DEREF_SOFTLINK (1 << 8)
#define FILEUTILS_DEREFERENCE_L0 (1 << 9)
#define FILEUTILS_VERBOSE 0
#define FILEUTILS_UPDATE (1 << 14)
#define FILEUTILS_NO_TARGET_DIR (1 << 15)
#define FILEUTILS_TARGET_DIR (1 << 16)
#define FILEUTILS_CP_OPTBITS 18
#define FILEUTILS_RMDEST (1 << 19)
#define FILEUTILS_REFLINK (1 << 20)
#define FILEUTILS_REFLINK_ALWAYS (1 << 21)
#define FILEUTILS_IGNORE_CHMOD_ERR (1u << 31)
#define FILEUTILS_CP_OPTSTR "pdRfinlsLHarPvuTt:"
#define bb_dev_null "/dev/null"
#define DEV_FD_PREFIX "/dev/fd/"

#define DOT_OR_DOTDOT(name) \
    ((name)[0] == '.' && ((name)[1] == '\0' || ((name)[1] == '.' && (name)[2] == '\0')))
#define LONE_DASH(name) ((name)[0] == '-' && (name)[1] == '\0')

typedef signed char smallint;
typedef unsigned char smalluint;

extern const char* applet_name;
extern int optind;
extern char* optarg;
extern const char bb_msg_write_error[];
extern const char bb_msg_memory_exhausted[];
extern const char bb_msg_requires_arg[];
extern const char bb_default_path[];
extern const char bb_PATH_root_path[];
extern const char bb_busybox_exec_path[];
extern const char bb_banner[];
extern char bb_common_bufsiz1[4096];
extern volatile sig_atomic_t bb_got_signal;
extern char** environ;

int echo_main(int argc, char** argv) FAST_FUNC;
char* bb_basename(const char* path) FAST_FUNC;
char* bb_stpcpy(char* destination, const char* source) FAST_FUNC;
void* xmalloc(size_t size) FAST_FUNC;
void* xzalloc(size_t size) FAST_FUNC;
void* xrealloc(void* pointer, size_t size) FAST_FUNC;
void* xmemdup(const void* source, size_t size) FAST_FUNC;
char* xstrdup(const char* text) FAST_FUNC;
ssize_t full_write(int fd, const void* buffer, size_t count) FAST_FUNC;
ssize_t safe_write(int fd, const void* buffer, size_t count) FAST_FUNC;
int bb_ask_y_confirmation(void) FAST_FUNC;
void bb_show_usage(void) NORETURN FAST_FUNC;
void bb_simple_error_msg(const char* text) FAST_FUNC;
void bb_simple_error_msg_and_die(const char* text) NORETURN FAST_FUNC;
void bb_simple_perror_msg(const char* text) FAST_FUNC;
void bb_error_msg(const char* format, ...) FAST_FUNC;
void bb_error_msg_and_die(const char* format, ...) NORETURN FAST_FUNC;
void bb_perror_msg(const char* format, ...) FAST_FUNC;
void bb_perror_msg_and_die(const char* format, ...) NORETURN FAST_FUNC;
unsigned getopt32(char** argv, const char* optstring, ...) FAST_FUNC;
unsigned getopt32long(char** argv, const char* optstring, const char* longopts, ...) FAST_FUNC;
int bb_cat(char** argv) FAST_FUNC;
mode_t bb_parse_mode(const char* text, mode_t base_mode) FAST_FUNC;
char* bb_get_last_path_component_strip(char* path) FAST_FUNC;
char* safe_strncpy(char* destination, const char* source, size_t size) FAST_FUNC;
char* concat_path_file(const char* path, const char* filename) FAST_FUNC;
char* is_prefixed_with(const char* string, const char* prefix) FAST_FUNC;
char* skip_whitespace(const char* text) FAST_FUNC;
char* skip_non_whitespace(const char* text) FAST_FUNC;
int bb_make_directory(char* path, long mode, int flags) FAST_FUNC;
int remove_file(const char* path, int flags) FAST_FUNC;
int copy_file(const char* source, const char* dest, int flags) FAST_FUNC;
typedef int (*stat_func)(const char* path, struct stat* info);
int cp_mv_stat2(const char* path, struct stat* info, stat_func stat_fn) FAST_FUNC;
int cp_mv_stat(const char* path, struct stat* info) FAST_FUNC;
char* dirname(char* path) FAST_FUNC;
int bb_process_escape_sequence(const char** text) FAST_FUNC;
const char* endofname(const char* text) FAST_FUNC;
unsigned bb_strtou(const char* text, char** endptr, int base) FAST_FUNC;
unsigned long bb_strtoul(const char* text, char** endptr, int base) FAST_FUNC;
unsigned long long bb_strtoull(const char* text, char** endptr, int base) FAST_FUNC;
unsigned monotonic_ms(void) FAST_FUNC;
unsigned long long monotonic_us(void) FAST_FUNC;
unsigned bb_clk_tck(void) FAST_FUNC;
char* utoa(unsigned value) FAST_FUNC;
void fflush_all(void) FAST_FUNC;
void xwrite(int fd, const void* buffer, size_t count) FAST_FUNC;
ssize_t nonblock_immune_read(int fd, void* buffer, size_t count) FAST_FUNC;
int fdprintf(int fd, const char* format, ...) FAST_FUNC;
void close_on_exec_on(int fd) FAST_FUNC;
void _exit_SUCCESS(void) NORETURN FAST_FUNC;
int sigprocmask_allsigs(int how) FAST_FUNC;
int sigprocmask2(int how, sigset_t* set) FAST_FUNC;
int sigaction_set(int signal_number, const struct sigaction* action) FAST_FUNC;
int find_applet_by_name(const char* name) FAST_FUNC;
void run_applet_no_and_exit(int applet_no, const char* name, char** argv) NORETURN FAST_FUNC;
void xfunc_die(void) NORETURN FAST_FUNC;
const char* get_signame(int signal_number) FAST_FUNC;
int get_signum(const char* name) FAST_FUNC;

#define ASSIGN_CONST_PTR(pptr, value) do { \
    *(void**)(pptr) = (void*)(value); \
    barrier(); \
} while (0)
#define XZALLOC_CONST_PTR(pptr, size) ASSIGN_CONST_PTR((pptr), xzalloc(size))
#define run_noexec_applet_and_exit(applet_no, name, argv) run_applet_no_and_exit((applet_no), (name), (argv))
#define bb_unreachable(statement) do { statement; __builtin_unreachable(); } while (0)

#define stpcpy bb_stpcpy
