// A network card's adapter settings -- see unetlink.h.
#include <stdio.h>
#include <string.h>
#include "lib/unetlink.h"
#include "lib/uconf.h"
#include "rt/sys.h"

const char *unetlink_card_key(const struct query_netdev *d, int i, char slot[18]) {
    if (i == 0) {
        unsigned long long m = (unsigned long long)d->mac;
        snprintf(slot, 18, "%02llx:%02llx:%02llx:%02llx:%02llx:%02llx",
                 m & 0xFF, (m >> 8) & 0xFF, (m >> 16) & 0xFF,
                 (m >> 24) & 0xFF, (m >> 32) & 0xFF, (m >> 40) & 0xFF);
        return slot;
    }
    if (i == 1) return d->location[0] ? d->location : 0;
    if (i == 2) return d->driver[0] ? d->driver : 0;
    return 0;
}

// --- words and labels --------------------------------------------------

static const struct { uint32_t rate; const char *word, *label; } k_rates[] = {
    { NET_RATE_10G,  "10g",  "10 Gb/s"  },
    { NET_RATE_5G,   "5g",   "5 Gb/s"   },
    { NET_RATE_2G5,  "2.5g", "2.5 Gb/s" },
    { NET_RATE_1G,   "1g",   "1 Gb/s"   },
    { NET_RATE_100M, "100m", "100 Mb/s" },
    { NET_RATE_10M,  "10m",  "10 Mb/s"  },
};
#define NRATES (sizeof k_rates / sizeof k_rates[0])

int64_t unetlink_speed_parse(const char *w) {
    if (!strcmp(w, "auto")) return 0;
    for (unsigned i = 0; i < NRATES; i++) if (!strcmp(w, k_rates[i].word)) return k_rates[i].rate;
    return -1;
}

const char *unetlink_speed_word(uint32_t cap) {
    for (unsigned i = 0; i < NRATES; i++) if (cap == k_rates[i].rate) return k_rates[i].word;
    return "auto";
}

const char *unetlink_rate_label(uint32_t rate) {
    for (unsigned i = 0; i < NRATES; i++) if (rate == k_rates[i].rate) return k_rates[i].label;
    return "?";
}

// The rate bits are in ascending order, so "at or below" is a mask.
uint32_t unetlink_rates_for(uint32_t supported, uint32_t cap) {
    if (!cap) return supported;
    return supported & ((cap << 1) - 1);
}

uint32_t unetlink_cap_of(uint32_t supported, uint32_t rates) {
    if ((rates & supported) == supported) return 0;
    uint32_t top = 0;
    for (uint32_t b = 1; b && b <= NET_RATE_ALL; b <<= 1) if (rates & b) top = b;
    return top;
}

static const char *const k_flow_word[4]  = { "off", "rx", "tx", "both" };
static const char *const k_flow_label[4] = { "Off", "Receive only", "Send only", "Receive and send" };
static const char *const k_mod_word[4]   = { "off", "low", "medium", "high" };
static const char *const k_mod_label[4]  = { "Off (lowest latency)", "Low", "Medium",
                                             "High (fewest interrupts)" };

int unetlink_flow_parse(const char *w) {
    for (int i = 0; i < 4; i++) if (!strcmp(w, k_flow_word[i])) return i;
    return -1;
}
const char *unetlink_flow_word(uint32_t f)  { return k_flow_word[f & 3]; }
const char *unetlink_flow_label(uint32_t f) { return k_flow_label[f & 3]; }

int unetlink_mod_parse(const char *w) {
    for (int i = 0; i < 4; i++) if (!strcmp(w, k_mod_word[i])) return i;
    return -1;
}
const char *unetlink_mod_word(uint32_t m)  { return m <= NET_MOD_HIGH ? k_mod_word[m] : "?"; }
const char *unetlink_mod_label(uint32_t m) { return m <= NET_MOD_HIGH ? k_mod_label[m] : "?"; }

// --- the file and the card ---------------------------------------------

