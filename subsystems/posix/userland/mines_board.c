#include "mines_board.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct mines_level_info k_levels[MINES_LEVEL_COUNT] = {
    {"Beginner", 9, 9, 10},
    {"Intermediate", 16, 16, 40},
    {"Expert", 30, 16, 99},
};

/* Clave de cada nivel en /disk/mines.ini. En minusculas y sin espacios porque
 * es una clave de archivo, no el rotulo del menu: cambiar "Intermediate" por
 * otro texto en la UI no tiene que invalidar los records de nadie. */
static const char *const k_score_keys[MINES_LEVEL_COUNT] = {
    "beginner",
    "intermediate",
    "expert",
};

static int g_best[MINES_LEVEL_COUNT] = {0, 0, 0};
static const char *g_scores_path = MINES_SCORES_PATH;

/* xorshift32. No hace falta mas: reparte 99 minas en 480 celdas, no genera
 * claves. El estado arranca en algo distinto de cero porque el cero es el punto
 * fijo de la familia -- un xorshift sembrado en 0 devuelve 0 para siempre, y el
 * tablero saldria siempre igual. */
static uint32_t g_random_state = 0x1d872b41u;

void mines_random_seed(uint32_t seed)
{
    g_random_state = seed != 0u ? seed : 0x1d872b41u;
}

void mines_random_seed_from_clock(void)
{
    /* monotonic_ns es el reloj fino, pero devuelve 0 si el TSC no llego a
     * calibrarse (savanxp/libc.h), y entonces la semilla la tiene que poner
     * otro: uptime_ms mas el pid alcanzan para que dos partidas de la misma
     * sesion no salgan identicas. */
    unsigned long long now = monotonic_ns();
    uint32_t seed = (uint32_t)(now ^ (now >> 32));

    seed ^= (uint32_t)uptime_ms() * 2654435761u;
    seed ^= (uint32_t)savanxp_getpid() << 16;
    mines_random_seed(seed);
}

static uint32_t mines_random_next(void)
{
    uint32_t state = g_random_state;

    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    g_random_state = state;
    return state;
}

/* Entero en [0, bound). El modulo puro sesga hacia los valores bajos; descartar
 * el sobrante lo saca sin pedir punto flotante. */
static uint32_t mines_random_below(uint32_t bound)
{
    uint32_t limit;

    if (bound == 0u)
    {
        return 0u;
    }
    limit = 0xffffffffu - (0xffffffffu % bound);
    for (;;)
    {
        uint32_t value = mines_random_next();

        if (value < limit)
        {
            return value % bound;
        }
    }
}

const struct mines_level_info *mines_level_info(int level)
{
    if (level < 0 || level >= MINES_LEVEL_COUNT)
    {
        level = MINES_LEVEL_BEGINNER;
    }
    return &k_levels[level];
}

int mines_board_cell_count(const struct mines_board *board)
{
    if (board == 0)
    {
        return 0;
    }
    return board->columns * board->rows;
}

int mines_board_index(const struct mines_board *board, int column, int row)
{
    if (board == 0 || column < 0 || row < 0 || column >= board->columns || row >= board->rows)
    {
        return -1;
    }
    return (row * board->columns) + column;
}

struct mines_cell *mines_board_cell(struct mines_board *board, int column, int row)
{
    int index = mines_board_index(board, column, row);

    if (index < 0)
    {
        return 0;
    }
    return &board->cells[index];
}

const struct mines_cell *mines_board_cell_const(const struct mines_board *board, int column, int row)
{
    int index = mines_board_index(board, column, row);

    if (index < 0)
    {
        return 0;
    }
    return &board->cells[index];
}

void mines_board_reset(struct mines_board *board, int level)
{
    const struct mines_level_info *info = mines_level_info(level);
    int marks = 1;

    if (board == 0)
    {
        return;
    }
    /* La preferencia de marcas es del jugador y sobrevive al cambio de nivel:
     * es lo unico de la partida anterior que se conserva. */
    if (board->columns != 0)
    {
        marks = board->marks_enabled;
    }
    memset(board, 0, sizeof(*board));
    board->level = (level < 0 || level >= MINES_LEVEL_COUNT) ? MINES_LEVEL_BEGINNER : level;
    board->columns = info->columns;
    board->rows = info->rows;
    board->mine_count = info->mines;
    board->state = MINES_STATE_READY;
    board->marks_enabled = marks;
}

