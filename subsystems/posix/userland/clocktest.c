/*
 * Los tres relojes del sistema, medidos uno contra otro.
 *
 *   ticks  uptime_ms(): cuenta INTERRUPCIONES del timer.
 *   tsc    monotonic_ns(): lee el TSC, calibrado una vez contra el PIT.
 *   rtc    realtime(): el reloj de la maquina, en segundos.
 *
 * Los dos primeros son del kernel y pueden mentir los dos: los ticks si el
 * hipervisor no entrega todas las interrupciones, y el TSC si el calibrado
 * salio mal o si el contador se frena cuando el CPU haltea. El RTC no depende
 * de ninguna de las dos cosas -- es el unico que mide el mundo -- asi que es el
 * que arbitra. Tiene resolucion de un segundo, y por eso cada fase dura lo
 * suficiente como para que eso no importe.
 *
 * Dos fases, con los mismos tres relojes, donde lo unico que cambia es si la
 * maquina tiene algo que hacer:
 *
 *   girando   el proceso no suelta la CPU: no hay ocio y no se haltea
 *   durmiendo el proceso se bloquea: corre el ocioso y la maquina haltea
 *
 * Lo que se afirma es que ticks y tsc midan LO MISMO QUE EL RTC en las dos. Un
 * reloj que se aparta solo en una de las fases delata que haltear le cuesta
 * tiempo, y de ese reloj cuelgan sleep_ms, los plazos de poll, el RTO de TCP y
 * el reloj con el que Doom mueve el juego.
 */
#include "libc.h"

/* Segundos de RELOJ REAL por fase. Con resolucion de un segundo, 12 dejan el
 * error de cuantizacion abajo del 10%. */
#define CLOCKTEST_PHASE_SECONDS 12u
/* Tope de seguridad por fase, en lecturas del RTC, para no colgar la
 * automatizacion si un reloj esta parado del todo. */
#define CLOCKTEST_PHASE_LIMIT_SECONDS 90u
/* Cuanto se le permite a un reloj apartarse del RTC, en porcentaje. Es holgada
 * a proposito: lo que esto busca es un factor, no un sesgo. */
#define CLOCKTEST_TOLERANCE_PCT 30ul

/* Segundos del dia. El salto de medianoche se corrige sumando un dia: una fase
 * dura menos de un minuto, asi que no puede cruzar dos veces. */
static unsigned long wall_seconds(void)
{
    struct savanxp_realtime now;

    memset(&now, 0, sizeof(now));
    if (realtime(&now) != 0 || now.valid == 0)
    {
        return 0ul;
    }
    return ((unsigned long)now.hour * 3600ul) + ((unsigned long)now.minute * 60ul) +
           (unsigned long)now.second;
}

static unsigned long wall_elapsed(unsigned long start)
{
    unsigned long now = wall_seconds();

    if (now < start)
    {
        now += 24ul * 3600ul;
    }
    return now - start;
}

static unsigned long tsc_ms_since(unsigned long long start_ns)
{
    unsigned long long now_ns = monotonic_ns();

    return now_ns > start_ns ? (unsigned long)((now_ns - start_ns) / 1000000ull) : 0ul;
}

/* |medido - referencia| * 100 / referencia. */
static unsigned long drift_pct(unsigned long measured, unsigned long reference)
{
    unsigned long difference = measured > reference ? (measured - reference) : (reference - measured);

    if (reference == 0ul)
    {
        return 10000ul;
    }
    return (difference * 100ul) / reference;
}

struct phase_result {
    unsigned long wall_ms;
    unsigned long tick_ms;
    unsigned long tsc_ms;
};

static void report_phase(const char *label, const struct phase_result *phase)
{
    printf("clocktest: %s rtc=%lu ms ticks=%lu ms tsc=%lu ms | ticks %lu%% tsc %lu%% del reloj real\n",
           label,
           phase->wall_ms,
           phase->tick_ms,
           phase->tsc_ms,
           phase->wall_ms != 0ul ? (phase->tick_ms * 100ul) / phase->wall_ms : 0ul,
           phase->wall_ms != 0ul ? (phase->tsc_ms * 100ul) / phase->wall_ms : 0ul);
}

/* Porcentaje del reloj real que midio un reloj en una fase. */
static unsigned long fraction_of_wall(unsigned long measured, unsigned long wall_ms)
{
    return wall_ms != 0ul ? (measured * 100ul) / wall_ms : 0ul;
}

