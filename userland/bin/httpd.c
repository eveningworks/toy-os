// httpd -- serve files from this machine's own filesystem over HTTP.
//
// The other half of the network: `wget` proved toy-os can reach out,
// this proves something can reach IN. A browser on the host sees the
// OS's disk.
//
// TWO MODES. On its own it is one connection at a time -- accept, serve,
// close, accept again. Under `-1` it serves ONE connection already on fd
// 0 and fd 1 and exits, which is how an inetd service is written: run it
// as `inetd -p 80 /bin/httpd -1 /` and each client gets its own process.
// The serial mode stays because it is the readable one and needs nothing
// else running.
//
// `-1` MUST NOT PRINT TO STDOUT: fd 1 is the client, so the accept log
// would land in the middle of the response body.
//
// HTTP/1.0 AND `Connection: close`: the response ends when the socket
// does, so there is no keep-alive state and no chunked encoding. The
// request is read up to the blank line and only the request LINE is
// parsed -- headers are read and discarded, because nothing here varies
// on one.
#include <stdint.h>
#include "rt/sys.h"
#include "net_abi.h"
#include "lib/cmd.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define USAGE        "httpd [-p <port>] [-1] [<root>]"
#define DEFAULT_PORT 80
#define REQ_MAX      1024
#define CHUNK        1024
#define ACCEPT_LOG   1

static const char *content_type(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".txt") || !strcmp(dot, ".conf") || !strcmp(dot, ".md"))
        return "text/plain";
    if (!strcmp(dot, ".html") || !strcmp(dot, ".htm")) return "text/html";
    if (!strcmp(dot, ".qoi")) return "image/qoi";
    if (!strcmp(dot, ".jpg") || !strcmp(dot, ".jpeg")) return "image/jpeg";
    return "application/octet-stream";
}

static void write_all(int fd, const char *buf, int len) {
    for (int off = 0; off < len; ) {
        int64_t w = sys_write(fd, buf + off, (size_t)(len - off));
        if (w <= 0) return;      // the client went away; the caller closes
        off += (int)w;
    }
}

static void say(int fd, const char *status, const char *type, long len) {
    char head[256];
    int n = snprintf(head, sizeof head,
                     "HTTP/1.0 %s\r\nServer: toy-os/httpd\r\nContent-Type: %s\r\n"
                     "Content-Length: %ld\r\nConnection: close\r\n\r\n",
                     status, type, len);
    write_all(fd, head, n);
}

static void serve_error(int fd, const char *status, const char *text) {
    char body[192];
    int n = snprintf(body, sizeof body,
                     "<html><body><h1>%s</h1><p>%s</p></body></html>\n", status, text);
    say(fd, status, "text/html", n);
    write_all(fd, body, n);
}

// A directory, as a page of links. The listing is built into a buffer
// first because Content-Length has to precede it -- and a length that
// disagrees with the body is worse than no length at all.
static void serve_listing(int fd, const char *path) {
    static char page[8192];
    static struct sys_dirent ents[64];
    int n = sys_listdir(path, ents, (int)(sizeof ents / sizeof ents[0]));
    if (n < 0) { serve_error(fd, "404 Not Found", "No such directory."); return; }

    int len = snprintf(page, sizeof page,
                       "<html><body><h1>%s</h1><ul>\n", path);
    for (int i = 0; i < n && len < (int)sizeof page - 256; i++) {
        int slash = path[strlen(path) - 1] == '/';
        len += snprintf(page + len, sizeof page - (size_t)len,
                        "<li><a href=\"%s%s%s\">%s%s</a></li>\n",
                        path, slash ? "" : "/", ents[i].name,
                        ents[i].name, ents[i].is_dir ? "/" : "");
    }
    len += snprintf(page + len, sizeof page - (size_t)len, "</ul></body></html>\n");

    say(fd, "200 OK", "text/html", len);
    write_all(fd, page, len);
}

static void serve_file(int fd, const char *path) {
    struct sys_stat st;
    if (sys_stat(path, &st) < 0) {
        serve_error(fd, "404 Not Found", "No such file on this machine.");
        return;
    }
    if (st.is_dir) { serve_listing(fd, path); return; }

    int in = sys_open(path, 0)   /* read-only is the default */;
    if (in < 0) { serve_error(fd, "403 Forbidden", "Cannot open that."); return; }

    say(fd, "200 OK", content_type(path), (long)st.size);
    static char chunk[CHUNK];
    for (;;) {
        int64_t got = sys_read(in, chunk, sizeof chunk);
        if (got <= 0) break;
        write_all(fd, chunk, (int)got);
    }
    sys_close(in);
}

