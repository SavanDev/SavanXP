/*
 * tlog.dll -- threads.exe's log of the thread notifications (WP3). DllMain
 * notes each reason it is called with, and threads.exe's TLS callbacks and
 * tquiet.dll note theirs through TlogNote, all in one sequence that the
 * main thread reads with TlogTake at points where no other thread runs.
 *
 * Letters: A/D this DLL's DLL_THREAD_ATTACH/DETACH, P/X its process attach
 * and detach; the others are the callers'.
 */
#include <windows.h>

#define LOG_SIZE 256

static char g_log[LOG_SIZE];
static LONG g_length;

DECLSPEC_IMPORT LONG WINAPI InterlockedIncrement(LPLONG value);

__declspec(dllexport) void WINAPI TlogNote(char letter) {
    const LONG at = InterlockedIncrement(&g_length) - 1;
    if (at < LOG_SIZE - 1) {
        g_log[at] = letter;
    }
}

/* What was noted since the last take, NUL-terminated into out[LOG_SIZE]. */
__declspec(dllexport) void WINAPI TlogTake(char *out) {
    LONG n = g_length < LOG_SIZE - 1 ? g_length : LOG_SIZE - 1;
    for (LONG i = 0; i < n; ++i) {
        out[i] = g_log[i];
    }
    out[n] = 0;
    g_length = 0;
}

static void say(const char *text) {
    DWORD written;
    DWORD n = 0;
    while (text[n] != 0) {
        ++n;
    }
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, n, &written, NULL);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)instance;
    (void)reserved;
    static const char letters[4] = {'X', 'P', 'A', 'D'};
    TlogNote(letters[reason & 3u]);
    if (reason == DLL_PROCESS_DETACH) {
        /* Threads still running are gone without their detach. */
        char rest[LOG_SIZE];
        TlogTake(rest);
        say("\r\ntlog: process detach, last notes [");
        say(rest);
        say("]\r\n");
    }
    return TRUE;
}
