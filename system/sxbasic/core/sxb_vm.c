/* SxBASIC core: the virtual machine. Its call stack is data it owns, not the
 * C stack, so a running procedure can stop and resume (sxb_vm_run's budget
 * today; ShowModal, DoEvents and the debugger later). */
#include "sxb_vm_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *sxb_error_text(int number)
{
    switch (number) {
    case SXB_ERR_INVALID_ARGUMENT: return "Invalid argument";
    case SXB_ERR_OVERFLOW: return "Overflow";
    case SXB_ERR_OUT_OF_MEMORY: return "Out of memory";
    case SXB_ERR_INDEX: return "Index out of range";
    case SXB_ERR_DIVISION_BY_ZERO: return "Division by zero";
    case SXB_ERR_TYPE_MISMATCH: return "Type mismatch";
    case SXB_ERR_STACK: return "Out of stack space";
    case SXB_ERR_FILE_NOT_FOUND: return "File not found";
    case SXB_ERR_IO: return "Input/output error";
    case SXB_ERR_PERMISSION: return "Permission denied";
    case SXB_ERR_PATH: return "Path not valid";
    case SXB_ERR_PROPERTY: return "Invalid property value";
    default: return "Application error";
    }
}

void sxb_vm_raise(struct sxb_vm *vm, int number, const char *description)
{
    if (vm->raised) {
        return;
    }
    vm->raised = 1;
    vm->raise_number = number;
    snprintf(vm->raise_description, sizeof(vm->raise_description), "%s",
             description && description[0] ? description : sxb_error_text(number));
}

/* ---- the machine ------------------------------------------------------------------ */

static void release_range(struct sxb_vm *vm, int from, int to)
{
    for (int i = from; i < to; i++) sxb_value_release(&vm->stack[i]);
}

static int enter(struct sxb_vm *vm, int proc, int return_pc)
{
    const struct sxb_proc_info *info = &vm->prog->procs[proc];
    if (vm->frame_count == SXB_MAX_FRAMES || vm->sp + info->local_count - info->param_count + 64 > SXB_STACK_VALUES) {
        sxb_vm_raise(vm, SXB_ERR_STACK, NULL);
        return -1;
    }
    struct sxb_frame *frame = &vm->frames[vm->frame_count++];
    frame->proc = proc;
    frame->return_pc = return_pc;
    frame->base = vm->sp - info->param_count;
    for (int i = info->param_count; i < info->local_count; i++) {
        vm->stack[vm->sp].kind = SXB_V_SCALAR;
        vm->stack[vm->sp].u.i = 0;
        vm->sp++;
    }
    vm->pc = info->entry;
    return 0;
}

static void leave(struct sxb_vm *vm)
{
    struct sxb_frame *frame = &vm->frames[vm->frame_count - 1];
    release_range(vm, frame->base, vm->sp);
    vm->sp = frame->base;
    while (vm->handler_count > 0 && vm->handlers[vm->handler_count - 1].frame >= vm->frame_count - 1) {
        vm->handler_count--;
    }
    vm->pc = frame->return_pc;
    vm->frame_count--;
}

struct sxb_vm *sxb_vm_create(const struct sxb_program *program, const struct sxb_host *host,
                             const char *arguments, const char *app_path)
{
    struct sxb_vm *vm = calloc(1, sizeof(*vm));
    if (vm == NULL) return NULL;
    vm->prog = program;
    vm->host = host;
    vm->stack = calloc(SXB_STACK_VALUES, sizeof(*vm->stack));
    vm->frames = calloc(SXB_MAX_FRAMES, sizeof(*vm->frames));
    vm->handlers = calloc(SXB_MAX_HANDLERS, sizeof(*vm->handlers));
    vm->globals = calloc((size_t)program->global_count + 1u, sizeof(*vm->globals));
    vm->arguments = malloc(strlen(arguments ? arguments : "") + 1u);
    vm->app_path = malloc(strlen(app_path ? app_path : "") + 1u);
    if (!vm->stack || !vm->frames || !vm->handlers || !vm->globals || !vm->arguments || !vm->app_path) {
        sxb_vm_destroy(vm);
        return NULL;
    }
    strcpy(vm->arguments, arguments ? arguments : "");
    strcpy(vm->app_path, app_path ? app_path : "");
    vm->start_ns = host->clock_ns ? host->clock_ns(host->user) : 0;
    vm->random_state = 0x2545F4914F6CDD1Dull;
    vm->status = SXB_RUNNING;
    enter(vm, program->entry_proc, -1);
    return vm;
}

