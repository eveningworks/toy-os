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
// -mno-sse), no `%p`, no precision, and no `*` width. Width on `%s`
// pads (and `%-Ns` left-justifies); a string longer than its field
// pushes the column rather than being truncated.
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

// THE SAME FORMATTER WITH NOWHERE TO RUN OUT OF ROOM.
//
// `sink` is handed each run of bytes as they are produced and `ctx` is
// whatever the caller wants to identify the destination -- a stream, a
// buffer it grows itself, a checksum. Nothing is stored on the way, so
// there is no capacity, nothing can be truncated, and no caller has to
// guess in advance how long a line might get.
//
// This is what lets ONE formatter serve both `snprintf` and a `printf`
// writing to a stream. The alternative -- formatting into a fixed
// scratch buffer and then writing that -- is what vga_printf() and
// klog_printf() do below, and it is fine for a diagnostic line and
// wrong for a C library: it caps the length of anything a program can
// print at whatever number this file happened to choose.
//
// The return value is the number of bytes handed to the sink, i.e. the
// full length. Unlike k_vsnprintf()'s, it cannot be "more than you
// got".
//
// The sink is called with SHORT runs (currently one byte at a time), so
// a sink that cares about efficiency must buffer on its own side --
// which is exactly what a FILE does. Do not add batching here: the
// scratch buffer it would need is the thing this call exists to avoid.
typedef void (*k_fmt_sink)(void *ctx, const char *s, size_t n);
size_t k_vcbprintf(k_fmt_sink sink, void *ctx, const char *fmt, va_list ap);
size_t k_cbprintf(k_fmt_sink sink, void *ctx, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

// The sink wrappers, which live in kfmt_print.c rather than kfmt.c.
// One header, two files, on purpose: kfmt.c is freestanding and is
// compiled a second time into libuapp.a so ring-3 programs share this
// formatter, and these two need vga.h/klog.h. See kfmt_print.c's top
// comment -- a kernel include in kfmt.c takes k_snprintf() away from
// userland without any other symptom.
//
// Both format into a fixed stack buffer (KFMT_LINE_MAX) and then hand
// the result to vga_write()/klog_write() -- a line longer than that is
// truncated, which is why this is for diagnostics and UI text rather
// than for anything that must not lose bytes. Nothing here allocates.
#define KFMT_LINE_MAX 256

void vga_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void klog_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
