#ifndef TERMKEY_CASES_H
#define TERMKEY_CASES_H

// The table both rings assert against, the same arrangement
// klineedit_cases.h has: the logic is shared source compiled twice, and
// a case written once is checked in ring 0 as a KTEST and in ring 3 as
// /tests/termkey_test. Without that, ring 3 could fail to link a byte
// of it and every ring-0 test would still pass.
#include "keyboard.h"

struct termkey_case {
    int         key;      // what an emulator was asked to send
    const char *seq;      // what it puts on the wire
    const char *why;      // what the sequence IS, for a failure message
};

// **THESE ARE NOT OURS TO CHOOSE.** Every one is what a VT100-family
// terminal sends and what xterm, Konsole and the Linux console all
// agree on; the point of the change was to stop inventing an encoding.
// The arrows and Home/End are CSI; F1-F4 are SS3, which is the one
// family that surprises people.
static const struct termkey_case TERMKEY_CASES[] = {
    { KEY_ARROW_UP,      "\x1b[A",  "CSI A" },
    { KEY_ARROW_DOWN,    "\x1b[B",  "CSI B" },
    { KEY_ARROW_RIGHT,   "\x1b[C",  "CSI C" },
    { KEY_ARROW_LEFT,    "\x1b[D",  "CSI D" },
    { KEY_HOME,          "\x1b[H",  "CSI H" },
    { KEY_END,           "\x1b[F",  "CSI F" },
    { KEY_DELETE,        "\x1b[3~", "CSI 3 ~" },
    { KEY_PAGE_UP,       "\x1b[5~", "CSI 5 ~" },
    { KEY_PAGE_DOWN,     "\x1b[6~", "CSI 6 ~" },

    // MODIFIED KEYS ARE THE `1;N` FORM, xterm's: N is 1 + a bitmask
    // where 1 is Shift, 2 is Alt and 4 is Ctrl. So Shift+Left is
    // `CSI 1 ; 2 D` and Ctrl+Left is `CSI 1 ; 5 D`. toy-os has codes
    // for exactly the combinations its keyboard driver folds.
    { KEY_SHIFT_ARROW_LEFT,  "\x1b[1;2D", "CSI 1;2 D" },
    { KEY_SHIFT_ARROW_RIGHT, "\x1b[1;2C", "CSI 1;2 C" },
    { KEY_SHIFT_ARROW_UP,    "\x1b[1;2A", "CSI 1;2 A" },
    { KEY_SHIFT_ARROW_DOWN,  "\x1b[1;2B", "CSI 1;2 B" },
    { KEY_SHIFT_HOME,        "\x1b[1;2H", "CSI 1;2 H" },
    { KEY_SHIFT_END,         "\x1b[1;2F", "CSI 1;2 F" },
    { KEY_CTRL_ARROW_LEFT,   "\x1b[1;5D", "CSI 1;5 D" },
    { KEY_CTRL_ARROW_RIGHT,  "\x1b[1;5C", "CSI 1;5 C" },

    // SS3, not CSI, for the first four function keys -- the VT100's
    // application keypad, and what xterm still sends.
    { KEY_F2,  "\x1bOQ", "SS3 Q" },
    { KEY_F3,  "\x1bOR", "SS3 R" },
    { KEY_F4,  "\x1bOS", "SS3 S" },
    // F10 has no SS3 slot; it is the CSI 21 ~ of the extended set.
    { KEY_F10, "\x1b[21~", "CSI 21 ~" },
};

#define TERMKEY_CASE_COUNT ((int)(sizeof TERMKEY_CASES / sizeof TERMKEY_CASES[0]))

#endif
