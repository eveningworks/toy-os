# What past sessions learned: diagnosing a failure

For when something is broken and you do not yet know why -- especially
a report you cannot reproduce. Entries are verbatim from the running log
past sessions kept, dated where they were written.

**The three that have cost the most, stated once:**

- A mechanism that EXPLAINS the symptoms is not the one that CAUSED
  them. Do the cheap disproving check before publishing a root cause.
- Prove a failure is pre-existing before owning it -- one build cycle
  against `HEAD` answers it.
- Ask what the user's environment does differently before doubting the
  report. `make run KVM=1` was the whole answer to a three-cause freeze,
  and it was mentioned in passing three exchanges in.

`docs/roadmap.md` holds the live known issues; this file holds the
method.

**2026-08-15 (same day, third half): that undiagnosed violation was
real, and finding it was mostly about not trusting green.**

- **A rare bug survives by looking absent.** The recorded repro came
  back CLEAN six times -- four idle, and one each at `-j 1` and `-j 2`
  with fourteen of sixteen cores busy-looping -- then reproduced with
  the recorded fingerprint byte for byte, and four more times after
  that. If a report names an exact fingerprint, a handful of clean runs
  is not evidence of anything; get a rate, not a verdict. And prove the
  harness awake FIRST (break one `wm_damage_rect()`), or "clean" and
  "measuring nothing" are the same output.
- **Two harness bugs, and both made broken runs look fine.**
  `damage_hunt.py` matched violations by searching output for the
  substring `DAMAGE BUG` -- which `gui_debug.py`'s own SOURCE contains,
  so a Python traceback was reported as a damage violation, quoting a
  line of Python as evidence. And a sweep that died in its first ten
  seconds (four guests booting at once, QMP connect timing out) was
  scored **PASS**, because the tool discarded `vm.py start`'s exit
  code. It reports `pass`/`fail`/`error` now. Generalise: a harness
  needs a third state for "did not measure", or its failures arrive
  disguised as successes.
- **Sparse files: `shutil.copyfile` fills the holes in.** `disk.img` is
  ~4 MB of data in a 9 GB sparse file, so each parallel slot was
  materialising 9 GB into a tmpfs -- `-j 4` died of ENOSPC, and `-j 2`
  ran every sweep behind ~18 GB of host memory pressure, which is most
  of what made "parallel" differ from "serial" at all. `cp
  --reflink=auto --sparse=always`.
- **Make the report carry SHAPE, not just a count.** "63 px at
  (349,264)" was unattributable for a week; adding the diff's bounding
  box turned it into `(349,264 9x15)` and `(115,20 12x19)` -- cursor
  sprite-shaped (13x19), sitting under the cursor -- and the bug was
  obvious within minutes. Same for naming the owning window and the
  cursor's last-drawn position. When a diagnostic keeps failing to
  diagnose, add a dimension to it rather than collecting more samples.
- **The mechanism was already written down, filed as harmless.** The
  cheap cursor path (`wm_render_cursor_move()`) drew the sprite without
  recording `prev_cursor_*`, so a later full frame erased the cursor
  from the wrong place. `docs/roadmap.md` had that as a papercut naming
  the exact function and missing bookkeeping -- two entries away from an
  unexplained report of its own symptom. **Before opening a hypothesis
  hunt, read the known-issues list for a mechanism that would produce
  the symptom.**
- **A test that cannot go red must not be committed.** Two versions of a
  targeted `cursor_damage_test.py` reported nothing against a kernel
  that was actively failing (a `gui click` queues four events consumed
  one per iteration, and its leading move is itself a cheap frame that
  cleans up), so it was DELETED rather than committed looking green.
  The knowledge went into a source comment instead.
- **The kernel has entropy now** -- `kernel/include/api/krandom.h`
  (`krandom_u64`/`krandom_bytes`/`krandom_quality`), RDSEED then RDRAND
  then TSC jitter, plus `SYS_GETRANDOM` and a `random` shell command.
  Three things worth carrying forward from building it: **CPU-dependent
  code needs two QEMU models** (default `qemu64` reports neither
  instruction, so `--cpu max` is the only way the hardware path runs at
  all); **a fallback's key claim has to be measured** (three boots gave
  three different values, so the jitter is not deterministic under TCG
  -- it could easily have been, and would then have been worthless);
  and **changing `__stack_chk_guard` while an instrumented frame is
  live panics that frame on return**, so the randomize call lives
  directly in `kernel_main()` and its function is
  `no_stack_protector`.
- **Never leave a busy loop running on the user's machine, and do not
  trust `jobs -p` to clean one up.** Generating host load with
  `for i in $(seq 1 14); do (while :; do :; done) & done` and killing
  it via `HOGS=$(jobs -p); kill $HOGS` does NOT work in a
  non-interactive `zsh -c`: `jobs -p` reports nothing, `kill` gets an
  empty list, `2>/dev/null` hides the error, and the script still
  prints "hogs stopped". Fourteen cores stayed pinned for an hour until
  the user noticed. If load is genuinely needed, capture `$!` per
  spawned job, kill by those exact PIDs, and VERIFY with `ps` before
  claiming it stopped -- the same "a fallible call whose caller ignores
  the result is a silent failure" rule this repo applies to C.
- **`gui move` holds for ONE wm_run() iteration.** An injected event
  overrides the real mouse for that iteration only, so the cursor snaps
  back to the real PS/2 position (640,360 after `mouse_init()`) on the
  next one. That is why strandings kept appearing at screen centre, and
  it is worth knowing before writing any cursor-position test.

**Three process lessons from that session, all of which generalise:**

- **When a decision's premise turns out wrong, re-put the decision --
  do not quietly proceed.** A history-rewrite scope was quoted as 60
  commits, then measured at 264 once file contents were included. The
  user had already chosen on the smaller number; the right move was to
  stop and ask again with the real one.
- **Estimate by measuring, before asking the user to choose.** Kernel
  ASLR's whole plan came from `ld --emit-relocs` plus a relocation
  count. The same half hour would have been spent guessing.
- **A green suite can be self-consistently wrong when a subsystem is
  reached through an indirection.** Test and code both read
  `p2_tables` by symbol, so both agreed while the hardware walked a
  different table. At least one check has to bypass the indirection.

**2026-08-16 (real-hardware performance): a whole bug class QEMU cannot
show you, and what to do about it.**

Reported as "drawing is really slow" on the maintainer's laptop (ASUS
Zenbook UX305FA) while being fine under QEMU. Four lessons, and the
first generalises well past this repo.

- **QEMU's framebuffer is cached host RAM, so an entire class of bug is
  structurally invisible to every test here.** Real hardware's linear
  framebuffer is UNCACHED MMIO: each store is a bus transaction the CPU
  stalls on. `gfx_present()` was writing three bytes per pixel, so a
  1920x1080 frame was 6.2 million individually-stalled writes -- seconds
  per repaint on metal, unmeasurable in emulation. A green
  `gui_regress` said nothing about it and could not have. **When a user
  reports something that reproduces only on hardware, ask FIRST what
  the emulator models differently, before doubting the report.**
- **Nothing in this kernel had ever set a memory type**, and the tell
  was cheap: `pat` and `mtrr` appeared only as CPUID feature-name
  strings in `cpu_features.h`. Grepping for whether a feature is
  *used* versus merely *named* took one command and pointed straight at
  the cause. The fix is PAT (per-page, so no alignment or sizing
  constraints), with slot 4 repointed at WC and slots 0-3 left alone.
- **The dangerous bit is silent: bit 12 is PAT on a 2MiB page and part
  of the PHYSICAL ADDRESS on a 4KiB one.** Writing the huge-page bit
  into a 4KiB PTE does not fault -- it repoints the mapping. Check
  `PAGE_HUGE` rather than reasoning that the range is never split.
- **A fallback nobody can execute is a guess, so give it a switch.**
  Every machine this OS runs on has PAT, which would have made the MTRR
  path permanently dead code. `nopat` on the GRUB line forces it, and
  both paths were then confirmed to boot and to report which mechanism
  they used -- the same rule `ata nodma` and `nokaslr` already follow.
  New: `docs/boot-flags.md` is the ONE list of these words (matching is
  by substring, spread across five files, no registry).
- **Say plainly what you could not verify.** The speed claim itself went
  to `docs/roadmap.md` with the exact command to settle it on the real
  machine, rather than being quietly implied by a green suite.
  **CORRECTION, from the next session:** this block's claim that it was
  "untestable in this environment" was WRONG, and the error is
  instructive -- `make run KVM=1` / `vm.py --kvm` honours guest memory
  types where TCG ignores them, so the whole bug class IS reproducible
  locally. "QEMU can't show this" was true of `make run` and got
  over-generalised to QEMU. Before recording something as unverifiable,
  check whether a different QEMU mode changes the answer.
