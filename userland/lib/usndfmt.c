// Per-card output formats -- see usndfmt.h.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lib/uconf.h"
#include "lib/usndfmt.h"
#include "sound_abi.h"

// `allowed` is written as rates in Hz, "44100,48000" -- what a person
// editing the file by hand would write -- and an unknown one is dropped
// rather than rounded.
static uint32_t parse_rates(const char *s) {
    uint32_t m = 0;
    while (*s) {
        char *end;
        unsigned long hz = strtoul(s, &end, 10);
        if (end == s) break;
        m |= snd_rate_mask((uint32_t)hz);
        s = end;
        while (*s == ',' || *s == ' ') s++;
    }
    return m;
}

void usndfmt_load(const char *card, struct usndfmt *f) {
    f->match = 1;
    f->fixed = SND_RATE;
    f->allowed = USNDFMT_DEFAULT_ALLOWED;
    f->bits = 0;
    if (!card || !card[0]) return;

    // HEAP, not stack: the document buffer is bigger than ring 3's
    // 2 KiB frame budget.
    struct etc_config_buf *buf = malloc(sizeof *buf);
    if (!buf) return;
    if (!uconf_load(SND_CARDS_FILE, buf)) { free(buf); return; }
    char v[96];
    if (etc_config_buf_get_in(buf, card, "rate", v, sizeof v)) {
        if (strcmp(v, "match") == 0) f->match = 1;
        else if (snd_rate_mask((uint32_t)strtoul(v, 0, 10))) {
            f->match = 0;
            f->fixed = (uint32_t)strtoul(v, 0, 10);
        }
    }
    if (etc_config_buf_get_in(buf, card, "allowed", v, sizeof v)) {
        uint32_t m = parse_rates(v);
        if (m) f->allowed = m;
    }
    if (etc_config_buf_get_in(buf, card, "bits", v, sizeof v))
        f->bits = strcmp(v, "auto") == 0 ? 0 : (snd_depth_mask((uint32_t)atoi(v)) ? (uint32_t)atoi(v) : 0);
    free(buf);
}

int usndfmt_save(const char *card, const struct usndfmt *f) {
    if (!card || !card[0]) return 0;
    char rate[16], bits[16], allowed[96];
    if (f->match) strcpy(rate, "match");
    else snprintf(rate, sizeof rate, "%u", (unsigned)f->fixed);
    if (f->bits) snprintf(bits, sizeof bits, "%u", (unsigned)f->bits);
    else strcpy(bits, "auto");
    size_t n = 0;
    allowed[0] = 0;
    for (int i = 0; i < SND_RATE_COUNT; i++)
        if (f->allowed & (1u << i))
            n += (size_t)snprintf(allowed + n, sizeof allowed - n, "%s%u", n ? "," : "",
                                  (unsigned)snd_rate_hz(i));
    return uconf_set_in(SND_CARDS_FILE, card, "rate", rate) &&
           uconf_set_in(SND_CARDS_FILE, card, "allowed", allowed) &&
           uconf_set_in(SND_CARDS_FILE, card, "bits", bits);
}

static uint32_t lowest(uint32_t mask) {
    for (int i = 0; i < SND_RATE_COUNT; i++)
        if (mask & (1u << i)) return snd_rate_hz(i);
    return SND_RATE;
}

uint32_t usndfmt_pick_rate(const struct usndfmt *f, uint32_t card_rates, uint32_t want) {
    if (!card_rates) return SND_RATE;
    uint32_t r = SND_RATE;
    if (!f->match) {
        r = f->fixed;
    } else {
        uint32_t ok = f->allowed & card_rates;
        if (want && (ok & snd_rate_mask(want))) r = want;
        else if (ok & SND_RATE_48000) r = SND_RATE;
        else if (ok) r = lowest(ok);
    }
    if (card_rates & snd_rate_mask(r)) return r;
    return (card_rates & SND_RATE_48000) ? SND_RATE : lowest(card_rates);
}

uint32_t usndfmt_pick_bits(const struct usndfmt *f, uint32_t card_depths) {
    if (!f->bits || !(card_depths & snd_depth_mask(f->bits))) return 0;
    return f->bits;
}

// Every digit the rate has and no more: 22.05, 11.025, 44.1, 48.
const char *usndfmt_rate_label(uint32_t hz, char *buf, uint32_t size) {
    unsigned rem = hz % 1000;
    if (!rem) { snprintf(buf, size, "%u kHz", (unsigned)(hz / 1000)); return buf; }
    char frac[4];
    snprintf(frac, sizeof frac, "%03u", rem);
    for (int i = 2; i > 0 && frac[i] == '0'; i--) frac[i] = 0;
    snprintf(buf, size, "%u.%s kHz", (unsigned)(hz / 1000), frac);
    return buf;
}
