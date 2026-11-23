/* Microsoft x86 C++ exception tables use 32-bit pointers and EBP funclets. */
#include "exception.h"

#define CXX_EXCEPTION 0xe06d7363u
#define CXX_MAGIC_VC6 0x19930520u
#define CXX_MAGIC_VC8 0x19930522u
#define CXX_SYNCHRONOUS 1u
#define CXX_NOEXCEPT 4u
#define CXX_CONST 1u
#define CXX_VOLATILE 2u
#define CXX_REFERENCE 8u
#define TYPE_SIMPLE 1u
#define TYPE_REFERENCE_ONLY 2u
#define TYPE_VIRTUAL_BASE 4u

typedef struct cxx_type_name {
    void *vtable;
    char *undecorated;
    char mangled[];
} cxx_type_name;

typedef struct cxx_type {
    unsigned int flags;
    const cxx_type_name *name;
    int member_offset;
    int vbptr_offset;
    int vbtable_offset;
    unsigned int size;
    void *copy;
} cxx_type;

typedef struct cxx_type_table {
    unsigned int count;
    const cxx_type *types[];
} cxx_type_table;

typedef struct cxx_throw_info {
    unsigned int flags;
    void (__thiscall *destroy)(void *);
    void *forward_handler;
    const cxx_type_table *table;
} cxx_throw_info;

typedef struct cxx_catch {
    unsigned int flags;
    const cxx_type_name *name;
    int object_offset;
    seh_funclet body;
} cxx_catch;

typedef struct cxx_try {
    int first_state;
    int last_state;
    int last_catch_state;
    unsigned int catch_count;
    const cxx_catch *catches;
} cxx_try;

typedef struct cxx_unwind {
    int parent;
    seh_funclet cleanup;
} cxx_unwind;

typedef struct cxx_function {
    unsigned int magic;
    unsigned int unwind_count;
    const cxx_unwind *unwind;
    unsigned int try_count;
    const cxx_try *tries;
    unsigned int ip_count;
    const void *ip_map;
    const void *expected_types;
    unsigned int flags;
} cxx_function;

typedef struct cxx_frame {
    seh_frame registration;
    int state;
    DWORD base;
} cxx_frame;

typedef struct active_catch {
    seh_frame registration;
    cxx_frame *frame;
    const cxx_function *function;
    const cxx_try *block;
    EXCEPTION_RECORD *record;
    void *stack;
} active_catch;

void __cdecl __declspec(noreturn) abort(void);
int __cdecl strcmp(const char *left, const char *right);
void *__cdecl memmove(void *destination, const void *source, unsigned int size);

static BOOL is_cxx(const EXCEPTION_RECORD *record) {
    return record->ExceptionCode == CXX_EXCEPTION && record->NumberParameters == 3 &&
           record->ExceptionInformation[0] >= CXX_MAGIC_VC6 &&
           record->ExceptionInformation[0] <= CXX_MAGIC_VC8 && record->ExceptionInformation[2] != 0;
}

static const cxx_throw_info *throw_info(const EXCEPTION_RECORD *record) {
    return is_cxx(record) ? (const cxx_throw_info *)record->ExceptionInformation[2] : NULL;
}

static void *adjust_object(const cxx_type *type, void *object) {
    BYTE *base = object;
    if (base != NULL) {
        if (type->vbptr_offset >= 0) {
            base += type->vbptr_offset;
            const BYTE *vbtable = *(const BYTE **)base;
            base += *(const int *)(vbtable + type->vbtable_offset);
        }
        base += type->member_offset;
    }
    return base;
}

static const cxx_type *matches(const cxx_catch *handler, const cxx_throw_info *info) {
    const unsigned int qualifiers = CXX_CONST | CXX_VOLATILE;
    if (info == NULL || (info->flags & qualifiers & ~handler->flags) != 0) {
        return NULL;
    }
    const cxx_type_table *table = info->table;
    for (unsigned int i = 0; i < table->count; ++i) {
        const cxx_type *type = table->types[i];
        if ((type->flags & TYPE_REFERENCE_ONLY) != 0 && (handler->flags & CXX_REFERENCE) == 0) {
            continue;
        }
        if (type->name == handler->name || strcmp(type->name->mangled, handler->name->mangled) == 0) {
            return type;
        }
    }
    return NULL;
}

