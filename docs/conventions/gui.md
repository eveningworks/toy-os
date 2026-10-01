# GUI, Toykit and the desktop

Toykit widgets, TWP/TWS, the compositor, the window manager and the
cursor. `docs/gui-guidelines.md` is the separate, binding spec for how
anything drawn should LOOK and BEHAVE; this file is how the machinery
works and what breaks if you edit it the obvious way.

These are the conventions indexed by headline in
`docs/conventions/INDEX.md` but not carried in full there -- the index
holds the rule and this file holds the reasoning and the trap. **The
headline of every entry here also appears in that index**, so a session
can see the warning without loading the body; come here when you are
actually working in this area, or when a headline there tells you
something you did not know. CLAUDE.md itself carries only the
conventions that fire UNANNOUNCED.

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

  **THE FIVE CHOKEPOINTS, and `tools/check_text_measure.py` FAILS THE
  BUILD when something goes round them.** `ugfx_text_width()` for a
  whole string, `ugfx_text_width_n()` for a prefix (a caret, a selection
  edge), `ugfx_text_fit_chars()` for how much fits, and
  `ugfx_text_index_at_x()` for the inverse -- which character a click
  landed on. Reserving space for text that does not exist yet is the
  fifth and is NOT a measurement: `ugfx_char_advance('0')` for a column
  of digits, `('n')` for ordinary text. The check flags an arithmetic
  use of the cell width whose other operand is a NAME rather than a
  literal, because a variable paired with the cell is nearly always a
  character count while a literal is a layout unit (`char_w / 2` of
  padding is fine and always was). Waive with `text-measure-ok:
  <reason>`; the legitimate reason is a monospace bracket.

  **A SLICE IS MEASURED ON THE SLICE.** Kerning is counted between
  adjacent characters, so the width of `str[start..i]` is not the
  difference of two offsets into the whole string -- it includes a kern
  pair the drawing of that slice never applies. `uui_textbox` measures
  the exact `shown[]` buffer it draws, which is why its caret, its
  selection and its hit test cannot drift apart.

- **AND `UUI_COLOR_UNSET` IS A COLOUR UNTIL A WIDGET RESOLVES IT.** It
  is `0xFF000000`, so a widget that passes `l->bg` straight to
  `uui_state_bg()` draws opaque BLACK -- and not obviously: the hover
  state of black is a dark grey bar that looks like a deliberate, if
  ugly, choice. `uui_radio_list` and `uui_checkbox` both did this, and
  it only surfaced when an app followed the convention below instead of
  naming theme colours at build time the way System Settings does. If
  you add a `bg`/`fg` pair to a widget, resolve it in `draw()`.

