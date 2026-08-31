// tftpd -- RFC 1350 file transfer, both directions.
//
//     tftpd [-p <port>] [-r <root>]
//
// THE PAIR TO telnetd, and the same reason: a shell on the machine is
// only half of remote work. The other half is getting a rebuilt binary
// ONTO it and a log file OFF it, and neither is something a shell can
// do on its own. TFTP is what lab and netboot gear has always used for
// exactly this -- small enough to fit in a boot ROM, which is why it
// exists at all -- and `curl` speaks it, so the host side needs
// nothing written.
//
// NO AUTHENTICATION, LIKE THE PROTOCOL, and here that means anybody who
// can reach the port can replace any file the root allows. That is the
// same standing it has on real lab gear, and the service ships DISABLED
// for it (/usr/share/services/tftpd).
//
// WHAT IS NOT IMPLEMENTED: the option extension (RFC 2347/2348), so
// blocks are 512 bytes and a transfer is lockstep -- one round trip per
// block, which is a few seconds for a 200 KB binary and the reason
// `blksize` exists. `netascii` is accepted as a synonym for `octet`
// rather than translating: every caller here moves binaries, and a
// silent CRLF rewrite of an ELF is worse than not supporting a mode.
#include <stdint.h>
#include <stdarg.h>
#include "rt/sys.h"
#include "net_abi.h"
#include "lib/cmd.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// DIAGNOSTICS GO TO fd 2, NOT stdout. A service is spawned by init with
// no stdout anybody reads -- the `tftpd: serving ...` line printed at
// startup reached nothing at all, which is a poor way to find out that a
// bind failed. fd 2 is the kernel log (inetd.c says so), so this lands
// in `dmesg` where a daemon's log belongs.
static void logf(const char *fmt, ...) {
    va_list ap;
    char buf[192];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    sys_write(2, buf, strlen(buf));
}

#define USAGE "tftpd [-p <port>] [-r <root>] [-1]"

#define OP_RRQ   1
#define OP_WRQ   2
#define OP_DATA  3
#define OP_ACK   4
#define OP_ERROR 5

// RFC 1350 error codes. Only the ones this can actually reach.
#define ERR_NOTFOUND  1
#define ERR_ACCESS    2
#define ERR_FULL      3
#define ERR_ILLEGAL   4

#define BLKSIZE   512
#define PKT_MAX   (4 + BLKSIZE)
#define RETRIES   5
#define TIMEOUT_MS 2000

#define PATH_MAX_LEN 128

static char g_root[64] = "/";

// ANSWER FROM THE REQUEST PORT INSTEAD OF A NEW ONE (-1).
//
// RFC 1350 says a transfer gets a fresh ephemeral port -- the TID -- and
// that is the default here. It is also the single most common reason a
// TFTP transfer fails across a firewall: the reply arrives from a port
// the client never sent to, so a stateful filter sees a NEW inbound flow
// rather than a reply and drops it. Linux ships `nf_conntrack_tftp`
// purely to teach conntrack about this, and a host without it loaded
// silently eats every ACK. Measured here: the ACKs left the guest (its
// tx counter climbed) and never reached a client one hop away.
//
// -1 answers from the request socket, so the reply matches the tuple the
// client sent to and any filter accepts it. The cost is real and is why
// this is not the default: one transfer at a time, because port 69 is
// busy for the duration of it. Loading the conntrack helper on the host
// is the correct fix; this is the one that needs nothing from the host.
static int g_single_port;
static int g_req_fd = -1;

// One packet buffer per direction, static rather than automatic: a
// ring-3 stack is 16 KiB behind one guard page with a 2 KiB frame
// budget, and a 516-byte packet each way does not fit twice over.
static uint8_t g_rx[PKT_MAX];
static uint8_t g_tx[PKT_MAX];

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

static void send_error(int fd, uint32_t ip, uint16_t port, uint16_t code,
                       const char *msg) {
    uint32_t n = 0;
    put16(g_tx, OP_ERROR); n = 2;
    put16(g_tx + n, code); n += 2;
    uint32_t m = (uint32_t)strlen(msg);
    if (m > PKT_MAX - n - 1) m = PKT_MAX - n - 1;
    memcpy(g_tx + n, msg, m); n += m;
    g_tx[n++] = 0;
    sys_sendto(fd, g_tx, n, ip, port);
}

