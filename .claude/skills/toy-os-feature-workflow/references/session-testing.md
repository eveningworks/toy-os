# What past sessions learned: testing and verification

How to prove a change works here, and -- mostly -- the many ways a green
suite turns out to be measuring nothing. Grouped from the running log
past sessions kept; entries are verbatim and dated where they were
written, newest ideas not necessarily last.

**The rule underneath almost every entry below: ask what a broken
version would still pass.** Everything else is a variation on it.

`CLAUDE.md` and `docs/testing.md` are the current reference and win over
anything here. These are lessons, not specifications.

**Three testing lessons from that day, the first of which generalises
past this repo:**

- **A positive control can turn nothing red because the test's DATA
  never reached the code under test.** The truncate tests wrote 16 KB,
  which fits TFS3's twelve direct pointers, so disabling the
  indirect-table handling entirely changed no result. The suite was
  green, thorough-looking, and blind to that path. When a control
  fires nothing, suspect the FIXTURE before the harness, and ask what
  input size or shape actually crosses the branch.
- **`ls` is a real `/bin/ls` ELF.** A test against a freshly formatted
  image has no `/bin`, so directory assertions written with `ls`
  measure whether the test seeded the image. Use shell builtins
  (`stat`, `cat`) when the image isn't seeded.
- **A repeated command needs the LAST section, not the first.** A
  check that read the first `cat f` rather than the one after the
  truncate would have passed whether truncate did anything or not --
  the same shape as the log-parsing "parse ONE frame" lesson above.
- **A leak from a positive control PERSISTS on `disk.img`** -- `make
  iso` re-seeds by sync, never reformat -- so the next run's `fsck`
  tests fail against damage the previous run left. `make clean-disk`
  after any control that leaks blocks, before believing a red result.

**And four testing lessons from the same stretch, each of which cost a
green suite that was measuring nothing:**

- **`gui spawn PATH` runs a ring-3 binary with no Terminal in the loop**
  -- use it. Tests used to open a Terminal and type at it, which drags
  that Terminal's allowlist, its single pending-process slot and its
  shell into a test about something else, and makes a client that never
  exits untestable (the Terminal that spawned it then cannot close).
  Also new: `gui rclick X Y` and `gui ctxmenu` (nothing could open a
  context menu, which is how its Close went on seizing windows
  unnoticed), `gui dialog [--json]` (the confirm dialog's message and
  button centres), a `not_responding` field on `gui windows --json`, and
  `gui key <c> alt|ctrl|shift` -- `gui key 0xa5 alt` is Alt+F4.
- **A log-parsing test must parse ONE FRAME, not the whole log.** An app
  re-logs its layout every draw, so a parser taking the last occurrence
  of each key reports every popup that has EVER been open as still open.
  Written that way first, it turned four real passes into failures and
  would have passed a menu that never closed. Slice from the last
  frame-boundary line.
- **A geometry line an app does not report is one a tool will
  re-derive.** `notepad_client_test.py` sampled the text area through a
  hardcoded band; when the menu bar (shorter than the old toolbar) moved
  the text up, that band sampled blank background in every state -- so
  "typed", "cleared" and "reopened" all compared EQUAL, two checks
  failed and a third passed for the wrong reason. Add the log line.
  Likewise `dialog_test.py` found buttons by colour-scanning a row,
  which silently only ever worked for a Yes/No dialog.
- **Check that a passing check could have failed.** The Alt+F4 close
  check passed VACUOUSLY on an already-destroyed window -- found only by
  running the positive control and reading the checks that stayed green.
  Positive controls tell you which check is load-bearing, and sometimes
  that none is.

**And the testing lessons from the same day, which cost real bugs:**

- **A user found three bugs a 91-check suite passed.** The Calculator
  shipped with NO VISIBLE BUTTONS, because every check asserted that
  clicking one changed the display -- which it did. **"It responds" is
  not "it is drawn."** Assert visibility separately.
- **Moving identical content is pixel-identical.** A scroll test typing
  forty copies of one line cannot tell a working scroll from a dead one.
  Number the rows.
- **A test must not assume the thing it is testing** -- reaching a start
  position by scrolling, to test scrolling.
- **Ask the app where things are.** Every testable app logs
  `<app>: layout <what> x y w h` on stderr. Four tools have now been
  bitten by re-deriving geometry in Python; one stayed GREEN for a
  release while clicking several pixels off centre.

**2026-08-13 (later): GUI work has real tooling now -- use it before QMP.**

- **`gui <sub>` on the serial debug console** (`userland/wm/wm_debug.c`),
  live while the desktop is up: `gui windows|probe X Y|menu|taskbar|
  state|apps` (each `--json`), and `gui open <App>|close N|click X Y|
  drag X1 Y1 X2 Y2|key c|wheel N`. `tools/gui_debug.py`'s DebugConsole
  wraps it; `menu_row("Terminal")` gives the real row centre instead of
  gui_flow.py's hardcoded arithmetic. Injected input enters BELOW the
  PS/2 driver and is asynchronous (events drain one per frame -- call
  `settle()`), because these commands run inside `wm_run()` itself.
- **`gui damage verify on` catches the WM's worst bug class**: the
  compositor repaints only declared damage, and anything undeclared
  leaves stale pixels with no crash and no assertion. Verify renders
  every frame twice and reports differing pixels with coordinates. It
  found four real bugs in its first minute. Turn it on for ANY change to
  drawing, damage, focus or chrome.
- **"UI Demo" (`apps/uidemo.c`) is a GUI app built to be tested
  against**: one of every `apps/ui/` widget at documented offsets, every
  interaction logged as one parseable line (`uidemo: button 2`,
  `uidemo: cancel btn`). Drive with `gui click`, assert on the log --
  no screenshot in the loop.
- **Widget behaviour belongs to the widget, configuration to the app**
  (`docs/gui-guidelines.md`). `apps/ui/ui_textview.h` is the worked
  example: scrolling was copy-pasted into three apps and the third copy
  shipped a scrollbar that drew and did nothing.
- **A graphics card is a `display_driver`** (`kernel/include/kernel/
  display.h`, drivers in `kernel/drivers/display/`). Adding one is a
  file plus a `display_register()` line.

