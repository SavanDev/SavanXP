#include "libc.h"
#include "windowd_presentation.h"

#define WINDOWD_RGB_LITERAL(red, green, blue) (((uint32_t)(red) << 16) | ((uint32_t)(green) << 8) | (uint32_t)(blue))

/* Identity colour for a window that declares no ACCENT. Only the value: if
 * nobody declared one, the active caption uses the Windows Standard gradient
 * rather than this colour (see accent_declared). */
#define WINDOWD_DEFAULT_ACCENT WINDOWD_RGB_LITERAL(59, 95, 156)

_Static_assert(
    WINDOWD_PRESENTATION_ICON_EXTENT == SAVANXP_WM_PRESENTATION_ICON_EXTENT,
    "the published icon and the frame's icon must be the same size");
_Static_assert(
    WINDOWD_PRESENTATION_LABEL_CAPACITY == SAVANXP_WM_PRESENTATION_TITLE_CAPACITY,
    "the published title and the window's label must have the same capacity");

static void copy_label(char *destination, size_t capacity, const char *source, size_t source_capacity)
{
    size_t index = 0;

    if (destination == 0 || capacity == 0)
    {
        return;
    }
    if (source != 0)
    {
        while (index < source_capacity && source[index] != '\0' && index + 1u < capacity)
        {
            destination[index] = source[index];
            index += 1u;
        }
    }
    destination[index] = '\0';
}

void windowd_presentation_default(struct windowd_presentation *presentation, const char *path)
{
    const char *name = path;
    const char *cursor = path;

    if (presentation == 0)
    {
        return;
    }
    memset(presentation, 0, sizeof(*presentation));
    presentation->fallback_icon_id = DESKTOP_ICON_DESKTOP;
    presentation->accent = WINDOWD_DEFAULT_ACCENT;
    if (path == 0)
    {
        return;
    }
    for (; *cursor != '\0'; ++cursor)
    {
        if (*cursor == '/')
        {
            name = cursor + 1;
        }
    }
    copy_label(presentation->label, sizeof(presentation->label), name, (size_t)-1);
}

int windowd_presentation_adopt(
    struct windowd_presentation *presentation,
    const struct savanxp_wm_presentation *published)
{
    struct windowd_presentation before;

    if (presentation == 0 || published == 0)
    {
        return 0;
    }
    before = *presentation;

    if (published->title[0] != '\0')
    {
        copy_label(presentation->label, sizeof(presentation->label), published->title, sizeof(published->title));
    }
    if ((published->flags & SAVANXP_WM_PRESENTATION_HAS_ACCENT) != 0u)
    {
        presentation->accent = published->accent & 0x00ffffffu;
        presentation->accent_declared = 1u;
    }
    presentation->window_flags = published->window_style;
    /* Only the exact slot size: anything else is not an icon this frame can
     * draw, and an extent beyond the array would read outside the header. */
    if (published->icon_extent == WINDOWD_PRESENTATION_ICON_EXTENT)
    {
        memcpy(presentation->icon_pixels, published->icon_pixels, sizeof(presentation->icon_pixels));
        presentation->icon_extent = WINDOWD_PRESENTATION_ICON_EXTENT;
    }

    {
        /* By hand: this libc has no memcmp. */
        const unsigned char *a = (const unsigned char *)&before;
        const unsigned char *b = (const unsigned char *)presentation;
        size_t index;

        for (index = 0; index < sizeof(before); ++index)
        {
            if (a[index] != b[index])
            {
                return 1;
            }
        }
    }
    return 0;
}

const char *windowd_presentation_label(const struct windowd_presentation *presentation, const char *path)
{
    if (presentation != 0 && presentation->label[0] != '\0')
    {
        return presentation->label;
    }
    /* Last resort: the bare path rather than a window with no identity. */
    return (path != 0 && path[0] != '\0') ? path : "App";
}

