#pragma once

#include <stdint.h>

#include "kernel/block.hpp"

namespace ramdisk {

// Ofrece una region de memoria como block device: en el LiveCD es la imagen de
// disco que Limine carga como modulo. Hay que llamarla ANTES de
// block::probe_all(); si no hay imagen adjunta, el enumerate no registra nada.
void attach_image(void* base, uint64_t size_bytes, bool writable, const char* name);

const block::Driver& driver();

} // namespace ramdisk
