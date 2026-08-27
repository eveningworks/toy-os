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

## The sidebar-and-pages redesign, and `uui_tree` (2026-08-19)

Control Panel became **System Settings** -- a `uui_tree` sidebar on the
left and one page on the right, KDE System Settings' shape. Four things
worth carrying.

**A new widget needs a second REAL caller, and the roadmap named one.**
`uui_tree` was written for this sidebar and deliberately says nothing
about settings, because the file manager the roadmap already lists needs
a directory pane. That is the toolkit's standing bar; a widget shaped
around one app's data is one nobody else can use.

**Make the easy path three lines and the powerful path available.** The
nodes are the app's flat `const` array with a `depth` per row, and the
widget derives parent/child from the depth run -- no allocation, no
ownership, no teardown, the same call `uui_menubar`'s const menu trees
make and still right now that ring 3 has `malloc`. Everything starts
expanded with row 0 selected, so a tree that is never told anything else
already behaves correctly; collapsing, ids and keyboard navigation are
there and cost nothing when unused.

**An app stores an ID, never a row.** Rows move as categories collapse.
`uui_tree_select_id()` also EXPANDS whatever was hiding the node --
selecting something and leaving it invisible looks to a user exactly
like the call did nothing.

**A sidebar needs categories, and the app must not own the list.**
`struct setting` gained a `category` string, so the sidebar is generated
exactly as the rows already were: a setting registered anywhere in the
kernel gets a sidebar home with no edit to the app. A category table in
the app would be the second source of truth the whole app exists to
avoid. NULL becomes `SETTING_CATEGORY_DEFAULT` at the ABI boundary, not
in each client.

**And one design mistake worth recognising:** the first version drew the
page's heading in `on_draw`, which runs AFTER the toolkit paints the
widgets -- so it landed on top of the first choice. Reserving space only
moves the problem, because the page SCROLLS and content would then slide
under a fixed heading. A heading is CHROME and belongs outside the
scroll view, which needs a label widget this toolkit does not have; the
information went in the status bar instead, which is already chrome and
already outside. Ask what a piece of text IS before deciding where to
draw it.

## Renaming a seeded app leaves the old binary behind (2026-08-19)

