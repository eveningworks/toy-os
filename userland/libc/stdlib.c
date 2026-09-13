// exit(), atexit() and abort() -- the part of <stdlib.h> that has to
// exist the moment stdio does.
//
// WHY THIS IS NOT STAGE 3's PROBLEM (docs/libc-design.md lists atexit
// there): buffering means output written is not output emitted, and the
// one thing that guarantees a program's last printf() reaches the
// screen is exit() flushing on the way out. A stdio that shipped
// without it would lose the final line of every program that does not
// end in a newline on a line-buffered stream -- which is a bug report
// about printf, not about atexit.
//
// crt0.asm calls exit() rather than sys_exit() for exactly this reason,
// and that ONE line is the whole hook. A program that calls sys_exit()
// itself still bypasses everything here; <stdio.h> says so.
#include <stdlib.h>
#include <stdio.h>
#include "rt/sys.h"
#include <errno.h>
#include <signal.h>
#include <float.h>

// 1e0..1e22 -- every power of ten a double represents EXACTLY. Past
// 1e22 the constant itself is already rounded, which is why scale10()
// below steps rather than reaching for one big power.
static const double POW10[] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
};

// C requires at least 32. A fixed array rather than a malloc'd list
// because the handlers must be runnable when the heap is in whatever
// state made the program exit.
#define ATEXIT_MAX 32

static void (*g_handlers[ATEXIT_MAX])(void);
static int g_count;
static int g_exiting; // exit() called from inside a handler: run no more

int atexit(void (*fn)(void)) {
    if (!fn || g_count >= ATEXIT_MAX) return -1;
    g_handlers[g_count++] = fn;
    return 0;
}

void exit(int code) {
    // LIFO, which is what C specifies and what makes handlers compose:
    // a handler registered later can rely on what an earlier one set up
    // still being there.
    //
    // The guard is not paranoia. A handler that calls exit() re-enters
    // this function, and without it the loop would run every handler
    // again from the top -- including the one that is mid-call.
    if (!g_exiting) {
        g_exiting = 1;
        while (g_count > 0) g_handlers[--g_count]();
    }
    // AFTER the handlers, because a handler is entitled to print.
    fflush(0);
    sys_exit(code);
}

void abort(void) {
    // stderr is unbuffered, so anything already reported is already
    // out; the buffered streams are deliberately NOT flushed, because
    // abort() means the program's state is not to be trusted and
    // committing a half-written file is a worse outcome than losing it.
    // C leaves this implementation-defined and both mainstream libcs
    // choose the same way.
    //
    // **RAISES SIGABRT RATHER THAN EXITING WITH ITS NUMBER.** This used
    // to call sys_exit(134) -- 128 + SIGABRT, the shell's convention --
    // on the stated grounds that the OS had no signals. It has them now,
    // and the difference is visible: a parent waiting on this child sees
    // a process that DIED ON A SIGNAL instead of one that chose to exit
    // with an unusual status, a handler installed for SIGABRT runs, and
    // a core-dumping default would be reachable if one is ever added.
    //
    // **THE INSTALLED HANDLER RUNS FIRST.** C says abort() raises
    // SIGABRT, and a handler for it is allowed to run -- that is the
    // whole point of catching it, and it is where a program writes its
    // crash report. Resetting to SIG_DFL before the first raise, as
    // this did, meant an installed handler was never called at all.
    //
    // C also says abort() must TERMINATE even if the handler returns.
    // So: raise once with whatever the program installed, and if we are
    // still here afterwards, reset to the default and raise again --
    // which cannot be caught a second time.
    raise(SIGABRT);
    signal(SIGABRT, SIG_DFL);
    raise(SIGABRT);
    // Unreachable unless SIGABRT is blocked, which C says abort() must
    // override -- unblocking it is not expressible here, so the old
    // exit status is the fallback rather than hanging.
    sys_exit(134);
}

// --- numbers from text ------------------------------------------------

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <limits.h>

