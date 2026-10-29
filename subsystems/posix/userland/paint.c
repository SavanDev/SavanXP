#include "libc.h"
#include "savanxp/img.h"
#include "savanxp/sxgui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "shared/version.h"

/*
 * Paint, laid out like the WinXP-era one: toolbox on the left, colours on
 * top and the canvas in the middle over the status bar.
 *
 * None of that is widgets: sxgui has no toolbar or palette, so the toolbox
 * and the colour strip paint in on_paint and hit-test in on_pointer, like the
 * Mines grid in mines.c. The only widget in the window is the status label.
 * The selected tool reads sunken, like a button left pressed.
 *
 * The document lives in an sx_bitmap the app owns (the WM keeps nothing:
 * without a backing store there is no Expose, so the app owns the original
 * and blits it in on_paint). It takes the size of the image it opened, up to
 * PAINT_DOC_W x PAINT_DOC_H. It saves only 24 bpp BI_RGB BMP (a 54-byte
 * header plus padded rows) and opens BMP or PNG, the latter through
 * libsximg. There is no PNG encoder, so a .png name never receives BMP bytes.
 *
 * Everything is 1 px and opaque: there is no sx_pen, no alpha and no flood
 * fill in sxgfx (docs/desktop/SXGFX_ROADMAP.md).
 */

/* --- document ------------------------------------------------------------- */

/* Size of a new document, and the largest image Paint opens: the window
 * layout is sized for it. */
#define PAINT_DOC_W 512
#define PAINT_DOC_H 384

#define PAINT_TOOL_PENCIL 0
#define PAINT_TOOL_BRUSH 1
#define PAINT_TOOL_ERASER 2
#define PAINT_TOOL_PICKER 3
#define PAINT_TOOL_FILL 4
#define PAINT_TOOL_LINE 5
#define PAINT_TOOL_RECT 6
#define PAINT_TOOL_ELLIPSE 7
#define PAINT_TOOL_COUNT 8
#define PAINT_TOOL_COLS 2

#define PAINT_BRUSH_MIN_RADIUS 1
#define PAINT_BRUSH_MAX_RADIUS 16
#define PAINT_BRUSH_DEFAULT_RADIUS 4

#define PAINT_UNDO_DEPTH 8

#define PAINT_CELL_SIZE 30
#define PAINT_CELL_GAP 2
#define PAINT_TOOL_PAD 4

#define PAINT_SWATCH_COUNT 16
#define PAINT_SWATCH_SIZE 22
#define PAINT_SWATCH_GAP 3
#define PAINT_CURRENT_SIZE 30

#define PAINT_PATH_CAPACITY 256
#define PAINT_STATUS_CAPACITY 160

#define PAINT_OPEN_WIDTH 360
#define PAINT_OPEN_HEIGHT 140
#define PAINT_ABOUT_WIDTH 340
#define PAINT_ABOUT_HEIGHT 160

enum paint_menu_id {
    PAINT_MENU_NEW = 1,
    PAINT_MENU_OPEN,
    PAINT_MENU_SAVE,
    PAINT_MENU_SAVE_AS,
    PAINT_MENU_EXIT,
    PAINT_MENU_UNDO,
    PAINT_MENU_TOOL_PENCIL,
    PAINT_MENU_TOOL_BRUSH,
    PAINT_MENU_TOOL_ERASER,
    PAINT_MENU_TOOL_PICKER,
    PAINT_MENU_TOOL_FILL,
    PAINT_MENU_TOOL_LINE,
    PAINT_MENU_TOOL_RECT,
    PAINT_MENU_TOOL_ELLIPSE,
    PAINT_MENU_ABOUT
};

static const uint32_t k_palette[PAINT_SWATCH_COUNT] = {
    SXGUI_RGB(0x00, 0x00, 0x00),
    SXGUI_RGB(0x80, 0x80, 0x80),
    SXGUI_RGB(0x80, 0x00, 0x00),
    SXGUI_RGB(0xFF, 0x00, 0x00),
    SXGUI_RGB(0x80, 0x40, 0x00),
    SXGUI_RGB(0xFF, 0x80, 0x00),
    SXGUI_RGB(0x80, 0x80, 0x00),
    SXGUI_RGB(0xFF, 0xFF, 0x00),
    SXGUI_RGB(0x00, 0x80, 0x00),
    SXGUI_RGB(0x00, 0xFF, 0x00),
    SXGUI_RGB(0x00, 0x80, 0x80),
    SXGUI_RGB(0x00, 0xFF, 0xFF),
    SXGUI_RGB(0x00, 0x00, 0x80),
    SXGUI_RGB(0x00, 0x00, 0xFF),
    SXGUI_RGB(0x80, 0x00, 0x80),
    SXGUI_RGB(0xFF, 0xFF, 0xFF),
};

static const char *k_tool_names[PAINT_TOOL_COUNT] = {
    "Pencil", "Brush", "Eraser", "Picker", "Fill", "Line", "Rectangle", "Ellipse"
};

static struct sxgui_app g_app;
static struct sxgui_widget g_widgets[1];

static struct sx_bitmap g_doc;
static struct sx_painter g_doc_painter;

/* Rows are packed (sx_bitmap_create sets pitch = width * 4), so a pixel is
 * pixels[y * doc_w() + x]. */
static int doc_w(void)
{
    return (int)g_doc.info.width;
}

static int doc_h(void)
{
    return (int)g_doc.info.height;
}

static size_t doc_bytes(void)
{
    return (size_t)doc_w() * (size_t)doc_h() * 4u;
}

static int g_tool = PAINT_TOOL_PENCIL;
static uint32_t g_color = SXGUI_RGB(0x00, 0x00, 0x00);
static int g_brush_radius = PAINT_BRUSH_DEFAULT_RADIUS;
static int g_dirty = 0;

/* Undo: pila de snapshots del documento entero. Se empuja el estado ANTES de
 * cada trazo; deshacer lo devuelve. 8 niveles son ~6 MB: caben en las arenas
 * del heap sin pedir tamano a medida. */
static uint32_t *g_undo[PAINT_UNDO_DEPTH];
static int g_undo_count = 0;

static char g_path[PAINT_PATH_CAPACITY];
static char g_status[PAINT_STATUS_CAPACITY];
static char g_open_path[PAINT_PATH_CAPACITY];

/* Geometria calculada en paint_layout a partir del tamano de la ventana. */
static struct sx_rect g_tool_panel;
static struct sx_rect g_tool_cells[PAINT_TOOL_COUNT];
static struct sx_rect g_current_rect;
static struct sx_rect g_swatch_rect;
static struct sx_rect g_canvas_outer;
static struct sx_rect g_canvas_inner;

/* Trazo en curso: ancla donde cayo el boton y punto actual para el preview. */
static int g_dragging = 0;
static int g_anchor_x = 0;
static int g_anchor_y = 0;
static int g_current_x = 0;
static int g_current_y = 0;
static int g_last_x = 0;
static int g_last_y = 0;
static uint32_t g_last_buttons = 0;

static struct sxgui_widget g_open_widgets[4];
static struct sxgui_dialog g_open_dialog;
static int g_open_mode = 0; /* 0 = abrir, 1 = guardar como */

static struct sxgui_widget g_about_widgets[4];
static struct sxgui_dialog g_about_dialog;

/* --- utilidades ----------------------------------------------------------- */

static uint32_t read_le16(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t read_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_le16(unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char)(value & 0xFFu);
    p[1] = (unsigned char)((value >> 8) & 0xFFu);
}

static void write_le32(unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char)(value & 0xFFu);
    p[1] = (unsigned char)((value >> 8) & 0xFFu);
    p[2] = (unsigned char)((value >> 16) & 0xFFu);
    p[3] = (unsigned char)((value >> 24) & 0xFFu);
}

static void update_status(const char *note)
{
    const char *name = g_path[0] != '\0' ? g_path : "(untitled)";
    char tool[48];

    if (g_tool == PAINT_TOOL_BRUSH || g_tool == PAINT_TOOL_ERASER)
    {
        snprintf(tool, sizeof(tool), "%s %dpx", k_tool_names[g_tool], g_brush_radius * 2 + 1);
    }
    else
    {
        snprintf(tool, sizeof(tool), "%s", k_tool_names[g_tool]);
    }
    if (note != 0 && note[0] != '\0')
    {
        snprintf(g_status, sizeof(g_status), "%s | %s%s | undo %d", name, note,
                 g_dirty ? " *" : "", g_undo_count);
    }
    else
    {
        snprintf(g_status, sizeof(g_status), "%s | %s%s | undo %d", name, tool,
                 g_dirty ? " *" : "", g_undo_count);
    }
}

static void paint_clear(uint32_t colour)
{
    sx_painter_fill(&g_doc_painter, colour);
}

