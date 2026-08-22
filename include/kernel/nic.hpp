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
    // Instala los callbacks hacia el stack. Se llama una vez, antes de bring_up.
    void (*attach)(const Events& events);
    // Deja el device operativo (reset, buffers, RX/TX enable). Idempotente.
    bool (*bring_up)();
    bool (*is_up)();
    // MAC propia. Valida recien despues de un bring_up() exitoso.
    const uint8_t* (*mac_address)();
    // Envia un frame Ethernet ya armado por el stack.
    bool (*transmit)(const void* frame, size_t length);
    // Drena el ring de recepcion, invocando Events::frame por cada frame. La
    // llaman tanto el handler de IRQ del driver como los loops de espera del
    // stack, asi que tiene que ser reentrante respecto de su propia IRQ.
    void (*poll_receive)();
    void (*get_stats)(Stats& stats);
};

// Driver candidato, mismo patron que display::Driver / audio::Driver.
struct Driver {
    const char* name;
    // Mayor gana. El orden de registro no importa.
    int priority;
    // Sondea el bus y deja el device reconocido, SIN levantarlo: la subida real
    // ocurre en bring_up(), cuando alguien abre /dev/net0 y pide NET_IOC_UP.
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
bool is_up();
const uint8_t* mac_address();
bool transmit(const void* frame, size_t length);
void poll_receive();
void get_stats(Stats& stats);

} // namespace nic
