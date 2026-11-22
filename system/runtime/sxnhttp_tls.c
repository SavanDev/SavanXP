/* sxnhttp's TLS through the OpenSSL port (docs/net/HTTP.md, "HTTPS";
 * ports/openssl/README.md). A program that links this file and DEPENDS on
 * libssl and libcrypto opens https:// URLs; sxnhttp.c moves the bytes.
 *
 * What is checked, and what each failure is called:
 *
 * - before anything: the kernel generator is seeded (TLS_ENTROPY) and the
 *   date can be believed (TLS_CLOCK);
 * - the trust store: SXNHTTP_TRUST_STORE, read once per process, or the
 *   request's own PEM; empty or unreadable is TLS_STORE;
 * - the chain to a trusted root (TLS_UNTRUSTED), the name or address
 *   (TLS_NAME), the dates (TLS_DATE): OpenSSL's verifier, with peer
 *   verification on; anything else in the handshake is TLS_HANDSHAKE;
 * - TLS 1.2 or 1.3 only, SNI for names (never for addresses, RFC 6066), ALPN
 *   offering only http/1.1, and a server that picks another is refused.
 *
 * There is no switch that lets a failed certificate through.
 *
 * OpenSSL is built without threads, and its state (the error queue, the
 * random generator) is the process's: every call into it holds g_lock, so
 * the windows of one program may load in parallel. No call blocks inside the
 * lock -- the bytes go through memory. */

#include "sxnhttp_tls.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>

#include "sxn/sxn.h"

/* The system store's file is a few hundred KiB at most. */
#define TRUST_FILE_MAX (1024u * 1024u)

struct tls_session
{
    SSL_CTX *context;
    SSL *ssl;
    BIO *in;  /* the peer's bytes, for OpenSSL */
    BIO *out; /* OpenSSL's bytes, for the peer */
};

/* 0 free, 1 held, 2 held with someone waiting. */
static volatile uint32_t g_lock;

