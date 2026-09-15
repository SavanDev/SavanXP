/*
 * calc - la calculadora estandar del escritorio.
 *
 * ---- por que la aritmetica es decimal y hecha a mano ----------------------
 *
 * El motor es un flotante DECIMAL propio: mantisa de 16 digitos en int64 mas
 * exponente de 10. Nacio cuando la userland in-tree iba -mno-sse y `double` no
 * compilaba, pero se queda por su propio motivo, que es el que vale:
 *
 * decimal y no binario, porque es lo que espera quien usa una calculadora:
 * 0.1 + 0.2 da 0.3 exacto y no 0.30000000000000004. La deuda que
 * si se paga es la de siempre -- 1/3 vuelve a 0.9999999999999999 al
 * multiplicar por 3 --, que es el redondeo a 16 digitos y no un artefacto de
 * la base.
 *
 * Las cuentas intermedias van en __int128, pero la division de 128 bits no:
 * el compilador la resuelve llamando a __divti3 de compiler-rt, que este
 * sistema no linkea. Por eso wide_divmod() hace la division larga a mano.
 */

#include "libc.h"
#include "savanxp/sxgui.h"

#include <stdio.h>
#include <string.h>

/* ---- flotante decimal ----------------------------------------------------- */

typedef __int128 calc_wide;
typedef unsigned __int128 calc_uwide;

/* 16 digitos significativos: el maximo que entra en int64 dejando lugar para
 * que calc_add alinee dos operandos sin desbordar el intermedio de 128 bits. */
#define CALC_DIGITS 16
#define CALC_MANTISSA_MAX ((int64_t)10000000000000000)  /* 10^16 */
#define CALC_MANTISSA_MIN ((int64_t)1000000000000000)   /* 10^15 */

/* Tope del exponente. No es el limite de int: un resultado mas grande que esto
 * es "Overflow" para el usuario, y acotarlo evita razonar sobre exponentes que
 * ninguna pantalla puede mostrar. */
#define CALC_EXPONENT_LIMIT 4000

enum calc_status {
    CALC_OK = 0,
    CALC_DIVIDE_BY_ZERO,
    CALC_OVERFLOW,
    CALC_UNDEFINED
};

/* valor = mantissa * 10^exponent, con |mantissa| en [10^15, 10^16) o cero. */
struct calc_number {
    int64_t mantissa;
    int exponent;
};

static const int64_t k_power_of_ten[CALC_DIGITS + 2] = {
    1LL,
    10LL,
    100LL,
    1000LL,
    10000LL,
    100000LL,
    1000000LL,
    10000000LL,
    100000000LL,
    1000000000LL,
    10000000000LL,
    100000000000LL,
    1000000000000LL,
    10000000000000LL,
    100000000000000LL,
    1000000000000000LL,
    10000000000000000LL,
    100000000000000000LL
};

/* Division larga binaria de 128 bits. Existe para no depender de __divti3:
 * ver el comentario de cabecera. Es O(128) por division y eso aca no se nota
 * -- el peor caso es una tecla apretada. */
static void wide_divmod(calc_uwide numerator, calc_uwide denominator, calc_uwide *quotient, calc_uwide *remainder)
{
    calc_uwide result = 0;
    calc_uwide rest = 0;
    int bit;

    if (denominator == 0)
    {
        *quotient = 0;
        *remainder = 0;
        return;
    }
    for (bit = 127; bit >= 0; --bit)
    {
        rest = (rest << 1) | ((numerator >> bit) & (calc_uwide)1);
        if (rest >= denominator)
        {
            rest -= denominator;
            result |= ((calc_uwide)1) << bit;
        }
    }
    *quotient = result;
    *remainder = rest;
}

static calc_uwide wide_abs(calc_wide value)
{
    return value < 0 ? (calc_uwide)(-value) : (calc_uwide)value;
}

static calc_uwide wide_power_of_ten(int exponent)
{
    calc_uwide result = 1;
    int index;

    for (index = 0; index < exponent; ++index)
    {
        result *= 10u;
    }
    return result;
}

static struct calc_number calc_zero(void)
{
    struct calc_number value;

    value.mantissa = 0;
    value.exponent = 0;
    return value;
}

static int calc_is_zero(struct calc_number value)
{
    return value.mantissa == 0;
}

/* Constante exacta: `digits` significativos con el valor `mantissa` alineado a
 * la izquierda. calc_number_of(1, 0) es 1, calc_number_of(100, 3) es 100. */
static struct calc_number calc_number_of(int64_t value, int digits)
{
    struct calc_number number;
    int index;

    number.mantissa = value;
    number.exponent = 0;
    for (index = digits; index < CALC_DIGITS; ++index)
    {
        number.mantissa *= 10;
        number.exponent -= 1;
    }
    return number;
}

/*
 * Lleva cualquier par (mantisa, exponente) a la forma canonica: exactamente 16
 * digitos significativos. Es el unico lugar que redondea, y redondea al mas
 * cercano alejandose del cero, que es lo que hace una calculadora de
 * escritorio y no lo que hace el hardware (medio al par).
 */
static int calc_normalize(calc_wide mantissa, int exponent, struct calc_number *out)
{
    calc_uwide magnitude = wide_abs(mantissa);
    int negative = mantissa < 0;

    if (mantissa == 0)
    {
        *out = calc_zero();
        return CALC_OK;
    }

    /* De mas a menos digitos con UNA division por 10^sobrantes, en vez de
     * dividir por 10 en un bucle: cada division cuesta 128 pasos. El redondeo
     * puede devolver el valor a 10^16 (999...9 -> 1000...0), y por eso el
     * while vuelve a mirar -- la segunda vuelta ya no tiene resto. */
    while (magnitude >= (calc_uwide)CALC_MANTISSA_MAX)
    {
        calc_uwide quotient;
        calc_uwide remainder;
        calc_uwide probe = magnitude;
        calc_uwide divisor;
        int excess = 0;

        while (probe >= (calc_uwide)CALC_MANTISSA_MAX)
        {
            probe /= 10u;
            excess += 1;
        }
        divisor = wide_power_of_ten(excess);
        wide_divmod(magnitude, divisor, &quotient, &remainder);
        if (remainder * 2u >= divisor)
        {
            quotient += 1u;
        }
        magnitude = quotient;
        exponent += excess;
    }
    while (magnitude != 0 && magnitude < (calc_uwide)CALC_MANTISSA_MIN)
    {
        magnitude *= 10u;
        exponent -= 1;
    }

    if (exponent > CALC_EXPONENT_LIMIT)
    {
        return CALC_OVERFLOW;
    }
    if (exponent < -CALC_EXPONENT_LIMIT)
    {
        /* Bajo flujo: para quien mira la pantalla esto es cero, no un error. */
        *out = calc_zero();
        return CALC_OK;
    }

    out->mantissa = negative ? -(int64_t)magnitude : (int64_t)magnitude;
    out->exponent = exponent;
    return CALC_OK;
}

