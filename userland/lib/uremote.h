#ifndef ULIB_UREMOTE_H
#define ULIB_UREMOTE_H

#include <stdint.h>
#include <stddef.h>
#include "query_abi.h"   // struct query_remotesess

// The remote desktop, from any program: /etc/remote.conf (what
// /bin/remoted serves and who may connect), the sessions open now, and
// the two things anyone may do to one -- end it, or make it view only.
// /bin/remoted, System Settings' Remote Desktop page and the tray flyout
// share it, so the three cannot disagree about what the file says.
//
// A SESSION IS ACTED ON BY SIGNAL, to its leader's pid: SIGTERM ends it
// (the kernel releases what its viewer held), SIGUSR1 makes it view only,
// SIGUSR2 gives control back. Signals rather than a channel because they
// need nothing open, they reach a telnet shell just as well (SIGTERM),
// and a session that is wedged still dies.

#define UREMOTE_CONF "/etc/remote.conf"
#define UREMOTE_PASSWORD_MAX 64
#define UREMOTE_TRUSTED_MAX 16
#define UREMOTE_LABEL_MAX 32

enum uremote_when { UREMOTE_ASK, UREMOTE_ASK_UNLESS_TRUSTED, UREMOTE_ALWAYS };
enum uremote_from { UREMOTE_FROM_NETWORK, UREMOTE_FROM_ANYWHERE };

struct uremote_net {                 // one trusted address or subnet
    uint32_t ip, mask;
    char text[24];                   // as written: "192.168.1.0/24"
    char label[UREMOTE_LABEL_MAX];   // "" when it has none
};

struct uremote_proto {
    int enabled;
    int port;
    char user[32];                   // RDP's; VNC has no user name
    char password[UREMOTE_PASSWORD_MAX];
};

struct uremote_conf {
    struct uremote_proto vnc, rdp;
    enum uremote_when when;
    enum uremote_from from;
    struct uremote_net trusted[UREMOTE_TRUSTED_MAX];
    int ntrusted;
    int view_only;
};

// Reads the file; anything missing is its default (everything off).
void uremote_load(struct uremote_conf *c);

// Writing, one key at a time -- the rest of the file is kept. Each
// returns 0 on success, -1 if the write failed. `proto` is "vnc" or "rdp".
int uremote_set_enabled(const char *proto, int on);
int uremote_set_port(const char *proto, int port);
int uremote_set_password(const char *proto, const char *pw);
int uremote_set_when(enum uremote_when w);
int uremote_set_from(enum uremote_from f);
// Adds `addr` ("a.b.c.d" or "a.b.c.d/bits") with an optional label, or
// relabels it if it is there. -1 for an address that does not parse.
int uremote_trust(const char *addr, const char *label);
int uremote_untrust(const char *addr);

// "a.b.c.d" or "a.b.c.d/bits"; 1 on success. Refuses rather than guesses.
int uremote_parse_net(const char *s, struct uremote_net *out);
void uremote_fmt_ip(uint32_t ip, char *out, size_t cap);

int uremote_trusted(const struct uremote_conf *c, uint32_t ip);
// Is `ip` on a network one of this machine's cards is on?
int uremote_local(uint32_t ip);

// The sessions open now (QUERY_REMOTESESS), at most `max`; returns how many.
int uremote_sessions(struct query_remotesess *out, int max);
// A remote DESKTOP session, as opposed to a telnet shell.
int uremote_is_desktop(const struct query_remotesess *s);
// What a session's row says: "VNC, full control" / "Telnet shell".
void uremote_describe(const struct query_remotesess *s, char *out, size_t cap);
int uremote_disconnect(int pid);
int uremote_set_view_only(int pid, int on);

#endif
