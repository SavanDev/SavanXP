/* The interface faces from /system/fonts (docs/desktop/UI_FONTS.md,
 * "Interface faces from files"): what a program's runtime calls once, before
 * it draws, to put the files' faces in the text primitives' slots.
 *
 * Compiled into the SUN and POSIX runtimes; each passes its own way of reading
 * a file. It refers to libsxfont weakly: a program that maps libsxfont gets
 * the faces from files, one that does not (every static program) keeps the
 * baked tables, and so does any program when a file is missing or damaged. */
#pragma once

#include <stddef.h>

/* The whole file in memory from malloc, or NULL. */
typedef void *(*sxfont_ui_read_file)(const char *path, size_t *size);

/* Installs the interface faces whose files read and open; the rest keep their
 * baked table. The first call does the work and later ones return its
 * answer. Returns how many slots took a face from a file. */
int sxfont_ui_load(sxfont_ui_read_file read_file);
