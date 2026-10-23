#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Gives the core to another ready thread, if there is one. Always 0. */
int sched_yield(void);

#ifdef __cplusplus
}
#endif
