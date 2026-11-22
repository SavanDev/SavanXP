/* Web Viewer's downloads: see download.h and docs/desktop/WEBVIEWER.md#downloads.
 *
 * Three parts, each on its side of the threads:
 *
 * - the worker, its own thread with its own resolver and HTTP client, so a
 *   page loads while a file downloads. It GETs the URL again (the page
 *   worker stopped at its head), writes the body into <path>.part through
 *   sxnhttp's sink, and renames it over <path> only when the body is whole;
 *   on any failure, Cancel or the browser closing, it deletes the .part;
 * - a single-slot mailbox under a futex lock, as the page worker's: the
 *   latest progress and the final result, handed over with sxngui_app_wake;
 * - the download window, a modeless window (sxngui_window) after IE5's File
 *   Download: name and host, a bar, sizes, the rate, where it goes, and
 *   Cancel; when it is done, Open Folder and Close.
 */

#include "download.h"

#include "sxgfx/sxgui.h"
#include "sxn/sxn.h"
#include "sxn/sxndns.h"
#include "sxn/sxnfiledlg.h"
#include "sxn/sxnprofile.h"

#include <stdio.h>
#include <string.h>

#include "shared/version.h"

#define DOWNLOAD_PATH_CAP SXN_FILE_DIALOG_PATH_MAX
#define DOWNLOAD_WORKER_STACK (64 * 1024)
#define DOWNLOAD_PROGRESS_NS 150000000ull /* the window hears at most this often */
#define DOWNLOAD_WRITE_BUFFER (64 * 1024)   /* the file is written in pieces this large */

enum download_state
{
    DOWNLOAD_RUNNING,
    DOWNLOAD_DONE,
    DOWNLOAD_FAILED,
    DOWNLOAD_CANCELLED,
};

struct download_job
{
    char url[SXNHTTP_URL_CAP];
    char path[DOWNLOAD_PATH_CAP];
    struct sxn_ipv4_endpoint dns;
};

/* What the worker reports. */
struct download_report
{
    enum download_state state;
    uint64_t received;
    uint64_t expected; /* 0: unknown */
    char host[SXNHTTP_HOST_CAP];
    char message[160];
};

static struct sxngui_app *g_app;

/* ---- the mailbox ----------------------------------------------------------- */

static uint32_t g_lock; /* 0 free, 1 held, 2 held with waiters */
static struct download_job g_job;  /* under the lock */
static int g_job_ready;
static int g_quit;
static struct download_report g_report;
static int g_report_ready;

static long g_work_event = -1;   /* auto-reset: a job, or quitting */
static long g_cancel_event = -1; /* manual-reset: drop the download */
static long g_worker = -1;
static unsigned char g_worker_stack[DOWNLOAD_WORKER_STACK] __attribute__((aligned(16)));

