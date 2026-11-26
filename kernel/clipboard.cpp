#include "kernel/clipboard.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/device.hpp"
#include "kernel/heap.hpp"
#include "kernel/object.hpp"
#include "kernel/process.hpp"
#include "kernel/string.hpp"
#include "abi/savanxp_abi.h"

/*
 * The system clipboard (`/dev/clipboard`, docs/desktop/CLIPBOARD.md).
 *
 * Why it lives in the kernel and not in the window manager, where a system
 * with more room would put it: windowd pays for every channel with descriptors
 * PER CLIENT (two today: the event pipe and the wake event) against the limit
 * of 64 per process in process::kMaxFileHandles. A channel or two more per
 * client for copy and paste would lower the ceiling on simultaneous windows,
 * which took some effort to raise (docs/desktop/WM_SUBSYSTEM.md). As a device
 * node a client opens it to copy or paste and closes it at once, it costs no
 * permanent fd, and it is there for processes without a window too.
 *
 * The content is a VALUE, not a stream: a set of named formats replaced as a
 * whole, read from the start with no cursor. There is no owner and no
 * negotiation in the X11 style -- whoever copies leaves every format it offers
 * and forgets about it --, which is the desktop model and the one that fits a
 * system where the program that copied may have ended before anyone pastes.
 *
 * `/dev/dragdata` is a second store with the same model and calls: what a
 * drag carries between programs (docs/desktop/DRAG_AND_DROP.md). The source
 * leaves its formats there before the drag starts, the target reads them
 * when it drops, and a drag never touches what the clipboard holds.
 */

