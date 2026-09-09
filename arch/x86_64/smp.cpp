#include "kernel/smp.hpp"

#include "kernel/console.hpp"
#include "kernel/cpu.hpp"

namespace {

// Cuanto se espera a que un AP se reporte, y cuanto a que llegue un ping. Son
// vueltas de spin, no tiempo: esto corre con las interrupciones apagadas y sin
// timer, asi que no hay reloj del que colgarse. El orden de magnitud alcanza
// para el peor caso razonable (TCG) sin colgar el boot si un core no arranca.
constexpr uint64_t kStartupSpinLimit = 200000000ull;
constexpr uint64_t kPingSpinLimit = 100000000ull;

struct Cpu {
    uint32_t processor_id;
    uint32_t lapic_id;
    volatile uint64_t* start_slot;
    // Escrita por el AP cuando ya acepta interrupciones; leida por el BSP.
    volatile uint32_t online;
};

Cpu g_cpus[smp::kMaxCpus] = {};
uint32_t g_cpu_entries = 0;
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

} // namespace

// Punto de entrada de cada AP. Lo llama el bootloader con la struct del core en
// RDI, que no se usa: el core se identifica leyendo su propio APIC local, que es
// lo mismo que va a hacer la fase 1 para indexar su estado por CPU.
//
// Todo lo que corre aca tiene que ser seguro sin ningun lock, porque todavia no
// hay ninguno. En particular NO se puede imprimir: la consola es estado global
// compartido. El AP solo toca registros de su propio core y atomicos.
extern "C" void savanxp_ap_entry(void*) {
    arch::x86_64::ap_initialize_cpu();
    arch::x86_64::ap_initialize_local_apic();

    const uint32_t lapic_id = arch::x86_64::local_apic_id();

    // Habilitar antes de reportarse, para que "online" signifique tambien "ya
    // acepta IPIs". El unico vector que puede llegarle a un AP es el ping: los
    // GSI del IOAPIC estan ruteados al BSP y su timer local nunca se arranca.
    arch::x86_64::enable_interrupts();

    for (uint32_t index = 0; index < g_cpu_entries; ++index) {
        if (g_cpus[index].lapic_id == lapic_id) {
            __atomic_store_n(&g_cpus[index].online, 1u, __ATOMIC_RELEASE);
            break;
        }
    }

    // Estacionado. La fase 1 le da estado propio y la fase 2 lo mete al
    // scheduler; hasta entonces despertarse por un IPI y volver a dormir es
    // todo lo que hace.
    for (;;) {
        arch::x86_64::halt_once();
    }
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
    return index < g_cpu_entries ? g_cpus[index].lapic_id : 0;
}

bool initialize(const boot::BootInfo& boot_info) {
    if (g_ready) {
        return g_online > 1;
    }
    g_ready = true;

    const boot::SmpInfo& info = boot_info.smp;
    if (!info.available || info.cpus == nullptr || info.cpu_entries == 0) {
        console::printf("smp: el bootloader no reporto MP, un solo core\n");
        return false;
    }

    g_reported_cpu_count = info.cpu_count;
    g_bsp_lapic_id = info.bsp_lapic_id;
    g_cpu_entries = static_cast<uint32_t>(
        info.cpu_entries < kMaxCpus ? info.cpu_entries : kMaxCpus);

    for (uint32_t index = 0; index < g_cpu_entries; ++index) {
        g_cpus[index].processor_id = info.cpus[index].processor_id;
        g_cpus[index].lapic_id = info.cpus[index].lapic_id;
        g_cpus[index].start_slot = info.cpus[index].start_slot;
        g_cpus[index].online = info.cpus[index].start_slot == nullptr ? 1u : 0u;
    }

    // La tabla tiene que estar entera antes de que arranque el primer AP: en
    // cuanto se escribe una casilla, ese core ya esta leyendo g_cpus.
    __atomic_thread_fence(__ATOMIC_RELEASE);

    uint32_t launched = 0;
    for (uint32_t index = 0; index < g_cpu_entries; ++index) {
        volatile uint64_t* slot = g_cpus[index].start_slot;
        if (slot == nullptr) {
            continue; // el BSP
        }
        __atomic_store_n(
            slot,
            reinterpret_cast<uint64_t>(&savanxp_ap_entry),
            __ATOMIC_RELEASE);
        ++launched;
    }

    // Esperar a que se reporten. Un core que no contesta no cuelga el boot: se
    // lo deja afuera y se sigue con los que hay.
    for (uint32_t index = 0; index < g_cpu_entries; ++index) {
        if (g_cpus[index].start_slot == nullptr) {
            continue;
        }
        for (uint64_t spin = 0; spin < kStartupSpinLimit; ++spin) {
            if (__atomic_load_n(&g_cpus[index].online, __ATOMIC_ACQUIRE) != 0) {
                break;
            }
            spin_hint();
        }
    }

    g_online = 0;
    for (uint32_t index = 0; index < g_cpu_entries; ++index) {
        if (__atomic_load_n(&g_cpus[index].online, __ATOMIC_ACQUIRE) != 0) {
            ++g_online;
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
    for (uint32_t index = 0; index < g_cpu_entries; ++index) {
        if (g_cpus[index].start_slot == nullptr ||
            __atomic_load_n(&g_cpus[index].online, __ATOMIC_ACQUIRE) == 0) {
            continue;
        }
        if (!arch::x86_64::send_ipi(
                g_cpus[index].lapic_id, arch::x86_64::kIpiPingVector)) {
            console::printf(
                "smp: fallo el IPI al lapic %u\n",
                static_cast<unsigned>(g_cpus[index].lapic_id));
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
