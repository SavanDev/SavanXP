// Host tests of the pure half of the hardware inventory
// (include/kernel/inventory_rules.hpp, docs/kernel/DEVICE_INVENTORY.md):
// classification, BARs decoded without sizing, the PCI tree with its loops,
// and a sink that runs out of room. D1 of docs/roadmaps/open/DRIVERS_ROADMAP.md
// asks for the table-full, unknown-ID and missing-field cases.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "kernel/inventory_rules.hpp"

namespace {

int g_checks = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        std::fprintf(stderr, "device-inventory-test: FAIL %s\n", what);
        std::exit(1);
    }
}

void test_classify_pci() {
    check(inventory::classify_pci(0x01, 0x06, 0x01) == SXN_DEVICE_CLASS_STORAGE, "AHCI is storage");
    check(inventory::classify_pci(0x01, 0x08, 0x02) == SXN_DEVICE_CLASS_STORAGE, "NVMe is storage");
    check(inventory::classify_pci(0x02, 0x00, 0x00) == SXN_DEVICE_CLASS_NETWORK, "Ethernet is network");
    check(inventory::classify_pci(0x0d, 0x80, 0x00) == SXN_DEVICE_CLASS_NETWORK, "wireless is network");
    check(inventory::classify_pci(0x03, 0x00, 0x00) == SXN_DEVICE_CLASS_DISPLAY, "VGA is display");
    check(inventory::classify_pci(0x00, 0x01, 0x00) == SXN_DEVICE_CLASS_DISPLAY, "pre-2.0 VGA is display");
    check(inventory::classify_pci(0x00, 0x00, 0x00) == SXN_DEVICE_CLASS_UNKNOWN, "pre-2.0 other is unknown");
    check(inventory::classify_pci(0x04, 0x03, 0x00) == SXN_DEVICE_CLASS_SOUND, "HD Audio is sound");
    check(inventory::classify_pci(0x06, 0x04, 0x00) == SXN_DEVICE_CLASS_SYSTEM, "PCI bridge is system");
    check(inventory::classify_pci(0x08, 0x06, 0x00) == SXN_DEVICE_CLASS_SYSTEM, "IOMMU is system");
    check(inventory::classify_pci(0x0c, 0x03, 0x30) == SXN_DEVICE_CLASS_USB, "xHCI is USB");
    check(inventory::classify_pci(0x0c, 0x05, 0x00) == SXN_DEVICE_CLASS_SYSTEM, "SMBus is system");
    check(inventory::classify_pci(0x09, 0x00, 0x00) == SXN_DEVICE_CLASS_KEYBOARD, "keyboard controller");
    check(inventory::classify_pci(0x09, 0x02, 0x00) == SXN_DEVICE_CLASS_POINTER, "mouse controller");
    check(inventory::classify_pci(0x09, 0x80, 0x00) == SXN_DEVICE_CLASS_INPUT, "other input controller");
    check(inventory::classify_pci(0x07, 0x00, 0x02) == SXN_DEVICE_CLASS_UNKNOWN, "serial port has no group");
    check(inventory::classify_pci(0xff, 0x00, 0x00) == SXN_DEVICE_CLASS_UNKNOWN, "0xff is unknown");
    check(inventory::classify_pci(0x42, 0x42, 0x42) == SXN_DEVICE_CLASS_UNKNOWN, "an unassigned class is unknown");
}

