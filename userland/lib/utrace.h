#ifndef ULIB_UTRACE_H
#define ULIB_UTRACE_H
// strace's records (abi/trace_abi.h) as text -- the decoding half of
// /bin/strace, kdump's to the kernel's ktrace. Names and argument kinds
// come from abi/syscall_rows.h, the same rows the kernel dispatches
// from, so the two cannot disagree about what a syscall is.
//
//     open("/etc/motd", O_WRITE|O_CREAT) = 3
//     read(9, 0x8010003020, 4096) = -9 EBADF
//     waitpid(23, 0x807ff00e10, 0) <unfinished ...>
//     <... waitpid resumed> = 23
//
// A formatter that runs out of room writes a shorter line, never past
// `cap`, and always terminates.
#include <stddef.h>
#include <stdint.h>
#include "trace_abi.h"

// A syscall's name, or NULL for a number with no row.
const char *utrace_name(int nr);
// The number for a name, or -1.
int utrace_lookup(const char *name);
// Does this call's return value carry an error? (A negative errno; a
// pointer-returning call's small negatives too, never its addresses.)
int utrace_failed(const struct trace_rec *exit);

// "name(args...)" from an ENTRY record. Returns the length.
size_t utrace_format_call(char *out, size_t cap, const struct trace_rec *entry);
// " = <value>" from an exit record of call `nr` -- decimal, hex for a
// pointer, "-2 ENOENT" for an error, " = ?" for one that never returns.
size_t utrace_format_ret(char *out, size_t cap, int nr, const struct trace_rec *exit);

// Pairs entries with their exits into whole lines. Feed it records in
// order; `emit` is called with each finished line (no newline).
struct utrace_printer {
    struct trace_rec pending;    // an entry still waiting for its exit
    int have_pending;
    void (*emit)(void *ctx, const char *line);
    void *ctx;
};
void utrace_printer_init(struct utrace_printer *p, void (*emit)(void *, const char *), void *ctx);
void utrace_feed(struct utrace_printer *p, const struct trace_rec *r);
// An entry left over at the end (the tracee died in it) as "<unfinished ...>".
void utrace_flush(struct utrace_printer *p);

#endif