// A REQUEST'S PATH IS UNTRUSTED INPUT and is refused rather than
// sanitised: any `..` at all, an empty name, or one that does not fit
// makes the request an error. Stripping `..` instead would leave the
// question of what the stripped path now names, which is exactly the
// class of bug path handling keeps producing elsewhere.
static int resolve(const char *name, char *out, uint32_t cap) {
    if (!name[0]) return 0;
    if (strstr(name, "..")) return 0;
    uint32_t rl = (uint32_t)strlen(g_root);
    int root_slash = rl && g_root[rl - 1] == '/';
    int name_slash = name[0] == '/';
    const char *sep = (root_slash || name_slash) ? "" : "/";
    if (root_slash && name_slash) name++;          // not both
    if (rl + strlen(sep) + strlen(name) + 1 > cap) return 0;
    out[0] = 0;
    strcat(out, g_root);
    strcat(out, sep);
    strcat(out, name);
    return 1;
}

// A transfer gets its own socket, and that is the protocol rather than
// a choice: RFC 1350 calls the ephemeral port a TID, and the client
// sends every later packet to the one the server's first reply came
// FROM. Answering from port 69 would work for one exchange and then
// collide with the next client's request.
static int open_tid(void) {
    if (g_single_port) return g_req_fd;
    int fd = sys_socket(NET_ABI_AF_INET, NET_ABI_SOCK_DGRAM, NET_ABI_IPPROTO_UDP);
    if (fd < 0) { logf("tftpd: no TID socket\n"); return -1; }
    int p = sys_bind(fd, 0, 0, 0);
    if (p < 0) { logf("tftpd: TID bind failed (%d)\n", p); sys_close(fd); return -1; }
    return fd;
}

// A NO-OP IN SINGLE-PORT MODE, and it has to be: closing the TID would
// close the REQUEST socket, and the server would answer exactly one
// transfer and then hear nothing again. That is not hypothetical -- it
// is what the experiment that established the firewall diagnosis did.
static void close_tid(int fd) {
    if (!g_single_port && fd >= 0) sys_close(fd);
}

// --- WRQ: the client writes a file to us -------------------------------

