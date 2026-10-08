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
#include "mbedtls/pk.h"
#include "mbedtls/ecp.h"
#include "mbedtls/sha256.h"
#include "psa/crypto.h"

#include "rt/sys.h"
#include "utls.h"
#include "toyos_tls_entropy.h"

struct utls {
    int fd;
    mbedtls_ssl_context      ssl;
    mbedtls_ssl_config       conf;
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509_crt         ca;    // a client's trust anchors; a server's own chain
    mbedtls_pk_context       key;   // a server's private key
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

static void utls_free(struct utls *t) {
    mbedtls_ssl_free(&t->ssl);
    mbedtls_ssl_config_free(&t->conf);
    mbedtls_ctr_drbg_free(&t->drbg);
    mbedtls_entropy_free(&t->entropy);
    mbedtls_x509_crt_free(&t->ca);
    mbedtls_pk_free(&t->key);
    free(t);
}

void utls_close(struct utls *t) {
    if (!t) return;
    mbedtls_ssl_close_notify(&t->ssl); // best effort; the peer may be gone
    utls_free(t);
}

const char *utls_version(struct utls *t) {
    const char *v = t ? mbedtls_ssl_get_version(&t->ssl) : NULL;
    return v ? v : "unknown";
}

const char *utls_ciphersuite(struct utls *t) {
    const char *c = t ? mbedtls_ssl_get_ciphersuite(&t->ssl) : NULL;
    return c ? c : "unknown";
}

// --- the server side -----------------------------------------------------
//
// A remote desktop server's TLS (VeNCrypt today, RDP later): a key and a
// SELF-SIGNED certificate made once on this machine and kept, so a viewer
// that remembers the fingerprint sees the same one next time. ECDSA P-256:
// generated in milliseconds where RSA-2048 is seconds of bignum work, and
// accepted by every viewer that speaks TLS 1.2 or 1.3.

static int read_file(const char *path, unsigned char **out, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz <= 0 || sz > 65536) { fclose(f); return -1; }
    rewind(f);
    unsigned char *b = malloc((size_t)sz + 1);
    if (!b) { fclose(f); return -1; }
    size_t got = fread(b, 1, (size_t)sz, f);
    fclose(f);
    b[got] = 0;
    *out = b;
    *len = got + 1;   // PEM parsers want the terminator counted
    return 0;
}

static int write_file(const char *path, const unsigned char *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t put = fwrite(data, 1, len, f);
    fclose(f);
    return put == len ? 0 : -1;
}

static void fingerprint(const mbedtls_x509_crt *crt, char *fp, size_t cap) {
    unsigned char h[32];
    mbedtls_sha256(crt->raw.p, crt->raw.len, h, 0);
    size_t o = 0;
    for (int i = 0; i < 32 && o + 3 < cap; i++)
        o += (size_t)snprintf(fp + o, cap - o, i ? ":%02X" : "%02X", h[i]);
}

// THE KEY: loaded, or made and written if it is missing or will not parse.
static int load_or_make_key(const char *key_path, mbedtls_pk_context *key,
                            mbedtls_ctr_drbg_context *drbg, char *err, size_t errcap) {
    unsigned char *kb = 0;
    size_t kl = 0;
    if (read_file(key_path, &kb, &kl) == 0 &&
        mbedtls_pk_parse_key(key, kb, kl, NULL, 0, mbedtls_ctr_drbg_random, drbg) == 0) {
        memset(kb, 0, kl);
        free(kb);
        return 0;
    }
    if (kb) { memset(kb, 0, kl); free(kb); }
    mbedtls_pk_free(key);
    mbedtls_pk_init(key);
    int rc;
    unsigned char *pem = calloc(1, 2048);
    if (!pem) { say(err, errcap, "out of memory"); return -1; }
    if ((rc = mbedtls_pk_setup(key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY))) != 0 ||
        (rc = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(*key),
                                  mbedtls_ctr_drbg_random, drbg)) != 0 ||
        (rc = mbedtls_pk_write_key_pem(key, pem, 2048)) != 0) {
        say_mbed(err, errcap, "making the key", rc);
        free(pem);
        return -1;
    }
    rc = write_file(key_path, pem, strlen((char *)pem));
    memset(pem, 0, 2048);
    free(pem);
    if (rc) say(err, errcap, "cannot write %s", key_path);
    return rc;
}

