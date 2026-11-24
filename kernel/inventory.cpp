// The hardware inventory: ids, driver bindings and the snapshot. See
// docs/kernel/DEVICE_INVENTORY.md.

#include "kernel/inventory.hpp"

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

// The scratch of a snapshot; the kernel lock keeps it to one at a time.
inventory::PciNode g_nodes[inventory::kMaxPciNodes] = {};
size_t g_order[inventory::kMaxPciNodes] = {};
long g_parent[inventory::kMaxPciNodes] = {};

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
        inventory::emit(sink, record);
    }
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

void pci_claim(const pci::DeviceInfo& device, const char* driver, uint32_t device_class) {
    PciBinding* binding = binding_of(device);
    if (binding == nullptr) {
        return;
    }
    copy_text(binding->driver, sizeof(binding->driver), driver);
    binding->device_class = device_class;
    set_state(*binding, SXN_DEVICE_FAILED, SXN_DEVICE_REASON_START_FAILED);
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
