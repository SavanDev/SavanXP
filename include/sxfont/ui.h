/* The interface faces from /system/fonts (docs/desktop/UI_FONTS.md,
 * "Interface faces from files" and "Choosing the faces"): what a program's
 * runtime calls before it draws, to put the files' faces in the text
 * primitives' slots, and again when the user picks other faces.
 *
 * Compiled into the SUN and POSIX runtimes; each passes its own way of reading
 * a file. It refers to libsxfont weakly: a program that maps libsxfont gets
 * the faces from files, one that does not (every static program) keeps the
 * baked tables, and so does any program when a file is missing or damaged.
 *
 * The choice is the user's, in appearance.cfg in the profile's Settings
 * folder, one key=value per line:
 *
 *     body_font=/system/fonts/MSSansSerif.ttf
 *     body_size=13
 *     caption_font=/system/fonts/RetroSans.ttf
 *     caption_size=17
 *     smoothing=0
 *
 * Sizes are pixels per em. Without smoothing every face is drawn 1-bit,
 * whatever its gasp table asks; with it, each face is grey where its gasp
 * table asks, as Windows' standard font smoothing (UI_FONTS.md, "The rule:
 * every UI face is 1-bit"). No file, an unknown key or a bad value leaves
 * the default for that key. */
#pragma once

#include <stddef.h>

/* The whole file in memory from malloc, or NULL. */
typedef void *(*sxfont_ui_read_file)(const char *path, size_t *size);

/* The setting's name in the profile (sxn_settings_path,
 * savanxp_settings_path). */
#define SXFONT_UI_SETTINGS_NAME "appearance.cfg"
/* Where windowd, which runs as the system and has no profile of its own,
 * reads the session's: the uid 1000 desktop's, as effects.cfg. */
#define SXFONT_UI_SESSION_SETTINGS "/users/user/Settings/appearance.cfg"

#define SXFONT_UI_ITEM_BODY 0    /* menus, dialogs, labels: GFX_FACE_UI */
#define SXFONT_UI_ITEM_CAPTION 1 /* title bars: GFX_FACE_TITLE */
#define SXFONT_UI_ITEMS 2

#define SXFONT_UI_PATH_MAX 128
#define SXFONT_UI_CONFIG_BYTES 512

/* The sizes each item may take, in pixels per em: the toolkit's controls
 * and windowd's title bar have fixed heights, and these are the sizes whose
 * lines still fit them. */
#define SXFONT_UI_BODY_MIN_SIZE 10u
#define SXFONT_UI_BODY_MAX_SIZE 16u
#define SXFONT_UI_CAPTION_MIN_SIZE 10u
#define SXFONT_UI_CAPTION_MAX_SIZE 20u

struct sxfont_ui_item {
    char path[SXFONT_UI_PATH_MAX]; /* an absolute path to a TrueType file */
    unsigned size;                 /* pixels per em */
};

struct sxfont_ui_config {
    struct sxfont_ui_item items[SXFONT_UI_ITEMS];
    int smoothing; /* 1: grey where each face's gasp asks; 0 (the default): 1-bit */
};

/* The faces the tables were baked from, at the sizes they were baked at:
 * what a missing appearance.cfg means, and the same pixels as the tables. */
void sxfont_ui_default_config(struct sxfont_ui_config *config);
/* The defaults, then every key `text` sets right. */
void sxfont_ui_parse_config(struct sxfont_ui_config *config, const char *text);
/* The file's text, NUL-terminated; its length, or 0 when it does not fit. */
size_t sxfont_ui_format_config(const struct sxfont_ui_config *config, char *out, size_t capacity);
int sxfont_ui_config_equal(const struct sxfont_ui_config *a, const struct sxfont_ui_config *b);
unsigned sxfont_ui_min_size(int item);
unsigned sxfont_ui_max_size(int item);
/* The configuration at `path` (NULL for none), or the defaults. */
void sxfont_ui_read_config(struct sxfont_ui_config *config, sxfont_ui_read_file read_file, const char *path);

/* One configuration's faces, open: what Display Properties' preview draws
 * with before it is applied, and what the runtime keeps installed. An item
 * whose file does not open falls back to its default file and size, and to
 * the baked table when that fails too. NULL without libsxfont or memory. */
struct sxfont_ui_faces;
struct sxfont_ui_faces *sxfont_ui_faces_open(const struct sxfont_ui_config *config, sxfont_ui_read_file read_file);
/* How many items took a face from a file. */
int sxfont_ui_faces_count(const struct sxfont_ui_faces *faces);
/* Puts them in the body and caption slots; NULL puts the baked tables
 * back. The faces stay in use until another call replaces them. */
void sxfont_ui_faces_install(const struct sxfont_ui_faces *faces);
void sxfont_ui_faces_close(struct sxfont_ui_faces *faces);

/* The process's faces, from the configuration at `config_path` (NULL for
 * the defaults). The first call does the work and later ones return its
 * answer; sxfont_ui_reload reads the configuration again, installs its
 * faces and closes the previous ones. Both return how many slots took a face
 * from a file. */
int sxfont_ui_load(sxfont_ui_read_file read_file, const char *config_path);
int sxfont_ui_reload(sxfont_ui_read_file read_file, const char *config_path);
/* What sxfont_ui_load or _reload installed, NULL before or without them:
 * for a preview that installs other faces to put these back. */
const struct sxfont_ui_faces *sxfont_ui_current(void);