static struct calc_number calc_negate(struct calc_number value)
{
    value.mantissa = -value.mantissa;
    return value;
}

static int calc_add(struct calc_number left, struct calc_number right, struct calc_number *out)
{
    struct calc_number larger;
    struct calc_number smaller;
    int shift;

    if (calc_is_zero(left))
    {
        *out = right;
        return CALC_OK;
    }
    if (calc_is_zero(right))
    {
        *out = left;
        return CALC_OK;
    }

    larger = left.exponent >= right.exponent ? left : right;
    smaller = left.exponent >= right.exponent ? right : left;
    shift = larger.exponent - smaller.exponent;

    /* Mas de 17 ordenes de diferencia y el chico no alcanza ni al ultimo
     * digito del grande: sumarlo seria escribir el mismo numero, y ademas
     * 10^shift ya no entraria en el intermedio. */
    if (shift > CALC_DIGITS + 1)
    {
        *out = larger;
        return CALC_OK;
    }
    return calc_normalize(
        (calc_wide)larger.mantissa * k_power_of_ten[shift] + smaller.mantissa,
        smaller.exponent,
        out);
}

static int calc_subtract(struct calc_number left, struct calc_number right, struct calc_number *out)
{
    return calc_add(left, calc_negate(right), out);
}

static int calc_multiply(struct calc_number left, struct calc_number right, struct calc_number *out)
{
    return calc_normalize(
        (calc_wide)left.mantissa * right.mantissa,
        left.exponent + right.exponent,
        out);
}

static int calc_divide(struct calc_number left, struct calc_number right, struct calc_number *out)
{
    calc_uwide numerator;
    calc_uwide denominator;
    calc_uwide quotient;
    calc_uwide remainder;
    int negative;

    if (calc_is_zero(right))
    {
        return CALC_DIVIDE_BY_ZERO;
    }
    if (calc_is_zero(left))
    {
        *out = calc_zero();
        return CALC_OK;
    }

    negative = (left.mantissa < 0) != (right.mantissa < 0);
    /* 17 digitos de mas en el dividendo: el cociente sale con 17 o 18 digitos
     * y calc_normalize se queda con los 16 de arriba, ya redondeados. */
    numerator = wide_abs((calc_wide)left.mantissa) * wide_power_of_ten(CALC_DIGITS + 1);
    denominator = wide_abs((calc_wide)right.mantissa);
    wide_divmod(numerator, denominator, &quotient, &remainder);
    if (remainder * 2u >= denominator)
    {
        quotient += 1u;
    }
    return calc_normalize(
        negative ? -(calc_wide)quotient : (calc_wide)quotient,
        left.exponent - right.exponent - (CALC_DIGITS + 1),
        out);
}

/* Raiz entera de 128 bits por Newton. El arranque es una potencia de dos con
 * la mitad de los bits del radicando, que converge en una decena de pasos. */
static calc_uwide wide_isqrt(calc_uwide value)
{
    calc_uwide guess;
    calc_uwide probe = value;
    int bits = 0;

    if (value == 0)
    {
        return 0;
    }
    while (probe != 0)
    {
        probe >>= 1;
        bits += 1;
    }
    guess = ((calc_uwide)1) << ((bits + 1) / 2);
    for (;;)
    {
        calc_uwide quotient;
        calc_uwide remainder;
        calc_uwide next;

        wide_divmod(value, guess, &quotient, &remainder);
        next = (guess + quotient) / 2u;
        if (next >= guess)
        {
            return guess;
        }
        guess = next;
    }
}

static int calc_sqrt(struct calc_number value, struct calc_number *out)
{
    calc_uwide scaled;
    calc_uwide root;
    int shift = 20;
    int exponent;

    if (value.mantissa < 0)
    {
        return CALC_UNDEFINED;
    }
    if (calc_is_zero(value))
    {
        *out = calc_zero();
        return CALC_OK;
    }

    /* La raiz de 10^e solo es exacta con e par, asi que el corrimiento se
     * elige para dejarlo par. 16 + 21 digitos como mucho, que es lo que entra
     * en 128 bits, y dan 18 o 19 digitos de raiz: mas que los 16 con los que
     * se va a quedar calc_normalize. */
    exponent = value.exponent - shift;
    if (exponent % 2 != 0)
    {
        shift += 1;
        exponent -= 1;
    }
    scaled = (calc_uwide)value.mantissa * wide_power_of_ten(shift);
    root = wide_isqrt(scaled);
    return calc_normalize((calc_wide)root, exponent / 2, out);
}

/* ---- texto ---------------------------------------------------------------- */

#define CALC_TEXT_CAPACITY 48

struct calc_writer {
    char *buffer;
    int capacity;
    int length;
};

static void calc_put(struct calc_writer *writer, char character)
{
    if (writer->length + 1 < writer->capacity)
    {
        writer->buffer[writer->length] = character;
        writer->length += 1;
        writer->buffer[writer->length] = '\0';
    }
}

static void calc_put_number(struct calc_writer *writer, int value)
{
    char digits[12];
    int count = 0;

    if (value == 0)
    {
        calc_put(writer, '0');
        return;
    }
    while (value > 0 && count < (int)sizeof(digits))
    {
        digits[count] = (char)('0' + (value % 10));
        value /= 10;
        count += 1;
    }
    while (count > 0)
    {
        count -= 1;
        calc_put(writer, digits[count]);
    }
}

/*
 * Numero a texto. Notacion normal mientras el punto decimal caiga donde se lee
 * de un vistazo, y cientifica cuando no: un cero de mas o de menos en
 * "0.00000000000001" es un error de lectura garantizado.
 */
