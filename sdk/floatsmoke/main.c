/* Smoke del camino de punto flotante (-Sse).
 *
 * Se construye como app externa CON -Sse y se corre headless desde init:
 *
 *     .\build.ps1 float-smoke
 *
 * Cubre las tres cosas que pueden fallar por separado:
 *
 *   1. el ABI -- que los double y float entren y salgan de una llamada por
 *      xmm0..7 como manda System V, incluso a traves de variadicas;
 *   2. la libm de runtime/math.c, contra valores conocidos;
 *   3. que el kernel realmente preserve el estado FPU/SSE en un cambio de
 *      contexto, que es la precondicion de todo lo anterior y la unica que no
 *      se puede verificar compilando.
 *
 *   4. el formateo y la lectura de punto flotante de la libc (%f/%e/%g y
 *      strtod), que viven detras de __SSE2__ por el mismo motivo que la libm.
 *
 * Nota: las secciones 1-3 siguen reportando los valores escalados a entero,
 * para que un %f roto no se lleve puesto el diagnostico del ABI.
 */

#include "savanxp/libc.h"

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

union sx_double_bits {
    double value;
    uint64_t bits;
};

static void report(const char* name, int ok) {
    if (ok) {
        printf("  ok    %s\n", name);
    } else {
        printf("  FALLA %s\n", name);
        g_failures += 1;
    }
}

/* Compara bit a bit: para los valores exactamente representables que usa este
 * smoke, cualquier diferencia es un error de verdad y no ruido de redondeo. */
static int same_double(double got, double want) {
    union sx_double_bits a;
    union sx_double_bits b;

    a.value = got;
    b.value = want;
    return a.bits == b.bits;
}

/* Error en unidades de 1e-15, para poder reportarlo con %d sin tocar %f. */
static long error_units(double got, double want) {
    double difference = fabs(got - want);

    if (difference > 1.0) {
        return 1000000000L;
    }
    return (long)(difference * 1.0e15);
}

static void check_close(const char* name, double got, double want, long budget_units) {
    long units = error_units(got, want);

    if (units <= budget_units) {
        printf("  ok    %-22s error=%de-15\n", name, (int)units);
    } else {
        printf("  FALLA %-22s error=%de-15 (presupuesto %de-15)\n",
               name, (int)units, (int)budget_units);
        g_failures += 1;
    }
}

/* noinline a proposito: el punto es que los argumentos y el retorno crucen un
 * limite de funcion de verdad, por xmm0..7, y no que el compilador pliegue la
 * cuenta en tiempo de compilacion. */
__attribute__((noinline)) static double mix_doubles(double a, double b, double c, double d) {
    return a * b + c / d;
}

__attribute__((noinline)) static float mix_floats(float a, float b, float c) {
    return a * b - c;
}

/* Variadica con doubles: en el ABI de System V el que llama tiene que dejar en
 * al la cantidad de registros vectoriales usados, y el que recibe vuelca xmm0..7
 * al area de guardado. Es la parte del ABI que mas facil se rompe si los dos
 * lados no se compilaron con los mismos flags. */
__attribute__((noinline)) static double sum_varargs(int count, ...) {
    va_list args;
    double total = 0.0;
    int index;

    va_start(args, count);
    for (index = 0; index < count; index += 1) {
        total += va_arg(args, double);
    }
    va_end(args);
    return total;
}

static void check_abi(void) {
    volatile double a = 3.0;
    volatile double b = 0.5;
    volatile double c = 7.0;
    volatile double d = 2.0;
    volatile float fa = 2.5f;
    volatile float fb = 4.0f;
    volatile float fc = 1.5f;

    printf("[1] ABI de punto flotante\n");
    report("double por xmm (3*0.5 + 7/2 == 5)", same_double(mix_doubles(a, b, c, d), 5.0));
    report("float por xmm (2.5*4 - 1.5 == 8.5)", mix_floats(fa, fb, fc) == 8.5f);
    report("variadica con doubles (suma == 10)", same_double(sum_varargs(4, 1.0, 2.0, 3.0, 4.0), 10.0));

    {
        /* Ocho doubles fuerzan el uso de los ocho registros vectoriales de
         * argumento, que es donde se nota si alguno se pisa. */
        double total = sum_varargs(8, 1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0, 128.0);
        report("variadica con 8 doubles (== 255)", same_double(total, 255.0));
    }
}

