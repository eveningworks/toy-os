# CLAUDE.md

Guidance for Claude sessions working in this repo. This is *not* a
restatement of the architecture -- `README.md` and `apps/README.md`
already cover that in depth (kernel layout, the syscall/process model,
the apps/ boundary, the window manager, the persistent filesystem).
Read those for "how toy-os works." This file is for the things that
aren't written down anywhere else: environment quirks, workflow, and
conventions that are easy to violate by accident.

## What this is

A small x86-64 OS (Multiboot2/GRUB-booted, freestanding C + NASM) with
ring0/ring3 separation, per-process paging, an ELF64 loader, syscalls,
a preemptive scheduler, a kernel-space window manager, and two
disk-backed filesystem (TFS3, mounted from an MBR/GPT partition or
from a flat volume; FAT32 is planned). No cross-compiler needed -- host and target are
both x86-64, so plain system `gcc`/`ld`/`nasm` with freestanding flags
work.

## Before you edit

- **Pull first.** This checkout is worked on from more than one session.
- **Read in this order**, and stop when you have what you need: this
  file (the rules and the trap index), then
  **`docs/conventions/<area>.md` for the area you are about to touch**
  -- this file indexes every convention by headline and that file
  carries the body -- then `docs/decisions.md` for "why is it like
  this" (**its INDEX first**, which is what makes it usable at all),
  then `docs/roadmap.md` for whether the thing is already known broken.
  Anything drawn adds `docs/gui-guidelines.md`, which is binding.
- **THE CODE WINS OVER A DOC THAT DISAGREES WITH IT.** A doc records
  what was true when someone wrote it; the code is what runs. So when
  the two conflict, believe the code, FIX THE DOC in the same change,
  and say plainly in your response that you did -- a silent fix leaves
  the next session re-deriving the same contradiction. This is not a
  licence to skip the docs: they are right far more often than not, and
  the ones that were wrong are wrong in ways worth recording.

## How the user wants to collaborate

Standing preferences from working on this project, independent of the
technical conventions below:

- Offer a few real choices before doing something non-trivial, unless
  the right path is genuinely unambiguous -- e.g. a data-layout
  tradeoff, or how far to build something this session vs. just
  planning it. Don't silently pick one approach when there's a real
  fork. A one-line obvious fix doesn't need this.
- Keep chat compact and terse -- only info that actually matters.
  Don't restate what's visible in a diff or build log, don't pad
  explanations.
