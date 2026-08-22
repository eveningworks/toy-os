# Signals, a terminal, and what `Ctrl-C` actually needs

A staged plan, in the shape `docs/init-design.md` and
`docs/query-design.md` used. It answers the question `docs/roadmap.md`
has carried since Phase 1 was written -- **how does `Ctrl-C` stop a
running program?** -- and it answers it by refusing to treat that as one
question.

**Status: STAGES 0-2 AND 4 ARE BUILT (2026-08-22). `Ctrl-C` works, on
the physical console AND in a Terminal window; so do `Ctrl-Z`, `jobs`,
`fg`, `bg` and `&`.** The terminal half of this
document was superseded the same day by `docs/tty-design.md`, which
turned "the console" into a TERMINAL OBJECT -- so the `SCHED_CHAN_KEY`
this file names below no longer exists (a reader parks on its own
terminal) and the INTR recognition left the keyboard driver for a real
line discipline. What is written here is what was TRUE WHEN IT WAS
PLANNED, and it is kept that way on purpose: the staging was right, and
the fact that stage 2 shipped without a TTY layer is the finding.

Stage 3 (user-space handlers) is the only one still a plan -- and stage
4 landing before it is itself a finding, since this document assumed the
order. Job control needed a stopped process and a way to report one, not
a signal a program can catch. Each stage's own section below says what
actually landed and where it differs from what was planned here, because
three of them do.

**AND ONE PREMISE OF THIS DOCUMENT WAS WRONG, which is why the whole
thing was cheaper than it looked.** Stage 1 said it needed the roadmap's
"Interruptible syscalls -- a trap gate plus retiring `g_next_kernel_rsp`
as a single global". It does not. Blocking in this kernel is already
implemented by rewriting a parked process's SAVED TRAPFRAME
(`scheduler_wake()` writes its `RAX` slot and marks it READY), and
resuming it abandons the syscall's kernel C frames entirely -- the
process returns to ring 3 through `iretq`, never through the C return
path. So `-EINTR` was not a new mechanism to build; it is a wake with a
different value, and it is about six lines. The roadmap item is real and
is about something else (a syscall being PREEMPTIBLE, which none are
today, since every gate is an interrupt gate).

## Why this is not "add signals"

`docs/roadmap-details.md` already states the trap, and it is worth
repeating as the first thing in this file because it decides the
ordering of everything else:

> Signals & process control can deliver a signal, but "deliver SIGINT to
> the foreground process" has no meaning without a foreground process.
> Shell pipes & job control's job control (`fg`/`bg`) is the same
> problem wearing a different hat. Doing those first means inventing a
> partial answer twice.

So this document covers three things that are usually three milestones:
signal DELIVERY, a terminal that knows which process is in FRONT, and
the `Ctrl-C` that only exists once both do. Building the first alone
produces a `kill -TERM` that works and a `Ctrl-C` that still does
nothing, which reads as the feature being broken.

## What exists today, measured

Not assumed -- these were checked against the tree before the plan was
written, the same way `init-design.md` was, and two of them changed it.

- **`struct sched_process` already carries what delivery needs.**
  `state`, `wait_chan`, `exit_code`, `ppid`, and -- the important one --
  `kernel_rsp`, "this process's saved trapframe pointer, valid whenever
  state != SCHED_UNUSED". Redirecting a process to a handler means
  editing that frame. There is no signal state: no pending mask, no
  dispositions, no process group.
- **Blocking and waking already exist**, and landed recently:
  `scheduler_block_current(regs, chan, reason)` parks the caller on a
  CHANNEL (an address naming the object waited on) and
  `scheduler_wake(chan, value)` releases exactly those waiters. A signal
  that must interrupt a blocked process has a mechanism to build on
  rather than one to invent.
- **There is exactly ONE place a foreground reader parks.**
  `sys_do_read_console()` (`kernel/proc/syscall_fd.c`) blocks on
  `SCHED_CHAN_KEY`. Whatever "the process reading the console" means, it
  is asleep there.
- **There is exactly ONE place a key arrives.** `keyboard.c` wakes
  `SCHED_CHAN_KEY`. If `^C` is to be noticed by the system rather than
  by whoever happens to read it, that is the place.