static void calc_format(struct calc_number value, char *out, int capacity)
{
    struct calc_writer writer;
    char digits[CALC_DIGITS + 1];
    int digit_count = CALC_DIGITS;
    int adjusted;
    int index;
    int64_t magnitude;

    writer.buffer = out;
    writer.capacity = capacity;
    writer.length = 0;
    if (capacity > 0)
    {
        out[0] = '\0';
    }

    if (calc_is_zero(value))
    {
        calc_put(&writer, '0');
        return;
    }

    magnitude = value.mantissa < 0 ? -value.mantissa : value.mantissa;
    for (index = CALC_DIGITS - 1; index >= 0; --index)
    {
        digits[index] = (char)('0' + (int)(magnitude % 10));
        magnitude /= 10;
    }
    digits[CALC_DIGITS] = '\0';
    while (digit_count > 1 && digits[digit_count - 1] == '0')
    {
        digit_count -= 1;
    }

    /* Potencia de diez del primer digito: el exponente de la notacion
     * cientifica y, a la vez, donde cae el punto en la notacion normal. */
    adjusted = value.exponent + (CALC_DIGITS - 1);

    if (value.mantissa < 0)
    {
        calc_put(&writer, '-');
    }

    if (adjusted > CALC_DIGITS - 1 || adjusted < -5)
    {
        calc_put(&writer, digits[0]);
        if (digit_count > 1)
        {
            calc_put(&writer, '.');
            for (index = 1; index < digit_count; ++index)
            {
                calc_put(&writer, digits[index]);
            }
        }
        calc_put(&writer, 'e');
        calc_put(&writer, adjusted < 0 ? '-' : '+');
        calc_put_number(&writer, adjusted < 0 ? -adjusted : adjusted);
        return;
    }

    if (adjusted >= digit_count - 1)
    {
        for (index = 0; index < digit_count; ++index)
        {
            calc_put(&writer, digits[index]);
        }
        for (index = digit_count - 1; index < adjusted; ++index)
        {
            calc_put(&writer, '0');
        }
        return;
    }
    if (adjusted >= 0)
    {
        for (index = 0; index <= adjusted; ++index)
        {
            calc_put(&writer, digits[index]);
        }
        calc_put(&writer, '.');
        for (index = adjusted + 1; index < digit_count; ++index)
        {
            calc_put(&writer, digits[index]);
        }
        return;
    }
    calc_put(&writer, '0');
    calc_put(&writer, '.');
    for (index = -1; index > adjusted; --index)
    {
        calc_put(&writer, '0');
    }
    for (index = 0; index < digit_count; ++index)
    {
        calc_put(&writer, digits[index]);
    }
}

/*
 * Texto a numero. Acepta lo que escribe el teclado y lo que llega del
 * portapapeles, que es texto de cualquiera: lo que no sea un digito, un punto
 * o el signo de adelante se ignora, en vez de rechazar la entrada entera.
 */
static struct calc_number calc_parse(const char *text)
{
    struct calc_number value = calc_zero();
    int64_t mantissa = 0;
    int exponent = 0;
    int negative = 0;
    int digits = 0;
    int after_point = 0;
    const char *cursor = text;

    if (cursor == 0)
    {
        return value;
    }
    while (*cursor == ' ')
    {
        cursor += 1;
    }
    if (*cursor == '-')
    {
        negative = 1;
        cursor += 1;
    }
    else if (*cursor == '+')
    {
        cursor += 1;
    }
    for (; *cursor != '\0'; ++cursor)
    {
        if (*cursor == '.')
        {
            after_point = 1;
            continue;
        }
        if (*cursor < '0' || *cursor > '9')
        {
            continue;
        }
        if (mantissa == 0 && *cursor == '0')
        {
            /* Los ceros de adelante no son digitos significativos; los de
             * despues del punto igual corren el exponente. */
            if (after_point)
            {
                exponent -= 1;
            }
            continue;
        }
        if (digits < CALC_DIGITS + 1)
        {
            mantissa = mantissa * 10 + (*cursor - '0');
            digits += 1;
            if (after_point)
            {
                exponent -= 1;
            }
        }
        else if (!after_point)
        {
            exponent += 1;
        }
    }
    if (negative)
    {
        mantissa = -mantissa;
    }
    (void)calc_normalize(mantissa, exponent, &value);
    return value;
}

/* ---- maquina de la calculadora -------------------------------------------- */

enum calc_operator {
    CALC_OP_NONE = 0,
    CALC_OP_ADD,
    CALC_OP_SUBTRACT,
    CALC_OP_MULTIPLY,
    CALC_OP_DIVIDE
};

/* Los digitos ocupan CALC_CMD_DIGIT_0 .. CALC_CMD_DIGIT_0 + 9. */
enum calc_command {
    CALC_CMD_NONE = 0,
    CALC_CMD_DIGIT_0 = 1,
    CALC_CMD_POINT = 16,
    CALC_CMD_ADD,
    CALC_CMD_SUBTRACT,
    CALC_CMD_MULTIPLY,
    CALC_CMD_DIVIDE,
    CALC_CMD_EQUALS,
    CALC_CMD_PERCENT,
    CALC_CMD_SQRT,
    CALC_CMD_RECIPROCAL,
    CALC_CMD_SIGN,
    CALC_CMD_BACKSPACE,
    CALC_CMD_CLEAR_ENTRY,
    CALC_CMD_CLEAR,
    CALC_CMD_MEMORY_CLEAR,
    CALC_CMD_MEMORY_RECALL,
    CALC_CMD_MEMORY_STORE,
    CALC_CMD_MEMORY_ADD,
    CALC_CMD_COPY,
    CALC_CMD_PASTE
};

/* Digitos que se dejan teclear. Uno mas que la mantisa no aportaria nada:
 * calc_parse lo redondearia igual. */
#define CALC_ENTRY_DIGITS CALC_DIGITS
/* signo + digitos + punto + NUL */
#define CALC_ENTRY_CAPACITY (CALC_ENTRY_DIGITS + 3)

static char g_entry[CALC_ENTRY_CAPACITY];
static int g_entering;
static struct calc_number g_display;
static struct calc_number g_accumulator;
static int g_pending;
/* Operador y operando del ultimo "=", para que volver a apretarlo repita la
 * operacion, como en la calculadora de siempre. */
static struct calc_number g_repeat_operand;
static int g_repeat_operator;
static struct calc_number g_memory;
static int g_memory_used;
static int g_status;
static char g_display_text[CALC_TEXT_CAPACITY];

static const char *calc_status_text(int status)
{
    switch (status)
    {
    case CALC_DIVIDE_BY_ZERO:
        return "Cannot divide by zero";
    case CALC_OVERFLOW:
        return "Overflow";
    case CALC_UNDEFINED:
        return "Invalid input";
    default:
        return "";
    }
}

static void calc_entry_reset(void)
{
    g_entry[0] = '0';
    g_entry[1] = '\0';
}

static void calc_sync_text(void)
{
    if (g_status != CALC_OK)
    {
        snprintf(g_display_text, sizeof(g_display_text), "%s", calc_status_text(g_status));
        return;
    }
    if (g_entering)
    {
        /* Lo tecleado se muestra tal cual se tecleo: mientras se escribe,
         * "0.50" no es "0.5". */
        snprintf(g_display_text, sizeof(g_display_text), "%s", g_entry);
        return;
    }
    calc_format(g_display, g_display_text, (int)sizeof(g_display_text));
}

