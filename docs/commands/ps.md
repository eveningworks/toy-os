# ps

**a `/bin` program.**

**Category:** System information

## Synopsis

    ps [--tree]

## Description

One line per process: pid, ppid, pgid, state, CPU time, memory, name.
Reads `SYS_PROC_INFO`. `--tree` shows the parent/child structure
instead, which is what makes reparenting to init visible.

**PPID and PGID answer different questions that look alike.** The parent
is who STARTED a process; the group is what a SIGNAL reaches. `kill -TERM
-<pgid>` and `Ctrl-C` both act on a group, and a shell puts each job it
runs in a group of its own -- so the PGID column is how you see which
processes one keystroke would end.

It walks by SLOT and reads the pid from each record rather than
assuming slot+1 -- that assumption holds only until a slot is reused.