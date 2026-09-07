#ifndef ULIB_LIMITS_H
#define ULIB_LIMITS_H

// C's <limits.h>, plus the POSIX names ported code reaches for.
//
// **EVERY INTEGER LIMIT IS DERIVED FROM GCC'S OWN PREDEFINES**, so none
// of them is a second copy of a number some other file has to keep
// true. `CHAR_MIN`/`CHAR_MAX` follow `__CHAR_UNSIGNED__` rather than
// assuming the x86-64 default, because a `-funsigned-char` build would
// otherwise get this silently wrong.
//
// **THIS HEADER INCLUDES NOTHING, AND MUST NOT START.** Nearly every
// translation unit pulls it in, ported code earliest of all, so a
// dependency here reaches everywhere -- which is why `PATH_MAX` is
// spelled out below instead of taken from `api/fs.h`'s `FS_PATH_MAX`.
// `userland/libc/access.c` asserts the two agree, so the duplication
// cannot drift.
//
// It exists because GCC's freestanding limits.h `#include_next`s the C
// library's, so without this header a `-nostdinc` compile fails inside
// GCC's own copy -- and `-nostdinc` is what a vendored third-party tree
// needs if no host header is to leak into it.

#define CHAR_BIT   __CHAR_BIT__
#define MB_LEN_MAX 1 // no locales and no multibyte conversion here

#define SCHAR_MAX __SCHAR_MAX__
#define SCHAR_MIN (-SCHAR_MAX - 1)
#define UCHAR_MAX (SCHAR_MAX * 2 + 1)

#ifdef __CHAR_UNSIGNED__
#define CHAR_MIN 0
#define CHAR_MAX UCHAR_MAX
#else
#define CHAR_MIN SCHAR_MIN
#define CHAR_MAX SCHAR_MAX
#endif

#define SHRT_MAX  __SHRT_MAX__
#define SHRT_MIN  (-SHRT_MAX - 1)
#define USHRT_MAX (SHRT_MAX * 2 + 1)

#define INT_MAX  __INT_MAX__
#define INT_MIN  (-INT_MAX - 1)
#define UINT_MAX (INT_MAX * 2U + 1U)

#define LONG_MAX  __LONG_MAX__
#define LONG_MIN  (-LONG_MAX - 1L)
#define ULONG_MAX (LONG_MAX * 2UL + 1UL)

#define LLONG_MAX  __LONG_LONG_MAX__
#define LLONG_MIN  (-LLONG_MAX - 1LL)
#define ULLONG_MAX (LLONG_MAX * 2ULL + 1ULL)

// POSIX. ssize_t is long here, so SSIZE_MAX is LONG_MAX.
#define SSIZE_MAX LONG_MAX

// PATH_MAX is the size of the buffer a path is declared with, counting
// the terminator -- the same thing `FS_PATH_MAX` means, which is what
// makes them comparable. NAME_MAX is one component of one, so it is
// PATH_MAX less the leading '/' and the terminator.
#define PATH_MAX 64
#define NAME_MAX (PATH_MAX - 2)

#endif
