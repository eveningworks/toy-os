#ifndef ULIB_LOCALE_H
#define ULIB_LOCALE_H

// **THERE IS ONE LOCALE AND IT IS "C".** Not a placeholder: this system
// is Latin-1 end to end, its fonts carry 101 glyphs, and its number and
// date formatting are fixed. A locale database would be a promise the
// rest of the system does not keep.
//
// setlocale() therefore SUCCEEDS for the locales that are really "C"
// and FAILS for anything else, rather than accepting a name and quietly
// ignoring it -- a caller that asks for de_DE and is told yes will
// format numbers wrongly and never find out. Refusing is this
// project's usual rule (a parser REJECTS rather than guesses); the
// exception is <sys/stat.h>'s mkdir mode, which is ignored because
// there is nothing for it to mean.

#include <stddef.h>

#define LC_ALL      0
#define LC_COLLATE  1
#define LC_CTYPE    2
#define LC_MONETARY 3
#define LC_NUMERIC  4
#define LC_TIME     5
#define LC_MESSAGES 6

// Set or query a locale. `locale` may be:
//   NULL      -- query: always returns "C"
//   ""        -- "take it from the environment", which here is "C"
//   "C"       -- the one locale there is
//   "POSIX"   -- its other name
// Anything else returns NULL, meaning the request was refused. dash
// calls this from var.c whenever LC_ALL or LANG is assigned, and takes
// NULL as "leave the old one alone", which is exactly right.
//
// The returned string is static and must not be freed or modified.
char *setlocale(int category, const char *locale);

// What the C locale's formatting looks like. Every pointer is to a
// static string; "" means "this locale has no such convention", and
// CHAR_MAX means "not available", both as C specifies.
struct lconv {
    char *decimal_point;
    char *thousands_sep;
    char *grouping;
    char *int_curr_symbol;
    char *currency_symbol;
    char *mon_decimal_point;
    char *mon_thousands_sep;
    char *mon_grouping;
    char *positive_sign;
    char *negative_sign;
    char int_frac_digits;
    char frac_digits;
    char p_cs_precedes;
    char p_sep_by_space;
    char n_cs_precedes;
    char n_sep_by_space;
    char p_sign_posn;
    char n_sign_posn;
};

struct lconv *localeconv(void);

#endif
