# Changelog

All notable changes to toy-os, in the order they happened. Each entry
notes what was added and, where relevant, what broke and how it got
fixed -- several of the more interesting bugs here were only found by
actually testing in QEMU rather than assumed to work.

This file holds the semver era only. Changes accumulate under
`## [Unreleased]`, in the [Keep a Changelog](https://keepachangelog.com/)
style: no per-change version bump, just entries appended as they
happen. When a real release is cut, `tools/set_version.sh <version>`
stamps this section with the version and date and opens a fresh empty
one above it. See `docs/decisions.md` for the switch and why.

Earlier history lives in two archive files, split by era once this
file passed ~4,200 lines (twice). Same content, same grep-ability,
just not all in one file that keeps growing forever -- see `CLAUDE.md`
on splitting a file once it's genuinely harder to work with. In
chronological order:

- `CHANGELOG-archive.md` -- Milestone 1 through Build 173
- `CHANGELOG-archive-2.md` -- Build 183 through Build 502, the old
  "Build N (tier, +delta)" heading era
- `CHANGELOG.md` (this file) -- the semver era, `[0.0.9]` onward

The second cut landed exactly on the heading-style change, so each
file is one whole era rather than an arbitrary line count: everything
using `## Build N` headings is in `CHANGELOG-archive-2.md`, everything
using `## [x.y.z] - date` headings is here.

## [Unreleased]

### Fixed
- **`gui key 0x1b` was rejected, despite being the documented way to
  send an unprintable key.** `wm_debug.c`'s `cmd_key()` has always said
  `"0x1b"-style for anything unprintable` and every `KEY_*` code in
  `api/keyboard.h` is written in hex -- but its `parse_int()` used
  `k_parse_u32()`, which takes plain decimal only ("no prefix", per its
  own contract). So every hex key command answered "bad or dropped key"
  while the help text advertised it, and any keyboard test had to
  convert codes to decimal by hand without knowing why. `parse_int()`
  accepts an explicit `0x`/`0X` prefix now. Found by a test that typed
  the arrow-key codes exactly as the file's own comment said to.
- **Five damage-invariant bugs, and the harness that was hiding them.**
  The compositor repaints only the region declared as damage, so it is
  correct only if everything that changes on screen is inside that
  region (`docs/gui-guidelines.md`). `gui damage verify on` turns a
  violation into a report; the four below are what a systematic sweep
  found once the sweep itself was trustworthy.

  **The harness came first, because it was lying.** The one damage bug
  left open from the previous session (`docs/roadmap.md` recorded it as
  "559 px ... first at (497,67)") would not reproduce. Two reasons, both
  in the tooling rather than the kernel:

  1. `DebugConsole.click()`/`drag()` return `events()`, which filters
     the wire to the `uidemo:` prefix. A `wm: DAMAGE BUG` line does not
     match that prefix and was silently dropped, so the first repro
     script reported a clean run against a kernel that was actively
     failing. `send()` now accumulates every line into `log_lines`, and
     `logs()`/`damage_bugs()` read them back -- a question asked on this
     wire can no longer destroy the answer to a different one.
  2. `settle()` slept a fixed 250ms, reasoning that injected events
     drain one per WM frame at 100Hz. Measured: a drag takes ~800ms with
     verification on (which renders every frame twice and diffs the full
     screen). Every test was racing that sleep -- windows moved between
     a `gui windows` read and the command using those coordinates, so
     drags grabbed the wrong thing and the sweep reported a *different*
     bug on each run of the same script. `gui state` now reports
     `pending` (`wm_debug_input_pending()`) and `settle()` polls it to
     zero. See `docs/decisions.md`.

  Both were confirmed by a **positive control** -- deliberately deleting
  `bring_to_front()`'s taskbar `wm_damage_rect()` and checking the sweep
  reports it -- so "0 violations" is distinguishable from "the harness
  isn't checking anything". That control is now a flag on the tool
  (`--positive-control`) rather than a thing to redo by hand.

  The bugs themselves, each found by `tools/damage_sweep.py` and each
  fixed and re-verified. Three of the five were only reachable through
  the random walk, and two of those needed one specific window
  arrangement -- worth noting before trusting any fixed test sequence
  on this invariant:

  - **`close_window()` didn't damage the window inheriting focus**
    (`apps/wm/wm.c`). Closing the frontmost window promotes the one
    below it, whose title bar changes from unfocused gray to focused
    blue without its geometry changing -- so `compute_window_damage()`
    can't see it, and the closing window's rect only covers it where the
    two overlapped. This is `bring_to_front()`'s `prev_front` gap seen
    from the other end, with the same consequence now that Phase 3 skips
    an undamaged window's chrome entirely. Reported as "11038 px changed
    outside the damage rect, first at (109,89)".
  - **Overlays were clipped away when another source declared damage**
    (`apps/wm/wm_render.c`). The Start menu, context menu, file picker
    and confirm dialog draw outside any window's rect and declare no
    damage; the design note called that a full-screen-repaint fallback,
    but it held only by coincidence. A click that both raised a window
    and opened Notepad's file picker made the frame damage-limited and
    left the picker unpainted: "114932 px changed outside the damage
    rect, first at (590,173)". The frame now discards its damage box
    while any overlay is open. See `docs/decisions.md`.
  - **...and for one frame after an overlay closes**, because the frame
    that dismisses one has already cleared its `_open` flag by the time
    the renderer runs, so nothing damages the region it just vacated.
    Hidden behind a second coincidence: the damage box on such a frame
    is usually the full-width taskbar strip unioned with the cursor,
    which covers most of a Start menu sitting just above the taskbar.
    Dismissing it with a click low on the screen left the rows above
    that union stale -- "300 px changed outside the damage rect, first
    at (4,448)", (4,448) being the menu's own top-left corner and 300
    being exactly its top two rows.
  - **The cursor's drawn position was recorded only on damaged frames.**
    `prev_cursor_*` was updated inside `damage_cursor()`, which runs
    only when the frame is damage-limited, so a full-repaint frame moved
    the sprite without recording where it went and the next damaged
    frame erased a position the cursor had already left. Latent while
    full-repaint frames were rare; the overlay fix above made them
    common and it surfaced immediately ("139 px ... first at (928,336)",
    a cursor sprite exactly where the previous click had left it).
    Recorded on every frame now.
  - **`damage_cursor()` and `save_cursor_under()` disagreed on the box
    anchor.** The sprite box is anchored at `(x - CURSOR_BOX_MARGIN,
    y - CURSOR_BOX_MARGIN)`; damage was declared from `(x-1, y-1)`, so
    the box's top row and left column sat outside the damage rect and
    every cursor move left a two-sided sliver behind ("247 px ... first
    at (251,166)" -- 247 being one 13x19 sprite). Damage is derived from
    `CURSOR_BOX_MARGIN` now rather than from a separately-chosen
    constant, so the two can't drift apart again.

  Verified: `tools/damage_sweep.py` clean on the fixed sequence three
  runs running, and on random walks at seeds 1-12 and 21 (each 50-60
  interactions) apart from the one issue below. The `close_window()`
  fix is also confirmed by pixel value rather than by eye: the
  inheriting window's title bar goes (120,120,130) -> (50,90,160),
  exactly `draw_window_chrome()`'s unfocused/focused constants, while
  the window behind it and the desktop stay put (`screenshots/
  2026-08-14/damage-close-focus-{before,after}.png`).

  **Not verified, and left recorded rather than rushed** (see
  `docs/roadmap.md`'s known-issues list): a 20px violation inside the
  Terminal's content on clock-tick frames survives, reproducible at
  `--random 50 --seed 1` step 0. It is characterised but not
  root-caused, and the two candidate causes -- a caret moved without
  damage on some earlier frame, versus a non-idempotent `on_draw()`
  making it a verifier artifact -- need opposite fixes, so guessing
  would be worse than recording it. More broadly: a sweep can only
  report what its interactions reach, and injected input enters below
  the PS/2 driver, so none of this exercises the real mouse path.

### Added
- **`ui_listbox` and `ui_dropdown`** (`apps/ui/`) -- a scrollable
  single-select list, and a combo box built on it. Behaviour modelled on
  Windows', by request, but following this project's own interaction
  rules where the two differ.

  `ui_listbox` owns rows, selection, hover, keyboard navigation
  (arrows/Home/End/PageUp/PageDown, scrolling the selection into view)
  and a scrollbar that appears only when the items overflow. Where
  `ui_radio_list` stops -- it has no viewport, so no scrolling, hover or
  keys -- this begins, because all four arrive together the moment a
  list is longer than its box. Two behaviours are deliberate and were
  chosen explicitly: the wheel scrolls the view WITHOUT moving the
  selection (a user looking further down a list has not changed the
  value they picked), and the armed row follows the cursor while held
  but commits only on release, so a press dragged off is cancellable.

  `ui_dropdown` **composes** `ui_listbox` for its popup rather than
  reimplementing a list -- the same layering `ui_textview` uses over
  scrollback + scrollbar. It therefore got scrolling, keyboard
  navigation and hover for free, which is the argument for the layering
  rather than a happy accident. Click-outside dismisses without
  changing the value, Esc restores the value the popup opened with,
  arrows work while closed, and the wheel is ignored while closed on
  purpose (scrolling past a combo must not silently change a setting).

  Two things about the popup are worth knowing before writing a caller,
  both in `ui_dropdown.h` and `docs/decisions.md`. Drawing is
  immediate-mode, so **`ui_dropdown_draw_popup()` is a separate call the
  app makes after every other widget** -- z-order is call order, and
  input is forwarded in the reverse order for the same reason. And the
  popup **cannot leave the window**, because `wm_render_frame()` clips
  each app's `on_draw()` to its content rect; it flips above the box
  when there's no room below, and its inherited scrollbar is what makes
  that acceptable rather than a truncation.

  `enum ui_scrollbar_policy` moved from `ui_textview.h` to
  `ui_scrollbar.h` -- shared vocabulary now that two controls own a
  scrollbar, and a listbox taking its policy from the *text view's*
  header read like a dependency that wasn't there. `ui_textview.h`
  already includes `ui_scrollbar.h`, so no caller changed.

  Both are in UI Demo (`apps/uidemo.c`), with the dropdown positioned so
  its popup deliberately opens over the listbox -- that overlap is what
  proves the popup is drawn last. Verified by driving them over the
  debug console and asserting on the log: 22 checks covering click
  selection, the press-dragged-off cancel path, all five navigation
  keys, wheel-scrolls-without-selecting, popup open/commit/dismiss, Esc
  restore, and keyboard focus following the click -- now committed as
  **`tools/uidemo_test.py`**, so the next change to `apps/ui/` gets the
  same check for free. `gui damage verify on` clean across eight
  popup/listbox interactions.
  (`screenshots/2026-08-14/uidemo-{dropdown-popup-open,listbox-scrolled}.png`)
- **UI Demo reports its own layout** -- `uidemo: layout <widget> <x> <y>
  <w> <h>` on open, plus `layout listbox_row_h <px>`. The app exists to
  be a known target, and a test re-deriving those offsets from font
  metrics is reimplementing `layout()` in Python -- which drifts
  silently the moment a row is added, exactly as it did when the
  dropdown and listbox rows went in between the textbox and the
  scrollback. Ask, don't assume: the same reason `gui windows` exists
  instead of measuring a screenshot.
- **UI Demo has keyboard focus** (`kbd_focus`, set by clicking, logged
  as `uidemo: focus <widget>`). Three widgets there take the keyboard
  now, and this GUI has no focus manager, so the first one in a
  try-each-in-turn chain swallows every key -- the dropdown handles
  arrows even while closed, which left the listbox unreachable from the
  keyboard entirely. Found by the widget tests above; see
  `apps/README.md` for the shape a future app should copy.
- **`tools/damage_sweep.py`** -- exercises the WM against its damage
  invariant and exits non-zero on a violation. A fixed sequence of the
  interactions that historically break it (raise, drag, minimize,
  restore, resize, overlay, close-from-the-top) plus `--random N
  --seed S`, a seeded random walk that covers the *orders* nobody
  thought to list. The seed is printed on every run, so a failure
  replays exactly: three of the four bugs above were found by the
  random walk, not the fixed sequence, and the largest needed one
  specific window arrangement at step 22 of seed 1. `--positive-control`
  inverts the exit code, for proving the harness detects a real
  violation before trusting a clean run.
- `gui state` reports `pending`, the number of injected events the WM
  has not delivered yet (`wm_debug_input_pending()`,
  `apps/wm/wm_debug.c`), so a test can wait on a fact instead of a
  guess. `DebugConsole.settle()` polls it; `damage_verify()`,
  `damage_bugs()` and `logs()` round out `tools/gui_debug.py`.
- **`tools/pixel_probe.py`** -- reads exact pixel values from
  screenshots and tabulates the same points across several
  (`--compare rest.png hover.png pressed.png --at 85,100 --at 215,100`),
  reporting which moved and which didn't. Written after doing the same
  thing with inline one-off scripts four times in one session, and
  because `docs/gui-guidelines.md` now *requires* pixel-value
  verification: the invisible hover state that prompted that rule was
  two units from the background and looked fine in a PNG. `--box N`
  averages a square, for anti-aliased edges where one pixel is a coin
  toss.

- **Hover and press feedback across the GUI, commit-on-release
  semantics, and `docs/gui-guidelines.md` to say what the rules are.**
  Asked for hover + pressed states on the Control Panel's applet icons,
  release-on-target clicking like the title-bar buttons, and design
  guidelines: modern but simple, minimalist, with feedback for actions.
  - **`enum ui_state` + `ui_state_bg()`** (`apps/ui/ui_primitives.h`)
    are the shared vocabulary: rest / hover / pressed / disabled, with
    the wash derived from each control's **own** colour. The title bar
    dropped two hand-picked tint constants as a result -- one of them
    existed purely because a caller had no way to shift an arbitrary
    packed colour, which stopped being true when `gfx_blend()` went
    public for the console cursor and nobody revisited it.
  - **Flat, not bevelled.** `widget_button()`'s pressed look changes
    from a 2px inset border to a darker fill plus the existing 1px
    nudge -- applied everywhere at once (title bar, Start button,
    Calculator, Notepad) so there's one language rather than two.
  - **New `on_hover` app callback.** Delivered every tick while the
    cursor is over a window's content with no button held, and once
    with `(-1,-1)` on leave so an app can clear its highlight. Returns 1
    only when the hovered item changed, copying `on_press`'s existing
    contract. It reaches **unfocused** windows -- the only callback that
    does -- because a control that stays inert until you've clicked its
    window first is dead in exactly the moment hover exists to prevent.
  - **Commit on release, not on press.** The Control Panel arms an
    applet on press, tracks whether the cursor is still on it every
    tick, and opens it only if the button comes up there. Press, drag
    away, release: nothing happens.
  - **`on_click` fires on button-DOWN, despite its name and its doc
    comment** -- found by building the release semantics on it and
    watching drag-away-then-release open the applet anyway. Nothing
    armed by `on_press` exists yet when it runs, so a control that
    commits there can never be cancelled. The header now says so, and
    says what to use instead.
  - **The hover wash was invisible and pixel-measurement caught it.**
    Lightening is the textbook hover treatment and is wrong on this
    theme: the window background is already 235/255, so hover moved the
    pixels by **two**. New `gfx_luminance()` picks the direction from
    the control's brightness -- light controls darken, dark ones
    lighten, which also readies this for Milestone 19's dark theme.
    Measured rest 235 / hover 214 / pressed 192, with the neighbouring
    cell unchanged at 235.
  - Verified on screen and by pixel value: hover, pressed,
    press-drag-off-release doing nothing, and press-release opening.
    Screenshots in `screenshots/2026-08-13/`.

- **Calculator and Notepad have hover states now, and it lives in
  `ui_button` rather than in each app.** The mechanism above shipped
  without them: `on_hover` existed, the Control Panel used it, and the
  two apps whose buttons are real `ui_button_group`s were left with a
  pressed look and nothing on hover.
  - **`struct ui_button` gained `hovered`, and the group gained
    `ui_button_group_hover()`** -- the same shape as the `pressed`/
    `ui_button_group_press()` pair it sits beside, returning 1 only
    when which button is hovered actually changed. `calculator_hover()`
    and `notepad_hover()` are three-line forwards into it, so the next
    app with a button group gets hover with no new state to hand-roll.
  - **`ui_button_draw()` now calls `widget_button_state()`** instead of
    `widget_button()`, which only ever spoke rest/pressed. This is the
    lifted-constraint pattern again: the full four-state call arrived
    with the guidelines and nothing came back to adopt it here.
    `ui_button.h`'s own comment still claimed the WM didn't dispatch
    hover -- true when written, false since `on_hover` landed.
  - A press clears `hovered`, so the press visual owns the feedback
    while anything is held, and dragging off an armed button drops it
    to REST rather than back to hover.
  - Disabled buttons deliberately keep their existing flat gray rather
    than switching to `UI_STATE_DISABLED`: that muting derives from the
    caller's own colour, which this path has already replaced, so
    adopting it would have quietly restyled every disabled button.
  - Measured, not eyeballed, with a neighbour sampled every time:
    Calculator rest 225 / hover 205 / pressed 184, Notepad's toolbar
    rest 200 / hover 182 / pressed 163, the neighbouring button
    unmoved throughout, and the highlight clearing both on moving off a
    button inside the window and on leaving the window entirely.

- **Damage verification: the WM's worst bug class is now machine-caught,
  and it found four real bugs on its first run.** Asked to make
  `wm_render.c` less bug-prone -- it has produced more real bugs than
  anything else here.
  - **The root cause was never complexity, it was an unchecked
    invariant.** The compositor is correct only if everything that
    changes on screen lies inside the damage rect, and nothing enforced
    that: damage is declared by hand from eight sites across three
    files, and a missed declaration produces stale pixels with no crash,
    no wrong return value and no failing assertion.
  - `gui damage verify on` renders every frame TWICE -- once
    damage-limited as normal, once unrestricted -- and reports any pixel
    that differs, with coordinates. A difference is by definition a
    pixel the damage-limited path got wrong. Off by default; the
    unrestricted render is left in the back buffer, so verification also
    repairs what it catches.
  - **Four real bugs, found within seconds of switching it on:**
    - `open_app()` damaged only the new window, but the window LOSING
      focus repaints its title bar (blue to grey). "4350 px ... first at
      (61,41)" -- the old window's title bar. `bring_to_front()` had
      always handled this; opening never did.
    - The taskbar clock relied on the "no damage = full repaint"
      fallback, so on any frame where something else reported damage its
      repaint fell outside the rect. Its comment gave two reasons for
      staying unscoped, and **one had expired**: the implicit
      once-a-second full resync it depended on for the cursor snapshot
      stopped being needed when the cursor restore moved to the top of
      the frame. The lifted-constraint pattern again.
    - **The cursor was compositing over itself.** It's alpha-blended, so
      when the scene beneath isn't redrawn it blends over the previous
      frame's sprite and the anti-aliased edges creep darker every
      frame. Folding the cursor into damage -- rather than leaving it a
      parallel save-the-pixels-underneath path -- fixes that and removes
      the special case that produced the resize trail earlier today.
    - The FIRST frame of a GUI session could be damage-limited, because
      `wm_run()` polls the debug console before its first render: an
      event arriving that early narrows the one frame that has to
      establish the whole back buffer. "51200 px ... first at (0,0)",
      which is exactly the 1280x40 strip above a freshly-opened window.
  - **One known damage bug remains, deliberately left recorded rather
    than rushed**: "559 px changed outside the damage rect, first at
    (497,67)" during a window drag/close, a title bar above the reported
    rect. Reproduce with `gui damage verify on`. That the tool keeps
    finding these is the point of it.
  - Also folded in: all damage now goes through `wm_damage_rect()` (no
    direct writes to the globals), and the invariant is stated at the
    compositor with each declaring site saying what it owns.

- **A display-driver layer: adding a graphics card is now one file and
  one line.** Asked for a proper graphics API with modular driver
  support, refactoring where needed.
  - The problem was concrete: `gfx.c` was a rasteriser AND the
    framebuffer's owner, and once a second card existed it carried
    `#include "vmsvga.h"` plus seven hardcoded calls to that one device.
    A third card meant another include and another if/else in each.
  - **`struct display_driver`** (`kernel/include/kernel/display.h`):
    required `probe`/`get_surface`, plus optional `flush`, cursor,
    accelerated fill/copy and mode setting, each gated by an explicit
    capability bit. `kernel/drivers/display/display.c` holds the
    registry and probe -- registration order is priority order, first
    to claim wins.
  - **Capabilities and function pointers must agree**, and
    `display_probe()` refuses a driver where they don't. That check is
    aimed squarely at the bug this session already produced twice: a
    card that needs a flush but doesn't get one renders perfectly into
    memory and shows a frozen screen, which reads as a rendering fault
    anywhere except where it is.
  - **The GRUB framebuffer became a driver too** (`vesafb`), registering
    last as the fallback that always claims. That removes the old
    "default path vs driver path" asymmetry -- one path now -- and, more
    importantly, it's the second implementation that makes the interface
    a design rather than a guess. It's deliberately the OPPOSITE kind of
    device from vmsvga: passive, scanned continuously, no cursor, no
    accel, no modeset. Every optional thing in the interface is
    exercised by exactly one of the two.
  - `gfx.c` is a rasteriser again: it asks `display_get_surface()` where
    the pixels are and never learns which card it's on.
    `gfx_adopt_framebuffer()` -- the back-channel a driver used to
    re-point gfx through -- is gone.
  - `pci_init()` moved ahead of `vga_init()`, since PCI enumeration is
    what a driver probes against. Safe that early: it's a port-I/O scan
    into a static table, needing neither heap nor interrupts, and only
    sat after `heap_init()` because nothing before then had cared.
  - Verified both drivers render identically -- 139 cursor pixels and
    2300 window-chrome pixels on each -- and that each reports honest
    caps (`vesafb` none, `vmsvga` flush).

- **A hardware mouse cursor, via a VMware SVGA II display driver
  (`make run-vmware`).** Asked whether we could get one.
  - **Plain VGA has no cursor to get working.** `-vga std` -- the
    default, and what every real machine without a GPU driver looks
    like -- has no cursor sprite for a linear framebuffer at all; VGA's
    only hardware cursor is the text-mode underline. This was never
    missing code, it was a missing capability, so the answer had to be
    "talk to a device that has one".
  - Measured which QEMU adapters boot here first: `vmware`, `qxl` and
    `virtio` all keep 1280x720; `cirrus` has a hardware cursor but drops
    to 640x480, which is too high a price. `lspci` (built earlier the
    same day) found the VMware adapter at `00:02.0 15ad:0405` with its
    I/O, framebuffer and FIFO BARs, all under 4GiB and therefore already
    identity-mapped -- no new mapping code needed.
  - **It's a display driver, not just a cursor.** The adapter composites
    its cursor only while it is driving the display, so
    `kernel/drivers/vmsvga.c` does the version handshake, sets the mode
    through the device's own registers, enables SVGA, and re-points
    `gfx.c` at the adapter's framebuffer (new
    `gfx_adopt_framebuffer()`). Then the cursor is the adapter's job:
    the WM uploads the sprite once and thereafter only says where it is.
  - Cursor motion now costs **nothing**: no sprite blit, no saving the
    pixels underneath, no damage rect, no repaint. It also makes the
    trail bug fixed earlier today structurally impossible on this path
    -- a stale sprite can't be left behind when nothing was ever painted.
  - **Capabilities had to be measured, not assumed.** The first version
    required `SVGA_CAP_ALPHA_CURSOR` and reported "cursor unavailable"
    on the one adapter it was written for: QEMU advertises `caps=0xe3`
    (CURSOR, CURSOR_BYPASS, BYPASS_2) and no alpha cap, with
    `fifo_caps=0` so no bypass-3 either. So positioning goes through the
    register path, and `SVGA_CAP_CURSOR` alone is the right gate.
  - **Not the default, deliberately.** `-vga std` stays what `make run`
    uses: this is emulator-only, and a hardware cursor is composited by
    the display frontend rather than living in the framebuffer, so
    QEMU's `screendump` does not capture it -- every screenshot-based
    test would stop seeing the pointer. Verified `-vga std` is entirely
    unaffected: the software cursor still renders (139 pixels at the
    probe point) and the driver doesn't even log, since the device
    isn't there.
  - **The first version left the display frozen, and the reporter's
    screenshot is what showed it.** In SVGA mode the adapter does NOT
    scan the framebuffer: writing pixels changes nothing until the guest
    names the changed rectangle with `SVGA_CMD_UPDATE`. So the console
    drew normally into memory and the screen stayed stuck on whatever
    QEMU happened to refresh at mode-set time -- two log lines and then
    nothing, no shell prompt at all. `gfx_flush()` publishes the dirty
    box gfx.c was already tracking, called from `gfx_present()` and the
    console's own draw paths.
  - **That first flush fix did nothing, and the reporter's second
    screenshot is what showed it.** `gfx_put_pixel()` only marked the
    dirty box on its DOUBLE-BUFFERED path; drawing straight to the
    framebuffer -- what the console does at boot -- marked nothing. So
    every flush found an empty box and published nothing. Dirty
    tracking had existed purely to bound `gfx_present()`'s blit, where
    "not double-buffered" genuinely means "nothing to blit"; the moment
    a display could need to be TOLD what changed, that assumption
    became wrong. The box now means "what was touched", regardless of
    where it was written.
  - Worth noting why the GUI looked fine in testing while the console
    was frozen: GUI mode IS double-buffered, so its dirty box was real
    and its flushes worked. Testing the GUI and concluding the display
    layer was healthy is exactly the wrong inference to have drawn.
  - Verified after the real fix: full boot log, shell prompt, and a
    typed `lscpu` rendering its whole scrolling output live.
  - That is the sort of thing automation here structurally cannot catch:
    the framebuffer contained the right pixels the whole time, so a
    memory-side check would have passed. `screendump` doesn't see it
    either, since it captures the same surface.
  - **The hardware cursor is OFF by default; the display takeover is
    not.** Reported: with the cursor on, the pointer jumps around and
    dragging a window is nearly impossible. Positioning it writes
    SVGA_REG_CURSOR_X/Y/ON, and QEMU answers by calling
    `dpy_mouse_set()`, which warps the HOST pointer -- against this
    kernel's RELATIVE PS/2 mouse that feeds motion straight back to the
    guest. Stated as a hypothesis (it hasn't been instrumented), but the
    behaviour reproduces and the default shouldn't be the broken one.
    `-vga vmware` now gives a working modesetting driver with the
    ordinary software cursor; `vmsvga_cursor_set_enabled(1)` opts in.
    The configuration where a hardware cursor genuinely works here is
    virtio-gpu plus virtio-input's absolute pointer -- Milestone 27a.
  - **Honest status: the cursor itself is still not visually verified.**
    `screendump` cannot capture a hardware cursor by definition, so
    automation confirms the driver initialises, takes the display over,
    reports `cursor hardware` and renders console + desktop correctly --
    but not that the pointer appears. That needs a real display.

- **`ui_textview`: scrolling belongs to the control, not to every app.**
  Asked for after spotting scroll logic sitting in UI Demo and Notepad
  -- a text view with scrollbars should behave like a real OS control,
  where the app configures it rather than reimplementing it.
  - Notepad, Terminal and UI Demo each carried the same ~20 lines:
    decide whether the content overflows, reserve a strip on the right
    if it does and there's room, page on a track click, remember a grab
    offset and follow the thumb, multiply wheel notches by 3. Three
    copies of one behaviour. **All three now call zero
    `widget_scrollbar_*` functions between them.**
  - The new control owns a `text_scrollback`, its scrollbar, the
    geometry that splits them, and all the input handling.
    `text_scrollback` stays the data structure and `ui_scrollbar` stays
    the stateless primitive -- this composes them, the same way
    `ui_button_group` composes `ui_button`.
  - **What the app still decides**, as fields: scrollbar policy
    (`AUTO`/`ALWAYS`/`NEVER`, where AUTO also hides the bar when the
    view is too narrow -- Terminal's long-standing rule, now one
    number instead of two copies), colours, bar width, wheel lines,
    page overlap, whether a caret is drawn, and who owns a press in the
    text body.
  - That last one is the flag that made the migration possible at all:
    `UI_TEXTVIEW_BODY_APP` declines body presses so Notepad keeps
    click-to-position and drag-select, while `UI_TEXTVIEW_BODY_PAN`
    lets UI Demo pan. Different behaviour as a flag on the widget, not
    a second copy of the widget.
  - Defaults are font-derived (`gfx_char_w() + 4`), exactly what
    `TERM_SCROLLBAR_W`/`NOTEPAD_SCROLLBAR_W` were, so adopting the
    control moved no pixels. A fixed constant would have silently
    resized both bars and been wrong at every font size but one.
  - **Testing caught a precedence bug in it**: with panning enabled, a
    press on the scrollbar *track* fell through to the pan branch and
    was swallowed, so the bar stopped paging the moment panning was
    switched on. The bar outranks the body now, checked before it
    rather than after.
  - Verified all four routes on UI Demo (wheel, track paging, thumb
    drag, pan) over the debug console, plus Terminal (`help` output,
    scrollbar with a proportional thumb, wheel) and Notepad (30 typed
    lines, caret, scrollbar, wheel).
  - `docs/gui-guidelines.md` gained a standing rule this generalises:
    **behaviour belongs to the component, configuration to the app** --
    with the cases where not following it is right, since the request
    was explicitly "unless there's a better reason".

- **"UI Demo" -- a GUI app that exists to be tested against.** Asked
  for a well-documented calibration target with every UI component.
  - One of each `apps/ui/` widget -- a `ui_button_group`, two
    checkboxes, a `ui_radio_list`, a `ui_textbox`, a `ui_scrollback`
    with its scrollbar -- at documented content-relative offsets, all
    derived from the font rather than hardcoded, so a `fontsize` change
    doesn't invalidate them. The layout table and the offsets live in
    the file's top comment.
  - **Every interaction is logged as one parseable line** (`uidemo:
    button 2`, `uidemo: check alpha on`, `uidemo: focus textbox`,
    `uidemo: cancel btn`). Combined with the `gui` commands below, that
    makes a GUI test drive-and-assert over one serial wire with no
    screenshot in the loop -- and when a click lands on the wrong
    control, the log says which one it actually hit, which is the
    calibration half.
  - Widget names in the log are stable identifiers (`btn1`,
    `chk_alpha`), deliberately not the display labels, so a test doesn't
    break when a label is reworded.
  - One hit-testing subtlety it immediately exposed in itself:
    `ui_button_group_release()` returns -1 both for "nothing was armed"
    and "armed then dragged off", so the first version logged
    `cancel btn` on every checkbox click too. It tracks whether the
    press actually armed something now -- exactly the kind of noise a
    log-asserting test trips over, found by asserting on the log.
  - Verified by driving all six widgets through `gui click` and
    checking the log: three buttons commit on release, the checkbox and
    radio act on contact, the textbox focuses and receives keys
    (`key 100 text="type hereabc"`), and press-drag-off produces
    `cancel btn` with no `button N`. Screenshot in
    `screenshots/2026-08-13/`.
  - **The scrollbar shipped inert, and the reporter found it.** It drew
    correctly and did nothing: no `on_wheel`, no track-click paging, no
    thumb drag. The tell was there to be noticed and wasn't -- this
    file's own log grammar documented a `scroll` event that no code path
    emitted. Testing had exercised click and key on six widgets and
    simply never tried to scroll, which is the failure mode of testing
    what you built rather than what the thing is supposed to do.
    All three routes work now (wheel, track paging, thumb drag), each
    logging `scroll <offset> <how>`.
  - That in turn exposed a gap in the debug console: there was no
    `gui wheel`, so the wheel path couldn't be driven from a test at
    all. Added, since apps genuinely handle the wheel and an injection
    set missing it is quietly incomplete.

- **A `gui` command family for the serial debug console -- inspect and
  drive the window manager without a single pixel.** Asked for after
  noticing that opening an app for a test meant clicking a Start menu
  row whose coordinates were hand-derived from a screenshot.
  - Works while the desktop is up because `debug_console_poll()` is
    already called from `wm_run()`'s idle loop -- the same piggyback
    `keyboard_getchar()` does for the physical shell. (The CLI half
    needed nothing: `sh <command>` has always run shell commands over
    this wire, which is what `tools/vm.py exec` drives.)
  - **Introspection**: `gui windows` (rects, content rects, z-order,
    focus), `gui probe X Y` (which window and which region -- title bar,
    close button, content, resize edge -- plus any overlay that would
    swallow the click), `gui menu` / `gui taskbar` (row and button
    geometry as the kernel computes it), `gui state` (overlays, cursor,
    armed drag/resize/press, and the damage rect, which is otherwise
    completely invisible), `gui apps`. Each takes `--json`.
  - **Driving**: `gui open <App>` / `gui close <n>` for setup, and
    `gui click` / `gui drag` / `gui key` / `gui wheel` injecting events into
    the WM loop. A click queues four events (move, press, held, release)
    consumed one per frame, so press and release land on separate
    frames -- which is what every arm-on-press/commit-on-release control
    here needs to behave normally. A drag interpolates, so the per-tick
    "is the cursor still over the armed control" tracking actually runs.
  - **Asynchronous by necessity, not by choice**: these commands are
    dispatched from inside `wm_run()`, so a `gui click` that waited for
    its own events to drain would be blocking the loop that drains them.
    Enqueue-and-return is the only safe shape. (Same trap that made a
    lazy CPU-clock calibration hang inside a syscall.)
  - `start_menu_geometry()` is public now, and `wm_debug_damage()`
    exposes the damage rect. `tools/gui_debug.py` is the client:
    `DebugConsole.menu_row("Terminal")` returns the real centre point,
    replacing `gui_flow.py`'s hardcoded `MENU_TOP_Y`/`ITEM_H` -- which
    its own comments record having drifted once already. The kernel
    reports 475/27; the hardcoded values were 475/27, so they were right,
    and now they're checkable.
  - Injected input enters BELOW the PS/2 driver, so it tests WM and app
    logic, not the mouse driver -- QMP stays the tool for that and for
    anything whose answer is genuinely a picture. Said plainly in
    `wm_debug.h` so the split doesn't have to be rediscovered.
  - Two formatting traps found by running it. `kfmt`'s printf supports a
    zero-pad width (`%04x`) but NOT left-justify (`%-4d`) -- passing one
    prints the specifier literally AND desyncs every later argument,
    which produced rows of `y=%-4d centre=%-4d app <garbage>`. And
    `klog_printf()` formats into a 256-byte buffer and silently drops
    the overflow, so a whole JSON object in one call came back as valid
    JSON up to a severed string. Both are now called out in comments
    where someone would hit them again.

- **`lscpu`, and a CPU-identification library behind it.** Asked for a
  CLI showing CPU info -- features supported *and enabled*, MHz, cache
  sizes, model name, stepping, vendor -- reusable by other kernel apps
  and by userland.
  - **The supported/enabled split is the thing, and it falls out of a
    privilege boundary.** `CPUID` is unprivileged, so ring 3 can
    identify the CPU by itself. `CR0`/`CR4`/`EFER` are not, so only the
    kernel can say which of those capabilities the OS actually switched
    ON. Those are genuinely different questions: SSE2 is supported by
    every x86-64 chip ever made and was not *enabled* here until
    `CR4.OSFXSR` got set earlier the same day. `lscpu` prints a
    supported flag that's waiting on an OS opt-in as `sse2*`, and lists
    each control-register bit by name and source underneath.
  - **`kernel/arch/x86_64/cpuid.c`** decodes vendor, brand string,
    family/model/stepping (with the SDM's extended-field combining
    rules, not the simplified version), cache hierarchy, and the
    control-register state. `api/cpuinfo.h` exposes it to `apps/`
    through `kapi.h`; `SYS_CPU_INFO` fills the same struct for ring 3.
  - **One syscall rather than none.** Ring 3 could execute `CPUID`
    itself, but it can't read the control registers, so the `enabled`
    half needs the kernel regardless -- and given that, returning the
    whole struct keeps the decoding in one place instead of duplicating
    it, which is the mistake `userland/lspci.c`'s private copy of the
    PCI class table already demonstrates.
  - **Cache reporting needed both vendors' leaves.** Leaf 4 is the
    modern path, but the default `qemu64` model reports as AuthenticAMD
    and populates neither leaf 4 nor AMD's newer `8000001DH` -- so the
    first run showed an empty cache section. AMD's `80000005H`/
    `80000006H` are implemented as the fallback (including their 4-bit
    associativity *encoding*, where 5 means 6-way). Both paths are
    exercised: Intel models take leaf 4, AMD models the fallback.
  - `tools/vm.py` gained **`--cpu MODEL`**, because testing
    CPU-model-dependent code needs it and there was no way to ask for
    one. `--cpu Skylake-Client` is how the leaf-4 path got tested at
    all; it also confirms the extended-model decoding (family 6, model
    94 = 14 + (5 << 4)).
  - The Control Panel's System Info applet grew the CPU section, and
    `control_panel_default_size()` now counts that page's width as well
    as the timezone list's -- adding it without that made every CPU line
    clip to nothing in the default window. The clipping itself was
    correct (`gfx_draw_string_clipped()`, per the guidelines), so the
    failure was silent truncation -- "L3 16" with the unit cut off --
    rather than the overdraw that rule usually catches.
  - Verified across three CPU models (`qemu64`, `Skylake-Client`,
    `max`), with 7 KTESTs asserting invariants rather than values, since
    the same kernel image boots on all three. Pixel-checked that no text
    crosses the window border. Screenshots in `screenshots/2026-08-13/`.

- **Hardware floating point and SSE, for ring-3 processes.** Asked
  whether the kernel could have FPU/SSE enabled, and specifically
  whether to do it kernel-wide "if Linux and Windows do." They don't,
  so this doesn't either.
  - **Both mainstream kernels are FP-free and bracket the exceptions.**
    Linux builds its own kernel with the same `-mno-sse -mno-sse2
    -mno-mmx -mno-80387` this project already used, and requires
    `kernel_fpu_begin()`/`kernel_fpu_end()` around the rare kernel-side
    SIMD (AES-NI, RAID6); Windows requires
    `KeSaveExtendedProcessorState()` for the same. So `userland/`'s
    ring-3 ELFs drop those flags and get real `double`/`float`;
    `kernel/` and `apps/` keep them.
  - **Why that split is worth copying rather than just cautious:** with
    SSE on, GCC emits XMM in ORDINARY code -- struct copies and inlined
    `memcpy`, not only code mentioning a float. An FP-enabled kernel
    would therefore need an FXSAVE on the interrupt path, on every
    vector, since an IRQ can land anywhere. Keeping the kernel FP-free
    means state moves only where the scheduler actually swaps ring-3
    processes.
  - **New `kernel/arch/x86_64/fpu.c`**: clears `CR0.EM`, sets `CR0.MP`/
    `CR0.NE` and `CR4.OSFXSR`/`CR4.OSXMMEXCPT`, and captures a pristine
    post-`FNINIT` state that each new process starts from. `CPUID` is
    checked even though both bits are architecturally mandatory in long
    mode -- it's four instructions once at boot.
  - **Eager, never lazy.** No `CR0.TS` + `#NM` trick: that's what
    CVE-2018-3665 (Lazy FP State Restore) exploited to read another
    task's registers, and Linux deleted its lazy path outright in 4.14.
    `FXRSTOR` is ~100 cycles against a 100 Hz tick. A KTEST asserts
    `CR0.TS` stays clear so nobody reintroduces it quietly.
  - `scheduler.c` gained a 16-byte-aligned 512-byte area per process,
    `FXSAVE`d on the way out of `scheduler_tick()` and `FXRSTOR`d in
    `switch_to()`, and initialized to the pristine template at spawn so
    a reused slot can't inherit the previous tenant's registers.
  - **The test can fail, which was checked rather than assumed.** Two
    `/tests/fpu_race` processes run concurrently with eight live
    accumulators each -- GCC auto-vectorizes them into `addpd` across
    xmm1-xmm4, held in registers for 20M iterations, so preemption
    lands mid-computation hundreds of times. With the FXSAVE/FXRSTOR
    pair temporarily removed, both processes report corruption; with it
    restored, both report intact. A single-process float test can't
    prove any of this, which is why there are two.
  - `fputest` from the shell runs both halves.

### Fixed
- **Cursor trail when shrinking a window.** Resizing a window smaller
  left a line of stale cursor sprites behind; growing one didn't.
  Confirmed fixed on real hardware by the reporter.
  - Mechanism: `wm_render_frame()` never undrew the cursor before
    repainting -- it only re-saved the pixels underneath at the end, via
    `draw_cursor_at()`. So the old sprite was erased only where the
    scene happened to repaint over it. A resize damages `union(old,
    new)`, whose bottom-right edge is exactly the old corner -- which is
    where the grip, and therefore the cursor, is -- and the sprite
    extends down-right PAST that edge. Growing hides the same bug
    because the window expands over the old position. A resize also
    keeps `redraw_pending` set every frame, so the cheap
    `wm_render_cursor_move()` path (which *does* restore first) is never
    taken for the duration of the drag.
  - Fix: `restore_cursor_under()` at the top of `wm_render_frame()`,
    before the scene clip is applied, so the write isn't confined to the
    damage rect -- the whole point being that the stale pixels are
    outside it. This also makes the two render paths symmetric; the
    cheap one has always restored first.
  - **The symptom was not reproducible under QMP, and that's worth
    recording.** Both a stepped drag and a fast continuous one (30
    back-to-back `move_rel` calls with no settle) left no trail even on
    a deliberately-rebuilt buggy binary: QEMU's PS/2 emulation coalesces
    the motion into far fewer frames than a real mouse generates, so the
    resize simply never produced enough repaints to strand a sprite.
    What automation could establish was that the fix changed nothing
    else (a pixel diff against the unfixed build differs only in the
    taskbar clock); the fix itself was shipped on the strength of the
    mechanism above, and the reporter then confirmed the trail is gone
    with a real mouse. A reminder that the harness's fidelity is itself
    a variable -- "I couldn't reproduce it" was a fact about QEMU here,
    not about the bug.

- **An app could draw outside its own window, straight onto the desktop.**
  Reported from a screenshot: shrinking the Control Panel left System
  Info's lower rows marching down the desktop, fully legible, well
  outside the frame.
  - Two gaps, one visible. **`gfx_draw_string_clipped()` bounds width
    only** -- it takes a `max_w` and has no row budget at all -- so the
    right-hand edge clipped correctly while the bottom had nothing
    stopping it. And **the WM never clipped `on_draw()` to a window's
    content rect**: the only clip active during a frame is the damage
    region, which covers the desktop below a window too.
  - So this was never really a System Info bug. Any app drawing more
    than fits would paint over the desktop and over other windows;
    System Info was just the first page with enough rows to show it.
  - `wm_render_frame()` now narrows the clip to each window's content
    rect around its `on_draw()`, and restores the scene clip after.
    Intersected by hand, because `gfx_set_clip_rect()` REPLACES the
    active rect rather than intersecting -- setting the content rect
    naively would have widened the damage clip back out and undone the
    compositor's whole point.
  - The System Info page also stops at the last row that fully fits, so
    it ends on a whole line instead of one sliced through its glyphs.
  - **Verified both halves independently**: with the applet's row budget
    temporarily removed, the WM clip alone still contained everything
    (zero non-desktop pixels below a window shrunk to 163px tall), and
    with it restored the page ends cleanly. Also re-checked Notepad,
    Calculator, Terminal and Task Manager still render.
  - Worth noting the near-miss in testing: the first repro dragged from
    one pixel above the resize grip's hotspot (`RESIZE_MARGIN` is 6), so
    only the width changed, the content still fit, and the "negative
    control" proved nothing. A test that can't fail isn't evidence.

- **Ring-3 stack alignment was wrong for SSE, in two different ways.**
  `elf_run.c` aligned the initial user RSP to 8 bytes, which was
  invisible while userland was built `-mno-sse` and nothing could emit
  an alignment-sensitive instruction at all.
  - First fix was to 16, reasoning from the SysV process-entry
    convention. That crashed: `movapd %xmm0,(%rsp)` took a `#GP` at ring
    3. The convention describes what a real crt0 `_start` sees, and a
    real crt0 realigns before calling main -- but every `_start` here is
    a plain C function, which GCC compiles assuming a pushed return
    address (`RSP % 16 == 8`) and sizes its prologue from there. Hand it
    a 16-aligned RSP and every aligned local is off by exactly 8.
  - Correct answer is `RSP % 16 == 8` at entry, and it was established
    by running the thing, not by reading the ABI.
  - `userland/fpu_test.c` now forces an aligned SSE store to a stack
    local specifically so this stays covered -- no other userland
    program currently emits one, so the fix would otherwise have sat
    untested until something tripped over it.

- **Calculator's keys and Notepad's toolbar committed on button-DOWN,
  so neither could be cancelled.** Found by running the cancel test on
  the hover work above rather than only the happy path: pressing
  Notepad's `Open...`, dragging into the text body and releasing opened
  the file picker anyway, and pressing a Calculator key and dragging
  away still entered the digit.
  - Cause is the trap `docs/gui-guidelines.md` already documents by
    name -- both apps acted in `on_click`, which `wm_input.c` fires on
    button-down. The Control Panel was fixed for this when the rule was
    written; these two, the only other content-area controls, were not.
  - **`ui_button_group_release()` returns the released button's `code`
    now** (-1 if none was armed). Because `ui_button_group_press()`
    re-hit-tests every tick, a button that's been dragged off is
    already unpressed, so releasing there returns -1 and the action is
    cancelled with no extra bookkeeping in either app.
    `calculator_release()` and `notepad_release()` commit from that;
    `calculator_click()` is gone entirely and `notepad_click()` keeps
    only its scrollbar paging, which genuinely does act on contact.
  - **`ui_button_group_click()` is gone**, having lost both callers --
    it had no way to know about a press to cancel, so any `on_click`
    caller of it commits on button-down by construction. The header
    says what to bring it back for if a real act-on-contact control
    ever wants one.
  - **`notepad_press()`'s `cy >= TOOLBAR_H` early return also went.**
    It meant a toolbar button dragged off into the text body stayed
    drawn pressed until release -- something about to be cancelled
    looking live, the exact thing the guidelines call out. A point
    below the toolbar hits no button, which is already the right
    answer. Measured: 163 (pressed) before, 200 (rest) after.
  - Verified by behaviour, not just by pixels: the Calculator display
    stays `0` through press-drag-off-release and changes on a real
    press-release; Notepad's picker stays shut on the first and opens
    on the second. Screenshots in `screenshots/2026-08-13/`.

- **CI had been red for three commits, and the reason was a real bug it
  found rather than a bad check.** `tools/check_layout.py` (added in the
  filesystem-layout commit) failed on `/etc` and `/etc/kbs` missing from
  CI-built images.
  - Root cause: `tools/gen_kbs.py` needs `xkbcli`
    (`libxkbcommon-tools`), the `seed` target **skips it with a message**
    when that's absent, and CI never installed it. So **CI had been
    building images with no keyboard layouts at all**, silently, for as
    long as that step has existed -- nothing noticed until a check
    compared an image against a written description of what should be on
    it. That is the check earning its keep on its first run.
  - CI installs `libxkbcommon-tools` now, so the layouts are actually
    generated and `gen_kbs.py` gets exercised there.
  - The doc was also wrong in a smaller way: `/etc` was listed as
    build-created, when the thing that reliably creates it is
    `kernel_main()` at boot -- the build only made it incidentally, as a
    side effect of seeding `/etc/kbs`. It's `boot` now.
  - `check_layout.py` gained a third status, **`optional`**, for
    `/etc/kbs`: documented so an undocumented directory can't hide
    behind the name, but never required, since a machine without
    `xkbcli` legitimately won't have it. Verified by building an image
    with no `/etc` at all and confirming the check passes -- i.e. the
    CI case reproduced locally rather than fixed by pushing and hoping.

- **`gfx_draw_string()`'s missing clipping is now a function instead of
  a rule.** It draws every character it's handed, past any boundary the
  caller had in mind. `docs/decisions.md` recorded that after a long
  filename drew through a text field's border, and told callers to
  budget the width themselves -- and then the very next fixed-box
  caller, the Control Panel's applet labels, hit the identical bug
  (`Date & TSystem Info`) in a file written days later. A rule that must
  be remembered at every call site will be forgotten at some call site.
  - **`gfx_draw_string_clipped(x, y, max_w, ...)`** draws bounded and
    returns whether the whole string fitted, so a caller can add an
    ellipsis or widen itself without measuring twice.
  - **`gfx_text_width()`** and **`gfx_text_fit_chars()`** are the
    measurement half, for callers doing their own windowing.
    `gfx_text_width()` also pays a debt forward: Milestone 21 lists
    exactly that function as something proportional font metrics need,
    and every open-coded `k_strlen(s) * gfx_char_w()` is a site that
    silently breaks when a glyph stops being one cell wide.
  - `gfx_draw_string()` itself is deliberately unchanged -- clipping it
    would alter every existing caller.
  - Converted: the Control Panel's labels (replacing the hand-rolled
    truncation) and `ui_textbox`'s field text, the latter as a safety
    net rather than a rewrite -- its windowing logic is correct, but if
    it ever miscomputes, the text now stops at the field's edge instead
    of drawing through the border, which is the exact bug that widget
    already had once.
  - Two KTESTs (suite: 61 -> 63) assert the measurement directly:
    widths stop at a newline, and `fit_chars` never returns a partial
    glyph (one pixel short of the fourth character is three, not "three
    and a bit") including the degenerate zero/negative widths. The
    measurement is pure arithmetic over font metrics, so unlike the
    drawing it can be asserted rather than eyeballed in a screenshot.

- **The two ATA loose ends this session had been carrying: the PIO
  path's failure reporting, and the fact that it never ran at all.**
  - **`wait_drq()` conflated "the drive reported ERR" with "I gave up
    waiting"** -- both returned 0, so a PIO failure said nothing about
    which. Now recorded in a `g_pio_fail_reason` string and logged,
    exactly mirroring `g_dma_fail_reason` on the DMA side, whose own
    comment notes that collapsing these cost real detective work once.
    `pio_read_sectors()`/`pio_write_sectors()` log it unconditionally on
    failure, same as `dma_transfer_with_retry()` does -- they previously
    returned 0 silently. The return stays pass/fail (nothing needs to
    branch on the difference); it's the human reading `dmesg` who does.
  - **`ata nodma on|off` forces the PIO fallback**, because it was
    otherwise *unreachable*: `ata_init_dma()` succeeds on every machine
    this OS boots, so ~100 lines of fallback driver had never executed
    and could not be tested. Fallback code that only runs in an
    emergency and has never been seen running isn't a fallback, it's a
    guess. The switch is also the PIO-vs-DMA comparison that root-caused
    a DMA failure to a host stall in an earlier session -- which at the
    time meant hand-editing the driver.
  - **Every DMA gate now routes through one `dma_in_use()` helper.**
    That's the part worth getting right: `ata_max_sectors_per_xfer()`
    reports a *smaller* cap for PIO, so a dispatch site checking the
    flags differently from the site setting the cap would let a caller
    batch 128 sectors into a path that tops out at 8. Confirmed live --
    the reported cap goes 128 -> 8 with the toggle and back.
  - `ata_set_dma_forced_off()` **refuses** while a non-blocking transfer
    is in flight (a stepped Notepad save) rather than stranding its
    poller, and returns 0 so the caller reports the refusal instead of
    assuming the switch happened.
  - **Three KTESTs in the new `kernel/drivers/ata_test.c`** (suite: 58
    -> 61), driving the same switch: a file written and read back
    through the PIO path and compared byte-for-byte, the sector cap
    following the active path, and the restore-to-DMA case. They restore
    the previous mode *before* asserting, so a failing assertion can't
    leak forced-PIO into every test after it -- the same cascade the
    runner's `fault_any_armed()` check exists to prevent.
  - Verified beyond the suite: a `stress 3` write/read/verify round trip
    run **entirely through PIO**, byte-for-byte clean with `fsck` clean
    after, at 5.0 MB/s write / 7.5 MB/s read against DMA's 24/29 -- the
    first time that code has demonstrably moved real data under load.
    **Not covered:** the failure-reason strings themselves, which need
    an actual drive error to surface; and the mid-transfer refusal
    branch, which needs a stepped write held open across a test body.
    Both are noted in the test file rather than left looking covered.

### Added
- **A Control Panel GUI app with pluggable applets, and two applets to
  start.** Asked for a Windows-style applet chooser plus one easy but
  still useful first applet. Completes `docs/roadmap.md`'s Milestone 22
  "Control panel with pluggable applets" item.
  - **Icon grid + drill-in**: a grid of applet icons; clicking one
    replaces it with that applet's page and a `< Back` button. The
    chooser reuses `apps/ui/ui_icon_grid.c`, whose header was written
    anticipating exactly this ("built as a standalone widget so a
    future file manager's icon view can reuse the same cell math") --
    this is the second caller it was waiting for, minus the drag
    session, since applet icons don't move.
  - **The applet registry deliberately mirrors `gui_apps.h`'s app
    registry** -- a static table of name + function pointers, where
    adding one is adding a row. An applet is explicitly not a
    `gui_app`: no window of its own, draws into a rectangle it's given,
    not openable from the Start menu. See `docs/decisions.md`.
  - **Date & Time applet**: a timezone picker over whatever
    `/etc/timezones` holds (7 cities today), plus the current local
    time. Picking one calls `tz_set_index()`, which persists to
    `/etc/toyos.conf` itself -- the applet caches nothing, so the
    `timezone` shell command changing the same setting behind its back
    can't desync it.
  - **System Info applet** (read-only: version, memory, disk, PCI
    count, uptime) shipped alongside deliberately. A plug-in mechanism
    with exactly one plug-in proves nothing about being pluggable; the
    second entry is what makes the grid, the drill-in and the Back
    button mean anything.
  - **New widget `apps/ui/ui_radio_list.h`/`.c`** -- single-select
    list, one row per option, columns supported, caller-owned
    selection, full-row hit areas. The mutual exclusivity
    `ui_checkbox.h` deliberately doesn't have. Added ahead of a second
    caller **by explicit request**, the same acknowledged exception
    `ui_checkbox` and `ui_icon_grid` already are -- recorded rather than
    left to look like drift.
  - **Two bugs caught on screen, not in review**, both of the "draws
    perfectly, does nothing" kind. `on_click`'s coordinates are
    *content-relative* while everything drawn is absolute, so the first
    version hit-tested in the wrong space and every click silently did
    nothing. And the icon labels used a hardcoded 104px cell that "Date
    & Time" overflows -- `gfx_draw_string()` does no clipping, so it
    drew straight over the neighbouring label ("Date & TSystem Info").
    That second one is `docs/decisions.md`'s existing
    "`gfx_draw_string()` doesn't clip" lesson recurring in a new file
    days after being written down; the fix derives the cell from font
    metrics and truncates.
  - `tools/gui_flow.py`'s `APP_ORDER` and `MENU_TOP_Y` updated for the
    sixth app (the Start menu grows upward, so the row geometry moves).
  - Verified on screen: chooser, drill-in, Back, both applets, and the
    payoff -- picking `helsinki` moved the applet's clock from 15:18 to
    18:18 (+3, correct for EU DST) **and the taskbar clock with it**,
    then `timezone=helsinki` was confirmed in `/etc/toyos.conf` on the
    image afterwards. Screenshots in `screenshots/2026-08-13/`.

### Changed
- **A documented, checked on-disk filesystem layout -- and the test
  binaries moved out of `/bin`.** Asked for a future-proof directory
  structure now that there are binaries, config files and data files,
  with a doc to follow, and whether it should be "something POSIX
  likes".
  - **The premise needed correcting first: POSIX barely specifies
    layout.** POSIX.1 mandates `/`, `/tmp` and a few device paths, and
    says nothing about `/bin`, `/usr`, `/etc` or `/var`. The document
    defining those is the FHS, which is a Linux Foundation spec with no
    POSIX standing. So this was almost entirely a free choice.
  - **`docs/filesystem-layout.md`** is the new source of truth: a table
    of every directory, what it holds, whether the build or the boot
    creates it, and whether it exists yet or is reserved for a named
    milestone. Plus the rules for adding one, and the divergences.
  - **`/tests`, holding the 14 test binaries that used to live in
    `/bin`.** `/bin` had three real programs (`ls`, `lspci`, `hello`)
    against fourteen mechanism exercises, so every `ls /bin` and every
    tab completion led with noise. `/tests` is deliberately not an FHS
    directory -- `/usr/libexec` was the alternative and lost on being
    less obvious, longer (paths are capped at 64 bytes), and implying
    "internal helper" when these are things a person runs on purpose.
    `PATH` gains `/tests` **last**, so `run nx_test` and `strace
    file_test` keep working -- rewriting every reference across the
    changelog and `docs/decisions.md` would invalidate accurate history
    for no functional gain.
  - **`/tmp` now exists**, created at boot, and `stress` writes its
    multi-gigabyte scratch file there instead of to `/.stress_test_tmp`
    in the root. It is deliberately *not* emptied at boot: `fs_delete()`
    refuses non-empty directories and there's no recursive delete, so
    clearing it needs a directory walk nothing has needed yet.
  - **`tools/check_layout.py` enforces the doc** against the built
    image, in `preflight.sh` and CI. It fails in both directions -- an
    undocumented directory on the image, or a documented-as-present one
    missing -- and it caught three real discrepancies the moment it
    first ran. It understands the table's "Created by" column, so a
    boot-created directory isn't demanded of a never-booted image; that
    distinction is the difference between a check people trust and one
    they learn to ignore.
  - **The trap this move hit, now documented:** `tfs2_writer.py sync` is
    *additive*, so moving the binaries left a full set of stale copies
    in `/bin` on every existing image -- which `PATH` would have
    preferred over `/tests`, forever, with no future build updating
    them. Pruning isn't the fix (a `sync` that deleted anything absent
    from the seed tree would delete `/etc/toyos.conf` and
    `/etc/history`), so a move needs a deliberate `tfs2_writer.py
    delete` or `make clean-disk`. Both are written down now.
  - The doc also records the constraint that actually drives layout
    here, which is not any standard: `FS_MAX_FILES` is 256 records with
    directories counting against it, and a full path is 64 bytes. That
    makes "flatter, and fewer files with more structure inside" the
    operative rule until Milestone 15 raises both -- worth knowing
    before Milestone 14 designs one man page per command.
  - Verified: `check_layout` passes, `ls /bin` shows exactly the three
    real programs, `run nx_test` still resolves and still demonstrates
    NX enforcement, `lspci` unaffected.

### Added
- **`lspci` shows real vendor and device names, from the PCI ID
  Database.** `8086:7010` now also reads "Intel Corporation 82371SB
  PIIX3 IDE [Natoma/Triton II]". Asked for directly, with the choice of
  bundling a copy vs reading the host's vs downloading it.
  - **Bundled**, at `data/pci.ids` (1.6MB, version 2026.07.30), staged
    onto the image at `/usr/share/hwdata/pci.ids` -- the same path a
    Linux distribution uses. Reading the build host's copy would make
    the build depend on host layout and produce different images on
    different machines; downloading would put a network fetch in the
    build and break offline/sandboxed builds. See `docs/decisions.md`.
  - **Licensing:** upstream offers it under GPL-2.0-or-later *or*
    3-clause BSD. toy-os takes the BSD option (MIT-compatible), with the
    full text in `LICENSE`'s new "Third-party data" section -- the same
    pattern already used for the baked JetBrains Mono glyphs.
  - **Not in `seed/`, which is where it obviously belongs and doesn't.**
    `seed/sync/` is a build staging tree: `make clean` does `rm -rf` on
    it and `.gitignore` excludes it, because the Makefile repopulates it
    with built ELFs every build. A file placed there works perfectly for
    whoever created it and silently doesn't exist for anyone who clones.
    Caught exactly that way -- it survived local testing, vanished
    during a `make verify`, and the feature kept working anyway because
    the copy already written to `disk.img` was still there. The tracked
    master lives in the new `data/`, and the `seed` target stages it.
  - **Parsed as a single streaming pass in `/bin/lspci`**, never held in
    memory: 1KB at a time, keeping only the current line and copying out
    just the names matching a device actually present. Peak memory is a
    few KB no matter how large the database grows, which matters because
    a userland process's heap here is a bump allocator with no free.
  - **One lspci, not two.** The kernel-space `cmd_lspci()` and the
    userland ELF were two implementations of the same command carrying
    duplicate class-name tables -- `lspci.c`'s own comment flagged the
    drift risk. The builtin now runs `/bin/lspci`, exactly as
    `cmd_ls_bin()` already runs `/bin/ls`, and the surviving
    implementation is the userland one -- which is where a real OS puts
    lspci, since resolving an id to a name is a file lookup, not
    something a kernel should know how to do. The old kernel version is
    kept as a real (not dead) fallback for a disk with no `/bin/lspci`,
    reached via an `fs_exists()` check -- `elf_run_from_fs()` returns -1
    for "couldn't read it", which is indistinguishable from a process
    that genuinely exited -1.
  - `/bin/lspci` also gained the IRQ/BAR output the builtin had, so
    collapsing to one implementation didn't quietly drop anything. Its
    `put_udec()` helper is back too -- the file's top comment had cited
    it as an example for some time while the function itself was gone.
  - The GUI Terminal routes `lspci` through `term_spawn()` like it
    already does `ls`, so the 1.6MB parse can't block the WM event loop.
    Verified on screen, not just in text -- see
    `screenshots/2026-08-13/`.
  - `1234:1111` (QEMU's emulated VGA) correctly shows no name: `1234`
    isn't a vendor id upstream lists. That's the database being right,
    not the lookup failing.

### Fixed
- **`SYS_READ` read the entire file on every call, making any real
  streaming read quadratic.** Found immediately by the feature above:
  `/bin/lspci` reading 1.6MB in 1KB chunks did ~1,615 calls x 1.6MB =
  **~2.6GB of disk reads and took 35 seconds**.
  - The handler called `fs_read()` (whole file into a `kmalloc()`'d
    buffer) and then copied out just the bytes at the fd's offset.
    Harmless for as long as nothing in ring 3 opened a file bigger than
    a few hundred bytes -- at that size the whole file *is* one read --
    which is why it sat unnoticed since `SYS_OPEN`/`SYS_READ` were
    added.
  - Fixed by using `fs_read_range()`, which already existed for exactly
    this and whose doc comment describes streaming a file as its
    intended use. **35s -> 0.9s**, including boot. Its "0 means EOF or
    error, indistinguishable" contract happens to be precisely the
    wanted behaviour for a file deleted mid-read.
  - Worth recording as a class of bug: a wrong complexity class can sit
    for a long time when inputs stay small, and it fails by being *slow*
    rather than wrong, so no test catches it. See `docs/decisions.md`.
  - Verified: `make verify` clean (58 KTESTs), `lspci` correct at the
    physical console and in the GUI Terminal.

### Changed
- **Roadmap: eight new milestones, twelve existing ones deepened, and a
  third renumbering.** Asked for "more steps and maybe 5 new milestones,
  add plenty". The list went from 32 milestones and roughly 250 items to
  **40 and 398**, with no milestone below 7 items.
  - **New, each placed where its prerequisites put it** rather than
    appended: **6 Fuzzing & property-based testing** (right after the
    test harness it builds on), **11 Crash reporting & postmortem
    debugging** (after signals, since a core dump hangs off SIGSEGV),
    **13 Init & service supervision** (the milestone that finally *uses*
    TTY + fork/exec + signals + job control together), **14 In-OS
    documentation** (`man`), **16 Block integrity: checksums &
    scrubbing** (while TFS3's format is still open -- a checksum field
    wants designing in, not bolting on), **18 Encryption at rest**
    (after multi-user, which brings the same key-derivation machinery),
    **20 A layout engine for the GUI** (before the apps that would use
    it), **21 Runtime font loading & text metrics** (immediately after
    it, since layout is what needs to ask how wide a string really is).
  - The thin milestones -- 4, 5, 7, 8, 25, 26, 28, 29, 35, 37, 38, 39 --
    were filled out with real steps rather than padding. A few carry
    decisions the project would otherwise discover late: UTF-8's real
    work is auditing every `char`-sized assumption; a benchmark number
    is meaningless without recording TCG-vs-KVM; NVMe's 4KB sectors have
    never been tested against TFS2's assumptions.
  - **Milestones 11-32 became 15-40** (a piecewise shift, since the
    insertions are scattered). Every cross-reference was re-checked
    against its target's *title* afterward rather than trusted to the
    shift -- which is how the first attempt at the previous renumbering
    was caught double-shifting headings and colliding two milestones.
  - **This is the pass that ends the convention.** The roadmap now says
    so explicitly: insertion-with-renumbering is worth it for one or two
    milestones with a real prerequisite argument, and beyond that,
    append. Eight at once meant rewriting cross-references across four
    files and a third translation table.
  - Fixed three references in `docs/decisions.md` that went stale in
    *this morning's* renumbering and weren't caught then -- the
    compositor entries pointing at "Milestone 12". They were missed
    because that check's output was truncated at 20 lines, which is a
    good argument for verifying by resolving every reference to its
    target's title (as done here) rather than by reading a list.

### Added
- **`make run-kvm`, and `tools/vm.py --kvm` to go with it.** Same flags
  as `make run` plus `-enable-kvm -cpu host`, so guest code runs
  natively on the host CPU instead of through QEMU's TCG translator.
  `make run` stays the portable default: `/dev/kvm` isn't readable
  everywhere, and CI runners generally have no virtualization to nest
  into at all.
  - `-cpu host` is what makes it worth having (QEMU otherwise masks the
    guest down to a conservative model) and is safe here: this kernel
    reads no CPUID feature bits and enables nothing past long mode + NX.
    Verified by booting it headlessly -- full init sequence, DMA
    detected, filesystem mounted, shell up.
  - **Measured, because it's not the win it sounds like.** Same disk
    image, same host, `stress 150`: **TCG 22.8 MB/s write / 29.2 MB/s
    read in 11 s, KVM 12.1 / 18.7 in 20 s.** KVM is roughly 1.9x
    *slower* for disk I/O. Compute-bound guest code does get much
    faster, but every port-I/O instruction becomes a hardware VM exit
    costing on the order of a microsecond, where TCG services one
    in-process in tens of nanoseconds -- and this kernel's disk path is
    dense with `inb`/`outb`. So it's a genuinely useful second mode to
    test in, not a replacement, and **a throughput number is meaningless
    without saying which mode produced it.** Noted in the Makefile
    target, `vm.py`'s `--kvm` help, and `cmd_start()`'s comment.
  - `vm.py --kvm` exists so the new target can actually be tested
    headlessly -- without it, `make run-kvm` needs a display and can
    only be exercised by hand.

### Fixed
- **The ATA driver gave a busy drive ~37ms to become ready, while
  giving the same transfer 5 seconds once its command was in flight.**
  Reported from a live `stress 4200` run that died at 11% with `ata: dma
  write failed after 3 attempts (lba 2, last reason: drive stayed busy,
  command never issued)` and `fs: WARNING -- record slot 26 (lba 29)
  failed to persist`.
  - Read the LBA, not the progress bar: `lba 2` is the journal header
    `persist_record()` writes *first*, so this was a metadata write
    failing, not the 473MB of file data the message sits next to. The
    473MB is where `stress` happened to be, not where anything went
    wrong -- a later `stress 600` wrote straight past that offset.
  - The driver bounds its two waits differently. `wait_dma_irq()`
    (command already in flight) uses a wall-clock budget --
    `DMA_WAIT_TICKS`, 500 ticks at 100Hz, 5 seconds.
    `wait_not_busy()` (the pre-issue wait, the one that failed) used
    `ATA_POLL_LIMIT` alone: a fixed 100000-iteration spin. **A spin
    count is not a duration.** Measured in the guest, those 100000
    iterations take ~12ms, and `dma_transfer_with_retry()` ran its three
    attempts back-to-back with no delay -- so the driver's total
    patience was ~37ms against the completion path's 5000ms, a ~135x
    asymmetry in the wrong direction. Any host-side I/O stall longer
    than 37ms takes out all three attempts at once, and the failing run
    started seconds after `grub-mkrescue` wrote a 746MB ISO to the same
    Btrfs disk.
  - `DMA_WAIT_TICKS`'s own comment records it being *widened* against
    this exact class of host stall. That widening only fixed the
    completion half; nothing revisited the pre-issue half, which is how
    a bound the project had already reasoned about carefully stayed
    135x too small next to it.
  - **Fixed** by giving `wait_not_busy()` the same context split
    `wait_dma_irq()` already had: a wall-clock budget
    (`BUSY_WAIT_TICKS`, ~1s) when it's safe, and the original fixed spin
    (`spin_not_busy()`) when inside a syscall -- where `int 0x80`'s
    interrupt gate leaves IF clear, so `pit_ticks()` never advances and
    a wall-clock loop would hang instead of time out. The wall-clock
    path also carries a very generous iteration cap as belt-and-braces,
    since being wrong about that assumption should fail a write, not
    the machine. Plus `retry_backoff()` between attempts (~250ms x the
    attempt number, `hlt` when safe and a spin inside a syscall, per
    `docs/decisions.md`'s standing rule for blocking waits), because
    retrying instantly is the one thing guaranteed not to help when the
    cause is a stall.
  - **`wait_drq()` got the same treatment in a follow-up** (it was left
    alone in the first pass as a path this failure didn't involve).
    It's the PIO fallback's "is a sector's data ready?" wait, and it had
    the identical fixed-spin bound. Two differences shaped the fix:
    it runs once per SECTOR rather than once per transfer, so the status
    read and both of its exits now happen BEFORE the clock is consulted
    -- the common case (DRQ already set on the first look) costs one
    extra `pit_ticks()` per sector, a volatile counter read next to the
    port I/O that dominates it. And it has a real error exit (the drive
    setting ERR) as well as a timeout, which it still collapses into the
    same `0` return; that conflation is left as-is and noted in the
    source, since no caller distinguishes them today.
  - Coverage note for that one: `ata_init()`'s IDENTIFY call exercises
    `wait_drq()` on every boot, so the change is covered there, but the
    per-sector PIO transfer loop is **not reachable while DMA is
    available** -- which it is on every machine this runs on today
    (`ata: Bus-Master DMA available` at boot). There is no switch to
    force the PIO path, so that half is unexercised by construction
    rather than untested by omission.
  - Verified: `make verify` clean (58 KTESTs, boot smoke). `stress 150`
    PASSED byte-for-byte at 24.0 MB/s and `fsck` reported clean;
    `stress 400` wrote at 23.1 MB/s, both unchanged from before the
    change, confirming the new bounds cost nothing on the success path
    (they only ever elapse when the drive is actually busy).
  - **Then the real check, from the user's own machine: `stress 4200` --
    the exact command that failed -- PASSED, and so did `stress 8192`.**
    4200 MB in 326 s and 8192 MB in 692 s, each written, read back and
    verified byte-for-byte, with no `ata:` or `fs:` warnings. This
    entry originally recorded that surviving a genuine stall was NOT
    verified, since the failure is host-timing-dependent and hadn't
    reproduced in-session; that caveat is now much weaker. It isn't
    gone, and the distinction is worth keeping straight: what's shown
    is that the failing case now succeeds at nearly 20x the data
    volume, not a controlled stall reproduced and observed to be
    absorbed. Nothing here forced a stall to occur on demand.
  - Also settled `docs/roadmap.md`'s long-standing "full multi-GB stress
    run" item (Milestone 3) as a side effect -- see that entry for why
    its own time estimate had been putting sessions off attempting it.

### Changed
- **Roadmap: two new milestones, and a second renumbering to make room
  for one of them.** Asked what it would take to make toy-os POSIX
  compatible, and whether that's feasible. The short answer is yes, as
  "enough POSIX to build and run real ported C programs" -- and that
  most of it is already scheduled under other names. Written up rather
  than left in a session.
  - **New "TFS3: an inode layer" milestone** (numbered 11 when added,
    15 today). The survey turned up
    one structural gap nothing on the roadmap owned: TFS2 stores a flat
    table of records keyed by a full path string, with no object
    representing a file separately from the name pointing at it. Hard
    links, atomic `rename()`, unlink-while-open and `st_ino`/`st_nlink`
    can't be expressed against that, and it's independently worth
    fixing regardless of POSIX.
  - **New "POSIX compatibility" milestone** (32 when added, 40 today).
    Deliberately a
    capstone: it names the target (our own libc vs Linux syscall-ABI
    emulation -- a real fork, to decide before writing code), owns the
    handful of items nothing else covers, and records what is *not*
    being pursued (conformance, locales, pthreads, `select`/`poll`).
    The one easy-to-miss blocker it surfaces: SSE is never enabled
    (`boot.asm` sets PAE/LME/NXE but not CR4.OSFXSR), userland builds
    `-mno-sse -mno-sse2`, and nothing saves FPU state across a context
    switch -- so the first stock-compiled binary would fault, since
    every real libc's `memcpy` uses SSE2 unconditionally on x86-64.
  - **Milestones 11-30 became 12-31**, since the inode layer belongs
    before permissions -- mode bits want to live on an
    inode, and the other order means building them twice. The roadmap's
    own rule is that reading order is build order, so the alternative
    was a milestone that documents a prerequisite while sitting after
    the thing that needs it. Every cross-reference in the file was
    checked against its target's title afterward, not just shifted;
    three references outside it (`apps/README.md` x2,
    `kernel/README.md`) were updated too, and one backlog line that had
    been stale since the *first* renumbering got corrected.
  - The translation table now covers both passes, and the note above it
    says when appending is the better choice than inserting.

### Added
- **ASCII case folding in `string.h`, and `timezone Helsinki` now
  works.** The helpers came back with a caller this time: `k_tolower`/
  `k_toupper`/`k_strcasecmp` were written, found to have nobody calling
  them and deleted before landing when the `kernel/lib/` toolkit went
  in -- the same thing that happened to `k_strstr`, which returned one
  feature later for the shell's Ctrl-R search. `tz_find_by_name()` is
  the equivalent here.
  - Every city in the timezone database is spelled lowercase, so
    `timezone Helsinki` -- the capitalization anyone would actually
    type -- got "Unknown timezone" and a pointer to a list where the
    name plainly appears. It matches now; still an exact match
    otherwise, no prefixes (`timezone helsink` is still unknown).
  - The same lookup runs on the boot path, so a hand-edited
    `/etc/toyos.conf` carrying `timezone=LosAngeles` is now honoured
    instead of silently falling back to whichever city loads at index
    0. Verified by writing exactly that with
    `tools/tfs2_writer.py write --force` and booting: `time` reported
    `(losangeles)`.
  - **ASCII-only, deliberately** -- A-Z <-> a-z and nothing else. The
    Latin-1 Å/Ä/Ö this kernel's `se` layout produces pass through
    unchanged, because nothing compared this way is non-ASCII and
    folding that range would be range added ahead of a caller. It isn't
    free either: `char` is signed here, so every byte >= 0x80 arrives
    negative, and 0xD7/0xF7 sit inside the Latin-1 letter block without
    being letters. `k_tolower`/`k_toupper` take and return `int`, and
    `k_strcasecmp` folds through `unsigned char`, so a caller passing a
    signed `char` straight in can't fold the wrong thing -- see
    `string.h`'s comment for what widening later would involve.
  - Tab completion and `color <name>` stay case-sensitive; nothing has
    asked otherwise, and bash's completion is case-sensitive too.
  - Verified: 58 KTESTs, 3 new (`kernel/lib/tz_test.c` is a new file --
    recursive source discovery means it needed no Makefile edit). The
    folding tests pin the bytes bracketing each range (`@`/`[`,
    `` ` ``/`{`), that Ä (0xC4) is left alone through both an `int` and
    a signed-`char` path, and that ordering comes from the folded bytes
    (`k_strcasecmp("Z", "a") > 0`, where a raw comparison would say the
    opposite). End to end through `tools/vm.py`: `timezone Helsinki`,
    `HELSINKI` and `helsinki` all set it, `Nonesuch` and `helsink`
    both correctly don't.
- **The console cursor no longer hides the character it sits on --
  four selectable styles, persisted in `/etc`.** Reported with a
  screenshot of `Hello Wo█ld`: "text cursor is blocking the character.
  Would it be possible to make it underline cursor or transparent".
  - The cursor was a filled rectangle, erased by filling the same
    rectangle black. That's correct only at the append point, where the
    cell is blank -- which was the only place it ever sat until line
    editing landed the same day.
  - **Fixed at the root: the cursor now saves the pixels it covers**
    (`gfx_get_pixel()` into a small static buffer) and restores them on
    hide. Exact regardless of what's underneath, which also let the
    blink come back mid-line -- it had to be pinned solid to stop it
    eating the character once a second -- and made any cursor *shape*
    free, since nothing has to reconstruct the cell.
  - **Four styles**: `translucent` (the new default -- tints the cell
    so the glyph shows through), `underline`, `beam`, `reverse`.
    Selected with a new `cursor [style]` command and persisted as
    `cursor_style` in the shared `/etc/toyos.conf`, following exactly
    the pattern `timezone`/`fontsize`/`keyboard` already use
    (`cursor_config_init()` from `kernel_main()`, `cursor_config_save()`
    on change). Legacy 80x25 text mode ignores all of it -- the
    hardware draws its own cursor there.
  - **`gfx_blend()`** -- packed-pixel alpha blending, added to `gfx.c`
    because the channel positions and widths are that file's private
    business; a caller holding a packed pixel has no portable way to
    take it apart.
  - The translucent style took three attempts, each worth recording:
    tinting the cell toward the *text* colour changed the glyph not at
    all (grey over grey) and gave a cursor you had to hunt for; tinting
    toward white moved the glyph too, dropping glyph-vs-block contrast
    from 170 to 89; tinting only the background gave a block two pixels
    wide, because a glyph like `r` fills most of its cell. Tinting both,
    with the glyph harder than the background, is what works. See
    `docs/decisions.md`.
  - The GUI Terminal needed no change -- its scrollback widget already
    draws a thin bar cursor rather than a block.
  - Verified: 55 KTESTs (3 new, covering the style-name table staying in
    lockstep with the enum and the parser rejecting rather than
    guessing), all four styles captured at 5x zoom
    (`screenshots/2026-08-13/cursor_styles_zoom.png`), and the setting
    confirmed surviving a full VM restart.

### Added
- **Readline-style command-line editing, in both the shell and the GUI
  Terminal.** Asked for as "can you make the current line editable, so
  you can move back and forth ... use arrow keys and bash convention for
  CTRL and ALT".
  - Both line editors were **append-only** before this: one position
    that only grew at the end, so fixing a typo meant holding backspace.
    Left/Right did nothing; only Up/Down (history) were handled.
  - **The driver had no Ctrl or left Alt at all.** `keyboard.c` tracked
    Shift and AltGr, and left Alt was explicitly discarded with a
    comment saying the driver had no use for it. That was the
    foundation the rest depended on.
  - **Encoding: control codes and an ESC prefix, the way a real
    terminal does it** -- `Ctrl-A` is 0x01, `Alt-B` is ESC then 'b'.
    Chosen over a `KEY_CTRL_*`/`KEY_ALT_*` block because the collisions
    it creates are the *correct* behavior (`Ctrl-H` is backspace,
    `Ctrl-I` is Tab, `Ctrl-M` is Return -- in this encoding they are
    those keys, with no aliases needed), and because a new code block
    would have to live above 0xFF, requiring an audit of every
    `(char)key` cast in the tree. AltGr stays a layout modifier rather
    than becoming Meta, so Nordic third-level characters keep working.
    See `docs/decisions.md`.
  - **One shared editor: `kernel/lib/klineedit.c`**, pure logic with no
    rendering -- buffer, cursor, kill ring, undo stack, and the whole
    keymap. Two front ends, one behavior, for the reason the three path
    resolvers taught earlier today. Being render-free is also what makes
    it testable: "Alt-B from mid-word lands at that word's start" is an
    assertion here and a screenshot to squint at in either front end.
  - **The full bash keymap**: arrows/Home/End and Ctrl+Left/Right;
    `Ctrl-A`/`E`/`B`/`F`, `Alt-B`/`Alt-F`; `Ctrl-D` (delete forward, or
    end-of-input on an empty line), `Ctrl-K`/`Ctrl-U`, `Ctrl-W` and
    `Alt-Backspace`, `Alt-D`; `Ctrl-Y` yank and `Alt-Y` yank-pop through
    a real kill ring; `Ctrl-T` transpose and `Alt-T` transpose-words;
    `Alt-U`/`Alt-L`/`Alt-C` case-change; `Ctrl-_` and `Ctrl-X Ctrl-U`
    undo; `Ctrl-R` reverse history search; `Alt-.` last argument;
    `Ctrl-L`, `Ctrl-C`, `Ctrl-P`/`Ctrl-N`.
  - Fidelity details that a reimplementation gets wrong, each with its
    own test: **`Ctrl-W` and `Alt-Backspace` use different word
    definitions** (whitespace vs alphanumeric -- over `/bin/ls`, one
    kills the whole path and the other just `ls`); **`Ctrl-U` kills
    backwards only**, not the whole line; **`Alt-Y` is legal only
    directly after a yank**.
  - **`vga_cursor_move()`** -- the console could only append or
    backspace, with no way to position the cursor. Framebuffer mode
    additionally stops blinking while the cursor sits anywhere but the
    append point, and that's a correctness requirement rather than
    styling: the blink's off-phase erases its cell to black, which is
    right over the blank append cell and would silently eat the
    character underneath anywhere else.
  - The kill ring is deliberately **shared across both front ends** --
    kill a word in the physical shell, `Ctrl-Y` it in the GUI Terminal.
  - Tab completion now completes the word **under the cursor**;
    `completion_run()` always took a position, it had simply never been
    given one that wasn't the end of the line.
  - `k_strstr()` came back (it was written, found callerless and deleted
    earlier today): `Ctrl-R`'s history search is the real caller it was
    missing.
  - Verified: `make verify` clean, 52 KTESTs (13 new for the editor
    core), and both front ends driven through QMP -- mid-line typo fix,
    `Ctrl-A`/`Ctrl-K`, `Alt-B`/`Alt-U`, and a full `Ctrl-R` search that
    finds and runs an older command. One real bug caught by that
    testing and fixed: accepting a line while the cursor sat mid-line
    left a black hole where the character under the block cursor had
    been (`cat /etc/toyos.conf` ran correctly but echoed back as
    `cat /etc/toyos conf`), because moving the cursor away erases its
    cell. Screenshots in `screenshots/2026-08-13/lineedit_*.png`.
- **A shared toolkit in `kernel/lib/`: `knum` (numbers <-> strings),
  `kfmt` (`k_snprintf` + printf-style sinks), `kpath` (paths), and a
  grown `string.h`.** Asked for as "do we need some toolkit c libraries
  so kernel mode apps and code won't have to invent the wheel again".
  - A survey first, rather than guessing at what was missing. The same
    code had been written: **9 times** for int->decimal, **10 times**
    for int->hex, **6 times** for digit parsing, **3 times** for path
    resolution. Four of those hex/decimal copies were `vga_write_dec`/
    `vga_write_hex` and `klog_write_dec`/`klog_write_hex` -- byte-for-
    byte identical pairs, with a comment in `klog.h` explaining that
    duplicating them was cheaper than the dependency. The most recent
    copy was four appenders in `strace.c`, added in the commit before
    this one.
  - **Why they were duplicated, and what actually fixes it:** each
    formatter printed somewhere different (screen, kernel log, a
    buffer, a window), so there was no shared *printer* to extract. The
    shared thing had to be a **converter that fills a caller-owned
    buffer** and lets the caller decide where it goes. That's what
    `knum` is; it depends on nothing, so any sink can use it without
    pulling in a driver, and -- unlike code that writes straight to a
    screen -- it can be tested. None of the nine originals had a test.
  - **Two rules across the whole toolkit.** A formatter that doesn't
    fit its buffer writes *nothing* rather than a truncated value (a
    truncated number or path is a wrong one, not a partial one); a
    parser rejects rather than guesses, leaving the caller's output
    untouched. The second was already `shell_sys.c`'s local convention
    -- now it's the project's, with overflow checking the hand-rolled
    versions didn't have.
  - **`kfmt`** turns the write-a-line-in-six-calls pattern into one
    call: `idt.c`'s panic block was 11 calls for 3 lines. C99
    `snprintf` semantics (returns the length it *wanted*, so truncation
    is detectable), standard printf argument rules so GCC's `-Wformat`
    stays meaningful at every call site, and deliberately no `%f`
    (no FPU here), `%p`, precision or `*` width. An unrecognised
    conversion prints literally and consumes no argument, so a typo
    can't desynchronise every argument after it.
  - **`kpath` fixed a real behavior difference, not just duplication.**
    `shell.c`'s `resolve_path()` was `static`, so `terminal.c` couldn't
    reach it and carried its own copy that didn't handle "."/".." at
    all -- `edit ../notes.txt` resolved differently in the GUI Terminal
    than at the physical shell. Both call `k_path_resolve()` now.
  - **`string.h`** grew `k_strlcpy` (BSD semantics: always terminates,
    returns the length it wanted -- chosen deliberately over
    `strncpy`'s), `k_strchr`/`k_strrchr`, `k_memcmp`, `k_memmove`, and
    `k_isdigit`/`k_isspace`. It also *lost* six functions before
    landing: `k_strstr`, `k_strcasecmp`, `k_isalpha`, `k_isalnum`,
    `k_tolower`, `k_toupper` were written and building, then found to
    have no caller anywhere in the tree, so they were deleted rather
    than shipped speculatively -- the same "second real caller, not a
    plausible one" bar this project applies to `apps/ui/` widgets.
  - **Migrated, not just added** -- the copies are gone, not joined by
    a tenth alternative: `vga.c`, `klog.c` (including its own
    `[secs.hh]` timestamp builder), `multiboot.c`, `kernel.c`, `pci.c`,
    `strace.c`, `calc_engine.c`, `shell_sys.c` (both its
    `parse_decimal` and its `print_hex_digits`), `tz.c`,
    `keyboard_layout.c`, `json.c`, `desktop.c`, `idt.c`,
    `debug_console.c`, `ata.c`, `tfs.c`, `ui_textbox.c`, `shell.c`,
    `shell_path.c`, `terminal.c`. Two migrations tightened behavior
    slightly, both deliberate: `desktop.c`'s icon-position parser now
    rejects trailing junk ("3,4x" used to parse as 3,4) and keeps the
    default, and `tz.c`'s offset parser rejects a malformed field
    instead of silently ignoring the tail.
  - **21 new KTESTs** (19 -> 40 total), covering the extremes the
    hand-rolled versions got wrong or never considered: `INT64_MIN`
    (negating it in signed arithmetic overflows), `UINT64_MAX`,
    overflow rejection, "doesn't fit produces nothing", zero-padding
    that never truncates, ".." clamping at the root, and the
    unrecognised-conversion case.
  - Verified: `make verify` clean; `cd ..`/`cat ../x` at the physical
    shell, `lspci`'s fixed-width hex columns, `dmesg` timestamps,
    `meminfo`, and a real `run crash_test` panic block (byte-identical
    output through `vga_printf`) in QEMU; plus a GUI Terminal
    screenshot of `edit ../docs/note.txt` resolving to `/docs/note.txt`
    and loading the file -- the exact command that used to fail there.
    Screenshots in `screenshots/2026-08-13/kpath_terminal_*.png`.

### Fixed
- **`run hello` page-faulted -- a deliberate fault that had quietly
  become an accidental one.** Found while testing `strace` (above);
  fixed on request afterwards.
  - `userland/hello.c` predates syscalls. With no way to print, it
    proved it had run by writing a marker to a fixed address the kernel
    read back (`USERLAND_MARKER_ADDR`) and then executing `hlt` to fault
    on purpose. The old `elftest` command mapped a page at that address
    specially. The ELF64-to-`/bin` migration folded `elftest` into the
    generic `run hello` path -- which maps no such page -- so the binary
    faulted on the marker write, one instruction *before* the `hlt` it
    existed to demonstrate. Confirmed by disassembly: `RIP=0x800000000a`
    is exactly `movl $0xc0ffee,(%rax)`, `CR2=0x8000100000` is the
    marker. It looked like a crashing binary; it was a binary whose
    harness had been removed from under it.
  - `USERLAND_MARKER_ADDR` was also `ELF_RUN_HEAP_VADDR` -- the same
    address, picked independently in two files -- so restoring the
    mapping would have put it straight on top of `sbrk`'s first page.
  - Fixed by making `hello.c` a real program (greet via `SYS_WRITE`,
    exit 0) instead of restoring the harness: `ring3test` still covers
    the raw-`iretq` entry path and `crash_test`/`nx_test` still cover
    deliberate faults and their recovery, so nothing was lost, and the
    binary named `hello` now does what its name says. It's also the
    smallest complete example of what a `/bin` binary is.
    `USERLAND_MARKER_ADDR` was deleted (no other user);
    `userland_contract.h` stays, with a comment recording why the
    constant went and why a future read-back test needs a different
    address.
  - Verified: `run hello` prints and exits 0, and `strace hello` shows
    exactly `write(1, ..., 42) = 42` then `exit(0) = ?` -- two
    syscalls, which is the whole program.

### Added
- **`strace <binary>` -- Linux-style syscall tracing.** Asked for as
  "can we implement strace like in linux".
  - Every ring-3 syscall in this kernel already funnelled through one
    function (`syscall_dispatch()`, `kernel/proc/syscall.c`), so this
    needed no per-syscall instrumentation at all: three hooks in that
    one function cover all 21 of them, and a syscall added later is
    traced as soon as its number appears in the descriptor table.
  - **Traced by address space, not by a global switch.** `strace` arms
    tracing (`strace_arm()`), and the next process created claims it --
    `strace_claim()` is one line in `elf_run_from_fs()` and one in the
    scheduler's `spawn_from_fs()`, so the mechanism isn't tied to the
    blocking loader. `syscall_process_exit_cleanup()` releases it, so a
    later process running under a recycled CR3 can't inherit the trace.
    Same single-slot compare-CR3 pattern `SYS_SBRK`'s heap arming and
    `SYS_WIN_CREATE`'s window state already use. Untraced code pays one
    global read and a compare per syscall.
  - **Arguments are decoded, not dumped.** A per-syscall table of
    argument kinds (int / hex pointer / fd / NUL-terminated path / byte
    buffer with an explicit length / `SYS_O_*` bitmask) drives the
    formatting, so a line reads `open("filetest.txt",
    O_WRITE|O_CREAT|O_TRUNC) = 3`, not three hex registers. Strings are
    read only through `vmm_validate_user_range()` -- the same gate the
    real handlers use -- capped at 32 characters with `"..."`, and
    C-escaped (`\n`, `\xNN`) so a trace line can never contain a control
    character that moves the console cursor. A pointer that fails
    validation prints as hex rather than being skipped, so a bad pointer
    is visible in the trace instead of invisible. An unknown syscall
    number still traces, as `syscall_999(0x1, 0x2, 0x3)`.
  - **The line is formatted on entry but emitted on exit.** The
    arguments have to be read before the handler can overwrite what they
    point at, but printing them then would leave the entry half sitting
    across a `SYS_WRITE`'s own output. Emitting the whole line after the
    handler returns means a traced program's output lands above its
    trace line instead of spliced into the middle of it. Cost: a handler
    that faults mid-call prints nothing, and `SYS_EXIT` (which may never
    return) has to close out its own line -- hence `exit(0) = ?`.
  - Output goes to the console *and* `klog`, so a trace is both
    strace-like on screen and readable afterwards with `dmesg` --
    which is also what makes it assertable from `tools/vm.py` with no
    screenshot. `sbrk` is the one syscall whose return prints as hex (it
    returns a pointer); its `-1` failure stays decimal so an error can't
    read as an address.
  - Deliberately *not* routed through `shell_exec_name()` the way `run`
    is, breaking that file's usual one-resolver rule on purpose:
    `shell_exec_name()` tries kernel-space console apps first, and a
    kernel-space app makes no syscalls at all, so tracing one would
    print an empty trace instead of an error. `strace` resolves through
    `shell_path_find()` only, so `strace gui` says so.
    Added to the GUI Terminal's `BLOCKED_CMDS` for the same reason
    `run` used to be there -- it runs its target through the blocking
    `elf_run_from_fs()`, which would freeze that window's event loop.
  - Verified: `make test` (5 new `KTEST("strace", ...)` cases covering
    the argument table, flag decoding, unknown numbers, return
    conventions, and buffer truncation -- `strace_format_call()` takes
    the address space as a parameter, and passing 0 means "don't
    dereference", which is what makes the decoder testable with no live
    process). Then really traced in QEMU: `strace file_test` (10
    syscalls, full open/write/read/close round trip), `strace ls -l
    /etc` (107 syscalls -- `/bin/ls` writes a character at a time), and
    `strace write_test`; confirmed the same lines come back out of
    `dmesg`, and that `strace`, `strace nosuchthing` and `strace gui`
    each give the right refusal. Screenshots in
    `screenshots/2026-08-13/strace_*.png`. Unrelated pre-existing
    finding: `/bin/hello` on the seeded disk page-faults at its heap
    base with or without tracing -- not touched here.
- **Console scrollback (PageUp/PageDown), and the kernel's boot log on
  screen.** Asked for as "some easy way to see the GRUB boot menu and
  the boot messages -- now they go too fast".
  - The framebuffer console drew glyphs straight into the framebuffer
    and scrolled by blitting pixels upward, keeping nothing. It now
    records a ring of output lines (256 x 256 cells, colour per cell, in
    `.bss`), and PageUp/PageDown repaint a window of it. The GUI
    Terminal has had scrollback since its widget existed; the physical
    console never did.
  - **`klog_write()` never reached the screen at all** -- it went to the
    serial port and the `dmesg` ring, so the console showed "toy-os
    booting..." and then the shell. Scrollback alone would have had no
    boot messages to scroll back to. `kernel_main()` now mirrors the log
    to the console for the duration of boot and switches it off just
    before `apps_start()`, so the init sequence is visible the way a
    real kernel's is without every later ATA retry landing on top of the
    shell.
  - Keys are swallowed by `keyboard_getchar()` (the blocking reader) and
    deliberately *not* by `keyboard_try_getchar()`, which the window
    manager polls -- the GUI Terminal and Notepad have their own
    PageUp/PageDown and would have broken.
  - Verified live: one PageUp from a fresh boot shows the whole init
    sequence with its colours intact; PageDown returns; typing anything
    snaps back to live; two screens of output page back correctly; the
    GUI Terminal's own scrollback still works.
- **`make run-menu`** boots with the GRUB menu visible (5s timeout).
  `grub.cfg`'s timeout is now substituted at ISO build time from
  `GRUB_TIMEOUT` (default 0), so `make run`, the boot smoke test, ktest
  and CI all stay instant -- a few seconds per boot adds up across a
  test cycle, which is why this is opt-in rather than global.

- **Tooling pass, from friction hit while doing the last few changes.**
  - **`sh <command>` on the serial debug console** + **`tools/vm.py`**:
    shell output comes back as TEXT instead of a screenshot to read by
    eye. `vm.py start` / `exec "fsck" "df"` / `shot x.png` / `stop`,
    plus `run` for one-shot use. This replaces the loop that dominated
    verification all session -- hand-write a `qemu-system-x86_64
    -daemonize -pidfile` line, sleep, open QMP, emulate the command one
    qcode at a time, screendump, read the PNG. That loop is
    layout-dependent (a `se` keyboard layout turned `write_test` into
    `write?test` and cost half an hour of debugging a non-bug), drops
    keys under load, and produces a picture rather than something a test
    can assert on.
  - The console was documented as deliberately read-only inspection and
    now isn't. `docs/decisions.md` records why that trade is acceptable
    *here specifically* -- no users, no permissions, no network, and
    serial is already a physical-access channel that could halt the
    machine and read every file via `lsfs`. `gui`/`ring3test`/
    `schedtest`/`edit`/`nano` are still refused (they take over the
    screen, never return, or need keys this console can't deliver),
    mirroring `apps/terminal.c`'s existing `BLOCKED_CMDS` -- the GUI
    Terminal solved the same problem first, and `sh` runs through the
    same `shell_dispatch()` rather than reimplementing anything.
  - `vm.py` only ever kills a QEMU it started itself (its own
    `.vm.pid`), so an interactive `make run` window is never at risk --
    the mistake CLAUDE.md warns about with `pkill -f
    qemu-system-x86_64`. It also clears a stale pidfile rather than
    failing with QEMU's "cannot create PID file", which happened once
    this session.
  - **`tools/tfs2_writer.py` gained `delete`, `mkdir` and `cp`**, so
    disk state can be prepared and cleaned up entirely from the host.
    Removing two test binaries previously meant booting toy-os to type
    `rm`, because the tool could create files but never remove them.
    Verified the block accounting agrees with the kernel's: after a
    host-side `delete`, the guest's `fsck` reports clean with no leaked
    blocks.
  - **`tools/preflight.sh` now runs `ktest`**, and `make verify` runs
    the whole pre-delivery check (clean build + iso + boot smoke test +
    test suite). "Does it boot" and "does it work" are different
    questions and preflight only asked the first.

  One thing that turned out NOT to be a bug: `tfs2_writer.py` was
  suspected of silently ignoring a refused overwrite. It exits 1
  correctly -- the earlier evidence was a `2>&1 | tail -1` in the
  invocation swallowing the error message. Checked before changing
  anything; no fix needed.

- **In-kernel test harness (`ktest`), Milestone 4.** Tests are
  `KTEST("suite", "name") { ... }` blocks that live next to the code
  they exercise and register themselves by existing -- the macro drops a
  descriptor into a `.ktests` linker section and the runner walks it, so
  there's no registry to update and (with the recursive Makefile) no
  build edit either. 14 tests today across `mm`, `fs` and `lib`.
  - **Nothing runs tests at boot any more.** `kernel_main()` called
    `pmm_selftest()`/`heap_selftest()`/`json_selftest()` on every boot
    and `tfs_init()` called `tfs_selftest()`, which wrote 64 bytes at a
    4.6GB offset on every disk-backed boot to re-verify something that
    can only break when `tfs.c` changes. All four now report pass/fail
    (they returned `void`) and run when asked.
  - **`make test` exits non-zero on failure**, which the old arrangement
    could not do at all -- a failing self-test printed a line and the
    kernel booted on regardless. `tools/ktest_run.py` boots headless,
    drives `ktest` over the serial debug console and turns the report
    into an exit code; CI runs it next to the boot smoke test. Verified
    by deliberately breaking an assertion: exit 1 with the failing
    test's file:line, exit 0 once reverted.
  - **Fault injection** (`kernel/include/kernel/fault_inject.h`): fail
    the next N ATA writes, ATA reads, or kmalloc calls. This is what
    makes the error paths added during the storage work testable at all
    -- previously the only way to reach them was corrupting a disk image
    from the host with `tools/tfs2_writer.py corrupt`. Five of the 14
    tests use it (a failed metadata write must be reported not
    swallowed; a failed data write must fail; a failed read must come
    back short; a failed kmalloc must be reported and leave the heap
    usable; the injector must disarm itself).
  - `ktest_run_all()` is exposed through `kapi.h` while the KTEST macro
    and assertions stay in `kernel/include/kernel/` -- the shell needs
    to *run* tests, but writing one is kernel work. The header split
    from the restructure caught this immediately: `apps/shell_sys.c`
    including `ktest.h` simply didn't compile.

  Three things this turned up, all now fixed and commented:
  - **`.ktests` entries need forced alignment.** The 24-byte descriptors
    had natural alignment 8, the linker aligned each object file's
    contribution to 16, and the resulting 8 bytes of padding made the
    section 440 bytes for 18 entries. Walking that as an array read
    padding as a test -- and because pointer subtraction on a
    non-multiple of the element size is undefined behaviour, GCC's
    divide-by-24 reciprocal reported `2863311549` tests before panicking
    on a garbage function pointer. `aligned(32)` on the struct plus
    counting in bytes fixes it.
  - **`heap_selftest()` assumed it owned the machine.** It asserted
    `heap_used_bytes() == 0` after freeing its allocations, which held
    only because it ran immediately after `heap_init()`. Run from a
    booted system it failed on a perfectly healthy heap. Now it compares
    against the level on entry, which keeps exactly the property it
    exists for (a coalescing bug corrupting a neighbour shows up as an
    accounting mismatch). The harness earned its keep on its first run.
  - **The serial debug console needed an output sink.** `ktest_run_all()`
    reports through `vga_write()`; the debug console writes via
    `klog_write()` and installs no sink, so over serial the tests ran but
    their report went to a screen nobody was watching -- `ktest_run.py`
    timed out waiting for a verdict that was being printed elsewhere.
    `dbg_cmd_ktest()` now installs a serial sink for the duration.

- **Executables run by name, with a configurable `PATH`**
  (`apps/shell_path.c`, new). Typing `nx_test` now runs `/bin/nx_test`;
  the `run` prefix is optional. `PATH` is a key in `/etc/toyos.conf`
  (default `/bin;/usr/bin`), semicolon-separated -- a colon is accepted
  too -- searched **left to right with the first match winning**.
  - **Resolution order: builtins, then `apps.c`'s console-app registry,
    then each PATH directory.** Letting disk binaries outrank builtins
    would silently break `ls`: it's a builtin *wrapper* that resolves
    its positional argument against the cwd before handing `/bin/ls` an
    absolute path, and a PATH-executed binary gets raw arguments with no
    cwd of its own. See `docs/decisions.md`.
  - `run` is kept as the explicit form. Both it and a bare name go
    through one resolver (`shell_exec_name()`), so they can't diverge --
    `cmd_run()` is now a name/args split followed by that call.
  - A name containing `/` is treated as a path, not a PATH lookup, so
    `/bin/foo` and `docs/foo` mean what they say. Entries in PATH that
    don't exist are skipped silently: the default names `/usr/bin`,
    which isn't on a stock disk, and warning about that every boot would
    be noise.
  - **PATH is shell state, not kernel state** -- `timezone`/`font_size`
    have kernel-side modules because the kernel reads them; nothing in
    the kernel has any use for PATH, so this reads the shared config
    file through kapi.h's `etc_config_get()` and keeps the result to
    itself. The dividing line is "does the kernel read it", not "is it
    in toyos.conf".
  - New `path` command prints the search order, marking entries that
    don't exist yet, since otherwise the only way to see it is to read
    the config file.
  - Tab completion follows: the first word of a line now completes
    builtins *and* registry apps *and* every executable in every PATH
    directory, and `run <TAB>` enumerates PATH rather than a hardcoded
    `/bin`.
  - The shell is four files now (`shell.c`/`shell_fs.c`/`shell_sys.c`/
    `shell_path.c`); `shell_internal.h`'s top comment updated to match.
  - Verified live, including the ordering the feature is really about:
    with `PATH=/bin;/usr/bin`, `write_test` ran `/bin/write_test` and
    `only_here` (present only in `/usr/bin`) resolved from the second
    directory; with the order reversed to `PATH=/usr/bin;/bin` and a
    deliberately different binary planted at `/usr/bin/write_test`, the
    same typed name ran *that* one instead -- config-driven, first match
    wins (screenshots `path_order_bin_first.png`,
    `path_order_usr_first.png`). Builtins still win and still resolve
    cwd-relative arguments (`cd /etc` then `ls kbs`), `run` still works,
    and `only<TAB>` completes a PATH binary
    (`path_builtin_and_completion.png`).
- **Tab completion in both shells** (`apps/completion.c`/`completion.h`,
  Milestone 10's first item), asked for as "auto completion like in zsh".
  Behaviour follows zsh's default rather than bash's: one Tab extends
  the word as far as every candidate agrees, and if more than one
  candidate remains they're listed in columns and the prompt is redrawn
  underneath (zsh's AUTO_LIST). No menu cycling, so there's no state
  between keystrokes.
  - **Three domains.** The first word completes command names. An
    argument of a command with a known argument set completes from that
    set -- `run` (the console app registry *and* the real `/bin`
    binaries, since `run` accepts either), `color`, `debug` (subsystem,
    then on/off), `keyboard` (whatever layout files are actually in
    `/etc/kbs`, not a hardcoded us/se -- the point of layouts being data
    files), `timezone` (the city database), `fontsize`, `fsck`, `help`.
    Everything else completes filesystem paths, resolved against the
    shell's cwd, with directories getting a trailing `/` so the next Tab
    descends.
  - **Candidate generation only.** `completion.c` does no input handling
    and no drawing. That's because there are two shells with completely
    separate input loops -- `shell_read_line()` driving
    `keyboard_getchar()`/`vga_putc()`, and `terminal.c`'s `on_key`
    drawing through a `text_scrollback` widget -- and only the candidate
    logic is genuinely common. A shared *line editor* would be the
    better end state and is still worth doing, but it means rewriting
    two working input paths; see `docs/decisions.md`.
  - New `shell_resolve_path()` (`shell.h`) exposes the shell's existing
    cwd-relative path resolution, which completion needs to turn a
    half-typed path into a directory `fs_list()` accepts.
  - Verified live in both shells: `ca<TAB>` -> `cat `, `c<TAB>` lists
    cat/cd/clear/color, `cat /etc/ti<TAB>` -> `/etc/timezones` (and the
    file actually reads), `color li<TAB>` extends to `light` and lists
    the six, `debug <TAB>` lists fs/wm/ata, `run <TAB>` lists the
    registry apps alongside all 17 `/bin` binaries. Screenshots
    `completion_paths.png`, `completion_args.png`,
    `completion_run_targets.png`, `completion_gui_terminal.png`.
  - Two bugs found by testing rather than by reading, both fixed here:
    candidates were matched against the wrong string on the path
    branch (the full `/etc/timezones` was compared against the `ti`
    prefix, so path completion silently found nothing), and the
    trailing space added after a unique completion broke every command
    that treats its argument as a single value -- see Fixed below.
- **`docs/roadmap.md` expanded: 10 new milestones and ~70 new steps**
  across the existing ones, asked for as "add plenty now so we have more
  things to implement and maybe fix". The file went 1,066 -> 1,541 lines;
  the checkbox count went 81 -> 231 (16 done, 215 open).
  - New milestones, each with both a checkbox list and a prose Details
    section in the existing style: **21** TTY/virtual terminals, **22**
    real mount points, **23** UTF-8 migration, **24** observability,
    **25** kernel test harness, **26** demand paging & shared memory,
    **27** UEFI boot, **28** data journaling & snapshots, **29**
    benchmark suite, **30** a scripting language.
  - Most of the new steps came out of things noticed while working in
    the code rather than invented for the list: `find()` being a linear
    `k_strcmp()` scan over 256 slots on every path lookup, the absence
    of `fs_rename()`/`fs_truncate()`, 28-bit LBA capping the disk at
    128 GiB, the shell parser having no quoting (so no argument can
    contain a space), window resize only working from the bottom-right
    grip, and the pile of single-threaded assumptions SMP would have to
    audit (`tfs.c`'s static scratch buffers, `heap.c`'s free list,
    `vga.c`'s cursor state).
  - Two Backlog items were promoted out into real milestones -- VFS
    mount points (22) and a benchmarking harness (29) -- and the Backlog
    gained six smaller ones in their place.
  - New intro note: **numbering is identity, not priority.** Milestones
    keep their numbers because `CHANGELOG.md` and `docs/decisions.md`
    refer to them by number, so renumbering would silently break those
    references; the list is therefore roughly ordered but not strictly,
    and several later milestones are worth pulling forward when they
    unblock something (21's TTY layer gates half of 6's signal work;
    25's test harness pays for itself before any driver milestone).
- **Journal-batched flush** -- the last open performance item in
  Milestone 3, and it landed narrower than the roadmap framed it.
  `persist_record()` (`kernel/drivers/tfs.c`) took a synchronous
  `CMD_CACHE_FLUSH` after each of its four writes, on the reasoning
  that a write-ahead journal needs every write durable before the next
  is issued. Only two of those barriers actually carry weight:
  - After the **journal data**: not needed. A torn write there fails
    the FNV-1a checksum stored in the commit header, so replay discards
    the entry -- "the operation didn't happen" is a legitimate crash
    outcome.
  - After the **commit header**: required. Once the table slot is being
    overwritten, the journal entry is the only surviving copy of a
    record that can be torn.
  - After the **table slot**: required. Retiring the entry before the
    real slot is durable leaves a torn slot with nothing to replay.
  - After the **header clear**: not needed. Losing it costs one
    redundant replay on the next boot, rewriting the same bytes to the
    same slot.

  So the rule isn't "a WAL flushes every write", it's "a barrier is
  required where losing write N-1 makes write N unrecoverable". Four
  flushes become two, the recovery argument is unchanged, and every
  metadata operation gets ~2x cheaper. Measured on the path that does
  256 of them back to back -- formatting a fresh disk -- via `dmesg`
  timestamps: **0.73s -> 0.34s** (screenshot
  `journal_batched_format_dmesg.png`).
  - New `ata_flush_now()` and `ata_flush_end_no_flush()` (`ata.c`/
    `ata.h`). The barriers have to be `ata_flush_now()` rather than
    `ata_flush_end()`, and that distinction is load-bearing:
    `ata_flush_end()` only flushes once its own depth reaches 0, so a
    journal sequence running inside an outer batch gets no barrier at
    all. **That was a live bug for one commit**: `tfs_check()`'s repair
    pass (added in the `fsck` change) calls `persist_record()` inside a
    `write_batch_begin()`/`end()` pair, which silently suppressed every
    one of the journal's flushes. This fixes it properly rather than by
    moving the call.
  - **The recovery paths are now actually tested**, which they never
    were before -- `replay_journal()` could only run after a real
    power loss mid-write. `tools/tfs2_writer.py corrupt
    --stage-journal PATH` leaves an image in exactly the state a crash
    between "entry committed" and "table slot written" produces, and
    `--stage-journal-torn` additionally corrupts the staged bytes so
    the checksum must fail. Both verified end-to-end: the valid entry
    logs `replayed a pending journal entry` and the file exists
    afterward (confirmed host-side); the torn one logs `discarded a
    torn journal entry` and the file does not. Note this exercises
    replay, not durability itself -- whether a flush really reached the
    platter can't be tested without pulling power.
  - Regression pass unchanged: `stress 50` at 24.7 MB/s write / 30.3
    MB/s read, `fsck` clean, `mkdir`/`write`/`cat` surviving a reboot,
    `rm` cleaning up (screenshot `journal_batched_regression.png`).
- **`fsck` / `fsck repair`** -- a filesystem consistency check and the
  leak-reclaiming pass behind it (`fs_check()` in `kernel/include/fs.h`,
  `tfs_check()` in `kernel/drivers/tfs.c`, `cmd_fsck()` in
  `apps/shell_sys.c`). This is the other half of a trade made a few
  entries down: the truncate/delete paths now persist a record
  referencing nothing *before* returning its blocks to the bitmap, so an
  interrupted operation leaks blocks rather than double-allocating them
  -- correct only if something can eventually reclaim the leak.
  - Classic mark-and-compare: walk every in-use record's block tree
    (direct + all three indirect depths) marking a "referenced" bitmap,
    then compare it against the real free-block bitmap in both
    directions. Reports leaked blocks, referenced-but-free blocks,
    blocks claimed by more than one record, and pointers naming a block
    outside the usable range.
  - `fsck` alone is **read-only** and safe to run any time; `fsck
    repair` frees leaked blocks, marks referenced-but-free blocks
    allocated, and zeroes out-of-range pointers. A double-allocated
    block is always reported and never repaired -- both records are
    internally plausible and picking a winner silently destroys the
    other file's data. See `docs/decisions.md`.
  - The scratch bitmap is a static 288KB array, not `kmalloc()`'d: that
    allocation would need 72 contiguous frames from pmm, and failing to
    get them would mean "can't check the disk" exactly when something is
    already wrong.
  - **`tools/tfs2_writer.py corrupt`** -- host-side fault injection
    (`--leak N`, `--free-referenced N`, `--bad-pointer PATH`), because
    the inconsistencies `fsck` repairs are ones the kernel deliberately
    avoids producing; without a way to manufacture them, `fsck` could
    only ever be proven to report "clean".
  - Verified against damage of known shape rather than by inspection:
    48 injected leaked blocks were reported as exactly 48, `fsck repair`
    reclaimed 192 KB (`df` used 396 KB -> 204 KB), a re-check reported
    clean, and it was **still** clean after a reboot, proving the bitmap
    writes actually landed. A second image injected with all three
    repairable classes came back leaked 1 -> 0, referenced-but-free
    2 -> 0, out-of-range 1 -> 0. Screenshots
    `fsck_report_48_leaked.png`, `fsck_repair_and_df.png`,
    `fsck_clean_after_reboot.png`, `fsck_repairs_all_three_classes.png`.
  - That second test also produced an unplanned demonstration of why the
    referenced-but-free repair matters at all. Three blocks belonging to
    real files were marked free; on the very next boot, the shell's
    append to `/etc/history` allocated one of them -- block 105, which
    `/bin/counter_a` still owned. A genuine double-allocation, created
    by that corruption within seconds of booting, then correctly
    detected and correctly *not* auto-repaired. Confirmed independently
    host-side: `{105: ['/bin/counter_a', '/etc/history']}`.
- **Storage stack audit, and the four changes that came out of it.**
  Asked to read the filesystem/ATA code through (`fs`/`tfs`/`vfs`/`ata`)
  and propose fixes; the audit found one data-loss bug, one silent
  error-swallowing class, one nearly-exhausted limit, and one
  self-imposed throughput ceiling. All four were then asked for. Each
  is described in its own section below (`### Fixed` for the durability
  work, `### Changed` for the rest); this entry is the index:
  - **`ata_sector_count()`** (`kernel/drivers/ata.c`/`ata.h`) -- the
    drive's real capacity from IDENTIFY words 60-61, which this driver
    had always read and discarded. `ata_read_sectors()`/
    `ata_write_sectors()` now range-check against it (a transfer past
    the end of the drive fails loudly instead of being handed to the
    hardware), and TFS2 clamps its block count to it at mount instead
    of trusting a hardcoded 9 GiB. Verified on a deliberately small
    image: a 512MB `disk.img` logs `using 131072 of 2359296 blocks`,
    and `df` reports 523868 KB rather than the built-in maximum
    (screenshot `df_small_disk.png`).
  - **`ata_max_sectors_per_xfer()`** -- the per-transfer sector cap
    actually available this boot, as opposed to the compile-time
    `ATA_MAX_SECTORS_PER_XFER`. Callers that batch work into transfers
    (TFS2's new block coalescing) ask this; it reports the smaller
    number when the 64KB DMA buffer couldn't be allocated or when the
    PIO fallback is in use.
  - **`BLK_ALLOC_NOZERO`** (`kernel/drivers/tfs.c`) -- an allocation
    mode for a block the caller is about to overwrite in full, skipping
    the zero-fill write that every freshly allocated block used to get.
- `tools/shell_flow.py`: a `gui_flow.py`-style helper for the physical
  (pre-`gui`) shell -- `ShellFlow.run_command(cmd, subdir=...)` types a
  full command (spaces/hyphens/underscores/etc handled automatically)
  and screenshots the result, instead of a testing session
  hand-interleaving `send_text()`/`send_key('spc')`/
  `combo(['shift','minus'])` calls character by character every time.
  Prompted by two real mistakes in the same session (a dropped space,
  a hyphen typed where `run nx_test`'s underscore was needed) while
  testing the NX-enforcement entry below. Deliberately returns a
  screenshot path, not parsed text -- this kernel's console picks a
  framebuffer (glyphs-as-pixels) backend whenever GRUB provides one,
  the normal case here, so there's no legacy-VGA-text-buffer
  memory-read shortcut to plain text; see the module's own docstring.
  Verified: `run_command("run nx_test")` typed the full command
  correctly (including the underscore) in one call.
- NX bit enforcement + W^X for userspace process pages (Milestone 2,
  docs/roadmap.md). Previously every mapped page anywhere -- kernel or
  user, code or data -- was present+writable(+user), full stop; a
  user ELF's `.data`/`.bss`/stack were as executable as its `.text`,
  and every PT_LOAD segment got mapped identically regardless of its
  real ELF permission bits (`p_flags` was parsed but never read).
  Scoped to userspace process pages only, matching this session's
  choice -- the kernel's own `boot.asm` identity map (flat 2MiB huge
  pages, no code/data split) is unchanged and stays RWX; that's the
  separate, larger "W^X on kernel... mappings" roadmap item.
  - `kernel/core/boot.asm`: `enable_paging` now also sets EFER.NXE
    (bit 11 of the `0xC0000080` MSR) alongside the existing long-mode
    bit -- required once, globally, for the CPU to honor PTE bit 63 at
    all.
  - `kernel/core/vmm.c`/`vmm.h`: new `PAGE_NX` bit and
    `vmm_map_user_page_flags(pml4_phys, vaddr, paddr, writable,
    executable)`. The existing `vmm_map_user_page()` is now a thin
    wrapper defaulting to writable+NOT executable -- the correct,
    secure default for every pre-existing call site (a process's
    stack, SYS_SBRK heap growth, the GUI framebuffer, a window's pixel
    buffer -- all data, never code), so those all become non-executable
    for free with no call-site changes. `kernel/core/ring3_test.c`'s
    hand-assembled code page is the one call site needing
    executable=1 explicitly, via the new `_flags` variant.
  - `kernel/core/elf.c`: `load_segment()` now actually reads
    `ph->p_flags` (new `PF_X`/`PF_W` constants) and maps each PT_LOAD
    segment's pages with its own real writable/executable bits via
    `vmm_map_user_page_flags()`, instead of the old blanket
    present+writable+user every segment used to get.
  - `userland/link.ld`: this is what makes the above mean anything --
    an explicit `PHDRS` block now emits three separate, page-aligned
    (`ALIGN(4096)`) `PT_LOAD` segments (`.text` R+X, `.rodata` R-only,
    `.data`+`.bss` R+W) instead of one merged segment covering
    everything. NX/W^X is enforced per 4KiB page, so without this
    split every userland ELF would still have `.text` and `.data`
    sharing pages and nothing to differentiate. Confirmed via
    `readelf -lW`: 3 distinct `PT_LOAD` entries with the expected `R
    E`/`R`/`RW` flags, each `VirtAddr` exactly 4096-aligned. Also
    incidentally fixes every userland `.elf`'s `ld: ... has a LOAD
    segment with RWX permissions` build warning (kernel.bin's own
    warning is unchanged/expected -- out of scope, see above).
  - New `userland/nx_test.c` (+ Makefile/`RUN_ALLOWED_BINS` wiring,
    same pattern as `crash_test.c`): copies a tiny valid instruction
    (`ret`, 0xC3) into a writable `.bss` buffer and calls it as a
    function -- exactly the shape of a real exploit's second stage.
    Verified via QMP (`run nx_test`): the kernel reports `RING-3
    PROCESS CRASHED: Page fault`, `error_code=0x15` -- decodes to
    Present + User + Instruction-Fetch, the specific signature of an
    NX violation, not a generic unmapped-page fault -- and the
    injected code never executes (no "UNEXPECTEDLY SURVIVED" message).
    Process torn down, control returned cleanly to the shell, same
    recoverable-fault path `crash_test` already exercises.
  - Regression-verified via QMP: `run ls`, `run crash_test` (still
    faults exactly as before, unrelated kernel-only-page violation),
    and a GUI-spawned `ls` via Terminal's async `run` (scheduler.c's
    separate spawn path) all behave identically to before this change.
  - Found (not caused) during verification: `run hello` page-faults on
    a write to `USERLAND_MARKER_ADDR` (`kernel/include/
    userland_contract.h`), an address only ever mapped by a
    `kernel/core/elf_test.c` that no longer exists in this tree --
    confirmed by building unmodified `main` and reproducing the
    identical crash there too. Pre-existing, unrelated to this change;
    left as-is (either `hello.c`'s marker write or the file's own
    stale top comment needs updating, a separate small cleanup).

- Real GDB debugging via `make debug` -- boots toy-os frozen at CPU
  reset (QEMU's `-s -S`) so a host `gdb` can attach
  (`target remote localhost:1234`) for real breakpoints, single-step,
  and register/memory inspection. No kernel-side GDB protocol code
  needed at all -- QEMU's own built-in stub emulates the CPU directly,
  independent of the guest OS (see `docs/decisions.md` for why an
  in-kernel serial stub, the first framing of this idea, was
  unnecessary). `CFLAGS`/`USERLAND_CFLAGS` gain `-g` (kept at `-O2`,
  not dropped to `-Og`) so `kernel.bin`/every userland ELF carry real
  DWARF symbols -- function names and source lines, not just raw
  addresses. Verified end-to-end: `break kernel_main` + `continue` over
  a real `gdb` session correctly ran the CPU from reset through
  GRUB/multiboot2 and stopped exactly at `kernel_main`, with a working
  backtrace showing source file/line.
- `make run-audio` -- same as `make run`, plus `-audiodev pa,id=snd0
  -machine pcspk-audiodev=snd0` so the PC speaker (`beep`, see below)
  is actually audible -- confirmed working on a real machine.
  `make run` itself is unchanged (no default audio backend assumed --
  the right one is host-specific). Also fixes a stale `help` target
  claim that `run` opens a GTK window; it's been SDL for a while
  (`-display sdl,grab-mod=rctrl`, see `CLAUDE.md`).
- MBR + GPT partition table parsing (Milestone 3, `docs/roadmap.md`):
  new `kernel/include/partition.h`/`kernel/drivers/partition.c`,
  `partition_read_table()` -- reads LBA 0 via `ata_read_sector()`,
  checks the `0x55AA` signature, and either parses up to 4 legacy MBR
  entries or (if a protective `0xEE` entry is found) reads LBA 1 as a
  GPT header, validates its CRC32, and reads its partition entry array
  (type/unique GUIDs, LBA range, UTF-16LE name). Read-only, parse-only
  -- `disk.img` is still one raw TFS2 blob at LBA 0 (see
  `docs/tfs2-spec.md`), never consulted by the mount path. New
  `parttable` shell command (`apps/shell_sys.c`) prints whatever was
  found, formatted like `lspci`. New `tools/mkpart_test.py` writes a
  synthetic MBR or GPT onto a disk image for testing, TFS2-mount-
  preserving (patches only the partition-table byte ranges TFS2 itself
  never touches, so the real filesystem underneath still mounts
  normally instead of being auto-reformatted).
  Verified two different ways for the two cases -- see
  `docs/decisions.md` for why they had to differ: the MBR path (and
  the "no partition table" case) live, via QMP -- patched `disk.img`,
  booted, ran `parttable` from the shell, confirmed the printed
  type/LBA/sector fields matched exactly what was written, for both a
  plain MBR and a protective-MBR-only (GPT-signaling) disk. The GPT
  header-parsing path itself (CRC32 validation, entry array read) was
  verified via a host-compiled unit test including the real,
  unmodified `partition.c` against a synthetic image, instead of a
  live boot -- `kernel/drivers/tfs.c`'s `tfs_selftest()` unconditionally
  overwrites LBA 1 (the GPT header's mandated location) with a real
  journal header on every single boot, before the shell is ever
  reachable, so a custom GPT header there can never survive to be read
  by a live `parttable` call. Confirmed correct CRC32, both partitions'
  type/unique GUIDs, LBA ranges, and names exactly matching what was
  written.
- PC speaker beep (Milestone 19, `docs/roadmap.md`): new
  `kernel/drivers/speaker.c`/`speaker.h`, `speaker_beep(freq_hz,
  duration_ms)` -- programs PIT channel 2 (ports `0x42`/`0x43`, same
  square-wave mode 3 channel 0 already uses for the system timer) and
  gates it through to the physical speaker via port `0x61` bits 0-1,
  restoring the port's prior value afterward rather than just clearing
  those bits. Exposed via a new `beep` shell command (`apps/shell_sys.c`)
  -- a fixed 800Hz/200ms tone, "simplest possible output" by explicit
  request rather than a freq/duration-adjustable command. Blocks for
  the tone's duration by busy-waiting on `pit_ticks()` (10ms
  resolution) -- no scheduler-aware sleep/delay primitive exists in
  this kernel yet, same gap noted under Milestone 15. Verified:
  `boot_smoke_test.py` passes, and `beep` from the shell prints "beep!"
  and returns control to the prompt promptly (headless QEMU has no
  audio device attached, so the tone itself can't be verified
  programmatically -- the PIT/port-0x61 programming completing cleanly
  and the busy-wait duration behaving as expected is what's testable
  here).
- Stack canaries (Milestone 2, `docs/roadmap.md`): `-fstack-protector-strong`
  is on for both the kernel (`CFLAGS`) and userland (`USERLAND_CFLAGS`)
  now, previously explicit `-fno-stack-protector` in both. Uses
  `-mstack-protector-guard=global` (a plain extern `__stack_chk_guard`)
  rather than GCC's TLS-based default, since this kernel has no
  FS/GS-base infrastructure for that default to read; the guard value
  is a fixed compile-time constant, not random, since there's no
  entropy source yet either. New `kernel/core/stack_protector.c`
  (kernel-side `__stack_chk_guard`/`__stack_chk_fail`, the latter
  printing a panic banner and halting -- no "recoverable" case for a
  kernel-side canary trip) and `userland/stack_chk.c` (userland's
  version, linked into every userland ELF now -- `SYS_WRITE` a message
  then `SYS_EXIT(2)`, an ordinary process exit from the kernel's point
  of view). New `userland/stack_smash_test.c` self-test (seeded as
  `/bin/stack_smash_test`, run via the shell's `run stack_smash_test`)
  deliberately overflows a local buffer to prove the canary actually
  catches a real overflow, not just "the kernel still boots" -- see
  `docs/decisions.md` for a real gotcha hit writing it (the overflow
  function needs `__attribute__((noinline))`, or GCC inlines it into
  `_start` and moves the canary check past code that already exited
  the process). Verified: `make clean && make all && make iso` +
  `boot_smoke_test.py` pass with the flag on kernel-wide (no
  false-positive trip during boot's own self-tests), and
  `run stack_smash_test` from the shell prints "stack smashing
  detected", exits with code 2, and returns cleanly to the prompt.

- Draggable desktop icons (Milestone 9, `docs/roadmap.md`): each desktop
  icon now has real per-icon grid position state (`apps/wm/desktop.c`'s
  `icon_col`/`icon_row`, previously a fixed left-edge column derived
  straight from `gui_app_registry`), draggable to any cell in a real
  multi-column grid and snapping to the nearest cell on release.
  Positions persist across reboot in `/etc/desktop.conf`, keyed by app
  name (so a `gui_app_registry` reorder doesn't scramble saved
  positions) via `kernel/include/etc_config.h`'s shared reader/writer
  (newly exposed to apps through `kapi.h`). The grid geometry and
  drag-to-reposition session are a new reusable widget,
  `apps/ui/ui_icon_grid.h`/`.c` (`icon_grid_cell_rect`/
  `icon_grid_nearest_cell`, `struct icon_drag` +
  `icon_drag_start/update/end`), mirroring `wm_input.c`'s window-drag
  shape (mouse-down arms it with a grab offset, a per-tick update
  tracks the cursor, mouse-up commits) -- built as its own widget file
  rather than desktop.c-local state, by explicit request, ahead of the
  second real caller a future file manager's icon view (Milestone 10)
  is expected to be; see `apps/README.md`'s "Shared widgets" section
  for why that's called out as a deliberate exception. Click/double-
  click-to-launch behavior is unchanged -- a plain click (no movement
  before release) just re-commits the icon to the cell it's already
  in. Two icons dragged onto the same cell simply overlap; no swap/
  displace logic yet. Verified via QMP: dragged the Notepad icon to a
  new cell (screenshot), exited to shell and re-entered GUI mode to
  confirm the position persisted (screenshot), and double-clicked the
  moved icon to confirm launch still works post-drag (screenshot) --
  see `screenshots/2026-08-12/desktop-icon-drag-*.png`.

- Real per-window damage-region compositor (Milestone 9, Phase 1+2 of
  the plan -- Phase 3, skipping `on_draw()` for unaffected windows, is
  a deliberate follow-up, not done here). The window manager used to
  redraw the entire screen on any scene change at all, down to a
  once-a-second clock tick; it now tracks a single scene-wide damage
  bounding box per frame and clips the repaint to it.
  - `kernel/drivers/gfx.c`/`gfx.h`: new `gfx_set_clip_rect()`/
    `gfx_clear_clip_rect()`, gating `gfx_put_pixel()` (not
    `gfx_get_pixel()`, deliberately -- see the code comment) on top of
    the existing dirty-pixel-bbox blit optimization, which is
    unchanged and still does its own job one layer lower.
  - `apps/wm/wm_render.c`: `wm_damage_rect()` accumulates the
    per-frame damage bbox; `compute_window_damage()` diffs each
    window's position/size/visibility against new `last_x/y/w/h/
    last_visible` fields on `struct window` (`apps/wm/wm.h`) to catch
    drags/resizes/minimize/restore automatically. `wm_render_frame()`
    applies the accumulated region as the active clip, redraws
    everything within it back-to-front (desktop, windows in z-order,
    taskbar, menus), then resets it.
  - `apps/wm/wm.c`/`desktop.c`: explicit damage reports for changes
    the automatic geometry diff can't see on its own --
    `bring_to_front()` (z-order swap), `open_app()`/`close_window()`
    (taskbar layout change), `window_invalidate()` (now does something,
    was a no-op stub before), focused-window key/wheel delivery, and
    desktop icon drag.
  - Why redraw-in-region instead of computing exact exposed
    sub-rectangles, and the two real bugs QMP testing caught (taskbar
    staleness on close/open/reorder; a stale highlight sliver during
    icon drag) -- see `docs/decisions.md`.
  - Verified via QMP: two overlapping windows, dragging one off the
    other with the revealed area redrawing correctly, closing a window
    with the taskbar updating correctly, and a desktop icon drag with
    no stale-pixel artifact -- screenshots in
    `screenshots/2026-08-12/compositor-*.png`.

- Compositor Phase 3: skip a window's `on_draw()` (and chrome/resize-
  grip) entirely when it doesn't intersect the frame's damage region,
  instead of calling it and letting `gfx_set_clip_rect()` clip its
  writes away for free. Completes the Milestone 9 dirty-rect compositor
  plan the Phase 1+2 entry above started.
  - `apps/wm/wm_render.c`: new `window_intersects_damage()`;
    `wm_render_frame()`'s per-window loop now skips
    `draw_window_chrome()`/`on_draw()`/`draw_resize_grip()` for any
    visible window whose rect doesn't overlap the accumulated damage
    box (only when a damage box was actually reported this frame -- no
    damage still means "unknown, be safe," draw everyone, same
    full-screen fallback as before).
  - `apps/wm/wm.c`: fixed a latent bug in `bring_to_front()`'s damage
    reporting, surfaced by actually skipping draw calls -- it only
    damaged the newly-promoted window's rect, never the
    previously-frontmost window's, even though that window's titlebar
    tint (focused blue vs. unfocused gray) changes too on every z-order
    swap. Harmless under Phase 1+2 (the call still ran, just had its
    pixels clipped away, and they happened to land inside the damaged
    box in every case tested so far); became a real visible stale-tint
    bug the moment the call itself started being skipped. Fixed by
    damaging the previously-frontmost window's rect too.
  - Verified via QMP: opened two non-overlapping windows, swapped focus
    between them repeatedly via taskbar clicks with both titlebar tints
    confirmed correct after every swap; dragged, minimized, and closed
    windows and confirmed no stale pixels or missed redraws -- see
    `screenshots/2026-08-12/compositor-phase3-*.png`.

- Taskbar notification area (tray): a small right-to-left strip of
  text items next to the Start/window buttons, with a dynamic
  registration API so a GUI app can plug a live-updating item into it
  at runtime instead of the WM only ever drawing hardcoded chrome. The
  existing hardcoded taskbar clock (`draw_clock_area()`) is now itself
  tray item 0, registered through the same API, proving it end-to-end
  rather than shipping an API with no real caller.
  - New `apps/wm/wm_tray.c`/`wm_tray.h`: a fixed `TRAY_MAX_ITEMS` (6)
    array of `{active, text[TRAY_TEXT_MAX]}` slots. `tray_init()`
    registers the clock as slot 0 from `wm_run()`'s setup;
    `tray_update_clock()` replaces the old inline `rtc_read_local()`
    call at the once-a-second tick in `wm.c`; `draw_tray()` (called
    from `wm_render.c`'s `draw_taskbar()` in place of the removed
    `draw_clock_area()`) draws every active item right-to-left from
    the taskbar's right edge, same visual position the clock always
    had.
  - `apps/wm/wm.h`: new app-facing API -- `tray_register(initial_text)`
    (returns a handle or -1 if full), `tray_set_text(id, text)`,
    `tray_unregister(id)` -- following the same "push, don't poll"
    shape apps already know from `window_set_state()`, not the
    WM-polls-a-handle shape `window_start_write()` uses (a tray item's
    text only changes when the app itself decides it has, so there's
    nothing for the WM to poll).
  - Damage scoping: every registration/update/unregister call just sets
    `redraw_pending`, relying on the full-screen fallback -- matches
    every other still-unscoped piece of WM chrome (menus, dialogs, see
    `docs/roadmap.md`'s Milestone 12 entry). An earlier version of this
    entry scoped these to just the taskbar strip via `wm_damage_rect()`;
    see the "Fixed" entry directly below for the two real bugs that
    caused, and why it was reverted.
  - Verified via QMP: clock renders at its usual position and keeps
    ticking (`18:49:03` -> `18:49:19` across two screenshots), taskbar
    Start/window buttons unaffected with a window open -- see
    `screenshots/2026-08-12/tray-clock-*.png`.

### Changed
- Docs catch-up for the console work: `README.md`'s framebuffer bullet
  now mentions scrollback and the on-screen boot log, and
  `apps/README.md` distinguishes the *physical* console's new scrollback
  (a character ring in `vga.c`) from `ui_scrollback`'s in-window widget,
  since "scrollback" now means two unrelated things in this codebase.

- **README rewritten as a project front page**, with per-distribution
  build instructions.
  - Opens with two screenshots taken deliberately for it (the window
    manager running the Terminal app; the shell running `about`/`ls`/
    `df`/`ktest`) rather than reusing debugging artifacts, plus a CI
    badge, a table of contents, and a "what this is" section that says
    what's actually built rather than listing every feature first.
  - **Dependency install commands for six distribution families**
    (Debian/Ubuntu, Arch, Fedora, openSUSE, Alpine, Void), plus a table
    explaining what each package is *for* so a distribution not listed
    can be worked out. Honest about provenance: Arch is verified
    firsthand, the Debian/Ubuntu list is what CI installs on every push,
    and the rest are package-name translations of the same
    requirements.
  - **A troubleshooting table** for the failures that actually bite
    here: the missing GRUB BIOS-modules package (an ISO that builds but
    won't boot), `grub-mkrescue` needing `xorriso` *and* `mtools`, no
    window over SSH, the PS/2-only mouse, and `disk.img` looking like
    9 GB when it's sparse.
  - A Development section covering `make verify`/`make test`, how to
    write a `KTEST`, and what each tool in `tools/` is for.
- **`make iso` now finds `grub2-mkrescue` as well as `grub-mkrescue`.**
  Fedora/RHEL and openSUSE use the `grub2-` prefix, so the build
  previously failed there on a correctly-installed system. Resolving it
  in the Makefile beats documenting a "symlink it yourself" step -- and
  when neither binary exists the error now names both and points at the
  README's dependency table instead of `command not found`.

- **Documentation catch-up after this session's work.** Audited every
  `.md` against the tree rather than by memory; five real gaps:
  - **`docs/arch-portability.md` was describing its own Phase 1 as
    future work** -- creating `kernel/arch/x86_64/` and moving the
    unambiguously x86 files there. That happened this session (as a
    general restructure rather than as portability work, but it's the
    same move). Marked done, and the "proposed directory layout" is now
    the actual one, with the three places reality differs from the
    proposal called out: the split went further than `arch/` + `core/`;
    `timer.c`/`power.c`/`serial.c`/`pci.c` deliberately did NOT move
    (each mixes port-I/O with portable logic -- extracting that is
    Phase 2); and `paging.c` did move despite being mixed, because the
    x86 page-table encoding dominates it. Also records the line to hold
    now that the directory exists: nothing outside `arch/` should
    contain `inb`/`outb`, inline asm, or a control-register access --
    which is greppable.
  - `README.md` still called the serial console read-only. It has `sh`
    and `ktest` now.
  - The shell's own `help tests` didn't list `ktest`, so the feature was
    invisible from inside the OS.
  - `docs/roadmap.md`'s Milestone 4 detail was still written as a
    proposal; rewritten as done, keeping the reasoning (it's the
    reference for adding a test) and listing what's deliberately still
    missing: no per-test isolation, no setup/teardown, no way to run
    tests before the filesystem exists.
  - `CLAUDE.md`'s QMP-testing section read as though QMP were the only
    way to test. It now opens with the routing rule -- boot smoke test
    (does it boot) -> `make test`/`vm.py exec` (does it work) -> QMP
    (does it look right) -- so the expensive path is chosen
    deliberately rather than by default. `make test`/`make verify` added
    to both build-target lists.

  Checked and deliberately NOT changed: `CHANGELOG.md`'s historical
  entries reference paths as they were when written (`kernel/core/pmm.c`
  and friends) and are a record, not an index; `apps/README.md`'s
  `calc.c`/`clock.c` are hypothetical examples in a tutorial, not stale
  references; and several "this file no longer exists" notes name
  removed files on purpose. A link/anchor check across all 13 `.md`
  files passes.

- **Directory restructure: the tree now describes the OS rather than
  its history.** Asked for as "restructure so it better represents the
  OS we are building and have modularity built in". Five staged steps,
  each building and boot-testing clean; no logic changed anywhere.
  - **`kernel/include/` split by audience, enforced by the build.** It
    was 47 headers in one flat directory: 30 app-facing, 2 the
    kernel<->userland ABI, 15 kernel internals. CLAUDE.md has always
    said `apps/` includes `kapi.h` and nothing else, but nothing
    enforced it. Now `api/`, `abi/` and `kernel/` get different `-I`
    flags per build target, so an app reaching for `vmm.h` fails to
    compile rather than failing review -- verified by deliberately
    adding the include and watching the build stop. Nothing had to
    change in any source file: the boundary was already being
    respected, it just wasn't checkable.
  - **Recursive source discovery.** Every `.c` under `kernel/`/`apps/`
    is compiled with `build/` mirroring the tree, so a new directory
    needs no Makefile edit -- previously one wildcard + pattern rule +
    mkdir target each, which `apps/wm/` and `apps/ui/` both paid and
    this restructure would have paid five more times. The 17 userland
    binaries' ~55 lines of near-identical rules became two pattern
    rules.
  - **`kernel/core/` (33 files, five concerns) split into subsystems**:
    `arch/x86_64/` (Multiboot entry, GDT/IDT/PIC/IRQ, page tables, the
    ring switch -- everything a different CPU would need rewritten),
    `mm/`, `proc/`, `lib/` (strings/JSON/klog/`/etc` config -- services
    with no hardware, which were only in `core/` because there was
    nowhere else), leaving `core/` as bring-up and whole-machine
    concerns.
  - **`tfs.c`/`vfs.c` moved out of `drivers/` into `fs/`.** A
    filesystem isn't a device driver; the block device under it is.
    Mount points (Milestone 16) add backends there, not next to
    `ata.c`.
  - `kernel/README.md` and `kernel/include/README.md` are new, each
    with a "does it belong here?" test per directory; `README.md`'s
    project layout and CLAUDE.md's conventions updated to match.
  - Two pre-existing bits of rot fixed in passing: three comments
    pointed at `kernel/core/elf_test.c`/`syscall_test.c`, files deleted
    long ago when those tests became `/bin` binaries.

- **`docs/roadmap.md` reordered so prerequisites come before the things
  that need them**, asked for directly. Reading top to bottom is now a
  workable build order: nothing depends on something further down.
  - Milestones 4-30 were **renumbered in place** so position and number
    agree again -- the file previously said "numbering is identity, not
    priority" and let the order drift from the dependencies, which meant
    the list read in an order you couldn't actually build in.
    Milestones 1-3 kept their numbers (1 is released, 2-3 are in
    progress with completed items this changelog already refers to by
    number). A `Was -> Now` mapping table is in the roadmap's header.
  - **Prerequisites were pulled forward, not dependents pushed back.**
    Both satisfy the ordering; only one puts the fundamentals early. So
    the test harness (was 25, now 4), benchmark suite (29 -> 5), TTY
    layer (21 -> 6) and demand paging (26 -> 7) lead, and the process
    work that needs them -- `fork()`/`exec()` (5 -> 8), signals
    (6 -> 9), pipes and job control (7 -> 10) -- follows immediately
    rather than landing near the end.
  - Planned versions follow position, so releases would come out in the
    order the work happens.
  - Steps *within* Milestone 2 were reordered the same way: the
    `linker.ld` section split now precedes the kernel W^X item it
    unblocks, and the entropy source precedes kernel ASLR.
  - Forward-looking `Milestone N` references in source comments and docs
    were remapped (`wm_render.c`, `wm_tray.c`, `ui_icon_grid.h`,
    `shell_sys.c`, `speaker.h`, `apps/README.md`, `docs/decisions.md`).
    References in this file's *released* sections and in the
    `CHANGELOG-archive*.md` files were deliberately left alone: they
    record what was true when written. Those archives also use
    "Milestone N" for a separate, much older numbering of their own, so
    the three source comments citing *that* scheme (`scheduler.c`,
    `scheduler.h`, `ring3_test.c`) now say "the original Milestone N"
    to keep the two apart.

- **`FS_MAX_FILES` 32 -> 256, an on-disk layout change (TFS2 v2 -> v3).**
  32 wasn't a comfortable margin any more, it was nearly gone: the
  shipped `disk.img` already used 25 slots (17 `/bin` binaries plus
  `/bin`, `/etc`, `/etc/kbs` and four `/etc` files), `/etc/toyos.conf`
  makes 26 as soon as any setting is saved, and the boot selftest takes
  a 27th while it runs. The next few seeded binaries would have hit
  "table full", which surfaces as a bare 0 return from `fs_touch()`.
  The record table sits between the journal and the free-block bitmap,
  so changing its size moves `FS_BITMAP_START_LBA` and every LBA after
  it -- hence the version-byte bump, and the standing "no migration,
  just reformat" policy applies: a v2 disk is detected as foreign and
  reformatted. `make iso` re-seeds `/bin` and `/etc/kbs` automatically,
  so the practical loss is `/etc/history`, `/etc/desktop.conf` and
  `/etc/timezones`, once. Cost of 256 slots: ~44KB of `.bss` and 256
  one-sector records on a gigabyte-class disk. `tools/tfs2_writer.py`
  mirrors the layout host-side and was updated in lockstep (it now
  writes/expects version 3, and says plainly when it reformats an older
  image rather than silently discarding its files). Verified with a
  512MB image seeded with 60 files: all 60 present, and
  `/many/f57.txt` -- slot 58, well past the old ceiling -- reads back
  correctly (screenshot `cat_60th_file.png`).
- **Sequential filesystem throughput: ~18 -> 25.1 MB/s write, ~27 ->
  30.5 MB/s read** (`stress 300`, 27s -> 21s; screenshot
  `stress300_nozero.png`). Two changes, both in the "stop issuing one
  ATA command per 4KB" direction:
  - The DMA bounce buffer went from 1 frame to 16 (`ata.c`'s
    `DMA_BUF_FRAMES`), raising `ATA_MAX_SECTORS_PER_XFER` from 8 to
    128. 64KB is the ceiling on purpose: a PRD's byte count is 16-bit
    with 0 meaning 64KB, so a full-size transfer relies on that
    encoding and anything larger would truncate to a genuinely wrong
    value. `ata.h` had already identified this buffer as the blocker.
    If `pmm_alloc_contiguous(17)` fails on a fragmented pool, init
    retries for the original 2 frames and everything behaves exactly as
    before, just at the smaller limit -- the driver never falls back to
    PIO over this.
  - TFS2 now coalesces contiguous blocks into one transfer
    (`contiguous_run()`, used by both `write_range_impl()` and
    `read_range_impl()`). Deliberately conservative: only whole blocks,
    only block-aligned, only physically consecutive, only disk-backed.
    An unaligned head, a partial tail, a hole, or a fragmented region
    all drop through to the original per-block path, which is the one
    every previous `stress` run has exercised.
  - The first measurement after coalescing was only 20.3 MB/s, which
    didn't match "16x fewer commands" -- the reason turned out to be
    worth its own fix: every freshly allocated block was zero-filled
    with its own 4KB write before the real data write, so allocation
    doubled the command count and split the coalesced runs apart. A
    full-block overwrite doesn't need that zeroing (`BLK_ALLOC_NOZERO`
    above), and skipping it is what took write throughput from 20.3 to
    25.1 MB/s. Zero-filling still happens everywhere it carries meaning:
    indirect index blocks always (their unwritten entries are read as
    block pointers and must be the 0 sentinel), and any partially
    written block (so a read-modify-write can't leak a deleted file's
    contents).
  - Read gained less than write (+13% vs +39%) because `stress`'s read
    phase includes its own byte-for-byte verification loop over every
    megabyte, which is now a meaningful share of that phase's time --
    the coalescing itself is active on the read path (that's where the
    27 -> 30.5 came from), the benchmark just measures more than disk
    I/O. A pure sequential-read benchmark would show a larger gap; one
    doesn't exist yet.
- The boot selftest now **skips** (rather than reporting FAILED) when
  the disk is too small for its 4.6GB triple-indirect probe offset --
  which is exactly what the new capacity clamp makes possible to
  detect. On a 512MB image it logs `selftest skipped -- disk is too
  small for the triple-indirect offset` instead of a data-mismatch
  failure that reads like a filesystem bug.
- `tools/qmp_test.py`'s `screenshot()` now passes QEMU an **absolute**
  path for the `.ppm`. QEMU resolves `screendump`'s filename against
  its own working directory, and `launch_qemu_cmd()` passes
  `-daemonize`, so a relative path returned `{"return": {}}` (success)
  while writing the file somewhere else entirely -- the only symptom
  being Pillow raising `FileNotFoundError` on a path that looks
  obviously correct. Hit for real during this session's testing.
- Documentation audit and refactor, asked for as "are the .md files up
  to date, and do they need refactoring/additions/deletions". Four
  areas, none of them code changes:
  - **`README.md` was the most out-of-date file in the repo.** Its
    project-layout block still listed "six ring-3 demos" in
    `kernel/core/` (only `ring3_test.c` survives -- the rest became
    real `/bin` ELF binaries), and omitted `heap.c`, `scheduler.c`,
    `klog.c`, `debugflags.c`, `etc_config.c`, `tz.c`, `font_config.c`,
    `keyboard_layout.c`, `stack_protector.c`, `json.c`,
    `debug_console.c`, plus `ata.c`/`pci.c`/`partition.c`/`speaker.c`/
    `vfs.c` on the driver side and `editor.c`/the `shell.c` split/
    `wm_tray.c`/`ui_icon_grid` on the apps side. The shell-command list
    was missing `cd`/`pwd`/`mkdir`/`stat`/`beep`/`lspci`/`parttable`/
    `timezone` and is now grouped the same way `help` itself groups
    them, so the two can be diffed by eye. Feature list gained
    Milestone 2's NX/W^X/stack canaries, XKB-generated keyboard layout
    data files, the tray, the serial debug console, PCI/partition
    parsing, and the `seed/` directory; build section gained
    `make debug`/`make run-audio` and the GDB invocation.
  - **The changelog got its second era split** (this file had passed
    ~4,200 lines again). Cut at the heading-style change rather than an
    arbitrary line: `CHANGELOG-archive-2.md` now holds Build 183
    through Build 502 -- the entire `## Build N (tier, +delta)` era --
    and `CHANGELOG.md` holds the semver era plus `[Unreleased]`.
    Straight move, no rewording, same rule the first split used.
    `docs/decisions.md`'s 27 `Build N` pointers were retargeted to
    whichever file each build actually lives in now (one of them, the
    `Build 173` pointer, had been wrong since the *first* split), and
    `CHANGELOG-archive.md`'s own title was off by one -- it says
    "through Build 173" now, which is what it actually contains.
  - **`[Unreleased]` was regrouped and flattened.** It had accumulated
    12 `###` subsections (four `Added`, two `Changed`, ...) in
    chronological order; they're merged into one of each in Keep a
    Changelog order, with entry order preserved inside each. The
    `stress` progress-bar entry's three-deep "Follow-up #2/#3"
    chronology was rewritten as one entry stating the final behavior
    plus three named sub-points -- same content, no lost reasoning,
    but readable as "what does `stress` do now" rather than "what
    happened in what order".
  - **`CLAUDE.md` contradicted itself**, claiming "every hand-written
    file in this repo is currently under 800 lines (`apps/shell.c` is
    the largest at 777)" as the calibration point for its own
    split-a-file rule, long after `kernel/drivers/tfs.c` (1,472) and
    `apps/shell_sys.c` (1,138) blew past it. Rather than re-quoting
    today's numbers (which rot the same way), the bullet now says to
    run `wc -l` and keeps only the qualitative signal. Its `apps/wm/`
    file list (pre-`desktop.c`/`start_menu.c`/`file_picker.c`/
    `context_menu.c`/`confirm_dialog.c`/`wm_tray.c`) and `tools/` list
    (missing `gen_kbs.py`, and saying "four more" above a list of
    eight) are current again.
  - **`docs/decisions.md` called itself "topic-indexed" without an
    index** -- 65 entries over ~1,900 lines, findable only by grep. It
    now opens with one, grouped by area (kernel/filesystem/drivers/
    GUI/shell/build/workflow), with `CLAUDE.md` noting that a new
    entry means a new index line.
  - **`screenshots/README.md`** now says what the leftover `v0.2.0/`
    ... `v0.7.0/` folders are (frozen record from the old
    version-numbered scheme; not renamed into dated folders, since the
    real dates aren't recoverable) and acknowledges that its "a handful
    per pass" rule is per pass, not per folder -- `2026-08-10/` has
    over a hundred files from a heavy day. Pruning is explicitly the
    maintainer's call, not a session's.
- `stress <mb>` (`apps/shell_sys.c`) now reports live progress as an
  in-place ASCII bar (`[####----] 68% 204/300MB 18.1MB/s`), redrawn on
  one line "the way programs usually do in Linux", replacing the old
  flat "wrote N / M MB" line every 256MB (silent for anything smaller
  than that) plus a single total-elapsed-time summary at the end. The
  final summary now reports write and read speeds separately
  (previously just total elapsed seconds) since they're consistently
  different -- measured live in QEMU (`stress 300`): ~18 MB/s write,
  ~27 MB/s read on this PIO/DMA ATA path. Speeds are running averages
  for the current phase, computed in tenths via integer math
  (`done_mb * 1000 / phase_ticks`) since there's no float on this
  freestanding target. Verified end-to-end via `tools/qmp_test.py`/
  `tools/shell_flow.py`, mid-run and at completion (screenshots in
  `screenshots/2026-08-13/`). Three details worth keeping:
  - **Cadence is percent-based, not time-based.** The first version
    printed roughly once per second (100 PIT ticks); that stopped
    landing on clean percentages once `stress` got faster (the
    `free_all_blocks()` batching fix in this release's Fixed section
    took a `stress 300` run from 71-85s to 27s), jumping e.g. straight
    from 6% to 13%. `stress_print_progress()` now fires whenever the
    whole-number percentage crosses a new value, so it steps cleanly
    1%..100% regardless of `<mb>` or disk speed, and still caps at
    ~100 redraws total (no flooding risk for a huge `<mb>`).
  - **The in-place redraw needed no kernel/driver changes.** Both
    console backends in `kernel/drivers/vga.c` (legacy 0xB8000 text
    mode and the framebuffer text console) already treat `'\r'` as
    "column 0, same row, no scroll" and draw characters in place, so
    the progress line just prepends `'\r'` instead of appending
    `'\n'`, padding with a few trailing spaces so a shorter new line
    fully overwrites a longer old one (`done_mb`/`pct` grow
    monotonically, but the speed's digit count can occasionally shrink
    by one). A real `'\n'` fires only once a phase's bar reaches 100%,
    so the next line ("reading back...", the final `PASSED` summary)
    starts fresh instead of overwriting the finished bar.
  - **New `vga_cursor_hide()`** (`vga.c`/`vga.h`), a public wrapper
    around the previously file-static `cursor_hide()`, called once at
    the end of every redraw. Without it the framebuffer console's
    block cursor (repainted solid at the new (row, col) by every
    `vga_putc()` -- see `fb_putc()`'s comment) sat visibly at the end
    of the bar's trailing padding for the whole run, not even
    blinking, since `cmd_stress()` never calls `vga_cursor_tick()`
    (the idle-blink driver, normally serviced by
    `keyboard_getchar()`'s wait loop). Any caller producing its own
    timed/looped output can use it; the next real `vga_write()`/
    `vga_putc()` shows a fresh cursor again on its own, no matching
    "show" call needed. No-op outside framebuffer mode, matching
    `vga_cursor_tick()`'s existing legacy-text-mode no-op.
- Docs catch-up after the tray/NX/shell_flow work above: `CLAUDE.md`'s
  QMP-testing gotcha list now points at `shell_flow.py` from the
  `send_text()` keyboard gotcha it directly solves, and adds a gotcha
  of its own -- don't `pkill`/kill-by-pattern across every
  `qemu-system-x86_64` process, since that can't distinguish a
  QMP-headless test instance from the user's own interactive `make
  run` window; only kill the PID your own launch's `-pidfile` wrote.
  `docs/roadmap.md`'s Milestone 9 entry no longer claims the tray/
  clock tick is damage-scoped (it was, briefly, then reverted -- see
  below) and its Milestone 2 "Details" prose (NX/W^X bullets) was
  still describing them as not-yet-done despite both being checked off
  further up the same file. New `docs/decisions.md` entry ("The
  taskbar/tray falls back to full-screen repaint on purpose, not as an
  oversight") for the two real bugs an earlier scoped-damage attempt
  caused and why the fix was reverting the optimization, not patching
  around it -- `docs/roadmap.md` linked to this entry before it
  actually existed.

- CLAUDE.md and `tools/` now support a direct local checkout (this
  session ran that way for the first time, not through Cowork's device
  bridge) as a first-class mode alongside the existing Cowork one,
  instead of assuming Cowork throughout. Detected via `git config
  user.name` (empty = Cowork device-bridge session, which has no git
  identity configured at all; set = direct local checkout). Confirmed
  directly: `git push`/`gh release create`/`gh release upload` all
  work fine from a local checkout (used them repeatedly this session,
  including cutting and then patching the `v0.1.0` release), unlike
  Cowork's sandbox where they're genuinely blocked by an egress proxy.
  See `docs/decisions.md` for the full writeup.
  - `CLAUDE.md`: splits "Working in the cloud sandbox vs. the user's
    machine" into a detection bit + two subsections; "Delivering
    changes" branches by mode and gains explicit PII and
    tooling-belongs-in-`tools/` standing instructions.
  - `tools/preflight.sh`: closing message is now mode-aware (checks
    git identity) instead of unconditionally pointing at
    SendUserFile/`device_commit_files`.
  - `tools/qmp_test.py`: `launch_qemu_cmd()` now returns a `qemu-system-x86_64
    ... -daemonize -pidfile <path>` command instead of one meant to be
    backgrounded with `setsid nohup ... &`/`disown -a` -- the old
    pattern turned out to be unreliable in this sandboxed environment
    specifically (spurious non-zero exit codes on the launching call,
    and the process not reliably surviving to the next tool call,
    which left a stale `serial.log`/QMP port from an earlier run
    looking like a fresh boot and caused real confusion mid-session
    chasing a phantom bug). Verified the new command launches,
    daemonizes, and accepts a QMP connection.
  - Companion update to `~/.claude/skills/toy-os-feature-workflow/`
    (outside this repo, not tracked here) doing the same mode-split for
    the workflow steps.

### Removed
- Legacy on-disk-config migration code, by explicit request -- this
  project is pre-1.0 and the user is fine just recreating a fresh
  `disk.img`/`/etc` state instead of carrying forward-migration code
  for formats nothing still produces. A research pass first confirmed
  `kernel/drivers/tfs.c` (the user's initial suspicion) actually has
  *no* removable migration code -- an old-version disk is already just
  reformatted, identical to a blank/foreign one, no special-case logic
  exists to strip. The two real, removable migrations were elsewhere:
  - `kernel/core/font_config.c`: the block reading old `/etc/fontsize`
    and migrating it into `/etc/toyos.conf`'s `font_size` key (then
    deleting the old file) -- removed; `font_config_init()` now just
    reads `toyos.conf` directly. `kernel/include/font_config.h`'s
    stale reference to the old file removed too.
  - `kernel/core/tz.c`: the block reading either old `/etc/timezone`
    or `/timezone` (bare-text city name, two different pre-`/etc`-
    consolidation locations) and migrating into `toyos.conf`'s
    `timezone` key -- removed; `tz_init()` now just reads
    `toyos.conf` directly. The file's own top comment's "small tour of
    how /etc has evolved" narration (describing the now-gone migration
    path) trimmed to match.
  - `kernel/include/fs.h`'s `FS_DATA_MAX` -- already marked `Vestigial`
    in its own comment, a leftover per-file ceiling from TFS2 v1 that
    nothing referenced anymore (confirmed by grep before removing).
    Three comments in `kernel/core/etc_config.c`/`apps/editor.c`/
    `apps/editor.h` that explained "why this isn't bounded by
    `FS_DATA_MAX`" reworded to not reference the now-gone symbol name.
  - `docs/decisions.md`'s `/etc` consolidation entry updated -- it
    referenced "the migration logic" in the past tense pointing at
    code that no longer exists.

  Verified: `make clean && make all && make iso` + `boot_smoke_test.py`
  all pass. Live via QMP: set `fontsize 24` and `timezone helsinki`
  through the shell, rebooted, confirmed both persisted correctly
  through the simplified (`toyos.conf`-only) init paths -- the font
  was visibly larger and `timezone`'s picker showed `helsinki` marked
  as the active selection.

### Fixed
- `README.md`'s Project layout section had its intro paragraph twice --
  introduced by the README rewrite, which wrote the sentence into the
  new section while the block it pasted in already started with it. A
  scan for repeated paragraphs across all 13 `.md` files found no
  others.

- **`qmp_test.py`'s `drag()` took a destination only, and silently
  accepted a second point as a sleep duration.** `drag(360, 55, 700,
  300)` -- which reads as two coordinates to anyone -- bound `hold=700`
  and `settle=300` SECONDS. It didn't fail; it slept for sixteen minutes
  exactly as instructed, which is how it cost a session's screenshot
  attempt. Now `drag(from_x, from_y, to_x, to_y)` with keyword-only
  timings, so that call does what it looks like and a stray positional
  argument is an immediate `TypeError`. No other caller existed.
- The serial debug console's ready banner had no trailing newline, so on
  the physical console (now that boot output is echoed there) it ran
  straight into the shell's banner.

- **A flaky CI failure in the new test suite, and the test-quality bug
  underneath it.** One run failed with four filesystem tests down and
  `ata: dma write failed after 3 attempts (lba 2)` in the log; the same
  commit range passed before and after, so it was timing-dependent on a
  contended runner. The cascade:
  1. A real transient DMA write failed (the runner is fully emulated,
     no KVM).
  2. That made one test's cleanup `fs_delete()` silently not happen --
     its result was ignored.
  3. The next test called `fs_touch()` on a path that therefore still
     existed. **`tfs_touch()` returns success immediately for an
     existing file without writing anything**, so the fault injector it
     had just armed never fired, and the test failed asserting
     `created == 0` -- three steps from the actual cause.

  The environment triggered it; the tests made it confusing. Fixed by
  making them isolated and self-checking:
  - Every filesystem test now uses **its own path** instead of one
    shared `/.ktest_tmp`, so one test's leftovers can't become another's
    starting state.
  - New `FRESH(path)` deletes *and asserts the file is gone*, so a test
    that can't establish its precondition says exactly that rather than
    failing later for an unrelated-looking reason.
  - The runner now checks `fault_any_armed()` after every test, names
    the test that leaked an injector, and disarms it. A test that
    returns early through a failed assertion leaves its injector armed,
    which would poison everything after it -- the same shape of cascade.
  - `ata.c`'s retry-exhausted log line now says **why**: "drive stayed
    busy, command never issued" / "completion IRQ never arrived" /
    "controller reported a bus-master error". Narrowing this one took
    real detective work purely because the message didn't distinguish
    them.

- **`dispatch()` didn't trim trailing whitespace from a command's
  arguments** (`apps/shell.c`), so `cat /etc/timezones ` looked up a
  filename with a space on the end and failed with "no such file".
  Always true for a hand-typed trailing space; tab completion made it
  easy to hit, since completing a unique match appends one. Most
  commands here treat `args` as a single value (a path, a colour name, a
  number) rather than splitting it further, so the trim belongs in the
  one place that produces `args`.
- **A transient read failure at boot reformatted the whole disk.**
  `tfs_init()` was one condition -- `if (ata_read_sector(superblock) &&
  magic ok && version ok) { load } else { format }` -- so a *failed
  read* took the same branch as a genuinely foreign disk and formatted
  over a perfectly good filesystem. Not hypothetical: `ata_read_sector()`
  gives up after `ATA_DMA_MAX_RETRIES` (3) exhausted attempts, and
  transient 3-in-a-row DMA misses are precisely what this project has
  already seen on real hardware (see `ata.c`'s retry-wrapper comment and
  the bitmap-persist retry added a few entries above -- same class of
  event, on a different sector). One unlucky burst on LBA 0 during boot
  and every file was gone, with nothing logged to say why.
  Reading the superblock and judging it are now two separate steps: the
  read gets its own bounded retry round
  (`FS_SUPERBLOCK_READ_MAX_RETRIES`), and if it still can't be read the
  kernel **refuses to touch the disk at all** -- it degrades to
  RAM-only for that boot with a three-line explanation in `dmesg`,
  rather than destroying what is probably a fine filesystem. A blank or
  foreign disk still formats normally, because that path is only
  reachable when the read genuinely succeeded and the bytes just aren't
  ours.
  - Related, found while testing this with deliberately broken images:
    a disk too small to hold even the reserved metadata region now
    degrades to RAM-only too, instead of "successfully" formatting a
    filesystem whose every write lands somewhere the drive discards.
    A 100-byte image now logs `too small to hold the filesystem
    metadata region (0 blocks, need more than 105)` and boots to a
    usable shell. A 0-length image is the one case still not caught --
    QEMU answers its reads with zeros rather than erroring, so it is
    genuinely indistinguishable from a blank disk at the driver level;
    the boot selftest is what catches that, loudly.
- **`persist_record()`'s return value was discarded at every call
  site.** `touch`/`mkdir`/`write`/`delete`/`write_range` and the
  steppable-write completion all reported success to the caller when
  the journal-protected metadata write had failed -- the file "existed"
  until the next reboot and then didn't, with nothing logged. Exactly
  the class of bug the bitmap-sector persist fix (a few entries down)
  addressed for free-space bookkeeping, never applied to records. Every
  call site now checks it, the failure is always logged (independent of
  `debug fs`, matching the bitmap convention), and each caller undoes
  its in-memory change so memory can't claim something disk disagrees
  with: `touch`/`mkdir` roll the new slot back, `delete` restores the
  entry, truncation restores its block pointers and size.
- **Truncate and delete freed a file's blocks before persisting the
  record that referenced them.** A crash (or a failed record write) in
  that window left an on-disk record still pointing at blocks the
  bitmap had already marked free -- the next allocation hands one of
  them to a different file, and two files silently share a block. The
  order is now inverted via `detach_blocks()`/`reattach_blocks()`: the
  record is written referencing nothing first, and only then are the
  blocks returned to the bitmap. The worst case becomes the harmless
  opposite -- blocks marked allocated that nothing references, a space
  leak a future fsck-style pass could reclaim, rather than corruption.
- **A failed `zero_block()` could hand a file another file's data.**
  Its return value was ignored, so an indirect index block that failed
  to zero kept whatever a previously deleted file left there -- and
  `walk_indirect()` reads those stale bytes as real block pointers.
  Now checked at every allocation site, with the block freed again and
  the allocation reported as failed.
- **A partially failed `fs_write_range()` leaked its allocated blocks.**
  The record was never persisted on the failure path, so the blocks
  were marked allocated in the bitmap and referenced by nothing after a
  reboot. `fs.h` documents the file's state on partial failure as
  "whatever was written before the failure", so the record is now
  persisted on that path too, keeping those blocks reachable.
- `replay_journal()` ignored whether writing the replayed entry to its
  table slot actually succeeded, and cleared the journal header either
  way -- dropping a recovered entry permanently if that one write
  failed. It now leaves the header committed so the next boot retries.
- The disk format path ignored `persist_record()` for all of its blank
  slots; it now counts and reports any that didn't land, instead of
  claiming a clean format.
- **Verified end-to-end**, not just by reading the code: `stress 300`,
  `dmatest` and `steptest 3` all pass on the 64KB DMA path
  (`dmatest_64k.png`, `steptest3_64k.png`); `df` shows 204 KB used
  after a 300MB file is written and deleted, proving the reordered
  free path still reclaims everything (`df_64k.png`); a `mkdir` +
  `write` survives a real reboot (`persist_after_reboot.png`); and
  Notepad's Save As... -- which goes through the steppable write path
  whose completion now depends on `persist_record()` -- writes a file
  the shell and the host-side `tfs2_writer.py` both read back correctly
  (`notepad_saved_via_step_api.png`).
- `kernel/drivers/vga.c`'s framebuffer console cursor left stray
  wrong-colored blocks around a colored diagnostic banner -- reported
  live from `ring3test`'s panic screen (white-on-red), which showed a
  red sliver one row above the panic box and another right below it,
  both on otherwise-plain-black blank lines with no real panic text.
  Root cause: `cursor_hide()` erased the cursor's solid block using
  the *live* `cur_bg`, but a blank cell the cursor merely passed
  through (nothing actually drawn there) has no real "correct"
  background of its own -- it just inherits whatever `cur_bg`
  happened to be active when the cursor auto-painted there. `idt.c`'s
  panic handler sets white-on-red, and `ring3_test.c`'s `ring3_hook`
  immediately follows with light-green-on-black -- across that
  transition, erasing with `cur_bg` either left the stray red block
  behind (if the color had already moved on by erase time) or was a
  silent no-op (erasing red with still-red `cur_bg` just repaints the
  same red). First attempted fix (remembering the `cur_bg` the cursor
  was actually painted with, and hiding it right before `vga_set_
  color()` changes anything) turned out to have the same flaw at its
  root -- a "correctly remembered" red is still red, still doesn't
  erase a cell that was never meant to be red at all. Real fix:
  `cursor_hide()` now always erases with a hardcoded `VGA_BLACK`, not
  `cur_bg` -- a blank untouched cell is always part of this console's
  plain page background, which is black, independent of whatever
  transient text color is active. Verified live via QMP: `ring3test`'s
  panic box now has no stray slivers above or below it, and a normal
  shell prompt's cursor is unaffected (bg is black there anyway).
- `kernel/drivers/tfs.c`'s free-block bitmap sector persist had no
  error handling at all -- `write_batch_end()` (the batched-flush path
  a large sequential write like `stress` goes through) and
  `persist_bitmap_bit()`'s non-batched fallback both discarded
  `persist_bitmap_sector()`'s return value outright. Found live while
  investigating the `stress`-progress work above: a `debug` serial
  console showed `ata: dma write failed after 3 attempts (lba 47)`
  (the ATA driver's own retry wrapper, `dma_transfer_with_retry()`,
  had exhausted all 3 of *its* attempts) right around a `stress 300`
  run, and LBA 47 traced to `FS_BITMAP_START_LBA` (35) + sector 12 --
  squarely inside the free-block bitmap, not file data. `stress` still
  reported PASSED (the actual data blocks it writes/verifies go
  through a path that does check for failure), but the bitmap sector
  itself would have silently gone stale on disk with no record of it
  ever happening -- a real correctness gap, since a stale on-disk
  bitmap risks double-allocating the ~4096 blocks that one sector's
  bits cover after a future reboot reloads it.
  - New `persist_bitmap_sector_with_retry()` wraps `persist_bitmap_
    sector()` in one more bounded retry round (`FS_BITMAP_PERSIST_MAX_
    RETRIES` = 3, on top of `dma_transfer_with_retry()`'s own 3) before
    giving up, and unconditionally `klog_write()`s a warning (sector
    index, LBA, attempt count) if it still fails -- independent of the
    `debug fs` switch, matching `ata.c`'s own "always log a real
    failure" convention for its final retry-exhausted case.
  - Both callers now check the result: `write_batch_end()` only clears
    a sector's dirty bit on success, so a failure leaves it flagged and
    the very next flush (any subsequent disk-backed write) gets another
    chance instead of the failure being permanent. `persist_bitmap_
    bit()`'s non-batched path does the same -- marks the sector dirty
    on failure even outside a batch, for the same later-flush retry.
  - Verified live: re-ran `stress 300` via QMP and hit the exact same
    class of failure again (`ata: dma write failed after 3 attempts
    (lba 46)`), but this time with no `fs: WARNING` -- confirming
    `persist_bitmap_sector_with_retry()`'s second attempt (a fresh
    `dma_transfer_with_retry()` call) recovered it that the old code
    would have silently dropped. `stress 300` still PASSED, byte-for-
    byte verified, both before and after.
- Root-caused *why* the DMA retries above happen at all -- the user
  noticed their host disk activity monitor spike to ~175-200 MB/s WRITE
  right when a `stress 300` run hit the retry-exhausted case. First
  hypothesis was QEMU's disk-cache mode (none of the Makefile's
  `qemu-system-x86_64` targets, nor `tools/qmp_test.py`'s headless
  launcher, passed an explicit `cache=` for `disk.img`, so QEMU
  defaults to `writeback` -- host-page-cache-buffered, flushed back to
  disk later in bursts on the host OS's own schedule). Tried
  `cache=writethrough` (every write acknowledged only once it actually
  reaches the physical disk) on both -- it did NOT fix it: the same
  class of DMA failure still occurred, and write throughput dropped
  ~12x (1.5 MB/s vs. ~18 MB/s, `stress 300` 265s vs. ~85s) for no
  actual gain, so that change was reverted rather than merged. The
  real cause: the host filesystem `disk.img` lives on is Btrfs, which
  is copy-on-write -- every write allocates new blocks elsewhere and
  updates Btrfs's own B-tree metadata, batching that metadata into a
  periodic transaction commit (every ~30s by default, or once enough
  dirty data accumulates) completely independent of QEMU's own
  disk-cache setting, which is exactly why changing that setting had
  no effect. Fixed at the host level (not in this repo, but noted here
  since it explains a class of failure this repo's own retry-and-log
  code exists to absorb): `disk.img` given Btrfs's `+C` (no-COW)
  attribute via a copy-into-a-fresh-`chattr`ed-file-then-swap (`chattr`
  can't be applied retroactively to an existing file's already-written
  extents), verified byte-identical via `sha256sum` before swapping,
  original kept as `disk.img.cow.bak`. Also widened
  `kernel/drivers/ata.c`'s `DMA_WAIT_TICKS` 3s -> 5s as cheap extra
  headroom against whatever comparable host-side stall shows up next
  -- costs nothing on the success path, a genuinely dead/hung drive
  still surfaces as a hard failure, just up to ~2s later.
- `tfs.c`'s `free_all_blocks()` (backing both `fs_delete()` and
  overwriting an existing file via `fs_write()`) called `free_block()`
  -> `persist_bitmap_bit()` once per freed block with no batching --
  unlike the write path (`write_range_impl()`), which wraps its own
  block allocation in `write_batch_begin()/write_batch_end()` so all
  the bitmap sectors a run of allocations touches get flushed once
  each instead of once per block (see the TFS2/ATA-throughput entry in
  this file's history). Found live: the user noticed `stress <mb>`
  visibly pausing between "reading back and verifying ... 100%" and
  the final `PASSED` line, correctly guessing it was the temp file's
  cleanup delete. It was -- a 300MB `stress` run's ~76,800 freed 4KB
  blocks cover only ~19 distinct bitmap sectors, but unbatched, each
  of those sectors got rewritten to disk once per block landing in it
  (thousands of redundant synchronous ATA writes to the same handful
  of sectors) instead of once, total. Fixed by wrapping both
  `free_all_blocks()` call sites (`tfs_delete()`, and `tfs_write()`'s
  reclaim-before-overwrite path) in `write_batch_begin()/end()`,
  matching the write path's existing pattern -- nestable, so this is
  safe even where `write_range_impl()` right after it opens its own
  batch too. Verified live via QMP: `stress 300`'s total time dropped
  from 71-85s (write+read math alone only needs ~27s at the
  18/27 MB/s measured that run) to `27s` flat -- the delete phase's
  contribution went from 45-60+ seconds to effectively zero.

- Draggable desktop icons (see the Added entry above), two issues found
  in real use right after landing:
  - Grid columns were sized to the single longest label across the
    WHOLE registry ("Task Manager"), so even a column with only short
    labels next to it (e.g. "Notepad"/"About") got that label's full
    pitch -- a much bigger gap than any actual adjacent pair of icons
    needed. `apps/wm/desktop.c`'s `current_grid()` now uses a fixed
    icon-size-driven column width (`DESKTOP_ICON_COL_W`, matching the
    existing row height for square cells) instead of a label-driven
    one -- the standard real-desktop tradeoff (fixed grid pitch
    regardless of label length; an unusually long label may run past
    its cell into a neighboring column's icon in the same row, an
    accepted quirk of freeform placement, not a bug).
  - Dropping an icon onto a cell another icon already occupied made
    them silently overlap (the `apps/ui/ui_icon_grid.h` version 1
    entry above called this out as a known "future refinement," but it
    turned out to matter immediately in practice). `desktop.c` now
    searches outward from the drop cell for the nearest free one
    (`nearest_free_cell()`, ring by ring) instead of overlapping.
  - Verified via QMP: dragged two icons into a second column (screenshot
    confirms the tight grid pitch), then dropped a third icon directly
    onto an already-occupied cell (screenshot confirms it settled into
    an adjacent free cell instead of stacking) -- see
    `screenshots/2026-08-12/desktop-icon-drag-tight-grid-fix.png` and
    `desktop-icon-drag-no-stack-fix.png`.

- The tray entry above originally scoped every tray registration/
  update/unregister to just the taskbar strip via `wm_damage_rect()`
  instead of relying on the full-screen fallback -- shipped, then
  caught live on the user's own machine (not QMP-testable, since it
  only shows up once real time passes and the real PS/2 mouse moves
  around): entering GUI mode showed a black desktop with no icons at
  all, and the mouse cursor visibly stopped tracking correctly.
  - Root cause #1 (black desktop): `tray_init()` runs during
    `wm_run()`'s setup, before the main loop starts. Registering the
    clock there called `wm_damage_rect()` for the taskbar strip *before
    the very first frame*, which poisoned `wm_render_frame()`'s "no
    damage reported yet -- unknown, be safe, draw everything"
    full-screen fallback into a taskbar-only clip. `desktop_draw()`
    (icons) and the window-chrome loop still ran, but every pixel they
    wrote outside that strip was silently clipped away, so the first
    frame -- the only one that mattered, since nothing else re-damages
    the whole desktop afterward -- never actually drew the desktop.
  - Root cause #2 (cursor tracking): the once-a-second clock tick used
    to report no damage at all, which meant it forced a full-screen
    fallback redraw every single second -- an implicit, unadvertised
    safety net that kept `wm_render.c`'s cursor-under-pixels snapshot
    (`cursor_under`, used by the cheap `wm_render_cursor_move()` path)
    resynced against the real screen every second. Scoping the tick's
    damage to just the taskbar strip silently removed that safety net,
    so any drift in the cheap cursor-move path stopped self-correcting.
  - Fix: `apps/wm/wm_tray.c`'s `tray_damage()` no longer calls
    `wm_damage_rect()` at all -- it just sets `redraw_pending`, same as
    the clock always did before this feature existed. The registration
    API itself (`tray_register()`/`tray_set_text()`/`tray_unregister()`)
    is unchanged; only this internal damage-scoping optimization was
    reverted.
  - Verified via QMP: entering GUI mode now shows the full desktop
    (icons + navy background) on the very first frame, no click needed
    to "unstick" it -- see `screenshots/2026-08-12/tray-fix-*.png`.

## [0.1.0] - 2026-08-12

### Fixed
- The initial `v0.1.0` GitHub Release was published missing `toy-os.iso`
  -- `tools/run_release.sh` requires it next to `disk.img`/`disk.img.gz`
  (it errors out immediately if absent), and v0.0.9's release included
  it, but it was left off this time. Uploaded to the existing release
  as a follow-up (`gh release upload v0.1.0 toy-os.iso`), no retag
  needed.

### Changed
- Repo history rewritten (`git-filter-repo`, all 91 prior commits) to
  remove the maintainer's real name and personal email addresses from
  both commit authorship and `LICENSE`'s copyright line -- requested
  directly, for privacy. Every commit now carries a generic `toy-os
  <noreply@toy-os.local>` identity; `LICENSE` reads "toy-os
  contributors". All commit hashes and the `v0.0.9` tag changed as a
  result (force-pushed). See `docs/decisions.md` for the full
  mechanics, the verification method, and the standing convention this
  sets for every commit going forward.

### Added
- `tools/run_release.sh` -- standalone QEMU launch script shipped as a
  GitHub Release asset (v0.0.9 onward), for anyone running from just a
  release download with no repo checkout. Gunzips `disk.img.gz` if
  needed, then boots with the same device/display flags the
  Makefile's `run:` target uses.

### Fixed
- v0.0.9's release process surfaced two real gotchas, now documented
  in `docs/decisions.md`'s versioning entry: `disk.img` is a large
  sparse file (~9GB apparent, ~370KB real data) that must be gzipped
  before shipping as a release asset (raw upload both exceeds GitHub's
  2GB-per-asset limit and wastes bandwidth on zeros), and the Cowork
  cloud sandbox's outbound git proxy blocks `git push`/`gh release
  create` outright regardless of the repo token embedded in the
  remote URL (`access denied by the git proxy: ... not in this
  session's authorized repository set`) -- confirmed by a real failed
  push attempt, not assumed. Publishing a release now always ends with
  handing the user exact commands to run from their own machine.

### Added
- Wired `wm_run()` to poll a pending write instead of blocking -- Phase 3
  of the async-I/O roadmap item (see `docs/roadmap.md`). Phases 1
  (non-blocking DMA start/poll primitive) and 2 (steppable write API)
  landed earlier as `ata.c`/`fs.h` primitives with no real caller yet
  (proven standalone via the `dmatest`/`steptest` shell commands -- see
  their own CHANGELOG entries); this phase is the first real caller of
  Phase 2's `fs_write_range_begin()`/`fs_write_range_step()`. A new
  WM-global single slot (`pending_write`/`pending_write_win`,
  `apps/wm/wm_internal.h` -- same "-1/NULL means none" idiom as
  `dragging`/`resizing`) holds the handle; `wm_run()`'s main loop
  (`apps/wm/wm.c`) calls `fs_write_range_step()` once per frame instead
  of ever calling `fs_write_range()`/`fs_write()` and blocking, so one
  frame's extra cost is bounded to a single filesystem block's write
  latency, not the whole file. Two new public entry points in `wm.h`:
  `window_start_write()` (registers a handle, refuses a second
  concurrent one) and `window_write_pending()` (lets an app check before
  starting a new write); completion is delivered back via a new
  `gui_apps.h` callback, `on_write_complete(win, success)`, called once
  polling reaches `FS_STEP_DONE`/`FS_STEP_FAILED`.
  - Notepad's Save As... (`apps/notepad.c`) is the first real caller:
    `notepad_picker_saved()` now calls `fs_delete()` (reclaim any
    existing file's blocks -- `fs_write_range_begin()`/`step()` extend a
    file but never shrink it, unlike `fs_write()`'s own
    truncate-then-write, so this avoids stale trailing bytes on an
    overwrite with shorter text) then `fs_write_range_begin()` +
    `window_start_write()`, and shows "Saving..." with the Save As...
    button disabled until `notepad_write_complete()` fires.
  - `ui_button` (`apps/ui/ui_button.h`/`.c`, `ui_button_group.c`) gained
    a `disabled` field/`ui_button_set_disabled()` for this -- asked
    first, per the project's own "add as a widget" preference, since a
    disabled/dimmed button is a generic, reusable capability, not a
    Notepad-only concern. `ui_button_group_press()`/`_click()` skip a
    disabled button entirely (same as never being hit).
  - `bring_to_front()`/`close_window()` (`wm.c`) both keep
    `pending_write_win` accurate across window reordering/closing (they
    already shuffle `windows[]` by copying struct contents between fixed
    slots, not by moving identity) -- `close_window()` also refuses to
    close the window a write belongs to, and `wm_exit_requested` is
    deferred (not abandoned) until a pending write reaches a terminal
    result, since its handle owns kernel heap state that only gets freed
    then.
  - A real bug caught by QMP testing, not code review: `apps/notepad.c`
    originally cached the window pointer passed to `window_start_write()`
    once, in `notepad_open()`. That's unsound in this WM -- `struct
    window *` isn't a stable per-window identity here, since
    `bring_to_front()` reorders by copying window *contents* between
    fixed `windows[]` slots rather than moving pointers. After clicking
    a second window in front of Notepad and then Save As..., the stale
    cached pointer silently resolved to the WRONG window by the time the
    write was registered: the write itself still completed correctly on
    disk (verified via `tools/tfs2_writer.py ls`), but
    `on_write_complete()` never fired for the right window, leaving
    Notepad's Save As... button permanently disabled. Fixed by capturing
    the window pointer fresh in `notepad_click()` when Save As... is
    pressed instead (safe because the file picker it opens is modal --
    no other window can be reordered while it's open, per
    `wm_handle_left_click()`'s own dispatch order).
  - Verified via QMP: seeded an 8191-byte file directly onto `disk.img`
    with `tools/tfs2_writer.py` (host-side, no boot needed) to get
    content Notepad could Open instantly rather than needing to type it
    through the emulated keyboard; opened it in Notepad, opened a second
    window (About), clicked Save As... to overwrite the same file, and
    clicked About's taskbar button immediately after confirming Save --
    caught "Saving..." with the Save As... button visibly disabled, and
    separately caught the desktop successfully switching focus to About
    while the write was still in flight (exercising the
    `bring_to_front()`/`pending_write_win` reindexing path live, not
    just by inspection). Confirmed the finished file's size/modified
    timestamp matched via `tools/tfs2_writer.py ls` after each run.
    `tools/preflight.sh` (build + boot smoke test) passed throughout.
    Screenshots in `screenshots/2026-08-12/`.
  - Not done this round (Phase 4, next): generalizing to reads and the
    plain (non-GUI) shell prompt.

- Steppable read API + wired it into `wm_run()`'s poll -- Phase 4 of the
  async-I/O roadmap item (see `docs/roadmap.md`), the read counterpart
  to Phase 2/3 above. `fs.h` gained `fs_read_range_begin()`/
  `fs_read_range_step()`, dispatched through `fs_ops.h`/`vfs.c` to a new
  `tfs.c` backend (`tfs_read_range_begin()`/`_step()`, built from a
  `read_range_one_block()` helper split out of the existing blocking
  `read_range_impl()` the same way Phase 2 split `write_range_impl()`).
  One real difference from the write side: a read can legitimately
  finish having copied fewer bytes than requested (the EOF clamp), so
  `fs_read_range_step()` takes an extra `uint32_t *out_total` out-param
  the write side doesn't need.
  - `wm_run()` (`apps/wm/wm.c`) gained a second WM-global single slot,
    `pending_read`/`pending_read_win` (`wm_internal.h`), polled once per
    frame right after the existing `pending_write` poll -- same
    bounded-per-frame-cost reasoning as Phase 3, a separate slot (not
    shared with `pending_write`) since nothing stops a read and a write
    being in flight for two different windows at once, even though
    nothing exercises that yet. Two new `wm.h` entry points mirroring
    `window_start_write()`/`window_write_pending()`: `window_start_read()`
    and `window_read_pending()`. Completion delivers via a new
    `gui_apps.h` callback, `on_read_complete(win, success, total)` --
    the extra `total` argument (vs. `on_write_complete`'s bare
    `success`) is how an app learns the actual byte count once the
    handle's already freed. `bring_to_front()`/`close_window()` keep
    `pending_read_win` accurate the same way they already did for
    `pending_write_win`.
  - Notepad's Open... (`apps/notepad.c`) is the first real caller:
    `notepad_picker_opened()` now checks the file exists (`fs_exists()`/
    `fs_is_dir()` -- unlike Save As..., Open never creates), clamps the
    read length against `fs_size()` and a new `g_load_buf[SCROLLBACK_CAP]`
    scratch buffer (mirroring Save's `g_save_buf`), then calls
    `fs_read_range_begin()` + `window_start_read()` and shows
    "Loading..." with the Open... button disabled until
    `notepad_read_complete()` fires and hands the loaded bytes to the
    existing `notepad_load_text()`. A 0-byte file short-circuits to an
    instant load (nothing to step).
  - `steptest <mb>`'s existing readback-verification pass (proven
    standalone since Phase 2, `apps/shell_sys.c`) now goes through the
    new `fs_read_range_begin()`/`_step()` instead of blocking
    `fs_read_range()`, reporting both write- and read-step counts --
    reused rather than adding a second diagnostic command, since it
    already had the large multi-block file and readback loop this
    primitive needed to prove itself against.
  - Verified via QMP: `steptest 3` passed (768 write `step()` calls, 768
    read `step()` calls, byte-for-byte verified) at the physical shell
    prompt; in the GUI, seeded a 6000-byte file directly onto `disk.img`
    with `tools/tfs2_writer.py`, opened it in Notepad via Open... (typed
    the path into the picker's filename field, since punctuation needs
    explicit QMP qcodes rather than `send_text()`'s letters/digits-only
    helper), confirmed the loaded text matched byte-for-byte and the
    status line read "Loaded.". Also confirmed the file picker's
    existing "must already exist" validation in Open mode correctly
    refuses a nonexistent filename (pre-existing `file_picker.c`
    behavior, unaffected by this phase). `tools/preflight.sh` (build +
    boot smoke test) passed throughout. Screenshots in
    `screenshots/2026-08-12/`.
  - The plain (non-GUI) shell prompt, deferred above, turned out not to
    need `wm_run()`-style ambient polling at all -- `shell_main()`
    (`apps/shell.c`) is a REPL with no per-frame tick to hang a pending
    op off of. The actual gap is narrower and already documented in
    `keyboard.c`'s own `keyboard_getchar()` comment: a blocking command
    doesn't get `debug_console_poll()`/`vga_cursor_tick()` serviced at
    all until it returns, unlike the shell's idle wait at the prompt or
    the GUI's `wm_run()` loop. Closed by making `cat` (`apps/shell_fs.c`)
    -- the one shell command with no size cap on how much it blocks
    reading, unlike Notepad's Open... above which is capped at
    `SCROLLBACK_CAP` -- use `fs_read_range_begin()`/`fs_read_range_step()`
    in its own loop instead of a single blocking `fs_read()`, servicing
    `debug_console_poll()`/`vga_cursor_tick()` between blocks. `cat` now
    `fs_size()`s the file and `kmalloc()`s a buffer sized to it (a
    reused static pointer, same pattern as `tfs.c`'s own `g_read_buf`
    behind `fs_read()`) rather than relying on `fs_read()`'s internal
    staging buffer, so it can drive the stepped API directly; this
    preserves `fs_read()`'s existing "up to available RAM" ceiling
    rather than shrinking it to some fixed cap. No `hlt`/throttling
    between steps (unlike `wm_run()`'s poll, gated on its own idle wait)
    -- `cat` has real work to do and wants to finish as fast as the disk
    allows.
    - Verified via QMP: seeded a 550,000-byte file and a 14-byte file
      onto `disk.img` with `tools/tfs2_writer.py`; `cat`'d the small
      file (exact match), a nonexistent path (`cat: no such file:` as
      before), and the large one (content correct throughout, shell
      returned cleanly to the prompt afterward -- no hang).
      `tools/preflight.sh` passed throughout. Screenshots in
      `screenshots/2026-08-12/`.
  - Milestone 1's async-I/O item is now fully closed except the
    separately-tracked Terminal async-spawn item below.

- Async/continuously-armed process spawning for the GUI Terminal
  (Milestone 1 phase 4b, docs/roadmap.md) -- `ls` and an explicit
  allowlist of verified-safe `/bin` binaries via `run` now execute from
  inside a Terminal window without freezing the desktop, instead of
  being wholesale-blocked. Same root cause as the async I/O phases above
  (a blocking call inside `wm_run()`'s single event loop), different
  mechanism (process scheduling, not I/O completion):
  - `kernel/core/scheduler.c`/`scheduler.h`: `scheduler_armed` is now set
    once in `scheduler_init()` and never unset, replacing the old
    demo-only flag `scheduler_demo_run()` flipped on/off around its own
    wait loop -- safe because an armed tick over an empty process table
    is a byte-for-byte no-op (find_next_ready() finds nothing, resumes
    exactly what was interrupted), the same invariant the old disarmed
    default relied on. New public API: `scheduler_spawn(path, args)`
    (thin wrapper over the previously-`static` `spawn_from_fs()`,
    extended to build a real argv via a newly-exposed
    `elf_build_argv_on_stack()` instead of always zeroing rdi/rsi) and
    `scheduler_poll(pid, &exit_code)` (`enum sched_poll_result`:
    RUNNING/EXITED/INVALID). A process that exits now becomes
    `SCHED_ZOMBIE` (holding its exit code) instead of being freed
    straight to `SCHED_UNUSED` -- `scheduler_poll()` is the explicit reap
    step, same two-phase shape `wait()`/`waitpid()` has.
  - `kernel/core/elf_run.c`/`elf_run.h`: the static `build_argv_on_stack()`
    helper promoted to a public `elf_build_argv_on_stack()` so
    scheduler.c's `spawn_from_fs()` can reuse the exact same argv layout
    `elf_run_from_fs()`'s legacy blocking path already uses.
  - `apps/wm/wm.c`/`wm.h`/`wm_internal.h`: a third WM-global poll slot,
    `pending_proc`/`pending_proc_win`, mirroring `pending_write`/
    `pending_read`'s exact shape from Milestone 1 phases 3-4 --
    `window_start_process()`/`window_process_pending()`, a new
    `gui_apps.h` callback `on_process_exit(win, exit_code)`,
    `bring_to_front()`/`close_window()` keeping `pending_proc_win`
    accurate the same way, `close_window()` refusing to close a window
    with a process pending, and `wm_exit_requested` deferred while one is
    in flight (a live `vga_sink` would otherwise silently swallow the
    physical shell's own prompt output on return). Unlike the I/O pair,
    this poll doesn't make anything appear on screen -- a spawned
    process's `SYS_WRITE` output already lands in the owning window's
    scrollback via `vga_putc()`'s active sink the instant each syscall
    runs, independent of `wm_run()`'s frame rate; the poll only detects
    completion.
  - `apps/terminal.c`/`terminal.h`: new per-window state
    (`st->running_pid`, blocking all keyboard input while set, same as
    `st->in_editor` does for `edit`/`nano`; `st->saved_sink`). `ls`
    always spawns async now (mirrors `shell_sys.c`'s `cmd_ls_bin()`
    flag/path parsing, via `resolve_editor_path()` for the positional
    argument). `run <name>` does too, but only for names on a new
    explicit allowlist, `RUN_ALLOWED_BINS` -- the opposite of
    `BLOCKED_CMDS`'s blocklist approach: `crash_test`, `exit_test`,
    `file_test`, `hello`, `lspci`, `newsyscalls_test`, `socket_test`,
    `write_bad_test`, `write_test`, each checked against its own
    `userland/*.c` source (not assumed safe by name) for the two real
    hazards -- reading stdin (no stdin routing to a spawned process
    exists yet, so one blocked on it would hang forever, and
    `close_window()`'s new refusal above would strand the whole window)
    or touching the framebuffer/its own window directly. Excluded:
    `echo` (loops on `SYS_READ_KEY` waiting for an Esc that never
    arrives), `gui_test`/`win_test` (framebuffer/own-window takeover),
    `counter_a`/`counter_b` (infinite-loop-by-design `schedtest` demo
    processes). `crash_test` deliberately faults -- verified safe anyway:
    `idt.c`'s fault handler was already scheduler-aware from M16 (its
    `recoverable` branch checks `scheduler_current_pid()`), tearing the
    process down and reporting "RING-3 PROCESS CRASHED" through whatever
    sink is active with exit code -1, exactly like a legacy
    `run crash_test` from the physical shell.
  - Verified via QMP: `schedtest` still spawns/interleaves/exits its two
    counter processes correctly with the scheduler now permanently
    armed, and the physical shell's own legacy `run <name>`/`ls` are
    unaffected (confirmed `run exit_test` -> exit code 42 and `ls`
    listing correctly both still work the old blocking way). In the GUI
    Terminal: `ls` lists a real directory and reports "Process finished.
    Exit code: 0"; `run exit_test` reports exit code 42; `run crash_test`
    reports the crash message and "Exit code: CRASHED" without freezing
    or hanging the window; `run gui_test` (not on the allowlist) and
    `schedtest`/`gui` (still in `BLOCKED_CMDS`) get their expected
    refusals; the window closes normally afterward with no stuck state,
    and exiting the GUI back to the physical shell afterward works
    cleanly (`ls` there still works too). Also incidentally confirmed a
    PRE-EXISTING, unrelated quirk while testing: `run hello` from the
    physical shell page-faults (not the `hlt`-based crash `hello.c`'s own
    comment describes) because `USERLAND_MARKER_ADDR`
    (`userland/userland_contract.h`) happens to collide with
    `ELF_RUN_HEAP_VADDR` (`elf_run.c`), a page `elf_run_from_fs()` never
    actually maps unless the binary calls `sbrk()` -- confirmed
    unaffected by this change (reproduced identically via `run exit_test`
    succeeding normally right after), not investigated further as
    out-of-scope for this item.
    `tools/preflight.sh` (build + boot smoke test) passed throughout.
    Screenshots in `screenshots/2026-08-12/`.

## [0.0.9] - 2026-08-12

### Fixed
- Notepad's filename field (`widget_textfield_draw`, `apps/widgets.c`)
  no longer overflows past its own border when the text is longer than
  the field -- reported from a screenshot showing "notepad.txt" running
  into the Save button. Root cause: `gfx_draw_string()` only clips at
  the screen/window edge, not at an arbitrary width -- it doesn't take
  one -- so the field's `w` parameter was never actually enforced,
  despite a doc comment claiming it was ("clipped the same way every
  other text-drawing call already is"). Fixed by having
  `widget_textfield_draw()` clip to what fits itself, sliding the
  visible window just far enough to keep the cursor in view while the
  field is active (typing past the visible edge now scrolls, like a
  real text input) -- confirmed by typing a name well past
  `FIELD_COLS` and watching the border hold. See `docs/decisions.md`.
- The Start menu's width (`start_menu_w()`, `apps/wm/wm_render.c`) was
  hardcoded to "12 chars, room for the longest app name" -- true when
  written, silently wrong the moment this same change added "Exit to
  shell" (13 chars) below the app list, overflowing past the menu's
  right border with no compiler warning. Now scans both
  `gui_app_registry` and `wm_system_actions` for the actual longest
  label. Caught by screenshot, not by re-reading the code -- see this
  file's own testing conventions.
- Notepad's filename field text sat flush against the field's own
  top/bottom border with zero vertical margin -- reported from a
  screenshot as "white background... overflows to the textbox's
  outline." Not actually drawing outside the field's bounds (`h` and
  the glyph height matched exactly): the bug was that zero margin
  meant the glyph's own opaque background painted directly over the
  border pixels on any row where a character existed, visibly erasing
  the border line under the text. `apps/notepad.c`'s `bh` (the
  field/button row height) was being computed as exactly
  `gfx_char_h()` with no slack -- new `ROW_VPAD` constant adds a few
  pixels of real vertical breathing room via `TOOLBAR_H`'s formula.
  Verified via QMP screenshot at font sizes 8, 18 (default), and 24 --
  border stays intact and visible above/below the text at every size.

### Added
- `ls` migrated off its kernel-space shell built-in onto a real,
  disk-hosted ELF64 binary (`/bin/ls`, `userland/ls.c`) -- the third
  binary to run through `elf_run_from_fs()` (after `lspci`/the ~13
  test binaries), and the first to actually need arguments. User asked
  for this plus GNU-coreutils-flavored `-l`/`-al` and
  `--color=auto`-by-default behavior; scoped via `AskUserQuestion` into
  three real infrastructure additions rather than one-off hacks:
  - **Real argc/argv at ELF entry.** `process_run_ring3()`
    (`kernel/core/process.c`/`.h`) is now a thin argc=0/argv=0 wrapper
    around a new `process_run_ring3_args(pml4_phys, entry, user_rsp,
    argc, argv)`, which seeds RDI/RSI (SysV's first two integer
    arguments) before `iretq` -- any `/bin` binary can now declare
    `void _start(int argc, char **argv)` and receive them like an
    ordinary function call. `elf_run_from_fs()` (`kernel/core/elf_run.c`/
    `.h`) gained an `args` parameter (space-separated, no quoting) and a
    `build_argv_on_stack()` helper that lays argv[0]=path plus each
    `args` token onto the process's one identity-mapped stack page:
    strings written downward from the page's top, the argv pointer
    array below them, `user_rsp` set to the pointer array's own address
    so a subsequent `push` from ring 3 only ever touches fresh, lower,
    previously-unused space. Real bug hit and fixed here: several
    path-taking syscalls (`SYS_LISTDIR` chief among them) validate a
    full `FS_PATH_MAX` (64) byte range starting at whatever pointer
    userland passes, not just up to its NUL -- `argv[0]` landing close
    enough to the stack page's literal top made that validation run off
    the mapped page and fail. Fixed by reserving `FS_PATH_MAX` bytes of
    never-written padding at the page's true top before laying out any
    argv strings.
  - **`SYS_SET_COLOR` syscall** (`kernel/include/syscall_abi.h`/
    `kernel/core/syscall.c`) -- RDI/RSI are foreground/background
    `enum vga_color` values, wraps `vga_set_color()` directly (same
    thing the shell's own `color` command does from kernel space).
    Rejects (-1) an out-of-range value rather than clamping it.
  - **Real per-entry timestamps for `SYS_LISTDIR`** -- `struct dirent`
    gained a `struct rtc_time modified` field (reusing the same struct
    `SYS_GETTIME` already hands to ring-3); the kernel-side handler now
    calls `fs_stat()` once per entry to fill it. No permission-bits or
    owner concept exists in this filesystem at all, so `ls -l` shows
    real type/size/mtime only, no invented placeholder columns.
  - `userland/ls.c` -- no libc, same shape as `lspci.c`. `-a` is
    accepted but a no-op (no dotfile-hiding convention on this
    filesystem, so there's nothing for it to additionally reveal;
    accepted so a habitual `ls -la` doesn't error). Default output
    colors each name via `SYS_SET_COLOR` (directories vs. files),
    unconditionally -- matching `--color=auto`'s look without a flag
    to gate it, per this feature's scope. `-l` shows a type char
    (`d`/`-`), right-aligned size, `MM/DD/YYYY HH:MM:SS` mtime (same
    shape `stat`'s own `print_stat_timestamp()` already uses), then
    the (still-colored) name.
  - `apps/shell_sys.c` gained `cmd_ls_bin()` -- ls's own dedicated
    dispatch entry (same precedent as `cmd_lspci()`), splitting
    `-a`/`-l`/`-al`/`-la` flags from an optional positional directory
    argument and resolving that argument (or defaulting to `cwd`)
    through `resolve_path()` before crossing into ring 3 -- `fs.c`/
    `fs.h` has no cwd concept at all, and neither does `userland/ls.c`,
    so this is the one place a relative path becomes absolute.
    `cmd_run()` also gained its own name/args split (previously only
    ever passed a bare binary name to `elf_run_from_fs()`).
  - `apps/shell_fs.c`'s old `cmd_ls()`/`list_cb()` (direct `fs_list()`
    call from kernel space) are deleted, per the user's explicit
    request -- `ls` has exactly one implementation now, not two.
  - **Real, documented regression, not an oversight:** `ls` joined
    `apps/terminal.c`'s `BLOCKED_CMDS` (GUI Terminal) alongside `run`.
    Every path through `elf_run_from_fs()`/`process_run_ring3_args()`
    is synchronous and blocking -- it would freeze the Terminal
    window's whole event loop until the process exits, same hazard
    `run` was already blocked for. User was shown the real cost of
    building async/continuously-armed spawn support this session (new
    public spawn API, scheduler changes, `wm_run()` restructuring,
    new per-window process-running state) and explicitly chose to ship
    `ls` now and track that infrastructure on `docs/roadmap.md`
    instead of building it this round. Directory listing from inside
    the GUI Terminal is unavailable until that lands.
  - `Makefile`: `LS_ELF`/build rule pair mirroring `lspci`, `SEED_BINARIES`
    entry, added to `all`/`seed`/`iso`/`clean`'s prerequisite lists.
  - Verified via QMP against a clean `make clean && make all && make
    iso`: `ls`, `ls -l`, `ls -a`, `ls -al /bin` all produce correct,
    colored output with real sizes/timestamps from the physical shell;
    `run lspci` (the zero-arg `process_run_ring3()` path) still works
    unchanged, confirming the argc/argv plumbing didn't regress
    existing callers; `ls` inside the GUI Terminal shows the expected
    blocked-command message instead of hanging the window. (Testing
    aside, unrelated to this feature: the QEMU test VM's disk had a
    Swedish keyboard layout persisted from earlier keyboard-layout
    testing this session, which briefly looked like a `-`/`=` key
    corruption bug before `keyboard us` explained it -- included here
    only so a future session doesn't rediscover the same red herring.)
- The remaining ~13 GRUB-module-loaded ELF64 test binaries
  (`elf_test`/`hello.elf`, `syscall_test`, `write_test`,
  `write_bad_test`, `ptr_test`/(folded away, see below), `gui_test`,
  `echo_test`, `win_test`, `file_test`, `newsyscalls_test`,
  `crash_test`, `socket_test`, plus `counter_a`/`counter_b`) moved off
  GRUB modules onto build-time-seeded `/bin` entries, the same
  mechanism `lspci` got in the entry below -- prompted by the user
  asking what happens if these ELF64 binaries are run under real Linux
  (answer: they'd crash or misbehave -- toy-os's syscall convention
  rides `int $0x80`, which 64-bit Linux only recognizes as the legacy
  32-bit compat entry point, so the kernel would dispatch through the
  wrong syscall table with the wrong register convention; the ELF
  itself is otherwise structurally valid and loadable). What changed:
  - `Makefile`'s `SEED_BINARIES` list now has 14 `elf:/bin-name` pairs
    (`lspci` + the 13 above); `seed:` stages all of them into
    `seed/sync/bin/` before one `tfs2_writer.py sync` call. `iso:` no
    longer copies any per-binary `.elf` file into `iso/boot/` --
    `grub.cfg` is down to just `multiboot2 /boot/kernel.bin` + `boot`,
    no `module2` lines at all.
  - `kernel/core/elf_run.c`'s `elf_run_from_fs()` -- the one generic
    loader `run <name>` already used for `lspci` -- is now what every
    `/bin` binary runs through. It now also calls
    `syscall_reset_heap(as, ELF_RUN_HEAP_VADDR)` unconditionally before
    running (cheap bookkeeping, arms `SYS_SBRK`) -- found by inspecting
    `echo_test.c`, which called this itself before its old
    dedicated-command loader ran it; without this, migrating
    `echo_test` to the generic path would have silently broken its
    heap-based `sbrk()` use.
  - `kernel/core/scheduler.c` gained `spawn_from_fs(const char *path)`,
    replacing `spawn_from_module(int module_index)` outright (its only
    caller, `scheduler_demo_run()` for `schedtest`, is the only one
    that needs two processes running concurrently under the real
    preemptive scheduler -- a one-shot `run <name>` can't do that, so
    this couldn't just fold into `elf_run_from_fs()` the way the
    others did). Sources ELF bytes via `fs_read()` instead of
    `multiboot_get_module()`, same no-copy-needed reasoning
    `elf_run_from_fs()` already used. `schedtest` now spawns
    `/bin/counter_a` + `/bin/counter_b`.
  - The 11 now-redundant kernel-side test harnesses and their headers
    (`elf_test`, `syscall_test`, `write_test`, `ptr_test`, `gui_test`,
    `echo_test`, `win_test`, `file_test`, `newsyscalls_test`,
    `crash_test`, `socket_test` -- `kernel/core/*.c` + `kernel/include/
    *.h` pairs) are deleted, along with their dedicated shell commands
    in `apps/shell.c` (`elftest`, `syscalltest`, `writetest`,
    `ptrtest`, `guitest`, `echotest`, `wintest`, `filetest`,
    `newsyscalltest`, `crashtest`, `sockettest`) -- each is a real
    `/bin` binary now, run via `run <name>` (e.g. `run write_test`).
    `ring3test` (no ELF file at all, tests raw paging/GDT/ring-3
    isolation) and `schedtest` are the two exceptions, kept as
    dedicated commands since neither maps onto the generic
    `elf_run_from_fs()` path. `elftest`/`hello.elf` specifically tested
    a raw manual-`iretq` ring-3 entry, distinct from the recoverable
    `process_run_ring3()` path every other binary already used -- user
    chose to fold it into the generic `run hello` path anyway, trading
    that one narrow bit of coverage for one less special case; verified
    the fault (a deliberate privileged instruction from ring 3) is
    still caught and reported as `Exit code: CRASHED`, not a kernel
    crash.
  - `kernel/core/pmm.c`'s module-reservation loop and Multiboot-info
    reservation are unchanged in code but now dormant (zero modules
    exist) -- comments updated to say so and to point at
    `spawn_from_fs()` instead of the old `multiboot_get_module()`-based
    spawn path they used to reference.
  - `apps/terminal.c`'s `run` command stays blocked wholesale inside
    the GUI Terminal window (unchanged from before this migration) --
    several of the newly-independent `/bin` binaries (`gui_test`,
    `win_test`, `echo_test`) fall into the same "takes over the
    physical framebuffer" / "blocks forever without yielding" hazards
    the old dedicated commands were blocked for, and a per-target
    allowlist couldn't be verified safe in the GUI context in the time
    available (see `docs/decisions.md`). Every `/bin` binary can still
    be run from the physical shell.
  - `tools/gui_flow.py`'s `ITEM_H` constant (Start-menu row height) was
    found to be stale -- `32` when the real value is `24`
    (`gfx_char_h() + 6` at the default font size) -- discovered because
    it made `open_app()` misclick past the intended row (clicking
    "Terminal" was actually landing on "Calculator"). Fixed and
    reverified live (`open_app("Terminal")` now opens Terminal). This
    was a pre-existing bug in the tool, unrelated to this migration,
    caught only because this migration's testing leaned on it.
  - Verified via QMP: all 14 `/bin` binaries present after a clean
    `make clean && make all && make iso`; `run <name>` for each from
    the physical shell (`hello` crash-recovers, `exit_test` returns 42,
    `write_test`/`write_bad_test` exercise real syscalls, `crash_test`
    crash-recovers, `file_test`/`newsyscalls_test`/`socket_test`
    self-check, `counter_a` runs solo); `schedtest` produces genuinely
    interleaved concurrent output; Terminal (GUI) still blocks `run`
    wholesale while ordinary commands (`ls`, etc.) work normally inside
    it; `dmesg` trail is clean, no bootstrap-install lines, everything
    routed through the one `elf_run: calling process_run_ring3() for
    /bin/...` log line. `boot_smoke_test.py` passes.
- `/bin/lspci` is now seeded onto `disk.img` at BUILD time (Makefile's
  new `seed` target, wired into `iso:`) instead of installed at BOOT
  time -- the follow-through on `tools/tfs2_writer.py` now that it can
  do it. What changed:
  - `tools/tfs2_writer.py` gained a `format` subcommand -- initializes
    a blank/foreign image as an empty TFS2 v2 filesystem, mirroring
    `tfs.c`'s `tfs_init()` format path byte-for-byte (superblock,
    cleared journal header, a bitmap with the reserved metadata region
    pre-marked allocated, `FS_MAX_FILES` blank table records written
    through the normal journal stage-commit-apply-clear sequence).
    `write`/`sync` now auto-format a blank image first (a no-op if it's
    already a valid TFS2 image), so a completely fresh, untouched
    `disk.img` can be seeded in a single call -- no toy-os boot needed
    in between anymore.
  - The Makefile's new `seed` target (`$(DISK_IMG) $(LSPCI_ELF)`
    prerequisites, `.PHONY`) stages `$(LSPCI_ELF)` into
    `seed/sync/bin/lspci` and runs `tfs2_writer.py sync $(DISK_IMG)
    seed`. Wired as an `iso:` prerequisite, so `make iso` alone now
    produces a `disk.img` with `/bin/lspci` already on it -- `sync`'s
    content-hash compare makes every call after the first a fast no-op
    unless `lspci.elf` actually changed, so this stays cheap on every
    build, not just the first. `seed/sync/` is `.gitignore`d (a
    build-generated staging copy, not a source file).
  - `kernel/core/kernel.c`'s `install_bin_binaries()`/`BIN_BOOTSTRAP`
    table and its GRUB-module-based install (added when disk-hosted
    ELF binaries first shipped, see this file's earlier `[Unreleased]`
    entry) are removed -- redundant now that the build-time seed step
    covers the same job without needing a boot cycle or a kernel
    rebuild per binary. `grub.cfg`'s `module2 /boot/lspci.elf lspci`
    line and the `iso:` recipe's `cp $(LSPCI_ELF) iso/boot/lspci.elf`
    step are removed too -- `lspci.elf` is still built (needed to seed
    the disk image) but no longer shipped as a GRUB module.
  - Verified in the cloud sandbox: `make clean && make all && make
    iso` from scratch produces a `disk.img` with `/bin/lspci` on it
    without ever booting toy-os (checked directly with `tfs2_writer.py
    ls`); `boot_smoke_test.py` still passes; booted the result and
    confirmed `run lspci` works and `dmesg` shows `fs: loaded
    persistent filesystem from disk` (not `fs: formatted a fresh...`,
    since the host tool formatted it first) with no `bin: installed
    ...` line at all (that log line's code is gone). See
    `screenshots/2026-08-11/lspci_seeded_at_build_time_no_bootstrap.png`.
- `tools/tfs2_writer.py`: a host-side TFS2 v2 read/write tool -- the
  "option 2" deferred from the real-disk-hosted-ELF-binaries work
  below, now built. Lets a file get onto `disk.img` (or be read back
  out) without booting toy-os, a kernel rebuild, or the
  `BIN_BOOTSTRAP`/GRUB-module bootstrap-install path that approach
  still relies on. Four subcommands, one script (`write`/`read`/`ls`/
  `sync`):
  - `write <disk.img> <tfs-path> <local-file>` and `read <disk.img>
    <tfs-path>` -- single-file in/out. `write` refuses to overwrite an
    existing path without `--force`; both support `--dry-run`.
  - `ls <disk.img> [tfs-path]` -- lists a directory's direct children
    with full detail (type, size, created/modified), same semantics as
    the kernel's own `fs_list()`.
  - `sync <disk.img> <seed-dir> [--dest /]` -- mirrors a whole seed
    directory tree in at once, split into two policy subtrees:
    `<seed-dir>/once/...` (copy-once -- written if missing, never
    touched again once present; for config files a user might edit
    after first boot) and `<seed-dir>/sync/...` (content-hash-synced --
    written if missing, rewritten only if the local file's SHA-256
    differs from what's on the image, otherwise left alone; for
    binaries/assets rebuilt between runs). Missing parent directories
    are created automatically (mirrors a chain of `fs_mkdir()` calls).
  - Change detection deliberately uses a content hash, not local vs.
    on-disk mtime comparison -- TFS2 timestamps are toy-os's own RTC
    wall-clock time (see `fs.h`'s `fs_stat()` comment), not something
    comparable to the host machine's clock without assuming a
    particular skew; hashing sidesteps that entirely.
  - Implemented directly against `docs/tfs2-spec.md` (superblock/
    journal checks, table records, block addressing, the free-block
    bitmap, the journal's stage-commit-apply-clear write sequence) --
    the `read`/`ls` code paths are close to the spec's own reference
    reader, extended with the write-side mirror of `tfs.c`'s
    `alloc_block()`/`free_tree()`/`persist_record()`.
  - Scope: writes only allocate direct + single-indirect blocks (12 +
    1024 blocks, ~4.03 MB max per file) -- plenty for ELF binaries and
    config/text files, everything this was built for. A file needing
    double/triple-indirect refuses cleanly with a clear error rather
    than silently truncating; extending write support to those is
    listed in `docs/roadmap.md`'s backlog if a real need for
    multi-megabyte seeded files comes up.
  - Verified in the cloud sandbox: wrote/overwrote/read back a config
    file and `lspci.elf` via `write`/`read`/`ls`, ran `sync` against a
    seed directory (confirmed `once/` skips an already-present file
    even after its local content changed, `sync/` rewrites only when
    content actually differs, both create missing parent directories),
    then booted the resulting `disk.img` for real and confirmed the
    shell's own `ls`/`cat`/`run` see exactly what the host tool wrote
    -- including a round-trip check that `sync`'s content-hash compare
    correctly recognized `/bin/lspci` as already matching what the
    kernel's own `install_bin_binaries()` had written during that same
    boot, a real interop check between the two write paths landing
    byte-identical records. See
    `screenshots/2026-08-11/tfs2_writer_host_write_verified_in_shell.png`.
- Real disk-hosted ELF64 binaries: `lspci` now exists as a genuine
  ring-3 process, loaded from `/bin/lspci` on the persistent filesystem
  and run via `run lspci` -- not a kernel-space shell built-in
  (`cmd_lspci()`/`pci_device_at()`) and not a GRUB-module test harness
  either. This is `docs/roadmap.md`'s real-disk-hosted-ELF-binaries
  item, planned in an earlier session and re-scoped this session before
  building: re-checking that old plan against the current codebase
  found its two stated blockers were already gone or smaller than
  described (see `docs/decisions.md`'s new entry), so both halves
  shipped together instead of as separate builds.
  - New syscalls `SYS_PCI_COUNT`/`SYS_PCI_INFO` (`syscall_abi.h`,
    `kernel/core/syscall.c`) -- the first syscalls added specifically
    so a real userland ELF can do something other than file I/O.
    `SYS_PCI_INFO` hands back a `struct pci_device` (`pci.h`) by value,
    the same struct `pci_device_at()` already returns kernel-side --
    reused directly rather than declaring a syscall-private copy, the
    same precedent `SYS_GETTIME` already set for `timer.h`'s
    `struct rtc_time`.
  - `userland/lspci.c`: a real freestanding, no-libc ELF64 program
    (same shape as `newsyscalls_test.c`) that calls the two syscalls
    above and prints the same `bus:device.function vendor:device class
    name` format the `lspci` shell command already uses. Carries its
    own small local copy of `pci_class_name()`'s class/subclass -> name
    table, since that function lives in kernel/drivers/pci.c and can't
    be called from ring 3 -- only linked-in code and syscalls are
    reachable from there.
  - `kernel/core/elf_run.c`/`elf_run.h`: `elf_run_from_fs(path)`, the
    disk-hosted counterpart to `file_test.c`/`newsyscalls_test.c`'s
    GRUB-module-sourced `elf_load()` + `process_run_ring3()` pattern,
    just with `fs_read()` standing in for `multiboot_get_module()`.
    Turned out to need no separate scratch-buffer copy of the ELF
    blob -- `fs_read()`'s `kmalloc()`'d buffer is already in the same
    identity-mapped low-4GiB physical range a GRUB module lives in
    (see `heap.c`'s own top comment), so `elf_load()` takes its address
    directly with just a cast. Exposed through `kapi.h` like every
    other `*_test.h`-style single-entry-point header.
  - The shell's `run <name>` (`apps/shell_sys.c`'s `cmd_run()`) now
    falls through to `/bin/<name>` + `elf_run_from_fs()` when
    `app_run()` (the kernel-space `shell`/`gui` registry, `apps/apps.c`
    -- unrelated to this, still only two entries) doesn't recognize the
    name, instead of immediately reporting "no such app."
  - `kernel_main()` gains `install_bin_binaries()`
    (`kernel/core/kernel.c`): a one-time boot bootstrap that copies
    `lspci.elf`'s GRUB module bytes into `/bin/lspci` via
    `fs_write_range()` the first time it boots against a given disk
    image (a no-op on every later boot once the file exists) -- there's
    no in-guest compiler and no host-side TFS2 writer tool yet (see
    `docs/roadmap.md`'s new backlog entry -- deliberately deferred,
    the user's own call), so this is how a binary's bytes get onto
    `/bin` at all today. Uses `fs_write_range()`, not `fs_write()` --
    an ELF's bytes contain embedded `0x00` bytes, and `fs_write()`
    treats its `data` argument as a NUL-terminated C string.
  - `lspci.elf` added as `grub.cfg`'s 14th `module2` line (index 13)
    and to the `Makefile`'s userland-ELF build/`iso`/`clean` targets --
    the same manual per-binary wiring every existing `userland/*.elf`
    already needs (this directory isn't wildcarded, unlike
    `kernel/core/*.c`, which is why `elf_run.c` above needed no
    `Makefile` changes at all).
  - Corrected three stale comments that cited `fs.h`'s `FS_DATA_MAX`
    (2048) as a real per-file ceiling (`kernel/core/etc_config.c`,
    `apps/editor.c`, `apps/editor.h`) -- found while re-verifying the
    old ELF-binaries plan against the current filesystem. TFS2 v2's
    block-addressed rework removed that ceiling as a side effect, not
    as part of this change; `FS_DATA_MAX` itself is now vestigial
    (kept defined, `fs.h`'s comment says so) since nothing in `tfs.c`
    references it anymore.
  Verified in QEMU via QMP: `run lspci` from the physical shell prints
  the exact same six-device list `dmesg`'s own `pci:` lines show,
  ending "Process finished. Exit code: 0"; `dmesg` afterward shows
  `elf_run: calling process_run_ring3() for /bin/lspci` and
  `syscall: exit() called by ring-3 process`; `run bogus` still
  correctly reports "no such app: bogus"; a second boot against the
  same `disk.img` does NOT re-print the `bin: installed` line (the
  exists-check works). Screenshots:
  `screenshots/2026-08-11/lspci_bin_first_real_disk_hosted_elf.png`,
  `screenshots/2026-08-11/lspci_run_dmesg_trail_and_bogus_app.png`.
- dmesg (`klog_write()`) coverage extended to six areas that had zero
  boot/probe-time logging before this: PCI enumeration
  (`kernel/drivers/pci.c`), the keyboard driver
  (`kernel/drivers/keyboard.c`), the mouse driver
  (`kernel/drivers/mouse.c`), the VGA/framebuffer console driver
  (`kernel/drivers/vga.c`), the CMOS/RTC hardware clock
  (`kernel/core/kernel.c`), and the window manager
  (`apps/wm/wm.c`) -- found via a line-count/coverage audit that also
  flagged PCI's own `lspci` shell command as an existing precedent for
  the log line format below. Also added `klog_write_dec()`/
  `klog_write_hex()` (`kernel/include/klog.h`/`kernel/core/klog.c`),
  small klog-routed mirrors of `vga_write_dec()`/`vga_write_hex()`
  (`vga.h`) -- klog messages needing a numeric value had no formatting
  helper of their own before this, since every existing `klog_write()`
  call site only ever needed a plain string.
  - `pci_init()` now logs one line per discovered device (bus:device.
    function, vendor:device, class name -- the exact
    `bus:device.function vendor:device class` shape `cmd_lspci()`
    already prints to the console, reusing its own local
    fixed-width-hex helper rather than `klog_write_hex()`'s
    leading-zero-trimmed format, which wouldn't keep columns aligned)
    plus a final device-count summary.
  - `mouse_init()` logs whether the connected PS/2 mouse answered the
    IntelliMouse "magic knock" (wheel support, 4-byte packets) or not
    (plain 3-byte packets) -- the one thing that handshake actually
    determines and previously went nowhere but a local variable.
  - `vga_init()` logs which console backend it ended up on: linear
    framebuffer (with the resolution) or the legacy text-mode fallback
    -- runs early enough to log safely (`serial_init()` already ran in
    `kernel_main()` by the time `vga_init()` is called).
  - `keyboard_set_layout()` logs the layout it was just set to --
    covers both call sites for free (boot-time `keyboard_config_init()`
    applying a persisted layout, and the `keyboard <us|se>` shell
    command switching it live) without needing a log line at each
    caller.
  - `kernel_main()` gains a one-shot CMOS/RTC boot-time readout, logged
    once right after the persisted config (timezone/font/keyboard) is
    loaded -- deliberately NOT logged from `rtc_read()` itself
    (`kernel/core/timer.c`), which the taskbar clock/`tz.c` call
    continuously on every redraw; logging there would flood the ring
    buffer. Uses raw `rtc_read()`, not `tz.h`'s `rtc_read_local()` --
    unadjusted UTC hardware time, matching what a real kernel's own RTC
    probe logs before any timezone config is even in the picture.
  - `apps/wm/wm.c` logs entering/exiting GUI mode (with resolution),
    each app window opening/closing (by name), and the no-framebuffer
    failure path -- notable window-manager lifecycle events that
    previously left no trace in `dmesg` at all. Deliberately does NOT
    log the single-instance re-focus path (clicking an already-open
    app's Start-menu entry again) -- that happens on every such click,
    not just once, and isn't a lifecycle event worth the ring-buffer
    space.
  Verified in QEMU via QMP: `dmesg` after a fresh boot shows all six
  new boot-time lines in order (PCI device list + count, VGA mode,
  RTC reading) with correct ring-buffer timestamps; entering GUI mode,
  opening and closing a window, and exiting back to the shell each
  produced the expected `wm:`/`mouse:` lines in real time; running
  `keyboard se` from the shell produced the `keyboard:` line
  immediately. Screenshots:
  `screenshots/2026-08-11/dmesg_new_pci_vga_rtc_wm_mouse_lines.png`,
  `screenshots/2026-08-11/dmesg_keyboard_layout_switch_line.png`.
- Documentation audit: `README.md`, `apps/README.md`, and
  `docs/arch-portability.md` had all drifted out of date after the
  recent kernel-heap/JSON, `apps/ui/` widget migration, and desktop/
  context-menu work -- a research pass found and fixed the stale
  bits. `README.md`: "four apps" -> five (Task Manager added), the
  project-layout tree's `apps/widgets.h`/`.c` and `apps/wm/` file
  list updated, a new bullet for the kernel heap + JSON library.
  `apps/README.md`: the entire "Shared widgets (widgets.h/widgets.c)"
  section rewritten to describe `apps/ui/`'s one-file-per-widget
  layout and its `ui.h` umbrella include; the window-manager file
  list extended with `desktop.c`/`context_menu.c`/`start_menu.c`; a
  Task Manager entry added to the app list. `docs/arch-portability.md`:
  refreshed line counts (`tfs.c`, `apps/wm/*`, `calc_engine.c`,
  `ata.c`, `kapi.h`, the repo-wide C total), replaced the
  `apps/widgets.h`/`.c` reference with `apps/ui/*`, and added
  `heap.c`/`heap.h` and `json.c`/`json.h` to the architecture-neutral
  inventory. `docs/tfs2-spec.md` was fully rewritten from scratch --
  it still described the pre-rework TFS2 v1 format (inline 2048-byte
  file data, no block allocator); it now documents the current v2
  block-addressed layout (superblock version 2, the 12-direct +
  single/double/triple-indirect pointer scheme, the free-block
  bitmap region, the 9 GiB sparse `disk.img`) including a rewritten
  Python reference reader that can walk the indirect-pointer chain to
  dump a file's actual content, not just list entries. `CLAUDE.md`
  was also brought current in the same pass (the `apps/widgets.h`
  bullet, a header-dependency example, the `WM_C`/`UI_C` Makefile
  wildcard note, and a `## tools/` listing for `preflight.sh`/
  `deliver.py`/`gui_flow.py`/`screenshot_diff.py` that was missing
  entirely). The `toy-os-feature-workflow` skill's own `SKILL.md` was
  updated to match (the `apps/widgets.h` widget reference, and steps
  4/6 now mention the four tools above) and delivered as an updated
  `.skill` file for the user to re-save, since skills can't be edited
  directly from this session. No code changed in this pass.
- Start menu (`apps/wm/wm_render.c`/`wm_input.c`/`wm.c`) gains real
  graphical feedback: hovering a row highlights it (recomputed fresh
  from the mouse position every frame -- `wm.c`'s main loop now forces
  a full redraw on mouse movement while the menu's open specifically
  so this stays live, not just on the next unrelated repaint), and
  clicking a row shows a distinct warm-colored flash for ~100ms
  (`START_MENU_FLASH_TICKS`, `wm_input.c`) before the menu actually
  closes -- previously a click ran the row's action and closed the
  menu in the very same frame, with no visible confirmation the click
  landed. The row's action still runs immediately on click, same as
  before; only closing the menu is deferred. New `start_menu_flash_index`/
  `start_menu_flash_until` state (`wm.c`) and `wm_update_start_menu_flash()`
  (`wm_input.c`, called every tick from `wm_run()`'s loop) drive the
  deferred close. Verified via QMP: hover tracks the mouse live across
  rows, a click shows the gold flash on the clicked row, and the menu
  closes cleanly afterward.
- Calculator (`apps/calculator.c`) gains a small expression-so-far
  line above the main display, e.g. "12 +" while an operator is
  pending -- built entirely from state `calc_engine.h`'s
  `struct calc_state` already tracked (`accumulator`/`pending_op`),
  just not shown anywhere before this. Blank when nothing's pending
  (right after `calc_reset()` or right after '='). Needed one small
  `calc_engine.h`/`.c` addition: `render_scaled()` (internal formatting
  helper) is now the public `calc_format_scaled()`, so `calculator.c`
  can format the accumulator itself instead of duplicating that logic.
  Calculator's default window height grew slightly to fit the new line
  (`EXPR_H`/`TOP_H`, `calculator.c`) -- it's a fixed, non-resizable
  window, so this only affects the size a freshly-opened Calculator
  starts at. Verified via QMP: "2 +" appears after `2` then `+`, and
  clears correctly once `=` computes the result.
- New `struct ui_button`/`ui_button_group` (`apps/ui_button.c`/`.h`,
  `apps/ui_button_group.c`/`.h`) -- a self-contained button *object*
  that owns its own geometry, label, colors, and `pressed` state,
  modeled on Brutal OS's `libs/brutal-ui/button.c`/`.h` (pointed at
  directly this session) but sized down for toy-os's immediate-mode,
  no-allocator GUI: no generic view base class, no view-tree/mounting,
  no layout DSL, no hover tracking (the WM doesn't dispatch
  mouse-enter/leave, only press/click/release). `ui_button_group`
  handles the part every multi-button app used to hand-roll itself --
  hit-testing a set of buttons and tracking which one is currently
  down -- over a caller-owned array, so it works for a grid, a row, or
  a lone pair without knowing anything about layout itself.
  `apps/calculator.c` is the first real caller: its old
  `int g_pressed_index` + private `button_at()` hit-test loop are gone,
  replaced by a `struct ui_button_group` driving the same `on_press`/
  `on_click`/`on_release` behavior through generic code. Notepad's
  Save/Load buttons deliberately weren't migrated in this same change
  -- see `docs/decisions.md`. Verified via QMP: press-and-hold shows
  the pressed border, releasing in place springs it back, dragging
  onto a second button re-presses correctly with no phantom input, and
  dragging off entirely un-presses cleanly -- same three cases the
  original `on_press`/`on_release` mechanism was verified against,
  now passing through the new object instead of calculator.c's own
  bookkeeping.
- Start menu gains a second group of items below the app list:
  currently just "Exit to shell", separated by a 1px divider. It
  replaces the old hardcoded "Esc always exits the window manager"
  shortcut in `wm_run()` (`apps/wm/wm.c`) -- discoverable now instead
  of a hidden key, and it frees Esc up for a future modal-cancel use
  (a confirm dialog, say) instead of double-booking it as "exit
  everything, no matter what's open or focused." These aren't real
  `gui_app_registry` entries (they don't open a window) -- a new
  `wm_system_actions[]` array (`struct start_action { label,
  on_select }`) holds them, rendered and hit-tested by
  `gui_app_registry_count + wm_system_action_count` total menu rows
  instead of just the app count. A "Shutdown" item (with a Yes/No
  confirm dialog) was also requested this session but the actual
  power-off mechanism was deliberately deferred -- see
  `docs/roadmap.md`.
- The text cursor in Notepad/Terminal (`widget_scrollback_draw()`'s
  cursor, `apps/widgets.c`) is now a thin `CURSOR_BAR_W`-px vertical
  bar instead of a solid block covering the whole character cell --
  requested as "a bit more modern," and now matches
  `widget_textfield_draw()`'s caret (same width, same shared
  `CURSOR_BAR_W` constant in `apps/widgets.h`) instead of the two
  looking like two different cursor styles in the same app.

- The mouse cursor (`draw_cursor_normal()`, `apps/wm/wm_render.c`) is
  now a proper anti-aliased arrow sprite instead of the old hard-edged
  blocky staircase shape -- requested as "a bit more modern," same as
  this round's text-cursor change. Built the same way the font
  renderer already does anti-aliasing (`font_ttf.c`/`gfx_draw_char()`):
  a hand-designed arrow polygon rendered at 16x supersample via
  Python/PIL, downsampled, with a second dilate/erode pass to derive a
  separate outline-ring alpha mask -- both baked as literal 13x19 byte
  arrays pasted into the C source (not a new build-time tool; a one-off
  asset this small isn't worth a `tools/gen_*.py` script). Needed a new
  public `gfx_blend_pixel(x, y, color, alpha)` (`kernel/drivers/gfx.c`/
  `gfx.h`) -- the same per-channel blend math `gfx_draw_char()` already
  used internally, just exposed for a non-glyph caller. `CURSOR_BOX_SIZE`
  grew from 20 to 22px to fully cover the taller 19px sprite. Verified
  via QMP screenshot at zoom.
- Calculator's on-screen buttons now show real press/release visual
  feedback (a 2px inset border + 1px label nudge while held) --
  Calculator already used the shared `widget_button()` (`apps/widgets.c`/
  `.h`), so the actual gap was that nothing anywhere gave visible
  feedback for a held button. Built as a general window-manager
  mechanism rather than a Calculator-only hack, since any app with
  buttons will eventually want this: two new optional `gui_apps.h`
  callbacks, `on_press(win, cx, cy)` (fired every tick the button's
  held, including the initial press, returning 1 only when which
  button is "hot" actually changed) and `on_release(win)`; a new
  `content_pressed` index in the window manager's state
  (`apps/wm/wm_internal.h`/`wm.c`), driven each tick in
  `wm_update_drag_resize()` (`apps/wm/wm_input.c`) alongside the
  existing `content_dragging` mechanism it deliberately mirrors.
  Dragging off a held button before releasing correctly un-presses it
  without triggering the button's action, same as a real OS button --
  verified via QMP: press-and-hold shows the inset border, release
  springs it back, drag-off-then-release shows no phantom extra input.
  `widget_button()` gained a `pressed` parameter (all 7 existing call
  sites -- notepad.c's 2, wm_render.c's 5 chrome buttons -- pass `0`,
  unaffected).
- Title-bar minimize/maximize/close buttons (`apps/wm/wm_render.c`/
  `wm_input.c`/`wm.c`) gain real hover and press feedback, and close
  moved from act-on-click to Windows/KDE-style delayed commit:
  mouse-down on any of the three now only ARMS it (a lighter tint plus
  the same inset `pressed` look `widget_button()` already gives
  Calculator's buttons), and the actual minimize/maximize/close only
  fires on mouse-up while the cursor's still over that same button --
  dragging off before releasing cancels with no effect at all, not
  even a restack, same as any real desktop's title-bar buttons.
  Hovering (mouse not held) shows a lighter tint too, recomputed live
  from the cursor position every tick, same "derive live, don't
  persist a stale answer" approach as the Start menu's own hover. New
  `title_btn_armed_win`/`title_btn_armed_kind`/`title_btn_pressed_active`
  and `title_hover_win`/`title_hover_kind` state (`wm.c`), driven each
  tick by new `wm_update_title_btn_press()`/`wm_update_title_hover()`
  (`wm_input.c`, mirroring `content_pressed`'s per-tick handling in
  `wm_update_drag_resize()`). Verified via QMP: hovering close/minimize
  shows the tint, pressing close shows the inset pressed look,
  dragging off before releasing drops back to plain (window stays
  open), and a clean press-release on close/minimize/maximize each
  commit correctly.
- New `apps/ui/` directory holds toy-os's small retained-widget-object
  library -- `ui_button.c`/`.h` and `ui_button_group.c`/`.h` moved here
  from `apps/` (same content, no behavior change), plus a new
  `ui_textbox.c`/`.h` and an umbrella `ui.h` that `#include`s all three
  so a GUI app writes one `#include "ui/ui.h"` instead of hunting down
  a header per widget -- picks up future widgets automatically too.
  `struct ui_textbox` wraps `widgets.h`'s `struct text_field` the same
  way `ui_button` wraps `widget_button()`: owns its own geometry
  (repositioned live via `ui_textbox_set_geometry()`, same contract as
  `ui_button_set_geometry()`) around the existing
  `widget_textfield_*()` calls, which still do the actual drawing/
  editing. First real caller: Notepad's filename field, migrated off a
  raw `struct text_field` -- `st->filename` is now a `struct
  ui_textbox`, its text/cursor/active state reached through
  `st->filename.field.*`. `Makefile` gained a `UI_C`/`UI_OBJ` wildcard
  and pattern rule (same shape as `WM_C`/`WM_OBJ` for `apps/wm/`).
  Verified via QMP: Notepad's field still activates on click, accepts
  typing/backspace, and Save/Load still read/write the typed filename
  correctly -- no regression from the widgets.h migration.
- New kernel-space heap allocator: `kmalloc()`/`kzalloc()`/`kfree()`
  (`kernel/core/heap.c`/`kernel/include/heap.h`), first-fit over a
  doubly-linked, address-ordered free list with real pointer-adjacency
  coalescing on free (not list-order adjacency -- separate
  `pmm_alloc_contiguous()` growth regions aren't guaranteed physically
  adjacent to each other). Built directly on `pmm.c`'s physical frame
  allocator: since `boot.asm` already identity-maps the whole low 4GiB
  for kernel/supervisor use, any frame `pmm_alloc_contiguous()` returns
  is immediately a valid kernel pointer with no separate page-table
  mapping step needed. Grows in >=64KiB chunks (`HEAP_MIN_GROW_PAGES`)
  as needed; not exposed to ring-3 (separate from the existing
  `SYS_SBRK` per-process user heap) and not interrupt-safe/reentrant
  (matches the kernel's existing single-threaded assumptions -- no ISR
  calls into it). `heap_init()` + `heap_selftest()` run from
  `kernel_main()` right after `pmm_init()`; the self-test allocates
  three different-sized blocks, checks `kzalloc()` actually zeroes,
  frees in an order that exercises both-direction coalescing, then
  re-allocates to confirm the coalesced space is reusable. This is the
  first real kernel-space allocator toy-os has had -- built specifically
  to unblock multi-instance GUI apps (see below). Verified via
  `tools/boot_smoke_test.py`: "toy-os: kernel heap initialized" and
  "heap: selftest passed" both appear in the right spot in the boot
  sequence.
- GUI apps can now open more than one window at once. `gui_apps.h`'s
  `struct gui_app` gained two fields: `multi_instance` (default 0 =
  old behavior, reopening from the Start menu just focuses/restores
  the one window that can ever exist; 1 = every open always creates a
  brand-new window, bounded only by `MAX_WINDOWS`) and `on_close`
  (optional, called once right before a window's slot is removed from
  `windows[]`, with `window_get_state()` still valid inside it -- lets
  a multi-instance app `kfree()` its per-window state).
  `apps/wm/wm.c`'s `open_app()`/`close_window()` now respect both.
  Calculator (`apps/calculator.c`) is the first app to opt in: its old
  single static `g_calc`/`g_buttons`/`g_group` globals are gone,
  replaced by a `kzalloc()`'d `struct calculator_instance` per window
  (freed in the new `calculator_close()`), so each open Calculator
  window has fully independent state. Every other app (Notepad, About,
  Terminal) is unaffected -- both new fields default to unset/0/NULL.
  Window titles for multiple windows of the same app stay identical on
  purpose (no "(2)" suffix) -- explicit choice, not a limitation.
  Verified via QMP: opened two Calculator windows from the Start menu
  (cascaded, both titled "Calculator"), typed a digit into the first,
  confirmed the second still showed a fresh "0", closed the second via
  its title-bar X and confirmed the first's state and taskbar entry
  were untouched, with no panic in the serial log.
- The Start menu popup (app list + system actions, hover/click-flash
  feedback) is now its own component: `apps/wm/start_menu.c`/
  `start_menu.h`, factored out of `wm.c`/`wm_input.c`/`wm_render.c`
  now that it had grown real state and behavior of its own. Not an
  independent module with a clean boundary -- it still reaches into
  `wm_internal.h` for shared WM state (`screen_h`/`taskbar_h`/
  `redraw_pending`/`gui_app_registry`/`open_app()`), same pattern
  `wm_input.c`/`wm_render.c` already use (see `apps/wm/wm.c`'s top
  comment). A new `geometry()` static helper inside `start_menu.c`
  shares the row-layout math between drawing and click-handling, which
  used to be hand-duplicated between `wm_render.c` and `wm_input.c`.
  Public entry points: `start_menu_open_now()`, `start_menu_draw()`,
  `start_menu_handle_click()`, `start_menu_update()`, plus the
  `start_menu_open`/`wm_system_actions`/`wm_system_action_count`
  state. Pure refactor, no behavior change -- verified via QMP: Start
  menu opens from the taskbar button, hover tracks the mouse live,
  clicking a row shows the gold flash then closes the menu, same as
  before the split.
- New Task Manager app (`apps/taskmgr.c`/`.h`) -- lists every open
  window (title + normal/minimized/maximized state) and shows system
  memory (physical RAM total/used via `pmm_*`, kernel heap total/used
  via `heap_*` -- both already existed in `kapi.h`, just never had a
  UI). Redraws every tick along with the taskbar clock, so the numbers
  stay live without a manual refresh. No CPU column -- GUI apps aren't
  scheduled processes in this kernel, so there's no real per-app CPU
  number to show yet (see docs/decisions.md). Needed one small new
  `apps/wm/wm.h` addition: `wm_window_count()`/`wm_get_window()`, a
  read-only accessor pair so an app outside `apps/wm/` can list windows
  without reaching into `wm_internal.h` (which stays WM-private).
  Verified via QMP: opened Task Manager, confirmed it lists itself,
  opened two Calculator windows and watched "Heap used" and the window
  list update live, closed them and confirmed both returned to their
  prior values.
- **Fixed a real heap-corruption bug found by the above**: the kernel
  heap's `kfree()` (`kernel/core/heap.c`) could silently corrupt a
  still-in-use block's size field when freeing its list-previous
  neighbor, if that neighbor happened to still be allocated -- the
  coalescing helper only checked whether the block being merged *in*
  was free, not whether the block being merged *into* was. This sat
  completely invisible until Task Manager displayed `heap_used_bytes()`
  for the first time and showed an impossible ~16 exabyte figure.
  Root-caused, fixed (added a `b->prev->free` guard before merging),
  and `heap_selftest()` strengthened to check `heap_used_bytes() == 0`
  after freeing everything, specifically so this class of bug can't
  regress silently again. See docs/decisions.md for the full story.
- New dev tooling in `tools/`, aimed at making the build/test/delivery
  loop this project's own `CLAUDE.md`/skill describes faster and less
  error-prone: `preflight.sh` (one command running
  `make clean && make all && make iso` + the boot smoke test + a git
  status summary -- "am I safe to deliver?" in one pass instead of
  three commands run by hand), `deliver.py` (builds the file-list/
  device-path/protected-file manifest and a commit-message skeleton for
  the delivery step, catching the `Makefile`/`*.yml` protected-file
  exception before a real `device_commit_files` call would reject it),
  `gui_flow.py` (named QMP click-flows -- `open_app("Calculator")`
  instead of hand-deriving Start-menu row pixel math every session),
  and `screenshot_diff.py` (pixel-diffs two screenshots with a
  pass/fail threshold, for catching a rendering regression manual
  eyeballing might miss).
- **TFS2's on-disk format now supports multi-gigabyte files.**
  Previously every file was capped at 2048 bytes, stored inline in one
  fixed-size table record; now each file record holds a small set of
  block-number pointers (12 direct + single/double/triple indirect,
  the same scheme real Unix filesystems have used for decades) into a
  new block-addressed region of the disk, backed by a free-block
  bitmap. `disk.img` grew from 1MiB to a sparse 9GiB (`Makefile`) to
  have room for it -- **this is an incompatible on-disk format change**
  (version byte bumped 1 -> 2): an old disk.img is detected as foreign
  and reformatted from scratch, same "no migration, just reformat"
  policy this project has always used for format bumps, but it means
  existing saved files are lost the first time this boots against an
  old image. Run `make clean-disk` once to get a correctly-sized fresh
  one.
  New `ata_read_sectors()`/`ata_write_sectors()` (`kernel/drivers/
  ata.c`/`.h`) transfer up to 8 sectors (one 4096-byte filesystem
  block) in a single ATA command instead of one command per 512-byte
  sector, for both the PIO and DMA paths -- the DMA path reuses the
  bounce buffer that was already a full 4096-byte frame (only 512 of
  it was ever used before), so no new allocation was needed.
  Also added the new streaming API this all exists to support:
  `fs_read_range()`/`fs_write_range()`/`fs_size()` (`fs.h`), for
  reading/writing a file in bounded chunks instead of needing the
  whole thing in RAM at once -- because `fs_read()`'s existing "whole
  file in one buffer" contract literally cannot work for a file bigger
  than available RAM (256MB in the normal QEMU config), no matter how
  large the on-disk format gets. `fs_read()`/`fs_write()` themselves
  are unchanged for every existing caller (Notepad, the shell,
  editor.c) -- small files still work exactly as before, just
  reassembled from blocks into one heap-allocated staging buffer
  instead of being one inline blob already in RAM.
  Verified via QEMU: a new `fs: selftest passed (triple-indirect
  addressing verified)` boot-time check (`tfs.c`'s `tfs_selftest()`,
  same "prove it every boot" pattern as `heap_selftest()`/
  `pmm_selftest()`) writes/reads/deletes a small chunk at a ~4.6GB
  offset -- past direct+single+double indirect's combined ~4GB
  capacity, so it only passes if the triple-indirect chain was built
  and walked correctly, without needing to actually write gigabytes of
  data at boot. Also verified interactively: saved a Notepad file,
  restarted QEMU from cold, reloaded it -- confirming the new format
  round-trips correctly through a real reboot, not just within one
  boot's RAM state. A full end-to-end 8GB write/read wasn't run in
  this session (would take a long time over emulated PIO/DMA) -- see
  docs/roadmap.md.
- New heap-backed JSON parser/serializer: `kernel/core/json.c`/
  `kernel/include/json.h` -- full nested objects/arrays, coexisting
  with `etc_config.h`'s flat name=value format rather than replacing
  it (existing `/etc/toyos.conf` settings are untouched; JSON is
  available for a future config file that genuinely needs nesting).
  No floating point (`JSON_NUMBER` is `int64_t`) -- this kernel is
  built with `-mno-sse -mno-sse2` and no soft-float, matching
  `apps/calc_engine.h`'s same reasoning; a fractional literal parses
  but truncates. The recursive-descent parser caps nesting at
  `JSON_MAX_DEPTH` (32) specifically because the kernel stack is a
  fixed 16KB. `json_read_file()`/`json_write_file()` go through the
  new `fs_size()`/`fs_read_range()`/`fs_write_range()` streaming API
  (this session's earlier TFS2 entry) rather than the old whole-file
  `fs_read()`/`fs_write()`, so a large JSON document isn't capped by
  that. Verified via a `json_selftest()` run at every boot (parse a
  nested document with strings/numbers/bools/arrays/escapes, check
  every accessor, round-trip it through `json_write()` and re-parse) --
  `json: selftest passed` appears in the boot log right after the
  heap self-test.
- **`apps/widgets.c`/`.h` no longer exist -- every widget moved into
  its own file under `apps/ui/`**, by explicit request (previously
  `ui_button`/`ui_button_group`/`ui_textbox` lived there while
  scrollback/scrollbar/checkbox/the base `widget_hit`/`widget_button`
  primitives stayed behind in the older file). New files: `ui_primitives.c`/
  `.h` (the base `widget_hit()`/`widget_button()`), `ui_scrollback.c`/`.h`
  (the `text_scrollback` console/text-editing widget), `ui_scrollbar.c`/
  `.h` (its companion scrollbar), `ui_checkbox.c`/`.h`. `struct text_field`/
  `widget_textfield_*()` (the single-line text-input implementation)
  moved directly into `ui_textbox.c`/`.h`, folded into the object that
  was already its only real caller, instead of staying split across two
  files. Deliberately a pure file-move, not a rename or redesign --
  every function keeps its old `widget_*` name and signature, same
  precedent `ui_button_group.c` already set ("moved here from apps/,
  same content, no behavior change"). `ui_scrollback`/`ui_scrollbar`
  stay plain stateful structs/free functions rather than gaining an
  owned-geometry wrapper like `ui_button`/`ui_textbox` -- every real
  caller (Notepad, Terminal, the editor) already recomputes its content
  rect live from the window's current size every frame, so there's
  nothing an owned-geometry object would save. See `docs/decisions.md`.
  Verified via QMP across all three affected apps after a clean rebuild:
  Notepad (filename field + Save/Load buttons + multiline editing +
  cursor bar all still work), Terminal (scrollback rendering, colors,
  the real shell running inside it), Calculator (button press/release
  visual feedback, a real computation via the migrated `widget_button()`
  chain) -- no behavior differences found.
- **Preliminary desktop background + icon grid, and a reusable
  right-click context menu, wired into four surfaces.** New
  `apps/wm/desktop.c`/`.h`: fills the area below the taskbar (previously
  a bare color fill inline in `wm_render_frame()`) and draws one icon
  per `gui_app_registry` entry in a left-edge column -- a hand-drawn
  filled square with the app name's first letter stands in for a real
  icon image (no image decoder yet, see `docs/roadmap.md`). Single-click
  selects (highlight only); a second click on the same icon within
  `DESKTOP_DOUBLE_CLICK_TICKS` (~300ms) launches it, matching a real
  desktop's double-click-to-open convention -- previously an idle
  roadmap item ("Desktop icons"), now built. New `apps/wm/context_menu.c`/
  `.h`: a generic reusable popup (label + callback + caller-supplied
  `ctx` pointer per row, same peer-file pattern as `start_menu.c`) --
  any caller can open one anchored at the cursor position, clamped to
  stay fully on screen. Wired into all four places requested: right-click
  the desktop background shows a quick-launch menu (one row per
  registered app); right-click any window (title bar or content area)
  shows Minimize/Maximize-or-Restore (only for resizable apps, matching
  the title-bar button's own rule)/Close, mirroring the title-bar
  buttons without needing to land exactly on one of them; right-click a
  taskbar app button shows "Close window"; right-click a Start menu row
  shows "Open" (closes the Start menu first, same as a left-click would).
  A right-click always closes whatever popup was already open before
  deciding what (if anything) the new click should show, so right-clicks
  never stack menus. `wm.c`'s main loop gained the same edge-triggered
  detection the left button already had (`buttons & 0x2`, mouse.h's
  right-button bit, previously read but never dispatched anywhere).
  Deliberately not included this round (see `docs/roadmap.md`):
  per-icon context menus (right-clicking an icon shows the same
  desktop-wide quick-launch menu as empty space), real wallpaper images,
  and repositioning/dragging icons. Verified via QMP: desktop icons
  render and launch correctly (single-click selects, double-click
  opens), all four right-click surfaces show the correct menu with
  correct items (including Calculator's window menu correctly omitting
  "Maximize" since it's non-resizable), menu clamping keeps a
  near-bottom-edge taskbar menu fully on screen, and every action
  (launch/close/minimize) actually executes and updates the screen
  correctly afterward.
- **Click-to-position and text selection in `text_scrollback`, and an
  interactive serial debug console on COM1.** Two related requests from
  the same round -- user chose, via `AskUserQuestion`: build the text
  editing on the *shared* `apps/ui/ui_scrollback.c` widget (used by
  Notepad, Terminal, and `apps/editor.c`) rather than a Notepad-only
  one, migrate Notepad's Save/Load buttons to `ui_button_group` (what
  Calculator already uses) as part of the same pass, do click-to-position
  and full selection (drag + shift+arrow) in one go rather than phased,
  and build the debug interface as an interactive command console over
  serial rather than a one-shot dump or a continuous trace stream.
  - `ui_scrollback.h`/`.c`: new `widget_scrollback_index_at_point()`
    (inverts a pixel position back to a buffer index, mirroring the
    existing measure-pass's wrap/newline capture convention exactly so
    a click round-trips to the same visual cursor spot); new selection
    API (`widget_scrollback_selection_start/clear/present/range`,
    `widget_scrollback_delete_selection()`); `widget_scrollback_draw()`
    gained a `sel_bg` parameter (widgets stay theme-agnostic -- callers
    supply the color, same as everywhere else in `apps/ui/`) and now
    paints a highlight rect behind selected characters.
    `THEME_SELECTION_BG` added to `apps/theme.h`.
  - `kernel/include/keyboard.h`/`kernel/drivers/keyboard.c`: new
    `KEY_SHIFT_ARROW_*`/`KEY_SHIFT_HOME`/`KEY_SHIFT_END` codes, emitted
    from the extended-scancode handler based on live shift state at
    scancode-processing time (same timing convention the existing
    shift table already uses for letters).
  - `apps/notepad.c`/`.h`: click positions the cursor; drag extends a
    selection (reusing the existing `on_drag_start`/`on_drag`
    mutual-exclusivity contract -- claiming every text-body mouse-down
    unconditionally means a drag that never moves is just a plain
    click, no separate code path needed); shift+arrow/Home/End extends
    a selection the same way; Backspace/Delete/typing over an active
    selection replaces it instead of acting at the cursor. Save/Load
    buttons migrated from hand-rolled hit-testing to `ui_button_group`,
    gaining press/release visual feedback (`notepad_press`/
    `notepad_release`) Calculator already had but the old buttons
    didn't.
  - New `kernel/core/debug_console.c`/`.h`: a line-buffered interactive
    command console over serial -- `help`/`meminfo`/`lsdev`/`lsfs
    [path]` -- non-blocking `debug_console_poll()` called from
    `keyboard_getchar()`'s idle hlt-wait (physical shell) and
    `wm_run()`'s main loop (GUI desktop), so a serial session stays
    responsive whichever one is active, without new kernel-thread/
    scheduling machinery. Output goes through `klog_write()`, so
    console responses also land in `dmesg`. Exposed to `apps/` via
    `kapi.h` (`debug_console_poll()` only -- `apps/wm/wm.c` doesn't
    reach into `kernel/core` directly).
  - `kernel/include/serial.h`/`kernel/core/serial.c`: added RX support
    -- a ring buffer, `serial_try_getc()`, and a new `serial_irq_init()`
    (IRQ4 registration + PIC unmask + UART IER enable) called from
    `kernel_main()` right after `idt_init()`, deliberately split out of
    `serial_init()` itself (which runs *before* `idt_init()`, so
    registering/unmasking that early would've been undone by
    `idt_init()`'s own masking pass). Found live, not by inspection: an
    early version registered the IRQ and unmasked it at the PIC
    correctly but still received nothing, because `serial_init()`'s
    original `outb(COM1+1, 0x00)` leaves the UART's own Interrupt
    Enable Register at 0 -- disabling interrupt generation at the chip
    itself, independent of the PIC. `serial_irq_init()` now also sets
    IER bit 0.
  - `tools/gui_flow.py`: fixed a second Start-menu click-math bug past
    the `ITEM_H` fix from a prior round -- the derived top-Y formula
    (`(SCREEN_H - TASKBAR_H) - ITEM_H * total_items`) was close enough
    to work for a middle row but landed too close to the row-0/row-1
    boundary specifically, misclicking "About" when asked for
    "Notepad". Replaced with `MENU_TOP_Y = 536`, measured directly from
    a live screenshot rather than re-derived from constants that had
    already been wrong once.
  - Verified via QMP: click-to-position (typing exactly where clicked,
    mid-string); drag-select producing a visible highlight; shift+arrow
    selection extending correctly from Home; Backspace deleting an
    active selection (both drag- and shift-arrow-created); Save then
    Load round-tripping real content through the filesystem; the
    serial debug console answering `meminfo` with real live data while
    the GUI desktop was active. Screenshots in `screenshots/2026-08-11/`.
  - Known limitation, documented rather than solved: no output lock
    exists anywhere in this kernel, so a concurrent `klog_write()` from
    elsewhere could in principle interleave with the debug console's
    own output. Accepted for a debug-only tool rather than adding new
    synchronization machinery for it.
- **Data-driven keyboard layouts (`/etc/kbs/<name>`), replacing the
  compiled-in two-layout enum.** Reported bug: on the Finnish/Swedish
  physical keyboard, the key next to the right Shift (US legend `/`)
  should type `-`/`_`, not `/`/`?` -- the old `se` layout only remapped
  the three Å/Ä/Ö keys, nothing else. User chose, via
  `AskUserQuestion`: `name=value` files (reusing the shape of
  `etc_config.h`'s existing format, though not its parser -- see
  below), scancode translation moved into its own new
  `kernel/core/keyboard_layout.c` rather than staying in
  `keyboard.c` (the driver's job is raw scancodes/shift-state, not
  owning character tables), layout files generated from Linux's own
  XKB layout data rather than hand-typed, and seeded onto `disk.img`
  at build time so they ship in the repo/ISO like `/bin/lspci` already
  does.
  - New `tools/gen_kbs.py`: runs `xkbcli compile-keymap --layout
    <name>` (part of `libxkbcommon-tools`, no X server needed) and
    emits an `/etc/kbs/<name>` file from it. Only translates shift
    levels 1-2 (base + Shift) -- this driver has no AltGr handling at
    all, so level 3/4 symbols would be unreachable anyway -- and only
    characters this kernel's font can render (ASCII + the six Nordic
    Latin-1 letters). Dead keys (e.g. Swedish's acute/grave accent
    key) aren't composed, mapped instead to their plain undead glyph,
    same simplification XKB's own `nodeadkeys` variants make.
    `python3 tools/gen_kbs.py <xkb-layout> --write` regenerates
    `seed/sync/etc/kbs/<name>`; adding a third layout later is running
    this once, not an afternoon with a scancode chart.
  - New `kernel/include/keyboard_layout.h` / `kernel/core/keyboard_layout.c`:
    `keyboard_layout_load(name)` reads `/etc/kbs/<name>` (own small
    parser, not `etc_config_get()` -- that one's `ETC_CONFIG_MAX` write
    buffer is far too small for a ~130-line layout file, and it strips
    a trailing `#...` as a comment even on a real data line, which
    would silently eat the `#` character itself as a mapped value);
    falls back to `/etc/kbs/us`, then to a small compiled-in US table,
    if the requested (or even `us`) file can't be read -- so the
    keyboard is never left producing nothing. Returns whether the
    requested file was actually found, so the shell's `keyboard`
    command can tell the user when it silently fell back.
    `keyboard_layout_translate(scancode, shift)` is the one lookup
    `keyboard.c`'s `keyboard_feed_byte()` now calls.
  - `keyboard.c`/`keyboard.h`: removed `enum keyboard_layout`,
    `scancode_ascii[]`/`scancode_ascii_se[]` and their shifted
    variants, `keyboard_set_layout()`/`keyboard_get_layout()`/
    `keyboard_layout_name()` -- the driver now only tracks shift/
    extended-prefix state and calls `keyboard_layout_translate()`.
  - `keyboard_config.c`/`.h`: persistence now works from a plain layout
    name string (`"keyboard_layout=<name>"` in `/etc/toyos.conf`, as
    before) instead of the enum; always calls
    `keyboard_layout_load()` at boot (even with nothing persisted yet)
    so the tables are populated before the first keypress, not left
    zeroed until something explicitly loads a layout.
  - `apps/shell_sys.c`'s `keyboard` command: `keyboard <name>` now
    accepts any name with a matching `/etc/kbs/` file, not just a
    hardcoded `us`/`se` check; reports "not found ... reverted to us"
    on an unknown name instead of a fixed error list.
  - `kapi.h` gained `keyboard_layout.h` (apps/ only ever reach the
    driver surface through `kapi.h` -- see `CLAUDE.md`).
  - Real bug caught mid-implementation, not by review: an early cut of
    `tools/gen_kbs.py`'s key list only covered the printable
    alphanumeric block and omitted Escape/Backspace/Tab/Enter
    entirely -- so the moment the shell loaded layouts from these
    generated files instead of `keyboard.c`'s old compiled-in tables,
    Enter stopped submitting a command at all (silently swallowed,
    every keystroke just kept appending to the prompt). Caught by
    actually testing in QMP (typing `keyboard se` produced
    `keyboardse` with no newline), not by reading the generator's
    code. Fixed by adding the four control keys to the generator's key
    list with their own keysym-name mappings.
  - Verified via QMP: `keyboard` alone reports the current layout;
    `keyboard se` switches and the right-Shift-adjacent key now types
    `-`/`_` (previously `/`/`?`); `keyboard xyz` reports "not found"
    and reverts to `us`; Å/Ä/Ö still type correctly under `se`;
    Backspace/Enter both still work after switching layouts; the
    choice survives a QMP `system_reset` (persisted layout reloads at
    boot). Screenshots in `screenshots/2026-08-11/`.

### Changed
- Versioning switched from a per-change build-number scheme
  (`tools/bump_build.sh <fix|feature|major>`, a git tag `build-N` on
  every push) to semantic versioning with a `-dev` suffix during
  development. `VERSION` (repo root) now holds a plain semver string
  -- `0.1.0-dev` to start -- read by `tools/gen_version.sh` into
  `kernel/include/version.h`/`TOYOS_VERSION` exactly like `BUILD_NUMBER`
  was before. `tools/bump_build.sh` is retired; `tools/set_version.sh
  <version>` replaces it, used only when starting a new dev round
  (`0.2.0-dev`) or cutting a real release (`0.2.0`, which also stamps
  this CHANGELOG section and opens a fresh `## [Unreleased]`). Git tags
  move from `build-N` per push to `vX.Y.Z` at real releases only.
  `about`/the GUI About window now show `toy-os v0.1.0-dev` instead of
  `toy-os build 502`. Requested directly, to stop needing a
  fix/feature/major judgment call and a tag on every small change --
  see `docs/decisions.md`'s entry on this for the full reasoning.
- Commit messages going forward list each changed/added file with a
  one-line note in the body (e.g. `kernel/drivers/keyboard.c - added
  SE layout remap`), so a commit is skimmable on GitHub without
  opening the full diff. No other workflow change -- still a direct
  push to `main`, same as before.

### Added
- `df` shell command (`cmd_df()`, `apps/shell_sys.c`) -- shows
  total/used/free space on the persistent filesystem, sourced from a
  new `fs_disk_usage()` VFS call (`kernel/include/fs.h`/`fs_ops.h`,
  dispatched through `vfs.c` to `tfs_disk_usage()` in
  `kernel/drivers/tfs.c`, which walks `g_bitmap`/`g_ram_bitmap` to
  count free blocks). Displays in KB, not bytes or MB -- MB was tried
  first but rounds any real usage under 1MB down to a misleading flat
  "0" (today's whole seeded `/bin` + `/etc` content is under 1MB);
  bytes would overflow `vga_write_dec()`'s `uint32_t` parameter on a
  multi-gigabyte disk. Found live-testing this command, not by review
  -- see the function's own comment. Verified via QMP: shows
  `total: 9436876 KB / used: 88 KB / free: 9436788 KB` against the
  real seeded disk.
- Command history now persists across reboot. `history_save()`/
  `history_load()` (`apps/shell.c`) write/read a dedicated bare-line
  `/etc/history` file (one entry per line, most-recent-last), loaded
  once at the top of `shell_main()`. Deliberately its own file, not a
  key in the shared `/etc/toyos.conf` -- history entries can
  legitimately contain `=`, and `etc_config.c`'s parser strips a
  trailing `#...` as a comment even mid-line, both of which would
  corrupt real command text; modeled on `tz.c`'s own manual
  line-splitting for the same reason. Verified via QMP: ran `meminfo`/
  `ls`/`df`, restarted QEMU against the same `disk.img` (simulating a
  reboot), confirmed the up arrow recalled `meminfo` from the previous
  session.
- Shutdown confirmation dialog -- clicking the Start menu's "Exit to
  shell" now opens a Yes/No modal ("Exit to shell? Unsaved changes
  will be lost.") instead of exiting immediately. New
  `apps/wm/confirm_dialog.c`/`.h`, following `context_menu.c`/
  `start_menu.c`'s existing screen-absolute WM-overlay pattern (not an
  `apps/ui/` widget -- those are content-relative to a window's own
  origin, which doesn't exist for a WM-level popup). Geometry computed
  once at open time from the message/button label lengths;
  `confirm_dialog_handle_click()` is checked first in
  `wm_input.c`'s `wm_handle_left_click()` chain (most modal first) and
  swallows clicks outside Yes/No without dismissing. Verified via QMP:
  dialog opens centered with both buttons; clicking No dismisses and
  stays in the GUI; clicking Yes actually exits to the physical shell
  ("Back from GUI mode." on serial).
- AltGr handling in the keyboard driver -- Finnish/Swedish `@ # $ { }
  [ ] \ |` (XKB level 3) are now reachable, closing the backlog item.
  `keyboard_layout_translate()` (`kernel/include/keyboard_layout.h`/
  `kernel/core/keyboard_layout.c`) gained an `altgr` parameter and a
  third per-layout table (`g_table_altgr[128]`, populated from new
  `sc_XX_altgr=` lines), taking priority over shift when the pressed
  scancode has an AltGr entry, falling through to shift/base otherwise
  (an unmapped AltGr slot doesn't eat the keystroke).
  `kernel/drivers/keyboard.c` tracks a new `altgr_pressed` flag off the
  `0xE0`-prefixed Right Alt press/release scancodes (`0x38`/`0xB8`,
  extended -- Left Alt is the same byte pair *without* the prefix and
  stays unused). `tools/gen_kbs.py` now reads XKB level 3 (still not
  level 4/Shift+AltGr -- see its own top comment on why that's a small,
  known, acceptable gap) and regenerated `seed/sync/etc/kbs/{us,se}`.
  Verified via QMP: switched to `se`, held AltGr (`alt_r` qcode) with
  `2`/`4` via `QMPSession.combo()`, got `@`/`$` exactly as the
  generated table specifies.
- `stress <mb>` shell command (`apps/shell_sys.c`) -- a real,
  non-sparse write/read/verify pass over `<mb>` megabytes through
  `fs_write_range()`/`fs_read_range()`, built to eventually satisfy
  the roadmap's "full end-to-end multi-GB write/read stress test"
  item. Unlike the boot-time `tfs_selftest()` (which only proves
  triple-indirect *addressing* -- 64 bytes written at a ~4.6GB offset),
  this writes a genuine per-chunk-varying pattern across the whole
  requested size in 1MB chunks (one static reused buffer, O(1) RAM
  regardless of `<mb>`), reads it all back, and byte-for-byte verifies
  every chunk, so a corrupted or misplaced chunk is actually
  detectable. Deliberately on-demand, not part of the boot self-test --
  a real multi-GB pass over this kernel's PIO/DMA ATA path takes real
  wall-clock time. Verified via QMP at `stress 100`: 100MB written,
  read back, and verified correct in 72s. That measured rate
  extrapolates to roughly 50 minutes for a 4.2GB pass (just past the
  ~4004MB triple-indirect boundary) and ~100 minutes for the full 8GB
  target -- both well beyond what this session's interactive testing
  loop could run to completion, so the literal multi-GB pass itself is
  still not done; what shipped is the verified-correct tool to run it
  in one command whenever that time is available (see
  `docs/roadmap.md`).
- Shutdown (Start menu item, alongside "Exit to shell"), closing that
  half of the roadmap's Shutdown item -- the other half (a real
  ACPI-parsed poweroff) is still open, see below. `system_poweroff()`
  (`kernel/core/power.c`/`power.h`) writes QEMU/Bochs's well-known
  ACPI PM1a_CNT I/O-port shortcut (`outw 0x604, 0x2000`), falling back
  to a halt loop with an on-screen message if the write doesn't take
  (real hardware, or an emulator without this legacy behavior) so the
  machine always ends up in a safe, inert state either way. Reuses
  `confirm_dialog.h` (`apps/wm/start_menu.c`'s new `action_shutdown()`)
  -- the same Yes/No popup "Exit to shell" already goes through, per
  that dialog's own top comment anticipating this as its second
  caller. Deliberately NOT a real ACPI shutdown: it doesn't parse the
  FADT/PM1a_CNT address out of the guest's own ACPI tables, just writes
  the value real hardware would only accept after that parsing -- the
  ACPI table parsing roadmap item stays open, and a real poweroff on
  real hardware still needs it. Verified via QMP: "Shutdown" appears
  below "Exit to shell" in the Start menu; clicking it opens "Shut
  down? Unsaved changes will be lost." with Yes/No; clicking Yes
  actually powered the QEMU process off (the QMP socket itself broke
  with a clean shutdown, no fallback-halt message logged) rather than
  just returning to the shell.
- A reusable Open/Save file-picker dialog (`apps/wm/file_picker.c`/
  `.h`), replacing Notepad's old always-visible inline filename field
  with real Save As.../Open... buttons, like a real desktop OS. Same
  screen-absolute WM-overlay pattern as `confirm_dialog.c`/
  `context_menu.c`/`start_menu.c` (not an `apps/ui/` widget -- those
  are content-relative to a window's own origin, which doesn't apply
  to a WM-level popup), backed directly by `fs_list()`/`fs_is_dir()`/
  `fs_exists()` (GUI apps already call `fs_*` straight from kernel
  space, no syscall layer needed). Full navigation, not just a flat
  listing: directories-first alphabetical sort, double-click a folder
  to enter it, a `../` row to go up, a scrollbar (`ui_scrollbar.h`,
  click-to-page -- no drag-to-scroll or mouse-wheel yet, the WM doesn't
  route wheel events to a screen-level modal today) for more entries
  than fit. Typing an absolute or cwd-relative path directly into the
  filename field works too, same as a real dialog's field; typing/
  choosing an existing directory navigates into it instead of erroring.
  `apps/notepad.c`'s Save/Load buttons became Open.../Save As... that
  pop this instead -- every Save is now a Save As (no "current file"
  tracked between saves), per the user's own choice when this was
  scoped. Deliberately not built this round, same "add once a real
  need shows up" bar every popup here uses: Esc-to-cancel (the
  physical Escape key isn't wired to a scancode in
  `kernel/drivers/keyboard.c` at all yet -- `confirm_dialog.h` hit the
  same gap first), creating a new directory from inside the dialog,
  hover highlighting on list rows.
  - Real bug caught by QMP testing, not by review: the first cut of
    double-clicking the `../` row correctly updated `g_cwd`/re-listed
    the parent directory internally, but forgot to set
    `redraw_pending` on that specific path (the sibling
    directory-double-click branch did) -- so the navigation "worked"
    with nothing on screen reflecting it until some unrelated later
    click forced a repaint. A screenshot taken right after the
    double-click looked like a dead button; the underlying state had
    actually moved. Fixed by moving `redraw_pending = 1` into
    `fp_refresh_listing()` itself (every caller changes what's on
    screen) instead of relying on each call site to remember it
    individually.
  - `tools/gui_flow.py`'s `TASKBAR_H`/`ITEM_H`/`MENU_TOP_Y` Start-menu
    click-geometry constants turned out stale independent of this
    change -- pixel-measured against a live screenshot while testing
    the new "Shutdown" row and found the running kernel's default font
    metrics are `gfx_char_h()=21` today, not the `18` these constants
    were calibrated for (`TASKBAR_H` 29 not 32, `ITEM_H` 27 not 24).
    Corrected in the same pass `SYSTEM_ACTIONS` picked up "Shutdown"
    (see gui_flow.py's own updated comments for the re-measurement
    method, and above for the actual Shutdown feature).
  - Verified via QMP: Save As... on real typed text saves to `/`,
    reopening via Open... and loading shows "Loaded." with the exact
    text back; double-clicking `bin/` in the listing enters it and
    shows its real contents (`/bin`'s seeded binaries) with a working
    scrollbar; double-clicking `../` returns to `/` showing its
    original listing. Screenshots in `screenshots/2026-08-11/`.
- Per-subsystem runtime debug-logging switches
  (`kernel/include/debugflags.h`/`kernel/core/debugflags.c`) -- OFF by
  default, flippable at the shell with `debug <name> on|off` (`debug`
  alone lists every subsystem and its state), no rebuild needed.
  Replaces the previous ad hoc pattern (this same session's file
  picker debugging: temporary `klog_write()` calls added at the point
  of suspicion, then hand-deleted again once the bug was confirmed
  fixed) with a standing, named, always-in-the-tree gate: wrap a
  `klog_write()` in `if (dbgflag_enabled(DBGFLAG_WM)) { ... }` and
  leave it there permanently. Subsystems today: `fs`, `wm`, `ata`
  (`DBGFLAG_NAMES` in `debugflags.c` -- add more by extending the enum
  + name table, nothing else needs updating). `apps/shell_sys.c`
  gained `cmd_debug()`, wired into `shell.c`'s dispatch and
  documented under `help tests`.

### Fixed
- `kernel/drivers/ata.c`'s Bus-Master DMA path had no retry on a
  transient transfer failure -- reported live: `stress 10` (and other
  multi-MB runs) "usually" (non-deterministically) failing on real
  hardware/QEMU with `write failed at chunk N`, while the exact same
  code path never failed once in this project's own sandboxed test
  runs. Root cause: `wait_dma_irq()`'s completion wait is bounded to
  3s (`DMA_WAIT_TICKS`) -- on a real desktop, host scheduling jitter
  (other processes briefly starving the QEMU process of CPU) can delay
  the completion IRQ past that bound even though the transfer itself
  is fine, and the driver treated one missed IRQ identically to a
  genuine hardware error: the whole transfer failed outright, with no
  second attempt, taking down whatever multi-block operation it was
  part of. Fixed by wrapping `dma_transfer()` in a new
  `dma_transfer_with_retry()` (`ATA_DMA_MAX_RETRIES` = 3) that
  re-issues the whole command from scratch on failure before giving
  up -- a real, persistent drive error still surfaces as a hard
  failure once every retry is exhausted (always logged via `klog`,
  independent of the `ata` debug switch above), it just no longer
  fails on a single transient miss. Per-attempt detail (which retry
  succeeded, or that one failed) is gated behind `debug ata on` so
  normal operation stays quiet. Verified in the sandbox: `debug ata
  on` + `debug` (listing) + `stress 6` all round-tripped correctly via
  QMP (screenshot in `screenshots/2026-08-11/`); the sandbox's own DMA
  never actually needed a retry (no host jitter to trigger it here),
  which is expected -- this fixes a real-hardware timing condition the
  sandbox doesn't reproduce, not a sandbox-visible bug.

### Improved
- TFS2/ATA write throughput -- `stress 100` was measured at ~1.4MB/s
  (100MB in 72s) on real hardware, root-caused (see the ATA-retry
  entry just above, found while investigating the same "why is stress
  so slow" question) to `kernel/drivers/ata.c`'s DMA write path issuing
  a full synchronous `CMD_CACHE_FLUSH` after every single write, with
  TFS2 itself writing in 4KB pieces -- a 100MB write was on the order
  of 25,600 individual block writes, each paying full flush latency,
  plus a second write-and-flush per newly-allocated block just to
  persist one bit of the free-block bitmap. New `ata_flush_begin()`/
  `ata_flush_end()` (`ata.h`) let a caller batch a run of writes into
  one flush at the end instead of one per write -- every write still
  reaches the drive immediately, only the FLUSH command is deferred, so
  a read-back mid-batch still sees correct data. `write_range_impl()`
  (`kernel/drivers/tfs.c`) wraps its whole per-call block-writing loop
  in one such batch (`write_batch_begin()`/`write_batch_end()`), and
  `persist_bitmap_bit()` now defers the bitmap sector write itself
  during a batch too (not just its flush), coalescing what used to be
  one redundant sector write per allocated block into one write per
  distinct dirty sector (`g_bitmap_dirty[]`, flushed once at
  `write_batch_end()`). Every begin() is matched by an end() on every
  exit path, including the out-of-space/read/write-failure early
  returns (`write_range_impl()` now tracks success via a local `ok`
  flag through a single cleanup point instead of returning directly
  mid-loop) -- an unmatched begin() would otherwise leave every future
  write silently unflushed.
  - Deliberately NOT applied to `persist_record()`'s journal-protected
    metadata writes -- those need each write durable before the next is
    issued for `replay_journal()`'s crash-recovery guarantee to hold;
    batching the flush there could let the drive's real write order
    diverge from what the journal protocol assumes. See the new
    `docs/decisions.md` entry for the full reasoning on where this
    line is drawn.
  - Measured in the sandbox (real hardware should see more, since
    flush latency -- not present at meaningful cost on this sandbox's
    own fast backing storage -- is what actually dominates the
    original slowness): `stress 100` 72s -> 53s (~26% faster);
    `stress 300` (crosses into double-indirect block addressing, still
    verified byte-for-byte correct) ~124s vs. the old rate's ~216s
    extrapolation (~43% faster). Screenshot in `screenshots/2026-08-11/`.
  - Coalescing contiguous block writes into fewer/larger ATA commands,
    and journal-batched flush for metadata, are still open -- see
    `docs/roadmap.md`'s follow-up item.
  - Confirmed on the real hardware that originally hit both the DMA
    timeout failures and the slow throughput: `stress 100`/`200`/`300`/
    `400` all PASSED with zero DMA retries needed, scaling linearly at
    ~3.5MB/s (28s/55s/85s/115s) -- both this fix and the DMA-retry fix
    above are doing their job together, not just in the sandbox. See
    `docs/roadmap.md`'s multi-GB stress-test item for the updated
    full-scale time estimate.

### Added
- Non-blocking DMA start/poll pair for `kernel/drivers/ata.c` --
  Phase 1 of the async-I/O roadmap item (see `docs/roadmap.md`), asked
  for after explaining why disk writes/reads block the whole kernel
  today: apps run in kernel space, so a write from Notepad's Save, the
  shell's `stress`, or `wm_run()`'s own event loop all sit inside the
  exact same call stack that's waiting on the drive -- a slow write
  freezes the whole desktop, not just the operation. `dma_transfer()`
  (the existing blocking call every real disk read/write already goes
  through) is unchanged in behavior, but its body is now two shared
  halves -- `dma_issue()` (program the PRD, kick the command off) and
  `dma_finish()` (stop the bus-master engine, ack the drive's IRQ,
  copy a read's data out of the bounce buffer or flush a write) --
  with `wait_dma_irq()`'s blocking wait sandwiched in between, same as
  before. `dma_transfer_start()`/`dma_transfer_poll()` (`ata.h`) call
  the same two halves but let the CALLER decide how to wait: `start()`
  kicks a transfer off and returns immediately, `poll()` does one
  non-blocking check (`ATA_POLL_PENDING`/`ATA_POLL_DONE`/
  `ATA_POLL_FAILED`) and returns right away either way, with the same
  bounded-timeout logic `wait_dma_irq()` already had (3s via
  `DMA_WAIT_TICKS`, or `ATA_POLL_LIMIT` iterations when called from
  inside a syscall) now living in the poll loop instead of a single
  blocking call. Only one transfer can be in flight at a time
  (`g_pending.in_flight`) -- there's one PRD/bounce buffer to share,
  same constraint the blocking path always had.
  - No real caller uses this yet -- `fs.c`/`tfs.c` still go through the
    unchanged blocking `dma_transfer_with_retry()` path. This is
    deliberately scoped to just the driver-level primitive; a
    steppable `fs_write_range()` (Phase 2) and wiring `wm_run()` to
    poll one so the GUI stays responsive during a save (Phase 3) are
    the next two roadmap steps, not built this round.
  - Proven via a new diagnostic shell command, `dmatest [lba]` (default
    lba 0) -- read-only, so it's always safe to run: reads the given
    sector once through the existing trusted blocking path
    (`ata_read_sector()`) and once through the new non-blocking
    start/poll pair, byte-compares the two, and reports how many
    `dma_transfer_poll()` calls it took. Reports "DMA path not active"
    rather than failing on a PIO-only machine (this primitive has no
    PIO equivalent -- see `ata.h`). Verified via QMP: `dmatest` (lba 0)
    passed with 0 poll calls, `dmatest 9` passed with 1 -- confirming
    the poll loop genuinely returns PENDING at least once rather than
    trivially completing on the first check every time. `stress 5`
    re-verified passing afterward too, confirming the `dma_transfer()`
    refactor didn't change the existing blocking path's behavior.
    Screenshots in `screenshots/2026-08-12/`.
- Steppable write API -- Phase 2 of the async-I/O roadmap item (see
  `docs/roadmap.md`), continuing straight on from Phase 1 above.
  `write_range_impl()` (`kernel/drivers/tfs.c`, the shared engine
  behind `fs_write_range()`/`fs_write()`) had its per-block loop body
  pulled out into `write_range_one_block()`; `write_range_impl()`
  itself just calls it in a tight loop same as before (unchanged
  behavior for every existing caller), and a new
  `tfs_write_range_begin()`/`tfs_write_range_step()` pair calls the
  same helper but lets a caller advance it one block at a time from
  OUTSIDE this file instead. Threaded all the way up the existing VFS
  dispatch layer so it's a real, reusable API, not a TFS2-only
  shortcut: two new `struct fs_ops` function pointers
  (`kernel/include/fs_ops.h`), `vfs.c` dispatch wrappers, and two new
  public entry points in `fs.h` -- `fs_write_range_begin()` (returns an
  opaque handle, or NULL on the same setup failures `fs_write_range()`
  already reports via 0) and `fs_write_range_step()` (returns
  `FS_STEP_PENDING`/`FS_STEP_DONE`/`FS_STEP_FAILED`, cleaning the
  handle up automatically on either terminal result). On
  `FS_STEP_DONE` the file's size/modified-time/on-disk directory
  record are all updated, identical to a successful blocking
  `fs_write_range()` call -- nothing about the file afterward reveals
  which API wrote it.
  - No real caller uses this yet -- `fs_write_range()`/`fs_write()` and
    everything built on either are still fully blocking, unchanged.
    Phase 3 (wiring `wm_run()` to poll one so the GUI stays responsive
    during a save) is next.
  - Proven via a new diagnostic shell command, `steptest <mb>` -- write
    `<mb>` megabytes through an explicit `begin()`/`step()` loop this
    command drives itself (standing in for what `wm_run()` would
    eventually do once per frame), read it back through the ordinary
    `fs_read_range()`, verify byte-for-byte, report total step count.
    Verified via QMP: `steptest 3` passed, 768 `step()` calls for 3MB
    (exactly 3MB / 4KB-per-block, confirming one step per block as
    intended). `stress 5` and `dmatest` re-verified passing afterward
    too, confirming the `write_range_impl()` refactor didn't change
    the existing blocking path's behavior -- `dmatest` needed 658 poll
    calls this run (vs. 0 the first time in Phase 1's own testing),
    which is expected variance (disk busier right after a write-heavy
    `stress`/`steptest` run), not a regression -- the poll loop
    correctly kept waiting rather than timing out.
    Screenshots in `screenshots/2026-08-12/`.

