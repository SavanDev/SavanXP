#pragma once

namespace i386 {

/* Rebanada minima del paquete "PS/2 and the desktop": el controlador 8042 y el
 * teclado por IRQ 1. Los scancodes set 1 se vuelcan por serie; la decodifica
 * con layout y la entrega al userland vienen en las rebanadas siguientes (el
 * driver completo es kernel/ps2.cpp, que en x86_64 apoya su IRQ en el
 * IOAPIC y su salida en la consola de texto). */
namespace ps2_32 {

void initialize();

} // namespace ps2_32

} // namespace i386