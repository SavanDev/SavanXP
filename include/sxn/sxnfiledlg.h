#pragma once

/* sxnfiledlg: the common Open and Save As dialogs for SUN programs
 * (docs/desktop/FILE_DIALOG.md), the counterpart of comdlg32's
 * GetOpenFileName and GetSaveFileName.
 *
 * The dialog lists a folder with the native calls (sxn_open_directory,
 * sxn_read_directory, sxn_query_path), shows each file's icon by type
 * (sxgfx/mime_icon.h) and hands back an absolute path. It does not open or
 * write the file: that stays the program's.
 *
 * The toolkit's dialogs do not block, so neither does this one: begin it, and
 * `on_done` is called once, with the path or with NULL when the person
 * cancelled. Only one dialog is up at a time, as in the toolkit; beginning
 * another one, or a message box, replaces it.
 *
 * Built from system/runtime/sxnfiledlg.c, linked into every native program;
 * the linker drops it from the ones that do not call it.
 */

#include "sxn/sxngui.h"

#ifdef __cplusplus
extern "C" {
#endif

enum sxn_file_dialog_mode {
    SXN_FILE_DIALOG_OPEN = 0,  /* pick an existing file */
    SXN_FILE_DIALOG_SAVE = 1,  /* name a file to write; asks before replacing one */
};

/* The longest path the dialog returns, with its NUL (vfs::kMaxPathLength). */
#define SXN_FILE_DIALOG_PATH_MAX 1024

struct sxn_file_dialog_request {
    int mode;                       /* enum sxn_file_dialog_mode */
    const char *title;              /* NULL: "Open" or "Save As" */
    /* The folder shown first. NULL, or one that cannot be listed: the folder
     * the last dialog of this process ended in, else "/". */
    const char *initial_directory;
    const char *file_name;          /* the name suggested in the field; NULL: empty */
    /* The files listed: "Log files (*.txt)" and "*.txt;*.log". NULL patterns
     * (or "*.*") list every file. Folders are always listed. One filter: the
     * toolkit has no combobox inside a dialog yet. */
    const char *filter_description;
    const char *filter_patterns;
    /* Save: added, after a dot, to a name typed without an extension. NULL:
     * nothing is added. */
    const char *default_extension;
    /* The absolute path chosen, or NULL when cancelled. The string lives
     * until the next dialog begins. */
    void (*on_done)(struct sxngui_app *app, const char *path, void *user);
    void *user;
};

/* Opens the dialog over `app`'s window. 0, or -1 when the request is
 * incomplete (no on_done) or no memory is left for the listing. */
int sxn_file_dialog_begin(struct sxngui_app *app, const struct sxn_file_dialog_request *request);

/* The folder and name logic, against the live file system: joining and
 * normalizing paths, matching patterns, and listing a known folder. Returns
 * how many checks failed and logs each one. */
int sxn_file_dialog_selftest(void);

#ifdef __cplusplus
}
#endif