// The request line only: "GET /path HTTP/1.0". Everything up to the
// blank line is consumed so the client is not left writing into a
// socket nobody is reading, and then discarded.
static int read_request(int fd, char *path, size_t cap) {
    static char req[REQ_MAX];
    int len = 0;
    while (len < (int)sizeof req - 1) {
        int64_t got = sys_read(fd, req + len, (size_t)((int)sizeof req - 1 - len));
        if (got <= 0) break;
        len += (int)got;
        req[len] = 0;
        if (strstr(req, "\r\n\r\n") || strstr(req, "\n\n")) break;
    }
    if (len <= 0) return 0;
    req[len] = 0;

    if (strncmp(req, "GET ", 4)) return -1;      // only GET is implemented
    const char *p = req + 4;
    size_t i = 0;
    while (*p && *p != ' ' && *p != '\r' && *p != '\n' && i < cap - 1) path[i++] = *p++;
    path[i] = 0;
    return i ? 1 : -1;
}

// `..` IS REFUSED RATHER THAN RESOLVED. This serves a subtree of a
// filesystem to anything that can reach the port, so a path escaping
// the root is the one input that must not work -- and refusing the
// component outright is checkable, where normalising and re-checking is
// the shape every directory-traversal bug has had.
static int path_is_safe(const char *p) {
    if (p[0] != '/') return 0;
    for (const char *s = p; *s; s++) {
        if (s[0] != '.' || s[1] != '.') continue;
        if ((s == p || s[-1] == '/') && (s[2] == '/' || s[2] == 0)) return 0;
    }
    return 1;
}

// One request, one response. `in` and `out` are the same socket in the
// serial mode and fds 0 and 1 under `-1`; keeping them apart is what
// makes the handler an ordinary filter rather than a socket program.
//
// `peer` is the client to name in the log line, or NULL for no line --
// which is what `-1` passes, because fd 1 there is the client.
static void serve_connection(int in, int out, const char *root, const char *peer) {
    char req_path[192], full[256];
    int rc = read_request(in, req_path, sizeof req_path);
    if (rc <= 0) {
        if (rc < 0) serve_error(out, "400 Bad Request", "Only GET is served here.");
        return;
    }
    if (!path_is_safe(req_path)) {
        serve_error(out, "403 Forbidden", "That path leaves the served root.");
        return;
    }
    int slash = root[strlen(root) - 1] == '/';
    snprintf(full, sizeof full, "%s%s", root,
             slash && req_path[0] == '/' ? req_path + 1 : req_path);
    if (peer) printf("httpd: %s GET %s\n", peer, full);
    serve_file(out, full);
}

int main(int argc, char **argv) {
    int port = DEFAULT_PORT, oneshot = 0;
    const char *root = "/";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-1")) oneshot = 1;
        else if (argv[i][0] == '-') { cmd_usage(USAGE); return 1; }
        else root = argv[i];
    }
    if (port <= 0 || port > 65535) { cmd_usage(USAGE); return 1; }

    // Already connected: the socket is fd 0 and fd 1, nothing to bind,
    // and the exit is what closes the connection.
    if (oneshot) { serve_connection(0, 1, root, 0); return 0; }

    int lis = sys_socket(NET_ABI_AF_INET, NET_ABI_SOCK_STREAM, NET_ABI_IPPROTO_TCP);
    if (lis < 0) { cmd_fail("httpd", "socket"); return 1; }
    if (sys_bind(lis, 0, (uint16_t)port, 0) < 0) { cmd_fail("httpd", "bind"); return 1; }
    if (sys_listen(lis) < 0) { cmd_fail("httpd", "listen"); return 1; }

    printf("httpd: serving %s on port %d -- Ctrl-C to stop\n", root, port);

    for (;;) {
        uint32_t peer = 0;
        uint16_t peer_port = 0;
        int c = sys_accept(lis, &peer, &peer_port, 0);
        if (c < 0) {
            if (sys_errno() == EINTR) break;      // Ctrl-C: stop serving
            cmd_fail("httpd", "accept");
            break;
        }

        // ONE LINE PER REQUEST, and that is a budget rather than a
        // preference: fd 1 here is the console, whose sink is a serial
        // port with a finite buffer, and a reader that is not draining
        // it stalls the whole guest. A second line per connection cost
        // this server a quarter of the requests it could answer before
        // an undrained harness wedged it.
        char peerbuf[32];
        snprintf(peerbuf, sizeof peerbuf, "%u.%u.%u.%u:%u",
                 (peer >> 24) & 0xFF, (peer >> 16) & 0xFF,
                 (peer >> 8) & 0xFF, peer & 0xFF, peer_port);
        serve_connection(c, c, root, ACCEPT_LOG ? peerbuf : 0);
        sys_close(c);
    }

    sys_close(lis);
    return 0;
}
