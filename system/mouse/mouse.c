/* Mouse Properties: native pointer scheme selection, preview and persistence. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sxn/sxngui.h"
#include "sxn/sxncursor.h"
#include "cursor_catalog.h"
#include "mouse.sunres.h"

enum { TABS, SCHEME_GROUP, SCHEME, CUSTOMIZE, POINTERS, DEFAULT, LICENSE, STATUS, OK, CANCEL, APPLY, WIDGETS };
static struct sxngui_app app;
static struct sxgui_widget widgets[WIDGETS];
static struct cursor_theme themes[CURSOR_MAX_THEMES];
static const char *theme_names[CURSOR_MAX_THEMES];
static const char *tabs[] = {"Pointers"};
static const char *role_names[SAVANXP_CURSOR_SHAPE_COUNT] = {
    "Normal Select", "Busy", "Text Select", "Move", "Horizontal Resize", "Vertical Resize",
    "Unavailable", "Link Select", "Diagonal Resize 1", "Diagonal Resize 2"
};
static struct cursor_image *preview;
static unsigned theme_count;
static int active = -1, selected = -1, valid_preview, unsaved;
static char status[144], license_text[80];
static struct sxgui_dialog license_dialog;
static struct sxgui_widget license_widgets[9];
static char notice[1024];
static const char *notice_lines[7];

static void update_buttons(void) {
    if (valid_preview && (selected != active || unsaved)) widgets[APPLY].flags &= ~SXGUI_FLAG_DISABLED;
    else widgets[APPLY].flags |= SXGUI_FLAG_DISABLED;
    if (valid_preview) widgets[OK].flags &= ~SXGUI_FLAG_DISABLED;
    else widgets[OK].flags |= SXGUI_FLAG_DISABLED;
}
static void preview_selected(void) {
    selected = widgets[SCHEME].value;
    valid_preview = selected >= 0 && (unsigned)selected < theme_count && preview &&
        cursor_catalog_read(themes[selected].id, preview, 1) == 0;
    if (!valid_preview) snprintf(status, sizeof(status), "This scheme is incomplete or unreadable. Choose another scheme.");
    else snprintf(status, sizeof(status), "%s", selected == active ? "Current pointer scheme." : "Preview only. Apply to use this scheme.");
    snprintf(license_text, sizeof(license_text), "License: %s",
        selected >= 0 && (unsigned)selected < theme_count ? themes[selected].license : "Unavailable");
    update_buttons(); sxngui_app_request_repaint(&app);
}
static void scheme_changed(struct sxgui_widget *widget, void *user) {
    (void)widget; (void)user; preview_selected();
}
static int apply_selected(void) {
    struct sxn_cursor_reply reply;
    long result;
    if (!valid_preview) return 0;
    result = sxncursor_select(themes[selected].id, &reply);
    if (result < 0) snprintf(status, sizeof(status), "The scheme could not be applied (%ld). Previous scheme retained.", result);
    else {
        active = selected; unsaved = reply.save_status < 0;
        snprintf(status, sizeof(status), "%s", unsaved ? "Applied for this session. Saving failed; Apply retries." : "Pointer scheme applied and saved.");
    }
    update_buttons(); sxngui_app_request_repaint(&app);
    return result == 0 && !unsaved;
}
static void action(struct sxgui_widget *widget, void *user) {
    (void)user;
    if (widget == &widgets[CANCEL]) sxngui_app_quit(&app, 0);
    else if (widget == &widgets[DEFAULT]) {
        unsigned i;
        for (i = 0; i < theme_count; ++i) if (!strcmp(themes[i].id, "classic")) {
            widgets[SCHEME].value = (int)i; preview_selected(); break;
        }
    } else if (widget == &widgets[OK]) {
        if (selected == active && !unsaved) sxngui_app_quit(&app, 0);
        else if (apply_selected()) sxngui_app_quit(&app, 0);
    } else if (widget == &widgets[APPLY]) (void)apply_selected();
}
static void close_license(struct sxgui_widget *widget, void *user) {
    (void)widget; (void)user; sxgui_dialog_end(&app.ui, 0);
}
static void show_license(struct sxgui_widget *widget, void *user) {
    char path[128];
    long handle, count;
    unsigned i;
    char *line;
    (void)widget; (void)user;
    if (selected < 0) return;
    snprintf(path, sizeof(path), "/system/cursors/%s/NOTICE", themes[selected].id);
    handle = sxn_open_file(path, SXN_FILE_READ);
    count = handle >= 0 ? sxn_read(handle, notice, sizeof(notice)-1) : handle;
    if (handle >= 0) (void)sxn_close(handle);
    if (count < 0) snprintf(notice, sizeof(notice), "License: %s\nSee /system/cursors/%s/LICENSE", themes[selected].license, themes[selected].id);
    else notice[count] = 0;
    line = notice;
    for (i = 0; i < 7; ++i) {
        char *end = strchr(line, '\n');
        notice_lines[i] = line;
        if (end) { *end = 0; line = end+1; }
        else line += strlen(line);
        license_widgets[i] = sxgui_label(sx_rect_make(14, 16+(int)i*24, 572, 22), notice_lines[i]);
    }
    license_widgets[7] = sxgui_button(sx_rect_make(496, 190, 88, 26), "Close", close_license, 0);
    memset(&license_dialog, 0, sizeof(license_dialog));
    license_dialog.title = "Cursor Scheme License"; license_dialog.widgets = license_widgets;
    license_dialog.widget_count = 8; license_dialog.default_button = 7; license_dialog.initial_focus = 7;
    sxgui_dialog_begin(&app.ui, &license_dialog, 600, 232);
    sxngui_app_request_repaint(&app);
}
static void layout(struct sxngui_app *a) {
    int width = (int)a->info.width, height = (int)a->info.height;
    widgets[TABS].rect = sx_rect_make(8, 8, width-16, height-52);
    widgets[SCHEME_GROUP].rect = sx_rect_make(22, 46, width-144, 80);
    widgets[SCHEME].rect = sx_rect_make(36, 76, width-172, 25);
    widgets[CUSTOMIZE].rect = sx_rect_make(22, 140, width-44, 20);
    widgets[POINTERS].rect = sx_rect_make(22, 164, width-44, height-294);
    widgets[DEFAULT].rect = sx_rect_make(width-238, height-118, 104, 26);
    widgets[LICENSE].rect = sx_rect_make(width-126, height-118, 104, 26);
    widgets[STATUS].rect = sx_rect_make(22, height-78, width-44, 24);
    widgets[OK].rect = sx_rect_make(width-280, height-34, 80, 26);
    widgets[CANCEL].rect = sx_rect_make(width-192, height-34, 80, 26);
    widgets[APPLY].rect = sx_rect_make(width-104, height-34, 80, 26);
    sxngui_app_request_repaint(a);
}
static void paint_image(struct sx_painter *p, const struct cursor_image *image, int left, int top, int small, uint32_t background) {
    int width = image->width, height = image->height, x, y;
    if (small && (width > 16 || height > 16)) {
        int longest = width > height ? width : height;
        width = width * 16 / longest; height = height * 16 / longest;
    }
    for (y = 0; y < height; ++y) for (x = 0; x < width; ++x) {
        unsigned index = (y*image->height/height)*image->width + x*image->width/width;
        sx_painter_fill_rect(p, sx_rect_make(left+x, top+y, 1, 1), cursor_image_pixel(image, index, background));
    }
}
static void paint(struct sxngui_app *a) {
    struct sx_painter *p = &a->ui.painter;
    int width = (int)a->info.width, shape = widgets[POINTERS].value;
    struct sx_rect box = sx_rect_make(width-110, 52, 88, 74);
    int row_height = sx_painter_text_height(p)+4, index;
    sx_painter_fill_rect(p, box, SXGUI_COLOR_FACE);
    sxchrome_draw_sunken(p, box);
    if (valid_preview) {
        const struct cursor_image *image = &preview[shape >= 0 ? shape : 0];
        paint_image(p, image, box.x+(box.width-image->width)/2, box.y+(box.height-image->height)/2, 0, 0xff000000u|SXGUI_COLOR_FACE);
        struct sx_rect list = widgets[POINTERS].rect;
        if (sx_painter_push_clip(p, sx_rect_make(list.x+2, list.y+2, list.width-20, list.height-4))) {
            for (index = widgets[POINTERS].scroll; index < SAVANXP_CURSOR_SHAPE_COUNT; ++index) {
                int top = list.y+2+(index-widgets[POINTERS].scroll)*row_height;
                uint32_t bg = 0xff000000u | (index == shape ? SXGUI_COLOR_SELECT : SXGUI_COLOR_FIELD);
                paint_image(p, &preview[index], list.x+list.width-46, top+(row_height-16)/2, 1, bg);
            }
            sx_painter_pop_clip(p);
        }
    }
    sx_painter_draw_text(p, 24, (int)a->info.height-110, license_text, SXGUI_COLOR_TEXT);
}
static void pointer_selected(struct sxgui_widget *widget, void *user) {
    (void)widget; (void)user; sxngui_app_request_repaint(&app);
}

static void selftest(void) {
    struct sxn_cursor_reply reply;
    struct cursor_theme available[CURSOR_MAX_THEMES];
    int32_t idle[2];
    unsigned count = cursor_catalog_discover(available), i;
    int failures = count != 3;
    if (sxn_port_connect(SXN_CURSOR_PORT, idle) == 0) {
        /* An idle peer must not prevent a different peer from being served. */
        if (sxncursor_query(&reply) < 0) ++failures;
        (void)sxn_close(idle[0]); (void)sxn_close(idle[1]);
    } else ++failures;
    for (i = 0; i < count; ++i) {
        if (sxncursor_select(available[i].id, &reply) < 0 || strcmp(reply.theme, available[i].id) || reply.save_status < 0) ++failures;
        if (sxncursor_query(&reply) < 0 || strcmp(reply.theme, available[i].id)) ++failures;
    }
    if (sxncursor_select("../classic", &reply) != -SAVANXP_EINVAL) ++failures;
    if (sxncursor_select("missing-pack", &reply) != -SAVANXP_ENOENT) ++failures;
    if (sxncursor_request(99, 0, &reply) != -SAVANXP_EINVAL) ++failures;
    if (sxncursor_query(&reply) < 0 || strcmp(reply.theme, "hackneyed")) ++failures;
    if (sxncursor_select("classic", &reply) < 0 || reply.save_status < 0) ++failures;
    (void)sxn_log(failures ? "CURSOR SETTINGS FAIL" : "CURSOR SETTINGS PASS");
    sxn_exit(failures ? 1 : 0);
}

