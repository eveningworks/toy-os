# CLAUDE.md

Guidance for Claude sessions working in this repo. **This file is the
always-loaded context, so it holds RULES -- what stops a session doing
the wrong thing before it knows to look anything up.** The reasoning
behind each rule lives one file away in `docs/`; keep the pointer here
to a line. `README.md` and `apps/README.md` cover the architecture.

## What this is

A hobby x86-64 OS (Multiboot2/GRUB-booted, freestanding C + NASM) with
ring0/ring3 separation, per-process paging, an ELF64 loader, syscalls, a
preemptive scheduler, a ring-3 compositor and window manager, and
disk-backed filesystems (TFS3, plus FAT32 for `/boot`). No
cross-compiler -- host and target are both x86-64, so plain system
`gcc`/`ld`/`nasm` with freestanding flags work.

## Before you edit

- **Pull first.** This checkout is worked on from more than one session.
- **Read in this order**, stopping when you have what you need: this
  file, then **`docs/conventions/INDEX.md`** -- every convention in this
  repo by headline -- then **`docs/conventions/<area>.md` for the area
  you are about to touch**, which carries the bodies, then
  `docs/decisions.md` for "why is it like this" (**its INDEX first**),
  then `docs/roadmap.md` for whether the thing is already known broken.
  Anything drawn adds `docs/gui-guidelines.md`, which is binding.
- **THE CODE WINS OVER A DOC THAT DISAGREES WITH IT.** Believe the code,
  FIX THE DOC in the same change, and say plainly in your response that
  you did -- a silent fix leaves the next session re-deriving the same
  contradiction. Not a licence to skip the docs: they are right far more
  often than not.

## How the user wants to collaborate

- **Offer a few real choices** before something non-trivial -- a data
  layout tradeoff, how far to build this session vs. just planning it.
  A one-line obvious fix doesn't need this.
- **Keep chat compact and terse.** Don't restate what a diff or a build
  log already shows.
- **Before adding a feature to a GUI app, consider whether it should be
  a reusable `userland/ui/` widget** instead of a one-off -- and ask the
  user first either way.
- **Act like an experienced OS/UI designer**, not a generic coding
  assistant: if there's an established better way (a real OS's approach,
  a better data structure, a cleaner API shape), say so.
- **SAY WHAT REAL SYSTEMS DO, before proposing a design.** Name how
  Linux and Windows solve it -- and for the desktop side, Wayland and a
  compositor that implements it (KWin, Mutter, wlroots), or X11 where
  the history explains the shape. Then say whether toy-os should follow
  or deliberately differ, and why.

  By area: kernel and process model -> Linux, Windows NT; filesystem ->
  ext2/3/4, NTFS; compositor and windowing -> Wayland
  (+KWin/Mutter/wlroots/XFCE), X11; settings -> sysctl, dconf/GSettings,
  macOS `defaults`, the Windows registry; init and services -> systemd,
  launchd; IPC -> D-Bus, Binder, Mach ports.

  This has repeatedly changed decisions here rather than decorating them
  -- kernel stacks got a guard page because that is `CONFIG_VMAP_STACK`;
  settings are `(namespace, name)` because sysctl, GSettings and
  `defaults` all namespace; a revoked compositor mapping is poisoned
  because that is what `wl_buffer.release` exists for. Two cautions.
  **Copy the SHAPE, not the size** -- "Linux has 400 syscalls" is not an
  argument for anything. And **check the claim before leaning on it**: a
  wrong premise argued confidently is worse than no comparison at all.
## Conventions worth knowing before editing

The ones below are in full because they fire UNANNOUNCED -- a session
trips them before it knows to look anything up. Everything else is
indexed by headline in the next section.

### Boundaries

- **`kapi.h` is the one header apps include** for kernel capabilities
  (console, keyboard, mouse, timer/RTC, filesystem, graphics). Never
  `#include` a driver header from `apps/`, never call `inb`/`outb` from
  app code. A missing capability belongs behind a new function in
  `kernel/core` or `kernel/drivers`, exposed through `kapi.h`.
- **`userland/wm/wm.h` is a second, peer-level boundary** -- the
  GUI-specific equivalent, included by GUI apps for `window_*` helpers.
  `kapi.h` never includes `wm/wm.h` or `gui_apps.h`.
- **`kernel/include/` is split by audience and the build enforces it** --
  `api/` (what `apps/` may use), `abi/` (the kernel<->userland contract
  `userland/` shares), `kernel/` (internal, NOT on `apps/`'s include
  path, so reaching for one is a compile error). See
  `kernel/include/README.md`, including where a new header starts life.

### The shared toolkit

**There is a toolkit in `kernel/lib/` -- check it before hand-rolling a
digit loop, a formatter, a path join, or a rasteriser.** The headers are
reachable through `kapi.h` and every one has KTESTs: `string.h`,
`knum.h`, `kfmt.h`, `kpath.h`, `fixed.h`, `geom.h`, `rubberband.h`,
`ttf.h`, `krandom.h`, `hid_parse.h`. What bites without warning:

- **`kfmt.h` is one header but TWO files** -- `kfmt.c` is freestanding
  and shared with ring 3, the kernel sinks live in `kfmt_print.c`; a
  kernel include in the former silently takes `snprintf` away from
  userland.
- **`fixed.h`'s angles are in TURNS, not radians**, so `FX_ONE` is a
  full rotation. There is no floating point in this kernel (`-mno-sse`).
- **`geom.h` is deliberately NOT a 3D engine** -- no matrices, faces,
  depth buffer or clipping planes; the face list is the CALLER's. Its
  `geom_rotate3` is yaw then pitch then roll, a fixed order because
  rotations don't commute. The header documents the rest.
- **`krandom.h` is deliberately NOT a CSPRNG, and `krandom_quality()`
  is how a caller finds that out** instead of assuming. The quality enum
  is ORDERED BY TRUST. Read `kernel/lib/stack_protector.c`'s comment
  before moving the boot-time canary randomization.
- **`ttf.h` PARSES UNTRUSTED INPUT, and every read in `ttf.c` is
  bounds-checked for that reason.** It allocates NOTHING -- working
  state is a caller-supplied `struct ttf_scratch`, which is what lets
  one implementation serve ring 0, ring 3 and a test.
- **Draw through the `gfx_draw_line()`/`gfx_draw_circle()`/
  `gfx_fill_ellipse()` wrappers in the kernel and `uui_canvas` in ring
  3**, not `geom_*` directly. `geom.c`/`rubberband.c` are **compiled
  TWICE from one source**, so neither may reference anything
  kernel-only; `geom.h` draws through a **callback**, never into a
  framebuffer.
- **There is no `k_strcpy`: a copy is `k_strlcpy` with the
  DESTINATION's size**, never guessed. The `write_dec`/`write_hex`
  chains beside `*_printf` are **FROZEN per file** by
  `tools/check_chains.py`, a ratchet that only goes down; new number
  formatting is `vga_printf()`/`klog_printf()`.
- **A formatter that doesn't fit its buffer writes NOTHING** rather than
  a truncated value, and **a parser REJECTS rather than guesses**.
  Adding follows this file's usual bar: **a second real caller, not a
  plausible one.**

### Widgets and layout

`docs/conventions/gui.md` carries the reasoning; these fire first.

