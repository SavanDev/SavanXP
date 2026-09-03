#include "libc.h"

#include <stdlib.h>

/*
 * Ejercita el malloc de userland (subsystems/posix/sdk/v1/runtime/posix.c)
 * ahora que la arena de BSS es solo el bootstrap y el resto del heap crece con
 * arenas respaldadas por secciones (section_create + map_view).
 *
 * Todos los tamanos grandes de aca estan MUY por encima de cualquier arena de
 * bootstrap razonable: si un malloc de esos vuelve no-nulo es porque el
 * allocator pidio una seccion al kernel. Y el bucle de reciclado falla si las
 * arenas vacias no se devuelven, porque el allocator topa el limite de arenas
 * (SX_ARENA_CAPACITY) o el pool global de secciones del kernel.
 */

/* Los tamanos se mantienen chicos a proposito: el smoke corre bajo TCG y cada
 * arena nueva le cuesta al kernel poner en cero todas sus paginas. 2 MiB ya son
 * 8x la arena de bootstrap, que es lo unico que hace falta para probar que el
 * bloque salio de una seccion. */
#define BIG_BYTES ((size_t)(2u * 1024u * 1024u))
#define LIVE_BYTES ((size_t)(1024u * 1024u))
#define LIVE_BLOCKS 6
#define RECYCLE_ROUNDS 12
#define TOUCH_STRIDE ((size_t)4096u)

/* Toca una pagina de cada una mas el ultimo byte: alcanza para probar que el
 * mapeo esta respaldado de punta a punta sin escribir megabytes bajo TCG. */
static void fill_pattern(unsigned char* buffer, size_t size, unsigned char seed) {
    size_t offset = 0;

    for (offset = 0; offset < size; offset += TOUCH_STRIDE) {
        buffer[offset] = (unsigned char)(seed + (unsigned char)(offset >> 12));
    }
    buffer[size - 1] = (unsigned char)(seed ^ 0xFFu);
}

static int check_pattern(const unsigned char* buffer, size_t size, unsigned char seed, const char* label) {
    size_t offset = 0;

    for (offset = 0; offset < size; offset += TOUCH_STRIDE) {
        const unsigned char expected = (unsigned char)(seed + (unsigned char)(offset >> 12));
        if (buffer[offset] != expected) {
            eprintf("heaptest: %s difiere en offset %u (%u != %u)\n",
                label, (unsigned)offset, (unsigned)buffer[offset], (unsigned)expected);
            return 0;
        }
    }
    if (buffer[size - 1] != (unsigned char)(seed ^ 0xFFu)) {
        eprintf("heaptest: %s difiere en el ultimo byte (%u)\n",
            label, (unsigned)buffer[size - 1]);
        return 0;
    }
    return 1;
}

/* Un solo bloque mas grande que el bootstrap: obliga a tomar una seccion. */
static int test_growth(void) {
    unsigned char* block = (unsigned char*)malloc(BIG_BYTES);

    if (block == 0) {
        eprintf("heaptest: malloc de %u KiB fallo\n", (unsigned)(BIG_BYTES / 1024u));
        return 0;
    }

    fill_pattern(block, BIG_BYTES, 0x11u);
    if (!check_pattern(block, BIG_BYTES, 0x11u, "arena dinamica")) {
        free(block);
        return 0;
    }

    free(block);
    return 1;
}

/* Lo que vive en el bootstrap no se tiene que mover cuando aparecen y
 * desaparecen arenas nuevas. */
static int test_bootstrap_survives(void) {
    char* small = (char*)malloc(64);
    unsigned char* block = 0;

    if (small == 0) {
        eprintf("heaptest: malloc chico fallo\n");
        return 0;
    }
    strcpy(small, "bootstrap");

    block = (unsigned char*)malloc(BIG_BYTES);
    if (block == 0) {
        eprintf("heaptest: malloc grande fallo con un bloque chico vivo\n");
        free(small);
        return 0;
    }
    fill_pattern(block, BIG_BYTES, 0x22u);
    free(block);

    if (strcmp(small, "bootstrap") != 0) {
        eprintf("heaptest: el bloque del bootstrap se corrompio (%s)\n", small);
        free(small);
        return 0;
    }

    free(small);
    return 1;
}

/* Si las arenas vacias no se devolvieran, esto muere apenas pasa el limite. */
static int test_recycle(void) {
    int round = 0;

    for (round = 0; round < RECYCLE_ROUNDS; ++round) {
        unsigned char* block = (unsigned char*)malloc(BIG_BYTES);
        if (block == 0) {
            eprintf("heaptest: la vuelta %d no consiguio arena (arenas filtradas?)\n", round);
            return 0;
        }
        fill_pattern(block, BIG_BYTES, (unsigned char)round);
        if (!check_pattern(block, BIG_BYTES, (unsigned char)round, "reciclado")) {
            free(block);
            return 0;
        }
        free(block);
    }
    return 1;
}

