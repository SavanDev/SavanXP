/*
 * tquiet.dll -- A DLL that asks for no thread notifications
 * (DisableThreadLibraryCalls) and notes any it gets anyway, as Q, in
 * tlog.dll's log, with its process attach as p.
 */
#include <windows.h>

DECLSPEC_IMPORT void WINAPI TlogNote(char letter);
DECLSPEC_IMPORT BOOL WINAPI DisableThreadLibraryCalls(HMODULE module);

__declspec(dllexport) unsigned WINAPI TquietValue(void) {
    return 3;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        TlogNote('p');
        DisableThreadLibraryCalls((HMODULE)instance);
    } else if (reason == DLL_THREAD_ATTACH || reason == DLL_THREAD_DETACH) {
        TlogNote('Q');
    }
    return TRUE;
}