**A lesson this session paid for three times: a passing test can be
testing the wrong path.** The GUI looked fine while the console was
frozen (GUI is double-buffered, console isn't). A cursor-trail repro
found nothing because QEMU coalesces mouse motion. A "negative control"
proved nothing because the drag started one pixel off the resize grip.
When something works in your test and not for the user, suspect the
harness before the code, and say plainly what you could NOT verify.

**Verify GUI work by pixel value, not by eye** (`tools/pixel_probe.py
--compare a.png b.png --at X,Y`). A hover state that moved a 235/255
background by two units looked completely plausible in a screenshot.
Always sample a control that should NOT have changed too.

**`ata nodma on|off`** forces the PIO disk path -- the fallback is
otherwise unreachable, and it's also the PIO-vs-DMA comparison that has
root-caused a real DMA bug before. **`make run KVM=1`/`vm.py --kvm`**
exist now, but KVM is ~1.9x SLOWER for disk I/O, so never compare a
throughput number across the two modes.

**2026-08-14 (later): geometry, and two ways a test can look fine and
prove nothing.**

- **A moving shape defeats a fixed sample point.** The usual advice
  here -- read pixel values at a coordinate -- silently stops working
  when the thing under test rotates: the curve moves off the sample
  point, so the probe reads background either way. Two assertions that
  DO work, both in `tools/gfxdemo_test.py`: compare whole regions
  between frames, and count DISTINCT COLOURS to tell anti-aliased from
  aliased drawing (468 vs 5 in the demo's canvas, because partial
  coverage is precisely what AA emits).
- **Pair "it changed" with something that must NOT change.** "The frame
  differs between samples" is satisfied by a blinking caret; "the frame
  is identical" is satisfied by a dead app. Only the pair -- it rotates
  at speed 3, it is pixel-identical at speed 0 -- rules out both.
- **A ring-3 GUI app had nowhere to log.** `sys_print()` goes to stdout,
  which for a windowed client is nowhere useful and for a spawned
  process is its parent's pipe. Diagnostics go to `sys_eprint()`
  (stderr), which the kernel routes to the kernel log and `dmesg` --
  that is what a test tool reads, the same path `strace` uses. Finding
  this took a confusing half hour of "the app is clearly running and
  the log is empty".
- **`--positive-control` does NOT break the kernel for you.** It only
  inverts the exit code; running it against a healthy build correctly
  reports failure. To actually validate `damage_sweep.py`, comment out
  one `wm_damage_rect()` call, rebuild, run it, then restore. Doing
  that caught the removed taskbar damage at y=695 -- proof the harness
  checks something, which a green run alone never is.

**2026-08-14: three committed test harnesses, and the harness lying
twice more.**

- **Run these before delivering GUI work. They exist so you don't
  hand-roll a driver script each time:**
  - `python3 tools/damage_sweep.py [--random N --seed S]` -- walks the
    interactions that break the damage invariant, exits non-zero on a
    violation. `--positive-control` inverts the exit code, for proving
    it detects a real one. Three of the five damage bugs it found came
    from the RANDOM walk, not the fixed sequence.
  - `python3 tools/uidemo_test.py` -- 27 assertions over UI Demo's
    widgets (selection, cancel paths, keyboard nav, Tab/Shift-Tab,
    Space-activates). Geometry comes from the app's own `uidemo: layout
    <widget> <x> <y> <w> <h>` lines -- never re-derive row offsets in
    Python, they drift the moment a row is added.
  - `python3 tools/dialog_test.py` -- the confirm dialog by pixel value.
  - `python3 tools/gfxdemo_test.py` -- the geometry primitives (2D AND
    the 3D projection behind the wireframe cube) and the `uui_canvas`
    widget, via the ring-3 "Shapes" demo.
  - `python3 tools/scrollbar_test.py` -- scrollbar behaviour against the
    ring-3 Notepad, per `docs/gui-guidelines.md`'s scrollbar section.
  - `python3 tools/menubar_test.py` -- the menu bar, its nested
    submenus and the status bar, against the ring-3 Notepad.
  - `python3 tools/forcequit_test.py` -- not-responding detection,
    the force-quit dialog, `scheduler_kill()` and slot reaping. Read its
    docstring for why `winclient` (declines) and `hangclient` (wedges)
    are tested AGAINST each other rather than separately.
  - `python3 tools/tfs3_v1_test.py` -- proves the kernel still mounts
    and uses a TFS3 **v1** image (8 checks). After any change to TFS3's
    geometry, journal or an operation's credit count.
  - `python3 tools/faulttest_run.py` -- the `/tests` binaries that fault
    ON PURPOSE, asserted against the KERNEL's serial-log report with
    required AND forbidden substrings (a stack overflow and a null
    dereference are both page faults). Own QEMU per test, because a
    ring-3 crash kills the serial debug console -- `vm.py` cannot drive
    these at all. Run it after touching the fault path, the ELF loader,
    or the user address-space layout.
  - `python3 tools/watch_vm.sh [slot...]` -- not a test: attaches a
    VIEW-ONLY VNC viewer to a running VM so the user can watch. Remmina's
    `-c vnc://...` URI cannot be view-only, and a viewer whose mouse
    fights the synthetic input looks exactly like a flaky test.
  - `python3 tools/gen_decisions_index.py` -- regenerates
    `docs/decisions.md` from `docs/decisions/`. Run it after adding a
    decision entry; `--check` is what check_docs.py calls.
  - `python3 tools/check_docs.py` -- the documentation rules: no
    pointer to the deleted changelog, no numbered or versioned milestone
    heading, no duplicated roadmap entry, no link to a doc that does not
    exist. In `preflight.sh`, so you rarely run it by hand; reach for it
    directly after any bulk docs edit, because a slice-replacement that
    lands on the wrong boundary is invisible in a diff.
  - `python3 tools/usertest_run.py` -- the NON-GUI ring-3 diagnostics in
    `/tests`, as one table. In `preflight.sh`, so you rarely run it by
    hand; reach for it directly after touching `userland/rt/`,
    `userland/lib/`, the ELF loader or a syscall.
  - `python3 tools/compositor_test.py` -- M41 stage 2's raw input path
    to a registered ring-3 compositor. Its design point generalises:
    every injected input is asserted TWICE, in the compositor's log AND
    in UI Demo's, because "the compositor got the click" is equally
    satisfied by an implementation that STOLE the input stream.
  - `python3 tools/crashtest_test.py` -- the fault paths (9 checks):
    the Crash Test app enumerates the kernel's fault kinds, kernel
    faults are REFUSED while disarmed, and a ring-3 crash kills the app
    without taking the desktop with it. Safe in gui_regress only
    because the dangerous half needs `faultinject` on the command line.
  - `python3 tools/panic_resolve.py < panic.txt` -- not a test: names
    every address in a pasted panic. Checks the build id first and says
    so when it does not match, because wrong names are worse than none.
  - `python3 tools/cursor_theme_test.py` -- cursor themes: the theme
    loads COMPLETELY, switching it changes the drawn pointer, the size
    setting scales it by the right magnitude, and a theme that does not
    exist still leaves a working pointer. Read its docstring first: the
    built-in fallback means "a cursor is on screen" proves nothing.
  - `python3 tools/gen_cursors.py` -- not a test: generates the shipped
    cursor themes into `data/cursors/` and is the authoring path for a
    new one. `--check` fails if they are stale. Re-run it after touching
    the built-in shapes in `userland/wm/wm_render.c`, which it extracts from.
  - `python3 tools/desktop_entries_test.py` -- the `.desktop` entry
    system: `ShowIn=` and live reload. Its ShowIn checks assert
    LOADED-but-filtered (`gui apps` vs `gui menu`), never just "absent"
    -- the first version asserted only absence and passed with the
    filter disabled outright.
  - `python3 tools/single_instance_test.py` -- one copy of an app, and
    relaunching it RAISING the copy that exists (`WIN_REQ_ACTIVATE`,
    `UAPP_SINGLE_INSTANCE`). Two of its checks are worth copying: it
    carries a multi-instance CONTROL (UI Demo declares no app id, so two
    windows is correct there, and an over-eager match reddens exactly
    that check), and it identifies the raised window by `client_pid`
    rather than by title -- the title version stayed green through the
    positive control, because a brand-new window is frontmost too.
  - `python3 tools/flake_hunt.py <tool> -n N` -- one tool N times,
    reporting which CHECKS failed and how often. Reach for it the moment
    a tool fails once and passes on re-run; a rate is the diagnosis, a
    verdict is not. Scores a run that printed no summary as `error`, not
    `pass`.
  - `python3 tools/frame_balance.py` -- does a process's teardown return
    exactly what it took? Two directions needing opposite fixes: DOWN
    and staying down is a leak, UP is an OVER-FREE (teardown handed back
    frames it never owned), which is quieter and worse. Boots its own VM
    because an over-free is only visible on the first cycle.
  - `python3 tools/mem_stress.py -n 8 --mem 128` -- several memory hogs
    at once, with a guest small enough that they actually run out.
    Drives `/tests/memtest`, whose pattern is ADDRESS-derived so two
    mappings sharing one frame is detectable at all. Ends with
    `meminfo audit`.
  - `python3 tools/check_dispatch.py` -- fails on a dispatch chain over
    ~20 branches; waive one in place with `dispatch-ok: <reason>`. In
    preflight and CI, so you rarely run it by hand.
  - `python3 tools/gui_regress.py` -- ALL of the app-level ones above
    plus the ring-3 client tests, each on its own fresh disk copy and
    its own VM, as one pass/fail table (~300 checks across twenty-three
    tools, a couple of minutes at the default -j4). **Always pass
    `--logs DIR`**: an intermittent too rare to reproduce on demand can
    only be diagnosed by a capture that was already running, and that
    cost one flake three sessions of anonymity. Reach for this first;
    drop to an individual tool only when one fails.
- **`settle()` POLLS now** (`gui state`'s `pending` = the WM's
  undelivered-event count). Never replace it with a sleep: the loop is
  not a metronome -- a drag takes ~110ms normally and ~800ms under
  `gui damage verify on`. The fixed sleep it replaced didn't fail
  loudly, it just let windows move between a `gui windows` read and the
  command using those coordinates, so the same script reported a
  different bug on every run.
- **Hover needs the REAL cursor, parked, and verified.** `gui move`
  holds for ONE WM iteration -- injected input overrides the mouse for
  that tick and then the real pointer takes over. Use
  `DebugConsole.warp_cursor(qmp, x, y)`, which drives the real PS/2
  cursor and confirms arrival via `gui state`; `QMPSession.goto()` is
  open-loop and a large jump was measured landing a THIRD of the way.
- **Don't sample the pixel under the cursor.** The sprite draws
  down-and-right from its hotspot with a black outline. A hover check
  written that way "passed" by reading pure black -- a green test
  measuring the cursor, not the control.
- **Route keys by FOCUS** (`apps/ui/ui_focus.h`), never by trying each
  widget in turn: the first one tried swallows every key it recognises.
  A widget joins by exporting one `const struct ui_focus_ops`. Tab
  order is array order.
- **Modifier bits ride alongside the key** (`KEY_MOD_*`,
  `keyboard_try_getchar_mods()`, `on_key(win, key, mods)`). The key half
  is unchanged and still terminal-encoded, so CLI consumers are
  untouched. Ctrl/Alt are already folded into `key` -- only
  `KEY_MOD_SHIFT` earns its keep (Shift-Tab is why it exists at all).
- **Popups are a SECOND draw call the app makes last**
  (`ui_dropdown_draw_popup()`): drawing is immediate-mode, so z-order is
  call order, and input is forwarded in the reverse order.
- **Disk image: `du disk.img`, not `ls -l`.** It is 9 GiB apparent and
  should be ~3 MiB allocated. The kernel TRIMs as it frees blocks now,
  but if it ever looks large run the MATCHING writer tool's trim --
  `python3 tools/tfs3_writer.py trim disk.img` for a fresh-built image
  (TFS3 is the default format now, see the 2026-08-14 section below),
  `tfs2_writer.py` for an old TFS2 one; each refuses the other's
  images, so the wrong pick fails loudly rather than damaging anything
  (non-destructive either way -- only already-free blocks).

**The harness lied twice more, in new ways worth recognising:**

- **A test that PASSES can be measuring the wrong thing.** The hover
  check above read (0,0,0) and reported success; that was the cursor
  sprite sitting on the sample point, not a hover wash. When a result
  looks right, ask what else could produce that exact value.
- **A recorded "known issue" can simply be wrong.** A papercut written
  from reasoning ("`ata nodma` disables TRIM") turned out false the
  moment it was measured -- the code path didn't consult the flag at
  all. Measure a claimed bug before fixing it, and DELETE a wrong entry
  rather than amending it, since a corrected one still implies something
  is broken.
- Corollary that keeps paying: use a **positive control**. Break the
  thing deliberately and check the tool notices, before trusting a clean
  run. That is how the damage sweep, the keyboard path and the TRIM
  reclaim were each confirmed to be checking anything at all.
- **When a known issue records competing hypotheses, design ONE
  experiment whose outcome differs under each -- don't fix toward
  either.** The damage sweep's "20px" issue recorded two candidate
  causes needing opposite fixes; a discriminating A/B (verify OFF so
  nothing self-repairs, screenshot the live screen, force a full
  repaint, diff the two) proved the screen genuinely stale in one
  shot -- and the root cause turned out to be a THIRD thing neither
  hypothesis named (a doc-vs-implementation contract mismatch in
  `gfx_set_clip_rect()`), with the recorded issue's own probe detail
  ("inside Terminal's content") simply wrong. Both recorded guesses
  being wrong is a live possibility; the experiment doesn't care.
- **A brand-new test's first catch is often a DESIGN hole, not a code
  bug.** `fs_switch_test.py`'s first-ever run found that reformatting
  a TFS3 disk as TFS2 left TFS3's backup superblocks claiming the
  corpse -- nothing any unit test of either backend could see,
  because the bug lived in the interaction the new test was the first
  thing ever to exercise. Budget time for the first run of a new
  integration test to find something real.

**Five testing lessons from that session, each of which cost something:**

- **"It responds" is not "it is drawn", again.** UI Demo shipped blank
  with 35 checks green because every one asserted on its LOG.
  `tools/blank_window_test.py` now opens EVERY app in the registry and
  requires more than a flat fill -- and it asks the KERNEL for the app
  list, so a new app is covered without anyone remembering.
- **A positive control can fail to fire because the FIXTURE is too
  big.** The new fs geometry KTEST stayed green with the `df` bug
  reintroduced: on a 9 GB disk, one group over-reported is 1.4%. The
  check that catches it runs against the ~24 MB live volume. When a
  control does not fire, ask whether the test's DATA can express the
  bug at all -- and write the limitation into the test.
- **An absence check cannot catch a dead control.** "The drag changed no
  selection" stayed green against a scrollbar that did nothing. Any
  assertion of the form "X did not happen" passes trivially when the
  control is inert.
- **Instrument rather than theorise.** `fsformat` failing with no
  message was found in ONE run by adding `klog_printf(__LINE__)` to
  every `return 0` in the format path. Three prior guesses were wrong.
- **A tool that fails once and passes on re-run is a KNOWN ISSUE, not
  noise.** `menubar_test.py` did that three times in one session; it is
  recorded in `docs/roadmap.md` with what has NOT been established (the
  failing check name), because "re-run it" is how an intermittent bug
  gets ignored for months.

**2026-08-16 (last session of the day): measure the gates before
trimming them, and five smaller lessons.**

- **The verify gates are CHEAP. Measure before you propose trimming
  one.** Asked which routinely-run tools don't earn their keep, this
  session's first instinct was "drop `make clean` from
  `preflight.sh`" -- and then measured the whole of preflight
  (clean rebuild + iso + check_deps + check_layout + boot smoke +
  183 ktests + 8 usertests) at **25 seconds**. The recommendation was
  wrong and was withdrawn before it shipped. `gui_regress.py` is the
  only gate with real wall clock, and even that is ~1.5 min.
- **When a suite is slow, suspect the SCHEDULE before dropping
  coverage.** `gui_regress` ran 339s of tool-time in 116s at `-j4`
  because `forcequit` (71s, genuinely waiting out ping timeouts) sat
  eleventh of fourteen and finished alone at the tail. Sorting the
  submission order longest-first (LPT) took it to 1:30 -- a 23% cut for
  a sort, with nothing removed. Dropping tests is the expensive fix;
  reach for it last.
- **A user-reported bug's SURVIVORS are the diagnosis.** The demo tour
  printed "Unknown command: lscpu" while `about`, `df`, `fsck`, `ls`
  and `lspci` all worked. That pattern *was* the answer: every survivor
  has its own builtin dispatch entry and `lscpu` was the one command
  resolved through PATH. Read what still works, not just what broke.
- **...and don't abandon a correct diagnosis when one fact seems to
  contradict it.** "PATH is empty" looked refuted because `ls` worked --
  until `dispatch()` showed `ls` is a builtin *wrapper* that hands
  `/bin/ls` an absolute path. Find the mechanism; do not flip to a new
  theory on the first apparent contradiction, and do not assert past it
  either.
- **An init step reachable by only ONE entry point is a bug waiting for
  a second entry point.** `shell_main()` held the only calls to
  `history_load()`/`shell_path_init()`, and `apps/demo.c` and the serial
  debug console both reach `shell_dispatch()` without it. Now an
  idempotent `shell_session_init()` called from both. Same family as
  `vfs.c`'s `ensure_layout()`; the `static int done` guard is what makes
  the rule unconditional.
- **`tools/demo_test.py` exists and is ON DEMAND ONLY** -- standing
  request; do not add it to `preflight.sh`, `gui_regress.py` or CI, it
  boots its own ISO and the demo is a showpiece. Its positive control is
  the reusable part: reverting the fix reddened exactly one of six
  checks and left "booted / reached the desktop / opened windows" green,
  which is the proof those three were never evidence the tour worked.
- **A pasted screenshot can arrive as raw PNG bytes.** When that
  happens, don't ask the user to re-send -- read the clipboard directly:
  `wl-paste -t image/png > shot.png` (or `xclip -selection clipboard -t
  image/png -o`), then Read the file.

**2026-08-17 (single instance, CPU accounting, clocksources): four
lessons, and the first two are about tests that look like they work.**

- **A positive control can pass because the bug produces `==` where you
  asserted `>`.** CPU accounting billed a whole tick per `SYS_YIELD`,
  and the test asserted "billed must not EXCEED elapsed". A yield
  returns about a tick later, so the buggy kernel bills exactly one tick
  per tick and lands on `billed == elapsed` -- a flat 100%, the reported
  symptom, sliding straight through a `>` comparison. The check only
  became real when it asserted a process doing nothing but yielding is
  billed SUBSTANTIALLY LESS than the window. **Ask what value the bug
  actually produces, not merely which direction it errs in** -- and the
  only reason this was caught is that the control was run at all.
- **The same shape again, from the other side: a check can pass for the
  wrong reason because the broken version satisfies it differently.**
  "The relaunch brought the window to the front" stayed green against a
  kernel with the raise disabled, because a brand-NEW window is
  frontmost too. Comparing the window's `client_pid` against the
  original's is what made it load-bearing. Both lessons are this repo's
  existing rule -- ask what a broken version would still pass -- so
  treat that rule as covering the ASSERTION's exact form, not just its
  subject.
- **A feature's primary path can be unreachable in the test
  environment, and a green suite then proves nothing about it.** The TSC
  clocksource needs an invariant TSC, and plain QEMU cannot provide one:
  TCG does not implement `invtsc` (it warns and clears the bit) and KVM
  withholds it even under `-cpu host` because a guest that has seen it
  cannot be live-migrated. `python3 tools/vm.py --kvm --cpu host,+invtsc`
  is the ONLY way to run that code. Before concluding a CPU feature
  "isn't available in QEMU", check whether it needs an explicit `+` flag
  AND which accelerator implements it -- those are independent. Then do
  what this repo always does for the other direction: `notsc` on the
  GRUB line keeps the coarse path reachable, as `nopat` and `ata nodma`
  already do.
- **Writing an invariant in a comment does not make you obey it.** The
  new billing carried "every path that stops running the current process
  must bill BEFORE changing `current_index`" -- and the same commit
  missed the kernel-context path, so a process was charged 9.51 SECONDS
  across a 300ms window. The test caught it. When you write a rule of
  the form "every path must X", immediately enumerate the paths and
  check them one at a time; the comment is a claim, not an
  implementation.

**TESTING LESSONS, and the first two are the ones that matter most:**

- **A positive control that reddens NOTHING means the check is not
  load-bearing -- and that happened TWICE in one session, on checks I
  had just written.** The Control Panel tool's hidden-page check
  compared a widget's band against the WHOLE page's ink, a baseline so
  much larger that it passed either way; the fix was to measure the SAME
  RECT in both states. The table's sort check compared only the sort
  STATE, which a widget can record without applying. **Run the control
  on a NEW test, not just an old one, and read which checks stayed
  green.**
- **"It predates me" is a measurement, not a defence.** A deterministic
  failure was proved not-mine by checking out the previous commit,
  rebuilding and seeing it fail identically -- which took one build
  cycle and turned "I broke this" into a recorded known issue with a
  precise scope. `TOYOS_ALLOW_STALE_ISO=1` exists partly for this.
  Then, when the time came to fix it, **the fix was in the TEST**: it
  took `launched[0]` and assumed that was the process it had just
  spawned, true only while that list started empty -- which stopped
  being true when the app it opens first became a ring-3 binary. The
  scheduler and the reaping were correct the whole time. Measure which
  pid is which before theorising about the mechanism.
- **`tools/iso_guard.py` now refuses to boot a stale ISO**, from both
  `vm.py` and `qmp_test.py`'s `launch_qemu_cmd()`. This is the trap
  every session hit: `make all` without `make iso`, or a `make iso` that
  FAILED, leaves the suite testing the previous build and reporting a
  clean PASS. It caught a real failed build within minutes of existing.
  Its own first version cried wolf on the first userland-only edit
  (comparing every tree against `build/kernel.bin`) -- **a guard that
  false-alarms is a guard people switch off**, so each tree is paired
  with the artifact it actually feeds.
- **A test that opens a window changes what a later click hits.** Three
  sorting checks failed while the widget was perfect, because the test
  spawned an extra process "so there is something to reorder" and that
  window took focus, landed on top, and swallowed every header click.
- **`DebugConsole.spawn()` polls with `logs()`, which CLEARS what it
  returns** -- so an app's own startup lines are consumed before the
  tool can read them. Send `gui spawn` directly and drain the log
  yourself when you need them.
- **A `gui spawn`ed program's STDOUT is invisible to the test.** It goes
  to its parent's pipe, not the kernel log; only stderr is readable from
  outside. Verifying a change by spawning `/bin/config get` does not
  work -- read the file with `sh cat` instead, which is a stronger
  independent path anyway.
- **Ask the app for geometry, including a COLUMN's rect.** The sorting
  test needed to click a header; deriving the column x from the
  character widths in `COLUMNS[]` is exactly the re-derivation that has
  drifted in four tools here. Task Manager reports each column's rect
  now, and its sort state and row order ON CHANGE rather than once at
  startup -- a state logged only at open cannot show whether a click did
  anything.
- **LOOK at what you drew, again.** The sort arrow was reported by the
  user, not by the suite: it was drawn at the column's right edge and a
  RIGHT-aligned title is positioned FROM that same edge, so it landed on
  the last character. Reserving clip width was not enough -- the edge
  the text is measured from has to move too. Left-aligned columns were
  always fine, which is why it looked like a clipping bug.

**A load-count check is not a draw check.** The cursor theme system
shipped its first working version loading 0 of 6 shapes and looking
perfect, because the built-in fallback drew a fine pointer. Its positive
control makes the split explicit: disabling the theme reddens the two
DRAWING checks and leaves both "loads every shape" checks green. If a
feature has a fallback, no check may assert on the fallback's output.

**A test must ESTABLISH the state it measures against.** `cursor_size`
persists to `/etc/toyos.conf` on the disk image, which a build does not
re-seed -- so an earlier run's `huge` made a later baseline 9x too large
and a ratio check read 0.44 instead of 4.00. Set what you measure
against, and clean up after yourself so the next tool starts where it
expects to.

**Two tools came out of this. Reach for them:**

- `python3 tools/kvm_soak.py [-n N]` -- boots the desktop under KVM
  across FRESH BOOTS and fails on the symptoms that only appear there:
  slow WM frames, a file that exists but will not read, an incomplete
  cursor-theme load, a varying desktop entry count. Fresh boots because
  the bug it was written for was intermittent at ~1 in 3; a single clean
  run says nothing. SKIPS loudly without `/dev/kvm`.
- `gui watchdog [<ms>|off]` on the debug console -- the threshold plus
  how often it has fired and the slowest frame seen. Report the counters
  as well as the threshold: "no SLOW FRAME lines" is only evidence of a
  fast WM if the watchdog was actually armed.

**A tool that cannot go red must not be committed, and this one nearly
was.** `kvm_soak.py` passed cleanly with the VFS preemption guard
disarmed entirely -- because the OTHER two fixes had independently
removed the disk pressure its workload depended on. Two consequences
worth carrying: forcing real work matters (it now churns
`/usr/wm/desktop` so the desktop genuinely re-reads every entry, since
the reload is skipped when the directory is unchanged and a cached read
never reaches the drive), and **when reintroducing one bug does not
reproduce the symptom, the right control is the ORIGINAL TREE** -- build
the pre-fix commit in a scratch worktree and run the tool against that.

## 2026-08-19: making the GUI suite faster, and why three of the four levers did nothing

**ON A FAN-OUT, THE WALL CLOCK IS THE MAXIMUM, NOT THE SUM. Look at the
slowest single item before touching parallelism.** `gui_regress.py` runs
24 tools totalling ~425 tool-seconds. Three levers were tried and
measured:

| change | wall |
|---|---|
| `-j4` (as it was) | 76s |
| `-j8` on a 16-core box | 72s |
| `-j8` + KVM (`--kvm`) | 64s |
| cutting the slowest tool's floor | **61s** |

Raising `-j` bought 4 seconds and KVM another 8, because the suite was
never CPU-bound: its slow tools sit WAITING on real timeouts, and no
amount of guest speed shortens a wall-clock wait. `forcequit_test.py`
alone waits out the WM's 3-second not-responding timeout about ten
times, which made it a 72s floor under every configuration. Turning that
timeout down for its run (a `gui pingtimeout` debug command, the same
shape as the existing `gui watchdog`) took the tool to 34s and the suite
to 61s.

Two things generalise. **Measure the baseline before believing your own
impression of it** -- this session opened by asserting the suite took
"about seven minutes", which was simply wrong; it was 76s, and the
seven minutes were the session's own polling. And **a constant that only
a test waits on is a legitimate test lever**: the timeout's VALUE was not
what `forcequit_test.py` asserts, so shortening it lost no coverage. Say
so in the code, and have the tool ASSERT the knob took -- otherwise every
wait silently becomes shorter than the thing it waits for, and the tool
fails looking like the feature is broken.

## Settled is not the same as UPDATED

`QMPSession.stable_pixels()` returns two identical consecutive reads. It
does not, and cannot, know whether the app has repainted YET -- two reads
of a stale frame are identical too. Under parallel load
`notepad_client_test.py`'s "New clears the editor" sampled the frame from
BEFORE Ctrl-N was handled and reported that New had cleared nothing.

The fix is not a longer settle. **Wait for something the app SAYS**, then
capture: Notepad's title returns to `untitled` when New clears the path,
so poll for that. Same shape fixed `gfxdemo_test.py`, whose failing check
was a LOG assertion read once after `dbg.settle()` -- settle knows the
console went quiet, not that this client has handled the click and
logged. Both tools now poll for the observable and both stopped flaking.

## 2026-08-21: the blind "gui" sleep, and converting the last fixed waits

**THE DESKTOP IS ALREADY UP AT BOOT, so every tool's `send "gui" +
sleep(2-3)` waited out something already finished.** init starts the
desktop under the graphical target, so by the time a tool connects the
desktop is running and the "gui" is refused. `gui state` returning a real
screen is an observable readiness signal (the WM only answers it while it
holds the compositor role), so `enter_gui()` in `gui_debug.py` polls that
and returns in one round trip. ~3s off every one of ~25 tools.

**Converting a fixed sleep is per-sleep judgment, in three buckets, and
the third one CANNOT be converted.** (1) The assert reads a debug-console
fact (a layout, a log line) -> poll that fact until it holds, with a
deadline; the assert still runs on timeout and reports the wrong value.
(2) The assert reads PIXELS -> `stable_pixels()` (a client draw plus a
compositor hop that `settle()` does not cover). (3) The assert is
NEGATIVE -- "commits nothing", "menu did not open" -- and **you cannot
poll for the absence of an event**; keep a bounded wait. Best is to key
even a negative check off a POSITIVE signal: taskmgr logs `armed pid N`,
so wait for the arm, THEN assert nothing was killed.

**A log that ACCUMULATES can still be polled if the reader cuts to the
current frame.** menubar_test's `Layout` parses only from the last
`notepad: layout scrollbar` line (emitted first every draw), so
`popups()` reflects live menu state even though every popup line ever
logged is still in the buffer -- which is what makes polling `popups() ==
[]` after a dismiss correct rather than permanently stuck. Verify the
reader's framing before assuming a poll converges.

**A tool re-run on the SAME VM lies; the suite gives each tool a FRESH
one.** taskmgr_test passed 20/20 on a fresh boot and failed at check 3 on
an immediate re-run -- a leftover Task Manager window from the first run.
`gui_regress.py` copies disk.img and boots a fresh VM per tool, so the
fresh-boot run is the representative one; restart between manual re-runs
or you will chase a state artifact.

**`cores//2` counts hardware THREADS, and going past it oversubscribes.**
The `DEFAULT_JOBS` cap is a ceiling against oversubscription, not a
target: one busy TCG guest per physical core is the sweet spot, and more
turns wall-clock into settle flakes. Raising the cap only helps a host
with threads to spare.

**A NEIGHBOURING app's cadence can flake an unrelated tool.** Font Demo
loaded its fonts off a `tick_ms` timer (the event loop blocks between
frames, so a `uapp_redraw()` from `on_draw` will not repaint until an
event). At 60ms the idle wakeups, while the app sat open beside
font_test's timing-sensitive desktop-font-switch checks, added enough
guest-CPU churn to flake them. 250ms fixed it. When a change adds a
cadence, ask what else is running in the same guest.

**Reading the failure text matters before theorising**: gfxdemo's failing
check was `check_log(...)`, not a pixel comparison, so "add
`stable_pixels`" would have been the wrong fix confidently applied.

## A guard that fires on a transient is worse than no guard

Everything here defaults to QMP port 4445, and two guests sharing it do
not fail as a port clash -- they fail minutes later as a
`BrokenPipeError` in whichever tool was mid-command, accusing the
innocent. `tools/port_guard.py` refuses at launch instead, from the two
chokepoints `iso_guard.py` already guards.

Its first version refused OUTRIGHT, and immediately turned a clean pair
of back-to-back suite runs into eighteen "Connection refused" failures:
a guest that has just been told to stop still holds its socket for a
moment, so the guard fired on a shutdown. It waits briefly now -- a real
clash lasts, a shutdown does not.

And the second half of that bug was worse than the first: `gui_regress.py`
IGNORED `vm.py`'s exit code, so a refused launch was invisible and
surfaced only as the tool failing to connect. **A launcher's exit code is
not optional.** Check it and report why, or every launch failure gets
attributed to whatever ran next.

## Do not add a wait loop for work that is already in the background

`until ! pgrep -f gui_regress; do sleep 10; done` beside a backgrounded
suite is both redundant (the completion notification is the signal) and
BROKEN: the waiting shell's own command line contains the pattern, so
`pgrep -f` matches the waiter and the loop never exits. Four accumulated
in one session, each reporting a finished suite as still running -- which
is worse than the leak, since a false "still going" is indistinguishable
from the real thing. Wait on the ARTIFACT (`until [ -s out.log ]`), which
cannot match itself.

## A dirty disk.img makes a flake comparison measure nothing

Chasing whether a `win_server` KTEST failure was this session's, the
comparison against `HEAD` came back WORSE -- 4 runs of 4 failing. The
failing checks were filesystem ones, i.e. the documented dirty-fixture
hazard: ~15 ktest runs had accumulated state on `disk.img` without a
`make clean-disk`. The comparison measured the fixture, not the code.
**`make clean-disk && make iso` before any flake rate you intend to
believe**, on BOTH sides of the comparison.

## A STALE COPY OF disk.img MADE A POSITIVE CONTROL LIE (2026-08-19)

The control looked like it disproved the feature. `ls` reports a
truncated listing when it fills its array; to prove that check could
fire, the cap was lowered to 32 against a 40-file directory. It listed
32 and printed NO message -- exactly the failure the change was supposed
to have fixed.

Nothing was wrong. The VM was booting a COPY of `disk.img` taken before
the rebuild, so the guest ran the OLD `/bin/ls` -- built when the cap was
256 -- against the NEW kernel, which clamped to 32. The binary compared
`count` against a maximum it had never asked for, so its own detection
could not fire. Re-running against a freshly seeded `disk.img` produced
the message immediately.

`CLAUDE.md` already says a copy goes stale the moment you rebuild. The
addition worth carrying is the SHAPE: **a stale copy does not only break
a feature, it invalidates a CONTROL** -- and a control that fails is read
as "the code is broken", which is the most expensive possible way to be
wrong about a fixture. Before believing a control, ask whether the ISO
*and* the disk under test both contain the change.

The general form, since this is the third variant of it recorded here:
when a positive control produces a surprising answer, suspect the
plumbing between the change and the machine before suspecting the change.

## The output you captured may be from a surface the output never reached

Two screenshots were taken to check whether `ls` colour appeared on the
console. Both showed only grey, which read as "the ANSI parser is not
working". The parser was fine: `vm.py exec` runs commands over the serial
debug console, whose output is redirected into a SINK for the duration --
so that output never went to the screen at all, and the screendump showed
what had been there beforehand.

**Match the evidence to the surface.** Three distinct surfaces exist here
and a given command reaches exactly one: the debug console sink
(`vm.py exec`, and what every GUI tool reads), the physical framebuffer
console (what a screendump shows, and only what was TYPED reaches it),
and a ring-3 client's own window. Asking the wrong one produces confident
negative evidence.

Colour has a second twist worth knowing: it can never appear over
`vm.py exec` by construction, because the console turns `ESC[...m` into a
colour before the sink sees a byte. What text CAN prove is the negative
-- an unwired parser shows up as literal `[1;36m` in the captured output.
The colour itself needs pixels from a typed-at console
(`ShellFlow.run_command()`, `target=text`, `kbd=us`).

## Changing a program's OUTPUT FORMAT is an API change

`/bin/ls` was given multi-column output as its default, which is what
`ls` looks like at a terminal. It broke `notepad_client_test.py`, whose
premise is that `sh ls /` can be read one name per line to derive which
row to click in a file dialog.

Real `ls` avoids this with `isatty()`: columns to a terminal, one per
line to a pipe. toy-os has no `isatty()`, so there is no way to make that
choice correctly -- which means **the default has to be the parseable
shape**, and columns became a flag.

Two things generalise. A program's output format is consumed by
something, and here that something is the test suite -- so "make the
output nicer" is a change with downstream callers, exactly like changing
a function signature. And when a capability real systems rely on is
MISSING (isatty), the honest move is to pick the conservative default and
record the gap, not to pick the pretty default and hope.

## Sorting in two places means the two orders are a contract

`/bin/ls` was made to sort by name -- a genuine improvement, since
`SYS_LISTDIR` returns whatever order the filesystem walked and the same
directory could list differently twice. It broke a check that derives a
row index from `ls` output and clicks that row in Notepad's file dialog,
because the dialog still listed unsorted.

The fix was to share one `dirsort()` between them rather than to sort the
dialog separately, so the two orders cannot drift again. **When a test
derives an index in one program and applies it in another, the ordering
is part of the interface** -- and the cheapest way to keep an interface
true is to make it one piece of code rather than two that agree today.

## 2026-08-19 (virtio-blk): the fixture decides what your test measures

**START FROM A CLEAN DISK BEFORE COMPARING TWO CONFIGURATIONS, or you
will compare neither.** Benchmarking ATA against virtio-blk, the virtio
run failed 7 filesystem tests and it looked like a real driver bug. It
was not: an earlier failing run had leaked blocks into `disk.img`, and
`r.leaked` from `fsck` is the documented dirty-fixture symptom. The
proof was running the ATA config -- the supposed control -- and
watching it fail identically. `make clean-disk && make iso` between
runs, and treat "the control also fails" as the fastest way to find out
your fixture is the variable.

**AND RUN EACH CONFIGURATION MORE THAN ONCE.** One clean ATA run passed,
the next failed, the third passed. A single run of each would have said
either "ATA is fine" or "virtio broke ATA" depending purely on which
one you happened to take. Three runs each gave the answer that actually
mattered: ATA 2 in 3, virtio 3 in 3.

**A TEST TIED TO ONE IMPLEMENTATION SILENTLY STOPS TESTING WHEN THE
IMPLEMENTATION CHANGES.** Four filesystem error-path KTESTs armed
`fault_fail_next_ata_writes()` and asserted the write failed. Mount the
same filesystem on virtio-blk and the injector controls nothing: the
writes succeed, and the tests fail -- which at least is loud. The
quieter version of this is a test that keeps PASSING for the wrong
reason. The fix was moving injection to the block layer, where every
backend passes through; the ATA pair stayed, because ATA's write-back
cache lives below that layer and cannot be reached from it.

Ask of any test using a fault injector, a fixture, or a device: **what
happens to this when the thing underneath is swapped?**

## Host tools: use the LSP and the linters (2026-08-19)

The machine gained `bear`, `ccache`, `ruff` and `shellcheck` this
session, and `clangd`/`clang-tidy`/`scan-build` were already there but
unusable. `docs/tools.md`'s "Host tools this repo expects" is the full
reference; what a SESSION needs to know:

- **`bear -- make all` (from a `make clean`) regenerates
  `compile_commands.json`, which is what makes `clangd` work here.**
  Reach for the LSP instead of grepping for a signature. The day this
  was set up, writing one new widget against `userland/ui/`'s 24
  existing ones produced FIVE wrong guesses in a single file --
  `uui_scrollbar_draw`'s arity, three `uui_widget_ops` function-pointer
  types, and `KEY_UP` where this kernel spells it `KEY_ARROW_UP` -- and
  cost a build cycle to find out. A toolkit with 24 widgets is exactly
  the shape where guessing a signature feels safe and is not.
- **Run `ruff check tools/` and `shellcheck tools/*.sh` after touching
  a harness**, before believing its result. Neither is in
  `preflight.sh` and neither should be. `ruff.toml` pins `F` + `E9`
  deliberately: the default ruleset reports ~320 findings here, all
  style, which buries the one class that matters -- a harness bug that
  reports a healthy system as broken. On its first run the narrow set
  found 8 real (benign) findings and NO undefined names, and
  shellcheck found one genuine bug: an unguarded `cd` in
  `preflight.sh` that would have run the whole gate, `make clean`
  included, in the caller's directory.

**And the general lesson, which is not about the tools:** the checks
worth running are the ones aimed at the failure mode you actually have.
This project's is not "the Python is ugly", it is "the harness lied".
Selecting for that is why the linter is usable at all.

## A measured rect must contain ONLY the thing under test (2026-08-19)

`settings_test.py`'s scroll check measured a rect spanning the whole
window content and parked the cursor at `content_width // 3` -- which,
with the new sidebar, is INSIDE the sidebar. So the wheel scrolled the
sidebar, the page never moved, and **the check passed anyway**, because
the sidebar's own scrolling changed pixels inside the measured rect.
The failure surfaced two checks later as "a choice only reachable by
scrolling can be applied: utc -> utc", which points nowhere near the
cause.

Three things to carry:

- **A region that includes something other than the thing under test
  can be satisfied by that other thing.** Measure the widget, point at
  the widget.
- **A layout change silently invalidates every coordinate a tool
  computed.** This tool's other three failures that day were all the
  same shape: geometry captured at OPEN, used after a RESIZE; a click
  at "just past the sidebar" landing in the layout's gap; and a click
  at the page's centre missing a radio list, because a radio list
  RECOMPUTES its width rather than filling what the layout offers
  (`uui_radio_list.h` says so).
- **Make the app report its own geometry, and assert the intermediate
  step.** The fix that made all of it tractable was having the app log
  one line per sidebar row -- id, click y, depth, label -- so the tool
  looks a row up BY LABEL instead of deriving it from a font size.
  Same reasoning as `DebugConsole.menu_row()`, and the same reason
  `gui_flow.py`'s calibrated constants have needed re-measuring three
  times. Adding "the timezone page opened" as its own check is what
  turned a mystery into a coordinate bug in one run.

## Assert on a logged FACT, never on what the screen says (2026-08-19)

`settings_test.py` checked that selecting a choice staged rather than
applied it by looking for the status bar's wording ("not applied yet")
in the debug log. The status bar is PIXELS -- that string never reaches
the log -- so the check failed while the app was working perfectly, and
the neighbouring checks that DID assert on disk contents passed.

The app logs `settings: staged <name> <value>` now and the test asserts
on that. The general rule this repo already has ("prefer
tools/gui_debug.py to pixels") has a corollary: **if you find yourself
grepping a log for text the app draws rather than logs, you are
asserting on something that is not there.**

Two more from the same tool, both worth recognising:

- **Accumulated logs need a MARK.** `drain()` appends to one list, so
  anything read without a starting index sees every page ever opened.
  The first version asked about the Mouse page and got the timezone
  page's control back -- a confident wrong answer. Take
  `mark = len(drain(dbg))` before the action and read `_log[mark:]`.
- **A tool that reads a log should PRINT it on failure.** This one
  drained the app's lines to make its assertions and printed none of
  them, so a failing check reported a coordinate and nothing about what
  the app thought it drew -- and the saved log file contained no trace
  of the app at all. Same rule as naming the phase in a truncated ktest
  summary.

## A check that passes on absent data is measuring nothing (2026-08-19)

"selecting a choice does NOT write it yet" compared `/etc` before and
after and passed -- with both readings `None`, because the key had never
been written. `None -> None` satisfies "unchanged" perfectly.

Establish the baseline rather than inherit it: the test now does
`sh config set system.mouse_speed normal` first and ASSERTS the value is
on disk before going near the staging checks. Same family as this repo's
existing rule about a fixture whose data never reaches the branch under
test.

## I bisected my own change for four cycles before running `clean-disk` (2026-08-20)

The runtime-font session ended with `gui_regress` reporting two
failures, `settings_test` (4 checks) and `screen_surface_test` (1).
Both had been green earlier the same session. So I went looking for
which of my changes did it, with genuinely well-chosen discriminating
experiments -- disable the newly registered setting, rebuild, run;
disable the new client-side event handler, rebuild, run; make the
kernel-side broadcast a no-op, rebuild, run. Four build-and-boot cycles.
Every one of them still failed, and the third one changed the failure
COUNT (34 passed, then 36), which I noted and did not act on.

Then `make clean-disk && make iso`: **38/38 and 14/14**. The failures
were the disk image. `settings_test` and several others WRITE to `/etc`,
`make iso` re-seeds by sync and never reformats, and I had run
`ktest_run` and a full suite over the same image in between.

CLAUDE.md already says this, in capitals, as the first thing to do
before believing any GUI test failure. I read it at the start of the
session and still bisected first, because the failure looked so much
like a regression in the specific thing I had just built.

Three things to take from it:

- **`make clean-disk && make iso` costs one cycle and comes FIRST.** Not
  after the cheap experiments -- before them. It is cheaper than any
  single bisect step and it eliminates the whole class.
- **A failure set that CHANGES between runs of the same binary is a
  fixture or a flake, never a deterministic regression.** 34 then 36
  passing was the tell, and it was sitting in front of me. If the code
  under test did not change and the answer did, stop bisecting the code.
- **A well-chosen experiment on the wrong hypothesis is still wasted.**
  All four experiments were discriminating and correctly interpreted;
  the hypothesis space simply never included the fixture. Ask what else
  changed besides the code -- the image, the config, the previous test.

## Pick a ruler whose signal is large, and crop what you measure (2026-08-20)

`font_test.py` had to show that a proportional face draws text NARROWER
than a monospace one. The obvious target was the desktop icon captions,
which gave 121px against 117px -- a 4-pixel difference that any
antialiasing change could produce. The reason is that captions are
CLIPPED to the icon cell, so a narrower font mostly just un-truncates
them rather than getting shorter.

The right target was the version text in the bottom-right corner: ~40
characters, drawn RIGHT-ALIGNED against the screen edge, so its leftmost
ink moves by the whole difference in string width. 1012 against 1063 --
51 pixels, and a measurement nobody has to squint at.

**Ask what the measurement's dynamic range IS before trusting it.** A
check comparing two numbers four apart is a check about noise.

And a mechanical trap in the same function: `QMPSession.stable_pixels()`
takes a `box`, but the box is what it COMPARES for stability -- the file
it writes is the whole screen. Scanning that file counts the taskbar and
every icon as ink. It read 53,968 "ink pixels" in a band with 20,300
pixels in it, which is the kind of number that is obviously wrong only
once you divide.

## A check that cannot fail is worse than no check (2026-08-20)

I wrote "an out-of-set character does not take the desktop down" to
document the 101-glyph ceiling, and it passed. It would also have passed
if the fallback drew a blank cell, drew garbage, or drew the wrong
letter -- the desktop survives all of those. It was a comment wearing a
check's clothes, and it would have been read as coverage.

It became a KTEST instead, where the fallback semantics are actually
reachable: `gfx_char_advance()` of a Euro sign, a CJK codepoint and an
unbaked Latin-1 letter must each equal the advance of `?`. That one can
fail, and it fails for the right reason.

**The test to apply is the repo's own: what would a broken version still
pass?** If the answer is "this check", delete it or move it somewhere it
can bite.

## "It measures" is not "it is drawn" (2026-08-21)

Fifteen checks passed while every letter in the app under test rendered
as a **hollow outline**. Every one of them read numbers out of the mapped
font atlas -- widths, advances, per-glyph ink counts -- and not one
looked at what reached the screen. The atlas was perfect; the app drew
onto a surface it had never cleared.

This is the repo's own "it responds is not it is drawn" arriving from a
new direction, and the new direction is worth naming because it feels
like stronger evidence than it is: **asserting on numbers the code
computed is not asserting on its output.** A measurement check and a
pixel check are different coverage, and a suite made only of the first
cannot see anything that goes wrong between computing and painting.

The user found it in a screenshot. Both rendering defects this session
were found that way and neither by the suite.

## A threshold picked without a control, again -- and it passed the broken build by 95,136 pixels (2026-08-21)

Writing the pixel check that should have caught the above, the obvious
probe was "is the text mostly SOLID ink rather than mostly edge pixels",
since an outline is nearly all edge. Measured on the broken build: the
black background counted as 95,136 "solid" pixels and it passed by a
wide margin.

What actually separates the two states is what MOST of the window is --
the panel background when the app cleared, near-black when it did not.
Dominant colour, not an ink ratio.

**The rule the repo already has, restated because I broke it twice in
one session: do not reason a threshold, measure it against a
deliberately broken build.** Both times the reasoning was plausible and
both times the number was somewhere else entirely.

## Two sabotages can mask each other (2026-08-21)

A positive control disabled the registry's range check AND the boot
reader's legacy-name table in one build. Nothing reddened on the check
that mattered: with the legacy names gone, `apply` rejected `"slow"`
anyway, so removing the registry's gate changed no outcome.

Re-run with ONE sabotage, the right assertion failed immediately.

**Sabotage one thing at a time**, for the same reason you change one
variable in any experiment -- and be suspicious of a control that
reddens nothing when you disabled two things, which is the case that
looks most like "the code is fine".

## Choose test DATA that the bug could actually change (2026-08-21)

The first kerning test asserted on the pairs every kerning demo uses --
`AV`, `To`. A build that SORTED the lookup key (an easy thing to write,
and invisible on symmetric pairs) passed it completely, because those
pairs happened to be in ascending glyph-id order already.

The fix was to find pairs that are ASYMMETRIC in the shipped font --
`(F,A)` kerns and `(A,F)` does not, with F's gid higher -- so a sorted
key answers the wrong one and both halves redden.

Same shape for emboldening: "more ink than before" and "not the whole
cell" both passed a smear that flooded every inked row to the cell's
edge, because the empty rows above and below stayed empty. What cannot
survive it is the RIGHTMOST INKED COLUMN moving by more than the smear
width.

**Ask what the data would have to look like for the bug to change the
answer**, then go and find that data in the real fixture. A plausible
example is not a discriminating one.

## A KTEST can be vacuous because of the tests that ran BEFORE it (2026-08-21)

A boot-time invariant -- "a font face that is selected must also be
built" -- looked like an obvious KTEST. It cannot be one: the font
KTESTs that run earlier in the same file build an atlas and restore it,
so by the time the assertion runs the invariant already holds and the
test passes on the broken build.

It moved to the Python tool and runs FIRST, before anything sets a face
or a size, because every later check REPAIRS the state it is looking
for.

**Two questions for any test of a boot-time or first-use state:** what
ran before it in this process, and does anything the test itself does
establish the thing it is checking? KTESTs run in the live kernel in
link order, which makes the first question much less obvious than it
sounds.

---

## AN IDENTITY IS ONLY A TEST WHERE IT IS WELL CONDITIONED

From the libm work (2026-08-21), and the most transferable thing in it.

`cosh(x)^2 - sinh(x)^2 == 1` is true for every real x, so it looks like
the perfect check for two functions at once: it holds everywhere, needs
no table of expected values, and cannot be satisfied by accident.

It reported errors up to **3e-8** against a `cosh` and `sinh` that were
individually correct to 1e-15.

The reason is arithmetic, not implementation: for large x both functions
are about `e^x/2`, so the two squares are enormous and nearly equal, and
subtracting them cancels almost every significant digit. The check was
measuring the SUBTRACTION. Rewriting it as `(cosh-sinh)(cosh+sinh)`
cancels just as badly, for the same reason -- which took a second round
to see.

The well-conditioned form is **`cosh + sinh == exp`**: it adds instead
of subtracting, so nothing cancels, and it pins both functions against a
THIRD one rather than only against each other.

**The general rule: before using an identity as a test, ask what it does
to the error.** An identity that subtracts two nearly-equal large
quantities tests floating point, not your code. Keeping the ill-
conditioned form for SMALL arguments only, where the quantities are not
yet nearly equal, is a legitimate way to have both.

## EXPECTED VALUES MUST COME FROM AN INDEPENDENT IMPLEMENTATION

Also from the libm and calendar work, and it is the reason both passed
first try in a way that meant something.

Every expected value in `libm_test.c` and `libc5_test.c` was generated
by the HOST's Python -- `math.sin`, `datetime` -- and pasted in. None
was obtained by running toy-os and writing down what it printed.

That distinction is invisible when the code is right and total when it
is wrong: a test whose expectations came from the code under test agrees
with the code under test, including everywhere both are wrong. A
calendar test written that way will happily certify a calendar that
thinks 1900 was a leap year.

**This is cheap.** A dozen lines of Python generating C source, run
once, pasted in. It costs less than deriving the values by hand and is
the difference between "the implementation is self-consistent" and "the
implementation is correct".

## THE CONTROL THAT FIRES NOTHING: THREE MORE SHAPES

This repo already has the rule -- a green control means the fixture
never reached the branch. Three fresh instances, because each failed
differently and the third is one nobody would predict:

- **The input was too simple to discriminate.** Breaking `strtol`'s
  "no conversion returns the ORIGINAL pointer" changed nothing, because
  the test fed it `"zz"` -- for which the original and post-sign
  pointers are the same address. `"-zz"` and `"0xzz"` reach the branch,
  because the sign and the prefix have already moved the cursor.
- **Something else restored the property.** Deleting `longjmp`'s `rsp`
  restore changed nothing, because `longjmp` also restores `rbp` -- so
  the caller's frame stayed addressable and a stale (lower) `rsp` merely
  left dead space below. Only reading `rsp` directly across the jump
  can see it.
- **THE BRANCH WAS IN A DIFFERENT FUNCTION THAN ITS NAME SUGGESTS.**
  Replacing `cal_is_leap()` with the naive every-four-years rule
  reddened NOTHING in the calendar tests -- because Hinnant's
  `days_from_civil` encodes the 100/400 rules inside its own era
  arithmetic and never calls `cal_is_leap` at all. The control fired in
  a KERNEL ktest instead, since that function's only callers are the DST
  rules. Dropping the `- yoe/100` term is what reddens the calendar.

**The lesson from the third: when a control fires nothing, do not only
ask whether the fixture reached the code -- ask whether the code you
broke is the code that implements the behaviour.** A well-named function
can be entirely bypassed by the path under test.

## A MEASUREMENT QUANTISED TO THE CLOCK IS NOT A MEASUREMENT

The cJSON benchmark, first version: every timing came back an exact
multiple of 10000 us. That is the 100 Hz tick showing through -- each
phase finished in about four ticks, so every figure carried less than
one significant digit while looking precise to five.

Then the opposite, on the same tool: under KVM a phase finished BETWEEN
ticks, and dividing by a floor of 1 us produced "657800000 KB/s" for
something that had simply not been measured at all.

**Two habits.** Size the work against the CLOCK's resolution, not
against taste -- a few hundred ms against a 10 ms tick. And make a
result below the resolution SAY SO ("too fast to measure at N rounds")
rather than dividing by a floor: an absurd number is worse than an
absent one, because somebody will quote it.

Round numbers are the tell. If every figure ends in the same zeros, the
tool is reporting the timer.

## ASSERT THE DOCUMENTED BEHAVIOUR, NOT THE ONE YOU REMEMBER

`stdio_test` asserted that a second `ungetc` is refused -- true, and
deliberately tested, while the pushback was one byte deep. `scanf`
needed more, `FILE` grew an eight-deep stack (C guarantees one and
permits more), and that assertion started failing.

The test was not wrong when written and was not wrong to exist. What it
needed was updating to the NEW documented depth, with the reason
recorded -- and a check that the ninth pushback is still refused, so the
new limit is tested rather than merely assumed.

**A failing test after a deliberate change is a question, not a
verdict**: did the contract change, or did the code break? Answer it in
the test's comment, so the next session does not have to re-derive which
it was.

## 2026-08-22 -- signals, Ctrl-C, and four harness defects that outnumbered the kernel bugs

The day's theme repeated the 2026-08-18 one: **more time went into
tools lying than into the OS.** Four of the five things that looked
like kernel bugs were the harness, and each has a transferable shape.

**A FILTER THAT DROPS "NOISE" WILL DROP THE ANSWER.** A helper skipped
kernel log lines by matching `^[a-z_]+: ` -- and `cat: needs a filename`
matches it. So a file containing exactly the diagnosis was reported as
EMPTY, and I published a "pipe with redirect is broken" diagnosis that
was entirely wrong. Pipes and redirects were fine. Before believing a
harness that reports NOTHING, print what it actually received.

**A PROBE'S PREMISE GOES STALE WITH THE THING IT PROBES.**
`console_shell_test.py` typed `touch /kprobe.txt` to prove the kernel
shell had stood down, on the premise that `touch` was a kernel builtin
NOT on tosh's PATH. `touch` became a `/bin` program; tosh found it, ran
it, and the check failed while the property it names was perfectly
intact -- and left the probe file behind to fail a LATER check too. A
probe must name something the other side genuinely cannot reach; here
that is `rescue <cmd>`, which no `/bin` can shadow by construction.

**SAMPLING A STATE THAT IS BEING REACHED IS A FLAKE.** "An idle tosh is
BLOCKED" sampled the process table once, right after boot, and caught a
shell that had not got to its blocking read yet -- 2 runs in 4. Polling
with a bound does not weaken it: a shell that spins never reaches
BLOCKED, so the bound is what fails. Same rule as
`QMPSession.stable_pixels()`, arriving from a different direction.

**AND THE ORACLE MATTERS MORE THAN THE ASSERTION.** Comparing two
screenshots of a typed line to prove two keyboard drivers agree failed
three times for three reasons that were all the harness: the two boots
log different things above the prompt (the crop reached into it), the
caret blinks, and `_` DRAWS NOTHING on the ring-0 console -- so the
screen literally cannot distinguish a lost keystroke from an invisible
glyph, which was the distinction under test. Switching the oracle to the
FILESYSTEM (`touch /kb_probe.txt`; `echo x | touch /kb_pipe.txt`) made
it exact, layout-independent and one line long. Ask what the oracle can
physically see before designing the assertion.

**A DISCRIMINATING PROBE BEATS AN OBVIOUS ONE.** For "did `|` arrive",
`echo hi | cat > f` looks natural and is useless: the same keystrokes
WITHOUT the pipe still create the file. `echo x | touch /kb_pipe.txt`
creates it only when the pipe is there. Ask what a broken version would
still pass -- this repo's oldest testing rule, and it applies to the
probe's SHAPE, not just to the assertion.

**HOST TOOLS HINT AND THE KERNEL DOES NOT.** Chasing an invisible
glyph, PIL reported the underscore as a crisp full-coverage row; the
kernel's own unhinted rasteriser put it two rows lower at a quarter
coverage. The host measurement was confidently wrong in both position
and weight. When the question is "what does THIS renderer produce",
measure it in the guest -- a KTEST dumping the real atlas settled in one
run what two host measurements had got wrong.

**MEASURE THE BLAST RADIUS BEFORE CHANGING A RASTERISER (or anything
else that runs over every element).** Before touching glyph rendering, a
KTEST counted how many of the 101 glyphs the proposed rule would touch.
The answer -- 1, and it was also the only glyph below half coverage --
turned a scary-sounding change into an obviously proportionate one, and
the count went into the code comment so the next reader does not have to
re-derive it.

**AND THE POSITIVE CONTROL FOUND A FIXTURE BUG, not a code bug.**
Disabling the EINTR wake left a check GREEN: the child had never been
scheduled, so the signal was delivered at its first syscall -- a real
and correct path, but not the one the check names. The test now WAITS
until the child is really `SCHED_BLOCKED` and asserts that it got there.
"When a control fires nothing, ask what OTHER path could satisfy the
same assertion" already existed here; this is the same rule catching a
fixture that never reached the state under test.

## 2026-08-22 -- testing a terminal, and three tests that measured nothing

**A TEST THAT ASSERTS A SIDE EFFECT DOES NOT TEST THE RENDERING.**
`/bin/edit` in a Terminal window: type, F2, F3, then read the file back
through a different path. That check passes whether the escape sequences
were OBEYED or PRINTED AS TEXT -- the editor writes the file either way.
Two assertions beside it are what make it mean something: the CARET must
be near cell 5 (an editor whose `ESC[1;6H` was printed would have one
hundreds of cells along), and the status bar must really be reverse
video. The general form is this file's oldest rule -- ask what a broken
version would still pass -- and it is easiest to forget when the feature
under test genuinely works.

**AND THE PIXEL CHECK FOR THAT WAS WRONG THE OTHER WAY.** Counting
pixels of the reverse-video colour does not distinguish a status bar
from text: glyphs are drawn in the same grey, so a frame with NO BAR AT
ALL scored 4304. What only a filled background produces is a long
unbroken horizontal RUN -- a glyph is a few pixels wide, a bar is
hundreds. **When a colour check does not discriminate, look for a
geometric property of the thing rather than more of its colour.**

**A PIXEL VALUE FOUND A BUG THAT LOOKED FINE IN THE SCREENSHOT**, which
is the repo's standing rule earning its place again: the status bar
rendered as dark letters on black instead of black on grey, because
`ugfx_draw_string()` blends the GLYPH's pixels against the background it
is handed and does not fill the cell. Legible, plausible, and not what
`ESC[7m` asks for. `im.getpixel()` on three rows settled it in seconds.

**TWO STALE PREMISES, AND THE SECOND HAD BEEN FIXED ELSEWHERE MONTHS
EARLIER.** `stdin_test.py` typed a bare `touch` to prove the kernel
shell was NOT reading the keyboard -- on the reasoning that `touch` was
a kernel builtin tosh could not find. `touch` became a `/bin` program;
tosh found it, ran it, and left the probe file behind, which reads as
"the kernel shell is still listening", i.e. exactly the failure the
check exists to catch, with nothing wrong. `console_shell_test.py` had
the identical defect fixed in `eaa7529`; this tool is run ON DEMAND, so
it kept the stale version. **A fix to one harness is worth grepping the
others for.**

**A PREMISE CAN ALSO GO STALE IN THE GOOD DIRECTION.** A bare `cat` in
the Terminal used to return at once, because the window had no terminal
and handed children a closed pipe; with a real pty it WAITS for input,
like everywhere else. The check that asserted "returns instead of
hanging" was right when written and is now testing the wrong behaviour.
It types Ctrl-D, as a person would. **When a test fails after a change
that made something more correct, check whether the test encoded the
workaround.**

**AND ONE FLAKE MEASURED HONESTLY RATHER THAN BLAMED.** A fault-
injection KTEST failed 2 runs in 10 on the new tree against 0 in 4 on
the previous commit -- which at those counts does not distinguish the
two (0 of 4 is what a 20% rate looks like 41% of the time). The
plausible mechanism, that the injector arms the NEXT kmalloc and a
concurrent kernel allocation eats it, is UNPROVEN and says so in
`docs/bugs.md`. Resist the pull to either claim it pre-existing or
own it; the number is the honest answer.
