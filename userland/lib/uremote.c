// See lib/uremote.h.
#include "lib/uremote.h"
#include "lib/uconf.h"
#include "rt/sys.h"
#include "signal_abi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void uremote_fmt_ip(uint32_t ip, char *out, size_t cap) {
    snprintf(out, cap, "%u.%u.%u.%u", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
             (ip >> 8) & 0xFF, ip & 0xFF);
}

int uremote_parse_net(const char *s, struct uremote_net *out) {
    const char *start = s;
    uint32_t ip = 0;
    for (int i = 0; i < 4; i++) {
        char *end;
        long v = strtol(s, &end, 10);
        if (end == s || v < 0 || v > 255) return 0;
        ip = (ip << 8) | (uint32_t)v;
        s = end;
        if (i < 3) {
            if (*s != '.') return 0;
            s++;
        }
    }
    int bits = 32;
    if (*s == '/') {
        char *end;
        long v = strtol(s + 1, &end, 10);
        if (end == s + 1 || v < 0 || v > 32) return 0;
        bits = (int)v;
        s = end;
    }
    if (*s) return 0;
    memset(out, 0, sizeof *out);
    out->mask = bits ? 0xFFFFFFFFu << (32 - bits) : 0;
    out->ip = ip & out->mask;
    snprintf(out->text, sizeof out->text, "%s", start);
    return 1;
}

static int yes(const char *v) {
    return !strcmp(v, "yes") || !strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true");
}

static void proto(const struct etc_config_buf *b, const char *sect, struct uremote_proto *p,
                  int port) {
    char v[UREMOTE_PASSWORD_MAX];
    p->enabled = etc_config_buf_get_in(b, sect, "enabled", v, sizeof v) && yes(v);
    p->port = port;
    if (etc_config_buf_get_in(b, sect, "port", v, sizeof v)) {
        int n = atoi(v);
        if (n > 0 && n < 65536) p->port = n;
    }
    p->password[0] = 0;
    etc_config_buf_get_in(b, sect, "password", p->password, sizeof p->password);
    p->encryption = UREMOTE_ENC_PREFER;
    if (etc_config_buf_get_in(b, sect, "encryption", v, sizeof v)) {
        if (!strcmp(v, "require")) p->encryption = UREMOTE_ENC_REQUIRE;
        else if (!strcmp(v, "off")) p->encryption = UREMOTE_ENC_OFF;
    }
    p->fingerprint[0] = 0;
    etc_config_buf_get_in(b, sect, "fingerprint", p->fingerprint, sizeof p->fingerprint);
    snprintf(p->user, sizeof p->user, "toy");
    etc_config_buf_get_in(b, sect, "user", p->user, sizeof p->user);
}

void uremote_load(struct uremote_conf *c) {
    memset(c, 0, sizeof *c);
    c->when = UREMOTE_ASK_UNLESS_TRUSTED;
    c->from = UREMOTE_FROM_NETWORK;
    c->vnc.port = 5900;
    c->rdp.port = 3389;
    static struct etc_config_buf b;
    if (!uconf_load(UREMOTE_CONF, &b)) return;
    proto(&b, "vnc", &c->vnc, 5900);
    proto(&b, "rdp", &c->rdp, 3389);

    char v[256];
    if (etc_config_buf_get(&b, "when", v, sizeof v)) {
        if (!strcmp(v, "ask")) c->when = UREMOTE_ASK;
        else if (!strcmp(v, "always")) c->when = UREMOTE_ALWAYS;
    }
    if (etc_config_buf_get(&b, "from", v, sizeof v) && !strcmp(v, "anywhere"))
        c->from = UREMOTE_FROM_ANYWHERE;
    c->view_only = etc_config_buf_get(&b, "view_only", v, sizeof v) && yes(v);

    // `trusted = 192.168.1.20 192.168.1.0/24`, words separated by spaces
    // or commas, and an optional label per address under [labels]. A word
    // that does not parse is skipped -- the rest still means what it says.
    if (etc_config_buf_get(&b, "trusted", v, sizeof v)) {
        char *save = 0;
        for (char *w = strtok_r(v, " ,", &save); w && c->ntrusted < UREMOTE_TRUSTED_MAX;
             w = strtok_r(0, " ,", &save)) {
            struct uremote_net *n = &c->trusted[c->ntrusted];
            if (!uremote_parse_net(w, n)) continue;
            etc_config_buf_get_in(&b, "labels", w, n->label, sizeof n->label);
            c->ntrusted++;
        }
    }
}

// --- writing ---------------------------------------------------------------

static int ok(int r) { return r ? 0 : -1; }

