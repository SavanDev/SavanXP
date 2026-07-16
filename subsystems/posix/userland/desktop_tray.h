#pragma once

#include "libc.h"
#include "desktop_icons.h"

/* Registro de iconos del area de notificaciones (system tray). Todavia no
 * existe ninguna fuente real de notificaciones, asi que la tabla esta vacia y
 * el area solo muestra el reloj; el layout y el render ya iteran este registro
 * para que sumar el primer icono sea solo agregar una entrada. */
struct desktop_tray_icon
{
    enum desktop_icon_id icon_id;
    const char *label;
};

int desktop_tray_icon_count(void);
const struct desktop_tray_icon *desktop_tray_icon_at(int index);
