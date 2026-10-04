// uhttp_download() -- a URL into a file, checked. See uhttp.h.
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "uhttp.h"
#include "uhash.h"

struct dl {
    struct uhttp_download *d;
    int fd;
    int write_failed;
    const struct uhash_alg *sha;
    union uhash_ctx hc;
    unsigned long total;
};

static void dl_headers(void *ctx, int status, unsigned long content_length) {
    struct dl *x = ctx;
    (void)status;
    x->total = content_length;
    if (x->d->progress) x->d->progress(x->d->ctx, 0, content_length);
}

static int dl_sink(void *ctx, const void *data, size_t len) {
    struct dl *x = ctx;
    if (x->d->cancelled && x->d->cancelled(x->d->ctx)) return 1;
    size_t put = 0;
    while (put < len) {
        long w = write(x->fd, (const char *)data + put, len - put);
        if (w <= 0) { x->write_failed = 1; return 1; }
        put += (size_t)w;
    }
    if (x->sha) x->sha->update(&x->hc, data, len);
    x->d->bytes += (unsigned long)len;
    if (x->d->progress) x->d->progress(x->d->ctx, x->d->bytes, x->total);
    return 0;
}

static int dl_fail(struct uhttp_download *d, const char *part, const char *msg) {
    if (part) unlink(part);
    snprintf(d->err, sizeof d->err, "%s", msg);
    return -1;
}

int uhttp_download(struct uhttp_download *d) {
    d->err[0] = 0;
    d->bytes = 0;
    d->status = 0;
    if (!d->url || !d->path) return dl_fail(d, NULL, "nothing to download");

    char part[512];
    if (snprintf(part, sizeof part, "%s.part", d->path) >= (int)sizeof part)
        return dl_fail(d, NULL, "the destination's name is too long");
    struct dl x = { .d = d };
    if (d->sha256) {
        x.sha = uhash_find("sha256");
        if (!x.sha || strlen(d->sha256) != 2 * x.sha->digest_len)
            return dl_fail(d, NULL, "the expected SHA-256 is not 64 hex digits");
        x.sha->init(&x.hc);
    }
    x.fd = open(part, O_WRONLY | O_CREAT | O_TRUNC);
    if (x.fd < 0) {
        snprintf(d->err, sizeof d->err, "cannot write %s", part);
        return -1;
    }

    struct uhttp_request req = {
        .url = d->url,
        .sink = dl_sink,
        .sink_ctx = &x,
        .insecure = d->insecure,
        .allow_weak_entropy = d->allow_weak_entropy,
        .max_redirects = d->max_redirects,
        .on_headers = dl_headers,
    };
    int rc = uhttp_fetch(&req);
    d->status = req.status;
    if (fsync(x.fd) != 0) x.write_failed = 1;
    close(x.fd);

    if (d->cancelled && d->cancelled(d->ctx)) return dl_fail(d, part, "cancelled");
    if (x.write_failed) return dl_fail(d, part, "writing the file failed -- is the disk full?");
    if (rc != 0) return dl_fail(d, part, req.err);
    if (req.status != 200) {
        char m[64];
        snprintf(m, sizeof m, "the server answered %d", req.status);
        return dl_fail(d, part, m);
    }
    if (x.total && d->bytes != x.total) return dl_fail(d, part, "the download was cut short");
    if (x.sha) {
        unsigned char dig[UHASH_DIGEST_MAX];
        char hex[2 * UHASH_DIGEST_MAX + 1];
        x.sha->final(&x.hc, dig);
        uhash_hex(dig, x.sha->digest_len, hex);
        // Case-blind: a published checksum is as often upper case.
        for (unsigned i = 0; i < 2 * x.sha->digest_len; i++) {
            char a = hex[i], b = d->sha256[i];
            if (b >= 'A' && b <= 'F') b = (char)(b - 'A' + 'a');
            if (a != b) return dl_fail(d, part, "the file does not match its known SHA-256");
        }
    }
    if (rename(part, d->path) != 0) return dl_fail(d, part, "cannot move the file into place");
    return 0;
}
