#ifndef ULIB_UHTTP_H
#define ULIB_UHTTP_H

// An HTTP/HTTPS client, as a library.
//
// It exists because there were about to be three copies of it. /bin/wget
// had a URL parser, a header/blank-line scanner and a status scrape;
// /bin/httpd has its own request-line parser; and docs/update-design.md
// designs a /bin/update that fetches an HTTP manifest. The part that is
// easy to get wrong is invisible in all three: a socket read returns at
// most SYS_NET_MSG_MAX (1472) bytes whatever buffer it is handed, so the
// blank line ending the headers can straddle any two reads.
//
// **NOT IN userland/lib/.** That directory is globbed into libuapp,
// which every program links, and this reaches TLS -- which would put
// mbedTLS behind every binary in the system. It builds into
// /lib/libhttp.so and a program opts in with ULIB_SO_<name>.
//
// **HTTP AND HTTPS DIFFER BY ONE FIELD.** The scheme decides whether a
// TLS session is wrapped around the socket; everything above that is
// the same code. That is the whole reason the transport is behind
// <utls.h> rather than open-coded in the fetch.
#include <stddef.h>
#include <stdint.h>

// FS_PATH_MAX is 64, so a URL path cannot be sized against it -- a URL
// is not a path, and sizing it like one truncates it (the shell
// conventions make the same point about a command line).
#define UHTTP_HOST_MAX 128
#define UHTTP_PATH_MAX 512
#define UHTTP_ERR_MAX  192

struct uhttp_url {
    char     host[UHTTP_HOST_MAX];
    char     path[UHTTP_PATH_MAX];
    uint16_t port;
    int      tls;          // https

    // A URL with NO scheme ("example.com") parses with tls=1 and port
    // 443, and these two say so. `uhttp_fetch()` then tries https and
    // falls back to http -- but ONLY when 443 does not connect, which
    // is the whole reason the caller has to be able to tell the two
    // apart. See the fetch's comment.
    int      scheme_given;
    int      port_given;
};

// 0 on success, -1 on a URL this cannot represent. REJECTS rather than
// guessing about anything but the SCHEME: a host that does not fit is
// an error, never a prefix.
int uhttp_parse_url(const char *url, struct uhttp_url *out);

// Called with each chunk of the body as it arrives. Return 0 to carry
// on, non-zero to abort the fetch. Streaming rather than buffering
// because the body may be larger than the heap.
typedef int (*uhttp_sink)(void *ctx, const void *data, size_t len);

struct uhttp_request {
    // --- in ---
    const char *url;
    uhttp_sink  sink;
    void       *sink_ctx;

    // Skip certificate verification. Encrypted but UNAUTHENTICATED --
    // anyone able to answer in the server's place can read and rewrite
    // the exchange. A caller setting this should say so on screen.
    int insecure;

    // Proceed even when QUERY_RANDOM says this machine's entropy is
    // only TSC jitter. Off by default; see <utls.h>.
    int allow_weak_entropy;

    const char *ca_dir; // NULL = UTLS_DEFAULT_CA_DIR

    // Called once, after the connection is up and before the body, so a
    // caller can print what it connected to. `tls_version` is NULL on a
    // plain connection. Either may be NULL.
    void (*on_connect)(void *ctx, uint32_t ip, const char *tls_version,
                       const char *tls_cipher);

    // Called when a scheme-less URL's https attempt could not CONNECT
    // and http is about to be tried instead. A downgrade the user did
    // not ask for should not be silent, so this exists to be printed --
    // it is not a debug hook.
    void (*on_fallback)(void *ctx, const char *host);

    // --- out ---
    int           status;      // the HTTP status, or 0 if none was read
    unsigned long body_bytes;  // what reached the sink
    int           used_tls;    // whether the fetch that SUCCEEDED was https
    char          err[UHTTP_ERR_MAX];
};

// 0 on success (a response was read, whatever its status), -1 otherwise
// with `err` set to a sentence. A non-2xx status is NOT a failure here:
// the body of an error page is usually the explanation, and deciding
// what to do about it is the caller's.
int uhttp_fetch(struct uhttp_request *req);

// Is this build able to speak https at all? Lets a caller say "no TLS
// in this build" rather than failing at connect time.
int uhttp_tls_available(void);

#endif
