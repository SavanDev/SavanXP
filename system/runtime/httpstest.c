/* sxnhttp's https:// against tools/tls_fixture.py: N5's gate in
 * docs/roadmaps/open/WEBVIEWER_ROADMAP.md. A SUN program with
 * system/runtime/sxnhttp_tls.c and the OpenSSL port's libraries.
 *
 * `httpstest <control port>` asks the echo server for the fixtures' ports,
 * fetches the test root over plain HTTP and trusts it for these requests
 * only (sxnhttp_request.trust_pem; it is never installed). Then:
 *
 * - a valid certificate works, by name (SNI, ALPN http/1.1) and by address;
 * - wrong name, expired, not yet valid, self-signed, missing intermediate,
 *   an untrusted CA and a TLS 1.1 server each fail with their own error;
 * - the system store does not trust the test root;
 * - with the date set before the release floor, https:// is TLS_CLOCK before
 *   anything is sent; the date is restored.
 *
 * `httpstest unseeded`, on a machine with no entropy source: TLS_ENTROPY.
 *
 * "HTTPS TEST PASS" or "HTTPS TEST FAIL" ends it. */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sxn/sxn.h"
#include "sxn/sxndns.h"
#include "sxn/sxnhttp.h"

static int g_failures;
static struct sxndns_resolver g_resolver;
static struct sxnhttp_client g_client;
static struct sxnhttp_response g_response;
static unsigned char g_body[16384];
static char g_root[8192];
static size_t g_root_length;

static void say(const char* text) { (void)sxn_log(text); }

static void check(int ok, const char* what) {
    char line[200];

    if (!ok) {
        g_failures++;
        snprintf(line, sizeof(line), "httpstest: FAILED %s", what);
        say(line);
    }
}

/* One line from the echo server: up to two numbers. */
static int control_ask(long control, const char* command, unsigned* first, unsigned* second) {
    struct sxn_io_interest interest = {(int32_t)control, SXN_IO_WRITE | SXN_IO_ERROR};
    unsigned values[2] = {0, 0};
    int index = 0, digits = 0;
    char byte = 0;

    if (sxn_wait_io(&interest, 1, 5000) != 0 ||
        sxn_write(control, command, strlen(command)) != (long)strlen(command)) {
        return -1;
    }
    interest.interests = SXN_IO_READ | SXN_IO_ERROR;
    for (;;) {
        if (sxn_wait_io(&interest, 1, 5000) != 0 || sxn_read(control, &byte, 1) != 1) return -1;
        if (byte == '\n') break;
        if (byte == ' ' && digits > 0 && index == 0) { index = 1; digits = 0; continue; }
        if (byte < '0' || byte > '9' || digits >= 6) return -1;
        values[index] = values[index] * 10u + (unsigned)(byte - '0');
        digits++;
    }
    *first = values[0];
    if (second != 0) *second = values[1];
    return digits > 0 ? 0 : -1;
}

static long fetch(const char* url, int trust_test_root) {
    struct sxnhttp_request request;
    long result;

    memset(&request, 0, sizeof(request));
    request.url = url;
    request.cancel = -1;
    if (trust_test_root) {
        request.trust_pem = g_root;
        request.trust_pem_length = g_root_length;
    }
    memset(&g_response, 0, sizeof(g_response));
    g_response.body = g_body;
    g_response.body_capacity = sizeof(g_body) - 1;
    result = sxnhttp_get(&g_client, &request, &g_response);
    g_body[g_response.body_length] = 0;
    return result;
}

/* `url` fails with `expected`; the line says what came back if not. */
static void expect(const char* url, int trust_test_root, long expected, const char* what) {
    const long result = fetch(url, trust_test_root);
    char line[240];

    if (result != expected) {
        snprintf(line, sizeof(line), "%s: %ld (%s), expected %ld", what, result, g_response.tls_detail,
            expected);
        check(0, line);
    }
}

