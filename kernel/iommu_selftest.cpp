#include "kernel/iommu.hpp"

#include "kernel/amd_vi.hpp"
#include "kernel/console.hpp"
#include "kernel/pci.hpp"
#include "kernel/physical_memory.hpp"
#include "kernel/string.hpp"
#include "kernel/timer.hpp"
#include "kernel/vmm.hpp"

#include "arch/cpu.hpp"

/* The M2 and M3 self-test (docs/kernel/IOMMU.md#testing). It drives QEMU's `edu`
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

// M3: edu's own domain maps two pages here. It is not memory on any board
// the smokes boot (QEMU's PCI hole), so the identity domain does not map it:
// a DMA there arrives only through the device's own domain.
constexpr uint64_t kDeviceWindow = 0xc0000000;

volatile uint8_t* g_edu = nullptr;

// What the owner of edu's domain was told.
struct OwnerLog {
    unsigned faults;
    uint16_t device;
};

OwnerLog g_owner_log = {};

void on_fault(void* context, uint16_t requester, uint64_t, bool) {
    auto* log = static_cast<OwnerLog*>(context);
    ++log->faults;
    log->device = requester;
}

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

/* A 4-byte DMA that must fault: logged, counted, and handed to the domain's
 * owner when `owner_hears`, or to nobody. Which access faulted is not
 * checked: QEMU 11 leaves the event's RW flag clear, as it leaves the
 * address zero. */
const char* expect_fault(uint64_t source, uint64_t destination, bool to_memory, uint16_t requester, bool owner_hears) {
    const uint64_t before = amd_vi::fault_count();
    const unsigned heard = g_owner_log.faults;
    if (!dma(source, destination, to_memory, 4)) {
        return "the faulting DMA never completed";
    }
    if (!wait_for([&] { return amd_vi::fault_count() != before; })) {
        return "a DMA that must fault was not reported";
    }
    if (!owner_hears) {
        return g_owner_log.faults == heard ? nullptr : "a fault reached an owner that no longer owns the device";
    }
    if (g_owner_log.faults == heard) {
        return "the fault did not reach the domain's owner";
    }
    if (g_owner_log.device != requester) {
        return "the owner was told about another device";
    }
    return nullptr;
}

/* M3: edu in a domain of its own, at device addresses that are not the
 * physical ones. */
const char* own_domain(const memory::PageAllocation& from, const memory::PageAllocation& to, uint16_t requester,
    const pci::DeviceInfo& edu) {
    const uint64_t tables_before = amd_vi::table_pages();
    iommu::Domain domain;
    if (!create_domain(&on_fault, &g_owner_log, domain)) {
        return "no domain could be created";
    }
    // `to` is where the device writes, `from` where it reads. `from` is also
    // mapped a second time, read-only, at a device address edu never reads:
    // QEMU keeps a translation it used with its permissions and refuses a
    // write through it without an event, so the permission fault is checked
    // on a page it has not cached.
    const uint64_t write_page = kDeviceWindow;
    const uint64_t read_page = kDeviceWindow + memory::kPageSize;
    const uint64_t read_only_page = kDeviceWindow + 2 * memory::kPageSize;
    if (!map(domain, write_page, to.physical_address, memory::kPageSize, kDeviceWrites) ||
        !map(domain, read_page, from.physical_address, memory::kPageSize, kDeviceReads) ||
        !map(domain, read_only_page, from.physical_address, memory::kPageSize, kDeviceReads)) {
        return "the domain's pages could not be mapped";
    }
    if (map(domain, read_page, to.physical_address, memory::kPageSize, kDeviceReads)) {
        return "a device address was mapped twice";
    }
    if (!attach(domain, edu) || amd_vi::domain_of(requester) != domain.index) {
        return "edu could not be attached to its domain";
    }

    auto* source = static_cast<uint8_t*>(from.virtual_address);
    auto* destination = static_cast<uint8_t*>(to.virtual_address);
    for (size_t i = 0; i < kEduBufferSize; ++i) {
        source[i] = static_cast<uint8_t>(0x33 + i * 5u);
    }
    memset(destination, 0, kEduBufferSize);
    if (!dma(read_page, kEduBuffer, false) || !dma(kEduBuffer, write_page, true) ||
        memcmp(source, destination, kEduBufferSize) != 0) {
        return "a DMA through edu's own domain did not arrive";
    }
    console::printf("iommu: selftest: edu in its own domain reached the pages mapped for it\n");

    // The identity domain is gone for edu: its physical pages are not.
    memset(destination, 0, kEduBufferSize);
    if (const char* failure = expect_fault(kEduBuffer, to.physical_address, true, requester, true)) {
        return failure;
    }
    // A page mapped for reading only.
    if (const char* failure = expect_fault(kEduBuffer, read_only_page, true, requester, true)) {
        return failure;
    }
    for (size_t i = 0; i < kEduBufferSize; ++i) {
        if (source[i] != static_cast<uint8_t>(0x33 + i * 5u)) {
            return "a write to a read-only page reached memory";
        }
    }
    // A page unmapped is gone once unmap returns.
    if (!unmap(domain, write_page, memory::kPageSize)) {
        return "unmap did not complete";
    }
    if (const char* failure = expect_fault(kEduBuffer, write_page, true, requester, true)) {
        return failure;
    }
    for (size_t i = 0; i < kEduBufferSize; ++i) {
        if (destination[i] != 0) {
            return "a DMA that faulted reached memory";
        }
    }
    console::printf("iommu: selftest: edu's physical pages, a read-only page and an unmapped one faulted, "
        "and each fault reached the domain's owner\n");

    // Destroying the domain leaves edu blocked: nothing reachable, and no
    // owner to tell.
    destroy_domain(domain);
    if (domain.valid() || amd_vi::domain_of(requester) != amd_vi::kBlockedDomain) {
        return "destroying the domain did not block edu";
    }
    if (const char* failure = expect_fault(read_page, kEduBuffer, false, requester, false)) {
        return failure;
    }
    if (const char* failure = expect_fault(kEduBuffer, to.physical_address, true, requester, false)) {
        return failure;
    }
    if (amd_vi::table_pages() != tables_before) {
        return "destroying the domain did not free its page tables";
    }
    console::printf("iommu: selftest: destroying the domain left edu in the blocked domain\n");
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

    if (const char* failure = own_domain(from, to, requester, edu)) {
        return failure;
    }
    // edu is blocked now and can reach neither page.
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
