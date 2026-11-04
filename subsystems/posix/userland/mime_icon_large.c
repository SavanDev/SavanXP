#include "mime_icon.h"

/*
 * The 32x32 cache, 48 KiB of BSS, in a file of its own so that only the
 * programs that draw large icons carry it (see MIME_ICON_CACHE_ENTRIES in
 * mime_icon.h).
 */

static struct mime_icon_cache_slot g_large_slots[MIME_ICON_CACHE_ENTRIES];
static uint32_t g_large_pixels[MIME_ICON_CACHE_ENTRIES * MIME_ICON_LARGE_SIZE * MIME_ICON_LARGE_SIZE];
static struct mime_icon_cache g_large_cache = {
    MIME_ICON_LARGE_SIZE, 0, g_large_slots, g_large_pixels};

const struct sx_bitmap *mime_icon_large(const char *icon_name)
{
    return mime_icon_cache_load(&g_large_cache, icon_name);
}

const struct sx_bitmap *mime_icon_large_for_file(const char *file_name, int is_dir, int launchable)
{
    return mime_icon_large(mime_icon_name_for(file_name, is_dir, launchable));
}
