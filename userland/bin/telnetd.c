// telnetd -- a shell on a pty, for whoever connects.
//
// Run per connection by inetd, so the socket arrives on fd 0 and fd 1
// and this program never accepts anything itself:
//
//     inetd -p 23 /bin/telnetd
//
// WHY THIS EXISTS. Half the open bugs in this project are bare-metal
// only -- a mouse that will not bind, a power button that needs two
// presses, a garbled product string -- and the only way in was a serial
// cable and somebody sitting at the machine. This is the network
// version of that seat.
//
// IT IS TELNET, WHICH MEANS NO ENCRYPTION AND NO AUTHENTICATION, and
// this OS has no users to authenticate anyway: a connection is a shell
// with the run of the machine. That is the standing arrangement for lab
// gear -- console servers and switches still do exactly this -- and it
// is why the service ships DISABLED (see /usr/share/services/telnetd).
// Do not enable it on a network you do not own.
//
// THE PTY IS THE POINT, not a detail. Handing the socket straight to
// /bin/tosh would have been twenty lines, and the shell would have had
// no controlling terminal: no Ctrl-C, no job control, no window size.
// This allocates one and puts the shell on it, exactly as the GUI
// Terminal does (userland/gui/apps/terminal.c) -- so the shell cannot
// tell the difference, which is the property worth having.
#include <stdint.h>
#include "lib/usetting.h"  // system.shell
#include <pthread.h>
#include "rt/sys.h"
#include "lib/cmd.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#define USAGE "telnetd  (run by inetd: inetd -p 23 /bin/telnetd)"

#define SHELL_FALLBACK "/bin/tosh"

// WHICH SHELL, from the registry rather than baked in (`system.shell`,
// kernel/lib/shell_config.c). Read per session rather than once at
// startup, so changing the setting affects the NEXT window or login
// instead of needing a restart -- and the fallback keeps a session
// possible when the registry cannot answer.
static const char *shell_path(char *buf, size_t cap) {
    if (usetting_get("system.shell", buf, cap) && buf[0]) return buf;
    return SHELL_FALLBACK;
}

// RFC 854. Only the commands a client actually sends are named; an
// option this does not implement is REFUSED rather than ignored, which
// is the spec's requirement and the difference between a client that
// gives up negotiating and one that waits forever.
#define IAC   255
#define DONT  254
#define DO    253
#define WONT  252
#define WILL  251
#define SB    250
#define SE    240

#define OPT_ECHO  1
#define OPT_SGA   3   // suppress go-ahead: character-at-a-time
#define OPT_NAWS 31   // window size, RFC 1073

#define BUF 1024

static int g_sock_in = 0, g_sock_out = 1;
static int g_master = -1;
static int g_child = -1;

static void sock_write(const void *p, uint32_t n) {
    const uint8_t *b = p;
    while (n) {
        int64_t w = write(g_sock_out, b, n);
        if (w <= 0) return;
        b += w; n -= (uint32_t)w;
    }
}

static void send_cmd(uint8_t cmd, uint8_t opt) {
    uint8_t c[3] = { IAC, cmd, opt };
    sock_write(c, 3);
}

// --- the shell's output, on its own thread ----------------------------

// TWO TRANSLATIONS, BOTH REQUIRED BY THE PROTOCOL. A literal 255 must
// be doubled or the client reads it as a command; and a bare newline
// must become CR LF, because an NVT line ends that way and this tty
// layer has no oflag to do it with (abi/tty_abi.h has lflag only, so
// there is no ONLCR to turn on). A program here writes bare \n --
// terminal.c says so in as many words -- so without this every line
// after the first starts under the end of the last one.
static void *shell_to_client(void *arg) {
    (void)arg;
    // STATIC, not on the stack: a ring-3 stack is 16 KiB behind one
    // guard page with a 2 KiB frame budget, and these are 3 KiB
    // between them. One thread runs this and one runs the loop in
    // main(), so each direction's buffers are private to a single
    // thread without being automatic.
    static uint8_t in[BUF], out[BUF * 2];
    for (;;) {
        int64_t n = read(g_master, in, sizeof in);
        if (n <= 0) break;
        uint32_t o = 0;
        for (int64_t i = 0; i < n; i++) {
            uint8_t b = in[i];
            if (b == IAC) { out[o++] = IAC; out[o++] = IAC; }
            else if (b == '\n') { out[o++] = '\r'; out[o++] = '\n'; }
            else out[o++] = b;
        }
        sock_write(out, o);
    }
    // The shell is gone: closing our end is what makes the client's
    // read return, so it sees the session end rather than hanging.
    close(g_sock_out);
    return 0;
}