/* --- BMP 24 bpp BI_RGB ----------------------------------------------------
 *
 * Sobre syscalls crudas y en pocos chunks grandes, no sobre stdio: cada
 * write que crece el archivo hace un commit de metadata completo (~46 s bajo
 * TCG), asi que crecer de a 512 B son ~1150 commits para esta imagen. Con el
 * tamano ya puesto via truncate, los writes de abajo no tocan metadata y van
 * a velocidad de datos (progman.c:copy_file hace lo mismo con su buffer).
 */

#define PAINT_SAVE_CHUNK (64u * 1024u)

static int write_all(int fd, const unsigned char *data, size_t size)
{
    size_t done = 0;

    while (done < size)
    {
        size_t chunk = size - done > PAINT_SAVE_CHUNK ? PAINT_SAVE_CHUNK : size - done;
        long result = savanxp_write(fd, data + done, chunk);

        if (result <= 0)
        {
            return -1;
        }
        done += (size_t)result;
    }
    return 0;
}

static int read_all(int fd, unsigned char *data, size_t size)
{
    size_t done = 0;

    while (done < size)
    {
        long result = savanxp_read(fd, data + done, size - done);

        if (result <= 0)
        {
            return -1;
        }
        done += (size_t)result;
    }
    return 0;
}

/* Paint writes BMP only. A name ending in .png must never receive BMP bytes:
 * other programs trust the extension, and the original PNG would be lost. */
static int path_is_png(const char *path)
{
    const char *dot = path != 0 ? strrchr(path, '.') : 0;

    return dot != 0 && strchr(dot, '/') == 0 && strcasecmp(dot, ".png") == 0;
}

static int paint_save_bmp(const char *path)
{
    int width = doc_w();
    int height = doc_h();
    uint32_t stride = ((uint32_t)width * 3u + 3u) & ~3u;
    uint32_t image_size = stride * (uint32_t)height;
    uint32_t file_size = 54u + image_size;
    unsigned char *image = 0;
    long fd = -1;
    int result = -1;
    int y;

    if (path == 0 || path[0] == '\0' || path_is_png(path))
    {
        return -1;
    }
    image = (unsigned char *)malloc(file_size);
    if (image == 0)
    {
        return -1;
    }
    memset(image, 0, file_size);
    image[0] = 'B';
    image[1] = 'M';
    write_le32(image + 2, file_size);
    write_le32(image + 10, 54u);
    write_le32(image + 14, 40u);
    write_le32(image + 18, (uint32_t)width);
    write_le32(image + 22, (uint32_t)height);
    write_le16(image + 26, 1u);
    write_le16(image + 28, 24u);
    write_le32(image + 34, image_size);

    /* Positive height = bottom-up: the first row in the file is the bottom
     * row of the picture. */
    for (y = height - 1; y >= 0; --y)
    {
        uint32_t *source = g_doc.pixels + ((size_t)y * (size_t)width);
        unsigned char *target = image + 54u + ((uint32_t)(height - 1 - y) * stride);
        size_t x;

        for (x = 0; x < (size_t)width; ++x)
        {
            uint32_t pixel = source[x];

            target[x * 3] = (unsigned char)(pixel & 0xFFu);
            target[x * 3 + 1] = (unsigned char)((pixel >> 8) & 0xFFu);
            target[x * 3 + 2] = (unsigned char)((pixel >> 16) & 0xFFu);
        }
    }

    fd = savanxp_open_mode(path, SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE);
    if (fd < 0)
    {
        free(image);
        return -1;
    }
    savanxp_close((int)fd);
    /* Pre-alocar el tamano final ANTES de escribir: ver el comentario de
     * arriba sobre el costo de crecer. */
    if (savanxp_truncate(path, file_size) != 0)
    {
        free(image);
        return -1;
    }
    fd = savanxp_open_mode(path, SAVANXP_OPEN_WRITE);
    if (fd >= 0 && write_all((int)fd, image, file_size) == 0)
    {
        result = 0;
    }
    if (fd >= 0)
    {
        savanxp_close((int)fd);
    }
    free(image);
    return result;
}

static void undo_clear(void);

/* Makes `next` the document. The old bitmap is freed, and so are the undo
 * snapshots: they have the old size and could not be restored into this one. */
static void paint_adopt_doc(struct sx_bitmap *next)
{
    undo_clear();
    sx_bitmap_destroy(&g_doc);
    g_doc = *next;
    sx_painter_init(&g_doc_painter, &g_doc);
}

/* Opens a decoded PNG as a document of its own size. */
static int paint_insert_png(const unsigned char *data, uint32_t size)
{
    struct sx_png_image image = {0, 0u, 0u};
    struct sx_bitmap next;

    if (sx_png_decode(data, (size_t)size, &image) != 0)
    {
        return -1;
    }
    if (image.width == 0u || image.height == 0u || image.width > (uint32_t)PAINT_DOC_W ||
        image.height > (uint32_t)PAINT_DOC_H ||
        !sx_bitmap_create(&next, (int)image.width, (int)image.height, SX_PIXEL_FORMAT_BGRX8888))
    {
        sx_png_free(&image);
        return -1;
    }
    memcpy(next.pixels, image.pixels, (size_t)image.width * (size_t)image.height * 4u);
    sx_png_free(&image);
    paint_adopt_doc(&next);
    return 0;
}

static int paint_load_image(const char *path)
{
    static const unsigned char k_png_signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    struct savanxp_stat info = {0};
    struct sx_bitmap next;
    long fd = -1;
    unsigned char *data = 0;
    uint32_t pixel_offset;
    uint32_t header_size;
    long width;
    long height;
    int top_down = 0;
    uint32_t stride;
    long y;
    int result;

    if (path == 0 || path[0] == '\0')
    {
        return -1;
    }
    /* 4 MB alcanza para cualquier PNG que quepa en el lienzo; el BMP se
     * valida por dentro igual que antes. */
    if (savanxp_stat(path, &info) != 0 || info.st_size < 8u ||
        info.st_size > 4u * 1024u * 1024u)
    {
        return -1;
    }
    data = (unsigned char *)malloc(info.st_size);
    if (data == 0)
    {
        return -1;
    }
    fd = savanxp_open_mode(path, SAVANXP_OPEN_READ);
    if (fd < 0 || read_all((int)fd, data, info.st_size) != 0)
    {
        if (fd >= 0)
        {
            savanxp_close((int)fd);
        }
        free(data);
        return -1;
    }
    savanxp_close((int)fd);

    if (info.st_size >= 8u && memcmp(data, k_png_signature, 8) == 0)
    {
        result = paint_insert_png(data, info.st_size);
        free(data);
        return result;
    }
    if (info.st_size < 54u)
    {
        free(data);
        return -1;
    }
    pixel_offset = read_le32(data + 10);
    header_size = read_le32(data + 14);
    /* Both fields are signed in the format: a negative height means the rows
     * are stored top-down. Read straight into a 64-bit long they would never
     * come out negative. */
    width = (long)(int32_t)read_le32(data + 18);
    height = (long)(int32_t)read_le32(data + 22);
    if (header_size < 40u || read_le16(data + 26) != 1u || read_le16(data + 28) != 24u ||
        read_le32(data + 30) != 0u)
    {
        /* Solo 24 bpp sin comprimir: lo demas (paletas, RLE, bitfields) no
         * existe en este esqueleto. */
        free(data);
        return -1;
    }
    if (height < 0)
    {
        top_down = 1;
        height = -height;
    }
    if (width <= 0 || height <= 0 || width > PAINT_DOC_W || height > PAINT_DOC_H)
    {
        free(data);
        return -1;
    }
    stride = (((uint32_t)width * 3u + 3u) & ~3u);
    if (pixel_offset > info.st_size || stride * (uint32_t)height > info.st_size - pixel_offset ||
        !sx_bitmap_create(&next, (int)width, (int)height, SX_PIXEL_FORMAT_BGRX8888))
    {
        free(data);
        return -1;
    }

    for (y = 0; y < height; ++y)
    {
        long source_row = top_down ? y : (height - 1 - y);
        const unsigned char *source = data + pixel_offset + ((uint32_t)source_row * stride);
        uint32_t *target = next.pixels + ((size_t)y * (size_t)width);
        long x;

        for (x = 0; x < width; ++x)
        {
            target[x] = SXGUI_RGB(source[x * 3 + 2], source[x * 3 + 1], source[x * 3]);
        }
    }
    free(data);
    paint_adopt_doc(&next);
    return 0;
}

/* --- undo ----------------------------------------------------------------- */

static void undo_clear(void)
{
    while (g_undo_count > 0)
    {
        g_undo_count -= 1;
        free(g_undo[g_undo_count]);
        g_undo[g_undo_count] = 0;
    }
}