// One parser under both signed and unsigned entry points. Splitting
// them would mean two copies of the base detection, the digit loop and
// the overflow rule -- and the overflow rule is the part that is easy
// to get subtly wrong in only one of them.
static unsigned long strto_core(const char *s, char **endptr, int base,
                                 int *negp, int *ovfp) {
    const char *start = s;
    *negp = 0;
    *ovfp = 0;

    while (isspace((unsigned char)*s)) s++;
    if (*s == '+' || *s == '-') { *negp = (*s == '-'); s++; }

    // **THE LEADING ZERO IS A CONVERTED DIGIT, NOT JUST A PREFIX**, and
    // forgetting that made strtol("0", &end, 0) report NO CONVERSION at
    // all -- base 0 saw the '0', chose octal, stepped past it, found no
    // digits after it and concluded nothing had been parsed. "09" and
    // "0x" went the same way, and the suite ASSERTED the "0xzz" case
    // (libc3_test.c), so the bug had a test defending it. Judged against
    // glibc by tools/libc_diff.py.
    //
    // `zero_at` is where to stop if the digit loop finds nothing: just
    // past that '0', which is the character that WAS converted.
    const char *zero_at = 0;
    if (base == 0) {
        // C's own literal rules, which is what base 0 means.
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; zero_at = s + 1; s += 2; }
        // C23's binary literal, which glibc has accepted for years. Same
        // shape as 0x, including that a bare "0b" converts just the '0'.
        else if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) { base = 2; zero_at = s + 1; s += 2; }
        else if (s[0] == '0') { base = 8; s++; zero_at = s; }
        else base = 10;
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        zero_at = s + 1;
        s += 2;
    } else if (base == 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
        zero_at = s + 1;
        s += 2;
    }

    unsigned long acc = 0;
    const char *digits = s;
    for (;;) {
        int c = (unsigned char)*s, d;
        if (isdigit(c)) d = c - '0';
        else if (isalpha(c)) d = tolower(c) - 'a' + 10;
        else break;
        if (d >= base) break;
        // Checked BEFORE the multiply, not after: an overflowing
        // multiply has already lost the information needed to detect
        // it, and "did it get smaller" is only true for some inputs.
        if (acc > (ULONG_MAX - (unsigned long)d) / (unsigned long)base) *ovfp = 1;
        else acc = acc * (unsigned long)base + (unsigned long)d;
        s++;
    }

    // NO CONVERSION: endptr gets the ORIGINAL pointer, not wherever the
    // sign or the "0x" left us. That is what lets a caller tell "0"
    // from "not a number", since both return zero.
    if (s == digits) {
        // A prefix was stepped over and no digits followed it: the '0'
        // itself is the conversion, and everything after it is the
        // remainder. Only a string with no digits AT ALL is "no
        // conversion", which is what gives endptr the ORIGINAL pointer.
        if (zero_at) { if (endptr) *endptr = (char *)zero_at; return 0; }
        if (endptr) *endptr = (char *)start;
        return 0;
    }
    if (endptr) *endptr = (char *)s;
    return acc;
}

long strtol(const char *nptr, char **endptr, int base) {
    int neg, ovf;
    unsigned long v = strto_core(nptr, endptr, base, &neg, &ovf);
    // LONG_MIN is one further from zero than LONG_MAX, so the limit
    // depends on the sign -- checking against LONG_MAX for both would
    // reject the one value that is legal only when negative.
    if (ovf || (!neg && v > (unsigned long)LONG_MAX) ||
        (neg && v > (unsigned long)LONG_MAX + 1)) {
        errno = ERANGE;
        return neg ? LONG_MIN : LONG_MAX;
    }
    // NEGATED IN UNSIGNED ARITHMETIC. `-(long)v` for v == 2^63 converts
    // out of range (implementation-defined) and then negates LONG_MIN,
    // which is undefined -- UBSan flags it on strtol("-9223372036854775808").
    // Subtracting from zero in unsigned gives the same bits with no UB.
    return neg ? (long)(0UL - v) : (long)v;
}

unsigned long strtoul(const char *nptr, char **endptr, int base) {
    int neg, ovf;
    unsigned long v = strto_core(nptr, endptr, base, &neg, &ovf);
    if (ovf) { errno = ERANGE; return ULONG_MAX; }
    // C really does specify that strtoul negates: strtoul("-1") is
    // ULONG_MAX. Surprising, and standard.
    // Same hazard as strtol's: stay in unsigned arithmetic.
    return neg ? 0UL - v : v;
}

int  atoi(const char *s) { return (int)strtol(s, 0, 10); }
long atol(const char *s) { return strtol(s, 0, 10); }