- **`SYS_KILL` exists and is NOT a signal.** It ends a process and sets
  its exit code -- `kill -9` semantics and only those. There is no
  number to pass and no way to be told about it.
- **`Ctrl-C` today is a LINE EDITOR key**, not a signal.
  `KLINE_CANCEL` -- "abandon this line, start a fresh one"
  (`api/klineedit.h`). It works only while something is editing a line,
  which is exactly when there is no running program to interrupt. The
  gap is not that `Ctrl-C` is unhandled; it is that it is handled in the
  one state where it has nothing to do.
- **fd 0 is RAW.** No line discipline, no echo control, no cooked mode.
  `docs/roadmap.md` lists that separately and this plan needs it.

## What real systems do

**Linux and every Unix**: a signal is a small number, a per-process
PENDING BITMASK, and a per-process disposition table. Delivery happens
on the way back to user mode -- not at the moment of sending -- because
that is the only point where the kernel is holding the target's register
state and can safely redirect it. A caught signal is delivered by
pushing a frame onto the user stack, pointing the return at the handler,
and having the handler return through a `sigreturn` syscall that
restores the frame. `SIGKILL` and `SIGSTOP` cannot be caught, so there
is always something that works.

**Windows NT** deliberately has no signals. It has APCs (asynchronous
procedure calls) and structured exception handling, and its console
sends `CTRL_C_EVENT` to a process group through the console subsystem
rather than through the kernel's scheduling machinery. That is a real
alternative and it is not the one to copy here: it needs a console
server, and this OS's console is a driver.

**The part worth copying is the ordering.** Unix's `Ctrl-C` is not a
kernel feature at all -- it is a TTY feature. The terminal's line
discipline recognises the INTR character and sends `SIGINT` to the
terminal's FOREGROUND PROCESS GROUP. Every piece of that sentence is a
separate mechanism, and the reason `Ctrl-C` is hard is that it is the
last one.

**Where toy-os should differ.** POSIX signals carry decades of
compatibility baggage this OS has no reason to inherit: 31 standard
signals plus 32 realtime ones, queued delivery, `sigaltstack`,
`SA_RESTART`, `sigprocmask`, stop/continue as separate states. Copy the
SHAPE -- a mask, a disposition, delivery on return to user mode -- and
not the size. A handful of signals is enough to make `Ctrl-C` work, to
suspend and resume a job, and to end a
process politely.

## The shape

**A signal is a number 1..31 and a bit in a pending mask.** No queuing:
two `SIGINT`s before delivery are one `SIGINT`, which is what standard
Unix signals do too. `uint32_t pending` in `struct sched_process`.

**The set is small and every member earns its place:**

| | | |
|---|---|---|
| `SIGINT` | 2 | what `Ctrl-C` sends |
| `SIGKILL` | 9 | uncatchable; what `SYS_KILL` does today |
| `SIGSEGV` | 11 | what a fault already does, given a name |
| `SIGTERM` | 15 | ask politely; the default for `kill` |
| `SIGCHLD` | 17 | what init already blocks in `waitpid` for |
| `SIGQUIT` | 3 | second-best escape hatch when a handler misbehaves |

Numbers are POSIX's, not compacted to 1..6. They cost nothing, and a
number that means something different here from everywhere else is a
trap for anybody who has used a Unix.

**Delivery happens on return to ring 3, never at send time.** The sender
sets a bit and, if the target is blocked on an interruptible channel,
wakes it. The kernel checks the pending mask at exactly one place: the
point where a trapframe is about to be restored. Doing it at send time
would mean writing another process's register state from inside a
syscall the sender made, which is the shape of bug this project has
already paid for once (see `docs/decisions.md` on a compositor's
mapping being revoked underneath a live process).

**Default dispositions only, at first.** Terminate for `SIGINT`,
`SIGTERM`, `SIGQUIT`, `SIGSEGV`; ignore for `SIGCHLD`. That is a
complete, useful system with no user-space code at all -- and it is
where `Ctrl-C` starts working.

**A foreground process, not yet a process group.** The console gets one
`foreground_pid`. `Ctrl-C` sends `SIGINT` to it. Process GROUPS are what
makes `cmd | cmd | cmd` interruptible as a unit and what `fg`/`bg` move
around, and they need `setpgid`, a group id per process, and a group as
the unit of delivery -- real work, and not needed for the first useful
version. The single pid is deliberately a stepping stone and is written
down as one, so nobody later mistakes it for the design.

