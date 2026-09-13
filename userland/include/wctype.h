#ifndef ULIB_WCTYPE_H
#define ULIB_WCTYPE_H

// The wide-character classifications, in the C locale -- see <wchar.h>
// for why that is a complete answer rather than a stub. Every one of
// these is its <ctype.h> counterpart for a value that fits in a byte,
// and false for anything above it: in a single-byte locale there are no
// characters above 0xFF to classify.

#include <wchar.h>

// A named character class, as returned by wctype(). The value is an
// opaque token; 0 means "no such class", which is what wctype() answers
// for a name it does not know.
typedef int wctype_t;

// Look up a class by POSIX name: "alpha", "digit", "space", "upper",
// "lower", "alnum", "punct", "print", "graph", "cntrl", "xdigit" and
// "blank". Returns 0 for anything else -- which a caller MUST check,
// because iswctype() with a 0 class is false for every character and a
// pattern like [[:nosuchclass:]] would otherwise silently match
// nothing. dash's expand.c is the caller that cares.
wctype_t wctype(const char *name);

// Is `wc` in class `desc`? False for a 0 class, always.
int iswctype(wint_t wc, wctype_t desc);

int iswalnum(wint_t wc);
int iswalpha(wint_t wc);
int iswblank(wint_t wc);
int iswcntrl(wint_t wc);
int iswdigit(wint_t wc);
int iswgraph(wint_t wc);
int iswlower(wint_t wc);
int iswprint(wint_t wc);
int iswpunct(wint_t wc);
int iswspace(wint_t wc);
int iswupper(wint_t wc);
int iswxdigit(wint_t wc);

wint_t towlower(wint_t wc);
wint_t towupper(wint_t wc);

#endif
