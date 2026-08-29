// inetd -- accept connections on a port and hand each one to its own
// process, with the socket on fd 0 and fd 1.
//
// The one concurrency this kernel can express without `fork`. A handler
// is an ordinary FILTER: it reads the client from stdin and writes the
// client to stdout, so `inetd -p 7 /bin/cat` is a real echo server and
// nothing in `cat` knows about sockets. That works precisely because
// only 0/1/2 cross a spawn (kernel/proc/syscall_fd.c's fd_inherit).
//
// fd 2 IS LEFT ALONE, and that is the useful half of the asymmetry: it
// stays the kernel log, so a handler's diagnostics reach `dmesg` and
// never reach the client.
//
// IT ALSO GIVES EVERY CONNECTION A READER, which is a correctness
// property here rather than a performance one. A connection's
// retransmission timers are driven by the process blocked reading it
// (kernel/net/tcp.c), so a server holding several connections and
// reading one leaves the rest to the idle loop. One process each means
// none of them is unattended.
#include <stdint.h>
#include "rt/sys.h"
#include "net_abi.h"
#include "lib/cmd.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define USAGE "inetd -p <port> [-c <children>] <program> [args...]"

// The ceiling is the STACK's, not a policy: SOCK_MAX and TCP_MAX_CONNS
// are 8 apiece and a listener costs one of each, with TCP_BACKLOG (2)
// more held by connections that have handshaked and not been accepted.
// Above six, accept() starts failing for reasons an operator cannot see.
#define CHILDREN_MAX     6
#define CHILDREN_DEFAULT 4

// Long enough that an idle server is not spinning, short enough that a
// child that exited while nothing was connecting is still reaped
// promptly -- this is the only thing that wakes the reap.
#define ACCEPT_SLICE_MS 1000

static int reap(int live) {
    for (;;) {
        int code = 0;
        int pid = sys_waitpid_nohang(-1, &code);
        if (pid <= 0) return live;
        if (live > 0) live--;
    }
}

int main(int argc, char **argv) {
    int port = 0, cap = CHILDREN_DEFAULT, i = 1;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) cap = atoi(argv[++i]);
        else break;
    }
    if (port <= 0 || port > 65535 || i >= argc) { cmd_usage(USAGE); return 1; }
    if (cap < 1 || cap > CHILDREN_MAX) {
        printf("inetd: -c must be 1..%d (the socket and connection tables are %d wide)\n",
               CHILDREN_MAX, CHILDREN_MAX);
        return 1;
    }

    const char *prog = argv[i++];
    // The handler's arguments, rejoined: SYS_SPAWN takes one
    // whitespace-separated string, not a vector.
    char args[192];
    args[0] = 0;
    for (int n = 0; i < argc; i++) {
        n += snprintf(args + n, sizeof args - (size_t)n, "%s%s",
                      n ? " " : "", argv[i]);
        if (n >= (int)sizeof args) { cmd_usage(USAGE); return 1; }
    }

    int lis = sys_socket(NET_ABI_AF_INET, NET_ABI_SOCK_STREAM, NET_ABI_IPPROTO_TCP);
    if (lis < 0) { cmd_fail("inetd", "socket"); return 1; }
    if (sys_bind(lis, 0, (uint16_t)port, 0) < 0) { cmd_fail("inetd", "bind"); return 1; }
    if (sys_listen(lis) < 0) { cmd_fail("inetd", "listen"); return 1; }

    printf("inetd: port %d -> %s (up to %d at once) -- Ctrl-C to stop\n",
           port, prog, cap);

    int live = 0;
    for (;;) {
        live = reap(live);

        // AT THE CAP, STOP ACCEPTING. The connection waits in the TCP
        // backlog and, past that, the SYN is dropped and the client's
        // own retransmission covers it -- which is the behaviour the
        // stack already chose for a full backlog, rather than a second
        // refusal policy that turns a burst into a hard failure.
        if (live >= cap) {
            int code = 0;
            // _intr, NOT the retrying sys_waitpid(): Ctrl-C has to be
            // able to end a server that is sitting at its cap, and the
            // plain one goes back to sleep on -EINTR.
            int done = sys_waitpid_intr(-1, &code);
            if (done > 0) live--;
            else if (sys_errno() == EINTR) break;
            // NO CHILDREN AT ALL means the count was wrong, not that we
            // should ask again -- without this the loop spins on a
            // permanent -1 and the server is wedged with nothing
            // running. Believe the kernel over the counter.
            else live = 0;
            continue;
        }

        uint32_t peer = 0;
        uint16_t peer_port = 0;
        int c = sys_accept(lis, &peer, &peer_port, ACCEPT_SLICE_MS);
        if (c < 0) {
            if (sys_errno() == EAGAIN) continue;   // the slice expired
            if (sys_errno() == EINTR) break;       // Ctrl-C
            cmd_fail("inetd", "accept");
            break;
        }

        struct sys_spawn_opts o;
        sys_spawn_opts_init(&o);
        o.args = args[0] ? args : 0;
        o.env = environ;
        o.stdin_fd = c;
        o.stdout_fd = c;
        int pid = sys_spawn_opts(prog, &o);
        if (pid > 0) {
            live++;
            printf("inetd: %u.%u.%u.%u:%u -> %s [%d]\n",
                   (peer >> 24) & 0xFF, (peer >> 16) & 0xFF,
                   (peer >> 8) & 0xFF, peer & 0xFF, peer_port, prog, pid);
        } else {
            cmd_fail("inetd", prog);
        }

        // OURS GOES NOW, WHOEVER WON. The child holds its own references
        // to the same open file, so this drops the connection to the
        // child alone -- and keeping it would both stop the client ever
        // seeing a close and exhaust an eight-entry socket table within
        // a handful of requests.
        sys_close(c);
    }

    sys_close(lis);
    return 0;
}
