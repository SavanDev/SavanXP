/* SxBASIC core: what each control has (docs/userland/SXBASIC.md#controls),
 * shared by the compiler, which checks a program against it, and the forms
 * runtime, which follows it. */
#include "sxb_internal.h"

#include <string.h>

#define K(kind) (1u << (kind))
#define VISUAL (K(SXB_CTL_LABEL) | K(SXB_CTL_BUTTON) | K(SXB_CTL_CHECKBOX) | K(SXB_CTL_RADIO) | K(SXB_CTL_TEXTBOX) | \
                K(SXB_CTL_LISTBOX) | K(SXB_CTL_COMBOBOX) | K(SXB_CTL_GROUPBOX) | K(SXB_CTL_SCROLLBAR) |          \
                K(SXB_CTL_TRACKBAR) | K(SXB_CTL_PROGRESS))
#define FOCUSABLE (K(SXB_CTL_BUTTON) | K(SXB_CTL_CHECKBOX) | K(SXB_CTL_RADIO) | K(SXB_CTL_TEXTBOX) |               \
                   K(SXB_CTL_LISTBOX) | K(SXB_CTL_COMBOBOX) | K(SXB_CTL_SCROLLBAR) | K(SXB_CTL_TRACKBAR))
#define LISTS (K(SXB_CTL_LISTBOX) | K(SXB_CTL_COMBOBOX))
#define RANGES (K(SXB_CTL_SCROLLBAR) | K(SXB_CTL_TRACKBAR) | K(SXB_CTL_PROGRESS))

static const char *const kind_names[SXB_CTL_COUNT] = {
    "Form", "Label", "Button", "CheckBox", "RadioButton", "TextBox", "ListBox", "ComboBox",
    "GroupBox", "ScrollBar", "TrackBar", "ProgressBar", "Timer", "Menu", "MenuItem"};

struct property_def {
    const char *name;
    int type;
    uint32_t kinds;      /* where it is read and written at run time */
    uint32_t read_only;  /* of those, where it is only read */
    uint32_t file_only;  /* where the form file sets it and code does not see it */
};

static const struct property_def properties[SXB_P_COUNT] = {
    [SXB_P_TEXT] = {"Text", SXB_TY_STRING,
                    K(SXB_CTL_FORM) | K(SXB_CTL_LABEL) | K(SXB_CTL_BUTTON) | K(SXB_CTL_CHECKBOX) | K(SXB_CTL_RADIO) |
                        K(SXB_CTL_TEXTBOX) | K(SXB_CTL_GROUPBOX) | K(SXB_CTL_COMBOBOX) | K(SXB_CTL_MENU) |
                        K(SXB_CTL_MENUITEM),
                    K(SXB_CTL_COMBOBOX), 0},
    [SXB_P_LEFT] = {"Left", SXB_TY_INT, VISUAL, 0, 0},
    [SXB_P_TOP] = {"Top", SXB_TY_INT, VISUAL, 0, 0},
    [SXB_P_WIDTH] = {"Width", SXB_TY_INT, VISUAL | K(SXB_CTL_FORM), K(SXB_CTL_FORM), 0},
    [SXB_P_HEIGHT] = {"Height", SXB_TY_INT, VISUAL | K(SXB_CTL_FORM), K(SXB_CTL_FORM), 0},
    [SXB_P_VISIBLE] = {"Visible", SXB_TY_BOOL, VISUAL, 0, 0},
    [SXB_P_ENABLED] = {"Enabled", SXB_TY_BOOL, VISUAL | K(SXB_CTL_TIMER) | K(SXB_CTL_MENUITEM), 0, 0},
    [SXB_P_VALUE] = {"Value", SXB_TY_INT, RANGES | K(SXB_CTL_CHECKBOX) | K(SXB_CTL_RADIO), 0, 0},
    [SXB_P_MIN] = {"Min", SXB_TY_INT, RANGES, 0, 0},
    [SXB_P_MAX] = {"Max", SXB_TY_INT, RANGES, 0, 0},
    [SXB_P_LARGECHANGE] = {"LargeChange", SXB_TY_INT, K(SXB_CTL_SCROLLBAR), 0, 0},
    [SXB_P_LISTCOUNT] = {"ListCount", SXB_TY_INT, LISTS, LISTS, 0},
    [SXB_P_LISTINDEX] = {"ListIndex", SXB_TY_INT, LISTS, 0, 0},
    [SXB_P_INTERVAL] = {"Interval", SXB_TY_INT, K(SXB_CTL_TIMER), 0, 0},
    [SXB_P_CHECKED] = {"Checked", SXB_TY_BOOL, K(SXB_CTL_MENUITEM), 0, 0},
    [SXB_P_GROUP] = {"Group", SXB_TY_INT, 0, 0, K(SXB_CTL_RADIO)},
    [SXB_P_MULTILINE] = {"MultiLine", SXB_TY_BOOL, 0, 0, K(SXB_CTL_TEXTBOX)},
    [SXB_P_HORIZONTAL] = {"Horizontal", SXB_TY_BOOL, 0, 0, K(SXB_CTL_SCROLLBAR)},
    [SXB_P_DEFAULT] = {"Default", SXB_TY_BOOL, 0, 0, K(SXB_CTL_BUTTON)},
    [SXB_P_CANCEL] = {"Cancel", SXB_TY_BOOL, 0, 0, K(SXB_CTL_BUTTON)},
    [SXB_P_RESIZABLE] = {"Resizable", SXB_TY_BOOL, 0, 0, K(SXB_CTL_FORM)},
};