namespace {

int negative_error(sx_status code) {
    return -static_cast<int>(code);
}

struct Entry {
    char name[SAVANXP_CLIPBOARD_NAME_CAPACITY];
    uint32_t offset; // into Store::data
    uint32_t length;
};

// One set of formats: every format's bytes in one heap block, in the order
// of `entries`; `data` is null while the set is empty or every entry is.
struct Store {
    uint8_t* data;
    Entry entries[SAVANXP_CLIPBOARD_MAX_FORMATS];
    uint32_t count;
    uint64_t sequence;
};

Store g_clipboard = {};
Store g_drag = {};

size_t name_length(const char* name) {
    size_t length = 0;
    while (length < SAVANXP_CLIPBOARD_NAME_CAPACITY && name[length] != '\0') {
        ++length;
    }
    return length;
}

bool same_name(const char* left, const char* right) {
    for (size_t index = 0; index < SAVANXP_CLIPBOARD_NAME_CAPACITY; ++index) {
        if (left[index] != right[index]) {
            return false;
        }
        if (left[index] == '\0') {
            return true;
        }
    }
    return true;
}

// A name is NUL-terminated inside its field, not empty, printable ASCII and
// does not start with a space: what every subsystem can map to its own
// identifiers (a Win32 registered format, an X atom) without escaping.
bool valid_name(const char* name) {
    const size_t length = name_length(name);
    if (length == 0 || length >= SAVANXP_CLIPBOARD_NAME_CAPACITY || name[0] == ' ') {
        return false;
    }
    for (size_t index = 0; index < length; ++index) {
        const unsigned char c = static_cast<unsigned char>(name[index]);
        if (c < 0x20 || c > 0x7e) {
            return false;
        }
    }
    return true;
}

const Entry* find(const Store& store, const char* name) {
    for (uint32_t index = 0; index < store.count; ++index) {
        if (same_name(store.entries[index].name, name)) {
            return &store.entries[index];
        }
    }
    return nullptr;
}

const Entry* text_entry(const Store& store) {
    return find(store, SAVANXP_CLIPBOARD_TEXT);
}

// Replaces the set with one already copied in full: only here does the
// visible content change, so a failure anywhere before leaves the previous
// set intact.
void install(Store& store, const Entry* entries, uint32_t count, uint8_t* data) {
    if (store.data != nullptr) {
        heap::free(store.data);
    }
    store.data = data;
    for (uint32_t index = 0; index < count; ++index) {
        store.entries[index] = entries[index];
    }
    store.count = count;
    store.sequence += 1;
    // Every descriptor that has not asked since is readable now.
    process::notify_device_readable();
}

int store_read(const Store& store, uint64_t user_buffer, size_t count) {
    if (count == 0) {
        return 0;
    }
    if (!process::validate_user_range(user_buffer, count, true)) {
        return negative_error(SX_EINVAL);
    }

    // No cursor: always from the start. A buffer smaller than the text gets
    // the prefix and the rest is lost, which is why CLIP_IOC_GET_INFO exists --
    // to size the buffer before reading.
    const Entry* text = text_entry(store);
    if (text == nullptr) {
        return 0;
    }
    const size_t copied = count < text->length ? count : text->length;
    if (copied != 0 && !process::copy_to_user(user_buffer, store.data + text->offset, copied)) {
        return negative_error(SX_EINVAL);
    }
    return static_cast<int>(copied);
}

int store_write(Store& store, uint64_t user_buffer, size_t count) {
    if (count > SAVANXP_CLIPBOARD_CAPACITY) {
        // Truncating silently would leave whoever pastes with half a text and
        // no way to find out; failing the copy is the useful error.
        return negative_error(SX_ENOSPC);
    }
    if (count == 0) {
        // write_handle stops a zero count before it reaches the device;
        // emptying the clipboard is CLIP_IOC_CLEAR.
        return 0;
    }
    if (!process::validate_user_range(user_buffer, count, false)) {
        return negative_error(SX_EINVAL);
    }

    // The bytes go to a new block before the old set is touched: if the user
    // pointer turns out to be invalid halfway, the previous clipboard stays
    // whole instead of half written.
    uint8_t* data = static_cast<uint8_t*>(heap::allocate(count));
    if (data == nullptr) {
        return negative_error(SX_ENOMEM);
    }
    if (!process::copy_from_user(data, user_buffer, count)) {
        heap::free(data);
        return negative_error(SX_EINVAL);
    }

    Entry text = {};
    memcpy(text.name, SAVANXP_CLIPBOARD_TEXT, sizeof(SAVANXP_CLIPBOARD_TEXT));
    text.offset = 0;
    text.length = static_cast<uint32_t>(count);
    install(store, &text, 1, data);
    return static_cast<int>(count);
}

int set_formats(Store& store, uint64_t argument) {
    savanxp_clipboard_set request = {};
    if (!process::validate_user_range(argument, sizeof(request), false) ||
        !process::copy_from_user(&request, argument, sizeof(request))) {
        return negative_error(SX_EINVAL);
    }
    if (request.count == 0 || request.count > SAVANXP_CLIPBOARD_MAX_FORMATS) {
        return negative_error(SX_EINVAL);
    }

    // At most eight entries of 80 bytes: the stack holds them. The data does
    // not go on the stack (eight pages, kKernelStackPages).
    savanxp_clipboard_entry entries[SAVANXP_CLIPBOARD_MAX_FORMATS];
    const size_t table_bytes = static_cast<size_t>(request.count) * sizeof(entries[0]);
    if (!process::validate_user_range(request.entries, table_bytes, false) ||
        !process::copy_from_user(entries, request.entries, table_bytes)) {
        return negative_error(SX_EINVAL);
    }

    uint64_t total = 0;
    for (uint32_t index = 0; index < request.count; ++index) {
        if (!valid_name(entries[index].name)) {
            return negative_error(SX_EINVAL);
        }
        for (uint32_t earlier = 0; earlier < index; ++earlier) {
            if (same_name(entries[earlier].name, entries[index].name)) {
                return negative_error(SX_EINVAL);
            }
        }
        total += entries[index].length;
    }
    if (total > SAVANXP_CLIPBOARD_CAPACITY) {
        return negative_error(SX_ENOSPC);
    }

    uint8_t* data = nullptr;
    if (total != 0) {
        data = static_cast<uint8_t*>(heap::allocate(static_cast<size_t>(total)));
        if (data == nullptr) {
            return negative_error(SX_ENOMEM);
        }
    }

    Entry installed[SAVANXP_CLIPBOARD_MAX_FORMATS];
    uint32_t offset = 0;
    for (uint32_t index = 0; index < request.count; ++index) {
        const uint32_t length = entries[index].length;
        if (length != 0 &&
            (!process::validate_user_range(entries[index].data, length, false) ||
             !process::copy_from_user(data + offset, entries[index].data, length))) {
            heap::free(data);
            return negative_error(SX_EINVAL);
        }
        memcpy(installed[index].name, entries[index].name, SAVANXP_CLIPBOARD_NAME_CAPACITY);
        installed[index].offset = offset;
        installed[index].length = length;
        offset += length;
    }
    install(store, installed, request.count, data);
    return 0;
}

int get_format(const Store& store, uint64_t argument) {
    savanxp_clipboard_get request = {};
    if (!process::validate_user_range(argument, sizeof(request), true) ||
        !process::copy_from_user(&request, argument, sizeof(request))) {
        return negative_error(SX_EINVAL);
    }
    if (name_length(request.name) >= SAVANXP_CLIPBOARD_NAME_CAPACITY) {
        return negative_error(SX_EINVAL);
    }
    const Entry* entry = find(store, request.name);
    if (entry == nullptr) {
        return negative_error(SX_ENOENT);
    }

    const uint32_t copied = request.capacity < entry->length ? request.capacity : entry->length;
    if (copied != 0 &&
        (!process::validate_user_range(request.buffer, copied, true) ||
         !process::copy_to_user(request.buffer, store.data + entry->offset, copied))) {
        return negative_error(SX_EINVAL);
    }
    request.length = entry->length;
    if (!process::copy_to_user(argument, &request, sizeof(request))) {
        return negative_error(SX_EINVAL);
    }
    return static_cast<int>(copied);
}

int enum_format(const Store& store, uint64_t argument) {
    savanxp_clipboard_format_info request = {};
    if (!process::validate_user_range(argument, sizeof(request), true) ||
        !process::copy_from_user(&request, argument, sizeof(request))) {
        return negative_error(SX_EINVAL);
    }
    if (request.index >= store.count) {
        return negative_error(SX_ENOENT);
    }
    const Entry& entry = store.entries[request.index];
    request.length = entry.length;
    memcpy(request.name, entry.name, SAVANXP_CLIPBOARD_NAME_CAPACITY);
    if (!process::copy_to_user(argument, &request, sizeof(request))) {
        return negative_error(SX_EINVAL);
    }
    return 0;
}

// A descriptor's state is the sequence it last got from CLIP_IOC_GET_INFO,
// plus one, so that zero -- a descriptor just opened -- has seen nothing.
uint64_t seen_state(uint64_t sequence) {
    return sequence + 1;
}

int store_ioctl(Store& store, uint64_t& state, uint64_t request, uint64_t argument, uint32_t granted_access) {
    switch (request) {
        case CLIP_IOC_GET_INFO: {
            const Entry* text = text_entry(store);
            savanxp_clipboard_info info = {};
            info.length = text != nullptr ? text->length : 0;
            info.capacity = SAVANXP_CLIPBOARD_CAPACITY;
            info.format = store.count == 0 ? SAVANXP_CLIPBOARD_FORMAT_EMPTY
                : text != nullptr          ? SAVANXP_CLIPBOARD_FORMAT_TEXT
                                           : SAVANXP_CLIPBOARD_FORMAT_OTHER;
            info.format_count = store.count;
            info.sequence = store.sequence;
            if (!process::validate_user_range(argument, sizeof(info), true) ||
                !process::copy_to_user(argument, &info, sizeof(info))) {
                return negative_error(SX_EINVAL);
            }
            state = seen_state(info.sequence);
            return 0;
        }
        case CLIP_IOC_CLEAR:
            if ((granted_access & object::access_write) == 0) {
                return negative_error(SX_EACCES);
            }
            install(store, nullptr, 0, nullptr);
            return 0;
        case CLIP_IOC_SET_FORMATS:
            if ((granted_access & object::access_write) == 0) {
                return negative_error(SX_EACCES);
            }
            return set_formats(store, argument);
        case CLIP_IOC_GET_FORMAT:
            return get_format(store, argument);
        case CLIP_IOC_ENUM_FORMAT:
            return enum_format(store, argument);
        default:
            return negative_error(SX_ENOSYS);
    }
}

// Readable means "changed since this descriptor last asked": a poll or an
// object wait on it sleeps until someone copies. A read never blocks either
// way; it returns the text, or zero bytes.
bool store_can_read(const Store& store, uint64_t state) {
    return state != seen_state(store.sequence);
}

// The device table takes plain functions: one set per store.
int clipboard_read(uint64_t buffer, size_t count) { return store_read(g_clipboard, buffer, count); }
int clipboard_write(uint64_t buffer, size_t count) { return store_write(g_clipboard, buffer, count); }
int clipboard_ioctl(uint64_t& state, uint64_t request, uint64_t argument, uint32_t granted_access) {
    return store_ioctl(g_clipboard, state, request, argument, granted_access);
}
bool clipboard_can_read(uint64_t state) { return store_can_read(g_clipboard, state); }

int drag_read(uint64_t buffer, size_t count) { return store_read(g_drag, buffer, count); }
int drag_write(uint64_t buffer, size_t count) { return store_write(g_drag, buffer, count); }
int drag_ioctl(uint64_t& state, uint64_t request, uint64_t argument, uint32_t granted_access) {
    return store_ioctl(g_drag, state, request, argument, granted_access);
}
bool drag_can_read(uint64_t state) { return store_can_read(g_drag, state); }

device::Device g_device = {
    .name = "clipboard",
    .read = clipboard_read,
    .write = clipboard_write,
    .ioctl = nullptr,
    .close = nullptr,
    .can_read = nullptr,
    .close_access = nullptr,
    .ioctl_state = clipboard_ioctl,
    .can_read_state = clipboard_can_read,
};

device::Device g_drag_device = {
    .name = "dragdata",
    .read = drag_read,
    .write = drag_write,
    .ioctl = nullptr,
    .close = nullptr,
    .can_read = nullptr,
    .close_access = nullptr,
    .ioctl_state = drag_ioctl,
    .can_read_state = drag_can_read,
};

bool g_ready = false;

} // namespace

namespace clipboard {

void initialize() {
    g_ready = device::register_node("/dev/clipboard", &g_device, true);
    g_ready = device::register_node("/dev/dragdata", &g_drag_device, true) && g_ready;
}

bool ready() {
    return g_ready;
}

} // namespace clipboard
