// THE syscall table. One row per number: the handler, the name, and
// what `strace` should make of the arguments and the return value.
//
// This is the only file that knows about every syscall at once. The
// handlers are ordinary functions living with the subsystem that owns
// them; the shape and the reasoning are in kernel/include/kernel/
// syscall_table.h, and the design call (one merged row rather than two
// parallel tables) is written up in docs/decisions.md.
//
// THE ROWS ARE abi/syscall_rows.h, an X-macro list this file expands
// with the handler and /bin/strace's decoder expands without it -- so
// adding a syscall is a row THERE (its comment says how). Nothing else
// is a registry -- there is no init call to forget.
#include "syscalls.h"
#include "syscall_abi.h"
#include "scheduler.h"      // SCHED_KSTACK_SYSCALL_MAX -- asserted against below
#include "syscall_stall.h"  // SYSCALL_STALL_MAX -- likewise
#include <stddef.h>

// Designated initializers, so a row's INDEX is its syscall number.
// That is what makes this list unorderable-by-accident, and it carries
// the two compile-time guarantees a generated table was proposed for:
// an undefined SYS_* does not compile, and -Wextra's -Woverride-init
// makes a duplicated number an error rather than a silent last-wins.
// A generator would add a parser over a header that is mostly prose to
// buy the same two things -- see docs/decisions.md.
//
// A number with no row (or a row with a NULL `fn`) is unimplemented:
// dispatch answers -ENOSYS (syscall.c), and `strace` prints it as
// `syscall_<n>` with hex arguments rather than hiding it.
// A number that USED to be a syscall and is not one now. A hole gets
// -ENOSYS from the dispatcher like any other, so what this row adds is
// the NAME: `strace` then says what a stale binary asked for. Retired
// numbers without a row are declared in SYSCALL_RETIRED below;
// `syscall/no row is half-filled in` is the KTEST that insists on it.
static int sys_removed(struct syscall_ctx *c) {
    c->regs[14] = (uint64_t)(int64_t)-ENOSYS;
    return 0;
}

static const struct syscall_desc SYSCALL_TABLE[] = {
#define SYSCALL_ROW(nr, name, fn, a0, a1, a2, r) [nr] = { name, fn, { a0, a1, a2 }, r },
#include "syscall_rows.h"
#undef SYSCALL_ROW
};

#define SYSCALL_TABLE_COUNT (sizeof SYSCALL_TABLE / sizeof SYSCALL_TABLE[0])

// THE TWO PER-SYSCALL DIAGNOSTIC TABLES ARE SIZED AGAINST THIS ONE, and
// this is the only place that can check it. Both index by syscall number
// and both silently DROP anything past their end, so a table that falls
// behind reports a clean, plausible, incomplete answer -- which is what
// SCHED_KSTACK_SYSCALL_MAX did for every number from 64 up.
_Static_assert(SYSCALL_TABLE_COUNT <= SCHED_KSTACK_SYSCALL_MAX,
               "SCHED_KSTACK_SYSCALL_MAX is below the syscall table -- "
               "`kstack syscalls` would silently drop the numbers above it");
_Static_assert(SYSCALL_TABLE_COUNT <= SYSCALL_STALL_MAX,
               "SYSCALL_STALL_MAX is below the syscall table -- "
               "`stalls` would silently drop the numbers above it");

// **RETIRED NUMBERS, WHICH ARE NOT FREE NUMBERS.** A syscall that is
// deleted leaves a hole: reusing the number would make an old binary's
// call land on something else, and renumbering everything above it
// would break every other caller to tidy one gap. So the hole stays and
// is DECLARED here, where the table's own KTEST can tell a deliberate
// one from the accident it exists to catch -- a number defined with no
// row, which would look to a caller exactly like a syscall that does not
// exist yet -- which is what it is, and the KTEST keeps the two apart.
static const uint64_t SYSCALL_RETIRED[] = {
    3,   // SYS_GUI_INIT     -- mapped the whole framebuffer to ANY caller
    4,   // SYS_GUI_POLL_KEY -- deleted 2026-09-09 with their one caller
    5,   // SYS_READ_KEY     -- fd 0 blocks now; see abi/syscall_abi.h
    7,   // SYS_WIN_CREATE  -- the kernel composited a window itself
    8,   // SYS_WIN_PRESENT -- deleted 2026-09-08, see abi/syscall_abi.h
    39,  // SYS_WIN_DEBUG   -- deleted 2026-09-09; the `gui` relay became
         //                    the diagnostic registry, SYS_DIAG
};

int syscall_is_retired(uint64_t nr) {
    for (uint64_t i = 0; i < sizeof SYSCALL_RETIRED / sizeof SYSCALL_RETIRED[0]; i++)
        if (SYSCALL_RETIRED[i] == nr) return 1;
    return 0;
}

const struct syscall_desc *syscall_desc_at(uint64_t nr) {
    if (nr >= SYSCALL_TABLE_COUNT) return NULL;
    return &SYSCALL_TABLE[nr];
}
