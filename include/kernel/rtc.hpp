#pragma once

#include "savanxp/syscall.h"

namespace rtc {

bool read_time(savanxp_realtime* value);
// Rango aceptable para escribir: el CMOS guarda el anio con dos digitos y la
// lectura suma 2000, asi que solo 2000..2099 da una vuelta completa.
bool valid_time(const savanxp_realtime& value);
// Escribe el RTC. Asume fecha valida (valid_time); devuelve falso si el chip
// no sale de su ciclo de actualizacion. No cambia el modo del chip (BCD o
// binario, 12 o 24 horas): codifica segun lo que haya.
bool write_time(const savanxp_realtime& value);

} // namespace rtc
