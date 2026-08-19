# Error codes: giving a failed syscall a reason

Every syscall here reports failure as `-1`. The REASON goes to the
kernel log, where a person can read it in `dmesg` and a program cannot:

```c
klog_write("syscall: write() rejected -- bad fd\n");         regs[14] = -1;
klog_write("syscall: write() rejected -- invalid buffer\n"); regs[14] = -1;
klog_write("syscall: write() rejected -- fd is read-only\n");regs[14] = -1;
```

Measured on 2026-08-19: **58 `-1` return sites across the five handler
files** (24 in `syscall_fd.c`, 13 in `sys_syscalls.c`, 9 in
`win_syscalls.c`, 8 in `proc_syscalls.c`, 4 in `fs_syscalls.c`), against
**42 syscalls** and **23 distinct rejection reasons already written out
as text**. The information exists; it is thrown away at the ABI.

## Why this is the next thing

It is not a libc feature -- it is an ABI change that several other
things are silently waiting on:

- **`open()` cannot say why.** `/bin/tosh`'s `find_program()` probes
  each PATH candidate by opening it and treats any `-1` as "not there",
  which would equally swallow a full descriptor table. A shell that
  cannot tell those apart reports "not found" for a machine that is out
  of fds.
- **`FILE*` and the rest of a libc need it.** `fopen` returning NULL is
  only useful with `errno` behind it, and every wrapper above that
  inherits the same gap.
- **The one place toy-os DID distinguish two outcomes proves the
  point.** `SYS_RETRY` (-2) exists because "0 means try again" made a
  pipe read report EOF the moment its writer produced something. A
  distinct value for a distinct outcome, because merging them cost a
  real bug. This is that lesson applied to the other 57 sites.

## Shape

**Return `-ERRNO`, not a global.** Linux's kernel convention: a small
negative number IS the error, and the C library turns it into `-1` plus
`errno`. The alternative -- a global `errno` -- needs thread-local
storage the moment threads exist, and toy-os has none (Milestone 24
lists TLS as absent on purpose). A returned code needs nothing.

The range matters: `SYS_RETRY` is already `-2`, so error numbers must
either avoid that value or `SYS_RETRY` must move. Deciding that is step
0, because every later step depends on the encoding.

**Which errors.** Not POSIX's full set. The 23 reasons already logged
are the honest starting list, collapsed to something like: `EBADF`,
`ENOENT`, `EEXIST`, `EINVAL`, `EFAULT`, `EMFILE`, `ENOSPC`, `EPERM`,
`EAGAIN`, `EIO`, `ENOMEM`, `ENOSYS`. Add one when a handler genuinely
distinguishes it, not because POSIX has it -- the same bar `kernel/lib/`
holds for a new helper.

## Staging

Each stage is independently shippable and independently verifiable.

### Stage 0 -- the encoding
Pick the numbering and its relationship to `SYS_RETRY`. Write the
`abi/errno.h` header with the values and one line each on what they
mean HERE (not what POSIX says). Nothing else changes.

**Exit criterion:** the header exists and `SYS_RETRY`'s value is
unambiguous against it.

### Stage 1 -- libsys reports it
`userland/rt/sys.c` maps a negative return to `-1` plus a readable
accessor (`sys_errno()`), so ring 3 can ask. Handlers still return `-1`,
so nothing changes behaviourally -- this is the plumbing that lets
stage 2 land one handler at a time.

**Exit criterion:** a `/tests` ELF can call `sys_errno()` and get
"no error" for every call that succeeds.

### Stage 2 -- the fd and filesystem handlers
`syscall_fd.c` and `fs_syscalls.c` first: 28 of the 58 sites, and the
ones a shell actually hits. Each `klog_write(... rejected ...)` becomes
a returned code; keep the log line, since a human reading `dmesg` still
wants the sentence.

**Exit criterion:** `open()` on a missing file and `open()` with a full
descriptor table are distinguishable from ring 3, asserted by a test
that fills the table on purpose.

### Stage 3 -- the rest
`proc_syscalls.c`, `win_syscalls.c`, `sys_syscalls.c`.

### Stage 4 -- the callers that were guessing
`find_program()` stops treating every failure as "not found". Anything
else found while doing stages 2-3.

**Exit criterion:** a shell asked to run a command with the fd table
full says so, rather than "not found".

## What this does NOT include

- **`strerror()`** -- a table of strings in ring 3. Cheap once the
  numbers exist; not part of the ABI.
- **A global `errno`** -- see above; the returned code is the ABI and a
  per-thread global is a TLS question that can wait for threads.
- **Changing what any syscall DOES.** Only what it reports. A stage that
  finds a genuine behaviour bug should split it into its own change.

## Traps this project has already recorded, that apply here

- **A sentinel must be a value the call can never legitimately return.**
  `SYS_RETRY` is `-2` for exactly this reason; a read of 0 bytes is real
  EOF. Any error encoding has to keep that property against every
  syscall's legitimate return range -- `SYS_SBRK` returns a POINTER, so
  "small negative" must not collide with a valid one.
- **Adding a syscall is three edits and one of them is a table row**
  (`CLAUDE.md`). Error codes touch the handlers, not the table, but the
  `strace` description in that same row is where a decoded error should
  eventually be printed.
- **A positive control that reddens nothing means the fixture never
  reached the branch.** Testing `EMFILE` needs a test that genuinely
  exhausts `FD_MAX`, not one that assumes it did.
