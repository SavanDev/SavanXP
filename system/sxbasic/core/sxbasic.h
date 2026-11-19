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

/* The project's Name, and whether it is a Windows program (one with forms). */
const char *sxb_program_name(const struct sxb_program *program);
int sxb_program_is_windows(const struct sxb_program *program);

/* What the project file says about the made executable: its .sxmeta and
 * .sxicon (docs/userland/SXBASIC.md#the-project-file). Empty strings for
 * keys the project leaves out. */
#define SXB_MAX_ICONS 4
struct sxb_project_info {
    char description[256];
    char vendor[256];
    char copyright[256];
    char category[64];
    char version[32];
    int has_accent;
    uint32_t accent;               /* 0x00RRGGBB */
    char icons[SXB_MAX_ICONS][128]; /* PNG files, relative to the project file */
    int icon_count;
};
const struct sxb_project_info *sxb_program_info(const struct sxb_program *program);

/* ---- the program image ---------------------------------------------------------
 *
 * A compiled program as bytes: what Make SXE puts in a made executable's
 * .sxbasic section, so the executable runs without compiling
 * (docs/userland/SXBASIC_IMAGE.md). The project info stays out: it goes into
 * .sxmeta and .sxicon. */

/* A malloc'd image of `program` in *data. 0, or -1 when out of memory. */
int sxb_program_save(const struct sxb_program *program, unsigned char **data, size_t *size);
/* The program in an image. NULL with `diag` filled in when the image is not
 * one this version reads. */
struct sxb_program *sxb_program_load(const void *data, size_t size, struct sxb_diagnostic *diag);

/* ---- the made executable --------------------------------------------------------
 *
 * Call-free like the rest of the core: the caller reads the stub and writes
 * the result. */

#define SXB_IMAGE_SECTION ".sxbasic"

/* The section `name` of an ELF64 file held in memory. 1 and its bytes, or 0. */
int sxb_elf_section(const void *elf, size_t size, const char *name, const unsigned char **data, size_t *length);

struct sxb_elf_add {
    const char *name;
    const void *data;
    size_t size;
};

/* A copy of the ELF64 file `elf` in a malloc'd *out with these sections added
 * as non-allocated; a section already there by that name is replaced. 0, or
 * -1 with `message` when `elf` is not an ELF64 file this can extend. */
int sxb_elf_add_sections(const void *elf, size_t size, const struct sxb_elf_add *sections, int count,
                         unsigned char **out, size_t *out_size, char message[128]);

/* The .sxmeta blob for a program with this name and info, in a malloc'd *out.
 * 0, or -1 with `message`. */
int sxb_exe_meta(const char *name, const struct sxb_project_info *info, unsigned char **out, size_t *size,
                 char message[128]);

/* The .sxicon blob for these square BGRA8888 images (sides 16 and 32 at
 * least), in a malloc'd *out. 0, or -1 with `message`. */
struct sxb_icon_image {
    int side;
    const uint32_t *pixels;
};
int sxb_exe_icons(const struct sxb_icon_image *images, int count, unsigned char **out, size_t *size,
                  char message[128]);

/* Reading a made executable's own presentation back from its sections:
 * ACCENT from .sxmeta, and a side x side icon from .sxicon copied into
 * `pixels` (side * side of them). 1 when found. */
int sxb_exe_meta_accent(const unsigned char *meta, size_t size, uint32_t *accent);
int sxb_exe_icon(const unsigned char *icons, size_t size, int side, uint32_t *pixels);

