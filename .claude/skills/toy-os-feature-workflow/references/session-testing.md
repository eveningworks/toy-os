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

- **DO NOT TOUCH THE TREE WHILE A SUITE IS RUNNING -- AND "TOUCH" MEANS
  EDIT, NOT ONLY REBUILD.** `iso_guard` refuses a stale image, and a
  tool that never launches reports as a FAIL with **no log file at
  all** -- which looks nothing like the guard and everything like a
  regression. It cost two full `gui_regress` runs in one session: the
  first from a `make all` mid-run, the second from plain source edits
  with no build at all. Eight tools "failed" the first time and
  twenty-two the second, while the change's real state was 38/40 and
  37/39. **The tell is a failing tool with an EMPTY log** -- check that
  before reading a single assertion, because the summary line looks
  exactly like a real failure. Docs-only edits are safe; anything inside
  a build's dependency graph is not. Start the suite, then work outside
  the repo, or wait.
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
- **A user-reported bug's SURVIVORS are the diagnosis.** A boot mode
  that ran canned commands printed "Unknown command: lscpu" while
  `about`, `df`, `fsck`, `ls` and `lspci` all worked. That pattern *was* the answer: every survivor
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
  `history_load()`/`shell_path_init()`, while a scripted boot mode and
  the serial debug console both reached `shell_dispatch()` without it.
  Now an
  idempotent `shell_session_init()` called from both. Same family as
  `vfs.c`'s `ensure_layout()`; the `static int done` guard is what makes
  the rule unconditional.
- **AN ON-DEMAND-ONLY TEST CANNOT NOTICE A REGRESSION.** The scripted
  demo tour had exactly one test, kept out of every suite by standing
  request because it booted its own ISO. When the desktop moved to
  ring 3 the tour's GUI half was left behind with zero callers, and
  nothing said so for weeks -- the removal on 2026-09-06 is what found
  it (`docs/decisions/build.md`). Its positive control is the reusable
  part: reverting the PATH fix reddened exactly one of six checks and
  left "booted / reached the desktop / opened windows" green, which is
  the proof those three were never evidence the tour worked.
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
`/usr/wm/applications` so the desktop genuinely re-reads every entry, since
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

**AND A COPY IS A BOOT MEDIUM NOW (2026-08-25).** The kernel is
installed onto `disk.img` itself, so a stale copy no longer means "the
new kernel against the old userland" -- it means an entire previous
build, kernel included, wearing the current one's name. That is harder
to spot, not easier, because nothing is mismatched. `iso_guard`'s
warning says which of the two you have; re-copy after every `make iso`,
same as before.


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

## 2026-08-22 -- job control, and a test that read the state the code writes

**THE HEADLINE, and it generalises to anything the kernel DECIDES: a
test that reads the same state the code under test writes cannot see the
bug.** Six KTESTs were written for a stopped process. Five asserted on
`scheduler_test_state()`, which reports the `stopped` flag. Deleting the
one line in `find_next_runnable()` that honours that flag -- so a
"stopped" process carries on running -- left all five GREEN, because the
flag is exactly what the bug does not touch.

The sixth spawns `/tests/spin_test`, suspends it, and asserts its
`cpu_ns` does not advance across twenty ticks. That one failed, on the
right assertion (`expected 100000000, got 200000000`). **The rule:
measure the CONSEQUENCE, not the bookkeeping the consequence is derived
from.** For the scheduler that is CPU time; for an allocator it would be
a footprint; for a cache it would be a hit that did not reach the disk.

It is the same shape as this file's older "the data never reached the
code under test", arriving from the other side: there the fixture missed
the branch, here the assertion missed the effect.

**AND THE SAME SESSION SHOWED THE CHEAP VERSION OF THE SAME CHECK.** The
GUI-side tool asks `ps` for the CPU column between two samples two
seconds apart. One number, no kernel instrumentation, and it
distinguishes "suspended" from "idle" -- which no state label can, since
both read as not-running.

**A POSITIVE CONTROL SHOULD LEAVE SOME CHECKS GREEN, and which ones is
information.** Deleting the SUSP branch in the line discipline reddened
six checks and left "the job is alive after Ctrl-Z" green -- correct,
because a job that never received the signal IS alive. Noticing that
told me the alive check was the weak one and the stopped check was
carrying the section. **Read which checks stayed green, not just that
something went red.**

**THREE CONTROLS, THREE DIFFERENT SETS.** Worth doing separately rather
than as one "break it and see": the picker's flag check, the discipline's
SUSP branch, and `fg`'s terminal handover each reddened a disjoint set,
which is what proves the checks are testing three things rather than one
thing three times. The handover control reddened exactly ONE check -- a
Ctrl-C sent to a job after `fg` -- and that check exists only because I
asked what a `fg` that forgot the terminal would still pass.

**A FAILING TEST FOUND A PRE-EXISTING BUG THAT NO CONTROL WOULD HAVE.**
`cat &` ate the first character of the next command. The cause was
`sys_do_read_console()` claiming the console on EVERY read while its own
comment said "on the FIRST read", so the foreground group was re-pointed
at whoever last called -- invisible for as long as one process was the
only reader. **A feature that adds a SECOND user of a resource is a test
of every assumption the first one was silently satisfying**, and this is
the third time this repo has learned that (the desktop and the keyboard;
init and the console; now a job and the terminal).

**WHEN A NEW CHECK FAILS, ASK WHETHER IT IS ASSERTING THE RIGHT THING
FIRST.** Two of the five failures in the first full run were mine
misdescribing correct behaviour: `bg` on a job that reads input stops it
again immediately (bash does the same), and a job's stop is reported one
prompt later than I assumed. Both looked like bugs and were assertions
written from a guess about the behaviour rather than from what the
behaviour should be. The fix in each case made the test say something
truer, and one of them became a check worth having -- "SIGTTIN is not a
one-shot".

**COUNT ZOMBIES.** Nothing else here did, and a `fg` that waited only for
the stage whose status it reports left every other stage of a resumed
pipeline unreaped forever. Every other check in the tool passed while it
happened. It costs one line against `ps` and it is the only thing that
can see a whole class of leak.

## The harness lied four ways in one afternoon (2026-08-24, the `kbd` tap)

Building `/bin/kbd` and `tools/kbd_test.py`. The feature was small; the
tooling around it was where all the time went, which is now the usual
shape here. Four of these cost a build-and-boot cycle each.

**`"ready" not in output` MATCHES "al-ready running".** `vm.py start`
answers a slot that is already taken with `vm: already running`, and
that string contains `ready` -- so the obvious readiness test passes on
the one output that means the opposite, and the tool then talks to a
serial socket that is not there. It surfaced as `FileNotFoundError` on
the SECOND boot, i.e. pointing at the second boot's feature rather than
at the readiness test. **Eight tools here carried it**; there is one
`vm.started_ok()` now. The general form: a substring test whose needle
is a substring of the failure message.

**A QMP port stays bound after the guest dies, so two boots cannot share
a slot.** The tool's own `QMPSession` leaves the connection in TIME_WAIT
on the SERVER side, so `4445+N` is unbindable for about a minute and
`port_guard` correctly refuses the next boot on it. Waiting it out adds
a minute per boot for nothing; take a fresh slot per boot
(`find_free_instance()`). Until that was understood it read as
"virtio-input does not boot".

**`ps` must be read for STATE, not for a NAME.** The kernel shell's
`spawn` does not reap, so every earlier run leaves a `kbd` zombie: `"kbd"
in ps_output` answers yes forever, and the check passed with live mode
wholly broken. The version that reads the newest matching row's STATE
column caught it immediately. Any "is it still running?" check written
against a name has this bug.

**`DebugConsole.send()` returns on a QUIET PERIOD, not on completion.**
So timing a command with it measures nothing, and a `0.0s` elapsed for a
command that should have taken ten seconds is the harness, not the
guest. Worse, output left in the socket by a previous command comes back
attached to the next one -- two of the "measurements" that sent me
looking for a bug were the previous command's tail. Assert on the
guest's own observable state (`ps`, a file) rather than on how long a
`send()` took.

