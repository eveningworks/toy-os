// The signal name table, compiled into both rings -- see api/ksignal.h
// for why it is shared rather than duplicated.
//
// FREESTANDING, and it has to stay that way: this file is on the
// Makefile's shared-source list, so anything kernel-only in here would
// silently take `kill -TERM` away from ring 3.
#include "ksignal.h"
#include "signal_abi.h"
#include "string.h" // k_strcmp, k_tolower -- the toolkit, in both rings
#include "knum.h"   // k_parse_u32 -- which REJECTS rather than guessing

// One row per signal this kernel has. A TABLE rather than a switch
// because both directions need it and a switch can only be walked one
// way -- signal_name() would be a second, hand-kept copy.
static const struct {
    int sig;
    const char *name;
} SIGNALS[] = {
    { SIGINT,  "INT"  },
    { SIGQUIT, "QUIT" },
    { SIGKILL, "KILL" },
    { SIGSEGV, "SEGV" },
    { SIGTERM, "TERM" },
    { SIGCHLD, "CHLD" },
};
#define SIGNAL_COUNT (int)(sizeof SIGNALS / sizeof SIGNALS[0])

const char *signal_name(int sig) {
    for (int i = 0; i < SIGNAL_COUNT; i++)
        if (SIGNALS[i].sig == sig) return SIGNALS[i].name;
    return "?";
}

// Case-insensitive compare of two whole names.
static int name_matches(const char *want, const char *row) {
    if (!want || !row) return 0;
    for (int i = 0; ; i++) {
        unsigned char a = (unsigned char)want[i], b = (unsigned char)row[i];
        if (k_tolower(a) != k_tolower(b)) return 0;
        if (!a) return 1;
    }
}

int signal_from_name(const char *name) {
    if (!name || !name[0]) return 0;

    // A NUMBER, first: `kill -9` is what people's fingers do. Parsed
    // through k_parse_u32, which refuses "9x" instead of reading a 9 out
    // of it -- the argument names something to destroy, so a partial
    // parse is the wrong kind of forgiving.
    uint32_t n = 0;
    if (k_parse_u32(name, &n))
        return SIGNAL_VALID((int)n) ? (int)n : 0;

    // "SIGTERM" and "TERM" both, and in any case. The prefix is stripped
    // here rather than stored a second time in every table row.
    const char *bare = name;
    if (k_tolower((unsigned char)name[0]) == 's' &&
        k_tolower((unsigned char)name[1]) == 'i' &&
        k_tolower((unsigned char)name[2]) == 'g')
        bare = name + 3;

    for (int i = 0; i < SIGNAL_COUNT; i++)
        if (name_matches(bare, SIGNALS[i].name)) return SIGNALS[i].sig;
    return 0;
}
