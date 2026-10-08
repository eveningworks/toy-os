// /etc/remote.conf, and who may connect. See rd.h.
#include "remoted/rd.h"
#include "lib/uconf.h"
#include "rt/sys.h"
#include "query_abi.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

void rd_fmt_ip(uint32_t ip, char *out, size_t cap) {
    snprintf(out, cap, "%u.%u.%u.%u", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
             (ip >> 8) & 0xFF, ip & 0xFF);
}

int rd_parse_net(const char *s, struct rd_net *out) {
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
    out->mask = bits ? 0xFFFFFFFFu << (32 - bits) : 0;
    out->ip = ip & out->mask;
    return 1;
}

static int yes(const char *v) {
    return !strcmp(v, "yes") || !strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true");
}

static void proto(const struct etc_config_buf *b, const char *sect, struct rd_proto *p,
                  int port) {
    char v[RD_PASSWORD_MAX];
    p->enabled = etc_config_buf_get_in(b, sect, "enabled", v, sizeof v) && yes(v);
    p->port = port;
    if (etc_config_buf_get_in(b, sect, "port", v, sizeof v)) {
        int n = atoi(v);
        if (n > 0 && n < 65536) p->port = n;
    }
    p->password[0] = 0;
    etc_config_buf_get_in(b, sect, "password", p->password, sizeof p->password);
    snprintf(p->user, sizeof p->user, "toy");
    etc_config_buf_get_in(b, sect, "user", p->user, sizeof p->user);
}

void rd_conf_load(struct rd_conf *c) {
    memset(c, 0, sizeof *c);
    c->when = RD_ASK_UNLESS_TRUSTED;
    c->from = RD_FROM_NETWORK;
    static struct etc_config_buf b;
    if (!uconf_load(RD_CONF, &b)) {
        c->vnc.port = 5900;
        c->rdp.port = 3389;
        return;
    }
    proto(&b, "vnc", &c->vnc, 5900);
    proto(&b, "rdp", &c->rdp, 3389);

    char v[256];
    if (etc_config_buf_get(&b, "when", v, sizeof v)) {
        if (!strcmp(v, "ask")) c->when = RD_ASK;
        else if (!strcmp(v, "always")) c->when = RD_ALWAYS;
    }
    if (etc_config_buf_get(&b, "from", v, sizeof v) && !strcmp(v, "anywhere"))
        c->from = RD_FROM_ANYWHERE;
    c->view_only = etc_config_buf_get(&b, "view_only", v, sizeof v) && yes(v);

    // `trusted = 192.168.200.103 192.168.200.0/24`: one list, words
    // separated by spaces or commas. A word that does not parse is
    // logged and skipped -- the rest of the list still means what it says.
    if (etc_config_buf_get(&b, "trusted", v, sizeof v)) {
        char *save = 0;
        for (char *w = strtok_r(v, " ,", &save); w && c->ntrusted < RD_TRUSTED_MAX;
             w = strtok_r(0, " ,", &save)) {
            if (rd_parse_net(w, &c->trusted[c->ntrusted])) c->ntrusted++;
            else rd_log("remoted: trusted: not an address: %s\n", w);
        }
    }
}

int rd_peer_trusted(const struct rd_conf *c, uint32_t ip) {
    for (int i = 0; i < c->ntrusted; i++)
        if ((ip & c->trusted[i].mask) == c->trusted[i].ip) return 1;
    return 0;
}

int rd_peer_local(uint32_t ip) {
    struct query_netdev d;
    QUERY_FOREACH(QUERY_NETDEV, d, i) {
        if (!d.ip || !d.netmask) continue;
        if ((ip & (uint32_t)d.netmask) == ((uint32_t)d.ip & (uint32_t)d.netmask)) return 1;
    }
    return 0;
}