static void bind_object(const cxx_catch *handler, const cxx_type *type, cxx_frame *frame, void *object) {
    if (type == NULL || handler->object_offset == 0) {
        return;
    }
    void *slot = (BYTE *)&frame->base + handler->object_offset;
    if ((handler->flags & CXX_REFERENCE) != 0) {
        *(void **)slot = adjust_object(type, object);
    } else if ((type->flags & TYPE_SIMPLE) != 0) {
        memmove(slot, object, type->size);
        if (type->size == sizeof(void *) && (type->member_offset != 0 || type->vbptr_offset >= 0)) {
            *(void **)slot = adjust_object(type, *(void **)slot);
        }
    } else {
        void *source = adjust_object(type, object);
        if (type->copy == NULL) {
            memmove(slot, source, type->size);
        } else if ((type->flags & TYPE_VIRTUAL_BASE) != 0) {
            ((void (__thiscall *)(void *, void *, int))type->copy)(slot, source, 1);
        } else {
            ((void (__thiscall *)(void *, void *))type->copy)(slot, source);
        }
    }
}

static void unwind_to(cxx_frame *frame, const cxx_function *function, int target) {
    while (frame->state != target) {
        const int state = frame->state;
        if (state < 0 || (unsigned int)state >= function->unwind_count) {
            abort();
        }
        const cxx_unwind *entry = &function->unwind[state];
        if (entry->parent >= state || entry->parent < -1) {
            abort();
        }
        frame->state = entry->parent;
        if (entry->cleanup != NULL) {
            seh_call_funclet(entry->cleanup, &frame->base, NULL);
        }
    }
}

static void destroy_exception(const EXCEPTION_RECORD *record) {
    const cxx_throw_info *info = throw_info(record);
    if (info != NULL && info->destroy != NULL) {
        info->destroy((void *)record->ExceptionInformation[1]);
    }
}

static BOOL same_exception(const EXCEPTION_RECORD *a, const EXCEPTION_RECORD *b) {
    return is_cxx(a) && is_cxx(b) && a->ExceptionInformation[1] == b->ExceptionInformation[1] &&
           a->ExceptionInformation[2] == b->ExceptionInformation[2];
}

/* The compiler keeps the ESP to restore one word before the registration. */
static void **stack_slot(cxx_frame *frame) {
    return (void **)((BYTE *)frame - sizeof(void *));
}

static __declspec(naked) __declspec(noreturn) void __cdecl continue_catch(void *ip, void *base, void *stack) {
    __asm__("movl 4(%esp), %eax\n\t"
            "movl 8(%esp), %ebp\n\t"
            "movl 12(%esp), %esp\n\t"
            "jmpl *%eax");
}

static void find_catch(EXCEPTION_RECORD *record, cxx_frame *frame, const cxx_function *function,
                       active_catch *active);

static EXCEPTION_DISPOSITION __cdecl catch_handler(EXCEPTION_RECORD *record, seh_frame *registration,
                                                   CONTEXT *context, seh_frame **dispatcher) {
    (void)context;
    (void)dispatcher;
    active_catch *active = (active_catch *)registration;
    if ((record->ExceptionFlags & SEH_UNWIND_FLAGS) != 0) {
        const int catch_start = active->block->last_state + 1;
        if (active->frame->state > catch_start && active->frame->state <= active->block->last_catch_state) {
            unwind_to(active->frame, active->function, catch_start);
        }
        *stack_slot(active->frame) = active->stack;
        if (!same_exception(record, active->record)) {
            destroy_exception(active->record);
        }
    } else {
        find_catch(record, active->frame, active->function, active);
    }
    return ExceptionContinueSearch;
}

__asm__(".safeseh _catch_handler");