static void calc_reset(void)
{
    calc_entry_reset();
    g_entering = 0;
    g_display = calc_zero();
    g_accumulator = calc_zero();
    g_pending = CALC_OP_NONE;
    g_repeat_operand = calc_zero();
    g_repeat_operator = CALC_OP_NONE;
    g_status = CALC_OK;
    calc_sync_text();
}

static struct calc_number calc_current(void)
{
    return g_entering ? calc_parse(g_entry) : g_display;
}

static int calc_entry_digit_count(void)
{
    int count = 0;
    int index;

    for (index = 0; g_entry[index] != '\0'; ++index)
    {
        if (g_entry[index] >= '0' && g_entry[index] <= '9')
        {
            if (count == 0 && g_entry[index] == '0')
            {
                continue;
            }
            count += 1;
        }
    }
    return count;
}

static int calc_entry_has_point(void)
{
    return strchr(g_entry, '.') != 0;
}

static void calc_entry_append(char character)
{
    int length = (int)strlen(g_entry);

    if (length + 1 < (int)sizeof(g_entry))
    {
        g_entry[length] = character;
        g_entry[length + 1] = '\0';
    }
}

static void calc_begin_entry(void)
{
    if (!g_entering)
    {
        calc_entry_reset();
        g_entering = 1;
    }
}

static void calc_type_digit(int digit)
{
    calc_begin_entry();
    if (calc_entry_digit_count() >= CALC_ENTRY_DIGITS)
    {
        return;
    }
    if (strcmp(g_entry, "0") == 0)
    {
        g_entry[0] = (char)('0' + digit);
        g_entry[1] = '\0';
        return;
    }
    if (strcmp(g_entry, "-0") == 0)
    {
        g_entry[1] = (char)('0' + digit);
        g_entry[2] = '\0';
        return;
    }
    calc_entry_append((char)('0' + digit));
}

static void calc_type_point(void)
{
    calc_begin_entry();
    if (!calc_entry_has_point())
    {
        calc_entry_append('.');
    }
}

static void calc_backspace(void)
{
    int length;

    if (!g_entering)
    {
        /* Sin nada tecleado no hay ultimo digito que borrar: lo que esta en
         * pantalla es el resultado de una cuenta, no una entrada. */
        return;
    }
    length = (int)strlen(g_entry);
    if (length > 0)
    {
        g_entry[length - 1] = '\0';
    }
    if (g_entry[0] == '\0' || strcmp(g_entry, "-") == 0)
    {
        calc_entry_reset();
    }
}

static void calc_toggle_sign(void)
{
    if (g_entering)
    {
        if (g_entry[0] == '-')
        {
            memmove(g_entry, g_entry + 1, strlen(g_entry));
            return;
        }
        {
            int length = (int)strlen(g_entry);

            if (length + 2 <= (int)sizeof(g_entry))
            {
                memmove(g_entry + 1, g_entry, (size_t)length + 1u);
                g_entry[0] = '-';
            }
        }
        return;
    }
    g_display = calc_negate(g_display);
}

static int calc_apply(int operation, struct calc_number left, struct calc_number right, struct calc_number *out)
{
    switch (operation)
    {
    case CALC_OP_ADD:
        return calc_add(left, right, out);
    case CALC_OP_SUBTRACT:
        return calc_subtract(left, right, out);
    case CALC_OP_MULTIPLY:
        return calc_multiply(left, right, out);
    case CALC_OP_DIVIDE:
        return calc_divide(left, right, out);
    default:
        *out = right;
        return CALC_OK;
    }
}

static void calc_set_operator(int operation)
{
    struct calc_number value = calc_current();

    if (g_pending != CALC_OP_NONE && g_entering)
    {
        struct calc_number result;
        int status = calc_apply(g_pending, g_accumulator, value, &result);

        if (status != CALC_OK)
        {
            g_status = status;
            return;
        }
        g_display = result;
        g_accumulator = result;
    }
    else
    {
        /* Cambiar de operador sin haber tecleado nada solo cambia el operador
         * pendiente: "5 + *" es "5 *", no "5 + 5 *". */
        g_accumulator = value;
        g_display = value;
    }
    g_pending = operation;
    g_entering = 0;
    g_repeat_operator = CALC_OP_NONE;
}

static void calc_equals(void)
{
    struct calc_number value = calc_current();
    struct calc_number result;
    int status;

    if (g_pending != CALC_OP_NONE)
    {
        g_repeat_operator = g_pending;
        g_repeat_operand = value;
        status = calc_apply(g_pending, g_accumulator, value, &result);
        g_pending = CALC_OP_NONE;
    }
    else if (g_repeat_operator != CALC_OP_NONE)
    {
        status = calc_apply(g_repeat_operator, value, g_repeat_operand, &result);
    }
    else
    {
        result = value;
        status = CALC_OK;
    }

    if (status != CALC_OK)
    {
        g_status = status;
        return;
    }
    g_display = result;
    g_accumulator = result;
    g_entering = 0;
}

/*
 * El % de una calculadora de escritorio no es un operador: reemplaza el
 * operando por un porcentaje. Con + y - es un porcentaje DEL acumulado
 * ("200 + 10 %" es 200 + 20), con * y / es el numero dividido por cien.
 */
static void calc_percent(void)
{
    struct calc_number value = calc_current();
    struct calc_number hundred = calc_number_of(100, 3);
    struct calc_number result;
    int status;

    if (g_pending == CALC_OP_ADD || g_pending == CALC_OP_SUBTRACT)
    {
        status = calc_multiply(g_accumulator, value, &result);
        if (status == CALC_OK)
        {
            status = calc_divide(result, hundred, &result);
        }
    }
    else
    {
        status = calc_divide(value, hundred, &result);
    }
    if (status != CALC_OK)
    {
        g_status = status;
        return;
    }
    g_display = result;
    g_entering = 0;
}

static void calc_unary(int command)
{
    struct calc_number value = calc_current();
    struct calc_number result;
    int status;

    if (command == CALC_CMD_SQRT)
    {
        status = calc_sqrt(value, &result);
    }
    else
    {
        status = calc_divide(calc_number_of(1, 1), value, &result);
    }
    if (status != CALC_OK)
    {
        g_status = status;
        return;
    }
    g_display = result;
    g_entering = 0;
}