static void check_libm(void) {
    printf("[2] libm (runtime/math.c)\n");

    /* Exactos: el resultado es representable, asi que se comparan bit a bit. */
    report("floor(-2.5) == -3", same_double(floor(-2.5), -3.0));
    report("ceil(-2.5) == -2", same_double(ceil(-2.5), -2.0));
    report("trunc(-2.5) == -2", same_double(trunc(-2.5), -2.0));
    report("round(-2.5) == -3", same_double(round(-2.5), -3.0));
    report("round(0.5) == 1", same_double(round(0.5), 1.0));
    report("fabs(-3.25) == 3.25", same_double(fabs(-3.25), 3.25));
    report("fmod(5.5, 2.0) == 1.5", same_double(fmod(5.5, 2.0), 1.5));
    report("fmod(-5.5, 2.0) == -1.5", same_double(fmod(-5.5, 2.0), -1.5));
    report("sqrt(16) == 4", same_double(sqrt(16.0), 4.0));
    report("copysign(3, -1) == -3", same_double(copysign(3.0, -1.0), -3.0));
    report("fmin(2, 3) == 2", same_double(fmin(2.0, 3.0), 2.0));
    report("fmax(2, 3) == 3", same_double(fmax(2.0, 3.0), 3.0));
    report("floorf(-2.5f) == -3f", floorf(-2.5f) == -3.0f);
    report("sqrtf(16f) == 4f", sqrtf(16.0f) == 4.0f);
    report("fmodf(5.5f, 2f) == 1.5f", fmodf(5.5f, 2.0f) == 1.5f);

    /* Aproximados: presupuesto en unidades de 1e-15. */
    check_close("sin(0)", sin(0.0), 0.0, 1);
    check_close("sin(pi/2)", sin(M_PI_2), 1.0, 10);
    check_close("sin(pi)", sin(M_PI), 0.0, 10);
    check_close("cos(0)", cos(0.0), 1.0, 1);
    check_close("cos(pi)", cos(M_PI), -1.0, 10);
    check_close("sin(pi/6)", sin(M_PI / 6.0), 0.5, 10);
    check_close("tan(pi/4)", tan(M_PI_4), 1.0, 100);
    check_close("atan(1)", atan(1.0), M_PI_4, 10);
    check_close("atan2(1,1)", atan2(1.0, 1.0), M_PI_4, 10);
    check_close("atan2(1,-1)", atan2(1.0, -1.0), 3.0 * M_PI_4, 10);
    check_close("sqrt(2)^2", sqrt(2.0) * sqrt(2.0), 2.0, 500);

    /* La reduccion de argumento tiene que seguir en pie lejos del origen. */
    check_close("sin(100*pi)", sin(100.0 * M_PI), 0.0, 5000);

    {
        volatile float angle = 1.0f;
        double as_double = (double)sinf(angle);
        check_close("sinf(1) vs sin(1)", as_double, sin(1.0), 100000000L);
    }
}

/* Deja un patron conocido en xmm5..xmm8, gira lo suficiente como para comerse
 * varias interrupciones de timer -- y por lo tanto varios cambios de contexto
 * hacia el otro proceso, que esta haciendo lo mismo con otro patron -- y los
 * vuelve a leer.
 *
 * Todo pasa dentro de UN bloque de asm a proposito. Si el compilador pudiera
 * meter mano entre el llenado y la lectura, spillearia esos registros a memoria
 * (en System V los xmm son caller-saved) y la prueba dejaria de mirar lo unico
 * que importa aca: lo que el kernel guarda y restaura con fxsave64/fxrstor64. */
static int fpu_registers_survive(uint64_t base, unsigned long spins) {
    uint64_t in0 = base;
    uint64_t in1 = base + 1u;
    uint64_t in2 = base + 2u;
    uint64_t in3 = base + 3u;
    uint64_t out0 = 0;
    uint64_t out1 = 0;
    uint64_t out2 = 0;
    uint64_t out3 = 0;
    unsigned long counter = spins;

    if (counter == 0) {
        return 1;
    }

    __asm__ volatile(
        "movq %[i0], %%xmm5\n\t"
        "movq %[i1], %%xmm6\n\t"
        "movq %[i2], %%xmm7\n\t"
        "movq %[i3], %%xmm8\n\t"
        "1:\n\t"
        "decq %[n]\n\t"
        "jnz 1b\n\t"
        "movq %%xmm5, %[o0]\n\t"
        "movq %%xmm6, %[o1]\n\t"
        "movq %%xmm7, %[o2]\n\t"
        "movq %%xmm8, %[o3]"
        : [o0] "=r"(out0), [o1] "=r"(out1), [o2] "=r"(out2), [o3] "=r"(out3),
          [n] "+r"(counter)
        : [i0] "r"(in0), [i1] "r"(in1), [i2] "r"(in2), [i3] "r"(in3)
        : "xmm5", "xmm6", "xmm7", "xmm8", "cc");

    if (out0 != in0 || out1 != in1 || out2 != in2 || out3 != in3) {
        printf("  xmm corrupto: base=%x got %x %x %x %x\n",
               (unsigned)base, (unsigned)out0, (unsigned)out1,
               (unsigned)out2, (unsigned)out3);
        return 0;
    }
    return 1;
}

