#include <stdio.h>

#include "libc.h"
#include "windowd_stats.h"

/* Cada cuanto sale la linea de resumen. Dos segundos es corto para seguir a
 * mano lo que cambia al mover una ventana, y largo para que el costo del write
 * no cuente contra lo que se esta midiendo. */
#define WINDOWD_STATS_INTERVAL_NS 2000000000ULL

static long long stats_area_of_region(const struct windowd_dirty_rect *dirty, unsigned long *rect_count)
{
    long long area = 0;
    size_t index;
    size_t count;

    count = windowd_dirty_rect_count(dirty);
    for (index = 0; index < count; ++index)
    {
        struct sx_rect rect;
        if (windowd_dirty_rect_at(dirty, index, &rect))
        {
            area += (long long)rect.width * (long long)rect.height;
        }
    }
    if (rect_count != 0)
    {
        *rect_count = (unsigned long)count;
    }
    return area;
}

void windowd_stats_open(struct windowd_stats *stats)
{
    if (stats == 0)
    {
        return;
    }
    memset(stats, 0, sizeof(*stats));
    stats->serial_fd = (int)savanxp_open_mode("/dev/serial", SAVANXP_OPEN_WRITE);
    stats->window_begin_ns = monotonic_ns();
}

void windowd_stats_close(struct windowd_stats *stats)
{
    if (stats == 0 || stats->serial_fd < 0)
    {
        return;
    }
    (void)savanxp_close(stats->serial_fd);
    stats->serial_fd = -1;
}

void windowd_stats_frame_begin(struct windowd_stats *stats)
{
    if (stats == 0 || stats->serial_fd < 0)
    {
        return;
    }
    stats->frame_begin_ns = monotonic_ns();
}

void windowd_stats_compose_done(struct windowd_stats *stats)
{
    if (stats == 0 || stats->serial_fd < 0)
    {
        return;
    }
    stats->compose_end_ns = monotonic_ns();
}

void windowd_stats_sync_begin(struct windowd_stats *stats)
{
    if (stats == 0 || stats->serial_fd < 0)
    {
        return;
    }
    stats->sync_begin_ns = monotonic_ns();
}

void windowd_stats_sync_end(struct windowd_stats *stats)
{
    unsigned long long now;

    if (stats == 0 || stats->serial_fd < 0 || stats->sync_begin_ns == 0)
    {
        return;
    }
    now = monotonic_ns();
    if (now > stats->sync_begin_ns)
    {
        stats->sync_pending_ns += now - stats->sync_begin_ns;
    }
    stats->sync_begin_ns = 0;
}

void windowd_stats_frame_end(
    struct windowd_stats *stats,
    const struct windowd_dirty_rect *dirty,
    struct savanxp_compositor_service_timing *timing)
{
    unsigned long long end_ns;
    unsigned long long compose_ns;
    unsigned long long present_ns;
    unsigned long rect_count = 0;
    struct sx_rect bounds;

    if (stats == 0 || stats->serial_fd < 0)
    {
        return;
    }
    /* Sin reloj calibrado monotonic_ns devuelve 0 y todos los intervalos darian
     * basura: se descarta el frame en vez de sumar ruido. */
    end_ns = monotonic_ns();
    if (stats->frame_begin_ns == 0 || stats->compose_end_ns < stats->frame_begin_ns ||
        end_ns < stats->compose_end_ns)
    {
        return;
    }

    compose_ns = stats->compose_end_ns - stats->frame_begin_ns;
    present_ns = end_ns - stats->compose_end_ns;

    stats->frames += 1u;
    stats->compose_ns_total += compose_ns;
    stats->present_ns_total += present_ns;
    if (compose_ns > stats->compose_ns_max)
    {
        stats->compose_ns_max = compose_ns;
    }
    if (present_ns > stats->present_ns_max)
    {
        stats->present_ns_max = present_ns;
    }

    stats->sync_ns_total += stats->sync_pending_ns;
    if (stats->sync_pending_ns > stats->sync_ns_max)
    {
        stats->sync_ns_max = stats->sync_pending_ns;
    }
    stats->sync_pending_ns = 0;

    if (timing != 0)
    {
        /* No se exige que service quede anidado adentro de present: con
           present asincrono (medido y revertido, docs/GRAPHICS_PERF.md) no lo
           esta, y descartar la muestra borraria el desglose. El ipc se acota
           a cero en el reporte. */
        stats->service_ns_total += timing->service_ns;
        stats->gpu_ns_total += timing->gpu_ns;
        stats->timeline_ns_total += timing->timeline_ns;
        memset(timing, 0, sizeof(*timing));
    }

    stats->damage_px_total += (unsigned long long)stats_area_of_region(dirty, &rect_count);
    stats->rects_total += rect_count;
    /* El bounding box es lo que se habria repintado y presentado cuando el
     * danio se fusionaba por bounding box: la diferencia contra damage es la
     * ganancia de la region exacta, medida en vivo y no en un test. */
    bounds = sx_region_bounds(&dirty->region);
    stats->bounds_px_total += (unsigned long long)bounds.width * (unsigned long long)bounds.height;
}

