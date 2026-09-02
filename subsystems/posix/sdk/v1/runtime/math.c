/* Biblioteca matematica de userland.
 *
 * Se compila y linkea SOLO cuando la app se construye con -Sse. Sin SSE el
 * x86-64 no tiene un ABI de punto flotante utilizable: clang pasa los double en
 * registros de proposito general -- una convencion propia, no la de System V --
 * y resuelve cada operacion con una llamada a los helpers de soft-float
 * (__adddf3, __mulsf3 y compania) que este sistema no tiene, asi que el codigo
 * con floats compila pero no linkea. Con -msse2 los argumentos viajan en
 * xmm0..7 como manda el ABI y las operaciones son instrucciones nativas.
 *
 * El kernel ya sostiene este camino desde antes: guarda y restaura el area
 * FPU/SSE de cada proceso en el cambio de contexto (fxsave64/fxrstor64, ver
 * kernel/process.cpp) y crt0.S deja rsp alineado a 16 bytes antes del primer
 * call, que es lo que necesitan los accesos alineados de SSE.
 */

#if !defined(__SSE2__)
#error "math.c necesita SSE2: construi la app con -Sse (ver tools/build-user.ps1)."
#endif

#include <math.h>

#include <stdint.h>

/* Reinterpretar el double como entero es la unica forma de tocarle el exponente
 * y la mantisa sin pasar por la FPU. */
union sx_double_bits {
    double value;
    uint64_t bits;
};

union sx_float_bits {
    float value;
    uint32_t bits;
};

#define SX_DOUBLE_SIGN_MASK 0x8000000000000000ull
#define SX_DOUBLE_MANTISSA_MASK 0x000fffffffffffffull

double fabs(double x) {
    union sx_double_bits value;

    value.value = x;
    value.bits &= ~SX_DOUBLE_SIGN_MASK;
    return value.value;
}

double copysign(double x, double y) {
    union sx_double_bits magnitude;
    union sx_double_bits sign;

    magnitude.value = x;
    sign.value = y;
    magnitude.bits = (magnitude.bits & ~SX_DOUBLE_SIGN_MASK) | (sign.bits & SX_DOUBLE_SIGN_MASK);
    return magnitude.value;
}

double sqrt(double x) {
    double result;

    /* sqrtsd es exacta (correctamente redondeada) y es parte de SSE2, asi que
     * aca no hay nada que aproximar. */
    __asm__("sqrtsd %1, %0" : "=x"(result) : "x"(x));
    return result;
}

/* floor/ceil/trunc se resuelven con bits y no con roundsd: roundsd es SSE4.1 y
 * el modelo de CPU con el que arranca QEMU por default (qemu64) no la expone.
 * La linea base de este puerto es SSE2, que es lo que garantiza x86-64. */
double floor(double x) {
    union sx_double_bits value;
    int exponent;
    uint64_t fraction_mask;

    value.value = x;
    exponent = (int)((value.bits >> 52) & 0x7ffu) - 1023;

    /* Con exponente >= 52 ya no quedan bits fraccionarios que borrar. Por ahi
     * salen tambien infinitos y NaN (exponente 0x7ff), que se devuelven tal
     * cual. */
    if (exponent >= 52) {
        return x;
    }

    if (exponent < 0) {
        /* |x| < 1: el piso es -1 o 0, salvo los ceros, que conservan su signo. */
        if ((value.bits << 1) == 0) {
            return x;
        }
        return (value.bits & SX_DOUBLE_SIGN_MASK) != 0 ? -1.0 : 0.0;
    }

    fraction_mask = SX_DOUBLE_MANTISSA_MASK >> exponent;
    if ((value.bits & fraction_mask) == 0) {
        return x;
    }
    if ((value.bits & SX_DOUBLE_SIGN_MASK) != 0) {
        /* Negativo: el piso se aleja del cero. Sumar la mascara antes de
         * recortarla es lo que propaga el acarreo hacia la parte entera. */
        value.bits += fraction_mask;
    }
    value.bits &= ~fraction_mask;
    return value.value;
}

double ceil(double x) {
    return -floor(-x);
}

double trunc(double x) {
    union sx_double_bits value;
    int exponent;

    value.value = x;
    exponent = (int)((value.bits >> 52) & 0x7ffu) - 1023;
    if (exponent >= 52) {
        return x;
    }
    if (exponent < 0) {
        /* |x| < 1 trunca al cero de su propio signo. */
        value.bits &= SX_DOUBLE_SIGN_MASK;
        return value.value;
    }
    value.bits &= ~(SX_DOUBLE_MANTISSA_MASK >> exponent);
    return value.value;
}