// The most specific section naming `key` for this card.
static int card_get(const struct etc_config_buf *conf, const struct query_netdev *d,
                    const char *key, char *out, uint32_t cap) {
    char slot[18];
    for (int i = 0; i < UNETLINK_CARD_KEYS; i++) {
        const char *k = unetlink_card_key(d, i, slot);
        if (k && *k && etc_config_buf_get_in(conf, k, key, out, cap) && out[0]) return 1;
    }
    return 0;
}

int unetlink_load(const struct etc_config_buf *conf, const struct query_netdev *d,
                  struct net_linkcfg *out, int *bad) {
    memset(out, 0, sizeof *out);
    strncpy(out->name, d->name, sizeof out->name - 1);
    if (bad) *bad = 0;
    int n = 0, b = 0;
    char v[16];
    uint32_t caps = (uint32_t)d->link_caps;

    if ((caps & NET_LINK_RATES) && card_get(conf, d, "speed", v, sizeof v)) {
        int64_t cap = unetlink_speed_parse(v);
        uint32_t r = cap < 0 ? 0 : unetlink_rates_for((uint32_t)d->rates_supported, (uint32_t)cap);
        // A cap below every rate the card has is unreadable, not "none".
        if (r) { out->rates = r; out->which |= NET_LINK_RATES; n++; } else b++;
    }
    if ((caps & NET_LINK_EEE) && card_get(conf, d, "eee", v, sizeof v)) {
        if (!strcmp(v, "on") || !strcmp(v, "off")) {
            out->eee = !strcmp(v, "on");
            out->which |= NET_LINK_EEE;
            n++;
        } else b++;
    }
    if ((caps & NET_LINK_FLOW) && card_get(conf, d, "flow", v, sizeof v)) {
        int f = unetlink_flow_parse(v);
        if (f >= 0) { out->flow = (uint32_t)f; out->which |= NET_LINK_FLOW; n++; } else b++;
    }
    if ((caps & NET_LINK_MODERATION) && card_get(conf, d, "moderation", v, sizeof v)) {
        int m = unetlink_mod_parse(v);
        if (m >= 0) { out->moderation = (uint32_t)m; out->which |= NET_LINK_MODERATION; n++; } else b++;
    }
    if (bad) *bad = b;
    return n;
}

int unetlink_set(const struct query_netdev *d, const struct net_linkcfg *want) {
    struct net_linkcfg req = *want;
    memset(req.name, 0, sizeof req.name);
    strncpy(req.name, d->name, sizeof req.name - 1);
    if (sys_net_link(&req) < 0) return -1;

    char slot[18];
    const char *sec = unetlink_card_key(d, 0, slot);
    int ok = 1;
    if (want->which & NET_LINK_RATES)
        ok &= uconf_set_in(UNETLINK_CONF, sec, "speed",
                           unetlink_speed_word(unetlink_cap_of((uint32_t)d->rates_supported, want->rates)));
    if (want->which & NET_LINK_EEE)
        ok &= uconf_set_in(UNETLINK_CONF, sec, "eee", want->eee ? "on" : "off");
    if (want->which & NET_LINK_FLOW)
        ok &= uconf_set_in(UNETLINK_CONF, sec, "flow", unetlink_flow_word(want->flow));
    if (want->which & NET_LINK_MODERATION)
        ok &= uconf_set_in(UNETLINK_CONF, sec, "moderation", unetlink_mod_word(want->moderation));
    // APPLIED BUT NOT SAVED is still applied: the card does what was asked
    // until the next boot, and the caller hears the save failed.
    return ok ? 0 : -1;
}

int unetlink_reset(const struct query_netdev *d) {
    struct net_linkcfg req;
    memset(&req, 0, sizeof req);
    strncpy(req.name, d->name, sizeof req.name - 1);
    req.which = NET_LINK_DEFAULTS;
    if (sys_net_link(&req) < 0) return -1;
    char slot[18];
    const char *sec = unetlink_card_key(d, 0, slot);
    static const char *const keys[] = { "speed", "eee", "flow", "moderation" };
    // A removal of a key that is not there "fails" in the rewriter; it is
    // not a failure here.
    for (unsigned i = 0; i < 4; i++) uconf_set_in(UNETLINK_CONF, sec, keys[i], 0);
    return 0;
}
