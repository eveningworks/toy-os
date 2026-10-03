#ifndef ULIB_UDURATION_H
#define ULIB_UDURATION_H

// A length of time typed by a person -- "2", "0.25", "90s", "1.5m",
// "2h", "1d" -- as whole milliseconds. GNU sleep's operand syntax: a
// decimal number with an optional fraction and one optional unit
// suffix (s, m, h, d; none means seconds).
//
// A FRACTION BELOW A MILLISECOND ROUNDS UP, never down, so a wait built
// on the answer never ends early ("0.0001" is 1 ms, not 0). Integer
// arithmetic throughout: there is no floating point in ring 3.
//
// It REJECTS rather than guesses: no sign, no exponent, no `inf`, no
// space before the suffix, nothing after it, and nothing that overflows
// 64 bits of milliseconds.
#include <stdint.h>

// 1 and *out_ms on success; 0 and *out_ms untouched on anything else.
int uduration_parse_ms(const char *s, uint64_t *out_ms);

#endif
