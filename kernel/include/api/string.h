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
char *k_strcpy(char *dst, const char *src);
int k_strncmp(const char *a, const char *b, size_t n);

// Copies at most `n` bytes and ALWAYS NUL-terminates, unlike C's
// strncpy (which doesn't, and pads to the full width when it does fit
// -- two behaviours that have caused real bugs everywhere they exist).
// Returns the length of `src`, so a caller can tell truncation happened
// by comparing against `n`. This is BSD strlcpy's contract, chosen
// deliberately over strncpy's.
size_t k_strlcpy(char *dst, const char *src, size_t n);

// First/last occurrence of `c` in `s`, or NULL. `c == '\0'` finds the
// terminator, matching C's strchr.
char *k_strchr(const char *s, char c);
char *k_strrchr(const char *s, char c);

// Byte comparison, same sign convention as k_strcmp.
int k_memcmp(const void *a, const void *b, size_t n);

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
// k_strstr, k_memcmp-style k_strcasecmp, k_isalpha/k_isalnum and
// k_tolower/k_toupper written and building -- and then nothing in the
// tree turned out to call them, so they were removed again before
// landing rather than shipped as a standing invitation. This project's
// rule is a second real caller, not a plausible future one (see
// CLAUDE.md on apps/ui/ widgets, same instinct); a case-folding helper
// in particular would need a decision about the non-ASCII range this
// kernel's own Å/Ä/Ö layouts live in, which is worth making when
// something actually needs it rather than in advance.
int k_isdigit(char c);
int k_isspace(char c); // space, tab, newline, carriage return, form feed, vertical tab

#endif