And one that is not a lie but a hard constraint worth knowing:
**a SPAWNED process's stdout goes to the console framebuffer and never
reaches the serial socket; the legacy `run` loader's does.** So `sh
<prog>` returns text you can assert on and `sh spawn /bin/<prog>` does
not. That decides the shape of a test: anything printing a table gets
run through the legacy loader, and anything needing a scheduler slot
(i.e. anything that sleeps) gets checked through the process table
instead.

## 2026-08-24 -- a hover check, and six ways the fixture rather than the code was wrong

The feature was small (display names, hover, type-ahead in a dropdown).
Every hour lost went to the harness, and none of it to the OS.

**A HOVER STATE CANNOT BE TESTED WITH `gui move`.** Injected input
overrides the pointer for the ONE `wm_run()` iteration that consumes it;
the next one reads the mouse driver again and snaps back. So it is
exactly right for a click -- press and release are edges -- and useless
for a state that has to survive a capture. The first version of the
check reported a working hover as dead, with the cursor confirmed at the
right coordinates, because the frame was taken after the pointer left.
Use `DebugConsole.hover_frames()`.

**AND THE HELPER I NEEDED ALREADY EXISTED.** I wrote `place_cursor()`
into `gui_debug.py`, made it work, and found `warp_cursor()` fifteen
lines above it -- better documented, and already used by
`dialog_test.py`. `grep` for the CONCEPT (`cursor`, `warp`, `hover`)
before adding to a module, not for the name you were about to give it.
The two are one function now, with both callers on it.

**A FIXED SLEEP IS A RACE THAT HAS NOT BEEN LOST YET.**
`font_test.py` slept 1.2s after each font change. Once hover started
causing real repaints, the desktop got busier and the establishing
change's `wm: font changed` line began landing AFTER the drain that
followed it -- so the next `wait_log()` returned the PREVIOUS face's
cell and two genuinely different faces compared equal. 3 runs in 5.
The sharper half: a change still in flight is observed by the NEXT
call's wait, so `set_size(14)` then `set_face("builtin")` returned on
the size's line and measured the old face -- which reads exactly like
"switching to builtin did nothing". Both wait on a line that is NEW
since their own command now.

**A COUNTED WAIT IS THE FIX WHEN THE OBSERVABLE IS NOT UNIQUE.** "Wait
for a line matching X" is not enough when the previous step also emits
X. Count the matches before acting and wait for the count to RISE.

**AN APP CACHES A SETTING WHEN IT BUILDS A PAGE**, so a fixture set from
outside mid-run is not necessarily what the next page shows. The
timezone was set with `sh config set` between two clicks, the file
contained the new value within half a second, and the page still showed
the old one. Establish a fixture BEFORE the app starts.

**AND THE LAYOUT LOG IS DEDUPED PER FRAME**, so re-opening the page that
is already open logs nothing at all -- every reader gets `None`, which
reads as a broken feature rather than an absent line. Navigate AWAY and
back.

**SELECTION OUTRANKS HOVER, which makes row 0 the wrong row to hover.**
On a fresh image the current city was the first row, and hovering it
measured nothing -- correctly, since a selected row keeps its selection
colour. The same check passed on a used image where the selection was
elsewhere. A fixture has to guarantee the thing it measures is
measurable.

**A TEST THAT APPLIES A SETTING CHANGES THE MACHINE FOR EVERY LATER
TOOL.** `settings_test` leaves `mouse_speed` and `mouse_accel` on disk;
a faster accelerated pointer then made `warp_cursor` overshoot in
`dialog_test` and `menubar_test`, which failed as "hover does nothing".
Three tools looked broken by my change and were broken by my earlier
RUN of a test. `make clean-disk && make iso` before believing a GUI
failure is in CLAUDE.md; this is the mechanism that makes it worth the
minute.

**AND TWO SUITE RUNS AT ONCE DO NOT FAIL AS A PORT CLASH.** I started a
`gui_regress` while another was still going: slots start at 0 for every
run, so they fought over the same pidfiles and QMP ports, and it
surfaced minutes later as `BrokenPipeError` in whichever tool was
mid-command. Seven tools "failed", none of them at fault.
`gui_regress.py` takes an flock now and refuses the second run outright.

**A LOAD FAILURE IS A MEASUREMENT TOO.** Three tools failed once each
across four full-suite runs (`uapp`, `idle`, `uterm`) and passed 3/3 in
isolation. Two were weak waits I fixed (`uapp` polled until the window
existed while the next line needed its resizable HINT -- a poll weaker
than what follows it). Two I left, measured and said so in the commit,
because nothing they touch was in the change. "Failed under eight-guest
load, passes 3/3 alone" is a sentence worth writing; "flaky" is not.

**2026-08-25 (partitioning, TFS2's removal, regex). THE THEME IS THAT A
POSITIVE CONTROL CAN LIE IN THREE DIFFERENT WAYS, AND I HIT ALL THREE.**

- **THE CONTROL FIRED AND THE MECHANISM WAS ALREADY DONE.** To prove the
  boot-time partition scan mattered I disabled the `return` that stops
  the flat probe running after a successful partition mount. Nothing
  reddened -- because `try_partitions()` had ALREADY called
  `mount_backend()` before returning, so skipping the `return` changed
  nothing. Moving the control down to `blk_part_register()` (make the
  partition device never register) turned the guest into a machine with
  no `/bin` at all, which is unmistakable. **Ask what the control
  actually severs, not what line it sits on.**
- **THE CONTROL FIRED AND MY FIXTURE WAS SOMEWHERE THE CODE NEVER
  TOUCHES.** Proving that a TFS2 disk is refused rather than reformatted
  needed a marker that a reformat would destroy. I put it at byte 4096 --
  inside the 32 KiB TFS3 never writes. The log line said "formatting"
  and the marker survived, which reads as "the guard works" and is
  actually "the test proves nothing". Moved to block 8 (the superblock)
  it came back as `TFS3\x02`. This is CLAUDE.md's own "the data never
  reached the code under test", and knowing the rule did not stop me.
- **THE FIXTURE MOVED UNDER THE TEST.** `grep_test` proved the pipe with
  `dmesg | grep partition`. It passed, then failed minutes later with
  grep working perfectly: dmesg's ring is finite and the boot lines had
  aged out. **A test fixture must be something you control** --
  `/tests/sample.txt` (401 numbered lines) instead, where every expected
  count is a number the file actually has.

**AN ORACLE THAT SHARES NO CODE IS THE ONLY THING THAT CATCHES A WRONG
EXPECTATION.** `/tests/regex_test` asserts the engine matches spans I
wrote; both halves came from the same head in the same hour, so it
cannot catch me being wrong about POSIX. Running the SAME table against
glibc agreed on 71 of 74 and isolated exactly three deliberate
divergences -- and a fourth case where my expected span was simply
miscounted had already fallen out minutes earlier. `tools/uimg_hostcheck.py`
does this against libjpeg. **When implementing something with a
specification, find another implementation and disagree with it on
purpose**: every difference is then a bug or a documented decision, with
no third category.

**ON-DEMAND TOOLS ROT, AND NOTHING NOTICES.** `fs_switch_test.py` was
failing 10 checks against a healthy system and `init_test.py` 16 of 31,
both for an unknown period, both found by accident. The cause in both
was the same: a command moved from a kernel builtin to a `/bin` program
and the tool kept parsing the old output (`df` for "Filesystem:", which
only `rescue df` still prints). `tools/ondemand_sweep.py` exists now to
run the ~22 tools nothing else runs; `tools/check_tool_commands.py` is
the static half and found `kvm_soak.py` driving a `delete` that is `rm`
now. **When an on-demand tool fails, measure it against HEAD before
believing you broke it** -- `tools/predates.py` does that in one line.

**AND A COMMENT INSIDE A TOOL AGES LIKE ANY OTHER.** `fs_switch_test`
said "asserted with `stat`, a builtin" -- true when written, false once
`stat` moved to `/bin`, and nothing noticed because the tool was never
run. Same shape as the `Ctrl-L` comment in CLAUDE.md's shell
conventions: **a comment stating a fact about the rest of the system
outlives that fact silently.**

## 2026-08-25 -- FAT32, and the oracle that shares no code with you

**A SELF-TEST CANNOT CATCH AN EXPECTATION BEING WRONG**, because the
same person wrote the writer and the reader. `kernel/fs/fat32_test.c`
formats a RAM volume and drives the backend directly, which proves the
driver agrees with ITSELF -- and a shared misreading of the on-disk
format passes both halves happily.

So the oracle is the HOST: `tools/fat32_test.py` has the guest write
into the real ESP, then reads it back with **mtools** and audits the
volume with **fsck.fat**. Neither shares a line with the driver. This is
the same call `regex_hostcheck.py` (GLIBC) and `uimg_hostcheck.py`
(libjpeg) already made; storage now has one too.

**The three checks that discriminate**, each replacing one that a broken
driver would pass:

- "`ls /boot` lists something" passes on a driver that mangles every
  long name -> read GRUB's own `grub.cfg` and look for text only a
  correct chain walk produces.
- "a file written reads back" passes on a driver whose format is
  privately wrong, since it is reading its own bytes -> extract a
  **185 KiB BINARY** with mtools and compare it byte for byte against
  the build artifact.
- "the volume still works" passes on one leaking clusters or
  cross-linking chains, which nothing observable shows for a long time
  -> `fsck.fat` has the last word.

**A LONG NAME IS THE CHECK; A SHORT ONE CANNOT SEE THE BUG.** This
driver shipped one build with its LFN set written forwards instead of
in reverse, which every other FAT implementation reads backwards. Every
short-named file worked perfectly. The KTEST asserts a long name
specifically, and additionally that the name is NOT its `~1` alias --
so a lookup that silently fell back to the 8.3 entry fails it.

**AN ADDRESS-DERIVED PATTERN, NOT A CONSTANT FILL**, for the
multi-cluster test: a constant cannot detect two file offsets mapping to
one cluster, because both read back the constant and look perfect.
`/tests/memtest` made the same call years earlier for the same reason.

**A PERSISTENCE CHECK SHOULD DO THE SMALLEST THING THAT OBSERVES
PERSISTENCE.** The reboot check originally unmounted `/boot` and
remounted it read-write before reading -- two commands the check did not
need, and it failed about one run in four while every host-side check on
the same image passed. Reading off the auto-mounted read-only mount is
one command and has been green three runs in three.

## 2026-08-25 -- a whole sweep invalidated by running things beside it

`ondemand_sweep.py` reported 12 of 20 tools failing. Every one of those
failures was mine: I had `fat32_test`, `fs_switch_test`, `partition_test`
and `ktest_run` going in parallel with it. The symptoms are exactly the
ones CLAUDE.md names -- `BrokenPipeError`, `ConnectionResetError`,
"could not start the VM", "connection refused" -- and they surface in
whichever tool was mid-command, never in the one that caused them.

Re-run alone on a fresh `make clean-disk && make iso`: **14 pass, 6
fail**, and every one of the six then measured as pre-existing or a
flake. **The sweep is single-job on purpose** (nearly every tool takes
the shared VM slot or the physical console), so the only thing that can
contend with it is another session -- which means yours.

**Do not judge a suite you are running things beside.** Start it, then
stop touching QEMU until it finishes.

## 2026-08-25 -- "it predates me" measured five times, and the wrapper that matters

Five sweep failures, all measured with `tools/predates.py`:

| tool | verdict |
|---|---|
| `taskbar` | PRE-EXISTING -- HEAD fails identically |
| `init` | PRE-EXISTING (already in `docs/bugs.md`) |
| `virtio_gpu` | PRE-EXISTING |
| `ansi` | passes alone on BOTH sides -- contention flake |
| `virtio_input` | passes alone on BOTH sides -- contention flake |

**The trap: run the tool the way its runner runs it.** The first attempt
was `predates.py "python3 tools/taskbar_test.py"` and both sides failed
with `Connection refused`, because that tool ATTACHES to a QMP session
somebody else launched. The comparison measured nothing and looked
authoritative. `predates.py "python3 tools/ondemand_sweep.py --only
taskbar"` is the right wrapper -- the sweep starts the VM for the tools
that need one, and stops it for the ones that launch their own.

**And check your tree afterwards.** Five predates runs left six
undropped `predates-*` stashes. The tree WAS intact (the tool restores
in a `finally`), but confirm it -- `git status --short | wc -l` against
the file count you expect -- before doing anything else, and clear the
stashes only once the work is committed and only by matching their
names.

## 2026-08-25 -- a fixture that assumed the image had been booted

`ls_test.py` staged `/tmp/lsbig` into `disk.img` from the host and
failed with `no such directory: /tmp` -- against a perfectly healthy
system. `/tmp` is created at BOOT by the kernel's layout pass and is
never seeded by the build, so a freshly `make clean-disk`ed image that
has not been booted yet does not have one.

It had been latent for as long as the tool existed; nobody had run it on
a never-booted image. **A host-side fixture must create the directories
it needs, not inherit them from a boot that may not have happened.**
Ignoring the mkdir's result is right rather than lazy here: on an image
that HAS been booted the directory is already there, and either outcome
leaves what the next line needs.

## 2026-08-25 -- changing a command's output breaks tools that read it positionally

`df` grew an `on` column (the mount point) and started printing one row
per mount. That shifted every column index by one and made "the tfs3
row" ambiguous -- and `live_boot_test.py` and `partition_test.py` both
parse those columns by position.

Both now pick the ROOT row **by its mount point** rather than by being
the only row. `tools/check_tool_commands.py` cannot catch this class: it
checks that a command a tool drives still EXISTS, not that its output
still means what the tool thinks. That gap is `ondemand_sweep.py`'s job,
and it is the third time this exact rot has bitten `df`'s readers.