static void undo_push(void)
{
    uint32_t *slot;

    if (g_undo_count == PAINT_UNDO_DEPTH)
    {
        /* Lleno: se recicla el mas viejo en vez de picar memoria. */
        slot = g_undo[0];
        memmove(&g_undo[0], &g_undo[1], sizeof(g_undo[0]) * (PAINT_UNDO_DEPTH - 1));
        g_undo[PAINT_UNDO_DEPTH - 1] = slot;
    }
    else
    {
        slot = (uint32_t *)malloc(doc_bytes());
        if (slot == 0)
        {
            return;
        }
        g_undo[g_undo_count] = slot;
        g_undo_count += 1;
    }
    memcpy(slot, g_doc.pixels, doc_bytes());
}

static int undo_pop(void)
{
    if (g_undo_count == 0)
    {
        return 0;
    }
    g_undo_count -= 1;
    memcpy(g_doc.pixels, g_undo[g_undo_count], doc_bytes());
    free(g_undo[g_undo_count]);
    g_undo[g_undo_count] = 0;
    g_dirty = 1;
    return 1;
}

/* --- actions -------------------------------------------------------------- */

static void paint_layout(void);

static void paint_new(void)
{
    struct sx_bitmap next;

    /* An opened image may have left a smaller document: go back to the
     * default size. If that allocation fails, keep the current one. */
    if ((doc_w() != PAINT_DOC_W || doc_h() != PAINT_DOC_H) &&
        sx_bitmap_create(&next, PAINT_DOC_W, PAINT_DOC_H, SX_PIXEL_FORMAT_BGRX8888))
    {
        paint_adopt_doc(&next);
    }
    paint_clear(SXGUI_RGB(0xFF, 0xFF, 0xFF));
    g_path[0] = '\0';
    g_dirty = 0;
    g_dragging = 0;
    undo_clear();
    paint_layout();
    update_status(0);
}

static int paint_open_file(const char *path)
{
    struct stat info = {0};

    if (stat(path, &info) == 0 && S_ISDIR(info.st_mode))
    {
        update_status("That path is a directory.");
        return -1;
    }
    if (paint_load_image(path) != 0)
    {
        update_status("Cannot open: BMP 24-bit or PNG 8-bit RGB/RGBA, 512x384 max.");
        return -1;
    }
    snprintf(g_path, sizeof(g_path), "%s", path);
    g_dirty = 0;
    g_dragging = 0;
    undo_clear();
    paint_layout();
    update_status(0);
    return 0;
}

static void paint_path_dialog(int save_as);

static int paint_save(void)
{
    /* No name yet, or a PNG Paint cannot write back: ask for a BMP name. */
    if (g_path[0] == '\0' || path_is_png(g_path))
    {
        paint_path_dialog(1);
        return -1;
    }
    if (paint_save_bmp(g_path) != 0)
    {
        update_status("Cannot write that file.");
        return -1;
    }
    g_dirty = 0;
    update_status("Saved.");
    return 0;
}

static void paint_select_tool(int tool)
{
    if (tool < 0 || tool >= PAINT_TOOL_COUNT)
    {
        return;
    }
    g_tool = tool;
    g_dragging = 0;
    update_status(0);
}

/* --- dialogos ------------------------------------------------------------- */

static void on_open_accept(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    if (g_open_path[0] == '\0')
    {
        return;
    }
    if (g_open_mode == 1)
    {
        if (path_is_png(g_open_path))
        {
            /* Keep the dialog open: the name is the thing to fix. */
            g_open_widgets[0].text = "Paint saves BMP only. Use a .bmp name:";
            sxgui_app_request_repaint(&g_app);
            return;
        }
        /* The document takes the new name only once it is really there. */
        if (paint_save_bmp(g_open_path) == 0)
        {
            snprintf(g_path, sizeof(g_path), "%s", g_open_path);
            g_dirty = 0;
            update_status("Saved.");
        }
        else
        {
            update_status("Cannot write that file.");
        }
    }
    else
    {
        (void)paint_open_file(g_open_path);
    }
    sxgui_dialog_end(&g_app.ui, 1);
}

static void on_open_cancel(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    sxgui_dialog_end(&g_app.ui, 0);
}

static void paint_path_dialog(int save_as)
{
    g_open_mode = save_as;
    if (g_path[0] != '\0')
    {
        snprintf(g_open_path, sizeof(g_open_path), "%s", g_path);
    }
    else
    {
        /* A new document starts in the user's profile: the root is the
         * system's. */
        const char *profile = savanxp_profile_dir();

        snprintf(g_open_path, sizeof(g_open_path), "%s%s", profile, strcmp(profile, "/") == 0 ? "" : "/");
    }
    if (save_as && path_is_png(g_open_path))
    {
        /* Offer the same name as a BMP: same length, so it always fits. */
        memcpy(strrchr(g_open_path, '.'), ".bmp", 4);
    }
    g_open_widgets[0].text = "File name:";
    g_open_widgets[1].caret = (int)strlen(g_open_path);
    g_open_dialog.title = save_as ? "Save As" : "Open";
    g_open_widgets[2].text = save_as ? "Save" : "Open";
    sxgui_dialog_begin(&g_app.ui, &g_open_dialog, PAINT_OPEN_WIDTH, PAINT_OPEN_HEIGHT);
}

static void on_about_ok(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    sxgui_dialog_end(&g_app.ui, 1);
}

/* --- menu ----------------------------------------------------------------- */

static void on_menu_command(int id, void *user)
{
    (void)user;
    switch (id)
    {
    case PAINT_MENU_NEW:
        paint_new();
        break;
    case PAINT_MENU_OPEN:
        paint_path_dialog(0);
        break;
    case PAINT_MENU_SAVE:
        (void)paint_save();
        break;
    case PAINT_MENU_SAVE_AS:
        paint_path_dialog(1);
        break;
    case PAINT_MENU_EXIT:
        sxgui_app_quit(&g_app, 0);
        break;
    case PAINT_MENU_UNDO:
        if (undo_pop())
        {
            update_status("Undone.");
        }
        break;
    case PAINT_MENU_TOOL_PENCIL:
        paint_select_tool(PAINT_TOOL_PENCIL);
        break;
    case PAINT_MENU_TOOL_BRUSH:
        paint_select_tool(PAINT_TOOL_BRUSH);
        break;
    case PAINT_MENU_TOOL_ERASER:
        paint_select_tool(PAINT_TOOL_ERASER);
        break;
    case PAINT_MENU_TOOL_PICKER:
        paint_select_tool(PAINT_TOOL_PICKER);
        break;
    case PAINT_MENU_TOOL_FILL:
        paint_select_tool(PAINT_TOOL_FILL);
        break;
    case PAINT_MENU_TOOL_LINE:
        paint_select_tool(PAINT_TOOL_LINE);
        break;
    case PAINT_MENU_TOOL_RECT:
        paint_select_tool(PAINT_TOOL_RECT);
        break;
    case PAINT_MENU_TOOL_ELLIPSE:
        paint_select_tool(PAINT_TOOL_ELLIPSE);
        break;
    case PAINT_MENU_ABOUT:
        sxgui_dialog_begin(&g_app.ui, &g_about_dialog, PAINT_ABOUT_WIDTH, PAINT_ABOUT_HEIGHT);
        break;
    default:
        break;
    }
    sxgui_app_request_repaint(&g_app);
}

static const struct sxgui_menu_item k_file_items[] = {
    {"New", PAINT_MENU_NEW, 0},
    {"Open...", PAINT_MENU_OPEN, 0},
    {"Save", PAINT_MENU_SAVE, 0},
    {"Save As...", PAINT_MENU_SAVE_AS, 0},
    {0, 0, 0},
    {"Exit", PAINT_MENU_EXIT, 0},
};

static const struct sxgui_menu_item k_edit_items[] = {
    {"Undo", PAINT_MENU_UNDO, 0},
};

static const struct sxgui_menu_item k_tool_items[] = {
    {"Pencil", PAINT_MENU_TOOL_PENCIL, 0},
    {"Brush", PAINT_MENU_TOOL_BRUSH, 0},
    {"Eraser", PAINT_MENU_TOOL_ERASER, 0},
    {"Picker", PAINT_MENU_TOOL_PICKER, 0},
    {"Fill", PAINT_MENU_TOOL_FILL, 0},
    {"Line", PAINT_MENU_TOOL_LINE, 0},
    {"Rectangle", PAINT_MENU_TOOL_RECT, 0},
    {"Ellipse", PAINT_MENU_TOOL_ELLIPSE, 0},
};

static const struct sxgui_menu_item k_help_items[] = {
    {"About Paint", PAINT_MENU_ABOUT, 0},
};