static void calc_memory(int command)
{
    struct calc_number value = calc_current();
    struct calc_number result;

    switch (command)
    {
    case CALC_CMD_MEMORY_CLEAR:
        g_memory = calc_zero();
        g_memory_used = 0;
        break;
    case CALC_CMD_MEMORY_RECALL:
        g_display = g_memory;
        g_entering = 0;
        break;
    case CALC_CMD_MEMORY_STORE:
        g_memory = value;
        g_memory_used = !calc_is_zero(value);
        g_entering = 0;
        break;
    case CALC_CMD_MEMORY_ADD:
        if (calc_add(g_memory, value, &result) == CALC_OK)
        {
            g_memory = result;
            g_memory_used = !calc_is_zero(result);
        }
        g_entering = 0;
        break;
    default:
        break;
    }
}

static void calc_copy(void)
{
    (void)clipboard_set_text(g_display_text);
}

static void calc_paste(void)
{
    char buffer[CALC_TEXT_CAPACITY];

    if (clipboard_get_text(buffer, (int)sizeof(buffer)) <= 0)
    {
        return;
    }
    g_status = CALC_OK;
    g_display = calc_parse(buffer);
    g_entering = 0;
}

static void calc_execute(int command)
{
    if (command == CALC_CMD_NONE)
    {
        return;
    }
    if (command == CALC_CMD_CLEAR)
    {
        calc_reset();
        return;
    }
    if (command == CALC_CMD_CLEAR_ENTRY)
    {
        calc_entry_reset();
        g_entering = 1;
        g_status = CALC_OK;
        calc_sync_text();
        return;
    }
    if (g_status != CALC_OK)
    {
        /* Con un error en pantalla la maquina queda detenida hasta que alguien
         * la limpie: seguir aceptando teclas daria resultados armados sobre un
         * valor que no existe. */
        return;
    }

    if (command >= CALC_CMD_DIGIT_0 && command <= CALC_CMD_DIGIT_0 + 9)
    {
        calc_type_digit(command - CALC_CMD_DIGIT_0);
    }
    else
    {
        switch (command)
        {
        case CALC_CMD_POINT:
            calc_type_point();
            break;
        case CALC_CMD_ADD:
            calc_set_operator(CALC_OP_ADD);
            break;
        case CALC_CMD_SUBTRACT:
            calc_set_operator(CALC_OP_SUBTRACT);
            break;
        case CALC_CMD_MULTIPLY:
            calc_set_operator(CALC_OP_MULTIPLY);
            break;
        case CALC_CMD_DIVIDE:
            calc_set_operator(CALC_OP_DIVIDE);
            break;
        case CALC_CMD_EQUALS:
            calc_equals();
            break;
        case CALC_CMD_PERCENT:
            calc_percent();
            break;
        case CALC_CMD_SQRT:
        case CALC_CMD_RECIPROCAL:
            calc_unary(command);
            break;
        case CALC_CMD_SIGN:
            calc_toggle_sign();
            break;
        case CALC_CMD_BACKSPACE:
            calc_backspace();
            break;
        case CALC_CMD_MEMORY_CLEAR:
        case CALC_CMD_MEMORY_RECALL:
        case CALC_CMD_MEMORY_STORE:
        case CALC_CMD_MEMORY_ADD:
            calc_memory(command);
            break;
        case CALC_CMD_COPY:
            calc_copy();
            break;
        case CALC_CMD_PASTE:
            calc_paste();
            break;
        default:
            break;
        }
    }
    calc_sync_text();
}

/*
 * Teclado. Las letras son las de la calculadora clasica salvo una: ahi C se
 * apretaba con ESC, y aca ESC es "cerrar la ventana" en todas las apps del
 * sistema, asi que C vive en la tecla 'c' y Del sigue siendo CE.
 */
static int calc_command_for_ascii(int ascii)
{
    if (ascii >= '0' && ascii <= '9')
    {
        return CALC_CMD_DIGIT_0 + (ascii - '0');
    }
    switch (ascii)
    {
    case '.':
    case ',':
        return CALC_CMD_POINT;
    case '+':
        return CALC_CMD_ADD;
    case '-':
        return CALC_CMD_SUBTRACT;
    case '*':
        return CALC_CMD_MULTIPLY;
    case '/':
        return CALC_CMD_DIVIDE;
    case '=':
        return CALC_CMD_EQUALS;
    case '%':
        return CALC_CMD_PERCENT;
    case '@':
    case 'q':
    case 'Q':
        return CALC_CMD_SQRT;
    case 'r':
    case 'R':
        return CALC_CMD_RECIPROCAL;
    case 'n':
    case 'N':
        return CALC_CMD_SIGN;
    case 'c':
    case 'C':
        return CALC_CMD_CLEAR;
    default:
        return CALC_CMD_NONE;
    }
}

/* ---- interfaz ------------------------------------------------------------- */

/*
 * La grilla es la de la calculadora estandar: la columna de memoria a la
 * izquierda, los digitos en el bloque de tres, los operadores y las funciones
 * a la derecha. Las medidas salen de las del toolkit -- el mismo GAP y el
 * mismo margen que usan las demas ventanas -- para que la calculadora al lado
 * de otra app no se lea como de otro sistema.
 */
#define CALC_KEY_MIN_WIDTH 32
#define CALC_KEY_HEIGHT 24
#define CALC_COLUMNS 6
#define CALC_ROWS 4
#define CALC_DISPLAY_HEIGHT 26
#define CALC_INDICATOR_WIDTH CALC_KEY_MIN_WIDTH
#define CALC_TOP_KEY_COUNT 3
#define CALC_WIDGET_COUNT (CALC_TOP_KEY_COUNT + CALC_ROWS * CALC_COLUMNS)

/*
 * El ancho no es una constante: sale de medir el rotulo mas largo.
 *
 * "Backspace" no entra en un boton de 54 px y se derramaba sobre el de al
 * lado. Fijar 72 a mano lo tapa hoy y se vuelve a romper con otra fuente o con
 * otro rotulo, asi que la fila de arriba se dimensiona por el texto y la
 * grilla de teclas se estira hasta igualarla: las dos filas terminan siempre
 * en la misma columna. Es lo mismo que hace sxgui_tabs_preferred_width.
 */
static int g_key_width = CALC_KEY_MIN_WIDTH;
static int g_top_button_width;
static int g_grid_width;

