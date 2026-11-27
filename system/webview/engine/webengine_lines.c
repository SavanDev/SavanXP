/* Web Viewer's inline flow (webengine_layout.h). The line boxes arrive with
 * the next part of E4; until then inline content takes no height. */

#include "webengine_layout.h"

int webengine_layout_lines(const struct webengine_fonts *fonts, struct webengine_box *block, int x, int y,
                           int width) {
    (void)fonts;
    (void)block;
    (void)x;
    (void)y;
    (void)width;
    return 0;
}
