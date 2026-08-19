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

## Before you edit

- **Pull first.** This checkout is worked on from more than one session.
- **Read in this order**, and stop when you have what you need: this
  file (conventions and traps), `docs/decisions.md` -- **its INDEX
  first**, which is what makes it usable at all -- then
  `docs/roadmap.md` for whether the thing is already known broken.
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
  rasteriser.** Seven headers, all reachable through `kapi.h`, all with
  KTESTs:
  - `string.h` -- strings/memory/char classes.
  - `knum.h` -- numbers <-> strings (`k_utoa`/`k_itoa`/`k_htoa`,
    `k_parse_u32`/`k_parse_hex`).
  - `kfmt.h` -- `k_snprintf`, plus `vga_printf`/`klog_printf` for a whole
    line in one call. **One header but TWO files**: `kfmt.c` is
    freestanding and shared with ring 3, the kernel sinks live in
    `kfmt_print.c`; a kernel include in the former silently takes
    `snprintf` away from userland.
  - `kpath.h` -- `k_path_join`/`_normalize`/`_resolve`/`_basename`/
    `_dirname`.
  - `fixed.h` -- Q16.16 fixed point and trig. **Angles are in TURNS, not
    radians**, so `FX_ONE` is a full rotation and `fx_sin(FX_ONE/4)` is
    exactly 1. There is no floating point in this kernel (`-mno-sse`).
  - `geom.h` -- `geom_line`/`_polyline`/`_ellipse`/`_circle`/
    `_fill_ellipse`/`_rotate`/`_transform`, each taking `GEOM_ALIASED` or
    `GEOM_AA`, **plus a small 3D section**: `geom_pt3`, `geom_rotate3`
    (yaw then pitch then roll, fixed order because rotations don't
    commute), `geom_project` (eye at `-dist`, denominator clamped so a
    point at the eye can't divide by zero or flip through the origin)
    and `geom_transform3`, which hands back each vertex's ROTATED depth
    so a caller can shade or sort by distance. Deliberately NOT a 3D
    engine -- no matrices, faces, depth buffer or clipping planes.
  - `rubberband.h` -- `rb_begin`/`rb_motion`/`rb_end`, a fixed-bitset
    selection set, RB_REPLACE/ADD/TOGGLE. Owns the band, the selection
    and the modifier rules; the caller supplies item geometry through a
    `struct rb_ops` and does its own drawing. Spec is
    `docs/gui-guidelines.md`'s "Rubber-band selection".
  - `krandom.h` -- `krandom_u64`/`krandom_bytes`, RDSEED/RDRAND with a
    TSC-jitter fallback. **Deliberately NOT a CSPRNG, and
    `krandom_quality()` is how a caller finds that out** instead of
    assuming. The stack canary is randomized from it at boot -- read
    `kernel/lib/stack_protector.c`'s comment before moving that call,
    since changing that global at the wrong moment panics innocent code.

  Two things about the drawing pair before extending either: **draw
  through the `gfx_draw_line()`/`gfx_draw_circle()`/`gfx_fill_ellipse()`
  wrappers in the kernel and `uui_canvas` in ring 3**, not `geom_*`
  directly (both handle the plot callback; the canvas also clips); and
  `geom.c`/`rubberband.c` are **compiled TWICE from one source**, kernel
  and userland, so neither may reference anything kernel-only. `geom.h`
  draws through a **callback**, never into a framebuffer, which is what
  lets one implementation serve the kernel, a ring-3 app and a test with
  no display at all.

  Two conventions to match in anything added here: a formatter that
  doesn't fit its buffer writes NOTHING rather than a truncated (i.e.
  wrong) value, and a parser REJECTS rather than guesses. Adding follows
  this file's usual bar -- **a second real caller, not a plausible one**
  (`k_strstr`/`k_strcasecmp`/`k_toupper` were written, found no caller,
  and were deleted before landing; all three came back later once one
  turned up). It exists because a survey found the same twenty lines
  written nine times for int->string and three for path resolution --
  and the path copies DISAGREED, so `edit ../x` meant different things
  in the GUI Terminal and the physical shell.
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
- **A PAGE THAT CAN OVERFLOW GOES IN A `uui_scrollview`, and the
  chrome stays outside it.** `uui_layout` does not shrink children
  below their natural size -- given too little room it OVERFLOWS,
  placing the last ones past its bottom edge with no scrollbar and
  nothing to say they are gone. Control Panel shrunk below its content
  lost its status bar entirely and left six of seven timezones
  unreachable. Wrap the page's layout in `uui_scrollview`
  (`UUI_FILL_W | UUI_FILL_H`) and the app writes no scrolling code at
  all. Three things to know. **Keep tabs and status bars OUTSIDE the
  scroll view** or they scroll away. **One scroll region per page** --
  a listbox inside one is laid out at full height and has nothing left
  to scroll itself, which is what you want. And **a `UUI_FILL` child
  now absorbs a SHORTFALL as well as leftover space**, which is what
  stops a too-tall child evicting its siblings; a container with
  nothing stretchable still overflows. Containers declare their items
  through `uui_widget_ops.children`, and a container with a `hit`
  CLIPS them (that is how a scroll view stops off-screen rows being
  clickable) while one without -- a plain layout -- does not. See
  `docs/decisions.md`.
