/*
 * services.h -- The host side of the gate's services (sxw_abi.h), shared by
 * the host harness and winhost so both answer the guest the same way.
 *
 * Portable C, no library dependency. The platform supplies the standard
 * streams, the clock, the registry file and, for a program with windows, its
 * windows and input; everything the guest passes is
 * checked against the regions the loader mapped before it is read or
 * written.
 */
#ifndef SAVANXP_WIN32_SERVICES_H
#define SAVANXP_WIN32_SERVICES_H

#include <stddef.h>
#include <stdint.h>

#include "loader.h"
#include "registry.h"
#include "sxw_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SXW_REGISTRY_TEXT 32768u

/* What a platform with windows supplies (sxw_abi.h, "Windows and input").
 * NULL in sxw_services for a host without them: then every window service
 * fails and an event never comes. */
typedef struct sxw_window_platform {
    /* Milliseconds since the process started. */
    uint32_t (*ticks)(void *context);
    void (*display)(void *context, uint32_t *width, uint32_t *height);
    /* A new top-level window with a client area of about *width x *height,
     * which is set to the one given. The title is UTF-8; kind, owner and the
     * position are SXW_SVC_WINDOW_CREATE's. An id > 0, or 0. */
    uint32_t (*create)(void *context, const char *title, uint32_t *width, uint32_t *height, uint32_t owner,
                       uint32_t kind, int32_t x, int32_t y);
    void (*destroy)(void *context, uint32_t window);
    int (*set_title)(void *context, uint32_t window, const char *title);
    /* The window's SXW_ICON_EXTENT-square icon, 0xAARRGGBB, top row first.
     * 0 on success. */
    int (*set_icon)(void *context, uint32_t window, const uint32_t *pixels);
    /* Copies the rectangles, already clipped to the buffer, from pixels
     * (stride pixels per row, width x height) and shows them. 0 on success. */
    int (*present)(void *context, uint32_t window, const uint32_t *pixels, uint32_t stride, uint32_t width,
                   uint32_t height, const int32_t *rects, uint32_t count);
    /* Waits up to timeout milliseconds (SXW_INFINITE, or 0 to poll) for an
     * event. 1 with *event filled, 0 on timeout. */
    int (*wait_event)(void *context, uint32_t timeout, sxw_event *event);
} sxw_window_platform;

/* ---- Files (WP4) --------------------------------------------------------- */

/* What a host path is (sxw_file_platform). */
#define SXW_HOST_NONE 0u
#define SXW_HOST_FILE 1u
#define SXW_HOST_DIRECTORY 2u
#define SXW_HOST_OTHER 3u

typedef struct sxw_host_info {
    uint32_t type;     /* SXW_HOST_* */
    uint32_t writable; /* the caller may write it */
    uint64_t size;
    uint64_t mtime;    /* seconds since 1970 UTC */
} sxw_host_info;

/* The platform's file operations fail with one of these. */
#define SXW_FS_OK 0
#define SXW_FS_NOT_FOUND (-1)
#define SXW_FS_EXISTS (-2)
#define SXW_FS_DENIED (-3)
#define SXW_FS_NOT_DIRECTORY (-4)
#define SXW_FS_IS_DIRECTORY (-5)
#define SXW_FS_NOT_EMPTY (-6)
#define SXW_FS_BUSY (-7)
#define SXW_FS_FULL (-8)
#define SXW_FS_IO (-9)
#define SXW_FS_INVALID (-10)

/* open() modes. */
#define SXW_OPEN_READ 1u
#define SXW_OPEN_WRITE 2u
#define SXW_OPEN_CREATE 4u /* create a missing file, empty an existing one */
#define SXW_OPEN_NEW 8u    /* with CREATE: fail with SXW_FS_EXISTS if it exists */

/* Host paths are absolute and UTF-8, found by the services without case.
 * A file is an opaque id >= 0. NULL in sxw_services: every file service
 * answers ERROR_PATH_NOT_FOUND. */
typedef struct sxw_file_platform {
    int (*stat)(void *context, const char *path, sxw_host_info *info);
    int (*open)(void *context, const char *path, uint32_t mode, long *file);
    /* At position; *done may be short at the end of the file. */
    int (*read)(void *context, long file, uint64_t position, void *buffer, uint32_t count, uint32_t *done);
    /* At position, filling a gap past the end with zeros. */
    int (*write)(void *context, long file, uint64_t position, const void *buffer, uint32_t count,
                 uint32_t *done);
    int (*set_size)(void *context, long file, uint64_t size);
    int (*query)(void *context, long file, sxw_host_info *info);
    int (*flush)(void *context, long file);
    void (*close)(void *context, long file);
    int (*make_directory)(void *context, const char *path);
    int (*remove_directory)(void *context, const char *path);
    int (*remove_file)(void *context, const char *path);
    /* The new path does not exist; the services check it first. */
    int (*rename)(void *context, const char *old_path, const char *new_path);
    /* A directory's entries, without "." and "..", in any order. next()
     * writes a NUL-terminated name of at most 255 bytes: 1 with an entry,
     * 0 at the end. */
    int (*list_open)(void *context, const char *path, long *list);
    int (*list_next)(void *context, long list, char *name, sxw_host_info *info);
    void (*list_close)(void *context, long list);
} sxw_file_platform;

