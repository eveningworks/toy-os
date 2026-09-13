#ifndef TERMKEY_H
#define TERMKEY_H

// termkey -- what a TERMINAL sends for a key, and how to read it back.
//
// **THE PROTOCOL ON A tty IS ANSI, AND THAT IS THE WHOLE POINT.** A key
// that is not a character travels as an escape sequence -- Up is
// `ESC [ A` -- because that is what every terminal since the VT100 has
// sent and what every program that reads a terminal already knows how
// to parse. Before this, toy-os sent the raw byte 0x91 instead: cheap,
// and understood by exactly the programs written for this system. The
// cost showed up the first time a real one was ported -- dash could not
// use its own libedit, because libedit expects ANSI and got a private
// encoding it had never heard of.
//
// **KEYSYMS ARE NOT AFFECTED, and the split is the one real systems
// make.** A GUI client still receives `KEY_*` values from the
// compositor, exactly as an X11 or Wayland client receives keysyms --
// nobody sends ANSI to a window. It is the TERMINAL EMULATOR that
// translates a keysym into a sequence for its pty, which is what
// Konsole and every other emulator does. So `api/keyboard.h`'s codes
// stay what they are; this is only about what crosses a tty.
//
// Two directions, and both are here so they cannot drift apart:
// `termkey_encode()` is what an emulator (and the console's own input
// path) writes, `termkey_feed()` is what a line editor or a pager
// reads. A sequence this encodes is a sequence this decodes -- which
// `termkey_cases.h` asserts as a round trip in both rings.
#include <stdint.h>

// The longest sequence encode() produces, including no terminator.
#define TERMKEY_MAX 8

// Writes the bytes a terminal sends for `key` (an `api/keyboard.h`
// KEY_* value, or an ordinary character) into `out`, and returns how
// many. An ordinary character is one byte, itself. Returns 0 for a key
// with no terminal representation, and the caller then sends nothing --
// which is right: a key a terminal has no sequence for is one a program
// reading that terminal could not act on anyway.
int termkey_encode(int key, char *out, int cap);

// The decoder's state. Zero it before first use; one per input stream.
struct termkey_state {
    // What has been consumed of a sequence in progress. Empty means
    // "not in one".
    char pending[TERMKEY_MAX];
    int  len;
};

// Not a key: the byte was consumed and the sequence is incomplete.
#define TERMKEY_MORE (-1)
// Not a key: the bytes were consumed and did not form one. A caller
// that wants to echo an unrecognised sequence can read `pending`.
#define TERMKEY_NONE (-2)

// Feeds one byte. Returns the key (an ASCII character or a KEY_*
// value), TERMKEY_MORE, or TERMKEY_NONE.
//
// **A LONE ESC IS RESOLVED BY THE NEXT BYTE, NOT BY A TIMER**, which is
// the one real difference from a hardware terminal. `ESC [` and `ESC O`
// begin a sequence; ESC followed by anything else is returned as ESC
// and the byte is fed again by the caller re-entering with it. That
// makes Alt-<key> and Esc behave as they always have here (klineedit's
// meta prefix), at the cost of Alt-[ being unreachable -- the same
// trade readline makes, and for the same reason.
int termkey_feed(struct termkey_state *st, int byte);

// After TERMKEY_NONE or a lone ESC: the bytes that were consumed and
// not turned into a key, so a caller can process them itself. `*len` is
// how many.
const char *termkey_pending(const struct termkey_state *st, int *len);

#endif
