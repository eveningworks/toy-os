#ifndef KNUM_H
#define KNUM_H

#include <stddef.h>
#include <stdint.h>

// Numbers <-> strings, in one place.
//
// This exists because the same twenty lines had been written nine
// separate times: `vga_write_dec`/`vga_write_hex` (vga.c) and
// `klog_write_dec`/`klog_write_hex` (klog.c) are byte-for-byte the same
// algorithm against two different sinks -- klog.h's own comment says so
// and explains the duplication was cheaper than the dependency -- plus
// local copies in multiboot.c, kernel.c, pci.c, calc_engine.c and
// (most recently) four appenders in strace.c. Parsing was the same
// story from the other direction: shell_sys.c, tz.c, json.c,
// keyboard_layout.c and desktop.c each had their own digit loop.
//
// The reason they were copied rather than shared is that each one
// writes to a DIFFERENT place -- the screen, the kernel log, a buffer,
// a window. So the shared thing can't be a printer; it has to be a
// converter that fills a caller-owned buffer and lets the caller
// decide where that goes. Every function here does exactly that, which
// is also what makes them unit-testable (kernel/lib/knum_test.c) --
// none of the nine originals had a single test, because you can't
// assert on something that has already gone to the screen.
//
// Conventions shared by everything in this header:
//   - Formatters return the length written, excluding the NUL. If the
//     result doesn't fit in `cap`, NOTHING is written except a NUL and
//     the return is 0 -- a truncated number is a wrong number, so it's
//     never produced silently.
//   - Parsers return 1 on success and 0 on rejection, leaving *out
//     untouched when they reject. They reject rather than guess: an
//     empty string, a stray non-digit, or a value too large for the
//     output type is a failure, not a best effort. (This is the
//     convention shell_sys.c's parse_decimal and keyboard_layout.c's
//     parse_hex2 already used; it's now the rule rather than a habit.)

// ---- number -> string ------------------------------------------------

// Unsigned decimal: 4096 -> "4096". A buffer of 21 bytes always fits.
size_t k_utoa(uint64_t v, char *out, size_t cap);

// Signed decimal, with a leading '-' when negative: -12 -> "-12".
// A buffer of 21 bytes always fits.
size_t k_itoa(int64_t v, char *out, size_t cap);

// Unsigned decimal, zero-padded to at least `min_digits`: (7, 2) ->
// "07". `min_digits` 0 or 1 behaves exactly like k_utoa. Padding never
// truncates a longer number -- (12345, 2) is still "12345".
size_t k_utoa_pad(uint64_t v, char *out, size_t cap, unsigned min_digits);

// Lowercase hex WITHOUT a "0x" prefix, zero-padded to at least
// `min_digits` (0 = no padding, shortest form): (0x1f, 0) -> "1f",
// (0x1f, 4) -> "001f". A buffer of 17 bytes always fits.
//
// The prefix is the caller's business on purpose -- both kinds of
// caller exist here. A one-off value wants "0x" and no padding
// (vga_write_hex); a table column wants fixed width and no prefix
// (pci.c's vendor:device fields, which is exactly why pci.c had its own
// copy rather than reusing klog_write_hex).
size_t k_htoa(uint64_t v, char *out, size_t cap, unsigned min_digits);

// ---- string -> number ------------------------------------------------

// Plain decimal, no sign, no whitespace, no prefix. Rejects an empty
// string, any non-digit anywhere, and anything that would overflow the
// output type.
int k_parse_u32(const char *s, uint32_t *out);
int k_parse_u64(const char *s, uint64_t *out);

// Signed decimal, optional leading '-'.
int k_parse_i64(const char *s, int64_t *out);

// Hex, with an optional "0x"/"0X" prefix, upper or lower case.
int k_parse_hex(const char *s, uint64_t *out);

// The same as k_parse_u64/k_parse_i64 but bounded to `n` characters
// rather than a NUL -- for parsing a field out of the middle of a
// larger buffer without copying it out first (tz.c's city-database
// rows, keyboard_layout.c's two-digit hex escapes). Trailing
// characters within `n` are still a rejection: the whole span must be
// the number.
int k_parse_u64_n(const char *s, size_t n, uint64_t *out);
int k_parse_i64_n(const char *s, size_t n, int64_t *out);

// One hex digit's value, or -1 if `c` isn't one. Exposed because three
// callers wanted exactly this and wrote it three times.
int k_hex_digit(char c);

#endif