/* Days since 1970 to a civil date (Howard Hinnant's civil_from_days). */
static void civil(uint64_t seconds, unsigned out[6]) {
    int64_t days = (int64_t)(seconds / 86400u) + 719468;
    const int64_t era = days / 146097;
    const unsigned doe = (unsigned)(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned month = mp < 10 ? mp + 3 : mp - 9;

    out[0] = (unsigned)(yoe + era * 400) + (month <= 2);
    out[1] = month;
    out[2] = doy - (153 * mp + 2) / 5 + 1;
    out[3] = (unsigned)(seconds % 86400u) / 3600u;
    out[4] = (unsigned)(seconds % 3600u) / 60u;
    out[5] = (unsigned)(seconds % 60u);
}

/* /bin/date YYYY MM DD HH MM SS, as the system: 0 when it exited 0. */
static int set_date(const unsigned when[6]) {
    char fields[6][8];
    char* argv[8];
    int status = -1;
    long child;

    argv[0] = "/bin/date";
    for (int index = 0; index < 6; ++index) {
        snprintf(fields[index], sizeof(fields[index]), "%u", when[index]);
        argv[index + 1] = fields[index];
    }
    argv[7] = 0;
    child = sxn_spawn("/bin/date", argv, 7);
    return child > 0 && sxn_waitpid(child, &status) == child && status == 0 ? 0 : -1;
}

static void test_clock(const char* good_url) {
    static const unsigned stale[6] = {2005, 1, 1, 12, 0, 0};
    const uint64_t before = sxn_system_time();
    const uint64_t started = sxn_clock_ns();
    struct sxn_clock_status clock;
    unsigned now[6];

    check(before != 0, "no date to restore");
    if (before == 0) return;
    check(set_date(stale) == 0, "date could not set 2005");
    check(sxn_clock_status(&clock) == 0 && clock.valid == 0, "2005 is a valid date");
    expect(good_url, 1, SXNHTTP_ERROR_TLS_CLOCK, "a stale clock");
    civil(before + (sxn_clock_ns() - started) / 1000000000ull, now);
    check(set_date(now) == 0, "the date could not be restored");
    check(sxn_clock_status(&clock) == 0 && clock.valid == 1, "the restored date is not valid");
}

static void run(unsigned control_port) {
    struct sxn_ipv4_endpoint peer = {0x0a000202u, (uint16_t)control_port, 0};
    struct sxn_ipv4_endpoint dns = {0x0a000202u, 0, 0};
    struct sxn_io_interest interest;
    unsigned http_port = 0, dns_port = 0, tls_port = 0, old_port = 0;
    char url[160], good[160];
    long control = sxn_socket_create(SXN_SOCKET_TCP);
    long result;

    check(control >= 0, "no control socket");
    if (control < 0) return;
    result = sxn_socket_connect(control, &peer);
    interest.handle = (int32_t)control;
    interest.interests = SXN_IO_WRITE | SXN_IO_ERROR;
    check((result == 0 || result == -SAVANXP_EAGAIN) && sxn_wait_io(&interest, 1, 5000) == 0 &&
              sxn_socket_status(control) == 0,
        "no control connection");
    check(control_ask(control, "HTTP\n", &http_port, 0) == 0 && control_ask(control, "DNS\n", &dns_port, 0) == 0 &&
              control_ask(control, "TLS\n", &tls_port, &old_port) == 0,
        "the fixtures' ports");
    sxn_close(control);
    check(tls_port != 0 && old_port != 0, "no TLS fixture: does the host have openssl?");
    if (http_port == 0 || dns_port == 0 || tls_port == 0) return;
    dns.port = (uint16_t)dns_port;
    sxndns_init(&g_resolver);
    sxndns_set_server(&g_resolver, &dns);
    sxnhttp_client_init(&g_client, &g_resolver);

    /* The test root, over plain HTTP, kept in memory only. */
    snprintf(url, sizeof(url), "http://10.0.2.2:%u/tls-root.pem", http_port);
    check(fetch(url, 0) == 0 && g_response.status == 200 && g_response.body_length < sizeof(g_root),
        "the test root");
    g_root_length = g_response.body_length;
    memcpy(g_root, g_body, g_root_length);

    snprintf(good, sizeof(good), "https://good.tls.savanxp:%u/", tls_port);
    expect(good, 1, 0, "a valid certificate");
    check(g_response.status == 200, "a valid certificate's page");
    check(strcmp((const char*)g_body, "secure good.tls.savanxp TLSv1.3 http/1.1") == 0,
        "SNI, TLS 1.3 and http/1.1 as the server saw them");
    check(strcmp(g_response.tls_protocol, "TLSv1.3") == 0 && g_response.tls_cipher[0] != '\0',
        "the response names the protocol and cipher");
    snprintf(url, sizeof(url), "https://10.0.2.2:%u/", tls_port);
    expect(url, 1, 0, "a valid certificate for an address");
    check(strncmp((const char*)g_body, "secure 10.0.2.2 ", 16) == 0, "the address's page");

#define CASE(name, error)                                                            \
    snprintf(url, sizeof(url), "https://" name ".tls.savanxp:%u/", tls_port); \
    expect(url, 1, error, name)
    CASE("wrongname", SXNHTTP_ERROR_TLS_NAME);
    CASE("expired", SXNHTTP_ERROR_TLS_DATE);
    CASE("notyet", SXNHTTP_ERROR_TLS_DATE);
    CASE("selfsigned", SXNHTTP_ERROR_TLS_UNTRUSTED);
    CASE("nochain", SXNHTTP_ERROR_TLS_UNTRUSTED);
    CASE("rogue", SXNHTTP_ERROR_TLS_UNTRUSTED);
#undef CASE
    snprintf(url, sizeof(url), "https://old.tls.savanxp:%u/", old_port);
    expect(url, 1, SXNHTTP_ERROR_TLS_HANDSHAKE, "a TLS 1.1 server");
    /* The system store is Mozilla's: the test root is not in it. */
    expect(good, 0, SXNHTTP_ERROR_TLS_UNTRUSTED, "the test root in the system store");
    /* A body that is not PEM is no store at all. */
    {
        struct sxnhttp_request request;

        memset(&request, 0, sizeof(request));
        request.url = good;
        request.cancel = -1;
        request.trust_pem = "not a certificate";
        request.trust_pem_length = 17;
        memset(&g_response, 0, sizeof(g_response));
        g_response.body = g_body;
        g_response.body_capacity = sizeof(g_body) - 1;
        check(sxnhttp_get(&g_client, &request, &g_response) == SXNHTTP_ERROR_TLS_STORE, "an empty store");
    }
    test_clock(good);
    /* After the clock is back, the same page works again. */
    expect(good, 1, 0, "a valid certificate after the clock is restored");
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "unseeded") == 0) {
        struct sxndns_resolver resolver;
        struct sxn_random_status random;

        check(sxn_random_status(&random) == 0 && random.seeded == 0, "the generator is seeded");
        sxndns_init(&resolver);
        sxnhttp_client_init(&g_client, &resolver);
        expect("https://10.0.2.2:1/", 0, SXNHTTP_ERROR_TLS_ENTROPY, "https:// with no entropy");
    } else if (argc > 1) {
        unsigned port = 0;

        for (const char* digit = argv[1]; *digit >= '0' && *digit <= '9'; ++digit) {
            port = port * 10u + (unsigned)(*digit - '0');
        }
        run(port);
    } else {
        check(0, "usage: httpstest <control port> | unseeded");
    }
    say(g_failures == 0 ? "HTTPS TEST PASS" : "HTTPS TEST FAIL");
    return g_failures == 0 ? 0 : 1;
}