// --- the client's input -----------------------------------------------

enum { S_DATA, S_IAC, S_OPT, S_SB, S_SB_IAC };

struct in_state {
    int  st;
    uint8_t cmd;
    uint8_t sb_opt;
    uint8_t sb[16];
    uint32_t sb_len;
    int  saw_cr;      // the last data byte was CR
};

// Answer a negotiation. The rule is RFC 854's: agree only to what is
// implemented, refuse everything else, and never answer a request that
// asks for the state already held -- an unconditional reply to every
// WILL is how two implementations loop forever.
static void negotiate(uint8_t cmd, uint8_t opt) {
    switch (cmd) {
    case DO:
        if (opt == OPT_ECHO || opt == OPT_SGA) send_cmd(WILL, opt);
        else send_cmd(WONT, opt);
        break;
    case DONT:
        send_cmd(WONT, opt);
        break;
    case WILL:
        if (opt == OPT_NAWS) send_cmd(DO, opt);
        else send_cmd(DONT, opt);
        break;
    case WONT:
        send_cmd(DONT, opt);
        break;
    default: break;
    }
}

static void subnegotiation(struct in_state *s) {
    // NAWS is the only one taken: four bytes, width then height, big
    // endian (RFC 1073). It is what makes `less` and `edit` size
    // themselves to the window somebody actually has.
    if (s->sb_opt == OPT_NAWS && s->sb_len >= 4 && g_master >= 0) {
        struct tty_winsize ws;
        ws.cols = (uint16_t)((s->sb[0] << 8) | s->sb[1]);
        ws.rows = (uint16_t)((s->sb[2] << 8) | s->sb[3]);
        if (ws.cols && ws.rows) sys_tcsetwinsz(g_master, &ws);
    }
}

// Strips the protocol out of `in`, leaving what the shell should read.
// Exported shape rather than a loop inside main() because the CR rule
// below is the part that is easy to get wrong and worth a test.
static uint32_t client_to_shell(struct in_state *s, const uint8_t *in,
                                uint32_t n, uint8_t *out) {
    uint32_t o = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t b = in[i];
        switch (s->st) {
        case S_DATA:
            if (b == IAC) { s->st = S_IAC; break; }
            // AN NVT SENDS CR LF FOR ENTER AND CR NUL FOR A BARE CR, so
            // the byte AFTER a CR decides what the CR meant and must not
            // reach the shell either way. klineedit takes CR and LF
            // alike as "submit", so this emits one CR and swallows the
            // partner -- without which every Enter arrives twice.
            if (s->saw_cr) {
                s->saw_cr = 0;
                if (b == '\n' || b == 0) break;      // the partner byte
            }
            if (b == '\r') { s->saw_cr = 1; out[o++] = '\r'; break; }
            out[o++] = b;
            break;
        case S_IAC:
            if (b == IAC) { out[o++] = IAC; s->st = S_DATA; break; }
            if (b == SB) { s->st = S_SB; s->sb_len = 0; s->sb_opt = 0; break; }
            if (b == DO || b == DONT || b == WILL || b == WONT) {
                s->cmd = b; s->st = S_OPT; break;
            }
            s->st = S_DATA;                 // a two-byte command: ignored
            break;
        case S_OPT:
            negotiate(s->cmd, b);
            s->st = S_DATA;
            break;
        case S_SB:
            if (b == IAC) { s->st = S_SB_IAC; break; }
            if (!s->sb_opt && !s->sb_len) { s->sb_opt = b; break; }
            if (s->sb_len < sizeof s->sb) s->sb[s->sb_len++] = b;
            break;
        case S_SB_IAC:
            if (b == SE) { subnegotiation(s); s->st = S_DATA; break; }
            if (b == IAC && s->sb_len < sizeof s->sb) s->sb[s->sb_len++] = IAC;
            s->st = S_SB;
            break;
        }
    }
    return o;
}

