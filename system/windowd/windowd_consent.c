#include "windowd_sys.h"

#include "libc.h"
#include <stdio.h>
#include <string.h>
#include "sxgfx/sxchrome.h"
#include "sxn/sxn.h"
#include "windowd_consent.h"

/*
 * The elevation prompt: the consent port, the prompt's input and its
 * drawing. See windowd_consent.h for the rules it keeps.
 */

#define CONSENT_WIDTH 420
#define CONSENT_HEIGHT 160
#define CONSENT_CAPTION_HEIGHT 20
#define CONSENT_PADDING 14
#define CONSENT_BUTTON_WIDTH 76
#define CONSENT_BUTTON_HEIGHT 24
#define CONSENT_BUTTON_GAP 10

static void close_fd(int *fd)
{
    if (*fd >= 0)
    {
        (void)savanxp_close(*fd);
        *fd = -1;
    }
}

static void drop_connection(struct windowd_consent *consent)
{
    close_fd(&consent->read_fd);
    close_fd(&consent->write_fd);
    consent->open = 0;
    consent->pressed = -1;
}

/* Answers and hangs up. With no connection (the selftest) it only records
 * the answer. */
static void send_answer(struct windowd_consent *consent, int32_t answer)
{
    if (consent->write_fd >= 0)
    {
        struct savanxp_consent_reply reply;

        reply.magic = SAVANXP_CONSENT_MAGIC;
        reply.answer = answer;
        (void)savanxp_write(consent->write_fd, &reply, sizeof(reply));
    }
    if (answer == 0 || answer == 1)
    {
        consent->last_answer = answer;
    }
}

static void answer_prompt(struct windowd_consent *consent, int yes)
{
    send_answer(consent, yes ? 1 : 0);
    eprintf("windowd: consent prompt answered %s\n", yes ? "yes" : "no");
    drop_connection(consent);
}

void windowd_consent_init(struct windowd_consent *consent)
{
    memset(consent, 0, sizeof(*consent));
    consent->port_fd = -1;
    consent->read_fd = -1;
    consent->write_fd = -1;
    consent->pressed = -1;
    consent->last_answer = -1;
}

int windowd_consent_open_port(struct windowd_consent *consent)
{
    long port = sxn_port_create(SAVANXP_CONSENT_PORT_NAME);

    if (port < 0)
    {
        return (int)port;
    }
    consent->port_fd = (int)port;
    return 0;
}

void windowd_consent_close(struct windowd_consent *consent)
{
    drop_connection(consent);
    close_fd(&consent->port_fd);
}

int windowd_consent_poll_fds(const struct windowd_consent *consent, struct savanxp_pollfd *fds)
{
    int count = 0;

    /* One broker at a time: the next waits in the port's backlog, so the
     * port is only worth waking for when nobody is connected. */
    if (consent->port_fd >= 0 && consent->read_fd < 0)
    {
        fds[count].fd = consent->port_fd;
        fds[count].events = SAVANXP_POLLIN;
        fds[count].revents = 0;
        ++count;
    }
    if (consent->read_fd >= 0)
    {
        fds[count].fd = consent->read_fd;
        fds[count].events = SAVANXP_POLLIN | SAVANXP_POLLHUP;
        fds[count].revents = 0;
        ++count;
    }
    return count;
}

static int request_valid(const struct savanxp_consent_request *request)
{
    return request->magic == SAVANXP_CONSENT_MAGIC && request->version == SAVANXP_ELEVATION_VERSION &&
        memchr(request->path, '\0', sizeof(request->path)) != 0 &&
        memchr(request->requester_name, '\0', sizeof(request->requester_name)) != 0;
}

static void show(struct windowd_consent *consent, unsigned long now_ms)
{
    consent->open = 1;
    consent->armed_at_ms = now_ms + WINDOWD_CONSENT_ARM_MS;
    consent->focused = WINDOWD_CONSENT_NO;
    consent->pressed = -1;
}