- **Anything drawn follows `docs/gui-guidelines.md`.** Six things bite
  most often:
  1. **`gfx_draw_string()` does not clip** -- use
     `gfx_draw_string_clipped()` and `gfx_text_width()` for anything in
     a fixed box. This caused the identical overlap bug in two files,
     the second written days after the first was documented.
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
     change: the whole UI reflows and the GUI suite passes unchanged.
     What does NOT reflow is a hardcoded pixel constant in a test tool
     -- prefer `DebugConsole.menu_row(label)` over `gui_flow.py`'s
     calibrated numbers, which have needed re-measuring three times.
  6. **An app cannot draw outside its own window, and that is enforced**
     -- the WM clips to the content area around every `on_draw()`
     (`clip_to_window_content()`), and a ring-3 client draws into its
     own buffer with no mapping of anything else. The explicit opt-out
     is `gfx_clear_clip_rect()`, lasting only for that paint. UI Demo
     overdraws on purpose and `uidemo_test.py` asserts the marker colour
     never reaches the screen, so the boundary is tested, not assumed.

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
  form is this file's existing rule: ask what a broken version would
  still pass.
- **A positive control can turn nothing red because the test's DATA
  never reached the code under test.** The truncate tests wrote 16 KB,
  which fits TFS3's twelve DIRECT pointers, so disabling the
  indirect-table handling outright changed no result -- the tests were
  green, thorough-looking, and blind to that whole path. The general
  form: when a control fires nothing, suspect the fixture before the
  harness, and ask what input size/shape actually reaches the branch.
  This is the same family as the three GUI ways above, and the fix was
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
  width. **`malloc`/`free`/`calloc` DO exist now**
  (`#include "lib/stdlib.h"`), and they are not a second allocator:
  they are `kernel/lib/heap_core.c` -- the kernel's own free list, with
  its red-zones and poisoning -- compiled a second time with `SYS_SBRK`
  behind it instead of the frame allocator (`api/heap_os.h`). Two things
  a caller inherits from sbrk: **`free()` never returns memory to the
  kernel** (the break cannot move down, so a process's footprint only
  grows), and a fresh region's pages arrive on touch. What still does
  NOT exist, on purpose: `realloc`, `FILE`, `printf`, `errno`, TLS
  (Milestone 24).
  Three traps are documented in `docs/decisions.md` and in the files
  themselves, all of which fail quietly: a header named `string.h`
  including `"string.h"` finds ITSELF (hence the `<>`), an archive
  member cannot be named `string.o` twice (hence `cmem.c`), and
  `USERLAND_CFLAGS`'s `-fno-tree-loop-distribute-patterns` is what stops
  a real `memcpy` recursing into itself through `k_memcpy` -- it LINKS
  and blows the stack at runtime. Adding to these headers follows the
  usual bar: a second real caller.
- **RING-3 CODE HAS A FRAME BUDGET NOW, and a link-time bound on the
  image.** `USERLAND_CFLAGS` carries `-Wframe-larger-than=2048` (the
  kernel has had 1024/2048 since kernel stacks got guard pages) and
  `userland/rt/link.ld` `ASSERT`s that the image stays below
  `UADDR_HEAP_BASE`. Both found something the day they were added: four
  `struct dirent` arrays on the WM's stack, the worst at **20,608
  bytes against a 16 KiB stack** -- which does not merely overflow, it
  steps clean OVER the single 4 KiB guard page into unmapped space
  (the Stack Clash shape; Linux widened its guard gap to 256 pages in
  4.11 for this). They are `static` now. **A big local array in ring 3
  is the thing to look for**, and note the warning names the function
  where a wider guard would only hide it.
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
  a callback and stores none of them**: Toykit had no allocator when this
  was written (it has `malloc` now, which changes nothing here -- see
  below), and
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
- **The GUI stack has names -- use them.** **TWP** (Toy Window Protocol,
  `abi/win_proto.h`) is the client<->server contract; **TWS** (Toy Window
  Server, `kernel/proc/win_server.c` + `userland/wm/wm_client.c`)
  implements it; **Toykit** (`userland/ui/`) is the client toolkit an app
  programs against -- roughly Wayland, its compositor, and GTK. Three
  names rather than one because the protocol is meant to outlive this
  server. Symbol prefixes are unchanged and stay that way (`uui_`,
  `ugfx_`, `uapp_`, `WIN_REQ_*`); a toolkit's name and its prefix need
  not match. See `docs/decisions.md`.

  **How a TWP message is CARRIED is its own seam** -- `struct
  win_transport` (`kernel/include/kernel/win_transport.h`), with
  `SYS_WIN_REQUEST` as one implementation rather than the only path. Two
  things follow. The `gui` debug commands are protocol messages
  (`WIN_REQ_DEBUG_CMD`/`WIN_EV_DEBUG_OUT`), so `debug_console.c` does
  NOT call into `userland/wm/` -- add a new `gui` subcommand in
  `wm_debug.c` as before, but write its output through its `struct
  dbg_out` sink, **never `klog_write()`** (a stray klog call still
  reaches the serial port, so it silently vanishes from the reply). And
  the seam has exactly ONE implementation, which by this repo's own
  unreachable-path rule means it is UNVALIDATED -- see
  `docs/decisions.md` before leaning on it.

  The migration that produced these names is DONE: the desktop is a
  ring-3 process and `docs/wm-ring3-design.md` is now history rather
  than a plan. **`tools/vm.py --vga vmware` is how you reach the
  modesetting driver at all** -- the default `std` adapter has neither
  modesetting nor a cursor plane, same shape as `--cpu max` for
  SMEP/SMAP. The hardware cursor is switched off on the only driver that
  has one (`vmsvga`'s `g_cursor_enabled = 0`, because a hw cursor over a
  relative PS/2 mouse makes the pointer jump), so it is unreachable on
  every configuration this OS boots.