enum sxb_status {
    SXB_RUNNING = 0, /* the step budget ran out; call sxb_vm_run again */
    SXB_DONE,        /* the program ended (End, or Sub Main returned) */
    SXB_ERROR,       /* a runtime error nothing caught ended it */
    SXB_IDLE,        /* a Windows program waits for its next event */
    SXB_SUSPENDED    /* a procedure waits in ShowModal, MsgBox or InputBox */
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

/* ---- forms (docs/userland/SXBASIC.md#forms) ----------------------------------------- */

enum sxb_control_kind {
    SXB_CTL_FORM = 0, SXB_CTL_LABEL, SXB_CTL_BUTTON, SXB_CTL_CHECKBOX, SXB_CTL_RADIO, SXB_CTL_TEXTBOX,
    SXB_CTL_LISTBOX, SXB_CTL_COMBOBOX, SXB_CTL_GROUPBOX, SXB_CTL_SCROLLBAR, SXB_CTL_TRACKBAR,
    SXB_CTL_PROGRESS, SXB_CTL_TIMER, SXB_CTL_MENU, SXB_CTL_MENUITEM, SXB_CTL_COUNT
};

enum sxb_event {
    SXB_EV_CLICK = 0, SXB_EV_DBLCLICK, SXB_EV_CHANGE, SXB_EV_TICK, SXB_EV_LOAD, SXB_EV_UNLOAD, SXB_EV_RESIZE,
    SXB_EV_COUNT
};

enum sxb_property {
    SXB_P_TEXT = 0, SXB_P_LEFT, SXB_P_TOP, SXB_P_WIDTH, SXB_P_HEIGHT, SXB_P_VISIBLE, SXB_P_ENABLED,
    SXB_P_VALUE, SXB_P_MIN, SXB_P_MAX, SXB_P_LARGECHANGE, SXB_P_LISTCOUNT, SXB_P_LISTINDEX, SXB_P_INTERVAL,
    SXB_P_CHECKED, SXB_P_GROUP, SXB_P_MULTILINE, SXB_P_HORIZONTAL, SXB_P_DEFAULT, SXB_P_CANCEL,
    SXB_P_RESIZABLE, SXB_P_COUNT
};

enum sxb_method {
    SXB_M_SHOW = 0, SXB_M_SHOWMODAL, SXB_M_HIDE, SXB_M_SETFOCUS, SXB_M_ADDITEM, SXB_M_REMOVEITEM,
    SXB_M_CLEAR, SXB_M_LIST, SXB_M_COUNT
};

/* A control as its form file sets it; `values` holds every property but
 * Text, in the property's own type (Booleans as 0 and 1). */
struct sxb_control_def {
    char name[64];
    int kind;
    int menu;                   /* a MenuItem's Menu, as a control index; else -1 */
    char text[256];
    int64_t values[SXB_P_COUNT];
    int handlers[SXB_EV_COUNT]; /* procedure index, -1 for none */
};

struct sxb_form_def {
    char name[64];
    struct sxb_control_def form; /* the form's own properties and events (kind SXB_CTL_FORM) */
    struct sxb_control_def *controls;
    int control_count;
};

int sxb_program_form_count(const struct sxb_program *program);
const struct sxb_form_def *sxb_program_form(const struct sxb_program *program, int form);
/* The form a Windows program starts with, or -1 when it starts at Sub Main. */
int sxb_program_startup_form(const struct sxb_program *program);
/* The procedure that gives a form's variables their initial values; the
 * forms runtime invokes it when the form loads, before Form_Load. */
int sxb_program_form_init(const struct sxb_program *program, int form);
const char *sxb_property_name(int property);
const char *sxb_control_kind_name(int kind);

/* A value crossing to the forms runtime. A string belongs to whoever made
 * it and lives until that side's next call. */
struct sxb_scalar {
    int type;           /* 1 Integer, 2 Double, 3 String, 4 Boolean, 0 none */
    int64_t i;
    double d;
    const char *s;
    size_t length;
};

/* What a call into the forms runtime returns: 0, an error number to raise, or
 * SXB_HOST_SUSPEND to make the running procedure wait for sxb_vm_resume. */
#define SXB_HOST_SUSPEND (-1)

struct sxb_form_host {
    void *user;
    int (*load)(void *user, int form);
    int (*unload)(void *user, int form);
    /* `control` is -1 for the form itself. */
    int (*get)(void *user, int form, int control, int property, struct sxb_scalar *out);
    int (*set)(void *user, int form, int control, int property, const struct sxb_scalar *value);
    int (*call)(void *user, int form, int control, int method, const struct sxb_scalar *args, int argc,
                struct sxb_scalar *result);
    int (*message)(void *user, const char *text, int style, const char *title);
    int (*input)(void *user, const char *prompt, const char *title, const char *initial);
};

void sxb_vm_set_form_host(struct sxb_vm *vm, const struct sxb_form_host *host);

/* Runs an event's procedure until it returns (SXB_IDLE), waits
 * (SXB_SUSPENDED), or the program ends. `cancel`, when not NULL, is the
 * ByRef Boolean of Form_Unload, read back on return. */
enum sxb_status sxb_vm_invoke(struct sxb_vm *vm, int proc, int *cancel);

/* Hands the procedure that waits last its result (NULL when it expects
 * none) and runs it on. */
enum sxb_status sxb_vm_resume(struct sxb_vm *vm, const struct sxb_scalar *result);

/* How many procedures wait, one inside the other. */
int sxb_vm_waiting(const struct sxb_vm *vm);

/* An unloaded form's variables go back to their defaults. */
void sxb_vm_reset_form(struct sxb_vm *vm, int form);

/* The text CStr gives a Double: at most 15 significant digits, '.' as the
 * decimal point, an exponent outside 1E-04..1E+15. `out` holds 32 bytes. */
void sxb_format_double(double value, char out[32]);

#ifdef __cplusplus
}
#endif

#endif
