#pragma once

#include "sys/types.h"
/* struct timeval is defined once, in <sys/select.h>, as POSIX puts it; a
 * second definition here broke every program that included both. */
#include "sys/select.h"

int gettimeofday(struct timeval* value, void* timezone_ptr);

