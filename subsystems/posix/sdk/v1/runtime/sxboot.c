/* Lo unico que crt0 puede tocar.
 *
 * crt0 corre antes de que exista una sola pagina de cualquier biblioteca
 * compartida: lo primero que hace el proceso es su propia imagen, y lo demas --el
 * cargador incluido-- se carga DESPUES, desde aca. Por lo tanto cualquier simbolo
 * que crt0 lea o escriba tiene que vivir dentro del ejecutable, o el acceso faulta
 * en el primer paso de la vida del proceso.
 *
 * Hay exactamente tres, y por tres razones distintas:
 *
 *   __stack_chk_guard / __stack_chk_fail  crt0 escribe el canario que el kernel
 *       le dejo en rdx antes de la primera llamada protegida. El valor tiene que
 *       estar en la imagen principal porque ahi lo escribe crt0.
 *
 *   sx_start_dynamic  la funcion que crt0 llama para arrancar el cargador. Si
 *       viviera en una libreria, esa llamada seria una entrada de PLT sin resolver
 *       en una imagen que todavia no esta cargada.
 *
 * Que esten en su propio archivo, y no en libc.c o en posix.c, es para que el
 * criterio sea comprobable y no una opinion: este archivo es lo que crt0 necesita,
 * y por definicion esto es lo que no se puede mover a una libreria. Los dos archivos
 * de donde salieron --libc.c y posix.c-- si se convierten en librerias, y este
 * sigue en el ejecutable.
 *
 * ldtest comprueba el invariante en caliente: que __stack_chk_guard se resuelva
 * contra el ejecutable, que crt0 lo haya sembrado, y que las dos mitades sean
 * distintas para que un smashed stack de ocho bytes no lo alcance. */
#include <stdint.h>

#include "savanxp/libc.h"

/* ---- Canario de pila ---------------------------------------------------- */

/* Lo inicializa crt0 con el valor que el kernel dejo en rdx, antes de la primera
 * llamada. Cero de momento: un binario que se ejecutara sin pasar por crt0 tendria
 * proteccion pero no detector, que es peor que no tenerla porque parece que la hay. */
uintptr_t __stack_chk_guard = 0;

/* No pasa por sx_abort: ese camino es libc y podria tener un frame protegido
 * justamente cuando esto se dispara. Sale con el estado de abort convencional. */
__attribute__((noreturn)) void __stack_chk_fail(void) {
    register long number asm("rax") = SAVANXP_SYS_EXIT;
    register long argument asm("rdi") = 134;
    __asm__ volatile("int $0x80" : : "r"(number), "r"(argument) : "memory");
    for (;;) {
        __asm__ volatile("");
    }
}

/* ---- Arranque dinamico -------------------------------------------------- */

/* El hook del interprete, definido por el programa que enlace el cargador de
 * librerias. Debil: los programas sin dependencias no lo definen y el enlazador lo
 * resuelve en 0, que es lo que se consulta.
 *
 * Que el chequeo este en C y no en ensamblador no es una preferencia: comprobar si
 * un simbolo debil esta definido exige mirar su entrada en el GOT, y en ensamblador
 * habria que hacer movq simbolo(%rip) -- que LEE la memoria de esa direccion. Con el
 * simbolo debil sin definir esa direccion es cero y el proceso muere leyendo la
 * pagina cero. */
extern int sx_run_interpreter(const char* interpreter_path, unsigned long image_base)
    __attribute__((weak));

/* Un write crudo. Ver la convencion de registros en libc.c: rax numero, rdi a,
 * rsi b, rdx c, r10 d. */
static void write_all(long fd, const void* data, unsigned long length)
{
    register unsigned long number asm("rax") = SAVANXP_SYS_WRITE;
    register unsigned long a asm("rdi") = (unsigned long)fd;
    register unsigned long b asm("rsi") = (unsigned long)data;
    register unsigned long c asm("rdx") = length;
    register unsigned long d asm("r10") = 0;
    __asm__ volatile("int $0x80" : "+r"(number) : "r"(a), "r"(b), "r"(c), "r"(d) : "memory");
}

/* Escribe con la syscall de write directamente, sin pasar por printf.
 *
 * Este es el unico mensaje que un programa puede dar si el interprete no arranca, y
 * lo da en el momento exacto en que printf todavia no existe: el cargador acaba de
 * fallar y ninguna biblioteca esta cargada. Depender de libc aca seria una paradoja
 * --el fallo de libc impediria informar del fallo de libc--.
 *
 * El numero de paso son los pasos de ldso_load, todos de una cifra, asi que el
 * formateo es un caracter y no un printf entero. */
static void report_failure(int result)
{
    /* Los pasos de ldso_load son de una o dos cifras, asi que el formateo son dos
     * caracteres y no un printf entero. */
    static const char prefix[] = "sx_start_dynamic: el interprete fallo (paso ";
    static const char digits[] = "0123456789";
    const unsigned step = (unsigned)(-result);
    char tail[6] = { digits[(step / 10) % 10], digits[step % 10], ')', '\n', 0, 0 };
    /* Con una sola cifra no se imprime el cero de las decenas. */
    unsigned tail_length = step >= 10 ? 4 : 3;
    if (step < 10)
    {
        tail[0] = digits[step % 10];
    }

    write_all(2, prefix, sizeof(prefix) - 1);
    write_all(2, tail, tail_length);
}

/* Lo que crt0 llama antes de main.
 *
 * Que este sea el UNICO lugar donde se decide si el interprete corre es lo que lo
 * hace automatico: un programa no necesita acordarse de llamarlo, y no puede
 * olvidarse.
 *
 * Devuelve el codigo de fallo para que crt0 pueda decidir, y lo escribe tambien,
 * porque un programa que no queda operativo no puede decir nada util con printf
 * todavia y el sintoma --punteros a cero, secciones que no abren-- no dice nada de
 * la causa. */
int sx_start_dynamic(const char* interpreter_path, unsigned long image_base)
{
    if (sx_run_interpreter == 0)
    {
        return 0;
    }
    const int result = sx_run_interpreter(interpreter_path, image_base);
    if (result != 0)
    {
        report_failure(result);
    }
    return result;
}