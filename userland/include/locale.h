#ifndef ULIB_LOCALE_H
#define ULIB_LOCALE_H

// **THE LOCALE IS A REGION'S FORMATS, NOT A LANGUAGE.** "C" plus one
// locale per row of /etc/locales ("iso", "fi", "us"): each says how a
// date, a time and a number are written and which day starts the week.
// Messages, month names and the character set stay English and Latin-1
// in every one -- LC_CTYPE, LC_COLLATE, LC_MESSAGES and LC_MONETARY are
// accepted and behave as "C".
//
// setlocale(LC_ALL, "") is how a program opts in, as on any POSIX
// system: LC_ALL, then LC_TIME/LC_NUMERIC, then LANG from the
// environment, and with none of those set, the system's choice in
// System Settings (/etc/locale.conf, declared by settings.d/locale.*).
// A program that never calls it stays in "C", and `LC_ALL=C` puts one
// that does back there -- what a script parsing a tool's output sets.
//
// A NAME THAT IS NOT A LOCALE IS REFUSED, not ignored: a caller told
// "yes" for de_DE would format numbers wrongly and never find out (a
// parser REJECTS rather than guesses).
#include <stddef.h>

#define LC_ALL      0
#define LC_COLLATE  1
#define LC_CTYPE    2
#define LC_MONETARY 3
#define LC_NUMERIC  4
#define LC_TIME     5
#define LC_MESSAGES 6

// Set or query a locale. `locale` may be:
//   NULL        -- query: the category's locale name (LC_ALL: LC_TIME's)
//   ""          -- the environment, else the system setting (above)
//   "C"/"POSIX" -- the C locale
//   a region    -- a name from /etc/locales, with that region's formats,
//                  optionally overridden: "fi@time=24colon,week=sunday"
//                  (keys date, time, number, week, weeknum; the words
//                  are the locale.* settings')
// Anything else returns NULL and changes nothing. dash calls this from
// var.c whenever LC_ALL or LANG is assigned, and takes NULL as "leave
// the old one alone", which is exactly right.
//
// The returned string is static and must not be freed or modified.
char *setlocale(int category, const char *locale);

// The LC_NUMERIC locale's decimal mark and digit grouping (`grouping`
// is "\3" when there is a separator), and C's empty monetary fields.
// Every pointer is to a static string; "" means "this locale has no
// such convention", and CHAR_MAX means "not available", as C specifies.
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
