#pragma once

#include "savanxp/compositor_protocol.h"
#include "windowd_render.h"

/*
 * Medicion por frame del compositor.
 *
 * Existe porque hasta ahora no habia ninguna: windowd no leia el reloj en
 * ningun lado, asi que cada vez que se toco el camino de composicion la unica
 * forma de saber si sirvio era mirar la pantalla. Lo que se mide es lo que
 * separa las tres hipotesis que compiten cuando "se siente pesado":
 *
 *   - compose  -- el costo de componer el frame en la superficie de display.
 *   - present  -- el camino entero para poner el frame en pantalla, DESGLOSADO
 *                 en sus cuatro tramos: ipc (transporte por los pipes mas
 *                 despertar al otro proceso), svc (compositord fuera de sus
 *                 syscalls), gpu (el syscall de present: kernel + backend) y
 *                 tl (el ioctl de timeline que lo precede). Sin ese desglose
 *                 "present domina" no dice si atacar el protocolo o el driver.
 *   - danio    -- cuantos pixeles se repintan y presentan de verdad, contra el
 *                 bounding box que los contiene. La diferencia es lo que se
 *                 ahorra por acumular el danio como region exacta.
 *
 * La linea de reporte sale por /dev/serial, NO por stderr: stderr termina en
 * console::write_char, que pinta celdas de texto sobre el framebuffer y le
 * ensuciaria la pantalla al escritorio que se esta midiendo.
 */

struct windowd_stats
{
    int serial_fd;              /* < 0 = sin canal, la medicion se apaga sola */
    unsigned long long frame_begin_ns;
    unsigned long long compose_end_ns;
    /* El paso de sync (el round trip de SYNC_PRESENT) corre ANTES de decidir
       si hay frame, a veces varias vueltas seguidas. Se acumula aca y se le
       imputa al proximo frame que salga. */
    unsigned long long sync_begin_ns;
    unsigned long long sync_pending_ns;
    unsigned long long window_begin_ns;

    unsigned long frames;
    unsigned long long compose_ns_total;
    unsigned long long compose_ns_max;
    unsigned long long present_ns_total;
    unsigned long long present_ns_max;
    unsigned long long sync_ns_total;
    unsigned long long sync_ns_max;
    unsigned long long service_ns_total;
    unsigned long long gpu_ns_total;
    unsigned long long timeline_ns_total;
    unsigned long long damage_px_total;
    unsigned long long bounds_px_total;
    unsigned long long rects_total;
};

/* Abre /dev/serial. Si no esta, todo lo demas se vuelve no-op sin avisar: la
 * medicion nunca puede ser el motivo de que el escritorio no arranque. */
void windowd_stats_open(struct windowd_stats *stats);
void windowd_stats_close(struct windowd_stats *stats);

void windowd_stats_frame_begin(struct windowd_stats *stats);
void windowd_stats_compose_done(struct windowd_stats *stats);
void windowd_stats_sync_begin(struct windowd_stats *stats);
void windowd_stats_sync_end(struct windowd_stats *stats);
/* Cierra el frame y le suma el danio que se acaba de presentar, mas el
 * desglose que estampo compositord. `timing` se CONSUME: se suma y se deja en
 * cero, asi un mismo desglose no se cuenta dos veces. Puede ser 0. */
void windowd_stats_frame_end(
    struct windowd_stats *stats,
    const struct windowd_dirty_rect *dirty,
    struct savanxp_compositor_service_timing *timing);

/* Emite la linea de resumen si paso el intervalo (o si `force`), y reinicia la
 * ventana. Sin frames medidos no emite nada: un escritorio quieto no llena el
 * log de ceros. */
void windowd_stats_report(struct windowd_stats *stats, int force);
