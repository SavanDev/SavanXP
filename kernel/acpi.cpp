#include "kernel/acpi.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/console.hpp"
#include "kernel/cpu.hpp"
#include "kernel/ioapic.hpp"
#include "kernel/string.hpp"

namespace {

// --- Port I/O (local al archivo, mismo patron que pcspeaker.cpp) ---

inline void out8(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

inline uint8_t in8(uint16_t port) {
    uint8_t value = 0;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

inline void out16(uint16_t port, uint16_t value) {
    asm volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

inline uint16_t in16(uint16_t port) {
    uint16_t value = 0;
    asm volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

void io_wait() {
    out8(0x80, 0);
}

void short_delay() {
    for (int i = 0; i < 1000; ++i) {
        io_wait();
    }
}

// --- Estructuras ACPI (offsets fijos del spec) ---

struct [[gnu::packed]] RsdpDescriptor {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
    // Campos v2 (solo validos si revision >= 2):
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
};

struct [[gnu::packed]] SdtHeader {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
};

struct [[gnu::packed]] GenericAddress {
    uint8_t address_space_id; // 0 = memoria del sistema, 1 = I/O
    uint8_t bit_width;
    uint8_t bit_offset;
    uint8_t access_size;
    uint64_t address;
};

struct [[gnu::packed]] Fadt {
    SdtHeader header;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t reserved0;
    uint8_t preferred_pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t acpi_enable;
    uint8_t acpi_disable;
    uint8_t s4bios_req;
    uint8_t pstate_cnt;
    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;
    uint32_t pm1b_cnt_blk;
    uint32_t pm2_cnt_blk;
    uint32_t pm_tmr_blk;
    uint32_t gpe0_blk;
    uint32_t gpe1_blk;
    uint8_t pm1_evt_len;
    uint8_t pm1_cnt_len;
    uint8_t pm2_cnt_len;
    uint8_t pm_tmr_len;
    uint8_t gpe0_blk_len;
    uint8_t gpe1_blk_len;
    uint8_t gpe1_base;
    uint8_t cst_cnt;
    uint16_t p_lvl2_lat;
    uint16_t p_lvl3_lat;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t duty_offset;
    uint8_t duty_width;
    uint8_t day_alrm;
    uint8_t mon_alrm;
    uint8_t century;
    uint16_t iapc_boot_arch;
    uint8_t reserved1;
    uint32_t flags;
    GenericAddress reset_reg;
    uint8_t reset_value;
    uint16_t arm_boot_arch;
    uint8_t fadt_minor_version;
    uint64_t x_firmware_ctrl;
    uint64_t x_dsdt;
    GenericAddress x_pm1a_evt_blk;
    GenericAddress x_pm1b_evt_blk;
    GenericAddress x_pm1a_cnt_blk;
    GenericAddress x_pm1b_cnt_blk;
};

constexpr uint32_t kFadtFlagResetRegSupported = 1u << 10;

// --- Estado del modulo ---

uint64_t g_hhdm_offset = 0;
bool g_ready = false;

uint16_t g_pm1a_cnt_port = 0;
uint16_t g_pm1b_cnt_port = 0;
uint8_t g_slp_typ_a = 0;
uint8_t g_slp_typ_b = 0;
bool g_s5_available = false;

bool g_reset_supported = false;
GenericAddress g_reset_reg = {};
uint8_t g_reset_value = 0;

// SCI + bloques de evento PM1 + GPE (para atender eventos fijos: boton de power).
uint16_t g_sci_int = 0;
uint16_t g_pm1a_evt_port = 0;
uint16_t g_pm1b_evt_port = 0;
uint8_t g_pm1_evt_len = 0;
uint16_t g_smi_cmd = 0;
uint8_t g_acpi_enable = 0;
uint16_t g_gpe0_port = 0;
uint8_t g_gpe0_len = 0;
uint16_t g_gpe1_port = 0;
uint8_t g_gpe1_len = 0;
bool g_sci_active = false;

// Bits de PM1 (status/enable comparten posiciones) y de PM1_CNT.
constexpr uint16_t kPm1PwrBtnBit = 1u << 8; // PWRBTN_STS / PWRBTN_EN
constexpr uint16_t kPm1CntSciEn = 1u << 0;  // SCI_EN en PM1a_CNT

// Limine entrega el RSDP como puntero HHDM ya virtual, mientras que las tablas
// ACPI guardan direcciones fisicas. Normalizamos: si la direccion ya cae en el
// half alto la usamos tal cual, si no le sumamos el offset HHDM.
template <typename T>
T* acpi_map(uint64_t address) {
    if (address >= g_hhdm_offset) {
        return reinterpret_cast<T*>(address);
    }
    return reinterpret_cast<T*>(address + g_hhdm_offset);
}

bool checksum_ok(const void* data, size_t length) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    uint8_t sum = 0;
    for (size_t i = 0; i < length; ++i) {
        sum = static_cast<uint8_t>(sum + bytes[i]);
    }
    return sum == 0;
}

const SdtHeader* find_table(const RsdpDescriptor* rsdp, const char (&signature)[5]) {
    const bool use_xsdt = rsdp->revision >= 2 && rsdp->xsdt_address != 0;
    const SdtHeader* root = use_xsdt
        ? acpi_map<SdtHeader>(rsdp->xsdt_address)
        : acpi_map<SdtHeader>(rsdp->rsdt_address);

    const size_t entry_size = use_xsdt ? sizeof(uint64_t) : sizeof(uint32_t);
    const size_t entry_count = (root->length - sizeof(SdtHeader)) / entry_size;
    const uint8_t* entries = reinterpret_cast<const uint8_t*>(root) + sizeof(SdtHeader);

    for (size_t i = 0; i < entry_count; ++i) {
        uint64_t table_phys = 0;
        if (use_xsdt) {
            uint64_t value = 0;
            memcpy(&value, entries + i * entry_size, sizeof(uint64_t));
            table_phys = value;
        } else {
            uint32_t value = 0;
            memcpy(&value, entries + i * entry_size, sizeof(uint32_t));
            table_phys = value;
        }

        const SdtHeader* table = acpi_map<SdtHeader>(table_phys);
        if (memcmp(table->signature, signature, 4) == 0) {
            return table;
        }
    }
    return nullptr;
}

// Escanea la DSDT buscando el objeto AML "_S5_" y extrae SLP_TYPa/SLP_TYPb.
// Tecnica minima estandar de hobby OS: no interpreta AML, solo decodifica el
// paquete que sigue al NameString.
void parse_s5(const SdtHeader* dsdt) {
    const uint8_t* body = reinterpret_cast<const uint8_t*>(dsdt) + sizeof(SdtHeader);
    if (dsdt->length <= sizeof(SdtHeader)) {
        return;
    }
    const size_t body_length = dsdt->length - sizeof(SdtHeader);

    for (size_t i = 0; i + 4 < body_length; ++i) {
        if (memcmp(body + i, "_S5_", 4) != 0) {
            continue;
        }

        // Validar que sea un NameOp (0x08), opcionalmente precedido de scope '\'.
        const bool preceded_by_name_op =
            (i >= 1 && body[i - 1] == 0x08) ||
            (i >= 2 && body[i - 2] == 0x08 && body[i - 1] == '\\');
        const uint8_t* p = body + i + 4;
        if (!preceded_by_name_op || *p != 0x12 /* PackageOp */) {
            continue;
        }

        p += 1;                              // saltar PackageOp
        p += ((*p & 0xC0) >> 6) + 1;         // saltar PkgLength (lead byte + extra)
        p += 1;                              // saltar NumElements

        if (*p == 0x0A) {                    // BytePrefix opcional
            p += 1;
        }
        g_slp_typ_a = static_cast<uint8_t>(*p & 0x07);
        p += 1;

        if (*p == 0x0A) {
            p += 1;
        }
        g_slp_typ_b = static_cast<uint8_t>(*p & 0x07);

        g_s5_available = true;
        return;
    }
}

// Enmascara todas las GPE. Sin interprete AML no podemos atenderlas, y la SCI es
// level-triggered: una GPE habilitada sin handler mantendria la linea asertada y
// causaria una tormenta de interrupciones. Layout de cada bloque GPE:
// [GPE_STS (len/2)][GPE_EN (len/2)]. Escribimos 0xFF al STS (write-1-to-clear) y
// 0 al EN.
void mask_all_gpes() {
    const uint16_t bases[2] = {g_gpe0_port, g_gpe1_port};
    const uint8_t lens[2] = {g_gpe0_len, g_gpe1_len};
    for (int b = 0; b < 2; ++b) {
        if (bases[b] == 0 || lens[b] == 0) {
            continue;
        }
        const uint8_t half = static_cast<uint8_t>(lens[b] / 2);
        for (uint8_t i = 0; i < half; ++i) {
            out8(static_cast<uint16_t>(bases[b] + i), 0xFF);        // STS
            out8(static_cast<uint16_t>(bases[b] + half + i), 0x00); // EN
        }
    }
}

// Handler de la SCI. Solo atendemos eventos fijos de PM1 (hoy: boton de power);
// las GPE estan enmascaradas. Es imprescindible limpiar el estado (write-1-to-
// clear) para que la SCI level-triggered no vuelva a dispararse.
void sci_interrupt() {
    uint16_t status = 0;
    if (g_pm1a_evt_port != 0) {
        status = static_cast<uint16_t>(status | in16(g_pm1a_evt_port));
    }
    if (g_pm1b_evt_port != 0) {
        status = static_cast<uint16_t>(status | in16(g_pm1b_evt_port));
    }

    if ((status & kPm1PwrBtnBit) != 0) {
        // Ack del boton y apagado ordenado por S5 (mismo camino que /dev/power).
        out16(g_pm1a_evt_port, kPm1PwrBtnBit);
        if (g_pm1b_evt_port != 0) {
            out16(g_pm1b_evt_port, kPm1PwrBtnBit);
        }
        acpi::shutdown(); // no retorna
    }

    // Otros bits fijos: limpiarlos para no re-disparar la SCI.
    if (status != 0) {
        out16(g_pm1a_evt_port, status);
        if (g_pm1b_evt_port != 0) {
            out16(g_pm1b_evt_port, status);
        }
    }
}

} // namespace

namespace acpi {

void initialize(const boot::BootInfo& boot_info) {
    g_hhdm_offset = boot_info.hhdm_offset;

    if (boot_info.acpi_rsdp_address == 0 || g_hhdm_offset == 0) {
        console::printf("acpi: RSDP no disponible desde el bootloader\n");
        return;
    }

    const RsdpDescriptor* rsdp = acpi_map<RsdpDescriptor>(boot_info.acpi_rsdp_address);
    if (memcmp(rsdp->signature, "RSD PTR ", 8) != 0 || !checksum_ok(rsdp, 20)) {
        console::printf("acpi: RSDP invalido (firma/checksum)\n");
        return;
    }

    const SdtHeader* fadt_header = find_table(rsdp, "FACP");
    if (fadt_header == nullptr) {
        console::printf("acpi: FADT (FACP) no encontrada\n");
        return;
    }

    const Fadt* fadt = reinterpret_cast<const Fadt*>(fadt_header);

    // PM1 control: preferir el bloque extendido de 64 bits si esta presente.
    const bool has_extended =
        fadt->header.length >= offsetof(Fadt, x_pm1a_cnt_blk) + sizeof(GenericAddress);
    if (has_extended && fadt->x_pm1a_cnt_blk.address != 0) {
        g_pm1a_cnt_port = static_cast<uint16_t>(fadt->x_pm1a_cnt_blk.address);
        g_pm1b_cnt_port = static_cast<uint16_t>(fadt->x_pm1b_cnt_blk.address);
    } else {
        g_pm1a_cnt_port = static_cast<uint16_t>(fadt->pm1a_cnt_blk);
        g_pm1b_cnt_port = static_cast<uint16_t>(fadt->pm1b_cnt_blk);
    }

    // Reset register (ACPI 2.0+).
    if (fadt->header.length >= offsetof(Fadt, reset_value) + sizeof(uint8_t) &&
        (fadt->flags & kFadtFlagResetRegSupported) != 0 &&
        fadt->reset_reg.address != 0) {
        g_reset_supported = true;
        g_reset_reg = fadt->reset_reg;
        g_reset_value = fadt->reset_value;
    }

    // SCI + bloques de evento PM1 + GPE (para start_sci / boton de power).
    g_sci_int = fadt->sci_int;
    g_smi_cmd = static_cast<uint16_t>(fadt->smi_cmd);
    g_acpi_enable = fadt->acpi_enable;
    g_pm1_evt_len = fadt->pm1_evt_len;
    if (has_extended && fadt->x_pm1a_evt_blk.address != 0) {
        g_pm1a_evt_port = static_cast<uint16_t>(fadt->x_pm1a_evt_blk.address);
        g_pm1b_evt_port = static_cast<uint16_t>(fadt->x_pm1b_evt_blk.address);
    } else {
        g_pm1a_evt_port = static_cast<uint16_t>(fadt->pm1a_evt_blk);
        g_pm1b_evt_port = static_cast<uint16_t>(fadt->pm1b_evt_blk);
    }
    g_gpe0_port = static_cast<uint16_t>(fadt->gpe0_blk);
    g_gpe0_len = fadt->gpe0_blk_len;
    g_gpe1_port = static_cast<uint16_t>(fadt->gpe1_blk);
    g_gpe1_len = fadt->gpe1_blk_len;

    // DSDT -> _S5_ (SLP_TYPa/b).
    uint64_t dsdt_phys = fadt->dsdt;
    if (has_extended && fadt->x_dsdt != 0) {
        dsdt_phys = fadt->x_dsdt;
    }
    if (dsdt_phys != 0) {
        const SdtHeader* dsdt = acpi_map<SdtHeader>(dsdt_phys);
        if (memcmp(dsdt->signature, "DSDT", 4) == 0) {
            parse_s5(dsdt);
        }
    }

    g_ready = true;
    console::printf(
        "acpi: FADT ok, PM1a_CNT=0x%x, S5 %s (SLP_TYPa=%u), reset %s\n",
        static_cast<unsigned>(g_pm1a_cnt_port),
        g_s5_available ? "ok" : "ausente",
        static_cast<unsigned>(g_slp_typ_a),
        g_reset_supported ? "ok" : "ausente");
}

bool ready() {
    return g_ready;
}

bool enable_power_button() {
    if (!g_ready || g_pm1a_evt_port == 0) {
        return false;
    }

    const uint16_t en_offset = static_cast<uint16_t>(g_pm1_evt_len / 2);
    const uint16_t pm1a_en = static_cast<uint16_t>(g_pm1a_evt_port + en_offset);
    out16(g_pm1a_evt_port, kPm1PwrBtnBit); // W1C de un evento pendiente viejo
    out16(pm1a_en, static_cast<uint16_t>(in16(pm1a_en) | kPm1PwrBtnBit));
    if (g_pm1b_evt_port != 0) {
        const uint16_t pm1b_en = static_cast<uint16_t>(g_pm1b_evt_port + en_offset);
        out16(g_pm1b_evt_port, kPm1PwrBtnBit);
        out16(pm1b_en, static_cast<uint16_t>(in16(pm1b_en) | kPm1PwrBtnBit));
    }
    return true;
}

bool start_sci() {
    if (!g_ready || g_pm1a_evt_port == 0) {
        // Sin bloque de evento PM1 no hay forma de atender la SCI.
        return false;
    }

    // 1) Habilitar el modo ACPI (SCI_EN) si aun no esta, via SMI_CMD. En modo
    //    legacy la firmware maneja los eventos por SMM; al habilitar ACPI el SO
    //    pasa a recibir la SCI.
    if (g_pm1a_cnt_port != 0 && (in16(g_pm1a_cnt_port) & kPm1CntSciEn) == 0 &&
        g_smi_cmd != 0 && g_acpi_enable != 0) {
        out8(g_smi_cmd, g_acpi_enable);
        for (int i = 0; i < 100000 && (in16(g_pm1a_cnt_port) & kPm1CntSciEn) == 0; ++i) {
            io_wait();
        }
    }

    // 2) Enmascarar las GPE antes de desenmascarar la SCI (evita tormentas).
    mask_all_gpes();

    // 3) Limpiar estado PM1 pendiente, dejar todos los eventos fijos apagados y
    //    habilitar solo el boton de encendido.
    const uint16_t en_offset = static_cast<uint16_t>(g_pm1_evt_len / 2);
    out16(g_pm1a_evt_port, 0xFFFF); // clear-all (write-1-to-clear)
    out16(static_cast<uint16_t>(g_pm1a_evt_port + en_offset), 0);
    if (g_pm1b_evt_port != 0) {
        out16(g_pm1b_evt_port, 0xFFFF);
        out16(static_cast<uint16_t>(g_pm1b_evt_port + en_offset), 0);
    }
    enable_power_button();

    // 4) Rutear la SCI. Por spec ACPI es level-triggered, active-low. Preferir el
    //    IOAPIC; si no hay, caer al PIC legacy cuando la GSI entra en 0..15.
    if (ioapic::ready()) {
        g_sci_active = ioapic::route_gsi(g_sci_int, ioapic::Polarity::active_low,
                                         ioapic::Trigger::level, sci_interrupt) != 0;
    } else if (g_sci_int < 16) {
        g_sci_active = arch::x86_64::register_irq_handler(
            static_cast<uint8_t>(g_sci_int), sci_interrupt);
        if (g_sci_active) {
            arch::x86_64::enable_irq(static_cast<uint8_t>(g_sci_int));
        }
    }

    console::printf("acpi: SCI %s en GSI %u (PWRBTN habilitado)\n",
                    g_sci_active ? "ruteado" : "no ruteado",
                    static_cast<unsigned>(g_sci_int));
    return g_sci_active;
}

[[noreturn]] void shutdown() {
    arch::x86_64::disable_interrupts();

    if (g_s5_available && g_pm1a_cnt_port != 0) {
        const uint16_t value_a = static_cast<uint16_t>((g_slp_typ_a << 10) | (1u << 13));
        out16(g_pm1a_cnt_port, value_a);
        if (g_pm1b_cnt_port != 0) {
            const uint16_t value_b = static_cast<uint16_t>((g_slp_typ_b << 10) | (1u << 13));
            out16(g_pm1b_cnt_port, value_b);
        }
        short_delay();
    }

    // Fallbacks de apagado conocidos por hipervisor.
    out16(0x604, 0x2000);   // QEMU (>= 2.0)
    out16(0xB004, 0x2000);  // Bochs / VirtualBox legacy
    out16(0x4004, 0x3400);  // VirtualBox

    arch::x86_64::halt_forever();
}

[[noreturn]] void reboot() {
    arch::x86_64::disable_interrupts();

    // 1) Reset register de la FADT.
    if (g_reset_supported) {
        if (g_reset_reg.address_space_id == 1) { // I/O
            out8(static_cast<uint16_t>(g_reset_reg.address), g_reset_value);
        } else if (g_reset_reg.address_space_id == 0) { // memoria
            *acpi_map<volatile uint8_t>(g_reset_reg.address) = g_reset_value;
        }
        short_delay();
    }

    // 2) Puerto 0xCF9 (PCI reset control).
    out8(0xCF9, 0x0E);
    short_delay();

    // 3) Controlador de teclado 8042: pulso de reset.
    for (int i = 0; i < 1000 && (in8(0x64) & 0x02) != 0; ++i) {
        io_wait();
    }
    out8(0x64, 0xFE);
    short_delay();

    // 4) Ultimo recurso.
    arch::x86_64::halt_forever();
}

} // namespace acpi
