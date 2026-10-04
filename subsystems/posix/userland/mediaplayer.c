#include "libc.h"
#include "savanxp/sxgui.h"

#include <stdio.h>
#include <string.h>

/*
 * Media Player is a system program, but its decoder is an optional port.
 *
 * The base image always contains this small launcher.  When the FFmpeg port
 * has been built and installed, it replaces this process with the real player.
 * Without the port, the launcher remains a normal SavanXP window and explains
 * what is missing instead of disappearing from the launcher.
 *
 * This was written, then removed with SxMedia in 26b16e0, because a launcher that
 * delegates to another program was pointless when nothing could be installed that
 * was not itself a program: the decoder had to be inside the binary, and then there
 * was exactly one binary and nothing to delegate to. The loader changed that --
 * /disk/lib/libffmpeg.so.0.4 is now the installable part and this program is what
 * stays.
 *
 * It only checks that the BACKEND exists. Whether the backend's own library loaded
 * is the backend's business and it reports it in its own window, which is the right
 * place for it: it is the one that has a screen.
 */

#define MEDIA_PLAYER_PORT "/disk/bin/mediaplayer-ffmpeg"
#define MEDIA_PLAYER_WIDTH 440
#define MEDIA_PLAYER_HEIGHT 156

static struct sxgui_app g_app;
static struct sxgui_widget g_widgets[2];

static const char *g_missing_lines[] = {
    "Media Player is unavailable.",
    "",
    "FFmpeg support is not installed.",
    "Build ports/ffmpeg to enable playback.",
};

static const char *g_start_error_lines[] = {
    "Media Player could not start.",
    "",
    "The FFmpeg backend could not start.",
    "Reinstall or rebuild ports/ffmpeg, then try again.",
};

static void on_close(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    sxgui_app_quit((struct sxgui_app *)user, 0);
}

static int show_unavailable(int installed_but_failed)
{
    const char **lines = installed_but_failed ? g_start_error_lines : g_missing_lines;
    int button_y = MEDIA_PLAYER_HEIGHT - SXGUI_CONTENT_MARGIN - SXGUI_BUTTON_HEIGHT;

    g_widgets[0] = sxgui_textview(
        sx_rect_make(
            SXGUI_CONTENT_MARGIN,
            SXGUI_CONTENT_MARGIN,
            MEDIA_PLAYER_WIDTH - SXGUI_CONTENT_MARGIN * 2,
            button_y - SXGUI_CONTENT_MARGIN * 2),
        lines,
        4);
    g_widgets[1] = sxgui_button(
        sx_rect_make(
            MEDIA_PLAYER_WIDTH - SXGUI_CONTENT_MARGIN - SXGUI_BUTTON_WIDTH,
            button_y,
            SXGUI_BUTTON_WIDTH,
            SXGUI_BUTTON_HEIGHT),
        "Close",
        on_close,
        &g_app);

    if (sxgui_app_init(&g_app, "mediaplayer", g_widgets, 2) < 0)
    {
        return 1;
    }
    (void)sxgui_app_set_content_size(&g_app, MEDIA_PLAYER_WIDTH, MEDIA_PLAYER_HEIGHT);
    sxgui_focus(&g_app.ui, 1);
    return sxgui_app_run(&g_app);
}

static int port_is_installed(void)
{
    struct savanxp_stat info;

    return savanxp_stat(MEDIA_PLAYER_PORT, &info) >= 0 &&
        (info.st_mode & SAVANXP_S_IFMT) == SAVANXP_S_IFREG;
}

int main(int argc, char **argv)
{
    long result;

    if (port_is_installed())
    {
        if (argc > 0 && argv != 0)
        {
            argv[0] = (char *)MEDIA_PLAYER_PORT;
        }
        result = exec(MEDIA_PLAYER_PORT, (const char *const *)argv, argc);
        if (result < 0)
        {
            eprintf("mediaplayer: could not exec %s: %s\n",
                MEDIA_PLAYER_PORT, result_error_string(result));
        }
        return show_unavailable(1);
    }

    return show_unavailable(0);
}