int mines_board_remaining_mines(const struct mines_board *board)
{
    if (board == 0)
    {
        return 0;
    }
    return board->mine_count - board->flag_count;
}

static void mines_count_neighbours(struct mines_board *board)
{
    int row;

    for (row = 0; row < board->rows; ++row)
    {
        int column;

        for (column = 0; column < board->columns; ++column)
        {
            struct mines_cell *cell = mines_board_cell(board, column, row);
            int count = 0;
            int dy;

            for (dy = -1; dy <= 1; ++dy)
            {
                int dx;

                for (dx = -1; dx <= 1; ++dx)
                {
                    const struct mines_cell *neighbour;

                    if (dx == 0 && dy == 0)
                    {
                        continue;
                    }
                    neighbour = mines_board_cell_const(board, column + dx, row + dy);
                    if (neighbour != 0 && neighbour->mine != 0u)
                    {
                        count += 1;
                    }
                }
            }
            cell->neighbours = (uint8_t)count;
        }
    }
}

/*
 * Siembra las minas evitando `safe_index`.
 *
 * Barajado parcial de la lista de candidatos y no "sortear hasta encontrar un
 * hueco": con 99 minas en 479 candidatos el rechazo todavia termina rapido,
 * pero el costo crece con la densidad y un tablero casi lleno de minas -- que
 * es exactamente lo que un "Custom" permitiria -- lo vuelve impredecible. El
 * barajado tarda lo mismo siempre.
 */
static void mines_place_mines(struct mines_board *board, int safe_index)
{
    static int candidates[MINES_MAX_CELLS];
    int count = mines_board_cell_count(board);
    int candidate_count = 0;
    int index;
    int placed;

    for (index = 0; index < count; ++index)
    {
        if (index == safe_index)
        {
            continue;
        }
        candidates[candidate_count] = index;
        candidate_count += 1;
    }

    placed = board->mine_count;
    if (placed > candidate_count)
    {
        placed = candidate_count;
        board->mine_count = placed;
    }
    for (index = 0; index < placed; ++index)
    {
        int pick = index + (int)mines_random_below((uint32_t)(candidate_count - index));
        int chosen = candidates[pick];

        candidates[pick] = candidates[index];
        candidates[index] = chosen;
        board->cells[chosen].mine = 1u;
    }
    mines_count_neighbours(board);
}

static void mines_reveal_all_mines(struct mines_board *board)
{
    int count = mines_board_cell_count(board);
    int index;

    for (index = 0; index < count; ++index)
    {
        struct mines_cell *cell = &board->cells[index];

        if (cell->mine != 0u && cell->mark != MINES_MARK_FLAG)
        {
            cell->revealed = 1u;
        }
    }
}

/* Victoria: no queda ninguna celda sin mina por descubrir. Las banderas no
 * entran en la cuenta -- se gana descubriendo, no marcando --, y al ganar el
 * juego pone las que falten para que el contador cierre en cero, igual que el
 * original. */
static void mines_check_win(struct mines_board *board)
{
    int safe_cells = mines_board_cell_count(board) - board->mine_count;
    int count;
    int index;

    if (board->revealed_count < safe_cells)
    {
        return;
    }
    board->state = MINES_STATE_WON;
    count = mines_board_cell_count(board);
    for (index = 0; index < count; ++index)
    {
        struct mines_cell *cell = &board->cells[index];

        if (cell->mine != 0u && cell->mark != MINES_MARK_FLAG)
        {
            cell->mark = MINES_MARK_FLAG;
            board->flag_count += 1;
        }
    }
}

/* Descubre una celda y arrastra a las vecinas si no tiene minas alrededor. La
 * pila es explicita y del tamano del tablero: cada celda entra una sola vez
 * porque se marca descubierta antes de empujar a las vecinas. */
