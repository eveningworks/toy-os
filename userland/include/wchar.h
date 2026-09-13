#ifndef ULIB_WCHAR_H
#define ULIB_WCHAR_H

// **THIS IS A C-LOCALE IMPLEMENTATION, AND THAT IS A COMPLETE ANSWER
// RATHER THAN A STUB.** In the C locale MB_CUR_MAX is 1: every byte is
// its own character, a multibyte sequence is never longer than one
// byte, and converting is a cast. That is not a simplification of the
// real thing -- it IS the real thing for a single-byte locale, and it
// is what glibc does when LC_ALL=C.
//
// It is also the honest answer for this system today, which is
// Latin-1 end to end: the console draws one glyph per byte and the
// fonts carry 101 of them (docs/conventions/gui.md). When the UTF-8
// migration happens these functions become real conversions and every
// caller keeps compiling -- which is the point of having them take
// mbstate_t now rather than later.
//
// What is NOT here: the wide-character string and I/O families
// (wcscpy, wprintf, fgetwc and the rest). Nothing needs them, and
// unlike the functions below they cannot be written correctly in an
// afternoon.

#include <stddef.h>
#include <stdint.h>

#ifndef WEOF
#define WEOF ((wint_t)-1)
#endif

// wchar_t is the compiler's, from <stddef.h>. wint_t is a wchar_t
// plus WEOF, which is why the isw* family takes it and the
// conversion functions above take wchar_t.
typedef int wint_t;

// **CONVERSION STATE, AND IN THIS LOCALE IT IS ALWAYS EMPTY.** A
// multibyte sequence never spans two calls when every sequence is one
// byte, so nothing is ever carried. The struct is not empty because C
// requires it to be a complete object type a caller can declare and
// zero; keeping a field makes `mbstate_t s = {}` mean what it looks
// like.
typedef struct { int __count; } mbstate_t;

// Bytes in the next multibyte character: 1 for any non-NUL byte, 0 for
// a NUL, and (size_t)-1 never, since no byte sequence is invalid in a
// single-byte locale. Returns (size_t)-2 -- "incomplete" -- only when
// `n` is 0, because then nothing was examined.
size_t mbrlen(const char *s, size_t n, mbstate_t *ps);

// Convert one multibyte character. Stores the byte, zero-extended, as
// the wide character. Same return convention as mbrlen(); a NUL stores
// 0 and returns 0, as C requires.
size_t mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps);

// Convert a whole string, advancing *src. Stops at the NUL (storing it
// and setting *src to NULL) or after `len` wide characters. Returns how
// many were produced, not counting the NUL.
size_t mbsrtowcs(wchar_t *dst, const char **src, size_t len, mbstate_t *ps);

// The first occurrence of `wc` in `ws`, or NULL. The NUL terminator is
// part of the string for this purpose, as strchr()'s is.
wchar_t *wcschr(const wchar_t *ws, wchar_t wc);

#endif
