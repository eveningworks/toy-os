# ps

**a `/bin` program.**

**Category:** System information

## Synopsis

    ps [--tree] [--threads]

## Description

One line per process: pid, ppid, pgid, state, CPU time, memory, name.
Reads `SYS_PROC_INFO`. `--tree` shows the parent/child structure
instead, which is what makes reparenting to init visible.

**Threads are hidden unless you ask** (`--threads`, or `-T`), as in
every Unix `ps`: a program's threads are that program's business, and a
default listing showing six rows where a person expects one process is
worse than one that needs a flag. A thread's name prints in braces --
`{tosh}` -- because it has no name of its own: it carries its leader's,
so an unmarked listing would look like six shells rather than one shell
with five threads. Its PID column is its TID, and its PPID is the
process it belongs to, which is also what nests it under that process
in `--tree`.

**The STATE column says what a blocked process is waiting FOR** --
`block(pipe)`, `block(key)`, `block(child)`, `block(timer)`,
`block(event)`. A process listing where half the machine reads `block`
tells you nothing; those five words are the difference between "waiting
for input that is never coming" and "waiting for a child that has
already exited". Linux puts this in a separate `WCHAN` column naming the
kernel symbol being slept on; here there are five reasons and each is
meaningless outside `block`, so it goes inside the state rather than
costing a seventh column. `block(?)` means the kernel reported a reason
this binary does not know about.

**`stopped` means suspended, not idle** -- a process that has been sent
`SIGSTOP` or `SIGTSTP` (`kill -STOP`, or `Ctrl-Z` on a job). It keeps
everything it holds and the scheduler simply stops choosing it, so the
way to confirm it really is suspended is the CPU column: a stopped
process's CPU time does not advance, while a merely idle one's does when
it gets work. `stopped` is reported ahead of whatever the process was
doing underneath -- one suspended mid-read shows `stopped`, not
`block(pipe)`, because the block is no longer why it is not running.

**PPID and PGID answer different questions that look alike.** The parent
is who STARTED a process; the group is what a SIGNAL reaches. `kill -TERM
-<pgid>` and `Ctrl-C` both act on a group, and a shell puts each job it
runs in a group of its own -- so the PGID column is how you see which
processes one keystroke would end.

It walks by SLOT and reads the pid from each record rather than
assuming slot+1 -- that assumption holds only until a slot is reused.