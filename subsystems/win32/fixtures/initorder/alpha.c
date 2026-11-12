/*
 * alpha.dll -- Imports from beta.dll, so beta.dll is initialized first.
 */
#include "say.h"

__declspec(dllimport) unsigned WINAPI BetaValue(void);

static const char marker[] = "alpha";
static const char *volatile marker_pointer = marker;

__declspec(dllexport) unsigned WINAPI AlphaValue(void) {
    return 3;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)instance;
    if (reason == DLL_PROCESS_ATTACH) {
        say_digit("alpha: attach, beta says ", BetaValue(), "");
        say(marker_pointer == marker ? ", pointers agree\r\n" : ", POINTERS DISAGREE\r\n");
    } else if (reason == DLL_PROCESS_DETACH) {
        say(reserved != NULL ? "alpha: detach, process ending\r\n" : "alpha: detach, unloaded\r\n");
    }
    return TRUE;
}
