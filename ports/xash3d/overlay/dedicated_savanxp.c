#include "common.h"
#include "net_ws.h"
#include "whereami.h"

int wai_getExecutablePath(char *out, int capacity, int *dirname_length)
{
    (void)out;
    (void)capacity;
    (void)dirname_length;
    /* The guest has no executable-path query. */
    return -1;
}

/* The platform has no TCP listener for XRCON. */
void XRcon_Init(void) {}
void XRcon_Shutdown(void) {}
void XRcon_Frame(void) {}
void XRcon_Print(const char *message) { (void)message; }
qboolean XRcon_IsActive(void) { return false; }

/* This build serves local game data over UDP. */
void HTTP_Init(void) {}
void HTTP_Shutdown(void) {}
void HTTP_Run(void) {}
qboolean HTTP_GetToMemory(const char *url, http_memory_cb_t callback, void *data)
{
    (void)url;
    (void)callback;
    (void)data;
    return false;
}
