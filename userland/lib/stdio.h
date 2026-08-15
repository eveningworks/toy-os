#ifndef ULIB_STDIO_H
#define ULIB_STDIO_H

#include <stddef.h>
#include <stdarg.h>
#include <kfmt.h> // angle brackets for the same reason as lib/string.h's

// The C names for formatted output, for ring 3 only.
// `#include "lib/stdio.h"`. See lib/string.h's header comment for the
// whole rationale -- this is its other half, and the two were built
// together.
//
// snprintf()/vsnprintf() are kfmt's, which means the supported
// conversions are kfmt's too and NOT a full printf's: %d %u %x %s %c
// %%, an optional zero-pad width, and the l/ll/z length modifiers. No
// %f (there is no floating point in this project and the build passes
// -mno-sse), no %p, no left-justify, no precision, no `*` width. Read
// kfmt.h before assuming a conversion exists -- an unrecognised one is
// emitted literally and consumes no argument, so a typo shows up in the
// output rather than desynchronising every argument after it.
//
// Return value is C99's: the length the full result WOULD have had, so
// `>= cap` means truncated.
//
// WHAT IS DELIBERATELY MISSING: FILE, fopen, fread, putchar, and
// printf() itself. All four need a buffered stream layer over the fd
// syscalls, and buffering is the part with real design in it -- an
// unbuffered printf() is one syscall per call, which is worse than the
// puts()-shaped code it would replace. That is Milestone 24's work, not
// this header's. Write to a buffer here and hand it to sys_print() or
// sys_eprint() (rt/sys.h); a GUI client's diagnostics go to
// sys_eprint(), since its stdout goes nowhere useful.

static inline size_t vsnprintf(char *out, size_t cap, const char *fmt, va_list ap) {
    return k_vsnprintf(out, cap, fmt, ap);
}

// Not an inline: k_snprintf is variadic, and a wrapper would have to
// unpack and re-pack the argument list, losing GCC's format checking at
// the call site in the process. A macro keeps both -- the call IS
// k_snprintf, so -Wformat still sees a real printf-attributed function
// and catches a mismatched argument where it is written.
#define snprintf k_snprintf

#endif
