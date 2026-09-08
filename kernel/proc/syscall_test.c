// Tests for the syscall table itself (kernel/proc/syscall_table.c).
//
// What these check is CONSISTENCY, not behaviour: every syscall's
// handler, its name and its argument description are one row now, and
// the failure these exist to catch is a row that is half-filled in. A
// handler with no name traces as `syscall_<n>`, which is precisely the
// drift that made merging the two tables worth doing -- fourteen
// syscalls were in that state, some for months, and nothing noticed
// because nothing compared the two lists.
//
// Deliberately NOT here: a second list of the syscall numbers to check
// the table against. That list could be forgotten exactly as easily as
// a row could, so it would prove nothing a reviewer doesn't already
// see. These walk the table itself instead.
#include "ktest.h"
#include "syscall_table.h"
#include "syscall_abi.h"
#include "strace.h"  // kernel-internal now: `strace` is a ring-3 program
#include "kapi.h"

// Every implemented syscall's number, from the table's own point of
// view: the first index with no row at all ends the walk.
static uint64_t table_len(void) {
    uint64_t n = 0;
    while (syscall_desc_at(n)) n++;
    return n;
}

KTEST("syscall", "the table bounds-checks, and index 0 is unused") {
    KTEST_ASSERT(table_len() > SYS_WIN_DEBUG); // the highest number today
    KTEST_ASSERT(syscall_desc_at(table_len()) == 0);
    KTEST_ASSERT(syscall_desc_at((uint64_t)-1) == 0);

    // Syscall numbers start at 1; row 0 exists but is empty, and
    // dispatch must treat it as unimplemented rather than calling
    // through a null pointer.
    const struct syscall_desc *zero = syscall_desc_at(0);
    KTEST_ASSERT(zero != 0);
    KTEST_ASSERT(zero->fn == 0 && zero->name == 0);
}

KTEST("syscall", "no row is half-filled in") {
    uint64_t n = table_len();
    for (uint64_t i = 1; i < n; i++) {
        const struct syscall_desc *d = syscall_desc_at(i);
        // A handler with no name would trace as a bare number; a name
        // with no handler would trace convincingly and do nothing.
        // Both are the drift this table was merged to make impossible.
        KTEST_ASSERT(!d->fn == !d->name);
        // A gap means a number was defined without a row -- dispatch
        // silently no-ops it, which looks to a caller exactly like a
        // syscall that returned 0. A RETIRED number is the one gap that
        // is meant: its syscall was deleted and the hole is kept so an
        // old binary's call cannot land on something else.
        if (syscall_is_retired(i)) {
            KTEST_ASSERT(d->fn == 0 && d->name == 0);
            continue;
        }
        KTEST_ASSERT(d->fn != 0);
        KTEST_ASSERT(d->name[0] != '\0');
    }
}

KTEST("syscall", "strace names come from the table") {
    // `strace` and `kstack syscalls` read the same rows dispatch does;
    // this is the assertion that they cannot answer differently.
    //
    // POINTER identity, not string equality, and that is the whole
    // point: a private list in strace.c holding the same text would
    // pass a k_strcmp and fail this.
    uint64_t n = table_len();
    for (uint64_t i = 1; i < n; i++) {
        const char *from_table = syscall_desc_at(i)->name;
        const char *from_strace = strace_syscall_name((int)i);
        KTEST_ASSERT(from_strace == from_table);
    }
    KTEST_ASSERT(strace_syscall_name(-1) == 0);
    KTEST_ASSERT(strace_syscall_name((int)n) == 0);

    // A spot check against the real names, so a table of empty strings
    // could not satisfy the loop above.
    KTEST_ASSERT(k_strcmp(strace_syscall_name(SYS_WRITE), "write") == 0);
    KTEST_ASSERT(k_strcmp(strace_syscall_name(SYS_WIN_DEBUG), "win_debug") == 0);
}

KTEST("syscall", "an argument list ends at A_END") {
    // `args` is a fixed three, so a syscall taking fewer says so by
    // leaving the rest A_END -- and A_END must stay 0, or an unlisted
    // argument decodes as whatever kind happens to be first.
    KTEST_ASSERT(A_END == 0);

    const struct syscall_desc *close = syscall_desc_at(SYS_CLOSE);
    KTEST_ASSERT(close->args[0] == A_FD);
    KTEST_ASSERT(close->args[1] == A_END);

    const struct syscall_desc *yield = syscall_desc_at(SYS_YIELD);
    KTEST_ASSERT(yield->args[0] == A_END);

    // sbrk is the only one returning a pointer, and the only reason
    // `ret` exists at all.
    KTEST_ASSERT(syscall_desc_at(SYS_SBRK)->ret == R_HEX);
    KTEST_ASSERT(syscall_desc_at(SYS_WRITE)->ret == R_DEC);
}