double round(double x) {
    double integral = trunc(x);
    double fraction = x - integral;

    /* El medio se redondea alejandose del cero. Se mira la parte fraccionaria
     * en vez de sumar 0.5 y truncar, porque esa suma redondea hacia arriba
     * valores como 0.49999999999999994 y devolveria 1 en vez de 0. */
    if (fraction >= 0.5) {
        return integral + 1.0;
    }
    if (fraction <= -0.5) {
        return integral - 1.0;
    }
    return integral;
}

double fmod(double x, double y) {
    double result;

    /* fprem da el resto exacto de IEEE truncado hacia cero, pero reduce de a 64
     * exponentes por vuelta: si el cociente no entro entero deja C2 prendido en
     * el status word y hay que repetirla. El x87 sigue vivo con SSE prendido y
     * el fxsave del kernel guarda las dos mitades del estado, asi que apoyarse
     * en el sale mas exacto y mas corto que rehacer la division larga a mano. */
    __asm__ volatile(
        "fldl %2\n\t"
        "fldl %1\n\t"
        "1:\n\t"
        "fprem\n\t"
        "fnstsw %%ax\n\t"
        "testb $0x04, %%ah\n\t"
        "jnz 1b\n\t"
        "fstpl %0\n\t"
        "fstp %%st(0)"
        : "=m"(result)
        : "m"(x), "m"(y)
        : "ax", "st", "st(1)", "memory");
    return result;
}

double fmin(double x, double y) {
    /* Un NaN no es un minimo: el estandar pide devolver el otro operando. */
    if (x != x) {
        return y;
    }
    if (y != y) {
        return x;
    }
    return x < y ? x : y;
}

double fmax(double x, double y) {
    if (x != x) {
        return y;
    }
    if (y != y) {
        return x;
    }
    return x > y ? x : y;
}

/* Nucleos polinomicos validos para |y| <= pi/4, con los ajustes minimax
 * clasicos de fdlibm. En ese intervalo dan alrededor de 1 ulp, que es todo lo
 * que hace falta una vez reducido el argumento. */
static double sx_kernel_sin(double y) {
    static const double S1 = -1.66666666666666324348e-01;
    static const double S2 = 8.33333333332248946124e-03;
    static const double S3 = -1.98412698298579493134e-04;
    static const double S4 = 2.75573137070700676789e-06;
    static const double S5 = -2.50507602534068634195e-08;
    static const double S6 = 1.58969099521155010221e-10;

    double z = y * y;
    double tail = S2 + z * (S3 + z * (S4 + z * (S5 + z * S6)));

    return y + (y * z) * (S1 + z * tail);
}

static double sx_kernel_cos(double y) {
    static const double C1 = 4.16666666666666019037e-02;
    static const double C2 = -1.38888888888741095749e-03;
    static const double C3 = 2.48015872894767294178e-05;
    static const double C4 = -2.75573143513906633035e-07;
    static const double C5 = 2.08757232129817482790e-09;
    static const double C6 = -1.13596475577881948265e-11;

    double z = y * y;
    double tail = z * (C1 + z * (C2 + z * (C3 + z * (C4 + z * (C5 + z * C6)))));

    return 1.0 - 0.5 * z + tail * z;
}

/* pi/2 partido en dos doubles (reduccion de Cody-Waite): restar n*(pi/2) en dos
 * pasos conserva los bits bajos que un solo double de pi/2 ya no tiene.
 *
 * Alcanza de sobra para |x| < 2^20; de ahi para arriba la reduccion se va
 * quedando sin digitos de pi y el resultado pierde precision de a poco. La
 * tabla de pi de varios cientos de bits que usa una libm completa para arreglar
 * eso queda fuera de alcance mientras nadie la necesite. */
#define SX_PIO2_HI 1.57079632673412561417e+00
#define SX_PIO2_LO 6.07710050650619224932e-11
#define SX_TWO_OVER_PI 6.36619772367581382433e-01

/* Deja en *reduced el argumento llevado a [-pi/4, pi/4] y devuelve en que
 * cuadrante cayo el original (n mod 4). */
static int sx_reduce_pio2(double x, double* reduced) {
    double quadrants;

    /* Corta infinitos, NaN y magnitudes donde el double ya no distingue
     * multiplos de pi/2: ahi no hay reduccion posible y el resultado honesto es
     * NaN. */
    if (!(fabs(x) < 9.0e15)) {
        *reduced = x - x;
        return 0;
    }

    quadrants = round(x * SX_TWO_OVER_PI);
    *reduced = (x - quadrants * SX_PIO2_HI) - quadrants * SX_PIO2_LO;
    return (int)((int64_t)quadrants & 3);
}

