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

- **A TITLE-BAR BUTTON IS A DISC, AND EVERY GLYPH CENTRES ON THE SAME
  PIXEL AS IT.** Adwaita's shape, chosen because a circle has no corners
  to alias at 18px. Four things, three of which were bugs first:
  - **THE CLOSE BUTTON IS GREY UNTIL HOVERED**, then red. Red marks the
    destructive action at the moment you are about to take it, not on
    every title bar all session -- where Windows, GNOME and KDE all
    ended up. The colour is DERIVED (`uui_state_bg()` shifting the
    button's own base), so nothing is hand-picked.
  - **A FILLED CIRCLE NEEDS AN ANTI-ALIASED OUTLINE OVER IT.** The
    midpoint rasteriser leaves a one-pixel spur at each cardinal point;
    at this size that does not read as a rough circle, it reads as a
    COG. Stroking the same radius with `GEOM_AA` in the same colour
    removes them.
  - **A DISC CENTRES ON A PIXEL AND A RECTANGLE CENTRES ON A SPAN**, and
    those agree only when the span is even. Placing a glyph at
    `(size - w) / 2` put it half a pixel left of its disc at 18px, and
    `(size - t) / 2` with a 1px stroke put the minimize bar a whole row
    high at EVERY size. Draw from `cx - h` to `cx + h` inclusive --
    always odd, always centred, at any font.
  - **HAIRLINE STROKES AT THIS SIZE.** A 2px wall around a 6px square
    leaves a 2px hole and stops reading as an outline; Adwaita, Breeze
    and Segoe MDL2 are all 1px here.

- **AN ICON COLUMN IN A SIDEBAR IS PER SIDEBAR, NOT PER ROW.**
  `uui_sidebar` reserves one gutter for every row when ANY heading has
  an icon. Indenting only the rows that have one pushed the headings
  right while their children stayed put, so the headings ended up
  further right than the rows beneath them and the hierarchy read
  backwards. An icon that only some rows carry cannot also be what sets
  their indent. **And the gutter counts towards natural_size** -- it is
  added to every row's text origin, so leaving it out asks for exactly
  that much too little and clips the longest label.
- **THE ICON CACHE IS THE TOOLKIT'S NOW, NOT THE WM'S, AND A SIDEBAR
  HEADING CAN CARRY AN ICON.** `icon_get()` moved from `userland/wm/` to
  `userland/lib/icon_cache.h` when `uui_sidebar` needed it -- a library
  that logs through one app's helper is a library only that app can
  link, so its `wm_logf()` became `ulogf()` and it dropped out of
  `EXTRA_OBJS_toywm` into `libuapp.a`. Three things:
  - **AN ICON IS A NAME, NOT A PATH**, as everywhere else here:
    `uui_sidebar_row.icon` is `"cat-input"` and resolves under
    `/usr/share/icons`. A missing file means no icon and a plain row,
    never an error -- which is what keeps it optional in fact.
  - **HEADINGS ONLY.** A category is stable and there are a handful;
    the rows under it come and go with what is registered, and an icon
    per setting is twenty pieces of art whose generic answers read worse
    than none. An icon on an ITEM is ignored rather than refused.
  - **THE SIZE IS FONT-DERIVED** (`ugfx_char_h()`), and both the drawing
    and the measuring ask the same helper for the indent -- a label
    measured at one indent and drawn at another is the bug that shape
    invites.
- **A WINDOW HAS TWO BUFFERS, AND THE COMPOSITOR NEVER READS THE ONE
  BEING DRAWN.** `WIN_REQ_PRESENT` flips which is front and returns the
  new index; the client draws into the other. Wayland's attach/commit,
  and it exists for the reason it does there: the compositor repaints on
  its OWN cadence -- the taskbar clock forces one every second -- so
  with a single buffer it eventually catches a frame halfway through,
  and an app that clears its surface first then flashes its background.
  That was the File Manager's flicker on every selection. Five things:
  - **BOTH BUFFERS STAY MAPPED, IN BOTH ADDRESS SPACES**, at `base` and
    `base + WIN_BUFFER_HALF`. Remapping one address per present would
    cost a page-table edit and a TLB flush per frame in two address
    spaces, on the hot path; mapping both once makes a flip a number in
    a message.
  - **THE SCARCE THING IS CONTIGUOUS PHYSICAL MEMORY**, not address
    space -- the slot is 64 MiB and the largest buffer is 8 MiB. Each
    buffer is a `pmm_alloc_contiguous()` run, and that is already what
    refuses a window when memory fragments.
  - **A FAILED SECOND ALLOCATION IS A SINGLE-BUFFERED WINDOW, NOT A
    REFUSED ONE.** It then tears exactly as every window did before,
    which is strictly better than not opening. `front` stays 0 and the
    flip is a no-op, so no caller needs a special case.
  - **THE FLIP HAPPENS INSIDE THE REQUEST**, before anyone is told: a
    client must know which buffer is safe the moment present returns,
    and the compositor must never be pointed at one the client has
    already started on.
  - **THE INVARIANT IS TESTED AS MEMORY, NOT AS A FLICKER.** Catching a
    torn frame means sampling fast enough to land inside one redraw --
    timing-dependent, and a check that passes more often the faster the
    machine gets. `winshare`'s "a present flips the buffer" KTEST writes
    a marker into the back buffer and asserts the compositor's front
    view cannot see it until the present.
- **THE LAYOUT LOG IS OFF UNLESS A TEST TURNS IT ON, AND DEDUPED WHEN IT
  IS.** Every Toykit app reports its widget geometry so a test can drive
  it by asking rather than by guessing pixels
  (`uapp_log_layout()`/`uapp_log_layout_line()`/`uapp_logf_layout()`),
  and it used to write that report EVERY FRAME, unconditionally, to the
  kernel log. Nine apps, twenty-odd lines a frame. `dmesg` on a machine
  with a window open was mostly one app repeating itself, and `dmesg -w`
  was a FEEDBACK LOOP -- printing a line moved the Terminal's caret,
  which redrew, which logged the new caret, which printed a line. Four
  things:
  - **`desktop.layout_log` gates it**, off by default, the same call
    `kernel.kbdtap` makes: the cost of recording is trivial, and the
    default is about what the machine SHOWS.
  - **READ ONCE, at first use.** A test sets it before launching what it
    means to watch, which `enter_gui()` does for every tool at once --
    re-reading per frame would put a syscall on the draw path to answer
    a question that does not change during a run.
  - **THE DEDUPE IS PER FRAME, NOT PER LINE.** An app emits several
    lines a frame and each differs from the one before it, so line-wise
    comparison would never match; what repeats is the whole BLOCK, which
    is what an idle window produces over and over. The toolkit collects
    the block during the draw and flushes it before `present()`, because
    only the toolkit knows when a frame has ended.
  - **AN ACTION IS NOT A LAYOUT LINE.** Notepad's `emit()` and uidemo's
    `logline()` serve both; only the layout callers were moved behind
    the gate. An action is an event a test waits for exactly once and
    must never be suppressed.
- **THE TERMINAL SCROLLS BY WHEEL AS WELL AS BY KEY, AND BOTH MOVE THE
  SAME STATE.** `on_wheel` adjusts the one `g_sb_view` Page Up/Down
  already moved, three lines per notch, clamped at both ends -- a
  terminal that scrolled differently by wheel than by key would be two
  notions of where the reader is. `uapp_desc.on_wheel` and
  `WIN_EV_WHEEL` already existed; the app simply never implemented the
  slot, which is CLAUDE.md's "a slot that is PRESENT and read by nobody"
  from the other direction -- here the toolkit offered it and the app
  declined.
- **`font glyph <char>` SHOWS WHAT WILL ACTUALLY BE DRAWN, AND IT READS
  BOTH SIDES.** `/bin/font` prints one glyph's coverage map, its line
  box and whether it has any ink -- from ring 0 (`QUERY_FONTGLYPH`) and
  from a client's own mapping of the atlas (`WIN_REQ_FONT`), and
  compares them with an FNV-1a hash over the coverage bytes. Five things
  to know:
  - **`ink NONE` is the point.** A glyph that rasterised to nothing is
    pixel-identical to a space, to a character the font does not carry,
    and to a font that failed to load; telling those apart had no answer
    anywhere, and the ambiguity cost a hunt (`docs/bugs.md`'s
    session-font cell that read as blank while 101/101 glyphs had been
    built). **`peak` is the harder half** -- a glyph can have ink and
    still be far too faint to read, which no yes/no flag expresses.
  - **THE TWO VIEWS ARE DIFFERENT MEMORY, and the disagreement is the
    diagnosis.** The hash is what makes that answerable without shipping
    a bitmap across; "these two are identical" is not a question to
    answer by eye.
  - **THE PICTURES ARE DIFFERENT DEPTHS ON PURPOSE.** Client: 8-bit
    coverage as a grayscale ramp, because how dark the ink is belongs to
    whoever draws it and a collapsed anti-alias is present, non-blank
    and unreadable. Kernel: a 1-bit ink map, because where the ink is
    belongs to ring 0 -- and a query record is capped at 256 bytes
    (`api/query.h`), which no cell's coverage bytes fit in.
  - **THE CLIENT HALF NEEDS A COMPOSITOR** (`win_server_request()`
    refuses everything with no role holder and no ring-0 presentation
    layer) and a SCHEDULED process -- so at a `#` prompt, where the
    legacy loader has no slot, only the kernel view is reachable. It
    says which refusal it hit rather than blaming the desktop.
  - **A CODEPOINT IS ACCEPTED AS WELL AS A CHARACTER** (`0x20`, `U+0020`,
    `32`), and that is not a convenience: a space cannot be passed as an
    argument through any shell here, and a space is exactly what you
    compare a suspected-blank glyph against.
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
  `apps/shell_complete.c` colours the shell's tab-completion with it.
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

- **`uui_meter` IS THE READING WIDGET, AND IT RESERVES EVERY ROW IT
  COULD USE.** A caption, a big number in its own font, a unit, an
  optional detail line and an optional bar -- `userland/ui/uui_meter.h`.
  It takes no input (no `hit`), because a reading is not a control, and
  the strings are POINTED AT rather than copied, like `uui_label`'s. The
  rule to know: **its height does NOT depend on which strings are set**,
  because a meter's content is a value that CHANGES -- counting the
  non-NULL ones gave Disk Mark's tiles a two-row box at layout time and
  four rows of content the moment a result arrived, drawn straight
  through the border. The big number's font is a field on the widget for
  the same reason `uui_label`'s is: `natural_size()` is asked long before
  any painting, so a value measured in one font and drawn in another
  lands in a box sized for something else.
- **LONG WORK BELONGS IN A CHILD PROCESS, NOT IN A GUI CLIENT'S EVENT
  LOOP.** The compositor pings every client
  (`WM_PING_INTERVAL_DEFAULT` / `WM_PING_TIMEOUT_DEFAULT`), so a client
  that blocks through a long job reads `(Not Responding)` for the
  duration. **Slicing it across `on_tick` is NOT enough** -- a slice can
  only be bounded between UNITS, and one unit here was a 1 MiB transfer,
  which is 1024 syscalls because `SYS_WRITE_MAX` is 1 KiB. Disk Mark
  shipped with a 120 ms slice budget that never got to run. Spawn a
  `/bin` program and poll it (the File Manager's `/bin/cp` pattern);
  the work is then also reachable from a shell, which is a different and
  more honest measurement. Three consequences: **a polled report is a
  SNAPSHOT, not a log** (`sys_read` carries 1 KiB, so appended results
  land past where a poller ever reads -- that showed as "Done." with
  empty tiles); **the poll is itself I/O**, so use ~500 ms rather than an
  animation cadence, which halved the numbers; and `uapp_busy_begin()`
  stays for work that is slow and SHORT.
- **A WIDGET ARRAY IS DECLARED TWICE, AND BOTH ARE LOAD-BEARING:
  `uapp_desc.layout` SIZES AND DRAWS, `uapp_desc.widgets` GETS INPUT.**
  Declaring only the first is a window that renders perfectly and cannot
  be clicked, with nothing anywhere to say so. Pass the SAME
  `struct uui_item` array to both (the router recurses into nested
  layouts through `uui_widget_ops.children`). And **a lone routed button
  reports through `on_widget` with the ITEM's id, not through
  `on_action`** -- `uapp.c` fires `on_action` only from
  `uapp_desc.buttons`, i.e. a `uui_button_group`, which is why
  Calculator (a grid of buttons in a group) looks like the opposite
  example.
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
  A sidebar deliberately has no collapsing and no second level of
  nesting; a HEADING may carry an icon (see the icon-cache entry).

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

  **A LAZY tree declares its parents instead of deriving them.**
  `UUI_TREE_CLOSED`/`UUI_TREE_OPEN` on `uui_tree_node.kind` mark a node
  whose children are simply ABSENT from the array until the app puts
  them there -- so parenthood cannot come from the depth run and has to
  be said. An expander click on one reports through
  `uui_tree_set_on_toggle()`'s callback and flips NOTHING: the app
  relists, rebuilds the array and calls `uui_tree_set_nodes_keep()`
  (set_nodes, but keeping the scroll position), and the new array's
  `kind` is the new truth -- GtkTreeView's
  test-expand-row/row-expanded split with the model left out. The
  collapse bitmap and its 64-node cap apply only to derived
  (`UUI_TREE_AUTO`) nodes, so a lazy tree may exceed
  `UUI_TREE_MAX_NODES`. Re-selection after a rebuild is the APP's, by
  its own key -- the File Manager keys on the PATH, because node ids
  are slots and a rebuild renumbers every slot. See
  `docs/decisions.md`.
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
  tunables). The shape is KDE System Settings': a `uui_sidebar` on
  the left, one page, a status bar. **The rename left a stale
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
- **THE KERNEL CONSOLE STOPS PRESENTING WHILE A COMPOSITOR OWNS THE
  SCREEN, AND KEEPS BUFFERING.** `vga_present()` returns early when
  `win_server_any()`; the console still draws into its own back buffer,
  and `vga_resume()` repaints on the way out. Linux's `KD_GRAPHICS`, and
  for the same reason -- a program's output must not paint over a
  graphical session. Without it ANY ring-3 process writing to fd 1 blits
  the whole text console over the desktop, which is not the writer's
  fault: `dmesg` covered 100% of the screen and DOOM's startup banner was
  how it was noticed. Three things. **`vga_present_force()` is the
  override, and a PANIC is its caller** -- guarding the routine path
  alone would have made every panic under a running desktop invisible,
  which is the opposite of what a panic report is for. **Default-safe**:
  a new routine caller gets the check without knowing it exists, and the
  two that mean to override say so. And **the console is not stopped from
  DRAWING**, only from publishing -- the back buffer is kernel-owned and
  nobody is looking at it, so the cost is nothing and the text survives
  to be repainted. `tools/console_bleed_test.py`.
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
- **A CLIENT NAMES ITS POINTER SHAPE, AND THE COMPOSITOR CLAMPS IT TO
  THE CONTENT AREA.** `WIN_REQ_CURSOR` (`abi/win_proto.h`) carries a
  `WIN_CURSOR_*` -- `DEFAULT`, `TEXT` or `WAIT` -- and the kernel forwards
  it to the compositor as `WIN_EV_CLIENT_CURSOR`. That split is Wayland's
  `cursor-shape-v1` and Win32's `WM_SETCURSOR`/`SetCursor`; a client here
  could not paint a pointer anyway, since it draws into its own buffer
  and the sprite is composited above every window. Six things.
  **Set on MOTION, not once at startup**, because a window is not
  uniformly one thing -- an editor's document wants the I-beam and its
  toolbar does not (X11's per-window `XDefineCursor` is why xterm shows
  an I-beam over its own scrollbar). **The list a CLIENT may name is
  SHORTER than the theme's**: no resize shapes, because the frame is the
  compositor's and a client naming `resize-h` would be claiming an edge
  it does not own -- `WIN_CURSOR_*` and `enum wm_cursor_kind` are
  deliberately two lists. **The clamp is the safety property**: a client
  is a process that answers a motion event some frames later, and a
  wedged one never answers at all, so `client_cursor_at()`
  (`wm_render.c`) honours the named shape ONLY inside that window's
  content area, only for the TOPMOST window at the point, and never
  under the taskbar or an open popup -- which bounds a stale answer to
  "wrong inside one window until the pointer crosses a boundary" instead
  of an I-beam stranded over the desktop. **The VALUE rides the event**,
  unlike thin `WIN_EV_CLIENT_TITLE`: it is one int, `WIN_REQ_WINDOW_INFO`
  has no return slot left, and a round trip would sit between the
  pointer entering a field and the shape changing. **The shape can move
  while the mouse does not**, so `wm.c` asks `wm_cursor_shape_changed()`
  beside `mouse_moved` -- a comparison against what was last drawn, not
  a dirty flag somebody has to remember to set. And **a widget declares
  it, an app only fills the gaps**: `uui_widget_ops.cursor` (NULL means
  `DEFAULT`) is what `uui_textbox`/`uui_textview` use, the router asks
  the deepest widget under the pointer on every motion, and
  `uapp_set_cursor()` is the escape hatch for a surface that is not a
  widget -- Notepad's document, the Terminal's grid (named ONCE at open,
  since the whole grid is text). `tools/cursor_ibeam_test.py`.
- **THE BUSY POINTER HAS TWO SOURCES, AND ONLY ONE OF THEM IS THE APP.**
  A client brackets work that is slow ON PURPOSE with
  `uapp_busy_begin()`/`uapp_busy_end()` -- the toolkit remembers what to
  restore, so "back to what?" is not a question every app answers
  differently -- and the request must go out BEFORE the caller blocks,
  which it does, because it is a syscall and the compositor reads its
  own queue. It DOES NOT NEST. Notepad's load/save and Image Viewer's
  decode are the callers. **The other source is the compositor**, which
  raises `WAIT` for a window that stopped answering pings and lets it
  OUTRANK whatever that window last named -- a wedged client cannot name
  a shape, because naming one needs the event loop that is wedged, so
  this is the case only the compositor can report. The two are
  distinguishable and a test must keep them apart: `/tests/hangclient`
  has a `b` key that is busy AND alive for exactly that reason.
- **EVERY CLIENT IS PINGED ON A CADENCE, not just one being closed.**
  `WM_PING_INTERVAL_DEFAULT` (2s) beside the existing
  `WM_PING_TIMEOUT_DEFAULT` (3s), both in `wm_internal.h`, both with a
  `gui` test lever (`pingtimeout`, `pinginterval`). Before this,
  `wm_client_ping()` had exactly ONE caller -- `wm_client_send_close()`
  -- so `(Not Responding)` could only ever appear during a close
  attempt, which is the one moment a hang is least surprising. Three
  things. **The interval is measured from the last ASK**, using
  `ping_sent_tick`, which outlives the serial a pong clears; worst-case
  detection is interval + timeout, because a window is only asked once
  the previous answer has landed. **A hung window nobody is closing
  still raises NO dialog** -- `check_liveness()` gates that on
  `close_asked_tick`, and a modal appearing on its own over a window the
  user never touched would be worse than the hang. And **the cost is one
  wakeup per client per interval**, since a ping is an event and an
  event wakes a client blocked in `sys_wait_event()`.
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

- **THE TRAY CLOCK OPENS A CALENDAR, AND THE PANEL OWNS IT --
  `userland/wm/calendar_popup.c`, not an app.** Clicking the clock opens
  a month grid anchored above it, today in the theme's ACCENT, `<` / `>`
  to page months, the title to snap back to today; clicking the clock
  again closes it. That is where Windows 11, GNOME Shell, Plasma's
  digital-clock applet and XFCE's clock plugin all put it -- a glance at
  the date costs no process spawn, no title bar and no taskbar button.
  Five things to know. **The days are not clickable**, deliberately:
  nothing stores events, and a selection highlight that does nothing
  reads as a broken control. **The panel is SIX week rows tall whatever
  the month needs**, or paging would move `<` and `>` out from under the
  cursor. **A dismissing click on the TASKBAR falls through and one
  anywhere else does not** -- the Start button acts on the same click
  that closed the popup (Windows and Plasma both), while a click on the
  desktop or a window is swallowed like any menu's. The clock itself is
  the third case, swallowed, which is what makes the second click a
  toggle rather than a reopen. **Its rect comes from
  `tray_clock_rect()`** (`wm_tray.h`), the same right-to-left walk that
  DRAWS the tray, because an app-registered tray item moves the clock
  and a hit-test phrased as "the right end of the strip" would then open
  the popup from the wrong control. And **`calendar_geometry()` is the
  one answer** the drawing, the hit-testing and `gui calendar --json`
  all ask -- the rule `gui taskbar` was rewritten for after reporting
  centres eight pixels off the real ones. A CLOSED popup reports TODAY's
  month, because that is what opening it now would show.

- **THE WEEK'S FIRST COLUMN IS A REGISTERED SETTING:
  `desktop.week_start` = `monday` | `sunday`.** Registered in
  `kernel/lib/week_start_config.c` as a PERSIST-ONLY descriptor sharing
  `/etc/desktop.conf` with the wallpaper and the Start button -- the
  namespace is the registered name of the FILE. Persist-only because the
  calendar that reads it is drawn by a ring-3 process and
  `setting_register()` takes function pointers a process cannot supply;
  `calendar_poll_config()` adopts it on the same `sys_fs_generation()`
  poll `taskbar_poll_config()` uses. **Two choices, not seven**: `cal(1)`
  offers these two, ISO 8601 says Monday, the US and Windows say Sunday,
  and nothing wants a week starting on a Wednesday. The default is
  `monday`. **A tool that changes it must set it back** -- the same rule
  the Start button's entry states, and for the same reason.

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

- **DOOM IS A VENDORED PORT, LINKED INTO ONE BINARY, AND ITS BACKEND IS
  NOT IN THE VENDORED DIRECTORY.** `userland/ports/doom/` is doomgeneric
  byte for byte; `userland/doom/dg_toyos.c` is the five `DG_*` functions
  and is ours. It is an ordinary `uapp` client -- window, chrome,
  taskbar button, icon, single-instance -- with no Doom-shaped special
  case anywhere in the WM or the kernel. Five things to know.
  **GPL-2 in an MIT repo**: an aggregation, and `EXTRA_OBJS_doom` links
  it into one binary so that nothing else can depend on it. **The
  vendored tree compiles with warnings OFF and the frame-size warning
  ON** -- the first are noise nobody is allowed to act on, the second is
  the Stack Clash guarantee. **`api/keyboard.h` and `doomkeys.h` cannot
  share a translation unit** (both define `KEY_F2`/`F3`/`F4`/`F10` with
  different values), so they meet through `dg_toyos.h`, whose `TOYKEY_*`
  copies `doom.c` static-asserts against the real macros. **The IWAD is
  not in the repository** -- `tools/fetch_wad.py`, and the app says so in
  its own window when there is none, rather than showing black. And
  **what the port actually needed was measured**: key releases were
  required, while the image ceiling (0.72 MiB of 1) and the growable
  stack (it fits in four pages) were not. See `docs/decisions.md`.
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
- **A DIRECTORY LISTING IS A WIDGET, `uui_fileview`, AND FOUR THINGS
  SHOULD BE DRAWING ONE.** Before it, three places listed a directory
  and every one was written from scratch: the WM's file picker
  (`userland/wm/file_picker.c`), Notepad's Open/Save dialog, and Image
  Viewer's sidebar -- all doing `sys_listdir()`, `dirsort()`, a
  synthetic `..` row and descend-on-activate. No real toolkit ships four
  (Windows has one `SysListView32`, Qt one `QFileSystemModel`, GTK one
  `GtkFileChooser`). **Image Viewer is converted; the other two are
  NOT** (`docs/roadmap.md` carries it), so the duplication is smaller
  and still there. Seven things to know:
  - **It COMPOSES `uui_table`** rather than reimplementing rows,
    scrolling, the sorting header and keyboard motion. What it adds is
    what is specific to directories.
  - **The caller owns the entry storage.** A full listing is
    `SYS_LISTDIR_MAX` x 80 bytes = 20 KB -- too big for a ring-3 frame
    (2 KiB) and wrong to bake in, since a sidebar wants 64 entries and a
    file manager wants 256. `uui_fileview_init()` takes the array.
  - **Filtering is a CALLBACK, not an extension list**, because Image
    Viewer decides what an image is by probing MAGIC BYTES -- a `.dat`
    holding a JPEG is listed and a `.jpg` holding text is not.
  - **`..` and directories lead under EVERY sort, and the comparator
    pre-multiplies by the sort direction to do it** -- `uui_table`
    multiplies by `sort_dir` itself, so the two cancel and the groups
    hold while the rows inside them reverse.
  - **A MARK NAMES A ROW, so every reload clears the marks.** They are a
    bitmap sized in ROWS (the listing plus the synthetic `..`, which is
    the off-by-one). A caller acting on marks must SNAPSHOT the paths
    first -- the File Manager does, because a copy in progress moves
    `SYS_FS_GENERATION` and reloads the panes underneath itself.
  - **It is usable through the ops table OR directly**, the same
    arrangement `uui_listbox` and `uui_table` have, because the WM's
    file picker is a screen-absolute modal the toolkit router never
    sees.
  - **`UUI_FILEVIEW_ICONS` is the first NON-TABLE mode** (list and
    details are both column sets on `uui_table`; icons is LVS_ICON to
    their LVS_REPORT/LIST): the grid draws, hit-tests, scrolls and
    moves the keyboard itself, over `icon_grid.h`'s cell math and
    `icon_get()`'s `folder`/`file` artwork -- while the SELECTION, the
    marks and the sort order stay the TABLE's state, so every path
    accessor answers identically in all three modes and switching back
    finds the header sort untouched (see `docs/decisions.md`). Dragging
    EMPTY SPACE sweeps a rubber band (`rubberband.h`'s second caller,
    the one it was shaped for) whose selection IS the marks, applied by
    toggle-to-match on every motion -- so a click on empty space is
    "unmark everything" with no special case. A caller that reloads on
    a timer must skip while `uui_fileview_band_active()` says a sweep
    is in progress: a reload clears the very marks the band is choosing
    (the desktop's `desktop_drag_active()` rule).
- **THE FILE MANAGER IS A TWO-PANE COMMANDER, NOT AN EXPLORER**
  (`userland/gui/apps/files.c`, `/bin/wm/apps/files`). Explorer's two
  primary verbs are copy/paste and drag-onto-a-window, and this system
  has neither a clipboard nor drag-and-drop -- both are their own
  roadmap milestone. Norton Commander's answer, kept by Midnight
  Commander, Total Commander and Krusader for forty years, needs
  neither: with two directories on screen the source is the active pane
  and the destination is the other one, so nothing is carried and
  nothing needs a carrier. Six things to know:
  - **File operations are CHILD PROCESSES.** F5 spawns `/bin/cp`, F8
    spawns `/bin/rm`, and `on_tick` reaps them with
    `sys_waitpid_nohang()`. One implementation of copying, testable as
    text at a prompt, and a failed copy cannot take the window down. The
    cost, stated rather than discovered: **no byte-level progress**,
    because a child reports an exit code and not a percentage.
  - **Marked files run through a QUEUE, one child at a time**, so the
    status line can say "Copy 3/7" and name the one that failed.
  - **Each pane carries its OWN path strip**, because one status line
    cannot say where two panes are, and the active one is drawn in the
    accent colour -- "which pane does F5 copy FROM" has to be answerable
    without asking.
  - **Each pane's directory is remembered in `/etc/files.conf`**, the
    per-app config convention's second user after `desktop.conf`. An
    explicit command-line argument WINS and is not saved: it is a
    statement about that launch.
  - **The View menu is per-PANE for the mode and per-WINDOW for the
    shape.** Details/Icons set the ACTIVE pane's `uui_fileview` mode
    (two panes in two modes is normal in any commander that grew a
    thumbnail view); "Second pane" collapses to one full-width pane --
    Tab still swaps WHICH one that is, and F5/F6 still aim at the
    hidden pane's directory, which keeps existing; "Folder tree" adds a
    lazy `uui_tree` column on the left whose rows navigate the active
    pane, with only user-expanded directories ever listed. All four
    choices persist in `/etc/files.conf`
    (`left_view`/`right_view`/`panes`/`tree`). The menu ticks its
    active options (`item_flags`), and a `uui_toolbar` under the bar
    presents Up/Refresh plus the four toggles through the same
    callback -- Up is greyed at the root.
  - **Refresh is `SYS_FS_GENERATION` polled in the tick**, the desktop's
    idiom -- one integer compare, no disk I/O, and a copy finishing in
    another process appears with nobody pressing anything. **An app that
    both watches the filesystem and writes to it must adopt the
    generation its OWN write produced**, or it reacts to itself: saving
    the pane directories bumped the counter, the next tick read that as
    an external change and reloaded both panes, and every navigation
    repainted twice half a second apart -- visible as a flicker.
    Measured at 2 frames per navigation before and 1 after. Every
    watcher needs this; it is why an inotify consumer tracks its own
    writes.
- **WHAT OPENS A FILE TYPE IS DECLARED BY THE APP THAT OPENS IT:
  `Handles=` on its `.desktop` entry.** `Handles=.txt .md .conf`, read
  by the File Manager when something is activated, matched whole and
  case-insensitively against the extension INCLUDING its dot (so `.md`
  does not claim `.mdx`). This is freedesktop's `mimeapps.list` shape
  with the MIME database left out, and leaving it out is the decision:
  a MIME registry is a second thing to seed and keep true, while this
  OS's one image decoder already identifies formats by sniffing. The
  cost is that an extension is a hint typed by a person. **A handler is
  spawned and NOT waited for** -- it is a launch, not an operation on
  files. **Notepad takes a path in `argv[1]`** because of this, and
  titles itself after it, which is also what lets a test tell "opened
  the file" from "opened a window".

- **A MOVE EVENT REACHES EVERY WIDGET AT EVERY DEPTH, AND A CLIPPED
  SUBTREE THE CURSOR HAS LEFT IS TOLD "NOWHERE".** `uui_router_motion()`
  walked ONE level of nested containers while press and wheel walked all
  of them, so nothing below two containers ever heard the pointer -- and
  a widget that never hears it simply never lights up, which reads as a
  missing feature rather than a routing bug. System Settings nests four
  deep, so every hover in it was dead, including a dropdown popup's
  rows. Three rules the fix encodes: every widget hears a move (unlike a
  press, which stops at the first taker), because more than one may need
  to CLEAR a highlight; a container with a `hit` clips its children, and
  a subtree the cursor is outside of gets a point no widget can contain
  rather than being skipped, or a row stays lit after the pointer has
  gone; and an OPEN OVERLAY gets the real point first and is skipped in
  the walk, since hearing the move twice would light a row and clear it
  again. See `docs/decisions.md`.
- **AN OPEN POPUP TAKES THE KEY BEFORE THE FOCUS RING DOES, AND A
  KEY-DRIVEN CHANGE IS REPORTED LIKE A CLICK.**
  `uui_router_overlay_key()` is the keyboard's half of `overlay_active`
  -- what makes typing into a dropdown work in an app with no focus ring
  at all. And `uui_focus_key()` now tells the app through `on_widget`
  with `UUI_REASON_KEY`, by the same id a click reports (looked up
  through `uui_router_id_of()`, because ids belong to the router); Tab
  is excluded, having changed no value. Before this a focused control's
  keyboard change was silently dropped by System Settings' Apply.
- **TYPING IN A LIST SEEKS, AND A REPEATED LETTER CYCLES WHILE A PREFIX
  EXPIRES.** `userland/ui/uui_seek.c` holds the search and BOTH
  `uui_listbox_key()` and `uui_table_key()` call it, so the dropdown
  popup, a standalone listbox, Task Manager and every `uui_fileview`
  gain it at once. Keys within `UUI_SEEK_WINDOW_MS` build a prefix
  ("h","e" -> Helsinki past Halifax); the same single letter again
  cycles to the next match and does NOT expire, because 'h' twice a
  minute apart should still reach the second h. It matches the string
  the user can SEE (the display name, "Los Angeles"), as a prefix, which
  is also why System Settings' "   (current)" suffix and
  `uui_fileview`'s trailing '/' on a directory do not interfere.
  A CLOSED dropdown takes letters too -- deliberately unlike the wheel,
  which it ignores, since a letter can only reach the control that has
  focus while the pointer merely passes over one.
- **A TABLE DECLARES WHICH COLUMN A LETTER MATCHES:
  `uui_table_set_seek_col()`.** GtkTreeView's `search-column`, and the
  reason it is not Win32's always-column-0 is that column 0 is the name
  in a file listing and the **PID** in Task Manager. It defaults to 0
  rather than to off: a table searching an unhelpful column says so the
  first time anyone types, while one that ignores letters fails
  silently. A negative column turns the search off. Two things to know.
  **The search walks VIEW positions, not app rows** -- a table's rows
  are pulled and sorted, so cycling in the data's order moves the
  selection somewhere the user is not looking, exactly the bug
  `uui_table_key()`'s arrows already avoid; `uui_table_source_row()`
  converts the answer back. And **`uui_table_set_rows()` must NOT reset
  the prefix**: Task Manager calls it on every refresh, so a resetting
  version drops the second letter of anything typed across a tick.
- **A WIDGET THAT TAKES KEYS STILL GETS NONE UNTIL THE APP ROUTES
  THEM.** `uapp.c` offers a key to `uapp_desc.focus` and then to
  `uapp_desc.on_key`; an app declaring NEITHER -- as Task Manager did
  from the day it was written -- reaches no widget's `key` op at all,
  and nothing says so. Its table's arrows, Home/End, paging and
  type-ahead were dead for months behind a suite that drives every
  check by mouse. Two ways to fix it and the choice is real: a
  `uapp_desc.focus` ring is the toolkit's own idiom and gives Tab
  between controls, and every widget that accepts focus draws an
  indicator now (see the entry below), so a Tab stop is visible;
  forwarding from `on_key` to the one widget that wants keys is what
  the File Manager does and what Task Manager now does, and is still
  the simpler answer for an app with exactly one key-taking widget. **The general check: when
  a widget gains a key handler, grep for the app's routing** -- the same
  shape as CLAUDE.md's rule about a slot that is present and read by
  nobody.
- **A FOCUS INDICATOR IS `uui_focus_ring()`, IN THE THEME'S ACCENT, AND
  THE WIDGET PASSES THE RECT.** Every widget that accepts keyboard focus
  draws one: a 1px ring in `UTHEME_ACCENT`, through the one helper in
  `uui_primitives.c`. Before it there were three geometries and a
  hover-derived tint between four widgets, and seven more that took keys
  and drew nothing at all -- so Tab moved an invisible cursor and the
  first thing typed went somewhere the user did not choose. Four things
  to know.

  **The ACCENT, not a wash of the control's own colour.** Focus and
  hover answer different questions -- "where will my typing go" against
  "what is under the pointer" -- and a shared visual vocabulary makes
  neither legible; `utheme.h` has named `accent` as the focus role since
  it was written. GTK, Qt and Windows all draw focus in the accent, and
  the File Manager had independently outlined its active pane in
  `UTHEME_ACCENT` already.

  **The CALLER passes the rect, because only the widget knows its own
  shape.** A list rings the focused ROW, a slider rings its thumb, a
  spinbox rings the whole control including its steppers (Up/Down are
  the spinbox's keys, not the field's). A ring the focus manager drew
  from `bounds` would be a 300px box round a table.

  **A ROW WIDGET FALLS BACK TO THE BOX** when the selection is scrolled
  out of view or there is none. An indicator that vanishes with the
  selection is the bug this exists to remove, and a widget can hold
  focus with nothing selected.

  **A SELECTION IS NOT AN INDICATOR**, which `uui_listbox` assumed for a
  long time in a comment: a selected row looks identical whether or not
  the list is the control answering the arrows, so two lists side by
  side say nothing about which one is listening.

  Tested at the widget level by `/tests/focusring_test` (all eleven,
  off-screen, each asserted BOTH ways -- absent unfocused, present
  focused, since a one-sided check passes on a control that rings itself
  unconditionally) and on-screen by `uidemo_test.py`. **Its
  load-bearing checks are the ROW ones**: a ring round the box and a
  ring on the row both put accent pixels on the surface, and only the
  height tells them apart.

- **A SETTING WHOSE CHOICES ARE DATA NAMES THEM ITSELF:
  `choice_label`.** `/etc/settings.d`'s `Choice.<value>=` lines cover a
  list a file's author can see; they cannot cover one that is COMPUTED
  (the timezones from `/etc/timezones`, the keyboard layouts from a
  directory) without regenerating the file whenever the data changes.
  Three sources, most specific first: `/etc/settings.d`, then
  `choice_label`, then the value itself -- so a client draws the ABI's
  `label` unconditionally and never decides, and an installation can
  still rename one choice. The VALUE stays the identity: `losangeles` is
  what is typed, matched and stored, and nothing parses a display name
  back. See `docs/decisions.md`.

## A WORKER THREAD MAY TOUCH NOTHING IN TOYKIT EXCEPT `uapp_post()`.

Every real toolkit has this rule and none of them enforce it: AppKit is
main-thread-only, Qt widgets are main-thread-only, GTK the same. Toykit
is no different and cannot be -- the widget tree, the canvas and the
window buffer are all plain process memory that the main thread may be
reading at the instant a worker writes.

So the division is: **a worker computes into memory it owns and posts
two numbers; the main thread runs the callback and touches the widgets.**
`uapp_post(a, a0, a1)` wakes this program's own event loop and
`uapp_desc.on_user` receives the pair on the main thread. Two numbers
rather than a pointer because an event is a MESSAGE -- fixed-layout and
readable in a log, the same reason the protocol spells every other event
out field by field. A worker with a bigger result puts it somewhere both
threads agreed on and posts an index.

**The wake is why this exists at all.** A Toykit app blocks in
`SYS_WAIT_EVENT`, so without a post a worker's completion is only
noticed on the next `tick_ms` -- which trades one polling cadence for
another, and the cadence is the thing worth deleting. Qt's `postEvent`,
GTK's `g_idle_add`, Win32's `PostMessage` and the eventfd a Wayland
client puts in its poll set are all the same mechanism.

**A CLIENT MAY POST ONLY TO ITSELF, AND ONLY `WIN_EV_USER`.**
`WIN_REQ_EVENT_PUSH` is otherwise compositor-only, because it is the one
request that reaches across processes and a client synthesising a
keystroke into another program would be the end of the protocol's access
control. Restricting the TYPE as well as the target is what keeps that
true of a client's own queue too: a stray self-post can never be
mistaken for input.

**AND LONG WORK STILL USUALLY BELONGS IN A CHILD PROCESS.** This does
not replace the `/bin` pattern -- Disk Mark spawning `/bin/diskbench`
and the File Manager spawning `/bin/cp` are still right, because a
`/bin` program is independently useful and independently testable where
a thread is neither. The case for a thread is work whose RESULT must
live in the app's own memory: a decoded image is the example, since
handing one back through a pipe that carries 1 KiB a read is absurd.

## A MENU BAR IN AN APP WITH ROUTED WIDGETS MUST BE `uui_menubar_ops`.

`uui_menubar` has two interfaces now and picking the wrong one is a
silent bug rather than a style choice.

The HAND-ROUTED one is what Notepad uses: the app owns its
`on_press`/`on_motion`/`on_release` and calls `uui_menubar_press()` and
friends from them. That is correct for an app whose other controls are
hand-drawn, and Notepad declares no routed widgets at all.

**AN APP THAT DECLARES `uapp_desc.widgets` MUST USE THE OPS TABLE
INSTEAD**, because a popup drops down OVER whatever is below the bar and
`uui_router_press()` runs before the app's own `on_press` (`uapp.c`).
Hand-routing there means a click on the File menu's first row ALSO lands
on the widget underneath it -- in Terminal, on a tab. That is the exact
problem `uui_widget_ops.overlay_active` exists to solve: a widget
claiming an overlay is offered every press first, with no hit test. The
popup is painted from `draw_overlay`, which `uui_router_draw()` runs
after every widget's `draw`, so the z-order comes out right with nothing
for the app to sequence by hand.

The one wrinkle is how a commit gets out. The ops `release` slot returns
only "did anything change", so there is nowhere for a code to come back
through: the widget PARKS it, and the app takes it with
`uui_menubar_take_code()` when the router names the widget through
`uapp_desc.on_widget`. Taken once and cleared, because a redraw must not
replay a command.

**KEYS ARE STILL THE APP'S.** There is no `key` slot on the table, so
F10 and the arrows are handled in the app's own `on_key` through
`uui_menubar_key()` -- which also means an app is free to decide that an
open menu outranks whatever else wants the keyboard. Terminal needs that
ordering explicitly: every key it does not claim is a byte for the shell.

**A HIDEABLE MENU BAR NEEDS A WAY BACK, AND F10 IS IT.** Terminal's
View > Menu Bar hides the row (Konsole's, because in a terminal a row of
chrome is a row of the product), and F10 REVEALS a hidden bar as well as
opening it, so the toggle is never a one-way door. Konsole's own
Ctrl+Shift+M is unavailable here: Ctrl folds `M` to 0x0D, so the binding
would be indistinguishable from Shift+Enter (`api/keyboard.h`).

## A TOOLBAR PRESENTS THE MENU'S COMMANDS, AND ONE `item_flags` ANSWERS FOR BOTH.

`userland/ui/uui_toolbar.h`. An item is an icon name, a tooltip and a
CODE -- the same code its menu item commits -- and enabled/latched
state is asked through the SAME `item_flags(int code)` callback the
menu bar uses, so a latched button and a ticked menu item cannot
disagree (Qt hosts one QAction in both places; this is that shape
without the object -- see `docs/decisions.md`). A checked item draws
PRESSED-IN via `uui_state_bg()`, the latched look every desktop gives
a view toggle; a disabled one takes no hover and no click.

**Tooltips ride the app's tick.** Hover records when it started,
`uui_toolbar_tick()` called from `on_tick` says when to repaint, and
the tip draws from `draw_overlay`, slid inward at the surface's edges.
An app that never ticks gets working buttons and no tooltips. Commits
are PARKED (`uui_toolbar_take_code()`), the menu bar's arrangement and
for the same reason -- the ops `release` slot can only say "changed".
First caller: the File Manager (Up/Refresh and the four View toggles,
with Up greyed at the root through the same flags callback).

## A TAB IS A SESSION, AND `uui_tabs` IS THE STRIP.

`userland/ui/uui_tabs.c` draws a row of tabs and reports clicks; it
knows nothing about what a tab CONTAINS. **The caller owns the array**,
exactly as `uui_fileview` owns its entries, and a label is a pointer the
caller must keep alive -- which is what lets Terminal point one at its
session's own title buffer and have a shell's OSC title appear with
nothing copied.

Three things the widget decides, because getting them wrong is what
makes a tab strip annoying rather than broken:

- **Equal shares, with a floor.** A tab whose width tracked its title
  would move its own close box while the pointer travelled to it.
- **`hit` is a BOOLEAN.** Returning the index would make tab 0 -- the
  one whose index is falsey -- report as not hit, which is CLAUDE.md's
  standing widget trap and would silently make the first tab unclickable.
- **Close commits on RELEASE**, like every other control here, and it
  matters more than usual because the action destroys something: a press
  dragged off the close box does nothing.

**IN TERMINAL, A TAB IS ITS OWN pty, SHELL, GRID, SCROLLBACK, ALTERNATE
SCREEN AND TITLE** -- Konsole's model, and GNOME Terminal's and Windows
Terminal's. The strip belongs to the application because only the
application can give a tab a terminal's state; WM-level window tabbing
(KDE has it) would make each tab a whole process and could show neither
a shell's title nor its scrollback. Per-tab job control is free: each
shell is spawned `PGID_NEW`, so `Ctrl-C` reaches the job in the tab you
are looking at.

**THE SELECTED TAB IS A LIGHT LIFT WITH AN ACCENT BAR ON TOP.** It
takes the theme's FIELD colour, rounds its top corners and carries a 2px
accent along its TOP edge, the way VS Code marks one
(`tab.activeBorderTop`). **Resting tabs are filled with the CONTROL
colour**, which is darker than the strip's own ground, so the strip
reads as wells with one tab raised out of them.

Both of those are corrections and the reasons are worth keeping. The
accent sat on the BOTTOM edge, where it is a thin line directly above a
terminal's black page and has the least contrast of anywhere it could
be; and resting tabs were left the strip's own colour, so the selected
one was lifted by TWENTY units out of 255 -- which shipped, and was
reported as hard to tell apart with three tabs open. Thirty is the
current lift and `uterm_test`'s c17 has its floor at twenty-five, above
what was reported, because a check that accepts twenty accepts the bug.

**A NEW TAB IS APPENDED, AND THE STRIP'S ORDER IS NOT SLOT ORDER.**
Terminal keeps sessions in a fixed array and RECYCLES slots, so a strip
built by walking that array appends only by accident -- it does so while
slots fill 0,1,2, and stops the moment a middle tab is closed, because
the next new tab takes the freed slot and reappears in the hole halfway
along. `g_tab_slot` is the order of record and `open_tab()` appends to
it, which is what every terminal and every browser does.

**The selection follows the SLOT, not the index**, for the same reason:
closing a tab to the left of the selected one shifts every index right
of it, so clamping an index silently moves the selection onto a
different shell. Only when the selected tab is the one that WENT does
its index become the right answer -- it then lands on whatever took its
place, which is Konsole's behaviour and every browser's.

**And a session's default title carries no number**: it is `Shell`, not
`Shell 3`. The strip numbers by POSITION and the slot is recycled, so
the two disagreed as soon as a middle tab was closed -- the third tab
would read `3: Shell 1`.

**THE TABS ARE NUMBERED WHEN THERE IS MORE THAN ONE**
(`uui_tabs.numbered`): `1: label`, Konsole's `%n: %d` and iTerm2's. It
answers two questions at once -- the highest number is how many there
are, the lit one is where you are -- and it earns its place because
terminal labels COLLIDE by default: every tab in the same directory
reports the same title, and three tabs reading `/` are indistinguishable
however they are shaded. The widget draws it rather than the caller
baking it into a label, because a label is not owned and a shell
rewrites its own asynchronously. The number goes FIRST and the label
takes what is left, so a long title clips and the number never does.

Filling the selected tab with the PAGE's own colour instead -- Windows
Terminal's and Konsole's merged tab -- was built and rejected: against
near-white chrome a terminal's black page makes the selected tab a black
block. See `docs/decisions/gui.md`. Every colour here is the theme's,
which is what that version could not be, since the page colour had to
come from the caller.

**THE `+` IS PINNED AT THE RIGHT END, AND IT TAKES ITS WIDTH BEFORE THE
TABS SHARE WHAT IS LEFT.** `uui_tabs.show_new` plus `on_new`, off by
default -- GtkNotebook's action widget and QTabBar's corner widget, and
the placement GNOME Terminal and macOS Terminal use. Pinned rather than
riding after the last tab (Chrome's, Windows Terminal's) because on an
equal-share strip a button that walks rightward as tabs open is a target
you have to look for. Subtracting its width FIRST is what keeps it off
the last tab's close box.

**ONE TAB STILL SHOWS THE STRIP**, which is a deliberate reversal of
Konsole's default: the strip carries the `+`, and a control that only
appears once you already have what it creates is not a control. The cost
is a row of chrome on a single-shell window; what is bought is a
geometry that does not jump when a second tab opens.

**EACH SESSION HAS A READER THREAD, AND IT TOUCHES ONLY ITS RING.**
Before tabs, Terminal polled one master every 30 ms; N tabs would have
been N polls. A thread per session blocks in a real read, copies into a
single-producer/single-consumer ring and calls `uapp_post()`; the MAIN
thread drains every ring and feeds the parsers. **There is no `tick_ms`
and no `on_tick` left at all** -- the window blocks and wakes only when
something has actually happened. A full ring makes the reader WAIT
rather than drop: dropping would corrupt a screen in a way nobody could
diagnose from the result.

## A TITLE COMES FROM THE SHELL, AS AN OSC.

`ESC]0;<text>BEL` is what every terminal has taken as "set your title"
since xterm defined it. `kernel/lib/ansi.c` resolves it to `ANSI_OSC`
with the text in `p->osc` (api/ansi.h), and `/bin/tosh` emits one **per
directory change** -- not per prompt, because `prompt()` is called
several times per repaint and a title emitted there would put an escape
sequence between every keystroke and its echo.

**A CONSUMER MAY IGNORE IT, AND THE PHYSICAL CONSOLE DOES.** The
sequence is swallowed either way (`vga.c`'s `default:` case), so a shell
that sets a title is correct on a terminal with nowhere to put one --
the same contract the alternate screen already has.

Two parser rules worth knowing before extending it: **an OSC with no
`;` is DROPPED rather than guessed at**, because "which string is this"
has no safe default; and a code the terminal does not implement (a
hyperlink, a clipboard write) is **swallowed, not reported**, or every
one of them would rename the tab. The title itself is TRUNCATED at
`ANSI_OSC_MAX` rather than refused -- the one place this parser guesses,
and deliberately, since a title is decoration and losing its tail beats
losing the whole thing.
