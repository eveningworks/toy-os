# GUI, Toykit and the desktop

Toykit widgets, TWP/TWS, the compositor, the window manager and the
cursor. `docs/gui-guidelines.md` is the separate, binding spec for how
anything drawn should LOOK and BEHAVE; this file is how the machinery
works and what breaks if you edit it the obvious way.

These are the conventions CLAUDE.md indexes by headline but does not
carry in full -- it is the always-loaded context, so it holds the rule
and this file holds the reasoning and the trap. **The headline of every
entry here also appears in CLAUDE.md**, so a session sees the warning
without loading the body; come here when you are actually working in
this area, or when a headline there tells you something you did not
know.

Same bar as `docs/decisions.md`: an entry earns its length from the
INVARIANT (what must stay true) and the TRAP (what breaks if you edit
this the obvious way), not from how much history it accumulated.

---

- **`apps/ui/` IS DOWN TO ONE WIDGET, and the GUI toolkit is
  `userland/ui/`.** What is left is `ui_scrollback.{c,h}`, which the
  KERNEL's own `edit` command draws with (`apps/editor.c`) and which
  therefore cannot move to ring 3. **A new widget goes in
  `userland/ui/`. There is no longer any such thing as a kernel-side
  one.** `apps/theme.h` survives for the same kind of reason:
  `apps/completion.c` colours the shell's tab-completion with it.
  Three rules the deleted widgets taught still apply to their ring-3
  twins: draw a popup LAST (drawing is immediate-mode, so z-order is
  call order); **route keys through the focus ring, never by trying each
  widget in turn** -- the first one tried swallows every key it
  recognises, which left a listbox next to a dropdown unreachable from
  the keyboard; and **a widget's BEHAVIOUR belongs to it, not to the
  app**. See `docs/gui-guidelines.md`'s "Behaviour belongs to the
  component" for the escape hatch.
- **Ring-3 GUI apps are written against Toykit's `uapp`, and a new one
  is a `.c` file in `userland/gui/` with NO Makefile edit.** Describe
  the app in a `struct uapp_desc` -- title, a `uui_layout`, callbacks --
  and `uapp_run()` owns the TWP handshake and the event loop
  (`userland/ui/uapp.h`). Every callback is optional with a library
  default, which is what lets TWS gain a feature without apps being
  edited. Do not hand-roll a window handshake or an event loop in a new
  client. Layout (`uui_layout.h`) means an app writes no coordinates:
  declare a column/row/grid, and the window sizes itself from the
  content. Resize, focus and wheel all arrive for free.
  `docs/uapp-design.md` is the full design.
- **An app with a cadence sets `tick_ms` and BLOCKS between frames.**
  `uapp_desc.tick_ms` arms a TWS timer (`WIN_REQ_TIMER` ->
  `WIN_EV_TIMER`), so `on_tick` arrives as an event instead of the loop
  spinning. Leaving it 0 keeps the old polling loop, which wakes a
  process 100 times a second whatever it actually wanted. Three rules:
  the interval is in MILLISECONDS and is floored at one tick, never
  zero; the next firing is computed from NOW so a slow client never
  accumulates a backlog of overdue firings; and there is ONE timer per
  window. See `docs/decisions.md`.
- **An app refuses its OWN second copy -- the launcher never does.** A
  `uapp_desc` with an `app_id` and `UAPP_SINGLE_INSTANCE` sends
  `WIN_REQ_ACTIVATE` before creating anything: TWS raises the window
  already carrying that id and the second copy exits 0 without ever
  appearing. Three things to know. The id rides `WIN_REQ_CREATE`'s
  `text` field so a window can never exist without it (a later "register
  my id" message leaves a gap exactly long enough for a second copy to
  miss its twin). It is an opaque token -- `"taskmgr"`, not a path and
  not the title. And **it is not a lock**: two launches in the same
  instant can both be told "nobody there", which is recorded rather than
  fixed because every launch path here is a human clicking a menu. See
  `docs/decisions.md`.