static const struct sxgui_menu k_menus[] = {
    {"File", k_file_items, (int)(sizeof(k_file_items) / sizeof(k_file_items[0]))},
    {"Edit", k_edit_items, (int)(sizeof(k_edit_items) / sizeof(k_edit_items[0]))},
    {"Tool", k_tool_items, (int)(sizeof(k_tool_items) / sizeof(k_tool_items[0]))},
    {"Help", k_help_items, (int)(sizeof(k_help_items) / sizeof(k_help_items[0]))},
};

static struct sxgui_menubar g_menubar = {
    k_menus,
    (int)(sizeof(k_menus) / sizeof(k_menus[0])),
    -1,
    -1,
    on_menu_command,
    0
};

/* --- layout --------------------------------------------------------------- */

static void paint_layout(void)
{
    int top = sxgui_menubar_height();
    int width = (int)g_app.gfx.info.width;
    int height = (int)g_app.gfx.info.height;
    int colors_y = top + SXGUI_MARGIN;
    int swatch_x = SXGUI_MARGIN + PAINT_CURRENT_SIZE + SXGUI_GAP;
    int tools_y = colors_y + PAINT_CURRENT_SIZE + SXGUI_GAP;
    int grid_width = PAINT_TOOL_COLS * PAINT_CELL_SIZE + (PAINT_TOOL_COLS - 1) * PAINT_CELL_GAP;
    int grid_rows = (PAINT_TOOL_COUNT + PAINT_TOOL_COLS - 1) / PAINT_TOOL_COLS;
    int grid_height = grid_rows * PAINT_CELL_SIZE + (grid_rows - 1) * PAINT_CELL_GAP;
    int canvas_x = SXGUI_MARGIN + grid_width + PAINT_TOOL_PAD * 2 + SXGUI_GAP;
    int status_y = height - SXGUI_MARGIN - SXGUI_STATUS_HEIGHT;
    int index;

    g_current_rect = sx_rect_make(SXGUI_MARGIN, colors_y, PAINT_CURRENT_SIZE, PAINT_CURRENT_SIZE);
    g_swatch_rect = sx_rect_make(swatch_x, colors_y + (PAINT_CURRENT_SIZE - PAINT_SWATCH_SIZE) / 2,
                                 PAINT_SWATCH_COUNT * (PAINT_SWATCH_SIZE + PAINT_SWATCH_GAP),
                                 PAINT_SWATCH_SIZE);

    g_tool_panel = sx_rect_make(SXGUI_MARGIN, tools_y, grid_width + PAINT_TOOL_PAD * 2,
                                grid_height + PAINT_TOOL_PAD * 2);
    for (index = 0; index < PAINT_TOOL_COUNT; ++index)
    {
        int column = index % PAINT_TOOL_COLS;
        int row = index / PAINT_TOOL_COLS;

        g_tool_cells[index] = sx_rect_make(g_tool_panel.x + PAINT_TOOL_PAD +
                                           column * (PAINT_CELL_SIZE + PAINT_CELL_GAP),
                                           g_tool_panel.y + PAINT_TOOL_PAD +
                                           row * (PAINT_CELL_SIZE + PAINT_CELL_GAP),
                                           PAINT_CELL_SIZE, PAINT_CELL_SIZE);
    }

    g_canvas_outer = sx_rect_make(canvas_x, tools_y, doc_w() + SXGUI_BORDER_SUNKEN * 2,
                                  doc_h() + SXGUI_BORDER_SUNKEN * 2);
    g_canvas_inner = sx_rect_make(g_canvas_outer.x + SXGUI_BORDER_SUNKEN,
                                  g_canvas_outer.y + SXGUI_BORDER_SUNKEN, doc_w(), doc_h());

    if (status_y < tools_y + g_canvas_outer.height + SXGUI_GAP &&
        status_y < tools_y + g_tool_panel.height + SXGUI_GAP)
    {
        status_y = tools_y + g_canvas_outer.height + SXGUI_GAP;
    }
    g_widgets[0] = sxgui_label(sx_rect_make(SXGUI_MARGIN, status_y, width - SXGUI_MARGIN * 2,
                                            SXGUI_STATUS_HEIGHT),
                               g_status);
    g_widgets[0].flags |= SXGUI_FLAG_SUNKEN;
}

static void on_resize(struct sxgui_app *app)
{
    (void)app;
    paint_layout();
}

/* --- dibujo --------------------------------------------------------------- */

static int tool_at_point(int x, int y)
{
    int index;

    for (index = 0; index < PAINT_TOOL_COUNT; ++index)
    {
        if (sx_rect_contains_point(g_tool_cells[index], x, y))
        {
            return index;
        }
    }
    return -1;
}

static int swatch_at_point(int x, int y)
{
    int index;

    for (index = 0; index < PAINT_SWATCH_COUNT; ++index)
    {
        struct sx_rect cell = sx_rect_make(g_swatch_rect.x + index * (PAINT_SWATCH_SIZE + PAINT_SWATCH_GAP),
                                           g_swatch_rect.y, PAINT_SWATCH_SIZE, PAINT_SWATCH_SIZE);

        if (sx_rect_contains_point(cell, x, y))
        {
            return index;
        }
    }
    return -1;
}

/* Mini-glifo de cada herramienta, centrado en su celda. Tinta plana sobre la
 * cara del boton: es una caja de iconos, no un lienzo. */
static void paint_tool_glyph(struct sx_painter *painter, int tool, struct sx_rect cell)
{
    int x0 = cell.x + 5;
    int y0 = cell.y + 5;
    int x1 = cell.x + cell.width - 6;
    int y1 = cell.y + cell.height - 6;

    switch (tool)
    {
    case PAINT_TOOL_PENCIL:
        sx_painter_draw_line(painter, x0, y1 - 2, x1 - 2, y0 + 2, SXGUI_COLOR_TEXT);
        sx_painter_fill_rect(painter, sx_rect_make(x1 - 3, y0 + 1, 3, 3), SXGUI_COLOR_TEXT);
        break;
    case PAINT_TOOL_BRUSH:
        sx_painter_fill_ellipse(painter, sx_rect_make(x0 + 3, y1 - 10, 7, 7), SXGUI_COLOR_TEXT);
        sx_painter_draw_line(painter, x0 + 8, y1 - 8, x1 - 2, y0 + 2, SXGUI_COLOR_TEXT);
        break;
    case PAINT_TOOL_ERASER:
        sx_painter_fill_rect(painter, sx_rect_make(x0 + 4, y0 + 9, 12, 8),
                             SXGUI_COLOR_FIELD);
        sx_painter_draw_frame(painter, sx_rect_make(x0 + 4, y0 + 9, 12, 8), SXGUI_COLOR_TEXT);
        sx_painter_draw_line(painter, x0 + 4, y1 - 4, x0 + 15, y0 + 9, SXGUI_COLOR_TEXT);
        break;
    case PAINT_TOOL_PICKER:
        sx_painter_draw_line(painter, x1 - 2, y0 + 2, x0 + 9, y0 + 11, SXGUI_COLOR_TEXT);
        sx_painter_set_pixel(painter, x0 + 5, y1 - 5, SXGUI_COLOR_TEXT);
        sx_painter_set_pixel(painter, x0 + 6, y1 - 4, SXGUI_COLOR_TEXT);
        sx_painter_set_pixel(painter, x0 + 4, y1 - 4, SXGUI_COLOR_TEXT);
        sx_painter_set_pixel(painter, x0 + 5, y1 - 3, SXGUI_COLOR_TEXT);
        break;
    case PAINT_TOOL_FILL:
    {
        /* Cubo ladeado que vierte una gota. */
        struct sx_point body[4];

        body[0].x = x0 + 5;
        body[0].y = y0 + 5;
        body[1].x = x1 - 3;
        body[1].y = y0 + 8;
        body[2].x = x1 - 7;
        body[2].y = y1 - 8;
        body[3].x = x0 + 8;
        body[3].y = y1 - 8;
        sx_painter_fill_polygon(painter, body, 4, SXGUI_COLOR_TEXT);
        sx_painter_set_pixel(painter, x0 + 12, y1 - 4, SXGUI_COLOR_TEXT);
        sx_painter_set_pixel(painter, x0 + 11, y1 - 3, SXGUI_COLOR_TEXT);
        sx_painter_set_pixel(painter, x0 + 13, y1 - 3, SXGUI_COLOR_TEXT);
        sx_painter_set_pixel(painter, x0 + 12, y1 - 2, SXGUI_COLOR_TEXT);
        break;
    }
    case PAINT_TOOL_LINE:
        sx_painter_draw_line(painter, x0, y1, x1, y0, SXGUI_COLOR_TEXT);
        break;
    case PAINT_TOOL_RECT:
        sx_painter_draw_frame(painter, sx_rect_make(x0, y0 + 3, x1 - x0, y1 - y0 - 6),
                              SXGUI_COLOR_TEXT);
        break;
    case PAINT_TOOL_ELLIPSE:
        sx_painter_draw_ellipse(painter, sx_rect_make(x0, y0 + 3, x1 - x0, y1 - y0 - 6),
                                SXGUI_COLOR_TEXT);
        break;
    default:
        break;
    }
}

