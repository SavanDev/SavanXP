#pragma once
#include "cursor_cur.h"
void cursor_catalog_load(void);
const struct cursor_image *cursor_catalog_get(unsigned shape);
int cursor_catalog_needs_background(void);
