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
#define SAVANXP_AUDIOD_VERSION 1u
#define SAVANXP_AUDIOD_MAX_DATAGRAM 1024u
#define SAVANXP_AUDIOD_HEADER_BYTES 16u
#define SAVANXP_AUDIOD_MAX_FRAMES \
    ((SAVANXP_AUDIOD_MAX_DATAGRAM - SAVANXP_AUDIOD_HEADER_BYTES) / 4u)

/* Control: otra magia, mismo socket. Sin handshake tampoco aca: cada mensaje
 * se identifica por el puerto origen (los de audio) o por el puerto destino
 * (los que fijan volumen). Todo versionado igual que el audio. */
#define SAVANXP_AUDIOD_CONTROL_MAGIC 0x53415543u /* "SAUC" */
#define SAVANXP_AUDIOD_CONTROL_VERSION 1u
#define SAVANXP_AUDIOD_NAME_BYTES 32u /* con NUL; se trunca sin piedad */

enum savanxp_audiod_control_kind {
    SAVANXP_AUDIOD_DECLARE = 1, /* "soy este": nombra el stream propio */
    SAVANXP_AUDIOD_SET_VOLUME = 2, /* fija el volumen de un stream ajeno */
    SAVANXP_AUDIOD_LIST = 3, /* pide el censo; responde con REPLies */
    SAVANXP_AUDIOD_REPLY = 4, /* un stream del censo */
};

struct savanxp_audiod_declare {
    uint32_t magic; /* CONTROL_MAGIC */
    uint16_t version; /* CONTROL_VERSION */
    uint16_t kind; /* DECLARE */
    char name[SAVANXP_AUDIOD_NAME_BYTES];
};

struct savanxp_audiod_set_volume {
    uint32_t magic;
    uint16_t version;
    uint16_t kind; /* SET_VOLUME */
    uint16_t target_port;
    uint8_t volume; /* 0..100 */
    uint8_t reserved;
};

struct savanxp_audiod_list_query {
    uint32_t magic;
    uint16_t version;
    uint16_t kind; /* LIST */
};

struct savanxp_audiod_list_reply {
    uint32_t magic;
    uint16_t version;
    uint16_t kind; /* REPLY */
    uint8_t count; /* streams en el censo */
    uint8_t index; /* cual es este */
    uint16_t port;
    uint8_t volume;
    uint8_t reserved;
    char name[SAVANXP_AUDIOD_NAME_BYTES];
};
/* IP propia en el user networking de QEMU, en el orden del arbol (igual que
 * kConfiguredIpv4 del kernel): el loopback es esta direccion, no 127.0.0.1,
 * que la pila no conoce. Si la red configurada cambia algun dia, el sendto
 * falla y los clientes siguen directo, asi que el fallo es ruidoso y local. */
#define SAVANXP_AUDIOD_HOST_IPV4 ((10u << 24) | (0u << 16) | (2u << 8) | 15u)

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
    /* Nombre que el demonio muestra para este stream ("doomgeneric").
     * Vacio = anonimo; se anuncia al pasar a remoto y cada 10 s, porque
     * la entrada expira con el silencio y un demonio nuevo no la conoce. */
    char client_name[SAVANXP_AUDIOD_NAME_BYTES];
    uint64_t last_declare_ms;
};

/* Entrada del censo: un stream con nombre y nivel, tal como lo ve el demonio. */
struct sx_audio_server_entry {
    uint16_t port;
    uint8_t volume;
    char name[SAVANXP_AUDIOD_NAME_BYTES];
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

/* Fija el nombre que el demonio muestra para este enlace. Sin efecto sobre
 * un stream ya anunciado: vale para el proximo. */
void sx_audio_server_set_client_name(
    struct sx_audio_server_link* link,
    const char* name);

/* Fija el volumen de un stream ajeno (0..100) por su puerto. socket_fd es
 * un socket UDP cualquiera. 0 ok, negativo -errno. Sin control de acceso:
 * monousuario y documentado. */
long sx_audio_server_set_volume(
    int socket_fd,
    uint16_t target_port,
    int volume);

/* Pide el censo y junta las respuestas hasta completarlo o 200 ms de
 * silencio. capacity topea lo guardado; el retorno dice cuantos hay en
 * total aunque no entren. Negativo -errno. */
long sx_audio_server_list(
    int socket_fd,
    struct sx_audio_server_entry* out,
    size_t capacity);

#ifdef __cplusplus
}
#endif
