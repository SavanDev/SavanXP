/* Cargador minimo de librerias compartidas.
 *
 * Que hace: coloca los PT_LOAD de un .so donde el ELF diga, recorre su cadena de
 * DT_NEEDED desde /lib, aplica sus reubicaciones, y deja searchable un simbolo
 * por nombre en todas las imagenes cargadas mas el ejecutable.
 *
 * Que NO hace todavia: no hay PT_INTERP -- el kernel mapea la imagen principal y
 * el programa tiene que llamar a ldso_start() a mano --, ni dlopen, ni control
 * de visibilidad mas alla de global, ni nombres versionados. DT_SONAME se lee y
 * se ignora: un DT_NEEDED se busca tal cual bajo /lib.
 *
 * Que se mapeen los segmentos de verdad (texto desde la seccion respaldada por
 * el archivo, datos como copias privadas) es lo que distingue esto de leer el
 * archivo y nada mas: un .so con GOT no corre si sus datos no estan donde el
 * codigo los busca. */
#ifndef SAVANXP_LDSO_H
#define SAVANXP_LDSO_H

/* Prepara el ejecutable: carga lo que declara en DT_NEEDED y rellena su GOT.
 *
 * El kernel mapea la imagen principal pero no la reubica, asi que un programa
 * que linkee contra una libreria tiene que llamar a esto ANTES de usar un
 * simbolo de ella. Sin la llamada, la entrada del GOT sigue vacia y la llamada
 * salta a donde se le ocurra.
 *
 * Devuelve 0 si quedo operativo, o un numero negativo diciendo en que paso
 * fallo, con la misma convencion que ldso_load. Un ejecutable sin .dynsym no
 * tiene nada que reubicar y devuelve 0. */
int ldso_start(void);

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
