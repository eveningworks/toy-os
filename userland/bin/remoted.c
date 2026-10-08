// remoted -- the remote desktop server: lets a VNC viewer on another
// computer see this screen and use its keyboard and mouse.
//
// STARTED ALWAYS, IDLE UNTIL SWITCHED ON, as ntpd is: /etc/remote.conf
// says which protocols are on, and this re-reads it every few seconds,
// so the switch in System Settings takes effect with no service to
// enable. A stock machine listens on nothing.
//
// ONE PROCESS PER VIEWER, inetd's shape: the listener accepts, judges
// the address, and spawns `remoted session vnc PEER` with the socket on
// fd 0 and fd 1. The session parses everything the stranger sends, so a
// bad message costs that one connection, never the listener; and each
// session has its own reader, which this TCP stack's timers need
// (inetd.c says why).
//
// The parts are in userland/remoted/ (rd.h).
#include <stdint.h>
#include "rt/sys.h"
#include "net_abi.h"
#include "lib/uargs.h"
#include "remoted/rd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>

#define SELF "/bin/remoted"
#define SLICE_MS 1000
// Two viewers at once at most: a session holds a socket and a TCP
// connection of the stack's eight, and a frame buffer of the screen's
// size twice over.
#define SESSIONS_MAX 2

static const struct uargs_cmd CMDS[] = {
    { "serve",   NULL,           "listen for viewers (what the service runs)" },
    { "status",  NULL,           "what is switched on, the ports and who is trusted" },
    { "session", "PROTOCOL PEER", "serve one viewer on fd 0/1 (the listener runs this)" },
    { 0 },
};

static const struct uargs_prog PROG = {
    .name = "remoted",
    .usage = "[COMMAND]",
    .summary = "The remote desktop server. Settings live in /etc/remote.conf\n"
               "(System Settings > Network > Remote Desktop).",
    .cmds = CMDS,
};

void rd_log(const char *fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof line) n = (int)sizeof line - 1;
    write(2, line, (size_t)n);
}

struct listener { int fd, port; };

static void stop(struct listener *l) {
    if (l->fd >= 0) close(l->fd);
    l->fd = -1;
    l->port = 0;
}

// Opens, moves or closes the listener to match `want` (0 = off).
static void reconcile(struct listener *l, int want, const char *proto) {
    if (l->fd >= 0 && l->port == want) return;
    if (l->fd >= 0) {
        rd_log("remoted: %s: stopped listening on %d\n", proto, l->port);
        stop(l);
    }
    if (!want) return;
    int fd = sys_socket(NET_ABI_AF_INET, NET_ABI_SOCK_STREAM, NET_ABI_IPPROTO_TCP);
    if (fd < 0) return;
    if (sys_bind(fd, 0, (uint16_t)want, 0) < 0 || sys_listen(fd) < 0) {
        rd_log("remoted: %s: cannot listen on port %d\n", proto, want);
        close(fd);
        return;
    }
    l->fd = fd;
    l->port = want;
    rd_log("remoted: %s: listening on port %d\n", proto, want);
}

static int serve(void) {
    struct listener vnc = { -1, 0 };
    int live = 0;
    for (;;) {
        int code;
        while (sys_waitpid_nohang(-1, &code) > 0) if (live > 0) live--;

        struct uremote_conf c;
        uremote_load(&c);
        reconcile(&vnc, c.vnc.enabled ? c.vnc.port : 0, "vnc");
        if (vnc.fd < 0) {
            sys_sleep_ms(SLICE_MS * 2);
            continue;
        }

        uint32_t peer = 0;
        uint16_t pport = 0;
        int s = sys_accept(vnc.fd, &peer, &pport, SLICE_MS);
        if (s < 0) continue;

        char ip[16];
        uremote_fmt_ip(peer, ip, sizeof ip);
        // `from = network` is judged before anything is spawned: a peer
        // from elsewhere gets a closed connection and no handshake.
        if (c.from == UREMOTE_FROM_NETWORK && !uremote_local(peer) && !uremote_trusted(&c, peer)) {
            rd_log("remoted: vnc: %s is not on this network -- refused\n", ip);
            close(s);
            continue;
        }
        if (live >= SESSIONS_MAX) {
            rd_log("remoted: vnc: %s refused -- %d viewers already\n", ip, live);
            close(s);
            continue;
        }
        char args[48];
        snprintf(args, sizeof args, "session vnc %u", peer);
        struct sys_spawn_opts o;
        sys_spawn_opts_init(&o);
        o.args = args;
        o.stdin_fd = s;
        o.stdout_fd = s;
        // ITS OWN SESSION, which is what makes it a remote one: the
        // kernel takes the peer from the socket we still hold and lists
        // the session (QUERY_REMOTESESS) until it exits -- the tray and
        // System Settings read that, not anything we say.
        o.pgid = PGID_NEW;
        o.flags = SPAWN_SETSID;
        if (sys_spawn_opts(SELF, &o) > 0) live++;
        else rd_log("remoted: vnc: could not start a session for %s\n", ip);
        close(s);   // the session holds its own reference
    }
    return 0;
}

static int status(void) {
    struct uremote_conf c;
    uremote_load(&c);
    static const char *const WHEN[] = { "ask", "ask unless trusted", "always allow" };
    printf("VNC   %s, port %d, password %s\n", c.vnc.enabled ? "on" : "off", c.vnc.port,
           c.vnc.password[0] ? "set" : "NOT SET (viewers are refused)");
    printf("RDP   %s, port %d\n", c.rdp.enabled ? "on" : "off", c.rdp.port);
    printf("When someone connects: %s; from %s%s\n", WHEN[c.when],
           c.from == UREMOTE_FROM_ANYWHERE ? "anywhere" : "this network only",
           c.view_only ? "; view only" : "");
    for (int i = 0; i < c.ntrusted; i++)
        printf("Trusted: %s%s%s\n", c.trusted[i].text, c.trusted[i].label[0] ? " -- " : "",
               c.trusted[i].label);
    return 0;
}

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    if (a.argc == 0 || !strcmp(a.argv[0], "serve")) return serve();
    if (!strcmp(a.argv[0], "status")) return status();
    // session PROTOCOL PEER
    if (a.argc != 3) return uargs_error(&PROG, "session needs PROTOCOL and PEER");
    struct uremote_conf c;
    uremote_load(&c);
    uint32_t peer = (uint32_t)strtoul(a.argv[2], 0, 10);
    if (!strcmp(a.argv[1], "vnc")) return rd_vnc_session(&c, peer);
    return uargs_error(&PROG, "no such protocol: %s", a.argv[1]);
}
