/*
 * comctl32.dll -- InitCommonControlsEx, the one function of the W0 matrix.
 * Solitaire calls it and uses no common control: its status bar is a class
 * of its own. There is no class to register, so it checks its argument the
 * way Windows XP does and succeeds.
 */
#define SXW_BUILDING_COMCTL32
#include <windows.h>

BOOL WINAPI InitCommonControlsEx(const INITCOMMONCONTROLSEX *picce) {
    return picce != NULL && picce->dwSize == sizeof(INITCOMMONCONTROLSEX);
}