static void mines_flood_reveal(struct mines_board *board, int start_index)
{
    static int stack[MINES_MAX_CELLS];
    int depth = 0;

    stack[depth] = start_index;
    depth += 1;

    while (depth > 0)
    {
        int index;
        int column;
        int row;
        struct mines_cell *cell;

        depth -= 1;
        index = stack[depth];
        cell = &board->cells[index];
        if (cell->revealed != 0u)
        {
            continue;
        }
        cell->revealed = 1u;
        cell->mark = MINES_MARK_NONE;
        board->revealed_count += 1;
        if (cell->neighbours != 0u)
        {
            continue;
        }

        column = index % board->columns;
        row = index / board->columns;
        {
            int dy;

            for (dy = -1; dy <= 1; ++dy)
            {
                int dx;

                for (dx = -1; dx <= 1; ++dx)
                {
                    int neighbour_index = mines_board_index(board, column + dx, row + dy);
                    const struct mines_cell *neighbour;

                    if (neighbour_index < 0 || neighbour_index == index)
                    {
                        continue;
                    }
                    neighbour = &board->cells[neighbour_index];
                    /* La bandera protege: la cascada no la levanta. Es lo que
                     * permite acordonar una zona y seguir descubriendo
                     * alrededor sin miedo a que el barrido entre igual. */
                    if (neighbour->revealed != 0u || neighbour->mark == MINES_MARK_FLAG)
                    {
                        continue;
                    }
                    stack[depth] = neighbour_index;
                    depth += 1;
                }
            }
        }
    }
}

int mines_board_reveal(struct mines_board *board, int column, int row)
{
    int index;
    struct mines_cell *cell;

    if (board == 0)
    {
        return 0;
    }
    if (board->state == MINES_STATE_WON || board->state == MINES_STATE_LOST)
    {
        return 0;
    }
    index = mines_board_index(board, column, row);
    if (index < 0)
    {
        return 0;
    }
    cell = &board->cells[index];
    if (cell->revealed != 0u || cell->mark == MINES_MARK_FLAG)
    {
        return 0;
    }

    if (board->state == MINES_STATE_READY)
    {
        mines_place_mines(board, index);
        board->state = MINES_STATE_PLAYING;
    }

    if (cell->mine != 0u)
    {
        cell->revealed = 1u;
        cell->exploded = 1u;
        board->state = MINES_STATE_LOST;
        mines_reveal_all_mines(board);
        return 1;
    }

    mines_flood_reveal(board, index);
    mines_check_win(board);
    return 1;
}

int mines_board_cycle_mark(struct mines_board *board, int column, int row)
{
    struct mines_cell *cell;

    if (board == 0 || board->state == MINES_STATE_WON || board->state == MINES_STATE_LOST)
    {
        return 0;
    }
    cell = mines_board_cell(board, column, row);
    if (cell == 0 || cell->revealed != 0u)
    {
        return 0;
    }

    if (cell->mark == MINES_MARK_NONE)
    {
        cell->mark = MINES_MARK_FLAG;
        board->flag_count += 1;
    }
    else if (cell->mark == MINES_MARK_FLAG)
    {
        board->flag_count -= 1;
        cell->mark = board->marks_enabled ? MINES_MARK_QUESTION : MINES_MARK_NONE;
    }
    else
    {
        cell->mark = MINES_MARK_NONE;
    }
    return 1;
}

int mines_board_chord(struct mines_board *board, int column, int row)
{
    const struct mines_cell *cell;
    int flags = 0;
    int changed = 0;
    int dy;

    if (board == 0 || board->state != MINES_STATE_PLAYING)
    {
        return 0;
    }
    cell = mines_board_cell_const(board, column, row);
    if (cell == 0 || cell->revealed == 0u || cell->neighbours == 0u)
    {
        return 0;
    }

    for (dy = -1; dy <= 1; ++dy)
    {
        int dx;

        for (dx = -1; dx <= 1; ++dx)
        {
            const struct mines_cell *neighbour = mines_board_cell_const(board, column + dx, row + dy);

            if (neighbour != 0 && neighbour->mark == MINES_MARK_FLAG)
            {
                flags += 1;
            }
        }
    }
    if (flags != (int)cell->neighbours)
    {
        return 0;
    }

    for (dy = -1; dy <= 1; ++dy)
    {
        int dx;

        for (dx = -1; dx <= 1; ++dx)
        {
            if (dx == 0 && dy == 0)
            {
                continue;
            }
            /* Se descubre por el camino normal, asi que una bandera mal puesta
             * explota aca igual que si se hubiera hecho click a mano. */
            if (mines_board_reveal(board, column + dx, row + dy))
            {
                changed = 1;
            }
        }
    }
    return changed;
}