static void download_lock(void)
{
    uint32_t state = 0;

    if (__atomic_compare_exchange_n(&g_lock, &state, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    {
        return;
    }
    if (state != 2)
    {
        state = __atomic_exchange_n(&g_lock, 2, __ATOMIC_ACQUIRE);
    }
    while (state != 0)
    {
        (void)sxn_wait_address(&g_lock, 2, sizeof(g_lock), -1);
        state = __atomic_exchange_n(&g_lock, 2, __ATOMIC_ACQUIRE);
    }
}

static void download_unlock(void)
{
    if (__atomic_exchange_n(&g_lock, 0, __ATOMIC_RELEASE) == 2)
    {
        (void)sxn_wake_address(&g_lock, 1);
    }
}

static void post(const struct download_report *report)
{
    download_lock();
    g_report = *report;
    g_report_ready = 1;
    download_unlock();
    (void)sxngui_app_wake(g_app);
}

/* ---- the worker ------------------------------------------------------------ */

static struct sxndns_resolver g_resolver;
static struct sxnhttp_client g_http;

/* The worker's own, for one job. */
struct transfer
{
    long file;
    long write_error;
    struct download_report report;
    uint64_t last_post_ns;
    size_t buffered;
};

/* The body arrives in pieces of a few KiB; each write to the file system
 * costs far more than its bytes, so they are gathered first. */
static unsigned char g_write_buffer[DOWNLOAD_WRITE_BUFFER];

static int transfer_flush(struct transfer *transfer)
{
    const unsigned char *bytes = g_write_buffer;
    size_t length = transfer->buffered;

    transfer->buffered = 0;
    while (length > 0)
    {
        long written = sxn_write(transfer->file, bytes, length);

        if (written <= 0)
        {
            transfer->write_error = written < 0 ? written : -SAVANXP_EIO;
            return -1;
        }
        bytes += written;
        length -= (size_t)written;
    }
    return 0;
}

static long transfer_sink(void *user, const unsigned char *bytes, size_t length)
{
    struct transfer *transfer = user;

    while (length > 0)
    {
        size_t take = sizeof(g_write_buffer) - transfer->buffered;

        if (take > length)
        {
            take = length;
        }
        memcpy(g_write_buffer + transfer->buffered, bytes, take);
        transfer->buffered += take;
        bytes += take;
        length -= take;
        if (transfer->buffered == sizeof(g_write_buffer) && transfer_flush(transfer) != 0)
        {
            return -1;
        }
    }
    return 0;
}

/* Only a 2xx is the file; an error page is not saved as if it were. */
static int transfer_head(void *user, const struct sxnhttp_response *response)
{
    (void)user;
    return response->status < 200 || response->status > 299;
}

static void transfer_progress(void *user, enum sxnhttp_phase phase, const char *host, uint64_t received,
                              uint64_t expected)
{
    struct transfer *transfer = user;
    const uint64_t now = sxn_clock_ns();

    snprintf(transfer->report.host, sizeof(transfer->report.host), "%s", host);
    transfer->report.received = received;
    transfer->report.expected = expected;
    if (phase != SXNHTTP_PHASE_RECEIVING || now - transfer->last_post_ns >= DOWNLOAD_PROGRESS_NS)
    {
        transfer->last_post_ns = now;
        post(&transfer->report);
    }
}

static void run_job(const struct download_job *job)
{
    static struct sxnhttp_response response;
    struct sxnhttp_request request;
    struct transfer transfer;
    char part[DOWNLOAD_PATH_CAP + 8];
    long result;

    memset(&transfer, 0, sizeof(transfer));
    transfer.report.state = DOWNLOAD_RUNNING;
    snprintf(part, sizeof(part), "%s.part", job->path);
    transfer.file = sxn_create_file(part, SXN_CREATE_ALWAYS);
    if (transfer.file < 0)
    {
        transfer.report.state = DOWNLOAD_FAILED;
        snprintf(transfer.report.message, sizeof(transfer.report.message), "Cannot write in that folder (error %ld).",
            -transfer.file);
        post(&transfer.report);
        return;
    }

    sxndns_set_server(&g_resolver, &job->dns);
    memset(&request, 0, sizeof(request));
    request.url = job->url;
    request.cancel = g_cancel_event;
    request.user_agent = "SavanXP-WebViewer/" SAVANXP_VERSION_STRING;
    request.progress = transfer_progress;
    request.head = transfer_head;
    request.sink = transfer_sink;
    request.user = &transfer;
    memset(&response, 0, sizeof(response));
    result = sxnhttp_get(&g_http, &request, &response);
    if (result == 0 && (response.flags & SXNHTTP_RESPONSE_HEAD_ONLY) != 0)
    {
        snprintf(transfer.report.message, sizeof(transfer.report.message), "The server answered HTTP %d.",
            response.status);
        result = -1;
    }
    else if (result == SXNHTTP_ERROR_SINK)
    {
        snprintf(transfer.report.message, sizeof(transfer.report.message), "Writing the file failed (error %ld).",
            -transfer.write_error);
    }
    else if (result == SXNHTTP_ERROR_DNS)
    {
        snprintf(transfer.report.message, sizeof(transfer.report.message), "Cannot resolve %s: %s",
            transfer.report.host, sxndns_error_text(response.dns_error));
    }
    else if (result != 0 && response.tls_detail[0] != '\0')
    {
        snprintf(transfer.report.message, sizeof(transfer.report.message), "%s (%s)", sxnhttp_error_text(result),
            response.tls_detail);
    }
    else if (result != 0)
    {
        snprintf(transfer.report.message, sizeof(transfer.report.message), "%s", sxnhttp_error_text(result));
    }

    /* Whole: written, durable, then under its name. Anything else leaves
     * nothing. */
    if (result == 0 && transfer_flush(&transfer) != 0)
    {
        snprintf(transfer.report.message, sizeof(transfer.report.message), "Writing the file failed (error %ld).",
            -transfer.write_error);
        result = -1;
    }
    if (result == 0 && sxn_flush_file(transfer.file) != 0)
    {
        snprintf(transfer.report.message, sizeof(transfer.report.message), "Writing the file failed.");
        result = -1;
    }
    (void)sxn_close(transfer.file);
    if (result == 0)
    {
        long renamed = sxn_rename_path(part, job->path);

        if (renamed != 0)
        {
            snprintf(transfer.report.message, sizeof(transfer.report.message),
                "The file could not take its name (error %ld).", -renamed);
            result = -1;
        }
    }
    if (result != 0)
    {
        (void)sxn_delete_path(part);
    }
    transfer.report.received = response.body_length;
    transfer.report.state = result == 0                          ? DOWNLOAD_DONE
                            : result == SXNHTTP_ERROR_CANCELLED ? DOWNLOAD_CANCELLED
                                                                : DOWNLOAD_FAILED;
    {
        char line[DOWNLOAD_PATH_CAP + 64];

        snprintf(line, sizeof(line), "webview: download %s: %s (%llu bytes)",
            transfer.report.state == DOWNLOAD_DONE ? "saved" : transfer.report.state == DOWNLOAD_CANCELLED
                ? "cancelled" : "failed",
            job->path, (unsigned long long)transfer.report.received);
        (void)sxn_log(line);
    }
    post(&transfer.report);
}

static void worker_main(void *argument)
{
    struct download_job job;

    (void)argument;
    sxndns_init(&g_resolver);
    sxnhttp_client_init(&g_http, &g_resolver);
    for (;;)
    {
        int have = 0;

        (void)sxn_wait_one(g_work_event, -1);
        download_lock();
        if (g_quit)
        {
            download_unlock();
            sxn_thread_exit(0);
        }
        if (g_job_ready)
        {
            job = g_job;
            g_job_ready = 0;
            (void)sxn_event_reset(g_cancel_event);
            have = 1;
        }
        download_unlock();
        if (have)
        {
            run_job(&job);
        }
    }
}

/* ---- the window ------------------------------------------------------------- */

#define WINDOW_WIDTH 400
#define WINDOW_HEIGHT 178
#define MARGIN 12
#define ROW 20
#define OPEN_FOLDER_WIDTH 100

enum
{
    W_NAME,
    W_BAR,
    W_SIZE,
    W_RATE,
    W_TARGET,
    W_OPEN_FOLDER,
    W_CLOSE,
    W_COUNT,
};

static struct sxngui_window g_window;
static struct sxgui_widget g_widgets[W_COUNT];
static char g_name_line[192];
static char g_size_line[96];
static char g_rate_line[96];
static char g_target_line[192];

/* The UI's own. */
static int g_busy;      /* a job was handed over and its end not yet seen */
static char g_path[DOWNLOAD_PATH_CAP];
static char g_name[SXNHTTP_FILENAME_CAP];
static uint64_t g_started_ns;
static struct download_report g_shown;
static char g_pending_url[SXNHTTP_URL_CAP];
static struct sxn_ipv4_endpoint g_pending_dns;
static long g_folder_pid = -1;

static void format_size(uint64_t bytes, char *out, size_t capacity)
{
    if (bytes < 1024u)
    {
        snprintf(out, capacity, "%llu bytes", (unsigned long long)bytes);
    }
    else if (bytes < 1024u * 1024u)
    {
        snprintf(out, capacity, "%llu.%llu KB", (unsigned long long)(bytes / 1024u),
            (unsigned long long)(bytes % 1024u * 10u / 1024u));
    }
    else
    {
        const uint64_t mib = 1024u * 1024u;

        snprintf(out, capacity, "%llu.%llu MB", (unsigned long long)(bytes / mib),
            (unsigned long long)(bytes % mib * 10u / mib));
    }
}

/* The labels from g_shown. */
static void refresh_window(void)
{
    char received[32], expected[32], rate[32];
    const uint64_t elapsed_ms = (sxn_clock_ns() - g_started_ns) / 1000000ull;
    const uint64_t per_second = elapsed_ms > 0 ? g_shown.received * 1000u / elapsed_ms : 0;

    if (!g_window.open)
    {
        return;
    }
    format_size(g_shown.received, received, sizeof(received));
    format_size(g_shown.expected, expected, sizeof(expected));
    format_size(per_second, rate, sizeof(rate));
    switch (g_shown.state)
    {
    case DOWNLOAD_RUNNING:
        snprintf(g_name_line, sizeof(g_name_line), "Saving %s from %s", g_name,
            g_shown.host[0] != '\0' ? g_shown.host : "...");
        if (g_shown.expected != 0)
        {
            snprintf(g_size_line, sizeof(g_size_line), "%s of %s copied", received, expected);
            g_widgets[W_BAR].value = (int)(g_shown.received * 100u / g_shown.expected);
        }
        else
        {
            snprintf(g_size_line, sizeof(g_size_line), "%s copied", received);
        }
        snprintf(g_rate_line, sizeof(g_rate_line), "Transfer rate: %s/s", rate);
        break;
    case DOWNLOAD_DONE:
        snprintf(g_name_line, sizeof(g_name_line), "Download complete: %s", g_name);
        snprintf(g_size_line, sizeof(g_size_line), "%s in %llu s", received,
            (unsigned long long)((elapsed_ms + 999u) / 1000u));
        snprintf(g_rate_line, sizeof(g_rate_line), "Transfer rate: %s/s", rate);
        g_widgets[W_BAR].value = 100;
        g_widgets[W_OPEN_FOLDER].flags |= SXGUI_FLAG_VISIBLE;
        g_widgets[W_CLOSE].text = "Close";
        break;
    case DOWNLOAD_FAILED:
    default:
        snprintf(g_name_line, sizeof(g_name_line), "Download failed: %s", g_name);
        snprintf(g_size_line, sizeof(g_size_line), "%s", g_shown.message);
        snprintf(g_rate_line, sizeof(g_rate_line), "Nothing was saved.");
        g_widgets[W_CLOSE].text = "Close";
        break;
    }
    sxngui_window_request_repaint(&g_window);
}

/* Cancel while it runs; afterwards, Close. */
static void on_close_button(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    if (g_shown.state == DOWNLOAD_RUNNING)
    {
        (void)sxn_event_set(g_cancel_event);
    }
    sxngui_window_close(&g_window);
}

/* The window's X is the same: a running download is cancelled, as in IE. */
static void on_window_close(struct sxngui_window *window)
{
    (void)window;
    if (g_shown.state == DOWNLOAD_RUNNING)
    {
        (void)sxn_event_set(g_cancel_event);
    }
}

static int on_window_key(struct sxngui_window *window, const struct savanxp_input_event *event)
{
    (void)window;
    if (event->type == SAVANXP_INPUT_EVENT_KEY_DOWN && event->key == SAVANXP_KEY_ESC)
    {
        on_close_button(0, 0);
        return 1;
    }
    return 0;
}

/* Files on the folder the file went to. */
static void on_open_folder(struct sxgui_widget *widget, void *user)
{
    char folder[DOWNLOAD_PATH_CAP];
    char *slash;
    char *argv[3];

    (void)widget;
    (void)user;
    snprintf(folder, sizeof(folder), "%s", g_path);
    slash = strrchr(folder, '/');
    if (slash == 0)
    {
        return;
    }
    slash[slash == folder ? 1 : 0] = '\0';
    if (g_folder_pid > 0)
    {
        struct sxn_process_query query;
        int status = 0;

        if (sxn_process_query(g_folder_pid, &query) != 0 || query.state != SXN_PROCESS_RUNNING)
        {
            (void)sxn_waitpid(g_folder_pid, &status);
        }
    }
    argv[0] = "/bin/filesapp.sxe";
    argv[1] = folder;
    argv[2] = 0;
    g_folder_pid = sxn_spawn_stdio(argv[0], argv, 2);
    sxngui_window_close(&g_window);
}

static void open_window(void)
{
    const int text_width = WINDOW_WIDTH - MARGIN * 2;
    const int buttons_y = WINDOW_HEIGHT - MARGIN - SXGUI_BUTTON_HEIGHT;

    sxngui_window_close(&g_window); /* the last download's, still showing */
    g_name_line[0] = '\0';
    g_size_line[0] = '\0';
    g_rate_line[0] = '\0';
    snprintf(g_target_line, sizeof(g_target_line), "Download to: %s", g_path);
    g_widgets[W_NAME] = sxgui_label(sx_rect_make(MARGIN, MARGIN, text_width, 18), g_name_line);
    g_widgets[W_BAR] = sxgui_progress(sx_rect_make(MARGIN, MARGIN + ROW + 4, text_width, 18), 0, 100, 0);
    g_widgets[W_SIZE] = sxgui_label(sx_rect_make(MARGIN, MARGIN + ROW * 2 + 12, text_width, 18), g_size_line);
    g_widgets[W_RATE] = sxgui_label(sx_rect_make(MARGIN, MARGIN + ROW * 3 + 12, text_width, 18), g_rate_line);
    g_widgets[W_TARGET] = sxgui_label(sx_rect_make(MARGIN, MARGIN + ROW * 4 + 12, text_width, 18), g_target_line);
    g_widgets[W_OPEN_FOLDER] = sxgui_button(
        sx_rect_make(WINDOW_WIDTH - MARGIN - SXGUI_BUTTON_WIDTH - 8 - OPEN_FOLDER_WIDTH, buttons_y, OPEN_FOLDER_WIDTH,
            SXGUI_BUTTON_HEIGHT),
        "Open Folder", on_open_folder, 0);
    g_widgets[W_OPEN_FOLDER].flags &= ~SXGUI_FLAG_VISIBLE;
    g_widgets[W_CLOSE] = sxgui_button(
        sx_rect_make(WINDOW_WIDTH - MARGIN - SXGUI_BUTTON_WIDTH, buttons_y, SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "Cancel", on_close_button, 0);
    g_window.on_key = on_window_key;
    g_window.on_close = on_window_close;
    if (sxngui_window_open(g_app, &g_window, "File Download", WINDOW_WIDTH, WINDOW_HEIGHT, g_widgets, W_COUNT) ==
        0)
    {
        sxgui_focus(&g_window.ui, W_CLOSE);
    }
    refresh_window();
}

/* ---- the UI's side ----------------------------------------------------------- */

void webview_download_init(struct sxngui_app *app)
{
    g_app = app;
    g_work_event = sxn_event_create(0);
    g_cancel_event = sxn_event_create(SXN_EVENT_MANUAL_RESET);
    if (g_work_event >= 0 && g_cancel_event >= 0)
    {
        g_worker = sxn_thread_create(worker_main, 0, g_worker_stack + sizeof(g_worker_stack), 0);
    }
}

int webview_download_busy(void)
{
    return g_busy;
}

static void start(const char *path)
{
    snprintf(g_path, sizeof(g_path), "%s", path);
    {
        const char *slash = strrchr(path, '/');

        snprintf(g_name, sizeof(g_name), "%s", slash != 0 ? slash + 1 : path);
    }
    memset(&g_shown, 0, sizeof(g_shown));
    g_shown.state = DOWNLOAD_RUNNING;
    g_started_ns = sxn_clock_ns();
    g_busy = 1;
    download_lock();
    snprintf(g_job.url, sizeof(g_job.url), "%s", g_pending_url);
    snprintf(g_job.path, sizeof(g_job.path), "%s", path);
    g_job.dns = g_pending_dns;
    g_job_ready = 1;
    g_report_ready = 0;
    download_unlock();
    (void)sxn_event_set(g_work_event);
    open_window();
}

static void on_save_chosen(struct sxngui_app *app, const char *path, void *user)
{
    (void)app;
    (void)user;
    if (path != 0 && !g_busy)
    {
        start(path);
    }
}

int webview_download_begin(const char *url, const char *suggested_name, const struct sxn_ipv4_endpoint *dns)
{
    struct sxn_file_dialog_request request;

    if (g_busy || g_worker < 0)
    {
        return -1;
    }
    snprintf(g_pending_url, sizeof(g_pending_url), "%s", url);
    g_pending_dns = *dns;
    memset(&request, 0, sizeof(request));
    request.mode = SXN_FILE_DIALOG_SAVE;
    request.title = "Save As";
    request.initial_directory = sxn_profile_directory();
    request.file_name = suggested_name;
    request.on_done = on_save_chosen;
    return sxn_file_dialog_begin(g_app, &request) == 0 ? 0 : -1;
}

void webview_download_wake(void)
{
    struct download_report report;
    int have = 0;

    download_lock();
    if (g_report_ready)
    {
        report = g_report;
        g_report_ready = 0;
        have = 1;
    }
    download_unlock();
    if (!have || !g_busy)
    {
        return;
    }
    g_shown = report;
    if (report.state != DOWNLOAD_RUNNING)
    {
        g_busy = 0;
    }
    if (report.state == DOWNLOAD_CANCELLED)
    {
        sxngui_window_close(&g_window);
        return;
    }
    refresh_window();
}

void webview_download_shutdown(void)
{
    if (g_worker < 0)
    {
        return;
    }
    download_lock();
    g_quit = 1;
    g_job_ready = 0;
    download_unlock();
    (void)sxn_event_set(g_cancel_event);
    (void)sxn_event_set(g_work_event);
    /* The worker finishes the job it has -- cancelled, its .part deleted --
     * before it sees the quit. */
    (void)sxn_wait_one(g_worker, 5000);
}
