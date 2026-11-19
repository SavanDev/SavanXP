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

static int enter(struct sxb_vm *vm, int proc, int return_pc, int boundary)
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
    frame->boundary = boundary;
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
    enter(vm, program->entry_proc, -1, 1);
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
    free(vm->breakpoints);
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
    if (vm->handler_count > 0 && vm->handlers[vm->handler_count - 1].frame >= vm->floor) {
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
    /* A debugger sees the program as the error left it, before it ends. */
    if (vm->debug != NULL) (void)vm->debug->stop(vm->debug->user, vm, SXB_STOP_ERROR);
    vm->status = SXB_ERROR;
}

/* Before a statement: whether the debugger wants to stop here, and why. */
static int debug_reason(struct sxb_vm *vm)
{
    if (vm->pause_requested) return SXB_STOP_PAUSE;
    if (vm->breakpoints != NULL && vm->breakpoints[vm->pc]) return SXB_STOP_BREAKPOINT;
    switch (vm->debug_mode) {
    case SXB_DEBUG_STEP_INTO: return SXB_STOP_STEP;
    case SXB_DEBUG_STEP_OVER: return vm->frame_count <= vm->debug_depth ? SXB_STOP_STEP : 0;
    case SXB_DEBUG_STEP_OUT: return vm->frame_count < vm->debug_depth ? SXB_STOP_STEP : 0;
    default: return 0;
    }
}

/* Called once per statement, just before its first instruction runs: the
 * statement it stopped before is never checked again. */
static void debug_check(struct sxb_vm *vm)
{
    int reason = debug_reason(vm);
    if (reason == 0) return;
    vm->pause_requested = 0;
    vm->fault_pc = vm->pc;
    vm->debug_mode = vm->debug->stop(vm->debug->user, vm, reason);
    vm->debug_depth = vm->frame_count;
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

static void form_ops(struct sxb_vm *vm, uint32_t op);

/* Runs until the invocation whose floor is `floor` returns (SXB_IDLE), waits,
 * the program ends, or the budget runs out. */
static enum sxb_status run(struct sxb_vm *vm, int floor, uint64_t max_steps)
{
    const uint32_t *code = vm->prog->code;
    uint64_t steps = 0;
    int saved_floor = vm->floor;
    vm->floor = floor;
    vm->runs++;
    vm->status = SXB_RUNNING;
    while (vm->status == SXB_RUNNING) {
        if (vm->frame_count == floor) {
            vm->status = vm->waiting_count > 0 ? SXB_SUSPENDED : SXB_IDLE;
            break;
        }
        if (max_steps != 0 && steps++ >= max_steps) {
            break;
        }
        if (vm->debug != NULL && vm->prog->statements[vm->pc]) debug_check(vm);
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
            enter(vm, proc, vm->pc, 0);
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
        case SXB_OP_FORMLOAD:
        case SXB_OP_PGET:
        case SXB_OP_PSET:
        case SXB_OP_CALLM:
        case SXB_OP_UNLOAD:
            form_ops(vm, op);
            break;
        default:
            sxb_vm_raise(vm, SXB_ERR_INVALID_ARGUMENT, "the program is damaged (unknown instruction)");
            break;
        }
        if (vm->suspend_requested) {
            vm->suspend_requested = 0;
            if (vm->runs > 1 || vm->waiting_count == SXB_MAX_WAITING) {
                sxb_vm_raise(vm, SXB_ERR_INVALID_ARGUMENT,
                             "ShowModal, MsgBox and InputBox cannot wait inside Form_Load or Form_Unload");
            } else if (vm->status == SXB_RUNNING) {
                struct sxb_waiting *w = &vm->waiting[vm->waiting_count++];
                w->floor = floor;
                w->result_type = vm->suspend_type;
                w->pc = vm->pc;
                vm->status = SXB_SUSPENDED;
            }
        }
        if (vm->raised) {
            dispatch(vm);
        }
    }
    vm->runs--;
    vm->floor = saved_floor;
    return (enum sxb_status)vm->status;
}

enum sxb_status sxb_vm_run(struct sxb_vm *vm, uint64_t max_steps)
{
    if (vm->status == SXB_DONE || vm->status == SXB_ERROR) return (enum sxb_status)vm->status;
    return run(vm, 0, max_steps);
}

