#include "kernel/smp.hpp"

#include "kernel/console.hpp"
#include "kernel/cpu.hpp"

namespace smp {

Cpu g_cpu_state[kMaxCpus] = {};

} // namespace smp

namespace {

// Cuanto se espera a que un AP se reporte, y cuanto a que llegue un ping. Son
// vueltas de spin, no tiempo: esto corre con las interrupciones apagadas y sin
// timer, asi que no hay reloj del que colgarse. El orden de magnitud alcanza
// para el peor caso razonable (TCG) sin colgar el boot si un core no arranca.
constexpr uint64_t kStartupSpinLimit = 200000000ull;
constexpr uint64_t kPingSpinLimit = 100000000ull;

// kernel_index de un core que no se arranca (no entra en kMaxCpus).
constexpr uint32_t kNoIndex = 0xffffffffu;

// Un core tal como lo reporto el bootloader, en su orden. No confundir con
// smp::Cpu: esto es la libreta del arranque, en el orden de Limine; el estado de
// cada core se indexa por cpu_index(), que es otro orden.
struct BootSlot {
    uint32_t processor_id;
    uint32_t lapic_id;
    volatile uint64_t* start_slot;
    // Indice denso que le toca (0 = BSP), o kNoIndex si no se arranca.
    uint32_t kernel_index;
    // Lo que leyo `str` el propio AP despues de cargar su TSS. El BSP lo compara
    // con el esperado: es la prueba de que cpu_index() identifica bien al core.
    volatile uint16_t loaded_selector;
    // Escrita por el AP cuando ya acepta interrupciones; leida por el BSP.
    volatile uint32_t online;
};

BootSlot g_slots[smp::kMaxCpus] = {};
uint32_t g_slot_count = 0;
uint64_t g_reported_cpu_count = 1;
uint32_t g_bsp_lapic_id = 0;
uint32_t g_online = 1;
bool g_ready = false;

// Pings recibidos entre todos los APs. Solo la comprueba el autotest de arranque.
volatile uint64_t g_ping_count = 0;

void ping_handler() {
    __atomic_add_fetch(&g_ping_count, 1ull, __ATOMIC_ACQ_REL);
}

void spin_hint() {
    asm volatile("pause");
}

uint16_t expected_selector(uint32_t kernel_index) {
    return static_cast<uint16_t>(
        arch::x86_64::kTssSelectorBase + kernel_index * arch::x86_64::kTssDescriptorSize);
}

[[noreturn]] void park() {
    for (;;) {
        arch::x86_64::halt_once();
    }
}

} // namespace

// Punto de entrada de cada AP. Lo llama el bootloader con la struct del core en
// RDI, que no se usa: el core se identifica leyendo su propio APIC local.
//
// Todo lo que corre aca tiene que ser seguro sin ningun lock, porque todavia no
// hay ninguno. En particular NO se puede imprimir: la consola es estado global
// compartido. El AP solo toca registros de su propio core, su BootSlot y
// atomicos. Su smp::Cpu ya la dejo lista el BSP antes de lanzarlo.
extern "C" void savanxp_ap_entry(void*) {
    arch::x86_64::ap_initialize_cpu();
    arch::x86_64::ap_initialize_local_apic();

    const uint32_t lapic_id = arch::x86_64::local_apic_id();

    BootSlot* slot = nullptr;
    for (uint32_t index = 0; index < g_slot_count; ++index) {
        if (g_slots[index].lapic_id == lapic_id) {
            slot = &g_slots[index];
            break;
        }
    }

    // Sin su entrada no sabe que TSS le toca, y cargar uno ajeno le pisaria el
    // rsp0 a otro core. Se queda dormido, con las interrupciones apagadas y sin
    // reportarse: el BSP lo cuenta como que no respondio.
    if (slot == nullptr || slot->kernel_index == kNoIndex) {
        park();
    }

    arch::x86_64::load_task_register(slot->kernel_index);
    // TR leido de verdad y no cpu_index(): aquel asm el compilador lo puede
    // adelantar por encima del `ltr`, y esto es justo lo que el BSP verifica.
    slot->loaded_selector = arch::x86_64::read_task_register();

    // Habilitar antes de reportarse, para que "online" signifique tambien "ya
    // acepta IPIs". El unico vector que puede llegarle a un AP es el ping: los
    // GSI del IOAPIC estan ruteados al BSP y su timer local nunca se arranca.
    arch::x86_64::enable_interrupts();
    __atomic_store_n(&slot->online, 1u, __ATOMIC_RELEASE);

    // Estacionado. La fase 2 lo mete al scheduler; hasta entonces despertarse
    // por un IPI y volver a dormir es todo lo que hace.
    park();
}

