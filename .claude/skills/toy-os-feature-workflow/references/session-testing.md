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
root-caused a real DMA bug before. **`make run-kvm`/`vm.py --kvm`**
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
