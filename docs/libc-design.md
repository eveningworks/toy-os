# A real C library for toy-os

A staged plan, in the shape `docs/init-design.md` and
`docs/signals-design.md` used. It answers the question
`docs/roadmap.md` has carried since Phase 4 was written -- **what is
actually missing between what ring 3 has today and a libc a stranger's
C program can be compiled against?**

**Status: designed, not built.** Each stage below ships on its own and
is verifiable on its own; that is the point of staging it rather than
the reason it is slow.

**The target is decided: our own POSIX-shaped libc, compiled against.**
Not Linux syscall-ABI emulation. `docs/roadmap-details.md`'s "A real C
library" entry lays out both and recommends this one; the decision here
is only that the recommendation is taken, and that "port-capable" is the
bar -- a third-party C program should compile and run, not merely a
program written to pass a test.

## Why this is not "write stdio"

Half a libc already exists and nobody has written it down in one place,
so every session re-derives the gap and gets it wrong in the same two
directions: it under-counts stdio (which is the whole project) and
over-counts floating point (which is finished). The measurement below is
the useful part of this document; the staging follows from it.

## What exists today, measured

Checked against the tree before this plan was written, not assumed. Two
of these changed it.

- **`crt0` is real and shared.** `userland/rt/crt0.asm` -- `_start`,
  argc/argv/envp off a SysV stack, `main()`, exit with its return value.
  The per-binary `_start` copies `docs/roadmap-details.md` still
  describes are gone.
- **The syscall layer is real and typed.** `userland/rt/sys.h`, ~60
  wrappers, one definition per call. **A libc sits ON this, not instead
  of it** -- the same relationship musl has to its `__syscall`.
- **`errno` EXISTS.** `abi/errno.h`, `sys_errno()`, `sys_strerror()`,
  and every wrapper whose failure value is `-1` sets it. What is missing
  is only the C spelling: `errno` as an lvalue macro.
  **TLS is not a prerequisite for it.** With no threads, musl's own
  `errno` is `*__errno_location()` over a global; TLS is a
  stack-protector and future-threads item, and listing it under libc has
  made it look like a blocker twice.
- **`string.h` and `mem*` are done**, as the C names over the shared
  `k_*` toolkit -- one implementation, not two (`userland/lib/string.h`).
- **`snprintf`/`vsnprintf` are done**, as kfmt's formatter. The
  conversion set is kfmt's: `%d %u %x %s %c %%`, zero-pad width, `l/ll/z`.
- **`malloc`/`calloc`/`free` are done**, as `kernel/lib/heap_core.c`
  compiled twice over `SYS_SBRK`. No `realloc`, and `free()` cannot
  return memory to the kernel until `mmap` exists.
- **RING-3 FLOATING POINT IS FINISHED, and this is the measurement that
  most changes the plan.** CR0.EM is cleared and CR4.OSFXSR set
  (`kernel/arch/x86_64/fpu.c`), `USERLAND_CFLAGS` carries no
  `-mno-sse`, and the scheduler FXSAVEs/FXRSTORs eagerly on every switch
  (`scheduler.c`, three call sites) -- deliberately eager, because
  CVE-2018-3665 killed the lazy trick. Both halves are tested:
  `/tests/fpu_test` proves FP works, `/tests/fpu_race` proves two
  preempted processes do not share the register file.
  `docs/roadmap-details.md` said "nothing FXSAVEs anything anywhere"
  until this plan was written; that bullet is corrected in the same
  change.
- **The fd table is real**: `open`/`read`/`write`/`close`/`dup`/`dup2`/
  `pipe`, `chdir`/`getcwd` per process, path `stat`. So most of that
  entry's "unglamorous syscall surface" list is already built.
