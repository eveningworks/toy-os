#ifndef STRING_H
#define STRING_H

#include <stddef.h>
#include <stdint.h>

// Freestanding string/memory primitives -- there's no libc here, so
// this is the whole of it. Everything is `k_`-prefixed rather than
// shadowing the C names, since GCC knows what `strlen` means and
// recognising a hand-written one can produce surprising code in a
// freestanding build.
//
// The set below grew in one go after a survey found the same handful of
// scans open-coded all over the tree (`while (*p && *p != ' ') p++;`
// and friends) purely because this header only had six functions. See
// docs/decisions.md on the toolkit libraries; the rule of thumb for
// adding here is the same as everywhere else in this project -- a
// second real caller, not a speculative one.
//
// For numbers <-> strings see knum.h, and for building a whole
// formatted line see kfmt.h; neither belongs in this header.

size_t k_strlen(const char *s);
int k_strcmp(const char *a, const char *b);
void k_memset(void *dst, uint8_t val, size_t n);
void k_memcpy(void *dst, const void *src, size_t n);
int k_strncmp(const char *a, const char *b, size_t n);

// Copies at most `n` bytes and ALWAYS NUL-terminates, unlike C's
// strncpy (which doesn't, and pads to the full width when it does fit
// -- two behaviours that have caused real bugs everywhere they exist).
// Returns the length of `src`, so a caller can tell truncation happened
// by comparing against `n`. This is BSD strlcpy's contract, chosen
// deliberately over strncpy's.
size_t k_strlcpy(char *dst, const char *src, size_t n);

// Appends `src` to `dst`, writing at most `n` bytes in total and ALWAYS
// NUL-terminating. Returns the length the result WOULD have had, so
// `>= n` means it was truncated -- BSD strlcat's contract, matching
// k_strlcpy above.
//
// Added for the hand-rolled `strcpy(dst + strlen(dst), src)` idiom,
// which is an UNBOUNDED append however carefully the destination was
// sized (there is no k_strcpy any more, for the same reason): the
// shell's `ls` argument builder overflowed its buffer with
// enough flags in front of a long path.
//
// Not for building a large buffer in a loop: it rescans `dst` on every
// call, so appending n pieces costs O(n^2). Code assembling a whole
// file (etc_config.c) correctly keeps its own length counter instead.
size_t k_strlcat(char *dst, const char *src, size_t n);

// First/last occurrence of `c` in `s`, or NULL. `c == '\0'` finds the
// terminator, matching C's strchr.
char *k_strchr(const char *s, char c);
char *k_strrchr(const char *s, char c);

// First occurrence of `needle` in `haystack`, or NULL. An empty needle
// matches at the start, same as C's strstr. Its caller is the shell's
// Ctrl-R reverse history search -- this was written and then deleted
// again for having none, so if that search ever goes, check whether
// this should follow it.
char *k_strstr(const char *haystack, const char *needle);

// Byte comparison, same sign convention as k_strcmp.
int k_memcmp(const void *a, const void *b, size_t n);

// FNV-1a 32-bit hash (offset basis 0x811C9DC5, prime 0x01000193) --
// the project's one non-cryptographic checksum, mirrored host-side in
// tools/tfs3_writer.py. Promoted here from a
// TFS2 static once tfs3.c became the second real caller (the
// toolkit's usual bar). Torn-write/bit-rot detection strength only;
// never treat it as tamper-proof.
uint32_t k_fnv1a(const void *buf, size_t len);

// Like k_memcpy, but correct when the regions overlap. apps/ui/
// ui_textbox.c's insert/delete shifts are the callers, and one of them
// carried a comment explaining it was hand-rolled precisely because
// k_memcpy() promises nothing about overlap and there was nothing else
// to call. (ui_scrollback.c's superficially similar shifts are NOT
// callers: those index a ring buffer modulo its capacity, so the
// regions aren't contiguous and a memmove would be wrong.)
void k_memmove(void *dst, const void *src, size_t n);

// ASCII character classes.
//
// Only these two, deliberately. The batch that added them also had
// k_isalpha/k_isalnum written and building -- and then nothing in the
// tree turned out to call them, so they were removed again before
// landing rather than shipped as a standing invitation. This project's
// rule is a second real caller, not a plausible future one (see
// CLAUDE.md on apps/ui/ widgets, same instinct).
int k_isdigit(char c);
int k_isspace(char c); // space, tab, newline, carriage return, form feed, vertical tab

// Space or tab ONLY -- C's isblank(). The distinction from k_isspace()
// is the whole point and it is not pedantry: every LINE-ORIENTED parser
// here treats '\n' as a terminator, so skipping it as whitespace would
// run the parser straight into the next line. That is why six separate
// places had each written `c == ' ' || c == '\t'` by hand rather than
// call k_isspace() -- etc_config.c and tz.c as private `is_space()`
// helpers, keyboard_layout.c, demo.c and wm_debug.c inline, twice.
int k_isblank(char c);

// ASCII case folding. A-Z <-> a-z and nothing else: every byte outside
// that range, including the Latin-1 Å/Ä/Ö (0xC4/0xC5/0xD6 and their
// lowercase forms) this kernel's `se` layout produces, is returned
// unchanged.
//
// That limit is the deliberate part. These were written once before,
// found to have no caller and deleted; they came back for
// a city name typed in any case, and nothing in the timezone database
// -- or any other name compared this way -- is non-ASCII.
// Folding Latin-1 as well would have been range added ahead of a
// caller, and it isn't free to get right: `char` is signed here, so
// every byte >= 0x80 arrives negative. Widening later means folding
// 0xC0-0xDE <-> 0xE0-0xFE with 0xD7/0xF7 (the multiplication and
// division signs, which sit inside that block and are not letters)
// excluded.
//
// Both take and return `int`, holding an unsigned char value or EOF-ish
// negatives untouched, so a caller passing a signed `char` straight in
// can't silently fold the wrong thing.
//
// (This used to add that klineedit.c's Alt-U/L/C word case-changing
// "predates these and needs no general helper". It did not: it was a
// hand-rolled copy of exactly these two, as were uui_menubar's private
// `lower()` and keyboard.c's Ctrl-key fold. All three call these now.
// A helper's own header claiming a duplicate is deliberate is worth
// re-checking against the code before believing it.)
int k_tolower(int c);
int k_toupper(int c);
// The same over Latin-1 (one byte, 0..0xFF): ASCII, plus 0xE0-0xFE ->
// 0xC0-0xDE less the division sign. Sharp s and y-diaeresis have no
// Latin-1 capital and are returned unchanged.
int k_latin1_toupper(int c);

// Like k_strcmp, but ASCII-case-insensitive, with the same sign
// convention. Comparison is on the folded bytes, so the sign of a
// mismatch is the folded difference, not the raw one.
int k_strcasecmp(const char *a, const char *b);

#endif
