#ifndef KFMT_H
#define KFMT_H

#include <stddef.h>
#include <stdarg.h>

// One bounded string formatter, plus the two sink wrappers that make it
// worth having.
//
// The problem it solves is visible all over this kernel: printing one
// line of diagnostics takes six calls, because the only tools were
// "write a string" and "write a number".
//
//     vga_write("RIP="); vga_write_hex(rip);
//     vga_write("  CS="); vga_write_hex(cs);
//     vga_write(" (ring "); vga_write_dec(cs & 3); vga_write(")\n");
//
// versus
//
//     vga_printf("RIP=%x  CS=%x (ring %u)\n", rip, cs, cs & 3);
//
// Built on knum.h rather than duplicating its conversion loops -- the
// whole point of the toolkit is that there is one implementation of
// "integer to digits" in the tree.
//
// Deliberately NOT a full printf. Supported conversions:
//
//   %d  signed decimal      %u  unsigned decimal    %x  lowercase hex
//   %s  const char * (NULL prints as "(null)")
//   %c  char                %%  a literal '%'
//
// with an optional zero-pad width between the '%' and the conversion
// (`%04x`, `%02u`), and the `l`/`ll`/`z` length modifiers for 64-bit
// arguments (`%lx`, `%zu`). Standard printf argument rules apply --
// `%x` is an `unsigned int`, `%lx` is 64-bit -- which is not pedantry:
// varargs are only promoted as far as `int`, so reading a plain `int`
// as 64-bit would pick up whatever was in the top half of the
// register. Following the standard rules is also what keeps the
// `format(printf, ...)` attributes below meaningful, so GCC's -Wformat
// catches a mismatched argument at the call site instead of it becoming
// a garbage value at runtime.
//
// There is no `%f` (this kernel has no floating point and compiles with
// -mno-sse), no `%p`, no left-justify, no precision, and no `*` width.
// Anything unrecognised is emitted literally and consumes no argument,
// so a typo shows up in the output instead of silently eating the rest
// of the format string and desynchronising every argument after it.

// C99 snprintf semantics: writes at most `cap` bytes INCLUDING the NUL,
// always NUL-terminates when cap > 0, and returns the length the full
// result WOULD have had. So a return >= cap means it was truncated --
// the caller can detect that rather than silently shipping half a line.
size_t k_vsnprintf(char *out, size_t cap, const char *fmt, va_list ap);
size_t k_snprintf(char *out, size_t cap, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

// The sink wrappers. Both format into a fixed stack buffer
// (KFMT_LINE_MAX) and then hand the result to vga_write()/klog_write()
// -- a line longer than that is truncated, which is why this is for
// diagnostics and UI text rather than for anything that must not lose
// bytes. Nothing here allocates.
#define KFMT_LINE_MAX 256

void vga_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void klog_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