void sxb_vm_set_form_host(struct sxb_vm *vm, const struct sxb_form_host *host)
{
    vm->forms = host;
}

enum sxb_status sxb_vm_invoke(struct sxb_vm *vm, int proc, int *cancel)
{
    if (vm->status == SXB_DONE || vm->status == SXB_ERROR) return (enum sxb_status)vm->status;
    if (proc < 0 || proc >= vm->prog->proc_count) return (enum sxb_status)vm->status;
    int saved_status = vm->status;
    int saved_pc = vm->pc;
    if (cancel != NULL) {
        vm->scratch.kind = SXB_V_SCALAR;
        vm->scratch.u.i = *cancel != 0;
        struct sxb_value *slot = sxb_vm_push_slot(vm);
        if (slot) {
            slot->kind = SXB_V_REF;
            slot->u.ref = &vm->scratch;
        }
    }
    if (vm->raised || enter(vm, proc, saved_pc, 1) != 0) {
        vm->raised = 0;
        vm->error.number = SXB_ERR_STACK;
        snprintf(vm->error.description, sizeof(vm->error.description), "%s", sxb_error_text(SXB_ERR_STACK));
        vm->status = SXB_ERROR;
        return SXB_ERROR;
    }
    enum sxb_status status = run(vm, vm->frame_count - 1, 0);
    if (cancel != NULL) *cancel = vm->scratch.u.i != 0;
    if (status == SXB_IDLE && vm->runs > 0) {
        /* Back to the instruction that invoked it, which goes on running. */
        vm->status = saved_status == SXB_RUNNING ? SXB_RUNNING : status;
    }
    return status;
}

enum sxb_status sxb_vm_resume(struct sxb_vm *vm, const struct sxb_scalar *result)
{
    if (vm->waiting_count == 0 || vm->runs > 0) return (enum sxb_status)vm->status;
    struct sxb_waiting w = vm->waiting[--vm->waiting_count];
    vm->pc = w.pc;
    if (w.result_type != 0) {
        struct sxb_scalar none;
        memset(&none, 0, sizeof(none));
        sxb_vm_push_scalar(vm, w.result_type, result ? result : &none);
    }
    return run(vm, w.floor, 0);
}

int sxb_vm_waiting(const struct sxb_vm *vm)
{
    return vm->waiting_count;
}

void sxb_vm_request_suspend(struct sxb_vm *vm, int result_type)
{
    vm->suspend_requested = 1;
    vm->suspend_type = result_type;
}

void sxb_vm_push_scalar(struct sxb_vm *vm, int type, const struct sxb_scalar *value)
{
    struct sxb_value *slot = sxb_vm_push_slot(vm);
    if (slot == NULL) return;
    switch (type) {
    case SXB_TY_STRING:
        slot->kind = SXB_V_STRING;
        slot->u.s = value->s ? sxb_vm_string(vm, value->s, value->length) : NULL;
        break;
    case SXB_TY_DOUBLE:
        slot->u.d = value->type == SXB_TY_INT ? (double)value->i : value->d;
        break;
    case SXB_TY_BOOL:
        slot->u.i = value->i != 0;
        break;
    default:
        slot->u.i = value->type == SXB_TY_DOUBLE ? (int64_t)value->d : value->i;
        break;
    }
}

void sxb_vm_reset_form(struct sxb_vm *vm, int form)
{
    const struct sxb_program *prog = vm->prog;
    if (form < 0 || form >= prog->form_count) return;
    int module = prog->form_modules[form];
    int first = prog->module_globals[module][0];
    for (int i = 0; i < prog->module_globals[module][1]; i++) sxb_value_release(&vm->globals[first + i]);
}

/* A value off the stack as a scalar for the forms runtime; strings point
 * into `keep`, which the caller releases after the call. */
static void pop_scalar(struct sxb_vm *vm, int type, struct sxb_scalar *out, struct sxb_value *keep)
{
    *keep = vm->stack[--vm->sp];
    memset(out, 0, sizeof(*out));
    out->type = type;
    if (type == SXB_TY_STRING) {
        out->s = sxb_str_data(keep->u.s);
        out->length = sxb_str_length(keep->u.s);
    } else if (type == SXB_TY_DOUBLE) {
        out->d = keep->u.d;
    } else {
        out->i = keep->u.i;
    }
}

