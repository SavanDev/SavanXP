/* Cuánto tarda el cargador con una libreria de verdad.
 *
 * Es la primera vez que se mide el cargador con algo que no fue hecho a medida. Las
 * seis librerias del sistema pesan entre 2 y 136 KB; libffmpeg.so.0.4 pesa 6.8 MB,
 * exporta 2334 simbolos y trae 9228 reubicaciones.
 *
 * La pregunta de la que nacio esto era si la busqueda de simbolos --un recorrido LINEAL
 * de la .dynsym con strcmp, una vez por reubicacion que necesita nombre, o sea 2029
 * veces contra 2334 simbolos-- era un problema. MEDIDA: NO. La carga va justa de
 * memoria, no de CPU en la busqueda, asi que el recorrido lineal se queda.
 *
 * Lo que sale de aqui: el reparto del tiempo. Colocar los segmentos se lleva ~1015 ms
 * (leer 6.8 MB y reservar 17.25 MB de BSS) y las 9228 reubicaciones ~1226 ms, que
 * son fallos de pagina de primer toque sobre 18 MB, no calculo. Abrir y mapear son
 * 3 ms. Los numeros varian entre ~2.2 s en caliente y ~3.6 s en frio.
 *
 * El programa declara DEPENDS libmath a proposito, y no para usar matematicas: de las
 * 112 referencias externas de libffmpeg, 70 las resuelve el runtime de C --que vive en
 * el ejecutable y se exporta con --export-dynamic-- y las otras 42 son exactamente
 * las funciones de doble precision, que libmath.so.0.4 ya tiene todas. Sin ese
 * DEPENDS la carga falla por simbolos, y se estaria midiendo el motivo equivocado.
 *
 * Mide el tiempo de CARGA COMPLETA: cabeceras, mapeo de segmentos, tabla dinamica y
 * TODAS las reubicaciones. Eso ultimo es donde esta el coste que se quiere conocer. */
#include "libc.h"
#include "ldso.h"

/* El runtime da milisegundos de reloj de pared, no de proceso: sirve para comparar
 * magnitudes, que es lo que se necesita. */
unsigned long uptime_ms(void);

/* Abrir el archivo y mapearlo, sin tocar ni un byte.
 *
 * NO es una medida de la lectura. map_view_at es perezoso a proposito, asi que aqui
 * solo se paga abrir y crear el mapa: unos 3 ms para 6.8 MB, que dice que el coste
 * de la carga no esta en abrir el archivo sino en lo que viene despues --copiar los
 * segmentos y reubicar, que son los unicos que tocan las paginas. */
static unsigned long time_map_only(void)
{
    const long handle = savanxp_open("/disk/lib/libffmpeg.so.0.4");
    if (handle < 0) {
        printf("ffmpegload: no se pudo abrir el archivo\n");
        return 0;
    }
    const long section = section_open((int)handle, SAVANXP_SECTION_READ);
    (void)savanxp_close((int)handle);
    if (section < 0) {
        printf("ffmpegload: no se pudo crear la seccion del archivo\n");
        return 0;
    }
    const unsigned long started = uptime_ms();
    const void* bytes = map_view_at((int)section, 0, SAVANXP_SECTION_READ);
    const unsigned long elapsed = uptime_ms() - started;
    if (bytes == 0) {
        printf("ffmpegload: no se pudo mapear el archivo\n");
    }
    return elapsed;
}

int main(void)
{
    const int before = ldso_count();
    const unsigned long map_ms = time_map_only();
    const unsigned long started = uptime_ms();

    const int why = ldso_load("/disk/lib/libffmpeg.so.0.4");
    const unsigned long elapsed = uptime_ms() - started;

    if (why != 0) {
        printf("ffmpegload: la carga fallo en el paso %d, tras %lu ms\n", -why, elapsed);
        const char* missing = ldso_missing();
        if (missing != 0) {
            printf("ffmpegload: el cargador nombra '%s'\n", missing);
        }
        return 1;
    }

    /* Un simbolo que libffmpeg DEFINE y que nadie mas define. Si esto no resuelve, la
     * carga fue parcial y el numero de milisegundos no mide nada. */
    if (ldso_lookup("avformat_open_input") == 0) {
        printf("ffmpegload: cargo pero avformat_open_input no esta\n");
        return 1;
    }

    printf("ffmpegload: %d imagenes antes, %d despues, %lu ms\n",
           before, ldso_count(), elapsed);
    printf("ffmpegload: abrir y mapear sin leer: %lu ms\n", map_ms);
    printf("FFMPEGLOAD OK\n");
    return 0;
}