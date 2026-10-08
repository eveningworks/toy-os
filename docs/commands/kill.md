# kill

**a `/bin` program.**

**Category:** Processes and programs

## Synopsis

    kill [-SIGNAL] <pid|-pgid> [pid...]    (`ps` for pids)

## Description

`/bin/kill` sends a **signal** to a process, or to a whole process
group. With no signal named it sends `TERM`, as every Unix does.

The signal may be given by name or by number, with or without a `SIG`
prefix and in any case: `-TERM`, `-SIGTERM`, `-term` and `-15` are the
same request. The signals this kernel has are `HUP` (1), `INT` (2),
`QUIT` (3), `ILL` (4), `ABRT` (6), `FPE` (8), `KILL` (9), `USR1` (10),
`SEGV` (11), `USR2` (12), `PIPE` (13), `TERM` (15), `CHLD` (17), `CONT`
(18), `STOP` (19), `TSTP` (20), `TTIN` (21), `TTOU` (22) and `WINCH`
(28) — POSIX's numbers, so nothing here means something different from
everywhere else; `kernel/include/abi/signal_abi.h` is the list itself.

**A negative target is a process GROUP**, POSIX's spelling: `kill -TERM
-4` signals every live member of group 4. Pids are 1-based, so a
negative number cannot be mistaken for one. `ps` prints a PGID column.

It **names the process before signalling it** — "sent SIGTERM to pid 4"
is only useful if it says which process that was, and the caller's
mental model of pid 4 is what needs confirming while there is still time
to check.

## What it does NOT promise

**A signal other than `KILL` is PENDING when this program exits.** The
kernel acts on it when the target next returns to ring 3 — its next
syscall, or the next timer tick that finds it running — so a process
that is doing anything at all dies within a few milliseconds, and one
wedged inside a kernel path may not die at all.

`-KILL` is the exception, and that is the whole reason it exists: it
terminates the target immediately and cannot be ignored. It is the last
resort rather than the first, because a process killed that way gets no
chance to finish what it was doing.

**init discards any signal it has no handler for**, `KILL` included —
Linux's `SIGNAL_UNKILLABLE`, and for its reason: a machine whose pid 1
died has nothing left to reap orphans or supervise anything. It still
*catches* what it has asked for, which is how `service` reaches it with
`HUP`. `kill 1` therefore succeeds and does nothing; the kernel log says
so.

**A process may ignore a signal**, and a shell ignores `INT` on purpose
— so `kill -INT` aimed at `/bin/tosh` does nothing, by design. `KILL`,
`QUIT` and `STOP` cannot be ignored, so there is always something that
works.

## Stopping and continuing

`-STOP` and `-TSTP` **suspend** a process: it keeps its memory, its
descriptors and its place in the process tree, and the scheduler simply
stops choosing it. `ps` reports it as `stopped`, and its CPU column
stops advancing — which is the way to see that it really is suspended
rather than merely idle. `-CONT` resumes it.

`STOP` cannot be ignored and `TSTP` can, the same split Unix makes
between the command and the `Ctrl-Z` key.

**A stop takes effect immediately**, unlike every other signal here,
because suspending a process only changes whether the scheduler picks
it — so it works even on one wedged inside a kernel path. The flip side
is stated below.

**And a stopped process cannot be `TERM`ed.** The `TERM` is set pending
and waits, because the kernel acts on a pending signal only when the
target returns to ring 3 and a suspended process never gets there. It
dies the moment somebody `-CONT`s it. `-KILL` works regardless — that
is what it is for, here as on Linux.

## Exit status

0 if every target was signalled, 1 if any was not — an unknown pid, an
empty group, a target that is not a number, or a signal name this kernel
does not have. Each failure says which.

A **signalled process reports `128 + the signal`** as its own exit code,
which is what a shell prints: 130 for a Ctrl-C, 143 for a `TERM`, 137
for a `KILL`. That is what distinguishes a signalled death from an
ordinary non-zero exit.

## See also

`service` for starting and stopping the things init supervises, which
is `kill -HUP 1` with a request behind it. `ps` for pids and groups. `Ctrl-C` at a shell prompt is the same
mechanism with no typing: it sends `INT` to the console's foreground
group — see `docs/signals-design.md`. `Ctrl-Z` is `-TSTP` the same way,
and `jobs`/`fg`/`bg` in `/bin/tosh` are `-CONT` with bookkeeping.
