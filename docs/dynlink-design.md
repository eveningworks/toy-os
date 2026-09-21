# Dynamic linking for toy-os

A staged plan, in the shape `docs/libc-design.md` and
`docs/signals-design.md` used. It answers the milestone
`docs/roadmap.md` has carried since Phase 4 -- **what would it take to
have shared libraries here, and is it worth it?**

**Status: Stages 0-4 are BUILT (0-3 on 2026-08-28, 4 on 2026-09-21);
what remains is Stage 5, the measured case for lazy binding.**
`dlopen`/`dlsym`/`dlclose`/`dlerror` work, and the open question below
-- "is the plugin case actually wanted?" -- is ANSWERED: `/bin/snddrv`
loads a sound driver per chip out of `/lib/snd/*.so`, so a new sound
card is a file dropped in a directory rather than a rebuilt binary. The C library
is and stays tolibc -- porting musl was sized and declined the day
Stage 3 landed (`docs/decisions.md`, "tolibc stays").
**There is a second shared library now**: `/lib/libhash.so`
(`userland/dynlib/uhash.c`), the first one here that exists to be USED
rather than to prove the loader works, linked by `/bin/sum` and
`/tests/hash_test`. It exercises a path Stage 3 did not: a library with
its OWN `DT_NEEDED` on `libc.so`, which the loader already handled.
`docs/conventions/build.md` has how to add another.
Stage 3 (2026-08-28): tolibc ships as `/lib/libc.so` and EVERY `/bin`
and GUI program links it (init, toywm and `/tests` stay static --
`docs/decisions.md`'s "userland is dynamically linked" entry has the
whole account, including the three bugs the flip surfaced and the
kernel image cache that makes frame sharing real).
Stage 2 (2026-08-28): `/lib/ld-toy.so` exists and works --
`userland/ldso/`, ~400 lines, freestanding; `/tests/dyn_test` proves
every relocation class including a library calling back into the
executable. One deliberate deviation from the plan below: the loader
is a fixed-base ET_EXEC, not ET_DYN, so the kernel never learned
ET_DYN at all -- `docs/decisions.md`'s loader entry has the reasoning
(it deletes the rtld self-relocation bootstrap).
Stage 1 (2026-08-28): the whole userland compiles `-fpie
-mcmodel=small` AT THE SAME BASE -- the measurement went the good way:
PIE code is RIP-relative, so the 2 GiB constraint is on the image's
span, not its placement, and `uaddr.h`'s map did not move. The one
casualty was the *ABS* TLS geometry symbols, which are data now
(link.ld QUADs). Stage 0 (`mmap`) -- 2026-08-28,
`kernel/mm/mmap.c`, file-backed and demand-paged, with `/bin/pmap` over
QUERY_PROCMAP; the design calls it forced are in `docs/decisions.md`
(the kernel file's mmap entry). The name and home are settled: the
library is `/lib/libc.so`, the loader `/lib/ld-toy.so`. Each stage
below ships on its own and is verifiable on its own.

**The precondition cleared.** This milestone was deliberately placed
after "a real C library", because a shared libc is the main reason to
want dynamic linking at all. tolibc is built (`docs/libc-design.md`),
so the argument can now be had on its merits.

## The honest case against, first

Because it is strong, and a plan that does not state it is selling
something.

- **The memory saving here is near zero.** The payoff normally quoted --
  one copy of libc instead of one per binary -- is worth having when a
  system runs hundreds of processes. toy-os runs about twenty-five small
  ones, `--gc-sections` already strips each to what it calls, and
  `libc.a` is 166 KB of which any given program links a fraction.
- **Static linking is a respectable modern choice, not a legacy one.**
  musl exists partly to make static linking pleasant, Go ships static by
  default, and every unikernel is static by construction. "Real systems
  do it" is not an argument here, because plenty of real systems do the
  opposite deliberately.
- **It is the largest single piece of machinery this project would take
  on**: a second ELF format, a loader that runs in ring 3 before `main`,
  relocation processing, symbol resolution, and page sharing across
  address spaces. Each of those is a place for a bug that presents as
  "the program crashed somewhere in libc".

**So what IS the case for it?** Two things, and neither is memory:

1. **Plugins** -- loading code that did not exist when the program was
   linked. There is no other way to do it, and it is what a window
   manager, a shell, or a driver framework eventually wants.
2. **Shipping something big.** The roadmap names Doom. A large ported
   program that expects to `dlopen` its own modules, or simply expects a
   dynamic toolchain, is much easier to accept than to fight.

If neither of those is wanted, the correct decision is **not to build
this**, and to record that. This document exists so the choice is made
with the costs visible rather than by drift.

## What exists today, measured

Checked against the tree, not assumed.

- **Every userland binary is `ET_EXEC` at a fixed address** (compiled
  PIC since Stage 1, still linked at `0x8000000000`). `elf_load()`
  handles `PT_LOAD` and nothing else -- no `PT_DYNAMIC`, no
  interpreter, no relocations.
- ~~The build explicitly disables PIC.~~ Stage 1 landed 2026-08-28:
  `-fpie -mcmodel=small`, same base, whole suite green (preflight and
  all of `gui_regress`).
- **THE PROJECT ALREADY DOES RELOCATION, for a different reason.** The
  KERNEL relocates itself at boot: `tools/genrelocs.py` extracts every
  absolute reference from a `--emit-relocs` link and
  `kernel/arch/x86_64/reloc.c` patches them for KASLR. That is the same
  arithmetic a dynamic loader does, already written, already tested, and
  proof the concept is understood here.
- ~~There is no `mmap`.~~ Stage 0 landed 2026-08-28. What is still
  true: `free()` does not yet return memory -- the allocator
  (`heap_core.c`, shared with ring 0) still draws from sbrk alone, and
  moving it onto mmap is its own change with its own measurement.
- **Frames CAN already be shared between address spaces.**
  `vmm_map_user_borrowed()` maps a frame a process does not own -- which
  is exactly what several processes sharing one copy of libc needs, and
  what the compositor already relies on.
- **`SYS_SPAWN` places argv and envp on the initial stack** and jumps
  straight to the ELF entry point. There is no step where a loader could
  run first.

## What real systems do

- **Linux**: the kernel maps the executable, sees `PT_INTERP`, and maps
  `ld.so` too -- then jumps to `ld.so`, NOT to the program. The dynamic
  linker is an ordinary userspace program that finishes the job and then
  transfers control. The kernel knows almost nothing about dynamic
  linking, which is the property worth copying: it keeps the complexity
  in ring 3 where a bug is a crashed process.
- **Windows**: `ntdll.dll` is mapped into every process and the loader
  lives there; imports are resolved through an Import Address Table that
  the loader patches. Same shape, different vocabulary.
- **macOS `dyld`**: same again, plus a shared cache -- one pre-linked
  image of all system libraries, mapped into every process. Worth
  knowing about because it is the answer to "relocation at every start
  is slow", and it is far beyond anything needed here.
- **Lazy binding (PLT/GOT)** is universal and is an OPTIMISATION: the
  first call to a function goes through a stub that resolves it and
  patches the table. Eager binding at load time is simpler, correct, and
  the right first cut -- glibc's `LD_BIND_NOW` does exactly that.

**Where toy-os should differ**: no symbol versioning, no `LD_PRELOAD`,
no shared cache, no lazy binding in the first version. Those solve
problems of scale and compatibility that this system does not have.

## Staging

### Stage 0 -- `mmap`, which is not really this milestone

`mmap`/`munmap` over the existing demand-paging machinery, enough to map
a file's pages into an address space at a chosen base. **Nothing else
here can start without it**, and it pays for itself immediately
elsewhere: `free()` gets a way to return memory, and `docs/libc-design.md`
loses its standing caveat.

Verifiable alone: a ring-3 test that maps a file, reads it through the
mapping, and unmaps it -- with `meminfo audit` clean afterwards, since
mapping a file is exactly where a frame's ownership gets miscounted.

### Stage 1 -- position-independent userland -- BUILT 2026-08-28

Turn on PIC and settle its interaction with `-mcmodel=large`. The
answer, measured: `-fpie -mcmodel=small` at the SAME base -- PIE code
is RIP-relative, so the model's 2 GiB constraint is on the image's
span, not its placement, and the address map did not move. Executables
are `-fpie` (library objects will be `-fpic` when Stage 3 builds them
-- fpie code may not enter a shared object). The only casualty was the
*ABS* TLS geometry symbols, unreachable RIP-relatively; they became
data (`link.ld`'s `__rt_tlsdesc` QUADs), which also retired tls.c's
`linker_value()` laundering.

Verifiable alone: the whole existing userland builds and every test
still passes, with nothing dynamic yet. **That is the point of doing it
as its own stage** -- if PIC breaks something, it is much easier to see
before a loader exists than after.

### Stage 2 -- `ET_DYN` and a loader that runs first -- BUILT 2026-08-28

Built as designed except where `docs/decisions.md`'s "fixed-base
loader" entry says otherwise: the kernel learned only PT_INTERP, the
loader is fixed-base, ET_DYN exists only in ring 3 (libraries).

Teach `elf_load()` about `PT_DYNAMIC` and `PT_INTERP`, and make the
kernel map the interpreter and enter IT rather than the program. The
kernel's part ends there -- deliberately, following Linux: everything
after is a ring-3 program.

Then `/lib/ld-toy.so`: parse `DT_*`, apply `R_X86_64_RELATIVE`,
`R_X86_64_GLOB_DAT` and `R_X86_64_JUMP_SLOT`, and jump to the real
entry. Eager binding only.

**The awkward part, named in advance**: the dynamic linker cannot use
tolibc, because tolibc is what it is about to load. It gets its own
minimal string/syscall subset -- which is why every real libc ships one
(`rtld`'s private `memcpy`), and is a duplication with a genuine reason
rather than the kind this project usually refuses.

Verifiable alone: one trivial `.so` with one function, called from one
program.

### Stage 3 -- shared libc -- BUILT 2026-08-28

Build `libc.so` and link programs against it. The payoff, such as it is.

**This is where frame sharing has to be real**: the loader must map the
same physical pages into every process rather than reading the file
again per process, or dynamic linking costs MORE memory than static did.
`vmm_map_user_borrowed()` is the mechanism; the accounting is the risk,
and `meminfo audit` is the check that already exists for it.

### Stage 3b -- shared toolkit -- BUILT 2026-09-04

`/lib/libuapp.so`, the toolkit beside the C library, linked by every
dynamic program. The case-against above said the memory saving is near
zero for libc, and it was; the toolkit measured differently -- see
`docs/decisions.md`, "The toolkit is a shared library".

### Stage 4 -- `dlopen` -- DONE 2026-09-21

`dlopen`/`dlsym`/`dlclose`/`dlerror`, which is the plugin case and the
reason worth doing any of this. It landed as predicted: nothing else in
the stack changed and the loader grew entry points that run after
startup instead of before it. Three things were not obvious in advance.

**THE CALL GOES THE OTHER WAY ROUND.** `/lib/ld-toy.so` is a fixed-base
`ET_EXEC` with no `.dynsym`, so nothing can resolve a symbol OUT of it.
But it already looks symbols UP in what it loaded -- that is how the
ABI stamp is read -- so libc DEFINES a vector (`abi/ldso_api.h`) and
the loader FILLS IT IN before jumping to the entry point. `dlopen()` in
libc is then one indirect call. A static program never runs the loader,
so the vector stays zeroed and `dlopen()` says "no dynamic loader"
rather than jumping through null.

**A FAILURE HAD TO STOP BEING FATAL.** Every error path in the loader
called `die()`, which is right at startup -- there is no program
without its libraries -- and wrong inside `dlopen`, whose caller is
running and expects a NULL. The load path returns a status now, and
`resolve()` records an undefined symbol instead of killing the process
when a `dlopen` is in progress.

**NOTHING IS UNLOADED.** `dlclose()` returns 0 and keeps the object
mapped. Unmapping means tracking each object's segments and
refcounting what its `DT_NEEDED` pulled in, for no caller that wants
it; `<dlfcn.h>` says so rather than implying otherwise.

`/lib/libplug.so` is the proof, deliberately linked by NOTHING --
`libhello.so` is a `DT_NEEDED` of its test, so `dlopen`ing that would
only prove the already-loaded path works. `/tests/dyn_test` checks that
the plugin's own `DT_NEEDED` on `libc.so` resolved, which is the half a
bare "load one file" would miss.

### Stage 5 -- lazy binding, only if it is measured to matter

PLT stubs and `_dl_runtime_resolve`. **Do not build this first.** It is
an optimisation over eager binding, it is the fiddliest assembly in the
whole plan, and whether it matters here is a measurement nobody has
taken -- a program linking against one library with a few hundred
symbols resolves them in microseconds.

## Open questions

- ~~**Is the plugin case actually wanted?**~~ ANSWERED YES, 2026-09-21,
  by a real caller rather than an argument: `/bin/snddrv` loads one
  `.so` per sound chip from `/lib/snd/`, so adding a card does not
  rebuild the host. `docs/umdf-design.md` is where that lives.
- ~~**Where do `.so` files live?**~~ `/lib`, and it is in
  `docs/filesystem-layout.md` now. Plugins go one level down in
  `/lib/snd/`, which is the same deliberate edit.
- **Does the WM's client protocol survive a relocated address space?**
  Window buffers are mapped at computed addresses (`uaddr.h`'s
  per-window stride); PIC does not change that, but Stage 1 moving the
  userland base might.
