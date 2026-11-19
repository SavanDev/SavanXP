/* SxBASIC forms runtime (docs/userland/SXBASIC.md#forms): each loaded form
 * is an array of SXGUI widgets, its controls' properties live in those
 * widgets, and the toolkit's actions become the program's events.
 *
 * The first form shown is the main window; another form shows as a dialog
 * with ShowModal, which makes the procedure that called it wait. MsgBox and
 * InputBox are dialogs too. A waiting procedure resumes from
 * sxb_forms_tick or sxb_forms_dialog_closed, never from inside the
 * toolkit's own event handling. */
#include "forms.h"
#include "core/sxb_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAPTION_CAPACITY 256
#define FIELD_CAPACITY 256
#define AREA_CAPACITY 32768
#define MESSAGE_LINES 8

struct control_state {
    int widget;               /* index into the form's widgets, -1 for none */
    char *text;               /* caption, or a text box's edit buffer */
    int capacity;
    char **items;
    int item_count;
    int item_capacity;
    int enabled;              /* timers and menu items */
    int interval;
    unsigned long due;
    int menu_slot;            /* a MenuItem's place in the form's item table */
};

struct form_state {
    int loaded;
    int shown;
    struct sxgui_widget *widgets;
    int widget_count;
    struct control_state *controls;
    char title[CAPTION_CAPACITY];
    struct sxgui_dialog dialog;
    struct sxgui_menu *menus;
    int menu_count;
    struct sxgui_menu_item *items;
    int item_count;
    int *item_control;
    struct sxgui_menubar bar;
    int focus;                /* the control to focus when it shows, -1 */
};

struct sxb_forms {
    const struct sxb_program *prog;
    struct sxb_vm *vm;
    struct sxb_window window;
    struct form_state *forms;
    int main_form;
    int modal_form;
    struct sxgui_context *ctx;
    int menu_offset;
    int ended;
    /* the procedure waiting in ShowModal, MsgBox or InputBox */
    int resume_pending;
    struct sxb_scalar resume_value;
    char resume_text[FIELD_CAPACITY];
    /* the message box or input box on screen */
    int message_active;
    int message_input;
    char message_text[1024];
    char message_lines[MESSAGE_LINES][160];
    char message_title[CAPTION_CAPACITY];
    char input_buffer[FIELD_CAPACITY];
    struct sxgui_widget message_widgets[MESSAGE_LINES + 4];
    int message_results[4];
    int message_button_first;
    struct sxgui_dialog message_dialog;
    int message_result;
    char text_out[FIELD_CAPACITY];
};

static struct sxb_forms *g_forms; /* the toolkit's callbacks carry no pointer to it */

static void settle(struct sxb_forms *forms);

/* ---- small helpers ----------------------------------------------------------------- */

static const struct sxb_control_def *control_def(struct sxb_forms *forms, int form, int control)
{
    const struct sxb_form_def *def = &forms->prog->forms[form];
    return control < 0 ? &def->form : &def->controls[control];
}

static struct sxgui_widget *widget_of(struct sxb_forms *forms, int form, int control)
{
    struct form_state *state = &forms->forms[form];
    if (control < 0 || !state->loaded || state->controls[control].widget < 0) return NULL;
    return &state->widgets[state->controls[control].widget];
}

static void repaint(struct sxb_forms *forms)
{
    if (forms->window.repaint) forms->window.repaint(forms->window.user);
}

static void end_program(struct sxb_forms *forms, int code)
{
    if (forms->ended) return;
    forms->ended = 1;
    forms->window.quit(forms->window.user, code);
}

/* After an invocation: an uncaught error or End ends the program. */
static void after_run(struct sxb_forms *forms, enum sxb_status status)
{
    if (status == SXB_ERROR && !forms->ended) {
        const struct sxb_runtime_error *error = sxb_vm_error(forms->vm);
        char message[512];
        snprintf(message, sizeof(message), "Runtime error %d: %s (%s, line %d)", error->number, error->description,
                 error->file, error->line);
        forms->window.fail(forms->window.user, message);
        end_program(forms, 1);
    } else if (status == SXB_DONE) {
        end_program(forms, 0);
    }
}

static void invoke(struct sxb_forms *forms, int proc, int *cancel)
{
    if (proc < 0 || forms->ended) return;
    after_run(forms, sxb_vm_invoke(forms->vm, proc, cancel));
}

void sxb_forms_fire(struct sxb_forms *forms, int form, int control, int event)
{
    if (!forms->forms[form].loaded) return;
    invoke(forms, control_def(forms, form, control)->handlers[event], NULL);
}

static char *dup_text(const char *text, int capacity)
{
    char *copy = malloc((size_t)capacity);
    if (copy) snprintf(copy, (size_t)capacity, "%s", text);
    return copy;
}

/* ---- events from the toolkit ------------------------------------------------------------ */