void test_classify_usb() {
    check(inventory::classify_usb_interface(0x03, 0x01, 0x01) == SXN_DEVICE_CLASS_KEYBOARD, "boot keyboard");
    check(inventory::classify_usb_interface(0x03, 0x01, 0x02) == SXN_DEVICE_CLASS_POINTER, "boot mouse");
    check(inventory::classify_usb_interface(0x03, 0x00, 0x00) == SXN_DEVICE_CLASS_INPUT, "tablet: plain HID");
    check(inventory::classify_usb_interface(0x03, 0x01, 0x00) == SXN_DEVICE_CLASS_INPUT, "boot, no protocol");
    check(inventory::classify_usb_interface(0x08, 0x06, 0x50) == SXN_DEVICE_CLASS_DISK, "mass storage");
    check(inventory::classify_usb_interface(0x09, 0x00, 0x00) == SXN_DEVICE_CLASS_USB, "hub");
    check(inventory::classify_usb_interface(0x01, 0x01, 0x00) == SXN_DEVICE_CLASS_SOUND, "audio");
    check(inventory::classify_usb_interface(0x02, 0x06, 0x00) == SXN_DEVICE_CLASS_NETWORK, "CDC");
    check(inventory::classify_usb_interface(0xff, 0x00, 0x00) == SXN_DEVICE_CLASS_UNKNOWN, "vendor specific");
}

void test_bars() {
    sxn_device_resource out[8];

    // I/O, 32-bit memory, a prefetchable 64-bit pair, an unassigned BAR.
    const uint32_t mixed[6] = {0x0000c001u, 0xfebf0000u, 0xe000000cu, 0x00000001u, 0, 0};
    std::memset(out, 0xa5, sizeof(out));
    size_t count = inventory::decode_bars(mixed, 0x00, out, 8);
    check(count == 3, "three assigned BARs");
    check(out[0].kind == SXN_DEVICE_RESOURCE_IO_PORT && out[0].start == 0xc000u && out[0].length == 0,
        "I/O base, no length");
    check(out[1].kind == SXN_DEVICE_RESOURCE_MEMORY && out[1].start == 0xfebf0000u && out[1].flags == 0,
        "32-bit memory");
    check(out[2].kind == SXN_DEVICE_RESOURCE_MEMORY && out[2].start == 0x1e0000000ull &&
            out[2].flags == (SXN_DEVICE_RESOURCE_64BIT | SXN_DEVICE_RESOURCE_PREFETCHABLE) && out[2].length == 0,
        "64-bit prefetchable folds the upper half in");

    // A bridge has two BARs; the rest of its header is bus numbers.
    const uint32_t bridge[6] = {0xfe000000u, 0, 0x00020100u, 0x12345678u, 0x9abcdef0u, 0x0badf00du};
    count = inventory::decode_bars(bridge, 0x81, out, 8);
    check(count == 1 && out[0].start == 0xfe000000u, "bridge: only its two BARs, multifunction bit ignored");

    // CardBus and unknown layouts list nothing.
    check(inventory::decode_bars(mixed, 0x02, out, 8) == 0, "CardBus lists no BARs");
    check(inventory::decode_bars(mixed, 0x7f, out, 8) == 0, "an unknown header lists no BARs");

    // A 64-bit BAR in the last slot has no upper half to read.
    const uint32_t last[6] = {0, 0, 0, 0, 0, 0xd0000004u};
    count = inventory::decode_bars(last, 0x00, out, 8);
    check(count == 1 && out[0].start == 0xd0000000u && (out[0].flags & SXN_DEVICE_RESOURCE_64BIT) == 0,
        "a 64-bit BAR at slot 5 reads as 32-bit");

    // Capacity is respected.
    const uint32_t full[6] = {0x1001u, 0x2001u, 0x3001u, 0x4001u, 0x5001u, 0x6001u};
    std::memset(out, 0xa5, sizeof(out));
    count = inventory::decode_bars(full, 0x00, out, 2);
    check(count == 2 && out[2].kind == 0xa5a5a5a5u, "never writes past capacity");

    // Nothing assigned: a function with every BAR zero.
    const uint32_t none[6] = {};
    check(inventory::decode_bars(none, 0x00, out, 8) == 0, "no BARs at all");
}

