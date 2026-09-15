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
// OPTIONS ARE NEGOTIATED (RFC 2347), and two of them matter here.
//
// `blksize` (RFC 2348) and `windowsize` (RFC 7440), because 512-byte
// lock-step is not slow for the reason it looks slow. Measured against
// the bare-metal laptop: 32 ms per block, of which ~1.8 ms is the
// network. The rest is a 10 ms scheduler tick per round trip -- a
// blocked process runs at the next tick, not when the packet lands --
// and ~22 ms of filesystem transaction, because every block was its own
// write(). A 4.7 MB kernel took five minutes.
//
// So all three are addressed: bigger blocks (2.8x fewer round trips AND
// writes), a window (one round trip per N blocks instead of per block),
// and a 64 KiB write buffer (9,272 transactions become ~72).
//
// **1428 IS THE CEILING AND IT IS NOT ARBITRARY.** kernel/net/ipv4.c
// does NOT fragment or reassemble -- a fragmented datagram is DROPPED --
// so a block that does not fit the MTU does not go slowly, it does not
// go at all. SYS_NET_MSG_MAX is 1472, so 1468 is the true maximum and
// 1428 is what RFC 2348 names, leaving room for a tunnel in the path.
//
// `netascii` is accepted as a synonym for `octet` rather than
// translating: every caller here moves binaries, and a silent CRLF
// rewrite of an ELF is worse than not supporting a mode.
#include <stdint.h>
#include <stdarg.h>
#include "rt/sys.h"
#include "query_abi.h"   // QUERY_REMOTE_XFER
#include "net_abi.h"
#include "lib/cmd.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

// DIAGNOSTICS GO TO fd 2, NOT stdout. A service is spawned by init with
// no stdout anybody reads -- the `tftpd: serving ...` line printed at
// startup reached nothing at all, which is a poor way to find out that a
// bind failed. fd 2 is the kernel log (inetd.c says so), so this lands
// in `dmesg` where a daemon's log belongs.

// One line per completed transfer, for the tray's remote-activity view.
// Separate from logf() above: that is diagnostics in the kernel log,
// this is a record about a REMOTE party and belongs where the indicator
// can find it.
static void remote_note(uint32_t ip, const char *verb, const char *path,
                        uint64_t bytes) {
    char line[120];
    snprintf(line, sizeof line, "%s %s (%llu bytes)", verb, path,
             (unsigned long long)bytes);
    sys_remote_log(QUERY_REMOTE_XFER, ip, line);
}

static void logf(const char *fmt, ...) {
    va_list ap;
    char buf[192];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    write(2, buf, strlen(buf));
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

#define OP_OACK  6

// The default a client that negotiates nothing still gets (RFC 1350),
// and the most we will agree to. See the file comment for why 1428 and
// not 1468.
#define BLKSIZE_DEFAULT 512
#define BLKSIZE_MAX     1428
#define BLKSIZE_MIN     8       // RFC 2348's floor

// RFC 7440, AND THE CEILING IS THE RECEIVER'S SOCKET QUEUE, not a
// number picked for speed.
//
// kernel/net/socket.c holds SOCK_QUEUE (4) datagrams per socket and
// leaves one slot unused, so three arrive and the rest of a window is
// DROPPED ON ARRIVAL -- not lost in the network, discarded at the door.
// A window of 16 measured 611 seconds for 1 MiB against 66 for plain
// lock-step, because every round trip delivered three blocks and
// retransmitted thirteen. A window larger than the receiver can hold is
// slower than no window at all.
//
// So this is 3, and raising it means raising SOCK_QUEUE first. The
// number is here rather than derived because a socket's depth is not
// something ring 3 can ask for; if that changes, these two must move
// together.
#define WINDOW_DEFAULT  1
#define WINDOW_MAX      3

#define PKT_MAX   (4 + BLKSIZE_MAX)
#define RETRIES   5
#define TIMEOUT_MS 2000

// WRITES ARE BUFFERED, and that is the larger half of the speedup.
// Every fs_write is one complete TFS3 transaction ending in two
// barriers, so a block per write made a 4.7 MB push 9,272 of them.
// SYS_WRITE_MAX is the size the ABI comment already says a write should
// be (abi/syscall_abi.h).
#define WRBUF_MAX 65536

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

// What this transfer negotiated. Reset per request, because a client
// that asks for nothing must get RFC 1350's defaults even if the
// previous one asked for everything.
static uint32_t g_blksize = BLKSIZE_DEFAULT;
static uint32_t g_window  = WINDOW_DEFAULT;

// The send window, so a retransmit does not have to seek the file back.
// WINDOW_MAX * BLKSIZE_MAX is ~23 KB of .bss, which costs nothing in
// the image and keeps the resend path a memcpy rather than an lseek --
// the latter being a syscall that can fail halfway through recovering
// from a failure.
static uint8_t g_win[WINDOW_MAX][PKT_MAX];
static uint32_t g_win_len[WINDOW_MAX];

static uint8_t g_wrbuf[WRBUF_MAX];
static uint32_t g_wrbuf_len;

// Flushes the write buffer. Returns 0 on failure, having reported
// nothing -- the caller owns the error packet, since only it knows the
// TID to send it on.
static int wrbuf_flush(int fd) {
    if (!g_wrbuf_len) return 1;
    int64_t n = write(fd, g_wrbuf, g_wrbuf_len);
    int ok = n == (int64_t)g_wrbuf_len;
    g_wrbuf_len = 0;
    return ok;
}

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
    if (p < 0) { logf("tftpd: TID bind failed (%d)\n", p); close(fd); return -1; }
    return fd;
}

