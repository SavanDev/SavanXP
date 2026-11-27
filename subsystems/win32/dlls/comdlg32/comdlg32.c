/*
 * comdlg32.dll -- The common dialogs. Today only what a program asks of the
 * printer: SavanXP has none (WIN32_WORDPAD_ROADMAP.md, D3), so PrintDlgW
 * finds no default printer, as Windows XP answers without one. The rest of
 * what programs import is declared (declared.txt).
 */
#include <windows.h>

#define CDERR_STRUCTSIZE 0x0001u
#define PDERR_NODEFAULTPRN 0x1008u

/* sizeof(PRINTDLGW) in commdlg.h, which packs it to one byte. */
#define PRINTDLGW_SIZE 66u

static DWORD g_error;

/* The error of the last dialog that failed; 0 after one that succeeded or
 * was cancelled. Per process here; Windows XP keeps it per thread. */
DWORD WINAPI CommDlgExtendedError(void) {
    return g_error;
}

/* No printer: neither PD_RETURNDEFAULT nor the dialog has one to give. The
 * dialog itself, which says so, comes with WP8. */
BOOL WINAPI PrintDlgW(void *lppd) {
    const DWORD *size = (const DWORD *)lppd;
    g_error = size == NULL || *size != PRINTDLGW_SIZE ? CDERR_STRUCTSIZE : PDERR_NODEFAULTPRN;
    return FALSE;
}