// --- the session -------------------------------------------------------

static int start_shell(void) {
    int slave = -1;
    if (sys_openpty(&g_master, &slave) < 0) return -1;

    // The child's 0/1/2 are the pty, so the shell is on a terminal and
    // its stderr reaches the client rather than dmesg. dup2 around the
    // spawn, as terminal.c and tosh's own redirection do: SYS_SPAWN
    // inherits the table, so placing them here places them in the child.
    int in0 = dup(0), out1 = dup(1), err2 = dup(2);
    dup2(slave, 0);
    dup2(slave, 1);
    dup2(slave, 2);
    // **A NEW SESSION, because this pty is a terminal of its own.** The
    // shell claims it for that session, and only what the shell starts
    // -- which inherits the session -- may take job control of it. A
    // second shell run inside this one (`dash`) is exactly that case,
    // and it fails with "Cannot set tty process group" if every process
    // is in init's one session. See abi/syscall_abi.h's SPAWN_SETSID.
    char shbuf[SETTING_ABI_VALUE_MAX];
    int pid = sys_spawn_flags(shell_path(shbuf, sizeof shbuf), 0, -1, 0,
                              PGID_NEW, SPAWN_SETSID);
    if (in0  >= 0) { dup2(in0, 0);  close(in0); }
    if (out1 >= 0) { dup2(out1, 1); close(out1); }
    if (err2 >= 0) { dup2(err2, 2); close(err2); }
    close(slave);          // or the master never sees end-of-file

    if (pid < 0) { close(g_master); g_master = -1; return -1; }

    // The shell's group is the terminal's FOREGROUND group, which is
    // what points a Ctrl-C typed at the far end at the shell's job
    // instead of at nothing.
    sys_tcsetpgrp(g_master, pid);
    return pid;
}

int main(int argc, char **argv) {
    if (argc > 1) { cmd_usage(USAGE); return 1; }
    (void)argv;

    // OFFERED, NOT ASSUMED. WILL ECHO and WILL SGA together are what put
    // a client in character-at-a-time mode, which is the mode the shell
    // wants: it does its own line editing (kernel/lib/klineedit.c), so a
    // client editing a line locally would hide every completion and
    // every history recall until Enter.
    send_cmd(WILL, OPT_ECHO);
    send_cmd(WILL, OPT_SGA);
    send_cmd(DO, OPT_NAWS);

    g_child = start_shell();
    if (g_child < 0) {
        static const char msg[] = "telnetd: cannot start a shell\r\n";
        sock_write(msg, sizeof msg - 1);
        return 1;
    }

    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &at, shell_to_client, 0) != 0) {
        sys_kill(-g_child, SIGKILL);
        return 1;
    }

    struct in_state st;
    memset(&st, 0, sizeof st);
    static uint8_t in[BUF], out[BUF];   // see shell_to_client()
    for (;;) {
        int64_t n = read(g_sock_in, in, sizeof in);
        if (n <= 0) break;                 // the client hung up
        uint32_t o = client_to_shell(&st, in, (uint32_t)n, out);
        if (o) write(g_master, out, o);
    }

    // THE WHOLE GROUP, not the shell's pid: a session that hung up with
    // a job running would otherwise leave that job on the machine with
    // nothing attached to it. This is the hangup a real telnetd sends.
    sys_kill(-g_child, SIGHUP);
    sys_kill(-g_child, SIGKILL);
    close(g_master);
    return 0;
}
