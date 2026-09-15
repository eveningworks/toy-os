// <utls.h> over mbedTLS.
//
// This file is OURS -- nothing in userland/ports/mbedtls/ is edited.
// It is the only place in the tree that includes an mbedtls/ header
// besides toyos_platform.c, which is what keeps the engine swappable.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <dirent.h>
#include <limits.h>
#include <unistd.h>

#include "mbedtls/build_info.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/error.h"

#include "rt/sys.h"
#include "utls.h"
#include "toyos_tls_entropy.h"

struct utls {
    int fd;
    mbedtls_ssl_context      ssl;
    mbedtls_ssl_config       conf;
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509_crt         ca;
};

int utls_available(void) { return 1; }

static void say(char *err, size_t cap, const char *fmt, ...) {
    if (!err || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

// mbedTLS's own sentence for a code, which is why MBEDTLS_ERROR_C is
// enabled: a bare -0x7780 in a user's terminal is not a bug report.
static void say_mbed(char *err, size_t cap, const char *what, int rc) {
    char buf[128];
    mbedtls_strerror(rc, buf, sizeof buf);
    say(err, cap, "%s: %s (-0x%04x)", what, buf, (unsigned)-rc);
}

// mbedtls/net_sockets.h is not vendored -- it is exactly the host-POSIX
// glue this backend replaces -- so its MBEDTLS_ERR_NET_* codes do not
// exist here. Every negative return but the two WANT_ codes is fatal to
// the record layer, so these values matter only for the message.
#define UTLS_ERR_SEND (-0x7f00)
#define UTLS_ERR_RECV (-0x7f01)

// The BIO pair. A short read is reported as a short read and an orderly
// close as 0, which is what read()/write() on a toy-os socket already
// do, so these are near pass-throughs.

static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
    struct utls *t = ctx;
    long n = write(t->fd, buf, len);
    if (n < 0) return UTLS_ERR_SEND;
    return (int)n;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len) {
    struct utls *t = ctx;
    long n = read(t->fd, buf, len);
    if (n < 0) return UTLS_ERR_RECV;
    return (int)n; // 0 is a clean close, which mbedTLS reads as EOF
}

// Loads every PEM file in a directory as a trust anchor.
//
// Ours rather than mbedtls_x509_crt_parse_path(), which wants stat()
// and dirent through MBEDTLS_FS_IO -- a configuration option that also
// drags in the rest of the library's filesystem surface. Reading the
// directory here costs a few lines and keeps the engine unable to touch
// a path at all.
//
// A file that will not parse is SKIPPED, not fatal: one malformed
// anchor in a bundle of a hundred should not cost the other
// ninety-nine. The count is what the caller checks.
static int load_anchors(mbedtls_x509_crt *ca, const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return 0;

    int loaded = 0;
    struct dirent *e;
    // One buffer for the whole walk: PATH_MAX is 4096, which a ring-3
    // stack can afford and -Wframe-larger-than=2048 still refuses.
    char *path = malloc(PATH_MAX);
    if (!path) { closedir(d); return 0; }
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;

        if ((size_t)snprintf(path, PATH_MAX, "%s/%s", dir, e->d_name) >= (size_t)PATH_MAX)
            continue; // a path that does not fit is skipped, never truncated

        FILE *f = fopen(path, "rb");
        if (!f) continue;
        if (fseek(f, 0, SEEK_END) != 0) { fclose(f); continue; }
        long sz = ftell(f);
        if (sz <= 0) { fclose(f); continue; }
        rewind(f);

        // mbedtls_x509_crt_parse() wants PEM NUL-terminated and the
        // length to INCLUDE the terminator. Getting that wrong parses
        // zero certificates and still returns success.
        char *pem = malloc((size_t)sz + 1);
        if (!pem) { fclose(f); continue; }
        size_t got = fread(pem, 1, (size_t)sz, f);
        fclose(f);
        pem[got] = '\0';

        if (got && mbedtls_x509_crt_parse(ca, (const unsigned char *)pem, got + 1) >= 0)
            loaded++;
        free(pem);
    }
    free(path);
    closedir(d);
    return loaded;
}

