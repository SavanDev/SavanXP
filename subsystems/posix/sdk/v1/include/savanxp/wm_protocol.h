#pragma once

#include "savanxp/syscall.h"

/*
 * WM <-> client protocol, version 5 (docs/desktop/DESKTOP_ARCHITECTURE.md,
 * docs/desktop/WM_SUBSYSTEM.md#descriptor-budget).
 *
 * A process gets a window by connecting to the WM's port, not by inheriting
 * channels from it: any process can connect, whoever started it (the desktop,
 * Program Manager, a shell, another program).
 *
 *   1. sxn_port_connect(SAVANXP_WM_PORT_NAME) gives the client two pipes: one
 *      to write to the WM (the request pipe) and one to read from it (the
 *      event pipe).
 *   2. The client writes a struct savanxp_wm_hello on the request pipe: the
 *      launch flags its own .sxe declares (a low-resolution fullscreen
 *      program sizes its surface differently) and whether it wants its input
 *      on a ring.
 *   3. The WM answers on the event pipe with a SAVANXP_WM_EVENT_WELCOME record
 *      (savanxp_wm_welcome). With status 0, the handles are already queued on
 *      that pipe, in this order: the surface section, the wake event, the
 *      session's submit event, then the input ring (if asked) or the window
 *      list section (for the shell, savanxp/wm_shell_protocol.h).
 *
 * From then on everything is as in v4:
 *
 *   - the surface section holds the header, the dirty-rect batches, the
 *     pixels, and the client's requests to the WM (savanxp_wm_client_requests:
 *     cursor shape, size hint, owned windows, presentation);
 *   - the event pipe carries savanxp_wm_event records -- keyboard of the focused
 *     window and pointer in coordinates local to the window `window_id` names
 *     -- and, in its handle queue, the sections of owned windows;
 *   - the wake event is set when composed_sequence or retired_sequence
 *     advances and when the WM sets the shutdown flag in the header;
 *   - the submit event, one for the whole session, is set by the client after
 *     it publishes a frame or a request.
 *
 * What it costs the WM: two descriptors per process (the write end of the
 * event pipe and the wake event); the section is closed once mapped and the
 * request pipe once the hello is read, except the shell's, which carries its
 * requests. Every channel is paid per window against the 64-handle table of
 * windowd, which is why everything else travels in the header.
 *
 * A v4 client -- one that expects channels on fixed descriptors -- finds none
 * and fails in gfx_open. External programs built against the SDK, such as
 * doomgeneric, must be rebuilt.
 *
 * The structs are shared with the native subsystem (they are the contract on
 * the wire, in include/abi/savanxp_abi.h); include/sxn/sxnwm.h is the native
 * client side.
 */

#define SAVANXP_WM_PORT_NAME "windowd"

/* How long a client waits for the welcome before giving up. */
#define SAVANXP_WM_CONNECT_TIMEOUT_MS 5000
