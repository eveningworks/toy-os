// wget -- fetch a URL over HTTP or HTTPS.
//
// The protocol lives in /lib/libhttp.so (<uhttp.h>), not here: there
// were about to be three copies of a URL parser and a header scan --
// this, /bin/httpd, and the /bin/update that docs/update-design.md
// designs -- and the part that is easy to get wrong is invisible in all
// three (a socket read returns at most 1472 bytes, so the blank line
// ending the headers can straddle any two reads).
//
// What is left here is the FRONT END: arguments, where the body goes,
// and how a failure is worded.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

#include "lib/cmd.h"
#include "uhttp.h"

#define USAGE "wget [-O <file>] [-k] [--weak-entropy] <url>"

struct out {
    int  fd;          // -1 means stdout
    int  insecure;    // for the warning printed on connect
};

static int sink(void *ctx, const void *data, size_t len) {
    struct out *o = ctx;
    const char *p = data;
    while (len) {
        long w = (o->fd < 0) ? (long)fwrite(p, 1, len, stdout)
                             : write(o->fd, p, len);
        if (w <= 0) return -1;
        p += w;
        len -= (size_t)w;
    }
    return 0;
}

// Printed before the body so a response from an unexpected host is
// attributable, which is why the address is shown rather than only the
// name. On https it also names the protocol version and the suite --
// the two facts that say what the encryption actually is.
static void on_connect(void *ctx, uint32_t ip, const char *ver, const char *cipher) {
    struct out *o = ctx;
    printf("connecting to %u.%u.%u.%u",
           (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
    if (ver) printf(" -- %s, %s", ver, cipher ? cipher : "?");
    printf("\n");
    if (ver && o->insecure)
        printf("wget: WARNING: the server's identity was NOT verified\n");
}

int main(int argc, char **argv) {
    const char *url = 0, *out_path = 0;
    struct uhttp_request req;
    memset(&req, 0, sizeof req);

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-O") && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(argv[i], "-k")) req.insecure = 1;
        else if (!strcmp(argv[i], "--weak-entropy")) req.allow_weak_entropy = 1;
        else if (argv[i][0] == '-') { cmd_usage(USAGE); return 1; }
        else url = argv[i];
    }
    if (!url) { cmd_usage(USAGE); return 1; }

    struct out o = { .fd = -1, .insecure = req.insecure };
    if (out_path) {
        o.fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC);
        if (o.fd < 0) { cmd_fail("wget", out_path); return 1; }
    }

    req.url        = url;
    req.sink       = sink;
    req.sink_ctx   = &o;
    req.on_connect = on_connect;

    int rc = uhttp_fetch(&req);
    if (rc != 0) {
        printf("wget: %s\n", req.err);
        // The library states the condition; naming the flag that
        // overrides it is this program's job, since the flag is this
        // program's.
        if (!req.allow_weak_entropy && strstr(req.err, "randomness"))
            printf("wget: pass --weak-entropy to accept it anyway\n");
        if (o.fd >= 0) close(o.fd);
        return 1;
    }

    if (o.fd >= 0) {
        close(o.fd);
        printf("saved %lu bytes to %s\n", req.body_bytes, out_path);
    }

    // A non-2xx status is REPORTED and the body still printed, because
    // an error page is usually the explanation. The exit status is what
    // a script reads.
    if (req.status < 200 || req.status > 299) {
        printf("wget: server returned status %d\n", req.status);
        return 1;
    }
    return 0;
}
