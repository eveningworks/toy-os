#ifndef _INTTYPES_H
#define _INTTYPES_H

// <inttypes.h> -- the widest integer type's conversions.
//
// WHAT IS HERE AND WHAT IS NOT. C specifies this header as two halves:
// the `intmax_t` functions below, and the PRI*/SCN* format-string
// macros. Only the functions are here. The macros are a table of about
// a hundred defines whose whole purpose is to paper over platforms
// where `long` is not 64 bits -- and on this one it always is, so
// `%ld` is already right everywhere `PRId64` would be.
//
// **THE PORT THAT NEEDED THEM ARRIVED**, which is the condition this
// comment used to name: dash writes PRIdMAX. They are still a table
// that changes nothing on this target, and that is precisely why they
// are safe to write down once -- every one expands to the spelling
// that was already correct.
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


// --- PRI*: printf conversions ------------------------------------------

#define PRId8  "hhd"
#define PRId16  "hd"
#define PRId32  "d"
#define PRId64  "ld"
#define PRIdMAX "ld"
#define PRIdPTR "ld"
#define PRIdLEAST8  "hhd"
#define PRIdFAST8   "hhd"
#define PRIdLEAST16  "hd"
#define PRIdFAST16   "hd"
#define PRIdLEAST32  "d"
#define PRIdFAST32   "d"
#define PRIdLEAST64  "ld"
#define PRIdFAST64   "ld"
#define PRIi8  "hhi"
#define PRIi16  "hi"
#define PRIi32  "i"
#define PRIi64  "li"
#define PRIiMAX "li"
#define PRIiPTR "li"
#define PRIiLEAST8  "hhi"
#define PRIiFAST8   "hhi"
#define PRIiLEAST16  "hi"
#define PRIiFAST16   "hi"
#define PRIiLEAST32  "i"
#define PRIiFAST32   "i"
#define PRIiLEAST64  "li"
#define PRIiFAST64   "li"
#define PRIu8  "hhu"
#define PRIu16  "hu"
#define PRIu32  "u"
#define PRIu64  "lu"
#define PRIuMAX "lu"
#define PRIuPTR "lu"
#define PRIuLEAST8  "hhu"
#define PRIuFAST8   "hhu"
#define PRIuLEAST16  "hu"
#define PRIuFAST16   "hu"
#define PRIuLEAST32  "u"
#define PRIuFAST32   "u"
#define PRIuLEAST64  "lu"
#define PRIuFAST64   "lu"
#define PRIo8  "hho"
#define PRIo16  "ho"
#define PRIo32  "o"
#define PRIo64  "lo"
#define PRIoMAX "lo"
#define PRIoPTR "lo"
#define PRIoLEAST8  "hho"
#define PRIoFAST8   "hho"
#define PRIoLEAST16  "ho"
#define PRIoFAST16   "ho"
#define PRIoLEAST32  "o"
#define PRIoFAST32   "o"
#define PRIoLEAST64  "lo"
#define PRIoFAST64   "lo"
#define PRIx8  "hhx"
#define PRIx16  "hx"
#define PRIx32  "x"
#define PRIx64  "lx"
#define PRIxMAX "lx"
#define PRIxPTR "lx"
#define PRIxLEAST8  "hhx"
#define PRIxFAST8   "hhx"
#define PRIxLEAST16  "hx"
#define PRIxFAST16   "hx"
#define PRIxLEAST32  "x"
#define PRIxFAST32   "x"
#define PRIxLEAST64  "lx"
#define PRIxFAST64   "lx"
#define PRIX8  "hhX"
#define PRIX16  "hX"
#define PRIX32  "X"
#define PRIX64  "lX"
#define PRIXMAX "lX"
#define PRIXPTR "lX"
#define PRIXLEAST8  "hhX"
#define PRIXFAST8   "hhX"
#define PRIXLEAST16  "hX"
#define PRIXFAST16   "hX"
#define PRIXLEAST32  "X"
#define PRIXFAST32   "X"
#define PRIXLEAST64  "lX"
#define PRIXFAST64   "lX"

// --- SCN*: scanf conversions ------------------------------------------
#define SCNd8  "hhd"
#define SCNd16  "hd"
#define SCNd32  "d"
#define SCNd64  "ld"
#define SCNdMAX "ld"
#define SCNdPTR "ld"
#define SCNi8  "hhi"
#define SCNi16  "hi"
#define SCNi32  "i"
#define SCNi64  "li"
#define SCNiMAX "li"
#define SCNiPTR "li"
#define SCNu8  "hhu"
#define SCNu16  "hu"
#define SCNu32  "u"
#define SCNu64  "lu"
#define SCNuMAX "lu"
#define SCNuPTR "lu"
#define SCNo8  "hho"
#define SCNo16  "ho"
#define SCNo32  "o"
#define SCNo64  "lo"
#define SCNoMAX "lo"
#define SCNoPTR "lo"
#define SCNx8  "hhx"
#define SCNx16  "hx"
#define SCNx32  "x"
#define SCNx64  "lx"
#define SCNxMAX "lx"
#define SCNxPTR "lx"

#endif // _INTTYPES_H
