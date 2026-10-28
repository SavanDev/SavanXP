#pragma once

#include "abi/savanxp_abi.h"

namespace rtc {

bool read_time(savanxp_realtime* value);
// Rango aceptable para escribir: el CMOS guarda el anio con dos digitos y la
// lectura suma 2000, asi que solo 2000..2099 da una vuelta completa.
bool valid_time(const savanxp_realtime& value);
// Escribe el RTC. Asume fecha valida (valid_time); devuelve falso si el chip
// no sale de su ciclo de actualizacion. No cambia el modo del chip (BCD o
// binario, 12 o 24 horas): codifica segun lo que haya.
bool write_time(const savanxp_realtime& value);

// Seconds since 1970-01-01 UTC, for file times. The chip is read once, the
// first time it is asked, and from then on the monotonic clock carries it: a
// read waits out the chip's update cycle, which a write path cannot afford on
// every call. write_time() re-reads it. 0 if the chip never gave a date.
uint64_t epoch_seconds();

} // namespace rtc
