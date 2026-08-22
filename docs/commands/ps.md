# ps

**a `/bin` program.**

**Category:** System information

## Synopsis

    ps [--tree]

## Description

One line per process: pid, ppid, pgid, state, CPU time, memory, name.
Reads `SYS_PROC_INFO`. `--tree` shows the parent/child structure
instead, which is what makes reparenting to init visible.

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

**PPID and PGID answer different questions that look alike.** The parent
is who STARTED a process; the group is what a SIGNAL reaches. `kill -TERM
-<pgid>` and `Ctrl-C` both act on a group, and a shell puts each job it
runs in a group of its own -- so the PGID column is how you see which
processes one keystroke would end.

It walks by SLOT and reads the pid from each record rather than
assuming slot+1 -- that assumption holds only until a slot is reused.