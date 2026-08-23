#pragma once

#include "savanxp/syscall.h"

/*
 * Protocolo WM <-> cliente (A3, ver docs/WM_SUBSYSTEM.md).
 *
 * Contrato entre el window manager (windowd, hoy subsystems/posix/userland/
 * desktop.c) y cada proceso cliente que tiene una ventana. El WM lo establece
 * al lanzar el cliente: hace fork y remapea con dup2 los extremos de sus pipes
 * y secciones sobre los descriptores fijos de abajo, antes del exec. El cliente
 * los encuentra ya abiertos al arrancar; no los abre ni los negocia.
 *
 * Este header existe porque el contrato estaba implicito y ya habia empezado a
 * divergir: la mitad-servidor usaba numeros crudos en dup2, la mitad-cliente
 * posix otros numeros crudos en dup/map_view, y el SDK nativo mantenia su
 * propia copia de constantes (SXN_GUI_FD_*) sincronizada a mano leyendo
 * desktop.c -- copia que ya quedo corta, sin el fd de cursor hint. Las tres
 * partes deben referirse a estos nombres.
 *
 * Las estructuras que viajan por estos canales (savanxp_gpu_client_surface_
 * header, savanxp_gpu_dirty_rect_batch, savanxp_input_event,
 * savanxp_gui_pointer_event, savanxp_desktop_launch_request,
 * savanxp_desktop_cursor_hint, savanxp_desktop_size_hint) estan en syscall.h.
 *
 * NOTA sobre el subsistema nativo: por diseno no comparte los headers del SDK
 * posix (ver docs/SYSTEM_LAYERING.md), asi que mantiene su espejo en
 * savanxp_native_gui.h. Este header es la fuente canonica: cualquier cambio
 * acá tiene que replicarse allá, y los valores deben coincidir.
 */

/* Seccion compartida de la superficie: header + anillo de dirty-rect batches +
 * pixeles. El cliente la mapea RW. El WM la crea y la dimensiona; el cliente
 * NO elige su tamano (se entera por el header, y por eventos RESIZED); puede
 * sugerirlo al arrancar por SAVANXP_WM_FD_SIZE_HINT, pero decide el WM. */
#define SAVANXP_WM_FD_SECTION 3

/* Teclado: el WM escribe savanxp_input_event al cliente con foco. Tambien
 * llegan por aca los eventos RESIZED sinteticos. Solo-lectura para el cliente. */
#define SAVANXP_WM_FD_INPUT 4

/* Puntero: el WM escribe savanxp_gui_pointer_event en coordenadas LOCALES a la
 * superficie del cliente, no de pantalla. Solo-lectura para el cliente.
 *
 * Un cliente que no drene este canal no bloquea al WM: los extremos de
 * escritura son no-bloqueantes y el WM descarta el evento si el pipe esta
 * lleno. Los eventos llevan posicion absoluta, asi que perder uno es
 * inofensivo. Vale para cualquier canal WM->cliente que se agregue. */
#define SAVANXP_WM_FD_MOUSE 5

/* Evento submit: el CLIENTE lo señaliza tras publicar un batch de frame, para
 * despertar al WM sin esperar su timeout. */
#define SAVANXP_WM_FD_SUBMIT_EVENT 6

/* Evento retire: el WM lo señaliza cuando los frames del cliente ya se
 * presentaron y sus slots de batch quedan libres. */
#define SAVANXP_WM_FD_RETIRE_EVENT 7

/* Evento shutdown: el WM lo señaliza para pedirle al cliente que termine. */
#define SAVANXP_WM_FD_SHUTDOWN_EVENT 8

/* Launch: el cliente escribe savanxp_desktop_launch_request para pedirle al WM
 * que lance otro programa. El que pide declara los flags de lanzamiento
 * (SAVANXP_DESKTOP_LAUNCH_FLAG_*): el WM no conoce ningun catalogo de apps. */
#define SAVANXP_WM_FD_LAUNCH 9

/* Cursor hint: el cliente escribe savanxp_desktop_cursor_hint para pedir una
 * forma de cursor sobre su ventana. El WM decide si la muestra. */
#define SAVANXP_WM_FD_CURSOR_HINT 10

/* Size hint: el cliente escribe savanxp_desktop_size_hint para pedir el tamano
 * de area util que necesita su contenido. El WM sigue siendo el que dimensiona
 * la superficie -- recorta el pedido y lo ignora si la ventana ya salio de su
 * geometria de lanzamiento --, pero el tamano natural de una ventana lo sabe
 * el programa, que es quien conoce su layout. */
#define SAVANXP_WM_FD_SIZE_HINT 11

/* Rango reservado: descriptores 0..2 son stdio y 3..11 este protocolo. Al
 * preparar el hijo, el WM no debe cerrar por numero un fd de origen que caiga
 * dentro del rango -- puede ser el destino recien mapeado de otro dup2. */
#define SAVANXP_WM_FD_FIRST SAVANXP_WM_FD_SECTION
#define SAVANXP_WM_FD_LAST SAVANXP_WM_FD_SIZE_HINT