## What this is NOT

- **Not `sigaction`.** One disposition per signal: default, ignore, or a
  handler. No flags, no masks during handlers, no restart semantics.
- **Not realtime signals.** No queuing, no payload, no priority.
- **Not stop/continue.** `SIGSTOP`/`SIGCONT` need a stopped state the
  scheduler does not have and job control to make it useful. Stage 4 --
  which is built now; this line records the scoping as it was.
- **Not a replacement for `SYS_KILL`'s honesty.** `kill(pid, SIGKILL)`
  must keep working when a process is wedged, ignoring dispositions
  entirely -- that is what makes Force Quit trustworthy.

## Staging

### Stage 0 -- the mechanism, default dispositions only -- **BUILT**

`pending` and `ignored` in `struct sched_process`; delivery on the way
back to ring 3; `SYS_KILL` takes a signal number, with the current
behaviour becoming `SIGKILL`. `/bin/kill` takes `-TERM`, `-KILL`,
`-INT`, by name or number, with or without a `SIG` prefix.

**Two dispositions, not three, and `disposition` became a BITMASK.** The
plan said a per-signal disposition table; with only `SIG_DFL` and
`SIG_IGN` to express, a 32-byte table per process would have been 31
bytes of a slot nobody reads. It becomes a table when handlers do.

**`SYS_SIGACTION` exists and REFUSES a handler pointer** (`-EINVAL`)
rather than accepting one it would never call. `SIGKILL` and `SIGQUIT`
cannot be ignored at all.

**DELIVERY TURNED OUT TO NEED TWO PLACES, not one.** The plan said "the
point where a trapframe is about to be restored". That is the end of
`isr_dispatch()`, and on its own it makes death a race for any process
that is about to call `exit()` -- losing that race is permanent, because
the exit path zombies the slot and the pending bit becomes unreachable.
The second place is SYSCALL ENTRY, before the handler runs, which a
process always reaches. Both are load-bearing and each was confirmed by
disabling it; see `docs/decisions.md`.

**Verified:** `kernel/proc/signal_test.c` (the state machine, on
fabricated slots) and `userland/tests/signal_test.c` (a real process
really dying, spawned by the KTEST above because the legacy loader has
no slot). 12 KTESTs, 27 ring-3 checks.

### Stage 1 -- interruptible blocking -- **BUILT**

A signal raised against a process parked in `scheduler_block_current()`
writes `-EINTR` into its saved trapframe and marks it READY, so it
reaches a delivery point. `EINTR` is a real errno now
(`abi/errno.h`), deliberately distinct from `SYS_RETRY` -- libsys
RETRIES the latter in a loop, which would send a caller straight back
into the call the signal was trying to end.

**It needed no new mechanism at all** -- see the correction at the top
of this file. `-EINTR` is a wake with a different value.

**Nothing in ring 3 ever OBSERVES `-EINTR` today**, because every signal
that can interrupt a call also terminates the process. It exists as the
honest answer at the ABI, and handlers are what make it visible.

**Verified** by a child parked on an empty pipe -- chosen over one
blocked on the console, which any keystroke would have released. The
test also has to WAIT until the child is really `SCHED_BLOCKED` before
signalling: a positive control showed the first version passing with the
wake removed entirely, because the child had not been scheduled once and
was killed at its first syscall instead. The fixture never reached the
code under test.

### Stage 2 -- a foreground GROUP, and `Ctrl-C` -- **BUILT**

**A GROUP, not the single `foreground_pid` this plan proposed.** The
stepping stone was rejected before it was built, for the reason written
into it: a pipeline is several processes, and interrupting only the last
stage leaves the others running with the shell still waiting on them --
so `cat big | grep x | less` would hang on `Ctrl-C` rather than stop.
Process groups turned out to be an int per process and a field compare,
which is a smaller thing than the plan assumed. `kernel/tty.h` holds the
console's owner and its foreground group; `SYS_TCSETPGRP`/`SYS_TCGETPGRP`
move it, and only the owner may.

