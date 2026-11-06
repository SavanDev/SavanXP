// USB mass storage: Bulk-Only Transport carrying a SCSI subset, exposed as
// block devices named usb0, usb1... Written from the USB Mass Storage Class
// Bulk-Only Transport 1.0 specification and SBC, with OpenBSD's umass.c and
// umass_scsi.c as reference. See docs/roadmaps/done/USB_ROADMAP.md.

#include <stddef.h>
#include <stdint.h>

#include "kernel/block.hpp"
#include "kernel/console.hpp"
#include "kernel/partition.hpp"
#include "kernel/storage.hpp"
#include "kernel/string.hpp"
#include "kernel/timer.hpp"
#include "kernel/usb.hpp"
#include "usb_internal.hpp"

namespace {

using usb::Device;

constexpr uint8_t kSubclassScsi = 0x06;
constexpr uint8_t kProtocolBulkOnly = 0x50;

constexpr uint8_t kRequestMassStorageReset = 0xff;

constexpr uint32_t kCbwSignature = 0x43425355u;
constexpr uint32_t kCswSignature = 0x53425355u;
constexpr uint8_t kCswPassed = 0;
constexpr uint8_t kCswFailed = 1;

constexpr uint8_t kScsiTestUnitReady = 0x00;
constexpr uint8_t kScsiRequestSense = 0x03;
constexpr uint8_t kScsiInquiry = 0x12;
constexpr uint8_t kScsiReadCapacity10 = 0x25;
constexpr uint8_t kScsiRead10 = 0x28;
constexpr uint8_t kScsiWrite10 = 0x2a;
constexpr uint8_t kScsiSynchronizeCache10 = 0x35;
constexpr uint8_t kScsiServiceActionIn16 = 0x9e;
constexpr uint8_t kScsiReadCapacity16 = 0x10;

constexpr size_t kMaxDisks = 4;
constexpr uint32_t kSectorsPerTransfer = usb::hc::kMaxBulkBytes / block::kSectorSize;
constexpr uint32_t kCommandTimeoutMs = 10000;
// How often a pulled stick whose volume is still in use is tried again.
constexpr uint64_t kForgetRetryMs = 1000;

// Below the ramdisk (10): a stick never becomes the root ahead of the
// system disk or the LiveCD's volume, since fs::mount_any takes the first
// device it recognizes in this order.
constexpr int kDriverPriority = 5;

struct [[gnu::packed]] CommandBlockWrapper {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_length;
    uint8_t flags;
    uint8_t lun;
    uint8_t length;
    uint8_t command[16];
};
static_assert(sizeof(CommandBlockWrapper) == 31);

struct [[gnu::packed]] CommandStatusWrapper {
    uint32_t signature;
    uint32_t tag;
    uint32_t residue;
    uint8_t status;
};
static_assert(sizeof(CommandStatusWrapper) == 13);

struct Disk {
    bool active;
    Device* device;
    uint32_t generation;
    uint8_t interface;
    uint8_t in;
    uint8_t out;
    uint32_t tag;
    uint32_t sector_count;
    size_t block_index;
    bool registered;
    // The stick was pulled out while its block device was registered: the
    // slot stays (block:: holds a pointer to it) until storage:: lets go.
    bool gone;
    uint64_t retry_at;
    char name[8];
    block::DeviceIdentity identity;
};

Disk g_disks[kMaxDisks] = {};
uint32_t g_next_number = 0;

bool alive(const Disk& disk) {
    return disk.active && !disk.gone && disk.device != nullptr && disk.device->present &&
        disk.device->generation == disk.generation;
}

uint32_t big_endian32(const uint8_t* bytes) {
    return (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16) |
        (static_cast<uint32_t>(bytes[2]) << 8) | bytes[3];
}

void put_big_endian32(uint8_t* bytes, uint32_t value) {
    bytes[0] = static_cast<uint8_t>(value >> 24);
    bytes[1] = static_cast<uint8_t>(value >> 16);
    bytes[2] = static_cast<uint8_t>(value >> 8);
    bytes[3] = static_cast<uint8_t>(value);
}

// After a stall: the controller's side of the endpoint was reset by the
// transfer; the device's halt is cleared here.
void clear_stall(Disk& disk, uint8_t endpoint) {
    (void)usb::clear_halt(*disk.device, endpoint);
}

// Reset Recovery (BOT 5.3.4): the device lost track of the protocol.
void reset_recovery(Disk& disk) {
    usb::SetupPacket setup = {};
    setup.request_type = usb::kRequestClass | usb::kRequestToInterface;
    setup.request = kRequestMassStorageReset;
    setup.index = disk.interface;
    (void)usb::hc::control(*disk.device, setup, nullptr);
    (void)usb::hc::reset_endpoint(*disk.device, disk.in);
    clear_stall(disk, disk.in);
    (void)usb::hc::reset_endpoint(*disk.device, disk.out);
    clear_stall(disk, disk.out);
}

// One SCSI command: CBW, data, CSW. Returns the CSW status, or -1 when the
// transport itself failed.
int transport(Disk& disk, const uint8_t* command, uint8_t command_length, void* data, uint32_t length, bool in) {
    if (!alive(disk)) {
        return -1;
    }
    CommandBlockWrapper cbw = {};
    cbw.signature = kCbwSignature;
    cbw.tag = ++disk.tag;
    cbw.data_length = length;
    cbw.flags = in ? 0x80 : 0x00;
    cbw.lun = 0;
    cbw.length = command_length;
    memcpy(cbw.command, command, command_length);
    int status = usb::hc::bulk(*disk.device, disk.out, &cbw, sizeof(cbw), nullptr, kCommandTimeoutMs);
    if (status != usb::status_ok) {
        reset_recovery(disk);
        return -1;
    }
    if (length != 0) {
        uint32_t actual = 0;
        status = usb::hc::bulk(*disk.device, in ? disk.in : disk.out, data, length, &actual, kCommandTimeoutMs);
        if (status == usb::status_stall) {
            // The device ends the data phase early; the CSW still follows.
            clear_stall(disk, in ? disk.in : disk.out);
        } else if (status != usb::status_ok) {
            reset_recovery(disk);
            return -1;
        }
    }
    CommandStatusWrapper csw = {};
    uint32_t actual = 0;
    status = usb::hc::bulk(*disk.device, disk.in, &csw, sizeof(csw), &actual, kCommandTimeoutMs);
    if (status == usb::status_stall) {
        clear_stall(disk, disk.in);
        status = usb::hc::bulk(*disk.device, disk.in, &csw, sizeof(csw), &actual, kCommandTimeoutMs);
    }
    if (status != usb::status_ok || actual != sizeof(csw) || csw.signature != kCswSignature || csw.tag != cbw.tag ||
        csw.status > kCswFailed) {
        reset_recovery(disk);
        return -1;
    }
    return csw.status;
}

// A command that failed with CHECK CONDITION leaves sense data the device
// wants read before the next one.
uint8_t request_sense(Disk& disk) {
    uint8_t command[6] = {kScsiRequestSense, 0, 0, 0, 18, 0};
    uint8_t sense[18] = {};
    if (transport(disk, command, sizeof(command), sense, sizeof(sense), true) != kCswPassed) {
        return 0;
    }
    return sense[2] & 0x0f;
}

bool command_ok(Disk& disk, const uint8_t* command, uint8_t length, void* data, uint32_t bytes, bool in) {
    const int status = transport(disk, command, length, data, bytes, in);
    if (status == kCswFailed) {
        (void)request_sense(disk);
    }
    return status == kCswPassed;
}

bool rw(Disk& disk, uint32_t lba, uint32_t count, void* buffer, bool write) {
    auto* bytes = static_cast<uint8_t*>(buffer);
    while (count != 0) {
        const uint32_t chunk = count < kSectorsPerTransfer ? count : kSectorsPerTransfer;
        uint8_t command[10] = {write ? kScsiWrite10 : kScsiRead10};
        put_big_endian32(command + 2, lba);
        command[7] = static_cast<uint8_t>(chunk >> 8);
        command[8] = static_cast<uint8_t>(chunk);
        if (!command_ok(disk, command, sizeof(command), bytes, chunk * block::kSectorSize, !write)) {
            return false;
        }
        lba += chunk;
        count -= chunk;
        bytes += static_cast<size_t>(chunk) * block::kSectorSize;
    }
    return true;
}

bool read_op(void* context, uint32_t lba, uint32_t count, void* buffer) {
    return rw(*static_cast<Disk*>(context), lba, count, buffer, false);
}

bool write_op(void* context, uint32_t lba, uint32_t count, const void* buffer) {
    return rw(*static_cast<Disk*>(context), lba, count, const_cast<void*>(buffer), true);
}

bool flush_op(void* context) {
    Disk& disk = *static_cast<Disk*>(context);
    // A pulled stick has nothing left to commit, and refusing would keep its
    // volume from ever being released.
    if (!alive(disk)) {
        return true;
    }
    uint8_t command[10] = {kScsiSynchronizeCache10};
    // Many sticks have no cache to commit and reject the command; that is
    // not a failed flush.
    (void)command_ok(disk, command, sizeof(command), nullptr, 0, false);
    return true;
}

// Written through: a stick can be pulled out, and writes held back in memory
// would be lost with it (and would keep its block device from going away).
const block::DeviceOps kOps = {
    &read_op,
    &write_op,
    &flush_op,
    /*deferred_writes=*/false,
};

void copy_trimmed(char* out, size_t capacity, const uint8_t* text, size_t length) {
    size_t written = 0;
    for (size_t i = 0; i < length && written + 1 < capacity; ++i) {
        out[written++] = text[i] >= 0x20 && text[i] < 0x7f ? static_cast<char>(text[i]) : ' ';
    }
    while (written > 0 && out[written - 1] == ' ') {
        --written;
    }
    out[written] = 0;
}

// Sticks often answer the first commands with UNIT ATTENTION or NOT READY
// while they wake up.
bool wait_ready(Disk& disk) {
    for (int attempt = 0; attempt < 10; ++attempt) {
        uint8_t command[6] = {kScsiTestUnitReady};
        if (command_ok(disk, command, sizeof(command), nullptr, 0, false)) {
            return true;
        }
        usb::delay_ms(100);
    }
    return false;
}

bool read_capacity(Disk& disk, uint64_t& sectors, uint32_t& block_size) {
    uint8_t command[10] = {kScsiReadCapacity10};
    uint8_t answer[32] = {};
    if (!command_ok(disk, command, sizeof(command), answer, 8, true)) {
        return false;
    }
    const uint32_t last = big_endian32(answer);
    block_size = big_endian32(answer + 4);
    if (last != 0xffffffffu) {
        sectors = static_cast<uint64_t>(last) + 1;
        return true;
    }
    // Larger than 2 TiB of sectors: READ CAPACITY (16).
    uint8_t long_command[16] = {kScsiServiceActionIn16, kScsiReadCapacity16};
    long_command[13] = 32;
    if (!command_ok(disk, long_command, sizeof(long_command), answer, 32, true)) {
        return false;
    }
    sectors = ((static_cast<uint64_t>(big_endian32(answer)) << 32) | big_endian32(answer + 4)) + 1;
    block_size = big_endian32(answer + 8);
    return true;
}

void register_disk(Disk& disk) {
    disk.block_index = block::register_device(kOps, &disk, disk.sector_count, /*writable=*/true, disk.name,
        &disk.identity);
    disk.registered = disk.block_index != block::kInvalidDevice;
}

void forget(Disk& disk) {
    if (!storage::forget_disk(disk.block_index)) {
        disk.retry_at = timer::monotonic_ms() + kForgetRetryMs;
        return;
    }
    console::printf("usb-storage: %s removed\n", disk.name);
    disk = {};
}

bool probe(Device& device, const usb::InterfaceDescriptor& interface, const uint8_t* extra, size_t extra_length) {
    if (interface.interface_class != usb::kClassMassStorage || interface.interface_subclass != kSubclassScsi ||
        interface.interface_protocol != kProtocolBulkOnly) {
        return false;
    }
    Disk* slot = nullptr;
    for (Disk& candidate : g_disks) {
        if (!candidate.active) {
            slot = &candidate;
            break;
        }
    }
    if (slot == nullptr) {
        return false;
    }
    Disk& disk = *slot;
    disk = {};
    disk.device = &device;
    disk.generation = device.generation;
    disk.interface = interface.interface_number;

    // The two bulk endpoints, and on USB 3 their burst size from the
    // companion descriptor that follows each.
    usb::EndpointDescriptor in = {};
    usb::EndpointDescriptor out = {};
    uint8_t in_burst = 0;
    uint8_t out_burst = 0;
    usb::EndpointDescriptor* last = nullptr;
    size_t offset = 0;
    while (offset + 2 <= extra_length && extra[offset] >= 2) {
        const uint8_t type = extra[offset + 1];
        if (type == usb::kDescriptorEndpoint && extra[offset] >= sizeof(usb::EndpointDescriptor)) {
            usb::EndpointDescriptor endpoint = {};
            memcpy(&endpoint, extra + offset, sizeof(endpoint));
            last = nullptr;
            if ((endpoint.attributes & 0x03) == usb::kTransferBulk) {
                last = (endpoint.address & 0x80) != 0 ? &in : &out;
                *last = endpoint;
            }
        } else if (type == usb::kDescriptorSsEndpointCompanion && extra[offset] >= 3 && last != nullptr) {
            (last == &in ? in_burst : out_burst) = extra[offset + 2];
        }
        offset += extra[offset];
    }
    if (in.address == 0 || out.address == 0 || !usb::hc::add_endpoint(device, in, in_burst) ||
        !usb::hc::add_endpoint(device, out, out_burst)) {
        console::write_line("usb-storage: no bulk endpoints");
        return false;
    }
    disk.in = in.address;
    disk.out = out.address;
    disk.active = true;

    uint8_t inquiry_command[6] = {kScsiInquiry, 0, 0, 0, 36, 0};
    uint8_t inquiry[36] = {};
    if (!command_ok(disk, inquiry_command, sizeof(inquiry_command), inquiry, sizeof(inquiry), true)) {
        console::write_line("usb-storage: INQUIRY failed");
        disk = {};
        return false;
    }
    // Peripheral device type 0: direct access. CD-ROMs (5) and the rest wait.
    if ((inquiry[0] & 0x1f) != 0) {
        console::printf("usb-storage: device type %u not supported\n", static_cast<unsigned>(inquiry[0] & 0x1f));
        disk = {};
        return false;
    }
    char vendor[9] = {};
    char product[17] = {};
    copy_trimmed(vendor, sizeof(vendor), inquiry + 8, 8);
    copy_trimmed(product, sizeof(product), inquiry + 16, 16);
    size_t length = 0;
    const char* const parts[] = {vendor, product};
    for (const char* part : parts) {
        if (part[0] == 0) {
            continue;
        }
        if (length != 0 && length + 1 < sizeof(disk.identity.model)) {
            disk.identity.model[length++] = ' ';
        }
        for (size_t i = 0; part[i] != 0 && length + 1 < sizeof(disk.identity.model); ++i) {
            disk.identity.model[length++] = part[i];
        }
    }
    disk.identity.model[length] = 0;
    usb::read_string(device, device.descriptor.serial_index, disk.identity.serial, sizeof(disk.identity.serial));

    uint64_t sectors = 0;
    uint32_t block_size = 0;
    if (!wait_ready(disk) || !read_capacity(disk, sectors, block_size)) {
        console::printf("usb-storage: \"%s\" has no medium or does not answer\n", disk.identity.model);
        disk = {};
        return false;
    }
    if (block_size != block::kSectorSize) {
        console::printf("usb-storage: \"%s\" has %u-byte sectors; only 512 is supported\n", disk.identity.model,
            static_cast<unsigned>(block_size));
        disk = {};
        return false;
    }
    disk.identity.full_sectors = sectors;
    disk.sector_count = sectors > 0xffffffffu ? 0xffffffffu : static_cast<uint32_t>(sectors);
    const uint32_t number = g_next_number++;
    disk.name[0] = 'u';
    disk.name[1] = 's';
    disk.name[2] = 'b';
    if (number < 10) {
        disk.name[3] = static_cast<char>('0' + number);
        disk.name[4] = 0;
    } else {
        disk.name[3] = static_cast<char>('0' + (number / 10) % 10);
        disk.name[4] = static_cast<char>('0' + number % 10);
        disk.name[5] = 0;
    }
    usb::set_driver_data(device, usb::storage_class_driver(), &disk);
    console::printf("usb-storage: %s \"%s\" serial \"%s\", %u sectors\n", disk.name, disk.identity.model,
        disk.identity.serial, static_cast<unsigned>(disk.sector_count));
    // Found after boot: register now. At boot block::probe_all() has not run
    // yet and would wipe the table; enumerate() registers it then.
    if (block::ready()) {
        register_disk(disk);
        if (disk.registered) {
            (void)partition::rescan(disk.block_index);
        }
    }
    return true;
}

void detach(Device& device) {
    for (Disk& disk : g_disks) {
        if (!disk.active || disk.gone || disk.device != &device) {
            continue;
        }
        if (!disk.registered) {
            disk = {};
            continue;
        }
        disk.gone = true;
        forget(disk);
        if (disk.active) {
            console::printf("usb-storage: %s pulled out while in use; its volume fails until released\n",
                disk.name);
        }
    }
}

// Pulled sticks whose volumes were still in use: try again.
bool service() {
    bool worked = false;
    const uint64_t now = timer::monotonic_ms();
    for (Disk& disk : g_disks) {
        if (disk.active && disk.gone && now >= disk.retry_at) {
            forget(disk);
            worked = true;
        }
    }
    return worked;
}

const usb::ClassDriver kStorageDriver = {
    "usb-storage",
    &probe,
    &detach,
    nullptr,
    &service,
};

void enumerate() {
    for (Disk& disk : g_disks) {
        if (disk.active) {
            register_disk(disk);
        }
    }
}

const block::Driver kBlockDriver = {
    "usb-storage",
    kDriverPriority,
    &enumerate,
    nullptr,
};

} // namespace

namespace usb {

const ClassDriver& storage_class_driver() {
    return kStorageDriver;
}

void register_storage_driver() {
    (void)register_class_driver(kStorageDriver);
}

const block::Driver& storage_driver() {
    return kBlockDriver;
}

} // namespace usb