static __declspec(noreturn) void handle_catch(EXCEPTION_RECORD *record, cxx_frame *frame,
        const cxx_function *function, const cxx_try *block, const cxx_catch *handler,
        const cxx_type *type, active_catch *parent) {
    /* Keep the original record as the catch's identity, including on a rethrow. */
    EXCEPTION_RECORD unwinding = *record;
    seh_frame *target = parent != NULL ? &parent->registration : &frame->registration;
    RtlUnwind(target, NULL, &unwinding, NULL);
    unwind_to(frame, function, block->first_state);
    frame->state = block->last_state + 1;

    active_catch active = {{target, catch_handler}, frame, function, block, record, *stack_slot(frame)};
    seh_set_head(&active.registration);
    bind_object(handler, type, frame, (void *)record->ExceptionInformation[1]);
    void *ip = (void *)seh_call_funclet(handler->body, &frame->base, NULL);
    *stack_slot(frame) = active.stack;
    if (parent == NULL || !same_exception(record, parent->record)) {
        destroy_exception(record);
    }
    seh_set_head(target);
    continue_catch(ip, &frame->base, active.stack);
}

static void find_catch(EXCEPTION_RECORD *record, cxx_frame *frame, const cxx_function *function,
                       active_catch *active) {
    const cxx_throw_info *info = throw_info(record);
    const unsigned int magic = function->magic & 0x1fffffffu;
    if (info == NULL && magic == CXX_MAGIC_VC8 && (function->flags & CXX_SYNCHRONOUS) != 0) {
        return;
    }
    const int minimum_state = active != NULL ? active->block->last_state + 1 : 0;
    for (unsigned int i = 0; i < function->try_count; ++i) {
        const cxx_try *block = &function->tries[i];
        if (block->first_state < minimum_state || frame->state < block->first_state ||
            frame->state > block->last_state) {
            continue;
        }
        for (unsigned int j = 0; j < block->catch_count; ++j) {
            const cxx_catch *handler = &block->catches[j];
            const cxx_type *type = handler->name != NULL ? matches(handler, info) : NULL;
            if (type != NULL || handler->name == NULL) {
                handle_catch(record, frame, function, block, handler, type, active);
            }
        }
    }
}

static __attribute__((used)) EXCEPTION_DISPOSITION __cdecl cxx_frame_handler(EXCEPTION_RECORD *record,
        cxx_frame *frame, const cxx_function *function) {
    const unsigned int magic = function->magic & 0x1fffffffu;
    if (magic < CXX_MAGIC_VC6 || magic > CXX_MAGIC_VC8) {
        return ExceptionContinueSearch;
    }
    if ((record->ExceptionFlags & SEH_UNWIND_FLAGS) != 0) {
        unwind_to(frame, function, -1);
    } else {
        find_catch(record, frame, function, NULL);
        if (is_cxx(record) && magic == CXX_MAGIC_VC8 && (function->flags & CXX_NOEXCEPT) != 0) {
            abort();
        }
    }
    return ExceptionContinueSearch;
}

/* Each compiled frame's thunk loads its function descriptor into EAX. */
__declspec(naked) EXCEPTION_DISPOSITION __cdecl __CxxFrameHandler(EXCEPTION_RECORD *record,
        seh_frame *frame, CONTEXT *context, seh_frame **dispatcher) {
    __asm__("pushl %eax\n\t"
            "pushl 12(%esp)\n\t"
            "pushl 12(%esp)\n\t"
            "calll _cxx_frame_handler\n\t"
            "addl $12, %esp\n\t"
            "retl");
}

__declspec(noreturn) void WINAPI _CxxThrowException(void *object, const cxx_throw_info *info) {
    if (info == NULL) {
        const seh_frame *frame = seh_head();
        while (frame != SEH_END && frame->handler != catch_handler) {
            frame = frame->next;
        }
        if (frame == SEH_END) {
            abort();
        }
        const EXCEPTION_RECORD *active = ((const active_catch *)frame)->record;
        object = (void *)active->ExceptionInformation[1];
        info = throw_info(active);
    }
    EXCEPTION_RECORD record = {0};
    record.ExceptionCode = CXX_EXCEPTION;
    record.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    record.NumberParameters = 3;
    record.ExceptionInformation[0] = CXX_MAGIC_VC6;
    record.ExceptionInformation[1] = (ULONG_PTR)object;
    record.ExceptionInformation[2] = (ULONG_PTR)info;
    SxwRaiseExceptionWithExitStatus(&record, 3);
    __builtin_unreachable();
}
