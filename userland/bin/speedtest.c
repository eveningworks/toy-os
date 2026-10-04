// speedtest -- how fast this machine's connection is, against speedtest.net.
//
//     speedtest [-l] [-s <id>] [-n <streams>] [-t <seconds>] [--no-upload]
//               [-k] [--weak-entropy]
//     speedtest --url <http-url> [-n <streams>] [-t <seconds>]
//
// THE SERVER LIST IS speedtest.net's, THE TEST IS PLAIN HTTP TO AN OOKLA
// SERVER. The list (`/api/js/servers`, https) comes back nearest first
// by geolocating this machine's address; each server answers
// `/speedtest/latency.txt`, `/download?size=N` and `POST /upload` on
// port 8080. These are the endpoints the open-source speedtest-cli
// uses, not a published API, so they can change under this program --
// which is why a failure names the step that failed.
//
// PARALLEL STREAMS, ONE THREAD EACH. Four by default, as Ookla's clients
// do: one TCP connection is limited by its own window and its own loss
// recovery, and several together measure the path rather than one
// connection's luck. Each thread blocks in its own read or write; the
// main thread only samples the byte counts.
//
// THE FIRST TWO SECONDS DO NOT COUNT. Every connection starts in slow
// start, so a rate averaged from zero under-reports a fast link; the
// result is the rate between the warm-up and the end.
//
// --url TURNS IT INTO A DOWNLOAD METER FOR ANY HTTP SERVER, a LAN one
// included, which is what separates "the internet is slow" from "this
// stack is slow". It measures download only: an arbitrary server has no
// upload endpoint to post to.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <pthread.h>
#include "rt/sys.h"
#include "lib/cmd.h"
#include "lib/uresolv.h"
#include "net_abi.h"
#include "uhttp.h"
#include "ports/cjson/cJSON.h"

#define USAGE "speedtest [-l] [-s <id>] [-n <streams>] [-t <seconds>] [--no-upload] " \
              "[-k] [--weak-entropy] [--url <http-url>]"

#define LIST_URL     "https://www.speedtest.net/api/js/servers?engine=js&limit=10"
#define LIST_MAX     (256 * 1024)
#define SERVERS_MAX  10
#define PING_CANDIDATES 5
#define STREAMS_MAX  6          // the kernel has 8 TCP blocks, and others need some
#define WARMUP_MS    2000
#define CHUNK        65536      // one read or write
#define DL_SIZE      25000000u  // bytes asked for per download request
#define UL_SIZE      25000000u  // bytes promised per upload request
#define CONNECT_MS   5000

struct server {
    char id[16];
    char host[UHTTP_HOST_MAX];
    uint16_t port;
    char name[64];
    char sponsor[64];
    int km;
    uint32_t ip;
};

// --- plumbing ------------------------------------------------------------

static void copy_str(char *dst, size_t cap, const cJSON *obj, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    dst[0] = 0;
    if (cJSON_IsString(v) && v->valuestring) snprintf(dst, cap, "%s", v->valuestring);
}

static int resolve(const char *host, uint32_t *ip) {
    // A dotted quad is used as it is; anything else goes to the resolver.
    unsigned a, b, c, d;
    char tail;
    if (sscanf(host, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) == 4 &&
        a < 256 && b < 256 && c < 256 && d < 256) {
        *ip = (a << 24) | (b << 16) | (c << 8) | d;
        return 0;
    }
    return uresolv_lookup(host, 0, ip);
}

// A connected stream, retried for a moment on ENOSPC: the previous
// phase's connections are still saying goodbye, and the kernel's pool of
// connection blocks is small.
static int dial(uint32_t ip, uint16_t port) {
    for (int tries = 0; tries < 30; tries++) {
        int fd = sys_socket(NET_ABI_AF_INET, NET_ABI_SOCK_STREAM, NET_ABI_IPPROTO_TCP);
        if (fd < 0) { sys_sleep_ms(100); continue; }
        int rc = sys_connect(fd, ip, port, CONNECT_MS);
        if (rc == 0) return fd;
        close(fd);
        if (sys_errno() != ENOSPC) return -1;
        sys_sleep_ms(100);
    }
    return -1;
}

static int send_all(int fd, const void *p, size_t n) {
    const char *c = p;
    while (n) {
        long w = write(fd, c, n);
        if (w <= 0) return -1;
        c += w;
        n -= (size_t)w;
    }
    return 0;
}

