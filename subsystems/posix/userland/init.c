#include "libc.h"

#include <pthread.h>
#include <pwd.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sxn/sxn.h"

static int is_space_char(char value) {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

static int text_contains(const char* text, const char* needle) {
    size_t needle_length = strlen(needle);
    if (needle_length == 0) {
        return 1;
    }
    for (size_t index = 0; text[index] != '\0'; ++index) {
        if (strncmp(text + index, needle, needle_length) == 0) {
            return 1;
        }
    }
    return 0;
}

static int text_starts_with(const char* text, const char* prefix) {
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

static const char* skip_spaces(const char* text) {
    while (text != 0 && is_space_char(*text)) {
        ++text;
    }
    return text;
}

static void trim_automation_spec(char* spec) {
    size_t length = strlen(spec);
    while (length != 0 && is_space_char(spec[length - 1])) {
        spec[length - 1] = '\0';
        length -= 1;
    }
}

static const char* automation_label_for_spec(const char* spec) {
    if (spec != 0 && text_contains(spec, "float")) {
        return "FLOAT SMOKE";
    }
    if (spec != 0 && text_contains(spec, "piesmoke")) {
        return "PIE SMOKE";
    }
    if (spec != 0 && text_contains(spec, "cmakesmoke")) {
        return "CMAKE SMOKE";
    }
    if (spec != 0 && text_contains(spec, "sxbasic")) {
        return "SXBASIC SMOKE";
    }
    if (spec != 0 && text_contains(spec, "sxbforms")) {
        return "SXBASIC FORMS SMOKE";
    }
    if (spec != 0 && text_contains(spec, "sxbmake")) {
        return "SXBASIC MAKE SMOKE";
    }
    if (spec != 0 && text_contains(spec, "sxbide")) {
        return "SXBIDE SMOKE";
    }
    if (spec != 0 && text_contains(spec, "ccleste")) {
        return "CCLESTE SELFTEST";
    }
    if (spec != 0 && text_contains(spec, "progman")) {
        return "PROGMAN SMOKE";
    }
    if (spec != 0 && text_contains(spec, "appwiz")) {
        return "APPWIZ SMOKE";
    }
    if (spec != 0 && text_contains(spec, "sxe")) {
        return "SXE SMOKE";
    }
    if (spec != 0 && text_contains(spec, "filesapp")) {
        return "FILESAPP SMOKE";
    }
    if (spec != 0 && text_contains(spec, "taskmgr")) {
        return "TASKMGR SMOKE";
    }
    if (spec != 0 && text_contains(spec, "diskmgmt")) {
        return "DISKMGMT SMOKE";
    }
    if (spec != 0 && text_contains(spec, "logview")) {
        return "LOGVIEW SMOKE";
    }
    if (spec != 0 && text_contains(spec, "imgview-selftest")) {
        return "IMGVIEW SELFTEST";
    }
    if (spec != 0 && text_contains(spec, "mines")) {
        return "MINES SMOKE";
    }
    if (spec != 0 && text_contains(spec, "calc")) {
        return "CALC SMOKE";
    }
    if (spec != 0 && text_contains(spec, "clocktest")) {
        return "CLOCK SMOKE";
    }
    if (spec != 0 && text_contains(spec, "soak")) {
        return "SOAK";
    }
    if (spec != 0 && text_contains(spec, "windowd-hw-native")) {
        return "NATIVE HW";
    }
    if (spec != 0 && text_contains(spec, "windowd")) {
        return "WINDOWD SMOKE";
    }
    if (spec != 0 && text_contains(spec, "kbd")) {
        return "KBD SMOKE";
    }
    if (spec != 0 && text_contains(spec, "mousetest")) {
        return "POINTER SMOKE";
    }
    if (spec != 0 && text_contains(spec, "audiostream")) {
        return "AUDIO STREAM";
    }
    if (spec != 0 && text_contains(spec, "audiorecord")) {
        return "AUDIO RECORD";
    }
    if (spec != 0 && text_contains(spec, "netsmoke")) {
        return "NET SMOKE";
    }
    if (spec != 0 && text_contains(spec, "tcptest")) {
        return "TCP SMOKE";
    }
    if (spec != 0 && text_contains(spec, "mediaplayer-show")) {
        return "MEDIAPLAYER DISPLAY";
    }
    if (spec != 0 && text_contains(spec, "mediaplayer")) {
        return "MEDIAPLAYER SMOKE";
    }
    if (spec != 0 && text_contains(spec, "sessiontest")) {
        return "SESSION SMOKE";
    }
    if (spec != 0 && text_contains(spec, "elevtest")) {
        return "ELEVATION SMOKE";
    }
    if (spec != 0 && text_contains(spec, "nativepower")) {
        return "NATIVE POWER";
    }
    if (spec != 0 && (text_contains(spec, "nativetest") || text_contains(spec, "nativecapture") || text_contains(spec, "nativedevices") || text_contains(spec, "nativepreferences"))) {
        return "NATIVE TEST";
    }
    if (spec != 0 && text_contains(spec, "win32test")) {
        return "WIN32 HELLO";
    }
    if (spec != 0 && text_contains(spec, "linuxtest")) {
        return "LINUX TEST";
    }
    if (spec != 0 && text_contains(spec, "sunlibtest")) {
        return "SUN LIBRARY TEST";
    }
    if (spec != 0 && text_contains(spec, "sonametest")) {
        return "SONAME TEST";
    }
    if (spec != 0 && text_contains(spec, "ldfailtest")) {
        return "LOADER FAIL TEST";
    }
    if (spec != 0 && text_contains(spec, "ldstatictest")) {
        return "LOADER STATIC TEST";
    }
    if (spec != 0 && text_contains(spec, "ldimagetest")) {
        return "LOADER IMAGE TEST";
    }
    if (spec != 0 && text_contains(spec, "linuxrestart")) {
        return "LINUX RESTART";
    }
    if (spec != 0 && text_contains(spec, "win32window")) {
        return "WIN32 WINDOW";
    }
    if (spec != 0 && text_contains(spec, "win32cards")) {
        return "WIN32 CARDS";
    }
    if (spec != 0 && text_contains(spec, "win32dialog")) {
        return "WIN32 DIALOG";
    }
    if (spec != 0 && text_contains(spec, "win32solitaire")) {
        return "WIN32 SOLITAIRE";
    }
    return "SMOKE";
}

static int run_automation_spec(const char* spec) {
    const char* smoke_argv[] = {"/bin/smoke", 0};
    const char* soak_argv[] = {"/bin/gputest", "--soak", 0, 0};
    const char* windowd_argv[] = {"/system/windowd.sxe", "--selftest", 0};
    const char* windowd_hw_native_argv[] = {"/system/windowd.sxe", "--hw-native-test", 0};
    const char* cursor_fallback_argv[] = {"/system/compositord.sxe", "--cursor-fallback-selftest", 0};
    const char* cursor_smoke_argv[] = {"/system/compositord.sxe", "--cursor-selftest", 0};
    const char* cursor_repro_argv[] = {"/system/windowd.sxe", "--cursor-repro", 0};
    const char* progman_selftest_argv[] = {"/bin/progman", "--selftest", 0};
    const char* appwiz_selftest_argv[] = {"/bin/appwiz", "--selftest", 0};
    const char* sxetest_argv[] = {"/bin/sxetest", 0};
    const char* filesapp_selftest_argv[] = {"/bin/filesapp", "--selftest", 0};
    const char* taskmgr_selftest_argv[] = {"/system/taskmgr.sxe", "--selftest", 0};
    const char* diskmgmt_selftest_argv[] = {"/system/diskmgmt.sxe", "--selftest", 0};
    const char* logview_selftest_argv[] = {"/system/logview.sxe", "--selftest", 0};
    const char* imgview_selftest_argv[] = {"/system/imgview.sxe", "--selftest", 0};
    const char* calc_selftest_argv[] = {"/bin/calc", "--selftest", 0};
    const char* mines_selftest_argv[] = {"/bin/mines", "--selftest", 0};
    const char* paint_selftest_argv[] = {"/bin/paint", "--selftest", 0};
    const char* clocktest_argv[] = {"/bin/clocktest", 0};
    const char* datetest_argv[] = {"/bin/datetest", 0};
    const char* needstest_argv[] = {"/bin/needstest", 0};
    const char* syslogtest_argv[] = {"/bin/syslogtest", 0};
    const char* loaderlog_argv[] = {"/bin/syslogtest", "--loader", 0};
    /* Los cuatro diagnósticos del cargador. Cada uno afirmaba en error y nada más,
     * así que ninguno podía tener escenario: ahora cada uno imprime su token de
     * éxito y hay uno que lo ejecuta. */
    const char* brokentest_argv[] = {"/bin/brokentest", 0};
    const char* missingtest_argv[] = {"/bin/missingtest", 0};
    const char* slottest_argv[] = {"/bin/slottest", 0};
    const char* diamondtest_argv[] = {"/bin/diamondtest", 0};
    const char* dltest_argv[] = {"/bin/dltest", 0};
    const char* ffmpegload_argv[] = {"/bin/ffmpegload", 0};
    const char* audiostream_argv[] = {"/bin/audiotest", "--stream", 0};
    const char* audiostream_quiet_argv[] = {"/bin/audiotest", "--stream-quiet", 0};
    const char* audiod_selftest_argv[] = {"/bin/audiod", "--selftest", 0};
    const char* volume_argv[] = {"/bin/volume", "75", 0};
    const char* audiorecord_argv[] = {"/bin/audiotest", "--record", 0};
    const char* nettest_argv[] = {"/bin/nettest", 0};
    const char* tcptest_argv[] = {"/bin/tcptest", 0, 0};
    const char* ntptest_argv[] = {"/bin/ntptest", 0, 0};
    const char* floatsmoke_argv[] = {"/bin/floatsmoke", 0};
    /* An external PIE program from the SDK (sdk/piesmoke). */
    const char* piesmoke_argv[] = {"/bin/piesmoke", 0};
    /* A program built with CMake and the SDK's toolchain file (sdk/cmakesmoke). */
    const char* cmakesmoke_argv[] = {"/bin/cmakesmoke", 0};
    /* The SxBASIC console sample against the output it must print (docs/userland/SXBASIC.md). */
    const char* sxbasic_argv[] = {"/system/sxbasic.sxe", "--expect", "/system/sxbasic/samples/hello/expected-output.txt",
                                  "/system/sxbasic/samples/hello/hello.sbp", 0};
    /* Its Windows samples with no window, driven by their selftest scripts. */
    const char* sxbforms_greeter_argv[] = {"/system/sxbasic.sxe", "--selftest", "/system/sxbasic/selftests/greeter.txt",
                                           "/system/sxbasic/samples/greeter/greeter.sbp", 0};
    const char* sxbforms_converter_argv[] = {"/system/sxbasic.sxe", "--selftest",
                                             "/system/sxbasic/selftests/converter.txt",
                                             "/system/sxbasic/samples/converter/converter.sbp", 0};
    /* Make SXE from inside SavanXP, then the made program run with its selftest. */
    const char* sxbmake_argv[] = {"/system/sxbasic.sxe", "--make-selftest", "/system/sxbasic/selftests/converter-made.txt",
                                  "/system/sxbasic/samples/converter/converter.sbp", "/tmp/Converter.sxe", 0};
    /* Savan BASIC's environment with no window: open, run, find an error, make. */
    const char* sxbide_argv[] = {"/system/sxbide.sxe", "--selftest", "/system/sxbasic/selftests/ide.txt", 0};
    /* Its form designer: draw, move, resize, delete, a handler written and run. */
    const char* sxbide_design_argv[] = {"/system/sxbide.sxe", "--selftest", "/system/sxbasic/selftests/ide-design.txt",
                                        0};
    const char* nativetest_argv[] = {"/bin/nativetest.sxe", 0};
    const char* nativenetwork_argv[] = {"/bin/nativetest.sxe", "network", 0, 0};
    const char* nativecapture_argv[] = {"/bin/nativetest.sxe", "audio-capture", 0};
    const char* nativedevices_argv[] = {"/bin/nativetest.sxe", "audio-devices", 0};
    const char* nativepreferences_argv[] = {"/bin/nativetest.sxe", "audio-preferences", 0};
    const char* nativepower_argv[] = {"/bin/nativetest.sxe", "power", 0};
    const char* nightwood_argv[] = {"/system/nightwood.sxe", "--selftest", 0};
    const char* sxgltest_argv[] = {"/system/sxgltest.sxe", 0};
    const char* win32test_argv[] = {"/system/win32test.sxe", 0};
    const char* linuxtest_argv[] = {"/system/linuxtest.sxe", 0};
    const char* sunlibtest_argv[] = {"/system/sunlibtest.sxe", 0};
    const char* sonametest_argv[] = {"/system/sonametest.sxe", 0};
    const char* ldfailtest_argv[] = {"/system/ldfailtest.sxe", 0};
    const char* ldstatictest_argv[] = {"/system/ldstatictest.sxe", 0};
    const char* ldimagetest_argv[] = {"/system/ldimagetest.sxe", 0};
    const char* linuxxeyes_argv[] = {"/system/linuxtest.sxe", "--xeyes", 0};
    const char* linuxxclient_argv[] = {"/system/linuxtest.sxe", "--xclient", 0};
    const char* linuxdynamic_argv[] = {"/system/linuxtest.sxe", "--dynamic", 0};
    const char* linuxnolib_argv[] = {"/system/linuxtest.sxe", "--dynamic", "nolib", 0};
    const char* linuxnointerp_argv[] = {"/system/linuxtest.sxe", "--dynamic", "nointerp", 0};
    const char* linuxtools_argv[] = {"/system/linuxtest.sxe", "--tools", 0};
    const char* linuxrestart_argv[] = {"/system/linuxtest.sxe", "--restart", 0};
    const char* win32window_argv[] = {"/system/win32test.sxe", "--window", 0};
    const char* win32cards_argv[] = {"/system/win32test.sxe", "--cards", 0};
    const char* win32dialog_argv[] = {"/system/win32test.sxe", "--dialog", 0};
    const char* win32solitaire_argv[] = {"/system/win32test.sxe", "--solitaire", 0};
    const char* pthreadtest_argv[] = {"/bin/pthreadtest", 0};
    const char* sessiontest_argv[] = {"/bin/sessiontest", 0};
    const char* elevtest_argv[] = {"/bin/elevtest", 0};

    const char* panictest_argv[] = {"/bin/panictest", 0};
    const char* panicdisplay_argv[] = {"/bin/panictest", "--display", 0, 0};
    const char* ccleste_argv[] = {"/bin/ccleste", "--selftest", 0};
    const char* kbdtest_argv[] = {"/bin/kbdtest", "--selftest", 0};
    const char* mousetest_argv[] = {"/bin/mousetest", "--selftest", 0};
    /* Three paths: audio only (WAV), video only with no container (raw MJPEG),
     * and both interleaved in AVI, which also measures the sync. Then two files
     * with codecs the port does not enable, which have to be named. */
    const char* mediaplayer_argv[] = {"/bin/mediaplayer", "--selftest", "/media/tono.wav",
                                      "/media/clip.mjpeg", "--sync", "/media/avsync.avi",
                                      "--unsupported", "ffv1", "/media/ffv1.avi",
                                      "--unsupported", "pcm_mulaw", "/media/mulaw.wav", 0};
    const char* mediaplayer_show_argv[] = {"/bin/mediaplayer", "--gpu-hold", "8000", "/media/avsync.avi", 0};
    const char* path = "/bin/smoke";
    const char* const* argv = smoke_argv;
    const char* label = automation_label_for_spec(spec);
    int argc = 1;
    int status = 0;

    if (spec != 0 && strcmp(spec, "smoke") != 0 && spec[0] != '\0') {
        if (strcmp(spec, "windowd-selftest") == 0 || strcmp(spec, "windowd") == 0) {
            path = "/system/windowd.sxe";
            argv = windowd_argv;
            argc = 2;
        } else if (strcmp(spec, "windowd-hw-native") == 0) {
            path = "/system/windowd.sxe";
            argv = windowd_hw_native_argv;
            argc = 2;
        } else if (strcmp(spec, "progman-selftest") == 0 || strcmp(spec, "progman") == 0) {
            path = "/bin/progman";
            argv = progman_selftest_argv;
            argc = 2;
        } else if (strcmp(spec, "appwiz-selftest") == 0 || strcmp(spec, "appwiz") == 0) {
            path = "/bin/appwiz";
            argv = appwiz_selftest_argv;
            argc = 2;
        } else if (strcmp(spec, "sxe-selftest") == 0 || strcmp(spec, "sxe") == 0) {
            path = "/bin/sxetest";
            argv = sxetest_argv;
            argc = 1;
        } else if (strcmp(spec, "filesapp-selftest") == 0 || strcmp(spec, "filesapp") == 0) {
            path = "/bin/filesapp";
            argv = filesapp_selftest_argv;
            argc = 2;
        } else if (strcmp(spec, "taskmgr-selftest") == 0 || strcmp(spec, "taskmgr") == 0) {
            path = "/system/taskmgr.sxe";
            argv = taskmgr_selftest_argv;
            argc = 2;
        } else if (strcmp(spec, "diskmgmt-selftest") == 0 || strcmp(spec, "diskmgmt") == 0) {
            path = "/system/diskmgmt.sxe";
            argv = diskmgmt_selftest_argv;
            argc = 2;
        } else if (strcmp(spec, "logview-selftest") == 0 || strcmp(spec, "logview") == 0) {
            path = "/system/logview.sxe";
            argv = logview_selftest_argv;
            argc = 2;
        } else if (strcmp(spec, "imgview-selftest") == 0) {
            path = "/system/imgview.sxe";
            argv = imgview_selftest_argv;
            argc = 2;
        } else if (strcmp(spec, "mines-selftest") == 0 || strcmp(spec, "mines") == 0) {
            path = "/bin/mines";
            argv = mines_selftest_argv;
            argc = 2;
        } else if (strcmp(spec, "paint-selftest") == 0 || strcmp(spec, "paint") == 0) {
            path = "/bin/paint";
            argv = paint_selftest_argv;
            argc = 2;
        } else if (strcmp(spec, "calc-selftest") == 0 || strcmp(spec, "calc") == 0) {
            path = "/bin/calc";
            argv = calc_selftest_argv;
            argc = 2;
        } else if (strcmp(spec, "ffmpegload") == 0) {
            /* Carga libffmpeg.so.0.4 y mide. El escenario instala el port antes,
             * asi que la libreria esta en el volumen. */
            path = "/bin/ffmpegload";
            argv = ffmpegload_argv;
            argc = 1;
        } else if (strcmp(spec, "brokentest") == 0) {
            path = "/bin/brokentest";
            argv = brokentest_argv;
            argc = 1;
        } else if (strcmp(spec, "missingtest") == 0) {
            path = "/bin/missingtest";
            argv = missingtest_argv;
            argc = 1;
        } else if (strcmp(spec, "slottest") == 0) {
            path = "/bin/slottest";
            argv = slottest_argv;
            argc = 1;
        } else if (strcmp(spec, "diamondtest") == 0) {
            path = "/bin/diamondtest";
            argv = diamondtest_argv;
            argc = 1;
        } else if (strcmp(spec, "dltest") == 0) {
            path = "/bin/dltest";
            argv = dltest_argv;
            argc = 1;
        } else if (strcmp(spec, "needstest") == 0) {
            /* El programa con una dependencia declarada. El escenario
             * needstest-missing borra la libreria del volumen antes de arrancar;
             * con ella en su sitio el mismo programa resuelve y se va. */
            path = "/bin/needstest";
            argv = needstest_argv;
            argc = 1;
        } else if (strcmp(spec, "syslogtest") == 0) {
            /* A POSIX line in the system log, with stdout and stderr closed. */
            path = "/bin/syslogtest";
            argv = syslogtest_argv;
            argc = 1;
        } else if (strcmp(spec, "loaderlog") == 0) {
            /* The loader's fallback notice from POSIX with stderr closed. */
            path = "/bin/syslogtest";
            argv = loaderlog_argv;
            argc = 2;
        } else if (strcmp(spec, "clocktest") == 0) {
            path = "/bin/clocktest";
            argv = clocktest_argv;
            argc = 1;
        } else if (strcmp(spec, "datetest") == 0) {
            path = "/bin/datetest";
            argv = datetest_argv;
            argc = 1;
        } else if (strcmp(spec, "netsmoke") == 0) {
            path = "/bin/nettest";
            argv = nettest_argv;
            argc = 1;
        } else if (strcmp(spec, "mediaplayer") == 0) {
            path = "/bin/mediaplayer";
            argv = mediaplayer_argv;
            argc = (int)(sizeof(mediaplayer_argv) / sizeof(mediaplayer_argv[0])) - 1;
        } else if (strcmp(spec, "mediaplayer-show") == 0) {
            path = "/bin/mediaplayer";
            argv = mediaplayer_show_argv;
            argc = 4;
        } else if (strcmp(spec, "floatsmoke") == 0 || strcmp(spec, "float-smoke") == 0) {
            path = "/bin/floatsmoke";
            argv = floatsmoke_argv;
            argc = 1;
        } else if (strcmp(spec, "piesmoke") == 0) {
            path = "/bin/piesmoke";
            argv = piesmoke_argv;
            argc = 1;
        } else if (strcmp(spec, "cmakesmoke") == 0) {
            path = "/bin/cmakesmoke";
            argv = cmakesmoke_argv;
            argc = 1;
        } else if (strcmp(spec, "sxbasic") == 0) {
            path = "/system/sxbasic.sxe";
            argv = sxbasic_argv;
            argc = 4;
        } else if (strcmp(spec, "sxbforms-greeter") == 0 || strcmp(spec, "sxbforms-converter") == 0) {
            path = "/system/sxbasic.sxe";
            argv = spec[9] == 'g' ? sxbforms_greeter_argv : sxbforms_converter_argv;
            argc = 4;
        } else if (strcmp(spec, "sxbmake") == 0) {
            path = "/system/sxbasic.sxe";
            argv = sxbmake_argv;
            argc = 5;
        } else if (strcmp(spec, "sxbide") == 0) {
            path = "/system/sxbide.sxe";
            argv = sxbide_argv;
            argc = 3;
        } else if (strcmp(spec, "sxbide-design") == 0) {
            path = "/system/sxbide.sxe";
            argv = sxbide_design_argv;
            argc = 3;
        } else if (strcmp(spec, "elevtest") == 0) {
            path = "/bin/elevtest";
            argv = elevtest_argv;
            argc = 1;
        } else if (strcmp(spec, "panictest") == 0) {
            path = "/bin/panictest";
            argv = panictest_argv;
            argc = 1;
        } else if (text_starts_with(spec, "panictest ")) {
            /* The panic text must be what the screen shows, whichever DISPI
             * buffer was visible (FB4, docs/roadmaps/open/FRAMEBUFFER_ROADMAP.md). */
            path = "/bin/panictest";
            panicdisplay_argv[2] = skip_spaces(spec + strlen("panictest"));
            argv = panicdisplay_argv;
            argc = 3;
        } else if (strcmp(spec, "sessiontest") == 0) {
            path = "/bin/sessiontest";
            argv = sessiontest_argv;
            argc = 1;
        } else if (strcmp(spec, "pthreadtest") == 0 || strcmp(spec, "pthread-test") == 0) {
            path = "/bin/pthreadtest";
            argv = pthreadtest_argv;
            argc = 1;
        } else if (text_starts_with(spec, "nativenetwork ")) {
            path = "/bin/nativetest.sxe";
            nativenetwork_argv[2] = skip_spaces(spec + strlen("nativenetwork"));
            argv = nativenetwork_argv;
            argc = 3;
        } else if (strcmp(spec, "nativepower") == 0) {
            /* Data written before the power button survives it: the host
             * powers the machine off and boots it again between the two runs
             * (docs/kernel/POWER.md). */
            path = "/bin/nativetest.sxe";
            argv = nativepower_argv;
            argc = 2;
        } else if (strcmp(spec, "nativepreferences") == 0) {
            path = "/bin/nativetest.sxe";
            argv = nativepreferences_argv;
            argc = 2;
        } else if (strcmp(spec, "nativedevices") == 0) {
            path = "/bin/nativetest.sxe";
            argv = nativedevices_argv;
            argc = 2;
        } else if (strcmp(spec, "nativecapture") == 0) {
            path = "/bin/nativetest.sxe";
            argv = nativecapture_argv;
            argc = 2;
        } else if (strcmp(spec, "nativetest") == 0 || strcmp(spec, "native-test") == 0) {
            path = "/bin/nativetest.sxe";
            argv = nativetest_argv;
            argc = 1;
        } else if (strcmp(spec, "nightwood-selftest") == 0) {
            path = "/system/nightwood.sxe";
            argv = nightwood_argv;
            argc = 2;
        } else if (strcmp(spec, "sxgltest") == 0) {
            path = "/system/sxgltest.sxe";
            argv = sxgltest_argv;
            argc = 1;
        } else if (strcmp(spec, "win32test") == 0) {
            /* The Win32 fixtures through winhost (docs/userland/WIN32.md). */
            path = "/system/win32test.sxe";
            argv = win32test_argv;
            argc = 1;
        } else if (strcmp(spec, "linuxtest") == 0) {
            /* The Linux fixtures through the launcher (docs/userland/LINUX.md). */
            path = "/system/linuxtest.sxe";
            argv = linuxtest_argv;
            argc = 1;
        } else if (strcmp(spec, "sunlibtest") == 0) {
            /* A SUN program mapping libmath through the shared loader
             * (docs/kernel/SHARED_LIBRARIES.md). */
            path = "/system/sunlibtest.sxe";
            argv = sunlibtest_argv;
            argc = 1;
        } else if (strcmp(spec, "ldfailtest") == 0) {
            /* The loader's failure paths, one fixture per step. */
            path = "/system/ldfailtest.sxe";
            argv = ldfailtest_argv;
            argc = 1;
        } else if (strcmp(spec, "ldstatictest") == 0) {
            /* A STATIC program that links the loader: step -11. */
            path = "/system/ldstatictest.sxe";
            argv = ldstatictest_argv;
            argc = 1;
        } else if (strcmp(spec, "ldimagetest") == 0) {
            /* The program's own image refused by the loader: step -10. */
            path = "/system/ldimagetest.sxe";
            argv = ldimagetest_argv;
            argc = 1;
        } else if (strcmp(spec, "sonametest") == 0) {
            /* How a DT_NEEDED resolves when the exact library is missing. */
            path = "/system/sonametest.sxe";
            argv = sonametest_argv;
            argc = 1;
        } else if (strcmp(spec, "linuxxeyes") == 0) {
            path = "/system/linuxtest.sxe";
            argv = linuxxeyes_argv;
            argc = 2;
        } else if (strcmp(spec, "linuxxclient") == 0) {
            path = "/system/linuxtest.sxe";
            argv = linuxxclient_argv;
            argc = 2;
        } else if (strcmp(spec, "linuxdynamic") == 0 || strcmp(spec, "linuxnolib") == 0 || strcmp(spec, "linuxnointerp") == 0) {
            path = "/system/linuxtest.sxe";
            argv = strcmp(spec, "linuxnolib") == 0 ? linuxnolib_argv :
                   strcmp(spec, "linuxnointerp") == 0 ? linuxnointerp_argv : linuxdynamic_argv;
            argc = strcmp(spec, "linuxdynamic") == 0 ? 2 : 3;
        } else if (strcmp(spec, "linuxtools") == 0) {
            path = "/system/linuxtest.sxe";
            argv = linuxtools_argv;
            argc = 2;
        } else if (strcmp(spec, "linuxrestart") == 0) {
            /* A Linux program's file read back after a restart: the host
             * resets the machine between the two runs. */
            path = "/system/linuxtest.sxe";
            argv = linuxrestart_argv;
            argc = 2;
        } else if (strcmp(spec, "win32window") == 0) {
            /* A Win32 window under windowd, driven by the host over QMP. */
            path = "/system/win32test.sxe";
            argv = win32window_argv;
            argc = 2;
        } else if (strcmp(spec, "win32cards") == 0) {
            /* The user's cards.dll, which the scenario copies in. */
            path = "/system/win32test.sxe";
            argv = win32cards_argv;
            argc = 2;
        } else if (strcmp(spec, "win32dialog") == 0) {
            /* Win32 menus and dialogs under windowd, driven over QMP. */
            path = "/system/win32test.sxe";
            argv = win32dialog_argv;
            argc = 2;
        } else if (strcmp(spec, "win32solitaire") == 0) {
            /* The user's sol.exe, which the scenario copies in, driven over QMP. */
            path = "/system/win32test.sxe";
            argv = win32solitaire_argv;
            argc = 2;
        } else if (strcmp(spec, "ccleste-selftest") == 0 || strcmp(spec, "ccleste") == 0) {
            path = "/bin/ccleste";
            argv = ccleste_argv;
            argc = 2;
        } else if (strcmp(spec, "cursor-fallback-selftest") == 0) {
            path = "/system/compositord.sxe";
            argv = cursor_fallback_argv;
            argc = 2;
        } else if (strcmp(spec, "cursor-selftest") == 0) {
            path = "/system/compositord.sxe";
            argv = cursor_smoke_argv;
            argc = 2;
        } else if (strcmp(spec, "windowd-cursor-repro") == 0) {
            path = "/system/windowd.sxe";
            argv = cursor_repro_argv;
            argc = 2;
        } else if (strcmp(spec, "soak") == 0 || strcmp(spec, "gputest --soak") == 0) {
            path = "/bin/gputest";
            argv = soak_argv;
            argc = 2;
        } else if (strcmp(spec, "audiostream") == 0) {
            path = "/bin/audiotest";
            argv = audiostream_argv;
            argc = 2;
        } else if (strcmp(spec, "audiostream-quiet") == 0) {
            path = "/bin/audiotest";
            argv = audiostream_quiet_argv;
            argc = 2;
        } else if (strcmp(spec, "audiodselftest") == 0) {
            path = "/bin/audiod";
            argv = audiod_selftest_argv;
            argc = 2;
        } else if (strcmp(spec, "volumesmoke") == 0) {
            path = "/bin/volume";
            argv = volume_argv;
            argc = 2;
        } else if (strcmp(spec, "audiorecord") == 0) {
            path = "/bin/audiotest";
            argv = audiorecord_argv;
            argc = 2;
        } else if (strcmp(spec, "kbdtest") == 0 || strcmp(spec, "kbd-selftest") == 0) {
            path = "/bin/kbdtest";
            argv = kbdtest_argv;
            argc = 2;
        } else if (strcmp(spec, "mousetest") == 0 || strcmp(spec, "mousetest --selftest") == 0) {
            /* El gemelo del kbdtest para el puntero: mismo patron (sesion
             * grafica, READY, el host mueve el device por QMP, checkpoints),
             * distinto device. */
            path = "/bin/mousetest";
            argv = mousetest_argv;
            argc = 2;
        } else if (text_starts_with(spec, "tcptest ")) {
            /* El puerto no se puede hornear en el binario: lo elige el host al
             * levantar tools/tcp_echo_server.py. */
            const char* port = skip_spaces(spec + strlen("tcptest"));
            if (port[0] == '\0') {
                printf("%s FAIL missing port\n", label);
                return 1;
            }
            path = "/bin/tcptest";
            tcptest_argv[1] = port;
            argv = tcptest_argv;
            argc = 2;
        } else if (text_starts_with(spec, "ntptest ")) {
            /* Igual que tcptest: el puerto lo elige el host al levantar
             * tools/ntp_server.py y llega sustituido en el spec. */
            const char* port = skip_spaces(spec + strlen("ntptest"));
            if (port[0] == '\0') {
                printf("%s FAIL missing port\n", label);
                return 1;
            }
            path = "/bin/ntptest";
            ntptest_argv[1] = port;
            argv = ntptest_argv;
            argc = 2;
        } else if (text_starts_with(spec, "gputest --soak ")) {
            const char* iterations = skip_spaces(spec + strlen("gputest --soak"));
            if (iterations[0] == '\0') {
                printf("%s FAIL missing soak iteration count\n", label);
                return 1;
            }
            path = "/bin/gputest";
            soak_argv[2] = iterations;
            argv = soak_argv;
            argc = 3;
        } else {
            printf("%s FAIL unknown runner '%s'\n", label, spec);
            return 1;
        }
    }

    long runner_fd = savanxp_open(path);
    if (runner_fd < 0) {
        printf("%s FAIL missing runner %s (%s)\n", label, path, result_error_string(runner_fd));
        return 1;
    }
    savanxp_close((int)runner_fd);

    long pid = spawn(path, argv, argc);
    if (pid < 0) {
        printf("%s FAIL spawn %s (%s)\n", label, path, result_error_string(pid));
        return 1;
    }

    savanxp_waitpid((int)pid, &status);
    printf("init: %s runner exited with %d\n", label, status);
    if (status == 0) {
        printf("%s PASS\n", label);
    } else {
        printf("%s FAIL status=%d\n", label, status);
    }
    return status;
}

#define KEYBOARD_LAYOUT_CONFIG_NAME "keyboard.cfg"
#define AUDIO_CONFIG_NAME "audio.cfg"
#define POINTER_CONFIG_NAME "mouse.cfg"

/* The one user (docs/kernel/USERS_AND_PERMISSIONS.md): its profile, made at
 * boot if missing, the user's own and nobody else's (0700). */
static const struct passwd* the_user(void) {
    return getpwnam("user");
}

static void ensure_user_profile(void) {
    const struct passwd* user = the_user();
    char settings[256];

    if (user == 0) {
        return;
    }
    (void)mkdir("/users", 0755);
    if (mkdir(user->pw_dir, 0700) == 0) {
        (void)chown(user->pw_dir, user->pw_uid, user->pw_gid);
        printf("init: made the profile %s\n", user->pw_dir);
    }
    snprintf(settings, sizeof(settings), "%s/%s", user->pw_dir, SAVANXP_SETTINGS_DIR);
    if (mkdir(settings, 0700) == 0) {
        (void)chown(settings, user->pw_uid, user->pw_gid);
    }
}

/* A setting of the user's, which init applies before anybody logs in, looked
 * up as savanxp_settings_path does for the user (init is the system, so it
 * cannot ask it): Settings, then loose in the profile, then the root. Moving
 * a loose copy is left to the user's own programs. */
static long open_user_setting(const char* name) {
    const struct passwd* user = the_user();
    char path[256];
    long fd = -1;

    if (user != 0) {
        snprintf(path, sizeof(path), "%s/%s/%s", user->pw_dir, SAVANXP_SETTINGS_DIR, name);
        fd = savanxp_open(path);
        if (fd < 0) {
            snprintf(path, sizeof(path), "%s/%s", user->pw_dir, name);
            fd = savanxp_open(path);
        }
    }
    if (fd < 0) {
        snprintf(path, sizeof(path), "/%s", name);
        fd = savanxp_open(path);
    }
    return fd;
}

/* The preferred layout (one ASCII digit, the pattern desktop.cfg uses), set
 * BEFORE windowd starts so it is active for the first key. Through
 * /dev/keyboard, which has the layout and none of the keys. */
static void apply_keyboard_layout_preference(void) {
    char digit = 0;
    long config_fd = open_user_setting(KEYBOARD_LAYOUT_CONFIG_NAME);
    long input_fd;

    if (config_fd < 0) {
        return;
    }
    if (savanxp_read((int)config_fd, &digit, 1) != 1) {
        savanxp_close((int)config_fd);
        return;
    }
    savanxp_close((int)config_fd);
    if (digit != '0' + SAVANXP_KEYBOARD_LAYOUT_EN) {
        return;
    }

    input_fd = savanxp_open_mode("/dev/keyboard", SAVANXP_OPEN_READ | SAVANXP_OPEN_WRITE);
    if (input_fd < 0) {
        return;
    }
    (void)input_set_layout((int)input_fd, SAVANXP_KEYBOARD_LAYOUT_EN);
    savanxp_close((int)input_fd);
}

/* The preferred pointer speed (the decimal speed, "10"; Mouse Properties
 * writes it), set before windowd starts so the first move already uses it.
 * Through /dev/pointer, which has the speed and none of the events. With no
 * file or a broken one the speed stays SAVANXP_POINTER_SPEED_DEFAULT. */
static void apply_pointer_speed_preference(void) {
    char text[8];
    long config_fd = open_user_setting(POINTER_CONFIG_NAME);
    long pointer_fd;
    long bytes_read;
    int speed = 0;
    size_t index = 0;

    if (config_fd < 0) {
        return;
    }
    bytes_read = savanxp_read((int)config_fd, text, sizeof(text) - 1);
    savanxp_close((int)config_fd);
    if (bytes_read <= 0) {
        return;
    }
    text[bytes_read] = '\0';
    while (text[index] >= '0' && text[index] <= '9' && speed <= SAVANXP_POINTER_SPEED_MAX) {
        speed = speed * 10 + (text[index] - '0');
        ++index;
    }
    while (is_space_char(text[index])) {
        ++index;
    }
    if (index == 0 || text[index] != '\0' ||
        speed < SAVANXP_POINTER_SPEED_MIN || speed > SAVANXP_POINTER_SPEED_MAX) {
        return;
    }

    pointer_fd = savanxp_open_mode("/dev/pointer", SAVANXP_OPEN_READ | SAVANXP_OPEN_WRITE);
    if (pointer_fd < 0) {
        return;
    }
    if (input_set_pointer_speed((int)pointer_fd, speed) == 0) {
        printf("init: pointer speed %d\n", speed);
    }
    savanxp_close((int)pointer_fd);
}

/* The preferred volume (keyboard.cfg's pattern with two numbers: "75 0" is
 * volume 75, not muted), set BEFORE windowd starts so the first sound already
 * plays at the saved level. With no file or a broken one nothing changes: the
 * kernel starts at 100, not muted. */
static void apply_audio_preference(void) {
    char text[16] = {0};
    long config_fd = open_user_setting(AUDIO_CONFIG_NAME);
    long audio_fd;
    long bytes_read;
    unsigned int volume = 0;
    unsigned int muted = 0;
    size_t index = 0;

    if (config_fd < 0) {
        return;
    }
    bytes_read = savanxp_read((int)config_fd, text, sizeof(text) - 1);
    savanxp_close((int)config_fd);
    if (bytes_read <= 0 || bytes_read >= (long)sizeof(text)) {
        return;
    }
    text[bytes_read] = '\0';

    while (is_space_char(text[index])) {
        ++index;
    }
    if (text[index] < '0' || text[index] > '9') {
        return;
    }
    while (text[index] >= '0' && text[index] <= '9') {
        volume = volume * 10u + (unsigned int)(text[index] - '0');
        if (volume > 100u) {
            return;
        }
        ++index;
    }
    if (!is_space_char(text[index])) {
        return;
    }
    while (is_space_char(text[index])) {
        ++index;
    }
    if (text[index] != '0' && text[index] != '1') {
        return;
    }
    muted = (unsigned int)(text[index] - '0');
    ++index;
    while (text[index] != '\0') {
        if (!is_space_char(text[index])) {
            return;
        }
        ++index;
    }

    /* Solo para los ioctl: el volumen vive en el kernel y se lee o fija sin
     * ser dueno del dispositivo, asi que abrir aca no le quita el audio a
     * nadie. */
    audio_fd = savanxp_open_mode("/dev/audio0", SAVANXP_OPEN_READ | SAVANXP_OPEN_WRITE);
    if (audio_fd < 0) {
        return;
    }
    (void)audio_set_volume((int)audio_fd, (int)volume);
    (void)audio_set_muted((int)audio_fd, (int)muted);
    savanxp_close((int)audio_fd);
}

/* --- the rescue system ------------------------------------------------------
 *
 * The kernel starts /rescue/bin/init when no disk mounted at "/"
 * (docs/kernel/FILESYSTEMS.md#the-rescue-system). There is no desktop to bring
 * up, no audiod and no /bin: init only keeps a shell from the initramfs alive
 * on the text console. */

#define RESCUE_ROOT "/rescue"
#define RESCUE_SHELL RESCUE_ROOT "/bin/sh"

static int started_from_rescue(int argc, char** argv) {
    return argc > 0 && argv != 0 && argv[0] != 0 && text_starts_with(argv[0], RESCUE_ROOT "/");
}

/* Runs a program and returns its exit status, or -1 if it did not start. A
 * failed check prints "rescue: FAIL", not the failure token: the runner stops
 * at the token, and the remaining checks still have something to say. */
static int rescue_run(const char* path, const char* const* argv, int argc) {
    int status = 0;
    long pid = spawn(path, argv, argc);
    if (pid < 0) {
        printf("rescue: FAIL spawn %s (%s)\n", path, result_error_string(pid));
        return -1;
    }
    savanxp_waitpid((int)pid, &status);
    return status;
}

static int rescue_expect(const char* path, const char* const* argv, int argc, int expected, const char* what) {
    const int status = rescue_run(path, argv, argc);
    if (status != expected) {
        printf("rescue: FAIL %s: %s exited with %d, expected %d\n", what, path, status, expected);
        return 0;
    }
    printf("rescue: ok %s\n", what);
    return 1;
}

/* What the rescue system has to be able to do with no disk at all. */
static int rescue_selftest(void) {
    const char* ls_argv[] = {RESCUE_ROOT "/bin/ls", RESCUE_ROOT "/bin", 0};
    const char* cat_argv[] = {RESCUE_ROOT "/bin/cat", RESCUE_ROOT "/README", 0};
    // Neither command is a path: sh has to find both in /rescue/bin, with no /bin.
    const char* sh_argv[] = {RESCUE_SHELL, "-c", "echo rescue-shell | cat", 0};
    const char* df_argv[] = {RESCUE_ROOT "/bin/df", 0};
    const char* imagetest_argv[] = {RESCUE_ROOT "/bin/imagetest", 0};
    int ok = 1;

    long disk_init = savanxp_open("/bin/init");
    if (disk_init >= 0) {
        savanxp_close((int)disk_init);
        puts_out("RESCUE SMOKE FAIL /bin/init exists: this boot has a disk\n");
        return 1;
    }
    ok = rescue_expect(ls_argv[0], ls_argv, 2, 0, "ls lists /rescue/bin") && ok;
    ok = rescue_expect(cat_argv[0], cat_argv, 2, 0, "cat reads /rescue/README") && ok;
    ok = rescue_expect(sh_argv[0], sh_argv, 3, 0, "sh finds commands in /rescue/bin") && ok;
    ok = rescue_expect(df_argv[0], df_argv, 1, 1, "df reports no root volume") && ok;
    ok = rescue_expect(imagetest_argv[0], imagetest_argv, 1, 0, "the in-memory executable reader") && ok;
    puts_out(ok ? "RESCUE SMOKE PASS\n" : "RESCUE SMOKE FAIL\n");
    return ok ? 0 : 1;
}

__attribute__((noreturn)) static void run_rescue_system(void) {
    const char* shell_argv[] = {RESCUE_SHELL, 0};

    long smoke_trigger = savanxp_open(RESCUE_ROOT "/SMOKE");
    if (smoke_trigger >= 0) {
        char automation_spec[64] = {};
        long bytes_read = savanxp_read((int)smoke_trigger, automation_spec, sizeof(automation_spec) - 1);
        savanxp_close((int)smoke_trigger);
        if (bytes_read < 0) {
            automation_spec[0] = '\0';
        }
        trim_automation_spec(automation_spec);
        if (strcmp(automation_spec, "rescue") == 0) {
            (void)rescue_selftest();
        } else {
            printf("RESCUE SMOKE FAIL '%s' needs the disk; this boot has none\n", automation_spec);
        }
        for (;;) {
            sleep_ms(1000);
        }
    }

    puts_out("init: no system disk mounted; starting the rescue shell\n");
    for (;;) {
        int status = 0;
        long pid = spawn(RESCUE_SHELL, shell_argv, 1);
        if (pid < 0) {
            printf("init: failed to spawn %s (%s)\n", RESCUE_SHELL, result_error_string(pid));
            sleep_ms(1000);
            continue;
        }
        savanxp_waitpid((int)pid, &status);
        printf("init: the rescue shell exited with %d, starting it again\n", status);
        sleep_ms(250);
    }
}

/* --- Shutdown (docs/kernel/POWER.md#ending-the-session) -----------------------
 *
 * init owns the shutdown, as Windows' winlogon does: whoever wants one asks on
 * init's port, and init ends the session in order before it touches
 * /dev/power. A second thread serves the port while the first stays in
 * waitpid. While a shutdown is under way the first restarts nothing, so
 * nothing init stops comes back. */

/* What init started and still watches, for the shutdown thread to stop. */
static volatile long g_audiod_pid = 0;
static volatile long g_elevationd_pid = 0;
static volatile long g_sessiond_pid = 0;
static volatile long g_shell_pid = 0;
/* The write end of each service's control channel, or -1
 * (docs/kernel/POWER.md#services). */
static volatile long g_audiod_control = -1;
static volatile long g_elevationd_control = -1;
/* The savanxp_shutdown_action under way, or 0. */
static volatile int g_shutting_down = 0;
/* /dev/power opened for reading, which the power button's presses come
 * through once init watches it (docs/kernel/POWER.md#the-power-button), or -1
 * when the kernel powers off on the button by itself. */
static long g_button_fd = -1;

/* How long a requester gets to read the last reply and hang up before the
 * power goes, and how long a request may take to arrive after connecting. */
#define INIT_SHUTDOWN_GOODBYE_MS 2000
#define INIT_SHUTDOWN_REQUEST_MS 2000
/* How long the services get to stop on their own, all together, before init
 * ends them: Windows' WaitToKillServiceTimeout, shorter. */
#define INIT_SERVICE_STOP_MS 5000UL

/* Starts a service as Windows' service control manager does: with
 * SAVANXP_SERVICE_ARGUMENT and the read end of a control pipe as its standard
 * input (SAVANXP_SERVICE_CONTROL_HANDLE). A POSIX spawn passes a child its
 * stdio and nothing else, so stdin is the channel. init keeps the write end in
 * `control`, which replaces the one of the instance before. Without a pipe
 * the service still starts, only without being asked anything. */
static long spawn_service(const char* path, volatile long* control) {
    const char* service_argv[] = {path, SAVANXP_SERVICE_ARGUMENT, 0};
    const char* plain_argv[] = {path, 0};
    int channel[2] = {-1, -1};
    long pid;

    if (*control >= 0) {
        (void)savanxp_close((int)*control);
        *control = -1;
    }
    if (savanxp_pipe(channel) < 0) {
        return spawn(path, plain_argv, 1);
    }
    pid = spawn_fd(path, service_argv, 2, channel[0], 1);
    (void)savanxp_close(channel[0]);
    if (pid < 0) {
        (void)savanxp_close(channel[1]);
    } else {
        *control = channel[1];
    }
    return pid;
}

/* Whether `pid` still runs: present and not a zombie the supervisor has yet
 * to reap. */
static int service_running(long pid) {
    struct savanxp_process_info process;
    unsigned long index;

    for (index = 0; pid > 0 && proc_info(index, &process) > 0; ++index) {
        if ((long)process.pid == pid) {
            return process.state != SAVANXP_PROC_ZOMBIE;
        }
    }
    return 0;
}

static void shutdown_reply(int32_t write_handle, int32_t status, uint32_t stage) {
    struct savanxp_shutdown_reply reply;

    memset(&reply, 0, sizeof(reply));
    reply.status = status;
    reply.stage = stage;
    (void)sxn_write(write_handle, &reply, sizeof(reply));
}

static int serve_shutdown_request(struct sxn_port_connection* connection, long port);

/* Takes a press off /dev/power. 1 if it was one. */
static int take_button_press(void) {
    struct savanxp_power_event event;

    return g_button_fd >= 0 && savanxp_read((int)g_button_fd, &event, sizeof(event)) == (long)sizeof(event) &&
        event.kind == SAVANXP_POWER_EVENT_BUTTON;
}

/* Asks sessiond to end the session and waits for its answer, however long the
 * user takes with "save changes?". Requests that arrive meanwhile are answered
 * (-EBUSY), not left waiting. 0 when it ended, or there was none to end;
 * -ECANCELED when an application turned its close down. */
static int end_session(long port) {
    int32_t handles[2] = {-1, -1};
    struct savanxp_shutdown_request request;
    struct savanxp_shutdown_reply reply;
    long got;

    if (g_sessiond_pid <= 0 || sxn_port_connect(SAVANXP_SESSION_PORT_NAME, handles) < 0) {
        return 0;
    }
    memset(&request, 0, sizeof(request));
    request.magic = SAVANXP_SHUTDOWN_MAGIC;
    request.action = SAVANXP_SHUTDOWN_END_SESSION;
    got = sxn_write(handles[0], &request, sizeof(request));
    while (got == (long)sizeof(request)) {
        int32_t waits[3] = {handles[1], (int32_t)port, (int32_t)g_button_fd};
        struct sxn_port_connection other;
        long ready = sxn_wait_many(waits, g_button_fd >= 0 ? 3 : 2, -1);

        if (ready == 0) {
            got = sxn_read(handles[1], &reply, sizeof(reply));
            break;
        }
        if (ready == 2 && take_button_press()) {
            /* As in Windows: the button does not hurry a shutdown already
             * waiting on the user. */
            printf("init: the power button was pressed during a shutdown; nothing to add\n");
        }
        while (sxn_port_accept(port, &other) == 0) {
            (void)serve_shutdown_request(&other, port);
            (void)sxn_close(other.read_handle);
            (void)sxn_close(other.write_handle);
        }
    }
    (void)sxn_close(handles[0]);
    (void)sxn_close(handles[1]);
    /* A sessiond that went away has no session left to end. */
    if (got != (long)sizeof(reply)) {
        return 0;
    }
    return reply.status == -SAVANXP_ECANCELED ? -SAVANXP_ECANCELED : 0;
}

static void stop_process(volatile long* pid) {
    if (*pid > 0) {
        (void)savanxp_kill((int)*pid, SAVANXP_SIGKILL);
    }
}

/* Asks every service to stop (SAVANXP_SERVICE_CONTROL_SHUTDOWN), gives them
 * INIT_SERVICE_STOP_MS together, and ends the ones still running. */
static void stop_services(void) {
    struct {
        const char* name;
        volatile long* pid;
        volatile long* control;
        long asked_pid;
    } services[] = {
        {"audiod", &g_audiod_pid, &g_audiod_control, 0},
        {"elevationd", &g_elevationd_pid, &g_elevationd_control, 0},
    };
    const unsigned count = sizeof(services) / sizeof(services[0]);
    struct savanxp_service_control request;
    unsigned long deadline = uptime_ms() + INIT_SERVICE_STOP_MS;
    unsigned index;

    memset(&request, 0, sizeof(request));
    request.magic = SAVANXP_SERVICE_CONTROL_MAGIC;
    request.control = SAVANXP_SERVICE_CONTROL_SHUTDOWN;
    for (index = 0; index < count; ++index) {
        /* The supervisor zeroes the pid when it reaps: keep the one asked. */
        long pid = *services[index].pid;

        if (pid > 0 && *services[index].control >= 0 &&
            savanxp_write((int)*services[index].control, &request, sizeof(request)) == (long)sizeof(request)) {
            services[index].asked_pid = pid;
            printf("init: asking %s to stop\n", services[index].name);
        }
    }
    for (;;) {
        int waiting = 0;

        for (index = 0; index < count; ++index) {
            waiting |= service_running(services[index].asked_pid);
        }
        if (!waiting || uptime_ms() >= deadline) {
            break;
        }
        sleep_ms(50);
    }
    for (index = 0; index < count; ++index) {
        if (services[index].asked_pid > 0 && !service_running(services[index].asked_pid)) {
            printf("init: %s stopped\n", services[index].name);
            continue;
        }
        if (services[index].asked_pid > 0) {
            printf("init: %s did not stop within %lu s; ending it\n", services[index].name,
                   INIT_SERVICE_STOP_MS / 1000UL);
        }
        stop_process(services[index].pid);
    }
}

/* /dev/power itself: the kernel syncs the volumes and cuts the power. */
static void power_device(int action) {
    long fd = savanxp_open_mode("/dev/power", SAVANXP_OPEN_WRITE);
    long result = fd;

    if (fd >= 0) {
        result = savanxp_ioctl((int)fd, action == SAVANXP_SHUTDOWN_REBOOT ? POWER_IOC_REBOOT : POWER_IOC_SHUTDOWN, 0);
        savanxp_close((int)fd);
    }
    printf("init: /dev/power did not %s (%s)\n", action == SAVANXP_SHUTDOWN_REBOOT ? "restart" : "power off",
           result_error_string(result));
}

/* Ends the session and stops the services. Returns 1 if the machine should go
 * down now; -ECANCELED when an application turned its close down. `requester`
 * is the write handle of whoever asked, or -1 for the power button. */
static int run_shutdown(int action, long port, int32_t requester) {
    int status = end_session(port);

    if (status == -SAVANXP_ECANCELED) {
        printf("init: an application cancelled the %s\n", action == SAVANXP_SHUTDOWN_REBOOT ? "restart" : "shutdown");
        g_shutting_down = 0;
        if (requester >= 0) {
            shutdown_reply(requester, status, SAVANXP_SHUTDOWN_STAGE_DONE);
        }
        return status;
    }

    /* The session is over; the services hold nothing the disk still needs. */
    stop_process(&g_sessiond_pid);
    stop_process(&g_shell_pid);
    stop_services();
    printf("init: session ended and services stopped; %s\n",
           action == SAVANXP_SHUTDOWN_REBOOT ? "restarting" : "powering off");
    if (requester >= 0) {
        shutdown_reply(requester, 0, SAVANXP_SHUTDOWN_STAGE_DONE);
    }
    return 1;
}

/* One request, start to end. Returns 1 if the machine should go down now. */
static int serve_shutdown_request(struct sxn_port_connection* connection, long port) {
    struct savanxp_shutdown_request request;
    int32_t ready = connection->read_handle;
    int action;

    if (sxn_wait_many(&ready, 1, INIT_SHUTDOWN_REQUEST_MS) != 0 ||
        sxn_read(connection->read_handle, &request, sizeof(request)) != (long)sizeof(request) ||
        request.magic != SAVANXP_SHUTDOWN_MAGIC ||
        (request.action != SAVANXP_SHUTDOWN_POWER_OFF && request.action != SAVANXP_SHUTDOWN_REBOOT)) {
        shutdown_reply(connection->write_handle, -SAVANXP_EINVAL, SAVANXP_SHUTDOWN_STAGE_ACCEPTED);
        return 0;
    }
    if (g_shutting_down != 0) {
        shutdown_reply(connection->write_handle, -SAVANXP_EBUSY, SAVANXP_SHUTDOWN_STAGE_ACCEPTED);
        return 0;
    }
    action = (int)request.action;
    g_shutting_down = action;
    shutdown_reply(connection->write_handle, 0, SAVANXP_SHUTDOWN_STAGE_ACCEPTED);
    printf("init: %s requested by pid %u; ending the session\n",
           action == SAVANXP_SHUTDOWN_REBOOT ? "restart" : "shutdown", (unsigned)connection->client_pid);

    if (run_shutdown(action, port, connection->write_handle) != 1) {
        return 0;
    }
    /* The requester may want to say something before the power goes: give it
     * until it hangs up, or a moment. */
    (void)sxn_wait_many(&ready, 1, INIT_SHUTDOWN_GOODBYE_MS);
    return 1;
}

/* Asks the kernel to bring the power button here instead of powering off at
 * once. Without it (an older kernel, no /dev/power) the button still powers
 * off, only without ending the session. */
static void watch_power_button(void) {
    long fd = savanxp_open_mode("/dev/power", SAVANXP_OPEN_READ);

    if (fd >= 0 && savanxp_ioctl((int)fd, POWER_IOC_WATCH_BUTTON, 0) == 0) {
        g_button_fd = fd;
        return;
    }
    if (fd >= 0) {
        savanxp_close((int)fd);
    }
    printf("init: cannot watch the power button; it powers off without ending the session\n");
}

/* The power did not go. Nothing is left running to go back to, so stay down. */
static void go_down(int action) {
    power_device(action);
    for (;;) {
        sleep_ms(1000);
    }
}

static void* shutdown_service(void* argument) {
    long port = (long)argument;

    /* Here, and not before the thread exists: a press nobody reads would make
     * the kernel wait for a second one before it powered off. */
    watch_power_button();
    for (;;) {
        struct sxn_port_connection connection;
        int32_t waits[2] = {(int32_t)port, (int32_t)g_button_fd};

        if (sxn_wait_many(waits, g_button_fd >= 0 ? 2 : 1, -1) == 1) {
            /* The power button: a shutdown nobody waits on, as Windows makes
             * of it. A press during one is taken in end_session(). */
            if (take_button_press() && g_shutting_down == 0) {
                g_shutting_down = SAVANXP_SHUTDOWN_POWER_OFF;
                printf("init: shutdown requested by the power button; ending the session\n");
                if (run_shutdown(SAVANXP_SHUTDOWN_POWER_OFF, port, -1) == 1) {
                    go_down(SAVANXP_SHUTDOWN_POWER_OFF);
                }
            }
            continue;
        }
        while (sxn_port_accept(port, &connection) == 0) {
            int go = serve_shutdown_request(&connection, port);

            (void)sxn_close(connection.read_handle);
            (void)sxn_close(connection.write_handle);
            if (go) {
                go_down(g_shutting_down);
            }
        }
    }
    return 0;
}


/* Opens init's port before the session starts, so a desktop always finds it. */
static void start_shutdown_service(void) {
    static pthread_t thread;
    long port = sxn_port_create(SAVANXP_SHUTDOWN_PORT_NAME);

    if (port < 0) {
        printf("init: cannot serve %s; programs fall back to /dev/power\n", SAVANXP_SHUTDOWN_PORT_NAME);
        return;
    }
    if (pthread_create(&thread, 0, shutdown_service, (void*)port) != 0) {
        printf("init: cannot serve %s; programs fall back to /dev/power\n", SAVANXP_SHUTDOWN_PORT_NAME);
    }
}

/* The system as it runs normally: services, the shutdown port, the session,
 * and the supervisor that restarts them. The graphical session belongs to
 * sessiond (docs/desktop/DESKTOP_ARCHITECTURE.md#the-session-manager-
 * sessiond); init keeps audiod, elevationd and the fallback to the text
 * shell. A smoke scenario that needs the real
 * session passes the program that drives it (`driver`), started once the
 * session is. Never returns. */
__attribute__((noreturn)) static void run_system(const char* const* driver);

int main(int argc, char** argv) {
    if (started_from_rescue(argc, argv)) {
        run_rescue_system();
    }

    /* The smoke trigger is staged into the initramfs, never onto the disk, so a
     * smoke build leaves the persistent image alone. */
    long smoke_trigger = savanxp_open("/rescue/SMOKE");
    if (smoke_trigger >= 0) {
        char automation_spec[64] = {};
        long bytes_read = savanxp_read((int)smoke_trigger, automation_spec, sizeof(automation_spec) - 1);
        savanxp_close((int)smoke_trigger);
        if (bytes_read < 0) {
            automation_spec[0] = '\0';
        }
        trim_automation_spec(automation_spec);
        if (automation_spec[0] == '\0') {
            memcpy(automation_spec, "smoke", sizeof("smoke"));
        }
        if (strcmp(automation_spec, "nativesessionend") == 0) {
            /* The real session, its shutdown included, driven from inside. */
            static const char* const driver[] = {"/bin/nativetest.sxe", "session-end", 0};
            run_system(driver);
        }
        if (strcmp(automation_spec, "nativebutton") == 0) {
            /* The real session again, shut down by the power button
             * (docs/kernel/POWER.md#the-power-button). */
            static const char* const driver[] = {"/bin/nativetest.sxe", "power-button", 0};
            run_system(driver);
        }
        (void)run_automation_spec(automation_spec);
        for (;;) {
            sleep_ms(1000);
        }
    }

    run_system(0);
}

__attribute__((noreturn)) static void run_system(const char* const* driver) {
    const char* sessiond_argv[] = {"/system/sessiond.sxe", 0};
    const char* shell_argv[] = {"/bin/sh", 0};
    unsigned long last_sessiond_start_ms = 0;
    unsigned long elevationd_started_ms = 0;
    int rapid_failures = 0;
    int driver_started = 0;

    ensure_user_profile();
    apply_keyboard_layout_preference();
    apply_pointer_speed_preference();
    apply_audio_preference();
    start_shutdown_service();

    /* The audio daemon before windowd, so it always wins the race for
     * /dev/audio0 and clients find it already listening. Without a network or
     * audio device it leaves on its own and everything plays directly; the
     * supervisor below restarts it only if it dies while serving, not when it
     * leaves on purpose. */
    g_audiod_pid = spawn_service("/bin/audiod", &g_audiod_control);
    if (g_audiod_pid < 0) {
        printf("init: failed to spawn audiod (%s)\n", result_error_string(g_audiod_pid));
        g_audiod_pid = 0;
    }

    /* The elevation broker before the session (docs/kernel/
     * USERS_AND_PERMISSIONS.md#elevation): its port name is taken before any
     * program could take it. It is restarted if it dies, unless it dies at
     * once, which is a broken binary or a name already taken. */
    g_elevationd_pid = spawn_service("/system/elevationd.sxe", &g_elevationd_control);
    elevationd_started_ms = uptime_ms();
    if (g_elevationd_pid < 0) {
        printf("init: failed to spawn elevationd (%s)\n", result_error_string(g_elevationd_pid));
        g_elevationd_pid = 0;
    }

    for (;;) {
        int status = 0;
        unsigned long runtime_ms = 0;

        if (g_sessiond_pid <= 0 && g_shutting_down == 0) {
            g_sessiond_pid = spawn("/system/sessiond.sxe", sessiond_argv, 1);
            if (g_sessiond_pid < 0) {
                printf("init: failed to spawn sessiond (%s)\n", result_error_string(g_sessiond_pid));
                g_sessiond_pid = 0;
                sleep_ms(1000);
            } else {
                last_sessiond_start_ms = uptime_ms();
            }
        }
        if (driver != 0 && !driver_started && g_sessiond_pid > 0) {
            long driver_pid = spawn(driver[0], driver, 2);

            driver_started = 1;
            if (driver_pid < 0) {
                printf("init: failed to spawn %s (%s)\n", driver[0], result_error_string(driver_pid));
            }
        }

        if (g_audiod_pid <= 0 && g_sessiond_pid <= 0) {
            sleep_ms(1000);
            continue;
        }

        long pid = savanxp_waitpid(-1, &status);
        if (pid < 0) {
            /* No child to reap yet (a race): try again in a moment. */
            sleep_ms(250);
            continue;
        }

        if (g_shutting_down != 0) {
            /* The shutdown thread is stopping these: reap, restart nothing. */
            if (pid == g_audiod_pid) {
                g_audiod_pid = 0;
            } else if (pid == g_elevationd_pid) {
                g_elevationd_pid = 0;
            } else if (pid == g_sessiond_pid) {
                g_sessiond_pid = 0;
            }
            continue;
        }

        if (pid == g_audiod_pid && g_audiod_pid != 0) {
            g_audiod_pid = 0;
            if (status == 0) {
                /* It left on purpose (no network, no audio, another daemon):
                 * the system plays directly, one program at a time; no point
                 * insisting. */
                printf("init: audiod left without mixing (status 0)\n");
            } else {
                printf("init: audiod exited with %d, restarting\n", status);
                sleep_ms(250);
                g_audiod_pid = spawn_service("/bin/audiod", &g_audiod_control);
                if (g_audiod_pid < 0) {
                    printf("init: failed to spawn audiod (%s)\n", result_error_string(g_audiod_pid));
                    g_audiod_pid = 0;
                }
            }
            continue;
        }

        if (pid == g_elevationd_pid && g_elevationd_pid != 0) {
            g_elevationd_pid = 0;
            if (uptime_ms() - elevationd_started_ms < 2000UL) {
                printf("init: elevationd exited at once with %d; elevation is unavailable\n", status);
            } else {
                printf("init: elevationd exited with %d, restarting\n", status);
                g_elevationd_pid = spawn_service("/system/elevationd.sxe", &g_elevationd_control);
                elevationd_started_ms = uptime_ms();
                if (g_elevationd_pid < 0) {
                    printf("init: failed to spawn elevationd (%s)\n", result_error_string(g_elevationd_pid));
                    g_elevationd_pid = 0;
                }
            }
            continue;
        }

        if (pid == g_sessiond_pid && g_sessiond_pid != 0) {
            runtime_ms = uptime_ms() - last_sessiond_start_ms;
            printf("init: sessiond exited with %d, restarting\n", status);
            g_sessiond_pid = 0;

            if (status != 0 && runtime_ms < 2000UL) {
                rapid_failures += 1;
            } else {
                rapid_failures = 0;
            }

            /* SESSIOND_EXIT_GAVE_UP (3): it already retried windowd itself and
             * the session will not come up. */
            if (status == 3 || rapid_failures >= 3) {
                printf("init: the graphical session is unstable, falling back to /bin/sh\n");
                g_shell_pid = spawn("/bin/sh", shell_argv, 1);
                if (g_shell_pid < 0) {
                    printf("init: failed to spawn fallback shell (%s)\n", result_error_string(g_shell_pid));
                    g_shell_pid = 0;
                    sleep_ms(1000);
                } else {
                    int shell_status = 0;
                    savanxp_waitpid((int)g_shell_pid, &shell_status);
                    g_shell_pid = 0;
                    printf("init: fallback shell exited with %d, retrying the session\n", shell_status);
                }
                rapid_failures = 0;
            }

            sleep_ms(250);
            continue;
        }
        /* A child that is none of the three (an app reparented when its
         * launcher died, for instance): reaped and ignored. */
    }
}
