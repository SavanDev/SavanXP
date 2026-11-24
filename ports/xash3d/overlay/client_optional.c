#include "common.h"
#include "voice.h"

void Voice_RegisterCvars(void) {}
qboolean Voice_Init(const char *codec, int quality, qboolean preinit) { return false; }
void Voice_Idle(double frametime) {}
qboolean Voice_IsRecording(void) { return false; }
void Voice_RecordStop(void) {}
void Voice_RecordStart(void) {}
void Voice_Disconnect(void) {}
void Voice_AddIncomingData(int ent, const byte *data, uint size, uint frames) {}
void Voice_StopChannel(int entnum) {}
void Voice_LoopbackAck(void) {}
voice_audio_info_t Voice_GetAudioInfo(void) { return (voice_audio_info_t){0}; }

void SteamBroker_Init(void) {}
void SteamBroker_Shutdown(void) {}
void SteamBroker_Frame(void) {}
void SteamBroker_TerminateGameConnection(void) {}
qboolean SteamBroker_InitiateGameConnection(netadr_t server, int challenge) { return false; }

CVAR_DEFINE_AUTO(voice_loopback, "0", FCVAR_PRIVILEGED, "voice loopback unavailable");
void CL_AddVoiceToDatagram(void) {}
void HTTP_AddCustomServer(const char *url) {}
void HTTP_AddDownload(const char *path, int size, qboolean process, resource_t *resource) {}
void HTTP_ClearCustomServers(void) {}
void HTTP_ResetProcessState(void) {}
qboolean Sound_LoadMPG(const char *name, const byte *buffer, size_t size) { return false; }
