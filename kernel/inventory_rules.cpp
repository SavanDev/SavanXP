// The pure half of the hardware inventory: classification, BAR decoding,
// the PCI tree and the record sink. See docs/kernel/DEVICE_INVENTORY.md.

#include "kernel/inventory_rules.hpp"

namespace inventory {

uint32_t classify_pci(uint8_t class_code, uint8_t subclass, uint8_t prog_if) {
    (void)prog_if;
    switch (class_code) {
        case 0x00:
            // Built before class codes existed; subclass 1 is a VGA.
            return subclass == 0x01 ? SXN_DEVICE_CLASS_DISPLAY : SXN_DEVICE_CLASS_UNKNOWN;
        case 0x01:
            return SXN_DEVICE_CLASS_STORAGE;
        case 0x02:
        case 0x0d: // wireless
            return SXN_DEVICE_CLASS_NETWORK;
        case 0x03:
            return SXN_DEVICE_CLASS_DISPLAY;
        case 0x04:
            return SXN_DEVICE_CLASS_SOUND;
        case 0x05: // memory controller
        case 0x06: // bridge
        case 0x08: // base system peripheral: PIC, timer, RTC, IOMMU
        case 0x0a: // docking station
        case 0x0b: // processor
        case 0x0e: // intelligent I/O
        case 0x10: // encryption
        case 0x11: // signal processing
        case 0x12: // processing accelerator
            return SXN_DEVICE_CLASS_SYSTEM;
        case 0x09:
            if (subclass == 0x00) {
                return SXN_DEVICE_CLASS_KEYBOARD;
            }
            if (subclass == 0x02) {
                return SXN_DEVICE_CLASS_POINTER;
            }
            return SXN_DEVICE_CLASS_INPUT;
        case 0x0c:
            return subclass == 0x03 ? SXN_DEVICE_CLASS_USB : SXN_DEVICE_CLASS_SYSTEM;
        default:
            return SXN_DEVICE_CLASS_UNKNOWN;
    }
}

uint32_t classify_usb_interface(uint8_t class_code, uint8_t subclass, uint8_t protocol) {
    switch (class_code) {
        case 0x01:
            return SXN_DEVICE_CLASS_SOUND;
        case 0x02: // communication
        case 0x0a: // its data interface
            return SXN_DEVICE_CLASS_NETWORK;
        case 0x03:
            // The boot protocols are the only ones the descriptor names; any
            // other HID says what it is in its report descriptor.
            if (subclass == 0x01 && protocol == 0x01) {
                return SXN_DEVICE_CLASS_KEYBOARD;
            }
            if (subclass == 0x01 && protocol == 0x02) {
                return SXN_DEVICE_CLASS_POINTER;
            }
            return SXN_DEVICE_CLASS_INPUT;
        case 0x08:
            return SXN_DEVICE_CLASS_DISK;
        case 0x09:
            return SXN_DEVICE_CLASS_USB;
        default:
            return SXN_DEVICE_CLASS_UNKNOWN;
    }
}

size_t decode_bars(const uint32_t bar[6], uint8_t header_type, sxn_device_resource* out, size_t capacity) {
    size_t bar_count = 0;
    switch (header_type & 0x7fu) {
        case 0x00:
            bar_count = 6;
            break;
        case 0x01:
            bar_count = 2;
            break;
        default:
            return 0;
    }
    size_t written = 0;
    for (size_t index = 0; index < bar_count && written < capacity; ++index) {
        const uint32_t raw = bar[index];
        sxn_device_resource resource = {};
        if ((raw & 0x1u) != 0) {
            resource.kind = SXN_DEVICE_RESOURCE_IO_PORT;
            resource.start = raw & ~0x3u;
        } else {
            resource.kind = SXN_DEVICE_RESOURCE_MEMORY;
            resource.start = raw & ~0xfu;
            if ((raw & 0x8u) != 0) {
                resource.flags |= SXN_DEVICE_RESOURCE_PREFETCHABLE;
            }
            if (((raw >> 1) & 0x3u) == 0x2u && index + 1 < bar_count) {
                resource.flags |= SXN_DEVICE_RESOURCE_64BIT;
                resource.start |= static_cast<uint64_t>(bar[index + 1]) << 32;
                ++index;
            }
        }
        if (resource.start == 0) {
            continue;
        }
        out[written++] = resource;
    }
    return written;
}

long pci_parent(const PciNode* nodes, size_t count, size_t index) {
    if (index >= count || nodes[index].bus == 0) {
        return -1;
    }
    for (size_t candidate = 0; candidate < count; ++candidate) {
        const uint8_t type = nodes[candidate].header_type & 0x7fu;
        if (candidate != index && (type == 0x01 || type == 0x02) &&
            nodes[candidate].secondary_bus == nodes[index].bus) {
            return static_cast<long>(candidate);
        }
    }
    return -1;
}

size_t order_pci(const PciNode* nodes, size_t count, size_t* order, long* parent) {
    if (count > kMaxPciNodes) {
        count = kMaxPciNodes;
    }
    bool listed[kMaxPciNodes] = {};
    size_t written = 0;
    // A pass lists every function whose parent is listed already; each pass
    // goes one bridge deeper. A pass that lists nothing leaves only loops.
    bool progress = true;
    while (written < count && progress) {
        progress = false;
        for (size_t index = 0; index < count; ++index) {
            if (listed[index]) {
                continue;
            }
            const long up = pci_parent(nodes, count, index);
            if (up >= 0 && !listed[up]) {
                continue;
            }
            listed[index] = true;
            order[written] = index;
            parent[index] = up;
            ++written;
            progress = true;
        }
    }
    for (size_t index = 0; index < count && written < count; ++index) {
        if (!listed[index]) {
            listed[index] = true;
            order[written++] = index;
            parent[index] = -1;
        }
    }
    return written;
}

void copy_text(char* out, size_t capacity, const char* text) {
    if (capacity == 0) {
        return;
    }
    size_t length = 0;
    if (text != nullptr) {
        while (text[length] != 0 && length + 1 < capacity) {
            out[length] = text[length];
            ++length;
        }
    }
    out[length] = 0;
}

void format_pci_location(char* out, size_t capacity, uint8_t bus, uint8_t device, uint8_t function) {
    static const char kDigits[] = "0123456789abcdef";
    char text[] = "PCI 00:00.0";
    text[4] = kDigits[bus >> 4];
    text[5] = kDigits[bus & 0xfu];
    text[7] = kDigits[(device >> 4) & 0xfu];
    text[8] = kDigits[device & 0xfu];
    text[10] = kDigits[function & 0xfu];
    copy_text(out, capacity, text);
}

void emit(Sink& sink, sxn_device_info& record) {
    record.generation = sink.generation;
    if (!sink.failed && sink.take != nullptr && !sink.take(record, sink.total, sink.context)) {
        sink.failed = true;
    }
    ++sink.total;
}

} // namespace inventory
