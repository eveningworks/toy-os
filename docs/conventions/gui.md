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

- **MEASURE TEXT, NEVER MULTIPLY: `gfx_char_advance()` in the kernel,
  `ugfx_char_advance()` in ring 3.** A face loaded from
  `/usr/share/fonts` may be PROPORTIONAL, so `k_strlen(s) *
  gfx_char_w()` is no longer a width -- it is the width of the widest
  possible string of that length. Everything that measures or steps
  through text is built on the advance:
  `gfx_text_width()`/`gfx_text_fit_chars()` and their `ugfx` peers, and
  the string-drawing functions. **The trap is that the wrong version
  still LOOKS right on the default face**, because `dejavu-sans-mono`
  is monospace and every advance equals the cell -- so a call site that
  multiplies is invisible until somebody selects `liberation-sans`, at
  which point its labels overlap. `tools/font_test.py`'s one
  load-bearing check exists for exactly this.

  Two related facts. `gfx_draw_char()` still paints the WHOLE cell,
  background included, because the console depends on it (a character
  replacing a wider one must leave nothing behind); string drawing
  paints only the advance. And the CONSOLE stays fixed-cell on purpose
  -- a terminal is monospace by definition -- so a proportional face
  gets a cell as wide as its widest advance there.

- **A LOADED FACE STILL ONLY DRAWS 101 GLYPHS.** The boundary that
  surprises people who have just seen a
  real TTF load. An atlas rasterizes exactly the set `tools/genttf.py`
  bakes -- ASCII 32-126 plus six Nordic letters (`font_ttf.h`'s
  `FONT_TTF_GLYPH_COUNT`) -- so DejaVu Sans Mono's other ~3,270 glyphs
  are parsed and unreachable, and anything outside the set falls back to
  `?`. That set is deliberate: it is what makes an atlas a DROP-IN for a
  baked variant everywhere from `gfx_draw_char()` to a client's own
  glyph indexing, and widening it means answering "what is a character?"
  first -- this OS says Latin-1 (`docs/decisions/drivers.md`), so it
  waits on UTF-8. **Do not "fix" it by widening the atlas alone**: the
  slot order is ABI, shared through `WIN_REQ_FONT`, so a client built
  against the old count indexes into the wrong glyph rather than failing.

  (A ring-3 PRIVATE font is narrower still -- 95 slots, ASCII only --
  because `ugfx`'s `glyph_index()` is `c - 32` and cannot address the
  six extras whatever an atlas holds. Rasterizing them would produce
  glyphs nothing can ask for.)

- **A FONT IS A HANDLE IN RING 3 AND A CONTEXT FLAG IN RING 0.** Ring 3
  passes `const struct ugfx_font *`, and `ugfx_set_font()` returns the
  previous one so save/restore is the shortest correct thing to write;
  `uui_label` has a `font` field. Ring 0 has `gfx_set_bold()`, which
  also returns the previous value. The asymmetry is deliberate -- ring
  0's callers are a console and a handful of drawing sites, while a
  widget tree is precisely where an unrestored global goes wrong, so the
  toolkit never got a `ugfx_set_bold()`.

  **A widget's font must be selected for MEASUREMENT as well as for
  drawing.** `natural_size()` is asked by the layout, long before and
  far away from any paint, so a bold label whose font is selected only
  at paint time is measured in regular and laid out too narrow -- text
  drawn into a box sized for a different font. That is why the font
  lives on the widget rather than being something an app sets around its
  draw call, and why `uui_label` wraps BOTH halves in a push/restore.

- **THERE ARE TWO FONT TIERS, AND THE SHARED ONE CANNOT GROW TO COVER
  THE OTHER.** `ugfx_font_session(weight)` is the desktop's face, mapped
  by the server, free, shared, and it changes under a running app -- one
  face, two weights, one size, and nothing else. Anything else is the
  app's own: `ugfx_font_load()` rasterizes a `.ttf` into memory the app
  owns, with the same `ttf.c` the kernel uses, at any size or face, and
  follows no setting.

  **The two tiers size their cell differently on purpose.** The session
  font's cell is squeezed (`ascent*0.89 + descent*0.60`, `genttf.py`'s
  formula) because it IS the layout grid and a looser one makes the whole
  UI taller; it clips descenders slightly, as every fixed-cell terminal
  font does. A private font is not a grid, so it uses the FULL ascent and
  descent -- copying the squeeze there just clips a 'g' for no benefit,
  which is what it did until somebody looked at a 24px heading.

  **Do not "fix" this by teaching `WIN_REQ_FONT` to serve arbitrary
  combinations.** An atlas is NEVER FREED -- clients hold the mappings
  and nothing can ask them to let go -- so that change is really "an
  unbounded product of faces, weights and sizes in a never-evicted ring-0
  cache". See `docs/decisions.md`.

