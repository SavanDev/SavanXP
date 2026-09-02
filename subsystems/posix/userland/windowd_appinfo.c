#include "libc.h"
#include "windowd_appinfo.h"

/* Sin <stdio.h>: windowd linkea libc.c y no posix.c, asi que printf tiene que
 * ser el de libc.h. Incluir stdio traeria el #define a sx_printf. */
#include "savanxp/sxe.h"

#define WINDOWD_RGB_LITERAL(red, green, blue) (((uint32_t)(red) << 16) | ((uint32_t)(green) << 8) | (uint32_t)(blue))

/* Las apps de diagnostico se compilan solo si el build las pide (build.ps1
 * -NoTestApps las excluye del rootfs y de esta tabla a la vez). */
#ifndef DESKTOP_INCLUDE_TEST_APPS
#define DESKTOP_INCLUDE_TEST_APPS 1
#endif

/*
 * Presentacion por path: nombre, icono y color de la barra de titulo. Un path
 * ausente no es un error -- la ventana usa defaults genericos.
 *
 * Icono generico en TODAS las filas: ya no hay ids horneados por app (el set
 * quedo reducido a DESKTOP_ICON_DESKTOP, docs/SXE_FORMAT.md). El icono real de
 * cada programa lo lee windowd_presentation_load() de su propio .sxicon antes
 * de llegar aca -- esta tabla es el escalon de abajo, solo entra en juego si
 * esa lectura no trae nada.
 */
static const struct windowd_appinfo k_window_items[] = {
    {"Program Manager", "/bin/progman", DESKTOP_ICON_DESKTOP, WINDOWD_RGB_LITERAL(66, 92, 150)},
    {"Shell", "/bin/shellapp", DESKTOP_ICON_DESKTOP, WINDOWD_RGB_LITERAL(0, 124, 96)},
    {"Files", "/bin/filesapp", DESKTOP_ICON_DESKTOP, WINDOWD_RGB_LITERAL(186, 128, 36)},
    {"About", "/bin/aboutapp", DESKTOP_ICON_DESKTOP, WINDOWD_RGB_LITERAL(58, 104, 190)},
    {"Notepad", "/bin/notepad", DESKTOP_ICON_DESKTOP, WINDOWD_RGB_LITERAL(120, 100, 60)},
    {"Doom", "/disk/bin/doomgeneric", DESKTOP_ICON_DESKTOP, WINDOWD_RGB_LITERAL(181, 81, 55)},
#if DESKTOP_INCLUDE_TEST_APPS
    {"Widgets", "/bin/widgetsdemo", DESKTOP_ICON_DESKTOP, WINDOWD_RGB_LITERAL(96, 110, 140)},
    {"Gfx Demo", "/bin/gfxdemo", DESKTOP_ICON_DESKTOP, WINDOWD_RGB_LITERAL(34, 142, 96)},
    {"Key Test", "/bin/keytest", DESKTOP_ICON_DESKTOP, WINDOWD_RGB_LITERAL(41, 111, 188)},
    {"Mouse Test", "/bin/mousetest", DESKTOP_ICON_DESKTOP, WINDOWD_RGB_LITERAL(156, 104, 38)},
#endif
};

const struct windowd_appinfo *windowd_appinfo_for_path(const char *path)
{
    int index;
    const int count = (int)(sizeof(k_window_items) / sizeof(k_window_items[0]));

    if (path == 0)
    {
        return 0;
    }

    for (index = 0; index < count; ++index)
    {
        if (strcmp(k_window_items[index].path, path) == 0)
        {
            return &k_window_items[index];
        }
    }
    return 0;
}

/* --- presentacion resuelta ------------------------------------------------ */

/* Azul de barra de titulo del sistema, para lo que no declara accent propio. */
#define WINDOWD_DEFAULT_ACCENT WINDOWD_RGB_LITERAL(59, 95, 156)

