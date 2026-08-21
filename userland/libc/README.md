# tolibc -- toy-os's C library

**The name is formed the way `tosh` was**: toy-os + `sh` gave `tosh`,
so toy-os + `libc` gives `tolibc`. It sits beside **Toykit**
(`userland/ui/`, the GUI toolkit) as the second thing a ring-3 program
links, and beneath both of them is **libsys** (`userland/rt/`), the
typed syscall layer.

**The archive is still `libc.a`, not `libtolibc.a`**, and deliberately:
a C library's archive has one conventional name, a linker expects to
find it under that name, and a ported build system that says `-lc`
should not have to know what this one is called. The NAME is for people;
the FILENAME is for tools.

## What is here

| File | What it holds |
|---|---|
| `stdio.c` | `FILE`, the buffering policy, `printf`, `sprintf` |
| `scanf.c` | `sscanf`/`fscanf`/`scanf` over one scanner |
| `stdlib.c` | `strtol`/`strtod`, `realloc`, `qsort`, `exit`/`atexit` |
| `string.c` | the half of `<string.h>` the toolkit has no equivalent of |
| `time.c` | `struct tm`, `mktime`, `strftime`, `clock` |
| `math.c` | the EXACT functions -- bit reasoning, no approximation |
| `math_trig.c` | the APPROXIMATE ones -- polynomials, with measured error |
| `printf_float.c` | `%f`/`%e`/`%g`, which the kernel's build cannot have |
| `dirent.c`, `assert.c`, `cmem.c`, `heap_os.c`, `setjmp.S` | the rest |

Public headers are `userland/include/`. Four more of tolibc's files are
not here at all: `string.c`, `knum.c`, `kfmt.c`, `heap_core.c` and
`caltime.c` live in `kernel/lib/` and are **compiled twice**, so a
ring-3 `strlen`, `snprintf`, `malloc` and calendar are the same code the
kernel runs rather than a second implementation.

## The rule about what goes in

**tolibc aims to be COMPLETE, not minimal.** If C specifies a function,
or POSIX specifies one that ported code reaches for constantly
(`strdup`, `strtok_r`, `isatty`), it belongs here even when nothing in
toy-os calls it yet -- because the alternative is a link error in
somebody else's source file, months from now, with no explanation
attached. That is the opposite of the bar the rest of this project holds
(`tools/`, Toykit and the kernel toolkit all require a second REAL
caller), and the difference is the audience: those serve code written
here, and this one serves code that has not been written yet.

**What is still absent is absent for a REASON, not for lack of time**,
and each says so in its header:

- **`fork`/`exec`** -- toy-os's process model is `posix_spawn`-shaped on
  purpose (`docs/init-design.md`). A `fork()` here would be a lie about
  the kernel, not a convenience.
- **`signal`/`raise`** -- `docs/signals-design.md` is designed and not
  built; a `signal()` that could never deliver anything is worse than
  its absence.
- **threads, locales, wide characters** -- listed as deliberately not
  pursued in `docs/roadmap-details.md`.
- **the `float` forms (`sinf`, `powf`) and `long double`** -- ordinary
  omissions, and the only ones on this list that are just work.

`docs/libc-design.md` is the full plan and the record of what each stage
found.

## Measuring it

`/tests/cjson_bench` puts tolibc under sustained load through cJSON --
parse and print between them hit `malloc`/`realloc` on every node,
`strtod` on every number, `sprintf` on every number written back, and
most of `<string.h>`. It is a load test of the allocator and the
formatter wearing a JSON hat.

**`spawn /tests/cjson_bench`, not `run`**: the legacy `run` loader has
no scheduler slot, so `clock()` reports "unavailable" and every CPU
figure collapses to zero. It says so rather than printing zeroes, and
the wall-clock numbers are still valid there. The report is written to
`/tmp/cjson_bench.out` as well as the console, because a spawned
program's output arrives while a harness is between commands.

**It is NOT in the gate, and must not be.** A timing number varies with
the host, and a benchmark in `preflight.sh` would make the gate flap for
reasons that have nothing to do with this OS. It is also why the numbers
are only worth reading as RATIOS -- the same build under `vm.py --kvm`
against the same build under TCG, or one commit against another on one
host. An absolute figure from an emulator measures the emulator.