- **BOLD IS A WEIGHT OF A FAMILY, AND A FAMILY IS A FILENAME RULE.**
  `<name>.ttf` plus an optional `<name>-bold.ttf` is ONE face, listed
  once, with both weights loaded and both mapped
  (`win_font_vaddr(weight)`). A family with no bold file still HAS a
  bold: the regular outlines are smeared (`ttf_embolden`) and every
  advance grows to match, which is what GDI does. `vera-mono` ships
  without a bold companion on purpose, so that path is exercised on
  every image rather than merely written.

  **Bold is not reliably WIDER, so do not test it that way.** On a
  monospace face a designed bold has exactly the regular advances, so a
  width probe reports a perfectly working bold as a fallback. What is
  true of every bold is more INK in the same letter -- `fontdemo.c`'s
  `glyph_ink()` reads it out of the atlas the client already has mapped.

- **KERNING IS APPLIED BY EVERY TEXT PATH, AND MEASURING MUST MATCH
  DRAWING.** `gfx_kern()`/`ugfx_kern()` give the pair adjustment, and
  every width, fit-count and draw loop in both rings applies it. A path
  that draws kerned and measures unkerned cuts a clipped string at a
  different character from the one it draws -- which is why the clipped
  ring-3 draw applies the kern ITSELF rather than leaving it to the
  one-character `ugfx_draw_string()` calls it makes (a run of one
  character has no preceding character, so its own loop always computes
  0).

  Only the legacy format-0 `kern` table is read; a GPOS-only face
  renders unkerned, on purpose (doing it properly is a shaping engine).
  `liberation-sans` is the only shipped face that kerns, so **a kerning
  check on the default monospace face passes vacuously** -- switch faces
  first.

  **The overlap trap, and it exists only in ring 0.** A glyph cell there
  is OPAQUE, so a negative kern makes the new cell's leading columns
  erase the tail of the letter just drawn ("To" lost the tip of the T's
  crossbar). `draw_glyph_kerned()` blends exactly those columns instead.
  Ring 3 needs none of it -- `ugfx_draw_char()` already skips
  fully-background pixels.

- **THE FONT CAN CHANGE UNDER A RUNNING CLIENT, AND `WIN_EV_FONT` IS
  HOW IT FINDS OUT.** A client maps the font once, at
  `ugfx_font_init()`, and caches the cell size; a face or size change
  makes every one of those numbers stale. The server broadcasts
  `WIN_EV_FONT` to every window AND to the compositor, and `uapp`
  handles it for an app that has never heard of fonts (re-map, re-run
  the layout, repaint) exactly as it handles `WIN_EV_RESIZE`. **The
  trap, which cost a debugging cycle: the compositor owns no window**,
  so a broadcast that only walks the window table reaches every client
  and misses the process that draws the chrome, the taskbar and the
  icons -- and the symptom is indistinguishable from the event never
  being delivered at all. `tell_compositor()` is a separate call, on
  purpose.

- **A FONT FACE IS NAMED BY ITS FILENAME, AND `builtin` IS NOT A
  FACE.** `/usr/share/fonts/dejavu-sans-mono.ttf` is the face
  `dejavu-sans-mono`, the same convention cursor themes use for
  directories, so listing what is available is a directory listing and
  nothing parses a font's internal `name` table. `builtin` names the
  ABSENCE of a face -- the glyphs `tools/genttf.py` baked into the
  kernel image -- so "go back to the built-in font" is a value the
  setting can hold and round-trip rather than a missing key. **The
  baked font is the guaranteed fallback and is not optional**: it is
  what draws before the filesystem is mounted, on the panic path, and
  whenever a face is missing or malformed. A console that could not
  draw text until a disk font loaded could not report why the disk font
  did not load.