struct method_def {
    const char *name;
    uint32_t kinds;
    int arg;      /* the one argument's type, or 0 */
    int result;
};

static const struct method_def methods[SXB_M_COUNT] = {
    [SXB_M_SHOW] = {"Show", K(SXB_CTL_FORM), 0, 0},
    [SXB_M_SHOWMODAL] = {"ShowModal", K(SXB_CTL_FORM), 0, 0},
    [SXB_M_HIDE] = {"Hide", K(SXB_CTL_FORM), 0, 0},
    [SXB_M_SETFOCUS] = {"SetFocus", FOCUSABLE, 0, 0},
    [SXB_M_ADDITEM] = {"AddItem", LISTS, SXB_TY_STRING, 0},
    [SXB_M_REMOVEITEM] = {"RemoveItem", LISTS, SXB_TY_INT, 0},
    [SXB_M_CLEAR] = {"Clear", LISTS, 0, 0},
    [SXB_M_LIST] = {"List", LISTS, SXB_TY_INT, SXB_TY_STRING},
};

static const struct {
    const char *name;
    uint32_t kinds;
} events[SXB_EV_COUNT] = {
    [SXB_EV_CLICK] = {"Click", K(SXB_CTL_BUTTON) | K(SXB_CTL_CHECKBOX) | K(SXB_CTL_RADIO) | LISTS | K(SXB_CTL_MENUITEM)},
    [SXB_EV_DBLCLICK] = {"DblClick", K(SXB_CTL_LISTBOX)},
    [SXB_EV_CHANGE] = {"Change", K(SXB_CTL_TEXTBOX) | K(SXB_CTL_SCROLLBAR) | K(SXB_CTL_TRACKBAR)},
    [SXB_EV_TICK] = {"Tick", K(SXB_CTL_TIMER)},
    [SXB_EV_LOAD] = {"Load", K(SXB_CTL_FORM)},
    [SXB_EV_UNLOAD] = {"Unload", K(SXB_CTL_FORM)},
    [SXB_EV_RESIZE] = {"Resize", K(SXB_CTL_FORM)},
};

const char *sxb_control_kind_name(int kind)
{
    return kind >= 0 && kind < SXB_CTL_COUNT ? kind_names[kind] : "?";
}