int windowd_consent_service(struct windowd_consent *consent, unsigned long now_ms)
{
    if (consent->port_fd < 0)
    {
        return 0;
    }

    if (consent->read_fd < 0)
    {
        struct sxn_port_connection_ex accepted;

        memset(&accepted, 0, sizeof(accepted));
        if (sxn_port_accept_ex(consent->port_fd, &accepted) < 0)
        {
            return 0;
        }
        consent->read_fd = accepted.read_handle;
        consent->write_fd = accepted.write_handle;
        /* Only the system asks: the broker. Anybody else gets no prompt to
         * answer for itself. */
        if (accepted.client.euid != 0u)
        {
            eprintf("windowd: pid %u may not ask for consent\n", (unsigned)accepted.client_pid);
            send_answer(consent, -SAVANXP_EACCES);
            drop_connection(consent);
            return 0;
        }
        consent->deadline_ms = now_ms + WINDOWD_CONSENT_REQUEST_MS;
        (void)savanxp_fcntl(consent->read_fd, SAVANXP_F_SETFL, SAVANXP_OPEN_NONBLOCK);
    }

    if (!consent->open)
    {
        struct savanxp_consent_request request;
        long got = savanxp_read(consent->read_fd, &request, sizeof(request));

        if (got == -SAVANXP_EAGAIN && now_ms < consent->deadline_ms)
        {
            return 0;
        }
        if (got != (long)sizeof(request) || !request_valid(&request))
        {
            send_answer(consent, -SAVANXP_EINVAL);
            drop_connection(consent);
            return 0;
        }
        consent->request = request;
        show(consent, now_ms);
        eprintf("windowd: consent prompt shown for %s\n", consent->request.path);
        return 1;
    }

    /* Up: the broker may only hang up, which withdraws it. Anything else it
     * writes means nothing. */
    for (;;)
    {
        char discard[32];
        long got = savanxp_read(consent->read_fd, discard, sizeof(discard));

        if (got == -SAVANXP_EAGAIN)
        {
            return 0;
        }
        if (got <= 0)
        {
            eprintf("windowd: consent prompt withdrawn\n");
            drop_connection(consent);
            return 1;
        }
    }
}

static int is_key(const struct savanxp_input_event *event, uint32_t key, char lower)
{
    return event->key == key || (lower != 0 && (event->ascii == lower || event->ascii == lower - 'a' + 'A'));
}

int windowd_consent_key(
    struct windowd_consent *consent, const struct savanxp_input_event *event, unsigned long now_ms, int *changed)
{
    *changed = 0;
    if (!consent->open)
    {
        return 0;
    }
    /* Releases, and anything before it is armed, are swallowed. */
    if (event->type != SAVANXP_INPUT_EVENT_KEY_DOWN || now_ms < consent->armed_at_ms)
    {
        return 1;
    }
    if (is_key(event, 0, 'y'))
    {
        answer_prompt(consent, 1);
        *changed = 1;
    }
    else if (is_key(event, SAVANXP_KEY_ESC, 'n'))
    {
        answer_prompt(consent, 0);
        *changed = 1;
    }
    else if (event->key == SAVANXP_KEY_TAB || event->key == SAVANXP_KEY_LEFT || event->key == SAVANXP_KEY_RIGHT)
    {
        consent->focused = consent->focused == WINDOWD_CONSENT_YES ? WINDOWD_CONSENT_NO : WINDOWD_CONSENT_YES;
        *changed = 1;
    }
    else if (event->key == SAVANXP_KEY_ENTER || event->ascii == ' ')
    {
        answer_prompt(consent, consent->focused == WINDOWD_CONSENT_YES);
        *changed = 1;
    }
    return 1;
}

struct sx_rect windowd_consent_rect(const struct savanxp_fb_info *info)
{
    int width = CONSENT_WIDTH;
    int height = CONSENT_HEIGHT;