- **`WIN_CLIENT_MAX_W/H` TRACKS THE DISPLAY CEILING, AND A SCREEN BIGGER
  THAN IT BREAKS MAXIMIZE SILENTLY.** `abi/win_proto.h`'s
  `WIN_CLIENT_MAX_W/H` is the largest pixel buffer the window server
  will hand a ring-3 client, and it used to be 1280x720 because that is
  what `boot.asm` asks GRUB for. The moment a modesetting driver
  (`bochs.c`, `vmsvga.c`, virtio-gpu) came up at 1920x1080, maximize
  asked for a 1918x1038 content area, `resize_window()` refused it, and
  **nothing anywhere said so** -- a refusal is a normal protocol outcome
  and is indistinguishable from a client declining. What you get is the
  failure `win_proto.h` already describes for the resize grip:
  full-screen chrome around a stale buffer, with undrawn desktop filling
  the difference. So **raise it WITH `DISPLAY_MAX_W/H`**
  (`kernel/drivers/display/display.c`), or the display gains pixels no
  window can use. It is 1920x1080 against a 3840x2160 display ceiling
  today, and the gap is deliberate: the server allocates a window's
  pixels CONTIGUOUSLY and up front, so 4K is 8100 contiguous frames
  asked for twice over on every resize -- `docs/roadmap.md`'s growable
  client buffers item is the prerequisite, not a bigger constant.
  `tools/hires_test.py` is the check, and it needs an ISO built with
  `KCMDLINE="video=..."` because at the default mode every assertion in
  it passes vacuously.
- **A DESKTOP-SIZED WINDOW IS "MAXIMIZED", AND THERE IS NO FULLSCREEN
  STATE.** `wm_toggle_maximize()` (`userland/wm/wm_input.c`) fills the
  screen ABOVE the taskbar and keeps the title bar; nothing removes
  chrome or covers the taskbar, and no client can ask for it. That is
  the same one implementation behind both the title-bar button and the
  context menu, and the client half is the part to read before touching
  it: a client is PROPOSED the new content size (the same configure/ack
  the resize grip uses, and the shape Wayland's
  `xdg_toplevel.configure` has) and `wm_client.c`'s
  `on_window_resized()` adopts w/h when the client answers. Imposing a
  size instead produces the stale-buffer failure above. A real
  fullscreen state is `docs/roadmap.md`'s window-size-as-a-property
  item.

- **`apps/ui/` IS GONE, and the GUI toolkit is `userland/ui/`.** The
  last thing in it was `ui_scrollback.{c,h}`, which survived only
  because the kernel's own `edit` drew with it; `edit` is `/bin/edit`
  now, over the same `utext` model Notepad uses, so **the kernel image
  contains no widget code at all.** A new widget goes in
  `userland/ui/`. There is no longer any such thing as a kernel-side
  one. `apps/theme.h` survives for the same kind of reason:
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
- **TERMINAL IS A TERMINAL EMULATOR, NOT A SHELL WITH A WINDOW.**

  It opens a pty (`SYS_OPENPTY`), spawns `/bin/tosh` on the slave, writes
  keystrokes into the master and paints what comes out. The shell in a
  Terminal window is a REAL PROCESS -- a pid, visible in `ps`, killable,
  reaped when the window closes.

  It used to link `tosh` as a LIBRARY and call `tosh_run_line()` from its
  key handler. That is why `Ctrl-C` did nothing here for so long: a
  window had no console, no foreground group and no terminal to have
  them on. It works now for the SAME REASON it works on the physical
  keyboard -- the key becomes the byte `0x03`, goes to a pty master, and
  `kernel/tty/ldisc.c` recognises it as INTR. Disabling that one function
  reddens the checks in `uterm_test.py` AND in `ctrlc_test.py`, which is
  how "one implementation" is known rather than claimed.

  Three things follow. **The window sends KEYS AS BYTES and decides
  nothing** -- the shell has the line editor, so a Terminal that
  interpreted Ctrl-A would be the second implementation `klineedit.c`
  exists to prevent. **Its screen is a GRID of cells, not a character
  stream**, which is what lets a full-screen program address it --
  `/bin/edit` runs in a window because of this, and scrollback is made
  of rows that scrolled off rather than of a stream. And **it drains its
  child on a 30ms tick, because there is no `poll()`** --
  `SYS_SET_NONBLOCK` is what makes that possible without freezing the
  window, and the real answer is on the roadmap.

