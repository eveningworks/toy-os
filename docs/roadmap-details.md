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

`root=` on the boot line -- `kernel/fs/vfs.c`'s `try_partitions()` mounts
the FIRST partition any volume-relative backend claims, minus the ones
whose GPT type says they are the firmware's. That is unambiguous while
TFS3 is the only filesystem and stops being so the moment FAT32 lands:
a disk with TWO ordinary partitions each holding a filesystem a backend
claims would boot the first, whichever the user meant. Real systems NAME
the root volume rather than discovering it (Linux's `root=`, then
`/etc/fstab`), and that is the fix -- not a cleverer probe order, which
would only move the guess. Wants `docs/boot-flags.md` to grow a row, and
the flag has to accept a partition INDEX at minimum; naming by GUID or
label is the better answer and needs somewhere to put the label.

The ESP half of this is already answered, and by type rather than by
guessing: the stock disk carries a FAT32 ESP (it is where the kernel
lives -- `tools/install_grub.py`), and `partition_is_firmware()` keeps
it out of the scan entirely, so a future FAT32 backend will never be
offered it. What is left is genuinely ambiguous cases.

**The root filesystem is a PARTITION, or it is RAM** -- designed in
`docs/rootfs-design.md`, not built. Three of the four roadmap items
above are its stages, and the order between them is load-bearing: a
`ramfs` backend has to exist before a flat volume can be refused, or
the refusal's failure path is a machine with no filesystem at all.