#define SXW_HOST_PATH 1024u
#define SXW_MAX_HANDLES 256u
#define SXW_MAX_OBJECTS 160u

#define SXW_OBJECT_NONE 0u
#define SXW_OBJECT_FILE 1u
#define SXW_OBJECT_FIND 2u
/* SXW_OBJECT_EVENT, _MUTEX, _SEMAPHORE and _THREAD are sxw_abi.h's. */
#define SXW_OBJECT_NAME 64u

/* What a handle names; duplicates name the same object. */
typedef struct sxw_object {
    uint32_t kind;
    uint32_t references; /* handles to it */
    union {
        struct {
            long id;
            uint64_t position;
            uint32_t read;
            uint32_t write;
            uint32_t share; /* FILE_SHARE_* */
            char path[SXW_HOST_PATH];
        } file;
        struct {
            char directory[SXW_HOST_PATH];
            char mask[256];
            char last[256]; /* the entry returned last */
            uint32_t state; /* 0 nothing yet, 1 after ".", 2 after "..", 3 after a name */
            uint32_t root;  /* a drive's root: no dot entries */
        } find;
        struct {
            uint32_t manual;
            uint32_t signaled;
        } event;
        struct {
            uint32_t owner;     /* thread index + 1, or 0 */
            uint32_t recursion;
            uint32_t abandoned; /* its owner ended holding it */
        } mutex;
        struct {
            int32_t count;
            int32_t maximum;
        } semaphore;
        struct {
            uint32_t index;
        } thread;
    } u;
    char name[SXW_OBJECT_NAME]; /* a named event, mutex or semaphore; "" */
} sxw_object;

/* ---- Threads (WP3) ------------------------------------------------------- */

/* What a platform supplies for threads. Every guest thread is a host thread;
 * they take turns through one lock the services keep (threads.c). */
typedef struct sxw_thread_platform {
    /* Starts a host thread that runs sxw_thread_main(services, index). 0 on
     * success. */
    int (*start)(void *context, uint32_t index);
    /* Parks the calling host thread while *word holds expected, at most
     * timeout milliseconds (SXW_INFINITE: no limit). It may return early. */
    void (*wait_word)(void *context, volatile uint32_t *word, uint32_t expected, uint32_t timeout);
    /* Releases every host thread parked on word. */
    void (*wake_word)(void *context, volatile uint32_t *word);
    /* A monotonic clock in milliseconds, for the waits' timeouts. */
    uint32_t (*milliseconds)(void *context);
} sxw_thread_platform;

#define SXW_MAX_THREADS 32u
#define SXW_MAX_PENDING 64u

#define SXW_THREAD_FREE 0u
#define SXW_THREAD_CREATED 1u /* not yet in guest code */
#define SXW_THREAD_RUNNING 2u
#define SXW_THREAD_ENDED 3u

typedef struct sxw_thread {
    uint32_t state;
    uint32_t id;
    uint32_t teb;
    uint32_t region;    /* its memory, 0 for the first thread's */
    uint32_t stack_top;
    uint32_t start;     /* the guest's thread function and its parameter */
    uint32_t parameter;
    uint32_t suspend;   /* SuspendThread's count */
    int32_t priority;
    uint32_t exit_code;
    uint32_t object;    /* its thread object, index + 1 */
    volatile uint32_t park; /* what it parks on: changed to wake it */
    uint32_t waiting;   /* in a wait the objects' changes end */
    uint32_t woken;     /* user32 posted or sent it a message */
    uint32_t leaving;   /* it called ExitThread: back to the host */
    uint32_t host_windows; /* host windows it created */
    sxw_event pending[SXW_MAX_PENDING]; /* input for its windows */
    uint32_t pending_head;
    uint32_t pending_count;
} sxw_thread;