**2026-08-25 (USB/xHCI). THE THEME IS THAT THREE SEPARATE "THE DRIVER IS
BROKEN" RESULTS WERE ALL THE HARNESS, and each one was diagnosed only by
instrumenting rather than reasoning.**

**A CONTROL CAN BE FREE, AND IT IS WORTH TEN MINUTES TO FIND OUT BEFORE
WRITING THE FEATURE.** Before a line of the xHCI driver existed, one
experiment asked: with `-device qemu-xhci -device usb-kbd` attached to a
build with NO USB support, does a typed `touch /marker` still create the
file? It did not -- QEMU routes keystrokes to the USB keyboard the moment
it is attached, so PS/2 receives nothing. That single measurement made
the whole test suite self-controlling: a broken driver receives no input
at all, so every keystroke assertion already has its control and no
`i8042=off` scaffolding was ever needed. **Ask what the emulator does
with the device BEFORE designing the test around it.** The same fact is
also why the axis must be off by default -- attaching it to any existing
tool silently deprives that tool of its keyboard.

**AND RUN THE CONTROL ARM EVEN WHEN THE RESULT LOOKS DECISIVE.** The
first version of that experiment reported "keys did not reach the
guest", which was the answer I wanted. The control arm -- the same probe
with no USB devices -- ALSO reported no keys, because the probe was
typing at a graphical boot with no shell prompt. The exciting result was
measuring nothing. Two more harness bugs hid behind it: the socket path
was `.vm.serial.3` where vm.py writes `.vm.3.serial`, and the guest
needed `system.default_target text` set on a prior boot.

**A TEST WHOSE ORACLE IS "N THINGS EXIST" MUST PRINT WHICH ONES DID
NOT.** The ring-wrap phase reported 20 of 25 files missing and looked
exactly like a driver dying partway. Printing the surviving names cost
one line and turned "the driver stops after 5 files" into three
successive, different root causes:

  1. `send_key` SILENTLY DROPS a character it has no qcode for. `touch
     /usb_one.txt` created `usbone.txt` -- the underscore needs
     `combo(['shift','minus'])`. The file was "missing" because it was
     never named that. **A typing helper must refuse an unmappable
     character, not drop it**; ours raises now.
  2. **AN UNDRAINED SERIAL SOCKET STALLS THE WHOLE GUEST.** With the
     text target every spawn logs to COM1; nothing was reading it, the
     buffer filled, the guest's serial write blocked and the kernel
     stopped -- USB polling included. It presents as a driver dying
     after N keystrokes AND RECOVERS the instant anything reads the
     socket, so a dump taken afterwards shows a perfectly healthy
     driver with nothing pending. Measured: 5 of 25 files without a
     periodic drain, 25 of 25 with it. A long-running QMP test must
     drain the console as it goes.
  3. Only after both of those did the real driver behaviour show
     through -- and it was correct.

**THE SIZE OF THE THING BEING TESTED SETS THE SIZE OF THE TEST.** An
event ring is 256 TRBs, so a driver that never flips its cycle bit works
for ~128 keystrokes and then goes permanently deaf. Every check that
types a short marker passes on that driver. The wrap phase types 25
files -- past a full lap -- and asserts the LAST one; its positive
control (cycle flip removed) loses files 8 onward while THE FIRST FILE
STILL PASSES, which is the whole argument in one line. Ask what the
ring/buffer/cache size is, and type past it.

**AN ASSERTION ON A REQUEST'S EFFECT CANNOT SEE A MISSING REQUEST.**
QEMU's `usb-hid` reports boot-format whether or not `SET_PROTOCOL(boot)`
was ever issued, so skipping it is invisible here and breaks on real
hardware. The only testable thing is that the request was SENT, which
means counting it in the driver and asserting the counter. Whenever the
emulator is more forgiving than hardware, assert the action, not the
outcome.

**A KTEST NAMED FOR THE WRONG ANSWER EARNED ITSELF THE SAME DAY.** The
mouse test was written as "dy is not negated" from a misreading of
`input_report_rel()`, and it failed against the driver -- because
`mouse_feed_rel()` ends in `mouse_y -= dy` and wants UP-positive dy,
which HID and evdev both invert. The test caught its own author. It is
renamed for the property rather than the guess, and `input.h` now states
the sign, which nothing did before.

**2026-08-27 -- `tools/init_test.py`: 15/31 to 37/37, and the guess about
why was half right in the worst way.** It had been written down as
harness rot, which three of the four causes were. The fourth was a
kernel bug the tool's own check could not see.

**AN ASSERTION A BROKEN VERSION STILL PASSES IS NOT A CHECK, and the
worked example is `1 in vm.ps()`.** The tool asked whether pid 1 was in
the process table after `kill 1`. A killed init is a ZOMBIE, and a
zombie is in the table -- so the check was green while init had been
dying on that line for months, and every check after it failed for
reasons that read like separate rot. Assert the STATE, not the presence.

**`check_tool_commands.py` BEING GREEN IS NOT EVIDENCE OF HARNESS ROT.**
It says every command a tool drives still exists, which was true
throughout; the triage note reasoned from "the commands are fine, so it
must be output-shape rot" and reached the wrong half of the system. It
cannot see a command whose OUTPUT changed, and it cannot see the guest
being broken.

**THE PHASE BEFORE THE FAILURES IS NOT NECESSARILY THE PHASE THAT CAUSED
THEM.** The open note blamed the crash-loop fixture, "the phase
immediately before the failures start". The phase immediately before was
actually `kill 1`, one earlier than the note had counted, and it was
killing init outright. Count the phases in the OUTPUT, not from the
narrative.

**A DESCRIPTOR BUILT LINE BY LINE CAN BE READ HALF-WRITTEN.** The
ordering fixtures wrote `Name=` then appended `Exec=`, `Restart=` and
`After=` into a directory init rescans on every filesystem change. A
rescan landing between the lines sees a file whose `After=` names a
service that does not exist YET -- which init correctly ignores with a
log line, and then starts it out of order. Measured `ordx ordz ordy`
against the asserted `ordz ordy ordx`. Build the file elsewhere and `mv`
it in; that is what a real system tells you to do with a unit file, for
this reason.

**dmesg IS A RING, so a stamp asserted at the end of a long run reads
`None`.** The readiness checks compare timestamps from the first two
seconds of the boot, and by the time the tool had killed the desktop,
driven a new command and written a dozen descriptors, those lines had
scrolled out. Snapshot early, assert late -- and note the failure mode
is a missing stamp, which looks exactly like the feature not working.

**A POLL ON THE KERNEL'S LINE IS NOT A POLL ON init's.** The tool waited
for `init started as pid`, which the KERNEL prints at the spawn, and
then grepped that same snapshot for init's own first service start tens
of milliseconds later. This repo's documented flake shape, found again:
a poll whose exit condition is weaker than what the code after it
needs.


## 2026-08-27 -- type-ahead in `uui_table`: a suite that drives by mouse, and a fixture that could not tell two orderings apart

Three lessons, and the first is the one that let a broken feature ship
with a green suite and a written try-it guide.

**A SUITE THAT DRIVES BY MOUSE CANNOT SEE A DEAD KEYBOARD PATH.** Every
check in `taskmgr_test.py` clicked -- rows, headers, buttons -- so the
app's entire key handler could be absent and all 20 checks passed. The
new widget test covered the widget in isolation and
`filemanager_test.py` covered an app that DOES route keys, so between
them nothing could see that a second app routed none. Two green tests
about a mechanism are not coverage of every caller of it. **Ask which
INPUT DEVICE each check uses, and whether any check uses the one your
change is about.**

**AND THE HANDOVER CLAIMED IT ANYWAY.** The delivery's try-it guide said
"Task Manager, type `t` -> selects toywm". That path was never run. A
try-it guide is a set of assertions about the built system: if a row of
it has not been executed, it is a guess, and the maintainer executing it
is the test. Run the guide before writing it.

**A FIXTURE WITH ONE MATCH PER LETTER CANNOT DISTINGUISH TWO ORDERINGS.**
The table's search must walk the SORTED view, not the app's row array.
The first fixture had four rows with distinct initials, so a control
that deliberately walked the array passed EVERY check -- one match is at
the same place in both orders. This is the repo's "the data never
reached the code under test" rule from a new angle: the data reached it,
and could not DISCRIMINATE. Rebuilt so two rows share an initial and the
array order is the reverse of the sorted order, the control failed
exactly two checks with the inverted values.

The general form: **for a check about ORDER, the fixture must have at
least two candidates AND the two orders must disagree about which comes
first.** Write down what the wrong implementation would answer, and
confirm the fixture makes that a different number.

**A NEW TEST'S NAME MAY ALREADY BE TAKEN, AND `cat >` DOES NOT ASK.**
`userland/tests/seek_test.c` was written straight over the existing
`lseek` test -- a tracked file, clobbered without being read. `git
status` showed ` M` rather than `??`, which is the tell. Restored with
`git checkout` and renamed to `typeahead_test.c`. Check the name is free
before creating a file, and read `git status` after: an ` M` on a file
you meant to CREATE is a file you destroyed.

**2026-08-27: THE BUG CLASS THAT NEEDS A CONFIGURATION NO TEST
PRODUCES. Three of them in one session, all reported from real
hardware, none findable by any check in this repo.**

- A UEFI boot failed in GRUB (`no suitable video mode found`) because no
  `grub.cfg` here loads a video driver. Every automated boot is BIOS,
  where the i386-pc core image has VBE built in.
- The live image displaced a real disk, because the guard asked
  `ata_present()` -- the LEGACY IDE probe. Every live test boots with
  **no disk**, which is the one configuration where the right and the
  wrong predicate agree.
- A machine's second drive did not exist, because the disk init line
  short-circuited. Every test here attaches **exactly one disk**, which
  is the one configuration where "which driver ran" and "which disk is
  root" cannot disagree.

**The general form: find the PARAMETER your fixtures all pin to one
value.** How many disks. Which firmware. Which controller. How many of a
thing exist. A suite built entirely at one value cannot see anything
that only misbehaves at another, and it will be completely green while
it cannot.

The habit that follows: when writing a test, say what its fixture holds
FIXED, and ask whether the code under test branches on it.
`tools/multidisk_test.py` exists because the answer was "the number of
disks", and it took a user booting a laptop to ask.

**REPRODUCE A REAL-HARDWARE REPORT LOCALLY BEFORE THEORISING.** The UEFI
failure looked like a hardware problem and took four minutes to
reproduce exactly, under OVMF (`/usr/share/edk2/x64/OVMF_CODE.4m.fd` as
pflash) -- report to repeatable test to two-line fix. Ask what a
different QEMU MACHINE would show, not only a different QEMU version:
`qemu_matrix.py` varies the emulator, while firmware, controller and
drive count are separate axes it does not touch.

