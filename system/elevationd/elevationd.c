#include <stdint.h>
#include <string.h>

#include "elevation/elevation_protocol.h"
#include "sxn/sxn.h"

/*
 * elevationd: the elevation broker (docs/kernel/USERS_AND_PERMISSIONS.md#
 * elevation). NT's Application Information service in shape: a program asks
 * it to run something with the system's rights, it asks windowd to show the
 * prompt, and on yes it starts the program itself with an elevated token. The
 * program that asked never gets the token.
 *
 * Who asked comes from the connection (SXN_SYS_PORT_ACCEPT_EX), never from
 * the request. The answer comes only from windowd's consent port, and only if
 * the system created it: port names are first come, first served, so a
 * program that took the name first must not be able to answer yes.
 *
 * One request at a time, as there is one prompt at a time. The programs it
 * starts are its children: it reaps them, and reports their exit to a
 * requester that asked to wait.
 *
 * Native: it needs ports, pipes, spawn and waitpid, nothing of POSIX. init
 * starts it before the session, so its name is taken before anybody else can
 * take it.
 */

/* How long a requester gets to write its request once connected. */
#define ELEVATIOND_REQUEST_MS 2000l
/* How long the prompt may stay unanswered before it counts as no. */
#define ELEVATIOND_CONSENT_MS (10l * 60l * 1000l)
/* How often the children are looked at while nothing else happens. */
#define ELEVATIOND_TICK_MS 250l
/* Programs started and not yet reaped. A request beyond them is -EAGAIN. */
#define ELEVATIOND_MAX_CHILDREN 16

struct elevationd_child {
    long pid;          /* 0: free */
    long reply_handle; /* the requester's reply pipe while it waits, or -1 */
};

static struct elevationd_child g_children[ELEVATIOND_MAX_CHILDREN];

static void log_line(const char* text) {
    (void)sxn_log(text);
}

static void close_handle(long* handle) {
    if (*handle >= 0) {
        (void)sxn_close(*handle);
        *handle = -1;
    }
}

static void send_reply(long handle, uint32_t record, int32_t status) {
    struct savanxp_elevation_reply reply;

    memset(&reply, 0, sizeof(reply));
    reply.magic = SAVANXP_ELEVATION_MAGIC;
    reply.record = record;
    reply.status = status;
    (void)sxn_write(handle, &reply, sizeof(reply));
}

static struct elevationd_child* free_child(void) {
    int index;

    for (index = 0; index < ELEVATIOND_MAX_CHILDREN; ++index) {
        if (g_children[index].pid == 0) {
            return &g_children[index];
        }
    }
    return 0;
}

/* Reaps what ended, and tells whoever waits for it. A requester that hung
 * up in the meantime only costs a write that fails. */
static void reap_children(void) {
    int index;

    for (index = 0; index < ELEVATIOND_MAX_CHILDREN; ++index) {
        struct elevationd_child* child = &g_children[index];
        struct sxn_process_query query;
        int status = 0;

        if (child->pid == 0) {
            continue;
        }
        if (sxn_process_query(child->pid, &query) == 0 && query.state != SXN_PROCESS_ENDED) {
            continue;
        }
        if (sxn_waitpid(child->pid, &status) < 0) {
            status = -1;
        }
        if (child->reply_handle >= 0) {
            send_reply(child->reply_handle, SAVANXP_ELEVATION_RECORD_EXITED, status);
            close_handle(&child->reply_handle);
        }
        child->pid = 0;
    }
}

/* Waits for `handle` to be readable. 1 if it is, 0 if the time ran out. */
static int wait_readable(long handle, long timeout_ms) {
    int32_t handles[1];

    handles[0] = (int32_t)handle;
    return sxn_wait_many(handles, 1, timeout_ms) == 0;
}

/*
 * The request is the only thing the requester decides, and it is checked
 * whole: magic, version, a NUL-terminated absolute path, and exactly argc
 * NUL-terminated strings inside args. `argv` gets pointers into it.
 */