- **`uui_table` sorts on a header click, and an app supplies only a
  COMPARATOR.** `uui_table_set_compare()` + `uui_table_set_sort()`; the
  widget owns the ordering (an `int order[]` permutation), the clickable
  header, the arrow and the toggle-to-reverse rule, exactly as Win32's
  `ListView_SortItems` and Qt's `lessThan` split it. **It cannot sort
  the text it draws** -- cells are formatted strings, so "10" would come
  before "9"; comparison has to be on the app's real values. Every
  public row index on the widget is an APP row, not a screen position,
  so a selection survives a re-sort. Two traps it exposed: a widget's
  `ops->hit` must cover the WHOLE widget (routing on the row-only hit
  meant header and scrollbar presses reached nothing), and anything
  comparing `selected` against `top` is mixing an app row with a view
  offset. See `docs/decisions.md`.
- **A `uui_scrollview` NOTICES when its content's item list changes**
  (`sv_children()` compares the `items` pointer and `count` against what
  it last laid out). It used to re-lay-out only on its own rect or
  offset moving, so an app that swapped a page's items left every NEW
  widget at a ZERO RECT, invisible and unclickable, while widgets
  carried over kept the PREVIOUS page's geometry -- which reads as a
  broken layout, one layer away from the cause.
  `uui_scrollview_content_changed()` is still the honest thing to call
  at the point of change and is no longer load-bearing.
- **`uui_slider` is for an ORDERED enum** (`userland/ui/uui_slider.h`)
  -- discrete stops, one per choice, with the value an INDEX into the
  same `options` array `uui_radio_list` and `uui_dropdown` take. So a
  setting can switch between all three by changing `Widget=` in
  `/etc/settings.d`, with no code change. It is deliberately NOT
  continuous: the registry's only list-carrying type is an enum, so a
  continuous slider would need a numeric setting type that does not
  exist. A drag tracks x ONLY (leaving the track vertically must not
  cancel it) and `press` returns non-zero on any hit, because the router
  takes its pointer grab only when press does.
- **A CONTROL BELOW THE FOLD IS UNREACHABLE, not merely hard to hit.**
  A scroll view with a `hit` clips its children from ROUTING, so a press
  never reaches a child outside the viewport -- correct, and the reason
  a tool must scroll before clicking rather than aiming at unscrolled
  coordinates, where it gets silence rather than an error. An app driven
  by tools should report a control's rect whenever it MOVES (a page
  change and a scroll alike), not only when a page changes.
- **`on_draw` RUNS BEFORE THE WIDGETS; `on_draw_over` RUNS AFTER.**
  `uapp.c`'s order is "clear, then the APP's own painting, then the
  widgets, then overlays", and it is that way deliberately: an app whose
  first line clears the surface -- the natural thing to write -- can
  then only ever wipe its own backdrop, which is a bug that shipped
  twice before the order was fixed. **So anything that must appear ON
  TOP of a widget goes in `on_draw_over`.** System Settings' System
  Information page painted from `on_draw`, the scroll view filled its
  rect straight over it, and the page came up EMPTY with the title,
  sidebar selection and status bar all still correct -- which reads as
  a data problem, not a paint-order one. Its own comment had asserted
  the opposite order, which is what made it invisible.

- **`uui_label` WRAPS ONLY IF ASKED, AND THE CALLER RESERVES THE ROWS.**
  `uui_label_set_wrap(l, rows)` is Qt's `QLabel::setWordWrap` and
  GtkLabel's `wrap` -- opt-in, because most labels are a word or two in
  a control and wrapping one would look broken. The height still comes
  from `rows`, NOT from the text: a real toolkit asks "how tall are you
  at this width?" (Qt's `heightForWidth`) and needs a second measure
  pass through the layout, which this does not have.

  **RESERVING A ROW FOR EVERYONE IS NOT THE ANSWER.** Turning wrapping
  on for every description with a flat two rows made nine descriptions
  in ten a row taller than they need, and that was enough to push the
  last control of a page BELOW THE SCROLL FOLD -- where it is
  unreachable, not merely awkward. Compute the rows from the text and
  the width instead.

  **And that computation belongs to the APP, once per page.** It does
  not break the `natural_size` rule, which forbids a widget measuring
  itself from where it currently IS during layout; this is the app
  deciding what to ask for before layout runs. Two wrong cadences were
  tried first: every frame relayouts under the user and resets the
  scroll position, so a long page cannot be scrolled at all; once per
  APP fits only whichever page opened first, because every later page's
  labels are laid out for the first time when it opens and so report a
  width of 0. Once per PAGE is the one that works.

