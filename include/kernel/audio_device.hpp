#pragma once

#include <stdint.h>

namespace audio_device {

// Registra /dev/audio0 con un dispatcher agnostico del backend: valida el buffer
// de usuario, gestiona el owner-pid (un solo escritor a la vez) y trocea la
// escritura en periodos, delegando cada periodo al backend de audio activo
// (namespace audio). Se llama en el boot solo si algun backend fue seleccionado.
bool initialize();

// Suelta la sesion de audio (reproduccion y captura) si `pid` era el dueno, y
// para el stream. Es el gemelo de display::release_session_for y existe por el
// mismo motivo: en una salida limpia el dueno cierra su propio fd y el handler
// de close corre en SU contexto, pero en un kill ese handler corre en el
// contexto del que mata (windowd le manda SIGKILL al cliente cuando cierran la
// ventana), donde la comparacion contra current_pid() falla. Sin esto el device
// queda tomado para siempre y la proxima app que quiera sonido recibe EBUSY.
void release_session_for(uint32_t pid);

} // namespace audio_device