    if (info == 0)
    {
        return sx_rect_make(0, 0, 0, 0);
    }
    if (width > (int)info->width)
    {
        width = (int)info->width;
    }
    if (height > (int)info->height)
    {
        height = (int)info->height;
    }
    return sx_rect_make(((int)info->width - width) / 2, ((int)info->height - height) / 2, width, height);
}

/* The two buttons, right-aligned at the bottom: Yes, then No. */
static struct sx_rect button_rect(const struct savanxp_fb_info *info, int button)
{
    struct sx_rect box = windowd_consent_rect(info);
    int right = box.x + box.width - CONSENT_PADDING;
    int y = box.y + box.height - CONSENT_PADDING - CONSENT_BUTTON_HEIGHT;
    int no_x = right - CONSENT_BUTTON_WIDTH;

    if (button == WINDOWD_CONSENT_NO)
    {
        return sx_rect_make(no_x, y, CONSENT_BUTTON_WIDTH, CONSENT_BUTTON_HEIGHT);
    }
    return sx_rect_make(no_x - CONSENT_BUTTON_GAP - CONSENT_BUTTON_WIDTH, y, CONSENT_BUTTON_WIDTH, CONSENT_BUTTON_HEIGHT);
}

static int button_at(const struct savanxp_fb_info *info, int x, int y)
{
    int button;

    for (button = WINDOWD_CONSENT_NO; button <= WINDOWD_CONSENT_YES; ++button)
    {
        if (sx_rect_contains_point(button_rect(info, button), x, y))
        {
            return button;
        }
    }
    return -1;
}

int windowd_consent_pointer(
    struct windowd_consent *consent,
    const struct savanxp_fb_info *info,
    int x,
    int y,
    uint32_t buttons,
    uint32_t last_buttons,
    unsigned long now_ms,
    int *changed)
{
    uint32_t left = buttons & SAVANXP_MOUSE_BUTTON_LEFT;
    uint32_t left_was = last_buttons & SAVANXP_MOUSE_BUTTON_LEFT;

    *changed = 0;
    if (!consent->open)
    {
        return 0;
    }
    if (left != 0u && left_was == 0u)
    {
        /* A press before it is armed was aimed at something else. */
        consent->pressed = now_ms < consent->armed_at_ms ? -1 : button_at(info, x, y);
        if (consent->pressed >= 0)
        {
            consent->focused = consent->pressed;
            *changed = 1;
        }
    }
    else if (left == 0u && left_was != 0u)
    {
        int pressed = consent->pressed;

        consent->pressed = -1;
        if (pressed >= 0 && button_at(info, x, y) == pressed)
        {
            answer_prompt(consent, pressed == WINDOWD_CONSENT_YES);
        }
        *changed = 1;
    }
    return 1;
}

static void draw_text_clipped(struct sx_painter *painter, struct sx_rect clip, int x, int y, const char *text, uint32_t colour)
{
    if (sx_painter_push_clip(painter, clip))
    {
        sx_painter_draw_text(painter, x, y, text, colour);
        sx_painter_pop_clip(painter);
    }
}

static void draw_button(struct sx_painter *painter, struct sx_rect rect, const char *label, int focused, int pressed)
{
    int text_x = rect.x + (rect.width - gfx_text_width(label)) / 2 + (pressed ? 1 : 0);
    int text_y = rect.y + (rect.height - gfx_text_height()) / 2 + (pressed ? 1 : 0);

    if (focused)
    {
        /* The default button's black ring, as a dialog of the era draws it. */
        sx_painter_fill_rect(painter, rect, SXCHROME_COLOR_DARK);
        rect = sx_rect_make(rect.x + 1, rect.y + 1, rect.width - 2, rect.height - 2);
    }
    sxchrome_fill_raised(painter, rect, SXCHROME_COLOR_FACE, pressed);
    sx_painter_draw_text(painter, text_x, text_y, label, SXCHROME_COLOR_TEXT);
}