double sin(double x) {
    double reduced;

    switch (sx_reduce_pio2(x, &reduced)) {
        case 0: return sx_kernel_sin(reduced);
        case 1: return sx_kernel_cos(reduced);
        case 2: return -sx_kernel_sin(reduced);
        default: return -sx_kernel_cos(reduced);
    }
}

double cos(double x) {
    double reduced;

    switch (sx_reduce_pio2(x, &reduced)) {
        case 0: return sx_kernel_cos(reduced);
        case 1: return -sx_kernel_sin(reduced);
        case 2: return -sx_kernel_cos(reduced);
        default: return sx_kernel_sin(reduced);
    }
}

double tan(double x) {
    double reduced;
    int quadrant = sx_reduce_pio2(x, &reduced);
    double sine = sx_kernel_sin(reduced);
    double cosine = sx_kernel_cos(reduced);

    /* En los cuadrantes impares el argumento reducido esta medido desde pi/2,
     * donde tan(pi/2 + y) = -cot(y). */
    if ((quadrant & 1) != 0) {
        return -cosine / sine;
    }
    return sine / cosine;
}

double atan2(double y, double x) {
    double result;

    /* fpatan calcula atan(st(1)/st(0)) y saca el cuadrante de los signos de los
     * dos operandos: eso es exactamente atan2, gratis y exacto. */
    __asm__ volatile(
        "fldl %1\n\t"
        "fldl %2\n\t"
        "fpatan\n\t"
        "fstpl %0"
        : "=m"(result)
        : "m"(y), "m"(x)
        : "st", "st(1)", "memory");
    return result;
}

double atan(double x) {
    return atan2(x, 1.0);
}

/* --- exponenciales y logaritmos --------------------------------------------
 *
 * Se apoyan en el x87, igual que fmod y atan2: fyl2x y f2xm1 dan el resultado
 * con 64 bits de mantisa, mejor que cualquier polinomio que entre en este
 * archivo, y en mucho menos codigo.
 *
 * La reduccion de argumento se hace en C y no con frndint/fscale a proposito.
 * Las formas de fsub/fsubr con DOS registros de x87 tienen el orden de
 * operandos invertido entre la sintaxis AT&T y la de Intel -- una trampa
 * historica de gas --, asi que las envolturas de asm de aca abajo toman un solo
 * operando y devuelven uno: no hay forma de equivocarse con el orden.
 */

/* x * 2^n, exacto. Los pasos intermedios existen para no perder el resultado
 * cuando n se va del rango de exponente de un solo double: multiplicar de a
 * tramos mantiene vivos los desbordes graduales. */
double ldexp(double x, int n) {
    union sx_double_bits scale;
    double y = x;

    if (n > 1023) {
        y *= 0x1p1023;
        n -= 1023;
        if (n > 1023) {
            y *= 0x1p1023;
            n -= 1023;
            if (n > 1023) {
                n = 1023;
            }
        }
    } else if (n < -1022) {
        /* El factor extra de 2^53 evita perder bits al pasar por subnormales. */
        y *= 0x1p-1022 * 0x1p53;
        n += 1022 - 53;
        if (n < -1022) {
            y *= 0x1p-1022 * 0x1p53;
            n += 1022 - 53;
            if (n < -1022) {
                n = -1022;
            }
        }
    }

    scale.bits = (uint64_t)(0x3ff + n) << 52;
    return y * scale.value;
}

/* 2^f - 1 para |f| <= 1. Es lo unico que sabe hacer f2xm1. */
static double sx_exp2m1_kernel(double f) {
    double result;

    __asm__ volatile(
        "fldl %1\n\t"
        "f2xm1\n\t"
        "fstpl %0"
        : "=m"(result)
        : "m"(f)
        : "st", "memory");
    return result;
}

/* Cola compartida de exp y pow: con z arriba de la pila, deja 2^z.
 *
 * Parte z en entero y fraccion para poder usar f2xm1, que solo vale en [-1, 1],
 * y despues reescala con fscale. La resta z - n se hace NEGANDO Y SUMANDO en vez
 * de con fsub: las formas de fsub/fsubr con dos registros de x87 tienen el orden
 * de operandos invertido entre AT&T e Intel, y la suma es conmutativa, asi que
 * por este camino no hay orden que equivocar.
 *
 * El punto de todo esto es que z NUNCA se guarda en un double intermedio:
 * componer exp como exp2(x * log2e) redondea el exponente antes de
 * exponenciarlo, y ese error se amplifica -- medido, 435 ulp en exp(-678). */
