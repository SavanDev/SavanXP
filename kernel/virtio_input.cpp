#include "kernel/virtio_input.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/console.hpp"
#include "kernel/inventory.hpp"
#include "kernel/lifecycle.hpp"
#include "kernel/lifecycle_selftest.hpp"
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

/* ---- start and stop (docs/kernel/DRIVER_LIFECYCLE.md) ---------------------
 *
 * Tablet and keyboard are the same virtio-input device and subsystem pair:
 * QEMU offers them as two PCI functions (virtio-tablet-pci and
 * virtio-keyboard-pci), told apart only by what each says it supports. So
 * each function is one instance, and start() gives it a role once its
 * transport is up and its EV_ABS bit read: the pointer if it has absolute
 * axes, the keyboard if not. Each role keeps its own record of what it
 * acquired. */

constexpr char kDriverName[] = "virtio-input";
// transport, classify, features, ranges (the pointer's only), queue,
// driver ok.
constexpr unsigned kStartStages = 6;

struct Role {
    const char* name;
    virtio_pci::Device* device;
    virtio_pci::Queue* queue;
    bool* ready;
    lifecycle::Acquired acquired;
    bool started;
    // Set while a failed start unwinds: the function is then given back as
    // a failed start, not as a clean stop.
    bool start_failing;
};

Role g_pointer_role = {"tablet", &g_device, &g_event_queue, &g_ready, {}, false, false};
Role g_keyboard_role = {"keyboard", &g_keyboard_device, &g_keyboard_event_queue, &g_keyboard_ready, {}, false, false};

void release_transport(void* context) {
    Role& role = *static_cast<Role*>(context);
    virtio_pci::release_device(*role.device, kDriverName, role.start_failing);
}

void release_queue(void* context) {
    Role& role = *static_cast<Role*>(context);
    virtio_pci::release_queue(*role.queue);
}

bool stage_failed(unsigned stage, const char* name) {
    if (!lifecycle::injected(kDriverName, stage)) {
        return false;
    }
    console::printf("virtio-input: start stage %u (%s) failed by injection\n", stage, name);
    return true;
}

bool same_function(const pci::DeviceInfo& left, const pci::DeviceInfo& right) {
    return left.bus == right.bus && left.slot == right.slot && left.function == right.function;
}

// The role a started function plays; nullptr when it is not started.
Role* role_of(const pci::DeviceInfo& pci_device) {
    Role* const roles[] = {&g_pointer_role, &g_keyboard_role};
    for (Role* role : roles) {
        if (role->started && same_function(role->device->pci_device, pci_device)) {
            return role;
        }
    }
    return nullptr;
}

// The nth virtio-input function no other driver holds and this one does not
// drive already (pci_available grants a driver its own functions).
bool find_device(size_t nth, pci::DeviceInfo& found) {
    size_t from = 0;
    size_t index = 0;
    size_t seen = 0;
    pci::DeviceInfo candidate = {};
    while (pci::ready() && virtio_pci::find_modern_device_from(from, kVirtioInputModernDevice,
               kVirtioInputSubsystemDevice, candidate, index)) {
        from = index + 1;
        if (inventory::pci_available(candidate, kDriverName) && role_of(candidate) == nullptr && seen++ == nth) {
            found = candidate;
            return true;
        }
    }
    return false;
}