`make iso` re-seeds by SYNC, never reformat, so renaming
`/bin/wm/system/cpanel` to `.../settings` left BOTH on the image -- the
new one working, the old one orphaned and no longer named by any
`.desktop` entry. `make clean-disk && make iso` is the fix for a fresh
image; an existing disk keeps the stale binary until it is deleted by
hand. CLAUDE.md already states this ("moving a seeded file needs an
explicit delete"); it is easy to read past and obvious the moment
`ls /bin/wm/system` shows two.

## Fill a widget's ops table against the header, not against a neighbour (2026-08-19)

FOUR widgets shipped with short `struct uui_widget_ops` tables, every
function they needed already written and only the table missing an
entry: `uui_dropdown` and `uui_textview` (no `natural_size`/
`set_geometry`), `uui_checkbox` and `uui_textbox` (those plus no
`release`).

Both gaps fail silently and at a distance:

- **No `natural_size`/`set_geometry`** and a layout cannot place the
  widget. It sits at a zero rect -- invisible, unclickable -- and the
  CONTAINER looks like the broken thing.
- **No `release`** and `uui_route.c` never names the widget to its app,
  because it reports one only when it has a release op. The control
  works perfectly on screen and the app hears nothing.

None of it had been noticed because no app had put those widgets in a
routed layout. **An ops slot nobody fills is a capability nobody has
tested.**

`tools/check_widget_ops.py` enforces both rules now and is in
`preflight.sh`; waive in place with `widget-ops-ok: <reason>`. It found
the last two on its first run, which is the argument for writing it
rather than writing a rule nobody re-reads.

## A control below the fold is UNREACHABLE (2026-08-19)

A scroll view with a `hit` clips its children from ROUTING, not just
from drawing -- so a press never reaches a child outside the viewport.
That is correct and deliberate. The consequence for anything driving the
UI is that a control which has not been scrolled into view cannot be
clicked at all, and a tool aiming at its unscrolled coordinates gets
SILENCE rather than an error, which looks exactly like a dead control.

Two habits: scroll first, and have the app report each control's rect
whenever it MOVES (a page change and a scroll alike) rather than only
when the page changes -- otherwise the tool has no idea where the
control went.

## Three shapes for one list, chosen by data not by code (2026-08-19)

`uui_radio_list`, `uui_dropdown` and `uui_slider` all take the same
`options` array and yield an INDEX, so one setting picks between them
with a `Widget=` line in `/etc/settings.d` and no code change anywhere.
Choose by what the values ARE: few and unordered (radio), many
(dropdown), ordered levels (slider).

That is worth copying whenever a toolkit grows a second way to show the
same data -- keeping the VALUE type identical is what makes the choice a
presentation decision rather than a rewrite.

## Metrics cached at startup, and the two things that changed under them (2026-08-20)

A ring-3 client maps the font ONCE (`ugfx_font_init()`) and caches the
cell size. That was safe while the font could only change at boot. It
stopped being safe the moment a face could be selected at runtime, and
the failure mode is quiet: the setting applies, the kernel logs it,
`/etc` records it, and the screen keeps the old glyphs until the desktop
restarts -- so the Appearance setting looks broken rather than deferred.

`WIN_EV_FONT` is the answer, and `uapp` handles it for every app whether
or not the app has heard of fonts: re-map, re-run the layout, repaint --
the same deal `WIN_EV_RESIZE` already gave them. The compositor handles
it by re-mapping and forcing one full frame, which is enough only
because every piece of its chrome is measured from `ugfx_char_h()` FRESH
each frame. That property is worth not losing: it is what made the
handler three lines instead of an invalidation pass.

The shape to copy: this is Wayland's `wl_output` scale change and X11's
XSETTINGS notification. The server does not re-lay-out anybody. It says
the metrics moved, and each client decides what that means.

## The console will paint over the compositor if you let it (2026-08-20)

`vga_reflow()` recomputes the console's rows and columns and then clears
the screen -- and the clear ends in `vga_present()`, which blits the
console's whole buffer. With a desktop up, `fontsize` at the physical
shell wiped the top two thirds of it. Pre-existing (the old `fontsize`
did it too), found only because a new command called the same function.

The fix is the same shape as the suspended keyboard reader: recompute
always, PAINT only when `win_server_any()` says nobody else owns the
screen. The console the user cannot see is the one that does not need
clearing.

Worth generalising while working anywhere in `vga.c`: every path in it
that ends in a present is a path that can overwrite the desktop, and the
ring-0 console has no idea a compositor exists unless it asks.

## An app that never clears its surface draws OUTLINES, not text (2026-08-21)

A ring-3 app with no widgets and no layout owns its whole surface and
**the toolkit clears nothing for it** (`uapp.h`'s `on_draw` says so). One
that skips the clear draws onto an uninitialised buffer -- and the way
that fails is much stranger than "wrong background":

`ugfx_draw_char()` SKIPS fully-background pixels, so the `bg` handed to
a text call never fills anything. It is only the colour the antialiased
rim is blended toward. On a black buffer that paints the glyph interiors
(alpha 255, near-black fg) invisibly and leaves only the rim, so every
letter renders as a hollow outline -- smeared-looking at 14px, an
outline typeface at 24px.

**Passing a `bg` to a text call is not the same as having a
background.** If an app looks like it is rendering in an outline font,
it never cleared.

## uapp creates the window BEFORE on_open, so slow setup shows a blank window (2026-08-21)

An app that loaded a ~400 KB font file in `on_open` sat on screen as an
empty rectangle for as long as the read took. `blank_window_test.py`
caught it as "only 1 distinct colour in its content area", which is
exactly what that tool is for.

**An app's first paint should depend on nothing it has to go and
fetch.** Do the expensive thing after the first frame and ask for a
repaint (`uapp_redraw`), with a placeholder in the meantime.

## Bold is not reliably WIDER -- measure ink (2026-08-21)

The obvious check that a bold weight is distinct from regular is that
the same string measures wider. On a MONOSPACE face it does not: a
designed bold has exactly the regular advances, because every cell is
one width by definition. A width probe reports a perfectly working bold
as a fallback.

What is true of every bold, monospace or not, is more INK in the same
letter. A client can count it directly -- the glyph coverage bytes are
mapped read-only, and reading them is the same data the server
rasterized.

Related, and it cost an hour: **colour and weight confound every
luminance-threshold measurement.** A dimmer heading has fewer pixels
under any darkness threshold whatever its weight, so "is this bolder"
cannot be measured until the two colours are made identical. Do that
first, as a temporary control.

## Reserving descender room cost a control its reachability (2026-08-21)

Glyph bitmaps are taller than the line pitch, so a descender on a
label's last row hangs below its box and whatever is drawn under it can
paint over the tail. Reserving those two pixels per label seemed
free -- until a settings page with twenty labels grew 42px and pushed a
control past the bottom of its own scroll view, which is the "a control
below the fold is unreachable" trap arriving from a direction nobody
watches.

It is opt-in now. **Two pixels a widget is nothing until a page has
twenty of them**, and the general form is that any per-widget size
increase is a per-PAGE increase you have not measured.

## A symmetric mistake reads as a deliberate design (2026-08-21)

A spinbox's stepper arrows were both drawn upside down. The drawing
routine varied the y DIRECTION between up and down while keeping the
width sequence the same, which flips both triangles -- so they still
looked like a matched pair pointing sensibly apart, and nothing about
them read as wrong until the user said so.

Direction is now carried by the width sequence with rows always running
downward, which makes "up" and "down" differ in exactly one place.

**A bug that breaks two things symmetrically survives an eyeball check**,
because the thing an eye catches is asymmetry.

## 2026-08-22 -- the Terminal became a terminal

**`apps/ui/` IS GONE.** The kernel image contains no widget code at all.
The last file in it survived only because the kernel's own `edit` drew
with it; `edit` is `/bin/edit` now, over `utext` -- the model Notepad
already used. **When one caller is holding a whole directory alive, ask
what it would take to move that caller.**

**THE TERMINAL RUNS `/bin/tosh` ON A PTY**, so the shell in a window is
a real process with a pid, visible in `ps`. It used to LINK tosh as a
library and call `tosh_run_line()` from its key handler. What that
deleted is the argument for it: the linked-in shell, the output sink,
`stdin_ok`'s empty-pipe stdin, a second line editor and a second command
history -- none of it wrong, all of it there because the window was not
a terminal.

**ITS SCREEN IS A GRID OF CELLS, NOT A CHARACTER STREAM.** That is what
lets a full-screen program address it. Scrollback is made of rows that
scrolled off, so history and screen meet with no seam -- they are the
same kind of thing.

**AND THE ANSI PARSER IS THE KERNEL'S, COMPILED TWICE.**
`kernel/lib/ansi.c` is a pure state machine that knows nothing about a
screen; the console and the window resolve `ESC[4;12H` through the same
code. A second parser in ring 3 would have been a second set of answers
to "what does `ESC[0m` clear". **Before writing a parser in ring 3,
check whether the kernel already has one that touches no hardware** --
`geom.c`, `klineedit.c`, `calc_engine.c`, `etc_config.c` and now
`ansi.c` all cross that way, and the Makefile rule already exists.

**A BACKGROUND IS A RECTANGLE, NOT A COLOUR ARGUMENT.**
`ugfx_draw_string(s, x, y, text, fg, bg)` blends the GLYPH's pixels
against `bg`; it does not fill the cell. For ordinary text the two are
indistinguishable, and for reverse video they are not -- a status bar
comes out as dark letters on black. Fill the run's rect first.

**KEYS GO THROUGH AS BYTES AND THE WINDOW DECIDES NOTHING.** The shell
has the line editor (`klineedit.c`, compiled twice), so a Terminal that
interpreted Ctrl-A would be the second implementation that file exists
to prevent. The toolkit's key codes ARE the byte codes the editor
switches on, so there is no translation layer -- which is the point: a
translation layer would be a third place for the keymap to drift.

**AN APP WITH A CHILD NEEDS A NON-BLOCKING READ, because there is no
`poll()`.** A window must service the compositor and drain its child and
cannot block on either. `SYS_SET_NONBLOCK` plus a `tick_ms` cadence is
the shape until `poll()` exists; an idle Terminal waking 33 times a
second is the honest cost, and it is written down rather than hidden.

## When one traversal recurses and its sibling does not (2026-08-24)

Reported as "the dropdown items do not highlight on hover". The dropdown
was fine: `uui_listbox` drew a hover row, `uui_dropdown` forwarded
motion to it, and the event never arrived. `uui_router_press()` and
`uui_router_wheel()` walked nested containers to any depth;
`uui_router_motion()` walked exactly ONE level and had since it was
written. System Settings nests four deep, so every hover state in the
app was dead, in every app that nests.

**The generalisable part: when three code paths do the same walk, an
asymmetry between them fails silently and at a distance.** Nothing
errors -- a widget that never hears the pointer simply never lights up,
which reads as a missing feature and sends you to the widget. When you
add or fix a traversal, read its siblings in the same file and ask
whether they agree. That is the same instinct as "fill a new widget's
ops table against `uui_widget.h`, never against the widget you copied".

Three rules the fix had to encode, none of which the one-level version
had to think about:

- **A widget the cursor has LEFT still has to hear the move**, or its
  highlight stays lit. Motion goes to everyone; a press stops at the
  first taker.
- **A clipping container must not let its children light up outside
  itself** -- a scroll view lays rows out past its own edges, and those
  coordinates are somewhere the pointer really can be. Skipping the
  subtree strands a lit row; passing the real point lights an invisible
  one. It is told a point no widget can contain instead.
- **An open overlay gets the real point first**, because it is drawn
  outside its own rect and its container's -- and is then SKIPPED in the
  walk, since hearing the move twice lights a row and clears it again.

## A keyboard path that the pointer had and the keyboard did not (2026-08-24)

Two gaps of the same shape, both found by giving a list type-ahead.
`overlay_active` made an open popup take the next PRESS; nothing gave it
the next KEY, so typing in a dropdown depended on a focus ring the app
might not have. And `uui_focus_key()` changed a focused widget's value
while `on_widget` only ever fired for pointer input, so a control
changed by the KEYBOARD was staged by nobody and silently dropped by
Apply.

**Ask what the pointer can do that the keyboard cannot, whenever you
touch either.** The two halves of an input system drift apart one
feature at a time, and each gap looks like a missing feature in the app
rather than a missing rule in the toolkit.

## Forwarding "everything" breaks the behaviour somebody chose (2026-08-24)

Making a CLOSED dropdown accept typed letters, I forwarded every key to
its list. Home and End are keys: the value started walking with the
popup shut, which `uidemo_test` asserts against by name ("Home while
closed does nothing") and which is a deliberate rule -- an arrow must
not change a setting the user cannot see. A letter is a deliberate
search; navigation is not. The forward is restricted to printable keys,
and the test that caught it is the reason the rule is written down.

## The widget takes keys; the app routes none (2026-08-27)

Type-ahead was added to `uui_table`, tested, shipped -- and did nothing
in Task Manager, which the maintainer found in a minute. The widget was
correct. `uapp.c` offers a key to `uapp_desc.focus` and then to
`uapp_desc.on_key`, and Task Manager declared NEITHER, so no key had
ever reached `uui_table_key()`. Arrows, Home/End and paging had been
dead there since the app was written; nobody noticed because nobody
tries to arrow around a process list.

This is one level up from the 2026-08-24 entry above. That one was about
the pointer having a path the keyboard did not INSIDE the toolkit. This
is the toolkit having the path and the APP not connecting to it -- and
it fails the same way, silently, looking like a broken widget.

**When you add a `key` op, grep for who routes to it.** `.focus` and
`.on_key` are the only two doors; an app with neither is a closed
building. That is CLAUDE.md's own "a slot that is PRESENT and read by
nobody" rule, arriving from the app side rather than the widget side --
and it is why `tools/check_key_routing.py` exists now.

**The routing choice is real, and it is not always the focus ring.** A
`uapp_desc.focus` ring is the toolkit's idiom and gives Tab between
controls -- but no widget draws a focus indicator yet, so adding a ring
adds a Tab stop nobody can see. For an app with ONE keyboard-hungry
widget, forwarding from `on_key` is what the File Manager does and what
Task Manager does now. Ask before assuming the documented idiom is the
right one for the app in front of you.