/* Mismo espiritu que la de arriba pero con matematica de verdad: un lazo cerrado
 * de acumuladores que tiene que dar exactamente lo mismo corriendo solo que
 * compitiendo con otro proceso. */
static double fp_workload(double seed, int rounds) {
    double a = seed;
    double b = seed * 0.5;
    double c = seed + 1.0;
    double d = seed - 1.0;
    int index;

    for (index = 0; index < rounds; index += 1) {
        a = a * 0.9999 + 0.001;
        b = b * 0.9998 + 0.002;
        c = c * 0.9997 + 0.003;
        d = d * 0.9996 + 0.004;
    }

    return a + b + c + d;
}

#define SX_SPINS 4000000ul
#define SX_ROUNDS 6
#define SX_WORKLOAD_ROUNDS 20000

static int run_contended_checks(uint64_t base, double seed, double reference) {
    int ok = 1;
    int round;

    for (round = 0; round < SX_ROUNDS; round += 1) {
        if (!fpu_registers_survive(base + (uint64_t)round * 16u, SX_SPINS)) {
            ok = 0;
        }
        if (!same_double(fp_workload(seed, SX_WORKLOAD_ROUNDS), reference)) {
            printf("  workload divergio en la vuelta %d\n", round);
            ok = 0;
        }
    }

    return ok;
}

static void check_context_switch(void) {
    double parent_seed = 3.5;
    double child_seed = 11.25;
    double parent_reference;
    long child_pid;

    printf("[3] estado FPU/SSE a traves de cambios de contexto\n");

    /* Referencia tomada antes del fork, con el proceso solo. */
    parent_reference = fp_workload(parent_seed, SX_WORKLOAD_ROUNDS);

    child_pid = savanxp_fork();
    if (child_pid < 0) {
        printf("  FALLA fork (%d)\n", (int)child_pid);
        g_failures += 1;
        return;
    }

    if (child_pid == 0) {
        /* El hijo usa OTRO patron y otra semilla: si el kernel no separara el
         * estado FPU de cada proceso, cada uno veria los registros del otro. */
        double child_reference = fp_workload(child_seed, SX_WORKLOAD_ROUNDS);
        int ok = run_contended_checks(0x5A5A0000u, child_seed, child_reference);
        exit(ok ? 0 : 1);
    }

    {
        int ok = run_contended_checks(0xA1B20000u, parent_seed, parent_reference);
        int status = 0;

        savanxp_waitpid((int)child_pid, &status);
        report("padre: xmm y workload intactos", ok);
        report("hijo: xmm y workload intactos", status == 0);
    }
}

