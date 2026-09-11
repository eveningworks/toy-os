# tolibc -- a real C library for toy-os

A staged plan, in the shape `docs/init-design.md` and
`docs/signals-design.md` used. It answers the question
`docs/roadmap.md` has carried since Phase 4 was written -- **what is
actually missing between what ring 3 has today and a libc a stranger's
C program can be compiled against?**

**Status: BUILT.** All six stages below are done, and cJSON -- a
program nobody working on this repo wrote -- parses and re-serialises
JSON on toy-os. What remains is named at the end of this file.

**It is also a SHARED library now** (2026-08-28,
`docs/dynlink-design.md`): the same sources build `/lib/libc.so` in a
second `-fpic` compile, and every `/bin` and GUI program links it
through `/lib/ld-toy.so`. `pthread.c` stays static
(`libc_nonshared.a`, glibc's shape) because its `__thread` state is
TLS a library here may not carry. **And it stays tolibc**: replacing
it with musl was sized and declined -- musl is Linux-only by
construction, and the port is really a Linux-syscall-compat project
(`fork`/`execve`, `futex`, `*at`, `ioctl`, `stat`) that would also
cost the compiled-both-rings design. `docs/decisions.md`'s "tolibc
stays" entry is the full account, including what IS taken from musl
(individual implementations, with attribution) and the one goal that
would reopen the question.

**It is called `tolibc`**, formed the way `tosh` was (toy-os + `sh`;
toy-os + `libc`). It sits beside Toykit, and `userland/libc/README.md`
is its own front page. The archive stays `libc.a`, because a linker
expects that name and a ported build system saying `-lc` should not have
to know what this one is called.

**AND THE BAR FOR WHAT GOES IN IS DIFFERENT FROM THE REST OF THIS
PROJECT.** `tools/`, Toykit and the kernel toolkit all require a second
REAL caller before a function is added. tolibc aims to be COMPLETE
instead: if C specifies it, or POSIX specifies one that ported code
reaches for constantly, it belongs here even when nothing in toy-os
calls it yet. The difference is the audience -- those serve code written
here, and this serves code that has not been written yet, for which the
alternative to a function is a link error in somebody else's source file
with no explanation attached. What is still absent is absent for a
REASON (see the end of this file), not for lack of a caller.

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
  **TLS was not a prerequisite for it, and is now what it uses.** With
  no threads, musl's own `errno` is `*__errno_location()` over a global,
  and that is what shipped. Threads (2026-08-26) made the global wrong
  rather than merely unfashionable, so it is `__thread` now and
  `__errno_location()` returns this thread's.
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
certification, no locales, no `wchar`, no `select`/`poll`, and no
shared-file `mmap`.

**`pthread.h` WAS ON THIS LIST AND ITS REASON EXPIRED**, the same way
`signal.h`'s did below: there were no threads to build it on. There are
now, so `<pthread.h>` exists -- create/join/detach/self/exit, attributes,
mutexes, condition variables and `pthread_once`. What it deliberately
does NOT have is listed in the header itself and is about the kernel
underneath rather than about the library: no cancellation, no
thread-specific data keys (`__thread` is the answer), no barriers or
rwlocks, and a mutex that spins and yields because there is no futex.

**`signal.h` WAS ON THIS LIST AND ITS REASON EXPIRED.** It said "that is
`docs/signals-design.md`'s, and a libc that ships a `signal()` which
cannot deliver anything is worse than one that does not declare it" --
correct when written, and signals were built afterwards. The condition
was "cannot deliver", not "signals are out of scope", so the header
followed the capability the moment there was one (Stage 9 below). The
reasoning is worth keeping precisely because it is the rule that let the
exclusion be lifted without re-litigating anything: **declare what can
be honoured, and nothing else.** Two things in Stage 8 are still refused
under it -- `sigprocmask()` and a non-empty `sa_mask`.

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

`strtol`/`strtoul`/`atoi`/`atol` (and the `long long` family --
`strtoll`/`strtoull`/`atoll`/`llabs`, added later, since C requires
them and their absence was a LINK error for any caller), `realloc`,
`qsort`/`bsearch`, `abs`/`labs`, `rand`/`srand`, `<ctype.h>`, `<assert.h>`, `<setjmp.h>`,
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

**`<unistd.h>` omits rather than stubs.** No `select`, no `poll` (and,
until 2026-09-11, no `fork` or `exec` -- both exist now, see
`docs/fork-design.md`). A `fork()` that always failed would be worse than a link
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

### Stage 5 -- time -- DONE

`time_t`, `struct tm`, `gmtime`/`localtime` (+ `_r`), `mktime`,
`strftime`, `asctime`/`ctime`, `difftime`.

**THE CALENDAR ARITHMETIC WAS EXTRACTED, NOT REIMPLEMENTED.** It lived
inside `kernel/lib/tz.c`, which also owns the `/etc/timezones` database,
the DST rules and the persisted city choice -- so it reaches for `fs.h`,
`klog.h` and `etc_config.h` and cannot be compiled into ring 3. It is
`kernel/lib/caltime.c` now: freestanding, shared into `libc.a`, and
`tz.c` calls it. The alternative was a second copy of Hinnant's
`days_from_civil` in userland, which is the duplication the whole
shared-source rule exists to prevent.

**`time()` IS UTC AND `localtime()` IS A REAL CONVERSION** -- since
2026-09-11, when the timezone left the kernel. `userland/libc/tz.c`
reads `/etc/timezones`, applies the selected city's offset and its DST
rule, and `tzset()` re-reads the selection. `timegm()` is the UTC
counterpart of `mktime()`, and `tz_localize()` does the same for the
broken-down time `SYS_GETTIME` returns. That is glibc's arrangement:
the kernel knows UTC, the C library owns the zone database.

Until then `gmtime()` and `localtime()` WERE the same function and
`time()` was not UTC -- the kernel converted at the syscall boundary, so
the filesystem's epochs were local-derived and a libc that returned true
UTC would have put a silent skew between two numbers that look
comparable. This section predicted the fix would be system-wide rather
than a libc patch, and that when it landed these two would become
genuinely different. Both held.

**`clock()` IS ABSENT**, and the reason is a missing capability rather
than a decision about time: C says it reports PROCESSOR time, the kernel
does track per-process `cpu_ns`, and a process has no way to learn its
own pid -- `SYS_PROC_INFO` is indexed by table slot. A `clock()` over
wall time would answer a different question in the same units. Same
choice `<unistd.h>` makes about `fork()` and `<math.h>` about `sin()`:
absent rather than quietly wrong.

**Where the leap rules actually live, because it is not where you would
look.** `cal_is_leap()` has no caller on the `<time.h>` path at all --
the 100/400 rules are encoded inside the era arithmetic of
`cal_days_from_civil()`. Breaking `cal_is_leap()` reddens a KERNEL
KTEST (its only callers are `tz.c`'s DST rules) and changes nothing in
ring 3; dropping the `- yoe/100` term reddens five ring-3 checks. Both
were measured, and the division is correct -- but a session hunting a
calendar bug should know which function to look in.

### Stage 6 -- proof -- DONE

**cJSON 1.7.19 parses and re-serialises JSON on toy-os**, compiled from
upstream's source byte for byte (`userland/ports/cjson/`, commit
`fb16e5cf3587`, MIT). `userland/tests/cjson_test.c` is the harness and
is ours; the 3,200 lines it links against are not.

**IT DID NOT BUILD ON THE FIRST TRY, WHICH IS THE ENTIRE VALUE OF THE
STAGE.** Every other test in `userland/tests/` was written by somebody
who knew what this library supported, so none of them could find a gap:
they were written around one. cJSON was written years before this OS
existed and asks for whatever C says exists. It found exactly two
things, and 3,200 lines compiled clean apart from them:

- **`sprintf`**, which `<stdio.h>` had called deliberately absent
  because it cannot be given a bound. Same reversal `<string.h>` made
  for `strncpy` and for the same reason: while the library served only
  toy-os's own code, refusing a footgun cost nothing; for a
  port-capable one, omitting a function C requires produces a link error
  in somebody else's source file rather than a helpful message. It is
  built on the sink form, so no intermediate buffer caps it.
- **`sscanf`**, which had simply never been needed. It is NOT built on
  kfmt: the output formatter is shared with the kernel because the
  kernel formats constantly, but nothing in ring 0 has ever PARSED a
  format string, so there is no second caller to share with.
  `userland/libc/scanf.c` is the C library's alone. `scanf`/`fscanf`
  stay absent -- no caller, and `sscanf` plus `fgets` is the safe
  combination anyway.
- and **`%i`** in the formatter, C's printf alias for `%d`, which no
  code written here had ever used.

**THE ASSERTION IS A ROUND TRIP AND THE VALUES ARE CHECKED, not the
shape.** A broken `strtod` that read 3.25 as 3.0 still produces a
perfectly valid JSON document, so "it parsed" and "it printed" measure
nothing. Deleting the fractional part of `strtod` reddens exactly the
five value checks and nothing else -- which is also the proof that the
harness exercises the C LIBRARY rather than cJSON's internals.

**What this does NOT prove.** cJSON is integer- and string-heavy, uses
no environment, opens no files, and spawns nothing. So the port says
nothing about `getenv` (which cannot work -- see the open questions),
about `<dirent.h>` or `<time.h>`, or about a program that expects
`fork`. It is one real program, not a compatibility claim.

## Stage 8 -- the second port, and the bug it found

**DOOM.** ~36,000 lines against cJSON's ~3,000, and the difference is
not size but SHAPE: cJSON is integer- and string-heavy, opens no files
and spawns nothing, which Stage 6 said out loud. Doom reads a 4 MB file
with `fopen`/`fseek`/`fread`, allocates a zone heap, uses floating
point, and formats strings it then uses as KEYS. That last one is what
made it valuable.

**Five functions were missing, and all five belong in a C library:**

- **`remove()` and `rename()`**, which `<stdio.h>` had listed as
  DELIBERATELY ABSENT on the grounds that they are `sys_unlink()` and
  `sys_rename()` under another name. True, and the wrong bar -- the
  audience is code not yet written, and code not yet written calls
  `remove()`. Doom's savegame handling was the first real caller.
- **`mkdir()`**, and with it `<sys/stat.h>`. The header deliberately
  does NOT provide `stat()`: `struct stat` is mostly fields TFS3 does
  not have, and inventing zeroes for them would let ported code compile
  and then take wrong branches on `st_mode`.
- **`access()`**, where only `F_OK` can mean anything -- there are no
  permission bits to check, so `R_OK`/`W_OK`/`X_OK` are accepted and
  answered as "yes, if it exists".
- **`system()`**, which needed `/bin/tosh -c` to exist first. Before
  that flag this could only have reported "no command processor", which
  POSIX does define an answer for and which would have been honest but
  out of date -- toy-os has a shell, a spawn and a wait.

**AND THE ONE THAT WAS NOT A GAP BUT A BUG.** Doom died at startup with
`W_GetNumForName: STCFN33 not found!`. The lump is `STCFN033`;
`hu_stuff.c` builds the name with `"STCFN%.3d"`; and `kfmt.c` parsed
precision and then IGNORED IT for integer conversions. The header said
so and justified it by pointing at `%s`, where precision truncates and
truncating a value is the one thing these formatters may not do. Right
about `%s`, wrong to generalise: **precision on an integer only ever
ADDS digits**, and the version that dropped them was the one producing a
wrong string.

Two corners came with the fix, neither obvious: the `'0'` flag is
IGNORED when a precision is given (`%08.3d` of 42 is `"     042"`), and
the two paddings COUNT DIFFERENT THINGS -- the flag pads the whole field
including the sign, a precision pads the digits excluding it. The first
attempt conflated them and put one zero too few in front of every
negative number.

**This is the argument for building somebody else's program, stated as
plainly as it can be**: `kfmt.c` had tests, the tests passed, and the
tests asserted the bug. A test written by someone who knows what the
library does cannot find what the library gets wrong. `kfmt.c` is
compiled into both rings, so the kernel's own formatter carried it too.

## Stage 7 -- completeness, after the port

The port passed with two functions missing, which raised a policy
question rather than a technical one: does tolibc omit what nothing has
asked for, or does it aim to be complete? **Complete** -- see the note
at the top of this file. What that decision added:

- **`scanf`/`fscanf`/`vfscanf`**, over the same scanner `sscanf` uses.
  A string and a stream differ only in where the next character comes
  from, so `struct src` is two function pointers and everything else is
  shared -- the alternative is two copies of the conversion table.
  **The stream side needed DEEPER PUSHBACK**: deciding that `0x` does
  not begin a number means putting several characters back, and C
  guarantees only one. `FILE` carries eight now, which is a permitted
  extension and cheaper than the private input buffer a scanner would
  otherwise need in front of the stream's own.
- **`clock()`, and `SYS_GETPID` under it.** It was absent because C
  says PROCESSOR time and a process could not find its own row --
  `SYS_PROC_INFO` is indexed by table slot. That is a missing
  capability rather than a design decision, so the capability was added:
  three edits, and `clock()` reports real `cpu_ns`.
- **The transcendentals** -- `sin`, `cos`, `tan`, the inverse trig,
  `exp`, `log`, `pow`, the hyperbolics, `hypot`, `cbrt`, `modf`
  (`userland/libc/math_trig.c`). Published minimax coefficients
  (fdlibm's, which glibc and musl descend from) rather than Taylor
  terms, and NOT the x87 instructions -- those work on a stack this
  userland does not otherwise touch, reduce against a 66-bit pi that
  Intel documents as lossy for large arguments, and are microcoded and
  slow, which is why every real libm stopped using them.

**THE ARGUMENT REDUCTION IS WHERE THE ACCURACY LIVES, and the test
found it.** `sin(1e6)` was wrong in the tenth digit with a two-term
pi/2, because multiplying the quadrant count by a plain rounded pi/2
rounds the product and the error scales with the count. The fix is
fdlibm's: split pi/2 into three doubles whose first has its low 33 bits
ZERO, so `n * PIO2_1` is exact for `|n| < 2^20`. Full accuracy now holds
to |x| ~ 1.6e6 and degrades past it; going further needs Payne-Hanek
reduction against a multi-hundred-bit pi, which is more machinery than
the whole file and buys correctness for arguments no program has a
physical reason to pass. The test asserts to 1e6 and stops there
deliberately.

**AND THE TEST WAS WRONG TWICE BEFORE IT WAS RIGHT**, which is the more
transferable lesson. `cosh(x)^2 - sinh(x)^2 == 1` reported errors up to
3e-8 against a `cosh` and `sinh` that were individually correct to
1e-15: for large x both are about `e^x/2`, so the subtraction cancels
almost every digit and the check measures the subtraction. Rewriting it
as `(cosh-sinh)(cosh+sinh)` cancels just as badly. The well-conditioned
form is `cosh + sinh == exp`, which adds instead of subtracting and pins
both against a third function. **An identity is only a test where it is
well conditioned** -- otherwise it measures floating point, not the
library.

Every expected value in `libm_test.c` was generated by the host's
Python, not by running toy-os and writing down what it said.

## Stage 8 -- the environment

`getenv`/`setenv`/`unsetenv`/`putenv`/`clearenv`, `environ`, and a
child that inherits.

**THE FORK THIS FILE RECORDED WAS A FALSE ONE.** It asked whether a
child should inherit the parent's environment automatically or be handed
one explicitly. Unix does not choose: `execve()` is the primitive and
takes `envp` EXPLICITLY, the kernel stores and inherits nothing, and
`execv()` -- no `e` -- is the C LIBRARY function that passes the global
`environ` for you. Inheritance is a library convention over an explicit
ABI. `posix_spawn` is the same shape; Windows is the outlier, where
`CreateProcess` takes an environment and NULL means "inherit".

toy-os copies the Unix split exactly, because its process model was
already `posix_spawn`-shaped:

- **`SYS_SPAWN` carries the environment explicitly** and the kernel
  keeps none of it. It outgrew three argument registers doing so, and
  became a `struct spawn_msg` rather than a second syscall number --
  the shape `SYS_SETTING` and `SYS_WIN_REQUEST` already use. One spawn,
  one shape.
- **`environ` lives in libsys**, not tolibc, because crt0 is libsys and
  argc/argv/envp arrive together -- the startup vector is one thing and
  one layer owns it. That is also what let `sys_spawn()` pass `environ`
  automatically, so tosh, init and the WM inherit with no change at all.
- **`sys_spawn()` is `execv` and `sys_spawn_env()` is `execve`.**
- **init seeds `PATH=/bin` and `HOME=/`**, and being pid 1 is what makes
  that the whole system's environment. Deliberately small: two things
  that are TRUE here, rather than a list copied from a Unix that has
  daemons and terminals to describe. `TERM` is absent because any value
  would be a lie a program then acts on.

**The environment blob is a single string, not a `char **`.** `"K=V\0K=V\0\0"`,
so the kernel copies one validated run of bytes instead of walking a
pointer array in user memory and validating each entry -- the same
reasoning that already makes `args` one string. Oversized is REFUSED
(`E2BIG`), never truncated: a child missing half its variables is worse
than a child that failed to start.

**A PROGRAM STARTED BY THE RING-0 SHELL'S `run` HAS NO ENVIRONMENT, and
that is correct rather than a gap.** Inheritance is a library
convention, so a process gets one only if its parent had one to pass --
and the kernel shell is not a ring-3 process. It disappears when the
shell does.

**Where the ownership bug would have been.** The array crt0 hands over
points into the INITIAL STACK, which the C library did not allocate and
must never free. The first `setenv` moves the whole thing to the heap
and sets a flag; without that distinction the second `setenv` frees
stack addresses. glibc carries the same flag for the same reason. The
test catches it in the CHILD rather than the parent, because only an
inherited environment has entries to lose.

## Stage 9 -- the POSIX half, over capabilities that arrived later

The library's ISO C surface was close to complete and its POSIX surface
was almost empty -- not because those functions were hard, but because
`tolibc` was written before the kernel had signals, ptys, termios,
process groups or a waitpid that could report a stop. **The library
lagged the kernel, and the effect was that a program ported from Linux
failed at `#include <signal.h>` rather than at a missing feature.**

**Everything here sits on a syscall that already existed.** No new
syscall was added. What is new is the TRANSLATION -- POSIX's structures
converted to the kernel's, a wait status decoded by macro instead of by
hand -- and that translation is what
`userland/tests/posix_test.c` tests. It deliberately does not re-test
the syscalls; they have their own tests, and a bug in a conversion
looks exactly like the call underneath working, which it is.

- **`<signal.h>`.** `signal()`, `sigaction()`, `raise()`, `kill()`,
  `strsignal()`, `sig_atomic_t`, and the five `sigset_t` operations.
  **`sigprocmask()` is absent and a non-empty `sa_mask` is REFUSED with
  `EINVAL`** -- there is no syscall to block a signal outside its own
  handler, and a call that returned success having quietly not done it
  would leave a caller believing a critical section is protected. That
  is this file's own rule ("declare what can be honoured") applied
  inside a function rather than to a header, and it is the same instinct
  as `kernel/lib`'s parsers rejecting rather than guessing.
- **The kernel's `struct sigaction` became `struct k_sigaction`.**
  POSIX's has different field names and carries `sa_mask`; the two
  cannot be one structure. **glibc makes this exact split** and calls
  its kernel-facing one `struct kernel_sigaction`. The rename touched
  twelve files and nothing else, and `SIG_IS_HANDLER` had to stop
  comparing against `SIG_IGN` -- `<signal.h>` redefines the sentinels as
  function POINTERS, as C requires, and a pointer on the right of that
  comparison is a constraint violation. It compares against the literal
  1 now, which is `SIG_IGN`'s value on the line above it.
- **`<sys/wait.h>`.** The macros are the point. `SYS_WAITPID` already
  reported all three outcomes, encoded as three disjoint ranges (a plain
  code, `SIGNAL_EXIT_BASE + sig`, `SIGNAL_STOP_BASE + sig`), and every
  caller decoded that by hand against those constants -- a rule copied
  into each program rather than stated once. **The encoding is not
  Linux's bit-packing and does not need to be**: portable code never
  looks, which is exactly why POSIX specifies the macros and not the
  layout. `WIFCONTINUED` is absent because nothing can report it --
  `SIGCONT` acts at send time and never enters the pending set, so there
  is no moment at which a waiter could observe it.
- **`waitpid()`'s options PICK AN ENTRY POINT** rather than being passed
  through: all four combinations already existed as separate functions
  with separate blocking contracts. The one real translation is
  `SYS_RETRY` becoming a return of 0, since POSIX's `WNOHANG` contract
  is that nothing to report is 0 and a pass-through would read as an
  error to every caller.
- **`<termios.h>`.** A separate `struct termios` rather than a typedef,
  because `t.c_lflag &= ~(ICANON | ECHO)` is the universal idiom and it
  cannot compile against a struct whose field is called `lflag`.
  `c_iflag`/`c_oflag`/`c_cflag` are declared, stored and ignored -- and
  that is honest rather than sloppy: **there is no flow control here to
  disable and no character size to choose**, so ignoring them is not
  ignoring a request the terminal could have honoured. **`VMIN`/`VTIME`
  are deliberately NOT defined**, because a non-canonical read here is
  exactly `VMIN=1/VTIME=0` and there is no machinery for any other pair
  -- leaving them undefined turns a polling read into a compile error
  naming the line, instead of a program that hangs. `tcsetattr()` MASKS
  `c_lflag` to the three bits the line discipline implements, so this
  header's inert names cannot be stored in a flag space a future
  `TTY_*` bit will want.
- **`<fcntl.h>`.** The names map onto `SYS_O_*`; `O_RDONLY` is 0, which
  is why the kernel never needed a name for it. `open()` takes and
  discards the variadic mode argument, so `open(p, O_CREAT|O_WRONLY, 0644)`
  compiles unchanged against a filesystem with no permission bits.
  **`fcntl()` itself is absent**: its two common uses have named calls
  that cannot be got wrong (`dup2`, `set_nonblock`) and the rest rests
  on machinery this kernel does not have.
- **`getopt()`, and it earns its place by ending a real inconsistency.**
  Every `/bin` program parsed its own arguments, and they disagreed --
  some accepted `-la`, some only `-l -a`. **POSIX behaviour, not
  glibc's**: parsing STOPS at the first operand rather than permuting
  `argv`, which is what BSD does unconditionally and what
  `POSIXLY_CORRECT` selects on Linux. `ls foo -l` meaning different
  things on different systems is the cost of the convenient version.
- **`<sys/types.h>`, `<strings.h>`.** Type names in one place, guarded
  so `<unistd.h>`'s existing `ssize_t`/`off_t` keep working whichever
  header arrives first; `strncasecmp`, `bzero`, `bcopy`. `strcasecmp` is
  NOT duplicated -- `<string.h>` already has it over `kernel/lib`'s
  `k_strcasecmp`, and a second implementation is what the shared-source
  rule exists to prevent.
- **`<unistd.h>` gained** `getpid`, `pipe`, `setpgid`/`getpgid`/`getpgrp`,
  `tcgetpgrp`/`tcsetpgrp` (POSIX puts those here, not in `<termios.h>`),
  `sleep`/`usleep`, `_exit` and `set_nonblock`. `fork()`, `execv`/
  `execve`/`execvp`, `getppid` and `<spawn.h>`'s `posix_spawn` arrived
  on 2026-09-11 (`docs/fork-design.md`); spawn is still what a program
  written for toy-os should use.

**WHAT THE TEST FOUND, and it is the usual shape.** Its first termios
check asserted that a terminal starts in `TTY_LFLAG_DEFAULT` -- true of
a terminal at creation, and false for anything a shell spawned, because
every shell here turns `ICANON` and `ECHO` off at startup. It failed
against a correct build. What replaced it tests the conversion in BOTH
directions (set the bits on, read back, clear them, read back), which is
strictly more than the original asserted: a `tcsetattr` that masked
everything to zero would have passed the clearing half alone.

## Open questions

- ~~**The environment.**~~ BUILT -- see Stage 8 above.
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

## printf's conversion coverage, audited (2026-08-24)

`%X` was missing, and the way it was found is the reason this section
exists: `/bin/font` printed `U+%04X slot %d` and got a codepoint where
the slot number belonged. An unrecognised conversion in `kfmt.c` prints
its letters literally and **consumes no argument**, so a gap does not
produce a wrong value -- it shifts every later argument in the call, and
the symptom appears somewhere unrelated. That is the same failure the
`%.3d` precision gap produced when Doom asked its WAD for `STCFN33`.

A FIFTH turned up the same way the second did -- by looking at a screen
and seeing the letters. `edit`'s line-number gutter writes `"%*d "`,
because the column width comes from the file's line count and cannot be
a literal; `*` was unsupported, so the gutter printed `%*d` down the
left margin and ate the width as if it were the value. Both `%*d` and
`%.*s` work now, including C's rules that a negative `*` width means
left-justify and a negative `*` precision means no precision at all.

Reading the switch after fixing `%X` found four more of the same class,
none of them yet in anyone's way: `%p`, `%o`, the `+`, space and `#`
flags, and the `h`/`hh` length modifiers. All are implemented now, with
`h`/`hh` accepted and ignored (promotion has already widened the
argument) but **consumed**, which is the half that matters.

What is still deliberately absent: `%n`, the one conversion that writes
through a caller-supplied pointer and a security footgun everywhere it
exists; and the `'` and `a`/`A` conversions, which have no caller.
Floating point lives behind `k_fmt_float()` because `va_arg(ap, double)`
alone emits SSE in a kernel built `-mno-sse`.

**The guard is `kernel/include/api/kfmt_cases.h`** -- a table of
(format, argument, expected output) cases run as a KTEST and as
`/tests/kfmt_test`, so both compilations of the formatter assert the
same thing. It is exhaustive over conversions and flags rather than a
selection of interesting ones, precisely because "does nothing" and "is
not parsed" are indistinguishable from the output of the conversion
itself -- only the argument AFTER it moves. Every case for a conversion
that could be missing therefore pins a following `%d` as well.
