#include "libc.h"
#include "savanxp/sxgui.h"

/*
 * Seleccion de texto y portapapeles en sxgui, headless.
 *
 * sxgui_context_init toma un buffer de pixeles plano, asi que el toolkit se
 * puede manejar entero sin ventana ni compositor: se le inyectan eventos de
 * teclado y de puntero y se miran caret, sel_anchor y el buffer. Es
 * determinista y corre en milisegundos, que es lo que hace falta para cubrir
 * una interaccion que si no habria que probar a mano cada vez.
 */

#define TEST_WIDTH 320
#define TEST_HEIGHT 200

static uint32_t g_pixels[TEST_WIDTH * TEST_HEIGHT];
static struct savanxp_fb_info g_info;
static struct sxgui_context g_ctx;
static struct sxgui_widget g_widgets[1];
static char g_buffer[512];

static int g_failures = 0;

static void fail(const char *label)
{
    eprintf("seltest: FAIL %s\n", label);
    g_failures += 1;
}

static void expect_text(const char *expected, const char *label)
{
    if (strcmp(g_buffer, expected) != 0)
    {
        eprintf("seltest: FAIL %s: buffer '%s', esperaba '%s'\n", label, g_buffer, expected);
        g_failures += 1;
    }
}

static void expect_sel(int anchor, int caret, const char *label)
{
    if (g_widgets[0].sel_anchor != anchor || g_widgets[0].caret != caret)
    {
        eprintf("seltest: FAIL %s: anchor=%d caret=%d, esperaba anchor=%d caret=%d\n",
                label, g_widgets[0].sel_anchor, g_widgets[0].caret, anchor, caret);
        g_failures += 1;
    }
}

static void expect_clipboard(const char *expected, const char *label)
{
    char actual[128];
    int result = clipboard_get_text(actual, (int)sizeof(actual));
    if (result < 0)
    {
        eprintf("seltest: FAIL %s: no se pudo leer el portapapeles\n", label);
        g_failures += 1;
        return;
    }
    if (strcmp(actual, expected) != 0)
    {
        eprintf("seltest: FAIL %s: portapapeles '%s', esperaba '%s'\n", label, actual, expected);
        g_failures += 1;
    }
}

static void key(uint32_t code, int ascii, uint32_t modifiers)
{
    struct savanxp_input_event event;
    event.type = SAVANXP_INPUT_EVENT_KEY_DOWN;
    event.key = code;
    event.ascii = ascii;
    event.modifiers = modifiers;
    (void)sxgui_handle_key(&g_ctx, &event);
}

static void shift_right(int times)
{
    int index;
    for (index = 0; index < times; ++index)
    {
        key(SAVANXP_KEY_RIGHT, 0, SAVANXP_KEY_MOD_SHIFT);
    }
}

static void pointer(int x, int y, uint32_t buttons)
{
    struct savanxp_gui_pointer_event event;
    event.x = x;
    event.y = y;
    event.buttons = buttons;
    (void)sxgui_handle_pointer(&g_ctx, &event);
}

static void reset(const char *initial)
{
    /* strcpy y no strncpy: esta libc no trae strncpy, y los textos del test son
     * literales cortos contra un buffer de 512. */
    strcpy(g_buffer, initial);
    g_widgets[0] = sxgui_textedit(sx_rect_make(4, 4, 300, 120), g_buffer, (int)sizeof(g_buffer));
    sxgui_context_init(&g_ctx, g_pixels, &g_info, g_widgets, 1);
    sxgui_focus(&g_ctx, 0);
    g_widgets[0].caret = 0;
    g_widgets[0].sel_anchor = -1;
}