static void calc_measure(void)
{
    int label = gfx_text_width("Backspace") + (SXGUI_BORDER_RAISED + SXGUI_TEXT_PAD) * 2;
    int top_width;

    g_top_button_width = label > SXGUI_BUTTON_WIDTH ? label : SXGUI_BUTTON_WIDTH;
    top_width = CALC_INDICATOR_WIDTH + SXGUI_GAP * 3 + g_top_button_width * 3;

    /* Techo de la division: la grilla nunca sale mas angosta que la fila. */
    g_key_width = (top_width - SXGUI_GAP * (CALC_COLUMNS - 1) + CALC_COLUMNS - 1) / CALC_COLUMNS;
    if (g_key_width < CALC_KEY_MIN_WIDTH)
    {
        g_key_width = CALC_KEY_MIN_WIDTH;
    }
    g_grid_width = g_key_width * CALC_COLUMNS + SXGUI_GAP * (CALC_COLUMNS - 1);
    /* Y ahora al reves, para repartir el sobrante del redondeo: los botones de
     * arriba se estiran hasta el borde derecho de la grilla. */
    g_top_button_width = (g_grid_width - CALC_INDICATOR_WIDTH - SXGUI_GAP * 3) / 3;
}

struct calc_key {
    const char *label;
    int command;
};

/* No son const porque viajan como el `user` del widget, que es void*. */
static struct calc_key g_top_keys[CALC_TOP_KEY_COUNT] = {
    {"Backspace", CALC_CMD_BACKSPACE},
    {"CE", CALC_CMD_CLEAR_ENTRY},
    {"C", CALC_CMD_CLEAR}
};

static struct calc_key g_keys[CALC_ROWS * CALC_COLUMNS] = {
    {"MC", CALC_CMD_MEMORY_CLEAR},
    {"7", CALC_CMD_DIGIT_0 + 7},
    {"8", CALC_CMD_DIGIT_0 + 8},
    {"9", CALC_CMD_DIGIT_0 + 9},
    {"/", CALC_CMD_DIVIDE},
    {"sqrt", CALC_CMD_SQRT},

    {"MR", CALC_CMD_MEMORY_RECALL},
    {"4", CALC_CMD_DIGIT_0 + 4},
    {"5", CALC_CMD_DIGIT_0 + 5},
    {"6", CALC_CMD_DIGIT_0 + 6},
    {"*", CALC_CMD_MULTIPLY},
    {"%", CALC_CMD_PERCENT},

    {"MS", CALC_CMD_MEMORY_STORE},
    {"1", CALC_CMD_DIGIT_0 + 1},
    {"2", CALC_CMD_DIGIT_0 + 2},
    {"3", CALC_CMD_DIGIT_0 + 3},
    {"-", CALC_CMD_SUBTRACT},
    {"1/x", CALC_CMD_RECIPROCAL},

    {"M+", CALC_CMD_MEMORY_ADD},
    {"0", CALC_CMD_DIGIT_0 + 0},
    {"+/-", CALC_CMD_SIGN},
    {".", CALC_CMD_POINT},
    {"+", CALC_CMD_ADD},
    {"=", CALC_CMD_EQUALS}
};

static struct sxgui_app g_app;
static struct sxgui_widget g_widgets[CALC_WIDGET_COUNT];
static struct sx_rect g_display_rect;
static struct sx_rect g_indicator_rect;

static void on_key_press(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    calc_execute(((struct calc_key *)user)->command);
    sxgui_app_request_repaint(&g_app);
}

/*
 * Layout fijo y por eso calculado una sola vez: una calculadora no tiene nada
 * que estirar. Si el WM le da otro tamano, la grilla se queda donde esta en
 * lugar de repartirse el espacio, que es lo que hace un cuadro de dialogo.
 */
static void calc_layout(void)
{
    int left = SXGUI_CONTENT_MARGIN;
    int top = sxgui_menubar_height() + SXGUI_CONTENT_MARGIN;
    int row;
    int column;
    int index;

    calc_measure();

    g_display_rect = sx_rect_make(left, top, g_grid_width, CALC_DISPLAY_HEIGHT);
    top += CALC_DISPLAY_HEIGHT + SXGUI_GAP * 2;

    g_indicator_rect = sx_rect_make(left, top, CALC_INDICATOR_WIDTH, CALC_KEY_HEIGHT);
    for (index = 0; index < CALC_TOP_KEY_COUNT; ++index)
    {
        int x = left + CALC_INDICATOR_WIDTH + SXGUI_GAP + index * (g_top_button_width + SXGUI_GAP);
        int width = g_top_button_width;

        /* El ultimo llega hasta el borde de la grilla: el sobrante entero, no
         * un pixel de aire que deje la fila corrida respecto de las teclas. */
        if (index == CALC_TOP_KEY_COUNT - 1)
        {
            width = left + g_grid_width - x;
        }
        g_widgets[index] = sxgui_button(
            sx_rect_make(x, top, width, CALC_KEY_HEIGHT),
            g_top_keys[index].label,
            on_key_press,
            &g_top_keys[index]);
    }
    top += CALC_KEY_HEIGHT + SXGUI_GAP * 2;

    for (row = 0; row < CALC_ROWS; ++row)
    {
        for (column = 0; column < CALC_COLUMNS; ++column)
        {
            int key = row * CALC_COLUMNS + column;

            g_widgets[CALC_TOP_KEY_COUNT + key] = sxgui_button(
                sx_rect_make(
                    left + column * (g_key_width + SXGUI_GAP),
                    top + row * (CALC_KEY_HEIGHT + SXGUI_GAP),
                    g_key_width,
                    CALC_KEY_HEIGHT),
                g_keys[key].label,
                on_key_press,
                &g_keys[key]);
        }
    }
}

static int calc_content_width(void)
{
    return SXGUI_CONTENT_MARGIN * 2 + g_grid_width;
}

static int calc_content_height(void)
{
    return sxgui_menubar_height() + SXGUI_CONTENT_MARGIN
        + CALC_DISPLAY_HEIGHT + SXGUI_GAP * 2
        + CALC_KEY_HEIGHT + SXGUI_GAP * 2
        + CALC_ROWS * CALC_KEY_HEIGHT + (CALC_ROWS - 1) * SXGUI_GAP
        + SXGUI_CONTENT_MARGIN;
}

static void calc_paint_display(struct sx_painter *painter)
{
    struct sx_rect inner = sx_rect_make(
        g_display_rect.x + SXGUI_BORDER_SUNKEN,
        g_display_rect.y + SXGUI_BORDER_SUNKEN,
        g_display_rect.width - SXGUI_BORDER_SUNKEN * 2,
        g_display_rect.height - SXGUI_BORDER_SUNKEN * 2);
    int text_width = sx_painter_text_width(painter, g_display_text);
    int text_y = inner.y + (inner.height - sx_painter_text_height(painter)) / 2;
    int text_x = inner.x + inner.width - SXGUI_TEXT_PAD - text_width;

    sx_painter_fill_rect(painter, g_display_rect, SXGUI_COLOR_FIELD);
    sxgui_draw_sunken_edge(painter, g_display_rect);

    /* Alineado a la derecha, que es por donde crece un numero. Si no entra, el
     * clip le come la cabeza y no la cola: los digitos que importan son los de
     * abajo, no los de arriba. */
    if (text_x < inner.x + SXGUI_TEXT_PAD)
    {
        text_x = inner.x + SXGUI_TEXT_PAD;
    }
    if (!sx_painter_push_clip(painter, inner))
    {
        return;
    }
    sx_painter_draw_text(painter, text_x, text_y, g_display_text, SXGUI_COLOR_TEXT);
    sx_painter_pop_clip(painter);
}

