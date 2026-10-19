#include "ps2_32.hpp"

#include <stdint.h>

#include "cpu.hpp"
#include "savanxp/syscall.h"
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

/* En set 1 el bit 7 del byte es la soltura (0x9E suelta la 0x1E); no existe
 * el prefijo 0xF0 de set 2. El 8042 arranca con traduccion (bit 6 del
 * config), asi que el teclado habla set 2 y el controlador entrega set 1. */
constexpr uint8_t kReleaseBit = 0x80;

/* Tablas scancode->ascii copiadas de kernel/ps2.cpp, sin tocar una sola
 * entrada (kNormalEs/kShiftedEs son el layout original; kNormalEn/kShiftedEn
 * es US QWERTY derivado del set 1 canonico). Estan aqui y no incluidas
 * porque kernel/ps2.cpp arrastra console/ioapic/virtio, que en i386 no
 * existen. MVP: normal/shifted, sin AltGr ni locks de teclado. */
const char kNormalEs[] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '\'', 0, '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '\'', '+', '\n', 0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '{', '|', 0, '}', 'z',
    'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '-', 0, '*', 0, ' ', 0
};
const char kShiftedEs[] = {
    0, 27, '!', '"', '#', '$', '%', '&', '/', '(', ')', '=', '?', 0, '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '"', '*', '\n', 0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '[', 0, 0, ']', 'Z',
    'X', 'C', 'V', 'B', 'N', 'M', ';', ':', '_', 0, '*', 0, ' ', 0
};
const char kNormalEn[] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z',
    'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ', 0
};
const char kShiftedEn[] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z',
    'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ', 0
};
constexpr size_t kTableSize = sizeof(kNormalEs);

/* Layout activo: ES por defecto, igual que kernel/ps2.cpp. Las tablas EN
 * acompanan al ES para que el cambio de layout sea un puntero (set_layout,
 * como en kernel/ps2.cpp) y no una reescritura de tablas. */
const char* g_normal = kNormalEs;
const char* g_shifted = kShiftedEs;
const struct {
    const char* normal;
    const char* shifted;
} kLayouts[2] = {
    {kNormalEs, kShiftedEs},
    {kNormalEn, kShiftedEn},
};

/* Modificadores pulsados. Sin AltGr ni caps/num (sin LEDs todavia). */
bool g_left_shift = false;
bool g_right_shift = false;
bool g_ctrl = false;
bool g_alt = false;
bool g_extended = false;

/* Ultimo evento decodificado, para el log y la consola. */
char g_last_ascii = 0;
uint32_t g_last_key = SAVANXP_KEY_NONE;

/* Cola de eventos para la syscall READ: la escribe solo la IRQ1 y la lee el
 * proceso unico desde el syscall handler. pop_event() protege su lectura con
 * cli (restaurando el IF previo) porque la espera de READ se hace con
 * interrupciones vivas; push() corre dentro de la IRQ, que ya esta con IF=0. */
constexpr size_t kQueueCapacity = 64;
savanxp_input_event g_queue[kQueueCapacity] = {};
size_t g_read_index = 0;
size_t g_write_index = 0;
size_t g_pending = 0;

