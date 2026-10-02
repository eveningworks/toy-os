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

`crashlog <report>` prints a summary, the **backtrace**, then the text
header. The backtrace is recovered on the machine (`lib/ucrash.h`, the
same reading the Crash Reports viewer shows): every word of the saved
stack from RSP up that points into code AND follows a call instruction,
named from that binary's own symbol table on disk --

    /bin/wm/apps/notepad (pid 18): Killed by SIGSEGV, sent by pid 20
    where: sys_futex_wait +0xa in notepad

    backtrace, found by scanning the stack:
      #0  0x80000081fa  sys_futex_wait +0xa  (notepad)
      #1  0x900005bce9  uapp_pump +0x249  (libuapp.so)
      #2  0x900005ca79  uapp_run +0x699  (libuapp.so)
      #3  0x8000007145  main +0x165  (notepad)

A scan can include a stale return address left in a local, and a binary
replaced since the crash names the wrong functions (it says so). Source
lines are the host's: `tools/panic_resolve.py --crash <file>` resolves
RIP and the stack against the build's DWARF. Get the file off the
machine with `tools/remote.py get`, or out of a QEMU disk image with
`tools/tfs3_writer.py read`.

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
