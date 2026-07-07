/*
 * SavanXP - runtime del subsistema nativo, Fase 2.
 *
 * Envoltura de syscalls sobre `int $0x80` con la convencion del kernel
 * (numero -> rax, args -> rdi/rsi/rdx, resultado -> rax), primitivas propias
 * del ABI nativo (info/log), heap del runtime y builtins de memoria.
 */
#include "savanxp_native.h"

/* --- Syscalls crudas -------------------------------------------------------- */

long sxn_syscall1(long number, long a) {
    long result;
    __asm__ volatile("int $0x80"
                     : "=a"(result)
                     : "a"(number), "D"(a)
                     : "memory");
    return result;
}

long sxn_syscall3(long number, long a, long b, long c) {
    long result;
    __asm__ volatile("int $0x80"
                     : "=a"(result)
                     : "a"(number), "D"(a), "S"(b), "d"(c)
                     : "memory");
    return result;
}

long sxn_syscall5(long number, long a, long b, long c, long d, long e) {
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    long result;
    __asm__ volatile("int $0x80"
                     : "=a"(result)
                     : "a"(number), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
                     : "memory");
    return result;
}

/* --- Identidad y log (ABI nativo propio) ------------------------------------ */

long sxn_info(struct sxn_native_info *out) {
    return sxn_syscall1(SXN_SYS_INFO, (long)out);
}

long sxn_log(const char *message) {
    return sxn_syscall1(SXN_SYS_LOG, (long)message);
}

long sxn_log_num(const char *label, long value) {
    char buffer[128];
    unsigned long position = 0;

    while (label[position] != '\0' && position < sizeof(buffer) - 24) {
        buffer[position] = label[position];
        position += 1;
    }
    buffer[position++] = '=';

    if (value < 0) {
        buffer[position++] = '-';
        value = -value;
    }

    char digits[24];
    unsigned long digit_count = 0;
    do {
        digits[digit_count++] = (char)('0' + (value % 10));
        value /= 10;
    } while (value != 0 && digit_count < sizeof(digits));

    while (digit_count != 0) {
        buffer[position++] = digits[--digit_count];
    }
    buffer[position] = '\0';
    return sxn_log(buffer);
}

/* --- Graficos (ABI nativo propio) --------------------------------------------- */

long sxn_gfx_info(struct sxn_gfx_info *out) {
    return sxn_syscall1(SXN_SYS_GFX_INFO, (long)out);
}

long sxn_gfx_acquire(void) {
    return sxn_syscall1(SXN_SYS_GFX_ACQUIRE, 0);
}

long sxn_gfx_release(void) {
    return sxn_syscall1(SXN_SYS_GFX_RELEASE, 0);
}

long sxn_gfx_present(const void *frame, unsigned int pitch,
                     unsigned int x, unsigned int y,
                     unsigned int width, unsigned int height) {
    struct sxn_gfx_present region;
    region.pixels = (unsigned long)frame;
    region.source_pitch = pitch;
    region.x = x;
    region.y = y;
    region.width = width;
    region.height = height;
    region.reserved0 = 0;
    return sxn_syscall1(SXN_SYS_GFX_PRESENT, (long)&region);
}

/* --- I/O y proceso (baseline transitorio) ------------------------------------ */

long sxn_write(int fd, const char *buf, int len) {
    return sxn_syscall3(SXN_SYS_WRITE, fd, (long)buf, len);
}

void sxn_exit(int code) {
    sxn_syscall3(SXN_SYS_EXIT, code, 0, 0);
    for (;;) {
        __asm__ volatile("hlt");
    }
}

void sxn_hello(void) {
    static const char msg[] = "hola desde Haxe\n";
    sxn_write(1, msg, (int)sizeof(msg) - 1);
}

/* --- Heap: arena BSS con free-list (first-fit, split, coalescing) ------------ */

#define SXN_HEAP_SIZE (4u * 1024u * 1024u)
#define SXN_ALLOC_ALIGNMENT 16u
#define SXN_ALLOC_MAGIC 0x53584e41u /* "SXNA" */

/* Header de 32 bytes antes de cada payload; con la arena alineada a 16, los
 * payloads quedan alineados a 16. `next_free` solo es valido en bloques libres
 * (magic == 0), encadenados en orden de direccion para poder coalescer. */
typedef struct sxn_block {
    unsigned long size; /* bytes del payload, multiplo de 16 */
    unsigned long magic; /* SXN_ALLOC_MAGIC = asignado, 0 = libre */
    struct sxn_block *next_free;
    unsigned long reserved0;
} sxn_block;

static unsigned char g_sxn_heap[SXN_HEAP_SIZE] __attribute__((aligned(16)));
static sxn_block *g_sxn_free_list = 0;
static int g_sxn_heap_ready = 0;

static unsigned long sxn_align_up(unsigned long value) {
    return (value + (SXN_ALLOC_ALIGNMENT - 1)) & ~(unsigned long)(SXN_ALLOC_ALIGNMENT - 1);
}

