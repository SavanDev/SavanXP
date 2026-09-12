#pragma once

#include <stdint.h>

#include "boot/boot_info.hpp"

namespace boot_screen {

void initialize(const boot::FramebufferInfo& framebuffer);
bool ready();

// Repinta el splash con el paso de arranque en curso. La primera llamada pinta
// el fondo, el logo y el nombre; las siguientes solo la barra y el estado.
void show(const char* status);

// Avanza la animacion de la barra hasta donde diga el reloj, y no hace nada si
// la fase no cambio. Un paso de arranque largo no vuelve a show() en todo ese
// rato, y sin esto la barra se queda clavada justo cuando el sistema mas parece
// colgado, asi que la llama todo lo que late durante el arranque: el tick del
// timer, cada linea de log, las esperas activas largas (calibrado del APIC) y
// el armado del namespace de uACPI. Un paso largo y mudo nuevo tiene que
// sumarse a esa lista o la barra se congela ahi.
void animate();

// El splash deja de ser dueño de la pantalla: alguien mas (la sesion grafica o
// un panico) va a escribir sobre el framebuffer y la animacion tiene que parar.
void finish();

} // namespace boot_screen
