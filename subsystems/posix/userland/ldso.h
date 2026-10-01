/* Cargador minimo de librerias compartidas.
 *
 * Alcance de esta version: tomar un unico .so, mapear sus segmentos donde el
 * ELF diga, aplicar las reubicaciones y exportar una forma de buscar un simbolo
 * por nombre. No hay PT_INTERP, ni busqueda de DT_NEEDED en cascada, ni ambito
 * de simbolos del ejecutable, ni dlopen.
 *
 * Que se mapeen los segmentos de verdad (texto desde la seccion respaldada por
 * el archivo, datos como copias privadas) es lo que distingue esto de leer el
 * archivo y nada mas: un .so con GOT no corre si sus datos no estan donde el
 * codigo los busca. */
#ifndef SAVANXP_LDSO_H
#define SAVANXP_LDSO_H

/* Abre, mapea y reubica una libreria. Devuelve 0 si todo fue bien. */
int ldso_load(const char* path);

/* Direccion del simbolo, o 0 si la libreria no lo define. */
void* ldso_lookup(const char* name);

/* Como la libreria se cargo bien, para poder distinguir "no existe" de "fallo". */
int ldso_loaded(void);

/* Diagnostico del ultimo fallo de carga: que segmento y con que errno. */
extern int g_lib_fail_index;
extern long g_lib_fail_errno;
extern unsigned long g_lib_bias;
extern int g_lib_reloc_step;
#endif
