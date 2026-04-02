#include "kernel/pcspeaker.hpp"

#include <stdint.h>

#include "kernel/cpu.hpp"
#include "kernel/device.hpp"
#include "kernel/process.hpp"
#include "kernel/timer.hpp"
#include "savanxp/syscall.h"

namespace {

constexpr uint16_t kPitCommandPort = 0x43;
constexpr uint16_t kPitChannel2Port = 0x42;
constexpr uint16_t kSpeakerPort = 0x61;
constexpr uint32_t kPitBaseFrequency = 1193182;

device::Device g_device = {
    .name = "pcspk",
    .read = nullptr,
    .write = nullptr,
    .ioctl = nullptr,
    .close = nullptr,
};

bool g_ready = false;

inline void out8(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

inline uint8_t in8(uint16_t port) {
    uint8_t value = 0;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

int negative_error(savanxp_error_code code) {
    return -static_cast<int>(code);
}

void stop_tone() {
    out8(kSpeakerPort, static_cast<uint8_t>(in8(kSpeakerPort) & ~0x03u));
}

bool start_tone(uint32_t frequency_hz) {
    if (frequency_hz < 20 || frequency_hz > 20000) {
        return false;
    }

    const uint16_t divisor = static_cast<uint16_t>(kPitBaseFrequency / frequency_hz);
    if (divisor == 0) {
        return false;
    }

    out8(kPitCommandPort, 0xb6);
    out8(kPitChannel2Port, static_cast<uint8_t>(divisor & 0xffu));
    out8(kPitChannel2Port, static_cast<uint8_t>((divisor >> 8) & 0xffu));
    out8(kSpeakerPort, static_cast<uint8_t>(in8(kSpeakerPort) | 0x03u));
    return true;
}

void wait_milliseconds(uint32_t duration_ms) {
    const uint64_t start = timer::ticks();
    const uint64_t target = start + ((static_cast<uint64_t>(duration_ms) * timer::frequency_hz() + 999ULL) / 1000ULL);

    while (timer::ticks() < target) {
        arch::x86_64::enable_interrupts();
        arch::x86_64::halt_once();
        arch::x86_64::disable_interrupts();
    }
}

int speaker_ioctl(uint64_t request, uint64_t argument) {
    switch (request) {
        case PCSPK_IOC_BEEP: {
            if (!process::validate_user_range(argument, sizeof(savanxp_pcspk_beep), false)) {
                return negative_error(SAVANXP_EINVAL);
            }

            savanxp_pcspk_beep beep = {};
            if (!process::copy_from_user(&beep, argument, sizeof(beep))) {
                return negative_error(SAVANXP_EINVAL);
            }
            if (beep.duration_ms == 0 || !start_tone(beep.frequency_hz)) {
                return negative_error(SAVANXP_EINVAL);
            }

            wait_milliseconds(beep.duration_ms);
            stop_tone();
            return 0;
        }
        case PCSPK_IOC_STOP:
            stop_tone();
            return 0;
        default:
            return negative_error(SAVANXP_ENOSYS);
    }
}

} // namespace

namespace pcspeaker {

void initialize() {
    g_device.ioctl = speaker_ioctl;
    g_ready = device::register_node("/dev/pcspk", &g_device, true);
    stop_tone();
}

bool ready() {
    return g_ready;
}

} // namespace pcspeaker