/* Pincel y goma: no hay sx_pen, asi que el trazo grueso es sellos de elipse
 * encadenados paso a paso (caminata por pixel: sin huecos corra lo que corra
 * el raton). La goma pinta de blanco, que es el fondo del documento. */
static void paint_stamp(struct sx_painter *painter, int x, int y, uint32_t colour, int radius)
{
    int diameter = radius * 2 + 1;

    sx_painter_fill_ellipse(painter, sx_rect_make(x - radius, y - radius, diameter, diameter),
                            colour);
}

static void paint_stamp_walk(int x0, int y0, int x1, int y1, uint32_t colour, int radius)
{
    int dx = x1 - x0;
    int dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;
    int steps = adx > ady ? adx : ady;
    int i;

    if (steps < 1)
    {
        steps = 1;
    }
    for (i = 0; i <= steps; ++i)
    {
        paint_stamp(&g_doc_painter, x0 + (dx * i) / steps, y0 + (dy * i) / steps, colour,
                    radius);
    }
}

static void paint_pick_color(int doc_x, int doc_y)
{
    if (doc_x < 0 || doc_y < 0 || doc_x >= doc_w() || doc_y >= doc_h())
    {
        return;
    }
    g_color = g_doc.pixels[(size_t)doc_y * (size_t)doc_w() + (size_t)doc_x] & 0x00FFFFFFu;
    update_status(0);
}

/* True when a fill at (x, y) would change something: inside the document
 * and on a colour other than the new one. The caller pushes undo only then. */
static int paint_fill_changes(int x, int y, uint32_t colour)
{
    if (x < 0 || y < 0 || x >= doc_w() || y >= doc_h())
    {
        return 0;
    }
    return g_doc.pixels[(size_t)y * (size_t)doc_w() + (size_t)x] != colour;
}

/* Queues pixel `index` if nobody has seen it yet and it has the target
 * colour. Marking it seen on the way in is what bounds the stack. */
static void paint_fill_visit(int index, uint32_t target, int *stack, size_t *top,
                             unsigned char *seen)
{
    unsigned char bit = (unsigned char)(1u << (index % 8));

    if ((seen[(size_t)index / 8u] & bit) == 0 && g_doc.pixels[(size_t)index] == target)
    {
        seen[(size_t)index / 8u] |= bit;
        stack[(*top)++] = index;
    }
}

/* Bucket: 4-connected flood fill with an explicit stack and a visited set,
 * no recursion (the call stack cannot hold a whole document of pixels). Each
 * pixel enters the stack at most once, so a stack of W*H always suffices and
 * the loop ends: it either paints new pixels or does nothing. Exact match, as
 * in the original: everything this program draws has hard edges, so there is
 * no tolerance to tune. */
static void paint_flood_fill(int seed_x, int seed_y, uint32_t colour)
{
    int width = doc_w();
    int height = doc_h();
    uint32_t target;
    int *stack = 0;
    unsigned char *seen = 0;
    size_t area = (size_t)width * (size_t)height;
    size_t top = 0;

    if (!paint_fill_changes(seed_x, seed_y, colour))
    {
        return;
    }
    target = g_doc.pixels[(size_t)seed_y * (size_t)width + (size_t)seed_x];
    stack = (int *)malloc(area * sizeof(int));
    seen = (unsigned char *)malloc((area + 7u) / 8u);
    if (stack == 0 || seen == 0)
    {
        free(stack);
        free(seen);
        return;
    }
    memset(seen, 0, (area + 7u) / 8u);

    paint_fill_visit(seed_y * width + seed_x, target, stack, &top, seen);
    while (top > 0)
    {
        int index = stack[--top];
        int x = index % width;
        int y = index / width;

        g_doc.pixels[(size_t)index] = colour;
        if (x > 0)
        {
            paint_fill_visit(index - 1, target, stack, &top, seen);
        }
        if (x + 1 < width)
        {
            paint_fill_visit(index + 1, target, stack, &top, seen);
        }
        if (y > 0)
        {
            paint_fill_visit(index - width, target, stack, &top, seen);
        }
        if (y + 1 < height)
        {
            paint_fill_visit(index + width, target, stack, &top, seen);
        }
    }
    free(stack);
    free(seen);
}

static void paint_shape(struct sx_painter *painter, int tool, int x0, int y0, int x1, int y1,
                        uint32_t colour)
{
    struct sx_rect rect;

    switch (tool)
    {
    case PAINT_TOOL_LINE:
        sx_painter_draw_line(painter, x0, y0, x1, y1, colour);
        break;
    case PAINT_TOOL_RECT:
        rect = sx_rect_make(x0 < x1 ? x0 : x1, y0 < y1 ? y0 : y1, x0 < x1 ? x1 - x0 : x0 - x1,
                            y0 < y1 ? y1 - y0 : y0 - y1);
        sx_painter_draw_frame(painter, rect, colour);
        break;
    case PAINT_TOOL_ELLIPSE:
        rect = sx_rect_make(x0 < x1 ? x0 : x1, y0 < y1 ? y0 : y1, x0 < x1 ? x1 - x0 : x0 - x1,
                            y0 < y1 ? y1 - y0 : y0 - y1);
        sx_painter_draw_ellipse(painter, rect, colour);
        break;
    default:
        break;
    }
}

static void commit_drag(void)
{
    if (g_tool != PAINT_TOOL_LINE && g_tool != PAINT_TOOL_RECT &&
        g_tool != PAINT_TOOL_ELLIPSE)
    {
        return;
    }
    /* A rectangle or ellipse with no width or no height draws nothing (a
     * line always draws at least its endpoint): no undo step, no '*'. */
    if (g_tool != PAINT_TOOL_LINE && (g_anchor_x == g_current_x || g_anchor_y == g_current_y))
    {
        return;
    }
    /* Shapes touch the document only here, on release, so this is where
     * the undo snapshot goes. */
    undo_push();
    paint_shape(&g_doc_painter, g_tool, g_anchor_x - g_canvas_inner.x, g_anchor_y - g_canvas_inner.y,
                g_current_x - g_canvas_inner.x, g_current_y - g_canvas_inner.y, g_color);
    g_dirty = 1;
}

static void on_paint(struct sxgui_app *app)
{
    struct sx_painter *painter = &app->ui.painter;
    int index;

    /* Caja de herramientas: la seleccion queda hundida. */
    sxgui_draw_raised_edge(painter, g_tool_panel);
    for (index = 0; index < PAINT_TOOL_COUNT; ++index)
    {
        if (index == g_tool)
        {
            sxgui_draw_sunken_edge(painter, g_tool_cells[index]);
        }
        else
        {
            sxgui_draw_raised_edge(painter, g_tool_cells[index]);
        }
        paint_tool_glyph(painter, index, g_tool_cells[index]);
    }

    /* Colores: el actual en grande y la tira al lado. */
    sx_painter_fill_rect(painter, g_current_rect, g_color);
    sxgui_draw_sunken_edge(painter, g_current_rect);
    for (index = 0; index < PAINT_SWATCH_COUNT; ++index)
    {
        struct sx_rect cell = sx_rect_make(g_swatch_rect.x + index * (PAINT_SWATCH_SIZE + PAINT_SWATCH_GAP),
                                           g_swatch_rect.y, PAINT_SWATCH_SIZE, PAINT_SWATCH_SIZE);

        sx_painter_fill_rect(painter, cell, k_palette[index]);
        if (k_palette[index] == g_color)
        {
            sxgui_draw_sunken_edge(painter, cell);
        }
        else
        {
            sxgui_draw_raised_edge(painter, cell);
        }
    }

    /* Lienzo. */
    sxgui_draw_sunken_edge(painter, g_canvas_outer);
    sx_painter_blit_bitmap(painter, &g_doc, g_canvas_inner.x, g_canvas_inner.y);

    /* Preview del arrastre sobre el lienzo, sin tocar el documento: el proximo
     * frame vuelve a blitear el original. Solo las formas lo tienen; el lapiz,
     * el pincel, la goma y el cuentagotas pintan (o leen) al andar. */
    if (g_dragging && (g_tool == PAINT_TOOL_LINE || g_tool == PAINT_TOOL_RECT ||
                       g_tool == PAINT_TOOL_ELLIPSE))
    {
        if (sx_painter_push_clip(painter, g_canvas_inner))
        {
            paint_shape(painter, g_tool, g_anchor_x, g_anchor_y, g_current_x, g_current_y,
                        g_color);
            sx_painter_pop_clip(painter);
        }
    }
}

