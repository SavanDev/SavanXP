/*
 * sxw_abi.h -- The private contract between winhost (or the host harness)
 * and the subsystem's own 32-bit DLLs.
 *
 * It is not a public ABI and not the NT system call table: winhost and the
 * DLLs are built and installed together, and ntdll refuses a host whose
 * SXW_ABI_VERSION differs. Every field is a fixed-width integer or a guest
 * address below 4 GiB. See docs/userland/WIN32.md, "The gate".
 */
#ifndef SAVANXP_WIN32_SXW_ABI_H
#define SAVANXP_WIN32_SXW_ABI_H

#define SXW_ABI_VERSION 3u

/* ---- Thread environment block (32-bit layout, the fields we fill) ------- */

#define SXW_TEB_EXCEPTION_LIST 0x00u /* 0xffffffff: no handler frame */
#define SXW_TEB_STACK_BASE 0x04u
#define SXW_TEB_STACK_LIMIT 0x08u
#define SXW_TEB_SELF 0x18u
#define SXW_TEB_PROCESS_ID 0x20u
#define SXW_TEB_THREAD_ID 0x24u
#define SXW_TEB_TLS_POINTER 0x2cu /* ThreadLocalStoragePointer: array of TLS blocks */
#define SXW_TEB_PEB 0x30u
#define SXW_TEB_LAST_ERROR 0x34u
/* WOW32Reserved in Windows' own TEB: the 32 -> 64 transition. Here it is the
 * guest-callable address of the service gate. */
#define SXW_TEB_GATE 0xc0u
#define SXW_TEB_SIZE 0x1000u

/* ---- Process environment block ------------------------------------------ */

#define SXW_PEB_IMAGE_BASE 0x08u
#define SXW_PEB_PROCESS_PARAMETERS 0x10u
#define SXW_PEB_SIZE 0x1000u

/* RTL_USER_PROCESS_PARAMETERS, 32-bit layout, the fields we fill. The host
 * puts it in the PEB's page at SXW_PEB_PARAMETERS_OFFSET, its strings after
 * it. A UNICODE_STRING is {uint16 Length, uint16 MaximumLength, uint32
 * Buffer}, lengths in bytes without the terminator. */
#define SXW_PEB_PARAMETERS_OFFSET 0x400u
#define SXW_PARAMS_STD_INPUT 0x18u
#define SXW_PARAMS_STD_OUTPUT 0x1cu
#define SXW_PARAMS_STD_ERROR 0x20u
#define SXW_PARAMS_IMAGE_PATH 0x38u
#define SXW_PARAMS_COMMAND_LINE 0x40u
#define SXW_PARAMS_SIZE 0x90u

/* ---- Services ------------------------------------------------------------ */
/*
 * The guest calls the gate as
 *     uint32_t __stdcall gate(uint32_t service, const uint32_t *arguments);
 * The arguments are a flat array of 32-bit values in guest memory. The host
 * checks every guest address before using it and never calls guest code.
 */

#define SXW_SVC_VERSION 0u       /* () -> SXW_ABI_VERSION */
#define SXW_SVC_EXIT_PROCESS 1u  /* (status) -> does not return */
#define SXW_SVC_STD_HANDLE 2u    /* (which: 0 in, 1 out, 2 err) -> handle or 0 */
#define SXW_SVC_WRITE 3u         /* (handle, buffer, length) -> bytes written, or SXW_FAILED */
#define SXW_SVC_DEBUG_OUTPUT 4u  /* (buffer, length) -> 0 */
/* () -> seconds since 1970 UTC, as a 32-bit time_t; a test run may fix the
 * start time (winhost --time). */
#define SXW_SVC_TIME 5u
/* (size) -> address of a new zeroed read-write region of at least size
 * bytes, or 0. The heap's arenas. */
#define SXW_SVC_ALLOCATE 6u
/* (name) -> base of the loaded module with that file name (an ANSI string,
 * any case, a path allowed), or 0. */
#define SXW_SVC_FIND_MODULE 7u
/* (name, modules, capacity, error) -> base, or 0 with a Win32 error code in
 * *error. Loads the DLL and what it needs; the modules loaded by this call
 * are written to modules[capacity] (sxw_start_module, initialization order)
 * and their count is the low half of *error on success. */
#define SXW_SVC_LOAD_LIBRARY 8u
/* (base, name or ordinal, modules, capacity, error) -> address, or 0 with a
 * Win32 error in *error. A name below 0x10000 is an ordinal. A forwarder may
 * load DLLs, reported as SXW_SVC_LOAD_LIBRARY reports them. */
#define SXW_SVC_GET_PROC 9u
/* Registry. Keys are handles from the host; the predefined ones are
 * Windows' (SXW_HKEY_*). Strings are UTF-16. Each returns a Win32 error. */
#define SXW_SVC_REG_OPEN 10u  /* (hkey, subkey, create, out hkey, out disposition) */
#define SXW_SVC_REG_CLOSE 11u /* (hkey) */
#define SXW_SVC_REG_QUERY 12u /* (hkey, name, out type, data, in/out size) */
#define SXW_SVC_REG_SET 13u   /* (hkey, name, type, data, size) */

/* Windows and input (W3). A window here is a top-level window of the host
 * (a windowd window on SavanXP, an offscreen surface in the harness); its
 * frame and caption are the host's. The guest owns the pixels: user32 keeps
 * one buffer per window, 0x00RRGGBB per pixel, and presents rectangles of it.
 * Child windows, the message queue and the rest of the window manager are
 * user32's. */
