/* The toolkit's clipboard formats in a SUN program (docs/desktop/CLIPBOARD.md).
 *
 * Linked LINK_PROFILE PIE against libsxgui.so, so the toolkit is the shared
 * one and finds the clipboard's format table the way an installed program's
 * would: libsxgui asks the program for sxgui_default_host_clipboard, and the
 * SUN runtime (system/runtime/sxngui.c) answers it over POSIX's weak one. No
 * window: the calls stand on the host alone.
 *
 * Each failure is a line of the system log; the exit status is the number of
 * failures, which the core smoke checks. */

#include <stdint.h>
#include <string.h>

#include "abi/savanxp_abi.h"
#include "sxgfx/sxgui.h"
#include "sxn/sxn.h"

static int g_failures = 0;

static void check(int ok, const char *what)
{
    char line[120] = "guicliptest: FAILED ";
    unsigned used = 20;

    if (ok)
    {
        return;
    }
    ++g_failures;
    while (*what != '\0' && used + 1 < sizeof(line))
    {
        line[used++] = *what++;
    }
    line[used] = '\0';
    (void)sxn_log(line);
}

int main(void)
{
    static const char uris[] = "file:///system/console.sxe\r\n";
    static const char text[] = "/system/console.sxe";
    const struct sxgui_clipboard_format formats[2] = {
        {SAVANXP_CLIPBOARD_URI_LIST, uris, sizeof(uris) - 1},
        {SAVANXP_CLIPBOARD_TEXT, text, sizeof(text) - 1},
    };
    struct savanxp_clipboard_info info;
    char buffer[64];
    char name[SAVANXP_CLIPBOARD_NAME_CAPACITY];
    long device;

    check(sxgui_clipboard_set_formats(formats, 2) >= 0, "set_formats");
    check(sxgui_clipboard_get_format(SAVANXP_CLIPBOARD_URI_LIST, buffer, (int)sizeof(buffer)) ==
                  (int)sizeof(uris) - 1 &&
              memcmp(buffer, uris, sizeof(uris) - 1) == 0,
          "get_format of the uri list");
    check(sxgui_clipboard_has_format(SAVANXP_CLIPBOARD_TEXT) && !sxgui_clipboard_has_format("image/bmp"),
          "has_format");
    check(sxgui_clipboard_enum_format(0, name, (int)sizeof(name)) == (int)sizeof(uris) - 1 &&
              strcmp(name, SAVANXP_CLIPBOARD_URI_LIST) == 0,
          "enum_format keeps the writer's order");
    check(sxgui_clipboard_enum_format(2, name, (int)sizeof(name)) == -SAVANXP_ENOENT, "enum_format past the end");
    check(sxgui_clipboard_get_format("image/bmp", buffer, (int)sizeof(buffer)) == -SAVANXP_ENOENT,
          "a missing format is ENOENT");

    /* What the host wrote is the device's: the text widgets' host reads the
     * same text. */
    device = sxn_open_device("clipboard", SXN_DEVICE_READ | SXN_DEVICE_WRITE);
    check(device >= 0, "open the device");
    if (device >= 0)
    {
        check(sxn_device_control(device, CLIP_IOC_GET_INFO, (unsigned long)&info) == 0 &&
                  info.format_count == 2u && info.length == sizeof(text) - 1,
              "the device holds both formats");
        (void)sxn_device_control(device, CLIP_IOC_CLEAR, 0);
        (void)sxn_close(device);
    }

    (void)sxn_log(g_failures == 0 ? "guicliptest: PASS" : "guicliptest: FAIL");
    return g_failures;
}