/* Varias arenas vivas a la vez, para que el first-fit tenga que cruzarlas. */
static int test_multiple_arenas(void) {
    unsigned char* blocks[LIVE_BLOCKS];
    int index = 0;
    int ok = 1;

    for (index = 0; index < LIVE_BLOCKS; ++index) {
        blocks[index] = 0;
    }

    for (index = 0; index < LIVE_BLOCKS; ++index) {
        blocks[index] = (unsigned char*)malloc(LIVE_BYTES);
        if (blocks[index] == 0) {
            eprintf("heaptest: no entro el bloque vivo %d\n", index);
            ok = 0;
            break;
        }
        fill_pattern(blocks[index], LIVE_BYTES, (unsigned char)(0x40u + index));
    }

    if (ok) {
        for (index = 0; index < LIVE_BLOCKS; ++index) {
            if (!check_pattern(blocks[index], LIVE_BYTES, (unsigned char)(0x40u + index), "bloque vivo")) {
                ok = 0;
                break;
            }
        }
    }

    for (index = 0; index < LIVE_BLOCKS; ++index) {
        free(blocks[index]);
    }
    return ok;
}

/* realloc que no entra en la arena de origen: tiene que copiar a la nueva. */
static int test_realloc_across_arenas(void) {
    unsigned char* block = (unsigned char*)malloc(1024);
    unsigned char* grown = 0;
    size_t offset = 0;

    if (block == 0) {
        eprintf("heaptest: malloc previo al realloc fallo\n");
        return 0;
    }
    for (offset = 0; offset < 1024; ++offset) {
        block[offset] = (unsigned char)(offset & 0xFFu);
    }

    grown = (unsigned char*)realloc(block, BIG_BYTES);
    if (grown == 0) {
        eprintf("heaptest: realloc a %u KiB fallo\n", (unsigned)(BIG_BYTES / 1024u));
        free(block);
        return 0;
    }

    for (offset = 0; offset < 1024; ++offset) {
        if (grown[offset] != (unsigned char)(offset & 0xFFu)) {
            eprintf("heaptest: realloc no preservo el byte %u\n", (unsigned)offset);
            free(grown);
            return 0;
        }
    }

    fill_pattern(grown, BIG_BYTES, 0x33u);
    if (!check_pattern(grown, BIG_BYTES, 0x33u, "bloque reallocado")) {
        free(grown);
        return 0;
    }

    free(grown);
    return 1;
}

/*
 * La vista de la arena se mapea PRIVATE, asi que fork tiene que clonar la
 * seccion con su contenido: el hijo ve el patron del padre y sus escrituras no
 * vuelven. Es la misma semantica que daba la arena de BSS.
 */
static int test_fork_private(void) {
    unsigned char* block = (unsigned char*)malloc(BIG_BYTES);
    long pid = 0;
    int status = -1;

    if (block == 0) {
        eprintf("heaptest: malloc previo al fork fallo\n");
        return 0;
    }
    fill_pattern(block, BIG_BYTES, 0x5Au);

    pid = savanxp_fork();
    if (pid < 0) {
        eprintf("heaptest: fork fallo (%s)\n", result_error_string(pid));
        free(block);
        return 0;
    }

    if (pid == 0) {
        if (!check_pattern(block, BIG_BYTES, 0x5Au, "arena heredada")) {
            exit(2);
        }
        fill_pattern(block, BIG_BYTES, 0xA5u);
        if (!check_pattern(block, BIG_BYTES, 0xA5u, "arena del hijo")) {
            exit(3);
        }
        free(block);
        exit(0);
    }

    if (savanxp_waitpid((int)pid, &status) < 0) {
        eprintf("heaptest: waitpid fallo\n");
        free(block);
        return 0;
    }
    if (status != 0) {
        eprintf("heaptest: el hijo salio con %d\n", status);
        free(block);
        return 0;
    }
    if (!check_pattern(block, BIG_BYTES, 0x5Au, "arena del padre tras el fork")) {
        free(block);
        return 0;
    }

    free(block);
    return 1;
}

struct heaptest_case {
    const char* name;
    int (*run)(void);
};

int main(void) {
    static const struct heaptest_case cases[] = {
        { "growth", test_growth },
        { "bootstrap", test_bootstrap_survives },
        { "recycle", test_recycle },
        { "arenas", test_multiple_arenas },
        { "realloc", test_realloc_across_arenas },
        { "fork", test_fork_private },
    };
    const int total = (int)(sizeof(cases) / sizeof(cases[0]));
    int index = 0;

    for (index = 0; index < total; ++index) {
        printf("heaptest: %s\n", cases[index].name);
        if (!cases[index].run()) {
            eprintf("heaptest: fallo %s\n", cases[index].name);
            return 1;
        }
    }

    printf("heaptest ok\n");
    return 0;
}