What the design settles, so it is not re-derived: "RAM-only" today is a
label on nothing (a diskless boot mounts no filesystem and every `fs_*`
call fails); ramfs is a real `fs_ops` backend over the kernel heap
rather than TFS3 on a RAM block device, because a journal and a
superblock buy nothing against memory that dies with the power; file
data is CHUNKED because `heap_os_alloc()` asks
`pmm_alloc_contiguous()`, so one buffer per file fails on a fragmented
machine while `meminfo` still shows memory free; and it takes a byte
budget (half of free RAM, tmpfs's own default) because this kernel has
no OOM killer and the ramfs would compete for frames with the allocator
everything else depends on. The doc also enumerates exactly what
refusing a flat volume breaks -- six things, including the live image,
which gains a partition table rather than an exemption.

A TFS3 test reaching double- and triple-indirect addressing -- TFS3 has
the same 12 direct + 3 indirect-level inode as TFS2, but the deepest
thing any current test touches is SINGLE-indirect (`fs_test.c`'s
"truncate cuts a file that uses indirect blocks", 20 blocks). The test
that covered the deeper tables was TFS2's own `tfs_selftest()` and was
removed with TFS2. Reaching the triple-indirect table means a file of
tens of megabytes, so this wants a host-driven tool (the shape
`tools/ls_test.py` uses to stage a 300-entry directory) rather than a
KTEST inside a booted kernel.

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

~~Mounting a filesystem from a partition, and writing a table~~ -- done.
`kernel/drivers/block/block_part.c` makes a partition a `block_device`
that shifts every LBA (Linux's `bd_start_sect`, Windows' `partmgr`), so
TFS3 mounts from one with no backend change; `vfs.c`'s `try_partitions()`
scans the table at boot and mounts the first partition a
volume-relative backend claims. Writing is `SYS_MKPART` + `/bin/mkpart`,
which take a table DESCRIPTION rather than a raw sector write -- this
kernel has no privilege model to gate a write-any-sector primitive
with. `partition.c` also stopped calling `ata_read_sector()` directly,
which had made `parttable` blind on any machine without an IDE
controller. Verified by `kernel/drivers/partition_test.c` (a GPT and an
MBR round-tripped through the writer and the parser in a running
kernel -- the live GPT proof the entry above could not get) and
`tools/partition_test.py` (four boots, two images, `df` reporting the
partition's size and not the disk's). Host side:
`seed_disk.py --partition`, `tfs3_writer.py --at-lba`,
`mkpart_test.py --layout`. See `docs/decisions.md`.

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
**The plan is `docs/signals-design.md`** (written 2026-08-20), which
covers signals, the foreground process and `Ctrl-C` as ONE problem for
the reason stated here.

**MOSTLY BUILT AS OF 2026-08-22, and the plan is
`docs/tty-design.md`.** A terminal is an OBJECT (`kernel/tty/`): an
input queue, a line discipline over it, an output sink, a `termios`, an
owner and a foreground group, with the physical console as `tty0` and a
pty per Terminal window. The INTR recognition left the keyboard driver
for `ldisc.c`, so `Ctrl-C` in a window and `Ctrl-C` on the keyboard are
one implementation -- disabling `signal_char()` reddens the checks for
both, and `Ctrl-Z` joined it later as three more lines.
Canonical mode, echo, erase/kill, `VEOF`, `termios` and the window size
all exist.

What is LEFT of this milestone: virtual terminals on
`Ctrl+Alt+F1..F4`, scrolling regions (DECSTBM), a controlling terminal
per process, and per-terminal scrollback. The ALTERNATE SCREEN was on
this list until 2026-08-24 and is done: `ESC[?1049h/l` in
`kernel/lib/ansi.c`, saved and restored by the GUI Terminal, which is
why `less` now quits to exactly the prompt it started from. Scrollback
is deliberately NOT saved across the switch, and a consumer may ignore
the sequence -- the console does.

Signals & process control can deliver a signal, but "deliver SIGINT to the foreground
process" has no meaning without a foreground process. Shell pipes & job control's job
control (`fg`/`bg`) is the same problem wearing a different hat. Doing
those first means inventing a partial answer twice.

That scoping said: a line discipline the shell reads through instead of
touching the keyboard driver; a per-terminal foreground process; then
virtual terminals as the payoff, "since once a terminal is a *thing*
rather than the only thing, having four of them on `Ctrl+Alt+F1..F4` is
mostly bookkeeping". It ended with "the GUI Terminal and the physical
console should end up as two clients of the same layer, which is a good
test that the abstraction is real."

**They did, and it was the right test** -- it is what found the two
things the design doc got wrong (a pty must be claimed by its first
READER, not its opener; and an emulator needs a non-blocking read,
because it cannot block on its window or on its child). One prediction
in that paragraph was wrong in a useful way: `klineedit.c` was NOT
"moved behind the discipline". Canonical mode was written beside it, and
every shell turns it off -- which is what `readline` does on Linux, and
is recorded at length in `docs/tty-design.md` as a cost that was chosen
rather than overlooked.

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
- Larger/growable user stack -- DONE 2026-08-23. It went 1 page -> 4
  pages -> this. A process now reserves 8 MiB of stack address space
  (`UADDR_STACK_MAX_PAGES`, deliberately Linux's default `RLIMIT_STACK`)
  and the loader maps four pages of it; everything below arrives on
  fault through the same hook that faults in heap pages, which is Linux's
  `expand_downwards()`. A fault more than `UADDR_STACK_GROW_GAP` (64 KiB,
  Linux's number) below the mapped bottom is REFUSED and logged rather
  than grown, which is what keeps a wild pointer inside the reservation
  a fault report instead of an answer -- and the pairing with
  `-Wframe-larger-than=2048` is what stops a big frame leaping the gap.
  `userland/tests/stackgrow_test.c` walks ~1.6 MiB down and verifies each
  frame on the way back out; no KTEST can see any of this, since the map
  macros are correct whether or not the handler grows anything.

### Signals & process control

**STAGES 0-2 ARE BUILT (2026-08-22): signals, process groups, and
`Ctrl-C` on the physical console.** `docs/signals-design.md` is the
authority on what landed and where it differs from what was planned;
this section is kept for the reasoning that led there, and the
paragraphs below are marked where the outcome contradicts them --
because two of them do, and one was wrong in a way worth recording.

What is LEFT: user-space handlers (a signal frame on the user stack and
a `sigreturn` to unwind it), `SIGSEGV` actually raised by the fault
handler rather than the process being torn down directly, `SIGCHLD`
actually sent on a child exit, and `Ctrl-C` in the GUI Terminal -- which
reads keys as window events, so it owns no console and has no foreground
group.

The original scoping, as written before any of it existed:

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

**WHAT THE BUILD ACTUALLY FOUND, against the four bullets above.** The
last bullet is the one that was wrong, and the third understated what
was needed:

- **"Nothing in the keyboard driver" is false, and had to be.** It is
  true that a ring-3 terminal spawning its own children knows its
  child's pid -- but the PHYSICAL shell is not that terminal. `/bin/tosh`
  blocks in `read(0)` while its job runs, so a `^C` byte typed at that
  moment is delivered to whichever process happens to read next, which
  is the shell, after the job it was meant to interrupt has finished.
  The key has to be recognised where it ARRIVES. `keyboard.c` does, and
  `kernel/tty.h` records that this belongs to a line discipline once one
  exists.
- **"The foreground process is the terminal's own state" is true and
  insufficient.** A pipeline is several processes, so the unit has to be
  a GROUP -- otherwise `cat big | grep x | less` loses one stage to a
  Ctrl-C and hangs on the rest. Groups turned out to be an int per
  process and a field compare, which is smaller than this section
  assumed a kernel concept would be.
- **"A per-process pending-signal flag the scheduler checks" needed TWO
  check points, not one.** The end of a trap alone makes death a race
  against the process reaching `exit()` first, and losing that race is
  permanent. See `docs/decisions.md`.
- **The exit status a shell can distinguish is `128 + the signal`**, the
  convention every Unix shell prints -- so a Ctrl-C'd program exits 130.
  The shell prints its own `^C`, because with a job running the line
  editor never sees the key.

The last bullet's premise still stands where it was aimed: user-space
handlers did NOT gate any of this.

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

**Most of this milestone is BUILT.** What follows was written when none
of it was, and is corrected here rather than left to be re-derived: `|`
pipes (N stages, not two), `>`/`<`/`>>` redirection, `Ctrl-C`, and
`Ctrl-Z`/`jobs`/`fg` all landed, in `userland/lib/tosh.c` and
`userland/lib/tosh_jobs.c`. The original scoping note's premise -- that
this needed a real process model first -- held exactly: every piece of
it arrived after `SYS_SPAWN`, process groups and the tty layer, and none
of it needed `fork()`.

What is left, and the one thing worth knowing about it:

- **Background jobs (`&`) and `bg`.** Not merely "the same thing without
  the wait". A background job that READS the terminal competes with the
  shell for the keyboard, which is the invisible-second-reader bug the
  init milestone already paid for once -- so `&` needs `SIGTTIN`/
  `SIGTTOU` (a background process touching the terminal is STOPPED, not
  served) in the same change, or it reintroduces it.
- Quoting, globbing, `&&`/`||`/`;`, `$?`, aliases -- the shell LANGUAGE,
  which is a separate problem from the process plumbing above and does
  not depend on any of it.

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

~~Basic image support~~ -- done: a BASELINE JPEG decoder in RING 3
(`userland/lib/uimg_jpeg.c`, behind `uimg.h`'s codec table), with
`/bin/imginfo` to inspect a file, `uui_image` to put one in a layout,
and Image Viewer to look at one. The kernel contains no image parser at
all and gained no syscall -- see `docs/decisions.md` on why this differs
from the font parser, which IS in ring 0. Progressive, arithmetic-coded,
12-bit and CMYK files are refused BY NAME (`-ENOTSUP`, with a sentence)
rather than half-decoded into a plausible wrong picture.

What it is checked against, since a decoder tested against itself is
worthless: libjpeg, in three places. `tools/uimg_hostcheck.py` compiles
the same `.c` with the host gcc and compares ~180 generated images pixel
by pixel (worst channel difference 3, which is the level two conforming
IDCTs are allowed to differ by); `/tests/uimg_test` runs nine committed
vectors in ring 3, including the three refusals; `tools/imgview_test.py`
compares the FRAMEBUFFER against libjpeg's decode of the same file. The
host harness earned its place immediately -- it showed the first
version's chroma upsampling was visibly wrong (replication needed a
tolerance of 70; libjpeg's triangle filter brought it to 3).

Measured cost, since it decides whether a wallpaper is affordable: 97 ms
to decode 1280x720 under TCG, once, at desktop start.

~~Real wallpaper images~~ -- done: `/usr/share/wallpapers` holds one
JPEG per background and two REGISTERED SETTINGS choose between them --
`desktop.wallpaper` (a filename stem, or `none`) and
`desktop.wallpaper_mode` (`fill`/`fit`). So `config set
desktop.wallpaper dusk` works from any shell, System Settings gets a row
with no edit to it, and Image Viewer's Desktop menu goes through the
same registry rather than writing a private key. The desktop notices
through the generation counter it already polls for `.desktop` files.
See `docs/decisions.md` for why the value is a NAME rather than a path,
and for the two GUI tools that had to start turning the wallpaper off.

~~Desktop icons are a letter in a tile~~ -- done: real artwork, in QOI
(the second codec, chosen for its ALPHA channel and its lossless edges
-- see `docs/decisions.md`), composited over the wallpaper at three draw
sites: the desktop grid, the Start menu rows and the taskbar buttons. A
`.desktop` entry's `Icon=` is a NAME resolved under `/usr/share/icons`,
a one-character value is still the old letter tile, and Crash Test ships
with no icon file on purpose so the fallback runs on every boot. The
artwork is drawn by `tools/gen_icons.py` and ENCODED BY PILLOW, so
nothing in this repo writes the format its own decoder reads.

Two limits, stated rather than left to be discovered: one 64x64 master
is box-filtered to every size (freedesktop keeps per-size art because a
reduction loses the silhouette -- if a 20px menu icon ever looks mushy,
that is the fix and it changes only the lookup), and there is one icon
set, so the theme mechanism cursors have is not here yet.

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

### `winshare/destroying a window poisons the compositor's mapping` KTEST fails

Measured 2026-08-20, on the commit that added the runtime font
rasterizer: **1 failure in 18 `tools/ktest_run.py` runs**, against **0 in
18** on the commit before it (`git stash push -u`, rebuild, count --
both batches via `tools/flake_hunt.py ktest`, both from a
`make clean-disk` image).

**That difference does not distinguish the two.** A 1-in-18 rate shows
zero failures in 18 runs about 40% of the time, so the earlier commit is
not exonerated by its clean batch and the later one is not convicted by
its single failure. Recorded with both counts rather than a verdict.

Reproduce with `python3 tools/flake_hunt.py ktest -n 20`; the failing
check names itself in the summary. No cause was established. Two things
that would plausibly matter and were not tested: the KTESTs added in
that commit rasterize several hundred glyphs while a desktop is live,
which is a real timing perturbation in the area this check measures; and
the check runs against a compositor whose window is being destroyed, so
it is sensitive to when the compositor next drains its event queue.

The check itself guards a real invariant with a known history -- a
revoked compositor mapping must be poisoned rather than left as a hole
(see `docs/decisions/gui.md`) -- so a failure here is worth taking
seriously rather than raising the tolerance.

### Runtime font loading & text metrics

**Most of this shipped on 2026-08-20** -- `kernel/lib/ttf.c` parses and
rasterizes a TrueType face in fixed point, `kernel/drivers/font_face.c`
is the `/usr/share/fonts` registry, `fontface`/`fontsize` switch face and
size (any size, not just a baked one), and text is MEASURED through
`gfx_char_advance()`/`ugfx_char_advance()` rather than multiplied.
`docs/decisions/drivers.md` and `docs/decisions/gui.md` carry the
reasoning; what follows is only what is still missing, and why each piece
is where it is.

**The 101-glyph ceiling is the one that actually limits the feature, and
it is not a font problem.** A runtime atlas rasterizes exactly the set
`tools/genttf.py` bakes -- ASCII 32-126 plus six Nordic letters -- so
DejaVu Sans Mono's other ~3,270 glyphs are loaded, parsed, and
unreachable. That set is deliberate and is the same one everywhere, which
is what makes an atlas a drop-in for a baked variant; widening it means
answering "what is a character?" first, and this OS says Latin-1 (see
`docs/decisions/drivers.md`'s Nordic/Latin-1 entry). So this unblocks
behind UTF-8 migration rather than inside this milestone. Until then the
honest description is a real font renderer drawing a 1980s character set.

**Weights are blocked by there being one active face.** `font_face.c`
holds one selected face and one atlas per size of it, so `-Bold.ttf` can
be *chosen* as its own face but cannot be drawn BESIDE the regular one --
no bold window title, no emphasised label. That is the same gap as
"multiple faces live at once, selected per widget", and weight is its
first real caller: a `uui_label` that could ask for bold would justify
the API, where "an app might want a different font" never quite did.

**Hinting is the cheapest visible win left.** Glyphs are rendered
unhinted -- outlines scaled and filled with no grid-fitting -- so below
about 10px stems land between pixel centres and the result is muddier
than the baked font at the same size, which FreeType hinted offline. This
is worth knowing before treating small-size rendering as a bug: it is a
missing feature with a name.

**Kerning** (`kern`/`GPOS`) matters only once proportional faces are in
normal use; `AV` and `To` sit visibly wrong, and nothing on the desktop
uses a proportional face by default.

**Moving the parse out of ring 0** is the security item, and it is not a
font task. A `.ttf` is untrusted input being parsed in the kernel -- the
surface Windows spent a decade of GDI CVEs on before Windows 10 moved it
to `fontdrvhost` -- and `ttf.c` is bounds-checked throughout because of
it. Actually moving it needs a way to hand a rasterized atlas from a
ring-3 process to the compositor and the console, i.e. shared memory this
OS does not have. Doing it without that would mean copying an atlas
through a syscall per font change, which is the wrong shape.

**Composite 2x2 transforms** are skipped: a component glyph is placed by
its offset and drawn at natural size. Every accent in both shipped faces
is a pure translation, so nothing renders wrong today, and this is
recorded so that a face which DOES scale a component is a known
limitation rather than a mystery.

**Complex-script shaping** -- bidirectional text, ligature substitution,
combining marks -- stays out. It needs UTF-8 first, and pretending
otherwise would put a dependency here on something far below it.

Two bounds that are decisions rather than gaps, stated so they are not
mistaken for bugs. The atlas cache holds 8 entries / 4 MiB and REFUSES a
build past either, keeping the current size drawing rather than evicting
an atlas a client still has mapped. And the cell is measured with
`genttf.py`'s tightening formula so a runtime face at 14px matches the
baked 14px to within a pixel -- layout everywhere is font-derived, so
disagreeing would reflow every window the moment a face was selected. The
cost is the slight accent and descender clipping every fixed-cell
terminal font accepts.

### Desktop productivity apps

~~File manager app -- needs a proper filesystem API surface first~~ --
done 2026-08-23, and **the precondition this entry named had already been
met without anyone updating it**: `SYS_LISTDIR`, `SYS_FSTAT`,
`SYS_MKDIR`, `SYS_RENAME`, `SYS_UNLINK`, `SYS_CHDIR` and `SYS_LSEEK` all
shipped in the meantime, so the "fixed ad hoc calls" this warned about
were already gone. `docs/filemanager-design.md` is the full writeup: it
is a two-pane COMMANDER rather than an Explorer, because copy and move
between two visible directories need neither the clipboard nor
drag-and-drop, and this system has neither.

**The folder tree following the active pane was built and REVERTED**
(2026-08-30), and the reason is the design, not the code. Following
means expanding every ancestor of wherever the pane is; the existing
tree also lets you COLLAPSE a branch by hand and expects it to stay
collapsed. Those two are in direct conflict the moment you collapse a
branch you are standing in -- and the tree's own tests assert the
collapse half, so the follow turned two passing checks red.

KDE's Folders panel resolves it by moving the SELECTION up to the
branch you collapsed rather than undoing the collapse, and that rule is
the missing piece. The reverted attempt otherwise settled four
questions worth keeping: follow always (Dolphin's behaviour; Explorer
hides the same thing behind "Expand to open folder", off by default),
auto-expanded ancestors STAY open, the node and open-path arrays grow on
demand rather than at fixed caps of 96/24, and the tree expands to the
restored directory at startup rather than sitting collapsed at `/`
contradicting the pane before the window is touched.

Nothing is left of this entry: the icon view landed 2026-08-28 as
`UUI_FILEVIEW_ICONS` (a mode of `uui_fileview`, not a new widget),
reusing `api/icon_grid.h` for cell geometry and giving
`api/rubberband.h` the second caller it was shaped for -- the sweep's
selection is applied straight onto the marks the multi-selection work
had already put in the widget.

~~That filesystem API surface also needs seek~~ -- `SYS_LSEEK` exists,
and Doom (which needed it to read a WAD's lump directory) runs on it.

Desktop calendar: DONE 2026-08-24. A popup panel above the taskbar,
opened by clicking the tray clock, showing a month grid with today in
the accent, `<`/`>` paging and a title that snaps back to today
(`userland/wm/calendar_popup.c`). Days are not clickable -- there are no
events to select one for. The week's first column is
`desktop.week_start`.

The plan here said "a reusable `widget_calendar` piece the same way
`widget_scrollback`/`widget_button` are", and that was stale twice over
by the time it was built: that widget set (`apps/ui/`) was deleted when
the desktop moved to ring 3, and the window manager hosts no `uui`
router at all -- it draws with `ugfx` like `start_menu.c` beside it. So
the grid is drawn by the popup, and a `uui_calendar` widget waits for a
second real caller. See `docs/decisions.md`.

Control panel window with pluggable "applets" (Windows-style) -- first
applet: display settings (font size + color theme), since both already
exist as the `fontsize`/`color` shell commands, so the applet is mostly a
GUI wrapper around logic that's already implemented and tested.

### GUI clipboard + drag-and-drop

The clipboard half is BUILT, and it landed differently from the sketch
this section opened with -- worth stating, because the sketch is what a
reader would otherwise plan the rest against.

**It holds FILES, not text, and the SERVER holds it.** The sketch said a
kernel-space buffer of plain text pasted by whichever widget has focus.
What the File Manager actually needed first was a set of PATHS, and the
buffer lives in the window server so a copy survives its source being
Force Quit -- which is the whole reason X11's selection-owner model is
considered a mistake and why Wayland's `wl_data_device` has the
compositor hold the data. `WIN_REQ_CLIP_SET`/`_GET`, `lib/uclip.h`, and
`WIN_EV_CLIPBOARD` to say it changed. Text is still unbuilt and is a
second FORMAT on the same buffer, not a second buffer.

**Ctrl+C/X/V are the APP's keys, not the WM's.** Ctrl+C is INTR in a
terminal, so a compositor that grabbed it would break the shell; every
desktop leaves these to the focused client for the same reason.

What remains, and what each needs:

- **Text on the clipboard**, and paste into Notepad/Terminal. Typed
  formats first (`text` vs `files`), since a paste has to know what it
  is getting.
- **Drag-and-drop within one window** -- the honest first step, and
  bigger than it looks. `uui_fileview` already owns press-and-move on
  empty space for the rubber band, so a drag on a ROW has to be told
  apart from a band on empty space; then a drop target that highlights,
  and a move-vs-copy rule (Windows: move within a volume, copy across;
  KDE asks with a menu). Testing it needs a click aimed at a chosen row,
  which the harness cannot yet do reliably -- see the Shift+click item.
- **Drag-and-drop BETWEEN windows** -- extends the above with a WM-level
  "carrying a payload" state, since the pointer leaves the source
  client's content area and the target must learn what is being offered
  before the release. This is where the clipboard's typed formats get
  reused rather than a second mechanism invented.
- **A clipboard history ring**, once there is more than one format.

### Runtime + interop

Inter-process IPC (message passing) -- today's ring-3 processes are
isolated from each other with no way to communicate.

A real C library on top of `filetest`'s fd-aware syscalls. **Most of
the runtime this paragraph once listed as missing now exists** -- crt0
with argc/argv/envp off a SysV stack (`userland/rt/crt0.asm`), a typed
syscall layer (`userland/rt/sys.h`), FPU/SSE context-switch
save/restore (eager FXSAVE/FXRSTOR in `scheduler.c`), and malloc/free
(`kernel/lib/heap_core.c` compiled twice over a demand-paged `SYS_SBRK`
that is no longer single-process-only). What is left is the libc
itself, staged in `docs/libc-design.md`.

TLS (FS.base) is still absent, and is the reason
`kernel/lib/stack_protector.c`'s stack-canary guard uses
`-mstack-protector-guard=global` instead of GCC's normal TLS-based
default -- confirmed directly, not theoretical, see
`docs/decisions.md`. **It is NOT a libc prerequisite**, despite having
been listed as one: with no threads, `errno` is a global behind
`__errno_location()`, which is what musl itself does.

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
produce that need. Both head starts this entry predicted have since been SPENT, and
correctly: the per-partition iteration is in the probe loop
(`try_partitions()` in `vfs.c`), and mounting TFS3 from a partition
needed no backend change at all -- the offset went into a
`block_device` wrapper (`block_part.c`) rather than into the seam, so
TFS3's volume is still `{0, blk_sector_count()}`. (TFS2 stays
absolute/flat-only, and declares `volume_relative = 0` to say so.)
What that did NOT buy is this milestone: one partition is mounted at a
time, exactly as one whole disk was. The remaining work is unchanged.
The per-backend-statics warning below applies to `tfs3.c` exactly as it
does to `tfs.c`.

**BUILT.** `kernel/fs/mount.c` holds the table and the boot policy;
`kernel/fs/vfs.c` resolves a path to a mount and forwards. The five
rules are in `kernel/mount.h` and the reasoning is in
`docs/decisions/storage.md`. TFS3 serves `/`, FAT32 serves `/boot`
(read-only by default), and `mount -t ramfs none /mnt` is a third.

**The suggested first proof was a second TFS3 image at `/mnt`, and that
is the one item still open** -- for the reason this entry predicted.
Several `tfs3.c` statics (the superblock, the group descriptors, both
bitmaps, the journal buffers, the block scratch) are per-*backend* state
that a second instance would need its own copy of, and so are FAT32's FAT
cache and directory scratch. Every backend declares
`fs_ops.max_mounts = 1` and the table refuses a second mount BY NAME,
which turns what would have been a silent data-loss bug (one set of
statics repointed, the first mount reading the second's volume) into a
refusal.

What replaced it as the isolating proof is **ramfs at `/mnt`**: a third
backend with no volume at all, mounted and unmounted by
`kernel/fs/mount_test.c` on every ktest run, exercising the same
dispatch with none of FAT32's code in the path.

Raising `max_mounts` needs more than per-instance state. Every op in
`struct fs_ops` takes a PATH and no handle, so a backend cannot tell
which of its mounts a call belongs to; the shape that fixes it is
`init()` returning an opaque handle that every op then takes -- Linux's
`super_block`. It buys nothing until a backend's state is per-instance,
which is why it was not done at the same time.

What DID have to change, and was not predicted here: `probe()`,
`format()`, `wipe()` and `init()` take a `struct block_device *` now
instead of reading `blk_active()`. With two mounts there is no single
active device a backend could correctly assume. See
`docs/conventions/storage.md`.

Two things this entry warned about that turned out to cost nothing:
relative paths and `..`. Every path reaching `fs_*` is normalized
before it arrives (`api/fs.h`), so `..` is gone before any mount is
consulted, and `cd` across a boundary is the shell's lexical
normalization doing exactly the right thing. `fs_list()` on a directory
containing a mount point works because the mount point is a REAL
directory on the parent filesystem -- rule 3 requires it.

### FAT32

**BUILT** on 2026-08-25 -- `kernel/fs/fat32.c`, read-write, with VFAT
long names, mounted at `/boot`. What follows is what it deliberately is
NOT, because every line of it is a decision rather than an oversight and
the next session should not have to re-derive them.

**Not FAT12 or FAT16.** Those are not "FAT32 with smaller numbers": the
root directory is a fixed-size region rather than a cluster chain, and
the FAT entries are 12 or 16 bits with a nibble-packed edge case. That
is a second set of paths through every function in the file, for a
format nothing on this machine uses. A volume that is not FAT32 is
REFUSED by name at `probe()` -- a parser rejects rather than guesses.

**Not 4096-byte sectors.** The block layer speaks 512, and reading a
4K-sector volume with a 512 stride produces plausible garbage rather
than an error, which is the worst available failure.

**Not Unicode.** Long names are read as UCS-2 and anything outside ASCII
becomes `?`; creating such a name is REFUSED, because it could never be
looked up again. This kernel has no Unicode anywhere else either, and a
half-done encoding is worse than a stated limit.

**Not journalled, because FAT is not.** An interrupted write leaves
whatever the last completed sector left. There is no analogue of TFS3's
credit-counted transactions to add -- the format has nowhere to put
one. That is a real reason to prefer TFS3 for anything that matters,
and the reason `/boot` is mounted read-only by default.

**`check()` is report-only.** It finds cross-linked clusters and
pointers outside the volume, and repairs nothing. A real FAT repair
means rebuilding a lost-cluster list and reconciling the FAT copies,
which is `fsck.vfat`'s whole job; a half-repair on the partition holding
the bootloader is worse than a clear report.

**The `ino` is the first cluster**, so every empty file shares 0 --
which is exactly why `fat32_ops` does not claim `FS_CAP_INODES`. FAT has
no inode number to report.

**A cluster count below 65525 is accepted, and that is out of spec.**
The threshold exists so a driver implementing FAT12/16/32 can tell them
apart; this one implements only FAT32 and discriminates on the BPB's
FAT32-only fields (`fat_size_16 == 0`, `root_entry_count == 0`, a
nonzero `fat_size_32`). It is what lets `kernel/fs/fat32_test.c` use a
512 KiB image rather than a 34 MiB one -- which matters because the test
volume is a `kmalloc()` and `pmm_alloc_contiguous()` fails on a
fragmented machine long before it fails on a small one. `format()` still
picks a cluster size clearing the floor wherever the volume allows, so
nothing toy-os WRITES is out of spec.

**The steppable read/write pair completes in `begin()`** rather than
genuinely stepping, as ramfs's does. Doing it properly means a second
write path through the chain walker, for a mount that is read-only by
default and holds a bootloader.

**Updating the machine's own kernel is NOT done.** Writing
`/boot/boot/kernel.bin` from inside toy-os works and is verified, but
nothing reinstalls GRUB's `core.img`, nothing verifies the image is a
kernel before overwriting the one that boots the machine, and nothing
keeps a known-good copy to fall back to. Those three, not the write, are
what "toy-os can update itself" would mean.

**The path is `/boot/boot/kernel.bin`, not `/boot/kernel.bin.`** The ESP
holds a `boot/` directory because `tools/install_grub.py` keeps the
ISO's paths exactly, so ONE `grub.cfg` serves both media. Mounting the
volume shows it as it is -- the same way a Linux ESP at `/boot/efi`
shows `/boot/efi/EFI/...`. Flattening it (kernel at the volume root,
prefix `(hd0,gpt2)/grub`) would read better and would cost that
shared-config property; it is a real fork, not an oversight.

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

**BUILT** -- `kernel/drivers/ahci.c` and `kernel/drivers/block/block_ahci.c`.
A SATA drive behind a host bus adapter carries the root filesystem, with
DMA transfers and interrupt-driven completion, on
`make run DISK=ahci`. `tools/ahci_test.py` is the only thing that
exercises it, and `docs/decisions/drivers.md` carries the three design
calls (why one port is driven and every port is reported, why there is
no sector cache, why `noahci` is a precedence word rather than a kill
switch).

What is deliberately NOT built, and what each would actually need:

- **NCQ.** The real reason AHCI outperforms IDE, and it needs an
  ASYNCHRONOUS block interface rather than more AHCI code:
  `struct block_device` has no submit/complete split and every caller
  above it issues one transfer and waits. Adding slot allocation and
  `PxSACT` handling today would keep exactly one command in flight,
  which is what slot 0 already does with none of it.
- **A second drive.** The driver already enumerates and reports every
  port; what it cannot do is register two block devices, because
  `blk_active()` is singular and this OS has no `/dev`. Real mount
  points made two MOUNTS possible, not two disks.
- **A shared sector cache.** `ata_cache.c` is ATA-private on purpose
  (its header says why). Generalising it to hold a `block_device` is
  the shape, and the bar is a measurement rather than symmetry --
  virtio-blk has been uncached and faster than ATA throughout.
- **A fault-injection hook that makes the DRIVE refuse a command.**
  `port_recover()` exists because a fatal error clears `PxCMD.ST` and
  nothing restarts it, so without it one bad sector wedges the disk
  permanently -- and it is the one path in the driver that no test
  reaches. The bounds check refuses a bad LBA before the drive sees
  one, and QEMU produces no media errors. `fault_inject.h` fails a
  transfer from the KERNEL side, which is the opposite direction: what
  is missing is a way to make the DEVICE answer with `PxIS.TFES` set.

- **Hot-plug and surprise removal.** `PxIE` masks the PHY and
  hot-plug bits today, because a handler with nothing to do about the
  event is worse than no handler. It needs the block layer to be able
  to say "this device is gone" to a mounted filesystem, which is the
  same missing piece as surprise removal of a USB stick.
- **Port multipliers.** Detected and reported by signature already
  (`/bin/ahci` names one); speaking to the devices behind it is the
  part that is not built.

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

**Table parsing, poweroff and reset are DONE (2026-08-30).**
`kernel/acpi/` finds the RSDP (multiboot2 tag 14/15, with a BIOS-area
scan as the fallback), walks the RSDT or XSDT, and decodes the FADT, the
DSDT's `_S5_` object and the MADT. `system_poweroff()` now writes the
sleep type this machine's own firmware named, to the port its own FADT
named, which is what makes shutdown work on VirtualBox and real
hardware rather than only on QEMU; `system_reboot()` tries the FADT's
reset register before the 8042 pulse. `/bin/acpi` prints all of it and
`tools/poweroff_test.py` asserts that the log line naming the parsed
path is the one that ran. The MADT half is `docs/smp-design.md`'s
Stage 1, so the processor list exists and nothing is started on it.

What is NOT done here is everything that needs an AML interpreter
(battery, thermal, S3) -- see `docs/decisions.md` on why the `_S5_`
byte scan is the one deliberate exception -- plus HPET, whose table is
now found and whose registration is described below.

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

**That blocker is GONE as of 2026-08-30.** `kernel/acpi/` walks the
tables and `acpi_find_table("HPET")` answers; `/bin/acpi` lists the
table on any QEMU machine here. What is left is a driver: read the
32 bytes the HPET table carries (base address, the counter's minimum
tick, the block id), map them uncached, work out the period from the
capability register and call `clocksource_register()`. Nothing else in
the tree is in its way.

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
**`docs/smp-design.md` is the full design**, staged so each step ships on
its own -- ACPI tables, then the Local APIC, then application processors
parked, then a real spinlock and one kernel lock, then the scheduler,
then TLB shootdown, then splitting the lock in measured order. It also
carries the measurement of what in the tree is single-core today, and
the honest case AGAINST doing this at all.

**Stage 1 is DONE (2026-08-30)** -- `kernel/acpi/` walks the tables and
the MADT, `QUERY_CPUS` lists the processors, and `/bin/lscpu` counts
them. It landed because SHUTDOWN needed the FADT, not because SMP was
started; nothing runs on the other cores and every entry reports
`online: no`. The prediction that made it look cheap held exactly: no
AML interpreter, four files.

**The finding that still shapes the rest** is the BKL. It is not a
mistake to avoid: it is what makes SMP shippable before the locking
audit is finished, which is exactly the position Linux 2.0 was in and
exactly the position this kernel is in now.

**What threads bought it** (done 2026-08-26): the scheduler entity is
now the right object for a per-CPU run queue to hold, and two runnable
threads of one program is the first real reason to want a second core.
Threads also create the first TLB shootdown hazard, since two cores can
now be in one address space.

### Confirm the xHCI BIOS handoff on the laptop it was written for

**CONFIRMED 2026-08-28**, on the machine itself (Intel `8086:9cb1`,
Wildcat Point-LP): the live ISO boots past USB bring-up and an external
Logitech wireless receiver works -- which exercises the handoff, the
composite multi-interface binding and the hardware-only bring-up paths
(port power, 64-byte contexts, scratchpad allocation) in one boot. The
bug entry is deleted per docs/bugs.md's rule; `git log` has its history,
and `docs/decisions/drivers.md`'s handoff entry carries the reasoning.

### `pci_bar_mem_size()`, so the xHCI capability walk is bounded by the real BAR

`walk_xecp()` follows a device-supplied chain whose every `next` is up
to 255 DWORDs, so 64 hops can reach ~65 KB from the capability base --
past a typical 64 KiB xHCI BAR, into MMIO nothing decodes, which on real
hardware is an unclaimed cycle rather than a polite 0xFFFFFFFF.

It is bounded today by `XHCI_XECP_MAX_OFF` (a flat 64 KiB) and an
all-ones check that stops the runaway one bad read would start. Both
stand in for the fact that would settle it: **how big the BAR actually
is.** `pci_internal.h` has `pci_bar_mem_addr()` and no size.

The probe is standard and belongs to PCI, not xHCI: disable memory
decode, write all-ones to the BAR, read back, restore the value and the
decode bit; the size is the low set bit of the returned mask. Getting
the restore wrong un-maps a working device, which is why it wants one
implementation with one caller-visible answer rather than open-coding in
a driver.

**Not urgent for correctness on the machine that prompted it** -- its
capabilities all sit between +0x8000 and +0x8480, well inside any
plausible BAR, and its hang was the BIOS handoff. This is the guard
becoming real rather than heuristic.

### USB (keyboard/mouse)
**BUILT** for xHCI, a HID boot keyboard and a HID boot mouse; see
`docs/conventions/kernel.md` and `docs/decisions/drivers.md`. What
remains is listed on the roadmap: the legacy-support handoff (hardware
only), mass storage, hubs, and report-descriptor parsing.

**FOUR THINGS THE PLAN BELOW GOT WRONG**, kept because the corrections
are the useful part. (1) Step 4 names `SET_ADDRESS`; there is no such
request on xHCI -- addressing is the *Address Device* command with an
Input Context, and the controller emits the wire request itself, so the
order is Enable Slot, Address Device, *then* GET_DESCRIPTOR. (2) Steps 6
and 7 predate the input core and say to feed `keyboard.c`/`mouse.c`
directly; the real work is registering an `input_source` and calling
`input_report_*`, which made both steps far smaller than estimated --
and, because `/etc/kbs` is keyed on evdev now, a USB keyboard needs no
layout table at all. (3) Step 8 conflates the xHCI USB Legacy Support
capability (a controller ownership semaphore) with BIOS/SMM PS/2
emulation (what actually produces ghost keys); they are unrelated and
both are hardware-only. (4) There is no PS/2 "handoff" to order against:
the input core is a multi-source registry by construction, both paths
stay registered, and Linux does the same with `atkbd` and `usbhid`.

The original plan follows.

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

BUILT through the PCM path, 2026-08-29: `kernel/drivers/sound/` holds
the class registry (`sound.c`, one exclusive stream, the shared ring of
`abi/sound_abi.h`) and the AC'97 driver; `/tests/tone` plays A440 and
`tools/audio_test.py` measures it in a host-side recording. Left: the
WAV player app and a mixer UI (the `volume` setting already gives
System Settings a Sound row). The original breakdown, kept for the
items still open:

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

**MEASURED 2026-08-27, so the starting point is known rather than
assumed.** Booting `toy-os.iso` under OVMF
(`/usr/share/edk2/x64/OVMF_CODE.4m.fd` as pflash) fails in two stages,
and only the first is fixed:

1. `video/video.c:grub_video_set_mode:782:no suitable video mode found`,
   because none of the `grub*.cfg` files loaded a video driver. GRUB's
   i386-pc core image has VBE built in, so a BIOS boot finds an adapter
   by luck; the EFI build reaches `efi_gop` only if something insmods
   it. **Fixed** -- `insmod all_video` and `set gfxpayload=auto`. That
   was a real bug on any EFI-booted medium and worth fixing on its own.
2. GRUB then **page-faults inside the firmware** and the kernel never
   runs -- no `kernel_main reached` on the serial line. So toy-os still
   does not boot under UEFI at all, and stage 1 only removed the error
   that was hiding stage 2.

Do not read "the video fix works" as "UEFI boots". It does not. What
that fix bought is a UEFI attempt that fails somewhere informative
instead of at the first mode set.


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

**The full staged plan is `docs/libc-design.md`** -- what exists today
measured against the tree, what the three remaining syscalls are, and
six stages that each ship on their own. Read it before starting any of
this; it exists because the gap was being re-derived (and got wrong in
the same two directions) every time.

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
- [x] ~~**SSE, and FPU state across a context switch.**~~ DONE, and it
  was the item most worth doing early: every real libc's
  `memcpy`/`strlen` uses SSE2 unconditionally on x86-64, so without it
  the first stock-compiled binary faults or silently corrupts another
  process's registers. `fpu_init()` clears CR0.EM and sets CR4.OSFXSR
  (`kernel/arch/x86_64/fpu.c`), `USERLAND_CFLAGS` carries no
  `-mno-sse`, and `scheduler.c` FXSAVEs/FXRSTORs EAGERLY on every switch
  -- eagerly because CVE-2018-3665 is what lazy FP restore costs, and
  Linux deleted its lazy path in 4.14. Both halves are tested:
  `/tests/fpu_test` proves FP works at all, `/tests/fpu_race` proves two
  preempted processes do not share the register file.
- **`time_t`.** Timestamps are broken-down local `struct rtc_time` with
  no stored UTC offset, on purpose (`docs/decisions.md`), which is
  exactly the field needed to convert an existing one. The conversion
  exists since TFS3: an inode layer (`tz_rtc_to_epoch()`/`tz_epoch_to_rtc()`,
  tz.c), `fs_stat()` reports epochs on both backends, and TFS3 stores
  them natively with inode room reserved -- what this item still owns
  is the STORED UTC offset and true-UTC semantics.
- **The unglamorous syscall surface**, most of which has since been
  built: `dup`/`dup2`, `pipe`, path `stat`, `getpid` (via
  `SYS_PROC_INFO`), a monotonic clock and a per-process `chdir`/`getcwd`
  all exist now. **THREE ARE STILL ABSENT, and stdio needs all three**:
  `lseek` (there is no seek at all -- file I/O is
  open-then-sequential-read), `fstat` on an open fd (only paths can be
  stat'd), and `O_APPEND` (`SYS_O_*` is WRITE/CREAT/TRUNC only).
  `isatty` is a field of `fstat`, not a call of its own.
  **All three landed** (Stage 0 of `docs/libc-design.md`), with
  `/tests/seek_test` as the proof and `docs/decisions.md` recording what
  giving an fd's position a meaning for WRITES changed underneath.
- [x] ~~**`crt0`.**~~ DONE -- `userland/rt/crt0.asm` is shared, and a
  program is a `main()` over `userland/rt/sys.h`'s typed wrappers. What
  it still hands `main()` is an envp of exactly NULL: there is no
  environment, and `SYS_SPAWN` has nowhere to put one.
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
boot. **DONE 2026-08-25** -- see "Real mount points" above; the original
backlog note follows. The VFS layer supports exactly one active backend today -- a real
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

Stretch: port a small classic game (e.g. Doom) -- **DONE 2026-08-23.**
`/bin/wm/apps/doom`, an ordinary ring-3 `uapp` client at 640x400
(doomgeneric's own 2x scale of Doom's 320x200), measured at ~34 fps
against Doom's own 35Hz target under TCG. `userland/ports/doom/` is
doomgeneric byte for byte; `userland/backends/doom/` is the backend.

**WHAT IT ACTUALLY NEEDED, measured rather than assumed.** Three things
were built for it in advance and only one was required:

- **Key releases: REQUIRED.** `DG_GetKey()` asks for an edge, and Doom's
  stock controls put fire on Ctrl, run on Shift and strafe on Alt --
  three of five are modifiers, which produced no client-visible event at
  all until they became keys.
- **The 1 MiB image ceiling: NOT required.** 0.72 MiB after
  `--gc-sections`. The 770 KB measured while planning was the sum of the
  object files' sections, and the linker drops what nothing reaches.
- **The growable stack: NOT required.** Disabling `grow_stack()` and
  running 1,750 frames at ~34 fps produced no fault; Doom fits in the
  original four pages. This entry previously flagged the fixed stack as
  the risk and said it was "untested whether Doom's actual stack depth
  would exceed it". It does not.

**And the bug it found, which is why porting somebody else's program is
worth doing at all**: `kfmt.c` parsed `printf` precision and ignored it
for integers, so `"STCFN%.3d"` of 33 gave `STCFN33` and the game died on
a lump that does not exist. That formatter is compiled into both rings
and had tests; the tests encoded the bug. See `docs/decisions.md`.

**What is deliberately not there:** sound (`i_sound.c` resolves to
silence on its own -- an audio driver EXISTS now, and wiring Doom to
the PCM ring is its own roadmap item under Sound), mouse look (Doom aims with
relative motion and a windowed client gets position, which would need a
pointer grab TWS does not have), and resizing.

**What a port still has to decide or build:**

- **The WAD.** Doom will not run without an IWAD. The shareware
  `doom1.wad` is ~4 MB; Freedoom is BSD-licensed and ~11 MB. Where it
  lives (in the repo, fetched at build time, or supplied by the user)
  is a licensing question as much as a size one.
- **The backend**, `DG_Init`/`DG_DrawFrame`/`DG_SleepMs`/`DG_GetTicksMs`/
  `DG_GetKey` over `uapp`, plus a palette-to-RGB conversion and an
  integer scale from 320x200.
- **Vendoring**, in `userland/ports/doom/` beside `userland/ports/cjson/`
  and linked per-binary through `EXTRA_OBJS_`, so nothing else in the
  tree can depend on third-party code. doomgeneric is GPL-2 in an MIT
  repo -- an aggregation, which is fine, and worth saying out loud in
  `LICENSE` rather than leaving to be inferred.
- **Frame rate under TCG**, which nothing here can predict and only a
  running port can measure. Every automated test in this repo runs TCG
  (see CLAUDE.md), so this is exactly the class of question a green
  suite says nothing about.

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

- [x] ~~**Blocking + wait queues.**~~ DONE 2026-08-20 -- a wait channel is an ADDRESS, so a wake reaches one pipe or one client rather than a category. The roadmap ticked this and this list did not, which is the drift a duplicated item always produces.

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

**Needs:** nothing for the next item -- the stack, both NIC drivers and `ping` are built.

**Items, in full.**

- [x] ~~NIC driver~~ -- the roadmap said "rtl8139 first", which predated anyone measuring what the emulator offers. QEMU's default `pc` machine has always attached an **e1000 with user-mode networking** when no `-net` option is given, so every guest this project ever booted had an unclaimed NIC on the bus; that made it the card a driver can assume. **virtio-net landed in the same change**, because an interface with one implementer gets shaped around that implementer and the previous four registries here all had to be re-cut for exactly that reason -- see `docs/decisions/drivers.md`.

- [x] ~~Ethernet/ARP/IP stack~~ -- `kernel/net/`, in the kernel rather than a ring-3 service: sockets would otherwise become IPC to a daemon and this OS has no IPC that can carry them (no unix sockets, `PIPE_MAX` is 8 kernel-wide, a pipe carries no credentials). NO fragmentation in either direction; routing is two rules per device. `docs/decisions/kernel.md` has the full argument including the case against.

- [x] ~~ICMP echo + a `ping` command~~ -- `/bin/ping` and `/bin/ifconfig`. The proof is `tools/net_test.py`, whose oracles are both on the HOST: SLIRP answers the echoes and shares no code with the guest, and every frame is dumped to a pcap and decoded with the IPv4 and ICMP checksums recomputed there.

- [x] ~~**UDP**~~ -- ports, the pseudo-header checksum, an ephemeral range for unbound senders, and ICMP port unreachable for a datagram nobody wanted. The checksum is the part worth knowing about: it covers a 12-byte pseudo-header that appears in no packet, so a stack that omits it agrees with itself perfectly and is rejected by everything else -- which is why `tools/net_test.py` recomputes it on the host rather than trusting a round trip.

- [x] ~~DHCP client~~ -- `/bin/dhcp`, a ring-3 program: DISCOVER/OFFER/REQUEST/ACK, applied through `SYS_NET_CONFIG` (the same call `ifconfig` uses, so it holds no privilege a person does not). It needed two things from the layers below: a socket bound to a DEVICE rather than just a port (Linux's `SO_BINDTODEVICE`, because the client broadcasts before any card has an address), and a broadcast path that works from an unaddressed device with 0.0.0.0 as its source. **The proof is a guest booted on 192.168.76.0/24** -- on QEMU's default network a working client and the hardcoded 10.0.2.15 are indistinguishable.

- [x] ~~DNS resolver~~ -- `userland/lib/uresolv.c`, shared by `/bin/host` and `/bin/ping`, with the server read from `/etc/resolv.conf` (Unix's path, this repo's `key=value`). The parser is the interesting half: a DNS name can end in a POINTER to an earlier offset, which lets a reply point a name at itself, so every walk carries a jump budget; and every answer is examined rather than the first, because a CNAME answers with the alias record *and* the address record.

- [x] ~~**A blocking receive**~~ -- the wakeup question is answered by having the driver's ISR wake ONE channel for the whole stack and the woken reader run `net_poll()` itself, so protocol code still never runs in interrupt context. `scheduler_block_current_until()` is the new general primitive (Linux's `schedule_timeout()`), and the deadline lives on the SOCKET because a blocking syscall here is re-run rather than resumed. Four clients lost their poll loops. It also exposed a pre-existing bug worth knowing: `fd_desc_alloc()` did not zero the slot, so `nonblock` survived a close and was inherited by the next program's socket -- see `docs/decisions/kernel.md`.

- [x] ~~**TCP**~~ -- client side. `kernel/net/tcp.c`: an active open, an in-order byte stream, retransmission with exponential backoff, and an orderly close. `/bin/wget` is the proof, and `tools/net_test.py`'s tenth phase fetches from python's own `http.server` on the host -- an implementation that will not complete a handshake this OS gets wrong.

  Three things it deliberately does not do, each recorded where it happens: no listen/accept, no out-of-order reassembly (a segment past `rcv_nxt` is dropped and re-acked, so the peer resends it), and no RTT estimate. The timers ride the blocking receive, which is what a kernel with no softirq and no kernel threads has available -- and the honest gap is that a connection nobody is reading has nobody to wake it.

  It found one design bug worth recording: a closed socket's connection block outlives the socket, because the peer is still owed a FIN. With nothing to reclaim it, four dead connections held the whole pool until reboot -- the seventh KTEST got `-ENOSPC`. An orphan is now released at CLOSED or after a 2 s linger.

- [x] ~~**A passive open: listen and accept**~~ -- `SYS_LISTEN`/`SYS_ACCEPT` and `/bin/httpd`, which serves this machine's own filesystem to a browser on the host. `accept()` returns a NEW socket, a half-open connection is never offered to it, and a full backlog DROPS the SYN rather than answering with a RST (Linux's default: the client's own retransmission succeeds a moment later). It also closed a real weakness found while writing the tests -- a RST was believed without validating its sequence, which is the blind-reset attack RFC 5961 exists for.

- [x] ~~**A connection per child process**~~ DONE 2026-08-29 -- `/bin/inetd`, and the one concurrency this kernel can express without `fork`. NOT the `dup2`-before-the-spawn this entry predicted: with no fork there is no child-side window to redirect in, so the parent would have to point its OWN fd 0/1 at the connection and put them back, and anything it printed in between would go to the client. `struct spawn_msg` gained `stdin_fd` instead, and both it and `stdout_fd` accept a connected SOCKET -- posix_spawn's file_actions in miniature. A handler is now an ordinary filter (`inetd -p 7 /bin/cat` is an echo server, with no networking code in `cat`), and `/bin/httpd -1` serves one connection on fd 0/1 and exits, which is how an inetd service is written.

- [ ] Out-of-order reassembly -- a segment past `rcv_nxt` is dropped and re-acked, so the peer resends it. Correct and slow on a path that reorders; a hole list is what it would take.

- [ ] An RTT estimate, and Nagle -- the retransmit timeout is a fixed 200 ms floor with exponential backoff, and every write goes out at once.

- [x] ~~Run `dhcp` at boot~~ -- `data/etc/services.d/dhcp`, a `Restart=no` one-shot on every target. The three open questions all had answers already in the tree: init's `Restart=no` IS systemd's `Type=oneshot` and already released the readiness barrier for a service that has had its run; a boot with no server costs nothing on the common path, because SLIRP answers in under a second and a machine with no card finds no device at all; and the client no longer holds anything up either way, since nothing is ordered after it. What the boot-time run DID change is that an address now arrives a moment after the console prompt does rather than before it -- `tools/net_test.py` waits for it, and so must anything that pings on a freshly booted guest.

- [x] ~~A link-local address when no server answers~~ -- RFC 3927 in `/bin/dhcp`, over `SYS_NET_ARP_PROBE`. The candidate is MAC-derived so it is stable across reboots, three probes go out a second apart with sender 0.0.0.0, and two announcements follow the claim. The mechanism/policy split is the same one the DHCP client already made: the kernel puts one ARP request on the wire and says whether a reply has been cached, and the counts and spacing stay in ring 3.

- [ ] Defend a link-local address -- RFC 3927 asks a host to keep watching for a conflicting ARP after it has taken an address, and either defend it once or give it up. Nothing watches: `arp_input()` learns from a conflicting frame without noticing that it conflicts, and there is no channel to tell ring 3 on. The honest scope is a kernel-side conflict counter plus something for a client to read it with, which is the same missing machinery an ICMP error queue needs.

- [ ] Renew the lease -- `/bin/dhcp` asks once and exits. A lease that expires under a long-running machine leaves it using an address the server has since given away. Renewal at T1 needs a daemon, and a daemon needs a reason to exist beyond one timer.

- [ ] An ICMP error reaching the socket that caused it -- reports are SENT (`icmp_send_port_unreachable`), and one that arrives is dropped, because a socket has nowhere to put an asynchronous failure. An error queue is the same missing machinery the blocking receive needs, so the two arrive together or not at all.

- [ ] `/etc/hosts`, and a resolver cache -- every lookup goes to the wire, every time. Honest at this scale, and the first thing to revisit if anything ever resolves in a loop.

- [ ] An `arp` command -- `arp_cache_at()` exists for the KTESTs and nothing exposes it to ring 3, so a resolution failure is diagnosable only by inference from `ifconfig`'s counters.

- [ ] A routing table -- today `ipv4_route()` is "an address in a device's own subnet goes to it, anything else to that device's gateway". Multi-homing works (`tools/net_test.py` proves traffic follows the subnet), but a second route to the same subnet, or a metric, has nowhere to live.

- [x] ~~Ring-3-readable millisecond-ish clock~~ -- `SYS_MONOTONIC_NS` (nanoseconds since boot, monotonic) answers this; when this was written the only ring-3 time source was `SYS_GETTIME`, wall-clock and second-resolution. Monotonic time is an INTERFACE and wall clock is not one of its implementations -- see CLAUDE.md.

- [x] ~~Sleep/delay primitive (timeouts, retransmission)~~ -- `SYS_SLEEP` landed 2026-08-18 with init, which had nothing to block on. It was a general kernel gap rather than a networking one, and one caller has NOT been converted: the PC speaker's `beep` still busy-waits on the shared tick counter (`kernel/drivers/speaker.c`), which is a papercut and not a blocker for anything here.

## Sound

**A system-wide sound daemon.** The kernel hands out one exclusive PCM
stream and never mixes, so today exactly one program is audible at a
time -- the Audio Player, or Minesweeper, or `aplay`, and the second one
to ask gets -EBUSY and plays silently. `userland/lib/usnd_sink.h` is the
seam a daemon arrives through: a second row in that table, tried before
the device, with the device as the fallback. No app changes, which is
the move `libasound` made when PulseAudio appeared.

**Needs:** connect-by-name IPC (see Runtime + interop). Pipes here are
INHERITED, not connected, so a client the daemon did not spawn has no
way to reach it -- that rendezvous is the missing primitive, and
`PIPE_MAX` being 8 kernel-wide is a number to raise beside it.

**It does NOT need shared memory.** 48 kHz stereo s16 is 192 KB/s; at
2 KiB chunks that is ~94 messages a second per client against a
`SYS_WRITE_MAX` of 64 KiB, and two copies plus a scheduling hop are
nothing against a 341 ms ring. A shared-memory ring per client is what
PipeWire needs for a 2 ms target, and toy-os has no such target.

**Doom's sound was NOT what this file used to say it would be.** The
item read "with Doom doing its own effect mixing in userspace", which
was written before `usnd` existed and describes PrBoom+'s shape. What
landed instead maps Doom's eight channels onto `usnd` voices with a
stereo gain each -- which is what Chocolate Doom actually does, via
SDL_mixer channels and `Mix_SetPanning`, and which needed no second
mixer. The policy was never the backend's anyway: `s_sound.c` does the
attenuation and the channel stealing and hands `I_StartSound` a volume
and a separation.

**A second codec.** `usnd.c`'s table is one row today. MP3 or Vorbis is
a `.c` file and a row: the codec reports its file's native rate and
hands out s16 frames in it, and the library's conversion stage -- which
already resamples 8, 22.05 and 44.1 kHz material -- does the rest. What
a new codec must NOT do is resample; that is the split the table exists
to keep.

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

- [ ] **A real C library.** **The staged plan is `docs/libc-design.md`** -- read it before starting any of this, because the gap was being re-derived from scratch every time and got wrong in the same two directions (under-counting stdio, over-counting floating point). The target is DECIDED there: our own POSIX-shaped libc, compiled against, with "a third-party C program builds and runs" as the bar. Already done and not to be re-planned: crt0 (`userland/rt/crt0.asm`), the typed syscall layer (`userland/rt/sys.h` -- a libc sits ON this, not instead of it), `malloc`/`free` (`kernel/lib/heap_core.c` compiled twice; `realloc` absent, and `free()` cannot return memory until `mmap` exists), `string.h`/`mem*`, `snprintf` (kfmt's formatter), **`errno`** (`abi/errno.h` + `sys_errno()` + `sys_strerror()`), and **ring-3 floating point** including context-switch save/restore. What remains, in dependency order: three syscalls (`lseek`, `fstat` on an fd, `O_APPEND`); an include root so `<stdio.h>` resolves; buffered `stdio` (the whole project -- unbuffered `printf` is one syscall per call, which is worse than the `put()`-shaped code it replaces); `strtol`/`qsort`/`realloc`/`ctype`/`assert`/`setjmp`/`dirent` and the `atexit`/`exit` split (ONE line in `crt0.asm`); `%f` via a conversion hook in kfmt rather than a second formatter; `time_t` and the calendar math; and an environment, which `SYS_SPAWN` has nowhere to put today.

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

- [ ] **Growable client buffers.** `win_server.c` allocates a window's pixels with `pmm_alloc_contiguous()` and maps the whole thing up front, so a window's size is capped twice over -- by `WIN_CLIENT_MAX_W/H` and by `WIN_BUFFER_STRIDE`, the per-window slot in the client's address space. `WIN_BUFFER_STRIDE` is 64 MiB, enough for a 4K window; `WIN_CLIENT_MAX_W/H` was raised to 1920x1080 alongside the `bochs` modesetting driver, which is enough for a full-screen client at every mode below 4K and makes the WM's own screen-bounds clamp the effective limit instead. **It is the contiguity that stops it going to the 4K the display layer now allows**: 3840x2160x4 is 8100 contiguous frames, asked for a second time before the first is freed on every resize. What is still owed is dropping the CONTIGUITY requirement: 2025 contiguous frames is a lot to ask a fragmented allocator for, and the failure is a silent refusal (a normal protocol outcome, indistinguishable from a client declining). Doing it needs a way to map scattered frames into a contiguous KERNEL virtual range as well as the client's, which this kernel has no helper for today -- everything kernel-side is identity-mapped. Related to Demand paging & shared memory's demand paging, and the natural time to do it is alongside that rather than on its own.

- [ ] Multiple windows per process: the protocol already carries window ids and `win_server.c` already tracks WIN_CLIENT_MAX per client, but `userland/tests/winclient.c` only ever opens one, so the path is untested with more.

- [ ] Move the transport from one-message-per-syscall to a shared-memory ring the client maps once. The message formats are already designed for it (no pointers, fixed layout) -- this is the step that makes the syscall count stop scaling with event rate.

- [x] ~~Force-close an unresponsive client~~ -- done. Not a timeout but a PING: `WIN_EV_PING`/`WIN_REQ_PONG` (xdg_shell's shape), answered inside Toykit's loop so no app contains ping code and an app stuck in its own callback correctly fails to answer. That distinction is the substance -- a client that REFUSES to close and one that is WEDGED are the same observation to a timer. Force Quit kills the PROCESS (`scheduler_kill()`), since dropping the window alone leaves a process drawing into an unmapped buffer. Covered by `tools/forcequit_test.py` (15 checks), which tests `winclient` (declines, keeps answering) against `hangclient` (stops pumping).

- [x] ~~Client-side window resize~~ -- done, as a configure/ack handshake rather than a size the server imposes: `WIN_EV_RESIZE` proposes, the client reallocates and acks. Landed in uapp stage 3 and touched ZERO lines in the clients that had not opted in, which was the acceptance test. Covered by `tools/uapp_test.py`.

- [x] ~~**Empty ring 0 of applications first**~~ -- done 2026-08-16 (design doc's stage 0). Notepad, Calculator and Terminal DELETED from the kernel now that the ring-3 versions ship and launch from the Start menu; About and UI Demo ported to `userland/gui/`; the checkbox, dropdown, listbox and text view deleted from `apps/ui/`. The design doc's claim that `apps/ui/` ends with no callers was WRONG and is corrected there: seven files in `apps/wm/` include it, so the rest of it retires with the WM in stage 4. **Task Manager and Control Panel deliberately did NOT move**: each needs a syscall ring 3 does not have (a process list, and `etc_config`), and stage 0 is defined as the stage that adds no kernel capability -- so they move in stage 4 with the syscalls they need, not before. Proven by `gui_regress.py` 13/13 with `uidemo_test.py`'s 28 checks now driving the RING-3 widgets.

- [ ] **Restore the About window's storage line.** The kernel-side About printed the filesystem backend and whether it persists (`fs_backend_name()`/`fs_is_persistent()`). This entry used to say the ring-3 port CANNOT, for want of a syscall; that stopped being true on 2026-08-20, when `QUERY_FSINFO` was added so `df` could stop being a builtin, and `sys_query_record(QUERY_FSINFO, 0, ...)` is all it needs. What is actually left is a LAYOUT change: the window's size callback derives from the widest line, so a new line means widening the window and re-checking that callback -- worth doing deliberately rather than as a side effect. `about.c`'s own top comment says the same. `df` reports both facts meanwhile.

- [ ] **Kernel command-line switches for the protections, not just `nokaslr`.** `multiboot_cmdline()` exists and kernel ASLR is its only user; `nowx`, `nonx`, `nosmap`/`nosmep` and a heap-debug switch would join it. The argument is not convenience, it is TESTING: proving a W^X or SMAP KTEST can go red currently means editing the kernel and rebuilding (see CLAUDE.md's positive-control note, and the session that read 132/132 green off a stale ISO), and a boot flag turns that into a launch argument the suite can run both ways. Two rules it has to follow, or it makes things worse: a disabled protection must be reported loudly (`dmesg` and `about`, as `nokaslr` already does with its note), and the affected KTESTs must SKIP with a reason rather than fail -- otherwise booting with `nowx` reddens six checks and the next session "fixes" the tests. Asked for 2026-08-16.

- [x] ~~**A Live-CD boot: run from the ISO with no disk.**~~ -- done 2026-08-16, all three stages in one pass. The ISO carries a TFS3 image as a GRUB module, a block-device layer sits between the filesystems and the disk, and a RAM device mounts the module. Two GRUB entries: the default prefers a disk and falls back to the image, `toy-os (live)` forces the image. Proven by `tools/live_boot_test.py`, which boots with NO -drive and asserts a shipped binary runs -- "it booted" proves nothing here, since the kernel degrades to an empty RAM filesystem and still reaches a shell. The ASLR blocker was real and is fixed. Costs: the ISO is ~162 MiB because TFS3's minimum volume is one 128 MiB block group. See docs/live-cd-design.md's "What actually shipped".

- [ ] **Let TFS3 blocks-per-group vary for small volumes.** `bpg` is already a superblock field; both the kernel (`T3_BPG`) and `tfs3_writer.py` range-check it to exactly 32768, so the smallest TFS3 volume is 128 MiB. That is what makes the live image -- and therefore the ISO -- an order of magnitude bigger than the data in it. Touches a tested filesystem's geometry validation, so it wants its own pass with `fs_switch_test.py` and `tfs3_v1_test.py`.

- [ ] **Raw input to the compositor.** The WM is the thing that decides focus, so it cannot receive input through the focus-routed event queue it is itself responsible for filling. Needs the raw keyboard/mouse stream exposed, running alongside today's routing until the WM actually moves.

- [ ] **A ring-3 allocator, and four smaller syscalls.** The WM's state is `kmalloc`'d and ring 3 has no `malloc` (Runtime + interop); plus `etc_config_*` for settings, a MONOTONIC tick (`sys_gettime` is RTC wall-clock, wrong for animation), and `scheduler_kill`/ `scheduler_poll` for force-quit and reaping.

- [ ] An abstract transport behind that protocol, so the server side can move to ring 3 later without rewriting every call site -- the same "one struct of function pointers" pattern `display_driver` and the VFS backend probe already use here

- [ ] A bigger process table (4 slots) -- now genuinely binding: a ring-3 terminal plus the program it spawned is already two, so two terminals running commands exhausts it.

- [ ] `tosh` improvements once the kernel supports them: pipelines (`a | b` -- the pipe primitive exists, the parsing doesn't), redirection, and Ctrl-C (see Signals & process control, whose requirements this migration is what makes achievable).

- [x] ~~A GROWABLE user stack~~ DONE 2026-08-23. Raised from 1 page to 4 after the ring-3 Notepad page-faulted opening its file dialog; the real answer was always a page-fault handler that maps another page when the faulting address is just below the stack, not a bigger constant, and that is what it is now.

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

**Items worth their own note.**

- [x] ~~**A FOCUS INDICATOR for every widget that takes keys.**~~ DONE
  2026-08-27. **The item's own list of six was wrong in both
  directions**, which is the part worth recording. It named
  `uui_button`, which has no `key` op at all and was therefore never a
  tab stop that could go invisible; and it omitted `uui_listbox` and
  `uui_fileview`, which do take keys and drew nothing -- the listbox
  having declined an indicator in a comment, on the argument that its
  selection is already visible. That argument is exactly what the item
  rejects: a selected row looks identical whether or not the list is
  the control answering the arrows, so two lists side by side say
  nothing about which one is listening. The real set was seven:
  `uui_slider`, `uui_spinbox`, `uui_table`, `uui_tree`, `uui_sidebar`,
  `uui_listbox` and `uui_fileview` (which forwards to the table it
  composes).

  What landed is one helper, `uui_focus_ring()` in `uui_primitives.c`,
  drawing a 1px ring in the theme's **accent** -- the role `utheme.h`
  has named `selection / highlight / focus / checkmark` since it was
  written and which nothing had used for focus. The four that already
  drew one were converted to it, so the three geometries and one
  hover-derived tint they used between them became one answer. The
  CALLER passes the rect, because only the widget knows its own shape:
  a list rings the focused ROW and falls back to the box when the
  selection is scrolled off, and a slider rings its thumb. Proved by
  `/tests/focusring_test`, whose load-bearing checks are the row ones
  -- a ring round the whole box and a ring on the selected row both put
  accent pixels on the surface, and only the height tells them apart.

  Two things this did NOT do. `uui_button` still takes no keys, so it
  is still not a tab stop; and no app puts a table, tree or sidebar in
  a focus ring yet, which is its own item ("System Settings' focus ring
  is the PAGE's controls").
- [x] ~~**Type-ahead in `uui_table`.**~~ DONE 2026-08-27. The column a
  letter matches is DECLARED, `uui_table_set_seek_col()` --
  GtkTreeView's `search-column`, because column 0 is the name in a file
  listing and the PID in Task Manager, and Win32's always-column-0 rule
  has no way to say so. It defaults to 0 rather than to off: a table
  searching an unhelpful column says so the first time anyone types,
  while one that ignores letters fails silently. The search itself came
  out of `uui_listbox` into `userland/ui/uui_seek.c` and both widgets
  call it, so there is one implementation rather than the two the item
  as written would have produced. The part that was not thirty lines
  moving over unchanged: a table's rows are PULLED and its app indices
  are not the order on screen, so the seek walks VIEW positions and
  converts back -- the same rule `uui_table_key()`'s arrows already
  followed. And the widget was only half of it: Task Manager declared
  neither `uapp_desc.focus` nor `on_key`, so no key had ever reached
  its table -- arrows included -- and the whole GUI suite passed because
  every check there drives by mouse. It forwards from `on_key` now, as
  the File Manager does.

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

### `gui_regress.py`'s `uterm` fails its two `edit` checks under full parallel load

`the editor's cursor sequences MOVED the caret, not printed` and `a
full-screen editor runs IN THE WINDOW and saves`, both from
`uterm_test.py`. **PRE-EXISTING**, and measured rather than assumed: 3
runs in 3 under the full suite on 2026-08-24, of which the third was
HEAD with that day's work stashed and rebuilt -- same two checks, same
`25 passed, 2 failed`. Run on its own, `gui_regress.py --only uterm`
gives 27/27.

It is the load class `docs/bugs.md` already lists twice (lost clicks,
`uidemo`): port-safe parallel tools still compete for cores, and `edit`
is a full-screen program whose checks are about escape sequences
arriving in order. No diagnosis beyond that has been attempted, and none
should be inferred from this entry -- what is recorded is the rate and
the fact that it does not depend on the surrounding change.

Repro: `python3 tools/gui_regress.py --logs DIR`, then read
`DIR/uterm.log`. Compare against `--only uterm`.

### `heap-debug`'s use-after-free check fails about 1 run in 15

`mm_test.c`'s "a write through a freed pointer is caught by
heap_check()" intermittently reports `FAIL: heap_check() == 1` -- i.e.
the scan found no violation, so the block it deliberately damaged was no
longer a damaged free block by the time the scan ran.

**PRE-EXISTING**: reproduced 1 in 15 on the previous commit, in-guest
(`ktest heap-debug` repeated in one boot, no desktop running). Still
live on 2026-08-20 and at about that rate: it failed 2 of ~8 full
`preflight.sh` runs in one session while every standalone `ktest_run.py`
in between passed, which is what the rate predicts and is also why it
reads as a flaky GATE rather than a flaky test. Three
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

- [ ] **The shell's command dispatch is a long `if/else` chain, and the fix is not the obvious one.** `apps/shell.c`'s `dispatch()` is a chain of `else if (k_strcmp(cmd, ...))` branches whose handlers spread across `shell_sys.c` and `shell_fs.c`. It is the largest dispatch chain left in the tree and the one `tools/check_dispatch.py` waives by name.

  Moving the everyday file commands to `/bin` (2026-08-20) shortened it by a fifth without touching the shape of the problem, which is the useful thing it proved: what is left is overwhelmingly KERNEL INTROSPECTION -- `meminfo`, `heap`, `kstack`, `ktest`, `dmesg`, `debug`, `ata`, `parttable`, `fsck`, `stress`, `gfxbench` -- so the table these want is a `/proc`-shaped interface (`docs/query-design.md`), not a registry of function pointers. Converting first would build the wrong table. The eviction is the part that could be done without answering that question, and it is done.

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

- [x] ~~**The ring-3 WM busy-waits instead of sleeping.**~~ DONE 2026-08-29. The wait is `SYS_WAIT_READY(ms)`: it parks until the compositor's event queue is non-empty or the deadline passes, and **consumes nothing** -- readiness, not delivery, because `wm_rawin.c` drains and dispatches the whole queue itself and a delivering wait would hide one event per wait from it. `poll()` with a timeout, which is what every Wayland compositor and the X server use. The deadline is the nearer of the earliest armed client timer and `WM_IDLE_WAIT_MS` (100 ms). Two things defeat the wait because the kernel cannot see them: a repaint already owed, and injected input from the debug console, which lives in ring-3 memory and would otherwise strand a press with its release still queued. **It was built on branch `wm-wait-ready` on 2026-08-27 and PARKED because it turned `uterm_test.py`'s two `edit` checks from intermittent into 6-failures-in-6; on 2026-08-29 the same change measured 6 passes in 6, against 6 in 6 for the control, and the full GUI suite green.** What changed in between was not established -- the COM1 backpressure fix (8be1b01) is the obvious candidate and reverting it alone did NOT reproduce the old idle-CPU baseline, so it is not evidence either way. The port also found a real defect the parking had hidden: `scheduler_wake_timers()` had since started writing `SYS_RETRY` rather than 0 for a deadline wake on any channel but `SCHED_CHAN_TIMER`, so the call timed out with `-4095`; `sys_wait_ready()` folds it.

- [ ] **Injected clicks are LOST under parallel `gui_regress` load, and the failing checks are finally named.** Reproduce by running the full suite (`python3 tools/gui_regress.py --logs DIR`) at the default `-j4`; it needs the parallel load, so a single tool cannot show it.

- [ ] **Get blocking disk I/O out of the WM's event loop.** The desktop still reads files synchronously inside `wm_run()`, so a frame can block for as long as the disk takes. That is much less painful than it was (2026-08-17: the lost-DMA-wakeup race is fixed, the `.desktop` reload only runs when that directory actually changed, each entry costs one read instead of six, and a write-back cache sits underneath) -- a reload now measures in tens of milliseconds where it measured 2.5-5.6 SECONDS. But the worst case is still unbounded in principle rather than by construction.

- [ ] **Make the GUI test tooling RESOLUTION-AGNOSTIC.** The suite assumes 1280x720 in at least two places, which is what stops the default resolution being changed (and stops the tools running against a `video=`-booted guest). Known assumptions: `qmp_test.py`'s `QMPSession` starts the cursor at a hardcoded **(640, 360)** -- the centre of 1280x720, and the value `mouse_init()` resets to -- so every open-loop `goto()` would be offset at any other size; and `gui_flow.py` carries calibrated Start-menu numbers that have needed re-measuring three times already. **Newly REACHABLE rather than newly broken**: the `bochs` driver means a `video=`-booted guest is now one `make iso KCMDLINE=` away on the default `-vga std`, where before this only `VGA=vmware`/`VGA=virtio` could change the mode at all. `tools/hires_test.py` runs clean at 1920x1080 today because it drives everything through `DebugConsole` (which injects input and reports geometry in guest coordinates) and never types a pixel of its own -- which is also the shape the rest of the suite would have to move to.

- [ ] **Retire `uui_button_group` once nothing needs it.** A standalone `uui_button` routes its own clicks now (press/motion/release on `uui_button_ops`), which is how QPushButton, GtkButton and a Win32 BUTTON all behave -- Qt's `QButtonGroup` exists for EXCLUSIVITY, not for delivering the press, so this toolkit's group is doing a job no real one does. It survives today only as a convenience for a grid of many buttons treated as one widget (Calculator's keypad, UI Demo's row). The work: move those callers to individual items and delete `uui_button_group.[ch]` plus its kernel-side twin. Not urgent -- both shapes work -- but the group is the one that should go, not the button.

- [x] ~~**`damage_sweep.py`'s `resize-shrink Terminal` step reports a real missed damage.**~~ FIXED 2026-08-20, and it was two separate things, neither of them a missed damage declaration.

  **The step was masking a compositor CRASH.** The sweep never got past it: `resize-shrink Terminal` killed the desktop, the sweep's next `by_title()` returned None, and the tool died with a `TypeError` before running its remaining seven interactions. The desktop was restarted by init within a frame, so `gui windows` afterwards showed a healthy machine with no windows and nothing pointed at a crash. Measured pre-existing against the previous commit (same failure, same `CR2=0x80c00fa000`, only the RIP moved).

  The cause: `comp_poison()`'s stated invariant -- *a live window's slot in the compositor's address space is never a HOLE* -- was written for a window being DESTROYED and silently did not cover one being made SMALLER. A shrink remaps the slot to fewer pages, but the compositor is a PROCESS and still holds the old width and height until it drains `WIN_EV_CLIENT_RESIZED` some frames later; its next blit ran off the end of the new mapping. `ugfx_blit()` faulted at exactly the pixel where the old size passed the new one -- CR2 decoded to pid 3, window 0, pixel 256000, against an old buffer of 670x450 and a new one of 560x380. `struct client_window::comp_span` is the fix: the slot's extent is a high-water mark that never shrinks while the window lives, and the tail beyond the live frames stays mapped to the poison page. A shrink now costs one frame of black at the bottom of the window instead of the desktop.

  **And 22 of the sweep's reports were not findings at all.** With the crash gone the sweep ran to completion and reported 12 violations. All of them were the verifier comparing two renders whose SOURCE had moved: a client window's content is another process's memory, written whenever that process likes -- there is no `wl_buffer.release`-style handshake in TWP, so `WIN_REQ_PRESENT` is a notification and not a promise to hold still. Terminal and Notepad blink a text caret; Calculator does not; every violation landed on the first two. `ugfx_verify_diff_masked()` excludes client CONTENT rectangles per pixel now, leaving chrome, the desktop, the taskbar, menus and the cursor -- everything the compositor draws itself, which is also where a missed damage declaration can actually originate -- fully verified. The sweep is 27 interactions, 0 violations.

  **Masking PER PIXEL rather than voiding the report is the part worth keeping.** The first attempt voided any report whose diff landed in client content, and the positive control caught it: a deliberately removed taskbar damage declaration produced a diff spanning a client window AND the taskbar strip underneath it, and voiding the whole report threw away the half that was genuinely verifiable. With masking, the same control reddens three checks, every one of them at y=700 -- the taskbar strip, exactly where it was broken.


- [ ] **A `sched` KTEST fails under KVM, and only under KVM.** `sched_test.c:133`'s `scheduler_poll(pid, &code) == SCHED_POLL_RUNNING` -- the "a scheduled process survives a legacy process running alongside" case. Reproduce: `python3 tools/vm.py --kvm start` then `vm.py exec "ktest sched"`. Confirmed PRE-EXISTING (2026-08-17) by stashing all local work and rebuilding: it fails identically on the committed tree, and passes every time under TCG. Almost certainly timing -- the whole suite runs in 0.9s under KVM against 4.7s under TCG, so the spawned process has already exited by the time the poll asks whether it is still running. The fix is probably to assert the process reached a terminal state rather than that it is RUNNING at one instant, but that has not been established.

- [ ] **On a machine with no invariant TSC, CPU percentages round to 0% for sub-tick work.** Accounting measures real elapsed time now (`kernel/clocksource.h`), but it can only be as fine as the live clocksource -- and where the TSC is unusable that is the 100Hz PIT, so anything finishing inside 10ms bills 0. Reproduce with `notsc` on the GRUB command line, or just boot under plain QEMU, which cannot offer an invariant TSC at all. Not a bug and not fixable in software: the honest fix is another clocksource with real resolution, which is what makes HPET (ACPI + real power/timer) worth more here than its rating suggests -- it works under plain TCG, where the TSC does not. Its discovery blocker went away on 2026-08-30; what remains is the driver.

- [ ] **`gfxbench`'s numbers are only meaningful under KVM or on real hardware.** Plain QEMU's TCG ignores guest memory types entirely, so a write-combined framebuffer behaves exactly like a cached one and the tool reports an implausible ~17 GB/s. This is not a bug to fix -- it is a permanent property of the emulator, recorded here because it has now cost two sessions. Use `make run KVM=1` / `python3 tools/vm.py --kvm run "gfxbench 20"` for any framebuffer performance question, and treat a TCG number as evidence of nothing. `gfxbench` prints the live write-combining mechanism and whether the console is buffered beside its timings for exactly this reason. (The 2026-08-16 write-combining fix itself is SETTLED: confirmed on the maintainer's ASUS Zenbook UX305FA -- the GUI and the Shapes demo both run well now. The console-scroll regression that same change introduced is fixed and documented in `docs/decisions.md`.) **The console fix is now confirmed ON METAL too** (2026-08-16, same Zenbook, live ISO): `gfxbench 20` reports **1.8 ms** per scrolled text line against the 178.5 ms measured before it, with the console self-reporting as `buffered`. Note the bare-metal figure is ~3.6x the 0.5 ms measured under `--kvm`, and that gap is EXPECTED rather than a shortfall -- KVM honours guest memory types but its framebuffer is still host RAM, while a real one is a PCIe-attached surface where even a write-combined store is a bus transaction. So a KVM timing is the right tool for "did this get better" and the wrong one for "how fast is it"; do not quote a KVM number as a hardware target, which this file previously came close to doing.

- [x] ~~**`rammeter` doesn't appear at the physical console.**~~ RESOLVED 2026-08-20 by REMOVING `rammeter`, at the maintainer's request. The entry's own analysis was the argument: it ticked from `wm_render_frame()` only, the console has no repaint loop to hang it off, and the two obvious hooks were both worse than the gap. With the desktop in ring 3 a kernel-side overlay is also a second writer to a surface the compositor owns. `kernel/lib/rammeter.c`, `api/rammeter.h`, the `rammeter` boot flag and the `gfx_overlay_*` API that existed only for it are all gone; see `docs/decisions.md` for what it measured and what a replacement should look like.

- [x] ~~**Control Panel applets can't show hover.**~~ ALREADY FIXED, by the ring-3 GUI migration rather than by anything aimed at it -- recorded 2026-08-20 when the entry was checked against the code. `struct applet` no longer exists anywhere in the tree: Control Panel became System Settings, a ring-3 Toykit app whose controls are ordinary widgets, and `uui_radio_list` tracks `hovered` through its own `motion` op. The entry survived because nothing re-reads a bug list against a migration that deleted its subject.

- [x] ~~**`ui_checkbox` and `ui_radio_list` aren't in the focus ring.**~~ FIXED 2026-08-20, and the entry's names were stale: they are `uui_checkbox` and `uui_radio_list` in `userland/ui/` since the ring-3 migration. The diagnosis held exactly as written -- both were act-on-contact with no keyboard behaviour, so a tab stop there would have been a stop that does nothing, which is also why a keyboard-only user could not toggle a checkbox at all.

  Both have `key`, `set_focused` and `accepts_focus` now. **Space toggles a checkbox** and nothing else does, which is what Win32, GTK and Qt all do (Enter belongs to the default BUTTON, not to the focused control). **The arrow keys move a radio selection and that IS the commit**, because arrowing is choosing on a radio group in all three -- there is no separate commit step, and the ends do not wrap (Win32 wraps, GTK does not; not wrapping cannot jump the selection across the whole list on a key repeat). Neither is an exception to `docs/gui-guidelines.md`'s press-then-commit rule: that rule exists so a press can be cancelled by dragging away, and a key has no drag.

  **The positive control found a second, older defect.** Disabling `uui_radio_list`'s `accepts_focus` should have made Tab skip it; Tab reached it anyway, because `uui_focus_next()`/`_prev()` walked to the next index unconditionally and NEVER CALLED `accepts_focus` at all. Five widgets declared that slot and nothing read it, so a listbox with no rows or a slider with no options was still a tab stop that did nothing -- the mirror of this project's usual ops-table trap, a slot present and ignored rather than absent and needed. The ring skips refusing widgets now, bounded by the item count so a ring where everything refuses clears focus instead of spinning.


- [ ] **`damage_hunt.py -j 4` loses VM SLOT 0 every run.** Reproduce: `python3 tools/damage_hunt.py --seeds 1 2 3 4 5 6 7 8 --random 20 -j 4` -- seeds 1 and 5 report ERROR every time (seed 1 with a `BrokenPipeError` partway through, seed 5 with a QMP `TimeoutError` at startup), while seeds 2,3,4,6,7,8 pass. Those two are exactly the seeds that land on slot 0 (`slot = index % j`), and seed 1 alone at `-j 1` passes with all 51 interactions, so it is neither seed-specific nor a kernel crash. Reported as `error` rather than counted clean since this session, so it is visible rather than silently reducing coverage. Likely slot 0's `.vm.pid`/`.vm.serial`/port 4445 being reused before the previous guest has fully gone; the other slots use per-instance names. Not diagnosed further.

- [ ] **`gui_regress.py`'s `uidemo` fails intermittently in the full parallel suite** with `RuntimeError: UI Demo reported no layout -- is this an older kernel?`, failing before its 15s spawn wait can matter. Measured 2026-08-16: roughly 3 failures in 6 full-suite runs, while `-k uidemo` alone and a 4-tool subset passed EVERY time (42/42 checks), on an unchanged kernel.

- [x] ~~Resizing a window by its grip sometimes doesn't take on the first drag~~ STOPPED REPRODUCING 2026-08-20, and the cause was never established -- recorded as a measurement rather than claimed as a fix. The symptom was a `resize-grow` followed by a `resize-shrink` starting from the *same* coordinates in `damage_sweep.py -v`, meaning the grow had moved nothing. It does not reproduce now: the sweep's shrink starts where its grow ended, and a focused loop of 8 alternating grip drags moved the window every time. The compositor crash fixed in the same change is a plausible cause -- a dead compositor cannot adopt a resize, and it died on exactly this step -- but that connection is NOT established, only consistent.

- [ ] **Kernel-side `fsformat tfs3` writes ~73 MB of zeroed inode tables (~3 s, and the host image loses that sparseness).** The host tool avoids it (skips fresh-image zeros, hole-punches on reformat); the kernel writes real zeros because it can't punch holes. Candidate fix: `ata_trim()` the table region instead, IF the drive guarantees deterministic-read-zero after TRIM (QEMU with discard=unmap does; IDENTIFY word 69 bit 5 is the honest gate). Until then it's a papercut, not a bug.

- [ ] The vmsvga HARDWARE cursor is off by default because it fights the relative PS/2 mouse (QEMU warps the host pointer). The display driver itself works. The configuration where a hardware cursor genuinely works is virtio-gpu + virtio-input below.

- [x] **Time sources -- DONE (2026-08-17).** `kernel/clocksource.h` registers PIT (rating 110) and TSC (300), and CPU accounting bills measured nanoseconds against whichever is live. See `docs/decisions.md`. The two pieces NOT done are both scheduled under ACPI + real power/timer, which is where they belong -- **HPET as a third clocksource** (it needed ACPI's HPET table to discover the base address; that table is found as of 2026-08-30, so only the driver is left) and the **`clock_event_device` half**, since timer EVENTS are still a fixed 100Hz PIT with no tickless idle. See that milestone's Details for why HPET matters more than its middle rating suggests. The original survey text follows.

- [ ] **~~Time sources -- the strongest candidate~~ (superseded above).** The tree names concrete clocks directly: `pit_ticks()` (monotonic 100Hz, the scheduler's billing unit and `SYS_TICKS`) and the TSC (calibrated in `cpuinfo`, used by `gfxbench` and the relocation path). Three call conventions, no abstraction. ACPI + real power/timer (ACPI + real power/timer) brings HPET and TSC-deadline, which is the real trigger; a Linux-style `clocksource` (monotonic, resolution, "is it reliable across sleep") is the natural shape.

- [ ] **Stack block devices rather than hooking the filesystem, for M18 encryption at rest.** `block_device` is already shaped so a device can wrap another, device-mapper style, and encryption is size-preserving so it composes cleanly -- this is dm-crypt, and writing it as a block layer instead of as TFS3 hooks is the decision that is cheap now and expensive to undo later. Same for M36 swap. Note TFS2 deliberately calls `ata_*` directly, so a stacking layer covers TFS3 only; that is fine, TFS2 is legacy.

- [ ] **M16 block checksums are NOT simply a block layer, and that is the decision to make.** A block-level checksum layer has to put the checksums somewhere -- either shrinking the device's apparent size or carving a separate metadata area -- and that is a filesystem-shaped choice, not a transparent wrapper. It is why ZFS checksums inside the filesystem (it wants them beside the block pointers, which also gets it self-healing) while dm-integrity does it at block level and pays for a metadata region. Decide WHERE THE CHECKSUM METADATA LIVES before writing either half; the layering follows from that answer rather than the reverse.

- [ ] **`block.h` has ONE ACTIVE DEVICE, mirroring the VFS's one active backend** -- the same "one active X" call made twice, in both cases when only one existed. The tension with `partition.c` is RESOLVED and not the way this predicted: a partition is now a `block_device` that REPLACES its parent (`block_part.c`), so partitions are mountable one at a time with the singular active device intact. **The VFS half GAVE on 2026-08-25** and the block half only partly: `blk_part_create()` makes a device without making it active and a mount holds its own (read through `blkdev_*`), so two filesystems ARE readable at once -- but `blk_active()` still exists, and it is what `parttable`, `mkpart` and `fsformat` mean by "the disk". That last part is the ambiguity left to resolve.

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
to the I/O APIC, and keeping the PIC path working for a machine that
has no APIC. **The ACPI half of that is already paid** (2026-08-30):
the MADT is parsed, the Local APIC address is known including the
type-5 override, and the I/O APIC count is reported. What the MADT walk
does NOT yet read is the type-2 interrupt source overrides, which is
what says an ISA IRQ number is not the GSI -- a loop body in
`acpi_madt_init()`, and the thing that makes this work on real hardware
rather than only on QEMU's default machine.

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

### Directory lookup is O(n)

Measured 2026-08-20 with `/bin/mkfiles`, on TFS3, 5,000 files in one
directory:

- **Creating is flat.** 2,500 ms per 250 files for all twenty batches,
  first to last -- 10 ms per file, with no visible dependence on how
  many entries the directory already had. The per-file cost is the
  journal and the write-back, not the directory.
- **Looking up is not.** The verify pass, which opens each file by
  name in order, went from 80 ms per 250 at index 3,000 to 150 ms per
  250 at index 5,000 -- time per lookup growing with position, which
  is a linear scan of the directory.

It is invisible at the ~300 entries anything else in this repo creates
and obvious at 5,000, which is the argument for the tool existing: this
was not reachable by typing `touch`.

Not yet a bug entry because nothing is misbehaving -- it is a design
limitation with a measurement attached. What it costs is any workload
that opens many files in a large directory. The usual answers are a
hashed directory (ext4's htree, NTFS's B+ tree) or keeping directories
small; TFS3 has neither today.

`ls` on such a directory also stops at `SYS_LISTDIR_MAX` (256) and says
so, which is the listing cap working as designed rather than a second
finding.



## Four overlays still opt out of damage tracking

The context menu, the calendar popup, the file picker and the confirm
dialog each force a full-screen repaint every frame while they are open.
The Start menu was converted and the three steps are the same: track the
hovered row rather than deriving it from `(mx, my)` inside the draw,
damage only the overlay's own rect when that row changes, and stop
setting `redraw_pending` unconditionally. See `start_menu.c`'s own
comment for the worked example and the measurement that motivated it.

## `Terminal=true` on a `.desktop` entry

Every `.desktop` entry today is a GUI program: `Exec=` names a binary
that opens its own window. A genuinely terminal-shaped program -- `edit`,
`tosh`, `less` -- has no way to appear in the Start menu, because
launching it would spawn a process with no window and stdout going to a
console nobody can see.

freedesktop's answer is a `Terminal=` key: `false` (the default) means
"GUI program, launch it directly", `true` means "this is a terminal
program -- run it inside a terminal emulator". toy-os has the emulator
already (`/bin/wm/apps/terminal` runs `/bin/tosh` on a pty), so the work
is the key, the parse, and having the desktop spawn the Terminal with a
command to run instead of the shell.

**This is NOT how a GUI app's diagnostics should be shown.** The question
came up as "should DOOM open a terminal to show its startup text?", and
the answer is no -- no desktop does that, and it would put a window in
front of the user on every launch to show `Z_Init:` lines nobody asked
for. DOOM's output goes to the console like any other program's and is
readable after `Exit to shell`. `Terminal=` is for programs whose
INTERFACE is a terminal, which is a different thing.
