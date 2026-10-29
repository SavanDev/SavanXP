#pragma once

#include <stdint.h>

/*
 * Elevation: the requester, the broker and the prompt
 * (docs/kernel/USERS_AND_PERMISSIONS.md#elevation).
 *
 *   1. A program connects to the broker's port (SAVANXP_ELEVATION_PORT_NAME)
 *      and writes one savanxp_elevation_request: what to run elevated.
 *   2. The broker, elevationd, knows who asked from the connection itself
 *      (SXN_SYS_PORT_ACCEPT_EX), never from the request. It connects to
 *      windowd's consent port, checks that the system created it
 *      (SXN_SYS_PORT_CONNECT_EX), and writes one savanxp_consent_request.
 *   3. windowd shows the prompt above every window, with the program and every
 *      argument it would get, and takes the answer only
 *      from the keyboard and mouse it reads itself (INPUT_IOC_CLAIM). It
 *      writes one savanxp_consent_reply and hangs up. It refuses a consent
 *      request from anybody but the system.
 *   4. On yes the broker starts the program as the same person acting as the
 *      system (uid unchanged, euid 0) and answers with a
 *      SAVANXP_ELEVATION_RECORD_STARTED reply: the pid, or -errno. The
 *      requester never gets the token itself.
 *   5. With SAVANXP_ELEVATION_WAIT the connection stays open, and a
 *      SAVANXP_ELEVATION_RECORD_EXITED reply carries the program's exit status
 *      when it ends: the program is the broker's child, not the requester's.
 *
 * A requester that already acts as the system is not asked about: it could
 * start the program itself.
 *
 * Nothing the requester writes after its request is read as an answer, and
 * nothing it writes reaches windowd: the only way to say yes is the keyboard
 * or the mouse.
 */

#define SAVANXP_ELEVATION_PORT_NAME "elevation"
#define SAVANXP_CONSENT_PORT_NAME "windowd.consent"

#define SAVANXP_ELEVATION_MAGIC 0x56454c45u /* "ELEV" */
#define SAVANXP_CONSENT_MAGIC 0x534e4f43u   /* "CONS" */
#define SAVANXP_ELEVATION_VERSION 1u
/* The broker and windowd's own exchange: 2 since it carries the arguments. */
#define SAVANXP_CONSENT_VERSION 2u

#define SAVANXP_ELEVATION_PATH_CAPACITY 128u
#define SAVANXP_ELEVATION_ARGS_CAPACITY 384u
#define SAVANXP_ELEVATION_MAX_ARGS 16u
#define SAVANXP_ELEVATION_NAME_CAPACITY 32u

enum savanxp_elevation_flags {
    /* Keep the connection open and report the exit status. */
    SAVANXP_ELEVATION_WAIT = 1u << 0,
};

/* Requester -> broker. */
struct savanxp_elevation_request {
    uint32_t magic;   /* SAVANXP_ELEVATION_MAGIC */
    uint32_t version; /* SAVANXP_ELEVATION_VERSION */
    uint32_t flags;   /* enum savanxp_elevation_flags */
    uint32_t argc;    /* strings in args, argv[0] included: 1..MAX_ARGS */
    char path[SAVANXP_ELEVATION_PATH_CAPACITY]; /* absolute, NUL-terminated */
    char args[SAVANXP_ELEVATION_ARGS_CAPACITY]; /* argc NUL-terminated strings, back to back */
};

enum savanxp_elevation_record {
    /* status: the pid (> 0), -EACCES when the user said no, or -errno. */
    SAVANXP_ELEVATION_RECORD_STARTED = 1,
    /* status: the exit status, as waitpid reports it. */
    SAVANXP_ELEVATION_RECORD_EXITED = 2,
};

/* Broker -> requester. */
struct savanxp_elevation_reply {
    uint32_t magic;  /* SAVANXP_ELEVATION_MAGIC */
    uint32_t record; /* enum savanxp_elevation_record */
    int32_t status;
    uint32_t reserved;
};

/* Broker -> windowd: what the prompt shows. The arguments are the request's
 * as they are, so what the user is shown is what the program gets; argv[0]
 * is not shown, the path is. A prompt that cannot show all of it on the
 * screen is not shown: windowd answers -E2BIG. */
struct savanxp_consent_request {
    uint32_t magic;   /* SAVANXP_CONSENT_MAGIC */
    uint32_t version; /* SAVANXP_CONSENT_VERSION */
    uint32_t requester_pid;
    uint32_t requester_uid;
    char requester_name[SAVANXP_ELEVATION_NAME_CAPACITY]; /* its image's file name */
    char path[SAVANXP_ELEVATION_PATH_CAPACITY];           /* what would run */
    uint32_t argc;                                        /* as in the elevation request */
    char args[SAVANXP_ELEVATION_ARGS_CAPACITY];           /* argc NUL-terminated strings */
};

/* Whether `args` holds exactly `argc` NUL-terminated strings, 1..MAX_ARGS of
 * them, inside its capacity. Both ends check it. */
static inline int savanxp_elevation_args_valid(const char *args, uint32_t argc)
{
    uint32_t offset = 0;
    uint32_t index;

    if (argc == 0u || argc > SAVANXP_ELEVATION_MAX_ARGS)
    {
        return 0;
    }
    for (index = 0; index < argc; ++index)
    {
        while (offset < SAVANXP_ELEVATION_ARGS_CAPACITY && args[offset] != '\0')
        {
            ++offset;
        }
        if (offset >= SAVANXP_ELEVATION_ARGS_CAPACITY)
        {
            return 0;
        }
        ++offset;
    }
    return 1;
}

/* windowd -> broker. */
struct savanxp_consent_reply {
    uint32_t magic;  /* SAVANXP_CONSENT_MAGIC */
    int32_t answer;  /* 1 yes, 0 no, -errno when windowd would not ask */
};
