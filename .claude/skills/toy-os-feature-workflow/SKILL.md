---
name: toy-os-feature-workflow
description: >
  Use this skill whenever the user asks for a new feature, fix, or change to
  toy-os (their x86-64 hobby OS, repo at ~/CodingProjects/toy-os -- reached
  either through the Cowork device bridge or directly on the user's own
  machine, see the skill body for how to tell which). Triggers on things like "add support for X",
  "can we improve Y", "let's build Z", or any bug report against the OS,
  shell, GUI, or filesystem -- even if the user doesn't say "toy-os" by name,
  treat any request touching kernel/, apps/, userland/, or the shell/GUI/
  filesystem behavior of this OS as toy-os work. This is the end-to-end
  playbook -- research the code first, present real implementation choices
  before writing anything, build and QEMU-test with screenshot proof, write
  the docs in the repo's established style, and ship the change to both the
  user's real checkout and GitHub-ready commits. Do not start editing
  kernel/app code for this repo without consulting this skill first.
---

# toy-os feature workflow

toy-os is a hand-built x86-64 kernel the user has been growing over many
sessions, each of which starts with no memory of the last one. The thing
that makes each session feel like a continuation of the same disciplined
project, instead of a fresh contractor improvising, is following the same
sequence every time. This skill is that sequence.

`CLAUDE.md` (repo root) is the reference for *mechanics* -- file layout,
build commands, git gotchas, the QMP testing tool's API. Read it before
your first edit if you haven't already; this skill won't repeat it.
What this skill covers instead is the *order of operations* and the
Cowork-specific tool choreography CLAUDE.md doesn't (and shouldn't) --
things like when to pause and ask, how to test what you built, and how
to actually get a change from your sandbox onto the user's machine.

**Re-read `CLAUDE.md` and `docs/decisions.md` fresh each session --
don't assume the conventions this skill describes are still current.**
This project's own workflow has changed under it before (the versioning
scheme this skill originally documented -- a per-change build-number
bump with a git tag on every push -- was replaced with semver + a
`-dev` suffix in a different session, without this skill knowing until
someone checked; more recently `apps/widgets.h`/`.c` -- referenced
below and by name in earlier revisions of this skill -- was split out
into `apps/ui/*`, one widget per file).

**2026-08-13 changed a lot of what this skill describes.** In short:
the kernel is now split into subsystems (`kernel/arch/x86_64/`, `mm/`,
`proc/`, `fs/`, `lib/`, `drivers/`, `core/`, `test/`) rather than
`core/` + `drivers/`; headers are split by audience under
`kernel/include/{api,abi,kernel}/` with the boundary ENFORCED by
include paths (an app including a kernel-internal header fails to
compile); source discovery is recursive so a new directory needs no
Makefile edit; there is an in-kernel test suite (`make test`, `ktest`);
and `tools/vm.py` can run shell commands headlessly and return TEXT,
which is usually a better check than a screenshot. The physical console
also has scrollback now (PageUp/PageDown) and the kernel echoes its boot
log to the screen, so "what did it print?" no longer means reading the
serial log. Read `CLAUDE.md`, `kernel/README.md` and
`kernel/include/README.md` before editing.

**Later the same day, three more things landed that change how you
should write code here:**

- **There is a shared toolkit in `kernel/lib/`, and you are expected to
  reach for it.** `knum.h` (numbers <-> strings), `kfmt.h`
  (`k_snprintf`, `vga_printf`, `klog_printf`), `kpath.h` (path
  join/normalize/resolve), a grown `string.h`, and `klineedit.h` (the
  line editor both command lines share). All reachable via `kapi.h`,
  all with KTESTs. **Do not hand-roll a digit loop, a hex formatter, a
  digit-parsing loop or a path join** -- that instinct is exactly what
  produced the nine/ten/six/three copies the toolkit replaced, and the
  most recent copy was added by a session that had just been told about
  the others. Two conventions to match if you extend it: a formatter
  that doesn't fit its buffer writes NOTHING rather than a truncated
  value, and a parser rejects rather than guesses.
- **`strace <binary>` exists**, and it is often the fastest way to
  understand what a `/bin` binary is actually doing -- one decoded line
  per syscall, and the same lines land in `dmesg`, so
  `vm.py exec "strace file_test"` gives you assertable text with no
  screenshot involved. Reach for it before adding temporary
  `klog_write()` calls to a syscall handler.
- **The console cursor saves the pixels it covers**, and its style
  (`translucent`/`underline`/`beam`/`reverse`) is a `/etc/toyos.conf`
  setting changed with the `cursor` command. If you touch console
  rendering, don't reintroduce "erase the cursor by filling its cell
  with the background colour" -- that assumption is what made the
  cursor eat characters once the shell could edit mid-line.
- **Ctrl and Alt reach apps as control codes and an ESC prefix**
  (`Ctrl-A` = 0x01, `Alt-B` = ESC then 'b'), not as `KEY_*` codes --
  which matters when QMP-testing anything keyboard-related: use
  `QMPSession.combo(['ctrl','a'])`, and remember a lone Esc is
  ambiguous with the start of a Meta sequence by design. Line editing
  itself belongs in `kernel/lib/klineedit.c`'s keymap, not in one front
  end -- adding a key to only the shell or only the Terminal
  re-creates the divergence that whole file exists to prevent.

**2026-08-15: the ring-3 GUI has a toolkit now -- do NOT hand-roll a
client.** The whole `docs/uapp-design.md` plan was built this day
(stages 0-4). What a session needs to know:

- **The stack has names.** **TWP** = the Toy Window Protocol
  (`abi/win_proto.h`, the client<->server contract), **TWS** = the Toy
  Window Server (`kernel/proc/win_server.c` + `apps/wm/wm_client.c`),
  **Toykit** = the ring-3 toolkit (`userland/ui/`). Roughly Wayland, its
  compositor, and GTK. Use the names.
- **A new ring-3 GUI app is a `.c` file in `userland/gui/` and NO
  Makefile edit.** `userland/` is split by role (`rt/ ui/ lib/ gui/
  bin/ tests/`); the directory decides that it is a program and where it
  seeds. Everything links `libuapp.a` with `--gc-sections`, so an app
  names no objects.
- **Write it as a `struct uapp_desc` plus callbacks** (`ui/uapp.h`), not
  a loop: `uapp_run()` owns the TWP handshake and the event loop. Every
  callback is optional with a library default -- that is what lets TWS
  gain features without editing apps, and it is tested (shipping resize
  touched zero lines in the clients that did not opt in).
- **Write no coordinates.** `uui_layout` (column/row/grid, nestable,
  font-derived margins) places widgets and the window sizes itself from
  the content. `uui_custom` lets an app's own drawing take part.
- Resize (`UAPP_RESIZABLE` + a min size), focus (`uapp_focused()`) and
  the wheel (`desc.on_wheel`) all arrive for free.
