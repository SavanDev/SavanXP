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
#include <savanxp/ldso.h>

#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- flotante -------------------------------------------------------------- */

/* 15 digitos significativos, que es lo que un double carga de verdad.
 *
 * El motor decimal usaba 16. Bajar a 15 no es arbitrarlo: 2^53 vale
 * 9.007e15, asi que de 16 digitos decimales el ultimo no siempre es cierto --
 * 9999999999999999 no tiene representacion exacta y redondea a 10000000000000000.
 * Mostrar 16 seria presume una precision que el numero no tiene, y se notaria
 * justo en el ultimo digito.
 *
 * Un double tiene 53 bits de mantisa, unos 15.95 decimales. Redondear a 15 es
 * redondear a la precision real del tipo. */
#define CALC_DIGITS 15

/* Exponente decimal por encima del cual el resultado es "Overflow" para el
 * usuario. Un double llega a 1e308, pero un exponente de 300 no entra en una
 * pantalla de calculadora: el tope no es del numero, es de la vista. */
#define CALC_EXPONENT_LIMIT 300

enum calc_status {
    CALC_OK = 0,
    CALC_DIVIDE_BY_ZERO,
    CALC_OVERFLOW,
    CALC_UNDEFINED
};

/* El valor es un double de IEEE-754 y la aritmetica viene de libmath.
 *
 * El motor propio que estaba antes --mantisa de 16 digitos en int64 mas exponente
 * de diez-- nacio cuando la userland in-tree iba -mno-sse y `double` no
 * compilaba. Hoy no queda esa restriccion y math.c es una libreria compartida,
 * asi que el motor propio es codigo que hay que mantener sin agregar nada.
 *
 * Lo que se pierde, y es lo unico que se pierde: en decimal 0.1 + 0.2 da 0.3, y
 * en binario da 0.30000000000000004. La pantalla NO muestra eso, porque
 * calc_format redondea a CALC_DIGITS significativos y por lo tanto muestra 0.3.
 * El numero interno conserva toda la precision que tenga.
 *
 * Ninguna operacion redondea aca. El hardware redondea al mas cercano con
 * medio al par, y el motor decimal redondeaba alejandose del cero. Es una
 * diferencia real, y del lado del hardware: a 0.1 todavia no se le puede
 * guardar el error de representacion. */
typedef double calc_number;

static calc_number calc_zero(void)
{
    return 0.0;
}

static int calc_is_zero(calc_number value)
{
    return value == 0.0;
}

static calc_number calc_negate(calc_number value)
{
    return -value;
}

/* Un resultado que no es un numero finito no es un resultado: es algo que la
 * pantalla tiene que poder nombrar. Dividir por cero ya se trata antes, asi que
 * un infinito solo aparece por overflow, y un NaN por 0 * infinito o por la raiz
 * de un negativo que llego aqui sin pasar por el control de la tecla. */
static int calc_finish(calc_number value, calc_number *out)
{
    if (__builtin_isnan(value))
    {
        return CALC_UNDEFINED;
    }
    if (__builtin_isinf(value))
    {
        return CALC_OVERFLOW;
    }
    *out = value;
    return CALC_OK;
}

static int calc_add(calc_number left, calc_number right, calc_number *out)
{
    return calc_finish(left + right, out);
}

static int calc_subtract(calc_number left, calc_number right, calc_number *out)
{
    return calc_finish(left - right, out);
}

static int calc_multiply(calc_number left, calc_number right, calc_number *out)
{
    return calc_finish(left * right, out);
}

static int calc_divide(calc_number left, calc_number right, calc_number *out)
{
    if (calc_is_zero(right))
    {
        return CALC_DIVIDE_BY_ZERO;
    }
    return calc_finish(left / right, out);
}

/* La raiz viene de libmath.so.0.4, no de codigo propio: es la clase de funcion
 * que tiene sentido compartir en vez de repetir en cada binario. */
static int calc_sqrt(calc_number value, calc_number *out)
{
    if (value < 0.0)
    {
        return CALC_UNDEFINED;
    }
    if (value == 0.0)
    {
        *out = calc_zero();
        return CALC_OK;
    }
    return calc_finish(sqrt(value), out);
}

/* ---- texto ---------------------------------------------------------------- */

#define CALC_TEXT_CAPACITY 48

