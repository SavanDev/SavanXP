#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kernel/process.hpp"

/*
 * Estado privado por proceso de POSIX (rebanada B de aislamiento).
 *
 * El kernel reserva estos bytes segun lo que POSIX declara en
 * subsystem::register_process_state_size y no mira dentro. Ac vive el
 * directorio actual, que es estado de RESOLUCION DE RUTAS: pertenece a la
 * capa que traduce, no al kernel. Por eso Process ya no tiene campo cwd.
 *
 * Para escribirlo no hace falta ningun mecanismo nuevo: se copia al buffer
 * del proceso con la misma primitiva que usa el resto de la traduccion.
 */

namespace posix {

// Descriptores que admite un poll. Es el tamano de la tabla de handles del
// proceso porque no puede haber mas: pedir mas solo se logra repitiendo un fd,
// que nadie hace. El tope importa porque la copia en kernel del pedido vive
// en el estado privado y es lo que deja reevaluar a un proceso bloqueado sin
// tocar su memoria de usuario.
constexpr size_t POSIX_MAX_POLL_ENTRIES = process::kMaxFileHandles;

struct ProcessState {
    char cwd[process::kProcessPathLength];
    // Pedido de poll() en curso (rebanada C de aislamiento).
    //
    // Antes vivia en Process, en el kernel, y el kernel lo reevaluaba en cada
    // tick. Ahora es de POSIX: el kernel solo sabe que el proceso esta
    // estacionado con vencimiento, y POSIX decide si eso era un poll.
    //
    // Es una copia EN KERNEL del pedido (no un puntero a memoria de usuario)
    // porque la reevaluacion por tick no puede cambiar de CR3 mil veces por
    // segundo. La memoria de usuario se toca una sola vez, al completar, para
    // devolver los revents.
    uint64_t poll_user_fds;
    uint32_t poll_count;
    uint32_t poll_active;
    savanxp_pollfd poll_entries[POSIX_MAX_POLL_ENTRIES];
};

// El estado del proceso actual, o nullptr si no hay (nativo, o sin memoria).
// El heap devuelve memoria alineada a 16 (heap::allocate), pero el puntero
// guardado en Process es uint8_t*: por eso el reinterpret_cast y no un
// static_cast.
inline ProcessState* state(const process::Process& proc) {
    return reinterpret_cast<ProcessState*>(proc.subsystem_state);
}

// El directorio actual, con la regla de POSIX: vacio o sin estado es "/".
inline const char* cwd_or_root(const process::Process& proc) {
    const ProcessState* const owned = state(proc);
    if (owned == nullptr || owned->cwd[0] == '\0') {
        return "/";
    }
    return owned->cwd;
}

// The poll path (slice C), implemented in posix_translation.cpp.
bool commit_poll_entries(ProcessState& state, process::Process& proc);
int evaluate_poll_entries(ProcessState& state, process::Process& proc);
void recheck_parked_poll(process::Thread& thread, uint64_t now_ms, bool honour_deadline);

} // namespace posix