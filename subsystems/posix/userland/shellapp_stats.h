#pragma once

#include "libc.h"

/*
 * Medicion por repintado de la terminal.
 *
 * Existe por el mismo motivo que la de windowd: cuando "se siente pesada" hay
 * varias hipotesis que compiten y mirar la pantalla no las separa. Lo que se
 * mide es lo que las distingue:
 *
 *   - draw    -- el costo de pintar, que es lo que baja al recortar mejor.
 *   - present -- el camino a pantalla, que incluye quedarse bloqueado esperando
 *                a que el compositor termine el frame anterior. Si present
 *                domina, recortar el dibujo no se va a notar por mas que el
 *                numero de draw baje.
 *   - rows    -- cuantas filas de texto se re-blittean por repintado. Es LA
 *                cifra del repintado por filas: sin el, una linea nueva
 *                costaba las 24 filas visibles.
 *   - danio   -- pixeles presentados de verdad contra el bounding box que los
 *                contiene. La diferencia es lo que se ahorra pintando por
 *                rectangulo en vez de por banda.
 *
 * Los parpadeos del cursor se cuentan aparte: son presents de 16 pixeles y
 * promediarlos con los repintados de contenido tapa lo que se quiere ver.
 *
 * La linea sale por /dev/serial y no por stderr, que en una app grafica
 * terminaria pintando celdas de texto sobre el framebuffer.
 */

struct shellapp_stats {
    int serial_fd;              /* < 0 = sin canal, la medicion se apaga sola */
    unsigned long long window_begin_ns;
    unsigned long long draw_begin_ns;
    unsigned long long present_begin_ns;

    unsigned long redraws;
    unsigned long blinks;
    unsigned long long draw_ns_total;
    unsigned long long draw_ns_max;
    unsigned long long present_ns_total;
    unsigned long long present_ns_max;
    unsigned long long damage_px_total;
    unsigned long long bounds_px_total;
    unsigned long long rows_total;
    unsigned long long rects_total;
};

/* Abre /dev/serial. Si no esta, todo lo demas se vuelve no-op sin avisar: la
 * medicion nunca puede ser el motivo de que la terminal no arranque. */
void shellapp_stats_open(struct shellapp_stats* stats);
void shellapp_stats_close(struct shellapp_stats* stats);

void shellapp_stats_draw_begin(struct shellapp_stats* stats);
void shellapp_stats_draw_done(struct shellapp_stats* stats, unsigned long rows_painted);
void shellapp_stats_present_begin(struct shellapp_stats* stats);
void shellapp_stats_present_done(
    struct shellapp_stats* stats,
    unsigned long long damage_px,
    unsigned long long bounds_px,
    unsigned long rects);
void shellapp_stats_blink(struct shellapp_stats* stats);

/* Emite el resumen si vencio la ventana (o si `force`, al cerrar). */
void shellapp_stats_report(struct shellapp_stats* stats, int force);
