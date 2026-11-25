// The hardware inventory: ids, driver bindings and the snapshot. See
// docs/kernel/DEVICE_INVENTORY.md.

#include "kernel/inventory.hpp"

#include "kernel/console.hpp"
#include "kernel/pci.hpp"
#include "kernel/string.hpp"
#include "kernel/usb.hpp"

namespace {

struct PciBinding {
    uint32_t id;
    uint32_t state;
    uint32_t reason;
    uint32_t flags;
    uint32_t device_class; // SXN_DEVICE_CLASS_UNKNOWN: from the class codes
    char driver[SXN_DEVICE_DRIVER_CAPACITY];
    char name[SXN_DEVICE_NAME_CAPACITY];
};

struct Platform {
    uint32_t id;
    uint32_t device_class;
    uint32_t state;
    uint32_t reason;
    uint32_t flags;
    char name[SXN_DEVICE_NAME_CAPACITY];
    char driver[SXN_DEVICE_DRIVER_CAPACITY];
    char location[SXN_DEVICE_LOCATION_CAPACITY];
    sxn_device_resource resources[4];
    uint32_t resource_count;
};

constexpr size_t kMaxPlatform = 8;

bool g_ready = false;
uint32_t g_next_id = 1;
uint32_t g_root_id = 0;
uint32_t g_generation = 1;
size_t g_pci_count = 0;
PciBinding g_pci[inventory::kMaxPciNodes] = {};
Platform g_platform[kMaxPlatform] = {};
size_t g_platform_count = 0;

// What drivers claimed inside their instances (docs/kernel/DEVICE_INVENTORY.md,
// "Resources"): a few per started driver.
constexpr size_t kMaxClaims = 256;
inventory::ResourceClaim g_claims[kMaxClaims] = {};
size_t g_claim_count = 0;

// The scratch of a snapshot; the kernel lock keeps it to one at a time.
inventory::PciNode g_nodes[inventory::kMaxPciNodes] = {};
size_t g_order[inventory::kMaxPciNodes] = {};
long g_parent[inventory::kMaxPciNodes] = {};

void drop_claims(uint32_t owner) {
    size_t kept = 0;
    for (size_t index = 0; index < g_claim_count; ++index) {
        if (g_claims[index].owner != owner) {
            g_claims[kept++] = g_claims[index];
        }
    }
    g_claim_count = kept;
}

// The driver of whichever instance has this id, for a log line.
const char* driver_of(uint32_t id);

const char* kind_name(uint32_t kind) {
    switch (kind) {
        case SXN_DEVICE_RESOURCE_MEMORY: return "memory";
        case SXN_DEVICE_RESOURCE_IO_PORT: return "I/O";
        case SXN_DEVICE_RESOURCE_IRQ: return "IRQ";
        default: return "resource";
    }
}

bool record_claim(uint32_t owner, const char* driver, uint32_t kind, uint64_t start, uint64_t length, uint32_t flags) {
    const inventory::ResourceClaim candidate = {owner, kind, flags & SXN_DEVICE_RESOURCE_SHARED, start, length};
    const long conflict = inventory::resource_conflict(g_claims, g_claim_count, candidate);
    if (conflict != -1) {
        console::printf("inventory: %s cannot claim %s 0x%llx+0x%llx%s%s\n", driver, kind_name(kind),
            start, length, conflict >= 0 ? ", held by " : "",
            conflict >= 0 ? driver_of(g_claims[conflict].owner) : "");
        return false;
    }
    for (size_t index = 0; index < g_claim_count; ++index) {
        const inventory::ResourceClaim& held = g_claims[index];
        if (held.owner == owner && held.kind == kind && held.start == start) {
            // Claimed again (a driver retrying its own function): the latest
            // length stands.
            g_claims[index] = candidate;
            inventory::changed();
            return true;
        }
    }
    if (g_claim_count >= kMaxClaims) {
        console::printf("inventory: %s cannot claim %s 0x%llx, the table is full\n", driver, kind_name(kind),
            start);
        return false;
    }
    g_claims[g_claim_count++] = candidate;
    inventory::changed();
    return true;
}

PciBinding* binding_of(const pci::DeviceInfo& device) {
    if (!g_ready) {
        return nullptr;
    }
    for (size_t index = 0; index < g_pci_count; ++index) {
        pci::DeviceInfo info = {};
        if (pci::device_info(index, info) && info.bus == device.bus && info.slot == device.slot &&
            info.function == device.function) {
            return &g_pci[index];
        }
    }
    return nullptr;
}

void set_state(PciBinding& binding, uint32_t state, uint32_t reason) {
    binding.state = state;
    binding.reason = reason;
    inventory::changed();
}

void describe_pci(inventory::Sink& sink) {
    for (size_t index = 0; index < g_pci_count; ++index) {
        pci::DeviceInfo info = {};
        (void)pci::device_info(index, info);
        g_nodes[index] = {info.bus, info.header_type, info.secondary_bus};
    }
    const size_t ordered = inventory::order_pci(g_nodes, g_pci_count, g_order, g_parent);
    for (size_t position = 0; position < ordered; ++position) {
        const size_t index = g_order[position];
        pci::DeviceInfo info = {};
        if (!pci::device_info(index, info)) {
            continue;
        }
        const PciBinding& binding = g_pci[index];
        sxn_device_info record = {};
        record.id = binding.id;
        record.parent_id = g_parent[index] >= 0 ? g_pci[g_parent[index]].id : g_root_id;
        record.bus = SXN_DEVICE_BUS_PCI;
        record.device_class = binding.device_class != SXN_DEVICE_CLASS_UNKNOWN
            ? binding.device_class
            : inventory::classify_pci(info.class_code, info.subclass, info.prog_if);
        record.state = binding.state;
        record.reason = binding.reason;
        record.flags = binding.flags;
        record.vendor_id = info.vendor_id;
        record.product_id = info.device_id;
        // Offset 0x2c holds the subsystem only in a type 0 header.
        if ((info.header_type & 0x7fu) == 0x00) {
            record.subsystem_vendor_id = info.subsystem_vendor_id;
            record.subsystem_id = info.subsystem_device_id;
        }
        record.class_code = info.class_code;
        record.subclass = info.subclass;
        record.protocol = info.prog_if;
        record.revision = info.revision;
        record.bus_number = info.bus;
        record.device_number = info.slot;
        record.function_number = info.function;
        inventory::copy_text(record.name, sizeof(record.name), binding.name);
        inventory::copy_text(record.driver, sizeof(record.driver), binding.driver);
        inventory::format_pci_location(record.location, sizeof(record.location), info.bus, info.slot, info.function);
        record.resource_count = static_cast<uint32_t>(
            inventory::decode_bars(info.bar, info.header_type, record.resources, SXN_DEVICE_MAX_RESOURCES));
        // The line the firmware wrote, when the function has a pin to use it.
        if (info.interrupt_pin != 0 && info.irq_line != 0xff && record.resource_count < SXN_DEVICE_MAX_RESOURCES) {
            sxn_device_resource& irq = record.resources[record.resource_count++];
            irq.kind = SXN_DEVICE_RESOURCE_IRQ;
            irq.start = info.irq_line;
            irq.length = 1;
        }
        record.resource_count = static_cast<uint32_t>(inventory::apply_claims(record.resources,
            record.resource_count, SXN_DEVICE_MAX_RESOURCES, g_claims, g_claim_count, record.id));
        inventory::emit(sink, record);
    }
}

void describe_platform(inventory::Sink& sink) {
    for (size_t index = 0; index < g_platform_count; ++index) {
        const Platform& device = g_platform[index];
        sxn_device_info record = {};
        record.id = device.id;
        record.parent_id = g_root_id;
        record.bus = SXN_DEVICE_BUS_PLATFORM;
        record.device_class = device.device_class;
        record.state = device.state;
        record.reason = device.reason;
        record.flags = device.flags;
        inventory::copy_text(record.name, sizeof(record.name), device.name);
        inventory::copy_text(record.driver, sizeof(record.driver), device.driver);
        inventory::copy_text(record.location, sizeof(record.location), device.location);
        record.resource_count = device.resource_count;
        for (uint32_t resource = 0; resource < device.resource_count; ++resource) {
            record.resources[resource] = device.resources[resource];
        }
        record.resource_count = static_cast<uint32_t>(inventory::apply_claims(record.resources,
            record.resource_count, SXN_DEVICE_MAX_RESOURCES, g_claims, g_claim_count, record.id));
        inventory::emit(sink, record);
    }
}

const char* driver_of(uint32_t id) {
    for (size_t index = 0; index < g_pci_count; ++index) {
        if (g_pci[index].id == id) {
            return g_pci[index].driver;
        }
    }
    for (size_t index = 0; index < g_platform_count; ++index) {
        if (g_platform[index].id == id) {
            return g_platform[index].driver;
        }
    }
    return "?";
}

} // namespace

