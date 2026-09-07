// The 128-bit division helpers GCC emits calls to, which normally live
// in libgcc.
//
// x86-64 multiplies 64x64 into 128 in one instruction (`mulq`), so
// `unsigned __int128` MULTIPLY compiles inline and needs nothing from
// here. There is no 128-bit DIVIDE instruction, so `a / b` on a 128-bit
// type becomes a call to one of these names instead -- and toy-os links
// -nostdlib with no libgcc, so without this file that call is an
// undefined symbol at link time rather than anything the compiler
// warns about.
//
// In tolibc rather than userland/rt/ because tolibc is the one library
// every program links, static or dynamic; nothing includes a header for
// these, the references are generated.
#include <stdint.h>

typedef unsigned __int128 u128;
typedef signed __int128 i128;

// Divide by zero is UNDEFINED for these helpers, and the useful
// behaviour is the loud one: `divq` with a zero divisor raises #DE,
// which reaches ring 3 as an ordinary process crash with a report in
// /var/crash. Returning a value would hide the caller's bug.
static inline void divide_by_zero(void) {
    __asm__ volatile("divq %0" : : "r"((uint64_t)0), "a"((uint64_t)1), "d"((uint64_t)0));
}

// Shift-subtract long division, one quotient bit per iteration. The
// fast path matters more than the loop: a caller whose operands both
// fit in 64 bits gets a single `divq` instead of 128 iterations, and
// that is the common case -- a bignum limb helper reaching for a
// 128-bit type usually has small values in it.
u128 __udivmodti4(u128 n, u128 d, u128 *rem) {
    if (d == 0) {
        divide_by_zero();
        return 0;
    }

    if ((d >> 64) == 0 && (n >> 64) == 0) {
        uint64_t q = (uint64_t)n / (uint64_t)d;
        if (rem) *rem = (uint64_t)n % (uint64_t)d;
        return q;
    }

    if (d > n) {
        if (rem) *rem = n;
        return 0;
    }

    u128 q = 0, r = 0;
    for (int i = 127; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) {
            r -= d;
            q |= (u128)1 << i;
        }
    }
    if (rem) *rem = r;
    return q;
}

u128 __udivti3(u128 n, u128 d) { return __udivmodti4(n, d, 0); }

u128 __umodti3(u128 n, u128 d) {
    u128 r;
    __udivmodti4(n, d, &r);
    return r;
}

// The signed forms divide magnitudes and reapply the sign, which is C's
// truncate-toward-zero rule. Negating with (u128)0 - x rather than -x
// keeps the most negative value (which has no positive counterpart)
// inside defined behaviour.
static inline u128 abs128(i128 v, int *neg) {
    *neg = v < 0;
    return *neg ? (u128)0 - (u128)v : (u128)v;
}

i128 __divti3(i128 a, i128 b) {
    int na, nb;
    u128 q = __udivmodti4(abs128(a, &na), abs128(b, &nb), 0);
    return (na ^ nb) ? (i128)((u128)0 - q) : (i128)q;
}

// The remainder takes the sign of the DIVIDEND, not of the divisor.
i128 __modti3(i128 a, i128 b) {
    int na, nb;
    u128 r;
    __udivmodti4(abs128(a, &na), abs128(b, &nb), &r);
    return na ? (i128)((u128)0 - r) : (i128)r;
}