// One HTTP/1.1 response on a kept-alive connection: the headers, then a
// Content-Length body (or, without one, everything to the close). Body
// bytes are added to *counter as they arrive and discarded. Returns the
// status, or -1 when the connection failed; *closing says the server
// will not take another request on it. `stop` ends it early.
static int read_response(int fd, char *buf, volatile uint64_t *counter,
                         volatile int *stop, int *closing) {
    size_t have = 0;
    char *end = 0;
    *closing = 0;
    while (!end) {
        if (have >= 8192) return -1;                    // absurd headers
        long n = read(fd, buf + have, 8192 - have);
        if (n <= 0) return -1;
        have += (size_t)n;
        buf[have] = 0;
        end = strstr(buf, "\r\n\r\n");
    }
    int status = 0;
    if (sscanf(buf, "HTTP/%*d.%*d %d", &status) != 1) return -1;

    long long length = -1;
    for (char *line = strstr(buf, "\r\n"); line && line < end; line = strstr(line + 2, "\r\n")) {
        char *h = line + 2;
        if (!strncasecmp(h, "Content-Length:", 15)) length = atoll(h + 15);
        if (!strncasecmp(h, "Connection:", 11) && strstr(h, "close") && strstr(h, "close") < strstr(h, "\r\n"))
            *closing = 1;
    }
    if (!strncmp(buf, "HTTP/1.0", 8)) *closing = 1;       // 1.0 keeps nothing alive

    size_t head = (size_t)(end + 4 - buf);
    long long got = (long long)(have - head);
    *counter += (uint64_t)got;
    while ((length < 0 || got < length) && !*stop) {
        long n = read(fd, buf, CHUNK);
        if (n < 0) return -1;
        if (n == 0) { *closing = 1; break; }            // closed: the body is what came
        got += n;
        *counter += (uint64_t)n;
    }
    if (*stop && length >= 0 && got < length) *closing = 1;   // abandoned mid-body
    return status;
}

// --- latency --------------------------------------------------------------

// The best of three round trips on one kept-alive connection, after a
// first one that pays for the handshake. Milliseconds, or -1.
static double latency_ms(const struct server *s) {
    int fd = dial(s->ip, s->port);
    if (fd < 0) return -1;
    char *buf = malloc(CHUNK + 1);
    if (!buf) { close(fd); return -1; }
    double best = -1;
    volatile uint64_t sink = 0;
    volatile int never = 0;
    for (int i = 0; i < 4; i++) {
        char req[384];
        snprintf(req, sizeof req,
                 "GET /speedtest/latency.txt?x=%u HTTP/1.1\r\nHost: %s\r\n"
                 "User-Agent: toy-os speedtest\r\nConnection: keep-alive\r\n\r\n",
                 (unsigned)sys_monotonic_ns(), s->host);
        unsigned long long t0 = sys_monotonic_ns();
        int closing = 0;
        if (send_all(fd, req, strlen(req)) < 0 ||
            read_response(fd, buf, &sink, &never, &closing) != 200) { best = -1; break; }
        double ms = (double)(sys_monotonic_ns() - t0) / 1e6;
        if (i > 0 && (best < 0 || ms < best)) best = ms;
        if (closing) break;
    }
    free(buf);
    close(fd);
    return best;
}

// --- the streams ---------------------------------------------------------

struct stream {
    pthread_t thread;
    const char *host;           // the Host header
    uint32_t ip;
    uint16_t port;
    const char *path;           // --url's path; NULL means the Ookla endpoints
    int upload;
    volatile uint64_t bytes;
    volatile int stop;
    int failed;
};