- **A widget's `ops->hit` is a BOOLEAN, and a widget whose own `_hit()`
  returns a ROW INDEX must convert it.** The router tests
  `!it->ops->hit(...)`, so returning the index makes ROW 0 report "not
  hit". Write `>= 0` -- **UNLESS THE WIDGET HAS A SCROLLBAR**, and then
  answer the whole rect (`uui_hit(x, y, w, h, ...)`), or the bar goes
  dead while the rows work. `tools/check_widget_ops.py` fails the build
  on the conflation.
- **A widget's `natural_size` must not depend on where the widget
  currently IS** -- measuring from the ORIGIN instead of as an extent is
  a feedback loop between layout and measurement.
- **A WIDGET'S OPS TABLE IS THE CONTRACT, AND A MISSING SLOT FAILS
  SILENTLY AND AT A DISTANCE.** No `natural_size`/`set_geometry` means
  never measured or positioned; the router names a widget to its app
  only when it has a `release`. **Fill a new widget's table against
  `uui_widget.h`, never against the widget you copied.** When a layout
  misbehaves, check the ops tables of everything in it before suspecting
  the layout. **AND THE INVERSE HAPPENS -- a slot that is PRESENT and
  read by nobody**; when you add a slot, grep for the code meant to
  consult it.
- **A lone `uui_button` routes its own clicks.** `uui_button_group` is
  worth keeping only for a GRID of them.
- **A DEFAULT MARGIN IS THE WINDOW'S EDGE, NOT EVERY NESTING LEVEL'S.**
  A `uui_layout` inside another container takes NO margin of its own
  unless it names one -- Qt's rule for a sub-layout.
- **A layout CAN grow a child along its stacking axis.** `UUI_FILL_H` in
  a column (and `UUI_FILL_W` in a row) absorbs leftover space.
- **A PAGE THAT CAN OVERFLOW GOES IN A `uui_scrollview`, and the chrome
  stays outside it.** `uui_layout` does not shrink children below their
  natural size -- given too little room it OVERFLOWS, with no scrollbar
  and nothing to say the last children are gone. Keep tabs and status
  bars OUTSIDE; one scroll region per page; a `UUI_FILL` child absorbs a
  SHORTFALL as well as leftover space, and a container with nothing
  stretchable still overflows.

### Anything drawn

**Follows `docs/gui-guidelines.md`**, which is binding. Six things bite
most often:

1. **`gfx_draw_string()` does not clip** -- use
   `gfx_draw_string_clipped()` and `gfx_text_width()` for anything in a
   fixed box (the identical overlap bug has shipped twice).
2. **`on_click` fires on button-DOWN despite its name**, so a control
   that commits there can never be cancelled -- arm in `on_press`, act
   in `on_release`. The menu bar is the documented exception.
3. **Esc closes nothing; Alt+F4 closes a window, and the WM handles
   it.** All three user-facing closes go through one
   `wm_request_close()`, which ASKS a ring-3 client and can be refused
   from `uapp_desc.on_close`. Route a fourth through the same function.
4. **`gfx_set_clip_rect()` with a non-positive w/h sets an EMPTY clip --
   nothing draws -- and only `gfx_clear_clip_rect()` removes a clip.**
5. **Layout is FONT-DERIVED, never in fixed pixels** -- window sizes
   from `default_size()`, chrome from `gfx_char_h()`, the desktop's
   column pitch from `gfx_char_w()`. A hardcoded pixel constant in a
   test tool does not reflow; prefer `DebugConsole.menu_row(label)`.
6. **An app cannot draw outside its own window, and that is enforced.**
   The explicit opt-out is `gfx_clear_clip_rect()`, lasting only for
   that paint.

**Interaction states come from `enum ui_state` / `ui_state_bg()`**,
which derives hover/pressed from the control's own colour: don't
hand-pick tints, and don't assume hover means "lighter" -- on this
near-white theme it has to darken, and `gfx_luminance()` decides.

### The line editor

**THERE IS ONE LINE EDITOR AND IT IS COMPILED TWICE.**
`kernel/lib/klineedit.c` also builds into `libuapp.a`, so `/bin/tosh`
and the ring-3 GUI Terminal edit with the SAME code as the physical
shell; each front end only paints the result. **Don't add an editing key
to one front end** -- add it to the core's keymap and all three gain it.
Five things to know:

- **THE LINE GROWS, AND THE EDITOR TAKES ITS MEMORY FROM THE FRONT
  END.** On the shared-source path the build strips the C library from
  its include path, so klineedit can name neither `kmalloc` nor
  `malloc`; a `struct kline_mem` is passed in (`kline_init_mem`). NULL
  is supported and costs UNDO.
- **A front end that re-inits per line must `kline_free()` first**, or
  it leaks the previous line's buffer and its whole undo stack, every
  line, forever.
- **A BYTE OFF fd 0 IS DECODED, because what crosses a terminal is
  ANSI** -- Up is `ESC [ A`, and `kline_feed()` turns it back into the
  `KEY_*` the keymap switches on. `api/termkey.h` has the keysym-versus-
  terminal split.
- **The console front end repaints with `\r` and TWO passes**, because
  `vga_cursor_move()` is a non-destructive seek ring 3 cannot reach. A
  line longer than the console is wide repaints wrongly -- the TTY
  layer's problem.
- **History is the FRONT END's** (`userland/lib/uhistory.c`), as are Tab
  and Ctrl-R. Tab works in both rings (`kernel/lib/completion.c`);
  Ctrl-R still does nothing in ring 3. **The shared CASE TABLE is what
  checks the second build** (`kernel/include/api/klineedit_cases.h`):
  add a case once, both rings assert it.

### The filesystem