**A POSITIVE CONTROL THAT REDDENS NOTHING MEANS THE TEST NEVER REACHES
THE PATH -- a finding about the TEST, not a green light.** A new
`SYS_WAIT_READY` KTEST got a control that made the call CONSUME the
event it waited for, the exact bug its design prevents. Nothing went
red: the test only exercised the block-then-wake path, where a woken
syscall returns through its saved trapframe and never re-runs the body,
so the consuming line could not execute. A fourth sub-check for the
already-queued path made the control fail on the right assertion
(`expected 4, got 3`).

The existing rule is "a green test proves nothing until you have seen it
go red". The sharper version: **when the control fires nothing, do not
conclude the code is fine -- find which path the fixture actually
takes.**

**A RESTORED TREE IS NOT A REBUILT TREE.** `predates.py` restores the
SOURCE and, until this session, left `build/` and `disk.img` holding
what HEAD produced -- so the next tool run tested the other kernel while
every file on disk said otherwise. It reported a working fix as broken
twice before the cause was spotted. The tool rebuilds after restoring
now; the habit to keep is that **anything building from `build/` or a
boot image needs the build to be current, and nothing tells you when it
is not.**

**AND `predates.py` STASHES UNTRACKED FILES.** A tool written this
session does not exist during the HEAD run, so the comparison prints an
argparse "unrecognized arguments" and calls it a difference. It warns
now. The measurement is only valid for a command that exists on both
sides.

**2026-08-27: I LEFT TWELVE BACKGROUND WAIT-LOOPS RUNNING, AND
CLAUDE.md ALREADY SAID NOT TO.** The user found them in their task list,
ages 35-100 minutes, every one of them mine.

The rule is "DON'T ADD A WAIT LOOP FOR WORK THAT IS ALREADY IN THE
BACKGROUND -- the completion is already the signal", and knowing it was
not enough, so here is the mechanical version.

**A background job NOTIFIES you when it finishes. That notification is
the wait.** Starting a second background job whose only purpose is to
watch the first is always redundant: it cannot make the answer arrive
sooner, and it doubles what has to be cleaned up. What it feels like at
the time is impatience -- the foreground Bash call has a 120 s cap, the
job takes eight minutes, and spawning a watcher feels like progress. It
is not. Start the job, say what you are waiting for, and stop.

**IF YOU DO WRITE ONE, WAIT ON SOMETHING THAT MUST BECOME TRUE.** Half
of mine could never exit:

- `until [ -f tools/idle_cpu.py ]` -- that file had gone to a BRANCH, so
  on `main` the condition was unreachable forever.
- `until [ -s <task output> ]` -- the job had already finished and had
  written nothing, so the file stayed empty for the rest of the session.
- `until ! pgrep -f preflight.sh` -- fine, except when the process had
  exited before the loop started, in which case it exits at once and is
  pure noise, and if it had NOT, the notification was coming anyway.

Before writing the condition, ask **what makes this true, and can it
still happen?** A file another process must create, that no process is
going to create, is a loop with no exit.

**AND CLEAN UP AT THE END.** `ps -eo pid,ppid,comm | awk '$3=="sleep"'`
finds them; killing the PARENT shell ends the task, and the harness then
reports it. A leaked sleeper costs nothing in CPU and everything in the
user's ability to see what is actually running.

## 2026-08-28 -- a settled frame said the machine was wedged, twice

Testing whether `printf("prompt: ")` + `scanf()` shows its prompt meant
photographing the ring-0 console while a program sat blocked on a read.
Three screenshots in a row led to a wrong conclusion, and the third one
led to publishing it.

**"Settled" said nothing about whether the guest had caught up.** The
entry above ("Settled is not the same as UPDATED") is about a CLIENT the
compositor has not painted yet. It applies just as hard with no
compositor in play: `stable_pixels()` polls twelve times at 0.15s, so a
console that has not yet repainted is two identical reads like any other,
and the capture is a settled photograph of the past. Two consecutive
captures 2s apart were byte-identical (`md5sum` on the PNGs), which read
as "nothing is happening" and was really "nothing has happened YET".

**From that I concluded the machine had hung, and said so.** The screen
showed a typed command with no output, so I reported the guest wedged.
It was not: the serial console answered `sh kstack slots` immediately and
showed the program alive and BLOCKED on its read, exactly as intended. A
frame is evidence about the framebuffer, never about the machine. **The
discriminating check costs one command** -- ask the serial console
something, since it is a separate path that does not care what the screen
is doing -- and it should come BEFORE any sentence containing "wedged",
"hung" or "frozen".

**The same mistake a second time, in the opposite direction.** Having
been bitten once, I then declared a bare `sum` at the `#` prompt "stuck
on the blocking read" -- again from a settled frame. A screenshot taken
3s later showed `Give me two numbers: Sum is: 5 + 6 = 11`: it had worked
the first time. The correction is not "wait longer". It is that the
ring-0 console has an observable and I was not using it: the serial
console's own reply to `sh kstack slots` says whether a process still
exists and what state it is in, which is the fact the check was actually
about.

**And the reflex that started it: I typed `spawn /bin/sum` for a program
that needs no `spawn`.** I had copied the shape from `stdin_test.py`,
which spawns `/bin/tosh` -- a program that must outlive the shell. `sum`
does not, and a bare name runs it correctly. The cost was not the extra
word: `spawn` returns immediately, so the shell prompt came back and
raced my typed input, and one run's numbers went to the shell as
`Unknown command: 3` instead of to the program. **A pattern copied from a
tool carries that tool's reasons.** CLAUDE.md already states this shape
for widget ops tables -- "fill a new widget's table against
`uui_widget.h`, never against the widget you copied, a copied table
inherits its gaps" -- and it is the same failure with a harness instead
of a struct. Ask what the original needed the line FOR before keeping it.

## A serial log line can arrive GLUED to a console echo (2026-08-28)