static void calc_paint_indicator(struct sx_painter *painter)
{
    sx_painter_fill_rect(painter, g_indicator_rect, SXGUI_COLOR_FACE);
    sxgui_draw_sunken_edge(painter, g_indicator_rect);
    if (g_memory_used)
    {
        int text_width = sx_painter_text_width(painter, "M");
        int text_x = g_indicator_rect.x + (g_indicator_rect.width - text_width) / 2;
        int text_y = g_indicator_rect.y + (g_indicator_rect.height - sx_painter_text_height(painter)) / 2;

        sx_painter_draw_text(painter, text_x, text_y, "M", SXGUI_COLOR_TEXT);
    }
}

static void on_paint(struct sxgui_app *app)
{
    calc_paint_display(&app->ui.painter);
    calc_paint_indicator(&app->ui.painter);
}

static int on_key(struct sxgui_app *app, const struct savanxp_input_event *event)
{
    int command = CALC_CMD_NONE;

    (void)app;
    if (event->type != SAVANXP_INPUT_EVENT_KEY_DOWN)
    {
        return 0;
    }

    if ((event->modifiers & SAVANXP_KEY_MOD_CTRL) != 0 &&
        (event->modifiers & SAVANXP_KEY_MOD_ALT_GR) == 0)
    {
        /* Los atajos de memoria son los de la calculadora clasica: MS es
         * Ctrl+M, M+ es Ctrl+P, MR es Ctrl+R y MC es Ctrl+L. */
        switch (event->ascii)
        {
        case 'c':
        case 'C':
            command = CALC_CMD_COPY;
            break;
        case 'v':
        case 'V':
            command = CALC_CMD_PASTE;
            break;
        case 'l':
        case 'L':
            command = CALC_CMD_MEMORY_CLEAR;
            break;
        case 'r':
        case 'R':
            command = CALC_CMD_MEMORY_RECALL;
            break;
        case 'm':
        case 'M':
            command = CALC_CMD_MEMORY_STORE;
            break;
        case 'p':
        case 'P':
            command = CALC_CMD_MEMORY_ADD;
            break;
        default:
            break;
        }
    }
    else if (event->key == SAVANXP_KEY_ENTER)
    {
        command = CALC_CMD_EQUALS;
    }
    else if (event->key == SAVANXP_KEY_BACKSPACE)
    {
        command = CALC_CMD_BACKSPACE;
    }
    else if (event->key == SAVANXP_KEY_DELETE)
    {
        command = CALC_CMD_CLEAR_ENTRY;
    }
    else if (event->key == SAVANXP_KEY_F9)
    {
        command = CALC_CMD_SIGN;
    }
    else
    {
        command = calc_command_for_ascii(event->ascii);
    }

    if (command == CALC_CMD_NONE)
    {
        return 0;
    }
    calc_execute(command);
    sxgui_app_request_repaint(&g_app);
    return 1;
}

/* ---- menu y dialogo -------------------------------------------------------- */

#define CALC_MENU_COPY 1
#define CALC_MENU_PASTE 2
#define CALC_MENU_ABOUT 3

#define CALC_ABOUT_WIDTH 320
#define CALC_ABOUT_ROW 18

static struct sxgui_dialog g_about_dialog;
static struct sxgui_widget g_about_widgets[9];

static const char *const k_about_lines[] = {
    "Calculator",
    "Decimal arithmetic with 16 significant digits",
    "",
    "0-9 . + - * /      Enter or =   result",
    "q or @  square root      r  reciprocal",
    "n  sign      c  clear      Del  clear entry",
    "Ctrl+C copy      Ctrl+V paste",
    "Ctrl+M store    Ctrl+P add    Ctrl+R recall"
};

#define CALC_ABOUT_LINES ((int)(sizeof(k_about_lines) / sizeof(k_about_lines[0])))
#define CALC_ABOUT_HEIGHT (SXGUI_DIALOG_MARGIN * 2 + CALC_ABOUT_LINES * CALC_ABOUT_ROW \
    + SXGUI_GAP + SXGUI_BUTTON_HEIGHT)

static void on_about_ok(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    sxgui_dialog_end(&g_app.ui, 1);
}

static void on_menu_command(int id, void *user)
{
    (void)user;
    switch (id)
    {
    case CALC_MENU_COPY:
        calc_execute(CALC_CMD_COPY);
        break;
    case CALC_MENU_PASTE:
        calc_execute(CALC_CMD_PASTE);
        break;
    case CALC_MENU_ABOUT:
        sxgui_dialog_begin(&g_app.ui, &g_about_dialog, CALC_ABOUT_WIDTH, CALC_ABOUT_HEIGHT);
        break;
    default:
        break;
    }
    sxgui_app_request_repaint(&g_app);
}

static const struct sxgui_menu_item k_edit_items[] = {
    {"Copy", CALC_MENU_COPY, 0},
    {"Paste", CALC_MENU_PASTE, 0}
};

static const struct sxgui_menu_item k_help_items[] = {
    {"About Calculator", CALC_MENU_ABOUT, 0}
};

static const struct sxgui_menu k_menus[] = {
    {"Edit", k_edit_items, (int)(sizeof(k_edit_items) / sizeof(k_edit_items[0]))},
    {"Help", k_help_items, (int)(sizeof(k_help_items) / sizeof(k_help_items[0]))}
};

static struct sxgui_menubar g_menubar = {
    k_menus,
    (int)(sizeof(k_menus) / sizeof(k_menus[0])),
    -1,
    -1,
    on_menu_command,
    0
};