/*
 * Numero a texto, con la misma convencion que el motor decimal: notacion normal
 * mientras el punto decimal caiga donde se lee de un vistazo, y cientifica
 * cuando no. Un cero de mas o de menos en "0.00000000000001" es un error de
 * lectura garantizado.
 *
 * No se usa %g porque su umbral de notacion cientifica no es el de siempre --
 * pone "1e-5" donde la pantalla de una calculadora pone "0.00001" -- y porque
 * dejaria ver el ruido binario. El redondeo a CALC_DIGITS es lo que salva la
 * diferencia con el motor decimal: 0.1 + 0.2 vale 0.30000000000000004 en
 * binario y aca se muestra 0.3, que es lo que el usuario espera.
 *
 * El cero final y el punto que queda solo se borran al final, despues de la
 * notacion, porque en "9." el punto no esta al final de la cadena.
 */
static void calc_format(calc_number value, char *out, int capacity)
{
    double magnitude;
    char *exponent;
    char *digits;
    int scientific_exponent = 0;
    int decimals;
    char *mantissa_end;

    if (capacity <= 0)
    {
        return;
    }
    if (calc_is_zero(value))
    {
        /* %g de un cero negativo imprime "-0". La pantalla jamas muestra eso. */
        out[0] = '0';
        out[1] = '\0';
        return;
    }
    if (__builtin_isnan(value))
    {
        snprintf(out, (size_t)capacity, "Undefined");
        return;
    }
    if (__builtin_isinf(value))
    {
        snprintf(out, (size_t)capacity, value < 0.0 ? "-Overflow" : "Overflow");
        return;
    }

    /* Potencia de diez del primer digito: decide donde cae el punto, y es el
     * exponente de la notacion cientifica. Se obtiene escalando y no con
     * log10 porque log10 vive en math.c, que ya no esta enlazado aca. */
    magnitude = value < 0.0 ? -value : value;
    if (magnitude != 0.0)
    {
        while (magnitude >= 10.0)
        {
            magnitude /= 10.0;
            scientific_exponent += 1;
        }
        while (magnitude < 1.0)
        {
            magnitude *= 10.0;
            scientific_exponent -= 1;
        }
    }

    if (scientific_exponent < -5 || scientific_exponent >= CALC_DIGITS)
    {
        snprintf(out, (size_t)capacity, "%.*e", CALC_DIGITS - 1, value);
    }
    else
    {
        decimals = CALC_DIGITS - 1 - scientific_exponent;
        if (decimals < 0)
        {
            decimals = 0;
        }
        snprintf(out, (size_t)capacity, "%.*f", decimals, value);
    }

    /* Ceros de relleno y punto solo: en notacion normal "1.500" debe ser "1.5", y
     * en cientifica "9.000e+16" debe ser "9e+16".
     *
     * Lo que se limpia es la MANTISA, o sea lo que va antes de la 'e'. Los
     * digitos del exponente quedan intactos, y por eso el corte se hace en la
     * 'e' y no en el final de la cadena: en "1.25000000000000e-7" el ultimo
     * caracter es un 7 del exponente, y barrenar desde ahi no borra nada.
     *
     * Y al terminar la mantisa hay que MOVER el exponente, no dejar un NUL en
     * su lugar: un NUL encima de la 'e' se come "e-07" y 1e-7 se muestra 1. */
    exponent = strchr(out, 'e');
    mantissa_end = exponent != 0 ? exponent : out + strlen(out);
    while (mantissa_end > out && mantissa_end[-1] == '0')
    {
        mantissa_end -= 1;
    }
    if (mantissa_end > out && mantissa_end[-1] == '.')
    {
        mantissa_end -= 1;
    }
    if (exponent != 0)
    {
        memmove(mantissa_end, exponent, strlen(exponent) + 1);
        exponent = mantissa_end;
    }
    else
    {
        *mantissa_end = '\0';
    }

    /* El exponente se reescribe sin el cero de la izquierda: %e lo emite con
     * dos digitos siempre, y "1.25e-07" tiene un cero de mas. El motor decimal
     * escribia el exponente como un entero, o sea "1.25e-7". */
    exponent = strchr(out, 'e');
    if (exponent != 0)
    {
        digits = exponent + 1;
        if (*digits == '-' || *digits == '+')
        {
            digits += 1;
        }
        if (digits[0] == '0' && digits[1] >= '0' && digits[1] <= '9')
        {
            memmove(digits, digits + 1, strlen(digits));
        }
    }
}