- **THE FILESYSTEM IS NOT RE-ENTRANT, and each MOUNT has ONE SLEEPING
  LOCK because of it.** A backend walks through per-mount scratch, so
  `FS_OP()` takes that mount's `lock` (a recursive `kmutex` in `struct
  mount`) around every backend call -- and its holder may SLEEP in a
  disk wait. So **never take it, or any kmutex, with the preemption
  guard raised or with interrupts off and no scheduler slot**: behind a
  sleeping holder that spins forever. `kmutex_lock()` logs `taken from
  atomic context` on entry when it happens. **The lock order is PARENT
  MOUNT BEFORE CHILD** (an `fs_list()` callback on `/` may stat `/boot`,
  never the reverse). A stretch longer than one fs call that must keep
  the disk quiet uses `fs_exclusive_begin()`/`_end()` (EVERY mount's
  lock, in that order), never the preemption guard -- and anything that
  reaches the DEVICE for a mount (a flush) does so under that mount's
  lock, or exclusion stops promising a quiet disk. The exceptions are
  a data read and an in-place OVERWRITE, which DROP the lock through
  `mount_io_begin()` -- a block free or exclusion waits for them
  (`mount_io_drain()`), and an allocating write must never drop it.
  Inside tfs3, an op locks every inode it touches BEFORE changing
  anything (`t3_lock()`; a 0 means return now, FS_OP re-runs it), never
  unlocks by hand, and never waits holding a lock. It does NOT make an
  `fs_list()` callback safe to call `fs_*` on the same mount (that is
  recursion). Finer locking inside a volume: `docs/fslock-design.md`.
- **THERE IS NO `fs_read()`. A whole-file read goes into memory the
  caller owns: `fs_read_into(path, buf, cap)`**, which REFUSES an
  oversized file rather than truncating; a file that may be large is
  `kmalloc`'d at `fs_size()` by its caller or streamed with
  `fs_read_range()`. The same shape exists for config files:
  `etc_config_load()` + `etc_config_buf_get()` read once and answer many
  keys, because `etc_config_get()` re-reads the whole file PER KEY.

### The static checks that fail the build

Each waives with a named comment, and the reason is the mechanism, not
the waiver. `docs/tools.md` has what each one looks for.

| Check | Fails on | Waiver |
|---|---|---|
| `check_widget_ops.py` | a widget ops slot it needs left NULL (`draw` needs `natural_size`+`set_geometry`; `set_geometry` needs `bounds`; `press` needs `release`; `key` needs `accepts_focus`; a scrollbar must not route `hit` through its row hit) | `widget-ops-ok:` |
| `check_key_routing.py` | the inverse -- a filled slot nobody can reach: an app naming a `.key` widget without `.focus` or `.on_key`, or declaring `.widgets` while hand-routing its menu bar | `key-routing-ok:` |
| `check_drivers.py` | a `.c` under `kernel/drivers/` with no `DRIVER_DECLARE`. It is FILE-SCOPE DATA, not a call inside `init()` | `driver-none:` |
| `check_dispatch.py` | a dispatch chain over ~20 branches that should be a table (this project already has `display_driver`, `block_device`, `clocksource`, `syscall_table.c`) | `dispatch-ok:` |
| `check_layout.py` | a staged file with no tracked source behind it | -- |

### Files, comments and history

- **Split a file once it's grown genuinely harder to work with** -- not
  preemptively. The signal is practical: more than one real concern, or
  long enough that finding the right part gets slow. **Don't calibrate
  against a line count quoted in a doc**; run `wc -l`. Excludes
  generated files like `kernel/drivers/font_ttf.c`. Follow the
  `userland/wm/` pattern (split by concern, share state through an
  `_internal.h`) and record the split's reasoning in a top-of-file
  comment.
- **PREFER FACTS THAT CANNOT GO STALE. Do not cite a number that some
  other file has to keep true.** What IS safe to point at: a named file
  or symbol; a named section in `docs/decisions.md`; a named milestone
  in `docs/roadmap.md`; a named rule here or in
  `.claude/skills/toy-os-feature-workflow/`. All are addressed by TITLE.
  Name the THING, not its index. **For the PRESENT point at a title; for
  the PAST point at a COMMIT** -- a short SHA is the one number that
  cannot go stale, paired with what it did ("the poison-page fix
  (978ebf7)"). This is not a rule against cross-references, but against
  ones whose correctness depends on someone updating a third file.
- **A comment's length should track how SURPRISING the code is and how
  dangerous it is to change** -- not how much history it accumulated.
  Two things earn length: **the invariant** and **the trap**. Four
  questions: would the sentence be true even if nobody had got it wrong
  first? Does `docs/decisions.md` already say it? Is a real system being
  NAMED (a clause) or used to JUSTIFY (a paragraph, and that belongs in
  `docs/decisions.md`)? And more than ~6 lines on a struct field or a
  small static function is a smell unless it is a genuine trap. **Cap
  the anecdote at one clause.** Existing long comments are deliberately
  not being retro-trimmed.
- **THERE IS NO CHANGELOG.** Where the five kinds of thing go:

  | Thing | Home |
  |---|---|
  | What changed, file by file | the COMMIT MESSAGE; `git log` is the record |
  | How the mechanism works, and its trap | a comment next to the code |
  | Why this way and not the obvious way | `docs/decisions.md`, written IN FULL there |
  | What is BROKEN | `docs/bugs.md`, one line each, repro in `docs/roadmap-details.md`. A fixed bug is DELETED, not struck through. Say the RATE for anything intermittent, and say plainly when a cause was never established. PRE-EXISTING means MEASURED |
  | What is NOT BUILT YET | `docs/roadmap.md`. Its "Known limitations" section is for things that work as designed; anything misbehaving is a bug |

  Commit messages before 2026-08-15 are one-liners, so for that work the
  deleted changelog was the only detailed account of a CHANGE.
- **A COMMIT MESSAGE IS PROBLEM, THEN CHANGE, THEN FILES -- not an
  essay.** Imperative subject under ~72 chars, prefixed with the area
  (`settings:`, `wm:`, `kernel:`); one or two short paragraphs on what
  was wrong and what caused it; a bullet per change; then every changed
  file with a one-line note. **No capitalised lede sentences, no war
  stories, no forensics** -- the reasoning has three better homes.
  Bodies before 2026-08-24 are in the old essay voice and are
  deliberately not rewritten.

### Traps that only bite while testing

These live here rather than in `docs/testing.md` because each one makes
a broken thing look fine.

- **A PROBE THAT OUTRUNS THE LOG DESTROYS THE EVIDENCE IT GATHERS, AND A
  RATE-LIMITED PROBE LOOKS EXACTLY LIKE A LOOP THAT STOPPED.** The klog
  ring holds a few hundred lines. Keep instrumentation under a line a
  second, give EVERY probe in a comparison the SAME limiter, and use a
  file-backed serial log for anything verbose.
- **Three ways a GUI test passes without testing anything:** (1) **"it
  responds" is not "it is drawn"** -- the ring-3 Calculator shipped with
  no visible buttons because every check asserted a click changed the
  display, which it did; (2) **moving identical content is
  pixel-identical**, so number the rows; (3) **a test must not assume
  the thing it is testing**. The general form: ask what a broken version
  would still pass.
- **A positive control can turn nothing red because the test's DATA
  never reached the code under test.** When a control fires nothing,
  suspect the fixture before the harness, and ask what input size or
  shape actually reaches the branch.
- **A screendump compared against another screendump must be a SETTLED
  frame** -- `QMPSession.screenshot()` IS one by default; `stable=False`
  is the opt-out. **And so must one compared against ITSELF.** A client
  that has drawn into its buffer has not necessarily been composited.
  The sibling trap: **a poll whose exit condition is weaker than what
  the code after it needs is a flake**. **AND SETTLED IS NOT CAUGHT UP:
  a frame is evidence about the FRAMEBUFFER, never about the machine** --
  ask the serial console BEFORE writing "hung", and wait on what it says
  rather than on pixels.
- **Verify GUI changes by reading PIXEL VALUES, not by looking at the
  screenshot** (`tools/pixel_probe.py`). **Always sample a control that
  should NOT have changed as well** -- half the assertion is the
  neighbour staying put.
- **A HOVER STATE CANNOT BE TESTED WITH `gui move`, and the tool for it
  is `DebugConsole.hover_frames()`.** An injected pointer position
  overrides the mouse for ONE `wm_run()` iteration and then snaps back
  -- right for a click, useless for a state that must survive a capture.
  **Assert the BAND, not the change**, and remember SELECTION OUTRANKS
  HOVER.
- **A USB AUDIO RESULT SAYS WHICH PATH IT EXERCISED, OR IT SAYS
  NOTHING.** One device binds DIFFERENTLY per speed -- a real G6 is
  UAC2 at high speed (8000 packets/s, rate negotiated through a Clock
  Source) and UAC1 at full speed (1000/s) -- and only a physical
  replug reaches high speed. **Read the `UAC1`/`UAC2` and the cadence
  out of the bind line before comparing two runs**, and do not read a
  full-speed result as a high-speed one. **AND TWO PLAYBACKS BACK TO
  BACK ARE ONE STREAM**: soundd keeps the card for 2 s of idle, so a
  restart fault only shows with a gap longer than that. The driver's
  `usbaudio: start`/`stop` lines (stale groups, dry count) are the
  evidence; `docs/bugs.md` has what is open.
- **`seed/sync/` KEEPS WHAT YOU DELETE FROM `data/`, and `make
  clean-disk` does not touch it.** The pair is `make clean` (wipes
  staging) THEN `make clean-disk` (wipes the image); either alone leaves
  the old file in place.
- **NEVER WRITE A HAND-AUTHORED FILE INTO `seed/sync/` -- IT IS BUILD
  STAGING, AND THE SOURCE IS `data/`.** It is gitignored and `make
  clean` deletes it, so a file written there ships ABSENT with nothing
  failing. A Start-menu entry goes in `data/wm/applications/`, an icon
  in `tools/gen_icons.py` -> `data/icons/`. After adding a data file,
  confirm `git status` shows it.
## Conventions indexed in `docs/conventions/INDEX.md`

**Everything above is what fires UNANNOUNCED. The other ~436 conventions
are indexed by headline in `docs/conventions/INDEX.md`, one hop away** --
`kernel.md`, `gui.md`, `storage.md`, `shell.md` and `build.md` carry the
bodies, and the index names every rule in all five.

**Read the index before editing an area**, ahead of the area file: skim
for a headline that tells you something you did not know, then open the
area file for its body. It lived here until it was 41 KB of always-loaded
context, which is carried rather than read; moving it deleted no rule.

**A new convention goes in `docs/conventions/<area>.md` AND gets its
headline added to that index.** Half of that pairing is not optional.

## This checkout, and the repo it pushes to

**The repo is `eveningworks/toy-os`** -- an ORGANIZATION since
2026-08-16, because a personal repo has no read-only collaborator role;
see `docs/decisions.md`. The maintainer's account was renamed in the
same stretch and the old handle scrubbed by a history rewrite. So: the
remote is `git@github.com:eveningworks/toy-os.git`, nothing in the tree
should name a personal account (`grep -rn` before believing a doc), and
**run `tools/backup_repo.sh` before any further change to the repo's
identity or history**.

- Git identity is already configured (`toy-os` /
  `noreply@toy-os.local`), so plain `git commit` works. **It is
  PER-REPOSITORY and a clone does not carry it.** `preflight.sh` refuses
  to run until SOME local identity is set. **That identity is the
  standing privacy convention, not a default to override** -- never let
  a commit here carry the maintainer's real name or personal email.
  `docs/development-setup.md` has the command sequence.
- `git push origin main` and `gh release create`/`upload` work from the
  session. Push ordinary verified work without asking; confirm tags,
  Releases, force-pushes and history rewrites first, since those are
  one-way.

## Building

**`docs/testing.md` is the full testing reference** and `docs/tools.md`
covers every script in `tools/`. This section holds only the targets and
the traps that fire before you know to look anything up.

```
make all    # kernel.bin + userland test ELFs
make iso    # + toy-os.iso; ALSO seeds disk.img and installs GRUB + the
            # kernel on it -- run before ANY headless test
