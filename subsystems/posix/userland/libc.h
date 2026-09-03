#pragma once

#include "savanxp/libc.h"

/* mmap/munmap vienen del runtime POSIX. La capa cruda tenia su propia copia con
 * otra firma, y como era la que se quedaba con el nombre, un error volvia como
 * -errno casteado a puntero -- que no es MAP_FAILED, asi que quien comparaba
 * contra MAP_FAILED tomaba el fallo por exito. */
#include <sys/mman.h>
