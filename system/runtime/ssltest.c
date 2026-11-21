/* The OpenSSL port in a running system (ports/openssl/README.md): what N5 of
 * docs/roadmaps/open/WEBVIEWER_ROADMAP.md needs before HTTPS is turned on.
 * One source, two programs: /system/sunssltest.sxe (SUN, SSLTEST_NATIVE) and
 * /bin/ssltest (POSIX), both mapping /lib/libcrypto.so.0.4 and
 * /lib/libssl.so.0.4. Nothing here touches the network:
 *
 * - the library is the pinned 3.5.3, and SHA-256 gives its known answer;
 * - RAND_bytes works: OpenSSL seeds from getentropy(), which the runtime
 *   puts on the kernel generator in either subsystem;
 * - /system/ssl/cert.pem, read by the program and handed over in memory,
 *   is the pinned store's 118 roots, nearly all valid today by time();
 * - a root verifies against the store, is refused as self-signed without
 *   it, and is refused as expired at a date past its end;
 * - a TLS client offers TLS 1.2 and 1.3 with SNI and ALPN http/1.1: its
 *   ClientHello, written to a memory BIO, carries both.
 *
 * Each failure is a line; "SSL TEST PASS" or "SSL TEST FAIL" (POSIX), "SUN
 * SSL TEST PASS" or "... FAIL" (SUN) ends it, and the exit status says the
 * same. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>

#ifdef SSLTEST_NATIVE
#include "sxn/sxn.h"
#define SSLTEST_NAME "SUN SSL TEST"
#else
#define SSLTEST_NAME "SSL TEST"
#endif

#define CA_FILE "/system/ssl/cert.pem"
#define CA_ROOTS 118
#define CA_FILE_MAX (512u * 1024u)

static int g_failures;

static void say(const char* line) {
#ifdef SSLTEST_NATIVE
    (void)sxn_log(line);
#else
    printf("%s\n", line);
#endif
}

static void check(int ok, const char* what) {
    char line[200];

    if (!ok) {
        g_failures++;
        snprintf(line, sizeof(line), "ssltest: FAILED %s", what);
        say(line);
    }
}

/* The whole file, through the program's own subsystem. */
static unsigned char* read_file(const char* path, size_t* length) {
    unsigned char* data = malloc(CA_FILE_MAX);
    size_t used = 0;

    *length = 0;
    if (data == 0) {
        return 0;
    }
#ifdef SSLTEST_NATIVE
    {
        const long handle = sxn_open_file(path, SXN_FILE_READ);
        long got;

        if (handle < 0) {
            free(data);
            return 0;
        }
        while (used < CA_FILE_MAX && (got = sxn_read(handle, data + used, CA_FILE_MAX - used)) > 0) {
            used += (size_t)got;
        }
        (void)sxn_close(handle);
    }
#else
    {
        FILE* file = fopen(path, "rb");
        size_t got;

        if (file == 0) {
            free(data);
            return 0;
        }
        while (used < CA_FILE_MAX && (got = fread(data + used, 1, CA_FILE_MAX - used, file)) > 0) {
            used += got;
        }
        fclose(file);
    }
#endif
    *length = used;
    return data;
}

static void test_library(void) {
    static const unsigned char abc_sha256[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
    };
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_length = 0;
    unsigned char first[32], second[32];
    const char* version = OpenSSL_version(OPENSSL_VERSION);

    check(version != 0 && strncmp(version, "OpenSSL 3.5.3 ", 14) == 0, "the library is not OpenSSL 3.5.3");
    check(EVP_Digest("abc", 3, digest, &digest_length, EVP_sha256(), 0) == 1 && digest_length == 32 &&
              memcmp(digest, abc_sha256, 32) == 0,
        "SHA-256(abc)");
    memset(first, 0, sizeof(first));
    memset(second, 0, sizeof(second));
    check(RAND_bytes(first, sizeof(first)) == 1 && RAND_bytes(second, sizeof(second)) == 1,
        "RAND_bytes: no entropy");
    check(memcmp(first, second, sizeof(first)) != 0, "RAND_bytes repeats");
}

/* The store, and the first root in it (one reference held). */
static X509_STORE* load_store(X509** root_out) {
    size_t length = 0;
    unsigned char* data = read_file(CA_FILE, &length);
    X509_STORE* store = X509_STORE_new();
    BIO* bio;
    X509* cert;
    int count = 0, current = 0;

    *root_out = 0;
    check(data != 0 && length > 0 && length < CA_FILE_MAX, "cannot read " CA_FILE);
    if (data == 0 || store == 0) {
        free(data);
        return store;
    }
    bio = BIO_new_mem_buf(data, (int)length);
    while (bio != 0 && (cert = PEM_read_bio_X509(bio, 0, 0, 0)) != 0) {
        count += X509_STORE_add_cert(store, cert) == 1;
        current += X509_cmp_current_time(X509_get0_notBefore(cert)) < 0 &&
                   X509_cmp_current_time(X509_get0_notAfter(cert)) > 0;
        if (*root_out == 0) {
            *root_out = cert;
        } else {
            X509_free(cert);
        }
    }
    ERR_clear_error(); /* the end of the data reads as "no start line" */
    BIO_free(bio);
    free(data);
    check(count == CA_ROOTS, "the store does not hold the pinned 118 roots");
    check(current >= CA_ROOTS - 8, "most roots are not valid by time()");
    return store;
}