- **LOOK at a thing you drew, even after the pixels assert correctly.**
  The new `rammeter` overlay passed its pixel probe and then, on screen,
  showed the heap at "89%" in warning yellow -- meaningless, because
  `heap_total_bytes()` is what the allocator has claimed so far and it
  claims more on demand. A meter that cries wolf every boot teaches the
  reader to ignore the one row where the colour means something.

**2026-08-16 (the other half of write-combining): an optimisation has a
DIRECTION, and six lessons that cost a green suite.**

The previous session's write-combining fix made the GUI fast on real
hardware and made the CLI console much slower. Both halves are the same
mechanism, and the session that shipped the first half did not think
about the second.

- **Write-combining helps writes and HURTS reads, and the same is true of
  most memory-type work.** WC coalesces stores into burst transfers; a
  load is still a full uncached bus round trip, with no cache fill and no
  prefetch, and it loses whatever caching the region had before. So
  marking a surface WC makes every read-modify-write path on it worse.
  The framebuffer console was the one surface that read the framebuffer
  back -- it scrolled by shifting visible pixels in place, and its cursor
  saved the cell underneath itself before painting over it. Measured with
  `gfxbench 20` under KVM: **178.5 ms -> 0.5 ms per scrolled text line**
  once the console got a back buffer, with full-screen FILL throughput
  identical (17.3 GB/s) either way -- which is the number that proves
  only the read path moved. **When you change a memory type, enumerate
  who READS that region, not just who writes it.**
- **`make run KVM=1` honours guest memory types; plain `make run` (TCG)
  ignores them entirely.** This is the single most useful fact from the
  session, because it converts "reproduces only on the maintainer's
  laptop" from untestable into a measurement:
  `python3 tools/vm.py --kvm run "gfxbench 20"`. The previous session
  recorded the bug class as structurally invisible to every test here,
  which was true of the default mode and wrong in general. Try the other
  emulator mode before declaring something unverifiable.
- **A suite that reads the BACK BUFFER cannot see a present bug.** After
  185 KTESTs and 212 GUI checks passed, one screenshot of the physical
  console showed the boot log missing entirely and a single stray glyph
  on screen: `gfx_flush()`'s last act is to clear the dirty box, which is
  harmless when drawing goes straight at the display and destroys the
  record of pending work once there is a back buffer. Every test passed
  because every one of them reads the back buffer or goes over serial.
  This is the repo's own "LOOK at a thing you drew" rule catching a real
  bug for the second session running -- treat it as mandatory for
  anything touching how pixels reach the screen, not as a nicety.
- **Do not keep a flag that duplicates a lower layer's truth.** The first
  cut of the fix set a `fb_present_pending` flag by hand at five call
  sites. `gfx.c` already tracked a dirty bounding box and `gfx_present()`
  already no-ops when it is empty, so the flag could only ever disagree
  with reality -- and the direction it would disagree in is the silent
  one (text drawn, flag unset, nothing shown, no error anywhere). Deleted
  before shipping. Same instinct as this repo's "one source of truth"
  rules elsewhere: ask what the lower layer already knows.
- **Two publish paths is one too many.** The same change briefly left
  `gfx_flush()` and `vga_present()` both meaning "make it visible", which
  is exactly the ambiguity that produced the bug above. Collapsed into
  one function that is correct in both modes, so no caller has to know
  which is live.
- **Test the MECHANISM before promising the feature.** Asked for a GRUB
  flag selecting the legacy text console, the obvious `gfxpayload=text`
  menu entry was built and did nothing. Two experiments settled it
  instead of two theories: with the multiboot2 header requesting
  1280x720x32, `gfxpayload=800x600x32` changed nothing (header wins); with
  the header set to 0/0/0 the resolution DID change to 1280x800 (proving
  the edit took effect) and `gfxpayload=text` still produced a graphics
  mode. GRUB always sets a graphics mode when the kernel carries a
  framebuffer request tag, and by the time a cmdline word could be read
  the adapter has already switched, so `0xB8000` shows nothing. Both
  experiments were reverted and the finding written to
  `docs/decisions.md` rather than a half-working entry being shipped.
  **A feature request answered with "here is why not, measured" is a real
  delivery**; one answered with a menu entry that silently does nothing
  is a defect.
- **A positive control tells you which check is load-bearing.** Breaking
  the scroll reddened exactly one of the two new KTESTs -- the shift
  check -- while the whole-height case stayed green, correctly, because
  it takes the `gfx_clear()` path instead of the memmove. Note which
  checks did NOT fire; those are the ones that would not have caught it.
- **Arm the capture for an intermittent BEFORE it fires.** The known
  `menubar_test.py` flake failed once in three full-suite runs this
  session, and that run had no `--logs`, so the failing check is still
  unnamed after three sessions. `docs/roadmap.md` now says to pass
  `--logs DIR` on every full-suite run: it costs nothing green, and a
  failure too rare to reproduce on demand can only be diagnosed by a
  capture that was already running.

**2026-08-16 (M41 stage 2, the desktop, and a data-loss bug): six things,
and the first is worth more than the rest.**

- **An intermittent is diagnosed by a RATE and a PROBE, never by
  reasoning.** `menubar_test.py`'s flake had survived three sessions of
  "re-run it and it passes". What settled it: get the rate under both
  conditions (5/5 pass ALONE, 2/4 fail in the parallel suite -- which
  alone rules out a logic bug), then instrument a REAL failure with a
  probe whose outcomes differ under each hypothesis. Two theories were
  wrong first (slow disk write, lagging recent-list); one probe killed
  both -- the file was on disk in 0.00s, and a SECOND identical hover
  opened the submenu. Item enabled, hover lost. **And record what you
  did NOT establish**: the "amplifier" that seemed to reproduce it on
  demand turned out not to (four more runs passed with the fix reverted
  AND the amplifier in place), so the fix rests on the mechanism, not on
  a measured before/after. Claiming the rate would have been wrong.
  `tools/flake_hunt.py` is the loop for this now -- run a tool N times,
  report which CHECKS failed and how often.
- **A tool that PARKS the real cursor must un-park it.**
  `warp_cursor()` is the right way to hold a hover (`gui move` lasts one
  WM iteration), but the cursor STAYS there, so a menu opened later
  finds the pointer already inside it. Applying the fix turned a
  DIFFERENT check red 5/5 -- while its partner ("a click outside
  dismisses the menu") stayed GREEN for the wrong reason: the menu had
  never opened. Park, measure, un-park.
- **`append` destroyed data for as long as TFS3 has existed**, and it
  was found by trying to write a config file from the shell rather than
  by any test. `do_write_inner()` asked whether the WRITE OFFSET was
  past EOF before skipping a partial block's read -- true on every
  append by definition -- so it zeroed the block: `write f AAAA` then
  `append f BBBB` left four NULs and BBBB. Two lessons. The regression
  test's fixture must be SMALLER than a block (a block-aligned append
  takes the fresh-block path and passes against the bug), and a
  size-only assertion passes too: the file was the right LENGTH and full
  of NULs. Also: `stat` said 8 bytes, `cat` printed nothing, every
  command returned success -- `tfs3_writer.py read` plus `cat -A` is
  what broke it open.
- **Before "fixing" something, prove it is yours.** Reverting one file
  and rebuilding took 40 seconds and turned "I broke append" into "this
  predates me". The same move settled a `damage_sweep.py` violation as
  pre-existing. Do it before writing the commit message, not after.
- **An orphan the layout checker reports may be LOAD-BEARING.**
  `check_layout.py` flagged six stale `/bin/<name>` binaries; deleting
  them (the remedy it prints) turned eight GUI tools red, because those
  tools had been spawning the stale copies long after seeding moved to
  `/bin/wm/{class}/`. The repair is to update the tools, not keep the
  corpse -- but re-run `gui_regress.py` after acting on that warning,
  and treat a test that still works after a file moved as evidence it is
  testing the old copy.
- **A per-process table sized by a literal WILL drift.**
  `win_events.c` read `4` long after `SCHED_MAX_PROCS` became 64, so any
  client in slot 4+ received no window events at all -- drawing
  perfectly, answering nothing. Its neighbour in `win_server.c` had a
  `_Static_assert` and did not drift. When you size anything per
  process, per window or per slot, assert the relationship.