/*
 * Texto a numero. Acepta lo que escribe el teclado y lo que llega del
 * portapapeles, que es texto de cualquiera: lo que no sea un digito, un punto
 * o el signo de adelante se ignora, en vez de rechazar la entrada entera.
 *
 * Por eso NO alcanza con strtod: el se detiene en el primer caracter que no
 * pertenece al numero, y "1,234.5" -- que es como se pega un numero con
 * separador de miles -- se leeria como 1. Se filtra el texto primero y se
 * entrega a strtod lo que si es numero.
 */
static calc_number calc_parse(const char *text)
{
    char filtered[CALC_TEXT_CAPACITY];
    size_t kept = 0;
    int after_point = 0;
    const char *cursor = text;

    if (cursor == 0)
    {
        return calc_zero();
    }
    while (*cursor == ' ')
    {
        cursor += 1;
    }
    /* El signo se copia en vez de consumirse: si se descarta, "4n" -- que
     * presiona el signo menos sobre la entrada -- daria 4 en vez de -4, y la
     * raiz de un negativo responderia 2 en vez de "Invalid input". */
    if (*cursor == '-' || *cursor == '+')
    {
        filtered[kept++] = *cursor;
        cursor += 1;
    }
    for (; *cursor != '\0' && kept + 1 < sizeof(filtered); ++cursor)
    {
        if (*cursor == '.')
        {
            /* Un solo punto: "1.2.3" es 1.23, no 1.2 con un 3 colgando. */
            if (!after_point)
            {
                after_point = 1;
                filtered[kept++] = '.';
            }
            continue;
        }
        if (*cursor < '0' || *cursor > '9')
        {
            continue;
        }
        filtered[kept++] = *cursor;
    }
    filtered[kept] = '\0';
    if (kept == 0)
    {
        return calc_zero();
    }
    return strtod(filtered, 0);
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
static calc_number g_display;
static calc_number g_accumulator;
static int g_pending;
/* Operador y operando del ultimo "=", para que volver a apretarlo repita la
 * operacion, como en la calculadora de siempre. */
static calc_number g_repeat_operand;
static int g_repeat_operator;
static calc_number g_memory;
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

static calc_number calc_current(void)
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

static int calc_apply(int operation, calc_number left, calc_number right, calc_number *out)
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
    calc_number value = calc_current();

    if (g_pending != CALC_OP_NONE && g_entering)
    {
        calc_number result;
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
    calc_number value = calc_current();
    calc_number result;
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
    calc_number value = calc_current();
    calc_number hundred = 100.0;
    calc_number result;
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
    calc_number value = calc_current();
    calc_number result;
    int status;

    if (command == CALC_CMD_SQRT)
    {
        status = calc_sqrt(value, &result);
    }
    else
    {
        status = calc_divide(1.0, value, &result);
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
    calc_number value = calc_current();
    calc_number result;

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

/*
 * Que SxGUI venga de la libreria y no de una copia en el binario.
 *
 * El selftest es aritmetico y nunca entra al camino de la interfaz, asi que por
 * si solo no probaria NADA de libsxgui.so.0.4. Lo que si se puede verificar sin
 * levantar un gestor de ventanas es que la libreria esta cargada, que sus
 * simbolos se resuelven por el ambito del cargador, y que la entrada que el
 * ejecutable tiene en su GOT apunta a la MISMA direccion que el resolvedor
 * reporta. Si las dos cosas divergieran, el GOT estaria en otro lado y la
 * aplicacion dibujaria con codigo distinto del que el cargador cree.
 *
 * Es el mismo truco que usa libtest con sqrt: la llamada sola no prueba nada,
 * la coincidencia de direcciones si.
 */
int calc_check_shared_sxgui(void);

int calc_check_shared_sxgui(void)
{
    void* reported = ldso_lookup("sxgui_app_init");
    if (reported == 0)
    {
        eprintf("calc: sxgui_app_init no esta en ninguna imagen cargada\n");
        return 0;
    }
    if ((const void*)reported != (const void*)sxgui_app_init)
    {
        eprintf("calc: el resolvedor y el GOT apuntan a distinto lado de SxGUI\n");
        return 0;
    }
    /* Y tiene que venir de la libreria. Comparar direcciones no alcanza: si el
     * ejecutable trajera su propia copia de SxGUI, las dos coincidirian igual y
     * la comprobacion pasaria sin que haya una libreria en juego. */
    if (!ldso_symbol_is_shared("sxgui_app_init"))
    {
        eprintf("calc: sxgui_app_init vino del ejecutable, no de libsxgui.so.0.4\n");
        return 0;
    }
    /* El control negativo: algo que solo existe en el ejecutable tiene que
     * seguir saliendo del ejecutable. Si esto pasara, la pregunta de arriba no
     * distinguiria nada. */
    if (ldso_symbol_is_shared("calc_check_shared_sxgui"))
    {
        eprintf("calc: una funcion del ejecutable aparece como de una libreria\n");
        return 0;
    }
    return 1;
}

static int calc_selftest(void)
{
    calc_number value;

    calc_reset();

    /* Aritmetica basica, y el resultado que un humano espera: 0.1 + 0.2 da
     * 0.30000000000000004 en binario, y la pantalla tiene que decir 0.3. Que lo
     * diga depende de que calc_format redondee a CALC_DIGITS; si algun dia
     * muestra los 17 digitos, este check es el que avisa.
     *
     * Este bloque cambio cuando el motor propio de 16 digitos decimales se
     * cambio por libmath. Lo que se conserva es lo que el usuario ve. */
    expect_script("2+3=", "5");
    expect_script("7-9=", "-2");
    expect_script("6*7=", "42");
    expect_script("1/8=", "0.125");
    expect_script("0.1+0.2=", "0.3");
    /* 15 significativos y no 16: 2^53 vale 9.007e15, asi que el dieciseisimo
     * digito decimal no siempre es cierto. Con 15, 2/3 se muestra con 15
     * decimales; el motor decimal mostraba el mismo numero con 16. */
    expect_script("2/3=", "0.666666666666667");
    /* Esta deuda SE MEJORO. El motor decimal redondeaba alejandose del cero y
     * 1/3 * 3 daba 0.9999999999999999; el hardware multiplica el double mas
     * cercano a 1/3 por 3 y da exactamente 1, que es la respuesta correcta.
     * La diferencia real que queda es la representacion de 0.1, que ninguna
     * base binaria resuelve. */
    expect_script("1/3=*3=", "1");

    /* El "=" repetido repite la ultima operacion. */
    expect_script("2+3==", "8");
    /* Encadenar operadores cierra la cuenta pendiente antes de seguir. */
    expect_script("2+3*4=", "20");
    /* Cambiar de operador sin teclear nada solo cambia el pendiente. */
    expect_script("5+*3=", "15");

    /* Funciones. La raiz viene de libmath.so.0.4 y no de codigo propio. */
    expect_script("9q", "3");
    /* Raiz de 2 a 15 significativos. El motor decimal mostraba 16 y daba el
     * mismo numero redondeado a un digito mas. */
    expect_script("2q", "1.4142135623731");
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

    if (!calc_check_shared_sxgui()) {
        return 1;
    }

    /* Entrada: el tope de digitos, el punto unico y el borrado.
     *
     * El tope bajo de 16 a 15 porque la entrada y la pantalla comparten
     * CALC_DIGITS, y una double no carga 16 digitos decimales exactos. Teclear
     * el dieciseisimo digito ya no alcanza. */
    expect_script("12345678901234567890", "123456789012345");
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

    /* Formato: notacion cientifica arriba y abajo del rango legible.
     *
     * Este caso baja a 15 digitos de entrada, asi que ya no es el mismo numero:
     * el motor decimal multiplicaba 9999999999999999 -- que en binario no tiene
     * representacion exacta, redondea a 1e16 -- por 9 y daba 8.999999999999999e16.
     * Ahora se teclea 999999999999999 y el producto es 8.99999999999999e15.
     *
     * La perdida de precision que introduce el motor binario es real y no se
     * disimula: es la de cualquier calculadora de escritorio, que tambien usa
     * IEEE-754. La que no existe aca es la contraria: con decimal, escribir
     * 9999999999999999 y multiplicar por 9 daba un producto exacto de 17
     * digitos. */
    expect_script("9999999999999999*9=", "8.99999999999999e+15");
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
