# Session history

The dated per-session notes that used to sit in SKILL.md: where the
project stood after each big day, and the lesson each one paid for. Moved
verbatim on 2026-10-05 so the playbook stays short. Read the entry for the
area you are touching; where one disagrees with CLAUDE.md,
`docs/decisions.md` or the code, those win.

## A recurring wrinkle worth knowing about upfront

An automated stop-hook may warn about "unpushed commits" after nearly
every turn in this repo. It means exactly what it says -- real unpushed
work on the real checkout -- and is worth surfacing to the user rather
than explaining away.

**2026-08-17 (the day the GUI moved to ring 3): the ring-3 desktop RUNS, and the
day's lessons are mostly about DIAGNOSIS rather than about this OS.**

Where the milestone stood THAT DAY, kept because the lessons below came
out of it: `gui3` started a ring-3 desktop passing 19 of 23 GUI tools,
`gui` was still the ring-0 one, and two copies of the WM existed.
**All of that is finished -- see the 2026-08-18 section below: the
desktop is a ring-3 process by default, `apps/wm/` is deleted, and
The GUI in ring 3 is complete.**

The four lessons worth carrying anywhere:

- **READ THE ABI COMMENT OF ANY CALL YOU SWAP IN.** The single most
  expensive mistake of the migration was replacing a non-blocking
  `scheduler_poll()` with `sys_waitpid()` and writing a comment claiming
  it was "non-blocking in the same sense" -- while `SYS_WAITPID`'s own
  first line said **BLOCKS**. The desktop then died silently on its
  first client. A PORT is exactly where this happens, because the new
  call's name resembles the old one's and the old one's semantics are
  the ones in your head.
- **A mechanism that explains the symptoms is not the mechanism that
  caused them.** One wrong root cause was written up and COMMITTED that
  day: it blamed a `sti; hlt` wait against a single global resume
  pointer, reasoned that it survives two contexts and not three, and
  fitted every observation. The hazard was real and documented -- just
  not this bug's. Do the cheap disproving check before publishing a
  diagnosis, and say plainly when you withdraw one.
- **A discriminating experiment beats a plausible story.** Spawning
  `/bin/hello` (exits at once) was harmless; spawning `winclient`
  (creates a window and waits) was fatal. That pair located the bug
  class in one run, after two theories had each cost a build-and-test
  cycle. Reach for the experiment whose OUTCOME DIFFERS under the
  competing explanations.
- **Instrument before theorising a third time.** A per-frame log line
  settled "is it stuck, or just slow?" instantly, after reasoning had
  failed twice. The repo already says this; it was still learned again.

And four things a ring-0 component loses the moment it becomes a
process -- all hit in one afternoon, all silent:

- `hlt` is **privileged**: the ported idle wait was a #GP on frame one.
- **The font is not free.** Anything drawing in ring 3 must call
  `ugfx_font_init()`; without it `ugfx_char_h()` is 0 and every
  font-derived measurement collapses without an error -- chrome became
  8px and icon labels vanished while their boxes still drew. It must
  run BEFORE any geometry derived from it.
- **Nobody polls the hardware.** Raw input only ever reached a
  compositor because the ring-0 WM forwarded it; with the WM gone there
  was no producer at all, and the desktop looked frozen.
- **One over-strict guard can produce two unrelated symptoms.** A single
  `!g_ops` check refused both the font and window creation, with a bare
  `return -1` that nothing logged -- so "no text" and "no windows"
  looked like two bugs.

Finally, a UI note that generalises: **if a file needs four lines of
comment to explain two button labels apart, the labels are wrong.** Task
Manager's "End Task"/"End Process" (Windows' names) became
"Close"/"Force Quit" -- and "Force Quit" was already this desktop's word
for that action elsewhere, so one action had stopped having two names.


**2026-08-18 (The GUI in ring 3 CLOSED, kernel stacks, settings namespaces,
and four harness bugs). Read this before believing anything above about
the WM being in ring 0.**

Where the project stands now, so a session does not re-derive it:

