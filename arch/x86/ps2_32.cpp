#include "ps2_32.hpp"

#include <stdint.h>

#include "cpu.hpp"
#include "serial.hpp"

namespace i386 {

namespace ps2_32 {

namespace {

constexpr uint16_t kDataPort = 0x60;
constexpr uint16_t kStatusPort = 0x64;
constexpr uint16_t kCommandPort = 0x64;

constexpr uint8_t kStatusOutputFull = 1u << 0;

constexpr uint8_t kCommandReadConfig = 0x20;
constexpr uint8_t kCommandWriteConfig = 0x60;
constexpr uint8_t kCommandDisableKeyboard = 0xAD;
constexpr uint8_t kCommandEnableKeyboard = 0xAE;
constexpr uint8_t kCommandDisableMouse = 0xA7;
constexpr uint8_t kCommandSelfTest = 0xAA;
constexpr uint8_t kCommandResetEnable = 0xAE;

constexpr uint8_t kConfigKeyboardIrq = 1u << 0;
constexpr uint8_t kConfigMouseIrq = 1u << 1;

constexpr uint8_t kScancodeExtendedPrefix = 0xE0;
constexpr uint8_t kKeyboardIrq = 1;
constexpr uint8_t kMouseIrq = 12;

/* Teclado: el unico codigo que llega es la tecla pulsada; el 0xF0 delante
 * significa que viene la soltura. */
constexpr uint8_t kScancodeReleasePrefix = 0xF0;

inline uint8_t inb(uint16_t port) {
    uint8_t value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

inline void outb(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

bool g_extended = false;
bool g_release_pending = false;

bool read_data(uint8_t& value) {
    /* Con las interrupciones apagadas se sondea el registro de estado: si el
     * buffer de salida no esta lleno no hay nada que leer, y las IRQ se
     * atmiten al_driver y se accuse al PIC. */
    if ((inb(kStatusPort) & kStatusOutputFull) == 0) {
        return false;
    }
    value = inb(kDataPort);
    return true;
}

/* Lectura de descarte: hay bytes del 8042 que solo interesan para vaciar el
 * buffer (el resultado del self-test, por ejemplo). */
void discard_data() {
    uint8_t ignored = 0;
    while (read_data(ignored)) {
    }
}

void keyboard_irq() {
    uint8_t code = 0;
    while (read_data(code)) {
        /* Los prefijos 0xE0 (tecla extendida) y 0xF0 (soltura) no son teclas:
         * se anotan y quedan pendientes para el byte siguiente. El codigo de
         * la tecla y el estado pendiente se limpian al llegar el codigo. */
        const bool after_extended = g_extended;
        const bool after_release = g_release_pending;
        g_extended = code == kScancodeExtendedPrefix;
        g_release_pending = code == kScancodeReleasePrefix;

        serial::write("ps2: scan ");
        serial::write_hex(code);
        if (after_extended) {
            serial::write(" (tras extendido)");
        }
        if (after_release) {
            serial::write(" (tras soltura)");
        }
        if (code != kScancodeExtendedPrefix && code != kScancodeReleasePrefix &&
            (after_extended || after_release)) {
            /* El codigo completo de una tecla (por ejemplo 0x1E para la 'a'):
             * aqui solo se anota el prefijo que lo acompana. Decodificarlo a
             * texto con los layouts es el siguiente paso. */
            serial::write(" (codigo de tecla)");
        }
        serial::write("\n");
    }
}

void mouse_irq() {
    uint8_t code = 0;
    while (read_data(code)) {
        serial::write("ps2: raton ");
        serial::write_hex(code);
        serial::write("\n");
    }
}

}

void initialize() {
    /* Secuencia estandar de un 8042: autoself-test, lectura de la
     * configuracion, puertos deshabilitados mientras se trabaja, vaciado del
     * buffer de salida y reactivacion solo del teclado con su IRQ. El raton
     * queda sin IRQ: el paquete es de teclado por ahora. */
    outb(kCommandPort, kCommandDisableKeyboard);
    outb(kCommandPort, kCommandDisableMouse);

    outb(kCommandPort, kCommandSelfTest);
    discard_data();

    outb(kCommandPort, kCommandReadConfig);
    uint8_t config = 0;
    if (read_data(config)) {
        config = static_cast<uint8_t>((config & ~kConfigMouseIrq) | kConfigKeyboardIrq);
        outb(kCommandPort, kCommandWriteConfig);
        outb(kDataPort, config);
    }

    /* Vaciar el buffer de salida: si el byte pendiente no se lee, el primer
     * uso parece un scancode fantasma. */
    discard_data();

    outb(kCommandPort, kCommandEnableKeyboard);
    outb(kCommandPort, kCommandResetEnable); /* el raton no se habilita todavia */

    cpu::register_irq_handler(kKeyboardIrq, keyboard_irq);
    cpu::register_irq_handler(kMouseIrq, mouse_irq);
    cpu::enable_irq(kKeyboardIrq);

    /* Solo el teclado abre su mascara: el resto sigue cerrado hasta que su
     * driver este. Las interrupciones NO se encienden aqui: el arranque sigue
     * con IF=0 mientras monta SxFS y carga la imagen (una IRQ en mitad de
     * esas fases las rompe), y se encienden solas cuando el proceso entra en
     * ring 3 con eflags 0x202 o cuando el kernel se queda ocioso tras EXIT. */
    serial::write("ps2: teclado listo (irq 1, scancodes set 1)\n");
}

} // namespace ps2_32

} // namespace i386