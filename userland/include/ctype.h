#ifndef ULIB_CTYPE_H
#define ULIB_CTYPE_H

// C's <ctype.h>, ASCII only.
//
// WHY THESE ARE NOT ALL k_* WRAPPERS, unlike <string.h>'s. The toolkit
// has exactly four character predicates (k_isdigit, k_isspace,
// k_tolower, k_toupper), because those are the four the KERNEL has ever
// needed. Adding the other eight to api/string.h to wrap them here
// would put eight functions with no kernel caller into the kernel
// image, which is the opposite of the toolkit's own bar -- a second
// REAL caller, not a plausible one. So the four that exist are wrapped
// and the rest are defined here, where the C library is the caller.
//
// **ASCII ONLY, and that is not a temporary state.** There is no locale
// and there will not be one (docs/roadmap-details.md lists locales as
// deliberately not pursued), so these answer for bytes 0..127 and
// report everything above as "not a letter, not a digit, not printable
// punctuation". A UTF-8 lead byte is not alphabetic here, which is the
// honest answer for a function whose entire input is one byte.
//
// The argument is an `int` holding an `unsigned char` value or EOF, as
// C requires. A NEGATIVE argument that is not EOF is undefined in C and
// answers 0 here rather than indexing anything.
#include <kstring.h>

static inline int isdigit(int c) { return k_isdigit((char)c); }
static inline int isspace(int c) { return k_isspace((char)c); }
static inline int tolower(int c) { return k_tolower(c); }
static inline int toupper(int c) { return k_toupper(c); }

static inline int islower(int c) { return c >= 'a' && c <= 'z'; }
static inline int isupper(int c) { return c >= 'A' && c <= 'Z'; }
static inline int isalpha(int c) { return islower(c) || isupper(c); }
static inline int isalnum(int c) { return isalpha(c) || isdigit(c); }
static inline int isxdigit(int c) {
    return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
// The C definitions, which are about the ASCII table rather than about
// what a terminal does with the byte: printable is 0x20..0x7E, graphic
// is the same minus the space, control is everything below 0x20 plus
// DEL, punctuation is printable-and-not-alphanumeric-and-not-space.
static inline int isprint(int c) { return c >= 0x20 && c < 0x7F; }
static inline int isgraph(int c) { return c > 0x20 && c < 0x7F; }
static inline int iscntrl(int c) { return (c >= 0 && c < 0x20) || c == 0x7F; }
static inline int ispunct(int c) { return isgraph(c) && !isalnum(c); }
static inline int isblank(int c) { return c == ' ' || c == '\t'; }

#endif