- **THE ANSI PARSER IS THE KERNEL'S, COMPILED TWICE.**
  `kernel/lib/ansi.c` is a pure state machine that knows nothing about a
  screen, so the physical console and the GUI Terminal resolve
  `ESC[4;12H` through the same code -- the shared-source rule, as with
  `geom.c` and `klineedit.c`. A second parser in ring 3 would be a second
  set of answers to "what does `ESC[0m` clear", and the two would drift
  the first time either was extended.

  **Reverse video is resolved IN the parser**, which swaps `fg`/`bg`
  before the caller sees them, so neither terminal has to know the
  attribute exists. **And a background is a RECTANGLE, not a colour
  argument**: `ugfx_draw_string()` blends the glyph's own pixels against
  the background it is given and does not fill the cell, so a status bar
  drawn without an explicit `ugfx_fill_rect()` comes out as dark letters
  on black -- legible, plausible in a screenshot, and wrong. Found by
  reading pixel values.

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
- **`uui_spinbox` IS FOR A NUMBER; `uui_slider` IS FOR AN ORDERED ENUM.**
  A slider shows a magnitude and cannot show or accept an exact value;
  a spinbox does both. A setting gets one by declaring
  `SETTING_TYPE_INT` with `min`/`max`/`step` -- System Settings reads
  the range from the REGISTRY, so bounds that change in the kernel need
  no edit in the app. **The registry enforces the range, not the
  widget**: `config set` and a hand-edited /etc file never pass through
  a control, and `/etc/settings.d` may override presentation but not
  bounds. **Typing does not change the value until Enter or focus
  loss** -- a half-typed "15" on the way to "150" must not be clamped
  under the user's fingers -- while a stepper applies immediately.
  A spinbox EMBEDS a `uui_textbox`, because there is one implementation
  of what editing means.

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
- **`uui_sidebar` IS THE NAVIGATION WIDGET; `uui_tree` MODELS CONTAINMENT.**
  They look alike and behave nothing alike. A sidebar's HEADING is a
  caption: bold, no indent, and unreachable by mouse, hover, selection,
  arrow keys or the focus ring -- five places that all route through one
  `is_item()` predicate so they cannot drift apart. A tree's parent is a
  destination that can be collapsed. Every desktop that ships a settings
  sidebar (KDE, GNOME, macOS) ships a flat list with inert headers, not
  an outline view; System Settings uses `uui_sidebar` for that reason.
  A sidebar deliberately has no collapsing, no second level of nesting,
  and no icons (the glyph set has no room for them).

- **`uui_tree` models containment** (`userland/ui/uui_tree.h`) --
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

- **COLOURS COME FROM THE THEME, SIZES FROM ITS METRICS -- neither is
  hardcoded.** `userland/ui/utheme.{h,c}` is one live `struct utheme`:
  colour ROLES read through `utheme_current()` (the `UTHEME_*` macros are
  now accessors into it, so existing sites are theme-driven for free), and
  METRICS (`utheme_pad/gap/indicator/control_h`) are functions of the font
  so chrome scales with `font_size`. A new colour is a role, not a fresh
  `ugfx_rgb()`; a control's default size is a metric, not a literal. The
  split (palette vs metrics) is Qt's QPalette vs QStyle -- see
  `docs/decisions/gui.md`. A dark mode / accent is a `utheme_set()` swap.

- **AN APP LOGS THROUGH `ulog()`/`ulogf()`, not a hand-rolled `logf_`.**
  `userland/ui/ulog.h` -- `ulog(s)` for a pre-formatted line, `ulogf(fmt,
  ...)` for a formatted one. Two calls on purpose: `--gc-sections` drops
  whichever an app doesn't use, so a lean app calling only `ulog()` never
  links `vsnprintf`. Diagnostics go to stderr, which the kernel routes to
  its log and a QMP test's console.