static void calc_build_about(void)
{
    int index;

    for (index = 0; index < CALC_ABOUT_LINES; ++index)
    {
        g_about_widgets[index] = sxgui_label(
            sx_rect_make(
                SXGUI_DIALOG_MARGIN,
                SXGUI_DIALOG_MARGIN + index * CALC_ABOUT_ROW,
                CALC_ABOUT_WIDTH - SXGUI_DIALOG_MARGIN * 2,
                CALC_ABOUT_ROW),
            k_about_lines[index]);
    }
    g_about_widgets[CALC_ABOUT_LINES] = sxgui_button(
        sx_rect_make(
            (CALC_ABOUT_WIDTH - SXGUI_BUTTON_WIDTH) / 2,
            CALC_ABOUT_HEIGHT - SXGUI_DIALOG_MARGIN - SXGUI_BUTTON_HEIGHT,
            SXGUI_BUTTON_WIDTH,
            SXGUI_BUTTON_HEIGHT),
        "OK",
        on_about_ok,
        0);
    g_about_dialog.title = "About Calculator";
    g_about_dialog.widgets = g_about_widgets;
    g_about_dialog.widget_count = CALC_ABOUT_LINES + 1;
    g_about_dialog.default_button = CALC_ABOUT_LINES;
}

/* ---- selftest -------------------------------------------------------------- */

static int g_failures;

static void expect_text(const char *actual, const char *expected, const char *what)
{
    if (strcmp(actual, expected) != 0)
    {
        printf("CALC SMOKE FAIL %s: \"%s\" en vez de \"%s\"\n", what, actual, expected);
        g_failures += 1;
    }
}

/* Teclea un guion como lo haria una persona: cada caracter pasa por el mismo
 * mapeo que el teclado real, asi que el selftest ejercita la maquina entera y
 * no una API paralela que solo el usa. */
static void calc_type_script(const char *script)
{
    const char *cursor;

    calc_execute(CALC_CMD_CLEAR);
    for (cursor = script; *cursor != '\0'; ++cursor)
    {
        calc_execute(calc_command_for_ascii(*cursor));
    }
}

static void expect_script(const char *script, const char *expected)
{
    calc_type_script(script);
    expect_text(g_display_text, expected, script);
}

static int calc_selftest(void)
{
    struct calc_number value;

    calc_reset();

    /* Aritmetica basica y lo que distingue a un motor decimal de uno binario:
     * 0.1 + 0.2 tiene que dar 0.3 y no 0.30000000000000004. */
    expect_script("2+3=", "5");
    expect_script("7-9=", "-2");
    expect_script("6*7=", "42");
    expect_script("1/8=", "0.125");
    expect_script("0.1+0.2=", "0.3");
    expect_script("2/3=", "0.6666666666666667");
    /* La deuda del redondeo a 16 digitos, anotada a proposito: si algun dia
     * este check cambia, cambio la precision del motor. */
    expect_script("1/3=*3=", "0.9999999999999999");

    /* El "=" repetido repite la ultima operacion. */
    expect_script("2+3==", "8");
    /* Encadenar operadores cierra la cuenta pendiente antes de seguir. */
    expect_script("2+3*4=", "20");
    /* Cambiar de operador sin teclear nada solo cambia el pendiente. */
    expect_script("5+*3=", "15");

    /* Funciones. */
    expect_script("9q", "3");
    expect_script("2q", "1.414213562373095");
    expect_script("8r", "0.125");
    expect_script("5n", "-5");
    expect_script("200+10%", "20");
    expect_script("200+10%=", "220");
    expect_script("50*10%=", "5");

    /* Errores: quedan clavados hasta que alguien limpie. */
    expect_script("1/0=", "Cannot divide by zero");
    calc_execute(CALC_CMD_DIGIT_0 + 5);
    expect_text(g_display_text, "Cannot divide by zero", "error: ignora teclas");
    calc_execute(CALC_CMD_CLEAR);
    expect_text(g_display_text, "0", "error: C limpia");
    expect_script("4n q", "Invalid input");

    /* Entrada: el tope de digitos, el punto unico y el borrado. */
    expect_script("12345678901234567890", "1234567890123456");
    expect_script("1.2.3", "1.23");
    expect_script("123", "123");
    calc_execute(CALC_CMD_BACKSPACE);
    expect_text(g_display_text, "12", "backspace");
    calc_execute(CALC_CMD_BACKSPACE);
    calc_execute(CALC_CMD_BACKSPACE);
    expect_text(g_display_text, "0", "backspace hasta el cero");

    /* Memoria. */
    calc_type_script("5");
    calc_execute(CALC_CMD_MEMORY_STORE);
    calc_execute(CALC_CMD_DIGIT_0 + 3);
    calc_execute(CALC_CMD_MEMORY_ADD);
    calc_execute(CALC_CMD_MEMORY_RECALL);
    expect_text(g_display_text, "8", "memoria: MS + M+ + MR");
    if (!g_memory_used)
    {
        printf("CALC SMOKE FAIL memoria: el indicador no se encendio\n");
        g_failures += 1;
    }
    calc_execute(CALC_CMD_MEMORY_CLEAR);
    if (g_memory_used)
    {
        printf("CALC SMOKE FAIL memoria: MC no apago el indicador\n");
        g_failures += 1;
    }

    /* Formato: notacion cientifica arriba y abajo del rango legible. */
    expect_script("9999999999999999*9=", "8.999999999999999e+16");
    expect_script("1/8=/1000000=", "1.25e-7");
    expect_script("0.0000001=", "1e-7");
    expect_script("0.00001=", "0.00001");

    /* Texto a numero y de vuelta, que es el camino del portapapeles. */
    value = calc_parse("  -12.500  ");
    calc_format(value, g_display_text, (int)sizeof(g_display_text));
    expect_text(g_display_text, "-12.5", "parse: espacios y ceros de cola");
    value = calc_parse("1,234.5");
    calc_format(value, g_display_text, (int)sizeof(g_display_text));
    expect_text(g_display_text, "1234.5", "parse: ignora lo que no es digito");
    value = calc_parse("no es un numero");
    calc_format(value, g_display_text, (int)sizeof(g_display_text));
    expect_text(g_display_text, "0", "parse: texto sin digitos");

    if (g_failures != 0)
    {
        printf("CALC SMOKE FAIL %d checks\n", g_failures);
        return 1;
    }
    printf("CALC SMOKE PASS digits=%d keys=%d\n", CALC_DIGITS, CALC_WIDGET_COUNT);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && argv != 0 && argv[1] != 0 && strcmp(argv[1], "--selftest") == 0)
    {
        return calc_selftest();
    }

    calc_reset();
    calc_layout();
    calc_build_about();

    if (sxgui_app_init(&g_app, "calc", g_widgets, CALC_WIDGET_COUNT) < 0)
    {
        return 1;
    }
    (void)sxgui_app_set_content_size(&g_app, calc_content_width(), calc_content_height());
    sxgui_set_menubar(&g_app.ui, &g_menubar);
    g_app.on_key = on_key;
    g_app.on_paint = on_paint;
    return sxgui_app_run(&g_app);
}