/* --- mejores tiempos ------------------------------------------------------ */

void mines_scores_set_path(const char *path)
{
    g_scores_path = (path != 0 && path[0] != '\0') ? path : MINES_SCORES_PATH;
}

int mines_scores_best(int level)
{
    if (level < 0 || level >= MINES_LEVEL_COUNT)
    {
        return 0;
    }
    return g_best[level];
}

static void mines_scores_save(void)
{
    FILE *stream = fopen(g_scores_path, "w");
    int level;

    /* Sin disco escribible no hay nada que hacer y tampoco nada que avisar: la
     * partida ya se jugo y el record se pierde al salir. */
    if (stream == 0)
    {
        return;
    }
    fprintf(stream, "# SavanXP Minesweeper best times, in seconds\n");
    for (level = 0; level < MINES_LEVEL_COUNT; ++level)
    {
        if (g_best[level] > 0)
        {
            fprintf(stream, "%s=%d\n", k_score_keys[level], g_best[level]);
        }
    }
    fclose(stream);
}

static char *mines_trim(char *text)
{
    size_t length;

    while (*text == ' ' || *text == '\t')
    {
        text += 1;
    }
    length = strlen(text);
    while (length != 0)
    {
        char last = text[length - 1u];

        if (last != ' ' && last != '\t' && last != '\r' && last != '\n')
        {
            break;
        }
        text[length - 1u] = '\0';
        length -= 1u;
    }
    return text;
}

void mines_scores_load(void)
{
    FILE *stream;
    char line[64];
    int level;

    for (level = 0; level < MINES_LEVEL_COUNT; ++level)
    {
        g_best[level] = 0;
    }
    stream = fopen(g_scores_path, "r");
    if (stream == 0)
    {
        return;
    }
    while (fgets(line, (int)sizeof(line), stream) != 0)
    {
        char *separator = strchr(line, '=');
        char *key;
        char *value;

        if (line[0] == '#' || line[0] == ';' || separator == 0)
        {
            continue;
        }
        *separator = '\0';
        key = mines_trim(line);
        value = mines_trim(separator + 1);
        for (level = 0; level < MINES_LEVEL_COUNT; ++level)
        {
            if (strcmp(key, k_score_keys[level]) != 0)
            {
                continue;
            }
            {
                int seconds = atoi(value);

                /* Un valor fuera de rango se descarta en vez de mostrarse: un
                 * archivo editado a mano no tiene por que romper el dialogo. */
                if (seconds > 0 && seconds <= MINES_COUNTER_MAX)
                {
                    g_best[level] = seconds;
                }
            }
            break;
        }
    }
    fclose(stream);
}

int mines_scores_record(int level, int seconds)
{
    if (level < 0 || level >= MINES_LEVEL_COUNT || seconds <= 0)
    {
        return 0;
    }
    if (seconds > MINES_COUNTER_MAX)
    {
        seconds = MINES_COUNTER_MAX;
    }
    if (g_best[level] != 0 && seconds >= g_best[level])
    {
        return 0;
    }
    g_best[level] = seconds;
    mines_scores_save();
    return 1;
}

void mines_scores_reset(void)
{
    int level;

    for (level = 0; level < MINES_LEVEL_COUNT; ++level)
    {
        g_best[level] = 0;
    }
    /* Se guarda un archivo vacio en vez de borrarlo: asi "sin records" es un
     * estado declarado y no se confunde con "todavia no jugo nadie". */
    mines_scores_save();
}

/* --- selftest ------------------------------------------------------------- */

#define MINES_SELFTEST_SCORES_DIR "/disk/tmp"
#define MINES_SELFTEST_SCORES_PATH "/disk/tmp/mines-selftest.ini"

static int g_selftest_failures = 0;

static void expect(int condition, const char *label)
{
    if (!condition)
    {
        printf("MINES SMOKE FAIL %s\n", label);
        g_selftest_failures += 1;
    }
}

