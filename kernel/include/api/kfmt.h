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
// Deliberately NOT a full printf, though the gap is narrow now.
// Supported conversions:
//
//   %d %i signed decimal    %u  unsigned decimal
//   %x %X hex, lower/upper  %o  octal
//   %p  a pointer, as "0x" + lowercase hex; NULL prints as "(nil)"
//   %s  const char * (NULL prints as "(null)")
//   %f %e %g (and %F %E %G) -- RING 3 ONLY, see k_fmt_float() below;
//              in the kernel these emit literally, since there is no
//              floating point there at all
//   %c  char                 %%  a literal '%'
//
// NOT here: %n, %a, and the wide-char conversions (%lc, %ls).
//
// Between the '%' and the conversion, C's own grammar, all of it parsed:
//
//   flags      - + space # 0
//   width      digits, or `*` to take it from an int argument -- and a
//              NEGATIVE `*` width left-justifies, as C says
//   precision  .N or .*, honoured by the INTEGER and FLOAT conversions
//   length     l/ll/z widen the argument to 64 bits; h/hh are parsed
//              and ignored, since default promotion has already widened
//              anything narrower than an int
//
// `%5u` pads with SPACES, `%05u` with zeroes, `%-5u` left-justifies.
// (It zero-padded every width until tolibc needed columns -- `%5d`
// printing 00042 is not what C means and not what a table wants.)
//
// PRECISION IS IGNORED BY `%s`, which is the difference between the two
// operations rather than an inconsistency: on an integer it only ever
// ADDS leading zeros, while on a string it TRUNCATES, and truncating a
// value is the one thing these formatters are not allowed to do. Width
// on `%s` pads (and `%-Ns` left-justifies); a string longer than its
// field pushes the column instead.
//
// Standard printf argument rules apply -- `%x` is an `unsigned int`,
// `%lx` is 64-bit -- which is not pedantry: varargs are only promoted
// as far as `int`, so reading a plain `int` as 64-bit would pick up
// whatever was in the top half of the register. Following the standard
// rules is also what keeps the `format(printf, ...)` attributes below
// meaningful, so GCC's -Wformat catches a mismatched argument at the
// call site instead of it becoming a garbage value at runtime.
//
// Anything unrecognised is emitted literally and consumes no argument,
// so a typo shows up in the output instead of silently eating the rest
// of the format string and desynchronising every argument after it.
// kfmt_cases.h is the exhaustive table that keeps this list honest.

// FLOATING POINT: kfmt.c CANNOT DO IT, and each build links one of two
// implementations of this.
//
// The formatter is compiled into a kernel built `-mno-sse`, where
// `va_arg(ap, double)` alone emits SSE instructions -- so the whole
// conversion sits behind this function, exactly as the sinks sit behind
// kfmt_print.c. `kernel/lib/kfmt_nofloat.c` is the kernel's and returns
// 0; `userland/libc/printf_float.c` is ring 3's and formats.
//
// Returning 0 means "not supported here", and the formatter falls
// through to its emit-it-literally path -- so `%f` in a KERNEL format
// string appears in the output as `%f`, rather than printing a wrong
// number or eating an argument.
//
// `conv` is one of f F e E g G, and `prec` is the precision or -1 for
// "not given" (which means 6, as C says).
//
// THE va_list IS PASSED BY VALUE AND THE CALLEE CONSUMES FROM IT. That
// reads wrong and is right here: on the SysV AMD64 ABI a `va_list` is an
// array type, so a `va_list` parameter is already a POINTER to the
// caller's state and a va_arg in the callee advances it. The caller must
// therefore NOT also consume the argument -- only this side knows a
// double is there. (C calls the caller's va_list indeterminate after
// this; on the one ABI this OS targets, it is the mechanism.)
//
// The buffer is the caller's and KFMT_FLOAT_MAX is what it must be:
// enough for %f of DBL_MAX's integer part plus a sign, a point and the
// precision cap.
#define KFMT_FLOAT_MAX 512
size_t k_fmt_float(char *out, size_t cap, va_list ap, char conv, int prec);

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
