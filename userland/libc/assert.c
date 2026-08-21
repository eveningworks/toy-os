// The out-of-line half of <assert.h>.
//
// Everything is written to STDERR, which is unbuffered, so the message
// is out before abort() runs. Going through stdout would risk the
// message dying in a buffer with the process -- which is the failure
// mode an assertion exists to avoid.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

void __assert_fail(const char *expr, const char *file, int line, const char *fn) {
    fprintf(stderr, "%s:%d: %s: assertion failed: %s\n", file, line, fn, expr);
    abort();
}
