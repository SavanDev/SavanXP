#pragma once

#include <stddef.h>
#include <stdint.h>

#include "abi/savanxp_abi.h"

/*
 * Shortcuts and what "open" means to the shell (docs/desktop/DESKTOP_ICONS.md).
 *
 * A shortcut is a text file, `.sxlnk`, of `key=value` lines like assoc.ini:
 *
 *   target=/bin/notepad.sxe     what it opens (required)
 *   args=/users/user/notes.txt  one argument for the program (optional)
 *   workdir=/users/user         where the program starts (optional)
 *   icon=/bin/notepad.sxe       a binary whose .sxicon to show, or an icon
 *                               name from /icons (optional; the target's)
 *
 * '#' and ';' start a comment; unknown keys are ignored, so the format can
 * grow. The desktop and Files share this module, so both open things the
 * same way. No malloc: fixed capacities and truncation. It makes no system
 * call: reading, writing and starting programs go through shell/shell_io.h.
 */

#define SHORTCUT_EXTENSION ".sxlnk"
#define SHORTCUT_PATH_CAPACITY SAVANXP_DESKTOP_LAUNCH_PATH_CAPACITY
#define SHORTCUT_MAX_BYTES 2048

/* Where a folder opens, and the editor a file with no association opens in. */
#define SHORTCUT_FILES_PATH "/bin/filesapp"
#define SHORTCUT_EDITOR_PATH "/bin/notepad"

struct shortcut
{
    char target[SHORTCUT_PATH_CAPACITY];
    char args[SHORTCUT_PATH_CAPACITY];
    char workdir[SHORTCUT_PATH_CAPACITY];
    char icon[SHORTCUT_PATH_CAPACITY];
};

/* Non-zero if the name ends in .sxlnk, in any case. */
int shortcut_is_shortcut_name(const char *name);
/* Non-zero for a program: a file in /bin (or /disk/bin), or an .sxe. */
int shortcut_is_program_path(const char *path);
/* Non-zero for an ELF64 x86-64 file inside the Linux guest tree
 * (/system/linux/, no dot components): what the shell opens through SSL. */
int shortcut_is_linux_program(const char *path);
/* The /icons name a Linux program shows when it brings no icon of its own;
 * the desktop, Files and the Start menu share it. */
#define SHORTCUT_LINUX_ICON "application-x-linux"

/* 0 and the fields filled, or -1 if the text has no target. */
int shortcut_parse(const char *text, size_t length, struct shortcut *out);
/* The file's text: target first, empty fields left out. Returns the length,
 * or -1 if it does not fit. */
int shortcut_format(const struct shortcut *shortcut, char *out, size_t capacity);
int shortcut_load(const char *path, struct shortcut *out);
/* Creates or replaces the file. 0, or -1. */
int shortcut_save(const char *path, const struct shortcut *shortcut);
/* A shortcut to `target`, named after it: "Notepad.sxlnk" for /bin/notepad,
 * "notes.txt.sxlnk" for a document. `name` gets the file name, without the
 * directory. */
void shortcut_for_target(const char *target, struct shortcut *out, char *name, size_t name_capacity);
/* A shortcut to `target` in the user's Desktop folder, made if it is
 * missing, named as shortcut_for_target names it: Send To > Desktop (create
 * shortcut). A name already taken gets " (2)" and on. 0, or -1. */
int shortcut_send_to_desktop(const char *target);

/* How shortcut_open_path opened something. */
enum shortcut_open_kind
{
    SHORTCUT_OPEN_FAILED = 0,
    SHORTCUT_OPEN_PROGRAM,    /* a program was started */
    SHORTCUT_OPEN_FOLDER,     /* a folder, in Files */
    SHORTCUT_OPEN_ASSOCIATED, /* a document, with the program associated to it */
    SHORTCUT_OPEN_EDITOR,     /* a document with no association, in Notepad */
};

/*
 * Opens `path` as the shell does on a double click: a program starts, a
 * folder opens in Files, a document opens with its associated program (or
 * Notepad), and a shortcut opens its target -- with its argument and in its
 * working directory. Returns how it was opened.
 */
enum shortcut_open_kind shortcut_open_path(const char *path);

/* Edit follows the same single shortcut and working-directory rules as Open,
 * but only launches an explicitly associated editor, without a fallback. */
enum shortcut_open_kind shortcut_edit_path(const char *path);
int shortcut_can_edit_path(const char *path);

/* Parse, format and naming checks. Returns the number of failures. */
int shortcut_selftest(void);
