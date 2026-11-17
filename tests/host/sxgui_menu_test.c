/* Real menu stepping: disabled actions and separators are not keyboard stops. */
#include <stdio.h>
#include "sxgfx/sxgui.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
int main(void)
{
    const struct sxgui_menu_item items[] = {
        {"Unavailable", 1, SXGUI_MENU_DISABLED}, {0, 0, 0},
        {"Open", 2, 0}, {"Edit", 3, SXGUI_MENU_DISABLED},
        {"Properties", 4, 0}, {0, 0, 0}, {"Unavailable", 5, SXGUI_MENU_DISABLED}
    };
    const struct sxgui_menu menu = {"Document", items, 7};
    const struct sxgui_menu_item unavailable[] = {{"Edit", 1, SXGUI_MENU_DISABLED}, {0, 0, 0}};
    const struct sxgui_menu disabled = {"Unavailable", unavailable, 2};
    const struct sxgui_menu empty = {"Empty", 0, 0};
    CHECK(sxgui_popup_menu_step(&menu, -1, 1) == 2);
    CHECK(sxgui_popup_menu_step(&menu, 2, 1) == 4);
    CHECK(sxgui_popup_menu_step(&menu, 4, 1) == 2);
    CHECK(sxgui_popup_menu_step(&menu, -1, -1) == 4);
    CHECK(sxgui_popup_menu_step(&menu, 2, -1) == 4);
    CHECK(sxgui_popup_menu_step(&menu, 4, -1) == 2);
    CHECK(sxgui_popup_menu_step(&disabled, -1, 1) == -1);
    CHECK(sxgui_popup_menu_step(&disabled, -1, -1) == -1);
    CHECK(sxgui_popup_menu_step(&empty, -1, 1) == -1);
    if (failures) return 1;
    puts("SXGUI MENU TEST PASS");
    return 0;
}
