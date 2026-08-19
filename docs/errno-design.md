# Error codes: giving a failed syscall a reason

**BUILT, 2026-08-19.** All five stages landed together. This file is now
the record of what was decided and what was deliberately left out; the
staging below is kept because the exit criteria are what the work was
checked against.

Before it, every syscall reported failure as `-1` and wrote the REASON
to the kernel log, where a person could read it in `dmesg` and a program
could not:

```c
klog_write("syscall: write() rejected -- bad fd\n");         regs[14] = -1;
klog_write("syscall: write() rejected -- invalid buffer\n"); regs[14] = -1;
klog_write("syscall: write() rejected -- fd is read-only\n");regs[14] = -1;
```

Measured on 2026-08-19: **58 `-1` return sites across the five handler
files**, against **42 syscalls** and **23 distinct rejection reasons
already written out as text**. The information existed; it was thrown
away at the ABI. The log lines are all still there -- a human reading
`dmesg` still wants the sentence, and the code is for the program.

## What was decided

- **Linux's numbers, not a private set.** `EPERM` is 1, `ENOENT` is 2,
  `EBADF` is 9. Anything ported here reads the way it does everywhere
  else. What is NOT copied is the SIZE of that table: `abi/errno.h`
  defines the fourteen codes a handler in this kernel actually
  distinguishes, and adding one needs a handler that genuinely tells
  that case apart.
- **`SYS_RETRY` moved from -2 to -4095**, one past the top of the error
  range, because -2 is now `-ENOENT`. It was NOT made `-EAGAIN`, which
  is the obvious-looking move: EAGAIN is an error a caller reports,
  while SYS_RETRY means the call did not fail at all -- the process was
  woken and must ask again, which libsys absorbs in a loop the caller
  never sees. Merging them would make every blocking call's spurious
  wakeup look like a failure one layer up, which is the same mistake as
  using 0.
- **The kernel returns `-ERRNO`; libsys turns it back into `-1` plus
  `sys_errno()`.** Call sites are unchanged -- every existing `if (fd <
  0)` still works -- and one that wants the reason now has somewhere to
  ask. The global lives in ring 3 rather than the kernel because the
  RETURNED code is the ABI.
- **`sys_errno()` is a plain global, and that is a TLS question
  deferred.** It is correct while a process has one thread, which is all
  toy-os has; `docs/roadmap.md` carries thread-local storage and threads
  as items, and when TLS lands this declaration moves and nothing else
  does. Recorded rather than hidden: this is the one thing in the design
  that has a known expiry.

## What was deliberately left out

- **The syscalls whose failure value is 0**, not -1: `unlink`, `kill`,
  `gettime`, `proc_info`, `win_create`, `set_color` and friends. A
  negative code is TRUTHY, so returning one from these would make every
  `if (!sys_unlink(p))` caller read a failure as SUCCESS -- silently,
  and at every call site at once. Flipping their polarity is a
  caller-visible change and belongs in its own commit; `docs/roadmap.md`
  carries it as an item, and `sys_unlink()`'s handler carries the
  comment.
- **`sbrk`.** It hands back a POINTER, and `(void *)-1` is the value
  `heap_os.c`, `ugfx.c` and the WM already test against -- a small
  negative code there would be a plausible and wrong address. It keeps
  returning -1 and libsys records ENOMEM beside it, which is exactly
  what POSIX `sbrk()` does. `strace` does not decode a pointer-returning
  syscall's -1 for the same reason: -1 is EPERM's value, and naming it
  would print a reason sbrk never gave.
- **A global `errno` in the KERNEL**, and changing what any syscall
  DOES. Only what it reports.

## Why this was the next thing

It was not a libc feature -- it is an ABI change several other things
were silently waiting on:

- **`open()` could not say why.** `/bin/tosh`'s `find_program()` probes
  each PATH candidate by opening it and treated any `-1` as "not there",
  which equally swallowed a full descriptor table. A shell that cannot
  tell those apart reports "not found" for a machine that is out of fds.
  That is now fixed, and it is the change's headline.
- **`FILE*` and the rest of a libc need it.** `fopen` returning NULL is
  only useful with `errno` behind it, and every wrapper above that
  inherited the same gap.
- **The one place toy-os DID distinguish two outcomes proved the
  point.** `SYS_RETRY` exists because "0 means try again" made a pipe
  read report EOF the moment its writer produced something. A distinct
  value for a distinct outcome, because merging them cost a real bug.
  This is that lesson applied to the other 57 sites.

## Staging

Each stage is independently shippable and independently verifiable.

### Stage 0 -- the encoding (DONE)
Pick the numbering and its relationship to `SYS_RETRY`. Write the
`abi/errno.h` header with the values and one line each on what they
mean HERE (not what POSIX says). Nothing else changes.

**Exit criterion:** the header exists and `SYS_RETRY`'s value is
unambiguous against it.

### Stage 1 -- libsys reports it (DONE)
`userland/rt/sys.c` maps a negative return to `-1` plus a readable
accessor (`sys_errno()`), so ring 3 can ask. Handlers still return `-1`,
so nothing changes behaviourally -- this is the plumbing that lets
stage 2 land one handler at a time.

**Exit criterion:** a `/tests` ELF can call `sys_errno()` and get
"no error" for every call that succeeds.

### Stage 2 -- the fd and filesystem handlers (DONE)
`syscall_fd.c` and `fs_syscalls.c` first: 28 of the 58 sites, and the
ones a shell actually hits. Each `klog_write(... rejected ...)` becomes
a returned code; keep the log line, since a human reading `dmesg` still
wants the sentence.

**Exit criterion:** `open()` on a missing file and `open()` with a full
descriptor table are distinguishable from ring 3, asserted by a test
that fills the table on purpose.

### Stage 3 -- the rest (DONE)
`proc_syscalls.c`, `win_syscalls.c`, `sys_syscalls.c`.

### Stage 4 -- the callers that were guessing (DONE)
`find_program()` stops treating every failure as "not found". Anything
else found while doing stages 2-3.

**Exit criterion:** a shell asked to run a command with the fd table
full says so, rather than "not found".

`find_program()` returns a third answer now -- 1 found, 0 not on PATH,
-1 the search could not be COMPLETED -- and stops walking the rest of
PATH on anything that is not ENOENT, since EMFILE means the next probe
cannot succeed either. **Its own branch is verified by inspection, not
by a test**, and that is worth stating plainly: driving `/bin/tosh` into
EMFILE needs a way to leak descriptors from the shell, and no builtin
does. What IS tested is everything it depends on -- `/tests/errno_test`
exhausts a real descriptor table and asserts open() answers EMFILE where
it used to answer -1.

## What this does NOT include

- ~~**`strerror()`**~~ -- built after all, since it was four lines once
  the numbers existed. `sys_strerror()` in libsys, with the POSIX name
  forwarding to it from `userland/lib/string.h` so there is one table
  rather than a libc copy that drifts. It returns the SENTENCE ("no such
  file or directory"); `strace` prints the macro name (`ENOENT`),
  because a person debugging the kernel greps for the identifier and a
  person at a prompt does not.
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