void windowd_stats_report(struct windowd_stats *stats, int force)
{
    char line[384];
    unsigned long long now_ns;
    unsigned long long elapsed_ns;
    unsigned long fps_tenths;
    unsigned long rects_tenths;
    unsigned long long service_ns;
    unsigned long long ipc_ns;
    unsigned long long svc_ns;
    int length;

    if (stats == 0 || stats->serial_fd < 0)
    {
        return;
    }

    now_ns = monotonic_ns();
    elapsed_ns = now_ns >= stats->window_begin_ns ? now_ns - stats->window_begin_ns : 0ULL;
    if (!force && elapsed_ns < WINDOWD_STATS_INTERVAL_NS)
    {
        return;
    }
    if (stats->frames == 0)
    {
        stats->window_begin_ns = now_ns;
        return;
    }

    /* Promedios en microsegundos: un frame se mide en decenas de miles de us
     * bajo TCG y en cientos bajo KVM, y en las dos escalas entra sin decimales.
     * fps sale del tiempo de pared de la ventana, no de la suma de los frames:
     * asi incluye lo que el compositor pasa esperando, que es justo lo que
     * distingue un escritorio quieto de uno saturado. */
    /* fps y rects en decimas: enteros truncaban a 0 justo donde importa --
       bajo TCG un frame puede tardar segundos, y "0 fps" no distingue un
       escritorio lento de uno colgado. window_ms va crudo para que el numero
       se pueda rehacer sin confiar en la division. */
    fps_tenths = (unsigned long)((stats->frames * 10000000000ULL) / (elapsed_ns != 0 ? elapsed_ns : 1ULL));
    rects_tenths = (unsigned long)((stats->rects_total * 10ULL) / stats->frames);

    /* Desglose, todo por frame. blk = sync + present: el tiempo que windowd
       pasa bloqueado en compositord, que es la cifra que hay que comparar
       entre present sincrono y asincrono (el asincrono MUEVE la espera del
       present al sync, no la borra de por si). ipc = present menos lo que
       compositord dice haber tardado, acotado a cero: con present sincrono
       es transporte + despertar al otro proceso; con asincrono cae a cero
       porque el servicio ya no queda adentro del present. */
    service_ns = stats->service_ns_total;
    ipc_ns = stats->present_ns_total > service_ns ? stats->present_ns_total - service_ns : 0ULL;
    svc_ns = service_ns > (stats->gpu_ns_total + stats->timeline_ns_total)
        ? service_ns - stats->gpu_ns_total - stats->timeline_ns_total
        : 0ULL;

    length = snprintf(
        line, sizeof(line),
        "windowd-stats: window_ms=%lu frames=%lu fps=%lu.%lu compose_us=%lu/%lu sync_us=%lu/%lu "
        "present_us=%lu/%lu blk_us=%lu ipc_us=%lu svc_us=%lu gpu_us=%lu tl_us=%lu "
        "damage_px=%lu bounds_px=%lu rects=%lu.%lu\n",
        (unsigned long)(elapsed_ns / 1000000ULL),
        stats->frames,
        fps_tenths / 10ul, fps_tenths % 10ul,
        (unsigned long)((stats->compose_ns_total / stats->frames) / 1000ULL),
        (unsigned long)(stats->compose_ns_max / 1000ULL),
        (unsigned long)((stats->sync_ns_total / stats->frames) / 1000ULL),
        (unsigned long)(stats->sync_ns_max / 1000ULL),
        (unsigned long)((stats->present_ns_total / stats->frames) / 1000ULL),
        (unsigned long)(stats->present_ns_max / 1000ULL),
        (unsigned long)(((stats->sync_ns_total + stats->present_ns_total) / stats->frames) / 1000ULL),
        (unsigned long)((ipc_ns / stats->frames) / 1000ULL),
        (unsigned long)((svc_ns / stats->frames) / 1000ULL),
        (unsigned long)((stats->gpu_ns_total / stats->frames) / 1000ULL),
        (unsigned long)((stats->timeline_ns_total / stats->frames) / 1000ULL),
        (unsigned long)(stats->damage_px_total / stats->frames),
        (unsigned long)(stats->bounds_px_total / stats->frames),
        rects_tenths / 10ul, rects_tenths % 10ul);

    if (length > 0)
    {
        (void)savanxp_write(stats->serial_fd, line, (unsigned long)length);
    }

    stats->frames = 0;
    stats->compose_ns_total = 0;
    stats->compose_ns_max = 0;
    stats->present_ns_total = 0;
    stats->present_ns_max = 0;
    stats->sync_ns_total = 0;
    stats->sync_ns_max = 0;
    stats->service_ns_total = 0;
    stats->gpu_ns_total = 0;
    stats->timeline_ns_total = 0;
    stats->damage_px_total = 0;
    stats->bounds_px_total = 0;
    stats->rects_total = 0;
    stats->window_begin_ns = now_ns;
}
