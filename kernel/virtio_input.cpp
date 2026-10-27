#include "kernel/virtio_input.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/console.hpp"
#include "kernel/input.hpp"
#include "kernel/string.hpp"
#include "kernel/virtio_pci.hpp"
#include "abi/savanxp_abi.h"

namespace {

constexpr uint16_t kVirtioInputModernDevice = 0x1052u;
constexpr uint16_t kVirtioInputSubsystemDevice = 18u;
constexpr uint16_t kVirtioInputEventQueue = 0;
constexpr uint16_t kMaxEventQueueSize = 64;

constexpr uint8_t kVirtioInputCfgEvBits = 0x11;
constexpr uint8_t kVirtioInputCfgAbsInfo = 0x12;

constexpr uint16_t kEvSyn = 0x00;
constexpr uint16_t kEvKey = 0x01;
constexpr uint16_t kEvRel = 0x02;
constexpr uint16_t kEvAbs = 0x03;
constexpr uint16_t kSynReport = 0;
constexpr uint16_t kRelWheel = 0x08;
constexpr uint16_t kAbsX = 0x00;
constexpr uint16_t kAbsY = 0x01;
constexpr uint16_t kBtnLeft = 0x110;
constexpr uint16_t kBtnRight = 0x111;
constexpr uint16_t kBtnMiddle = 0x112;

// evdev codes (Linux input-event-codes.h) that a virtio-keyboard sends in
// VirtioInputEvent.code when type == kEvKey. From 1 to 0x58 Linux has always
// used, on purpose, the same number as the non-extended set 1 scancode
// (KEY_ESC=1=0x01, KEY_A=30=0x1e, ... KEY_F12=88=0x58), so they go straight
// to input::submit_scancode. Keys that a physical keyboard sends with the
// 0xE0 prefix (right ctrl/alt, arrows, home/end...) Linux renumbered higher
// (>=96): those are mapped by hand to the scancode they stand for under that
// prefix. SysRq and Pause stay out of the table: virtio-input delivers them as
// a clean press/release, without the multi-byte sequence real PS/2 hardware
// sends for them, so they go to input::submit_key.
constexpr uint16_t kEvdevScancodeIdentityMax = 0x58;
constexpr uint16_t kEvdevSysRq = 99;
constexpr uint16_t kEvdevPause = 119;

struct EvdevExtendedKey {
    uint16_t evdev_code;
    uint8_t scancode;
};

const EvdevExtendedKey kEvdevExtendedKeys[] = {
    {96, 0x1c},  // KEY_KPENTER
    {97, 0x1d},  // KEY_RIGHTCTRL
    {98, 0x35},  // KEY_KPSLASH
    {100, 0x38}, // KEY_RIGHTALT (AltGr)
    {102, 0x47}, // KEY_HOME
    {103, 0x48}, // KEY_UP
    {104, 0x49}, // KEY_PAGEUP
    {105, 0x4b}, // KEY_LEFT
    {106, 0x4d}, // KEY_RIGHT
    {107, 0x4f}, // KEY_END
    {108, 0x50}, // KEY_DOWN
    {109, 0x51}, // KEY_PAGEDOWN
    {110, 0x52}, // KEY_INSERT
    {111, 0x53}, // KEY_DELETE
    {125, 0x5b}, // KEY_LEFTMETA
    {126, 0x5c}, // KEY_RIGHTMETA
    {127, 0x5d}, // KEY_COMPOSE (menu)
};

bool map_evdev_keycode(uint16_t evdev_code, uint8_t& scancode, bool& extended) {
    if (evdev_code >= 1 && evdev_code <= kEvdevScancodeIdentityMax) {
        scancode = static_cast<uint8_t>(evdev_code);
        extended = false;
        return true;
    }

    for (const EvdevExtendedKey& entry : kEvdevExtendedKeys) {
        if (entry.evdev_code == evdev_code) {
            scancode = entry.scancode;
            extended = true;
            return true;
        }
    }

    return false;
}

struct [[gnu::packed]] VirtioInputAbsInfo {
    uint32_t min;
    uint32_t max;
    uint32_t fuzz;
    uint32_t flat;
    uint32_t resolution;
};

struct [[gnu::packed]] VirtioInputConfig {
    uint8_t select;
    uint8_t subsel;
    uint8_t size;
    uint8_t reserved[5];
    VirtioInputAbsInfo abs;
};

struct [[gnu::packed]] VirtioInputEvent {
    uint16_t type;
    uint16_t code;
    int32_t value;
};

virtio_pci::Device g_device = {};
virtio_pci::Queue g_event_queue = {};
virtio_pci::Device g_keyboard_device = {};
virtio_pci::Queue g_keyboard_event_queue = {};
bool g_keyboard_ready = false;
uint32_t g_framebuffer_width = 0;
uint32_t g_framebuffer_height = 0;
int32_t g_abs_min_x = 0;
int32_t g_abs_max_x = 0;
int32_t g_abs_min_y = 0;
int32_t g_abs_max_y = 0;
int32_t g_current_abs_x = 0;
int32_t g_current_abs_y = 0;
int32_t g_last_screen_x = 0;
int32_t g_last_screen_y = 0;
uint32_t g_buttons = 0;
// Ticks de rueda acumulados del reporte en curso: EV_REL llega como uno o mas
// eventos sueltos y recien el EV_SYN cierra el reporte.
int32_t g_pending_wheel = 0;
bool g_have_abs_x = false;
bool g_have_abs_y = false;
bool g_have_screen_position = false;
bool g_ready = false;

volatile VirtioInputConfig* device_cfg(virtio_pci::Device& device) {
    return reinterpret_cast<volatile VirtioInputConfig*>(virtio_pci::device_cfg_base(device));
}

// VIRTIO_INPUT_CFG_EV_BITS: cfg->size queda en 0 si el device no soporta ese
// tipo de evento. Los devices virtio-input son la misma pareja PCI
// device/subsystem para tablet y teclado (QEMU los distingue por function,
// no por id); EV_ABS presente es lo que separa al puntero (posicion
// absoluta) del teclado (solo EV_KEY). Es legal leer esto ya con
// ACKNOWLEDGE|DRIVER puesto, antes de negociar features.
bool device_advertises_ev(virtio_pci::Device& device, uint8_t ev_type) {
    volatile VirtioInputConfig* cfg = device_cfg(device);
    cfg->select = kVirtioInputCfgEvBits;
    cfg->subsel = ev_type;
    virtio_pci::memory_barrier();
    return cfg->size > 0;
}

bool read_abs_info(virtio_pci::Device& device, uint8_t axis, int32_t& minimum, int32_t& maximum) {
    volatile VirtioInputConfig* cfg = device_cfg(device);
    cfg->select = kVirtioInputCfgAbsInfo;
    cfg->subsel = axis;
    virtio_pci::memory_barrier();

    if (cfg->size < sizeof(VirtioInputAbsInfo)) {
        return false;
    }

    minimum = static_cast<int32_t>(cfg->abs.min);
    maximum = static_cast<int32_t>(cfg->abs.max);
    return maximum > minimum;
}

// El rango que el device publica en su config space no es fijo: depende de la
// geometria con la que el host presenta la pantalla, asi que un cambio de modo
// puede cambiarlo. Leerlo una sola vez en el probe deja el mapeo
// normalize_axis usando un rango viejo contra una pantalla nueva, y el puntero
// queda corrido de forma sistematica. Releerlo aqui, junto con el extent que
// ya se relee, cierra las dos mitades del problema.
//
// Es una lectura de config space, no un comando, asi que no invalida nada en
// cola: si falla se conservan los valores buenos en vez de dejar el puntero sin
// rango, que lo dejaria clavado en 0,0 por el clamp de normalize_axis.
void refresh_pointer_range() {
    if (!g_ready) {
        return;
    }

    int32_t min_x = 0;
    int32_t max_x = 0;
    int32_t min_y = 0;
    int32_t max_y = 0;
    if (!read_abs_info(g_device, kAbsX, min_x, max_x) ||
        !read_abs_info(g_device, kAbsY, min_y, max_y)) {
        return;
    }

    g_abs_min_x = min_x;
    g_abs_max_x = max_x;
    g_abs_min_y = min_y;
    g_abs_max_y = max_y;
}

VirtioInputEvent* queue_event_buffer(virtio_pci::Queue& queue, uint16_t index) {
    return reinterpret_cast<VirtioInputEvent*>(
        virtio_pci::queue_extra(queue, sizeof(VirtioInputEvent) * index)
    );
}

uint64_t queue_event_buffer_physical(const virtio_pci::Queue& queue, uint16_t index) {
    return virtio_pci::queue_extra_physical(queue, sizeof(VirtioInputEvent) * index);
}

bool setup_event_queue(virtio_pci::Device& device, virtio_pci::Queue& queue) {
    if (!virtio_pci::setup_queue(
            device,
            kVirtioInputEventQueue,
            kMaxEventQueueSize,
            sizeof(VirtioInputEvent) * kMaxEventQueueSize,
            16,
            queue)) {
        return false;
    }

    virtio_pci::Descriptor* descriptors = virtio_pci::queue_descriptors(queue);
    virtio_pci::UsedHeader* used = virtio_pci::queue_used_header(queue);
    used->flags = 0;
    used->idx = 0;

    for (uint16_t index = 0; index < queue.size; ++index) {
        descriptors[index] = {
            .addr = queue_event_buffer_physical(queue, index),
            .len = sizeof(VirtioInputEvent),
            .flags = virtio_pci::kDescriptorFlagWrite,
            .next = 0,
        };
        if (!virtio_pci::submit_descriptor_head(queue, index)) {
            return false;
        }
    }

    virtio_pci::memory_barrier();
    virtio_pci::notify_queue(device, queue);
    return true;
}

int32_t normalize_axis(int32_t value, int32_t minimum, int32_t maximum, uint32_t extent) {
    if (extent <= 1 || maximum <= minimum) {
        return 0;
    }

    if (value < minimum) {
        value = minimum;
    } else if (value > maximum) {
        value = maximum;
    }

    const int64_t range = static_cast<int64_t>(maximum) - static_cast<int64_t>(minimum);
    const int64_t scaled = (static_cast<int64_t>(value - minimum) * static_cast<int64_t>(extent - 1)) + (range / 2);
    return static_cast<int32_t>(scaled / range);
}

// La posicion del tablet es absoluta y verdadera: el host la manda y no depende
// de lo que el invitado haya acumulado. Por eso viaja entera ademas del delta,
// que sigue siendo lo que necesitan los dispositivos relativos y lo que
// necesita un consumidor que solo suma.
void submit_screen_position(int32_t screen_x, int32_t screen_y, int32_t wheel) {
    if (!g_have_screen_position) {
        g_last_screen_x = screen_x;
        g_last_screen_y = screen_y;
        g_have_screen_position = true;
        input::submit_mouse_event({
            .delta_x = screen_x,
            .delta_y = screen_y,
            .wheel = wheel,
            .buttons = g_buttons,
            .absolute_x = screen_x,
            .absolute_y = screen_y,
            .has_absolute = true,
            });
        return;
    }

    input::submit_mouse_event({
        .delta_x = screen_x - g_last_screen_x,
        .delta_y = screen_y - g_last_screen_y,
        .wheel = wheel,
        .buttons = g_buttons,
        .absolute_x = screen_x,
        .absolute_y = screen_y,
        .has_absolute = true,
    });

    g_last_screen_x = screen_x;
    g_last_screen_y = screen_y;
}

void process_pointer_event(const VirtioInputEvent& event) {
    if (event.type == kEvAbs) {
        if (event.code == kAbsX) {
            g_current_abs_x = event.value;
            g_have_abs_x = true;
        } else if (event.code == kAbsY) {
            g_current_abs_y = event.value;
            g_have_abs_y = true;
        }
        return;
    }

    // La rueda del tablet viene por EV_REL aunque la posicion sea absoluta:
    // REL_WHEEL es relativo por definicion, no tiene rango que reportar en el
    // abs_info. Positivo = lejos del usuario, misma convencion que el campo
    // wheel de savanxp_mouse_event, asi que pasa sin invertir.
    if (event.type == kEvRel) {
        if (event.code == kRelWheel) {
            g_pending_wheel += event.value;
        }
        return;
    }

    if (event.type == kEvKey) {
        const uint32_t mask =
            event.code == kBtnLeft ? SAVANXP_MOUSE_BUTTON_LEFT :
            event.code == kBtnRight ? SAVANXP_MOUSE_BUTTON_RIGHT :
            event.code == kBtnMiddle ? SAVANXP_MOUSE_BUTTON_MIDDLE : 0u;
        if (mask != 0) {
            if (event.value != 0) {
                g_buttons |= mask;
            } else {
                g_buttons &= ~mask;
            }
        }
        return;
    }

    if (event.type != kEvSyn || event.code != kSynReport) {
        return;
    }

    const int32_t wheel = g_pending_wheel;
    g_pending_wheel = 0;

    if (!g_have_abs_x || !g_have_abs_y) {
        // Rueda antes del primer reporte de posicion. No hay cursor que mover,
        // pero el tick es real: descartarlo junto con el resto del reporte
        // perderia el scroll de quien usa la rueda sin haber movido el mouse.
        if (wheel != 0) {
            input::submit_mouse_event({
                .delta_x = 0,
                .delta_y = 0,
                .wheel = wheel,
                .buttons = g_buttons,
                // Sin posicion no hay nada que mandar: hasta ahora no se leyo
                // ningun EV_ABS, asi que el flag no se pone y el consumidor
                // sigue sumando deltas, que para la rueda es lo unico que hay.
                .absolute_x = 0,
                .absolute_y = 0,
                .has_absolute = false,
                    });
        }
        return;
    }

    submit_screen_position(
        normalize_axis(g_current_abs_x, g_abs_min_x, g_abs_max_x, g_framebuffer_width),
        normalize_axis(g_current_abs_y, g_abs_min_y, g_abs_max_y, g_framebuffer_height),
        wheel
    );
}

// Unlike the pointer, a keyboard has no state to gather across events
// (nothing like waiting for EV_SYN before emitting): each EV_KEY is a whole
// key and goes out as soon as it arrives. The keyboard core applies the
// layout, case and modifiers.
void process_keyboard_event(const VirtioInputEvent& event) {
    if (event.type != kEvKey) {
        return;
    }

    const bool pressed = event.value != 0;
    if (event.code == kEvdevSysRq) {
        input::submit_key(SAVANXP_KEY_PRINT_SCREEN, pressed);
        return;
    }
    if (event.code == kEvdevPause) {
        input::submit_key(SAVANXP_KEY_PAUSE, pressed);
        return;
    }

    uint8_t scancode = 0;
    bool extended = false;
    if (map_evdev_keycode(event.code, scancode, extended)) {
        input::submit_scancode(scancode, extended, pressed);
    }
}

void fail_device(const char* reason) {
    virtio_pci::fail_device(g_device);
    if (reason != nullptr) {
        console::printf("virtio-input: %s\n", reason);
    }
    g_ready = false;
}

void fail_keyboard_device(const char* reason) {
    virtio_pci::fail_device(g_keyboard_device);
    if (reason != nullptr) {
        console::printf("virtio-input: %s\n", reason);
    }
    g_keyboard_ready = false;
}

// true si reclamo pci_device para el puntero (tablet). false sin tocar estado
// global si el device resulta ser de otro tipo (deja el virtio reseteado a
// status=0, listo para que try_initialize_keyboard lo vuelva a intentar).
bool try_initialize_pointer(const pci::DeviceInfo& pci_device) {
    virtio_pci::Device device = {};
    if (!virtio_pci::initialize_device(pci_device, true, device)) {
        return false;
    }

    virtio_pci::set_device_status(device, 0);
    virtio_pci::memory_barrier();
    virtio_pci::set_device_status(device, static_cast<uint8_t>(virtio_pci::kStatusAcknowledge | virtio_pci::kStatusDriver));

    if (!device_advertises_ev(device, kEvAbs)) {
        virtio_pci::fail_device(device);
        return false;
    }

    g_device = device;
    if (!virtio_pci::negotiate_features(g_device, 0, virtio_pci::kFeatureVersion1Bit)) {
        fail_device("feature negotiation failed");
        return false;
    }
    if (!read_abs_info(g_device, kAbsX, g_abs_min_x, g_abs_max_x) ||
        !read_abs_info(g_device, kAbsY, g_abs_min_y, g_abs_max_y)) {
        fail_device("failed to read ABS ranges");
        return false;
    }
    if (!setup_event_queue(g_device, g_event_queue)) {
        fail_device("failed to initialize event queue");
        return false;
    }

    virtio_pci::set_device_status(g_device, static_cast<uint8_t>(virtio_pci::device_status(g_device) | virtio_pci::kStatusDriverOk));
    g_ready = true;
    console::printf(
        "virtio-input: tablet ready pci=%x:%x.%u abs_x=%d..%d abs_y=%d..%d\n",
        static_cast<unsigned>(g_device.pci_device.bus),
        static_cast<unsigned>(g_device.pci_device.slot),
        static_cast<unsigned>(g_device.pci_device.function),
        static_cast<int>(g_abs_min_x),
        static_cast<int>(g_abs_max_x),
        static_cast<int>(g_abs_min_y),
        static_cast<int>(g_abs_max_y)
    );
    return true;
}

void try_initialize_keyboard(const pci::DeviceInfo& pci_device) {
    virtio_pci::Device device = {};
    if (!virtio_pci::initialize_device(pci_device, true, device)) {
        return;
    }

    virtio_pci::set_device_status(device, 0);
    virtio_pci::memory_barrier();
    virtio_pci::set_device_status(device, static_cast<uint8_t>(virtio_pci::kStatusAcknowledge | virtio_pci::kStatusDriver));

    if (device_advertises_ev(device, kEvAbs)) {
        // Tiene eje absoluto: es un puntero, no un teclado. Si llego hasta
        // aca es porque try_initialize_pointer ya lo descarto por otro
        // motivo (por ejemplo, un segundo tablet); no es candidato a teclado.
        virtio_pci::fail_device(device);
        return;
    }

    g_keyboard_device = device;
    if (!virtio_pci::negotiate_features(g_keyboard_device, 0, virtio_pci::kFeatureVersion1Bit)) {
        fail_keyboard_device("feature negotiation failed");
        return;
    }
    if (!setup_event_queue(g_keyboard_device, g_keyboard_event_queue)) {
        fail_keyboard_device("failed to initialize event queue");
        return;
    }

    virtio_pci::set_device_status(
        g_keyboard_device, static_cast<uint8_t>(virtio_pci::device_status(g_keyboard_device) | virtio_pci::kStatusDriverOk));
    g_keyboard_ready = true;
    console::printf(
        "virtio-input: keyboard ready pci=%x:%x.%u\n",
        static_cast<unsigned>(g_keyboard_device.pci_device.bus),
        static_cast<unsigned>(g_keyboard_device.pci_device.slot),
        static_cast<unsigned>(g_keyboard_device.pci_device.function)
    );
}

void poll_pointer_queue() {
    if (!g_ready || !g_event_queue.enabled || g_event_queue.size == 0) {
        return;
    }

    const virtio_pci::UsedHeader* used = virtio_pci::queue_used_header(g_event_queue);
    const virtio_pci::UsedElement* ring = virtio_pci::queue_used_ring(g_event_queue);
    bool notified = false;

    while (g_event_queue.last_used_index != used->idx) {
        virtio_pci::memory_barrier();
        const virtio_pci::UsedElement element = ring[g_event_queue.last_used_index % g_event_queue.size];
        if (element.id < g_event_queue.size) {
            const VirtioInputEvent* event = queue_event_buffer(g_event_queue, static_cast<uint16_t>(element.id));
            process_pointer_event(*event);
            if (virtio_pci::submit_descriptor_head(g_event_queue, static_cast<uint16_t>(element.id))) {
                notified = true;
            }
        }
        g_event_queue.last_used_index = static_cast<uint16_t>(g_event_queue.last_used_index + 1);
    }

    if (notified) {
        virtio_pci::memory_barrier();
        virtio_pci::notify_queue(g_device, g_event_queue);
    }
}

void poll_keyboard_queue() {
    if (!g_keyboard_ready || !g_keyboard_event_queue.enabled || g_keyboard_event_queue.size == 0) {
        return;
    }

    const virtio_pci::UsedHeader* used = virtio_pci::queue_used_header(g_keyboard_event_queue);
    const virtio_pci::UsedElement* ring = virtio_pci::queue_used_ring(g_keyboard_event_queue);
    bool notified = false;

    while (g_keyboard_event_queue.last_used_index != used->idx) {
        virtio_pci::memory_barrier();
        const virtio_pci::UsedElement element = ring[g_keyboard_event_queue.last_used_index % g_keyboard_event_queue.size];
        if (element.id < g_keyboard_event_queue.size) {
            const VirtioInputEvent* event = queue_event_buffer(g_keyboard_event_queue, static_cast<uint16_t>(element.id));
            process_keyboard_event(*event);
            if (virtio_pci::submit_descriptor_head(g_keyboard_event_queue, static_cast<uint16_t>(element.id))) {
                notified = true;
            }
        }
        g_keyboard_event_queue.last_used_index = static_cast<uint16_t>(g_keyboard_event_queue.last_used_index + 1);
    }

    if (notified) {
        virtio_pci::memory_barrier();
        virtio_pci::notify_queue(g_keyboard_device, g_keyboard_event_queue);
    }
}

} // namespace

