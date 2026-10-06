#ifndef ULIB_USNDFMT_H
#define ULIB_USNDFMT_H

// usndfmt -- each sound card's chosen output format: its rate policy and
// its width, kept in SND_CARDS_FILE (abi/sound_abi.h) as one `[<card>]`
// section per card, keyed by the card's stable name. Read by soundd and
// the direct device sink, which apply it; written by `sndfmt`, System
// Settings and Device Manager. PipeWire's `default.clock.rate` and
// `allowed-rates`, per card as Windows and macOS keep it.

#include <stdint.h>

// THE RATE POLICY. `match`: follow what plays -- a stream starting on an
// idle card takes the card to its own rate when `allowed` holds it, and
// everything else is resampled to whatever the card runs at. Otherwise
// `fixed`: one rate, always.
struct usndfmt {
    int      match;
    uint32_t fixed;      // Hz, when !match
    uint32_t allowed;    // SND_RATE_* -- `match`'s choices
    uint32_t bits;       // 0 = the deepest the card offers
};

// The default for a card nobody has set: follow what plays, between the
// two rate families' everyday members, at the card's deepest width.
#define USNDFMT_DEFAULT_ALLOWED (SND_RATE_44100 | SND_RATE_48000)

// Fills `f` from the file, the defaults for whatever is missing. Never
// fails: a card with no section is a card left on its defaults.
void usndfmt_load(const char *card, struct usndfmt *f);

// Writes all four keys of the card's section. 0 when the write failed.
int usndfmt_save(const char *card, const struct usndfmt *f);

// THE DECISION, in one place: the rate to run a card at, given what it
// offers (SND_RATE_*; 0 = it does not say, so SND_RATE only) and the
// rate of the content asking (0 = no preference). Always a rate the card
// takes: a policy naming one it does not falls back to SND_RATE, then to
// the lowest it offers.
uint32_t usndfmt_pick_rate(const struct usndfmt *f, uint32_t card_rates, uint32_t want);

// The width to ask for: the setting when the card offers it, else 0 (the
// deepest).
uint32_t usndfmt_pick_bits(const struct usndfmt *f, uint32_t card_depths);

// "44.1 kHz", "48 kHz", "192 kHz" -- for a person.
const char *usndfmt_rate_label(uint32_t hz, char *buf, uint32_t size);

#endif // ULIB_USNDFMT_H
