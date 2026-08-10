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
a preemptive scheduler, a kernel-space window manager, and a
disk-backed filesystem. No cross-compiler needed -- host and target are
both x86-64, so plain system `gcc`/`ld`/`nasm` with freestanding flags
work.

## Conventions worth knowing before editing

- **`kapi.h` is the one header apps include** for kernel capabilities
  (console, keyboard, mouse, timer/RTC, filesystem, graphics). Never
  `#include` a driver header directly from `apps/`, never call
  `inb`/`outb` from app code. If a capability isn't in `kapi.h` yet,
  that's a sign it belongs behind a new function in `kernel/core` or
  `kernel/drivers`, exposed through `kapi.h` -- not a reason to reach
  around the boundary.
- **`apps/wm/wm.h` is a second, peer-level boundary**, not part of
  `kapi.h` -- it's the GUI-specific equivalent, included by GUI apps
  for `window_*` helpers. `kapi.h` never includes `wm/wm.h` or
  `gui_apps.h`.
- **`apps/widgets.h`** (`widget_hit`/`widget_button`) and
  **`apps/theme.h`** (`THEME_*` named colors) are small apps-internal
  helpers, same peer-level pattern as `wm/wm.h`. Both are deliberately
  minimal on purpose -- see their top comments before adding to them.
  Add a new widget primitive or theme color only once a second real
  caller needs it, not preemptively.
- **The window manager lives in `apps/wm/`** (`wm.c`/`wm_input.c`/
  `wm_render.c`/`wm_internal.h`), split by concern for readability --
  it's still one tightly-coupled event loop sharing state through
  `wm_internal.h`'s `extern`s, not decoupled components. See
  `apps/wm/wm.c`'s top comment.
- **`/etc` on the persistent filesystem is the config-file convention**
  (`kernel_main()` creates it right after `fs_init()`, before anything
  that might read a config file runs) -- see `kernel/core/tz.c` for the
  first example (`/etc/timezone`). Put any future config file there
  too, not at the filesystem root or somewhere app-specific.
- Every non-trivial change so far has gotten a `CHANGELOG.md` entry in
  the same style: what changed, why, and what was verified. Keep doing
  that -- it's the project's record of *why* things are the way they
  are, which matters a lot in a codebase this hand-rolled.
- **`kernel/include/version.h` is GENERATED, not hand-edited** --
  `tools/gen_version.sh` regenerates it automatically as the first step
  of `make all`/`make iso`, embedding whatever `BUILD_NUMBER` (repo
  root, plain integer) currently holds. Never edit `version.h` directly
  -- it'll just get overwritten on the next build.
- **Bump `BUILD_NUMBER` once per real change, via `tools/bump_build.sh
  <fix|feature|major>`** -- do this as part of finishing a change,
  *before* the final build/deliver, not on every `make` invocation (see
  the script's top comment for the three tiers and their deltas:
  fix +1, feature +10, major +50). This is a Windows-build-number-style
  scheme (see CHANGELOG for the request and the design choices behind
  it) that replaced an earlier date-plus-same-day-counter scheme
  (`YYYY.MM.DD.N`, before that a hand-bumped `0.1.0`-style semver --
  see CHANGELOG for both). Reference the new number and tier in the
  CHANGELOG entry for the change, e.g. "Build 110 (feature, +10): ...".
  Don't bump `BUILD_NUMBER` by hand or skip `bump_build.sh` -- picking
  the right tier and writing it down in the changelog is the whole
  point of this scheme over the fully-automatic one it replaced.
- **Every commit that lands a `BUILD_NUMBER` bump on GitHub gets a
  matching git tag, `build-<N>`** (e.g. `build-120`) -- added once the
  repo went from local-only to actually pushed (see CHANGELOG). Makes
  "what commit was build 120?" answerable with `git show build-120`
  instead of digging through commit dates against CHANGELOG.md. Applies
  to every tier (fix/feature/major), not just the big ones -- tag it as
  part of the same push:
  ```
  git tag build-<N>
  git push origin main --tags
  ```
  **Major (+50) bumps additionally get a GitHub Release**, title =
  `build-<N>`, body = that bump's CHANGELOG section, with the built
  `.iso` attached as a downloadable asset (`gh release create build-<N>
  toy-os.iso --title "build-<N>" --notes-file <path>`, or the GitHub
  web UI) -- so grabbing a working ISO of a real milestone doesn't
  require cloning + building. Fix/feature bumps get the tag above but
  no release; the ISO for those is easy enough to build locally that a
  standing download isn't worth a release per small bump.