- **The kernel's idle work has ONE owner: `scheduler_idle()`**
  (`api/scheduler.h`). Any loop that is waiting rather than working
  calls it -- the physical shell's key wait, `wm.c`'s event loop, a
  long `cat`, the demo's timer. What it owns today is
  `debug_console_poll()`, and the reason it exists is that the serial
  debug console had no owner at all: it was polled by whichever loop
  happened to be running, and the WM's copy is the load-bearing one,
  because all 23 GUI tools and their ~300 checks arrive over that
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
  It says something changed, never what. Its load-bearing point: all 23 GUI test tools drive the WM
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
  which is still the right shape now that ring 3 HAS `malloc`: a declared
  tree needs no teardown and cannot leak. Per-item checked/disabled state is
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
  controls are in the commit that added them.
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
- **`meminfo audit` COMPARES PAGE TABLES AGAINST THE ALLOCATOR, and it
  is the check that would have caught both memory bugs of 2026-08-18.**
  The invariant: every frame a live mapping points at must be one pmm
  considers HANDED OUT. A mapping of a free frame is memory the
  allocator may give to somebody else while the process is still using
  it -- and it costs nothing until that happens, which is exactly why
  nobody noticed. `api/mm_audit.h` (`mm_audit_report()`) walks every
  live address space; `vmm_audit_space()` does one. Three counters are
  descriptive and one is a bug: **`dangling`**. Two things to know.
  **`unmanaged` is NORMAL, not a finding** -- a framebuffer is MMIO, not
  RAM pmm ever accounted for, and the desktop legitimately shows ~900
  such pages. And **borrowed pages are audited too, on purpose**: a
  borrowed mapping whose real owner freed the frame is precisely the
  use-after-free worth catching. **It does NOT find ordinary leaks** (a
  used frame nothing references) -- page tables, the heap, the kernel
  image and DMA buffers all hold frames no page table points at, so that
  direction needs every owner to declare its frames; see
  `docs/roadmap.md`.
- **THERE IS A PROCESS TREE NOW: `ppid`, reparenting, and
  `waitpid(-1)`.** `struct sched_process` had no parent link at all
  until 2026-08-18, so there was no tree to walk and no way to ask
  "has any child of mine died" -- the question an init's whole main
  loop is. Stage 0 of `docs/init-design.md`. Four things to know.
  **ppid 0 means the KERNEL spawned it** (`scheduler_current_pid()` is
  0 in kernel context), which is every process started by `spawn`,
  `gui` or a KTEST. **A dying process's children are reparented to 0
  rather than left naming it**, and that is correctness rather than
  tidiness: a pid is a slot index plus one and slots are reused, so a
  stale ppid makes the orphan look like a child of whatever process
  gets that slot next, and THAT process's `waitpid(-1)` would hand it
  somebody else's corpse. **`waitpid(-1)`'s two negative answers are
  different**: -1 means "no children at all" and is PERMANENT, while a
  live-but-not-dead child blocks (or answers `SYS_RETRY` under
  `SYS_WNOHANG`) -- an init that conflates them either spins forever or
  stops reaping. And **wait-any cannot work under the shell's `run`**:
  the legacy loader is not a scheduled process, so it has no pid,
  so nothing it spawns has a parent. Use `spawn`, which goes through
  the scheduler. `scheduler_reparent()` is the adoption half, which
  stage 1 uses to hand orphans to init.
- **THERE IS AN INIT NOW, IT HOLDS PID 1, AND IT CANNOT BE KILLED.**
  `/bin/init` (`userland/bin/init.c`) is spawned from `kernel_main()`
  before anything else, which is the only reason it is pid 1 -- slots
  are handed out lowest-first, so being FIRST is what makes it so, as
  on Linux. Stage 1 of `docs/init-design.md`. Five things to know.
  **`kill 1` no longer restarts the desktop**: find the `toywm` pid with
  `ps` and kill that. **A boot with no `/bin/init` is supported and
  quiet** -- `scheduler_init_pid()` stays 0, orphans stay parentless as
  before, and everything that treats init specially ASKS for the pid
  rather than testing `pid == 1` (see `docs/decisions.md` for the boot
  that would otherwise have an unkillable desktop). **Adoption only
  covers orphans**, i.e. children of a parent that DIED -- a live parent
  that never waits still leaks its zombies, which is why the shell's
  `spawn` reparents to init explicitly and why `gui` and the KTESTs
  deliberately do not. **init is BLOCKED whenever it is idle**, never
  spinning; if `ps` ever shows it ready, something has regressed to a
  poll loop.
- **INIT STARTS AND SUPERVISES THE DESKTOP, and the desktop is a
  SERVICE.** `/bin/init` reads `system.default_target` -- `text` or
  `graphical`, persisted in `/etc/toyos.conf` -- and starts every
  descriptor in `/etc/services.d` whose `Target=` matches.
  `data/etc/services.d/toywm` is the one real service; its README is the
  format. Seven things to know:
  - **`target=text` on the GRUB line overrides the setting for ONE boot
    and does not write the file** -- the escape hatch for a desktop that
    faults at boot. It shows as a live-vs-stored difference in `config
    diff` rather than silently reconfiguring the machine. **`config
    reload` DISCARDS the override**, since it re-reads every file; that
    is the one place the two can diverge, and it is deliberate.
  - **A dying desktop is restarted with a doubling backoff (0/250/500/
    1000/2000 ms) and a crash-loop give-up** after five consecutive
    failures that each ran under two seconds. The threshold is on how
    long it RAN, so killing it by hand twenty times never gives up while
    a binary that cannot start is abandoned in four seconds.
  - **`Restart=on-failure` is the DEFAULT, and a clean exit means stop**
    -- the Start menu's *Exit to shell* returns 0, and under an
    unconditional `always` that menu item silently did nothing. `always`
    and `no` are the other two values.
  - **The rescan happens when init WAKES, not immediately**: it blocks
    in `waitpid(-1)`, so a change to `/etc/services.d` is seen the next
    time a child exits (or within 250 ms with no children). REMOVING a
    descriptor then killing the service works -- the kill is the wake-up
    -- while ADDING one while the desktop is up needs a nudge (`spawn
    /bin/hello`, since `spawn` reparents to init).
  - **`gui` at the shell REFUSES when a desktop is already up** and
    names the pid; it is still how you reach one from a `target=text`
    boot.
  - **init is spawned AFTER `debug_console_init()`**, and the ordering
    is load-bearing: spawning first had the desktop doing its startup
    disk I/O while the console came up, and `ktest_run.py` timed out
    waiting for the prompt.
  - **Nothing ORDERS the services** -- they all start at once, because
    with one service an ordering graph would be a data structure
    pretending to be a design.

  And **`rm /etc/services.d/<name>` DISABLES a service without stopping
  it** -- init rescans when `fs_generation()` moves, so the running copy
  is left alone and simply not restarted (systemd's `disable`, not
  `stop`). That is the ONLY way to take the desktop out of init's hands
  without a reboot, and **a test that needs to be the only compositor
  must do it**: `screen_surface_test.py` and `compositor_death_test.py`
  both kill the desktop, and without this init restarts it with a ZERO
  backoff and the new desktop claims the role straight back. Both had
  passed for months on the accidental interlock that `gui` blocking the
  shell provided -- **automating a lifecycle removes interlocks somebody
  depended on**; look for them.