make test   # boot headless, run the in-kernel test suite
make verify # full pre-delivery gate (= tools/preflight.sh)
make run    # QEMU with an SDL window (the user's machine, not headless)
make live-iso / usb-image / debug
```

**THERE IS ONE RUN TARGET, AND EVERY WAY TO BOOT IS A VARIABLE ON IT --
do not add a target for a new combination.** It was twelve near-identical
QEMU lines that grew by MULTIPLICATION; the aliases are gone for the same
reason. **Check a change with `make -n run <FLAGS>`.**

```
make run KVM=1 VIRTIO=1 AUDIO=1 NOGRAPHIC=1 MENU=1 MEM=512
make run DISK=virtio VGA=virtio INPUT=virtio   # what VIRTIO=1 expands to
make run WINDOW=full | fit    # sdl cannot scale; fit is the only one that
                              # helps when the guest mode is BIGGER, and the
                              # only one that blurs the font
make run LIVE=1 | BOOT=cd | BOOT=disk
make all STRIP=0 / make live-iso COMPRESS=0
```

- **`VIRTIO=1` MEANS EVERY DEVICE CLASS -- disk, GPU and input.** It used
  to mean the disk alone, so `make run KVM=1 VIRTIO=1` never exercised
  the virtio-gpu driver. A per-class value overrides it.
- **`BOOT` IS DERIVED BY DEFAULT**: `make run` boots the DISK, but only
  if `disk.img` actually carries GRUB and the kernel.
- Three traps if you touch the Makefile: **`LIVE` changes the
  PREREQUISITE as well as the command line**; **every definition is
  DEFERRED (`=`, never `:=`) and uses `$(if ...)` rather than `ifeq`**,
  since `ifeq` is evaluated once at parse time; and **`MENU=1` works by
  deriving `GRUB_TIMEOUT`**, baked into both media at build time.

**`EXTRAS=1` IS THE ONLY THING THAT MAKES A BUILD REACH THE NETWORK, AND
AN EXTRAS IMAGE IS NOT YOURS TO PUBLISH.** It fetches
differently-licensed material after showing its licence (`LICENSE=agree`
answers for CI; a build with no terminal is REFUSED, not prompted).
Fetching is not distributing -- publishing the resulting ISO is, which is
why such an image carries `/usr/share/licenses/extras.txt`.

**Boot flags can be baked into the media**: `make iso
KCMDLINE="video=1920x1080 nokaslr"`. `docs/boot-flags.md` lists every
word. GRUB's `e` editor shows the menuentry BODY only, so the boot-word
summary is repeated inside each `menuentry`. **`video=<W>x<H>` only does
something on a MODESETTING driver** -- on a plain VESA framebuffer GRUB
has already fixed the mode and the flag is inert.

**Source discovery is recursive** -- every `.c` under `kernel/` or
`apps/` is compiled, so a new directory needs no Makefile edit. The flip
side: there is no scratch file the build ignores.

- **A plain `make all` is safe after editing a shared header**
  (`-MMD`/`-MP`, proven live by `tools/check_deps.py`). **What is NOT
  tracked is a CFLAGS change** -- `make clean` after one, and treat a
  compiler flag that appears to work only partially as a stale-object
  symptom first.
- **`make all` REACHES NO BOOT MEDIUM.** It writes `build/kernel.bin`
  and stops; the kernel reaches `disk.img` in the `seed` step `make iso`
  runs. A `make all` without `make iso` leaves the whole suite testing
  the PREVIOUS build, and it fails as a clean PASS. `tools/iso_guard.py`
  refuses a stale one now.
- **Every automated test here runs TCG, so a green suite says nothing
  about two bug classes** -- anything depending on how FAST the emulated
  hardware is, and anything depending on guest MEMORY TYPES (TCG ignores
  PAT). Both have shipped real bugs. Try `tools/vm.py --kvm` before
  concluding a hardware-only report is untestable; `tools/kvm_soak.py`
  is the standing check.
- **Test against a COPY of `disk.img` if the user might have QEMU open**
  -- `cp --reflink=auto --sparse=always` (it is ~4 MB of data in a 9 GB
  sparse file). **A COPY goes stale the moment you rebuild**: a copy is
  a BOOT MEDIUM, so a stale one runs an entire earlier build and reads
  exactly like a bug in the app.
- **A copy is not enough for `make iso`/`make verify`/`preflight.sh` --
  ASK the user to close their QEMU first** (standing request). Those
  three re-seed the real `disk.img` regardless of what a test is pointed
  at. Check `ps aux | grep qemu-system` BEFORE the gate.
- **BEFORE BELIEVING ANY GUI TEST FAILURE, RE-RUN IT ON A FRESH IMAGE.**
  `make iso` re-seeds by SYNC, never reformat, and several tools' apps
  WRITE. So `make clean-disk && make iso`, then re-run with `--logs
  DIR`. If it still fails, prove it is not yours by rebuilding `HEAD`.
  NOT a universal explanation -- a failure surviving `clean-disk` may be
  a genuine pre-existing flake.
- **AND A TEST THAT APPLIES A SETTING CHANGES THE MACHINE FOR EVERY
  LATER TOOL.** `settings_test` leaves `mouse_speed` and `mouse_accel`
  on disk; a faster pointer then made two unrelated tools fail as "hover
  does nothing". The write does not have to be in the failing tool, or
  in the same run.
- **`strace <binary>` is often the fastest way to see what a `/bin`
  binary is doing** -- one decoded line per syscall, and the same lines
  land in `dmesg`. Reach for it before adding temporary `klog_write()`
  calls. It is a `/bin` PROGRAM, so at a `#` prompt it needs `spawn`.