namespace virtio_input {

void poll();
void set_screen_extent(uint32_t width, uint32_t height);
void reset_pointer();

void initialize(const boot::FramebufferInfo& framebuffer) {
    memset(&g_device, 0, sizeof(g_device));
    memset(&g_event_queue, 0, sizeof(g_event_queue));
    memset(&g_keyboard_device, 0, sizeof(g_keyboard_device));
    memset(&g_keyboard_event_queue, 0, sizeof(g_keyboard_event_queue));
    g_ready = false;
    g_keyboard_ready = false;
    g_buttons = 0;
    g_pending_wheel = 0;
    g_have_abs_x = false;
    g_have_abs_y = false;
    g_have_screen_position = false;
    g_framebuffer_width = framebuffer.available ? static_cast<uint32_t>(framebuffer.width) : 0u;
    g_framebuffer_height = framebuffer.available ? static_cast<uint32_t>(framebuffer.height) : 0u;

    if (!pci::ready()) {
        return;
    }

    // Tablet y teclado son el mismo par device/subsystem virtio-input; QEMU
    // los expone como dos PCI functions separadas (-device virtio-tablet-pci
    // y -device virtio-keyboard-pci), asi que hay que recorrer todos los
    // matches -- find_modern_device se hubiera quedado con el primero nada
    // mas -- y clasificar cada uno por lo que declara soportar.
    size_t search_from = 0;
    pci::DeviceInfo pci_device = {};
    size_t found_index = 0;
    while (virtio_pci::find_modern_device_from(search_from, kVirtioInputModernDevice, kVirtioInputSubsystemDevice, pci_device, found_index)) {
        search_from = found_index + 1;
        if (!g_ready && try_initialize_pointer(pci_device)) {
            continue;
        }
        if (!g_keyboard_ready) {
            try_initialize_keyboard(pci_device);
        }
    }

    if (g_ready || g_keyboard_ready) {
        (void)input::register_driver({
            .name = "virtio-input",
            .keyboard = g_keyboard_ready,
            .pointer = g_ready,
            .poll = poll,
            .set_keyboard_leds = nullptr,
            .set_screen_extent = g_ready ? set_screen_extent : nullptr,
            .reset_pointer = g_ready ? reset_pointer : nullptr,
        });
    }
}

void poll() {
    poll_pointer_queue();
    poll_keyboard_queue();
}

void set_screen_extent(uint32_t width, uint32_t height) {
    g_framebuffer_width = width;
    g_framebuffer_height = height;
    refresh_pointer_range();
}

void reset_pointer() {
    g_have_screen_position = false;
}

} // namespace virtio_input