// THE `long long` FAMILY DELEGATES, and that is only correct because
// this ABI is LP64: long and long long are both 64 bits, so the ranges
// coincide and strtol()'s saturation and ERANGE are already the right
// answers. On an ABI where long long is wider these need strto_core()'s
// own accumulator against LLONG_MAX -- which is why they are written
// out here rather than left as macros in the header.
long long strtoll(const char *nptr, char **endptr, int base) {
    return strtol(nptr, endptr, base);
}

unsigned long long strtoull(const char *nptr, char **endptr, int base) {
    return strtoul(nptr, endptr, base);
}

long long atoll(const char *s) { return strtoll(s, 0, 10); }

// --- _Exit, and C11's second exit list ---------------------------------

// NO HANDLERS, NO FLUSH. A caller reaching for this has decided the
// process must stop being itself immediately -- the usual reason is a
// child that must not run its parent's atexit handlers a second time.
void _Exit(int code) { sys_exit(code); }

// A SEPARATE LIST FROM atexit's, which is the whole point of C11's
// pair: quick_exit runs these and NOT the atexit ones, so a handler
// registered here is for what must happen even when ordinary teardown
// is skipped. It does not flush either.
#define QUICK_MAX 32
static void (*g_quick[QUICK_MAX])(void);
static int g_quick_count;
static int g_quick_exiting;

int at_quick_exit(void (*fn)(void)) {
    if (!fn || g_quick_count >= QUICK_MAX) return -1;
    g_quick[g_quick_count++] = fn;
    return 0;
}

void quick_exit(int code) {
    // Same re-entry guard, and for the same reason as exit()'s: a
    // handler that calls quick_exit() would otherwise restart the list.
    if (!g_quick_exiting) {
        g_quick_exiting = 1;
        while (g_quick_count > 0) g_quick[--g_quick_count]();
    }
    sys_exit(code);
}

// --- div ---------------------------------------------------------------
//
// C99 pins `/` to truncate toward zero and `%` to take the dividend's
// sign, so these are the plain operators -- the value is that the pair
// is computed together and cannot disagree.
div_t   div(int num, int den)             { div_t r   = { num / den, num % den }; return r; }
ldiv_t  ldiv(long num, long den)          { ldiv_t r  = { num / den, num % den }; return r; }
lldiv_t lldiv(long long num, long long den) { lldiv_t r = { num / den, num % den }; return r; }

int       abs(int v)         { return v < 0 ? -v : v; }
long      labs(long v)       { return v < 0 ? -v : v; }
long long llabs(long long v) { return v < 0 ? -v : v; }

// --- realloc ----------------------------------------------------------

void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    if (!n) { free(p); return 0; }
    void *q = malloc(n);
    if (!q) return 0; // C: the ORIGINAL block is still valid on failure
    // ASKS THE ALLOCATOR how big the old block is. Copying `n` bytes
    // instead would read past a shorter block, and past the LAST block
    // in a region that is an unmapped page rather than merely other
    // people's data. kmalloc_size() was added for exactly this -- see
    // api/heap.h.
    size_t old = kmalloc_size(p);
    k_memcpy(q, p, old < n ? old : n);
    free(p);
    return q;
}

// --- sorting and searching --------------------------------------------

static void swap_bytes(char *a, char *b, size_t n) {
    for (size_t i = 0; i < n; i++) { char t = a[i]; a[i] = b[i]; b[i] = t; }
}