static void copy_label(char *destination, size_t capacity, const char *source)
{
    size_t index = 0;

    if (destination == 0 || capacity == 0)
    {
        return;
    }
    if (source != 0)
    {
        while (source[index] != '\0' && index + 1u < capacity)
        {
            destination[index] = source[index];
            index += 1u;
        }
    }
    destination[index] = '\0';
}

/* Copia el icono del blob al almacenamiento propio del cliente. El blob vive
 * en un scratch compartido que se pisa con el proximo cliente. */
static void adopt_icon(struct windowd_presentation *presentation, const struct sxe_icons *icons)
{
    const struct sxe_icon_entry *entry = sxe_icons_best(icons, WINDOWD_PRESENTATION_ICON_EXTENT);
    const uint32_t *pixels = 0;

    /* Solo cuadrados que entren en el slot: el chrome dibuja a 16 y agrandar el
     * slot costaria memoria en TODOS los clientes. */
    if (entry == 0 || entry->width != entry->height || entry->width > WINDOWD_PRESENTATION_ICON_EXTENT)
    {
        return;
    }
    pixels = sxe_icons_pixels(icons, entry);
    if (pixels == 0)
    {
        return;
    }
    memcpy(presentation->icon_pixels, pixels, (size_t)entry->width * entry->height * sizeof(uint32_t));
    presentation->icon_extent = entry->width;
}

void windowd_presentation_load(struct windowd_presentation *presentation, const char *path)
{
    /*
     * Scratch compartido y estatico: 68 KiB una sola vez para todo el WM, en
     * vez de por cliente. El de iconos va al TOPE DEL FORMATO y no al tamano
     * que hoy emiten los manifiestos -- ajustarlo dejaria sin icono a un
     * binario que ademas traiga 48x48, degradado silencioso por un blob valido.
     */
    _Alignas(4) static uint8_t meta_buffer[SXE_META_MAX_BYTES];
    _Alignas(4) static uint8_t icon_buffer[SXE_ICON_MAX_BYTES];
    const struct windowd_appinfo *item = 0;
    struct sxe_meta meta;
    struct sxe_icons icons;
    char text[WINDOWD_PRESENTATION_LABEL_CAPACITY];
    uint32_t value = 0;

    if (presentation == 0)
    {
        return;
    }
    memset(presentation, 0, sizeof(*presentation));
    presentation->fallback_icon_id = DESKTOP_ICON_DESKTOP;
    presentation->accent = WINDOWD_DEFAULT_ACCENT;

    /* Escalon 2: la tabla por path, para lo que no trae recursos. */
    item = windowd_appinfo_for_path(path);
    if (item != 0)
    {
        copy_label(presentation->label, sizeof(presentation->label), item->label);
        presentation->fallback_icon_id = (uint32_t)item->icon_id;
        presentation->accent = item->accent;
    }

    /* Escalon 1: lo que el binario declara de si mismo, que gana. */
    if (sxe_load_meta(path, meta_buffer, sizeof(meta_buffer), &meta) == SXE_OK)
    {
        /* Al temporal y no directo al campo: sxe_meta_string vacia el destino
         * ANTES de buscar el tag, asi que escribir en label borraria lo que
         * dio la tabla cuando el binario no declara nombre. */
        if (sxe_meta_string(&meta, SXE_TAG_NAME, text, sizeof(text)) > 0u)
        {
            copy_label(presentation->label, sizeof(presentation->label), text);
        }
        if (sxe_meta_u32(&meta, SXE_TAG_ACCENT, &value))
        {
            presentation->accent = value;
        }
    }

    if (sxe_load_icons(path, icon_buffer, sizeof(icon_buffer), &icons) == SXE_OK)
    {
        adopt_icon(presentation, &icons);
    }
}

const char *windowd_presentation_label(const struct windowd_presentation *presentation, const char *path)
{
    if (presentation != 0 && presentation->label[0] != '\0')
    {
        return presentation->label;
    }
    /* Ultimo recurso: el path pelado antes que una ventana sin identidad. */
    return (path != 0 && path[0] != '\0') ? path : "App";
}