- **READ THE ABI COMMENT OF ANY CALL YOU SWAP IN.** The most expensive
  mistake of the ring-3 GUI migration was replacing a non-blocking
  `scheduler_poll()` with `sys_waitpid()` while `SYS_WAITPID`'s own
  first line said **BLOCKS**. A port is exactly where this happens.
- **AND A MECHANISM THAT EXPLAINS THE SYMPTOMS IS NOT THE MECHANISM THAT
  CAUSED THEM.** Before publishing a root cause, do the cheap disproving
  check, and prefer a discriminating experiment over a plausible story.

## Debugging with GDB

`make debug` boots frozen at CPU reset (`-s -S`) for real
breakpoint/single-step debugging via QEMU's own GDB stub -- **no
kernel-side GDB protocol code needed**. `docs/decisions.md` says why an
in-kernel serial stub was deliberately not built.

```
gdb build/kernel.bin -ex "target remote localhost:1234"
```

`CFLAGS`/`USERLAND_CFLAGS` both carry `-g`, so `kernel.bin` and every
userland ELF have real DWARF. Kept at `-O2` deliberately -- same binary
as every other build, so some locals show as "optimized out".

## Testing in QEMU headlessly, via QMP

**First: is this actually a GUI change?** If not, `tools/vm.py` is
faster and gives text you can assert on. Order of cheapness:
`boot_smoke_test.py` -> `make test` / `vm.py exec` -> QMP.

**`docs/testing.md` is the full reference** for the `QMPSession`/
`GuiFlow` API and the dozen gotchas already handled in
`tools/qmp_test.py`. Six things worth knowing without opening it:

- **Use `tools/qmp_test.py`, don't rederive it.** Add to the module when
  it lacks something rather than writing a one-off.
- **Never `pkill -f qemu-system-x86_64`.** It cannot tell your headless
  launch from the user's interactive window. Kill only the PID your own
  launch wrote to its `-pidfile`.
- **TWO GUESTS ON ONE QMP PORT DO NOT FAIL AS A PORT CLASH.** Everything
  defaults to 4445, and the error surfaces MINUTES LATER as a
  `BrokenPipeError` in whichever tool was mid-command -- never the one
  that caused it. `tools/port_guard.py` refuses at the launch.
  **Every tool takes `--instance N`**, which derives the QMP port AND
  the serial socket from one number. **`auto` belongs to whatever
  LAUNCHES the guest, and only there** -- a tool connecting to a running
  guest must name the slot it is ON. **Name a slot whenever anything
  else might be running** (`gui_regress.py` holds `0..DEFAULT_JOBS-1`).
  What it does NOT fix is CPU contention: a concurrent run is fine for
  getting an answer, and not evidence when the suite is being judged.
- **DON'T ADD A WAIT LOOP FOR WORK THAT IS ALREADY IN THE BACKGROUND.**
  A backgrounded command's own completion notification IS the signal, so
  a waiter beside it is redundant even when it works -- twelve were left
  running in one session, several unable to exit at all.

  **IF A WAIT IS GENUINELY NEEDED, WAIT ON A PID OR AN ARTIFACT, AND
  BOUND IT** -- `tools/wait_for.sh <pid>`, `--file PATH`, both with a
  timeout. Never match a process by NAME: the waiter's own command line
  contains the name it is looking for, so the loop matches ITSELF and can
  never exit, reporting a finished suite as still RUNNING. The `[f]oo`
  trick does not save it -- that hides the grep, not the shell whose
  command line carries the word somewhere else. Two such waiters ran for
  two hours in one session while `preflight.sh` reported none, because
  its pattern looked for `while [` and these were `while ps ... | grep`.
- **Prefer `tools/gui_debug.py` to pixels** for anything not literally
  about rendering. It is asynchronous: call `DebugConsole.settle()`
  before asserting, never a fixed sleep.
- **Screenshots are a TESTING TOOL, not a deliverable.** Take as many as
  a check needs, into a scratch directory; do NOT save them into
  `screenshots/`. Show the user one when SEEING it is the answer.
## tools/

Dev/build helper scripts, not shipped as part of the OS. **`docs/tools.md`
is the full reference** -- what each one does, why it exists, and the
traps it encodes. **This is only the index: which tool answers which
question, and nothing about how.** Read that file before reaching for
anything here you have not used recently, and add to it (not to a
one-off script) when something would save a future session real time.
The bar is "does this fix a rederive-from-scratch cost".

| Question | Tool |
|---|---|
| Is it MINE, or already broken? | `predates.py "<command>"` -- "it predates me" is a MEASUREMENT |
| Is it safe to commit? | `preflight.sh` (**stop your `vm.py` guest first**) |
| Has the on-demand half rotted? | `ondemand_sweep.py` -- the ~30 tools no other runner covers |
| Drive the BARE-METAL machine | `remote.py` (`exec`/`put`/`get`/`sync`/`flash`/`screenshot`/`shell`) |
| Run the GUI TOOLS on it | `gui_regress.py --host <ip>`, over `remote_gui.py` |
| Drive a VM | `vm.py` (text in, text out), `qmp_test.py`, `gui_debug.py`, `gui_flow.py`, `shell_flow.py`, `serial_console.py`, `serial_capture.py`, `watch_vm.sh`, `run_release.sh` |
| Is it INTERMITTENT, and at what rate? | `boot_rate.py` (bare metal), `flake_hunt.py` (VM) |
| Test runners | `boot_smoke_test.py`, `ktest_run.py`, `usertest_run.py`, `faulttest_run.py`, `gui_regress.py`, `damage_sweep.py`, `damage_hunt.py` |
| Diagnose | `panic_resolve.py` (**never hand-roll `nm`**), `acpi_dump.py`, `aml_walk.py`, `QMPSession.hmp()` (**the one oracle the guest cannot fake**), `corrupt_diff.py`, `window_resize_probe.py`, `pixel_probe.py`, `screenshot_diff.py`, `iso_guard.py` |
| Does it actually SOUND right? | `audio_loopback_test.py` -- records the G6 back on line in; needs the cable patched in |
| Check an implementation against a FOREIGN one | `libc_diff.py`, `uimg_codec_hostcheck.py`, `usnd_hostcheck.py`, `hash_hostcheck.py`, `divti3_hostcheck.py`, `regex_hostcheck.py`, `umd_hostcheck.py`, `ugfx_text_hostcheck.py`, `utext_hostcheck.py`, `utween_hostcheck.py`, `term_scheme_hostcheck.py` |
| Measure | `idle_cpu.py` (quote DIFFERENCES only), `timer_bench.py` (a timer configuration, idle and under load), `loc.py`, `dup_scan.py` (copy-paste; a REPORT, never a gate), `ping_rtt.py`, `latency_under_io.py`, `fs_isolation.py`, `frame_balance.py` |
| Disk images, from the host | `seed_disk.py`, `install_grub.py` (also `boot_medium()`), `tfs3_writer.py`, `mkpart_test.py`, `fetch_wad.py` |
| Generated data | `gen_version.sh`/`set_version.sh`, `gen_kconfig.sh`, `genttf.py`, `gen_kbs.py`, `gen_cursors.py`, `gen_icons.py`, `gen_imgdata.py`, `gen_audio.py`, `gen_music.py`, `gen_mp3_tables.py`, `gen_signames.py`, `genrelocs.py`, `gen_syms.py`, `drivers_conf.py`, `gen_modalias.py`, `gen_decisions_index.py`, `gen_commands_index.py`, `gen_next_up.py`, `fetch_ca_bundle.py` |
| The repo itself | `backup_repo.sh` -- run before ANY change to the repo's identity or history |
| Wait for something, safely | `wait_for.sh` -- a PID or a file, never a process NAME, always bounded |