static void on_action(struct sxgui_widget *widget, void *user)
{
    struct sxb_forms *forms = g_forms;
    intptr_t code = (intptr_t)user;
    int form = (int)(code >> 16), control = (int)(code & 0xFFFF);
    if (forms == NULL || form >= forms->prog->form_count) return;
    int kind = control_def(forms, form, control)->kind;
    int event = -1;
    switch (kind) {
    case SXB_CTL_BUTTON:
    case SXB_CTL_CHECKBOX:
    case SXB_CTL_RADIO:
    case SXB_CTL_COMBOBOX:
        event = SXB_EV_CLICK;
        break;
    case SXB_CTL_LISTBOX:
        event = widget->action == SXGUI_ACTION_ACTIVATE ? SXB_EV_DBLCLICK : SXB_EV_CLICK;
        break;
    case SXB_CTL_TEXTBOX:
    case SXB_CTL_SCROLLBAR:
    case SXB_CTL_TRACKBAR:
        event = SXB_EV_CHANGE;
        break;
    default:
        break;
    }
    if (event >= 0) sxb_forms_fire(forms, form, control, event);
}

static void on_menu(int id, void *user)
{
    struct sxb_forms *forms = g_forms;
    (void)user;
    if (forms == NULL || forms->main_form < 0) return;
    struct form_state *state = &forms->forms[forms->main_form];
    if (id < 0 || id >= state->item_count) return;
    sxb_forms_fire(forms, forms->main_form, state->item_control[id], SXB_EV_CLICK);
}

/* ---- loading ---------------------------------------------------------------------- */

static struct sx_rect rect_of(const struct sxb_control_def *def, int offset)
{
    return sx_rect_make((int)def->values[SXB_P_LEFT], (int)def->values[SXB_P_TOP] + offset,
                        (int)def->values[SXB_P_WIDTH], (int)def->values[SXB_P_HEIGHT]);
}

static void sync_items(struct control_state *state, struct sxgui_widget *widget)
{
    widget->items = (const char *const *)state->items;
    widget->item_count = state->item_count;
}

static int build_widgets(struct sxb_forms *forms, int form)
{
    const struct sxb_form_def *def = &forms->prog->forms[form];
    struct form_state *state = &forms->forms[form];
    state->widgets = calloc((size_t)def->control_count + 1u, sizeof(*state->widgets));
    state->controls = calloc((size_t)def->control_count + 1u, sizeof(*state->controls));
    if (state->widgets == NULL || state->controls == NULL) return SXB_ERR_OUT_OF_MEMORY;
    int menu_items = 0;
    for (int k = 0; k < def->control_count; k++) {
        if (def->controls[k].kind == SXB_CTL_MENU) state->menu_count++;
        if (def->controls[k].kind == SXB_CTL_MENUITEM) menu_items++;
    }
    if (state->menu_count) {
        state->menus = calloc((size_t)state->menu_count, sizeof(*state->menus));
        state->items = calloc((size_t)menu_items + 1u, sizeof(*state->items));
        state->item_control = calloc((size_t)menu_items + 1u, sizeof(*state->item_control));
        if (!state->menus || !state->items || !state->item_control) return SXB_ERR_OUT_OF_MEMORY;
    }
    int menu = -1;
    for (int k = 0; k < def->control_count; k++) {
        const struct sxb_control_def *c = &def->controls[k];
        struct control_state *cs = &state->controls[k];
        struct sx_rect rect = rect_of(c, 0);
        struct sxgui_widget w;
        int has_widget = 1;
        cs->widget = -1;
        cs->menu_slot = -1;
        cs->enabled = c->values[SXB_P_ENABLED] != 0;
        cs->interval = (int)c->values[SXB_P_INTERVAL];
        int capacity = CAPTION_CAPACITY;
        if (c->kind == SXB_CTL_TEXTBOX) capacity = c->values[SXB_P_MULTILINE] ? AREA_CAPACITY : FIELD_CAPACITY;
        cs->text = dup_text(c->text, capacity);
        cs->capacity = capacity;
        if (cs->text == NULL) return SXB_ERR_OUT_OF_MEMORY;
        switch (c->kind) {
        case SXB_CTL_LABEL: w = sxgui_label(rect, cs->text); break;
        case SXB_CTL_BUTTON: w = sxgui_button(rect, cs->text, on_action, NULL); break;
        case SXB_CTL_CHECKBOX: w = sxgui_checkbox(rect, cs->text, c->values[SXB_P_VALUE] != 0); break;
        case SXB_CTL_RADIO: w = sxgui_radio(rect, cs->text, (int)c->values[SXB_P_GROUP], c->values[SXB_P_VALUE] != 0); break;
        case SXB_CTL_TEXTBOX:
            w = c->values[SXB_P_MULTILINE] ? sxgui_textedit(rect, cs->text, capacity)
                                           : sxgui_textfield(rect, cs->text, capacity);
            break;
        case SXB_CTL_LISTBOX: w = sxgui_listbox(rect, NULL, 0); w.value = -1; w.flags |= SXGUI_FLAG_EMPTY_SELECTION; break;
        case SXB_CTL_COMBOBOX: w = sxgui_combobox(rect, NULL, 0, 0); break;
        case SXB_CTL_GROUPBOX: w = sxgui_groupbox(rect, cs->text); break;
        case SXB_CTL_SCROLLBAR:
            w = sxgui_scrollbar(rect, (int)c->values[SXB_P_MIN], (int)c->values[SXB_P_MAX],
                                (int)c->values[SXB_P_LARGECHANGE], (int)c->values[SXB_P_VALUE]);
            if (c->values[SXB_P_HORIZONTAL]) w.flags |= SXGUI_FLAG_HSCROLL;
            break;
        case SXB_CTL_TRACKBAR:
            w = sxgui_trackbar(rect, (int)c->values[SXB_P_MIN], (int)c->values[SXB_P_MAX], (int)c->values[SXB_P_VALUE]);
            break;
        case SXB_CTL_PROGRESS:
            w = sxgui_progress(rect, (int)c->values[SXB_P_MIN], (int)c->values[SXB_P_MAX], (int)c->values[SXB_P_VALUE]);
            break;
        case SXB_CTL_MENU:
            menu++;
            state->menus[menu].title = cs->text;
            state->menus[menu].items = &state->items[state->item_count];
            has_widget = 0;
            break;
        case SXB_CTL_MENUITEM: {
            int slot = state->item_count++;
            int separator = strcmp(cs->text, "-") == 0;
            state->items[slot].text = separator ? NULL : cs->text;
            state->items[slot].id = slot;
            state->items[slot].flags = (cs->enabled ? 0 : SXGUI_MENU_DISABLED) |
                                       (c->values[SXB_P_CHECKED] ? SXGUI_MENU_CHECKED : 0);
            state->item_control[slot] = k;
            cs->menu_slot = slot;
            if (menu >= 0) state->menus[menu].item_count++;
            has_widget = 0;
            break;
        }
        default:
            has_widget = 0; /* Timer */
            break;
        }
        if (!has_widget) continue;
        w.on_action = on_action;
        w.user = (void *)(intptr_t)((form << 16) | k);
        if (c->values[SXB_P_VISIBLE]) w.flags |= SXGUI_FLAG_VISIBLE;
        else w.flags &= ~(uint32_t)SXGUI_FLAG_VISIBLE;
        if (!c->values[SXB_P_ENABLED]) w.flags |= SXGUI_FLAG_DISABLED;
        cs->widget = state->widget_count;
        state->widgets[state->widget_count++] = w;
    }
    state->bar.menus = state->menus;
    state->bar.menu_count = state->menu_count;
    state->bar.open_menu = -1;
    state->bar.hot_item = -1;
    state->bar.on_command = on_menu;
    snprintf(state->title, sizeof(state->title), "%s", def->form.text);
    state->focus = -1;
    return 0;
}

