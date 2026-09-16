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
  file, then **`docs/conventions/<area>.md` for the area you are about
  to touch** -- this file indexes every convention by headline and that
  file carries the body -- then `docs/decisions.md` for "why is it like
  this" (**its INDEX first**), then `docs/roadmap.md` for whether the
  thing is already known broken. Anything drawn adds
  `docs/gui-guidelines.md`, which is binding.
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
digit loop, a formatter, a path join, or a rasteriser.** Seven headers,
all reachable through `kapi.h`, all with KTESTs: `string.h`, `knum.h`,
`kfmt.h`, `kpath.h`, `fixed.h`, `geom.h`, `rubberband.h`, `ttf.h`,
`krandom.h`. What bites without warning:

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

- **THE FILESYSTEM IS NOT RE-ENTRANT, and `vfs.c` holds a preemption
  guard because of it.** `tfs3.c` walks through module-level scratch
  buffers and a ring-3 process is preemptible inside a syscall, so two
  interleaved reads overwrite each other's block. `FS_OP()` wraps every
  backend call in `scheduler_preempt_disable()`/`_enable()`. It does NOT
  make an `fs_list()` callback safe to call `fs_*` from (that is
  recursion, which a depth counter cannot see), and an unbalanced
  `disable()` hangs the machine -- which is why `_enable()` clamps at
  zero.
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
## Conventions indexed here, written up in `docs/conventions/`

**Every headline below is a real rule; the body is one file away.**
CLAUDE.md is the always-loaded context, so it carries the RULE and
`docs/conventions/<area>.md` carries the reasoning and the trap -- the
same split this file already asks for everywhere else. **Read the file
for the area you are touching before you edit it**, and read the entry
whenever a headline here tells you something you did not already know.

### The kernel: syscalls, memory, processes, init

`docs/conventions/kernel.md`

- **Monotonic time is an INTERFACE, and wall clock is not one of its
  implementations.**
- **THE WALL CLOCK IS A SOFTWARE CLOCK ANCHORED TO THE CLOCKSOURCE, AND
  THE RTC IS READ ONCE AT BOOT -- everything the kernel hands out is
  UTC, and a LOCAL time is ring 3's to compute**
- **THE TIMEZONE IS RING 3's: the city database, the DST rules and the
  conversion are in libc (`tzset`/`localtime`/`tz_localize`), and the
  kernel keeps only the SELECTION**
- **SETTING THE CLOCK IS `SYS_SETTIME`, AND SPEAKING NTP IS A RING-3
  PROGRAM'S JOB**
- **The kernel's idle work has ONE owner: `scheduler_idle()`**
- **EVERY KEY REPORTS SOMETHING, AND THE KEYPAD REPORTS CHARACTERS**
- **THE BIOS OWNS THE xHCI UNTIL YOU ASK FOR IT, AND THE ASK COMES
  BEFORE THE RESET**
- **AN INTEL xHCI'S USB2 PORTS MAY BE ROUTED TO AN EHCI, AND SWITCHING
  THEM IS A SECOND QUIRK**
- **ONE USB3 SOCKET IS TWO PORT NUMBERS, THE CONTROLLER DOES NOT SAY
  WHICH PAIR, AND NEITHER RANGE COMES FIRST**
- **A DMA TARGET MUST NOT BE ON THE STACK, AND THE FAILURE IS A
  CORRUPTED SAVED REGISTER SOMEWHERE ELSE**
- **A BAR'S SIZE IS PROBED ONCE, AT ENUMERATION, AND A DRIVER ASKS
  `pci_bar_mem_size()`**
- **USB IS xHCI ONLY, ITS PORTS WAIT ON PED RATHER THAN PRC, AND EVERY
  DMA OBJECT IS ITS OWN FRAME**
- **INPUT DEVICES REGISTER WITH THE INPUT CORE, and the canonical event
  is evdev -- including `/etc/kbs`, so only the PS/2 driver ever sees a
  scancode**
- **`kbd` PRINTS EVERY STAGE OF A KEYPRESS, AND ITS KERNEL LOG IS OFF BY
  DEFAULT**
- **A LOG LINE'S LEVEL ARRIVES IN-BAND (`klog_printf(KLOG_ERR "...")`),
  AND THE THRESHOLD IS THE CONSOLE'S -- THE RING KEEPS EVERY LEVEL**
- **KERNEL LOG OUTPUT IS QUEUED, NEVER WAITED ON -- A STALLED COM1
  CONSUMER MUST NOT STOP THE MACHINE**
- **A GUEST SPIN-WAIT NEEDS `cpu_relax()` (`pause`), AND UNDER KVM THAT
  IS NOT AN OPTIMISATION**
- **VIRTIO INTERRUPTS ARE OPT-IN, a forgotten ISR read hangs the
  machine, and ENABLING IS THE LAST STEP**
- **USING A SUBSYSTEM BEFORE ITS init() IS A PANIC, not a soft failure**
- **A RING-3 CRASH WRITES A REPORT TO `/var/crash`, AND A KERNEL PANIC
  DOES NOT**
- **A panic NAMES THE FUNCTION**
- **A kernel panic prints enough to diagnose from a pasted log**
- **Kernel stacks are 16 KiB, have a GUARD PAGE, and carry a CANARY**
- **A FAILED SYSCALL RETURNS `-ERRNO`, AND `-1` IS `-EPERM`.**
- **FILE DESCRIPTORS ARE TWO LEVELS, AND 0/1/2 ARE ORDINARY ENTRIES.**
- **A FULL PIPE BLOCKS ITS WRITER, AND A CHILD INHERITS ONLY 0/1/2.**
- **One process can run another and read its output**
- **Per-process facts exist, and Task Manager is a ring-3 app.**
- **`meminfo audit` COMPARES PAGE TABLES AGAINST THE ALLOCATOR.**
- **THERE IS A PROCESS TREE: `ppid`, reparenting, and `waitpid(-1)`.**
- **THERE IS AN INIT, IT HOLDS PID 1, AND IT CANNOT BE KILLED.**
- **INIT STARTS AND SUPERVISES THE DESKTOP, and the desktop is a
  SERVICE.**
- **A SERVICE CAN SAY IT IS READY, AND `After=` THEN MEANS "USABLE"
  RATHER THAN "SPAWNED".**
- **A SERVICE IS CONTROLLED OVER A CHANNEL, WITH THE FILE PLUS DOORBELL
  AS THE FALLBACK, AND `/bin/service` IS THE LEVER**
- **INIT CANNOT BE KILLED BY A SIGNAL IT HAS NOT CAUGHT, and the guard
  is in `do_default_action()`, not only `scheduler_kill()`**
- **`SYS_SLEEP` exists, and a caller with no scheduler slot gets -1.**
- **`ps` is a REAL `/bin` PROGRAM, not a builtin**
- **A PROCESS'S MEMORY IS FREED WHEN IT DIES, NOT WHEN IT IS REAPED --
  and killing needs a DIFFERENT entry point from exiting.**
- **A SWAPPED PAGE IS A NON-PRESENT PTE THAT STILL RECORDS THE PAGE, and
  a walker that reads one as a hole LEAKS ITS SLOT -- which nothing
  audits.**
- **A USER MAPPING SAYS WHETHER IT OWNS ITS FRAME, and getting that
  wrong is silent.**
- **Kernel code touches user memory ONLY through `vmm.h`'s copy
  helpers**
- **`SYS_WNOHANG` exists, and the bug that produced it is the lesson.**
- **`SYS_SBRK` RESERVES; THE PAGE ARRIVES ON TOUCH.**
- **THE RING-3 MAP IS SIZED FOR 4K, and a region's END is what the next
  thing must clear.**
- **A FRAME IS ALLOCATED FROM A ZONE, EVERY CALLER NAMES ONE, AND
  `kmalloc` MEMORY MAY BE ABOVE 4 GiB.**
- **EVERYTHING CPU-ONLY IS ON `ANY` NOW, AND THE FALLBACK INTO DMA32
  STOPS AT A RESERVE.**
- **`SYS_SBRK` is PER PROCESS.**
- **A RING-3 IMAGE HAS NO SIZE LIMIT, BECAUSE THE HEAP STARTS WHERE IT
  ENDS.**
- **THE USER STACK IS RESERVED AND GROWN ON FAULT, and the GAP is what
  keeps that safe.**
- **The ring-3 address-space map is `kernel/include/kernel/uaddr.h`,
  stated once.**
- **`SYS_MMAP` IS A REGION LIST, ITS ARENA IS ITS OWN RANGE, AND A FILE-
  BACKED FAULT-IN REFUSES INSIDE AN `FS_OP`**
- **A DYNAMIC EXECUTABLE IS ENTERED THROUGH `/lib/ld-toy.so`, AND THE
  KERNEL NEVER LEARNS ET_DYN**
- **EVERY `/bin` AND GUI PROGRAM LINKS `/lib/libc.so` AND
  `/lib/libuapp.so`; init, reboot AND `/tests` ARE STATIC; AND THE `#`
  SHELL'S BARE NAME SPAWNS**