- Before adding a new feature to a GUI app, consider whether it should
  be a reusable `userland/ui/` widget instead of a one-off (see this
  file's own note on widgets above) -- and ask the user first either
  way, don't decide unilaterally.
- Act like a genuinely experienced OS/UI designer, not a generic
  coding assistant bolted onto a hobby project -- if there's an
  established better way to do something (a real OS's approach, a
  better data structure, a cleaner API shape), say so and suggest it,
  rather than only doing exactly what's literally asked.
- **SAY WHAT REAL SYSTEMS DO, before proposing a design.** Name how
  Linux and Windows solve it -- and for anything on the desktop side,
  Wayland and a compositor that implements it (KWin, Mutter, wlroots),
  or X11 where the history explains the shape. Then say whether toy-os
  should follow or deliberately differ, and why. Having something to
  compare against is what turns "this seems fine" into a judgement.

  Where to look, by area: kernel and process model -> Linux, Windows NT;
  filesystem -> ext2/3/4, NTFS; compositor and windowing -> Wayland
  (+ KWin/Mutter/wlroots/XFCE), X11; settings -> sysctl, dconf/GSettings,
  macOS `defaults`, the Windows registry; init and services -> systemd,
  launchd; IPC -> D-Bus, Binder, Mach ports.

  This has repeatedly changed decisions here rather than decorating
  them: kernel stacks got a guard page because that is
  `CONFIG_VMAP_STACK` and Linux had toy-os's exact bug before 4.9;
  settings are `(namespace, name)` because sysctl, GSettings and
  `defaults` all namespace and a flat global name was the outlier;
  a revoked compositor mapping is poisoned rather than unmapped, which
  is the problem `wl_buffer.release` exists for; the cursor is named by
  the client and drawn by the compositor because Wayland added
  `cursor-shape-v1` to undo the opposite.

  Two cautions. **Copy the SHAPE, not the size** -- these systems carry
  decades of compatibility baggage toy-os has no reason to inherit, and
  "Linux has 400 syscalls" is not an argument for anything. And **check
  the claim before leaning on it**: "Linux can switch schedulers" is a
  common misreading (it has compile-time classes; `sched_ext` is an
  escape hatch), and a wrong premise argued confidently is worse than
  no comparison at all.

## Conventions worth knowing before editing

The ones below are in full because they fire UNANNOUNCED -- a session
trips them before it knows to look anything up.

- **`kapi.h` is the one header apps include** for kernel capabilities
  (console, keyboard, mouse, timer/RTC, filesystem, graphics). Never
  `#include` a driver header directly from `apps/`, never call
  `inb`/`outb` from app code. If a capability isn't in `kapi.h` yet,
  that's a sign it belongs behind a new function in `kernel/core` or
  `kernel/drivers`, exposed through `kapi.h` -- not a reason to reach
  around the boundary.
- **There is a shared toolkit in `kernel/lib/` -- check it before
  hand-rolling a digit loop, a formatter, a path join, or a
  rasteriser.** Seven headers, all reachable through `kapi.h`, all with
  KTESTs: `string.h` (strings/memory/char classes), `knum.h` (numbers
  <-> strings), `kfmt.h` (`k_snprintf`, `vga_printf`/`klog_printf`),
  `kpath.h` (`k_path_join`/`_normalize`/`_resolve`/`_basename`/
  `_dirname`), `fixed.h` (Q16.16 and trig), `geom.h` (2D rasterising
  plus a small 3D section), `rubberband.h` (band + selection set),
  `ttf.h` (TrueType parsing and glyph rasterising), and `krandom.h`.
  Seven things about them:
  - **`kfmt.h` is one header but TWO files**: `kfmt.c` is freestanding
    and shared with ring 3, the kernel sinks live in `kfmt_print.c`; a
    kernel include in the former silently takes `snprintf` away from
    userland.
  - **`fixed.h`'s angles are in TURNS, not radians**, so `FX_ONE` is a
    full rotation and `fx_sin(FX_ONE/4)` is exactly 1. There is no
    floating point in this kernel (`-mno-sse`).
  - **`geom.h` is deliberately NOT a 3D engine** -- no matrices, faces,
    depth buffer or clipping planes. Its `geom_rotate3` is yaw then
    pitch then roll, a fixed order because rotations don't commute.
  - **`krandom.h` is deliberately NOT a CSPRNG, and
    `krandom_quality()` is how a caller finds that out** instead of
    assuming. The quality enum is ORDERED BY TRUST, and a source that
    cannot exist at boot registers later through
    `krandom_register_source()` -- which is how virtio-rng raises a
    QEMU guest above TSC jitter, and why it SEEDS rather than answering
    each draw. The stack canary is randomized from it at boot -- read
    `kernel/lib/stack_protector.c`'s comment before moving that call.
  - **`ttf.h` PARSES UNTRUSTED INPUT, and every read in `ttf.c` is
    bounds-checked for that reason** -- a font file is attacker-shaped
    data being parsed in ring 0, which is the surface Windows spent a
    decade of GDI CVEs on before moving it to a sandboxed user-mode host
    (`docs/decisions/drivers.md` says why toy-os differs anyway). It
    allocates NOTHING: the ~69 KB of working state is a caller-supplied
    `struct ttf_scratch`, which is what lets one implementation serve
    ring 0, ring 3 and a test. Glyph coverage comes out in exactly the
    layout `font_ttf.h`'s baked tables use, so an atlas is a drop-in.
  - **Draw through the `gfx_draw_line()`/`gfx_draw_circle()`/
    `gfx_fill_ellipse()` wrappers in the kernel and `uui_canvas` in
    ring 3**, not `geom_*` directly (both handle the plot callback; the
    canvas also clips). `geom.c`/`rubberband.c` are **compiled TWICE
    from one source**, kernel and userland, so neither may reference
    anything kernel-only -- `geom.h` draws through a **callback**, never
    into a framebuffer, which is what lets one implementation serve the
    kernel, a ring-3 app and a test with no display at all.
  - **A formatter that doesn't fit its buffer writes NOTHING** rather
    than a truncated (i.e. wrong) value, and **a parser REJECTS rather
    than guesses**. Adding follows this file's usual bar: **a second
    real caller, not a plausible one.**
- **A widget's `ops->hit` is a BOOLEAN, and a widget whose own
  `_hit()` returns a ROW INDEX must convert it.** The router tests it as
  `!it->ops->hit(...)` (`userland/ui/uui_route.c`), so returning the
  index makes ROW 0 -- the one row whose index is falsey -- report "not
  hit", and that row silently cannot be clicked while every other row
  works. Write `>= 0`.
- **A widget's `natural_size` must not depend on where the widget
  currently IS.** Natural size is the size a widget WANTS, asked before
  anyone knows where it goes; measuring from the ORIGIN instead of as an
  extent is a feedback loop between layout and measurement.
- **A lone `uui_button` routes its own clicks** -- `press`/`motion`/
  `release` are on `uui_button_ops`, as on QPushButton and a Win32
  BUTTON. `uui_button_group` still routes its own buttons and is worth
  keeping only for a GRID of them (Calculator's keypad);
  `docs/roadmap.md` has retiring it as an item.
- **A layout CAN grow a child along its stacking axis.** `UUI_FILL_H` in
  a column (and `UUI_FILL_W` in a row) absorbs the leftover space,
  flexbox's flex-grow.
- **A PAGE THAT CAN OVERFLOW GOES IN A `uui_scrollview`, and the
  chrome stays outside it.** `uui_layout` does not shrink children
  below their natural size -- given too little room it OVERFLOWS,
  placing the last ones past its bottom edge with no scrollbar and
  nothing to say they are gone (which cost System Settings its status
  bar and six of seven timezones). Wrap the page's layout in
  `uui_scrollview` (`UUI_FILL_W | UUI_FILL_H`) and the app writes no
  scrolling code at all. Three things to know. **Keep tabs and status
  bars OUTSIDE the scroll view** or they scroll away. **One scroll
  region per page** -- a listbox inside one is laid out at full height
  and has nothing left to scroll itself, which is what you want. And
  **a `UUI_FILL` child absorbs a SHORTFALL as well as leftover space**,
  which is what stops a too-tall child evicting its siblings; a
  container with nothing stretchable still overflows. Containers declare
  their items through `uui_widget_ops.children`, and a container with a
  `hit` CLIPS them (that is how a scroll view stops off-screen rows
  being clickable) while a plain layout does not. See
  `docs/decisions.md`.
- **Anything drawn follows `docs/gui-guidelines.md`.** Six things bite
  most often:
  1. **`gfx_draw_string()` does not clip** -- use
     `gfx_draw_string_clipped()` and `gfx_text_width()` for anything in
     a fixed box (the identical overlap bug has shipped twice).
  2. **`on_click` fires on button-DOWN despite its name**, so a control
     that commits there can never be cancelled -- arm in `on_press`, act
     in `on_release`. The menu bar is the documented exception: a menu
     OPENS on press, as on every real desktop, and its items still
     commit on release.
  3. **Esc closes nothing; Alt+F4 closes a window, and the WM handles
     it** -- it never reaches the app, as on Windows and in KDE. All
     three user-facing closes (the X, the context menu's Close, Alt+F4)
     go through one `wm_request_close()`, which ASKS a ring-3 client and
     can be refused from `uapp_desc.on_close`. Route a fourth through
     the same function rather than repeating the client check --
     repeating it is how the context menu drifted into seizing a window
     instead of asking for it.
  4. **`gfx_set_clip_rect()` with a non-positive w/h sets an EMPTY clip
     -- nothing draws -- and only `gfx_clear_clip_rect()` removes a
     clip.** Conflating the two once handed an app's whole `on_draw()`
     an unclipped screen.
  5. **Layout is FONT-DERIVED, never in fixed pixels** -- window sizes
     from each app's `default_size()`, chrome from `gfx_char_h()`, the
     desktop's column pitch from `gfx_char_w()`. That is what makes the
     default font size (`kernel/drivers/gfx.c`) a genuine one-line
     change. What does NOT reflow is a hardcoded pixel constant in a
     test tool -- prefer `DebugConsole.menu_row(label)` over
     `gui_flow.py`'s calibrated numbers.
  6. **An app cannot draw outside its own window, and that is enforced**
     -- the WM clips to the content area around every `on_draw()`, and a
     ring-3 client draws into its own buffer with no mapping of anything
     else. The explicit opt-out is `gfx_clear_clip_rect()`, lasting only
     for that paint. UI Demo overdraws on purpose and `uidemo_test.py`
     asserts the marker colour never reaches the screen, so the boundary
     is tested, not assumed.

  And **interaction states come from `enum ui_state` / `ui_state_bg()`**,
  which derives hover/pressed from the control's own colour: don't
  hand-pick tints, and don't assume hover means "lighter" -- on this
  near-white theme it has to darken, and `gfx_luminance()` decides.
- **Three ways a GUI test passes without testing anything**, each of
  which has shipped a real bug past a green suite (see
  `docs/gui-guidelines.md`): (1) **"it responds" is not "it is drawn"**
  -- the ring-3 Calculator shipped with no visible buttons at all,
  because every check asserted that clicking one changed the display,
  which it did; (2) **moving identical content is pixel-identical** --
  a scroll test typing forty copies of one line cannot tell a working
  scroll from a dead one, so number the rows; (3) **a test must not
  assume the thing it is testing** -- reaching a known start position by
  scrolling, in order to test scrolling, proves nothing. The general
  form: ask what a broken version would still pass.
- **A positive control can turn nothing red because the test's DATA
  never reached the code under test.** The truncate tests wrote 16 KB,
  which fits TFS3's twelve DIRECT pointers, so disabling the
  indirect-table handling changed no result. When a control fires
  nothing, suspect the fixture before the harness, and ask what input
  size/shape actually reaches the branch.
- **A screendump compared against another screendump must be a SETTLED
  frame** -- `QMPSession.stable_pixels()` (two identical consecutive
  reads). A client that has drawn into its buffer, and even logged that
  it did, has not necessarily been composited yet, and that process hop
  varies with load. Do not use it on a window you expect to animate. The
  sibling trap: **a poll whose exit condition is weaker than what the
  code after it needs is a flake** -- one tool waited for the first of N
  layout lines and then required all N.
- **Verify GUI changes by reading PIXEL VALUES, not by looking at the
  screenshot** (`tools/pixel_probe.py`). A hover state that moved the
  background by two units out of 255 looked perfectly plausible in a
  PNG and was invisible in practice; the number is what caught it.
  Always sample a control that should NOT have changed as well -- half
  the assertion is the neighbour staying put.
- **A HOVER STATE CANNOT BE TESTED WITH `gui move`, and the tool for it
  is `DebugConsole.hover_frames()`.** An injected pointer position
  overrides the mouse for the ONE `wm_run()` iteration that consumes it,
  and the next one reads the driver again and snaps back -- right for a
  click (press and release are edges), useless for a state that must
  survive a capture, so the frame photographs the screen after the
  pointer left and a working hover reads as dead. `hover_frames()` warps
  the REAL cursor (confirmed against the WM, since `goto()` is open-loop
  and the WM ACCELERATES the delta) and returns two SETTLED frames;
  `changed_rows()` compares a band of rows for a control whose rows have
  no reported geometry. **Assert the BAND, not the change** -- one band,
  containing the pointer, no taller than a row -- and remember that
  SELECTION OUTRANKS HOVER, so hovering the selected row measures
  nothing.
- **THERE IS ONE LINE EDITOR AND IT IS COMPILED TWICE.**
  `kernel/lib/klineedit.c` also builds into `libuapp.a`, so `/bin/tosh`
  and the ring-3 GUI Terminal edit with the SAME code as the physical
  shell (buffer/cursor/kill ring/undo/keymap); each front end only
  paints the result. **Don't add an editing key to one front end** --
  add it to the core's keymap and all three gain it. Four things to
  know. **A byte off fd 0 is fed in as-is** -- specials arrive as
  0x91-0xA6, which ARE the `KEY_*` codes `kline_key()` switches on, so a
  translation layer would be a third place to drift; Ctrl/Alt reach apps
  as control codes and an ESC prefix, terminal-style, NOT as `KEY_*`
  (see `keyboard.h`'s "Ctrl and Alt" comment). **The console front end
  repaints with `\r` and TWO passes**, because `vga_cursor_move()` is a
  non-destructive seek ring 3 cannot reach and must not get a syscall
  for; the cost is that a line longer than the console is wide repaints
  wrongly, which is the TTY layer's problem. **History is the FRONT
  END's** (`userland/lib/uhistory.c`), as are Tab and Ctrl-R -- **Tab
  works in both rings now** (`kernel/lib/completion.c`, compiled twice
  behind a `completion_env`); Ctrl-R still does nothing in ring 3, since
  it needs a second prompt line and a caret control `redraw()` has not
  got. And **the shared CASE TABLE is what checks the second
  build**: `kernel/include/api/klineedit_cases.h` runs as a KTEST in
  ring 0 and as `/tests/klineedit_test` in ring 3, because the existing
  KTESTs cover the logic and would pass whether or not ring 3 could link
  a byte of it. Add a case once; both rings assert it.
- **`userland/wm/wm.h` is a second, peer-level boundary**, not part of
  `kapi.h` -- it's the GUI-specific equivalent, included by GUI apps
  for `window_*` helpers. `kapi.h` never includes `wm/wm.h` or
  `gui_apps.h`.
- **A WIDGET'S OPS TABLE IS THE CONTRACT, AND A MISSING SLOT FAILS
  SILENTLY AND AT A DISTANCE.** A table with no
  `natural_size`/`set_geometry` is never positioned or measured in a
  layout, and **the router names a widget to its app only when that
  widget has a `release` op** -- so a checkbox toggles on screen and the
  app is never told. **Fill a new widget's table against
  `uui_widget.h`, never against the widget you copied** -- a copied
  table inherits its gaps. And **when a layout misbehaves, check the ops
  tables of everything in it before suspecting the layout**: three short
  tables presented as `uui_layout` stopping after four children, and
  `uui_layout_run()` has no early exit at all. **AND THE INVERSE
  HAPPENS TOO -- a slot that is PRESENT and read by nobody.**
  `accepts_focus` was declared by five widgets while
  `uui_focus_next()` walked to the next index unconditionally, so a
  listbox with no rows was still a tab stop that did nothing; found by a
  positive control that disabled it and watched Tab arrive anyway. When
  you add a slot, grep for the code that is supposed to consult it.
- **THE FILESYSTEM IS NOT RE-ENTRANT, and `vfs.c` holds a preemption
  guard because of it.** `tfs3.c` walks directories, inodes and data
  through module-level scratch buffers, and a ring-3 process is
  preemptible inside a syscall, so two interleaved reads overwrite each
  other's block (it presented as the WM reporting files that plainly
  exist as missing, on about one boot in three under KVM). `FS_OP()`
  wraps every backend call in
  `scheduler_preempt_disable()`/`_enable()`, at the VFS because that is
  the one place every caller passes through. Three things to know: it
  does NOT make an `fs_list()` callback safe to call `fs_*` from (that
  is recursion, which a depth counter cannot see); it is NOT the
  nested-read refusal below, which protects one buffer during one call
  rather than the backend's state across the whole call; and an
  unbalanced `disable()` hangs the machine, which is why `_enable()`
  clamps at zero instead of going negative and silently disarming the
  next section. See `docs/decisions.md`.
- **Prefer `fs_read_into()` to `fs_read()` in anything the kernel
  context parses.** `fs_read()` hands back a pointer into a shared
  staging buffer, and that contract is unstatable in a preemptible
  kernel -- a caller can honour it perfectly and still lose the buffer
  to a ring-3 syscall mid-parse (`cursor_theme.c` did, with a comment
  reasoning the parse happens first: true of the function, not of the
  machine). `fs_read_into(path, buf, cap)` reads into memory the caller
  owns, so there is no shared buffer to invalidate, and REFUSES an
  oversized file rather than truncating. The same shape exists for
  config files: `etc_config_load()` + `etc_config_buf_get()` read once
  and answer many keys, because `etc_config_get()` re-reads the whole
  file PER KEY -- which made a nine-entry desktop reload 54 whole-file
  reads.
- **`fs_read()` REFUSES a nested whole-file read**, returning NULL as it
  does for a missing file, because every backend frees one shared
  staging buffer and does a BLOCKING read into it -- so a preempted read
  has its buffer freed underneath it. **The cost to know**: a refusal
  looks exactly like "no such file" at the call site, so it is logged.
  See `docs/decisions.md`.
- **Split a file once it's grown big enough to be genuinely harder to
  work with -- don't wait for it to become unmanageable, but don't
  split preemptively either.** There's no hard line-count rule; the
  signal is practical: a file mixing more than one real concern (event
  handling + rendering, like `wm.c` before its split), or long enough
  that finding the right part of it gets slow and error-prone. **Don't
  calibrate against a line count quoted in a doc** -- those rot; run
  `wc -l` on the actual tree for today's numbers. A hand-written file
  pushing toward a couple thousand lines is the point to seriously
  consider a split, not a hard trigger. This deliberately excludes
  *generated* data files like `kernel/drivers/font_ttf.c` -- the concern
  there is regenerating them correctly (`tools/genttf.py`), not
  readability. When a split does make sense: follow the `userland/wm/`
  pattern (split by concern, share state through a `_internal.h` of
  `extern`s if it's still fundamentally one component, not a real
  boundary -- see `docs/decisions.md`) rather than inventing a new
  pattern each time, and record the split's own reasoning in a
  top-of-file comment.
- **`tools/check_widget_ops.py` FAILS THE BUILD on a widget ops table
  with a slot it needs left NULL**, because four widgets shipped that
  way in one day and every one of them failed silently and at a
  distance. A table with `draw` needs `natural_size` and `set_geometry`
  (a layout cannot place what it cannot measure); a table with `press`
  needs `release` (`uui_route.c` names a widget to its app only when it
  has one); a table with `key` needs `accepts_focus` (the focus ring
  SKIPS a widget that refuses focus, so one that takes keys has to say
  whether it wants them -- that rule found `uui_tree` relying on the
  default the hour it was added). Waive with a `widget-ops-ok: <reason>`
  comment, as with `check_dispatch.py`.
- **AND `tools/check_key_routing.py` FAILS THE BUILD ON THE INVERSE:
  A FILLED SLOT NOBODY CAN REACH.** An app whose `uapp_desc` names a
  widget with a `.key` op must declare `.focus` or `.on_key` -- the only
  two doors in `uapp.c`. Task Manager declared neither, so `uui_table`'s
  arrows, paging and type-ahead were dead there for months while every
  check in its test drove by MOUSE. The key-capable set is DERIVED from
  `userland/ui/*.c`, so a new widget is covered the day it gains a key
  handler. Waive with `key-routing-ok: <reason>`.
- **A DISPATCH CHAIN OVER ~20 BRANCHES SHOULD BE A TABLE, and
  `tools/check_dispatch.py` fails the build when one isn't.** The
  recurring shape here: something dispatches on a kind -- a syscall
  number, a command name, a message type -- as one `if/else` or
  `switch`, and it grows by a branch per capability until the handlers
  have nowhere to live but beside it. This project already has the right
  pattern and applied it inconsistently (`display_driver`,
  `block_device`, `clocksource`, `struct setting`, `.ktests`,
  `syscall_table.c` are all registries or tables); what was missing was
  anything that noticed the growth. **Waive with a `dispatch-ok:
  <reason>` comment** when the branch set is genuinely BOUNDED -- a
  keymap is bounded by the keyboard, a PCI class table is data -- and
  note the reason is the mechanism, not the waiver. The two waived are
  `klineedit.c`'s keymap and `apps/shell.c`'s command chain, the latter
  deliberately NOT converted: most of those commands are kernel
  introspection, so the table they want is a `/proc`-shaped interface
  reached once the shell moves to ring 3, and converting first would
  build the wrong table (see `docs/roadmap.md`).
- **NEVER WRITE A HAND-AUTHORED FILE INTO `seed/sync/` -- IT IS BUILD
  STAGING, AND THE SOURCE IS `data/`.** It is gitignored and `make
  clean` deletes it wholesale (and `preflight.sh` STARTS with a `make
  clean`), so a file written there is never committed, survives until
  the delivery gate, and then ships ABSENT with nothing failing. That
  has happened three times: the cursor themes fell back to built-in
  shapes, and a new app's `.desktop` entry went missing so the app was
  installed and unlaunchable while its binary and icon -- which come
  from `build/` and `data/` -- were both present. A Start-menu entry
  goes in `data/wm/desktop/`, an icon in `tools/gen_icons.py` ->
  `data/icons/`, a font/wallpaper/cursor theme in its own `data/`
  directory; `make iso` stages them. `tools/check_layout.py` FAILS the
  build on a staged file with no tracked source behind it and names the
  directory it belonged in. The generic check: after adding a data file,
  confirm `git status` shows it.
- **`kernel/include/` is split by audience and the build enforces it**
  -- `api/` (what `apps/` may use), `abi/` (the kernel<->userland
  contract `userland/` shares), `kernel/` (internal, and NOT on
  `apps/`'s include path, so reaching for one is a compile error rather
  than a review catch). See `kernel/include/README.md`, including where
  a new header starts life (`kernel/`, moving to `api/` only when an app
  genuinely needs it).
- **PREFER FACTS THAT CANNOT GO STALE. Do not cite a number that some
  other file has to keep true.** Every maintenance burden this repo has
  deleted was the same shape: a pointer to a number -- build numbers,
  milestone numbers, target versions on roadmap items, test counts in
  prose ("192 GUI checks across 14 tools", wrong within weeks, twice).

  **What IS safe to point at**, and these four carry most of the load:
  a named file or symbol in the code; a named section in
  `docs/decisions.md`; a named milestone in `docs/roadmap.md`; a named
  rule in this file or in `.claude/skills/toy-os-feature-workflow/`. All
  are addressed by TITLE, so they survive edits, moves and reordering.

  Name the THING, not its index: "see `uui_route.c`'s pointer grab", not
  "see build 412"; "the GUI-in-ring-3 milestone", not "M41". If a count
  genuinely helps, say what it measures and accept it as a snapshot
  ("~300 checks across 23 tools"), or leave it out.

  **The split, stated once: for the PRESENT point at a title; for the
  PAST point at a COMMIT.** A state has a name and keeps it; a change
  does not. A short SHA is the one number here that cannot go stale, but
  pair it with what it did -- "the poison-page fix (978ebf7)", not "see
  978ebf7". The one hazard: a history rewrite invalidates every SHA, and
  this repo has done one.

  This is not a rule against cross-references -- `docs/decisions.md` and
  source comments point at each other constantly, because both ends are
  named things. It is a rule against cross-references whose correctness
  depends on someone remembering to update a third file.
- **A comment's length should track how SURPRISING the code is and how
  dangerous it is to change -- not how much history it accumulated.**
  Two things earn length: **the invariant** (what must stay true) and
  **the trap** (what breaks if you edit this the obvious way). This
  codebase leans hard on comments and mostly earns it -- the note above
  `damage_cursor()` about what `prev_cursor_*` actually records exposed
  a real bug weeks later. Keep writing those.

  Four questions, because this rule was broken by a session that had
  read it: would the sentence be true even if nobody had got it wrong
  first (if not, it is history)? Does `docs/decisions.md` already say it
  (then don't repeat it)? Is a real system being NAMED (a clause) or
  used to JUSTIFY (a paragraph, and it belongs in `docs/decisions.md`)?
  And more than ~6 lines on a struct field, a `#define` or a small
  static function is a smell unless it is a genuine trap. There is a
  worked before/after in the workflow skill's `doc-templates.md`.

  What does NOT earn it is the war story. **Cap the anecdote at one
  clause** -- "(a missed declaration left a second cursor on screen)"
  persuades exactly as well as the forensics, and the forensics are in
  `git log`. Existing long comments in the SOURCE are deliberately not
  being retro-trimmed: a bulk rewrite's most likely casualty is the one
  sentence that saves a future session.
- **THERE IS NO CHANGELOG**, and **where the four kinds of thing go**:
  - *What changed, file by file* -- the COMMIT MESSAGE, which lists
    every changed file with a one-line note. `git log` is the record.
  - *How the mechanism works, and the trap in it* -- a comment next to
    the code. This is what gets found by whoever edits it.
  - *Why this way and not the obvious way* -- `docs/decisions.md`,
    written IN FULL there rather than as a pointer elsewhere.
  - *What is BROKEN* -- `docs/bugs.md`. One line each, with the
    reproduction under a matching heading in `docs/roadmap-details.md`.
    A fixed bug is DELETED from there rather than struck through (the
    opposite of the roadmap's rule, and deliberate: a bug list is only
    useful as a list of what is still wrong; `git log` records fixes).
    Say the RATE for anything intermittent -- "5 boots in 9" is a
    measurement, "sometimes" is a shrug -- and say plainly when a cause
    was never established. PRE-EXISTING means MEASURED against an
    earlier commit, not assumed.
  - *What is NOT BUILT YET* -- `docs/roadmap.md`. Its "Known
    limitations and papercuts" section is for things that work as
    designed and could be better; anything actually misbehaving belongs
    in `docs/bugs.md` instead. The test is "is something broken?", not
    "would I like this to be better?".

  It was stopped because the same reasoning was being written three
  times, and deleted rather than frozen because a frozen file still has
  to be reasoned about by anyone editing near it. What was lost, stated
  rather than glossed: commit messages before the freeze are one-liners,
  so for pre-2026-08-15 work the changelog was the only detailed
  account of a CHANGE -- the decisions themselves are in
  `docs/decisions.md`.
- **A COMMIT MESSAGE IS PROBLEM, THEN CHANGE, THEN FILES -- not an
  essay.** Imperative subject under ~72 chars, prefixed with the area
  (`settings:`, `wm:`, `kernel:`); one or two short paragraphs saying
  what was wrong and what caused it; a bullet per change; then every
  changed/added file with a one-line note. **No capitalised lede
  sentences, no war stories, no forensics** -- the reasoning has three
  better homes (`docs/decisions.md` for why-this-way, a comment beside
  the code for the trap, `docs/conventions/` for the rule), and a
  commit repeating them is the third copy. Bodies before 2026-08-24 are
  in the old essay voice and are deliberately not rewritten. See
  `docs/decisions.md`'s versioning entry for the worked example.

## Conventions indexed here, written up in `docs/conventions/`

**Every headline below is a real rule; the body is one file away.**
CLAUDE.md is the always-loaded context, so it carries the RULE and
`docs/conventions/<area>.md` carries the reasoning and the trap -- the
same split this file already asks for everywhere else. **Read the file
for the area you are touching before you edit it**, and read the entry
whenever a headline here tells you something you did not already know.

### The kernel: syscalls, memory, processes, init

`docs/conventions/kernel.md`

- **Monotonic time is an INTERFACE, and wall clock is not one of its implementations.**
- **The kernel's idle work has ONE owner: `scheduler_idle()`**
- **EVERY KEY REPORTS SOMETHING, AND THE KEYPAD REPORTS CHARACTERS** -- Insert, Menu, the locks, Pause, Print Screen and the keypad used to produce nothing at all; the function row is complete F1-F12; the keypad emits its keycaps' characters rather than new codes; NumLock's off-state is deliberately not modelled; Pause has no release; the fake shifts around Print Screen are dropped.
- **USB IS xHCI ONLY, ITS PORTS WAIT ON PED RATHER THAN PRC, AND EVERY DMA OBJECT IS ITS OWN FRAME** -- `kernel/drivers/usb/`; no HCD ops table (one implementer); PORTSC is seven RW1C bits plus a write-1-to-DISABLE one; an interrupt ack writes back ONE bit, never the register; a context entry is 32 or 64 bytes and nothing may assume; `input_report_rel()` wants UP-positive dy, so HID negates. `USB=xhci` is off by default because attaching a `usb-kbd` takes the keyboard away from PS/2.
- **INPUT DEVICES REGISTER WITH THE INPUT CORE, and the canonical event is evdev -- including `/etc/kbs`, so only the PS/2 driver ever sees a scancode**
- **`kbd` PRINTS EVERY STAGE OF A KEYPRESS, AND ITS KERNEL LOG IS OFF BY DEFAULT** -- all four encodings on one line (PS/2 scancode, evdev keycode, character, modifiers), from a ring the driver fills. **`kernel.kbdtap` is OFF unless a human turns it on, and turning it off WIPES the ring**, because a buffer of the last ~128 keystrokes is a keylogger and `SYS_QUERY` has no privilege check; `kbd`'s live mode arms it for its own duration and disarms on every way out. Keyboard only; the tool never reads a keyboard, which is what lets it run in a Terminal window without stealing a key from the desktop.
- **A GUEST SPIN-WAIT NEEDS `cpu_relax()` (`pause`), AND UNDER KVM THAT IS NOT AN OPTIMISATION** -- KVM's Pause-Loop Exiting triggers on that instruction, so a spin without one keeps its whole timeslice and STARVES THE HOST THREAD IT IS WAITING FOR. It hung virtio-blk's first FLUSH of every run under KVM+SDL while reads and writes worked. Only the VIRTQUEUE is exposed: `chain_done()` reads plain guest RAM, while `ata.c`/`ahci.c` poll MMIO and always trap. A TCG-only suite cannot see this class. **EARN IT, don't do it unconditionally**: pausing from the first iteration is free on an idle host and cost a THIRD of write throughput on a busy one, because yielding means waiting for a real reschedule -- spin tight first, back off only once the wait is clearly long.
- **VIRTIO INTERRUPTS ARE OPT-IN, a forgotten ISR read hangs the machine, and ENABLING IS THE LAST STEP** -- `virtio_intx_line()` then `virtio_intx_enable()`, publish first and enable last; a device that can interrupt before its handler can see it hangs the machine exactly as a forgotten ISR read does, and only under KVM.
- **USING A SUBSYSTEM BEFORE ITS init() IS A PANIC, not a soft failure**
- **A panic NAMES THE FUNCTION**
- **A kernel panic prints enough to diagnose from a pasted log**
- **Kernel stacks are 16 KiB, have a GUARD PAGE, and carry a CANARY**
- **A FAILED SYSCALL RETURNS `-ERRNO`, AND `-1` IS `-EPERM`.**
- **FILE DESCRIPTORS ARE TWO LEVELS, AND 0/1/2 ARE ORDINARY ENTRIES.**
- **A FULL PIPE BLOCKS ITS WRITER, AND A CHILD INHERITS ONLY 0/1/2.**
- **One process can run another and read its output**
- **Per-process facts exist, and Task Manager is a ring-3 app.**
- **`meminfo audit` COMPARES PAGE TABLES AGAINST THE ALLOCATOR.**
- **THERE IS A PROCESS TREE: `ppid`, reparenting, and `waitpid(-1)`.**
- **THERE IS AN INIT, IT HOLDS PID 1, AND IT CANNOT BE KILLED.**
- **INIT STARTS AND SUPERVISES THE DESKTOP, and the desktop is a SERVICE.**
- **A SERVICE CAN SAY IT IS READY, AND `After=` THEN MEANS "USABLE" RATHER THAN "SPAWNED".** -- `SYS_NOTIFY_READY` sets a bit the kernel does nothing with; `Ready=notify` in a descriptor makes init wait for it. A syscall rather than a socket or a pipe because neither ports (no unix sockets; `PIPE_MAX` is 8 kernel-wide and a pipe carries no credentials). The barrier ALWAYS expires -- `ReadyTimeout=`, then init says so and starts the dependents anyway.
- **A SERVICE IS CONTROLLED BY A FILE PLUS A DOORBELL, AND `/bin/service` IS THE LEVER** -- `list`/`status` read `/tmp/init.status` (init is the only thing that can say a service is down ON PURPOSE), which is PUBLISHED ON DEMAND and only on a SETTLED pass, partly because **a filesystem write while the desktop is STARTING UP wedges the compositor** (`docs/bugs.md`); `start`/`stop` append to `/tmp/init.ctl` and send `SIGHUP`, runit's `supervise/control` plus SysV's `kill -HUP 1`. The signal cannot be the message and the file cannot be the signal. **`sys_waitpid()` RETRIES `-EINTR`** -- `sys_waitpid_intr()` is the wait a doorbell can reach. An admin stop is its own flag and outranks `Restart=always`; `stop` and DELETING the descriptor stay different requests. A descriptor must APPEAR WHOLE (`mv` it in), or a rescan reads it half-written and the ordering key is silently ignored.
- **INIT CANNOT BE KILLED BY A SIGNAL IT HAS NOT CAUGHT, and the guard is in `do_default_action()`, not only `scheduler_kill()`** -- Linux's `SIGNAL_UNKILLABLE`. Signals added a SECOND termination path that takes SYS_EXIT's route when the victim is the running process, so `kill 1` killed init for months while a `kill 1` check asserted `1 in ps`, which a ZOMBIE passes. init still CATCHES what it handles, and a FAULT still kills it (`signal_deliver_fault()` never reaches the default action).
- **`SYS_SLEEP` exists, and a caller with no scheduler slot gets -1.**
- **`ps` is a REAL `/bin` PROGRAM, not a builtin**
- **A PROCESS'S MEMORY IS FREED WHEN IT DIES, NOT WHEN IT IS REAPED -- and killing needs a DIFFERENT entry point from exiting.**
- **A USER MAPPING SAYS WHETHER IT OWNS ITS FRAME, and getting that wrong is silent.**
- **Kernel code touches user memory ONLY through `vmm.h`'s copy helpers**
- **`SYS_WNOHANG` exists, and the bug that produced it is the lesson.**
- **`SYS_SBRK` RESERVES; THE PAGE ARRIVES ON TOUCH.**
- **THE RING-3 MAP IS SIZED FOR 4K, and a region's END is what the next thing must clear.**
- **`SYS_SBRK` is PER PROCESS.**
- **A RING-3 IMAGE HAS NO SIZE LIMIT, BECAUSE THE HEAP STARTS WHERE IT ENDS.** -- derived per process from `elf_load()`'s `out_image_end` (Linux's `set_brk()`), which deleted `link.ld`'s 1 MiB `ASSERT`; the end is the MAXIMUM over segments, not the last one's.
- **THE USER STACK IS RESERVED AND GROWN ON FAULT, and the GAP is what keeps that safe.** -- 8 MiB reserved, four pages mapped, the rest through the heap's own fault hook; a fault more than `UADDR_STACK_GROW_GAP` below the bottom is refused, and that gap plus `-Wframe-larger-than=2048` are one guarantee, not two. Bound anything against `UADDR_STACK_FLOOR`, never the moving bottom.
- **The ring-3 address-space map is `kernel/include/kernel/uaddr.h`, stated once.** -- except the two boundaries that are per process (`heap_base`, `stack_bottom`), which live in `struct sched_mm`.
- **The kernel heap has a debug mode, and it is a RUNTIME toggle**
- **The kernel RELOCATES ITSELF at boot -- it is not running where it was linked.**
- **A BLOCKED PROCESS WAITS ON A CHANNEL, AND A CHANNEL IS AN ADDRESS.**
- **THE KERNEL STORES NO ENVIRONMENT, AND `SYS_SPAWN` TAKES A STRUCT**
- **A SIGNAL SETS A BIT; THE KERNEL ACTS ON IT WHEN IT IS SAFE TO.** -- except STOP/CONTINUE, which act at SEND time and never touch the pending set; STOPPED is a FLAG beside the state, and a test that reads the flag cannot see the bug. **`pending` is not `deliverable`**, and confusing them swallows a handler's own `SYS_SIGRETURN`.
- **A HANDLER IS RING-3 CODE, AND THE KERNEL BORROWS ITS STACK TO CALL IT.** -- the restorer comes from ring 3 (`SA_RESTORER`, not a vDSO), it must not touch the stack, a signal is blocked inside its own handler, and a fault with no handler still prints the full report.
- **A CHILD'S DEATH RAISES SIGCHLD, AND THE NOTIFICATION HAS ONE HOME.** -- `notify_parent()`, called by BOTH deaths (exit and kill); exit only, never a stop or a continue; and it costs a parent with no handler one compare.
- **A PROCESS GROUP IS AN INT, AND SPAWN TAKES IT.**
- **THE CONSOLE HAS AN OWNER AND A FOREGROUND GROUP, AND THE INTR KEY IS TEMPORARY WHERE IT IS.**
- **A TRACER NAMES ITS CHILD AT THE SPAWN, AND THE TRACE GOES TO ITS TERMINAL.** -- `SPAWN_TRACE` on `SYS_SPAWN`, an unknown spawn flag is `-EINVAL`, and the sink is the tracer's fd 1 (fd 2 here is the KERNEL LOG, not a second terminal stream).
- **A THREAD IS A SLOT WHOSE `tgid` NAMES SOMEBODY ELSE** -- Linux's shape, not NT's: no thread object, no second scheduler entity. The GROUP owns the address space, the fd table (keyed by CR3, so shared for free), the heap, the cwd, the parent link and the process group; the SLOT owns the kernel stack, FP state, trapframe, signal dispositions and thread pointer. **The process dies as a whole** (`exit_group`; a tid is not separately killable), **a thread is not a child** (`waitpid` never returns one), and **the stack is RING 3's** -- `SYS_THREAD_CREATE` allocates nothing, so a thread stack has no guard page and a DETACHED thread's stack is never reclaimed. `scheduler_current_pid()` is the THREAD; `scheduler_current_tgid()` is the PROCESS, and a caller has to know which it means.
- **RING-3 `malloc` TAKES A LOCK; THE KERNEL'S DOES NOT** -- `heap_core.c` is compiled into both rings and has ONE free list, and `heap_os_lock()` is a real lock in ring 3 (threads are preempted mid-walk) and a no-op in the kernel (nothing preempts kernel code mid-`kmalloc`). The kernel's half stops being a no-op at SMP, where it is split #1. **The race is real by inspection and was NOT reproducible** -- three controls with the lock removed, up to 8000 allocations over a fully-walked list, found nothing; the window is a few instructions against a 100 Hz tick on one core. And `malloc` is not async-signal-safe: the lock is not recursive, so a handler that allocates while its own thread holds it now HANGS rather than corrupts.
- **THE THREAD POINTER IS FS.base, AND THE SCHEDULER RELOADS IT** -- `iretq` leaves the hidden segment bases alone, so without a reload on every switch every thread reads the last-scheduled thread's `__thread` storage, silently. The kernel owns ONE number (`SYS_SET_TLS`, `arch_prctl(ARCH_SET_FS)`'s job); the layout behind it is `userland/rt/tls.c`'s. The LEGACY loader has one too, in the kernel context's own slot -- refusing there kills every ring-3 program in `crt0`, because errno is `__thread` now.
- **ADDING A SYSCALL IS THREE EDITS, AND ONE OF THEM IS A TABLE ROW.**

### GUI, Toykit and the desktop

`docs/conventions/gui.md`

- **MEASURE TEXT, NEVER MULTIPLY: `gfx_char_advance()` / `ugfx_char_advance()`.**
- **A LOADED FACE STILL ONLY DRAWS 101 GLYPHS.**
- **`font glyph <char>` SHOWS WHAT WILL ACTUALLY BE DRAWN, AND IT READS BOTH SIDES.** -- `/bin/font`: one glyph's coverage map, line box and ink, from ring 0 (`QUERY_FONTGLYPH`) and from a client's own atlas mapping, compared by hash. `ink NONE` is the point; `peak` is the harder half (a glyph can have ink and be far too faint). The client half needs a compositor AND a scheduled process. A codepoint (`0x20`) is accepted because a space cannot be typed as an argument.
- **A FONT IS A HANDLE IN RING 3 AND A CONTEXT FLAG IN RING 0.**
- **THERE ARE TWO FONT TIERS, AND THE SHARED ONE CANNOT GROW TO COVER THE OTHER.**
- **BOLD IS A WEIGHT OF A FAMILY, AND A FAMILY IS A FILENAME RULE.**
- **KERNING IS APPLIED BY EVERY TEXT PATH, AND MEASURING MUST MATCH DRAWING.**
- **THE FONT CAN CHANGE UNDER A RUNNING CLIENT, AND `WIN_EV_FONT` IS HOW IT FINDS OUT.**
- **A FONT FACE IS NAMED BY ITS FILENAME, AND `builtin` IS NOT A FACE.**
- **`WIN_CLIENT_MAX_W/H` TRACKS THE DISPLAY CEILING, AND A SCREEN BIGGER THAN IT BREAKS MAXIMIZE SILENTLY.**
- **A DESKTOP-SIZED WINDOW IS "MAXIMIZED", AND THERE IS NO FULLSCREEN STATE.**
- **`-vga virtio` IS A REAL DISPLAY DRIVER, and nothing else boots it**
- **`apps/ui/` IS GONE, and the GUI toolkit is `userland/ui/`** -- the kernel image contains no widget code at all.
- **Ring-3 GUI apps are written against Toykit's `uapp`, and a new one is a `.c` file in `userland/gui/` with NO Makefile edit.**
- **A MENU BAR IN AN APP WITH ROUTED WIDGETS MUST BE `uui_menubar_ops`, NOT HAND-ROUTED** -- the router runs before an app's `on_press`, so a hand-routed popup's first row is also a click on the widget under it; the ops table declares `overlay_active` (offered every press first, no hit test) and paints from `draw_overlay` (after every widget's `draw`). A commit is PARKED and taken with `uui_menubar_take_code()` from `on_widget`, because the ops `release` slot has nowhere to return a code. Keys stay the app's -- there is no `key` slot -- and **F10 REVEALS a hidden bar as well as opening it**, since Konsole's Ctrl+Shift+M folds to Shift+Enter here.
- **A TAB IS A SESSION, AND `uui_tabs` IS THE STRIP** -- its own pty, shell, grid, scrollback, alternate screen and title; Konsole's model, because only the app can give a tab a terminal's state. The CALLER owns the tab array and a label must outlive the widget. **The SELECTED tab is a LIGHT LIFT with an accent bar on TOP** -- the theme's field colour against CONTROL-coloured resting tabs, a thirty-unit lift; the accent was on the BOTTOM edge (least contrast there, against a black page) and the lift was twenty, which shipped and was reported as hard to see. **Tabs are NUMBERED past one** (`numbered`, Konsole's `%n: %d`) -- terminal labels collide, since every tab in one directory reports the same title. **A NEW TAB IS APPENDED and the strip's order is NOT slot order** -- slots are recycled, so a slot-ordered strip appends only until a middle tab is closed and then drops the next new tab into the hole; the SELECTION follows the slot rather than the index for the same reason. A default title is `Shell` with no number, since the strip numbers by position and a slot is recycled. Filling it with the PAGE's colour (Windows Terminal's merged tab) was built and REJECTED -- against near-white chrome a black page makes the selected tab a black block. **The `+` is `show_new`/`on_new`, pinned at the RIGHT END**, and its width comes out of the strip BEFORE the tabs share what is left. **One tab still shows the strip**, deliberately unlike Konsole, because the strip carries the `+`. **Each session has a READER THREAD** that touches only its SPSC ring and calls `uapp_post()`; `tick_ms`/`on_tick` are GONE, and a full ring makes the reader wait rather than drop. Bindings are Konsole's: Ctrl+Shift+T/W, Ctrl+PgUp/PgDn -- and a Ctrl+Shift+letter is matched as the CONTROL CODE plus the Shift bit, because the letter is already folded by the time an app sees it.
- **A TITLE COMES FROM THE SHELL, AS AN OSC** -- `ESC]0;text BEL` -> `ANSI_OSC` (`api/ansi.h`), and `/bin/tosh` emits one per DIRECTORY CHANGE, never per prompt (`prompt()` runs several times a repaint). A consumer may ignore it and the physical console does. An OSC with no `;` is DROPPED rather than guessed at; an unimplemented code is swallowed, not reported, or every hyperlink would rename a tab; the title TRUNCATES at `ANSI_OSC_MAX`.
- **TERMINAL IS A TERMINAL EMULATOR, NOT A SHELL WITH A WINDOW** -- it runs `/bin/tosh` on a pty, so the shell in a window is a real process and `Ctrl-C` there is the same code as the console's.
- **THE TERMINAL'S SCREEN IS A GRID, AND THE ANSI PARSER IS THE KERNEL'S COMPILED TWICE** -- `/bin/edit` runs in a window because a full-screen program can address a grid; `kernel/lib/ansi.c` answers for both terminals.
- **An app with a cadence sets `tick_ms` and BLOCKS between frames.**
- **An app refuses its OWN second copy -- the launcher never does.**
- **`uui_table` sorts on a header click, and an app supplies only a COMPARATOR.**
- **A `uui_scrollview` NOTICES when its content's item list changes**
- **`uui_spinbox` IS FOR A NUMBER; `uui_slider` IS FOR AN ORDERED ENUM.**
- **`uui_slider` is for an ORDERED enum**
- **A CONTROL BELOW THE FOLD IS UNREACHABLE, not merely hard to hit.**
- **`on_draw` RUNS BEFORE THE WIDGETS; `on_draw_over` RUNS AFTER.**
- **`uui_meter` IS THE READING WIDGET, AND IT RESERVES EVERY ROW IT COULD USE** -- a caption, a big number in its own font, a unit, a detail line, a bar; no `hit`, because a reading is not a control. Its height must NOT depend on which strings are set: a meter's content is a value that CHANGES, and counting the non-NULL ones drew Disk Mark's tiles straight through their own borders.
- **LONG WORK BELONGS IN A CHILD PROCESS, NOT IN A GUI CLIENT'S EVENT LOOP** -- slicing across `on_tick` is NOT enough, because a slice is bounded only between UNITS and one unit can be unbounded (a 1 MiB transfer is 1024 syscalls: `SYS_WRITE_MAX` is 1 KiB). Spawn a `/bin` program and poll it, the File Manager's `/bin/cp` pattern. **A polled report is a SNAPSHOT, not a log** -- `sys_read` carries 1 KiB, so appended results land past where a poller reads. **The poll is itself I/O**: ~500 ms, not an animation cadence. `uapp_busy_begin()` is for work that is slow and SHORT.
- **A WIDGET ARRAY IS DECLARED TWICE: `uapp_desc.layout` SIZES AND DRAWS, `uapp_desc.widgets` GETS INPUT** -- declaring only the first is a window that renders perfectly and cannot be clicked, silently. And **a lone routed button reports through `on_widget` with the ITEM's id**, not `on_action`, which `uapp.c` fires only from `uapp_desc.buttons` (a `uui_button_group` -- which is why Calculator looks like the opposite example).
- **`uui_label` WRAPS ONLY IF ASKED, AND THE CALLER RESERVES THE ROWS.**
- **`uui_label` is the caption widget**
- **`uui_sidebar` IS THE NAVIGATION WIDGET; `uui_tree` MODELS CONTAINMENT.**
- **`uui_tree` models containment**
- **A SETTING DECLARES ITS CATEGORY, and the sidebar is generated from it.**
- **Control Panel is now SYSTEM SETTINGS**
- **`uui_table` is the multi-column widget**
- **Editable text has ONE implementation of what editing means**
- **A ring-3 app does NOT route mouse input to its widgets -- the toolkit does**
- **The toolkit DRAWS the declared widgets too, popups last.**
- **The GUI stack has names -- use them.**
- **`ugfx` has a SCREEN surface now, and it is the compositor's**
- **The registered compositor can be GRANTED the real framebuffer**
- **`uui_radio_list` arms on press and COMMITS ON RELEASE**
- **A WINDOW'S APPLICATION IDENTITY IS THE KERNEL'S, not the app's**
- **The TASKBAR'S LAYOUT IS ONE FUNCTION, and past a floor it groups by app**
- **The WM has a SLOW-FRAME WATCHDOG**
- **There is a Crash Test app**
- **THE KERNEL CONSOLE STOPS PRESENTING WHILE A COMPOSITOR OWNS THE SCREEN** -- `vga_present()` no-ops on `win_server_any()` and `vga_resume()` repaints on the way out (Linux's `KD_GRAPHICS`); otherwise any ring-3 process writing to fd 1 blits the whole text console over the desktop. **`vga_present_force()` is the override and a PANIC is its caller** -- guarding the routine path alone hides every panic under a running desktop. The console keeps DRAWING, so the text survives to be repainted.
- **A CLIENT NAMES ITS POINTER SHAPE, AND THE COMPOSITOR CLAMPS IT TO THE CONTENT AREA** -- `WIN_REQ_CURSOR` carries a `WIN_CURSOR_*` (`DEFAULT`/`TEXT`/`WAIT`), Wayland's `cursor-shape-v1`; the list a client may name excludes the resize shapes because the frame is not its; the clamp is what stops a wedged client stranding an I-beam over the desktop; a widget declares it through `uui_widget_ops.cursor` and `uapp_set_cursor()` fills the gaps (Notepad's document, the Terminal's grid).
- **THE BUSY POINTER HAS TWO SOURCES** -- `uapp_busy_begin()`/`_end()` for work that is slow on purpose (the toolkit remembers what to restore; does NOT nest), and the COMPOSITOR raising it for a window that stopped answering, which OUTRANKS whatever that window last named because a wedged client cannot name anything. `/tests/hangclient`'s `b` key is busy-and-alive, which is what keeps the two testable apart.
- **EVERY CLIENT IS PINGED ON A CADENCE** -- `WM_PING_INTERVAL_DEFAULT` beside `WM_PING_TIMEOUT_DEFAULT`, levers `gui pingtimeout`/`gui pinginterval`. `wm_client_ping()` used to have ONE caller (the close path), so `(Not Responding)` could only appear while closing. A hung window nobody is closing still raises no DIALOG -- that stays gated on `close_asked_tick`.
- **The cursor's shapes are DATA FILES, and a theme is a directory.**
- **The cursor's drawn extent is DERIVED, not a constant.**
- **A compositor's view of a dead window is POISONED, not unmapped**
- **A ring-3 compositor delivers events through TWP, not by calling the kernel.**
- **`SYS_FS_GENERATION` is how ring 3 asks "has the filesystem changed?"**
- **A ring-3 process can own a real window**
- **`Exec=builtin:` is GONE, and ring 0 contains no applications.**
- **The Start menu and desktop icons are built from FILES**
- **A Start-menu entry launches a RING-3 program**
- **There is no limit on open windows**
- **Super/Win toggles the Start menu, and Alt+F4 closes a window**
- **A window may be dragged off the left/right/bottom edges and UNDER the taskbar**
- **The window manager lives in `userland/wm/`**
- **`win_server_active()` MEANS A RING-0 LAYER, and the desktop is not one.**
- **THE DESKTOP IS A RING-3 PROCESS.**
- **Killing the desktop is survivable, and that is the milestone's exit criterion**
- **A GUI tool that needs the compositor role must ASK WHO HOLDS IT**
- **A client that needs raw input without a desktop cannot be driven by keystrokes**
- **Four things a ring-0 component loses the moment it becomes a process:**
- **COLOURS COME FROM THE THEME, SIZES FROM ITS METRICS -- neither is hardcoded.**
- **A WORKER THREAD MAY TOUCH NOTHING IN TOYKIT EXCEPT `uapp_post()`** -- the widget tree, the canvas and the window buffer are plain memory the main thread may be reading; every real toolkit has this rule (AppKit and Qt widgets are main-thread-only, GTK the same) and none enforce it. A worker computes into memory it owns and posts TWO NUMBERS; `uapp_desc.on_user` runs on the MAIN thread. A client may post only to ITSELF and only `WIN_EV_USER`, which is what keeps "a client cannot synthesise input" true of its own queue. **This does not replace the `/bin` child-process pattern** -- a thread is for work whose RESULT must live in the app's own memory (a decoded image), not for work a `/bin` program could do better.
- **AN APP LOGS THROUGH `ulog()`/`ulogf()`, not a hand-rolled `logf_`.**
- **THE TOOLKIT OWNS THE KEYBOARD FOCUS RING: set `uapp_desc.focus`.**
- **A WIDGET REPORTS ITS RECT THROUGH THE `bounds` OP; a test-facing geometry log is `uapp_log_layout(a, prefix)`.**
- **AN IMAGE IS DECODED IN RING 3, AND `lib/uimg.h`'s CODEC TABLE IS THE EXTENSION POINT** -- never add an image parser to the kernel, and `-ENOTSUP` (a file this build refuses) is not `-EINVAL` (a broken one). Two codecs: JPEG for photographs, QOI for anything needing ALPHA.
- **AN ICON IS A NAME, NOT A PATH, AND IT IS COMPOSITED** -- `Icon=notepad` resolves to `/usr/share/icons/notepad.qoi` through `icon_get()`, which CACHES the decoded and scaled result; blit it with `ugfx_blit_alpha()`, never `ugfx_blit()`, or its transparent corners land as black.
- **A WINDOW'S TITLE BAR CARRIES ITS APP ICON, AND `title_icon()` ANSWERS FOR BOTH DRAWING AND CLICKING** -- resolved from the window's own `app_id` (the client never supplies artwork, as on Wayland); no decode means no rect, so there is never a clickable square with nothing in it; clicking it opens the window menu through the same `wm_open_window_menu()` the right-click uses.
- **TEXT ON A WALLPAPER IS `ugfx_draw_string_shadowed()`, NEVER A GUESSED `bg`** -- `UGFX_TRANSPARENT` blends against what is really on the surface (the one path that reads back), and the shadow's shade is DERIVED from the ink's luminance, because no single ink is legible on every photograph.
- **`uui_image` IS THE ONLY WIDGET THAT OWNS MEMORY, AND IT MUST BE RELEASED.**
- **THE WALLPAPER IS A REGISTERED SETTING, AND ITS VALUE IS A NAME** -- `desktop.wallpaper`, a filename stem under `/usr/share/wallpapers` or `none`; a GUI test measuring ink over the desktop must turn it off first.
- **THE START BUTTON'S APPEARANCE IS A REGISTERED SETTING** -- `desktop.start_button` = `text` | `icon` | `both` (XFCE Whisker's three-way), default `text` so the strip's geometry is unchanged; `start_mark()` in `wm_render.c` is the ONE decision the width, the drawing and `gui taskbar --json` all ask, or a missing `start.qoi` yields an icon-width button with a text label in it.

- **THE TRAY CLOCK OPENS A CALENDAR, AND THE PANEL OWNS IT** -- `userland/wm/calendar_popup.c`, a month grid anchored above the clock with `<`/`>` paging and today in the accent, where Windows/GNOME/Plasma/XFCE all put it; days are not clickable, the panel is always six week rows tall, a dismissing click on the TASKBAR falls through (the Start button acts on it) while one anywhere else is swallowed, and the clock's own rect comes from `tray_clock_rect()` rather than from "the right end of the strip".
- **THE WEEK'S FIRST COLUMN IS A REGISTERED SETTING: `desktop.week_start` = `monday` | `sunday`** -- persist-only in `/etc/desktop.conf` like the wallpaper and the Start button, adopted on the WM's generation poll; a tool that changes it must set it back.

- **A KEY RELEASE IS `WIN_EV_KEY_UP`, AND THE FOUR MODIFIER KEYS ARE KEYS** -- `uapp_desc.on_key_up`, a separate type and callback so a press-only app is unchanged; releases ride a parallel transition queue because a terminal is a byte stream and a release is not a byte; `KEY_SHIFT`/`KEY_CTRL`/`KEY_ALT`/`KEY_ALTGR` exist only there; a release carries what the PRESS produced (first press wins, so autorepeat cannot strand it); an unmatched release is legal; and `wm_rawin.c`'s key slot became a QUEUE, because a dropped release is a key held forever.
- **A SECONDARY CLICK IS THE CLIENT'S INSIDE ITS CONTENT AREA, AND THE WM'S EVERYWHERE ELSE** -- right-click reaches a ring-3 app as button bit `0x2`, the frame/title bar/taskbar keep the window menu (Windows/X11/Wayland's split); Toykit activates widgets on `0x1` alone while `on_press` sees every button.
- **DOOM IS A VENDORED PORT IN `userland/ports/doom/`, LINKED INTO ONE BINARY** -- doomgeneric byte for byte (GPL-2 in an MIT repo, so an aggregation, and the per-binary `EXTRA_OBJS_doom` is what makes "nothing else depends on it" a build property); OUR backend is `userland/doom/`, outside the vendored directory on purpose; the vendored tree compiles with warnings OFF but the frame-size warning ON; and `api/keyboard.h` and `doomkeys.h` CANNOT share a translation unit (both define `KEY_F2`/`F3`/`F4`/`F10` differently), which is why `dg_toyos.h` carries `TOYKEY_*` copies that `doom.c` static-asserts against the real macros. **The IWAD is NOT in the repo** -- `tools/fetch_wad.py`.
- **MINESWEEPER IS THE FIRST GAME, AND IT IS AN ORDINARY CLIENT** -- `userland/gui/apps/mines.c`; it draws its own board rather than adding a `uui_grid` widget for one caller, its board palette is content and not theme, and flagging commits on PRESS.
- **A DIRECTORY LISTING IS A WIDGET, `uui_fileview`, AND FOUR THINGS SHOULD BE DRAWING ONE** -- it composes `uui_table`; the CALLER owns the 20 KB entry array; filtering is a callback (Image Viewer probes magic bytes); `..` and directories lead under every sort; a MARK names a row so every reload clears the marks; Image Viewer is converted and the WM's file picker and Notepad's dialog are NOT yet.
- **THE FILE MANAGER IS A TWO-PANE COMMANDER, NOT AN EXPLORER** -- `userland/gui/apps/files.c`; copy and move between two visible directories need neither the clipboard nor drag-and-drop, and this system has neither. File operations are CHILD PROCESSES (`/bin/cp`, `/bin/rm`) reaped with `sys_waitpid_nohang()`, marked files run through a QUEUE, each pane carries its own path strip, and the pair is remembered in `/etc/files.conf`.
- **WHAT OPENS A FILE TYPE IS DECLARED BY THE APP THAT OPENS IT: `Handles=` on its `.desktop` entry** -- freedesktop's `mimeapps.list` shape with the MIME database left out; matched whole and case-insensitively including the dot; the handler is spawned and NOT waited for, and Notepad takes a path in `argv[1]` because of it.
### Storage, the filesystem, and /etc

`docs/conventions/storage.md`

- **THE CURRENT DIRECTORY IS THE KERNEL'S, and every path syscall resolves against it.**
- **Six filesystem syscalls exist**
- **The disk has a WRITE-BACK CACHE, and its flush can fail**
- **A FACT IS READ THROUGH `SYS_QUERY`, AND ADDING ONE IS A PROVIDER, NOT A SYSCALL.**
- **THERE ARE THREE WORDS FOR SYSTEM STATE AND THEY ARE FIXED: FACT, SETTING, TUNABLE.**
- **Setting a setting to the value it already has does NOTHING**
- **`etc_config.c` is SPLIT: the parser is shared, the file I/O is kernel-only.**
- **EVERY DISK DRIVER RUNS, AND THE ROOT IS A SEPARATE CHOICE** -- all three inits are called unconditionally and each registers what it finds into **block.h's device table** (`ata0`, `ahci0`, `virtio0`, `ram0`; partitions `ata0p3`, numbered by the PARTITION TABLE so they match `parttable`). The root comes from `root=` on the boot line, else registration order. It was `if (!blk_virtio_init() && !blk_ahci_init()) blk_ata_init();` -- a short circuit, so a virtio machine never ran the AHCI driver and its SATA disk did not EXIST, and a live boot skipped disk init entirely so a live session could not see the machine's own drives. **Every test here boots ONE disk, which is the one shape where the bug cannot show.** Three traps: a device created but never made active must still be TRACKED (`blk_track()`, or `/boot`'s partition has no name and cannot be mounted); every disk's partitions are named, not just the root's (`partition_read_table_of()`); and a second volume of one format still will not mount, which is `fs_ops.max_mounts` and not this. `/bin/lsblk` shows the table.
- **THE DISK PRECEDENCE IS VIRTIO-BLK, THEN AHCI, THEN ATA, and each rung has a boot word that steps down to the next** -- decided in ONE line in `kernel/fs/mount.c`; `novirtio` and `noahci` are what keep the lower rungs reachable. `noahci` is NOT a driver kill switch: only `blk_ahci_init()` reads it.
- **AHCI ENUMERATES EVERY PORT AND DRIVES ONE, AND SAYS SO** -- `kernel/drivers/ahci.c`; NCQ and 64-bit addressing are REPORTED, not used (what NCQ needs is an asynchronous block interface, not more AHCI code); NO sector cache, unlike ATA, which is what makes `BLK_CAP_FLUSH` a real FLUSH CACHE EXT. Start the engine only after `PxCLB`/`PxFB` are set and unmask INTx only after the handler is registered; acknowledge **port first, then the HBA**; `CFL` is the FIS length in DWORDS (five), not the 64-byte slot. The PRDT is one entry per 4 KiB page, and `tools/ahci_test.py` is the only thing that runs any of it.
- **ALL THREE DISKS DISCARD, AND THE CAPABILITY IS THE DEVICE'S ANSWER RATHER THAN ITS FEATURE BIT** -- a virtio device may negotiate DISCARD and advertise a `max_discard_sectors` of ZERO (QEMU does, without `discard=unmap`), so `block_virtio.c` declares `BLK_CAP_TRIM` from the MAXIMUM and `block_ahci.c` from IDENTIFY word 169. **A TRIM that acknowledges and discards nothing is invisible from inside the guest**, so the KTESTs cover REFUSALS only and the real oracle is the HOST: write 40 MiB, delete it, require the sparse image's allocated size back at baseline. **`notrim` turns discards off for EVERY backend**, gated once in `blk_trim_supported()` -- a diagnostic A/B, since a flush after a hole-punch is far slower on some hosts than others.
- **VIRTIO-BLK IS THE PREFERRED DISK; ATA IS THE LEGACY PATH.**
- **A filesystem talks to a `block_device`, not to a disk.**
- **TFS3's last block group may be PARTIAL**
- **A new TFS3 operation must COUNT ITS JOURNAL CREDITS, and the count is the design.**
- **Shrinking a file, or anything else that stops referencing a block, commits the pointer change BEFORE freeing the bit.**
- **THE STOCK `disk.img` IS PARTITIONED AND BOOTABLE, AND ONLY A BLANK IMAGE GETS THAT** -- a GPT holding a BIOS boot partition (GRUB's `core.img`), a FAT32 `/boot` and TFS3, in that order. `seed_disk.py` asks `mkpart_test.py`'s `volume_of()` what shape an image already is and KEEPS it, so an existing checkout is untouched until `make clean-disk`. **There is no flat option any more** -- `--flat` is gone and the kernel refuses a whole-disk volume, so even the live ISO's RAM image carries a table. **A host tool that reaches into the filesystem must ask `volume_of()`** and pass `--at-lba`/`--sectors` to `tfs3_writer.py` -- hardcoding 2048 is the pointer-somebody-must-maintain shape, and "partition 1" is now WRONG as well (`volume_of()` finds the TFS3 volume by looking). `try_partitions()` mounts the FIRST partition a backend claims, minus the firmware's; that stops being unambiguous when FAT32 lands (`root=` is the roadmap item).
- **REMOVING A FILESYSTEM BACKEND SILENTLY REFORMATS EVERY DISK IN THAT FORMAT** -- the probe treats a disk no backend claims as "readable but unclaimed", which is the blank-disk case, which FORMATS. TFS2's removal needed a recognise-and-refuse guard for exactly this; the guard was then removed on the maintainer's word that no such disks exist, so **a TFS2 disk booted today IS reformatted**. Make that call deliberately for the next format: the guard is ~15 lines and the failure is unrecoverable. `struct fs_ops` stays a registry with one row because FAT32 is next.
- **`/etc` on the persistent filesystem is the config-file convention.**
- **A PARTITION IS A BLOCK DEVICE, AND THE FILESYSTEM NEVER LEARNS ITS OFFSET** -- `block_part.c` wraps a parent and shifts every LBA (Linux's `bd_start_sect`, Windows' `partmgr`); the active device stays SINGULAR; `blk_read_sectors()` is the VOLUME and `blk_disk_read_sectors()` is the DISK, so a table parser must use the latter; capabilities are inherited and TRIM is CLAMPED; and a backend declares `fs_ops.volume_relative`, the guard that stops the scan offering a partition to a backend that ignores the block layer (TFS2 declared 0; FAT32 will declare 1).
- **A DRIVE'S ROOT IS A PARTITION, OR IT IS RAMFS -- AND NOTHING IS AUTO-FORMATTED** -- `probe_and_mount()` (`kernel/fs/vfs.c`) is a table of situations: a live module, a claimable partition, a table with nothing claimable, NO TABLE, no disk. The last three all end in **ramfs** (`kernel/fs/ramfs.c`), a real filesystem in the kernel heap. A whole-disk volume is REFUSED by name, told what to run (`make clean-disk && make iso`, or `mkpart`/`fsformat`), and left untouched -- Windows will not boot one either, and no Linux installer has produced one in twenty years. The old blank-disk auto-format is GONE with it: there is nowhere left for it to write, so "an unrecognised disk is not an invitation" is true by construction rather than by a branch remembering it. **`docs/rootfs-design.md` is the full account.**
- **RAMFS IS NOT IN `g_backends`, AND PUTTING IT THERE WOULD DESTROY A DISK** -- that table is the ON-DISK registry, and `fs_format_backend()` wipes every OTHER backend's signatures before formatting with the named one. `fsformat ramfs confirm` would therefore erase TFS3's superblock from a working disk and then fail its own persistence check. It is reached through its own pointer. **A registry is not a neutral place to put something** -- it is a list of things every consumer of that registry will act on.
- **`init()` IS THREE-VALUED: 1 persistent, 0 mounted-but-not, -1 COULD NOT MOUNT** -- the same shape `probe()` uses. Before -1 existed, TFS3 (which has no RAM-only mode) returned 0 for failure and `vfs.c` read it as "mounted, not persistent", announcing `tfs3 (RAM-only)` over a machine where every `fs_*` call failed. A backend that cannot mount now leaves NO active backend and says so.
- **RAMFS HAS A BUDGET, HALF OF FREE MEMORY AT MOUNT** -- tmpfs's own default, and not optional: this kernel has no OOM killer, and ramfs draws from the same frames as the allocator everything else depends on. Over it, an allocating write fails exactly as a full disk does. File data is CHUNKED (4 KiB) because `heap_os_alloc()` asks `pmm_alloc_contiguous()`, so one buffer per file would fail on a fragmented machine while `meminfo` still showed memory free.
- **A PARTITIONED DISK IS NEVER AUTO-FORMATTED, AND THE FIRST PARTITION THAT IS OURS IS LEFT ACTIVE** -- a flat format SURVIVES a GPT (TFS3 reserves volume blocks 0-7), so claiming an unclaimed partitioned disk would lay a whole-disk volume across every partition's data while `parttable` kept printing the table correctly. `fsformat` formats the ACTIVE device, which is what makes `mkpart` -> reboot -> `fsformat` land inside a partition. **"Ours" excludes the FIRMWARE's** -- `vfs.c`'s scan skips a GPT BIOS-boot or ESP partition (`partition_is_firmware()`), so no backend probes one and the fallback cannot aim `fsformat` at the 1 MiB partition holding GRUB; `partition_test.py`'s last phase asserts the partition NUMBER left active on a table with no filesystem in it, the only state where that choice is visible.
- **WRITING A TABLE IS A SYSCALL THAT TAKES A TABLE, NOT A SECTOR** -- `SYS_MKPART` takes a `struct mkpart_request` and the kernel encodes it, because this kernel has no privilege model to gate a write-any-sector primitive with (Linux's `BLKPG` shape). `MKPART_CONFIRM` is a SPEED BUMP, not a permission check. GPT is written backup-first and protective-MBR-last; nothing is remounted.
- **A ROOT IS ONE FILESYSTEM, BUT A PATH TREE IS SEVERAL: THERE IS A MOUNT TABLE** -- `kernel/fs/mount.c` holds it and the boot policy, `kernel/fs/vfs.c` resolves a path to a mount and forwards. FIVE RULES (`kernel/mount.h`): longest prefix wins **at a component boundary** (`/boot` never claims `/bootloader`, which is why `under()` is not a `k_strncmp`); the backend is handed a **root-relative** path and never learns where it is mounted; mounting over a non-empty directory **allows and hides** it (Unix) and the point must already exist as a directory (Linux); an op naming TWO paths (`rename`, `link`) across mounts is **refused**, Unix's EXDEV; and `..` cannot escape a mount root **for free**, because every path is normalized before it arrives. `mount`/`umount` are `/bin` programs and the source is a PARTITION NUMBER -- this OS has no `/dev`.
- **A BACKEND DECLARES HOW MANY TIMES IT MAY BE MOUNTED, and every one says ONE** -- `fs_ops.max_mounts`. Not decoration: without it `mount 3 /mnt` on a second TFS3 partition succeeds, repoints one set of module-level statics, and the ROOT starts reading the other volume with no error anywhere. Raising it needs per-instance state AND an opaque handle threaded through every op (Linux's `super_block`), and buys nothing until something wants two volumes of one format.
- **A PROBE MUST NOT DISTURB A MOUNT, and that contract was only ever honoured by accident** -- `fs_ops.probe()` has always said "no side effects beyond the read", and both backends record the device they are handed. Mounting `/boot` probes every backend against the ESP, so a TFS3 serving `/` was repointed at partition 2: `df` kept reporting the right numbers off the cached superblock while every path lookup failed, and **the root went silently empty**. Both backends save and restore their volume around a probe; `mount.c` also declines to probe a backend already at its limit. The general shape: when you make something happen at a SECOND time, re-read what it promised.
- **`fs_ops.init()` TAKES A DEVICE, and `blk_active()` is not it** -- with two mounts there is no single active device a backend could correctly assume, and one that assumed anyway reads the WRONG VOLUME and reports no error. Linux's `super_block->s_bdev`. A backend reads through `blkdev_*` (same fault-injection hooks); `blk_part_create()` makes a partition device WITHOUT making it active, and the same window asked for twice returns the same device, so pointer identity answers "is this volume already mounted?".
- **FAT32 IS A GENERIC DRIVER AND KNOWS NOTHING ABOUT BOOTLOADERS** -- `kernel/fs/fat32.c` mentions none; the read-only-by-default policy is in `mount_boot_auto()`, which is Linux's split (`fs/fat/` is generic, the ESP is an ordinary mount). NOT FAT12/16 (a different root layout and FAT width -- a non-FAT32 volume is refused by name), NOT 4096-byte sectors, NOT Unicode (non-ASCII in a long name becomes `?` on read and is REFUSED on create), and NOT journalled because FAT is not. **The LFN set is stored in REVERSE** -- highest index first, carrying `0x40` -- and writing it forwards produces a name every other driver reads backwards; that shipped for one build, and a short name cannot show it. Growing flushes the FAT chain BEFORE the size; shrinking reverses it. It accepts a cluster count below FAT32's 65525 floor on purpose (that threshold is for a driver telling FAT12/16/32 apart; this one discriminates on the BPB's FAT32-only fields), which is what lets a 512 KiB KTEST volume exist.
- **`/boot` IS READABLE FROM INSIDE toy-os NOW, AND IT IS THE ESP** -- mounted read-only at boot. What is inside it is the ESP's OWN layout: `install_grub.py` writes `boot/kernel.bin` and `boot/grub/` so one `grub.cfg` serves the ISO and the disk with identical paths, so the running kernel is at **`/boot/boot/kernel.bin`**. That nesting is the volume as it really is, the same way a Linux ESP at `/boot/efi` shows `/boot/efi/EFI/...`.
- **TOY-OS BOOTS FROM ITS OWN DISK, AND `/boot` IS FAT32 BECAUSE GRUB CANNOT READ TFS3** -- `tools/install_grub.py` writes `boot.img`, `core.img` and `/boot/kernel.bin` onto `disk.img` at every `make iso`; an ordinary run is `-boot order=c` with no `-cdrom` at all. The ISO stays a boot medium (live, demo, a release), so this is a CHOICE, not a replacement.
- **NEVER LEAVE THE BOOT ORDER OUT OF A QEMU LINE, AND ASK `boot_medium()` WHICH ONE** -- `0x55AA` at LBA 0 is all SeaBIOS checks, so with no order it boots ANY partitioned disk and, if nothing installed GRUB on that one, jumps into the table and hangs with **no serial output at all** -- indistinguishable from a kernel that died before its first print. The medium is DERIVED from the image (`install_grub.boot_medium()`, asked by `QEMU_RUN`, `vm.py`, `launch_qemu_cmd()`, `boot_smoke_test.py`, `serial_console.py`), so an image predating this layout still boots the ISO; `BOOT=disk|cd` and `--boot` override. A tool that builds its OWN image (`partition_test.py`, `virtio_boot_test.py`, `run_release.sh`) hardcodes `order=d`, because nothing put a bootloader on it.

### The shell, the console, and line editing

`docs/conventions/shell.md`

- **EVERY COMMAND HAS A PAGE IN `docs/commands/`, AND THE BUILD CHECKS IT.**
- **AN EVERYDAY COMMAND IS A `/bin` PROGRAM, NOT A BUILTIN, AND THE KERNEL'S OWN COPIES LIVE BEHIND ONE NAME: `rescue`.** -- and `rescue` is the commands you would need to put `/bin` BACK, not everything ring 0 happens to be able to do: `strace` moved out to `/bin` and did not go there.
- **A PROGRAM STARTED BY A BARE NAME PRINTS NOTHING EXTRA WHEN IT SUCCEEDS -- AND `run <name>` STILL DOES.**
- **TAB COMPLETION IS ONE ENGINE COMPILED TWICE, AND A RING SUPPLIES A `struct completion_env`** -- `kernel/lib/completion.c` into the kernel and into `libuapp.a`, like `klineedit.c`; it is FREESTANDING and touches no filesystem (ring 0 has `fs_list()`, ring 3 has `sys_listdir()`), so a kernel include here silently takes completion away from ring 3. `apps/shell_complete.c` and `userland/lib/ucomplete.c` are the two envs and are deliberately NOT parity. An argument completer returns a `completion_domain`, which is what makes `cd`'s directories-only a domain rather than a filter in every caller. `/tests/complete_test` exists because a KTEST cannot see the ring-3 LINK.
- **TAB COMPLETION IN COMMAND POSITION IS BUILTINS PLUS ALL OF `PATH`, DEDUPLICATED AND SORTED, WITH NO DIRECTORIES.**
- **`/bin/tosh -c <command>` RUNS ONE LINE AND EXITS** -- the non-interactive shell `system()` needed; it returns before any interactive setup and must NOT touch the terminal, since a `system()` caller may have inherited somebody else's.
- **`#` IS RING 0 AND `$` IS RING 3, AND THE PROMPT IS WHERE THAT LIVES** -- all three shells show the cwd, so the last character is the difference; `Ctrl-C` works only at a `$`.
- **A BUILTIN MUST NOT SHADOW A `/bin` PROGRAM THAT DOES MORE** -- `/bin/tosh` has six (`cd`, `pwd`, `help`, `jobs`, `fg`, `bg`) and each has to be one; `cat`, `ls` and `echo` were the same mistake three times. The test is "could a program do this better", and a builtin passes it by touching the SHELL's own state -- writing it (`cd`) or reading it (`fg`).
- **A WRAPPER BUILTIN IS ONE COMMAND WITH TWO HALVES IN TWO RINGS, AND THE RING-3 HALF WILL BE WRONG.**
- **A COMMAND WITH A READ HALF AND A WRITE HALF MOVES AS ONE PIECE OR NOT AT ALL.**
- **COLOUR IS AN ESCAPE SEQUENCE, NOT A SYSCALL.**
- **`edit` IS A `/bin` PROGRAM, AND THE KERNEL DRAWS NOTHING** -- it renders with ANSI on fd 1 over a raw fd 0, its model is `utext` (shared with Notepad), and moving it emptied `apps/ui/`.
- **`cp` EXISTS NOW, AND COPYING IS A PROGRAM RATHER THAN A SYSCALL** -- `/bin/cp [-r]`, spawned by the GUI file manager rather than reimplemented in it; `rm` grew `-r` in the same change; both walk breadth-first over an explicit QUEUE because one listing is 20 KB against a 2 KiB frame budget, and `rm -r` removes the collected directories in REVERSE (deepest-first, a post-order walk with no recursion).
- **Ctrl-L CLEARS IN BOTH SHELLS NOW, AND THE COMMENT THAT STOPPED IT WAS TRUE WHEN IT WAS WRITTEN.** -- `/bin/tosh` printed a newline because "there is no terminal under this yet", which the TTY layer made false and nothing noticed. The general trap: a comment stating a FACT about the rest of the system outlives that fact silently.
- **A TITLE-BAR BUTTON IS A DISC, AND EVERY GLYPH CENTRES ON THE SAME PIXEL AS IT.** -- close is grey until hovered; a filled circle needs an AA outline over it or its cardinal spurs read as a COG; **a disc centres on a PIXEL and a rectangle on a SPAN**, so draw glyphs from `cx-h` to `cx+h` inclusive (odd, always centred) rather than at `(size-w)/2`.
- **AN ICON COLUMN IN A SIDEBAR IS PER SIDEBAR, NOT PER ROW** -- indenting only the rows that have an icon puts headings further right than their own children. The gutter must also count towards `natural_size`, or the longest label clips.
- **A MOVE EVENT REACHES EVERY WIDGET AT EVERY DEPTH NOW, AND HOVER BELOW TWO CONTAINERS WAS DEAD UNTIL IT DID.** -- `uui_router_motion()` walked ONE level while press and wheel recursed; System Settings nests four deep, so no hover in it could light up, including a dropdown popup's rows. A clipped subtree the cursor has LEFT is told a point no widget can contain (so a stale highlight clears), and an open overlay gets the real point first.
- **AN OPEN POPUP TAKES THE KEY, AND A KEY-DRIVEN CHANGE IS REPORTED LIKE A CLICK.** -- `uui_router_overlay_key()` is `overlay_active`'s keyboard half, which is what makes typing into a dropdown work in an app with no focus ring; `uui_focus_key()` reports `UUI_REASON_KEY` by the router's id, since a focused control's keyboard change used to be silently dropped by Apply.
- **TYPING IN A LIST SEEKS, AND ONE SEARCH SERVES BOTH WIDGETS.** -- `userland/ui/uui_seek.c`, called by `uui_listbox_key()` and `uui_table_key()`; keys within a second build a prefix, the same letter again CYCLES and never expires; matched against the DISPLAY name as a prefix. A closed dropdown takes letters (unlike the wheel, which it ignores).
- **A TABLE DECLARES WHICH COLUMN A LETTER MATCHES: `uui_table_set_seek_col()`** -- GtkTreeView's `search-column`, not Win32's always-column-0, because column 0 is the name in a file listing and the PID in Task Manager. Defaults to 0 rather than off (a wrong column announces itself; ignoring letters does not). The search walks VIEW positions and converts back, or cycling moves the selection somewhere the user is not looking; and `uui_table_set_rows()` must NOT reset the prefix, since Task Manager calls it every refresh.
- **A WIDGET THAT TAKES KEYS STILL GETS NONE UNTIL THE APP ROUTES THEM** -- `uapp.c` offers a key to `uapp_desc.focus` and then to `uapp_desc.on_key`, so an app declaring NEITHER reaches no widget's `key` op and nothing says so; Task Manager's table had arrows, paging and type-ahead dead for months behind a suite that drives every check by MOUSE. A focus ring is the toolkit's idiom but adds a Tab stop no widget draws yet; forwarding from `on_key` is what the File Manager and Task Manager do. When a widget gains a key handler, grep for the app's routing.
- **A FOCUS INDICATOR IS `uui_focus_ring()`, IN THE THEME'S ACCENT, AND THE WIDGET PASSES THE RECT** -- one helper in `uui_primitives.c`, drawn by every widget that accepts focus; the ACCENT rather than a wash of the control's own colour, because focus and hover are different questions and `utheme.h` has named the role since it was written. A list rings the focused ROW and falls back to the BOX when the selection is scrolled off (an indicator that vanishes is the bug), a slider rings its thumb. **A SELECTION IS NOT AN INDICATOR** -- `uui_listbox` assumed it was, in a comment, and two lists side by side then say nothing about which one is listening.
- **A SETTING WHOSE CHOICES ARE DATA NAMES THEM ITSELF: `choice_label`, tried after `/etc/settings.d` and before the raw value.** -- `Choice.<value>=` lines cannot cover a COMPUTED list (`/etc/timezones`, the keyboard layouts) without regenerating the file whenever the data changes. `/etc/timezones` grew a fourth field for it; a three-field row still loads and falls back by name, which is what makes an existing disk show "Los Angeles" without being rewritten. The VALUE stays the identity.
- **THE ICON CACHE IS THE TOOLKIT'S NOW (`userland/lib/icon_cache.h`), AND A SIDEBAR HEADING CAN CARRY AN ICON.** -- moved out of `userland/wm/` when `uui_sidebar` needed it; an icon is a NAME, headings only, size font-derived. A missing file means a plain row, never an error.
- **A WINDOW HAS TWO BUFFERS, AND THE COMPOSITOR NEVER READS THE ONE BEING DRAWN.** -- `WIN_REQ_PRESENT` flips and returns the new front; both buffers stay mapped in both address spaces at `base` and `base + WIN_BUFFER_HALF`, so a flip is a number rather than a remap. A failed second allocation is a SINGLE-BUFFERED window, not a refused one. Tested as memory (a marker invisible until presented), never as a flicker.
- **THE LAYOUT LOG IS OFF UNLESS A TEST TURNS IT ON, AND DEDUPED WHEN IT IS.** -- `desktop.layout_log`, off by default like `kernel.kbdtap`. Nine apps wrote ~20 geometry lines a FRAME to the kernel log, which made `dmesg` unreadable with any window open and made `dmesg -w` a feedback loop. Read once at first use (`enter_gui()` turns it on for every tool); deduped per FRAME, not per line, because what repeats is the whole block.
- **THE TERMINAL SCROLLS BY WHEEL AS WELL AS BY KEY, AND BOTH MOVE THE SAME STATE.** -- `on_wheel` was a toolkit slot the app never implemented; three lines a notch, clamped both ends.
- **THERE IS AN ALTERNATE SCREEN, AND IT IS WHY A PAGER LEAVES NO WRECKAGE.** -- `ESC[?1049h`/`l`; the GUI Terminal saves its grid and cursor and restores them, so `less` quits to exactly the prompt it started from. 1049 only; a consumer may ignore it and the console does; scrollback is deliberately not saved.
- **`dmesg` IS A `/bin` PROGRAM, AND THE LOG LEAVES THE KERNEL THROUGH `QUERY_KLOG`.** -- byte slices, not lines, each carrying its ABSOLUTE offset since boot so a reader can see the ring moved under it. Pagination is gone (`dmesg | less`); the ring-0 copy is `rescue dmesg`.
- **A JOB IS A PROCESS GROUP, AND THE JOB TABLE IS THE SHELL'S** -- `Ctrl-Z`, `jobs` and `fg`; the kernel knows about groups and nothing about jobs. `[1]+ Done` is printed at a PROMPT, and a `SIGCHLD` handler with NO `SA_RESTART` is what produces one when nobody is typing.
- **A TERMINAL IS AN OBJECT, AND THE CONSOLE IS `tty0`** -- `kernel/tty/` holds the line discipline, and `Ctrl-C` on the physical keyboard and in a window are one implementation. INTR left the keyboard driver; `SCHED_CHAN_KEY` is gone.
- **A `text` BOOT REACHES A RING-3 SHELL, AND THE KERNEL SHELL STANDS DOWN FOR IT.**
- **RING 3 CAN READ THE CONSOLE -- fd 0, and it BLOCKS.**
- **A QMP TEST THAT TYPES PUNCTUATION MUST PIN THE GUEST'S KEYBOARD LAYOUT.**
- **RING 0's BLOCKING KEYBOARD READERS ARE SUSPENDED WHILE A COMPOSITOR HOLDS THE ROLE**

### The build, the userland layout, and releases

`docs/conventions/build.md`

- **mtools DOES NOT READ stdin -- IT OPENS `/dev/tty`, so a CAPTURED PROMPT HANGS FOREVER** -- any `mcopy`/`mmd`/`mformat` under `capture_output=True` needs `start_new_session=True` (NOT just `stdin=DEVNULL`) plus a timeout, or `make iso` stops dead with no output at all and `make clean-disk` looks like the cure. Measured: a hung `mmd` had fd 4 on `/dev/tty`.
- **THE C LIBRARY IS CALLED `tolibc`, and its bar for adding a function is the OPPOSITE of everything else here -- it aims to be COMPLETE.**
- **REGEX IS `<regex.h>` IN tolibc, AND IT IS AN NFA** -- `regcomp`/`regexec`, a Thompson NFA with no input that makes it slow (`(a*)*b` is linear here and exponential in a backtracker). NO BACK-REFERENCES, which is what an NFA cannot do and what `regcomp()` refuses by name. Program jumps are RELATIVE, which is what makes `{n,m}` a memcpy rather than a jump-target rewrite. **`/bin/grep` speaks ERE and has no `-E`** -- POSIX's BRE is compatibility baggage, and `regcomp()` implements BRE anyway. **`tosh` splits on `|` before anything else and has NO QUOTING**, so a pattern containing one cannot be typed.
- **WHEN IMPLEMENTING A SPEC, DISAGREE WITH AN INDEPENDENT IMPLEMENTATION ON PURPOSE** -- `tools/regex_hostcheck.py` runs the regex case table against GLIBC, `tools/uimg_hostcheck.py` runs the JPEG decoder against libjpeg, and `tools/fat32_test.py` reads what the guest wrote back with **mtools** and audits the volume with **fsck.fat**. A self-test cannot catch an EXPECTATION being wrong, because the same person wrote both halves. Every difference is then a bug or a documented decision, with no third category. The storage one shows what a strong check looks like: not "the file reads back" (which a privately-wrong format passes, reading its own bytes) but a 185 KiB BINARY extracted on the host and compared byte for byte.
- **`tolibc` GREW A SECOND PORT'S WORTH OF FUNCTIONS, AND ONE OF THEM WAS A BUG** -- Doom needed `remove()`/`rename()` (both previously listed as deliberately absent), `mkdir()` with a new `<sys/stat.h>` that deliberately has NO `stat()`, `access()` (only `F_OK` can mean anything), and `system()` (which needed `tosh -c`). The bug: `kfmt.c` ignored `printf` precision on integers, so `"%.3d"` of 33 gave `33` and Doom asked its WAD for a lump that does not exist -- in a file compiled into both rings, whose tests asserted the old behaviour.
- **`SYS_WRITE_MAX` IS A THROUGHPUT CONSTANT, NOT JUST A BUFFER SIZE, AND IT IS 64 KiB** -- every `fs_write*()` is one TFS3 transaction and `txn_commit()` ends with TWO barriers, so the cap sets how many device flushes a megabyte of ring-3 writing costs (2048/MiB at 1 KiB, against 2/MiB for ring-0 `stress`). Raising it measured 3.4 -> 114.3 MB/s sequential write. The REMAINING gap is architectural -- Linux does not flush on write at all. **Raising it nearly deadlocked pipes**: `pipe_write()` parks a writer that does not fit, safe only while 1024 < `PIPE_BUF_SIZE`, so `sys_do_write_pipe()` clamps explicitly now.
- **`sys_write()` COMPLETES THE WHOLE BUFFER, because the kernel caps one write at `SYS_WRITE_MAX` (1024) and a short write loses data SILENTLY.** -- libsys returned the short count and dropped the rest, so every caller ignoring the count truncated at 1 KB. `less` looked like a pager bug for two rounds because of it. Asking for 2000 bytes means 2000 bytes.
- **AN UNRECOGNISED printf CONVERSION DESYNCHRONISES EVERY ARGUMENT AFTER IT, AND `kfmt_cases.h` IS THE TABLE THAT STOPS A FOURTH ONE.** -- `kfmt.c` is tolibc's `printf`; an unknown conversion prints its letters and consumes NOTHING, so a missing feature corrupts output far away from itself. Three have shipped (`%.3d`, `%X`, then `%p`/`%o`/`%+d`/`%hd` by audit). Every conversion and flag C defines has a case, run from BOTH rings.
- **THE POSIX HALF OF `tolibc` IS HEADERS OVER SYSCALLS THAT ALREADY EXIST** -- `<signal.h>`, `<sys/wait.h>`, `<termios.h>`, `<fcntl.h>`, `<strings.h>`, `getopt()`; the kernel-facing action struct is `struct k_sigaction` and POSIX's is converted at the call (glibc's split), and anything that cannot be honoured is REFUSED rather than ignored (a non-empty `sa_mask` is `EINVAL`; `VMIN`/`VTIME` are undefined on purpose).
- **`userland/` is split by ROLE, and the build derives things from it -- adding a program is a `.c` file and nothing else.**
- **In ring 3 the toolkit is reachable under the C names -- don't hand-roll a `my_strlen` or a digit loop there either.**
- **EVERY RING-3 PROGRAM CARRIES A TLS BLOCK, AND `crt0` INSTALLS IT BEFORE `main()`** -- `.tdata`/`.tbss` from `userland/rt/link.ld`, laid out by `userland/rt/tls.c` with `%fs` pointing at the block's END (the psABI's variant II, so a `__thread` variable is at a NEGATIVE offset). `-ftls-model=local-exec`, because every other model wants a dynamic linker. Two traps: the block must be rounded to the SEGMENT's alignment, not a convenient one; and **a linker symbol's address is data GCC does not believe is data** -- a loop bounded by one is compiled bottom-tested, so a size of 0 counts to 2^64 (a page fault in every ring-3 program until `linker_value()` laundered it).
- **RING-3 CODE HAS A FRAME BUDGET, and a link-time bound on the image.**
- **Every ring-3 program is just a `main()`.**
- **`linker.ld` decides kernel memory PERMISSIONS, not just placement.**
- **CI RUNS THE KERNEL SUITE TWICE, on ATA and on virtio-blk, and the second one earns its place.**
- **A graphics card is a `display_driver`, not a special case.**
- **`kernel/` directories are subsystems, not filing cabinets**
- **`kernel/include/api/version.h` is GENERATED, not hand-edited**
- **Versioning is semver + a `-dev` suffix, not a per-change build number.**
- **A GitHub Release's notes follow ONE shape, and it is terse.**

## This checkout, and the repo it pushes to

**The repo is `eveningworks/toy-os`** -- an ORGANIZATION, since
2026-08-16. It moved off a personal account because a personal repo has
no read-only collaborator role (every collaborator gets write), and
branch protection is unavailable on a free private repo either way; see
`docs/decisions.md`. The maintainer's account was renamed in the same
stretch, and the old handle was scrubbed from committer metadata and the
current tree by a history rewrite. Practical consequences for a session:
the remote is `git@github.com:eveningworks/toy-os.git`, nothing in the
tree should name a personal account (`grep -rn` for one before
believing a doc), and **run `tools/backup_repo.sh` before any further
change to the repo's identity or history**.

Sessions work directly against this checkout with ordinary file and
Bash tools:

- Git identity is already configured (`toy-os` /
  `noreply@toy-os.local`), so plain `git commit` just works. **It is
  PER-REPOSITORY and a clone does not carry it** -- a fresh checkout on
  another machine falls back to the global identity, which is the real
  name a history rewrite once removed from every commit here.
  `preflight.sh` refuses to run until SOME local identity is set --
  not this exact one, since demanding that would refuse a fork's own
  contributors; it notes the difference and proceeds. The guard is in
  the gate because `.git/hooks` is not cloned either. README's "Setting
  up another machine (or a fork)" has the full command sequence. **That
  identity is the standing privacy convention, not a default to
  override** -- never let a commit here carry the maintainer's real
  name or personal email (see `docs/decisions.md`'s entry on the
  history rewrite that scrubbed one out of every prior commit; don't
  reintroduce what that fixed).
- `git push origin main` and `gh release create`/`gh release upload`
  all work from the session -- confirmed by doing both, including
  cutting the `v0.1.0` Release end to end. Push ordinary verified work
  without asking; confirm tags, Releases, force-pushes and history
  rewrites first, since those are one-way.

(This repo used to also be worked on from a Cowork cloud session
through a device bridge, which needed a `.new`-file relay for protected
files, a `git` wrapper that swept stale lock files, and `mv`-instead-of-
`rm` because the bridge could not delete. That mode is retired as of
2026-08-19 and all of it is gone -- `git log` has it if it is ever
needed again.)

## Building

**`docs/testing.md` is the full testing reference** -- how to run this
OS, drive it headlessly, and prove a change works -- and `docs/tools.md`
covers every script in `tools/`. This section holds only the targets and
the traps that have to fire before you know to look anything up.

```
make all    # kernel.bin + userland test ELFs
make iso    # + toy-os.iso (grub-mkrescue); ALSO seeds disk.img and
            # installs GRUB + the kernel on it -- still the target to
            # run before any headless test, whichever medium boots
make test   # boot headless, run the in-kernel test suite
make verify # the full pre-delivery gate: clean build + iso + boot test + ktest
            # (same as tools/preflight.sh, which also summarises `git status`)
make run    # boots in QEMU with an SDL window (the user's machine, not headless)
make live-iso   # toy-os-live.iso -- carries a TFS3 image as a GRUB module
make demo-iso   # toy-os-demo.iso -- boots straight into a scripted tour
make debug  # boots frozen (-s -S) for real GDB debugging -- see below
```

**THERE IS ONE RUN TARGET, AND EVERY WAY TO BOOT IS A VARIABLE ON IT --
so do not add a target for a new combination.**

```
make run KVM=1 VIRTIO=1 AUDIO=1 NOGRAPHIC=1 MENU=1 MEM=512
make run WINDOW=full   # or WINDOW=fit -- see below; sdl cannot scale
make run DISK=virtio VGA=virtio INPUT=virtio   # what VIRTIO=1 is short for
make run LIVE=1    # the live ISO, no disk attached (implies live-iso)
make run DEMO=1    # the scripted tour, no disk (implies demo-iso)
make run BOOT=cd   # boot the ISO; BOOT=disk forces the other way
```

**`WINDOW` IS THE AXIS FOR A GUEST MODE THAT DOES NOT FIT THE MONITOR.**
`-display sdl` (the default) has no scaling and no window placement --
its only suboptions are gl/grab-mod/show-cursor/window-close -- so the
window is the framebuffer plus decorations and a `video=1920x1080` guest
cannot fit a 1920x1080 screen. `WINDOW=full` adds `-full-screen` (no
decorations, still pixel-exact); `WINDOW=fit` switches to
`gtk,zoom-to-fit=on`, the only one that helps when the guest mode is
BIGGER than the monitor and the only one that blurs the font. An unknown
value is refused rather than falling back, because `fit`/`full` are this
Makefile's names and a typo cannot be caught downstream.

**`BOOT` IS DERIVED BY DEFAULT, AND THAT IS THE POINT.** `make run` boots
the DISK, because `disk.img` carries GRUB and the kernel now
(`tools/install_grub.py`) -- but only if that image actually has them, so
a checkout whose `disk.img` predates the layout keeps booting the ISO
with nothing to configure. `make -n run` prints the line it chose.

**`VIRTIO=1` MEANS EVERY DEVICE CLASS -- disk, GPU and input -- not the
disk.** It used to mean the disk alone, which is a name broader than its
effect, so the `make run KVM=1 VIRTIO=1` everyone reached for still
booted `-vga std` and never exercised the virtio-gpu driver. Each class
now picks its implementation BY NAME (`DISK=ide|virtio`,
`VGA=std|virtio|vmware`, `INPUT=ps2|virtio`) and a per-class value
overrides `VIRTIO=1`, so `VIRTIO=1 VGA=std` is legal. A name rather than
a boolean because a boolean cannot express a third one and NVMe is a
roadmap item. **An old `VIRTIO=1` invocation now switches more than it
used to** rather than failing -- deliberate, and the reason the help text
says so.

It was twelve near-identical `qemu-system-x86_64` lines, which grew by
MULTIPLICATION rather than addition -- the same shape CLAUDE.md already
legislates for C, which `tools/check_dispatch.py` enforces on `.c` files
and cannot see in a Makefile. The aliases (`run-kvm`, `run-virtio`) are
gone too, because a name per combination multiplies exactly as fast as a
recipe per combination; `debug` survives only because it is not a
combination of the axes.

Three traps if you touch it. **`LIVE` and `DEMO` change the PREREQUISITE
as well as the command line**, which works only because a command-line
variable is set before the Makefile is parsed, so `$(if)` expands
correctly even in a prerequisite list -- a target-specific variable would
not. **Every definition is DEFERRED (`=`, never `:=`) and uses `$(if
...)` rather than `ifeq`** -- `ifeq` is evaluated once when the Makefile
is read, so a flag arriving later is invisible to it and silently does
not appear. And **`MENU=1` works by deriving `GRUB_TIMEOUT`**, because
the timeout is baked into `grub.cfg` at build time rather than passed to
QEMU -- into BOTH media, since one repo-root `grub.cfg` is generated
into the ISO tree and into `disk.img`'s FAT32 `/boot/grub`. **Check a change here with `make -n run <FLAGS>`**, which prints
the command line without running it.

**Boot flags can be baked into the media** rather than typed into the
GRUB menu each boot: `make iso KCMDLINE="video=1920x1080 nokaslr"` (also
`live-iso`/`demo-iso`) -- into the ISO and into `disk.img`'s
`/boot/grub/grub.cfg` alike, from one source file. Empty by default, so every automated path is
unaffected. `docs/boot-flags.md` lists every word. **GRUB's `e` editor
shows the menuentry BODY only**, so the boot-word summary is repeated
inside each `menuentry` in the three `grub*.cfg` files -- keep the inline
copy to a few lines, since the edit screen is ~20 lines. And
**`video=<W>x<H>` only does something on a MODESETTING driver**: `vmsvga`
programs the CRTC and honours it, while on a plain VESA framebuffer GRUB
has already fixed the mode and the flag is inert.

**Source discovery is recursive** -- every `.c` under `kernel/` or
`apps/` is compiled and every `.asm` under `kernel/` assembled, with
`build/` mirroring the source tree, so a new directory needs no Makefile
edit. The flip side: a `.c` file anywhere under `kernel/` or `apps/` IS
in the kernel image -- there's no scratch file the build ignores, so
throwaway code goes somewhere else.

**A plain `make all` is safe after editing a shared header** -- the
Makefile tracks header dependencies (`-MMD`/`-MP`) and
`tools/check_deps.py` proves per build directory that the tracking is
live, so if that check is green believe it and look elsewhere. **What is
NOT tracked is a CFLAGS change**: the `.d` files record headers, so
editing `CFLAGS`/`USERLAND_CFLAGS` invalidates nothing and the next build
links objects compiled under the old flags. **`make clean` after a flags
change**, and treat a compiler flag that appears to work only partially
as a stale-object symptom first (that is exactly how
`-ffunction-sections` presented).

**`make all` REACHES NO BOOT MEDIUM.** It writes `build/kernel.bin` and
stops: the kernel gets onto `disk.img` (and into `toy-os.iso`) in the
`seed` step that `make iso` runs. So a `make all` without `make iso`
leaves the whole suite testing the PREVIOUS build -- and it does not
fail loudly, it fails as a clean pass. **`make iso`, not `make all`,
before any headless run**, even though the ordinary run no longer boots
the ISO -- that target is what installs the kernel on the disk too.
`tools/iso_guard.py` refuses a stale one now (comparing against
`build/.bootdisk` on a disk boot and `toy-os.iso` on a CD boot), but the
reasoning is worth keeping: the data never reached the code under test
because the code never reached the machine.

**Every automated test here runs TCG, and a green suite therefore says
nothing about two whole bug classes** -- anything depending on how FAST
the emulated hardware is, and anything depending on guest MEMORY TYPES
(TCG ignores PAT, so a write-combined framebuffer behaves like cached
RAM). Both have shipped real bugs here. When a report reproduces only on
hardware, try `python3 tools/vm.py --kvm` BEFORE concluding it is
untestable, and ask what the emulator models differently before doubting
the report. `tools/kvm_soak.py` is the standing check.

**Test against a COPY of `disk.img` if the user might have their own QEMU
open** -- QEMU takes a write lock, and `make iso` re-seeds `disk.img`
underneath a VM already booted from it. `cp --reflink=auto
--sparse=always disk.img /tmp/test.img` then `vm.py --disk /tmp/test.img`
avoids both. **A COPY goes stale the moment you rebuild**, so re-copy
after every `make iso`, not once at the start of a session: a copy is a
BOOT MEDIUM now (the kernel is installed onto the image), so a stale one
runs an entire earlier build -- kernel and userland together, nothing
mismatched to notice -- which reads exactly like a bug in the app.
`iso_guard` warns and says which case you are in.

**A copy is not enough for `make iso`/`make verify`/`preflight.sh` --
ASK the user to close their QEMU first (standing request).** Those three
re-seed the real `disk.img` regardless of what any test is pointed at,
and nothing fails loudly at the time. So check (`ps aux | grep
qemu-system`) BEFORE the verify gate rather than after. What does NOT
need them to close anything: editing, `make all`, and `vm.py --disk
<copy>`.

**BEFORE BELIEVING ANY GUI TEST FAILURE, RE-RUN IT ON A FRESH IMAGE.**
`make iso` re-seeds `disk.img` by SYNC, never reformat, so anything an
earlier run wrote is still there -- and several tools' apps WRITE
(`menubar_test` saves a file, `settings_test` and `cursor_theme_test`
persist to `/etc`). A dirty fixture fails in a way that reads exactly
like a code regression. So: `make clean-disk && make iso`, then re-run
with `--logs DIR`. If it still fails, prove it is not yours by rebuilding
`HEAD` (`git stash push -u -m <tag>`, apply by SHA, never a bare pop).
Note this is NOT a universal explanation -- a failure surviving
`clean-disk` may be a genuine pre-existing flake.

**AND A TEST THAT APPLIES A SETTING CHANGES THE MACHINE FOR EVERY LATER
TOOL, not just for itself.** `settings_test` leaves `mouse_speed` and
`mouse_accel` on disk; a faster accelerated pointer then made
`warp_cursor` overshoot in `dialog_test` and `menubar_test`, so two
unrelated tools failed as "hover does nothing" and looked exactly like a
regression in the change under test. The write does not have to be in
the failing tool, or in the same run.

**`strace <binary>` is often the fastest way to see what a `/bin` binary
is doing** -- one decoded line per syscall, and the same lines land in
`dmesg`, so `python3 tools/vm.py exec "spawn /bin/strace file_test"`
followed by `dmesg` returns text you can assert on. Reach for it before
adding temporary `klog_write()` calls inside a syscall handler. **It is
a `/bin` PROGRAM, not a builtin**, so at a `#` prompt it needs `spawn`
(it spawns and waits, which the legacy `run` loader cannot do); at a `$`
prompt it is an ordinary command, and the trace comes out in THAT
terminal.

**READ THE ABI COMMENT OF ANY CALL YOU SWAP IN.** The most expensive
mistake of the ring-3 GUI migration was replacing a non-blocking
`scheduler_poll()` with `sys_waitpid()` while `SYS_WAITPID`'s own first
line said **BLOCKS**. A port is exactly where this happens, because the
new call's name resembles the old one's.

**AND A MECHANISM THAT EXPLAINS THE SYMPTOMS IS NOT THE MECHANISM THAT
CAUSED THEM.** One wrong diagnosis of that bug was written up and
committed: it fitted every observation, and the hazard it blamed was
real -- just not this bug's. Before publishing a root cause, do the cheap
disproving check, and prefer a discriminating experiment (`/bin/hello`
exits at once and was harmless; `winclient` waits and was fatal -- that
pair located the bug class in one run) over a plausible story.

## Debugging with GDB

`make debug` boots toy-os frozen at CPU reset (`-s -S`) instead of
running immediately, for real breakpoint/single-step/register/memory
debugging via QEMU's own built-in GDB remote stub -- **no kernel-side
GDB protocol code needed at all**: QEMU emulates the CPU directly, so
it can already do all of this regardless of what the guest OS does.
See `docs/decisions.md` for why an in-kernel serial-based GDB stub
(the seemingly obvious approach) is unnecessary and was deliberately
not built.

In another terminal, once `make debug` is sitting frozen:
```
gdb build/kernel.bin -ex "target remote localhost:1234"
```
then `break kernel_main` (or any other function -- `CFLAGS`/
`USERLAND_CFLAGS` both carry `-g` now, so `kernel.bin` and every
userland ELF carry real DWARF symbols: function names, source lines,
local variables, not just raw addresses) and `continue`. Confirmed
working end-to-end: `break kernel_main` + `continue` correctly runs
the CPU from reset through GRUB/multiboot2 and stops exactly at
`kernel_main`, with a real backtrace showing source file/line.

Kept at `-O2` (not dropped to `-Og`/`-O0` for a separate debug build)
deliberately -- same binary as every other build, just now carrying
symbols. Some locals may show as "optimized out" in GDB as a result;
accepted rather than maintaining a second build config just for
debugging.


## Testing in QEMU headlessly, via QMP

**First: is this actually a GUI change?** If not, `tools/vm.py` is
faster and gives you text you can assert on instead of a screenshot you
have to read. The order of cheapness is `boot_smoke_test.py` (does it
boot) -> `make test` / `vm.py exec` (does it work) -> QMP (does it look
right). Reach for QMP when the answer genuinely depends on pixels.

There is no interactive display here, so GUI testing goes through QEMU's
QMP socket: launch headless, drive keyboard/mouse over QMP, `screendump`
to prove it visually. **`docs/testing.md` is the full reference** -- the
`QMPSession`/`GuiFlow` API and the dozen gotchas already handled in
`tools/qmp_test.py` (why `-display none` silently breaks input routing,
why there must be no `usb-tablet`, the `-daemonize` launch, cursor drift
and `recalibrate()`, dropped keystrokes, absolute screendump paths).
Read it before writing QMP code. Six things worth knowing without
opening it:

- **Use `tools/qmp_test.py`, don't rederive it.** Import `QMPSession` /
  `launch_qemu_cmd()` rather than hand-rolling socket/JSON code or a
  `qemu-system-x86_64` invocation; add to the module when it lacks
  something, instead of writing a one-off.
- **Never `pkill -f qemu-system-x86_64`.** It cannot tell your headless
  launch from the user's interactive `make run` window. Kill only the
  PID your own launch wrote to its `-pidfile`.
- **TWO GUESTS ON ONE QMP PORT DO NOT FAIL AS A PORT CLASH.** Everything
  here defaults to 4445, so a second launch fights the first and the
  error surfaces MINUTES LATER as a `BrokenPipeError` or
  `ConnectionResetError` in whichever tool was mid-command -- which is
  never the tool that caused it. `tools/port_guard.py` refuses at the
  launch now, from the same two chokepoints `iso_guard.py` guards.
  **Use `--instance auto` (or `--instance N`) whenever anything else
  might be running** -- `gui_regress.py` holds slots `0..DEFAULT_JOBS-1`
  while it runs (up to 8, i.e. QMP 4445-4452), so don't hand-pick a low
  slot; `auto` probes for the lowest free one and prints it, which is
  what makes the run replayable. **What it does NOT fix is CPU contention**: a tool
  run beside the full suite is port-safe and still competes for cores,
  and an app that ANIMATES can fail a settled-frame comparison under
  that load. So a concurrent run is fine for getting an answer, and not
  evidence when the suite is the thing being judged.
- **DON'T ADD A WAIT LOOP FOR WORK THAT IS ALREADY IN THE BACKGROUND --
  and if you do write one, never `pgrep` for a pattern your own command
  line contains.** `gui_regress.py` and `preflight.sh` take minutes, so
  they get run in the background; their COMPLETION is already the
  signal. A `until ! pgrep -f gui_regress; ...` waiter beside them is
  redundant AND broken: that shell's own command line contains
  `gui_regress`, so `pgrep -f` matches the waiter itself and the loop
  can never exit -- reporting a finished suite as still RUNNING, which
  is worse than the leak because it is indistinguishable from the real
  thing. If a wait genuinely is needed, wait on the ARTIFACT (`until [
  -s out.log ]`), which cannot match itself.
- **Prefer `tools/gui_debug.py` to pixels** for anything not literally
  about rendering -- it returns facts to assert on rather than an image
  to interpret. It is asynchronous: call `DebugConsole.settle()` before
  asserting, and never replace that with a fixed sleep.
- **Screenshots are a TESTING TOOL, not a deliverable.** Take as many as
  a check needs, into a scratch directory. Do NOT save them into
  `screenshots/` as evidence -- that convention is retired. Show the
  user one when SEEING it is the answer; don't attach one to prove a
  check passed, since a `gui_regress.py` table is better evidence than
  an image a reader has to interpret.

## tools/

Dev/build helper scripts, not compiled or shipped as part of the OS.
**`docs/tools.md` is the full reference** -- what each one does, why it
exists, and the traps it encodes. This is the index; read that file
before reaching for anything here you have not used recently, and add to
it (not to a one-off script) when something would save a future session
real time. The bar is "does this fix a rederive-from-scratch cost".

- **Is it MINE, or was it already broken?** -- `predates.py "<command>"`
  stashes, rebuilds HEAD, runs it, restores and compares. CLAUDE.md's
  own rule is that "it predates me" is a MEASUREMENT; this is that
  procedure as a script, including the `-u` and the recover-by-SHA that
  the prose version gets wrong.
- **Has the on-demand half rotted?** -- `ondemand_sweep.py` runs the
  ~22 tools neither `preflight.sh` nor `gui_regress.py` covers. TWO WERE
  FOUND RED BY ACCIDENT in one session after rotting for an unknown
  period. A SKIP is counted apart from a PASS. Never a gate.
- **Verify before delivering** -- `preflight.sh` (**stop your `vm.py`
  guest first -- it refuses to start while one holds disk.img's write
  lock**; the gate: clean build +
  iso + `check_deps.py` + `check_layout.py` + `check_dispatch.py` +
  `check_widget_ops.py` + `check_key_routing.py` +
  `boot_smoke_test.py` + `ktest_run.py` + `usertest_run.py`),
  `check_docs.py`, `check_licenses.py` (**every vendored port and shipped font is named in `LICENSE`** -- `userland/ports/doom/` is GPL-2-OR-LATER inside an MIT repo and was not mentioned there at all, and the font inventory said two when there were five), `check_tool_commands.py` (**every guest command a
  tool drives still EXISTS** -- it found `kvm_soak.py` driving `delete`,
  which is `rm` now, so its cleanup had been a no-op and it had been
  littering `disk.img`. It cannot see a command whose OUTPUT changed,
  which is the rot that actually bit `fs_switch_test`; that is
  `ondemand_sweep.py`'s job).
- **Drive a VM** -- `vm.py` (text in, text out: the fastest path for
  anything that is not about pixels; **`vm.py spawn <path>` runs a
  spawned test and prints the file it writes**, replacing the
  spawn/sleep/cat dance), `qmp_test.py` (QMP GUI helpers),
  `gui_debug.py` (ask the WM what it is doing), `gui_flow.py`,
  `shell_flow.py`, `serial_console.py` (COM1 as a socket: text in, text
  out, and it does not care who owns the keyboard), `serial_capture.py`
  (read a guest that is DYING),
  `watch_vm.sh` (view-only VNC onto a headless run), `run_release.sh`.
- **Test runners** -- `boot_smoke_test.py` (does it boot),
  `ktest_run.py` (`make test`), `usertest_run.py` (the `/tests` ELFs),
  `faulttest_run.py` (the ones that fault ON PURPOSE),
  `gui_regress.py` (every GUI tool, ~300 checks, ~1.5 min),
  `flake_hunt.py` (is it intermittent, and at what RATE),
  `damage_sweep.py` / `damage_hunt.py` (the damage invariant).
- **GUI tools**, all run by `gui_regress.py` -- `blank_window_test.py`,
  `calculator_client_test.py`, `compositor_test.py`,
  `compositor_death_test.py`, `settings_test.py`, `crashtest_test.py`,
  `font_test.py`,
  `cursor_theme_test.py`, `desktop_entries_test.py`, `dialog_test.py`,
  `forcequit_test.py`, `gfxdemo_test.py`, `idle_desktop_test.py`,
  `menubar_test.py`, `notepad_client_test.py`, `sched_gui_test.py`,
  `screen_surface_test.py`, `scrollbar_test.py`,
  `single_instance_test.py`, `taskmgr_test.py` (**the table widget, and
  the only checks in the suite that use the KEYBOARD there** -- every
  other one drives by mouse, which is how the table's whole key handler
  stayed dead and unnoticed), `uapp_test.py`,
  `uiclient_test.py`, `uidemo_test.py`, `uterm_test.py`,
  `keyup_test.py` (key RELEASES reaching a ring-3 client, including the
  four modifier keys -- its load-bearing check holds a key DOWN with
  `QMPSession.key_down()`, which `send-key` cannot do),
  `winclient_test.py`, `imgview_test.py`, `icons_test.py`,
  `mines_test.py`, `filemanager_test.py` (**the File Manager, and every
  file operation asserted through `ls` rather than through the app**),
  `calendar_test.py` (**the tray clock's calendar popup** -- its grid is
  checked against the HOST's `datetime`, which shares no code with the
  guest's `cal_days_from_civil()`, and every open/close check is paired
  with the panel's own pixels so "flagged open" cannot pass for "drawn").
- **Run on demand, not in the gate** -- `doom_test.py` (DOOM runs, draws,
  animates and takes input; SKIPS cleanly when no IWAD has been fetched,
  which is why it is not in the suite),
  `cursor_ibeam_test.py` (**named pointer shapes: the I-beam, the busy
  pointer, and the clamp** -- all four ways a shape gets named, each
  with a control point beside it; the shapes are told apart by where
  they sit RELATIVE TO THE HOTSPOT, so a MISSING sprite fails every test
  rather than one. Two load-bearing checks: the title bar plus a window
  dragged UNDER the taskbar for the clamp, and a wedged window NOBODY
  ASKED TO CLOSE, which is the ping cadence's test as much as the
  cursor's),
  `console_bleed_test.py` (**the kernel console must not paint over the
  desktop, and must not lose the text either** -- a noisy program's fd 1
  reaches the framebuffer console, which used to blit its whole buffer
  over the screen; both halves are checked, since a fix that just stopped
  the console DRAWING would pass one and fail the other. DELETES
  `/etc/services.d/toywm` and does not restore it -- `make iso` re-seeds
  it), `ansi_cursor_test.py` (ANSI
  cursor movement and erasing, as PIXELS -- it kills the desktop first,
  since the console is what it photographs), `init_test.py` (init and
  service supervision), `console_shell_test.py` (a `text` boot reaching a ring-3
  prompt with the kernel shell stood down; boots twice and rewrites
  `/etc`),
  `terminal_probe.py` (**the GUI Terminal: editing keys, paging,
  scrolling, clearing** -- the keymap half asserts through the
  FILESYSTEM, since a keystroke that worked leaves different bytes on
  disk; the pixel half reports the PERCENTAGE of the content area that
  moved, because the caret blinks and "changed" is not a measurement.
  Encodes five harness traps that cost five invalid runs),
  `usb_test.py` (**an xHCI controller and a HID keyboard and mouse** --
  its control is free and worth knowing: attaching `usb-kbd` makes QEMU
  route keystrokes to THAT device, so on a build whose USB driver is
  dead the guest receives nothing at all, from USB or PS/2. Measured
  before the driver was written. Its load-bearing check is the RING
  WRAP: a driver that ignores the event ring's cycle bit works for
  exactly one lap -- 256 TRBs, about 128 keystrokes -- so it types 25
  files and asserts the LAST one. Also drains the serial socket as it
  types, because an undrained COM1 stalls the whole guest and reads
  exactly like a driver dying after N keys),
  `kbd_test.py` (**`kbd`'s four columns, on both input drivers** -- the
  load-bearing half is the SECOND boot: the same keys must give the same
  keycode and the same character on `INPUT=virtio` with the scancode
  column BLANK, which nothing that is not really reading each stage can
  fake),
  `grep_test.py` (**`/bin/grep` through a real ring-3 shell** -- the
  engine is covered three other ways, so what this adds is the half that
  is not regex: reading stdin from a PIPE, the flags and the exit
  status. The pipe is unreachable from the kernel debug console, which
  splits on spaces and hands `|` to the program as an argument),
  `keyboard_paths_test.py` (**the same keys do the same thing on
  PS/2 and on virtio-input** -- boots both, types `_` and `|`, and asserts
  through the FILESYSTEM rather than the screen, because `_` draws
  nothing on the ring-0 console and a screenshot cannot tell that from a
  lost keystroke), `ctrlc_test.py` (**Ctrl-C interrupting a real job**, through
  the real keyboard on a `text` boot: a spinning job dies, a two-stage
  PIPELINE dies as a unit, the shell survives, and at an empty prompt the
  key is still a keystroke that cancels the line),
  `jobs_test.py` (**job control** -- its sibling, same shape, same `text`
  boot: a job SURVIVES and stops accruing CPU, `jobs` names it,
  `fg`/`bg` resume it, `&` backgrounds one, a pipeline suspends as ONE
  group, and a background READER is stopped rather than served. Three
  discriminating checks: the Ctrl-C after `fg`, which catches a resume
  that forgot the terminal; the command typed after `cat &`, which
  catches keystroke theft; and a short `&` job reaped with NO KEYSTROKE
  SENT, which is what `SIGCHLD` bought and which every other check here
  would pass without),
  `stdin_test.py`
  (blocking fd 0 and `/bin/tosh`, which
  needs the physical console and so takes the desktop down first),
  `qemu_matrix.py` (the suite against SEVERAL QEMU
  versions in Docker -- **the bug class one QEMU cannot show you**: a
  virtio-blk defect was invisible on 11.1 and reproduced every time on
  8.2.2, which is what GitHub's runner has, because the host decides
  which clocksource the kernel picks. Faster and more controllable than
  finding it through CI),
  `kvm_soak.py` (the timing bugs TCG cannot show),
  `ls_test.py` (`/bin/ls`'s flags, ordering and the listing cap -- it
  stages a 300-entry directory from the HOST, since the cap is
  unreachable by typing `touch`),
  `hires_test.py` (a desktop above 1280x720, and whether a client window
  can actually FILL it -- the `WIN_CLIENT_MAX_W/H` vs `DISPLAY_MAX_W/H`
  pair; needs an ISO built with `KCMDLINE="video=1920x1080"`, since at
  the default mode every check in it passes vacuously),
  `taskbar_test.py` (opens enough windows to overflow the taskbar and
  asserts the strip never reaches the tray -- shrink, then grouping by
  application; slow, since every window is a real process),
  `mem_stress.py`, `frame_balance.py` (does teardown balance),
  `fat32_test.py` (**FAT32 and the mount table against an INDEPENDENT
  implementation** -- `mtools` reads back what the guest wrote and
  `fsck.fat` audits the volume, neither sharing a line with
  `kernel/fs/fat32.c`; the same call `regex_hostcheck.py` and
  `uimg_hostcheck.py` made. Its load-bearing checks are the ones a
  broken driver would still pass: GRUB's own `grub.cfg` read through a
  real chain walk, a 185 KiB BINARY extracted on the host and compared
  byte for byte, and `fsck.fat`'s verdict. Boots twice against a COPY of
  `disk.img`; SKIPS without `mtools`),
  `multidisk_test.py` (**two disks on two different drivers, which is the
  configuration no other test here boots** -- every one attaches exactly
  one, and that is the shape the enumerate-everything bug needed. Asserts
  both are named, that `root=` picks the boot disk over the precedence,
  and that an unknown `root=` reports rather than hangs),
  `partition_test.py` (**toy-os booting with its filesystem INSIDE an
  MBR or GPT partition** -- the only thing that exercises `vfs.c`'s
  boot-time scan and `block_part.c`'s window. Its load-bearing check is
  `df`: a kernel ignoring partitions still BOOTS, just RAM-only, so
  "it booted" proves nothing, while "the mounted volume is 256 MiB and
  the image is 2 GiB" only a correct window can produce. Reboots, and
  finishes by driving `/bin/mkpart` in the guest),
  `diskmark_test.py` (**the Disk Mark GUI benchmark** -- its
  load-bearing check is the NUMBERS, since a build whose arithmetic
  truncated to zero still logged "all four passes complete"; the title
  bar is its CONTROL and immediately caught the client missing the
  compositor's pings, i.e. running the whole benchmark as
  `(Not Responding)`),
  `ahci_test.py` (**toy-os booting with its filesystem on a SATA drive
  behind an AHCI HBA** -- the only thing that reaches
  `kernel/drivers/ahci.c`, since every `ahci` KTEST skips on a machine
  with no controller. Its load-bearing check is therefore **`0
  skipped`**, not "the tests passed". Also reboots to prove the write
  landed, compares the guest's read of `/boot` against **mtools** on the
  host, and rewrites the image's `grub.cfg` to test the `noahci`
  precedence rung),
  `virtio_boot_test.py` (TFS3 mounting off virtio-blk on a
  machine with NO IDE controller, written and read back across a
  REBOOT), `virtio_gpu_test.py` (the GPU -- the ONLY thing here that
  boots `-vga virtio`, so it is also what stops the driver's KTESTs
  skipping on every run; its pixel-format oracle is a second boot on
  `-vga std`), `virtio_input_test.py` (keyboard, mouse and
  TABLET on virtio -- `vm.py --virtio-input`; it is the only thing that
  attaches them, and it asserts the shared-IRQ case `irq.c`'s handler
  chain exists for), `live_boot_test.py`, `fs_switch_test.py`, `tfs3_v1_test.py`,
  `mkpart_test.py`, `demo_test.py`.
- **Disk images, from the host** -- `seed_disk.py` (the format-aware
  front end `make iso` calls; **`--partition gpt|mbr` builds a
  PARTITIONED, BOOTABLE image** -- a BIOS boot partition, a FAT32
  `/boot` and the filesystem -- instead of a flat volume at LBA 0),
  `install_grub.py` (**puts GRUB and the kernel ON `disk.img`**:
  `boot.img` at LBA 0, `core.img` embedded in the BIOS boot partition,
  `/boot/kernel.bin` and `/boot/grub` written into the FAT32 one with
  `mtools`. Also `boot_medium()`, the ONE answer to "does this image
  boot itself, or does it need the ISO?", which every launcher here
  asks), `tfs3_writer.py` (**every
  subcommand takes `--at-lba`/`--sectors`**, the host-side twin of the
  kernel's volume seam -- that is what lets one image hold a table AND
  a filesystem in a partition), `mkpart_test.py` (**`--layout
  SIZE[:KIND][,...]` writes a REAL, usable table**, aligned and with
  GPT's backup structures, where the default writes a synthetic one for
  the parser to chew on; `:bios`/`:esp` name a firmware partition type,
  and `volume_of()` finds the TFS3 volume by LOOKING rather than
  answering "partition 1"),
  `fetch_wad.py` (puts a Doom IWAD where `make iso` will seed it -- the
  WAD is deliberately NOT in the repository; `--from` takes one you
  already own).
- **How big is it** -- `loc.py` (source lines with generated files,
  comments and blanks excluded; add anything a `gen_*` writes into the
  tree to its `GENERATED` list, or the count silently inflates).
- **Diagnose** -- `panic_resolve.py` (name every address in a panic),
  `regex_hostcheck.py` (**tolibc's `<regex.h>` against GLIBC's**, over the
  same case table `/tests/regex_test` runs -- an oracle that shares no
  code catches the failure a self-test cannot, which is an EXPECTATION
  being wrong. Every remaining difference is listed with its reason in
  `KNOWN_DIVERGENCES`; an unexplained one fails),
  `uimg_hostcheck.py` (the JPEG decoder against libjpeg on the HOST,
  over a couple of hundred generated images -- the breadth
  `/tests/uimg_test` cannot carry),
  `pixel_probe.py` (read exact pixel values -- how a GUI change is
  verified), `screenshot_diff.py`, `iso_guard.py`.
- **Generated data and the build** -- `gen_version.sh` / `set_version.sh`
  (versioning), `genfont.py` / `genttf.py`, `gen_kbs.py` (keyboard
  layouts from XKB data), `gen_cursors.py` (cursor themes),
  `gen_imgdata.py` (the wallpapers, and the image decoders' test vectors
  -- whose reference pixels are PILLOW's, not this decoder's),
  `gen_icons.py` (the app icons, drawn here and encoded by Pillow so no
  QOI writer in this repo can agree with a bug in its reader),
  `genrelocs.py` (the kernel's own relocation table), `gen_syms.py` (the
  panic symbol table), `gen_decisions_index.py`, `gen_commands_index.py`,
  `gen_next_up.py` (the roadmap's "Next up" section, from the `**NEXT**`
  markers on the items themselves).
- **The repo itself** -- `backup_repo.sh` (run it before ANY change to
  the repo's identity or history -- a mirror clone is not a backup here,
  release assets live only on GitHub).

**HOST TOOLS THIS REPO EXPECTS, none required to build it** --
`docs/tools.md`'s "Host tools this repo expects" has the full entry for
each. The two worth knowing before you start:

- **`bear -- make all` regenerates `compile_commands.json`, and that is
  what makes `clangd` work here.** Run it from a `make clean`, since it
  only captures what actually recompiles. Reach for the LSP rather than
  grepping for a signature: `userland/ui/` is a couple of dozen widgets
  whose ops tables are easy to guess wrong, and guessing cost a build
  cycle and five wrong signatures in one file the day this was set up.
  (`tools/check_widget_ops.py` prints the current count; it was written
  here as a number and had already drifted by two.)
- **`ruff check tools/` and `shellcheck tools/*.sh`** before touching a
  harness. `ruff.toml` pins a narrow ruleset (`F` + `E9`) on purpose --
  the default reports ~320 style findings and buries the class that
  matters here, which is a harness bug reporting a healthy system as
  broken. NEITHER is in `preflight.sh`: the gate must not start
  requiring a tool a checkout may not have, same rule that keeps Docker
  out of it.

`ccache` is wired into the Makefile (`CC = $(CCACHE) gcc`, falling back
to plain `gcc`), so a checkout without it is unaffected. It matters on
the GATE, which always starts with `make clean` -- measured 2.37s ->
0.40s for a full rebuild.

Five standing rules that are cheaper to know than to rediscover:

- **WHAT THE USER SAYS, AND WHAT IT MEANS.** Three checks, three
  phrasings, and they are easy to confuse because two of them mention
  QEMU:
  - **"run the matrix"** / "check the old QEMUs" / "test on 8.2" ->
    `python3 tools/qemu_matrix.py` (add `--versions 8.2`, `--virtio`).
    LOCAL, Docker, ~15s per version. This is the one for "does this
    depend on the host's QEMU?".
  - **"run CI"** / "trigger the GitHub build" / "kick off Actions" ->
    `gh workflow run build.yml`, then poll `gh run list`. REMOTE, on
    GitHub's runner, ~90s when it behaves. This is the one for "does it
    build from a clean clone on someone else's machine?". It no longer
    runs on push, so it only happens when asked or on a release tag.
  - **"run preflight"** / "verify" / "is it safe to commit" ->
    `bash tools/preflight.sh`. The ordinary per-change gate.
  "Run the tests" with no qualifier means preflight. If a request is
  ambiguous between the local matrix and GitHub, ask -- they answer
  different questions and one of them costs a remote round trip.
- **`qemu_matrix.py` RUNS AT A RELEASE AND WHEN THE USER ASKS -- not
  automatically** (standing instruction, 2026-08-19). It is ~15s per
  version on a cached image (69 MB each; the first run per version
  pulls a base), so ~45s for all three, and it is deliberately NOT in
  `preflight.sh` -- which must not start requiring Docker -- and not a
  per-change habit.
  **OFFER it, do not silently run it,** when a change plausibly depends
  on the host's QEMU or CPU: a driver, a poll loop, a timeout, a
  clocksource, DMA. And reach for it unasked only when a failure will
  not reproduce locally, which is the case it was built for.
  **A finding is a REPORT, not a blocker.** An older QEMU disagreeing
  can be an emulator quirk rather than a bug in this OS, and deciding
  which is the user's call -- bring them the evidence (which versions,
  what differed, whether it reproduces on the current one) and let them
  choose whether it is worth fixing. Do not hold up a release on it
  unaided.
- **GITHUB CI NO LONGER RUNS ON EVERY PUSH** (2026-08-19). It runs on a
  **release tag** and on demand (`gh workflow run build.yml`). What it
  is uniquely good for is a CLEAN-CHECKOUT build on somebody else's
  machine -- "works because of an untracked file, a stale object, or
  something only installed locally" is invisible to any local run. What
  it was bad at was being a per-push gate: ~90 s at best, and red for
  reasons that were not this repo's (one run died with `apt-get
  install` timing out after 8 minutes). A gate that cries wolf gets
  ignored, which is worse than no gate. The half worth keeping -- a
  second QEMU -- is `qemu_matrix.py` now.
- **`gui_regress.py` is the standard check** after touching
  `userland/`, or anything the WM draws -- always with `--logs DIR`. It
  is ~56s; **its wall clock is bounded by the SLOWEST SINGLE TOOL and by
  the sum over the job count, whichever is larger** (~408 tool-seconds
  over ~25 tools, `notepad` ~41s the ceiling), so on any fan-out here
  look at the maximum, not just the total. The slow tools WAIT on real
  timeouts no guest CPU shortens; the levers that worked were cutting
  those floors and making every tool wait on an OBSERVABLE rather than a
  fixed sleep -- `enter_gui()` (in `gui_debug.py`) polls the desktop
  ready instead of sleeping ~3s, and `menubar`/`taskmgr` poll the app's
  own layout/log reports. `DEFAULT_JOBS` is `min(12, cores//2)`: the cap
  only bites a host with more than 24 hardware threads, so
  oversubscription stays off the common box.
- **`demo_test.py` is ON DEMAND ONLY.** Never add it to `preflight.sh`,
  `gui_regress.py` or CI; it boots its own ISO and the demo is a
  showpiece, not something an ordinary change breaks.
- **A clean `damage_sweep.py` proves nothing until `--positive-control`
  has shown the harness can fail.** Same for any positive control here.
- **`iso_guard.py` refuses a stale `toy-os.iso`**, from `vm.py` and
  `launch_qemu_cmd()`, because `make all` without `make iso` otherwise
  leaves the whole suite testing the previous build and reporting a
  clean PASS. `TOYOS_ALLOW_STALE_ISO=1` bypasses it deliberately.
- **A copy of `disk.img` must stay SPARSE** -- `cp --reflink=auto
  --sparse=always`. It is ~4 MB of data in a 9 GB sparse file, so a
  hole-filling copy costs 9 GB, of RAM when the destination is a tmpfs.

## docs/

**This file is the always-loaded context, so it holds RULES -- what stops
a session doing the wrong thing before it knows to look anything up.
Reference material lives in `docs/` behind a one-line pointer.** Add
detail there, and keep the pointer here to a line. What each file is:

- **`docs/testing.md`** -- how to run and drive this OS, the QMP
  mechanics, what the emulator does and does not model.
- **`docs/commands/`** -- one page per command; see the entry for
  `docs/commands.md` below. **A NEW COMMAND NEEDS ITS PAGE IN THE SAME
  CHANGE** -- the build refuses otherwise, which is the point, but a
  page written later by somebody reconstructing the reasoning is worth
  much less than one written by the person who had it.
- **`docs/tools.md`** -- the full reference for every script in
  `tools/`.
- **`docs/conventions/`** -- the BODY of every convention this file
  indexes by headline: `kernel.md`, `gui.md`, `storage.md`, `shell.md`,
  `build.md`. Read the one for the area you are touching. A new
  convention goes in the file for its area **and** gets its headline
  added to this file's index, or it is invisible to the next session --
  that pairing is the whole mechanism, and half of it is not optional.
- **`docs/decisions.md`** -- the INDEX over `docs/decisions/`, which
  answers "why does toy-os work this way?" split by area (`kernel.md`,
  `gui.md`, `storage.md`, `build.md`, `shell.md`, `drivers.md`,
  `workflow.md`). See the rules below before adding to it.
- **`docs/gui-guidelines.md`** -- how the GUI is supposed to look and
  behave: the four `enum ui_state` interaction states and their flat
  (non-bevelled) rendering, the press-then-commit-on-release rule, the
  `on_hover` contract, text/layout budgeting, and how to verify a GUI
  change properly. **Read it before touching anything drawn.**
- **`docs/filesystem-layout.md`** -- what lives where on the OS's own
  disk (`/bin` vs `/tests` vs `/usr/share` vs `/etc`), the deliberate
  divergences from the FHS, and the budgets that constrain it (64-byte
  caller-side paths everywhere). Not advisory: `tools/check_layout.py`
  reads its table and fails `preflight`/CI if the built image disagrees,
  in either direction. Read it before adding a directory, a config file
  or any seeded data.
- **`docs/boot-flags.md`** -- every word the kernel looks for on the
  GRUB command line (`nokaslr`, `nopat`, `live`, `demo`).
  Matching is by SUBSTRING with no parser, spread across five files with
  no registry, so **this table is the only list of them** -- add a row
  when adding a flag.
- **`docs/settings-and-queries.md`** -- facts vs settings vs tunables,
  `config`'s verbs, and how an app reads or changes either. Read it
  before adding a setting.
- **`docs/query-design.md`** -- how kernel state reaches ring 3, and why
  it is NOT `/proc`. BUILT: `SYS_QUERY`, the self-describing
  `QUERY_PROVIDERS` class, and a provider per fact. Its staging section
  is the authority on which introspection commands have moved.
- **`docs/errno-design.md`** -- giving a failed syscall a REASON, staged
  so each step ships on its own.
- **`docs/libc-design.md`** -- **`tolibc`**, toy-os's C library: the six
  stages that built it and what each one found. **Read it before
  starting any libc-shaped work.** The headline facts: it is BUILT
  (stdio, math including the transcendentals, time, dirent, setjmp,
  scanf), cJSON runs on it, and **its bar for adding a function is the
  opposite of the rest of this project** -- complete rather than
  second-real-caller, because its audience is code not yet written.
  `userland/libc/README.md` is the shorter front page.
- **`docs/dynlink-design.md`** -- shared libraries: what they would
  take, staged, and **the honest case AGAINST** them here (the memory
  payoff is near zero at this scale; static linking is a respectable
  modern choice). Read it before starting anything `.so`-shaped -- its
  first finding is that `mmap` blocks everything and is worth building
  on its own merits anyway, and its second is that this project ALREADY
  does relocation, for KASLR.
- **`docs/signals-design.md`** -- signals, a foreground process, and what
  `Ctrl-C` actually needs. Designed, not built. **Read it before
  starting any of Phase 1's signal/TTY/job-control work**: its whole
  point is that those are one problem, and that building signal delivery
  alone yields a working `kill -TERM` and a `Ctrl-C` that still does
  nothing.
- **`docs/smp-design.md`** -- more than one core: ACPI/MADT, the Local
  APIC, bringing up application processors, a real spinlock and ONE
  kernel lock first (Linux 2.0's move), then splitting it in measured
  order. Designed, not built. **Read it before adding a module-level
  buffer to anything a syscall reaches**, and note the two findings that
  change the scoping: the RSDP is already in hand (a multiboot2 tag
  `multiboot.c` already walks, so no AML interpreter is needed), and the
  BKL is the thing that makes SMP shippable before the locking audit is
  done. It carries the measurement of what is single-core in the tree
  today, and the honest case AGAINST.
- **`docs/rootfs-design.md`** -- what the root filesystem is allowed to
  be: a PARTITION on a drive, or RAM. Designed, not built. **Read it
  before touching `fs_init()`'s mount policy or adding a backend**, and
  note the two findings that change how the work is scoped: today's
  "RAM-only" is a label on NOTHING (a diskless boot mounts no filesystem
  and every `fs_*` call fails), and a `ramfs` backend has to land BEFORE
  a flat volume can be refused, or the refusal's failure path is a
  machine with no filesystem at all.
- **`docs/commands.md`** -- the INDEX over `docs/commands/`, which holds
  ONE PAGE PER COMMAND (every `/bin` program and every shell builtin).
  The index keeps only what is true of the shell rather than of any one
  command -- how a name is resolved, the line-editing keys.
  **`tools/check_docs.py` FAILS THE BUILD when a command has no page**,
  and when a page documents nothing that exists; where a program
  declares a `cmd_usage()` string -- a literal or a named constant -- the
  page must carry it verbatim, so a
  flag added to the program and not to the page is a build failure. The
  prose is deliberately unchecked -- that is the part only a person can
  write. Exemptions are listed by name with a reason in that script's
  `COMMAND_PAGE_EXEMPT` (`init`, `hello`, `tosh`, and the `gui3`/`nano`
  aliases). See `docs/commands/README.md`.
- **`docs/bugs.md`** / **`docs/roadmap.md`** / **`docs/roadmap-details.md`**
  -- what is broken, what is not built yet, and the long form of both.

**Adding a decision entry**: write it in the file for its area and run
`tools/gen_decisions_index.py`. The index is GENERATED and
`tools/check_docs.py` fails the build when it is stale, because an index
kept true by someone remembering is the shape of every convention this
project has had to delete. **A NEW FILE must be added to that script's
`ORDER`** -- it refuses to run otherwise, since `workflow.md` was named
here for a long time while the file did not exist, and creating it
indexed nothing while every check still reported the index as current.

**Entries are SELF-CONTAINED.** They used to be pointers into a
changelog that held the real writeup, and when that was deleted the
entries leaning on it were the ones left stranded. Write the reasoning
in the entry. The bar: it answers "why this way and not the obvious
way", and a future session would plausibly re-litigate it. When you
resolve a "wait, why is this built this way" question during a session,
ask whether a future session would hit it again -- if so, it earns an
entry, the same judgment call as `tools/`'s "does this fix a
rederive-from-scratch cost" bar.

**Milestones are NAMED, not numbered** -- a milestone is its title and
its position is its layer, so nothing is renumbered when one is
inserted. Older `Milestone N` references in git history and some source
comments resolve through the one-way legend at the end of
`docs/roadmap-details.md`; do not add a number to a new one.

**URGENCY IS A SECOND AXIS, AND IT IS MARKED WHERE THE ITEM LIVES.**
Put `**NEXT**` on the roadmap item itself and run
`tools/gen_next_up.py --write`; the "Next up" section at the top of
`docs/roadmap.md` is GENERATED from those markers, and
`tools/check_docs.py` fails the build when it is stale. Do NOT hand-write
a list of titles at the top -- that is the pointer-someone-must-remember
shape every maintenance burden this repo has deleted. `**NEXT**` means
"do this before the unmarked work around it", never "this is broken":
something that MISBEHAVES belongs in `docs/bugs.md`.

**`docs/roadmap.md` IS ORDERED BY WHAT MUST BE BUILT FIRST.** Phases 1-4
are a dependency chain -- the system runs itself, then memory, then the
process model, then the userland runtime -- so reading top to bottom
answers "what next". Below them are **tracks** (storage, GUI, hardware,
tooling) which depend on neither the phases nor each other, ordered
internally only; saying so is the honest part, since a total order would
imply dependencies that do not exist. **EVERY ITEM IS ONE LINE.** No
rationale, no measurements, no repros -- those go to
`docs/roadmap-details.md` under a heading of the same name, which is why
a milestone's title must match in both files. A **Needs:** line appears
at most once per milestone and only where the dependency is real and not
obvious. When you tick an item, keep it to one line too.

## The session workflow skill lives IN this repo

`.claude/skills/toy-os-feature-workflow/` -- the end-to-end playbook a
session follows (research first, offer real choices, build and test with
proof, write the docs, ship). It used to live in `~/.claude/skills/`,
outside version control, which meant ~1,750 lines of accumulated project
knowledge had no history, no diff review and no backup. It is tracked
here now for the same reason everything else is.

**`SKILL.md` is the PLAYBOOK; the accumulated lessons live in
`references/`.** Everything a session learned the hard way was inline
until it made `SKILL.md` bigger than this file, at which point the whole
thing loaded on every invocation whether or not any of it applied. It is
split by what an entry is ABOUT -- `session-testing.md`,
`session-diagnosis.md`, `session-gui.md`, `session-design.md`, beside
the existing `delivery-checklist.md`, `questions-that-worked.md` and
`doc-templates.md` -- the same split-by-area call `docs/decisions.md`
already made, and for the same reason. **A new lesson goes in the
reference file for its area, not back into `SKILL.md`**, which holds the
sequence and nothing else.

Two more consequences. **Update it in the repo**, not in the home
directory, or the two copies drift and the untracked one silently wins.
And `.gitignore` excludes `/.claude/worktrees/` specifically rather than
all of `.claude/`, because those are transient checkouts while the skill
beside them is real content.

## Delivering changes

**END EVERY DELIVERY WITH A SHORT "TRY IT YOURSELF" GUIDE** (standing
project instruction). Whenever a change adds or alters something a
person can SEE or DO in toy-os, the final response must say -- in a few
lines, not an essay -- how to reach it on a real boot: which `make run`
flags if it needs particular ones, which app or which command, what to
type, and what should happen. The maintainer runs this OS interactively;
a change that is only ever demonstrated through a test tool's pass/fail
table has not actually been handed over.

Three rules that keep it useful rather than decorative:

- **Say what to TYPE and what to EXPECT**, as a table or a short block.
  "Ctrl-C now works" is not a guide; "`spin_test 900000`, then Ctrl-C ->
  job stops, `^C`, prompt back" is.
- **Say when a feature is NOT reachable from the default boot.** Several
  are: `Ctrl-C` needs a `text` target, the ATA/PIO control only greys
  out on a machine with no DMA engine (`make run VIRTIO=1`), `hires` work
  needs `KCMDLINE="video=1920x1080"`. A guide that quietly assumes the
  default boot sends the reader to look for something that cannot be
  there.
- **Skip it when there is genuinely nothing to see** -- a refactor, a doc
  edit, an internal invariant with no user-facing surface. Say so in a
  line rather than inventing a demonstration.

This is the same instinct as the file list and the layer diagram below,
pointed at the person rather than at the code: the file list says what
changed, the diagram says what the system now looks like, and this says
what is different when you boot it.

List every file added or edited in the final response, as a compact
list (standing project instruction) -- always, regardless of mode.

**AND WHEN A CHANGE MOVES A LAYER BOUNDARY, DRAW THE STACK** (standing
project instruction). If the work adds a layer, removes one, moves a
seam, or changes who calls whom, the final response shows the layering
-- an ASCII diagram of the stack with the changed part marked, and the
directory tree when files moved or appeared. Not prose describing it.

**ONCE, WHEN THE WORK IS DONE** -- this is a delivery-time summary, not
a running commentary. Draw the stack as it ENDS UP, in the response
that hands the finished work over. Not per stage, not per commit, and
not mid-task while the shape is still moving: a diagram of an
intermediate state is worse than none, because it describes something
that was never true for longer than an hour.

The reason is that a file list says WHAT changed and never says what
the system now looks like, and this project's structure is the thing
sessions most often re-derive from scratch: the `apps/` vs `userland/`
split, `kapi.h` as the one app-facing header, the registries
(`display_driver`, `block_device`, `clocksource`) that a new driver
plugs into instead of inventing a mechanism beside. A picture at
delivery is what makes the NEXT session inherit that instead of
grepping for it.

What counts as moving a boundary: a new subsystem directory; a new
registry or a new implementation of an existing one; a header changing
audience (`kernel/` -> `api/`, or a new one in either); a call site
moving between rings, or between the kernel and a driver; anything that
changes what a component is allowed to include. An ordinary bug fix,
a new widget in an existing toolkit, or a doc edit does NOT -- do not
draw a diagram for the sake of having one.

Show BOTH axes when they differ, because in this codebase they usually
do: what the thing sits ON (the vertical call stack) and what it
PLUGS INTO (the class registry). virtio is the worked example -- the
transport is one vertical stack, while virtio-blk reaching
`block_device` and a later virtio-gpu reaching `display_driver` is a
second, orthogonal one, and a single diagram of either alone is
misleading about where the next device goes.

Never write personal information (PII) into any file being edited or
added. If a change genuinely seems to need some, ask first, or
anonymize it and say so plainly.

Any genuinely reusable tooling built or used during a session (a
helper script, a test harness) belongs in `tools/`, not left as a
scratch/one-off -- see `## tools/` above for the bar ("does this fix a
rederive-from-scratch cost"). Update the files that describe `tools/`
(this file at minimum) to match when something's added there.

The files are already on the real checkout -- there is nothing to
"deliver". Commit with plain `git`, and push verified work.