- **`uui_label` is the caption widget** (`userland/ui/uui_label.h`) --
  one line of text the LAYOUT reserves a row for, with no behaviour and
  **no `hit`**, so a click passes through to whatever is behind. Reach
  for it instead of painting a caption in `on_draw`: that runs AFTER
  the toolkit paints widgets, so hand-drawn text lands ON TOP of a
  control, and reserving space by hand leaves content sliding under it
  the moment the page scrolls. Its natural HEIGHT does not depend on
  its text (`rows`), or a caption changing would reflow the page.
- **`uui_tree` is the navigation widget** (`userland/ui/uui_tree.h`) --
  rows at a DEPTH with collapsible parents. **The nodes are the app's
  flat `const` array**, each carrying its depth; the widget derives
  parent/child from the depth run, so there is no allocation, no
  ownership and no teardown -- the same call `uui_menubar`'s const menu
  trees make, and still right now that ring 3 has `malloc`. Five things
  to know. **The easy path is three lines** (declare nodes, read
  `uui_tree_selected_id()`): everything starts expanded with row 0
  selected. **An app stores an ID, never a row** -- rows move as things
  collapse -- and `uui_tree_select_id()` EXPANDS whatever was hiding the
  node, because selecting something invisible looks exactly like doing
  nothing. **Collapsed state is a BITMAP on the widget**, not a flag on
  the node, since the nodes are the caller's `const` array. **The
  expander toggles without navigating**, because exploring a section is
  not choosing it. And **`natural_size` counts every node, collapsed or
  not** -- a tree that shrank when collapsed would make the layout
  twitch under the user's own click.
- **A SETTING DECLARES ITS CATEGORY, and the sidebar is generated from
  it.** `struct setting.category` (`api/setting.h`) is a free string --
  `"Appearance"`, `"Input"`, `"Startup"` -- carried to ring 3 on
  `SETTING_OP_INFO`, with NULL becoming `SETTING_CATEGORY_DEFAULT` at
  the ABI boundary rather than in each client. So a setting registered
  anywhere in the kernel gets a sidebar home the same way it already
  gets a System Settings row, and **System Settings holds no list of
  categories any more than it holds a list of settings** -- a table in
  the app is the second source of truth the whole app exists to avoid.
- **Control Panel is now SYSTEM SETTINGS** --
  `userland/gui/system/settings.c`, `/bin/wm/system/settings`, driven by
  `tools/settings_test.py`. Renamed because Control Panel is Windows'
  name and this shows exactly the SETTINGS registry (not facts, not
  tunables). The shape is KDE System Settings': a `uui_tree` sidebar,
  one page, a status bar. **The rename left a stale
  `/bin/wm/system/cpanel` on any existing `disk.img`**, because `make
  iso` re-seeds by SYNC -- `make clean-disk && make iso` for a fresh
  image, or delete it by hand.
