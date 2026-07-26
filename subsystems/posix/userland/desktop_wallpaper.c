#include "desktop_wallpaper.h"

#define DESKTOP_WALLPAPER_CONFIG_PATH "/disk/desktop.cfg"
#define DESKTOP_WALLPAPER_IMAGE_PATH "/disk/wallpaper.bmp"
#define DESKTOP_WALLPAPER_MAX_DIMENSION 2048

static int g_mode = DESKTOP_WALLPAPER_TEAL;
/* La imagen vive en una seccion propia: esta libc no tiene malloc y un buffer
 * estatico del tamano maximo inflaria el BSS del proceso desktop. */
static int g_image_section_fd = -1;
static uint32_t *g_image_pixels = 0;
static int g_image_width = 0;
static int g_image_height = 0;

static uint16_t read_le16(const unsigned char *bytes)
{
    return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

static uint32_t read_le32(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static int read_exact(int fd, void *buffer, unsigned long count)
{
    unsigned char *out = (unsigned char *)buffer;
    unsigned long done = 0;

    while (done < count)
    {
        long result = read(fd, out + done, count - done);
        if (result <= 0)
        {
            return -1;
        }
        done += (unsigned long)result;
    }
    return 0;
}

static void unload_image(void)
{
    if (g_image_pixels != 0)
    {
        (void)unmap_view(g_image_pixels);
        g_image_pixels = 0;
    }
    if (g_image_section_fd >= 0)
    {
        close(g_image_section_fd);
        g_image_section_fd = -1;
    }
    g_image_width = 0;
    g_image_height = 0;
}

/* Decodifica un BMP sin compresion (BI_RGB) de 24 o 32 bpp, bottom-up o
 * top-down, a pixeles BGRX listos para el painter. Cualquier campo fuera de
 * ese perfil descarta la imagen en silencio: el fondo cae a los modos
 * integrados. */
static void load_image(void)
{
    unsigned char header[54];
    static unsigned char row[DESKTOP_WALLPAPER_MAX_DIMENSION * 4u];
    int fd = -1;
    uint32_t pixel_offset = 0;
    int32_t raw_height = 0;
    int width = 0;
    int height = 0;
    int top_down = 0;
    unsigned int bytes_per_pixel = 0;
    unsigned long row_stride = 0;
    int loaded = 0;
    int y;

    unload_image();

    fd = (int)open_mode(DESKTOP_WALLPAPER_IMAGE_PATH, SAVANXP_OPEN_READ);
    if (fd < 0)
    {
        return;
    }

    if (read_exact(fd, header, sizeof(header)) == 0 && header[0] == 'B' && header[1] == 'M')
    {
        uint16_t bpp = read_le16(header + 28);
        pixel_offset = read_le32(header + 10);
        width = (int)(int32_t)read_le32(header + 18);
        raw_height = (int32_t)read_le32(header + 22);
        top_down = raw_height < 0;
        height = top_down ? -(int)raw_height : (int)raw_height;

        if (read_le32(header + 14) >= 40u &&           /* BITMAPINFOHEADER o mayor */
            read_le16(header + 26) == 1u &&            /* planes */
            (bpp == 24u || bpp == 32u) &&
            read_le32(header + 30) == 0u &&            /* BI_RGB */
            width > 0 && width <= DESKTOP_WALLPAPER_MAX_DIMENSION &&
            height > 0 && height <= DESKTOP_WALLPAPER_MAX_DIMENSION &&
            pixel_offset >= sizeof(header) &&
            seek(fd, (long)pixel_offset, SAVANXP_SEEK_SET) >= 0)
        {
            bytes_per_pixel = bpp / 8u;
            row_stride = (((unsigned long)width * bytes_per_pixel) + 3ul) & ~3ul;
            g_image_section_fd = (int)section_create(
                (unsigned long)width * (unsigned long)height * sizeof(uint32_t),
                SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
            if (g_image_section_fd >= 0)
            {
                void *view = map_view(g_image_section_fd, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
                if (!result_is_error((long)view))
                {
                    g_image_pixels = (uint32_t *)view;
                    loaded = 1;
                    for (y = 0; y < height; ++y)
                    {
                        uint32_t *dest;
                        int x;

                        if (read_exact(fd, row, row_stride) < 0)
                        {
                            loaded = 0;
                            break;
                        }
                        dest = g_image_pixels + ((unsigned long)(top_down ? y : (height - 1 - y)) * (unsigned long)width);
                        for (x = 0; x < width; ++x)
                        {
                            const unsigned char *px = row + ((unsigned long)x * bytes_per_pixel);
                            dest[x] = ((uint32_t)px[2] << 16) | ((uint32_t)px[1] << 8) | (uint32_t)px[0];
                        }
                    }
                }
            }
        }
    }

    close(fd);
    if (loaded)
    {
        g_image_width = width;
        g_image_height = height;
        return;
    }
    unload_image();
}

static void load_config(void)
{
    char digit = 0;
    int fd = (int)open_mode(DESKTOP_WALLPAPER_CONFIG_PATH, SAVANXP_OPEN_READ);

    if (fd < 0)
    {
        /* Sin config previa (instalacion fresca): si el build trae el
         * wallpaper default en /disk, arrancar mostrandolo. */
        if (g_image_pixels != 0)
        {
            g_mode = DESKTOP_WALLPAPER_IMAGE;
        }
        return;
    }
    if (read(fd, &digit, 1) == 1 && digit >= '0' && digit < '0' + DESKTOP_WALLPAPER_MODE_COUNT)
    {
        g_mode = digit - '0';
    }
    close(fd);

    if (g_mode == DESKTOP_WALLPAPER_IMAGE && g_image_pixels == 0)
    {
        g_mode = DESKTOP_WALLPAPER_TEAL;
    }
}

static void save_mode(int mode)
{
    char digit = (char)('0' + mode);
    int fd = (int)open_mode(
        DESKTOP_WALLPAPER_CONFIG_PATH,
        SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE | SAVANXP_OPEN_TRUNCATE);

    if (fd < 0)
    {
        return;
    }
    (void)write(fd, &digit, 1);
    close(fd);
}

static void save_config(void)
{
    save_mode(g_mode);
}

/* Presencia del BMP sin decodificarlo: alcanza para saber si el modo IMAGE es
 * elegible. */
static int image_file_present(void)
{
    int fd = (int)open_mode(DESKTOP_WALLPAPER_IMAGE_PATH, SAVANXP_OPEN_READ);

    if (fd < 0)
    {
        return 0;
    }
    close(fd);
    return 1;
}

/* Lee el modo persistido. Devuelve -1 si no hay config legible. */
static int read_persisted_mode(void)
{
    char digit = 0;
    int mode = -1;
    int fd = (int)open_mode(DESKTOP_WALLPAPER_CONFIG_PATH, SAVANXP_OPEN_READ);

    if (fd < 0)
    {
        return -1;
    }
    if (read(fd, &digit, 1) == 1 && digit >= '0' && digit < '0' + DESKTOP_WALLPAPER_MODE_COUNT)
    {
        mode = digit - '0';
    }
    close(fd);
    return mode;
}

void desktop_wallpaper_init(void)
{
    load_image();
    load_config();
}

int desktop_wallpaper_mode(void)
{
    return g_mode;
}

int desktop_wallpaper_cycle(void)
{
    int next = g_mode;

    do
    {
        next = (next + 1) % DESKTOP_WALLPAPER_MODE_COUNT;
    } while (next == DESKTOP_WALLPAPER_IMAGE && g_image_pixels == 0);

    g_mode = next;
    save_config();
    return g_mode;
}

int desktop_wallpaper_cycle_config(void)
{
    const int has_image = image_file_present();
    int mode = read_persisted_mode();
    int next;

    if (mode < 0)
    {
        /* Sin config previa: mismo criterio que load_config(). */
        mode = has_image ? DESKTOP_WALLPAPER_IMAGE : DESKTOP_WALLPAPER_TEAL;
    }

    next = mode;
    do
    {
        next = (next + 1) % DESKTOP_WALLPAPER_MODE_COUNT;
    } while (next == DESKTOP_WALLPAPER_IMAGE && !has_image);

    /* Solo persiste: no toca g_mode, porque quien cambia el fondo puede no ser
     * quien lo dibuja. El que dibuja se entera por desktop_wallpaper_reload(). */
    save_mode(next);
    return next;
}

int desktop_wallpaper_reload(void)
{
    int mode = read_persisted_mode();

    if (mode < 0 || mode == g_mode)
    {
        return 0;
    }
    if (mode == DESKTOP_WALLPAPER_IMAGE && g_image_pixels == 0)
    {
        return 0;
    }
    g_mode = mode;
    return 1;
}

static void draw_gradient(struct sx_painter *painter, const struct savanxp_fb_info *info)
{
    const int band_height = 4;
    const int height = (int)info->height;
    int y;

    for (y = 0; y < height; y += band_height)
    {
        int t = (y * 255) / (height > 1 ? height - 1 : 1);
        uint32_t colour = gfx_rgb(
            0,
            (uint8_t)(24 + ((132 - 24) * t) / 255),
            (uint8_t)(96 + ((160 - 96) * t) / 255));
        sx_painter_fill_rect(painter, sx_rect_make(0, y, (int)info->width, band_height), colour);
    }
}

/* Rejilla sutil sobre teal oscuro. Solo recorre las lineas que cruzan el clip
 * activo del painter: el compose repinta por fragmentos y las lineas 1px son
 * demasiadas para iterarlas todas en cada sub-rect. */
static void draw_pattern(struct sx_painter *painter, const struct savanxp_fb_info *info)
{
    const int cell = 24;
    const uint32_t base = gfx_rgb(0, 104, 104);
    const uint32_t line = gfx_rgb(0, 122, 122);
    struct sx_rect clip = painter->has_clip ? painter->clip_rect : sx_rect_make(0, 0, (int)info->width, (int)info->height);
    int start;
    int position;

    sx_painter_fill(painter, base);

    start = (clip.x / cell) * cell;
    for (position = start; position < clip.x + clip.width; position += cell)
    {
        sx_painter_fill_rect(painter, sx_rect_make(position, clip.y, 1, clip.height), line);
    }
    start = (clip.y / cell) * cell;
    for (position = start; position < clip.y + clip.height; position += cell)
    {
        sx_painter_fill_rect(painter, sx_rect_make(clip.x, position, clip.width, 1), line);
    }
}

static void draw_image(struct sx_painter *painter, const struct savanxp_fb_info *info)
{
    struct sx_bitmap bitmap;
    struct savanxp_fb_info image_info;

    memset(&image_info, 0, sizeof(image_info));
    image_info.width = (uint32_t)g_image_width;
    image_info.height = (uint32_t)g_image_height;
    image_info.pitch = (uint32_t)g_image_width * (uint32_t)sizeof(uint32_t);
    image_info.bpp = 32u;
    image_info.buffer_size = image_info.pitch * image_info.height;
    sx_bitmap_wrap(&bitmap, g_image_pixels, &image_info, SX_PIXEL_FORMAT_BGRX8888);

    if (g_image_width == (int)info->width && g_image_height == (int)info->height)
    {
        sx_painter_blit_bitmap(painter, &bitmap, 0, 0);
        return;
    }
    sx_painter_draw_scaled_bitmap_nearest(
        painter,
        &bitmap,
        sx_rect_make(0, 0, (int)info->width, (int)info->height),
        sx_rect_make(0, 0, g_image_width, g_image_height));
}

void desktop_wallpaper_draw(struct sx_painter *painter, const struct savanxp_fb_info *info)
{
    if (painter == 0 || info == 0)
    {
        return;
    }

    switch (g_mode)
    {
    case DESKTOP_WALLPAPER_GRADIENT:
        draw_gradient(painter, info);
        return;
    case DESKTOP_WALLPAPER_PATTERN:
        draw_pattern(painter, info);
        return;
    case DESKTOP_WALLPAPER_IMAGE:
        if (g_image_pixels != 0)
        {
            draw_image(painter, info);
            return;
        }
        break;
    default:
        break;
    }
    sx_painter_fill(painter, gfx_rgb(0, 128, 128));
}
