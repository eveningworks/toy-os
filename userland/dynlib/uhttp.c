// <uhttp.h> -- the HTTP/1.0 client, over a plain socket or a TLS session.
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rt/sys.h"
#include "uhttp.h"
#include "lib/uinflate.h"
#include "utls.h"
#include "lib/uresolv.h"

int uhttp_tls_available(void) { return utls_available(); }

// One transport, two implementations, chosen by the scheme. Everything
// above this point in the file is unaware of which it has.
struct transport {
    int          fd;
    struct utls *tls; // NULL for plain http
};

static long tp_write(struct transport *t, const void *buf, size_t n) {
    return t->tls ? utls_write(t->tls, buf, n) : write(t->fd, buf, n);
}

static long tp_read(struct transport *t, void *buf, size_t n) {
    return t->tls ? utls_read(t->tls, buf, n) : read(t->fd, buf, n);
}

// A stream write may be short -- the kernel's send buffer is 4 KiB and
// a TLS record is its own size -- so every write is a loop. This is the
// bug that made wget's request send correct by accident when it was one
// call and a small request.
static int write_all(struct transport *t, const void *buf, size_t n) {
    const char *p = buf;
    while (n) {
        long w = tp_write(t, p, n);
        if (w <= 0) return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

int uhttp_parse_url(const char *url, struct uhttp_url *out) {
    if (!url || !out) return -1;
    memset(out, 0, sizeof *out);

    const char *p = url;
    if (!strncmp(p, "https://", 8)) {
        out->tls = 1;
        out->port = 443;
        out->scheme_given = 1;
        p += 8;
    } else if (!strncmp(p, "http://", 7)) {
        out->tls = 0;
        out->port = 80;
        out->scheme_given = 1;
        p += 7;
    } else if (strstr(p, "://")) {
        return -1; // a scheme this does not speak, not a bare host
    } else {
        // NO SCHEME: guess https, and let the fetch fall back. Browsers
        // upgrade a typed address this way; GNU wget and curl both
        // default to plaintext instead, which is the older answer to a
        // question the web has since settled.
        out->tls = 1;
        out->port = 443;
        out->scheme_given = 0;
    }

    // host[:port], up to the first '/'. An IPv6 literal in brackets is
    // not accepted, because nothing in this stack speaks IPv6 and
    // parsing an address the socket layer would refuse is worse than
    // refusing it here.
    const char *slash = strchr(p, '/');
    const char *hostend = slash ? slash : p + strlen(p);
    const char *colon = memchr(p, ':', (size_t)(hostend - p));

    size_t hostlen = (size_t)((colon ? colon : hostend) - p);
    if (hostlen == 0 || hostlen >= sizeof out->host) return -1;
    memcpy(out->host, p, hostlen);
    out->host[hostlen] = '\0';

    if (colon) {
        unsigned port = 0;
        for (const char *q = colon + 1; q < hostend; q++) {
            if (*q < '0' || *q > '9') return -1;
            port = port * 10 + (unsigned)(*q - '0');
            if (port > 65535) return -1;
        }
        if (port == 0) return -1;
        out->port = (uint16_t)port;
        out->port_given = 1;

        // AN EXPLICIT PORT CANCELS THE https GUESS, unless it is 443.
        // Chrome upgrades a typed address only on the default port, and
        // the reason is the same here: `host:8080` is overwhelmingly a
        // plain server, and guessing https for it fails at the
        // handshake -- which must NOT fall back, since that is
        // indistinguishable from a server whose certificate is wrong.
        // Somebody who wants https on an odd port can say so.
        if (!out->scheme_given) out->tls = (out->port == 443);
    }

    const char *path = slash ? slash : "/";
    if (strlen(path) >= sizeof out->path) return -1; // refuse, never truncate
    strlcpy(out->path, path, sizeof out->path);
    return 0;
}

// Finds the blank line ending the headers.
//
// It keeps the last three bytes seen ACROSS calls, because "\r\n\r\n"
// can straddle any two reads and a socket read here returns at most
// SYS_NET_STREAM_READ_MAX bytes whatever buffer it is handed -- so on a
// response with more than about 1.4 KB of headers the boundary lands
// mid-sequence routinely rather than rarely.
struct hdr_scan {
    char tail[3]; // the three bytes before the one being examined
    int  have;    // how many of them are real yet
    int  done;
    // The header LINE being assembled, so a name can be recognised
    // across a read boundary -- the same straddling hazard the blank
    // line has, and the reason this is state rather than a scan of
    // `buf`. A line longer than this is truncated for MATCHING only:
    // the names looked for here are short, and the body offset comes
    // from the blank-line walk above regardless.
    char line[160];
    int  line_len;
    unsigned long content_length;   // 0 = the server did not say
    int gzip;                       // Content-Encoding: gzip
    // Location, kept WHOLE when a redirect may be followed: unlike the
    // names above it is a value of any length, so it is gathered beside
    // the line rather than in it. Heap; the scan's owner frees it.
    int want_loc, in_loc, loc_over;
    char *loc;
    size_t loc_len;
};

// Longer than any redirect a real server sends; a longer one is refused.
#define UHTTP_LOCATION_MAX 4096

static void loc_add(struct hdr_scan *s, char c) {
    if (s->loc_over) return;
    if (!s->loc && !(s->loc = malloc(UHTTP_LOCATION_MAX))) { s->loc_over = 1; return; }
    if (s->loc_len + 1 >= UHTTP_LOCATION_MAX) { s->loc_over = 1; return; }
    s->loc[s->loc_len++] = c;
    s->loc[s->loc_len] = 0;
}

static int hdr_prefix(const char *line, const char *name) {
    while (*name) {
        char a = *line++, b = *name++;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

// One completed header line. Two are read: Content-Length, which a
// progress meter needs, and Content-Encoding, because a gzipped body has
// to be inflated before it reaches the caller's sink. A header nobody
// consumes is a parser to keep correct for nothing.
static void hdr_line(struct hdr_scan *s) {
    s->line[s->line_len] = 0;
    if (hdr_prefix(s->line, "content-encoding:")) {
        const char *v = s->line + sizeof "content-encoding:" - 1;
        while (*v == ' ' || *v == '\t') v++;
        // `gzip` and the older `x-gzip` are the same thing. `deflate` is
        // NOT accepted: the name is a decades-old muddle -- some servers
        // send a zlib stream and some a bare one -- and we never ask for
        // it, so a server sending it anyway is better refused than
        // guessed at.
        s->gzip = hdr_prefix(v, "gzip") || hdr_prefix(v, "x-gzip");
    }
    if (hdr_prefix(s->line, "content-length:")) {
        const char *v = s->line + sizeof "content-length:" - 1;
        while (*v == ' ' || *v == '\t') v++;
        unsigned long n = 0;
        int any = 0;
        for (; *v >= '0' && *v <= '9'; v++) { n = n * 10 + (unsigned long)(*v - '0'); any = 1; }
        if (any) s->content_length = n;
    }
    s->line_len = 0;
}

// Returns the offset in `buf` at which the BODY starts, or -1 if the
// blank line has not been seen yet.
static long find_body(struct hdr_scan *s, const char *buf, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c = buf[i];

        int crlf = (s->have >= 3 && s->tail[0] == '\r' && s->tail[1] == '\n' &&
                    s->tail[2] == '\r' && c == '\n');
        // A bare LF LF, which some servers still emit and which the
        // CRLF test above cannot see.
        int lflf = (s->have >= 1 && s->tail[2] == '\n' && c == '\n');

        s->tail[0] = s->tail[1];
        s->tail[1] = s->tail[2];
        s->tail[2] = c;
        if (s->have < 3) s->have++;

        if (c == '\n') {
            s->in_loc = 0;
            hdr_line(s);
        } else if (c != '\r') {
            if (s->line_len < (int)sizeof s->line - 1) s->line[s->line_len++] = c;
            if (s->in_loc) {
                loc_add(s, c);
            } else if (s->want_loc && s->line_len == 9 && hdr_prefix(s->line, "location:")) {
                s->in_loc = 1;   // a second Location replaces the first
                s->loc_len = 0;
                s->loc_over = 0;
            }
        }

        if (crlf || lflf) {
            s->done = 1;
            return (long)i + 1;
        }
    }
    return -1;
}

// A ceiling on what a server may send us compressed. A decompression
// bomb is a small download that expands without limit; this bounds the
// COMPRESSED side, and the sink's own refusal bounds the other.
#define UHTTP_GZIP_MAX (64u * 1024u * 1024u)

// Inflated bytes on their way to the caller's sink.
struct uhttp_gz {
    struct uhttp_request *req;
    int stopped;
};

static int gz_sink(void *vctx, const uint8_t *data, size_t n) {
    struct uhttp_gz *c = vctx;
    if (!c->req->sink) return 0;
    if (c->req->sink(c->req->sink_ctx, (const char *)data, n) != 0) {
        c->stopped = 1;
        return -1;
    }
    return 0;
}

static void fail(struct uhttp_request *r, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->err, sizeof r->err, fmt, ap);
    va_end(ap);
}

// One attempt at one scheme. THREE-VALUED, and the middle value is the
// whole point: only "nothing is listening on that port" may be retried
// over http, because falling back on a TLS or certificate failure would
// turn "this server's identity is wrong" into "let us use plaintext
// instead" -- the downgrade HSTS exists to stop.
// REDIRECT hands `*location` (heap) to the caller, which follows it.
enum attempt { ATTEMPT_OK, ATTEMPT_NO_CONNECT, ATTEMPT_FAILED, ATTEMPT_REDIRECT };

static int is_redirect(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

static enum attempt attempt_fetch(struct uhttp_request *req,
                                  const struct uhttp_url *up, uint32_t ip, char **location) {
    // By pointer: the URL is over 2 KiB, which a copy would put on the stack.
    const struct uhttp_url *u = up;

    req->status = 0;
    req->body_bytes = 0;
    char *reqbuf = NULL;
    struct hdr_scan scan;
    memset(&scan, 0, sizeof scan);
    scan.want_loc = req->max_redirects > 0;

    // The compressed body, when there is one. Declared here so every
    // `goto done_err` can release it.
    char *gz = NULL;
    size_t gz_len = 0, gz_cap = 0;

    struct transport t = { .fd = -1, .tls = NULL };
    t.fd = sys_socket(NET_ABI_AF_INET, NET_ABI_SOCK_STREAM, NET_ABI_IPPROTO_TCP);
    if (t.fd < 0) {
        fail(req, "cannot open a socket");
        return ATTEMPT_FAILED;
    }

    if (sys_connect(t.fd, ip, u->port, 0) < 0) {
        // The REASON, as wget and curl print it: refused and timed out
        // are different faults with different fixes.
        fail(req, "cannot connect to %s port %u: %s", u->host, (unsigned)u->port,
             strerror(errno));
        close(t.fd);
        return ATTEMPT_NO_CONNECT;
    }

    if (u->tls) {
        struct utls_config cfg = {
            .hostname = u->host,
            .ca_dir   = req->ca_dir,
            .insecure = req->insecure,
            .entropy  = req->allow_weak_entropy ? UTLS_ENTROPY_ALLOW_WEAK
                                                : UTLS_ENTROPY_REQUIRE_GOOD,
        };
        char terr[UHTTP_ERR_MAX];
        t.tls = utls_connect(t.fd, &cfg, terr, sizeof terr);
        if (!t.tls) {
            // NOT ATTEMPT_NO_CONNECT: the port answered and TLS failed,
            // which is a reason to stop rather than to try plaintext.
            fail(req, "%s", terr);
            close(t.fd);
            return ATTEMPT_FAILED;
        }
    }
    req->used_tls = (t.tls != NULL);

    if (req->on_connect)
        req->on_connect(req->sink_ctx, ip,
                        t.tls ? utls_version(t.tls) : NULL,
                        t.tls ? utls_ciphersuite(t.tls) : NULL);

    // HTTP/1.0 with `Connection: close`: the server ends the body by
    // closing, so there is no chunked decoding and no Content-Length
    // arithmetic. The Host header is sent anyway -- it is 1.1's, but
    // name-based virtual hosting is universal and a server given no
    // name serves the wrong site.
    size_t reqcap = UHTTP_PATH_MAX + UHTTP_HOST_MAX + 128;
    reqbuf = malloc(reqcap);
    if (!reqbuf) {
        fail(req, "out of memory for the request");
        goto done_err;
    }
    int n = snprintf(reqbuf, reqcap,
                     "GET %s HTTP/1.0\r\nHost: %s\r\n"
                     "User-Agent: toy-os/uhttp\r\n"
                     "Accept-Encoding: gzip\r\n"
                     "Connection: close\r\n\r\n",
                     u->path, u->host);
    if (n <= 0 || (size_t)n >= reqcap) {
        fail(req, "the request does not fit in %zu bytes", reqcap);
        goto done_err;
    }
    if (write_all(&t, reqbuf, (size_t)n) != 0) {
        fail(req, "the connection closed while sending the request");
        goto done_err;
    }

    static char buf[4096]; // static: ring 3 warns above a 2 KiB frame
    int saw_status = 0;

    for (;;) {
        long got = tp_read(&t, buf, sizeof buf);
        if (got < 0) {
            fail(req, "the connection failed while reading the response");
            goto done_err;
        }
        if (got == 0) break; // the peer closed: for HTTP/1.0 that is the end

        size_t off = 0;
        if (!scan.done) {
            if (!saw_status) {
                // "HTTP/1.x NNN ..." -- the code after the first space.
                const char *sp = memchr(buf, ' ', (size_t)got);
                if (sp) {
                    req->status = atoi(sp + 1);
                    saw_status = 1;
                }
            }
            long body = find_body(&scan, buf, (size_t)got);
            if (body < 0) continue; // still in the headers
            off = (size_t)body;
            // A redirect to follow: its body is a page nobody reads.
            if (scan.want_loc && is_redirect(req->status) && scan.loc_len && !scan.loc_over) {
                *location = scan.loc;
                scan.loc = NULL;
                free(reqbuf);
                if (t.tls) utls_close(t.tls);
                close(t.fd);
                return ATTEMPT_REDIRECT;
            }
            if (req->on_headers)
                req->on_headers(req->sink_ctx, req->status, scan.content_length);
        }

        if (off < (size_t)got) {
            // A GZIPPED BODY IS BUFFERED, NOT STREAMED, and that is the
            // one cost of this. lib/uinflate.h consumes one contiguous
            // input so that it can be a straight loop rather than a
            // suspend-anywhere state machine; what is held is the
            // COMPRESSED size, which is what asking for gzip bought.
            // An uncompressed body still streams straight through.
            if (scan.gzip) {
                size_t add = (size_t)got - off;
                if (gz_len + add > gz_cap) {
                    size_t want = gz_cap ? gz_cap * 2 : 16384;
                    while (want < gz_len + add) want *= 2;
                    if (want > UHTTP_GZIP_MAX) {
                        fail(req, "the compressed body is larger than %u bytes",
                             (unsigned)UHTTP_GZIP_MAX);
                        goto done_err;
                    }
                    char *ng = realloc(gz, want);
                    if (!ng) { fail(req, "out of memory for the compressed body"); goto done_err; }
                    gz = ng;
                    gz_cap = want;
                }
                memcpy(gz + gz_len, buf + off, add);
                gz_len += add;
            } else if (req->sink) {
                if (req->sink(req->sink_ctx, buf + off, (size_t)got - off) != 0) {
                    fail(req, "the fetch was stopped by its caller");
                    goto done_err;
                }
            }
        }
        req->body_bytes += (unsigned long)((size_t)got - off);
    }

    // The whole member is here now, so inflate it into the sink. The
    // caller never learns the body arrived compressed -- which is the
    // point: Content-Encoding is a transfer detail, not the document.
    if (scan.gzip && gz_len) {
        struct uhttp_gz gctx = { req, 0 };
        size_t out_len = 0;
        // The decoder's window is the caller's -- too big for a stack
        // frame here (lib/uinflate.h).
        struct uinflate_scratch *sc = malloc(sizeof *sc);
        if (!sc) { fail(req, "out of memory to decompress the body"); goto done_err; }
        int zrc = uinflate(gz, gz_len, UINFLATE_GZIP, sc, gz_sink, &gctx, &out_len);
        free(sc);
        free(gz);
        gz = NULL;
        if (gctx.stopped) {
            fail(req, "the fetch was stopped by its caller");
            goto done_err;
        }
        if (zrc < 0) {
            fail(req, "the server's gzip body could not be read: %s",
                 uinflate_error());
            goto done_err;
        }
        // What the CALLER moved is the decompressed length; the
        // compressed one is the transport's business.
        req->body_bytes = (unsigned long)out_len;
    }
    free(gz);
    free(scan.loc);
    free(reqbuf);

    if (t.tls) utls_close(t.tls);
    close(t.fd);
    return ATTEMPT_OK;

done_err:
    free(gz);
    free(scan.loc);
    free(reqbuf);
    if (t.tls) utls_close(t.tls);
    close(t.fd);
    return ATTEMPT_FAILED;
}

// Where a redirect points, from where it came from: an absolute URL, a
// scheme-relative "//host/path", or a path on the same server. Anything
// else ("page.html" relative to a directory) is refused, not guessed.
static int next_url(const struct uhttp_url *cur, const char *loc, struct uhttp_url *out) {
    while (*loc == ' ' || *loc == '\t') loc++;
    if (!strncmp(loc, "http://", 7) || !strncmp(loc, "https://", 8))
        return uhttp_parse_url(loc, out);
    if (loc[0] == '/' && loc[1] == '/') {
        size_t n = strlen(loc) + 8;
        char *full = malloc(n);
        if (!full) return -1;
        snprintf(full, n, "%s:%s", cur->tls ? "https" : "http", loc);
        int rc = uhttp_parse_url(full, out);
        free(full);
        return rc;
    }
    if (loc[0] == '/') {
        if (strlen(loc) >= sizeof out->path) return -1;
        *out = *cur;
        strlcpy(out->path, loc, sizeof out->path);
        return 0;
    }
    return -1;
}

// Resolved per hop: a redirect may name another host.
static int resolve_host(struct uhttp_request *req, const char *host, uint32_t *ip) {
    if (uresolv_parse_ip(host, ip)) return 0;   // 1 = it was a dotted quad already
    int rc = uresolv_lookup(host, 0, ip);
    if (rc == -ENODEV) {
        fail(req, "no nameserver configured -- run `netd`, or set one in /etc/resolv.conf");
        return -1;
    }
    if (rc != 0) {
        fail(req, "%s: not found", host);
        return -1;
    }
    return 0;
}

int uhttp_fetch(struct uhttp_request *req) {
    if (!req || !req->url) return -1;
    req->err[0] = '\0';
    req->status = 0;
    req->body_bytes = 0;
    req->used_tls = 0;

    // Two URLs, the current and the next hop, on the heap: each is over
    // 2 KiB (UHTTP_PATH_MAX).
    struct uhttp_url *u = malloc(2 * sizeof *u);
    if (!u) {
        fail(req, "out of memory for the URL");
        return -1;
    }
    struct uhttp_url *nx = u + 1;
    int rc = -1;
    if (uhttp_parse_url(req->url, u) != 0) {
        fail(req, "cannot parse '%s' as a URL", req->url);
        goto out;
    }

    if (u->tls && !utls_available()) {
        if (u->scheme_given) {
            fail(req, "this build has no TLS, so https is not available");
            goto out;
        }
        u->tls = 0; // guessed https on a build without it; http is the answer
        if (!u->port_given) u->port = 80;
    }

    for (int hop = 0;; hop++) {
        // Resolved ONCE per host. The fallback changes the scheme and the
        // port, never the host, so a second lookup would ask a question
        // already answered -- and could answer it differently.
        uint32_t ip = 0;
        if (resolve_host(req, u->host, &ip) != 0) goto out;

        char *loc = NULL;
        enum attempt r = attempt_fetch(req, u, ip, &loc);

        // THE FALLBACK, and its three conditions are all necessary. Only
        // a GUESSED scheme may be downgraded (an explicit https:// that
        // will not connect is an error, not an invitation); only from
        // https; and only when the port did not answer at all.
        if (r == ATTEMPT_NO_CONNECT && hop == 0 && !u->scheme_given && u->tls) {
            if (req->on_fallback) req->on_fallback(req->sink_ctx, u->host);
            u->tls = 0;
            if (!u->port_given) u->port = 80;
            r = attempt_fetch(req, u, ip, &loc);
        }
        if (r != ATTEMPT_REDIRECT) {
            rc = r == ATTEMPT_OK ? 0 : -1;
            goto out;
        }
        int bad = next_url(u, loc, nx) != 0;
        free(loc);
        if (bad) {
            fail(req, "the server redirected to an address this cannot follow");
            goto out;
        }
        if (u->tls && !nx->tls) {
            fail(req, "refused a redirect from https to plain http");
            goto out;
        }
        if (hop + 1 > req->max_redirects) {
            fail(req, "more than %d redirects", req->max_redirects);
            goto out;
        }
        struct uhttp_url *t = u; u = nx; nx = t;
    }
out:
    free(u < nx ? u : nx);   // the one allocation, whichever slot is first
    return rc;
}
