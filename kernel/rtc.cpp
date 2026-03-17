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

bool update_in_progress() {
    return (read_register(kRegisterStatusA) & 0x80u) != 0;
}

uint8_t bcd_to_binary(uint8_t value) {
    return static_cast<uint8_t>((value & 0x0fu) + (((value >> 4) & 0x0fu) * 10u));
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

} // namespace rtc
