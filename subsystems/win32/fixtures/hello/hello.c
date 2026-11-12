/*
 * hello.exe -- The first W1 fixture. It writes one line to standard output
 * through an imported WriteFile and exits with status 42. The same file runs
 * on Windows XP, in the host harness and on SavanXP.
 */
#include <windows.h>

static const char message[] = "Hello from a PE32 executable.\r\n";

void WINAPI start(void) {
    DWORD written = 0;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    BOOL ok = WriteFile(out, message, sizeof(message) - 1, &written, NULL);
    ExitProcess(ok && written == sizeof(message) - 1 ? 42 : 1);
}