int uremote_set_enabled(const char *p, int on) {
    return ok(uconf_set_in(UREMOTE_CONF, p, "enabled", on ? "yes" : "no"));
}

int uremote_set_port(const char *p, int port) {
    if (port <= 0 || port > 65535) return -1;
    char v[8];
    snprintf(v, sizeof v, "%d", port);
    return ok(uconf_set_in(UREMOTE_CONF, p, "port", v));
}

int uremote_set_password(const char *p, const char *pw) {
    return ok(uconf_set_in(UREMOTE_CONF, p, "password", pw));
}

int uremote_set_encryption(const char *p, enum uremote_enc e) {
    static const char *const E[] = { "prefer", "require", "off" };
    return ok(uconf_set_in(UREMOTE_CONF, p, "encryption", E[e]));
}

int uremote_set_when(enum uremote_when w) {
    static const char *const W[] = { "ask", "ask-unless-trusted", "always" };
    return ok(uconf_set(UREMOTE_CONF, "when", W[w]));
}

int uremote_set_from(enum uremote_from f) {
    return ok(uconf_set(UREMOTE_CONF, "from", f == UREMOTE_FROM_ANYWHERE ? "anywhere" : "network"));
}

static int write_list(const struct uremote_conf *c) {
    char v[UREMOTE_TRUSTED_MAX * 24];
    v[0] = 0;
    for (int i = 0; i < c->ntrusted; i++) {
        if (i) strlcat(v, " ", sizeof v);
        strlcat(v, c->trusted[i].text, sizeof v);
    }
    if (!c->ntrusted) return ok(uconf_unset(UREMOTE_CONF, "trusted"));
    return ok(uconf_set(UREMOTE_CONF, "trusted", v));
}

int uremote_trust(const char *addr, const char *label) {
    struct uremote_net n;
    if (!uremote_parse_net(addr, &n)) return -1;
    struct uremote_conf c;
    uremote_load(&c);
    int have = 0;
    for (int i = 0; i < c.ntrusted; i++) if (!strcmp(c.trusted[i].text, n.text)) have = 1;
    if (!have) {
        if (c.ntrusted >= UREMOTE_TRUSTED_MAX) return -1;
        c.trusted[c.ntrusted++] = n;
        if (write_list(&c)) return -1;
    }
    if (label && label[0]) return ok(uconf_set_in(UREMOTE_CONF, "labels", n.text, label));
    return 0;
}

int uremote_untrust(const char *addr) {
    struct uremote_conf c;
    uremote_load(&c);
    int k = 0;
    for (int i = 0; i < c.ntrusted; i++)
        if (strcmp(c.trusted[i].text, addr)) c.trusted[k++] = c.trusted[i];
    if (k == c.ntrusted) return 0;
    c.ntrusted = k;
    uconf_set_in(UREMOTE_CONF, "labels", addr, NULL);   // a removal (uconf.h)
    return write_list(&c);
}

// --- who may connect -------------------------------------------------------

int uremote_trusted(const struct uremote_conf *c, uint32_t ip) {
    for (int i = 0; i < c->ntrusted; i++)
        if ((ip & c->trusted[i].mask) == c->trusted[i].ip) return 1;
    return 0;
}

int uremote_local(uint32_t ip) {
    struct query_netdev d;
    QUERY_FOREACH(QUERY_NETDEV, d, i) {
        if (!d.ip || !d.netmask) continue;
        if ((ip & (uint32_t)d.netmask) == ((uint32_t)d.ip & (uint32_t)d.netmask)) return 1;
    }
    return 0;
}

// --- sessions --------------------------------------------------------------

int uremote_sessions(struct query_remotesess *out, int max) {
    int n = 0;
    struct query_remotesess q;
    QUERY_FOREACH(QUERY_REMOTESESS, q, i) {
        if (n >= max) break;
        out[n++] = q;
    }
    return n;
}

int uremote_is_desktop(const struct query_remotesess *s) { return !strcmp(s->comm, "remoted"); }

void uremote_describe(const struct query_remotesess *s, char *out, size_t cap) {
    if (s->status[0]) snprintf(out, cap, "%s", s->status);
    else if (uremote_is_desktop(s)) snprintf(out, cap, "Remote desktop");
    else if (!strcmp(s->comm, "telnetd")) snprintf(out, cap, "Telnet shell");
    else snprintf(out, cap, "%s", s->comm);
}

int uremote_disconnect(int pid) { return sys_kill(pid, SIGTERM) < 0 ? -1 : 0; }

int uremote_set_view_only(int pid, int on) {
    return sys_kill(pid, on ? SIGUSR1 : SIGUSR2) < 0 ? -1 : 0;
}