- **The desktop is a ring-3 process and `apps/wm/` is GONE** (~10,400
  lines, with `apps/gui_apps.c` and `apps/ui/`'s widget set). `gui`
  spawns `/bin/wm/system/toywm`. There is ONE window manager again --
  the "make every fix twice" hazard is over. `apps/ui/` was down to
  `ui_scrollback` and is now GONE ENTIRELY (2026-08-22), because `edit`
  -- its last caller -- became `/bin/edit`; a new widget goes in
  `userland/ui/`, always.
- **`kill <pid>` and `spawn <path>` are shell commands.** They exist
  because the milestone's exit criterion was untestable without them:
  `gui kill` cannot end the WM (dispatched from inside its own loop, and
  `scheduler_kill()` refuses the CURRENT process) and `run` cannot start
  one (the legacy loader is not a scheduled process, so its
  `win_request()` is refused). `ps` for the desktop's pid, `kill <pid>`
  then `spawn /bin/wm/system/toywm` kills and restarts it -- it was
  `kill 1` until init took pid 1 (docs/init-design.md stage 1), which is
  the argument for looking a pid up rather than assuming one.
- **Settings are namespaced**: identity is (namespace, name), the
  namespace being the registered name of the file, so `system.font_size`.
  A bare name works when unique and is REFUSED when ambiguous.
- **Kernel stacks are 16 KiB with a guard page, a canary, and a build-
  time frame budget** (`-Wframe-larger-than`). `kstack` at the shell
  reports high-water usage per process; use it BEFORE a crash.

**THE MOST TRANSFERABLE LESSON OF THE DAY: a stack frame is not the sum
of what you can see.** `syscall_dispatch()` was 4832 bytes on every
syscall. Two rounds of extracting the obvious suspects -- a dozen
message structs, then two of the four KiB-sized bounce buffers --
changed the total by NOTHING (4832 -> 4896, slightly up), because GCC
already overlapped them. The cause was ONE 4 KiB
`SYS_GETRANDOM_MAX` buffer everything else hid behind. `-fstack-usage`
answered it in one command. Ask the compiler; do not reason about which
struct is biggest.

**And the kernel-design lesson: when a kernel teardown revokes something
a PROCESS is using, it cannot be stopped mid-use.** Force Quit killed
the ring-3 desktop because `destroy_window()` unmapped the compositor's
view of the dying client synchronously while only QUEUEING the event --
so the WM returned from its own `sys_kill()` with a dead window still in
its list and blitted it. A revoked slot is remapped to a shared
read-only zero page now, never left as a hole. Generalise: a ring-0
component becoming a process turns every "and then it will notice"
into a race.

**FOUR HARNESS BUGS, and three of them reported a healthy system as
broken.** This is the day's real theme -- more time went into tools
lying than into the OS.

- **A forbidden-substring check must be scoped to the test's own
  output.** `usertest_run.py` matched "FAILED" against everything the
  serial console said, and the kernel uses that word for its own reasons
  (`atac: FLUSH FAILED`). CI reported a test that printed "all phases
  passed" and exited 0 as failed. Scope it, and QUOTE the offending line
  -- the tail-only report showed none of the relevant output.
- **Compare SETTLED frames.** A capture landing mid-paint fails a
  comparison with nothing wrong with it, and a client having drawn into
  its buffer (or even LOGGED that it did) is not the compositor having
  painted it -- with the WM in ring 3 that is an extra process hop.
  `QMPSession.stable_pixels()` (two identical consecutive reads) took
  `calculator_client_test.py` from 2 runs in 6 to 8 in 8. Do not use it
  on a window that animates on purpose.
- **A poll whose exit condition is weaker than what follows it is a
  flake.** The same tool waited for the FIRST of N layout lines and then
  required all N. Only visible once the louder bug above was fixed.
- **Put the detail in the line a truncated log keeps.** A test that
  prints "at least one phase FAILED" and the reason thousands of
  characters earlier is a diagnosis nobody gets. The summary names the
  phase now.

**A STANDING PRACTICE, added at the user's request after today made the
case for it: before proposing a design, say what Linux and Windows do
-- and on the desktop side, Wayland and a real compositor (KWin, Mutter,
wlroots) or X11 where the history explains the shape.** Then say whether
toy-os should follow or deliberately differ, and why. It changed four
decisions today rather than decorating them: guard pages
(`CONFIG_VMAP_STACK`, and Linux had this exact bug before 4.9), a
syscall TABLE with per-subsystem handlers (Linux's generated
`sys_call_table`, NT's SSDT), `(namespace, name)` settings (sysctl,
GSettings, macOS `defaults` -- a flat global name was the outlier), and
poisoning a revoked compositor mapping (the problem
`wl_buffer.release` exists for). Copy the SHAPE, not the size, and
CHECK the claim -- a confidently wrong premise is worse than no
comparison. CLAUDE.md carries the full version, including where to look
per area.

**AND THE RULE THE WHOLE DAY'S DOC WORK CONVERGED ON: prefer facts that
cannot go stale.** Everything removed as a maintenance burden was the
same shape -- a pointer to a NUMBER that some other file had to keep
true. Build numbers died with the changelog that indexed them; milestone
numbers cost three renumberings and a translation table before becoming
titles; target versions predicted releases nobody had committed to; test
counts in prose were wrong within weeks, twice. Name the thing, not its
index: "see `uui_route.c`'s pointer grab", not "see build 412". The safe
targets are the ones addressed by title -- a file or symbol, a
`docs/decisions.md` section, a named milestone in `docs/roadmap.md`, a
rule in CLAUDE.md or in this skill -- because a title survives edits and
reordering. Each of those four has a bar for entry, though: decisions =
why-this-way; roadmap = not built, or broken with a repro; CLAUDE.md = a
convention or trap that changes how you edit; this skill = how to work.
Anything meeting none of them goes in a comment beside the code or in
the commit message -- do not add content to a doc just to have somewhere
to point at.

**The split that makes this easy: for the PRESENT, point at a title; for
the PAST, point at a COMMIT.** A short SHA is the one number that cannot
go stale, because nothing has to be maintained for it to keep meaning
what it meant -- which is exactly what build numbers and changelog
entries failed at. Pair it with what it did ("the poison-page fix,
978ebf7"), since a bare hash tells a reader nothing, and reach for it
whenever the thing being referenced is a change rather than a state.
CLAUDE.md carries the full version, including the one hazard (a history
rewrite invalidates every SHA, and this repo has done one).

**Two process notes, both mine and both worth avoiding:**

- **AN edit that replaces a slice by index can silently delete or
  duplicate its neighbours, and it will do it three times before you
  learn.** This was written about DOCS and applies just as much to
  CODE: on 2026-08-31 a slice replacement in `tools/iso_guard.py`
  removed `BYPASS_ENV`, `ARTIFACT_PAIRS` and `UNSEEDED` along with the
  block it meant to replace. That one failed loudly (NameError) rather
  than silently, which was luck rather than a property of the method --
  the fix is `git checkout` the file and redo it with ANCHORED
  replacements. Mine did: three roadmap entries vanished in one commit
  (including a plan the user had asked for), an entry was duplicated in
  another, and then `s[:start] + new + s[end:]` on CLAUDE.md re-appended
  2,673 lines because the END anchor occurred EARLIER in the file than
  the start -- 3,094 lines to 5,789, in a commit whose diff was far too
  large to read.

  Three rules, each of which would have caught one of those: **search
  anchors DIRECTIONALLY** (`s.index(end, start)`, never `s.index(end)`),
  **assert the invariant you assume** (`end > start`), and **check the
  line count afterwards** -- a docs edit that changes a file's size by
  thousands of lines is not the edit you meant. `tools/check_docs.py`
  now fails on a repeated heading in any bulk-edited doc, which is the
  cheap signal for all three.
- **"It predates me" is a measurement.** Two failures were proved
  pre-existing by stashing the work and rebuilding the previous commit;
  one flake was honestly recorded as "stopped reproducing before I
  touched it, cause unidentified" rather than claimed as fixed. What is
  fixed is the fragility -- that is a different sentence, and the
  roadmap says so.

**2026-08-18 (a long day: the syscall table, two live memory bugs, a
process tree, and a scroll view). Read the first two before touching
memory or believing a memory measurement.**

Where the project stands after it, so a session does not re-derive it:

- **Syscalls are a TABLE**, `kernel/proc/syscall_table.c`, one row per
  number carrying the handler AND what `strace` prints. Handlers live
  with the subsystem that owns them. Adding one is three edits and no
  registry: a number in `abi/syscall_abi.h`, a handler plus its
  prototype in `kernel/include/kernel/syscalls.h`, a row in the table.
- **`tools/check_dispatch.py` fails the build** on an `if/else` or
  `switch` over ~20 branches, waivable in place with a
  `dispatch-ok: <reason>` comment. It exists because `syscall.c` grew to
  37 branches and NOTHING NOTICED -- one more `else if` is always
  cheaper than a table until it isn't.
- **Processes have a PARENT** (`ppid`), orphans are reparented, and
  `waitpid(-1)` reaps any child. That is stage 0 of
  `docs/init-design.md`, which plans init as pid 1, the tree under it,
  and the shell moving to ring 3. Read it before starting any of that:
  it records that `fork()` is NOT a prerequisite (`SYS_SPAWN` is already
  `posix_spawn`-shaped) and that `/proc` needs a mount table the VFS
  does not have.
- **A user mapping records whether it OWNS its frame**
  (`vmm_map_user_borrowed`), and `meminfo audit` checks every live
  address space against the allocator.

**THE MEASUREMENT LESSON, which is the most transferable thing here: AN
OVER-FREE FIRES ONCE AND THEN GOES QUIET.** Every GUI client maps the
kernel's glyph tables read-only and nothing unmapped them on exit, so an
exiting client returned four frames of the KERNEL IMAGE to the physical
allocator. The first measurement read `+4, +0, +0, +0` -- which looks
exactly like noise followed by a clean bill of health, and was very
nearly filed as that. `pmm_free_frame()` only counts a frame that was
marked used, so the second client to exit finds them already free and
changes nothing. **Any accounting check for this class must run on a
FRESH BOOT and believe only its first cycle**, which is why
`tools/frame_balance.py` boots its own VM.

**And the invariant nobody had ever checked:** a frame a live mapping
points at must be one the allocator considers HANDED OUT. Nothing
compared page tables against the bitmap. `meminfo audit` does, catches
both of the day's bugs, and was verified by reverting the fix and
watching it name the address. It audits only that direction on purpose --
a used frame nothing references (an ordinary leak) needs every kernel
owner to declare its frames, which is a separate project, and claiming
a "leak detector" that only worked one way would be worse than naming
the one it does.

**Killing a process freed NOTHING** until this day: `scheduler_kill()`
zombied it and `scheduler_poll()` reaped the slot, while
`syscall_process_exit_cleanup()` only ever ran from `sys_exit`. ~18
frames a kill, compounding, reachable from the desktop via Force Quit.
Two entry points now, and the reason is worth knowing: the exit path
switches CR3 to the kernel's address space, which is free when the dying
process is the one running and WRONG when the caller is somebody else --
the WM force-quitting a client would resume in the wrong address space.

**Testing memory: make the pattern depend on the ADDRESS.**
`/tests/memtest` fills everything `sbrk` gives it with
`(va * K) ^ salt`. A constant fill cannot detect two virtual pages
sharing one physical frame -- both read back the constant and look
perfect. Address-derived means the loser reads the OTHER page's value;
a random per-process salt extends it across processes; and keeping the
mix invertible lets a mismatch report WHOSE pattern the memory holds.
Proven by deliberately aliasing every 64th heap page.

**GUI: `uui_layout` OVERFLOWS rather than shrinking children**, so a
window smaller than its content silently hides part of it. A page whose
content can grow goes in a `uui_scrollview`, with tabs and status bars
OUTSIDE it. Three things that came out of building it:

- **A container declares its items through `uui_widget_ops.children`.**
  The router used to recognise containers by comparing `ops` against
  `uui_layout_ops`, which worked for one container and SWALLOWED every
  other one's child ids -- a press inside a scroll view reported the
  scroll view's id, so Control Panel's radio buttons silently stopped
  applying. A container with a `hit` clips its children; a plain layout
  declares none, deliberately.
- **The ROUTER paints the children, so a container cannot clip what it
  does not paint.** Hence `children_begin`/`children_end`. And the
  container's own `draw` is not called when it has children, or
  `uui_layout`'s would paint everything twice.
- **A `UUI_FILL` child now absorbs a SHORTFALL, not just leftover
  space.** Without it the scroll view took its full natural height and
  pushed the status bar off the window anyway: it fixed what was inside
  it and could do nothing about what came after.

**Four ways I misled myself in one day, all worth recognising:**

- **A harness that ACCUMULATES a cumulative source double-counts.**
  `mem_stress.py` appended `sh dmesg` each poll, but dmesg returns the
  whole buffer every time, so eight processes reported as sixteen passes
  and the early-exit fired before they had all run.
- **A helper that takes RELATIVE coordinates will happily accept
  absolute ones.** `cpanel_test.py`'s `click()` adds the content origin
  itself; passing screen coordinates offset them twice and landed
  outside the window, which reads exactly like a dead control.
- **A threshold picked without a control is a guess.** The new "the
  status bar survives" check passed WITH the bug present (777 px against
  4675), because the scrolled page's own content lands in those rows and
  "is there any ink" is satisfied either way. Run the control on the
  test you just wrote, and read which checks stayed green.
- **A count written out as a literal drifts.** `.widget_count = 4` while
  the array was 3 sent the router one item past the end into a garbage
  ops pointer. Derive it (`sizeof a / sizeof a[0]`).

**One process note.** A plan asked for first (`docs/init-design.md`)
changed itself in four places purely by measuring: no parent field
existed at all, there is no stdin, `/proc` needs a mount table, and the
roadmap's claim that init needs `fork()` was wrong. Half an hour of
greps beat a stage of building.

**2026-08-18 (second session that day: init as pid 1, a demand-paged
heap, malloc, and the roadmap reordered). Read the first two before
touching memory; the third is about tests, and it is the one that cost
the most.**

Where the project stands after it:

- **There is an `init`, it holds pid 1, and it cannot be killed.**
  `/bin/init` (`userland/bin/init.c`), spawned from `kernel_main()`
  before anything else -- being FIRST is the only reason it is pid 1,
  as on Linux. `kill 1` no longer restarts the desktop: find the
  `toywm` pid with **`ps`** (a real `/bin` program now, over
  `SYS_PROC_INFO`, with `--tree`) and kill that.
- **`SYS_SLEEP` exists**, because init had nothing to block on: it
  blocks in `waitpid(-1)` while it has children and sleeps when it does
  not. A caller with no scheduler slot gets -1, so a `/bin` program that
  sleeps CANNOT be driven with `run` -- use `spawn`.
- **`sbrk` reserves and maps nothing**; the heap is ~2046 MiB of address
  space and pages arrive on touch. The ring-3 map is sized for 4K
  (64 MiB per-window stride), which does NOT make 4K work --
  `pmm_alloc_contiguous()` and `WIN_CLIENT_MAX_W/H` still bound it.
- **`malloc`/`free` in ring 3 are the KERNEL's allocator compiled
  twice** (`kernel/lib/heap_core.c` + `api/heap_os.h`), the same
  shared-source rule as `geom.c`. `kernel/mm/heap.c` no longer exists.
- **The roadmap is ordered by what to build FIRST** -- four dependency
  phases, then tracks that depend on nothing. Every item is ONE LINE;
  detail goes to `roadmap-details.md` under the same heading. Keep it
  that way when you tick something.

**THE LESSON THAT COST THE MOST: A REDUNDANT CODE PATH MAKES A POSITIVE
CONTROL LIE.** Demand paging needed a fault-in hook in three places --
the #PF handler, the copy helpers, and `vmm_validate_user_range()` --
because ring 0 walks page tables rather than dereferencing user
pointers, so a syscall handed an untouched buffer never faults at all.
Disabling the hook in the copy helpers ALONE reddened nothing, because
the syscall under test validates its pointer first and that path has its
own fault-in. Both had to be disabled before anything failed. **When a
control fires nothing, ask what OTHER path could satisfy the same
assertion** -- this repo's existing "the fixture never reached the
branch" rule, arriving from a different direction.

**And it happened twice more in the same session, on checks I had just
written:**

- The init tool asked for "at least one orphan reaped" and stayed GREEN
  with adoption removed entirely, because a separate fix produces
  exactly one reap on its own. It counts against what the fixture says
  it abandoned now.
- The malloc test asked whether the process FOOTPRINT grew after freeing
  three blocks and requesting their combined size -- green with
  coalescing disabled outright, because the allocator claims memory in
  64 KiB regions and the region's own leftover satisfied the request
  either way. It compares the returned ADDRESS now (a merged block
  starts where the first of the three did), and that version reddens
  exactly one check.

**A FLAG YOU HAVE NOT GOT IS A CLASS OF BUG YOU CANNOT SEE.**
`USERLAND_CFLAGS` had no `-Wframe-larger-than` while the kernel has had
one for months. Adding it named four oversized frames in the WM the same
minute, the worst at **20,608 bytes against a 16 KiB ring-3 stack** --
a frame that does not merely overflow but steps clean over the single
4 KiB guard page into unmapped space (Stack Clash; Linux widened its
guard gap to 256 pages in 4.11 for exactly this). All four were
`struct dirent` arrays on the stack. Ask what the kernel side checks
that userland does not, and vice versa.

**Two smaller ones worth recognising:**

- **A region's END is what the next thing must clear, not its base.**
  Growing the heap into `0x8080000000` landed inside the compositor's
  window region, whose base looked isolated at `0x8010000000` while it
  spans gigabytes -- because its size is DERIVED
  (`MAX_PIDS * CLIENT_MAX * STRIDE`). Read a map of derived regions as
  a whole before moving anything in it.
- **A test's own bound goes stale with the map it was written against.**
  `guard_test` walked 1 MiB at a time up to 64 MiB to find the heap
  limit -- fine at ~14 MiB, meaningless at ~2 GiB, and with a lazy
  `sbrk` "walking to the limit" means trying to allocate the machine.
  Its own comment had predicted this after the FIRST time it happened.
  Provoke a bound directly (ask for a terabyte) rather than walking to
  it.

**One process note.** Four separate mid-task requests arrived while
building (4K, >4 GiB RAM, TTF fonts, GPUs). Three were already roadmap
items and one was not; the useful move was to MEASURE each against the
code before answering -- which found that ">4 GiB" is not a constant to
raise but a direct-map project, and that `kfree()`'s red-zone detection
silently depends on heap pointers fitting in 32 bits. Answer with the
dependency, not with enthusiasm.

**2026-09-02 (the laptop's GPU: an Intel display driver, brightness,
a triple-buffered page flip, and runtime mode switching). Read before
touching `kernel/drivers/display/`, `win_surface.c`, or the present
path in `ugfx.c`.** The state and the design calls are in
`references/session-design.md`; the five near-misses (rebuilding under
a running suite, a probe contaminating a guest, a persisted setting
outliving a crashed tool, a KTEST acting under a live compositor, and
the laptop's wrong `sum`) in `session-testing.md`; and the one lesson
that came from the maintainer's eyes rather than any tool -- the
two-buffer flip that tore -- in `session-diagnosis.md`. What is next is
marked `**NEXT**` on `docs/roadmap.md`: modesetting on the Intel
driver, EDID readout first.

**2026-09-03 (Intel modesetting stages 1 and 2: EDID over eDP AUX, the
firmware readout).** The EDID is a display-layer fact now
(`edid.c`, `read_edid`, `QUERY_DISPLAY`, `lsdisplay`), and the
laptop's firmware timing matched its panel's EDID to the kHz. What the
readout found, and what stage 3 (the native re-modeset, with DP link
training) needs from it, is under "Intel modesetting" in
`docs/roadmap-details.md`; the design calls are in
`references/session-design.md`. **Later the same day stage 3 landed too**:
`intel_modeset.c` re-programs the native mode end to end (panel power,
port clock, link training, the timings from the EDID) and the driver
advertises MODESET with that one mode; the lessons -- one mechanism per
flash, readback over eye, the T3 wait -- are in the same reference file. Stage 4a followed: smaller modes through
the panel fitter and a `system.scaling` setting (aspect | full |
center); the fitter's size register is its ARMING write. The blitter was then MEASURED and declined (a
software copy is 1.6 ms a screen), and the measurement found that
write-combining had been typing whole 2 MiB pages of RAM -- now split
to 4 KiB (`references/session-diagnosis.md`).


## Kernel-level traps that have bitten more than once

Worth checking against before debugging from scratch:

- **A misleading symptom usually means shared state.** A ring-3 client
  dying with a page fault at an unrelated syscall turned out to be a
  legacy `run` inheriting that client's RSP0 and overwriting its saved
  trapframe. If a failure appears in a component that did nothing wrong,
  ask what it SHARES with whatever ran just before.
- **A sentinel must be a value the call can never legitimately
  return.** "0 means try again" broke `read` the moment reads could
  block, because 0 is a real EOF.
- **Alignment bugs disguise themselves.** A crt0 that entered `main()`
  16-aligned instead of 8 faulted only SSE-using binaries; every plain
  program worked perfectly, which points nowhere near the stack.
- **An execution context the scheduler has no entry for cannot be
  treated as schedulable.** The legacy `process_run_ring3()` path has no
  `procs[]` slot, so it must not be switched away from.

**2026-08-22 (the TTY layer, in three commits). Read this before
anything terminal-, shell- or Ctrl-C-shaped.**

Where the project stands after it, so a session does not re-derive it:

- **A TERMINAL IS AN OBJECT** (`kernel/tty/`): an input queue, a LINE
  DISCIPLINE over it, an output sink, a `termios`, an owner and a
  foreground group. The physical console is `tty0`; a pty is the same
  object with a different DRIVER. `docs/tty-design.md` is the plan.
- **`Ctrl-C` WORKS IN A TERMINAL WINDOW**, through the same code as the
  keyboard's. The GUI Terminal is a real terminal emulator running
  `/bin/tosh` on a pty, so the shell in a window is a real process.
- **THE ANSI PARSER AND THE LINE EDITOR ARE BOTH COMPILED TWICE.**
  `kernel/lib/ansi.c` and `kernel/lib/klineedit.c` serve ring 0 and
  ring 3. Before writing either in ring 3, check what already exists.
- **`edit` IS `/bin/edit`** and `apps/ui/` is gone with it. The kernel
  draws no widgets.
- **`/bin/tosh`'S BUILTINS ARE THE ONES THAT HAVE TO BE.** `cd`, `pwd`,
  `help`, and (since job control) `jobs`, `fg`, `bg`. A builtin must not
  shadow a `/bin` program that does more; `cat`, `ls` and `echo` all
  did, and all three are gone. The test is "could a program do this
  better", and a builtin passes it by touching the SHELL's own state --
  writing it (`cd`) or reading it (`fg`).
- **New syscalls**: `SYS_OPENPTY`, `SYS_TCGET/SETATTR`,
  `SYS_TCGET/SETWINSZ`, `SYS_SET_NONBLOCK`. `SYS_TCSETPGRP` gained an
  fd. `SCHED_CHAN_KEY` is gone -- a reader parks on ITS terminal.

The lessons are in `references/session-design.md`,
`session-testing.md` and `session-gui.md` under the same date.

**2026-08-22 (job control, in three commits on top of the TTY layer).
THE THEME IS THAT A TEST READING THE SAME STATE THE CODE WRITES CANNOT
SEE THE BUG.** Read that before writing a test for anything the kernel
DECIDES.

Where the project stands after it:

- **A PROCESS CAN BE STOPPED**, and `stopped` is a FLAG beside its
  state, not a fifth `sched_state`. One line in `find_next_runnable()`
  honours it. `SIGSTOP`/`SIGTSTP`/`SIGCONT`/`SIGTTIN` exist;
  `docs/decisions/kernel.md` has why, and when to revisit.
- **STOP AND CONTINUE NEVER TOUCH THE PENDING SET.** They act at SEND
  time, which keeps `pending != 0` meaning "must die" with no policy
  lookup and makes `Ctrl-Z` safe from the keyboard IRQ. The cost: a
  `SIGTERM` to a stopped process waits until something continues it.
- **A JOB IS A PROCESS GROUP; THE JOB TABLE IS THE SHELL'S**
  (`userland/lib/tosh_jobs.c`). The kernel knows nothing about jobs, and
  nothing in it would be improved by learning. A job holds EVERY stage's
  pid -- one pid meant `fg` on a resumed pipeline leaked zombies.
- **`&` NEEDED `SIGTTIN` IN THE SAME CHANGE.** A background job inherits
  the shell's fd 0, so without it two processes read one keyboard.
  `tty_check_background_read()` is asked by BOTH ring-3 read paths.
  There is deliberately no `SIGTTOU`.
- **`SYS_WUNTRACED` reports a stop as `SIGNAL_STOP_BASE + sig`** (256),
  beside the existing 128 + sig -- so a waitpid result can now name a
  child that is STILL ALIVE, which is why it is a separate wrapper
  (`sys_waitpid_untraced()`) rather than a flag.

The lessons are in `references/session-testing.md` and
`session-design.md` under the same date.

**2026-08-28 (dynamic linking, Stages 0-3, in four commits). Read this
before anything mmap-, loader- or libc-shaped.**

Where the project stands after it:

- **`mmap`/`munmap` exist** -- a region list in `struct sched_mm`, an
  arena above the window regions, file-backed demand paging, `pmap`
  over `QUERY_PROCMAP`. A file-backed fault-in REFUSES inside an FS_OP.
- **The userland is PIC at the same base** (`-fpie -mcmodel=small`);
  the TLS geometry is DATA in `.rodata` (`__rt_tlsdesc`), not *ABS*
  symbols, and `linker_value()` is gone.
- **Dynamic linking works end to end**: `PT_INTERP` makes spawn load
  `/lib/ld-toy.so` (a fixed-base ET_EXEC -- the kernel never learned
  ET_DYN) and enter it with a minimal auxv; the loader mmaps DT_NEEDED
  libraries from /lib, relocates eagerly, resolves exe-first.
- **tolibc ships as `/lib/libc.so` and every /bin and GUI program
  links it.** Static by contract: init, toywm, /tests, ld-toy itself.
  pthread is `libc_nonshared.a`. musl was sized and DECLINED
  (`docs/decisions.md`, "tolibc stays").
- **The `#` shell's bare name SPAWNS AND WAITS now; `run` keeps the
  legacy loader** and refuses dynamic binaries by name -- the harness
  still drives static /tests through it for the exit banner.
- **Three flip bugs, all fixed and written up**: the /lib image cache
  (per-spawn page faults through the FS made the whole machine
  stutter), `SPAWN_FOREGROUND` (the tcsetpgrp-after-spawn race), and
  `fd_inherit()` taking a NAMED parent instead of CR3.

The lessons are in `references/session-design.md`,
`session-diagnosis.md` and `session-testing.md` under the same date.

**2026-08-18 (init stage 2: a boot target, services, supervision). THE
THEME OF THIS SESSION IS THAT AUTOMATING A LIFECYCLE REMOVES INTERLOCKS
NOBODY KNEW THEY DEPENDED ON.** Read that first; it caused four of the
five bugs below.

Where the project stands: init reads `system.default_target`
(`text`/`graphical`, overridable for one boot with `target=` on the GRUB
line) and starts the services described in `/etc/services.d`. The desktop
is one of them -- init's child, restarted when it dies, with a backoff and
a crash-loop give-up. `gui` still starts one by hand and now refuses when
a desktop already holds the compositor role.

**THE INTERLOCK LESSON, in the four places it bit.** Before this, `gui`
blocked the shell inside its own spawn-and-wait loop for the desktop's
entire lifetime. That was never a designed guarantee, and four things
were silently relying on it:

  - **The physical shell stopped competing for the keyboard.** With the
    desktop started by init, the shell sits at a prompt behind it and both
    drained the same key ring -- a key typed at the desktop got executed
    by an invisible shell. Fixed with a suspend flag set from the one
    place the compositor role changes.
  - **...and the shell stopped DRAWING.** Suspending the read was not
    enough: the wait loop's BODY calls `vga_cursor_tick()` and
    `vga_present()`, which publish into the framebuffer the compositor
    owns. A blinking text cursor appeared on top of a desktop icon. **The
    user found it in a screenshot while all 23 GUI tools were green.**
  - **Two GUI tools could take the compositor role and keep it.**
    `screen_surface_test.py` and `compositor_death_test.py` both kill the
    desktop first; init restarted it with a zero backoff and it claimed
    the role straight back.
  - **`Exit to shell` worked.** It makes the desktop return 0, and an
    unconditional restart put it straight back, so the menu item silently
    did nothing.

So: **when you make something automatic, enumerate what the manual step
was quietly guaranteeing.** "Nothing else was running at the same time"
is the most common one.

**AND THE GAP THAT LET THE CURSOR BUG THROUGH IS STRUCTURAL, NOT AN
OVERSIGHT.** Every GUI tool here DRIVES the desktop and then asserts on
what changed. Not one asked the opposite question -- with nobody touching
it, does the screen sit still? A suite built entirely from "do X, check Y
changed" cannot see anything that happens when nothing is done.
`tools/idle_desktop_test.py` asks it now, and its own control is the
part to copy: the taskbar clock must CHANGE, which is what proves the
capture pipeline can see motion at all. Without such a control, a harness
returning one cached frame reports a beautifully steady desktop and
passes.

**A PREDICATE NAMED AFTER THE ONLY IMPLEMENTATION IT EVER HAD.**
`win_server_active()` meant "a RING-0 presentation layer is registered"
(the predicate and that layer are both deleted now).
The desktop stopped being one when it became a process, and three KTESTs
that guarded themselves with it -- so they would SKIP while a desktop was
up -- quietly stopped skipping, with their comments still claiming they
did. Nothing noticed until `make test` finally ran with a desktop up.
`win_server_any()` is the predicate that covers both kinds, and one
non-test caller had been open-coding it correctly all along. **When a
subsystem gains a second implementation, re-read every predicate named
after the first.**

**"IT PREDATES ME" IS A MEASUREMENT, AND IT PAID FOR ITSELF TWICE.**
Three intermittent ktest failures appeared. Stashing the whole session
(`git stash push -u -m <tag>`, apply by SHA, never a bare pop) and
building the previous commit gave rates rather than verdicts: the
console-timeout flake reproduced **5 boots in 9** on HEAD, and
`heap-debug`'s use-after-free check **1 run in 15** in-guest. Both
pre-existing, both now recorded with the measured rate instead of a
guess. `tools/flake_hunt.py ktest -n N` is the loop for this now.
The one that WAS mine, found the same way: three heap KTESTs compared
`heap_used_bytes()` against a snapshot, which any concurrent kernel
allocation breaks -- newly true because a desktop is always running.
Fixed by establishing the precondition (`scheduler_preempt_disable()`),
not by loosening the assertion.

**A KTEST RUN LEAVES STATE ON `disk.img`, so a rate that CLIMBS run over
run is a dirty fixture, not a worsening bug.** One failed write leaked
two blocks, and the next two runs' `fsck` checks failed against the
leftovers. `make clean-disk && make iso` between batches.

**WHEN A CHECK YOU JUST WROTE ASSERTS AN INTERMEDIATE STATE, ASK WHETHER
IT STILL EXISTS.** Both compositor tools asserted "the role is released"
by SAMPLING it -- and supervision refills it before a sample can see it
empty, reusing the dead process's slot so even the pid looks unchanged.
The first fix weakened the assertions to tolerate that. The right fix was
to ESTABLISH the precondition (`rm /etc/services.d/toywm`, then kill) and
keep the original assertions, which is strictly better evidence for the
same property.

**AND ONE FACTUAL ASIDE COST A DESIGN DECISION.** Presenting the restart
policy options, the note against "restart only on non-zero exit" said "a
kill is exit code 0-ish here, so `kill` would still restart it". That is
false -- a killed process reports -1. A wrong aside made the right option
look useless, the wrong one was chosen, and it broke *Exit to shell*.
**Check the claim you attach to an option, not just the options.**


**2026-10-07 (run programs from the File Manager; a refactor round).**
Feature: the "Run backup.sh?" card (`lib/ulaunch`, `ui/uui_runask`,
`/bin/wm/system/runask`, `uterm -e`), chosen from a mockup canvas
(D1, apps run and the rest ask, all extras, shared). Bug: dropdowns in
dialog windows never shown, because only the main window presented
popups (0572ecce). Then the maintainer asked what to refactor, then
asked for the MODULAR version of each rather than file moves, and all
of it landed except one: `uopen_decide()`; declared options
(`lib/uprefs` + bound `uui_prefs` rows, all four Options windows);
one toplevel type in uapp and its split; the kernel shell's chains;
Terminal's engine as `ui/uvterm`; the xHCI split. **The xHCI virtual
root hub waits** (roadmap): it changes root-port behaviour, the Lenovo
was down, and on the ASUS USB is the network link. Lesson: "should we
refactor?" was answered with file sizes and duplication counts; the
better answer came on the second question -- which seams carry a REAL
bug class (two present paths, a copied options cycle, two port state
machines) -- so ask that one first.