static int verify(X509_STORE* store, X509* cert, int64_t at, int* error) {
    X509_STORE_CTX* context = X509_STORE_CTX_new();
    int result = -1;

    *error = -1;
    if (context != 0 && X509_STORE_CTX_init(context, store, cert, 0) == 1) {
        if (at != 0) {
            X509_VERIFY_PARAM_set_time(X509_STORE_CTX_get0_param(context), (time_t)at);
        }
        result = X509_verify_cert(context);
        *error = X509_STORE_CTX_get_error(context);
    }
    X509_STORE_CTX_free(context);
    return result;
}

static void test_verification(X509_STORE* store, X509* root) {
    X509_STORE* empty = X509_STORE_new();
    int error;

    if (root == 0) {
        check(0, "no root to verify");
        X509_STORE_free(empty);
        return;
    }
    check(verify(store, root, 0, &error) == 1 && error == X509_V_OK, "a root does not verify against the store");
    check(verify(empty, root, 0, &error) == 0 && error == X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT,
        "a root verifies without the store");
    /* 2099-12-31, past every root's end. */
    check(verify(store, root, 4102358400ll, &error) == 0 && error == X509_V_ERR_CERT_HAS_EXPIRED,
        "a root is not expired at 2099-12-31");
    X509_STORE_free(empty);
}

static int contains(const unsigned char* data, long length, const char* text) {
    const long needle = (long)strlen(text);

    for (long at = 0; at + needle <= length; ++at) {
        if (memcmp(data + at, text, (size_t)needle) == 0) {
            return 1;
        }
    }
    return 0;
}

static void test_client_hello(X509_STORE* store) {
    static const unsigned char alpn[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
    SSL_CTX* context = SSL_CTX_new(TLS_client_method());
    SSL* ssl;
    BIO* in;
    BIO* out;
    unsigned char* hello = 0;
    long length;

    check(context != 0, "SSL_CTX_new");
    if (context == 0) {
        return;
    }
    check(SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION) == 1, "minimum TLS 1.2");
    check(SSL_CTX_set_alpn_protos(context, alpn, sizeof(alpn)) == 0, "ALPN http/1.1");
    if (X509_STORE_up_ref(store) == 1) {
        SSL_CTX_set_cert_store(context, store);
    }
    SSL_CTX_set_verify(context, SSL_VERIFY_PEER, 0);
    ssl = SSL_new(context);
    in = BIO_new(BIO_s_mem());
    out = BIO_new(BIO_s_mem());
    check(ssl != 0 && in != 0 && out != 0, "SSL_new and its memory BIOs");
    if (ssl == 0 || in == 0 || out == 0) {
        BIO_free(in);
        BIO_free(out);
        SSL_free(ssl);
        SSL_CTX_free(context);
        return;
    }
    SSL_set_bio(ssl, in, out);
    check(SSL_set_tlsext_host_name(ssl, "savanxp.test") == 1 && SSL_set1_host(ssl, "savanxp.test") == 1,
        "SNI and the expected name");
    check(SSL_connect(ssl) == -1 && SSL_get_error(ssl, -1) == SSL_ERROR_WANT_READ,
        "the handshake does not wait for the server");
    length = BIO_get_mem_data(out, (char**)&hello);
    /* A handshake record (22), its ClientHello (1). */
    check(length > 100 && hello[0] == 0x16 && hello[1] == 0x03 && hello[5] == 0x01, "no ClientHello written");
    check(length > 0 && contains(hello, length, "savanxp.test"), "the ClientHello carries no SNI");
    check(length > 0 && contains(hello, length, "http/1.1"), "the ClientHello offers no http/1.1");
    SSL_free(ssl);
    SSL_CTX_free(context);
}

int main(void) {
    X509* root = 0;
    X509_STORE* store;

    test_library();
    store = load_store(&root);
    if (store != 0) {
        test_verification(store, root);
        test_client_hello(store);
    } else {
        check(0, "X509_STORE_new");
    }
    X509_free(root);
    X509_STORE_free(store);
    say(g_failures == 0 ? SSLTEST_NAME " PASS" : SSLTEST_NAME " FAIL");
    return g_failures == 0 ? 0 : 1;
}