static void check_libm_extra(void) {
    printf("[5] libm: agregados\n");

    /* Exactos. */
    report("hypot(3,4) == 5", same_double(hypot(3.0, 4.0), 5.0));
    report("cbrt(-8) == -2", same_double(cbrt(-8.0), -2.0));
    report("rint(2.5) == 2 (al par)", same_double(rint(2.5), 2.0));
    report("rint(3.5) == 4 (al par)", same_double(rint(3.5), 4.0));
    report("rint(-2.5) == -2 (al par)", same_double(rint(-2.5), -2.0));
    report("lrint(2.5) == 2", lrint(2.5) == 2);
    report("lround(2.5) == 3", lround(2.5) == 3);
    report("lround(-2.5) == -3", lround(-2.5) == -3);
    report("scalbn(1, 10) == 1024", same_double(scalbn(1.0, 10), 1024.0));
    report("fdim(5,3) == 2", same_double(fdim(5.0, 3.0), 2.0));
    report("fdim(3,5) == 0", same_double(fdim(3.0, 5.0), 0.0));
    report("asin(0) == 0", same_double(asin(0.0), 0.0));
    report("acos(1) == 0", same_double(acos(1.0), 0.0));
    report("sinh(0) == 0", same_double(sinh(0.0), 0.0));
    report("cosh(0) == 1", same_double(cosh(0.0), 1.0));
    report("tanh(0) == 0", same_double(tanh(0.0), 0.0));
    report("log1p(0) == 0", same_double(log1p(0.0), 0.0));
    report("expm1(0) == 0", same_double(expm1(0.0), 0.0));
    report("acosh(1) == 0", same_double(acosh(1.0), 0.0));
    report("nextafter(1,2) > 1", nextafter(1.0, 2.0) > 1.0);
    report("nextafter(1,0) < 1", nextafter(1.0, 0.0) < 1.0);

    /* remainder redondea el cociente al par, a diferencia de fmod. */
    report("remainder(5,3) == -1", same_double(remainder(5.0, 3.0), -1.0));
    report("remainder(4,3) == 1", same_double(remainder(4.0, 3.0), 1.0));
    report("remainder(3,2) == -1", same_double(remainder(3.0, 2.0), -1.0));
    report("remainder(1,2) == 1", same_double(remainder(1.0, 2.0), 1.0));

    {
        double integral = 0.0;
        const double fraction = modf(3.75, &integral);
        report("modf(3.75) parte entera", same_double(integral, 3.0));
        report("modf(3.75) fraccion", same_double(fraction, 0.75));
    }
    {
        double integral = 0.0;
        const double fraction = modf(-3.75, &integral);
        report("modf(-3.75) parte entera", same_double(integral, -3.0));
        report("modf(-3.75) fraccion", same_double(fraction, -0.75));
    }
    {
        int exponent = 0;
        const double mantissa = frexp(8.0, &exponent);
        report("frexp(8) mantisa", same_double(mantissa, 0.5));
        report("frexp(8) exponente", exponent == 4);
    }
    {
        int exponent = 99;
        const double mantissa = frexp(0.0, &exponent);
        report("frexp(0)", same_double(mantissa, 0.0) && exponent == 0);
    }

    /* Aproximados, con presupuesto en unidades de 1e-15. */
    check_close("asin(1)", asin(1.0), M_PI_2, 10);
    check_close("asin(0.5)", asin(0.5), M_PI / 6.0, 10);
    check_close("acos(0)", acos(0.0), M_PI_2, 10);
    check_close("acos(-1)", acos(-1.0), M_PI, 10);
    check_close("cbrt(27)", cbrt(27.0), 3.0, 1000);
    check_close("log1p(1)", log1p(1.0), M_LN2, 10);
    check_close("expm1(1)", expm1(1.0), M_E - 1.0, 100);
    check_close("sinh(1)", sinh(1.0), 1.1752011936438014, 1000);
    check_close("cosh(1)", cosh(1.0), 1.5430806348152437, 1000);
    check_close("tanh(1)", tanh(1.0), 0.7615941559557649, 1000);
    check_close("tanh(30) == 1", tanh(30.0), 1.0, 1);
    check_close("asinh(1)", asinh(1.0), 0.8813735870195430, 1000);
    check_close("acosh(2)", acosh(2.0), 1.3169578969248166, 1000);
    check_close("atanh(0.5)", atanh(0.5), 0.5493061443340549, 1000);
    check_close("hypot(1e300,1e300)", hypot(1e300, 1e300) / 1e300, 1.4142135623730951, 10000);

    /* log1p cerca de cero es donde se nota que no es log(1+x) a secas. */
    check_close("log1p(1e-12) escalado", log1p(1e-12) * 1e12, 1.0, 100000);
    check_close("expm1(1e-12) escalado", expm1(1e-12) * 1e12, 1.0, 100000);

    /* Variantes float. */
    report("hypotf(3,4) == 5f", hypotf(3.0f, 4.0f) == 5.0f);
    report("cbrtf(-8) == -2f", cbrtf(-8.0f) == -2.0f);
    report("rintf(2.5f) == 2f", rintf(2.5f) == 2.0f);
    {
        float integral = 0.0f;
        const float fraction = modff(3.5f, &integral);
        report("modff(3.5f)", integral == 3.0f && fraction == 0.5f);
    }
}

static void expect_text(const char* name, const char* got, const char* want) {
    if (strcmp(got, want) == 0) {
        printf("  ok    %-24s '%s'\n", name, got);
    } else {
        printf("  FALLA %-24s '%s' (esperaba '%s')\n", name, got, want);
        g_failures += 1;
    }
}

