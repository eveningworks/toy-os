# Roadmap: details

Full reasoning, phased test plans and `docs/decisions.md` pointers for
the items in **[docs/roadmap.md](roadmap.md)**, which is the list itself.

Split out on 2026-08-18, when the two halves together passed 4,300
lines: the list is read every session and the details once per item, so
keeping them in one file meant scrolling past the half you did not want.
The order here follows the old milestone numbering rather than the
list's layers, deliberately -- these are anchors, and re-sorting them
would break every pointer into them.

Full reasoning, phased test plans, and the git history/decisions.md pointers for
every item above, in the same order.

### Async I/O to the desktop

Apps run in kernel space, so a disk write from Notepad's Save, the shell's
`stress`, or a future Terminal command all sit inside the same call stack
`wm_run()`'s event loop is also on -- a slow write freezes the whole
desktop, not just the operation. Broken into phases, each independently
testable:

1. [x] ~~Non-blocking DMA start/poll primitive~~ -- done (see the v0.1.0 release): `dma_transfer_start()`/`dma_transfer_poll()`
   (`kernel/drivers/ata.c`/`ata.h`), built from the same `dma_issue()`/
   `dma_finish()` halves the existing blocking `dma_transfer()` uses, so the
   blocking path is unchanged. No real caller yet -- proven standalone via
   the `dmatest [lba]` shell command (read-only, byte-compares a blocking
   read against the non-blocking one, reports poll count).
2. [x] ~~Steppable write API~~ -- done (see the v0.1.0 release
   entry): `fs_write_range_begin()`/`fs_write_range_step()` (`fs.h`,
   dispatched through `fs_ops.h`/`vfs.c` to `tfs.c`'s
   `tfs_write_range_begin()`/`_step()`), built from the same
   `write_range_one_block()` helper `write_range_impl()` uses, so the
   existing blocking path is unchanged. No real caller yet -- proven
   standalone via the `steptest <mb>` shell command (writes via an explicit
   step loop the command drives itself, verifies byte-for-byte against
   readback, reports step count).