static int mines_count_mines(const struct mines_board *board)
{
    int count = mines_board_cell_count(board);
    int mines = 0;
    int index;

    for (index = 0; index < count; ++index)
    {
        if (board->cells[index].mine != 0u)
        {
            mines += 1;
        }
    }
    return mines;
}

/* Recuenta los vecinos a mano y compara contra lo que dejo la siembra. Es la
 * unica forma de que el conteo no se valide contra si mismo. */
static int mines_neighbours_consistent(const struct mines_board *board)
{
    int row;

    for (row = 0; row < board->rows; ++row)
    {
        int column;

        for (column = 0; column < board->columns; ++column)
        {
            const struct mines_cell *cell = mines_board_cell_const(board, column, row);
            int expected = 0;
            int dy;

            for (dy = -1; dy <= 1; ++dy)
            {
                int dx;

                for (dx = -1; dx <= 1; ++dx)
                {
                    const struct mines_cell *neighbour;

                    if (dx == 0 && dy == 0)
                    {
                        continue;
                    }
                    neighbour = mines_board_cell_const(board, column + dx, row + dy);
                    if (neighbour != 0 && neighbour->mine != 0u)
                    {
                        expected += 1;
                    }
                }
            }
            if ((int)cell->neighbours != expected)
            {
                return 0;
            }
        }
    }
    return 1;
}

static void selftest_levels(void)
{
    struct mines_board board;
    int level;

    memset(&board, 0, sizeof(board));
    for (level = 0; level < MINES_LEVEL_COUNT; ++level)
    {
        const struct mines_level_info *info = mines_level_info(level);

        mines_board_reset(&board, level);
        expect(board.columns == info->columns && board.rows == info->rows,
               "geometria del nivel");
        expect(board.mine_count == info->mines, "minas del nivel");
        expect(board.columns <= MINES_MAX_COLUMNS && board.rows <= MINES_MAX_ROWS,
               "el nivel entra en la capacidad reservada");
        expect(board.mine_count < mines_board_cell_count(&board),
               "quedan celdas sin mina");
        expect(board.state == MINES_STATE_READY, "arranca en READY");
        expect(mines_count_mines(&board) == 0, "READY no tiene minas sembradas");
    }
    /* Un nivel invalido cae al principiante en vez de dejar el tablero en cero,
     * que seria una ventana sin celdas. */
    mines_board_reset(&board, 99);
    expect(board.level == MINES_LEVEL_BEGINNER, "nivel invalido cae a Beginner");
}

/* El primer click no puede perder. Se prueba sobre muchas semillas Y muchas
 * celdas: una version que solo probara la esquina (0,0) pasaria incluso con una
 * siembra que ignorara el argumento. */
static void selftest_first_click_is_safe(void)
{
    struct mines_board board;
    struct mines_board other;
    int failures = 0;
    int seed;

    memset(&board, 0, sizeof(board));
    memset(&other, 0, sizeof(other));
    for (seed = 1; seed <= 200; ++seed)
    {
        int cells;
        int index;

        mines_random_seed((uint32_t)seed);
        mines_board_reset(&board, MINES_LEVEL_EXPERT);
        cells = mines_board_cell_count(&board);
        index = (seed * 7919) % cells;
        if (!mines_board_reveal(&board, index % board.columns, index / board.columns))
        {
            failures += 1;
            continue;
        }
        if (board.state == MINES_STATE_LOST || board.cells[index].mine != 0u)
        {
            failures += 1;
        }
        if (mines_count_mines(&board) != board.mine_count)
        {
            failures += 1;
        }
        if (!mines_neighbours_consistent(&board))
        {
            failures += 1;
        }
    }
    expect(failures == 0, "el primer descubrimiento nunca pisa una mina");

    /* La siembra tiene que depender de la semilla: el mismo tablero en cada
     * partida es como se ve un generador roto. */
    {
        int same = 1;
        int index;

        mines_random_seed(1u);
        mines_board_reset(&board, MINES_LEVEL_EXPERT);
        (void)mines_board_reveal(&board, 0, 0);
        mines_random_seed(2u);
        mines_board_reset(&other, MINES_LEVEL_EXPERT);
        (void)mines_board_reveal(&other, 0, 0);
        for (index = 0; index < mines_board_cell_count(&board); ++index)
        {
            if (board.cells[index].mine != other.cells[index].mine)
            {
                same = 0;
                break;
            }
        }
        expect(!same, "dos semillas dan tableros distintos");

        /* La misma semilla, en cambio, tiene que dar el mismo tablero: sin eso
         * este selftest no podria afirmar nada dos veces. */
        mines_random_seed(1u);
        mines_board_reset(&other, MINES_LEVEL_EXPERT);
        (void)mines_board_reveal(&other, 0, 0);
        same = 1;
        for (index = 0; index < mines_board_cell_count(&board); ++index)
        {
            if (board.cells[index].mine != other.cells[index].mine)
            {
                same = 0;
                break;
            }
        }
        expect(same, "la misma semilla da el mismo tablero");
    }
}