/* --- entrada -------------------------------------------------------------- */

static int toolkit_owns_input(void)
{
    return sxgui_dialog_active(&g_app.ui) || g_menubar.open_menu >= 0;
}

static int on_pointer(struct sxgui_app *app, const struct savanxp_gui_pointer_event *event)
{
    uint32_t buttons = event->buttons;
    int left = (buttons & SAVANXP_MOUSE_BUTTON_LEFT) != 0u;
    int left_before = (g_last_buttons & SAVANXP_MOUSE_BUTTON_LEFT) != 0u;
    int tool;
    int swatch;

    (void)app;
    if (toolkit_owns_input())
    {
        g_last_buttons = buttons;
        return 0;
    }

    tool = tool_at_point(event->x, event->y);
    if (tool >= 0)
    {
        if (left && !left_before)
        {
            paint_select_tool(tool);
            sxgui_app_request_repaint(&g_app);
        }
        g_last_buttons = buttons;
        return 1;
    }

    swatch = swatch_at_point(event->x, event->y);
    if (swatch >= 0)
    {
        if (left && !left_before)
        {
            g_color = k_palette[swatch];
            g_dragging = 0;
            update_status(0);
            sxgui_app_request_repaint(&g_app);
        }
        g_last_buttons = buttons;
        return 1;
    }

    if (sx_rect_contains_point(g_canvas_inner, event->x, event->y))
    {
        int doc_x = event->x - g_canvas_inner.x;
        int doc_y = event->y - g_canvas_inner.y;

        if (left && !left_before)
        {
            /* Button down: the stroke starts. Each tool pushes undo only
             * when it is about to change the document: the picker never
             * does, the bucket only on another colour, and shapes on
             * release (commit_drag). */
            g_dragging = 1;
            g_anchor_x = event->x;
            g_anchor_y = event->y;
            g_current_x = event->x;
            g_current_y = event->y;
            g_last_x = event->x;
            g_last_y = event->y;
            if (g_tool == PAINT_TOOL_PICKER)
            {
                paint_pick_color(doc_x, doc_y);
            }
            else if (g_tool == PAINT_TOOL_FILL)
            {
                if (paint_fill_changes(doc_x, doc_y, g_color))
                {
                    undo_push();
                    paint_flood_fill(doc_x, doc_y, g_color);
                    g_dirty = 1;
                }
                update_status(0);
            }
            else if (g_tool == PAINT_TOOL_PENCIL)
            {
                undo_push();
                sx_painter_set_pixel(&g_doc_painter, doc_x, doc_y, g_color);
                g_dirty = 1;
                update_status(0);
            }
            else if (g_tool == PAINT_TOOL_BRUSH || g_tool == PAINT_TOOL_ERASER)
            {
                uint32_t ink = g_tool == PAINT_TOOL_BRUSH
                    ? g_color
                    : SXGUI_RGB(0xFF, 0xFF, 0xFF);

                undo_push();
                paint_stamp(&g_doc_painter, doc_x, doc_y, ink, g_brush_radius);
                g_dirty = 1;
                update_status(0);
            }
        }
        else if (left && left_before && g_dragging)
        {
            if (g_tool == PAINT_TOOL_PICKER)
            {
                paint_pick_color(doc_x, doc_y);
            }
            else if (g_tool == PAINT_TOOL_PENCIL)
            {
                /* Los eventos son snapshots: se une el ultimo punto con el
                 * actual para no dejar huecos cuando el raton corre. */
                sx_painter_draw_line(&g_doc_painter, g_last_x - g_canvas_inner.x,
                                     g_last_y - g_canvas_inner.y, doc_x, doc_y, g_color);
                g_dirty = 1;
                update_status(0);
            }
            else if (g_tool == PAINT_TOOL_BRUSH || g_tool == PAINT_TOOL_ERASER)
            {
                uint32_t ink = g_tool == PAINT_TOOL_BRUSH
                    ? g_color
                    : SXGUI_RGB(0xFF, 0xFF, 0xFF);

                paint_stamp_walk(g_last_x - g_canvas_inner.x, g_last_y - g_canvas_inner.y,
                                 doc_x, doc_y, ink, g_brush_radius);
                g_dirty = 1;
                update_status(0);
            }
            g_current_x = event->x;
            g_current_y = event->y;
            g_last_x = event->x;
            g_last_y = event->y;
        }
        else if (!left && left_before && g_dragging)
        {
            g_current_x = event->x;
            g_current_y = event->y;
            commit_drag();
            g_dragging = 0;
            update_status(0);
        }
        g_last_buttons = buttons;
        return 1;
    }

    /* Soltar fuera del lienzo tambien cierra el trazo. */
    if (!left && left_before && g_dragging)
    {
        commit_drag();
        g_dragging = 0;
        update_status(0);
        g_last_buttons = buttons;
        return 1;
    }
    g_last_buttons = buttons;
    return 0;
}

static int on_key(struct sxgui_app *app, const struct savanxp_input_event *event)
{
    (void)app;
    if (event->type != SAVANXP_INPUT_EVENT_KEY_DOWN || toolkit_owns_input())
    {
        return 0;
    }
    if (event->key == SAVANXP_KEY_F2)
    {
        (void)paint_save();
        return 1;
    }
    if (event->key == SAVANXP_KEY_F3)
    {
        paint_path_dialog(0);
        return 1;
    }
    if ((event->modifiers & SAVANXP_KEY_MOD_CTRL) != 0u &&
        (event->ascii == 'z' || event->ascii == 'Z'))
    {
        if (undo_pop())
        {
            update_status("Undone.");
            sxgui_app_request_repaint(&g_app);
        }
        return 1;
    }
    if (event->ascii >= '1' && event->ascii <= '8')
    {
        paint_select_tool(event->ascii - '1');
        sxgui_app_request_repaint(&g_app);
        return 1;
    }
    if ((event->ascii == '+' || event->ascii == '=') &&
        g_brush_radius < PAINT_BRUSH_MAX_RADIUS)
    {
        g_brush_radius += 1;
        update_status(0);
        sxgui_app_request_repaint(&g_app);
        return 1;
    }
    if ((event->ascii == '-' || event->ascii == '_') &&
        g_brush_radius > PAINT_BRUSH_MIN_RADIUS)
    {
        g_brush_radius -= 1;
        update_status(0);
        sxgui_app_request_repaint(&g_app);
        return 1;
    }
    return 0;
}

/* --- selftest -------------------------------------------------------------
 *
 * Headless: sin ventana, sin WM. Ejercita las mismas funciones que la UI
 * (paint_shape, paint_save_bmp, paint_load_image), no una API paralela: lo que
 * se prueba es lo que dibuja y guarda de verdad. El escenario paint-smoke lo
 * corre como /bin/paint --selftest y espera PAINT SMOKE PASS. */

#define PAINT_SELFTEST_PATH "/paint-selftest.bmp"
#define PAINT_SELFTEST_BAD "/paint-selftest-bad.bin"
#define PAINT_SELFTEST_PNG "/paint-selftest.PNG"

static int paint_selftest_fail(const char *what)
{
    printf("PAINT SMOKE FAIL %s\n", what);
    return 1;
}