// THE CERTIFICATE, rebuilt from the key and the names every time and the
// same bytes every time they are the same: the serial is the public key's
// hash, the dates are fixed and the signature is deterministic. Twenty
// years, because a self-signed certificate is trusted by its fingerprint,
// not by a date. Writes `pem` (`cap` bytes).
static int build_cert(mbedtls_pk_context *key, const char *const *names, int nnames,
                      unsigned char *pem, size_t cap, mbedtls_ctr_drbg_context *drbg,
                      char *err, size_t errcap) {
    mbedtls_x509write_cert crt;
    mbedtls_x509write_crt_init(&crt);
    mbedtls_x509_san_list san[UTLS_NAMES_MAX];
    unsigned char ips[UTLS_NAMES_MAX][4];
    int ns = 0;
    for (int i = 0; i < nnames && ns < UTLS_NAMES_MAX; i++) {
        unsigned a, b, c, d;
        char tail;
        memset(&san[ns], 0, sizeof san[ns]);
        if (sscanf(names[i], "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) == 4 &&
            a < 256 && b < 256 && c < 256 && d < 256) {
            ips[ns][0] = (unsigned char)a; ips[ns][1] = (unsigned char)b;
            ips[ns][2] = (unsigned char)c; ips[ns][3] = (unsigned char)d;
            san[ns].node.type = MBEDTLS_X509_SAN_IP_ADDRESS;
            san[ns].node.san.unstructured_name.p = ips[ns];
            san[ns].node.san.unstructured_name.len = 4;
        } else {
            san[ns].node.type = MBEDTLS_X509_SAN_DNS_NAME;
            san[ns].node.san.unstructured_name.p = (unsigned char *)names[i];
            san[ns].node.san.unstructured_name.len = strlen(names[i]);
        }
        if (ns) san[ns - 1].next = &san[ns];
        ns++;
    }
    unsigned char der[256], serial[32];
    int dl = mbedtls_pk_write_pubkey_der(key, der, sizeof der);
    if (dl > 0) mbedtls_sha256(der + sizeof der - dl, (size_t)dl, serial, 0);
    serial[0] &= 0x7F;   // positive, as DER integers must be
    char subject[96];
    snprintf(subject, sizeof subject, "CN=%s,O=toy-os remote desktop", nnames ? names[0] : "toy-os");
    int rc;
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, key);
    mbedtls_x509write_crt_set_issuer_key(&crt, key);
    if ((rc = mbedtls_x509write_crt_set_subject_name(&crt, subject)) != 0 ||
        (rc = mbedtls_x509write_crt_set_issuer_name(&crt, subject)) != 0 ||
        (rc = mbedtls_x509write_crt_set_serial_raw(&crt, serial, 16)) != 0 ||
        (rc = mbedtls_x509write_crt_set_validity(&crt, "20260101000000", "20460101000000")) != 0 ||
        (rc = mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1)) != 0 ||
        (ns && (rc = mbedtls_x509write_crt_set_subject_alternative_name(&crt, san)) != 0) ||
        (rc = mbedtls_x509write_crt_pem(&crt, pem, cap, mbedtls_ctr_drbg_random, drbg)) != 0) {
        say_mbed(err, errcap, "making the certificate", rc);
        mbedtls_x509write_crt_free(&crt);
        return -1;
    }
    mbedtls_x509write_crt_free(&crt);
    return 0;
}

static int seed(mbedtls_entropy_context *e, mbedtls_ctr_drbg_context *d, char *err, size_t cap) {
    char src[64];
    if (toyos_entropy_grade(src, sizeof src) < TOYOS_ENTROPY_GOOD) {
        say(err, cap, "this machine's randomness is %s, too weak to make a key that "
            "stays secret", src[0] ? src : "of unknown quality");
        return -1;
    }
    // PSA FIRST: with MBEDTLS_USE_PSA_CRYPTO the key and certificate
    // writers sign through PSA, and an uninitialised PSA answers
    // "PK - Bad input parameters" rather than saying so. Idempotent.
    if (psa_crypto_init() != PSA_SUCCESS) {
        say(err, cap, "the crypto library would not start");
        return -1;
    }
    const char *pers = "toy-os utls server";
    int rc = mbedtls_ctr_drbg_seed(d, mbedtls_entropy_func, e, (const unsigned char *)pers,
                                   strlen(pers));
    if (rc) { say_mbed(err, cap, "seeding the RNG", rc); return -1; }
    return 0;
}

// Loads what utls_server_identity() made; nothing is made here.
static int load_identity(const char *key_path, const char *crt_path, mbedtls_pk_context *key,
                         mbedtls_x509_crt *crt, mbedtls_ctr_drbg_context *drbg,
                         char *err, size_t errcap) {
    unsigned char *kb = 0, *cb = 0;
    size_t kl = 0, cl = 0;
    int ok = read_file(key_path, &kb, &kl) == 0 && read_file(crt_path, &cb, &cl) == 0 &&
             mbedtls_pk_parse_key(key, kb, kl, NULL, 0, mbedtls_ctr_drbg_random, drbg) == 0 &&
             mbedtls_x509_crt_parse(crt, cb, cl) == 0;
    if (kb) { memset(kb, 0, kl); free(kb); }
    free(cb);
    if (!ok) say(err, errcap, "no usable key and certificate in %s and %s", key_path, crt_path);
    return ok ? 0 : -1;
}