void sxb_vm_destroy(struct sxb_vm *vm)
{
    if (vm == NULL) return;
    if (vm->stack) release_range(vm, 0, vm->sp);
    if (vm->globals) {
        for (int i = 0; i < vm->prog->global_count; i++) sxb_value_release(&vm->globals[i]);
    }
    free(vm->stack);
    free(vm->frames);
    free(vm->handlers);
    free(vm->globals);
    free(vm->arguments);
    free(vm->app_path);
    free(vm);
}

const struct sxb_runtime_error *sxb_vm_error(const struct sxb_vm *vm)
{
    return &vm->error;
}

/* Sends a raised error to the innermost Try, or ends the program with it. */
static void dispatch(struct sxb_vm *vm)
{
    vm->raised = 0;
    vm->err_number = vm->raise_number;
    snprintf(vm->err_description, sizeof(vm->err_description), "%s", vm->raise_description);
    if (vm->handler_count > 0) {
        struct sxb_handler h = vm->handlers[--vm->handler_count];
        release_range(vm, h.sp, vm->sp);
        vm->sp = h.sp;
        vm->frame_count = h.frame + 1;
        vm->pc = h.catch_pc;
        return;
    }
    const struct sxb_program *prog = vm->prog;
    int at = vm->fault_pc >= 0 && vm->fault_pc < prog->code_length ? vm->fault_pc : 0;
    vm->error.number = vm->raise_number;
    snprintf(vm->error.description, sizeof(vm->error.description), "%s", vm->raise_description);
    snprintf(vm->error.file, sizeof(vm->error.file), "%s",
             prog->code_length ? prog->module_files[prog->modules[at]] : "");
    vm->error.line = prog->code_length ? prog->lines[at] : 0;
    vm->status = SXB_ERROR;
}

static struct sxb_value *cell(struct sxb_vm *vm, uint32_t location)
{
    struct sxb_value *value = (location & 1u) ? &vm->globals[location >> 1]
                                              : &vm->stack[vm->frames[vm->frame_count - 1].base + (int)(location >> 1)];
    return value->kind == SXB_V_REF ? value->u.ref : value;
}

static struct sxb_value *raw_cell(struct sxb_vm *vm, uint32_t location)
{
    return (location & 1u) ? &vm->globals[location >> 1]
                           : &vm->stack[vm->frames[vm->frame_count - 1].base + (int)(location >> 1)];
}

static int compare(int op, int order)
{
    switch (op) {
    case SXB_OP_EQ: return order == 0;
    case SXB_OP_NE: return order != 0;
    case SXB_OP_LT: return order < 0;
    case SXB_OP_GT: return order > 0;
    case SXB_OP_LE: return order <= 0;
    default: return order >= 0;
    }
}

static void store(struct sxb_value *target, struct sxb_value value)
{
    sxb_value_release(target);
    *target = value;
}

static void push_int(struct sxb_vm *vm, int64_t value)
{
    struct sxb_value *slot = sxb_vm_push_slot(vm);
    if (slot) slot->u.i = value;
}

static void push_double(struct sxb_vm *vm, double value)
{
    struct sxb_value *slot = sxb_vm_push_slot(vm);
    if (slot) slot->u.d = value;
}

static void push_string(struct sxb_vm *vm, struct sxb_string *string)
{
    struct sxb_value *slot = sxb_vm_push_slot(vm);
    if (slot) {
        slot->kind = SXB_V_STRING;
        slot->u.s = string;
    } else {
        sxb_string_release(string);
    }
}