static void host_result(struct sxb_vm *vm, int error, int result_type)
{
    if (error == SXB_HOST_SUSPEND) {
        sxb_vm_request_suspend(vm, result_type);
    } else if (error > 0) {
        sxb_vm_raise(vm, error, NULL);
    }
}

static void form_ops(struct sxb_vm *vm, uint32_t op)
{
    const uint32_t *code = vm->prog->code;
    const struct sxb_form_host *forms = vm->forms;
    int form = (int)code[vm->pc++];
    if (op == SXB_OP_FORMLOAD || op == SXB_OP_UNLOAD) {
        if (forms == NULL) return;
        host_result(vm, op == SXB_OP_FORMLOAD ? forms->load(forms->user, form) : forms->unload(forms->user, form), 0);
        return;
    }
    int control = (int)code[vm->pc++];
    int member = (int)code[vm->pc++];
    struct sxb_scalar value;
    struct sxb_value keep;
    memset(&value, 0, sizeof(value));
    keep.kind = SXB_V_SCALAR;
    if (op == SXB_OP_PGET) {
        int type = (int)code[vm->pc++];
        int error = forms ? forms->get(forms->user, form, control, member, &value) : SXB_ERR_PROPERTY;
        if (error > 0) {
            sxb_vm_raise(vm, error, NULL);
            return;
        }
        sxb_vm_push_scalar(vm, type, &value);
        return;
    }
    if (op == SXB_OP_PSET) {
        int type = (int)code[vm->pc++];
        pop_scalar(vm, type, &value, &keep);
        int error = forms ? forms->set(forms->user, form, control, member, &value) : SXB_ERR_PROPERTY;
        sxb_value_release(&keep);
        host_result(vm, error, 0);
        return;
    }
    int argc = (int)code[vm->pc++];
    int result_type = (int)code[vm->pc++];
    struct sxb_scalar argument, result;
    memset(&argument, 0, sizeof(argument));
    memset(&result, 0, sizeof(result));
    if (argc == 1) pop_scalar(vm, sxb_method_arg(member), &argument, &keep);
    int error = forms ? forms->call(forms->user, form, control, member, &argument, argc, &result) : SXB_ERR_PROPERTY;
    sxb_value_release(&keep);
    host_result(vm, error, result_type);
    if (error == 0 && result_type != 0) sxb_vm_push_scalar(vm, result_type, &result);
}

/* ---- debugging ----------------------------------------------------------------- */

int sxb_vm_set_debug(struct sxb_vm *vm, const struct sxb_debug_host *debug, int mode)
{
    if (vm->prog->statements == NULL || vm->prog->code_length == 0) return -1;
    if (vm->breakpoints == NULL) {
        vm->breakpoints = calloc((size_t)vm->prog->code_length, 1);
        if (vm->breakpoints == NULL) return -1;
    }
    vm->debug = debug;
    vm->debug_mode = mode;
    vm->debug_depth = vm->frame_count;
    return 0;
}

static int module_named(const struct sxb_program *prog, const char *file)
{
    const char *base = strrchr(file, '/');
    base = base ? base + 1 : file;
    for (int m = 0; m < prog->module_count; m++) {
        const char *name = prog->module_files[m];
        const char *name_base = strrchr(name, '/');
        if (strcmp(name, file) == 0 || strcmp(name_base ? name_base + 1 : name, base) == 0) return m;
    }
    return -1;
}

int sxb_vm_set_breakpoint(struct sxb_vm *vm, const char *file, int line, int on)
{
    const struct sxb_program *prog = vm->prog;
    int module = module_named(prog, file), count = 0;
    if (prog->statements == NULL || prog->code_length == 0 || module < 0) return 0;
    /* Set before sxb_vm_set_debug as well as after. */
    if (vm->breakpoints == NULL && (vm->breakpoints = calloc((size_t)prog->code_length, 1)) == NULL) return 0;
    for (int pc = 0; pc < prog->code_length; pc++) {
        if (prog->statements[pc] && prog->modules[pc] == module && prog->lines[pc] == line) {
            vm->breakpoints[pc] = (uint8_t)(on != 0);
            count++;
        }
    }
    return count;
}

