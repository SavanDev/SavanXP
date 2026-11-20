#include "kernel/iommu.hpp"

#include "kernel/amd_vi.hpp"
#include "kernel/console.hpp"
#include "kernel/pci.hpp"
#include "kernel/physical_memory.hpp"
#include "kernel/string.hpp"
#include "kernel/timer.hpp"
#include "kernel/vmm.hpp"

#include "arch/cpu.hpp"

/* The M2 self-test (docs/kernel/IOMMU.md#testing). It drives QEMU's `edu`
 * teaching device, whose only use here is a DMA engine the kernel can point
 * anywhere: a buffer of 4 KiB inside the device, copied to or from any bus
 * address. Not a driver, and nothing else binds the device. */
namespace iommu {

namespace {

constexpr uint16_t kEduVendor = 0x1234;
constexpr uint16_t kEduDevice = 0x11e8;
constexpr uint32_t kEduIdentification = 0x00;
constexpr uint32_t kEduDmaSource = 0x80;
constexpr uint32_t kEduDmaDestination = 0x88;
constexpr uint32_t kEduDmaCount = 0x90;
constexpr uint32_t kEduDmaCommand = 0x98;
constexpr uint64_t kEduDmaRun = 1u << 0;
constexpr uint64_t kEduDmaToMemory = 1u << 1;
// Where the device's own buffer sits in its DMA address space.
constexpr uint64_t kEduBuffer = 0x40000;
constexpr uint64_t kEduBufferSize = 4096;

constexpr uint16_t kPciCommand = 0x04;
constexpr uint16_t kPciCommandMemory = 1u << 1;
constexpr uint16_t kPciCommandBusMaster = 1u << 2;

// Far above the RAM of any machine the smokes boot, below 2^48, and not in
// any reserved range QEMU describes: the identity domain must not map it.
constexpr uint64_t kUnmappedAddress = 64ull << 30;
constexpr uint64_t kDmaWaitMs = 2000;
// The tick drains every 100 ms; the test drains itself, but a fault may need
// a moment to reach the log.
constexpr uint64_t kFaultWaitMs = 1000;

volatile uint8_t* g_edu = nullptr;

uint64_t read64(uint32_t offset) {
    return *reinterpret_cast<volatile uint64_t*>(g_edu + offset);
}

// edu takes its DMA registers as 8-byte writes only; a 4-byte one would set
// the whole register to the 32-bit value.
void write64(uint32_t offset, uint64_t value) {
    *reinterpret_cast<volatile uint64_t*>(g_edu + offset) = value;
}

bool dma(uint64_t source, uint64_t destination, bool to_memory, uint64_t count = kEduBufferSize) {
    write64(kEduDmaSource, source);
    write64(kEduDmaDestination, destination);
    write64(kEduDmaCount, count);
    write64(kEduDmaCommand, kEduDmaRun | (to_memory ? kEduDmaToMemory : 0));
    const uint64_t until = timer::monotonic_ms() + kDmaWaitMs;
    while ((read64(kEduDmaCommand) & kEduDmaRun) != 0) {
        if (timer::monotonic_ms() > until) {
            return false;
        }
        arch::cpu_relax();
    }
    return true;
}

/* Drains until `ready` holds or the wait runs out. The tick drains as well,
 * so the counters may already have moved by the time this looks. */
template <typename Ready>
bool wait_for(Ready ready) {
    const uint64_t until = timer::monotonic_ms() + kFaultWaitMs;
    for (;;) {
        amd_vi::drain_events();
        if (ready()) {
            return true;
        }
        if (timer::monotonic_ms() > until) {
            return false;
        }
        arch::cpu_relax();
    }
}

/* One 4-byte DMA outside the domain: one I/O page fault, from edu. QEMU 11
 * leaves the address field of its fault events zero (its own trace shows the
 * right address), so the address is checked only when the event carries
 * one. */
const char* single_fault(uint16_t requester) {
    const uint64_t before = amd_vi::fault_count();
    if (!dma(kEduBuffer, kUnmappedAddress, true, 4)) {
        return "the faulting DMA never completed";
    }
    if (!wait_for([&] { return amd_vi::fault_count() != before; })) {
        return "a DMA outside the identity domain was not reported";
    }
    uint16_t device = 0;
    uint64_t address = 0;
    amd_vi::last_fault(device, address);
    if (device != requester) {
        return "the fault reported names another device";
    }
    if (address != 0 && (address & ~(memory::kPageSize - 1)) != kUnmappedAddress) {
        return "the fault reported names another address";
    }
    return nullptr;
}

bool round_trip(const memory::PageAllocation& from, const memory::PageAllocation& to, uint8_t seed) {
    auto* source = static_cast<uint8_t*>(from.virtual_address);
    auto* destination = static_cast<uint8_t*>(to.virtual_address);
    for (size_t i = 0; i < kEduBufferSize; ++i) {
        source[i] = static_cast<uint8_t>(seed + i * 7u);
    }
    memset(destination, 0, kEduBufferSize);
    if (!dma(from.physical_address, kEduBuffer, false) || !dma(kEduBuffer, to.physical_address, true)) {
        return false;
    }
    return memcmp(source, destination, kEduBufferSize) == 0;
}

const char* run() {
    if (!amd_vi::running()) {
        return "translation is off (boot with iommu)";
    }
    pci::DeviceInfo edu = {};
    if (!pci::find_device(kEduVendor, kEduDevice, edu)) {
        return "no edu device (QEMU -device edu)";
    }
    pci::BarInfo bar = {};
    void* mapping = nullptr;
    if (!pci::bar_info(edu, 0, bar) || !bar.present || bar.io_space ||
        !vm::map_kernel_mmio(bar.base, memory::kPageSize, vm::kPagePresent | vm::kPageWrite, &mapping)) {
        return "edu's registers cannot be mapped";
    }
    g_edu = static_cast<volatile uint8_t*>(mapping);
    if ((*reinterpret_cast<volatile uint32_t*>(g_edu + kEduIdentification) & 0xffu) != 0xed) {
        return "edu does not identify itself";
    }
    const uint16_t command = pci::read_config_u16(edu.bus, edu.slot, edu.function, kPciCommand);
    pci::write_config_u16(edu.bus, edu.slot, edu.function, kPciCommand,
        static_cast<uint16_t>(command | kPciCommandMemory | kPciCommandBusMaster));
    const uint16_t requester = static_cast<uint16_t>((edu.bus << 8) | (edu.slot << 3) | edu.function);

    memory::PageAllocation from = {};
    memory::PageAllocation to = {};
    if (!memory::allocate_page(from) || !memory::allocate_page(to)) {
        return "no memory";
    }
    if (!amd_vi::identity_maps(from.physical_address) || !amd_vi::identity_maps(to.physical_address)) {
        return "the identity domain does not map freshly allocated memory";
    }
    if (amd_vi::identity_maps(kUnmappedAddress)) {
        return "the identity domain maps the address meant to fault";
    }

    if (!round_trip(from, to, 0x11)) {
        return "a DMA through the identity domain did not arrive";
    }
    console::printf("iommu: selftest: DMA through the identity domain arrived\n");

    if (const char* failure = single_fault(requester)) {
        return failure;
    }
    console::printf("iommu: selftest: DMA outside the identity domain faulted and was reported\n");

    // A 4 KiB DMA outside the domain: QEMU faults each 4 bytes of it, more
    // events than the log holds. The log must keep working after it: the
    // next fault is reported as before. Whether the overflow itself is
    // reported is the unit's business; QEMU sets the overflow bit only when
    // the event log interrupt is on, and drops the rest silently otherwise.
    const uint64_t before_flood = amd_vi::fault_count();
    if (!dma(kEduBuffer, kUnmappedAddress, true)) {
        return "the faulting DMA never completed";
    }
    if (!wait_for([&] { return amd_vi::fault_count() > before_flood + 1; })) {
        return "a flood of faults was not reported";
    }
    if (const char* failure = single_fault(requester)) {
        return failure;
    }
    console::printf("iommu: selftest: a flood of faults filled the event log, and the next fault was still reported\n");

    // The unit and the device keep working after the fault.
    if (!round_trip(from, to, 0x5a)) {
        return "DMA stopped working after the fault";
    }
    memory::free_allocation(from);
    memory::free_allocation(to);
    return nullptr;
}

} // namespace

void selftest() {
    const char* failure = run();
    if (failure != nullptr) {
        console::printf("iommu: selftest failed: %s\n", failure);
        return;
    }
    console::printf("iommu: selftest passed\n");
}

} // namespace iommu
