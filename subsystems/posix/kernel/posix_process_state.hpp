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
};

// POSIX per-thread state (subsystem::register_thread_state_size): the poll()
// request in progress (isolation slice C). It was per process while a POSIX
// process had one thread; two threads polling at once need one each.
//
// The kernel only knows the thread is parked with a deadline; POSIX decides
// whether that was a poll. The request is a copy IN THE KERNEL, not a pointer
// to user memory, because the per-tick re-evaluation cannot switch CR3 a
// thousand times a second. User memory is touched once, on completion, to
// return the revents.
struct ThreadState {
    uint64_t poll_user_fds;
    uint32_t poll_count;
    uint32_t poll_active;
    savanxp_pollfd poll_entries[POSIX_MAX_POLL_ENTRIES];
};

// The thread's state, allocated on first use; nullptr without memory.
inline ThreadState* thread_state(process::Thread& thread) {
    return reinterpret_cast<ThreadState*>(process::thread_subsystem_state(thread));
}

// The state a parked thread already has, without allocating: the wait hook
// runs for every parked thread, and one that never polled has nothing to see.
inline ThreadState* existing_thread_state(const process::Thread& thread) {
    return thread.subsystem_state_bytes == sizeof(ThreadState)
        ? reinterpret_cast<ThreadState*>(thread.subsystem_state)
        : nullptr;
}

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
bool commit_poll_entries(ThreadState& state, process::Process& proc);
int evaluate_poll_entries(ThreadState& state, process::Process& proc);
void recheck_parked_poll(process::Thread& thread, uint64_t now_ms, bool honour_deadline);

} // namespace posix