static void free_state(struct form_state *state, int control_count)
{
    if (state->controls) {
        for (int k = 0; k < control_count; k++) {
            free(state->controls[k].text);
            for (int i = 0; i < state->controls[k].item_count; i++) free(state->controls[k].items[i]);
            free(state->controls[k].items);
        }
    }
    free(state->controls);
    free(state->widgets);
    free(state->menus);
    free(state->items);
    free(state->item_control);
    memset(state, 0, sizeof(*state));
}

static int load_form(struct sxb_forms *forms, int form)
{
    struct form_state *state = &forms->forms[form];
    if (state->loaded) return 0;
    int error = build_widgets(forms, form);
    if (error) {
        free_state(state, forms->prog->forms[form].control_count);
        return error;
    }
    state->loaded = 1;
    unsigned long now = forms->window.now_ms(forms->window.user);
    for (int k = 0; k < forms->prog->forms[form].control_count; k++) state->controls[k].due = now;
    invoke(forms, sxb_program_form_init(forms->prog, form), NULL);
    invoke(forms, forms->prog->forms[form].form.handlers[SXB_EV_LOAD], NULL);
    return 0;
}

static int widget_index_of(struct form_state *state, int control)
{
    return control >= 0 ? state->controls[control].widget : -1;
}

/* Ends a dialog form, and wakes the procedure waiting in its ShowModal. */
static void close_modal(struct sxb_forms *forms, int form)
{
    struct form_state *state = &forms->forms[form];
    if (forms->modal_form != form) return;
    forms->modal_form = -1;
    state->shown = 0;
    if (forms->ctx && sxgui_dialog_active(forms->ctx) && forms->ctx->modal == &state->dialog) {
        sxgui_dialog_end(forms->ctx, 0);
    }
    forms->resume_pending = 1;
    forms->resume_value.type = 0;
    repaint(forms);
}

static int unload_form(struct sxb_forms *forms, int form)
{
    struct form_state *state = &forms->forms[form];
    if (!state->loaded) return 0;
    int cancel = 0;
    invoke(forms, forms->prog->forms[form].form.handlers[SXB_EV_UNLOAD], &cancel);
    if (cancel || forms->ended) return 0;
    if (forms->modal_form == form) close_modal(forms, form);
    free_state(state, forms->prog->forms[form].control_count);
    sxb_vm_reset_form(forms->vm, form);
    if (form == forms->main_form) {
        forms->main_form = -1;
        end_program(forms, 0);
    }
    return 0;
}

