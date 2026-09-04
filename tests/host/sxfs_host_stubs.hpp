#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kernel/vfs.hpp"

// Backends de mentira para correr el driver real (kernel/sxfs.cpp) en el host,
// sin bootear. El driver solo depende de block::{device_info,read,write} y de
// vfs::{ensure_directory,install_external_file}, asi que con estos dos stubs
// alcanza para ejercitar su maquina de estados de montaje contra una imagen
// SxFS en memoria. Mismo espiritu que el preview de sxgui en el host: el
// codigo bajo prueba es el del kernel, sin copias.
namespace hoststub {

// Publica `image` como el block device 0. `writable` se comporta como el flag
// del device real: con false, block::write falla sin tocar la imagen, que es
// como se reproduce un journal que no se pudo recuperar.
void attach_device(uint8_t* image, uint32_t sector_count, bool writable);
void detach_device();

// Hace fallar las proximas `count` escrituras y despues vuelve a la normalidad.
// Modela un error de I/O transitorio: el device es escribible, pero la
// recuperacion del journal se topa con un fallo y no se puede persistir.
void fail_next_writes(size_t count);

// Cuantas escrituras rechazo el device por ser de solo lectura.
size_t rejected_writes();

// Tabla de vnodes publicados por attach(). Sustituye al arbol del vfs.
void reset_vfs();
vfs::Vnode* find_node(const char* path);
size_t node_count();

} // namespace hoststub
