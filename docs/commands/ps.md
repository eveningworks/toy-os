# ps

**a `/bin` program.**

**Category:** System information

## Synopsis

    ps [--tree]

## Description

One line per process: pid, ppid, state, CPU time, memory, name. Reads
`SYS_PROC_INFO`. `--tree` shows the parent/child structure instead,
which is what makes reparenting to init visible.

It walks by SLOT and reads the pid from each record rather than
assuming slot+1 -- that assumption holds only until a slot is reused.