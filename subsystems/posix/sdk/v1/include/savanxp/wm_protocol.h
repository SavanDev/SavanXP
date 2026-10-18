#pragma once

#include "savanxp/syscall.h"

/*
 * Protocolo WM <-> cliente, version 4 (ver docs/desktop/WM_SUBSYSTEM.md).
 *
 * Contrato entre el window manager (windowd, subsystems/posix/userland/
 * windowd.c) y cada proceso cliente que tiene una ventana. El WM lo establece
 * al lanzar el cliente: hace fork, remapea con dup2 sus canales sobre los
 * descriptores fijos de abajo y cierra todo lo demas antes del exec. El
 * cliente los encuentra ya abiertos al arrancar; no los abre ni los negocia.
 *
 * Por que tiene esta forma: el WM paga cada canal con un descriptor POR
 * VENTANA contra el limite de 64 por proceso (process::kMaxFileHandles), y
 * cada pipe contra los 64 pipes del sistema entero. La v3 le costaba nueve
 * descriptores y cinco pipes a cada ventana y no llegaba ni a la mitad de las
 * WINDOWD_MAX_OVERLAY_CLIENTS que declara. La v4 le cuesta DOS descriptores
 * (el pipe de eventos y el evento de wake) y un pipe: la seccion se cierra en
 * el WM despues del fork, el evento de submit es uno solo para toda la sesion,
 * y lo que iba por pipes cliente -> WM viaja en el header de la superficie.
 *
 * Un cliente de la v3 falla en gfx_open (la version del header no coincide)
 * en vez de hablar un protocolo que el WM ya no entiende.
 *
 * Las estructuras que viajan por estos canales (savanxp_gpu_client_surface_
 * header, savanxp_wm_client_requests, savanxp_gpu_dirty_rect_batch,
 * savanxp_wm_event, savanxp_desktop_launch_request) estan en syscall.h.
 *
 * Una conexion, varias ventanas: estos canales son del PROCESO, no de una
 * ventana. Ademas de la principal, el cliente puede pedir ventanas con dueno
 * (dialogos) por el header de la principal (savanxp_wm_client_requests.window);
 * el WM crea su seccion y se la entrega como handle por la cola de handles del
 * pipe de eventos, y sus eventos viajan por ese mismo pipe con su window_id. Ni
 * el WM ni el cliente abren un canal nuevo por ventana. Ver
 * docs/desktop/OWNED_WINDOWS.md.
 *
 * NOTA sobre el subsistema nativo: comparte estas structs (son el contrato
 * en el alambre y no se duplican); lo que difiere es el transporte. Un
 * cliente nativo no puede leer el pipe de eventos, asi que hoy es
 * solo-salida: mapea la seccion, publica batches y senala eventos con
 * syscalls SXN (ver sdk/nativedemo/main.c). La entrada por anillo sobre
 * memoria compartida es trabajo futuro (docs/kernel/KERNEL_NATIVE_API.md).
 */

/* Seccion compartida de la superficie: header + anillo de dirty-rect batches +
 * pixeles. El cliente la mapea RW. El WM la crea y la dimensiona; el cliente
 * NO elige su tamano (se entera por el header). En el header tambien van sus
 * pedidos al WM (savanxp_wm_client_requests) y el pedido de cierre del WM
 * (SAVANXP_GPU_CLIENT_SURFACE_FLAG_SHUTDOWN). */
#define SAVANXP_WM_FD_SECTION 3

/* Eventos: el WM escribe registros savanxp_wm_event -- teclado del cliente con
 * foco y puntero en coordenadas LOCALES a la superficie de la ventana que dice
 * window_id. Solo-lectura para el cliente, que lee de a registros enteros. Su
 * cola de handles trae las secciones de las ventanas con dueno. El resize no viaja aca: el cliente
 * lo sintetiza al ver cambiar el ancho o el alto del header.
 *
 * Un cliente que no drene este canal no bloquea al WM: el extremo de escritura
 * es no-bloqueante y el WM descarta el evento si el pipe esta lleno. Como
 * teclado y puntero comparten el pipe, el runtime drena los dos tipos aunque la
 * app solo pida uno, y guarda el otro para despues. */
#define SAVANXP_WM_FD_EVENTS 4

/* Evento wake (manual reset): el WM lo senala cuando avanza composed_sequence
 * o retired_sequence, y cuando prende el pedido de cierre en el header. Quien
 * espera tiene que resetearlo, volver a mirar el header y recien ahi esperar:
 * es un solo evento para varias condiciones. */
#define SAVANXP_WM_FD_WAKE_EVENT 5

/* Evento submit (manual reset): el CLIENTE lo senala despues de publicar un
 * batch de frame o un pedido en el header, para despertar al WM sin esperar
 * su timeout. Es el MISMO objeto para todos los clientes de la sesion: el WM
 * lo resetea antes de revisarlos a todos, asi que un cliente no puede hacerle
 * perder un submit a otro por mas de un timeout del loop. */
#define SAVANXP_WM_FD_SUBMIT_EVENT 6

/* Anillo de entrada (solo clientes nativos): seccion con un struct
 * savanxp_wm_input_ring que el WM mapea RW. El WM la crea en el launch si el
 * binario es nativo (EI_OSABI) y la duplica al fd 7; el pipe de eventos
 * sigue existiendo pero el cliente nativo lo ignora. El wake (fd 5) tambien
 * se senala con entrada pendiente: quien espera resetea, mira el header Y el
 * anillo, y recien ahi espera. Lleno se descarta igual que el pipe lleno. */
#define SAVANXP_WM_FD_INPUT_RING 7

/* Rango reservado: descriptores 0..2 son stdio, 3..6 este protocolo y 7 el
 * anillo de entrada de un cliente nativo (ausente en los POSIX). */
#define SAVANXP_WM_FD_FIRST SAVANXP_WM_FD_SECTION
#define SAVANXP_WM_FD_LAST SAVANXP_WM_FD_SUBMIT_EVENT