// A NO-OP IN SINGLE-PORT MODE, and it has to be: closing the TID would
// close the REQUEST socket, and the server would answer exactly one
// transfer and then hear nothing again. That is not hypothetical -- it
// is what the experiment that established the firewall diagnosis did.
static void close_tid(int fd) {
    if (!g_single_port && fd >= 0) close(fd);
}

// --- WRQ: the client writes a file to us -------------------------------

// A TRANSFER LANDS ON A TEMPORARY AND IS RENAMED OVER THE TARGET, and
// that is not tidiness -- it is the difference between replacing a file
// and destroying one.
//
// Writing into the target with O_TRUNC has two failure modes this
// removes. A reader that opens the file mid-transfer gets a half-written
// one, and ld-toy maps a library's segments FILE-BACKED, so overwriting
// a live /lib/*.so under a running program can fault in the new bytes
// beneath the old relocations. And an ABORTED transfer -- a timeout, a
// client that goes away -- used to leave the target truncated, so a
// failed push of /boot/boot/kernel.bin destroyed the kernel it was
// replacing. Now the original is untouched until the last byte is in.
//
// This is dpkg's `.dpkg-new` plus a rename, for the same reason.
//
// RENAME HERE IS CREATE-ONLY, which is why publishing is three steps
// rather than one. fs_rename() refuses an existing destination in all
// three backends -- TFS3, FAT32 and ramfs each say "destination taken"
// -- so a plain rename over the target would fail on every overwrite,
// which is the normal case. The old file is moved aside first, the new
// one takes the name, and only then is the old one dropped. Nothing is
// destroyed until the replacement is in place, and a crash in the
// two-operation gap leaves the original under a name that says what it
// is rather than leaving nothing at all.
#define WR_SUFFIX  ".tftp-new"
#define OLD_SUFFIX ".tftp-old"

// Builds "<path><suffix>", or 0 if it would not fit. REFUSED rather
// than truncated: a truncated name is a different file, and renaming
// that over the target would put the transfer somewhere nobody asked
// for.
static int suffixed(const char *path, const char *suffix,
                    char *out, unsigned long cap) {
    unsigned long n = strlen(path), sn = strlen(suffix);
    if (n + sn + 1 > cap) return 0;
    memcpy(out, path, n);
    memcpy(out + n, suffix, sn + 1);
    return 1;
}

