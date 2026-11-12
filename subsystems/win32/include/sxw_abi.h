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

#define SXW_ABI_VERSION 1u

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
#define SXW_PEB_SIZE 0x1000u

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

#define SXW_FAILED 0xffffffffu

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