3. [x] ~~Wire up one real caller~~ -- done (see the v0.1.0 release): `wm_run()` polls a pending write once per frame
   (`apps/wm/wm.c`, a new WM-global `pending_write` slot) instead of
   calling `fs_write_range()`/`fs_write()` and blocking; Notepad's Save As...
   (`apps/notepad.c`) is the first non-blocking caller, via two new public
   entry points (`wm.h`'s `window_start_write()`/`window_write_pending()`)
   and a completion callback (`gui_apps.h`'s `on_write_complete`). A real
   bug (a stale cached `struct window *`, not safe to hold across frames in
   this WM -- see `docs/decisions.md`) was caught and fixed during QMP
   testing, not by code review alone. Test: QMP -- seeded a large file
   directly onto `disk.img` (the writer tools via `tools/seed_disk.py`, no boot needed),
   Notepad Save As... over it, clicked a second window immediately after
   confirming Save -- caught "Saving..." with the Save button disabled, and
   the desktop successfully switching focus to the other window while the
   write was still in flight, proving it didn't freeze.
4. [x] ~~Generalize to reads and the plain shell prompt~~ -- done (see
   the v0.1.0 release). Read side:
   `fs_read_range_begin()`/`fs_read_range_step()` (`fs.h`, dispatched
   through `fs_ops.h`/`vfs.c` to `tfs.c`'s `tfs_read_range_begin()`/
   `_step()`), built the same way Phase 2 built the write side, plus a
   second `wm_run()` poll slot (`pending_read`/`pending_read_win`)
   mirroring Phase 3's write slot -- Notepad's Open... is the first
   real caller.
   Shell side turned out not to need `wm_run()`-style ambient polling
   at all: `shell_main()` (`apps/shell.c`) is a REPL with no equivalent
   per-frame tick between "block on keyboard input" and "dispatch one
   command", so there's no loop to hang a pending op off of the way
   `wm_run()`'s `for(;;) { hlt; ...; }` does. The actual gap (see
   `keyboard.c`'s own `keyboard_getchar()` comment) is narrower: a
   blocking command doesn't get `debug_console_poll()`/
   `vga_cursor_tick()` serviced at all until it returns, unlike the
   shell's idle wait at the prompt (already rides `keyboard_getchar()`'s
   `hlt` loop) or the GUI (rides `wm_run()`'s poll). Closed by making
   `cat` (`apps/shell_fs.c`) -- the shell's one command with no size cap
   on how much it blocks reading, unlike Notepad's Open... which is
   capped at `SCROLLBACK_CAP` and finishes in 1-2 steps regardless -- use
   the stepped read API in its own loop, calling
   `debug_console_poll()`/`vga_cursor_tick()` between blocks instead of
   not at all. No `hlt`/throttling in that loop (unlike `wm_run()`'s,
   which is gated on its own idle wait) -- `cat` has real work to do and
   wants to finish as fast as the disk allows, it just also services the
   debug console/cursor between blocks now.

Separately, async/continuously-armed process spawning for the GUI Terminal
(Async I/O to the desktop phase 4b) -- [x] done, see the v0.1.0 release
entry. Was deliberately deferred, not started, when `ls` migrated to a
real `/bin` binary (blocked in the Terminal same as `run` at the time,
see docs/decisions.md) -- the blocker was architectural, not a small fix:
`elf_run_from_fs()`/`process_run_ring3_args()` (`kernel/proc/elf_run.c`,
`kernel/proc/process.c`) is synchronous and blocking by design, and
`wm_run()` is a plain, uninterrupted kernel-space event loop, never itself
scheduler-managed. Built as planned:
- **A public, non-blocking spawn API**: `scheduler_spawn(path, args)`/
  `scheduler_poll(pid, &exit_code)` (`kernel/include/api/scheduler.h`,
  `kernel/proc/scheduler.c`) -- a thin public wrapper over the M16
  scheduler's existing (previously `static`) `spawn_from_fs()`, extended
  to build a real argv via a newly-exposed `elf_build_argv_on_stack()`
  (promoted out of `elf_run.c`, same layout `elf_run_from_fs()` itself
  uses) instead of the trapframe always zeroing rdi/rsi. A process that
  exits now becomes a `SCHED_ZOMBIE` (holding its exit code) instead of
  being freed straight to `SCHED_UNUSED` -- `scheduler_poll()` is the
  explicit reap step, same two-phase shape a real `wait()`/`waitpid()`
  has.
- **The scheduler continuously armed**: `scheduler_armed` is now set once
  in `scheduler_init()` and never unset (`scheduler_demo_run()`/
  `schedtest` no longer touches the flag at all) -- safe because an armed
  tick over an empty process table is a byte-for-byte no-op, the same
  invariant that made the old demo-only flag safe in the first place, see
  `scheduler.c`'s updated top comment.
- **`wm_run()` polling a running process**: a third WM-global slot,
  `pending_proc`/`pending_proc_win` (`apps/wm/wm_internal.h`), polled
  once per frame via `scheduler_poll()` -- mirrors `pending_write`/
  `pending_read`'s exact shape (`wm.h`'s `window_start_process()`/
  `window_process_pending()`, `gui_apps.h`'s `on_process_exit`
  callback), including `bring_to_front()`/`close_window()` keeping
  `pending_proc_win` accurate and `close_window()` refusing to close a
  window with a process pending. One real difference: this poll doesn't
  make the process's output appear -- that already streams straight into
  the window's scrollback via `vga_putc()`'s active sink (`vga.h`) the
  instant each `SYS_WRITE` syscall runs, independent of `wm_run()`'s
  frame rate; the poll only detects completion.
- **Per-window process state in `apps/terminal.c`**: `st->running_pid`
  (blocks all keyboard input while set, same as `st->in_editor` does for
  `edit`/`nano`) and `st->saved_sink`. `ls` always spawns async now;
  `run <name>` does too, but ONLY for names on a new explicit allowlist,
  `RUN_ALLOWED_BINS` -- the opposite of `BLOCKED_CMDS`'s blocklist
  approach, deliberately: each entry (`crash_test`, `exit_test`,
  `file_test`, `hello`, `lspci`, `newsyscalls_test`, `socket_test`,
  `write_bad_test`, `write_test`) was checked against its own
  `userland/*.c` source for the two real hazards -- reading stdin (there's
  no stdin routing to a spawned process yet, so one blocked on it would
  hang forever, and `close_window()` refuses to close a window with a
  process pending, stranding the whole window) or touching the
  framebuffer/its own window directly -- not assumed safe by name alone.
  Excluded: `echo` (loops on `SYS_READ_KEY` waiting for Esc, which never
  arrives), `gui_test`/`win_test` (framebuffer/own-window takeover),
  `counter_a`/`counter_b` (infinite-loop-by-design `schedtest` demo
  processes, not real commands). `crash_test` deliberately faults --
  verified safe anyway: `idt.c`'s fault handler was already
  scheduler-aware from M16 (`recoverable`'s `scheduler_current_pid()`
  check), tearing the process down and reporting "RING-3 PROCESS
  CRASHED" through whatever sink is active with exit code -1, exactly
  like a legacy `run crash_test` from the physical shell -- confirmed by
  actually running it through the new path in QMP testing, not assumed.
- Test: QMP -- `schedtest` still spawns/interleaves/exits its two counter
  processes correctly with the scheduler now permanently armed; the
  physical shell's legacy `run <name>`/`ls` unaffected; in the GUI
  Terminal, `ls` lists a real directory and reports "Process finished.
  Exit code: 0", `run exit_test`/`run crash_test` report the right exit
  code (42, CRASHED) without freezing or hanging, `run gui_test` and
  `schedtest`/`gui` still get the expected refusals, and the window
  closes normally afterward with no stuck state. `tools/preflight.sh`
  (build + boot smoke test) passed throughout. Screenshots in
  `screenshots/2026-08-12/`.
- Not done this round, deliberately out of scope: stdin routing to a
  running process (needed before more of `RUN_ALLOWED_BINS`'s exclusions
  could be reconsidered), and any Terminal-side visual indicator that a
  process is running beyond the missing prompt/blocked input (no
  spinner/"Running..." status the way Notepad's Save/Load show one --
  every allowlisted binary finishes in well under a frame in practice, so
  there was nothing to visually prove was non-blocking the way Notepad's
  Save As... needed a deliberately large file to demonstrate).

### Memory protection hardening

New milestone, lightly scoped -- added because the build surfaced the
gap itself: `make all` used to emit `ld: warning: build/kernel.bin has
a LOAD segment with RWX permissions`, and `CFLAGS` explicitly passed
`-fno-stack-protector`. Neither was a bug (nothing about them broke
correctness), but they're exactly the kind of baseline hardening a real OS
has and a hobby kernel this far along is a natural point to start closing.
Both of those are closed now; SMEP/SMAP, ASLR and the entropy source
they need are what's left:

- ~~NX bit enforcement~~ (userspace) -- done, see the commit that added it: EFER.NXE set at boot, `vmm_map_user_page()`
  now defaults to non-executable (correct for the stack/heap/
  framebuffer/window-buffer pages that were its only pre-existing
  callers), and `elf.c`'s loader reads each PT_LOAD segment's real
  `p_flags` instead of mapping everything RWX -- which only means
  anything because `userland/rt/link.ld` now emits separate page-aligned
  segments per permission class instead of one merged one. Verified
  with exactly the deliberate "jump into a data page" test this bullet
  originally called for (`userland/tests/nx_test.c`, `run nx_test`): the
  kernel reports a Present+User+Instruction-Fetch page fault and tears
  the process down instead of executing the injected code. The
  kernel's own identity map (`boot.asm`) was left RWX at this point --
  the W^X bullet below is where the kernel half landed.
- ~~W^X on kernel + userspace mappings~~ -- both halves done. Userspace
  came with NX above (same `vmm_map_user_page()`/`elf.c` change: a
  segment's writable bit now comes from its real `PF_W` flag too, not a
  blanket 1). The kernel half is `paging_enforce_wx()`
  (`kernel/arch/x86_64/paging.c`, called from the top of
  `kernel_main()`): every one of the identity map's 2048 2MiB PDEs gets
  its NX bit set and stays a huge page, and the single slot holding the
  read-only part of the kernel image is split to 4KiB so `.text` can be
  executable-and-read-only while `.rodata` is read-only and NX. Plus
  CR0.WP, without which ring 0 ignores the read-only bit entirely and
  half the protection is decorative. `linker.ld` supplies the four real
  PT_LOAD segments, the `ALIGN(4096)`s and the boundary symbols it
  reads. This turned out much smaller than the estimate above: the
  whole read-only part of the image fits in the FIRST 2MiB page, so it
  is one static table out of `.bss` and no allocator, and `pmm.c`
  needed no change at all. Seven KTESTs, two positive controls that
  each fire on exactly the right check, and a live `PANIC: Page fault`
  from a deliberate write to `.text` -- see `docs/decisions.md`'s
  kernel W^X entry and the commit that added it.
- ~~Stack canaries~~ -- done, see the commit that added it:
  `-fstack-protector-strong` is on for both the kernel and userland now,
  with `-mstack-protector-guard=global` (a fixed constant, not random --
  no entropy source exists yet, see `docs/decisions.md`) since there's
  no TLS/FS-base infrastructure for GCC's default guard to read. A
  canary violation is caught and reported, not silently corrupting the
  stack -- verified for real with a deliberate userland self-test
  (`run stack_smash_test`), not just "the kernel still boots".
- ~~Kernel ASLR~~ -- done, all three stages; see the milestone list
  above for what landed and what the scoping got wrong. Was written up
  here as the lowest priority of the four, on the grounds that its real
  value depends on an attacker model this toy OS does not have -- which
  is still true, and the mechanism is worth having anyway. The entropy
  source it was blocked on arrived first, and the base still cannot use
  it (`krandom_init()` spins on a PIT that has not started this early),
  so the base has its own RDSEED/RDRAND/TSC path.

### Storage hardening

~~Full end-to-end multi-GB file write/read stress test over TFS2~~ --
**done, 2026-08-13.** Both `stress 4200` (326 s) and `stress 8192`
(692 s) PASSED, each written, read back and verified byte-for-byte,
at 23.2 and 21.7 MB/s write. 8192 MB is the whole point of the 9 GiB
image: it exercises the block allocator, the indirect-pointer chains and
the journal at a scale nothing else here reaches.

Two things worth keeping from how this finally happened. It had been
sitting unattempted because the estimate was wrong in a discouraging
direction -- this entry predicted ~20 and ~40 minutes from a
then-current ~3.5 MB/s, but the write-batching work landed in between
and the real runs came in at 5.5 and 11.5 minutes. An estimate written
against an old measurement is worth re-deriving before letting it decide
what's too expensive to try. And the first `stress 4200` attempt did NOT
pass: it died at 11% on an ATA timeout that turned out to be a real
driver bug (see the commit that added it on the pre-issue
busy wait). The scale test earned its keep by failing first.

~~TFS2/ATA write performance, part 2~~ -- done, see the commit that added it. Went exactly the way this item predicted (raise the
per-command cap, then batch contiguous runs into it): the DMA bounce
buffer grew from 1 frame to 16, `ATA_MAX_SECTORS_PER_XFER` went 8 -> 128
(one full PRD), and `contiguous_run()` merges consecutive blocks into a
single `ata_write_sectors()`/`ata_read_sectors()` call. One thing this
item didn't predict and the measurement did: the zero-fill write every
freshly allocated block used to get was doubling the command count and
splitting the coalesced runs apart, which is why the first cut only
reached 20.3 MB/s of the eventual 25.1. Still open in this area: a
sequential-read benchmark that isn't dominated by `stress`'s own
byte-for-byte verify loop, which is why the read-side gain (27 -> 30.5
MB/s) is measured less precisely than the write side.

~~Also: journal-batched flush~~ -- done, and it landed differently
from how this item framed it. The framing here was "flush once per
logical operation rather than once per physical block", which would have
weakened the per-record guarantee to a per-operation one. What the
protocol actually needs is narrower and costs nothing: of
`persist_record()`'s four flushes, only two are barriers the recovery
path depends on (after the commit header, after the table slot); a torn
journal-data write is caught by the checksum, and a lost header-clear
costs one idempotent replay. Dropping the other two halved metadata cost
with the crash-safety argument unchanged -- a 256-record format went
0.73s to 0.34s. See `docs/decisions.md`.

~~GPT/MBR partition table parsing~~ -- done, see the commit that added it: `kernel/drivers/partition.c`'s
`partition_read_table()` reads LBA 0 (and LBA 1 + the entry array, for a
protective-MBR-signaled GPT disk), exposed via a new `parttable` shell
command. Read-only, parse-only, same as originally scoped here --
a fresh `disk.img` is one raw TFS3 volume (which deliberately
reserves its first 32 KiB for an MBR/GPT -- see docs/tfs3-design.md's
"Volumes and partitions"), not a partitioned disk,
and this never gets consulted by the mount path. See `docs/decisions.md`
for why the GPT half of this couldn't be verified via a live in-VM boot
test the way the MBR half was (TFS2's own self-test unconditionally
overwrites LBA 1, the GPT header's mandated location, on every boot) and
what verified it instead (a host-compiled unit test including the real
kernel source unmodified, `tools/mkpart_test.py`).

### Kernel test harness

**Done** (2026-08-13) -- see the commit that added it. Kept
here because the reasoning is still the reference for adding tests.

The problem it solved: `heap_selftest()`, `tfs_selftest()`, the PMM
check and `json_selftest()` were real tests that had each caught real
bugs, but they were hand-called from `kernel_main()`, ran on every boot
whether you wanted them or not (`tfs_selftest()` wrote at a 4.6GB offset
on every disk-backed boot to re-verify something that only breaks when
`tfs.c` changes), couldn't be run individually, and a failure printed a
line and carried on booting -- so nothing failed and CI never noticed.

What exists now:

- `KTEST("suite", "name") { ... }` blocks living next to the code they
  exercise, registering themselves through a `.ktests` linker section --
  no registry to update, and with recursive source discovery no build
  edit either. `kernel/include/kernel/ktest.h` is the reference.
- `ktest` / `ktest <suite>` from the shell or the serial console;
  `make test` boots headless, drives it over serial and **exits
  non-zero**, which the old arrangement could not do at all. CI runs it
  beside `boot_smoke_test.py`, and `tools/preflight.sh` includes it.
- Fault injection (`kernel/include/kernel/fault_inject.h`): fail the
  next N ATA writes, ATA reads or `kmalloc` calls -- what made the
  storage error paths testable from inside rather than only by
  corrupting a disk image from the host.
- Nothing runs tests at boot any more.

Two things worth carrying forward to any test you add: they run inside
the **live** kernel, so don't assume a pristine heap or an empty
filesystem (that exact assumption is what made `heap_selftest()` fail on
its first run under the harness -- it asserted `heap_used_bytes() == 0`,
true only immediately after `heap_init()`); and a test that arms a fault
injector must disarm it, since there's no automatic teardown.

Still open in this area, and deliberately not done: no per-test
isolation or setup/teardown, no way to run tests before the filesystem
exists, and nothing catches a leaked fault injector between tests beyond
`fault_any_armed()` existing.

### Benchmark suite

`stress`, `dmatest` and `steptest` each answer one question well and were
each written for one change. There's no way to ask "is anything slower
than it was" and get an answer.

The specific gap that motivated this: when the DMA/coalescing work
measured read throughput, the number was polluted by `stress`'s own
byte-for-byte verification loop, so the read-side gain (27 -> 30.5 MB/s)
is known less precisely than the write side. A benchmark that measures
one thing at a time would have answered it directly.

Wanted: a `bench` command covering disk, memory, scheduler and rendering;
baselines checked into the repo; and a comparison that flags a regression
past a threshold, the same shape `tools/screenshot_diff.py` already uses
for pixels. CI wiring is optional and probably shouldn't gate merges --
emulated timings under a shared CI runner are noisy enough to produce
false alarms, which is worse than no signal.

### Fuzzing & property-based testing

`ktest` (Kernel test harness) checks the cases someone thought of. This checks
the ones nobody did, and it belongs here -- immediately after the
harness it builds on -- because everything below it is easier to trust
once it exists. The syscall layer is the obvious first target: it is the
one place ring-3 code hands the kernel arbitrary numbers, and
`syscall.c` already validates pointers precisely because that's where
the danger is. A fuzzer's job is to find the validation nobody wrote.

Two properties of a good fuzzer that a first attempt usually skips, both
worth building in from the start. **A failure has to be reproducible**:
drive everything from a seeded PRNG and print the seed, or a crash found
once is gone forever. And **a failure has to be small**: the raw input
that broke something is usually 500 random syscalls, of which two
mattered, so shrinking it before reporting is the difference between a
bug report and a haystack. `fault_inject.h` is already most of the
infrastructure -- it fails the Nth allocation or ATA write on command;
letting the same seed choose *which* N turns it into a fuzzing input.

The intended relationship with CI is a time budget, not a pass/fail run:
fuzz for a fixed number of seconds, keep any failing seed as a permanent
regression test in the corpus. That way the suite grows with what's
actually been found rather than what someone predicted.

### TTY / virtual terminals

The shell doesn't run *on* a terminal today -- it **is** the terminal. It
reads `keyboard_getchar()` directly and writes through `vga_write()`, and
the GUI Terminal app gets in on that by swapping the output sink
(`vga_set_sink()`, see `docs/decisions.md`). That works, and it's why
`edit`/`nano` can run in both places unchanged, but it leaves no layer
that owns the questions a terminal is supposed to answer: which process
is in the foreground, what `Ctrl+C` means, whether input is line-buffered
or raw.

This is the milestone that keeps showing up as a prerequisite elsewhere.
Signals & process control can deliver a signal, but "deliver SIGINT to the foreground
process" has no meaning without a foreground process. Shell pipes & job control's job
control (`fg`/`bg`) is the same problem wearing a different hat. Doing
those first means inventing a partial answer twice.

Scope it as: a line discipline (echo, raw vs. cooked) that
the shell reads through instead of touching the keyboard driver -- the
line-*editing* part of that now exists as `kernel/lib/klineedit.c` and
should be moved behind the discipline rather than rewritten; a
per-terminal notion of the foreground process; and then multiple virtual
terminals as the payoff, since once a terminal is a *thing* rather than
the only thing, having four of them on `Ctrl+Alt+F1..F4` is mostly
bookkeeping. The GUI Terminal and the physical console should end up as
two clients of the same layer, which is a good test that the abstraction
is real.

### Demand paging & shared memory

Today a process's pages are all mapped up front, and every process gets
its own private copy of everything. That's the simplest thing that works,
and it's why `fork()` (`fork()`/`exec()`-style process model) needs copy-on-write to not be
absurd -- but COW is one instance of a general mechanism this kernel
doesn't have: deciding what to map at fault time rather than at load
time.

Build the general version once: a page-fault handler that consults a
per-process mapping description, then allocate-on-first-touch, then
file-backed `mmap`, then `MAP_SHARED` between processes. Shared read-only
text pages between two instances of the same binary fall out of it almost
for free, and are a satisfying thing to demonstrate in Task Manager
(two Calculator windows, one copy of the code).

Ordering note: this and `fork()`/`exec()`-style process model's COW are the same machinery. Whoever
does either should look at the other first -- doing `fork()`/`exec()`-style process model's COW as
a `fork()`-specific special case would mean writing it twice.

### `fork()`/`exec()`-style process model

New milestone, lightly scoped. Today's only way to start a ring-3 process
is `spawn_from_fs()`/`elf_run_from_fs()` -- load a fresh ELF from disk and
jump straight to its entry point; there's no way for a running process to
duplicate itself or replace its own image. A real Unix-style shell (job
control in Shell pipes & job control, `run` as it exists today) eventually wants the
`fork()`+`exec()` split instead: a first rough breakdown --

- `fork()`-style address-space duplication -- copy a process's page tables
  (copy-on-write to avoid a full physical copy up front; `vmm.c`'s
  per-process PML4 model already gives each process an isolated address
  space, so this is "duplicate and mark pages read-only + a fault handler
  that copies on write," not a new isolation mechanism).
- `exec()`-style in-place replacement -- tear down a process's current
  address space and load a new ELF into it without allocating a fresh
  scheduler slot, unlike `spawn_from_fs()` today.
- `wait()`/exit-status reporting -- a parent process needs to learn its
  child exited and with what code; `scheduler_on_exit()` today discards
  the exit code entirely (see its own comment: "no exit-code tracking
  yet").
- Real PID allocation -- `scheduler.c`'s `MAX_PROCS = 4` fixed-slot table
  reuses slot indices as "PIDs" today; a real fork/exec model wants PIDs
  that don't get reused the instant a slot frees up, so a parent's
  `wait(pid)` can't accidentally match the wrong process.
- Larger/growable user stack -- every ring-3 process gets exactly one
  fixed 4KB page at a hardcoded `STACK_VADDR`
  (`kernel/proc/elf_run.c`/`scheduler.c`), mapped once at process start
  with no growth mechanism (no stack-fault-triggered auto-growth
  anywhere in the codebase). Fine for today's small test binaries;
  flagged directly by scoping out what a real C program (e.g. a
  `doomgeneric`-style port, see the Backlog's Doom entry) would need --
  untested whether 4KB is actually tight enough to matter for that
  specific case, but worth having a real answer (a bigger fixed stack,
  or real growth) rather than an unverified assumption either way.

### Signals & process control

New milestone, lightly scoped, paired with `fork()`/`exec()`-style process model above (a real
process model wants a way to influence a process besides "let it exit on
its own"). A first rough breakdown:

- Basic signal delivery -- a `kill(pid, sig)`-equivalent syscall that
  interrupts a running ring-3 process; needs a per-process pending-signal
  flag `scheduler_tick()` can check.
- Default dispositions -- at minimum "terminate" (the common case) and
  "ignore," without requiring a process to install a handler first.
- A `kill`/`ps`-style shell command -- list running processes
  (`scheduler.c`'s `procs[]` table has this info already, just not
  exposed) and send them a signal.
- Exit-status visible to a waiting parent -- largely closed already:
  `scheduler_on_exit()` records the code and holds the slot as a
  SCHED_ZOMBIE until `scheduler_poll()` reaps it (Async I/O to the desktop phase 4b).
  What is missing is a ring-3-visible `wait()`, not the bookkeeping.

**Ctrl-C specifically, because it is the feature people actually miss,
and because this file used to overstate what it needs.**

The long-standing claim was that SIGINT needs TTY / virtual terminals's TTY layer,
on the reasoning that "deliver SIGINT to the foreground process" is
meaningless without a foreground process. That is true for the PHYSICAL
shell, and it is true for job control (`fg`/`bg`). It is NOT true once
the terminal is a ring-3 process that spawns its own children
(The GUI in ring 3): such a terminal knows its child's pid because it asked
for it, so "the foreground process" is the terminal's own state and
needs no kernel concept at all.

What Ctrl-C actually requires, then:

- A `kill`-equivalent syscall and a per-process pending-signal flag the
  scheduler checks. This is the only new kernel mechanism.
- A TERMINATE default disposition applied at a safe point -- the next
  tick or a syscall return, never mid-handler -- and routed through the
  same teardown a normal exit uses, so the parent still gets a reaped
  result rather than a pid that disappears.
- An exit status a shell can distinguish from a clean exit, so it can
  say "Interrupted".
- Nothing in the keyboard driver: Ctrl already arrives as a control
  code (`keyboard.h`), so a ring-3 terminal receives `^C` today and
  simply has nowhere to send it.

Userspace signal handlers are a separate, larger item and must not gate
this: terminate-by-default is the behaviour nearly every program wants
from Ctrl-C, and shipping that first is what makes the shell usable.

### Crash reporting & postmortem debugging

When this kernel panics today it prints a message and stops. When a
ring-3 process faults, it's torn down and the reason is a line in the
log. Both are recoverable situations that currently throw away almost
everything that would explain them -- and the information isn't hard to
get, it's just never collected.

Most of the raw material already exists. `CFLAGS`/`USERLAND_CFLAGS`
carry `-g`, so every binary has real DWARF; the fault handler already
knows the faulting address and has the register state in hand; the
filesystem works. What's missing is walking the frame pointers to
produce a backtrace, a symbol table baked in so those addresses turn
into names, and somewhere to put the result that survives the reboot
that follows. A panic screen you can act on beats one you photograph.

Placed after signals because a core dump is naturally something that
happens *on the way to* killing a process -- the same path that will
deliver SIGSEGV. Worth keeping the two failure classes clearly apart in
whatever gets recorded: "the kernel faulted" is a bug, while "a process
faulted and the kernel tore it down cleanly" is the system working, and
a log that conflates them will train everyone to ignore it. Stack
overflow deserves its own detection (a guard page) for the same reason:
today it presents as an arbitrary fault somewhere unrelated, which is
exactly the confusing shape `text_scrollback`-on-the-stack already
produced once (see `docs/decisions.md`).

### Shell pipes & job control

New milestone, lightly scoped. Today's shell (`apps/shell.c`) dispatches
one command at a time to completion -- no `|`, no redirection, no
backgrounding. A first rough breakdown, and why it comes after Milestones
5-6 above: piping needs two processes running with a connected fd, and
backgrounding needs a process that isn't blocking the shell's own prompt --
both want a real process model first, not just today's "one blocking
`run`."

- `|` pipes -- connect one command's stdout fd to the next command's stdin
  fd; needs an in-kernel pipe buffer (a new fd kind, similar in spirit to
  `syscall.c`'s existing `FD_KIND_FILE`/`FD_KIND_SOCKET` tagged union).
- `>`/`<`/`>>` redirection -- reopen a command's stdin/stdout against a
  real file before it runs, reusing the existing `fs_*` calls.
- Background jobs (`&`) -- run a command via `fork()`/`exec()`-style process model's non-blocking
  spawn instead of `process_run_ring3()`'s synchronous one, returning
  control to the prompt immediately.
- `fg`/`bg`/`jobs` -- track backgrounded processes (extends the `ps`-style
  listing from Signals & process control) and let the shell wait on one explicitly.

### Init & service supervision

The first four process milestones (TTY, `fork`/`exec`, signals, job
control) each build a mechanism. This is the milestone that uses all
four at once for something real, which is why it sits after them rather
than among them -- and using a mechanism in anger is reliably where its
gaps show up.

Scope it by what an init actually has to do rather than by what real
init systems have grown into: be process 1, start what `/etc` says to
start, adopt orphans whose parent died, restart what exits unexpectedly,
and shut down in reverse order. The restart policy needs a backoff from
the start -- a service that crashes instantly and is restarted instantly
is an infinite loop that will look like a hung machine.

The last item matters most: **one real service, not a framework with no
users.** A supervisor with nothing to supervise proves nothing, and this
project has the `k_strcasecmp` precedent for what happens to a mechanism
without a caller.

**STAGES 0-2 ARE DONE** (`docs/init-design.md` is the staged plan; stage
2 landed 2026-08-18). What exists: init holds pid 1 and cannot be
killed, orphans are adopted and reaped, `system.default_target` decides
what boots with `target=` on the GRUB line overriding it for one boot,
and `/etc/services.d/<name>` descriptors say what to start. The desktop
is the one real service -- it is init's child now, not the shell's, so
its clients are init's grandchildren and killing it reparents them here.
`Restart=always` restarts with a doubling backoff and gives up on a
crash loop.

The service candidate went the other way from the guess above: the
serial debug console stayed kernel-side (it is `scheduler_idle()`'s
work, and every GUI test tool arrives over it), and the DESKTOP became
the service instead -- which is better evidence, because it is a real
process with a real lifetime that a person can kill and watch come
back.

Three things that are NOT built, and the middle one is the interesting
one. Nothing ORDERS the services: they all start at once, because with
two services' worth of ordering requirements (none) a unit graph with no
edges would be a data structure pretending to be a design. Nothing shuts
them down in reverse order on `reboot`. And a service's output still
goes to the kernel log via stderr rather than anywhere a person would
choose -- readable with `dmesg`, which is enough for one service and
will not be for six.

**What stage 2 exposed rather than built:** the physical shell and the
desktop were competing for the keyboard the moment both were running at
once (see `docs/decisions.md`). Ring 0's blocking readers are suspended
while a compositor holds the role, which is a placeholder for real
console ownership -- the TTY milestone's per-TTY input queue with a
foreground process is the actual answer, and stage 3's console device is
the next step toward it.

### In-OS documentation

toy-os has an unusually large amount of written reasoning for a hobby
OS, and none of it is readable *from inside the OS*. `help` lists
commands with a one-line description and that's the whole of it. A `man`
command is a small amount of work that makes everything else
discoverable without a second machine open.

Deliberately not troff. A tiny format a shell can render and a person
can hand-write is the right call for the same reason TFS2 isn't ext4 --
the goal is a working thing that's understandable end to end. The one
design point worth care: **generate the builtin pages from the same
table `help` already uses**, so a new command can't get a help line and
no page, or worse, two descriptions that disagree. The CI check in the
item list exists to enforce exactly that.

Pages get seeded onto the image at build time through
the host writer tools, the same path `/bin` already takes, so no
boot-time install step is needed.

### TFS3: an inode layer

**LANDED 2026-08-14** -- `docs/tfs3-spec.md` is the format as shipped,
`docs/tfs3-design.md` the decision record. The prose below is kept as
the design-time motivation (written in future tense; the forks it
poses were all decided -- no migration, `FS_MAX_FILES` retired to
TFS2's table size, `FS_PATH_MAX` still binding callers only).

TFS2 stores a flat table of up to 256 records, each keyed by a full
`char path[FS_PATH_MAX]` string (`struct file`, `tfs.c`). Directories
are implied by those path strings rather than being containers -- there
is no object representing "the file itself" separately from the name
pointing at it. That's a fair design for what it does today, and it's
also the single structural reason several ordinary things can't be
expressed:

- **Hard links** need two names for one file. A path-keyed table has
  exactly one name per record, by construction.
- **`rename()`** is a directory operation on a name; here it can only be
  copy-then-delete, which is neither atomic nor cheap for a large file.
- **Unlink-while-open** -- deleting a file something still holds an fd
  to, and having reads keep working until the last close -- needs a link
  count separate from the name. Ordinary POSIX programs do this
  routinely (it's the standard temp-file idiom).
- **`st_ino`/`st_nlink`/`st_dev`** have nothing to be derived from.

The change: split each record into a directory entry (name -> inode
number) and an inode (metadata + the block pointers `tfs.c` already
has). The block layer, the journal, and the write-ahead sequence all
stay -- this is a change to what a record *is*, not to how records reach
the disk. `rename()` becomes a single journalled metadata write, which
is the one place this makes an existing operation genuinely simpler
rather than just more capable.

Worth doing before Multi-user & file permissions (permissions) rather than after: owner
and mode bits belong on the inode, so the other order means putting them
on the path record first and moving them immediately after. While the
record layout is open, it's also the moment to add room for Milestone
32's `time_t`, raise `FS_PATH_MAX` (64) and `FS_MAX_FILES` (256), and
decide whether TFS2 v3 images migrate or just get reformatted -- the
project has already accepted "start fresh" once for `/etc` config
formats (see `docs/decisions.md`), so reformatting is a legitimate
answer here as long as it's a decision and not an accident.

Not a prerequisite for POSIX *alone* -- it's the piece POSIX compatibility
needs that no other milestone owns, but hard links and atomic rename are
worth having regardless of whether that milestone ever happens.

### Block integrity: checksums & scrubbing

`fsck` can tell you the filesystem's *shape* is wrong -- a leaked block,
a bad pointer, a dangling reference. It cannot tell you a block's
*contents* are wrong, because nothing anywhere records what they should
have been. A single flipped bit inside a file is, to every layer in this
kernel, simply the file's contents.

Placed immediately after the inode layer for a practical reason: a
per-block checksum wants to live next to the block pointer that
references it, and TFS3: an inode layer shipped that structure with the room
already reserved (inode bytes 92-95). Add
it then and it's a field; add it later and it's a second format change
with another migration.

CRC32C is the answer for the algorithm, and not only because it's the
standard choice -- `kernel/drivers/partition.c` already needs a CRC32
for GPT header verification, so this shares code with something that
exists rather than introducing a new dependency. The honest limitation
to write down: with no redundancy anywhere, detection is all this can
offer. A mismatch means "this data is wrong", never "here's the right
data" -- which still beats silently returning corruption, and is what
makes a `scrub` command worth having (finding rot while a good copy may
still exist somewhere off-machine is the entire value).

Testing this is unusually tractable: the writer tools already
injects deliberate, precisely-known corruption for `fsck`. A
`--flip-bit` mode is the same idea one layer down.

### Multi-user & file permissions

New milestone, lightly scoped. toy-os is single-user with no concept of
"who owns this file" today -- `struct file` (`tfs.c`) has no owner/mode
fields at all. Wants TFS3: an inode layer's inode layer first (see above). A
first rough breakdown:

- A minimal user/group model -- a small, probably `/etc`-config-backed
  (see `etc_config.h`'s existing pattern) table of users, not a full
  `/etc/passwd`-equivalent to start.
- Per-file owner + permission bits -- on TFS3 the inode already
  RESERVES the room (bytes 92-95, earmarked uid u16 + mode u16 at
  TFS3: an inode layer), so filling it is not even a format bump; TFS2 would
  need a record-layout version bump and is the awkward case, probably
  answered with "permissions are a TFS3 feature" + a caps bit.
- Permission checks in `fs_ops` calls -- `vfs.c`'s dispatch layer is the
  natural enforcement point (one place, every backend benefits), rather
  than duplicating checks in `tfs.c`.
- A login prompt -- even if the default (and only) account needs no
  password yet, having the concept in place makes every later step of
  this milestone meaningful instead of theoretical.

### Encryption at rest

Placed after multi-user rather than with the storage work, because the
two need the same machinery: deriving a key from a passphrase and
verifying a password are the same slow-hash problem, and building it
twice would be the `k_path_resolve` mistake again -- two
implementations that disagree in ways nobody notices until they do.

**This is the one milestone where "it runs" tells you nothing about
whether it's correct.** An AES implementation with a subtly wrong round
key still produces confident-looking ciphertext, encrypts and decrypts
consistently, and passes any end-to-end test written against itself --
while being worthless. Published test vectors, as KTESTs, are not
optional here the way they'd merely be nice elsewhere. That's the real
reason this milestone is scoped as "primitives first, plumbing second".

Design intent is full-volume encryption *below* the filesystem, so TFS3
stays completely unaware of it -- the same layering argument as
`fs_ops`: the block device is a driver, what runs on top of it isn't. A
per-block tweak derived from the block number is the one cryptographic
subtlety that can't be skipped; without it, identical plaintext blocks
encrypt identically and the ciphertext leaks the shape of the data.

And write the threat model down plainly, because encryption invites
overclaiming: this protects a powered-off disk image. It does nothing
against anything with access to the running machine, and a toy OS with
no memory protection between the kernel and its own apps should say so
rather than imply otherwise.

### Desktop visual polish

Basic image support (a JPEG or similar decoder, plus a way to blit a
decoded image into the framebuffer) -- the prerequisite for real wallpaper
images and window-chrome visual polish, see `docs/decisions.md` for that
discussion.

Real wallpaper images for the desktop background (`apps/wm/desktop.c`
currently fills a plain color) -- blocked on the image decoder above.

~~Desktop icon repositioning/dragging~~ -- done, see the commit that added it: each icon now has real per-icon {col, row} state
(`apps/wm/desktop.c`'s `icon_col`/`icon_row`), draggable via a reusable
icon-grid + drag-session widget (`apps/ui/ui_icon_grid.h`) built with a
future file manager's icon view (Desktop productivity apps) as a second caller in
mind, not desktop-only. Positions persist across reboot in
`/etc/desktop.conf`, keyed by app name.

Per-icon desktop context menus (Rename/Properties/etc) -- needs icons to
have real per-icon identity/state beyond "which registry index" first;
see `docs/decisions.md`.

More compositor work beyond `gfx_present()`'s dirty-pixel blit and the
cursor-sprite save/restore path -- partially done now, see
the commit that added it: `apps/wm/wm_render.c` computes a
SCENE-level damage region each frame (comparing each window's last-
rendered rect/visibility to its current one, plus explicit reports from
`bring_to_front()`/`close_window()`/desktop icon drag/keyboard input to
the focused window) and clips the whole repaint pass to it via a new
`gfx_set_clip_rect()` (`kernel/drivers/gfx.c`) -- a window move, resize,
open, close, minimize, restore, or z-order change no longer touches the
full screen. Redrawing everything within the damaged region, back-to-
front, is what makes this correct for overlapping windows without
needing separate "what got exposed" tracking -- confirmed directly via
QMP (dragged one window off another, the revealed area repainted
correctly with no stale pixels; see
`screenshots/2026-08-12/compositor-exposure-after-drag.png`). Two real
bugs found and fixed during that testing, not just inspection -- see
`docs/decisions.md`: the taskbar strip wasn't included in a window's
own damage (a stale taskbar button survived a close until the next
unrelated full repaint), and a thin sliver of a desktop icon's
selection-highlight (which draws 4px above the icon's own y) was left
behind mid-drag by a damage strip anchored exactly at that y with no
margin.

Phase 3 (skipping a window's draw call entirely, not just its pixel
writes, when it doesn't intersect the damage region) is done now too --
see the commit that added it: `wm_render_frame()`
(`apps/wm/wm_render.c`) tests each visible window's rect against the
frame's damage box and skips `draw_window_chrome()`/`on_draw()`/
`draw_resize_grip()` entirely for one that doesn't overlap, rather than
calling them and letting `gfx_put_pixel()` clip their writes away.
Doing this precisely enough to matter surfaced one real latent bug in
Phase 1+2's own damage reporting -- `bring_to_front()` (`apps/wm/wm.c`)
was only damaging the newly-promoted window's rect, not the
previously-frontmost window's, even though that window's titlebar tint
(focused blue vs. unfocused gray) changes too. Harmless before Phase 3
(that window's chrome still got called, just clipped away outside the
old damage box, and its tint pixels happened to be inside it anyway in
every case tested), it became a real visible bug the moment the call
itself started getting skipped -- fixed by damaging the
previously-frontmost window's rect too. Verified via QMP: opened two
non-overlapping windows, swapped focus between them via taskbar clicks
and confirmed both titlebar tints update correctly every time; dragged,
minimized, and closed windows and confirmed no stale pixels or missed
redraws anywhere -- screenshots in
`screenshots/2026-08-12/compositor-phase3-*.png`.

Still open: menu/taskbar-content-click/dialog redraws still fall back
to a full-screen repaint (no damage reported for those yet) -- a
natural next step, not attempted this round.

### A layout engine for the GUI

Every widget position in `apps/` is arithmetic somebody worked out by
hand. That's why windows can't be resized: the numbers are baked into
each app's draw function, so there's nothing to recompute. It's also
why `docs/decisions.md` has an entry about `gfx_draw_string()` not
clipping, and a follow-up about the same bug one axis over -- both are
what happens when a widget's size is a constant rather than something
derived from its content.

Placed before the apps that would use it (Desktop productivity apps's file manager,
control panel and image viewer) for the obvious reason: writing three
more apps' worth of hand-computed rectangles first, then converting
them, is strictly more work than having the engine first.

Scope is a box model, not a browser. Stack children in a direction with
spacing and padding, let a child grow into leftover space, propagate
minimum and preferred sizes up from the leaves. The piece that makes it
real is teaching `apps/ui/` widgets to *report* a preferred size instead
of being handed a rectangle -- today the caller decides and the widget
obeys, which is precisely the coupling that prevents resizing.

Two decisions worth making deliberately rather than by accident. **A
window's minimum size should fall out of its content's minimum**, not be
a guessed constant, or resizing just moves the clipping bugs somewhere
new. And **immediate-mode versus retained**: the WM already redraws from
scratch each frame, so recomputing layout each frame is the option that
matches how everything else here works -- worth confirming that's still
true when the time comes rather than assuming.

Convert Calculator first. Its grid is pure arithmetic today, it's
`multi_instance` so two windows can be compared side by side, and if the
engine can't express a uniform button grid it can't express anything.

### Runtime font loading & text metrics

The layout engine above needs to ask "how wide is this string?" and get
a true answer. Today the only honest answer is "character count times a
fixed cell width", because the console and every widget assume one
character is one fixed-width cell -- an assumption baked in deep enough
that `docs/decisions.md` has an entry about it (the Latin-1 choice)
and UTF-8 migration exists to revisit it.

That makes this the natural next step after layout, and it's why the two
are adjacent: proportional text without a layout engine has nothing to
inform, and a layout engine over monospace-only text is measuring
something it doesn't need to measure.

The font data itself is currently baked at build time by
`tools/genttf.py` into `kernel/drivers/font_ttf.c` -- 11,800+ lines of
generated glyph data, and a fixed set of characters. Loading a TTF from
disk at runtime replaces the *source* of glyphs, not the rendering; the
baked font stays as a guaranteed fallback, because a console that can't
draw text until a disk font loads is a console that can't report why the
disk font didn't load.

Deliberately bounded: this milestone stops at advance widths, kerning
pairs and multiple faces. Complex-script shaping -- bidirectional text,
ligature substitution, combining marks -- needs UTF-8 migration's UTF-8
work first, and pretending otherwise would put a dependency here on
something 16 milestones below it.

### Desktop productivity apps

File manager app -- needs a proper filesystem API surface first
(list/stat/create/delete as real syscalls or a library layer, not the
fixed ad hoc calls the shell uses today), then the app built on top of
that. Its icon view can reuse `apps/ui/ui_icon_grid.h` (built for exactly
this, see Desktop visual polish's entry above) for cell geometry and drag-to-
reposition instead of re-deriving that math.

That filesystem API surface also needs seek: today's ring-3 file I/O
(`SYS_OPEN`/`SYS_READ`/`SYS_WRITE`/`SYS_CLOSE`, `kernel/include/abi/syscall_abi.h`)
is open-then-sequential-read-only -- no `SYS_SEEK`/lseek-equivalent
exists anywhere, and a file fd's `SYS_WRITE` always appends rather than
writing at a caller-chosen offset. Random access matters for more than
just a file manager -- e.g. reading a WAD file's lump directory (see the
Backlog's Doom entry) needs seeking to arbitrary offsets, not just
reading a file start-to-finish.

Desktop calendar: a small popup panel above the taskbar, opened by
clicking the clock, showing a month grid (view-only, no events yet) --
built as a reusable `widget_calendar` piece the same way
`widget_scrollback`/`widget_button` are, so any future app can embed it
too.

Control panel window with pluggable "applets" (Windows-style) -- first
applet: display settings (font size + color theme), since both already
exist as the `fontsize`/`color` shell commands, so the applet is mostly a
GUI wrapper around logic that's already implemented and tested.

### GUI clipboard + drag-and-drop

New milestone, lightly scoped. Placed after Desktop productivity apps since a file
manager gives drag-and-drop its most natural first real use (dragging a
file onto Notepad). A first rough breakdown:

- System clipboard -- a small kernel-space buffer (`apps/wm/`-level, not
  per-app) plain text lives in until something pastes it; Ctrl+C/Ctrl+V
  wired into whichever widget currently has focus.
- Paste into Notepad/Terminal -- the first two real consumers, both
  already text-input-capable via `ui_textbox`/`text_scrollback`.
- Drag-and-drop between windows -- extends `wm_input.c`'s existing
  mouse-drag handling (already used for window moves/resizes) with a
  "carrying a payload" state.
- Drag a file from the file manager into Notepad -- the first real
  cross-app use of the mechanism above, once Desktop productivity apps's file manager
  exists.

### Runtime + interop

Inter-process IPC (message passing) -- today's ring-3 processes are
isolated from each other with no way to communicate.

A real C library on top of `filetest`'s fd-aware syscalls: CRT0
(argc/argv from the initial stack -- partially there already,
`elf_build_argv_on_stack()`/`process_run_ring3_args()`, `userland/bin/ls.c`
is the one existing caller), TLS (FS.base), FPU/SSE context-switch
save/restore, and malloc/free -- none of which exist yet. `SYS_SBRK`
(`kernel/include/abi/syscall_abi.h`) is the only allocator-adjacent syscall
today, and it's grow-only (no shrink/free) and explicitly documented as
"legacy-single-process-only" (`kernel/proc/syscall.c`) -- no userland
code anywhere builds real malloc/free semantics on top of it. TLS in
particular is also why `kernel/lib/stack_protector.c`'s stack-canary
guard uses `-mstack-protector-guard=global` instead of GCC's normal
TLS-based default -- confirmed directly, not theoretical, see
`docs/decisions.md`.

A FAT16/FAT32 driver -- real interop with other OSes' tools and USB
drives, distinct from the AHCI/SATA item (that's the controller; this is
the on-disk format).

`g_next_kernel_rsp` reentrancy fixed properly, so a real blocking syscall
doesn't need to spin-poll from ring 3 the way `echotest` does today.
(Partially addressed already -- `isr_in_progress()`/`isr_reset_depth()`,
see `docs/decisions.md`, cover the DMA-wait case Async I/O to the desktop relies on --
this item is the general fix.)

`wintest` (`SYS_WIN_*`) windows made non-modal, sharing scheduler time
with the kernel-space window manager instead of taking the CPU
exclusively. Needs the scheduler to give the kernel-space WM loop and a
scheduled ring-3 process fair turns -- `scheduler_tick()` currently only
resumes kernel-space code when nothing is `READY`, and once any process is
armed, kernel-space code doesn't get scheduled again until every process
exits. Also: mouse input isn't piped to ring 3 at all yet, so `wintest`'s
close button is drawn but not clickable.

### Real mount points

`vfs.c` dispatches every call to one ACTIVE backend (`g_fs`) -- since
TFS3: an inode layer selected at boot by a superblock probe over two compiled-in
backends (TFS3 and TFS2), not a compile-time constant, but still one at
a time; `docs/decisions.md` explains why a mount table stayed out of
scope. What sets this milestone off is wanting two filesystems READABLE
at once -- FAT (Runtime + interop) and USB mass storage (USB) both
produce that need. Two head starts already exist: the probe loop is the
natural place a per-partition iteration slots in, and TFS3 is fully
volume-relative behind a `{base_lba, sector_count}` seam
(`docs/tfs3-design.md`'s "Volumes and partitions"), so mounting it from
a partition needs no backend change. (TFS2 stays absolute/flat-only.)
The per-backend-statics warning below applies to `tfs3.c` exactly as it
does to `tfs.c`.

The work is a mount table (longest-matching path prefix -> backend), path
resolution that consults it, and `mount`/`umount` commands. The honest
first proof isn't FAT: it's mounting a *second TFS3 image* at `/mnt`,
because that isolates "does dispatch-by-prefix work" from "does the new
filesystem driver work". Only then is FAT read-only mounting a
meaningful test.

Watch for: relative paths (`cd` across a mount boundary), `fs_list()` on
a directory containing a mount point, and the fact that several `tfs.c`
statics (scratch buffers, the bitmap) are per-*backend* state that a
second instance of the same backend would need its own copy of. That last
one is the real work, and it's worth knowing before starting rather than
discovering at the halfway mark.

### Observability

Every debugging tool this project has is either a print statement or an
external debugger. `dmesg` is genuinely good, the `debug` flags are
genuinely useful, and GDB works -- but there's no way to ask the running
system what it's *doing*, only to have anticipated the question in
advance and left a `klog_write()` there.

Four pieces, roughly independent:

- **Panic backtraces.** `CFLAGS` already carries `-g`, so the kernel
  binary has real DWARF. A panic that prints `kernel_main ->
  fs_write -> persist_record` instead of a bare RIP would have saved time
  on more than one bug in this repo's history.
- **A `/proc`-style tree.** Read-only introspection exposed through the
  VFS, so `cat /proc/meminfo` works with no new syscall surface. This
  gets much easier after Real mount points (it's a filesystem backend that
  synthesizes its contents).
- **A sampling profiler.** The timer interrupt already fires 100x/second
  and already has a stack to look at.
- **Counters.** Cache hits, DMA retries, allocation failures -- the
  things currently inferable only by turning on a debug flag and reading
  a wall of text.

### AHCI/SATA driver

Today's `ata.c` depends on the legacy IDE controller real modern hardware
increasingly lacks. Moderate step up from `ata.c`, not a new paradigm:
still "one drive, DMA + interrupt," just MMIO-based (mapped ABAR, BAR5)
instead of fixed I/O ports, with a richer per-port command/FIS format
instead of `select_lba()`'s register writes. Reuses existing
infrastructure throughout (`pci.c` enumeration, `pmm_alloc_contiguous()`
for the command list/FIS-receive area, `irq_register_handler()` for
completion). Broken into steps, each with its own pass/fail so a session
can stop at any boundary with something real proven:

1. PCI discovery + ABAR mapping -- find the controller (class 0x01,
   subclass 0x06), map BAR5's MMIO region, read the HBA's
   capability/global registers. Test: `lspci`-adjacent output shows the
   controller found, with version and implemented-port-count fields read
   back correctly.
2. Port detection -- enumerate implemented ports (PI register), read each
   one's SIG/SSTS.DET to find which actually have a drive attached. Test:
   a diagnostic command lists detected ports and per-port drive presence,
   matching whatever QEMU was launched with (`-device ahci` + an attached
   drive).
3. Bring up one port -- allocate its command list + FIS-receive area,
   initialize the structures, start the port's command engine. Test: no
   hang/crash; the port's PxCMD register reads back "running" per the
   AHCI spec.
4. IDENTIFY DEVICE, polled (no IRQ yet) -- issue the first real command (a
   Register FIS wrapping ATA IDENTIFY), poll for completion, parse
   capacity out of the response. Test: a shell command prints the drive's
   real model string and capacity.
5. IRQ-driven read -- wire up the port's interrupt, issue a 48-bit READ
   DMA EXT, confirm completion via IRQ instead of polling. Test: read a
   known sector (e.g. the TFS2 superblock) and byte-compare against the
   same sector read via legacy `ata.c`.
6. IRQ-driven write + read/write parity with `ata.c`'s public API -- same
   shape as `ata_read_sector()`/`ata_write_sector()` so `tfs.c` doesn't
   need to change to use either backend. Test: TFS2's own boot self-test
   (`fs: selftest passed`) passes when routed through the AHCI path.
7. Multi-sector transfers + PRDT scatter-gather sized to a whole 4KB TFS2
   block in one command (matching `FS_BLOCK_SECTORS`). Test: `stress <mb>`
   passes end-to-end over the AHCI path, including the write-batching from
   Storage hardening -- confirms the new driver behaves correctly under real
   sustained load, not just a handful of manual reads.
8. Backend selection + fallback -- `fs.c` prefers AHCI when a controller's
   found at boot, falls back to legacy IDE, then RAM-only, same fallback
   spirit `fs.c` already has. Test: boot once against a legacy-IDE-only
   QEMU invocation and once against an AHCI one, confirm `dmesg` shows the
   correct backend chosen each time.

### NVMe / modern storage

New milestone, lightly scoped. AHCI (AHCI/SATA driver) covers SATA; NVMe is
PCIe-attached and the actual default storage interface on most real
hardware sold today, so it's the natural step past AHCI rather than an
alternative to it. A first rough breakdown:

- PCIe NVMe controller discovery -- class 0x01, subclass 0x08, similar
  shape to AHCI's own PCI discovery step.
- Admin queue + identify command -- NVMe's equivalent of ATA IDENTIFY,
  but queue-based (a circular submission/completion queue pair) rather
  than a single register-poll command.
- I/O submission/completion queues -- the actual read/write path, one or
  more queue pairs (NVMe is designed for many parallel queues, though a
  first driver only needs one).
- Backend parity with `ata.c`/AHCI (AHCI/SATA driver) -- same
  `ata_read_sector()`/`ata_write_sector()`-shaped contract so `tfs.c`
  doesn't care which backend is active, matching the pattern AHCI itself
  follows.

### Data journaling & snapshots

`tfs.c`'s top comment is honest about the gap: the journal protects one
table *record* -- metadata and block pointers -- and nothing else. A
crash mid-write to a large file can leave a data block partially written,
or a pointer set before its target block's content was durable. The
record itself can't be torn, so the filesystem stays structurally sound;
the file's contents just might not be what the last write said they were.

Closing that means journaling arbitrary-sized writes, which needs a
journal that isn't one record wide, which is most of the work. Once
blocks are being written copy-on-write rather than in place, snapshots
are close to free -- and that's the argument for treating these as one
milestone rather than two: the expensive part is shared.

One consequence worth planning for: `fsck` currently treats a block
referenced from two places as corruption it refuses to repair (see
`docs/decisions.md`). Under snapshots that's the *normal* case, so the
check needs a notion of intentional sharing -- refcounts, or a snapshot
generation number -- before snapshots land, not after.

### ACPI + real power/timer

ACPI table parsing (RSDP/MADT/FADT) -- also unlocks a real software
poweroff (today's `system_poweroff()` only does the QEMU/Bochs
`outw(0x604, 0x2000)` I/O-port trick with a halt-and-message fallback,
deliberately the "works today in this exact dev/test setup" option, not a
real ACPI-based one) and is the prerequisite for discovering other CPU
cores (SMP).

**HPET as a clocksource, which is now a small job.** The registry
landed 2026-08-17 (`kernel/include/kernel/clocksource.h`), so adding
HPET is one file plus a `clocksource_register()` call -- a rating of 250
on Linux's scale, sitting between the PIT's 110 and the TSC's 300. What
it is waiting on is discovery: HPET is memory-mapped and its base
address comes from ACPI's HPET table, which is why this sits here rather
than beside the registry. (The alternative, hardcoding the conventional
`0xFED00000`, works on QEMU and essentially every PC chipset and was
deliberately not done -- it is a guess rather than a discovery, and the
machine it fails on is the one that is hardest to debug.) It also needs
its MMIO range mapped uncached, which the PAT work already provides.

**Its real value here is REACHABILITY, not resolution.** The TSC
clocksource cannot be exercised under plain QEMU at all: TCG does not
implement `invtsc` (it warns and clears the bit) and KVM withholds it
even under `-cpu host`, so the only way to run that path is
`python3 tools/vm.py --kvm --cpu host,+invtsc`. HPET works under plain
TCG. Adding it would make sub-microsecond timekeeping testable in the
DEFAULT environment -- CI and `gui_regress.py` included, where KVM is
not a given -- which is worth more than the middle rating suggests. It
would also give the CPU percentages real resolution on any machine
without an invariant TSC, where they currently round sub-tick work to
0% (see the known-issues entry).

**APIC + a clock_event_device, the other half.** Timekeeping (a counter
you read) and timer EVENTS (deciding when to interrupt) are separate
jobs, and only the first one has an interface today -- the tick is still
a fixed 100 Hz PIT interrupt, so there is no tickless idle and no
one-shot deadline. Linux calls the second half `clock_event_device`;
Windows went dynamic-tick for the same reason. This is what would let
the machine actually sleep between an animating client's frames rather
than being interrupted a hundred times a second regardless. Also a
prerequisite for SMP.

### SMP (multi-core)
Large undertaking, and a prerequisite is ACPI/MADT parsing (ACPI + real power/timer)
to even discover the other cores. Lightly sketched, not yet scoped to the
AHCI/USB level of rigor -- a reasonable first breakdown once picked up:

1. Discover other cores via the MADT's local APIC entries (needs
   ACPI + real power/timer done first).
2. Bring up application processors via the INIT-SIPI-SIPI sequence,
   starting each one in a small real-mode trampoline that gets it into
   long mode.
3. Give each core its own GDT/IDT/stack -- today's kernel assumes exactly
   one of each.
4. Make the scheduler aware of more than one core (today's
   `scheduler_tick()`/`switch_to()` assume a single running context).

### USB (keyboard/mouse)
Needs a USB host controller driver (UHCI/EHCI/xHCI, found the same way the
e1000 NIC already is, via the existing PCI enumeration) before any device
can even be enumerated. Low priority: PS/2 already covers mouse/keyboard
for every target so far, real hardware and QEMU alike. Substantially
bigger than the AHCI item above -- really two projects stacked on each
other (a host controller driver, THEN a USB device stack, THEN a HID class
driver on top of that), and xHCI (QEMU's modern default, and what most
real hardware presents) is a small scheduler in its own right -- command
ring, event ring, doorbell registers, per-device transfer descriptors --
not a simple DMA-and-interrupt device the way AHCI/legacy IDE are. Broken
into steps, each independently testable against QEMU's emulated USB
devices (`-device qemu-xhci -device usb-kbd -device usb-mouse`):

1. Host controller discovery -- find it via PCI (class 0x0C, subclass
   0x03), read the prog-if to identify UHCI/EHCI/xHCI. Test:
   `dmesg`/`lspci`-adjacent output names the right controller type,
   matching what QEMU was launched with.
2. Bring up ONE controller type (xHCI recommended, since it's QEMU's
   default and closest to real hardware; UHCI is dramatically simpler if
   the goal is first proving out the general model) -- reset it, read its
   capability registers, get it to a "running" state. No device-facing
   work yet. Test: controller reports running without hanging; capability
   register values match what's documented for QEMU's emulated
   controller.
3. Root port + device detection -- notice a device plugged into a root
   port, read its connect/enable status and negotiated speed. Test: a
   diagnostic command reports "device detected on port N" when QEMU's
   emulated `usb-kbd`/`usb-mouse` is attached.
4. Control transfers + enumeration -- implement the endpoint-0
   control-transfer pipeline (SETUP/DATA/STATUS, or the equivalent TRB
   sequence for xHCI), issue GET_DESCRIPTOR (device), SET_ADDRESS,
   GET_DESCRIPTOR (configuration), SET_CONFIGURATION. Test: the real
   vendor/product ID and descriptor strings read back match QEMU's
   emulated device identity, printed via `dmesg`.
5. HID boot-protocol interrupt transfers -- schedule a periodic interrupt
   IN transfer on the HID interface's endpoint, receive raw 8-byte
   boot-protocol reports (the fixed, simple report format -- not full HID
   report-descriptor parsing, a much bigger and unnecessary scope for
   "keyboard and mouse work"). Test: a debug command dumps raw report
   bytes when a key is pressed / the mouse is moved in the QEMU window.
6. Keyboard integration -- translate boot-protocol keyboard reports into
   real key events (feeding the existing `keyboard.c` model, or a new
   parallel event queue if that's cleaner). Test: type in a running shell
   using only the emulated USB keyboard and get correct characters,
   including modifier keys (Shift/Ctrl).
7. Mouse integration -- same for the mouse boot protocol (movement deltas
   + button bits), feeding `mouse.c`'s existing state. Test: move/click
   with the emulated USB mouse and see the GUI cursor respond, same as it
   does today via PS/2.
8. Legacy handoff -- disable BIOS/SMM's USB legacy PS/2 emulation (xHCI's
   USB Legacy Support Extended Capability) so the real driver has
   exclusive control. Test: no duplicate/ghost key events after handoff --
   a scripted keystroke sequence's event count matches expected, not
   double.

Stretch, not required for "keyboard and mouse work": hub support (device
behind a hub, not a root port), multiple simultaneous devices, anything
beyond the boot protocol (full HID report-descriptor parsing for
non-standard devices).

### Networking

A large addition, comparable in scope to the filesystem or window manager.
Most of the infrastructure it needs has zero precedent-free work left --
PCI enumeration, a real IRQ-handler registration mechanism, contiguous/
DMA-friendly physical memory, a socket-like fd abstraction + syscalls
(`SYS_SOCKET`/`SYS_SEND`/`SYS_RECV`, currently scaffolding -- always
return -1, "no transport yet"), and a real IRQ-driven DMA transfer example
are all already done (see the commit for build 390/400/410/420/470 and
`docs/decisions.md`). What's left is the actual driver and protocol stack:

Realistic path: pick a simple NIC to target (QEMU's `rtl8139` emulation is
the classic "easy first NIC driver" choice, much simpler than
e1000/virtio-net) -> IRQ registration (mechanism already exists) -> a
minimal Ethernet/ARP/IP/UDP stack before ever touching TCP (TCP's state
machine and retransmission logic only makes sense once packets can
reliably get in and out) -> TCP + the existing socket syscalls. Two
smaller gaps along the way, both confirmed to matter beyond just
networking (not scoped to TCP specifically, even though that's where
they were first identified): there's a kernel-internal tick counter
(`pit_ticks()`, `kernel/core/timer.c`) but nothing exposes it to ring 3
-- `SYS_GETTIME` is wall-clock/second-resolution only -- so a
millisecond-ish clock needs its own syscall before TCP's timers (or
anything else timing-sensitive in ring 3) can measure elapsed time at
all; and there's still no actual sleep/delay primitive once something
*can* measure time, needed for TCP's retransmission timeouts here and
already hit directly by `kernel/drivers/speaker.c`'s `beep` command,
which busy-waits on the tick counter instead of sleeping for exactly
this reason (see `docs/decisions.md`). The
driver-integration pattern itself is in good shape to build on --
`ata.c`'s probe/init-function/capability-header-through-`kapi.h`
structure, including its working IRQ-driven Bus-Master DMA path, is a
reasonable template, and the existing `*_test.c` diagnostic pattern is a
natural fit for early loopback/ARP verification -- but this is its own
multi-session project with its own milestones, not a single build bump.

### Sound

New milestone, lightly scoped. No audio subsystem exists today. A first
rough breakdown, cheapest-to-hardest:

- ~~PC speaker beep~~ -- done, see the commit that added it:
  `kernel/drivers/speaker.c`'s `speaker_beep(freq_hz, duration_ms)`
  drives PIT channel 2 + port `0x61`'s gate/data bits, exposed via a
  new `beep` shell command (a fixed 800Hz/200ms tone, "simplest
  possible output" by explicit request, not a freq/duration-adjustable
  command). Blocks for the tone's duration -- no scheduler-aware
  sleep/delay primitive exists yet (same gap as Networking's own
  item), so this busy-waits on `pit_ticks()` like everything else in
  this codebase that needs to wait a while.
- AC97 or HDA PCI audio device driver -- QEMU emulates AC97
  (`-device AC97`), the simpler of the two to target first; HDA is
  QEMU's more modern default and closer to real hardware.
- A basic mixer/volume syscall surface -- the app-facing API once a real
  device is playing samples.
- A sound-producing test app -- proves the whole path end to end, same
  `*_test.c` diagnostic-app pattern used elsewhere.

### Dynamic linking / shared libraries

New milestone, lightly scoped, deliberately placed after Runtime + interop's
real C library (a shared libc is the main reason to want this at all). A
first rough breakdown:

- A shared-object (`.so`-style) file format -- an ELF variant
  (`ET_DYN`), position-independent, distinct from today's fixed-load-
  address `ET_EXEC` binaries `elf_load()` handles.
- A userspace dynamic linker -- resolves a binary's needed libraries and
  maps them in before jumping to the real entry point; today's
  `elf_run_from_fs()` has no equivalent step at all.
- Shared libc -- the actual payoff: one mapped copy of libc instead of
  every binary statically linking its own.
- Lazy symbol binding (PLT/GOT-style) -- resolve a symbol on first call
  rather than at load time; the standard technique, not required for a
  first working version (eager binding at load time is a valid simpler
  first cut).

### Swap / paging to disk

New milestone, lightly scoped. Today's virtual memory is identity-mapped
physical RAM with no reclaim path at all -- running out of physical frames
is just a hard allocation failure (`pmm_alloc_frame()` returning 0), not
something a swap file could relieve. A first rough breakdown:

- A swap-backed page reclaim path -- pick a reclaim policy (even a simple
  one, e.g. clock/second-chance) for choosing which page to evict under
  pressure.
- Page-out under memory pressure -- write an evicted page's contents to
  the swap area, free its physical frame.
- Page-in on fault -- a page fault on a swapped-out page's now-invalid
  PTE reads it back in from swap instead of the fault being fatal.
- A swap file on TFS2 (or a raw disk region) -- needs contiguous,
  reliably-addressable disk space; a raw reserved region (like TFS2's own
  bitmap/journal areas) is simpler to start than a real swap *file*
  routed through the filesystem.

### UTF-8 migration

toy-os is Latin-1 end to end, deliberately (see `docs/decisions.md`:
one byte per character keeps the console, the font atlas, and the
filesystem's fixed-width path field all trivially indexable, and it was
enough for the six Nordic letters that motivated it). This milestone is
the exit path, for when "enough" stops being true -- a filename in a
language that isn't covered, or a text file from anywhere else.

It's a wide change rather than a deep one, which is exactly why it wants
its own milestone instead of being smuggled into a font change: decode/
encode helpers, a console that advances by codepoint rather than by
byte, a font atlas keyed by codepoint (today's is a flat array indexed by
byte value), keyboard layout files that emit codepoints, and a TFS2 path
field that becomes a byte budget rather than a character count.

The part that needs deciding early, not late: what happens to content
already on disk. A file whose name contains byte 0xE5 (`å` in Latin-1) is
not valid UTF-8, and pretending otherwise produces mojibake rather than
an error. Either the mount path detects and converts, or the version byte
gets bumped and it's a reformat like every other format change here.

### UEFI boot

The kernel boots as a Multiboot2 image via GRUB, on BIOS/CSM. That's
fine in QEMU and increasingly not fine on real hardware, where CSM is
disappearing from firmware entirely. This milestone is about the same
kernel binary being bootable both ways, not about replacing the existing
path.

Mechanically it's a different prologue for the same kernel: a UEFI stub
that acquires a framebuffer from GOP (instead of reading GRUB's multiboot
tag), gets the memory map from `GetMemoryMap()` (instead of the multiboot
memory map `multiboot.c` parses), calls `ExitBootServices()`, and jumps
into the existing `kernel_main()` with the same information in the same
shape. The interesting design question is where the two paths converge --
ideally at a small struct that says "here's your framebuffer, here's your
memory map", with everything downstream unable to tell which firmware
produced it.

Testable entirely in QEMU with OVMF, which makes this much less scary
than it sounds. See `docs/arch-portability.md` for the adjacent question
of what else is x86-64-specific.

### A scripting language

Everything toy-os runs today it also compiles: the `/bin` binaries are
built by the same Makefile as the kernel. A scripting language is the
first thing that would let the OS run a program it wasn't built with --
you write the script *on the machine*, in `edit`, and run it.

That makes it a genuine integration test of everything else. A REPL needs
TTY / virtual terminals's line discipline to be pleasant. Reading a script file
needs the file I/O syscalls (they exist). Any non-trivial program needs a
real heap in userspace (Runtime + interop) and will find whatever is wrong
with it. And it's the first program here big enough that
`-mcmodel=large`, the stack size, and the syscall error convention all
start to matter at once.

A small Lisp is the least code by a wide margin -- an interpreter fits in
a few hundred lines, and the reader is the hard part rather than the
parser. A BASIC is more period-appropriate for a hobby OS with a text
console, and its line-numbered structure sidesteps needing a real parser
at all. Either is fine; pick on taste, but pick before starting, because
the two want different internals.

### POSIX compatibility

**Read this as a capstone, not a project.** Most of what "POSIX
compatible" means is already scheduled under other names -- this
milestone exists to name the target, own the handful of items nothing
else covers, and say plainly what is *not* being pursued. Placed last
because almost everything it needs is above it.

**Already owned by other milestones**: a TTY layer with echo control and
raw/cooked (6), `fork`/`exec`/`wait`, COW, argv/envp, a growable stack
and a real `brk` (8), signals including SIGSEGV/SIGCHLD and a userspace
handler trampoline (9), pipes, redirection, job control, globbing and
environment variables (10), an inode layer with hard links, `rename` and
unlink-while-open (11), uid/gid and mode bits (12), `mmap`, a real C
library, kernel threads, time syscalls, an `errno` convention, and the
`g_next_kernel_rsp` reentrancy fix (16), dynamic linking (28), UTF-8
(30). Nothing below duplicates those; if an item here starts growing,
it probably belongs in one of them instead.

**What this milestone actually owns:**

- **The target itself, decided before any code.** Two coherent answers,
  and they lead to different kernels:
  - *Our own libc over our own syscalls* -- POSIX-*shaped*, ports get
    recompiled against it. Every syscall stays honest to this codebase's
    own design decisions (broken-down time at the ABI boundary, the
    one-active-backend probe-selected VFS, the
    fd table as it is). More work per ported program, no surprises.
  - *Linux x86-64 syscall-ABI emulation*, good enough to run statically
    linked musl binaries unmodified. A complete, real libc arrives for
    free and actual software runs much sooner -- but it binds the
    kernel's semantics to Linux's, including the places this project
    deliberately chose differently, and honest emulation is a long tail
    of syscalls rather than a fixed set. The recommendation on the table
    is the first, with the second kept as a later compatibility *shim*
    rather than the foundation -- but this is a real fork, not a
    formality.
- **SSE, and FPU state across a context switch.** `boot.asm` sets PAE,
  LME and NXE but never CR4.OSFXSR; userland builds with `-mno-sse
  -mno-sse2`; nothing FXSAVEs anything anywhere. Every real libc's
  `memcpy`/`strlen` uses SSE2 unconditionally on x86-64, so the first
  stock-compiled binary faults or silently corrupts another process's
  registers. Small, self-contained, and blocks the entire "run ported
  code" story -- worth doing early even if the rest of this slips.
- **`time_t`.** Timestamps are broken-down local `struct rtc_time` with
  no stored UTC offset, on purpose (`docs/decisions.md`), which is
  exactly the field needed to convert an existing one. The conversion
  exists since TFS3: an inode layer (`tz_rtc_to_epoch()`/`tz_epoch_to_rtc()`,
  tz.c), `fs_stat()` reports epochs on both backends, and TFS3 stores
  them natively with inode room reserved -- what this item still owns
  is the STORED UTC offset and true-UTC semantics.
- **The unglamorous syscall surface**: `lseek` (file I/O is
  open-then-sequential-read today), `dup`/`dup2` (which Shell pipes & job control's
  redirection wants anyway), `stat`/`fstat`, `getpid`, `pipe`,
  `isatty`, `clock_gettime`, and `chdir`/`getcwd` -- there is no
  per-process cwd at all right now, it lives in the shell
  (`apps/shell_path.c`).
- **`crt0`.** Every binary in `userland/` carries its own copy of two
  inline syscall stubs and its own `_start`; a real one replaces that.
- **Proof.** Build and run a program nobody working on this repo wrote.
  A test that only exercises code written to pass it proves nothing
  here -- this is the same lesson as the Nordic-character gates
  (`docs/decisions.md`), where six matching call sites were fixed and a
  seventh, worded differently, was found only by typing at the real
  keyboard.

**Deliberately not pursued, and worth writing down so it stays
decided**: formal conformance or certification (a paid process against a
test suite -- irrelevant to a hobby OS), locales, pthreads,
`select`/`poll`, terminal `ioctl` beyond what TTY / virtual terminals needs, and
shared file `mmap`. "Enough POSIX to build and run real ported C
programs" is the goal; conformance is not.

### The GUI in ring 3

**The goal**: apps as ring-3 processes talking a windowing protocol,
the way a real OS does it, instead of kernel-space C compiled straight
into `kernel.bin`.

**Three shapes were weighed. All three give ring-3 APPS** -- that is not
what separates them. The only real difference is whether the window
manager ITSELF also runs in ring 3:

- **A -- WM stays in the kernel.** Smallest change by far: the WM keeps
  calling `gfx.c`, `fs` and the keyboard driver directly, window
  syscalls get added, apps move out one at a time, every step ships.
  But ~5,000 lines of WM stay ring-0 forever (a WM bug is a panic, not
  a crashed app), and because the boundary is a pile of syscalls,
  moving the WM out later means rewriting every call site. A one-way
  door.
- **B -- WM moves to ring 3 now.** Real isolation, and what Linux does.
  But the WM currently *is* a kernel program: in ring 3 every one of
  those direct calls becomes a syscall or an IPC round trip, and it
  needs IPC, a userland C library, and a userland `gfx.c` + font before
  anything boots to a desktop again. A big-bang migration with nothing
  runnable in the middle -- the wrong shape for this project.
- **C -- build A, with B's boundary. CHOSEN.** The modularity actually
  wanted here comes from the PROTOCOL, not from the privilege level. If
  the WM and its clients only ever talk through a defined message
  protocol over a shared buffer -- never by calling into each other --
  the modules are already clean and swappable whether the server sits
  in ring 0 or ring 3. Every increment ships like A's; moving the
  server to ring 3 later becomes "swap the transport, port the gfx
  library" rather than a redesign. The extra up-front cost is one
  struct of function pointers, which is the same pattern
  `kernel/include/kernel/display.h`'s `display_driver` and the VFS
  backend probe already use here -- an applied pattern, not a new one.

**Step zero, and why it blocked everything**: `scheduler_tick()` used
to restore the kernel context only on a tick that found nothing
`SCHED_READY`, so a ready ring-3 process starved kernel code -- and
`wm_run()` is kernel code -- until every process exited. Done; see
the commit that added it and `scheduler.c`'s `ROT_KERNEL`
comment.

**What the current syscall surface is missing**, beyond that:

- **No mouse syscall at all.** The numbers stop at `SYS_CPU_INFO` (22);
  ring 3 can poll a key and nothing else. No event routing to a focused
  window either.
- ~~**No blocking wait**~~ -- done. A client parks in `SYS_WAIT_EVENT`
  and uses no timeslices at all. `SYS_READ_KEY` stays non-blocking (its
  contract is published and `echo.c` depends on it); new code should
  use the event API instead.
- ~~**One window per process at a fixed vaddr**~~ -- superseded.
  `SYS_WIN_CREATE`/`SYS_WIN_PRESENT` remain for `userland/tests/win_test.c`
  (modal, outside the window list); new clients use `SYS_WIN_REQUEST`.
- **4-process table, one 4KB stack page, no growth, no IPC, no
  `fork`/`exec`.**
- **Drawing lived in the kernel** -- half addressed. `userland/ui/ugfx.c`
  now gives a client rectangles and real anti-aliased text, with the
  font mapped read-only from the kernel's own tables (`WIN_REQ_FONT`)
  rather than duplicated. What is still ring-0-only is `apps/ui/`'s
  WIDGET set, which is what a real app like Calculator is actually
  built from -- see the widget-port item above.

**ELF loader hardening belongs here**, not to a security milestone:
`elf_load()` is never told the file's size (`elf_run_from_fs()` has it
from `fs_read()` and discards it), so `p_offset + p_filesz` is
unbounded and `load_segment()` will copy out of identity-mapped
physical memory past the buffer into a page it then maps into
userland. `p_vaddr` is unrange-checked too, so a segment can claim the
stack or heap vaddr the runner maps afterwards. `PT_INTERP` is silently
ignored, and frames already mapped leak when a later segment fails.
There are no KTESTs for the loader at all. All of that is tolerable
while every ELF is one this build produced; it stops being tolerable
the moment loading ring-3 apps is the ordinary path.

### Scheduler: blocking, priorities, classes

**Where this came from.** A user-reported desktop freeze (2026-08-17)
turned out to have three causes, and the third was structural: any
ring-3 app doing file I/O could corrupt a kernel-side filesystem
operation, because the backends keep module-level scratch state and the
kernel context is preemptible. That was fixed where the damage was. The
question it raised -- "what change stops this happening again for any
ring-3 app?" -- is what this milestone is.

**What is actually wrong today, in order of what it costs.**

1. **No blocking state.** This is the gap everything else hangs off.
   `uapp_desc.tick_ms` exists precisely because an app without a cadence
   has nothing to wait ON, so it polls; the WM waits for the disk by
   halting inside a rotation participant. Wait queues fix the CPU waste,
   make `tick_ms` optional, and stop a busy app degrading the desktop.
2. **No priorities.** The compositor and a background demo are peers.
   Every real desktop ranks the compositor above ordinary apps.
3. **The kernel context is itself a rotation participant** (`ROT_KERNEL`
   in `scheduler.c`), which is unusual, load-bearing today, and
   DISAPPEARS with The GUI in ring 3 stage 4. Strong argument not to build
   scheduler features that assume it.

**On "make the scheduler pluggable, like Linux".** Worth being precise,
because the premise is a common misreading: Linux does not swap
schedulers. It has scheduling CLASSES -- stop, deadline, RT,
CFS/EEVDF, idle -- queried in a fixed priority order and compiled in.
Pluggable schedulers were proposed repeatedly and explicitly rejected
for years; the runtime-swappable mechanism (`sched_ext`, BPF) only
landed in 6.12 and is an escape hatch for experimentation, not how Linux
schedules. So the model worth copying here is classes, not plugins.

**And a plugin interface would be actively wrong for this project right
now**, by its own standing rule: a seam with exactly one implementation
is UNVALIDATED (see `docs/decisions.md` on `struct win_transport`). A
scheduler plugin API with one scheduler behind it is that trap with more
surface area. Two concrete classes with two real users is the honest
version of the same idea; a plugin interface can come later if a genuine
third policy ever turns up.

**Suggested order:** wait queues first (biggest win, subsumes the
workarounds), then the two classes, then convert the preemption guard to
a sleeping lock. Only then consider anything pluggable.

### Backlog

VFS: multiple filesystem backends mounted at once, not just one chosen at
boot. The VFS layer supports exactly one active backend today -- a real
mount-point scheme (`/` on one backend, `/data` on another, say) is the
natural next step if a second filesystem ever actually shows up and needs
to coexist with the first, rather than replace it; deferred until then
since it's meaningfully more code (cross-mount path resolution, boundary
conflicts) for a capability nothing needs yet.

Virtio drivers (disk/net) -- QEMU's paravirtualized devices, as a modern
addition alongside the legacy ATA/e1000 paths already used.

A benchmarking harness -- so a future change that regresses boot time or a
hot path (e.g. `gfx_present()`) gets caught instead of just "feeling"
slower.

Multi-architecture support (e.g. RISC-V 64 alongside x86_64) -- assessed,
not started: roughly a tenth of the codebase is architecture-specific and
it's already well-insulated behind `kapi.h`, but boot/interrupts/paging/
port-I/O are a real per-arch project. Full breakdown, proposed
`kernel/arch/<arch>/` layout, and a phased plan in
`docs/arch-portability.md`.

Stretch: port a small classic game (e.g. Doom) as an end-to-end stress
test of real disk-hosted ELF binaries + libc, once both exist. Real
prerequisite breakdown below, from a research pass through the actual
ring-3 syscall surface (`kernel/include/abi/syscall_abi.h`, every
`userland/*.c`) rather than assumption -- the realistic target is a
`doomgeneric` (github.com/ozkl/doomgeneric)-style port, which reduces
the porting surface to implementing a handful of platform functions
(`DG_Init`, `DG_DrawFrame`, `DG_SleepMs`, `DG_GetKey`, `DG_GetTicksMs`)
around the original portable Doom source, not a from-scratch renderer.

**Already there today, confirmed directly -- no roadmap work needed:**
- Disk-hosted ELF execution (`run <name>`, Async I/O to the desktop, done) and
  static `ET_EXEC` loading with argc/argv delivery.
- Non-blocking keyboard input (`SYS_READ_KEY`, translated ASCII/`KEY_*`
  codes) -- covers `DG_GetKey` as-is.
- A private pixel buffer + present (`SYS_WIN_CREATE`/`SYS_WIN_PRESENT`)
  -- 32bpp direct RGB, up to 640x480, comfortably over Doom's 320x200.
  No indexed/paletted mode exists, but that's not a toy-os gap:
  converting Doom's internal 8-bit palette to RGB per frame is the
  port's own job, the same thing every real `doomgeneric` backend
  already does. Covers `DG_DrawFrame`.
- Basic file read (`SYS_OPEN`/`SYS_READ`/`SYS_CLOSE`) -- enough to read
  a WAD file's bytes, just not to seek within it (see below).

**Needed, maps to existing milestones (see each milestone's own entry
above for the full writeup):**
- malloc/free -- Runtime + interop (Real C library). `SYS_SBRK` exists but
  is bump-only/single-process-only; nothing builds real allocator
  semantics on top of it yet, and Doom's zone allocator needs a real
  heap (the shareware WAD alone is a few MB).
- A ring-3-readable millisecond clock + a real sleep/delay primitive --
  Networking (currently filed under Networking, but confirmed general
  -- see that milestone's own entry). Covers `DG_GetTicksMs`/`DG_SleepMs`.
- `SYS_SEEK`/lseek -- Desktop productivity apps (Real filesystem API surface). A WAD
  file is a directory of lumps at arbitrary offsets; today's file I/O
  is open-then-sequential-read only.
- A larger/growable user stack -- `fork()`/`exec()`-style process model (process model). Real,
  call-heavy C code against a single fixed 4KB page is a genuine risk,
  though untested whether Doom's actual stack depth would exceed it --
  flagged as "verify with a real answer" rather than an assumed blocker.

**Explicitly NOT required, despite sounding related:**
- Dynamic linking / shared libc (Dynamic linking / shared libraries) -- Doom can ship as one
  statically-linked ELF, same as every userland binary today.
- A real audio device (Sound's AC97/HDA item) -- a first port
  can ship silent, or use the already-done PC speaker `beep` for
  simple cues; digital sound is optional, not a blocker.
- `wintest` non-modal support (Runtime + interop) -- Doom can run in
  exclusive/fullscreen mode the same way `gui` mode already takes over
  the screen; not needing to coexist with other windows for a first
  version.

---

Deliberately left out as out of scope for toy-os: an own bootloader (GRUB
is fine here), a self-hosted C compiler, and additional CPU architectures
beyond the RISC-V backlog item above (toy-os is x86-64-first by design,
per the project description). [brutal-org/brutal](https://github.com/brutal-org/brutal)'s
own roadmap has all three as goals -- not goals here.

---

## Legend: the old milestone numbers

Milestones are named rather than numbered as of 2026-08-18 (see
[roadmap.md](roadmap.md) for why). This table is ONE-WAY and frozen: it
exists only to resolve `Milestone N` references in things that cannot be
rewritten -- git history, the changelogs, older source comments.

It is not an index, nothing is maintained against it, and a new
milestone does not get a number. If a reference does not appear here it
predates one of the three renumberings the old scheme went through, in
which case match it by TITLE against the list instead.

| Old number | Milestone |
|---|---|
| 1 | Async I/O to the desktop |
| 2 | Memory protection hardening |
| 3 | Storage hardening |
| 4 | Kernel test harness |
| 5 | Benchmark suite |
| 6 | Fuzzing & property-based testing |
| 7 | TTY / virtual terminals |
| 8 | Demand paging & shared memory |
| 9 | `fork()`/`exec()`-style process model |
| 10 | Signals & process control |
| 11 | Crash reporting & postmortem debugging |
| 12 | Shell pipes & job control |
| 13 | Init & service supervision |
| 14 | In-OS documentation |
| 15 | TFS3: an inode layer |
| 16 | Block integrity: checksums & scrubbing |
| 17 | Multi-user & file permissions |
| 18 | Encryption at rest |
| 19 | Desktop visual polish |
| 20 | A layout engine for the GUI |
| 21 | Runtime font loading & text metrics |
| 22 | Desktop productivity apps |
| 23 | GUI clipboard + drag-and-drop |
| 24 | Runtime + interop |
| 25 | Real mount points |
| 26 | Observability |
| 27 | AHCI/SATA driver |
| 27a | virtio, and a real GPU driver |
| 27b | other emulated hardware worth claiming |
| 28 | NVMe / modern storage |
| 29 | Data journaling & snapshots |
| 30 | ACPI + real power/timer |
| 31 | SMP |
| 32 | USB |
| 33 | Networking |
| 34 | Sound |
| 35 | Dynamic linking / shared libraries |
| 36 | Swap / paging to disk |
| 37 | UTF-8 migration |
| 38 | UEFI boot |
| 39 | A scripting language |
| 40 | POSIX compatibility |
| 41 | The GUI in ring 3 |
| 42 | Scheduler: blocking, priorities, classes |

---

# Detail moved out of the roadmap (2026-08-18)

The roadmap became one line per item, ordered by what has to be built
first. Everything it used to carry inline is below, under the same
milestone headings -- the reasoning, the measurements, the
reproductions, and any item whose full text did not fit on one line.

## Demand paging & shared memory

**Build STEP 1 first, and specifically its first two items: a per-process
region list, then `mmap(MAP_ANONYMOUS)`/`munmap` over it.** Everything
else in this milestone is written in terms of that structure, and
nothing else here can start without it.

*Listed in BUILD ORDER, not as a wish list -- reordered 2026-08-18 after
the first two items landed and made the dependencies visible. Two things
the old list hid: everything below the first heading needs per-process
address-space bookkeeping that does not exist yet, and everything under
"sharing" needs a per-frame refcount that also does not.*

**Done, and what they proved**

**STEP 1 -- address-space bookkeeping.** The keystone, and the only
item here with no prerequisite left. Today
`uheap_fault()` answers "is this address yours?" by testing ONE
hardcoded range, which is exactly enough for a single grow-only heap and
cannot express anything else. Everything after this section is written
in terms of the structure this adds.

**STEP 2 -- file backing.** Needs step 1: a region has to record what it
is backed BY before it can be backed by anything.

**STEP 3 -- sharing.** Needs step 1 for the regions and, before any of
its own items, a per-frame refcount.

**NOT PART OF THE SEQUENCE -- independent, small, and found while
measuring it.** Either can be done at any point, including first.

**Items, in full.**

- [x] ~~Page-fault-driven mapping (allocate on first touch, not up front)~~ DONE 2026-08-18, for the ring-3 HEAP. `SYS_SBRK` reserves and maps nothing; the frame arrives on touch

- [x] ~~A page-fault handler that can tell "this address is legitimately unmapped, map it now" from "this is a real fault"~~ DONE 2026-08-18 -- and the finding to carry into everything below is that the FAULT HANDLER IS NOT THE ONLY ENTRY POINT: ring 0 walks page tables rather than dereferencing user pointers, so a syscall handed an untouched buffer never faults at all. It is a registered hook with three callers; see `docs/decisions.md`

- [ ] A per-process list of mapped REGIONS with attributes (base, length, protection, backing) -- Linux's VMAs, a maple tree there since 6.1 and an rbtree before; a sorted array is plenty at this scale

- [ ] `mmap(MAP_ANONYMOUS)` and `munmap` over it. Anonymous first because it needs no filesystem and immediately gives `free()` somewhere to return memory to, which `sbrk` structurally cannot

- [ ] A `pmap`-style command showing one process's mappings. **Build it WITH this, not after** -- it is how the rest of the milestone gets debugged, and the current list had it last

- [ ] Accounting: resident vs. mapped, visible in Task Manager. Falls out of the same structure -- "mapped" is the region list, "resident" is the page tables, and having both is what makes the difference observable at all

- [ ] Lazy zero-filling: one shared zero page mapped read-only until first write. A small optimisation ON the anonymous path, so it wants the anonymous path to exist first

- [ ] **Note the hazard before starting:** a fault can happen inside a syscall, and this filesystem is NOT re-entrant (`vfs.c` holds a preemption guard, and `fs_read()` refuses a nested whole-file read). Reading a page from disk on the fault path is therefore a re-entrancy question, not just an I/O one

- [ ] Shared read-only text pages between instances of the same binary -- the first real payoff, and the cheapest sharing case because read-only needs no copy path

- [ ] **A per-frame reference count.** Nothing in this kernel has one. `PAGE_BORROWED` says "somebody else frees this", NOT "count me", so it cannot express two owners -- which is what every item below needs. Roughly what Linux's `struct page` array buys, and the same structure the one-directional `meminfo audit` would need to find ordinary leaks

- [x] ~~A frame-size bound for ring 3~~ DONE 2026-08-18 (`-Wframe-larger-than=2048`). It found four oversized frames the day it landed, the worst at 20,608 bytes against a 16 KiB stack -- past the guard page entirely. Still open underneath it: the guard is ONE page, where Linux uses 256

- [x] ~~A check on the ~1 MiB between a ring-3 image and its heap~~ DONE 2026-08-18 -- an `ASSERT` in `userland/rt/link.ld`, the only place that knows how big an image actually got

## More than 4 GiB of RAM

*Measured 2026-08-18, while raising the per-process heap. This is not a
constant to raise -- it is a structural property of how this kernel
reaches physical memory, and it is worth stating precisely before
anyone tries.*

**There is no higher-half kernel and no separate kernel address space.**
`boot.asm` identity-maps the low 4 GiB with 2 MiB pages before long
mode, every user PML4 shares kernel entry 0, and ALL physical access
goes through that map: `vmm.c` reads page tables as `table_at(phys)`,
and the copy helpers reach user frames through it (which is exactly what
makes SMAP absolute here, with no STAC/CLAC window). So a frame above
4 GiB would have no kernel virtual address at all -- unreachable, not
merely unallocated.

**Items, in full.**

- [ ] A direct map that is not the identity map -- Linux's `__va`/`__pa` at `0xffff888000000000`, i.e. a higher-half kernel. This is the real work; everything below is bookkeeping behind it

- [ ] **`kfree()`'s red-zone detection depends on heap pointers fitting in 32 bits.** It tells a red-zoned block from a plain one by reading the eight bytes before the payload -- unambiguous only because a heap pointer's top half is zero while the magic's is not. A kernel heap above 4 GiB breaks that SILENTLY, on the freeing path. See CLAUDE.md and `docs/decisions.md`

## Swap / paging to disk

**Needs:** Demand paging & shared memory -- swap is demand paging with a backing store.

## SMP

**Needs:** Scheduler: blocking, priorities, classes (a scheduler that can block), ACPI + real power/timer
(ACPI, to enumerate CPUs and program the APIC).

## Layer 2 -- Core kernel services (syscalls, processes, IPC)

The contract userland is written against. Changing it later means
changing every program that uses it, which is why these come before
anything that would.

**Items, in full.**

- [ ] A real spinlock primitive, plus an audit of everything currently assuming single-threaded: `tfs.c`'s static scratch buffers, `heap_core.c`'s free list, `vga.c`'s cursor state

## Scheduler: blocking, priorities, classes

Today's scheduler is a preemptive round-robin over `procs[]` with the
KERNEL CONTEXT as one of the rotation participants, no priorities, and
**no blocking state at all**. Nothing can sleep until an event; it can
only spin. Everything below follows from that one gap, and the staging
matters more than the individual items -- see the Details entry.

Prompted by a real bug (2026-08-17): a ring-3 app doing ordinary file
I/O could freeze the desktop for seconds and silently corrupt
kernel-side filesystem operations. Both were fixed at the point of
damage (a preemption guard in `vfs.c`, `fs_read_into()`, a lost-wakeup
race in `ata.c`), and none of those fixes address the shape that let it
happen.

**Sequencing:** do this AFTER the GUI's move to ring 3, stage 4. Moving the WM to
ring 3 deletes the scheduler's strangest case -- the kernel context as a
rotation participant (`ROT_KERNEL`) -- and it would be a waste to design
priorities and wait queues around a participant that is about to stop
existing.

**Items, in full.**

- [ ] **Blocking + wait queues.** The big one, and it subsumes several current workarounds. A process waiting on a timer, a pipe, a window event or the disk should be OFF the run queue until the thing it waits for happens.

- [ ] **Retire `uapp_desc.tick_ms` as a REQUIREMENT.** It exists because an app with no cadence otherwise polls with `sys_yield()` at full speed; Control Panel omits it and burns 100% of every slice it is given. With wait queues it becomes an optimisation rather than the difference between a well-behaved app and a spinning one.

- [ ] **Two scheduling classes, Linux-shaped.** A compositor should outrank a background demo; today they are peers, which is why an actively-working app measurably degrades the desktop. Classes queried in priority order (realtime-ish, then normal), NOT a plugin interface -- see the Details entry for why.

- [ ] **Replace the preemption guard with a real sleeping lock.** `scheduler_preempt_disable()` (added 2026-08-17) is a blunt critical section: correct, and it blocks EVERY process for the duration of a filesystem operation. A lock that sleeps the contender is the right shape once there is a wait queue to sleep it on.

- [ ] **Bound how long a frame can block on I/O.** Related but separate: see the "Get blocking disk I/O out of the WM's event loop" item under Known issues.

## Signals & process control

**Items, in full.**

- [ ] **Ctrl-C interrupting a running program**, the way it works in a Linux shell. Broken out because it is the signal feature people actually miss, and because the prerequisite chain turns out to be SHORTER than this file assumed -- see the Details entry below. The requirements, smallest first: - [ ] `SYS_KILL(pid, sig)` -- a ring-3 process can signal another. Needs a per-process pending-signal field on `struct sched_process`, which `scheduler_tick()` checks. - [ ] A default disposition of TERMINATE, applied at a safe point (the next tick or syscall return, never mid-handler), which must tear the process down through the SAME path a normal exit takes -- zombie + exit code -- so the parent's `scheduler_poll()`/wait sees a result rather than a vanished pid. - [ ] A distinguishable exit status, so a shell can print "Interrupted" rather than reporting a clean exit. - [ ] The foreground concept. **This does NOT need the TTY milestone's full TTY layer** once the terminal is a ring-3 process that spawns its own children (The GUI in ring 3): the terminal knows its own child's pid, so "the foreground process" is the terminal's own state. A kernel TTY is needed for the PHYSICAL shell's Ctrl-C, and for job control (`fg`/`bg`, Shell pipes & job control) -- not for this. - [ ] Nothing in the keyboard driver. Ctrl already reaches apps as a control code (0x03), see `keyboard.h`'s "Ctrl and Alt" note -- a ring-3 terminal receives `^C` as an ordinary key event today and simply has nothing to do with it yet. - [ ] Userspace handlers (a `signal()`-style trampoline that returns through the kernel) are explicitly NOT required for Ctrl-C and should not gate it -- terminate-by-default is the whole behaviour most programs want.

## `fork

**Needs:** Demand paging & shared memory (shared memory / copy-on-write is what makes
`fork()` cheap rather than a full copy).

**Items, in full.**

- [x] ~~Hardware floating point / SSE for ring-3 processes~~ -- done (unplanned, asked for directly mid-session), see the commit that added it: `CR4.OSFXSR` enabled at boot, a 512-byte FXSAVE area per process saved/restored eagerly across a scheduler switch, `userland/` built without `-mno-sse`. Kernel and `apps/` stay FP-free, matching what Linux and Windows both actually do -- see `docs/decisions.md`. `fputest` proves it, including a concurrent two-process XMM race.

- [ ] A `kernel_fpu_begin()`/`kernel_fpu_end()` bracket, if kernel-side or `apps/`-side SIMD is ever genuinely wanted (that's how both Linux and Windows allow it). Deliberately not built yet -- no caller, and the standing rule is a mechanism arrives with its first real one.

- [ ] AVX/XSAVE support -- FXSAVE covers x87+SSE only, so an AVX-using process would silently lose its upper YMM halves across a switch. Nothing emits AVX today (userland is built for baseline x86-64), but enabling `-mavx` without this would be a real bug.

## TTY / virtual terminals

**Needs:** Signals & process control -- a terminal without signals cannot deliver
Ctrl-C, which is most of what makes it a terminal.

**Items, in full.**

- [ ] A line discipline (line editing, echo control) separate from the shell's own input loop. **Partly built ahead of this milestone**: `kernel/lib/klineedit.c` is a real, shared line editor (readline keymap, kill ring, undo) that the physical shell and the GUI Terminal both drive, so "two clients of the same editing layer" already holds. What's still missing is the *discipline* half -- it's a library each front end calls, not something they read through, and it has no echo control or raw/cooked distinction. see the git history.

- [ ] `Ctrl+C`/`Ctrl+D`/`Ctrl+Z` as terminal signals, not keystrokes an app happens to notice. The *encoding* groundwork is done -- the keyboard driver emits Ctrl as control codes and Alt as an ESC prefix, so these keys now reach an app at all (they didn't before); today `Ctrl+C` abandons the input line and `Ctrl+D` on an empty line is recognised but has nothing to exit to.

## Shell pipes & job control

**Needs:** Signals & process control (signals) and TTY / virtual terminals (the process groups
job control suspends and resumes).

**Read this as "make `tosh` a real shell".** `userland/lib/tosh.c` (Milestone
41) already runs in ring 3 and spawns programs with their output piped
back, so this milestone is no longer hypothetical -- it is the specific
list of things standing between that and something bash-shaped. Each
item below says whether it needs KERNEL work or is purely the shell's,
because that is the distinction that decides what can be done today:

**Needs new kernel support first:**

**Purely the shell's own work, doable now:**

**Needs both:**

## Layer 3 -- Storage

The filesystem is real and journalled already; what is left is
durability and structure -- integrity checking, snapshots, mount points
-- none of which the layers above can add for it.

**Items, in full.**

- [ ] **stdin redirection in `SYS_SPAWN`.** It takes an stdout fd today and nothing else, so a child can be read FROM but never written TO. `|`, `<` and any interactive child all need this, and it is the single highest-value item here -- one more argument.

- [ ] **`dup`/`dup2`-style fd plumbing**, so the shell can wire an arbitrary fd to 0/1/2 rather than the two special cases spawn hardcodes. The general form of the item above.

- [ ] **A per-process cwd.** `tosh` keeps its own, and a spawned child does not inherit it -- so `cd /bin` then `hello` finds the program only because PATH is absolute. Also listed under POSIX compatibility.

- [ ] **An environment passed to a child.** `crt0.asm` already reads `envp` off the stack per SysV; the kernel always passes an empty one. `export` cannot mean anything until a child receives it.

- [ ] Quoting/escaping, `&&`/`||`/`;`, globbing, aliases, `$?`/`$1`, and a history buffer. All parsing and string work over syscalls that already exist.

- [ ] Line editing. Worth noting `kernel/lib/klineedit.c` is freestanding and could be compiled for userland through the shared-source rule (`build/userland/shared/`) rather than reimplemented -- the same trick that keeps one arithmetic engine behind both Calculators.

- [ ] Background jobs (`&`) and `fg`/`bg`/`jobs` -- the shell tracks the table, but "which job is in the foreground" is also what Ctrl-C needs, and a background child writing to a terminal that has moved on needs the TTY layer (TTY / virtual terminals) to arbitrate.

- [x] ~~Tab completion (commands, then paths)~~ -- done, see the commit that added it (commands, paths, and per-command argument sets, shared by both shells)

- [ ] Environment variables + `export` -- note `PATH` already exists as a config key read at shell startup (`/etc/toyos.conf`, see `apps/shell_path.c`); this item is the general mechanism, of which PATH would become one instance

## Storage hardening

**Items, in full.**

- [x] ~~Full multi-GB stress run (`stress 4200` / `stress 8192`)~~ -- done, both PASSED byte-for-byte on 2026-08-13 (4200 MB in 326 s, 8192 MB in 692 s), see the commit that added it

- [x] ~~Coalesce contiguous block writes into fewer ATA commands~~ -- done, see the commit that added it (64KB DMA buffer + run coalescing + skipping the redundant zero-fill: 18 -> 25.1 MB/s write)

- [x] ~~Journal-batched flush~~ -- done, see the commit that added it (4 flushes per metadata op -> the 2 the recovery protocol actually depends on; format 0.73s -> 0.34s)

- [x] ~~Stop treating an unreadable superblock as a foreign disk~~ -- done, see the commit that added it (this was a data-loss bug, not just hardening)

- [x] ~~An fsck-style pass to reclaim leaked blocks~~ -- done, see the commit that added it (`fsck`/`fsck repair`, plus `tools/tfs2_writer.py corrupt` to test it against known damage)

- [ ] Directory index -- lookups scan linearly on both backends (TFS2: `find()` over 256 record slots; TFS3: a dirent-chain scan per component, softened by its in-RAM name cache). The on-disk index remains open.

- [x] ~~`fs_rename()`~~ -- done on both backends, plus `mv` in the shell, see the commit that added it. One journal transaction on TFS3 (files and directories, across directories, `..` and link counts included); one record edit per descendant on TFS2, non-atomic and documented as such. No atomic replace of an existing destination, on purpose -- see `docs/decisions.md`. Note the prediction in this item was WRONG in an interesting way: it did NOT fit the 4-slot transaction (a directory changing parents needs five blocks), which is what prompted TFS3 format v2's 32-slot journal and the `txn_begin()` credit reservation.

- [x] ~~`fs_truncate()`~~ -- done on both backends, plus `truncate` in the shell, see the commit that added it. Growing is sparse (metadata only); shrinking frees the tail in two phases with a commit between them, so a crash can cost a leak but never a double allocation.

- [x] ~~TRIM/discard on delete, so freed blocks are reported to the device~~ -- done on both backends (`ata_trim()` from tfs.c's `free_block()` and tfs3.c's `trim_run()`, plus `discard=unmap` on every `-drive` line); see the git history

- [ ] Per-record checksums in the table itself -- TFS2-only now: TFS3 checksums every inode at rest (verified on each read); TFS2's records still have no at-rest integrity check

## Block integrity: checksums & scrubbing

*Right after the inode layer, while that on-disk format is already
open -- a checksum field wants to be designed in, not bolted on.*

**Items, in full.**

- [ ] Pick and justify one algorithm for DATA blocks -- CRC32C is the classic answer, but TFS3's metadata checksums shipped as FNV-1a-32 (docs/tfs3-spec.md), so this item now includes deciding whether two hashes are acceptable or FNV wins by reuse. Note the kernel's only CRC32 today is plain CRC-32, static inside the GPT parser. TFS3 already RESERVES the per-group checksum table behind superblock flags bit 0, and the kernel refuses to mount unknown flag bits -- so this milestone fills a slot that exists, it doesn't redesign the format

## TFS3: an inode layer

*Same correction as Kernel test harness above: the heading claimed completion
with three boxes unchecked (unlink-while-open, raising `FS_PATH_MAX`,
and the symlink implementation). TFS3 itself is done and is the default
format; those three are follow-ups it did not include.*

*Spec as shipped: `docs/tfs3-spec.md`; design record:
`docs/tfs3-design.md`; built in five staged commits (see
the git history). TFS2 stays in the kernel as a second
probe-selected backend, with live switching via `fsformat` -- see
`docs/decisions.md`.*

**Items, in full.**

- [x] ~~Hard links (`link()`), and the `.`/`..` entries that fall out of having them~~ -- done (`ln` shell command; first optional fs_ops op, gated by FS_CAP_HARDLINKS)

- [x] ~~`rename()` as a directory operation, atomic through the journal~~ -- done, see the commit that added it. It did NOT fit the 4-slot transaction as this item predicted: a directory changing parents needs five blocks (both dirent blocks, the child's `..`, both parents' link counts), which is what prompted format v2's 32-slot journal.

- [ ] Raise `FS_PATH_MAX` (64) -- the FORMAT no longer caps anything (255-byte names, unlimited depth, ~590k inodes on 9 GiB), but every caller still holds 64-byte buffers; raising the API constant is its own audit. `FS_MAX_FILES` stays as TFS2's table size only.

- [x] ~~Room in the inode for owner/mode (for Multi-user & file permissions) and `time_t` (POSIX compatibility)~~ -- done: 128-byte inode reserves uid/mode, timestamps are epoch seconds outright

- [ ] Symlink IMPLEMENTATION (create/read, backend-internal resolve loop with an ELOOP-style hop cap) -- deliberately deferred; see the design doc's Symlinks section for where resolution has to live and why

- [x] ~~`fsck` taught to check link counts, not just block ownership~~ -- done (+ inode checksums, `.`/`..` targets, orphan reclaim, free-count recompute, backup-superblock restore on repair)

- [x] ~~A migration path (or an explicit "reformat, no migration" decision) from TFS2 v3 images~~ -- decided: no migration, and no forced reformat either -- TFS2 images keep mounting as TFS2; `make clean-disk && make iso` (or `fsformat tfs3 confirm`) is the deliberate move

- [x] ~~Host tooling for the new format~~ -- done as `tools/tfs3_writer.py` (tfs2_writer.py untouched; format chosen by magic probe everywhere -- kernel, `seed_disk.py`, `check_layout.py`)

## Data journaling & snapshots

**Needs:** the TFS3 inode layer's remaining items -- snapshots are a property of
the inode layer, not of the block layer under it.

**Items, in full.**

- [ ] Decide the durability contract explicitly: today's write-through is easy to reason about, and data journaling changes what a caller can assume after a write returns

## Encryption at rest

*After multi-user, which brings password hashing -- the key derivation
this needs is the same machinery, and building it twice would be silly.*

## Layer 4 -- Devices and drivers

Each of these is one file behind an existing registry (`display_driver`,
`block_device`) rather than new architecture. They are here because
everything above can be built and tested without them.

## virtio, and a real GPU driver

**Why virtio first, and why the GPU is the prize.** One transport layer
(virtqueues + the PCI capability/config negotiation) unlocks every
virtio device at once, which is a better return than any single
hand-written driver. `virtio-gpu` in particular has a proper cursor
plane and a resource/scanout model -- i.e. a hardware cursor that works
where the VMware SVGA one does, but through an interface that is
actually specified rather than reverse-engineered from a vendor header,
and that real hardware drivers resemble far more closely.

Measured 2026-08-13: `-vga virtio` boots this kernel fine at 1280x720
(GRUB sets a VBE mode and we use it), so the device is available to
develop against today with no bring-up risk.

**The GUI in ring 3's cursor requirement landed here (2026-08-17).** Stage
4a listed "the cursor over TWP" as a requirement and measuring it moved
it out: `DISPLAY_CAP_CURSOR` is declared only by `vmsvga`, which
disables it by default because a hardware cursor over a RELATIVE PS/2
mouse makes the pointer jump -- so the capability is unreachable on
every configuration this OS boots, and a protocol path to reach it
would have been worse than useless. It belongs with the two things that
make it work, both of which are on this list: an absolute pointer
(`virtio-input`) and a specified cursor plane (`virtio-gpu`). Until
then the compositor draws a software sprite, which is what every
reachable configuration already does. See `docs/decisions.md`.

**Items, in full.**

- [ ] **A live bug to fix when the hardware path is reachable**: the software cursor resolves four shapes (`enum wm_cursor_kind` -- arrow, horizontal, vertical, diagonal) and the hardware path uploads ONE sprite and ignores the kind, because `draw_cursor_at()` (`apps/wm/wm_render.c`) returns before `resolve_cursor_kind()` is consulted. So on a hw-cursor adapter a resize edge would show a plain arrow. Unnoticed because nothing reaches that path; reproduce with `tools/vm.py --vga vmware` once `vmsvga.c`'s `g_cursor_enabled` is turned on.

- [ ] virtio transport: PCI capability parsing, virtqueue (descriptor table / avail / used rings), notification + ISR handling. Every item below depends only on this.

- [ ] `virtio-gpu`: resource create/attach, set_scanout, transfer + flush, and the CURSOR queue -- a hardware cursor on a specified interface, replacing the vendor-specific vmsvga path as the preferred one where both exist

- [ ] `virtio-blk`: a block device that isn't ATA -- would exercise the VFS's backend seam (already exercised once by TFS3 -- see docs/decisions.md's probe-selected-backends entry) without writing AHCI first

## other emulated hardware worth claiming

Devices QEMU already presents to this machine that nothing drives yet.
Listed with the honest reason each is or isn't attractive.

**Items, in full.**

- [ ] **e1000 ethernet** (`8086:100e`) -- ALREADY on our PCI bus and visible in `lspci` today, sitting unused. Descriptor rings, no firmware blob, thoroughly documented. The single biggest capability jump available, and the prerequisite for anything networked

- [ ] **Cirrus** -- has a hardware cursor and is the simplest register interface of any of them, but measured at only 640x480 here against 1280x720 for the others. Recorded so the option isn't re-investigated from scratch; the resolution cost rules it out

## NVMe / modern storage

**Items, in full.**

- [ ] The 4KB-sector question: NVMe devices commonly aren't 512-byte, which neither TFS2's nor TFS3's on-disk assumptions (both 512-byte-sector based) have ever been tested against

## Networking

**Needs:** a NIC driver, i.e. virtio, and a real GPU driver's virtio-net.

**Items, in full.**

- [ ] Ring-3-readable millisecond-ish clock (a tick counter exposed via syscall -- today's only ring-3 time source, `SYS_GETTIME`, is wall-clock/second-resolution only)

- [ ] Sleep/delay primitive (timeouts, retransmission -- a general kernel gap, not networking-specific: also why the PC speaker's `beep` busy-waits on a shared tick counter instead of sleeping, see `docs/decisions.md`)

## Sound

## Layer 5 -- System services and policy

The first layer that is POLICY rather than mechanism, and the first that
needs processes to outlive the thing that started them.

## Init & service supervision

**Planned in detail: [docs/init-design.md](init-design.md)**, which
stages this together with the process tree it needs, the console device
a ring-3 shell needs, and retiring the kernel shell.

**Needs:** nothing outstanding for stage 2 (a TARGET setting); stages
0 and 1 are done. Stage 3 (a console device) is what the ring-3 shell
waits on. **Unlocks:**
the ring-3 settings daemon, a ring-3 shell, and any future name service.

*This item used to say it needed `fork`/`exec`, signals and job control.
That was checked against the code on 2026-08-18 and is wrong about the
first: `SYS_SPAWN` is already `posix_spawn()`-shaped, which is what an
init needs, and `fork()`'s real content is copy-on-write -- so it
depends on demand paging and sits BELOW this, not above it. Signals are
needed to stop a service gracefully, not to start or reap one, so they
gate the later checkboxes rather than the first three. The TTY is
genuinely needed, but only for the shell half; see the design doc's
staging.*

**Items, in full.**

- [x] ~~A real `init`: the first process, started by the kernel, parent of everything else~~ DONE 2026-08-18 (stage 1). `/bin/init` reaps orphans; it starts nothing yet -- that is the TARGET below

- [x] ~~A parent link (`ppid`) and reparenting of orphans~~ DONE 2026-08-18 (stage 0). Reparenting goes to 0 until init exists; `scheduler_reparent()` is the adoption half stage 1 uses

- [x] ~~pid 1 refuses to be killed~~ DONE 2026-08-18 (stage 1), by asking which pid init holds rather than testing `pid == 1` -- see `docs/decisions.md` for the boot where the difference matters

- [ ] A TARGET setting (`text` / `graphical`) deciding what init starts -- systemd's `multi-user.target` / `graphical.target`, SysV's runlevels 3 and 5 -- with a `target=` boot word overriding it so a desktop that faults on boot cannot cost you the machine

## Multi-user & file permissions

*Wants the TFS3 inode layer's inode layer first -- per-file owner/mode bits
belong on an inode, not on a path-keyed record.*

## Layer 6 -- Userland runtime

What a ring-3 program can assume exists. Every item above the GUI layer
that wants an allocator, a FILE, or a shared library is waiting on this
one.

## Runtime + interop

**Items, in full.**

- [ ] **A real C library.** Partly started: `userland/rt/crt0.asm` and `userland/rt/sys.c` (libsys) landed with the ring-3 GUI work, so a program is already just a `main()` over typed syscall wrappers. What a *libc* still needs on top of that, in dependency order: - [x] ~~crt0: `_start`, argc/argv/envp off a SysV stack, call `main()`, exit with its return value~~ -- done. - [x] ~~A syscall layer with one definition per call~~ -- done (`userland/rt/sys.h`). A libc sits ON this, not instead of it. - [x] ~~`malloc`/`free`~~ DONE 2026-08-18, the second way round: `kernel/lib/heap_core.c` compiled twice, with sbrk behind it in ring 3 (`api/heap_os.h`, `userland/lib/stdlib.h`). So it is one allocator, not two. `realloc` is still absent, and `free()` cannot return memory to the kernel until `mmap` exists -- see this file's demand-paging milestone - [x] ~~`string.h`/`mem*`~~ -- done: `userland/lib/string.h`, the C names over the same `k_*` code (one implementation, not two). `memcpy`/`memmove`/`memset`/`memcmp` are real symbols in `userland/lib/cmem.c` because GCC can emit calls to them itself; everything else is a `static inline`. See the git history, and note the `-fno-tree-loop-distribute-` `patterns` flag that now has to stay in `USERLAND_CFLAGS`. - [x] ~~`snprintf`~~ -- done: `userland/lib/stdio.h`, which is kfmt's formatter. It needed `kernel/lib/kfmt.c` split first (the `vga_printf`/`klog_printf` sinks moved to `kfmt_print.c`) so the rest could be freestanding enough for the shared-source rule. `userland/tests/libc_test.c` is its first ring-3 caller and its test. - [ ] `stdio` proper: `printf` and a buffered `FILE` layer over the fd syscalls. Buffering is the part with real design in it -- unbuffered `printf` is one syscall per call, which is worse than the `put()`-shaped code it would replace. - [ ] `errno`. Syscalls return 0/-1/a count today with no shared vocabulary for *why*; this is listed separately below and is a prerequisite for a libc that reports failures usefully. - [ ] TLS (FS.base) -- needed for a per-thread `errno` and for GCC's default stack-protector guard. This is why `-mstack-protector-guard=global` is used today, which is a real workaround rather than a preference (`docs/decisions.md`). - [ ] `atexit`/`exit` split: crt0 currently calls `sys_exit()` directly and says so. A libc interposes `exit()` to run handlers and flush stdio -- that ONE line in `crt0.asm` is the whole change, and the layering is already shaped for it. - [ ] Decide the target before building much of it: our own POSIX-shaped libc, or enough Linux syscall-ABI compatibility to run stock musl binaries. POSIX compatibility owns that decision and it changes what "done" means here. The SysV entry ABI landing already removed one obstacle to the musl route.

## Dynamic linking / shared libraries

**Needs:** Runtime + interop (an allocator and a real ELF runtime) and
Demand paging & shared memory (mapping a library into an existing address space).

## UTF-8 migration

**Items, in full.**

- [ ] Audit every `char`-sized assumption first -- this codebase has been bitten by exactly that before (see `docs/decisions.md` on the signed-char gates), and the audit is the milestone's real work

## A scripting language

**Needs:** Runtime + interop -- a scripting language with no allocator is an
exercise in avoiding one.

## POSIX compatibility

**Needs:** `fork()`/`exec()`-style process model, 10 and 24. POSIX is mostly a promise about
those three.

Mostly a *capstone* over Fuzzing & property-based testing-16 rather than new ground -- see
its Details entry for what each of those already covers and what's left
that nothing else owns.

## Layer 7 -- The GUI

Sits highest deliberately: the desktop is a ring-3 process now, so
everything here is an ordinary program's problem rather than the
kernel's.

## The GUI in ring 3

Run the desktop the way a real OS does: apps as ring-3 processes talking
a windowing protocol, not kernel-space C compiled into `kernel.bin`.
Chosen shape is **"kernel compositor, userspace-server-ready protocol"**
-- see the Details entry for the three options weighed and why this one.

**The apps half is done. The WM itself is not, and
`docs/wm-ring3-design.md` is the staged plan for finishing it** --
written 2026-08-16, five stages, each shipping on its own. Read it
before starting any item below; it costs the four kernel capabilities
that are actually missing and records what already exists (more than
this list implies -- a ring-3 process can already map the framebuffer,
and `vmm_map_user_page()` already takes an explicit address space).

**The consequence that is easy to miss:** all 20 GUI test tools drive
the WM through `apps/wm/wm_debug.c`'s `gui` commands, over the KERNEL's
serial console. A ring-3 WM cannot answer those, so the 280 checks that
are the only proof the desktop works have to move with it. The design
doc makes that its own stage, deliberately before the WM moves.
**Stage 3 did it (2026-08-16):** those commands now travel as TWP
messages over `struct win_transport`, and the whole suite passes with no
tool edited -- so the tooling has already crossed the boundary the WM
still has to.

**Stage 4's requirements are written (2026-08-17)** -- R1-R9 in the
design doc, measured from `apps/wm/`'s own call surface rather than
estimated, plus a 4a-4d sub-staging sketch and two forks settled in
`docs/decisions.md` (the WM owns the back buffer; the kernel restores
the text console when the WM dies). **There is a second easy-to-miss
consequence, and it is R5:** `debug_console_poll()` -- the drain for
that same serial wire -- is called from `wm_run()`'s loop, so while the
desktop is up the WM is what keeps the kernel's console answering at
all. Move it to ring 3 without giving ring 0 its own drain point and
every one of the 280 checks stops arriving.

**Stage 4a has started (2026-08-17): R4 and R5 are built.**
`SYS_FS_GENERATION` gives ring 3 the filesystem's change counter
(`userland/tests/fsgen_test.c`, 8 checks), and `scheduler_idle()`
(`api/scheduler.h`) makes the kernel the owner of its own idle work, so
the WM leaving ring 3 deletes a call rather than the console. Unifying
the four hand-rolled idle loops also fixed a live bug: the poll is not
re-entrant, and `dbg_dispatch()`'s `arg` points into `line_buf`, so a
serial command typed during a long `sh` overwrote the running one's
arguments.

**R1 landed the same day, so 4a is complete except R3.** The registered
compositor can be granted a writable, write-combining mapping of the
real framebuffer (`WIN_REQ_FB_MAP`) and publish a damage rect
(`WIN_REQ_FB_PRESENT`), owned by `kernel/proc/win_surface.c` and
revoked wherever the compositor role is cleared. R3 (the hardware
cursor and mouse bounds over TWP) is deliberately deferred to 4b: the
ring-0 WM calls `gfx_hw_cursor_*` directly and cannot exercise a
protocol cursor request, so building it now would add a path whose only
caller is a test.

      **This is now the ONLY thing between here and a 4K desktop**
      (2026-08-18). The address space was sized for one in the same
      change that made the heap lazy: `WIN_BUFFER_STRIDE` is 64 MiB, so
      a 3840x2160x4 buffer (31.6 MiB) has a slot, and the compositor
      region and framebuffer moved up to clear it. What is left is this
      item plus raising `WIN_CLIENT_MAX_W/H` -- and raising the caps
      FIRST would be worse than not raising them, since it turns a hard
      limit into an intermittent silent refusal: 31.6 MiB is 8192
      contiguous frames.

**Items, in full.**

- [x] ~~The kernel context is a scheduler participant, so `wm_run()` keeps drawing while a ring-3 process runs~~ -- done, see the commit that added it. Step zero: nothing else here works until this does.

- [x] ~~A blocking wait, so a GUI client doesn't spin-poll its whole timeslice~~ -- done, see the commit that added it. Blocking syscalls DESCHEDULE rather than wait in place; the naive version hangs after one event and the reason is now in `docs/decisions.md`.

- [x] ~~An event message format, and delivery to a ring-3 process~~ -- done: `abi/win_proto.h`'s `struct win_event` plus per-process queues and `SYS_POLL_EVENT`/`SYS_WAIT_EVENT`.

- [x] ~~Event SOURCES: route real keyboard and mouse input to the client that owns the focused window~~ -- done. The WM decides WHO an event belongs to (focus, hit-testing, z-order, all unchanged); the protocol decides what it says.

- [x] ~~Client windows in the WM's own window list, with real chrome, focus, z-order and a taskbar button~~ -- done, see the git history. `SYS_WIN_REQUEST` carries typed messages; `win_server.c` owns the memory half and `wm_client.c` the presentation half.

- [x] ~~**An app model for clients (`uapp`)**~~ -- done, stages 0 through 4; see `docs/uapp-design.md` for the design and what each stage actually landed, and the git history. The toolkit is **Toykit** (`userland/ui/`), the protocol **TWP** and the server **TWS**. A ring-3 GUI app is now one `.c` file in `userland/gui/` with no Makefile edit: a `struct uapp_desc` and callbacks, all of them optional with a library default. Window behaviour became a property of the WINDOW rather than of a kernel-side `struct gui_app`, which is what unblocked `WIN_EV_RESIZE` -- defined since the protocol was written and never sent until stage 3. All six ring-3 GUI apps run on it and all are resizable, none containing any resize code. The acceptance test held: stage 3 changed zero lines in the clients that didn't opt in.

- [ ] **Growable client buffers.** `win_server.c` allocates a window's pixels with `pmm_alloc_contiguous()` and maps the whole thing up front, so a window's size is capped twice over -- by `WIN_CLIENT_MAX_W/H` and by `WIN_BUFFER_STRIDE`, the per-window slot in the client's address space. Both were raised to the 1280x720 display size, which is enough for a full-screen client and makes the WM's own screen-bounds clamp the effective limit instead. What is still owed is dropping the CONTIGUITY requirement: 900 contiguous frames is a lot to ask a fragmented allocator for, a resize allocates the new buffer before freeing the old, and the failure is a silent refusal (a normal protocol outcome, indistinguishable from a client declining). Doing it needs a way to map scattered frames into a contiguous KERNEL virtual range as well as the client's, which this kernel has no helper for today -- everything kernel-side is identity-mapped. Related to Demand paging & shared memory's demand paging, and the natural time to do it is alongside that rather than on its own.

- [ ] Multiple windows per process: the protocol already carries window ids and `win_server.c` already tracks WIN_CLIENT_MAX per client, but `userland/tests/winclient.c` only ever opens one, so the path is untested with more.

- [ ] Move the transport from one-message-per-syscall to a shared-memory ring the client maps once. The message formats are already designed for it (no pointers, fixed layout) -- this is the step that makes the syscall count stop scaling with event rate.

- [x] ~~Force-close an unresponsive client~~ -- done. Not a timeout but a PING: `WIN_EV_PING`/`WIN_REQ_PONG` (xdg_shell's shape), answered inside Toykit's loop so no app contains ping code and an app stuck in its own callback correctly fails to answer. That distinction is the substance -- a client that REFUSES to close and one that is WEDGED are the same observation to a timer. Force Quit kills the PROCESS (`scheduler_kill()`), since dropping the window alone leaves a process drawing into an unmapped buffer. Covered by `tools/forcequit_test.py` (15 checks), which tests `winclient` (declines, keeps answering) against `hangclient` (stops pumping).

- [x] ~~Client-side window resize~~ -- done, as a configure/ack handshake rather than a size the server imposes: `WIN_EV_RESIZE` proposes, the client reallocates and acks. Landed in uapp stage 3 and touched ZERO lines in the clients that had not opted in, which was the acceptance test. Covered by `tools/uapp_test.py`.

- [x] ~~**Empty ring 0 of applications first**~~ -- done 2026-08-16 (design doc's stage 0). Notepad, Calculator and Terminal DELETED from the kernel now that the ring-3 versions ship and launch from the Start menu; About and UI Demo ported to `userland/gui/`; the checkbox, dropdown, listbox and text view deleted from `apps/ui/`. The design doc's claim that `apps/ui/` ends with no callers was WRONG and is corrected there: seven files in `apps/wm/` include it, so the rest of it retires with the WM in stage 4. **Task Manager and Control Panel deliberately did NOT move**: each needs a syscall ring 3 does not have (a process list, and `etc_config`), and stage 0 is defined as the stage that adds no kernel capability -- so they move in stage 4 with the syscalls they need, not before. Proven by `gui_regress.py` 13/13 with `uidemo_test.py`'s 28 checks now driving the RING-3 widgets.

- [ ] **Restore the About window's storage line.** The kernel-side About printed the filesystem backend and whether it persists (`fs_backend_name()`/`fs_is_persistent()`); the ring-3 port cannot, because neither has a syscall behind it, and stage 0 added none. Fold it into stage 4's settings/process syscall batch rather than adding a one-off. `df` and `fsck` report both facts meanwhile.

- [ ] **Kernel command-line switches for the protections, not just `nokaslr`.** `multiboot_cmdline()` exists and kernel ASLR is its only user; `nowx`, `nonx`, `nosmap`/`nosmep` and a heap-debug switch would join it. The argument is not convenience, it is TESTING: proving a W^X or SMAP KTEST can go red currently means editing the kernel and rebuilding (see CLAUDE.md's positive-control note, and the session that read 132/132 green off a stale ISO), and a boot flag turns that into a launch argument the suite can run both ways. Two rules it has to follow, or it makes things worse: a disabled protection must be reported loudly (`dmesg` and `about`, as `nokaslr` already does with its note), and the affected KTESTs must SKIP with a reason rather than fail -- otherwise booting with `nowx` reddens six checks and the next session "fixes" the tests. Asked for 2026-08-16.

- [x] ~~**A Live-CD boot: run from the ISO with no disk.**~~ -- done 2026-08-16, all three stages in one pass. The ISO carries a TFS3 image as a GRUB module, a block-device layer sits between the filesystems and the disk, and a RAM device mounts the module. Two GRUB entries: the default prefers a disk and falls back to the image, `toy-os (live)` forces the image. Proven by `tools/live_boot_test.py`, which boots with NO -drive and asserts a shipped binary runs -- "it booted" proves nothing here, since the kernel degrades to an empty RAM filesystem and still reaches a shell. The ASLR blocker was real and is fixed. Costs: the ISO is ~162 MiB because TFS3's minimum volume is one 128 MiB block group. See docs/live-cd-design.md's "What actually shipped".

- [ ] **Let TFS3 blocks-per-group vary for small volumes.** `bpg` is already a superblock field; both the kernel (`T3_BPG`) and `tfs3_writer.py` range-check it to exactly 32768, so the smallest TFS3 volume is 128 MiB. That is what makes the live image -- and therefore the ISO -- an order of magnitude bigger than the data in it. Touches a tested filesystem's geometry validation, so it wants its own pass with `fs_switch_test.py` and `tfs3_v1_test.py`.

- [ ] **Raw input to the compositor.** The WM is the thing that decides focus, so it cannot receive input through the focus-routed event queue it is itself responsible for filling. Needs the raw keyboard/mouse stream exposed, running alongside today's routing until the WM actually moves.

- [ ] **A ring-3 allocator, and four smaller syscalls.** The WM's state is `kmalloc`'d and ring 3 has no `malloc` (Runtime + interop); plus `etc_config_*` for settings, a MONOTONIC tick (`sys_gettime` is RTC wall-clock, wrong for animation), and `scheduler_kill`/ `scheduler_poll` for force-quit and reaping.

- [ ] An abstract transport behind that protocol, so the server side can move to ring 3 later without rewriting every call site -- the same "one struct of function pointers" pattern `display_driver` and the VFS backend probe already use here

- [ ] A bigger process table (4 slots) -- now genuinely binding: a ring-3 terminal plus the program it spawned is already two, so two terminals running commands exhausts it.

- [ ] `tosh` improvements once the kernel supports them: pipelines (`a | b` -- the pipe primitive exists, the parsing doesn't), redirection, and Ctrl-C (see Signals & process control, whose requirements this migration is what makes achievable).

- [ ] A GROWABLE user stack. Raised from 1 page to 4 after the ring-3 Notepad page-faulted opening its file dialog; the real answer is a page-fault handler that maps another page when the faulting address is just below the stack (`fork()`/`exec()`-style process model), not a bigger constant.

- [x] ~~A userland drawing runtime, so a client can render more than flat colour~~ -- done: `userland/ui/ugfx.c` (rects, anti-aliased text, metrics), with the desktop's font mapped READ-ONLY via `WIN_REQ_FONT` rather than copied into each binary. See the git history; `userland/tests/uiclient.c` is the app-shaped client built on it.

- [x] ~~Port the `apps/ui/` widgets Calculator needs to userland~~ -- done: `userland/ui/uui.c` (`ui_primitives` + `ui_button` + `ui_button_group`). Statically linked per client for now, not a shared library -- see the note below on when that should change.

- [x] ~~Migrate one real app (Calculator) to `userland/`~~ -- done, see the git history. `apps/calc_engine.c` is SHARED (compiled twice, once per code model) rather than copied, so there is only ever one arithmetic implementation.

- [x] ~~Migrate Notepad to `userland/`~~ -- done, see the git history. Its file dialog is drawn by the APP, not the window server, which is what GTK/Qt do; `apps/wm/file_picker.c` is a WM modal and was not portable.

- [x] ~~Port the remaining `apps/ui/` widgets~~ -- done (`userland/ui/uwidgets.c`): scrollbar, text field, checkbox, radio list, listbox, dropdown, focus ring.

- [x] ~~Migrate Terminal to `userland/`~~ -- done, and it needed new kernel machinery rather than a port: see the pipes/spawn entry in the git history. Its shell (`userland/lib/tosh.c`) runs in ring 3 too rather than proxying the kernel's.

- [x] ~~Geometry primitives, so a client can draw more than rectangles and text~~ -- done: `kernel/lib/geom.c` + `fixed.c` (lines, polylines, ellipses, circles, filled ellipses, rotation, both aliased and anti-aliased), wrapped as `gfx_draw_*()` in the kernel and as the `uui_canvas` widget in ring 3. Shared source compiled twice, the same pattern as `calc_engine.c`. `userland/gui/gfxdemo.c` ("Shapes") is the ring-3 demo; `tools/gfxdemo_test.py` and `kernel/lib/geom_test.c` test it.

- [x] ~~A not-responding timeout and a way to force-quit a client that ignores `WIN_EV_CLOSE`~~ -- done, see the git history. Built on a real liveness ping (`WIN_EV_PING`/`WIN_REQ_PONG`, i.e. xdg_shell's) rather than a close timeout, because a client that DECLINES and one that is WEDGED are the same observation to a timer. Force Quit terminates the process (`scheduler_kill()`), and the WM reaps the pids it launched, which is what makes it repeatable. What is still NOT built: any indication that an app is hung outside a close attempt -- the ping is only sent when the WM asks a window to close, so that is the only time the title-bar mark can appear.

- [ ] **`WIN_REQ_POPUP` -- a popup SURFACE, so a menu can leave its window.** The caller now exists: `userland/ui/uui_menubar.c` resolves its placement (flip / slide / clamp) against a bounds rectangle the app hands it, and today that rectangle is the client's own content area, because a TWP client can draw nowhere else. On Windows a popped-up menu is a real `HWND` of the `#32768` class in SCREEN coordinates, constrained against the monitor work area; on KDE it is a `Qt::Popup`, which under Wayland is an `xdg_popup` with a positioner the compositor resolves. Neither is bounded by its parent window. The shape here: a TWP message creating a child surface anchored to a parent rect, composited above the parent by TWS, owning an input grab, and destroyed on click-out or on the client's say-so. The widget then takes the screen rect instead of the window's and changes nothing else -- the placement maths is already the right maths. What that buys: a full menu on a window too small to hold one, which is the only case where the current behaviour is visibly not a desktop's. What it costs: z-order, damage and input routing in `wm_client.c` for a window kind that is not in the window list.

- [ ] Fill a POLYGON, not just an ellipse. `geom_fill_ellipse()` is a scanline fill of one specific shape; the general version is an edge-list/active-edge-table scanline fill taking arbitrary points, which is what a filled triangle (and therefore any real 2D drawing) needs. Deliberately not built yet -- there is one caller's worth of demand (the demo's vertex dots), and the bar here is a second real caller.

- [ ] Clipping RECTANGLES as a first-class concept in `ugfx`, rather than each widget wrapping the plot callback itself. `uui_canvas` clips because it owns its callback; a text widget drawing into a scrolled viewport would want the same thing and would currently have to reimplement it. The right shape is probably a clip rect on `struct ugfx_surface` that every draw call honours -- but see `docs/gui-guidelines.md` on `gfx_draw_string()` not clipping, which is the kernel-side version of the same unfinished decision.

- [ ] An animation/timer event, so a client does not have to poll. Shapes spins by looping and calling `sys_yield()`, because there is no "wake me in 16ms" event -- which means it burns its timeslice whenever it is open, and its frame rate is whatever the scheduler happens to give it. A `WIN_EV_TIMER` delivered on a client-requested interval is the fix, and it is a prerequisite for anything animated that should also be well-behaved.

- [ ] Decide whether the userland widget/graphics code becomes a real shared library rather than being statically linked into each client. Right now `ugfx.o` + `uui.o` are linked per binary, which is fine at two clients and wasteful at ten. The font already set the precedent for the answer (share one copy, no drift) -- but sharing CODE needs the dynamic-linking work in Dynamic linking / shared libraries, which is why this is a note and not a task yet.

- [x] ~~Make the ring-3 apps reachable from the desktop~~ -- done: `gui_apps.h`'s `exec_path` turns a registry entry into a launcher for a `/bin` binary, so Shapes, Calculator (ring 3), Notepad (ring 3) and Terminal (ring 3) are in the Start menu and on the desktop. The apps also moved `/tests` -> `/bin`, where a user-facing program belongs.

- [ ] Remove the kernel-space Calculator once the ring-3 one is the default. **This is the next step, and it now has a second reason:** the Start menu carries both, distinguished only by a "(ring 3)" suffix on the label. Retiring the kernel-space Calculator, Notepad and Terminal drops the suffix and halves those menu rows. What it costs is the side-by-side comparison that made the migration verifiable, so the ring-3 versions should get a round of testing as the ONLY implementation first. Deliberately NOT done in the same change: keeping both is what made the migration verifiable (the two were compared side by side, and the shared engine means they cannot disagree on arithmetic). Retiring the old one is its own decision.

- [ ] ELF loader hardening -- `elf_load()` isn't told the file's size, so `p_offset`/`p_filesz` are unbounded and `p_vaddr` unchecked. Tolerable while every binary is one we built; not once loading ring-3 apps is the normal path. See the Details entry.

## A layout engine for the GUI

*Before the apps that would use it. Every widget position in `apps/` is
hand-computed arithmetic today, which is why no window can be resized.*

## Runtime font loading & text metrics

**Needs:** Runtime + interop -- loading a font at runtime means allocating
for it.

*After the layout engine, which is the thing that actually needs to ask
"how wide is this string?" -- and needs a true answer, not a monospace
guess.*

**Items, in full.**

- [ ] Note the boundary: complex-script shaping (bidi, ligatures, combining marks) needs UTF-8 migration's UTF-8 work first; this milestone stops at metrics and kerning for single-byte text

## Desktop visual polish

**Items, in full.**

- [ ] Full dirty-rect compositor -- mostly done, see the commit that added it: window move/resize/open/close/minimize/ z-order and desktop icon drag now clip repaints to a computed damage region instead of always touching the full screen, and (Phase 3) a window whose rect doesn't intersect the damage region is skipped entirely -- its chrome/`on_draw()`/resize-grip calls never run, not just have their pixels clipped away. Still open: menu/taskbar-content-click/dialog redraws -- and the taskbar/tray (including the clock tick) itself -- still fall back to a full-screen repaint (imprecise but safe, never worse than before). An initial attempt at scoping the tray/clock tick to just the taskbar strip shipped and was reverted the same day -- see `docs/decisions.md`'s notification-area entry for the two real bugs that caused (a poisoned first frame, and losing an implicit once-a-second full-repaint safety net the mouse cursor turned out to depend on). Also still open: **giving each overlay (Start menu, context menu, file picker, confirm dialog) a real damage rect of its own.** They currently force the whole frame to a full repaint while open, which is correct but blunt -- and is now enforced rather than assumed, because "these fall back to a full-screen repaint" used to be true only by accident and inverted the moment anything else declared damage in the same frame (see the git history damage-sweep entry). Doing it properly needs each overlay to expose its own geometry, which only `start_menu` does today.

- [x] ~~Taskbar notification area (tray)~~ -- done, see the commit that added it: a dynamic `tray_register()`/ `tray_set_text()`/`tray_unregister()` API (`apps/wm/wm.h`), with the taskbar clock as its first item (`apps/wm/wm_tray.c`). No other GUI app registers a tray item yet -- the API is there for one to use next time a feature calls for it (an async job's progress, a background download, etc).

- [ ] **A tween/easing helper, once a second real caller exists.** Nothing in the tree interpolates anything over time: the Start menu's click flash, the tray clock and `demo.c`'s `wait` are all "is the deadline reached?" checks, not motion. `kernel/include/api/fixed.h` already has what one needs (Q16.16, `fx_mul`/`fx_div`, `fx_sin` for ease-in/out), so this is small when it is wanted. Deliberately NOT built yet: the obvious consumers -- a cursor walking a path and an animated window drag -- turn out to be the same caller ("walk a point from A to B over N ms"), which fails this repo's second-real-caller bar. Build it when a genuinely different consumer turns up: a WM animation, or The GUI in ring 3's `WIN_EV_TIMER`. **Pace it by `pit_ticks()`, not by frame count** -- `wm_run()` is a free-running loop paced only by `hlt` (`apps/wm/wm.c:592`), so it wakes on any interrupt and runs much slower under `gui damage verify on`; a frame-paced animation would silently change speed between a demo and a test run.

- [ ] Scripted interaction that spans frames, so the demo tour can show real use -- `demo_gui_tick()` runs exactly one step per WM iteration and advances `g_next` unconditionally (`apps/demo.c:138`), so no scripted action can take time. `wait` is the sole exception (`g_wait_until`, `apps/demo.c:140`) and is the shape the rest would follow. With the three test-harness items above (Kernel test harness) this is what would let the tour walk the cursor to a desktop icon, double-click it -- the WM already has double-click, `apps/wm/desktop.c:254`, 300ms -- and drag a window visibly. Window drags already animate per frame (`wm_update_drag_resize()`), so that half needs nothing new.

## Desktop productivity apps

**Needs:** Runtime + interop (allocator, file I/O), A layout engine for the GUI (layout) and
Runtime font loading & text metrics (fonts). This is the leaf the three of them exist for.

## Layer 8 -- Tooling, observability and docs

Cross-cutting, and cheap relative to what they save. They are listed
last because they gate nothing -- not because they matter least; this
repo's test tooling has repeatedly been what turned a mystery into a
measurement.

**Items, in full.**

- [ ] Real RING-3 filesystem API surface (list/stat/create/delete/ seek -- the KERNEL-side fs API grew stat-with-ino, hardlinks and capability queries at TFS3: an inode layer, but the syscall surface is still `SYS_OPEN`/`SYS_READ`/`SYS_CLOSE` sequential-read-only; no `SYS_SEEK`/lseek-equivalent exists at all)

- [x] ~~Control panel with pluggable applets~~ -- done, see the commit that added it (icon-grid chooser + drill-in, with Date & Time and System Info applets)

## Kernel test harness

*The heading used to be struck through as "completed 2026-08-13" while
five boxes below it were unchecked. That is the milestone lying about
itself: what shipped is the harness -- registration, `make test`, CI,
fault injection, and moving the boot self-tests behind it -- and what is
listed after that is coverage work nobody has done. Struck-through means
DONE here, so the strike came off rather than the boxes going on.*

**Items, in full.**

- [x] ~~A registration mechanism for in-kernel tests~~ -- done, see the commit that added it (KTEST() + a `.ktests` linker section: tests register by existing)

- [x] ~~A `make test` target that boots, runs every registered test, and exits non-zero on failure~~ -- done (`tools/ktest_run.py` drives `ktest` over the serial debug console)

- [x] ~~Fault injection as a first-class facility~~ -- done, see `kernel/include/kernel/fault_inject.h` (fail the next N ATA writes/reads or kmalloc calls; 5 of the 14 tests use it)

- [x] ~~Move the existing boot self-tests behind it, so a normal boot stops paying for them~~ -- done; `kernel_main()` runs no tests at all now, and `tfs_init()` no longer writes at a 4.6GB offset on every disk-backed boot

- [ ] Coverage honesty: a list of what has NO test (the PIO disk path, the ELF loader's error branches, the WM event loop) rather than a percentage nobody can act on

- [ ] **A scriptable POINTER, not a one-frame override.** An injected cursor position survives exactly one `wm_run()` iteration -- it is applied at `apps/wm/wm.c:619` and clobbered by `mouse_get_state()` at the top of the next frame (`apps/wm/wm.c:606`) -- and there is no `mouse_set_position()` in the driver API at all, only `mouse_set_bounds`. So nothing can drive the cursor along a path: a hover test has to park the REAL PS/2 cursor via `DebugConsole.warp_cursor()` and confirm arrival, and no test can observe a drag mid-flight. The fix is a persistent pointer SOURCE the WM reads from, which is the same seam The GUI in ring 3's raw-input stage and a USB HID driver (USB) would both plug into -- so it is worth building as a source rather than as a test hook. Note `gui state` reports the real mouse (`wm_debug.c:451`), so it must learn to say which one is authoritative or it becomes a second thing that lies.

- [ ] **`gui icons [--json]` -- desktop icon geometry.** No `gui` command reports it: `gui probe` answers the bare region string `"desktop"` with no index, and the rects are private to `apps/wm/desktop.c` (`icon_hit_test()`, `icon_grid_cell_rect()`). So no test can click a desktop icon without hardcoding coordinates -- and those are the worst kind to hardcode, since icons are user-draggable and their positions persist to `/etc/desktop.conf`. Needs a `desktop_icon_rect()` accessor behind it. This is CLAUDE.md's "a geometry line an app does not report is one a tool will re-derive", still true for the one surface that has never reported any.

- [ ] **Finer `gui drag` interpolation.** `DRAG_STEPS = 8` (`apps/wm/wm_debug.c:622`), so a scripted drag moves in eight big hops rather than the per-frame motion a real drag produces. It can therefore step clean over a hit region, and it does not exercise `wm_update_drag_resize()` the way a hand does -- a drag test can pass while a real drag is broken.

- [ ] **A golden-image baseline for the GUI, diffed automatically.** The GUI suite asserts on facts it thought to check -- a pixel here, a reported geometry there -- so a rendering change nobody wrote a check for lands green. A committed reference screenshot per app, compared each run, catches the class of regression the targeted checks miss by construction. `tools/screenshot_diff.py` already does the comparison with a threshold and a highlight image; what is missing is the baseline set and the masking. **Masking is the whole difficulty, not a detail**: the taskbar clock changes in every screenshot, so a whole-screen diff is pure noise, and the same goes for anything else time- or state-dependent. So this needs per-baseline ignore rectangles (the tray strip at minimum) and a way to regenerate a baseline deliberately when a change is intended, or it becomes a check everyone learns to re-bless without reading. Note this is NOT the retired `screenshots/` convention coming back: those were per-commit artifacts a human was expected to eyeball, which is exactly the part that does not work -- the value here is in the machine doing the comparison. See `CLAUDE.md`'s "Screenshots are a TESTING TOOL, not a deliverable".

## Benchmark suite

**Items, in full.**

- [ ] Separate the two questions a benchmark answers -- throughput (MB/s) and latency (worst-case single operation) -- since a desktop cares about the second and `stress` only reports the first

## Fuzzing & property-based testing

*Placed right after the test harness and benchmark suite: it's the third
leg of the same stool, and every milestone below it is easier to trust
once this exists.*

## Crash reporting & postmortem debugging

*After signals, because SIGSEGV delivery is what a core dump hangs off.*

**Items, in full.**

- [x] ~~Stack-overflow detection via a guard page, reported as such rather than as a mystery fault~~ -- done as Memory protection hardening's guard page item; `uaddr_is_stack_guard()` is what names it, and `tools/faulttest_run.py` asserts the report

## In-OS documentation

*No hard prerequisites; placed by the shell cluster because that's what
it serves. Small, and it makes everything above it discoverable.*

## Not built yet, and deliberately so

## Known issues and papercuts (unscheduled)

### The kernel ships ~62 KB of `.eh_frame` unwind tables nothing can ever read

Measured on the current image: `.eh_frame` is **62,076 bytes, 2.5% of
`kernel.bin`**. Those are DWARF call-frame tables, and their only possible
consumer is an unwinder -- C++ exceptions, `_Unwind_Backtrace`, a debugger
walking frames from inside the process. This kernel has none of the three.

It cannot even be used by the one thing that looks like it would want it:
the panic backtrace is deliberately a **stack SCAN**, not a frame walk,
because the build is `-O2` and an RBP chain would be fiction (see
`kernel/arch/x86_64/idt.c`). GDB unwinds from OUTSIDE via QEMU's stub and
the separate `.debug_*` sections, which are not loaded.

`-fno-asynchronous-unwind-tables` in CFLAGS removes them. What makes this
worth doing carefully rather than casually:

  * **`linker.ld` places `.eh_frame` EXPLICITLY**, in the read-only band,
    with a comment saying it is placed "rather than left as an orphan"
    because with `PHDRS` declared an orphan lands wherever `ld` chose --
    and the silent direction is the R+X band. Removing the section means
    removing that placement too, and a stale `*(.eh_frame)` line matching
    nothing is harmless while a REMOVED one that the flag does not
    actually suppress is the orphan trap all over again. Check with
    `size -A build/kernel.bin` afterwards, not by reading the script.
  * **The W^X KTESTs assert on band boundaries.** Deleting a section moves
    every address above it. That is exactly what those tests are for, so
    expect them to be the thing that tells you whether it worked.

Not urgent -- 62 KB of read-only NX data costs nothing at runtime. It is
recorded because "why is this in the image at all" had never been asked,
and the answer turned out to be "nobody passed the flag".

### The in-kernel test suite is ~30% of `.text` and ships in release images

Measured: `.text` is 365.9 KiB, of which **108.9 KiB (29.8%) comes from
`*_test.o`** -- re-measure with `size -A build/kernel.bin` and
`find build/kernel -name '*_test.o' -exec size -A {} \;`, since both
figures are a snapshot of one build. Every ISO ever cut -- including the v0.2.0 release people
downloaded -- contains all 285 tests, their fixtures and their assertion
strings.

**This is a deliberate trade, not an oversight**, and the trade is good:
`ktest` runs inside the LIVE booted kernel, which is what makes the tests
meaningful. They run against a real heap and a real mounted filesystem,
and that is precisely how three `heap-debug` tests were caught assuming a
quiescent heap once a desktop was always running. A suite that could only
run in a stripped-down test build would not have found that.

The registry costs almost nothing on top: `.ktests` is 9,120 bytes for 285
tests, **exactly 32.0 bytes each** -- one `struct ktest_case` per test,
dropped in by `__attribute__((used, section(".ktests")))` and bracketed by
`__ktests_start`/`__ktests_end`. There is no registry file and no init
call to forget, because the linker IS the registry.

So the item is not "remove it". It is: **decide whether a release build
should differ from a dev build at all, and write the decision down.**
Today they are byte-identical, which has a real virtue -- the thing users
run is the thing that was tested, and a user can run `ktest` on their own
hardware and send you the output, which has already been useful for
CPU-dependent paths this environment cannot reach (SMEP/SMAP, the
invariant TSC).

If it is ever taken: a `-DNO_KTEST` that compiles the `KTEST()` macro to
nothing is the cheap version, and the thing to verify is that
`__ktests_start == __ktests_end` leaves `ktest_run_all()` reporting "0
tests" rather than walking a null range. The 100 KiB is the cheapest
saving available in the image, and it is still probably not worth losing
the ability to test a shipped one.


### Two win-server KTESTs only run on a `target=text` boot, since a live desktop removes what they test

`win_server`'s "requests are refused when no server is registered" and
`winshare`'s "claiming needs no registered presentation layer" both
assert what happens with NO window server present. Since init started
the desktop at boot (2026-08-18) the default boot always has one, so
both skip -- `make test` and CI report 3 skipped where they used to
report 1.

They are not dead, and that was checked rather than assumed:

    make iso KCMDLINE="target=text" && python3 tools/ktest_run.py

reports 284 passed / 1 skipped, i.e. both run and both pass. Rebuild the
plain ISO afterwards.

The reason this is recorded rather than fixed: the honest alternatives
are all worse. Evicting the live compositor inside a KTEST would tear
down the user's desktop to test a refusal path. Faking "no server" needs
a way to lie to `win_server_request()` about global state, which is a
test hook in production code. Leaving the guard off -- which is how they
were until this was found -- meant they silently asserted the opposite
of what they were written for.

The right fix is probably that these two belong to a boot-mode-specific
suite that `ktest_run.py` runs in a second, `target=text` VM. That is a
harness change, not a kernel one, and it is worth doing once a third
test wants the same regime.

### The desktop died once at 1.15 s while a `/bin` program ran through the legacy loader -- cause unestablished

Seen exactly once, in a `tools/init_test.py` run on 2026-08-18: init
logged `toywm (pid 2) exited with code -1 after 1150 ms` at t=1.57 s and
restarted it correctly. Nothing in the test had killed it. The tool's
first actions after boot include `config get system.default_target`,
which runs `/bin/config` through `elf_run.c`'s LEGACY loader while the
desktop is a live scheduled process -- and that combination is a known
hazard with an existing roadmap item (`g_next_kernel_rsp` reentrancy: a
legacy `run` inherits a client's RSP0 and can overwrite its saved
trapframe).

**That is a suspicion, not a diagnosis, and the difference matters.**
What was actually established: six fresh boots with no serial commands
showed zero spontaneous exits, and a deliberate `config get` against a
live desktop did not reproduce it once. So the rate is at most low and
the mechanism is unconfirmed.

Why it is worth writing down anyway: init starting the desktop makes
this class of interaction the DEFAULT rather than something you had to
set up. Every `python3 tools/vm.py exec "<some /bin program>"` now runs
the legacy loader alongside a live desktop. If the reentrancy item is
ever picked up, this is the observation to try to reproduce first.

To hunt it: `python3 tools/flake_hunt.py` has no init entry, so loop
`init_test.py` directly and grep for a `toywm ... exited` line that no
`kill` explains. The check that catches it is scoped to the kill now, so
a spurious exit no longer makes the restart check pass for the wrong
reason -- but it does not fail either, so grep rather than trusting the
exit code.

### `tools/ktest_run.py` reports the debug console never came up

Symptom: either `ktest_run: FAIL -- the serial debug console never came
up` or a bare `ConnectionResetError: [Errno 104] Connection reset by
peer` from the socket read. The run measures NOTHING either way, and
passes on re-run.

**Measured 5 boots in 9 on 2026-08-18**, against the previous commit
with that session's work stashed: 5 connection resets, 4 clean passes,
zero assertion failures. So it was not the desktop starting at boot and
not the guest being slower -- it was the harness or the QEMU serial
socket.

**Measured again 2026-08-20: 0 in 29** (9 runs, then 20 more on a
`make clean-disk` image), zero errors. Nothing in between targeted it.
A true rate of 5-in-9 would produce 29 clean runs about once in ten
billion, so the rate genuinely changed; 29 clean runs bound it below
roughly 10% at 95% confidence, which is not the same as zero. **The
cause was never established**, so this stays here rather than being
deleted. One contributing cause WAS found and fixed on the way (init
used to be spawned before `debug_console_init()`, so the desktop was
loading its font, cursor theme and nine desktop entries off the disk
while the console was still coming up); on the current evidence that may
have been more of the story than it looked at the time.

**What changed on 2026-08-20 is that the next occurrence will be
diagnosable in one run instead of another nine-boot measurement.** The
harness now reports the failure as facts rather than a verdict --
elapsed time, bytes received on the wire, whether QEMU is still alive
and with what exit code, the tail of QEMU's own log, and the last thing
the guest said (`tools/serial_console.py`'s `diagnostics()`). A dead
socket is CAUGHT and reported rather than raised, so the
`ConnectionResetError` symptom becomes a report instead of a traceback,
and the two symptoms above stop looking like unrelated problems. The
value was demonstrated accidentally during the same session: a run
launched beside the batch failed, and instead of "the debug console
never came up" it said `Failed to get "write" lock -- Is another
process using the image [disk.img]?`.

`python3 tools/flake_hunt.py ktest -n 10` is the loop for measuring any
change to it, and it scores these runs as `error` rather than `pass`.

### `heap-debug`'s use-after-free check fails about 1 run in 15

`mm_test.c`'s "a write through a freed pointer is caught by
heap_check()" intermittently reports `FAIL: heap_check() == 1` -- i.e.
the scan found no violation, so the block it deliberately damaged was no
longer a damaged free block by the time the scan ran.

**PRE-EXISTING**: reproduced 1 in 15 on the previous commit, in-guest
(`ktest heap-debug` repeated in one boot, no desktop running). Three
SIBLING tests in the same suite had a related fragility that WAS fixed --
they compared `heap_used_bytes()` against a pre-test snapshot, which any
concurrent kernel allocation breaks, and they now hold
`scheduler_preempt_disable()` across the measured section.

That fix does not extend to this one, and the reason is the interesting
part: the window here is between `kfree(b)` and `heap_check()`, and
`heap_check()` walks the WHOLE heap -- so the test is not asserting about
its own block in isolation, it is asserting that nothing else in the
system tidied that block away first. Making it robust means either
asking `heap_check()` about one address (a narrower API that does not
exist) or accepting a tolerance, and a tolerance on "did the corruption
detector fire" is worse than an occasional red.

### One `etc_config_set()` write failed on a graphical boot, and did not reproduce

Seen once, in a full `ktest_run.py` on 2026-08-18:
`FAIL: etc_config_set(SCRATCH, "colour", "amber") expected 1, got 0`, a
write to `/tmp/ktest_etc.conf` reporting failure. The two runs after it
failed `fsck`'s `r.leaked == 0` with 2 leaked blocks -- which is the
DOCUMENTED dirty-fixture cascade, not a second bug: `make iso` re-seeds
`disk.img` by sync and never reformats, so blocks leaked by one run are
still leaked for the next. `make clean-disk && make iso` cleared it.

**Not reproduced**: 12 consecutive in-guest `ktest etc_config` runs with
the desktop up were clean, and `fsck` was clean afterwards. So there is
one observation of a failed write and no mechanism.

Why it is worth recording anyway: since init starts the desktop, EVERY
ktest run now has a compositing process doing its own file I/O
concurrently, and `etc_config_set()` bumping `fs_generation()` is exactly
what makes the desktop re-read `/usr/wm/desktop`. That is a legitimate
thing for an OS to support -- it is what Notepad saving a file does --
but it is newly the default during the test suite. If a write failure
recurs, this is the interaction to instrument first, and
`kernel/fs/vfs.c`'s `FS_OP()` preemption guard is where to look.

## Idea: make heap debug reachable from boot (2026-08-17)

`heap debug on` is a RUNTIME toggle, so allocations made before someone
types it are not red-zoned -- and the desktop, the WM's window table and
every driver's buffers are all allocated before that point. Catching a
corruption whose victim was allocated at boot therefore needs luck.

A `heapdebug` boot word would close that, in the same style as
`nokaslr`/`nopat`/`notsc`/`faultinject`: every allocation from the first
one gets a red zone. Not built. The cost is memory and speed on a boot
nobody asked for it, which is exactly why it would be a flag.

Raised after a corruption hunt where the runtime toggle DID find the
culprit -- so this is an improvement, not a gap that blocked anything.

## DIAGNOSED and fixed: GP fault in the heap after repeated setting changes (2026-08-17)

Kept as a worked example, because the shape recurs.

**Symptom**: switching a setting in Control Panel a few times panicked
with a General protection fault, and merely HOVERING the choice list
froze the desktop for seconds.

**Three defects, found by `heap debug on`:**

1. `cpanel`'s `on_widget()` discarded `reason`, so every routed event --
   including plain `UUI_REASON_MOTION` -- was treated as a commit.
   Hovering applied a setting per motion event.
2. `setting_set()` wrote the file and bumped `fs_generation()` even when
   the value was unchanged, and everything watching that counter does
   real work (the desktop re-reads every `.desktop` file).
3. **The actual corruption**: `fs_read()` was not re-entrant. Both
   backends free one shared staging buffer, allocate a new one, then do
   a BLOCKING read into it. The kernel context is a scheduler
   participant, so the WM was preempted mid-read; a ring-3 syscall then
   read a file, freed that buffer and allocated a 96-byte one, and the
   suspended read resumed writing `.desktop` bytes past its end.

The red-zone report named it exactly: a 96-byte block whose right
red-zone held `ame=Calc` -- the tail of `Name=Calculator`.

**The reusable part**: (1) and (2) were amplifiers, not the bug. Hundreds
of small writes SHOULD be survivable; they only weren't because a latent
kernel bug was waiting. Fixing only the UI would have hidden (3) again.
And `heap debug on` is what turned "something corrupted the heap" into a
named block in one run, after three scripted reproductions had failed.

## Where else the `block_device`/`fs_ops` layering approach fits (design, unscheduled)

A survey of where this repo's registry-and-interface pattern (one
`display_driver`, one `fs_ops` backend, one `block_device`) is missing
and would pay. **Not a commitment to build any of it** -- the standing
bar is a second REAL caller, not a plausible one, and most of these do
not clear it yet. What is worth doing now is keeping each seam SHAPED
so it can appear later without a rewrite: no `inb`/`outb` leaking
upward, no caller naming the hardware.

      **The 2026-08-17 CPU-accounting bug is evidence for this**, and it
      is the concrete argument to make when this comes up again:
      `SYS_YIELD` billed a whole tick because the code incremented a
      tick counter rather than asking a clock how much time had passed.
      An interface that answers "how much time elapsed" makes that bug
      harder to write. Its own follow-up -- billing by TSC delta per
      context switch, so sub-tick work stops reading 0% -- would be the
      second real caller.

      Two cautions. `rtc_read_local()`/`tz_rtc_to_epoch()` is WALL CLOCK
      and does not belong behind the same interface; Linux keeps
      clocksource and RTC separate deliberately, and merging them here
      would be the mistake this entry is meant to prevent. And
      `cpuinfo.h` already warns that a lazy calibration deadlocks inside
      a syscall -- calibration ORDERING is the hard part, not the
      interface.

## Completed milestones

Kept for the record, and because commits and `docs/decisions.md`
refer to them by number.

**Items, in full.**

- [ ] **Group DRAG for a rubber-band selection** -- moving every selected item together as one gesture. The selection half is built and shared (`kernel/lib/rubberband.c`, both surfaces); this is the layer above it. What it needs: a per-item commit callback so the module can offer "item i moved by (dx, dy)" without learning what an item is, and a decision about how it composes with the desktop's existing single-icon drag (which currently arms on any press over an icon, including one that is already selected).

- [ ] **A ring-3 file manager**, the second caller the rubber-band module was shaped for. Until it exists, `RB_ADD`/`RB_TOGGLE` and the whole shared-source arrangement have exactly one caller -- which is this project's usual bar not yet met, recorded honestly rather than presented as vindicated.

- [ ] **Nothing detects an ordinary memory LEAK, in either allocator.** `meminfo audit` compares page tables against the frame allocator and catches the dangerous direction -- a live mapping of a frame pmm considers free. The reverse, a frame marked used that nothing references, is not symmetric and is not covered: page tables, the kernel heap, the kernel image and any DMA buffer all hold frames no page table points at, so a naive sweep reports every one of them as leaked.

- [x] ~~**`ata_dma_nonblocking_selftest()` has a 1040-byte stack frame.**~~ FIXED 2026-08-18: 64 bytes. Its two `uint8_t[ATA_SECTOR_SIZE]` buffers were 1024 of it and GCC cannot overlap them -- comparing one against the other is the point of the function -- so both moved to the heap as ONE allocation, which keeps the cleanup to a single `kfree()` across its six exits and makes the pair all-or-nothing. No alignment constraint applies: `dma_issue()` copies through its own bounce buffer, so the DMA engine never sees the pointer.

- [ ] **The shell's command dispatch is a 60-branch chain, and the fix is not the obvious one.** `apps/shell.c`'s `dispatch()` is 60 `else if (k_strcmp(cmd, ...))` branches whose handlers spread across `shell_sys.c` (1,950 lines) and `shell_fs.c`. It is the largest dispatch chain left in the tree and the one `tools/check_dispatch.py` waives by name.

- [x] ~~**Killing a process leaks its entire address space.**~~ FIXED 2026-08-18. `scheduler_kill()` zombied the victim and `scheduler_poll()` reaped it by marking the slot unused, and NEITHER ever tore the address space down -- that only ever happened when a process called `sys_exit` on itself. So a kill lost the victim's ELF pages, stack, heap and window buffer for the rest of the boot, at ~18 frames a time, reachable from the desktop through Force Quit.

- [x] ~~**The taskbar overflows off the right edge once enough windows are open.**~~ FIXED 2026-08-20. Reported with a screenshot, 2026-08-17: thirteen windows, and the last button clipped by the screen edge and running under the clock. The cause was that `win_btn_w()` returned a CONSTANT and three separate places walked the window list with it -- `draw_taskbar()`, and both hit-tests in `wm_input.c` -- so nothing could shrink a button without the other two disagreeing about where it was. A fourth walk existed in the debug console and was ALREADY wrong: it placed button 0 at `sw + 4` where the real one sat at `4 + sw + 8`, so every button centre it reported, and every test click aimed at one, was eight pixels left of the button.

  The layout is one function now, `taskbar_layout()` in `userland/wm/wm_taskbar.c`, and all four callers read it. Buttons take their natural width while they fit, SHRINK toward a font-derived floor (three characters plus padding), and past the floor windows of the same application COLLAPSE into one button carrying a count -- Windows' behaviour, and KDE Plasma's before it wraps to a second row. A collapsed button opens a list of its windows on click (Windows' jump list). The deliberate difference from both: past the point where even collapsed floor-width buttons will not fit, this DROPS the extras and reports them through `taskbar_hidden()` rather than drawing off-screen. A second row is the obvious next step and was not taken, because `taskbar_h` is a constant that the desktop icon area, the Start menu's anchor, the context-menu clamp and every damage rect all derive from.

  Grouping needed `struct window.app_id` to actually be populated, and on the ring-3 desktop it never was: `wm_client.c` passed `""` to `on_window_created()` because `WIN_REQ_WINDOW_INFO` carries exactly one `text` field and that was already the title. `WIN_REQ_WINDOW_APPID` is the second request that fixes it, asked once at create (an app id never changes). Every `uapp` now declares an `app_id`, not just the single-instance ones.

  Measured with `tools/taskbar_test.py`: 80px buttons to 12 windows, 65 at 16, 51 at 20, 42 at 24, then one `notepad (28)` button at 28, with nothing ever crossing the tray's left edge. The positive control -- flat natural width AND the placement guard removed -- reproduces the original bug at 14 windows, the rightmost button ending at 1248 with the tray starting at 1184.

- [x] ~~**The ring-3 WM stops the moment it spawns a process.**~~ FIXED 2026-08-17, and the cause was mine and much duller than the diagnosis before it.

- [x] ~~**GUI tools that assume the desktop is NOT a process.**~~ FIXED 2026-08-18, and **the desktop is now ring-3 by default** -- all 23 tools pass against it, and `make iso KCMDLINE="gui0"` still selects the ring-0 one while `apps/wm/` remains in the tree.

- [x] ~~**`calculator_client_test.py` is INTERMITTENT.**~~ FIXED 2026-08-18. Two separate TOOL bugs, neither of them in the OS. Measured before: 2 runs in 6 passed. After: 8 in 8.

- [ ] **`newsyscalls_test` fails intermittently in CI, and not locally.** Seen once, 2026-08-18, on a DOCS-ONLY commit -- so it is not a regression from the change it failed on, and the two commits either side of it passed. Exit 1 with "at least one phase FAILED"; the phase was not captured, because the harness printed only the tail and the detail line was earlier.

- [ ] **Other GUI tools may share the calculator's mid-paint flake.** Every tool that compares one screendump against another has the same exposure: a capture landing while the frame is still being painted fails a comparison with nothing wrong with it, and the window manager being a PROCESS widens that window.

- [x] ~~**A syscall TABLE, and handlers in the subsystem that owns them.**~~ DONE 2026-08-18. `kernel/proc/syscall_table.c` is one row per number -- `{ name, handler, argument kinds, return kind }` -- and `syscall_dispatch()` is a bounds-checked call through it. The 37-branch `if/else` chain is gone; the handlers live in `kernel/proc/syscall_fd.c`, `kernel/fs/fs_syscalls.c`, `kernel/proc/proc_syscalls.c`, `kernel/proc/win_syscalls.c` and `kernel/core/sys_syscalls.c`.

- [x] ~~**`strace`'s syscall-name table stops at `SYS_GETRANDOM`.**~~ FIXED 2026-08-18: the fourteen syscalls added since it was written -- process control (spawn/waitpid/kill/pipe), the window protocol, the settings registry, the crash and power paths -- have names and argument types now. `kstack syscalls` reads the same table on purpose (it is the kernel's only list of these names), so it stopped reporting the deepest syscall as `#34`.

- [x] ~~**`syscall_dispatch()` has a 4832-byte stack frame.**~~ FIXED 2026-08-18: 864 bytes after the big branches were extracted, and **96** once the chain became a table (each handler pays for its own frame). The deepest measured path in the kernel went from 8680 bytes to 4456 (53% of a kernel stack to 27%).

- [x] ~~**Force Quit kills the ring-3 desktop.**~~ FIXED 2026-08-18, and the previous entry's "no crash in the log" was simply wrong -- the log has the crash, four lines after the kill:

- [x] ~~**`taskmgr` clicks row 0 and calls it the first listed process.**~~ FIXED 2026-08-17: it finds its victim's ROW BY PID now, clicking rows until Task Manager reports `selected pid N` for the pid it spawned -- robust to sort order, to the desktop being present, and to any future process appearing.

- [x] ~~**`taskmgr_test.py`'s "found the victim's row" is INTERMITTENT.**~~ The check is rewritten and no longer fragile, but **the flake itself stopped reproducing before that**, and the cause was never identified -- recorded plainly rather than claimed as fixed.

- [x] ~~**The ring-3 desktop cannot give a client a window.**~~ FIXED 2026-08-17. `win_server_request()` had `if (!g_ops) return -1;`, which refused every request past the compositor/framebuffer ones whenever no RING-0 presentation layer was registered -- which, with the WM in ring 3, is always. One guard, and it silently refused both `WIN_REQ_FONT` (so the desktop drew no text and every font-derived measurement collapsed: `WM_TITLEBAR_H` is `ugfx_char_h() + 8`, so chrome became 8px) and `WIN_REQ_CREATE` (so no client could ever get a window). A window server is EITHER a registered ring-0 layer or a registered compositor.

- [ ] **The ring-3 WM busy-waits instead of sleeping.** `wm.c`'s frame loop halted on `hlt` in ring 0; in ring 3 that is privileged, so it calls `sys_yield()` and gives up the rest of its slice. Correct, but an idle desktop now costs a round-robin slot per tick rather than nothing.

- [ ] **Injected clicks are LOST under parallel `gui_regress` load, and the failing checks are finally named.** Reproduce by running the full suite (`python3 tools/gui_regress.py --logs DIR`) at the default `-j4`; it needs the parallel load, so a single tool cannot show it.

- [ ] **Get blocking disk I/O out of the WM's event loop.** The desktop still reads files synchronously inside `wm_run()`, so a frame can block for as long as the disk takes. That is much less painful than it was (2026-08-17: the lost-DMA-wakeup race is fixed, the `.desktop` reload only runs when that directory actually changed, each entry costs one read instead of six, and a write-back cache sits underneath) -- a reload now measures in tens of milliseconds where it measured 2.5-5.6 SECONDS. But the worst case is still unbounded in principle rather than by construction.

- [ ] **Make the GUI test tooling RESOLUTION-AGNOSTIC.** The suite assumes 1280x720 in at least two places, which is what stops the default resolution being changed (and stops the tools running against a `video=`-booted guest). Known assumptions: `qmp_test.py`'s `QMPSession` starts the cursor at a hardcoded **(640, 360)** -- the centre of 1280x720, and the value `mouse_init()` resets to -- so every open-loop `goto()` would be offset at any other size; and `gui_flow.py` carries calibrated Start-menu numbers that have needed re-measuring three times already.

- [ ] **Retire `uui_button_group` once nothing needs it.** A standalone `uui_button` routes its own clicks now (press/motion/release on `uui_button_ops`), which is how QPushButton, GtkButton and a Win32 BUTTON all behave -- Qt's `QButtonGroup` exists for EXCLUSIVITY, not for delivering the press, so this toolkit's group is doing a job no real one does. It survives today only as a convenience for a grid of many buttons treated as one widget (Calculator's keypad, UI Demo's row). The work: move those callers to individual items and delete `uui_button_group.[ch]` plus its kernel-side twin. Not urgent -- both shapes work -- but the group is the one that should go, not the button.

- [ ] **`damage_sweep.py`'s `resize-shrink Terminal` step reports a real missed damage.** Reproduces every run, no seed needed: `python3 tools/vm.py --disk <copy> start` then `python3 tools/damage_sweep.py`. The report is

- [ ] **A `sched` KTEST fails under KVM, and only under KVM.** `sched_test.c:133`'s `scheduler_poll(pid, &code) == SCHED_POLL_RUNNING` -- the "a scheduled process survives a legacy process running alongside" case. Reproduce: `python3 tools/vm.py --kvm start` then `vm.py exec "ktest sched"`. Confirmed PRE-EXISTING (2026-08-17) by stashing all local work and rebuilding: it fails identically on the committed tree, and passes every time under TCG. Almost certainly timing -- the whole suite runs in 0.9s under KVM against 4.7s under TCG, so the spawned process has already exited by the time the poll asks whether it is still running. The fix is probably to assert the process reached a terminal state rather than that it is RUNNING at one instant, but that has not been established.

- [ ] **On a machine with no invariant TSC, CPU percentages round to 0% for sub-tick work.** Accounting measures real elapsed time now (`kernel/clocksource.h`), but it can only be as fine as the live clocksource -- and where the TSC is unusable that is the 100Hz PIT, so anything finishing inside 10ms bills 0. Reproduce with `notsc` on the GRUB command line, or just boot under plain QEMU, which cannot offer an invariant TSC at all. Not a bug and not fixable in software: the honest fix is another clocksource with real resolution, which is what makes HPET (ACPI + real power/timer) worth more here than its rating suggests -- it works under plain TCG, where the TSC does not.

- [ ] **`gfxbench`'s numbers are only meaningful under KVM or on real hardware.** Plain QEMU's TCG ignores guest memory types entirely, so a write-combined framebuffer behaves exactly like a cached one and the tool reports an implausible ~17 GB/s. This is not a bug to fix -- it is a permanent property of the emulator, recorded here because it has now cost two sessions. Use `make run KVM=1` / `python3 tools/vm.py --kvm run "gfxbench 20"` for any framebuffer performance question, and treat a TCG number as evidence of nothing. `gfxbench` prints the live write-combining mechanism and whether the console is buffered beside its timings for exactly this reason. (The 2026-08-16 write-combining fix itself is SETTLED: confirmed on the maintainer's ASUS Zenbook UX305FA -- the GUI and the Shapes demo both run well now. The console-scroll regression that same change introduced is fixed and documented in `docs/decisions.md`.) **The console fix is now confirmed ON METAL too** (2026-08-16, same Zenbook, live ISO): `gfxbench 20` reports **1.8 ms** per scrolled text line against the 178.5 ms measured before it, with the console self-reporting as `buffered`. Note the bare-metal figure is ~3.6x the 0.5 ms measured under `--kvm`, and that gap is EXPECTED rather than a shortfall -- KVM honours guest memory types but its framebuffer is still host RAM, while a real one is a PCIe-attached surface where even a write-combined store is a bus transaction. So a KVM timing is the right tool for "did this get better" and the wrong one for "how fast is it"; do not quote a KVM number as a hardware target, which this file previously came close to doing.

- [ ] **`rammeter` doesn't appear at the physical console.** It ticks from `wm_render_frame()` only, so it shows on the desktop and nowhere else. The console has no repaint loop to hang it off, and the obvious hooks are both worse than the gap: drawing from the PIT IRQ can interleave with a compositor mid-blit, and hooking `keyboard_getchar()`'s wait would have a driver calling into gfx. Reproduce: boot with `rammeter` and stay at the shell -- nothing is drawn until `gui`.

- [x] ~~**Control Panel applets can't show hover.**~~ ALREADY FIXED, by the ring-3 GUI migration rather than by anything aimed at it -- recorded 2026-08-20 when the entry was checked against the code. `struct applet` no longer exists anywhere in the tree: Control Panel became System Settings, a ring-3 Toykit app whose controls are ordinary widgets, and `uui_radio_list` tracks `hovered` through its own `motion` op. The entry survived because nothing re-reads a bug list against a migration that deleted its subject.

- [x] ~~**`ui_checkbox` and `ui_radio_list` aren't in the focus ring.**~~ FIXED 2026-08-20, and the entry's names were stale: they are `uui_checkbox` and `uui_radio_list` in `userland/ui/` since the ring-3 migration. The diagnosis held exactly as written -- both were act-on-contact with no keyboard behaviour, so a tab stop there would have been a stop that does nothing, which is also why a keyboard-only user could not toggle a checkbox at all.

  Both have `key`, `set_focused` and `accepts_focus` now. **Space toggles a checkbox** and nothing else does, which is what Win32, GTK and Qt all do (Enter belongs to the default BUTTON, not to the focused control). **The arrow keys move a radio selection and that IS the commit**, because arrowing is choosing on a radio group in all three -- there is no separate commit step, and the ends do not wrap (Win32 wraps, GTK does not; not wrapping cannot jump the selection across the whole list on a key repeat). Neither is an exception to `docs/gui-guidelines.md`'s press-then-commit rule: that rule exists so a press can be cancelled by dragging away, and a key has no drag.

  **The positive control found a second, older defect.** Disabling `uui_radio_list`'s `accepts_focus` should have made Tab skip it; Tab reached it anyway, because `uui_focus_next()`/`_prev()` walked to the next index unconditionally and NEVER CALLED `accepts_focus` at all. Five widgets declared that slot and nothing read it, so a listbox with no rows or a slider with no options was still a tab stop that did nothing -- the mirror of this project's usual ops-table trap, a slot present and ignored rather than absent and needed. The ring skips refusing widgets now, bounded by the item count so a ring where everything refuses clears focus instead of spinning.

- [ ] `tools/damage_sweep.py`'s random walk sometimes drives Notepad's file picker open by clicking where its content happens to be (seed 1, step 22 did exactly that). Harmless -- the picker is a legitimate thing to have open, and the sweep now covers it -- but worth knowing when reading a failure label that says "raise" and finding a modal in the state dump.

- [ ] **`damage_hunt.py -j 4` loses VM SLOT 0 every run.** Reproduce: `python3 tools/damage_hunt.py --seeds 1 2 3 4 5 6 7 8 --random 20 -j 4` -- seeds 1 and 5 report ERROR every time (seed 1 with a `BrokenPipeError` partway through, seed 5 with a QMP `TimeoutError` at startup), while seeds 2,3,4,6,7,8 pass. Those two are exactly the seeds that land on slot 0 (`slot = index % j`), and seed 1 alone at `-j 1` passes with all 51 interactions, so it is neither seed-specific nor a kernel crash. Reported as `error` rather than counted clean since this session, so it is visible rather than silently reducing coverage. Likely slot 0's `.vm.pid`/`.vm.serial`/port 4445 being reused before the previous guest has fully gone; the other slots use per-instance names. Not diagnosed further.

- [ ] **`gui_regress.py`'s `uidemo` fails intermittently in the full parallel suite** with `RuntimeError: UI Demo reported no layout -- is this an older kernel?`, failing before its 15s spawn wait can matter. Measured 2026-08-16: roughly 3 failures in 6 full-suite runs, while `-k uidemo` alone and a 4-tool subset passed EVERY time (42/42 checks), on an unchanged kernel.

- [ ] Resizing a window by its grip sometimes doesn't take on the first drag (visible in `damage_sweep.py -v`: a `resize-grow` followed by a `resize-shrink` starting from the *same* coordinates, meaning the grow moved nothing). Not diagnosed. Possibly a minimum/maximum size clamp doing its job, possibly a grip hit-test that needs the press to land more precisely than a test does.

- [ ] **Kernel-side `fsformat tfs3` writes ~73 MB of zeroed inode tables (~3 s, and the host image loses that sparseness).** The host tool avoids it (skips fresh-image zeros, hole-punches on reformat); the kernel writes real zeros because it can't punch holes. Candidate fix: `ata_trim()` the table region instead, IF the drive guarantees deterministic-read-zero after TRIM (QEMU with discard=unmap does; IDENTIFY word 69 bit 5 is the honest gate). Until then it's a papercut, not a bug.

- [ ] The vmsvga HARDWARE cursor is off by default because it fights the relative PS/2 mouse (QEMU warps the host pointer). The display driver itself works. The configuration where a hardware cursor genuinely works is virtio-gpu + virtio-input below.

- [x] **Time sources -- DONE (2026-08-17).** `kernel/clocksource.h` registers PIT (rating 110) and TSC (300), and CPU accounting bills measured nanoseconds against whichever is live. See `docs/decisions.md`. The two pieces NOT done are both scheduled under ACPI + real power/timer, which is where they belong -- **HPET as a third clocksource** (it needs ACPI's HPET table to discover the base address) and the **`clock_event_device` half**, since timer EVENTS are still a fixed 100Hz PIT with no tickless idle. See that milestone's Details for why HPET matters more than its middle rating suggests. The original survey text follows.

- [ ] **~~Time sources -- the strongest candidate~~ (superseded above).** The tree names concrete clocks directly: `pit_ticks()` (monotonic 100Hz, the scheduler's billing unit and `SYS_TICKS`) and the TSC (calibrated in `cpuinfo`, used by `gfxbench` and the relocation path). Three call conventions, no abstraction. ACPI + real power/timer (ACPI + real power/timer) brings HPET and TSC-deadline, which is the real trigger; a Linux-style `clocksource` (monotonic, resolution, "is it reliable across sleep") is the natural shape.

- [ ] **Stack block devices rather than hooking the filesystem, for M18 encryption at rest.** `block_device` is already shaped so a device can wrap another, device-mapper style, and encryption is size-preserving so it composes cleanly -- this is dm-crypt, and writing it as a block layer instead of as TFS3 hooks is the decision that is cheap now and expensive to undo later. Same for M36 swap. Note TFS2 deliberately calls `ata_*` directly, so a stacking layer covers TFS3 only; that is fine, TFS2 is legacy.

- [ ] **M16 block checksums are NOT simply a block layer, and that is the decision to make.** A block-level checksum layer has to put the checksums somewhere -- either shrinking the device's apparent size or carving a separate metadata area -- and that is a filesystem-shaped choice, not a transparent wrapper. It is why ZFS checksums inside the filesystem (it wants them beside the block pointers, which also gets it self-healing) while dm-integrity does it at block level and pays for a metadata region. Decide WHERE THE CHECKSUM METADATA LIVES before writing either half; the layering follows from that answer rather than the reverse.

- [ ] **`block.h` has ONE ACTIVE DEVICE, mirroring the VFS's one active backend** -- the same "one active X" call made twice, in both cases when only one existed. Already in mild tension with `partition.c`, which parses MBR/GPT and can enumerate partitions that cannot then be independently mounted, and it has to give for Real mount points (real mount points). Not urgent.

- [ ] **Interfaces that exist with exactly ONE implementation are the same problem seen from the other side, and this repo already flags them as unvalidated.** `struct win_transport` is called out in `docs/decisions.md` for precisely this; `win_server_ops` gets its second implementation in M41 stage 4. These matter more than the missing-interface cases above, because a wrong guess is already baked in rather than still open -- read the decisions entry before designing stage 4 around either.

- [ ] **Keep shaped, do not build (one implementation each).** Input (`mouse.c`/`keyboard.c`, PS/2 only; second arrives with M32 USB or virtio-input), audio (`speaker.c`, PC speaker; M34 sound card), networking (nothing today; M33), fonts (baked `font_ttf.c` tables; M21 runtime loading). Each stays concrete until the second one is real.

## ~~Memory protection hardening~~

      *How it works.* `tools/genrelocs.py` extracts every absolute
      reference from a `ld --emit-relocs` link and emits it as a table
      (~7,400 fixups, 29 KB) that the kernel carries in `.krelocs`;
      `kernel/arch/x86_64/reloc.c` picks a base, copies the image,
      applies the table to the copy and repoints CR3 at the copied page
      tables. It runs from `long_mode_start`, before `kernel_main` --
      it moves the stack, so it cannot return into the frame that
      called it. The build fails if the table and the image disagree.
      `nokaslr` on the GRUB command line disables it.

      *Measured, not assumed.* 114 candidate bases on a 256 MB guest
      (~6.8 bits, against the ~6.5 predicted below); five consecutive
      boots gave five different bases, which is what makes the TSC
      fallback worth having; hardware entropy confirmed separately
      under `--cpu max`, where SMEP/SMAP also still engage. Eight
      KTESTs (suite `reloc`), and the whole suite passing on a
      randomized base -- 175 KTESTs, 8/8 ring-3 diagnostics, 3/3 fault
      tests, 13/13 GUI tools -- is what carries the claim that a
      relocated kernel actually works, since no test inside a running
      kernel can move the image out from under itself.

      *The scoping below got three things wrong, and the CR3 one is the
      interesting failure.* **CR3 DOES have to be repointed** -- not for
      any mapping reason (the boot tables identity-map the whole low
      4 GiB and already cover the new location) but because `paging.c`
      reaches them by LINKER SYMBOL, so after relocation
      `paging_enforce_wx()` writes into the copied table while the CPU
      walks the original. Nothing faults; W^X just stops applying. All
      six W^X KTESTs stay green with that step disabled, because they
      read the same symbol the code wrote -- only a check that asks the
      CPU for CR3 catches it. **`pmm.c` reserves two ranges, not one**,
      since a single span would swallow most of RAM on a randomized
      base. And **no `__bss_start` was needed**: `.bss` is copied rather
      than zeroed, which is what lets the caller keep its stack frame.

      *Still true from the scoping:* the base must stay in the low
      2 GiB (`-mcmodel=kernel`), and the copy is bounded by RAM and by
      the image's ~14.5 MB in-memory size, not by the random source.

      **The measurements below are from the 2026-08-15 scoping, kept
      because they are the part worth not repeating.**

      *Feasible, and the identity map is why.* VA==PA across the low
      4 GiB, so moving the image keeps it mapped and every PC-relative
      reference survives untouched. Linking the current objects with
      `ld --emit-relocs` and counting what lands in LOADED sections
      (`.text`/`.rodata`/`.eh_frame`/`.ktests`/`.data`) gives:

      | type | count | needs fixup |
      |---|---|---|
      | `PC32` + `PLT32` | 11,947 | no -- relative |
      | `32S` | 5,364 | yes |
      | `64` | 1,816 | yes |
      | `32` | 9 | yes |

      So **7,189 fixups**, about a 29 KB table -- small enough to embed
      in the image and apply at boot, which is Linux's `CONFIG_RELOCATABLE`
      shape (a build-time relocs tool over `--emit-relocs`, not a PIE
      link).

      *Two constraints found by measuring, not by reading.*
      `-mcmodel=kernel` (Makefile) makes every absolute reference a
      32-bit sign-extended immediate, so **the base must stay in the low
      2 GiB** -- a higher-half design is a different, much larger change.
      And the image is 2.3 MB on disk but **~14.5 MB in memory**, because
      `.bss` carries gfx's 13 MB back buffer; on a 256 MB guest that
      leaves roughly 92 slots at 2 MiB alignment, i.e. **~6.5 bits of
      entropy** (Linux's x86 physical KASLR gets ~9). Real, but modest,
      and worth deciding about before building rather than after.

      *Suggested staging, because a wrong fixup is a machine that does
      not boot.* (1) the build-time relocs tool plus a check that the
      table matches the image; (2) self-relocate with offset ZERO --
      nothing moves, so a failure there means the fixup code is wrong
      and nothing else; (3) turn on the random offset. Stage 2 is
      independently useful (it makes the kernel genuinely relocatable)
      and is the positive control for stage 3. `make debug` + GDB is the
      recovery path when a stage does not boot.

      *The three "also unresolved" items this scoping listed were all
      real, though not always for the reason given.* `pmm.c`'s
      reservation did need changing, but into TWO ranges rather than a
      wider one. `boot.asm`'s page tables living in `.bss` did force a
      CR3 reload -- not because the new location goes unmapped (it does
      not) but because `paging.c` names those tables by linker symbol,
      so after a move the code and the hardware disagree about which
      copy is live. And the copy stub did need to be position-
      independent -- satisfied by keeping it in C that runs entirely
      from the OLD image and only ever jumping into the new one at the
      end, in the two lines of assembly that adjust `rsp` and the call
      target.

### Interruptible syscalls

`int 0x80` runs through an INTERRUPT gate (`idt_set_gate(128, isr128, 0,
0xEE)`), so IF is clear for the whole syscall and a ring-3 process
cannot be preempted inside one. That is what actually freezes the
machine during disk I/O -- not `vfs.c`'s preemption guard -- and it is
why `ata.c`'s `wait_dma_irq()` polls the Bus-Master status register
instead of blocking.

The work is a trap gate (`0xEF`, leaving IF set) plus the reason that was
unsafe before: `g_next_kernel_rsp` (idt.c) is a single global "where to
resume" pointer, so a nested interrupt overwrites it while an outer
`int 0x80` handler is still on the stack. That bug has been hit twice
here -- once for `SYS_READ_KEY`, once during the ring-3 GUI migration --
and descheduling was adopted specifically to sidestep it rather than fix
it. Making syscalls interruptible means finally making it per-context.

It unlocks three things below it: a real sleeping lock in place of the
FS preemption guard, bounding how long a frame can block on I/O, and
`hlt`-based waiting inside a syscall instead of polling.

See `docs/decisions.md`'s entry on why `FS_OP()` is not a sleeping lock,
which records the full measurement -- including that a spin lock inside a
syscall would deadlock rather than merely wait.

### The compositor should use the hardware cursor plane instead of a software sprite

`display_driver` has had `cursor_define`/`cursor_move`/`cursor_show`
since vmsvga, and virtio-gpu implements them on its own queue -- but
nothing calls `gfx_hw_cursor_*()`. The ring-3 WM draws a sprite into the
framebuffer and maintains the bookkeeping that erases it again
(`prev_cursor_*`, `damage_cursor()` in `userland/wm/wm_render.c`).

Moving the pointer to the plane means a `win_proto` request the
compositor can call, and the sprite path standing down when the display
reports `DISPLAY_CAP_CURSOR` -- which is the delicate half: that
bookkeeping is what a stranded-sprite bug already came out of, so it has
to be disabled cleanly rather than bypassed. The payoff is that pointer
motion stops costing a damage rectangle, a transfer and a flush; on a
NEEDS_FLUSH device that is two virtqueue round trips per mouse move.

Until then `hwcursor` (the shell command) is the only caller, and it
exists so the plane has one at all.

### Runtime mode switching: a display driver can set a mode after boot

virtio-gpu can program a mode -- that is what its probe does, and what
makes `video=<W>x<H>` work there. It does not advertise
`DISPLAY_CAP_MODESET` because a mode change AFTER boot would allocate a
new framebuffer and free the old one, while `gfx.c` caches the surface
pointer it got at `gfx_init()` and `win_surface.c` has mapped those
frames into the compositor. Both would keep writing into freed memory.

What it needs: `display_set_mode()` re-plumbing `gfx` (a re-init against
the new surface), revoking and re-granting the compositor's framebuffer
mapping, and telling the compositor its screen changed size so it can
re-lay out. None of that is driver work; all of it is display-layer and
`win_server` work. See `docs/decisions.md`.

### Initcall levels: drivers declare a boot slot instead of being called by name from `kernel_main()`

Linux's shape: a linker section collects `DRIVER_INIT(core, foo)` and
the core runs each level in turn, with link order deciding within a
level. This repo already has the machinery -- `.ktests` is exactly that
trick.

Not built yet, on purpose. The problem ordering mistakes actually caused
was that they failed SILENTLY, and that is fixed
(`kernel/include/kernel/bootstage.h` panics naming the caller). What is
left is the ergonomics of `kernel_main()` being edited for every new
driver -- which is a cost worth paying while the list is short enough to
read, because the list is also the best documentation of boot that
exists. Build this when a third driver needs a slot and the ordering
argument is being had for the third time, not before. Note that levels
alone do not express dependencies: Linux needs `-EPROBE_DEFER` on top
for that, which is its own project.

### A Local APIC, and MSI-X interrupts on top of it

virtio-input uses legacy INTx, because MSI-X is not deliverable on this
kernel: an MSI interrupt is a memory write the device performs to
`0xFEE00000`, and it is the **Local APIC** that turns that write into a
CPU vector. This kernel has no APIC code at all --
`kernel/arch/x86_64/irq.c` is the 8259 PIC's 16 lines with a handler
chain per line.

What a LAPIC would buy, in rough order of value: MSI-X (no shared lines,
so no "was it me?" ISR read, and one vector PER VIRTQUEUE rather than
one per device); more than 16 interrupt vectors; a per-CPU timer better
than the PIT; and it is the prerequisite for SMP, which is the real
reason to want it. What it costs: enabling the LAPIC, moving IRQ routing
to the I/O APIC (which needs ACPI table parsing, or the MP tables), and
keeping the PIC path working for a machine that has no APIC.

Worth doing before a NIC, since a busy network device is where per-queue
interrupts start to matter. Not worth doing for input, which is why INTx
was the right size here.

### Re-key `/etc/kbs` layouts to evdev keycodes, removing the input core's translation table

The canonical input event is an evdev keycode
(`kernel/include/kernel/input.h`), but the keyboard layout files are
keyed on AT set-1 scancodes, so `input_report_key()` translates keycode
-> set 1 before feeding the existing state machine. The table is small
(codes 1..83 are identical; only the 0xE0-prefixed block needs
entries), and it is the seam's one piece of legacy.

Removing it means regenerating `/etc/kbs/*` keyed on evdev codes --
`tools/gen_kbs.py` already reads XKB keycodes, which ARE evdev + 8, so
it is arguably a simplification of the generator -- and changing
`keyboard.c` to switch on keycodes rather than scancodes. The PS/2
driver then translates set 1 -> keycode at its edge, which is exactly
what Linux's `atkbd` does.

Not done with the input core because it touches every layout file and
the keyboard state machine at once, and the seam is correct either way:
what a new driver reports is evdev today.