- **A WIDGET RESOLVES ITS COLOURS WHEN IT DRAWS, NOT WHEN IT IS BUILT.**
  `UUI_COLOR_UNSET` in an init means "ask the theme", and
  `UUI_COLOR(v, UTHEME_ROLE)` at the draw is what asks. An init that
  copied `UTHEME_*` into the widget would freeze whatever palette was
  live at construction -- and every app builds its widgets once, at
  open -- so a theme change would reach only widgets created afterwards.
  GTK and Qt both resolve a style at paint for the same reason. An app
  that sets a colour explicitly still wins, because any other value is
  not the sentinel.

  **THE PALETTE HAS THREE GREYS AND THEY ARE NOT INTERCHANGEABLE.**
  `border` frames a window or a menu and is nearly black; `outline`
  edges something you can click (a field, a scrollbar thumb); and
  `separator` is a decorative rule (a table's grid lines). Drawing one
  with another's colour is immediately wrong, and Qt separates them too
  (WindowText against Mid/Dark). `selection_bg` is likewise not
  `accent`: a selected row here is a pale wash under ordinary dark text,
  while `accent` is the saturated colour a focus ring uses, and a dark
  mode has to move them independently.

- **`utext` IS A FIXED GRID, AND ITS CALLERS OWE IT A MONOSPACE FACE.**
  It places every glyph at `col * ugfx_char_w()` and hit-tests by
  dividing, which is a measurement only in a monospace face. Notepad's
  `doc_font()` and `uui_textview.c`'s `grid_font()` are the two brackets
  that pay that debt; `uui_textview` had no bracket at all until
  2026-09-14, so it drew the proportional session face at cell pitch --
  ragged text with a caret nowhere near the click. **And every early
  return inside such a bracket leaks the face**, leaving the whole
  process measuring in it.

  Two related facts. `gfx_draw_char()` still paints the WHOLE cell,
  background included, because the console depends on it (a character
  replacing a wider one must leave nothing behind); string drawing
  paints only the advance. And the CONSOLE stays fixed-cell on purpose
  -- a terminal is monospace by definition -- so a proportional face
  gets a cell as wide as its widest advance there.

- **A SHARED GEOMETRY HELPER MEASURES IN WHATEVER FACE THE CALLER
  CURRENTLY HAS SELECTED, AND `ugfx_set_font()` IS PER PROCESS.** Font
  selection is a process-wide register, not an argument, so
  `uui_scrollbar_natural_size()` and every other toolkit measurement
  answers a different number depending on WHO IS ASKING and WHEN. An app
  that switches faces mid-frame -- Terminal and Notepad both do, to draw
  a document in the monospace family while their chrome stays in the
  interface one -- must call each helper from INSIDE the same bracket
  every time, or the two call sites disagree.

  The failure is silent and asymmetric: a scrollbar DRAWN in one face and
  HIT-TESTED in the other is a thumb that tracks the pointer at the wrong
  rate (measured: 52px of travel per 60px dragged), and a chrome height
  computed both ways clips a row. Neither looks like a font bug. The fix
  is to pin the face inside the helper's own wrapper --

  ```c
  static int bar_w(void) {
      const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_REGULAR));
      int w = 0, h = 0;
      uui_scrollbar_natural_size(&w, &h);
      ugfx_set_font(was);
      return w;
  }
  ```

  -- so there is one answer rather than one per call site. A single-face
  app never sees this, which is why it survived until the interface
  became proportional.

  **AND EVERY EARLY RETURN INSIDE A BRACKET LEAKS THE FACE**, leaving the
  whole process measuring in the document's font until something else
  sets it -- a handler's `if (!buttons) return;` is enough. Put the
  acquire BELOW the exits that measure nothing, and restore on the ones
  that remain; six of these were in the first version of Notepad's and
  Terminal's brackets.

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
- **A NEW OR REDESIGNED APP FOLLOWS THE APP DESIGN LANGUAGE** at the top
  of `docs/gui-guidelines.md`: neutral grey chrome, a command bar coloured by
  action role, the content as the hero, soft accent selection, rounded
  shapes, colour by meaning, a full screen with floating controls. The
  Image Viewer is the reference; mock a redesign up before coding it.
- **A HELD BUTTON FREEZES THE HOVERED WINDOW: no leave, no enter, until
  the release** (`wm_update_content_hover()`). Wayland's implicit grab.
  A leave reaches a client as a move to (-1,-1) with NO button held,
  and an app tracking a drag in `on_motion` reads that as the release
  -- so the WM sending one on the first held move killed every
  app-level drag (the Image Viewer's pan never moved) while widget
  drags, which the router grabs, still worked. A test of a drag must
  park the pointer outside what it compares: the cursor sprite alone
  makes two frames differ.
- **A WINDOW HAS THREE BUFFERS, AND A CLIENT DRAWS ONLY INTO ONE THE
  COMPOSITOR HAS RELEASED** (`WIN_EV_BUF_RELEASE`). A present names the
  new front; the compositor reads it on its OWN cadence -- the taskbar
  clock forces a repaint every second -- so a client that drew into a
  buffer the compositor might still be reading put a frame halfway
  through on screen, and an app that clears its surface first flashed
  its background. That was the File Manager's flicker, twice: on every
  selection with one buffer, and while scrolling with two (the client
  repainted the old front while a preempted composite was still reading
  it). Wayland's attach/commit plus `wl_buffer.release`. Six things:
  - **BOTH BUFFERS STAY MAPPED, IN BOTH PROCESSES.** Each is a named shm
    object the CLIENT creates (`WIN_BUF_NAME_FMT`: pid, slot, buffer)
    and grants to the compositor, which maps it once and re-opens the
    name only when a present carries a higher generation (a resize
    replaced the object). Remapping per present would cost a page-table
    edit and a TLB flush per frame in two address spaces, on the hot
    path; mapping both once makes a flip a number in a message.
  - **THE MEMORY IS THE CLIENT'S, SO THE CLIENT CLAMPS.** The kernel
    used to refuse an oversized window because it was doing the
    allocating; `uapp_resize()` bounds the size where the allocation is
    now (`WIN_CLIENT_MAX_W/H`). Nothing is contiguous any more -- the
    frames behind an shm object are whatever the allocator has.
  - **A FAILED SECOND BUFFER IS A SINGLE-BUFFERED WINDOW, NOT A REFUSED
    ONE.** It then tears exactly as every window did before, which is
    strictly better than not opening. `front` stays 0 and the flip is a
    no-op, so no caller needs a special case.
  - **THE FLIP IS THE CLIENT'S, AND THE FRAME NAMES ITSELF**: a present
    says which buffer, which object (its generation) and how big, so the
    compositor adopts the geometry of what it is about to show. There is
    no second record of a buffer's size anywhere (`lib/uwmchan.h`).
  - **A BUFFER COMES BACK ONLY WHEN THE COMPOSITOR SAYS SO.** It sends
    `WIN_EV_BUF_RELEASE` for the old front as it handles the present
    that replaces it -- a single-threaded compositor has no composite in
    flight then. `ui/uapp.c`'s `surf_back()` picks the back buffer when
    a FRAME STARTS, not at present time (when the old front is always
    still busy), so with prompt releases two buffers alternate and the
    third is made only under contention; with none free the frame stays
    dirty until a release wakes the loop, and a release lost to an
    overflowed queue is taken back after 250 ms rather than freezing the
    window. `tools/half_frame_test.py` measures it.
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

- **THERE ARE TWO FONT FAMILIES: A PROPORTIONAL ONE FOR THE INTERFACE
  AND A FIXED-CELL ONE FOR TERMINALS AND CODE.**
  `system.font_face` and `system.font_mono`, both a face name, both
  rasterized by `/bin/fontd` at the one shared `system.font_size`. GNOME
  keeps `font-name` beside `monospace-font-name` and Windows pairs its
  UI font with Consolas, for the same reason: no face is good at both
  jobs, and a grid of cells drawn in a proportional face does not line
  up.

  **A WIDGET SAYS NOTHING AND GETS THE UI FAMILY.**
  `ugfx_font_session(weight)` IS the UI family -- which is why adding
  the second one changed no existing call site -- and
  `ugfx_font_mono(weight)` is the other. Ask for mono only where a
  fixed cell is part of the MEANING: a terminal grid, a hex dump, a
  code view. Naming the unusual case is what makes a terminal silently
  drawing proportional text a visible mistake rather than an inherited
  default.

  **THE SLOT IS `family * weights + weight`, ENCODED INTO THE OLD
  NAME.** `font.0`/`font.1` still mean exactly what they meant -- the UI
  family's two weights -- and mono was added at `font.2`/`font.3`, so
  the ABI grew by addition and no client had to move.
  `FONT_SHM_SLOT()` (`abi/font_shm.h`) and `UGFX_FONT_SLOT()`
  (`ui/ugfx.h`) are the two sides of that arithmetic and must agree.
  The `family` field was APPENDED to `struct font_shm` for the same
  reason: every table in it is reached through an explicit offset, so a
  reader that predates the field computes the same addresses and simply
  never looks.

  **THE KERNEL FALLBACK HAS NO FAMILIES AND DOES NOT NEED ANY.** Its
  baked tables are monospace, so a mono slot falling back to them is
  right and a UI slot falling back to them is what every client did
  before this existed. Only the WEIGHT crosses that seam.

  **NOTHING REFUSES A PROPORTIONAL FACE IN THE MONO SLOT** -- whether
  every advance is equal is knowable only once the face is rasterized,
  which happens in fontd -- but fontd says so in its log and the
  published header carries a `monospace` flag for a client that wants
  to check.

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
- **A WINDOW HAS ROUNDED CORNERS UNLESS IT IS MAXIMIZED, AND THE CORNER
  IS BLENDED OVER WHAT IS REALLY BENEATH.** `wm_render.c` saves the
  pixels under each corner before a window paints and blends them back
  afterwards by the arc's coverage (`corners_save()` /
  `corners_round()`), so the edge is anti-aliased against the wallpaper
  or the window below rather than a guessed colour. It works because
  the compositor repaints everything under the damage box back to
  front (`docs/decisions/gui.md`); nothing else knows a corner is
  transparent, and nothing needs to. **The radius is half the line
  height**, 8 px at the default font (Breeze's; a third was tried and
  read as square), and **a maximized window is
  square** -- Breeze's and Windows 11's rule both. **Hit testing stays
  rectangular**: a click in a corner belongs to the window. Two
  consequences for tests: a pixel sampled at a window's outermost
  corner is backdrop now, so sample inward; and a test that COUNTS
  colours in a content rect sees the backdrop's in the bottom corners.
  **THE MAXIMIZE BUTTON SHOWS A RESTORE GLYPH WHILE MAXIMIZED** (two
  overlapping squares), and **A DOUBLE-CLICK ON THE TITLE BAR TOGGLES
  MAXIMIZE** -- `wm_input.c` names the window by its client ids rather
  than its index, which `bring_to_front()` moves, and uses the desktop
  icons' threshold. Both route through `wm_toggle_maximize()`, so a
  fixed-size window refuses all three the same way.
- **"MAXIMIZED" STOPS AT THE TASKBAR AND KEEPS THE TITLE BAR; FULLSCREEN
  IS A FLAG BESIDE THE STATE, AND `window_has_chrome()` IS THE ONE PLACE
  IT MEANS "NO CHROME".** `wm_toggle_maximize()` and
  `wm_set_fullscreen()` (`userland/wm/wm_input.c`) share the shape: the
  client is PROPOSED the new content size (the configure/ack the resize
  grip uses, Wayland's `xdg_toplevel.configure`) and `wm_client.c`
  adopts w/h on the next present. Fullscreen is `win->fullscreen`, not
  a fourth `enum window_state`, so every `state ==` test stays true and
  `fs_prev` says where leaving lands. A window that has ADOPTED the
  screen's size (`wm_top_covers_screen()`) hides the wallpaper, the
  taskbar and everything under it -- and the taskbar takes no clicks --
  but until the adoption the desktop is still drawn under the old
  buffer. A client asks with `uapp_set_fullscreen()`
  (`WIN_REQ_FULLSCREEN`); the window menu offers it for any resizable
  window, and **Alt+Space reaches that menu in fullscreen**, which has
  no title bar and hides the taskbar. Never persisted (`wm_geometry_save()` stores the saved rect).
  See `docs/scanout-design.md`.
- **A FULLSCREEN CLIENT MAY BE LEASED THE DISPLAY'S OWN SCANOUTS, AND
  WHILE IT HOLDS THEM THE COMPOSITOR DRAWS NOTHING.**
  `WIN_REQ_FB_LEASE` (`kernel/proc/win_surface.c`) maps the grant's
  three buffers into the client at `WIN_FB_VADDR` and lets it
  `WIN_REQ_FB_PRESENT` for itself -- a KMS lease, never an import of
  the client's scattered write-back buffer (`docs/scanout-design.md`).
  `wm_scanout.c` decides once per frame: topmost, fullscreen, adopted,
  `WIN_HINT_SCANOUT` (`UAPP_SCANOUT`: the app draws WRITE-ONLY -- the
  buffers are write-combining, and a widget that blends reads them),
  no popup, no overlay, hardware cursor active. **Three traps.** The
  compositor must not present under a lease (`wm_render_frame()`
  returns first) or its frame overwrites the client's. Ending a lease
  must `ugfx_screen_forget()` -- the lessee's flips moved the rotation
  and the buffers hold its frames, so the next present is a whole
  screen. **And a lease ends in TWO PHASES**: the display comes back at
  once, the ex-lessee's pages come off only after its next present from
  its own buffer (`wm_scanout_client_presented()` -> `WIN_REQ_FB_LEASE`
  with `b = 1`), because a game is mid-frame into them when the lease
  ends -- the immediate unmap killed DOOM with a page fault at
  `WIN_FB_VADDR`. A dying address space is forgotten before a page of
  it is freed (`win_surface_space_gone()` in `release_process_state()`),
  so the deferred unmap never writes freed tables.

- **`apps/ui/` IS GONE, and the GUI toolkit is `userland/ui/`.** The
  last thing in it was `ui_scrollback.{c,h}`, which survived only
  because the kernel's own `edit` drew with it; `edit` is `/bin/edit`
  now, over the same `utext` model Notepad uses, so **the kernel image
  contains no widget code at all.** A new widget goes in
  `userland/ui/`. There is no longer any such thing as a kernel-side
  one. `apps/theme.h` is gone too: it named
  colours for those widgets and outlived every one of them, and the
  file it was said to still serve used no `THEME_*` name at all. **The
  palette is `userland/ui/utheme.c`, and it is the only one.**
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

- **AN APP'S OWN PREFERENCES ARE THE APP'S, NOT THE SETTINGS REGISTRY'S
  -- `/etc/terminal.conf` IS THE FIRST.**

  Everything with a knob in this system registers a setting and gets a
  row in System Settings for free (`api/setting.h`), and that stays
  right for what the SYSTEM owns: the font, the wallpaper, the shell.
  A terminal's palette is not one of those. Konsole, GNOME Terminal and
  Windows Terminal all keep theirs in the application, because somebody
  changing a scheme is changing that window and not the desktop --
  and `setting_register()` is the KERNEL's, so following the registry
  here would put a ring-3 app's appearance in ring 0.

  The file is `etc_config`'s format through `lib/uconf.h`, so `edit
  /etc/terminal.conf` and `config` read it like any other and there is
  no second parser. `userland/term/` holds it: the emulator is one
  translation unit, the configuration and the dialog are two more, wired
  in through `EXTRA_OBJS_terminal` exactly as `userland/fm/` is -- a
  second `.c` under `userland/gui/apps/` would become a second BINARY.

  Four things to know.

  **A COLOUR SCHEME IS A DATA FILE, NOT SIXTEEN CONTROLS.**
  `/usr/share/terminal/<name>.scheme`, picked by name -- Konsole's
  `.colorscheme`, GNOME Terminal's built-in schemes and Windows
  Terminal's named JSON schemes are all this shape, and none of them
  offers a grid of sixteen colour pickers as the primary UI. There is no
  colour widget here anyway.

  **THE FILE IS IN ANSI ORDER AND A CELL HOLDS A VGA INDEX.** `Color0`
  is black, `Color1` is RED; `enum vga_color` has blue at 1 and red at
  4. `term_scheme_load()` permutes through the kernel parser's own
  `ansi_color()` so a published palette can be copied in verbatim --
  **and `Foreground=`/`Background=` name a `Color<N>`, so they permute
  too** (reading one as a VGA slot put Solarized Dark's foreground on
  light red, which rendered perfectly and was the wrong colour).
  `tools/term_scheme_hostcheck.py` is the host oracle for that, with a
  positive control.

  **CHANGING THE SCHEME REMAPS THE CELLS THAT ARE ALREADY THERE.** A
  cell stores an index and has no "this is the default" bit, so moving
  the default pair from 7-on-0 to 12-on-8 would otherwise leave the
  whole scrollback drawn in two colours the new scheme reserves for
  something else. The two halves move independently, because a coloured
  `ls` leaves an explicit foreground over a default background.

  **CLOSING A WINDOW WITH MORE THAN ONE TAB ASKS, WHICH MAKES Alt+F4
  TWO-PHASE.** `wm_request_close()` takes 0 for "not yet" and a modal
  answers frames later, so the refusal goes out first and the real
  close is a `uapp_quit()` from the dialog's commit. **A TOOL THAT
  CLOSES A TERMINAL CAN NO LONGER ASSUME Alt+F4 CLOSES IT** -- on a
  multi-tab window the key puts a dialog up and nothing shuts;
  `uterm_test.py` closes by PID for exactly that reason, having first
  written a loop that closed nothing and reported it as a spawn
  failure.

  **THE GRID'S FONT IS THE TERMINAL'S AND THE CHROME'S IS THE
  DESKTOP'S.** `font_size = 0` -- the default -- follows the desktop and
  rasterizes nothing; any other value loads a private atlas
  (`ugfx_font_load`, `ui/ugfx.h`'s tier 2). ugfx's font selection is per
  PROCESS, so the grid measures with `cell_w()`/`cell_h()` and `draw()`
  is the one place the selection moves -- the menu bar and tab strip are
  the toolkit's and are drawn after `on_draw` in the session font.

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
  `uapp_desc` with `UAPP_SINGLE_INSTANCE` sends `WIN_REQ_ACTIVATE`
  before creating anything: the compositor raises the twin and the
  second copy exits 0 without ever appearing. Four things to know.
  **The request CARRIES NOTHING** -- the compositor asks the kernel what
  program the ASKING pid is and compares that, so the answer cannot
  depend on a string an app declares about itself (two apps declaring
  one `app_id` used to raise each other's windows, and a "yes" means
  "exit now", so the second app simply never appeared). **It is the one
  channel message with a REPLY**, `uchan_call()`, and it is the only
  round trip in the protocol; no channel and no answer both mean "no
  twin", because a false yes hides an app and a false no shows a window
  the user can close. `app_id` is still declared and still rides
  `WIN_REQ_CREATE`, but nothing MATCHES on it any more -- it labels a
  taskbar group and picks an icon. And **it is not a lock**: two
  launches in the same instant can both be told "nobody there", which is
  recorded rather than fixed because every launch path here is a human
  clicking a menu. See `docs/decisions.md`.
- **WHAT PROGRAM A CLIENT IS COMES FROM ITS SPAWN PATH, AND THE
  COMPOSITOR ASKS THE KERNEL FOR IT** -- `QUERY_PROCPATH`, one record
  per live process, interned to an int by `wm_client.c`'s
  `identity_for_pid()`. It is the grouping key for the taskbar and the
  match for single instance, and both fail SILENTLY when it is wrong: a
  wrong merge puts two programs on one button, a wrong match makes an
  app exit without drawing. A path the kernel derives cannot be
  misdeclared, which is the property neither `app_id` nor a `.desktop`
  entry has. -1 (no path) matches nothing, which is the safe direction.
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
- **A SCROLLBAR CAN LIE DOWN (`UUI_SCROLLBAR_HORIZ`), AND ITS OFFSET
  THEN RUNS THE OTHER WAY.** One implementation serves both axes -- the
  functions read `x`/`w` as the scrolled axis when the flag is set, and
  `total_lines`/`visible_rows` as columns -- so a thumb cannot be drawn
  in one place and hit-tested in another. **The direction is the trap.**
  A vertical bar here is a SCROLLBACK: offset 0 is pinned to the NEWEST
  text at the bottom, because that is what a terminal and an editor's
  view want. Horizontally there is no "newest", so 0 is the LEFT MARGIN
  as it is in every toolkit -- which means passing a vertical offset to
  a horizontal bar puts the thumb at the wrong END, not merely
  sideways. The zone names stay vertical (`UUI_SB_UP` is the LEFT
  arrow); two enums for one set of answers would have been worse.

- **A SCROLLBAR'S SHAPE IS A RADIUS THE APP CHOOSES, AND THE DEFAULT IS
  A CAPSULE.** `struct uui_scrollbar_style` carries a `track_radius` and
  a `thumb_radius` in pixels; `UUI_SB_CAPSULE` means half the short axis,
  and any radius is clamped to that. `uui_scrollbar_draw()` is
  `uui_scrollbar_draw_styled()` with `uui_scrollbar_style_default` --
  capsule on both parts, which is Breeze's groove and handle
  (`drawRoundedRect(rect, 0.5 * w, 0.5 * w)`) and what Konsole therefore
  shows. A radius per part rather than a round/square flag because that
  is what the systems this copies actually express: `border-radius` on
  `::-webkit-scrollbar-thumb`, GTK's CSS, the radius a Qt style passes
  its painter. **The arc is BLENDED against what is already on the
  surface**, so a caller must paint under the bar in the same pass --
  every one does (`uapp.c` clears the window, and each container fills
  its own rect first), and blending against a stale back buffer would
  darken the corner a little every frame. Only the drawing knows about
  the radius: `uui_scrollbar_hit()` and the drag maths still work on the
  rectangle, as they do in every toolkit, so a click on a rounded-away
  corner pixel still belongs to the bar.

- **`utext` HAS A WRAP MODE, AND THE CALLER OWNS ITS STORAGE.**
  `UTEXT_WRAP_WORD` breaks at a space, never mid-word, and falls back to
  a hard break for a word wider than the view because such a word has
  nowhere else to go; `UTEXT_WRAP_OFF` does not break at all and the
  view scrolls SIDEWAYS instead, which is Windows Notepad's View > Word
  wrap and why `utext` carries an `hscroll` at all. Four things to know.
  **The buffer is the CALLER's** (`utext_init_buf`) -- utext allocates
  nothing, so a small editor hands it a static `UTEXT_CAP` array and
  Notepad sizes one to the file it is opening. **Wrapping is DERIVED,
  never stored**, and what is cached is a sparse checkpoint index
  rebuilt whenever the text, the width or the mode changes -- which is
  what makes a 1.6 MB document cost a screenful of work per frame rather
  than a documentful. **`line_span()` is the one place a break is
  decided**, and draw, measure and hit-testing all walk it, because two
  copies of that arithmetic is how a click lands one character off.
  And **the horizontal bar is hidden while wrapping**, since a wrapped
  document has nothing to the right of the view and the bar would be a
  permanently full thumb taking a row off the page.

- **A WIDGET WITH A SCROLLBAR ANSWERS `hit` WITH ITS WHOLE RECT, AND
  `_hit()` KEEPS THE ROW QUESTION.** `uui_route.c` gates press AND wheel
  on `ops->hit`, so a widget that routes on its row hit -- which
  deliberately excludes the bar column, and in `uui_sidebar` every
  heading too -- refuses input exactly where the scrollbar is. It does
  not look like a routing bug: the rows scroll by wheel and select by
  click, and only the bar is dead, so it reads as the scrollbar itself
  being unimplemented. Answer `uui_hit(x, y, w, h, ...)` in the ops slot
  and keep `_hit()` for "which row"; `>= 0` there is still right for a
  widget with no bar. Five widgets shipped the conflation --
  `uui_listbox`, `uui_table` and `uui_fileview` each fixed it, and
  `uui_sidebar` and `uui_tree` carried it until 2026-09-03, which is why
  `tools/check_widget_ops.py` now fails the build on it (rule 5).
- **A `uui_scrollview` NOTICES when its content's item list changes**
  (`sv_children()` compares the `items` pointer and `count` against what
  it last laid out). It used to re-lay-out only on its own rect or
  offset moving, so an app that swapped a page's items left every NEW
  widget at a ZERO RECT, invisible and unclickable, while widgets
  carried over kept the PREVIOUS page's geometry -- which reads as a
  broken layout, one layer away from the cause.
  `uui_scrollview_content_changed()` is still the honest thing to call
  at the point of change and is no longer load-bearing.
- **A STRING SETTING GETS A TEXT FIELD IN SYSTEM SETTINGS, AND ITS
  `staged` IS A CHANGED FLAG RATHER THAN AN INDEX.**
  `SETTING_TYPE_STRING` used to draw an EMPTY `uui_radio_list` -- a row
  that looks broken and could only be changed with `config set`, which
  the app's own comment admitted. `CTRL_TEXT` wires `uui_textbox` in as
  a fifth control kind, so every registry client with free text becomes
  editable, not just the setting that prompted it.

  **The trap is `struct slot`'s `staged`/`baseline` pair, which are
  INTS.** A field has no choice list to index into, so it keeps
  `baseline` at 0 and sets `staged` to 1 when the text differs from the
  value the page opened with -- which leaves every `staged != baseline`
  test on the page reading exactly as it did. Only `staged_value()`
  knows the difference, which is what that pair was designed for.

  Two things that had to move with it. **`UUI_TEXTBOX_MAX` is 64 now**,
  matching `SETTING_ABI_VALUE_MAX`, with a `_Static_assert` tying them
  together -- a shorter field would silently truncate what it was handed,
  which is a wrong answer rather than a full field. And **`uui_textbox`
  grew a `disabled` flag**, because it was the only control here without
  one: a setting the registry has made unavailable must READ as
  unavailable, and every interactive slot checks it (pointer AND focus
  ring, since refusing in one leaves the other way in).

  **AND THE SIDEBAR'S PAGE CAP IS REAL.** `MAX_GROUPS` was 16 and one
  new page made 17, which dropped the Kernel category's last page with
  no word said -- caught only because `settings_test.py` asserts that
  page by name. It is 24 now and an overflow is LOGGED, since a missing
  page looks exactly like a setting nobody registered.

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

- **A DRAG NEEDS THE BUTTON STILL DOWN, AND THE POINTER GRAB IS NOT
  THAT FACT.** A widget holds the grab from its `press` to its
  `release`, so a `dragging` flag only says the press was yours -- a
  motion can arrive inside that window with nothing held, and a handler
  that treats it as a drag moves the value to wherever the pointer is.
  Test the `buttons` mask (`0x1` is primary, `abi/win_proto.h`), which
  is what `uui_button` has always done. It cost a real check: a click on
  the right of a volume scale set it to 100 on the press and a
  button-up motion dragged it back to 0 before the release arrived, so
  every click after the first reported 0 and the GEOMETRY looked wrong.
  `uui_slider` had the identical shape and nothing drove it that way.

- **`uui_scale` IS FOR A CONTINUOUS NUMBER; `uui_slider` IS FOR AN
  ORDERED ENUM.** GTK's split -- `GtkScale` is a value on a range, and a
  widget for named levels is a different control. `uui_slider`'s value is
  an INDEX and it draws a tick per stop, which is exactly wrong for a
  three-minute song; `uui_meter` is a READING and has no `hit`. So a
  position bar, a volume control or a percentage takes
  `userland/ui/uui_scale.h`. **It carries NO LABEL** -- the readout is
  text the app already formats, and reserving a row for it would make the
  widget's height depend on which strings are set, which is the trap
  `uui_meter` documents. **A drag reports EVERY motion and the app
  decides what that means**: volume acts on `UUI_REASON_MOTION` and
  follows the thumb live, a seek acts on `UUI_REASON_RELEASE` because
  re-seeking a decoder per pixel is work nobody asked for. Clicking the
  track JUMPS there, as on every scale outside a Win32 trackbar. Sizes
  are font-derived, so the thumb is a text row tall rather than ten
  pixels forever.
- **ONE MENU WIDGET SERVES A BAR AND A CONTEXT MENU:
  `uui_menubar_open_at()`.** A free-floating popup at (x, y), placed by
  the same flip/slide/clamp arithmetic a dropdown uses and driven by the
  same hit-testing, hover, submenus, `item_flags` and keyboard. Qt's
  QMenu and GTK's GtkPopoverMenu are one class serving both, and the
  reason is what a user notices when they are two: different padding, a
  different tick gutter, arrows that work in one and not the other.

  Two rules. **USE A SEPARATE INSTANCE, initialised with `count == 0`**
  -- a context menu has no bar strip, and sharing one instance with a
  real menu bar would let Left/Right walk out of the popup into the
  bar's titles; with no titles there is nothing to walk to. And **OPEN
  IT ON THE SECONDARY RELEASE, NOT THE PRESS**, which is Win32's
  WM_RBUTTONUP and here is not a preference: `uui_router_release()` runs
  for EVERY button (only the PRESS is filtered to the primary one), so a
  menu opened during the press is handed that same gesture's release and
  commits whichever row landed under the cursor. The menu bar's
  open-on-press exception in `docs/gui-guidelines.md` is about the
  primary button and does not carry over.

- **A POPUP IS THE TOOLKIT'S CARD: PAINTED SQUARE INTO A SURFACE, CUT
  ROUND BY THE COMPOSITOR.** `ui/uui_popup.h` owns the card's radius
  (`uui_popup_radius()`, font-derived), ground and hairline, so a menu,
  a dropdown list and a tooltip are one shape. A widget drawing into a
  popup SURFACE paints a square card; `wm_render.c` saves what is under
  the corners, composites the surface, and cuts the arc as it does a
  window's -- reading the ring's colour off the middle of the surface's
  TOP EDGE, so the arc continues whatever hairline that widget drew. A
  widget drawing IN-WINDOW (no provider: the WM's own menus) rounds with
  `uui_fill_round_rect()` itself. **Do not round inside a surface**: its
  corner pixels would blend against the surface's stale buffer, not the
  scene, and the compositor's cut would carry that blend. A popup's
  shadow follows the radius but stays below its top edge (`wm_shadow.c`).
- **A POPUP IS A SURFACE OF ITS CLIENT, PLACED BY THE COMPOSITOR, AND A
  PRESS OUTSIDE THE CLIENT'S SURFACES DISMISSES IT.** `WIN_REQ_POPUP`
  (`abi/win_proto.h`) opens a second window of the same client, anchored
  to a rect of its parent in the PARENT's content coordinates, with the
  side it prefers; the compositor flips/slides/clamps it against the work
  area and replies with where it landed, in the same coordinates. No
  chrome, no taskbar button, no saved geometry; the parent keeps the
  active title bar and Alt+F4, the popup gets the keys. **THE GRAB IS
  OPT-IN (`WIN_POPUP_GRAB`)**: for a GRABBING popup a press inside one of
  the client's popups or inside its own window's content is delivered and
  a press anywhere else closes every popup of that client with
  `WIN_EV_POPUP_DONE` and is consumed -- while a popup WITHOUT the flag
  routes input as though it were not there, which is what a TOOLTIP is.
  `docs/decisions.md` has the comparison with `xdg_popup` and Win32 and
  the six calls. Six things to know when touching it:
  - **A GRABBING POPUP'S KEYS GO TO THE POPUP'S SLOT, NOT ITS PARENT'S**,
    so anything routing a popup's events home must route EVERY type, not
    just the pointer. A dropdown in the file chooser -- a dialog window
    with its own router -- seeked with keys delivered to the MAIN
    window's widgets until `uapp` recorded which window each popup hangs
    off.
  - **COORDINATES STAY THE PARENT'S; ONLY THE DRAWING MOVES.** A widget
    keeps its popup rect in the window's content coordinates (the reply)
    and draws with the level's origin SUBTRACTED into the surface
    `uui_popup_surface()` hands back; uapp adds the offset back to any
    pointer event that arrives on the popup slot. Hit-testing,
    `describe` and every test rect are therefore untouched. Do not
    convert rects to popup-local anywhere else -- two coordinate spaces
    in one widget is how a click lands one row off.
  - **A LEAVE CARRIES NO POSITION.** The compositor's "the pointer left
    you" is a move to (-1,-1) in the surface's own coordinates; for a
    popup, translated, that is a REAL point one pixel above-left of it
    (Notepad's menu bar, so every submenu hover switched menus). uapp
    drops a popup's out-of-bounds move, as `wl_pointer.leave` has no
    coordinates. Keep it that way.
  - **EVERY PATH THAT SHORTENS A MENU CHAIN GOES THROUGH `set_depth()`**
    (`uui_menubar.c`). A level may own a surface, and writing `depth--`
    leaves the compositor showing a menu nothing hit-tests any more.
  - **AN EVENT FOR A SLOT NOT IN USE IS STALE AND IS DROPPED** -- the
    client closed the popup while the press was in flight. Read as the
    toplevel's it lands, with raw coordinates, on whatever is at that
    point in the window.
  - **A POPUP IS A ROW IN `windows[]`, NOT A `wm_overlay`**, so it
    inherits the blit, the damage, the ping and the dead-client sweep,
    and `gui windows --json` lists it (`popup: true`, `parent: <slot>`).
    The four `window_content_*()` accessors are the one place "no
    chrome" lives; `wm_focus_index()` is "the focused toplevel" and is
    what every "frontmost" test in the WM now asks. `uui_popup.h` is the
    seam a second widget (the dropdown, the tooltip) plugs into; a
    refusal there means "draw it in the window", and every widget must
    keep that path.
- **A DIALOG IS A SECOND TOPLEVEL OF ITS CLIENT, OWNED BY ONE OF ITS
  WINDOWS, AND `uapp_window_open()` IS HOW AN APP GETS ONE.**
  `WIN_REQ_DIALOG` (`abi/win_proto.h`) opens a window WITH chrome --
  title, close button, dragged and stacked like any other -- centred on
  its owner, always above it, and with no taskbar button of its own.
  Win32's owned dialog, `xdg_toplevel.set_parent`, X11's
  `WM_TRANSIENT_FOR`. With `WIN_DIALOG_MODAL` the owner takes no input:
  a press on it raises the dialog instead, which is what KDE and Windows
  both do. Six things to know.
  - **IT IS NOT A POPUP AND NOT `ui/uui_dialog.h`.** A popup has no
    chrome, cannot be moved and is dismissed by the first press outside
    it -- right for a menu, wrong for anything typed into. `uui_dialog`
    is a modal drawn INSIDE the app's own window, which is right for a
    question that fits there ("Save changes?") and wrong for one that
    wants more room than its owner has. Qt ships both for the same
    reason (`QMessageBox` against `QFileDialog`).
  - **IT HAS ITS OWN WIDGET ARRAY, ROUTER AND FOCUS RING** -- a second
    content area, not a second view of the first. `uapp_window_desc`
    carries them; everything else (reaping, the ping, the font change,
    the clipboard) is the PROCESS's and is already running.
  - **`bring_to_front()` STILL LEAVES THE PROMOTED WINDOW LAST.** Four
    call sites in `wm_input.c` index `window_count - 1` right after it
    to start a drag or a resize, so raising a window's dialogs INSIDE it
    silently broke those: the owner's title bar armed a drag and the
    thing dragged was the dialog. `raise_with_dialogs()` is the version
    that carries them, and the taskbar uses it -- the button is the only
    handle a blocked window has left out there.
  - **THE LAYOUT LOG IS PER SURFACE.** Set `uapp_window_desc.log_prefix`
    and the toolkit emits the report itself (a dialog has no `on_draw`
    for an app to call `uapp_log_layout()` from). The per-frame dedupe
    keeps one previous block PER SURFACE, because a toplevel and a
    dialog reporting alternately never match and the dedupe would be off
    exactly when two windows are open.
  - **MODAL BLOCKS THREE DOORS, NOT ONE**: the primary click, the
    SECONDARY click, and MOTION. Blocking only the first leaves the
    owner's context menu live and its menu bar lighting up under the
    cursor -- controls advertising themselves on a window that refuses
    every one of them. Win32 gets all three free by disabling the owner
    HWND; here each is a `wm_dialog_blocker()` call, and the first
    version had only one.
  - **`gui windows --json` SAYS SO**: `dialog`, `owner`, `modal` beside
    `popup`/`parent`. A dialog is a toplevel with chrome, so `popup`
    cannot tell a test one from an ordinary window.
  - **ONE LEVEL.** A dialog never owns a dialog -- the compositor
    refuses it, because the owner chain is what `modal` is resolved
    against.
- **THE FILE CHOOSER IS ONE WIDGET IN ONE WINDOW: `ui/uui_filedialog.h`,
  AND THREE APPS OPEN IT.** Open, Save and Choose-a-folder, over
  `uui_fileview` with a Places strip (Windows' Quick access, KDE's
  Places) and a Details/Icons toggle. `uui_filedialog_open()` takes the
  mode, a starting directory, a name and a FILTER -- the app's own, so
  the chooser lists exactly what that app can open, by probe and never
  by extension. The callback runs once, with the path or with NULL, and
  the window is closed BEFORE it runs so it may open another.

  It has a **"Files of type" combo** (`uui_filedialog_filter` rows,
  Win32's `lpstrFilter`, KDE's Filter): each app declares its own rows
  and ends with `UUI_FILEDIALOG_ALL_FILES`, written once here rather
  than three times. **A DIRECTORY ALWAYS PASSES, WHATEVER THE ROW SAYS**
  -- every app's filter answers "is this a file I can open" and so says
  no to a folder, which is right for a sidebar pinned to one directory
  and leaves a CHOOSER with an empty root and no way out; the gate is in
  `uui_filedialog.c`, not asked of each caller. And **a form label needs
  `UUI_FILL_H`**: its natural height is one text row, so without it the
  box sits at the TOP of a taller row and the caption reads as belonging
  to whatever is above. `uui_label` already centres its text in a taller
  box -- the flag is what gives it one.

  Three more things. **THE OUT-OF-PROCESS CHOOSER IS DELIBERATELY NOT
  COPIED** (`xdg-desktop-portal`, macOS's panel service): it exists so a
  sandboxed app that cannot read the filesystem can be handed one file,
  and there is no such boundary here. **A NESTED LAYOUT NOW TAKES NO
  MARGIN AT ALL** -- it used to read anything <= 0 as the font-derived
  default, so three rows asking for no padding took a third of the
  window's height (the `margin = 1` idiom was the workaround, and is no
  longer needed). A layout laid out as a CHILD contributes nothing
  unless it names a margin, which is Qt's rule for a sub-layout. And **the FILTER is what let
  Image Viewer and Audio Player keep their pinned sidebars**: each is
  still the filmstrip for one directory, and Open is how you reach a
  different one -- which neither could do at all before.
- **THERE IS A SYSTEM CLIPBOARD, IT IS A RING-3 SERVICE
  (`/bin/clipboardd`, `lib/uclip.h`), IT HOLDS FILES OR TEXT, AND A
  PASTE COSTS NO SYSCALL.** Apps use `userland/lib/uclip.h` rather than
  touching the page themselves, because two callers that pack the
  payload differently do not interoperate -- which is the one thing a
  clipboard exists to do -- and because the page is under a seqlock.

  Seven things to know. **IT IS NOT IN THE KERNEL, and it used to be**
  (`SYS_WIN_CLIP`, now a dead number): the daemon creates a named
  shared-memory page and owns its LIFETIME, and clients read and write
  it directly, so a paste is a memcpy rather than a syscall
  (`docs/decisions.md` has why the kernel was the wrong home and why
  the compositor was too). **A COPY IS A COPY, NOT A PROMISE** -- the
  opposite of an X11 selection and of Wayland's `wl_data_source`, and
  it is why closing the app you copied from does not lose the clipboard
  here. **THE KIND IS DECLARED, NEVER SNIFFED**: `uclip_kind()` answers
  FILES or TEXT and `uclip_text()` returns NULL for anything that is not
  text, which is what stops a path being pasted into a document as a
  line. **A CUT MOVES NOTHING UNTIL THE PASTE** (`UCLIP_CUT`), as in
  Explorer and Dolphin, and **it is SPENT by that paste**; a cut of TEXT
  is refused outright, so an editor's Cut copies and deletes its own
  selection. **A COPY THAT DOES NOT FIT IS REFUSED**, never truncated:
  half a cut set pasted is files silently left behind, and half a
  paragraph is worse than none -- say so to the person, since silence is
  the one outcome a Copy must never have. **THERE IS NO BROADCAST any
  more** -- `WIN_EV_CLIPBOARD` is gone, because a client can read the
  serial out of shared memory for free; `uapp` polls it and
  `uapp_desc.on_clipboard` still fires. And **the keys are the APP's,
  not the WM's**: `Ctrl+C` is INTR in a terminal, so a compositor that
  routed it globally would take that away -- which is why Notepad binds
  `Ctrl+C/X/V` and the Terminal binds `Ctrl+Shift+V`.

  **HOLD A `struct uclip` STATICALLY.** It is a snapshot with the
  payload in it, far past the ring-3 frame budget; a local is a build
  warning rather than a crash, which is the only reason the struct is
  allowed to stay that shape.

- **`uui_splitter` IS THE DRAGGABLE DIVIDER, AND IT OWNS A FRACTION
  RATHER THAN A PIXEL COLUMN.** Qt's `QSplitter`, GTK's `GtkPaned`,
  Explorer's navigation-pane divider. The value is per mille of the
  travel (`UUI_SPLIT_SCALE`), which is what makes a window that got
  wider keep the proportion the user chose instead of stranding one side
  at the width it had when the window was small -- and what makes the
  saved value survive a font change and a different screen. The APP
  supplies the track (`uui_splitter_set_track`) on every layout pass,
  because only the app knows what the two children are and what is left
  after the chrome; the widget answers `pos`/`before`/`after` from one
  piece of arithmetic, so a caller using two of them cannot disagree
  with itself.

  Four things to know. **A DRAG IS A DELTA FROM THE PRESS, never the
  pointer's absolute position** -- the handle then follows the pointer
  exactly whatever margins and gaps sit between the track's origin and
  the band, which is what lets one widget serve File Manager's
  hand-computed rects and System Settings' `uui_layout` row (where the
  layout inserts a gap on each side of the band, and an absolute mapping
  would offset every drag by one gap). **THE MINIMA ARE THE WIDGET'S,
  not the app's to re-check**: a pane dragged to nothing leaves no
  handle to drag back. **A DOUBLE CLICK RESETS IT** to `def_frac`, KDE's
  escape hatch from a bad drag. And **the band is the HIT ZONE and is
  wider than the line drawn in it** -- a one-pixel target is not a
  target, which is why every real splitter's handle is a few pixels of
  otherwise empty strip.

- **A LAYOUT CHILD'S SIZE CAN BE PINNED FROM OUTSIDE: `uui_item.main_size`.**
  Non-zero overrides what that child's `natural_size` asked for, along
  the container's stacking axis only -- CSS's `flex-basis`, QSplitter's
  `setSizes()`. It exists because a divider's whole job is to own a
  size the widget beside it would otherwise choose, and the alternative
  -- a `set_width()` on `uui_sidebar`, then on `uui_tree`, then on
  `uui_listbox` -- is that one answer written once per widget type.
  **It is the LAST field in `struct uui_item` on purpose**: six apps
  initialise that struct positionally, so a field inserted in the middle
  silently renumbers every one of them (and the compiler only says
  `-Wmissing-field-initializers`, which is not the error it deserves).
  Those arrays are designated-initialiser now, which is the real fix.

- **A CLIENT MAY ASK FOR A RESIZE CURSOR NOW:
  `WIN_CURSOR_RESIZE_H`/`_RESIZE_V`.** They were the frame's alone --
  `abi/win_proto.h` said so -- until a client had a divider of its own to
  drag, which is exactly why Wayland's `cursor-shape-v1` exposes
  `col-resize`/`ew-resize` to clients. The shapes were already on disk in
  every cursor theme (`data/cursors/*/resize-h`), so this cost two
  defines and a `switch` arm in `wm_render.c`'s `client_cursor_at()`.
  **The compositor still wins wherever the two overlap**: a window edge
  is geometry it owns, and `resolve_cursor_kind()` asks the frame first.
  A test reads the resolved shape from `gui state --json`'s
  `cursor.shape` (`DebugConsole.cursor_shape()`) rather than trying to
  recognise a 15x21 sprite in a screenshot.

- **A CONTROL BELOW THE FOLD IS UNREACHABLE, not merely hard to hit.**
  A scroll view with a `hit` clips its children from ROUTING, so a press
  never reaches a child outside the viewport -- correct, and the reason
  a tool must scroll before clicking rather than aiming at unscrolled
  coordinates, where it gets silence rather than an error. An app driven
  by tools should report a control's rect whenever it MOVES (a page
  change and a scroll alike), not only when a page changes. **And the
  clip is at the VIEWPORT EDGE, so a control STRADDLING it is half
  routable**: System Settings' mouse-speed spinbox stepped up and never
  down for as long as a tool scrolled it until it was merely visible,
  because its lower half -- the down stepper -- hung past the scroll
  view. Scroll until the whole rect is inside, not until the top of it
  is.
- **A SCREENSAVER IS A PROGRAM IN `/bin/wm/savers`, AND THE COMPOSITOR
  OWNS THE CLOCK.** `wm_idle.c` counts ticks since the last input and
  spawns the configured saver as an ordinary FULLSCREEN CLIENT; a saver
  is a `.c` file in `userland/gui/savers/` with no Makefile edit, and
  the setting's choice list IS that directory (`choice_dir`), so
  dropping one in gives it a row in System Settings. Three things bite.
  **A saver must declare `UAPP_RESIZABLE` and a non-zero `w`/`h`**: the
  compositor refuses fullscreen to a window that is not resizable, and
  `uapp_open()` refuses a window with no size at all -- the symptom of
  either is a saver that quietly stays a small box in the corner.
  **It is KILLED, never asked**, because `wm_request_close()` may be
  refused and this is the one window where that must not be possible.
  And **the timeout of zero is the enable**, so there is no second
  switch to disagree with it. See `docs/decisions.md`.

  **A saver that caches the screen size must re-read it**, because it
  opens at its descriptor's size and is resized to the screen a frame
  later -- two of them built their grid once and covered a 640x480
  corner of a 1280x720 screen, which looks like a saver that half
  works. And **a saver is spawned by anything, not only the idle
  clock**: the compositor recognises a client by its SPAWN PATH and
  adopts it, so System Settings' Test button and a shell prompt both
  get dismiss-on-input for free, and neither can strand the screen.
- **A SAVER'S OPTIONS ARE A DATA FILE BESIDE IT, AND SYSTEM SETTINGS
  GENERATES THE CONTROLS FROM IT.** `/usr/wm/savers/<name>.saver`
  declares what a saver lets you change and `/etc/savers/<name>.conf`
  holds what it is set to; `userland/lib/usaver.h` is the one parser,
  compiled into the saver and into System Settings alike. Adding an
  option is a line of text in the descriptor plus the code that reads
  it -- there is nothing to register and nothing to edit in the settings
  app. That is XScreenSaver's arrangement, where each hack ships an XML
  descriptor of its options and `xscreensaver-settings` builds the
  dialog from it; Windows' is the other one, where a `.scr` run with
  `/c` draws its own dialog, and it puts a whole settings window in
  every saver.

  Five things to know.

  **THE DESCRIPTORS ARE NOT IN `/bin/wm/savers`.** The screensaver
  setting's choice list IS that directory (`choice_dir`), so a
  `starfield.saver` sitting beside `starfield` would become a saver you
  could select and which could not start.

  **THE DESCRIPTOR NAMES ITS OPTIONS IN AN `Options=` LINE**, because
  `etc_config` cannot enumerate a document's keys and hand-walking the
  buffer would be a second parser beside the shared one. That line is
  also the ORDER the controls appear in.

  **AN OPTION IS A BOUNDED INT OR A NAMED CHOICE, AND DELIBERATELY NOT A
  BOOL** -- the settings registry has neither a bool type nor a checkbox
  control, so a toggle is `enum:off,on` and gets the two radio buttons
  every other two-way choice here gets.

  **A VALUE THE DESCRIPTOR DOES NOT ALLOW READS AS THE DEFAULT.** These
  are ordinary text files `edit` can change, and the descriptor is the
  only thing between a typo and a saver asked to draw nine million
  stars. The refusal is in `usaver_load()`, so both callers get it.

  **THE OPTIONS ARE NOT REGISTRY SETTINGS, and System Settings
  SYNTHESISES rows for them** past the registry's own (settings.c's
  `g_saver_base`). They cannot be registered: what a saver offers
  depends on which saver is selected, and that changes while the page is
  open. Only the choice list and the WRITE know an option from a
  setting; everything else -- layout, staging, the focus ring -- is one
  path. Two consequences a change here must keep: the rows are REBUILT
  when the dropdown moves rather than the page being re-opened, or a
  staged timeout above them is silently discarded; and the Test button
  WRITES the staged options before it spawns, because a child process
  reads a file and there is no channel for an unwritten value.

  **And the Test button moved UP when this landed, to sit under the
  chooser.** It was the LAST item on the page, which fitted while the
  Screensaver page was two rows and put it below the fold -- unreachable
  rather than merely awkward -- as soon as a saver's options appeared
  above it. Under the dropdown is where Windows puts Preview anyway, and
  being near the top is what keeps it clear of the fold whatever the
  saver declares.

  **The trap that cost two attempts: A ROOT-LAYOUT ITEM CANNOT CHANGE
  VISIBILITY PER PAGE.** `uapp` runs `uapp_desc.layout` at startup, on a
  resize and on a font change, and at no other time -- so an item there
  whose `hidden` follows the open page is placed ONCE, while hidden, at
  ZERO SIZE, and never placed again. A zero-sized button still draws its
  label, so the symptom is a stray caption where a control should be,
  which points at drawing rather than at layout. A widget whose
  visibility changes with the page belongs in the page's own item list,
  which IS rebuilt and re-run on every page change.
- **A CLICK THAT MISSES THE WINDOW LANDS ON WHATEVER IS BEHIND IT, AND
  AT THE BOTTOM OF THE SCREEN THAT IS THE TASKBAR.** The taskbar's
  button for the focused window MINIMIZES it, and a minimized client
  draws nothing -- so it reports nothing, and every later assertion
  reads the state it had before the miss. That is the worst shape a
  harness bug can take: it is silent, it arrives late, and it reads as
  the app ignoring input rather than as a click that went somewhere
  else. A tool driving a window should REFUSE a point outside the
  content area rather than send it, naming the coordinate
  (`settings_test.py`'s `click()`).
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

- **`uui_progress` IS A JOB'S PROGRESS; A NEGATIVE VALUE IS BUSY, NOT
  ZERO.** `userland/ui/uui_progress.h`: a track and a fill in per mille,
  control-height, stretched by its row (`UUI_FILL_W`). Not `uui_meter`'s
  bar, which is one row of a READING. A negative value is QProgressBar's
  busy / Windows' marquee -- a sliding block for work whose total is not
  known yet -- and it is distinct from 0, which says "none done"; call
  `uui_progress_tick()` from `on_tick` to move it. No `hit`, and its
  `describe` reports the value so a test can check the pixels against
  it (System Update and `tools/sysupdate_test.py`).
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
  which was 1024 syscalls against the 1 KiB `SYS_WRITE_MAX` of the time.
  Disk Mark
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
  tunables). The shape: a searchable `uui_sidebar` of category
  headings and their pages on the left, the page as `uui_setting_row`
  cards, and a footer with Reset and Apply. **The rename left a stale
  `/bin/wm/system/cpanel` on any existing `disk.img`**, because `make
  iso` re-seeds by SYNC -- `make clean-disk && make iso` for a fresh
  image, or delete it by hand.
- **SYSTEM SETTINGS PICKS A SETTING'S CONTROL FROM ITS VALUES**
  (`pick_kind()` in `userland/settings/set_page.c`): a `uui_switch` for
  an on/off pair (on/off, yes/no, true/false, enabled/disabled, 1/0), a
  `uui_segmented` for two to four names that fit side by side, a
  `uui_radio_list` inside the card for a few long ones, a dropdown from
  `CHOICES_DROPDOWN_MIN` up. `Widget=dropdown`, `Widget=slider` and
  `Widget=gallery` in `/etc/settings.d` still win; `Widget=radio` now
  means the same as `auto`. A number is a spinbox, a string a field, a key combination a
  capture control, as before.
- **A GALLERY'S PICTURE IS THE CALLER'S PAINTER, AND `Preview=` NAMES IT.**
  `uui_gallery` draws the cards -- the soft accent selection (a FILL, no
  edge), hover, the label, and the focus ring only after Tab or an arrow
  key, never after a click (Windows' focus-visible rule) -- and calls `draw_tile(surface, i, x, y, w, h, ctx)` for each
  tile, clipped to it; it knows nothing of what it shows. In System
  Settings a `Widget=gallery` setting's `Preview=<word>` picks the
  painter from `set_preview.c`'s table (`cursor` today; a wallpaper
  picker adds a row), read by the app itself with
  `uschema_text_word()` -- no ABI field carries it. A painter that
  decodes anything CACHES it per page (`preview_reset()` from
  `open_group()`): the page repaints on every hover. A gallery stages
  like any choice control, so Apply/Reset are unchanged, and the app
  logs `settings: gallery <slot> <name> card <i> x y w h tile <0|1>
  shown <label>` (content-relative) for a test.
- **A TEST THAT STAGES A CHANGE IN SYSTEM SETTINGS MUST APPLY OR RESET IT
  BEFORE IT NAVIGATES.** Leaving a page with a change asks Apply /
  Discard / Cancel (`settings: ask leave page changes N`), so a sidebar
  click after an unfinished stage opens a question instead of the page,
  and every check after it reads the wrong page.
- **A FOCUSED `uui_button` PRESSES ON SPACE OR ENTER, AND A DISABLED ONE
  REFUSES FOCUS.** A NULL `accepts_focus` means "yes", so a button with no
  focus slots sat in rings it could neither show nor act on. Its ring is
  drawn OUTSIDE the face, or an accent (primary) button hides it.
- **UNDER A WM OVERLAY, NO WINDOW IS HOVERED.** `wm_update_content_hover()`
  asks `wm_overlay_under()` first: a point on an open context menu, Start
  menu, tray panel or on-screen keyboard (their `contains` op, else their
  `rect`; the modal confirm dialog covers everything) sends the window
  beneath the "pointer left" move instead of the position -- a row lit up
  UNDER a menu was the window hovering what it could not show. A new
  overlay needs a `rect` or a `contains`, or it hides nothing.
- **AN OUTLINED BUTTON AND A STRONG SELECTION ARE OPT-IN STYLES**:
  `uui_button.outlined = 1` draws a 1px border darker than the face with
  soft corners, and `uui_tree.sel_style = UUI_SEL_STRONG` fills the
  selected row in the accent with its text colour and draws no focus ring
  on it (the fill is the indicator, so a strong tree looks the same
  focused or not) -- both the look of the
  2026-09-28 Device Manager mockup, which uses them. The defaults (flat,
  pale wash) are unchanged, so no existing app moved.
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
  Server, `userland/wm/wm_client.c`, with `kernel/proc/win_role.c`
  holding the role it runs under) implements it; **Toykit** (`userland/ui/`) is the client toolkit an app
  programs against -- roughly Wayland, its compositor, and GTK. Three
  names rather than one because the protocol is meant to outlive this
  server. Symbol prefixes are unchanged and stay that way (`uui_`,
  `ugfx_`, `uapp_`, `WIN_REQ_*`); a toolkit's name and its prefix need
  not match. See `docs/decisions.md`.

  **How a TWP message is CARRIED is not the protocol.** A client's
  window requests ride `uchan` to the compositor and never enter the
  kernel; what is left on `SYS_WIN_REQUEST` is the role, the framebuffer
  and the font. The `win_transport` registry that used to abstract this
  was deleted in stage 6c, having only ever had the one implementation.
  Two things still follow. The `gui` debug commands are protocol messages
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
- **THE TASKBAR'S LAYOUT IS FOUR INDEPENDENT SETTINGS, AND EVERY RECT
  COMES FROM `taskbar_geom()`.** `desktop.taskbar_buttons` = `labelled` |
  `icons`, `desktop.taskbar_align` and `desktop.start_position` (shown
  on the Desktop > Start page) = `left` | `center`, `desktop.taskbar_float` = `off` | `on` -- Plasma's shape,
  where each is its own control -- and `desktop.taskbar_theme` = `dark` |
  `light` picks `taskbar_palette()`. ONE `taskbar_layout()` places every
  combination: Start first, taking its room out of the strip, then the
  buttons sized for what is left and placed by alignment; a centred
  Start over left-aligned buttons sits alone in the middle.
  `draw_taskbar()` branches once per indicator. **Hit-test against the BAND,
  draw inside the PANEL**: the screen's bottom edge hits the button
  above it (Fitts), the Start button owns the corner when it is
  leftmost (`taskbar_start_hit_rect()`), and the gap under a floating
  panel is the strip's, not the desktop's. **A floating panel DEFLATES
  while any window is maximized** -- `taskbar_h` never moves with the
  panel's shape. **The strip lists and groups by `struct
  window.task_rank`**, never `windows[]` order, which is z-order and
  changes on every click; `open_seq` is a window's IDENTITY (peek, a
  button's `key`), `task_rank` its PLACE. **A press on a button only
  ARMS it**: the click acts on release, and 4px of movement first makes
  it a drag, which `taskbar_layout()` itself turns into the live row
  (`apply_drag()`) and the release commits into ranks. Buttons are
  DRAWN at `taskbar_draw_x()` (a glide) and HIT at their layout x. `gui taskbar --json` reports all of it;
  `tools/taskbar_style_test.py` checks each where it is drawn.
- **TASKBAR PEEK IS AN OVERLAY THAT HOLDS WINDOWS BY `open_seq`.**
  `userland/wm/wm_peek.c`, `desktop.taskbar_peek` = `off` | `preview` |
  `highlight`: a card of box-averaged CONTENT thumbnails (no chrome),
  rescaled on the client's present and at most every 200 ms, never per
  frame. It opens after the tooltip's delay, switches between buttons at
  once, survives a short grace crossing the gap to the card, and stays
  shut after a click until the pointer leaves the button. Highlight is
  drawn by `render_scene()`: every other window, a dim, then the lifted
  one again through `draw_one_window()`. **A client buffer is
  `0x00RRGGBB`** -- scale one through `uimg_scale()` with `has_alpha`
  0, which reads alpha as opaque; the first build scaled every
  thumbnail to black. `gui peek --json`; `tools/taskbar_peek_test.py`.
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
  `/usr/share/cursors/<theme>/<shape>` (`CURSOR_SHAPE_COUNT` names in
  `cursor_theme.c`'s `g_names[]`), generated by `tools/gen_cursors.py`
  and loaded by `userland/wm/cursor_theme.c`. The FORMAT -- `struct
  cursor_shape`, the parser, decoding an image, the pixel query -- is
  `userland/lib/ucursor.c`, shared with System Settings' gallery, which
  draws a theme without being the compositor; the WM keeps the live
  theme, the settings and its own file reads. Two registered settings
  pick the theme and the size. Five things to know. **A shape is MASKS
  or an IMAGE** (Xcursor's split): masks carry COVERAGE, coloured by the
  compositor, so one set serves a light theme and a dark one; an image
  shape names straight-alpha QOI files (`image=`, `image2=`, `image3=`,
  one per size) whose colours are the theme's own, and the loader
  decodes ONLY the one the size setting wants -- so a size change
  reloads the theme. **`hand`, `move` and `not-allowed` fall back to
  the theme's ARROW**, not to a built-in: they have none, and an arrow
  is what showed there before they existed. **The built-in shapes are the floor**: a
  missing or malformed file costs its own shape, not the pointer --
  which also means **a theme that loads NOTHING still draws a perfect
  pointer**, so never test this by checking that a cursor is on screen
  (the first version shipped loading 0 of 6 and looked right).
  **Scaling is integer nearest-neighbour** where there is no rendering
  for the size, and the size is its own setting, not derived from
  `font_size`. And **every SHIPPED theme is an image theme** (Classic
  and Classic bold since 2026-10-01; the default is Graphite,
  `CURSOR_THEME_DEFAULT`), painted over ONE geometry in
  `tools/gen_cursors.py` -- the masks are the format's and the
  built-ins', no longer any set's. See `docs/decisions.md`.
- **A CLIENT NAMES ITS POINTER SHAPE, AND THE COMPOSITOR CLAMPS IT TO
  THE CONTENT AREA.** `WIN_REQ_CURSOR` (`abi/win_proto.h`) carries a
  `WIN_CURSOR_*` -- `DEFAULT`, `TEXT`, `WAIT`, the two resize shapes,
  `HAND` (a link: `uui_markdown`'s `cursor` op), `MOVE` or
  `NOT_ALLOWED` -- and the kernel forwards
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
- **MOUSE MOTION IS A STATE, NOT A BACKLOG, AND A FULL EVENT QUEUE SHEDS
  INPUT BEFORE A NOTIFICATION.** `win_input_push()` -- the compositor's
  kernel queue, the only one there is since stage 8 -- merges a move
  into the NEWEST queued move when the buttons match, so motion never
  holds more than one of the 32 slots (a client's motion is deduped by
  the compositor before it is written, `client_last_mx`) --
  Windows keeps one `WM_MOUSEMOVE` per queue and X compresses
  `MotionNotify` for the same reason. When the queue still overflows,
  the oldest INPUT event goes (raw or delivered: keys, buttons, wheel),
  and only with no input queued the oldest of all. The trap that made
  it: `WIN_EV_SCREEN` sat in the compositor's queue behind a real mouse
  moving through one 300 ms frame, was the oldest event when the 33rd
  move arrived, and was shed -- the kernel then scanned 1280x1024 while
  the desktop kept painting 1366x768, which read as a broken panel
  fitter (a cut right edge, a band of stale boot text flipping between
  three scanouts). `guictl compositor` shows the drop count; a
  notification lost to it is otherwise invisible. Only the newest slot
  merges, so a press between two moves keeps its own position.
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
- **THE POINTER RIDES THE HARDWARE CURSOR PLANE WHEN THE DRIVER HAS
  ONE, AND THE HANDOVER IS PER SHAPE** -- `userland/wm/wm_hwcursor.c`
  over `WIN_REQ_FB_CURSOR` (compositor-gated, beside the framebuffer
  grant): the WM defines the theme's shape as a 64x64 ARGB sprite on
  shape changes -- from `cursor_shape_argb()`, the one answer the
  software sprite draws too, so a mask theme and an image theme reach
  the plane alike -- the KERNEL moves the plane from `win_input.c` on every
  pointer event (zero syscalls per motion), and QEMU renders it as the
  real host pointer -- which is what makes leaving the window seamless
  under `-vga virtio`. A shape the plane cannot hold
  (`cursor_size=huge` past 64px, a built-in resize/text/wait shape,
  no plane on this driver) falls back to the software sprite, and the
  sprite's save-under/damage bookkeeping stands down by the same
  per-shape answer (`wm_hwcursor_sync()`). **A hardware cursor is
  INVISIBLE to `screendump`** -- a test asserting its pixels appear is
  asserting it failed (`tools/virtio_gpu_test.py`'s oracle: a pure
  pointer move repaints NOTHING). The plane belongs to the ROLE and is
  hidden wherever the compositor role dies. See `docs/decisions.md`.
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
- **A ring-3 compositor delivers events by WRITING THEM, not by calling
  the kernel.** `wm_client.c` writes a `struct win_event` into the
  client's channel ring (`uchan_server_send()`, the inbox in
  `lib/uchan_page.h`) and wakes it through the ring's own futex word;
  no syscall names another process's queue any more. `WIN_REQ_EVENT_
  PUSH` is retired; `WIN_REQ_EVENT_STATS` reports the compositor's OWN
  kernel queue. Access control did not weaken, it moved: a client's
  inbox is a page only the compositor was granted, so "a client cannot
  synthesise input into another" holds by construction.
- **A CLIENT'S EVENTS ARRIVE ON ITS OWN RING, THE KERNEL QUEUE IS THE
  COMPOSITOR'S ALONE, AND A STATE THE INBOX CANNOT TAKE IS RE-SENT.**
  Stage 8 of `docs/winserver-ring3-design.md`. Linux keeps evdev in the
  kernel and gives every Wayland client a socket the compositor writes;
  Windows keeps a per-thread message queue IN the kernel, which is what
  toy-os had. Now: `win_input.c` is the one queue and it serves the one
  process holding the compositor role -- `SYS_WAIT_EVENT`/`SYS_POLL_
  EVENT`/`SYS_WAIT_READY` answer -EPERM to anyone else -- and Toykit
  parks on its inbox's futex (`uchan_client_wait()`) with no kernel
  queue behind it. Four things to know. **A single writer cannot evict**:
  the compositor's ring writer can never touch what the client has not
  read, so the kernel's shed-oldest-input trick is impossible there.
  Input the inbox cannot take is DROPPED and counted (`ev_dropped` on
  the window, one `wm:` line per client); a STATE -- close, resize,
  focus, font, screen, a popup's dismissal -- is remembered as a bit on
  the window (`ev_pending`) and sent by `wm_client_flush_pending()`
  next frame with the state as it is THEN, which is
  `xdg_surface.configure`'s "latest wins". **A ping the inbox refused
  was never asked**: no serial is left outstanding, and a client whose
  inbox stays full is found unresponsive by exactly that route. **A
  dead compositor is noticed, not announced**: the kernel no longer
  asks clients to close; a client that waits out `UAPP_WAIT_MS` with
  nothing arriving asks `uchan_client_server_alive()` -- the beacon's
  live pid against the one its ring was granted to -- and closes itself,
  as a Wayland client sees its socket close. And **`uapp_post()` never
  enters the kernel**: a worker appends to a private queue and kicks
  the inbox word (`uchan_client_kick()`, an atomic add because two
  writers bump it). `WIN_EV_FONT`/`WIN_EV_SCREEN` reach the compositor
  on its queue and it forwards them, DRM-hotplug-uevent style.
- **`SYS_FS_GENERATION` is how ring 3 asks "has the filesystem
  changed?" -- and `SYS_FS_GENERATION_OF(path)` asks it of ONE
  directory, which is what a watcher of one directory should use.** No
  arguments for the first, the counter in RAX; a free poll is the
  entire reason it exists instead of a directory scan. But it moves for
  EVERY change on the disk -- every line `logd` writes -- so a reader
  that rescans on it rescans constantly: `init` re-read all twelve
  service descriptors per wake until 2026-09-28. The scoped one moves
  only when that path or a direct child changes (a hash collision can
  move it spuriously; nothing can make it miss). The compositor itself
  uses `SYS_FS_WATCH` events now. Either says something changed, never
  what.
- **A ring-3 process can own a real window** (`userland/wm/wm_client.c` +
  `kernel/proc/win_role.c`, protocol in
  `kernel/include/abi/win_proto.h`). Two rules matter before touching
  it. **Every client operation is a typed MESSAGE carried by the one
  `SYS_WIN_REQUEST` syscall, never a syscall of its own** -- that is
  what keeps the boundary a protocol; see `docs/decisions.md`. And
  **the split is no longer memory vs. presentation**: `wm_client.c` owns
  the whole window -- the list, its buffers, chrome, z-order and input
  routing -- and `win_role.c` owns the compositor ROLE, the
  framebuffer, the font and the compositor's input queue. They met at a registered
  `struct win_server_ops` until stage 6c deleted it. **A client draws with
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
  is a SURFACE of its own now** (the entry below; the bounds rect the app
  passes in is the in-window fallback when no surface is granted); and
  **there are no
  Alt+letter mnemonics yet** -- possible since Alt reaches a window as
  a bit rather than an ESC prefix; `KEY_F10` focuses the bar.
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
- **The Start menu is built from FILES**, one `.desktop`-style entry
  per app in `/usr/wm/applications/` (source of truth:
  `data/wm/applications/`, format documented in its README). `gui_apps.c`
  scans that directory at desktop startup, so **adding an app is
  dropping a file there**, not editing a table -- and it is picked up
  LIVE, no restart: the WM watches `fs_generation()` and re-reads the
  directory when it moves. **One directory feeds the Start menu and the
  desktop menu's Open > submenu**, with `ShowIn=desktop startmenu`
  choosing which; the desktop's own icons come from `/home/desktop`
  (the entry above), never from here. **Anything positional
  must go through `gui_app_visible_count()`/`_at()`** -- the Start
  menu's rows are indexed by position, so filtering the draw while
  hit-testing the unfiltered registry lands every click on the wrong app
  and looks correct in a screenshot. Windowed binaries live under
  `/bin/wm/{system,apps,demos}/` -- the class is the SOURCE directory
  (`userland/gui/<class>/`) and the Makefile derives the destination,
  same rule that already made `userland/gui` mean `/bin`.
- **A GUI TOOL REACHES A MACHINE THROUGH `QMPSession` AND
  `DebugConsole`, AND NOWHERE ELSE** -- which is what lets
  `TOYOS_REMOTE_HOST` point the whole suite at the bare-metal machine
  without touching a tool (`tools/remote_gui.py`,
  `gui_regress.py --host`). A tool that opens its own socket, shells out
  to `vm.py`, or hardcodes `.vm.serial` can only ever drive a VM. What
  hardware cannot do is REFUSED BY NAME (`RemoteUnsupported`: the
  monitor, a held button, relative motion) and reported as N/A rather
  than as a failure.
- **ON HARDWARE, ONE TELNET SESSION PER PROCESS, GIVEN BACK AT EXIT.**
  `inetd` serves four children (`userland/bin/inetd.c`), so a leaked
  session is a quarter of the machine's capacity and the next connection
  is refused as `connection closed` -- in whatever command happened to
  be in flight, which reads as the desktop being gone. Same shape as the
  VM rule that two `DebugConsole`s on one serial socket steal each
  other's replies.
- **AND THE KLOG IS NOT ON THAT WIRE.** A VM's debug console shares the
  serial port the kernel logs to, so `logs()`/`events()` see an app's
  layout report for free; over telnet they are fetched from `dmesg` as a
  delta. A tool that reads app output through the console works on both
  because of that, not by accident.
- **THE START MENU SHOWS ONE FOLDER AT A TIME**, from the entries'
  `Category=` key: a rail of the categories actually present, the
  selected one's apps beside it as two-line rows (name and `Comment=`),
  a header with the search field and a settings button, and the system
  actions as labelled buttons in a footer (Plasma's Kickoff in shape).
  Three things follow. **A folder
  exists exactly when something is in it** -- the sidebar is derived
  from the registry, so an empty folder is unrepresentable and an
  unknown key becomes its own folder rather than losing the app.
  **The app column scrolls past `SM_MAX_ROWS`** (the wheel, the keyboard,
  or a drag of its overlay scrollbar -- thin at rest, the shared uui_scrollbar
  while the pointer is on it), so a folder that outgrows
  the screen still fits. And **a row that is not drawn has no geometry** -- a test
  reaching an app in another folder opens that folder first
  (`DebugConsole.menu_app_row()` does it), which is why `gui menu
  --json` reports every app's folder beside the drawn rows.
- **THE SIDEBAR'S FIRST FOLDERS ARE NOT CATEGORIES.** Favourites,
  Recent and All Apps come before the `Category=` ones, and the first
  two exist only when they hold something -- the same "a folder exists
  when something is in it" rule the categories follow. By default the
  menu opens on the first of them, so it opens on what you use
  (Kickoff's default) and on All Apps on a machine where nothing has
  been pinned or launched yet; `desktop.start_opens` can name Recent,
  All Apps or the last folder used instead.
- **WHAT IS PINNED AND WHAT HAS BEEN LAUNCHED IS KEYED BY APP ID**, in
  `/etc/start-menu.conf` (`userland/wm/start_store.h`). Not by name and
  not by row: both move -- a rename, and every reload's re-sort -- and
  the desktop's Name-keyed icon positions already pay that price. A
  launch is recorded in `open_app()`, the ONE place a launcher starts
  anything, so the desktop's icons and the context menu's Open count too.
- **THE APP COLUMN SCROLLS, AND A ROW INDEX IS NOT A SCREEN POSITION.**
  The pane is capped (`SM_MAX_ROWS` detailed rows of height) and shows a
  window onto the list: `pane_count()` is the list in ITEMS,
  `pane_lines()` in LINES (a grid's row of cells), `L.pane_rows` is the
  window in lines, `scroll` is the first line shown, and `item_at()`
  turns a screen item into a list index. Anything PERSISTED -- the keyboard selection, what a
  click launches -- is in LIST coordinates, and every place that
  conflates the two is a bug. The keyboard scrolls its selection into
  view, or the highlight walks off the bottom while Enter goes on
  launching something invisible.
- **AND ITS SCROLLBAR IS AN OVERLAY** -- a thin thumb over the rows'
  right end that widens, animated, into `uui_scrollbar`'s groove and
  thumb while the pointer is on it or a drag holds it; the thumb drags
  and a click on the groove pages. Drawn and hit-tested with the shared
  `uui_scrollbar_*` helpers rather than a second implementation, and it
  counts LINES, so a grid scrolls by its rows.
- **THE START MENU'S SETTINGS ARE READ ONCE PER OPEN**
  (`read_settings()`, the Desktop > Start page): `desktop.start_list`
  (detailed | compact | grid), `start_opens`, `start_recent`,
  `start_hover`, `start_power`. A change applies at the next open, so
  nothing re-lays-out under the pointer. Every list style keeps the
  DETAILED footprint (width and height cap), so switching style changes
  what is in the column, not the card. `start_power` offers a TAIL of
  `wm_system_actions[]` -- `foot_count()`/`foot_action()` -- to the
  footer AND to search. `start_recent=off` FORGETS: launches are
  unrecorded and the stored ones removed (`start_menu_recent_on()`,
  asked by `open_app()` before it records). Hover opens a folder after a
  150 ms DWELL, so a hand crossing the rail does not switch folders.
- **A GRID'S KEYS ARE TWO-LEVEL**: with no cell selected Left/Right move
  the folder (as in a list); Down selects the first cell, then the
  arrows move between cells, and Up from the top line goes back to no
  selection.
- **TEXT THAT DOES NOT FIT IS MARKED, NOT JUST CUT** --
  `ugfx_draw_string_elided()`, which draws what fits and then `..`. The
  mark is two ASCII dots and NOT U+2026, because the font is indexed
  from ASCII 32 and an ellipsis glyph draws as nothing, which would make
  an elided line indistinguishable from a complete one. The desktop's
  icon captions have always done this; the Start menu's strip and rows
  do now, and anything new that clips should.
- **THE PANEL HAS A TOOLTIP, AND IT TAKES NO INPUT** --
  `userland/wm/wm_tooltip.h`, a row in the overlay table placed FIRST so
  it paints last and lands on top. Its click op always returns 0: a
  tooltip that consumed the click the person was about to make is the
  classic way to get this wrong. It appears on a DELAY
  (`UUI_TOOLTIP_DELAY_TICKS`, the toolkit's own, so the desktop does not
  feel like two systems), a move to another row re-arms it from zero
  rather than swapping the text under a box already up, and it is
  cancelled by whatever it describes going away. `uui_toolbar`'s is the
  widget version for a ring-3 app and cannot be reused: the panel has no
  widget tree and no surfaces of its own.
- **AND EVERY PATH THAT CLOSES THE START MENU GOES THROUGH
  `start_menu_close()`.** Three did it by clearing the flag inline, so
  the one thing a close has to tidy was done in one of them and
  forgotten in the others -- measured: a launch left the tooltip on
  screen over a menu that had gone.
- **SEARCH RANKS: exact, then prefix, then substring**, ties keeping the
  list's own order. "te" offers Terminal before Crash Test, which is
  what a person typing three letters means and what a plain substring
  match gets wrong.
- **TYPING GOES TO THE SEARCH FIELD WHENEVER THE MENU IS OPEN**, with no
  click into it, because it is the only thing in the popup that takes
  text -- Windows' and KDE's behaviour, and there is deliberately no
  focus to move. Search spans every folder AND the system actions
  (typing "shut" finds Shutdown), matching is case-insensitive
  SUBSTRING and deliberately not fuzzy, and Esc clears the query before
  it closes anything -- one level at a time, as a menu's Esc always is.
  The field is a real `uui_textbox` (`bare`: the menu draws its frame):
  a blinking caret, Shift+arrow and drag selection, the clipboard keys,
  a click that places the caret, and the I-beam over it. Left/Right/
  Home/End edit the query while there is one, and move the selection
  only while it is empty.
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
  `context_menu.c`, `confirm_dialog.c`, `wm_tray.c`,
  `cursor_theme.c`, `wm_client.c`. Split by concern for readability --
  it's still one tightly-coupled event loop, not decoupled components.
- **THERE IS ONE KIND OF WINDOW SERVER, AND `win_server_any()` IS HOW
  YOU ASK FOR IT.** It means "a ring-3 compositor holds the role", which
  is what `win_server_request()` gates on and what `vga.c` means by "is
  anything else painting the screen". There used to be a second kind, a
  registered ring-0 presentation layer, with a narrower
  `win_server_active()` beside it; that layer and that predicate were
  deleted in stage 6c (`docs/winserver-ring3-design.md`). **The pair is
  worth remembering as a shape rather than as an API**: two KTESTs
  guarded themselves with the narrow one so they would SKIP while the
  desktop was up, and quietly stopped skipping the moment the desktop
  became a process. A predicate named for what used to be the only
  implementation is worth re-reading whenever that stops being true.
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

## A WINDOW'S GEOMETRY IS REMEMBERED PER APP, AND THE KEY IS `app_id` -- NEVER `app_identity`

`userland/wm/wm_geometry.c` saves position and size in
`/etc/windows.conf` at `close_window()` and applies them at create time,
for kernel-space apps and ring-3 clients alike. Default on; an app opts
out with `RememberGeometry=false` in its `.desktop` entry.

**`app_identity` is a per-boot spawn-order index** (`win_server.c`), so
it is not a persistence key however much it looks like one -- "identity
3" is a different app after a reboot. `app_id` is the app's own stable
string.

**Saving belongs at the close, not where geometry changes.** Five call
sites write x/y/w/h and a drag writes them every frame.

**A client's size is a request, AND A CLIENT MAY SAY NO.** It owns its
buffer, so a restore asks via `wm_client_send_resize()` and the client
answers; the WM must not assign `win->w` for a client window at all.
Assigning it as well as asking is a bug with a delayed fuse: a
FIXED-SIZE app declines (Toykit refuses a `WIN_EV_RESIZE` unless the app
declared `WIN_HINT_RESIZABLE`), and the frame then sits at a size the
client never adopted -- a small window painted into the corner of a big
one, with nothing able to correct it, since the present that would carry
the truth matches the `client_w/h` the restore never touched. The frame
follows the PRESENT, for an accepted resize and a refused one alike.

**A declining client still has to answer**, by presenting: the WM has
already been told a size, and a present carries the buffer's own
dimensions. Ignoring the event leaves the same stale frame.

**Which means one wrong size is otherwise permanent**, because the file
is rewritten from the frame on every close. A `gui resize` on a
fixed-size app, or a font change moving its natural size, would reopen
it that way for ever.



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
  **A widget's DEFAULT colour is `UUI_COLOR_UNSET`, resolved against a
  role at draw time -- never a copy of the role's current value.** The
  greys are a ladder; a widget that had copied a rung as a literal fell
  off it the first time the ground moved (labels drew as lighter boxes,
  the menu bar vanished into the page).

- **A WINDOW ANIMATES AS A GHOST, AND THE STATE CHANGE THAT STARTS IT
  CALLS `wm_anim_*()` FIRST, THEN CHANGES THE STATE.** A window opening,
  closing, minimizing or coming back is drawn by `userland/wm/wm_anim.c`
  as a SNAPSHOT of its chrome and content, blitted scaled and faded
  along a 150 ms tween while the live window is hidden
  (`wm_anim_hides()`) or already gone. The snapshot is taken from the
  window as it IS, so every site that minimizes, restores or destroys
  a window calls the starter before it changes `state` or unmaps the
  buffers -- `on_window_destroyed()` calls `wm_anim_close()` before its
  `unmap_client_window()`, and a new site that changes a window's state
  does the same. The first present of a new toplevel starts the open
  animation, not window creation: a ghost is a snapshot of what arrived.
  Ghosts damage their old and new rects every frame through
  `wm_damage_window_rect()`, the WM loop waits `WM_ANIM_FRAME_MS` while
  one is in flight, and `gui state --json` reports `anims` so
  `DebugConsole.settle()` waits them out -- a frame compared while one
  runs is a frame of a ghost. `desktop.animations` turns all of it off.

- **AN EFFECT THAT PAINTS SOMETHING OTHER THAN ITS LERPED RECT MUST
  DAMAGE THE WHOLE TRAVEL.** The genie fills a tube from the window's
  top edge to the taskbar button; `shatter` strings staggered tiles
  between the two. In both cases the rect `wm_anim_step()` computes
  covers only part of what is drawn, so `damage_ghost()` special-cases
  them -- and the failure is not subtle: shatter's first version damaged
  the shrinking union and left **15,500 stale pixels** starting at the
  window's own top-left, because the last tiles to leave were still
  sitting there. `animation_test.py`'s "nothing stale once it has
  settled" is the check that catches it, by comparing the settled screen
  against a forced full repaint.

- **AN EFFECT'S OWN OPTIONS ARE A DESCRIPTOR, THE WAY A SCREENSAVER'S
  ARE.** `/usr/wm/effects/<name>.effect` declares what an effect lets
  you change and `/etc/effects/<name>.conf` holds the values -- the same
  format, the same parser (`usaver_load_files()`) and the same reason as
  `/usr/wm/savers`: an option belongs to ONE effect, and a flat setting
  in `/etc/settings.d` cannot say so. `shatter`'s piece count would
  otherwise sit in Appearance -> Effects being asked about while Scale
  is selected, and every effect added later would leave another orphan
  row. System Settings synthesises the rows for whichever owner the page
  carries (`owner_kind()`), so a second effect with options needs no
  edit there. **An effect with no descriptor has no options**, which is
  a state and not an error -- it is every effect but `shatter`.

- **AN EFFECT'S OPTIONS OPEN IN A DIALOG; A SAVER'S STAY ON ITS PAGE.**
  `Settings...` sits under the effect dropdown and opens a modal owned
  window (`uapp_window_open()`, `UAPP_WIN_MODAL`) carrying the same
  slots the page would have drawn -- `emit_slot()` builds a control for
  either surface, so there is one switch and not two. The button
  appears only where the selected owner declares options; an effect
  without a descriptor gets none rather than a dialog that opens empty.
  **The saver keeps its options inline** and that is not an
  inconsistency: its page carries Test, and a window over it would
  cover what Test is previewing. `owner_uses_dialog()` is where that
  decision lives. Set the dialog's `log_prefix`, or a test has to guess
  pixels at a window the toolkit would otherwise describe for free.

- **ONE `g_saver` SERVES WHICHEVER PAGE HAS AN OWNER, so a page without
  one must CLEAR it.** It held the last page's options otherwise, and
  Appearance -> Effects offered a Settings... button reporting the
  SCREENSAVER's three options and opening nothing. The inverse is just
  as wrong: a dropdown whose staged name is momentarily empty must NOT
  clear, or moving the saver dropdown away and back loses its options.
  `open_group()` logs `settings: owner <kind> <name> opts <n>` so which
  of the two happened is a fact rather than a symptom.

- **AND THE STAGGER IS WHAT TELLS TWO PIECEWISE MOTIONS APART.** Shatter
  pours with a 35% row stagger, so the window empties from the edge
  nearest the taskbar; it explodes with 8%, because a burst whose bottom
  row leaves a third of a second before its top row reads as a window
  PEELING rather than breaking. The kick is bounded
  (`SHATTER_KICK_PCT`) precisely so the damage margin can be.

- **A WINDOW'S DAMAGE IS ITS OUTER RECT -- THE FRAME PLUS ITS SHADOW --
  AND `wm_damage_window_rect()` IS HOW IT IS DAMAGED.** The compositor
  paints a drop shadow outside every toplevel and popup
  (`userland/wm/wm_shadow.c`), so a damage call that names a window's
  own `x, y, w, h` leaves the shadow behind when the window moves,
  closes, or changes focus (the focused shadow is larger). Every site
  that damages a window rect goes through `wm_damage_window_rect()`,
  which pads by `wm_shadow_margin()`; `window_intersects_damage()` tests
  the padded rect too. A rect that is NOT a window's -- a title bar
  alone, a drag outline, the taskbar strip -- keeps `wm_damage_rect()`.
  The WM's own menus and flyouts cast the popup shadow and pad their
  damage the same way; a Toykit popup window gets both from
  `render_scene()`. `tools/damage_sweep.py` is the check, and its
  positive control -- `gui damage shrink 4` before a drag -- is the proof
  the check can see a miss. FOCUS IS PART OF THIS TOO: the focused
  window's shadow is larger, so `compute_window_damage()` damages both
  windows whenever the focused identity changes between frames, however
  focus moved (a raise, a minimize of the top window, a close).

- **A SCROLL GLIDES AS A DISPLACEMENT OF THE DRAWN CONTENT, NEVER OF THE
  POSITION -- AND A WIDGET ARMS IT, THE HELPER DOES NOT GUESS.**
  `ui/uui_scrollanim.h`: the widget's `top`/`offset`/`scroll_offset`
  jumps exactly as before, so every reader, hit-test and test tool sees
  the same numbers; only the draw adds `disp` to its content y for
  ~150 ms (`UUI_SCROLL_MS`), eased by `lib/utween.h`, and folds the same
  `disp` into a PIXEL-unit scrollbar so the thumb moves with the rows.
  Three calls: `arm()` in the wheel, trough-page and key paths;
  `cancel()` in a thumb drag (the thumb follows the cursor 1:1,
  gui-guidelines rule 2) and on a data reload; `sync(pos_px)` at the top
  of draw. Arming is explicit because a change that is not the user's
  (a reload that shortens a list, a resize that re-clamps) must not
  glide -- it reads as the content lurching on its own. The widget
  draws the rows the displacement uncovers (`uui_scrollanim_extra_rows`)
  under `ugfx_clip_intersect()` of its own rect, so a displaced row
  cannot paint over a header or a neighbour, and hit-tests subtract
  `anim.disp` so a click mid-glide lands on what is on screen. Frames
  come from `uui_anim_request()` (`ui/uui_anim.h`): uapp's pump waits
  one frame instead of its long park while one is pending, so a process
  wakes per frame only while something moves. `desktop.smooth_scroll`
  turns it off; the helper reads the setting at each `arm()`, one
  syscall a notch. `utext` gates the whole thing behind `animate`,
  because `/bin/edit` draws once per keystroke and a displaced frame
  there would sit until the next key.

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

- **A WIDGET DESCRIBES ITSELF, AND THE LAYOUT LOG HAS ONE VOCABULARY:
  `<prefix>: layout <name>[.<part>] [i [j]] x y w h`.** An app names a
  widget with `uui_item.name` and calls `uapp_log_layout(a, prefix)`
  from `on_draw`; the walk emits every named widget's `bounds` and then
  whatever its `describe` op adds -- a menu's `title i`, `popup l`,
  `item l i` and `open`; a strip's `slot i`, `new` and `selected`; an
  image's `picture`; a file view's `selected` (`ui/uui_describe.h`).
  Containers are entered, so a widget inside a scroll view reports like
  any other. **A widget that a test needs to drive gains a `describe`
  op, never a per-app logger**: the seven hand-rolled loggers this
  replaced each spelled the same menu rects in their own words
  (`menutitle`, `title`, `layout item`), so nothing written for one
  tool served the next. An app with hand-drawn chrome that still owns
  a real widget reports it with `uapp_log_widget()` (Minesweeper's menu
  bar); what it draws itself -- a board, a caret cell, a canvas -- it
  still logs through `uapp_logf_layout()`, which is the same gate and
  the same per-frame dedupe. `.name` sits AFTER `main_size` so a
  positional initialiser is unchanged; an unnamed item is left out of
  the log, which is how a label stays quiet.

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

- **AN IMAGE IS ALSO ENCODED IN RING 3, AND THE TWO FORMATS ARE FOR
  DIFFERENT THINGS.** `uimg_encode()` / `uimg_save()` are the other half
  of the codec table, and a row now carries an `encode` slot beside its
  `decode`. Two formats, and the pair is the point rather than variety:
  **`qoi` is what toy-os can open again** (the Image Viewer decodes it,
  it is lossless, and on flat UI content it beats PNG), and **`png` is
  what LEAVES the machine** -- a host, a browser, a bug report. Nothing
  here decodes PNG, so its row has `decode = NULL` and `uimg_decode()`
  answers `-ENOTSUP` with a sentence saying this build writes the format
  and cannot read it, which is a different answer from a corrupt file
  and the reason that error code exists.

  **THE FORMAT IS NAMED, NEVER GUESSED FROM THE PIXELS**; `uimg_save()`
  takes it from the path's extension because that is what a person
  typing a filename means, and an unknown extension is refused rather
  than defaulted.

  **AND THE ENCODERS ARE CHECKED BY A FOREIGN DECODER, NEVER BY OURS**
  (`tools/uimg_codec_hostcheck.py`, Pillow plus Python's `zlib`). An
  encoder tested against this repo's own decoder passes whenever the two
  share a mistake, and the two mistakes an image encoder actually makes
  are exactly that shape: a QOI index table updated on the wrong chunk,
  or a Huffman code packed least-significant-bit-first. Both produce a
  file that round-trips perfectly here and that nothing else can open.

- **PNG IS READ AND WRITTEN NOW, AND COMPRESSION IS A LIBRARY:
  `lib/uinflate.h`.** DEFLATE both directions, with the zlib and gzip
  wrappers. It is a library rather than something private to the PNG
  codec because it has TWO real callers -- the codec, and `/bin/wget`
  inflating a gzip `Content-Encoding` -- which is the bar this project
  sets for factoring anything out.

  **The input is one contiguous buffer and the output is a callback**,
  and that asymmetry is the design. A decompressor that can suspend
  anywhere is a state machine with a dozen resume points whose bugs only
  appear on an input split at an awkward byte; consuming from one buffer
  keeps it a straight loop. The cost is that a caller holds the
  COMPRESSED input -- which PNG already did, and which for an HTTP body
  is the size gzip bought you.

  **RING 3 ONLY, and that is not an oversight.** Linux carries
  `lib/zlib_inflate/` in the kernel, but to unpack its own kernel and
  initrd; toy-os compresses neither, so there is no ring-0 caller to
  compile twice for.

  **What the decoder reads is 8-bit, every colour type, not
  interlaced** -- greyscale, truecolour, palette, both alpha variants,
  plus tRNS. A 1/2/4/16-bit depth or an Adam7 file is `-ENOTSUP` with a
  sentence naming what it found, which is `uimg_jpeg.c`'s arrangement
  for progressive and CMYK: the file is fine, this build is not, and
  that is a different sentence from "corrupt".

- **THE COMPOSITOR COPIES PIXELS AND THE CLIENT ENCODES THEM:
  `WIN_REQ_SCREENSHOT`, `lib/ushot.h`.** A program asking for a capture
  creates a shared-memory object, grants it, and names a rectangle; the
  compositor copies its back buffer into it and answers with the rect it
  actually took. Nothing about files, formats or paths crosses the
  protocol -- which is what lets `/bin/screenshot` be an ordinary
  program with no window, usable over `telnet` on a machine whose screen
  nobody is watching, and is how `tools/remote.py screenshot` reaches
  the bare-metal laptop at all.

  This is `wlr-screencopy-v1`'s shape, and KWin's `ScreenShot2` (an fd
  rather than a name). It is deliberately NOT X11's, where any client
  reads the root window -- there is no permission model here to enforce,
  but the SHAPE is what makes adding one later a change in one function
  rather than in the protocol.

  Four things that fail silently if forgotten. **The capture RENDERS A
  FRAME FIRST**, because a request arrives mid-pump and anything that
  changed this iteration is not in the back buffer yet. **The alpha byte
  is forced OPAQUE**: a compositor surface is `0x00RRGGBB` and a
  `struct uimg` means `0xAARRGGBB`, so a straight copy is an image every
  alpha-aware consumer reads as fully transparent (it shipped as a black
  preview). **A scanout lease is refused with `-EBUSY`** rather than
  answered with the stale frame behind a fullscreen game. And
  **`WIN_SHOT_NO_SELF` leaves the ASKING client's windows out** for that
  one frame, so a screenshot tool is not in its own screenshot -- the
  window visibly goes away and comes back, which is what GNOME
  Screenshot and Spectacle look like.

- **A PICKER ASKS WHAT IT WOULD GET BEFORE IT ASKS FOR IT:
  `WIN_SHOT_WINDOW_AT` + `WIN_SHOT_PROBE`.** Pointing at a window to
  capture it needs the window's rectangle on every pointer move, so
  there is a mode that resolves a rect from a POINT and a flag that
  answers it while copying nothing -- no render, no buffer, no capacity
  check. Rendering a frame per mouse motion to find out what is under
  the cursor would be a full repaint per motion.

  **The choice is made over a FROZEN capture, not the live screen.** The
  app takes the full-screen shot first, goes fullscreen showing it, and
  crops in memory when you click -- so a window is captured as it was
  when you pressed the button, not as it is when you finally reach it.
  Spectacle and GNOME's shell both work this way.

  **The outline is TWO rings, accent inside dark.** One ring is
  invisible against a window whose own chrome is that colour, which on
  this theme is every focused title bar -- the same reason
  `ugfx_draw_string_shadowed()` exists. And the overlay REDRAWS ONLY
  WHEN THE TARGET CHANGES: a repaint there is a full-screen blit, and
  one per pointer move over the same window is a megabyte of memcpy for
  an identical picture.

- **THE POINTER IS EXCLUDED BY DEFAULT, AND IT IS SUBTLE IN BOTH
  DIRECTIONS.** `WIN_SHOT_POINTER` asks for it; without it a capture has
  none, matching X11, Windows' PrintScreen and GNOME's default. What
  makes it more than a flag is that the back buffer's state depends on
  the machine: with a SOFTWARE cursor the sprite is already in the
  buffer, so "no pointer" means UNDRAWING it (from the compositor's own
  saved backing pixels, exactly); on a HARDWARE cursor plane it is not
  there at all, so "with pointer" means drawing it. Neither is the
  caller's to get right, which is why the flag is on the request and
  both halves live in `wm_render.c`.

- **MP3 IS THE CODEC TABLE'S SECOND ROW, AND ITS TABLES CARRY THEIR OWN
  PROOF**

  `userland/lib/usnd_mp3.c` is MPEG-1 Layer III, written here rather than
  vendored -- the same call `uimg_jpeg.c` made against libjpeg. Adding it
  was a file and a row in `usnd.c`'s `g_codecs[]`, which is what that
  table exists for.

  Three things to know before editing it. **The data is separate from the
  logic** (`usnd_mp3_tables.h`), because only three tables have no
  generating formula -- the Huffman codes, the 512-tap synthesis window
  and the scalefactor band edges. Everything else a decoder needs IS
  derivable and is derived at runtime; adding a table for something
  computable is the wrong instinct here. **`usnd_mp3_selftest()` proves
  the Huffman tables structurally** -- every one a complete prefix code,
  Kraft sum exactly 1 over exactly `dim*dim` pairs, no audio required --
  and it exists because a hand-written table failed it during
  development, which is how that approach was abandoned. And **it
  REFUSES rather than guesses**: Layer I/II, MPEG-2/2.5, free-format and
  intensity stereo are `-ENOTSUP`, the same distinction the JPEG decoder
  draws on a progressive image.

  Verify a change with `tools/usnd_hostcheck.py` (against ffmpeg, with a
  `--positive-control` that must go red), never by listening. Measured
  agreement is ~1.6e-6 -- one LSB in 32768 -- so a real break is never
  subtle.

- **MIDI IS THE CODEC TABLE'S THIRD ROW: A SCORE RENDERED THROUGH A
  SOUNDFONT, ON A FIXED BLOCK GRID**

  Three files, split by concern: `usnd_mid.c` (the Standard MIDI File
  parser and sequencer, and the codec row), `usnd_synth.c` (a GM
  synthesiser -- voices, envelopes, LFOs, filter, modulators) and
  `usnd_sf2.c` (the SoundFont 2 parser, which flattens presets x
  instruments into regions). The bank is `/usr/share/soundfonts/`: any
  `.sf2` there outranks the built-in `toy-gm.sf2`
  (`tools/gen_sf2.py`), which stays as the fallback.

  What bites. **The synth's output must not depend on how the CALLER
  chunks its reads**: control blocks sit on a fixed 64-frame grid of
  song time, and a seek re-aligns it (`usynth_align()`), or a seek
  cannot land where continuous playback would have -- `midi_test`
  seeks OFF the grid on purpose, because a seek to a multiple of 64
  passes without the fix. **The per-sample path is integer**: under
  TCG every SSE instruction is a softfloat call, and the float version
  dropped out on a real bank. **Every controller reaches sound through
  the region's MODULATORS** -- velocity, CC7/10/11, the wheel, the bend
  are the SF2 defaults merged with the bank's, never a side path that
  a bank's own modulator could not override. **The attenuation
  generator counts 0.4 dB a unit** (FluidSynth's and the EMU8000's
  reading, which real banks are voiced against), and **the modulation
  envelope's attack is convex**. **`usnd_load_info()` does not load the
  bank** (`info_only`), so selecting a file in the Player never waits
  on 30 MB.

  Verify with `tools/midi_hostcheck.py` -- the shipped bank AND a real
  one (`--sf2`) against FluidSynth, plus a sanitizer fuzz of both
  parsers -- and `/tests/midi_test`, whose byte-built sine bank makes
  every pitch and onset exact.

- **AUDIO IS DECODED AND MIXED IN RING 3, AND `lib/usnd.h` HAS THREE
  SEAMS.** The kernel gives out ONE exclusive PCM stream and never mixes,
  so formats, rate conversion and playing several sounds at once are all
  this library's -- the same call `uimg.h` makes about images, and what
  ALSA's dmix, PulseAudio, PipeWire and Windows' audio engine all do.
  The seams: a **codec table** (`probe/open/read/seek/close`: WAV, MP3
  and MIDI, each a file and a row), a **sink** (`usnd_sink.h`, where mixed samples
  go -- the exclusive device today, a sound daemon as a second row), and
  **voices** (an eight-voice per-process mixer; with a daemon it becomes
  the app's submix). **A CODEC NEVER RESAMPLES** -- it reports its file's
  native rate and yields s16 frames in it, and the library converts once,
  where every real system puts that stage. **Nothing in the public
  header names the ring, `hw_pos` or `SND_*`**, which is the whole reason
  a daemon can arrive without touching an app. **No hardware is not an
  error**: `usnd_init()` returning -ENODEV (no card) or -EBUSY (another
  process holds the stream) is ordinary, and an app that wants sound if
  it can get it ignores the result and plays into silence -- the default
  boot has no AC97 at all. Errors follow `uimg.h`'s split, `-ENOTSUP`
  for a good file this build refuses (a float WAV) against `-EINVAL` for
  a broken one, with the sentence in `usnd_last_error()`. **Long
  playback is a WORKER THREAD's**, not an `on_tick`'s: a missed refill is
  audible and the ring is only 341 ms deep.

- **TWO WIDGETS OWN MEMORY, AND BOTH MUST BE RELEASED: `uui_image` and
  `uui_markdown`.** The image borrows the `struct uimg` (the app decodes
  and owns that) but owns the SCALED copy it caches, so an app that
  re-points one
  at image after image without `uui_image_release()` leaks a screen's
  worth of pixels each time. The cache is the reason the widget exists at
  all rather than three lines of `ugfx_blit()` per app: resampling a
  screen-sized picture costs tens of milliseconds and a repaint happens
  on every damage event. **Set `max_w`/`max_h` on any viewer of arbitrary
  files** -- natural size is the image's own, and `uui_layout` OVERFLOWS
  rather than shrinking, so a 4000px photograph otherwise asks for a
  4000px window and gets one.

  `uui_markdown` owns three FONT ARENAS -- two heading sizes and a
  monospace face, about 200 KB -- released by `uui_markdown_free()`. An
  app that keeps one view for its lifetime never needs to call it; one
  that creates and drops views does, and nothing will tell it so.

- **A WIDGET BORROWS ITS TEXT, SO THERE IS NO SET-CALL PER UPDATE.**
  `uui_label` and `uui_statusbar` hold a `const char *` into the app's
  own buffer and never copy it, so once `uui_label_init()` or a pane's
  `.text` points at `g_status`, rewriting that buffer IS the update --
  `snprintf(g_status, sizeof g_status, ...)` and nothing else. Calling
  `uui_label_set_text()` again afterwards re-assigns the same pointer
  and does nothing. The trap is the inverse, and it is silent: a buffer that
  goes out of scope, or one reused for a second widget, leaves the
  widget reading freed or wrong memory with no call to blame it on.
  Same rule as `uui_image`'s `struct uimg` and `uui_dialog`'s rows.

- **A MARKDOWN DOCUMENT IS A WIDGET, AND IT DOES NOT PARSE ANYTHING.**
  `uui_markdown` draws; `lib/umd.h` decides what Markdown MEANS --
  `umd_classify()` for blocks and `umd_inline_walk()` for `code`,
  **bold** and links. That split is the point: `/bin/doc` renders the
  same pages to a TERMINAL through umd's other half, and a second parser
  in the widget would be two subtly different ideas of what `**` means
  in documents nobody would think to check both ways. Four things to
  know. **It scrolls in PIXELS, not lines**, because a heading, a rule
  and a code block are not the same height -- that is the difference
  between it and `utext`, where every row is one character cell. **The
  caller owns the text**; the widget keeps a pointer and re-walks it
  every frame, so "update the preview" is handing it the buffer again
  and there is no parsed tree to keep in sync with an edit. **One walk
  measures AND draws**, because two would be two ideas of how tall a
  heading is and the scrollbar would disagree with the page. And **a
  font that fails to load is not an error** -- it falls back to the
  session bold face, so a machine with no fonts on disk still renders a
  readable document.

- **A LINK IN A MARKDOWN PAGE IS A CODE SPAN THE APP NAMES.** The pages
  write `ping`, never a Markdown link to the page's file -- the same text reads right in a
  terminal through `/bin/doc` -- so `uui_markdown_set_links()` takes a
  predicate and the widget makes every inline-code word it accepts a
  link: accent ink, underlined, no code tint. Where each was drawn is
  recorded AS IT IS DRAWN, so a link scrolled out of view cannot be
  clicked and nothing re-derives the wrap. A press arms, a release on
  the same link commits, and the app reads it with
  `uui_markdown_take_link()` from `on_widget`. Help's predicate is "names
  another page": the page's own name is not a link.

- **ALT+ARROW BELONGS TO THE WINDOW, NOT THE FOCUSED WIDGET.** It is
  Back/Forward in every browser, file manager and help viewer, so a
  focused widget that uses bare arrows (`uui_tree`) returns 0 for them
  with Alt held and the key reaches the app's `on_key`. The focus ring
  delivers keys BEFORE `on_key`, which is why the widget has to decline.

- **A PATH BAR AND A PLACES LIST NAVIGATE NOTHING: THEY PARK A PATH.**
  `uui_pathbar` (a breadcrumb that turns into a text field) and
  `uui_places` (named folders, then every mount with its free space) are
  shared by the File Manager and `uui_filedialog`. A click or an Enter
  parks the path for `uui_pathbar_take()`/`uui_places_take()`; the APP
  decides whether it exists, goes there, and calls
  `uui_pathbar_set_path()`/`uui_places_select_path()` with where it
  actually is. A typed path the app refuses leaves the field up with the
  text in it. Read the taken path from `on_widget` whatever the reason --
  a hover reaches there too, and only a click parks anything.

- **A TOOLBAR BUTTON MAY CARRY A WORD AND A CHEVRON, AND THE APP OPENS
  THE MENU.** `uui_toolbar_item` has `label`, `flags` and `accel` after
  `code`; `-Wextra` warns on a positional initialiser that leaves any of
  them out, so every one names all six. `UUI_TB_MENU` draws
  a chevron (the app opens a popup under `uui_toolbar_item_rect()` when
  the code commits), `UUI_TB_END` packs that item and the rest against
  the right edge, and `compact` (on the toolbar) sizes a strip that
  lives inside a status bar.

- **A TOOLBAR THAT CAN RUN OUT OF ROOM SETS `overflow`, AND WHAT DOES
  NOT FIT FOLDS INTO ITS MENU FROM THE END.** QToolBar's extension
  button, Windows 11's "See more": with `overflow = 1` the trailing
  items the width cannot hold leave the strip for a `uui_menubar`
  opened as a context menu under one more button (`t->more`, "See
  more" unless the app relabels it). The rows are the items' labels (or
  tips) and `accel`s, ticked and greyed by the SAME `item_flags`, and a
  row commits its code exactly as the button would -- a `UUI_TB_MENU`
  item's popup, anchored at its folded zero-width rect, opens under
  More. So ORDER THE ARRAY
  BY WHAT MAY FOLD FIRST -- it is last -- and set the window's
  `min_w` from `uui_toolbar_shown()` so the items that must stay, do
  (Shapes measures it in `on_size`, with its readout at its widest). A
  folded item's rect has zero width; a `UUI_TB_TEXT` readout never folds into the menu, so keep it
  ahead of what can. In a layout, give the strip `UUI_FILL_W`:
  `natural_size` still asks for every item.

- **A DETAILS VIEW DROPS COLUMNS FROM THE END, AND A COLUMN'S INDEX IS
  ITS SORT KEY.** `uui_fileview`'s columns are Name, Size, Modified,
  Type; a pane too narrow for all four shows fewer by lowering
  `col_count`, never by removing one from the middle, so every index --
  which is what `uui_fileview_set_sort()` and the header click store --
  keeps its meaning. `uui_table` sorts by a hidden column all the same.
  A row's icon comes from `uui_table_set_icon()`, a cache lookup at text
  height (`lib/ufiletype.h` names the type and the icon).

- **A DIALOG IS A CARD, AND ITS DANGEROUS ANSWER IS RED.** `uui_dialog`
  dims the window, draws a rounded, shadowed card, and styles its
  buttons: `UUI_DLG_PRIMARY` in the accent, `UUI_DLG_DANGER` in red,
  plain otherwise -- and with no button styled, the DEFAULT one is drawn
  primary. `uui_dialog_set_picture()` puts what the question is about
  beside it (a thumbnail, a type icon) and `uui_dialog_set_note()` a
  tinted consequence line under the rows; both are cleared by every
  open, so set them after it. A text field is the dialog's BODY (focus
  it with `uui_dialog_focus()`): Return still commits the default.

- **A NAME IS EDITED WHERE IT IS, AND THE APP DOES THE RENAME.**
  `uui_fileview_begin_rename()` turns the selected item's name into a
  field over its label or name cell, the part before the extension
  selected; Enter or a click elsewhere parks the new name for
  `uui_fileview_take_rename()`, Esc drops it. The row is found by NAME
  each frame, so a reload under the edit keeps it on the right file,
  and a navigation ends it. An app routes EVERY key to the pane while
  `uui_fileview_renaming()` -- F2, Delete and Ctrl+C are text editing
  in there -- and finishes the edit on a press anywhere outside it
  (`uui_fileview_finish_rename()`), since a press on another widget
  never reaches the pane.

- **A POPUP ASKED TO CLOSE IS DESTROYED, NOT JUST FORGOTTEN.** A
  `WIN_EV_CLOSE` on a popup's slot is answered with `WIN_REQ_DESTROY`
  before the owner hears `done`; releasing the buffers alone left the
  compositor holding a window nobody drew, and it raised "not
  responding" over the app -- a box that then took the next click.
  `WIN_EV_POPUP_DONE` needs no destroy: the compositor already swept it.

- **THE WALLPAPER IS A REGISTERED SETTING, AND ITS VALUE IS A NAME.**
  `desktop.wallpaper` (a filename stem under `/usr/share/wallpapers`, or
  `none`) and `desktop.wallpaper_mode` (`fill`/`fit`), DECLARED by
  `/etc/settings.d/desktop.wallpaper*` -- a file says what the setting
  is, the ring-3 desktop owns the behaviour and notices through the
  generation counter. So `config set
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

- **THE TASKBAR'S THICKNESS IS A REGISTERED SETTING:
  `desktop.taskbar_height`, in PIXELS, 24..96, default 48.** DECLARED
  by `/etc/settings.d/desktop.taskbar_height` as an `int`, beside the
  Start button and the wallpaper in `/etc/desktop.conf`, so
  System Settings shows it as a spinbox with no app edit. A pixel count
  rather than small/medium/large because that is XFCE's "Row size
  (pixels)" and the number KDE stores for a panel (Windows 11 fixes 48,
  GNOME exposes nothing). **The default is a CONSTANT, not a font
  formula**: the registry answers from ring 0 and the strip is drawn in
  ring 3, and the two tiers' fonts do not share a line height -- a
  formula gave 36 on one side and 40 on the other. **`taskbar_h` is no
  longer `WM_TITLEBAR_H`**: a panel and a title bar are separate
  measurements on every desktop, and the title bar stays font-derived.
  The WM adopts a change on `taskbar_poll_config()`'s generation poll
  and relays through `wm_layout_changed()` -- the walk
  `wm_screen_changed()` already made (icon grid, maximized windows, the
  clamp, the overlays), factored out so a height change and a mode
  change cannot re-derive the usable area differently. **It is the
  BAR's thickness; `taskbar_h` is the BAND**, which is the bar plus
  `TASKBAR_FLOAT_GAP` while the panel floats -- size things from
  `taskbar_bar_h()`, reserve space with `taskbar_h`. A button's icon is
  half the bar (24 at the default, Windows 11's) and a tray icon two
  thirds of it (32), CAPPED at `TASKBAR_ICON_MAX` and that plus 4,
  because the masters are 64px and a 96px strip would otherwise
  upscale. And **a button's
  natural width includes the icon column** (`win_btn_w()`), since
  `make_label()` subtracts it -- "untitled" once drew as "un" until it
  did. `tools/taskbar_test.py` sets,
  reads back and unsets it, before its overflow section fills the
  process table.

- **THE START BUTTON'S APPEARANCE IS A REGISTERED SETTING:
  `desktop.start_button` = `text` | `icon` | `both`.** DECLARED by
  `/etc/settings.d/desktop.start_button`, sharing `/etc/desktop.conf`
  with the wallpaper -- which is what makes
  it `desktop.`-namespaced, since a namespace is the registered name of
  the FILE. Three choices rather than a boolean because that is XFCE's
  Whisker Menu verbatim (Icon / Title / Icon and title) and KDE's
  launcher option, and because a boolean cannot say `both`, which is
  what Windows 95 through 7 shipped. **The default is `icon`** (since
  2026-09-29, with the taskbar redesign); **it was `text` until
  2026-09-07 and `both` between**, so that a machine with nothing written
  looked as it had before the setting existed; the cost of moving it is
  that the button's width is derived from what is in it and every window
  button starts to the right of it, so the whole strip shifts under every
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

- **AN OVERLAY IS A ROW IN A TABLE, AND THE TABLE DRIVES DRAWING,
  CLICKS AND HOVER -- `userland/wm/wm_overlay.h`.** The Start menu, the
  context menu, the calendar, the two tray flyouts, the file picker
  and the confirm dialog are seven rows in modality order; drawing
  walks it BACKWARDS, so the row that gets the first click is painted
  last and lands on top. **The hover op is the reason it exists.** An overlay
  supplies `hover_at(mx, my)` returning an OPAQUE TOKEN for whichever
  control the pointer is over, plus `damage()`; the core compares the
  token against the last one and damages on a change, which is the
  whole mechanism and used to be written out four different ways.
  Three things to know. **Forgetting to join the table is LOUD**, and
  that is why the table carries `draw` and `handle_click` as well: an
  overlay left out of it never appears and cannot be clicked, where a
  hover-only registry would still fail the silent way. **A hover
  derived from `(mx, my)` inside a draw is INVISIBLE, not slow** -- a
  mouse move alone takes the compositor's cursor-only path, so such a
  highlight is painted only when something else asks for a frame, which
  on an idle desktop is the clock, once a second; the volume flyout
  shipped that way for a day and the context menu had been that way
  since it was written. And **nothing re-hovers while the primary
  button is down**, stated once in the core rather than guarded per
  overlay, so a dragged slider or an armed button keeps its highlight.
  **AN OPEN OVERLAY OWNS THE KEYBOARD, THROUGH THE TABLE'S `key` OP**,
  offered in the same modality order as a click -- an xdg_popup's grab,
  and Windows' menu loop. **It consumes only what it acts on**: a key
  held with Ctrl/Alt/Super falls straight through, or a global shortcut
  would stop working whenever a popup happened to be open. The op is
  asked in `wm.c` AFTER the Super gesture and BEFORE `wm_shortcut_fire()`
  and any routing to a window.
  **A DISMISSABLE OVERLAY DECLARES `close`, AND AN OPEN PATH CALLS
  `wm_overlay_close_others(keep)` RATHER THAN NAMING ITS PEERS** --
  which is what makes the popups mutually exclusive, and what the
  hand-written version got wrong: the Super key closed two of the four
  it should have, so opening the Start menu over a volume or brightness
  flyout left the flyout drawn underneath it. A MODAL row leaves `close`
  NULL (the confirm dialog, the file picker), so another popup opening
  cannot dismiss it. **And a popup is placed by `wm_popup_place()`**,
  one clamp for the four that each had their own: `WM_POPUP_MARGIN`
  from the left and right edges and from the taskbar, never above the
  top. Adopting it moved the calendar and a taskbar-anchored context
  menu up 4 px (their gap was 0) and pulled the tray flyouts' right
  edge in by 4 (it was 8).
- **BOTH TRAY FLYOUTS ARE ONE FILE: `userland/wm/tray_slider_popup.c`.**
  A tray flyout that is a slider over a registered setting -- a panel
  anchored above its tray item, a `uui_scale` with an icon cell left and
  a "NN%" caption right, the debounced write, the drag, the wheel, and
  the overlay row's open/close/damage -- is written once, and
  `volume_popup.c` and `brightness_popup.c` sit on it. They were the
  same file twice: the same 250 ms debounce, the same `update_press`
  drag, the same wheel scoping, the same geometry-is-the-one-answer
  rule, each fixed separately or not at all. Two things to know. **The
  OWNER keeps what is only its own** -- volume's mute toggle and device
  rows, brightness's `unavailable` sentence -- below the slider row,
  starting at the geometry's `below_y`, and it keeps its own
  `*_geometry()`. And **the shared half owes the write, not the
  owner**: `tray_slider_set_level()` takes a `commit_now` flag for the
  paths that must not wait out the debounce (a release, a mute).
- **THE TRAY HAS A VOLUME FLYOUT, AND THE PANEL OWNS IT TOO --
  `userland/wm/volume_popup.c`.** A speaker icon left of the clock opens
  a panel with a level slider, a mute toggle and the output devices;
  the WHEEL over the icon moves the level by 5 without opening
  anything. That is Windows 11's flyout, Plasma's audio applet and
  GNOME's quick settings, wheel included. The slider row, the debounce
  and the drag are `tray_slider_popup.c`'s, shared with the brightness
  flyout; what follows is what this file still owns or still decides.
  Six things to know.
  **It knows nothing about audio**: it reads and writes `system.volume`
  and `system.audio_device` over `SYS_SETTING`, and its device rows ARE
  that setting's CHOICE list -- so a card plugged in after boot appears
  as a row with no code here learning what a card is. **The write is
  DEBOUNCED** (~250 ms after the last movement, and immediately on
  release), because a setting write validates, applies AND persists --
  one `/etc` write per call, so a dragged slider without it is a
  hundred of them. **MUTE IS A LEVEL OF ZERO**, not a second piece of
  state: the registry holds one number, and the pre-mute level is
  remembered in the popup, so unmuting works within a session and a
  reboot while muted comes back at zero. **The wheel is SCOPED to the
  tray item and the open panel** -- taken anywhere else it would eat
  every scroll in every app, which is the half `volume_test.py` asserts
  negatively. **The slider is dragged through
  `volume_update_press()`**, called every tick like the dialogs', since
  a control that only sees the button-down edge cannot follow the
  pointer. And **`volume_geometry()` is the one answer** drawing,
  hit-testing and `gui volume --json` all ask.
- **THE SCREEN CAN CHANGE MODE AT RUNTIME, `screen_set_mode()` IS THE
  ONE PLACE THAT DOES IT, AND THE GRANT NEVER SHRINKS.** `config set
  resolution 1600x900`, or the Resolution dropdown under Display in
  System Settings, runs `kernel/core/screen.c` in the one safe order:
  the driver's `set_mode`, write-combining re-applied, `gfx_remode()`,
  `vga_reflow()`, the pointer's bounds, `win_surface_remode()`, and last
  a `WIN_EV_SCREEN` broadcast. Five things to know. **Every QEMU adapter
  advertises `DISPLAY_CAP_MODESET` now** -- bochs ADOPTS GRUB's mode
  instead of declining (so `-vga std` has a modesetting driver on every
  boot), vmsvga and virtio-gpu set modes as they always could -- and the
  Intel driver lists the panel's native mode and every ladder entry
  smaller than it, shown through the panel fitter with the native timing
  kept (the buffer never moves: a smaller mode is the same stride
  scanned w x h).
  **The setting is an ENUM whose choices are the driver's mode list**
  (`display_ladder_mode()` filtered by what the adapter accepts), so
  nothing offers a mode the adapter will refuse; an unlisted one is
  refused before anything is touched, and every driver's refusal leaves
  the old mode running. **The compositor is a process that may be
  mid-blit when the mode changes**, so `win_surface.c` keeps a
  HIGH-WATER MARK: every framebuffer slot stays mapped up to the largest
  extent it ever had, the tail past the real buffers pointing at one
  writable scratch frame -- the same rule as `comp_span` for a client
  window. A shrink therefore never opens a hole, and a stale blit is
  garbage the next unconditional frame repaints. **The compositor
  re-maps on `WIN_EV_SCREEN`** (`ugfx_screen_remode()`, then
  `wm_screen_changed()`: the icon grid, the pointer, every window's
  position, maximized windows re-proposed at the new size, the overlays
  closed, then `wm_render_reset()`); clients need nothing, their windows
  are resized through `WIN_EV_RESIZE` as ever. And **a stored resolution
  is applied at the config stage of boot**, after the console and before
  init starts the desktop, so an installed machine no longer needs
  `video=` on the GRUB line; one the driver refuses is logged and the
  boot mode kept. `modeset_test.py` measures the change at the DEVICE:
  a QMP screendump's own size must match what the desktop believes.
- **A MODE SMALLER THAN THE PANEL IS PLACED BY `system.scaling`, A
  SETTING ON EVERY MACHINE, AND THE FITTER'S SIZE REGISTER IS THE ARMING
  WRITE.** `aspect` (the largest same-shape rectangle, centred; the
  default, i915's eDP default), `full` (stretched) or `center`
  (unscaled), a dropdown under Display > Screen beside Resolution, with
  the registry's sentence on a display without `DISPLAY_CAP_SCALING`.
  The policy lives in the display layer (`display_scaling()`), so a
  driver's `set_mode` reads it and the boot-time resolution is placed
  the way the user chose; `set_scaling` re-places the current mode.
  `intel_display_fit_window()` is the pure window maths, exact on the
  limiting axis and KTESTed. **The trap that cost three flashes**: the
  Intel fitter's `PF_WIN_SZ` is the arming write, DSPSURF-style, so the
  order is `PF_CTL`, `PF_WIN_POS`, `PF_WIN_SZ` (i915's `ilk_pfit_enable`)
  -- written control-last, every register read back as asked and the
  source sat unscaled at the top-left, position included. **The second
  trap, a skewed screen**: the window must EQUAL the pipe's active area,
  `panel = 2 * position + size` on each axis (i915's
  `intel_pch_pfit_check_dst_window`), so the size rounds UP to even and
  the position is exactly half the border, odd if it must be -- rounding
  both to even left 1366x768 centred two pixels short (2*276+1366) and
  the fitter walked every line out of step, while 1600x900 centred
  satisfied the rule by luck. A KTEST holds the invariant over every
  ladder mode and policy.
- **A PRESENT FLIPS ON A DISPLAY WITH THREE SCANOUTS, THE FLIP NEVER
  WAITS, AND THE COMPOSITOR REPAINTS BY BUFFER AGE.** `DISPLAY_CAP_FLIP`
  (`display.h`) means a driver has `scanout_count` buffers of the
  surface's geometry, can ask for any of them to be scanned from the
  next vblank (`flip`), and can say which is being scanned right now
  (`scanout_live`); the Intel driver (`DSPSURF`/`DSPSURFLIVE`) and
  virtio-gpu (`SET_SCANOUT`) both do. `WIN_REQ_FB_MAP` maps EVERY
  scanout, buffer i at `WIN_FB_VADDR + i * WIN_FB_BUFFER_STRIDE`, and
  returns the count in `mods` and the BACK index in `window`;
  `WIN_REQ_FB_PRESENT` flips to the buffer the compositor drew and
  returns the next back index. Five things to know. **THREE BUFFERS,
  BECAUSE THE FLIP NEVER WAITS** (mailbox mode, what DWM does): a flip
  asked for before the previous one landed simply replaces it, so a
  present runs with no wait inside a syscall -- and the buffer handed
  back is the one that is neither the live scanout nor the one just
  asked for, which only a third buffer can always be. Two buffers with
  no wait hand back the LIVE buffer, and drawing into it tore worse
  than no flip at all (measured by eye on the laptop, the day it was
  built that way). **The buffer handed back is SEVERAL FRAMES OLD**, so
  `ugfx_screen_present()` numbers its presents, remembers when each
  buffer was last painted, and copies the union of every frame's damage
  since -- Wayland's `buffer_age` -- from a small ring, and the whole
  screen into a buffer never painted or older than the ring. **A driver
  with fewer than three scanouts is treated as having one**;
  `win_surface.c` refuses the two-buffer case for the reason above.
  **Revoking the grant flips back to buffer 0**, the one `gfx.c` draws
  the console into; the console knows nothing about flips. And **`gui
  fb --json` reports buffers, the back index and how many presents
  flipped** -- a display reporting three buffers whose index never
  changes is a flip that is not happening, which no screenshot can
  see. On `-vga std` and vmsvga the count is 1 and nothing changes.
- **A TRAY ITEM'S VISIBILITY IS A SETTING, `desktop.tray_<item>` =
  `auto` | `always` | `never`, AND `auto` ASKS THE HARDWARE.**
  `tray_want_shown(key, hardware_present)` in `userland/wm/wm_tray.c`
  is the one place that policy is resolved, so a new tray item needs no
  new code for it -- that is Windows 11's Taskbar > System tray icons
  and Plasma's per-item panel control. Three things bite.
  **HIDING KEEPS THE SLOT** (`tray_set_hidden()`, never
  `tray_unregister()`): a slot index IS the item's left-to-right
  position, so freeing it lets an item that comes back reorder its
  neighbours. **RESOLVE IT ON THE SETTINGS GENERATION, NEVER PER
  FRAME** -- `tray_want_shown()` is a `sys_setting()` GET whose kernel
  getter reads `/etc/desktop.conf`, so a per-frame call is a whole-file
  disk read at frame rate; hang it off the owner's existing generation
  poll. And **A GUI TOOL THAT DRIVES A HARDWARE-GATED ITEM MUST PIN IT
  TO `always`** and put the setting back afterwards: every tool here
  runs under QEMU, where no adapter has a backlight, so on `auto` the
  sun is not in the tray at all -- and a setting left behind changes the
  taskbar geometry every later tool measures.
- **THE TRAY HAS A NETWORK ITEM, IT READS `QUERY_NETDEV`, AND IT
  WRITES NOTHING -- `userland/wm/network_popup.c`.** A state-shaped
  icon left of the speaker opens a read-only panel: interface,
  address, netmask, gateway, link. Windows 11, GNOME and Plasma all
  keep the address one CLICK away rather than in the strip, and the
  panel width is the taskbar's. Four things to know.
  **`link_known` IS THREE-VALUED AND A DRIVER THAT CANNOT ANSWER IS NOT
  ONE REPORTING "DOWN"** (`abi/query_abi.h`) -- the e1000 in a default
  QEMU guest is exactly that case, so "connected" is decided by the
  ADDRESS and never by the link flag. **DHCP STATE IS NOT READABLE**:
  `/bin/netd` keeps its lease in its own memory and publishes none of
  it, so the panel distinguishes only an address, a link-local one
  (the 169.254/16 prefix `udhcp.c` falls back to) and none -- it must
  not claim "leased". **THE DEVICE IS POLLED ON A CADENCE, THE
  VISIBILITY ON THE SETTINGS GENERATION**, and the two cannot share a
  clock: there is no netdev generation in the ABI (the read is a
  memcpy with no I/O, so a 1 Hz read is affordable), while
  `tray_want_shown()` reads `/etc/desktop.conf` and must not. And
  **STATE IS CARRIED BY SHAPE, NEVER COLOUR** -- a tray icon is blitted
  TINTED to the panel's ink, so a red "disconnected" does not exist;
  there is one icon file per state.
- **THE TRAY SAYS WHEN SOMEBODY IS ON THIS MACHINE OVER THE NETWORK, AND
  THE KERNEL DERIVES THAT -- `userland/wm/remote_popup.c`.**
  `desktop.tray_remote` = `auto` | `always` | `never`, where `auto` means
  "while a remote session is open": krfb's shape, and the reason the item
  exists at all is that the owner must never have to guess whether
  somebody else is acting on their machine. The flyout lists the last ten
  records from `QUERY_REMOTELOG` and can neither start nor stop a
  session.

  **A SESSION IS REMOTE WHEN THE PROCESS THAT CREATED IT WAS SERVING A
  CONNECTION**, asked at `scheduler_make_session_leader()` -- the one
  place a session is born -- and asked of EVERY descriptor, not fd 0:
  telnetd dup2s a pty onto 0/1/2 around the spawn and keeps its socket
  elsewhere. The peer travels with the session, so a command, a spawn
  and a transfer all carry the address without anybody passing it down.
  **`SYS_REMOTE_LOG` is for the two things the kernel cannot see** -- the
  command line as typed (a builtin spawns nothing) and which file a
  transfer moved -- and the kernel decides whether to keep the record,
  stamping the SESSION's peer rather than the caller's claim.
- **THE TRAY HAS A BRIGHTNESS FLYOUT, HIDDEN BY DEFAULT WHERE THERE IS
  NO BACKLIGHT -- `userland/wm/brightness_popup.c`.** A sun icon left
  of the speaker opens one slider; the wheel over the icon steps it by
  5. It is the volume flyout's twin -- literally, since both are
  `tray_slider_popup.c` now: the same debounced write, the same
  `update_press` drag, the same one geometry function behind drawing,
  hit-testing and `gui brightness --json` -- and it talks to nothing
  but `system.brightness` over `SYS_SETTING`. Three things to know.
  **It is REGISTERED whether or not the display has a backlight and
  then hidden in its slot**, so `desktop.tray_brightness = always`
  brings it back on a machine that has none -- which is how
  `brightness_test.py` covers the flyout in QEMU at all. When it IS
  shown without a backlight the panel shows the registry's
  `unavailable` sentence over a disabled track rather than inventing
  its own reason, which is what `setting_abi.h` asks of every client.
  **When unavailable it WRITES NOTHING** -- the slider and the wheel leave the
  level alone, and the test asserts that against `config get`, since
  a build that skipped the check would push a value the registry
  refuses and show a level the hardware never took. **The wheel stays
  consumed over the icon even then**: the notch was aimed at this
  control, and letting it fall through to a window would scroll
  something the user was not looking at.
- **THE CLOCK IS ALWAYS THE RIGHTMOST TRAY ITEM, whatever slot it
  holds.** It takes slot 0 and the strip is walked from the highest slot
  down, so before this the first item registered after it landed between
  the clock and the screen edge -- the one position no desktop puts a
  tray icon in. **A tray item may be an ICON now**
  (`tray_register_icon()`), sized from `taskbar_icon_size()` and drawn
  from the same right-to-left walk; an icon with no file draws NOTHING
  rather than the letter tile an app icon falls back to, which would
  read as a control the panel invented. `tray_item_rect()` gives any
  item's box from that same walk, so a click cannot be told a different
  position from the one it was drawn at.
- **`uui_menubar_key()` ALWAYS WRITES `*out_code`, AND -1 IS THE ONLY
  "NOTHING COMMITTED" VALUE** -- the same one `uui_menubar_release()`
  returns, so the mouse path and the keyboard path mean the same thing by
  the same value. A caller never initialises it and never picks a
  sentinel. It used to be "left alone otherwise", and six apps then
  chose two conventions: `mines` and `terminal` used -1, while `files`,
  `imgview`, `player` and `notepad` used 0 -- which is only safe while no
  command has id 0, and **all four had numbered their command enums from
  1** with nothing recording that they had to. A command numbered 0 would
  have worked with the mouse and silently done nothing from the keyboard.
- **SHARE THE PART THAT DOES NOT DIFFER, NOT A LOOP WHOSE DIFFERENCES
  BECOME CALLBACKS.** `userland/lib/uline.h` states it: the line editor's
  PAINT is shared by `tosh` and `dash`, and the read loop deliberately is
  not, because each front end genuinely differs there and sharing it
  would cost a callback per difference. Measured 2026-09-17 with
  `tools/dup_scan.py`, two candidates met the "second real caller" bar on
  the letter and were declined on this rule: `lspci`/`lsusb` share 21
  lines of bounded-line file reader whose only variable is one
  `handle_line()` callback, and `imgview`/`player` share ~46 lines of the
  same media-browser shape while differing in decode-versus-play. Total
  duplication is small enough that neither pays: 71 duplicated lines
  across 27 GUI files, 75 across 88 `/bin` programs.
- **A TRAY ITEM HAS NO HOVER STATE, AND LIGHTS UP ONLY WHILE IT IS
  HELD.** The item's box gets a rounded fill in `uui_state_bg(panel,
  UUI_STATE_PRESSED)` for exactly as long as the left button is down on
  it: nothing on hover, and nothing latched afterwards -- a popup the
  press opened does NOT keep its item lit. That is the macOS menu-bar
  rule, not Windows 11's or Breeze's, both of which also highlight on
  hover; `docs/decisions.md` has why. Three mechanical things.
  `tray_update_press()` runs every wm_run() tick, not on the button-down
  edge, because the pill has to go out on a release or a drag off the
  item and neither is an edge. It asks the SAME right-to-left walk that
  draws the strip (`tray_walk()`), so a press cannot light an item a
  click would miss. And a TEXT item's glyph cells are painted opaque, so
  the pressed clock's `ugfx_draw_string_clipped()` must be given the
  PILL's colour as its background, or it punches the strip's colour back
  through its own fill.
- **THE ROUNDED RECT IS `uui_fill_round_rect()`, ONE RASTERISER**
  (`userland/ui/uui_primitives.h`), and **its arcs BLEND against what is
  already on the surface** -- so the caller must have painted under the
  rect in this same pass, or the corner darkens a little every frame.
  `UUI_CAPSULE` is half the short axis, and any larger radius clamps to
  it. It lived inside `uui_scrollbar.c` until the tray's pressed pill
  became its second real caller; `UUI_SB_CAPSULE` is now an alias.
- **ugfx's FILLS ARE ANTI-ALIASED, AND A MESH MUST NOT USE THEM.**
  `ugfx_fill_circle()`, `_ellipse()` and `_polygon()` blend their edges
  by coverage for every caller (`ugfx_fill.c`, sixteen sub-scanlines,
  checked against Pillow by `tools/ugfx_fill_hostcheck.py`); the
  footprint is the aliased fill's, so swapping one for the other moves
  nothing but the edge. Faces that share an edge -- Shapes, the teapot --
  fill through `uui_canvas`/`geom_fill_*`, whose pixel-centre rule meets
  exactly; two anti-aliased faces leave a seam. Do not hand-roll a
  coverage loop in a widget: the transport's first version did, and the
  library is now the one rasteriser.

- **A MEDIA TRANSPORT IS `uui_transport`** -- previous, play/pause,
  next, clicked like buttons (armed on press, committed on release over
  the same part) and taken with `uui_transport_take()`. Its parts are
  REPORTED (`<name>.prev/.play/.next`), so a test clicks what was laid
  out. The Audio Player and the Image Viewer's slideshow pill both use
  it; the pill's background is painted in `on_draw` so the widget lands
  on top of it.

- **A DATE OR A NUMBER SHOWN TO A PERSON GOES THROUGH `lib/udate.h` OR
  `lib/unum.h`, never a hand-written `%04u-%02u` or `%u.%u`.**
  `udate_format()` writes the LC_TIME locale's spelling (and ISO in "C",
  so a program that never opted in prints what it always did);
  `unum_localize()` rewrites a C-formatted number with the LC_NUMERIC
  locale's decimal mark, and groups its digits only when asked
  (`UNUM_GROUP`) -- a size in a column is never grouped. **Only the FIRST
  number in the string is touched, so never pass it an IPv4 address.**
  A column holding a date is sized for the widest region's ("12/31/2026
  12:59 PM"), not for ISO's. GUI apps are already in the system's locale:
  `uapp` calls `setlocale(LC_ALL, "")` at open and again on the
  forwarded `WIN_EV_SETTING`.

- **A SYSTEM SETTINGS PAGE'S EXTRAS ARE FOUND BY THE SETTING IT CARRIES**
  -- the screensaver's Test, Date & time's live clock and Change...
  (`set_clock.c`), Region & formats' preview -- never by the page's
  title, which `/etc/settings.d` can rename. `settings <qualified name>`
  opens on the page carrying that setting, for the same reason.
  **Setting the clock is an ACTION, not a staged setting**: Change...
  writes at once and is unavailable while `system.ntp` is on, as
  `timedatectl set-time` refuses.

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

- **THE CALENDAR'S WEEK -- ITS FIRST DAY AND ITS NUMBERS -- IS THE
  LOCALE'S.** `calendar_poll_config()` reads
  `nl_langinfo(_NL_TIME_FIRST_WEEKDAY)` and `_TOY_WEEK_NUMBERS`, which
  fold the region and its `locale.week_start`/`locale.week_numbers`
  overrides together (`userland/libc/locale.c`); it never reads a
  setting itself. The WM is not a uapp, so **`wm_locale_sync()`
  (`wm_tray.c`) is what re-runs `tzset()` and `setlocale()`** when the
  settings generation moves -- the tray clock calls it each second. A
  week number belongs to the row's MONDAY (its second column on a Sunday
  week), ISO 8601's rule. **A tool that changes either setting must set
  it back.**

- **THE POPUP'S CLOCK CARD TICKS WITH THE TRAY CLOCK, AND ITS LINK OPENS
  SETTINGS BY A SETTING'S NAME.** The card shows seconds, so
  `tray_update_clock()` damages the open popup each second -- which is
  also why `calendar_test.py` compares the panel BELOW the card. "Date &
  time settings..." spawns `settings system.timezone`: System Settings
  opens on whichever page carries that setting, so a renamed page keeps
  the link.

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

- **AN ICON ON A PANEL IS SYMBOLIC: IT TAKES THE PANEL'S INK, NOT ITS
  OWN.** The tray blits through `ugfx_blit_tinted()` -- the art's alpha
  as coverage, the panel's `fg` as the colour -- which is what GTK's
  `-symbolic` icons and Windows' MDL2 glyphs are for. The reason is not
  tidiness: icon art carries ONE ink, and this desktop has a near-white
  toolbar and a near-black taskbar, so the same speaker glyph that reads
  correctly in a popup was toolbar ink on the taskbar at **a sixth of
  the contrast of the clock beside it** (measured: 93 against 596). One
  master serves both, tinted in one place and native in the other, which
  recolouring the art would not have achieved. Every tray item today is
  the shell's own indicator; an app-registered icon would have to say it
  is NOT symbolic, since a colourful logo tinted flat is worse than a
  dark one. **A tray icon is also sized by `tray_icon_size()`, not by
  `taskbar_icon_size()`** -- a tray icon IS the item, where a taskbar
  button's sits beside a label inside the button's padding, so the two
  want different sizes off the same font-derived height.

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
- **A CHARACTER WITH Ctrl OR Alt HELD IS A SHORTCUT, NEVER TEXT; A GAME
  READS KEYS BY POSITION.** A window gets Ctrl+1 as `'1'` with
  `KEY_MOD_CTRL` and Alt+B as `'b'` with `KEY_MOD_ALT`
  (`api/keyboard.h`, "Ctrl and Alt"); Ctrl+letter is still its control
  code. uapp routes such a key past the widgets straight to `on_key`, a
  text widget declines it, and **an app that hands keys to an editor
  itself must check `uui_key_is_shortcut()` first** -- Notepad and the
  File Manager's rename do, or Ctrl+1 types a 1. A TERMINAL wants the
  old bytes: `tty_input()` and the GUI Terminal add the ESC prefix and
  drop Ctrl-with-a-character, and nothing else may. **A game sets
  `uapp_desc.on_phys_key`** for evdev keycodes (`abi/input_keys.h`),
  both edges, no repeats, alongside the translated keys; it gets no
  releases for keys held when focus leaves, so `on_focus(0)` releases
  them. `gui key` injects TRANSLATED keys only -- drive a physical-key
  client through QMP. See `docs/decisions/gui.md`.
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
  byte for byte; `userland/backends/doom/dg_toyos.c` is the five `DG_*` functions
  and is ours. It is an ordinary `uapp` client -- window, chrome,
  taskbar button, icon, single-instance -- with no Doom-shaped special
  case anywhere in the WM or the kernel. Five things to know.
  **GPL-2 in an MIT repo**: an aggregation, and `EXTRA_OBJS_doom` links
  it into one binary so that nothing else can depend on it. **The
  vendored tree compiles with warnings OFF and the frame-size warning
  ON** -- the first are noise nobody is allowed to act on, the second is
  the Stack Clash guarantee. **`api/keyboard.h` and `doomkeys.h` cannot
  share a translation unit** (both define `KEY_F2`/`F3`/`F4`/`F10` with
  different values), so they meet through `dg_toyos.h`, and keys cross
  it as evdev KEYCODES (`abi/input_keys.h`), which neither defines. **The IWAD is
  not in the repository** -- `tools/fetch_wad.py`, and the app says so in
  its own window when there is none, rather than showing black. And
  **what the port actually needed was measured**: key releases were
  required, while the image ceiling (0.72 MiB of 1) and the growable
  stack (it fits in four pages) were not. See `docs/decisions.md`.
- **DOOM'S SOUND IS THE REST OF THE PORT, NOT A REWRITE.** doomgeneric
  IS Chocolate Doom with the platform layer and sound removed -- 24
  files carry Simon Howard's copyright, and `sound_module_t`,
  `music_module_t` and the GENMIDI handling are all Chocolate Doom's
  design. So the music files were taken back from
  `chocolate-doom-2.1.0`, a tag chosen by MEASUREMENT rather than guess:
  there `memio.c` is byte-identical and `i_sound.h` differs only by the
  declarations doomgeneric appended, so the module structs match and
  `i_oplmusic.c` compiles against the headers already present. Later
  tags drift.

  **`FEATURE_SOUND` IS DEFINED ON THE COMPILER LINE, never in the
  vendored `doomfeatures.h`**, and the three things that flag then
  reaches for are answered from `userland/backends/doom/`: an empty
  `compat/SDL_mixer.h` (included and never used), a `compat/SDL.h` that
  maps byte swaps and a mutex/condition pair onto `__builtin_bswap` and
  pthreads, and `opl_toyos.c` exporting `opl_sdl_driver` because
  `opl.c`'s driver list names that symbol unconditionally. Not one
  vendored byte changed. `-D__DJGPP__` would have suppressed two of the
  three and was refused -- it changes real behaviour in five other
  files.

  **THE OPL RENDER THREAD IS LOAD-BEARING, NOT AN OPTIMISATION.**
  `opl.c`'s `InitDriver` calls `OPL_Detect()`, which calls
  `OPL_Delay()`, which blocks on a callback that only fires from the
  render path. SDL's audio thread was already running by then; here
  nothing is, so without a thread turning the clock `I_InitMusic`
  deadlocks and the window sticks on the pre-WAD title with sound
  reported as up. It also keeps the tempo off Doom's frame rate: **the
  OPL clock is SAMPLES PRODUCED, never wall time.**

  **Effects map onto `usnd` voices and the backend does no mixing** --
  `s_sound.c` has already done the attenuation and the channel
  stealing and hands `I_StartSound` a volume and a separation, so
  `dg_sound.c` turns that pair into a stereo gain and stops. And the
  precache loop must use `W_CheckNumForName`: `S_Init` precaches over
  the whole of `S_sfx[]` including a nameless dummy entry, and
  `W_GetNumForName` calls `I_Error` on a miss, which kills the game
  before its window opens.

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
  (the WM's own, deleted 2026-09-14), Notepad's Open/Save dialog, and Image
  Viewer's sidebar -- all doing `sys_listdir()`, `dirsort()`, a
  synthetic `..` row and descend-on-activate. No real toolkit ships four
  (Windows has one `SysListView32`, Qt one `QFileSystemModel`, GTK one
  `GtkFileChooser`). **Image Viewer and Notepad are converted; the WM's
  file picker is NOT** (`docs/roadmap.md` carries it) -- and it is the
  awkward one, since it is the compositor's rather than an app's.
  Seven things to know:
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
  - **A thumbnail is a LOOKUP the app answers, never a decode the
    widget does.** `uui_fileview_set_thumb()`'s callback returns a
    ready image for a cell or NULL for the generic icon; the File
    Manager's cache decodes LAZILY on its tick, a couple of files per
    pass (`thumb_tick()`), keyed by path + mtime + size so a rewritten
    file re-decodes. Decoding in the draw path would freeze the window
    for as many full JPEG decodes as the folder has photos, which is
    why every desktop thumbnails asynchronously (see
    `docs/decisions.md`). Sniff before loading: `uimg_probe()` on 16
    header bytes, because most files are not images and
    `uimg_load()` reads the whole file.
  - **`uui_fileview_set_active_mark()` draws the active-pane border
    from INSIDE the widget's own draw** -- the File Manager drew it
    from `on_draw_over`, which runs after the router's overlay pass, so
    the accent outline painted straight across an open menu.
    `on_draw_over` is for what belongs ABOVE popups (the modal);
    anything that must sit under them has to be drawn in the widget
    pass. Dolphin's split view marks its active pane inside the view
    for the same reason.
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
- **THE FILE MANAGER IS A COMMANDER THAT OPENS AS AN EXPLORER**
  (`userland/gui/apps/files.c` plus `userland/fm/`, `/bin/wm/apps/files`).
  It was built as a two-pane commander for a real reason: Explorer's two
  primary verbs are copy/paste and drag-onto-a-window, and this system
  had neither a clipboard nor drag-and-drop. Norton Commander's answer,
  kept by Midnight Commander, Total Commander and Krusader for forty
  years, needs neither: with two directories on screen the source is the
  active pane and the destination is the other one, so nothing is
  carried and nothing needs a carrier.

  **BOTH OF THOSE ARRIVED, so the argument for two panes as the DEFAULT
  went with them** -- there is a system clipboard (`lib/uclip.h`) and
  drag-and-drop within and between windows. The commander layout is
  still here, still the better tool for moving things, and one toolbar
  click away; what changed (2026-09-14) is that a first run opens with
  ONE pane and the folder tree, which is Explorer's shape and Dolphin's
  and Nautilus's. Two panes reads as cluttered to somebody who has not
  asked for it, and the tree is how most people navigate.

  **`g_single`/`g_tree_on` DEFAULT TO 1 AND ARE PERSISTED**, so the
  compiled-in value decides a machine's first run only; after that
  `/etc/files.conf` wins, which is what makes changing it safe. Ten
  things to know, the first being where
  they are: **the app is SIX translation units** -- `files.c` is the
  menus, the commands, input and `main()`; `userland/fm/` holds the view,
  the job queue, the tree, the thumbnails and the modal, sharing state
  directly through `fm_internal.h`. That directory is outside
  `USERLAND_PROGRAM_DIRS` for the reason `userland/wm/` is: those turn
  every `.c` into its own ELF, which is right for a program and wrong for
  one program's parts. The units are LISTED in `EXTRA_OBJS_files`, not
  wildcarded, so a stray `.c` fails to link rather than being absorbed.
  Then:
  - **File operations run IN PROCESS, over `userland/lib/ufileop.h`**,
    which owns the copy loop and the tree walk and takes POLICY as
    callbacks. They were spawned `/bin/cp` and `/bin/rm` children; the
    three shell programs are front ends over the same engine now, which
    is what keeps "one implementation, testable as text at a prompt"
    true (`tools/fileop_test.py`). What the change bought is what a
    child could never do: byte-level progress, a cancel, and asking
    anything at all when a destination exists. What it cost is that a
    bug in the copy loop faults the window instead of one child --
    stated rather than glossed. **The state is the CALLER'S** (`struct
    ufileop`, ~30 KB, file scope), the `ttf.h` arrangement, because a
    worker thread and the main thread must not share scratch.
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
  - **THE FIVE VERBS ARE ON THE TOOLBAR, and the bottom key row is
    gone.** Copy/Move/New folder/Rename/Delete were a row of buttons
    across the bottom -- Norton's function-key bar, which every
    commander since has kept -- and they moved up beside Up/Refresh
    because the same five commands were already in the File menu and on
    F5-F8, so the row was a THIRD copy costing a whole row of pane
    height. **The keys are untouched** and the status bar still names
    them, which is the half of the commander habit worth keeping; what
    went is the strip, not the bindings.
  - **A SECONDARY CLICK INSIDE A PANE OPENS A CONTEXT MENU**, and it
    SELECTS the row it was pointed at first -- Explorer's and Dolphin's
    rule, and here it is a safety property: a menu acting on a row other
    than the one under the pointer deletes the wrong file
    (`uui_fileview_select_at()` exists for exactly this, and never
    activates, so a second right-click cannot count as a double click).
    The menu is a second `uui_menubar` with no bar of its own, opened
    with `uui_menubar_open_at()`; a click on the chrome opens nothing.
  - **PROPERTIES IS A PROCESS, `/bin/wm/apps/properties`**, spawned with
    the path -- Explorer's and Dolphin's arrangement, and not a modal in
    this window for three reasons: a folder's total size is a RECURSIVE
    WALK no event loop should be doing, a Properties window you can
    leave open beside the listing is more useful than one that blocks
    it, and anything that can name a path can open one. It reports name,
    type (`"TXT file"`, Windows' wording, derived from the extension --
    there is no magic-number sniffing here), location, size, what opens
    it, both timestamps and the inode; a FOLDER gets a recursive count
    instead, walked a few directories per tick so the numbers climb
    while you watch. **The queue is fixed and a full one is REPORTED**
    ("at least 4.2 MB"), because a floor presented as a total is a wrong
    answer wearing a right answer's clothes. It is NOT single-instance:
    two files have two sets of properties, and comparing them is why you
    open the second.
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
- **WHAT OPENS A FILE TYPE IS DECLARED BY THE APP THAT OPENS IT --
  AND THE USER'S CHOICE IN `/etc/mimeapps.conf` OUTRANKS IT.**
  `Handles=.txt .md .conf` on the app's `.desktop` entry declares;
  `/etc/mimeapps.conf` (`.jpg=imgview`, a desktop-entry name, a
  literal `/path`, or `-` for cleared) overrides -- freedesktop's
  declarations-vs-`mimeapps.list` split, resolved by
  `userland/lib/uopen.c` with NO daemon (Linux and KDE run none
  either; a service here would need query IPC the OS has not got, to
  answer a directory scan). `/bin/open` speaks the same resolver from
  a prompt and manages the override file (`-l`/`-s`), so a double
  click and `open x.txt` cannot disagree; a directory opens the File
  Manager; a DANGLING override falls through to the declarations
  rather than making a type unopenable. Extensions match whole and
  case-insensitively INCLUDING the dot (so `.md`
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
  gone; and an OPEN OVERLAY OWNS THE POINTER OUTRIGHT -- it alone gets
  the real point, and everything else is told "nowhere" while it is
  open. That last rule hardened: the overlay used to merely come FIRST,
  with the walk still handing everyone the real point, so icons lit up
  under an open menu and the highlight showed past the popup's edge
  (every desktop's popup grab forbids exactly this). "Nowhere" rather
  than skipping the walk, so a highlight lit before the popup opened
  still clears. See `docs/decisions.md`.
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
  `choice_label`, or `choice_file` when the data IS a file.**
  `/etc/settings.d`'s `Choice.<value>=` lines cover a list a file's
  author can see; they cannot cover one that is COMPUTED (the keyboard
  layouts from a directory) without regenerating the file whenever the
  data changes. `choice_file` is the third form: the registry reads the
  named file's lines, first comma-separated field the value and last the
  label, and knows nothing else about them -- which is how the timezone
  dropdown survived the city database leaving ring 0.
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

## A GLOBAL SHORTCUT IS THE COMPOSITOR'S, AND SUPER IS NOW A MODIFIER.

Bindings are registry settings under the `shortcuts` namespace
(`kernel/lib/shortcut_actions.c` holds the action table, which the
compositor reads too). `userland/wm/wm_shortcut.c` matches every key
before routing, so **no app can shadow one** -- the same rule Alt+F4 has
always followed, and the reason a Wayland client cannot grab a key.

- **`KEY_SUPER` IS NO LONGER IN THE BYTE STREAM.** It is a modifier
  (`KEY_MOD_SUPER`) and reaches the compositor as a TRANSITION. "Super
  alone opens the Start menu" is the compositor's policy and fires on the
  RELEASE, only when nothing was pressed in between.
- **THE COMMAND IN THE ACTION TABLE MUST MATCH `data/wm/applications/`,
  AND NOTHING CHECKS IT AT BUILD TIME.** Two of the first four were
  wrong; the symptom is `pid -1` in the log and no window.
  `tools/shortcut_test.py` compares the lists.
- **Ctrl WITH A NON-LETTER ARRIVES AS THE KEY PLUS `KEY_MOD_CTRL`**
  (it used to be dropped by the driver). Ctrl+letter still folds to a
  control code before anyone sees it, which `keycombo_matches()` undoes
  -- do not re-derive that anywhere else.
- **A CONTROL THAT RECORDS A SHORTCUT MUST HOLD
  `uapp_inhibit_shortcuts()`** while it listens, or the compositor spends
  the very keys it is waiting for. It lapses on focus loss by design.

## AN APP HANDED A FILE MUST SELECT IT, NOT JUST ITS FOLDER.

Image Viewer and Music Player both browse a DIRECTORY and show the
selected row (imgview.c's header says why), so a path argument is used
twice: `k_path_dirname()` picks the folder and the basename picks the
row. Both apps did only the first, and `uui_fileview_set_dir()` selects
row 0 -- so opening a picture from the File Manager opened the
alphabetically first picture in its folder instead. It shipped in two
apps because the second was written from the first.

**The selection goes AFTER `set_dir()`**, which clears whatever was
selected before, and the name survives the `reload()` in `on_open()`
because reload preserves a selection BY NAME. A file that the app's own
filter rejects simply misses and row 0 stands, which is the right
answer for "open this .txt with the Image Viewer".

**And the regression test must LAUNCH WITH AN ARGUMENT.** Every check in
`tools/imgview_test.py` started the app bare and then clicked, so all of
them passed with the bug present -- the argument path had no coverage at
all. Name a file that is NOT first in its directory, or the broken
version passes.

## A MENU BAR IN AN APP WITH ROUTED WIDGETS MUST BE `uui_menubar_ops`.

`uui_menubar` has two interfaces now and picking the wrong one is a
silent bug rather than a style choice.

The HAND-ROUTED one is what Minesweeper uses: the app owns its
`on_press`/`on_motion`/`on_release` and calls `uui_menubar_press()` and
friends from them. That is correct for an app whose other controls are
hand-drawn -- Minesweeper's board is a grid it paints itself -- and it
declares no routed widgets at all. Notepad was the other one and is not
any more; its bars and its file dialog are routed now.

**AN APP THAT DECLARES `uapp_desc.widgets` MUST USE THE OPS TABLE
INSTEAD** -- `tools/check_key_routing.py` fails the build on the other
pair now, after Image Viewer and Player shipped it (a click on a popup
row re-selected the list row beneath and decoded the file again) --
because a popup drops down OVER whatever is below the bar and
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

- **Natural width, capped, packed LEFT; equal shares only once they no
  longer fit.** Konsole's, Windows Terminal's and Chrome's strip -- one
  tab does not stretch across the window. The cost of a width that
  tracks its title is a close box that moves while the pointer travels
  to it, and Chrome's answer is the one taken: **widths are FROZEN while
  the pointer is inside the strip** and recomputed when it leaves or the
  count changes. `UUI_TABS_FREEZE_MAX` caps the table because the widget
  owns no memory; past it the strip simply does not freeze.
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
(`tab.activeBorderTop`). **Resting tabs are filled with the theme's
`tab_rest` colour** (`UTHEME_TAB_REST`), darker than the strip's own
ground and than the control face, so the strip reads as wells with one
tab raised out of them.

Both of those are corrections and the reasons are worth keeping. The
accent sat on the BOTTOM edge, where it is a thin line directly above a
terminal's black page and has the least contrast of anywhere it could
be; and resting tabs were left the strip's own colour, so the selected
one was lifted by TWENTY units out of 255 -- which shipped, and was
reported as hard to tell apart with three tabs open. Fifty is the
current lift (resting tabs took a theme colour of their own on
2026-09-02, at the maintainer's request for darker resting tabs) and
`uterm_test`'s c17 has its floor at twenty-five, above what was
reported, because a check that accepts twenty accepts the bug.

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

## DIAGNOSTICS ARE A NAMED REGISTRY: THE COMPOSITOR IS THE PROVIDER `gui`

The window manager's whole `gui` vocabulary -- `state`, `windows`,
`compositor`, `taskbar`, `probe`, the input verbs -- reaches it from
either ring: `gui <sub>` and `diag gui <sub>` at the serial console,
`/bin/guictl <sub>` from ring 3. Neither parses anything, so a
subcommand added to `userland/wm/wm_debug.c` works everywhere the day it
lands.

**THE ENDPOINT IS A NAME, NOT THE COMPOSITOR.** This was
`WIN_REQ_DEBUG_*` on the window protocol, hardwired to whoever held the
compositor role. It is `SYS_DIAG` over `abi/diag_abi.h` now, and the
kernel holds a table of named providers: a service CLAIMS a name, and
`diag` with no arguments lists what is registered. The compositor is one
provider called `gui`; a service with no window is reachable the same
way. See `docs/decisions.md`.

**A PROVIDER IS WOKEN THROUGH ITS WAKEWORD.** The old path posted a
window event, which only a windowing client has. Every process has a
wakeword (`futex_note_ready`), which is what makes this general. The
compositor still POLLS once per frame rather than waiting on an event,
and that is deliberate: a client presents every frame, the event queue
is 32 deep and sheds the oldest, so a busy client used to flood the
diagnostic out of the queue and the console timed out forever while the
desktop drew perfectly. Polling costs one refused request per frame and
cannot be starved.

Four things to know before touching that path.

**A RING-3 CALLER IS NEVER MADE TO WAIT IN THE KERNEL.** The kernel
posts the command and returns `DIAG_F_PENDING` immediately; the caller
polls with `DIAG_MORE`. The serial console keeps its `sti; hlt` wait
because it is NOT a scheduled process -- `api/scheduler.h` says a
blocking syscall "MUST go through" `scheduler_block_current()` rather
than waiting in place with interrupts on, and "that was tried". Handing
a process the console's wait faults inside `isr_common`, measured, on
the first run.

**THE CHANNEL IS ONE SLOT, AND THE SECOND CALLER IS REFUSED.** The reply
buffer and the pending command are single -- fine with one client, not
with two. A command arriving mid-drain gets `-EBUSY`; the claim is
per-pid and LAPSES after three seconds, because a client killed between
its command and its last chunk would otherwise hold the channel until
reboot.

**`flags` IS AN OUT-PARAMETER EXCEPT ON THE REPLY, AND THAT COST A
FEATURE.** `diag_request()` clears `msg->flags` at entry, which is right
for every path but `DIAG_REPLY`, where the provider is telling the
kernel what its answer IS -- so the incoming flags are read BEFORE the
clear. When that was got wrong, `DIAG_F_UNKNOWN` was dropped and `gui
nosuchthing` printed NOTHING, looking exactly like a command with no
output; `DIAG_F_MORE` went with it, setting "reply ready" on the FIRST
chunk of a multi-chunk answer -- a race that had not been lost only
because the compositor sends its chunks back to back without yielding.

**A NAME NOBODY HOLDS AND A WEDGED PROVIDER ARE DIFFERENT FACTS.** The
console says which, and lists what IS registered, because "no answer"
otherwise reads as "not running" and sends the reader looking in the
wrong place.

## DAMAGING A RECT DOES NOT ASK FOR A FRAME -- SET `redraw_pending` TOO

`wm_damage_rect()` accumulates a rectangle and nothing else. It does not
schedule a repaint, and `wm.c`'s frame loop only calls
`wm_render_frame()` when `redraw_pending` is set -- a plain mouse move
otherwise takes the CURSOR-ONLY path, which moves the sprite and
repaints no scene at all.

So **anything that changes what the screen should show must set
`redraw_pending`**, not merely damage. Damage says WHERE to repaint;
`redraw_pending` says WHETHER to.

**The failure is silent and looks like slowness, not breakage.** A
change that only damages appears on the next frame something ELSE asks
for -- and on an idle desktop that is the tray clock, once a second. The
Start menu's hover highlight sat like that: the cursor moved smoothly on
the cheap path while the highlight arrived up to a second later, which
reads as "laggy", not as "broken", and no settled screenshot can see it
because settling outlasts the clock.

**Verify it by counting frames, never by looking.** `gui state` reports
`scene repaints` -- `wm_render_frame()` calls, excluding the cursor-only
path -- so a test can ask "did that input repaint anything?" without
pixels. `tools/hover_test.py` is the guard, and it asserts BOTH
directions: eight hover changes must repaint, and eight moves over
nothing must not. Without the second half, a WM that repainted on every
move would pass.

**The regression to learn from**: e960ad3 moved every overlay's hover
into one table (`wm_overlay.c`) so that no popup could be forgotten --
and dropped the `redraw_pending = 1` the per-overlay code had carried,
while fixing this exact symptom on the volume flyout. `wm_overlay_hover()`
RETURNS whether the hover changed; the call site ignored it.


**AND THE INVERSE: `redraw_pending` WITHOUT DAMAGE IS A FULL REPAINT
ONLY IN A QUIET FRAME.** The full-screen fallback fires when the damage
box is EMPTY; a client presenting a frame (Shapes, every tick) has
already declared its rect in the same iteration, so the render is
clipped to that rect and a state change elsewhere is never painted --
or never un-painted, which is a close button stuck red on a window
nobody points at. It presented on every flipping display and under
KVM, where presents are frequent, and mostly not on `-vga std` under
TCG, where the race is usually lost. A state change that knows its
rect DAMAGES it (`wm_damage_rect()`) and sets the flag; the title-bar
buttons' hover and press do now (`damage_title_buttons()`), as the
tray and Start menu always did. `tools/hover_test.py`'s animating-
window check is the regression, and it discriminates on `--vga
virtio`. Bare `redraw_pending = 1` sites beside a possible client
present are the audit `docs/roadmap.md` lists.

## THE COMPOSITOR SLEEPS BETWEEN FRAMES, AND TWO THINGS MUST DEFEAT THE WAIT.

`wm.c`'s frame loop no longer calls `sys_yield()` -- that returned
immediately, so an idle desktop ran a full frame's polling every tick.
It waits on `sys_wait_ready()` now, with a deadline of whichever is
nearer: the earliest armed client timer (`wm_client_next_timer_due()`),
or `WM_IDLE_WAIT_MS`.

**`WM_IDLE_WAIT_MS` is the cadence of everything nobody sends an event
for** -- the tray clock, the client pings, the `/etc` generation polls,
the Start-menu click flash, reaping a launched process. 100 ms, because
none of that is animation: the clock renders whole seconds and a
generation poll is one integer compare. Anything that IS animation
arrives as input or as a client timer, and the wait is clamped to it.

**TWO THINGS MUST MAKE THE WAIT ZERO, and the kernel can see neither.**
Injected input from the debug console (`gui click`, `gui drag`) lives in
ring-3 memory, so pushing to it wakes nobody -- and the loop consumes
ONE per iteration on purpose, so a press would block with its release
still queued and every GUI tool would hang. `wm_debug_work_pending()` is
the guard. And a repaint already owed should not wait, which is also
what keeps the first frame prompt.

The corollary for anything added to the loop: **work that must happen on
a cadence is now bounded by that wait, not by the tick.** A new poll
needing to be faster than 100 ms has to say so -- by clamping the
deadline the way the client timers do, not by assuming the loop spins.

**Measured, host CPU over 30 s untouched, three samples each: 0.76 s ->
0.57 s, about a quarter.** The GUEST cannot see it -- `ps` reports the
same CPU seconds and only the STATE moves, `ready` to `block(event)`.
`tools/idle_cpu.py` is the instrument, and only its DIFFERENCES mean
anything.

## `uui_dialog` IS THE MODAL QUESTION, AND IT SWALLOWS EVERY KEY WHILE IT IS UP.

A title, up to six rows of text and up to six buttons, centred in the
bounds the app hands it. It draws through `draw_overlay` and declares
`overlay_active`, so it lands on top of the app's widgets without the
app ordering anything, and `uui_dialog_take_code()` hands back the
button's code once.

Four things to know:

- **Its `key` op returns 1 for EVERY key**, not just the ones it uses.
  Behind a File Manager dialog is a listing where a letter seeks and
  Enter descends, so a keystroke let through is a modal that is not one.
- **It commits on RELEASE, and only on the button the press armed** --
  this GUI's rule for every control, `docs/gui-guidelines.md`.
- **A press ANYWHERE is consumed, including outside the box**, and a
  click outside does NOT dismiss. These ask questions whose default
  answer is not obvious, and a stray click is not an answer.
- **`rows` are caller-owned pointers** and must outlive the dialog being
  up; point them at the app's own buffers.
- **`.margin = 0` MEANS "UNSET", NOT "NONE"** -- `uui_layout_margin()`
  tests `> 0`, so a container asking for zero gets the font-derived
  default anyway. A nested layout gets none because `nested` is set,
  not because anyone wrote 0. **AND A SCROLL VIEW'S CONTENT IS NESTED**:
  it is run directly rather than through the ops table, so it used to
  keep the outermost default and the page inside one paid the margin
  TWICE -- once for the window, once again inside it. Task Manager's
  Overview sat 29 px in where its own tab strip sat 14.
- **A ROW IS NOT A GRID. `UUI_GRID` is** -- uniform cells, `cols`
  across, a partial last row simply leaving cells empty. A column of
  rows sizes each row's height independently and shares each row's
  leftover width among whatever declares `UUI_FILL_W`, so a last row
  holding one tile and a spacer gives that tile its natural width PLUS
  half the leftover -- wider than the cards above it, never the same
  twice, and no spacer needed once it is a real grid.
- **AN ACTIVE OVERLAY DOES NOT YET OWN THE CURSOR, and an app that
  names its own must test for a modal itself.** `uui_router_cursor()`
  falls THROUGH an overlay to the widgets behind it, so the I-beam of a
  text area nobody can click shows over a modal. **So an app that names
  its own cursor must gate it on `uui_dialog_is_open()` itself**, in
  `on_motion` AND where it opens the dialog -- Alt+F4 and the X move no
  pointer, so a motion-only gate leaves the caret until the mouse
  happens to move. Notepad is the worked example. The toolkit fix --
  stop the walk at an overlay, and resolve the app's request per frame
  -- was written, verified in QEMU, and WITHDRAWN when it crash-looped
  `toywm` on bare metal with no mechanism established (`docs/bugs.md`,
  `docs/decisions/gui.md`, and the roadmap item). Read those before
  attempting it again, and delete the per-app gates when it lands.
- **`uui_dialog_take_code()` RETURNS -1 ON EVERY PRESS**, because
  `on_widget` runs for the press as well as the release and the dialog
  parks its code only on the release. A `switch` whose `default` acts --
  clearing state, cancelling, closing -- therefore acts on the press,
  half an event before the answer arrives. Take the code, `break` on a
  negative, and only then switch; the menu bar's `if (code > 0)` is the
  same guard. This cost a real bug in Notepad: the press wiped the
  parked action and Don't Save closed nothing, while Cancel looked
  perfectly fine because "do nothing" is what Cancel means anyway.

Its buttons are equal-width, sized to the widest label and then CLAMPED
so the row fits the box -- the row is right-aligned, so an unclamped
sixth button pushes the FIRST one off the left edge and it draws as a
fragment of its own label.

## A LONG FILE OPERATION RUNS ON A WORKER THREAD, AND ITS QUESTIONS COME BACK AS POSTS.

`userland/fm/fm_jobs.c` drives `lib/ufileop.h` on a `pthread`, because
the alternatives both fail: a loop on the event loop freezes the window
(this file's long-work rule), and a spawned `/bin/cp` cannot report
progress, be cancelled, or be asked anything.

**The worker touches NOTHING in Toykit.** It writes progress into a
block under `g_lock` and calls `uapp_post()`; every widget, every draw
and every decision happens on the main thread. A conflict is posted the
same way and the worker then BLOCKS until the main thread answers.

**The wait is `sys_sleep_ms(30)`, not a condition variable**, because
`pthread_cond_wait` in `userland/libc/pthread.c` spins on `sys_yield()`
-- a worker parked on one burns a core for as long as a person takes to
read a dialog.

**A no-answer sentinel of `-1` is TRUTHY**, so `while (!answer)` falls
through instantly and the copy proceeds while the question is still on
screen. Write `< 0`.

**A test must not count keystrokes to reach a button.** The app reports
`layout dialog <open> <hot>` for exactly that: stepping one arrow at a
time and confirming `hot` each time is the difference between testing
the dialog and testing keystroke delivery -- one dropped arrow otherwise
commits the button beside the intended one, which passes as the wrong
behaviour rather than failing.

## A WIDGET IS NAMED BY ITS ID, NEVER BY ITS POSITION IN THE ARRAY.

`g_widgets[]` is an app's widget list and `uui_item.id` is what names an
entry. An index derived from the array's LENGTH -- `(g_widget_count -
3)` -- is a number a different file has to keep true, which is the shape
CLAUDE.md's "prefer facts that cannot go stale" rule exists to stop.

It shipped in the File Manager: appending `uui_dialog` shifted three
macros at once, so hiding the tree hid the tree SPLITTER, and hiding the
pane splitter hid the CONTEXT MENU. The visible results were a tree
drawn over the menu bar and a right-click that did nothing in
single-pane view -- neither of them anywhere near the array.

**`tools/check_widget_ops.py` fails the build on it now**, chained forms
included, waivable with `widget-ops-ok: <reason>`. The replacement is a
linear `widget_by_id()` over a dozen entries.

## A TEST MUST NOT DERIVE GEOMETRY THE APP ALREADY KNOWS.

A row's origin, the row under the pointer, whether a button is up: the
app has each of these exactly, and a test that recomputes one is
measuring its own arithmetic. The File Manager reports `layout rowy`
(where row 0 starts, past the column header), `layout hoverv` (the
hovered row as a VIEW position, since `hover` is a source row a test
cannot map back), `layout dim` and `layout cancel` for this reason.

**The failure is quiet, which is why it earns a rule.** "Pane top plus
n rows" is off by the header and lands on the neighbouring row -- a
perfectly plausible thing to have clicked, so the test reports the
feature as broken rather than the aim as missed. That cost four full
runs in one session.

Two things follow. **Confirm the aim, don't just take it** --
`DebugConsole.warp_confirmed()` re-warps until the app agrees what is
under the pointer, and `warp_cursor()` alone is not that (it confirms
the POSITION, a different claim). And **poll with `layout_now()`, not
`wait_layout()`, when nothing needs to change**: the block is emitted
only when it CHANGES, so waiting on a predicate after a no-op warp sees
no frame at all and times out.

## A PERSISTED VIEW STATE IS INHERITED BY EVERY LATER RUN.

Anything an app writes to its `/etc/<app>.conf` -- the File Manager's
pane count, tree visibility and view mode -- is state the next boot
starts in, so a test that never sets it only ever exercises one value.
The whole GUI suite was green while right-click was dead in single-pane
view, because no check had ever turned the second pane off.

**Visit each value of every persisted toggle**, and restore what you
changed: a test that leaves the app in single-pane mode hands the next
section a layout it did not expect. That is also why `make clean-disk`
can "fix" a bug -- it resets the config, not the code.


## A WINDOW'S SIZE BELONGS TO ITS BUFFER, AND THE COMPOSITOR ADOPTS IT ON THE PRESENT.

A client owns its pixels, so a resize is a proposal it answers
(`WIN_EV_RESIZE` out, `WIN_REQ_RESIZE` back). The question this rule
settles is WHEN the window changes size, and the obvious answer is
wrong: adopting at the ACK puts the chrome around a buffer with nothing
in it yet, and the compositor paints a freshly zeroed window until the
client repaints. That measured **100-240 ms of solid black** on Image
Viewer under TCG -- two whole frames -- and reads as the window
flashing.

So the size travels with the pixels:

- **A resize rebuilds only the BACK buffer.** The front still holds the
  last finished frame at the size it was drawn at, and the compositor
  keeps showing it for the whole round trip.
- **`WIN_EV_CLIENT_PRESENT` carries the front buffer's own w/h**
  (`WIN_PRESENT_SIZE`), and the WM adopts THERE. It never has to guess
  whether a frame was drawn before or after the resize it proposed.
- **The buffer left at the old size is rebuilt inside the next
  present**, where the pixels it holds have just stopped being needed.
  A failure there means NO FLIP -- the compositor keeps the frame it has
  and the next present retries -- rather than handing the client a
  buffer it would overrun.
- **AND "REBUILT" IS DECIDED BY THE DIMENSIONS, NEVER BY THE LENGTH.**
  A buffer is page-rounded, so a one-pixel resize usually leaves the
  rounded byte count identical -- and Toykit's `buf_ensure()` compared
  lengths, so it read a real resize as "already the right size" and did
  not replace the object. That is still the rule: compare w and h.
  **The consequence it used to have is designed out** -- there was a
  second record of every buffer's size, in the kernel, that the client
  had to keep in step with `WIN_REQ_BUFFER`, and skipping the update
  left a frame drawn at one stride and composited at another (a window
  sheared one pixel per row, permanently). The frame carries its own
  size now, so the only thing that can still shear a window is a present
  that lies about it. `tools/resize_stride_test.py` sweeps sixteen
  one-pixel resizes and asserts the composited size is the drawn one.
- **A SINGLE-BUFFERED window still flashes**, because it has nowhere to
  hide the change. That is the same degradation it already accepts for
  tearing.

This is Wayland's rule (a `wl_buffer` carries its dimensions; a surface
adopts them at commit). X11's server-resizes-then-app-repaints is the
shape that flickers, and is what this used to be.

**A resize is INTERACTIVE, and the pacing is one proposal in flight.**
The window follows the pointer during the drag, as on Windows and in
KDE -- there is no rubber-band outline any more. What keeps that from
drowning a slow app is that the next proposal goes out when the client
ACKS the previous one (`wm_input.c`'s `resize_pump()`), so the client
sets the pace; an unanswered proposal times out after half a second,
because ignoring one is legal and a client that does must not take the
grip with it.

**WHETHER IT IS INTERACTIVE IS A COUNT, NOT A SCREENSHOT.** Injected
input drains as fast as the WM iterates, so a scripted drag is over in
~150 ms whatever step count it asks for and any size polled during it is
luck. `gui state` reports `resizes_asked`; a WM that only asked on
release moves it by exactly one per drag. The other half -- that nothing
goes black -- needs a client slow enough to repaint that the gap is
visible, which is why it is checked in `imgview_test.py` and not in
`uapp_test.py` (winclient repaints instantly, and the check passes there
with the bug present).


## A DRAG'S APPEARANCE IS A SETTING, AND `auto` LEARNS RATHER THAN GUESSES.

`desktop.resize_mode` (`auto` | `live` | `outline`, default `auto`) and
`desktop.move_mode` (`live` | `outline`, default `live`), declared by
their files in `/etc/settings.d`, persisted to `/etc/desktop.conf`,
read by the WM when a drag BEGINS -- one syscall per drag, so there is
no poll and no generation tracking.

Windows has had this switch since XP (`SPI_SETDRAGFULLWINDOWS`, "Show
window contents while dragging") and KDE DELETED its version in Plasma
5, on the grounds that a compositor makes live dragging always
affordable. KWin is right about KWin, whose clients redraw in
milliseconds. It is wrong here, because a resize is a round trip to a
client that may be rescaling a JPEG: Image Viewer measures 200-300 ms to
become a size it was asked for under TCG, against Terminal's 20-30 ms.

**`auto` IS THE THIRD CHOICE NEITHER SYSTEM OFFERS, and it works by
REMEMBERING.** The WM already times each proposal against the PRESENT
that adopts it, so `struct window.resize_lag_ms` is free; a window
measured over 100 ms (Nielsen's "instantaneous" threshold) is outlined
from the first pixel of its NEXT drag. Three things follow:

- **The first drag of a never-measured window is live, and that is
  correct** -- there is nothing to judge it on, and `auto` will not
  guess from the app's name or its size.
- **A mid-drag fallback exists too**, for the long human drag where the
  measurement arrives while the button is still down. One strike and no
  way back within that drag: a window alternating between following the
  pointer and being an outline is worse than either.
- **It self-corrects in both directions**, because even an outlined
  drag measures its one resize on release.

**MOVING HAS NO `auto`, deliberately.** The WM owns a window's position
and moves it with no client involved, so a move cannot fall behind and
a third choice would be one that never happens. The outline is offered
for moving because some people want it, not because anything is slow.

**PACE ON THE PRESENT, NOT THE ACK, AND CLEAR ON ANY PRESENT.** A client
acks a proposal from inside its event handler and draws afterwards, so
the ack times a syscall and the present times what the user waits for --
pacing on the ack sends proposals a slow client will never draw, and
measuring it reports every client as fast. Two traps came out of
getting that right: a proposal for the size the window ALREADY IS
produces a present that changes nothing (the press beginning a drag is
a motion with a zero delta, so it is the first thing a drag asks for),
and a rule that waited for a CHANGED size then waited forever with the
rest of the drag queued behind it. So: never propose the current size,
and let any present clear the in-flight slot while only a size-changing
one feeds the measurement.

**THE COUNTS AND THE NUMBER ARE IN `gui state`** -- `resizes_asked`,
`resize_paint`/`move_paint` (what the last drag actually SHOWED, which
is the only place `auto`'s decision is visible), and `resize_lag_ms`.
A test asserts on those, because a scripted drag is over in ~150 ms and
nothing about a drag can be watched.

## THERE IS AN ON-SCREEN KEYBOARD, IT IS A WM OVERLAY, AND IT ENCODES KEYS THE WAY THE PHYSICAL ONE DOES

`userland/wm/osk.c`: a panel of keycaps above the taskbar, toggled by a
tray item, that types into the focused client. It exists because a
machine can have a working pointer and no usable keyboard, which is not
hypothetical here.

**AN OVERLAY, NOT AN APP, AND THAT IS THE WHOLE DESIGN.** GNOME's OSK is
part of gnome-shell; Windows ships `osk.exe` as an app using `SendInput`
with `WS_EX_NOACTIVATE`; KDE uses maliit over Wayland's
`input-method-v1`. Being an overlay buys two things at once: an overlay
is not a window, so a keycap click cannot take focus from the window
being typed into, and the keystroke is a direct `wm_client_send_key()`
call, so no ring-3 program gains the ability to type into another one --
the hole X11's XTEST leaves open and `virtual-keyboard-v1` was written
to close. The cost is that the compositor contains a keyboard; see
`docs/decisions/gui.md`.

Four things to know before editing it:

- **IT ENCODES KEYS THE WAY `keyboard.c` DOES FOR A WINDOW, and every
  branch fails SILENTLY if it does not.** Ctrl-C is the control code
  `0x03`, not `'c'` with `KEY_MOD_CTRL` -- `api/keyboard.h` tells an app
  not to test that bit for a letter, so the naive version reaches the
  Terminal and does nothing at all. Alt-B is `'b'` with `KEY_MOD_ALT`,
  one keystroke (the Terminal adds the ESC its pty wants), and Ctrl with
  a non-letter is the key with `KEY_MOD_CTRL`.
- **ITS LAYOUT IS A SECOND COPY OF `FALLBACK_US`** in
  `kernel/lib/keyboard_layout.c`, and the two must agree character for
  character. A cap that disagrees types something the physical keyboard
  would not, on this machine only.
- **THE MODIFIERS ARE STICKY**, armed by a click and consumed by the
  next ordinary key, because one pointer cannot hold Ctrl and click C.
  An armed modifier draws in the ACCENT rather than as hovered --
  selection outranks hover.
- **ITS OVERLAY ROW HAS NO `close` OP**, which is what stops another
  popup dismissing it (`wm_overlay.h`): a keyboard has to survive the
  click that puts the caret in the field it is typing into. It is LAST
  in the table, so a menu overlapping it takes the click and paints on
  top.

**Test it through the filesystem, not through pixels.**
`tools/osk_test.py` types `mkdir /<name>` into the Terminal and asks the
shell whether the directory exists -- one assertion covering the keycap
hit-test, `wm_client_send_key()`, the client, the line editor and the
disk. Ink rising in a Notepad window is the "it responds, therefore it
works" trap: a blinking caret moves those pixels too. And **warp the
cursor rather than clicking open-loop** -- the WM accelerates an
injected delta, so `click_at()` alone misses a 42x40 tray item and reads
as a dead control.

**`gui osk` and `gui osk key <cap>` report the panel, the tray item and
any keycap's box**, so a test never derives a cap's position; the caps
are named (`Space`, `Left`, `Enter`) partly for that reason, since a cap
labelled `" "` cannot be passed as a console token.

- **THE TERMINAL HAS A SCROLLBAR, IN A RESERVED GUTTER, AND THE GRID
  NARROWS FOR IT.** Konsole, xterm and GNOME Terminal all reserve rather
  than overlay; an overlay costs no columns and puts an indicator on top
  of the shell's output. `size_changed()` subtracts the bar's width and
  `default_size()` adds it back -- **the two are inverses, and a bar
  counted in only one of them is a window that opens a column narrower
  than it asked for.** The width comes from
  `uui_scrollbar_natural_size()`, never a literal.

  Three things follow. **`sb_view` already counts from the BOTTOM**,
  which is what a vertical `uui_scrollbar`'s offset means
  (`uui_scrollbar.h`), so unlike `uui_listbox` there is no conversion --
  and a conversion added "for symmetry" would put the thumb at the wrong
  end. **The colours are Breeze's DARK pair** (`#31363b` track,
  `#76797c` thumb) rather than the toolkit theme's: this page is the
  ANSI palette on black by definition, and Notepad's near-white bar down
  the side of it would be the brightest thing on the window. And **the
  trough PAGES, the thumb drags, and both had to be written** -- this
  project has shipped four bars that drew and did nothing.

- **A MOTION WITH NO BUTTON HELD IS IGNORED, NOT TREATED AS A RELEASE.**
  The convention above says to test the `buttons` mask on a drag, and it
  is right that a button-up motion must not move anything. ENDING the
  drag on one is a different act and is wrong: the compositor sends
  exactly such a motion immediately after every press -- a leave at
  `(-1, -1)` telling the window it is no longer hovered, because
  `wm_update_content_hover()` suppresses hover the moment anything is
  pressed. A handler that ends its drag there discards every real motion
  that follows, and the symptom is a selection that never grows past
  zero bytes with the press and the release both arriving correctly.
  `on_release` is what ends a drag.

- **A SELECTION IS ANCHORED IN THE BUFFER, NOT ON THE SCREEN.** The
  Terminal's selection points are lines in a VIRTUAL buffer -- scrollback
  then screen, so view row `r` is line `sb_count - sb_view + r` whichever
  half it comes from -- which is what keeps an anchor on the text it was
  put on when the view scrolls during a drag. It is DROPPED when the text
  moves under it (`vt_scroll()`, a screen clear) rather than tracked
  through, because this scrollback is a ring of evicted rows and carries
  no line identity to follow; Konsole keeps it and has one.

  **COPY-ON-SELECT is on**, which is X11's PRIMARY habit and a Konsole
  option. There is one clipboard here, so releasing a drag does overwrite
  what was last copied -- deliberate, because a terminal selection is
  made in order to be pasted essentially always. **`Ctrl+Shift+C`, never
  `Ctrl+C`**: `0x03` is INTR and has to reach the shell.

- **A ROW-COUNT CHANGE SCROLLS THE GRID; IT DOES NOT JUST CLAMP THE
  CURSOR.** Shrinking pushes the rows the cursor would fall past off the
  TOP into scrollback, exactly as if the program had printed them, and
  growing pulls them back out -- xterm's semantics, shared by Konsole and
  VTE, and the property that makes shrink-then-grow a ROUND TRIP.

  Clamping alone is what this did, and it fails in two visible ways:
  shrinking drops the cursor onto text that did not move, so the prompt
  lands in the middle of old output; and growing leaves it stranded
  there with blank rows below, because nothing ever moves it back. The
  second is the one that gets reported -- "resize it smaller then larger
  and the prompt keeps the small window's position".

  **THE WHOLE GRID MOVES BY THE SAME AMOUNT, and that is what keeps the
  program on the other end correct.** A shell records how many rows
  below its prompt the caret sits; a uniform shift preserves that
  offset, so its own repaint still erases the right rows. A reflow that
  moved rows by different amounts would not.

  **The alternate screen is exempt**: it has no scrollback to scroll
  into, and a full-screen program owns every row and redraws them all
  when it hears the size changed.

- **A FULL-SCREEN PROGRAM MUST HANDLE SIGWINCH, AND ASKING ONCE AT
  STARTUP IS NOT ENOUGH.** `upager.c` read its terminal's size before
  its key loop and then blocked in a read forever, so `doc` and `less`
  in a resized window went on drawing the old page at the old width with
  no way to notice -- and a keypress afterwards did not recover it
  either, because the repaint was gated on the scroll position having
  MOVED. A resize handler has to re-ask the size, re-size any buffer
  derived from it, clamp the position to the new bottom, AND defeat that
  cache.

  The handler sets a flag and nothing else (async-signal-safety), the
  action is installed with `sys_sigaction` and NO `SA_RESTART` -- the
  interruption is the message, and `sys_signal()` would restart the read
  and swallow it -- and EINTR from that read is a resize, not the
  terminal going away. Treating it as the latter closes the pager on
  every window drag.

  **AND RE-PAGING IS NOT RE-WRAPPING.** The pager owns the page; it does
  not own the wrapping, which belongs to whoever RENDERED the text at
  whatever width the terminal was when they asked. The signal pair --
  the kernel raising SIGWINCH on the foreground group (d50fa0c) and the
  pager handling it (f42eadf6) -- delivers the resize and stops there,
  which is worth knowing because "we built a signal for that" is the
  natural reason to assume the whole problem was solved. So `doc` in a widened
  window still showed 62-column paragraphs with the rest of the window
  empty, while every row count and percentage was correct.
  `upager_run_src()` takes a `struct upager_source` for that: a callback
  the pager asks for the text again at the new width, whose buffer stays
  the CALLER's. A caller that pages bytes it merely read (`less`)
  supplies none. The index has to be rebuilt with the text, and sized to
  it -- a narrower width wraps into more lines than the old array holds.

- **A WINDOW IS RESIZED IN A TEST BY `gui resize W H`, NEVER BY
  DRAGGING THE GRIP.** The grip needs a real pointer the compositor
  tracks across frames; injected input is not one, and neither is a
  warped-cursor drag, because the frame drag is the WM's own and not a
  client's. A check that dragged it reported the window as the same size
  before and after and still passed both its assertions -- the exact
  "it responds is not it happened" shape. The command routes through the
  same `resize_ask()` the grip does, so it drives the real path
  (propose, the client answers, adopt) rather than a second one.

## A DRAG IS A ROUTER SESSION BETWEEN A SOURCE AND THE WIDGET UNDER THE POINTER, AND IT STAYS INSIDE ONE WINDOW

`uui_route.c`'s third rule (2026-09-10). A press is a click until the
pointer moves `UUI_DRAG_THRESHOLD` with the button held; then the grab
holder is asked `drag_start` ONCE, and if it fills a `struct uui_drag`
the grab becomes a drag: every motion goes to the widget under the
pointer as `drag_over` (with a leave, `UUI_NOWHERE`, to the one being
left), the release becomes `drop` on a target whose last `drag_over`
accepted, the source hears `drag_end`, and the app is told with
`on_widget(id, UUI_REASON_DROP)` naming the TARGET -- it reads the
payload with `uapp_drag()` and where from the widget
(`uui_fileview_drop_target()`, `uui_tree_drop_id()`). Esc cancels.
Qt's QDrag/QDropEvent and GTK's drag-motion/drag-drop have this shape.

**THREE THINGS THAT ARE NOT THE APP'S.** The copy/move bit: `d->copy`
is Ctrl held, set by the router, so every source and target agree
(Explorer's rule; Dolphin asks with a menu on drop, which a target is
free to do instead). The ghost: the source's `drag_draw`, painted after
every overlay, because there is no drag cursor shape. And the
threshold, so a sloppy click never moves a file.

**THE TRAP IS THE PRESS.** A plain press used to clear the marked set
on the way DOWN, so a drag could never carry more than one file. The
fileview defers that clear to a release that turned out to be a click
(`deferred_clear`); `drag_start` cancels it. A widget copied from the
fileview inherits the deferral only if it copies that too.

Between WINDOWS is the compositor's, not the toolkit's -- the pointer
leaves the source client's surface -- as `wl_data_device` is Wayland's;
`docs/roadmap.md` has it. `tools/check_widget_ops.py` refuses a `drop`
without a `drag_over` and a `drag_start` without a `drag_end`.

## AN EMPTY-SPACE CLICK DESELECTS, AND THE RUBBER BAND WORKS IN EVERY VIEW

A plain press on a fileview's empty space clears the marks AND the
cursor row (`selected = -1`, an already-legal state) -- Explorer's and
Dolphin's rule -- and arms a band; Ctrl or Shift keeps the set and
makes the band ADD. Decided at the PRESS (`fv_empty_press()`), not at
`rb_end()`, which is what makes it identical in Details and List: those
views had no band and so no band-clear to fall out of. The band's rects
there are the table's rows (`tb_rb_rect()`), the same rubberband.h
engine as the icons grid.

## THE FOLDER TREE FOLLOWS A NAVIGATION, NEVER A TOGGLE

`tree_reveal_path()` (userland/fm/fm_tree.c) opens every ANCESTOR of
the active pane's new directory, rebuilds, selects the node and
`uui_tree_select_id()` scrolls it into view. It runs from
`on_pane_dir()` and nowhere else. The first version was reverted for
fighting a branch the user collapsed while standing in it; the fix is
the trigger, not the mechanism: a collapse changes no directory, so the
collapse stays until the next navigation re-reveals -- which is what
Dolphin's folder panel does, and what Explorer's "expand to current
folder" option does when it is on. The node itself is NOT opened, only
its ancestors.

## MARKS SURVIVE A RELOAD BY NAME, BECAUSE THE VOLUME'S GENERATION NEVER STOPS MOVING

`uui_fileview_reload()` remembers the marked NAMES (up to
`UUI_FILEVIEW_KEEP_MARKS`) and an empty selection, re-reads the rows,
and marks the same names again -- the selection already survived that
way. It used to clear the set, on the sound reasoning that a mark
names a row and the rows are being replaced; what that missed is WHEN
a reload happens. The File Manager reloads on a tick whenever
`SYS_FS_GENERATION` moved, and on a running desktop it moves every
second or so (service logs are files), so marks vanished about half a
second after they were made unless the user happened to be mid-band.
That was the whole of the "marking files does nothing" cluster in
`docs/bugs.md`: Insert marked the row, the next tick unmarked it, and
the tool read the second state. Found with a return-address log in
`clear_marks()`, resolved through `pmap` and `addr2line`.

**THE TRAP THAT REMAINS**: a caller acting on marks still snapshots the
paths before a long operation (`queue_from()` does) -- a name can be
re-marked on a row that no longer means the same file only if the
file was replaced under the same name, which is the case a snapshot
cannot help with either.

## THE DESKTOP'S ICON SIZE IS A NAMED SETTING, THE ICONS ARE CENTRED, AND A CAPTION IS TWO LINES

`desktop.icon_size` = `small` | `medium` | `large` (32/48/64 px),
declared by `/etc/settings.d/desktop.icon_size`, read by
the desktop on the same generation poll as the wallpaper and applied
at once by the context menu. Named sizes rather than a pixel count
because a menu can list three names with a tick and System Settings
draws an ordered enum unaided (Windows' View > Large/Medium/Small
icons; KDE names its steps too). The pixel value of a name is the
DESKTOP's, not the registry's. **A SIZE CHANGE REFLOWS ONLY WHAT NO
LONGER FITS**: a saved cell is a (column, row), and rows that fit at 48
px do not at 64, so a full column's tail went under the taskbar on the
laptop. `reflow_overflow()` moves those icons to the nearest free cell
above the taskbar (`nearest_free_cell()` is bounded by
`rows_that_fit()` now) and saves them; everything that still fits stays.

The icon sits centred in its column and the caption is up to two
word-wrapped lines under it, centred, the second cut with `..` when
the name runs on -- KDE's and Windows' default -- through the toolkit's
`uui_label_wrap_next()`. The File Manager's icon view does the same
with the same helper. `icon_box()` is the ONE function that says where
an icon is: drawing, the hit test, the rubber band and `gui icons
--json` all read it, which is what lets a test click what the desktop
reports instead of hardcoding a 48 px tile at (16, 16). The cell's own
x stays the column's left edge, which is what the grid, the drag and
the saved positions speak; only the drawing and the hit box are offset.

## A POPUP OPENED FROM ANOTHER OVERLAY NAMES IT AS ITS PARENT, AND `close_others()` SPARES BOTH

`wm_overlay_set_parent(name)` before opening the child; the child clears
it when it closes. That is Wayland's popup chain -- an `xdg_popup` does
not dismiss the surface it hangs off -- and it is what keeps the Start
menu up while the right-click menu for one of its rows is open, as
Windows does. The table order already supported it: `context` sits
before `start`, so the child takes the click and is painted last.

**A PARENT DOES NOT HOVER WHILE ITS CHILD IS UP.** The popup is drawn
over it, so a row lighting up under the menu belongs to neither -- the
Start menu went on tracking the pointer and highlighted whichever of its
rows the context menu happened to cover. `wm_overlay_hover()` hovers the
parent at a point that is on nothing, which also DROPS the row it was
holding rather than freezing the last one lit. Only a PIXEL sees this:
the highlight is not a state any overlay reports.

**A LAUNCHING VERB STILL DISMISSES THE PARENT.** `ctx_open_app()` closes
the Start menu itself, because opening a window is what closes Start
everywhere else; "Add to desktop" deliberately does not, so you can do
another. **And the child MUST clear the parent on BOTH its close paths**
-- `context_menu_close()` and the commit inside
`context_menu_handle_click()` -- or the next `close_others()` spares an
overlay nobody meant to keep.

## A DEFAULT ICON CELL IS CHOSEN AFTER THE SAVED ONES, NOT BEFORE

`desktop_load_positions()` runs in two passes. Pass 1 places only the
cells the user actually chose and parks everything else OFF-GRID; pass 2
gives each remaining icon the first FREE cell in the default
column-major order. It used to default every icon to its index's cell
and then overwrite that from `desktop.conf`, so a NEW icon's default
could be a cell a saved position already owned -- with nothing checking.
A launcher added from the Start menu landed ON TOP of another icon.

The off-grid park is what makes pass 2 work: without it "not placed yet"
and "placed at 0,0" are the same value. `gui icons --json` reports every
icon's rect, so two sharing one is assertable.

## A SELECTION CHANGE MUST DAMAGE THE RECTS IT CHANGED, NOT JUST SET `redraw_pending`

`redraw_pending` with no damage repaints everything only in a QUIET
frame, and the taskbar clock is damaging one most seconds -- so a
deselected desktop icon kept its highlight PAINTED while
`gui icons --json` said it was not selected. Clicking icons in turn
looked exactly like Ctrl+click accumulating a selection, and no
assertion on the app's own answer could see it: the report was right
the whole time. `desktop_handle_click()` snapshots the selection and
damages every icon whose state moved.

**THE TEST FOR THIS CLASS IS A PIXEL, NOT A REPORT.** `icons_test.py`
asserts both halves and the pixel half is the one that reddens -- its
positive control leaves the JSON check green.

## THE WM CONTEXT MENU IS `uui_menubar`, WITH THE PANEL'S ITEM MODEL OVER IT, AND THE DESKTOP'S MENU IS WINDOWS' SHAPE

`context_menu.c` opens a `struct uui_menubar` through
`uui_menubar_open_at()` with `count == 0`, so the widget draws and
hit-tests and this file only supplies rows. `struct context_menu_item`
keeps `label`/`on_select`/`ctx` -- a caller packs a `gui_app *` or a
window index into `ctx`, which the widget's `int code` cannot carry;
the code maps back to the row here -- plus the optional `sub`,
`sub_count`, `checked`, `separator`, and the widget's row extras:
`icon`/`tint` (an action role), `accel`, `strip`, `disabled`. `checked`
and `disabled` are answered through the widget's `item_flags`, so they
follow the caller's live struct with nothing to keep in sync.

**It has NO COLOURS OF ITS OWN**: the WM's menus are the toolkit's card
(`ui/uui_popup.h`), so a desktop menu and an app's are one design.
Submenus nest TWO deep (Open > Category > app) out of one pool of rows.
**It takes the keyboard** through the overlay registry's `key` op
(`context_menu_key()`): the widget's keymap, a commit running the row
exactly as a click does. A separator is a `NULL` label. `gui ctxmenu
--json` reports each row's own rect (`x`, `w`, `cx` -- a strip button's
are its own), `disabled`, the open submenu under `sub` and the one below
it under `sub2`, all from `uui_menubar_item_rect()`.

It works inside the compositor because `uui_popup_open()` is a no-op
with no provider and the panel installs none for its own surface, so
every level draws into `wm_surface()` -- clamped against the rectangle
`wm_popup_place()` defines, handed to the widget as its bounds.

The desktop's background menu is Open > (by the Start menu's
categories), New folder, Paste (greyed with nothing to paste), Refresh,
Sort by name, Icon size > (ticked), System Settings; an icon's is a
strip of Cut, Copy, Rename, Delete, then Open and Properties -- Windows
11's. Each row's icon is coloured by what it does.

**THE TRAP FOR A TEST**: a real-mouse click after a `warp_cursor()`
onto a submenu row did not land (`icons_test.py` measured it); the
injected `dbg.click(x, y)` at the reported row centre does.

**THE RESIZE CURSOR IS RASTERISED ANALYTICALLY, NOT STEPPED ALONG ITS
AXIS.** For each pixel, how far ALONG the arrow (u) and how far ACROSS
it (v) decides whether it is filled -- which is solid in any direction
and makes the three one arrow rotated. Stepping along the axis sets only
the pixels whose `x + y` is even on a DIAGONAL, so the corner cursor came
out a checkerboard and read as bigger and different from the edge ones;
`RZ_DIAG` (256/sqrt(2)) is what keeps a diagonal arrow 19 px long rather
than 27. `wm_render.c`'s `draw_resize_cursor()` is the BUILT-IN
fallback only: since Classic was redrawn (2026-10-01) every shipped
theme is an image theme over `tools/gen_cursors.py`'s one supersampled
geometry, and the generator no longer ports this arithmetic (it was
`resize_shape()`). The shape is CENTRED on its hotspot, so
`cursor_rect()` gives it a centred box: the other built-ins draw down
and right from theirs.

## THE DESKTOP'S SELECTION IS GLASS, ITS HOVER ASKS `wm_point_on_desktop()`, AND A CAPTION RENAMES IN PLACE

The selection and the hover are `uui_glass_round_rect()` over the
wallpaper (white at a per-state alpha, a rounded edge when selected),
drawn in `icon_hl_rect()` so what is painted and what is damaged stay
one rect. **The hover is fed from `wm.c` with the WM's own answer to
"is the bare desktop under the pointer"** (`wm_point_on_desktop()`: no
overlay, no taskbar, no window), because the desktop cannot see what
is on top of it -- asking only `icon_hit_test()` lights an icon through
a window. It is a REGISTRY index like the selection, so a reload resets
it.

**Rename is in place**: `begin_rename()` puts a `uui_textbox` over the
caption with the name -- not the extension -- selected (the File
Manager's rule), `desktop_handle_key()` takes every key while it is up,
and `desktop_rename_click()` runs FIRST for every left click, consuming
one on the field and committing on any other. A file goes through
`sys_rename()`, which refuses an existing name; a LAUNCHER's caption is
its `Name=`, rewritten line by line with the rest of the file kept, so
the file name, its saved position and its AppId do not move. `gui icons
--json` reports `hovered` and `renaming` per icon.

## THE DESKTOP IS `/home/desktop` AND NOTHING ELSE: A `.desktop` FILE THERE IS A LAUNCHER, THE APPLICATION DATABASE IS `/usr/wm/applications`, AND EVERY VERB IS A CHILD PROCESS

Every icon in `userland/wm/desktop.c` is an entry of `/home/desktop`, in
name order, directories first -- KDE's Folder View and the Windows
desktop are folders with the shortcuts in them. A `.desktop` file there
is drawn as the launcher it describes (`gui_app_read_entry()`, the same
parser the application database uses) and opens through the registry
entry with its `AppId`, or runs its `Exec=` directly when the database
no longer has one. It is cut, copied, dragged and deleted like any
file. **The application database (`/usr/wm/applications`, formerly
`/usr/wm/desktop`) puts NOTHING on the desktop**: it feeds the Start
menu and the desktop menu's Open > submenu, and a Start menu row's
right-click offers "Add to desktop", which writes `<app_id>.desktop`
into the folder. Five launchers are seeded ONCE (`seed/once/`, copied
only when missing), so deleting one sticks across `make iso`. The
folder is re-listed on the same generation poll as the wallpaper, and
a change re-derives the grid (positions are saved under
`file:<filename>`, so a launcher keeps its cell whatever its `Name=`).
A plain file opens through `/bin/open` (its association), a directory
in the File Manager.

**THE COMPOSITOR DOES NO FILE WORK.** Copy and Cut put the selected
files' paths on the system clipboard (`lib/uclip.h`, the File
Manager's own carrier, so the two exchange freely: Ctrl+C on a desktop
icon then Ctrl+V in a pane, or Ctrl+C in a pane then Paste on the
desktop). Paste spawns `/bin/cp -r` or `/bin/mv` per item with an argv;
Delete asks through the WM's confirm dialog and spawns `/bin/rm -r`;
only "New folder" is a syscall, because `mkdir` is one. A long
operation in the compositor's loop would freeze every window
(docs/conventions/gui.md, "LONG WORK BELONGS IN A CHILD PROCESS").

**The desktop is the keyboard focus when no window is** (`wm.c`'s
dispatch): Ctrl+C/X/V and Delete reach `desktop_handle_key()` there and
nowhere else. And a right-click over an icon SELECTS it first and opens
that icon's menu -- Open for a launcher; Open, Cut, Copy, Delete for a
file -- the rule every file manager has. The per-icon identity
`desktop.h` once said was missing is a file's path.

## A DRAG BETWEEN WINDOWS IS BROKERED BY THE COMPOSITOR, AND ITS PAYLOAD RIDES A SLOT BESIDE THE CLIPBOARD

`userland/wm/wm_dnd.c`. A client's toolkit drag cannot see past its
window, so when one starts the client says `WIN_REQ_DRAG_START`
(having put the files in the clipboard page's DRAG SLOT,
`uclip_drag_begin/add/commit`), and the compositor offers the drag to
whatever the held pointer is over that is not the source:
`WIN_EV_DRAG_OVER` while it hovers, `DRAG_LEAVE` when it moves on,
`DROP` on the release. In the receiving client `uapp` turns those into
the router's `uui_router_extern_over/leave/drop`, so a widget's
`drag_over`/`drop` ops serve a drag from anywhere -- the File Manager's
panes and tree accept a drop from the desktop with no code of their
own beyond `do_drop_extern()`, which reads the slot. The desktop is a
target (files land in `/home/desktop`, Ctrl copies) and a source (a file
icon dragged onto a window; the icon stays in its cell when a window
took the drop). Wayland's `wl_data_device` is this shape; X11's XDND
made the two windows talk directly, which every compositor since has
undone.

**THE SLOT IS SEPARATE FROM THE CLIPBOARD AND IS NOT CLEARED ON THE
RELEASE.** Separate, because a drag must not clobber what was copied
(`XdndSelection` and Wayland's data source are separate for the same
reason). Not cleared, because the DROP reaches its target as an event
after the source's release: the first version cleared it on the
release and the pane accepted mid-drag and then moved nothing. The
next drag's begin overwrites it, and nothing reads it between drags.

The source keeps its own session throughout: it still gets
`MOUSE_MOVE`/`MOUSE_UP` with the button held, so a drop back in its own
window is its own toolkit's, and a drop elsewhere reaches it as a
release nothing accepted while the file has moved anyway. Outside the
source the compositor draws the ghost (a count), since a client's ghost
stops at its window edge.

## A RESIZE EDGE IS A `WM_EDGE_*` MASK, AND THE TITLE BAR'S TOP STRIP IS PART OF IT

`wm_find_resize_zone()` returns a mask, not a pair of bools, and a
corner is two bits rather than a ninth case -- Wayland's
`xdg_toplevel.resize_edge` shape. Three things that bite:

- **The edge test must run BEFORE the title-bar test.** The top edge IS
  the title bar on every window here, so refusing the whole bar (which
  is what the code did until 2026-09-16) makes the top edge and both
  top corners unreachable while looking like a deliberate exclusion.
- **The opposite edge is the anchor, so LEFT and TOP clamp the ORIGIN,
  not the extent.** A left drag at the minimum width must stop moving
  `x`; clamping `w` instead walks the window sideways at a constant
  size. The four edges clamp four different quantities on purpose.
- **MOST OF THE BORDER IS OUTSIDE THE WINDOW** -- `RESIZE_OUTSIDE` 8
  against `RESIZE_INSIDE` 2. The inside is expensive (the top edge lies
  over the title bar; at 8 the corner ate 16 rows of a 23-row bar and
  showed a diagonal at its midpoint) and the outside is free. Two
  guards make the band safe: a non-resizable window projects none, and
  a window owns everything genuinely inside it, so a lower window's
  band cannot reach through one in front.
- **Anything that hit-tests a resize edge must CALL this function.**
  `wm_debug.c` open-coded the same two comparisons and so reported the
  old zones for a day; `wm_render.c` asks it every frame for the cursor.
  A second copy of the test is a second thing to forget.

**And a test that drags edges must reset the window between cases.** A
window dragged into the screen corner cannot grow further up or left,
and the clamps correctly refuse -- which reads as five broken
directions. `tools/resize_edges_test.py` resets, and asserts the reset
worked, so a fixture failure says so instead of blaming the WM.

### A long alpha run is one span, not a loop of `ugfx_blend_pixel()`

`ugfx_blend_pixel()` re-checks the surface, the bounds and the clip and
re-marks damage on EVERY call, so a per-pixel loop pays that overhead
once per pixel. `ugfx_blend_hspan()` does the clipping and the damage
mark ONCE for a whole horizontal run, taking per-pixel coverage from an
array or one constant alpha for the run.

The drop shadows are why it exists. They walk a window's perimeter band
every frame a window is dragged, which is ~113,000 pixels for a large
window; moving the flat middle of each edge onto a constant-alpha span
and the varying left/right bands onto a coverage array made the shadow
**2.6x cheaper** (5.93 -> 2.28 ms per drag step, measured against a
shadows-off control that stayed within 2%).

Two things that made the decomposition exact, both worth knowing before
editing `wm_shadow.c`:

- **The skipped window interior IS the middle segment.** `ix0 == cx0`
  and `ix1 == cx1 + 1`, because the casting rect is offset only in y
  (`sx == x`), so an interior row simply omits its middle span instead
  of needing a second range test per pixel.
- **The straight edges' falloff is a LUT, and only the corners are a
  tile.** `fall_alpha()` divides; it now fills `tile.edge[]` once per
  (radius, alpha) change and the hot path is a lookup.

**`cov` is indexed from the UNCLIPPED `x`**, so clipping a run does not
shift its coverage -- pass the run's true origin, not the clipped one.

## AN OVERLAY ANSWERS WHERE IT IS; THE CORE DAMAGES IT

`struct wm_overlay`'s `rect` op (`userland/wm/wm_overlay.h`). An overlay
reports its rect and nothing else: **the core adds the shadow and the
rect it was LAST DRAWN at**, because those are the two things an
overlay that spells its own damage forgets.

Both were got wrong before it existed. The volume flyout called
`wm_damage_rect()` where every other shadowed popup called
`wm_damage_window_rect()`, and left a ghost of its shadow on close. And
when it grew a slider per audio stream it became the first overlay
whose SIZE changes -- anchored above the taskbar, so gaining a row
moves its TOP UP, and damaging only where it is now leaves the band it
vacated holding the old frame.

**`damage()` SHOULD BE ONE LINE**: `void volume_damage(void) {
wm_overlay_damage("volume"); }`. Keep a hand-written one only where the
damage is genuinely not one rect -- the context menu damages a rect per
open submenu, the confirm dialog asks for a full repaint on purpose --
and those two pass `rect = 0`.

**The core records the rect after it DRAWS each overlay**, so "where
was it" is bookkeeping nobody has to remember. `wm_render.c` already
did this for WINDOWS, comparing each one's last-rendered rect against
its current one, and its own comment called the overlay half "a
deliberately scoped first cut, not the final word". Weston spells the
same idea `weston_view_damage_below()`; wlroots' scene graph keeps each
node's last-rendered box for it.

## AN OVERLAY THAT REFRESHES ONLY WHILE OPEN GETS AN `on_open`, AND THE CORE FIRES IT

`struct wm_overlay`'s `on_open` op. The core compares every overlay's
`is_open()` against the last draw pass and calls the hook on the
transition, from INSIDE `wm_overlay_draw()` -- which is after input and
before drawing, so a click that opened the overlay is served in the
frame that click belongs to.

**THE PER-FRAME POLL IS NOT SOON ENOUGH, and the gap is not one frame's
worth of milliseconds.** `volume_poll_config()` and its peers run
BEFORE input, so the click that opens a panel is the last thing in that
frame and the correcting poll is the NEXT one -- and the compositor
idles between frames, so "next frame" is as long as that idle. Measured
on the test laptop: the volume flyout drew a slider for an audio stream
that had already ended, for 100-300 ms, reported three times as a
flash before it was instrumented.

**DO NOT PUT IT IN THE OVERLAY'S OWN OPEN PATH.** There is more than
one: a tray popup opens inside `tray_slider_click()`, not in its own
`*_open_now()` -- which turned out to have NO CALLERS, so the first
attempt at this fix was dead code that changed nothing anybody could
see. The core knows the transition; no overlay should have to.

NULL for the overlays polled every frame regardless, which is most of
them -- they are never more than a frame behind. It is for state an
overlay deliberately does NOT track while closed because reading it
costs something and nothing draws it.

**AND THE FIELD IS LAST IN THE STRUCT ON PURPOSE**: the table is
written with POSITIONAL initialisers, so a field added in the middle
shifts every row's later ops by one. Adding this before `close` moved
nine overlays' close op into it, and only
`-Wmissing-field-initializers` said so.

## THE COMPOSITOR'S CONFIG IS PUSHED; NOTHING ON THE FRAME PATH POLLS `sys_fs_generation()`

That counter moves on every write in the machine, so a poller keyed on
it re-read its configs every frame under any disk-heavy program -- and
each read queued for the one filesystem lock behind that program's disk
wait (0.6-1.7 s frames, measured). The WM's pollers compare a TOPIC
counter from `userland/wm/wm_watch.h` instead: `WM_TOPIC_SETTINGS` moves
on `WIN_EV_SETTING` (the registry's generation bump, pushed), the path
topics on `WIN_EV_FSWATCH` from a `SYS_FS_WATCH` on `/etc`,
`/etc/effects`, `/usr/wm/applications` and `/home/desktop`. A new
config poller picks a topic, or adds one with its path; it never reads
the fs generation. The watch is on a directory's DIRECT children, so a
file two levels down needs its own topic (that is why `/etc/effects` is
separate from `/etc`). Hand edits are covered by the directory watch;
the registry's own writes arrive both ways, which is harmless.

## THE TEXT CARET BLINKS FROM ONE PHASE PER PROCESS (`ui/uui_caret.h`), AND STOPS SOLID TEN SECONDS AFTER THE LAST INPUT

A widget that draws a caret asks `uui_caret_visible()` at the moment it
draws it -- `uui_textbox` and `utext` (so `uui_textview`, Notepad) do --
and never keeps a blink timer of its own. The call also RECORDS that a
caret was drawn in this phase, and `uapp`'s wait loop wakes for the next
flip only then; a key or a press anywhere in the process restarts the
cycle on. Windows, Qt and GTK all blink one caret per app, and GTK's
`gtk-cursor-blink-timeout` is copied: after ten seconds of no input the
caret stays SOLID, so an idle window sits still
(`docs/gui-guidelines.md`, "An idle screen must SIT STILL").
`desktop.caret_blink` turns it off. The Terminal's grid cursor keeps its
own `cursor_blink`, since a terminal's cursor is the program's, not a
field's.

**THE TRAP: a flip is due, the repaint draws NO caret (its field lost
focus), and the loop asks for the repaint again forever** -- 100% CPU
in every window that had once had a caret. `uui_caret_wait_ms()` returns
0 ONCE per phase for that reason; a caller must act on the 0 it gets
rather than asking twice.

## "SHOW ADVANCED SETTINGS" APPEARS ONLY ON A PAGE THAT HAS ONE

System Settings' disclosure is shown when the open page declares an
`Advanced=1` setting, not whenever `g_show_advanced` is set: that flag
is global, and testing it put the checkbox on every page once it had
been ticked on one. No stock setting is `Advanced=1` now --
`system.cursor_style`, the only one, is a plain setting on the Console
page -- so the checkbox appears only for a setting a user adds.

## `uui_table` GROUPS AND ITS TREE ARE ORDERINGS, AND THE FOLDED STATE IS THE APP'S

`uui_table_set_groups()` and `uui_table_set_tree()` add to the sort, in
one `order`: groups outermost (ascending, never re-sorted), then the
parent tree, then the comparator among SIBLINGS -- so a sorted tree keeps
each child under its parent, as GtkTreeView and KDE System Monitor do. A
group's caption is a VIEW row with no app row: `uui_table_source_row()`
answers -1 for it, keys step over it and nothing selects it. A parent in
another group does not nest, and a cycle is cut rather than followed.

**The table never stores whether a parent is folded.** It asks
`collapsed(row)` and leaves an expander click, or Left/Right, in
`toggled` for `uui_table_take_toggled()`; the app flips its own state,
keyed by what the row IS (Task Manager: the pid), and calls set_rows(). A
row index moves on every refresh, so a table that remembered "row 7 is
folded" would fold whichever process slid into row 7. Cell HEAT
(`uui_table_set_heat()`, 0..255) is shaded in the theme's accent, and
selection outranks it. `userland/tests/table_tree_test.c` covers all of
it with a fixture stored out of screen order.

## A BUTTON'S `on_widget` FIRES ON PRESS, MOTION AND RELEASE; ACT ON RELEASE OR KEY

The router names a widget to its app whenever the widget reports a
change, with the reason (`UUI_REASON_*`). A lone `uui_button` changes on
the press (armed), on motion (dragged off), and on the release -- and
only the release, or a key, is the click. An app that acts on every
report acts three or four times: Task Manager's first rail build sent
SIGSTOP four times per click and four init requests per service click,
the extra three timing out behind the first. Test
`reason == UUI_REASON_RELEASE || reason == UUI_REASON_KEY` before a verb.

## A CLIENT HOLDS ITS OWN MINIMUM: `uapp` CLAMPS A PROPOSED SIZE TO `min_w`/`min_h`

TWS clamps a resize proposal to the minimum a client declared in its
hints -- but a size remembered in `/etc/windows.conf` is proposed at
CREATE, before those hints arrive, so it can be anything. `uapp`'s
`WIN_EV_RESIZE` handler therefore raises a proposal to `desc.min_w` and
`min_h` before resizing; the client commits its own size (Wayland's
configure rule), and the frame follows the buffer it presents. Task
Manager sets its minimum from the font in `on_size()` (so the minimum is
font-derived), which is why its `uapp_desc` is a static rather than
main()'s local.

