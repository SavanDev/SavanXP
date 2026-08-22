#include "kernel/nic.hpp"

namespace nic {

namespace {

const Driver* g_drivers[kMaxDrivers] = {};
size_t g_driver_count = 0;
const Driver* g_bound_driver = nullptr;
const Nic* g_nic = nullptr;

uint8_t g_no_mac[6] = {};

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
    if (g_nic != nullptr) {
        g_nic->attach(events);
    }
}

bool bring_up() { return g_nic != nullptr && g_nic->bring_up(); }
bool is_up() { return g_nic != nullptr && g_nic->is_up(); }
const uint8_t* mac_address() { return g_nic != nullptr ? g_nic->mac_address() : g_no_mac; }

bool transmit(const void* frame, size_t length) {
    return g_nic != nullptr && g_nic->transmit(frame, length);
}

void poll_receive() {
    if (g_nic != nullptr) {
        g_nic->poll_receive();
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