#define SX_X87_POW2_TAIL          \
    "fld %%st(0)\n\t"            /* z, z */          \
    "frndint\n\t"                /* n, z */          \
    "fld %%st(0)\n\t"            /* n, n, z */       \
    "fchs\n\t"                   /* -n, n, z */      \
    "faddp %%st(0), %%st(2)\n\t" /* n, f=z-n */      \
    "fxch %%st(1)\n\t"           /* f, n */          \
    "f2xm1\n\t"                  /* 2^f-1, n */      \
    "fld1\n\t"                                       \
    "faddp %%st(0), %%st(1)\n\t" /* 2^f, n */        \
    "fscale\n\t"                 /* 2^f * 2^n, n */  \
    "fstp %%st(1)\n\t"           /* resultado */

/* El control word del x87 trae un campo de PRECISION, y no todos los entornos
 * lo dejan igual: un kernel que arranca con fninit queda en extendida (64 bits
 * de mantisa), pero Windows, por ejemplo, lo pone en 53. La diferencia no es
 * academica -- las instrucciones que cargan constantes (fldl2e, fldln2)
 * devuelven la constante YA REDONDEADA a esa precision, asi que con 53 bits
 * exp arranca con un log2(e) degradado y el error se amplifica con |x|: medido
 * contra la libm del host, 3e-14 relativo en exp(700) contra 1e-17.
 *
 * Por eso estos kernels fijan la precision a extendida y reponen el control
 * word al salir, en vez de confiar en el que dejo puesto quien llamo. */
#define SX_X87_WIDEN_PRECISION     \
    "fnstcw %[saved]\n\t"         \
    "movw %[saved], %%ax\n\t"     \
    "orw $0x0300, %%ax\n\t"       \
    "movw %%ax, %[wide]\n\t"      \
    "fldcw %[wide]\n\t"

#define SX_X87_RESTORE_PRECISION "fldcw %[saved]\n\t"

/* 2^(y * log2(x)), todo adentro del x87. */
static double sx_pow_kernel(double y, double x) {
    double result;
    uint16_t saved_control;
    uint16_t wide_control;

    __asm__ volatile(
        SX_X87_WIDEN_PRECISION
        "fldl %[y]\n\t"
        "fldl %[x]\n\t"
        "fyl2x\n\t"
        SX_X87_POW2_TAIL
        "fstpl %[out]\n\t"
        SX_X87_RESTORE_PRECISION
        : [out] "=m"(result), [saved] "=m"(saved_control), [wide] "=m"(wide_control)
        : [y] "m"(y), [x] "m"(x)
        : "ax", "st", "st(1)", "st(2)", "st(3)", "memory");
    return result;
}

/* e^x = 2^(x * log2(e)), con log2(e) cargado por fldl2e. */
static double sx_exp_kernel(double x) {
    double result;
    uint16_t saved_control;
    uint16_t wide_control;

    __asm__ volatile(
        SX_X87_WIDEN_PRECISION
        "fldl2e\n\t"
        "fldl %[x]\n\t"
        "fmulp %%st(0), %%st(1)\n\t"
        SX_X87_POW2_TAIL
        "fstpl %[out]\n\t"
        SX_X87_RESTORE_PRECISION
        : [out] "=m"(result), [saved] "=m"(saved_control), [wide] "=m"(wide_control)
        : [x] "m"(x)
        : "ax", "st", "st(1)", "st(2)", "st(3)", "memory");
    return result;
}

/* y * log2(x) a secas, para los logaritmos. */
static double sx_ylog2x_kernel(double y, double x) {
    double result;

    __asm__ volatile(
        "fldl %1\n\t"
        "fldl %2\n\t"
        "fyl2x\n\t"
        "fstpl %0"
        : "=m"(result)
        : "m"(y), "m"(x)
        : "st", "st(1)", "memory");
    return result;
}

double log2(double x) {
    if (x != x) {
        return x;
    }
    if (x < 0.0) {
        return NAN;
    }
    if (x == 0.0) {
        return -HUGE_VAL;
    }
    return sx_ylog2x_kernel(1.0, x);
}

double log(double x) {
    if (x != x) {
        return x;
    }
    if (x < 0.0) {
        return NAN;
    }
    if (x == 0.0) {
        return -HUGE_VAL;
    }
    /* ln(x) = ln2 * log2(x), con el producto adentro del x87. */
    return sx_ylog2x_kernel(M_LN2, x);
}

double log10(double x) {
    if (x != x) {
        return x;
    }
    if (x < 0.0) {
        return NAN;
    }
    if (x == 0.0) {
        return -HUGE_VAL;
    }
    return sx_ylog2x_kernel(0.30102999566398119521, x);
}