void test_pci_tree() {
    // 0: host bridge (bus 0); 1: root port 0 -> bus 1; 2: device on bus 1;
    // 3: switch on bus 1 -> bus 2; 4: device on bus 2; 5: device on bus 7
    // with no bridge to it; 6 and 7: two bridges pointing at each other.
    const inventory::PciNode nodes[] = {
        {0, 0x00, 0},
        {0, 0x01, 1},
        {1, 0x00, 0},
        {1, 0x81, 2},
        {2, 0x00, 0},
        {7, 0x00, 0},
        {5, 0x01, 6},
        {6, 0x01, 5},
    };
    constexpr size_t count = sizeof(nodes) / sizeof(nodes[0]);

    check(inventory::pci_parent(nodes, count, 0) == -1, "bus 0 hangs from the computer");
    check(inventory::pci_parent(nodes, count, 2) == 1, "bus 1 hangs from its root port");
    check(inventory::pci_parent(nodes, count, 4) == 3, "bus 2 hangs from the switch");
    check(inventory::pci_parent(nodes, count, 5) == -1, "an orphan bus hangs from the computer");
    check(inventory::pci_parent(nodes, count, 99) == -1, "out of range");

    size_t order[count];
    long parent[count];
    const size_t ordered = inventory::order_pci(nodes, count, order, parent);
    check(ordered == count, "every function is listed");
    size_t position[count];
    bool seen[count] = {};
    for (size_t index = 0; index < ordered; ++index) {
        check(order[index] < count && !seen[order[index]], "each function once");
        seen[order[index]] = true;
        position[order[index]] = index;
    }
    for (size_t index = 0; index < count; ++index) {
        check(parent[index] < 0 || position[parent[index]] < position[index], "a parent always comes first");
    }
    check(parent[2] == 1 && parent[4] == 3 && parent[3] == 1, "the tree is kept");
    check(parent[6] == -1 && parent[7] == -1, "a loop of bridges is listed under the computer");

    // No functions at all.
    check(inventory::order_pci(nodes, 0, order, parent) == 0, "empty table");
}

struct Collected {
    sxn_device_info records[3];
    size_t taken;
    bool fail_at_one;
};

bool take(const sxn_device_info& record, size_t index, void* context) {
    Collected& collected = *static_cast<Collected*>(context);
    if (collected.fail_at_one && index == 1) {
        return false;
    }
    if (index < 3) {
        collected.records[index] = record;
        ++collected.taken;
    }
    return true;
}

void test_sink() {
    // More instances than room: the count keeps going, the copies stop.
    Collected collected = {};
    inventory::Sink sink = {&take, &collected, 42, 0, false};
    for (uint32_t id = 1; id <= 5; ++id) {
        sxn_device_info record = {};
        record.id = id;
        record.generation = 7;
        inventory::emit(sink, record);
    }
    check(sink.total == 5 && collected.taken == 3 && !sink.failed, "table full: total counts every instance");
    check(collected.records[0].generation == 42 && collected.records[2].generation == 42,
        "every record carries the snapshot's generation");
    check(collected.records[2].id == 3, "records in order");

    // A store that fails stops the copying, not the counting.
    Collected failing = {};
    failing.fail_at_one = true;
    inventory::Sink broken = {&take, &failing, 1, 0, false};
    for (int index = 0; index < 4; ++index) {
        sxn_device_info record = {};
        inventory::emit(broken, record);
    }
    check(broken.failed && broken.total == 4 && failing.taken == 1, "a failed copy is remembered");

    // Counting only.
    inventory::Sink counting = {nullptr, nullptr, 1, 0, false};
    sxn_device_info record = {};
    inventory::emit(counting, record);
    inventory::emit(counting, record);
    check(counting.total == 2 && !counting.failed, "no taker: count only");
}

void test_text() {
    char small[6];
    inventory::copy_text(small, sizeof(small), "Realtek RTL8168H");
    check(std::strcmp(small, "Realt") == 0, "cut to fit");
    inventory::copy_text(small, sizeof(small), nullptr);
    check(small[0] == 0, "missing text is empty");
    char location[SXN_DEVICE_LOCATION_CAPACITY];
    inventory::format_pci_location(location, sizeof(location), 0x00, 0x1f, 0x2);
    check(std::strcmp(location, "PCI 00:1f.2") == 0, "PCI location");
    inventory::format_pci_location(location, sizeof(location), 0xab, 0x03, 0x7);
    check(std::strcmp(location, "PCI ab:03.7") == 0, "PCI location, high bus");
}