**Three diagnostic habits from the same session:**

- **An arithmetic impossibility in a user's screenshot IS the
  diagnosis.** Two processes each reporting 100% CPU on a single-core
  machine cannot both be true, so the bug was in the accounting rather
  than in the scheduling -- established before reading any code, and it
  ruled out the entire "it polls too much" theory the roadmap had
  already written down. Look for a claim the system makes that cannot be
  true, before looking for the mechanism.
- **A recorded known issue can be confidently wrong.** `docs/roadmap.md`
  said Task Manager's 100% was "accurate rather than wrong, which is why
  this is a papercut and not a bug". It was an artefact. The entry was
  DELETED rather than amended -- an amended known-issue entry still
  implies something is broken.
- **Prove a failure is pre-existing before owning it.** A `sched` KTEST
  failed under KVM; stashing every local change and rebuilding showed it
  failing identically on the committed tree, which turned "I broke the
  scheduler" into a roadmap entry in about two minutes. In a
  worktree-isolated session use `git stash push -u -m <unique-tag>`,
  capture the SHA, and `git stash apply <sha>` -- never a bare
  `stash`/`pop`, since the stack is shared with every other worktree.

**A panic is diagnosable from a pasted log now** (`kernel/arch/x86_64/
idt.c`): the relocation offset, the LINK-TIME RIP and a stack scan, all
to the SERIAL log with the `addr2line` command printed ready to paste.
The RIP line used to go to the screen only, which is why panics arrived
as photographs. The backtrace is a stack scan, not an RBP walk (-O2
omits frame pointers), so it overreports -- read it as candidates. When
a panic lands in `kfree`/`try_merge_next`, that is heap CORRUPTION
written earlier, not a bug at that line: reproduce with `heap debug on`
typed at the physical shell BEFORE `gui`, which red-zones every
subsequent allocation and names the offending block at the free.

**Debugging a crash, end to end -- the loop that worked:**

1. `heap debug on` at the PHYSICAL shell, before `gui`. Red-zoned
   blocks report the violation at the offending free WITH the block
   named. Three scripted reproductions had failed to fire; this named
   the culprit in one run, and the corrupting bytes (`ame=Calc`) named
   the writer.
2. Read the panic. It now prints the function, the faulting context,
   the registers, the build id and a named stack scan -- and
   `tools/panic_resolve.py` resolves a pasted one (it checks the BUILD
   ID first, because resolving against a different build gives
   confident, wrong names).
3. Reproduce it deliberately with the Crash Test app or
   `SYS_CRASHTEST`, booting with `faultinject`.
4. **Prove it is yours before owning it**: stash, rebuild, run the same
   seed against HEAD. A damage sweep that reported three violations
   showed FIVE on HEAD, each of the three a subset -- pre-existing, in
   one build cycle.

**When a panic lands in `kfree`/`try_merge_next`/`split_block`, that is
heap CORRUPTION written earlier, not a bug at that line.** The RIP will
point nowhere near the cause. Go to step 1.

**And three process lessons from the same day:**

- **`grep -E "error|warning"` over a build log is CASE-SENSITIVE and
  hides `Error 1`.** A kernel link failure read as a clean build for two
  rounds because of that filter. Grep case-insensitively, or check the
  exit status -- a filter that can hide the failure is worse than no
  filter.

- **`grep -E "error|warning"` over a build log is case-sensitive and
  hides `Error 1`.** A kernel link failure (a struct copy GCC lowered to
  a `memcpy` this kernel has no symbol for) read as a clean build for
  two rounds because of that filter. Grep case-insensitively, or check
  the exit status.
- **Prove a failure is pre-existing rather than assuming it.** A damage
  sweep reported three violations; stashing the session's work
  (`git stash push -u -m <tag>`, apply by SHA, never a bare pop) and
  rebuilding showed the same seed producing FIVE on `HEAD`, each of the
  three a byte-for-byte subset. That took one build cycle and turned "I
  broke the compositor" into a recorded pre-existing issue.

**2026-08-17 (a user-reported desktop freeze): THE MOST IMPORTANT
LESSON IN THIS FILE FOR ANYONE DEBUGGING A REPORT YOU CANNOT
REPRODUCE.**

The report was "Control Panel sticks for a few seconds". It had THREE
independent causes, none of which any test here could see, because
**every automated test in this repo runs under TCG and the user runs
`make run KVM=1`.**

- **A lost-wakeup race in the ATA driver.** `dma_issue()` cleared its
  completion flag AFTER writing the command byte, so an interrupt
  arriving in that window was wiped; the waiter then burned the full 5s
  `DMA_WAIT_TICKS` and the retry succeeded instantly. The window is a
  function of how FAST the transfer completes, so TCG never hit it and
  KVM (microsecond completions) hit it constantly. **Generalise: anything
  armed after its own trigger has this bug**, and emulator speed decides
  whether you ever see it.
- **54 whole-file reads per desktop reload.** `etc_config_get()` re-reads
  the file per key and each `.desktop` entry was asked six questions.
  40ms under TCG, 2.5s under KVM where each port-I/O is a VM exit.
- **The filesystem was not re-entrant.** `tfs3.c` uses module-level
  scratch buffers and the kernel context is preemptible, so any ring-3
  app doing file I/O corrupted kernel-side lookups. This was WORSE than
  the freeze -- the desktop silently lost cursor shapes on ~1 boot in 3
  -- and invisible because a fallback covered it.

**The debugging loop that worked, in order:**

1. **Ask what the user's environment does differently before doubting
   the report.** `make run KVM=1` was mentioned in passing, three
   exchanges in, and it was the whole answer. Ask early.
