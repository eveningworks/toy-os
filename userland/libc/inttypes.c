// <inttypes.h>'s functions -- see the header for why its PRI*/SCN*
// macros are deliberately not here.
//
// intmax_t is `long long` on this ABI, so every one of these is its
// `ll` counterpart under C's widest-type name. Written out rather than
// left as macros because a target where intmax_t is wider needs exactly
// these four bodies changed and nothing else.
#include <inttypes.h>

intmax_t strtoimax(const char *nptr, char **endptr, int base) {
    return strtoll(nptr, endptr, base);
}

uintmax_t strtoumax(const char *nptr, char **endptr, int base) {
    return strtoull(nptr, endptr, base);
}

intmax_t imaxabs(intmax_t v) { return v < 0 ? -v : v; }

imaxdiv_t imaxdiv(intmax_t num, intmax_t den) {
    imaxdiv_t r = { num / den, num % den };
    return r;
}