- **THE TOOLKIT OWNS THE KEYBOARD FOCUS RING: set `uapp_desc.focus`.**
  uapp click-updates it on a press and routes keys through it (Tab moves
  focus, other keys reach the focused widget) before `on_key` -- like it
  routes the mouse through `widgets`. A SEPARATE list from `widgets` (tab
  order isn't z-order; a focusable uses cut-down `_focus_ops`). Don't
  hand-roll `uui_focus_click`/`uui_focus_key` in `on_press`/`on_key`;
  `on_key` still fires so an app can re-read a widget the ring changed.

- **A WIDGET REPORTS ITS RECT THROUGH THE `bounds` OP; a test-facing
  geometry log is `uapp_log_layout(a, prefix)`.** `uui_widget_ops.bounds`
  is the getter `set_geometry` lacked; `uapp_log_layout()` walks the
  router and emits `<prefix>: layout <id> x y w h` per declared widget, so
  a test drives a control by asking rather than guessing pixels. Add
  `bounds` to a widget when a test needs to drive it; don't re-hand-roll
  the per-app geometry logger.

- **AN IMAGE IS DECODED IN RING 3, AND `lib/uimg.h`'s CODEC TABLE IS THE
  EXTENSION POINT.** `uimg_load(path, &im)` gives a `struct uimg` of
  0x00RRGGBB pixels -- exactly what `ugfx_blit()` takes -- and
  `uimg_free()` releases it. **Do NOT add an image parser to the
  kernel**: this is the one place toy-os deliberately does NOT copy its
  own font-parsing decision, because the console needs glyphs before any
  process exists and nothing in ring 0 needs a picture (Linux's only
  in-kernel image is an uncompressed PPM; Windows' codecs are user-mode
  WIC; a Wayland compositor is handed pixels). A second format is a
  `struct uimg_codec` row in `uimg.c` plus a file beside it -- never an
  `if (jpeg) ... else if (png)`. **Errors are negative errnos and the two
  are not interchangeable**: `-ENOTSUP` is a valid file this build
  refuses (progressive JPEG, CMYK, 12-bit), `-EINVAL` is a broken one,
  and `uimg_last_error()` carries the sentence. Formats are identified by
  PROBING magic bytes, not by extension.

- **`uui_image` IS THE ONLY WIDGET THAT OWNS MEMORY, AND IT MUST BE
  RELEASED.** It borrows the `struct uimg` (the app decodes and owns
  that) but owns the SCALED copy it caches, so an app that re-points one
  at image after image without `uui_image_release()` leaks a screen's
  worth of pixels each time. The cache is the reason the widget exists at
  all rather than three lines of `ugfx_blit()` per app: resampling a
  screen-sized picture costs tens of milliseconds and a repaint happens
  on every damage event. **Set `max_w`/`max_h` on any viewer of arbitrary
  files** -- natural size is the image's own, and `uui_layout` OVERFLOWS
  rather than shrinking, so a 4000px photograph otherwise asks for a
  4000px window and gets one.

- **THE WALLPAPER IS A REGISTERED SETTING, AND ITS VALUE IS A NAME.**
  `desktop.wallpaper` (a filename stem under `/usr/share/wallpapers`, or
  `none`) and `desktop.wallpaper_mode` (`fill`/`fit`), registered in
  `kernel/lib/wallpaper_config.c` as PERSIST-ONLY descriptors -- the
  kernel owns the description, the ring-3 desktop owns the behaviour and
  notices through the generation counter. So `config set
  desktop.wallpaper dusk` works from any shell, System Settings gets a
  row for free. An unknown name is STORED, not refused -- the desktop
  logs why nothing appeared and shows its plain colour, the same as a
  bogus cursor theme. **A name, not a path**, the same rule `fontface` and cursor
  themes follow; a picture elsewhere on the disk has to be copied into
  the directory first, and Image Viewer says so rather than failing
  quietly. **A GUI test that measures ink over the desktop must turn it
  off first** (`config set desktop.wallpaper none`): both the cursor and
  font tools count pixels differing from a FLAT background, and a
  wallpaper saturates them -- establish the precondition, do not weaken
  the assertion.

- **THE START BUTTON'S APPEARANCE IS A REGISTERED SETTING:
  `desktop.start_button` = `text` | `icon` | `both`.** Registered in
  `kernel/lib/start_button_config.c` as a PERSIST-ONLY descriptor
  sharing `/etc/desktop.conf` with the wallpaper -- which is what makes
  it `desktop.`-namespaced, since a namespace is the registered name of
  the FILE. Three choices rather than a boolean because that is XFCE's
  Whisker Menu verbatim (Icon / Title / Icon and title) and KDE's
  launcher option, and because a boolean cannot say `both`, which is
  what Windows 95 through 7 shipped. **The default is `text`, and that
  is a testing decision as much as a taste one**: the button's width is
  derived from what is in it and every window button starts to the right
  of it, so changing the default would move the whole strip under every
  pixel-based GUI check at once. **The mark is `/usr/share/icons/start.qoi`**,
  a name like any other icon (`START_ICON`), drawn by
  `tools/gen_icons.py`; it is deliberately NOT an app pictogram, since
  the button opens a menu of all of them. **`start_mark()` in
  `wm_render.c` is the ONE decision about whether a mark is shown** --
  the width, the drawing and `gui taskbar --json` all ask it, because a
  width that says "icon" while the drawing falls back to "text" is a
  narrow button with a clipped word in it, and that is exactly what
  missing artwork would produce if the three decided separately. **A
  tool that changes it must set it back**, since `make iso` re-seeds
  `disk.img` by sync and a written setting outlives the run.

- **AN ICON IS A NAME, IT IS CACHED, AND IT IS COMPOSITED.** A `.desktop`
  entry's `Icon=` is a NAME resolved to `/usr/share/icons/<name>.qoi`
  (freedesktop's rule, and the same filename-is-the-name convention
  `fontface` and cursor themes follow); a one-character value is the
  LETTER fallback instead, and a name whose file is missing falls back to
  it too -- Crash Test ships with no icon file so that path runs on every
  boot. **Ask `icon_get(name, size)`** (`userland/wm/icon_cache.h`),
  never `uimg_load()` at a draw site: it decodes and scales ONCE per
  (name, size), and a draw site that re-decoded would spend milliseconds
  per repaint producing last frame's pixels. **Blit with
  `ugfx_blit_alpha()`**: an icon is a rounded tile on transparency, and a
  plain `ugfx_blit()` writes the file's (0,0,0,0) corners as BLACK --
  which looks like a square icon rather than like a bug. Sizes come from
  ONE 64px master, box-filtered down; per-size art (freedesktop's
  `16x16/`, `48x48/`) is the fix if small icons ever look mushy, and it
  changes only the lookup.

- **A WINDOW IS MATCHED TO ITS LAUNCHER BY `AppId=`.** The taskbar needs
  an icon for a window it did not launch, and a window only knows the
  `app_id` its client declared -- which is not always the Exec basename
  (Shapes runs `/bin/wm/demos/shapes` and calls itself `gfxdemo`). The
  entry's optional `AppId=` states the pairing, defaulting to the Exec
  basename. This is freedesktop's `StartupWMClass`, which exists for
  exactly this mismatch.

- **A WINDOW'S TITLE BAR CARRIES ITS APP ICON, AND `title_icon()` IS THE
  ONE ANSWER FOR BOTH DRAWING AND CLICKING.** The far-left square of
  every title bar is the app's icon -- Windows' system-menu icon,
  Breeze's and XFWM's window-menu button. The client never supplies one:
  it is resolved from the window's `app_id` through
  `wm_window_icon_name()` (in `wm.c`, not `wm_taskbar.c` -- the taskbar
  was merely the first asker), exactly as Wayland's
  `xdg_toplevel.set_app_id` leaves the artwork to the compositor.
  **`title_icon(idx, &x, &y, &size)` returns the decoded picture AND the
  rect together**, so an icon that fails to decode yields no rect either
  -- never a clickable square with nothing visible in it. `gui windows
  --json` reports that same rect, so a test asserts on the compositor's
  own geometry rather than re-deriving it. **Left-clicking it opens the
  window menu**, on PRESS (a menu is `docs/gui-guidelines.md`'s
  documented exception to commit-on-release) and anchored under the icon
  rather than at the cursor, through the SAME `wm_open_window_menu()`
  the right-click uses -- a second copy is where Close drifts back from
  asking the client to seizing it.

- **TEXT ON A WALLPAPER IS `ugfx_draw_string_shadowed()`, NEVER A
  GUESSED `bg`.** Every `ugfx` text call blends a glyph's partial
  coverage against the `bg` the caller passes, which is right inside a
  widget and wrong over a wallpaper: the desktop's icon labels and its
  version watermark both passed the flat desktop blue, and every
  anti-aliased edge then carried a halo of a colour no longer on screen.
  **`UGFX_TRANSPARENT` as `bg`** blends against what is actually on the
  surface -- the one path that reads the surface back, so every other
  caller keeps the no-read-back contract. **The shadowed forms add
  legibility on top of that**, which transparency alone does not give:
  a wallpaper is a user-chosen photograph and no single ink works on all
  of them, so the string is drawn twice and the shadow's shade is
  DERIVED from the ink's luminance (the `ui_state_bg()` rule -- never
  hand-pick a contrast). Sampling the backdrop to choose an ink was
  considered and is worse: a gradient gives a different answer at each
  end of one string.