// Insertion sort over a byte-swap, NOT a quicksort.
//
// The honest reasons, in order. Every array anything here sorts is
// bounded and small (SYS_LISTDIR_MAX entries, UUI_TABLE_MAX_ROWS rows).
// A quicksort needs a pivot strategy or it is quadratic on sorted
// input, which is the common case in a file manager. And a recursive
// one needs stack this ring-3 frame budget (2 KiB) does not have to
// spare, while an iterative one needs an explicit stack that is more
// code than this whole function.
//
// So: C's qsort() is a CONTRACT, not an algorithm -- the standard names
// the function after quicksort and then requires nothing about how it
// sorts. What a caller is promised is the ordering and the comparator
// protocol, both of which this keeps. If a real caller ever sorts
// something big enough for O(n^2) to matter, that is the moment to
// write a better one, behind this same signature.
// HEAPSORT, not the insertion sort this used to be and not quicksort.
//
// **INSERTION SORT IS O(n^2), AND the caller cannot see that coming.**
// It is fine for the dozen-element arrays this tree sorts today and
// quadratic for anything a ported program hands it -- a directory
// listing of a few thousand names is tens of millions of comparisons,
// each an indirect call. A library function's cost is not the caller's
// to guess at.
//
// Heapsort rather than quicksort for two reasons that matter more here
// than the constant factor: it allocates NOTHING (quicksort needs a
// stack for the recursion, and this library's malloc can fail), and its
// worst case IS its average case, so there is no adversarial input that
// turns it quadratic. glibc's introsort is faster on random data and
// needs both a recursion stack and a fallback for the case where it is
// not; that is the right trade for glibc and the wrong one here.
static void sift(char *a, size_t root, size_t n, size_t size,
                 int (*cmp)(const void *, const void *)) {
    for (;;) {
        size_t big = root, l = 2 * root + 1, r = l + 1;
        if (l < n && cmp(a + big * size, a + l * size) < 0) big = l;
        if (r < n && cmp(a + big * size, a + r * size) < 0) big = r;
        if (big == root) return;
        swap_bytes(a + root * size, a + big * size, size);
        root = big;
    }
}

void qsort(void *base, size_t n, size_t size,
           int (*cmp)(const void *, const void *)) {
    if (!base || !cmp || size == 0 || n < 2) return;
    char *a = (char *)base;
    // Build the heap from the last parent downwards. `i` counts down
    // through a size_t, so the loop tests i-- rather than i > 0 to stay
    // clear of wrapping at zero.
    for (size_t i = n / 2; i-- > 0; ) sift(a, i, n, size, cmp);
    for (size_t end = n; end-- > 1; ) {
        swap_bytes(a, a + end * size, size);
        sift(a, 0, end, size, cmp);
    }
}

