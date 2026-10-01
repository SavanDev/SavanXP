/* Par de librerias para probar la cadena de DT_NEEDED.
 *
 * Es un fixture, no codigo de produccion: libmath no depende de nada, asi que
 * sin esto el recorrido de DT_NEEDED no tendria nada que recorrer y pasaria sin
 * haber probado nada.
 *
 * Un solo archivo construye las dos. Sin CHAIN_TOP define chain_base; con el
 * define, define chain_top, que llama a chain_base de la otra. La de arriba se
 * enlaza contra la de abajo, y de ahi sale el DT_NEEDED. */

#ifdef CHAIN_TOP

/* Se resuelve en tiempo de carga contra libchainbase.so.0.4. Si la cadena no
 * se recorre, esta llamada queda con un GOT sin rellenar y no vuelve.
 *
 * El #ifdef tiene que envolver tambien la DEFINICION de chain_base. Con la
 * definicion fuera, cada libreria se trae su propia copia, lld no deja el
 * simbolo indefinido, y la dependencia queda declarada pero sin usar: el
 * DT_NEEDED existe y no hace falta para que la prueba pase. */
extern int chain_base(int x);
/* Definido por el EJECUTABLE, no por ninguna libreria. Una cadena de DT_NEEDED
 * sola no lo alcanza: hace falta que el ejecutable este en el ambito de
 * simbolos, que es una capacidad distinta de recorrer dependencias. */
extern int exe_answer(void);

int chain_top(int x) {
    return chain_base(x) * 2 + exe_answer();
}

#else

int chain_base(int x) {
    return x + 1;
}

#endif