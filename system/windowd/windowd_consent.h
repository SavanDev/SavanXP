#pragma once

#include "libc.h"
#include "sxgfx/gfx2d.h"
#include "elevation/elevation_protocol.h"

/*
 * The elevation prompt (docs/kernel/USERS_AND_PERMISSIONS.md#elevation).
 *
 * The broker connects to SAVANXP_CONSENT_PORT_NAME, and only a connection
 * from the system is heard. windowd draws the prompt on its own surface,
 * above every window (fullscreen included), and while it is up the keyboard
 * and the pointer reach nothing else: no window, no hotkey, no WM key. The
 * answer comes only from the events windowd reads from /dev/input0 and
 * /dev/mouse0, which it claims, so no other process can read or answer for
 * the user. Nothing a client sends can close it.
 *
 * Input that was already queued when the prompt opened is not an answer:
 * nothing counts until WINDOWD_CONSENT_ARM_MS after it appears, so a key the
 * user was typing into a window cannot say yes. The focused button starts on
 * No, so Enter on its own does not either.
 */

/* How long after the prompt appears before input counts. */
#define WINDOWD_CONSENT_ARM_MS 500ul
/* How long the broker gets to write its request once accepted. */
#define WINDOWD_CONSENT_REQUEST_MS 2000ul

enum windowd_consent_button
{
    WINDOWD_CONSENT_NO = 0,
    WINDOWD_CONSENT_YES = 1,
};

struct windowd_consent
{
    int port_fd;  /* SAVANXP_CONSENT_PORT_NAME, or -1 */
    int read_fd;  /* the broker's connection, or -1 */
    int write_fd;
    unsigned long deadline_ms; /* to read the request */
    int open;     /* the prompt is on screen */
    unsigned long armed_at_ms;
    int focused;  /* enum windowd_consent_button */
    int pressed;  /* button under a press of the left button, or -1 */
    /* The last answer given: 1 yes, 0 no, -1 none yet. For the selftest. */
    int last_answer;
    struct savanxp_consent_request request;
};

void windowd_consent_init(struct windowd_consent *consent);
/* Creates the port. A negative -errno if it is taken. */
int windowd_consent_open_port(struct windowd_consent *consent);
/* Withdraws the prompt and closes everything. */
void windowd_consent_close(struct windowd_consent *consent);

/* What the main loop polls for: the port, and the broker's pipe. Returns how
 * many it filled (at most 2). */
int windowd_consent_poll_fds(const struct windowd_consent *consent, struct savanxp_pollfd *fds);

/* Accepts a connection, reads its request, notices the broker hanging up.
 * 1 when the prompt opened or closed: the screen has to be repainted. */
int windowd_consent_service(struct windowd_consent *consent, unsigned long now_ms);

/* While the prompt is up, every event is its own: 1 if it took the event
 * (always, while it is open), 0 if the prompt is not up. *changed is set when
 * the prompt has to be repainted (focus moved, or it closed). */
int windowd_consent_key(
    struct windowd_consent *consent, const struct savanxp_input_event *event, unsigned long now_ms, int *changed);
int windowd_consent_pointer(
    struct windowd_consent *consent,
    const struct savanxp_fb_info *info,
    int x,
    int y,
    uint32_t buttons,
    uint32_t last_buttons,
    unsigned long now_ms,
    int *changed);

/* The input rules, without a session or a broker: 0 if they hold. */
int windowd_consent_selftest(void);

/* Where the prompt is, centred on the screen. */
struct sx_rect windowd_consent_rect(const struct savanxp_fb_info *info);
void windowd_consent_draw(struct sx_painter *painter, const struct windowd_consent *consent, const struct savanxp_fb_info *info);