**Static checks**, run by `preflight.sh` or beside it: `check_deps.py`,
`check_layout.py`, `check_dispatch.py`, `check_syscalls.py`,
`check_widget_ops.py`, `check_text_measure.py`, `check_key_routing.py`,
`check_drivers.py`, `check_initcalls.py`, `check_copy_user.py`,
`check_chains.py`, `check_docs.py`, `check_licenses.py`,
`check_config_size.py`, `check_tool_coverage.py`,
`check_tool_commands.py`, `tfs3_writer_test.py`.

**Every tool, by the runner that names it** -- generated from the
runners themselves; `--list` on either runner is the live answer.

*Run by `preflight.sh` (the gate):*

`boot_smoke_test.py`, `check_chains.py`, `check_config_size.py`,
`check_copy_user.py`, `check_deps.py`, `check_dispatch.py`,
`check_docs.py`, `check_drivers.py`, `check_initcalls.py`,
`check_key_routing.py`, `check_layout.py`, `check_licenses.py`,
`check_syscalls.py`, `check_text_measure.py`, `check_tool_commands.py`,
`check_tool_coverage.py`, `check_widget_ops.py`, `genttf.py`,
`iso_guard.py`, `tfs3_writer_test.py`, `usertest_run.py`

*Run by `gui_regress.py`:*

`animation_test.py`, `blank_window_test.py`, `brightness_test.py`,
`calculator_client_test.py`,
`calendar_test.py`, `clipboard_test.py`, `compositor_death_test.py`,
`compositor_test.py`, `crashtest_test.py`, `cursor_theme_test.py`,
`desktop_entries_test.py`,
`dialog_test.py`, `filedialog_test.py`, `filemanager_test.py`,
`forcequit_test.py`, `fullscreen_test.py`,
`gfxdemo_test.py`, `hover_test.py`, `icons_test.py`,
`idle_desktop_test.py`, `imgview_test.py`, `keyup_test.py`, `kvm_soak.py`,
`menubar_test.py`, `mines_test.py`, `modeset_test.py`,
`mouse_buttons_test.py`,
`network_tray_test.py`, `notepad_client_test.py`, `osk_test.py`,
`pager_test.py`, `player_test.py`, `popup_test.py`,
`resize_edges_test.py`, `resize_stride_test.py`, `sched_gui_test.py`,
`screen_surface_test.py`, `screensaver_test.py`, `screenshot_test.py`,
`scrollbar_test.py`,
`settings_test.py`, `shadow_test.py`, `shortcut_test.py`,
`single_instance_test.py`, `smooth_scroll_test.py`, `start_menu_test.py`,
`taskmgr_test.py`, `thumbcache_test.py`, `tray_press_test.py`,
`uapp_test.py`, `uiclient_test.py`, `uidemo_test.py`, `uterm_test.py`,
`vm.py`, `volume_test.py`, `wallpaper_mode_test.py`, `winclient_test.py`,
`window_geometry_test.py`

*Run by `ondemand_sweep.py` (on demand, never a gate):*

`ahci_test.py`, `ansi_cursor_test.py`, `audio_loopback_test.py`,
`audio_test.py`, `boot_rate.py`, `console_bleed_test.py`,
`console_shell_test.py`, `ctrlc_test.py`,
`devclaim_test.py`, `hdacodec_test.py`, `mixer_test.py`,
`cursor_ibeam_test.py`, `dash_gap.py`, `dash_test.py`, `diskmark_test.py`,
`divti3_hostcheck.py`, `doc_test.py`, `doom_sound_test.py`,
`dup_scan.py`,
`doom_test.py`, `fat32_test.py`, `fetch_wad.py`,
`filemanager_harness_hostcheck.py`, `fileop_test.py`, `flake_hunt.py`,
`font_test.py`, `fs_switch_test.py`, `grep_test.py`,
`gui_regress.py`, `guictl_test.py`,
`hash_hostcheck.py`, `hid_parse_hostcheck.py`, `highmem_consume.py`, `highmem_test.py`,
`hires_test.py`, `https_test.py`, `hwdata_test.py`, `init_test.py`,
`install_test.py`, `jobs_test.py`, `kbd_test.py`,
`keyboard_paths_test.py`, `ktest_run.py`, `latency_under_io.py`, `fs_isolation.py`,
`libc_diff.py`, `live_boot_test.py`, `logrotate_test.py`, `ls_test.py`, `mkpart_test.py`,
`module_test.py`, `msi_test.py`, `multidisk_test.py`, `net_test.py`,
`netheal_test.py`, `ntp_test.py`, `nvme_test.py`, `partition_test.py`, `ping_rtt.py`, `pixel_probe.py`,
`poweroff_test.py`, `predates.py`, `preflight.sh`, `qemu_matrix.py`,
`remote.py`, `remote_test.py`, `sector4k_test.py`, `serial_backpressure_test.py`,
`settings_harness_hostcheck.py`, `shutdown_sync_test.py`,
`soundd_test.py`, `stdin_test.py`,
`sum_test.py`, `taskbar_test.py`, `term_scheme_hostcheck.py`,
`terminal_probe.py`, `tfs3_v1_test.py`, `ugfx_text_hostcheck.py`,
`uimg_codec_hostcheck.py`, `uimg_hostcheck.py`, `umd_hostcheck.py`,
`usb_audio_test.py`, `usb_test.py`, `usnd_hostcheck.py`,
`utext_hostcheck.py`, `utween_hostcheck.py`, `virtio_boot_test.py`,
`virtio_gpu_test.py`, `virtio_input_test.py`

*Libraries, generators and drivers -- named by no runner:*

`acpi_dump.py`, `aml_walk.py`, `backup_repo.sh`, `corrupt_diff.py`,
`drivers_conf.py`, `faulttest_run.py`, `fetch_ca_bundle.py`,
`fetch_extras.py`, `frame_balance.py`, `gen_audio.py`,
`gen_commands_index.py`, `gen_cursors.py`, `gen_decisions_index.py`,
`gen_icons.py`, `gen_imgdata.py`, `gen_kbs.py`, `gen_modalias.py`,
`gen_mp3_tables.py`, `gen_music.py`, `gen_next_up.py`, `gen_signames.py`,
`gen_kconfig.sh`, `gen_syms.py`, `gen_version.sh`, `genrelocs.py`,
`gui_debug.py`, `gui_flow.py`, `idle_cpu.py`, `install_grub.py`, `loc.py`,
`remote_gui.py`,
`mem_stress.py`, `panic_resolve.py`, `port_guard.py`, `qmp_test.py`,
`regex_hostcheck.py`, `run_release.sh`, `screenshot_diff.py`,
`seed_disk.py`, `serial_capture.py`, `serial_console.py`,
`set_version.sh`, `shell_flow.py`, `tfs3_writer.py`, `timer_bench.py`, `wait_for.sh`,
`watch_vm.sh`, `window_resize_probe.py`