static void sxn_heap_initialize(void) {
    sxn_block *initial = (sxn_block *)g_sxn_heap;
    initial->size = SXN_HEAP_SIZE - sizeof(sxn_block);
    initial->magic = 0;
    initial->next_free = 0;
    initial->reserved0 = 0;
    g_sxn_free_list = initial;
    g_sxn_heap_ready = 1;
}

static unsigned char *sxn_block_end(sxn_block *block) {
    return (unsigned char *)block + sizeof(sxn_block) + block->size;
}

void *sxn_alloc(unsigned long size) {
    if (!g_sxn_heap_ready) {
        sxn_heap_initialize();
    }
    if (size == 0) {
        size = SXN_ALLOC_ALIGNMENT;
    }
    size = sxn_align_up(size);

    sxn_block *previous = 0;
    sxn_block *block = g_sxn_free_list;
    while (block != 0) {
        if (block->size >= size) {
            /* Partir el bloque si el resto alcanza para header + minimo. */
            if (block->size >= size + sizeof(sxn_block) + SXN_ALLOC_ALIGNMENT) {
                sxn_block *remainder =
                    (sxn_block *)((unsigned char *)block + sizeof(sxn_block) + size);
                remainder->size = block->size - size - sizeof(sxn_block);
                remainder->magic = 0;
                remainder->next_free = block->next_free;
                remainder->reserved0 = 0;
                block->size = size;
                if (previous != 0) {
                    previous->next_free = remainder;
                } else {
                    g_sxn_free_list = remainder;
                }
            } else {
                if (previous != 0) {
                    previous->next_free = block->next_free;
                } else {
                    g_sxn_free_list = block->next_free;
                }
            }
            block->magic = SXN_ALLOC_MAGIC;
            block->next_free = 0;
            return (unsigned char *)block + sizeof(sxn_block);
        }
        previous = block;
        block = block->next_free;
    }
    return 0;
}

void sxn_free(void *ptr) {
    if (ptr == 0) {
        return;
    }
    sxn_block *block = (sxn_block *)((unsigned char *)ptr - sizeof(sxn_block));
    if (block->magic != SXN_ALLOC_MAGIC) {
        sxn_log("heap: sxn_free sobre puntero invalido");
        return;
    }
    block->magic = 0;

    /* Insertar en la free-list ordenada por direccion. */
    sxn_block *previous = 0;
    sxn_block *cursor = g_sxn_free_list;
    while (cursor != 0 && cursor < block) {
        previous = cursor;
        cursor = cursor->next_free;
    }
    block->next_free = cursor;
    if (previous != 0) {
        previous->next_free = block;
    } else {
        g_sxn_free_list = block;
    }

    /* Coalescer con el vecino siguiente y con el anterior. */
    if (cursor != 0 && sxn_block_end(block) == (unsigned char *)cursor) {
        block->size += sizeof(sxn_block) + cursor->size;
        block->next_free = cursor->next_free;
    }
    if (previous != 0 && sxn_block_end(previous) == (unsigned char *)block) {
        previous->size += sizeof(sxn_block) + block->size;
        previous->next_free = block->next_free;
    }
}

void *sxn_realloc(void *ptr, unsigned long size) {
    if (ptr == 0) {
        return sxn_alloc(size);
    }
    if (size == 0) {
        sxn_free(ptr);
        return 0;
    }

    sxn_block *block = (sxn_block *)((unsigned char *)ptr - sizeof(sxn_block));
    if (block->magic != SXN_ALLOC_MAGIC) {
        sxn_log("heap: sxn_realloc sobre puntero invalido");
        return 0;
    }
    if (block->size >= size) {
        return ptr;
    }

    void *replacement = sxn_alloc(size);
    if (replacement == 0) {
        return 0;
    }
    memcpy(replacement, ptr, block->size);
    sxn_free(ptr);
    return replacement;
}

/* --- Builtins de memoria ------------------------------------------------------ */

void *memcpy(void *destination, const void *source, unsigned long count) {
    unsigned char *out = (unsigned char *)destination;
    const unsigned char *in = (const unsigned char *)source;
    for (unsigned long index = 0; index < count; ++index) {
        out[index] = in[index];
    }
    return destination;
}

void *memmove(void *destination, const void *source, unsigned long count) {
    unsigned char *out = (unsigned char *)destination;
    const unsigned char *in = (const unsigned char *)source;
    if (out < in) {
        for (unsigned long index = 0; index < count; ++index) {
            out[index] = in[index];
        }
    } else if (out > in) {
        for (unsigned long index = count; index != 0; --index) {
            out[index - 1] = in[index - 1];
        }
    }
    return destination;
}

void *memset(void *destination, int value, unsigned long count) {
    unsigned char *out = (unsigned char *)destination;
    for (unsigned long index = 0; index < count; ++index) {
        out[index] = (unsigned char)value;
    }
    return destination;
}

int memcmp(const void *left, const void *right, unsigned long count) {
    const unsigned char *a = (const unsigned char *)left;
    const unsigned char *b = (const unsigned char *)right;
    for (unsigned long index = 0; index < count; ++index) {
        if (a[index] != b[index]) {
            return a[index] < b[index] ? -1 : 1;
        }
    }
    return 0;
}
