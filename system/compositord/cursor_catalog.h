#pragma once
#include "abi/savanxp_abi.h"
#include "sxn/sxncursor.h"
#include "cursor_cur.h"

#define CURSOR_MAX_THEMES 16
struct cursor_theme {
    char id[SXN_CURSOR_ID_SIZE];
    char name[48];
    char license[24];
};

/* /system/cursors/themes.list: id|display name|license, one pack per line. */
unsigned cursor_catalog_discover(struct cursor_theme themes[CURSOR_MAX_THEMES]);
long cursor_catalog_read(const char *theme, struct cursor_image *images, int strict);
void cursor_catalog_load(void);
long cursor_catalog_select(const char *theme);
const char *cursor_catalog_theme(void);
long cursor_catalog_save_status(void);
long cursor_catalog_save(void);
const struct cursor_image *cursor_catalog_get(unsigned shape);
int cursor_catalog_needs_background(void);