static void *download_thread(void *arg) {
    struct stream *st = arg;
    char *buf = malloc(CHUNK + 1);
    if (!buf) { st->failed = 1; return 0; }
    int fd = -1;
    unsigned n = 0;
    while (!st->stop) {
        if (fd < 0 && (fd = dial(st->ip, st->port)) < 0) { st->failed = 1; break; }
        char req[768];
        if (st->path)
            snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s\r\n"
                     "User-Agent: toy-os speedtest\r\nConnection: keep-alive\r\n\r\n",
                     st->path, st->host);
        else
            snprintf(req, sizeof req, "GET /download?nocache=%u.%u&size=%u HTTP/1.1\r\n"
                     "Host: %s\r\nUser-Agent: toy-os speedtest\r\nConnection: keep-alive\r\n\r\n",
                     (unsigned)sys_monotonic_ns(), n++, DL_SIZE, st->host);
        int closing = 0;
        if (send_all(fd, req, strlen(req)) < 0) { st->failed = 1; break; }
        int status = read_response(fd, buf, &st->bytes, &st->stop, &closing);
        if (status != 200) { if (!st->stop) st->failed = 1; break; }
        if (closing) { close(fd); fd = -1; }
    }
    if (fd >= 0) close(fd);
    free(buf);
    return 0;
}

static void *upload_thread(void *arg) {
    struct stream *st = arg;
    char *buf = malloc(CHUNK + 1);
    if (!buf) { st->failed = 1; return 0; }
    // Incompressible enough: nothing on the path should be able to
    // shrink it, and it costs one fill.
    uint32_t x = 0x9E3779B9u ^ (uint32_t)sys_monotonic_ns();
    for (int i = 0; i < CHUNK; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; buf[i] = (char)x; }

    int fd = -1;
    unsigned n = 0;
    while (!st->stop) {
        if (fd < 0 && (fd = dial(st->ip, st->port)) < 0) { st->failed = 1; break; }
        char req[512];
        snprintf(req, sizeof req, "POST /upload?nocache=%u.%u HTTP/1.1\r\nHost: %s\r\n"
                 "User-Agent: toy-os speedtest\r\nContent-Type: application/octet-stream\r\n"
                 "Content-Length: %u\r\nConnection: keep-alive\r\n\r\n",
                 (unsigned)sys_monotonic_ns(), n++, st->host, UL_SIZE);
        if (send_all(fd, req, strlen(req)) < 0) { st->failed = 1; break; }
        uint32_t left = UL_SIZE;
        while (left && !st->stop) {
            size_t k = left < CHUNK ? left : CHUNK;
            long w = write(fd, buf, k);
            if (w <= 0) { st->failed = 1; break; }
            left -= (uint32_t)w;
            st->bytes += (uint64_t)w;
        }
        if (st->failed || st->stop) break;
        volatile uint64_t sink = 0;
        int closing = 0;
        if (read_response(fd, buf, &sink, &st->stop, &closing) != 200) {
            if (!st->stop) st->failed = 1;
            break;
        }
        if (closing) { close(fd); fd = -1; }
    }
    if (fd >= 0) close(fd);
    free(buf);
    return 0;
}

static uint64_t total_bytes(struct stream *st, int n) {
    uint64_t t = 0;
    for (int i = 0; i < n; i++) t += st[i].bytes;
    return t;
}

// One timed phase: `n` streams for `secs` seconds, a live line while it
// runs, and the rate after the warm-up. Mbit/s, or -1 when every stream
// failed.
static double run_phase(const char *label, struct stream *proto, int n, int secs) {
    struct stream st[STREAMS_MAX];
    for (int i = 0; i < n; i++) {
        st[i] = *proto;
        st[i].bytes = 0;
        st[i].stop = 0;
        st[i].failed = 0;
        if (pthread_create(&st[i].thread, 0, st[i].upload ? upload_thread : download_thread,
                           &st[i]) != 0) {
            n = i;
            break;
        }
    }
    if (!n) return -1;

    int live = isatty(1);
    unsigned long long t0 = sys_monotonic_ns();
    unsigned long long warm_at = 0, end = t0 + (unsigned long long)secs * 1000000000ull;
    uint64_t warm_bytes = 0;
    for (;;) {
        sys_sleep_ms(250);
        unsigned long long now = sys_monotonic_ns();
        uint64_t b = total_bytes(st, n);
        if (!warm_at && now - t0 >= (unsigned long long)WARMUP_MS * 1000000ull) {
            warm_at = now;
            warm_bytes = b;
        }
        if (live) {
            // From the start until half a second of post-warm-up data
            // exists, or the display dips to zero at the switch.
            int after = warm_at && now - warm_at >= 500000000ull;
            unsigned long long since = after ? warm_at : t0;
            uint64_t base = after ? warm_bytes : 0;
            double s = (double)(now - since) / 1e9;
            double mbps = s > 0 ? (double)(b - base) * 8 / 1e6 / s : 0;
            printf("\r%s: %8.2f Mbit/s", label, mbps);
            fflush(stdout);
        }
        if (now >= end) break;
        int alive = 0;
        for (int i = 0; i < n; i++) alive += !st[i].failed;
        if (!alive) break;
    }
    unsigned long long stop_at = sys_monotonic_ns();
    uint64_t b = total_bytes(st, n);
    for (int i = 0; i < n; i++) st[i].stop = 1;
    for (int i = 0; i < n; i++) pthread_join(st[i].thread, 0);

    int failed = 0;
    for (int i = 0; i < n; i++) failed += st[i].failed;
    if (live) printf("\r%*s\r", 40, "");       // the live line, cleared
    if (!warm_at || failed == n) return -1;
    double s = (double)(stop_at - warm_at) / 1e9;
    if (failed) printf("speedtest: %d of %d %s streams failed\n", failed, n, label);
    return s > 0 ? (double)(b - warm_bytes) * 8 / 1e6 / s : -1;
}

