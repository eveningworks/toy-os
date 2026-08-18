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
users.** The serial debug console is the natural candidate, since it's
already a thing that should always be running and currently just gets
polled from an idle loop. A supervisor with nothing to supervise proves
nothing, and this project has the `k_strcasecmp` precedent for what
happens to a mechanism without a caller.

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