static int show_form(struct sxb_forms *forms, int form, int modal)
{
    int error = load_form(forms, form);
    if (error || forms->ended) return error;
    struct form_state *state = &forms->forms[form];
    const struct sxb_control_def *def = &forms->prog->forms[form].form;
    int width = (int)def->values[SXB_P_WIDTH], height = (int)def->values[SXB_P_HEIGHT];
    if (forms->main_form < 0 && !modal) {
        forms->menu_offset = state->menu_count ? sxgui_menubar_height() : 0;
        for (int i = 0; i < state->widget_count; i++) state->widgets[i].rect.y += forms->menu_offset;
        forms->ctx = forms->window.open(forms->window.user, state->title, width, height + forms->menu_offset,
                                        state->widgets, state->widget_count);
        if (forms->ctx == NULL) return SXB_ERR_OUT_OF_MEMORY;
        if (state->menu_count) sxgui_set_menubar(forms->ctx, &state->bar);
        forms->main_form = form;
        state->shown = 1;
        if (widget_index_of(state, state->focus) >= 0) sxgui_focus(forms->ctx, widget_index_of(state, state->focus));
        repaint(forms);
        return 0;
    }
    if (form == forms->main_form) return 0;
    if (!modal) {
        return SXB_ERR_INVALID_ARGUMENT; /* a second form shows with ShowModal in this version */
    }
    if (forms->ctx == NULL || forms->modal_form >= 0 || forms->message_active) return SXB_ERR_INVALID_ARGUMENT;
    memset(&state->dialog, 0, sizeof(state->dialog));
    state->dialog.title = state->title;
    state->dialog.widgets = state->widgets;
    state->dialog.widget_count = state->widget_count;
    state->dialog.initial_focus = widget_index_of(state, state->focus) > 0 ? widget_index_of(state, state->focus) : 0;
    for (int k = 0; k < forms->prog->forms[form].control_count; k++) {
        const struct sxb_control_def *c = &forms->prog->forms[form].controls[k];
        if (c->kind == SXB_CTL_BUTTON && c->values[SXB_P_DEFAULT] && state->controls[k].widget > 0) {
            state->dialog.default_button = state->controls[k].widget;
        }
    }
    sxgui_dialog_begin(forms->ctx, &state->dialog, width, height);
    forms->window.watch_dialog(forms->window.user, &state->dialog);
    forms->modal_form = form;
    state->shown = 1;
    repaint(forms);
    return SXB_HOST_SUSPEND;
}

/* ---- the message box and the input box ---------------------------------------------- */

static void message_button(struct sxgui_widget *widget, void *user)
{
    struct sxb_forms *forms = g_forms;
    (void)widget;
    if (forms == NULL || !forms->message_active) return;
    forms->message_result = (int)(intptr_t)user;
    if (forms->ctx && sxgui_dialog_active(forms->ctx)) sxgui_dialog_end(forms->ctx, forms->message_result);
}