uint32_t windowd_presentation_accent(const struct windowd_presentation *presentation)
{
    return presentation != 0 ? presentation->accent : WINDOWD_DEFAULT_ACCENT;
}

const struct desktop_embedded_bitmap *windowd_presentation_icon(
    const struct windowd_presentation *presentation,
    struct desktop_embedded_bitmap *storage)
{
    if (presentation == 0)
    {
        return desktop_icon_small(DESKTOP_ICON_DESKTOP);
    }
    if (presentation->icon_extent != 0u && storage != 0)
    {
        storage->width = presentation->icon_extent;
        storage->height = presentation->icon_extent;
        storage->pixels = presentation->icon_pixels;
        return storage;
    }
    return desktop_icon_small((enum desktop_icon_id)presentation->fallback_icon_id);
}

/* --- selftest ------------------------------------------------------------- */

static int g_presentation_failures = 0;

static void expect(int condition, const char *label)
{
    if (!condition)
    {
        printf("WINDOWD SMOKE FAIL %s\n", label);
        g_presentation_failures += 1;
    }
}

int windowd_presentation_selftest(void)
{
    static struct windowd_presentation presentation;
    /* Buffer de una lectura INDEPENDIENTE del mismo .sxicon, por la API
     * publica de sxe.h en vez del camino interno de windowd_presentation_load.
     * Es lo que reemplaza la comparacion contra el set horneado ahora que
     * desktop_icons.h ya no tiene un id por app (docs/SXE_FORMAT.md, "icon= ya
     * no elige de un catalogo"): no queda un SEGUNDO camino independiente que
     * hornee el mismo PNG para cruzar contra el primero. */
    _Alignas(4) uint8_t icon_buffer[SXE_ICON_MAX_BYTES];
    struct sxe_icons icons;
    const struct sxe_icon_entry *entry = 0;
    const uint32_t *raw_pixels = 0;
    struct desktop_embedded_bitmap storage;
    const struct desktop_embedded_bitmap *icon = 0;

    g_presentation_failures = 0;

    /*
     * Binario estampado. El nombre y el accent del manifiesto coinciden a
     * proposito con los de la tabla -- los .sxres reproducen la presentacion
     * que antes vivia hardcodeada --, asi que la prueba de que el camino del
     * .sxe corrio de verdad es el ICONO: la tabla solo puede dar un icon_id,
     * nunca pixeles propios.
     */
    windowd_presentation_load(&presentation, "/bin/notepad");
    expect(strcmp(windowd_presentation_label(&presentation, "/bin/notepad"), "Notepad") == 0,
        "presentacion: nombre de notepad");
    expect(windowd_presentation_accent(&presentation) == WINDOWD_RGB_LITERAL(120, 100, 60),
        "presentacion: accent de notepad");
    expect(presentation.icon_extent == WINDOWD_PRESENTATION_ICON_EXTENT,
        "presentacion: icono propio de notepad");

    icon = windowd_presentation_icon(&presentation, &storage);
    expect(icon == &storage, "presentacion: usa el icono propio");
    expect(icon != 0 && icon->width == 16u && icon->height == 16u, "presentacion: icono de 16x16");

    /*
     * Los pixeles que windowd_presentation_load copio a icon_pixels tienen
     * que ser identicos a los que hay REALMENTE en el .sxicon de notepad,
     * leidos aca de nuevo por un camino aparte (sxe_load_icons directo, no
     * el de windowd_presentation_load). Sin este cruce, un bug en
     * adopt_icon() -- un offset mal, una entrada equivocada elegida por
     * sxe_icons_best -- pasaria sin que ningun otro assert lo note: todos
     * los de arriba solo verifican que HAYA algo en el slot, no que sea lo
     * correcto.
     */
    if (sxe_load_icons("/bin/notepad", icon_buffer, sizeof(icon_buffer), &icons) == SXE_OK)
    {
        entry = sxe_icons_best(&icons, WINDOWD_PRESENTATION_ICON_EXTENT);
        raw_pixels = entry != 0 ? sxe_icons_pixels(&icons, entry) : 0;
    }
    if (icon != 0 && entry != 0 && raw_pixels != 0 && entry->width == 16u && entry->height == 16u)
    {
        int same = 1;
        uint32_t index = 0;

        for (index = 0; index < WINDOWD_PRESENTATION_ICON_PIXELS; ++index)
        {
            if (icon->pixels[index] != raw_pixels[index])
            {
                same = 0;
                break;
            }
        }
        expect(same, "presentacion: pixeles copiados iguales a una lectura independiente del .sxicon");
    }
    else
    {
        expect(0, "presentacion: no se pudo releer el .sxicon para comparar");
    }

    /*
     * init nunca declaro un init.sxres, pero el build lo estampa igual con un
     * .sxmeta minimo (NAME/VERSION/SUBSYSTEM/BUILD_ID). El titulo ya no cae al
     * path porque el nombre sale del binario -- pero ese nombre resulta ser
     * "init", que es EL MISMO texto que el fallback de basename mostraba
     * antes: el default no cambia ninguna ventana existente, solo deja de
     * inferir lo que ahora esta declarado. Icono y accent siguen sin dueno:
     * eso es enriquecimiento, y el estampado automatico no lo inventa.
     */
    windowd_presentation_load(&presentation, "/bin/init");
    expect(strcmp(windowd_presentation_label(&presentation, "/bin/init"), "init") == 0,
        "presentacion: NAME automatico de init");
    expect(presentation.icon_extent == 0u,
        "presentacion: init sin icono (enriquecimiento, no identidad)");
    expect(windowd_presentation_accent(&presentation) == WINDOWD_DEFAULT_ACCENT,
        "presentacion: init con accent default (sin ACCENT declarado)");
    expect(windowd_presentation_icon(&presentation, &storage) == desktop_icon_small(DESKTOP_ICON_DESKTOP),
        "presentacion: init cae al icono horneado");

    /*
     * Escalon de la tabla, con nombre y accent -- lo unico que sigue siendo
     * genuinamente de la tabla. El icono de Doom ya no lo es: se fue del set
     * horneado (vive en sdk/doomgeneric/icon.png, via icon_file=), asi que
     * fallback_icon_id vuelve al generico igual que para cualquier programa
     * sin fila propia. Doom se instala con un build APARTE -- en esta imagen
     * puede estar o no --, asi que su .sxicon real no es un hecho estable
     * para afirmar aca; nombre y accent si lo son, porque salen de la tabla
     * sin importar si el binario esta instalado.
     */
    windowd_presentation_load(&presentation, "/disk/bin/doomgeneric");
    expect(strcmp(windowd_presentation_label(&presentation, "/disk/bin/doomgeneric"), "Doom") == 0,
        "presentacion: nombre de doom");
    expect(windowd_presentation_accent(&presentation) == WINDOWD_RGB_LITERAL(181, 81, 55),
        "presentacion: accent de doom");

    /* Path inexistente y path nulo: no deben romper nada. */
    windowd_presentation_load(&presentation, "/bin/__no_existe__");
    expect(presentation.icon_extent == 0u, "presentacion: path inexistente sin icono");
    expect(strcmp(windowd_presentation_label(&presentation, "/bin/__no_existe__"), "/bin/__no_existe__") == 0,
        "presentacion: path inexistente cae al path");
    windowd_presentation_load(&presentation, 0);
    expect(strcmp(windowd_presentation_label(&presentation, 0), "App") == 0,
        "presentacion: path nulo cae a App");
    expect(windowd_presentation_accent(&presentation) == WINDOWD_DEFAULT_ACCENT,
        "presentacion: path nulo con accent default");

    return g_presentation_failures;
}