double exp2(double x) {
    double whole;
    double fraction;

    if (x != x) {
        return x;
    }
    /* Fuera de estos limites el resultado ya no es representable: 2^1024 se
     * desborda y 2^-1075 se hunde por debajo del subnormal mas chico. */
    if (x >= 1024.0) {
        return HUGE_VAL;
    }
    if (x <= -1075.0) {
        return 0.0;
    }

    whole = round(x);
    fraction = x - whole; /* queda en [-0.5, 0.5], bien adentro del dominio de f2xm1 */
    return ldexp(sx_exp2m1_kernel(fraction) + 1.0, (int)whole);
}

double exp(double x) {
    if (x != x) {
        return x;
    }
    if (x > 710.0) {
        return HUGE_VAL;
    }
    if (x < -746.0) {
        return 0.0;
    }
    return sx_exp_kernel(x);
}

/* pow tiene mas casos borde que cuenta. Se resuelven antes de tocar el x87,
 * que no sabe nada de bases negativas ni de las convenciones del estandar. */
double pow(double x, double y) {
    int y_is_integer = 0;
    int y_is_odd_integer = 0;

    if (y == 0.0) {
        return 1.0; /* incluido pow(NaN, 0) == 1, que es lo que pide el estandar */
    }
    if (x == 1.0) {
        return 1.0;
    }
    if (x != x || y != y) {
        return NAN;
    }

    if (y == floor(y) && fabs(y) < 9.007199254740992e15) {
        y_is_integer = 1;
        y_is_odd_integer = fmod(fabs(y), 2.0) == 1.0;
    }

    if (x == 0.0) {
        if (y < 0.0) {
            return y_is_odd_integer && (1.0 / x) < 0.0 ? -HUGE_VAL : HUGE_VAL;
        }
        return y_is_odd_integer && (1.0 / x) < 0.0 ? -0.0 : 0.0;
    }

    if (x < 0.0) {
        /* Una base negativa solo tiene potencia real con exponente entero. */
        if (!y_is_integer) {
            return NAN;
        }
        {
            double magnitude = sx_pow_kernel(y, -x);
            return y_is_odd_integer ? -magnitude : magnitude;
        }
    }

    return sx_pow_kernel(y, x);
}

float ldexpf(float x, int n) {
    return (float)ldexp((double)x, n);
}

float log2f(float x) {
    return (float)log2((double)x);
}

float logf(float x) {
    return (float)log((double)x);
}

float log10f(float x) {
    return (float)log10((double)x);
}

float exp2f(float x) {
    return (float)exp2((double)x);
}

float expf(float x) {
    return (float)exp((double)x);
}

float powf(float x, float y) {
    return (float)pow((double)x, (double)y);
}

/* Las variantes float se calculan en doble y se redondean al final. Un double
 * tiene mas del doble de mantisa que un float, asi que el redondeo simple da el
 * mismo resultado que un nucleo dedicado en todos los casos que le importan a
 * una app, y ahorra duplicar los polinomios. sqrtf es la excepcion: tiene
 * instruccion propia, y ahi el doble redondeo si podria desviarse un ulp. */
float fabsf(float x) {
    union sx_float_bits value;

    value.value = x;
    value.bits &= 0x7fffffffu;
    return value.value;
}

float copysignf(float x, float y) {
    union sx_float_bits magnitude;
    union sx_float_bits sign;

    magnitude.value = x;
    sign.value = y;
    magnitude.bits = (magnitude.bits & 0x7fffffffu) | (sign.bits & 0x80000000u);
    return magnitude.value;
}

float sqrtf(float x) {
    float result;

    __asm__("sqrtss %1, %0" : "=x"(result) : "x"(x));
    return result;
}

float floorf(float x) {
    return (float)floor((double)x);
}

float ceilf(float x) {
    return (float)ceil((double)x);
}

float truncf(float x) {
    return (float)trunc((double)x);
}

float roundf(float x) {
    return (float)round((double)x);
}

float fmodf(float x, float y) {
    return (float)fmod((double)x, (double)y);
}

float fminf(float x, float y) {
    return (float)fmin((double)x, (double)y);
}

float fmaxf(float x, float y) {
    return (float)fmax((double)x, (double)y);
}

float sinf(float x) {
    return (float)sin((double)x);
}

float cosf(float x) {
    return (float)cos((double)x);
}

float tanf(float x) {
    return (float)tan((double)x);
}

float atanf(float x) {
    return (float)atan((double)x);
}

float atan2f(float y, float x) {
    return (float)atan2((double)y, (double)x);
}
