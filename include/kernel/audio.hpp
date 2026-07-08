#pragma once

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

void set_backend(const Backend& backend);

bool ready();
bool get_info(savanxp_audio_info& info);
bool configure();
int submit_period(uint64_t user_buffer, uint32_t byte_count);
void stop();

} // namespace audio
