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
  `k_*` toolkit -- one implementation, not two (`userland/include/string.h`).
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
                #include <stdio.h>          <- userland/include/  [Stage 1]
                     |
   +-----------------+------------------+
   |     libc.a (userland/libc/)        |   <- Stages 2-5
   |  stdio  stdlib  ctype  time  ...   |
   +-----------------+------------------+
                     |
        kfmt (shared, ONE formatter)  ------ sink form [Stage 2]
                     |                        float hook [Stage 4]
                     |
          libsys (userland/rt/sys.h)        <- exists; unchanged
                     |
                  syscalls              <- lseek/fstat/O_APPEND [Stage 0]
```

## What this is NOT

Stated so it stays decided, and consistent with
`docs/roadmap-details.md`'s existing list: no conformance or
certification, no locales, no `wchar`, no pthreads, no `select`/`poll`,
no shared-file `mmap`, and **no `signal.h`** -- that is
`docs/signals-design.md`'s, and a libc that ships a `signal()` which
cannot deliver anything is worse than one that does not declare it.

## Staging

### Stage 0 -- the three missing syscalls -- DONE

`SYS_LSEEK`, `SYS_FSTAT` and `SYS_O_APPEND`, with `/tests/seek_test` as
the proof. `isatty` is a FLAG of `fstat` (`SYS_STAT_TTY`) rather than a
call of its own -- a syscall returning one bit is a syscall the next
question makes redundant -- and `SYS_STAT_SEEKABLE` answers the other
half stdio needs.

**What it changed underneath, and this is the part to know: a write to a
file fd used to APPEND unconditionally**, ignoring the position the read
path maintained. That is not a thing `lseek` can be added to -- a
position one half of the interface ignores is not a position. So the
position is now shared by reads and writes, as in POSIX, and
`SYS_O_APPEND` is how a caller asks for the old behaviour. Nothing that
opens with `SYS_O_TRUNC` (almost everything here) changed at all:
position 0 of an emptied file is its end. The one real caller that DID
rely on the old silence is the shell's `>>`, which now says
`SYS_O_APPEND` -- and that is what finally makes `>` and `>>` different
operations rather than the same one with a truncate in front.

**And it fixed a bug found on the way.** `SYS_OPEN` tested a file's
existence by `fs_read()`-ing the whole thing, which cost a full read per
open and -- worse -- reported `-ENOENT` for any file too large for the
staging buffer, since `fs_read()` returns NULL for "cannot load this
whole" exactly as it does for "not there". A libc that opens real files
walks into that immediately. It asks `fs_exists()` now.

### Stage 1 -- an include root -- DONE

`userland/include/` holds the C library's PUBLIC headers and is on the
ring-3 `-I`, so `<stdio.h>`, `<string.h>` and `<stdlib.h>` resolve.
`userland/lib/` keeps the toy-os-internal ones (`cmd.h`, `tosh.h`,
`human.h`, `dirsort.h`, `tunable.h`, `uhistory.h`) and the
implementation. The split is by AUDIENCE, the same call
`kernel/include/api|abi|kernel` already made, because the alternative --
putting `userland/lib/` itself on the angle-bracket path -- makes the
libc's public surface "whatever happens to be in that directory".

**TWO THINGS COLLIDED, and both are worth knowing before touching the
include path again.**

`kernel/include/api/string.h` already owns the name `<string.h>`. The
C library's has to WIN that -- an app asking for `<string.h>` means the
C library's -- so `userland/include` goes ahead of `kernel/include/api`.
That leaves the libc header with no way to name the toolkit's: both
`<string.h>` and `"string.h"` come back to itself, where the include
guard turns the reference into a silent no-op and every `k_*`
disappears. `kernel/include/api/kstring.h` exists purely to give the
toolkit a second name, and does nothing else.

And **the shared sources had to have the libc taken back OFF their
path.** `kernel/lib/klineedit.c`, `kfmt.c` and `heap_core.c` are
compiled into both rings and all three include `"string.h"`. With
`-Iuserland/include` in front, that one source line resolves to the C
library's header in the ring-3 pass and the toolkit's in the kernel
pass -- the same line meaning two different files depending on which
pass compiled it. Demonstrated with `gcc -M`, not assumed. The
shared-source rule strips the flag (`SHARED_CFLAGS`), which turns
"freestanding, toolkit only" from a comment asking nicely into something
the build enforces.

### Stage 2 -- stdio -- DONE

`FILE`, `stdin`/`stdout`/`stderr`, `fopen`/`fclose`/`fread`/`fwrite`/
`fgets`/`fputs`/`fputc`/`getc`/`ungetc`/`fseek`/`ftell`/`rewind`/
`fflush`/`setvbuf`/`feof`/`ferror`/`fileno`, and the `printf` family
over them (`userland/libc/stdio.c`). Buffering is the standard policy:
unbuffered `stderr`, line-buffered on a terminal, fully buffered
otherwise, with "is it a terminal" being Stage 0's `SYS_STAT_TTY` asked
once per stream on first use.

**A FILE IS A READER OR A WRITER, NEVER BOTH**, and that is the one
place this diverges visibly from POSIX. The kernel's open file has a
single mode, so there is no read-write descriptor for `"r+"` to be built
on; `fopen` REFUSES those modes rather than opening something weaker. It
removes a real libc's hardest corner -- the flush-and-reposition dance
when a stream changes direction -- and removes it honestly.

**`printf` is built on `k_vcbprintf`, kfmt's SINK form**, added in the
same change. The formatter already funnelled every byte through one
`put()`, so a sink was a small change to shared code rather than a
second formatter: `struct out` gained a callback and `k_vsnprintf`
became one of two entry points onto the same `vformat()`. That keeps ONE
formatter in the tree and gives `printf` no maximum line length -- the
alternative, formatting into a fixed scratch buffer (what `vga_printf`
does), caps everything a program can print at a number this file would
have had to invent.

**`exit`/`atexit` landed here rather than in Stage 3**, because
buffering means output written is not output emitted, and the thing that
guarantees a program's last `printf` arrives is `exit()` flushing. That
is the one-line change in `crt0.asm` the plan predicted. `abort()`
deliberately does NOT flush: it means the state is not to be trusted,
and committing a half-written file is worse than losing it.

Three traps, all now in comments beside the code. **`ungetc` is a
one-byte pushback, never a backward seek** -- a pipe and a terminal have
no position, and those are exactly what a parser looks ahead on.
**`ftell` on a read stream is the fd's position MINUS what is unread in
the buffer**, which is invisibly correct for the first `BUFSIZ` bytes of
any file and wrong after. And **`SYS_WRITE` silently caps at
`SYS_WRITE_MAX` and returns the capped count**, so every write loops -- a
single call drops the tail of anything longer with no error to notice.

The cost, stated plainly: `crt0` now references `exit`, so every ring-3
program links the flush path whether or not it prints.
`-ffunction-sections` plus `--gc-sections` keeps that to the flush path
rather than the whole of stdio.

### Stage 3 -- the rest of the freestanding half -- DONE

`strtol`/`strtoul`/`atoi`/`atol`, `realloc`, `qsort`/`bsearch`,
`abs`/`labs`, `rand`/`srand`, `<ctype.h>`, `<assert.h>`, `<setjmp.h>`,
`<dirent.h>`, `<unistd.h>`, and `errno` as an lvalue.
(`exit`/`atexit`/`abort` moved to Stage 2 -- `exit()` has to flush, so
stdio could not ship without them.)

**Four things this stage settled that the plan had wrong or had not
seen.**

**`qsort` has no caller here, and the plan said it did.** Both sorts in
this tree are deliberately STABLE with the reasoning written down --
`uui_table` keeps a previous column's order within ties, `dirsort`
tie-breaks by name -- and `qsort` is not stable, so replacing either
would be a regression. `uui_table.c`'s "there is no qsort in this
toolkit" was a statement of fact, not a wish. It is in the C library
because C has one; that is a different bar from the toolkit's
second-real-caller rule, and the difference is the point of choosing
port-capable.

**`struct dirent` had to be renamed.** POSIX's `<dirent.h>` declares its
own with `d_name`, and two structs cannot share a tag in one
translation unit. The syscall ABI's entry is `struct sys_dirent` now,
which every other ABI struct's naming (`struct sys_stat`) already
implied.

**`errno.h` collided the way `string.h` did**, and the fix is the same:
`abi/kerrno.h` is a forwarding header giving the kernel's list a name
the C library has not taken. **The pattern is now general and stated in
both shims** -- when the C library takes a name a kernel header already
uses, the KERNEL header gets a k-prefixed alias. Only those two collide;
`api/` and `abi/` also own `fs.h`, `heap.h`, `timer.h`, `pipe.h` and
`query.h`, none of which the C library wants.

**`realloc` forced one addition to the shared allocator.**
`kmalloc_size()` (`api/heap.h`), because realloc has to copy
min(old, new) bytes and only the allocator knows the first -- copying
`new` from a shorter block walks off the end of the last block in a
region and into an unmapped page. That is a real requirement rather than
a convenience, which is why it was added despite having one caller.

**`<unistd.h>` omits rather than stubs.** No `fork`, no `exec`, no
`select`. A `fork()` that always failed would be worse than a link
error: the link error says "this program needs something this OS does
not have", which is exactly true and exactly what a porter needs to
read. `SYS_SPAWN` is `posix_spawn`-shaped on purpose
(`docs/init-design.md`), and a header pretending otherwise is how that
decision would get quietly reversed.

`<ctype.h>` wraps the four predicates the toolkit has and defines the
other eight itself, rather than growing `api/string.h` with eight
functions no kernel code calls. ASCII only, permanently -- there is no
locale and locales are on the deliberately-not-pursued list.

### Stage 4 -- floating point in the formatter -- DONE

`%f %e %g` (and `%F %E %G`), `strtod`/`atof`, and the algebraic half of
`<math.h>`.

**The shape predicted here was right, but the mechanism is a LINKED
SPLIT rather than a runtime hook.** `kfmt.c` calls `k_fmt_float()`
unconditionally and each build links exactly one implementation:
`kernel/lib/kfmt_nofloat.c` returns 0, `userland/libc/printf_float.c`
formats. That is the same one-header-two-files split `kfmt_print.c`
already uses for the sinks, so the module gained no new pattern -- and
it is better than the function pointer originally sketched here, which
would have needed something to register it before the first `printf`.

The kernel's returning 0 falls through to the formatter's
emit-it-literally path, so `%f` in a kernel format string appears in the
output AS `%f`. That is the right failure: visible, consumes no
argument, and cannot desynchronise the rest of the line. A KTEST asserts
it, because a ring-3 test cannot -- there, `%f` works.

**A precision had to be added to the shared parser.** `.N` is parsed for
every conversion and honoured only by the float ones -- the same shape
`-` already had (parsed, then ignored on a number). Not laziness: C's
precision on `%s` TRUNCATES, and truncating a string is the one thing
these formatters are not allowed to do.

**ACCURACY IS LIMITED AND SAID SO OUT LOUD.** Digits come from repeated
scaling by ten, not from exact arithmetic over the mantissa, so the last
place is not guaranteed and `strtod(printf("%.17g"))` does not round
trip. Getting that right means Dragon4 or Grisu/Ryu -- several hundred
lines and a table of powers of ten, buying accuracy in the 17th digit
that nothing here uses. `printf_float.c` is the whole surface to replace
if it ever matters, which is the reason the conversion is behind one
function.

`<math.h>` carries only what is EXACT: `sqrt` (one SSE instruction,
correctly rounded by the hardware), `fabs`, `floor`, `ceil`, `trunc`,
`round`, `fmod`, `copysign`, `ldexp`, `frexp`, and the classification
macros -- all written against the IEEE bits, because
`(double)(long long)x` is undefined once `|x|` passes `LLONG_MAX` and
that is not hypothetical for `floor(1e300)`. **The transcendentals are
deliberately absent**: `sin`, `exp`, `pow` need argument reduction and a
polynomial with an accuracy claim attached, which is a numerical-library
project rather than a corner of this one. A port that calls `sin()` gets
a link error naming it -- the same choice `<unistd.h>` makes about
`fork()`.

Not to be confused with `kernel/lib/fixed.h`, which is the KERNEL's
Q16.16 maths and exists precisely because ring 0 has no floating point.
Its angles are in turns. The two never meet.

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
- ~~**Whether `libc` and `libuapp` stay separate archives.**~~ DECIDED:
  SEPARATE. A `/bin` program links `libc`; a GUI app links `libc` plus
  the Toykit. It matches the split-by-role convention the build already
  derives things from, and it keeps the libc's audience "any C program"
  rather than "toy-os apps" -- which is the whole point of the
  port-capable target. Cheap to do while the libc is new, awkward after.
