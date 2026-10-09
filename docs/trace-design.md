# Syscall tracing in ring 3, and what a debugger needs beside it

A staged plan, in the shape `docs/netstack-design.md` used. It answers
"should `strace`'s decoding leave the kernel, what would it take, and
how much of it does a ring-3 debugger share?" -- and it RE-ARGUES a
decision rather than starting from nothing: `docs/decisions/shell.md`,
"`strace` is a `/bin` program, and the trace goes to the tracer's
terminal".

**STAGE 1 IS BUILT (2026-10-09).** The stage markers are on the
headings.

## The finding that shapes the plan

**The 2026-08-23 decision kept the formatting in the kernel for one
concrete reason: "relaying would mean the kernel handing every line to
a ring-3 process through a channel that does not exist."** That is no
longer true. Since then the tree gained shared memory with an owner
(`SYS_SHM_GRANT`), a ring over it (`userland/lib/uchan.h`) and a wait
that consumes nothing (`SYS_WAKEWORD`); the compositor, sound and the
clipboard run on them. The netstack plan found the same thing.

**The second finding: `strace` and a debugger want DIFFERENT
mechanisms, and Linux's choice to share one is the expensive one.**
Linux's `strace` runs on `ptrace(2)`: the tracee STOPS at every syscall
entry and exit, and the tracer reads its registers and memory. That is
the debugger's mechanism, and it costs two context switches per call. A
tracer only needs to be TOLD what happened. A debugger needs the
process held still while it looks. So this plan builds the cheap one
for `strace` now, and leaves the stop for when there is a debugger to
use it -- sharing the parts that really are common: naming the target,
the decode table, and the record of a call.

## What real systems do

| system | syscall tracing | debugging |
|---|---|---|
| **Linux** | `strace` over `ptrace` stops (decoding in ring 3); `perf trace` reads the `raw_syscalls` tracepoints from a kernel RING BUFFER instead, with no stop | `ptrace`; `gdbserver` speaks the remote protocol over it |
| **FreeBSD** | **`ktrace(2)`**: the kernel appends binary records -- `KTR_SYSCALL`, `KTR_SYSRET`, `KTR_NAMEI` (a path, copied at the call), `KTR_GENIO` (I/O bytes) -- and **`kdump(1)` decodes them in ring 3**. `truss` is the ptrace-shaped alternative | `ptrace` |
| **Windows** | ETW: the kernel logger writes system-call events into per-CPU buffers; decoding is the consumer's | debug objects (`NtCreateDebugObject`, `DbgUi`) -- a stop model of its own |
| **toy-os today** | the kernel formats every line and writes it to the tracer's terminal | none (`docs/kdebug-design.md` keeps user-space debugging out of the kernel stub) |

**FreeBSD's `ktrace`/`kdump` split is the shape for `strace`**: a record
stream the traced process never waits for, path strings copied at the
moment of the call (so a later write to that buffer cannot change what
the trace says), and every word of formatting in ring 3. **Linux's and
NT's stop model is the shape for the debugger**, and nobody ships the
debugger on the record stream.

## What exists, measured (2026-10-09)

| | |
|---|---|
| the hooks | three in `syscall_dispatch()` (`kernel/proc/syscall.c`): `strace_begin()` before the handler, `strace_end()` or `strace_end_noreturn()` after |
| what is formatted in ring 0 | everything: the name, three argument kinds (`A_PATH`, `A_BUF`, `A_OFLAGS` beside int/fd/hex), the return value, 14 errno names -- `kernel/proc/strace.c`, 507 lines |
| arguments | three registers -- ALL the ABI has (RDI, RSI, RDX; `userland/rt/sys.c`): a call needing more passes a struct, and the struct's contents (`spawn_msg`, `setting_msg`) are what the trace cannot show |
| strings | copied from the tracee at entry (`vmm_copy_from_user()` with its pml4), 32 bytes before `...` |
| a call that parks | prints `= ?` and never its result -- a `read` on a pipe, `waitpid`, `SYS_WAIT_EVENT` |
| who is traced | ONE address space machine-wide (`g_traced_pml4`), named at the spawn (`SPAWN_TRACE`), followed across exec, not across fork |
| where it goes | the terminal the tracer's fd 1 names, and klog; `/bin/strace` (99 lines) spawns and waits, and prints nothing itself |
| what it cannot do | attach, follow children, filter (`-e`), count (`-c`), write to a file (`-o`) -- the decision lists them as "all of which want `ptrace`" |
| the decode data | the syscall table (`kernel/proc/syscall_table.c`) -- name, three arg kinds, DEC or HEX return -- kernel-only; ring 3 sees names only, through `kstack syscalls` |
| a debugger's raw material | NONE in ring 3: no cross-process memory read, no register access, no SIGTRAP, no stop-on-syscall. The kernel GDB stub can read a process's memory, but halts the whole machine to do it |

**Three of the "what it cannot do" items do not want `ptrace` at all.**
Filtering, counting and `-o` are things a ring-3 decoder does to a
stream it already has. Only attaching and the debugger need a stop.

## The choices

### 1. What the kernel hands over

| | |
|---|---|
| **A. A record stream (recommended)** | `ktrace`'s shape: a fixed-size record per syscall entry and exit -- pid, number, the three argument registers, the return value, plus the bytes of any path or buffer argument, copied at entry -- written into a ring the tracer owns. The traced process never stops for the tracer. |
| B. Stops | Linux `strace`'s shape: the tracee stops at entry and exit, and the tracer reads registers and memory. One mechanism for both tools, and the expensive one -- and it needs the whole debugger's machinery (stop, cross-process read, registers) before `strace` works again. |
| C. Nothing | Keep the 2026-08-23 decision: format in ring 0, extend it there. |