- **`uui_table` is the multi-column widget** (`userland/ui/uui_table.h`)
  -- columns with per-column width (in CHARACTERS, or 0 to stretch) and
  alignment, a header, selection, scrolling. **It PULLS its rows through
  a callback and stores none of them**: Task Manager re-reads the
  process table several times a second, so there is nothing cached to go
  stale (ring 3 having `malloc` now changes nothing here). Sizing is
  derived, so a resizable window reflows with no arithmetic in the app.
  See `docs/decisions.md`.
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
  hand-dispatch input in a new app** -- a widget an app forgets to
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
  -- every widget's id landed in `hidden`, and the compiler's
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

  **`tools/vm.py --vga vmware` is how you reach the modesetting driver
  at all** -- the default `std` adapter has neither modesetting nor a
  cursor plane, same shape as `--cpu max` for SMEP/SMAP. The hardware
  cursor is switched off on the only driver that has one (`vmsvga`'s
  `g_cursor_enabled = 0`, because a hw cursor over a relative PS/2 mouse
  makes the pointer jump), so it is unreachable on every configuration
  this OS boots.
- **`ugfx` has a SCREEN surface now, and it is the compositor's**
  (`struct ugfx_screen`). `ugfx_screen_init()` takes ring 1's
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
  and a fault are the same path. Two things to know. **The memory
  type must reach the USER PTE** (`vmm_map_user_page_type()`,
  `VMM_MT_WC`): the kernel's identity map and the compositor's mapping
  are separate PTEs, and `paging_set_write_combining()` only touches
  the former, so without this a ring-3 compositor gets a CACHED
  framebuffer -- the bug class TCG cannot show you. And **present is
  required, not advisory**: `vmsvga` declares `DISPLAY_CAP_NEEDS_FLUSH`,
  where written pixels are invisible until the driver is told. See
  `docs/decisions.md`.
- **`uui_radio_list` arms on press and COMMITS ON RELEASE**, restoring
  the previous row if the pointer left the list -- the rule
  `docs/gui-guidelines.md` states for every control. Two traps came out
  of adding it. The router only names a widget to the app when that
  widget HAS a `release` op; and `press` must return non-zero on ANY
  hit, because the router takes its pointer grab only when press does --
  with 0 for the already-selected row, that one row silently loses its
  release. **An app must honour `reason`**: discarding it applies a
  setting on every pointer-motion event, which froze the desktop for
  seconds and exposed the `fs_read()` bug above.
- **A WINDOW'S APPLICATION IDENTITY IS THE KERNEL'S, not the app's.**
  What a window's application IS comes from the full path its owning
  process was spawned from (`scheduler_exec_path()`), interned by the
  kernel into `struct window.app_identity`. **Do not key anything on
  `uapp_desc.app_id`** -- that is a DISPLAY NAME now, and two apps
  declaring the same one used to raise each other's windows (a
  single-instance app told "your twin is up" exits without ever drawing,
  so the symptom is an app that does not start) and merge into one
  taskbar button. No runtime check can catch that, because two copies of
  one program are supposed to match. `WIN_REQ_ACTIVATE` therefore takes
  NO INPUT: it asks "is a window of MY program open?" and the kernel
  answers from a fact the asker cannot influence, which is why
  `UAPP_SINGLE_INSTANCE` no longer needs an `app_id` at all. The
  compositor receives an opaque NUMBER rather than the path, because
  `win_request_msg.text` is 32 bytes and a path is 64 -- shipping it
  would truncate and re-create the collision. See `docs/decisions.md`.

- **The TASKBAR'S LAYOUT IS ONE FUNCTION, and past a floor it groups by
  app.** `taskbar_layout()` (`userland/wm/wm_taskbar.c`) decides which
  buttons exist, how wide they are and what each stands for; the
  renderer, both hit-tests in `wm_input.c` and the debug console all
  read it. **Do not reintroduce a width constant** -- `win_btn_w()` was
  one, three places walked the list with it, and buttons ran off the
  screen edge and under the clock because no single place could shrink
  them. The fourth walk, in the debug console, had already drifted eight
  pixels, so tests were clicking beside the buttons they aimed at.
  Buttons shrink to a font-derived floor, then windows of one
  APPLICATION collapse into one counted button whose click opens a list
  of them; anything that still will not fit is dropped and counted by
  `taskbar_hidden()` rather than drawn off-screen. Grouping is by
  `struct window.app_identity` (see above), which reaches the compositor
  through `WIN_REQ_WINDOW_APPID`, asked once at create because
  `WIN_REQ_WINDOW_INFO`'s single `text` is the title. A window with no
  identity groups by client pid instead. See
  `docs/decisions.md`, and `tools/taskbar_test.py` for the thresholds.