const char *sxb_property_name(int property)
{
    return property >= 0 && property < SXB_P_COUNT ? properties[property].name : "?";
}

const char *sxb_method_name(int method)
{
    return method >= 0 && method < SXB_M_COUNT ? methods[method].name : "?";
}

const char *sxb_event_name(int event)
{
    return event >= 0 && event < SXB_EV_COUNT ? events[event].name : "?";
}

int sxb_find_control_kind(const char *name, int length)
{
    for (int k = 0; k < SXB_CTL_COUNT; k++) {
        if (sxb_name_equal(name, length, kind_names[k])) return k;
    }
    return -1;
}

int sxb_find_property(const char *name, int length)
{
    for (int p = 0; p < SXB_P_COUNT; p++) {
        if (sxb_name_equal(name, length, properties[p].name)) return p;
    }
    return -1;
}

int sxb_find_method(const char *name, int length)
{
    for (int m = 0; m < SXB_M_COUNT; m++) {
        if (sxb_name_equal(name, length, methods[m].name)) return m;
    }
    return -1;
}

int sxb_find_event(const char *name, int length)
{
    for (int e = 0; e < SXB_EV_COUNT; e++) {
        if (sxb_name_equal(name, length, events[e].name)) return e;
    }
    return -1;
}

int sxb_property_type(int property, int kind)
{
    if (property == SXB_P_VALUE && (kind == SXB_CTL_CHECKBOX || kind == SXB_CTL_RADIO)) return SXB_TY_BOOL;
    return properties[property].type;
}

int sxb_property_access(int property, int kind)
{
    const struct property_def *p = &properties[property];
    if (p->file_only & K(kind)) return SXB_ACCESS_FILE;
    if (!(p->kinds & K(kind))) return SXB_ACCESS_NONE;
    return (p->read_only & K(kind)) ? SXB_ACCESS_READ : SXB_ACCESS_WRITE;
}

int sxb_method_applies(int method, int kind)
{
    return (methods[method].kinds & K(kind)) != 0;
}

int sxb_method_arg(int method)
{
    return methods[method].arg;
}

int sxb_method_result(int method)
{
    return methods[method].result;
}

int sxb_event_applies(int event, int kind)
{
    return (events[event].kinds & K(kind)) != 0;
}

void sxb_control_defaults(int kind, struct sxb_control_def *control)
{
    memset(control->values, 0, sizeof(control->values));
    control->kind = kind;
    control->menu = -1;
    control->text[0] = '\0';
    for (int e = 0; e < SXB_EV_COUNT; e++) control->handlers[e] = -1;
    int64_t *v = control->values;
    v[SXB_P_VISIBLE] = 1;
    v[SXB_P_ENABLED] = 1;
    v[SXB_P_LISTINDEX] = -1;
    v[SXB_P_WIDTH] = 84;
    v[SXB_P_HEIGHT] = 24;
    switch (kind) {
    case SXB_CTL_FORM: v[SXB_P_WIDTH] = 320; v[SXB_P_HEIGHT] = 240; break;
    case SXB_CTL_BUTTON: v[SXB_P_HEIGHT] = 26; break;
    case SXB_CTL_LABEL: v[SXB_P_HEIGHT] = 20; break;
    case SXB_CTL_LISTBOX: v[SXB_P_WIDTH] = 120; v[SXB_P_HEIGHT] = 96; break;
    case SXB_CTL_GROUPBOX: v[SXB_P_WIDTH] = 160; v[SXB_P_HEIGHT] = 96; break;
    case SXB_CTL_SCROLLBAR: v[SXB_P_WIDTH] = 16; v[SXB_P_HEIGHT] = 96; v[SXB_P_LARGECHANGE] = 10; break;
    default: break;
    }
    if (kind == SXB_CTL_SCROLLBAR || kind == SXB_CTL_TRACKBAR || kind == SXB_CTL_PROGRESS) {
        v[SXB_P_MAX] = 100;
    }
}
