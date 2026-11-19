/* SxBASIC core: the virtual machine's state, shared by the interpreter
 * (sxb_vm.c), the builtins (sxb_builtins.c) and the values (sxb_value.c). */
#ifndef SXB_VM_INTERNAL_H
#define SXB_VM_INTERNAL_H

#include "sxb_internal.h"

#define SXB_STACK_VALUES 65536
#define SXB_MAX_FRAMES 4096
#define SXB_MAX_HANDLERS 1024
#define SXB_MAX_ARRAY 16777216   /* elements */

struct sxb_frame {
    int proc;
    int return_pc;
    int base;
    int boundary;        /* an invocation starts here: returning from it ends the run */
};

#define SXB_MAX_WAITING 16

struct sxb_waiting {
    int floor;           /* frame count once the waiting invocation returns */
    int result_type;     /* what sxb_vm_resume pushes, 0 for nothing */
    int pc;              /* where it goes on */
};

struct sxb_handler {
    int frame;
    int sp;
    int catch_pc;
};

struct sxb_vm {
    const struct sxb_program *prog;
    const struct sxb_host *host;
    struct sxb_value *stack;
    int sp;
    struct sxb_frame *frames;
    int frame_count;
    struct sxb_handler *handlers;
    int handler_count;
    struct sxb_value *globals;
    int pc;
    int fault_pc;        /* where the instruction running now starts */
    int status;
    /* a raised error, until the loop dispatches it */
    int raised;
    int raise_number;
    char raise_description[128];
    /* Err */
    int err_number;
    char err_description[128];
    struct sxb_runtime_error error;
    char *arguments;
    char *app_path;
    uint64_t start_ns;
    uint64_t random_state;
    /* forms */
    const struct sxb_form_host *forms;
    int floor;           /* the running invocation's floor */
    int runs;            /* runs nested in each other through the forms runtime */
    int suspend_requested;
    int suspend_type;
    struct sxb_waiting waiting[SXB_MAX_WAITING];
    int waiting_count;
    struct sxb_value scratch; /* Form_Unload's Cancel */
};

/* Asks the running procedure to wait for sxb_vm_resume, which will push a
 * value of `result_type` (0 for none). */
void sxb_vm_request_suspend(struct sxb_vm *vm, int result_type);
/* Pushes a value crossing from the forms runtime; strings are copied. */
void sxb_vm_push_scalar(struct sxb_vm *vm, int type, const struct sxb_scalar *value);

/* Raises a runtime error; NULL takes the standard description. */
void sxb_vm_raise(struct sxb_vm *vm, int number, const char *description);
const char *sxb_error_text(int number);

/* Values. A string or array value owns one reference. */
void sxb_value_retain(const struct sxb_value *value);
void sxb_value_release(struct sxb_value *value);
struct sxb_array *sxb_array_new(int element_type, int64_t count);
void sxb_array_release(struct sxb_array *array);
/* The array in `cell`, unshared so it can be written; NULL when empty. */
struct sxb_array *sxb_array_writable(struct sxb_vm *vm, struct sxb_value *cell);

static inline const char *sxb_str_data(const struct sxb_string *s)
{
    return s ? s->data : "";
}

static inline size_t sxb_str_length(const struct sxb_string *s)
{
    return s ? s->length : 0;
}

/* A new string, NULL for "" (or after raising error 7). */
struct sxb_string *sxb_vm_string(struct sxb_vm *vm, const char *data, size_t length);

/* Code points in UTF-8 text, and the byte where code point `index` starts. */
int64_t sxb_utf8_count(const char *text, size_t length);
size_t sxb_utf8_offset(const char *text, size_t length, int64_t index);

/* Parses text as a number the way CInt, CDbl and IsNumeric read it.
 * Returns 1 and sets *is_integer and the value, or 0. */
int sxb_parse_number(const char *text, size_t length, int *is_integer, int64_t *integer, double *real);

void sxb_builtin(struct sxb_vm *vm, int id, int argc);

static inline struct sxb_value *sxb_vm_push_slot(struct sxb_vm *vm)
{
    if (vm->sp >= SXB_STACK_VALUES) {
        sxb_vm_raise(vm, SXB_ERR_STACK, NULL);
        return NULL;
    }
    struct sxb_value *slot = &vm->stack[vm->sp++];
    slot->kind = SXB_V_SCALAR;
    slot->u.i = 0;
    return slot;
}

#endif