static void check_formatting(void) {
    char buffer[128];

    printf("[4] formateo (%%f/%%e/%%g) y strtod\n");

    snprintf(buffer, sizeof(buffer), "%f", 1.5);
    expect_text("%f", buffer, "1.500000");
    snprintf(buffer, sizeof(buffer), "%f", -0.5);
    expect_text("%f negativo", buffer, "-0.500000");
    snprintf(buffer, sizeof(buffer), "%f", 0.0);
    expect_text("%f cero", buffer, "0.000000");
    snprintf(buffer, sizeof(buffer), "%.2f", 3.14159);
    expect_text("%.2f", buffer, "3.14");
    snprintf(buffer, sizeof(buffer), "%.2f", 0.999);
    expect_text("%.2f con acarreo", buffer, "1.00");
    snprintf(buffer, sizeof(buffer), "%.0f", 2.5);
    expect_text("%.0f", buffer, "3");
    snprintf(buffer, sizeof(buffer), "%.3f", 1.0 / 3.0);
    expect_text("%.3f", buffer, "0.333");
    snprintf(buffer, sizeof(buffer), "%10.2f|", 3.5);
    expect_text("%10.2f", buffer, "      3.50|");
    snprintf(buffer, sizeof(buffer), "%-10.2f|", 3.5);
    expect_text("%-10.2f", buffer, "3.50      |");

    snprintf(buffer, sizeof(buffer), "%e", 1500.0);
    expect_text("%e", buffer, "1.500000e+03");
    snprintf(buffer, sizeof(buffer), "%.2e", 0.000123);
    expect_text("%.2e", buffer, "1.23e-04");
    snprintf(buffer, sizeof(buffer), "%E", 1500.0);
    expect_text("%E", buffer, "1.500000E+03");

    snprintf(buffer, sizeof(buffer), "%g", 100000.0);
    expect_text("%g plano", buffer, "100000");
    snprintf(buffer, sizeof(buffer), "%g", 1000000.0);
    expect_text("%g exponencial", buffer, "1e+06");
    snprintf(buffer, sizeof(buffer), "%g", 0.0001);
    expect_text("%g chico plano", buffer, "0.0001");
    snprintf(buffer, sizeof(buffer), "%g", 0.00001);
    expect_text("%g chico exponencial", buffer, "1e-05");
    snprintf(buffer, sizeof(buffer), "%g", 1.5);
    expect_text("%g sin ceros de cola", buffer, "1.5");

    {
        const double zero = 0.0;
        const double one = 1.0;
        snprintf(buffer, sizeof(buffer), "%f", one / zero);
        expect_text("%f inf", buffer, "inf");
        snprintf(buffer, sizeof(buffer), "%f", -one / zero);
        expect_text("%f -inf", buffer, "-inf");
        snprintf(buffer, sizeof(buffer), "%f", zero / zero);
        expect_text("%f nan", buffer, "nan");
    }

    /* strtod: los valores exactos en binario se comparan bit a bit. */
    report("strtod 0.5", same_double(strtod("0.5", 0), 0.5));
    report("strtod -2.5e3", same_double(strtod("-2.5e3", 0), -2500.0));
    report("strtod con espacios", same_double(strtod("  42", 0), 42.0));
    check_close("strtod 3.14", strtod("3.14", 0), 3.14, 10);
    check_close("strtod 1e-3", strtod("1e-3", 0), 0.001, 10);

    {
        char* end = 0;
        double value = strtod("12.5rest", &end);
        report("strtod deja endptr",
               same_double(value, 12.5) && end != 0 && strcmp(end, "rest") == 0);
    }
    {
        char* end = 0;
        (void)strtod("abc", &end);
        report("strtod sin digitos no avanza", end != 0 && strcmp(end, "abc") == 0);
    }
    report("strtod inf", strtod("inf", 0) > 1.0e300);
    report("atof coincide con strtod", same_double(atof("2.25"), 2.25));
}

int main(void) {
    printf("FLOATSMOKE: arrancando\n");

    check_abi();
    check_libm();
    check_libm_extra();
    check_formatting();
    check_context_switch();

    if (g_failures == 0) {
        printf("FLOATSMOKE: todo ok\n");
        return 0;
    }

    printf("FLOATSMOKE: %d fallas\n", g_failures);
    return 1;
}
