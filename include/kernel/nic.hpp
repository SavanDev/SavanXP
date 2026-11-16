#pragma once

#include <stddef.h>
#include <stdint.h>

namespace nic {

// Callbacks con los que el driver habla hacia arriba. Los instala el stack en
// attach(); el driver no conoce nada mas del mundo de net::.
struct Events {
    // Un frame Ethernet completo recibido, ya sin el CRC del ring.
    void (*frame)(const uint8_t* data, size_t length);
    // Un valor de savanxp_net_status que solo el driver puede diagnosticar
    // (TX_FAILED, TX_TIMEOUT, RX_INVALID, BRING_UP_FAILED). El resto de los
    // estados -- ARP, ping, READY/IDLE -- los maneja el stack.
    void (*status)(uint32_t net_status);
};

struct Stats {
    uint32_t tx_frames;
    uint32_t rx_frames;
    uint32_t tx_errors;
    uint32_t rx_errors;
};

// Vtable del NIC: es TODO lo que el stack sabe del hardware. Del otro lado no
// se filtran registros, puertos de I/O ni el layout del ring de ningun chip.
struct Nic {
    // Install stack callbacks once, before either initialization entry.
    void (*attach)(const Events& events);
    // Blocking, idempotent setup over the same steps as prepare.
    bool (*bring_up)();
    bool (*is_up)();
    // Local MAC, valid after either setup entry reports readiness.
    const uint8_t* (*mac_address)();
    // Try to submit a frame without waiting for DMA. False leaves ownership
    // with the dispatcher, which retries its bounded queue on the next pass.
    bool (*transmit)(const void* frame, size_t length);
    // Deliver at most one fixed RX budget. Kernel entries serialize callbacks
    // with socket operations through the big kernel lock.
    void (*poll_receive)();
    void (*get_stats)(Stats& stats);
    // One initialization step: 0 ready, -EAGAIN pending, other negative failure.
    int (*prepare)();
};

// Driver candidato, mismo patron que display::Driver / audio::Driver.
struct Driver {
    const char* name;
    // Mayor gana. El orden de registro no importa.
    int priority;
    // Probe and recognize hardware without starting it. NET_IOC_UP uses the
    // blocking bring_up entry; native sockets advance it through prepare.
    bool (*probe)();
    // Solo valido despues de un probe() que devolvio true.
    const Nic& (*nic)();
};

constexpr size_t kMaxDrivers = 4;

bool register_driver(const Driver& driver);
// Corre los probes por prioridad descendente y ata el primero que reclama el
// hardware. Devuelve el driver elegido, o nullptr si no hay NIC.
const Driver* bind_best();
const Driver* bound_driver();

// Dispatcher. Todo es no-op seguro si no quedo ningun NIC atado.
bool present();
void attach(const Events& events);
bool bring_up();
int prepare();
bool is_up();
const uint8_t* mac_address();
// Acceptance copies the frame into a bounded queue. An optional owner token
// allows the protocol to cancel queued frames before releasing its state.
bool transmit(const void* frame, size_t length, const void* owner = nullptr);
void cancel_transmit(const void* owner);
void poll_receive();
void get_stats(Stats& stats);

} // namespace nic