- **The kernel heap has a debug mode, and it is a RUNTIME toggle**
- **The kernel RELOCATES ITSELF at boot -- it is not running where it
  was linked.**
- **A BLOCKED PROCESS WAITS ON A CHANNEL, AND A CHANNEL IS AN ADDRESS.**
- **THE KERNEL STORES NO ENVIRONMENT, AND `SYS_SPAWN` TAKES A STRUCT**
- **A SPAWN NAMES THE CHILD'S fd 0 AND fd 1, AND A SOCKET IS ACCEPTED ON
  BOTH**
- **A SERVICE'S STDOUT IS A LOG RECORD TAGGED WITH ITS NAME, AND
  `SPAWN_FD_LOG` IS HOW A SPAWN ASKS FOR IT -- a record is a WRITE, not
  a line, and `eol` is what lets a reader join them**
- **A SIGNAL SETS A BIT; THE KERNEL ACTS ON IT WHEN IT IS SAFE TO.**
- **A HANDLER IS RING-3 CODE, AND THE KERNEL BORROWS ITS STACK TO CALL
  IT.**
- **A CHILD'S DEATH RAISES SIGCHLD, AND THE NOTIFICATION HAS ONE HOME.**
- **A PROCESS GROUP IS AN INT, AND SPAWN TAKES IT.**
- **THE CONSOLE HAS AN OWNER AND A FOREGROUND GROUP, AND THE INTR KEY IS
  TEMPORARY WHERE IT IS.**
- **A TRACER NAMES ITS CHILD AT THE SPAWN, AND THE TRACE GOES TO ITS
  TERMINAL.**
- **A THREAD IS A SLOT WHOSE `tgid` NAMES SOMEBODY ELSE**
- **RING-3 `malloc` TAKES A LOCK; THE KERNEL'S DOES NOT**
- **THE THREAD POINTER IS FS.base, AND THE SCHEDULER RELOADS IT**
- **THERE IS A SOUND CLASS, ITS STREAM IS EXCLUSIVE, AND THE RING IS
  SHARED MEMORY**
- **AN INIT IS DECLARED, NOT CALLED: `INITCALL(fn, LEVEL)` BESIDE THE
  FUNCTION, AND `kernel_main()` WALKS THE LEVELS**
- **A PCI DRIVER DECLARES A MATCH TABLE AND A `probe()`, AND
  `pci_bind()` CALLS IT ONCE PER DEVICE**
- **A SYSCALL HANDLER RUNS WITH INTERRUPTS OFF, AND A WAIT ON
  `pit_ticks()` THERE NEVER ENDS**
- **A SIGNAL CAN BE BLOCKED FROM RING 3 NOW, AND `sigsuspend` IS THE ONE
  WAIT THAT IS NEVER RESTARTED**
- **A SYSCALL'S DURATION IS A STALL EVERYTHING ELSE FEELS, AND `stalls`
  IS WHAT MEASURES IT -- TIMED WITH THE TSC, BECAUSE THE SYSTEM CLOCK
  CANNOT SEE ITS OWN WINDOW**
- **INTEL HDA IS THE THIRD SOUND DEVICE, ITS CODEC IS ROUTED BY A
  GENERIC WALK, AND THE VOLUME TAPER IS THE USB DRIVER'S**
- **WRITE-COMBINING IS A 4 KiB DECISION: A 2 MiB PAGE A RANGE ONLY
  PARTLY COVERS IS SPLIT BEFORE IT IS TYPED**
- **THE INTEL DISPLAY DRIVER INHERITS THE FIRMWARE'S MODE AT BOOT AND
  CAN RE-PROGRAM THE NATIVE ONE, AND A BACKLIGHT IS A DISPLAY
  CAPABILITY**
- **THE MONITOR'S EDID IS A DISPLAY-LAYER FACT READ ONCE AT PROBE, AND
  THE INTEL DRIVER READS ITS FIRMWARE TIMINGS BACK BEFORE IT MAY WRITE
  ANY**
- **A STATE TRANSITION THAT ENDS IN A SWITCH MUST NOT BE PREEMPTIBLE**
- **A BOUNDED WAIT USES A DEADLINE WHERE THE CLOCK ADVANCES WITH
  INTERRUPTS OFF, AND A POLL COUNT WHERE IT DOES NOT**
- **THERE IS A LOCAL APIC NOW, AND A DEVICE MAY BE ON A VECTOR INSTEAD
  OF A LINE**
- **THE I/O APIC DELIVERS EVERY LINE, THE MADT SAYS WHERE AN ISA IRQ
  ARRIVES, AND A PCI PIN IS ROUTED BY `_PRT`**
- **THE TICK IS A CLOCKEVENT, AND ON A MACHINE WITH A LAPIC IT IS NOT
  THE PIT**
- **A VIRTIO DEVICE TAKES MSI-X ONLY, ITS QUEUE VECTORS ARE WRITTEN BY
  `virtqueue_setup()`, AND ITS ARMING WRITE IS `DRIVER_OK`**
- **A USB ETHERNET ADAPTER IS A `net_device`, AND ITS CONFIGURATION IS A
  CHOICE**
- **A VENDOR CONFIGURATION NEEDS A DRIVER THAT NAMES THE DEVICE, AND AN
  RTL8153 IS FRAMED RATHER THAN RAW**
- **A DEVICE CANNOT REGISTER BEFORE ITS CORE'S init(), AND NOW IT IS
  TOLD SO**
- **USB AUDIO IS A SOUND DEVICE ON AN ISOCHRONOUS ENDPOINT, UAC1 OR
  UAC2, AND THE FORMAT IS NOT NEGOTIATED (BUT A UAC2 RATE IS SET)**
- **AN ISOCHRONOUS PACKET IS THE RATE'S SHARE OF ONE SERVICE INTERVAL,
  NOT wMaxPacketSize**
- **AN ISOCHRONOUS ENDPOINT DOES NOT HALT, AND ITS RING RUNNING DRY IS
  NORMAL**
- **THERE IS A NETWORK DEVICE CLASS, THE STACK IS IN THE KERNEL, AND THE
  RECEIVE PATH IS SPLIT ACROSS AN INTERRUPT**
- **TCP IS CLIENT-SIDE, REASSEMBLES IN THE RECEIVE BUFFER, AND ITS
  TIMERS RIDE THE BLOCKING RECEIVE**
- **THE TCP RECEIVE BUFFER IS NOT RE-ENTRANT, AND BOTH ENDS HOLD A
  PREEMPTION GUARD**
- **A SOCKET RECEIVE BLOCKS, ONE CHANNEL SERVES THE WHOLE STACK, AND THE
  DEADLINE LIVES ON THE SOCKET**
- **UDP IS A PORT DEMUX, DHCP AND DNS ARE RING-3 PROGRAMS, AND A NAME IS
  RESOLVED BY A LIBRARY**
- **NOTHING INVENTS AN ADDRESS: A CARD COMES UP UNCONFIGURED,
  `/bin/netd` RUNS AT BOOT, AND NO SERVER MEANS LINK-LOCAL**
- **A WAIT CAN CARRY A DEADLINE, AND READINESS IS NOT DELIVERY**
- **THE MACHINE STOPS THROUGH ITS OWN ACPI TABLES, AND EVERY FALLBACK
  BELOW THAT LOGS A LINE**
- **TURN THE WAKE SOURCES OFF BEFORE THE SLEEP WRITE, OR S5 IS A REBOOT
  -- THE GPE BLOCKS AS WELL AS PM1_STS**
- **A MACHINE IS REACHABLE OVER THE NETWORK NOW, AND BOTH SERVICES SHIP
  DISABLED**
- **A PROGRAM MUST NOT PRINT ITS OWN VERSION AS THE SYSTEM'S: ASK
  `QUERY_VERSION`**
- **A LEASE IS RENEWED, NOT RE-ASKED, AND A SINGLE SLEEP IS SILENTLY
  CAPPED**
- **A NETWORK CLIENT WAITS FOR CARRIER, AND A CONFIG FILE MUST FIT THE
  PARSER'S BUFFER**
- **A DRIVER DECLARES ITSELF AS DATA, AND THE CLASS REGISTRY NAMES EACH
  DEVICE IT BINDS**
- **A DRIVER CAN BE A MODULE, `drivers.conf` SAYS WHICH, AND A MODULE
  MAY LINK ONLY AGAINST `kernel/core/kexports.c`**
- **A SEND WINDOW MAY NOT EXCEED THE RECEIVER'S SOCKET QUEUE, OR IT IS
  SLOWER THAN NO WINDOW**
- **AN MTU-SIZED DATAGRAM IS THE CEILING, BECAUSE NOTHING FRAGMENTS**
- **A CONNECTION IS LOGGED WHERE IT IS OPENED, AND THE NAME COMES FROM
  THE RESOLVER**
- **THERE IS A NETWORK DAEMON, IT OWNS NAMING AND ADDRESSES, AND ITS
  RULES ARE A FILE**