namespace inventory {

void initialize() {
    g_root_id = allocate_id();
    g_pci_count = pci::device_count();
    if (g_pci_count > kMaxPciNodes) {
        g_pci_count = kMaxPciNodes;
    }
    for (size_t index = 0; index < g_pci_count; ++index) {
        g_pci[index] = {};
        g_pci[index].id = allocate_id();
        g_pci[index].state = SXN_DEVICE_UNBOUND;
        g_pci[index].reason = SXN_DEVICE_REASON_NOT_CLAIMED;
        // The host bridge carries every configuration cycle, and a PCI-to-PCI
        // bridge every one for the buses behind it: pci:: uses both, as the
        // firmware set them up, to scan and reach every function.
        pci::DeviceInfo info = {};
        if (pci::device_info(index, info) && info.class_code == 0x06 &&
            (info.subclass == 0x00 || info.subclass == 0x04)) {
            copy_text(g_pci[index].driver, sizeof(g_pci[index].driver), "pci");
            g_pci[index].state = SXN_DEVICE_STARTED;
            g_pci[index].reason = SXN_DEVICE_REASON_NONE;
            g_pci[index].flags = SXN_DEVICE_FIRMWARE;
        }
    }
    g_ready = true;
}

uint32_t allocate_id() {
    return g_next_id++;
}

uint32_t root_id() {
    return g_root_id;
}

void changed() {
    ++g_generation;
}

bool pci_claim(const pci::DeviceInfo& device, const char* driver, uint32_t device_class) {
    PciBinding* binding = binding_of(device);
    if (binding == nullptr) {
        // Before initialize() or off the scan: nothing to record, nothing
        // anyone else can hold.
        return driver != nullptr && driver[0] != 0;
    }
    if (!claim_allowed(binding->driver, binding->state, driver)) {
        console::printf("inventory: %s refused at %x:%x.%u, held by %s\n", driver != nullptr ? driver : "?",
            static_cast<unsigned>(device.bus), static_cast<unsigned>(device.slot),
            static_cast<unsigned>(device.function), binding->driver);
        return false;
    }
    copy_text(binding->driver, sizeof(binding->driver), driver);
    binding->device_class = device_class;
    set_state(*binding, SXN_DEVICE_FAILED, SXN_DEVICE_REASON_START_FAILED);
    return true;
}

bool pci_release(const pci::DeviceInfo& device, const char* driver) {
    PciBinding* binding = binding_of(device);
    if (binding == nullptr || driver == nullptr || binding->driver[0] == 0 ||
        strcmp(binding->driver, driver) != 0) {
        return false;
    }
    binding->driver[0] = 0;
    binding->name[0] = 0;
    binding->device_class = SXN_DEVICE_CLASS_UNKNOWN;
    drop_claims(binding->id);
    set_state(*binding, SXN_DEVICE_UNBOUND, SXN_DEVICE_REASON_NOT_CLAIMED);
    return true;
}

bool pci_release_failed(const pci::DeviceInfo& device, const char* driver) {
    PciBinding* binding = binding_of(device);
    if (binding == nullptr || driver == nullptr || binding->driver[0] == 0 ||
        strcmp(binding->driver, driver) != 0) {
        return false;
    }
    drop_claims(binding->id);
    set_state(*binding, SXN_DEVICE_UNBOUND, SXN_DEVICE_REASON_START_FAILED);
    return true;
}

size_t pci_claim_count(const pci::DeviceInfo& device) {
    const PciBinding* binding = binding_of(device);
    if (binding == nullptr) {
        return 0;
    }
    size_t count = 0;
    for (size_t index = 0; index < g_claim_count; ++index) {
        if (g_claims[index].owner == binding->id) {
            ++count;
        }
    }
    return count;
}

bool pci_claim_resource(const pci::DeviceInfo& device, uint32_t kind, uint64_t start, uint64_t length,
    uint32_t flags) {
    PciBinding* binding = binding_of(device);
    if (binding == nullptr) {
        // Off the scan: nothing to record it against.
        return true;
    }
    if (binding->driver[0] == 0 || binding->state == SXN_DEVICE_UNBOUND) {
        console::printf("inventory: a resource of %x:%x.%u claimed with no driver holding it\n",
            static_cast<unsigned>(device.bus), static_cast<unsigned>(device.slot),
            static_cast<unsigned>(device.function));
        return false;
    }
    return record_claim(binding->id, binding->driver, kind, start, length, flags);
}

bool pci_claim_bar(const pci::DeviceInfo& device, const pci::BarInfo& bar) {
    if (!bar.present || bar.base == 0 || bar.size == 0) {
        return true;
    }
    return pci_claim_resource(device, bar.io_space ? SXN_DEVICE_RESOURCE_IO_PORT : SXN_DEVICE_RESOURCE_MEMORY,
        bar.base, bar.size);
}

bool claim_resource(uint32_t id, uint32_t kind, uint64_t start, uint64_t length, uint32_t flags) {
    return record_claim(id, driver_of(id), kind, start, length, flags);
}

bool pci_available(const pci::DeviceInfo& device, const char* driver) {
    const PciBinding* binding = binding_of(device);
    if (binding == nullptr) {
        return driver != nullptr && driver[0] != 0;
    }
    return claim_allowed(binding->driver, binding->state, driver);
}

bool pci_holder(const pci::DeviceInfo& device, const char*& driver, uint32_t& state, uint32_t& reason) {
    const PciBinding* binding = binding_of(device);
    if (binding == nullptr) {
        return false;
    }
    driver = binding->driver;
    state = binding->state;
    reason = binding->reason;
    return true;
}

void pci_started(const pci::DeviceInfo& device) {
    if (PciBinding* binding = binding_of(device)) {
        set_state(*binding, SXN_DEVICE_STARTED, SXN_DEVICE_REASON_NONE);
    }
}

void pci_failed(const pci::DeviceInfo& device) {
    if (PciBinding* binding = binding_of(device)) {
        set_state(*binding, SXN_DEVICE_FAILED, SXN_DEVICE_REASON_START_FAILED);
    }
}

void pci_unsupported(const pci::DeviceInfo& device) {
    if (PciBinding* binding = binding_of(device)) {
        drop_claims(binding->id);
        set_state(*binding, SXN_DEVICE_UNBOUND, SXN_DEVICE_REASON_UNSUPPORTED);
    }
}

void pci_name(const pci::DeviceInfo& device, const char* name) {
    if (PciBinding* binding = binding_of(device)) {
        copy_text(binding->name, sizeof(binding->name), name);
        changed();
    }
}

void pci_boot_option(uint8_t class_code, uint8_t subclass) {
    for (size_t index = 0; index < g_pci_count; ++index) {
        pci::DeviceInfo info = {};
        if (pci::device_info(index, info) && info.class_code == class_code && info.subclass == subclass &&
            g_pci[index].driver[0] == 0) {
            g_pci[index].reason = SXN_DEVICE_REASON_BOOT_OPTION;
        }
    }
    changed();
}

uint32_t pci_id(uint8_t bus, uint8_t slot, uint8_t function) {
    pci::DeviceInfo device = {};
    device.bus = bus;
    device.slot = slot;
    device.function = function;
    const PciBinding* binding = binding_of(device);
    return binding != nullptr ? binding->id : 0;
}

uint32_t platform_add(const PlatformDevice& device) {
    if (g_platform_count >= kMaxPlatform) {
        return 0;
    }
    Platform& entry = g_platform[g_platform_count++];
    entry = {};
    entry.id = allocate_id();
    entry.device_class = device.device_class;
    entry.state = device.state;
    entry.reason = device.reason;
    entry.flags = device.flags;
    copy_text(entry.name, sizeof(entry.name), device.name);
    copy_text(entry.driver, sizeof(entry.driver), device.driver);
    copy_text(entry.location, sizeof(entry.location), device.location);
    entry.resource_count = device.resource_count < 4u ? device.resource_count : 4u;
    for (uint32_t index = 0; index < entry.resource_count; ++index) {
        entry.resources[index] = device.resources[index];
    }
    changed();
    return entry.id;
}

size_t snapshot(Sink& sink) {
    sink.generation = g_generation;
    sink.total = 0;
    sink.failed = false;
    if (!g_ready) {
        return 0;
    }
    sxn_device_info root = {};
    root.id = g_root_id;
    root.bus = SXN_DEVICE_BUS_ROOT;
    root.device_class = SXN_DEVICE_CLASS_COMPUTER;
    root.state = SXN_DEVICE_STARTED;
    emit(sink, root);
    describe_pci(sink);
    describe_platform(sink);
    usb::describe(sink);
    return sink.total;
}

} // namespace inventory