## Working in the cloud sandbox vs. the user's machine

This repo is normally edited from a Cowork cloud session with the
user's real checkout reachable through the device bridge
(`mcp__remote-devices__*`), not directly. Two things about that setup
that aren't obvious until you hit them:

- **`Makefile` is a protected file** -- `device_commit_files` will
  reject writes to it. Edit it in the cloud sandbox as normal, verify
  the build there, then deliver it to the user as `Makefile.new` via
  `SendUserFile` and tell them to copy it over `Makefile` themselves.
- **The device bridge can't delete files** -- `device_bash`'s `rm`/
  `rmdir`/`unlink` fail with "Operation not permitted" on mounted
  files, and `device_commit_files` only writes. To remove a
  now-superseded file from the user's machine, `mv` it (via
  `device_bash`) into a `_to_delete/` subfolder next to it, then tell
  the user which folder to delete themselves.
- Build and test in the cloud sandbox first (`make clean && make all
  && make iso`), confirm it's clean, *then* deliver + commit files to
  the user's machine. Don't commit unverified changes.

## Building

```
make all   # kernel.bin + userland test ELFs
make iso   # + toy-os.iso (grub-mkrescue)
make run   # boots in QEMU with an SDL window (the user's machine, not usable headlessly)
```
`apps/*.c` is picked up by a `wildcard`, but it's non-recursive --
`apps/wm/*.c` needed its own `WM_C` wildcard, pattern rule, and mkdir
target when that subfolder was added. If you add another subfolder
under `apps/`, it'll need the same treatment.

**`make run` uses `-display sdl,grab-mod=rctrl`, no explicit pointer
device.** Two things worth knowing if you ever touch this line:
`grab-mod` (the key that captures/releases the mouse once grabbed,
here right Ctrl) is an SDL-only display option -- QEMU rejects it
outright on `gtk` ("Parameter 'grab-mod' is unexpected"), which is why
this isn't `-display gtk,...` even though gtk was tried first. And
deliberately NO `-device usb-tablet`/`-device usb-mouse` -- this
kernel's mouse driver only speaks PS/2 (see `kernel/drivers/mouse.c`),
there's no USB stack at all, and adding an explicit USB pointer device
makes QEMU route host mouse motion to THAT instead of the emulated
PS/2 mouse, so the guest receives nothing and the cursor just never
moves. Bit an actual user session once (see CHANGELOG.md around build
293's Makefile fix) -- looked exactly like a driver bug, wasn't one.

**Always `make clean && make all` before testing a GUI change, not a
plain `make all`.** The Makefile doesn't track header dependencies (no
`-MMD`/`-MP`; see its `version:` target comment) -- editing a shared
header like `apps/widgets.h` or `apps/gui_apps.h` doesn't trigger a
rebuild of every `.o` that includes it, only the ones whose `.c` file
also changed. A stale `.o` still compiled against the OLD struct
layout, sitting next to freshly-rebuilt ones that see the NEW layout,
silently desyncs (e.g. an array indexed with the wrong element stride).
This produced a genuinely bizarre-looking bug once -- the Start menu's
item labels showed raw function-prologue machine code reinterpreted as
text -- that a clean rebuild fixed instantly, no code changes needed.
If a GUI test shows something inexplicable right after touching a
header, suspect a stale build before suspecting the new code.

## Testing in QEMU headlessly, via QMP

There's no interactive display in this environment, so GUI testing
goes through QEMU's QMP socket: launch headless, drive keyboard/mouse
via QMP commands, `screendump` to prove it visually.

**Use `tools/qmp_test.py` -- don't rederive this from scratch.** It's a
committed, working helper module (`QMPSession`, with `goto()`/`click()`/
`drag()`/`wheel()`/`mouse_down()`/`mouse_up()`/`recalibrate()`/
`send_key()`/`send_text()`/`screenshot()`) built from exactly this kind
of testing, with the gotchas below already handled. Past sessions each
independently hand-rolled similar scripts in the cloud sandbox (never
committed, so lost between sessions) and paid the cost of hitting these
gotchas fresh each time -- that's why this module exists now. Import it
(`sys.path.insert(0, "tools"); from qmp_test import QMPSession`) rather
than writing new inline socket/JSON code, and add to it (rather than
writing a one-off script) if you need a capability it doesn't have yet.

The gotchas it already gets right, for when you need to know why:

