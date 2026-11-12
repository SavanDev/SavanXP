/*
 * beta.dll -- Imported by both initorder.exe and alpha.dll, and linked at
 * the same preferred base as alpha.dll, so one of the two is relocated.
 */
#include "say.h"

static const char marker[] = "beta";
/* Holds an absolute address: right only if relocations were applied. */
static const char *volatile marker_pointer = marker;

__declspec(dllexport) unsigned WINAPI BetaValue(void) {
    return 7;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)instance;
    if (reason == DLL_PROCESS_ATTACH) {
        say(reserved != NULL ? "beta: attach, static load" : "beta: attach, dynamic load");
        say(marker_pointer == marker ? ", pointers agree\r\n" : ", POINTERS DISAGREE\r\n");
    } else if (reason == DLL_PROCESS_DETACH) {
        say(reserved != NULL ? "beta: detach, process ending\r\n" : "beta: detach, unloaded\r\n");
    }
    return TRUE;
}