- **The WM has a SLOW-FRAME WATCHDOG** (`userland/wm/wm_watchdog.c`): it
  times each `wm_run()` iteration by phase and logs anything over a
  threshold (150ms by default). The design point worth preserving: it
  measures only the work AFTER the frame's `hlt`, so a SILENT watchdog
  during a visible freeze is a real answer -- the loop was not running,
  i.e. the stall is below us (host scheduling, the display backend, an
  emulator's fsync) -- rather than a missing measurement. `gui watchdog
  [<ms>|off]` tunes it and reports the fired/peak counters, which matter
  as much as the threshold: "no SLOW FRAME lines" is only evidence of a
  fast WM if it was armed. The attribution trap: `wmwd_phase()` names
  the phase ABOUT TO START, so the interval it closes belongs to the
  PREVIOUS one.
- **There is a Crash Test app** (`userland/gui/demos/crashtest.c`,
  Start menu only -- no desktop icon on purpose). Ring-3 buttons fault
  in the app's own code and prove the desktop survives; Ring-0 buttons
  ask the KERNEL to panic and are **refused unless booted with
  `faultinject`**. The kernel owns the fault list (`api/crashtest.h`),
  so adding a kind there gives the app a button with no edit -- and
  `tools/crashtest_test.py` is safe in `gui_regress` precisely because
  the dangerous half is disarmed by default. To exercise a real panic:
  `make iso KCMDLINE="faultinject"`.
- **The cursor's shapes are DATA FILES, and a theme is a directory.**
  `/usr/share/cursors/<theme>/<shape>` (six shapes: `arrow`,
  `resize-h`, `resize-v`, `resize-diag`, `text`, `wait`), generated by
  `tools/gen_cursors.py` and loaded by `userland/wm/cursor_theme.c`. Two
  registered settings pick the theme and the size. Four things to know.
  **A shape file carries COVERAGE, not colour** -- an outline mask and a
  fill mask, coloured by the compositor -- so one shape set serves a
  light theme and a dark one. **The built-in shapes are the floor**: a
  missing or malformed file costs its own shape, not the pointer --
  which also means **a theme that loads NOTHING still draws a perfect
  pointer**, so never test this by checking that a cursor is on screen
  (the first version shipped loading 0 of 6 and looked right).
  **Scaling is integer nearest-neighbour** and the size is its own
  setting, not derived from `font_size`. And **the generator EXTRACTS
  the arrow from `wm_render.c`'s own arrays**, so re-run it after
  touching those or the shipped theme drifts from the fallback
  (`--check` fails on stale). See `docs/decisions.md`.
- **The cursor's drawn extent is DERIVED, not a constant.**
  `cursor_rect()` (`userland/wm/wm_render.c`) is the one place that
  answers "what box does the pointer occupy", and the save/restore pair
  and the damage rect both ask it. A theme's size, its hotspot and the
  size setting all move that box, so a fixed `CURSOR_BOX_SIZE` could not
  survive themes -- and the two consumers disagreeing is the
  stale-sprite bug this file's comments record paying for twice. The
  previous box is STORED rather than recomputed, because the shape under
  the old position may not be the shape there now.
- **A compositor's view of a dead window is POISONED, not unmapped**
  (`comp_poison()` in `kernel/proc/win_server.c`). The invariant: while
  a compositor is registered, a window buffer's slot in its address
  space is never a HOLE -- frames that go away are replaced by one
  shared read-only zero page. A compositor is a PROCESS: it learns a
  window died from a queued event and may blit the slot once more before
  it drains that, and a hole there is a page fault, i.e. the desktop
  dying (which is exactly what Force Quit did). The frames really are
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
  synthesise a keystroke into any other.
- **`SYS_FS_GENERATION` is how ring 3 asks "has the filesystem
  changed?"** -- no arguments, the counter in RAX. Its own syscall
  rather than a `SYS_SYSINFO` field on purpose: the desktop polls it
  ONCE PER FRAME to decide whether to re-read `/usr/wm/desktop/`, and a
  free poll is the entire reason the counter exists instead of a
  directory scan. It says something changed, never what.
- **A ring-3 process can own a real window** (`userland/wm/wm_client.c` +
  `kernel/proc/win_server.c`, protocol in
  `kernel/include/abi/win_proto.h`). Two rules matter before touching
  it. **Every client operation is a typed MESSAGE carried by the one
  `SYS_WIN_REQUEST` syscall, never a syscall of its own** -- that is
  what keeps the boundary a protocol; see `docs/decisions.md`. And
  **the split is memory vs. presentation**: `win_server.c` owns
  ids/buffers/mappings/teardown (page tables and the frame allocator),
  `wm_client.c` owns the window list, chrome, z-order and input routing,
  and they meet at a registered `struct win_server_ops` -- the same
  registry pattern as `display_driver`. **A client draws with
  `userland/ui/ugfx.c`**, not with syscalls -- there is no drawing
  syscall and there shouldn't be, since only the framebuffer is
  privileged, not drawing. The one thing a client can't produce for
  itself is the font, which `WIN_REQ_FONT` maps READ-ONLY out of the
  kernel's own tables rather than copying (one instance in memory, and
  client text can't drift from the desktop's when `font_size` changes).
  Two widgets have no kernel-side ancestor and were written here first:
  **`uui_menubar`** (submenus nested to any depth -- a menu is const
  arrays pointing at each other, which is still the right shape now that
  ring 3 HAS `malloc`: a declared tree needs no teardown and cannot
  leak; per-item checked/disabled state is ASKED FOR through an
  `item_flags` hook rather than stored in the tree) and
  **`uui_statusbar`**. Three things to know: **the menu bar opens on
  PRESS**, the one deliberate bend in the commit-on-release rule (the
  item still commits on release -- see `docs/decisions.md`); **a popup
  is clamped to a bounds rect the app passes in**, which is the client's
  window today and becomes the screen when `WIN_REQ_POPUP` lands, so the
  flip/slide/clamp code is already the right code; and **there are no
  Alt+letter mnemonics on purpose** -- Alt is an ESC prefix here, so
  Alt-F is ambiguous with Esc, and `KEY_F10` focuses the bar instead.
  **A file needed by both the kernel and a client is COMPILED TWICE,
  never copied** (`build/userland/shared/`): the two builds use
  different code models so the objects can't be shared, but the source
  can. Only freestanding files qualify.
- **`Exec=builtin:` is GONE, and ring 0 contains no applications.**
  An entry still naming that form is refused loudly rather than shown as
  a row that does nothing -- an entry file can outlive the mechanism it
  names. One live consequence: the WM's live-`.desktop`-reload deferral
  (`userland/wm/wm.c`) is now UNREACHABLE, because it triggers on a
  window holding a `gui_app_registry[]` pointer and only a kernel-space
  app ever held one. The guard is kept and correct;
  `desktop_entries_test.py` asserts the property that makes it
  unreachable, so a kernel-space app coming back turns that check red
  instead of producing a mystery rebinding bug.
- **The Start menu and desktop icons are built from FILES**, one
  `.desktop`-style entry per app in `/usr/wm/desktop/` (source of truth:
  `data/wm/desktop/`, format documented in its README). `gui_apps.c`
  scans that directory at desktop startup, so **adding an app to the
  desktop is dropping a file there**, not editing a table -- and it is
  picked up LIVE, no restart: the WM watches `fs_generation()` and
  re-reads the directory when it moves. **One directory feeds BOTH
  surfaces**, with `ShowIn=desktop startmenu` choosing which; a second
  directory per surface was rejected because an app wanted in both would
  have its file duplicated and the copies drift. **Anything positional
  must go through `gui_app_visible_count()`/`_at()`** -- the Start
  menu's rows are indexed by position, so filtering the draw while
  hit-testing the unfiltered registry lands every click on the wrong app
  and looks correct in a screenshot. Windowed binaries live under
  `/bin/wm/{system,apps,demos}/` -- the class is the SOURCE directory
  (`userland/gui/<class>/`) and the Makefile derives the destination,
  same rule that already made `userland/gui` mean `/bin`.
- **A Start-menu entry launches a RING-3 program**, named by `exec_path`
  on the registry entry (`apps/gui_apps.h`): `open_app()` spawns it and
  the process makes its own window through the windowing protocol. Two
  consequences before adding one: a launcher always SPAWNS (it never
  focuses an existing window -- the WM can't enforce single-instance on
  a ring-3 program, and shouldn't; the APP refuses a second copy of
  itself instead), and **a real ring-3 app is seeded to `/bin`, not
  `/tests`** -- see `docs/filesystem-layout.md`, and note that moving a
  seeded file needs an explicit delete since `sync` is additive.
- **There is no limit on open windows** -- `windows[]` is a grown-on-
  demand block (`wm_windows_reserve()`), not a fixed array. Two rules
  follow from it MOVING when it grows: never hold a `struct window *`
  across anything that can open a window, and index rather than cache.
  `WM_WINDOWS_INITIAL` is a starting capacity, not a limit.
- **Super/Win toggles the Start menu, and Alt+F4 closes a window** --
  both are WM shortcuts consumed before keys reach the focused window,
  so a full-screen app cannot swallow either. `KEY_SUPER` comes from the
  0xE0-prefixed 0x5B/0x5C scancodes; left and right send the same code.
- **A window may be dragged off the left/right/bottom edges and UNDER
  the taskbar**, keeping 8 character-widths of title bar grabbable and
  never above the top edge (every other edge is recoverable by dragging
  the title bar; the title bar cannot recover itself). A window that
  ends up unreachable anyway is pulled back by `wm_ensure_reachable()`
  when its taskbar button is clicked -- this desktop has no
  Alt+Space/Win+arrow escape, so that button is the only handle such a
  window has.
- **The window manager lives in `userland/wm/`** -- the core event
  loop/input/render split (`wm.c`/`wm_input.c`/`wm_render.c`, sharing
  state through `wm_internal.h`'s `extern`s) plus the pieces that grew
  their own files as they appeared: `desktop.c`, `start_menu.c`,
  `context_menu.c`, `confirm_dialog.c`, `file_picker.c`, `wm_tray.c`,
  `cursor_theme.c`, `wm_client.c`. Split by concern for readability --
  it's still one tightly-coupled event loop, not decoupled components.
- **`win_server_active()` MEANS A RING-0 LAYER, and the desktop is not
  one.** Use **`win_server_any()`** for "is there a window server at
  all" -- either a ring-0 presentation layer or a registered compositor,
  which is what `win_server_request()` itself gates on. Three places
  open-coded this and two got it wrong by omitting the compositor half:
  two KTESTs guarded themselves with `win_server_active()` so they would
  SKIP while the desktop was up, and quietly stopped skipping the moment
  the desktop became a process. **A predicate named for the thing that
  used to be the only implementation is worth re-reading whenever that
  stops being true.**
- **THE DESKTOP IS A RING-3 PROCESS.** `/bin/wm/system/toywm` is
  `userland/wm/` compiled as a ring-3 program, spawned and waited on by
  `apps/gui3.c`; `gui` starts it. It claims the compositor role, takes
  the framebuffer grant, loads the font, composites, opens client
  windows and answers the `gui` debug console. There is exactly ONE
  window manager, and `apps/` holds no GUI at all: the shell, the
  editor, the demo, and `gui3.c`.
- **Killing the desktop is survivable, and that is the milestone's exit
  criterion**: killing it at the shell (`ps` for the `toywm` pid, then
  `kill <pid>`) revokes the framebuffer grant, ASKS each client window
  to close (never destroys it -- that would fault a client mid-draw),
  restores the text console and leaves the kernel running; `spawn
  /bin/wm/system/toywm` starts a new one. **`kill` and `spawn` are shell
  commands for exactly this reason.** `gui kill` cannot end the desktop
  (it is dispatched from inside the WM's own loop and `scheduler_kill()`
  refuses the CURRENT process), and `run` cannot start one (the legacy
  loader is not a scheduled process, so its `win_request()` is refused).
  `compositor_death_test.py` asserts the whole cycle.
- **A GUI tool that needs the compositor role must ASK WHO HOLDS IT**
  (`gui compositor --json`). The role is SINGLE, so a tool that spawns
  its own stand-in evicts the desktop and then asks questions of a
  client that does not implement them -- which is how three tools failed
  the moment the desktop became a process. Each picks its scenario from
  that answer now.
- **A client that needs raw input without a desktop cannot be driven by
  keystrokes** -- with no WM the physical shell owns the keyboard, so
  injected keys go there. `screenclient auto` runs its whole sequence
  itself for this reason, announcing each step so a reply can be
  attributed to the command that produced it.
- **Four things a ring-0 component loses the moment it becomes a
  process:** (1) **`hlt` is PRIVILEGED** -- `sys_yield()` replaces it
  (and busy-waits, see the roadmap). (2) **The font is not free**:
  anything drawing in ring 3 must call `ugfx_font_init()`, which every
  window client gets inside `uapp_run()` and the WM has to ask for
  itself -- without it `ugfx_char_h()` is 0 and every font-derived
  measurement silently collapses (chrome becomes a sliver, icon labels
  vanish while their boxes still draw). It must happen BEFORE any
  geometry is computed from it. (3) **Nobody polls the hardware any
  more** -- `kernel/proc/win_input.c` polls and pushes `WIN_EV_RAW_*`
  from `scheduler_idle()`, and starts the mouse, staying silent while a
  ring-0 layer is registered so it cannot steal that WM's keys. (4) **A
  single `!g_ops` guard refused everything** -- a window server is now
  either a ring-0 layer or a registered compositor.

## `-vga virtio` IS A REAL DISPLAY DRIVER, and nothing else boots it

`virtio-gpu` (`kernel/drivers/virtio/virtio_gpu.c` plus
`kernel/drivers/display/display_virtio.c`) claims the display ahead of
vesafb and programs its own mode, so `video=<W>x<H>` is honoured there.
Three things follow that are easy to get wrong.

**It NEEDS_FLUSH, and it means it.** The framebuffer is ordinary guest
RAM that the device reads on command -- pixels written and never
published are invisible. The kernel's own drawing goes through
`gfx_present()`; a ring-3 compositor's goes through
`win_surface_present()` (`WIN_REQ_FB_PRESENT`). Before this driver every
adapter here scanned memory continuously, so that path was never
actually load-bearing and a compositor that skipped it looked fine.

**Its mode is set at PROBE, not at runtime.** It does not advertise
`DISPLAY_CAP_MODESET`: changing mode later frees the framebuffer that
`gfx.c` caches a pointer to and that the compositor has mapped. See
`docs/decisions.md`.

**Test it with `tools/virtio_gpu_test.py`.** Every other GUI tool and
`make test` launch `-vga std`, so the driver's KTESTs skip everywhere
else -- that tool is what supplies the hardware, and it runs
`ktest virtio-gpu` inside the guest as well as reading pixels from
outside. Its pixel-format oracle is a second boot on `-vga std`,
because "something is on screen" passes on a black screen and a
red/blue check passes on a format that rotates channels.

**A hardware cursor is invisible to `screendump`** -- QEMU hands a
device-composited cursor to the display client out of band, exactly as
real scanout hardware does. Do not write a pixel assertion for one.