### 2. A full ring

The handler runs with interrupts off, so the record cannot simply wait
for space.

| | |
|---|---|
| **A. Stop the tracee on its way back to ring 3 (recommended)** | Mark it, and park it at the return edge -- one of the two places signals already deliver (`docs/decisions/kernel.md`, "Signals deliver on the way back to ring 3") -- until the tracer drains the ring and bumps its wakeword. Nothing is lost, which is what `ktrace` gets by writing synchronously. |
| B. Drop, and count | `perf`'s and ETW's choice: a LOST record says how many. Never slows the tracee, and a trace with holes in it is a worse `strace`. |

### 3. Where the decode table lives

| | |
|---|---|
| **A. Generated, compiled into both rings (recommended)** | The name and argument kinds become data in `kernel/include/abi/` that `syscall_table.c` and `/bin/strace` both read -- the "one source, two rings" rule, and `tools/gen_signames.py`'s precedent for dash. A build where they disagree cannot exist. |
| B. Asked at run time | A query provider hands out the table, so an old `/bin/strace` decodes a new kernel. Strace is shipped with its kernel here (System Update replaces both), so the case it covers does not arise. |

## The stages

Each stage has a caller of its own before the next one needs it.

### Stage 1 -- records, beside the text -- BUILT 2026-10-09

`SPAWN_TRACE` gains a ring: the tracer creates an shm object, grants it
to nobody, and names it in the spawn message; the kernel writes entry
and exit records into it and bumps the tracer's wakeword. **The text
path keeps working unchanged**, so stage 1 can be checked against it
line for line. A call that parks gets its EXIT record when it finally
returns -- Linux's `<... read resumed>`, which `= ?` cannot say today.
The handler resumes inside `syscall_dispatch()` after the wake (a real
context switch), so the value is in RAX by then and the record is
written in the tracee's own context.

**A full ring stops the tracee BEFORE the call, not after it** (choice
2A, as built): at entry, with no room for an entry and an exit record,
the dispatcher rewinds RIP over the 2-byte `int $0x80`, sleeps the
tracee a millisecond and puts RAX back, so the call is re-issued once
the tracer has drained -- the signal path's SA_RESTART rewind. Nothing
has run, so nothing is lost, and nothing parks with interrupts off.

### Stage 2 -- `/bin/strace` decodes, and the kernel's formatter goes

`/bin/strace` prints the records to its own fd 1, from the generated
table, and `strace.c`'s formatting is deleted (the arming, the hooks
and the string copy stay). **What it gains for free**: `-o FILE`, `-e
trace=open,read`, `-c` for a count per call, and errno names from
`errno.h` rather than a 14-entry switch. Its tests are the existing
`kernel/proc/strace_test.c` cases, moved to `/tests` with the decoder.

### Stage 3 -- more than one tracee

One ring per tracer rather than one global: the tracer's pid owns the
ring, records carry the pid, and **fork follows** (the open roadmap item
"`strace` extended to follow a process's children once `fork()`
exists") -- a forked child writes into its parent's ring, as `strace -f`
shows.

### Stage 4 -- the debugger's half, when there is a compiler

The roadmap item "A debugger for ring-3 programs" -- a `ptrace`-shaped
set: attach to a pid (the device claim's shape, one holder), stop it
(SIGSTOP and `SYS_WUNTRACED` already report a stopped child), read and
write its memory and registers, a breakpoint via SIGTRAP. `gdbserver`
in ring 3 speaks the protocol `kernel/debug/gdbstub.c` already speaks.
**It reuses from stages 1-3**: naming the target, the decode table (GDB's
`catch syscall`), and the return-edge stop from choice 2A. Not before
`/bin/cc` builds programs on the machine (`docs/cc-design.md`), because
until then every binary was built on a host that has GDB already.

## What does NOT move

- **The hooks and the string copy.** The kernel is the only place that
  sees every call, and the bytes of a path must be copied AT the call --
  a ring-3 tracer reading them later would race the tracee's next write,
  which is why `ktrace` copies too.
- **The arming at the spawn.** It has no race (the decision explains
  why), and a `ptrace`-style attach is stage 4's, not a replacement for
  it.

## What it buys

Not containment -- the formatter parses nothing a stranger sends; it
reads the tracee's own memory with bounds already checked. It buys
**the missing features without `ptrace`** (`-o`, `-e`, `-c`, children,
the result of a call that blocked), about 350 lines out of
ring 0, and a record format the debugger's `catch syscall` reuses.

## The case against

- **Nothing is broken, and `strace` is a diagnostic.** Nobody has asked
  for `-e` or `-c` yet; the decision said so in 2026-08.
- **A shm ring is more moving parts than a `tty_output()` call.** A
  tracer that dies leaves a ring nobody drains -- choice 2A must not
  stop the tracee for ever, so a ring whose owner has exited is
  abandoned and tracing ends.
- **Stage 1 alone fixes `= ?`** even if stage 2
  never happens -- the strongest point in favour, as stages 1-2 were
  for the netstack.

## Decided so far

Picked 2026-10-09: **1A, a record stream**, and **2A, a full ring
stops the tracee** -- before its call, by re-issue (Stage 1). Choice 3 was not put to the
maintainer and takes its recommendation, **3A, a generated table**,
unless reopened. The debugger's stage 4 is not scheduled.