/* Tablero armado a mano, que es la unica forma de afirmar algo exacto sobre la
 * cascada: una mina en (0,0) de un 4x4 deja tres celdas con vecinos y doce
 * vacias, asi que descubrir la esquina opuesta tiene que traer las 15 de una. */
static void selftest_flood(void)
{
    struct mines_board board;

    memset(&board, 0, sizeof(board));
    mines_board_reset(&board, MINES_LEVEL_BEGINNER);
    board.columns = 4;
    board.rows = 4;
    board.mine_count = 1;
    board.state = MINES_STATE_PLAYING;
    board.cells[0].mine = 1u;
    mines_count_neighbours(&board);

    expect(board.cells[mines_board_index(&board, 1, 1)].neighbours == 1u, "vecino diagonal cuenta");
    expect(board.cells[mines_board_index(&board, 2, 2)].neighbours == 0u, "celda lejana sin vecinos");

    expect(mines_board_reveal(&board, 3, 3) == 1, "descubrir la esquina opuesta");
    expect(board.revealed_count == 15, "la cascada trae las 15 celdas sin mina");
    expect(board.state == MINES_STATE_WON, "descubrir todo lo seguro gana");
    expect(board.cells[0].revealed == 0u, "la mina no se descubre al ganar");
    expect(board.cells[0].mark == MINES_MARK_FLAG, "al ganar se marcan las minas que faltan");
    expect(mines_board_remaining_mines(&board) == 0, "el contador cierra en cero");

    /* La bandera frena la cascada: con (1,1) marcada, descubrir (3,3) trae todo
     * menos esa celda. */
    mines_board_reset(&board, MINES_LEVEL_BEGINNER);
    board.columns = 4;
    board.rows = 4;
    board.mine_count = 1;
    board.state = MINES_STATE_PLAYING;
    board.cells[0].mine = 1u;
    mines_count_neighbours(&board);
    expect(mines_board_cycle_mark(&board, 1, 1) == 1, "marcar una celda tapada");
    expect(mines_board_reveal(&board, 3, 3) == 1, "descubrir con una bandera en el camino");
    expect(board.cells[mines_board_index(&board, 1, 1)].revealed == 0u,
           "la cascada no levanta banderas");
    expect(board.state == MINES_STATE_PLAYING, "con una celda sin descubrir no se gano");
}

