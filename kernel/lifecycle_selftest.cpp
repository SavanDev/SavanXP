// The shared start and stop self-test. See docs/kernel/DRIVER_LIFECYCLE.md.

#include "kernel/lifecycle_selftest.hpp"

#include "arch/cpu.hpp"
#include "kernel/ac97.hpp"
#include "kernel/console.hpp"
#include "kernel/inventory.hpp"
#include "kernel/lifecycle.hpp"
#include "kernel/pci.hpp"
#include "kernel/physical_memory.hpp"
#include "kernel/rtl8139.hpp"
#include "kernel/string.hpp"

namespace {

// Every driver written to the contracts, by the name the boot option uses.
struct Entry {
    const char* name;
    const lifecycle::Subject& (*subject)();
};

const Entry kSubjects[] = {
    {"rtl8139", &rtl8139::lifecycle_subject},
    {"ac97", &ac97::lifecycle_subject},
};

constexpr unsigned kCycles = 4;
// The cycle brought up through bring_up(), when the driver has one.
constexpr unsigned kBroughtUpCycle = 2;

bool fail(const lifecycle::Subject& subject, const char* step) {
    console::printf("lifecycle: selftest failed: %s: %s\n", subject.driver, step);
    return false;
}

// Nothing of the device left behind: the pages, the claims, the state.
bool nothing_left(const lifecycle::Subject& subject, const pci::DeviceInfo& device, uint64_t free_pages,
    const char* listed_driver, uint32_t state, uint32_t reason) {
    if (memory::free_page_count() != free_pages) {
        console::printf("lifecycle: %s: %llu free pages, %llu before\n", subject.driver,
            memory::free_page_count(), free_pages);
        return false;
    }
    if (inventory::pci_claim_count(device) != 0) {
        console::printf("lifecycle: %s: %u ranges still claimed\n", subject.driver,
            static_cast<unsigned>(inventory::pci_claim_count(device)));
        return false;
    }
    const char* holder = nullptr;
    uint32_t current_state = 0;
    uint32_t current_reason = 0;
    if (!inventory::pci_holder(device, holder, current_state, current_reason) ||
        strcmp(holder, listed_driver) != 0 || current_state != state || current_reason != reason) {
        console::printf("lifecycle: %s: the device reads as '%s' %u/%u\n", subject.driver,
            holder != nullptr ? holder : "?", static_cast<unsigned>(current_state),
            static_cast<unsigned>(current_reason));
        return false;
    }
    if (!subject.idle()) {
        console::printf("lifecycle: %s: the driver still holds state of its own\n", subject.driver);
        return false;
    }
    return true;
}

bool run(const lifecycle::Subject& subject) {
    pci::DeviceInfo device = {};
    if (!subject.find(device)) {
        return fail(subject, "no device to test with");
    }
    const uint64_t free_pages = memory::free_page_count();

    // A failure injected at each stage gives back exactly what the stages
    // before took. Stage 1 fails before the claim: the device is as it was.
    for (unsigned stage = 1; stage <= subject.stages; ++stage) {
        lifecycle::inject_failure(subject.driver, stage);
        const bool started = subject.start(device);
        lifecycle::inject_failure(nullptr, 0);
        if (started) {
            return fail(subject, "a start went through an injected failure");
        }
        const bool claimed = stage > 1;
        if (!nothing_left(subject, device, free_pages, claimed ? subject.driver : "", SXN_DEVICE_UNBOUND,
                claimed ? SXN_DEVICE_REASON_START_FAILED : SXN_DEVICE_REASON_NOT_CLAIMED)) {
            console::printf("lifecycle: %s: after a failure injected at stage %u\n", subject.driver, stage);
            return fail(subject, "a failed start left something behind");
        }
    }

    for (unsigned cycle = 0; cycle < kCycles; ++cycle) {
        if (!subject.start(device)) {
            return fail(subject, "the device did not start");
        }
        if (inventory::pci_claim_count(device) == 0) {
            return fail(subject, "a started device claims nothing");
        }
        const uint8_t vector = subject.vector != nullptr ? subject.vector() : 0;
        const uint8_t handlers = vector != 0 ? arch::interrupt_handler_count(vector) : 0;
        if (cycle == kBroughtUpCycle && subject.bring_up != nullptr && !subject.bring_up()) {
            return fail(subject, "the device did not come up");
        }
        if (!subject.stop()) {
            return fail(subject, "the device did not stop");
        }
        if (vector != 0 && arch::interrupt_handler_count(vector) != static_cast<uint8_t>(handlers - 1)) {
            return fail(subject, "the handler is still on its line");
        }
        if (!nothing_left(subject, device, free_pages, "", SXN_DEVICE_UNBOUND, SXN_DEVICE_REASON_NOT_CLAIMED)) {
            console::printf("lifecycle: %s: after stop %u\n", subject.driver, cycle + 1);
            return fail(subject, "a stop left something behind");
        }
    }
    console::printf("lifecycle: %s %x:%x.%u: %u injected failures and %u start/stop cycles, nothing left\n",
        subject.driver, static_cast<unsigned>(device.bus), static_cast<unsigned>(device.slot),
        static_cast<unsigned>(device.function), subject.stages, kCycles);
    return true;
}

const lifecycle::Subject* subject_named(const char* name, size_t length) {
    for (const Entry& entry : kSubjects) {
        if (strlen(entry.name) == length && memcmp(entry.name, name, length) == 0) {
            return &entry.subject();
        }
    }
    return nullptr;
}

} // namespace

namespace lifecycle {

void run_boot_selftest(const char* names, size_t length) {
    bool passed = length != 0;
    if (!passed) {
        console::write_line("lifecycle: selftest failed: lifecycle-selftest names no driver");
        return;
    }
    size_t begin = 0;
    while (begin <= length && passed) {
        size_t end = begin;
        while (end < length && names[end] != ',') {
            ++end;
        }
        const Subject* subject = subject_named(names + begin, end - begin);
        if (subject == nullptr) {
            char name[32];
            const size_t name_length = end - begin < sizeof(name) - 1 ? end - begin : sizeof(name) - 1;
            memcpy(name, names + begin, name_length);
            name[name_length] = 0;
            console::printf("lifecycle: selftest failed: no driver called '%s' is written to the contracts\n",
                name);
            return;
        }
        passed = run(*subject);
        begin = end + 1;
    }
    if (passed) {
        console::write_line("lifecycle: selftest passed");
    }
}

} // namespace lifecycle