- **Widgets: one `uui_widget_ops` per widget**, all slots optional. A
  widget reports a `natural_size()` (the preferred MINIMUM; 0 means "no
  preference") and carries its own geometry via `set_geometry()`.
- **Client windows can now fill the screen** (1280x720). They were
  capped at 640x480 by TWO constants, and only one was documented:
  `WIN_CLIENT_MAX_W/H` *and* `WIN_BUFFER_STRIDE`, the per-window slot in
  a client's address space, which bounded a window to 2 MiB of pixels
  whatever the first said. Both were raised. **Still contiguous
  (`pmm_alloc_contiguous`)**, so a fragmented allocator can still refuse
  a big window, and a refusal is SILENT by design -- it is a normal
  protocol outcome, indistinguishable from a client declining. Growable
  (non-contiguous) buffers are on the roadmap under M41.

**2026-08-15 (later): `geom.h` has 3D in it now** -- `geom_pt3`,
`geom_rotate3` (yaw/pitch/roll, applied in that FIXED order because
rotations do not commute), `geom_project` (perspective) and
`geom_transform3` (scale+rotate+project a model, reporting each vertex's
rotated depth for shading). Deliberately not a 3D engine: no matrices,
no faces, no depth buffer. A model is points plus the caller's own edge
list -- the Shapes demo's cube is 8 corners, 12 edges and no arithmetic
of its own. Two things it teaches: **`fx_round()` takes a FIXED-POINT
value**, and applying it to an already-integer result silently shifts it
16 bits to zero (it shipped as "the shading isn't very strong" rather
than as an obvious bug); and when a demo gains a mode, give it a
**scene toggle with a logged state** rather than drawing everything at
once -- it keeps the canvas legible and gives the test a named state to
assert on.

**2026-08-15 (last): the GUI grew chrome, closing, and liveness. Four
things a session should know before touching any of it.**

- **Ring-3 apps have a MENU BAR and a STATUS BAR now** (`uui_menubar`,
  `uui_statusbar` in `userland/ui/`, first caller the ring-3 Notepad,
  which lost its toolbar). A menu is a **declared const tree**, not a
  built one -- Toykit has no allocator, so `UUI_MENU`/`UUI_SUBMENU`/
  `UUI_MENU_SEP` arrays point at each other and nest to any depth. Per
  item checked/disabled state is **asked for** through an `item_flags`
  hook, never stored in the tree, so there is no "refresh the menu" step
  to forget. Two rules: a menu bar **opens on PRESS** (the documented
  exception to commit-on-release; the ITEM still commits on release),
  and a popup is clamped to a **bounds rect the app passes in** -- the
  window today, the screen when `WIN_REQ_POPUP` lands, which is a
  one-rect change rather than a rewrite.
- **Esc closes nothing. Alt+F4 does, and the WM handles it** -- it never
  reaches the app, as on Windows/KDE. All THREE user-facing closes (the
  X, the context menu's Close, Alt+F4) go through one
  `wm_request_close()`, which asks a ring-3 client and can be refused
  from `uapp_desc.on_close`. Route a fourth through the same function:
  repeating the client check is exactly how the context menu drifted
  into seizing a window instead of asking, destroying clients that had
  explicitly declined.
- **"Not responding" is a PING, not a timeout** (`WIN_EV_PING` /
  `WIN_REQ_PONG`, i.e. xdg_shell's). The reason is worth carrying to any
  similar problem: a client that REFUSES to close and one that is WEDGED
  are the same observation to a timer, so a timeout must either kill
  apps that said no or abandon apps that are hung. Toykit answers pings
  inside its loop, so no app contains ping code and an app stuck in its
  own callback correctly fails to answer. Force Quit kills the PROCESS
  (`scheduler_kill()`); dropping the window alone leaves a process
  drawing into an unmapped buffer.
- **`MAX_PROCS` is 4, and a zombie holds its slot until someone polls
  it.** A Start-menu launch had no poller, so the desktop silently
  stopped launching anything after four opens. Fixed (the WM reaps what
  it launched), but the shape recurs: if you add a path that spawns,
  ask who reaps it.

**2026-08-15 (last, really): ring 3 has the toolkit under the C names.
Four things, and the third is a class of bug worth carrying anywhere.**

- **Do NOT hand-roll a `my_strlen` or a digit loop in a ring-3 program.**
  `#include "lib/string.h"` for `strlen`/`strcmp`/`strlcpy`/`mem*`/the
  `ctype` handful and `#include "lib/stdio.h"` for `snprintf`. Not a
  second implementation -- the same `k_*` code compiled a second time
  into `libuapp.a`, so the ring-3 and kernel versions cannot diverge.
  For a fixed-width number reach for `knum.h`'s `k_utoa`/`k_htoa`
  directly: kfmt's printf has zero-pad widths for numbers but no `*`
  width and no left-justify. Still NOT a libc -- no `malloc`, `FILE`,
  `printf`, `errno` or TLS (M24). Two `/bin` programs had each grown
  their own `my_strlen` + decimal loop + hex loop, with a comment in
  each calling it deliberate, which it was: the toolkit genuinely could
  not be linked into a ring-3 ELF until `kfmt.c` was made freestanding.
  Check whether a "deliberate duplicate" comment is still TRUE before
  copying its reasoning.
- **`kfmt.c` must stay freestanding, and nothing enforces it.** Its two
  kernel sinks live in `kfmt_print.c` for exactly this. Adding one
  `#include "klog.h"` to `kfmt.c` silently takes `snprintf` away from
  every ring-3 program, with no error at the point of the mistake.
  A new conversion goes in `kfmt.c`, a new sink next door.
- **Three failure modes here were all SILENT, which is the pattern.** A
  header named `string.h` that includes `"string.h"` finds ITSELF (a
  quoted include searches the including file's directory first) -- the
  guard makes it a no-op and every symbol is then undeclared. An archive
  cannot hold two members named `string.o`, and `ar` stores basenames
  only, so a second one links fine right up until the two define the
  same symbol. And a real `memcpy` that calls `k_memcpy` recurses
  forever if GCC rewrites `k_memcpy`'s loop into a `memcpy` call --
  which LINKS, and blows the stack at runtime
  (`-fno-tree-loop-distribute-patterns` in `USERLAND_CFLAGS` is the
  fix, and `nm -u` on the object is how you check). When adding
  anything to the link, ask what would happen if it half-worked.
- **`python3 tools/usertest_run.py` runs the `/tests` ring-3
  diagnostics** and is in `preflight.sh`. Before it, nothing ran a plain
  `/tests` binary except a person typing `run <name>` -- KTESTs run
  inside the kernel and `gui_regress.py` covers only windowed clients.
  Read its `EXCLUDED` list before adding to it: a test that faults on
  purpose, blocks on serial, needs a desktop, or needs a parent to spawn
  it will fail in a way that says nothing about the code under test.
  Also `check_layout.py` now warns about orphaned seeded files -- and
  its first run found all four ring-3 GUI apps still in `/tests` long
  after they moved to `/bin`, because `sync` is additive and nothing had
  ever checked whether that documented trap had already sprung.

**2026-08-15 (filesystem day): the shell has a NAME, and TFS3 has a
second on-disk layout. Five things.**

- **The shell is `tosh`** (t + OS + h). The name covers the shell
  LANGUAGE, which has two front ends -- `apps/shell.c` in the kernel
  and `userland/lib/tosh.c` in ring 3 (renamed from `ush.c`, symbols
  `tosh_*`). It is NOT the terminal; `uterm` stays the terminal
  emulator. `docs/decisions.md` records the collisions it dodges, so
  don't relitigate the name.
- **`mv` and `truncate` exist now** (`fs_rename()`/`fs_truncate()` in
  `fs.h`, both backends). Rename refuses an existing destination
  deliberately -- there is no atomic replace -- plus a directory into
  its own subtree, and the root.
- **A new TFS3 operation must COUNT ITS JOURNAL CREDITS.**
  `txn_begin(n)` reserves n distinct metadata blocks up front (jbd2's
  discipline) and refuses before changing anything; count the WORST
  case. An insert that may grow a directory has to be staged FIRST,
  since the grow commits its own transaction and can only do that
  while nothing else is staged.
- **Anything that stops referencing a block commits the pointer change
  BEFORE freeing the bit.** A crash between costs a leak (fsck
  reclaims); the other order hands a live file's blocks to the next
  allocation. This is why truncation is two-phase and keeps the
  straddling pointer tables in memory across the commit -- read
  `docs/decisions.md`'s truncation entry before touching it.
- **Shipping a v2 of a format can make v1 untestable, which is worse
  than untested.** Once `format` wrote v2, nothing in the repo could
  produce a v1 image, while the kernel still mounted them (it must:
  a probe answering "not mine" hands the image to the blank-disk
  policy, which formats it). Fixed with `tfs3_writer.py format
  --fs-version 1` + `tools/tfs3_v1_test.py`. **When you add a format
  version, a capability tier or a compatibility path, ask immediately
  what can still PRODUCE the old one.**

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

**2026-08-15 (later): three lessons that each cost a real bug.**

- **A window operation the WM performs itself must still go through the
  client's handshake.** Maximize set `w->w`/`w->h` directly at both call
  sites (title-bar button and context menu, one copy each), which for a
  ring-3 client meant full-screen chrome around a 640x400 buffer with
  bare desktop filling the rest. Nothing tested it because nothing had
  ever maximized a client window. When a feature exists for one kind of
  window, ask what it does to the other kind.
- **A widget's behaviour that an app re-implements WILL drift.** The
  ring-3 Notepad passed `grab_offset_in_thumb = 0` to the scrollbar's
  drag maths, so the thumb leapt to put its top under the cursor and the
  bar was grabbable only by its top edge. The header documents the
  parameter; both kernel-side callers use it correctly. The user found
  it by using the desktop, after a green suite. The general fix is in
  `docs/gui-guidelines.md` now -- **"Scrollbars: what a real one does"**,
  eight points every real toolkit implements identically, plus
  `tools/scrollbar_test.py` asserting four of them by measuring the
  thumb's PIXELS. Write the spec down when you find a control whose
  correct behaviour lives only in one implementation.
- **A positive control tells you which check is load-bearing, not just
  that the suite works.** Re-breaking the scrollbar grab turned exactly
  ONE check red; the drag-back-and-return check stayed green, because
  the jump slams the thumb into the end of the track and returning from
  a clamped position looks correct. Run the control and note which
  checks did NOT fire -- those are the ones that would not have caught it.

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

- **`gui <sub>` on the serial debug console** (`apps/wm/wm_debug.c`),
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

**Two documents now govern whole areas, and both are enforced, not
advisory. Read the relevant one BEFORE writing code in its area:**

- **`docs/filesystem-layout.md`** -- what lives where on the OS's own
  disk. `/bin` holds real programs only; the test binaries live in
  `/tests`; bundled data goes under `/usr/share/<category>/`; config in
  `/etc` via `etc_config_*`. `tools/check_layout.py` compares the built
  image against that file's table and fails `preflight`/CI in either
  direction, so a directory can't appear without being described first.
  It also records the budget that actually constrains layout: 256
  filesystem records INCLUDING directories, and 64-byte full paths.
- **`docs/gui-guidelines.md`** -- how anything drawn must look and
  behave. The four `enum ui_state` states and their flat (non-bevelled)
  rendering, press-then-commit-on-release, the `on_hover` contract, and
  when feedback is NOT wanted (an action whose result is already
  visible doesn't also need a confirmation).

**Three traps that cost real time this session, all of which look fine
until tested:**

- **`gfx_draw_string()` does not clip.** Use
  `gfx_draw_string_clipped()`/`gfx_text_width()` for anything in a
  fixed box. This produced the identical overlapping-label bug in two
  different files, the second written days after the first was written
  up as a lesson -- which is why it's now a function rather than a rule.
- **`on_click` fires on button-DOWN despite its name.** A control that
  commits there fires on press and can never be cancelled by dragging
  away. Arm in `on_press` (called every tick with live coordinates),
  act in `on_release`.
- **A "works on my machine" build is invisible locally.** CI caught
  that `gen_kbs.py` silently skips when `xkbcli` is absent, so CI had
  been building images with no keyboard layouts at all. If a build step
  can skip, assume it is skipping somewhere.

**Verify GUI work by pixel value, not by eye** (`tools/pixel_probe.py
--compare a.png b.png --at X,Y`). A hover state that moved a 235/255
background by two units looked completely plausible in a screenshot.
Always sample a control that should NOT have changed too.

**`ata nodma on|off`** forces the PIO disk path -- the fallback is
otherwise unreachable, and it's also the PIO-vs-DMA comparison that has
root-caused a real DMA bug before. **`make run-kvm`/`vm.py --kvm`**
exist now, but KVM is ~1.9x SLOWER for disk I/O, so never compare a
throughput number across the two modes.

**2026-08-14 (later still): a feature can be complete and still
unreachable.**

- **Finishing a migration is not finishing the feature.** Calculator,
  Notepad and Terminal all ran correctly in ring 3, every test passed,
  and the desktop still could not launch any of them -- the Start menu
  registry could only describe kernel-space apps, so it kept opening the
  ring-0 versions while the ring-3 binaries sat in `/tests` reachable
  only by typing `run calculator`. Nothing was broken; it just looked
  broken, which is its own defect. When a migration lands, ask what the
  USER's path to the new thing is, not just whether it works.
- **`/bin` vs `/tests` is a real distinction and it drifts.** Every
  ring-3 client landed in `/tests` because the first one did. Check
  `docs/filesystem-layout.md` when adding a seeded binary, and remember
  `sync` is ADDITIVE -- moving one needs an explicit
  `tfs3_writer.py delete` of the old path or the stale copy lives
  forever.
- **Growth exposes layout assumptions that were fine at the old size.**
  Adding four registry entries walked desktop icons off the bottom of
  the screen (the default layout was one unbounded column) and, once
  wrapped, made long labels overprint the neighbouring column. Both had
  been invisible for as long as the list was short. After changing
  anything that grows a list, LOOK at it -- `gui_regress.py` passed
  clean through both of those bugs, because no test asserts "the
  desktop is legible".
- **Check a claimed regression twice before believing it.** A click on
  the wrapped column produced zero pixel change and looked like a
  hit-testing bug in the new code; it was the icon still being selected
  from the previous run. The second, clean run changed 226 pixels in
  exactly the right box. A zero-diff is as easily a stale-state artifact
  as a real bug.

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
  - `python3 tools/usertest_run.py` -- the NON-GUI ring-3 diagnostics in
    `/tests`, as one table. In `preflight.sh`, so you rarely run it by
    hand; reach for it directly after touching `userland/rt/`,
    `userland/lib/`, the ELF loader or a syscall.
  - `python3 tools/compositor_test.py` -- M41 stage 2's raw input path
    to a registered ring-3 compositor. Its design point generalises:
    every injected input is asserted TWICE, in the compositor's log AND
    in UI Demo's, because "the compositor got the click" is equally
    satisfied by an implementation that STOLE the input stream.
  - `python3 tools/cursor_theme_test.py` -- cursor themes: the theme
    loads COMPLETELY, switching it changes the drawn pointer, the size
    setting scales it by the right magnitude, and a theme that does not
    exist still leaves a working pointer. Read its docstring first: the
    built-in fallback means "a cursor is on screen" proves nothing.
  - `python3 tools/gen_cursors.py` -- not a test: generates the shipped
    cursor themes into `data/cursors/` and is the authoring path for a
    new one. `--check` fails if they are stale. Re-run it after touching
    the built-in shapes in `apps/wm/wm_render.c`, which it extracts from.
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
  - `python3 tools/gui_regress.py` -- ALL of the app-level ones above
    plus the ring-3 client tests, each on its own fresh disk copy and
    its own VM, as one pass/fail table (~280 checks across twenty
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

**2026-08-14 (later): the filesystem is plural now -- TFS3 landed
(Milestone 15), TFS2 stayed, and the VFS chooses by probe.**

- **Two on-disk formats coexist.** `kernel/fs/tfs3.c` (block groups,
  real inodes, hardlinks, journal transactions, fsck, superblock
  backups -- spec: `docs/tfs3-spec.md`) and `kernel/fs/tfs.c` (TFS2,
  format byte-for-byte unchanged). `vfs.c` probes superblock magics at
  boot and mounts whichever claims the disk; a BLANK disk gets the
  default, TFS3. An existing TFS2 `disk.img` keeps working untouched
  -- `make clean-disk && make iso` (or `fsformat tfs3 confirm` in the
  OS) is the deliberate move. Which is active shows in `df`/`fsck`/
  `about`.
- **Capabilities are declared, not discovered**: `fs_ops.caps`
  (FS_CAP_INODES/HARDLINKS/SYMLINKS/EPOCH_TIME) with the display_driver
  honesty rule -- an optional op (only `link()` so far) and its bit are
  one fact stated twice, and the probe loop refuses a backend whose two
  statements disagree. Apps ask `fs_has()`; see the `ln` command for
  the shape of a good refusal message. `fs_stat()` returns
  `struct fs_stat_info` now: an `ino` (real on tfs3, synthetic table
  slot on tfs2) plus EPOCH-second timestamps (`tz_rtc_to_epoch()`/
  `tz_epoch_to_rtc()` in tz.c are the kernel's civil<->epoch
  converters). The ring-3 dirent ABI still carries `rtc_time` --
  conversion happens at the syscall boundary.
- **After touching anything in `kernel/fs/`, run
  `python3 tools/fs_switch_test.py`** -- boots a disk copy and proves
  probe, wipefs, live `fsformat` both ways, and reboot persistence in
  12 checks. It is the test that caught a real design hole (stale TFS3
  backup superblocks resurrecting a reformatted-as-TFS2 disk) on its
  first run. `tools/tfs3_writer.py corrupt` stages known damage
  (leaks, bad link counts, smashed superblock, committed/torn journal)
  for fsck and recovery testing.
- **Two failure-semantics rules the fault-injection KTESTs enforce:**
  a CRASH may cost a leaked block (fsck reclaims -- the
  prefer-a-leak-to-a-double-allocation ordering), but a RUNTIME
  failure must roll back everything the operation allocated. And
  "reformatting with a different filesystem" must wipe the OLD
  format's signatures including backups (`fs_ops.wipe()`), or its
  probe keeps claiming the corpse.
- **Two harness lessons from the landing, both expensive:** a stale
  `.o` after a struct-layout change (a field added mid-`fs_ops`) made
  a correct honesty check read garbage and refuse a good backend --
  when behavior contradicts source you just read, `make clean` before
  theorizing (CLAUDE.md's dependency-tracking caveat, observed live).
  And `make verify` re-seeds `disk.img` by SYNC, not reformat -- damage
  from a previous run persists on it, so a fix can look broken against
  leftovers; reproduce on a `make clean-disk` fresh image before
  concluding anything.

**2026-08-15 (documentation change, applies to EVERY session from
here): comments get shorter, and `CHANGELOG.md` is closed.**

On comments: this repo leans hard on them and mostly earns it -- write
**the invariant** (what must stay true) and **the trap** (what breaks
if you edit this the obvious way) at whatever length they need. What
does not earn its length is the war story: retelling two or three past
incidents with pixel counts and dates, forty lines where the rule is
two sentences. Cap the anecdote at one clause; `git log` has the rest.
Existing long comments are NOT being retro-trimmed -- they read
cheaply, and a bulk rewrite would most likely delete the one sentence a
future session needs.

And on the changelog: Do not add entries to it, and treat
any instruction elsewhere in this skill or in the repo that says to as
out of date. What changed goes in the commit message (file-by-file, as
before), how a mechanism works goes in a comment next to the code, why
it is built that way goes in `docs/decisions.md` written out in full
rather than as a pointer, and what is still broken goes in
`docs/roadmap.md` with a replayable reproduction. The reason: the same
reasoning was being written three times, and the changelog copy was the
one nobody re-read -- it had reached ~12,000 lines across four files.
They stay in the tree, frozen, because ~800 places point into them.

**2026-08-15 (hardening day): Milestone 2 closed, and three lessons
that each cost real time.**

- **`make all` does NOT rebuild `toy-os.iso`, and every headless test
  boots the ISO.** A positive control (make `.text` writable, expect
  the new W^X KTESTs to go red) came back 132/132 GREEN, which reads
  exactly like "this test measures nothing" and sends you auditing the
  test. The ISO was simply one build old. `make iso` before any
  `ktest_run.py`/`boot_smoke_test.py`/`vm.py`/GUI run, and when a
  positive control fires nothing, check `ls -l build/kernel.bin
  toy-os.iso` BEFORE suspecting the harness. Same family as the
  fixture lesson, different cause: the code never reached the machine.
- **The kernel's own memory is W^X now**, and `linker.ld` is where the
  permissions are decided: four PT_LOAD segments, `ALIGN(4096)` between
  the bands, and `__kimage_start`/`__ktext_start`/`__ktext_end`/
  `__kdata_start`, which `paging_enforce_wx()`
  (`kernel/arch/x86_64/paging.c`) reads at the top of `kernel_main()`
  to rewrite the identity map. Adding an output section means placing
  it EXPLICITLY and assigning it to a segment -- with PHDRS declared,
  an orphan's permissions are wherever `ld` felt like putting it, and
  the dangerous direction (landing in the R+X band) is silent.
- **CR0.WP is the half of read-only protection that is invisible when
  missing.** With WP clear -- how the CPU resets -- a supervisor write
  ignores the read/write bit, so ring 0 can overwrite `.text` through a
  mapping that dumps as read-only in every page table. Its KTEST is
  separate from the page-bit KTESTs for exactly this reason, and the
  positive control proves the point: commenting out the WP line reddens
  ONLY the CR0 check and leaves all six page-table checks green.
- Generalise it: **when a protection has two independent switches, test
  each one separately**, or the suite passes with half the mechanism
  off.

**2026-08-15 (same day, bug-fixing half): three more.**

- **A recorded known issue can simply be gone, and measuring that is
  the work.** `docs/roadmap.md`'s 205px damage violation named an exact
  repro (`damage_sweep.py --random 50 --seed 5`, step 47). Step 47 was
  still exactly the recorded interaction -- so the walk had not
  diverged -- and it no longer fired, three runs, plus five other seeds
  clean. The harness was proved awake first (one `wm_damage_rect()`
  removed -> three violations), because otherwise "0 violations" and "a
  harness checking nothing" are the same output. Entry DELETED, not
  amended: a corrected known-issue entry still implies breakage. What
  was NOT claimed: which change fixed it.
- **If a step belongs to "having a filesystem" rather than to
  "booting", put it beside the MOUNT.** `/etc` and `/tmp` were two
  `fs_mkdir()`s after `fs_init()` in `kernel_main()` -- correct exactly
  once per boot, and `fsformat` remounts a live disk without ever going
  near that line, so a reformatted volume had neither directory until
  the next reboot. Now `ensure_layout()` in `vfs.c`, called from
  `fs_init()` AND `fs_format_backend()`; `fs_mkdir()` being a no-op on
  an existing directory is what lets the rule be unconditional.
- **A function that can fail, whose caller returns `void`, is a silent
  failure waiting for a reason to happen.** Three `*_config_save()`s
  returned void and `tz_set_index()` returned "is the index valid",
  so `timezone Helsinki` on a disk with no `/etc` printed "Timezone set
  to helsinki." and wrote nothing. `etc_config_set()` was never at
  fault -- it returned 0 to callers that did not look. All four return
  `enum setting_result` (INVALID/SAVED/UNSAVED) now, three-way because
  "you gave me nonsense" and "I could not write it down" need different
  words, with UNSAVED non-zero so existing `if (!...)` callers still
  read correctly.
- **New tool: `tools/damage_hunt.py`** -- `damage_sweep.py` across many
  seeds, fresh disk + own VM slot each, one table. Its `-j` defaults to
  1 on purpose: parallel VMs reported a violation the same seed does
  not reproduce serially, and that is recorded as undiagnosed in
  `docs/roadmap.md` rather than fixed toward either hypothesis.

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
  The knowledge went into `CHANGELOG.md` instead.
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

**2026-08-15 (Milestone 2, second half): the user address space grew
rules, and the CPU now enforces them. Five things, and the last two are
about testing rather than about this kernel.**

- **`kernel/include/kernel/uaddr.h` is the ring-3 address-space map,
  stated once** -- heap base and limit, the guard region, stack bottom
  and top, page counts. Two loaders (`scheduler.c`'s spawn path,
  `elf_run.c`'s legacy one) used to carry identical private copies. The
  guard region below the stack **is defined by being UNMAPPED**; there
  is no PTE to set, so an overflow always faulted. What the header buys
  is the two things a hole cannot do for itself: `SYS_SBRK` is bounded
  against it (it had NO ceiling, so a big enough request mapped pages
  straight over the live stack, silently), and a fault there is reported
  as `Stack overflow` instead of an anonymous #PF. An mmap or ASLR
  REPLACES this header rather than adding beside it.
- **Kernel code touches user memory ONLY through `vmm.h`'s copy
  helpers** (`vmm_copy_from_user`/`_to_user`/`_string_from_user`).
  CR4.SMEP and CR4.SMAP are on wherever CPUID reports them, so a raw
  `*(T *)user_ptr` in ring-0 code is now a page fault rather than a
  subtle bug. The helpers walk to the frame and copy through the
  kernel's identity map (U=0), which SMAP does not police -- **so this
  kernel sets EFLAGS.AC nowhere and has no STAC/CLAC window in which the
  protection is off.** Available only because the whole low 4 GiB is
  identity-mapped; most kernels cannot choose it. The helpers also
  subsume `vmm_validate_user_range()` wherever it was paired with a
  manual copy loop, closing the gap between checking a mapping and using
  it. The live trap: `paging_make_user_page()` adds U=1 to the KERNEL's
  own identity mapping, so any page it touches becomes SMAP-protected
  against the kernel's normal access to it.
- **New tool: `tools/faulttest_run.py`.** The `/tests` binaries that
  fault ON PURPOSE -- which `usertest_run.py` correctly excludes, and
  which therefore nothing ran at all. A faulting binary has no exit code
  and no output, so the assertion is the KERNEL's report read from the
  serial log, with required AND forbidden substrings per entry: a stack
  overflow and a null dereference are both page faults, so every entry
  doubles as the positive control for its neighbours. Each gets its own
  QEMU, because **a ring-3 crash takes the serial debug console down
  with it** -- `vm.py` cannot drive any of these, `crash_test` included,
  which is worth knowing before debugging a "hung" test. The command is
  typed at the physical shell over QMP instead.
- **Background colour belongs to a CELL, not to a region**
  (`docs/gui-guidelines.md` now says so). A scroll fills the incoming
  row with the console's DEFAULT background, never the live `cur_bg` --
  that row is blank, so it is the console's, and filling it with a
  transient colour painted a full-width band that later text only partly
  repainted (ragged red stripes under the panic banner). If you add
  anything that paints console background, remember **the console
  redraws from scrollback on PageUp**: paint straight into the
  framebuffer without going through `sb_record()` and it vanishes the
  first time the user scrolls. That exact bug collapsed a five-line
  banner into one stripe.
- **A positive control's job is to change the work, not to confirm it --
  and twice here it did.** Removing the sbrk bound left the ring-3 test
  entirely GREEN, because it asked for 1 GiB and the guest ran out of
  PHYSICAL memory long before it ran out of address space: sbrk refused
  for the wrong reason. Resizing the request to 2 MiB (just past the
  gap, trivially allocatable) reddened it -- and writing THROUGH the
  returned pointer was needed on top of that, because an alias costs
  nothing until somebody writes. Ask not just "does the control fire"
  but "does my input actually cross the boundary, and is the damage
  observable yet".
- **Some properties cannot be tested by the suite that covers them, and
  saying so is part of the work.** The SMAP KTESTs assert CR4 against
  CPUID (so they are meaningful on `qemu64` AND `--cpu max` and can fail
  under either) -- but they cannot show ENFORCEMENT, because the copy
  helpers never touch a user mapping and behave identically with the bit
  on or off. That took a separate, uncommittable probe: one raw
  dereference put back, ring-0 #PF under `--cpu max`, same build clean
  on `qemu64`. Record what the suite does NOT prove.
- **When the optimizer is between you and the behaviour, read the
  disassembly.** A test meant to overflow the stack hung forever instead:
  GCC's accumulator form of tail-recursion elimination had turned
  `return frame[0] + burn(depth + 1)` into a LOOP with one reused frame
  at -O2. `volatile` on the frame does not prevent it. The fix is a
  `volatile` function pointer plus reading the frame AFTER the call --
  and `objdump -d` is what settled it, not re-reading the C.
- **Before proposing a big change, MEASURE its feasibility.** Kernel
  ASLR was scoped this session without building anything: linking with
  `ld --emit-relocs` and counting relocations in loaded sections gave
  7,189 needing fixup out of 19,136 (the rest are PC-relative and
  survive a move), `-mcmodel=kernel` was found to pin the base to the
  low 2 GiB, and `.bss`'s 13 MB back buffer was found to cut a 256 MB
  guest to ~6.5 bits of entropy. All three are in `docs/roadmap.md` now.
  Half an hour of measurement turned "the biggest item left" into a
  costed plan with a staging order -- do that before asking the user to
  choose a scope, not after.

**2026-08-16 (later, one long session): M41 stages 0-1, a block layer,
a Live CD, and a lot of the GUI toolkit. Read this before touching the
desktop or the filesystem -- most of it invalidates older advice above.**

- **The WM's apps are GONE from ring 0.** M41 stage 0 deleted the
  kernel-space Notepad/Calculator/Terminal and ported About and UI Demo
  to `userland/gui/`; only Task Manager and Control Panel remain
  kernel-side (they need syscalls ring 3 lacks, which is stage 4's job).
  `apps/ui/` lost four widgets and is SHRINKING -- add a widget to
  `userland/ui/` unless the WM itself needs it.
- **The Start menu is built from FILES**, `/usr/wm/desktop/*.desktop`
  (source: `data/wm/desktop/`). Adding an app is dropping a file, not
  editing `gui_apps.c`. Windowed binaries live in
  `/bin/wm/{system,apps,demos}/`, the class coming from
  `userland/gui/<class>/`.
- **The toolkit ROUTES POINTER INPUT and DRAWS the widgets.** An app
  declares `uapp_desc.widgets` (a `uui_item[]` with ids) and gets
  `on_widget(app, id, reason)`. It writes NO press/drag/release
  dispatch, and usually no draw calls: the library paints declared
  widgets, overlays last. **`on_draw` runs BENEATH the widgets** -- that
  ordering is deliberate, because the reverse let an app's `ugfx_fill()`
  wipe every widget and ship UI Demo completely blank past a 35-check
  suite. Use `on_draw_over` for anything that must land on top.
- **Editing text has ONE implementation** (`userland/ui/uui_edit.h`):
  caret, selection and the standard keymap (Ctrl+A, Shift+arrows,
  typing replaces the selection), with storage delegated through four
  accessors so the single-line field and the multi-line document cannot
  diverge. Same split `kernel/lib/klineedit.c` already had.
- **A filesystem talks to a `block_device`** (`kernel/include/kernel/
  block.h`), not to ATA. TFS3 does; TFS2 deliberately does not.
  `persistent` is a field on the DEVICE -- a backend cannot tell RAM
  from disk, and a live session claiming persistence is the worst thing
  that layer could do.
- **TFS3's last block group may be PARTIAL** (ext2/3/4's rule).
  `group_span(g)` answers "how big is group g"; `T3_BPG` is the stride.
  This also recovered ~127 MB on a 9 GiB disk that floor division had
  been wasting.
- **There is a Live CD and a demo ISO**, both SEPARATE artifacts:
  `make live-iso` / `make run-live`, `make demo-iso` / `make run-demo`.
  The ordinary ISO carries no GRUB module on purpose -- a 129 MiB one
  took the boot smoke test from 1.6s to 7.0s and turned CI red, because
  GRUB reads the whole module off the emulated CD before the kernel
  starts. Measure before folding it back in.
- **MAX_PROCS is 64 now** (`SCHED_MAX_PROCS`), not 4. And `gui close`
  used to destroy a client's window WITHOUT telling the client, so every
  launcher leaked a process slot -- it goes through `wm_request_close()`
  like every other close.

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

**2026-08-16: v0.2.0 SHIPPED, Milestone 2 CLOSED, and the repo moved.
Read this before assuming anything about the project's state.**

- **The repo is `eveningworks/toy-os`** -- an organization, not a
  personal account. It moved because a personal repo has NO read-only
  collaborator role (every collaborator gets write), and branch
  protection is unavailable on a free private repo either way -- both
  verified against the API, including that a free ORG does not unlock
  it. The maintainer's handle was renamed in the same stretch and
  scrubbed from committer metadata. Consequences: the remote is
  `git@github.com:eveningworks/toy-os.git`, and nothing in the tree
  should name a personal account.
- **`tools/backup_repo.sh` exists -- run it before touching the repo's
  identity or history.** `git clone --mirror` is NOT a backup here: it
  captures every commit and none of the ~130 MB of release assets,
  which live only on GitHub. It verifies what it can; the restore test
  (clone the mirror, `make all`) is a manual step and worth the minutes.
- **v0.2.0 is released** (178 commits since v0.1.0; milestones 2, 4 and
  15). `VERSION` is `0.3.0-dev`. The release process is unchanged, but
  note `CHANGELOG.md`'s `[Unreleased]` stamp did its one useful job
  there and is a permanent no-op now.
- **The roadmap's "Milestone N (planned v0.N.0)" convention is
  RETIRED** -- 38 headings carried a planned version and the mapping had
  stopped being true. Milestone numbers order work; they do not name
  releases.
- **Milestone 2 is complete**: heap red-zones and use-after-free
  poisoning behind a RUNTIME toggle (`heap debug on`), and kernel ASLR
  -- the kernel relocates itself to a random base each boot and patches
  ~7,400 of its own absolute references. Two traps live there and both
  fail silently: `kfree()` tells red-zoned blocks from plain ones by the
  eight bytes before the payload (sound only because heap pointers fit
  in 32 bits and the magic's top half does not), and the relocation must
  repoint CR3 because `paging.c` reaches the page tables by LINKER
  SYMBOL -- forget it and W^X silently stops applying while **all six
  W^X KTESTs stay green**. Only a check that asks the CPU catches that.
- **`docs/wm-ring3-design.md` is the staged plan for Milestone 41** --
  moving the WM itself to ring 3. Its load-bearing finding: all 13 GUI
  test tools drive the WM through `wm_debug.c`'s `gui` commands over the
  KERNEL's serial console, so the 280 checks that prove the desktop
  works have to move with it, and that gets its own stage BEFORE the WM
  moves.

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
  instructive -- `make run-kvm` / `vm.py --kvm` honours guest memory
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
- **`make run-kvm` honours guest memory types; plain `make run` (TCG)
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

**And three design calls from the same session, each of which had an
obvious wrong answer:**

- **Asked for a second directory, give a KEY.** "Add /usr/wm/startmenu
  for the Start menu" -- but one directory already fed both surfaces, so
  a second one means any app wanted in both has its file duplicated and
  the copies drift. `ShowIn=desktop startmenu` instead, which is what
  freedesktop.org did for the same reason. When a request names a
  MECHANISM, check whether the underlying need is already half-met.
- **Watching a directory without inotify is a COUNTER, not a poll.**
  `fs_generation()` -- one integer the VFS bumps on any mutation -- makes
  "has anything changed?" free per frame, where re-listing on a timer
  means a real disk read every few seconds forever on an idle machine.
- **A widget two surfaces will want is SHARED SOURCE compiled twice**,
  not built kernel-side and ported later. `kernel/lib/rubberband.c`
  takes `geom.c`'s path into both the kernel and `libuapp.a`. The
  alternative produces two implementations that drift, which this repo
  has paid for three times. It must stay freestanding to qualify.

**2026-08-16 (M41 stage 3, and a long feature run): the traps were all
SILENT, and three of them were in code I had just written.**

- **A COPY of `disk.img` goes STALE the moment you rebuild.** `make iso`
  re-seeds the real image with the new `/bin` binaries; a copy taken
  before that still holds the old ones. The VM then runs the NEW kernel
  against the OLD userland, so a ring-3 fix looks like it did nothing
  while the kernel half of the same change plainly works. Re-copy after
  every `make iso`, not once per session. This cost real time debugging
  a layout fix that had already landed.
- **A widget's `ops->hit` is a BOOLEAN.** The router tests
  `!it->ops->hit(...)`, so a widget whose `_hit()` returns a ROW INDEX
  makes row 0 -- the one falsey index -- unclickable, while every other
  row works. `uui_listbox` shipped that way and a 42-check tool never
  noticed; it was found only when a new widget copied the line. Write
  `>= 0`.
- **A `natural_size` that depends on the widget's POSITION is a feedback
  loop.** `uui_button_group_natural_size()` measured from the origin
  rather than reporting the union's extent, which is the same number
  only while the group sits at (0,0). Once a layout moved it, it
  reported offset-plus-size, ate the sibling's growth allowance, and the
  symptom was a table growing 16 px against a 300 px resize -- which
  reads as a broken resize path, not a broken measurement. **When a
  number is wrong by a specific amount, work backwards from the amount**:
  286 was arithmetically only explicable one way, and that named the
  function.
- **Check a stated blocker against the code before planning around it.**
  `docs/wm-ring3-design.md` listed "a ring-3 allocator" as a stage-4
  blocker because "the WM's per-window state is kmalloc'd". It is not
  and was not: the only `kmalloc` in `apps/wm/` was a COMMENT pointing
  at a file stage 0 had already deleted. A whole prerequisite evaporated
  on one grep.
- **Ship the test tool WITH the app, not after.** Task Manager shipped
  with no tool, and a resize bug went out with it. The tool written
  afterwards found the bug in ten seconds -- and then found a
  pre-existing one in `uui_listbox` besides. If an app is worth adding
  to `gui_regress.py`, it was worth adding before the commit.
- **Assert the MAGNITUDE, not the change.** "The table resized" is
  satisfied by 16 px out of 300, which is exactly the shipped bug. The
  check has to be "it grew by roughly what the window grew by".
- **A check that cannot distinguish success from a missed click passes
  vacuously.** "One click arms and kills nothing" is equally satisfied
  by a click that landed nowhere. The fix was to make the APP log its
  state transitions (`taskmgr: armed kill pid N`, `no row selected`),
  which is the same "ask the app where things are" rule applied to
  state rather than geometry.
- **A geometry logged once at startup cannot answer a question about
  resizing** -- and if a widget moves with the thing that resized, log
  that too. Reporting only the table left a tool clicking the buttons'
  pre-resize coordinates and concluding the buttons were broken.
- **`DebugConsole.logs()` CLEARS what it returns.** A second parser over
  it finds nothing, so a value reported once vanishes. Accumulate across
  calls, or parse everything in one pass.
- **I recorded a wrong diagnosis and had to withdraw it.** A slot-0
  correlation for a flaky tool went into `docs/roadmap.md` inferred from
  a neighbouring entry rather than observed -- and `--logs` did not
  record the slot, so it could not be checked afterwards. Write down
  what was MEASURED; if the tooling cannot answer the question, fix the
  tooling (it records the slot now) rather than guessing.

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

**2026-08-17 (M41 stage 4's prerequisites, and five silent bugs): ring 0
now contains NO applications. Read this before touching settings, the
Toykit widgets, or believing a green test run.**

- **`Exec=builtin:` is GONE and `apps/` holds no apps at all.** Control
  Panel was the last one; it is `userland/gui/system/cpanel.c` now. The
  builtin table, its lookup and its struct are deleted from
  `gui_apps.c`. Stage 4's prerequisite list is CLOSED -- what remains is
  the WM itself.
- **A setting REGISTERS itself** (`kernel/include/api/setting.h`): name,
  label, type, file, a choice ENUMERATOR, a getter, and one `apply` that
  validates, applies AND persists. Announced from `settings_init()` the
  way a `display_driver` announces itself. **Do not add a setting as a
  bare `etc_config_get`/`_set` pair any more.**

  The reasoning generalises, and it is the most reusable thing here:
  the plan asked for "syscalls for `etc_config_*`", i.e. let ring 3 read
  and write `/etc`. That would have solved ACCESS and left the real
  problem -- nothing could answer *what settings exist*, so a Control
  Panel had to carry its own list, a second source of truth that drifts.
  **When a request names a mechanism, check what the underlying need
  is.** (Same call as `ShowIn=` instead of a second directory.)
- **The files did NOT change and must not.** Settings are still plain
  `name=value` text under `/etc`, editable in `edit`. The registry is an
  INDEX over those files -- which is the half `/etc` has never been able
  to provide about itself, and the answer to "which file is this in?".
  Keeping them hand-editable costs two things, both paid for rather than
  dodged: `settings_reload()` re-reads after an edit and REPORTS
  refusals, and a `generation` counter rides every reply so a client
  notices someone else's change.
- **`/etc/config.d` is how a config FILE declares itself** -- one
  `Name`/`Path`/`Description` descriptor each, over a built-in floor.
  Files-only could not bootstrap (a blank disk has no such directory,
  and a deleted descriptor would leave `/etc/toyos.conf` nameless), so
  built-ins are the floor and a descriptor with the same name OVERRIDES
  one. That is the vendor-default/`/etc`-override pattern, and it is
  what lets a ring-3 program declare its config with no kernel change --
  which the WM needs after stage 4.
- **`/bin/config`** is the front end: `list get set unset where diff
  reload files show find register unregister`. It contains **no
  `name=value` parser** -- every value comes back from the kernel's one
  parser, including the FILE's value as distinct from the live one
  (`diff`). A second parser there would drift from `etc_config.c`, and
  the drift would surface as `config` and the system disagreeing.

**FOUR TOYKIT BUGS, all general, all silent, none specific to the app
that found them.** Each makes a widget look broken in a way that points
somewhere else:

- **`uui_listbox` and `uui_radio_list` had no `natural_size`/
  `set_geometry` in their ops tables**, so neither could be POSITIONED
  by a layout -- they drew at whatever `init()` was given, on top of
  their siblings and outside the content area. It reads as a clipping
  bug.
- **`uui_radio_list` has no `init()`**, so a metric the caller did not
  assign stayed 0 -- and `row_h`/`col_w` of 0 makes the control
  zero-sized: it draws nothing, hit-tests nothing, and reports a natural
  size of nothing, so a layout gives it no room. Invisible AND
  unclickable from one unassigned field. It has font-derived defaults
  now. **A widget set up by field assignment rather than an init() is
  worth auditing for this shape.**
- **`hidden` was honoured by the router and NOT by the layout's draw**,
  while `uui_widget.h` has always promised "not drawn". An app declaring
  both a layout and `.widgets` (the normal shape) got a control that was
  unclickable and still perfectly visible -- the page it had hidden
  painting straight over the page it switched to.
- **`ops->hit` must cover the WHOLE widget, not the rows.** `uui_table`
  routed on `uui_table_hit()`, which deliberately excludes the header
  and the scrollbar column because it answers "which ROW". The router
  therefore never delivered a press to either, and a header click
  reached nothing at all. This is the SECOND `ops->hit` trap in this
  toolkit (the first was returning a row index from a boolean slot).

**Sortable columns, and the split worth copying.** `uui_table` sorts on
a header click: the WIDGET owns the ordering (an `int order[]`
permutation) and the APP owns the comparison. That is Win32's
(`LVN_COLUMNCLICK` + `ListView_SortItems`), Qt's
(`QSortFilterProxyModel` + `lessThan`) and GTK's (`GtkTreeSortable`)
split, and **none of them sort the DISPLAYED TEXT** -- which is the part
to copy. This table's cells are formatted strings, so a text sort puts
"10" before "9" and orders "4 KB" against "1 MB" meaninglessly. Every
public row index on the widget stays an APP row, so a selection survives
a re-sort. Insertion sort because it is STABLE and n is bounded; there
is no `qsort` here.

**A GUI you cannot test the same way twice: `video=<W>x<H>` and
`KCMDLINE`.** A user reported VirtualBox booting at 640x480 from the
Live CD. Two independent causes, and only one was ours -- worth
separating before "fixing" anything. VirtualBox's VBoxVGA has a short
VESA mode list, GRUB falls back, and **no code here can change that**
(there is no modesetting driver for a plain VESA framebuffer; the fix is
VirtualBox's own `CustomVideoMode1` extradata). But with VMSVGA
(15ad:0405, what QEMU's `-vga vmware` also presents) `vmsvga.c` IS a
modesetting driver -- and it was mirroring GRUB's geometry "so the
takeover is invisible", faithfully re-programming 640x480 on an adapter
that could do far better. It walks a fallback LADDER now
(`display_mode_candidate()`), because "the adapter cannot do 1920x1080"
should mean "then try 1600x900", not "keep whatever GRUB left".

`make iso KCMDLINE="video=1920x1080 nokaslr"` bakes boot words into the
ISO (also `live-iso`/`demo-iso`), so trying a flag no longer means
pressing `e` in the GRUB menu every boot. The `grub*.cfg` files carry a
one-screen summary of `docs/boot-flags.md` for whoever reads them on the
ISO.

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

**Documentation has to live where the reader is looking.** The boot-word
list went into the top of each `grub*.cfg` -- and GRUB's `e` editor
shows the menuentry BODY only, so it was invisible to the one person it
was written for. Reported by the user with a screenshot of the editor.
It is repeated inside each `menuentry` now, kept to four lines because
the edit screen is ~20 and a full table would push `multiboot2` and
`boot` off it. **Verified by booting the live ISO with a menu, pressing
`e` over QMP and screenshotting it** -- the general form of this repo's
"LOOK at what you drew" rule, applied to a documentation surface rather
than a drawn one. Reasoning about GRUB's parser would have been cheaper
and would not have answered the question.

**Two toolkit additions, both earned by a survey rather than a hunch:**
`k_strlcat` (two real callers, one of which was an unbounded append that
overflowed in the shell's `ls`) and `k_isblank` (SIX hand-rolled copies
of `c == ' ' || c == '\t'`, all deliberately not `k_isspace` because
`'\n'` terminates a line in every parser here). `kfmt` grew `%Ns`/`%-Ns`
column padding for the same reason. And the survey's other finding is
the better lesson: **`k_tolower`/`k_toupper` already existed and four
places hand-rolled them anyway** -- including `klineedit.c`, whose own
header comment called the duplicate deliberate. **A header calling its
own duplicate deliberate is worth re-checking against the code.**

**2026-08-17 (M41 stage 4a, cursor themes, and a bug that shipped
INVISIBLY): read this before touching the WM, the settings, or anything
you intend to seed onto the disk.**

- **Stage 4a is BUILT and stage 4's requirements are written up as
  R1-R9** (`docs/wm-ring3-design.md`). R1 (the framebuffer grant), R4
  (`SYS_FS_GENERATION`) and R5 (the idle-work owner) landed; R3 was
  REMOVED rather than deferred. What is left of Milestone 41 is 4b-4d:
  the WM binary itself.
- **`scheduler_idle()` owns the kernel's idle work** (`api/scheduler.h`).
  Any loop that is WAITING rather than working calls it -- the shell's
  key wait, `wm.c`'s event loop, a long `cat`, the demo's timer. Do not
  add a bare `debug_console_poll()` to a new waiting loop. The reason is
  Milestone 41: the serial debug console had no owner, it was polled by
  whichever loop happened to be running, and the WM's copy is the
  load-bearing one because every GUI test tool arrives over that wire.
  Naming it kernel-side means the WM's departure deletes a CALL, not the
  capability. Not on the timer tick: a dispatched command can be
  `sh cat big`, which blocks on the filesystem.
- **The registered compositor can be GRANTED the real framebuffer**
  (`WIN_REQ_FB_MAP`/`WIN_REQ_FB_PRESENT`, `kernel/proc/win_surface.c`).
  Three traps live there. **The memory type must reach the USER PTE**
  (`vmm_map_user_page_type()`, `VMM_MT_WC`) -- the kernel's identity map
  and the client's mapping are separate PTEs, so without it a ring-3
  compositor gets a CACHED framebuffer, the bug class TCG cannot show.
  **Present is required, not advisory** -- `vmsvga` declares
  `DISPLAY_CAP_NEEDS_FLUSH`, where written pixels stay invisible until
  the driver is told. And **a ring-3 write is TRANSIENT while the WM is
  still ring 0**: it survives until the WM's next frame, so a test that
  looks for a painted block in a screenshot FAILS against a working
  kernel. Assert instead that the mapping is the real screen, by
  comparing a client's read against a screendump of the same pixel.

**BEFORE DESIGNING A PATH TO REACH A CAPABILITY, CHECK THE CAPABILITY IS
SWITCHED ON SOMEWHERE.** Stage 4a listed "the cursor over TWP" as a
requirement, to be deferred so it would have a real caller.
`DISPLAY_CAP_CURSOR` turned out to be declared by ONE driver, which
disables it by default (a hardware cursor over a relative PS/2 mouse
makes the pointer jump) -- so it is unreachable on every configuration
this OS boots, and a protocol path to reach it would have been worse
than the problem it solved. The requirement was DELETED and moved to
M27a, where virtio-input and virtio-gpu make it real. `tools/vm.py
--vga vmware` exists partly because that check had no way to be run:
the modesetting driver and the cursor plane are both unreachable under
the default `std` adapter, the same shape as `--cpu max` for SMEP/SMAP.

**Cursor themes: the pointer's shapes are DATA FILES** (`data/cursors/
<theme>/<shape>`, generated by `tools/gen_cursors.py`, loaded by
`apps/wm/cursor_theme.c`). The layering is the one Windows and Wayland
both converged on: an app NAMES a shape, the compositor owns the theme
and produces pixels, the display layer owns any hardware plane. Wayland
originally had clients supply the pixels and added `cursor-shape-v1` to
undo it -- don't repeat that. A shape file carries COVERAGE, not colour,
so one shape set serves a light theme and a dark one. Nothing touches
the kernel, so it all moves to ring 3 with the WM.

**THE BUG WORTH THE MOST HERE: a file written into `seed/sync/` is
GITIGNORED and deleted by `make clean`.** `docs/filesystem-layout.md`
rule 5 says so, and the cursor themes were generated straight into it
anyway. They therefore existed only in the working tree that made them:
never committed, absent from every other checkout, and the maintainer's
machine logged `0 of 6 shapes loaded` while every test on the authoring
machine passed. Anything hand-authored or generated that must SHIP goes
in a tracked directory (`data/...`) and is staged by the Makefile's
`seed` target. **Generalise it: when a feature has a fallback, the
fallback will hide the feature's absence** -- which is the same trap as
the next bullet, arriving from a direction the test could not see.

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

**And two process lessons from the same day:**

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

The specific commands below
were verified current as of the last time this skill was updated, but
if `CLAUDE.md`, `VERSION`/`BUILD_NUMBER`, `apps/ui/`, or
`CHANGELOG.md`'s structure look different from what's described here,
trust the repo over this document, and consider that this skill itself
may be due for an update to match (see its own delivery step 6 -- the
same "keep docs matching reality" instinct applies to this file too).

## The sequence

0. **Figure out which mode this session is in before anything else.**
   This repo gets worked on two ways: a Cowork cloud session where the
   user's real checkout is reachable only through the device bridge
   (`mcp__remote-devices__*`), or a session (e.g. local Claude Code)
   with normal file/Bash tools directly against the user's real
   checkout. Deterministic tell: run `git config user.name` -- empty
   means Cowork/device-bridge (that session has no git identity
   configured at all), non-empty means direct local. Corroborate with
   whether `mcp__remote-devices__*`-style tools are actually available
   to call. If those disagree or it's still unclear, ask the user
   directly rather than guessing -- see CLAUDE.md's "Working in the
   cloud sandbox vs. directly on the user's machine" section, which
   this step summarizes. Steps 3 and 6 below branch on the answer.

1. **Research before proposing anything.** Even a request that sounds
   simple ("add X support") usually touches more of the codebase than it
   first appears -- this repo has caught real terminology mixups (EFI vs.
   ELF) and stale-README claims (a widget that already existed) by
   researching first. Use a subagent for anything that means surveying
   more than one or two files -- driver code, rendering, existing
   patterns for a similar feature. Come back with a concrete picture of
   what exists today and what a change would actually touch, not just an
   instinct.

2. **Present a few real choices before writing code.** This user's
   standing instruction is "always give me a few choices to choose from
   if not specified otherwise" -- use `AskUserQuestion`. The choices
   should come from what research actually found, not be generic
   (e.g. not "should we do this well or poorly" -- more like the real
   forks a build has: which of two data-layout tradeoffs, which of two
   places a setting could live, how far to build something this session
   vs. just planning it). Skip this step only for a genuinely
   unambiguous one-line fix. If a change touches the GUI and could
   plausibly be a reusable primitive (a button, a field, a toggle), that
   choice -- build it as an `apps/ui/` widget (note there is now a
   ring-3 mirror of that toolkit in `userland/uui.c`/`uwidgets.c` for
   client apps; a widget needed by both is ported, not shared, since
   the kernel one draws to the framebuffer and takes WM callbacks) (see
   `apps/README.md`'s
   "Shared widgets" section -- one file per widget, pulled together via
   the `apps/ui/ui.h` umbrella include) vs. one-off -- belongs in this
   round of questions too, per the project's own instruction to ask
   before adding widgets.
   See `references/questions-that-worked.md` for real examples from past
   sessions, including ones the user directly praised (phasing options,
   storage-design tradeoffs, encoding choices).

3. **Implement -- where, depends on step 0's answer.**
   - **Cowork/device-bridge:** edit in `/home/claude/toy-os` (or
     wherever this session's sandbox clone lives) using normal file
     tools, build and test there first, and only copy verified results
     out to the user's real machine at the end (step 6). Editing the
     device checkout directly means every trial-and-error build cycle
     round-trips through the device bridge for no reason.
   - **Direct local checkout:** just edit the real checkout directly
     with normal file tools -- there's no separate mirror to keep in
     sync, no round-trip cost to avoid. Step 6 becomes "commit what's
     already there," not "copy it somewhere."

4. **Build and test -- scale the testing to what changed.**
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
     `boot_smoke_test.py`, `ktest_run.py`, then a `git status --short`
     summary) in one command -- `make verify` is the same thing without the git
     summary -- use it as the default "am I safe to keep going /
     safe to deliver" check instead of chaining the steps by hand.
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
     exists; its `APP_ORDER` list needs to stay in sync with
     `apps/gui_apps.c`'s registry order if a GUI app is added/reordered.
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
     be honest in `CHANGELOG.md`/`docs/roadmap.md` about what the
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

5. **Write the docs, in the existing style, not a new one.** Every
   substantive change gets a `CHANGELOG.md` entry under its
   `## [Unreleased]` heading (Keep a Changelog style -- `### Added`/
   `### Changed`/`### Fixed`/etc. subsections, no version number
   attached to the entry itself); read a couple of recent entries first
   and match the shape (what was asked, what was found, what was
   decided and why, what got verified) rather than inventing a new
   format. Add a `docs/decisions.md` entry only when the change answers
   a "why does toy-os work this way" question a future session would
   plausibly hit again -- most changes don't need one. Update
   `docs/roadmap.md` (checkbox list, `- [ ]`/`- [x]`) if this session
   only planned something rather than building it, striking through and
   linking the CHANGELOG entry once something listed there actually
   ships; update `README.md`'s own feature/command description instead
   if it actually got built. See `references/doc-templates.md` for the
   exact shapes and real excerpts to copy the tone from -- don't
   freehand these from scratch, and double-check the template still
   matches the live files (see the note at the top of this skill about
   conventions changing).

6. **Ship it -- usually without a version bump.** Most changes are a
   commit under the current `-dev` version, not a release. Shared by
   both modes:
   - List every file you added or edited in your response to the user --
     standing instruction, keep it compact. `tools/deliver.py` builds
     the delivery file-list, device-path mapping, and a commit-message
     skeleton from `git status --short` -- a faster and less
     error-prone starting point than reconstructing the list by hand
     (Cowork mode especially; a local checkout can just read `git
     status --short` directly), though it doesn't distinguish a deleted
     file from a modified one (see the Cowork bullets below for the
     "can't delete over the device bridge, `mv` to `_to_delete/`"
     handling a deletion still needs there -- not applicable locally,
     where a plain `git rm`/delete just works).
   - Rebuild (`make all && make iso`) and re-run the boot smoke test one
     more time against the final state (`tools/preflight.sh` again is
     the fastest way to do this), so what you deliver is what you
     actually verified.
   - Never write PII into any file; if a change seems to need it, ask
     first or anonymize and say so.
   - Any genuinely reusable tooling from this session belongs in
     `tools/`, not left as scratch -- see CLAUDE.md's `## tools/`
     section for the bar, and update CLAUDE.md's own `tools/` listing
     (and any other doc that describes it) to match if you add one.
   - **No git tag for a routine change.** Tags (`v<version>`) only
     happen when actually cutting a release via
     `tools/set_version.sh <version>` -- that's a separate judgment
     call (does this feel milestone-worthy enough to want a pinned,
     downloadable version), not something to do by default per change.
     If the user does want to cut a release as part of this change, see
     `references/delivery-checklist.md` for the full mechanics (both
     modes).

   **Cowork/device-bridge:**
   - Deliver every changed/added file with `SendUserFile`, then
     `mcp__remote-devices__device_commit_files` to land them on the
     user's real checkout.
   - Commit on the device checkout via `tools/device_git.sh` (never a
     raw `git` call over the device bridge -- even a read-only `status`
     leaves a stale lock file behind; see `CLAUDE.md`). **Always pass
     `-c user.name="toy-os" -c user.email="noreply@toy-os.local"`
     explicitly** -- the device-bridge session has no git identity
     configured at all, so a plain commit fails, and this is also the
     repo's standing privacy convention (a real name/email had to be
     scrubbed from history once already; see `docs/decisions.md`). The
     commit message body lists each changed/added file with a one-line
     note (subject line stays a short summary) -- see
     `references/delivery-checklist.md` for the exact shape.
   - Mirror the same commit (and tag, if one was cut) in the cloud
     sandbox clone too (plain `git` works there, it's not going
     through the device bridge -- but still use the same `-c
     user.name="toy-os" -c user.email="noreply@toy-os.local"` identity,
     not a distinct "sandbox" one; see `references/delivery-checklist.md`
     step 5 for why that matters even though this mirror is never
     pushed) -- keeps the sandbox's history matching the real repo's
     for the next thing in this session, but this mirror is
     **intentionally never pushed**.
   - **Never push from the session, on either checkout -- and this is
     enforced, not just a rule.** The cloud sandbox's outbound git
     proxy blocks `git push`/`gh release create` outright regardless
     of the repo token in the remote URL (confirmed by an actual
     failed push while cutting v0.0.9: `access denied by the git
     proxy: ... not in this session's authorized repository set`), and
     the device bridge has no network access at all. Give the user the
     exact command to run themselves: `git push origin main` (add
     `--tags` only if a release tag was actually cut this round). If a
     release tag was cut, see `references/delivery-checklist.md` step
     6 for the full asset list (it's `toy-os.iso` +
     gzipped `disk.img.gz` + `tools/run_release.sh`, not just the ISO)
     and the `gh release create` command to hand over alongside the
     push.
   - See `references/delivery-checklist.md` for the full mechanical
     rundown (exact tool calls, the Makefile-is-a-protected-file
     workaround, what "list every file" should look like) if any of
     this is unfamiliar.

   **Direct local checkout:**
   - The files are already on the user's real checkout -- there's no
     separate deliver step. Just `git add` the changed/added files and
     commit with plain `git` (identity is already configured, see
     CLAUDE.md) -- the same per-file-body-line commit message
     convention still applies (see `references/delivery-checklist.md`).
   - `git push origin main` and `gh release create`/`gh release upload`
     both work directly from here -- confirmed in real sessions, not
     blocked the way Cowork's proxy blocks them.
   - **Push policy (user's standing grant, 2026-08-14, reaffirmed in
     session): push ordinary commits to `origin/main` WITHOUT asking**
     when the change is clear-cut -- built, tested, in scope of what
     was asked. From a worktree-isolated background session that means
     `git push origin HEAD:main` (a fast-forward), since the worktree's
     branch isn't `main`. What still gets confirmed first: tags,
     GitHub releases, force-pushes, history rewrites, and anything the
     user framed as tentative.

## A recurring wrinkle worth knowing about upfront (Cowork mode)

An automated stop-hook may warn about "unpushed commits" after nearly
every turn in this repo. In Cowork/device-bridge mode, that's almost
always the cloud sandbox mirror from step 6 (intentionally never
pushed), not the user's real checkout -- check which one it means
before reacting; if it's the sandbox mirror, a one-line explanation is
enough, the same explanation every time (no need to re-litigate it). In
a direct local checkout there's no separate mirror, so this warning
means exactly what it says -- real unpushed work on the real
checkout -- and is worth surfacing to the user rather than
explaining away.

## Verification habits this project rewards

Learned repeatedly, and cheap to repeat:

- **A green test proves nothing until you have seen it go red.** Before
  trusting a new test, break the thing it covers and confirm it fails
  on the RIGHT assertion. Two kernel bugs and one GUI regression in the
  ring-3 GUI work were only credible because of this; `damage_sweep.py`
  ships a `--positive-control` flag for the same reason. Restore the
  code afterwards and re-run.
- **Assert round trips, not appearances.** The strongest checks written
  here compare a state against ITSELF after a cycle: save a file, clear
  the buffer, reopen it, require the rendered pixels to match; or check
  the bytes on disk through an INDEPENDENT path (`sh cat`) rather than
  believing the app that wrote them.
- **Pick an assertion the failure mode cannot pass.** "Text appeared"
  is weak; "a builtin left N ink and an external program left 3N"
  specifically distinguishes "the pipe carried the child's output" from
  "the command line was echoed". Ask what a broken version would still
  pass.
- **When something fails twice, stop reasoning and go look.** Repeatedly
  in this repo the second guess was also wrong and a screenshot or a
  `dmesg` line settled it in one step. Guessing a third time is the
  expensive move.
- **Suspect your own test before the code, but verify either way.**
  Several "bugs" here were the harness: a script that never entered GUI
  mode, a listing parser that swallowed kernel log lines, a scratch file
  named `bisect.py` shadowing the stdlib. Equally, a real kernel bug hid
  behind a plausible-looking test failure -- so confirm which it is
  rather than assuming.

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

## When the user's request is small

Not every message needs the full six-step ceremony. A one-line
copy-edit or an obvious typo fix doesn't need an `AskUserQuestion` round
or a `docs/decisions.md` entry -- and its `CHANGELOG.md` entry can be a
sentence, not a multi-part writeup. Use judgment on ceremony the way
the project itself does (compare how short a docs-only or one-line fix
entry is against a real feature's multi-paragraph one, in any recent
stretch of `CHANGELOG.md`) -- the sequence above is the shape for a
real feature or fix, not a rulebook to apply uniformly regardless of
size.
