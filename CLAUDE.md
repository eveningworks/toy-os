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
- **`apps/ui/ui.h`** (the umbrella include for `ui_primitives.h`'s
  `widget_hit`/`widget_button`, `ui_scrollback.h`, `ui_scrollbar.h`,
  `ui_checkbox.h`, `ui_button.h`/`ui_button_group.h`, `ui_textbox.h`)
  and **`apps/theme.h`** (`THEME_*` named colors) are small apps-internal
  helpers, same peer-level pattern as `wm/wm.h`. Both are deliberately
  minimal on purpose -- see their top comments before adding to them.
  Add a new widget primitive or theme color only once a second real
  caller needs it, not preemptively. (`apps/widgets.h`/`.c` -- the
  single file all of `apps/ui/` was split out of -- no longer exists;
  see `docs/decisions.md`.)
- **The window manager lives in `apps/wm/`** (`wm.c`/`wm_input.c`/
  `wm_render.c`/`wm_internal.h`), split by concern for readability --
  it's still one tightly-coupled event loop sharing state through
  `wm_internal.h`'s `extern`s, not decoupled components. See
  `apps/wm/wm.c`'s top comment.
- **Split a file once it's grown big enough to be genuinely harder to
  work with, the same call that produced the `apps/wm/` split above --
  don't wait for it to become unmanageable, but don't split
  preemptively either.** There's no hard line-count rule; the signal is
  practical: a file mixing more than one real concern (e.g. event
  handling + rendering, like `wm.c` before its split), or long enough
  that finding/editing the right part of it gets slow and error-prone.
  As a rough reference point, every hand-written file in this repo is
  currently under 800 lines (`apps/shell.c` is the largest at 777) and
  that's comfortable -- a hand-written file pushing toward a couple
  thousand lines is the point to seriously consider it, not a hard
  trigger. This deliberately excludes *generated* data files like
  `kernel/drivers/font_ttf.c` (11,800+ lines of baked glyph data) --
  splitting those for line count alone would miss the point; the
  concern there is regenerating them correctly (`tools/genttf.py`), not
  readability. When a split does make sense: follow the `apps/wm/`
  pattern (split by concern, share state through a `_internal.h` of
  `extern`s if it's still fundamentally one component, not a real
  boundary -- see `docs/decisions.md`'s entry on this) rather than
  inventing a new pattern each time, and record the split's own
  reasoning in a top-of-file comment the way `apps/wm/wm.c` and
  `kernel/drivers/tfs.c` do. `CHANGELOG.md` itself is the same
  instinct applied to docs, not code -- split by era once it got
  unwieldy to search: `CHANGELOG-archive.md` holds Milestone 1 through
  Build 172 (moved out at ~4,200 lines), `CHANGELOG.md` keeps Build 173
  onward plus `## [Unreleased]`. Both stay grep-able; if this ever
  needs a second split, follow the same pattern (cut at one heading,
  straight move, no rewording) rather than inventing a new one.
- **`/etc` on the persistent filesystem is the config-file convention**
  (`kernel_main()` creates it right after `fs_init()`, before anything
  that might read a config file runs). Don't hand-roll a parser for a
  new setting -- read/write it through `kernel/include/etc_config.h`'s
  `etc_config_get()`/`etc_config_set()` (name=value lines, `#` comments,
  see `kernel/core/etc_config.c`'s top comment for the exact format).
  By default, put a new setting's key in the shared `/etc/toyos.conf`
  every setting lives in today (`timezone`, `font_size` -- see
  `kernel/core/tz.c`/`font_config.c` for the pattern: a small
  `*_init()` called from `kernel_main()` that loads via
  `etc_config_get()`, and a `*_save()`/`*_set_*()` that writes via
  `etc_config_set()`). `etc_config_*()` takes a `path` on every call,
  though -- nothing forces one shared file. A setting with enough of
  its own keys to be unwieldy sharing `toyos.conf` (a GUI app with a
  dozen preferences, say) should get its own `/etc/<name>.conf` instead
  of cramming into the shared one just to match convention.
- Every non-trivial change so far has gotten a `CHANGELOG.md` entry in
  the same style: what changed, why, and what was verified. Keep doing
  that -- it's the project's record of *why* things are the way they
  are, which matters a lot in a codebase this hand-rolled.
- **`kernel/include/version.h` is GENERATED, not hand-edited** --
  `tools/gen_version.sh` regenerates it from `VERSION` (repo root, e.g.
  `0.1.0-dev`) as the first step of `make all`/`make iso`. Never edit
  `version.h` directly.
- **Versioning is semver + a `-dev` suffix, not a per-change build
  number.** `VERSION` only changes via `tools/set_version.sh
  <version>`: `0.2.0-dev` starts a new dev round, `0.2.0` (no `-dev`)
  cuts a release and also stamps `CHANGELOG.md`'s `## [Unreleased]`
  section with the version + date, opening a fresh one above it. Git
  tags (`v<version>`) and GitHub Releases happen at real releases only,
  cut by hand after `set_version.sh` -- see `docs/decisions.md` for the
  full mechanics, commands, and why this replaced the old
  `tools/bump_build.sh <fix|feature|major>` scheme.
- **Commit messages list each changed/added file with a one-line note
  in the body** (subject line stays a short summary) -- see
  `docs/decisions.md`'s versioning entry for the exact format.

## Working in the cloud sandbox vs. the user's machine

This repo is normally edited from a Cowork cloud session with the
user's real checkout reachable through the device bridge
(`mcp__remote-devices__*`), not directly. Two things about that setup
that aren't obvious until you hit them:

- **`Makefile` and anything under `.github/workflows/*.yml` are
  protected against `device_commit_files`** (writes get rejected --
  check its response's `rejected` array, don't assume a batch landed
  in full). Edit + verify in the cloud sandbox as normal, deliver as
  `Makefile.new` / `build.yml.new` via `SendUserFile` +
  `device_commit_files`, then apply it yourself over `device_bash`
  (`cp Makefile.new Makefile`, `diff` to confirm, move `Makefile.new`
  into `_to_delete/`) -- `device_bash` is NOT blocked from writing
  these files directly, only `device_commit_files` is. See
  `docs/decisions.md` for the full mechanics.
- **The device bridge can't delete files, and `git` run through it
  leaves stale `.git/index.lock` files behind** (even a read-only
  `git status`). Both fixed the same way -- `mv`, not `rm`. To remove a
  file, `mv` it into a `_to_delete/` subfolder and tell the user to
  delete that folder themselves. For git, **always use
  `tools/device_git.sh`**, never run `git` directly via `device_bash`
  -- it sweeps stale locks both before and after the real command, so
  the repo comes back lock-free. See `docs/decisions.md` for why this
  is needed even for reads.
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
target when that subfolder was added, and `apps/ui/*.c` got the same
treatment (`UI_C`) when that subfolder was added later. If you add
another subfolder under `apps/`, it'll need the same treatment.

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

**A plain `make all` is safe after editing a shared header now** (as of
build 308) -- the Makefile tracks header dependencies (`-MMD`/`-MP`;
see CFLAGS/USERLAND_CFLAGS and the `-include` line near `$(KERNEL)`'s
rule), so editing e.g. `apps/ui/ui_scrollback.h` correctly rebuilds
every `.o` that includes it, not just the ones whose own `.c` file
changed. This
used to not be true, and it produced a genuinely bizarre-looking bug
once -- the Start menu's item labels showed raw function-prologue
machine code reinterpreted as text -- caused by exactly the failure
mode dependency tracking now prevents: a stale `.o` compiled against
an OLD struct layout sitting next to freshly-rebuilt ones that saw the
NEW layout (e.g. an array indexed with the wrong element stride). A
`make clean && make all` is still a reasonable "when in doubt" move if
a GUI test ever shows something inexplicable right after a header
change (dependency tracking is only as good as the `.d` files being
correct), but it should no longer be *routine* -- if you find yourself
needing it regularly, that's a sign the tracking broke somehow, worth
investigating rather than working around. One subtlety if you ever
touch `tools/gen_version.sh`: it's deliberately idempotent (only
rewrites `kernel/include/version.h` when `VERSION`'s value actually
changed) specifically so this dependency tracking doesn't regress --
`kapi.h` includes `version.h`, so an unconditional rewrite every build
would make every file that includes `kapi.h` (nearly everything) look
"out of date" and rebuild every single time.

**`tools/boot_smoke_test.py`** -- a fast, non-GUI boot check: boots
`toy-os.iso` headlessly, watches `serial.log` for the expected kernel
init sequence (or a `PANIC:`), exits 0/1 in a few seconds. No QMP, no
mouse/keyboard, no screenshots. Use this as the first check for a
kernel/driver-level change (a new driver, a filesystem backend, a
syscall) -- it answers "does it still boot cleanly," which is most of
what those changes need verified, much faster than the full QMP
GUI-testing dance below. It does NOT replace QMP testing for anything
that touches rendering, input, or window behavior -- a clean boot log
says nothing about whether a button is drawn in the right place; see
its own module docstring for the same division stated in code.

**GitHub Actions (`.github/workflows/build.yml`)** runs `make clean &&
make all && make iso` plus `tools/boot_smoke_test.py` on every push/PR
to `main` -- so a build break or boot regression is caught
automatically, independent of whether a session (or a human) remembered
to verify locally first. This doesn't replace verifying locally before
delivering a change (still do that -- see "Working in the cloud
sandbox" above), it's a second, automatic check behind it.

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
  `send_key('spc')`, for punctuation the matching qcode name (e.g.
  `bracket_left`, `semicolon`, `apostrophe`, `slash`, `dot` -- not the
  literal character; `query-qmp-schema`'s `QKeyCode` enum has the full
  list). For Shift/Ctrl/Alt combos (an uppercase letter, a shifted
  punctuation key), use `QMPSession.combo(['shift', 'bracket_left'])`
  -- QMP's `send-key` presses+releases every qcode in `keys` together,
  which is exactly a held-modifier combo; there's no separate "hold
  key down" primitive.
- **Rapid `send_key()` calls with little/no delay between them can
  silently drop keystrokes** at the guest keyboard-controller level
  (hit testing Notepad's filename field, build 490 -- 11 back-to-back
  backspaces dropped most of them). `send_text()`'s built-in per-char
  delay covers plain typing, but a manual sequence of `send_key()`
  calls needs its own explicit `time.sleep()` (0.05-0.08s has been
  reliable) between each one.
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
(QEMU/QMP GUI testing helpers, see above), `boot_smoke_test.py` (fast
non-GUI boot check, see above), `gen_version.sh`/`set_version.sh`
(versioning, see the `version.h`/`VERSION` bullets above),
`device_git.sh` (wraps a `git` command run over the device bridge with
the stale-`index.lock` workaround, see the "Working in the cloud
sandbox" section above).

Four more, added once the build/test/delivery loop above had enough
repeated manual steps to be worth automating:
- **`preflight.sh`** -- one command running `make clean && make all &&
  make iso` + `boot_smoke_test.py` + a `git status --short` summary, so
  "am I safe to deliver?" is one call instead of three run by hand.
  `--skip-clean` skips the initial `make clean`.
- **`deliver.py`** -- builds the delivery file-list/device-path/
  protected-file manifest and a commit-message skeleton for the
  shipping step (see "Delivering changes" below), from `git
  status --short` (or explicit file args) -- flags `Makefile`/
  `.github/workflows/*.yml` as PROTECTED with the `.new`-suffix
  workaround instructions before a real `device_commit_files` call
  would reject them.
- **`gui_flow.py`** -- named, composable QMP click-flows on top of
  `qmp_test.py`'s `QMPSession` (`GuiFlow` class: `enter_gui()`,
  `open_app(name)`, `run_system_action(label)`, `screenshot_named()`),
  so a testing session doesn't hand-derive Start-menu row pixel math
  from scratch every time. `APP_ORDER` must stay in sync with
  `apps/gui_apps.c`'s registry order.
- **`screenshot_diff.py`** -- Pillow-based pixel diff between two
  screenshots with a pass/fail `--threshold` (default 0.2%) and an
  optional `--out` diff-highlight image, for catching a rendering
  regression manual eyeballing might miss.
- **`tfs2_writer.py`** -- host-side TFS2 v2 read/write tool: get files
  onto (or off of) `disk.img` without booting toy-os. `format`
  initializes a blank/foreign image as an empty TFS2 v2 filesystem
  (mirrors `tfs_init()`'s format path byte-for-byte); `write`/`read`
  for a single file; `ls` for a directory listing; `sync <seed-dir>` to
  mirror a whole seed tree in (`once/` = copy-once, `sync/` =
  content-hash-synced -- see its own docstring and
  `docs/decisions.md`). `write`/`sync` auto-format a blank image first
  (no-op if already formatted), so a completely fresh `disk.img` can be
  seeded in one call with no toy-os boot in between -- this is what the
  Makefile's `seed` target (runs on every `make iso`) uses to get
  `/bin/lspci` onto disk at BUILD time now, replacing the old boot-time
  `BIN_BOOTSTRAP`/GRUB-module install (removed from `kernel.c`/
  `grub.cfg` -- see `docs/decisions.md`). Writes in-place by default;
  `--dry-run` on `write`/`sync`/`format` previews without touching the
  image. Scoped to direct+single-indirect blocks (~4.03 MB/file) -- see
  `docs/decisions.md` for why.

Add new tools here freely when something would save a future session
real time -- the bar is "does this fix a rederive-from-scratch cost,"
the same reasoning that produced all of the above.

## docs/

`docs/decisions.md` -- short, topic-indexed answers to "why does
toy-os work this way?" for the handful of decisions that come up again
once code has grown around them (e.g. "why is the VFS single-backend,
not mount points", "why doesn't `fs_delete` recurse"). Deliberately a
*pointer* file, not a second copy of the reasoning: each entry is a
couple sentences plus a link into the relevant `CHANGELOG.md` (or
`CHANGELOG-archive.md`, for anything before Build 173) section or
source file, not the reasoning itself restated. README.md/
apps/README.md already cover architecture in depth and CHANGELOG.md +
CHANGELOG-archive.md together are the full chronological history with
rationale -- `docs/decisions.md` exists because neither is indexed by
topic, so "why is X built this way" otherwise means scrolling/
searching the whole history.
Forward-looking "not built yet" items belong in `docs/roadmap.md`
instead (already actively maintained, with completed items struck
through and linked to the CHANGELOG build that finished them) -- don't
duplicate that list here or start a second one.

When you resolve a "wait, why is this built this way" question during
a session (by reading CHANGELOG.md, a source comment, or by asking the
user), consider whether it's the kind of question a future session
would hit again -- if so, add a short entry to `docs/decisions.md`
pointing at the answer, the same judgment call as `tools/`'s "does
this fix a rederive-from-scratch cost" bar.

## Delivering changes

List every file added or edited in the final response (standing
project instruction). Deliver files via `SendUserFile` +
`mcp__remote-devices__device_commit_files` to the user's real checkout
-- editing only the cloud sandbox copy doesn't reach the user's
machine on its own.