2. **Measure from INSIDE the guest.** `userland/wm/wm_watchdog.c` times each
   `wm_run()` iteration by phase and logs anything over a threshold
   (`gui watchdog [<ms>|off]`). Its design point is worth copying: it
   measures only the work AFTER the frame's `hlt`, which makes a SILENT
   watchdog during a visible freeze a real answer ("the stall was not
   ours") rather than a missing measurement.
3. **Narrow by bisecting the WORK, not the code.** The watchdog named
   the phase; per-file timing inside that phase named the call; ATA
   debug (`debug ata on`) showed `dma read attempt 1 failed / ok on
   attempt 2`, which is a timeout, not slowness.
4. **A quantised duration is a TIMEOUT, not throughput.** Stalls
   clustered at ~2.5s and ~5.5s against a 5.0s `DMA_WAIT_TICKS`. Read
   the constant before theorising about disk speed.

**Three ways I fooled MYSELF, all worth recognising:**

- **My harness produced clean 6.2-second "stalls" that did not exist.**
  Two threads shared one `DebugConsole` socket, so replies interleaved
  and every read hit its 6.0s timeout. `gui_debug`'s console is ONE
  request/response channel -- never poll it from a second thread. Single
  -threaded, the stalls vanished. Suspect the harness when a number
  lands suspiciously close to one of its own timeouts.
- **My filter hid the evidence I had asked for.** Two separate times a
  script printed only lines matching `SLOW FRAME`, so the diagnostic
  lines I had just added were drained and discarded, and I concluded the
  code was not running. `DebugConsole.logs()` CLEARS what it returns --
  print everything you drained, or you are debugging your grep.
- **A positive control that reddens nothing means the check is not
  load-bearing -- and mine did, twice.** A flush-failure KTEST wrapped
  its assertions in `if (!ok)`, so making a failed write-back report
  success changed no result. It checks its precondition explicitly now
  (`ata_cache_dirty() != 0`, else SKIP) and asserts unconditionally.
  **Run the control on the test you just wrote, not only on old ones.**

**And one thing that generalises past this repo: WHEN THE PREMISE OF A
DECISION TURNS OUT WRONG, RE-PUT IT.** Three times here:

- The brief said "cache in the block layer, under both filesystems".
  Those cannot both be true -- TFS2 and `partition.c` bypass the block
  layer -- and a bypass past a WRITE-BACK cache is a silent correctness
  hole in both directions. Asked, got "the ATA driver", built there.
- "Lower `DMA_WAIT_TICKS`" was the obvious fix and would have re-broken
  a documented one: the comment records it being WIDENED from 3s to 5s
  after a real host-side stall. Read the comment above a constant before
  changing it. The resolution was to make the per-ATTEMPT budget
  escalate (0.3s / 1.0s / 5.0s) while leaving the total intact.
- The premise "Linux can switch schedulers" is a common misreading:
  Linux has scheduling CLASSES, compiled in; pluggable schedulers were
  rejected for years and `sched_ext` (6.12) is an escape hatch. Say so
  rather than building the thing that was asked for.

## 2026-08-19: three deadlocks, and the tool that lied about all of them

Building pipes produced three hangs in a row. What settled each is worth
more than the bugs.

**`strace` runs the binary on the LEGACY LOADER, which cannot park.**
The first trace showed a write to a full pipe returning 0 instead of
blocking, which reads exactly like the blocking-writer change not
working. It was correct: `strace <binary>` goes through the shell's
`run`, and that path has no scheduler slot, so `scheduler_block_current()`
refuses and the handler falls back to "report 0 rather than spin". The
same class as `SYS_SLEEP` returning -1 there and `pipe_test` exiting 3.
**Before believing a trace, ask which loader started the process** --
`spawn` for anything that blocks, `run` only for programs that just
compute and exit.

**A blocked pair is not evidence of the mechanism you just changed.**
Two processes both in state 4 looked like the lost wakeup I had just
theorised, so I fixed that -- and they still hung. The actual cause was
somewhere else entirely (the child inherited the pipe's write end, so
EOF was impossible). The preemption fix was real and needed, which is
what made it convincing. **Two plausible bugs can both be present; fixing
one and re-testing is the only way to tell them apart, and "it still
hangs" is information, not a refutation of the fix you just made.**

**`kstack slots` shows a state, not a REASON.** Both processes read as
blocked and that was as far as the state got me. What actually settled
it was `strace` showing `write(4, ..., 256) = 0` -- one line naming the
count, from which "the pipe thinks it has no readers" follows
immediately. Reach for the tool that prints VALUES over the one that
prints states.

**A test can create the failure it reports.** `fd_test`'s "what was
printed landed in the file" check failed because `check()` itself writes
to fd 1 -- so its own `ok` line went into the file it was asserting
about. Anything that redirects a stream must not print between the
redirect and the restore, including the test harness. Capture results
into variables and assert AFTER restoring.

## CI hanging is infrastructure until proven otherwise

Three runs sat "in progress" for one, two and three hours. The hung step
was `Install build + QEMU dependencies` -- an `apt-get` against a mirror,
before a single line of this project ran. GitHub's default job timeout is
SIX HOURS, so nothing failed; the commit list simply showed no verdict
all afternoon, which is worse than a red X because a pending run looks
like a test still thinking rather than infrastructure that has died.

Two things to carry: **check WHICH STEP is hung before suspecting the
code** (`gh run view --job=<id>` lists them with the running one marked),
and **a job with no `timeout-minutes` cannot tell you it is stuck**. The
workflow has 20 minutes on the job and 8 on the install step now, against
a normal run of 1-7 minutes.

The same look also confirmed something useful about the ATA
fault-injection flake: it turned CI red on a DOCS-ONLY commit, which is
independent evidence it is environmental rather than caused by any code
change -- the sort of confirmation that is hard to get from a local
bisect.

## A SUBSTRING ANCHOR MATCHES A LONGER HEADING (2026-08-19)

The doc-slice hazard already recorded here bit again, in a new variant,
and the line-count check is the only thing that caught it.

Trimming a stray tail from `docs/roadmap-details.md`:

```python
start = s.index("## Backlog")     # intended: the "## Backlog" heading
s = s[:start]                      # 39 lines expected
```

It deleted **1,347 lines**. `## Backlog` occurs earlier in the file as a
substring of `### Backlog` -- a different, real section -- and `index()`
found that first. `grep -n "^## Backlog"` had reported exactly one match,
which is what made the mistake feel safe: the anchored grep and the
unanchored `index()` were asking different questions.

**Anchor on the line, not the text**: `s.index("\n## Backlog\n")`. And
the rule that saved it is the one already written down -- *check the line
count afterwards*. A docs edit that changes a file's size by an order of
magnitude more than intended is not the edit you meant. Both guards are
cheap; only one of them fired.

## An in-place permutation is the wrong amount of clever

Sorting a directory listing, the sort produced an index permutation and
the caller applied it in place with the standard cycle walk:

```c
while (idx[i] != i) { j = idx[i]; swap(e[i], e[j]); swap(idx[i], idx[j]); }
```

Tracing three elements by hand -- `[C,A,B]` with `idx=[1,2,0]` -- gives
`[B,C,A]`, not `[A,B,C]`. The failure mode is the dangerous one: a
listing that is plausibly ordered and wrong, which no glance at a screen
would catch.

The fix was to retreat, not to debug: sort the ENTRIES with an insertion
sort and one scratch element. It moves more memory -- ~80 bytes an entry
against 4 for an index -- and at a few hundred entries that is nothing
next to a bug of that shape. **Reach for the obviously-correct algorithm
when the data is small enough that the clever one buys nothing you can
measure.** The permutation was solving a problem this code did not have.

## Prove it is yours the same way you prove it is not

`gui_regress` went red on `notepad` in the middle of a change. The
established move here is to stash and rebuild `HEAD` to show a failure
PREDATES the work -- and it is just as useful run the other way. `HEAD`
passed 20/20, which converted "probably a flake, the fixture has been
rewritten a lot today" into "this is mine" in one build cycle, and
pointed straight at the two output changes responsible.

The habit worth keeping: **run the comparison before forming a theory**,
in either direction. It costs one build and removes the entire class of
wrong explanations that begin "it was probably already broken".

## 2026-08-19: verify a bulk doc edit against CONTENT, not a grep you typed

Splitting `docs/bugs.md` out of the roadmap, I made the same class of
mistake three times in one session, and it is worth naming because each
one FELT like careful checking.

- The extraction regex assumed `- [ ]` bullets where the file uses
  `###` headings, so it matched 4 of 19 entries and silently left the
  rest.
- Checking what had been lost, `grep -c "rammeter doesn't appear"`
  returned 0 -- because the file says `` `rammeter` doesn't appear ``,
  with backticks. I reported lost content to the user that was never
  lost.
- Earlier the same day, "2 of 6 bugs have no repro" was wrong for the
  same reason: the search strings were too literal.

**The fix is method, not care.** A hand-typed grep tests your memory of
the text, not the text. Verify a bulk edit by SET COMPARISON in the
script that performs it -- assert every line removed from A appears
verbatim in B, before either file is written -- and let it throw rather
than reporting a count you then have to interpret. Line counts are
equally useless here: the total went UP while an entry was missing,
because the new file added a header.

And when it does go wrong: `git checkout` the files and redo it. The
restore cost seconds; the alternative was reasoning about a
half-migrated pair of files.

## 2026-08-19 (the CI hunt): instrumentation that prints a CONSTANT is worse than none

Four wrong diagnoses in a row on one bug, and every one of them was
enabled by a tool or a message that could not distinguish the cases it
was being asked about. The bug was real -- `virtqueue_poll()` timing
out on a budget that was ~12 ms rather than the 5 s it appeared to
offer, then letting late completions permanently desync the used ring
-- and it reproduced only on CI, whose QEMU (8.2.2 against 11.1) and
host CPU made the kernel choose a different clocksource.

The sequence, because the SHAPE repeats:

1. **A verbose flag that filtered.** `ktest_run.py -v` said "print the
   whole serial transcript" and applied the failure report's line
   filter, dropping every boot message. Grepping its output for
   `block: virtio-blk active` found nothing, so I concluded virtio
   never came up -- and filed it as a bug. The same grep finds nothing
   locally on a run where virtio provably carries the filesystem.
2. **A grep string that could not match.** Twice, including once with
   backticks around the term. Reported lost content that was never lost.
3. **A timeout that named no budget.** "It timed out" is not a
   diagnosis when the two code paths differ by two orders of magnitude.
   Printing WHICH branch was taken answered it in one run.
4. **A message printing the constant instead of the measurement.**
   `after 5000 ms` read identically whether the wait really took five
   seconds or the clock jumped and expired instantly -- opposite
   diagnoses, same text. Printing MEASURED elapsed *and* the poll count
   made the contradiction visible immediately: `after 927725008 us and
   454 poll(s)` cannot both be true, so the clock was lying.

**The rule: make the instrumentation distinguish the hypotheses, not
just report that something happened.** Two numbers whose ratio is
impossible beat one number that is merely large. And when a check comes
back clean or empty, ask whether it COULD have come back otherwise
before believing it.

**The fix that finally worked was to stop trusting the clock.** A poll
count is monotonic and cannot lie; a wall clock is exactly right until
it is catastrophically wrong. Bounding by count is approximate on a
fast or slow machine and that is the better trade.

## `workflow_dispatch` -- do not push to test CI

`.github/workflows/build.yml` already carries `workflow_dispatch: {}`,
so `gh workflow run build.yml` runs the whole pipeline with NO commit.
I pushed six times iterating on a CI failure before noticing. Check for
it before treating a push as the only way to get a CI run.

Better still for a version-specific bug: the runner's image is public,
so the exact toolchain can be reproduced locally in a container
(`docker run ubuntu:24.04`, which is where QEMU 8.2.2 came from) rather
than round-tripping through CI at ~90 s per attempt.

## READ THE COMPONENT BEFORE EXPERIMENTING ON IT (2026-08-19)

A settings page rendered its first control and nothing after it. I spent
three build-and-test cycles on experiments from the OUTSIDE -- ruling out
frame ordering, ruling out hidden sibling widgets, ruling out the scroll
view by removing it -- each of which was a real discriminating experiment
and none of which found the bug.

Then the user asked "could it be at component level or lower?", I opened
`uui_layout.c` for the first time, and the answer was visible in ninety
seconds: **`uui_layout_run()` walks every child with no early exit.** It
was never the layout. The real cause was one layer away -- a scroll view
that re-positions its content when its own rect or its offset moves, and
not when the content's ITEM LIST changes.

The lesson is not "experiments are bad" -- the three of them correctly
eliminated three suspects. It is that **an experiment tells you where the
bug ISN'T, and reading tells you where it IS**, and I had reached for
the expensive one first on a component I had never opened. The rule
this repo already has ("when something fails twice, stop reasoning and
go look") needs a companion: *look at the CODE, not only at the screen.*

Three specific things that generalise:

- **A symptom names a layer; the cause is often one layer up or down.**
  "The layout stops after four children" was an accurate description of
  the screen and pointed at the wrong file entirely.
- **When a container misbehaves, suspect the ops tables of its
  children.** Three widgets had short tables that day
  (`uui_dropdown_ops`, `uui_checkbox_ops`, and the scroll view's missing
  third dependency), each failing silently and at a distance.
- **A comment that enumerates what something depends on is a checklist.**
  `place_content()`'s own comment said "the viewport's rect, or the
  offset" -- exactly two of the three things it actually depends on.
  When a list like that is one short, the missing item is the bug.

## Make it impossible, then prove it (2026-08-19)

Having fixed the scroll view by adding an explicit
`uui_scrollview_content_changed()` call, the honest next question was
whether the next app would remember to call it. It would not.

So the check moved INTO the widget: `sv_children()` -- the single
accessor the router uses before routing input and before painting --
compares the content's `items` pointer and `count` against what was last
laid out. The explicit call still exists and is still worth writing at
the point of change; it is simply no longer load-bearing.

**Then prove it, the same way any other claim gets proved here:** delete
the explicit call, rebuild, and confirm the geometry is byte-identical.
A "now it's impossible" that was never tested without the belt is just a
belt and a claim.

## A broadcast that reaches N-1 of N looks exactly like a broadcast nobody sent (2026-08-20)

`WIN_EV_FONT` tells every GUI client that the font changed. The first
version walked `windows[pid][slot]` -- the window table -- and pushed the
event to every live window. Every client got it. The screen did not
change at all, and the symptom was indistinguishable from "the event is
never delivered": no repaint, no log line, nothing.

The compositor owns NO WINDOW. It draws the screen, it is not a client
of it, so it is not in that table -- and the chrome, the taskbar, the
icons and every window frame are drawn by it. `tell_compositor()` is a
separate call for exactly this reason, and it was sitting three
functions up in the same file, used by fourteen other events.

The generalisable part: **when a broadcast appears to reach nobody, first
ask who the recipient list is DERIVED from, and which participants are
not in that data structure.** A list built from "everything with a
window" silently excludes the one process whose job is windows. The
existing code already knew this -- the fix was to use the helper the
file's other broadcasts use, not to invent anything.

The tell I missed for a while: the kernel logged the font change (so the
setting applied), and the client-side handler was correct in isolation
(so reading either half proved nothing). What settled it in one step was
looking at the WM's own log and seeing NO line from the handler at all --
i.e. asking "did the recipient run?" rather than "did the sender send?".

## I published a wrong root cause to myself, again -- and the disproving check was one measurement (2026-08-21)

A GUI tool went from 0-in-4 on HEAD to 2-in-4 with a change (measured by
stashing, which is the repo's rule and it worked). I theorised the cause
was a client startup cost the change had added -- every client now made
two font-mapping round trips instead of one -- made the second one lazy,
and re-measured: **3 in 6. No better.**

The theory was coherent, the mechanism was real, and it was not this
bug's. Exactly the trap CLAUDE.md names, and I walked into it because
the theory arrived before the measurement did.

The lazy mapping was kept on its own merits. **A fix that does not fix
the bug is not thereby a bad change -- but it must be reported as what
it is**, or the next person reads the commit and believes the flake was
explained.

## Evidence that evaporates when you check the control (2026-08-21)

Chasing the same flake, I grepped the failing runs' logs for the
client's expected reply line, found zero, and concluded "the line never
arrives". Then I grepped a PASSING run: also zero. The tool's log simply
does not contain those lines.

Ten seconds of checking destroyed a conclusion I had already started
building on. **Before believing an absence, confirm the thing would be
PRESENT in the passing case.** An absence is only evidence against a
background where the presence is expected.

## Instrumenting the wrong branch (2026-08-21)

The helper I instrumented had two paths -- one for "a desktop is up,
send a keystroke", one for "no desktop, the client ran autonomously and
steps are found by markers". I patched the first and re-ran; no timing
output appeared at all, which I nearly read as "the reply never came"
rather than "this code did not run".

The tool takes the desktop DOWN for this test, so it was always the
second path. Instrumenting that one found the cause in a single run.

**When instrumentation produces no output, the first hypothesis is that
the instrumented code did not execute** -- not that the thing being
measured did not happen.

## The environment was the bug, and it cost most of an investigation (2026-08-21)

Category headings would not render bold. I measured ink, equalised
colours, forced every row bold as a control, and got byte-identical
numbers -- which looked like the drawing code being ignored entirely.
Verified the pipeline was live with a red-heading control (225 red
pixels), so the build WAS reaching the VM.

The machine was on the BAKED font, which has one weight, so bold
correctly fell back to regular. An earlier face-switching loop in the
same long-lived VM had left `font_face=builtin` in `/etc`.

Two lessons. **A long-lived test VM accumulates state that is
indistinguishable from a bug** -- this is the dirty-fixture rule, but
the fixture here was a SETTING I had written myself twenty minutes
earlier. And **when a control changes nothing, ask what the feature
depends on that the environment might not be providing**, before
concluding the code path is dead.

(It did lead somewhere: the reason a fresh disk had no bold at all was a
real pre-existing bug -- a selected font face was never BUILT unless
/etc happened to carry a size key. See session-design.md.)

## Capture the failure before re-running it (2026-08-21)

A preflight run on a dirty disk failed three KTESTs and I re-ran without
capturing which. It passed clean, so the failures are CONSISTENT with
the documented dirty-fixture pattern -- but that is inference, not a
measurement, and `docs/bugs.md` says capture first for exactly this
reason.

Recorded in the commit as inference rather than diagnosis. **The cost of
capturing is one redirect; the cost of not capturing is that the
question can never be reopened.**

## The legacy `run` loader freezes the clock, not just the sleep (2026-08-24)

`/bin/kbd`'s live mode spun the machine dead, three times, before the
cause was found -- and the reason it took three is that the documented
half of the hazard did not explain it.

**What is written down**: `SYS_SLEEP` refuses a caller with no scheduler
slot (`-EPERM`), so `run <prog>` cannot sleep and `/bin/less` says to use
`spawn`. Fine, and a poll loop that ignores the return just spins hot.

**What is NOT written down anywhere, and is the actual killer**: in that
context `sys_monotonic_ns()` never advances either. Measured -- five
consecutive polls all read `6210000000`. That context does not reach a
timer tick, so a loop with a *deadline* has a deadline that can never
arrive. A hot spin is survivable; a hot spin that can never exit takes
the whole machine, and every subsequent `sh` command on that guest times
out with no clue as to why.

Three transferable things:

- **A timeout counted in POLLS silently assumes the sleep works.** The
  first version counted 500 × 20ms and called that ten seconds; under
  `spawn` it was ten seconds and under `run` it was microseconds. One
  number meaning two very different durations depending on how the
  program was started is a thing nobody debugs twice. Read a clock.
- **...and then check the clock is running.** Which is the joke: moving
  to a clock is correct and, on its own, made the hang WORSE (from
  "exits far too early" to "never exits"). Both bugs were live at once,
  which is why two rounds of reasoning produced two wrong answers.
- **Prose in a comment is not enough when the failure mode is a dead
  machine.** `less` documents the same hazard and lives with it, because
  a pager that spins is merely slow. `kbd` guards instead:
  `sys_sleep_ms(0) < 0` means "no scheduler slot", and it says so and
  exits. Match the strength of the guard to the cost of the failure, not
  to how unlikely you think it is.

**And the instrumentation lesson, again.** Two rounds of reasoning about
the loop produced two wrong answers; four `sys_eprint()` lines into the
KERNEL LOG (fd 2 -- which reaches `dmesg` even when fd 1 is a
framebuffer nobody can see) answered it in one boot. When a ring-3
program's stdout is invisible, fd 2 is the way out.

## The log was right and the filesystem was empty (2026-08-25)

Symptom: `rescue ls /` printed nothing, `rescue ls /bin` said "not a
directory", and `rescue df` cheerfully reported **35 MB used** on a
9 GB persistent TFS3 volume. Every boot line was correct -- the
partition scan found partition 3, TFS3 mounted, `/boot` mounted.

The discriminating observation is the pair: **usage numbers came from
the cached superblock, and path lookups came from the volume.** Anything
that makes those two disagree has repointed the volume without
disturbing the cache. That located it in one step -- mounting `/boot`
probes every backend against the ESP, and TFS3's `probe()` writes the
same `g_vol` its mount uses.

**Generalise: when one half of a subsystem reports healthy and the other
half fails, ask which half is reading CACHED state.** The healthy half
is usually the one that stopped looking.

## Two failures in a row, and the second was the harness again (2026-08-25)

`fs_switch_test` failed, then failed again after a rebuild. The second
run's output had `no response within 30.0s` and a `BlockingIOError` on
the serial socket. Running `fsformat tfs3 confirm` by hand worked
perfectly, first try.

What was actually wrong: an `ondemand_sweep.py` I had started ten
minutes earlier was still running and owned the shared VM slot. This
repo's own rule -- suspect your test before the code, but verify which
it is -- would have got there faster than two rebuild-and-rerun cycles.
**Before diagnosing a VM-driven failure, run `ps -eo args | grep
qemu-system` and account for every guest.**

**2026-08-25 (the `make iso` hang). THE THEME IS THAT I "FOUND IT" TWICE
AND WAS WRONG BOTH TIMES, AND THE ANSWER CAME FROM ONE `ls -l /proc/PID/fd`
THE USER RAN.**

The report: `make iso` stops dead after the last echoed command, no
output, no error, and `make clean-disk` is the only way out. Ten
reproduction attempts here, all clean. What eventually solved it was not
reasoning at all -- it was asking the person who could reproduce it to
dump the hung process's open files.

**THE ROOT CAUSE, WORTH KNOWING ON ITS OWN: mtools does not read stdin.
It opens `/dev/tty` and reads the controlling terminal directly.** So
under `capture_output=True` its question goes into a pipe nobody reads
while it blocks forever on a terminal nobody can see it waiting on. The
fix is `start_new_session=True` (no controlling terminal, so
`open("/dev/tty")` fails with ENXIO and the child gives up, putting its
complaint in the captured stderr) -- NOT `stdin=DEVNULL`, which mtools
sails straight past. The evidence was one line: `fd 4 -> /dev/tty`.

**TWO FALSE POSITIVES, BOTH MY OWN MEASUREMENT ERROR, BOTH ANNOUNCED
BEFORE BEING CHECKED.** Recorded because the pattern matters more than
the bug:

  1. *"The guest writes to the ESP."* I checksummed `/boot` before and
     after a boot and it changed. But `make run` RE-RUNS `make iso`
     first, so my before/after straddled a rebuild -- I was measuring
     install_grub's own writes. Isolating with raw QEMU (no `make`)
     showed the guest changes ZERO sectors. **When a "before" is taken
     around a command that itself rebuilds, it is not a before.**
  2. *"Your mtools config differs from mine."* It did exist on their
     machine and not mine, which felt decisive. It was the stock
     distribution sample with every line commented out -- inert.
     **The existence of a config file is not a difference in
     configuration.**

**WHEN YOU CANNOT REPRODUCE, FIX THE INVISIBILITY RATHER THAN GUESSING
AT THE CAUSE.** Three commits landed before the root cause was known,
and none of them claimed to be the fix: a timeout so the hang cannot be
silent, the timeout lowered from 180s to 60s because the reporter had
killed the build by hand at 18s and would never have seen it, and a
shared `_timeout_exit()` because one call site was throwing a raw
`TimeoutExpired` traceback out of subprocess internals. That third
commit's message is what produced the `mmd` name; the `/proc` dump it
asked for produced the cause.

**A TIMEOUT NOBODY WAITS OUT TEACHES NOTHING.** Pick the budget from how
long a human will actually stare at a stalled build, not from how slow
the operation could theoretically be.

**CHECK THE DIAGNOSTIC COMMAND YOU HAND SOMEONE.** I told them to run
`fsck.fat -n -v <(dd ...)`. `fsck.fat` seeks, a process substitution is
a pipe, and they got `Seek to 0:Illegal seek` -- a wasted round trip on
advice I had not run myself.

**AND THE SELF-MATCH TRAP BIT AGAIN**, exactly as CLAUDE.md warns: a
`ps aux | grep "[q]emu-system"` inside a shell whose own command line
contained that string matched itself and printed nonsense.

**2026-08-27 -- a one-line feature wedged the compositor, and the only
thing that saw it was one test's CONTROL check.** init started writing a
~120-byte status file at 0.8 s of boot. Everything passed except
`idle_desktop_test`'s "the taskbar clock DOES change (proves motion is
visible)" -- the check that exists to stop the two checks above it from
passing vacuously. Without it, a suite of ~300 GUI checks would have
called a desktop that presents NOTHING a clean run.

**A CONTROL CHECK FAILING IS NOT A HARNESS PROBLEM, and the temptation
to read it as one is strong** -- it is the check that is not about the
feature. Here it was the only true statement in the report: the two
"steady" checks passing meant the screen was frozen, which is what they
look like when the harness is right and the guest is dead.

**EVERY PIECE OF EVIDENCE POINTED AT A HEALTHY MACHINE.** `ps` showed
the desktop `ready` with its CPU advancing; `gui state --json` answered
every time with `redraw_pending` false and no damage; `uptime` proved
the timer tick was fine; the kernel log was silent. Three of those are
worthless for the question asked: `gui state` reads KERNEL-side window
records, so it answers whatever the client is doing, and a compositor
that has stopped presenting still burns CPU in its own loop. **Ask what
your instrument is actually attached to** -- the one that settled it was
injecting a mouse move and watching the CURSOR not move, which is the
only probe that goes all the way to the screen.

**"IT REPRODUCES ONLY UNDER THE HARNESS" MEANT "ONLY ON A FRESH BOOT."**
Every manual attempt passed, because a VM I had been poking at for two
minutes was long past the window. The failing runs were all ~9 s after
boot. When a tool fails and a hand-run of the same steps does not,
compare the machine's AGE before doubting the tool.

**BISECT YOUR OWN CHANGE BEFORE READING ANY MORE CODE.** Four candidate
mechanisms, three of them plausible, and one build with the write
disabled answered it in 40 s. I had read four source files first.

**A PARTIAL FIX IS NOT A FIX, AND SHIPPING ONE IS WORSE THAN NOTHING.**
Guarding the WM's desktop-entry reload until the first composited frame
took the failure from 1 distinct frame in 8 to 2 in 8. That is a real
finding about the mechanism and it was written into `docs/bugs.md`; the
code was REVERTED, because a half-fix in a file the change does not
otherwise touch reads to the next session as a solved problem.

**AND `predates.py` ANSWERS THE ONLY QUESTION THAT MATTERS FIRST.**
Three runs, HEAD passing and the tree failing every time, is what turned
"is this mine?" from an argument into a fact -- and one run each would
not have, because the failure had already been seen to pass once on an
aged VM.


**2026-08-27: TURNING AN INTERMITTENT INTO A DETERMINISTIC FAILURE IS
STILL A REGRESSION, AND ONLY A RATE ON BOTH SIDES SHOWS IT.**

A compositor change made `uterm_test`'s two `edit` checks fail. Those
checks were ALREADY in `docs/bugs.md` as intermittent, cause never
established -- so "it predates me" was true and would have been the
wrong conclusion. Measured on equally clean disks: **HEAD 1 run in 3,
the change 6 in 6.** At a true rate of 1/3, six-for-six is p≈0.0014.

The verdict that fits: the defect predates the change, and the change
makes it reliable. That is still not shippable, and it is a different
sentence from either "I broke it" or "it predates me".

**Two things this needed that a single paired run does not give.**
`predates.py` runs each side ONCE, which cannot distinguish 1/3 from
6/6 -- it reported "HEAD fails and the working tree does not" on one
sampling and the opposite on another. For anything already known to be
flaky, run N on each side. And **re-run on a fresh image first**: the
first paired comparison ran against a `disk.img` the KTEST suite had
left a corrupted file on, and `make iso` then failed outright with
`sparse files are outside this tool's write scope`. `make clean-disk`
between batches is in CLAUDE.md for this reason and it still caught me.

**WHEN TWO HYPOTHESES FAIL, STOP AND INSTRUMENT.** Both were plausible
and both were wrong, and each cost a build-and-test cycle:

- *The wait is too long.* Set `WM_IDLE_WAIT_MS` to 10 -- exactly the old
  100 Hz cadence. Still failed 1 in 1. So it is not the duration, it is
  the blocking.
- *The compositor out-runs its clients.* An event-woken loop runs a
  frame per present, where a tick-paced one could not exceed 100/s, so a
  streaming client should be able to squeeze the pty chain. Added a
  `WM_MIN_FRAME_MS` floor. It got WORSE -- 9 failures instead of 2.

Both were discriminating experiments, which is why each was worth one
cycle. After the second, the honest move was to park the work on a
branch with the evidence in the commit message rather than guess a
third time. The repo's own rule is "when something fails twice, stop
reasoning and go look" -- looking here means a frame counter, not
another theory.

**A FAILURE CAN BE THE HARNESS NOT HAVING RUN AT ALL.** A `uterm` run
that printed no `FAIL` lines looked like a pass; it was `vm.py start`
racing the previous `stop`, so `QMPSession` got `Connection refused` and
the tool died before its first check. Empty output is not a green run.
Wait on the PORT accepting rather than on a fixed sleep -- an `until`
loop probing `connect_ex` -- which is the same "wait on an observable,
not a duration" rule the GUI tools already follow.

**AND THE CONSOLE PREFIX MATTERS: `vm.py` sends `sh <cmd>`.** The ring-0
debug console's own commands are kernel introspection; `df`, `dmesg` and
`ls` are `/bin` PROGRAMS reached through `sh`. A hand-rolled serial
driver that forgets the prefix gets `unknown command: df` and reads as a
broken guest; one that doubles it gets `Unknown command: sh`.

**2026-08-27: TWO BUGS CAN HIDE EACH OTHER, AND THE PAIR LOOKS
HEALTHY.** `tfs3_writer.py trim` punched free blocks at a
VOLUME-relative offset through the raw fd -- every other access in that
file goes through `Tfs3Image`, which adds `base_lba * 512`, and this one
did not. On a partitioned image that is one partition-start early, onto
blocks in use. The SECOND bug walked each group's bitmap to
`BLOCKS_PER_GROUP` when the last group may be PARTIAL, so it asked to
punch ~71 MB past the volume's end -- which the offset error pulled back
inside the file. A trim-past-the-end became a corrupt-the-beginning, and
together they printed a perfectly plausible `punched N blocks`.

**Fixing one made the other visible immediately** -- the bounds guard
added with the offset fix fired on the first run and named the second.
The generalisable bit: **when a destructive operation has been "working"
and you find one fault in it, expect a second that the first was
masking**, and add the guard before assuming the fix is complete. A
punch is silent by nature; it cannot fail loudly the way a bad write
can, which is exactly why it needed a range check it did not have.

**AND THE SYMPTOM POINTED NOWHERE NEAR THE CAUSE.** The report was "the
live CD has binaries but no apps on the desktop". `/bin` was intact, the
shell worked, the filesystem mounted and reported its size correctly.
Only three directories were gone, and `live_boot_test.py`'s "a directory
the image shipped is readable" check passed throughout because it reads
`/bin/wm/apps` -- which happened to sit far enough from a free run to
survive. **A corruption that lands somewhere SPECIFIC needs a check that
looks THERE**; "some directory is readable" is a check that cannot fail
for the right reason.

The bisect that found it in two commands: list the image BEFORE the
build's last step and AFTER it. The build printed `sync: 218 written, 0
unchanged` and the tree has exactly 218 files, so the write half was
provably fine and only what came after could be at fault.

## Three bugs from one flip, and the instrument that ended each theory (2026-08-28)

Making every /bin and GUI program dynamic broke the GUI suite in a
SMEARED way -- a different subset of uterm checks red each run. Three
distinct defects were hiding under one symptom, and each fell to a
different instrument after theorizing had gone nowhere:

- **The moving failure set meant SYSTEMWIDE slowness, not a broken
  feature.** Every dynamic spawn demand-paged libc.so through the
  filesystem, each fault a preempt-disabled device poll; marginal
  checks flaked under load. The fix was the design's own frame sharing
  (the /lib image cache). When failures WANDER between unrelated
  checks, stop reading the checks and ask what got slower everywhere.
- **Two klog timestamps ended three wrong theories about the
  foreground race**: `[4.22] SIGTTIN fg=4` / `[4.24] tcsetpgrp fg=6`
  -- the child's first read beat the shell's tcsetpgrp by a whole
  timeslice, because the heavier spawn syscall now ended the shell's
  slice. Fixed at the root: `SPAWN_FOREGROUND`, the terminal handoff
  riding ON the spawn (musl's POSIX_SPAWN_TCSETPGROUP -- POSIX shells
  close this gap from the child's side between fork and exec, and a
  spawn ABI has no child side). "Instrument before theorising a third
  time" is already in this file; it was re-learned at full price.
- **`ps` typed at the console printed into a Terminal window** --
  `fd_inherit()` read its parent off CR3, and a kernel-context caller
  runs with whatever address space the scheduler last loaded. The
  parent is a named argument now. The misleading-symptom rule
  (something shared with whatever ran before), CR3 edition.

Two measurements that kept the day honest: `predates.py` proved
`ansi_cursor_test`'s 5/10 pre-existing (HEAD fails identically -- filed
in docs/bugs.md, not chased), and the ktest `fsck r.leaked=12` cluster
was the documented dirty-disk.img fixture (clean-disk cleared it).

**2026-08-29 (three audio bugs, and where each was actually found).**

- **A DEADLOCK CAN LIVE IN CODE YOU DID NOT WRITE AND DID NOT CALL.**
  Doom's window opened stuck on its pre-WAD title, with `doom: sound on`
  logged and no music line. Cause: `opl.c`'s `InitDriver` calls
  `OPL_Detect()`, which calls `OPL_Delay()`, which schedules a callback
  and BLOCKS on a condition variable -- and callbacks only fire from the
  render path. SDL's audio thread was already running by then; ours did
  not exist yet. **When porting a subsystem, ask what the platform layer
  you are replacing was doing BEFORE init was called.**
- **THE SYMPTOM NAMED THE WRONG SUBSYSTEM.** "No window" and "no cache
  line" looked like the sound module failing, and the sound module had
  logged success. The failing call was two layers down in vendored code.
  Reading the init path in call order found it; guessing did not.
- **A PRECACHE LOOP MUST TOLERATE ENTRIES THAT DO NOT EXIST.** `S_Init`
  precaches over the WHOLE of `S_sfx[]`, including a dummy with an empty
  name, and `W_GetNumForName` calls `I_Error` on a miss -- killing the
  game before its window opened. `W_CheckNumForName` is the one to use.
  The general shape: a table you are handed may have padding entries.
- **A CFLAGS CHANGE INVALIDATES NOTHING, AND THE FAILURE IS SILENT.**
  Defining `FEATURE_SOUND` relinked a stale `i_sound.o`, so the module
  list stayed empty and `nm -u` showed no reference to the module the
  flag was supposed to enable. `rm -rf build/<subtree>` was the fix.
  CLAUDE.md documents this exactly; it still cost a diagnosis round.
  **After a flags change, check the OBJECT, not just the build result.**
- **REVIEW FOUND THE FOURTH ONE, AND REVIEW IS ALLOWED TO.** Doom's
  `S_Shutdown` calls `I_ShutdownSound()` then `I_ShutdownMusic()`, so
  the first would free the mixer and join its worker while the OPL
  thread was still feeding it. Reference-counted `usnd_init`/`_shutdown`
  fixed it. Reachable through Doom's own Quit, never observed, and said
  so plainly in the commit rather than claiming a crash was seen.

**2026-08-29 (the compositor "wedge" that was not one). THE THEME IS
THAT EVERY WRONG ANSWER CAME FROM AN INSTRUMENT, NOT FROM THE SYSTEM.**
Four hypotheses were formed and withdrawn -- the timer had stopped, the
scheduler was starving the process, the frame loop had stopped, the
process was parked in a thread call -- and each was killed by a cheap
measurement that should have come first. What follows is the order to do
it in next time.

- **RESOLVE AN ADDRESS WITH `panic_resolve.py`, NEVER WITH `nm` BY
  HAND.** A hand-rolled "nearest symbol below" over `nm` output named
  `sys_thread_detach`, and the WM does not call it -- an hour went into
  reconciling that. The real answer was `sys_yield` at
  `userland/rt/sys.c:217`, the loop top, which would have said "the WM
  is healthy" immediately. The tool reads DWARF and gives file and LINE,
  it takes `--elf` for a RING-3 binary and `--delta 0` for one (userland
  is not relocated), and for the kernel `--delta` is the
  `kernel relocated +0x...` value in `dmesg`. `nm`'s nearest-below is
  wrong whenever the target sits in a function whose symbol was folded
  or reordered.
- **A NUMBER WITHOUT A CONTROL MEANS NOTHING, AND TWO OF THEM LOOKED
  DAMNING.** `toywm` showing a flat `0.20` of CPU while apparently stuck
  reads as a starved process -- until a HEALTHY boot shows exactly
  `0.20` too. Same for a ring-3 interrupt rate of ~2400/s. Before
  drawing anything from a counter, run the identical probe on a working
  boot; here that turned two "findings" into noise.
- **A PROBE THAT OUTRUNS THE KLOG RING DESTROYS ITS OWN EVIDENCE.** The
  ring holds a few hundred lines. A probe printing twice a second wraps
  it in seconds, and reading the surviving tail says "the system stopped
  doing X" when X never stopped -- that is exactly where "the timer
  stopped" and "the frame loop stopped" came from. Keep instrumentation
  under about a line a second, or write to a file-backed serial log
  (`-serial file:`) where nothing is lost.
- **GIVE EVERY PROBE IN A COMPARISON THE SAME RATE LIMITER.** Half the
  probes used `n <= 6` and half `n % 300`; the first set stopped
  printing at iteration 7 and was read as the loop dying at iteration 7.
  The difference measured was the instrument's.
- **ASK THE EMULATOR BEFORE ASKING THE GUEST** --
  `QMPSession.hmp("info registers")` / `("info pic")`. The guest's own
  reporting is written in the code under suspicion; QEMU's is not.
  `HLT=1` with `IF` set, `CPL=0`, and a PIC showing `imr` with the timer
  unmasked and `isr=00` together eliminated the whole interrupt layer in
  one command. Sample a DISTRIBUTION: reading registers stops the vCPU,
  so a mostly-idle guest reads as halted every single time.
- **A FAULT THAT RECOVERS FASTER THAN YOU CAN QUERY CANNOT BE PROBED
  FROM OUTSIDE.** A `vm.py exec` round trip is one to two seconds; the
  stall was about three. Every manual sample of ticks, the RTC and the
  screen landed after recovery and read perfectly normal. Anything
  shorter than a few seconds needs in-guest instrumentation that
  survives the window, not a query afterwards.
- **`flake_hunt.py` ALREADY IS THE REPEAT-UNTIL-IT-FAILS LOOP.** It runs
  a GUI tool N times, reports the RATE and names the failing checks, and
  keeps the logs with `--keep`. That loop was hand-rolled here at least
  four times, badly. Reach for it the moment the question is "how often,
  and which check".
- **AND THE REPRODUCTION HAS TO BE THE REAL TRIGGER.** Three attempts
  used a service that wrote a file "early", and none reproduced, because
  the write landed at 1.3 s -- after the first composited frame. The
  original trigger (init's own unconditional status write) reproduced at
  ~1 in 2 immediately. When a bug names its trigger, restore THAT, do
  not build something of the same shape.

**2026-08-31 (writing an MP3 decoder). THE THEME IS THAT FOUR SEPARATE
THINGS "EXPLAINED" THE BUG BEFORE THE REAL ONE, AND THREE OF THEM WERE
MY OWN INSTRUMENTS.** The bug was one line; finding it cost most of a
day, almost entirely spent on evidence that was not evidence.

The actual defect, for context: the Huffman decode tree marked a leaf in
BOTH CHILDREN of the node the codeword path ended at, so the walk only
saw it after descending one level further. Every symbol consumed one
extra bit. That decodes to plausible small numbers rather than to an
error, which is why nothing upstream noticed.

- **AN ORACLE THAT ADAPTS TO ITS INPUT CANNOT VALIDATE THAT INPUT.** The
  decoder's own bit accounting looked perfect -- granule after granule
  landed "1 bit over budget", which reads like exactness. It was
  meaningless: the count1 region decodes UNTIL the bit budget is spent,
  so it fills to whatever number you give it. Any budget, right or
  wrong, produces a granule that lands on it. I treated that as
  confirmation for hours. Before believing a check, ask what it would do
  if the value under test were wrong -- if the answer is "the same
  thing", it is not a check.
- **`grep -l` MATCHES COMMENTS, AND A COMMENT CAN SAY THE OPPOSITE OF
  THE CODE.** I grepped for `float` in `uimg_jpeg.c`, got a hit, and
  told the user ring-3 floating point was already in use there. The hit
  was a comment saying this build has NO floating point to fold
  constants from. The conclusion happened to be right for other reasons
  (`fpu.c`, `/tests/fpu_test`, tolibc's `math.c`), but the evidence was
  backwards. Look at what a hit actually IS before citing it.
- **A PATTERN THAT FITS PERFECTLY IS STILL WORTHLESS IF THE TWO SIDES
  ARE NOT THE SAME THING.** I compared my decoder's side-info dump for
  frame ~20 against pdmp3's for frame 2 and found an exact five-bit
  shift: their table_select[0] was my [1], their [1] was my [2], their
  [2] was the bits after mine. That is a compelling story, and it was
  coincidence -- different frames. The fix was to make both sides dump
  the SAME granules, which took one edit and immediately showed the side
  info matching field for field.
- **MY OWN SHELL PIPELINE KILLED THE PROCESS AND I READ IT AS A BUG.**
  `./probe file 2>&1 >out.raw | head -2` closes the pipe after two
  lines; the next stderr write takes SIGPIPE and the process dies. I
  measured "only 3069 frames decoded" and started hunting a VBR frame-
  iteration bug that did not exist. Suspect the harness first -- and
  `head` on a pipe is a harness.
- **A SECOND INDEPENDENT SOURCE IS WORTH MORE THAN ANY AMOUNT OF
  REASONING ABOUT ONE.** The tables came from minimp3; walking pdmp3's
  completely different format and comparing entry for entry is what
  finally proved the data right and moved suspicion to the code. It also
  caught the reverse: table 24 "disagreed" in 63 of 256 entries because
  my pdmp3 walker ignored that format's `>= 250` long-jump escape. The
  disagreement was my second reader, not the data -- which is exactly
  what a second reader is for.
- **A STRUCTURAL INVARIANT YOU CAN CHECK WITHOUT A REFERENCE IS WORTH
  BUILDING EARLY.** Every MP3 Huffman table must be a complete prefix
  code (Kraft sum exactly 1). That is checkable with no oracle at all,
  it killed the transcribe-from-memory approach within minutes of the
  first big table, and it now ships as `usnd_mp3_selftest()`. Ask what
  property the data must have on its own, before asking what it should
  equal.
- **AND THE SLICE-EDIT HAZARD APPLIES TO CODE, NOT JUST DOCS.**
  CLAUDE.md records that replacing a docs slice by index deletes its
  neighbours; I did it to `tools/iso_guard.py` and removed `BYPASS_ENV`,
  `ARTIFACT_PAIRS` and `UNSEEDED` along with the block I meant to
  replace. It failed loudly (NameError) rather than silently, which was
  luck. `git checkout` the file and redo the edit with ANCHORED
  replacements -- never `s[:i] + new + s[j:]` across code you have not
  re-read.
