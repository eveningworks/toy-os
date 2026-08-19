# What past sessions learned: the GUI

Toykit, the widgets, the compositor and the chrome -- and the traps that
make a widget look broken in a way that points somewhere else. Entries
are verbatim from the running log past sessions kept, dated where they
were written.

**`docs/gui-guidelines.md` is binding and wins over anything here**;
these are the incidents that produced several of its rules. `CLAUDE.md`
carries the current form of the ones that generalised.

**2026-08-15: the ring-3 GUI has a toolkit now -- do NOT hand-roll a
client.** The whole `docs/uapp-design.md` plan was built this day
(stages 0-4). What a session needs to know:

- **The stack has names.** **TWP** = the Toy Window Protocol
  (`abi/win_proto.h`, the client<->server contract), **TWS** = the Toy
  Window Server (`kernel/proc/win_server.c` + `userland/wm/wm_client.c`),
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

**Cursor themes: the pointer's shapes are DATA FILES** (`data/cursors/
<theme>/<shape>`, generated by `tools/gen_cursors.py`, loaded by
`userland/wm/cursor_theme.c`). The layering is the one Windows and Wayland
both converged on: an app NAMES a shape, the compositor owns the theme
and produces pixels, the display layer owns any hardware plane. Wayland
originally had clients supply the pixels and added `cursor-shape-v1` to
undo it -- don't repeat that. A shape file carries COVERAGE, not colour,
so one shape set serves a light theme and a dark one. Nothing touches
the kernel, so it all moves to ring 3 with the WM.
