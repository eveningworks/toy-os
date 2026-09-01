// wget -- fetch a URL over HTTP. The first program that makes the
// network useful rather than demonstrable.
//
// It exercises every layer at once, which is why it is the proof that
// TCP works: DNS resolves the host, TCP opens a connection and carries
// a byte stream in both directions with retransmission underneath, and
// the body comes back through read() on an ordinary descriptor.
//
// HTTP/1.0 WITH `Connection: close`, deliberately. That makes the
// SERVER end the body by closing, so this needs no chunked decoding, no
// Content-Length arithmetic and no persistent-connection state -- the
// end of the stream is the end of the response. The cost is one
// connection per fetch, which is the right trade for a client with no
// second request to make.
//
// NO HTTPS. TLS is a different project entirely; a URL naming it is
// refused by name rather than attempted.
#include <stdint.h>
#include "rt/sys.h"
#include "net_abi.h"
#include "lib/uresolv.h"
#include "lib/cmd.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

#define BUF 2048
#define READ_TIMEOUT_MS 8000

struct url {
    char host[128];
    char path[256];
    uint16_t port;
};

// http://host[:port][/path]. Returns 0 and says what was wrong.
static int parse_url(const char *s, struct url *u) {
    if (!strncmp(s, "https://", 8)) {
        printf("wget: https is not supported -- there is no TLS here\n");
        return 0;
    }
    if (!strncmp(s, "http://", 7)) s += 7;

    u->port = 80;
    unsigned i = 0;
    while (*s && *s != '/' && *s != ':' && i < sizeof u->host - 1) u->host[i++] = *s++;
    u->host[i] = 0;
    if (!i) { printf("wget: no host in that URL\n"); return 0; }

    if (*s == ':') {
        s++;
        int port = 0;
        while (*s >= '0' && *s <= '9') port = port * 10 + (*s++ - '0');
        if (port <= 0 || port > 65535) { printf("wget: bad port\n"); return 0; }
        u->port = (uint16_t)port;
    }

    if (!*s) { strcpy(u->path, "/"); return 1; }
    i = 0;
    while (*s && i < sizeof u->path - 1) u->path[i++] = *s++;
    u->path[i] = 0;
    return 1;
}

int main(int argc, char **argv) {
    const char *url_text = 0, *out_path = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-O") && i + 1 < argc) out_path = argv[++i];
        else if (argv[i][0] == '-') { cmd_usage("wget [-O <file>] <url>"); return 1; }
        else url_text = argv[i];
    }
    if (!url_text) { cmd_usage("wget [-O <file>] <url>"); return 1; }

    struct url u;
    if (!parse_url(url_text, &u)) return 1;

    // A host may be given as an address, in which case there is nothing
    // to resolve -- and asking a nameserver about one is a question
    // whose answer is already in hand.
    uint32_t ip = 0;
    if (!uresolv_parse_ip(u.host, &ip)) {
        int rc = uresolv_lookup(u.host, 0, &ip);
        if (rc < 0) {
            if (rc == -ENODEV)
                printf("wget: no nameserver configured -- run `dhcp`\n");
            else if (rc == -ENOENT)
                printf("wget: %s not found\n", u.host);
            else
                printf("wget: cannot resolve %s (%s)\n", u.host, strerror(-rc));
            return 1;
        }
    }
    printf("connecting to %s (%u.%u.%u.%u) port %u\n", u.host,
           (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF, u.port);

    int fd = sys_socket(NET_ABI_AF_INET, NET_ABI_SOCK_STREAM, NET_ABI_IPPROTO_TCP);
    if (fd < 0) { cmd_fail("wget", "socket"); return 1; }
    if (sys_connect(fd, ip, u.port, 0) < 0) {
        cmd_fail("wget", u.host);
        close(fd);
        return 1;
    }

    char req[512];
    int n = snprintf(req, sizeof req,
                     "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: toy-os/wget\r\n"
                     "Connection: close\r\n\r\n", u.path, u.host);
    // A stream takes what it can: writing to completion is the caller's
    // job, exactly as it is for a pipe.
    for (int off = 0; off < n; ) {
        int64_t w = write(fd, req + off, (size_t)(n - off));
        if (w <= 0) { cmd_fail("wget", "write"); close(fd); return 1; }
        off += (int)w;
    }

    int out = -1;
    if (out_path) {
        out = open(out_path, O_WRONLY | O_CREAT | O_TRUNC);
        if (out < 0) { cmd_fail("wget", out_path); close(fd); return 1; }
    }

    // THE HEADERS ARE SKIPPED BY FINDING THE BLANK LINE, and it may
    // land anywhere -- including split across two reads, which is why
    // the search runs over a small carry rather than over one buffer.
    char buf[BUF];
    int in_body = 0, status = 0;
    long body_bytes = 0;
    char carry[4] = {0};
    int carry_len = 0;

    for (;;) {
        int64_t got = read(fd, buf, sizeof buf);
        if (got <= 0) break;          // 0 is the peer's FIN: the response ended

        int start = 0;
        if (!in_body) {
            if (!status) {
                // "HTTP/1.x NNN" -- reported so a 404 does not look
                // like a successful fetch of an error page.
                const char *sp = memchr(buf, ' ', (size_t)got);
                if (sp) status = atoi(sp + 1);
            }
            for (int i = 0; i < got && !in_body; i++) {
                char window[5];
                int wl = 0;
                for (int k = carry_len; k > 0; k--) window[wl++] = carry[carry_len - k];
                window[wl++] = buf[i];
                if (wl >= 4 && !memcmp(window + wl - 4, "\r\n\r\n", 4)) {
                    in_body = 1;
                    start = i + 1;
                }
                if (wl >= 2 && !memcmp(window + wl - 2, "\n\n", 2)) {
                    in_body = 1;    // a server that used bare newlines
                    start = i + 1;
                }
                carry_len = wl < 3 ? wl : 3;
                for (int k = 0; k < carry_len; k++) carry[k] = window[wl - carry_len + k];
            }
            if (!in_body) continue;
        }

        int len = (int)got - start;
        if (len <= 0) continue;
        body_bytes += len;
        if (out >= 0) write(out, buf + start, (size_t)len);
        else write(1, buf + start, (size_t)len);
    }

    close(fd);
    if (out >= 0) close(out);

    if (status && (status < 200 || status >= 300))
        printf("\nwget: server said %d\n", status);
    if (out_path)
        printf("saved %ld bytes to %s\n", body_bytes, out_path);
    return status && (status < 200 || status >= 300) ? 1 : 0;
}
