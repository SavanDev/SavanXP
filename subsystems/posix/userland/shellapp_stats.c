#include <stdio.h>

#include "shellapp_stats.h"

/* Cada cuanto sale la linea de resumen. Dos segundos, igual que windowd: corto
 * para seguir a mano lo que cambia al correr un comando, y largo para que el
 * write no cuente contra lo que se esta midiendo. */
#define SHELLAPP_STATS_INTERVAL_NS 2000000000ULL

void shellapp_stats_open(struct shellapp_stats* stats) {
    if (stats == 0) {
        return;
    }
    memset(stats, 0, sizeof(*stats));
    stats->serial_fd = (int)savanxp_open_mode("/dev/serial", SAVANXP_OPEN_WRITE);
    stats->window_begin_ns = monotonic_ns();
}

void shellapp_stats_close(struct shellapp_stats* stats) {
    if (stats == 0 || stats->serial_fd < 0) {
        return;
    }
    (void)savanxp_close(stats->serial_fd);
    stats->serial_fd = -1;
}

void shellapp_stats_draw_begin(struct shellapp_stats* stats) {
    if (stats == 0 || stats->serial_fd < 0) {
        return;
    }
    stats->draw_begin_ns = monotonic_ns();
}

void shellapp_stats_draw_done(struct shellapp_stats* stats, unsigned long rows_painted) {
    unsigned long long elapsed_ns;

    if (stats == 0 || stats->serial_fd < 0) {
        return;
    }
    elapsed_ns = monotonic_ns() - stats->draw_begin_ns;
    stats->draw_ns_total += elapsed_ns;
    if (elapsed_ns > stats->draw_ns_max) {
        stats->draw_ns_max = elapsed_ns;
    }
    stats->rows_total += rows_painted;
}

void shellapp_stats_present_begin(struct shellapp_stats* stats) {
    if (stats == 0 || stats->serial_fd < 0) {
        return;
    }
    stats->present_begin_ns = monotonic_ns();
}

void shellapp_stats_present_done(
    struct shellapp_stats* stats,
    unsigned long long damage_px,
    unsigned long long bounds_px,
    unsigned long rects) {
    unsigned long long elapsed_ns;

    if (stats == 0 || stats->serial_fd < 0) {
        return;
    }
    elapsed_ns = monotonic_ns() - stats->present_begin_ns;
    stats->present_ns_total += elapsed_ns;
    if (elapsed_ns > stats->present_ns_max) {
        stats->present_ns_max = elapsed_ns;
    }
    stats->damage_px_total += damage_px;
    stats->bounds_px_total += bounds_px;
    stats->rects_total += rects;
    ++stats->redraws;
}

void shellapp_stats_blink(struct shellapp_stats* stats) {
    if (stats == 0 || stats->serial_fd < 0) {
        return;
    }
    ++stats->blinks;
}

void shellapp_stats_report(struct shellapp_stats* stats, int force) {
    char line[320];
    unsigned long long now_ns;
    unsigned long long elapsed_ns;
    unsigned long rows_tenths;
    unsigned long rects_tenths;
    int length;

    if (stats == 0 || stats->serial_fd < 0) {
        return;
    }

    now_ns = monotonic_ns();
    elapsed_ns = now_ns >= stats->window_begin_ns ? now_ns - stats->window_begin_ns : 0ULL;
    if (!force && elapsed_ns < SHELLAPP_STATS_INTERVAL_NS) {
        return;
    }
    /* Una terminal quieta no repinta nada: sin esto la ventana se reiniciaria
     * igual y el proximo resumen mediria un intervalo que no ocurrio. */
    if (stats->redraws == 0) {
        stats->window_begin_ns = now_ns;
        stats->blinks = 0;
        return;
    }

    /* rows y rects en decimas: el caso bueno es "una fila por repintado" y con
     * enteros no se distingue de cero ni de dos. */
    rows_tenths = (unsigned long)((stats->rows_total * 10ULL) / stats->redraws);
    rects_tenths = (unsigned long)((stats->rects_total * 10ULL) / stats->redraws);

    length = snprintf(
        line, sizeof(line),
        "shellapp-stats: window_ms=%lu redraws=%lu blinks=%lu draw_us=%lu/%lu "
        "present_us=%lu/%lu rows=%lu.%lu damage_px=%lu bounds_px=%lu rects=%lu.%lu\n",
        (unsigned long)(elapsed_ns / 1000000ULL),
        stats->redraws,
        stats->blinks,
        (unsigned long)((stats->draw_ns_total / stats->redraws) / 1000ULL),
        (unsigned long)(stats->draw_ns_max / 1000ULL),
        (unsigned long)((stats->present_ns_total / stats->redraws) / 1000ULL),
        (unsigned long)(stats->present_ns_max / 1000ULL),
        rows_tenths / 10ul, rows_tenths % 10ul,
        (unsigned long)(stats->damage_px_total / stats->redraws),
        (unsigned long)(stats->bounds_px_total / stats->redraws),
        rects_tenths / 10ul, rects_tenths % 10ul);

    if (length > 0) {
        (void)savanxp_write(stats->serial_fd, line, (unsigned long)length);
    }

    stats->window_begin_ns = now_ns;
    stats->redraws = 0;
    stats->blinks = 0;
    stats->draw_ns_total = 0;
    stats->draw_ns_max = 0;
    stats->present_ns_total = 0;
    stats->present_ns_max = 0;
    stats->damage_px_total = 0;
    stats->bounds_px_total = 0;
    stats->rows_total = 0;
    stats->rects_total = 0;
}
