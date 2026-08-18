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
disk-backed filesystems (TFS3 the default, TFS2 kept as a
probe-selected second backend). No cross-compiler needed -- host and target are
both x86-64, so plain system `gcc`/`ld`/`nasm` with freestanding flags
work.

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
  be a reusable `apps/ui/` widget instead of a one-off (see this
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

- **`kapi.h` is the one header apps include** for kernel capabilities
  (console, keyboard, mouse, timer/RTC, filesystem, graphics). Never
  `#include` a driver header directly from `apps/`, never call
  `inb`/`outb` from app code. If a capability isn't in `kapi.h` yet,
  that's a sign it belongs behind a new function in `kernel/core` or
  `kernel/drivers`, exposed through `kapi.h` -- not a reason to reach
  around the boundary.
- **There is a shared toolkit in `kernel/lib/` -- check it before
  hand-rolling a digit loop, a formatter, a path join, or a
  rasteriser.** Seven headers, all reachable through `kapi.h` and all
  with KTESTs. The four original ones:
  `string.h` (strings/memory/char classes), `knum.h` (numbers <->
  strings: `k_utoa`/`k_itoa`/`k_htoa`, `k_parse_u32`/`k_parse_hex`,
  ...), `kfmt.h` (`k_snprintf`, plus `vga_printf`/`klog_printf` for a
  whole line in one call -- one header but TWO files, since `kfmt.c` is
  freestanding and shared with ring 3 while the two kernel sinks live in
  `kfmt_print.c`; a kernel include in the former silently takes
  `snprintf` away from userland), `kpath.h` (`k_path_join`/`_normalize`/
  `_resolve`/`_basename`/`_dirname`). Plus two for drawing:
  `fixed.h` (Q16.16 fixed point and trig -- **angles are in TURNS, not
  radians**, so `FX_ONE` is a full rotation and `fx_sin(FX_ONE/4)` is
  exactly 1; there is no floating point in this kernel, the build
  passes `-mno-sse`) and `geom.h` (`geom_line`/`_polyline`/`_ellipse`/
  `_circle`/`_fill_ellipse`/`_rotate`/`_transform`, each taking a
  `GEOM_ALIASED` or `GEOM_AA` flag; **plus a small 3D section** --
  `geom_pt3`, `geom_rotate3` (yaw then pitch then roll, in that fixed
  order because rotations don't commute), `geom_project` (perspective,
  eye at `-dist`, denominator clamped so a point at the eye can't divide
  by zero or flip through the origin) and `geom_transform3`, which
  scales/rotates/projects a model and hands back each vertex's ROTATED
  depth so a caller can shade or sort by distance. Deliberately not a 3D
  engine: no matrices, no faces, no depth buffer, no clipping planes --
  a model is points plus whatever edge list the caller keeps beside
  them, which is all the Shapes demo's cube is). And one for
  And one for SELECTION: `rubberband.h` (`rb_begin`/`rb_motion`/
  `rb_end`, a fixed-bitset selection set, and RB_REPLACE/ADD/TOGGLE) --
  the drag-a-rectangle-to-select behaviour, owning the band, the
  selection and the modifier rules while the caller supplies item
  geometry through a `struct rb_ops` and does its own drawing. Shared
  source compiled twice, like `geom.c`, so the kernel desktop and a
  future ring-3 file manager cannot diverge; the spec is
  `docs/gui-guidelines.md`'s "Rubber-band selection" section. And one
  for unpredictability: `krandom.h` (`krandom_u64`/`krandom_bytes`,
  RDSEED/RDRAND with a TSC-jitter fallback) -- **deliberately NOT a
  CSPRNG, and `krandom_quality()` is how a caller finds that out**
  instead of assuming. The stack canary is randomized from it at boot,
  which is the one place here where changing a global at the wrong
  moment makes innocent code panic: read
  `kernel/lib/stack_protector.c`'s comment before moving that call.
  Reach for the `gfx_draw_line()`/
  `gfx_draw_circle()`/`gfx_fill_ellipse()` wrappers in the kernel and
  `uui_canvas` in ring 3 rather than `geom_*` directly -- both handle
  the plot callback, and the canvas handles clipping. Two things about
  `geom.h` worth knowing before extending it: it draws through a
  **callback**, never into a framebuffer (which is what lets the same
  code serve the kernel, a ring-3 app and a test with no display at
  all), and it is compiled TWICE from one source, once for the kernel
  and once for userland -- so it must not reference anything
  kernel-only. See `docs/decisions.md` for all three.
  This exists because a survey
  found the same twenty lines written nine times for int->string, ten
  for hex, six for parsing and three for path resolution -- and the
  path one wasn't just duplication, the copies disagreed (`edit
  ../x` meant different things in the GUI Terminal and the physical
  shell). Two conventions everything there follows, worth matching in
  anything added to it: a formatter that doesn't fit its buffer writes
  NOTHING rather than a truncated (i.e. wrong) value, and a parser
  REJECTS rather than guesses. Adding to the toolkit follows this
  file's usual bar -- a second real caller, not a plausible one; the
  batch that introduced it had `k_strstr`/`k_strcasecmp`/`k_toupper`
  written and building, found no caller for them, and deleted them
  again before landing. (All three have since come back, each once a
  real caller turned up -- `k_strstr` for the shell's `Ctrl-R` history
  search, the case-folding pair for `timezone Helsinki`'s lookup, which
  is also why that folding is ASCII-only; see `docs/decisions.md`.
  That's the rule working, not an argument against it.)
- **A widget's `ops->hit` is a BOOLEAN, and a widget whose own
  `_hit()` returns a ROW INDEX must convert it.** The router tests it as
  `!it->ops->hit(...)` (`userland/ui/uui_route.c`), so returning the
  index makes ROW 0 -- the one row whose index is falsey -- report "not
  hit". The first row then silently cannot be clicked while every other
  row works, on a widget that draws, scrolls and hovers perfectly.
  `uui_listbox` shipped that way and nothing noticed until `uui_table`
  reproduced it by copying the line; `uui_radio_list` had it right all
  along. Write `>= 0`.
- **A widget's `natural_size` must not depend on where the widget
  currently IS.** `uui_button_group_natural_size()` measured its
  buttons' far edge from the ORIGIN, which equals the union's extent
  only while the group sits at (0,0) -- true until something moved one.
  Placed at y=284 it reported a natural height of 312 (its offset plus
  its size), which inflated what the layout believed its children needed
  and left Task Manager's table growing 16 px against a 300 px resize.
  Natural size is the size a widget WANTS, asked before anyone knows
  where it goes; anything else is a feedback loop between layout and
  measurement.
- **A lone `uui_button` routes its own clicks now** -- `press`/`motion`/
  `release` are on `uui_button_ops`, as on QPushButton and a Win32
  BUTTON. It used to be draw-and-hit only, so a single declared button
  drew perfectly and ignored every click, which is a silent trap.
  `uui_button_group` still routes its own buttons and is worth keeping
  only for a GRID of them (Calculator's keypad); `docs/roadmap.md` has
  retiring it as an item.
- **A layout CAN grow a child along its stacking axis.** `UUI_FILL_H` in
  a column (and `UUI_FILL_W` in a row) absorbs the leftover space,
  flexbox's flex-grow. Before that nothing in a column could get taller
  and a resized window just grew empty space under the last child.
- **Anything drawn follows `docs/gui-guidelines.md`.** Three things
  bite most often. (1) **`gfx_draw_string()` does not clip** -- use
  `gfx_draw_string_clipped()` and `gfx_text_width()` for anything in a
  fixed box; this caused the identical overlap bug in two different
  files, the second one written days after the first was documented.
  (2) **`on_click` fires on button-DOWN despite its name**, so a
  control that commits there can never be cancelled -- arm in
  `on_press`, act in `on_release`, which is what the title bar has
  always done. (The menu bar is the documented exception: a menu OPENS
  on press, as it does on every real desktop, and its items still commit
  on release.)
  (2b) **Esc closes nothing; Alt+F4 closes a window, and the WM handles
  it** -- it never reaches the app, the way it doesn't on Windows or in
  KDE. All three user-facing closes (the X, the context menu's Close,
  Alt+F4) go through one `wm_request_close()`, which ASKS a ring-3
  client and can be refused from `uapp_desc.on_close`. Route a fourth
  one through the same function rather than repeating the client check;
  repeating it is exactly how the context menu drifted into seizing a
  window instead of asking for it. (3) **`gfx_set_clip_rect()` with a non-positive w/h sets an EMPTY
  clip -- nothing draws -- and only `gfx_clear_clip_rect()` removes a
  clip**; conflating the two once handed an app's whole `on_draw()` an
  unclipped screen (the damage sweep's long-standing "20px"
  violation). (4) **Layout is FONT-DERIVED, never in fixed pixels** --
  window sizes come from each app's `default_size()` at open time,
  chrome from `gfx_char_h()`, the desktop's column pitch from
  `gfx_char_w()`. That is what makes the default font size
  (`kernel/drivers/gfx.c`, 14pt since 2026-08-14, down from 18) a
  genuine one-line change: the whole UI reflows and all 82 GUI
  regression checks pass unchanged. What does NOT reflow is a hardcoded
  pixel constant in a test tool -- prefer `DebugConsole.menu_row(label)`
  over `gui_flow.py`'s calibrated numbers, which have needed
  re-measuring three times now.
  (5) **An app cannot draw outside its own window, and that is
  enforced** -- the WM clips to the content area around every
  `on_draw()` (`clip_to_window_content()`), and a ring-3 client draws
  into its own buffer with no mapping of anything else. The explicit
  opt-out is `gfx_clear_clip_rect()`, which lasts only for that paint.
  UI Demo overdraws on purpose every frame and `uidemo_test.py` asserts
  the marker colour never reaches the screen, so the boundary is tested
  rather than assumed.
  (6) **Interaction states come from `enum ui_state` /
  `ui_state_bg()`**, which derives hover/pressed from the control's own
  colour; don't hand-pick tints, and don't assume hover means "lighter"
  (on this near-white theme it has to darken -- `gfx_luminance()`
  decides).
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
  form is this file's existing rule: ask what a broken version would
  still pass.
- **A positive control can turn nothing red because the test's DATA
  never reached the code under test.** The truncate tests wrote 16 KB,
  which fits TFS3's twelve DIRECT pointers, so disabling the
  indirect-table handling outright changed no result -- the tests were
  green, thorough-looking, and blind to that whole path. The general
  form: when a control fires nothing, suspect the fixture before the
  harness, and ask what input size/shape actually reaches the branch.
  This is the same family as the three GUI ways below, and the fix was
  the same -- a case whose input crosses the boundary (20 blocks
  truncated to 15, straddling a pointer table).
- **A screendump compared against another screendump must be a SETTLED
  frame** -- `QMPSession.stable_pixels()` (two identical consecutive
  reads). A client that has drawn into its buffer, and even logged that
  it did, has not necessarily been composited yet; with the WM in ring 3
  that is an extra process hop whose timing varies with load, so a
  capture taken too early fails a comparison that is otherwise correct.
  Measured on `calculator_client_test.py`: 2 runs in 6 passed before,
  8 in 8 after, with a probe showing EVERY capture needing a retry. Do
  not use it on a window you expect to animate. And the sibling trap
  from the same hunt: **a poll whose exit condition is weaker than what
  the code after it needs is a flake** -- that tool waited for the first
  of N layout lines and then required all N.
- **Verify GUI changes by reading PIXEL VALUES, not by looking at the
  screenshot** (`tools/pixel_probe.py`). A hover state that moved the
  background by two units out of 255 looked perfectly plausible in a
  PNG and was invisible in practice; the number is what caught it.
  Always sample a control that should NOT have changed as well -- half
  the assertion is the neighbour staying put.
- **Line editing is `kernel/lib/klineedit.c`'s, in both front ends.**
  The physical shell and the GUI Terminal share one readline-style
  editor (buffer/cursor/kill ring/undo/keymap); each front end only
  paints the result. Don't add an editing key to one of them -- add it
  to the core's keymap and both get it. Ctrl/Alt reach apps as control
  codes and an ESC prefix, terminal-style, NOT as `KEY_*` codes (see
  `keyboard.h`'s "Ctrl and Alt" comment and `docs/decisions.md`).
- **`userland/wm/wm.h` is a second, peer-level boundary**, not part of
  `kapi.h` -- it's the GUI-specific equivalent, included by GUI apps
  for `window_*` helpers. `kapi.h` never includes `wm/wm.h` or
  `gui_apps.h`.
- **`apps/ui/` IS DOWN TO ONE WIDGET, and the GUI toolkit is
  `userland/ui/`.** M41's stage 0 deleted the checkbox, dropdown,
  listbox and text view; stage 4c deleted everything the WM drew with
  (buttons, primitives, focus ring, scrollbar, textbox, radio list, icon
  grid) along with `userland/wm/` itself. What is left is
  `ui_scrollback.{c,h}` -- `struct text_scrollback`, which the KERNEL's
  own `edit` command draws with (`apps/editor.c`) and which therefore
  cannot move to ring 3. **A new widget goes in `userland/ui/`. There is
  no longer any such thing as a kernel-side one.** `apps/theme.h`
  survives for the same kind of reason: `apps/completion.c` colours the
  shell's tab-completion with it.
  The rules those widgets taught still apply to their ring-3 twins and
  are documented there: draw a popup LAST (drawing is immediate-mode, so
  z-order is call order), and **route keys through the focus ring, never
  by trying each widget in turn** -- the first one tried swallows every
  key it recognises, which left a listbox next to a dropdown unreachable
  from the keyboard. So does the standing rule that **a widget's
  BEHAVIOUR belongs to it, not to the app**: input handling, hit-testing
  and geometry live in the widget, and an app configures and forwards
  rather than reimplementing. See `docs/gui-guidelines.md`'s "Behaviour
  belongs to the component" for the escape hatch, and
  `docs/decisions.md` for why `apps/widgets.h`/`.c`, the single file all
  of this was split out of, no longer exists either.
- **`userland/` is split by ROLE, and the build derives things from it
  -- adding a program is a `.c` file and nothing else.** `rt/` (crt0,
  libsys, stack_chk, link.ld), `ui/` (the GUI toolkit -- ugfx, utheme,
  utext, the widgets; mirrors `apps/ui/` so a widget's kernel-side and
  ring-3 versions sit at the same relative path), `lib/` (userland
  libraries that aren't UI -- `tosh`, the shell the ring-3 Terminal
  links against, plus `string.h`/`stdio.h`/`cmem.c`, the C names over
  the toolkit; see the bullet below), `gui/` (windowed apps), `bin/` (command-line
  programs), `tests/` (single-mechanism diagnostics). **The first three
  produce objects; the last three produce one ELF per `.c`, and the
  directory also says where it seeds** -- `gui/` and `bin/` to `/bin`,
  `tests/` to `/tests`, which is `docs/filesystem-layout.md`'s
  distinction stated once instead of restated as a Makefile list that
  could drift from it. Only three programs' on-disk names differ from
  their file names (`SEED_NAME_*` in the Makefile: terminal->uterm,
  gfxdemo->shapes, echo->echo_test). Note `tests/` holds windowed
  diagnostics too (`winclient`, `uiclient`) -- the directories name a
  DESTINATION, and those two exercise the windowing protocol rather
  than being programs a user wants offered. **Includes are
  path-qualified** (`#include "ui/ugfx.h"`, `#include "rt/sys.h"`) off
  a single `-Iuserland`, so an include line says which layer it reaches
  into. ELFs build to `build/userland/**`, not into the source tree.
- **In ring 3 the toolkit is reachable under the C names -- don't
  hand-roll a `my_strlen` or a digit loop there either.**
  `#include "lib/string.h"` for `strlen`/`strcmp`/`strlcpy`/`mem*`/the
  `ctype` handful, `#include "lib/stdio.h"` for `snprintf`. These are
  NOT a second implementation: they are the same `k_*` code, compiled a
  second time into `libuapp.a` by the Makefile's shared-source rule, so
  a ring-3 `strlen` and the kernel's `k_strlen` cannot diverge. The
  kernel keeps the `k_` prefix and these headers are not on its include
  path. Reach for `knum.h`'s `k_utoa`/`k_htoa` directly when you need a
  fixed-width number -- kfmt's printf has zero-pad widths for numbers
  and `%Ns`/`%-Ns` column padding for STRINGS (a value longer than its
  field pushes the column rather than being truncated), but no `*`
  width. What does NOT exist yet, on
  purpose: `malloc`, `FILE`, `printf`, `errno`, TLS (Milestone 24).
  Three traps are documented in `docs/decisions.md` and in the files
  themselves, all of which fail quietly: a header named `string.h`
  including `"string.h"` finds ITSELF (hence the `<>`), an archive
  member cannot be named `string.o` twice (hence `cmem.c`), and
  `USERLAND_CFLAGS`'s `-fno-tree-loop-distribute-patterns` is what stops
  a real `memcpy` recursing into itself through `k_memcpy` -- it LINKS
  and blows the stack at runtime. Adding to these headers follows the
  usual bar: a second real caller.
- **Every ring-3 program is just a `main()`.** `userland/rt/crt0.asm`
  provides `_start` (reads argc/argv off the stack per SysV, calls
  `main`, passes its return to `sys_exit`) and `userland/rt/sys.c` is
  libsys -- one typed wrapper per syscall. **Never hand-roll an
  `int $0x80` stub in a new program**; that duplication across twenty
  files is exactly what libsys replaced. **A program names no other
  objects either** -- `build/userland/libuapp.a` (the toolkit,
  `userland/lib/`, and the sources shared with the kernel) is linked
  into every ELF with `--gc-sections`, so each binary gets exactly the
  members it references and nothing else; `hello` pulls no toolkit code
  at all. Adding a GUI app is a `.c` file in `userland/gui/` with no
  Makefile edit. Two things this depends on, both easy to break:
  `userland/rt/link.ld` must match `.text.*` (function-sections put
  every function in its own section, and a script matching only
  `.text` links an empty program that faults at its entry point), and
  the archive must come LAST on the link line. And the archive rule
  **deletes `libuapp.a` before rebuilding it**: `ar rcs` updates an
  existing archive and never removes a member whose source file is
  gone, so a deleted or renamed `.c` leaves its object inside forever.
  That does not fail loudly -- a linker pulls the first member
  satisfying a symbol and only errors when two members it already
  pulled collide -- so the build quietly links the deleted file's code
  until the two versions finally differ. It happened here: splitting
  `uwidgets.c` left `uwidgets.o` in the archive for three commits. `sys_call()` is the raw escape
  hatch and is for the `/tests` diagnostics that poke the raw ABI on
  purpose, not for ordinary code. Two things to know before touching
  `crt0.asm`: the entry ABI is the STANDARD SysV stack layout (argc at
  `(%rsp)`), and `%rsp` must be **16-aligned before `call main`** -- a
  `sub rsp, 8` there looks like it restores the old convention and
  instead faults every SSE-using binary while leaving plain ones
  working, see `docs/decisions.md`.
- **Ring-3 GUI apps are written against Toykit's `uapp`, and a new one
  is a `.c` file in `userland/gui/` with NO Makefile edit.** Describe
  the app in a `struct uapp_desc` -- title, a `uui_layout`, callbacks --
  and `uapp_run()` owns the TWP handshake and the event loop
  (`userland/ui/uapp.h`). Every callback is optional with a library
  default, which is what lets TWS gain a feature without apps being
  edited. Do not hand-roll a window handshake or an event loop in a new
  client; that is what this replaced. Layout (`uui_layout.h`) means an
  app writes no coordinates: declare a column/row/grid, and the window
  sizes itself from the content. Resize, focus and wheel all arrive for
  free. `docs/uapp-design.md` is the full design and its staging.
- **Monotonic time is an INTERFACE, and wall clock is not one of its
  implementations.** `kernel/clocksource.h` -- sources register like
  `display_driver`s, best rating wins (PIT 110, TSC 300), and the core
  converts a raw counter with a mult/shift pair so the overflow
  reasoning lives in one audited place. `rtc_read_local()` stays out of
  it: "what time is it" jumps when the clock is set and says nothing
  about elapsed time, which is why Linux separates clocksource from RTC
  too. **The TSC needs an INVARIANT TSC** (CPUID 8000_0007H EDX bit 8),
  or its rate changes as the CPU throttles and every duration is
  silently wrong. **Reaching that path is the trap**: plain TCG cannot
  (`-cpu max,+invtsc` warns "TCG doesn't support requested feature") and
  KVM withholds it even under `-cpu host`, so the ONLY way is
  `python3 tools/vm.py --kvm --cpu host,+invtsc`. `notsc` on the GRUB
  line forces the PIT back, so the coarse path stays reachable on
  hardware where the TSC wins -- same rule as `nopat`/`ata nodma`.
- **An app with a cadence sets `tick_ms` and BLOCKS between frames.**
  `uapp_desc.tick_ms` arms a TWS timer (`WIN_REQ_TIMER` ->
  `WIN_EV_TIMER`), so `on_tick` arrives as an event instead of the loop
  spinning: Task Manager asks for 500ms, Shapes for 10ms. Leaving it 0
  keeps the old polling loop, which is what makes this additive -- but
  polling wakes a process 100 times a second whatever it actually
  wanted. Three rules: the interval is in MILLISECONDS (the tick rate
  is the kernel's business) and is floored at one tick, never zero; the
  next firing is computed from NOW so a slow client never accumulates a
  backlog of overdue firings; and there is ONE timer per window. See
  `docs/decisions.md`.
- **An app refuses its OWN second copy -- the launcher never does.** A
  `uapp_desc` with an `app_id` and `UAPP_SINGLE_INSTANCE` sends
  `WIN_REQ_ACTIVATE` before creating anything: TWS raises the window
  already carrying that id and the second copy exits 0 without ever
  appearing. Task Manager and About opt in; everything else is unchanged
  and opens as many copies as it is asked to. Three things to know. The
  id rides `WIN_REQ_CREATE`'s previously-unused `text` field so a window
  can never exist without it (a later "register my id" message leaves a
  gap exactly long enough for a second copy to miss its twin). It is an
  opaque token -- `"taskmgr"`, not a path and not the title. And **it is
  not a lock**: two launches in the same instant can both be told
  "nobody there", which is recorded rather than fixed because every
  launch path here is a human clicking a menu. See `docs/decisions.md`.
- **`uui_table` sorts on a header click, and an app supplies only a
  COMPARATOR.** `uui_table_set_compare()` + `uui_table_set_sort()`; the
  widget owns the ordering (an `int order[]` permutation), the clickable
  header, the arrow and the toggle-to-reverse rule, exactly as Win32's
  `ListView_SortItems` and Qt's `lessThan` split it. **It cannot sort
  the text it draws** -- cells are formatted strings, so "10" would come
  before "9" and "4 KB" against "1 MB" is meaningless; comparison has to
  be on the app's real values. Every public row index on the widget is
  an APP row, not a screen position, so a selection survives a re-sort
  and an app that sorts is otherwise unchanged. Two traps it exposed:
  a widget's `ops->hit` must cover the WHOLE widget (routing on the
  row-only hit meant header and scrollbar presses reached nothing), and
  anything comparing `selected` against `top` is mixing an app row with
  a view offset. See `docs/decisions.md`.
- **`uui_table` is the multi-column widget** (`userland/ui/uui_table.h`)
  -- columns with per-column width (in CHARACTERS, or 0 to stretch) and
  alignment, a header, selection, scrolling. **It PULLS its rows through
  a callback and stores none of them**: Toykit has no allocator, and
  Task Manager re-reads the process table several times a second, so
  there is nothing cached to go stale. Sizing is derived, so a resizable
  window reflows with no arithmetic in the app. See `docs/decisions.md`.
- **Editable text has ONE implementation of what editing means**
  (`userland/ui/uui_edit.h`): the caret, the selection and the keymap --
  Ctrl+A, Shift+arrows, typing replaces the selection, Backspace and
  Delete remove it -- with STORAGE delegated through four accessors, so
  the single-line `uui_textbox` and the multi-line `utext` share
  behaviour without sharing a buffer. Same split as
  `kernel/lib/klineedit.c` kernel-side. Don't add a keymap to a widget:
  add the accessors and call `uui_edit_key()`. It deliberately declines
  Enter (a field commits, a document inserts a newline) and declines
  Up/Down unless the caller supplies line accessors.
- **A ring-3 app does NOT route mouse input to its widgets -- the
  toolkit does** (`userland/ui/uui_route.h`). Declare
  `uapp_desc.widgets` (a `struct uui_item[]`, each with an app-chosen
  `id`) and the library hit-tests them, delivers
  press/motion/release/wheel, and holds a **pointer GRAB** so a drag
  keeps reaching the widget that started it. The app gets
  `on_widget(a, id, reason)` and reads the new value from the widget
  (`uui_dropdown_selected()`, `cb.checked`, `list.selected`). **Do not
  hand-dispatch input in a new app** -- that is what UI Demo's
  twenty-one forwarding calls became, and a widget an app forgets to
  forward is not an error: it draws perfectly and does nothing, which
  is how `uui_listbox` shipped an undraggable scrollbar. Two things to
  know: a widget with a popup declares `overlay_active` so it is
  offered presses before anything is hit-tested (input order is the
  reverse of draw order), and **the wheel goes to the widget under the
  cursor**, so a test has to park the REAL cursor first
  (`DebugConsole.warp_cursor()`; `gui move` lasts one WM iteration).
- **The toolkit DRAWS the declared widgets too, popups last.** A widget
  owns its colours (defaulted from the theme at init), exports a `draw`
  slot, and `uapp` paints every item in `uapp_desc.widgets` before
  calling `on_draw` -- which an app needs only for painting the toolkit
  has no widget for. `uui_item.hidden` removes a widget from BOTH the
  picture and hit-testing, which is how an app shows and hides a
  control. Note `struct uui_item` is initialised with DESIGNATED
  initialisers (`.ops`, `.widget`, `.id`): positional ones silently
  re-bind when a field is added, and adding `hidden` did exactly that
  -- every widget's id landed in `hidden` and the compiler's
  missing-initializer warning was the only thing that noticed.
- **The GUI stack has names -- use them.** **TWP** (Toy Window
  Protocol, `abi/win_proto.h`) is the client<->server contract;
  **TWS** (Toy Window Server, `kernel/proc/win_server.c` +
  `userland/wm/wm_client.c`) implements it; **Toykit** (`userland/ui/`) is
  the client toolkit an app programs against -- roughly Wayland, its
  compositor, and GTK. Three names rather than one because the protocol
  is meant to outlive this server (M41 moves TWS to ring 3). Symbol
  prefixes are unchanged and stay that way (`uui_`, `ugfx_`, `uapp_`,
  `WIN_REQ_*`); a toolkit's name and its prefix need not match. See
  `docs/decisions.md`. **How a TWP message is CARRIED is now its own
  seam** -- `struct win_transport`
  (`kernel/include/kernel/win_transport.h`), with `SYS_WIN_REQUEST` as
  one implementation rather than the only path. Two things follow. The
  `gui` debug commands are protocol messages now
  (`WIN_REQ_DEBUG_CMD`/`WIN_EV_DEBUG_OUT`), so `debug_console.c` does
  NOT call into `userland/wm/` any more -- add a new `gui` subcommand in
  `wm_debug.c` as before, but write its output through its `struct
  dbg_out` sink, never `klog_write()` (a stray klog call still reaches
  the serial port, so it silently vanishes from the reply). And the
  seam has exactly ONE implementation, which by this repo's own
  unreachable-path rule means it is UNVALIDATED -- see
  `docs/decisions.md` for what is most likely wrong with it before
  designing stage 4 around it. **The WM itself is still ring 0, and
  `docs/wm-ring3-design.md` is the staged plan for moving it** -- STAGES
  0-3 ARE DONE (stage 3 landed 2026-08-16: the `gui` commands travel as
  protocol messages over `struct win_transport`, so the test tooling has
  already crossed the boundary the WM still has to). Stage 4 is the WM
  itself; of its prerequisites the ELF hardening and the tick/process
  syscalls are done (and monotonic time went further than the plan asked
  -- `SYS_MONOTONIC_NS` over a clocksource registry), the settings
  syscalls are done too (and went further than the plan asked -- a
  SETTINGS REGISTRY behind `SYS_SETTING`, not raw `etc_config` access),
  and the ring-3 allocator
  turned out NOT to be a blocker: `userland/wm/` allocates nothing, and the
  claim that it did was a comment pointing at a since-deleted file.
  **Every prerequisite is now complete, and stage 4's own REQUIREMENTS
  are written up as R1-R9** (2026-08-17), measured from `userland/wm/`'s
  call surface rather than estimated. **Stage 4a is DONE: R1, R4 and R5
  are BUILT and R3 was REMOVED** -- the hardware cursor is switched off
  on the only driver that has one (`vmsvga`'s `g_cursor_enabled = 0`,
  because a hw cursor over a relative PS/2 mouse makes the pointer
  jump), so it is unreachable on every configuration this OS boots and
  moved to M27a with virtio-input. **`tools/vm.py --vga vmware` is how
  you reach the modesetting driver at all** -- the default `std`
  adapter has neither modesetting nor a cursor plane, same shape as
  `--cpu max` for SMEP/SMAP. Read that before touching
  anything in `userland/wm/` with the migration in
  mind.
- **The kernel's idle work has ONE owner: `scheduler_idle()`**
  (`api/scheduler.h`). Any loop that is waiting rather than working
  calls it -- the physical shell's key wait, `wm.c`'s event loop, a
  long `cat`, the demo's timer. What it owns today is
  `debug_console_poll()`, and the reason it exists is that the serial
  debug console had no owner at all: it was polled by whichever loop
  happened to be running, and the WM's copy is the load-bearing one,
  because all 20 GUI tools and their 280 checks arrive over that
  console. **Don't add a bare `debug_console_poll()` to a new waiting
  loop** -- call `scheduler_idle()`, so the WM's move to ring 3 deletes
  a call rather than the capability. Two things it deliberately does
  NOT do: run from the timer tick (a dispatched command can be `sh cat
  big`, which blocks on the filesystem; nothing is lost waiting for a
  normal context, since COM1's receive is already interrupt-driven into
  a ring buffer), and touch `vga_cursor_tick()`/`vga_present()` (console
  upkeep belongs to whoever owns the screen, and the desktop owns it
  while it is up). The poll is NOT re-entrant and refuses a nested
  call: `dbg_dispatch()`'s `arg` points into `line_buf`, so a command
  typed during a long `sh` used to overwrite the running one's
  arguments. See `docs/decisions.md`.
- **`ugfx` has a SCREEN surface now, and it is the compositor's**
  (`struct ugfx_screen`, M41 stage 4b). `ugfx_screen_init()` takes R1's
  framebuffer grant and allocates a matching back buffer from sbrk;
  `ugfx_screen_present()` copies out only the damaged box and publishes
  it. Three rules ride with it. **Never read the mapped framebuffer** --
  it is write-combining, where a read is a full uncached round trip, so
  a compositor composites in the back buffer and copies OUT. **Present
  is required, not advisory** (a driver may declare
  `DISPLAY_CAP_NEEDS_FLUSH`). And **there is no free**: the back buffer
  and the verify scratch come from sbrk, which only grows, so a screen
  is initialised once per process and `ugfx_verify_release()`
  deliberately keeps its memory. The surface also gained a **clip rect
  and a damage box** shared with ordinary window surfaces -- same
  contract as the kernel's, including that a non-positive w/h is an
  EMPTY clip rather than an absent one.
- **The registered compositor can be GRANTED the real framebuffer**
  (`WIN_REQ_FB_MAP` / `WIN_REQ_FB_PRESENT`, owned by
  `kernel/proc/win_surface.c`). Writable and WRITE-COMBINING at
  `WIN_FB_VADDR`, refused to anyone but the compositor, and revoked
  wherever the role is cleared -- one place, so deregistration, a kill
  and a fault are the same path. Three things to know. **The memory
  type must reach the USER PTE** (`vmm_map_user_page_type()`,
  `VMM_MT_WC`): the kernel's identity map and the compositor's mapping
  are separate PTEs, and `paging_set_write_combining()` only touches
  the former, so without this a ring-3 compositor gets a CACHED
  framebuffer -- the bug class TCG cannot show you. **Present is
  required, not advisory**: `vmsvga` declares
  `DISPLAY_CAP_NEEDS_FLUSH`, where written pixels are invisible until
  the driver is told. And **a ring-3 write is TRANSIENT while the WM is
  still ring 0** -- it survives until the WM's next frame, so a test
  that looks for it in a screenshot fails against a working kernel;
  assert the mapping is the real screen by comparing a client read
  against a screendump of the same pixel. See `docs/decisions.md`.
- **THE FILESYSTEM IS NOT RE-ENTRANT, and `vfs.c` holds a preemption
  guard because of it.** `tfs3.c` walks directories, inodes and data
  through module-level scratch buffers (`g_blk`, `g_ptr_blk`); the
  kernel context is a scheduler participant and a ring-3 process is
  preemptible inside a syscall, so an app's file read interleaved with
  the WM's and overwrote the block the WM was parsing. It did not look
  like a filesystem bug: the WM reported files that plainly exist as
  missing, silently, on about one boot in three under KVM. `FS_OP()` in
  `vfs.c` wraps every backend call in
  `scheduler_preempt_disable()`/`_enable()` (api/scheduler.h), at the
  VFS because that is the one place every caller passes through. Three
  things to know: it does NOT make an `fs_list()` callback safe to call
  `fs_*` from (that is recursion, which a depth counter cannot see); it
  is NOT the nested-read refusal below, which protects one buffer during
  one call rather than the backend's state across the whole call; and an
  unbalanced `disable()` hangs the machine, which is why `_enable()`
  clamps at zero instead of going negative and silently disarming the
  next section. See `docs/decisions.md`.
- **Prefer `fs_read_into()` to `fs_read()` in anything the kernel
  context parses.** `fs_read()` hands back a pointer into a shared
  staging buffer, and that contract is unstatable in a preemptible
  kernel -- a caller can honour it perfectly and still lose the buffer
  to a ring-3 syscall mid-parse. `cursor_theme.c` did exactly that, with
  a comment reasoning the parse happens first (true of the function, not
  of the machine). `fs_read_into(path, buf, cap)` is `fs_size()` plus
  `fs_read_range()` into memory the caller owns, so there is no shared
  buffer to invalidate; it REFUSES an oversized file rather than
  truncating. The same shape exists for config files:
  `etc_config_load()` + `etc_config_buf_get()` read once and answer many
  keys, because `etc_config_get()` re-reads the whole file PER KEY --
  which made a nine-entry desktop reload 54 whole-file reads.
- **The disk has a WRITE-BACK CACHE, and its flush can fail**
  (`kernel/drivers/ata_cache.c`, under `ata_read_sectors()`/
  `ata_write_sectors()` rather than in the block layer -- TFS2 and
  `partition.c` bypass the block layer, and a bypass past a write-back
  cache is a silent correctness hole in both directions). The
  consequence that matters: a write that returned success can be refused
  LATER, at the flush, so `blk_flush()`, `ata_flush_now()` and the block
  device's flush op all RETURN A STATUS now and TFS3's `txn_commit()`
  checks it -- barrier 1 failing ABANDONS the transaction rather than
  overwriting targets. A failed write-back keeps its line dirty rather
  than dropping it. `sync` flushes and reports how many sectors it
  wrote; shutdown and reboot flush first. **Fault injection sits at the
  public entry AND on the write-back path**, because with a cache in
  front "the drive refused this write" no longer happens during the
  caller's `write()` at all.
- **`fs_read()` REFUSES a nested whole-file read**, returning NULL as it
  does for a missing file. Every backend frees one shared staging
  buffer, allocates a new one and does a BLOCKING read into it -- so the
  WM being preempted mid-read while a ring-3 syscall reads a file meant
  the second call freed the buffer the first was still writing into.
  Caught as a heap red-zone violation on a 96-byte block holding
  `ame=Calc`. **The cost to know**: a refusal looks exactly like "no such
  file" at the call site, so it is logged. See `docs/decisions.md`.
- **Setting a setting to the value it already has does NOTHING** --
  `setting_set()` compares the live value AND the file first, and skips
  the write and the generation bump. This is not micro-optimisation:
  everything watching `setting_generation()` does real work when it
  moves (the desktop re-reads every `.desktop` file), so a UI that
  over-reports a change turns into disk I/O and a desktop-wide reload.
- **`uui_radio_list` arms on press and COMMITS ON RELEASE**, restoring
  the previous row if the pointer left the list -- the rule
  `docs/gui-guidelines.md` states for every control. Two traps came out
  of adding it. The router only names a widget to the app when that
  widget HAS a `release` op, so a list without one is invisible to an
  app that acts on release; and `press` must return non-zero on ANY hit,
  because the router takes its pointer grab only when press does -- with
  0 for the already-selected row, that one row silently loses its
  release. **An app must honour `reason`**: Control Panel discarded it
  and applied a setting on every pointer-motion event, which froze the
  desktop for seconds and exposed the `fs_read()` bug above.
- **The WM has a SLOW-FRAME WATCHDOG** (`userland/wm/wm_watchdog.c`): it
  times each `wm_run()` iteration by phase and logs anything over a
  threshold (150ms by default) as
  `wm: SLOW FRAME 620 ms -- worst phase 'desktop_entries' 610 ms`. The
  design point worth preserving: it measures only the work AFTER the
  frame's `hlt`, so a SILENT watchdog during a visible freeze is a real
  answer -- the loop was not running, i.e. the stall is below us (host
  scheduling, the display backend, an emulator's fsync) -- rather than a
  missing measurement. `gui watchdog [<ms>|off]` tunes it and reports
  the fired/peak counters, which matter as much as the threshold: "no
  SLOW FRAME lines" is only evidence of a fast WM if it was armed. The
  attribution trap: `wmwd_phase()` names the phase ABOUT TO START, so
  the interval it closes belongs to the PREVIOUS one.
- **A panic NAMES THE FUNCTION now**, on screen and in the log:
  `in crash_gp_fault+0xa`, plus the faulting context (`pid 1` or
  `kernel context`), the general registers, the build id and the uptime.
  The symbol table is baked into the image by `tools/gen_syms.py` into
  its own `.ksyms` section -- the same two-pass trick `.krelocs` uses,
  and for the same reason: linker.ld places it after every address it
  records, so pass 1's addresses stay correct in pass 2, and `--verify`
  fails the build if that stops holding. The blob contains NO POINTERS
  (link-time addresses as u32 literals, names in a string table), so it
  adds nothing to the ~8,000 relocations the kernel patches at boot.
  The register dump earns its keep immediately: a #GP has no CR2, and
  the bad pointer is usually sitting in RAX.
- **There is a Crash Test app** (`userland/gui/demos/crashtest.c`,
  Start menu only -- no desktop icon on purpose). Ring-3 buttons fault
  in the app's own code and prove the desktop survives; Ring-0 buttons
  ask the KERNEL to panic and are **refused unless booted with
  `faultinject`**. The kernel owns the fault list
  (`api/crashtest.h`), so adding a kind there gives the app a button
  with no edit -- and `tools/crashtest_test.py` (9 checks) is safe in
  `gui_regress` precisely because the dangerous half is disarmed by
  default. To exercise a real panic: `make iso KCMDLINE="faultinject"`.
- **A kernel panic now prints enough to diagnose from a pasted log.**
  The relocation offset, the LINK-TIME RIP (the kernel relocates itself,
  so a raw RIP is meaningless on its own) and a stack scan, all to the
  SERIAL log -- the RIP line used to go to the screen only, which is why
  panics arrived as photographs. Paste the printed
  `addr2line -f -e build/kernel.bin 0x...` straight in. The backtrace is
  a STACK SCAN, not a frame-pointer walk (this kernel builds at -O2, so
  an RBP chain would be fiction): it overreports stale return addresses,
  so read it as candidates rather than a call chain. Ring 0 only -- a
  ring-3 RIP belongs to some userland ELF, and resolving it against the
  kernel image would be confidently wrong.
- **The cursor's shapes are DATA FILES, and a theme is a directory.**
  `/usr/share/cursors/<theme>/<shape>` (six shapes: `arrow`,
  `resize-h`, `resize-v`, `resize-diag`, `text`, `wait`), generated by
  `tools/gen_cursors.py` and loaded by `userland/wm/cursor_theme.c`. Two
  registered settings pick the theme and the size, so both get a
  Control Panel row and an `/etc/toyos.conf` key for free. Four things
  to know. **A shape file carries COVERAGE, not colour** -- an outline
  mask and a fill mask, coloured by the compositor -- so one shape set
  serves a light theme and a dark one; don't bake colours into a theme.
  **The built-in shapes are the floor**: a missing or malformed file
  costs its own shape, not the pointer -- which also means **a theme
  that loads NOTHING still draws a perfect pointer**, so never test this
  by checking that a cursor is on screen (the first version shipped
  loading 0 of 6 and looked right). **Scaling is integer
  nearest-neighbour** and the size is its own setting, not derived from
  `font_size`. And **the generator EXTRACTS the arrow from
  `wm_render.c`'s own arrays**, so re-run it after touching those or the
  shipped theme drifts from the fallback (`--check` fails on stale).
  Nothing here touches the kernel: the compositor draws the pointer into
  the framebuffer it owns, so it all moves to ring 3 with the WM. See
  `docs/decisions.md`.
- **The cursor's drawn extent is DERIVED, not a constant.**
  `cursor_rect()` (`userland/wm/wm_render.c`) is the one place that answers
  "what box does the pointer occupy", and the save/restore pair and the
  damage rect both ask it. A theme's size, its hotspot and the size
  setting all move that box, so the old fixed `CURSOR_BOX_SIZE` could
  not survive themes -- and the two consumers disagreeing is precisely
  the stale-sprite bug this file's comments record paying for twice.
  The previous box is STORED rather than recomputed, because the shape
  under the old position may not be the shape there now.
- **`etc_config.c` is SPLIT: the parser is shared, the file I/O is
  kernel-only.** `kernel/lib/etc_config.c` holds the `name=value`
  parser plus `etc_config_buf_get()`/`etc_config_buf_set()`, is
  freestanding, and is compiled a second time into `libuapp.a`;
  `etc_config_file.c` holds the four entry points that reach for
  `fs.h`. Same shape as `kfmt.c`/`kfmt_print.c` and for the same
  reason -- the ring-3 WM reads `.desktop` files and writes its own
  icon positions, and CLAUDE.md's own rule is that a second
  `name=value` parser drifts from the first, surfacing as the system
  and `config` disagreeing about a file. **If you add an entry point,
  ask which half it belongs in: does it look at a buffer, or at a
  file?** The rewrite loop that `etc_config_set()` and
  `etc_config_unset()` each carried a copy of is now one
  buffer-to-buffer function, which is what made it shareable.
- **Kernel stacks are 16 KiB, have a GUARD PAGE, and carry a CANARY**
  (`kernel/proc/scheduler.c`). They are their own page-aligned array,
  not a member of `struct sched_process`, so the page below each one can
  be unmapped -- Linux's `CONFIG_VMAP_STACK`. Three things ride with it.
  **The `#DF` gate runs on an IST** (`gdt.c`'s `df_stack`, `tss.ist[0]`,
  set in `idt_init()`): without it an overflow triple-faults and the
  machine reboots with nothing printed, because the push that would
  report the #PF is itself on the broken stack. **A canary at each stack
  base is checked on every switch**, covering the frame big enough to
  step OVER the guard. And **`-Wframe-larger-than=1024` is in CFLAGS**
  (2048 for `apps/`, which runs on the kernel context's stack, not a
  per-process one) -- it found `syscall_dispatch()`'s **4832-byte
  frame** the moment it existed, which turned out to be a single 4 KiB
  `SYS_GETRANDOM_MAX` buffer and is ~864 bytes now. **A frame is not the
  sum of what you can see** -- GCC overlaps disjoint locals and stops
  once an address escapes; `-fstack-usage` answers it in one command,
  and two rounds of reasoning about which struct was biggest answered
  it wrongly. The bug that caused all this: an 8 KiB
  stack overflowed on `SYS_SETTING` -> `etc_config` -> VFS -> TFS3 ->
  ATA and zeroed the NEXT SLOT'S saved trapframe, so the window manager
  `iretq`'d into CS=0. **Do not grow a kernel stack dynamically** -- no
  mainstream kernel does, and the reasoning is in `docs/decisions.md`.
  **There are TWO owners and they share `kernel/kstack.h`**: the
  scheduler's per-slot stacks and `process.c`'s LEGACY loader stack,
  which is the one a `run` or `config set` typed at the physical shell
  actually uses. Fixing only the first left `config set` double-faulting
  the kernel; the header exists so the next change cannot apply to one
  and not the other. **`kstack` at the shell reports all of it** --
  per-process high-water usage, the canary, what a slot would be resumed
  into, and (`kstack track on`) which syscall pushed the water line
  down. Reach for it BEFORE a crash: `config set` uses 8680 bytes.
- **A compositor's view of a dead window is POISONED, not unmapped**
  (`comp_poison()` in `kernel/proc/win_server.c`). The invariant: while
  a compositor is registered, a window buffer's slot in its address
  space is never a HOLE -- frames that go away are replaced by one
  shared read-only zero page. A compositor is a PROCESS: it learns a
  window died from a queued event and may blit the slot once more before
  it drains that, and a hole there is a page fault, i.e. the desktop
  dying (which is exactly what Force Quit did -- the WM triggers the
  teardown from inside its own `sys_kill()`). The frames really are
  freed, so this is not a use-after-free; the mapping that stays live
  IS one. **The trap: `vmm_map_user_page_type()` does NOT invalidate the
  TLB when it replaces a PRESENT entry**, so poison has to be unmapped
  (`comp_unpoison()`, from `comp_map()`) before real frames go over it,
  or a live compositor reads zeros from a window that draws perfectly.
  See `docs/decisions.md`.
- **A ring-3 compositor delivers events through TWP, not by calling the
  kernel.** `WIN_REQ_EVENT_PUSH` (put an event on a client's queue) and
  `WIN_REQ_EVENT_STATS` (queue depth), both **refused to anyone but the
  registered compositor** -- this is the one request that reaches across
  into another process's queue, and without that check any client could
  synthesise a keystroke into any other. `struct win_request_msg` has a
  `mods` field for it, mirroring `struct win_event`'s: `window` plus
  `a`-`d` is one field short of carrying an event.
- **`SYS_FS_GENERATION` is how ring 3 asks "has the filesystem
  changed?"** -- no arguments, the counter in RAX,
  `sys_fs_generation()` in libsys. Its own syscall rather than a
  `SYS_SYSINFO` field on purpose: the desktop polls it ONCE PER FRAME
  to decide whether to re-read `/usr/wm/desktop/`, and a free poll is
  the entire reason the counter exists instead of a directory scan.
  It says something changed, never what. Its load-bearing point: all 20 GUI test tools drive the WM
  through `wm_debug.c`'s `gui` commands over the KERNEL's serial
  console, so the 280 checks that prove the desktop works have to move
  with it, and that gets its own stage BEFORE the WM moves.
- **A ring-3 process can own a real window** (`userland/wm/wm_client.c` +
  `kernel/proc/win_server.c`, protocol in
  `kernel/include/abi/win_proto.h`). Two rules matter before touching
  it. **Every client operation is a typed MESSAGE carried by the one
  `SYS_WIN_REQUEST` syscall, never a syscall of its own** -- that is
  what keeps the boundary a protocol, so the window server can later
  move to ring 3 as a transport swap instead of a rewrite; see
  `docs/decisions.md`. And **the split is memory vs. presentation**:
  `win_server.c` owns ids/buffers/mappings/teardown (page tables and
  the frame allocator, which `apps/` can't reach), `wm_client.c` owns
  the window list, chrome, z-order and input routing, and they meet at
  a registered `struct win_server_ops` -- the same registry pattern as
  `display_driver`. **A client draws with `userland/ui/ugfx.c`**, not with
  syscalls -- there is no drawing syscall and there shouldn't be, since
  only the framebuffer is privileged, not drawing. The one thing a
  client can't produce for itself is the font, which
  `WIN_REQ_FONT` maps READ-ONLY out of the kernel's own tables rather
  than copying (one instance in memory, and client text can't drift
  from the desktop's when `font_size` changes). Widgets for a client
  live in **`userland/ui/uui.c`** (the ported `ui_button_group` and
  friends) with colours in `userland/ui/utheme.h` -- port more from
  `apps/ui/` only when a client actually needs them, the same bar
  `apps/ui/` holds itself to. Two of them have no kernel-side twin and
  were written here first: **`uui_menubar`** (a menu bar with submenus
  nested to any depth -- a menu is const arrays pointing at each other,
  since Toykit has no allocator, and per-item checked/disabled state is
  ASKED FOR through an `item_flags` hook rather than stored in the tree)
  and **`uui_statusbar`** (panes: a message that stretches, indicators
  that don't, widths in characters). Both are Notepad's. Three things
  to know before touching them: **the menu bar opens on PRESS**, the one
  deliberate bend in the commit-on-release rule (the item still commits
  on release -- see `docs/decisions.md`); **a popup is clamped to a
  bounds rect the app passes in**, which is the client's window today
  and becomes the screen when `WIN_REQ_POPUP` lands, so the flip/slide/
  clamp code is already the right code; and **there are no Alt+letter
  mnemonics on purpose** -- Alt is an ESC prefix here, so Alt-F is
  ambiguous with Esc, and `KEY_F10` focuses the bar instead. **A file needed by both the kernel and a
  client is COMPILED TWICE, never copied** (`build/userland/shared/`,
  see the Makefile): the two builds use different code models so the
  objects can't be shared, but the source can, which is why the ring-3
  and kernel Calculators cannot disagree about arithmetic. Only
  freestanding files qualify.
- **One process can run another and read its output**: `SYS_PIPE` +
  `SYS_SPAWN` + `SYS_WAITPID`, wrapped by libsys. `userland/lib/tosh.c` is a
  shell built on them and `userland/gui/terminal.c` the ring-3 Terminal
  around it. Two rules to know. **The retry sentinel is `SYS_RETRY`
  (-2), never 0** -- 0 is a real answer for `read` (EOF), and using it
  as "ask again" made a pipe read report end-of-file the instant its
  writer produced something; see `docs/decisions.md`. And **a client
  that spawns must close its own copy of the pipe's write end**, or the
  read never sees EOF even after the child exits, because a live writer
  (itself) still exists.
- **Per-process facts exist now, and Task Manager is a ring-3 app.**
  `struct sched_process` gained a name, `cpu_ticks` and (via
  `vmm_user_bytes()`) memory; `abi/proc_info.h` is what userland sees,
  reached by `SYS_PROC_INFO` (by SLOT, not pid -- an empty slot is a
  SUCCESSFUL report of pid 0, so enumeration skips rather than stops).
  `SYS_KILL` force-ends a process and `SYS_TICKS` is the MONOTONIC
  counter `cpu_ticks` is billed against -- a CPU percentage is a delta
  of one over a delta of the other, and `sys_gettime` (RTC wall-clock)
  cannot serve. `cpu_ns` is deliberately a TOTAL, not a percentage;
  see `docs/decisions.md`. **CPU time is MEASURED, not counted** -- the
  scheduler asks a clocksource how long each slice actually was
  (`bill_current()`), so the field is NANOSECONDS and `SYS_MONOTONIC_NS`
  is its denominator. It was `cpu_ticks`, incremented per timer
  interrupt, and that was wrong twice: billing from `SYS_YIELD` charged
  a whole tick for microseconds (every polling app read a fake 100%,
  several at once, which one CPU cannot do), and billing only from the
  timer made anything finishing inside a tick read 0%. **The invariant:
  every path that stops running the current process bills BEFORE
  changing `current_index`** -- missing the kernel-context case charged
  one process 9.51 seconds across a 300ms window.
  **`SYS_KILL` is unprivileged on purpose** --
  there is no user model to gate it on, and killing the ring-3 WM is
  stage 4's exit criterion rather than a hole.
- **`Exec=builtin:` is GONE, and ring 0 contains no applications.**
  Control Panel was the last one; it is `userland/gui/system/cpanel.c`
  now, and the builtin table, its lookup and its struct are deleted from
  `gui_apps.c`. An entry still naming that form is refused loudly rather
  than shown as a row that does nothing -- an entry file can outlive the
  mechanism it names. One live consequence: the WM's live-`.desktop`-
  reload deferral (`userland/wm/wm.c`) is now UNREACHABLE, because it
  triggers on a window holding a `gui_app_registry[]` pointer and only a
  kernel-space app ever held one. The guard is kept and correct;
  `desktop_entries_test.py` asserts the property that makes it
  unreachable, so a kernel-space app coming back turns that check red
  instead of producing a mystery rebinding bug.

  The historical note, for context:
- **`Exec=builtin:` used to have ONE user: Control Panel.** Task Manager
  moved to `userland/gui/system/taskmgr.c` in this round, so the
  registry's builtin table is down to a single row and disappears with
  it in stage 4. Two live consequences: an entry naming a builtin that
  no longer exists is REFUSED (loudly -- that is correct, and it broke
  `desktop_entries_test.py`'s fixture when taskmgr moved), and Control
  Panel is now the only lever for "a kernel-space app is open", which
  the WM's live-reload deferral is gated on.
- **The Start menu and desktop icons are built from FILES**, one
  `.desktop`-style entry per app in `/usr/wm/desktop/` (source of truth:
  `data/wm/desktop/`, format documented in its README). `gui_apps.c`
  scans that directory at desktop startup, so **adding an app to the
  desktop is dropping a file there**, not editing a table and
  rebuilding -- and it is picked up LIVE, no restart: the WM watches
  `fs_generation()` (one integer compare per frame, no I/O unless the
  filesystem actually changed) and re-reads the directory when it moves.
  **One directory feeds BOTH surfaces**, with `ShowIn=desktop startmenu`
  choosing which; a second directory per surface was rejected because an
  app wanted in both would have its file duplicated and the copies
  drift. Anything positional must go through
  `gui_app_visible_count()`/`_at()` -- the Start menu's rows are indexed
  by position, so filtering the draw while hit-testing the unfiltered
  registry lands every click on the wrong app and looks correct in a
  screenshot. `Exec=/bin/wm/apps/foo` spawns a binary;
  `Exec=builtin:taskmgr` names a kernel-space app's callbacks, and that
  form disappears when the last one moves to ring 3. Windowed binaries
  live under `/bin/wm/{system,apps,demos}/` -- the class is the SOURCE
  directory (`userland/gui/<class>/`) and the Makefile derives the
  destination, same rule that already made `userland/gui` mean `/bin`.
- **A Start-menu entry can launch a RING-3 program, not just a
  kernel-space app.** `apps/gui_apps.c`'s registry entries normally
  carry callbacks; one carrying `exec_path` instead (see
  `apps/gui_apps.h`) names a `/bin` binary, and `open_app()` spawns it
  rather than creating a window -- the process makes its own through
  the windowing protocol. Shapes, Calculator (ring 3), Notepad (ring 3)
  and Terminal (ring 3) are the current four. Two consequences worth
  knowing before adding one: a launcher always SPAWNS (it never focuses
  an existing window -- the WM can't enforce single-instance on a ring-3
  program, and shouldn't; the APP refuses a second copy of itself
  instead, see the single-instance bullet below), and **a real ring-3
  app is seeded to `/bin`,
  not `/tests`** -- see `docs/filesystem-layout.md`, and note that
  moving a seeded file needs an explicit delete since `sync` is additive.
- **There is no limit on open windows** -- `windows[]` is a grown-on-
  demand block (`wm_windows_reserve()`), not a fixed array, so the only
  ceiling is memory. Two rules follow from it MOVING when it grows:
  never hold a `struct window *` across anything that can open a window,
  and index rather than cache. (The z-order already renumbered indices,
  so the second half of that rule predates this.) `MAX_WINDOWS` is gone;
  `WM_WINDOWS_INITIAL` is a starting capacity.
- **Super/Win toggles the Start menu, and Alt+F4 closes a window** --
  both are WM shortcuts consumed before keys reach the focused window,
  so a full-screen app cannot swallow either. `KEY_SUPER` comes from the
  0xE0-prefixed 0x5B/0x5C scancodes; left and right send the same code.
- **A window may be dragged off the left/right/bottom edges and UNDER
  the taskbar**, keeping 8 character-widths of title bar grabbable and
  never above the top edge (every other edge is recoverable by dragging
  the title bar; the title bar cannot recover itself). A window that
  ends up unreachable anyway is pulled back by
  `wm_ensure_reachable()` when its taskbar button is clicked -- this
  desktop has no Alt+Space/Win+arrow escape, so that button is the only
  handle such a window has.
- **The window manager lives in `userland/wm/`** -- the core event
  loop/input/render split (`wm.c`/`wm_input.c`/`wm_render.c`, sharing
  state through `wm_internal.h`'s `extern`s) plus the pieces that grew
  their own files as they appeared: `desktop.c`, `start_menu.c`,
  `context_menu.c`, `confirm_dialog.c`, `file_picker.c`, `wm_tray.c`,
  `cursor_theme.c`,
  `wm_client.c`.
  Split by concern for readability -- it's still one tightly-coupled
  event loop, not decoupled components. See `userland/wm/wm.c`'s top
  comment.
- **Split a file once it's grown big enough to be genuinely harder to
  work with, the same call that produced the `userland/wm/` split above --
  don't wait for it to become unmanageable, but don't split
  preemptively either.** There's no hard line-count rule; the signal is
  practical: a file mixing more than one real concern (e.g. event
  handling + rendering, like `wm.c` before its split), or long enough
  that finding/editing the right part of it gets slow and error-prone.
  Don't calibrate this against a line count quoted in a doc -- those
  rot (this bullet claimed "every hand-written file is under 800 lines"
  well after `kernel/fs/tfs.c` and `apps/shell_sys.c` had both
  passed 1,000). Run `wc -l` on the actual tree if you want today's
  numbers. A hand-written file pushing toward a couple thousand lines
  is the point to seriously consider a split, not a hard
  trigger. This deliberately excludes *generated* data files like
  `kernel/drivers/font_ttf.c` (11,800+ lines of baked glyph data) --
  splitting those for line count alone would miss the point; the
  concern there is regenerating them correctly (`tools/genttf.py`), not
  readability. When a split does make sense: follow the `userland/wm/`
  pattern (split by concern, share state through a `_internal.h` of
  `extern`s if it's still fundamentally one component, not a real
  boundary -- see `docs/decisions.md`'s entry on this) rather than
  inventing a new pattern each time, and record the split's own
  reasoning in a top-of-file comment the way `userland/wm/wm.c` and
  `kernel/fs/tfs.c` do. (This used to be followed by the changelog's own
  splitting rule -- split by era every ~4,200 lines. That rule is
  retired along with the changelog itself: the four files are frozen,
  see the delivery section below.)
- **`linker.ld` decides kernel memory PERMISSIONS now, not just
  placement.** Four PT_LOAD segments (R / R+X / R / RW) and four
  boundary symbols -- `__kimage_start`, `__ktext_start`, `__ktext_end`,
  `__kdata_start` -- which `paging_enforce_wx()`
  (`kernel/arch/x86_64/paging.c`, called from the top of `kernel_main()`)
  reads at boot to rewrite the identity map: `.text` read-only and the
  only executable range, the rest of the image read-only and NX,
  everything else writable and NX, plus CR0.WP. Two things follow.
  **A new output section must be placed explicitly and assigned to a
  segment** -- with PHDRS declared, an orphan's permissions are wherever
  `ld` decided to put it, and the failure is silent in the direction
  that matters (a section landing in the R+X band becomes executable).
  **The `ALIGN(4096)`s between the bands are load-bearing**: W^X is
  enforced per 4KiB page, so two sections sharing a page get one
  permission and the more permissive one always wins. Adding to the
  `paging` KTESTs is the cheap way to keep this honest; their positive
  controls are in `CHANGELOG.md`'s entry.
- **A filesystem talks to a `block_device`, not to a disk.**
  `kernel/include/kernel/block.h` -- five required ops, two optional
  behind capability bits, one active device, registered like
  `display_driver`. TFS3 uses it (that is what lets a live image mount
  from RAM); **TFS2 deliberately still calls `ata_*` directly**, since a
  live image is always TFS3. Two things to know: capabilities are
  checked at registration (claim FLUSH with no `flush()` and you are
  refused), and **`persistent` is a field on the DEVICE** -- a backend
  cannot tell RAM from disk, so `fs_is_persistent()` is
  `fs->init() && blk_persistent()`. Getting that wrong makes a live
  session tell the user their files are saved.
- **TFS3's last block group may be PARTIAL** (ext2/3/4's rule), so a
  volume need not be a multiple of 128 MiB. `group_span(g)` is the one
  place that answers "how big is group g"; `T3_BPG` still means the
  STRIDE between groups. Blocks past the volume's end are marked used in
  the last group's bitmap at format time, which is why nothing else
  needed special-casing. The host writer must agree exactly or an image
  will not mount.
- **A graphics card is a `display_driver`, not a special case.**
  `kernel/include/kernel/display.h` defines the interface (required
  probe/get_surface; optional flush, cursor, accel, modeset, each behind
  a capability bit) and `kernel/drivers/display/` holds the registry plus
  the drivers -- `vesafb` (GRUB's framebuffer, registers last, always
  claims) and `vmsvga`. Adding a card is one file and one
  `display_register()` line; `gfx.c` is a rasteriser that never learns
  which card it's on. `display_probe()` REFUSES a driver whose
  capability bits and function pointers disagree, because a card that
  needs a flush and doesn't get one shows a frozen screen while memory
  holds the right pixels -- a genuinely hard bug to read, and one this
  project has already paid for twice.
- **`kernel/` directories are subsystems, not filing cabinets** --
  `arch/x86_64/` (anything a different CPU would need rewritten),
  `core/` (bring-up and whole-machine concerns), `mm/`, `proc/`, `fs/`,
  `drivers/` (one piece of hardware each), `lib/` (services with no
  hardware of their own). `kernel/README.md` has the "does it belong
  here?" test per directory. Two lines worth holding: nothing outside
  `arch/` should contain `inb`/`outb`, inline assembly or a
  control-register access; and a filesystem backend goes in `fs/`, not
  `drivers/` -- the block device is the driver, the filesystem on top
  of it isn't.
- **Kernel code touches user memory ONLY through `vmm.h`'s copy
  helpers** (`vmm_copy_from_user`/`_to_user`/`_string_from_user`).
  CR4.SMEP and CR4.SMAP are on wherever the CPU has them
  (`paging_enable_smep_smap()`), so a raw `*(T *)user_ptr` in ring-0
  code is a page fault, not a subtle bug. The helpers walk to the frame
  and copy through the kernel's own identity map (U=0), which SMAP does
  not police -- **so this kernel sets EFLAGS.AC nowhere and there is no
  STAC/CLAC window in which the protection is off.** They also subsume
  `vmm_validate_user_range()` where it used to be paired with a manual
  copy loop, closing the gap between checking a mapping and using it.
  Two traps: `paging_make_user_page()` adds U=1 to the KERNEL's own
  identity mapping, so any page it touches becomes SMAP-protected
  against the kernel's normal access to it; and **both bits are absent
  on QEMU's default `qemu64`**, so `--cpu max` is the only way the
  hardware path runs (the KTESTs assert CR4 against CPUID rather than
  demanding the bits, so they are meaningful under both). See
  `docs/decisions.md`.
- **THE DESKTOP IS A RING-3 PROCESS. This is the default since
  2026-08-18.** `/bin/wm/system/toywm` is `userland/wm/` compiled as a
  ring-3 program (sources in `userland/wm/`), spawned and waited on by
  `apps/gui3.c`; `gui` starts it. It claims the compositor role, takes
  the framebuffer grant, loads the font, composites, opens client
  windows and answers the `gui` debug console, and **all 23 GUI tools
  pass against it**. **`userland/wm/` IS GONE** (deleted 2026-08-18 with the
  `gui0` flag, ~10,400 lines including `apps/ui/` and
  `apps/gui_apps.c`), so there is ONE window manager again and the
  "make every fix twice" hazard is over. Milestone 41 is complete.
  `apps/` now holds no GUI at all: the shell, the editor, the demo, and
  `gui3.c`, which spawns the desktop and waits for it.
- **Killing the desktop is survivable, and that is the milestone's exit
  criterion**: `kill 1` at the shell revokes the framebuffer grant, ASKS
  each client window to close (never destroys it -- that would fault a
  client mid-draw), restores the text console and leaves the kernel
  running; `spawn /bin/wm/system/toywm` starts a new one.
  **`kill` and `spawn` are shell commands for exactly this reason.**
  `gui kill` cannot end the desktop (it is dispatched from inside the
  WM's own loop and `scheduler_kill()` refuses the CURRENT process), and
  `run` cannot start one (the legacy loader is not a scheduled process,
  so its `win_request()` is refused). `compositor_death_test.py` asserts
  the whole cycle.
- **A GUI tool that needs the compositor role must ASK WHO HOLDS IT**
  (`gui compositor --json`: pid 0 under the ring-0 desktop, the WM's pid
  under the ring-3 one). The role is SINGLE, so a tool that spawns its
  own stand-in evicts the desktop and then asks questions of a client
  that does not implement them -- which is how `compositor`, `screen`
  and `compdeath` all failed the moment the desktop became a process.
  Each picks its scenario from that answer now.
- **A client that needs raw input without a desktop cannot be driven by
  keystrokes** -- with no WM the physical shell owns the keyboard, so
  injected keys go there. `screenclient auto` runs its whole sequence
  itself for this reason, announcing each step so a reply can be
  attributed to the command that produced it.
- **Four things a ring-0 component loses the moment it becomes a
  process, all of which this migration hit:**
  (1) **`hlt` is PRIVILEGED** -- the WM's idle wait was a #GP on the
  first frame; `sys_yield()` replaces it (and busy-waits, see the
  roadmap). (2) **The font is not free**: anything drawing in ring 3
  must call `ugfx_font_init()`, which every window client gets inside
  `uapp_run()` and the WM had to ask for itself -- without it
  `ugfx_char_h()` is 0 and every font-derived measurement silently
  collapses (`WM_TITLEBAR_H` became 8px, so the chrome was a sliver and
  icon labels vanished while their boxes still drew). It must happen
  BEFORE any geometry is computed from it. (3) **Nobody polls the
  hardware any more** -- raw input reached a compositor only because the
  ring-0 WM forwarded it, so `kernel/proc/win_input.c` now polls and
  pushes `WIN_EV_RAW_*` from `scheduler_idle()`, and starts the mouse,
  staying silent while a ring-0 layer is registered so it cannot steal
  that WM's keys. (4) **A single `!g_ops` guard refused everything** --
  `win_server_request()` demanded a registered RING-0 presentation
  layer, so a ring-3 WM was refused the font AND window creation, with a
  bare `return -1` that nothing logged. A window server is now either a
  ring-0 layer or a registered compositor.
- **`SYS_WNOHANG` exists, and the bug that produced it is the lesson.**
  `SYS_WAITPID` BLOCKS -- its own first ABI line says so -- and the
  ring-3 WM's per-frame reap used it, so the desktop parked on the first
  client that did not immediately exit, silently and forever. Use
  `sys_waitpid_nohang()` for any "has it finished?" poll, and note it
  has NO retry loop on purpose: `SYS_RETRY` is the answer there ("still
  running"), not a signal to ask again.
- **`SYS_SBRK` is PER PROCESS, and it used to be reachable only from
  the legacy loader.** The break lives in `struct sched_process` as a
  `struct sched_heap`, armed when the slot is created; the syscall
  reaches it through `scheduler_current_heap()`, which returns NULL for
  the kernel context -- meaning "not a scheduled process", never "no
  heap". `elf_run.c`'s legacy blocking loader keeps its own single slot
  (it has no scheduler slot to use) of the SAME type through the same
  handler, so the two owners cannot drift. Before M41 stage 4b there was
  only that legacy slot, armed by `syscall_reset_heap()`, so **every
  scheduler-spawned process got -1 from `sbrk()` unconditionally** --
  silently, because Toykit has no allocator and nothing spawned had ever
  asked. The ring-3 heap is also ~14 MiB now, not 1 MiB, because a
  compositor's back buffer is 3.5 MiB at 1280x720. See
  `docs/decisions.md`.
- **The ring-3 address-space map is `kernel/include/kernel/uaddr.h`,
  stated once.** Heap base, heap limit, guard region, stack bottom/top
  and page counts, read by `scheduler.c`'s spawn path, `elf_run.c`'s
  legacy loader, `SYS_SBRK` and `idt.c`'s fault report. **The guard
  region below the stack is defined by being UNMAPPED** -- there is no
  PTE to set, so an overflow always faulted; what the header buys is
  that sbrk is bounded against it (it had NO ceiling, and a big enough
  request mapped pages over the live stack with nothing faulting or
  logged) and that a fault there is reported as `Stack overflow` rather
  than as an anonymous #PF. Adding an mmap or ASLR replaces this
  header rather than adding beside it; see `docs/decisions.md`.
- **The kernel heap has a debug mode, and it is a RUNTIME toggle**
  (`heap debug on|off`, `heap check`; `heap_set_debug()` from a test).
  Blocks allocated while it is on get a red-zone each side and are
  poisoned on free; a violation is logged and the block QUARANTINED
  (leaked on purpose -- its metadata is what proved untrustworthy), so
  detection stays assertable from a KTEST instead of needing a panic.
  The trap, if you touch `heap.c`: blocks of both shapes coexist, and
  `kfree()` tells them apart by reading the eight bytes before the
  payload -- `HEAP_RZ_MAGIC` in a red-zoned block, the header's `prev`
  in a plain one. **That is only unambiguous because every heap pointer
  fits in 32 bits (identity-mapped low 4 GiB) while the magic's top
  half is nonzero**, and because `prev` is the header's LAST field.
  Break either and the failure is silent, on the freeing path. See
  `docs/decisions.md`.
- **The kernel RELOCATES ITSELF at boot -- it is not running where it
  was linked.** `kernel_relocate_boot()` (`kernel/arch/x86_64/reloc.c`)
  runs from `long_mode_start`, picks a random 2 MiB-aligned base, copies
  the image there, patches its ~7,400 absolute references from the
  `.krelocs` table and repoints CR3 at the copied page tables. `dmesg`
  says where it landed; **`nokaslr` on the GRUB command line turns it
  off**, which is the first thing to try if something breaks in a way
  that smells address-dependent. Four things to know before touching
  any of it: it **cannot log** (serial isn't up -- decisions go into
  globals that `kernel_main()` prints), it **cannot call
  `krandom_init()`** (that spins on a PIT that hasn't started), every
  global it sets must be assigned **before the copy** or it lands only
  in the abandoned image, and `.krelocs` must stay after `.data` and
  before `.bss` in `linker.ld`. The one that cost a near-miss: `paging.c`
  reaches the page tables by LINKER SYMBOL, so a relocation that forgets
  CR3 leaves W^X silently not applied -- **and all six W^X KTESTs stay
  green**, because they read the same symbol the code wrote. Only the
  KTEST that asks the CPU for CR3 catches it. See `docs/decisions.md`.
- **`kernel/include/` is split by audience and the build enforces it**
  -- `api/` (what `apps/` may use), `abi/` (the kernel<->userland
  contract `userland/` shares), `kernel/` (internal, and NOT on
  `apps/`'s include path, so reaching for one is a compile error rather
  than a review catch). See `kernel/include/README.md`, including where
  a new header starts life (`kernel/`, moving to `api/` only when an app
  genuinely needs it).
- **A new TFS3 operation must COUNT ITS JOURNAL CREDITS, and the count
  is the design.** `txn_begin(n)` reserves `n` distinct metadata blocks
  up front (jbd2's discipline in miniature) and refuses before anything
  changes if the volume's journal can't hold them; `txn_stage()` past
  the reservation fails rather than tearing. Count the worst case, not
  the common one -- rename needs five for a cross-parent DIRECTORY move
  (both dirent blocks, the child's `..`, both parents' link counts) and
  three or four for everything else, which is why v1 images (four
  slots) refuse exactly that one operation and nothing else. Two rules
  around it: the reservation is a MAX, so dedup via `txn_stage()`
  returning the same image is free; and an insert that may GROW a
  directory has to be staged FIRST, because the grow commits its own
  transaction and can only do that while nothing else is staged.
- **Shrinking a file, or anything else that stops referencing a block,
  commits the pointer change BEFORE freeing the bit.** A crash between
  costs a leak (fsck reclaims); the other order hands a live file's
  blocks to the next allocation. That forces a commit into the middle
  of truncation, which is why both backends keep the straddling pointer
  tables' original images in memory across it -- see
  `docs/decisions.md`'s truncation entry before touching either.
- **`/etc` on the persistent filesystem is the config-file convention**
  (`vfs.c`'s `ensure_layout()` creates it, and `/tmp`, after EVERY
  mount -- `fs_init()` at boot and `fs_format_backend()` when
  `fsformat` reformats a live disk. It used to be two `fs_mkdir()`
  calls in `kernel_main()`, which is correct exactly once per boot and
  left a reformatted disk with neither directory; see
  `docs/decisions.md`. Anything that belongs to "having a filesystem"
  rather than "booting" goes beside the mount). And **a setting says
  whether it actually PERSISTED**: `tz_set_index()` and the three
  `*_config_save()`s return `enum setting_result`
  (`SETTING_INVALID`/`SAVED`/`UNSAVED`) and their shell commands print
  `(NOT saved -- ...)` rather than an unqualified success, because
  reporting "applied" as "saved" is a lie the user only finds after a
  reboot. Don't hand-roll a parser for a
  new setting -- read/write it through `kernel/include/api/etc_config.h`'s
  `etc_config_get()`/`etc_config_set()` (name=value lines, `#` comments,
  see `kernel/lib/etc_config.c`'s top comment for the exact format).
  **A new setting REGISTERS itself** (`kernel/include/api/setting.h`) --
  a `struct setting` with a name, label, type, file, a choice
  enumerator, a getter and one `apply` that validates, applies AND
  persists, announced from `settings_init()` the way a `display_driver`
  announces itself. That is what lets `SYS_SETTING` hand ring 3 the
  whole list, so the ring-3 Control Panel is GENERATED and a setting
  added anywhere in the kernel gains a Control Panel row and a `config`
  entry with no edit to either. **Don't add a setting as a bare
  `etc_config_get`/`_set` pair any more** -- that is the shape the
  registry replaced, and it leaves nothing able to answer "what settings
  exist". The files are unchanged and still hand-editable; the registry
  is an INDEX over them, which is the half `/etc` cannot provide about
  itself. `settings_reload()` (and `config reload`) re-reads them after
  a hand edit, and REPORTS refusals. See `docs/decisions.md`.
  **A setting's identity is (NAMESPACE, name), and the namespace is the
  registered name of its FILE** -- `system.font_size` for `font_size` in
  `/etc/toyos.conf`. Derived, not declared, so no `struct setting` and
  no `/etc` file changed when it landed, and a ring-3 program that
  declares its own config file with an `/etc/config.d` descriptor gets a
  namespace for free. **A bare name works only when exactly one setting
  has it and is REFUSED when several do** -- never resolved by
  registration order, which would make the answer depend on boot
  sequence; `setting_matches()` is how a caller tells "no such setting"
  from "say which one". A WRITE (`config set`/`unset`) refuses ambiguity
  where a read merely reports it. Registration refuses a duplicate PAIR,
  so the same name in two different files is two settings.
  **`/etc/config.d` is how a config FILE declares itself** -- one
  `Name`/`Path`/`Description` descriptor each, so a ring-3 program can
  register its config with no kernel change, over a built-in floor that
  lets a blank disk still describe itself (`api/config_file.h`).
  By default, put a new setting's key in the shared `/etc/toyos.conf`
  every setting lives in today (`timezone`, `font_size`, `PATH` -- see
  `kernel/lib/tz.c`/`font_config.c` for the pattern: a small
  `*_init()` called from `kernel_main()` that loads via
  `etc_config_get()`, and a `*_save()`/`*_set_*()` that writes via
  `etc_config_set()`). `etc_config_*()` takes a `path` on every call,
  though -- nothing forces one shared file. A setting with enough of
  its own keys to be unwieldy sharing `toyos.conf` (a GUI app with a
  dozen preferences, say) should get its own `/etc/<name>.conf` instead
  of cramming into the shared one just to match convention.
- **A comment's length should track how SURPRISING the code is and how
  dangerous it is to change -- not how much history it accumulated.**
  This codebase leans hard on comments and mostly earns it: the note
  above `damage_cursor()` saying `prev_cursor_*` means "where the
  cursor was last actually DRAWN, recorded in `wm_render_frame()`, NOT
  here" is what exposed a real bug weeks later, and `cpuinfo.h`'s "a
  lazy calibration deadlocks inside a syscall" has stopped at least one
  session moving that call. Keep writing those. Two things earn their
  length: **the invariant** (what must stay true), and **the trap**
  (what breaks if you edit this the obvious way).

  What does NOT earn it is the war story. Several comments here retell
  two or three past incidents with exact pixel counts and coordinates,
  forty lines where the rule is two sentences. **Cap the anecdote at
  one clause** -- "(a missed declaration left a second cursor on
  screen)" persuades exactly as well as the forensics, and the
  forensics are in `git log` if anyone truly wants them. Overall
  density is ~25% of all `.c`/`.h` lines; headers run 65-80% and that's
  fine, a header IS the documentation.

  Existing long comments are deliberately NOT being retro-trimmed: they
  read cheaply, and a bulk rewrite's most likely casualty is the one
  sentence that saves a future session. This is a rule for new writing.
- **`CHANGELOG.md` is CLOSED as of 2026-08-15. Do not add entries to
  it.** For most of this project every non-trivial change got a full
  writeup there, and the four files grew to ~12,000 lines. The reason
  for stopping is not that the record was worthless, it is that the
  same reasoning was being written three times -- once in a comment
  beside the code, once in `docs/decisions.md`, and once at length in a
  file that in practice nobody re-reads (a session measured its own
  usage: it read `CLAUDE.md`, `docs/roadmap.md`'s known-issues, the
  `docs/decisions.md` index and source comments, and never opened the
  archives at all). Writing it was a real fraction of every change's
  cost; reading it wasn't happening.

  **Where the three kinds of thing go now:**
  - *What changed, file by file* -- the COMMIT MESSAGE, which already
    lists every changed file with a one-line note (see
    `docs/decisions.md`'s versioning entry for the format). `git log`
    is the chronological record.
  - *How the mechanism works, and the trap in it* -- a comment next to
    the code. This is what actually gets found by whoever edits it.
  - *Why this way and not the obvious way* -- `docs/decisions.md`, and
    write the reasoning THERE rather than a pointer to somewhere else.
    That file is topic-indexed, which is why it gets read.
  - *What is broken / not built yet* -- `docs/roadmap.md`, with a
    reproduction precise enough to replay.

  The four changelog files stay in the tree, frozen: 299 places across
  the repo (109 in `docs/decisions.md` alone, plus comments in ~40
  source, tool and doc files) say "see `CHANGELOG.md`'s entry", and those
  pointers still resolve. Don't delete them, don't split them, don't
  update them. A new `docs/decisions.md` entry should be
  self-contained instead of pointing into them.
- **`kernel/include/api/version.h` is GENERATED, not hand-edited** --
  `tools/gen_version.sh` regenerates it from `VERSION` (repo root, e.g.
  `0.1.0-dev`) as the first step of `make all`/`make iso`. Never edit
  `version.h` directly. It defines three macros:
  `TOYOS_VERSION` (the bare string), `TOYOS_BUILD_ID` (the short commit
  plus `-dirty` when the tree did not match it) and
  **`TOYOS_VERSION_FULL`, which is what anything human-facing should
  display** -- `0.3.0-dev (2034bb1)` on a dev build (with `-dirty` when
  the tree had uncommitted changes) and a bare `0.3.0` on a release,
  always. A dirty RELEASE build is a loud stderr warning from
  `gen_version.sh` instead of a display string: the person who needs to
  know is the one running the build, and "dirty" means nothing to
  someone reading an About window. **Never add a build TIMESTAMP to it**: the script is
  deliberately idempotent (it rewrites `version.h` only when the content
  changed) because `kapi.h` includes it, and a value that differs every
  build turns every build into a full rebuild. See `docs/decisions.md`.
  **The build DATE lives in its own generated header for exactly that
  reason** -- `kernel/include/api/build_date.h` (`TOYOS_BUILD_DATE`),
  also written by `gen_version.sh`, at DAY granularity, and included by
  ONE file (`userland/wm/desktop.c`, the desktop's watermark). So it
  rebuilds one object at most once a day instead of the tree every
  build. Include it only where it is displayed; pulling it into a widely
  included header recreates the problem it is shaped to avoid. Both
  generated headers are gitignored.
- **Versioning is semver + a `-dev` suffix, not a per-change build
  number.** `VERSION` only changes via `tools/set_version.sh
  <version>`: `0.2.0-dev` starts a new dev round, `0.2.0` (no `-dev`)
  cuts a release and also stamps `CHANGELOG.md`'s `## [Unreleased]`
  section with the version + date, opening a fresh one above it. Git
  tags (`v<version>`) and GitHub Releases happen at real releases only,
  cut by hand after `set_version.sh` -- see `docs/decisions.md` for the
  full mechanics, commands, and why this replaced the old
  `tools/bump_build.sh <fix|feature|major>` scheme.
- **A GitHub Release's notes follow ONE shape, and it is terse.** Set
  when v0.2.0's had to be rewritten (2026-08-17); follow it for every
  release so they read as one series rather than as whoever wrote them.

  `docs/release-notes-template.md` is v0.2.0's notes kept verbatim as
  the worked example -- copy its shape rather than re-deriving it.

  Order: **Install first**, then the areas that changed, and nothing
  else. Install goes at the top because a release page's job is to get
  somebody running the thing -- prerequisites, the three files to
  download, the command, and one line on what persists between runs.
  Then one `##` per area (Windowing / Filesystem / Memory protection /
  Process model / Testing / Structure), flat bullets under each.

  What is deliberately NOT in them:
  - **No commit counts** and **no milestone numbers.** Both are internal
    bookkeeping; a milestone number means nothing to a reader and dates
    the note the moment the roadmap is renumbered.
  - **No pointer to `CHANGELOG.md`.** It is frozen (see below), so the
    pointer rots.
  - No promotional framing ("the largest structural change the project
    has had"). State what exists.

  **And the accuracy rule that caused this:** v0.2.0 shipped titled *the
  GUI moves to ring 3*, which was not true -- at that tag
  `apps/calculator.c`, `notepad.c`, `terminal.c`, `uidemo.c`, `about.c`
  and `taskmgr.c` were all still kernel-space beside their new ring-3
  twins, and `userland/wm/` was ring 0 (as it still is). A release note is
  the one document written from memory rather than from the code, so
  **check every claim against the TAG** -- `git ls-tree -r v<x> --name-only`
  and `git show v<x>:<file>` answer it in seconds -- and say plainly what
  is still in progress.
- **Commit messages list each changed/added file with a one-line note
  in the body** (subject line stays a short summary) -- see
  `docs/decisions.md`'s versioning entry for the exact format.

## Working in the cloud sandbox vs. directly on the user's machine

This repo gets worked on both ways: from a Cowork cloud session with
the user's real checkout reachable only through the device bridge
(`mcp__remote-devices__*`), and directly on the user's own machine
(e.g. a local Claude Code session) with normal file/Bash tools against
the real checkout. The mechanics below differ a lot between the two,
so **figure out which one you're in before following either half**:

- **Deterministic tell: run `git config user.name`.** A device-bridge
  session has NO git identity configured at all, local or global (it's
  its own isolated VM) -- empty output means Cowork/device-bridge.
  Non-empty (this repo's convention sets it to `toy-os`, see below)
  means a direct local checkout.
- Corroborating signal: are `mcp__remote-devices__*` tools (or
  equivalent device-bridge tools) actually available to call this
  session? Present means Cowork; absent means direct.
- If those disagree, or it's still unclear, just ask the user directly
  rather than guessing -- getting this wrong means either trying to
  push from a session where it's actually blocked, or going through
  the whole SendUserFile/device_commit_files dance unnecessarily.

### Cowork cloud sandbox (device bridge)

Four things about that setup that aren't obvious until you hit them:

- **`Makefile` and anything under `.github/workflows/*.yml` are
  protected against `device_commit_files`** (writes get rejected --
  check its response's `rejected` array, don't assume a batch landed
  in full). Edit + verify in the cloud sandbox as normal, deliver as
  `Makefile.new` / `build.yml.new` via `SendUserFile` +
  `device_commit_files`, then apply it yourself over `device_bash`
  (`cp Makefile.new Makefile`, `diff` to confirm, move `Makefile.new`
  into `_to_delete/`) -- `device_bash` is NOT blocked from writing
  these files directly, only `device_commit_files` is. See
  `docs/decisions.md` for the full mechanics.
- **The device bridge can't delete files, and `git` run through it
  leaves stale `.git/index.lock` files behind** (even a read-only
  `git status`). Both fixed the same way -- `mv`, not `rm`. To remove a
  file, `mv` it into a `_to_delete/` subfolder and tell the user to
  delete that folder themselves. For git, **always use
  `tools/device_git.sh`**, never run `git` directly via `device_bash`
  -- it sweeps stale locks both before and after the real command, so
  the repo comes back lock-free. See `docs/decisions.md` for why this
  is needed even for reads.
- **The device-bridge session has NO git identity configured, local or
  global** -- confirmed directly: `git config user.name`/`user.email`
  and `git config --global --list` all come back empty in a fresh
  `device_bash` call, because that call runs in its own isolated VM,
  not the user's actual desktop environment. A plain `device_git.sh
  commit` will fail outright ("Please tell me who you are") unless the
  identity is passed explicitly every time:
  `bash tools/device_git.sh -c user.name="toy-os" -c
  user.email="noreply@toy-os.local" commit -m "..."`. This is also the
  standing privacy convention for this repo now, not just a workaround
  -- **never let a commit here carry the maintainer's real name or
  personal email**, session-made or otherwise (see `docs/decisions.md`'s
  entry on the history rewrite that scrubbed a real name out of every
  prior commit -- don't reintroduce what that fixed).
- Build and test in the cloud sandbox first (`make clean && make all
  && make iso`), confirm it's clean, *then* deliver + commit files to
  the user's machine. Don't commit unverified changes.
- **`git push`/`gh release create` from the cloud sandbox is not just
  discouraged, it's actually blocked.** Confirmed directly (v0.0.9
  release): pushing with the repo's token embedded in the remote URL
  still fails with `remote: access denied by the git proxy: ... not in
  this session's authorized repository set` -- the sandbox's outbound
  git egress goes through an allow-list proxy, independent of
  credentials. Read-only git (`fetch`, `ls-remote`) works fine through
  the same proxy. The device bridge has no network access at all
  either (by design). So there is no path in this environment to
  actually publish -- always tag/prep locally on both checkouts, then
  give the user the exact commands to run from their own machine's
  terminal. `gh` isn't preinstalled in the sandbox (`apt-get install
  -y gh` if needed there for read-only checks).

### Direct local checkout

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

Confirmed directly in a real local session (2026-08-12): this is
simpler than the Cowork setup in every way that setup works around --

- Git identity is already configured (`toy-os` /
  `noreply@toy-os.local`, matching this repo's standing privacy
  convention -- see above), so plain `git commit` just works with no
  `-c user.name=...`/`-c user.email=...` needed on every call. Still
  worth double-checking `git config user.name` if it's ever in doubt
  rather than assuming.
- Plain `git` works throughout -- no stale-`index.lock` issue, no
  `tools/device_git.sh` wrapper needed, no `mv`-instead-of-`rm`
  workaround for deleting a file.
- No protected-file restriction -- `Makefile` and
  `.github/workflows/*.yml` can be edited and committed directly, no
  `.new`-suffix relay needed.
- `git push origin main` and `gh release create` both work directly
  from the session -- confirmed by actually doing both (pushing
  ordinary commits repeatedly, and cutting the `v0.1.0` GitHub Release
  end-to-end with `gh release create` + `gh release upload`). Still
  treat both as actions to confirm with the user first per this file's
  general "Executing actions with care" guidance (pushing/publishing is
  visible to others), just don't tell the user it's *impossible* the
  way the Cowork section above correctly says it is there.
- `tools/preflight.sh`'s closing message and the QMP-launch pattern in
  `tools/qmp_test.py` are both mode-aware/updated for this case now --
  see their own comments if either looks like it's giving Cowork-only
  advice.

## Building

**Boot flags can be baked into the ISO** rather than typed into the
GRUB menu each boot: `make iso KCMDLINE="video=1920x1080 nokaslr"`
(also `live-iso`/`demo-iso`). Empty by default, so every automated path
is unaffected. `docs/boot-flags.md` lists every word, and the
`grub*.cfg` files carry a short summary of it for anyone reading them on
the ISO.

**GRUB's `e` editor shows the menuentry BODY only**, so the boot-word
summary is repeated inside each `menuentry` in the three `grub*.cfg`
files, not just at the top of them -- a comment above `menuentry` is
never seen by anyone editing the line it documents. Keep the inline copy
to a few lines: the edit screen is ~20 lines and a full table pushes the
actual commands off it.

**`video=<W>x<H>` only does something on a MODESETTING driver.**
`vmsvga` programs the CRTC and honours it; a plain VESA framebuffer
cannot, so on an adapter GRUB has already fixed the flag is inert --
which is the VirtualBox VBoxVGA case, where the only fix is
`VBoxManage setextradata <vm> CustomVideoMode1 1280x720x32` to put the
mode in the BIOS list at all. The driver walks a fallback LADDER
(`display_mode_candidate()`) rather than giving up on one refusal, and
falls back to GRUB's mode only when nothing on it works.

```
make all    # kernel.bin + userland test ELFs
make iso    # + toy-os.iso (grub-mkrescue)
make test   # boot headless, run the in-kernel test suite, exit non-zero on failure
make verify # the full pre-delivery gate: clean build + iso + boot test + ktest
            # (same as tools/preflight.sh, which also summarises `git status`)
make run   # boots in QEMU with an SDL window (the user's machine, not usable headlessly)
make live-iso   # toy-os-live.iso -- carries a TFS3 image as a GRUB module
make run-live   # boots that with NO disk attached at all
make demo-iso   # toy-os-demo.iso -- boots straight into a scripted tour
make run-demo   # boots that; for showing the system with nobody typing
make run-audio  # same as run, + a PulseAudio backend so the PC speaker (`beep`) is audible
make run-kvm    # same as run, but KVM-accelerated (-enable-kvm -cpu host) instead of
                # TCG emulation -- needs /dev/kvm readable
make debug # boots frozen (-s -S) for real GDB debugging -- see "Debugging with GDB" below
```

**`make run-kvm` is not a straight speedup, and throughput numbers from
the two modes are not comparable.** Measured on the same disk image,
same host, `stress 150`: TCG 22.8 MB/s write / 29.2 MB/s read, KVM
12.1 / 18.7 -- KVM about 1.9x *slower* for disk I/O. Guest code that's
actually computing gets much faster, but every port-I/O instruction
becomes a hardware VM exit (~1us) where TCG services one in-process
(~tens of ns), and this kernel's disk path is dense with `inb`/`outb`.
So: useful as a second mode to test in, and useful for anything
CPU-bound, but always say which mode a benchmark came from --
`tools/vm.py --kvm` runs the same configuration headlessly.

**The other reason to reach for KVM has nothing to do with speed: it
HONOURS GUEST MEMORY TYPES and TCG does not.** Under `make run` a
write-combined or uncached framebuffer behaves exactly like cached RAM,
so an entire class of graphics performance bug cannot happen there --
which is what made the console's write-combined scroll regression
reproduce on the maintainer's laptop and under `make run-kvm`, and
nowhere else. If a report is "slow only on real hardware", try
`python3 tools/vm.py --kvm run "gfxbench 20"` BEFORE concluding it is
untestable here; a previous session recorded exactly that conclusion and
it was wrong. See `docs/decisions.md`.
**Source discovery is recursive now** -- every `.c` under `kernel/` or
`apps/` is compiled and every `.asm` under `kernel/` assembled, with
`build/` mirroring the source tree, so a new directory needs no Makefile
edit at all. (It used to be one hand-written wildcard + pattern rule +
mkdir target per directory; `userland/wm/` and `apps/ui/` each paid that tax
when they appeared.) The flip side: a `.c` file anywhere under
`kernel/` or `apps/` IS in the kernel image -- there's no scratch file
the build ignores, so throwaway code goes somewhere else.

**`make run` uses `-display sdl,grab-mod=rctrl`, no explicit pointer
device.** Two things worth knowing if you ever touch this line:
`grab-mod` (the key that captures/releases the mouse once grabbed,
here right Ctrl) is an SDL-only display option -- QEMU rejects it
outright on `gtk` ("Parameter 'grab-mod' is unexpected"), which is why
this isn't `-display gtk,...` even though gtk was tried first. And
deliberately NO `-device usb-tablet`/`-device usb-mouse` -- this
kernel's mouse driver only speaks PS/2 (see `kernel/drivers/mouse.c`),
there's no USB stack at all, and adding an explicit USB pointer device
makes QEMU route host mouse motion to THAT instead of the emulated
PS/2 mouse, so the guest receives nothing and the cursor just never
moves. Bit an actual user session once (see CHANGELOG-archive-2.md around build
293's Makefile fix) -- looked exactly like a driver bug, wasn't one.

**A plain `make all` is safe after editing a shared header now** (as of
build 308) -- the Makefile tracks header dependencies (`-MMD`/`-MP`;
see CFLAGS/USERLAND_CFLAGS and the `-include` line near `$(KERNEL)`'s
rule), so editing e.g. `apps/ui/ui_scrollback.h` correctly rebuilds
every `.o` that includes it, not just the ones whose own `.c` file
changed. This
used to not be true, and it produced a genuinely bizarre-looking bug
once -- the Start menu's item labels showed raw function-prologue
machine code reinterpreted as text -- caused by exactly the failure
mode dependency tracking now prevents: a stale `.o` compiled against
an OLD struct layout sitting next to freshly-rebuilt ones that saw the
NEW layout (e.g. an array indexed with the wrong element stride). A
`make clean && make all` is still a reasonable "when in doubt" move if
a GUI test ever shows something inexplicable right after a header
change (dependency tracking is only as good as the `.d` files being
correct), but it should no longer be *routine* -- if you find yourself
needing it regularly, that's a sign the tracking broke somehow, worth
investigating rather than working around. **It broke exactly that way
once, and stayed broken:** the `-include` line named six directories by
hand, and when source discovery went recursive the kernel's objects
moved to `build/kernel/...`, so 88 of 161 `.d` files silently stopped
being read and `touch kernel/include/kernel/process.h` rebuilt
*nothing*. It's a recursive `find` now, and **`tools/check_deps.py`
(in `preflight.sh` and CI) proves per build directory that the tracking
is actually live** -- so if that check is green, believe the tracking
and go looking elsewhere. **What is NOT tracked is a CFLAGS change** --
the `.d` files record headers, so editing `CFLAGS`/`USERLAND_CFLAGS`
invalidates nothing and the next build happily links objects compiled
under the old flags. That bit for real when `-ffunction-sections` was
added: `--gc-sections` looked like it half-worked (binaries shrank,
because unreferenced archive members were skipped, but unused functions
survived inside every object that WAS pulled in) purely because nothing
had recompiled. **`make clean` after a flags change**, and treat a
compiler flag that appears to work partially as a stale-object symptom
first. See `docs/decisions.md`. One subtlety if you ever
touch `tools/gen_version.sh`: it's deliberately idempotent (only
rewrites `kernel/include/api/version.h` when `VERSION`'s value actually
changed) specifically so this dependency tracking doesn't regress --
`kapi.h` includes `version.h`, so an unconditional rewrite every build
would make every file that includes `kapi.h` (nearly everything) look
"out of date" and rebuild every single time.

**The live and demo ISOs are SEPARATE artifacts, on purpose.** The
ordinary `toy-os.iso` carries no GRUB module: a 129 MiB one took the
boot smoke test from 1.6s to 7.0s locally and blew CI's 12s timeout
outright, because GRUB reads the whole module off the emulated CD-ROM
before the kernel starts. The live image is 24 MiB now (partial block
groups), so folding it back into the default ISO is possible -- but
measure the boot first. `tools/live_boot_test.py` drives the live one
(launch with `launch_qemu_cmd(disk=None)`, which omits `-drive`
entirely); the demo one is `data/wm/demo.script`, a text file on the
image, performed one step per WM iteration through `wm_debug_dispatch()`
-- the same path the GUI tests use.

**`make all` does NOT update `toy-os.iso`, and every headless test boots
the ISO.** `ktest_run.py`, `boot_smoke_test.py`, `vm.py` and the GUI
tools all launch `toy-os.iso`, so a `make all` without `make iso`
leaves them testing the PREVIOUS build while reporting on the current
one. This does not fail loudly -- it fails as a clean pass. It bit for
real this session in the worst possible place: a positive control (make
`.text` writable, expect the W^X KTESTs to go red) came back 132/132
green, which reads exactly like "the test is measuring nothing" and
sent a session looking at the test instead of the build. The rebuild
made it fire on precisely the two right checks. So: **`make iso`, not
`make all`, before any headless run**, and when a positive control
turns nothing red, check that the ISO is newer than the change before
suspecting anything else (`ls -l build/kernel.bin toy-os.iso`). This is
the same lesson as the fixture one below with a different cause -- the
data never reached the code under test because the code never reached
the machine.

**`make test` / `tools/ktest_run.py`** -- the in-kernel test suite.
Tests are `KTEST("suite", "name") { ... }` blocks living next to the
code they exercise (`kernel/mm/mm_test.c`, `kernel/fs/fs_test.c`, ...);
they register themselves through a `.ktests` linker section, so a new
test file needs no registry entry and no Makefile edit. `make test`
boots headless, drives `ktest` over the serial debug console and exits
non-zero on failure; `ktest` / `ktest <suite>` runs them interactively.
Two things worth knowing before writing one: tests run inside the LIVE
booted kernel (so don't assume a pristine heap or an empty filesystem --
that assumption is exactly what broke `heap_selftest()` when it moved
off the boot path), and `kernel/include/kernel/fault_inject.h` can fail
the next N ATA writes/reads or kmalloc calls, which is how the error
paths get tested at all. Nothing runs tests at boot any more.

**`tools/vm.py` -- start a VM once, then talk to it in TEXT.** This is
the fastest path for anything that isn't about pixels:

```
python3 tools/vm.py start
python3 tools/vm.py exec "fsck" "df"     # real shell output, as text
python3 tools/vm.py shot look.png        # pixels when you want them
python3 tools/vm.py stop
python3 tools/vm.py run "ktest"          # start+exec+stop in one
python3 tools/vm.py --kvm run "stress 150"   # same, KVM-accelerated (see `make run-kvm`)
python3 tools/vm.py --cpu Skylake-Client run "lscpu"  # a specific QEMU CPU model
python3 tools/vm.py --vga vmware start   # the MODESETTING driver (vmsvga); `std` has none
python3 tools/vm.py --instance 2 --disk /tmp/b.img start  # a second VM, alongside
```

`--instance N` is how you run more than one VM at once: pidfile, serial
socket, QMP port and VNC display are all derived from N, so slot 2 can
never stop slot 0's VM or connect to its console. Slot 0 is the default
and is exactly what it always was.

**Some CPU features are unreachable in BOTH default modes, and the
invariant TSC is one.** `--cpu` applies under `--kvm` too now, and that
combination is the ONLY way to run the kernel's TSC clocksource:

```
python3 tools/vm.py --kvm --cpu host,+invtsc start   # the TSC path
```

TCG does not implement `invtsc` at all (`-cpu max,+invtsc` prints
`warning: TCG doesn't support requested feature` and clears the bit),
and KVM withholds it even under `-cpu host`, because a guest that has
seen it cannot be live-migrated. So the default test environment
exercises only the PIT-backed clocksource, and a green suite says
nothing about the TSC one. `notsc` on the GRUB line reaches the same
split from the other side. **Generalise it: before concluding a feature
"just isn't available in QEMU", check whether it needs an explicit `+`
flag AND which accelerator supports it** -- the two are independent, and
this cost real time.

**`--cpu MODEL` matters more than it sounds** for anything reading
CPUID: the default `qemu64` reports as **AuthenticAMD** with no CPUID
leaf 4, so cache-topology code takes the AMD `80000005H`/`80000006H`
fallback there and the leaf-4 path never runs at all. `--cpu
Skylake-Client` is GenuineIntel with leaf 4 populated; `--cpu max`
gives the widest feature set. Test CPU-dependent code against more than
one, or half of it is unexercised.

It drives the serial debug console's `sh <command>` rather than
emulating keystrokes, so there's no keyboard-layout dependence (a `se`
layout turns `write_test` into `write?test`), no dropped keys, and the
result is assertable instead of a screenshot to read. It only ever kills
a QEMU it started itself (its own `.vm.pid`), so an interactive `make
run` window is never at risk. GUI/rendering work still needs
`qmp_test.py`/`gui_flow.py` -- a text transcript says nothing about
whether a button is drawn in the right place.

**There is a GUI app built to be tested against: "UI Demo"**
(`userland/gui/uidemo.c` -- a RING-3 process since Milestone 41's
stage 0; it was `apps/uidemo.c`). One of every Toykit widget at documented,
font-derived, content-relative offsets, and every interaction logged as
one parseable line (`uidemo: button 2`, `uidemo: check alpha on`,
`uidemo: cancel btn`). Combined with the `gui` commands below, a GUI
test becomes drive-and-assert over a single serial wire with no
screenshot in the loop -- and when a click lands on the wrong thing, the
log says which widget it actually hit. Read its top comment for the
layout table and the log grammar before writing coordinates by hand.

**Its ring-3 counterpart is "Shapes"** (`userland/gui/gfxdemo.c`, `run
shapes` from a Terminal) -- **two scenes, switched with the `2D / 3D`
button or `S`**: a rotating wireframe triangle and ellipse, or a
perspective-projected wireframe CUBE with depth-shaded edges. Both are
drawn with the shared geometry module, with the same one-line-per-state
log grammar (`gfxdemo: aa off`, `gfxdemo: scene 3d`) and self-reported
`gfxdemo: layout canvas <x> <y> <w> <h>` / `layout buttons <x> <y> <w>
<h> <pitch> <count>` -- take button positions from the second of those
rather than deriving them from the canvas rect. Use it as the known
target when testing
`kernel/lib/geom.c`, `uui_canvas`, or ring-3 drawing generally;
`tools/gfxdemo_test.py` drives it. **A ring-3 app's diagnostics go to
`sys_eprint()` (stderr), not `sys_print()`** -- stderr reaches the
kernel log and `dmesg`, where a test can read it, while a GUI client's
stdout goes nowhere useful (it has no terminal attached) and a spawned
process's stdout goes into its parent's pipe.

**`tools/gui_debug.py` -- ask the WM what it's doing, instead of
measuring a screenshot.** The serial debug console has a `gui` command
family now (`userland/wm/wm_debug.c`), live while the desktop is up:

```
gui windows [--json]     rects, content rects, z-order, focus
gui probe X Y [--json]   which window/region is at a point, and what overlay would take the click
gui menu | gui taskbar    row + button geometry, as the kernel computes it
gui ctxmenu [--json]     the OPEN right-click menu's rows, same shape as `menu`
gui state [--json]       overlays, cursor, armed drag/resize/press, damage rect
gui damage [verify on|off]  the damage rect; verify catches missed damage
gui open <App>           open a window directly -- no Start-menu clicking
gui dialog [--json]      the open confirm dialog's message and button CENTRES
gui compositor [--json]  the registered compositor pid, queue depth, drops
gui spawn PATH [args]    run a ring-3 binary directly -- no Terminal in the loop
gui watchdog [<ms>|off]  slow-frame threshold, plus how often it fired
gui kill PID             end a process -- `gui spawn`'s counterpart
gui click X Y | gui rclick X Y | gui drag X1 Y1 X2 Y2 | gui key <c> [alt|ctrl|shift]
```

`gui spawn` is the ONLY way a test starts a client now -- since M41's
stage 0 there is no kernel-space Terminal to type `run <name>` at, and
`gui open Terminal` spawns the ring-3 one, whose window does not exist
yet when injected keys arrive. `DebugConsole.spawn(path, title)` wraps
it, waits for the window and gives the client a moment to present its
first frame; a capture taken before that reads DESKTOP through the
window's rect. It also takes ARGS now (`gui spawn /tests/spin_test 40`).
It is the one to reach for when a test needs a ring-3 client:
`gui open` can only launch what is in the app registry, so tests used to
open a Terminal and type at it, dragging that Terminal's allowlist, its
pending-process slot and its shell into a test about something else --
and a client that never exits could not be tested at all, because the
Terminal that spawned it then could not be closed. `gui dialog` reports
the confirm dialog's buttons; do not scan for them by colour, which
assumed "Yes"/"No" sizing and breaks on "Force Quit"/"Wait".

`rclick`/`ctxmenu` are newer than the rest and exist for a specific
reason: no test could open a context menu at all, which is how its
Close row went on destroying ring-3 windows without their close
handshake while the X button beside it asked politely. Use
`DebugConsole.ctxmenu_row("Close")` rather than deriving a row from
`item_h`. And note `gui key` takes modifier words -- `gui key 0xa5 alt`
is Alt+F4, which is the only way to close a window from a test now that
Esc doesn't.

**`gui damage verify on` catches the WM's worst bug class.** The
compositor only repaints declared damage, so anything that changes on
screen without being declared leaves stale pixels -- no crash, no
assertion, often visible in one interaction only. Verify mode renders
every frame twice (damage-limited, then unrestricted) and reports any
differing pixel with coordinates. It found four real bugs in its first
minute. **Read the whole report line before believing it**: it also
carries the diff's BOUNDING BOX (63 px are a caret, a border or a
scrollbar depending on their extent) and a verdict from a THIRD render
of the same frame -- `scene stable (real missed damage)` means the
comparison is trustworthy, `SCENE UNSTABLE -- verdict void` means
`render_scene()` disagreed with itself and the report proves nothing.
See `docs/decisions.md`. Turn it on whenever you touch drawing, damage, focus or
chrome; `docs/gui-guidelines.md` has the invariant it enforces.

Reach for this BEFORE QMP for anything that isn't literally about
pixels: it returns facts you can assert on rather than an image to read,
and `DebugConsole.menu_row("Terminal")` gives the real row centre
instead of `gui_flow.py`'s hardcoded menu arithmetic. Two things to
know: injected input enters at the WM loop **below the PS/2 driver**, so
it exercises WM/app logic and proves nothing about the mouse driver; and
it is **asynchronous** -- call `DebugConsole.settle()` before asserting,
because a command dispatched from inside `wm_run()` cannot block waiting
on `wm_run()`.

**`DebugConsole.capture_panic()` is how you read a dying guest.** A
panicking kernel never returns a prompt, so the normal command/response
cycle cannot complete and every read looks like a timeout; sending an
empty line and taking what arrives before the read gives up is the
panic block. **Do NOT open a second connection to the serial socket** --
the console already holds it and the new one receives nothing, which
cost three attempts before the helper existed.

**`settle()` POLLS, and don't replace it with a sleep.** It waits on
`gui state`'s `pending` (the WM's own undelivered-event count) rather
than sleeping a fixed interval, because the loop is not a metronome: a
drag takes ~110ms normally and ~800ms under `gui damage verify on`, and
the number moves again with font size, window count and display driver.
The fixed sleep this replaced didn't fail loudly -- it let windows move
between a `gui windows` read and the command using those coordinates, so
drags grabbed the wrong thing and a damage test reported a different bug
on each run of the same script. **Also: `click()`/`drag()` return
`events()`, which filters to the `uidemo:` prefix and will silently drop
a `wm:` line** -- use `logs()`/`damage_bugs()` for anything the kernel
logs. Both traps cost a session real time; see `docs/decisions.md`.

**`tools/boot_smoke_test.py`** -- a fast, non-GUI boot check: boots
`toy-os.iso` headlessly, watches `serial.log` for the expected kernel
init sequence (or a `PANIC:`), exits 0/1 in a few seconds. No QMP, no
mouse/keyboard, no screenshots. Use this as the first check for a
kernel/driver-level change (a new driver, a filesystem backend, a
syscall) -- it answers "does it still boot cleanly," which is most of
what those changes need verified, much faster than the full QMP
GUI-testing dance below. It does NOT replace QMP testing for anything
that touches rendering, input, or window behavior -- a clean boot log
says nothing about whether a button is drawn in the right place; see
its own module docstring for the same division stated in code.

**GitHub Actions (`.github/workflows/build.yml`)** runs `make clean &&
make all && make iso` plus `check_deps.py`, `check_layout.py`,
`boot_smoke_test.py`, `ktest_run.py` and `usertest_run.py` on every
push/PR
to `main` -- so a build break or boot regression is caught
automatically, independent of whether a session (or a human) remembered
to verify locally first. This doesn't replace verifying locally before
delivering a change (still do that -- see "Working in the cloud
sandbox" above), it's a second, automatic check behind it.

**READ THE ABI COMMENT OF ANY CALL YOU SWAP IN.** The single most
expensive mistake of the M41 migration was replacing a non-blocking
`scheduler_poll()` with `sys_waitpid()` and writing a comment asserting
it was "non-blocking in the same sense" -- while `SYS_WAITPID`'s own
first line said **BLOCKS**. It cost a day: the ring-3 desktop died
silently on its first client, and the hunt produced THREE wrong
mechanisms before the header was read. A port is exactly where this
happens, because the new call's name resembles the old one's.

**AND A MECHANISM THAT EXPLAINS THE SYMPTOMS IS NOT THE MECHANISM THAT
CAUSED THEM.** One of those wrong diagnoses was written up and committed:
it blamed a `sti; hlt` wait against `idt.c`'s single `g_next_kernel_rsp`,
reasoned that it survives two contexts and not three, and fitted every
observation -- the hazard was real and documented, just not this bug's.
Before publishing a root cause, do the cheap disproving check, and
prefer a discriminating experiment (`/bin/hello` exits at once and was
harmless; `winclient` waits and was fatal -- that pair located the bug
class in one run) over a plausible story.

**BEFORE BELIEVING ANY GUI TEST FAILURE, RE-RUN IT ON A FRESH IMAGE.**
`make iso` re-seeds `disk.img` by SYNC, never reformat, so anything an
earlier run wrote is still there -- and several tools' apps WRITE.
`menubar_test` saves a file (a stale recent-files entry changes a
submenu's contents, so every later click in it lands on a different
row), `cpanel_test` and `cursor_theme_test` both persist settings to
`/etc`. A dirty fixture fails in a way that reads exactly like a code
regression or a flake. So: `make clean-disk && make iso`, then re-run
with `--logs DIR`. If it still fails, prove it is not yours by
rebuilding `HEAD` (`git stash push -u -m <tag>`, apply by SHA, never a
bare pop) -- that took one build cycle and correctly exonerated a change
here on 2026-08-17. Note this is NOT a universal explanation: the same
day, a `menubar`/`gfxdemo` failure survived `clean-disk` and turned out
to be a pre-existing parallel-load flake (see `docs/roadmap.md`).

**A COPY of `disk.img` GOES STALE the moment you rebuild.** `make iso`
re-seeds the real `disk.img` with the newly built `/bin` binaries; a
copy taken before that still holds the OLD ones. So a VM booted from the
copy runs the NEW kernel (from the ISO) against the OLD userland, and a
ring-3 fix appears not to work while the kernel half of the same change
plainly does. That reads exactly like a bug in the app. **Re-copy after
every `make iso`**, not once at the start of a session -- it cost real
time here, debugging a layout fix that had already landed.

**Test against a COPY of `disk.img` if the user might have their own
QEMU open.** Two separate hazards, both hit for real: QEMU takes a
write lock on the image, so a headless launch dies with `Failed to get
"write" lock` while an interactive `make run` holds it; and `make iso`
re-seeds `disk.img` every time, which rewrites the filesystem
underneath a VM already booted from it. `cp --reflink=auto disk.img
/tmp/test.img` and then `vm.py --disk /tmp/test.img` (or
`launch_qemu_cmd(disk=...)`) avoids both -- and is the right move
anyway whenever a test needs particular files on disk, since it leaves
the real image alone. Both also take `--qmp-port`/`--vnc`
(`qmp_port=`/`vnc_display=`) for running a second instance alongside
an existing one.

**A copy is not enough for `make iso`/`make verify`/`preflight.sh` --
ASK the user to close their QEMU first (standing request).** Working
against a copy dodges the write lock, but those three targets re-seed
the real `disk.img` regardless of what any test is pointed at, so
they rewrite the filesystem underneath a VM the user has open and
leave it running on a stale in-memory view. This is easy to miss
because nothing fails loudly at the time. What does NOT need them to
close anything: editing, `make all` (kernel only, never touches the
image), and `vm.py --disk <copy>`. So check whether a QEMU is running
(`ps aux | grep qemu-system`) before the verify gate rather than
after, and ask -- don't just work around it silently, and don't ask
for the cases in the previous sentence either.

**`gfxbench` measures the framebuffer, and its number is MEANINGLESS
under QEMU.** QEMU's framebuffer is cached host RAM; real hardware's is
uncached MMIO, where every store is a bus transaction the CPU stalls on.
That makes a whole class of performance bug structurally invisible to
every test here -- a clean `gui_regress` says nothing about it. The
framebuffer is write-combined at display probe now (PAT, or an MTRR
under `nopat`), and `gfxbench` prints which mechanism is live beside its
timings. Expect ~2500 MB/s in emulation and treat that as evidence the
figure is not real. **If a user reports something that reproduces only
on hardware, ask what the emulator models differently BEFORE doubting
the report.** See `docs/decisions.md`.

**AND KVM HIDES A DIFFERENT CLASS AGAIN -- TIMING.** The memory-type
note above is about what TCG does not model; this is about what it makes
too slow to notice. Under KVM a transfer completes in microseconds, so a
driver race whose window is "between issuing a command and arming its
completion flag" goes from unreachable to constant. That is a real bug
this project shipped: `dma_issue()` cleared `g_dma_irq_fired` AFTER the
command byte, an interrupt landing in that window was wiped, and the
waiter then burned the whole 5s `DMA_WAIT_TICKS` budget before a retry
that succeeded instantly. The desktop froze for 2-5 seconds per disk
read under `make run-kvm` and was perfect under `make run`.

Generalise it: **every automated test in this repo runs TCG**, so a
green suite says nothing about anything whose behaviour depends on how
FAST the emulated hardware is. When a user reports something this
environment cannot reproduce, try `--kvm` before doubting the report --
and note the report may be of a symptom (a freeze) whose cause is a race,
not slowness. `tools/kvm_soak.py` is the standing check for this.

**But `make run-kvm` DOES honour guest memory types, and that is the one
way to reproduce this class of bug locally.** TCG ignores PAT entirely,
so a write-combined framebuffer behaves exactly like a cached one under
plain `make run`; KVM does not. That distinction is what turned "slow
only on the maintainer's laptop" into a measurement:
`python3 tools/vm.py --kvm run "gfxbench 20"` reported **178.5 ms** per
scrolled text line against **0.5 ms** once the console stopped reading
the framebuffer. Reach for `--kvm` before concluding a hardware-only
report is untestable.

**The framebuffer console draws into a back buffer and PUBLISHES
separately, and the invariant is that it never READS the framebuffer.**
Write-combining is a write optimisation that makes reads strictly worse
(uncached, no prefetch), so the console's old scroll -- shifting visible
pixels up in place -- became its slowest operation the moment WC landed.
It draws through `gfx.c`'s back buffer now and publishes with
`gfx_present()`. **The trap: a path that prints and then HALTS without
reaching a flush point leaves its text in RAM only.** The flush points
are `vga_present()` in `keyboard_getchar_mods()`'s idle loop, a
tick-throttled present at the end of `vga_putc()`, and an explicit call
on `idt.c`'s panic path -- add one to any new print-then-halt path.
Note there is deliberately no dirty flag in `vga.c`: `gfx.c` already
tracks the dirty box and `gfx_present()` no-ops when empty, so a second
copy could only disagree with it silently. See `docs/decisions.md`.

**`strace <binary>` is often the fastest way to see what a `/bin`
binary is doing** -- one decoded line per syscall
(`open("notes.txt", O_WRITE|O_CREAT) = 3`), and the same lines land in
`dmesg`, so `python3 tools/vm.py exec "strace file_test"` returns text
you can assert on. Reach for it before adding temporary `klog_write()`
calls inside a syscall handler; see `kernel/proc/strace.c`.

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

**First: is this actually a GUI change?** If not, `tools/vm.py` (above)
is faster and gives you text you can assert on instead of a screenshot
you have to read. The order of cheapness is
`boot_smoke_test.py` (does it boot) -> `make test` / `vm.py exec` (does
it work) -> QMP (does it look right). Reach for this section when the
answer genuinely depends on pixels -- widget layout, rendering, mouse
behaviour, window chrome -- because a text transcript says nothing
about any of those.

There's no interactive display in this environment, so GUI testing
goes through QEMU's QMP socket: launch headless, drive keyboard/mouse
via QMP commands, `screendump` to prove it visually.

**Use `tools/qmp_test.py` -- don't rederive this from scratch.** It's a
committed, working helper module (`QMPSession`, with `goto()`/`click()`/
`drag()`/`wheel()`/`mouse_down()`/`mouse_up()`/`recalibrate()`/
`send_key()`/`send_text()`/`screenshot()`) built from exactly this kind
of testing, with the gotchas below already handled. Past sessions each
independently hand-rolled similar scripts in the cloud sandbox (never
committed, so lost between sessions) and paid the cost of hitting these
gotchas fresh each time -- that's why this module exists now. Import it
(`sys.path.insert(0, "tools"); from qmp_test import QMPSession`) rather
than writing new inline socket/JSON code, and add to it (rather than
writing a one-off script) if you need a capability it doesn't have yet.

The gotchas it already gets right, for when you need to know why:

- **Launch via `tools/qmp_test.py`'s `launch_qemu_cmd()` -- call it (or
  copy its returned command verbatim), don't hand-roll a
  `qemu-system-x86_64` invocation from scratch.** It returns a command
  backgrounded with `-daemonize -pidfile <path>`, not a plain `&` or a
  `setsid nohup ... & ); disown -a` -- a bare `&` tied to one Bash tool
  call's shell gets killed when that call returns, and `setsid
  nohup`-style detaching (an earlier approach, superseded) turned out
  to be unreliable in at least one sandboxed environment (spurious
  non-zero exit codes on the launching call, the process not actually
  surviving to the next tool call). QEMU's own `-daemonize` avoids all
  of that -- it forks, detaches, and returns control immediately, no
  shell job-control subtlety to get wrong. Hand-rolling the command
  instead of using `launch_qemu_cmd()` is also how a QMP port mismatch
  happens silently: `launch_qemu_cmd()`/`QMPSession()`/`GuiFlow()` all
  default to port 4445, but nothing stops a hand-typed `-qmp
  tcp:127.0.0.1:4444,...` from picking a different one -- the failure
  mode is a flat `Connection refused` when the session tries to
  connect, not an obviously-QEMU-related error.
- **`GuiFlow(qmp_port=4445)` constructs its own internal `QMPSession` --
  don't create a `QMPSession` yourself and pass it in.** `GuiFlow.
  __init__` takes a port number (or other `QMPSession` kwargs), not a
  session instance; passing one positionally fails with a confusing
  `TypeError` inside `QMPSession.__init__` rather than an obvious
  "wrong argument" message. Access the session it already made via
  `flow.session` (e.g. `flow.session.screenshot(...)`,
  `flow.session.recalibrate()`) instead of holding a separate one.
- **Don't chain a `pkill` with further commands in the same shell
  invocation** (e.g. `pkill -f qemu-system-x86_64; rm -f qemu.pid; ...`
  or piping its result into a launch command) -- `pkill` exits 1 when
  nothing matched (nothing to kill is the common case, not an error),
  which trips `errexit` and aborts the rest of the chain with a
  spurious-looking `exit code 144`, even though every individual
  command in it would have worked fine run separately. Run `pkill` (or
  skip it entirely and check `ps aux | grep qemu` first) as its own
  Bash call, then launch fresh in a separate call.
- **Use `-serial file:/path/to/serial.log`, not `-serial stdio`** for
  most testing (kernel boot/test output is easy to `tail`). But note
  some userland tests (`echotest`) block forever reading from the
  serial port when it's a plain file with nothing on the other end --
  that's an environment limitation of headless testing, not a kernel
  bug, if a test hangs at "calling process_run_ring3()" with no
  further output.
- **Do NOT pass `-display none`.** It disables the display head
  entirely, which silently breaks `input-send-event` mouse routing --
  clicks and moves return `{"return": {}}` (success) but never reach
  the guest. Use `-vga std -vnc :N` (no `-display none`) instead; VNC
  doesn't need an actual client connected, it just needs to exist as a
  head for input routing to work.
- **That VNC head is also how the user WATCHES a headless run live**, and
  it needs no setup or flag -- the head is always there. The display
  number is derived from the VM slot: `vm.py start` (slot 0) and
  `qmp_test.py`'s `launch_qemu_cmd()` default are `:5` (TCP 5905),
  `vm.py --instance N` is `:5+N`, and `gui_regress.py`'s four parallel
  slots are `:5`-`:8`. Any viewer works (`remmina -c vnc://localhost:5905`
  is what's installed on the maintainer's machine -- but that URI form
  cannot be view-only; use `tools/watch_vm.sh` below, which launches a
  saved profile that is). **View-only matters**: a connected viewer's real mouse motion goes into the same
  emulated PS/2 device the synthetic input uses, and the two fighting
  looks exactly like a flaky test rather than like interference.
  Attaching or detaching mid-run is free.
- **Mouse input:** this kernel's mouse driver is PS/2, not USB HID --
  never add `-device usb-tablet` OR `-device usb-mouse` to a headless
  test launch (same reasoning as `make run`'s comment above -- it's
  not just a "wrong device" ergonomics thing, it actively breaks
  routing). `QMPSession.goto()`/`click()`/`drag()` use
  `input-send-event` with `rel` axis events against the default
  emulated PS/2 mouse, tracking cursor position client-side since
  there's no absolute cursor query. `wheel()` sends synthetic
  `wheel-up`/`wheel-down` button press/release pairs -- QEMU's
  IntelliMouse PS/2 emulation reports the scroll wheel that way, there
  is no separate scroll event type.
- **Cursor position drifts across separate `QMPSession`s that share an
  already-open GUI session** (a previous script left GUI mode running
  instead of pressing Esc back to the shell). Each new session assumes
  the cursor starts at (640, 360) without querying the guest's real
  position, so if the real cursor moved since, every `goto()` lands
  offset from where it should -- looks exactly like a click "not
  registering." Fix: call `session.recalibrate()`, but ONLY in scripts
  that are reusing an already-open GUI session rather than entering it
  fresh -- and if a script does both (enters GUI mode itself, and
  wants to recalibrate), the recalibrate call must come AFTER "gui" +
  Enter, never before (the guest's mouse device isn't even enabled
  until `mouse_init()` runs as part of entering GUI mode, and that
  same call resets the cursor to screen center deterministically,
  which is why (640, 360) is the default in the first place). See
  `recalibrate()`'s own docstring for the full reasoning -- getting
  this ordering backwards was a real mistake in an earlier session,
  worth not repeating.
- **Keyboard:** `send-key` with `{"type":"qcode","data":"<key>"}`,
  one character/qcode at a time (`QMPSession.send_key()`/`send_text()`).
  `send_text()` only handles lowercase letters/digits -- for space use
  `send_key('spc')`, for punctuation the matching qcode name (e.g.
  `bracket_left`, `semicolon`, `apostrophe`, `slash`, `dot` -- not the
  literal character; `query-qmp-schema`'s `QKeyCode` enum has the full
  list). For Shift/Ctrl/Alt combos (an uppercase letter, a shifted
  punctuation key), use `QMPSession.combo(['shift', 'bracket_left'])`
  -- QMP's `send-key` presses+releases every qcode in `keys` together,
  which is exactly a held-modifier combo; there's no separate "hold
  key down" primitive. **Typing a real physical-shell command** (e.g.
  `run nx_test`) means hitting this gotcha repeatedly in one line --
  `tools/shell_flow.py`'s `ShellFlow.run_command()` does the
  character-by-character `send_text()`/`send_key('spc')`/
  `combo(['shift','minus'])` mapping for you (space, hyphen,
  underscore, and a few other punctuation chars); use it instead of
  hand-rolling the dance inline. Two real mistakes from doing it by
  hand (a dropped space, a hyphen typed where an underscore was
  needed) are what prompted building it.
- **Don't `pkill`/kill-by-pattern across ALL `qemu-system-x86_64`
  processes** if there's any chance the user has their own `make run`
  QEMU open (an interactive SDL window, not a QMP-headless one) --
  matching by process name alone can't tell the two apart, and killing
  the user's real window is a genuinely bad surprise, not just a
  failed test. Track and kill only the PID your own launch wrote to
  its `-pidfile` (`cat qemu.pid; kill <pid>`), and if you ever do need
  to sweep stale instances, `ps aux | grep qemu-system-x86_64` first
  and eyeball which ones are actually yours (a QMP-headless launch has
  `-qmp tcp:...` and `-vnc :N` in its command line; the user's
  interactive one has `-display sdl` instead) rather than a blind
  `pkill -f qemu-system-x86_64`.
- **Rapid `send_key()` calls with little/no delay between them can
  silently drop keystrokes** at the guest keyboard-controller level
  (hit testing Notepad's filename field, build 490 -- 11 back-to-back
  backspaces dropped most of them). `send_text()`'s built-in per-char
  delay covers plain typing, but a manual sequence of `send_key()`
  calls needs its own explicit `time.sleep()` (0.05-0.08s has been
  reliable) between each one.
- **`drag()` takes BOTH points and its timings are keyword-only** --
  `drag(from_x, from_y, to_x, to_y)`. It used to take a destination
  only (`drag(x, y, hold, settle)`), and a call that reasonably read as
  four coordinates silently bound `hold=700`/`settle=300` SECONDS: it
  didn't fail, it slept for sixteen minutes. The `*` in the signature
  makes that a `TypeError` now, but the lesson generalises -- a helper
  whose positional arguments can absorb a mistake as a plausible value
  is worth reshaping rather than documenting.
- **Screenshots:** `screendump` writes a `.ppm`; `QMPSession.screenshot()`
  converts to `.png` via Pillow in one call so it's ready for the Read
  tool / `SendUserFile`. It hands QEMU an ABSOLUTE path on purpose --
  QEMU resolves `screendump`'s filename against its own working
  directory, and `-daemonize` leaves that somewhere other than the repo,
  so a relative path reports `{"return": {}}` (success) and writes the
  file somewhere else; the only symptom is Pillow raising
  `FileNotFoundError` on a path that looks obviously correct.

**Screenshots are a TESTING TOOL, not a deliverable.** Take as many as
a check needs, into a scratch directory. Do NOT save them into
`screenshots/` as evidence -- that convention is retired (2026-08-15, at
the maintainer's request: the artifacts were not being used and
producing them slowed the loop down). `screenshots/` is kept for what
is already in it, not added to.

Show the user a screenshot when SEEING it is the answer -- a layout
that has to be looked at, a rendering question a number can't settle.
Don't attach one to prove a check passed: `docs/gui-guidelines.md`
already asks for pixel values with a control point (`pixel_probe.py`),
and a pass/fail table from `gui_regress.py` is better evidence than an
image, because a reader has to interpret the image and can only read
the table.

## tools/

Dev/build helper scripts, not compiled or shipped as part of the OS:
`genfont.py`/`genttf.py` (font generation, pre-existing), `gen_kbs.py`
(generates the `seed/sync/etc/kbs/<layout>` keyboard-layout data files
from Linux's own XKB data -- see `docs/decisions.md` on layouts being
data files, not a compiled-in enum), `qmp_test.py`
(QEMU/QMP GUI testing helpers, see above), `boot_smoke_test.py` (fast
non-GUI boot check, see above), `gen_version.sh`/`set_version.sh`
(versioning, see the `version.h`/`VERSION` bullets above),
`ktest_run.py` (drives the in-kernel test suite over serial and turns
it into an exit code -- what `make test` and CI run), `vm.py` (start a
headless VM and run shell commands against it, getting text back -- see
above),
`device_git.sh` (Cowork-only: wraps a `git` command run over the device
bridge with the stale-`index.lock` workaround, see "Working in the
cloud sandbox vs. directly on the user's machine" above -- not needed,
and not applicable, on a direct local checkout).

The rest, added once the build/test/delivery loop above had enough
repeated manual steps to be worth automating:
- **`backup_repo.sh`** -- a complete, verifiable backup of the GitHub
  repo: mirror clone, a bundle of LOCAL refs (catching branches never
  pushed), **every release asset**, and the repo/PR/issue metadata.
  Run it before anything that changes the repo's identity or history --
  a transfer, a visibility change, an account rename, a history
  rewrite. The reason it exists: `git clone --mirror` is NOT a backup
  here, because release assets (~130 MB of ISOs and disk images) live
  only on GitHub and cannot be recovered from a clone. It verifies the
  bundle and checksums any release shipping a `SHA256SUMS`, but does
  NOT run the restore test (clone the mirror, `make all`) -- do that by
  hand before relying on it, since matching hashes prove the bytes
  survived and only a build proves it restores to a working project.
  See `docs/decisions.md`.
- **`genrelocs.py`** -- builds the kernel's own relocation table for
  kernel ASLR: extracts every ABSOLUTE reference from a
  `ld --emit-relocs` link and emits it as a C array the second link
  pass embeds in `.krelocs` (~7,400 fixups, 29 KB), which
  `kernel/arch/x86_64/reloc.c` applies at boot. `--verify` re-derives
  the table from the FINAL image and fails the build if the two
  disagree -- run automatically by the kernel's link rule, because a
  table that disagrees with its image is otherwise a kernel that does
  not boot with nothing to read. Three traps live in the Makefile rule
  and are commented there: the shipped kernel must have its `.rela`
  sections stripped (GRUB will not boot the `--emit-relocs` image, and
  the symptom is an EMPTY serial log), `build/krelocs.c` must be a
  named prerequisite and `.PRECIOUS` or make deletes it as an
  intermediate, and `linker.ld`'s `.krelocs` must stay after `.data`.
  See `docs/decisions.md`.
- **`check_deps.py`** -- proves the build's header dependency tracking
  is actually live: touches one header per build directory (discovered
  from `build/`, not listed, so a new source directory is covered as
  soon as it's been built once), asks `make all -n` what it would
  rebuild, and fails on any directory that answers "nothing". Restores
  mtimes, so a run changes nothing. Exists because that tracking broke
  silently for the whole of `kernel/` when objects moved directories
  and nothing noticed -- a clean build can't observe a stale `.o`, so
  neither could CI. In `preflight.sh` and CI.
- **`preflight.sh`** -- one command running `make clean && make all &&
  make iso` + `check_deps.py` + `check_layout.py` + `boot_smoke_test.py` + `ktest_run.py`
  + `usertest_run.py`
  + a `git status --short` summary (`fs_switch_test.py` is NOT in it --
  that one needs a disk copy and a longer boot cycle, run it yourself
  after `kernel/fs/` changes), so
  "am I safe to deliver?" is one call instead of three run by hand.
  `--skip-clean` skips the initial `make clean`.
- **`deliver.py`** -- builds the delivery file-list/device-path/
  protected-file manifest and a commit-message skeleton for the
  shipping step (see "Delivering changes" below), from `git
  status --short` (or explicit file args) -- flags `Makefile`/
  `.github/workflows/*.yml` as PROTECTED with the `.new`-suffix
  workaround instructions before a real `device_commit_files` call
  would reject them.
- **`gui_flow.py`** -- named, composable QMP click-flows on top of
  `qmp_test.py`'s `QMPSession` (`GuiFlow` class: `enter_gui()`,
  `open_app(name)`, `run_system_action(label)`, `screenshot_named()`),
  so a testing session doesn't hand-derive Start-menu row pixel math
  from scratch every time. `APP_ORDER` must stay in sync with
  `apps/gui_apps.c`'s registry order.
- **`shell_flow.py`** -- the same idea as `gui_flow.py`, for the
  PHYSICAL (pre-`gui`) shell instead of the GUI: `ShellFlow.
  run_command(cmd, subdir=...)` types a full command -- including
  spaces/hyphens/underscores/a few other punctuation chars
  `qmp_test.py`'s `send_text()` can't handle on its own -- presses
  Enter, waits, and screenshots, instead of hand-interleaving
  `send_text()`/`send_key('spc')`/`combo(['shift','minus'])` calls
  character by character every session (a real mistake -- a dropped
  space, a hyphen typed where an underscore was needed -- happened
  twice in the session this was built in). Returns a screenshot path,
  not parsed text: this kernel's console auto-selects a framebuffer
  backend (glyphs drawn as pixels) whenever GRUB provides one, which
  is the normal case for this project's QEMU launch flags, so there's
  no legacy-VGA-text-buffer memory-read shortcut to plain text the way
  there might be on a kernel that only ever used 0xB8000 -- see the
  module's own docstring.
- **`check_layout.py`** -- verifies `disk.img`'s directory structure
  matches `docs/filesystem-layout.md`'s table, which is the source of
  truth for where things live on the OS's own filesystem. Runs in
  `preflight.sh` and CI. Fails in both directions (an undocumented
  directory on the image, or a documented-as-present one missing), and
  understands the table's "Created by" column -- a `build`-created
  directory must exist on a freshly built image, a `boot`-created one
  needn't until the OS has run. **Read that doc before adding a
  directory, a config file, or any new seeded data**: it also records
  the budgets (64-byte caller-side path buffers everywhere; the
  256-record table on TFS2-legacy images only -- TFS3, the default
  since Milestone 15, has ~590k inodes) and the `sync`-never-deletes
  trap that makes moving a seeded file need an explicit cleanup. It
  also WARNS (never fails) about orphans -- a file in a seeded directory
  that `seed/sync/` no longer places there, i.e. exactly that trap
  having already happened -- and prints the `delete` commands to fix it.
  That check found all four ring-3 GUI apps still sitting in `/tests`
  months after they moved to `/bin`, each frozen at the build that put
  them there. **It fired again in 2026-08-16, and that time the orphans
  were LOAD-BEARING**: six stale binaries at `/bin/<name>` were what
  eight GUI test tools had been spawning, long after seeding moved them
  to `/bin/wm/{system,apps,demos}/`. Deleting the orphans (the remedy
  the tool prints) turned the suite red instantly. The right repair is
  to point the tools at the CURRENT path, not to keep the stale copy --
  but the lesson generalises: **an orphan the tool reports may be
  something you are still using, so re-run `gui_regress.py` after acting
  on that warning**, and treat a test that still works after a file
  moved as evidence it is testing the old copy.
- **`usertest_run.py`** -- runs the self-checking ring-3 diagnostics in
  `/tests` (`libc_test`, `fpu_test`, `newsyscalls_test`, `file_test`,
  `write_test`, `exit_test`, `random_test`) as one pass/fail table, asserting BOTH an
  exit code and required output. In `preflight.sh`. It fills a real gap:
  `make test` runs inside the kernel and `gui_regress.py` covers the
  windowed clients, so nothing ever ran a plain `/tests` binary except a
  person typing `run <name>`. **Read its `EXCLUDED` list before adding
  to it** -- a test that faults on purpose, blocks on the serial port,
  needs a desktop, or needs a parent to spawn it will fail in a way that
  says nothing about the code under test. `pipe_test` is the worked
  example: it exits 3 under `run` because its `waitpid` finds no parent,
  and passes fine under the KTEST that spawns it properly.
- **`faulttest_run.py`** -- the ring-3 diagnostics that FAULT ON
  PURPOSE, which `usertest_run.py` correctly excludes and which
  therefore nothing ran at all. A faulting binary has no exit code and
  no output of its own, so the assertion is the KERNEL's report, read
  out of the serial log: each entry names required AND forbidden
  substrings, which is where the value is -- a stack overflow and a
  null dereference are both page faults, and every entry doubles as the
  positive control for its neighbours. Each test gets its own QEMU,
  because a ring-3 crash takes the serial debug console down with it
  (`vm.py` cannot drive these at all -- `crash_test` included), so the
  command is typed at the PHYSICAL shell over QMP instead. Not in
  `gui_regress.py`; run it after touching the fault path, the ELF
  loader, or the user address-space layout. `stack_smash_test` is
  deliberately absent -- its message goes to the process's stdout, i.e.
  the screen, so there is nothing in the log to assert on.
- **`pixel_probe.py`** -- reads exact pixel values out of screenshots,
  and tabulates the same points across several (`--compare a.png b.png
  --at 85,100 --at 215,100`), flagging which moved and which didn't.
  This is how `docs/gui-guidelines.md` says to verify a GUI change, and
  the rule exists because a hover state that shifted the background by
  TWO units out of 255 looked entirely plausible in a PNG. Always
  include a point that should NOT change -- half the assertion is the
  neighbour staying put. `--box N` averages a square, for anti-aliased
  edges where a single pixel is a coin toss.
- **`kvm_soak.py`** -- the desktop under KVM, across FRESH BOOTS, failing
  on the symptoms that appear only there: a WM frame over a threshold, a
  file that exists but will not read, an incomplete cursor-theme load, a
  varying desktop entry count. Exists because **every other test here
  runs TCG**, and on 2026-08-17 that hid three real bugs at once -- a
  lost-wakeup race in the ATA driver (5s frozen desktop per disk read),
  a 54-read desktop reload (40ms TCG / 2.5s KVM), and a non-re-entrant
  filesystem that silently lost cursor shapes on ~1 boot in 3. Fresh
  boots per round because that last one is intermittent and one clean
  run says nothing. Not in `gui_regress.py` (needs `/dev/kvm`, boots its
  own VMs); run it after touching the disk driver, the filesystem, the
  VFS, the scheduler's preemption handling, or anything the WM reads
  from disk -- and whenever a user reports something this environment
  cannot reproduce. SKIPS loudly without KVM rather than passing
  quietly. **Its workload CHURNS `/usr/wm/desktop` on purpose**: the
  desktop only re-reads when that directory changed and a cached read
  never reaches the drive, so without it the tool does almost no disk
  I/O -- verified by disarming the VFS preemption guard entirely and
  still getting four clean rounds.
- **`serial_capture.py`** -- read a running VM's serial console RAW,
  optionally sending one command first. The case `gui_debug.py` cannot
  cover: a command that KILLS the guest. `DebugConsole` is
  request/response, so a panicking kernel never returns a prompt and the
  panic block is discarded as a timeout -- and `capture_panic()` only
  helps while the console still answers. Only ONE reader may hold the
  socket, so drive input through `--gui` (QMP) or this tool's `--send`,
  never a second console. Exits 2 if the capture contains a `PANIC:`,
  because a run that caught one is not a successful test. Pipe it into
  `panic_resolve.py`.
- **`panic_resolve.py`** -- paste a panic (from the log, or typed off a
  photograph) and it names every address in it, RIP and stack scan
  alike, annotating the original lines. It finds the relocation delta
  from the text itself. **It checks the BUILD ID first and refuses to
  be quietly wrong**: resolving against a different build gives
  confident, plausible, wrong names -- verified, the address that was
  `try_merge_next` in one report is `rtc_read_local` a few commits
  later. `--elf` points it at a userland ELF for a ring-3 crash.
- **`gen_syms.py`** -- bakes the kernel's function symbol table into the
  image so a panic can name the function instead of printing an address
  nobody can resolve (the kernel relocates itself, so a raw RIP is
  meaningless without the boot log). Same two-pass + `--verify` shape as
  `genrelocs.py`; the blob is deliberately pointer-free so it costs no
  relocations.
- **`crashtest_test.py`** -- the fault paths (9 checks): the app
  enumerates the kernel's fault kinds, kernel faults are refused while
  disarmed, and a ring-3 crash kills the app WITHOUT taking the desktop
  with it. In `gui_regress.py`, which is only safe because the kernel
  half is disarmed unless `faultinject` is on the command line.
- **`gen_cursors.py`** -- generates the shipped cursor themes into
  `data/cursors/`, which the Makefile's `seed` target stages onto the
  image. **Into `data/`, NOT `seed/sync/`** -- that tree is gitignored
  and `make clean` deletes it, so the first version's themes were never
  committed and every checkout but the authoring one silently got the
  built-in fallback. It is the authoring path for a new theme too
  (a theme is a function returning shape name -> masks). It
  EXTRACTS the default arrow from `userland/wm/wm_render.c`'s own baked
  arrays and ports the procedural resize shapes, so the files on disk
  cannot drift from the built-in fallback they mirror. `--check` fails
  if they are stale.
- **`cursor_theme_test.py`** -- cursor themes end to end (9 checks): the
  theme loads completely, switching it changes the drawn pointer, the
  size setting scales it by the right MAGNITUDE, returning to normal is
  pixel-exact, and a theme that does not exist still leaves a working
  pointer. Read its docstring before editing: the built-in fallback
  means "a cursor is on screen" proves nothing, so every check asserts a
  load count or a pixel difference. It also sets both settings
  explicitly at the start -- they persist to the disk image, so
  inheriting them makes every measurement relative to a silently wrong
  baseline. In `gui_regress.py`.
- **`check_layout.py`** -- see the `docs/` section: verifies the built
  image's directories against `docs/filesystem-layout.md`, and warns
  about orphaned seeded files. Runs in `preflight.sh` and CI.
- **`dialog_test.py`** -- verifies the confirm dialog's buttons by
  PIXEL VALUE: hover moves the hovered button and leaves its neighbour
  alone, a press dragged off doesn't commit, No closes it. Three traps
  it encodes: hover needs the REAL cursor parked (use
  `DebugConsole.warp_cursor()` -- `gui move` holds for one WM iteration
  only, and `QMPSession.goto()` is open-loop and undershoots a large
  jump); don't sample the pixel under the cursor sprite; and take the
  button rects from `gui dialog --json`, not by scanning a row for
  THEME_BUTTON_BG, which only ever worked for a Yes/No dialog and
  cannot measure the wider "Force Quit"/"Wait" one.
- **`uidemo_test.py`** -- drives UI Demo's widgets and asserts on its
  log (27 checks: click selection, cancel paths, keyboard navigation,
  Tab/Shift-Tab focus cycling, Space activating a focused button,
  wheel-scrolls-without-selecting, the dropdown popup's open/commit/
  dismiss/Esc, and keyboard focus). Exits non-zero on a failed check.
  Run it after touching anything in `apps/ui/`. Geometry comes from the
  app's own `uidemo: layout ...` lines rather than from re-deriving row
  offsets in Python -- the Python copy drifts silently the moment a row
  is added to the app, which is exactly what happened when the dropdown
  and listbox rows landed mid-file.
- **`calculator_client_test.py`** -- drives the RING-3 Calculator
  (`userland/gui/calculator.c`) and asserts on it (8 checks). Worth reading
  for two techniques: it uses **no OCR** -- every check is a round trip
  (a state change must alter the display's pixels, and returning to the
  same logical state must restore them EXACTLY), which proves rendering
  and arithmetic together and also catches a right number drawn in the
  wrong place; and its last check presses a button, drags OFF it and
  releases, which must NOT commit. That one matters because a client
  acting on button-down passes every other check and fails only that.
  Geometry is derived from the window's reported content size, not
  hardcoded, so it survives a font-size change.
- **`uterm_test.py`** -- drives the RING-3 Terminal. Its key check is
  worth copying elsewhere: it distinguishes a BUILTIN (`echo hi`,
  handled inside the shell with no spawn) from an EXTERNAL program
  (`lscpu`, dozens of lines that can only appear if it was spawned and
  its stdout piped back) by INK VOLUME. A terminal that echoed commands
  but never captured output passes every other check and fails that
  one.
- **`notepad_client_test.py`** -- drives the RING-3 Notepad and asserts
  a full round trip: type, save, verify the bytes on disk via `cat` (a
  completely independent path -- the editor claiming success proves
  nothing), clear, reopen, and require the rendered text to match pixel
  for pixel. Two traps it encodes: `ls`'s output on this console is
  interleaved with kernel log lines, so parsing it needs a strict
  entry-shaped regex rather than `split()[-1]`; and a reference
  screenshot must park the caret first, since `load_file()` resets the
  cursor to 0 and a caret bar is a real pixel difference.
- **`uiclient_test.py`** -- drives `userland/tests/uiclient.c`, the ring-3
  client that renders real text with `userland/ui/ugfx.c`, and asserts on
  it (8 checks: text actually rendered, the button drew, a click and a
  key each repaint, the unchanged label comes back identical, the close
  handshake works). Two things it encodes: "text was rendered" is
  asserted as INK COVERAGE in a band rather than a single-pixel sample
  (a glyph run puts a countable number of non-background pixels in its
  rows; a blank window and a solid fill are both distinguishable that
  way), and **a client's `stdout` goes to the owning Terminal's
  scrollback, not the serial console** -- so `DebugConsole.logs()`
  can't see a client's own log lines even though a shell-spawned
  process's are visible. Run it after touching `userland/ui/ugfx.c` or
  the font-sharing path.
- **`winclient_test.py`** -- drives `userland/tests/winclient.c`, the ring-3
  client that owns a real window on the desktop, and asserts the
  windowing protocol end to end (8 checks: the window appears in the
  WM's own list at the requested size, the client's pixels reach the
  screen, a key and a click each route to it and make it redraw, the
  window behind it does NOT change, the close handshake completes, the
  desktop survives). Geometry comes from `gui windows` and content from
  PIXEL VALUES with a control point, per `docs/gui-guidelines.md`. Run
  it after touching `userland/wm/wm_client.c`, `kernel/proc/win_server.c`,
  or anything in `abi/win_proto.h`.
- **`sched_gui_test.py`** -- proves the desktop stays ALIVE while a
  ring-3 process runs, the end-to-end counterpart to
  `kernel/proc/sched_test.c`'s KTESTs. The trick it encodes: the `gui`
  debug commands are dispatched from inside `wm_run()`, so a frozen WM
  cannot answer one -- which makes "did the WM answer?" a direct
  liveness test with no screenshot to interpret. Every sample is paired
  with the WM's own `proc_pid` (`gui state --json`) so only samples
  overlapping a genuinely live process count; OVERLAP is the claim, not
  speed. Run it after touching the scheduler, `wm_run()`'s loop, or
  anything about process spawning. Both it and the KTESTs were checked
  as positive controls with the change disabled (0 overlapping samples
  there, versus a continuously responsive desktop) -- do that again
  before trusting a clean run, same reasoning as `damage_sweep.py`'s
  `--positive-control`.
- **`gfxdemo_test.py`** -- drives the Shapes demo (`userland/gui/gfxdemo.c`)
  and asserts on its log + its pixels, 13 checks. Run it after touching
  `kernel/lib/geom.c`/`fixed.c`, `uui_canvas`, or anything a ring-3
  client draws with. Three of its checks encode reasoning worth
  reusing: the window is proved to be a ring-3 client from `gui windows
  --json`'s `client_pid` rather than from how it looks; "it rotates" is
  paired with "it stops dead at speed 0", because either half alone
  proves almost nothing; and the AA toggle is checked by COUNTING
  DISTINCT COLOURS in the canvas (468 with, 5 without) rather than by
  sampling a point, since a curve moves and a fixed sample point
  doesn't follow it.
- **`scrollbar_test.py`** -- scrollbar BEHAVIOUR, against the ring-3
  Notepad: the thumb doesn't jump when grabbed anywhere on it, a drag is
  reversible, the trough pages while an arrow steps, and the strip is
  wide enough to hit. It measures the THUMB'S PIXELS (track and thumb
  are known flat colours, so a column scan gives its exact top and
  height) rather than reading text, and takes the strip's rect from
  Notepad's own `notepad: layout scrollbar` line. Written after the
  ring-3 Notepad shipped a bar that scrolled -- so every other check
  passed -- while jumping to put the thumb's top under the cursor,
  making it grabbable only by its top edge. The spec it enforces is
  `docs/gui-guidelines.md`'s "Scrollbars: what a real one does"; run it
  after touching either `apps/ui/ui_scrollbar.c` or
  `userland/ui/uui_scrollbar.c`.
- **A tool that PARKS the real cursor must un-park it.**
  `DebugConsole.warp_cursor()` is the right way to hold a hover -- `gui
  move` lasts one WM iteration -- but the cursor then STAYS there, and a
  menu opened later finds the pointer already inside it. That turned one
  check red 5/5 while its partner ("a click outside dismisses the menu")
  stayed green for the wrong reason: the menu had never opened. Park,
  measure, un-park; `menubar_test.py`'s `hover()`/`unpark()` pair is the
  worked example. This is what the long-standing menubar flake turned
  out to be -- see `docs/decisions.md`.
- **`menubar_test.py`** -- the menu bar, its nested submenus and the
  status bar (`userland/ui/uui_menubar.*`, `uui_statusbar.*`), driven
  through the ring-3 Notepad. 22 checks: the popup is DRAWN (not merely
  responsive), a title opens on press while an item commits on release,
  a press dragged off commits nothing, a click outside dismisses AND is
  swallowed rather than reaching the text, submenus open on hover and
  are placed to the right, Esc closes one level, disabled and checkable
  items behave, and the status bar's indicator tracks the cursor while
  its message pane does not. Two positive controls are recorded in its
  docstring, and they are the reusable part: commit-on-press reddens
  exactly the drag-off check, and a dismissing click that falls through
  leaves "the menu closed" GREEN and reddens only the caret measurement.
  Run it after touching either widget.
- **`forcequit_test.py`** -- not-responding detection and force quit
  (TWP's ping/pong, `scheduler_kill()`, the dialog, and the slot
  reaping). 15 checks. The design point it encodes: a client that
  REFUSES to close and one that is WEDGED are identical to a plain
  timeout, so `winclient` (declines, keeps answering) and
  `userland/tests/hangclient.c` (stops pumping on `h`) are tested
  against each other -- neither half means much alone. Its positive
  control reddens exactly one check and leaves the winclient ones green,
  which is worth reading before trusting them.
- **`blank_window_test.py`** -- opens EVERY app in the registry and
  requires its window to contain more than a flat fill. Reads the app
  list from the KERNEL (`gui apps`), so an app added tomorrow is covered
  without editing it. Exists because UI Demo shipped completely blank
  and a 35-check suite passed it: every check asserted on the app's LOG,
  and the widgets were live, hit-testable and simply never painted. In
  `gui_regress.py`.
- **`live_boot_test.py`** -- boots `toy-os-live.iso` with NO disk and
  asserts a shipped binary RUNS, plus that `df` reports the image's real
  size and says RAM-only. Not in `gui_regress.py` (it builds its own
  QEMU); run it after touching the block layer, TFS3's geometry or the
  live path. **"It booted" proves nothing here** -- the kernel degrades
  to an empty RAM filesystem and still reaches a shell and a desktop.
- **`demo_test.py`** -- boots `toy-os-demo.iso` and asserts the scripted
  tour actually PERFORMS (6 checks). **On demand only** -- do not add it
  to `preflight.sh`, `gui_regress.py` or CI (standing request: it boots
  its own ISO and the demo is a showpiece, not something an ordinary
  change breaks). Reach for it when the tour is suspect, or after
  touching `apps/demo.c`, `data/wm/demo.script` or shell dispatch/init.
  Its load-bearing check is that a PATH-RESOLVED command really reached
  `elf_run` -- the other five stayed green through a real shipped bug
  where every PATH lookup in the tour failed, because "it booted,
  reached the desktop and opened windows" is satisfied by a tour whose
  every command failed. See `docs/decisions.md`.
- **`compositor_death_test.py`** -- the compositor death path (M41's
  R7, 10 checks). Killing the compositor must not panic the kernel, and
  must not take the desktop with it. Two things it encodes. **The
  teardown is conditional on that compositor BEING the desktop** -- while
  the ring-0 WM is registered it still owns the screen, so a stand-in
  compositor leaving is a second consumer going away, not a desktop
  dying; the first version tore down live windows and `compositor_test`
  caught it as UI Demo going silent. And its positive control reddens
  exactly TWO of the ten, because the other eight are regression cover
  for stage 4a's role-clear path rather than tests of R7 -- read that
  before trusting a green run. In `gui_regress.py`.
- **`compositor_test.py`** -- M41 stage 2's raw input path to a
  registered ring-3 compositor (`userland/tests/compclient.c`), 16
  checks. Its design point: every injected input is asserted TWICE, once
  in the compositor's log and once in UI Demo's, because "the compositor
  received the click" is equally satisfied by an implementation that
  stole the input stream outright -- and stage 2's whole shape is that
  both paths run at once. Run it after touching `userland/wm/wm.c`'s loop,
  `win_server.c`'s compositor registration, or the `WIN_EV_RAW_*`
  events. In `gui_regress.py`.
- **`screen_surface_test.py`** -- a ring-3 compositor's SCREEN surface
  (M41 stage 4b): the back buffer, the clip rect, the damage box, the
  blit, the publish path and R2's verify diff, driven through
  `userland/tests/screenclient.c`. 14 checks. Run it after touching
  `userland/ui/ugfx.[ch]`, `SYS_SBRK`, or `kernel/include/kernel/
  uaddr.h`. Two things it encodes. Every geometric check asserts an
  EXACT number, not "it changed" -- a damage box that covers only the
  last rect passes any did-it-change test, and a clipped blit that
  offsets its destination but not its SOURCE draws the right count of
  pixels in the right box with the wrong contents. And its first check
  is a real gate on the ring-3 HEAP: the client reports geometry only
  if sbrk handed over a full screen of back buffer. In
  `gui_regress.py`.
- **`desktop_entries_test.py`** -- the `.desktop` entry system: the
  `ShowIn=` key and live reload, 13 checks. Its reusable lesson is in the
  ShowIn checks: they assert an entry is **LOADED but filtered** (`gui
  apps` versus `gui menu`), never just "absent from the menu" -- the
  first version asserted only absence and passed with the filter
  disabled outright, because `write` truncates and the check was racing
  the transient invalid file. In `gui_regress.py`.
- **`cpanel_test.py`** -- the ring-3 Control Panel and, through it, the
  settings registry (14 checks). Run it after touching
  `kernel/lib/setting.c`, `SYS_SETTING`/`SYS_SYSINFO`, or
  `uui_listbox`/`uui_radio_list`/`uui_statusbar`/`uui_layout`'s `hidden`
  handling. Two things it encodes. A change is verified by reading the
  BYTES ON DISK through the console's own `sh cat`, not by believing the
  app -- and note `/bin/config get` does NOT work for this, because a
  spawned program's stdout goes to its parent's pipe rather than the
  kernel log (only stderr is readable from outside). And its
  hidden-page check measures the SAME RECT in both states: the first
  version compared the widget's band against the WHOLE page's ink, a
  baseline so much larger that a positive control (making the layout
  ignore `hidden` again) reddened nothing at all. Recorded numbers:
  55% of the shown ink survives when hidden works, 98% when it does not.
- **`iso_guard.py`** -- refuses to boot a stale `toy-os.iso`, called
  from `vm.py` and `qmp_test.py`'s `launch_qemu_cmd()`. `make all`
  without `make iso`, or a `make iso` that FAILED, otherwise leaves the
  whole suite testing the previous build and reporting a clean PASS --
  which is the worst possible direction and has cost time in many
  sessions. Each source tree is checked against the artifact it feeds
  (`userland/` -> `build/userland`, not `kernel.bin`), and the seeding
  step is witnessed by `build/.seeded` rather than `disk.img`'s mtime,
  because seeding is content-hash based and a byte-identical rebuild
  correctly rewrites nothing. `TOYOS_ALLOW_STALE_ISO=1` bypasses it, for
  deliberately booting an older image -- e.g. building an earlier commit
  to prove a failure predates your work.
- **`taskmgr_test.py`** -- the ring-3 Task Manager: `uui_table`, resize
  reflow, and ending a process (12 checks). Its resize check asserts the
  table grew by ROUGHLY WHAT THE WINDOW GREW BY, not merely that it
  changed -- the bug it was written after grew the width correctly and
  the height by 16 px against 300, so "it changed" was satisfied. On its
  first run it found a pre-existing bug in `uui_listbox` (see the
  widget-`hit` trap in the widget section above).
- **`single_instance_test.py`** -- one copy of an app, and relaunching
  it raising the copy that exists (`WIN_REQ_ACTIVATE`,
  `UAPP_SINGLE_INSTANCE`; 9 checks). Run it after touching TWP's create
  path, `wm_client.c`'s window list or `uapp_run()`'s startup. Two of
  its checks are worth copying: the multi-instance CONTROL (UI Demo
  declares no app id, so two windows is the right answer there, and an
  over-eager match reddens exactly that check), and identifying the
  raised window by **`client_pid`, not by title** -- with the raise
  disabled a brand-new window is frontmost too, so the title-only
  version of that check stayed green through the positive control.
- **`gui_regress.py`** -- runs every GUI test tool, each against
  its own freshly-copied disk image and its own VM, and prints one
  pass/fail table (~1.5 minutes, ~300 checks across 23 tools). This is the standard check
  after touching `apps/ui/`, `userland/`, or anything the WM draws.
  Tools are **STARTED longest-first** (`COST_S`/`pick_order()`), because
  a parallel run cannot end before its slowest member does and
  `forcequit` (71s) used to sit eleventh of fourteen and finish alone --
  that sort alone took a run from 1:56 to 1:30. The summary table is
  still printed in declared dependency order; only the start order
  changed, and a tool missing from `COST_S` is assumed SLOW so a new
  one can never become the straggler by omission.
  `-k NAME` for a subset, `--logs DIR` to keep each tool's full output,
  `--list` to see what's in it. The per-tool fresh image and fresh VM
  are the parts that matter: several tools write files, and every one
  of them expects an empty desktop -- a tool inheriting the previous
  one's state fails in ways that look exactly like real widget bugs.
  It runs **four tools at a time** (`-j N` to change, `-j1` for the old
  serial behaviour -- that took 107s), each in its own **VM slot**:
  `vm.py --instance N` derives that VM's pidfile, serial socket, QMP
  port and VNC display from one number, and the slot is LEASED for as
  long as the VM lives rather than derived from the tool's position in
  the list. Use `--instance` yourself any time you need a second
  headless VM alongside one that's already up; slot 0 is the plain
  `.vm.pid`/`.vm.serial`/4445 every existing caller assumes.
  `damage_sweep.py` is deliberately NOT in it (much slower under
  `gui damage verify on`, and it has its own `--positive-control`
  protocol) -- run that separately.
- **`uapp_test.py`** -- the TWP resize handshake and focus events, via
  `winclient` (which contains no resize code -- it sets
  `.flags = UAPP_RESIZABLE` and nothing else, so what is under test is
  Toykit's and TWS's). 8 checks. Its focus check is a ROUND TRIP:
  capture a Terminal's content focused, take focus away and require it
  to CHANGE, give focus back and require it to match the first capture
  EXACTLY -- "it changed" alone is satisfied by almost anything.
- **`damage_sweep.py`** -- drives the WM through the interactions that
  historically break the damage invariant with `gui damage verify on`,
  and exits non-zero on a violation. Run it after touching anything
  that draws, damages, focuses or changes window chrome. `--random N
  --seed S` adds a seeded random walk (the seed prints on every run, so
  a failure replays exactly); `--positive-control` inverts the exit
  code, for proving the harness detects a real violation before
  trusting a clean run -- a clean sweep otherwise can't be told apart
  from a sweep that isn't checking anything, which has happened here
  for real.
- **`flake_hunt.py`** -- one GUI tool run N times, reporting which
  CHECKS failed and how often (`python3 tools/flake_hunt.py menubar -n 6
  --keep /tmp/flake`). The sibling of `damage_hunt.py`: that one varies
  a SEED, this one varies nothing and asks whether a tool is
  intermittent. Reach for it the moment a tool fails once and passes on
  re-run -- a rate is the diagnosis, a verdict is not, and this repo has
  a recorded case of a real bug coming back clean six times before
  reproducing five times running. Scores a run that never printed a
  summary as `error`, not `pass`: a run that measured nothing must not
  look like a good one. Also the way to check a fix -- and to catch a
  fix that starts a DIFFERENT check failing, which is what happened when
  the menubar flake was fixed.
- **`damage_hunt.py`** -- `damage_sweep.py` over MANY seeds, a fresh
  disk copy and its own `vm.py --instance` slot each, as one pass/fail
  table; non-zero if any seed violated the invariant. One seed is one
  ORDERING, and this bug family lives in orderings, so "does any of a
  batch fail" is the question worth asking -- and the four-line shell
  loop that answers it had been written from scratch twice, getting the
  slot/`--sock`/`--qmp-port` triple wrong each time. **A seed reports
  `pass`, `fail` or `error`, and the third one is load-bearing** -- a
  sweep that crashed (a guest too slow to accept a QMP connection at
  high `-j`, a serial socket dropping mid-run) measured NOTHING, and
  this tool used to score exactly that as a PASS. `-j` still defaults
  to 1, but for a plainer reason than before: each slot boots its own
  guest, and four booting at once is enough to lose two of them. Not in
  `gui_regress.py`, same reason `damage_sweep.py` isn't. The earlier
  "parallel VMs report a violation `-j 1` doesn't" claim is retired --
  it failed to reproduce six times, and the mechanism that made `-j 2`
  special was this tool putting 9 GB of tmpfs behind each slot (see the
  next bullet).
- **A copy of `disk.img` must stay SPARSE, and `shutil.copyfile` does
  not.** The image is ~4 MB of data in a 9 GB sparse file, so a
  hole-filling copy costs 9 GB -- of RAM, when the destination is
  `/tmp` on a tmpfs. That silently turned `damage_hunt.py -j N` into
  "N x 9 GB of host memory pressure" and killed `-j 4` outright with
  ENOSPC. Use `cp --reflink=auto --sparse=always` (what the tool does
  now); `cp --reflink=auto` is already what this file recommends
  elsewhere for the same file.
- **`watch_vm.sh`** -- attach a VIEW-ONLY VNC viewer to a headless VM,
  so a run can be watched live without interfering with it.
  `tools/watch_vm.sh [slot...]`; the display derives from the VM slot
  exactly as `vm.py --instance N` does (slot N is `:5+N`, TCP
  `5905+N`). View-only is the point, not a preference -- a connected
  viewer's real mouse motion goes into the same emulated PS/2 device
  the synthetic input uses, and the two fighting looks exactly like a
  flaky test. Remmina's quick-connect URI (`remmina -c
  vnc://localhost:5905`) has NO view-only option, so this writes a
  saved profile with `viewonly=1` and launches that instead, which is
  the whole reason it's a script rather than a line in this file.
- **`screenshot_diff.py`** -- Pillow-based pixel diff between two
  screenshots with a pass/fail `--threshold` (default 0.2%) and an
  optional `--out` diff-highlight image, for catching a rendering
  regression manual eyeballing might miss.
- **`tfs2_writer.py`** -- host-side TFS2 v3 read/write tool: get files
  onto (or off of) `disk.img` without booting toy-os. **`trim` returns
  every free block's space to the host** by punching holes through them
  -- run it if `du disk.img` ever looks large. The image is sparse when
  created and only ever loses that: a block written once stays allocated
  on the host even after toy-os deletes the file that owned it, and the
  dev image had reached 8.1 GiB actually allocated against 2.3 MiB in
  use before this existed. The kernel issues ATA TRIM as it frees blocks
  now (`ata_trim()`, plus `discard=unmap` on every `-drive` line), which
  stops new images getting there; `trim` is for images already in that
  state, and for the host-side seeding path, which never boots the
  kernel. Non-destructive: only blocks the filesystem already considers
  free are touched. `format`
  initializes a blank/foreign image as an empty TFS2 v3 filesystem --
  note that running it on a BLANK image opts that image out of the
  TFS3 default; that's `seed_disk.py`'s job to decide, not a thing to
  do casually; `write`/`read`
  for a single file; `ls` for a directory listing; `sync <seed-dir>` to
  mirror a whole seed tree in (`once/` = copy-once, `sync/` =
  content-hash-synced -- see its own docstring and
  `docs/decisions.md`). `write`/`sync` auto-format a blank image first
  (no-op if already formatted), so a completely fresh `disk.img` can be
  seeded in one call with no toy-os boot in between -- the Makefile's
  `seed` target (runs on every `make iso`) goes through
  `tools/seed_disk.py` now, which delegates here only when the image's
  magic says TFS2 (a fresh/blank image gets TFS3 -- see
  `tfs3_writer.py` below). This replaced the old boot-time
  `BIN_BOOTSTRAP`/GRUB-module install (removed from `kernel.c`/
  `grub.cfg` -- see `docs/decisions.md`). Writes in-place by default;
  `--dry-run` on `write`/`sync`/`format` previews without touching the
  image. `delete`/`mkdir`/`cp` manage paths inside the image, so test
  state can be set up and cleaned up entirely from the host rather than
  booting toy-os to type `rm`. Scoped to direct+single-indirect blocks (~4.03 MB/file) -- see
  `docs/decisions.md` for why. `corrupt` injects a KNOWN inconsistency
  (`--leak N`, `--free-referenced N`, `--bad-pointer PATH`) so the
  kernel's `fsck` can be tested against damage whose exact shape is
  known in advance, and `--stage-journal PATH` (+ `--stage-journal-torn`)
  leaves an image in the state a crash mid-`persist_record()` produces,
  which is the only way to exercise `replay_journal()` without an actual
  power loss -- the inconsistencies `fsck` repairs are ones the
  kernel deliberately avoids producing, so without this it could only
  ever be tested against a clean disk and proven to report "clean".
- **`tfs3_writer.py`** -- the TFS3 sibling of `tfs2_writer.py`: format
  (writes superblock backups + GDT snapshots, wipes a stale TFS2
  signature per the wipefs rule, keeps images sparse by skipping/
  hole-punching the zeroed inode tables) / ls / read / write / mkdir /
  delete / sync (`once/` + `sync/` convention) / trim / info /
  corrupt (`--leak`, `--free-referenced`, `--bad-link-count`,
  `--smash-superblock`, `--stage-journal[-torn]` -- known damage for
  fsck/backup/journal-replay testing, same reasoning as
  tfs2_writer's). Spec: `docs/tfs3-spec.md`; the kernel backend
  (`kernel/fs/tfs3.c`) is kept in lockstep and the same bar applies
  as tfs2_writer's: direct+single-indirect write scope only.
  **`format --fs-version {1,2}`** picks the on-disk layout: v2 (32
  journal slots, GDT at 42, group 0 at 58) is the default and what a
  fresh image gets; v1 (four slots, GDT at 14, group 0 at 30) exists so
  the layout the kernel still mounts stays PRODUCIBLE and therefore
  testable -- `tools/tfs3_v1_test.py` is its caller.
  **TFS3 is the default format for FRESH images** (blank-disk policy
  in `vfs.c` and `seed_disk.py`); an existing TFS2 disk.img keeps
  mounting as TFS2 -- `make clean-disk && make iso` is the deliberate
  move. Use whichever writer matches the image's magic (both refuse
  the other's images; `trim` before gzipping a release image means
  the MATCHING tool's trim).
- **`seed_disk.py`** -- the format-aware seeding front-end the
  Makefile's `seed` target calls: probes the image's magic, delegates
  `sync` to the matching writer, and formats a blank image with the
  default (tfs3) -- the same policy the kernel's blank-disk path
  applies at boot.
- **`tfs3_v1_test.py`** -- boots a freshly built TFS3 **v1** image and
  proves the kernel still mounts and uses the older on-disk layout (8
  checks). Run it after touching TFS3's geometry, journal, or any
  operation's credit count. It exists because v2 made v1 support
  simultaneously untested AND untestable -- once `format` wrote v2,
  nothing in the repo could produce a v1 image at all, so
  `tfs3_writer.py format --fs-version 1` was added alongside it. Same
  rule as `ata nodma` keeping the PIO path reachable: a fallback
  nothing can reach is a guess. Its load-bearing check is that a
  cross-parent DIRECTORY move is refused there (v1's four journal slots
  cannot hold the five-block transaction) and that the refusal changed
  NOTHING -- the only end-to-end view of the credit reservation.
- **`fs_switch_test.py`** -- boots a COPY of disk.img and proves the
  multi-backend story end-to-end: probe mounts the image's own
  format, `fsformat` live-switches both ways (wipefs rule included),
  writes work on each side, files survive reboots, fsck ends clean.
  Run it after touching anything in `kernel/fs/`; it exercises the
  probe/format/remount/reboot cycle no KTEST can (the suite runs
  inside one booted kernel).
- **`mkpart_test.py`** -- writes a synthetic legacy MBR or GPT partition
  table onto a disk image, for testing `kernel/drivers/partition.c`'s
  parser (`parttable` shell command). Its mount-preserving guarantee
  was designed for (and verified against) TFS2 images; a TFS3 image
  deliberately leaves its first 32 KiB untouched for exactly this, so
  coexistence is by-design there, but the tool hasn't been re-verified
  against one -- check before trusting it on TFS3. TFS2-mount-preserving: patches
  only the partition-table byte ranges TFS2 itself never touches
  (reads the existing LBA 0 sector first rather than blindly
  overwriting it), so the real filesystem underneath still mounts
  normally afterward instead of `tfs_init()` seeing foreign magic and
  auto-reformatting. `--mbr`/`--gpt`; see its own docstring for the
  CRC32/GUID-encoding details and `docs/decisions.md` for why GPT
  verification needed a host-compiled unit test instead of a live
  boot (TFS2's own journal header collides with the GPT header's LBA).

- **`run_release.sh`** -- standalone QEMU launcher shipped as a GitHub
  Release asset (not part of the build), for running from just a
  release download with no checkout. Gunzips `disk.img.gz` if needed,
  boots with `make run`'s same device/display flags. When cutting a
  release: rebuild `disk.img` fresh (`make clean-disk` first), then
  `gzip -k -9 disk.img` before attaching it -- it's a large SPARSE file
  (9GB apparent, ~2MB of real data on a freshly-trimmed image), and
  GitHub's 2GB-per-asset limit plus plain bandwidth sense both rule out
  the raw file. Run the matching writer tool's trim
  (`tools/tfs3_writer.py trim disk.img` for a fresh-built image,
  `tfs2_writer.py` for an old TFS2 one) before gzipping
  -- sparseness is only ever lost, and an untrimmed image compresses
  whatever stale data it is still carrying. See
  `docs/decisions.md`'s versioning entry for the full v0.0.9 writeup.

Add new tools here freely when something would save a future session
real time -- the bar is "does this fix a rederive-from-scratch cost,"
the same reasoning that produced all of the above.

## docs/

`docs/decisions.md` -- short, topic-indexed answers to "why does
toy-os work this way?" for the handful of decisions that come up again
once code has grown around them (e.g. "why does the VFS run one
ACTIVE backend (probe-selected), not mount points", "why doesn't
`fs_delete` recurse"). Deliberately a
*pointer* file, not a second copy of the reasoning: each entry is a
couple sentences plus a link into the relevant changelog section
(`CHANGELOG.md` for the semver era, `CHANGELOG-archive-2.md` for Build
183-502, `CHANGELOG-archive.md` for anything older) or
source file, not the reasoning itself restated. It opens with a
grouped index of every entry -- add a line there when adding an entry,
or the index silently stops being one. README.md/
apps/README.md already cover architecture in depth and the three
changelog files together are the full chronological history with
rationale -- `docs/decisions.md` exists because neither is indexed by
topic, so "why is X built this way" otherwise means scrolling/
searching the whole history.
`docs/filesystem-layout.md` -- what lives where on the OS's own disk
(`/bin` vs `/tests` vs `/usr/share` vs `/etc`), the rules for adding to
it, the deliberate divergences from the FHS, and the budgets that constrain it
(64-byte caller-side paths everywhere; the 256-record table on
TFS2-legacy images only). Not advisory: `tools/check_layout.py` reads its table and fails
`preflight`/CI if the built image disagrees, in either direction. Read
it before adding a directory, a config file or any seeded data.

`docs/commands.md` -- the full shell command reference, grouped the way
`help` groups it. Lives here rather than in `README.md` so the README
stays a landing page; update it when adding a command.

`docs/boot-flags.md` -- every word the kernel looks for on the GRUB
command line (`nokaslr`, `nopat`, `rammeter`, `live`, `demo`), what each
does, and how to set one without rebuilding. Matching is by SUBSTRING
with no parser, spread across five files with no registry, so this table
is the only list of them -- add a row when adding a flag.

`docs/gui-guidelines.md` -- how the GUI is supposed to look and behave:
the four `enum ui_state` interaction states and their flat (non-bevelled)
rendering, the press-then-commit-on-release rule every control follows,
when feedback IS and ISN'T wanted, the `on_hover` contract, text/layout
budgeting, and how to verify a GUI change properly. Read it before
touching anything drawn.

Forward-looking "not built yet" items belong in `docs/roadmap.md`
instead (already actively maintained, with completed items struck
through and linked to the CHANGELOG build that finished them) -- don't
duplicate that list here or start a second one.

When you resolve a "wait, why is this built this way" question during
a session (by reading CHANGELOG.md, a source comment, or by asking the
user), consider whether it's the kind of question a future session
would hit again -- if so, add a short entry to `docs/decisions.md`
pointing at the answer, the same judgment call as `tools/`'s "does
this fix a rederive-from-scratch cost" bar.

## The session workflow skill lives IN this repo

`.claude/skills/toy-os-feature-workflow/` -- the end-to-end playbook a
session follows (research first, offer real choices, build and test with
proof, write the docs, ship). It used to live in `~/.claude/skills/`,
outside version control, which meant ~1,750 lines of accumulated project
knowledge had no history, no diff review and no backup. It is tracked
here now for the same reason everything else is.

Two consequences. **Update it in the repo**, not in the home directory,
or the two copies drift and the untracked one silently wins. And
`.gitignore` excludes `/.claude/worktrees/` specifically rather than all
of `.claude/`, because those are transient checkouts while the skill
beside them is real content.

## Delivering changes

List every file added or edited in the final response, as a compact
list (standing project instruction) -- always, regardless of mode.

Never write personal information (PII) into any file being edited or
added. If a change genuinely seems to need some, ask first, or
anonymize it and say so plainly.

Any genuinely reusable tooling built or used during a session (a
helper script, a test harness) belongs in `tools/`, not left as a
scratch/one-off -- see `## tools/` above for the bar ("does this fix a
rederive-from-scratch cost"). Update the files that describe `tools/`
(this file at minimum) to match when something's added there.

How the change actually reaches the user depends on which mode this
session is in (see "Working in the cloud sandbox vs. directly on the
user's machine" above for how to tell):

- **Cowork/device-bridge:** deliver files via `SendUserFile` +
  `mcp__remote-devices__device_commit_files` to the user's real
  checkout -- editing only the cloud sandbox copy doesn't reach the
  user's machine on its own.
- **Direct local checkout:** the files are already on the user's real
  checkout -- there's nothing to "deliver," just commit (and push, if
  asked) directly with plain `git`.