// --- the server list -----------------------------------------------------

struct list_buf { char *p; size_t len; };

static int list_sink(void *ctx, const void *data, size_t len) {
    struct list_buf *b = ctx;
    if (b->len + len >= LIST_MAX) return -1;
    memcpy(b->p + b->len, data, len);
    b->len += len;
    return 0;
}

// Up to SERVERS_MAX servers, nearest first. -1 with a sentence printed.
static int fetch_servers(struct server *out, int insecure, int weak) {
    struct list_buf b = { malloc(LIST_MAX), 0 };
    if (!b.p) { printf("speedtest: out of memory\n"); return -1; }
    struct uhttp_request req;
    memset(&req, 0, sizeof req);
    req.url = LIST_URL;
    req.sink = list_sink;
    req.sink_ctx = &b;
    req.insecure = insecure;
    req.allow_weak_entropy = weak;
    if (uhttp_fetch(&req) != 0 || req.status != 200) {
        printf("speedtest: cannot fetch the server list: %s\n",
               req.err[0] ? req.err : "unexpected status");
        if (!insecure && strstr(req.err, "certif"))
            printf("speedtest: pass -k to skip verification, or build with EXTRAS=1 "
                   "for a trust store\n");
        if (!weak && strstr(req.err, "randomness"))
            printf("speedtest: pass --weak-entropy to accept it anyway\n");
        free(b.p);
        return -1;
    }

    cJSON *root = cJSON_ParseWithLength(b.p, b.len);
    free(b.p);
    if (!cJSON_IsArray(root)) {
        printf("speedtest: the server list is not what speedtest.net used to send\n");
        cJSON_Delete(root);
        return -1;
    }
    int n = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, root) {
        if (n >= SERVERS_MAX) break;
        struct server *s = &out[n];
        char hostport[UHTTP_HOST_MAX + 8];
        copy_str(hostport, sizeof hostport, e, "host");
        char *colon = strrchr(hostport, ':');
        if (!colon || !hostport[0]) continue;
        *colon = 0;
        s->port = (uint16_t)atoi(colon + 1);
        if (!s->port || strlen(hostport) >= sizeof s->host) continue;
        strcpy(s->host, hostport);
        copy_str(s->id, sizeof s->id, e, "id");
        copy_str(s->name, sizeof s->name, e, "name");
        copy_str(s->sponsor, sizeof s->sponsor, e, "sponsor");
        const cJSON *d = cJSON_GetObjectItemCaseSensitive(e, "distance");
        s->km = cJSON_IsNumber(d) ? (int)d->valuedouble : -1;
        s->ip = 0;
        n++;
    }
    cJSON_Delete(root);
    if (!n) printf("speedtest: the server list was empty\n");
    return n ? n : -1;
}

static void print_result(const char *label, double mbps) {
    if (mbps < 0) printf("%s: failed\n", label);
    else printf("%s: %.2f Mbit/s\n", label, mbps);
}

// --- --url ----------------------------------------------------------------