int main(int argc, char **argv) {
    struct sxn_cursor_reply reply;
    unsigned i;
    int result;
    if (argc > 1 && !strcmp(argv[1], "--selftest")) selftest();
    preview = malloc(sizeof(*preview)*SAVANXP_CURSOR_SHAPE_COUNT);
    theme_count = cursor_catalog_discover(themes);
    for (i = 0; i < theme_count; ++i) theme_names[i] = themes[i].name;
    if (sxncursor_query(&reply) == 0) {
        for (i = 0; i < theme_count; ++i) if (!strcmp(themes[i].id, reply.theme)) active = (int)i;
        unsaved = reply.save_status < 0;
    }
    widgets[TABS] = sxgui_tabs(sx_rect_make(0,0,0,0), tabs, 1, 0);
    widgets[SCHEME_GROUP] = sxgui_groupbox(sx_rect_make(0,0,0,0), "Scheme");
    widgets[SCHEME] = sxgui_combobox(sx_rect_make(0,0,0,0), theme_names, (int)theme_count, active >= 0 ? active : 0);
    widgets[SCHEME].on_action = scheme_changed;
    widgets[CUSTOMIZE] = sxgui_label(sx_rect_make(0,0,0,0), "Pointers:");
    widgets[POINTERS] = sxgui_listbox(sx_rect_make(0,0,0,0), role_names, SAVANXP_CURSOR_SHAPE_COUNT);
    widgets[POINTERS].value = 0; widgets[POINTERS].on_action = pointer_selected;
    widgets[DEFAULT] = sxgui_button(sx_rect_make(0,0,0,0), "Use Default", action, 0);
    widgets[LICENSE] = sxgui_button(sx_rect_make(0,0,0,0), "License...", show_license, 0);
    widgets[STATUS] = sxgui_label(sx_rect_make(0,0,0,0), status);
    widgets[OK] = sxgui_button(sx_rect_make(0,0,0,0), "OK", action, 0);
    widgets[CANCEL] = sxgui_button(sx_rect_make(0,0,0,0), "Cancel", action, 0);
    widgets[APPLY] = sxgui_button(sx_rect_make(0,0,0,0), "Apply", action, 0);
    result = sxngui_app_init(&app, &sun_resources, widgets, WIDGETS);
    if (result < 0) { (void)sxn_log("mouse: window unavailable"); sxn_exit(1); }
    app.on_resize = layout; app.on_paint = paint;
    (void)sxngui_app_set_content_size(&app, 540, 510);
    layout(&app); preview_selected();
    result = sxngui_app_run(&app);
    free(preview); sxn_exit(result);
    return 0;
}