`uterm_test`'s t4 failed with the evidence in hand: `titles seen:
['gui state --jsuterm: tab 1 title /bin']` -- the app's log line
arrived concatenated to the debug console's own command echo, so a
positional `l.split()[2]` parsed the echo, not the line. The title had
reached the right tab all along. Any parse of a serial-stream line
must RESLICE from its marker (`l[l.index("uterm: tab"):]`) before
splitting. Under load this happens often enough to fail a suite run.

Same session, two self-inflicted invalid repros, both already-warned
shapes: `send_text()` silently drops `/` `.` `_` (typed
`edit /t.txt`, got `editptye` -- use the debug console's key
injection), and a REUSED guest poisoned a click-coordinate section (my
leftover Terminal window shifted every position; the run failed
window-management checks that were fine). Fresh guest per GUI repro,
and read the tool's own typing helpers before improvising.

**2026-08-29 (audio: testing something whose output is a WAVEFORM).**

- **ISOLATE THE PATHS; DO NOT COMPARE A MIXTURE AGAINST A PART.** The
  first Doom audio test played the game normally and then with
  `-nomusic`, requiring window COVERAGE to collapse. Measured: 86% with
  both against 69% with effects alone -- a 17-point gap, because Doom's
  attract demo is almost continuously noisy. Far too weak a thing to
  hang a verdict on. The rewrite boots twice and isolates each path
  (`-nosfx`, then `-nomusic`); ANY signal in each run is conclusive on
  its own, because nothing else can be making it. **Two silences that
  should not be silent beat one difference of degree.**
- **THAT NEEDED A LEVER, AND ADDING IT WAS PART OF THE TEST WORK.**
  `-nosfx`/`-nomusic` only reach Doom because the app forwards its
  argv, which it did not before. When a test cannot separate two
  things, ask what small product change would let it.
- **CALIBRATE A THRESHOLD ON THE SIGNAL IT MEASURES.** "Peak > 4000"
  was set from the effects' level (~19000) and failed music, which
  peaks at 1735 -- 21 dB quieter, and correctly so. Two different
  signals want two different bars.
- **AND THAT FAILING THRESHOLD WAS A REAL FINDING.** Music being 21 dB
  under the effects meant it was inaudible under gunfire. The fix was a
  product change (a fixed gain on the music source), not a looser
  check. A threshold that fails is worth understanding before it is
  moved.
- **THE RECORDING'S FIRST SECONDS ARE NOT THE PROGRAM RUNNING.** WAD
  loading and 56 sound conversions are silent, and counting them
  dropped a healthy run to 80%. Measure the TAIL.
- **QEMU PADS A LAGGING GUEST WITH HOST-SIDE SILENCE**, so coverage
  tops out near 88% on a healthy build and 100% is not the bar. The
  same trap `audio_test.py` already documents, met again from a new
  direction.
- **USE `port_guard.port_is_free`, NOT A HAND-ROLLED WAIT.** A stopped
  VM does not release its QMP port instantly. A connect-based wait
  reports "free" while the port is in TIME_WAIT -- and port_guard
  checks by BIND with SO_REUSEADDR deliberately unset, so the next
  launch is refused anyway. One whole run lost to rederiving a
  predicate the repo already exports.
- **A CHECK THAT PASSES WHEN THE RUN DID NOT HAPPEN IS WORSE THAN A
  FAILURE.** "The music module did NOT come up" is trivially true of a
  guest that never booted, and it printed `ok` on a run whose VM was
  refused. Guard the phase: if the boot failed, FAIL the phase rather
  than evaluating checks whose subject does not exist.
- **DO NOT REBUILD WHILE A TEST IS RUNNING.** A `make all` mid-run made
  `build/userland` newer than `build/.seeded`, and `iso_guard` refused
  the second boot. It was right to; the lesson is to let a run finish.

**2026-08-29 (networking, and a fresh disk that was not fresh).**

- **A "FRESH" DISK IS TWO COMMANDS, AND ONE OF THEM IS `make clean`.**
  `seed/sync/` is build staging that `make clean-disk` does NOT touch,
  so a file deleted from `data/` keeps being staged onto the next image.
  Three reproduction attempts here ran against a machine with two
  service descriptors I had already removed, starting programs I was not
  expecting. The pair is `make clean` (wipes staging) THEN
  `make clean-disk` (wipes the image). Confirm with what the guest
  actually runs -- `ls /etc/services.d` -- not with what `data/` holds.
- **THE HOST IS THE ORACLE FOR ANYTHING THAT LEAVES THE MACHINE.** For
  the network stack that meant three independent checks, each catching
  what the others could not: QEMU's SLIRP ANSWERING (a wrong checksum is
  simply never replied to), a REAL PYTHON SOCKET or `http.server` at the
  far end (bytes compared against what the host sent), and a PCAP
  decoded host-side with the checksums RECOMPUTED. The third is the one
  that catches a pseudo-header a lenient peer would tolerate -- a stack
  that omits it agrees with itself perfectly.
- **PREFER A SERVER ON LOOPBACK TO A SITE ON THE INTERNET.** The suite
  must not depend on the machine having connectivity, and testing that
  leaves the machine has to be DISCLOSED in the commit (see
  `.claude/skills/network-egress-disclosure/`). `net_test.py` fetches
  from a local `http.server` and SKIPS its DNS checks when the host
  cannot resolve.
- **A TEST THAT DOES NOT WAIT FOR THE DESKTOP IS A FLAKE WITH A DATE ON
  IT.** `idle_desktop_test.py` was the only GUI tool that connected and
  queried immediately instead of going through `enter_gui()`; it failed
  1 run in 6 at HEAD and 3 in 6 on a busier tree. `predates.py` measured
  both, which is what turned "is this mine?" into a number rather than
  an argument -- and at those counts the two rates are not
  distinguishable, which is exactly why the fix was the poll rather than
  a claim about whose change it was. It calls `wait_for_desktop()` now,
  deliberately NOT `enter_gui()`: that also turns on the per-frame
  layout log, and a tool measuring an IDLE desktop is the last one that
  should be given more to log.
- **A SPAWNED `/tests` PROGRAM REPORTS THROUGH A FILE, NOT STDOUT**, and
  which of the two it is decides whether its checks can measure
  anything. `usertest_run.py` drives tests with the legacy `run` loader,
  which has NO SCHEDULER SLOT -- so a test whose checks are about
  BLOCKING (a receive that must really wait out its timeout) measures
  nothing there: the kernel correctly gives a slotless caller the
  non-blocking answer. Register it with an exit code of `None` to have
  it SPAWNED instead, and have it write its verdict to
  `/tmp/<name>.out`, because a spawned process's console output arrives
  while the harness is between commands and is dropped.

**2026-08-29 (moving a guarantee out of the kernel: what it does to
every test that inherited it).** The kernel stopped assigning a boot-time
IP so `/bin/dhcp` could. Nothing about the stack changed, and twenty
checks across two suites went red or went quiet.

- **WHEN A PRECONDITION MOVES FROM RING 0 TO RING 3, IT STOPS BEING
  INSTANT.** The address used to exist before the first process ran; it
  now lands about a second after the debug prompt. Six `net_test.py`
  phases pinged immediately and failed as `no such device` -- which
  looks like a routing bug and is a race. The fix belongs in the
  LAUNCHER, not in each phase: `launch()` polls until every card that
  will get an address has one. Ask, of anything a change makes
  asynchronous, which tests were relying on it being synchronous.
- **AND A KTEST THAT INHERITED THAT PRECONDITION SILENTLY LOSES
  COVERAGE.** `addressed_device()` returned the first device holding an
  address, and fourteen KTESTs skipped without one. They still passed --
  the lease happened to arrive before `ktest` ran -- so nothing failed,
  and the whole file's coverage had quietly become a race. A skip
  reports only as a count nobody reads. The helper establishes the
  precondition itself now, which is this repo's own rule arriving from a
  new direction: **a test must not inherit a precondition somebody else
  is now responsible for.** Found by asking what used to guarantee it,
  not by a failure.
- **THE POSITIVE CONTROL FOR THAT IS ONE LINE**: `if (0 && d && d->ip)`
  forces the fallback, and the run then shows the fallback's own log
  line and 13 net tests passing with 0 skipped through it. Without it
  "green" would only have said the fast path still worked.
- **BUILD THE FIXTURE THE EMULATOR CANNOT BE.** Link-local only happens
  where nothing answers DHCP, and SLIRP always answers. `-netdev
  socket,listen=127.0.0.1:PORT` with no peer is a segment with nobody on
  it; connecting a Python socket to it makes the test a NEIGHBOUR as
  well as an observer, since QEMU's socket netdev is raw Ethernet behind
  a four-byte big-endian length. That one fixture supplied the negative
  case, the ARP frames as evidence, and the conflicting host.
- **DO NOT PREDICT WHAT THE CODE WILL CHOOSE -- MAKE IT CHOOSE TWICE.**
  Checking duplicate-address detection needs a taken address. Computing
  the guest's candidate on the host would be checking its arithmetic
  against a copy of itself. Two boots instead: one unopposed to learn
  the address, one where the host answers for exactly that address, and
  the assertion is that the second differs from the first. It moved from
  `.205.161` to `.200.14`.
- **A ONE-SHOT IS STILL `running` WHILE IT WORKS.** The link-local path
  applies the address and then announces for two more seconds, so
  `ifconfig` answering is not the client having finished, and a status
  read there says `running` -- true, and not what the check was about.
  Poll for the service to leave `running` before asserting what it
  settled as.
- **`AF_UNIX path too long` IS 107 BYTES AND IT FAILS AS "never reached
  a debug prompt".** A scratch directory deep enough to exceed it made
  `Shell(sock)` raise `OSError` inside a retry loop that swallowed it.
  One socket name a single character longer than another was the whole
  difference. Keep guest serial sockets under a short path.
- **THE FIRST `service` RUN ON A MACHINE FINISHES AFTER THE CONSOLE'S
  READ WINDOW.** init publishes `/tmp/init.status` only when a doorbell
  asks, so the first reader rings, waits, and its output turns up in
  front of the NEXT command -- which reads exactly like the command
  having printed nothing. Ask again; do not lengthen one read.
- **A HARNESS CAN ENCODE A VERB THE GUEST DOES NOT HAVE.** `Shell.run()`
  in two tools exempted `spawn ` from its `sh ` prefix, and the debug
  console has no `spawn` -- its verbs are edit/gui/help/ktest/lsdev/
  lsfs/meminfo/nano/polled/schedtest/sh/usb. Every such call had been
  answering `unknown command: spawn` into whatever check read it. The
  same class as a tool driving a command that moved to `/bin`, which is
  what `check_tool_commands.py` exists for -- and it cannot see this
  one, because the command it names is real, just not there.
- **A WAIT LOOP ON A FILE THAT NEVER APPEARS RUNS UNTIL THE SESSION
  ENDS, AND `until [ -s … ]` CANNOT TELL "not yet" FROM "never".**
  Five of them were left spinning for six hours on
  `scratchpad/fm5/files.log` -- a run that was superseded before it
  wrote anything, so the directory existed and the log did not. They
  surfaced only because the maintainer asked what the running tasks
  were.

  CLAUDE.md already states this rule, in these words, and it was read
  and then broken anyway. So the mechanical version: **before writing
  the condition, ask what makes it true and whether that can still
  happen** -- a superseded run's artifact cannot. And note the loops
  were REDUNDANT even when they worked: the background command's own
  completion notification is the signal, so a waiter beside it adds
  nothing but a way to leak.

  The check costs one command: `ps aux | grep "[z]sh -c source"` names
  every shell the session still holds, and any `until`/`while` among
  them is one that should already have exited.
- **A TIMED-OUT TOOL THAT PRINTS NOTHING IS THE WORST FAILURE A GUARD
  CAN HAVE.** `gui_regress.py` reported the bare word TIMEOUT, so a
  `files` that had merely GROWN past its 360s hang guard read as a hang
  with zero checks and no clue where -- indistinguishable from a crash
  on line one. It kept the tool's partial output afterwards, and the
  last check printed is the whole diagnosis. **Re-check that guard's
  margin whenever the slowest tool grows**; it is sized against `files`,
  which is several times the next one.
- **AN ASSERTION CAN DEPEND ON A FACT NOBODY DECIDED -- AND THE PID
  TABLE IS ONE.** `taskmgr_test` pressed Down and required the table to
  report a new selection. `uui_table_key()` reports nothing when the
  selection CANNOT move, so Down on the LAST row logs nothing and reads
  exactly like a dead key handler -- the very bug that check exists to
  catch. Whether the victim was last depended on which pid Task Manager
  happened to get, and a change elsewhere in the kernel shifted that: a
  guest that reaped `/bin/dhcp` before the app launched handed it pid 2
  instead of 5, which put the victim last. Two checks went red with
  nothing wrong in the guest, in a change that had not touched key
  routing at all.

  Three things this cost, worth knowing before spending them again.
  `tools/predates.py` said **YOURS** -- correctly, since the change DID
  cause the red -- which reads as "you broke the feature" and is not the
  same sentence. Reverting the whole GUI half still failed, which is
  what finally pointed away from the code under suspicion. And the
  answer was in a DIFF OF THE TWO LOGS, not in the code: 5 rows
  `[1,3,4,5]` against 4 rows `[1,2,3,4]` named the cause in one line
  after an hour of bisecting had not.

  **So: when a check goes red in a change that cannot plausibly reach
  it, diff the passing and failing RUNS before bisecting the tree.**
  And when writing a check that drives a widget, ask what state would
  make the action a no-op -- an arrow at the end of a list, a Back
  button on the first page -- because the widget reports nothing in
  exactly that case and the test cannot tell it from broken.

**2026-08-31 (the MP3 host harness). THE THEME IS THAT THE POSITIVE
CONTROL FOUND TWO FAULTS IN THE HARNESS AND ZERO IN THE CODE -- and
both faults had been reporting a broken decoder as fine.**

The suite was eight lame-encoded files compared against ffmpeg. It went
green. Then the control ran, and neither of these would have been found
any other way:

- **A TOLERANCE PICKED BY EYE WAS 10 000x TOO LOOSE.** I set 0.02 RMS
  because it "sounded about right" for a lossy codec. Measured agreement
  turned out to be 1.6e-6 -- one LSB in 32768 -- so the bar sat four
  orders of magnitude above the noise floor. `--positive-control` (two
  Huffman tables swapped) showed a deliberately broken decoder PASSING
  two of the eight checks under it. The repo already says a threshold
  picked without a control is a guess; this is what the guess costs.
  Calibrate from the measured clean value, not from intuition about the
  domain.
- **THE REPORT'S PRECISION IS PART OF THE TEST.** Printing `rms 0.00000`
  to five decimals made a 2.4x change in the metric invisible: my first
  control nudged a window coefficient, the number did not visibly move,
  and I briefly concluded the harness was not comparing at all. It was;
  the DISPLAY was lossy. Scientific notation plus the WORST SINGLE
  SAMPLE beside the mean made every later control legible. A mean over
  260 000 samples hides exactly the small systematic error a weak
  control produces.
- **AN ORACLE'S ALIGNMENT ASSUMPTION CAN CONDEMN CORRECT CODE.** The
  comparison searched for the best lag in whole frames (1152 samples),
  which is right for a bare stream. A file carrying a LAME/Xing gapless
  tag makes ffmpeg drop the ENCODER DELAY -- 1105 samples, not a
  multiple of anything -- so the shipped song reported RMS 1.8e-1 and
  looked badly broken while being bit-accurate. Searching every sample
  found lag 1105 and RMS 1.4e-6. When an oracle disagrees only on ONE
  input, suspect what is special about that input before the code.
- **SHAPE THE CONTROL TO DEFEAT THE CHECK YOU ARE NOT TESTING.** The
  cheap structural check (Kraft: is every table a complete prefix code?)
  catches a corrupted codeword instantly. That makes it useless as a
  control for the expensive check, because it fires first. Swapping two
  codewords of EQUAL LENGTH leaves the code complete and prefix-free, so
  the structural check still passes and only the ffmpeg comparison can
  see it. A control the cheap gate catches proves nothing about the
  expensive one.
- **A GENERATOR FOR TAKEN DATA IS A TEST, NOT A CONVENIENCE.**
  `tools/gen_mp3_tables.py` re-derives the committed tables from two
  independent public-domain sources and REFUSES TO WRITE unless they
  agree entry for entry. That turns "these 473 lines came from
  somewhere and are probably right" into something re-runnable, and it
  keeps `loc.py` honest about what was actually written here. Any data
  the repo takes rather than writes deserves the same treatment.

## 2026-09-01 -- when a settled screenshot is the WRONG instrument

`QMPSession.stable_pixels()` and `hover_frames()` exist because a
capture landing mid-paint fails a comparison with nothing wrong. But
settling has a cost that took a session to name: **it outlasts anything
else that repaints.**

The desktop repaints once a second for the tray clock. So a check that
warps the cursor onto a menu row, settles, and photographs it finds the
highlight correctly placed -- *having arrived up to a second late*.
That lateness IS the bug being looked for, and settling launders it
away. The test that works counts frames (`gui state`'s
`scene repaints`) and never looks at a pixel.

**The rule: settle when asking WHETHER something is drawn; never when
asking WHEN.** For latency, find a number.

## A test tool must match the runner's argument shape

`gui_regress.py` passes `--sock` and `--qmp-port`; a new tool taking
`--instance` fails with `unrecognized arguments` and shows up as a red
tool that runs correctly by hand. Copy the argparse block from a tool
the runner already drives (`keyup_test.py`) rather than inventing one.

## An injected pointer position snaps back, so it changes hover TWICE

`gui move X Y` overrides the pointer for ONE `wm_run()` iteration; the
next reads the real mouse. So an injected move onto a menu row changes
the hovered row to that row AND THEN back to none. A check written as
"moving within one row must not repaint" measured the snap-back and
failed against correct code.

It is still the right tool for asking whether an input causes a FRAME --
one console round trip (~10 ms) against a warp's several hundred, which
is what keeps a 1 Hz clock out of a frame-counting measurement. Use
`hover_frames()` when the STATE must survive a capture; use `gui move`
when the question is whether something happened at all.

## Ask the fixture for the thing it will actually be checked for

A poll that waits for "any content" in a file catches it MID-WRITE. One
read returned `3830823995 ` with the rest of the line still coming, and
the verify behind it reported a malformed manifest -- indistinguishable
from a broken parser. Poll for the LAST token of what is being written.

## Sizes as the fixture, and asserting your own precondition (2026-09-01)

**My first test for a block-leak passed WITH the bug present, and the
reason is this repo's oldest testing trap.** `delete_path()` in
`tools/tfs3_writer.py` freed a file's data blocks plus the
single-indirect table, and never the double- or triple-indirect ones. I
tested an overwrite with a 100 KB file: 25 blocks, single-indirect only,
so the broken branch was never reached and the test was green. The
threshold is 12 direct + 1024 single = ~4.05 MB, and the seed tree has
exactly ONE file past it. Same shape as the 16 KB truncate fixture that
fit twelve direct pointers.

The fix is to make the SIZES the test: one case per block-map level,
each reaching one level further than the last. It then reddens on
exactly the double-indirect case, by exactly 2 blocks, with the two
levels the code already handled staying green -- which is far better
evidence than one red line, because it shows the harness can measure
both outcomes. **When a data structure has levels, tiers or a growth
path, the fixture has to cross each boundary or the test only covers the
first one.** Ask which branch each case reaches, not just whether it
passes.

**And when a harness ENABLES something for a suite, it must assert the
enabling worked.** `ktest_run.py` stops the desktop so the winshare
KTESTs can take the compositor role; those tests skip when they cannot.
A suite that skipped them would then report PASS having tested nothing
-- so the harness fails the run when the skip reason appears in the
transcript. That check caught a real race in its own first version
within one run: the stop was sent before init had read
`/etc/services.d`, init answered `supervises no service called toywm`,
and the desktop started anyway, about half the time. Without the
assertion that would have been an intermittent, silent loss of coverage
rather than a red run. **The general form: after arranging a
precondition, assert from the run's own output that it held.**

## The same runner, or it is not a measurement (2026-09-02)

Two `ktest_run.py` runs in a row on the pre-session commit stayed
clean, while the second `preflight.sh` in a row on today's tree failed
three `fs` KTESTs on `fsck` leaks -- and that was nearly reported as a
regression. Two `preflight.sh` runs on the OLD commit reproduced it
exactly: the ring-3 `/tests` programs preflight runs BETWEEN kernel
suites leak the blocks. "It predates me" has to be measured with the
runner that showed the failure, not a cheaper one that shares half of
its steps. `make clean-disk` before the gate is the workaround; it is
recorded in `docs/bugs.md`.

## A control that never reached the branch, again (2026-09-02)

Bounding the xHCI capability walk by BAR size, the positive control
set the bound to 0x100 and then 0x50 and NOTHING changed -- QEMU's
chain sits at +0x20 and +0x30 and ends on its own before either. The
control only went red at 0x30, after logging the walk's base. CLAUDE.md
already says "ask what input actually reaches the branch"; the extra
lesson is to PRINT the fixture's shape before choosing the control's
value, not after the control fails.

## A wait that ends on the FIRST required string (2026-09-02)

`faulttest_run.py` ended its read early on `required[0]` and then
demanded `required[1]`, which had not arrived. The rule "a poll whose
exit condition is weaker than what the code after it needs is a
flake" -- caught here by the first entry that required two strings.

## A rounded corner shows what is beneath it (2026-09-02)

Two GUI checks failed after windows gained rounded corners, and
neither was a bug: one sampled a client's border 2 px from its corner,
where the frame's outline ring now is; the other compared a window's
content before and after refocusing while a second window's edge lay
UNDER its corner. Both tests read the backdrop through the corner.
Sample an edge's middle, or inset the compared box by a radius.

## The slice trap, third time (2026-09-02)

`s[:i] + new + s[j:]` with `j = s.index(end)` -- searched from the
start, which landed BEFORE `i` -- duplicated 25 headings in
`roadmap-details.md`. `check_docs.py` caught it; the fix was
`s.index(end, i)` plus a line count before and after, which the skill
already prescribes and I did not do.

**2026-09-02 (the 8 GiB suite). THE THEME IS THAT A NEW MACHINE SHAPE
NEEDS THE WHOLE SUITE, NOT THE SUBSYSTEM'S.**

- **Run everything at the new configuration, not the tests for the
  thing you changed.** The `mm` suite was green at 8 GiB while eight
  VIRTIO tests were red there, because 8 GiB is where SeaBIOS moves the
  PCI 64-bit window to 768 GiB -- a fact about the machine, not about
  memory management. `highmem_test.py` runs the whole suite for that
  reason, and refuses a skip of the above-4-GiB checks by grepping for
  its own skip text rather than "0 skipped" (other suites skip for
  their own reasons).
- **A check that asserts an INTERMEDIATE state breaks the moment the
  next stage lands.** "Frames above 4 GiB are idle" was true for one
  commit; the heap became a consumer in the next and the assertion
  went red for the right reason. Assert the invariant (free <= total,
  a zoned alloc returns high) rather than the snapshot.
- **A test can encode the assumption being removed.** `virtio_test`
  asserted every window is below 4 GiB -- a restatement of the refusal
  the change deleted. When lifting a limit, grep the tests for the
  literal.
- **A positive control for a MAPPING is a crash, not a red line**, if
  the test dereferences before it asserts. The reachability test asserts
  `phys + 4096 <= paging_identity_limit()` FIRST so the control goes
  red instead of faulting ring 0 with no report.
- **Restoring a control with `git checkout <file>` also reverts every
  uncommitted edit in that file.** It did, silently; the suite caught
  it as the heap test going red after the "restore". Undo a control
  with the inverse `sed`, or copy the file aside before breaking it.
- **Check an edit script's EXIT before building on its output.** One
  anchored replacement mismatched on a comment line, the script
  stopped there, and the next command in the chain built a half-applied
  tree and ran the suite on it. `&&` between the script and the build.
- **A doc/comment edit that names a stage number rots in a day.** "Not
  yet allocatable" was true at noon and false by evening. Say what the
  reader can observe ("kernel heap only, not yet for processes").
- **A CHECK COPIED FROM A WORKING TOOL CAN CARRY A DEAD ASSERTION.**
  `mem_stress.py` runs `sh meminfo audit` and requires "no dangling" in
  the reply -- but the command is `meminfo --audit`, so it prints a
  usage line and exits 1, and that check has been unsatisfiable since it
  was written. Nobody noticed because nobody looked at a FAILING run of
  it. Found by copying the three lines into a new tool and watching them
  fail on a machine that was demonstrably clean. When you lift a check
  from a tool that "passes", run it once with the outcome you expect it
  to REJECT.
- **A KTEST THAT MAPS A USER PAGE MUST USE A REAL USER ADDRESS.**
  Everything under PML4 entry 0 shares the kernel's own PDPT (every
  address space points at the same one), so a page mapped at 0x400000
  goes into the KERNEL's tables, is skipped by
  `vmm_destroy_address_space()`, and leaks its frame. The mapping works
  and the copy helpers reach it, so nothing fails -- the check that
  caught it was asking whether the intermediate table was above 4 GiB
  and getting the kernel's answer. Map at `UADDR_IMAGE_BASE` or above.
- **A POLICY THAT ONLY FIRES ON A MACHINE YOU CANNOT BUILD NEEDS A
  SETTER, and that is not test-only scaffolding.** pmm's DMA32 floor is
  enforced only when an `ANY` allocation falls back, which a machine
  with 5 GiB of high zone never does -- and draining that zone is not a
  test. `pmm_set_dma32_reserve_frames()` lets the check jam the floor to
  four frames below free and require the next request to be refused.
  Linux exposes the same knob as `lowmem_reserve_ratio`, which is the
  argument that it is an interface rather than a hole cut for a test.
- **A SUITE THAT NAMES ONE ADDRESS CANNOT SEE A RANGE NOBODY NAMES.**
  Seven `winshare` KTESTs asserted things about the compositor's mapping
  of a window, all of them at the slot's BASE, and the fixture's windows
  are 64x32 -- one page. `comp_map()` had always mapped a SECOND buffer
  at `+WIN_BUFFER_HALF` that no revocation path touched, so every window
  close left the compositor holding writable PTEs into freed frames, and
  all seven checks stayed green for months. The check that found it asks
  the whole address space instead of an address: `vmm_audit_space()` on
  the compositor after a destroy, requiring zero dangling. When a
  subsystem has a RANGE, prefer one assertion over the range's invariant
  to N assertions about points in it.
- **RUN THE AUDIT YOU ALREADY HAVE AGAINST A MACHINE THAT HAS BEEN
  USED.** `meminfo --audit` existed for months and every caller ran it on
  a machine that had just booted or just run one test. Pointed at the
  bare-metal laptop after ordinary desktop use it reported 1933 dangling
  mappings immediately. A diagnostic only ever run on a clean fixture is
  a diagnostic that has never been asked a hard question.
- **AIM A POSITIVE CONTROL AT THE CODE THAT MAKES THE ASSERTION TRUE,
  NOT AT THE FIRST PLAUSIBLE LINE.** Breaking `comp_clear()`'s loop
  changed nothing red, because `comp_poison()` maps over the stale PTEs
  afterwards and repairs the damage. Breaking `comp_poison()`'s loop
  reddened a DIFFERENT assertion in the same test (a hole, not a
  dangling mapping). Only breaking both reproduced the original defect.
  Three arms, three different answers, and the first one alone would
  have been read as "the test cannot fail".
- **NEVER REBUILD WHILE `gui_regress.py` IS RUNNING.** `make all`
  rewrites `build/kernel.bin`, `iso_guard.py` then refuses every guest
  launched after it, and the run ends with a dozen or more tools failing
  as `the guest never started: iso_guard: REFUSING to boot a stale
  image` -- a list long enough, and containing enough tools the change
  could not touch, to read as a catastrophic regression. The tools that
  had already launched pass, so the split looks meaningful and is not.
  It cost a full suite run. Read one failing tool's line before
  believing any mass failure: the guard names itself.

**2026-09-02 (the display work). FIVE WAYS A DAY OF GREEN RUNS NEARLY
LIED, and one measurement that cannot be automated.**

- **REBUILDING WHILE THE SUITE RUNS INVALIDATES THE SUITE.** A `make
  all` for a KTEST file during `gui_regress` tripped `iso_guard` on
  every tool that launched afterwards -- 19 "failures" reading as a
  regression. Everything that ran before the rebuild passed. Do not
  touch the tree while a suite holds `disk.img`; write docs instead.
- **A PROBE OF YOUR OWN CONTAMINATES THE GUEST FOR EVERY LATER TOOL.**
  Three `gui click` presses on the Start button left it OPEN, and the
  brightness tool that ran next on that guest failed its first two
  checks. The menubar tool then failed once in that same contaminated
  guest and never again in four fresh boots plus two paired runs, with
  `predates.py` finding HEAD and the working tree both green. Recorded
  in `docs/bugs.md` as unattributed. Reset what a probe changed, or run
  the tool on a fresh guest, before believing a failure.
- **A TOOL THAT PERSISTS A SETTING MUST RESTORE IT IN A `finally`.** The
  mode-change tool crashed once between switching to 1600x900 and
  switching back; the stored resolution then booted every later guest
  at 1600x900 (`make iso` syncs, never wipes `/etc`), and the next run
  SKIPPED as "already at the target". The rule about a setting changing
  the machine for every later tool now applies to every later BOOT.
- **A KTEST THAT ACTS ON THE SCREEN MUST SKIP WHILE A COMPOSITOR HOLDS
  IT.** The virtio flip KTEST counted commands and flipped scanouts
  under a live desktop presenting concurrently: red inside
  `virtio_gpu_test.py`, green from `vm.py exec` before the desktop was
  up. `win_surface_holder()` is the predicate. And the tool's own "did
  not all skip" check then had to allow the one legitimate skip.
- **THE LAPTOP'S `sum` IS WRONG ON A LARGE FILE.** A flashed kernel's
  crc32 disagreed with the host's while a 10-byte file agreed;
  `remote.py get` plus `cmp` showed zero differing bytes. Verify a
  flash by pulling it back, not by the checksum (`docs/bugs.md`).

**The measurement that cannot be automated:** tearing. `guictl fb`
proves flips happen; only the maintainer dragging a window says
whether they are tear-free, and the first design was wrong exactly
there. For a change to the present path, ask them to look BEFORE the
long gate, and say plainly in the commit that it was not measured.

## A laptop that does not answer after a flash is not necessarily the kernel (2026-09-03)

The first boot of the EDID-probe kernel never came back on the network
and the session had no way to tell a hung display probe from the
known 1-in-~9 boot that loses the whole xHCI enumeration
(`docs/bugs.md`). It was the latter: the maintainer saw a desktop with
no mouse and no NIC, rebooted, and everything was there. Two things to
do before concluding a flash broke boot: wait longer than one boot's
worth, then ASK what the screen shows -- the question has three
answers (desktop up, black, panic text) and each sends the next step
somewhere different. And a `$R exec ...` with `R` a string does not
word-split under zsh; a function or a script file does.


## THE DEBUG CONSOLE IS NOT A TERMINAL A LONG-RUNNING PROGRAM CAN WRITE TO (2026-09-05)

`vm.py exec "spawn /bin/foo"` returns the output `foo` produces around
the spawn and **nothing it prints later**, even while the socket stays
open. This was diagnosed twice as "the feature is broken" before an
instrument settled it: a `netlog -f` poll loop printed its first pass
and then appeared dead, so the flag was written up as not working. It
works.

The sequence that got there, and the order matters:

- **`ps` first.** The process was `block(timer)`, which says it is
  alive and sleeping -- so "it exited" and "it never loops" were both
  out before any theory was formed.
- **`strace` second**, read back through `dmesg`, which does not depend
  on the console staying attached: `sleep(500) = ?` then two `query()`
  calls, repeating. That is the loop running. Nothing else had to be
  guessed after that line.
- **A second `vm.py exec` beside the first is not a second channel** --
  two `DebugConsole`s on one serial socket steal each other's replies,
  and connecting a raw socket to `.vm.serial` behaves the same way.

**The way to observe such a program is to give it somewhere durable to
write.** `spawn /bin/tosh -c netlog -f > /follow.log` works and the
quoting is the trick: the `#` shell's `spawn` splits argv on
whitespace with no quote handling, and `tosh -c` REJOINS argv[2..] into
one line, so an UNQUOTED redirect survives the round trip and a quoted
one does not. Then `cat /follow.log` from an ordinary `exec`, twice,
and assert it GREW.