struct utls *utls_connect(int fd, const struct utls_config *cfg,
                          char *err, size_t errcap) {
    if (!cfg || !cfg->hostname || !cfg->hostname[0]) {
        say(err, errcap, "no hostname given");
        return NULL;
    }

    // The entropy gate, before anything is allocated. Under QEMU with
    // no virtio-rng the source is TSC jitter -- software timing
    // software -- and a key from it is not secret. This refuses rather
    // than warning because a warning printed above a page of HTML is a
    // warning nobody reads.
    char src[64];
    enum toyos_entropy_grade grade = toyos_entropy_grade(src, sizeof src);
    if (grade < TOYOS_ENTROPY_GOOD && cfg->entropy != UTLS_ENTROPY_ALLOW_WEAK) {
        say(err, errcap,
            "this machine's randomness is %s, which is too weak to key a "
            "connection with -- give the guest a virtio-rng device, or "
            "accept it explicitly",
            src[0] ? src : "of unknown quality");
        return NULL;
    }

    struct utls *t = calloc(1, sizeof *t);
    if (!t) {
        say(err, errcap, "out of memory");
        return NULL;
    }
    t->fd = fd;
    mbedtls_ssl_init(&t->ssl);
    mbedtls_ssl_config_init(&t->conf);
    mbedtls_entropy_init(&t->entropy);
    mbedtls_ctr_drbg_init(&t->drbg);
    mbedtls_x509_crt_init(&t->ca);

    int rc;
    const char *pers = "toy-os utls";
    if ((rc = mbedtls_ctr_drbg_seed(&t->drbg, mbedtls_entropy_func, &t->entropy,
                                    (const unsigned char *)pers, strlen(pers))) != 0) {
        say_mbed(err, errcap, "seeding the RNG", rc);
        goto fail;
    }

    if ((rc = mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_CLIENT,
                                          MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
        say_mbed(err, errcap, "configuring TLS", rc);
        goto fail;
    }

    if (cfg->insecure) {
        mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_NONE);
    } else {
        const char *dir = cfg->ca_dir ? cfg->ca_dir : UTLS_DEFAULT_CA_DIR;
        int n = load_anchors(&t->ca, dir);
        if (n == 0) {
            // Distinguished from a verification FAILURE on purpose:
            // an empty trust store is a machine that was never told
            // whom to trust, and the fix is different.
            say(err, errcap,
                "no trust anchors in %s, so no certificate can be verified "
                "(build with EXTRAS=1 for the Mozilla bundle, or pass -k)", dir);
            goto fail;
        }
        mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&t->conf, &t->ca, NULL);
    }

    mbedtls_ssl_conf_rng(&t->conf, mbedtls_ctr_drbg_random, &t->drbg);

    if ((rc = mbedtls_ssl_setup(&t->ssl, &t->conf)) != 0) {
        say_mbed(err, errcap, "setting up the session", rc);
        goto fail;
    }

    // SNI, and the name verification uses the same string.
    if ((rc = mbedtls_ssl_set_hostname(&t->ssl, cfg->hostname)) != 0) {
        say_mbed(err, errcap, "setting the server name", rc);
        goto fail;
    }

    mbedtls_ssl_set_bio(&t->ssl, t, bio_send, bio_recv, NULL);

    while ((rc = mbedtls_ssl_handshake(&t->ssl)) != 0) {
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue; // the socket blocks, so this is progress, not a spin
        if (rc == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
            char why[256];
            uint32_t flags = mbedtls_ssl_get_verify_result(&t->ssl);
            mbedtls_x509_crt_verify_info(why, sizeof why, "", flags);
            char *nl = strchr(why, '\n');
            if (nl) *nl = '\0'; // the first reason is the useful one
            say(err, errcap, "certificate verification failed: %s", why);
        } else {
            say_mbed(err, errcap, "TLS handshake", rc);
        }
        goto fail;
    }

    return t;

fail:
    mbedtls_ssl_free(&t->ssl);
    mbedtls_ssl_config_free(&t->conf);
    mbedtls_ctr_drbg_free(&t->drbg);
    mbedtls_entropy_free(&t->entropy);
    mbedtls_x509_crt_free(&t->ca);
    free(t);
    return NULL;
}

long utls_read(struct utls *t, void *buf, size_t n) {
    for (;;) {
        int rc = mbedtls_ssl_read(&t->ssl, buf, n);
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        // A peer that closed cleanly is end of stream, not an error --
        // and for HTTP/1.0 with `Connection: close` it is how the body
        // ends, so folding it into the error path would break every
        // fetch.
        if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
        return rc;
    }
}

long utls_write(struct utls *t, const void *buf, size_t n) {
    for (;;) {
        int rc = mbedtls_ssl_write(&t->ssl, buf, n);
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        return rc;
    }
}

void utls_close(struct utls *t) {
    if (!t) return;
    mbedtls_ssl_close_notify(&t->ssl); // best effort; the peer may be gone
    mbedtls_ssl_free(&t->ssl);
    mbedtls_ssl_config_free(&t->conf);
    mbedtls_ctr_drbg_free(&t->drbg);
    mbedtls_entropy_free(&t->entropy);
    mbedtls_x509_crt_free(&t->ca);
    free(t);
}

const char *utls_version(struct utls *t) {
    const char *v = t ? mbedtls_ssl_get_version(&t->ssl) : NULL;
    return v ? v : "unknown";
}

const char *utls_ciphersuite(struct utls *t) {
    const char *c = t ? mbedtls_ssl_get_ciphersuite(&t->ssl) : NULL;
    return c ? c : "unknown";
}