int utls_server_identity(const char *key_path, const char *crt_path,
                         const char *const *names, int nnames,
                         char *fp, size_t fpcap, char *err, size_t errcap) {
    struct {   // on the heap: the entropy pool alone is kilobytes
        mbedtls_entropy_context e;
        mbedtls_ctr_drbg_context d;
        mbedtls_pk_context key;
        mbedtls_x509_crt crt;
        unsigned char pem[4096];
    } *w = calloc(1, sizeof *w);
    if (!w) { say(err, errcap, "out of memory"); return -1; }
    mbedtls_entropy_init(&w->e);
    mbedtls_ctr_drbg_init(&w->d);
    mbedtls_pk_init(&w->key);
    mbedtls_x509_crt_init(&w->crt);
    unsigned char *old = 0;
    size_t oldlen = 0, len;
    int rc = -1;
    if (seed(&w->e, &w->d, err, errcap) || load_or_make_key(key_path, &w->key, &w->d, err, errcap) ||
        build_cert(&w->key, names, nnames, w->pem, sizeof w->pem, &w->d, err, errcap))
        goto out;
    len = strlen((char *)w->pem);
    // Written only when it differs: the same names make the same bytes.
    if ((read_file(crt_path, &old, &oldlen) != 0 || oldlen != len + 1 || memcmp(old, w->pem, len)) &&
        write_file(crt_path, w->pem, len) != 0) {
        say(err, errcap, "cannot write %s", crt_path);
        goto out;
    }
    if (mbedtls_x509_crt_parse(&w->crt, w->pem, len + 1) != 0) {
        say(err, errcap, "the certificate made just now will not parse");
        goto out;
    }
    if (fp && fpcap) fingerprint(&w->crt, fp, fpcap);
    rc = 0;
out:
    free(old);
    mbedtls_x509_crt_free(&w->crt);
    mbedtls_pk_free(&w->key);
    mbedtls_ctr_drbg_free(&w->d);
    mbedtls_entropy_free(&w->e);
    free(w);
    return rc;
}

// THE READ WITH A DEADLINE. A timed receive returns 0 both for "nothing
// came" and "the peer closed"; a timeout returns only once the deadline
// has passed and a close returns at once, so the clock tells them apart
// (remoted's plain reader does the same).
static int bio_recv_timeout(void *ctx, unsigned char *buf, size_t len, uint32_t ms) {
    struct utls *t = ctx;
    if (!ms) return bio_recv(ctx, buf, len);
    unsigned long long t0 = sys_monotonic_ns();
    uint32_t src;
    uint16_t port;
    int64_t n = sys_recvfrom(t->fd, buf, len, &src, &port, ms);
    if (n > 0) return (int)n;
    if (n < 0) return UTLS_ERR_RECV;
    if ((sys_monotonic_ns() - t0) / 1000000ull + 2 >= ms) return MBEDTLS_ERR_SSL_TIMEOUT;
    return 0;
}

struct utls *utls_accept(int fd, const char *key_path, const char *crt_path,
                         char *err, size_t errcap) {
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
    mbedtls_pk_init(&t->key);
    int rc;
    if (seed(&t->entropy, &t->drbg, err, errcap) != 0 ||
        load_identity(key_path, crt_path, &t->key, &t->ca, &t->drbg, err, errcap) != 0)
        goto fail;
    if ((rc = mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_SERVER,
                                          MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
        say_mbed(err, errcap, "configuring TLS", rc);
        goto fail;
    }
    mbedtls_ssl_conf_rng(&t->conf, mbedtls_ctr_drbg_random, &t->drbg);
    // `ca` holds OUR certificate on the server side: the chain we present.
    if ((rc = mbedtls_ssl_conf_own_cert(&t->conf, &t->ca, &t->key)) != 0 ||
        (rc = mbedtls_ssl_setup(&t->ssl, &t->conf)) != 0) {
        say_mbed(err, errcap, "setting up the session", rc);
        goto fail;
    }
    mbedtls_ssl_set_bio(&t->ssl, t, bio_send, bio_recv, bio_recv_timeout);
    // A peer silent for this long mid-handshake is given up on, rather
    // than holding the caller's connection slot for ever.
    mbedtls_ssl_conf_read_timeout(&t->conf, UTLS_ACCEPT_TIMEOUT_MS);
    while ((rc = mbedtls_ssl_handshake(&t->ssl)) != 0) {
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        say_mbed(err, errcap, "TLS handshake", rc);
        goto fail;
    }
    mbedtls_ssl_conf_read_timeout(&t->conf, 0);
    return t;

fail:
    utls_free(t);
    return NULL;
}

long utls_read_timeout(struct utls *t, void *buf, size_t n, int timeout_ms) {
    // Already decrypted and waiting: no need to touch the socket.
    if (mbedtls_ssl_get_bytes_avail(&t->ssl) == 0)
        mbedtls_ssl_conf_read_timeout(&t->conf, timeout_ms > 0 ? (uint32_t)timeout_ms : 0);
    for (;;) {
        int rc = mbedtls_ssl_read(&t->ssl, buf, n);
        mbedtls_ssl_conf_read_timeout(&t->conf, 0);
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (rc == MBEDTLS_ERR_SSL_TIMEOUT) return UTLS_TIMEOUT;
        if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
        return rc;
    }
}
