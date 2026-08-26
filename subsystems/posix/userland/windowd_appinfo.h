#pragma once

#include "libc.h"
#include "desktop_icons.h"

/*
 * Presentacion de ventanas: titulo, icono y color de barra de titulo.
 *
 * DESDE LA FASE 4 DE SXE (docs/SXE_FORMAT.md) el WM ya no adivina: lee el
 * .sxmeta/.sxicon del binario que lanzo, una sola vez, al crear la ventana.
 * La tabla por path de abajo quedo como FALLBACK para lo que todavia no trae
 * recursos -- Doom, que se construye aparte -- y para los binarios que nunca
 * los van a traer.
 *
 * Esto no contradice la decision de A2.3a ("el que pide declara los flags de
 * lanzamiento: el WM no conoce ningun catalogo de apps"): lo que se prohibio
 * ahi fue que el WM tenga un CATALOGO, una tabla de apps conocidas que hay que
 * mantener a mano. Leer la autodescripcion del binario que le acaban de pedir
 * lanzar es lo contrario -- no hay tabla, no hay conocimiento previo, y un
 * programa que el WM nunca vio se presenta solo.
 *
 * Precedencia de cada campo: .sxe del binario > tabla por path > generico.
 */

struct windowd_appinfo
{
    const char *label;
    const char *path;
    enum desktop_icon_id icon_id;
    uint32_t accent;
};

/* Devuelve 0 si el path no esta en la tabla: el llamador usa defaults. */
const struct windowd_appinfo *windowd_appinfo_for_path(const char *path);

#define WINDOWD_PRESENTATION_LABEL_CAPACITY 32
/* El chrome de ventana y el Task List dibujan el icono chico. */
#define WINDOWD_PRESENTATION_ICON_EXTENT 16u
#define WINDOWD_PRESENTATION_ICON_PIXELS \
    (WINDOWD_PRESENTATION_ICON_EXTENT * WINDOWD_PRESENTATION_ICON_EXTENT)

/*
 * Presentacion ya resuelta de una ventana. Vive adentro de windowd_client y se
 * llena UNA vez, al crear la ventana: el costo esta acotado por la cantidad de
 * ventanas abiertas, no por el tamano de un directorio.
 *
 * Los pixeles se guardan por VALOR y no como puntero al blob: el buffer de
 * lectura es scratch compartido y se pisa con el siguiente cliente. Tampoco se
 * guarda un puntero a este mismo array, para que la struct siga siendo
 * trivialmente copiable.
 *
 * Cuesta ~1 KiB por cliente (14 posibles: 12 overlays + shell + fondo).
 */
struct windowd_presentation
{
    /* Vacio = ni el binario ni la tabla dieron nombre. */
    char label[WINDOWD_PRESENTATION_LABEL_CAPACITY];
    uint32_t accent;
    /* 0 = el binario no trajo icono; se usa fallback_icon_id. */
    uint32_t icon_extent;
    uint32_t fallback_icon_id; /* enum desktop_icon_id */
    uint32_t icon_pixels[WINDOWD_PRESENTATION_ICON_PIXELS];
};

/*
 * Resuelve la presentacion de `path` leyendo su .sxe y cayendo a la tabla.
 * Nunca falla: un binario que no abre, no tiene recursos o trae un blob
 * invalido simplemente se queda con el fallback. La creacion de la ventana no
 * depende de esto.
 */
void windowd_presentation_load(struct windowd_presentation *presentation, const char *path);

/* Titulo resuelto. `path` es el ultimo recurso cuando nadie dio un nombre. */
const char *windowd_presentation_label(const struct windowd_presentation *presentation, const char *path);

/* Color de la barra de titulo con la ventana activa. */
uint32_t windowd_presentation_accent(const struct windowd_presentation *presentation);

/*
 * Icono chico. `storage` lo aporta el llamador para describir los pixeles
 * propios sin que la struct tenga que guardar un puntero a si misma. Nunca
 * devuelve 0: sin icono propio cae al set horneado.
 */
const struct desktop_embedded_bitmap *windowd_presentation_icon(
    const struct windowd_presentation *presentation,
    struct desktop_embedded_bitmap *storage);

/* Valida el fallback y la lectura de recursos reales. 0 si todo pasa. */
int windowd_presentation_selftest(void);