- **A NETWORK INTERFACE IS NAMED BY THE CARD, NOT BY THE SOCKET -- AND
  RENAMING IS RING 3's**
- **A NETWORK DEVICE CAN BE REMOVED NOW, AND REMOVAL HAS TO UNDO THREE
  THINGS**
- **TWO PROCESSES SHARE MEMORY THROUGH A NAME, AND THE NAME IS THE HARD
  PART**
- **SOUND IS MIXED BY A SERVICE, AND THE CARD IS STILL EXCLUSIVE**
- **HTTPS IS TWO LIBRARIES, AND NEITHER MAY LIVE IN `userland/lib/`**
- **A RESIZE IS A SIGNAL, AND ONLY WHEN THE SIZE ACTUALLY MOVED**
- **A LINE LONGER THAN THE TERMINAL IS WIDE NEEDS ROWS, NOT `\r`**
- **ADDING A SYSCALL IS THREE EDITS, AND ONE OF THEM IS A TABLE ROW.**
- **A SPAWN CARRIES AN ARGV VECTOR WITH A LENGTH, AND THE STRING FORM IS
  SPLIT AT THE EDGE**
- **THERE IS A `fork()` NOW, IT SHARES COPY-ON-WRITE, AND SPAWN IS STILL
  THE DOOR**
- **WHAT CROSSES A tty IS ANSI; WHAT REACHES A WINDOW IS A KEYSYM**
- **A TERMINAL BELONGS TO A SESSION, NOT TO ONE PID**
- **A SCRIPT RUNS BY NAME, AND `#!` IS THE LOADER'S JOB -- NEVER A
  SHELL'S**
- **AN EXEC LOADS BEFORE IT TEARS DOWN, TAKES `struct spawn_msg`, AND
  KEEPS THE SLOT**

### GUI, Toykit and the desktop

`docs/conventions/gui.md`

- **A WINDOW'S GEOMETRY IS REMEMBERED PER APP, AND THE KEY IS `app_id`
  -- NEVER `app_identity`**
- **MEASURE TEXT, NEVER MULTIPLY: `gfx_char_advance()` /
  `ugfx_char_advance()`**
- **`UUI_COLOR_UNSET` IS A COLOUR (0xFF000000) UNTIL A WIDGET RESOLVES
  IT**
- **A PICKER ASKS WHAT IT WOULD GET BEFORE IT ASKS FOR IT:
  `WIN_SHOT_WINDOW_AT` + `WIN_SHOT_PROBE`**
- **A WIDGET RESOLVES ITS COLOURS WHEN IT DRAWS, NOT WHEN IT IS BUILT**
- **`utext` IS A FIXED GRID, AND ITS CALLERS OWE IT A MONOSPACE FACE**
- **A SHARED GEOMETRY HELPER MEASURES IN WHATEVER FACE THE CALLER HAS
  SELECTED, AND `ugfx_set_font()` IS PER PROCESS**
- **A LOADED FACE STILL ONLY DRAWS 101 GLYPHS.**
- **`font glyph <char>` SHOWS WHAT WILL ACTUALLY BE DRAWN, AND IT READS
  BOTH SIDES.**
- **RING 0 PARSES NO FONT: THE CONSOLE DRAWS BAKED BITMAPS, THE DESKTOP
  DRAWS `/bin/fontd`'s ATLAS**
- **THERE ARE TWO FONT TIERS, AND THE SHARED ONE CANNOT GROW TO COVER
  THE OTHER.**
- **THERE ARE TWO FONT FAMILIES -- `system.font_face` IS PROPORTIONAL
  AND `system.font_mono` IS THE FIXED CELL -- AND A WIDGET SAYS NOTHING:
  `ugfx_font_session()` IS THE UI FAMILY, `ugfx_font_mono()` IS THE
  OTHER**
- **BOLD IS A WEIGHT OF A FAMILY, AND A FAMILY IS A FILENAME RULE.**
- **KERNING IS APPLIED BY EVERY TEXT PATH, AND MEASURING MUST MATCH
  DRAWING.**
- **THE FONT CAN CHANGE UNDER A RUNNING CLIENT, AND `WIN_EV_FONT` IS HOW
  IT FINDS OUT.**
- **A FONT FACE IS NAMED BY ITS FILENAME, AND `builtin` IS NOT A FACE.**
- **`WIN_CLIENT_MAX_W/H` TRACKS THE DISPLAY CEILING, AND A SCREEN BIGGER
  THAN IT BREAKS MAXIMIZE SILENTLY.**
- **A WINDOW HAS ROUNDED CORNERS UNLESS IT IS MAXIMIZED, AND THE CORNER
  IS BLENDED OVER WHAT IS REALLY BENEATH**
- **"MAXIMIZED" STOPS AT THE TASKBAR AND KEEPS THE TITLE BAR; FULLSCREEN
  IS A FLAG BESIDE THE STATE, AND `window_has_chrome()` IS THE ONE PLACE
  IT MEANS "NO CHROME"**
- **A FULLSCREEN CLIENT MAY BE LEASED THE DISPLAY'S OWN SCANOUTS, AND
  WHILE IT HOLDS THEM THE COMPOSITOR DRAWS NOTHING**
- **`-vga virtio` IS A REAL DISPLAY DRIVER, and nothing else boots it**
- **`apps/ui/` IS GONE, and the GUI toolkit is `userland/ui/`**
- **Ring-3 GUI apps are written against Toykit's `uapp`, and a new one
  is a `.c` file in `userland/gui/` with NO Makefile edit.**
- **A TOOLBAR IS `uui_toolbar`, AND ITS STATE CALLBACK IS THE MENU
  BAR'S**
- **A MENU BAR IN AN APP WITH ROUTED WIDGETS MUST BE `uui_menubar_ops`,
  NOT HAND-ROUTED**
- **ONE MENU WIDGET SERVES A BAR AND A CONTEXT MENU:
  `uui_menubar_open_at()`, opened on the secondary RELEASE**
- **A POPUP IS A SURFACE OF ITS CLIENT, PLACED BY THE COMPOSITOR, AND A
  PRESS OUTSIDE THE CLIENT'S SURFACES DISMISSES IT**
- **A DIALOG IS A SECOND TOPLEVEL OF ITS CLIENT, OWNED BY ONE OF ITS
  WINDOWS, AND `uapp_window_open()` IS HOW AN APP GETS ONE**
- **THE FILE CHOOSER IS ONE WIDGET IN ONE WINDOW: `ui/uui_filedialog.h`,
  AND THREE APPS OPEN IT**
- **A TAB IS A SESSION, AND `uui_tabs` IS THE STRIP**
- **A TITLE COMES FROM THE SHELL, AS AN OSC**
- **TERMINAL IS A TERMINAL EMULATOR, NOT A SHELL WITH A WINDOW**
- **AN APP'S OWN PREFERENCES ARE THE APP'S, NOT THE SETTINGS REGISTRY'S
  -- `/etc/terminal.conf` IS THE FIRST, A COLOUR SCHEME IS A DATA FILE,
  THE FILE IS IN ANSI ORDER WHILE A CELL HOLDS A VGA INDEX, AND THE
  GRID'S FONT IS THE TERMINAL'S WHILE THE CHROME'S IS THE DESKTOP'S**
- **THE TERMINAL'S SCREEN IS A GRID, AND THE ANSI PARSER IS THE KERNEL'S
  COMPILED TWICE**
- **An app with a cadence sets `tick_ms` and BLOCKS between frames.**
- **An app refuses its OWN second copy -- the launcher never does.**
- **WHAT PROGRAM A CLIENT IS COMES FROM ITS SPAWN PATH
  (`QUERY_PROCPATH`), THE COMPOSITOR ASKS FOR IT, AND `WIN_REQ_ACTIVATE`
  IS THE ONE CHANNEL MESSAGE WITH A REPLY**
- **`uui_table` sorts on a header click, and an app supplies only a
  COMPARATOR.**
- **A WIDGET WITH A SCROLLBAR ANSWERS `hit` WITH ITS WHOLE RECT, AND
  `_hit()` KEEPS THE ROW QUESTION**
- **A SCROLLBAR CAN LIE DOWN (`UUI_SCROLLBAR_HORIZ`), AND ITS OFFSET
  THEN RUNS THE OTHER WAY**
- **A SCROLLBAR'S SHAPE IS A RADIUS THE APP CHOOSES, AND THE DEFAULT IS
  A CAPSULE**
- **`utext` HAS A WRAP MODE, AND THE CALLER OWNS ITS STORAGE**
- **A `uui_scrollview` NOTICES when its content's item list changes**
- **A STRING SETTING GETS A TEXT FIELD IN SYSTEM SETTINGS, AND ITS
  `staged` IS A CHANGED FLAG RATHER THAN AN INDEX**
- **`uui_spinbox` IS FOR A NUMBER; `uui_slider` IS FOR AN ORDERED
  ENUM.**
