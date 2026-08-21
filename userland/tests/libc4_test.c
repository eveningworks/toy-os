// Stage 4 of the C library: %f/%e/%g, strtod, and the algebraic half of
// <math.h> (docs/libc-design.md).
//
// HOW THIS IS ASSERTED, and why it is not "the number looked right".
// Every check compares the FORMATTED TEXT against a literal, because
// that is the only thing a caller of printf can observe and because a
// float comparison would hide exactly the rounding this file is about.
// The accuracy limit is real and documented (printf_float.c): digits
// come from repeated scaling, not from exact arithmetic over the
// mantissa, so nothing here asserts a 17th significant digit -- and the
// test says so rather than quietly choosing easy inputs.
//
// The load-bearing checks:
//  - **9.999 at two places** proves rounding CARRIES into the integer
//    part. Rounding the fraction separately gets 9.99 -> "9.100" or
//    "10.00" wrong, and every friendlier input passes either way.
//  - **%g on both sides of its exponent rule** proves the rule is
//    implemented rather than one branch of it.
//  - **fmod on a huge ratio** proves it is not x - trunc(x/y)*y, which
//    loses everything once x/y passes the mantissa.
//  - **A %f in a KERNEL format string must stay literal** -- checked
//    from the kernel's own side by a KTEST, not here.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "rt/sys.h"

static int g_fail;
static char buf[128];

static void check(int ok, const char *what) {
    fputs(ok ? "  ok   " : "  FAIL ", stdout);
    fputs(what, stdout);
    fputc('\n', stdout);
    if (!ok) g_fail++;
}

// Formats and compares against the expected text, printing what came
// out when they differ -- a bare pass/fail on a formatter is very hard
// to act on.
static void fmt_is(const char *fmt, double v, const char *want, const char *what) {
    snprintf(buf, sizeof buf, fmt, v);
    int ok = strcmp(buf, want) == 0;
    if (!ok) {
        static char msg[192];
        snprintf(msg, sizeof msg, "%s   -- got \"%s\", wanted \"%s\"", what, buf, want);
        check(0, msg);
    } else {
        check(1, what);
    }
}

