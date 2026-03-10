#include <stddef.h>

#include "net_client.h"
#include "net_dedicated.h"
#include "net_gui.h"
#include "net_io.h"
#include "net_loop.h"
#include "net_query.h"
#include "net_server.h"
#include "net_sdl.h"
#include "i_system.h"

boolean net_client_connected = false;
boolean net_client_received_wait_data = false;
net_waitdata_t net_client_wait_data = {0};
boolean net_waiting_for_launch = false;
char *net_player_name = "player";

sha1_digest_t net_server_wad_sha1sum = {0};
sha1_digest_t net_server_deh_sha1sum = {0};
unsigned int net_server_is_freedoom = 0;
sha1_digest_t net_local_wad_sha1sum = {0};
sha1_digest_t net_local_deh_sha1sum = {0};
unsigned int net_local_is_freedoom = 0;

boolean drone = false;
net_addr_t net_broadcast_addr = {0};

static boolean NET_StubInit(void)
{
    return false;
}

static void NET_StubSendPacket(net_addr_t *addr, net_packet_t *packet)
{
    (void)addr;
    (void)packet;
}

static boolean NET_StubRecvPacket(net_addr_t **addr, net_packet_t **packet)
{
    (void)addr;
    (void)packet;
    return false;
}

static void NET_StubAddrToString(net_addr_t *addr, char *buffer, int buffer_len)
{
    (void)addr;
    if (buffer != NULL && buffer_len > 0)
    {
        buffer[0] = '\0';
    }
}

static void NET_StubFreeAddress(net_addr_t *addr)
{
    (void)addr;
}

static net_addr_t *NET_StubResolveAddress(char *addr)
{
    (void)addr;
    return NULL;
}

net_module_t net_loop_client_module = {
    NET_StubInit,
    NET_StubInit,
    NET_StubSendPacket,
    NET_StubRecvPacket,
    NET_StubAddrToString,
    NET_StubFreeAddress,
    NET_StubResolveAddress,
};

net_module_t net_loop_server_module = {
    NET_StubInit,
    NET_StubInit,
    NET_StubSendPacket,
    NET_StubRecvPacket,
    NET_StubAddrToString,
    NET_StubFreeAddress,
    NET_StubResolveAddress,
};

net_module_t net_sdl_module = {
    NET_StubInit,
    NET_StubInit,
    NET_StubSendPacket,
    NET_StubRecvPacket,
    NET_StubAddrToString,
    NET_StubFreeAddress,
    NET_StubResolveAddress,
};

boolean NET_CL_Connect(net_addr_t *addr, net_connect_data_t *data)
{
    (void)addr;
    (void)data;
    return false;
}

void NET_CL_Disconnect(void)
{
}

void NET_CL_Run(void)
{
}

void NET_CL_Init(void)
{
}

void NET_CL_LaunchGame(void)
{
}

void NET_CL_StartGame(net_gamesettings_t *settings)
{
    (void)settings;
}

void NET_CL_SendTiccmd(ticcmd_t *ticcmd, int maketic)
{
    (void)ticcmd;
    (void)maketic;
}

boolean NET_CL_GetSettings(net_gamesettings_t *settings)
{
    (void)settings;
    return false;
}

void NET_Init(void)
{
}

void NET_BindVariables(void)
{
}

void NET_DedicatedServer(void)
{
    I_Error("Dedicated networking is not available in this port.");
}

void NET_WaitForLaunch(void)
{
}

net_context_t *NET_NewContext(void)
{
    return NULL;
}

void NET_AddModule(net_context_t *context, net_module_t *module)
{
    (void)context;
    (void)module;
}

void NET_SendPacket(net_addr_t *addr, net_packet_t *packet)
{
    (void)addr;
    (void)packet;
}

void NET_SendBroadcast(net_context_t *context, net_packet_t *packet)
{
    (void)context;
    (void)packet;
}

boolean NET_RecvPacket(net_context_t *context, net_addr_t **addr, net_packet_t **packet)
{
    (void)context;
    (void)addr;
    (void)packet;
    return false;
}

char *NET_AddrToString(net_addr_t *addr)
{
    (void)addr;
    return "offline";
}

void NET_FreeAddress(net_addr_t *addr)
{
    (void)addr;
}

net_addr_t *NET_ResolveAddress(net_context_t *context, char *address)
{
    (void)context;
    (void)address;
    return NULL;
}

int NET_StartLANQuery(void)
{
    return 0;
}

int NET_StartMasterQuery(void)
{
    return 0;
}

void NET_LANQuery(void)
{
    I_Error("LAN queries are not available in this port.");
}

void NET_MasterQuery(void)
{
    I_Error("Master server queries are not available in this port.");
}

void NET_QueryAddress(char *addr)
{
    (void)addr;
    I_Error("Network queries are not available in this port.");
}

net_addr_t *NET_FindLANServer(void)
{
    return NULL;
}

int NET_Query_Poll(net_query_callback_t callback, void *user_data)
{
    (void)callback;
    (void)user_data;
    return 0;
}

net_addr_t *NET_Query_ResolveMaster(net_context_t *context)
{
    (void)context;
    return NULL;
}

void NET_Query_AddToMaster(net_addr_t *master_addr)
{
    (void)master_addr;
}

boolean NET_Query_CheckAddedToMaster(boolean *result)
{
    if (result != NULL)
    {
        *result = false;
    }
    return false;
}

void NET_Query_MasterResponse(net_packet_t *packet)
{
    (void)packet;
}

void NET_SV_Init(void)
{
}

void NET_SV_Run(void)
{
}

void NET_SV_Shutdown(void)
{
}

void NET_SV_AddModule(net_module_t *module)
{
    (void)module;
}

void NET_SV_RegisterWithMaster(void)
{
}