Two smaller ones from the same afternoon:

- **`vm.py exec` takes one command per flag.** `exec "a" exec "b"` is
  parsed with the literal word `exec` as a command ("Unknown command:
  exec") and everything after it silently belongs to the first
  invocation -- which reads as the second command doing nothing.
- **An outer `timeout` equal to `--timeout` races the reader.**
  `_exec_one` returns its buffer only after its own deadline, so
  `timeout 30 ... --timeout 30` kills the process just before it
  prints. Give the outer one slack.

And the fixture note, which cost a wrong verdict on an unrelated tool:
**files an interactive session writes into the guest land on the real
`disk.img` and outlive it.** `net_test.py`'s `[server]` phase serves a
directory listing and compares it, so `/tmp/ex.html` left behind by
hand failed it. `make clean-disk && make iso` before believing any
listing-shaped failure -- and then MEASURE with `predates.py` anyway,
because on that occasion the clean image failed identically and the
real answer was "pre-existing".

**2026-09-15: THREE POSITIVE CONTROLS IN A ROW PASSED AGAINST BROKEN
CODE, EACH FOR A DIFFERENT REASON.** The bug was real and fixed; what
took the time was proving the regression check could see it. Worth
reading as a set, because each failure mode is one a session would hit
alone and shrug off:

- **The redundant-fix control.** The fix had two halves (a `reason`
  guard, and a `set_text()` that stops re-texting a field from clearing
  its `active` flag). Disabling ONE left the other satisfying the
  assertion. This repo already records the shape -- "a redundant code
  path makes a positive control lie" -- and it arrives here as two
  deliberate fixes rather than one accidental second path. **Write one
  assertion per defect, and revert one fix at a time.**
- **The control that reverted the INSTRUMENTATION too.** `git checkout
  <file>` to get the broken version also removed the `name.text` line
  the check reads, so the check failed because its oracle was gone, not
  because the bug was back. It looked like a perfect red. **Revert the
  FIX, keep the probe** -- by hand, or from a copy taken after the
  instrumentation and before the fix.
- **`gui move` cannot reproduce a hover.** The bug needed the pointer
  resting over a strip WHEN A DIALOG OPENED. `DebugConsole.move()`
  injects a position for ONE `wm_run()` iteration and the driver snaps
  it back on the next, so the motion never persisted to the open and
  the check passed against unfixed code -- twice, in two different
  shapes. `dbg.warp_cursor(qmp, x, y)` moves the REAL cursor and
  reddened it immediately. This file already said so at three separate
  places and it was still walked into: **if a check involves where the
  pointer IS rather than a click, it needs the real cursor.**

The general rule that covers all three: **a control is only evidence if
the ONLY thing it changed is the fix.** Ask what else that revert
touched -- the other half of the fix, the instrumentation, the timing --
before believing either colour.

**AND A FOURTH, THE SAME DAY: A CONTROL THAT DOES NOT REACH FAR ENOUGH.**
`kctx_test.c` asserts a parked kernel context's stack survives, and the
control for it -- removing the stack switch, so both contexts share one
stack -- had to be iterated TWICE before it reddened anything. First the
coroutine kept its state in globals, so no read ever touched the memory
being clobbered. Then it kept locals, and the test's own calls between
the park and the resume were too SHALLOW to overwrite them. It bites
only with a deliberate 4 KB `stack_churn()` in that gap, which is the
size of the work a real scheduler does there anyway. **Ask not just
"does the control disable the fix" but "does the test then EXECUTE the
thing the fix was protecting"** -- the truncate-test rule (the fixture
never reached the branch) arriving from the control's side.

