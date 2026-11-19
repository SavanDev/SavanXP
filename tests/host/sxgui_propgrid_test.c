/* SXGUI_PROPGRID: the property grid an IDE edits a form with. Keys and
 * clicks go in through the toolkit's real entry points; values are read back
 * from the caller's buffers, and CHANGE is counted. */
#include <stdio.h>
#include <string.h>
#include "sxgfx/sxgui.h"
#include "sxgfx/pixels.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static struct sxgui_context g_ctx;
static int g_changes;
static int g_changed_row = -1;

static void on_action(struct sxgui_widget *widget, void *user)
{
    (void)user;
    if (widget->action == SXGUI_ACTION_CHANGE)
    {
        ++g_changes;
        g_changed_row = widget->value;
    }
}

static void key(unsigned int code, int ascii, unsigned int modifiers)
{
    struct savanxp_input_event event = {SAVANXP_INPUT_EVENT_KEY_DOWN, code, ascii, modifiers};
    sxgui_handle_key(&g_ctx, &event);
}
static void type(const char *text)
{
    for (; *text != '\0'; ++text) key((unsigned char)*text, *text, 0);
}
static void click(int x, int y)
{
    struct savanxp_gui_pointer_event press = {x, y, 0, 1};
    struct savanxp_gui_pointer_event release = {x, y, 0, 0};
    sxgui_handle_pointer(&g_ctx, &press);
    sxgui_handle_pointer(&g_ctx, &release);
}

int main(void)
{
    static uint32_t pixels[320 * 200];
    struct savanxp_fb_info info = {320, 200, 320 * 4, 32, sizeof(pixels)};
    static const char *const borders[] = {"0 - None", "1 - Fixed Single", "2 - Sizable"};
    char name[32] = "Form1";
    char visible[8] = "True";
    char border[32] = "2 - Sizable";
    char back[16] = "&H00FF00&";
    struct sxgui_property rows[4] = {
        {"Name", SXGUI_PROPERTY_TEXT, name, sizeof(name), 0, 0},
        {"Visible", SXGUI_PROPERTY_BOOL, visible, sizeof(visible), 0, 0},
        {"BorderStyle", SXGUI_PROPERTY_LIST, border, sizeof(border), borders, 3},
        {"BackColor", SXGUI_PROPERTY_COLOR, back, sizeof(back), 0, 0},
    };
    struct sxgui_widget widgets[1];
    struct sxgui_widget *w = &widgets[0];
    int row_height = gfx_text_height() + 4;
    int value_x = 200; /* right of the name column (2/5 of the width) */
    int index;

    widgets[0] = sxgui_propgrid(sx_rect_make(0, 0, 300, 180), rows, 4);
    widgets[0].on_action = on_action;
    sxgui_context_init(&g_ctx, pixels, &info, widgets, 1);
    sxgui_focus(&g_ctx, 0);
    CHECK(w->value == 0);

    /* Moving fires nothing; BOOL flips with Space and Enter. */
    key(SAVANXP_KEY_DOWN, 0, 0);
    CHECK(w->value == 1 && g_changes == 0);
    key(' ', ' ', 0);
    CHECK(strcmp(visible, "False") == 0 && g_changes == 1 && g_changed_row == 1);
    key(SAVANXP_KEY_ENTER, 0, 0);
    CHECK(strcmp(visible, "True") == 0 && g_changes == 2);

    /* LIST steps and wraps; Shift goes back. */
    key(SAVANXP_KEY_DOWN, 0, 0);
    key(' ', ' ', 0);
    CHECK(strcmp(border, "0 - None") == 0 && g_changed_row == 2);
    key(' ', ' ', SAVANXP_KEY_MOD_SHIFT);
    CHECK(strcmp(border, "2 - Sizable") == 0 && g_changes == 4);

    /* TEXT: typing replaces, and CHANGE fires once, on Enter. */
    key(SAVANXP_KEY_HOME, 0, 0);
    type("Main");
    CHECK(w->prop_editing && strcmp(name, "Main") == 0 && g_changes == 4);
    key(SAVANXP_KEY_ENTER, 0, 0);
    CHECK(!w->prop_editing && strcmp(name, "Main") == 0 && g_changes == 5 && g_changed_row == 0);

    /* F2 edits from the end; Esc puts the old text back and fires nothing. */
    key(SAVANXP_KEY_F2, 0, 0);
    key(SAVANXP_KEY_BACKSPACE, 0, 0);
    key(SAVANXP_KEY_BACKSPACE, 0, 0);
    type("xx");
    CHECK(strcmp(name, "Maxx") == 0);
    key(SAVANXP_KEY_ESC, 0, 0);
    CHECK(!w->prop_editing && strcmp(name, "Main") == 0 && g_changes == 5);

    /* Leaving the row keeps the edit. */
    key(SAVANXP_KEY_F2, 0, 0);
    type("1");
    key(SAVANXP_KEY_DOWN, 0, 0);
    CHECK(strcmp(name, "Main1") == 0 && g_changes == 6 && w->value == 1);

    /* Clicks: another row's name selects it; the selected row's value acts. */
    click(20, 2 + 3 * row_height + 3);
    CHECK(w->value == 3 && g_changes == 6);
    click(20, 2 + 1 * row_height + 3);
    CHECK(w->value == 1 && strcmp(visible, "True") == 0);
    click(value_x, 2 + 1 * row_height + 3);
    CHECK(strcmp(visible, "False") == 0 && g_changes == 7);

    /* sxgui_propgrid_commit keeps an edit in progress, for a caller that is
     * about to read the values. */
    key(SAVANXP_KEY_HOME, 0, 0);
    key(SAVANXP_KEY_F2, 0, 0);
    type("2");
    CHECK(sxgui_propgrid_commit(w) == 1 && strcmp(name, "Main12") == 0 && g_changes == 8);
    CHECK(sxgui_propgrid_commit(w) == 0);

    /* The colour shows as a swatch. */
    sxgui_paint_content(&g_ctx);
    {
        int green = 0;
        for (index = 0; index < 320 * 200; ++index)
        {
            if ((pixels[index] & 0xFFFFFFu) == 0x00FF00u) ++green;
        }
        CHECK(green > 20);
    }

    /* More rows than fit: the first row shown follows the selection. */
    {
        static struct sxgui_property many[40];
        static char values[40][8];
        for (index = 0; index < 40; ++index)
        {
            snprintf(values[index], sizeof(values[index]), "%d", index);
            many[index].name = "Row";
            many[index].kind = SXGUI_PROPERTY_TEXT;
            many[index].value = values[index];
            many[index].capacity = sizeof(values[index]);
        }
        widgets[0] = sxgui_propgrid(sx_rect_make(0, 0, 300, 180), many, 40);
        sxgui_focus(&g_ctx, 0);
        key(SAVANXP_KEY_END, 0, 0);
        CHECK(w->value == 39 && w->prop_first > 0 && w->prop_first <= 39);
        key(SAVANXP_KEY_HOME, 0, 0);
        CHECK(w->value == 0 && w->prop_first == 0);
    }

    if (failures) return 1;
    puts("SXGUI PROPGRID TEST PASS");
    return 0;
}