- **Launch with `setsid nohup ... & ); disown -a`**, not a plain `&` --
  a bare background job tied to one Bash tool call's shell gets killed
  when that call returns. `setsid` detaches it so it survives across
  tool calls. (`tools/qmp_test.py`'s `launch_qemu_cmd()` returns the
  command to background this way.)
- **Use `-serial file:/path/to/serial.log`, not `-serial stdio`** for
  most testing (kernel boot/test output is easy to `tail`). But note
  some userland tests (`echotest`) block forever reading from the
  serial port when it's a plain file with nothing on the other end --
  that's an environment limitation of headless testing, not a kernel
  bug, if a test hangs at "calling process_run_ring3()" with no
  further output.
- **Do NOT pass `-display none`.** It disables the display head
  entirely, which silently breaks `input-send-event` mouse routing --
  clicks and moves return `{"return": {}}` (success) but never reach
  the guest. Use `-vga std -vnc :N` (no `-display none`) instead; VNC
  doesn't need an actual client connected, it just needs to exist as a
  head for input routing to work.
- **Mouse input:** this kernel's mouse driver is PS/2, not USB HID --
  never add `-device usb-tablet` OR `-device usb-mouse` to a headless
  test launch (same reasoning as `make run`'s comment above -- it's
  not just a "wrong device" ergonomics thing, it actively breaks
  routing). `QMPSession.goto()`/`click()`/`drag()` use
  `input-send-event` with `rel` axis events against the default
  emulated PS/2 mouse, tracking cursor position client-side since
  there's no absolute cursor query. `wheel()` sends synthetic
  `wheel-up`/`wheel-down` button press/release pairs -- QEMU's
  IntelliMouse PS/2 emulation reports the scroll wheel that way, there
  is no separate scroll event type.
- **Cursor position drifts across separate `QMPSession`s that share an
  already-open GUI session** (a previous script left GUI mode running
  instead of pressing Esc back to the shell). Each new session assumes
  the cursor starts at (640, 360) without querying the guest's real
  position, so if the real cursor moved since, every `goto()` lands
  offset from where it should -- looks exactly like a click "not
  registering." Fix: call `session.recalibrate()`, but ONLY in scripts
  that are reusing an already-open GUI session rather than entering it
  fresh -- and if a script does both (enters GUI mode itself, and
  wants to recalibrate), the recalibrate call must come AFTER "gui" +
  Enter, never before (the guest's mouse device isn't even enabled
  until `mouse_init()` runs as part of entering GUI mode, and that
  same call resets the cursor to screen center deterministically,
  which is why (640, 360) is the default in the first place). See
  `recalibrate()`'s own docstring for the full reasoning -- getting
  this ordering backwards was a real mistake in an earlier session,
  worth not repeating.
- **Keyboard:** `send-key` with `{"type":"qcode","data":"<key>"}`,
  one character/qcode at a time (`QMPSession.send_key()`/`send_text()`).
  `send_text()` only handles lowercase letters/digits -- for space use
  `send_key('spc')`, for punctuation the matching qcode name, and there
  is no way to send an uppercase letter distinct from lowercase (no
  shift handling) as of this writing.
- **Screenshots:** `screendump` writes a `.ppm`; `QMPSession.screenshot()`
  converts to `.png` via Pillow in one call so it's ready for the Read
  tool / `SendUserFile`.

The project's standing instruction is to include screenshots as proof
whenever testing happens this way -- send them with `SendUserFile`,
don't just describe what the screenshot showed. Also save a handful of
the representative ones into `screenshots/YYYY-MM-DD/` (today's date,
not `TOYOS_VERSION` -- see `screenshots/README.md`) with descriptive
filenames, not the generic `shot13.png`-style names a testing session
naturally produces. This is in addition to sending them to the user,
not instead of it.

## tools/

Dev/build helper scripts, not compiled or shipped as part of the OS:
`genfont.py`/`genttf.py` (font generation, pre-existing), `qmp_test.py`
(QEMU/QMP testing helpers, see above), `gen_version.sh`/
`bump_build.sh` (build-number versioning, see the `version.h` bullet
above). Add new tools here freely when something would save a future
session real time -- the bar is "does this fix a rederive-from-scratch
cost," the same reasoning that produced `qmp_test.py`.

## Delivering changes

List every file added or edited in the final response (standing
project instruction). Deliver files via `SendUserFile` +
`mcp__remote-devices__device_commit_files` to the user's real checkout
-- editing only the cloud sandbox copy doesn't reach the user's
machine on its own.
