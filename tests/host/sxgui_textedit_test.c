/* SXGUI_TEXTEDIT with a document the toolkit owns: the buffer grows past any
 * starting size, and undo/redo walk the edits back and forth by step. Keys go
 * in through sxgui_handle_key, as a window's would. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sxgfx/sxgui.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static char g_clipboard[256];
static int clip_get(char *buffer, int capacity)
{
    int length = (int)strlen(g_clipboard);
    if (length == 0 || capacity <= 0) return 0;
    snprintf(buffer, (size_t)capacity, "%s", g_clipboard);
    return length;
}
static int clip_set(const char *text)
{
    snprintf(g_clipboard, sizeof(g_clipboard), "%s", text);
    return 0;
}
static const struct sxgui_host g_host = {0, clip_get, clip_set};

static struct sxgui_context g_ctx;

static void key(unsigned int code, int ascii, unsigned int modifiers)
{
    struct savanxp_input_event event = {SAVANXP_INPUT_EVENT_KEY_DOWN, code, ascii, modifiers};
    sxgui_handle_key(&g_ctx, &event);
}
static void type(const char *text)
{
    for (; *text != '\0'; ++text)
    {
        if (*text == '\n') key(SAVANXP_KEY_ENTER, 0, 0);
        else key((unsigned char)*text, *text, 0);
    }
}
static void ctrl(int letter, unsigned int extra) { key((unsigned)letter, letter, SAVANXP_KEY_MOD_CTRL | extra); }

int main(void)
{
    static uint32_t pixels[320 * 200];
    struct savanxp_fb_info info = {320, 200, 320 * 4, 32, sizeof(pixels)};
    struct sxgui_widget widgets[1];
    struct sxgui_widget *w = &widgets[0];
    char fixed[8] = "";
    char big[3000];
    int index;

    sxgui_set_host(&g_host);

    /* A fixed buffer keeps its old behaviour: it stops at capacity, no undo. */
    widgets[0] = sxgui_textedit(sx_rect_make(0, 0, 300, 180), fixed, (int)sizeof(fixed));
    sxgui_context_init(&g_ctx, pixels, &info, widgets, 1);
    sxgui_focus(&g_ctx, 0);
    type("abcdefghij");
    CHECK(strcmp(fixed, "abcdefg") == 0);
    CHECK(!sxgui_textedit_can_undo(w));
    ctrl('z', 0);
    CHECK(strcmp(fixed, "abcdefg") == 0);

    /* A document: grows from its initial text. */
    CHECK(sxgui_textedit_document(w, "Sub Main\n") == 0);
    CHECK(w->edit_buffer != fixed && strcmp(w->edit_buffer, "Sub Main\n") == 0);
    CHECK(w->modified == 0 && w->caret == 0);
    w->caret = 9;
    for (index = 0; index < (int)sizeof(big) - 1; ++index) big[index] = (char)('a' + index % 26);
    big[sizeof(big) - 1] = '\0';
    type(big);
    CHECK((int)strlen(w->edit_buffer) == 9 + (int)sizeof(big) - 1);
    CHECK(w->edit_capacity > (int)sizeof(big));
    CHECK(w->modified == 1);
    /* The whole run of typing is one step. */
    ctrl('z', 0);
    CHECK(strcmp(w->edit_buffer, "Sub Main\n") == 0);
    CHECK(w->caret == 9);
    CHECK(!sxgui_textedit_can_undo(w) && sxgui_textedit_can_redo(w));
    ctrl('y', 0);
    CHECK((int)strlen(w->edit_buffer) == 9 + (int)sizeof(big) - 1);
    CHECK(sxgui_textedit_undo(w) == 1 && strcmp(w->edit_buffer, "Sub Main\n") == 0);

    /* Enter closes the run: each line undoes on its own. */
    type("  x = 1\n  y = 2");
    CHECK(strcmp(w->edit_buffer, "Sub Main\n  x = 1\n  y = 2") == 0);
    CHECK(!sxgui_textedit_can_redo(w)); /* a new edit drops what was undone */
    ctrl('z', 0);
    CHECK(strcmp(w->edit_buffer, "Sub Main\n  x = 1\n") == 0);
    ctrl('z', 0);
    CHECK(strcmp(w->edit_buffer, "Sub Main\n  x = 1") == 0);
    ctrl('z', 0);
    CHECK(strcmp(w->edit_buffer, "Sub Main\n") == 0);
    ctrl('Z', SAVANXP_KEY_MOD_SHIFT); /* Ctrl+Shift+Z redoes */
    CHECK(strcmp(w->edit_buffer, "Sub Main\n  x = 1") == 0);

    /* A run of Backspace is one step, and so is a run of Delete. */
    key(SAVANXP_KEY_BACKSPACE, 0, 0);
    key(SAVANXP_KEY_BACKSPACE, 0, 0);
    key(SAVANXP_KEY_BACKSPACE, 0, 0);
    key(SAVANXP_KEY_BACKSPACE, 0, 0);
    CHECK(strcmp(w->edit_buffer, "Sub Main\n  x") == 0);
    w->caret = 0;
    key(SAVANXP_KEY_DELETE, 0, 0);
    key(SAVANXP_KEY_DELETE, 0, 0);
    key(SAVANXP_KEY_DELETE, 0, 0);
    key(SAVANXP_KEY_DELETE, 0, 0);
    CHECK(strcmp(w->edit_buffer, "Main\n  x") == 0);
    ctrl('z', 0);
    CHECK(strcmp(w->edit_buffer, "Sub Main\n  x") == 0 && w->caret == 4);
    ctrl('z', 0);
    CHECK(strcmp(w->edit_buffer, "Sub Main\n  x = 1") == 0);

    /* Typing over a selection undoes as one step: back to the selected text. */
    w->sel_anchor = 0;
    w->caret = 3;
    type("Function");
    CHECK(strcmp(w->edit_buffer, "Function Main\n  x = 1") == 0);
    ctrl('z', 0);
    CHECK(strcmp(w->edit_buffer, "Sub Main\n  x = 1") == 0);

    /* Cut and paste are steps of their own, and a paste grows the buffer. */
    w->sel_anchor = 0;
    w->caret = 4;
    ctrl('x', 0);
    CHECK(strcmp(g_clipboard, "Sub ") == 0 && strcmp(w->edit_buffer, "Main\n  x = 1") == 0);
    w->caret = (int)strlen(w->edit_buffer);
    ctrl('v', 0);
    CHECK(strcmp(w->edit_buffer, "Main\n  x = 1Sub ") == 0);
    CHECK(sxgui_textedit_undo(w) && strcmp(w->edit_buffer, "Main\n  x = 1") == 0);
    CHECK(sxgui_textedit_undo(w) && strcmp(w->edit_buffer, "Sub Main\n  x = 1") == 0);
    CHECK(sxgui_textedit_redo(w) && strcmp(w->edit_buffer, "Main\n  x = 1") == 0);

    /* Loading another document empties the history. */
    CHECK(sxgui_textedit_document(w, 0) == 0);
    CHECK(w->edit_buffer[0] == '\0' && !sxgui_textedit_can_undo(w) && !sxgui_textedit_can_redo(w));

    /* The history is bounded: past the limit the oldest steps are forgotten,
     * and what is kept still undoes correctly. */
    for (index = 0; index < 5000; ++index)
    {
        type("x\n");
    }
    CHECK((int)strlen(w->edit_buffer) == 10000);
    for (index = 0; sxgui_textedit_undo(w); ++index)
    {
    }
    CHECK(index > 0 && index < 10000);
    CHECK((int)strlen(w->edit_buffer) == 10000 - index);

    sxgui_textedit_release(w);
    CHECK(w->edit_state == 0 && w->edit_buffer == 0);

    if (failures) return 1;
    puts("SXGUI TEXTEDIT TEST PASS");
    return 0;
}