static int parse_request(struct savanxp_elevation_request* request, char** argv) {
    uint32_t offset = 0;
    uint32_t index;

    if (request->magic != SAVANXP_ELEVATION_MAGIC || request->version != SAVANXP_ELEVATION_VERSION ||
        (request->flags & ~(uint32_t)SAVANXP_ELEVATION_WAIT) != 0u ||
        request->argc == 0u || request->argc > SAVANXP_ELEVATION_MAX_ARGS) {
        return -1;
    }
    if (request->path[0] != '/' || memchr(request->path, '\0', sizeof(request->path)) == 0) {
        return -1;
    }
    for (index = 0; index < request->argc; ++index) {
        const char* end;

        if (offset >= sizeof(request->args)) {
            return -1;
        }
        end = (const char*)memchr(request->args + offset, '\0', sizeof(request->args) - offset);
        if (end == 0) {
            return -1;
        }
        argv[index] = request->args + offset;
        offset = (uint32_t)(end - request->args) + 1u;
    }
    argv[request->argc] = 0;
    return 0;
}

/*
 * Asks windowd, showing it the program and its arguments as the request
 * carries them. 1 yes, 0 no, -errno when it cannot be asked. While the
 * prompt is up, the requester's pipe is watched only for it hanging up, which
 * withdraws the request; anything it writes is read and thrown away.
 */
static int ask_consent(
    const struct sxn_port_connection_ex* client, long client_read, const struct savanxp_elevation_request* asked) {
    struct savanxp_consent_request request;
    struct savanxp_consent_reply reply;
    struct sxn_identity creator;
    struct sxn_process_query query;
    int32_t channel[2] = {-1, -1};
    long to_windowd = -1;
    long from_windowd = -1;
    uint64_t deadline_ns = sxn_clock_ns() + (uint64_t)ELEVATIOND_CONSENT_MS * 1000000ull;
    int answer = 0;

    memset(&creator, 0, sizeof(creator));
    if (sxn_port_connect_ex(SAVANXP_CONSENT_PORT_NAME, channel, &creator) < 0) {
        /* No session, so no prompt: nothing can be elevated. */
        return -SAVANXP_ENOENT;
    }
    to_windowd = channel[0];
    from_windowd = channel[1];
    if (creator.euid != 0u) {
        log_line("elevationd: the consent port is not the system's; refusing");
        close_handle(&to_windowd);
        close_handle(&from_windowd);
        return -SAVANXP_EACCES;
    }

    memset(&request, 0, sizeof(request));
    request.magic = SAVANXP_CONSENT_MAGIC;
    request.version = SAVANXP_CONSENT_VERSION;
    request.requester_pid = client->client_pid;
    request.requester_uid = client->client.uid;
    if (sxn_process_query((long)client->client_pid, &query) == 0) {
        memcpy(request.requester_name, query.name, sizeof(request.requester_name) - 1u);
    }
    memcpy(request.path, asked->path, sizeof(request.path) - 1u);
    request.argc = asked->argc;
    memcpy(request.args, asked->args, sizeof(request.args));
    if (sxn_write(to_windowd, &request, sizeof(request)) != (long)sizeof(request)) {
        close_handle(&to_windowd);
        close_handle(&from_windowd);
        return -SAVANXP_EIO;
    }

    for (;;) {
        int32_t handles[2];
        uint64_t now = sxn_clock_ns();
        long which;

        if (now >= deadline_ns) {
            answer = 0;
            break;
        }
        handles[0] = (int32_t)from_windowd;
        handles[1] = (int32_t)client_read;
        which = sxn_wait_many(handles, 2, (long)((deadline_ns - now) / 1000000ull) + 1);
        if (which == 0) {
            long got = sxn_read(from_windowd, &reply, sizeof(reply));

            /* windowd gone, or not this protocol: no. */
            answer = (got == (long)sizeof(reply) && reply.magic == SAVANXP_CONSENT_MAGIC)
                ? (reply.answer == 1 ? 1 : (reply.answer < 0 ? (int)reply.answer : 0))
                : 0;
            break;
        }
        if (which == 1) {
            char discard[64];
            long got = sxn_read(client_read, discard, sizeof(discard));

            if (got <= 0) {
                /* The requester hung up: hanging up on windowd closes the
                 * prompt. */
                answer = -SAVANXP_EPIPE;
                break;
            }
        }
    }
    close_handle(&to_windowd);
    close_handle(&from_windowd);
    return answer;
}