/* () -> milliseconds since the process started; GetTickCount and the
 * timers. A test run may drive it (the harness's input script). */
#define SXW_SVC_TICKS 14u
/* (out size[2]) -> 0: the screen's width and height in pixels. */
#define SXW_SVC_DISPLAY 15u
/* (title, width, height, out size[2]) -> a window id, or 0. Asks for a
 * client area of width x height; out size is the one the host gave, which a
 * window manager may clip. The title is UTF-16. */
#define SXW_SVC_WINDOW_CREATE 16u
/* (window) -> 0 */
#define SXW_SVC_WINDOW_DESTROY 17u
/* (window, pixels, stride, size[2], rects, count) -> 0, or SXW_FAILED.
 * Copies count RECTs (left, top, right, bottom) of the guest's buffer
 * (stride bytes per row, size[0] x size[1]) to the window and shows them. */
#define SXW_SVC_WINDOW_PRESENT 18u
/* (window, title) -> 0, or SXW_FAILED */
#define SXW_SVC_WINDOW_TITLE 19u
/* (timeout, out event) -> 1 with an event, 0 when the timeout passed.
 * timeout is in milliseconds, SXW_INFINITE to wait for an event, 0 to poll.
 * out points at an sxw_event. */
#define SXW_SVC_WAIT_EVENT 20u
#define SXW_SVC_COUNT 21u

#define SXW_INFINITE 0xffffffffu

/* Events from the host, in client coordinates of the window they name. */
#define SXW_EVENT_POINTER 1u /* a = x, b = y, c = buttons (SXW_BUTTON_*) */
#define SXW_EVENT_KEY 2u     /* a = virtual key, b = 1 down / 0 up, c = UTF-16 character or 0 */
#define SXW_EVENT_RESIZE 3u  /* a = client width, b = client height */
#define SXW_EVENT_CLOSE 4u   /* the frame's close button, or its shortcut */

#define SXW_BUTTON_LEFT 1u
#define SXW_BUTTON_RIGHT 2u
#define SXW_BUTTON_MIDDLE 4u

typedef struct sxw_event {
    unsigned int kind;
    unsigned int window;
    unsigned int a;
    unsigned int b;
    unsigned int c;
    unsigned int time; /* SXW_SVC_TICKS when it happened */
} sxw_event;

#define SXW_FAILED 0xffffffffu

#define SXW_HKEY_CLASSES_ROOT 0x80000000u
#define SXW_HKEY_CURRENT_USER 0x80000001u
#define SXW_HKEY_LOCAL_MACHINE 0x80000002u

/* Win32 error codes the services return. */
#define SXW_ERROR_SUCCESS 0u
#define SXW_ERROR_FILE_NOT_FOUND 2u
#define SXW_ERROR_ACCESS_DENIED 5u
#define SXW_ERROR_INVALID_HANDLE 6u
#define SXW_ERROR_NOT_ENOUGH_MEMORY 8u
#define SXW_ERROR_INVALID_PARAMETER 87u
#define SXW_ERROR_MOD_NOT_FOUND 126u
#define SXW_ERROR_PROC_NOT_FOUND 127u
#define SXW_ERROR_INVALID_ORDINAL 182u
#define SXW_ERROR_BAD_EXE_FORMAT 193u
#define SXW_ERROR_MORE_DATA 234u
#define SXW_ERROR_DLL_INIT_FAILED 1114u
#define SXW_ERROR_REGISTRY_IO_FAILED 1016u

/* ---- Process start ------------------------------------------------------- */
/*
 * The host loads every image, binds the imports and prepares TLS, then
 * enters the guest once, at ntdll's exported SxwProcessStart, with the
 * address of a start block. ntdll runs each module's TLS callbacks and
 * DllMain in the block's order (dependencies first), then the executable's
 * TLS callbacks and its entry point, and exits with the entry's result.
 *
 * Exit goes through ntdll's RtlExitUserProcess, which detaches in the order
 * Windows XP does: each DLL in reverse initialization order (its TLS
 * callbacks, then DllMain), then the executable's TLS callbacks.
 */

#define SXW_START_DLL 1u /* the module is a DLL: call DllMain */

typedef struct sxw_start_module {
    unsigned int base;          /* the module's load address */
    unsigned int entry;         /* entry point VA, or 0 */
    unsigned int tls_callbacks; /* VA of a NUL-terminated array of VAs, or 0 */
    unsigned int flags;         /* SXW_START_DLL */
} sxw_start_module;

typedef struct sxw_start_block {
    unsigned int version;       /* SXW_ABI_VERSION */
    unsigned int count;         /* modules, the executable last */
    sxw_start_module modules[1];
} sxw_start_block;

/* DllMain reasons, as Windows defines them. */
#define SXW_DLL_PROCESS_DETACH 0u
#define SXW_DLL_PROCESS_ATTACH 1u

/* NT status values a process ends with when it cannot start, the same ones
 * Windows XP reports for the same failure. */
#define SXW_STATUS_NO_MEMORY 0xc0000017u
#define SXW_STATUS_INVALID_IMAGE_FORMAT 0xc000007bu
#define SXW_STATUS_DLL_NOT_FOUND 0xc0000135u
#define SXW_STATUS_ORDINAL_NOT_FOUND 0xc0000138u
#define SXW_STATUS_ENTRYPOINT_NOT_FOUND 0xc0000139u
#define SXW_STATUS_DLL_INIT_FAILED 0xc0000142u

#endif
