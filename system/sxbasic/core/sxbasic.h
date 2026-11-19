/* SxBASIC, the core: compiler and virtual machine for Savan BASIC
 * (docs/userland/SXBASIC.md, docs/roadmaps/open/SXBASIC_ROADMAP.md).
 *
 * The core makes no system call. What it needs from the system (files, the
 * standard streams, the clock) comes through struct sxb_host, so the same
 * sources build into the SUN runner and into the host test. It uses the C
 * library's memory, string and math functions, which both have.
 *
 * Use: sxb_compile_project (or sxb_compile_source for one module), then
 * sxb_vm_create and sxb_vm_run until it stops returning SXB_RUNNING. */
#ifndef SXBASIC_H
#define SXBASIC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Runtime error numbers (docs/userland/SXBASIC.md#errors). The file
 * callbacks of the host answer with these too. */
enum sxb_error_number {
    SXB_ERR_INVALID_ARGUMENT = 5,
    SXB_ERR_OVERFLOW = 6,
    SXB_ERR_OUT_OF_MEMORY = 7,
    SXB_ERR_INDEX = 9,
    SXB_ERR_DIVISION_BY_ZERO = 11,
    SXB_ERR_TYPE_MISMATCH = 13,
    SXB_ERR_STACK = 28,
    SXB_ERR_FILE_NOT_FOUND = 53,
    SXB_ERR_IO = 57,
    SXB_ERR_PERMISSION = 70,
    SXB_ERR_PATH = 75,
    SXB_ERR_PROPERTY = 380
};

struct sxb_host {
    void *user;
    /* Writes program output (Print) to standard output. */
    void (*write_out)(void *user, const char *text, size_t length);
    /* Reads a whole file into a buffer from malloc, which the core frees.
     * Returns 0, or an error number of the file group (53, 57, 70, 75). */
    int (*read_file)(void *user, const char *path, char **data, size_t *size);
    /* Replaces a file's contents. 0 or an error number of the file group. */
    int (*write_file)(void *user, const char *path, const char *data, size_t size);
    /* 1 if a regular file exists at the path, else 0. */
    int (*file_exists)(void *user, const char *path);
    /* A line of standard input without its end, in a buffer from malloc, or
     * -1 at the end of the input. Returns the length. */
    long (*read_line)(void *user, char **line);
    /* A monotonic clock, in nanoseconds. */
    uint64_t (*clock_ns)(void *user);
};

/* A compile error, or the reason a project could not be read. */
struct sxb_diagnostic {
    char file[128];
    int line;    /* 0 when it is about the whole file */
    int column;
    char message[256];
};

struct sxb_program;
struct sxb_vm;

/* Reads a project file (.sbp) and its modules through host->read_file and
 * compiles them. `project_path` is absolute. NULL on failure, with `diag`
 * filled in. */
struct sxb_program *sxb_compile_project(const char *project_path, const struct sxb_host *host,
                                        struct sxb_diagnostic *diag);

/* Compiles one module as a console program starting at its Sub Main. For
 * tests and tools; `name` is the module's file name in messages. */
struct sxb_program *sxb_compile_source(const char *name, const char *text, size_t length,
                                       struct sxb_diagnostic *diag);

void sxb_program_free(struct sxb_program *program);

/* The project's Name, and whether it is a Windows program (which has forms,
 * and which this core does not run yet). */
const char *sxb_program_name(const struct sxb_program *program);
int sxb_program_is_windows(const struct sxb_program *program);

enum sxb_status {
    SXB_RUNNING = 0, /* the step budget ran out; call sxb_vm_run again */
    SXB_DONE,        /* the program ended */
    SXB_ERROR        /* a runtime error nothing caught ended it */
};

struct sxb_runtime_error {
    int number;
    char description[128];
    char file[128];
    int line;
};

/* `arguments` is what Command() returns; `app_path` what AppPath() returns.
 * Both are copied. The program must outlive the VM. */
struct sxb_vm *sxb_vm_create(const struct sxb_program *program, const struct sxb_host *host,
                             const char *arguments, const char *app_path);

/* Runs at most `max_steps` instructions (0 = until it stops). */
enum sxb_status sxb_vm_run(struct sxb_vm *vm, uint64_t max_steps);

/* What ended the program when sxb_vm_run returned SXB_ERROR. */
const struct sxb_runtime_error *sxb_vm_error(const struct sxb_vm *vm);

void sxb_vm_destroy(struct sxb_vm *vm);

/* The text CStr gives a Double: at most 15 significant digits, '.' as the
 * decimal point, an exponent outside 1E-04..1E+15. `out` holds 32 bytes. */
void sxb_format_double(double value, char out[32]);

#ifdef __cplusplus
}
#endif

#endif
