#include "kernel/nic.hpp"
#include "abi/savanxp_abi.h"
#include "kernel/string.hpp"
#include "kernel/timer.hpp"

namespace nic {

namespace {

const Driver* g_drivers[kMaxDrivers] = {};
size_t g_driver_count = 0;
const Driver* g_bound_driver = nullptr;
const Nic* g_nic = nullptr;

uint8_t g_no_mac[6] = {};
// Kernel entries, including IRQs and the BSP heartbeat, hold the big kernel
// lock. No driver callback sleeps or releases it. Queue ownership is therefore
// shared by POSIX and SUN without an additional lock or recursive RX entry.
struct TxFrame {
    const void* owner;
    size_t length;
    uint64_t deadline;
    uint8_t data[1600];
};
TxFrame g_tx[64] = {};
size_t g_tx_head = 0, g_tx_size = 0;
Events g_events = {};

void drain_tx() {
    for (size_t budget = 0; budget < 16 && g_tx_size; ++budget) {
        auto& frame = g_tx[g_tx_head];
        if (!g_nic->transmit(frame.data, frame.length)) {
            if (timer::monotonic_ns() < frame.deadline) break;
            if (g_events.status) g_events.status(SAVANXP_NET_STATUS_TX_TIMEOUT);
        }
        g_tx_head = (g_tx_head + 1) % 64;
        --g_tx_size;
    }
}

} // namespace

bool register_driver(const Driver& driver) {
    if (g_driver_count >= kMaxDrivers || driver.probe == nullptr || driver.nic == nullptr) {
        return false;
    }
    g_drivers[g_driver_count] = &driver;
    ++g_driver_count;
    return true;
}

const Driver* bind_best() {
    // Seleccion directa sobre el array, igual que display::bind_best: se corta
    // en el primero que reclama, asi un driver descartado por prioridad no
    // llega a tocar el bus.
    bool probed[kMaxDrivers] = {};
    for (size_t attempt = 0; attempt < g_driver_count; ++attempt) {
        size_t best = kMaxDrivers;
        for (size_t i = 0; i < g_driver_count; ++i) {
            if (probed[i]) {
                continue;
            }
            if (best == kMaxDrivers || g_drivers[i]->priority > g_drivers[best]->priority) {
                best = i;
            }
        }
        if (best == kMaxDrivers) {
            break;
        }

        probed[best] = true;
        const Driver* candidate = g_drivers[best];
        if (candidate->probe()) {
            g_bound_driver = candidate;
            g_nic = &candidate->nic();
            return candidate;
        }
    }
    return nullptr;
}

const Driver* bound_driver() { return g_bound_driver; }

bool present() { return g_nic != nullptr; }

void attach(const Events& events) {
    g_events = events;
    if (g_nic != nullptr) {
        g_nic->attach(events);
    }
}

bool bring_up() { return g_nic != nullptr && g_nic->bring_up(); }
int prepare() { return g_nic != nullptr ? g_nic->prepare() : -SX_ENODEV; }
bool is_up() { return g_nic != nullptr && g_nic->is_up(); }
const uint8_t* mac_address() { return g_nic != nullptr ? g_nic->mac_address() : g_no_mac; }

bool transmit(const void* frame, size_t length, const void* owner) {
    if (!g_nic || !g_nic->is_up() || !frame || !length || length > 1600 || g_tx_size == 64) return false;
    auto& queued = g_tx[(g_tx_head + g_tx_size) % 64];
    queued.owner = owner;
    queued.length = length;
    queued.deadline = timer::monotonic_ns() + 100000000ull;
    memcpy(queued.data, frame, length);
    ++g_tx_size;
    return true;
}

void cancel_transmit(const void* owner) {
    if (!owner) return;
    size_t kept = 0;
    for (size_t i = 0; i < g_tx_size; ++i) {
        auto& frame = g_tx[(g_tx_head + i) % 64];
        if (frame.owner == owner) continue;
        if (kept != i) g_tx[(g_tx_head + kept) % 64] = frame;
        ++kept;
    }
    g_tx_size = kept;
}

void poll_receive() {
    if (g_nic != nullptr) {
        drain_tx();
        g_nic->poll_receive();
        drain_tx();
    }
}

void get_stats(Stats& stats) {
    if (g_nic == nullptr) {
        stats = {};
        return;
    }
    g_nic->get_stats(stats);
}

} // namespace nic
