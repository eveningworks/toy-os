# Signals, a terminal, and what `Ctrl-C` actually needs

A staged plan, in the shape `docs/init-design.md` and
`docs/query-design.md` used. It answers the question `docs/roadmap.md`
has carried since Phase 1 was written -- **how does `Ctrl-C` stop a
running program?** -- and it answers it by refusing to treat that as one
question.

**Status: designed, not built.** Nothing here exists yet. Each stage
below ships on its own and is verifiable on its own; that is the point
of staging it rather than the reason it is slow.

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
not the size. Six signals is enough to make `Ctrl-C` work and to end a
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
  scheduler does not have and job control to make it useful. Stage 4.
- **Not a replacement for `SYS_KILL`'s honesty.** `kill(pid, SIGKILL)`
  must keep working when a process is wedged, ignoring dispositions
  entirely -- that is what makes Force Quit trustworthy.

## Staging

### Stage 0 -- the mechanism, default dispositions only

`pending` and `disposition` in `struct sched_process`; a delivery check
where a trapframe is restored; `SYS_KILL` gains a signal number, with
the current behaviour becoming `SIGKILL`. `/bin/kill` gains `-TERM`,
`-KILL`, `-INT`.

**Verifiable on its own:** a process killed with `SIGTERM` exits with a
distinguishable code; `SIGCHLD` is ignored and changes nothing; `kill
-KILL` still ends a wedged process. No `Ctrl-C` yet, and the docs should
say so plainly rather than implying the milestone is closer than it is.

### Stage 1 -- interruptible blocking

A signal delivered to a process blocked in `scheduler_block_current()`
wakes it, and the syscall it was in returns `-EINTR`. **Needs** the
`docs/roadmap.md` item "Interruptible syscalls -- a trap gate plus
retiring `g_next_kernel_rsp` as a single global", which is not
incidental: a signal that cannot interrupt a blocked read cannot stop a
program waiting for input, which is most of them.

### Stage 2 -- a foreground process, and `Ctrl-C`

The console learns `foreground_pid`, set by whatever spawned the
process it is waiting on. `keyboard.c` recognises the INTR character
before the key reaches the key ring and sends `SIGINT` to it.
**This is the stage where `Ctrl-C` starts working**, and it is the
first one a user can see.

`KLINE_CANCEL` stays as it is: with no foreground process, `Ctrl-C` at
a prompt should still abandon the line. The two do not conflict --
they apply in states that cannot overlap.

### Stage 3 -- handlers

`SYS_SIGACTION` to register one, a signal frame pushed onto the user
stack, and `SYS_SIGRETURN` to unwind it. This is the stage with the
genuinely hard part in it: the frame must be restored exactly, and a
handler that faults must not corrupt the interrupted state.

### Stage 4 -- process groups and job control

`setpgid`, a group as the unit of delivery, `SIGSTOP`/`SIGCONT` and a
stopped state, `fg`/`bg` in `/bin/tosh`. Also where multiple virtual
terminals become mostly bookkeeping, since a terminal is by then a
THING rather than the only thing.

## Open questions

- **Does the kernel shell get a foreground process?** It runs programs
  through `elf_run_from_fs()`, whose legacy loader has no scheduler slot
  -- so there is no pid to nominate. Either that path grows one, or
  `Ctrl-C` works only for `spawn`ed programs and `/bin/tosh`, which may
  be the honest answer given the shell is meant to move to ring 3.
- **What does `SIGSEGV` do to the fault handler's diagnostics?** Today a
  ring-3 fault prints a panic-grade report and kills the process, which
  is genuinely useful. Naming it `SIGSEGV` must not quietly lose that.
- **Where does the INTR character get recognised?** `keyboard.c` is the
  one place a key arrives, but a line discipline is the thing that
  should own it -- and the discipline does not exist yet. Putting it in
  the driver first and moving it later is probably right, and should be
  written down as temporary rather than discovered later as a layering
  mistake.
