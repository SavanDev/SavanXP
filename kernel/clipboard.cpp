#include "kernel/clipboard.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/device.hpp"
#include "kernel/process.hpp"
#include "kernel/string.hpp"
#include "savanxp/syscall.h"

/*
 * Portapapeles del sistema (`/dev/clipboard`).
 *
 * Por que vive en el kernel y no en el window manager, que es donde lo pondria
 * un sistema con mas holgura: windowd guarda NUEVE descriptores por cliente
 * (seccion, teclado, puntero, tres eventos, launch, cursor hint, size hint)
 * contra el limite de 64 por proceso de process::kMaxFileHandles. Sumarle un
 * par de canales por cliente para copiar y pegar le bajaria el techo de
 * ventanas simultaneas, que ya es lo bastante ajustado. Como device node el
 * cliente lo abre al copiar o pegar y lo cierra enseguida, no cuesta ningun fd
 * permanente, y ademas queda disponible para procesos sin ventana.
 *
 * El contenido es un VALOR, no un stream: `write` reemplaza todo y `read`
 * devuelve todo desde el principio, sin cursor. No hay dueño ni negociacion de
 * formatos al estilo X11 -- el que copia deja los bytes y se olvida --, que es
 * el modelo de Windows y el que le corresponde a un sistema donde el que copio
 * puede haber terminado antes de que alguien pegue.
 */

namespace {

int negative_error(savanxp_error_code code) {
    return -static_cast<int>(code);
}

uint8_t g_content[SAVANXP_CLIPBOARD_CAPACITY];
uint32_t g_length = 0;
uint32_t g_format = SAVANXP_CLIPBOARD_FORMAT_EMPTY;
uint64_t g_sequence = 0;

int clipboard_read(uint64_t user_buffer, size_t count) {
    if (count == 0) {
        return 0;
    }
    if (!process::validate_user_range(user_buffer, count, true)) {
        return negative_error(SAVANXP_EINVAL);
    }

    // Sin cursor: siempre desde el principio. Un buffer mas chico que el
    // contenido se lleva el prefijo y el resto se pierde, que es por lo que
    // CLIP_IOC_GET_INFO existe -- para poder dimensionar antes de leer.
    size_t copied = count < g_length ? count : g_length;
    if (copied != 0 && !process::copy_to_user(user_buffer, g_content, copied)) {
        return negative_error(SAVANXP_EINVAL);
    }
    return static_cast<int>(copied);
}

int clipboard_write(uint64_t user_buffer, size_t count) {
    if (count > SAVANXP_CLIPBOARD_CAPACITY) {
        // Truncar en silencio dejaria al que pega con medio texto y sin forma
        // de enterarse; que falle el que copia es el error util.
        return negative_error(SAVANXP_ENOSPC);
    }
    if (count != 0 && !process::validate_user_range(user_buffer, count, false)) {
        return negative_error(SAVANXP_EINVAL);
    }

    // Se copia a un temporal antes de pisar el contenido vigente: si el puntero
    // de userland resulta invalido a mitad de camino, el portapapeles anterior
    // sigue intacto en vez de quedar a medio escribir. El temporal es estatico
    // y NO del stack: el stack de kernel son cuatro paginas (16 KiB, ver
    // kKernelStackPages) y un buffer de 8 KiB ahi se comeria la mitad en un
    // solo frame -- el mismo error que tenia ensure_capacity en SxFS.
    static uint8_t g_staging[SAVANXP_CLIPBOARD_CAPACITY];
    if (count != 0 && !process::copy_from_user(g_staging, user_buffer, count)) {
        return negative_error(SAVANXP_EINVAL);
    }

    if (count != 0) {
        memcpy(g_content, g_staging, count);
    }
    g_length = static_cast<uint32_t>(count);
    g_format = count != 0 ? SAVANXP_CLIPBOARD_FORMAT_TEXT : SAVANXP_CLIPBOARD_FORMAT_EMPTY;
    g_sequence += 1;
    return static_cast<int>(count);
}

int clipboard_ioctl(uint64_t request, uint64_t argument) {
    switch (request) {
        case CLIP_IOC_GET_INFO: {
            savanxp_clipboard_info info = {};
            info.length = g_length;
            info.capacity = SAVANXP_CLIPBOARD_CAPACITY;
            info.format = g_format;
            info.reserved0 = 0;
            info.sequence = g_sequence;
            if (!process::validate_user_range(argument, sizeof(info), true) ||
                !process::copy_to_user(argument, &info, sizeof(info))) {
                return negative_error(SAVANXP_EINVAL);
            }
            return 0;
        }
        case CLIP_IOC_CLEAR:
            g_length = 0;
            g_format = SAVANXP_CLIPBOARD_FORMAT_EMPTY;
            g_sequence += 1;
            return 0;
        default:
            return negative_error(SAVANXP_ENOSYS);
    }
}

bool clipboard_can_read() {
    // Siempre legible: leer un portapapeles vacio devuelve cero bytes, no
    // bloquea. Sin esto un poll() sobre el device quedaria colgado.
    return true;
}

device::Device g_device = {
    .name = "clipboard",
    .read = clipboard_read,
    .write = clipboard_write,
    .ioctl = clipboard_ioctl,
    .close = nullptr,
    .can_read = clipboard_can_read,
};

bool g_ready = false;

} // namespace

namespace clipboard {

void initialize() {
    g_ready = device::register_node("/dev/clipboard", &g_device, true);
}

bool ready() {
    return g_ready;
}

} // namespace clipboard