**HOST TOOLS THIS REPO EXPECTS, none required to build it** --
`docs/tools.md` has the full entry for each. Two worth knowing first:

- **`bear -- make all` regenerates `compile_commands.json`, and that is
  what makes `clangd` work here.** Run it from a `make clean`. Reach for
  the LSP rather than grepping for a signature: `userland/ui/` is a
  couple of dozen widgets whose ops tables are easy to guess wrong.
- **`ruff check tools/` and `shellcheck tools/*.sh`** before touching a
  harness. `ruff.toml` pins a narrow ruleset (`F` + `E9`) on purpose --
  the default buries the class that matters, which is a harness bug
  reporting a healthy system as broken. NEITHER is in `preflight.sh`:
  the gate must not start requiring a tool a checkout may not have.

`ccache` is wired in (`CC = $(CCACHE) gcc`, falling back to plain gcc),
so a checkout without it is unaffected. It matters on the GATE, which
always starts with `make clean` -- measured 2.37s -> 0.40s.

**WHAT THE USER SAYS, AND WHAT IT MEANS.** Three checks, easy to confuse
because two mention QEMU:

| Phrasing | Means |
|---|---|
| "run the matrix" / "check the old QEMUs" | `tools/qemu_matrix.py` -- LOCAL, Docker, ~15s per version. Answers "does this depend on the host's QEMU?" |
| "run CI" / "kick off Actions" | `gh workflow run build.yml`. REMOTE, ~90s. Answers "does it build from a clean clone on someone else's machine?" |
| "run preflight" / "verify" / "run the tests" | `bash tools/preflight.sh` -- the ordinary per-change gate |

If a request is ambiguous between the matrix and GitHub, ask -- they
answer different questions.

- **`qemu_matrix.py` RUNS AT A RELEASE AND WHEN THE USER ASKS**, not
  automatically (standing instruction). **OFFER it** when a change
  plausibly depends on the host's QEMU or CPU: a driver, a poll loop, a
  timeout, a clocksource, DMA. **A finding is a REPORT, not a blocker** --
  an older QEMU disagreeing can be an emulator quirk, and which it is
  is the user's call.
- **GITHUB CI NO LONGER RUNS ON EVERY PUSH** (2026-08-19). It runs on a
  release tag and on demand. What it is uniquely good for is a
  CLEAN-CHECKOUT build on somebody else's machine. What it was bad at
  was being a per-push gate, and a gate that cries wolf gets ignored.
- **`gui_regress.py` is the standard check** after touching `userland/`
  or anything the WM draws -- always with `--logs DIR`. Its wall clock
  is bounded by the SLOWEST SINGLE TOOL and by the sum over the job
  count, whichever is larger, so look at the maximum. The per-tool
  timeout is a HANG GUARD sized against that maximum, not a budget.
  `DEFAULT_JOBS` is `min(12, cores//2)`.
- **`damage_sweep.py` and `damage_hunt.py` are named by NO runner** --
  deliberately, with the reason in `ondemand_sweep.py`'s exclusions
  list. Run them by hand after touching anything that draws or damages.
  **`damage_sweep.py` NEEDS A GUEST WITH SOUND HARDWARE to cover the
  tray's volume panel** (`vm.py --audio-wav <path> --audio both start`);
  without one it reports the panel's resize steps as NOT COVERED rather
  than passing quietly.
- **A clean `damage_sweep.py` proves nothing until `--positive-control`
  has shown the harness can fail.** Same for any positive control here.
- **`iso_guard.py` refuses a stale `toy-os.iso`**, because `make all`
  without `make iso` otherwise leaves the suite testing the previous
  build and reporting a clean PASS. `TOYOS_ALLOW_STALE_ISO=1` bypasses
  it deliberately.
## docs/

**This file holds RULES; reference material lives in `docs/` behind a
one-line pointer.** Add detail there, and keep the pointer here to a
line.

| File | What it is |
|---|---|
| `docs/testing.md` | how to run and drive this OS, the QMP mechanics, what the emulator does and does not model |
| `docs/tools.md` | the full reference for every script in `tools/` |
| `docs/conventions/` | the BODY of every convention this file indexes by headline: `kernel.md`, `gui.md`, `storage.md`, `shell.md`, `build.md` |
| `docs/decisions.md` | the INDEX over `docs/decisions/` -- "why does toy-os work this way?", split by area |
| `docs/gui-guidelines.md` | how the GUI should look and behave. **Read it before touching anything drawn** |
| `docs/driver-guide.md` | how to write a driver, ordered by task. `e1000.c` and `r8169.c` are the worked examples |
| `docs/devices.md` | every driver in the tree, by class registry. `check_docs.py` fails the build when a `DRIVER_DECLARE` has no row |
| `docs/filesystem-layout.md` | what lives where on the OS's own disk, and the deliberate FHS divergences. Not advisory: `check_layout.py` fails the gate if the built image disagrees, in either direction |
| `docs/boot-flags.md` | every word the kernel looks for on the GRUB command line. Matching is by SUBSTRING across five files with no registry, so **this table is the only list of them** |
| `docs/settings-and-queries.md` | facts vs settings vs tunables, and how an app reads or changes either |
| `docs/commands.md` | the INDEX over `docs/commands/`, one page per command |
| `docs/bugs.md` / `docs/roadmap.md` / `docs/roadmap-details.md` | what is broken, what is not built yet, and the long form of both |

**Design documents** -- read the one for the area before starting work
shaped like it. Each carries the finding that decides the work's scope,
and several carry the honest case AGAINST:

`query-design.md` (BUILT), `errno-design.md`, `libc-design.md`
(**tolibc** -- BUILT; read before any libc-shaped work),
`cc-design.md` (**a `cc` that runs ON toy-os** -- PLANNED, nothing built;
read before any compiler- or SDK-shaped work, and before believing the
roadmap's "self-hosted C compiler is out of scope" line covers it),
`umdf-design.md` (**a driver as a PROCESS** -- PLANNED, nothing built;
read before any user-mode-driver work, and for why an IOMMU-less DMA
driver in ring 3 buys crash isolation and NOT containment),
`dynlink-design.md`, `signals-design.md` (read before Phase 1's
signal/TTY/job-control work -- its point is that those are ONE problem),
`blocking-design.md` (stages 1-3 BUILT; **read before touching
`switch_to()`, `block_common()` or `FS_OP()`**), `fslock-design.md`
(**finer filesystem locking** -- stage 0 BUILT; read before touching
tfs3's scratch state or adding a caller that sleeps under the fs lock), `smp-design.md` (stage 1 BUILT; **read
before adding a module-level buffer to anything a syscall reaches**),
`modules-design.md`, `update-design.md`, `rootfs-design.md`,
`winserver-ring3-design.md` (stages 0-1 BUILT; **read before touching
`kernel/proc/win_*.c`**).

**Rules for editing docs/:**

- **A NEW COMMAND NEEDS ITS PAGE IN `docs/commands/` IN THE SAME
  CHANGE** -- the build refuses otherwise. Where a program declares a
  `cmd_usage()` string the page must carry it verbatim, so a flag added
  to the program and not to the page is a build failure. The prose is
  deliberately unchecked; exemptions are named in `check_docs.py`'s
  `COMMAND_PAGE_EXEMPT`.
- **A new convention goes in `docs/conventions/<area>.md` AND gets its
  headline added to this file's index**, or it is invisible to the next
  session. That pairing is the whole mechanism and half of it is not
  optional.
- **Adding a decision entry**: write it in the file for its area and run
  `tools/gen_decisions_index.py`. The index is GENERATED and
  `check_docs.py` fails the build when it is stale. **A NEW FILE must be
  added to that script's `ORDER`.** **Entries are SELF-CONTAINED** --
  they used to be pointers into a changelog, and when that was deleted
  the entries leaning on it were stranded. The bar: it answers "why this
  way and not the obvious way", and a future session would plausibly
  re-litigate it.
- **Milestones are NAMED, not numbered** -- a milestone is its title and
  its position is its layer. Older `Milestone N` references resolve
  through the one-way legend at the end of `docs/roadmap-details.md`.
- **URGENCY IS A SECOND AXIS, MARKED WHERE THE ITEM LIVES.** Put
  `**NEXT**` on the roadmap item and run `tools/gen_next_up.py --write`;
  the "Next up" section is GENERATED. Do NOT hand-write a list of titles
  at the top. `**NEXT**` means "do this before the unmarked work around
  it", never "this is broken".
- **`docs/roadmap.md` IS ORDERED BY WHAT MUST BE BUILT FIRST.** Phases
  1-4 are a dependency chain; below them are tracks (storage, GUI,
  hardware, tooling) which depend on neither the phases nor each other.
  **EVERY ITEM IS ONE LINE** -- rationale, measurements and repros go to
  `docs/roadmap-details.md` under a heading of the same name, which is
  why the titles must match in both files.

## Two skills live IN this repo

`.claude/skills/toy-os-feature-workflow/` -- the end-to-end playbook
(research first, offer real choices, build and test with proof, write
the docs, ship). `.claude/skills/network-egress-disclosure/` -- what to
disclose when testing reached a host outside this machine.

Both are tracked here rather than in `~/.claude/skills/` so they have
history, diff review and a backup. **Update them in the repo**, or the
two copies drift and the untracked one silently wins.

**`SKILL.md` is the PLAYBOOK; the accumulated lessons live in
`references/`** -- `session-testing.md`, `session-diagnosis.md`,
`session-gui.md`, `session-design.md`, beside `delivery-checklist.md`,
`questions-that-worked.md` and `doc-templates.md`. **A new lesson goes
in the reference file for its area, not back into `SKILL.md`**, which
holds the sequence and nothing else.
## Delivering changes

The files are already on the real checkout -- there is nothing to
"deliver". Commit with plain `git`, and push verified work.

**END EVERY DELIVERY WITH A SHORT "TRY IT YOURSELF" GUIDE** (standing
project instruction). Whenever a change adds or alters something a
person can SEE or DO in toy-os, the final response says -- in a few
lines, not an essay -- how to reach it on a real boot: which `make run`
flags, which app or command, what to type, and what should happen. The
maintainer runs this OS interactively; a change only ever demonstrated
through a test tool's pass/fail table has not been handed over.

- **Say what to TYPE and what to EXPECT.** "Ctrl-C now works" is not a
  guide; "`spin_test 900000`, then Ctrl-C -> job stops, `^C`, prompt
  back" is.
- **Say when a feature is NOT reachable from the default boot.** Several
  are not -- `Ctrl-C` needs a `text` target, the ATA/PIO control only
  greys out on a machine with no DMA engine, `hires` work needs
  `KCMDLINE="video=1920x1080"`.
- **Skip it when there is genuinely nothing to see** -- a refactor, a
  doc edit, an internal invariant. Say so in a line rather than
  inventing a demonstration.

**LIST EVERY FILE ADDED OR EDITED IN THE FINAL RESPONSE**, as a compact
list (standing project instruction) -- always, regardless of mode.

**AND IF TESTING LEFT THIS MACHINE, REPORT EVERY EXTERNAL HOST IT
REACHED -- IN THE CHAT, NOT IN THE COMMIT** (standing project
instruction, changed 2026-09-07). Network testing here can reach real
hosts through QEMU's user-mode networking, from the maintainer's address
and connection. So the final response ends with an `External hosts
contacted during testing:` list -- each host, its resolved ADDRESSES,
the protocol, and what did it. Say `none (SLIRP and localhost only)`
when that is the answer. **SLIRP's 10.0.2.3 is a FORWARDER, not a
resolver**: a DNS lookup through it leaves the machine even though the
address looks local. Prefer a server on loopback. Full rule:
`.claude/skills/network-egress-disclosure/`.

**AND WHEN A CHANGE MOVES A LAYER BOUNDARY, DRAW THE STACK** (standing
project instruction) -- an ASCII diagram of the stack with the changed
part marked, and the directory tree when files moved or appeared. Not
prose describing it.

**ONCE, WHEN THE WORK IS DONE.** Draw the stack as it ENDS UP, in the
response that hands the finished work over -- not per stage, not per
commit, not mid-task while the shape is still moving: a diagram of an
intermediate state describes something that was never true for longer
than an hour.

A file list says WHAT changed and never says what the system now looks
like, and this project's structure is what sessions most often re-derive
from scratch. **What counts as moving a boundary:** a new subsystem
directory; a new registry or a new implementation of an existing one; a
header changing audience; a call site moving between rings, or between
the kernel and a driver; anything that changes what a component may
include. An ordinary bug fix, a new widget in an existing toolkit, or a
doc edit does NOT.

**Show BOTH axes when they differ**, because here they usually do: what
the thing sits ON (the vertical call stack) and what it PLUGS INTO (the
class registry). virtio is the worked example -- the transport is one
vertical stack, while virtio-blk reaching `block_device` and virtio-gpu
reaching `display_driver` is a second, orthogonal one.

**Never write personal information into any file** being edited or
added. If a change genuinely seems to need some, ask first, or anonymize
it and say so plainly.

**Any genuinely reusable tooling built during a session belongs in
`tools/`**, not left as a scratch one-off -- see `## tools/` for the
bar. Update the files that describe `tools/` to match.

**AND A NEW TEST TOOL MUST BE NAMED BY A RUNNER** -- `preflight.sh`,
`gui_regress.py` or `ondemand_sweep.py`, or the sweep's
deliberate-exclusions list with a reason. A tool no runner names is run
when somebody types it, which is never: five were found outside every
runner at once, one of them red and pre-existing. Nothing enforces this;
the audit is to enumerate `tools/*_test.py` and subtract what each
runner names.

**AND AT EVERY COMMIT, SWEEP THE BACKGROUND SHELLS** (standing project
instruction, 2026-08-30). A waiter polling for an artifact that never
comes to exist can never exit, and nothing mentions it again -- five
were found spinning six hours into one session, only because the
maintainer asked. So a commit is the checkpoint: **list what is still
running, and close what the finished work no longer needs.** `ps aux |
grep "[z]sh -c source"` names every shell the session holds, and
`preflight.sh` prints any `until`/`while` poll among them with its
elapsed time -- minutes is normal, hours is the leak. Kill by PID.