static int url_mode(const char *url, int streams, int secs) {
    static struct uhttp_url u;   // static: over 2 KiB, the ring-3 frame budget
    if (uhttp_parse_url(url, &u) != 0 || u.tls || !u.scheme_given) {
        printf("speedtest: --url takes an http:// URL (the streams do not speak TLS)\n");
        return 1;
    }
    uint32_t ip;
    if (resolve(u.host, &ip) < 0) { printf("speedtest: cannot resolve %s\n", u.host); return 1; }

    unsigned long long t0 = sys_monotonic_ns();
    int fd = dial(ip, u.port);
    if (fd < 0) { printf("speedtest: cannot connect to %s port %u\n", u.host, u.port); return 1; }
    printf("Connected to %s (%u.%u.%u.%u) port %u in %.3f ms\n", u.host,
           (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF, u.port,
           (double)(sys_monotonic_ns() - t0) / 1e6);
    close(fd);

    printf("Testing download speed (%d stream%s, %d s)...\n", streams, streams == 1 ? "" : "s", secs);
    struct stream proto = { .host = u.host, .ip = ip, .port = u.port, .path = u.path };
    print_result("Download", run_phase("Download", &proto, streams, secs));
    return 0;
}

// --- main -------------------------------------------------------------------

int main(int argc, char **argv) {
    int list_only = 0, no_upload = 0, insecure = 0, weak = 0;
    int streams = 4, secs = 10;
    const char *want_id = 0, *url = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-l")) list_only = 1;
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) want_id = argv[++i];
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) streams = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) secs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-upload")) no_upload = 1;
        else if (!strcmp(argv[i], "-k")) insecure = 1;
        else if (!strcmp(argv[i], "--weak-entropy")) weak = 1;
        else if (!strcmp(argv[i], "--url") && i + 1 < argc) url = argv[++i];
        else { cmd_usage(USAGE); return 1; }
    }
    if (streams < 1 || streams > STREAMS_MAX) {
        printf("speedtest: -n takes 1 to %d streams\n", STREAMS_MAX);
        return 1;
    }
    // The warm-up is not counted, so a test must outlast it.
    if (secs < 3 || secs > 60) { printf("speedtest: -t takes 3 to 60 seconds\n"); return 1; }

    if (url) return url_mode(url, streams, secs);

    printf("Retrieving speedtest.net server list...\n");
    static struct server servers[SERVERS_MAX];
    int n = fetch_servers(servers, insecure, weak);
    if (n < 0) return 1;

    if (list_only) {
        for (int i = 0; i < n; i++)
            printf("%8s) %s (%s) [%d km]\n", servers[i].id, servers[i].sponsor,
                   servers[i].name, servers[i].km);
        return 0;
    }

    // The candidates: the one asked for, or the nearest few, of which
    // the lowest latency wins -- distance is where Ookla thinks this
    // machine is, latency is where the packets actually go.
    int first = 0, count = n < PING_CANDIDATES ? n : PING_CANDIDATES;
    if (want_id) {
        count = 0;
        for (int i = 0; i < n; i++)
            if (!strcmp(servers[i].id, want_id)) { first = i; count = 1; }
        if (!count) {
            printf("speedtest: server %s is not among the %d nearest (see -l)\n", want_id, n);
            return 1;
        }
    }
    printf("Selecting best server based on ping...\n");
    int best = -1;
    double best_ms = -1;
    for (int i = first; i < first + count; i++) {
        if (resolve(servers[i].host, &servers[i].ip) < 0) continue;
        double ms = latency_ms(&servers[i]);
        if (ms >= 0 && (best < 0 || ms < best_ms)) { best = i; best_ms = ms; }
    }
    if (best < 0) { printf("speedtest: no server answered\n"); return 1; }

    struct server *s = &servers[best];
    printf("Hosted by %s (%s) [%d km]: %.3f ms\n", s->sponsor, s->name, s->km, best_ms);

    struct stream proto = { .host = s->host, .ip = s->ip, .port = s->port };
    printf("Testing download speed (%d stream%s, %d s)...\n", streams, streams == 1 ? "" : "s", secs);
    double down = run_phase("Download", &proto, streams, secs);
    print_result("Download", down);

    double up = 0;
    if (!no_upload) {
        printf("Testing upload speed (%d stream%s, %d s)...\n", streams, streams == 1 ? "" : "s", secs);
        proto.upload = 1;
        up = run_phase("Upload", &proto, streams, secs);
        print_result("Upload", up);
    }
    return (down < 0 || up < 0) ? 1 : 0;
}
