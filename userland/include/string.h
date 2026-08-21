#ifndef ULIB_STRING_H
#define ULIB_STRING_H

#include "rt/sys.h" // strerror() -- one table, in libsys
#include <stddef.h>
#include <stdint.h>
// <kstring.h>, NOT <string.h>, and that is not a style choice. THIS
// file is the one <string.h> now resolves to: userland/include comes
// ahead of kernel/include/api on the ring-3 include path, or an app's
// <string.h> would find the toolkit's header instead of the C library's.
// So neither spelling of "string.h" can reach the other header from
// here -- both come back to this one, where the include guard turns the
// reference into a silent no-op and every k_* below goes undeclared, a
// confusing failure for a line that looks obviously correct.
// kernel/include/api/kstring.h exists to give the toolkit a second name
// this file can say.
#include <kstring.h> // the k_* implementations this header renames

// The C names, for ring 3 only. `#include <string.h>`.
//
// WHAT THIS IS. Every function here is the toolkit's k_* equivalent
// under its standard name -- there is no second implementation, and
// there must never be one. kernel/lib/string.c is compiled a second
// time into libuapp.a (the Makefile's shared-source rule), so a ring-3
// strlen() and the kernel's k_strlen() are the same code built for a
// different code model.
//
// WHY IT EXISTS. Two ring-3 programs had already hand-rolled their own
// `my_strlen`, `put_udec` and `put_hex`, each with a comment explaining
// that the toolkit was kernel-only -- which was true, and is the exact
// duplication the toolkit was created to end (nine copies of an
// int->string loop, ten of a hex one). It was true only because
// kfmt.c was not freestanding; it is now, so ring 3 gets the real
// thing. See lib/stdio.h for the formatter.
//
// WHY THE C NAMES AND NOT k_*. Three reasons, in order of weight.
// GCC can emit calls to memcpy/memset/memmove/memcmp on its own -- for
// a large struct assignment or an array initialiser -- even under
// -ffreestanding, and those calls need real symbols under exactly those
// names; nothing in this tree provided them, so that was a latent link
// failure waiting for the first big struct copy. Second, a future port
// of any outside C code expects these names. Third, ring-3 code is
// ordinary application code and reads better in the ordinary
// vocabulary. The KERNEL deliberately keeps the k_ prefix (see
// api/string.h's own note on why), and this header does not change
// that -- it is not on the kernel's include path.
//
// THE SPLIT BETWEEN A REAL SYMBOL AND AN INLINE. The four the compiler
// can call by itself are real functions in lib/string.c, because a
// compiler-emitted call cannot be satisfied by an inline. Everything
// else is a static inline wrapper: zero cost, no archive member, and
// nothing to link against by accident. That is the whole rule -- it is
// not a judgment call per function.
//
// Deliberately NOT a libc. No str[n]cat, no strtok, no locale, no
// allocation, and nothing here that api/string.h does not already
// implement. The bar for adding is this project's usual one: a second
// real caller. In particular strncpy() is absent on purpose -- k_strlcpy
// exists because strncpy's contract (no guaranteed NUL, pads to width)
// has caused real bugs everywhere it exists; strlcpy() below is the one
// to reach for.

// --- the four GCC can emit calls to (real symbols, lib/string.c) -----
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);

// --- everything else (inline, straight through to the toolkit) ------
static inline size_t strlen(const char *s) { return k_strlen(s); }
static inline int strcmp(const char *a, const char *b) { return k_strcmp(a, b); }
static inline int strncmp(const char *a, const char *b, size_t n) { return k_strncmp(a, b, n); }
static inline char *strcpy(char *dst, const char *src) { return k_strcpy(dst, src); }
static inline char *strchr(const char *s, int c) { return k_strchr(s, (char)c); }
static inline char *strrchr(const char *s, int c) { return k_strrchr(s, (char)c); }
static inline char *strstr(const char *h, const char *n) { return k_strstr(h, n); }
static inline int strcasecmp(const char *a, const char *b) { return k_strcasecmp(a, b); }

// BSD, not C -- and the reason it is here rather than strncpy is in the
// header comment above. Copies at most `n` bytes, ALWAYS NUL-terminates,
// and returns strlen(src) so truncation is detectable.
static inline size_t strlcpy(char *dst, const char *src, size_t n) { return k_strlcpy(dst, src, n); }
static inline size_t strlcat(char *dst, const char *src, size_t n) { return k_strlcat(dst, src, n); }

// <string.h>'s error message, under the C name. ONE implementation --
// this forwards to libsys's, which is where the table lives beside the
// codes it names (rt/sys.h). A second table here is exactly the drift
// this repo's shared-source rule exists to prevent.
static inline char *strerror(int e) { return (char *)sys_strerror(e); }

// <ctype.h>'s handful, ASCII-only (which is all k_tolower/k_toupper
// promise -- see api/string.h).
static inline int isdigit(int c) { return k_isdigit((char)c); }
static inline int isspace(int c) { return k_isspace((char)c); }
static inline int tolower(int c) { return k_tolower(c); }
static inline int toupper(int c) { return k_toupper(c); }

#endif
