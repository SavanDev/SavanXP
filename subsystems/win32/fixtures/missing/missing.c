/*
 * missingdll.exe and missingsym.exe -- Fixtures whose load must fail. The
 * first imports from nosuch.dll, which is never installed; the second
 * imports a function kernel32.dll does not export. Built from one source,
 * linked against different import libraries.
 */
#include <windows.h>

__declspec(dllimport) unsigned WINAPI MissingFunction(void);

void WINAPI start(void) {
    ExitProcess(MissingFunction());
}