inline uint8_t inb(uint16_t port) {
    uint8_t value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

inline void outb(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

bool read_data(uint8_t& value) {
    if ((inb(kStatusPort) & kStatusOutputFull) == 0) {
        return false;
    }
    value = inb(kDataPort);
    return true;
}

/* Lectura de descarte: bytes del 8042 que solo interesan para vaciar el
 * buffer (el resultado del self-test, por ejemplo). */
void discard_data() {
    uint8_t ignored = 0;
    while (read_data(ignored)) {
    }
}

uint32_t modifiers_now() {
    uint32_t modifiers = 0;
    if (g_left_shift || g_right_shift) {
        modifiers |= SAVANXP_KEY_MOD_SHIFT;
    }
    if (g_ctrl) {
        modifiers |= SAVANXP_KEY_MOD_CTRL;
    }
    if (g_alt) {
        modifiers |= SAVANXP_KEY_MOD_ALT;
    }
    return modifiers;
}

/* Codigo SAVANXP_KEY_* de las teclas sin ascii, con el mismo mapeo que
 * translate_key_code de kernel/ps2.cpp (las imprimibles devuelven NONE y su
 * identidad es el ascii, igual que alli). */
uint32_t key_code_for(uint8_t scancode, bool extended) {
    if (extended) {
        switch (scancode) {
            case 0x1c: return SAVANXP_KEY_ENTER;
            case 0x1d: return SAVANXP_KEY_CTRL;
            case 0x35: return SAVANXP_KEY_NONE;
            case 0x38: return SAVANXP_KEY_ALT_GR;
            case 0x47: return SAVANXP_KEY_HOME;
            case 0x48: return SAVANXP_KEY_UP;
            case 0x49: return SAVANXP_KEY_PAGE_UP;
            case 0x4b: return SAVANXP_KEY_LEFT;
            case 0x4d: return SAVANXP_KEY_RIGHT;
            case 0x4f: return SAVANXP_KEY_END;
            case 0x50: return SAVANXP_KEY_DOWN;
            case 0x51: return SAVANXP_KEY_PAGE_DOWN;
            case 0x52: return SAVANXP_KEY_INSERT;
            case 0x53: return SAVANXP_KEY_DELETE;
            case 0x5b:
            case 0x5c: return SAVANXP_KEY_SUPER;
            case 0x5d: return SAVANXP_KEY_MENU;
            default: return SAVANXP_KEY_NONE;
        }
    }

    switch (scancode) {
        case 0x01: return SAVANXP_KEY_ESC;
        case 0x0e: return SAVANXP_KEY_BACKSPACE;
        case 0x0f: return SAVANXP_KEY_TAB;
        case 0x1c: return SAVANXP_KEY_ENTER;
        case 0x1d: return SAVANXP_KEY_CTRL;
        case 0x2a:
        case 0x36: return SAVANXP_KEY_SHIFT;
        case 0x38: return SAVANXP_KEY_ALT;
        case 0x3a: return SAVANXP_KEY_CAPSLOCK;
        case 0x3b: return SAVANXP_KEY_F1;
        case 0x3c: return SAVANXP_KEY_F2;
        case 0x3d: return SAVANXP_KEY_F3;
        case 0x3e: return SAVANXP_KEY_F4;
        case 0x3f: return SAVANXP_KEY_F5;
        case 0x40: return SAVANXP_KEY_F6;
        case 0x41: return SAVANXP_KEY_F7;
        case 0x42: return SAVANXP_KEY_F8;
        case 0x43: return SAVANXP_KEY_F9;
        case 0x44: return SAVANXP_KEY_F10;
        case 0x45: return SAVANXP_KEY_NUMLOCK;
        case 0x46: return SAVANXP_KEY_SCROLLLOCK;
        case 0x57: return SAVANXP_KEY_F11;
        case 0x58: return SAVANXP_KEY_F12;
        default: return SAVANXP_KEY_NONE;
    }
}

char ascii_for(uint8_t scancode, bool extended) {
    const bool shift = g_left_shift || g_right_shift;
    if (extended) {
        if (scancode == 0x1c) {
            return '\n';
        }
        if (scancode == 0x35) {
            return '/';
        }
        return 0;
    }
    if (scancode >= kTableSize) {
        return 0;
    }
    const char normal = g_normal[scancode];
    if (normal == 0) {
        return 0;
    }
    return shift ? g_shifted[scancode] : normal;
}

void push_event(uint32_t type, uint32_t key, int32_t ascii) {
    if (g_pending >= kQueueCapacity) {
        return;
    }
    g_queue[g_write_index].type = type;
    g_queue[g_write_index].key = key;
    g_queue[g_write_index].ascii = ascii;
    g_queue[g_write_index].modifiers = modifiers_now();
    g_write_index = (g_write_index + 1) % kQueueCapacity;
    ++g_pending;
}

/* Un byte set 1 (o el segundo byte de un par 0xE0) llega aqui. El registro
 * crudo se imprime SIEMPRE con el formato viejo `ps2: scan 0x...` porque es
 * lo que tools/legacy_boot_test.py --expect-key sigue exigiendo; la linea
 * decodificada va despues. */
void handle_code(uint8_t code) {
    serial::write("ps2: scan ");
    serial::write_hex(code);
    serial::write("\n");

    if (code == kScancodeExtendedPrefix) {
        g_extended = true;
        return;
    }
    const bool extended = g_extended;
    g_extended = false;

    const bool released = (code & kReleaseBit) != 0;
    const uint8_t scancode = static_cast<uint8_t>(code & ~kReleaseBit);

    /* Modificadores primero: el ASCII de las teclas siguientes depende de su
     * estado, igual que update_modifier_state en kernel/ps2.cpp. Los bits
     * extendidos de Ctrl/Alt (E0 1D / E0 38) son otro boton, no el izq. */
    if (!extended) {
        if (scancode == 0x2a) {
            g_left_shift = !released;
        } else if (scancode == 0x36) {
            g_right_shift = !released;
        } else if (scancode == 0x1d) {
            g_ctrl = !released;
        } else if (scancode == 0x38) {
            g_alt = !released;
        }
    } else if (scancode == 0x1d) {
        g_ctrl = !released;
    } else if (scancode == 0x38) {
        g_alt = !released;
    }

    uint32_t key = key_code_for(scancode, extended);
    char ascii = ascii_for(scancode, extended);
    if (key == SAVANXP_KEY_NONE && ascii != 0) {
        /* Tecla imprimible: su codigo es el propio ascii (el enum de
         * savanxp/syscall.h no trae letras; alli donde kernel/ps2.cpp
         * devuelve NONE el ascii identifica la tecla). */
        key = static_cast<uint32_t>(static_cast<unsigned char>(ascii));
    }
    if (key == SAVANXP_KEY_NONE && ascii == 0) {
        return;
    }
    if (released) {
        /* La soltura lleva su ascii para que el userland pueda emparejar
         * down/up de la misma tecla; g_last_ascii solo registra el del
         * pulsado (el estado "ultimo caracter" se limpia al soltar). */
        ascii = ascii_for(scancode, extended);
    }

    g_last_key = key;
    g_last_ascii = released ? 0 : ascii;
    push_event(released ? SAVANXP_INPUT_EVENT_KEY_UP : SAVANXP_INPUT_EVENT_KEY_DOWN,
               key, ascii);

    serial::write("ps2: tecla ");
    serial::write(released ? "up " : "down ");
    serial::write_hex(key);
    if (ascii >= 32 && ascii < 127) {
        serial::write(" '");
        serial::write_char(ascii);
        serial::write("'");
    }
    serial::write("\n");
}

void keyboard_irq() {
    uint8_t code = 0;
    while (read_data(code)) {
        handle_code(code);
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

}  // namespace

bool has_pending_event() {
    return g_pending != 0;
}

bool pop_event(savanxp_input_event& out) {
    /* Chequeo y extraccion atomicos respecto de la IRQ1: el consumidor
     * corre con interrupciones vivas (la espera de READ usa sti;hlt), y sin
     * este cli el contador g_pending se podria perder entre el ++ de la IRQ
     * y el -- de aqui. El IF previo se restaura: en el syscall handler el
     * gate llega con IF=0 y asi debe salir para que sti;hlt no pierda el
     * despertar. */
    const uint32_t flags = [&]() {
        uint32_t value;
        asm volatile("pushf; pop %0; cli" : "=r"(value) : : "memory");
        return value;
    }();
    if (g_pending == 0) {
        if ((flags & (1u << 9)) != 0) {
            asm volatile("sti");
        }
        return false;
    }
    out = g_queue[g_read_index];
    g_read_index = (g_read_index + 1) % kQueueCapacity;
    --g_pending;
    if ((flags & (1u << 9)) != 0) {
        asm volatile("sti");
    }
    return true;
}

char last_ascii() {
    return g_last_ascii;
}

uint32_t last_key() {
    return g_last_key;
}

uint32_t modifiers() {
    return modifiers_now();
}

bool set_layout(int layout) {
    if (layout != SAVANXP_KEYBOARD_LAYOUT_ES && layout != SAVANXP_KEYBOARD_LAYOUT_EN) {
        return false;
    }
    g_normal = kLayouts[layout].normal;
    g_shifted = kLayouts[layout].shifted;
    return true;
}

int get_layout() {
    return g_normal == kNormalEn ? SAVANXP_KEYBOARD_LAYOUT_EN : SAVANXP_KEYBOARD_LAYOUT_ES;
}

void initialize() {
    /* Secuencia estandar de un 8042: autoself-test, lectura de la
     * configuracion, puertos deshabilitados mientras se trabaja, vaciado del
     * buffer de salida y reactivacion solo del teclado con su IRQ. */
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

    discard_data();

    outb(kCommandPort, kCommandEnableKeyboard);
    outb(kCommandPort, kCommandResetEnable);

    cpu::register_irq_handler(kKeyboardIrq, keyboard_irq);
    cpu::register_irq_handler(kMouseIrq, mouse_irq);
    cpu::enable_irq(kKeyboardIrq);

    serial::write("ps2: teclado listo (irq 1, scancodes set 1)\n");
}

}  // namespace ps2_32

}  // namespace i386