- **A DRAG NEEDS THE BUTTON STILL DOWN, AND THE POINTER GRAB IS NOT THAT
  FACT**
- **`uui_scale` IS FOR A CONTINUOUS NUMBER; `uui_slider` IS FOR AN
  ORDERED ENUM**
- **THERE IS A SYSTEM CLIPBOARD, IT IS A RING-3 SERVICE
  (`/bin/clipboardd`, `lib/uclip.h`), IT HOLDS FILES OR TEXT, AND A
  PASTE COSTS NO SYSCALL**
- **A CUT MOVES NOTHING UNTIL THE PASTE, AND IS SPENT BY IT; THE
  CLIPBOARD KEYS ARE THE APP'S, NOT THE WM'S**
- **`uui_splitter` IS THE DRAGGABLE DIVIDER, AND IT OWNS A FRACTION
  RATHER THAN A PIXEL COLUMN**
- **A LAYOUT CHILD'S SIZE CAN BE PINNED FROM OUTSIDE:
  `uui_item.main_size` (LAST in the struct -- apps initialise it
  positionally)**
- **A CLIENT MAY ASK FOR A RESIZE CURSOR NOW:
  `WIN_CURSOR_RESIZE_H`/`_RESIZE_V`, and the compositor still wins on a
  window edge**
- **A CONTROL BELOW THE FOLD IS UNREACHABLE, not merely hard to hit**
- **A SCREENSAVER IS A PROGRAM IN `/bin/wm/savers` AND THE COMPOSITOR
  OWNS THE IDLE CLOCK**
- **A SAVER'S OPTIONS ARE A DATA FILE BESIDE IT, AND SYSTEM SETTINGS
  GENERATES THE CONTROLS FROM IT**
- **A CLICK THAT MISSES THE WINDOW LANDS ON WHATEVER IS BEHIND IT, AND
  AT THE BOTTOM OF THE SCREEN THAT IS THE TASKBAR**
- **`on_draw` RUNS BEFORE THE WIDGETS; `on_draw_over` RUNS AFTER.**
- **`uui_meter` IS THE READING WIDGET, AND IT RESERVES EVERY ROW IT
  COULD USE**
- **LONG WORK BELONGS IN A CHILD PROCESS, NOT IN A GUI CLIENT'S EVENT
  LOOP**
- **A WIDGET ARRAY IS DECLARED TWICE: `uapp_desc.layout` SIZES AND
  DRAWS, `uapp_desc.widgets` GETS INPUT**
- **`uui_label` WRAPS ONLY IF ASKED, AND THE CALLER RESERVES THE ROWS.**
- **`uui_sidebar` IS THE NAVIGATION WIDGET; `uui_tree` MODELS
  CONTAINMENT.**
- **A SETTING DECLARES ITS CATEGORY, and the sidebar is generated from
  it.**
- **`uui_table` is the multi-column widget**
- **Editable text has ONE implementation of what editing means**
- **A ring-3 app does NOT route mouse input to its widgets -- the
  toolkit does**
- **The toolkit DRAWS the declared widgets too, popups last.**
- **The GUI stack has names -- use them.**
- **`ugfx` has a SCREEN surface now, and it is the compositor's**
- **The registered compositor can be GRANTED the real framebuffer**
- **`uui_radio_list` arms on press and COMMITS ON RELEASE**
- **A WINDOW'S APPLICATION IDENTITY IS THE KERNEL'S, not the app's**
- **The TASKBAR'S LAYOUT IS ONE FUNCTION, and past a floor it groups by
  app**
- **The WM has a SLOW-FRAME WATCHDOG**
- **There is a Crash Test app**
- **THE KERNEL CONSOLE STOPS PRESENTING WHILE A COMPOSITOR OWNS THE
  SCREEN**
- **A CLIENT NAMES ITS POINTER SHAPE, AND THE COMPOSITOR CLAMPS IT TO
  THE CONTENT AREA**
- **THE BUSY POINTER HAS TWO SOURCES**
- **MOUSE MOTION IS A STATE, NOT A BACKLOG, AND A FULL EVENT QUEUE SHEDS
  INPUT BEFORE A NOTIFICATION**
- **A CLIENT'S EVENTS ARRIVE ON ITS OWN RING, THE KERNEL QUEUE IS THE
  COMPOSITOR'S ALONE, AND A STATE THE INBOX CANNOT TAKE IS RE-SENT**
- **EVERY CLIENT IS PINGED ON A CADENCE**
- **The cursor's shapes are DATA FILES, and a theme is a directory.**
- **The cursor's drawn extent is DERIVED, not a constant.**
- **THE POINTER RIDES THE HARDWARE CURSOR PLANE WHEN THE DRIVER HAS
  ONE**
- **A compositor's view of a dead window is POISONED, not unmapped**
- **A ring-3 compositor delivers events by WRITING THEM, not by calling
  the kernel.**
- **`SYS_FS_GENERATION` is how ring 3 asks "has the filesystem
  changed?"**
- **A ring-3 process can own a real window**
- **`Exec=builtin:` is GONE, and ring 0 contains no applications.**
- **The Start menu is built from FILES**
- **A Start-menu entry launches a RING-3 program**
- **There is no limit on open windows**
- **Super/Win toggles the Start menu, and Alt+F4 closes a window**
- **A window may be dragged off the left/right/bottom edges and UNDER
  the taskbar**
- **The window manager lives in `userland/wm/`**
- **THERE IS ONE KIND OF WINDOW SERVER, AND `win_server_any()` IS HOW
  YOU ASK FOR IT**
- **THE DESKTOP IS A RING-3 PROCESS.**
- **Killing the desktop is survivable, and that is the milestone's exit
  criterion**
- **A GUI tool that needs the compositor role must ASK WHO HOLDS IT**
- **A client that needs raw input without a desktop cannot be driven by
  keystrokes**
- **Four things a ring-0 component loses the moment it becomes a
  process:**
- **COLOURS COME FROM THE THEME, SIZES FROM ITS METRICS -- neither is
  hardcoded.**
- **A WORKER THREAD MAY TOUCH NOTHING IN TOYKIT EXCEPT `uapp_post()`**
- **AN APP LOGS THROUGH `ulog()`/`ulogf()`, not a hand-rolled `logf_`.**
- **THE TOOLKIT OWNS THE KEYBOARD FOCUS RING: set `uapp_desc.focus`.**
- **A WIDGET DESCRIBES ITSELF, AND THE LAYOUT LOG HAS ONE VOCABULARY:
  `<prefix>: layout <name>[.<part>] [i [j]] x y w h`**
- **AN IMAGE IS DECODED IN RING 3, AND `lib/uimg.h`'s CODEC TABLE IS THE
  EXTENSION POINT**
- **COMPRESSION IS A LIBRARY, `lib/uinflate.h`, WITH TWO REAL CALLERS**
- **AN IMAGE IS ALSO ENCODED IN RING 3, AND THE TWO FORMATS ARE FOR
  DIFFERENT THINGS**
- **THE COMPOSITOR COPIES PIXELS AND THE CLIENT ENCODES THEM:
  `WIN_REQ_SCREENSHOT`, `lib/ushot.h`**
- **THE POINTER IS EXCLUDED BY DEFAULT, AND IT IS SUBTLE IN BOTH
  DIRECTIONS**
- **A WIDGET SET CAN BE SWAPPED AT RUNTIME (`uapp_set_widgets`), AND THE
  LAYOUT IS A SECOND HALF**
- **AUDIO IS DECODED AND MIXED IN RING 3, AND `lib/usnd.h` HAS THREE
  SEAMS**
- **MP3 IS THE CODEC TABLE'S SECOND ROW, AND ITS TABLES CARRY THEIR OWN
  PROOF**
- **AN ICON IS A NAME, NOT A PATH, AND IT IS COMPOSITED**
- **AN ICON ON A PANEL IS SYMBOLIC: IT TAKES THE PANEL'S INK, NOT ITS
  OWN**
- **A WINDOW'S TITLE BAR CARRIES ITS APP ICON, AND `title_icon()`
  ANSWERS FOR BOTH DRAWING AND CLICKING**
- **TEXT ON A WALLPAPER IS `ugfx_draw_string_shadowed()`, NEVER A
  GUESSED `bg`**
- **TWO WIDGETS OWN MEMORY, AND BOTH MUST BE RELEASED: `uui_image` and
  `uui_markdown`**
- **A MARKDOWN DOCUMENT IS A WIDGET, AND IT DOES NOT PARSE ANYTHING**
- **THE WALLPAPER IS A REGISTERED SETTING, AND ITS VALUE IS A NAME**
- **THE TASKBAR'S THICKNESS IS A REGISTERED SETTING:
  `desktop.taskbar_height`, in PIXELS, 24..96, default 40.**
- **THE START BUTTON'S APPEARANCE IS A REGISTERED SETTING**
- **DIAGNOSTICS ARE A NAMED REGISTRY, AND THE COMPOSITOR IS THE PROVIDER
  `gui`**