int main(void)
{
    g_info.width = TEST_WIDTH;
    g_info.height = TEST_HEIGHT;
    g_info.pitch = TEST_WIDTH * 4;
    g_info.bpp = 32;
    g_info.buffer_size = sizeof(g_pixels);

    /* Shift + flecha extiende; la flecha pelada suelta el ancla. */
    reset("hola mundo");
    shift_right(4);
    expect_sel(0, 4, "shift+derecha extiende");
    key(SAVANXP_KEY_RIGHT, 0, 0);
    if (g_widgets[0].sel_anchor >= 0)
    {
        fail("la flecha sin shift tendria que soltar el ancla");
    }

    /* Copiar lo seleccionado, sin tocar el documento. */
    reset("hola mundo");
    shift_right(4);
    key((uint32_t)'c', 'c', SAVANXP_KEY_MOD_CTRL);
    expect_clipboard("hola", "ctrl+c copia la seleccion");
    expect_text("hola mundo", "ctrl+c no toca el buffer");

    /* Cortar copia y saca. */
    reset("hola mundo");
    shift_right(5);
    key((uint32_t)'x', 'x', SAVANXP_KEY_MOD_CTRL);
    expect_clipboard("hola ", "ctrl+x copia");
    expect_text("mundo", "ctrl+x saca lo seleccionado");
    expect_sel(-1, 0, "ctrl+x deja el caret donde empezaba la seleccion");

    /* Pegar en el caret. */
    key(SAVANXP_KEY_END, 0, 0);
    key((uint32_t)'v', 'v', SAVANXP_KEY_MOD_CTRL);
    expect_text("mundohola ", "ctrl+v pega en el caret");

    /* Pegar sobre una seleccion la reemplaza. */
    reset("AAABBB");
    (void)clipboard_set_text("-");
    shift_right(3);
    key((uint32_t)'v', 'v', SAVANXP_KEY_MOD_CTRL);
    expect_text("-BBB", "pegar reemplaza la seleccion");

    /* Escribir sobre una seleccion la reemplaza. */
    reset("AAABBB");
    shift_right(3);
    key((uint32_t)'z', 'z', 0);
    expect_text("zBBB", "escribir reemplaza la seleccion");

    /* Backspace con seleccion borra la seleccion entera, no un caracter. */
    reset("AAABBB");
    shift_right(3);
    key(SAVANXP_KEY_BACKSPACE, 0, 0);
    expect_text("BBB", "backspace borra la seleccion entera");

    /* Ctrl+A selecciona todo. */
    reset("hola");
    key((uint32_t)'a', 'a', SAVANXP_KEY_MOD_CTRL);
    expect_sel(0, 4, "ctrl+a selecciona todo");

    /* Un Ctrl+letra que no manejamos se consume igual: sin eso escribiria. */
    reset("");
    key((uint32_t)'b', 'b', SAVANXP_KEY_MOD_CTRL);
    expect_text("", "ctrl+b no escribe una b");

    /* AltGr llega como Ctrl+Alt en varias distribuciones; ahi la tecla SI se
     * escribe, porque el usuario quiso el caracter y no un atajo. */
    reset("");
    key((uint32_t)'v', 'v', SAVANXP_KEY_MOD_CTRL | SAVANXP_KEY_MOD_ALT_GR);
    expect_text("v", "ctrl+altgr escribe el caracter");

    /* Click ancla, arrastre extiende, y los dos calculan el caret igual: al
     * soltar el boton no se tiene que mover nada. */
    reset("hola mundo");
    pointer(10, 10, SAVANXP_MOUSE_BUTTON_LEFT);
    {
        int anchored = g_widgets[0].sel_anchor;
        int caret_before;

        if (anchored < 0)
        {
            fail("el click tendria que anclar");
        }
        pointer(140, 10, SAVANXP_MOUSE_BUTTON_LEFT);
        if (g_widgets[0].caret <= anchored)
        {
            fail("arrastrar a la derecha tendria que extender la seleccion");
        }
        if (g_widgets[0].sel_anchor != anchored)
        {
            fail("arrastrar movio el ancla");
        }
        caret_before = g_widgets[0].caret;
        pointer(140, 10, 0);
        if (g_widgets[0].caret != caret_before)
        {
            fail("soltar el boton movio el caret");
        }
    }

    /* Pintar con seleccion viva: ejercita el recorte del tramo resaltado, que
     * hace aritmetica de indices contra un buffer de linea de tamano fijo. */
    reset("una linea larga para pintar con la seleccion puesta");
    key((uint32_t)'a', 'a', SAVANXP_KEY_MOD_CTRL);
    sxgui_paint(&g_ctx);

    if (g_failures != 0)
    {
        eprintf("seltest: %d fallo(s)\n", g_failures);
        return 1;
    }
    puts("seltest: PASS\n");
    return 0;
}
