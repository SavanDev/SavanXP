#pragma once

#include <stdint.h>

/*
 * Protocolo WM <-> cliente de shell (barra de tareas).
 *
 * Extension OPCIONAL de savanxp/wm_protocol.h. Un cliente normal no tiene
 * estos descriptores abiertos: el WM los cablea unicamente al proceso que
 * lanza en el rol de shell. Eso importa porque los fds del protocolo se pagan
 * POR CLIENTE y windowd ya guarda nueve contra el limite de 64 por proceso
 * (ver docs/WM_SUBSYSTEM.md), asi que estos dos son dos fds en todo el sistema
 * y no dos por ventana.
 *
 * Por que existe: la barra de tareas es un CLIENTE, no chrome del WM -- el
 * modelo de explorer.exe, y el mismo layering que separo a shellui y progman
 * del window manager. Pero un cliente no puede saber que ventanas hay: el
 * z-order, los titulos y el foco los tiene el WM. Este es el canal por el que
 * se los cuenta.
 *
 * NOTA: el subsistema nativo mantiene su espejo aparte (ver
 * docs/SYSTEM_LAYERING.md). Este header es la fuente canonica.
 */

/*
 * Lista de ventanas: SECCION compartida, no pipe.
 *
 * La lista pesa cerca de un KiB y un pipe de 8 KiB puede aceptarla a medias si
 * el cliente se atrasa. Una lectura parcial desincroniza el stream para
 * siempre, y este WM ya se colgo una vez por escrituras parciales a pipe. Con
 * una seccion no hay mensajes que puedan partirse: el WM pisa el contenido y el
 * cliente lee el ultimo estado, que es exactamente lo que una barra de tareas
 * necesita -- no le sirve el historico, solo el ahora.
 *
 * El cliente la mapea de SOLO LECTURA.
 */
#define SAVANXP_WM_FD_WINDOW_LIST 12

/*
 * Pedidos del shell al WM. Aca si un pipe es lo correcto: los mensajes son
 * chicos, esporadicos (los produce un click) y van en la direccion que no
 * tiene la restriccion de "el WM nunca bloquea".
 */
#define SAVANXP_WM_FD_SHELL_REQUEST 13

/* Ultimo descriptor del protocolo CON la extension de shell. El WM lo necesita
 * para no cerrar por numero un fd de origen que caiga en el rango: puede ser el
 * destino recien mapeado de otro dup2, y cerrarlo deja el hueco libre para que
 * lo tome el primer open/dup que venga. */
#define SAVANXP_WM_SHELL_FD_LAST SAVANXP_WM_FD_SHELL_REQUEST

#define SAVANXP_WM_SHELL_PROTOCOL_VERSION 1u
#define SAVANXP_WM_WINDOW_TITLE_CAPACITY 64u
/* Igual que WINDOWD_MAX_OVERLAY_CLIENTS, mas el shell. */
#define SAVANXP_WM_MAX_WINDOWS 13u

enum savanxp_wm_window_flags {
    SAVANXP_WM_WINDOW_FLAG_NONE = 0,
    SAVANXP_WM_WINDOW_FLAG_ACTIVE = 1u << 0,
    SAVANXP_WM_WINDOW_FLAG_MINIMIZED = 1u << 1,
};

struct savanxp_wm_window_entry {
    /* Identidad estable mientras la ventana viva. NO es el indice en el
     * arreglo: el arreglo se reordena con el z-order, asi que un click que
     * llegara con un indice viejo activaria OTRA ventana. El WM resuelve el id
     * contra sus clientes al recibir el pedido. */
    uint32_t window_id;
    uint32_t flags; /* savanxp_wm_window_flags */
    /* Icono chico, resuelto por el WM desde los recursos SXE del binario (ver
     * docs/SXE_FORMAT.md). El cliente lo dibuja con desktop_icon_small(). */
    uint32_t icon_id;
    uint32_t reserved0;
    char title[SAVANXP_WM_WINDOW_TITLE_CAPACITY];
};

/*
 * Seqlock. El WM sube `sequence` a impar antes de tocar las entradas y a par
 * cuando termino; el cliente lee la secuencia, copia, y vuelve a leerla: si
 * cambio o quedo impar, reintenta. Hace falta de verdad -- el WM es otro
 * proceso y lo pueden desalojar en medio de la escritura.
 */
struct savanxp_wm_window_list {
    uint32_t version; /* SAVANXP_WM_SHELL_PROTOCOL_VERSION */
    uint32_t count;
    uint64_t sequence;
    struct savanxp_wm_window_entry windows[SAVANXP_WM_MAX_WINDOWS];
};

enum savanxp_wm_shell_action {
    /* Traer al frente, restaurando si estaba minimizada. */
    SAVANXP_WM_SHELL_ACTIVATE = 1,
    /* Minimizar. Es lo que hace click sobre el boton de la ventana YA activa,
     * como en Win95. */
    SAVANXP_WM_SHELL_MINIMIZE = 2,
};

struct savanxp_wm_shell_request {
    uint32_t action; /* savanxp_wm_shell_action */
    uint32_t window_id;
};