static void do_write(uint32_t ip, uint16_t port, const char *path,
                     uint32_t oack_len) {
    int tid = open_tid();
    if (tid < 0) return;

    char tmp[PATH_MAX_LEN + sizeof WR_SUFFIX];
    char aside[PATH_MAX_LEN + sizeof OLD_SUFFIX];
    if (!suffixed(path, WR_SUFFIX, tmp, sizeof tmp) ||
        !suffixed(path, OLD_SUFFIX, aside, sizeof aside)) {
        send_error(tid, ip, port, ERR_ACCESS, "path too long");
        close_tid(tid);
        return;
    }
    // A leftover from a crash would make the move-aside below fail and
    // wedge every later push of this path.
    remove(aside);

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        send_error(tid, ip, port, ERR_ACCESS, "cannot create");
        close_tid(tid);
        return;
    }

    g_wrbuf_len = 0;
    uint16_t expect = 1;          // the next block we want
    uint16_t acked  = 0;          // the last block we have acknowledged
    uint64_t total = 0;
    int final = 0;

    // THE OACK REPLACES THE FIRST ACK, and only the first (RFC 2347): a
    // client that negotiated waits for it and answers with DATA 1, so
    // sending ACK 0 as well would look like a duplicate.
    uint8_t first[PKT_MAX];
    uint32_t first_len;
    if (oack_len) {
        memcpy(first, g_tx, oack_len);
        first_len = oack_len;
    } else {
        put16(first, OP_ACK);
        put16(first + 2, 0);
        first_len = 4;
    }
    sys_sendto(tid, first, first_len, ip, port);

    int tries = 0;
    for (;;) {
        uint32_t src = 0; uint16_t sport = 0;
        int64_t n = sys_recvfrom(tid, g_rx, sizeof g_rx, &src, &sport,
                                 TIMEOUT_MS);
        if (n <= 0) {
            // A WINDOW STALLS SILENTLY WHEN A BLOCK IS LOST -- the
            // sender is waiting for an ACK it will not get, because we
            // stopped advancing. So a timeout ACKs what we DO have,
            // which is what tells it where to resume (RFC 7440).
            if (++tries > RETRIES) break;
            if (acked == 0 && expect == 1) sys_sendto(tid, first, first_len, ip, port);
            else { put16(g_tx, OP_ACK); put16(g_tx + 2, acked);
                   sys_sendto(tid, g_tx, 4, ip, port); }
            continue;
        }
        tries = 0;
        if (src != ip || sport != port) continue;
        if (n < 4) continue;
        uint16_t op = get16(g_rx);
        if (op == OP_ERROR) break;
        if (op != OP_DATA) continue;

        uint16_t b = get16(g_rx + 2);
        if (b != expect) {
            // Either a duplicate of something we already took, or a
            // block from beyond a gap. Neither may be written: writing
            // the second would put it at the wrong offset. Re-ACK so a
            // sender waiting on a lost ACK moves.
            if (b <= acked) { put16(g_tx, OP_ACK); put16(g_tx + 2, acked);
                              sys_sendto(tid, g_tx, 4, ip, port); }
            continue;
        }

        uint32_t len = (uint32_t)n - 4;
        if (len) {
            if (g_wrbuf_len + len > WRBUF_MAX && !wrbuf_flush(fd)) {
                send_error(tid, ip, port, ERR_FULL, "write failed");
                close(fd); close_tid(tid);
                remove(tmp);          // the target keeps what it had
                return;
            }
            memcpy(g_wrbuf + g_wrbuf_len, g_rx + 4, len);
            g_wrbuf_len += len;
            total += len;
        }
        expect++;
        if (len < g_blksize) final = 1;   // the short block ends it

        // ACK once per window, and always on the last block.
        if (final || (uint16_t)(expect - 1 - acked) >= (uint16_t)g_window) {
            acked = (uint16_t)(expect - 1);
            put16(g_tx, OP_ACK);
            put16(g_tx + 2, acked);
            sys_sendto(tid, g_tx, 4, ip, port);
        }
        if (final) break;
    }

    int ok = wrbuf_flush(fd);
    close(fd);
    close_tid(tid);

    // ONLY A TRANSFER THAT REACHED ITS LAST BLOCK IS PUBLISHED. The loop
    // above also breaks on a timeout and on the client's own ERROR, and
    // neither of those has the whole file -- so `final` is the condition
    // rather than "we stopped receiving".
    if (!ok || !final) {
        remove(tmp);
        logf("tftpd: %s: transfer did not complete (%llu bytes) -- the "
             "existing file is unchanged\n", path, (unsigned long long)total);
        return;
    }
    // Move the old one aside, publish, then drop it. Each step is undone
    // if the next fails, so the target is never left missing on a path
    // this code can see.
    int had_old = (rename(path, aside) == 0);
    if (rename(tmp, path) != 0) {
        if (had_old) rename(aside, path);
        remove(tmp);
        logf("tftpd: %s: could not replace the file\n", path);
        return;
    }
    if (had_old) remove(aside);
    logf("tftpd: wrote %s, %llu bytes (blksize %u, window %u)\n", path,
         (unsigned long long)total, g_blksize, g_window);
    // THE OTHER HALF OF WHAT A FLASH DOES, for the tray's remote view.
    // The peer is passed EXPLICITLY because tftpd is a service rather
    // than a session -- naming the connection it is serving is what
    // makes the kernel keep the record (abi/syscall_abi.h).
    remote_note(ip, "put", path, total);
}

