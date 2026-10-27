#include "kernel/ps2.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/console.hpp"
#include "kernel/cpu.hpp"
#include "kernel/input.hpp"
#include "kernel/ioapic.hpp"
#include "abi/savanxp_abi.h"

namespace {

constexpr uint16_t kDataPort = 0x60;
constexpr uint16_t kStatusPort = 0x64;
constexpr uint16_t kCommandPort = 0x64;

constexpr uint8_t kStatusOutputReady = 1u << 0;
constexpr uint8_t kStatusInputBusy = 1u << 1;
constexpr uint8_t kStatusAuxiliaryOutput = 1u << 5;

constexpr uint8_t kControllerCommandReadConfig = 0x20;
constexpr uint8_t kControllerCommandWriteConfig = 0x60;
constexpr uint8_t kControllerCommandWriteSecondPort = 0xd4;
constexpr uint8_t kControllerCommandSelfTest = 0xaa;
constexpr uint8_t kControllerCommandTestFirstPort = 0xab;
constexpr uint8_t kControllerCommandEnableSecondPort = 0xa8;
constexpr uint8_t kControllerCommandTestSecondPort = 0xa9;
constexpr uint8_t kControllerCommandDisableFirstPort = 0xad;
constexpr uint8_t kControllerCommandEnableFirstPort = 0xae;
constexpr uint8_t kControllerCommandDisableSecondPort = 0xa7;

constexpr uint8_t kConfigIrqFirstPort = 1u << 0;
constexpr uint8_t kConfigIrqSecondPort = 1u << 1;
constexpr uint8_t kConfigDisableFirstPortClock = 1u << 4;
constexpr uint8_t kConfigDisableSecondPortClock = 1u << 5;
constexpr uint8_t kConfigTranslation = 1u << 6;

constexpr uint8_t kKeyboardCommandSetLeds = 0xed;
constexpr uint8_t kKeyboardCommandSetScancode = 0xf0;
constexpr uint8_t kKeyboardCommandDisableScanning = 0xf5;
constexpr uint8_t kKeyboardCommandSetDefaults = 0xf6;
constexpr uint8_t kKeyboardCommandEnableScanning = 0xf4;
constexpr uint8_t kKeyboardCommandReset = 0xff;

constexpr uint8_t kMouseCommandSetDefaults = 0xf6;
constexpr uint8_t kMouseCommandEnableDataReporting = 0xf4;
constexpr uint8_t kMouseCommandSetSampleRate = 0xf3;
constexpr uint8_t kMouseCommandGetDeviceId = 0xf2;
constexpr uint8_t kMouseCommandReset = 0xff;

constexpr uint8_t kMouseDeviceIdStandard = 0x00;
constexpr uint8_t kMouseDeviceIdWheel = 0x03;
constexpr uint8_t kMouseDefaultSampleRate = 100;

constexpr uint8_t kKeyboardResponseAck = 0xfa;
constexpr uint8_t kKeyboardResponseResend = 0xfe;
constexpr uint8_t kKeyboardResponseBatOk = 0xaa;
constexpr uint8_t kKeyboardResponseSelfTestFailed0 = 0xfc;
constexpr uint8_t kKeyboardResponseSelfTestFailed1 = 0xfd;
constexpr uint8_t kKeyboardResponseBufferError0 = 0x00;
constexpr uint8_t kKeyboardResponseBufferError1 = 0xff;

constexpr uint8_t kMouseResponseAck = 0xfa;
constexpr uint8_t kMouseResponseResend = 0xfe;
constexpr uint8_t kMouseResponseBatOk = 0xaa;

constexpr uint8_t kScancodeSet2 = 0x02;

constexpr size_t kRawQueueCapacity = 256;
constexpr uint32_t kIoWaitSpinCount = 100000;
constexpr uint32_t kCommandRetryCount = 3;
struct RawByte {
    uint8_t value;
    bool auxiliary;
};

bool g_ready = false;
bool g_mouse_ready = false;
// The main port's counterpart of g_mouse_ready: false when another input
// driver already provides a keyboard, and the 8042 stays quiet so each key is
// not reported twice (QEMU feeds the host's input to every backend at once).
bool g_keyboard_active = false;

RawByte g_raw_queue[kRawQueueCapacity] = {};
size_t g_raw_read_index = 0;
size_t g_raw_write_index = 0;
size_t g_raw_count = 0;
bool g_raw_overflow = false;

bool g_extended_prefix = false;
uint8_t g_pause_sequence_index = 0;
uint8_t g_print_screen_state = 0;

// Lock state the keyboard core last reported (input::kLockModifiers), and
// whether the keyboard's LEDs still have to be told.
uint32_t g_lock_modifiers = 0;
bool g_led_sync_pending = false;

// Cuatro bytes en modo IntelliMouse (el cuarto es la rueda), tres en el modo
// estandar. g_mouse_packet_length es el framing vigente: lo fija la deteccion
// del arranque y no cambia despues.
uint8_t g_mouse_packet[4] = {};
uint8_t g_mouse_packet_index = 0;
uint8_t g_mouse_packet_length = 3;

inline void out8(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

inline uint8_t in8(uint16_t port) {
    uint8_t value = 0;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

bool wait_input_ready() {
    for (uint32_t spin = 0; spin < kIoWaitSpinCount; ++spin) {
        if ((in8(kStatusPort) & kStatusInputBusy) == 0) {
            return true;
        }
    }
    return false;
}

bool wait_output_ready() {
    for (uint32_t spin = 0; spin < kIoWaitSpinCount; ++spin) {
        if ((in8(kStatusPort) & kStatusOutputReady) != 0) {
            return true;
        }
    }
    return false;
}

bool controller_write(uint8_t value) {
    if (!wait_input_ready()) {
        return false;
    }
    out8(kCommandPort, value);
    return true;
}

bool data_write(uint8_t value) {
    if (!wait_input_ready()) {
        return false;
    }
    out8(kDataPort, value);
    return true;
}

bool data_read(uint8_t& value) {
    if (!wait_output_ready()) {
        return false;
    }
    value = in8(kDataPort);
    return true;
}

bool read_output_byte(uint8_t& value, bool& auxiliary) {
    if (!wait_output_ready()) {
        return false;
    }

    const uint8_t status = in8(kStatusPort);
    auxiliary = (status & kStatusAuxiliaryOutput) != 0;
    value = in8(kDataPort);
    return true;
}

void flush_controller_output() {
    while ((in8(kStatusPort) & kStatusOutputReady) != 0) {
        (void)in8(kDataPort);
    }
}

void reset_decoder_state() {
    g_extended_prefix = false;
    g_pause_sequence_index = 0;
    g_print_screen_state = 0;
}

void reset_input_state() {
    g_led_sync_pending = false;
    reset_decoder_state();
    g_mouse_packet[0] = 0;
    g_mouse_packet[1] = 0;
    g_mouse_packet[2] = 0;
    g_mouse_packet[3] = 0;
    g_mouse_packet_index = 0;
    g_mouse_packet_length = 3;
}

void clear_raw_queue() {
    g_raw_read_index = 0;
    g_raw_write_index = 0;
    g_raw_count = 0;
    g_raw_overflow = false;
}

void enqueue_raw_byte(uint8_t value, bool auxiliary) {
    if (g_raw_count == kRawQueueCapacity) {
        g_raw_overflow = true;
        return;
    }

    g_raw_queue[g_raw_write_index] = {
        .value = value,
        .auxiliary = auxiliary,
    };
    g_raw_write_index = (g_raw_write_index + 1) % kRawQueueCapacity;
    g_raw_count += 1;
}

bool dequeue_raw_byte(RawByte& value) {
    if (g_raw_count == 0) {
        return false;
    }

    value = g_raw_queue[g_raw_read_index];
    g_raw_read_index = (g_raw_read_index + 1) % kRawQueueCapacity;
    g_raw_count -= 1;
    return true;
}

void drain_controller_output_to_queue() {
    while ((in8(kStatusPort) & kStatusOutputReady) != 0) {
        const uint8_t status = in8(kStatusPort);
        enqueue_raw_byte(in8(kDataPort), (status & kStatusAuxiliaryOutput) != 0);
    }
}

bool controller_read_config(uint8_t& config) {
    if (!controller_write(kControllerCommandReadConfig)) {
        return false;
    }
    return data_read(config);
}

bool controller_write_config(uint8_t config) {
    if (!controller_write(kControllerCommandWriteConfig)) {
        return false;
    }
    return data_write(config);
}

bool controller_expect_byte(uint8_t expected) {
    uint8_t value = 0;
    return data_read(value) && value == expected;
}

bool keyboard_wait_for_ack() {
    for (;;) {
        uint8_t value = 0;
        if (!data_read(value)) {
            return false;
        }

        if (value == kKeyboardResponseAck) {
            return true;
        }
        if (value == kKeyboardResponseResend ||
            value == kKeyboardResponseSelfTestFailed0 ||
            value == kKeyboardResponseSelfTestFailed1 ||
            value == kKeyboardResponseBufferError0 ||
            value == kKeyboardResponseBufferError1) {
            return false;
        }
    }
}

bool keyboard_send_byte_with_ack(uint8_t value) {
    for (uint32_t attempt = 0; attempt < kCommandRetryCount; ++attempt) {
        if (!data_write(value)) {
            return false;
        }
        if (keyboard_wait_for_ack()) {
            return true;
        }
    }
    return false;
}

bool keyboard_send_command(uint8_t command) {
    return keyboard_send_byte_with_ack(command);
}

bool keyboard_send_command(uint8_t command, uint8_t argument) {
    return keyboard_send_byte_with_ack(command) && keyboard_send_byte_with_ack(argument);
}

bool write_mouse_data(uint8_t value) {
    return controller_write(kControllerCommandWriteSecondPort) && data_write(value);
}

bool mouse_wait_for_ack() {
    for (;;) {
        uint8_t value = 0;
        bool auxiliary = false;
        if (!read_output_byte(value, auxiliary)) {
            return false;
        }
        if (!auxiliary) {
            continue;
        }
        if (value == kMouseResponseAck) {
            return true;
        }
        if (value == kMouseResponseResend) {
            return false;
        }
    }
}

bool mouse_send_byte_with_ack(uint8_t value) {
    for (uint32_t attempt = 0; attempt < kCommandRetryCount; ++attempt) {
        if (!write_mouse_data(value)) {
            return false;
        }
        if (mouse_wait_for_ack()) {
            return true;
        }
    }
    return false;
}

// Siguiente byte de datos del puerto auxiliar, salteando los del principal.
// Solo vale durante la inicializacion, cuando el mouse todavia no esta en
// streaming: ahi cada byte auxiliar es la respuesta a un comando y no dato de
// un paquete. Mismo criterio que mouse_wait_for_ack().
bool mouse_read_data_byte(uint8_t& value) {
    for (;;) {
        bool auxiliary = false;
        if (!read_output_byte(value, auxiliary)) {
            return false;
        }
        if (auxiliary) {
            return true;
        }
    }
}

bool keyboard_reset_device() {
    for (uint32_t attempt = 0; attempt < kCommandRetryCount; ++attempt) {
        if (!data_write(kKeyboardCommandReset)) {
            return false;
        }

        uint8_t response = 0;
        if (!data_read(response)) {
            return false;
        }
        if (response == kKeyboardResponseResend) {
            continue;
        }
        if (response != kKeyboardResponseAck) {
            return false;
        }

        if (!data_read(response)) {
            return false;
        }
        return response == kKeyboardResponseBatOk;
    }
    return false;
}

bool mouse_reset_device() {
    for (uint32_t attempt = 0; attempt < kCommandRetryCount; ++attempt) {
        if (!write_mouse_data(kMouseCommandReset)) {
            return false;
        }

        uint8_t response = 0;
        bool auxiliary = false;
        if (!read_output_byte(response, auxiliary)) {
            return false;
        }
        if (!auxiliary) {
            continue;
        }
        if (response == kMouseResponseResend) {
            continue;
        }
        if (response != kMouseResponseAck) {
            return false;
        }

        if (!read_output_byte(response, auxiliary) || !auxiliary || response != kMouseResponseBatOk) {
            return false;
        }
        if (!read_output_byte(response, auxiliary) || !auxiliary) {
            return false;
        }
        return true;
    }
    return false;
}

uint8_t current_led_mask() {
    return static_cast<uint8_t>(
        ((g_lock_modifiers & input::modifier_scroll_lock) != 0 ? 0x01u : 0u) |
        ((g_lock_modifiers & input::modifier_num_lock) != 0 ? 0x02u : 0u) |
        ((g_lock_modifiers & input::modifier_caps_lock) != 0 ? 0x04u : 0u));
}

bool synchronize_leds_locked() {
    return keyboard_send_command(kKeyboardCommandDisableScanning) &&
        keyboard_send_command(kKeyboardCommandSetLeds, current_led_mask()) &&
        keyboard_send_command(kKeyboardCommandEnableScanning);
}

void emit_mouse_event(int32_t delta_x, int32_t delta_y, int32_t wheel, uint32_t buttons) {
    input::submit_mouse_event({
        .delta_x = delta_x,
        .delta_y = delta_y,
        .wheel = wheel,
        .buttons = buttons,
        // Un mouse PS/2 es relativo: no sabe donde esta el puntero, solo cuanto
        // se movio. Lo que el accumulation del consumidor y el del host hacen
        // siempre igual es lo que mantiene los dos alineados, asi que un delta
        // perdido se recupera solo en la proxima muestra. No hay posicion
        // verdadera que mandar.
        .absolute_x = 0,
        .absolute_y = 0,
        .has_absolute = false,
    });
}

void handle_make_break_code(uint8_t raw_code, bool extended) {
    input::submit_scancode(static_cast<uint8_t>(raw_code & 0x7fu), extended, (raw_code & 0x80u) == 0);
}

void process_pause_byte(uint8_t byte) {
    static const uint8_t kPauseSequence[] = {0xe1, 0x1d, 0x45, 0xe1, 0x9d, 0xc5};

    if (byte != kPauseSequence[g_pause_sequence_index]) {
        g_pause_sequence_index = 0;
        return;
    }

    g_pause_sequence_index += 1;
    if (g_pause_sequence_index == sizeof(kPauseSequence)) {
        input::submit_key(SAVANXP_KEY_PAUSE, true);
        g_pause_sequence_index = 0;
    }
}

void process_print_screen_state(uint8_t byte, bool& handled) {
    handled = false;

    if (g_print_screen_state == 1) {
        handled = true;
        if (byte == 0xe0) {
            g_print_screen_state = 3;
            return;
        }
        g_print_screen_state = 0;
        return;
    }
    if (g_print_screen_state == 2) {
        handled = true;
        if (byte == 0xe0) {
            g_print_screen_state = 4;
            return;
        }
        g_print_screen_state = 0;
        return;
    }
    if (g_print_screen_state == 3) {
        handled = true;
        if (byte == 0x37) {
            input::submit_key(SAVANXP_KEY_PRINT_SCREEN, true);
        }
        g_print_screen_state = 0;
        return;
    }
    if (g_print_screen_state == 4) {
        handled = true;
        if (byte == 0xaa) {
            input::submit_key(SAVANXP_KEY_PRINT_SCREEN, false);
        }
        g_print_screen_state = 0;
    }
}

void process_keyboard_byte(uint8_t byte) {
    if (byte == 0x54 || byte == 0xd4) {
        input::submit_key(SAVANXP_KEY_PRINT_SCREEN, byte == 0x54);
        return;
    }
    if (byte == kKeyboardResponseAck || byte == kKeyboardResponseResend) {
        return;
    }
    if (byte == kKeyboardResponseBufferError0 || byte == kKeyboardResponseBufferError1) {
        reset_decoder_state();
        return;
    }
    if (byte == kKeyboardResponseSelfTestFailed0 || byte == kKeyboardResponseSelfTestFailed1) {
        g_ready = false;
        reset_decoder_state();
        console::write_line("ps2: keyboard reported a self-test failure");
        return;
    }

    if (g_pause_sequence_index != 0 || byte == 0xe1) {
        process_pause_byte(byte);
        return;
    }

    bool handled_print_screen = false;
    process_print_screen_state(byte, handled_print_screen);
    if (handled_print_screen) {
        return;
    }

    if (byte == 0xe0) {
        g_extended_prefix = true;
        return;
    }

    if (g_extended_prefix) {
        g_extended_prefix = false;
        if (byte == 0x37) {
            input::submit_key(SAVANXP_KEY_PRINT_SCREEN, true);
            return;
        }
        if (byte == 0xb7 || byte == 0xaa) {
            input::submit_key(SAVANXP_KEY_PRINT_SCREEN, false);
            return;
        }
        if (byte == 0x2a) {
            g_print_screen_state = 1;
            return;
        }
        if (byte == 0xb7) {
            g_print_screen_state = 2;
            return;
        }
        handle_make_break_code(byte, true);
        return;
    }

    handle_make_break_code(byte, false);
}

int32_t decode_mouse_delta(uint8_t value, bool negative) {
    int32_t delta = static_cast<int32_t>(value);
    if (negative) {
        delta -= 0x100;
    }
    return delta;
}

int32_t clamp_mouse_delta(int32_t value, int32_t limit) {
    if (value > limit) {
        return limit;
    }
    if (value < -limit) {
        return -limit;
    }
    return value;
}

void process_mouse_packet() {
    const uint8_t first = g_mouse_packet[0];
    const uint8_t second = g_mouse_packet[1];
    const uint8_t third = g_mouse_packet[2];

    if ((first & 0x08u) == 0) {
        return;
    }
    if ((first & 0xc0u) != 0) {
        return;
    }

    uint32_t buttons = 0;
    if ((first & 0x01u) != 0) {
        buttons |= SAVANXP_MOUSE_BUTTON_LEFT;
    }
    if ((first & 0x02u) != 0) {
        buttons |= SAVANXP_MOUSE_BUTTON_RIGHT;
    }
    if ((first & 0x04u) != 0) {
        buttons |= SAVANXP_MOUSE_BUTTON_MIDDLE;
    }

    int32_t delta_x = decode_mouse_delta(second, (first & 0x10u) != 0);
    int32_t raw_delta_y = decode_mouse_delta(third, (first & 0x20u) != 0);

    // Safety net: a desynced packet (dropped byte, glitched controller state)
    // can still decode to near the full +-256 range, which no real relative
    // motion reaches in a single packet. Clamp instead of dropping the packet
    // outright, so corruption gets capped instead of teleporting the cursor,
    // while legitimate fast motion keeps tracking instead of stalling.
    constexpr int32_t kMaxPlausibleDelta = 150;
    delta_x = clamp_mouse_delta(delta_x, kMaxPlausibleDelta);
    raw_delta_y = clamp_mouse_delta(raw_delta_y, kMaxPlausibleDelta);

    // Cuarto byte: Z en complemento a dos, positivo hacia el usuario. El signo
    // se invierte porque savanxp_mouse_event.wheel va al reves (positivo = lejos
    // del usuario, como REL_WHEEL). Mismo criterio de clamp que los deltas: un
    // paquete corrupto no puede pasar de un tick plausible.
    int32_t wheel = 0;
    if (g_mouse_packet_length == 4) {
        constexpr int32_t kMaxPlausibleWheel = 8;
        wheel = clamp_mouse_delta(
            -static_cast<int32_t>(static_cast<int8_t>(g_mouse_packet[3])), kMaxPlausibleWheel);
    }

    emit_mouse_event(delta_x, -raw_delta_y, wheel, buttons);
}

void process_mouse_byte(uint8_t byte) {
    // Do NOT filter 0xFA/0xFE here: in streaming mode every aux byte is packet
    // data, and a delta of -6 encodes as 0xFA while -2 encodes as 0xFE. Dropping
    // those mid-packet desyncs the 3-byte framing, which is why left/down motion
    // (negative deltas) corrupted the stream while right/up (positive) did not.
    // Command ACK/RESEND bytes never reach here; init consumes them synchronously
    // via mouse_wait_for_ack() straight off the controller port.
    if (g_mouse_packet_index == 0 && (byte & 0x08u) == 0) {
        return;
    }

    g_mouse_packet[g_mouse_packet_index++] = byte;
    if (g_mouse_packet_index < g_mouse_packet_length) {
        return;
    }

    process_mouse_packet();
    g_mouse_packet_index = 0;
}

void service_pending_led_update() {
    if (!g_led_sync_pending || !g_ready || !g_keyboard_active) {
        return;
    }

    // Guardar y reponer IF en vez de prenderlas al salir: esto corre tambien
    // desde el tick y desde el IRQ del teclado, con el lock del kernel tomado,
    // y una interrupcion anidada ahi se quedaria esperando ese mismo lock.
    const uint64_t flags = arch::x86_64::save_and_disable_interrupts();
    if (g_raw_count != 0 || (in8(kStatusPort) & kStatusOutputReady) != 0) {
        arch::x86_64::restore_interrupts(flags);
        return;
    }

    if (synchronize_leds_locked()) {
        g_led_sync_pending = false;
    }
    arch::x86_64::restore_interrupts(flags);
}

bool detect_second_port() {
    uint8_t config = 0;
    if (!controller_write(kControllerCommandEnableSecondPort)) {
        return false;
    }
    if (!controller_read_config(config)) {
        return false;
    }
    const bool second_port_present = (config & kConfigDisableSecondPortClock) == 0;
    if (!controller_write(kControllerCommandDisableSecondPort)) {
        return false;
    }
    return second_port_present;
}

bool initialize_keyboard() {
    flush_controller_output();
    if (!keyboard_reset_device()) {
        console::write_line("ps2: keyboard reset failed");
        return false;
    }
    if (!keyboard_send_command(kKeyboardCommandSetDefaults)) {
        console::write_line("ps2: failed to restore keyboard defaults");
        return false;
    }
    if (!keyboard_send_command(kKeyboardCommandSetScancode, kScancodeSet2)) {
        console::write_line("ps2: failed to select keyboard scancode set 2");
        return false;
    }
    if (!synchronize_leds_locked()) {
        console::write_line("ps2: failed to synchronize keyboard LEDs");
        return false;
    }
    return true;
}

// "Knock" de IntelliMouse: la secuencia de sample rates 200/100/80 es el codigo
// que un mouse con rueda reconoce para pasar a paquetes de 4 bytes. Un mouse sin
// rueda la ignora y sigue devolviendo el ID 0x00, asi que no hay nada que
// preguntar antes: se intenta siempre y manda el ID resultante.
//
// Va ANTES de habilitar el reporte de datos (0xf4) a proposito. Con el stream ya
// abierto, los ACK de estos siete comandos se mezclarian con los bytes de los
// paquetes y desincronizarian el framing -- exactamente el modo de falla que
// documenta process_mouse_byte().
bool enable_mouse_wheel() {
    constexpr uint8_t kKnockRates[] = {200, 100, 80};
    for (const uint8_t rate : kKnockRates) {
        if (!mouse_send_byte_with_ack(kMouseCommandSetSampleRate) ||
            !mouse_send_byte_with_ack(rate)) {
            return false;
        }
    }

    if (!mouse_send_byte_with_ack(kMouseCommandGetDeviceId)) {
        return false;
    }

    uint8_t device_id = kMouseDeviceIdStandard;
    if (!mouse_read_data_byte(device_id) || device_id != kMouseDeviceIdWheel) {
        return false;
    }

    // El knock deja el sample rate en 80. Reponer el default para no heredar la
    // tasa del codigo de deteccion como configuracion de funcionamiento.
    return mouse_send_byte_with_ack(kMouseCommandSetSampleRate) &&
        mouse_send_byte_with_ack(kMouseDefaultSampleRate);
}

bool initialize_mouse() {
    flush_controller_output();
    if (!mouse_reset_device()) {
        console::write_line("ps2: mouse reset failed");
        return false;
    }
    if (!mouse_send_byte_with_ack(kMouseCommandSetDefaults)) {
        console::write_line("ps2: failed to restore mouse defaults");
        return false;
    }

    const bool wheel = enable_mouse_wheel();
    g_mouse_packet_length = wheel ? 4 : 3;
    console::write_line(wheel
        ? "ps2: mouse wheel enabled (4-byte packets)"
        : "ps2: mouse without wheel (3-byte packets)");

    if (!mouse_send_byte_with_ack(kMouseCommandEnableDataReporting)) {
        console::write_line("ps2: failed to enable mouse data reporting");
        return false;
    }
    return true;
}

bool initialize_controller(bool& second_port_present) {
    second_port_present = false;
    flush_controller_output();

    if (!controller_write(kControllerCommandDisableFirstPort) ||
        !controller_write(kControllerCommandDisableSecondPort)) {
        console::write_line("ps2: failed to disable controller ports");
        return false;
    }

    flush_controller_output();

    if (!controller_write(kControllerCommandSelfTest) || !controller_expect_byte(0x55)) {
        console::write_line("ps2: controller self-test failed");
        return false;
    }

    if (!controller_write(kControllerCommandTestFirstPort) || !controller_expect_byte(0x00)) {
        console::write_line("ps2: first PS/2 port test failed");
        return false;
    }

    second_port_present = detect_second_port();
    if (second_port_present) {
        if (!controller_write(kControllerCommandEnableSecondPort)) {
            second_port_present = false;
        } else if (!controller_write(kControllerCommandTestSecondPort) || !controller_expect_byte(0x00)) {
            console::write_line("ps2: second PS/2 port test failed");
            second_port_present = false;
        } else if (!controller_write(kControllerCommandDisableSecondPort)) {
            second_port_present = false;
        }
    }

    uint8_t config = 0;
    if (!controller_read_config(config)) {
        console::write_line("ps2: failed to read controller config");
        return false;
    }

    config &= static_cast<uint8_t>(~(kConfigIrqFirstPort | kConfigIrqSecondPort));
    config &= static_cast<uint8_t>(~kConfigDisableFirstPortClock);
    if (second_port_present) {
        config &= static_cast<uint8_t>(~kConfigDisableSecondPortClock);
    } else {
        config |= kConfigDisableSecondPortClock;
    }
    config |= kConfigTranslation;
    if (!controller_write_config(config)) {
        console::write_line("ps2: failed to write controller config");
        return false;
    }

    if (!controller_write(kControllerCommandEnableFirstPort)) {
        console::write_line("ps2: failed to enable first PS/2 port");
        return false;
    }
    if (second_port_present && !controller_write(kControllerCommandEnableSecondPort)) {
        console::write_line("ps2: failed to enable second PS/2 port");
        second_port_present = false;
    }

    // A driver registered earlier wins (input::Driver): the 8042 port stays
    // quiet so the same key does not arrive twice.
    const bool keyboard_taken = input::keyboard_available();
    if (keyboard_taken) {
        console::write_line("ps2: skipping keyboard, another input driver provides one");
    } else if (!initialize_keyboard()) {
        return false;
    }
    g_keyboard_active = !keyboard_taken;

    const bool pointer_taken = input::pointer_available();
    if (second_port_present && pointer_taken) {
        console::write_line("ps2: skipping auxiliary mouse, another input driver provides a pointer");
    }

    g_mouse_ready = second_port_present && !pointer_taken && initialize_mouse();

    if (!controller_read_config(config)) {
        console::write_line("ps2: failed to reload controller config");
        return false;
    }

    if (g_keyboard_active) {
        config |= kConfigIrqFirstPort;
        config &= static_cast<uint8_t>(~kConfigDisableFirstPortClock);
    } else {
        config &= static_cast<uint8_t>(~kConfigIrqFirstPort);
        config |= kConfigDisableFirstPortClock;
    }
    if (g_mouse_ready) {
        config |= kConfigIrqSecondPort;
        config &= static_cast<uint8_t>(~kConfigDisableSecondPortClock);
    } else {
        config &= static_cast<uint8_t>(~kConfigIrqSecondPort);
        config |= kConfigDisableSecondPortClock;
        (void)controller_write(kControllerCommandDisableSecondPort);
    }
    config |= kConfigTranslation;
    if (!controller_write_config(config)) {
        console::write_line("ps2: failed to enable PS/2 IRQs");
        return false;
    }

    flush_controller_output();
    return true;
}

void keyboard_irq() {
    drain_controller_output_to_queue();
}

void mouse_irq() {
    drain_controller_output_to_queue();
}

void set_keyboard_leds(uint32_t lock_modifiers) {
    g_lock_modifiers = lock_modifiers;
    g_led_sync_pending = true;
}

} // namespace

namespace ps2 {

void initialize() {
    g_ready = false;
    g_mouse_ready = false;
    g_keyboard_active = false;
    clear_raw_queue();
    reset_input_state();

    bool second_port_present = false;
    if (!initialize_controller(second_port_present)) {
        return;
    }

    // Preferir el IOAPIC (rutea via GSI aplicando los overrides de la MADT y
    // entrega por el Local APIC). Si no hay IOAPIC (p.ej. config sin MADT), caer
    // al PIC legacy para no romper el arranque.
    const bool use_ioapic = ioapic::ready();

    if (g_keyboard_active) {
        const bool keyboard_routed = use_ioapic
            ? ioapic::route_legacy_irq(1, keyboard_irq) != 0
            : arch::x86_64::register_irq_handler(1, keyboard_irq);
        if (!keyboard_routed) {
            console::write_line("ps2: failed to route irq1");
            return;
        }
    }

    if (g_mouse_ready) {
        const bool mouse_routed = use_ioapic
            ? ioapic::route_legacy_irq(12, mouse_irq) != 0
            : arch::x86_64::register_irq_handler(12, mouse_irq);
        if (!mouse_routed) {
            console::write_line("ps2: failed to route irq12");
            g_mouse_ready = false;
        }
    }

    // route_legacy_irq deja la entrada del IOAPIC ya desenmascarada; el PIC en
    // cambio arranca todo enmascarado y hay que habilitar la linea a mano.
    if (!use_ioapic) {
        arch::x86_64::enable_irq(1);
        if (g_mouse_ready) {
            arch::x86_64::enable_irq(12);
        }
    }
    g_ready = true;

    if (g_keyboard_active || g_mouse_ready) {
        (void)input::register_driver({
            .name = "ps2",
            .keyboard = g_keyboard_active,
            .pointer = g_mouse_ready,
            .poll = poll,
            .set_keyboard_leds = set_keyboard_leds,
            .set_screen_extent = nullptr,
            .reset_pointer = nullptr,
        });
    }
}

void poll() {
    drain_controller_output_to_queue();

    if (g_raw_overflow) {
        g_raw_overflow = false;
        console::write_line("ps2: raw input queue overflow");
    }

    RawByte value = {};
    while (dequeue_raw_byte(value)) {
        if (value.auxiliary) {
            if (g_mouse_ready) {
                process_mouse_byte(value.value);
            }
        } else if (g_keyboard_active) {
            process_keyboard_byte(value.value);
        }
    }

    service_pending_led_update();
}

} // namespace ps2