void windowd_consent_draw(struct sx_painter *painter, const struct windowd_consent *consent, const struct savanxp_fb_info *info)
{
    struct sx_rect box = windowd_consent_rect(info);
    struct sx_rect caption = sx_rect_make(box.x + 3, box.y + 3, box.width - 6, CONSENT_CAPTION_HEIGHT);
    struct sx_rect text_clip = sx_rect_make(box.x + CONSENT_PADDING, box.y, box.width - 2 * CONSENT_PADDING, box.height);
    int line = gfx_text_height() + 4;
    int x = box.x + CONSENT_PADDING;
    int y = caption.y + caption.height + CONSENT_PADDING;
    int column;
    char requester[SAVANXP_ELEVATION_NAME_CAPACITY + 32];
    int step;

    if (!consent->open)
    {
        return;
    }
    sxchrome_fill_raised(painter, box, SXCHROME_COLOR_FACE, 0);
    /* The active caption's navy-to-blue run, drawn in columns. */
    for (step = 0; step < caption.width; step += 4)
    {
        int amount = caption.width > 1 ? step * 255 / (caption.width - 1) : 0;
        uint32_t left = SXCHROME_COLOR_CAPTION_ACTIVE;
        uint32_t right = SXCHROME_COLOR_CAPTION_ACTIVE_GRADIENT;
        uint32_t r = (((left >> 16) & 0xffu) * (uint32_t)(255 - amount) + ((right >> 16) & 0xffu) * (uint32_t)amount) / 255u;
        uint32_t g = (((left >> 8) & 0xffu) * (uint32_t)(255 - amount) + ((right >> 8) & 0xffu) * (uint32_t)amount) / 255u;
        uint32_t b = ((left & 0xffu) * (uint32_t)(255 - amount) + (right & 0xffu) * (uint32_t)amount) / 255u;
        int width = caption.width - step < 4 ? caption.width - step : 4;

        sx_painter_fill_rect(painter, sx_rect_make(caption.x + step, caption.y, width, caption.height), SXCHROME_RGB(r, g, b));
    }
    draw_text_clipped(painter, caption, caption.x + 6, caption.y + (caption.height - gfx_text_height()) / 2,
        "Run with system rights", SXCHROME_COLOR_CAPTION_TEXT);

    draw_text_clipped(painter, text_clip, x, y, "Do you want to allow this program to make", SXCHROME_COLOR_TEXT);
    y += line;
    draw_text_clipped(painter, text_clip, x, y, "changes to the system?", SXCHROME_COLOR_TEXT);
    y += line + line / 2;

    column = x + gfx_text_width("Requested by:") + 10;
    draw_text_clipped(painter, text_clip, x, y, "Program:", SXCHROME_COLOR_TEXT);
    draw_text_clipped(painter, text_clip, column, y, consent->request.path, SXCHROME_COLOR_TEXT);
    y += line;
    snprintf(requester, sizeof(requester), "%s (pid %u)",
        consent->request.requester_name[0] != '\0' ? consent->request.requester_name : "unknown",
        (unsigned)consent->request.requester_pid);
    draw_text_clipped(painter, text_clip, x, y, "Requested by:", SXCHROME_COLOR_TEXT);
    draw_text_clipped(painter, text_clip, column, y, requester, SXCHROME_COLOR_TEXT);

    draw_button(painter, button_rect(info, WINDOWD_CONSENT_YES), "Yes",
        consent->focused == WINDOWD_CONSENT_YES, consent->pressed == WINDOWD_CONSENT_YES);
    draw_button(painter, button_rect(info, WINDOWD_CONSENT_NO), "No",
        consent->focused == WINDOWD_CONSENT_NO, consent->pressed == WINDOWD_CONSENT_NO);
}

/* --- Selftest ------------------------------------------------------------ */

static struct savanxp_input_event test_key(uint32_t key, int32_t ascii)
{
    struct savanxp_input_event event;