int main(void) {
    printf("libc4_test: %%f/%%e/%%g, strtod and math.h\n");

    // --- %f ----------------------------------------------------------
    fmt_is("%f", 0.0, "0.000000", "%f defaults to six places");
    fmt_is("%.2f", 3.14159, "3.14", "%.2f rounds down");
    fmt_is("%.2f", 3.14559, "3.15", "and up");
    fmt_is("%.2f", 9.999, "10.00", "a carry reaches the INTEGER part");
    fmt_is("%.0f", 2.5, "3", "%.0f rounds half away from zero");
    fmt_is("%.3f", -0.0005, "-0.001", "a negative keeps its sign through rounding");
    fmt_is("%.1f", 1234567.89, "1234567.9", "a seven-digit integer part is whole");
    fmt_is("%f", 1.0 / 0.0, "inf", "infinity");
    fmt_is("%f", -1.0 / 0.0, "-inf", "and negative infinity");
    // NAN, not 0.0/0.0: on x86-64 the NaN that division produces has
    // its SIGN BIT SET, so it prints as "-nan" -- and that is correct,
    // not a bug. glibc does the same, and C permits printf to report a
    // NaN's sign. Asserting against 0.0/0.0 was testing the FPU's
    // choice of default NaN rather than this formatter.
    fmt_is("%F", NAN, "NAN", "%F reports NaN in upper case");
    fmt_is("%f", 0.0 / 0.0, "-nan", "and a division's NaN keeps the sign bit x86 gives it");

    // --- %e ----------------------------------------------------------
    fmt_is("%e", 0.0, "0.000000e+00", "%e of zero");
    fmt_is("%.2e", 1234.5, "1.23e+03", "%.2e");
    fmt_is("%.2e", 0.000123, "1.23e-04", "a negative exponent");
    fmt_is("%.0e", 9.9, "1e+01", "a mantissa rounding to 10 bumps the exponent");
    fmt_is("%.3E", 6.02214e23, "6.022E+23", "%E is upper case and pads the exponent to two");

    // --- %g ----------------------------------------------------------
    fmt_is("%g", 100.0, "100", "%g drops a trailing point");
    fmt_is("%g", 0.0001, "0.0001", "and stays fixed at the -4 boundary");
    fmt_is("%g", 0.00001, "1e-05", "and switches to exponent past it");
    fmt_is("%g", 1234567.0, "1.23457e+06", "and past six significant digits");
    fmt_is("%.3g", 1234.0, "1.23e+03", "%.3g counts SIGNIFICANT digits");
    fmt_is("%g", 1.5, "1.5", "a real fraction keeps its digits");

    // --- width applies to floats too ---------------------------------
    snprintf(buf, sizeof buf, "[%10.2f][%-10.2f]", 3.5, 3.5);
    check(strcmp(buf, "[      3.50][3.50      ]") == 0, "width and '-' work on a float");

    // --- strtod ------------------------------------------------------
    char *end;
    check(strtod("1.5", &end) == 1.5 && *end == '\0', "strtod reads a simple value");
    check(strtod("-0.25xyz", &end) == -0.25 && strcmp(end, "xyz") == 0,
          "and endptr is the first unused byte");
    check(strtod("1e3", &end) == 1000.0, "an exponent");
    check(strtod("1.5e-3", &end) == 0.0015, "a negative exponent");
    const char *bad = "abc";
    check(strtod(bad, &end) == 0.0 && end == bad, "no conversion returns the original pointer");
    // "1e" is a value followed by a letter, NOT a number with an empty
    // exponent -- the classic strtod edge case.
    const char *trunc_exp = "1e";
    check(strtod(trunc_exp, &end) == 1.0 && end == trunc_exp + 1,
          "\"1e\" parses as 1 with the 'e' left over");
    check(isinf(strtod("inf", &end)) && isnan(strtod("nan", &end)),
          "inf and nan are accepted");
    // Round trip through the formatter at a precision the accuracy note
    // actually promises.
    snprintf(buf, sizeof buf, "%.6f", strtod("3.141593", &end));
    check(strcmp(buf, "3.141593") == 0, "a six-place value survives text -> double -> text");

    // --- math.h ------------------------------------------------------
    check(sqrt(144.0) == 12.0, "sqrt");
    check(fabs(-3.5) == 3.5 && fabs(3.5) == 3.5, "fabs");
    check(floor(2.7) == 2.0 && floor(-2.1) == -3.0, "floor goes toward -inf");
    check(ceil(2.1) == 3.0 && ceil(-2.7) == -2.0, "ceil goes toward +inf");
    check(trunc(2.7) == 2.0 && trunc(-2.7) == -2.0, "trunc goes toward zero");
    check(round(2.5) == 3.0 && round(-2.5) == -3.0, "round goes half AWAY from zero");
    check(floor(1e300) == 1e300, "floor of a value with no fraction bits is itself");
    check(copysign(3.0, -1.0) == -3.0, "copysign");
    check(ldexp(1.5, 3) == 12.0, "ldexp");
    int e;
    check(frexp(12.0, &e) == 0.75 && e == 4, "frexp splits into mantissa and exponent");
    check(fmod(10.0, 3.0) == 1.0, "fmod");
    check(fmod(-10.0, 3.0) == -1.0, "fmod keeps the dividend's sign");
    // EXACT expected values, not a range. "the answer is in [0, y)" is
    // satisfied by the naive x - trunc(x/y)*y, which returns 0.0 here
    // -- a control that broke fmod this way changed nothing until this
    // check named the number it wanted.
    //
    // 2^100 is exactly representable, and 2 == -1 (mod 3) so
    // 2^100 == 1 (mod 3); likewise 2^4 == 1 (mod 5) and 100 is a
    // multiple of 4, so 2^100 == 1 (mod 5). Both answers are 1.0, and
    // both ratios are far past what a 53-bit mantissa can hold.
    double big = ldexp(1.0, 100);
    check(fmod(big, 3.0) == 1.0, "fmod(2^100, 3) is exactly 1, not an approximation");
    check(fmod(big, 5.0) == 1.0, "fmod(2^100, 5) too -- the ratio is past the mantissa");
    check(isnan(fmod(1.0, 0.0)), "fmod by zero is NaN");
    check(isinf(INFINITY) && !isfinite(INFINITY) && isnan(NAN), "the classification macros");
    check(signbit(-0.0) && !signbit(0.0), "signbit sees a negative zero");

    if (g_fail) printf("libc4_test: %d FAILURES\n", g_fail);
    else        printf("libc4_test: all checks passed\n");
    return g_fail;
}