static void serve(long port) {
    struct sxn_port_connection_ex client;
    struct savanxp_elevation_request request;
    char* argv[SAVANXP_ELEVATION_MAX_ARGS + 1];
    struct elevationd_child* slot = 0;
    long client_read = -1;
    long client_write = -1;
    long pid = 0;
    int consent = 0;

    memset(&client, 0, sizeof(client));
    if (sxn_port_accept_ex(port, &client) < 0) {
        return;
    }
    client_read = client.read_handle;
    client_write = client.write_handle;

    memset(&request, 0, sizeof(request));
    if (!wait_readable(client_read, ELEVATIOND_REQUEST_MS) ||
        sxn_read(client_read, &request, sizeof(request)) != (long)sizeof(request) ||
        parse_request(&request, argv) < 0) {
        /* Silence or not this protocol: an answer only to a real request. */
        close_handle(&client_read);
        close_handle(&client_write);
        return;
    }

    slot = free_child();
    if (slot == 0) {
        send_reply(client_write, SAVANXP_ELEVATION_RECORD_STARTED, -SAVANXP_EAGAIN);
        close_handle(&client_read);
        close_handle(&client_write);
        return;
    }

    /* The system asking is not asked about: it could start it itself. */
    consent = client.client.euid == 0u ? 1 : ask_consent(&client, client_read, &request);
    if (consent == 1) {
        struct sxn_spawn_request spawn;
        struct sxn_identity elevated;

        /* The same person, acting as the system. */
        memset(&elevated, 0, sizeof(elevated));
        elevated.uid = client.client.uid;
        elevated.euid = 0u;
        elevated.gid = client.client.gid;
        sxn_spawn_request_init(&spawn, request.path, argv, (int)request.argc);
        pid = sxn_spawn_ex_as(&spawn, &elevated);
        log_line(pid > 0 ? "elevationd: started an elevated program" : "elevationd: the elevated program did not start");
    } else {
        pid = consent < 0 ? consent : -SAVANXP_EACCES;
        log_line(consent == 0 ? "elevationd: the user declined" : "elevationd: the request was not asked about");
    }

    if (consent != -SAVANXP_EPIPE) {
        send_reply(client_write, SAVANXP_ELEVATION_RECORD_STARTED, (int32_t)pid);
    }
    close_handle(&client_read);
    if (pid > 0) {
        slot->pid = pid;
        slot->reply_handle = -1;
        if ((request.flags & SAVANXP_ELEVATION_WAIT) != 0u && consent != -SAVANXP_EPIPE) {
            slot->reply_handle = client_write;
            client_write = -1;
        }
    }
    close_handle(&client_write);
}

int main(void) {
    long port = sxn_port_create(SAVANXP_ELEVATION_PORT_NAME);
    int index;

    if (port < 0) {
        log_line("elevationd: the port is taken; not starting");
        sxn_exit(1);
    }
    for (index = 0; index < ELEVATIOND_MAX_CHILDREN; ++index) {
        g_children[index].pid = 0;
        g_children[index].reply_handle = -1;
    }
    log_line("elevationd: ready");

    for (;;) {
        if (sxn_wait_one(port, ELEVATIOND_TICK_MS) == 0) {
            serve(port);
        }
        reap_children();
    }
}
