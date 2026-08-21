// The KERNEL's half of kfmt's floating-point conversion: there isn't
// one.
//
// This file exists so that kfmt.c -- which is compiled into both the
// kernel and ring 3 from one source -- can call k_fmt_float()
// unconditionally, with the LINKER choosing the implementation instead
// of a preprocessor flag choosing it. Ring 3's is
// userland/libc/printf_float.c.
//
// WHY THE KERNEL CANNOT SIMPLY HAVE THE REAL ONE. It is built
// -mno-sse/-mno-sse2 (see kernel/include/kernel/fpu.h for why that
// split is worth copying from Linux and Windows rather than merely
// being conservative), and `va_arg(ap, double)` on x86-64 reads the
// varargs FP save area -- SSE instructions, in a kernel that does not
// save SSE state on the interrupt path. There is no version of this
// that is "just a bit of arithmetic".
//
// Returning 0 means "not supported", which sends the formatter down its
// emit-it-literally path: `%f` in a kernel format string appears in the
// output as `%f`. That is the right failure -- it is visible, it
// consumes no argument, and it cannot desynchronise the rest of the
// line. The alternative (printing a zero, or a garbage number read out
// of an integer register) would look like data.
#include "kfmt.h"

size_t k_fmt_float(char *out, size_t cap, va_list ap, char conv, int prec) {
    (void)out; (void)cap; (void)ap; (void)conv; (void)prec;
    return 0;
}
