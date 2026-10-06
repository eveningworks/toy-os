// lssound -- the sound devices, and what each one says it can do.
//
// THERE WAS NO WAY TO LIST THEM. The only view was the `audio_device`
// setting's choice list, which answers "what may I pick" -- a question
// about the SETTING. This answers "what is this", which is a question
// about the hardware, and is where a capability belongs.
//
// WHAT THE CARD SAYS IS NOT WHAT THE STACK USES. Every rate and depth
// here is read off the device and reported unchanged; the shared ring
// is 48 kHz 16-bit stereo throughout (abi/sound_abi.h), and nothing
// yet asks a card for anything else. These are the facts that would
// have to exist before that could change -- printed now so the
// question is answered by a measurement rather than a guess, which is
// the shape the EDID readout took before the modesetting work.
#include <stdio.h>
#include <string.h>
#include "rt/sys.h"
#include "query_abi.h"
#include "sound_abi.h"
#include "lib/cmd.h"

#define USAGE "lssound [-v]"

// In the mask's bit order, so the printed list is ascending.
static const struct { uint32_t bit; const char *text; } g_rates[] = {
    { SND_RATE_8000,   "8" },     { SND_RATE_11025,  "11.025" },
    { SND_RATE_16000,  "16" },    { SND_RATE_22050,  "22.05" },
    { SND_RATE_32000,  "32" },    { SND_RATE_44100,  "44.1" },
    { SND_RATE_48000,  "48" },    { SND_RATE_88200,  "88.2" },
    { SND_RATE_96000,  "96" },    { SND_RATE_176400, "176.4" },
    { SND_RATE_192000, "192" },
};

static const struct { uint32_t bit; const char *text; } g_depths[] = {
    { SND_DEPTH_8,  "8" },  { SND_DEPTH_16, "16" }, { SND_DEPTH_20, "20" },
    { SND_DEPTH_24, "24" }, { SND_DEPTH_32, "32" },
};

// A MASK OF 0 IS "THE DRIVER DOES NOT SAY", not "nothing" -- a device
// that supported no rate at all could not be registered. Printing them
// differently is the whole reason the distinction is in the ABI.
static void put_rates(uint32_t mask) {
    if (!mask) { printf("not reported"); return; }
    int first = 1;
    for (unsigned i = 0; i < sizeof g_rates / sizeof g_rates[0]; i++) {
        if (!(mask & g_rates[i].bit)) continue;
        printf("%s%s", first ? "" : " ", g_rates[i].text);
        first = 0;
    }
    printf(" kHz");
}

static void put_depths(uint32_t mask) {
    if (!mask) { printf("not reported"); return; }
    int first = 1;
    for (unsigned i = 0; i < sizeof g_depths / sizeof g_depths[0]; i++) {
        if (!(mask & g_depths[i].bit)) continue;
        printf("%s%s", first ? "" : " ", g_depths[i].text);
        first = 0;
    }
    printf("-bit");
}

int main(int argc, char **argv) {
    int verbose = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) { verbose = 1; continue; }
        cmd_usage(USAGE);
        return 1;
    }

    struct query_sound q;
    int n = 0;
    QUERY_FOREACH(QUERY_SOUND, q, i) {
        n++;
        // The active one is marked, as `lsblk` and the device rows in
        // the volume flyout mark theirs -- one card is playing and the
        // others are merely present.
        printf("%s %-10s %-24s %s\n", q.active ? "*" : " ", q.name,
               q.label, q.driver);
        if (!verbose) continue;
        printf("             rates:  "); put_rates(q.rates);  printf("\n");
        printf("             depths: "); put_depths(q.depths); printf("\n");
        if (q.bits) printf("             plays:  %u-bit\n", (unsigned)q.bits);
    }
    if (!n) {
        printf("no sound device\n");
        return 1;
    }
    if (!verbose)
        printf("\n%d device(s); -v for what each one supports\n", n);
    return 0;
}