static int paint_selftest(void)
{
    uint32_t *reference = 0;
    struct savanxp_stat info = {0};
    FILE *probe = 0;
    unsigned char magic[2];
    const uint32_t stride = ((uint32_t)PAINT_DOC_W * 3u + 3u) & ~3u;
    const long expected_size = (long)(54u + stride * (uint32_t)PAINT_DOC_H);

    if (!sx_bitmap_create(&g_doc, PAINT_DOC_W, PAINT_DOC_H, SX_PIXEL_FORMAT_BGRX8888))
    {
        return paint_selftest_fail("canvas alloc");
    }
    sx_painter_init(&g_doc_painter, &g_doc);
    sx_painter_fill(&g_doc_painter, SXGUI_RGB(0xFF, 0xFF, 0xFF));

    sx_painter_set_pixel(&g_doc_painter, 5, 5, SXGUI_RGB(0x00, 0x00, 0x00));
    paint_shape(&g_doc_painter, PAINT_TOOL_LINE, 10, 10, 100, 50, SXGUI_RGB(0xFF, 0x00, 0x00));
    paint_shape(&g_doc_painter, PAINT_TOOL_RECT, 200, 100, 300, 200,
                SXGUI_RGB(0x00, 0x00, 0xFF));
    paint_shape(&g_doc_painter, PAINT_TOOL_ELLIPSE, 200, 100, 300, 200,
                SXGUI_RGB(0x00, 0x80, 0x00));

    /* Bresenham incluye los extremos; el marco, las esquinas. */
    if (g_doc.pixels[5 * PAINT_DOC_W + 5] != SXGUI_RGB(0x00, 0x00, 0x00))
    {
        sx_bitmap_destroy(&g_doc);
        return paint_selftest_fail("pencil pixel");
    }
    if (g_doc.pixels[10 * PAINT_DOC_W + 10] != SXGUI_RGB(0xFF, 0x00, 0x00))
    {
        sx_bitmap_destroy(&g_doc);
        return paint_selftest_fail("line endpoint");
    }
    if (g_doc.pixels[100 * PAINT_DOC_W + 200] != SXGUI_RGB(0x00, 0x00, 0xFF))
    {
        sx_bitmap_destroy(&g_doc);
        return paint_selftest_fail("rect corner");
    }
    if (g_doc.pixels[300 * PAINT_DOC_W + 400] != SXGUI_RGB(0xFF, 0xFF, 0xFF))
    {
        sx_bitmap_destroy(&g_doc);
        return paint_selftest_fail("background");
    }

    /* Pincel: el centro y los vecinos quedan cubiertos. */
    paint_stamp(&g_doc_painter, 60, 60, SXGUI_RGB(0xFF, 0x00, 0x00), 4);
    if (g_doc.pixels[60 * PAINT_DOC_W + 60] != SXGUI_RGB(0xFF, 0x00, 0x00) ||
        g_doc.pixels[60 * PAINT_DOC_W + 61] != SXGUI_RGB(0xFF, 0x00, 0x00) ||
        g_doc.pixels[61 * PAINT_DOC_W + 60] != SXGUI_RGB(0xFF, 0x00, 0x00))
    {
        sx_bitmap_destroy(&g_doc);
        return paint_selftest_fail("brush stamp");
    }

    /* Cuentagotas: lee el pixel y lo deja como color. */
    sx_painter_set_pixel(&g_doc_painter, 70, 70, SXGUI_RGB(0x00, 0x00, 0xFF));
    paint_pick_color(70, 70);
    if (g_color != SXGUI_RGB(0x00, 0x00, 0xFF))
    {
        sx_bitmap_destroy(&g_doc);
        return paint_selftest_fail("picker");
    }
    paint_pick_color(-1, -1);
    if (g_color != SXGUI_RGB(0x00, 0x00, 0xFF))
    {
        sx_bitmap_destroy(&g_doc);
        return paint_selftest_fail("picker bounds");
    }
    g_color = SXGUI_RGB(0x00, 0x00, 0x00);

    /* Undo: empuja, pinta encima, deshace y vuelve lo mismo. */
    {
        uint32_t *before = (uint32_t *)malloc((size_t)PAINT_DOC_W * (size_t)PAINT_DOC_H * 4u);

        if (before == 0)
        {
            sx_bitmap_destroy(&g_doc);
            return paint_selftest_fail("undo alloc");
        }
        memcpy(before, g_doc.pixels, (size_t)PAINT_DOC_W * (size_t)PAINT_DOC_H * 4u);
        undo_push();
        if (g_undo_count != 1)
        {
            free(before);
            sx_bitmap_destroy(&g_doc);
            return paint_selftest_fail("undo push");
        }
        sx_painter_draw_line(&g_doc_painter, 0, 0, 200, 200, SXGUI_RGB(0x00, 0x80, 0x00));
        if (!undo_pop())
        {
            free(before);
            sx_bitmap_destroy(&g_doc);
            return paint_selftest_fail("undo empty");
        }
        if (g_undo_count != 0 ||
            memcmp(g_doc.pixels, before, (size_t)PAINT_DOC_W * (size_t)PAINT_DOC_H * 4u) != 0)
        {
            free(before);
            sx_bitmap_destroy(&g_doc);
            return paint_selftest_fail("undo");
        }
        free(before);
    }

    /* Cubo: marco cerrado, rellena dentro sin tocar el marco ni salir. La
     * segunda pasada con el mismo color tiene que volver al instante. */
    paint_shape(&g_doc_painter, PAINT_TOOL_RECT, 300, 200, 400, 280,
                SXGUI_RGB(0x00, 0x00, 0xFF));
    paint_flood_fill(350, 240, SXGUI_RGB(0xFF, 0x00, 0x00));
    paint_flood_fill(350, 240, SXGUI_RGB(0xFF, 0x00, 0x00));
    if (g_doc.pixels[240 * PAINT_DOC_W + 350] != SXGUI_RGB(0xFF, 0x00, 0x00))
    {
        sx_bitmap_destroy(&g_doc);
        return paint_selftest_fail("fill interior");
    }
    if (g_doc.pixels[200 * PAINT_DOC_W + 300] != SXGUI_RGB(0x00, 0x00, 0xFF) ||
        g_doc.pixels[200 * PAINT_DOC_W + 350] != SXGUI_RGB(0x00, 0x00, 0xFF))
    {
        sx_bitmap_destroy(&g_doc);
        return paint_selftest_fail("fill frame");
    }
    if (g_doc.pixels[240 * PAINT_DOC_W + 299] != SXGUI_RGB(0xFF, 0xFF, 0xFF) ||
        g_doc.pixels[240 * PAINT_DOC_W + 400] != SXGUI_RGB(0xFF, 0xFF, 0xFF))
    {
        sx_bitmap_destroy(&g_doc);
        return paint_selftest_fail("fill leak");
    }

    reference = (uint32_t *)malloc((size_t)PAINT_DOC_W * (size_t)PAINT_DOC_H * 4u);
    if (reference == 0)
    {
        sx_bitmap_destroy(&g_doc);
        return paint_selftest_fail("reference alloc");
    }
    memcpy(reference, g_doc.pixels, (size_t)PAINT_DOC_W * (size_t)PAINT_DOC_H * 4u);

    (void)remove(PAINT_SELFTEST_PATH);
    if (paint_save_bmp(PAINT_SELFTEST_PATH) != 0)
    {
        free(reference);
        sx_bitmap_destroy(&g_doc);
        return paint_selftest_fail("save");
    }
    /* El tamano clava el formato por fuera: si el padding estuviera mal, el
     * round-trip podria seguir pasando y el archivo seria invalido. */
    if (savanxp_stat(PAINT_SELFTEST_PATH, &info) != 0 || info.st_size != (uint32_t)expected_size)
    {
        free(reference);
        sx_bitmap_destroy(&g_doc);
        return paint_selftest_fail("file size");
    }
    probe = fopen(PAINT_SELFTEST_PATH, "rb");
    if (probe == 0 || fread(magic, 1, 2, probe) != 2 || magic[0] != 'B' || magic[1] != 'M')
    {
        if (probe != 0)
        {
            fclose(probe);
        }
        free(reference);
        sx_bitmap_destroy(&g_doc);
        return paint_selftest_fail("magic");
    }
    fclose(probe);

    sx_painter_fill(&g_doc_painter, SXGUI_RGB(0x00, 0x00, 0x00));
    if (paint_load_image(PAINT_SELFTEST_PATH) != 0)
    {
        free(reference);
        sx_bitmap_destroy(&g_doc);
        return paint_selftest_fail("load");
    }
    if (memcmp(g_doc.pixels, reference, (size_t)PAINT_DOC_W * (size_t)PAINT_DOC_H * 4u) != 0)
    {
        free(reference);
        sx_bitmap_destroy(&g_doc);
        return paint_selftest_fail("round-trip");
    }

    probe = fopen(PAINT_SELFTEST_BAD, "wb");
    if (probe == 0)
    {
        free(reference);
        sx_bitmap_destroy(&g_doc);
        return paint_selftest_fail("bad-file setup");
    }
    (void)fwrite("hello", 1, 5, probe);
    fclose(probe);
    if (paint_load_image(PAINT_SELFTEST_BAD) == 0)
    {
        free(reference);
        sx_bitmap_destroy(&g_doc);
        return paint_selftest_fail("garbage accepted");
    }

    /* The real PNG on the volume (libsximg, through the UI path): the
     * document takes the image's own size, and known pixels come through. */
    if (paint_load_image("/paint-test.png") != 0)
    {
        free(reference);
        sx_bitmap_destroy(&g_doc);
        return paint_selftest_fail("png asset");
    }
    if (doc_w() != 16 || doc_h() != 16)
    {
        free(reference);
        sx_bitmap_destroy(&g_doc);
        return paint_selftest_fail("png size");
    }
    if (g_doc.pixels[0] != 0x000000u || g_doc.pixels[15 * doc_w() + 15] != 0xBE90C0u ||
        g_doc.pixels[3 * doc_w() + 7] != 0x1AC4ECu)
    {
        free(reference);
        sx_bitmap_destroy(&g_doc);
        return paint_selftest_fail("png pixels");
    }

    /* A .png name never receives BMP bytes, whatever its case. */
    (void)remove(PAINT_SELFTEST_PNG);
    if (paint_save_bmp(PAINT_SELFTEST_PNG) == 0 || savanxp_stat(PAINT_SELFTEST_PNG, &info) == 0)
    {
        free(reference);
        sx_bitmap_destroy(&g_doc);
        return paint_selftest_fail("png overwritten");
    }

    /* Saving keeps the opened size: 16x16 out, 16x16 back, same pixels. */
    memcpy(reference, g_doc.pixels, doc_bytes());
    if (paint_save_bmp(PAINT_SELFTEST_PATH) != 0 || savanxp_stat(PAINT_SELFTEST_PATH, &info) != 0 ||
        info.st_size != 54u + 48u * 16u)
    {
        free(reference);
        sx_bitmap_destroy(&g_doc);
        return paint_selftest_fail("small save");
    }
    if (paint_load_image(PAINT_SELFTEST_PATH) != 0 || doc_w() != 16 || doc_h() != 16 ||
        memcmp(g_doc.pixels, reference, doc_bytes()) != 0)
    {
        free(reference);
        sx_bitmap_destroy(&g_doc);
        return paint_selftest_fail("small round-trip");
    }

    /* Top-down BMP (negative height), 2x2: the first row in the file is the
     * top row of the picture. Rows are 6 bytes padded to 8. */
    {
        unsigned char bmp[54 + 16];

        memset(bmp, 0, sizeof(bmp));
        bmp[0] = 'B';
        bmp[1] = 'M';
        write_le32(bmp + 2, sizeof(bmp));
        write_le32(bmp + 10, 54u);
        write_le32(bmp + 14, 40u);
        write_le32(bmp + 18, 2u);
        write_le32(bmp + 22, (uint32_t)-2);
        write_le16(bmp + 26, 1u);
        write_le16(bmp + 28, 24u);
        write_le32(bmp + 34, 16u);
        bmp[54 + 2] = 0xFF; /* (0, 0) red, stored as B G R */
        bmp[62 + 0] = 0xFF; /* (0, 1) blue */
        probe = fopen(PAINT_SELFTEST_PATH, "wb");
        if (probe == 0 || fwrite(bmp, 1, sizeof(bmp), probe) != sizeof(bmp))
        {
            if (probe != 0)
            {
                fclose(probe);
            }
            free(reference);
            sx_bitmap_destroy(&g_doc);
            return paint_selftest_fail("top-down setup");
        }
        fclose(probe);
        if (paint_load_image(PAINT_SELFTEST_PATH) != 0 || doc_w() != 2 || doc_h() != 2 ||
            g_doc.pixels[0] != SXGUI_RGB(0xFF, 0x00, 0x00) ||
            g_doc.pixels[2] != SXGUI_RGB(0x00, 0x00, 0xFF))
        {
            free(reference);
            sx_bitmap_destroy(&g_doc);
            return paint_selftest_fail("top-down bmp");
        }
    }

    /* The bucket on its own colour changes nothing, so it pushes no undo. */
    if (paint_fill_changes(0, 0, g_doc.pixels[0]) ||
        !paint_fill_changes(0, 0, SXGUI_RGB(0x00, 0xFF, 0x00)))
    {
        free(reference);
        sx_bitmap_destroy(&g_doc);
        return paint_selftest_fail("fill no-op");
    }

    (void)remove(PAINT_SELFTEST_PATH);
    (void)remove(PAINT_SELFTEST_BAD);
    free(reference);
    sx_bitmap_destroy(&g_doc);
    printf("PAINT SMOKE PASS pixels=%d\n", PAINT_DOC_W * PAINT_DOC_H);
    return 0;
}

