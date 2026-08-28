#include "kernel/ac97.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/audio.hpp"
#include "kernel/console.hpp"
#include "kernel/pci.hpp"
#include "kernel/physical_memory.hpp"
#include "kernel/process.hpp"
#include "kernel/string.hpp"
#include "savanxp/syscall.h"

namespace {

// --- Formato fijo del ABI ----------------------------------------------------
// AC'97 corre nativamente a 48 kHz, asi que el formato fijo del ABI (S16 estereo
// 48 kHz) es exactamente el rate nativo del DAC frontal: sin VRA ni resampling.
constexpr uint32_t kSampleRateHz = 48000u;
constexpr uint32_t kChannels = 2u;
constexpr uint32_t kBitsPerSample = 16u;
constexpr uint32_t kFrameBytes = 4u;      // 2 canales * 16 bits
constexpr uint32_t kPeriodBytes = 4096u;  // ~21 ms por periodo

// El Buffer Descriptor List del hardware tiene 32 entradas fijas; CIV/LVI son
// indices de 5 bits que envuelven en 32. Usamos las 32 ranuras para que el wrap
// por hardware coincida con el nuestro, pero limitamos cuantos periodos dejamos
// en vuelo (kMaxInFlight) para acotar la latencia a ~170 ms en vez de ~680 ms.
constexpr uint32_t kRingSlots = 32u;
constexpr uint32_t kMaxInFlight = 8u;
// Colchon inicial de periodos de silencio encolados al preparar el stream. El
// productor (Doom) alimenta "lo que paso en tiempo real" por frame; sin colchon
// el ring se vacia entre rafagas y el DMA hace underrun (audio robotico). Como
// en regimen alimentacion == consumo, este offset persiste y absorbe el jitter.
constexpr uint32_t kPrimePeriods = 4u;
constexpr uint32_t kRingBytes = kRingSlots * kPeriodBytes;
// buffer_bytes del ABI es un hint de "cuanto bufferear por delante", no el ring
// fisico. Publicamos 4 periodos (igual que virtio) para que los consumidores que
// lo usan para dimensionar su escritura (audiotest) reciban un valor razonable;
// el ring DMA real es mucho mas grande y lo maneja el driver internamente.
constexpr uint32_t kAdvertisedBufferBytes = 4u * kPeriodBytes;

// --- Registros del mixer (NAM, Native Audio Mixer) ---------------------------
constexpr uint16_t kNamReset = 0x00u;
constexpr uint16_t kNamMasterVolume = 0x02u;
constexpr uint16_t kNamPcmOutVolume = 0x18u;

// --- Registros del bus master (NABM, Native Audio Bus Master) ----------------
// Caja del stream PCM OUT: base 0x10 dentro del NABM.
constexpr uint16_t kPoBdbar = 0x10u;  // dword: base fisica del BDL
constexpr uint16_t kPoCiv = 0x14u;    // byte:  current index value (RO)
constexpr uint16_t kPoLvi = 0x15u;    // byte:  last valid index (RW)
constexpr uint16_t kPoSr = 0x16u;     // word:  status
constexpr uint16_t kPoCr = 0x1Bu;     // byte:  control
constexpr uint16_t kGlobCnt = 0x2Cu;  // dword: control global
constexpr uint16_t kGlobSta = 0x30u;  // dword: status global

constexpr uint8_t kCrRunPause = 0x01u;   // RPBM: 1=corriendo
constexpr uint8_t kCrReset = 0x02u;      // RR: reset de registros del stream
constexpr uint16_t kSrStatusClear = 0x1Cu;  // bits RWC de status (LVBCI/BCIS/FIFOE)
constexpr uint32_t kGlobCntColdReset = 1u << 1;
constexpr uint32_t kGlobStaPrimaryReady = 1u << 8;
// Este driver reproduce por polling de CIV y NO registra un IRQ de AC'97. Por eso
// las entradas del BDL NO llevan el bit IOC (interrupt-on-completion): con IOC, la
// reproduccion continua dispararia una interrupcion por periodo que nadie atiende
// (tormenta de IRQ -> ~15x mas lento). CIV avanza igual sin IOC.
constexpr uint16_t kBdlControlNone = 0x0000u;

constexpr uint32_t kResetSpins = 50000000u;

struct [[gnu::packed]] BdlEntry {
    uint32_t address;    // direccion fisica del buffer (alineada a 2 bytes)
    uint16_t samples;    // cantidad de samples de 16 bits en el buffer
    uint16_t control;    // bit15 IOC, bit14 BUP
};

pci::DeviceInfo g_pci = {};
uint16_t g_nam_base = 0;   // I/O base del mixer (BAR0)
uint16_t g_nabm_base = 0;  // I/O base del bus master (BAR1)

memory::PageAllocation g_dma = {};
volatile BdlEntry* g_bdl = nullptr;  // 32 entradas al inicio de la region DMA
uint8_t* g_buffers = nullptr;        // 32 buffers de kPeriodBytes
uint64_t g_buffers_phys = 0;

savanxp_audio_info g_info = {};
uint32_t g_head = 0;       // proxima ranura del BDL a llenar
uint32_t g_tail = 0;       // ranura mas vieja aun no consumida por el hardware
uint32_t g_in_flight = 0;  // periodos entregados al hardware sin consumir
uint32_t g_underruns = 0;  // veces que el ring se vacio durante la reproduccion
bool g_ready = false;
bool g_prepared = false;   // motor DMA preparado para la sesion de escritura actual
bool g_running = false;    // RPBM activo

inline void out8(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}
inline void out16(uint16_t port, uint16_t value) {
    asm volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}
inline void out32(uint16_t port, uint32_t value) {
    asm volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}
inline uint8_t in8(uint16_t port) {
    uint8_t value = 0;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
inline uint32_t in32(uint16_t port) {
    uint32_t value = 0;
    asm volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
inline void compiler_barrier() {
    asm volatile("" ::: "memory");
}

// Localiza el controlador AC'97 por clase PCI (0x04 multimedia / 0x01 audio),
// que cubre tanto QEMU (-device AC97) como VirtualBox. La subclase 0x03 seria
// HD Audio, que este driver no maneja.
bool find_controller(pci::DeviceInfo& out) {
    const size_t count = pci::device_count();
    for (size_t index = 0; index < count; ++index) {
        pci::DeviceInfo info = {};
        if (!pci::device_info(index, info)) {
            continue;
        }
        if (info.class_code == 0x04u && info.subclass == 0x01u) {
            out = info;
            return true;
        }
    }
    return false;
}

bool map_io_bars() {
    pci::BarInfo nam = {};
    pci::BarInfo nabm = {};
    if (!pci::bar_info(g_pci, 0, nam) || !pci::bar_info(g_pci, 1, nabm)) {
        return false;
    }
    if (!nam.present || !nam.io_space || !nabm.present || !nabm.io_space) {
        return false;
    }
    g_nam_base = static_cast<uint16_t>(nam.base);
    g_nabm_base = static_cast<uint16_t>(nabm.base);
    return g_nam_base != 0 && g_nabm_base != 0;
}

void enable_pci_io_and_busmaster() {
    // Command register (offset 0x04): bit0 I/O space, bit2 bus master enable.
    uint16_t command = pci::read_config_u16(g_pci.bus, g_pci.slot, g_pci.function, 0x04);
    command = static_cast<uint16_t>(command | 0x0005u);
    pci::write_config_u16(g_pci.bus, g_pci.slot, g_pci.function, 0x04, command);
}

// Saca al controlador del cold reset y espera a que el codec primario reporte
// listo. Sin esto los registros del mixer devuelven basura.
bool cold_reset_and_wait_codec() {
    const uint32_t control = in32(static_cast<uint16_t>(g_nabm_base + kGlobCnt));
    out32(static_cast<uint16_t>(g_nabm_base + kGlobCnt), control | kGlobCntColdReset);

    for (uint32_t spin = 0; spin < kResetSpins; ++spin) {
        if ((in32(static_cast<uint16_t>(g_nabm_base + kGlobSta)) & kGlobStaPrimaryReady) != 0) {
            return true;
        }
        compiler_barrier();
    }
    return false;
}

void unmute_mixer() {
    // Reset del mixer y volumenes al maximo sin mute (0x0000 = 0 dB, bit15 mute
    // en 0). Cubrimos master y PCM-out; el resto queda en su default.
    out16(static_cast<uint16_t>(g_nam_base + kNamReset), 0x0000u);
    out16(static_cast<uint16_t>(g_nam_base + kNamMasterVolume), 0x0000u);
    out16(static_cast<uint16_t>(g_nam_base + kNamPcmOutVolume), 0x0000u);
}

// Detiene el motor DMA y resetea sus registros de indice (RR solo aplica con el
// bus master parado). Deja CIV/LVI/SR en cero.
void halt_engine() {
    out8(static_cast<uint16_t>(g_nabm_base + kPoCr), 0x00u);
    for (uint32_t spin = 0; spin < kResetSpins; ++spin) {
        if ((in8(static_cast<uint16_t>(g_nabm_base + kPoCr)) & kCrRunPause) == 0) {
            break;
        }
        compiler_barrier();
    }
    out8(static_cast<uint16_t>(g_nabm_base + kPoCr), kCrReset);
    for (uint32_t spin = 0; spin < kResetSpins; ++spin) {
        if ((in8(static_cast<uint16_t>(g_nabm_base + kPoCr)) & kCrReset) == 0) {
            break;
        }
        compiler_barrier();
    }
    g_running = false;
}

// Reclama las ranuras que el hardware ya termino de reproducir. CIV apunta al
// buffer que se esta procesando ahora, asi que todo lo anterior (desde g_tail)
// esta consumido.
void reclaim_consumed() {
    const uint8_t civ = in8(static_cast<uint16_t>(g_nabm_base + kPoCiv));
    while (g_in_flight > 0 && g_tail != civ) {
        g_tail = (g_tail + 1u) & (kRingSlots - 1u);
        --g_in_flight;
    }
}

void set_bdl_descriptor(uint32_t index, uint32_t byte_count) {
    volatile BdlEntry& entry = g_bdl[index];
    entry.address = static_cast<uint32_t>(g_buffers_phys + static_cast<uint64_t>(index) * kPeriodBytes);
    entry.samples = static_cast<uint16_t>(byte_count / sizeof(int16_t));
    entry.control = kBdlControlNone;
}

// OJO con el tipo de `user_buffer`: es una direccion de userland completa, de
// 64 bits. Estuvo declarada uint32_t y el truncado no se notaba porque todo
// buffer de audio venia de la imagen ELF o de la BSS (por debajo de 4 GiB).
// Desde que el malloc de userland crece con arenas respaldadas por secciones,
// el buffer puede vivir en una vista mapeada en kSectionViewBase (64 GiB):
// truncar ahi da una direccion basura, copy_from_user falla, audio_write
// devuelve EINVAL y el cliente (Doom) apaga su audio para toda la sesion.
// OJO con el tipo de `user_buffer`: es una direccion de userland completa, de
// 64 bits. Estuvo declarada uint32_t y el truncado no se notaba porque todo
// buffer de audio venia de la imagen ELF o de la BSS (por debajo de 4 GiB).
// Desde que el malloc de userland crece con arenas respaldadas por secciones,
// el buffer puede vivir en una vista mapeada en kSectionViewBase (64 GiB):
// truncar ahi da una direccion basura, copy_from_user falla, audio_write
// devuelve EINVAL y el cliente (Doom) apaga su audio para toda la sesion.
bool copy_period(uint64_t user_buffer, uint32_t byte_count) {
    uint8_t* destination = g_buffers + static_cast<size_t>(g_head) * kPeriodBytes;
    if (!process::copy_from_user(destination, user_buffer, byte_count)) {
        return false;
    }
    set_bdl_descriptor(g_head, byte_count);
    return true;
}

// Publica `lvi` como ultima ranura valida y (re)arranca el motor: limpia el
// status RWC y asegura RPBM. Si el DMA se habia detenido por underrun, fijar
// LVI+RPBM lo reanuda.
void publish(uint8_t lvi) {
    compiler_barrier();
    out8(static_cast<uint16_t>(g_nabm_base + kPoLvi), lvi);
    out16(static_cast<uint16_t>(g_nabm_base + kPoSr), kSrStatusClear);
    const uint8_t control = in8(static_cast<uint16_t>(g_nabm_base + kPoCr));
    if ((control & kCrRunPause) == 0) {
        out8(static_cast<uint16_t>(g_nabm_base + kPoCr), static_cast<uint8_t>(control | kCrRunPause));
    }
    g_running = true;
}

// Encola kPrimePeriods de silencio para arrancar con un colchon de audio.
void prime_silence() {
    uint8_t last = 0;
    for (uint32_t i = 0; i < kPrimePeriods; ++i) {
        memset(g_buffers + static_cast<size_t>(g_head) * kPeriodBytes, 0, kPeriodBytes);
        set_bdl_descriptor(g_head, kPeriodBytes);
        last = static_cast<uint8_t>(g_head);
        g_head = (g_head + 1u) & (kRingSlots - 1u);
        ++g_in_flight;
    }
    publish(last);
}

// --- Backend audio::Backend --------------------------------------------------

bool ac97_ready() {
    return g_ready;
}

bool ac97_get_info(savanxp_audio_info& info) {
    if (!g_ready) {
        return false;
    }
    info = g_info;
    return true;
}

bool ac97_configure() {
    if (!g_ready) {
        return false;
    }
    if (g_prepared) {
        return true;
    }

    halt_engine();
    memset(const_cast<BdlEntry*>(g_bdl), 0, sizeof(BdlEntry) * kRingSlots);
    out32(static_cast<uint16_t>(g_nabm_base + kPoBdbar), static_cast<uint32_t>(g_dma.physical_address));
    g_head = 0;
    g_tail = 0;
    g_in_flight = 0;
    g_underruns = 0;
    g_running = false;
    prime_silence();
    g_prepared = true;
    return true;
}

int ac97_submit_period(uint64_t user_buffer, uint32_t byte_count) {
    if (!g_ready || !g_prepared) {
        return -static_cast<int>(SAVANXP_ENODEV);
    }
    if (byte_count == 0 || byte_count > kPeriodBytes || (byte_count % kFrameBytes) != 0) {
        return -static_cast<int>(SAVANXP_EINVAL);
    }

    reclaim_consumed();

    // No bloqueante: si el ring esta lleno, descartar este periodo en vez de
    // esperar. Bloquear aca seria un spin con interrupciones deshabilitadas (los
    // syscalls corren con IRQ off) que congelaria el timer y, con el, el reloj
    // del guest; el productor se guia por ese reloj para dosificar, asi que
    // colapsaria a casi-cero (underfeed catastrofico). Con el colchon y la
    // alimentacion en tiempo real, "lleno" casi nunca ocurre; un descarte
    // ocasional es un glitch inaudible, mejor que congelar el reloj.
    if (g_in_flight >= kMaxInFlight) {
        return 0;
    }

    // Si el ring quedo vacio (underrun por un hipo del productor), reinyectar el
    // colchon de silencio antes del dato real para no reproducir al borde y
    // volver a hacer underrun al siguiente hipo.
    if (g_in_flight == 0) {
        ++g_underruns;
        prime_silence();
    }

    if (!copy_period(user_buffer, byte_count)) {
        return -static_cast<int>(SAVANXP_EINVAL);
    }

    const uint8_t new_lvi = static_cast<uint8_t>(g_head);
    g_head = (g_head + 1u) & (kRingSlots - 1u);
    ++g_in_flight;

    publish(new_lvi);
    return 0;
}

void ac97_stop() {
    if (!g_ready) {
        return;
    }
    halt_engine();
    if (g_underruns != 0) {
        console::printf("ac97: stop con %u underruns\n", static_cast<unsigned>(g_underruns));
    }
    g_head = 0;
    g_tail = 0;
    g_in_flight = 0;
    g_prepared = false;
}

const audio::Backend g_backend = {
    .ready = ac97_ready,
    .get_info = ac97_get_info,
    .configure = ac97_configure,
    .submit_period = ac97_submit_period,
    .stop = ac97_stop,
};

} // namespace

namespace ac97 {

void initialize() {
    g_ready = false;
    g_prepared = false;
    g_running = false;
    g_head = 0;
    g_tail = 0;
    g_in_flight = 0;

    if (!pci::ready() || !find_controller(g_pci)) {
        return;
    }
    if (!map_io_bars()) {
        console::write_line("ac97: BARs de I/O ausentes o invalidos");
        return;
    }

    enable_pci_io_and_busmaster();
    if (!cold_reset_and_wait_codec()) {
        console::write_line("ac97: el codec primario no reporto listo");
        return;
    }
    unmute_mixer();

    // BDL (256 bytes) + 32 buffers de kPeriodBytes, fisicamente contiguos. El BDL
    // vive en la primera pagina; los buffers arrancan en la pagina siguiente para
    // que cada uno quede alineado a pagina.
    const uint64_t needed_bytes = memory::kPageSize + kRingBytes;
    const uint64_t page_count = (needed_bytes + memory::kPageSize - 1) / memory::kPageSize;
    if (!memory::allocate_contiguous_pages(page_count, g_dma)) {
        console::write_line("ac97: no se pudo asignar memoria DMA");
        return;
    }

    g_bdl = reinterpret_cast<volatile BdlEntry*>(g_dma.virtual_address);
    g_buffers = reinterpret_cast<uint8_t*>(g_dma.virtual_address) + memory::kPageSize;
    g_buffers_phys = g_dma.physical_address + memory::kPageSize;
    memset(const_cast<BdlEntry*>(g_bdl), 0, sizeof(BdlEntry) * kRingSlots);

    halt_engine();
    out32(static_cast<uint16_t>(g_nabm_base + kPoBdbar), static_cast<uint32_t>(g_dma.physical_address));

    g_info = {
        .sample_rate_hz = kSampleRateHz,
        .channels = kChannels,
        .bits_per_sample = kBitsPerSample,
        .frame_bytes = kFrameBytes,
        .period_bytes = kPeriodBytes,
        .buffer_bytes = kAdvertisedBufferBytes,
        .backend = SAVANXP_AUDIO_BACKEND_AC97,
        .flags = 0,
    };
    g_ready = true;

    console::printf(
        "ac97: ready pci=%x:%x.%u nam=0x%x nabm=0x%x pcm=%uHz/%uch/%ubit\n",
        static_cast<unsigned>(g_pci.bus),
        static_cast<unsigned>(g_pci.slot),
        static_cast<unsigned>(g_pci.function),
        static_cast<unsigned>(g_nam_base),
        static_cast<unsigned>(g_nabm_base),
        static_cast<unsigned>(kSampleRateHz),
        static_cast<unsigned>(kChannels),
        static_cast<unsigned>(kBitsPerSample)
    );
}

bool ready() {
    return g_ready;
}

const audio::Backend& backend() {
    return g_backend;
}

namespace {
// Prioridad media: fallback de audio cuando no hay virtio-sound (VirtualBox).
constexpr int kDriverPriority = 50;

bool driver_probe() {
    initialize();
    return ready();
}

const audio::Driver kDriver = {
    "ac97",
    kDriverPriority,
    &driver_probe,
    &backend,
};
} // namespace

const audio::Driver& driver() { return kDriver; }

} // namespace ac97
