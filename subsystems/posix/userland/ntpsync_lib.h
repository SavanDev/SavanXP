#pragma once

#include <stdint.h>

/* SNTP de un tiro (RFC 4330, modo cliente): pide la hora por UDP, valida la
 * respuesta y fija el RTC por SET_REALTIME. Todo UTC, como el resto del
 * arbol. Sin punto flotante (userland va sin SSE): milesimos enteros.
 *
 * `ipv4` en orden host (a.b.c.d = (a<<24)|(b<<16)|(c<<8)|d), `port` el UDP
 * del servidor (123 por default en el protocolo). En exito devuelve 0 y deja
 * en `applied_unix` los segundos Unix aplicados y en `rtt_ms` la ida y vuelta
 * medida con uptime_ms. En fallo devuelve -errno: lo del socket tal cual,
 * -ETIMEDOUT si no contesta, -EIO si contesta basura o una hora imposible,
 * -EINVAL por argumento malo. */
long ntp_sync_once(uint32_t ipv4, unsigned int port, unsigned long timeout_ms,
                   long *applied_unix, unsigned long *rtt_ms);