static void selftest_marks(void)
{
    struct mines_board board;
    const struct mines_cell *cell;

    memset(&board, 0, sizeof(board));
    mines_board_reset(&board, MINES_LEVEL_BEGINNER);
    board.marks_enabled = 1;
    expect(mines_board_remaining_mines(&board) == 10, "contador inicial");
    expect(mines_board_cycle_mark(&board, 0, 0) == 1, "primera marca");
    cell = mines_board_cell_const(&board, 0, 0);
    expect(cell->mark == MINES_MARK_FLAG, "NONE -> FLAG");
    expect(mines_board_remaining_mines(&board) == 9, "la bandera descuenta del contador");
    (void)mines_board_cycle_mark(&board, 0, 0);
    expect(cell->mark == MINES_MARK_QUESTION, "FLAG -> QUESTION");
    expect(mines_board_remaining_mines(&board) == 10, "el signo de pregunta no descuenta");
    (void)mines_board_cycle_mark(&board, 0, 0);
    expect(cell->mark == MINES_MARK_NONE, "QUESTION -> NONE");

    /* Sin marcas el ciclo tiene dos estados, no tres. */
    board.marks_enabled = 0;
    (void)mines_board_cycle_mark(&board, 0, 0);
    (void)mines_board_cycle_mark(&board, 0, 0);
    expect(cell->mark == MINES_MARK_NONE, "sin marcas el ciclo salta el signo de pregunta");

    /* Poner mas banderas que minas es legal y el contador se va a negativo, que
     * es lo que muestra el original. */
    {
        int column;

        for (column = 0; column < 9; ++column)
        {
            (void)mines_board_cycle_mark(&board, column, 0);
            (void)mines_board_cycle_mark(&board, column, 1);
        }
        expect(mines_board_remaining_mines(&board) == -8, "el contador admite negativos");
    }

    /* Una celda descubierta no acepta marca. */
    mines_board_reset(&board, MINES_LEVEL_BEGINNER);
    mines_random_seed(11u);
    (void)mines_board_reveal(&board, 4, 4);
    expect(mines_board_cycle_mark(&board, 4, 4) == 0, "no se marca lo descubierto");

    /* Y una celda con bandera esta protegida del click izquierdo. La celda se
     * BUSCA en vez de clavarla: con 10 minas repartidas al azar, cualquier
     * coordenada fija puede haber quedado descubierta por la cascada, y el
     * selftest fallaria segun la semilla en vez de segun la regla. */
    {
        int index;
        int covered = -1;

        for (index = 0; index < mines_board_cell_count(&board); ++index)
        {
            if (board.cells[index].revealed == 0u)
            {
                covered = index;
                break;
            }
        }
        expect(covered >= 0, "siempre queda alguna celda tapada");
        if (covered >= 0)
        {
            int column = covered % board.columns;
            int row = covered / board.columns;

            expect(mines_board_cycle_mark(&board, column, row) == 1, "marcar para proteger");
            expect(mines_board_reveal(&board, column, row) == 0,
                   "la bandera protege del descubrimiento");
        }
    }
}

static void selftest_lose(void)
{
    struct mines_board board;
    int index;
    int mine_index = -1;
    int count;

    memset(&board, 0, sizeof(board));
    mines_random_seed(7u);
    mines_board_reset(&board, MINES_LEVEL_BEGINNER);
    (void)mines_board_reveal(&board, 0, 0);
    count = mines_board_cell_count(&board);
    for (index = 0; index < count; ++index)
    {
        if (board.cells[index].mine != 0u)
        {
            mine_index = index;
            break;
        }
    }
    expect(mine_index >= 0, "hay al menos una mina sembrada");
    if (mine_index < 0)
    {
        return;
    }
    expect(mines_board_reveal(&board, mine_index % board.columns, mine_index / board.columns) == 1,
           "descubrir una mina");
    expect(board.state == MINES_STATE_LOST, "descubrir una mina pierde");
    expect(board.cells[mine_index].exploded != 0u, "queda marcada la mina que explota");
    {
        int hidden_mines = 0;

        for (index = 0; index < count; ++index)
        {
            if (board.cells[index].mine != 0u && board.cells[index].revealed == 0u)
            {
                hidden_mines += 1;
            }
        }
        expect(hidden_mines == 0, "al perder se muestran todas las minas");
    }
    /* Partida terminada: no acepta mas jugadas. */
    expect(mines_board_reveal(&board, 8, 8) == 0, "perdida no acepta descubrir");
    expect(mines_board_cycle_mark(&board, 8, 8) == 0, "perdida no acepta marcar");
}