    memset(&event, 0, sizeof(event));
    event.type = SAVANXP_INPUT_EVENT_KEY_DOWN;
    event.key = key;
    event.ascii = ascii;
    return event;
}

int windowd_consent_selftest(void)
{
    struct windowd_consent consent;
    struct savanxp_fb_info info;
    struct savanxp_input_event event;
    struct sx_rect yes;
    int changed = 0;

    memset(&info, 0, sizeof(info));
    info.width = 1024;
    info.height = 768;
    yes = button_rect(&info, WINDOWD_CONSENT_YES);

    /* Closed, it takes nothing. */
    windowd_consent_init(&consent);
    event = test_key(0, 'y');
    if (windowd_consent_key(&consent, &event, 0, &changed) != 0)
    {
        return 1;
    }

    /* Typed ahead, before it is armed: swallowed, not an answer. */
    show(&consent, 1000);
    if (!windowd_consent_key(&consent, &event, 1000 + WINDOWD_CONSENT_ARM_MS - 1, &changed) ||
        !consent.open || consent.last_answer != -1)
    {
        return 1;
    }
    /* Enter alone is No: the focus starts there. */
    event = test_key(SAVANXP_KEY_ENTER, '\r');
    (void)windowd_consent_key(&consent, &event, 1000 + WINDOWD_CONSENT_ARM_MS, &changed);
    if (consent.open || consent.last_answer != 0)
    {
        return 1;
    }

    /* Tab moves to Yes, Enter takes it. */
    show(&consent, 1000);
    event = test_key(SAVANXP_KEY_TAB, '\t');
    (void)windowd_consent_key(&consent, &event, 2000, &changed);
    event = test_key(SAVANXP_KEY_ENTER, '\r');
    (void)windowd_consent_key(&consent, &event, 2000, &changed);
    if (consent.open || consent.last_answer != 1)
    {
        return 1;
    }

    /* Esc is No, a key release is nothing. */
    show(&consent, 1000);
    event = test_key(0, 'y');
    event.type = SAVANXP_INPUT_EVENT_KEY_UP;
    (void)windowd_consent_key(&consent, &event, 2000, &changed);
    if (!consent.open)
    {
        return 1;
    }
    event = test_key(SAVANXP_KEY_ESC, 27);
    (void)windowd_consent_key(&consent, &event, 2000, &changed);
    if (consent.open || consent.last_answer != 0)
    {
        return 1;
    }

    /* A click on Yes counts only when the press came after arming, and the
     * release lands on the same button. */
    show(&consent, 1000);
    consent.last_answer = -1;
    (void)windowd_consent_pointer(&consent, &info, yes.x + 4, yes.y + 4, SAVANXP_MOUSE_BUTTON_LEFT, 0, 1100, &changed);
    (void)windowd_consent_pointer(&consent, &info, yes.x + 4, yes.y + 4, 0, SAVANXP_MOUSE_BUTTON_LEFT, 2000, &changed);
    if (!consent.open || consent.last_answer != -1)
    {
        return 1;
    }
    (void)windowd_consent_pointer(&consent, &info, yes.x + 4, yes.y + 4, SAVANXP_MOUSE_BUTTON_LEFT, 0, 2000, &changed);
    (void)windowd_consent_pointer(&consent, &info, 0, 0, 0, SAVANXP_MOUSE_BUTTON_LEFT, 2000, &changed);
    if (!consent.open || consent.last_answer != -1)
    {
        return 1;
    }
    (void)windowd_consent_pointer(&consent, &info, yes.x + 4, yes.y + 4, SAVANXP_MOUSE_BUTTON_LEFT, 0, 2000, &changed);
    (void)windowd_consent_pointer(&consent, &info, yes.x + 6, yes.y + 6, 0, SAVANXP_MOUSE_BUTTON_LEFT, 2000, &changed);
    return consent.open || consent.last_answer != 1;
}
