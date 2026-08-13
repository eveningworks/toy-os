# kernel/include/

Three header directories, three audiences. Which one a header lives in
is not a filing preference -- it decides who can include it, because
each part of the build gets a different set of `-I` flags (see the
Makefile's `API_INCLUDES`/`KERNEL_INCLUDES`/`APPS_CFLAGS`).

| Directory | Who may include it | Enforced by |
|---|---|---|
| `api/` | `apps/`, `userland/`, and the kernel | on every include path |
| `abi/` | `userland/` and the kernel | on both, not a separate rule |
| `kernel/` | the kernel only | **absent** from `apps/`'s and `userland/`'s include path |

## api/ -- the app-facing surface

`kapi.h` and everything it aggregates. This is the one header `apps/`
is supposed to include (see CLAUDE.md), and everything reachable from it
is a promise: console, graphics, keyboard/mouse, timer/RTC, filesystem,
heap, PCI, version, and the shared toolkit (`string.h`, `knum.h`,
`kfmt.h`, `kpath.h`, `klineedit.h` -- see CLAUDE.md's note to check
these before hand-rolling a digit loop, a formatter or a path join).
Adding a header here means committing to it.

## abi/ -- the kernel<->userland contract

`syscall_abi.h` (syscall numbers and their calling convention) and
`userland_contract.h` (what a freestanding `/bin` ELF may assume on
entry). Shared by the kernel and by `userland/`'s programs, which are
compiled separately with their own flags and linked with no libc at
all. Nothing else has any business including these.

Changing anything here changes an interface that *already-built
binaries on disk* depend on -- `/bin` binaries are seeded into
`disk.img` and are not rebuilt by a kernel-only `make`. Treat it the
way you'd treat a published ABI.

## kernel/ -- internals

Paging, the physical/virtual memory managers, the syscall
implementation, process/scheduler internals, driver-private headers
(`i8042.h`, `io.h`), and the filesystem backend vtable (`fs_ops.h`).

These are deliberately *not* on `apps/`'s include path. Before this
split every header sat in one flat directory and the boundary CLAUDE.md
describes was convention only -- an app could `#include "vmm.h"` and
nothing would object. Now that's a compile error:

```
apps/about.c:1:10: fatal error: vmm.h: No such file or directory
```

## Which directory does a new header go in?

Start in `kernel/`. Move it to `api/` when an app genuinely needs it,
and treat that move as the decision it is -- see CLAUDE.md's note that a
capability missing from `kapi.h` is a sign it belongs behind a new
function in `kernel/`, not a reason to reach around the boundary.