void sxb_vm_pause(struct sxb_vm *vm)
{
    vm->pause_requested = 1;
}

void sxb_vm_where(const struct sxb_vm *vm, const char **file, int *line, const char **proc)
{
    const struct sxb_program *prog = vm->prog;
    int at = vm->fault_pc >= 0 && vm->fault_pc < prog->code_length ? vm->fault_pc : 0;
    if (file) *file = prog->code_length ? prog->module_files[prog->modules[at]] : "";
    if (line) *line = prog->code_length ? prog->lines[at] : 0;
    if (proc) *proc = vm->frame_count > 0 ? prog->procs[vm->frames[vm->frame_count - 1].proc].name : "";
}

static void format_value(const struct sxb_value *value, int type, char *out, size_t size)
{
    char number[32];
    if (value->kind == SXB_V_REF) value = value->u.ref;
    if (SXB_TY_IS_ARRAY(type)) {
        snprintf(out, size, "(%lld items)", value->u.a ? (long long)value->u.a->count : 0LL);
        return;
    }
    switch (type) {
    case SXB_TY_INT: snprintf(out, size, "%lld", (long long)value->u.i); break;
    case SXB_TY_DOUBLE:
        sxb_format_double(value->u.d, number);
        snprintf(out, size, "%s", number);
        break;
    case SXB_TY_BOOL: snprintf(out, size, "%s", value->u.i ? "True" : "False"); break;
    case SXB_TY_STRING: snprintf(out, size, "\"%s\"", sxb_str_data(value->kind == SXB_V_STRING ? value->u.s : NULL)); break;
    default: snprintf(out, size, "(%s)", sxb_type_name(type)); break;
    }
}

/* The procedure it stopped in, or -1. */
static int stopped_proc(const struct sxb_vm *vm)
{
    return vm->frame_count > 0 ? vm->frames[vm->frame_count - 1].proc : -1;
}

static const struct sxb_value *local_cell(const struct sxb_vm *vm, const struct sxb_debug_symbol *symbol)
{
    int at = vm->frames[vm->frame_count - 1].base + symbol->slot;
    return at >= 0 && at < vm->sp ? &vm->stack[at] : NULL;
}

int sxb_vm_local(const struct sxb_vm *vm, int index, const char **name, char *value, size_t size)
{
    const struct sxb_program *prog = vm->prog;
    int proc = stopped_proc(vm);
    for (int i = 0; i < prog->symbol_count && proc >= 0; i++) {
        const struct sxb_debug_symbol *symbol = &prog->symbols[i];
        if (symbol->proc != proc || index-- > 0) continue;
        const struct sxb_value *cell = local_cell(vm, symbol);
        if (name) *name = symbol->name;
        if (cell) format_value(cell, symbol->type, value, size);
        else snprintf(value, size, "(not yet)");
        return 0;
    }
    return -1;
}

int sxb_vm_watch(const struct sxb_vm *vm, const char *name, char *value, size_t size)
{
    const struct sxb_program *prog = vm->prog;
    int proc = stopped_proc(vm), length = (int)strlen(name);
    int module = proc >= 0 ? prog->procs[proc].module : 0;
    /* Its own variables first, then its module's, then the project's Public ones. */
    for (int pass = 0; pass < 3; pass++) {
        for (int i = 0; i < prog->symbol_count; i++) {
            const struct sxb_debug_symbol *symbol = &prog->symbols[i];
            if (!sxb_names_equal(symbol->name, (int)strlen(symbol->name), name, length)) continue;
            if (pass == 0 && (symbol->proc != proc || proc < 0)) continue;
            if (pass == 1 && (symbol->proc != -1 || symbol->module != module)) continue;
            if (pass == 2 && (symbol->proc != -1 || !symbol->is_public)) continue;
            const struct sxb_value *cell = pass == 0 ? local_cell(vm, symbol) : &vm->globals[symbol->slot];
            if (cell == NULL) continue;
            format_value(cell, symbol->type, value, size);
            return 0;
        }
    }
    return -1;
}