static void selftest_chord(void)
{
    struct mines_board board;

    /* 3x3 con una sola mina en (0,0). Descubierto (1,1) queda en 1: con la
     * bandera puesta sobre la mina, el acorde tiene que traer las otras siete. */
    memset(&board, 0, sizeof(board));
    mines_board_reset(&board, MINES_LEVEL_BEGINNER);
    board.columns = 3;
    board.rows = 3;
    board.mine_count = 1;
    board.state = MINES_STATE_PLAYING;
    board.cells[0].mine = 1u;
    mines_count_neighbours(&board);
    board.cells[mines_board_index(&board, 1, 1)].revealed = 1u;
    board.revealed_count = 1;

    expect(mines_board_chord(&board, 1, 1) == 0, "sin banderas el acorde no hace nada");
    (void)mines_board_cycle_mark(&board, 0, 0);
    expect(mines_board_chord(&board, 1, 1) == 1, "con la bandera correcta el acorde descubre");
    expect(board.state == MINES_STATE_WON, "el acorde puede ganar la partida");

    /* Con la bandera mal puesta el acorde pierde: es el atajo y tambien el
     * riesgo. */
    mines_board_reset(&board, MINES_LEVEL_BEGINNER);
    board.columns = 3;
    board.rows = 3;
    board.mine_count = 1;
    board.state = MINES_STATE_PLAYING;
    board.cells[0].mine = 1u;
    mines_count_neighbours(&board);
    board.cells[mines_board_index(&board, 1, 1)].revealed = 1u;
    board.revealed_count = 1;
    (void)mines_board_cycle_mark(&board, 2, 2);
    expect(mines_board_chord(&board, 1, 1) == 1, "acorde con bandera equivocada");
    expect(board.state == MINES_STATE_LOST, "una bandera mal puesta hace explotar el acorde");

    /* Sobre una celda tapada no hay acorde que hacer. */
    mines_board_reset(&board, MINES_LEVEL_BEGINNER);
    board.state = MINES_STATE_PLAYING;
    expect(mines_board_chord(&board, 0, 0) == 0, "no hay acorde sobre celda tapada");
}

static void selftest_scores(void)
{
    mines_scores_set_path(MINES_SELFTEST_SCORES_PATH);
    (void)savanxp_mkdir(MINES_SELFTEST_SCORES_DIR);
    (void)savanxp_unlink(MINES_SELFTEST_SCORES_PATH);

    mines_scores_load();
    expect(mines_scores_best(MINES_LEVEL_BEGINNER) == 0, "sin archivo no hay records");
    expect(mines_scores_record(MINES_LEVEL_BEGINNER, 42) == 1, "primer tiempo es record");
    expect(mines_scores_record(MINES_LEVEL_BEGINNER, 50) == 0, "un tiempo peor no es record");
    expect(mines_scores_best(MINES_LEVEL_BEGINNER) == 42, "el mejor se conserva");
    expect(mines_scores_record(MINES_LEVEL_BEGINNER, 41) == 1, "un tiempo mejor es record");
    expect(mines_scores_record(MINES_LEVEL_EXPERT, 0) == 0, "cero segundos no es un tiempo");

    /* Ida y vuelta por disco: el record tiene que sobrevivir al proceso. Si el
     * disco es de solo lectura -- la ISO live --, guardar no hace nada y esto no
     * se puede afirmar: se dice y se sigue, en vez de fallar por algo que no es
     * un bug del juego. */
    mines_scores_load();
    if (mines_scores_best(MINES_LEVEL_BEGINNER) == 0)
    {
        printf("MINES SMOKE SKIP records: el disco no acepto la escritura\n");
    }
    else
    {
        expect(mines_scores_best(MINES_LEVEL_BEGINNER) == 41, "el record sobrevive al archivo");
        mines_scores_reset();
        mines_scores_load();
        expect(mines_scores_best(MINES_LEVEL_BEGINNER) == 0, "reset borra los records");
    }

    (void)savanxp_unlink(MINES_SELFTEST_SCORES_PATH);
    mines_scores_set_path(0);
}

int mines_board_selftest(void)
{
    g_selftest_failures = 0;

    selftest_levels();
    selftest_first_click_is_safe();
    selftest_flood();
    selftest_marks();
    selftest_lose();
    selftest_chord();
    selftest_scores();

    if (g_selftest_failures != 0)
    {
        printf("MINES SMOKE FAIL %d checks\n", g_selftest_failures);
        return 1;
    }
    printf("MINES SMOKE PASS levels=%d expert=%dx%d/%d\n",
           MINES_LEVEL_COUNT,
           mines_level_info(MINES_LEVEL_EXPERT)->columns,
           mines_level_info(MINES_LEVEL_EXPERT)->rows,
           mines_level_info(MINES_LEVEL_EXPERT)->mines);
    return 0;
}