**2026-09-15: A `make` DURING a running `gui_regress.py` VOIDS THE REST
OF THE SUITE, and it reads as a mass failure rather than as your
mistake.** `iso_guard` compares `build/kernel.bin` against the boot
medium, so a `make all` run to check an unrelated edit makes every
tool launched after that moment refuse with "REFUSING to boot a stale
image" -- 40 tools red, in 1-2 seconds each, with a summary line naming
essentially every tool in the suite. The guard is doing exactly its job
(that is the bug it exists to catch); what is misleading is that the
tools which already ran keep their real results, so the log is a mix of
genuine passes and guard refusals.

Two rules. **While a suite is running, the tree is FROZEN** -- edit
docs if you must, run nothing that writes to `build/`. And **a failure
list that names nearly every tool is a harness or fixture fault, not a
regression**: no single change breaks forty unrelated tools, so read the
FAILURE TEXT before reading the list. The same shape as this file's
other entries -- suspect the instrument when the reading is impossibly
bad.

**And the more general one from the same session: measure the baseline
before believing ANY of it.** Five GUI tools failed; four
(`font`, `fullscreen`, `saver`, `imgview`) failed identically or worse
on a stashed HEAD, and `font` was actually BETTER with the change in
(6 failures against 8). Only `files` was real. Running the failing
tools alone against a stashed baseline took ~20 minutes and was the
difference between one honest finding and five wrong ones.