- **DAMAGING A RECT DOES NOT ASK FOR A FRAME -- SET `redraw_pending`
  TOO**
- **THE COMPOSITOR SLEEPS BETWEEN FRAMES, AND TWO THINGS MUST DEFEAT THE
  WAIT**
- **AN OVERLAY IS A ROW IN A TABLE, AND THE TABLE DRIVES DRAWING, CLICKS
  AND HOVER**
- **A DISMISSABLE OVERLAY DECLARES `close`, AND AN OPEN PATH CALLS
  `wm_overlay_close_others()` RATHER THAN NAMING ITS PEERS**
- **THE TRAY HAS A VOLUME FLYOUT, AND THE PANEL OWNS IT TOO**
- **A TRAY ITEM'S VISIBILITY IS A SETTING, `desktop.tray_<item>` =
  `auto` | `always` | `never`, AND `auto` ASKS THE HARDWARE**
- **THE TRAY HAS A NETWORK ITEM, IT READS `QUERY_NETDEV`, AND IT WRITES
  NOTHING**
- **THE TRAY SAYS WHEN SOMEBODY IS ON THIS MACHINE OVER THE NETWORK, AND
  THE KERNEL DERIVES THAT**
- **THE TRAY HAS A BRIGHTNESS FLYOUT, HIDDEN BY DEFAULT WHERE THERE IS
  NO BACKLIGHT**
- **BOTH TRAY FLYOUTS ARE ONE FILE: `userland/wm/tray_slider_popup.c`**
- **A MODE SMALLER THAN THE PANEL IS PLACED BY `system.scaling`, A
  SETTING ON EVERY MACHINE, AND THE FITTER'S SIZE REGISTER IS THE ARMING
  WRITE -- AND ITS WINDOW MUST EQUAL THE PIPE ACTIVE AREA, `panel = 2 *
  position + size`, OR THE SCREEN SKEWS**
- **A PRESENT FLIPS ON A DISPLAY WITH THREE SCANOUTS, THE FLIP NEVER
  WAITS, AND THE COMPOSITOR REPAINTS BY BUFFER AGE**
- **THE SCREEN CAN CHANGE MODE AT RUNTIME, `screen_set_mode()` IS THE
  ONE PLACE THAT DOES IT, AND THE GRANT NEVER SHRINKS**
- **THE CLOCK IS ALWAYS THE RIGHTMOST TRAY ITEM, whatever slot it
  holds**
- **THE TRAY CLOCK OPENS A CALENDAR, AND THE PANEL OWNS IT**
- **THE WEEK'S FIRST COLUMN IS A REGISTERED SETTING:
  `desktop.week_start` = `monday` | `sunday`**
- **THERE IS AN ON-SCREEN KEYBOARD, IT IS A WM OVERLAY, AND IT ENCODES
  KEYS THE WAY THE PHYSICAL ONE DOES**
- **A KEY RELEASE IS `WIN_EV_KEY_UP`, AND THE FOUR MODIFIER KEYS ARE
  KEYS**
- **A SECONDARY CLICK IS THE CLIENT'S INSIDE ITS CONTENT AREA, AND THE
  WM'S EVERYWHERE ELSE**
- **DOOM IS A VENDORED PORT IN `userland/ports/doom/`, LINKED INTO ONE
  BINARY**
- **DOOM'S SOUND IS THE REST OF THE PORT, NOT A REWRITE -- AND THE THREE
  SHIMS ARE ON OUR SIDE**
- **MINESWEEPER IS THE FIRST GAME, AND IT IS AN ORDINARY CLIENT**
- **A DIRECTORY LISTING IS A WIDGET, `uui_fileview`, AND FOUR THINGS
  SHOULD BE DRAWING ONE**
- **THE FILE MANAGER IS A COMMANDER THAT OPENS AS AN EXPLORER -- one
  pane and the tree by default (`g_single`/`g_tree_on` = 1, persisted),
  the two-pane layout one toolbar click away**
- **THE FILE MANAGER'S FIVE VERBS ARE ON THE TOOLBAR NOW, and a
  secondary click opens a context menu that SELECTS what it points at**
- **PROPERTIES IS A PROCESS, `/bin/wm/apps/properties`, and a folder's
  total is walked a few directories per tick**
- **WHAT OPENS A FILE TYPE IS DECLARED BY THE APP THAT OPENS IT
  (`Handles=`), AND `/etc/mimeapps.conf` OUTRANKS IT**
- **A TITLE-BAR BUTTON IS A DISC, AND EVERY GLYPH CENTRES ON THE SAME
  PIXEL AS IT.**
- **AN ICON COLUMN IN A SIDEBAR IS PER SIDEBAR, NOT PER ROW**
- **A MOVE EVENT REACHES EVERY WIDGET AT EVERY DEPTH NOW, AND HOVER
  BELOW TWO CONTAINERS WAS DEAD UNTIL IT DID.**
- **AN OPEN POPUP TAKES THE KEY, AND A KEY-DRIVEN CHANGE IS REPORTED
  LIKE A CLICK.**
- **TYPING IN A LIST SEEKS, AND ONE SEARCH SERVES BOTH WIDGETS.**
- **A TABLE DECLARES WHICH COLUMN A LETTER MATCHES:
  `uui_table_set_seek_col()`**
- **A WIDGET THAT TAKES KEYS STILL GETS NONE UNTIL THE APP ROUTES THEM**
- **A FOCUS INDICATOR IS `uui_focus_ring()`, IN THE THEME'S ACCENT, AND
  THE WIDGET PASSES THE RECT**
- **A SETTING WHOSE CHOICES ARE DATA NAMES THEM ITSELF: `choice_label`,
  tried after `/etc/settings.d` and before the raw value.**
- **THE ICON CACHE IS THE TOOLKIT'S NOW (`userland/lib/icon_cache.h`),
  AND A SIDEBAR HEADING CAN CARRY AN ICON.**
- **A WINDOW HAS TWO BUFFERS, AND THE COMPOSITOR NEVER READS THE ONE
  BEING DRAWN.**
- **A DRAG'S APPEARANCE IS A SETTING, AND `auto` LEARNS RATHER THAN
  GUESSES**
- **A WINDOW'S SIZE BELONGS TO ITS BUFFER, AND THE COMPOSITOR ADOPTS IT
  ON THE PRESENT**
- **A BUFFER IS RESIZED BY ITS DIMENSIONS, NOT BY ITS LENGTH**
- **A FRAME CARRIES ITS OWN BUFFER, GENERATION AND SIZE, AND THE KERNEL
  HOLDS NO WINDOW STATE AT ALL**
- **THE LAYOUT LOG IS OFF UNLESS A TEST TURNS IT ON, AND DEDUPED WHEN IT
  IS.**
- **THE TERMINAL SCROLLS BY WHEEL AS WELL AS BY KEY, AND BOTH MOVE THE
  SAME STATE.**
- **`uui_dialog` IS THE MODAL QUESTION, AND IT SWALLOWS EVERY KEY WHILE
  IT IS UP.**
- **A LONG FILE OPERATION RUNS ON A WORKER THREAD, AND ITS QUESTIONS
  COME BACK AS POSTS.**
- **A WIDGET IS NAMED BY ITS ID, NEVER BY ITS POSITION IN THE ARRAY.**
- **A TEST MUST NOT DERIVE GEOMETRY THE APP ALREADY KNOWS.**
- **A PERSISTED VIEW STATE IS INHERITED BY EVERY LATER RUN.**
- **THE TERMINAL HAS A SCROLLBAR, IN A RESERVED GUTTER, AND THE GRID
  NARROWS FOR IT.**
- **A MOTION WITH NO BUTTON HELD IS IGNORED, NOT TREATED AS A RELEASE.**
- **A SELECTION IS ANCHORED IN THE BUFFER, NOT ON THE SCREEN.**
- **A ROW-COUNT CHANGE SCROLLS THE GRID; IT DOES NOT JUST CLAMP THE
  CURSOR.**
- **A WINDOW IS RESIZED IN A TEST BY `gui resize W H`, NEVER BY DRAGGING
  THE GRIP.**
- **A FULL-SCREEN PROGRAM MUST HANDLE SIGWINCH, AND ASKING ONCE AT
  STARTUP IS NOT ENOUGH.**
- **A DRAG IS A ROUTER SESSION BETWEEN A SOURCE AND THE WIDGET UNDER THE
  POINTER, AND IT STAYS INSIDE ONE WINDOW**
- **AN EMPTY-SPACE CLICK DESELECTS, AND THE RUBBER BAND WORKS IN EVERY
  VIEW**
- **THE FOLDER TREE FOLLOWS A NAVIGATION, NEVER A TOGGLE**
- **THE DESKTOP'S ICON SIZE IS A NAMED SETTING, THE ICONS ARE CENTRED,
  AND A CAPTION IS TWO LINES**
- **A POPUP OPENED FROM ANOTHER OVERLAY NAMES IT AS ITS PARENT, AND
  `close_others()` SPARES BOTH**