- **A KEY RELEASE IS `WIN_EV_KEY_UP`, AND THE FOUR MODIFIER KEYS ARE
  KEYS.** `WIN_EV_KEY` is a PRESS; a client that needs to know a key is
  HELD -- a game, a drag modifier, push-to-talk -- sets
  `uapp_desc.on_key_up`. A separate event type and a separate callback
  rather than a flag, so an app that has never heard of releases is
  unchanged (X11's `KeyRelease`, Wayland's `wl_keyboard.key` state,
  Windows' `WM_KEYUP`; press-only was this protocol's outlier). Five
  things to know. **A release cannot ride the console byte stream** --
  presses go through `tty_input()` and a terminal is bytes, so releases
  take a parallel transition queue (`keyboard_try_get_transition()`)
  carrying exactly what the byte stream cannot: all releases, and both
  edges of the modifiers. **`KEY_SHIFT`/`KEY_CTRL`/`KEY_ALT`/`KEY_ALTGR`
  exist ONLY on that path** and are never pushed as bytes, or pressing
  Shift would put a character in front of every shell. **A release
  carries what the PRESS produced**, not what the key would produce now
  -- press W, press Shift, release W reports `'w'`, and the driver
  remembers per keycode with FIRST PRESS WINNING so autorepeat under a
  changed modifier cannot strand the original. **An unmatched release is
  legal and must be tolerated** -- the WM claims Super and Alt+F4 on the
  press and delivers the release anyway, the shape an X11 grab produces.
  And **`wm_rawin.c`'s key slot is a QUEUE now**: a dropped press is a
  keystroke the user repeats, a dropped release is a key held forever.
  Proven by `tools/keyup_test.py`, whose load-bearing check uses
  `QMPSession.key_down()` -- `send-key` presses and releases together
  and so cannot tell a real release path from a synthesised one.
