#pragma once

#include "libc.h"

/*
 * Reglas del Buscaminas, sin ventana.
 *
 * Esta separado de mines.c por el mismo motivo que file_assoc de filesapp: lo
 * que se puede afirmar de un juego -- que el primer click nunca pisa una mina,
 * que el conteo de vecinos cierra, que la victoria se detecta cuando y solo
 * cuando falta descubrir exactamente las minas -- no necesita pantalla, y
 * probarlo mirando pixeles seria probarlo por el camino mas fragil. El
 * selftest de aca es el que corre `./build.sh smoke mines-smoke`.
 *
 * Sin malloc, igual que el resto del userland: el tablero es del tamano del
 * peor caso y vive en el llamador.
 */

/* El mayor tablero que ofrece el juego es el experto (30x16). Las filas van
 * hasta 24 porque es el limite del Buscaminas clasico para un tablero a medida,
 * y reservarlas ahora cuesta 720 bytes de celdas: el dia que exista el dialogo
 * "Custom" no hay que tocar ninguna capacidad. */
#define MINES_MAX_COLUMNS 30
#define MINES_MAX_ROWS 24
#define MINES_MAX_CELLS (MINES_MAX_COLUMNS * MINES_MAX_ROWS)

/* El contador de la esquina tiene tres digitos, y el clasico se planta en 999
 * en vez de dar la vuelta. Lo mismo vale para el reloj. */
#define MINES_COUNTER_MAX 999

enum mines_level
{
    MINES_LEVEL_BEGINNER = 0,
    MINES_LEVEL_INTERMEDIATE,
    MINES_LEVEL_EXPERT,
    MINES_LEVEL_COUNT
};

enum mines_state
{
    /* Tablero armado pero todavia sin minas: se siembran en el primer
     * descubrimiento, que es lo que garantiza que no explote. */
    MINES_STATE_READY = 0,
    MINES_STATE_PLAYING,
    MINES_STATE_WON,
    MINES_STATE_LOST
};

/* La marca del boton derecho cicla NONE -> FLAG -> QUESTION -> NONE. El paso
 * por QUESTION es opcional (Game > Marks (?)), como en el original. */
enum mines_mark
{
    MINES_MARK_NONE = 0,
    MINES_MARK_FLAG,
    MINES_MARK_QUESTION
};

struct mines_cell
{
    uint8_t mine;
    uint8_t revealed;
    uint8_t mark;        /* enum mines_mark */
    uint8_t neighbours;  /* minas adyacentes, 0..8 */
    /* La mina que termino la partida. Se dibuja sobre rojo: sin esto, un
     * tablero perdido no dice DONDE se perdio. */
    uint8_t exploded;
};

struct mines_level_info
{
    const char *name;
    int columns;
    int rows;
    int mines;
};

struct mines_board
{
    int level;          /* enum mines_level */
    int columns;
    int rows;
    int mine_count;
    int state;          /* enum mines_state */
    int revealed_count; /* celdas descubiertas, minas excluidas */
    int flag_count;     /* banderas puestas, correctas o no */
    /* 1 = el ciclo del boton derecho pasa por el signo de pregunta. */
    int marks_enabled;
    struct mines_cell cells[MINES_MAX_CELLS];
};

const struct mines_level_info *mines_level_info(int level);

/* Deja el tablero del nivel pedido vacio y en READY. No siembra minas: eso
 * pasa en el primer mines_board_reveal. */
void mines_board_reset(struct mines_board *board, int level);

int mines_board_cell_count(const struct mines_board *board);
int mines_board_index(const struct mines_board *board, int column, int row);
struct mines_cell *mines_board_cell(struct mines_board *board, int column, int row);
const struct mines_cell *mines_board_cell_const(const struct mines_board *board, int column, int row);

/* Minas que el jugador todavia no marco. Puede ser NEGATIVO -- poner mas
 * banderas que minas es legal y el contador clasico lo muestra en rojo. */
int mines_board_remaining_mines(const struct mines_board *board);

/*
 * Descubre una celda (boton izquierdo). Devuelve 1 si el tablero cambio.
 *
 * En el primer descubrimiento siembra las minas evitando la celda pedida, que
 * es lo que hace el original: el primer click no puede perder la partida, pero
 * tampoco se le regala un area vacia -- solo esa celda queda garantizada.
 *
 * Una celda con bandera esta protegida y no se descubre; una con signo de
 * pregunta si. Si la celda no tiene vecinos con mina, arrastra a las vecinas
 * en cascada (iterativa: la recursion sobre 720 celdas es pila que esta libc
 * no tiene para regalar).
 */
int mines_board_reveal(struct mines_board *board, int column, int row);

/* Cicla la marca de una celda tapada (boton derecho). Devuelve 1 si cambio. */
int mines_board_cycle_mark(struct mines_board *board, int column, int row);

/*
 * Acorde (boton del medio, o los dos botones): sobre un numero ya descubierto
 * con tantas banderas alrededor como dice el numero, descubre las vecinas sin
 * marcar. Es el atajo clasico -- y tambien la forma clasica de perder, porque
 * si una de esas banderas esta mal puesta, explota.
 */
int mines_board_chord(struct mines_board *board, int column, int row);

/* --- mejores tiempos ------------------------------------------------------
 *
 * Persisten en /disk/mines.ini, al lado de progman.ini y assoc.ini: es
 * configuracion del usuario, no un dato del programa. En una imagen de solo
 * lectura (la ISO live) la escritura falla en silencio y el juego funciona
 * igual, que es lo unico que puede hacer.
 */

#define MINES_SCORES_PATH "/disk/mines.ini"

/* Lee el archivo. Sin archivo deja los tres niveles sin record, que no es un
 * error: es una instalacion en la que nadie gano todavia. */
void mines_scores_load(void);
/* Segundos del mejor tiempo del nivel, o 0 si no hay ninguno. */
int mines_scores_best(int level);
/* Registra un tiempo. Devuelve 1 si es un record nuevo (y lo guarda). */
int mines_scores_record(int level, int seconds);
/* Borra los tres records, en memoria y en disco. */
void mines_scores_reset(void);
/* Redirige el archivo. Existe para el selftest, que no tiene por que pisar los
 * records del usuario para probar el formato. */
void mines_scores_set_path(const char *path);

/* --- azar -----------------------------------------------------------------
 *
 * Generador propio y no rand(): esta libc no tiene uno, y un juego necesita
 * poder FIJAR la semilla -- el selftest reparte tableros conocidos, y sin eso
 * "el primer click no pisa una mina" seria una afirmacion sobre una corrida y
 * no sobre la regla.
 */
void mines_random_seed(uint32_t seed);
/* Semilla del reloj, para una partida de verdad. */
void mines_random_seed_from_clock(void);

/* Valida siembra, conteo de vecinos, cascada, marcas, acorde, victoria,
 * derrota y el formato de los records. 0 si todo pasa. */
int mines_board_selftest(void);
