#include "kernel/serial_dev.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/console.hpp"
#include "kernel/device.hpp"
#include "kernel/process.hpp"
#include "savanxp/syscall.h"

/*
 * Salida de diagnostico solo-serial (`/dev/serial`).
 *
 * Por que existe habiendo stderr: console::write_char escribe en el puerto
 * serie Y en la consola de texto del framebuffer. Con el escritorio andando eso
 * significa que cualquier printf de un proceso grafico pinta celdas de texto
 * ENCIMA de lo compuesto. Para un error fatal da igual -- la sesion se termina
 * igual --, pero un proceso que quiera emitir mediciones mientras el escritorio
 * se usa necesita un canal que no toque la pantalla.
 *
 * Es de solo escritura y sin estado: no hay cursor, no hay buffer y no hay
 * lectura. Lo que se escribe sale por COM1 (y por el puerto de debug del
 * emulador), que es donde `build.ps1` ya redirige con `-serial file:`.
 */

namespace {

constexpr size_t kChunkBytes = 256;

bool g_ready = false;
device::Device g_device = {};

int serial_write(uint64_t user_buffer, size_t count) {
    size_t written = 0;

    if (count == 0) {
        return 0;
    }
    if (!process::validate_user_range(user_buffer, count, false)) {
        return -static_cast<int>(SAVANXP_EINVAL);
    }

    /* De a pedazos y por una copia intermedia: el buffer es de usuario y no se
     * lee directo, y un write grande no puede quedarse con una copia entera en
     * el stack del kernel. */
    while (written < count) {
        char chunk[kChunkBytes];
        const size_t remaining = count - written;
        const size_t take = remaining < kChunkBytes ? remaining : kChunkBytes;

        if (!process::copy_from_user(chunk, user_buffer + written, take)) {
            return written != 0 ? static_cast<int>(written) : -static_cast<int>(SAVANXP_EINVAL);
        }
        console::serial_write(chunk, take);
        written += take;
    }

    return static_cast<int>(written);
}

} // namespace

namespace serial_dev {

void initialize() {
    g_device.write = serial_write;
    g_ready = device::register_node("/dev/serial", &g_device, true);
}

bool ready() {
    return g_ready;
}

} // namespace serial_dev
