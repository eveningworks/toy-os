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
make run   # boots in QEMU with a GTK window (not usable headlessly)
```
`apps/*.c` is picked up by a `wildcard`, but it's non-recursive --
`apps/wm/*.c` needed its own `WM_C` wildcard, pattern rule, and mkdir
target when that subfolder was added. If you add another subfolder
under `apps/`, it'll need the same treatment.

## Testing in QEMU headlessly, via QMP

There's no interactive display in this environment, so GUI testing
goes through QEMU's QMP socket: launch headless, drive keyboard/mouse
via QMP commands, `screendump` to prove it visually.

**Use `tools/qmp_test.py` -- don't rederive this from scratch.** It's a
committed, working helper module (`QMPSession`, with `goto()`/`click()`/
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
  don't bother with `-device usb-tablet`/absolute positioning, it's
  the wrong device for this guest. `QMPSession.goto()`/`click()` use
  `input-send-event` with `rel` axis events and track cursor position
  client-side, since there's no absolute cursor query.
- **Keyboard:** `send-key` with `{"type":"qcode","data":"<key>"}`,
  one character/qcode at a time (`QMPSession.send_key()`/`send_text()`).
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
