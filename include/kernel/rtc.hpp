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

// The same clock in nanoseconds since 1970-01-01 UTC: the chip's second,
// carried by the monotonic clock's nanoseconds. 0 if the chip never gave a
// date.
uint64_t epoch_ns();

// Whether the date can be believed (docs/kernel/TIME.md, "Is the date
// right?"): what certificate dates are checked against. A date is valid when
// the machine gave one and it falls between kCivilFloor and kCivilCeiling.
enum CivilSource : uint32_t {
    kCivilSourceNone = 0, // the chip never gave a date
    kCivilSourceRtc = 1,  // read from the chip at boot, unconfirmed
    kCivilSourceSet = 2,  // set since boot (SET_REALTIME: date, ntpsync)
};
enum CivilReason : uint32_t {
    kCivilOk = 0,
    kCivilNoDate = 1,       // no date at all
    kCivilBeforeFloor = 2,  // earlier than this kernel's release: a dead battery or a reset chip
    kCivilAfterCeiling = 3, // past what the chip can hold
};
// 2026-11-01 00:00:00 UTC: no correct clock reads earlier than the tree was
// written. Moved forward at each release (docs/kernel/TIME.md).
constexpr uint64_t kCivilFloor = 1793491200ull;
// 2100-01-01 00:00:00 UTC: the chip's two-digit year ends at 2099.
constexpr uint64_t kCivilCeiling = 4102444800ull;

struct CivilStatus {
    bool valid;
    uint32_t source; // CivilSource
    uint32_t reason; // CivilReason
    uint64_t now;    // seconds since 1970 UTC; 0 when there is no date
};
void civil_status(CivilStatus& out);

} // namespace rtc