namespace {

bool selftest_fail(const char* step) {
    console::printf("inventory: selftest failed: %s\n", step);
    return false;
}

bool selftest_record(const pci::DeviceInfo& device, const char* driver, uint32_t state, uint32_t reason) {
    const char* holder = nullptr;
    uint32_t current_state = 0;
    uint32_t current_reason = 0;
    return inventory::pci_holder(device, holder, current_state, current_reason) && strcmp(holder, driver) == 0 &&
        current_state == state && current_reason == reason;
}

// A range no machine decodes: far above any physical address.
constexpr uint64_t kSelftestRange = 0x7fff000000000000ull;

struct Capture {
    uint32_t id;
    bool found;
    sxn_device_info record;
};

bool capture_take(const sxn_device_info& record, size_t, void* context) {
    Capture& capture = *static_cast<Capture*>(context);
    if (record.id == capture.id) {
        capture.record = record;
        capture.found = true;
    }
    return true;
}

// The listed length of the self-test's range on `device`, or 0 when it is
// not listed claimed.
uint64_t selftest_listed(const pci::DeviceInfo& device) {
    Capture capture = {};
    capture.id = inventory::pci_id(device.bus, device.slot, device.function);
    inventory::Sink sink = {&capture_take, &capture, 0, 0, false};
    (void)inventory::snapshot(sink);
    if (!capture.found) {
        return 0;
    }
    for (uint32_t index = 0; index < capture.record.resource_count && index < SXN_DEVICE_MAX_RESOURCES; ++index) {
        const sxn_device_resource& resource = capture.record.resources[index];
        if (resource.kind == SXN_DEVICE_RESOURCE_MEMORY && resource.start == kSelftestRange &&
            (resource.flags & SXN_DEVICE_RESOURCE_CLAIMED) != 0) {
            return resource.length;
        }
    }
    return 0;
}

bool run_selftest() {
    constexpr char kFirst[] = "selftest-a";
    constexpr char kSecond[] = "selftest-b";
    pci::DeviceInfo held = {};
    pci::DeviceInfo idle = {};
    bool have_held = false;
    bool have_idle = false;
    for (size_t index = 0; index < g_pci_count; ++index) {
        pci::DeviceInfo info = {};
        if (!pci::device_info(index, info)) {
            continue;
        }
        const PciBinding& binding = g_pci[index];
        if (!have_held && binding.state == SXN_DEVICE_STARTED && binding.driver[0] != 0 &&
            (binding.flags & SXN_DEVICE_FIRMWARE) == 0) {
            held = info;
            have_held = true;
        }
        if (!have_idle && binding.state == SXN_DEVICE_UNBOUND && binding.driver[0] == 0) {
            idle = info;
            have_idle = true;
        }
    }
    if (!have_held || !have_idle) {
        return selftest_fail("no started function and no free one to test with");
    }

    const char* holder = nullptr;
    uint32_t state = 0;
    uint32_t reason = 0;
    (void)inventory::pci_holder(held, holder, state, reason);
    char held_driver[SXN_DEVICE_DRIVER_CAPACITY];
    inventory::copy_text(held_driver, sizeof(held_driver), holder);
    const uint32_t generation = g_generation;
    if (inventory::pci_claim(held, kFirst)) {
        return selftest_fail("a started function was taken by another driver");
    }
    if (inventory::pci_release(held, kFirst)) {
        return selftest_fail("a driver released a function it does not hold");
    }
    if (!selftest_record(held, held_driver, SXN_DEVICE_STARTED, SXN_DEVICE_REASON_NONE) ||
        g_generation != generation) {
        return selftest_fail("a refused claim changed the record");
    }

    if (inventory::pci_claim_resource(idle, SXN_DEVICE_RESOURCE_MEMORY, kSelftestRange, 0x1000)) {
        return selftest_fail("a range was claimed on a function nobody holds");
    }
    if (!inventory::pci_claim(idle, kFirst) ||
        !selftest_record(idle, kFirst, SXN_DEVICE_FAILED, SXN_DEVICE_REASON_START_FAILED)) {
        return selftest_fail("a free function was not taken");
    }
    if (!inventory::pci_claim_resource(idle, SXN_DEVICE_RESOURCE_MEMORY, kSelftestRange, 0x1000) ||
        selftest_listed(idle) != 0x1000) {
        return selftest_fail("the holder's range was not listed claimed with its length");
    }
    if (inventory::pci_claim_resource(held, SXN_DEVICE_RESOURCE_MEMORY, kSelftestRange + 0x800, 0x1000)) {
        return selftest_fail("another instance claimed an overlapping range");
    }
    if (inventory::pci_claim(idle, kSecond) || !selftest_record(idle, kFirst, SXN_DEVICE_FAILED,
            SXN_DEVICE_REASON_START_FAILED)) {
        return selftest_fail("a second driver took a function in its start");
    }
    if (!inventory::pci_claim(idle, kFirst)) {
        return selftest_fail("a driver could not take its own function again");
    }
    if (!inventory::pci_release(idle, kFirst) ||
        !selftest_record(idle, "", SXN_DEVICE_UNBOUND, SXN_DEVICE_REASON_NOT_CLAIMED)) {
        return selftest_fail("the holder could not give the function back");
    }
    if (selftest_listed(idle) != 0) {
        return selftest_fail("a released function still lists its range");
    }
    if (!inventory::pci_claim(idle, kSecond) ||
        !inventory::pci_claim_resource(idle, SXN_DEVICE_RESOURCE_MEMORY, kSelftestRange, 0x1000) ||
        !inventory::pci_release(idle, kSecond)) {
        return selftest_fail("a function given back, or its range, was not free");
    }
    console::printf("inventory: refused %x:%x.%u, held by %s; took and gave back %x:%x.%u\n",
        static_cast<unsigned>(held.bus), static_cast<unsigned>(held.slot), static_cast<unsigned>(held.function),
        held_driver, static_cast<unsigned>(idle.bus), static_cast<unsigned>(idle.slot),
        static_cast<unsigned>(idle.function));
    return true;
}

} // namespace

namespace inventory {

void selftest() {
    if (run_selftest()) {
        console::write_line("inventory: selftest passed");
    }
}

} // namespace inventory