static void double_result(struct sxb_vm *vm, double value)
{
    if (!isfinite(value)) {
        sxb_vm_raise(vm, isnan(value) ? SXB_ERR_INVALID_ARGUMENT : SXB_ERR_OVERFLOW, NULL);
        value = 0;
    }
    push_double(vm, value);
}

static struct sxb_string *to_string(struct sxb_vm *vm, int type, const struct sxb_value *value)
{
    char text[40];
    switch (type) {
    case SXB_TY_INT: snprintf(text, sizeof(text), "%lld", (long long)value->u.i); break;
    case SXB_TY_DOUBLE: sxb_format_double(value->u.d, text); break;
    default: snprintf(text, sizeof(text), "%s", value->u.i ? "True" : "False"); break;
    }
    return sxb_vm_string(vm, text, strlen(text));
}

static void write_out(struct sxb_vm *vm, const char *text, size_t length)
{
    if (vm->host->write_out && length) vm->host->write_out(vm->host->user, text, length);
}

enum sxb_status sxb_vm_run(struct sxb_vm *vm, uint64_t max_steps)
{
    const uint32_t *code = vm->prog->code;
    uint64_t steps = 0;
    while (vm->status == SXB_RUNNING) {
        if (max_steps != 0 && steps++ >= max_steps) {
            return SXB_RUNNING;
        }
        vm->fault_pc = vm->pc;
        uint32_t op = code[vm->pc++];
        struct sxb_value a, b;
        switch (op) {
        case SXB_OP_NOP:
            break;
        case SXB_OP_PUSH_SMALL:
            push_int(vm, (int32_t)code[vm->pc++]);
            break;
        case SXB_OP_PUSH_CONST: {
            const struct sxb_constant *k = &vm->prog->constants[code[vm->pc++]];
            struct sxb_value *slot = sxb_vm_push_slot(vm);
            if (slot == NULL) break;
            if (k->type == SXB_TY_STRING) {
                slot->kind = SXB_V_STRING;
                slot->u.s = k->string;
                if (k->string) k->string->refs++;
            } else if (k->type == SXB_TY_DOUBLE) {
                slot->u.d = k->double_value;
            } else {
                slot->u.i = k->int_value;
            }
            break;
        }
        case SXB_OP_PUSH_DEFAULT: {
            uint32_t type = code[vm->pc++];
            struct sxb_value *slot = sxb_vm_push_slot(vm);
            if (slot && SXB_TY_IS_ARRAY(type)) slot->kind = SXB_V_ARRAY;
            else if (slot && type == SXB_TY_STRING) slot->kind = SXB_V_STRING;
            break;
        }
        case SXB_OP_LOAD: {
            struct sxb_value *source = cell(vm, code[vm->pc++]);
            struct sxb_value *slot = sxb_vm_push_slot(vm);
            if (slot) {
                *slot = *source;
                sxb_value_retain(slot);
            }
            break;
        }
        case SXB_OP_STORE:
            store(cell(vm, code[vm->pc++]), vm->stack[--vm->sp]);
            break;
        case SXB_OP_REF: {
            struct sxb_value *target = raw_cell(vm, code[vm->pc++]);
            struct sxb_value *slot = sxb_vm_push_slot(vm);
            if (slot) {
                slot->kind = SXB_V_REF;
                slot->u.ref = target->kind == SXB_V_REF ? target->u.ref : target;
            }
            break;
        }
        case SXB_OP_ALOAD: {
            int64_t index = vm->stack[--vm->sp].u.i;
            a = vm->stack[--vm->sp];
            struct sxb_array *array = a.u.a;
            if (array == NULL || index < 0 || index >= array->count) {
                sxb_value_release(&a);
                sxb_vm_raise(vm, SXB_ERR_INDEX, NULL);
                break;
            }
            struct sxb_value element = array->items[index];
            sxb_value_retain(&element);
            sxb_value_release(&a);
            vm->stack[vm->sp++] = element;
            break;
        }
        case SXB_OP_ASTORE: {
            struct sxb_value *target = cell(vm, code[vm->pc++]);
            b = vm->stack[--vm->sp];
            int64_t index = vm->stack[--vm->sp].u.i;
            struct sxb_array *array = sxb_array_writable(vm, target);
            if (array == NULL || index < 0 || index >= array->count) {
                sxb_value_release(&b);
                if (!vm->raised) sxb_vm_raise(vm, SXB_ERR_INDEX, NULL);
                break;
            }
            store(&array->items[index], b);
            break;
        }
        case SXB_OP_ANEW:
        case SXB_OP_REDIM: {
            uint32_t location = op == SXB_OP_REDIM ? code[vm->pc++] : 0;
            int element_type = (int)code[vm->pc++];
            int preserve = op == SXB_OP_REDIM ? (int)code[vm->pc++] : 0;
            int64_t upper = vm->stack[--vm->sp].u.i;
            if (upper < -1 || (op == SXB_OP_ANEW && upper < 0)) {
                sxb_vm_raise(vm, SXB_ERR_INDEX, "an array's upper bound cannot be negative");
                break;
            }
            if (upper + 1 > SXB_MAX_ARRAY) {
                sxb_vm_raise(vm, SXB_ERR_OUT_OF_MEMORY, NULL);
                break;
            }
            struct sxb_array *array = sxb_array_new(element_type, upper + 1);
            if (array == NULL) {
                sxb_vm_raise(vm, SXB_ERR_OUT_OF_MEMORY, NULL);
                break;
            }
            struct sxb_value fresh;
            fresh.kind = SXB_V_ARRAY;
            fresh.u.a = array;
            if (op == SXB_OP_ANEW) {
                vm->stack[vm->sp++] = fresh;
                break;
            }
            struct sxb_value *target = cell(vm, location);
            struct sxb_array *old = target->kind == SXB_V_ARRAY ? target->u.a : NULL;
            if (preserve && old != NULL) {
                for (int64_t i = 0; i < old->count && i < array->count; i++) {
                    store(&array->items[i], old->items[i]);
                    sxb_value_retain(&array->items[i]);
                }
            }
            store(target, fresh);
            break;
        }
        case SXB_OP_UBOUND:
            a = vm->stack[--vm->sp];
            push_int(vm, a.u.a ? a.u.a->count - 1 : -1);
            sxb_value_release(&a);
            break;
        case SXB_OP_POP:
            sxb_value_release(&vm->stack[--vm->sp]);
            break;
        case SXB_OP_ADDI:
        case SXB_OP_SUBI:
        case SXB_OP_MULI:
        case SXB_OP_IDIV:
        case SXB_OP_MODI: {
            int64_t y = vm->stack[--vm->sp].u.i;
            int64_t x = vm->stack[--vm->sp].u.i;
            int64_t r = 0;
            int overflow = 0;
            if (op == SXB_OP_ADDI) overflow = __builtin_add_overflow(x, y, &r);
            else if (op == SXB_OP_SUBI) overflow = __builtin_sub_overflow(x, y, &r);
            else if (op == SXB_OP_MULI) overflow = __builtin_mul_overflow(x, y, &r);
            else if (y == 0) { sxb_vm_raise(vm, SXB_ERR_DIVISION_BY_ZERO, NULL); break; }
            else if (x == INT64_MIN && y == -1) overflow = 1;
            else r = op == SXB_OP_IDIV ? x / y : x % y;
            if (overflow) { sxb_vm_raise(vm, SXB_ERR_OVERFLOW, NULL); break; }
            push_int(vm, r);
            break;
        }
        case SXB_OP_NEGI: {
            int64_t x = vm->stack[vm->sp - 1].u.i;
            if (x == INT64_MIN) { sxb_vm_raise(vm, SXB_ERR_OVERFLOW, NULL); break; }
            vm->stack[vm->sp - 1].u.i = -x;
            break;
        }
        case SXB_OP_ADDD:
        case SXB_OP_SUBD:
        case SXB_OP_MULD:
        case SXB_OP_DIVD:
        case SXB_OP_POWD: {
            double y = vm->stack[--vm->sp].u.d;
            double x = vm->stack[--vm->sp].u.d;
            if ((op == SXB_OP_DIVD && y == 0.0) || (op == SXB_OP_POWD && x == 0.0 && y < 0.0)) {
                sxb_vm_raise(vm, SXB_ERR_DIVISION_BY_ZERO, NULL);
                break;
            }
            double r = op == SXB_OP_ADDD ? x + y : op == SXB_OP_SUBD ? x - y : op == SXB_OP_MULD ? x * y
                     : op == SXB_OP_DIVD ? x / y : pow(x, y);
            double_result(vm, r);
            break;
        }
        case SXB_OP_NEGD:
            vm->stack[vm->sp - 1].u.d = -vm->stack[vm->sp - 1].u.d;
            break;
        case SXB_OP_I2D:
            vm->stack[vm->sp - 1].u.d = (double)vm->stack[vm->sp - 1].u.i;
            break;
        case SXB_OP_I2D2:
            vm->stack[vm->sp - 2].u.d = (double)vm->stack[vm->sp - 2].u.i;
            break;
        case SXB_OP_CMPI:
        case SXB_OP_CMPD:
        case SXB_OP_CMPS:
        case SXB_OP_CMPB: {
            int cmp_op = (int)code[vm->pc++];
            b = vm->stack[--vm->sp];
            a = vm->stack[--vm->sp];
            int order;
            if (op == SXB_OP_CMPI) order = a.u.i < b.u.i ? -1 : a.u.i > b.u.i;
            else if (op == SXB_OP_CMPD) order = a.u.d < b.u.d ? -1 : a.u.d > b.u.d;
            else if (op == SXB_OP_CMPB) order = (a.u.i != 0) != (b.u.i != 0);
            else {
                size_t la = sxb_str_length(a.u.s), lb = sxb_str_length(b.u.s);
                size_t n = la < lb ? la : lb;
                order = n ? memcmp(sxb_str_data(a.u.s), sxb_str_data(b.u.s), n) : 0;
                if (order == 0) order = la < lb ? -1 : la > lb;
                sxb_value_release(&a);
                sxb_value_release(&b);
            }
            push_int(vm, compare(cmp_op, order));
            break;
        }
        case SXB_OP_NOTB:
            vm->stack[vm->sp - 1].u.i = !vm->stack[vm->sp - 1].u.i;
            break;
        case SXB_OP_XORB: {
            int64_t y = vm->stack[--vm->sp].u.i;
            vm->stack[vm->sp - 1].u.i = (vm->stack[vm->sp - 1].u.i != 0) != (y != 0);
            break;
        }
        case SXB_OP_TOSTR: {
            int type = (int)code[vm->pc++];
            a = vm->stack[--vm->sp];
            push_string(vm, to_string(vm, type, &a));
            break;
        }
        case SXB_OP_CONCAT: {
            b = vm->stack[--vm->sp];
            a = vm->stack[--vm->sp];
            if (b.u.s == NULL) { vm->stack[vm->sp++] = a; break; }
            if (a.u.s == NULL) { vm->stack[vm->sp++] = b; break; }
            size_t la = a.u.s->length, lb = b.u.s->length;
            struct sxb_string *joined = malloc(sizeof(*joined) + la + lb + 1u);
            if (joined == NULL) {
                sxb_value_release(&a);
                sxb_value_release(&b);
                sxb_vm_raise(vm, SXB_ERR_OUT_OF_MEMORY, NULL);
                break;
            }
            joined->refs = 1;
            joined->length = la + lb;
            memcpy(joined->data, a.u.s->data, la);
            memcpy(joined->data + la, b.u.s->data, lb);
            joined->data[la + lb] = '\0';
            sxb_value_release(&a);
            sxb_value_release(&b);
            push_string(vm, joined);
            break;
        }
        case SXB_OP_JMP:
            vm->pc = (int)code[vm->pc];
            break;
        case SXB_OP_JF:
        case SXB_OP_JT: {
            int target = (int)code[vm->pc++];
            int64_t condition = vm->stack[--vm->sp].u.i;
            if ((condition != 0) == (op == SXB_OP_JT)) vm->pc = target;
            break;
        }
        case SXB_OP_CALL: {
            int proc = (int)code[vm->pc++];
            enter(vm, proc, vm->pc);
            break;
        }
        case SXB_OP_RET:
            leave(vm);
            break;
        case SXB_OP_RETV:
            a = vm->stack[--vm->sp];
            leave(vm);
            vm->stack[vm->sp++] = a;
            break;
        case SXB_OP_BUILTIN: {
            int id = (int)code[vm->pc++];
            int argc = (int)code[vm->pc++];
            sxb_builtin(vm, id, argc);
            break;
        }
        case SXB_OP_TRY: {
            int target = (int)code[vm->pc++];
            if (vm->handler_count == SXB_MAX_HANDLERS) {
                sxb_vm_raise(vm, SXB_ERR_STACK, NULL);
                break;
            }
            struct sxb_handler *h = &vm->handlers[vm->handler_count++];
            h->frame = vm->frame_count - 1;
            h->sp = vm->sp;
            h->catch_pc = target;
            break;
        }
        case SXB_OP_ENDTRY:
            if (vm->handler_count > 0) vm->handler_count--;
            break;
        case SXB_OP_RAISE: {
            b = vm->stack[--vm->sp];
            int64_t number = vm->stack[--vm->sp].u.i;
            char description[128];
            snprintf(description, sizeof(description), "%s", sxb_str_data(b.u.s));
            sxb_value_release(&b);
            sxb_vm_raise(vm, number < 0 || number > 2147483647 ? SXB_ERR_INVALID_ARGUMENT : (int)number, description);
            break;
        }
        case SXB_OP_ERRNUM:
            push_int(vm, vm->err_number);
            break;
        case SXB_OP_ERRDESC:
            push_string(vm, sxb_vm_string(vm, vm->err_description, strlen(vm->err_description)));
            break;
        case SXB_OP_FORTESTI:
        case SXB_OP_FORTESTD: {
            struct sxb_value *var = cell(vm, code[vm->pc]);
            struct sxb_value *end = cell(vm, code[vm->pc + 1]);
            struct sxb_value *step = cell(vm, code[vm->pc + 2]);
            vm->pc += 3;
            int go;
            if (op == SXB_OP_FORTESTI) go = step->u.i >= 0 ? var->u.i <= end->u.i : var->u.i >= end->u.i;
            else go = step->u.d >= 0 ? var->u.d <= end->u.d : var->u.d >= end->u.d;
            push_int(vm, go);
            break;
        }
        case SXB_OP_STEPCHECK: {
            struct sxb_value *step = cell(vm, code[vm->pc]);
            int type = (int)code[vm->pc + 1];
            vm->pc += 2;
            if (type == SXB_TY_INT ? step->u.i == 0 : step->u.d == 0.0) {
                sxb_vm_raise(vm, SXB_ERR_INVALID_ARGUMENT, "a For loop's Step cannot be 0");
            }
            break;
        }
        case SXB_OP_PRINT: {
            int newline = (int)code[vm->pc++];
            a = vm->stack[--vm->sp];
            write_out(vm, sxb_str_data(a.u.s), sxb_str_length(a.u.s));
            if (newline) write_out(vm, "\n", 1);
            sxb_value_release(&a);
            break;
        }
        case SXB_OP_END:
            vm->status = SXB_DONE;
            break;
        default:
            sxb_vm_raise(vm, SXB_ERR_INVALID_ARGUMENT, "the program is damaged (unknown instruction)");
            break;
        }
        if (vm->raised) {
            dispatch(vm);
        }
    }
    return (enum sxb_status)vm->status;
}