static void do_write(uint32_t ip, uint16_t port, const char *path) {
    int tid = open_tid();
    if (tid < 0) return;

    int fd = sys_open(path, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    if (fd < 0) {
        send_error(tid, ip, port, ERR_ACCESS, "cannot create");
        close_tid(tid);
        return;
    }

    uint16_t block = 0;
    uint64_t total = 0;
    for (;;) {
        // ACK the block just taken (0 acknowledges the request itself),
        // then wait for the next. The ACK is what a retransmit repeats,
        // so it is built once and resent unchanged.
        put16(g_tx, OP_ACK);
        put16(g_tx + 2, block);

        int done = 0;
        int got = 0;
        for (int try = 0; try < RETRIES && !got; try++) {
            int64_t sr = sys_sendto(tid, g_tx, 4, ip, port);
            if (sr < 0) logf("tftpd: ACK %u sendto -> %d\n", block, (int)sr);
            uint32_t src = 0; uint16_t sport = 0;
            int64_t n = sys_recvfrom(tid, g_rx, sizeof g_rx, &src, &sport,
                                     TIMEOUT_MS);
            if (n <= 0) continue;                      // timed out: resend
            if (src != ip || sport != port) continue;  // a different TID
            if (n < 4) continue;
            uint16_t op = get16(g_rx);
            if (op == OP_ERROR) { done = 1; got = 1; break; }
            if (op != OP_DATA) continue;
            uint16_t b = get16(g_rx + 2);
            if (b == block) continue;      // our ACK was lost; resend it
            if (b != (uint16_t)(block + 1)) continue;
            block = b;
            uint32_t len = (uint32_t)n - 4;
            if (len && sys_write(fd, g_rx + 4, len) != (int64_t)len) {
                send_error(tid, ip, port, ERR_FULL, "write failed");
                sys_close(fd); close_tid(tid);
                return;
            }
            total += len;
            got = 1;
            // A SHORT BLOCK ENDS THE TRANSFER -- including a zero-length
            // one, which is how a file that is an exact multiple of 512
            // is terminated. Its ACK is still owed, so the loop runs
            // once more before leaving.
            if (len < BLKSIZE) done = 2;
        }
        if (!got) break;                   // the client stopped answering
        if (done) {
            if (done == 2) { put16(g_tx, OP_ACK); put16(g_tx + 2, block);
                             sys_sendto(tid, g_tx, 4, ip, port); }
            break;
        }
    }

    sys_close(fd);
    close_tid(tid);
    logf("tftpd: wrote %s, %llu bytes\n", path, (unsigned long long)total);
}

// --- RRQ: the client reads a file from us ------------------------------

static void do_read(uint32_t ip, uint16_t port, const char *path) {
    int tid = open_tid();
    if (tid < 0) return;

    int fd = sys_open(path, 0);
    if (fd < 0) {
        send_error(tid, ip, port, ERR_NOTFOUND, "no such file");
        close_tid(tid);
        return;
    }

    uint16_t block = 0;
    uint64_t total = 0;
    for (;;) {
        int64_t len = sys_read(fd, g_tx + 4, BLKSIZE);
        if (len < 0) len = 0;
        block++;
        put16(g_tx, OP_DATA);
        put16(g_tx + 2, block);

        int acked = 0;
        for (int try = 0; try < RETRIES && !acked; try++) {
            sys_sendto(tid, g_tx, 4 + (uint32_t)len, ip, port);
            uint32_t src = 0; uint16_t sport = 0;
            int64_t n = sys_recvfrom(tid, g_rx, sizeof g_rx, &src, &sport,
                                     TIMEOUT_MS);
            if (n <= 0) continue;
            if (src != ip || sport != port) continue;
            if (n < 4) continue;
            if (get16(g_rx) == OP_ERROR) { acked = -1; break; }
            if (get16(g_rx) == OP_ACK && get16(g_rx + 2) == block) acked = 1;
        }
        if (acked != 1) break;
        total += (uint64_t)len;
        if (len < BLKSIZE) break;          // the short block ends it
    }

    sys_close(fd);
    close_tid(tid);
    logf("tftpd: sent %s, %llu bytes\n", path, (unsigned long long)total);
}

// --- the request port --------------------------------------------------

// A request is | opcode | filename | 0 | mode | 0 |. Both strings come
// off the wire, so the terminators are checked rather than assumed: an
// unterminated name would otherwise be read past the end of the packet.
static int parse_request(const uint8_t *p, uint32_t n, const char **name,
                         const char **mode) {
    if (n < 4) return 0;
    uint32_t i = 2;
    *name = (const char *)p + i;
    while (i < n && p[i]) i++;
    if (i >= n) return 0;
    i++;
    if (i >= n) return 0;
    *mode = (const char *)p + i;
    while (i < n && p[i]) i++;
    return i < n;
}

int main(int argc, char **argv) {
    int port = 69;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) {
            if (strlen(argv[i + 1]) >= sizeof g_root) {
                printf("tftpd: root path too long\n");
                return 1;
            }
            strcpy(g_root, argv[++i]);
        } else if (!strcmp(argv[i], "-1")) g_single_port = 1;
        else { cmd_usage(USAGE); return 1; }
    }
    if (port <= 0 || port > 65535) { cmd_usage(USAGE); return 1; }

    int fd = sys_socket(NET_ABI_AF_INET, NET_ABI_SOCK_DGRAM, NET_ABI_IPPROTO_UDP);
    if (fd < 0) { logf("tftpd: no socket\n"); return 1; }
    int bound = sys_bind(fd, 0, (uint16_t)port, 0);
    if (bound < 0) {
        logf("tftpd: cannot bind port %d (%d)\n", port, bound);
        sys_close(fd);
        return 1;
    }
    g_req_fd = fd;
    logf("tftpd: serving %s on port %d%s\n", g_root, bound,
         g_single_port ? " (single port)" : "");
    sys_notify_ready();

    for (;;) {
        uint32_t ip = 0; uint16_t cport = 0;
        int64_t n = sys_recvfrom(fd, g_rx, sizeof g_rx, &ip, &cport, 0);
        if (n <= 0) continue;

        logf("tftpd: op %u from %u.%u.%u.%u:%u, %d bytes\n",
             (unsigned)get16(g_rx), (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
             (ip >> 8) & 0xFF, ip & 0xFF, (unsigned)cport, (int)n);

        const char *name = 0, *mode = 0;
        if (!parse_request(g_rx, (uint32_t)n, &name, &mode)) {
            send_error(fd, ip, cport, ERR_ILLEGAL, "malformed request");
            continue;
        }
        uint16_t op = get16(g_rx);
        char path[PATH_MAX_LEN];
        if (!resolve(name, path, sizeof path)) {
            send_error(fd, ip, cport, ERR_ACCESS, "bad path");
            continue;
        }
        if (op == OP_WRQ)      do_write(ip, cport, path);
        else if (op == OP_RRQ) do_read(ip, cport, path);
        else send_error(fd, ip, cport, ERR_ILLEGAL, "not a request");
    }
}
