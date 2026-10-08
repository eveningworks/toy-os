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
#include "lib/uconf.h"
#include "utls.h"
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

// VeNCrypt's key and certificate, made HERE rather than in a session so
// no viewer waits on it, and the fingerprint recorded in /etc/remote.conf
// for System Settings to show. A viewer checks the certificate against
// what it DIALLED, so the certificate names this machine's addresses and
// is re-made when they change (a DHCP lease, a cable).
static void identity(const struct uremote_conf *c) {
    static char last[UTLS_NAMES_MAX * 16], key[sizeof last];   // static: serve()'s frame
    static char fp[100];
    static char ips[UTLS_NAMES_MAX][16];
    const char *names[UTLS_NAMES_MAX];
    int n = 0;
    names[n++] = "toy-os";
    struct query_netdev d;
    QUERY_FOREACH(QUERY_NETDEV, d, i) {
        if (!d.ip || n >= UTLS_NAMES_MAX - 1) continue;
        uremote_fmt_ip((uint32_t)d.ip, ips[n], sizeof ips[n]);
        names[n] = ips[n];
        n++;
    }
    // No address yet (DHCP still asking): nobody can dial in, and a
    // certificate made now would change again in a second.
    if (n == 1) return;
    names[n++] = "127.0.0.1";   // a viewer reaching it through a port forward
    key[0] = 0;
    for (int i = 0; i < n; i++) {
        strncat(key, names[i], sizeof key - strlen(key) - 1);
        strncat(key, " ", sizeof key - strlen(key) - 1);
    }
    if (strcmp(key, last) != 0) {
        k_strlcpy(last, key, sizeof last);
        char err[160];
        fp[0] = 0;
        if (utls_server_identity(RD_TLS_KEY, RD_TLS_CRT, names, n, fp, sizeof fp, err,
                                 sizeof err)) {
            rd_log("remoted: vnc: no encryption: %s\n", err);
            fp[0] = 0;
            return;
        }
        rd_log("remoted: vnc: certificate for %s-- %s\n", key, fp);
    }
    // Checked every pass, not only when made: a file written whole (a
    // restore, an editor) loses the key until remoted puts it back.
    if (fp[0] && strcmp(fp, c->vnc.fingerprint) != 0)
        uconf_set_in(UREMOTE_CONF, "vnc", "fingerprint", fp);
}

static int serve(void) {
    struct listener vnc = { -1, 0 };
    int live = 0;
    for (;;) {
        int code;
        while (sys_waitpid_nohang(-1, &code) > 0) if (live > 0) live--;

        static struct uremote_conf c;   // static: kilobytes, and one loop
        uremote_load(&c);
        reconcile(&vnc, c.vnc.enabled ? c.vnc.port : 0, "vnc");
        if (c.vnc.enabled && c.vnc.encryption != UREMOTE_ENC_OFF) identity(&c);
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
        // REAPED HERE TOO, not only at the top: the loop sat in accept()
        // while a session ended, and a viewer arriving just after one left
        // would be refused on the stale count. A session that is still
        // finishing (sending TLS's close_notify) gets a second.
        for (int i = 0; i < 20; i++) {
            while (sys_waitpid_nohang(-1, &code) > 0) if (live > 0) live--;
            if (live < SESSIONS_MAX) break;
            sys_sleep_ms(50);
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
    static const char *const ENC[] = { "offered (VeNCrypt), plain accepted", "required", "off" };
    printf("VNC   %s, port %d, password %s\n", c.vnc.enabled ? "on" : "off", c.vnc.port,
           c.vnc.password[0] ? "set" : "NOT SET (viewers are refused)");
    printf("      encryption %s%s%s\n", ENC[c.vnc.encryption],
           c.vnc.fingerprint[0] ? "; certificate " : "", c.vnc.fingerprint);
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
