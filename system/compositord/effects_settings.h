#pragma once
#include <string.h>
#include "sxn/sxneffects.h"

/* Unknown keys and malformed values leave that option at its default. */
static inline uint32_t effects_settings_parse(const char *text) {
    uint32_t effects = SXN_EFFECTS_ALL;
    while (*text) {
        const char *end = strchr(text, '\n');
        size_t length = end ? (size_t)(end - text) : strlen(text);
        uint32_t bit = 0;
        size_t prefix = 0;
        if (length == 16 && !memcmp(text, "window_shadows=", 15)) { bit = SXN_EFFECTS_WINDOWS; prefix = 15; }
        if (length == 15 && !memcmp(text, "cursor_shadow=", 14)) { bit = SXN_EFFECTS_CURSOR; prefix = 14; }
        if (bit && (text[prefix] == '0' || text[prefix] == '1')) {
            if (text[prefix] == '0') effects &= ~bit; else effects |= bit;
        }
        if (!end) break;
        text = end + 1;
    }
    return effects;
}
