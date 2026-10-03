/* Cuatro librerias para el diamante de DT_NEEDED.
 *
 *   libdia_leaf  define dia_leaf
 *   libdia_left  necesita leaf, define dia_left que llama a dia_leaf
 *   libdia_right necesita leaf, define dia_right que llama a dia_leaf
 *   libdia_top   necesita left y right, define dia_top que llama a las dos
 *
 * Cargar top tiene que traer leaf UNA vez, no dos. Sin el cursor `needed_done` el
 * recorrido de DT_NEEDED lo carga por cada rama que lo nombra, y entonces left y
 * right apuntarian a copias distintas de leaf.
 *
 * Por que importa de verdad, y no solo como fjambre: en un grafo real de
 * librerias el caso es comun -- libavcodec y libavformat comparten libavutil, y
 * libswscale con libswresample -- y cargar dos veces la misma significa dos copias
 * mapeadas y dos conjuntos de datos relocalizados que no son el mismo.
 *
 * Todo en un archivo, una libreria por DEFINEs, como el par de la cadena. */
#include "libc.h"

#if defined(DIA_LEAF)
int dia_leaf(int value)
{
    return value + 1;
}

#elif defined(DIA_LEFT)
extern int dia_leaf(int value);

int dia_left(int value)
{
    return dia_leaf(value) * 2;
}

#elif defined(DIA_RIGHT)
extern int dia_leaf(int value);

int dia_right(int value)
{
    return dia_leaf(value) * 3;
}

#elif defined(DIA_TOP)
extern int dia_left(int value);
extern int dia_right(int value);

int dia_top(int value)
{
    return dia_left(value) + dia_right(value);
}
#endif