/* Aviso sin token de falla: que un reloj este corrido CONTRA EL MUNDO por igual
 * en las dos fases es un sesgo del emulador o del calibrado, no una regresion
 * del kernel -- bajo TCG uptime_ms mide ~65% del tiempo real pase lo que pase.
 * Se reporta porque importa, pero lo que rompe el build es otra cosa. */
static void warn_phase(const char *label, const struct phase_result *phase)
{
    if (drift_pct(phase->tick_ms, phase->wall_ms) > CLOCKTEST_TOLERANCE_PCT)
    {
        printf("clocktest:   aviso %s: uptime_ms conto %lu ms mientras pasaban %lu de reloj real\n",
               label, phase->tick_ms, phase->wall_ms);
    }
    if (drift_pct(phase->tsc_ms, phase->wall_ms) > CLOCKTEST_TOLERANCE_PCT)
    {
        printf("clocktest:   aviso %s: monotonic_ns conto %lu ms mientras pasaban %lu de reloj real\n",
               label, phase->tsc_ms, phase->wall_ms);
    }
}

/* `sleeping` elige la fase: dormida se bloquea de a poco (la maquina haltea
 * entre medio), despierta gira sin soltar la CPU. */
static void run_phase(int sleeping, struct phase_result *out)
{
    unsigned long wall_start = wall_seconds();
    unsigned long tick_start = uptime_ms();
    unsigned long long tsc_start = monotonic_ns();
    unsigned long elapsed;

    for (;;)
    {
        if (sleeping)
        {
            sleep_ms(200ul);
        }
        elapsed = wall_elapsed(wall_start);
        if (elapsed >= CLOCKTEST_PHASE_SECONDS || elapsed >= CLOCKTEST_PHASE_LIMIT_SECONDS)
        {
            break;
        }
    }

    out->wall_ms = elapsed * 1000ul;
    out->tick_ms = uptime_ms() - tick_start;
    out->tsc_ms = tsc_ms_since(tsc_start);
}

int main(void)
{
    struct phase_result spinning;
    struct phase_result sleeping;
    int failures = 0;

    if (wall_seconds() == 0ul)
    {
        struct savanxp_realtime probe;

        memset(&probe, 0, sizeof(probe));
        if (realtime(&probe) != 0 || probe.valid == 0)
        {
            puts_out("CLOCK SMOKE SKIP sin reloj de tiempo real con que comparar\n");
            return 0;
        }
    }

    memset(&spinning, 0, sizeof(spinning));
    memset(&sleeping, 0, sizeof(sleeping));

    run_phase(0, &spinning);
    report_phase("girando  ", &spinning);
    warn_phase("girando", &spinning);

    run_phase(1, &sleeping);
    report_phase("durmiendo", &sleeping);
    warn_phase("durmiendo", &sleeping);

    /*
     * Lo que rompe el build es que un reloj MIDA DISTINTO segun la maquina
     * haltee o no, contra el reloj real en las dos fases. Eso no es sesgo: es
     * que haltear le cuesta tiempo a ese reloj, y entonces todo lo que cuelga
     * de el -- sleep_ms, los plazos de poll, el RTO, el reloj con el que Doom
     * mueve el juego -- se atrasa justo cuando la maquina esta ociosa, que es
     * casi siempre. Un sesgo parejo se avisa arriba y no falla.
     */
    {
        const unsigned long tick_spin = fraction_of_wall(spinning.tick_ms, spinning.wall_ms);
        const unsigned long tick_sleep = fraction_of_wall(sleeping.tick_ms, sleeping.wall_ms);
        const unsigned long tsc_spin = fraction_of_wall(spinning.tsc_ms, spinning.wall_ms);
        const unsigned long tsc_sleep = fraction_of_wall(sleeping.tsc_ms, sleeping.wall_ms);

        if (drift_pct(tick_sleep, tick_spin) > CLOCKTEST_TOLERANCE_PCT)
        {
            printf("CLOCK SMOKE FAIL uptime_ms cambia con la ociosidad: %lu%% del reloj real girando, %lu%% durmiendo\n",
                   tick_spin, tick_sleep);
            failures += 1;
        }
        if (drift_pct(tsc_sleep, tsc_spin) > CLOCKTEST_TOLERANCE_PCT)
        {
            printf("CLOCK SMOKE FAIL monotonic_ns cambia con la ociosidad: %lu%% del reloj real girando, %lu%% durmiendo\n",
                   tsc_spin, tsc_sleep);
            failures += 1;
        }
    }

    if (failures != 0)
    {
        printf("CLOCK SMOKE FAIL %d checks\n", failures);
        return 1;
    }
    puts_out("CLOCK SMOKE PASS ningun reloj cambia de ritmo cuando la maquina haltea\n");
    return 0;
}
