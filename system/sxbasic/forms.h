/* SxBASIC forms runtime: a program's forms as SXGUI widgets
 * (docs/userland/SXBASIC.md#forms). It makes no system call: the window it
 * draws in comes through struct sxb_window, which the SUN frame provides
 * (sxbasic.c) and so does the windowless selftest. */
#ifndef SXBASIC_FORMS_H
#define SXBASIC_FORMS_H

#include "core/sxbasic.h"
#include "sxgfx/sxgui.h"

struct sxb_window {
    void *user;
    /* Opens the main window with a client area of width x height bound to
     * `widgets`; returns its toolkit context, or NULL. */
    struct sxgui_context *(*open)(void *user, const char *title, int width, int height,
                                  struct sxgui_widget *widgets, int count);
    void (*set_title)(void *user, const char *title);
    void (*repaint)(void *user);
    /* Tells the frame to call sxb_forms_dialog_closed once `dialog` closes. */
    void (*watch_dialog)(void *user, struct sxgui_dialog *dialog);
    /* The last form unloaded: the program ends. */
    void (*quit)(void *user, int exit_code);
    /* An uncaught runtime error: shown, then the program ends. */
    void (*fail)(void *user, const char *message);
    unsigned long (*now_ms)(void *user);
};

struct sxb_forms;

struct sxb_forms *sxb_forms_create(const struct sxb_program *program, struct sxb_vm *vm,
                                   const struct sxb_window *window);
void sxb_forms_destroy(struct sxb_forms *forms);

/* Starts the program: runs its start until it waits for events. */
void sxb_forms_start(struct sxb_forms *forms);

/* Fires timers that are due and resumes a procedure whose dialog closed.
 * The frame calls it on every tick, outside the toolkit's event handling. */
void sxb_forms_tick(struct sxb_forms *forms);

/* The frame's dialog watch: the dialog the forms runtime gave it closed. */
void sxb_forms_dialog_closed(struct sxb_forms *forms, struct sxgui_dialog *dialog);

/* The main window's close button: unloads the main form, unless its
 * Form_Unload cancels. 1 when it unloaded. */
int sxb_forms_close_main(struct sxb_forms *forms);

/* For the selftest: a control by "Form.Control" (or "Form"), its widget,
 * and the message box on screen. */
struct sxgui_widget *sxb_forms_widget(struct sxb_forms *forms, const char *path);
int sxb_forms_find(struct sxb_forms *forms, const char *path, int *form, int *control);
const char *sxb_forms_message_text(const struct sxb_forms *forms);
/* Presses the message box's button for this result (mbResultOk...). */
int sxb_forms_answer(struct sxb_forms *forms, int result, const char *text);
/* Fires a timer's Tick or a menu item's Click as the frame would. */
void sxb_forms_fire(struct sxb_forms *forms, int form, int control, int event);
/* A property's value as text, the way CStr writes it. */
int sxb_forms_property_text(struct sxb_forms *forms, int form, int control, int property, char *out, size_t size);

#endif