namespace smp {

uint32_t cpu_count() {
    return static_cast<uint32_t>(g_reported_cpu_count);
}

uint32_t online_count() {
    return g_online;
}

uint32_t bsp_lapic_id() {
    return g_bsp_lapic_id;
}

uint32_t lapic_id_of(uint32_t index) {
    return index < kMaxCpus ? g_cpu_state[index].lapic_id : 0;
}

bool initialize(const boot::BootInfo& boot_info) {
    if (g_ready) {
        return g_online > 1;
    }
    g_ready = true;

    // El BSP es el core 0 haya o no informacion de MP: cargo el TSS 0 en
    // initialize_cpu(), asi que this_cpu() ya lo resolvia desde entonces.
    g_cpu_state[0].index = 0;
    g_cpu_state[0].lapic_id = arch::x86_64::local_apic_id();
    if (arch::x86_64::read_task_register() != expected_selector(0)) {
        console::printf("smp: el BSP no tiene cargado el TSS 0\n");
    }

    const boot::SmpInfo& info = boot_info.smp;
    if (!info.available || info.cpus == nullptr || info.cpu_entries == 0) {
        console::printf("smp: el bootloader no reporto MP, un solo core\n");
        return false;
    }

    g_reported_cpu_count = info.cpu_count;
    g_bsp_lapic_id = info.bsp_lapic_id;
    g_slot_count = static_cast<uint32_t>(
        info.cpu_entries < kMaxCpus ? info.cpu_entries : kMaxCpus);

    // Indices densos: el BSP es el 0 y los APs siguen en el orden en que los
    // reporto el bootloader. El que no entra en kMaxCpus se reporta y no arranca.
    uint32_t next_index = 1;
    for (uint32_t index = 0; index < g_slot_count; ++index) {
        BootSlot& slot = g_slots[index];
        slot.processor_id = info.cpus[index].processor_id;
        slot.lapic_id = info.cpus[index].lapic_id;
        slot.start_slot = info.cpus[index].start_slot;
        slot.loaded_selector = 0;

        if (slot.start_slot == nullptr) {
            slot.kernel_index = 0; // el BSP
            slot.online = 1;
            continue;
        }

        slot.online = 0;
        if (next_index >= kMaxCpus) {
            slot.kernel_index = kNoIndex;
            continue;
        }
        slot.kernel_index = next_index++;
        g_cpu_state[slot.kernel_index].index = slot.kernel_index;
        g_cpu_state[slot.kernel_index].lapic_id = slot.lapic_id;
    }

    // Las dos tablas tienen que estar enteras antes de que arranque el primer
    // AP: en cuanto se escribe una casilla, ese core ya las esta leyendo.
    __atomic_thread_fence(__ATOMIC_RELEASE);

    uint32_t launched = 0;
    for (uint32_t index = 0; index < g_slot_count; ++index) {
        BootSlot& slot = g_slots[index];
        if (slot.start_slot == nullptr || slot.kernel_index == kNoIndex) {
            continue;
        }
        __atomic_store_n(
            slot.start_slot,
            reinterpret_cast<uint64_t>(&savanxp_ap_entry),
            __ATOMIC_RELEASE);
        ++launched;
    }

    // Esperar a que se reporten. Un core que no contesta no cuelga el boot: se
    // lo deja afuera y se sigue con los que hay.
    for (uint32_t index = 0; index < g_slot_count; ++index) {
        BootSlot& slot = g_slots[index];
        if (slot.start_slot == nullptr || slot.kernel_index == kNoIndex) {
            continue;
        }
        for (uint64_t spin = 0; spin < kStartupSpinLimit; ++spin) {
            if (__atomic_load_n(&slot.online, __ATOMIC_ACQUIRE) != 0) {
                break;
            }
            spin_hint();
        }
    }

    g_online = 0;
    uint32_t aps_online = 0;
    uint32_t tss_verified = 0;
    for (uint32_t index = 0; index < g_slot_count; ++index) {
        BootSlot& slot = g_slots[index];
        if (__atomic_load_n(&slot.online, __ATOMIC_ACQUIRE) == 0) {
            continue;
        }
        ++g_online;
        if (slot.start_slot == nullptr) {
            continue; // el BSP, ya verificado arriba
        }
        ++aps_online;
        if (slot.loaded_selector == expected_selector(slot.kernel_index)) {
            ++tss_verified;
        }
    }

    console::printf(
        "smp: %u cores reportados, %u en linea (bsp lapic %u, %s)\n",
        static_cast<unsigned>(g_reported_cpu_count),
        static_cast<unsigned>(g_online),
        static_cast<unsigned>(g_bsp_lapic_id),
        info.x2apic ? "x2APIC" : "xAPIC");

    // Estrictamente menor, no distinto: contar hacia abajo desde un g_online
    // inesperadamente alto daria la vuelta y reportaria un numero absurdo.
    if (g_online < launched + 1) {
        console::printf(
            "smp: %u de %u APs no respondieron\n",
            static_cast<unsigned>(launched + 1 - g_online),
            static_cast<unsigned>(launched));
    }

    if (aps_online != 0) {
        if (tss_verified == aps_online) {
            console::printf(
                "smp: TSS propio en %u/%u APs ok\n",
                static_cast<unsigned>(tss_verified),
                static_cast<unsigned>(aps_online));
        } else {
            console::printf(
                "smp: TSS propio en %u/%u APs -- cpu_index() no identifica al core\n",
                static_cast<unsigned>(tss_verified),
                static_cast<unsigned>(aps_online));
        }
    }

    return g_online > 1;
}

bool selftest_ipi() {
    if (g_online <= 1) {
        return true; // nada que probar
    }

    if (!arch::x86_64::register_interrupt_handler(
            arch::x86_64::kIpiPingVector,
            ping_handler,
            arch::x86_64::InterruptEoi::local_apic)) {
        console::printf("smp: no se pudo registrar el vector de ping\n");
        return false;
    }

    uint32_t expected = 0;
    for (uint32_t index = 0; index < g_slot_count; ++index) {
        BootSlot& slot = g_slots[index];
        if (slot.start_slot == nullptr ||
            __atomic_load_n(&slot.online, __ATOMIC_ACQUIRE) == 0) {
            continue;
        }
        if (!arch::x86_64::send_ipi(slot.lapic_id, arch::x86_64::kIpiPingVector)) {
            console::printf(
                "smp: fallo el IPI al lapic %u\n",
                static_cast<unsigned>(slot.lapic_id));
            continue;
        }
        ++expected;
    }

    for (uint64_t spin = 0; spin < kPingSpinLimit; ++spin) {
        if (__atomic_load_n(&g_ping_count, __ATOMIC_ACQUIRE) >= expected) {
            break;
        }
        spin_hint();
    }

    const uint64_t received = __atomic_load_n(&g_ping_count, __ATOMIC_ACQUIRE);
    if (received < expected) {
        console::printf(
            "smp: ping IPI %u/%u -- el ICR no entrega\n",
            static_cast<unsigned>(received),
            static_cast<unsigned>(expected));
        return false;
    }

    console::printf(
        "smp: ping IPI %u/%u ok\n",
        static_cast<unsigned>(received),
        static_cast<unsigned>(expected));
    return true;
}

} // namespace smp