/* --- arranque ------------------------------------------------------------- */

static void build_dialogs(void)
{
    g_open_widgets[0] = sxgui_label(sx_rect_make(SXGUI_DIALOG_MARGIN, SXGUI_DIALOG_MARGIN,
                                                 PAINT_OPEN_WIDTH - SXGUI_DIALOG_MARGIN * 2, 18),
                                    "File name:");
    g_open_widgets[1] = sxgui_textfield(sx_rect_make(SXGUI_DIALOG_MARGIN,
                                                     SXGUI_DIALOG_MARGIN + 26,
                                                     PAINT_OPEN_WIDTH - SXGUI_DIALOG_MARGIN * 2,
                                                     SXGUI_FIELD_HEIGHT),
                                        g_open_path, sizeof(g_open_path));
    g_open_widgets[2] = sxgui_button(sx_rect_make(PAINT_OPEN_WIDTH - SXGUI_DIALOG_MARGIN -
                                                      SXGUI_BUTTON_WIDTH * 2 - SXGUI_GAP,
                                                  PAINT_OPEN_HEIGHT - SXGUI_DIALOG_MARGIN -
                                                      SXGUI_BUTTON_HEIGHT,
                                                  SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
                                     "Open", on_open_accept, 0);
    g_open_widgets[3] = sxgui_button(sx_rect_make(PAINT_OPEN_WIDTH - SXGUI_DIALOG_MARGIN -
                                                      SXGUI_BUTTON_WIDTH,
                                                  PAINT_OPEN_HEIGHT - SXGUI_DIALOG_MARGIN -
                                                      SXGUI_BUTTON_HEIGHT,
                                                  SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
                                     "Cancel", on_open_cancel, 0);
    g_open_dialog.title = "Open";
    g_open_dialog.widgets = g_open_widgets;
    g_open_dialog.widget_count = 4;
    g_open_dialog.initial_focus = 1;
    g_open_dialog.default_button = 2;

    g_about_widgets[0] = sxgui_label(sx_rect_make(SXGUI_DIALOG_MARGIN, SXGUI_DIALOG_MARGIN,
                                                  PAINT_ABOUT_WIDTH - SXGUI_DIALOG_MARGIN * 2, 18),
                                     "SavanXP Paint");
    g_about_widgets[1] = sxgui_label(sx_rect_make(SXGUI_DIALOG_MARGIN, SXGUI_DIALOG_MARGIN + 26,
                                                  PAINT_ABOUT_WIDTH - SXGUI_DIALOG_MARGIN * 2, 18),
                                     "Version: " SAVANXP_VERSION_STRING);
    g_about_widgets[2] = sxgui_label(sx_rect_make(SXGUI_DIALOG_MARGIN, SXGUI_DIALOG_MARGIN + 52,
                                                  PAINT_ABOUT_WIDTH - SXGUI_DIALOG_MARGIN * 2, 18),
                                     "F2 saves  F3 opens  1-8 tools  ^Z undoes");
    g_about_widgets[3] = sxgui_button(sx_rect_make((PAINT_ABOUT_WIDTH - SXGUI_BUTTON_WIDTH) / 2,
                                                   PAINT_ABOUT_HEIGHT - SXGUI_DIALOG_MARGIN -
                                                       SXGUI_BUTTON_HEIGHT,
                                                   SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
                                      "OK", on_about_ok, 0);
    g_about_dialog.title = "About Paint";
    g_about_dialog.widgets = g_about_widgets;
    g_about_dialog.widget_count = 4;
    g_about_dialog.default_button = 3;
}

int main(int argc, char **argv)
{
    if (argc > 1 && argv != 0 && argv[1] != 0 && strcmp(argv[1], "--selftest") == 0)
    {
        return paint_selftest();
    }
    build_dialogs();
    if (!sx_bitmap_create(&g_doc, PAINT_DOC_W, PAINT_DOC_H, SX_PIXEL_FORMAT_BGRX8888))
    {
        puts_err("paint: out of memory for the canvas\n");
        return 1;
    }
    sx_painter_init(&g_doc_painter, &g_doc);
    paint_clear(SXGUI_RGB(0xFF, 0xFF, 0xFF));
    g_path[0] = '\0';
    g_dirty = 0;
    update_status(0);

    if (sxgui_app_init(&g_app, "paint", g_widgets, 1) < 0)
    {
        return 1;
    }
    g_app.on_key = on_key;
    g_app.on_pointer = on_pointer;
    g_app.on_paint = on_paint;
    g_app.on_resize = on_resize;
    sxgui_set_menubar(&g_app.ui, &g_menubar);
    (void)sxgui_app_set_content_size(&g_app, 620, 520);
    paint_layout();

    /* argv[1] es el archivo a abrir: es como lo lanza Files. */
    if (argc > 1 && argv != 0 && argv[1] != 0 && argv[1][0] != '\0')
    {
        (void)paint_open_file(argv[1]);
    }
    sxgui_focus(&g_app.ui, -1);
    return sxgui_app_run(&g_app);
}