void *bsearch(const void *key, const void *base, size_t n, size_t size,
              int (*cmp)(const void *, const void *)) {
    if (!key || !base || !cmp || size == 0) return 0;
    const char *a = (const char *)base;
    size_t lo = 0, hi = n;
    while (lo < hi) {
        // lo + (hi - lo) / 2, not (lo + hi) / 2: the second overflows
        // once the two are large, and a size_t that wraps indexes
        // somewhere arbitrary rather than reporting anything.
        size_t mid = lo + (hi - lo) / 2;
        int c = cmp(key, a + mid * size);
        if (c == 0) return (void *)(a + mid * size);
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return 0;
}

// --- pseudo-random ----------------------------------------------------

// C99's own example generator, verbatim, so a seeded sequence is
// reproducible against any other implementation that copies it. It is
// NOT unguessable and must not be used as if it were -- sys_getrandom()
// is the real source and says so about its own quality.
static unsigned long g_seed = 1;

int rand(void) {
    g_seed = g_seed * 1103515245 + 12345;
    return (int)((g_seed / 65536) % 32768);
}

void srand(unsigned seed) { g_seed = seed; }

// --- strtod -----------------------------------------------------------
//
// The same accuracy caveat as printf's float side, and for the same
// reason: digits are accumulated by multiply-and-add rather than by
// exact arithmetic over the mantissa, so the last place is not
// guaranteed. strtod(printf("%.17g")) is NOT a round trip. What works:
// the ~15 significant digits anything here actually writes down.
#include <math.h>

// Apply a decimal exponent WITHOUT letting the intermediate overflow.
//
// **BUILDING THE POWER FIRST AND THEN DIVIDING LOSES EVERYTHING BELOW
// 1e-308.** The old code computed 10^|e| by squaring and then divided:
// for 1e-310 that is 10^310, which is +inf, and 1/inf is 0. So every
// subnormal parsed as zero. It also turned "0e999" into 0 * inf = NaN,
// which is a value the input cannot possibly denote.
//
// Scaling in steps of at most 1e22 keeps every intermediate finite and
// lets a subnormal result arrive by underflow rather than by accident.
// 1e22 because that is the largest power of ten a double holds EXACTLY
// -- past it the step itself would carry error into every iteration.
//
// NOT correctly rounded, and the difference is visible: 1e308 comes out
// one ULP high because fourteen multiplies each round. A correctly
// rounded conversion (Clinger, or Eisel-Lemire) is a real project and
// docs/libc-design.md has it as one; this is accurate to a couple of
// ULP and honest about it. tools/libc_diff.py measures the gap.
// Case-insensitive prefix match, for the spelled-out inf/nan forms.
static int ci_prefix(const char *s, const char *word) {
    for (; *word; s++, word++)
        if (tolower((unsigned char)*s) != *word) return 0;
    return 1;
}

static double scale10(double v, int e) {
    // ZERO STAYS ZERO WHATEVER THE EXPONENT. "0e999" denotes 0, and
    // reaching the multiply at all produced 0 * inf = NaN.
    if (v == 0.0) return v;
    while (e > 22) {
        v *= 1e22; e -= 22;
        if (v > DBL_MAX) { errno = ERANGE; return v; }   // +inf: overflowed
    }
    while (e < -22) {
        v /= 1e22; e += 22;
        if (v == 0.0) { errno = ERANGE; return v; }      // underflowed to zero
    }
    if (e >= 0) v *= POW10[e];
    else        v /= POW10[-e];
    if (v > DBL_MAX) errno = ERANGE;
    else if (v == 0.0) errno = ERANGE;
    return v;
}

double strtod(const char *nptr, char **endptr) {
    const char *s = nptr;
    while (isspace((unsigned char)*s)) s++;

    int neg = 0;
    if (*s == '+' || *s == '-') { neg = (*s == '-'); s++; }

    // inf/nan before digits, because "inf" is a legal input and would
    // otherwise parse as no conversion at all.
    // **"infinity" AND "nan(...)" ARE THE LONGER SPELLINGS C SPECIFIES,
    // and stopping at three characters is a wrong ANSWER rather than a
    // refusal**: "infinity" parsed as inf with endptr after "inf",
    // leaving "inity" for the caller to trip over. C says take the
    // LONGEST match of either form.
    if (ci_prefix(s, "inf")) {
        s += ci_prefix(s, "infinity") ? 8 : 3;
        if (endptr) *endptr = (char *)s;
        return neg ? -INFINITY : INFINITY;
    }
    if (ci_prefix(s, "nan")) {
        s += 3;
        // An n-char-sequence in parentheses is part of the token. The
        // payload is accepted and DISCARDED -- this library has one NaN
        // -- but it must be consumed, or the caller sees "(1234)".
        if (*s == '(') {
            const char *q = s + 1;
            while (*q == '_' || isalnum((unsigned char)*q)) q++;
            if (*q == ')') s = q + 1;
        }
        if (endptr) *endptr = (char *)s;
        return NAN;
    }

    // **HEXADECIMAL FLOATS, C99's 0x1.8p3 FORM.** Not a curiosity: it is
    // the only decimal-free way to write a double, so it is what test
    // vectors and serialisers use when they need a value to survive a
    // round trip exactly. Without it "0x1p2" parsed as the single digit
    // 0 and left "x1p2" behind, which is a wrong ANSWER rather than a
    // refusal -- the caller cannot tell it from a genuine zero.
    //
    // The binary exponent is applied by ldexp-style doubling rather than
    // by pow(2, p), so it stays exact: every step is a power of two and
    // a double multiplies by one without rounding.
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X') &&
        (isxdigit((unsigned char)s[2]) ||
         (s[2] == '.' && isxdigit((unsigned char)s[3])))) {
        s += 2;
        double hv = 0.0;
        int hany = 0, bexp = 0;
        for (; isxdigit((unsigned char)*s); s++) {
            int c = (unsigned char)*s;
            int d = isdigit(c) ? c - '0' : (tolower(c) - 'a' + 10);
            hv = hv * 16.0 + d;
            hany = 1;
        }
        if (*s == '.') {
            s++;
            for (; isxdigit((unsigned char)*s); s++) {
                int c = (unsigned char)*s;
                int d = isdigit(c) ? c - '0' : (tolower(c) - 'a' + 10);
                hv = hv * 16.0 + d;
                bexp -= 4;          // each hex digit past the point is 2^-4
                hany = 1;
            }
        }
        if (!hany) { if (endptr) *endptr = (char *)nptr; return 0.0; }
        if (*s == 'p' || *s == 'P') {
            const char *psave = s;
            s++;
            int pneg = 0;
            if (*s == '+' || *s == '-') { pneg = (*s == '-'); s++; }
            if (!isdigit((unsigned char)*s)) s = psave;   // "0x1p" -- p is not ours
            else {
                int pe = 0;
                for (; isdigit((unsigned char)*s); s++)
                    if (pe < 100000) pe = pe * 10 + (*s - '0');
                bexp += pneg ? -pe : pe;
            }
        }
        // ERANGE on the way out of range, the same contract scale10()
        // holds for the decimal form -- a caller cannot tell a genuine
        // infinity from an overflowed one without it.
        while (bexp > 0)  {
            hv *= 2.0; bexp--;
            if (hv > DBL_MAX) { errno = ERANGE; break; }
        }
        while (bexp < 0)  {
            hv /= 2.0; bexp++;
            if (hv == 0.0) { errno = ERANGE; break; }
        }
        if (endptr) *endptr = (char *)s;
        return neg ? -hv : hv;
    }

    // **THE MANTISSA IS AN INTEGER AND THE DIGIT COUNT IS AN EXPONENT.**
    // It used to accumulate into a double: `v = v * 10 + d` for the
    // integer part, and a parallel `frac`/`scale` pair for the fraction.
    // That overflows to +inf after 309 digits, BEFORE the exponent has
    // been applied -- so "1" followed by 309 zeros and "e-309", whose
    // value is 1, came out infinite, and "0." followed by 310 ones came
    // out NaN (inf/inf). The magnitude of the written-out number is not
    // the magnitude of what it denotes, and the old shape conflated
    // them.
    //
    // Accumulating at most DIG_MAX significant digits into a uint64 and
    // COUNTING the rest into exp10 keeps every intermediate finite
    // whatever the input's length. 19 because 10^19 overflows a uint64
    // and 10^18 does not; a double carries ~17 significant digits, so
    // nothing that would have changed the result is being dropped.
    #define DIG_MAX 19
    uint64_t mant = 0;
    int ndig = 0, exp10 = 0, any = 0;
    // **A LEADING ZERO IS NOT A SIGNIFICANT DIGIT, and counting it as one
    // spends the budget before the number starts.** "0." followed by 320
    // zeros and a 1 filled DIG_MAX with zeros and then DISCARDED the
    // only digit that carried value, returning 0 for a number that is
    // small but perfectly representable. `mant == 0` is the test for
    // "nothing significant yet".
    for (; isdigit((unsigned char)*s); s++) {
        any = 1;
        if (mant == 0 && *s == '0') continue;       // leading zero: free
        if (ndig < DIG_MAX) { mant = mant * 10 + (uint64_t)(*s - '0'); ndig++; }
        else exp10++;          // past capacity: its PLACE still counts
    }
    if (*s == '.') {
        s++;
        for (; isdigit((unsigned char)*s); s++) {
            any = 1;
            // A leading zero here costs no budget but DOES move the
            // point -- that is the whole difference from the integer
            // side, where a leading zero means nothing at all.
            if (mant == 0 && *s == '0') { exp10--; continue; }
            // A digit we can still hold moves the point left; one we
            // cannot is dropped and moves nothing, being below the
            // precision a double can express.
            if (ndig < DIG_MAX) {
                mant = mant * 10 + (uint64_t)(*s - '0');
                ndig++;
                exp10--;
            }
        }
    }
    if (!any) { if (endptr) *endptr = (char *)nptr; return 0.0; }
    double v = (double)mant;

    // exp10 carries the decimal point's position even when no 'e'
    // follows, so the scaling below is not conditional on one.
    if (*s == 'e' || *s == 'E') {
        const char *save = s;
        s++;
        int eneg = 0;
        if (*s == '+' || *s == '-') { eneg = (*s == '-'); s++; }
        if (!isdigit((unsigned char)*s)) {
            // "1e" with no exponent digits: the 'e' is NOT part of the
            // number, so back up rather than treating it as e0.
            s = save;
        } else {
            int e = 0;
            for (; isdigit((unsigned char)*s); s++) {
                if (e < 100000) e = e * 10 + (*s - '0');
            }
            if (eneg) e = -e;
            // CLAMPED before it is added, so a written exponent near
            // INT_MIN/INT_MAX cannot wrap when the digit count is folded
            // into it. Anything past +-100000 is unambiguously overflow
            // or underflow and scale10() will say so.
            if (e > 100000) e = 100000;
            if (e < -100000) e = -100000;
            exp10 += e;
        }
    }
    v = scale10(v, exp10);
    if (endptr) *endptr = (char *)s;
    return neg ? -v : v;
}

double atof(const char *s) { return strtod(s, 0); }