typedef struct sxw_services {
    sxw_process *process;
    void *context;
    /* Writes to standard input (0), output (1) or error (2). Returns the
     * bytes written, or a negative value on failure. */
    long (*write)(void *context, int stream, const void *buffer, uint32_t length);
    /* Seconds since 1970 UTC. */
    uint64_t (*now)(void *context);
    /* Replaces the registry file with text. 0, or nonzero on failure. NULL
     * keeps the registry in memory only. */
    int (*save_registry)(void *context, const char *text, size_t length);
    /* HKEY_CURRENT_USER, loaded by the platform before the guest starts. */
    sxw_registry *registry;
    /* With fixed_time nonzero, the guest's clock reads that second and stays
     * there (a test run): a program that seeds from it deals the same however
     * long its start takes. */
    uint32_t fixed_time;
    /* Set when the guest asked to end, with its 32-bit status. */
    int exit_requested;
    uint32_t exit_status;
    /* Windows and input, or NULL. */
    const sxw_window_platform *windows;
    /* Files, or NULL. */
    const sxw_file_platform *files;
    /* The host folder that is C:, or NULL; Z: is the host's root. */
    const char *drive_c;
    /* Handle slot -> object index + 1, or 0. */
    uint8_t handles[SXW_MAX_HANDLES];
    sxw_object objects[SXW_MAX_OBJECTS];
    /* Threads, or NULL for a host that runs one. */
    const sxw_thread_platform *thread_platform;
    sxw_thread threads[SXW_MAX_THREADS];
    /* The lock guest threads take turns by: a ticket lock. */
    volatile uint32_t turn_next;
    volatile uint32_t turn_serving;
    /* ExitProcess's thread + 1: every other thread stops for good. */
    uint32_t ending;
    /* ntdll!SxwThreadStart, where a new thread enters the guest. */
    uint32_t thread_entry;
    /* Host window id -> the thread that made it, index + 1. */
    uint8_t window_owner[64];
    char registry_text[SXW_REGISTRY_TEXT];
} sxw_services;

/* Call once before the guest starts, on the host thread that will run the
 * guest's first thread: it takes the turn for it. */
void sxw_services_start(sxw_services *services);

/* Performs the service the guest's stack names, for the guest thread at
 * index (SXW_TEB_HOST_THREAD). guest_esp points at
 * [return address][service][arguments]. Returns the value for eax. When it
 * returns, sxw_should_leave() says whether the thread goes back to the host
 * instead of the guest. */
uint32_t sxw_service(sxw_services *services, uint32_t thread, uint32_t guest_esp);

/* 1 when the guest thread at index must go back to the host: the process
 * or the thread ended. */
int sxw_should_leave(const sxw_services *services, uint32_t thread);

/* A new host thread's whole life (sxw_thread_platform.start): it takes its
 * turn, waits while suspended, then calls enter(index, entry, esp) to run
 * the guest from entry with the stack at esp until the thread leaves, then
 * gives back its memory and its turn. Returns 1 when the process ended
 * instead (the host ends the process), 0 when only the thread did. */
int sxw_thread_main(sxw_services *services, uint32_t index,
                    void (*enter)(void *context, uint32_t index, uint32_t entry, uint32_t esp), void *context);

/* The thread services (threads.c), SXW_SVC_THREAD_CREATE to
 * SXW_SVC_END_OTHER_THREADS, and the wait for host input
 * (SXW_SVC_WAIT_EVENT) that a thread with windows makes. */
uint32_t sxw_thread_service(sxw_services *services, uint32_t thread, uint32_t service, const uint32_t *arguments);
uint32_t sxw_wait_event(sxw_services *services, uint32_t thread, uint32_t timeout, sxw_event *event);

/* The first thread's state, from the loaded process (services.c calls it). */
void sxw_threads_start(sxw_services *services);
/* A host window the thread made: its input goes to that thread. */
void sxw_window_created(sxw_services *services, uint32_t thread, uint32_t window);
/* At each call to the host: give the turn to a waiting thread, stop while
 * suspended or ended. */
void sxw_thread_checkpoint(sxw_services *services, uint32_t thread);

/* The turn: taken around the guest's code, given up while a thread waits. */
void sxw_turn_take(sxw_services *services);
void sxw_turn_give(sxw_services *services);

/* Object operations files.c's handle table shares with threads.c. */
sxw_object *sxw_object_of(sxw_services *services, uint32_t handle, uint32_t kind);
sxw_object *sxw_new_object(sxw_services *services, uint32_t kind);
uint32_t sxw_new_handle(sxw_services *services, sxw_object *object);

/* Whether [address, address + length) lies inside one mapped guest region. */
int sxw_guest_range(const sxw_process *process, uint32_t address, uint32_t length);

/* The handle and file services (files.c), SXW_SVC_CLOSE to FIND_NEXT. */
uint32_t sxw_file_service(sxw_services *services, uint32_t service, const uint32_t *arguments);

/* Closes a handle of any kind: a Win32 error. */
uint32_t sxw_close_handle(sxw_services *services, uint32_t handle);

/* The first handle value; handles are multiples of 4 from here. */
#define SXW_HANDLE_FIRST 0x40u

/* The console handles Windows XP gives a console program, by stream. */
#define SXW_HANDLE_STDIN 3u
#define SXW_HANDLE_STDOUT 7u
#define SXW_HANDLE_STDERR 11u

/* STATUS_ACCESS_VIOLATION: a service frame or argument block that is not
 * guest memory ends the process with it. */
#define SXW_STATUS_ACCESS_VIOLATION 0xc0000005u
/* STATUS_INVALID_SYSTEM_SERVICE: an unknown service number. */
#define SXW_STATUS_INVALID_SYSTEM_SERVICE 0xc000001cu

#ifdef __cplusplus
}
#endif

#endif