- **A DEFAULT ICON CELL IS CHOSEN AFTER THE SAVED ONES, NOT BEFORE**
- **A SELECTION CHANGE MUST DAMAGE THE RECTS IT CHANGED, NOT JUST SET
  `redraw_pending`**
- **THE WM CONTEXT MENU IS `uui_menubar`, WITH THE PANEL'S ITEM MODEL
  OVER IT, AND THE DESKTOP'S MENU IS WINDOWS' SHAPE**
- **A DRAG BETWEEN WINDOWS IS BROKERED BY THE COMPOSITOR, AND ITS
  PAYLOAD RIDES A SLOT BESIDE THE CLIPBOARD**
- **THE DESKTOP IS `/home/desktop` AND NOTHING ELSE: A `.desktop` FILE
  THERE IS A LAUNCHER, THE APPLICATION DATABASE IS
  `/usr/wm/applications`, AND EVERY VERB IS A CHILD PROCESS**
- **MARKS SURVIVE A RELOAD BY NAME, BECAUSE THE VOLUME'S GENERATION
  NEVER STOPS MOVING**

### Storage, the filesystem, and /etc

`docs/conventions/storage.md`

- **A PATH HAS THREE BOUNDS AND THEY ARE NOT INTERCHANGEABLE:
  `FS_PATH_MAX` (4096) is what a CALL may be handed,
  `FS_PATH_STORED_MAX` (256) is what a STRUCT may remember,
  `FS_NAME_MAX` (255) is one COMPONENT**