// --- RRQ: the client reads a file from us ------------------------------

static void do_read(uint32_t ip, uint16_t port, const char *path,
                    uint32_t oack_len) {
    int tid = open_tid();
    if (tid < 0) return;

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        send_error(tid, ip, port, ERR_NOTFOUND, "no such file");
        close_tid(tid);
        return;
    }

    // The OACK is acknowledged by an ACK 0 before any data flows.
    if (oack_len) {
        uint8_t oack[PKT_MAX];
        memcpy(oack, g_tx, oack_len);
        int ready = 0;
        for (int try = 0; try < RETRIES && !ready; try++) {
            sys_sendto(tid, oack, oack_len, ip, port);
            uint32_t src = 0; uint16_t sport = 0;
            int64_t n = sys_recvfrom(tid, g_rx, sizeof g_rx, &src, &sport,
                                     TIMEOUT_MS);
            if (n < 4 || src != ip || sport != port) continue;
            if (get16(g_rx) == OP_ERROR) { close(fd); close_tid(tid); return; }
            if (get16(g_rx) == OP_ACK && get16(g_rx + 2) == 0) ready = 1;
        }
        if (!ready) { close(fd); close_tid(tid); return; }
    }

    uint16_t base = 1;            // first block of the window in flight
    uint64_t total = 0;
    int eof = 0;                  // the short block has been READ
    uint16_t last = 0;            // ...and it was this one
    int tries = 0;

    for (;;) {
        // Fill and send the window. Blocks are kept so a retransmit is a
        // resend of the same bytes rather than a seek.
        uint32_t filled = 0;
        for (uint32_t i = 0; i < g_window && !eof; i++) {
            int64_t len = read(fd, g_win[i] + 4, g_blksize);
            if (len < 0) len = 0;
            uint16_t b = (uint16_t)(base + i);
            put16(g_win[i], OP_DATA);
            put16(g_win[i] + 2, b);
            g_win_len[i] = 4 + (uint32_t)len;
            filled++;
            if ((uint32_t)len < g_blksize) { eof = 1; last = b; }
        }
        for (uint32_t i = 0; i < filled; i++)
            sys_sendto(tid, g_win[i], g_win_len[i], ip, port);

        uint32_t src = 0; uint16_t sport = 0;
        int64_t n = sys_recvfrom(tid, g_rx, sizeof g_rx, &src, &sport,
                                 TIMEOUT_MS);
        if (n <= 0) {
            if (++tries > RETRIES) break;
            continue;                       // resend the same window
        }
        if (src != ip || sport != port) continue;
        if (n < 4) continue;
        if (get16(g_rx) == OP_ERROR) break;
        if (get16(g_rx) != OP_ACK) continue;

        uint16_t a = get16(g_rx + 2);
        // Anything before the window is a stale ACK; anything past its
        // end cannot have been received. Both are ignored rather than
        // trusted -- an out-of-range ACK would otherwise skip data.
        if ((uint16_t)(a - base) >= filled) continue;
        tries = 0;

        // Count what this ACK confirms, then start the next window
        // after it. A partial ACK means blocks were lost: the sender
        // resumes from there, which is RFC 7440's whole recovery story.
        uint32_t confirmed = (uint32_t)(a - base) + 1;
        for (uint32_t i = 0; i < confirmed; i++) total += g_win_len[i] - 4;
        if (eof && a == last) break;
        if (confirmed < filled) eof = 0;    // rewind: refill from a+1
        if (confirmed < filled) {
            // The file position must go back to where the unacked block
            // started, since the next window re-reads from there.
            lseek(fd, (int64_t)total, 0 /* SEEK_SET */);
        }
        base = (uint16_t)(a + 1);
    }

    close(fd);
    close_tid(tid);
    logf("tftpd: sent %s, %llu bytes (blksize %u, window %u)\n", path,
         (unsigned long long)total, g_blksize, g_window);
    remote_note(ip, "get", path, total);
}

// --- the request port --------------------------------------------------