- **RING 3 CAN READ THE CONSOLE NOW -- fd 0, and it BLOCKS.**
  `sys_read(0, buf, n)` parks the caller on `SCHED_WAIT_KEY` and
  `keyboard.c`'s `ring_push()` wakes it from the IRQ, the same
  park-and-return shape pipes, `waitpid` and `sleep` already use --
  nothing reopens the `sti`-in-the-handler hazard `SYS_READ_KEY`'s
  comment describes, because the handler does not wait, it RETURNS.
  That is what `/bin/tosh` (`userland/bin/tosh.c`) needed to exist.
  Five things to know. **It never returns 0** -- a console has no EOF,
  and 0 would tell a shell its input had closed. **It is RAW**: one byte
  per key, exactly the code `keyboard_try_getchar()` gives (specials are
  0x91-0xA6), with no echo, no editing and no escape translation, so
  the reader echoes what it reads -- all three are a line discipline and
  belong above a real TTY. **The first fd-0 read CLAIMS the console**
  and the kernel shell stands down until that process dies; the claim is
  a SECOND flag beside the compositor's, released from
  `fd_release_all()` so a crashing shell gives the keyboard back on its
  own. **A ring-3 reader tests `keyboard_compositor_owns()`, never
  `keyboard_blocking_suspended()`** -- the combined predicate is true of
  its own claim and would deadlock it. And **whoever waits for a key
  also presents the screen**, so the handler flushes the console back
  buffer before parking; the ring-0 loop that normally does it is
  suspended on this reader's behalf. See `docs/decisions.md`.
- **A QMP TEST THAT TYPES PUNCTUATION MUST PIN THE GUEST'S KEYBOARD
  LAYOUT.** A qcode names a PHYSICAL key by its US-layout label, and
  this OS defaults to `se`, where that key produces something else: every
  `/` `tools/shell_flow.py` typed arrived as `-`, so `spawn /bin/tosh`
  became `spawn -bin-tosh` and `touch /probe.txt` created a file
  genuinely called `-probe.txt` -- which a substring assertion passed.
  `kbd=us` on the GRUB line (`docs/boot-flags.md`) or `sh keyboard us`
  over the debug console fixes it; porting the table per layout was
  rejected, since it would then be silently wrong for anyone who changed
  the setting. The general form is this file's existing rule about
  assertions a broken version still passes -- **an exact match would
  have caught it and a substring did not**.
- **RING 0's BLOCKING KEYBOARD READERS ARE SUSPENDED WHILE A COMPOSITOR
  HOLDS THE ROLE** (`keyboard_suspend_blocking()`, set from the one place
  in `win_server.c` the role changes). With init starting the desktop,
  the physical shell sits at a prompt BEHIND it and both were draining
  the same key ring -- so a key typed at the desktop could be executed
  by an invisible shell. That was MEASURED, not predicted. `gui`'s
  spawn-and-wait used to block the shell for the desktop's whole life,
  which is the accidental interlock this replaces. The non-blocking
  `keyboard_try_getchar*` are untouched, which is what keeps
  `win_input.c` feeding the compositor. Two consequences: **the physical
  console is deaf (and hidden) for as long as a desktop is up** -- drive
  the machine over the serial debug console, or `target=text` -- and **a
  compositor that registers and never draws leaves a console both blank
  and deaf**, which looks exactly like a hung machine. Real console
  ownership is the TTY milestone's job; this is a placeholder that makes
  exactly one thing read the keyboard at a time.
  **SUSPENDING THE READ WAS NOT ENOUGH -- the loop BODY draws.** The
  first version put the check in the `while` condition only, so the
  blocking reader stopped returning keys and went on calling
  `vga_cursor_tick()` and `vga_present()` every iteration: console
  upkeep, published straight into the framebuffer the compositor owns.
  The symptom was a blinking text cursor sitting on a desktop icon,
  reported by the user from a screenshot with **all 23 GUI tools
  green**. Both calls are guarded now; `scheduler_idle()` is not, because
  it is the one thing in that loop that is not the console's. The
  general form: when you suspend a loop, ask what its BODY does as well
  as what its exit condition is.
