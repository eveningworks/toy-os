#ifndef SYSCALL_TABLE_H
#define SYSCALL_TABLE_H

#include <stdint.h>

// The syscall table: one row per syscall number, and the only central
// thing about syscalls in this kernel.
//
// The handlers themselves are ordinary functions living with the
// subsystem that owns them (kernel/fs/fs_syscalls.c,
// kernel/proc/proc_syscalls.c, ...), which is the shape Linux and
// Windows NT both settled on -- Linux defines each call with
// SYSCALL_DEFINEn() in its own subsystem and dispatches through
// `sys_call_table[]`; NT's SSDT is an array of service pointers with
// the implementations in Io/Ob/Ps/Mm. Neither has a dispatch chain and
// neither keeps the implementations together.
//
// ONE table rather than two: `strace` used to carry its own list of
// names and argument kinds keyed by the same numbers, and it drifted --
// fourteen syscalls traced as a bare number, some for months, because
// nothing tied the two together. Dispatch and tracing now cannot
// disagree, and `kstack syscalls` reads the same rows.

// How `strace` prints one argument. Order matters only in that A_END
// must be 0, so an unlisted argument is absent rather than garbage.
enum sc_arg {
    A_END = 0, // no more arguments
    A_INT,     // signed decimal
    A_HEX,     // pointer/opaque, as hex
    A_FD,      // a file descriptor -- decimal, but named for readability
    A_PATH,    // pointer to a NUL-terminated path, printed as a quoted string
    A_BUF,     // pointer to a byte buffer whose length is the NEXT argument
    A_OFLAGS,  // SYS_O_* bitmask
};

enum sc_ret { R_DEC = 0, R_HEX };

// One of these is built per syscall entry and handed to the handler.
// It exists so a handler is a function of its ARGUMENTS rather than of
// the trapframe's register layout: `regs[9]` meaning "the first
// argument" is knowledge that belongs in one place, and every branch of
// the if/else chain this replaced repeated it.
//
// `pml4` is read unconditionally (a `mov %cr3`, a few cycles) where
// each branch that needed it used to call vmm_current_pml4() itself.
// Uniformity is worth more than the handlers that don't need it: one
// that forgets to read it and then validates a user pointer against
// nothing is the bug this removes the opportunity for.
struct syscall_ctx {
    uint64_t *regs; // the caller's trapframe; regs[14] is its return value
    uint64_t a0, a1, a2; // RDI, RSI, RDX -- the three argument registers
    uint64_t pml4;  // the CALLER's address space, for the vmm.h copy helpers
};

// A handler writes its own return value into `c->regs[14]` and returns
// whether it PARKED the caller (1) or not (0).
//
// That split -- rather than returning the value -- is deliberate:
// SYS_SBRK returns a pointer, so no 64-bit value is free to serve as an
// "I blocked, do not write a return value" sentinel. See
// docs/decisions.md.
typedef int (*syscall_fn)(struct syscall_ctx *c);

struct syscall_desc {
    const char *name;    // as `strace` and `kstack syscalls` print it
    syscall_fn fn;       // NULL for a number nothing implements
    enum sc_arg args[3];
    enum sc_ret ret;
};

// The row for `nr`, or NULL if the number is out of range. A row that
// exists but whose `fn` is NULL is a number with no handler; dispatch
// treats it as a no-op, and `strace` still names it if it has a name.
const struct syscall_desc *syscall_desc_at(uint64_t nr);

// Was this number DELETED rather than never used? A retired number keeps
// its hole for ever: reusing it would land an old binary's call on
// something else. The table's KTEST asks so it can still fail on an
// accidental gap, which is the thing that looks exactly like a syscall
// returning 0.
int syscall_is_retired(uint64_t nr);

#endif
