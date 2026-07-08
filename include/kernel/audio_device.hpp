#pragma once

namespace audio_device {

// Registra /dev/audio0 con un dispatcher agnostico del backend: valida el buffer
// de usuario, gestiona el owner-pid (un solo escritor a la vez) y trocea la
// escritura en periodos, delegando cada periodo al backend de audio activo
// (namespace audio). Se llama en el boot solo si algun backend fue seleccionado.
bool initialize();

} // namespace audio_device