void test_claims() {
    check(inventory::claim_allowed("", SXN_DEVICE_UNBOUND, "ahci"), "a free function is granted");
    check(inventory::claim_allowed(nullptr, SXN_DEVICE_UNBOUND, "ahci"), "no holder at all is granted");
    check(inventory::claim_allowed("azalia", SXN_DEVICE_UNBOUND, "ac97"), "a function given back is free");
    check(!inventory::claim_allowed("virtio-net", SXN_DEVICE_STARTED, "rtl8139"), "a started holder keeps it");
    check(!inventory::claim_allowed("nvme", SXN_DEVICE_FAILED, "ahci"), "a failed holder keeps it");
    check(!inventory::claim_allowed("pci", SXN_DEVICE_STARTED, "xhci"), "a bridge stays with pci");
    check(inventory::claim_allowed("virtio-input", SXN_DEVICE_FAILED, "virtio-input"),
        "a driver takes its own function again");
    check(!inventory::claim_allowed("ehci", SXN_DEVICE_STARTED, "ehci2"), "a longer name is another driver");
    check(!inventory::claim_allowed("ehci2", SXN_DEVICE_STARTED, "ehci"), "a shorter name is another driver");
    check(!inventory::claim_allowed("", SXN_DEVICE_UNBOUND, ""), "an empty claimant is refused");
    check(!inventory::claim_allowed("", SXN_DEVICE_UNBOUND, nullptr), "a missing claimant is refused");
}

void test_resource_conflicts() {
    using inventory::ResourceClaim;
    const ResourceClaim held[] = {
        {5, SXN_DEVICE_RESOURCE_IO_PORT, 0, 0x1f0, 8},
        {5, SXN_DEVICE_RESOURCE_IO_PORT, 0, 0x3f6, 1},
        {7, SXN_DEVICE_RESOURCE_MEMORY, 0, 0xfebf0000u, 0x1000},
        {7, SXN_DEVICE_RESOURCE_IRQ, SXN_DEVICE_RESOURCE_SHARED, 11, 1},
        {9, SXN_DEVICE_RESOURCE_IRQ, 0, 1, 1},
    };
    const size_t count = sizeof(held) / sizeof(held[0]);
    check(inventory::resource_conflict(held, count, {6, SXN_DEVICE_RESOURCE_IO_PORT, 0, 0x1f7, 1}) == 0,
        "the last port of a range collides");
    check(inventory::resource_conflict(held, count, {6, SXN_DEVICE_RESOURCE_IO_PORT, 0, 0x1e8, 8}) == -1,
        "the range just below is free");
    check(inventory::resource_conflict(held, count, {6, SXN_DEVICE_RESOURCE_IO_PORT, 0, 0x1f8, 8}) == -1,
        "the range just above is free");
    check(inventory::resource_conflict(held, count, {6, SXN_DEVICE_RESOURCE_IO_PORT, 0, 0x3f0, 0x10}) == 1,
        "a range around a single port collides");
    check(inventory::resource_conflict(held, count, {5, SXN_DEVICE_RESOURCE_IO_PORT, 0, 0x1f0, 8}) == -1,
        "an owner never collides with itself");
    check(inventory::resource_conflict(held, count, {6, SXN_DEVICE_RESOURCE_MEMORY, 0, 0x1f0, 8}) == -1,
        "memory and ports are different spaces");
    check(inventory::resource_conflict(held, count, {6, SXN_DEVICE_RESOURCE_MEMORY, 0, 0xfebf0800u, 0x1000}) == 2,
        "overlapping memory collides");
    check(inventory::resource_conflict(held, count,
              {6, SXN_DEVICE_RESOURCE_IRQ, SXN_DEVICE_RESOURCE_SHARED, 11, 1}) == -1,
        "two shared lines coexist");
    check(inventory::resource_conflict(held, count, {6, SXN_DEVICE_RESOURCE_IRQ, 0, 11, 1}) == 3,
        "an exclusive claim of a shared line collides");
    check(inventory::resource_conflict(held, count,
              {6, SXN_DEVICE_RESOURCE_IRQ, SXN_DEVICE_RESOURCE_SHARED, 1, 1}) == 4,
        "sharing an exclusive line collides");
    check(inventory::resource_conflict(held, count, {6, SXN_DEVICE_RESOURCE_MEMORY, 0, 0x1000, 0}) == -2,
        "no length is refused");
    check(inventory::resource_conflict(held, count,
              {6, SXN_DEVICE_RESOURCE_MEMORY, 0, ~static_cast<uint64_t>(0) - 1, 4}) == -2,
        "a range that wraps is refused");
    check(inventory::resource_conflict(nullptr, 0, {6, SXN_DEVICE_RESOURCE_MEMORY, 0, 0x1000, 1}) == -1,
        "nothing held, nothing collides");
}

