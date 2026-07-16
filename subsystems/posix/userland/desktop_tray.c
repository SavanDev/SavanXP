#include "desktop_tray.h"

/* Sin fuentes de notificacion por ahora: la capacidad es 1 solo para que el
 * array tenga un tipo completo; count reporta 0. */
static const struct desktop_tray_icon k_tray_icons[1] = {
    {DESKTOP_ICON_DESKTOP, ""},
};

int desktop_tray_icon_count(void)
{
    return 0;
}

const struct desktop_tray_icon *desktop_tray_icon_at(int index)
{
    if (index < 0 || index >= desktop_tray_icon_count())
    {
        return 0;
    }
    return &k_tray_icons[index];
}