- **`win_server_active()` MEANS A RING-0 LAYER, and the desktop is not
  one.** Use **`win_server_any()`** for "is there a window server at
  all" -- either a ring-0 presentation layer or a registered compositor,
  which is what `win_server_request()` itself gates on. Three places
  open-coded this and two got it wrong by omitting the compositor half:
  two KTESTs guarded themselves with `win_server_active()` so they would
  SKIP while the desktop was up, and quietly stopped skipping the moment
  the desktop became a process. Nothing noticed until init started the
  desktop at boot and `make test` finally ran with one registered -- at
  which point they failed, and a third (`wintransport`) failed
  intermittently for the same reason. **A predicate named for the thing
  that used to be the only implementation is worth re-reading whenever
  that stops being true.**
- **`SYS_SLEEP` exists, and a caller with no scheduler slot gets -1.**
  RDI is milliseconds; the caller parks on a deadline and the timer tick
  releases it, so the RESOLUTION is one tick and a sleep never returns
  EARLY. The refusal is the part to know: `run` uses the legacy loader,
  which has no process-table slot, so a `/bin` program that sleeps
  cannot be tested with `run <name>` -- use `spawn`. Returning 0 there
  would say "you slept" and a polling loop would spin on it.
- **`ps` is a REAL `/bin` PROGRAM, not a builtin** (`userland/bin/ps.c`,
  over `SYS_PROC_INFO`) -- pid, ppid, state, cumulative CPU, memory,
  name, with `--tree`. It is where the process tree finally has a
  reader: `ppid` had existed in the ABI since stage 0 with nothing able
  to show it. Two things worth knowing. **It cannot see itself at the
  physical shell** (the legacy loader has no slot, so there is nothing
  in the table to report), and **kfmt's numeric width ZERO-pads** --
  `%5u` of 1 is `00001`, so a right-aligned column means formatting the
  number first and padding it with `%5s`.
- **A PROCESS'S MEMORY IS FREED WHEN IT DIES, NOT WHEN IT IS REAPED --
  and killing needs a DIFFERENT entry point from exiting.**
  `syscall_process_exit_cleanup()` is for a process ending itself and
  switches CR3 to the kernel's address space on the way;
  `syscall_process_kill_cleanup()` is for `scheduler_kill()` and leaves
  CR3 alone, because the caller there is a different, still-running
  process (the WM force-quitting a client) that would otherwise resume
  in the wrong address space. Neither used to run on the kill path at
  all, so every kill leaked the victim's ELF pages, stack, heap and
  window buffer permanently -- ~18 frames a time, reachable from the
  desktop via Force Quit. Two ordering rules: the teardown runs AFTER
  `win_server_client_gone()` (which reaches into address spaces and
  needs this one alive), and `pml4_phys` is zeroed straight after so
  nothing follows it again. `tools/frame_balance.py` covers both paths.
- **A USER MAPPING SAYS WHETHER IT OWNS ITS FRAME, and getting that
  wrong is silent.** `vmm_destroy_address_space()` frees every frame it
  finds in a dying process's page tables, so anything mapped in that the
  process does NOT own must go through `vmm_map_user_borrowed()`
  (`PAGE_BORROWED`, a spare PTE bit). The question to ask at any new
  mapping site is **who calls `pmm_free_frame()` for this frame?** -- if
  the answer is not "this address space's teardown", it is borrowed.
  Five sites were wrong: the font (pages of the KERNEL IMAGE, mapped
  read-only into every GUI client), the framebuffer twice, a client's
  window buffer (the window server allocates and frees it), and the
  poison page -- one frame mapped at every page of a slot, so an owning
  teardown freed a permanent singleton dozens of times. Closing one GUI
  app returned four frames of kernel `.rodata` to the allocator.
  **The trap in MEASURING it: an over-free fires ONCE and then goes
  quiet**, because `pmm_free_frame()` only counts a frame that was
  marked used -- so `+4, +0, +0` reads as noise then health, and is not.
  Check on a fresh boot and believe only the first cycle;
  `tools/frame_balance.py` does exactly that. This is NOT refcounting --
  it says "somebody else frees this", not "count me" -- and CoW and
  `MAP_SHARED` still need a real per-frame refcount. See
  `docs/decisions.md`.
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
  criterion**: killing the desktop at the shell (`ps` for the `toywm`
  pid, then `kill <pid>` -- it was `kill 1` until init took that pid)
  revokes the framebuffer grant, ASKS
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
- **`SYS_SBRK` RESERVES; THE PAGE ARRIVES ON TOUCH.** The break is a
  claim, not a mapping (2026-08-18), which is what makes the ~2046 MiB
  per-process heap affordable -- it was ~14 MiB and eagerly mapped.
  Four things to know. **sbrk can no longer report OUT OF MEMORY** -- it
  refuses only a request past `UADDR_HEAP_LIMIT`, and the machine
  running out kills the process at the page it cannot be given; that is
  overcommit, as on Linux. **The fault handler is NOT the only entry
  point**, which is the trap: ring 0 walks page tables rather than
  dereferencing user pointers, so a syscall handed an untouched buffer
  never faults -- it gets a walk that finds nothing. Hence a REGISTERED
  hook (`vmm_set_fault_handler`, `kernel/mm/vmm.c`) with three callers:
  the #PF handler, the copy helpers, and `vmm_validate_user_range()`.
  **Those last two are redundant with each other**, so a positive
  control that disables one reddens NOTHING -- disable both, and
  `guard_test`'s two "untouched sbrk page" checks are the ones that
  fire. And **`mapped_end` is gone** from `struct sched_heap`: the page
  tables already record which pages exist, and a second record could
  only disagree with them silently.