// A request is | opcode | filename | 0 | mode | 0 |. Both strings come
// off the wire, so the terminators are checked rather than assumed: an
// unterminated name would otherwise be read past the end of the packet.
static int parse_request(const uint8_t *p, uint32_t n, const char **name,
                         const char **mode, uint32_t *opt_start) {
    if (n < 4) return 0;
    uint32_t i = 2;
    *name = (const char *)p + i;
    while (i < n && p[i]) i++;
    if (i >= n) return 0;
    i++;
    if (i >= n) return 0;
    *mode = (const char *)p + i;
    while (i < n && p[i]) i++;
    if (i >= n) return 0;
    *opt_start = i + 1;   // the first option pair, if there is one
    return 1;
}

// RFC 2347: after `name\0mode\0` the request may carry `opt\0value\0`
// pairs. Sets the negotiated globals and builds the OACK naming ONLY
// what was accepted -- a server must not acknowledge an option it did
// not honour, since the client then assumes it is in force.
//
// Returns the OACK's length, or 0 when nothing was negotiated (in which
// case RFC 1350's defaults stand and no OACK is sent at all).
static uint32_t parse_options(const uint8_t *p, uint32_t n, uint32_t start,
                              uint64_t fsize, int have_size) {
    g_blksize = BLKSIZE_DEFAULT;
    g_window  = WINDOW_DEFAULT;

    uint32_t out = 0;
    put16(g_tx, OP_OACK);
    out = 2;

    uint32_t i = start;
    while (i < n) {
        const char *opt = (const char *)p + i;
        while (i < n && p[i]) i++;
        if (i >= n) break;
        i++;
        if (i >= n) break;
        const char *val = (const char *)p + i;
        while (i < n && p[i]) i++;
        if (i >= n) break;
        i++;

        char ack[24];
        uint32_t acked = 0;
        if (!strcmp(opt, "blksize")) {
            long v = atol(val);
            if (v < BLKSIZE_MIN) v = BLKSIZE_MIN;
            if (v > BLKSIZE_MAX) v = BLKSIZE_MAX;   // clamped, and the
            g_blksize = (uint32_t)v;                // OACK says the real
            snprintf(ack, sizeof ack, "%u", g_blksize);  // value
            acked = 1;
        } else if (!strcmp(opt, "windowsize")) {
            long v = atol(val);
            if (v < 1) v = 1;
            if (v > WINDOW_MAX) v = WINDOW_MAX;
            g_window = (uint32_t)v;
            snprintf(ack, sizeof ack, "%u", g_window);
            acked = 1;
        } else if (!strcmp(opt, "tsize") && have_size) {
            // RFC 2349. Answered only for a READ, where the size is
            // known; a client's WRQ tsize is its own claim and echoing
            // it back would promise a check this does not make.
            snprintf(ack, sizeof ack, "%llu", (unsigned long long)fsize);
            acked = 1;
        }
        if (!acked) continue;   // silently unnegotiated, per RFC 2347

        uint32_t ol = (uint32_t)strlen(opt) + 1;
        uint32_t al = (uint32_t)strlen(ack) + 1;
        if (out + ol + al > PKT_MAX) continue;
        memcpy(g_tx + out, opt, ol);  out += ol;
        memcpy(g_tx + out, ack, al);  out += al;
    }
    return out > 2 ? out : 0;
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
        close(fd);
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
        uint32_t opt_start = 0;
        if (!parse_request(g_rx, (uint32_t)n, &name, &mode, &opt_start)) {
            send_error(fd, ip, cport, ERR_ILLEGAL, "malformed request");
            continue;
        }
        uint16_t op = get16(g_rx);
        char path[PATH_MAX_LEN];
        if (!resolve(name, path, sizeof path)) {
            send_error(fd, ip, cport, ERR_ACCESS, "bad path");
            continue;
        }

        // tsize is answerable only for a READ, and only if the file is
        // there -- so the size is looked up before the options are
        // parsed rather than promised and then found missing.
        uint64_t fsize = 0;
        int have_size = 0;
        if (op == OP_RRQ) {
            struct sys_stat st;
            if (sys_stat(path, &st) == 0) { fsize = st.size; have_size = 1; }
        }
        uint32_t oack_len = parse_options(g_rx, (uint32_t)n, opt_start,
                                          fsize, have_size);

        if (op == OP_WRQ)      do_write(ip, cport, path, oack_len);
        else if (op == OP_RRQ) do_read(ip, cport, path, oack_len);
        else send_error(fd, ip, cport, ERR_ILLEGAL, "not a request");
    }
}
