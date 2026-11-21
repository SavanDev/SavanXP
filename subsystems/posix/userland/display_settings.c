#include "display_settings.h"

#include <stdio.h>
#include <string.h>

#define DISPLAY_SETTINGS_NAME "display.cfg"

static int parse_number(const char **cursor, uint32_t *value)
{
    const char *text = *cursor;
    uint32_t result = 0;
    int digits = 0;

    while (*text >= '0' && *text <= '9' && digits < 6)
    {
        result = result * 10u + (uint32_t)(*text - '0');
        ++text;
        ++digits;
    }
    if (digits == 0)
    {
        return -1;
    }
    *cursor = text;
    *value = result;
    return 0;
}

int display_settings_read_mode(uint32_t *width, uint32_t *height)
{
    char path[256];
    char text[64];
    const char *cursor;
    long length;
    int fd;

    if (width == 0 || height == 0 ||
        savanxp_settings_path(DISPLAY_SETTINGS_NAME, 0, path, sizeof(path)) != 0)
    {
        return -1;
    }
    fd = (int)savanxp_open_mode(path, SAVANXP_OPEN_READ);
    if (fd < 0)
    {
        return -1;
    }
    length = savanxp_read(fd, text, sizeof(text) - 1);
    savanxp_close(fd);
    if (length <= 0)
    {
        return -1;
    }
    text[length] = '\0';

    cursor = strstr(text, "mode=");
    if (cursor == 0)
    {
        return -1;
    }
    cursor += 5;
    if (parse_number(&cursor, width) < 0 || *cursor != 'x')
    {
        return -1;
    }
    ++cursor;
    if (parse_number(&cursor, height) < 0 || *width == 0 || *height == 0)
    {
        return -1;
    }
    return 0;
}

int display_settings_write_mode(uint32_t width, uint32_t height)
{
    char path[256];
    char text[48];
    int length;
    long written;
    int fd;

    length = snprintf(text, sizeof(text), "mode=%ux%u\n", (unsigned int)width, (unsigned int)height);
    if (length <= 0 || length >= (int)sizeof(text) ||
        savanxp_settings_path(DISPLAY_SETTINGS_NAME, 1, path, sizeof(path)) != 0)
    {
        return -1;
    }
    fd = (int)savanxp_open_mode(path, SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE | SAVANXP_OPEN_TRUNCATE);
    if (fd < 0)
    {
        return -1;
    }
    written = savanxp_write(fd, text, (unsigned long)length);
    savanxp_close(fd);
    return written == length ? 0 : -1;
}