static void lock(void)
{
    uint32_t seen = 0;

    if (__atomic_compare_exchange_n(&g_lock, &seen, 1u, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    {
        return;
    }
    while (__atomic_exchange_n(&g_lock, 2u, __ATOMIC_ACQUIRE) != 0)
    {
        (void)sxn_wait_address(&g_lock, 2u, sizeof(g_lock), -1);
    }
}

static void unlock(void)
{
    if (__atomic_exchange_n(&g_lock, 0u, __ATOMIC_RELEASE) == 2u)
    {
        (void)sxn_wake_address(&g_lock, 1);
    }
}

/* The system store, loaded on first use and kept. */
static X509_STORE *g_system_store;

/* The roots in `pem` added to `store`: how many. */
static int add_roots(X509_STORE *store, const char *pem, size_t length)
{
    BIO *bio = BIO_new_mem_buf(pem, (int)length);
    X509 *cert;
    int count = 0;

    while (bio != 0 && (cert = PEM_read_bio_X509(bio, 0, 0, 0)) != 0)
    {
        count += X509_STORE_add_cert(store, cert) == 1;
        X509_free(cert);
    }
    BIO_free(bio);
    ERR_clear_error(); /* the end of the data reads as "no start line" */
    return count;
}

static X509_STORE *system_store(void)
{
    unsigned char *data;
    size_t used = 0;
    long handle;
    long got;
    X509_STORE *store;

    if (g_system_store != 0)
    {
        return g_system_store;
    }
    handle = sxn_open_file(SXNHTTP_TRUST_STORE, SXN_FILE_READ);
    if (handle < 0)
    {
        return 0;
    }
    data = malloc(TRUST_FILE_MAX);
    if (data == 0)
    {
        sxn_close(handle);
        return 0;
    }
    while (used < TRUST_FILE_MAX && (got = sxn_read(handle, data + used, TRUST_FILE_MAX - used)) > 0)
    {
        used += (size_t)got;
    }
    sxn_close(handle);
    store = X509_STORE_new();
    if (store != 0 && (used == TRUST_FILE_MAX || add_roots(store, (const char *)data, used) == 0))
    {
        X509_STORE_free(store);
        store = 0;
    }
    free(data);
    g_system_store = store;
    return store;
}

/* "10.0.2.2": an address, checked as one and never sent as SNI. */
static int is_ipv4_literal(const char *host)
{
    int dots = 0;

    for (; *host != '\0'; ++host)
    {
        if (*host == '.')
        {
            dots += 1;
        }
        else if (*host < '0' || *host > '9')
        {
            return 0;
        }
    }
    return dots == 3;
}

static long tls_ready(void)
{
    struct sxn_random_status random;
    struct sxn_clock_status clock;

    if (sxn_random_status(&random) != 0 || random.seeded == 0)
    {
        return SXNHTTP_ERROR_TLS_ENTROPY;
    }
    if (sxn_clock_status(&clock) != 0 || clock.valid == 0)
    {
        return SXNHTTP_ERROR_TLS_CLOCK;
    }
    return 0;
}

static void session_free(struct tls_session *session)
{
    if (session->ssl != 0)
    {
        SSL_free(session->ssl); /* and both BIOs, which it owns */
    }
    else
    {
        BIO_free(session->in);
        BIO_free(session->out);
    }
    SSL_CTX_free(session->context);
    free(session);
}

static long open_locked(const char *host, const char *trust_pem, size_t trust_pem_length,
                        struct tls_session *session)
{
    static const unsigned char alpn[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
    X509_VERIFY_PARAM *param;

    session->context = SSL_CTX_new(TLS_client_method());
    if (session->context == 0)
    {
        /* What fails here is the generator OpenSSL seeds from the kernel's. */
        return SXNHTTP_ERROR_TLS_ENTROPY;
    }
    if (SSL_CTX_set_min_proto_version(session->context, TLS1_2_VERSION) != 1 ||
        SSL_CTX_set_alpn_protos(session->context, alpn, sizeof(alpn)) != 0)
    {
        return SXNHTTP_ERROR_TLS_HANDSHAKE;
    }
    if (trust_pem != 0)
    {
        X509_STORE *store = X509_STORE_new();

        if (store == 0 || add_roots(store, trust_pem, trust_pem_length) == 0)
        {
            X509_STORE_free(store);
            return SXNHTTP_ERROR_TLS_STORE;
        }
        SSL_CTX_set_cert_store(session->context, store);
    }
    else
    {
        X509_STORE *store = system_store();

        if (store == 0)
        {
            return SXNHTTP_ERROR_TLS_STORE;
        }
        SSL_CTX_set1_cert_store(session->context, store);
    }
    SSL_CTX_set_verify(session->context, SSL_VERIFY_PEER, 0);

    session->in = BIO_new(BIO_s_mem());
    session->out = BIO_new(BIO_s_mem());
    session->ssl = SSL_new(session->context);
    if (session->in == 0 || session->out == 0 || session->ssl == 0)
    {
        return SXNHTTP_ERROR_TLS_HANDSHAKE;
    }
    SSL_set_bio(session->ssl, session->in, session->out);
    param = SSL_get0_param(session->ssl);
    X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    if (is_ipv4_literal(host))
    {
        if (X509_VERIFY_PARAM_set1_ip_asc(param, host) != 1)
        {
            return SXNHTTP_ERROR_TLS_HANDSHAKE;
        }
    }
    else if (SSL_set_tlsext_host_name(session->ssl, host) != 1 || SSL_set1_host(session->ssl, host) != 1)
    {
        return SXNHTTP_ERROR_TLS_HANDSHAKE;
    }
    SSL_set_connect_state(session->ssl);
    return 0;
}

static long tls_open(const char *host, const char *trust_pem, size_t trust_pem_length, void **out)
{
    struct tls_session *session = calloc(1, sizeof(*session));
    long result;

    *out = 0;
    if (session == 0)
    {
        return SXNHTTP_ERROR_NETWORK;
    }
    lock();
    ERR_clear_error();
    result = open_locked(host, trust_pem, trust_pem_length, session);
    if (result != 0)
    {
        session_free(session);
    }
    unlock();
    if (result == 0)
    {
        *out = session;
    }
    return result;
}

static void tls_close(void *opaque)
{
    lock();
    session_free(opaque);
    unlock();
}

static long tls_put(void *opaque, const void *data, size_t length)
{
    struct tls_session *session = opaque;
    long result = 0;

    lock();
    while (length > 0 && result == 0)
    {
        const int piece = length > 65536u ? 65536 : (int)length;
        const int wrote = BIO_write(session->in, data, piece);

        if (wrote != piece)
        {
            result = SXNHTTP_ERROR_NETWORK;
            break;
        }
        data = (const unsigned char *)data + piece;
        length -= (size_t)piece;
    }
    unlock();
    return result;
}

static size_t tls_take(void *opaque, void *out, size_t capacity)
{
    struct tls_session *session = opaque;
    int got;

    lock();
    got = BIO_pending(session->out) > 0
        ? BIO_read(session->out, out, capacity > 0x7fffffffu ? 0x7fffffff : (int)capacity)
        : 0;
    unlock();
    return got > 0 ? (size_t)got : 0;
}

/* The verifier's verdict, in sxnhttp's terms. */
static long verify_error(long code)
{
    switch (code)
    {
    case X509_V_ERR_CERT_HAS_EXPIRED:
    case X509_V_ERR_CERT_NOT_YET_VALID:
    case X509_V_ERR_ERROR_IN_CERT_NOT_BEFORE_FIELD:
    case X509_V_ERR_ERROR_IN_CERT_NOT_AFTER_FIELD:
        return SXNHTTP_ERROR_TLS_DATE;
    case X509_V_ERR_HOSTNAME_MISMATCH:
    case X509_V_ERR_IP_ADDRESS_MISMATCH:
        return SXNHTTP_ERROR_TLS_NAME;
    default:
        return SXNHTTP_ERROR_TLS_UNTRUSTED;
    }
}

static long tls_handshake(void *opaque, char *detail, size_t detail_capacity)
{
    struct tls_session *session = opaque;
    long result;
    int done;

    lock();
    ERR_clear_error();
    done = SSL_do_handshake(session->ssl);
    if (done == 1)
    {
        const unsigned char *chosen = 0;
        unsigned int chosen_length = 0;

        SSL_get0_alpn_selected(session->ssl, &chosen, &chosen_length);
        result = chosen_length == 0 || (chosen_length == 8 && memcmp(chosen, "http/1.1", 8) == 0)
            ? 0
            : SXNHTTP_ERROR_TLS_HANDSHAKE;
        if (result != 0)
        {
            snprintf(detail, detail_capacity, "the server chose a protocol other than http/1.1");
        }
    }
    else
    {
        const int error = SSL_get_error(session->ssl, done);
        const long verdict = SSL_get_verify_result(session->ssl);

        if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE)
        {
            result = SXNHTTP_TLS_WANT_INPUT;
        }
        else if (verdict != X509_V_OK)
        {
            result = verify_error(verdict);
            snprintf(detail, detail_capacity, "%s", X509_verify_cert_error_string(verdict));
        }
        else
        {
            const char *reason = ERR_reason_error_string(ERR_peek_error());

            result = SXNHTTP_ERROR_TLS_HANDSHAKE;
            snprintf(detail, detail_capacity, "%s", reason != 0 ? reason : "the handshake failed");
        }
    }
    unlock();
    return result;
}

static long tls_write(void *opaque, const void *data, size_t length)
{
    struct tls_session *session = opaque;
    size_t written = 0;
    int ok;

    lock();
    ERR_clear_error();
    ok = SSL_write_ex(session->ssl, data, length, &written);
    unlock();
    return ok == 1 && written == length ? (long)length : SXNHTTP_ERROR_TLS_HANDSHAKE;
}

static long tls_read(void *opaque, void *out, size_t capacity)
{
    struct tls_session *session = opaque;
    size_t got = 0;
    long result;

    lock();
    ERR_clear_error();
    if (SSL_read_ex(session->ssl, out, capacity, &got) == 1)
    {
        result = (long)got;
    }
    else
    {
        const int error = SSL_get_error(session->ssl, 0);

        result = error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE ? SXNHTTP_TLS_WANT_INPUT
               : error == SSL_ERROR_ZERO_RETURN                                ? 0
                                                                               : SXNHTTP_ERROR_TLS_HANDSHAKE;
    }
    unlock();
    return result;
}

/* Many servers close without a close_notify. The response's own framing
 * decides: a Content-Length or chunked body that is complete is complete,
 * and one that is not is SXNHTTP_ERROR_TRUNCATED from the parser. */
static long tls_end_of_input(void *opaque)
{
    (void)opaque;
    return 0;
}

static void tls_describe(void *opaque, char *protocol, size_t protocol_capacity, char *cipher,
                         size_t cipher_capacity)
{
    struct tls_session *session = opaque;

    lock();
    snprintf(protocol, protocol_capacity, "%s", SSL_get_version(session->ssl));
    snprintf(cipher, cipher_capacity, "%s", SSL_get_cipher_name(session->ssl));
    unlock();
}

static const struct sxnhttp_tls_backend g_backend = {
    tls_ready, tls_open, tls_close, tls_put, tls_take, tls_handshake,
    tls_write, tls_read, tls_end_of_input, tls_describe,
};

const struct sxnhttp_tls_backend *sxnhttp_tls_backend(void)
{
    return &g_backend;
}
