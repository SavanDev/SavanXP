#pragma once

#include <stdint.h>

#include "kernel/cpu.hpp"

/* Spinlock de tickets.
 *
 * Cada core que quiere el lock saca un numero (`next_ticket`) y gira hasta que
 * `now_serving` llegue a el. Es FIFO: con un solo lock grande para todo el
 * kernel, un spinlock de test-and-set dejaria que el core que acaba de soltarlo
 * lo vuelva a tomar antes que el que espera, y un core podria quedarse girando
 * indefinidamente mientras los otros se lo pasan.
 *
 * No guarda ni restaura IF: quien lo toma ya tiene las interrupciones apagadas,
 * porque todas las entradas al kernel son interrupt gates. Tomarlo con IF=1 es
 * un error, no un caso a tolerar -- una interrupcion en el mismo core que
 * quiera el mismo lock se queda girando para siempre.
 *
 * No es recursivo. `owner` existe para poder preguntar "lo tengo yo?" y para
 * detectar la re-entrada en vez de colgarse en silencio.
 */
namespace sync {

constexpr uint32_t kNoOwner = 0xffffffffu;

// Los campos se tocan solo con los builtins __atomic_*, que ya fuerzan la
// lectura y escritura real en memoria: no hace falta `volatile`, y con el el
// tipo deja de ser literal y no se puede inicializar en tiempo de compilacion.
struct SpinLock {
    uint32_t next_ticket = 0;
    uint32_t now_serving = 0;
    // Indice del core que lo tiene (arch::x86_64::cpu_index()), o kNoOwner.
    // Solo lo escribe quien tiene el lock, asi que el unico que puede leer su
    // propio indice aca es el duenio: la lectura sin lock es valida para
    // preguntar "lo tengo yo?", y solo para eso.
    uint32_t owner = kNoOwner;
};

inline bool held_by_this_cpu(const SpinLock& lock) {
    return __atomic_load_n(&lock.owner, __ATOMIC_ACQUIRE) == arch::x86_64::cpu_index();
}

inline void acquire(SpinLock& lock) {
    const uint32_t ticket = __atomic_fetch_add(&lock.next_ticket, 1u, __ATOMIC_ACQ_REL);
    while (__atomic_load_n(&lock.now_serving, __ATOMIC_ACQUIRE) != ticket) {
        asm volatile("pause");
    }
    __atomic_store_n(&lock.owner, arch::x86_64::cpu_index(), __ATOMIC_RELEASE);
}

inline void release(SpinLock& lock) {
    // El duenio se borra ANTES de pasar el turno: si fuera despues, el siguiente
    // core podria tomar el lock y escribir su indice, y este lo pisaria.
    __atomic_store_n(&lock.owner, kNoOwner, __ATOMIC_RELEASE);
    __atomic_add_fetch(&lock.now_serving, 1u, __ATOMIC_ACQ_REL);
}

} // namespace sync