- **A SECONDARY CLICK IS THE CLIENT'S INSIDE ITS CONTENT AREA, AND THE
  WM'S EVERYWHERE ELSE.** A right-click on a window's own pixels is
  delivered as `WIN_EV_MOUSE_DOWN` with button bit `0x2`
  (`abi/win_proto.h`); the title bar, the border, the taskbar button and
  the app icon still open the window menu, and so does the desktop. That
  is the split Windows, X11 and Wayland all make -- a right-click in
  Notepad's text area opens Notepad's menu, not the system menu -- and
  toy-os used to be the outlier, seizing the button over the WHOLE
  window so that no ring-3 client could ever receive one. Three things
  to know. **The WM arms the press with the button that made it**
  (`content_pressed_btn` in `wm_internal.h`): a right-press armed and
  released on the LEFT button's bit would leave the client holding a
  `MOUSE_DOWN` with no `MOUSE_UP`, which is the same bug the left button
  already shipped once. **Toykit acts on `0x1` alone** -- a secondary
  press never arms a widget, moves the focus or commits a menu item, as
  in Qt and GTK -- while `uapp_desc.on_press` still receives every
  button, because only the app knows what a right-click means to it.
  And **the cost is real and was accepted**: the window menu is no
  longer reachable from the middle of an app's window, which is exactly
  the bargain every real desktop makes, and four other ways in remain.

- **MINESWEEPER IS THE FIRST GAME, AND IT IS AN ORDINARY CLIENT**
  (`userland/gui/apps/mines.c`, `/bin/wm/apps/mines`). It draws its own
  board rather than introducing a `uui_grid`, because one grid-shaped
  app does not justify a widget (the second-real-caller bar); a second
  one -- Sudoku, a memory game, a chess board -- is where that widget
  comes from. Two things worth knowing. **Its board palette is NOT the
  theme's**: the numbers 1-8 keep Minesweeper's own colours, because
  those are CONTENT a player reads the board with rather than chrome,
  the same argument syntax highlighting makes; the window, panel, menu
  bar and borders are all themed, and the 3D bevel is dropped for the
  flat look `docs/gui-guidelines.md` requires. And **flagging commits on
  PRESS**, the same documented exception a menu gets: a flag is instant
  in every implementation of this game, and it is undone by
  right-clicking again rather than by dragging off.
