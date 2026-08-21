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
    // 134 is 128 + SIGABRT, the shell convention for "died on signal 6"
    // -- borrowed as a recognisable number even though this OS has no
    // signals yet (docs/signals-design.md).
    sys_exit(134);
}

// --- numbers from text ------------------------------------------------

#include <ctype.h>
#include <errno.h>
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

    if (base == 0) {
        // C's own literal rules, which is what base 0 means.
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (s[0] == '0') { base = 8; s++; }
        else base = 10;
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
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
    if (s == digits) { if (endptr) *endptr = (char *)start; return 0; }
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
    return neg ? -(long)v : (long)v;
}

unsigned long strtoul(const char *nptr, char **endptr, int base) {
    int neg, ovf;
    unsigned long v = strto_core(nptr, endptr, base, &neg, &ovf);
    if (ovf) { errno = ERANGE; return ULONG_MAX; }
    // C really does specify that strtoul negates: strtoul("-1") is
    // ULONG_MAX. Surprising, and standard.
    return neg ? (unsigned long)(-(long)v) : v;
}

int  atoi(const char *s) { return (int)strtol(s, 0, 10); }
long atol(const char *s) { return strtol(s, 0, 10); }

int  abs(int v)   { return v < 0 ? -v : v; }
long labs(long v) { return v < 0 ? -v : v; }

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
void qsort(void *base, size_t n, size_t size,
           int (*cmp)(const void *, const void *)) {
    if (!base || !cmp || size == 0 || n < 2) return;
    char *a = (char *)base;
    for (size_t i = 1; i < n; i++)
        for (size_t j = i; j > 0 && cmp(a + (j - 1) * size, a + j * size) > 0; j--)
            swap_bytes(a + (j - 1) * size, a + j * size, size);
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

double strtod(const char *nptr, char **endptr) {
    const char *s = nptr;
    while (isspace((unsigned char)*s)) s++;

    int neg = 0;
    if (*s == '+' || *s == '-') { neg = (*s == '-'); s++; }

    // inf/nan before digits, because "inf" is a legal input and would
    // otherwise parse as no conversion at all.
    if ((s[0] == 'i' || s[0] == 'I') && (s[1] == 'n' || s[1] == 'N') &&
        (s[2] == 'f' || s[2] == 'F')) {
        if (endptr) *endptr = (char *)(s + 3);
        return neg ? -INFINITY : INFINITY;
    }
    if ((s[0] == 'n' || s[0] == 'N') && (s[1] == 'a' || s[1] == 'A') &&
        (s[2] == 'n' || s[2] == 'N')) {
        if (endptr) *endptr = (char *)(s + 3);
        return NAN;
    }

    double v = 0.0;
    int any = 0;
    for (; isdigit((unsigned char)*s); s++) { v = v * 10.0 + (*s - '0'); any = 1; }
    if (*s == '.') {
        s++;
        // The fraction is accumulated as an INTEGER and scaled once at
        // the end. Dividing as it goes (v += d / scale) compounds a
        // rounding error per digit; one division at the end has one.
        double frac = 0.0, scale = 1.0;
        for (; isdigit((unsigned char)*s); s++) {
            frac = frac * 10.0 + (*s - '0');
            scale *= 10.0;
            any = 1;
        }
        if (scale > 1.0) v += frac / scale;
    }
    if (!any) { if (endptr) *endptr = (char *)nptr; return 0.0; }

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
            // Powers of ten by squaring rather than a loop of
            // multiplies: 1e308 is 308 roundings the slow way and about
            // nine this way.
            double p = 1.0, base = 10.0;
            int n = e < 0 ? -e : e;
            while (n) {
                if (n & 1) p *= base;
                base *= base;
                n >>= 1;
            }
            v = e < 0 ? v / p : v * p;
        }
    }
    if (endptr) *endptr = (char *)s;
    return neg ? -v : v;
}

double atof(const char *s) { return strtod(s, 0); }
