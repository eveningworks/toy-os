# crashlog

**a `/bin` program.**

**Category:** System information

## Synopsis

    crashlog [<report>]

## Description

Lists the crash reports in `/var/crash`, or prints one. A report is
written by the kernel when a ring-3 process faults and is torn down
(`kernel/proc/crash_report.c`): `<program>-<pid>.crash`, a text header
-- the program, pid, fault, registers, memory map and the tail of the
kernel log -- followed by the raw bytes of the process's stack.

A core-dumping signal's default action writes one too (`Killed by
SIGSEGV`). On the desktop the same reports are the Crash Reports app
(`/bin/wm/apps/crashreports`), which a crash notice's Details opens.

`crashlog` shows the header only. The stack is for the host:
`tools/panic_resolve.py --crash <file>` names RIP and every return
address on it against the program's ELF, which is where the DWARF is.
Get the file off the machine with `tools/remote.py get`, or out of a
QEMU disk image with `tools/tfs3_writer.py read`.

What is deliberately NOT here: a kernel panic. `/var/crash` holds
processes that faulted while the kernel carried on, which is the system
working; a panic's record is a separate mechanism, not built yet, and
the two are kept apart so that a list of crashes never trains anyone to
ignore it.

## Traps

A report is not written when a filesystem operation was in flight at
the fault -- a page fault inside a copy helper under a read, say -- and
the kernel log says so: `crash: no report -- a filesystem operation is
in flight`. Nor for a program started by the `#` shell's legacy `run`
loader, which has no pid; `spawn` it instead.