void test_apply_claims() {
    using inventory::ResourceClaim;
    sxn_device_resource resources[4] = {};
    resources[0] = {SXN_DEVICE_RESOURCE_MEMORY, SXN_DEVICE_RESOURCE_64BIT, 0xfe000000u, 0};
    resources[1] = {SXN_DEVICE_RESOURCE_IO_PORT, 0, 0xc000, 0};
    resources[2] = {SXN_DEVICE_RESOURCE_IRQ, 0, 11, 1};
    const ResourceClaim claims[] = {
        {3, SXN_DEVICE_RESOURCE_MEMORY, 0, 0xfe000000u, 0x4000},
        {4, SXN_DEVICE_RESOURCE_IO_PORT, 0, 0xc000, 0x100},
        {3, SXN_DEVICE_RESOURCE_IRQ, SXN_DEVICE_RESOURCE_SHARED, 16, 1},
        {3, SXN_DEVICE_RESOURCE_IO_PORT, 0, 0x60, 1},
        {3, SXN_DEVICE_RESOURCE_IO_PORT, 0, 0x64, 1},
    };
    const size_t count = inventory::apply_claims(resources, 3, 4, claims, 5, 3);
    check(count == 4, "the firmware line goes, a claimed line and one port come, the last does not fit");
    check(resources[0].length == 0x4000 &&
            resources[0].flags == (SXN_DEVICE_RESOURCE_64BIT | SXN_DEVICE_RESOURCE_CLAIMED),
        "a claimed BAR gets its length and keeps its flags");
    check(resources[1].length == 0 && resources[1].flags == 0, "another owner's claim changes nothing");
    check(resources[2].kind == SXN_DEVICE_RESOURCE_IRQ && resources[2].start == 16 &&
            resources[2].flags == (SXN_DEVICE_RESOURCE_CLAIMED | SXN_DEVICE_RESOURCE_SHARED),
        "the claimed line replaces the firmware's, shared");
    check(resources[3].kind == SXN_DEVICE_RESOURCE_IO_PORT && resources[3].start == 0x60 &&
            resources[3].length == 1,
        "a claim with no listed match is appended");

    sxn_device_resource lines[2] = {};
    lines[0] = {SXN_DEVICE_RESOURCE_IRQ, 0, 10, 1};
    check(inventory::apply_claims(lines, 1, 2, claims, 5, 4) == 2 && lines[0].kind == SXN_DEVICE_RESOURCE_IRQ &&
            lines[0].start == 10 && lines[0].flags == 0 && lines[1].start == 0xc000,
        "an owner without a claimed line keeps the firmware's");
}

} // namespace

int main() {
    static_assert(SXN_DEVICE_INFO_V1_SIZE == sizeof(sxn_device_info), "V1 is the whole record today");
    test_classify_pci();
    test_classify_usb();
    test_bars();
    test_pci_tree();
    test_sink();
    test_text();
    test_claims();
    test_resource_conflicts();
    test_apply_claims();
    std::printf("device-inventory-test: %d checks passed\n", g_checks);
    return 0;
}
