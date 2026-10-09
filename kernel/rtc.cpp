#include "kernel/rtc.hpp"

#include <stdint.h>

#include "kernel/string.hpp"

namespace {

constexpr uint16_t kCmosAddressPort = 0x70;
constexpr uint16_t kCmosDataPort = 0x71;

constexpr uint8_t kRegisterSeconds = 0x00;
constexpr uint8_t kRegisterMinutes = 0x02;
constexpr uint8_t kRegisterHours = 0x04;
constexpr uint8_t kRegisterWeekday = 0x06;
constexpr uint8_t kRegisterDay = 0x07;
constexpr uint8_t kRegisterMonth = 0x08;
constexpr uint8_t kRegisterYear = 0x09;
constexpr uint8_t kRegisterStatusA = 0x0a;
constexpr uint8_t kRegisterStatusB = 0x0b;

struct RtcRaw {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint8_t year;
    uint8_t status_b;
};

inline void out8(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

inline uint8_t in8(uint16_t port) {
    uint8_t value = 0;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

uint8_t read_register(uint8_t reg) {
    out8(kCmosAddressPort, static_cast<uint8_t>(reg | 0x80u));
    return in8(kCmosDataPort);
}

void write_register(uint8_t reg, uint8_t value) {
    out8(kCmosAddressPort, static_cast<uint8_t>(reg | 0x80u));
    out8(kCmosDataPort, value);
}

bool update_in_progress() {
    return (read_register(kRegisterStatusA) & 0x80u) != 0;
}

uint8_t bcd_to_binary(uint8_t value) {
    return static_cast<uint8_t>((value & 0x0fu) + (((value >> 4) & 0x0fu) * 10u));
}

uint8_t binary_to_bcd(uint8_t value) {
    return static_cast<uint8_t>(((value / 10u) << 4) | (value % 10u));
}

bool is_leap_year(uint16_t year) {
    return (year % 4u == 0 && year % 100u != 0) || (year % 400u == 0);
}

uint8_t days_in_month(uint16_t year, uint8_t month) {
    switch (month) {
        case 1:
        case 3:
        case 5:
        case 7:
        case 8:
        case 10:
        case 12:
            return 31;
        case 4:
        case 6:
        case 9:
        case 11:
            return 30;
        case 2:
            return is_leap_year(year) ? 29 : 28;
        default:
            return 0;
    }
}

// Dia de la semana 1..7 (1 = domingo, como lo espera el CMOS) por el algoritmo
// de dias-desde-lo-civil: 1970-01-01 fue jueves.
uint8_t weekday_for(uint16_t year, uint8_t month, uint8_t day) {
    int y = static_cast<int>(year);
    unsigned m = month;
    y -= m <= 2 ? 1 : 0;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + day - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    const int days = era * 146097 + static_cast<int>(doe) - 719468;
    int dow = (days + 4) % 7;
    if (dow < 0) {
        dow += 7;
    }
    return static_cast<uint8_t>(dow + 1);
}

void read_raw(RtcRaw& raw) {
    raw.second = read_register(kRegisterSeconds);
    raw.minute = read_register(kRegisterMinutes);
    raw.hour = read_register(kRegisterHours);
    raw.day = read_register(kRegisterDay);
    raw.month = read_register(kRegisterMonth);
    raw.year = read_register(kRegisterYear);
    raw.status_b = read_register(kRegisterStatusB);
    (void)read_register(kRegisterWeekday);
}

bool same_raw(const RtcRaw& left, const RtcRaw& right) {
    return left.second == right.second &&
        left.minute == right.minute &&
        left.hour == right.hour &&
        left.day == right.day &&
        left.month == right.month &&
        left.year == right.year &&
        left.status_b == right.status_b;
}

bool convert_raw(const RtcRaw& raw, savanxp_realtime& value) {
    uint8_t second = raw.second;
    uint8_t minute = raw.minute;
    uint8_t hour = raw.hour;
    uint8_t day = raw.day;
    uint8_t month = raw.month;
    uint8_t year = raw.year;

    const bool binary_mode = (raw.status_b & 0x04u) != 0;
    const bool hour_24 = (raw.status_b & 0x02u) != 0;

    if (!binary_mode) {
        second = bcd_to_binary(second);
        minute = bcd_to_binary(minute);
        day = bcd_to_binary(day);
        month = bcd_to_binary(month);
        year = bcd_to_binary(year);
        hour = static_cast<uint8_t>((hour & 0x80u) | bcd_to_binary(hour & 0x7fu));
    }

    if (!hour_24) {
        const bool pm = (hour & 0x80u) != 0;
        hour = static_cast<uint8_t>(hour & 0x7fu);
        if (hour == 12) {
            hour = pm ? 12 : 0;
        } else if (pm) {
            hour = static_cast<uint8_t>(hour + 12);
        }
    }

    if (second > 59 || minute > 59 || hour > 23 || day == 0 || day > 31 || month == 0 || month > 12) {
        return false;
    }

    memset(&value, 0, sizeof(value));
    value.year = static_cast<uint16_t>(2000u + year);
    value.month = month;
    value.day = day;
    value.hour = hour;
    value.minute = minute;
    value.second = second;
    value.valid = 1;
    return true;
}

} // namespace

namespace rtc {

bool read_time(savanxp_realtime* value) {
    if (value == nullptr) {
        return false;
    }

    RtcRaw first = {};
    RtcRaw second = {};

    for (uint32_t attempt = 0; attempt < 100000u; ++attempt) {
        if (update_in_progress()) {
            continue;
        }

        read_raw(first);
        if (update_in_progress()) {
            continue;
        }
        read_raw(second);
        if (!same_raw(first, second)) {
            continue;
        }
        return convert_raw(second, *value);
    }

    memset(value, 0, sizeof(*value));
    return false;
}

bool valid_time(const savanxp_realtime& value) {
    if (value.year < 2000u || value.year > 2099u) {
        return false;
    }
    if (value.month == 0 || value.month > 12) {
        return false;
    }
    if (value.day == 0 || value.day > days_in_month(value.year, value.month)) {
        return false;
    }
    if (value.hour > 23 || value.minute > 59 || value.second > 59) {
        return false;
    }
    return true;
}

bool write_time(const savanxp_realtime& value) {
    uint8_t hour;
    uint8_t pm;

    if (!valid_time(value)) {
        return false;
    }

    // Esperar a que termine un ciclo en curso: escribir encima de el se
    // pierde. Acotado para que un chip atascado falle en vez de colgar.
    for (uint32_t attempt = 0; attempt < 1000000u; ++attempt) {
        if (!update_in_progress()) {
            break;
        }
        if (attempt + 1u >= 1000000u) {
            return false;
        }
    }

    const uint8_t status_b = read_register(kRegisterStatusB);
    const bool binary_mode = (status_b & 0x04u) != 0;
    const bool hour_24 = (status_b & 0x02u) != 0;

    hour = value.hour;
    pm = 0;
    if (!hour_24) {
        // Inversa de convert_raw: 0 es 12 AM, 12 es 12 PM.
        if (hour == 0) {
            hour = 12;
        } else if (hour == 12) {
            pm = 0x80u;
        } else if (hour > 12) {
            hour = static_cast<uint8_t>(hour - 12u);
            pm = 0x80u;
        }
    }
    if (!binary_mode) {
        // El bit PM no es BCD: se codifica la hora y se le suma despues.
        hour = static_cast<uint8_t>(binary_to_bcd(hour) | pm);
    } else {
        hour = static_cast<uint8_t>(hour | pm);
    }

    // Congelar las actualizaciones (bit SET) para que los siete registros
    // queden de la misma marca; al limpiar vuelve a correr.
    write_register(kRegisterStatusB, static_cast<uint8_t>(status_b | 0x80u));
    if (binary_mode) {
        write_register(kRegisterSeconds, value.second);
        write_register(kRegisterMinutes, value.minute);
        write_register(kRegisterHours, hour);
        write_register(kRegisterDay, value.day);
        write_register(kRegisterMonth, value.month);
        write_register(kRegisterYear, static_cast<uint8_t>(value.year - 2000u));
        write_register(kRegisterWeekday, weekday_for(value.year, value.month, value.day));
    } else {
        write_register(kRegisterSeconds, binary_to_bcd(value.second));
        write_register(kRegisterMinutes, binary_to_bcd(value.minute));
        write_register(kRegisterHours, hour);
        write_register(kRegisterDay, binary_to_bcd(value.day));
        write_register(kRegisterMonth, binary_to_bcd(value.month));
        write_register(kRegisterYear, binary_to_bcd(static_cast<uint8_t>(value.year - 2000u)));
        write_register(kRegisterWeekday, binary_to_bcd(weekday_for(value.year, value.month, value.day)));
    }
    write_register(kRegisterStatusB, status_b);
    return true;
}

} // namespace rtc