// The transport, the role (pointer or keyboard), the features, the
// pointer's axis ranges, the event queue, then DRIVER_OK. False with
// everything given back; a function with no role left (a second tablet) is
// given back clean, as one this driver does not use.
bool start(const pci::DeviceInfo& pci_device) {
    unsigned stage = 1;
    if (stage_failed(stage, "transport")) {
        return false;
    }
    virtio_pci::Device device = {};
    if (!virtio_pci::initialize_device(pci_device, true, device)) {
        console::write_line("virtio-input: missing required MMIO capabilities");
        // Given back if it was taken here; left alone if another driver has it.
        (void)inventory::pci_release_failed(pci_device, kDriverName);
        return false;
    }

    ++stage;
    if (stage_failed(stage, "classify")) {
        virtio_pci::release_device(device, kDriverName, true);
        return false;
    }
    virtio_pci::set_device_status(device, 0);
    virtio_pci::memory_barrier();
    virtio_pci::set_device_status(device,
        static_cast<uint8_t>(virtio_pci::kStatusAcknowledge | virtio_pci::kStatusDriver));
    const bool pointer = device_advertises_ev(device, kEvAbs);
    Role* chosen = pointer ? &g_pointer_role : &g_keyboard_role;
    if (chosen->started) {
        // Another tablet or keyboard: this driver drives one of each.
        (void)virtio_pci::quiesce_device(device);
        virtio_pci::release_device(device, kDriverName, false);
        return false;
    }
    Role& role = *chosen;
    *role.device = device;
    memset(role.queue, 0, sizeof(*role.queue));
    bool ok = role.acquired.push("transport", &release_transport, &role);
    if (!ok) {
        virtio_pci::release_device(*role.device, kDriverName, true);
        return false;
    }

    ++stage;
    ok = !stage_failed(stage, "features") &&
        virtio_pci::negotiate_features(*role.device, 0, virtio_pci::kFeatureVersion1Bit);
    if (ok) {
        ++stage;
        ok = !stage_failed(stage, "ranges");
        if (ok && pointer &&
            (!read_abs_info(*role.device, kAbsX, g_abs_min_x, g_abs_max_x) ||
                !read_abs_info(*role.device, kAbsY, g_abs_min_y, g_abs_max_y))) {
            console::write_line("virtio-input: failed to read ABS ranges");
            ok = false;
        }
    }
    if (ok) {
        ++stage;
        ok = !stage_failed(stage, "queue") && role.acquired.push("queue", &release_queue, &role);
        if (ok && !setup_event_queue(*role.device, *role.queue)) {
            console::write_line("virtio-input: failed to initialize event queue");
            ok = false;
        }
    }
    if (ok) {
        ++stage;
        ok = !stage_failed(stage, "driver ok");
        if (ok) {
            virtio_pci::set_device_status(*role.device,
                static_cast<uint8_t>(virtio_pci::device_status(*role.device) | virtio_pci::kStatusDriverOk));
        }
    }
    if (!ok) {
        console::printf("virtio-input: could not start the %s at %x:%x.%u (stage %u); everything it took is "
            "given back\n", role.name, static_cast<unsigned>(pci_device.bus),
            static_cast<unsigned>(pci_device.slot), static_cast<unsigned>(pci_device.function), stage);
        // Reset first: a queue the device may still use is never freed.
        (void)virtio_pci::quiesce_device(*role.device);
        role.start_failing = true;
        role.acquired.unwind();
        role.start_failing = false;
        return false;
    }

    role.started = true;
    *role.ready = true;
    if (pointer) {
        console::printf(
            "virtio-input: tablet ready pci=%x:%x.%u abs_x=%d..%d abs_y=%d..%d\n",
            static_cast<unsigned>(pci_device.bus),
            static_cast<unsigned>(pci_device.slot),
            static_cast<unsigned>(pci_device.function),
            static_cast<int>(g_abs_min_x),
            static_cast<int>(g_abs_max_x),
            static_cast<int>(g_abs_min_y),
            static_cast<int>(g_abs_max_y)
        );
    } else {
        console::printf("virtio-input: keyboard ready pci=%x:%x.%u\n", static_cast<unsigned>(pci_device.bus),
            static_cast<unsigned>(pci_device.slot), static_cast<unsigned>(pci_device.function));
    }
    return true;
}

// Reset and read back, then the queue and the function given back. A
// device that does not reset keeps everything and is listed as failed.
bool stop(const pci::DeviceInfo& pci_device) {
    Role* role = role_of(pci_device);
    if (role == nullptr) {
        return true;
    }
    *role->ready = false;
    if (!virtio_pci::quiesce_device(*role->device)) {
        console::printf("virtio-input: the %s did not reset; it keeps its queue\n", role->name);
        inventory::pci_failed(pci_device);
        return false;
    }
    role->acquired.unwind();
    role->started = false;
    return true;
}

bool idle(const pci::DeviceInfo& pci_device) {
    return role_of(pci_device) == nullptr && (g_pointer_role.started || g_pointer_role.acquired.count() == 0) &&
        (g_keyboard_role.started || g_keyboard_role.acquired.count() == 0);
}

const lifecycle::Subject kSubject = {
    kDriverName,
    kStartStages,
    &find_device,
    &start,
    nullptr,
    &stop,
    &idle,
    nullptr,
};

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
    g_buttons = 0;
    g_pending_wheel = 0;
    g_have_abs_x = false;
    g_have_abs_y = false;
    g_have_screen_position = false;
    g_framebuffer_width = framebuffer.available ? static_cast<uint32_t>(framebuffer.width) : 0u;
    g_framebuffer_height = framebuffer.available ? static_cast<uint32_t>(framebuffer.height) : 0u;

    // Every function is tried: each start finds its role (see start()), and
    // one this driver has no role left for is given back. A function started
    // is no longer available, so the next one is always index 0.
    pci::DeviceInfo pci_device = {};
    size_t skipped = 0;
    while (find_device(skipped, pci_device)) {
        if (!start(pci_device)) {
            ++skipped;
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

const lifecycle::Subject& lifecycle_subject() { return kSubject; }

} // namespace virtio_input
