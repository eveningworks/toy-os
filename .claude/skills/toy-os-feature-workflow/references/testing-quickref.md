# Testing quick reference

Step 4 of the toy-os feature workflow, in full: how to test a change here,
from the boot smoke test up to QMP GUI checks, and the harness traps past
sessions paid for. Moved verbatim out of SKILL.md (2026-10-05) so the
playbook stays short; `session-testing.md` holds the longer lessons.

**Build and test -- scale the testing to what changed.**
   - **Check in order of cost.** `tools/boot_smoke_test.py` answers
     "does it still boot" in seconds. `make test` (the in-kernel suite,
     `tools/ktest_run.py`) answers "does it still work" and exits
     non-zero, so it's a real gate. `python3 tools/vm.py exec "<shell
     command>"` runs anything in the booted OS and returns its output as
     TEXT -- use it instead of a screenshot whenever the answer isn't
     about pixels. Only reach for QMP (below) when it genuinely is:
     widget layout, rendering, mouse behaviour, window chrome.
   - **Add a test, not just a manual check.** A `KTEST("suite", "name")
     { ... }` block in a `*_test.c` next to the code (see
     `kernel/include/kernel/ktest.h`) registers itself -- no registry,
     no Makefile edit. `kernel/include/kernel/fault_inject.h` can fail
     the next N ATA writes/reads or `kmalloc` calls, which is how error
     paths get covered. Two rules learned the hard way: tests run in the
     LIVE kernel (don't assume a pristine heap or an empty filesystem),
     and a test must establish its own preconditions rather than
     inherit them from whatever ran before -- a shared temp path plus an
     unchecked cleanup produced a confusing CI cascade.
   - `tools/preflight.sh` runs the standard build+test loop
     (`make clean && make all && make iso`, then `check_layout.py`,
     `check_dispatch.py`, `check_widget_ops.py`, `check_key_routing.py`,
     `boot_smoke_test.py`, `ktest_run.py`, `usertest_run.py`, then a
     `git status --short` summary) in one command -- `make verify` is the
     same thing without the git summary -- use it as the default "am I
     safe to keep going / safe to deliver" check instead of chaining the
     steps by hand.
     `--skip-clean` skips the initial `make clean` for a faster
     iteration loop while still testing the fresh boot path.
   - Anything touching rendering, input, or window/widget behavior:
     the boot smoke test doesn't prove a button is in the right place --
     follow up with real QMP GUI testing via `tools/qmp_test.py`
     (`QMPSession`: `goto`/`click_at`/`drag`/`send_key`/`send_text`/
     `combo`/`screenshot`, plus the module's own docstring for the
     mouse/keyboard/display gotchas already paid for by past sessions --
     read it, don't rederive it). `tools/gui_flow.py` builds named,
     composable click-flows on top of `QMPSession` (`GuiFlow`:
     `enter_gui()`, `open_app(name)`, `run_system_action(label)`,
     `screenshot_named()`) so a common flow like "enter GUI, open
     Notepad" doesn't need re-deriving pixel math each session -- prefer
     it over hand-rolled coordinates when the flow it needs already
     exists. It finds menu rows BY LABEL from the kernel's own geometry
     -- it used to carry an `APP_ORDER` list mirroring the WM's desktop
     entries, and when one was added and the list was not, `open_app`
     silently opened the wrong app (the menu's origin is derived from
     the list's LENGTH, so a stale list breaks the position as well as
     the index). Prefer `DebugConsole.open_app(name)` outright unless
     the test genuinely wants the menu exercised; it sends `gui open
     <name>` with no pixels involved. Pass `console=` if you already
     hold a `DebugConsole` -- two of them on one serial socket steal
     each other's replies.
     `GuiFlow(qmp_port=4445)` builds its own internal `QMPSession` --
     don't construct a `QMPSession` yourself and pass it in (that's a
     confusing `TypeError`, not an obvious "wrong argument" one); reach
     the session it already made via `flow.session` for anything
     `GuiFlow` doesn't wrap directly (`flow.session.screenshot(...)`,
     `.recalibrate()`, etc). `tools/shell_flow.py`'s `ShellFlow` is the
     same idea as `GuiFlow`, but for the PHYSICAL (pre-`gui`) shell:
     `run_command(cmd, subdir=...)` types a full command -- spaces,
     hyphens, underscores, and a few other punctuation chars `send_text()`
     can't handle on its own -- presses Enter, and screenshots, instead
     of hand-interleaving `send_text()`/`send_key('spc')`/
     `combo(['shift','minus'])` character by character (a dropped space,
     a hyphen typed where an underscore was needed, both real mistakes
     from doing this by hand in the session that built the tool). It
     returns a screenshot path, not parsed text -- this kernel's console
     picks a framebuffer glyphs-as-pixels backend whenever GRUB provides
     one (the normal case for this project's QEMU flags), so there's no
     legacy-VGA-text-buffer memory-read shortcut to plain text. And
     launch QEMU via `tools/qmp_test.py`'s `launch_qemu_cmd()` (or copy
     its returned command verbatim) rather than hand-rolling a
     `qemu-system-x86_64` line -- a hand-typed `-qmp` port that doesn't
     match `QMPSession`/`GuiFlow`'s default (4445) fails as a flat
     `Connection refused` with no obvious QEMU-related cause. Don't
     chain a `pkill` into the same shell call as what follows it either
     (`pkill -f qemu-system-x86_64; rm -f qemu.pid; ...`) -- it exits 1
     when nothing matched (the common case), which trips `errexit` and
     aborts the rest of the chain with a spurious `exit code 144`; run
     it as its own call. And never `pkill`/kill-by-pattern across EVERY
     `qemu-system-x86_64` process if there's any chance the user has
     their own `make run` QEMU open (an interactive SDL window, not a
     QMP-headless one) -- matching by process name alone can't tell
     them apart. Track and kill only the PID your own launch wrote to
     its `-pidfile`; if sweeping stale instances, `ps aux | grep
     qemu-system` first and check the command line (`-qmp`/`-vnc`
     means yours, `-display sdl` means the user's).
   - **ASK the user to close their QEMU before `make iso`/`make verify`/
     `preflight.sh`** (standing request, see CLAUDE.md). Testing against
     a copy dodges the write lock but NOT the re-seed: those targets
     rewrite the real `disk.img` whatever a test is pointed at, leaving
     an open VM on a stale view with nothing failing loudly. Editing and
     `make all` need no such ask.
   - **If the user might have their own QEMU open, test against a COPY
     of `disk.img`, not the real one.** Two separate hazards, both hit
     in one session: QEMU takes a write lock, so your headless launch
     dies with `Failed to get "write" lock` while theirs holds it; and
     `make iso` re-seeds `disk.img` on every run, which rewrites the
     filesystem underneath a VM that's already booted from it. Both
     go away with `cp --reflink=auto disk.img $SOMEWHERE/test.img` and
     then `vm.py --disk <copy>` / `launch_qemu_cmd(disk=<copy>)` --
     also the right move any time a test needs specific files on disk,
     since it leaves the real image untouched. Note `vm.py` and
     `launch_qemu_cmd()` also take `--qmp-port`/`--vnc` (and
     `qmp_port=`/`vnc_display=`), which you need if a second instance
     is already using the defaults.
     Test the actual new behavior, not just that the app opens: type the
     new characters, click the new widget, reboot if the change is
     supposed to persist. A behavior that "should obviously work" from
     reading the code is exactly the kind of claim this repo has caught
     being wrong before (see `references/questions-that-worked.md`'s
     note on the signed-char bug that six code-reviewed gates still
     missed one instance of) -- testing the real behavior beats
     re-reading the code you just wrote.
   - **Read a helper's signature before calling it, especially one
     taking coordinates.** `qmp_test.py`'s `drag()` used to take a
     destination only, so a call that read as four coordinates bound
     `hold=700`/`settle=300` SECONDS and slept for sixteen minutes
     without failing. It takes both points now, with keyword-only
     timings -- but the general hazard stands for any helper whose
     positional arguments can absorb a mistake as a plausible value.
   - **For a rendering change, assert on PIXEL VALUES, not on how the
     screenshot looks.** A cursor that "looks fine" at 1280x720 can be
     a 2-pixel sliver or a block whose glyph contrast has collapsed --
     both happened in one session, and both looked plausible in the
     PNG. Reading the actual values (`Image.open(p).convert('RGB')`,
     then print a small brightness map of the cell) turned each into a
     number: block at 119 vs glyph at 208, or 2 tinted columns out of
     8. Cropping the region and scaling it up with `Image.NEAREST`
     before looking is the other half of this -- a 5x zoom of one cell
     answers in a glance what a full-screen screenshot cannot.
   - **A HOVER STATE NEEDS `DebugConsole.hover_frames()`, never `gui
     move`.** Injected input overrides the pointer for ONE `wm_run()`
     iteration and then the real mouse takes over, so a capture taken
     after it photographs the screen with nothing hovered -- a working
     hover reported as dead, with the cursor confirmed at the right
     coordinates. The helper warps the REAL cursor (confirmed against
     the WM: `goto()` is open-loop and the WM ACCELERATES the delta) and
     returns two settled frames; `changed_rows()` compares a band for a
     control whose rows have no reported geometry. Assert the BAND --
     one, containing the pointer, no taller than a row -- and hover a
     row that is NOT selected, since selection outranks hover.
   - **ONE `gui_regress` AT A TIME**, and it refuses a second concurrent
     run now (an flock). Two suites share VM slots from 0 up, and the
     collision surfaces minutes later as `BrokenPipeError` in whichever
     tool was mid-command rather than as a port clash -- seven tools
     "failed" that way in one run, none of them at fault.
   - **A blinking element needs several samples across its period.**
     Sampling a 500ms blink at 250ms intervals aliased into "it never
     blinks" twice in a row and sent me looking for a bug that wasn't
     there. Take 5-6 shots at an interval that doesn't divide the
     period, and keep the frame where the thing is actually drawn.
   - **Press Enter before typing the next command.** A QMP loop that
     typed `cursor beam` without submitting the previous line appended
     it into that line instead, so four "different" style screenshots
     were all the same style -- and it looked exactly like the feature
     being broken. If consecutive runs produce identical output,
     suspect the harness before the kernel.
   - `send_text()` silently drops uppercase letters (it handles
     lowercase and digits only). A test that types "Hello World" gets
     "ello orld" and a confusing screenshot; use `combo(['shift', c])`
     or stick to lowercase in test input.
   - **Screenshots are a testing tool, not a deliverable** (changed
     2026-08-15 at the maintainer's request -- the saved artifacts
     weren't being used and producing them slowed the loop down). Take
     as many as a check needs, into a scratch directory; do NOT save
     them into `screenshots/`, which is closed. For evidence, prefer
     what a reader can check rather than interpret: pixel values with a
     control point (`tools/pixel_probe.py`) and `gui_regress.py`'s
     pass/fail table. Show the user an image when SEEING it is the
     answer -- a layout to look at, a rendering question a number can't
     settle -- not to prove a check passed. `tools/screenshot_diff.py`
     (Pillow-based pixel diff, default 0.2% threshold, optional
     `--out` highlight image) is still useful for confirming a
     rendering change didn't regress an unrelated screen.
   - **Check that the thing you're verifying is actually reaching the
     place you're looking.** Console scrollback appeared to work --
     built, ran, PageUp responded -- while showing an empty history,
     because `klog_write()` never wrote to the console at all; the boot
     messages it was supposed to recover only ever went to the serial
     port. A feature that plausibly responds is not a feature that
     works; check the data got there, not just that the code ran.
   - **A change whose full verification is genuinely slow (a multi-GB
     disk stress test, anything bottlenecked on emulated PIO/DMA
     throughput rather than QMP interaction) doesn't need to run to
     completion in-session to ship.** Measure the real rate at a small,
     fast scale first (e.g. a `stress 100` -- 100MB -- pass, timed), then
     be honest in the commit message and `docs/roadmap.md` about what the
     extrapolated full-scale time would be and that it wasn't run. Real
     example: a `stress <mb>` shell command (real, non-sparse
     write/read/verify over `fs_write_range()`/`fs_read_range()`) was
     built and verified correct at 100MB (72s) -- extrapolating that
     rate, the full 8GB target would take ~100 minutes, far past what an
     interactive QMP session can wait out. Shipping the verified-correct
     tool plus an honest "not run at full scale yet, here's why and
     here's the exact command" roadmap note is a real, useful delivery;
     silently attempting the full run (and likely getting cut off
     mid-test) or silently skipping the feature entirely are both worse
     than saying so plainly.