**2026-09-19: `vm.py start` RETURNS WHEN THE SHELL IS READY, NOT WHEN
THE SERVICES HAVE FINISHED WHAT THEY DO AT STARTUP -- and the gap reads
as an off-by-one in the code under test.** A retention test listed
`/var/log/boot` immediately after boot and found three files where the
setting said two, which looks exactly like a pruning loop that stops one
short. It was `logd` not having reached its prune yet. The tell was that
a second `ls`, seconds later, showed the right answer; the confirmation
was `rm` on the "extra" file answering *no such file* while a plain `ls`
still listed it, i.e. the two commands saw different moments.

Two rules. **Wait on the ARTIFACT, bounded, not on a sleep and not on
`start` having returned** -- poll until the directory holds what it
should, give up after a few seconds, and report what you actually saw.
And **when a count is off by exactly one, check the CLOCK before the
arithmetic**: an off-by-one in a loop and a listing taken one moment too
early are indistinguishable from the failure line alone, and only one of
them is in your code.

**The sibling trap in the same session: the LAST LINE of a captured
reply is the one most likely to be missing.** `log --list` prints the
live boot last, and an assertion counting its rows failed twice on a
capture that ended one line short. Anything asserted from the tail of a
serial capture wants a different oracle -- here, `ls` of the directory,
which is what the assertion was actually about.

**2026-09-19: `vm.py exec` DOES NOT PARSE SHELL OPERATORS, and a `>` or
a `|` in one arrives at the program as an ARGUMENT.** `ls /etc >
/var/tmp/probe` exits 2 with a usage message, because `ls` was handed
`>` and the path as two more operands; `log -u kernel | grep -c foo`
prints `log`'s usage for the same reason. tosh itself lexes both (see
`docs/conventions/shell.md`), so the instinct that they work is right
about the SHELL and wrong about this transport.

It matters because of how it fails: the redirection silently writes
nothing, so a test that creates a fixture that way and then reads it
back gets "no such file or directory" -- which is indistinguishable from
whatever the test was actually checking having destroyed it. That cost a
false failure in a durability test, where "the file is not there after
the reboot" was exactly the bug under investigation.

Write a fixture with a real program (`cp <something> <path>`), and do
any filtering on the HOST from the captured text, not in the guest.

**2026-09-20: `iso_guard` COMPARES SOURCE MTIMES, so EDITING the tree
during a suite run stales the image -- not just building.** The known
hazard was "don't rebuild under a running suite". Touching a `.c` file
does it too: the guard's own message names the source and says the
build "did not run, or it FAILED", which reads like a broken build
rather than like a file saved two minutes ago. Both times it happened
here, the suite reported ~40 tools as `FAIL ... 2s the guest never
started` -- a wall of red that looks catastrophic and means nothing.

The rule that covers both: **once a suite is running, the tree is
frozen.** Queue the edits, or work in the scratchpad.

**AND THE COROLLARY FOR `predates.py`: while it runs, the tree is
SOMEONE ELSE'S.** It stashes the working tree to build HEAD, so every
file read during that window shows HEAD's content. Half a diagnosis was
drawn here from a `settings.c` that "still called `sys_setting`" -- it
did, at HEAD, because the conversion was sitting in the stash. If a
file's content contradicts an edit you are certain you made, check for
a running `predates.py` before disbelieving yourself.

**It also leaves the tree's DELETIONS unstaged** when it restores.
`check_docs.py` then walks `git ls-files`, hits a header the change
removed, and dies with a `FileNotFoundError` rather than a check
failure. `git add -A` before the gate.

**2026-09-20: A GUI SUITE'S FAILURE COUNT IS NOT COMPARABLE ACROSS JOB
COUNTS.** `settings_test` reported 3 failures at `-j12`, 3 at `-j2`, 1
at `-j3` and 1 at `-j1` -- the same tree each time. Two of its checks
stage a spinbox change and assert the app logged it, and they lose that
race under contention. A tool judged against HEAD must be run at the
SAME job count as HEAD was, and a tool judged at all is worth one run at
`-j1`: the 1-failure reading matched HEAD exactly and settled a question
three concurrent runs had only muddied.

**2026-09-21: A RUN PIPED THROUGH `tail -N` HAS NO PROGRESS, AND A
HEALTHY 25-MINUTE SUITE LOOKS EXACTLY LIKE A HANG.** `gui_regress.py
... 2>&1 | tail -70` in the background buffers everything until the
process exits, so the task's output file stays EMPTY the whole time.
The maintainer asked "is that task stuck?" twice before anyone went
looking. The progress signal was there all along -- `--logs DIR` writes
one file per tool as it finishes, so `ls -t` on that directory answers
"how far, and when was the last one" in one command. **Check the
artifact the run is already producing before reporting a run as
healthy OR as stuck**, and prefer letting a background run write its
full output to the task file over trimming it at the pipe. The same
mistake destroyed a `remote.py flash` file list on 2026-09-16
(`docs/bugs.md`), which is the second time a pipe has eaten the
evidence rather than the bug eating it.

**And the reading error beside it: `grep -i FAILED` MATCHES `0
failed`.** A summary built that way reported 44 of 44 tools as failing
when 5 were. Match the COUNT (`[1-9][0-9]* failed`) or parse the pair,
never the word -- a harness that over-reports failures burns the same
trust as one that under-reports, and this one did it in front of the
maintainer.

**AND THE RULE ABOVE ABOUT FREEZING THE TREE WAS ALREADY WRITTEN TWICE
WHEN IT WAS BROKEN AGAIN** -- 2026-09-15 and 2026-09-20, the second one
the day before. Two suite runs were voided in one session: the first by
a `make` for a positive control, the second by editing a `.h` and a
`.c` while it ran. Knowing the rule is not the same as checking it, and
the check is one question at launch: **what am I about to touch, and is
a suite running?** What `iso_guard` actually watches is narrower than
"the tree" and worth knowing exactly, because a needlessly broad rule
is one people work around: `.c`, `.h`, `.asm` and `.ld` under
`kernel/`, `apps/` and `userland/` (`SOURCE_TREES`/`SOURCE_SUFFIXES`).
Editing `docs/` during a run is genuinely safe.

**2026-09-23: A DISK IMAGE'S HOST FILESYSTEM IS PART OF THE MEASUREMENT.**
The scratchpad is TMPFS, so a `cp --reflink` "copy of disk.img" made
there is a file whose `fsync` is free -- and every guest cache flush is
a host fsync. The in-kernel suite took 16 s on such a copy and 37 s on
the real `disk.img` (btrfs) with the same build, and HEAD did the same
(16 s / 36 s): a "regression" that was only the two sides sitting on
different filesystems. Before comparing two runs of anything that
touches the disk, check `df -T` on both images.

**2026-09-24: `pgrep -f` MATCHES THE SHELL THAT RUNS IT -- as a waiter AND
as a killer.** CLAUDE.md already forbids waiting on a process by name; the
same session then (1) left an `until ! pgrep -f "..."` waiter spinning for
an hour because its own command line contained the pattern, and (2) killed
its own cleanup shell with `for p in $(pgrep -f "..."); do kill $p` (exit
144, halfway through). Find a PID from a pidfile, `$!`, or `ps` filtered
with `grep -v grep` AND a check that the PID is not `$$` -- or put the loop
in a script file, whose command line does not contain the pattern.

**And a setting applied to a guest from outside must be READ BACK.** A
QMP `block_set_io_throttle` sent right after boot silently did nothing on
one run in two, and that run's "slow disk" numbers came from a fast one.
`info block -v` shows whether it took.

**2026-09-29: A RUNNING `gui_regress` OWNS THE TREE UNTIL IT ENDS.** Twice
in one session a full run came back with ~20 tools "failed" in 2 s each,
every one `iso_guard: REFUSING to boot a stale image`: once after a
`make all` for the bare-metal laptop, once after editing two WM sources
to fix code-review findings. Each tool boots a fresh copy when its turn
comes, so anything that makes the image stale mid-run stops every later
tool -- correctly, and at the cost of the whole run. Build for the
laptop, and land review fixes, before or after a suite, never during it.

**2026-09-30 (fragmentation, TCP window scaling and congestion control,
speedtest, one-shot boot entries). Seven ways a check lied or nearly
did.**

- **A KTEST THAT RUNS LONG LEAKS ITS FAKE PEER TO SLIRP, AND SLIRP ANSWERS
  WITH A VALID RST.** `net_test.c`'s fixtures address a made-up peer on
  QEMU's user network; a test that streams 256 KiB runs long enough for
  the real receive path to deliver SLIRP's reset -- whose sequence is
  exactly `rcv_nxt`, so it is BELIEVED -- and the connection is
  `ECONNRESET` mid-test. Capture the transmit for the whole of a long
  test, not just around the frame you read.
- **A FAILED KTEST LEAKS ITS CONNECTION BLOCK AND FAILS EVERY LATER TCP
  TEST WITH `connect rc -28`.** A positive control that breaks the sender
  hides the other controls behind pool exhaustion. `finish_close()` now
  ends with a valid RST, and new tests clean up BEFORE they assert -- a
  control reddens only the test it aims at.
- **A THRESHOLD CAN PASS THE BUG IT GUARDS.** "The scaled window exceeds
  65535" stayed green with the window unscaled, which over-promises the
  ring eightfold. Assert the DELTA instead (queue 1460 bytes, the window
  shrinks by 1460) -- the positive control is what found this.
- **A THROUGHPUT NUMBER IN A VM IS OFTEN THE SINK'S.** `wget -O /tmp/x`
  measured `/tmp`'s ramfs (2 MB/s, falling as the file grew);
  `speedtest --url`, which discards, measured 207 Mbit/s on the same
  path. And SLIRP on loopback has no latency, so a window change shows
  ~20% there and nothing about a real path.
- **THE DEBUG CONSOLE'S SHELL HAS NO REDIRECTION**, so `echo x > /etc/f`
  through `vm.py exec` writes NOTHING and says nothing -- and the next
  line of a test acted on a file that did not exist (it rebooted the
  guest). Put files in with `vm.py put` and `cp`, and read them back.
- **QMP `click_at` MISSED A 19-PIXEL MENU ROW** (the pointer is
  accelerated); a context menu that "closed without acting" was never
  clicked. `DebugConsole.click()` at `ctxmenu_row(label)` is exact.
- **GRUB'S MODULE DIRECTORY SATISFIES A MISSING CORE MODULE.** `disk.img`
  carries `/boot/grub/i386-pc/`, an installed laptop does not, so a VM
  test of anything GRUB loads must DELETE the module first or it passes
  for a reason no laptop has (`boot_entry_test.py` removes
  `loadenv.mod`; with loadenv also out of the core, it goes red).
