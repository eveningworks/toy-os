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
// real caller.
//
// **strncpy() IS HERE NOW, AND THE REVERSAL IS THE POINT.** It was
// absent on purpose while this header served only toy-os's own code:
// its contract (no guaranteed NUL, pads the whole width) has caused
// real bugs everywhere it exists, and k_strlcpy is better in every way.
// But the audience changed -- a port-capable C library that omits a
// function C requires does not fail with a helpful message, it fails at
// LINK TIME in somebody else's source file. So it exists, with the
// warning attached rather than the function removed, and strlcpy()
// remains the one to reach for in code written here.

// --- the four GCC can emit calls to (real symbols, lib/string.c) -----
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);

// --- everything else (inline, straight through to the toolkit) ------
static inline size_t strlen(const char *s) { return k_strlen(s); }
static inline int strcmp(const char *a, const char *b) { return k_strcmp(a, b); }
static inline int strncmp(const char *a, const char *b, size_t n) { return k_strncmp(a, b, n); }
// C's contract, kept because tolibc aims to be COMPLETE; the kernel has
// no k_strcpy any more (string.h, api), so this is its own loop.
static inline char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++)) {}
    return dst;
}
static inline char *strchr(const char *s, int c) { return k_strchr(s, (char)c); }
static inline char *strrchr(const char *s, int c) { return k_strrchr(s, (char)c); }
static inline char *strstr(const char *h, const char *n) { return k_strstr(h, n); }
static inline int strcasecmp(const char *a, const char *b) { return k_strcasecmp(a, b); }

// --- the rest of C's set (real symbols, userland/libc/string.c) ------
//
// Out of line rather than inline, because a ported program may take the
// address of one -- qsort(a, n, sz, (int(*)(const void*,const void*))strcmp)
// is ordinary C, and a static inline gives every translation unit its
// own copy of that address.
//
// **strncpy: NOT a bounded strcpy.** It does not NUL-terminate when the
// source is `n` or more characters, and it pads the whole of the rest
// of `dst` with zeroes when it is shorter. Both surprise people. It is
// here because C requires it; use strlcpy().
char  *strncpy(char *dst, const char *src, size_t n);

// **RETURNS A POINTER TO THE NUL IT WROTE, or to d+n when it wrote
// none** -- which is the whole reason to use it over strncpy, whose
// return value tells a caller nothing it did not already have. It pads
// the remainder of `n` with NULs as strncpy does, and as with strncpy a
// source that fills the buffer exactly leaves the result UNTERMINATED,
// with the returned pointer at d+n. dash's jobs.c builds its command
// strings with it.
char  *stpncpy(char *d, const char *s, size_t n);
char  *strcat(char *dst, const char *src);
char  *strncat(char *dst, const char *src, size_t n);
void  *memchr(const void *s, int c, size_t n);
// The span of `s` made only of bytes in `accept` / not in `reject`.
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char  *strpbrk(const char *s, const char *accept);
// **strtok MODIFIES its input and keeps STATIC state between calls**,
// which makes it unusable from two places at once. strtok_r takes the
// state explicitly and is what to use in anything new.
char  *strtok(char *s, const char *delim);
char  *strtok_r(char *s, const char *delim, char **saveptr);
// POSIX rather than C, and universal in ported code. The result comes
// from malloc() and the caller frees it.
char  *strdup(const char *s);
char  *strndup(const char *s, size_t n);

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
// glibc's: the macro name ("ENOENT"), or NULL for an unknown code.
static inline const char *strerrorname_np(int e) { return sys_errname(e); }

// A memset() the optimiser is not allowed to delete. An ordinary
// memset() over a buffer that is dead afterwards -- a key, a
// passphrase -- is a store nothing reads, and GCC is entitled to remove
// it; that is why wiping a secret needs a name of its own rather than
// care at the call site. BSD's spelling, and the one ported crypto code
// reaches for.
void explicit_bzero(void *p, size_t n);

// THE "C" LOCALE IS THE ONLY LOCALE HERE, so collation is byte order
// and strcoll() IS strcmp(). They exist because C requires them and
// because a program written against them should not have to know that
// -- not because they can do anything strcmp() cannot.
int    strcoll(const char *a, const char *b);
size_t strxfrm(char *dst, const char *src, size_t n);

// <ctype.h>'s functions used to live here, which was always the wrong
// header for them -- they are in <ctype.h> now, where C puts them, and
// that file explains why only four of them are k_* wrappers.

#endif
