#pragma once

/*
 * windowd runs on the native API (docs/desktop/DESKTOP_ARCHITECTURE.md, phase
 * 3 step 6). Its sources were written against the POSIX SDK; this header,
 * included first by every windowd unit, renames the handful of POSIX calls
 * they make to wd_* functions with the same signatures, implemented on SXN in
 * windowd_sys.c. The libc.h declarations that follow are renamed with them, so
 * the prototypes stay checked. Nothing here is POSIX at run time.
 *
 * What does not map one to one:
 *   - text output (printf, eprintf, puts_fd) goes to /dev/serial;
 *   - savanxp_poll is wait-many plus a readiness check per handle;
 *   - savanxp_open_mode opens a device by its bare name ("/dev/input0");
 *   - savanxp_kill ends a child (SXN_SYS_TERMINATE): windowd only ends its
 *     own children (compositord, its selftest's programs);
 *   - spawn gives the child stdio only, as the POSIX spawn did;
 *   - main is windowd_main, which windowd_sys.c ends with sxn_exit.
 */

#define printf wd_printf
#define eprintf wd_eprintf
#define puts_fd wd_puts_fd
#define uptime_ms wd_uptime_ms
#define monotonic_ns wd_monotonic_ns
#define sleep_ms wd_sleep_ms
#define savanxp_getpid wd_getpid
#define event_create wd_event_create
#define event_set wd_event_set
#define event_reset wd_event_reset
#define wait_one wd_wait_one
#define pipe_send_handle wd_pipe_send_handle
#define savanxp_read wd_read
#define savanxp_write wd_write
#define savanxp_kill wd_kill
#define savanxp_dup wd_dup
#define section_create wd_section_create
#define map_view wd_map_view
#define unmap_view wd_unmap_view
#define savanxp_waitpid wd_waitpid
#define savanxp_fcntl wd_fcntl
#define savanxp_poll wd_poll
#define savanxp_open_mode wd_open_mode
#define spawn wd_spawn
#define savanxp_close wd_close
#define main windowd_main