- **A PATH BUFFER IS NOT A KERNEL LOCAL -- `kpath_get()`/`kpath_put()`,
  Linux's `getname`/`putname` -- AND `kpath.c` CANNOT ALLOCATE ONE FOR
  YOU**, so `k_path_resolve()` takes a `struct kpath_scratch` from the
  caller (`klineedit.c`'s constraint, same answer); `-Wframe-larger-
  than` is what finds the sites
- **A CONSTANT BORROWED TO MEAN SOMETHING IT DOES NOT NAME BREAKS THE
  FIRST TIME THE THING IT NAMES MOVES**
- **ASK FOR A SCRATCH PATH, NEVER SPELL ONE -- `tmppath(buf, cap,
  TMP_VOLATILE|TMP_PERSISTENT, "name")` (`api/tmppath.h`). Both
  directories are SETTINGS, one registry answers in both rings, and a
  service descriptor says `%T`/`%V`.**
- **`/tmp` IS IN RAM AND `/var/tmp` IS THE DISK, and picking the wrong
  one fails SILENTLY -- anything measuring the disk, or expected to
  survive a reboot, wants the second. Runtime state is `/run`.**
- **A RAMFS MOUNT'S SIZE IS `-o size=`, THEN `storage.ramfs_size`, THEN
  HALF OF FREE -- and the setting's default MUST stay 0, or a diskless
  root gets a /tmp-sized cap.**
- **THE CURRENT DIRECTORY IS THE KERNEL'S, and every path syscall
  resolves against it.**
- **Six filesystem syscalls exist**
- **The disk has a WRITE-BACK CACHE, and its flush can fail**
- **A FACT IS READ THROUGH `SYS_QUERY`, AND ADDING ONE IS A PROVIDER,
  NOT A SYSCALL.**
- **WALKING A LIST CLASS IS `QUERY_FOREACH(cls, var, idx)`, AND THE
  LOOSE LOOPS BESIDE IT ARE A DIFFERENT DECISION, NOT A DIALECT**
- **THERE ARE THREE WORDS FOR SYSTEM STATE AND THEY ARE FIXED: FACT,
  SETTING, TUNABLE.**
- **Setting a setting to the value it already has does NOTHING**
- **A RING-3 PROGRAM READS AND WRITES ONE SETTING THROUGH
  `userland/lib/usetting.h`, AND `usetting_set()` RETURNS THE REGISTRY'S
  THREE-WAY ANSWER.**
- **A CONFIG FILE CAN HAVE `[SECTIONS]`, THE SECTION IS AN ARGUMENT, AND
  A NEW KEY LANDS AT THE END OF ITS OWN SECTION**
- **A `.desktop` OR `mimeapps.conf` FILE READS WITH ITS HEADER OR
  WITHOUT**
- **`etc_config.c` is SPLIT: the parser is shared, the file I/O is
  kernel-only.**
- **EVERY DISK DRIVER RUNS, AND THE ROOT IS A SEPARATE CHOICE**
- **THE DISK PRECEDENCE IS VIRTIO-BLK, THEN AHCI, THEN ATA, and each
  rung has a boot word that steps down to the next**
- **AHCI ENUMERATES EVERY PORT AND DRIVES ONE, AND SAYS SO**
- **ALL THREE DISKS DISCARD, AND THE CAPABILITY IS THE DEVICE'S ANSWER
  RATHER THAN ITS FEATURE BIT**
- **VIRTIO-BLK IS THE PREFERRED DISK; ATA IS THE LEGACY PATH.**
- **A filesystem talks to a `block_device`, not to a disk.**
- **TFS3's last block group may be PARTIAL**
- **A new TFS3 operation must COUNT ITS JOURNAL CREDITS, and the count
  is the design.**
- **Shrinking a file, or anything else that stops referencing a block,
  commits the pointer change BEFORE freeing the bit.**
- **THE STOCK `disk.img` IS PARTITIONED AND BOOTABLE, AND ONLY A BLANK
  IMAGE GETS THAT**
- **REMOVING A FILESYSTEM BACKEND SILENTLY REFORMATS EVERY DISK IN THAT
  FORMAT**
- **`/etc` on the persistent filesystem is the config-file convention.**
- **A RESOLVED PATH IS CACHED, AND `ncache_flush()` IS WHAT INVALIDATES
  IT**
- **`sync` IS `fs_sync()`, IT FLUSHES EVERY MOUNT, AND ZERO SECTORS IS
  NORMAL**
- **`fsync(fd)` IS SCOPED TO THE VOLUME, NOT THE FILE**
- **`storage.sync = batched` HOLDS A TRANSACTION OPEN, AND THREE THINGS
  MUST KEEP IT HONEST**
- **`storage.sync = lazy` TURNS OFF THE JOURNAL'S BARRIERS, AND THAT IS
  ext4's `nobarrier`**
- **A READ THAT CROSSES BLOCKS COALESCES, AND A POINTER TABLE IS CACHED
  PER LEVEL -- BUT ONLY UNTIL THE NEXT WRITE**
- **THE BLOCK LAYER TIMES EVERY OPERATION, AND `clock-granularity-ns`
  SAYS WHETHER TO BELIEVE IT**
- **A PARTITION IS A BLOCK DEVICE, AND THE FILESYSTEM NEVER LEARNS ITS
  OFFSET**
- **A DRIVE'S ROOT IS A PARTITION, OR IT IS RAMFS -- AND NOTHING IS
  AUTO-FORMATTED**
- **RAMFS IS NOT IN `g_backends`, AND PUTTING IT THERE WOULD DESTROY A
  DISK**
- **`init()` IS THREE-VALUED: 1 persistent, 0 mounted-but-not, -1 COULD
  NOT MOUNT**
- **RAMFS HAS A BUDGET, HALF OF FREE MEMORY AT MOUNT**
- **A PARTITIONED DISK IS NEVER AUTO-FORMATTED, AND THE FIRST PARTITION
  THAT IS OURS IS LEFT ACTIVE**
- **THERE IS AN INSTALLER, AND IT IS FIVE ORDINARY OPERATIONS**
- **THE INSTALLER WRITES GPT BY DEFAULT AND MBR ON REQUEST, AND A LEGACY
  BIOS IS WHY**
- **`tools/tfs3_writer.py` WRITES THE WHOLE BLOCK MAP NOW, AND THE CAP
  IT HAD WAS A SECOND IMPLEMENTATION DRIFTING**
- **A DIRECTORY BIGGER THAN ONE LISTING NEEDS `SYS_LISTDIR_AT`**
- **`mkpart` CAN WRITE ANY DISK, AND A DISK NOTHING IS MOUNTED FROM IS
  RE-READ AT ONCE**
- **WRITING A TABLE IS A SYSCALL THAT TAKES A TABLE, NOT A SECTOR**
- **A ROOT IS ONE FILESYSTEM, BUT A PATH TREE IS SEVERAL: THERE IS A
  MOUNT TABLE**
- **FORMATTING A VOLUME IS `SYS_MKFS`, AND A BACKEND MUST PUT ITS OWN
  VOLUME STATE BACK**
- **A BACKEND DECLARES HOW MANY TIMES IT MAY BE MOUNTED, and all three
  say MOUNT_MAX**
- **A BACKEND'S VOLUME STATE IS PER MOUNT, AND THE VFS SAYS WHICH MOUNT
  A CALL MEANS**
- **A PROBE MUST NOT DISTURB A MOUNT, and that contract was only ever
  honoured by accident**
- **`fs_ops.init()` TAKES A DEVICE, and `blk_active()` is not it**
- **FAT32 IS A GENERIC DRIVER AND KNOWS NOTHING ABOUT BOOTLOADERS**
- **`/boot` IS READABLE FROM INSIDE toy-os NOW, AND IT IS THE ESP**
- **TOY-OS BOOTS FROM ITS OWN DISK, AND `/boot` IS FAT32 BECAUSE GRUB
  CANNOT READ TFS3**
- **NEVER LEAVE THE BOOT ORDER OUT OF A QEMU LINE, AND ASK
  `boot_medium()` WHICH ONE**
- **A BLOCK-KEYED CACHE THAT OUTLIVES ONE OPERATION MUST BE FORGOTTEN
  WHEN ITS BLOCK IS FREED**
- **`SYS_WRITE_MAX` SETS THE TRANSACTION COUNT, NOT THE COMMAND SIZE**
- **THE WRITE PATH RESOLVES THROUGH THE PATH CACHE, BECAUSE `resolve()`
  CACHES AND NOT `lookup()`**
- **UNDER `batched`, THE ALLOCATION BITMAP RIDES THE DEFERRED COMMIT**

### The shell, the console, and line editing

`docs/conventions/shell.md`

- **THE MANUAL IS `doc`, THE PAGES ARE THE REPOSITORY'S OWN MARKDOWN,
  AND A CATEGORY IS A DIRECTORY**
- **EVERY COMMAND HAS A PAGE IN `docs/commands/`, AND THE BUILD CHECKS
  IT.**
- **A PAGE IS THE MANUAL `doc` RENDERS, SO IT CARRIES NO HISTORY AND
  LISTS ITS FLAGS**
- **AN EVERYDAY COMMAND IS A `/bin` PROGRAM, NOT A BUILTIN, AND THE
  KERNEL'S OWN COPIES LIVE BEHIND ONE NAME: `rescue`.**
- **A PROGRAM STARTED BY A BARE NAME PRINTS NOTHING EXTRA WHEN IT
  SUCCEEDS -- AND `run <name>` STILL DOES.**
- **TAB COMPLETION IS ONE ENGINE COMPILED TWICE, AND A RING SUPPLIES A
  `struct completion_env`**
- **CTRL-R IS ONE LOOP, COMPILED TWICE, AND HISTORY IS APPENDED**
- **RING-3 HISTORY PERSISTS, AND IT APPENDS RATHER THAN REWRITING**
- **TAB COMPLETION IN COMMAND POSITION IS BUILTINS PLUS ALL OF `PATH`,
  DEDUPLICATED AND SORTED, WITH NO DIRECTORIES.**
- **`/bin/tosh -c <command>` RUNS ONE LINE AND EXITS, WITH A SHELL'S
  EXIT STATUS**
- **`#` IS RING 0 AND `$` IS RING 3, AND THE PROMPT IS WHERE THAT
  LIVES**
- **A COMMAND LINE IS NOT A PATH, AND SIZING IT LIKE ONE TRUNCATES IT**
- **A BUILTIN MUST NOT SHADOW A `/bin` PROGRAM THAT DOES MORE**
- **A WRAPPER BUILTIN IS ONE COMMAND WITH TWO HALVES IN TWO RINGS, AND
  THE RING-3 HALF WILL BE WRONG.**
- **A COMMAND WITH A READ HALF AND A WRITE HALF MOVES AS ONE PIECE OR
  NOT AT ALL.**
- **COLOUR IS AN ESCAPE SEQUENCE, NOT A SYSCALL.**
- **`edit` IS A `/bin` PROGRAM, AND THE KERNEL DRAWS NOTHING**
- **`cp` EXISTS NOW, AND COPYING IS A PROGRAM RATHER THAN A SYSCALL**
- **Ctrl-L CLEARS IN BOTH SHELLS NOW, AND THE COMMENT THAT STOPPED IT
  WAS TRUE WHEN IT WAS WRITTEN.**
- **THERE IS AN ALTERNATE SCREEN, AND IT IS WHY A PAGER LEAVES NO
  WRECKAGE.**
- **`dmesg` IS A `/bin` PROGRAM, AND THE LOG LEAVES THE KERNEL THROUGH
  `QUERY_KLOG`.**
- **A JOB IS A PROCESS GROUP, AND THE JOB TABLE IS THE SHELL'S**
- **A TERMINAL IS AN OBJECT, AND THE CONSOLE IS `tty0`**
- **EVERY ORDINARY BOOT REACHES `/bin/tosh`, AND THE KERNEL SHELL IS THE
  `rescue` TARGET**
- **RING 3 CAN READ THE CONSOLE -- fd 0, and it BLOCKS.**
- **A QMP TEST THAT TYPES PUNCTUATION MUST PIN THE GUEST'S KEYBOARD
  LAYOUT.**
- **RING 0's BLOCKING KEYBOARD READERS ARE SUSPENDED WHILE A COMPOSITOR
  HOLDS THE ROLE**
- **QUOTING IS DECIDED TWICE -- tosh's LEXER AND THE KERNEL'S SPLIT OF
  THE STRING FORM -- AND THE TWO MUST AGREE**

### The build, the userland layout, and releases

`docs/conventions/build.md`

- **A `.d` FILE MUST NEVER BE REMAKEABLE, OR make BUILDS THE WRONG FILE
  AND STILL EXITS 0**
- **mtools DOES NOT READ stdin -- IT OPENS `/dev/tty`, so a CAPTURED
  PROMPT HANGS FOREVER**
- **THE C LIBRARY IS CALLED `tolibc`, and its bar for adding a function
  is the OPPOSITE of everything else here -- it aims to be COMPLETE.**
- **REGEX IS `<regex.h>` IN tolibc, AND IT IS AN NFA**
- **WHEN IMPLEMENTING A SPEC, DISAGREE WITH AN INDEPENDENT
  IMPLEMENTATION ON PURPOSE**
- **`tolibc` GREW A SECOND PORT'S WORTH OF FUNCTIONS, AND ONE OF THEM
  WAS A BUG**
- **`SYS_WRITE_MAX` IS A THROUGHPUT CONSTANT, NOT JUST A BUFFER SIZE,
  AND IT IS 64 KiB**
- **`sys_write()` COMPLETES THE WHOLE BUFFER, because the kernel caps
  one write at `SYS_WRITE_MAX` (1024) and a short write loses data
  SILENTLY.**
- **AN UNRECOGNISED printf CONVERSION DESYNCHRONISES EVERY ARGUMENT
  AFTER IT, AND `kfmt_cases.h` IS THE TABLE THAT STOPS A FOURTH ONE.**
- **WRITE C LIBRARY NAMES, AND REACH FOR `sys_*` ONLY WHERE THERE IS NO
  EQUIVALENT**
- **THE POSIX HALF OF `tolibc` IS HEADERS OVER SYSCALLS THAT ALREADY
  EXIST**
- **`userland/` is split by ROLE, and the build derives things from it
  -- adding a program is a `.c` file and nothing else.**
- **In ring 3 the toolkit is reachable under the C names -- don't hand-
  roll a `my_strlen` or a digit loop there either.**
- **EVERY RING-3 PROGRAM CARRIES A TLS BLOCK, AND `crt0` INSTALLS IT
  BEFORE `main()`**
- **RING-3 CODE HAS A FRAME BUDGET, and a link-time bound on the
  image.**
- **READING A WHOLE FILE IS `lib/ufile.h`, AND THE PART IT EXISTS FOR IS
  THE LOOP**
- **A SELF-CHECKING `/tests` PROGRAM REPORTS THROUGH
  `userland/lib/utest.h`, AND ITS EPILOGUE IS ONE LINE IN ONE SHAPE**
- **Every ring-3 program is just a `main()`.**
- **WHAT GOES ON THE MEDIA IS `$(KERNEL_MEDIA)`, THE KERNEL GZIPPED, AND
  GRUB DECOMPRESSES IT**
- **`drivers.conf` IS THE KERNEL CONFIG NOW -- `option <name> = <value>`
  BESIDE THE DRIVER LINES** (FreeBSD's `conf/GENERIC`, Linux's
  `.config`): `strip`, `compress`, `cmdline`, `grub_timeout`, `extras`.
  An unknown option FAILS THE BUILD by name. **Its include is at the TOP
  of the Makefile and that is load-bearing**
- **`STRIP=0` KEEPS THE KERNEL'S DEBUG INFO IN `kernel.bin` AND
  `COMPRESS=0` SHIPS THE LIVE IMAGE PLAIN; BOTH DEFAULT ON, AND BOTH
  NEED THEIR STAMP FILE**
- **THE KERNEL'S DEBUG INFO IS SPLIT OUT (`build/kernel.debug`), AND
  `--add-gnu-debuglink` IS WHAT KEEPS `addr2line`, `gdb` AND
  `panic_resolve.py` WORKING**
- **`linker.ld` decides kernel memory PERMISSIONS, not just placement.**
- **CI RUNS THE KERNEL SUITE TWICE, on ATA and on virtio-blk, and the
  second one earns its place.**
- **A graphics card is a `display_driver`, not a special case.**
- **`kernel/` directories are subsystems, not filing cabinets**
- **`kernel/include/api/version.h` is GENERATED, not hand-edited**
- **Versioning is semver + a `-dev` suffix, not a per-change build
  number.**
- **A SHARED LIBRARY IS `userland/dynlib/` PLUS ONE MAKEFILE LINE, AND A
  PROGRAM OPTS IN**
- **A FLASH REPLACES THE KERNEL, NEVER THE BOOTLOADER -- AND `install
  --bootloader` IS HOW A MACHINE GAINS ONE**
- **THE BARE-METAL KERNEL IS REPLACED WITH `remote.py flash`, AND THE
  RESCUE ENTRY NEEDS A GRUB TIMEOUT**
- **dash's LINE EDITING IS A libedit SHIM, NOT A SECOND EDITOR**
- **A GitHub Release's notes follow ONE shape, and it is terse.**

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
- **DON'T ADD A WAIT LOOP FOR WORK THAT IS ALREADY IN THE BACKGROUND --
  and never `pgrep` for a pattern your own command line contains.** That
  shell's command line matches the waiter itself, so the loop can never
  exit, reporting a finished suite as still RUNNING. A backgrounded
  command's own completion notification IS the signal. If a wait
  genuinely is needed, wait on an ARTIFACT that MUST come to exist --
  twelve wait-loops were left running in one session, several unable to
  exit at all.
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
| Drive a VM | `vm.py` (text in, text out), `qmp_test.py`, `gui_debug.py`, `gui_flow.py`, `shell_flow.py`, `serial_console.py`, `serial_capture.py`, `watch_vm.sh`, `run_release.sh` |
| Is it INTERMITTENT, and at what rate? | `boot_rate.py` (bare metal), `flake_hunt.py` (VM) |
| Test runners | `boot_smoke_test.py`, `ktest_run.py`, `usertest_run.py`, `faulttest_run.py`, `gui_regress.py`, `damage_sweep.py`, `damage_hunt.py` |
| Diagnose | `panic_resolve.py` (**never hand-roll `nm`**), `acpi_dump.py`, `aml_walk.py`, `QMPSession.hmp()` (**the one oracle the guest cannot fake**), `corrupt_diff.py`, `window_resize_probe.py`, `pixel_probe.py`, `screenshot_diff.py`, `iso_guard.py` |
| Check an implementation against a FOREIGN one | `libc_diff.py`, `uimg_codec_hostcheck.py`, `usnd_hostcheck.py`, `hash_hostcheck.py`, `divti3_hostcheck.py`, `regex_hostcheck.py`, `umd_hostcheck.py`, `ugfx_text_hostcheck.py`, `utext_hostcheck.py`, `term_scheme_hostcheck.py` |
| Measure | `idle_cpu.py` (quote DIFFERENCES only), `loc.py`, `ping_rtt.py`, `latency_under_io.py`, `frame_balance.py` |
| Disk images, from the host | `seed_disk.py`, `install_grub.py` (also `boot_medium()`), `tfs3_writer.py`, `mkpart_test.py`, `fetch_wad.py` |
| Generated data | `gen_version.sh`/`set_version.sh`, `genfont.py`, `genttf.py`, `gen_kbs.py`, `gen_cursors.py`, `gen_icons.py`, `gen_imgdata.py`, `gen_audio.py`, `gen_music.py`, `gen_mp3_tables.py`, `gen_signames.py`, `genrelocs.py`, `gen_syms.py`, `drivers_conf.py`, `gen_modalias.py`, `gen_decisions_index.py`, `gen_commands_index.py`, `gen_next_up.py`, `fetch_ca_bundle.py` |
| The repo itself | `backup_repo.sh` -- run before ANY change to the repo's identity or history |

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

`blank_window_test.py`, `brightness_test.py`, `calculator_client_test.py`,
`calendar_test.py`, `clipboard_test.py`, `compositor_death_test.py`,
`compositor_test.py`, `crashtest_test.py`, `cursor_theme_test.py`,
`damage_hunt.py`, `damage_sweep.py`, `desktop_entries_test.py`,
`dialog_test.py`, `filedialog_test.py`, `filemanager_test.py`,
`font_test.py`, `forcequit_test.py`, `fullscreen_test.py`,
`gfxdemo_test.py`, `hover_test.py`, `icons_test.py`,
`idle_desktop_test.py`, `imgview_test.py`, `keyup_test.py`, `kvm_soak.py`,
`menubar_test.py`, `mines_test.py`, `modeset_test.py`,
`network_tray_test.py`, `notepad_client_test.py`, `osk_test.py`,
`pager_test.py`, `player_test.py`, `popup_test.py`,
`resize_stride_test.py`, `sched_gui_test.py`, `screen_surface_test.py`,
`screensaver_test.py`, `screenshot_test.py`, `scrollbar_test.py`,
`settings_test.py`, `single_instance_test.py`, `taskmgr_test.py`,
`uapp_test.py`, `uiclient_test.py`, `uidemo_test.py`, `uterm_test.py`,
`vm.py`, `volume_test.py`, `wallpaper_mode_test.py`, `winclient_test.py`,
`window_geometry_test.py`

*Run by `ondemand_sweep.py` (on demand, never a gate):*

`ahci_test.py`, `ansi_cursor_test.py`, `audio_test.py`, `boot_rate.py`,
`console_bleed_test.py`, `console_shell_test.py`, `ctrlc_test.py`,
`cursor_ibeam_test.py`, `dash_gap.py`, `dash_test.py`, `diskmark_test.py`,
`divti3_hostcheck.py`, `doc_test.py`, `doom_sound_test.py`,
`doom_test.py`, `fat32_test.py`, `fetch_wad.py`,
`filemanager_harness_hostcheck.py`, `fileop_test.py`, `flake_hunt.py`,
`fs_switch_test.py`, `grep_test.py`, `gui_regress.py`, `guictl_test.py`,
`hash_hostcheck.py`, `highmem_consume.py`, `highmem_test.py`,
`hires_test.py`, `https_test.py`, `hwdata_test.py`, `init_test.py`,
`install_test.py`, `jobs_test.py`, `kbd_test.py`,
`keyboard_paths_test.py`, `ktest_run.py`, `latency_under_io.py`,
`libc_diff.py`, `live_boot_test.py`, `ls_test.py`, `mkpart_test.py`,
`module_test.py`, `msi_test.py`, `multidisk_test.py`, `net_test.py`,
`ntp_test.py`, `partition_test.py`, `ping_rtt.py`, `pixel_probe.py`,
`poweroff_test.py`, `predates.py`, `preflight.sh`, `qemu_matrix.py`,
`remote.py`, `remote_test.py`, `serial_backpressure_test.py`,
`settings_harness_hostcheck.py`, `soundd_test.py`, `stdin_test.py`,
`sum_test.py`, `taskbar_test.py`, `term_scheme_hostcheck.py`,
`terminal_probe.py`, `tfs3_v1_test.py`, `ugfx_text_hostcheck.py`,
`uimg_codec_hostcheck.py`, `uimg_hostcheck.py`, `umd_hostcheck.py`,
`usb_audio_test.py`, `usb_test.py`, `usnd_hostcheck.py`,
`utext_hostcheck.py`, `virtio_boot_test.py`, `virtio_gpu_test.py`,
`virtio_input_test.py`

*Libraries, generators and drivers -- named by no runner:*

`acpi_dump.py`, `aml_walk.py`, `backup_repo.sh`, `corrupt_diff.py`,
`drivers_conf.py`, `faulttest_run.py`, `fetch_ca_bundle.py`,
`fetch_extras.py`, `frame_balance.py`, `gen_audio.py`,
`gen_commands_index.py`, `gen_cursors.py`, `gen_decisions_index.py`,
`gen_icons.py`, `gen_imgdata.py`, `gen_kbs.py`, `gen_modalias.py`,
`gen_mp3_tables.py`, `gen_music.py`, `gen_next_up.py`, `gen_signames.py`,
`gen_syms.py`, `gen_version.sh`, `genfont.py`, `genrelocs.py`,
`gui_debug.py`, `gui_flow.py`, `idle_cpu.py`, `install_grub.py`, `loc.py`,
`mem_stress.py`, `panic_resolve.py`, `port_guard.py`, `qmp_test.py`,
`regex_hostcheck.py`, `run_release.sh`, `screenshot_diff.py`,
`seed_disk.py`, `serial_capture.py`, `serial_console.py`,
`set_version.sh`, `shell_flow.py`, `tfs3_writer.py`, `watch_vm.sh`,
`window_resize_probe.py`

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
`dynlink-design.md`, `signals-design.md` (read before Phase 1's
signal/TTY/job-control work -- its point is that those are ONE problem),
`blocking-design.md` (**read before touching `switch_to()`,
`block_common()` or `FS_OP()`**), `smp-design.md` (stage 1 BUILT; **read
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
