#include "pit.hpp"

#include "cpu.hpp"
#include "serial.hpp"

namespace i386 {

namespace pit {

namespace {

/* Puerto de datos del canal 0 y puerto de comando del PIT. */
constexpr uint16_t kChannel0Data = 0x40;
constexpr uint16_t kCommandPort = 0x43;
/* Comando: canal 0, acceso lo/hi byte, modo 3 (onda cuadrada), binario. */
constexpr uint8_t kChannel0Command = 0x36;
/* Frecuencia base del PIT: el divisor es 1193182 / 100 = 11931 (100.003 Hz;
 * un error de 0.003% no importa para un uptime en decenas de ms). */
constexpr uint32_t kInputFrequency = 1193182;
constexpr uint8_t kTimerIrq = 0;

/* Escrito por la IRQ0, leido desde cualquier parte. volatile porque el
 * incremento vive en un manejador y un leido sin volatile podria cachearse
 * en un lazo (la lectura alineada en si es atomica en x86). */
volatile uint32_t g_ticks = 0;

void timer_irq() {
    /* El EOI lo manda i386_handle_irq despues de volver; aqui solo se cuenta.
     * Un tick es 10 ms de reloj de pared mientras IF=1. */
    ++g_ticks;
}

inline void outb(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

}  // namespace

void initialize() {
    const uint16_t divisor = static_cast<uint16_t>(kInputFrequency / kFrequencyHz);
    outb(kCommandPort, kChannel0Command);
    outb(kChannel0Data, static_cast<uint8_t>(divisor & 0xFFu));
    outb(kChannel0Data, static_cast<uint8_t>((divisor >> 8) & 0xFFu));

    cpu::register_irq_handler(kTimerIrq, timer_irq);
    cpu::enable_irq(kTimerIrq);

    serial::write("pit: canal 0 a ");
    serial::write_dec(kFrequencyHz);
    serial::write(" Hz (divisor ");
    serial::write_dec(divisor);
    serial::write(")\n");
}

uint32_t ticks() {
    return g_ticks;
}

uint64_t uptime_ms() {
    return static_cast<uint64_t>(g_ticks) * kTickMs;
}

void sleep_ms(uint32_t milliseconds) {
    const uint64_t deadline = uptime_ms() + milliseconds;
    while (uptime_ms() < deadline) {
        asm volatile("sti\n\thlt");
    }
}

}  // namespace pit

}  // namespace i386