- **THE RING-3 MAP IS SIZED FOR 4K, and a region's END is what the next
  thing must clear.** `WIN_BUFFER_STRIDE` is 64 MiB (a 3840x2160x4
  buffer is 31.6 MiB), `WIN_CLIENT_BASE` `0x8080000000`,
  `WIN_COMPOSITOR_BASE` `0x80A0000000` (16 GiB -- 64 pids x 4 windows),
  `WIN_FB_VADDR` `0x8500000000`, heap ~2046 MiB below the stack. The
  trap that caught this change first: the compositor region is DERIVED
  (`MAX_PIDS * CLIENT_MAX * STRIDE`), so its base looks isolated while
  it spans gigabytes, and growing the heap to `0x8080000000` landed
  inside where it used to be. **This does NOT make 4K work** --
  `WIN_CLIENT_MAX_W/H` is still 1280x720 and window buffers still come
  from `pmm_alloc_contiguous()` (8192 contiguous frames at 4K, refused
  silently under fragmentation). Raising the caps before that is fixed
  turns a hard limit into an intermittent silent failure.
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
  asked. The ring-3 heap is ~2046 MiB now (it was 1 MiB, then ~14 MiB once a
  compositor's back buffer needed 3.5 MiB at 1280x720). See
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
  The trap, if you touch the allocator (`kernel/lib/heap_core.c` --
  SHARED with ring 3's malloc now, so a change there lands in both):
  blocks of both shapes coexist, and
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
  note the reason is the mechanism, not the waiver. The two waived today
  are `klineedit.c`'s keymap and `apps/shell.c`'s 60-branch command
  chain, the latter deliberately NOT converted: most of those commands
  are kernel introspection, so the table they want is a `/proc`-shaped
  interface reached once the shell moves to ring 3, and converting first
  would build the wrong table (see `docs/roadmap.md`).
- **ADDING A SYSCALL IS THREE EDITS, AND ONE OF THEM IS A TABLE ROW.**
  The number in `abi/syscall_abi.h`, a handler in the subsystem that
  owns it (`kernel/proc/syscall_fd.c` for anything taking an fd,
  `kernel/fs/fs_syscalls.c`, `kernel/proc/proc_syscalls.c`,
  `kernel/proc/win_syscalls.c`, `kernel/core/sys_syscalls.c`) with its
  prototype in `kernel/include/kernel/syscalls.h`, and a row in
  `kernel/proc/syscall_table.c`. There is no registry and no init call
  to forget -- `syscall_dispatch()` is a bounds-checked call through
  that table and nothing else. It was a 37-branch `if/else` chain in a
  1,492-line `syscall.c`; the shape is Linux's `sys_call_table[]` and
  NT's SSDT, deliberately without their generators. Four things to
  know. **The row carries the `strace` description too** (name,
  argument kinds, return kind) -- that is one table where there were
  two, because the second one DRIFTED and fourteen syscalls traced as a
  bare number for months; `kstack syscalls` reads it as well. **A
  handler writes its own return value into `c->regs[14]` and RETURNS
  whether it parked the caller**, because `SYS_SBRK` returns a pointer
  so no 64-bit value is free to be a "blocked" sentinel. **A local
  added to `syscall_dispatch()` is paid for by every syscall** -- that
  is how its frame reached 4832 bytes; it is 96 now, and each handler
  pays for its own. And **`syscall_process_exit_cleanup()` calls one
  release hook per file** (`fd_release_all`, `proc_syscall_release`,
  `win_syscall_release`): kernel-side state keyed by an address space
  is not part of that address space, so tearing it down frees none of
  it. See `docs/decisions.md`.
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
- **PREFER FACTS THAT CANNOT GO STALE. Do not cite a number that some
  other file has to keep true.** Every maintenance burden this repo has
  deleted was the same shape: a pointer to a number.

  - Build numbers ("see build 379") died with the changelog they indexed.
  - Milestone NUMBERS needed three renumberings and a translation table;
    milestones are named now, and a title does not drift.
  - Target versions on roadmap items predicted a release nobody had
    committed to, and were removed.
  - Test counts in prose ("192 GUI checks across 14 tools") were wrong
    within weeks -- twice.

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

  What does NOT earn it is the war story. **Cap the anecdote at one
  clause** -- "(a missed declaration left a second cursor on screen)"
  persuades exactly as well as the forensics, and the forensics are in
  `git log`. Existing long comments are deliberately NOT being
  retro-trimmed: a bulk rewrite's most likely casualty is the one
  sentence that saves a future session. This is a rule for new writing.
- **THERE IS NO CHANGELOG.** Closed 2026-08-15, deleted 2026-08-18 (the
  four files had reached ~12,000 lines). It was stopped because the same
  reasoning was being written three times -- a comment beside the code,
  an entry in `docs/decisions.md`, and a writeup nobody re-read -- and
  deleted rather than frozen because a frozen file still has to be
  reasoned about by anyone editing near it.

  **Where the four kinds of thing go:**
  - *What changed, file by file* -- the COMMIT MESSAGE, which lists
    every changed file with a one-line note. `git log` is the record.
  - *How the mechanism works, and the trap in it* -- a comment next to
    the code. This is what gets found by whoever edits it.
  - *Why this way and not the obvious way* -- `docs/decisions.md`,
    written IN FULL there rather than as a pointer elsewhere.
  - *What is broken / not built yet* -- `docs/roadmap.md`, with a
    reproduction precise enough to replay.

  **What was lost, stated rather than glossed:** commit messages before
  the freeze are one-liners, so for pre-2026-08-15 work the changelog
  was the only detailed account. What is gone is the blow-by-blow of old
  changes -- not the decisions, which are in `docs/decisions.md`.
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
  cuts a release. Git
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
  - **No pointer to a changelog.** There isn't one (see below).
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

**`docs/testing.md` is the full testing reference** -- how to run this
OS, drive it headlessly, and prove a change works. This section holds
the build targets and the traps worth knowing before you look anything
up.

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

**Source discovery is recursive now** -- every `.c` under `kernel/` or
`apps/` is compiled and every `.asm` under `kernel/` assembled, with
`build/` mirroring the source tree, so a new directory needs no Makefile
edit at all. (It used to be one hand-written wildcard + pattern rule +
mkdir target per directory; `userland/wm/` and `apps/ui/` each paid that tax
when they appeared.) The flip side: a `.c` file anywhere under
`kernel/` or `apps/` IS in the kernel image -- there's no scratch file
the build ignores, so throwaway code goes somewhere else.

**A plain `make all` is safe after editing a shared header** -- the
Makefile tracks header dependencies (`-MMD`/`-MP`, plus the `-include`
line near `$(KERNEL)`'s rule), and `tools/check_deps.py` proves per
build directory that the tracking is live, so if that check is green
believe it and look elsewhere. It broke silently once for the whole of
`kernel/` when objects moved directories, which is why the check exists.
**What is NOT tracked is a CFLAGS change** -- the `.d` files record
headers, so editing `CFLAGS`/`USERLAND_CFLAGS` invalidates nothing and
the next build links objects compiled under the old flags. **`make
clean` after a flags change**, and treat a compiler flag that appears to
work only partially as a stale-object symptom first (that is exactly how
`-ffunction-sections` presented). `tools/gen_version.sh` is idempotent
for this reason: `kapi.h` includes `version.h`, so an unconditional
rewrite would rebuild the tree every build.

**`make all` does NOT update `toy-os.iso`, and every headless test boots
the ISO.** `ktest_run.py`, `boot_smoke_test.py`, `vm.py` and the GUI
tools all launch `toy-os.iso`, so a `make all` without `make iso` leaves
them testing the PREVIOUS build -- and it does not fail loudly, it fails
as a clean pass. **`make iso`, not `make all`, before any headless
run.** When a positive control turns NOTHING red, check that the ISO is
newer than the change before suspecting anything else (`ls -l
build/kernel.bin toy-os.iso`); `tools/iso_guard.py` now refuses a stale
one, but the reasoning is worth keeping: the data never reached the code
under test because the code never reached the machine.

**Every automated test here runs TCG, and a green suite therefore says
nothing about two whole bug classes** -- anything depending on how FAST
the emulated hardware is (a driver race whose window is microseconds),
and anything depending on guest MEMORY TYPES (TCG ignores PAT, so a
write-combined framebuffer behaves like cached RAM). Both have shipped
real bugs here. When a report reproduces only on hardware, try
`python3 tools/vm.py --kvm` BEFORE concluding it is untestable, and ask
what the emulator models differently before doubting the report.
`tools/kvm_soak.py` is the standing check. See `docs/testing.md`.

**`strace <binary>` is often the fastest way to see what a `/bin` binary
is doing** -- one decoded line per syscall
(`open("notes.txt", O_WRITE|O_CREAT) = 3`), and the same lines land in
`dmesg`, so `python3 tools/vm.py exec "strace file_test"` returns text
you can assert on. Reach for it before adding temporary `klog_write()`
calls inside a syscall handler; see `kernel/proc/strace.c`.

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
`QMPSession`/`GuiFlow` API, and the dozen gotchas already handled in
`tools/qmp_test.py` (why `-display none` silently breaks input routing,
why there must be no `usb-tablet`, the `-daemonize` launch, cursor
drift and `recalibrate()`, dropped keystrokes, absolute screendump
paths). Read it before writing QMP code.

Four things worth knowing without opening it:

- **Use `tools/qmp_test.py`, don't rederive it.** Import `QMPSession` /
  `launch_qemu_cmd()` rather than hand-rolling socket/JSON code or a
  `qemu-system-x86_64` invocation; add to the module when it lacks
  something, instead of writing a one-off.
- **Never `pkill -f qemu-system-x86_64`.** It cannot tell your headless
  launch from the user's interactive `make run` window. Kill only the
  PID your own launch wrote to its `-pidfile`.
- **Prefer `tools/gui_debug.py` to pixels** for anything not literally
  about rendering -- it returns facts to assert on rather than an image
  to interpret. It is asynchronous: call `DebugConsole.settle()` before
  asserting, and never replace that with a fixed sleep.
- **Screenshots are a TESTING TOOL, not a deliverable.** Take as many as
  a check needs, into a scratch directory. Do NOT save them into
  `screenshots/` as evidence -- that convention is retired (2026-08-15,
  at the maintainer's request). Show the user one when SEEING it is the
  answer; don't attach one to prove a check passed, since a
  `gui_regress.py` table is better evidence than an image a reader has
  to interpret.

## tools/

Dev/build helper scripts, not compiled or shipped as part of the OS.
**`docs/tools.md` is the full reference** -- what each one does, why it
exists, and the traps it encodes. This is the index; read that file
before reaching for anything here you have not used recently, and add to
it (not to a one-off script) when something would save a future session
real time. The bar is "does this fix a rederive-from-scratch cost".

- **Verify before delivering** -- `preflight.sh` (the gate: clean build +
  iso + `check_deps.py` + `check_layout.py` + `check_dispatch.py` +
  `boot_smoke_test.py` + `ktest_run.py` + `usertest_run.py`),
  `check_docs.py`, `deliver.py`.
- **Drive a VM** -- `vm.py` (text in, text out: the fastest path for
  anything that is not about pixels), `qmp_test.py` (QMP GUI helpers),
  `gui_debug.py` (ask the WM what it is doing), `gui_flow.py`,
  `shell_flow.py`, `serial_capture.py` (read a guest that is DYING),
  `watch_vm.sh` (view-only VNC onto a headless run), `run_release.sh`.
- **Test runners** -- `boot_smoke_test.py` (does it boot),
  `ktest_run.py` (`make test`), `usertest_run.py` (the `/tests` ELFs),
  `faulttest_run.py` (the ones that fault ON PURPOSE),
  `gui_regress.py` (every GUI tool, ~300 checks, ~1.5 min),
  `flake_hunt.py` (is it intermittent, and at what RATE),
  `damage_sweep.py` / `damage_hunt.py` (the damage invariant).
- **GUI tools**, all run by `gui_regress.py` -- `blank_window_test.py`,
  `calculator_client_test.py`, `compositor_test.py`,
  `compositor_death_test.py`, `cpanel_test.py`, `crashtest_test.py`,
  `cursor_theme_test.py`, `desktop_entries_test.py`, `dialog_test.py`,
  `forcequit_test.py`, `gfxdemo_test.py`, `idle_desktop_test.py`,
  `menubar_test.py`, `notepad_client_test.py`, `sched_gui_test.py`,
  `screen_surface_test.py`, `scrollbar_test.py`,
  `single_instance_test.py`, `taskmgr_test.py`, `uapp_test.py`,
  `uiclient_test.py`, `uidemo_test.py`, `uterm_test.py`,
  `winclient_test.py`.
- **Run on demand, not in the gate** -- `init_test.py` (init and service
  supervision), `stdin_test.py` (blocking fd 0 and `/bin/tosh`, which
  needs the physical console and so takes the desktop down first),
  `kvm_soak.py` (the timing bugs TCG cannot show),
  `mem_stress.py`, `frame_balance.py` (does teardown balance),
  `live_boot_test.py`, `fs_switch_test.py`, `tfs3_v1_test.py`,
  `mkpart_test.py`, `demo_test.py`.
- **Disk images, from the host** -- `seed_disk.py` (the format-aware
  front end `make iso` calls), `tfs2_writer.py`, `tfs3_writer.py`.
- **Diagnose** -- `panic_resolve.py` (name every address in a panic),
  `pixel_probe.py` (read exact pixel values -- how a GUI change is
  verified), `screenshot_diff.py`, `iso_guard.py`.
- **Generated data and the build** -- `gen_version.sh` / `set_version.sh`
  (versioning), `genfont.py` / `genttf.py`, `gen_kbs.py` (keyboard
  layouts from XKB data), `gen_cursors.py` (cursor themes),
  `genrelocs.py` (the kernel's own relocation table), `gen_syms.py` (the
  panic symbol table), `gen_decisions_index.py`.
- **The repo itself** -- `backup_repo.sh` (run it before ANY change to
  the repo's identity or history -- a mirror clone is not a backup here,
  release assets live only on GitHub), `device_git.sh` (Cowork only).

Five standing rules that are cheaper to know than to rediscover:

- **`gui_regress.py` is the standard check** after touching `apps/ui/`,
  `userland/`, or anything the WM draws -- always with `--logs DIR`.
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
Reference material lives in `docs/` behind a one-line pointer.** Two
files carry most of that: `docs/testing.md` (how to run and drive this
OS, the QMP mechanics, what the emulator does and does not model) and
`docs/tools.md` (the full reference for every script in `tools/`). Add
detail there, and keep the pointer here to a line.

`docs/decisions.md` -- the INDEX over `docs/decisions/`, which holds the
answers to "why does toy-os work this way?" split by area: `kernel.md`
(55 entries), `gui.md` (75), `storage.md` (35), `build.md`, `shell.md`,
`drivers.md`, `workflow.md`. Add an entry to the file for its area and
run `tools/gen_decisions_index.py`; the index is GENERATED and
`tools/check_docs.py` fails the build when it is stale, because an index
kept true by someone remembering is the shape of every convention this
project has had to delete.

**Entries are SELF-CONTAINED.** They used to be pointers into a
changelog that held the real writeup, and when that was deleted the
entries leaning on it were the ones left stranded. Write the reasoning
in the entry. The bar: it answers "why this way and not the obvious
way", and a future session would plausibly re-litigate it -- what
changed goes in the commit message, how a mechanism works goes in a
comment beside the code, what is broken goes in `docs/roadmap.md`.

`README.md`/`apps/README.md` cover architecture and `git log` is the
chronological record; this exists because neither is indexed by TOPIC,
so "why is X built this way" otherwise means grep and guesswork.

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

**Milestones are NAMED, not numbered** (2026-08-18) -- a milestone is
its title and its position is its layer, so nothing is renumbered when
one is inserted. Older `Milestone N` references in git history, the
changelogs and some source comments resolve through the one-way legend
at the end of `docs/roadmap-details.md`; do not add a number to a new
one. **`docs/roadmap.md` IS ORDERED BY WHAT MUST BE BUILT FIRST** (restructured
2026-08-18, from eight ground-up LAYERS). **Phases 1-4 are a dependency
chain** -- the system runs itself, then memory, then the process model,
then the userland runtime -- so reading top to bottom answers "what
next". Below them are **tracks** (storage, GUI, hardware, tooling) which
depend on neither the phases nor each other, ordered internally only;
saying so is the honest part, since a total order would imply
dependencies that do not exist.

**EVERY ITEM IS ONE LINE.** No rationale, no measurements, no repros --
those go to `docs/roadmap-details.md` under a heading of the same name,
which is why a milestone's title must match in both files. A **Needs:**
line appears at most once per milestone and only where the dependency is
real and not obvious. When you tick an item, keep it to one line too:
the file was 2,939 lines before this and nobody could see the order for
the prose.

Forward-looking "not built yet" items belong in `docs/roadmap.md`
instead (already actively maintained, with completed items struck
through and linked to the commit that finished them) -- don't
duplicate that list here or start a second one.

When you resolve a "wait, why is this built this way" question during
a session (by reading the git history, a source comment, or by asking the
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
