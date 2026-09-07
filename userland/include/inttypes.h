#ifndef _INTTYPES_H
#define _INTTYPES_H

// <inttypes.h> -- the widest integer type's conversions.
//
// WHAT IS HERE AND WHAT IS NOT. C specifies this header as two halves:
// the `intmax_t` functions below, and the PRI*/SCN* format-string
// macros. Only the functions are here. The macros are a table of about
// a hundred defines whose whole purpose is to paper over platforms
// where `long` is not 64 bits -- and on this one it always is, so
// `%ld` is already right everywhere `PRId64` would be. Adding them
// would be a hundred lines that change nothing; when a port arrives
// that needs them, they belong beside the type definitions rather than
// guessed at now.
//
// <stdint.h> comes from the compiler (this is a freestanding build), so
// intmax_t is already defined by the time anyone includes this.
#include <stdint.h>
#include <stdlib.h>

// intmax_t is `long long` here, so these are the `ll` parsers under the
// names C gives them for the widest type. They are separate functions
// rather than macros so that a target where intmax_t is wider has one
// place to change.
intmax_t  strtoimax(const char *nptr, char **endptr, int base);
uintmax_t strtoumax(const char *nptr, char **endptr, int base);
intmax_t  imaxabs(intmax_t v);

// imaxdiv, like div()/ldiv(): quotient and remainder together.
typedef struct { intmax_t quot, rem; } imaxdiv_t;
imaxdiv_t imaxdiv(intmax_t num, intmax_t den);

#endif // _INTTYPES_H