static int open_message(struct sxb_forms *forms, int input, const char *text, int style, const char *title,
                        const char *initial)
{
    /* Over a ShowModal form, the message box takes the toolkit's one dialog
     * for a while; the form's dialog comes back when it closes. */
    if (forms->ctx == NULL || forms->message_active) return SXB_ERR_INVALID_ARGUMENT;
    memset(forms->message_widgets, 0, sizeof(forms->message_widgets));
    snprintf(forms->message_text, sizeof(forms->message_text), "%s", text);
    snprintf(forms->message_title, sizeof(forms->message_title), "%s", title);
    int lines = 0, width = 240;
    for (const char *p = text; lines < MESSAGE_LINES;) {
        const char *nl = strchr(p, '\n');
        int n = nl ? (int)(nl - p) : (int)strlen(p);
        snprintf(forms->message_lines[lines], sizeof(forms->message_lines[0]), "%.*s", n, p);
        if (n * 7 + 32 > width) width = n * 7 + 32;
        lines++;
        if (!nl) break;
        p = nl + 1;
    }
    if (width > 560) width = 560;
    int count = 0, y = SXGUI_DIALOG_MARGIN;
    for (int i = 0; i < lines; i++, y += 18) {
        forms->message_widgets[count++] = sxgui_label(sx_rect_make(12, y, width - 24, 18), forms->message_lines[i]);
    }
    y += 8;
    int field = -1;
    if (input) {
        snprintf(forms->input_buffer, sizeof(forms->input_buffer), "%s", initial ? initial : "");
        field = count;
        forms->message_widgets[count++] = sxgui_textfield(sx_rect_make(12, y, width - 24, SXGUI_FIELD_HEIGHT),
                                                          forms->input_buffer, (int)sizeof(forms->input_buffer));
        y += SXGUI_FIELD_HEIGHT + 10;
    }
    static const char *const names[] = {"OK", "Cancel", "Yes", "No"};
    int results[4], buttons = 0, buttons_style = input ? 1 : (style & 7);
    switch (buttons_style) {
    case 1: results[0] = 1; results[1] = 2; buttons = 2; break;
    case 3: results[0] = 6; results[1] = 7; results[2] = 2; buttons = 3; break;
    case 4: results[0] = 6; results[1] = 7; buttons = 2; break;
    default: results[0] = 1; buttons = 1; break;
    }
    int x = width - buttons * (SXGUI_BUTTON_WIDTH + SXGUI_GAP) - 6;
    forms->message_button_first = count;
    for (int i = 0; i < buttons; i++) {
        const char *name = results[i] == 1 ? names[0] : results[i] == 2 ? names[1] : results[i] == 6 ? names[2] : names[3];
        forms->message_results[i] = results[i];
        forms->message_widgets[count++] = sxgui_button(sx_rect_make(x, y, SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
                                                       name, message_button, (void *)(intptr_t)results[i]);
        x += SXGUI_BUTTON_WIDTH + SXGUI_GAP;
    }
    y += SXGUI_BUTTON_HEIGHT + SXGUI_DIALOG_MARGIN;
    memset(&forms->message_dialog, 0, sizeof(forms->message_dialog));
    forms->message_dialog.title = forms->message_title;
    forms->message_dialog.widgets = forms->message_widgets;
    forms->message_dialog.widget_count = count;
    forms->message_dialog.default_button = forms->message_button_first;
    forms->message_dialog.initial_focus = field > 0 ? field : forms->message_button_first;
    forms->message_active = 1;
    forms->message_input = input;
    forms->message_result = 2; /* closed without a button: Cancel */
    sxgui_dialog_begin(forms->ctx, &forms->message_dialog, width, y);
    forms->window.watch_dialog(forms->window.user, &forms->message_dialog);
    repaint(forms);
    return SXB_HOST_SUSPEND;
}

const char *sxb_forms_message_text(const struct sxb_forms *forms)
{
    return forms->message_active ? forms->message_text : NULL;
}

int sxb_forms_answer(struct sxb_forms *forms, int result, const char *text)
{
    if (!forms->message_active) return -1;
    if (text && forms->message_input) snprintf(forms->input_buffer, sizeof(forms->input_buffer), "%s", text);
    for (int i = 0; i < 4; i++) {
        if (forms->message_results[i] == result) {
            struct sxgui_widget *button = &forms->message_widgets[forms->message_button_first + i];
            button->action = SXGUI_ACTION_CLICK;
            button->on_action(button, button->user);
            sxb_forms_dialog_closed(forms, &forms->message_dialog);
            return 0;
        }
    }
    return -1;
}

/* ---- the VM's side ------------------------------------------------------------------- */

static int host_load(void *user, int form)
{
    return load_form(user, form);
}

static int host_unload(void *user, int form)
{
    return unload_form(user, form);
}

static int list_get(struct control_state *cs, int64_t index, struct sxb_scalar *out)
{
    if (index < 0 || index >= cs->item_count) return SXB_ERR_INDEX;
    out->type = SXB_TY_STRING;
    out->s = cs->items[index];
    out->length = strlen(out->s);
    return 0;
}

static int host_get(void *user, int form, int control, int property, struct sxb_scalar *out)
{
    struct sxb_forms *forms = user;
    int error = load_form(forms, form);
    if (error) return error;
    struct form_state *state = &forms->forms[form];
    const struct sxb_control_def *def = control_def(forms, form, control);
    memset(out, 0, sizeof(*out));
    out->type = sxb_property_type(property, def->kind);
    if (control < 0) {
        if (property == SXB_P_TEXT) {
            out->s = state->title;
            out->length = strlen(state->title);
        } else {
            out->i = def->values[property];
        }
        return 0;
    }
    struct control_state *cs = &state->controls[control];
    struct sxgui_widget *w = widget_of(forms, form, control);
    switch (property) {
    case SXB_P_TEXT:
        if (def->kind == SXB_CTL_COMBOBOX) {
            if (w && cs->item_count > 0) return list_get(cs, w->value, out);
            out->s = "";
            return 0;
        }
        out->s = cs->text;
        out->length = strlen(cs->text);
        return 0;
    case SXB_P_LEFT: out->i = w->rect.x; return 0;
    case SXB_P_TOP: out->i = w->rect.y - (form == forms->main_form ? forms->menu_offset : 0); return 0;
    case SXB_P_WIDTH: out->i = w->rect.width; return 0;
    case SXB_P_HEIGHT: out->i = w->rect.height; return 0;
    case SXB_P_VISIBLE: out->i = (w->flags & SXGUI_FLAG_VISIBLE) != 0; return 0;
    case SXB_P_ENABLED: out->i = w ? !(w->flags & SXGUI_FLAG_DISABLED) : cs->enabled; return 0;
    case SXB_P_VALUE: out->i = w->value; return 0;
    case SXB_P_MIN: out->i = w->range_min; return 0;
    case SXB_P_MAX: out->i = w->range_max; return 0;
    case SXB_P_LARGECHANGE: out->i = w->page; return 0;
    case SXB_P_LISTCOUNT: out->i = cs->item_count; return 0;
    case SXB_P_LISTINDEX: out->i = cs->item_count == 0 ? -1 : w->value; return 0;
    case SXB_P_INTERVAL: out->i = cs->interval; return 0;
    case SXB_P_CHECKED: out->i = (state->items[cs->menu_slot].flags & SXGUI_MENU_CHECKED) != 0; return 0;
    default: return SXB_ERR_PROPERTY;
    }
}

static int host_set(void *user, int form, int control, int property, const struct sxb_scalar *value)
{
    struct sxb_forms *forms = user;
    int error = load_form(forms, form);
    if (error || forms->ended) return error;
    struct form_state *state = &forms->forms[form];
    const struct sxb_control_def *def = control_def(forms, form, control);
    if (control < 0) {
        snprintf(state->title, sizeof(state->title), "%.*s", (int)value->length, value->s);
        if (form == forms->main_form && forms->window.set_title) forms->window.set_title(forms->window.user, state->title);
        repaint(forms);
        return 0;
    }
    struct control_state *cs = &state->controls[control];
    struct sxgui_widget *w = widget_of(forms, form, control);
    int64_t v = value->i;
    switch (property) {
    case SXB_P_TEXT:
        if ((int)value->length >= cs->capacity) return SXB_ERR_PROPERTY;
        memcpy(cs->text, value->s, value->length);
        cs->text[value->length] = '\0';
        if (def->kind == SXB_CTL_TEXTBOX && w) {
            w->caret = (int)value->length;
            if (def->values[SXB_P_MULTILINE]) w->value = -1;
        }
        repaint(forms);
        if (def->kind == SXB_CTL_TEXTBOX) sxb_forms_fire(forms, form, control, SXB_EV_CHANGE);
        return 0;
    case SXB_P_LEFT: w->rect.x = (int)v; break;
    case SXB_P_TOP: w->rect.y = (int)v + (form == forms->main_form ? forms->menu_offset : 0); break;
    case SXB_P_WIDTH:
    case SXB_P_HEIGHT:
        if (v < 0 || v > 4096) return SXB_ERR_PROPERTY;
        if (property == SXB_P_WIDTH) w->rect.width = (int)v;
        else w->rect.height = (int)v;
        break;
    case SXB_P_VISIBLE:
        if (v) w->flags |= SXGUI_FLAG_VISIBLE;
        else w->flags &= ~(uint32_t)SXGUI_FLAG_VISIBLE;
        break;
    case SXB_P_ENABLED:
        cs->enabled = v != 0;
        if (w) {
            if (v) w->flags &= ~(uint32_t)SXGUI_FLAG_DISABLED;
            else w->flags |= SXGUI_FLAG_DISABLED;
        } else if (cs->menu_slot >= 0) {
            if (v) state->items[cs->menu_slot].flags &= ~(uint32_t)SXGUI_MENU_DISABLED;
            else state->items[cs->menu_slot].flags |= SXGUI_MENU_DISABLED;
        } else {
            cs->due = forms->window.now_ms(forms->window.user) + (unsigned long)cs->interval;
        }
        break;
    case SXB_P_VALUE:
        if (def->kind == SXB_CTL_CHECKBOX || def->kind == SXB_CTL_RADIO) {
            w->value = v != 0;
            if (def->kind == SXB_CTL_RADIO && v) {
                for (int i = 0; i < state->widget_count; i++) {
                    struct sxgui_widget *other = &state->widgets[i];
                    if (other != w && other->kind == SXGUI_RADIO && other->group == w->group) other->value = 0;
                }
            }
        } else {
            if (v < w->range_min || v > w->range_max) return SXB_ERR_PROPERTY;
            w->value = (int)v;
            if (def->kind != SXB_CTL_PROGRESS) {
                repaint(forms);
                sxb_forms_fire(forms, form, control, SXB_EV_CHANGE);
                return 0;
            }
        }
        break;
    case SXB_P_MIN:
    case SXB_P_MAX:
        if (property == SXB_P_MIN) w->range_min = (int)v;
        else w->range_max = (int)v;
        if (w->range_max < w->range_min) w->range_max = w->range_min;
        if (w->value < w->range_min) w->value = w->range_min;
        if (w->value > w->range_max) w->value = w->range_max;
        break;
    case SXB_P_LARGECHANGE:
        if (v < 1) return SXB_ERR_PROPERTY;
        w->page = (int)v;
        break;
    case SXB_P_LISTINDEX:
        if (v < -1 || v >= cs->item_count || (v == -1 && def->kind == SXB_CTL_COMBOBOX && cs->item_count > 0)) {
            return SXB_ERR_PROPERTY;
        }
        w->value = (int)v;
        repaint(forms);
        sxb_forms_fire(forms, form, control, SXB_EV_CLICK);
        return 0;
    case SXB_P_INTERVAL:
        if (v < 0) return SXB_ERR_PROPERTY;
        cs->interval = (int)v;
        cs->due = forms->window.now_ms(forms->window.user) + (unsigned long)v;
        break;
    case SXB_P_CHECKED:
        if (v) state->items[cs->menu_slot].flags |= SXGUI_MENU_CHECKED;
        else state->items[cs->menu_slot].flags &= ~(uint32_t)SXGUI_MENU_CHECKED;
        break;
    default:
        return SXB_ERR_PROPERTY;
    }
    repaint(forms);
    return 0;
}

static int host_call(void *user, int form, int control, int method, const struct sxb_scalar *args, int argc,
                     struct sxb_scalar *result)
{
    struct sxb_forms *forms = user;
    (void)argc;
    if (method == SXB_M_SHOW || method == SXB_M_SHOWMODAL) return show_form(forms, form, method == SXB_M_SHOWMODAL);
    int error = load_form(forms, form);
    if (error || forms->ended) return error;
    struct form_state *state = &forms->forms[form];
    if (method == SXB_M_HIDE) {
        if (forms->modal_form == form) close_modal(forms, form);
        else if (form == forms->main_form) return SXB_ERR_INVALID_ARGUMENT; /* the main window does not hide */
        return 0;
    }
    struct control_state *cs = &state->controls[control];
    struct sxgui_widget *w = widget_of(forms, form, control);
    switch (method) {
    case SXB_M_SETFOCUS:
        state->focus = control;
        if (form == forms->main_form && forms->ctx && w) sxgui_focus(forms->ctx, cs->widget);
        return 0;
    case SXB_M_ADDITEM: {
        if (cs->item_count == cs->item_capacity) {
            int capacity = cs->item_capacity ? cs->item_capacity * 2 : 16;
            char **grown = realloc(cs->items, sizeof(*grown) * (size_t)capacity);
            if (grown == NULL) return SXB_ERR_OUT_OF_MEMORY;
            cs->items = grown;
            cs->item_capacity = capacity;
        }
        char *item = malloc(args[0].length + 1u);
        if (item == NULL) return SXB_ERR_OUT_OF_MEMORY;
        memcpy(item, args[0].s, args[0].length);
        item[args[0].length] = '\0';
        cs->items[cs->item_count++] = item;
        sync_items(cs, w);
        repaint(forms);
        return 0;
    }
    case SXB_M_REMOVEITEM: {
        int64_t index = args[0].i;
        if (index < 0 || index >= cs->item_count) return SXB_ERR_INDEX;
        free(cs->items[index]);
        memmove(&cs->items[index], &cs->items[index + 1], sizeof(*cs->items) * (size_t)(cs->item_count - index - 1));
        cs->item_count--;
        if (w->value >= cs->item_count) w->value = cs->item_count - 1;
        sync_items(cs, w);
        repaint(forms);
        return 0;
    }
    case SXB_M_CLEAR:
        for (int i = 0; i < cs->item_count; i++) free(cs->items[i]);
        cs->item_count = 0;
        w->value = control_def(forms, form, control)->kind == SXB_CTL_COMBOBOX ? 0 : -1;
        w->scroll = 0;
        sync_items(cs, w);
        repaint(forms);
        return 0;
    case SXB_M_LIST:
        return list_get(cs, args[0].i, result);
    default:
        return SXB_ERR_INVALID_ARGUMENT;
    }
}

static int host_message(void *user, const char *text, int style, const char *title)
{
    return open_message(user, 0, text, style, title, NULL);
}

static int host_input(void *user, const char *prompt, const char *title, const char *initial)
{
    return open_message(user, 1, prompt, 1, title, initial);
}

/* ---- the frame's side ---------------------------------------------------------------- */

struct sxb_forms *sxb_forms_create(const struct sxb_program *program, struct sxb_vm *vm,
                                   const struct sxb_window *window)
{
    struct sxb_forms *forms = calloc(1, sizeof(*forms));
    if (forms == NULL) return NULL;
    forms->forms = calloc((size_t)program->form_count + 1u, sizeof(*forms->forms));
    if (forms->forms == NULL) {
        free(forms);
        return NULL;
    }
    forms->prog = program;
    forms->vm = vm;
    forms->window = *window;
    forms->main_form = -1;
    forms->modal_form = -1;
    static struct sxb_form_host host;
    host.user = forms;
    host.load = host_load;
    host.unload = host_unload;
    host.get = host_get;
    host.set = host_set;
    host.call = host_call;
    host.message = host_message;
    host.input = host_input;
    sxb_vm_set_form_host(vm, &host);
    g_forms = forms;
    return forms;
}

void sxb_forms_destroy(struct sxb_forms *forms)
{
    if (forms == NULL) return;
    for (int f = 0; f < forms->prog->form_count; f++) free_state(&forms->forms[f], forms->prog->forms[f].control_count);
    free(forms->forms);
    if (g_forms == forms) g_forms = NULL;
    free(forms);
}

void sxb_forms_start(struct sxb_forms *forms)
{
    enum sxb_status status = sxb_vm_run(forms->vm, 0);
    after_run(forms, status);
    if (!forms->ended && status == SXB_IDLE && forms->main_form < 0 && forms->modal_form < 0 &&
        !forms->message_active) {
        end_program(forms, 0); /* Sub Main returned and nothing is on screen */
    }
}

static void settle(struct sxb_forms *forms)
{
    while (forms->resume_pending && !forms->ended && sxb_vm_waiting(forms->vm) > 0) {
        forms->resume_pending = 0;
        struct sxb_scalar value = forms->resume_value;
        after_run(forms, sxb_vm_resume(forms->vm, value.type ? &value : NULL));
    }
    forms->resume_pending = 0;
}

void sxb_forms_dialog_closed(struct sxb_forms *forms, struct sxgui_dialog *dialog)
{
    if (dialog == &forms->message_dialog && forms->message_active) {
        forms->message_active = 0;
        if (forms->modal_form >= 0 && forms->ctx) {
            struct form_state *modal = &forms->forms[forms->modal_form];
            const struct sxb_control_def *def = &forms->prog->forms[forms->modal_form].form;
            sxgui_dialog_begin(forms->ctx, &modal->dialog, (int)def->values[SXB_P_WIDTH], (int)def->values[SXB_P_HEIGHT]);
            forms->window.watch_dialog(forms->window.user, &modal->dialog);
        }
        forms->resume_pending = 1;
        memset(&forms->resume_value, 0, sizeof(forms->resume_value));
        if (forms->message_input) {
            snprintf(forms->resume_text, sizeof(forms->resume_text), "%s",
                     forms->message_result == 1 ? forms->input_buffer : "");
            forms->resume_value.type = SXB_TY_STRING;
            forms->resume_value.s = forms->resume_text;
            forms->resume_value.length = strlen(forms->resume_text);
        } else {
            forms->resume_value.type = SXB_TY_INT;
            forms->resume_value.i = forms->message_result;
        }
    } else if (forms->modal_form >= 0 && dialog == &forms->forms[forms->modal_form].dialog && !forms->message_active) {
        /* Closed from its frame (Esc, its close button): its Cancel button
         * answers, as Esc would press it; without one, the form unloads. */
        int form = forms->modal_form;
        const struct sxb_form_def *def = &forms->prog->forms[form];
        int cancel = -1;
        for (int k = 0; k < def->control_count; k++) {
            if (def->controls[k].kind == SXB_CTL_BUTTON && def->controls[k].values[SXB_P_CANCEL]) cancel = k;
        }
        forms->forms[form].shown = 0;
        forms->modal_form = -1;
        forms->resume_pending = 1;
        forms->resume_value.type = 0;
        if (cancel >= 0) sxb_forms_fire(forms, form, cancel, SXB_EV_CLICK);
        else unload_form(forms, form);
    }
    settle(forms);
}

void sxb_forms_tick(struct sxb_forms *forms)
{
    if (forms->ended) return;
    settle(forms);
    if (sxb_vm_waiting(forms->vm) > 0 && forms->modal_form < 0 && !forms->message_active) return;
    unsigned long now = forms->window.now_ms(forms->window.user);
    for (int f = 0; f < forms->prog->form_count && !forms->ended; f++) {
        if (!forms->forms[f].loaded) continue;
        const struct sxb_form_def *def = &forms->prog->forms[f];
        for (int k = 0; k < def->control_count && !forms->ended; k++) {
            if (def->controls[k].kind != SXB_CTL_TIMER || !forms->forms[f].loaded) continue;
            struct control_state *cs = &forms->forms[f].controls[k];
            if (!cs->enabled || cs->interval <= 0 || (long)(now - cs->due) < 0) continue;
            cs->due = now + (unsigned long)cs->interval;
            sxb_forms_fire(forms, f, k, SXB_EV_TICK);
        }
    }
    settle(forms);
}

int sxb_forms_close_main(struct sxb_forms *forms)
{
    if (forms->main_form < 0) return 1;
    unload_form(forms, forms->main_form);
    return forms->ended;
}

int sxb_forms_find(struct sxb_forms *forms, const char *path, int *form, int *control)
{
    const char *dot = strchr(path, '.');
    int name_length = dot ? (int)(dot - path) : (int)strlen(path);
    for (int f = 0; f < forms->prog->form_count; f++) {
        const struct sxb_form_def *def = &forms->prog->forms[f];
        if (!sxb_names_equal(path, name_length, def->name, (int)strlen(def->name))) continue;
        *form = f;
        *control = -1;
        if (!dot) return 1;
        for (int k = 0; k < def->control_count; k++) {
            if (sxb_names_equal(dot + 1, (int)strlen(dot + 1), def->controls[k].name, (int)strlen(def->controls[k].name))) {
                *control = k;
                return 1;
            }
        }
        return 0;
    }
    return 0;
}

struct sxgui_widget *sxb_forms_widget(struct sxb_forms *forms, const char *path)
{
    int form, control;
    if (!sxb_forms_find(forms, path, &form, &control) || control < 0) return NULL;
    return widget_of(forms, form, control);
}

int sxb_forms_property_text(struct sxb_forms *forms, int form, int control, int property, char *out, size_t size)
{
    struct sxb_scalar value;
    int error = host_get(forms, form, control, property, &value);
    if (error) return error;
    switch (value.type) {
    case SXB_TY_STRING: snprintf(out, size, "%.*s", (int)value.length, value.s); break;
    case SXB_TY_BOOL: snprintf(out, size, "%s", value.i ? "True" : "False"); break;
    default: snprintf(out, size, "%lld", (long long)value.i); break;
    }
    return 0;
}