- **THREE SYSCALLS ARE GENUINELY ABSENT, and stdio needs all three**:
  `lseek` (there is no seek at all -- file I/O is open-then-sequential),
  `fstat` on an open fd (only paths can be stat'd), and `O_APPEND`
  (`SYS_O_*` is WRITE/CREAT/TRUNC only).
- **There is no environment.** `crt0.asm` hands `main()` an envp of
  exactly `NULL`, and `SYS_SPAWN` has nowhere to put one.
- **There is no `qsort`** -- `userland/ui/uui_table.c` hand-rolls a sort
  and says so in a comment, which makes it the second real caller the
  toolkit's bar asks for.

## What real systems do

- **musl** is the structure to copy: one static archive, no
  configuration, no porting layer between libc and the kernel, and a
  thin typed syscall layer underneath that the libc is built on rather
  than around. toy-os already has that split; it just stops halfway up.
- **newlib** interposes a `libgloss` stub layer (`_read`, `_write`,
  `_sbrk`) so one libc serves many targets. toy-os has exactly one
  target and should NOT pay for that indirection -- `userland/rt/` is
  already the stub layer, and adding a second would be two of them.
- **picolibc** is the most directly relevant precedent, because its
  reason to exist is **tiered `printf`**: an integer-only formatter and
  a float-capable one, chosen at link time, so an embedded image does
  not carry float conversion it never calls. That is precisely the
  position `kfmt` is in -- shared with a kernel built `-mno-sse`.
- **PDCLib** is the minimal teaching libc; worth reading for what it
  leaves out, not for its structure.

**Where toy-os should differ from all of them: the formatter stays
SHARED.** Every one of those projects has its own private `printf`. Here
the formatter is `kernel/lib/kfmt.c`, compiled into ring 0 and ring 3
from one source, and the whole toolkit convention exists to stop a
second copy appearing. Stage 4 below is how float gets in without
breaking that.

## The shape

```
              a ported C program
                     |
                #include <stdio.h>          <- Stage 1: a real include root
                     |
   +-----------------+------------------+
   |        libc (userland/libc/)       |   <- Stages 2-5
   |  stdio  stdlib  ctype  time  ...   |
   +-----------------+------------------+
                     |
        kfmt (shared, one formatter)  ------ float hook, Stage 4
                     |
          libsys (userland/rt/sys.h)        <- exists; unchanged
                     |
                  syscalls                  <- Stage 0 adds three
```

## What this is NOT

Stated so it stays decided, and consistent with
`docs/roadmap-details.md`'s existing list: no conformance or
certification, no locales, no `wchar`, no pthreads, no `select`/`poll`,
no shared-file `mmap`, and **no `signal.h`** -- that is
`docs/signals-design.md`'s, and a libc that ships a `signal()` which
cannot deliver anything is worse than one that does not declare it.

## Staging

### Stage 0 -- the three missing syscalls

`lseek`, `fstat` on an fd, and `O_APPEND`. Each is CLAUDE.md's usual
three edits (a number in `abi/syscall_abi.h`, a handler plus prototype,
a row in `syscall_table.c`). `isatty` is a FIELD of `fstat`, not its own
call -- one round trip, and the buffering policy in Stage 2 is its only
caller.

Verifiable alone, with no libc: a `/tests/seek_test` that writes a file,
seeks backwards, reads what it wrote, and appends to it.

### Stage 1 -- an include root

`userland/include/` on the userland `-I`, so `<stdio.h>`, `<string.h>`,
`<stdlib.h>` resolve. Today they are `"lib/stdio.h"`, which is a
perfectly good internal toolkit and is not a libc: **a program written
elsewhere includes `<stdio.h>` or it does not compile**, and that single
build decision is what separates the two.

Verifiable alone: an existing `/tests` binary rewritten to include only
angle-bracket headers, building and passing unchanged.

### Stage 2 -- stdio

The big one, and the only stage with real design in it. `FILE`,
`stdin`/`stdout`/`stderr`, `fopen`/`fclose`/`fread`/`fwrite`/`fgets`/
`fputs`/`fputc`/`getc`/`ungetc`/`fseek`/`ftell`/`rewind`/`fflush`/
`feof`/`ferror`, and the `printf` family over them.

The design is the buffering policy, and it is the standard one for a
reason: **unbuffered `stderr`, line-buffered when the fd is a terminal,
fully buffered otherwise** -- which is what Stage 0's `fstat` is for. An
unbuffered `printf` is one syscall per call, which is worse than the
`sys_print()`-shaped code it replaces, so a libc that skips buffering
makes the system slower and is not worth having.

Two traps to write down before building it. **`ungetc` is a one-byte
pushback, not a seek** -- implementing it by seeking backwards breaks on
a pipe. And **`exit()` must flush**, which is why Stage 3 owns
`atexit`/`exit` and why they cannot be deferred past this.

### Stage 3 -- the rest of the freestanding half

`strtol`/`strtoul`/`strtoll` with `endptr`, `base` and `ERANGE` (the
`k_parse_*` family has no endptr, so this is new code, not a wrapper),
`atoi`/`atol` on top; `realloc` (copy-based -- it cannot grow in place
and cannot shrink the process); `qsort` and `bsearch`; `abs`/`labs`/
`div`; `rand`/`srand` over `krandom`; `abort`; **`exit`/`atexit`**,
which is one line in `crt0.asm` -- it calls `sys_exit()` directly today
and says so; a full `<ctype.h>` table; `<assert.h>`; `<setjmp.h>`
(~15 lines of asm, needed by ports and by nothing here yet);
`<dirent.h>`'s `opendir`/`readdir`/`closedir` over `sys_listdir`; and a
`<unistd.h>` that is a thin header over wrappers that all already exist.

`errno` as an lvalue macro lands here: `#define errno (*__errno_location())`,
the storage moving out of `sys.c`'s file-static.

### Stage 4 -- floating point in the formatter

**Not a second formatter.** `kfmt.c` is compiled into a kernel built
`-mno-sse`, so it cannot contain float conversion; and a private libc
`printf` beside it is exactly the duplication the toolkit exists to
prevent.

The shape that keeps one formatter: **a conversion hook.** `kfmt.c`
handles `%f/%e/%g` by calling a function pointer that is NULL in the
kernel -- where the existing "unrecognised conversion is emitted
literally" behaviour is already correct and already documented -- and is
set by the libc's initialiser in ring 3, where the hook body is the only
object file compiled with SSE. This is picolibc's tiered printf reached
by a different route, and it keeps the kernel FP-free by construction
rather than by discipline.

Then `strtod`, and a `<math.h>` subset. Note that `kernel/lib/fixed.h`
is NOT this and must not be confused with it: its angles are in turns
and it exists precisely because the kernel has no float.

### Stage 5 -- time

`time_t`, `gmtime`/`localtime`/`mktime`/`strftime`, `clock()`. The epoch
conversion already exists (`tz_rtc_to_epoch()`/`tz_epoch_to_rtc()`, and
`fs_stat()` reports epochs on both backends); what this owns is the
calendar math and the stored UTC offset that
`docs/roadmap-details.md`'s `time_t` bullet already describes.

### Stage 6 -- proof

**Build and run a program nobody working on this repo wrote.** A test
that only exercises code written to pass it proves nothing here -- the
same lesson as the Nordic-character gates in `docs/decisions.md`. Pick
something small, self-contained, public-domain and integer-only for the
first one; the failures it produces are the real specification for
everything above.

## Open questions

- **The environment.** `getenv` can be written today and will always
  return NULL, because `crt0.asm` receives an envp of exactly `NULL` and
  `SYS_SPAWN` has nowhere to put one. Ported programs read `HOME`,
  `TERM` and `PATH`. Giving `SYS_SPAWN` an environment is a real
  kernel-side design item and is deliberately NOT in the staging above;
  it should be decided before Stage 6 picks a program, since it changes
  which programs are candidates.
- **What `free()` can never do.** Until `mmap`/`munmap` exist the
  process footprint only grows. That is fine for everything here and is
  a surprise to ported code that allocates in phases; say so in the
  header rather than discovering it in a soak test.
- **Whether `libc` and `libuapp` stay separate archives.** A GUI app
  links the toolkit; a `/bin` program should not have to. Splitting them
  is easy while the libc is new and awkward afterwards.
