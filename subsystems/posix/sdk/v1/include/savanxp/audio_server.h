#pragma once

#include <stddef.h>
#include <stdint.h>

/* Habla el demonio de audio con sus clientes.
 *
 * Transporte: datagramas UDP al loopback (la IP propia, sin ARP ni NIC de por
 * medio en la practica: el kernel cortocircuita el envio a si mismo). No hay
 * handshake ni conexion: el primer datagrama de un puerto registra el stream
 * y el silencio prolongado lo da de baja. Sin demonio, el sendto falla y el
 * cliente sigue directo a /dev/audio0, que es como se comportaba todo antes.
 *
 * Cada datagrama lleva esta cabecera (orden del host, solo loopback) seguida
 * de muestras s16le stereo al rate declarado, como maximo
 * SAVANXP_AUDIOD_MAX_FRAMES. Mas de 1024 bytes totales el kernel los rechaza,
 * asi que el tope no es negociable.
 */
#define SAVANXP_AUDIOD_PORT 47830u
#define SAVANXP_AUDIOD_MAGIC 0x53415544u /* "SAUD" */
/* IP propia en el user networking de QEMU, en el orden del arbol (igual que
 * kConfiguredIpv4 del kernel): el loopback es esta direccion, no 127.0.0.1,
 * que la pila no conoce. Si la red configurada cambia algun dia, el sendto
 * falla y los clientes siguen directo, asi que el fallo es ruidoso y local. */
#define SAVANXP_AUDIOD_HOST_IPV4 ((10u << 24) | (0u << 16) | (2u << 8) | 15u)
#define SAVANXP_AUDIOD_VERSION 1u
#define SAVANXP_AUDIOD_MAX_DATAGRAM 1024u
#define SAVANXP_AUDIOD_HEADER_BYTES 16u
#define SAVANXP_AUDIOD_MAX_FRAMES \
    ((SAVANXP_AUDIOD_MAX_DATAGRAM - SAVANXP_AUDIOD_HEADER_BYTES) / 4u)

struct savanxp_audiod_header {
    uint32_t magic; /* SAVANXP_AUDIOD_MAGIC */
    uint16_t version; /* SAVANXP_AUDIOD_VERSION */
    uint16_t channels; /* 2, lo unico que el demonio mezcla */
    uint32_t sample_rate_hz; /* tiene que ser el del dispositivo */
    uint32_t sequence; /* por datagrama, para contar perdidas */
};

/* Estado de un enlace cliente->demonio. El cliente lo guarda y lo pasa en
 * cada llamada; el demonio no existe para el mas alla de estos campos. */
struct sx_audio_server_link {
    int mode; /* 0 indeciso, 1 directo, 2 remoto */
    int udp_fd; /* valido en remoto */
    uint32_t sequence;
};

#ifdef __cplusplus
extern "C" {
#endif

/* Pone un enlace en reposo (sin socket). memset no vale: udp_fd 0 es un fd
 * valido y pareceria un socket. */
void sx_audio_server_link_init(struct sx_audio_server_link* link);

/* Cierra el socket del enlace si hay y lo deja como recien iniciado. */
void sx_audio_server_link_close(struct sx_audio_server_link* link);

/* Manda frames (s16le stereo a rate_hz) al demonio en datagramas de 1 KiB.
 * socket_fd es un socket UDP (el bind es automatico al primer envio);
 * *sequence avanza un paso por datagrama. 0 entregado, negativo -errno
 * (EIO = nadie escucha). Nunca bloquea. */
long sx_audio_server_send(
    int socket_fd,
    uint32_t* sequence,
    const int16_t* frames,
    size_t frame_count,
    uint32_t rate_hz);

/* Saca PCM por el demonio si esta, o directo a /dev/audio0 si no. link
 * arranca en modo 0 y aprende solo: el primer write con EBUSY lo pasa a
 * remoto, el primer write que entra lo deja en directo, y si el remoto
 * falla vuelve a probar directo en el proximo llamado. audio_fd es un fd
 * abierto de /dev/audio0 (abrir no contiende nunca). bytes tiene que ser
 * multiplo del frame S16 stereo. 1 entregado, negativo fatal. */
long sx_audio_server_output(
    struct sx_audio_server_link* link,
    int audio_fd,
    const int16_t* frames,
    size_t bytes,
    uint32_t rate_hz);

#ifdef __cplusplus
}
#endif