**Groups are set at SPAWN, not by a later `setpgid()`** -- `SYS_SPAWN`
carries a `pgid`. POSIX closes the equivalent window by having both
sides of a `fork()` call `setpgid()`; with no fork there is no second
side, so the window would be unfixable. See `docs/decisions.md`.

`KLINE_CANCEL` stays exactly as it was, and the two states cannot
overlap as predicted -- but the mechanism is the other way round from
what this plan implied. The kernel does not deliver the byte AND the
signal: with a job in front it signals the group and DISCARDS the key,
which is what a line discipline does with INTR; with no job it signals
nothing and the byte goes through. The consequence is that **the shell
prints its own `^C`**, because with a job running the editor never sees
the key. `bash` does the same, from the same place.

**Verified** by `tools/ctrlc_test.py`, through the real keyboard on a
`text` boot: a spinning job dies, a two-stage pipeline dies as a unit,
the shell survives, and at an empty prompt the line is cancelled instead.
Both halves have positive controls.

**What Ctrl-C still does NOT reach: the GUI Terminal.** It reads keys as
window events rather than from fd 0, so it owns no console and has no
foreground group. That is a roadmap item, not an oversight.

### Stage 3 -- handlers

`SYS_SIGACTION` to register one, a signal frame pushed onto the user
stack, and `SYS_SIGRETURN` to unwind it. This is the stage with the
genuinely hard part in it: the frame must be restored exactly, and a
handler that faults must not corrupt the interrupted state.

### Stage 4 -- process groups and job control -- **BUILT**

`setpgid`, a group as the unit of delivery, `SIGSTOP`/`SIGCONT` and a
stopped state, `fg`/`bg` in `/bin/tosh`. Also where multiple virtual
terminals become mostly bookkeeping, since a terminal is by then a
THING rather than the only thing.

**WHAT LANDED, and the three places it differs from the plan above.**
Process groups arrived early, with stage 2, because `Ctrl-C` needed
them; what was left here was stop/continue and the shell's half.

- **A stopped process is a FLAG beside its state, not "a stopped
  state".** The plan's phrasing assumed Linux's `TASK_STOPPED`, and that
  shape needs to wake an interruptible sleeper in order to stop it
  promptly -- a mechanism this kernel does not have. As a flag it
  composes with BLOCKED for free, which is the case that decides it: a
  process suspended mid-read must come back to that read.
  `docs/decisions/kernel.md` has the full argument.
- **Stop and continue do not go through the pending set at all**, so
  this stage did not weaken the invariant stage 1 established. They are
  applied at send time, which is also what makes `Ctrl-Z` safe from the
  keyboard IRQ.
- **It arrived BEFORE stage 3**, which this document did not anticipate.
  Job control needs a process that can be suspended and a way to report
  a suspension to a waiter; it needs nothing a program can catch. The
  ordering here was a guess about difficulty, not a dependency.

`SIGTTIN` was not in the plan and turned out to be non-optional: `&`
without it means two processes reading one keyboard, which is a race
over every keystroke. There is deliberately no `SIGTTOU` -- see
`abi/signal_abi.h`.

## Open questions -- and what the answers turned out to be

- **Does the kernel shell get a foreground process?** ANSWERED: no, and
  the honest answer was the right one. `Ctrl-C` works for `/bin/tosh`
  and anything it runs, because the console's owner is whoever reads fd
  0 and the legacy loader is not a scheduled process at all. Nothing was
  added to that path; it disappears when the ring-0 shell does.
- **What does `SIGSEGV` do to the fault handler's diagnostics?** STILL
  OPEN, and untouched: a ring-3 fault still prints its panic-grade
  report and tears the process down directly, without going near the
  signal machinery. `SIGSEGV` has a number and nothing raises it. The
  question stands exactly as written.
- **Where does the INTR character get recognised?** ANSWERED as
  predicted: in `keyboard.c`, and written down as temporary in
  `kernel/tty.h` rather than left to be discovered. It moves into a line
  discipline when there is one.
- **NEW, and it cost a leak before it was noticed:** "not the running
  process" and "not the loaded address space" are different questions.
  `switch_to_kernel()` leaves CR3 on the process it switched away from.
  See `docs/decisions.md`.
