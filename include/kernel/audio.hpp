#pragma once

#include <stddef.h>
#include <stdint.h>

#include "savanxp/syscall.h"

namespace audio {

// Vtable de operaciones dependientes del hardware de sonido. Cada backend
// (virtio-sound, AC97) llena una instancia estatica con punteros a sus propias
// funciones y audio:: solo despacha, igual que display::Backend para la GPU.
// La logica comun (registro de /dev/audio0, owner-pid, validacion de usuario y
// troceado en periodos) vive en audio_device.cpp, no aca.
struct Backend {
    bool (*ready)();
    bool (*get_info)(savanxp_audio_info& info);
    // Prepara el stream para reproducir (idempotente). Se llama antes del primer
    // periodo de cada escritura; el backend cachea el estado ya preparado.
    bool (*configure)();
    // Envia un periodo de PCM leido de memoria de usuario. Devuelve 0 en exito o
    // un -errno negativo. El troceado a period_bytes lo hace el llamador.
    int (*submit_period)(uint64_t user_buffer, uint32_t byte_count);
    // Detiene y libera el stream (fin de la sesion de escritura / close del fd).
    void (*stop)();
};

// Driver de audio candidato, espejo de display::Driver. Los probes corren por
// prioridad y el primero que reclama el hardware se lleva el backend; si
// ninguno lo hace no queda backend atado y audio_device no registra /dev/audio0.
struct Driver {
    const char* name;
    // Mayor gana. El orden de registro no importa.
    int priority;
    // Inicializa el driver y responde si reclama el hardware. bind_best() corta
    // en el primer true: un driver descartado por prioridad no se inicializa.
    bool (*probe)();
    // Solo valido despues de un probe() que devolvio true.
    const Backend& (*backend)();
};

constexpr size_t kMaxDrivers = 8;

// false si la tabla esta llena o el driver viene incompleto.
bool register_driver(const Driver& driver);
// Corre los probes por prioridad descendente y ata el backend del primero que
// reclama el hardware. Devuelve el driver elegido, o nullptr si no hay sonido.
const Driver* bind_best();
// El driver que ato bind_best(), o nullptr.
const Driver* bound_driver();

// Mecanismo de bajo nivel detras de bind_best().
void set_backend(const Backend& backend);

bool ready();
bool get_info(savanxp_audio_info& info);
bool configure();
int submit_period(uint64_t user_buffer, uint32_t byte_count);
void stop();

} // namespace audio