uint32_t windowd_presentation_accent(const struct windowd_presentation *presentation)
{
    return presentation != 0 ? presentation->accent : WINDOWD_DEFAULT_ACCENT;
}

int windowd_presentation_has_accent(const struct windowd_presentation *presentation)
{
    return presentation != 0 && presentation->accent_declared != 0u;
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

/* Pure rules, no files and no session: the end-to-end path (a real client
 * publishing from its own .sxe) is asserted in windowd's selftest against
 * progman. */
int windowd_presentation_selftest(void)
{
    static struct windowd_presentation presentation;
    static struct savanxp_wm_presentation published;
    struct desktop_embedded_bitmap storage;
    const struct desktop_embedded_bitmap *icon = 0;

    g_presentation_failures = 0;

    windowd_presentation_default(&presentation, "/bin/notepad");
    expect(strcmp(windowd_presentation_label(&presentation, "/bin/notepad"), "notepad") == 0,
        "presentation: default title is the file name");
    expect(!windowd_presentation_has_accent(&presentation), "presentation: default has no declared accent");
    expect(windowd_presentation_accent(&presentation) == WINDOWD_DEFAULT_ACCENT,
        "presentation: default accent");
    expect(presentation.window_flags == 0u, "presentation: default is an ordinary window");
    expect(windowd_presentation_icon(&presentation, &storage) == desktop_icon_small(DESKTOP_ICON_DESKTOP),
        "presentation: default icon is the generic one");
    windowd_presentation_default(&presentation, 0);
    expect(strcmp(windowd_presentation_label(&presentation, 0), "App") == 0, "presentation: no path gives App");

    windowd_presentation_default(&presentation, "/bin/notepad");
    memset(&published, 0, sizeof(published));
    memcpy(published.title, "Notepad", 8);
    published.flags = SAVANXP_WM_PRESENTATION_HAS_ACCENT;
    published.accent = 0xff786440u; /* the top byte is not colour */
    published.window_style = SAVANXP_WM_WINDOW_STYLE_FIXED_SIZE;
    published.icon_extent = WINDOWD_PRESENTATION_ICON_EXTENT;
    published.icon_pixels[0] = 0xff112233u;
    published.icon_pixels[WINDOWD_PRESENTATION_ICON_PIXELS - 1u] = 0xff445566u;
    expect(windowd_presentation_adopt(&presentation, &published) == 1, "presentation: adopting reports a change");
    expect(strcmp(windowd_presentation_label(&presentation, "/bin/notepad"), "Notepad") == 0,
        "presentation: adopted title");
    expect(windowd_presentation_has_accent(&presentation) &&
        windowd_presentation_accent(&presentation) == 0x00786440u, "presentation: adopted accent, masked");
    expect(presentation.window_flags == SAVANXP_WM_WINDOW_STYLE_FIXED_SIZE, "presentation: adopted style");
    icon = windowd_presentation_icon(&presentation, &storage);
    expect(icon == &storage && icon->pixels[0] == 0xff112233u &&
        icon->pixels[WINDOWD_PRESENTATION_ICON_PIXELS - 1u] == 0xff445566u, "presentation: adopted icon pixels");
    expect(windowd_presentation_adopt(&presentation, &published) == 0, "presentation: same values are no change");

    /* Hostile values: a title with no terminator, an icon at the wrong size. */
    windowd_presentation_default(&presentation, "/bin/x");
    memset(&published, 0, sizeof(published));
    memset(published.title, 'A', sizeof(published.title));
    published.icon_extent = 64u;
    (void)windowd_presentation_adopt(&presentation, &published);
    expect(strlen(presentation.label) == WINDOWD_PRESENTATION_LABEL_CAPACITY - 1u,
        "presentation: an unterminated title is cut and terminated");
    expect(presentation.icon_extent == 0u, "presentation: an icon at the wrong size is ignored");

    return g_presentation_failures;
}
