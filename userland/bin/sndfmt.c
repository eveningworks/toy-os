// sndfmt -- show or set a sound card's output format: the rate policy
// and the width, kept per card in SND_CARDS_FILE (lib/usndfmt.h). The
// command line beside System Settings' and Device Manager's Format
// controls, which write the same file.
//
// IT DOES NOT TOUCH THE CARD. soundd (or, with no daemon, the playing
// program) reads the setting when it next decides a rate -- the next
// sound that starts -- so a change never cuts into what is playing.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt/sys.h"
#include "query_abi.h"
#include "sound_abi.h"
#include "lib/uargs.h"
#include "lib/usndfmt.h"

static const char *f_rate, *f_allow, *f_bits;

static const struct uargs_opt OPTS[] = {
    { "rate",  'r', "RATE", "match (follow what plays) or a rate in Hz", 0, &f_rate },
    { "allow", 'a', "LIST", "the rates match may switch to, in Hz: 44100,48000", 0, &f_allow },
    { "bits",  'b', "BITS", "auto (the deepest) or a width: 16, 24, 32", 0, &f_bits },
    { 0 },
};

static const struct uargs_prog PROG = {
    .name = "sndfmt",
    .usage = "[CARD] [-r RATE] [-a LIST] [-b BITS]",
    .summary = "Show or set a sound card's output format.",
    .opts = OPTS,
    .notes = "CARD is a name from `lssound`; the active card when left out. With no\n"
             "option, the card's setting and what it plays now are printed. A change\n"
             "applies from the next sound that starts.",
};

static void put_rates(uint32_t mask) {
    char b[16];
    int first = 1;
    for (int i = 0; i < SND_RATE_COUNT; i++) {
        if (!(mask & (1u << i))) continue;
        printf("%s%s", first ? "" : ", ", usndfmt_rate_label(snd_rate_hz(i), b, sizeof b));
        first = 0;
    }
    if (first) printf("none");
}

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    if (a.argc > 1) return uargs_error(&PROG, "one card at a time");

    struct query_sound q, card;
    int found = 0;
    QUERY_FOREACH(QUERY_SOUND, q, qi) {
        if (a.argc ? strcmp(q.name, a.argv[0]) == 0 : (int)q.active) { card = q; found = 1; }
    }
    if (!found) {
        fprintf(stderr, "sndfmt: %s\n", a.argc ? "no such card -- see lssound"
                                              : "no sound device");
        return 1;
    }

    struct usndfmt f;
    usndfmt_load(card.name, &f);
    int change = f_rate || f_allow || f_bits;
    if (f_rate) {
        if (!strcmp(f_rate, "match")) f.match = 1;
        else {
            uint32_t hz = (uint32_t)strtoul(f_rate, 0, 10);
            if (!(snd_rate_mask(hz) & card.rates))
                return uargs_error(&PROG, "%s does not take '%s' Hz", card.name, f_rate);
            f.match = 0;
            f.fixed = hz;
        }
    }
    if (f_allow) {
        uint32_t m = 0;
        for (const char *p = f_allow; *p;) {
            char *end;
            uint32_t hz = (uint32_t)strtoul(p, &end, 10);
            if (end == p || !(snd_rate_mask(hz) & card.rates))
                return uargs_error(&PROG, "%s does not take a rate in '%s'", card.name, f_allow);
            m |= snd_rate_mask(hz);
            p = end;
            while (*p == ',' || *p == ' ') p++;
        }
        f.allowed = m;
    }
    if (f_bits) {
        if (!strcmp(f_bits, "auto")) f.bits = 0;
        else {
            uint32_t b = (uint32_t)atoi(f_bits);
            if (!(snd_depth_mask(b) & card.depths))
                return uargs_error(&PROG, "%s does not play %s bits", card.name, f_bits);
            f.bits = b;
        }
    }
    if (change && !usndfmt_save(card.name, &f)) {
        fprintf(stderr, "sndfmt: cannot write %s\n", SND_CARDS_FILE);
        return 1;
    }

    char b[16];
    printf("%s (%s)\n", card.name, card.label);
    if (f.match) { printf("  rate:    match what plays, from "); put_rates(f.allowed & card.rates); printf("\n"); }
    else printf("  rate:    %s\n", usndfmt_rate_label(f.fixed, b, sizeof b));
    if (f.bits) printf("  bits:    %u\n", (unsigned)f.bits);
    else printf("  bits:    auto\n");
    printf("  playing: %u-bit, %s\n", (unsigned)card.bits,
           usndfmt_rate_label(card.rate ? card.rate : SND_RATE, b, sizeof b));
    if (change) printf("applies from the next sound that starts\n");
    return 0;
}
