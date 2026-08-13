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

## How the user wants to collaborate

Standing preferences from working on this project, independent of the
technical conventions below:

- Offer a few real choices before doing something non-trivial, unless
  the right path is genuinely unambiguous -- e.g. a data-layout
  tradeoff, or how far to build something this session vs. just
  planning it. Don't silently pick one approach when there's a real
  fork. A one-line obvious fix doesn't need this.
- Keep chat compact and terse -- only info that actually matters.
  Don't restate what's visible in a diff or build log, don't pad
  explanations.
- Before adding a new feature to a GUI app, consider whether it should
  be a reusable `apps/ui/` widget instead of a one-off (see this
  file's own note on widgets above) -- and ask the user first either
  way, don't decide unilaterally.
- Act like a genuinely experienced OS/UI designer, not a generic
  coding assistant bolted onto a hobby project -- if there's an
  established better way to do something (a real OS's approach, a
  better data structure, a cleaner API shape), say so and suggest it,
  rather than only doing exactly what's literally asked.

## Conventions worth knowing before editing

- **`kapi.h` is the one header apps include** for kernel capabilities
  (console, keyboard, mouse, timer/RTC, filesystem, graphics). Never
  `#include` a driver header directly from `apps/`, never call
  `inb`/`outb` from app code. If a capability isn't in `kapi.h` yet,
  that's a sign it belongs behind a new function in `kernel/core` or
  `kernel/drivers`, exposed through `kapi.h` -- not a reason to reach
  around the boundary.
- **There is a shared toolkit in `kernel/lib/` -- check it before
  hand-rolling a digit loop, a formatter, or a path join.** Four
  headers, all reachable through `kapi.h` and all with KTESTs:
  `string.h` (strings/memory/char classes), `knum.h` (numbers <->
  strings: `k_utoa`/`k_itoa`/`k_htoa`, `k_parse_u32`/`k_parse_hex`,
  ...), `kfmt.h` (`k_snprintf`, plus `vga_printf`/`klog_printf` for a
  whole line in one call), `kpath.h` (`k_path_join`/`_normalize`/
  `_resolve`/`_basename`/`_dirname`). This exists because a survey
  found the same twenty lines written nine times for int->string, ten
  for hex, six for parsing and three for path resolution -- and the
  path one wasn't just duplication, the copies disagreed (`edit
  ../x` meant different things in the GUI Terminal and the physical
  shell). Two conventions everything there follows, worth matching in
  anything added to it: a formatter that doesn't fit its buffer writes
  NOTHING rather than a truncated (i.e. wrong) value, and a parser
  REJECTS rather than guesses. Adding to the toolkit follows this
  file's usual bar -- a second real caller, not a plausible one; the
  batch that introduced it had `k_strstr`/`k_strcasecmp`/`k_toupper`
  written and building, found no caller for them, and deleted them
  again before landing. (`k_strstr` came back one feature later, when
  the shell's `Ctrl-R` history search needed it -- which is the rule
  working, not an argument against it.)
- **Line editing is `kernel/lib/klineedit.c`'s, in both front ends.**
  The physical shell and the GUI Terminal share one readline-style
  editor (buffer/cursor/kill ring/undo/keymap); each front end only
  paints the result. Don't add an editing key to one of them -- add it
  to the core's keymap and both get it. Ctrl/Alt reach apps as control
  codes and an ESC prefix, terminal-style, NOT as `KEY_*` codes (see
  `keyboard.h`'s "Ctrl and Alt" comment and `docs/decisions.md`).
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
- **The window manager lives in `apps/wm/`** -- the core event
  loop/input/render split (`wm.c`/`wm_input.c`/`wm_render.c`, sharing
  state through `wm_internal.h`'s `extern`s) plus the pieces that grew
  their own files as they appeared: `desktop.c`, `start_menu.c`,
  `context_menu.c`, `confirm_dialog.c`, `file_picker.c`, `wm_tray.c`.
  Split by concern for readability -- it's still one tightly-coupled
  event loop, not decoupled components. See `apps/wm/wm.c`'s top
  comment.
- **Split a file once it's grown big enough to be genuinely harder to
  work with, the same call that produced the `apps/wm/` split above --
  don't wait for it to become unmanageable, but don't split
  preemptively either.** There's no hard line-count rule; the signal is
  practical: a file mixing more than one real concern (e.g. event
  handling + rendering, like `wm.c` before its split), or long enough
  that finding/editing the right part of it gets slow and error-prone.
  Don't calibrate this against a line count quoted in a doc -- those
  rot (this bullet claimed "every hand-written file is under 800 lines"
  well after `kernel/fs/tfs.c` and `apps/shell_sys.c` had both
  passed 1,000). Run `wc -l` on the actual tree if you want today's
  numbers. A hand-written file pushing toward a couple thousand lines
  is the point to seriously consider a split, not a hard
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
  `kernel/fs/tfs.c` do. The changelog is the same
  instinct applied to docs, not code -- split by era each time it
  passed ~4,200 lines, cutting at one heading with a straight move and
  no rewording: `CHANGELOG-archive.md` holds Milestone 1 through Build
  173, `CHANGELOG-archive-2.md` holds Build 183 through Build 502 (the
  whole `## Build N` heading era), and `CHANGELOG.md` keeps the semver
  era plus `## [Unreleased]`. All three stay grep-able; a future split
  follows the same pattern rather than inventing a new one. Note
  `docs/decisions.md`'s `Build N` pointers name whichever file that
  build actually lives in -- keep them accurate when a split moves
  entries.
- **`kernel/` directories are subsystems, not filing cabinets** --
  `arch/x86_64/` (anything a different CPU would need rewritten),
  `core/` (bring-up and whole-machine concerns), `mm/`, `proc/`, `fs/`,
  `drivers/` (one piece of hardware each), `lib/` (services with no
  hardware of their own). `kernel/README.md` has the "does it belong
  here?" test per directory. Two lines worth holding: nothing outside
  `arch/` should contain `inb`/`outb`, inline assembly or a
  control-register access; and a filesystem backend goes in `fs/`, not
  `drivers/` -- the block device is the driver, the filesystem on top
  of it isn't.
- **`kernel/include/` is split by audience and the build enforces it**
  -- `api/` (what `apps/` may use), `abi/` (the kernel<->userland
  contract `userland/` shares), `kernel/` (internal, and NOT on
  `apps/`'s include path, so reaching for one is a compile error rather
  than a review catch). See `kernel/include/README.md`, including where
  a new header starts life (`kernel/`, moving to `api/` only when an app
  genuinely needs it).
- **`/etc` on the persistent filesystem is the config-file convention**
  (`kernel_main()` creates it right after `fs_init()`, before anything
  that might read a config file runs). Don't hand-roll a parser for a
  new setting -- read/write it through `kernel/include/api/etc_config.h`'s
  `etc_config_get()`/`etc_config_set()` (name=value lines, `#` comments,
  see `kernel/lib/etc_config.c`'s top comment for the exact format).
  By default, put a new setting's key in the shared `/etc/toyos.conf`
  every setting lives in today (`timezone`, `font_size`, `PATH` -- see
  `kernel/lib/tz.c`/`font_config.c` for the pattern: a small
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
- **`kernel/include/api/version.h` is GENERATED, not hand-edited** --
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

## Working in the cloud sandbox vs. directly on the user's machine

This repo gets worked on both ways: from a Cowork cloud session with
the user's real checkout reachable only through the device bridge
(`mcp__remote-devices__*`), and directly on the user's own machine
(e.g. a local Claude Code session) with normal file/Bash tools against
the real checkout. The mechanics below differ a lot between the two,
so **figure out which one you're in before following either half**:

- **Deterministic tell: run `git config user.name`.** A device-bridge
  session has NO git identity configured at all, local or global (it's
  its own isolated VM) -- empty output means Cowork/device-bridge.
  Non-empty (this repo's convention sets it to `toy-os`, see below)
  means a direct local checkout.
- Corroborating signal: are `mcp__remote-devices__*` tools (or
  equivalent device-bridge tools) actually available to call this
  session? Present means Cowork; absent means direct.
- If those disagree, or it's still unclear, just ask the user directly
  rather than guessing -- getting this wrong means either trying to
  push from a session where it's actually blocked, or going through
  the whole SendUserFile/device_commit_files dance unnecessarily.

### Cowork cloud sandbox (device bridge)

Four things about that setup that aren't obvious until you hit them:

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
- **The device-bridge session has NO git identity configured, local or
  global** -- confirmed directly: `git config user.name`/`user.email`
  and `git config --global --list` all come back empty in a fresh
  `device_bash` call, because that call runs in its own isolated VM,
  not the user's actual desktop environment. A plain `device_git.sh
  commit` will fail outright ("Please tell me who you are") unless the
  identity is passed explicitly every time:
  `bash tools/device_git.sh -c user.name="toy-os" -c
  user.email="noreply@toy-os.local" commit -m "..."`. This is also the
  standing privacy convention for this repo now, not just a workaround
  -- **never let a commit here carry the maintainer's real name or
  personal email**, session-made or otherwise (see `docs/decisions.md`'s
  entry on the history rewrite that scrubbed a real name out of every
  prior commit -- don't reintroduce what that fixed).
- Build and test in the cloud sandbox first (`make clean && make all
  && make iso`), confirm it's clean, *then* deliver + commit files to
  the user's machine. Don't commit unverified changes.
- **`git push`/`gh release create` from the cloud sandbox is not just
  discouraged, it's actually blocked.** Confirmed directly (v0.0.9
  release): pushing with the repo's token embedded in the remote URL
  still fails with `remote: access denied by the git proxy: ... not in
  this session's authorized repository set` -- the sandbox's outbound
  git egress goes through an allow-list proxy, independent of
  credentials. Read-only git (`fetch`, `ls-remote`) works fine through
  the same proxy. The device bridge has no network access at all
  either (by design). So there is no path in this environment to
  actually publish -- always tag/prep locally on both checkouts, then
  give the user the exact commands to run from their own machine's
  terminal. `gh` isn't preinstalled in the sandbox (`apt-get install
  -y gh` if needed there for read-only checks).

### Direct local checkout

Confirmed directly in a real local session (2026-08-12): this is
simpler than the Cowork setup in every way that setup works around --

- Git identity is already configured (`toy-os` /
  `noreply@toy-os.local`, matching this repo's standing privacy
  convention -- see above), so plain `git commit` just works with no
  `-c user.name=...`/`-c user.email=...` needed on every call. Still
  worth double-checking `git config user.name` if it's ever in doubt
  rather than assuming.
- Plain `git` works throughout -- no stale-`index.lock` issue, no
  `tools/device_git.sh` wrapper needed, no `mv`-instead-of-`rm`
  workaround for deleting a file.
- No protected-file restriction -- `Makefile` and
  `.github/workflows/*.yml` can be edited and committed directly, no
  `.new`-suffix relay needed.
- `git push origin main` and `gh release create` both work directly
  from the session -- confirmed by actually doing both (pushing
  ordinary commits repeatedly, and cutting the `v0.1.0` GitHub Release
  end-to-end with `gh release create` + `gh release upload`). Still
  treat both as actions to confirm with the user first per this file's
  general "Executing actions with care" guidance (pushing/publishing is
  visible to others), just don't tell the user it's *impossible* the
  way the Cowork section above correctly says it is there.
- `tools/preflight.sh`'s closing message and the QMP-launch pattern in
  `tools/qmp_test.py` are both mode-aware/updated for this case now --
  see their own comments if either looks like it's giving Cowork-only
  advice.

## Building

```
make all    # kernel.bin + userland test ELFs
make iso    # + toy-os.iso (grub-mkrescue)
make test   # boot headless, run the in-kernel test suite, exit non-zero on failure
make verify # the full pre-delivery gate: clean build + iso + boot test + ktest
            # (same as tools/preflight.sh, which also summarises `git status`)
make run   # boots in QEMU with an SDL window (the user's machine, not usable headlessly)
make run-audio  # same as run, + a PulseAudio backend so the PC speaker (`beep`) is audible
make debug # boots frozen (-s -S) for real GDB debugging -- see "Debugging with GDB" below
```
**Source discovery is recursive now** -- every `.c` under `kernel/` or
`apps/` is compiled and every `.asm` under `kernel/` assembled, with
`build/` mirroring the source tree, so a new directory needs no Makefile
edit at all. (It used to be one hand-written wildcard + pattern rule +
mkdir target per directory; `apps/wm/` and `apps/ui/` each paid that tax
when they appeared.) The flip side: a `.c` file anywhere under
`kernel/` or `apps/` IS in the kernel image -- there's no scratch file
the build ignores, so throwaway code goes somewhere else.

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
moves. Bit an actual user session once (see CHANGELOG-archive-2.md around build
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
rewrites `kernel/include/api/version.h` when `VERSION`'s value actually
changed) specifically so this dependency tracking doesn't regress --
`kapi.h` includes `version.h`, so an unconditional rewrite every build
would make every file that includes `kapi.h` (nearly everything) look
"out of date" and rebuild every single time.

**`make test` / `tools/ktest_run.py`** -- the in-kernel test suite.
Tests are `KTEST("suite", "name") { ... }` blocks living next to the
code they exercise (`kernel/mm/mm_test.c`, `kernel/fs/fs_test.c`, ...);
they register themselves through a `.ktests` linker section, so a new
test file needs no registry entry and no Makefile edit. `make test`
boots headless, drives `ktest` over the serial debug console and exits
non-zero on failure; `ktest` / `ktest <suite>` runs them interactively.
Two things worth knowing before writing one: tests run inside the LIVE
booted kernel (so don't assume a pristine heap or an empty filesystem --
that assumption is exactly what broke `heap_selftest()` when it moved
off the boot path), and `kernel/include/kernel/fault_inject.h` can fail
the next N ATA writes/reads or kmalloc calls, which is how the error
paths get tested at all. Nothing runs tests at boot any more.

**`tools/vm.py` -- start a VM once, then talk to it in TEXT.** This is
the fastest path for anything that isn't about pixels:

```
python3 tools/vm.py start
python3 tools/vm.py exec "fsck" "df"     # real shell output, as text
python3 tools/vm.py shot look.png        # pixels when you want them
python3 tools/vm.py stop
python3 tools/vm.py run "ktest"          # start+exec+stop in one
```

It drives the serial debug console's `sh <command>` rather than
emulating keystrokes, so there's no keyboard-layout dependence (a `se`
layout turns `write_test` into `write?test`), no dropped keys, and the
result is assertable instead of a screenshot to read. It only ever kills
a QEMU it started itself (its own `.vm.pid`), so an interactive `make
run` window is never at risk. GUI/rendering work still needs
`qmp_test.py`/`gui_flow.py` -- a text transcript says nothing about
whether a button is drawn in the right place.

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

## Debugging with GDB

`make debug` boots toy-os frozen at CPU reset (`-s -S`) instead of
running immediately, for real breakpoint/single-step/register/memory
debugging via QEMU's own built-in GDB remote stub -- **no kernel-side
GDB protocol code needed at all**: QEMU emulates the CPU directly, so
it can already do all of this regardless of what the guest OS does.
See `docs/decisions.md` for why an in-kernel serial-based GDB stub
(the seemingly obvious approach) is unnecessary and was deliberately
not built.

In another terminal, once `make debug` is sitting frozen:
```
gdb build/kernel.bin -ex "target remote localhost:1234"
```
then `break kernel_main` (or any other function -- `CFLAGS`/
`USERLAND_CFLAGS` both carry `-g` now, so `kernel.bin` and every
userland ELF carry real DWARF symbols: function names, source lines,
local variables, not just raw addresses) and `continue`. Confirmed
working end-to-end: `break kernel_main` + `continue` correctly runs
the CPU from reset through GRUB/multiboot2 and stops exactly at
`kernel_main`, with a real backtrace showing source file/line.

Kept at `-O2` (not dropped to `-Og`/`-O0` for a separate debug build)
deliberately -- same binary as every other build, just now carrying
symbols. Some locals may show as "optimized out" in GDB as a result;
accepted rather than maintaining a second build config just for
debugging.

## Testing in QEMU headlessly, via QMP

**First: is this actually a GUI change?** If not, `tools/vm.py` (above)
is faster and gives you text you can assert on instead of a screenshot
you have to read. The order of cheapness is
`boot_smoke_test.py` (does it boot) -> `make test` / `vm.py exec` (does
it work) -> QMP (does it look right). Reach for this section when the
answer genuinely depends on pixels -- widget layout, rendering, mouse
behaviour, window chrome -- because a text transcript says nothing
about any of those.

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

- **Launch via `tools/qmp_test.py`'s `launch_qemu_cmd()` -- call it (or
  copy its returned command verbatim), don't hand-roll a
  `qemu-system-x86_64` invocation from scratch.** It returns a command
  backgrounded with `-daemonize -pidfile <path>`, not a plain `&` or a
  `setsid nohup ... & ); disown -a` -- a bare `&` tied to one Bash tool
  call's shell gets killed when that call returns, and `setsid
  nohup`-style detaching (an earlier approach, superseded) turned out
  to be unreliable in at least one sandboxed environment (spurious
  non-zero exit codes on the launching call, the process not actually
  surviving to the next tool call). QEMU's own `-daemonize` avoids all
  of that -- it forks, detaches, and returns control immediately, no
  shell job-control subtlety to get wrong. Hand-rolling the command
  instead of using `launch_qemu_cmd()` is also how a QMP port mismatch
  happens silently: `launch_qemu_cmd()`/`QMPSession()`/`GuiFlow()` all
  default to port 4445, but nothing stops a hand-typed `-qmp
  tcp:127.0.0.1:4444,...` from picking a different one -- the failure
  mode is a flat `Connection refused` when the session tries to
  connect, not an obviously-QEMU-related error.
- **`GuiFlow(qmp_port=4445)` constructs its own internal `QMPSession` --
  don't create a `QMPSession` yourself and pass it in.** `GuiFlow.
  __init__` takes a port number (or other `QMPSession` kwargs), not a
  session instance; passing one positionally fails with a confusing
  `TypeError` inside `QMPSession.__init__` rather than an obvious
  "wrong argument" message. Access the session it already made via
  `flow.session` (e.g. `flow.session.screenshot(...)`,
  `flow.session.recalibrate()`) instead of holding a separate one.
- **Don't chain a `pkill` with further commands in the same shell
  invocation** (e.g. `pkill -f qemu-system-x86_64; rm -f qemu.pid; ...`
  or piping its result into a launch command) -- `pkill` exits 1 when
  nothing matched (nothing to kill is the common case, not an error),
  which trips `errexit` and aborts the rest of the chain with a
  spurious-looking `exit code 144`, even though every individual
  command in it would have worked fine run separately. Run `pkill` (or
  skip it entirely and check `ps aux | grep qemu` first) as its own
  Bash call, then launch fresh in a separate call.
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
  key down" primitive. **Typing a real physical-shell command** (e.g.
  `run nx_test`) means hitting this gotcha repeatedly in one line --
  `tools/shell_flow.py`'s `ShellFlow.run_command()` does the
  character-by-character `send_text()`/`send_key('spc')`/
  `combo(['shift','minus'])` mapping for you (space, hyphen,
  underscore, and a few other punctuation chars); use it instead of
  hand-rolling the dance inline. Two real mistakes from doing it by
  hand (a dropped space, a hyphen typed where an underscore was
  needed) are what prompted building it.
- **Don't `pkill`/kill-by-pattern across ALL `qemu-system-x86_64`
  processes** if there's any chance the user has their own `make run`
  QEMU open (an interactive SDL window, not a QMP-headless one) --
  matching by process name alone can't tell the two apart, and killing
  the user's real window is a genuinely bad surprise, not just a
  failed test. Track and kill only the PID your own launch wrote to
  its `-pidfile` (`cat qemu.pid; kill <pid>`), and if you ever do need
  to sweep stale instances, `ps aux | grep qemu-system-x86_64` first
  and eyeball which ones are actually yours (a QMP-headless launch has
  `-qmp tcp:...` and `-vnc :N` in its command line; the user's
  interactive one has `-display sdl` instead) rather than a blind
  `pkill -f qemu-system-x86_64`.
- **Rapid `send_key()` calls with little/no delay between them can
  silently drop keystrokes** at the guest keyboard-controller level
  (hit testing Notepad's filename field, build 490 -- 11 back-to-back
  backspaces dropped most of them). `send_text()`'s built-in per-char
  delay covers plain typing, but a manual sequence of `send_key()`
  calls needs its own explicit `time.sleep()` (0.05-0.08s has been
  reliable) between each one.
- **`drag()` takes BOTH points and its timings are keyword-only** --
  `drag(from_x, from_y, to_x, to_y)`. It used to take a destination
  only (`drag(x, y, hold, settle)`), and a call that reasonably read as
  four coordinates silently bound `hold=700`/`settle=300` SECONDS: it
  didn't fail, it slept for sixteen minutes. The `*` in the signature
  makes that a `TypeError` now, but the lesson generalises -- a helper
  whose positional arguments can absorb a mistake as a plausible value
  is worth reshaping rather than documenting.
- **Screenshots:** `screendump` writes a `.ppm`; `QMPSession.screenshot()`
  converts to `.png` via Pillow in one call so it's ready for the Read
  tool / `SendUserFile`. It hands QEMU an ABSOLUTE path on purpose --
  QEMU resolves `screendump`'s filename against its own working
  directory, and `-daemonize` leaves that somewhere other than the repo,
  so a relative path reports `{"return": {}}` (success) and writes the
  file somewhere else; the only symptom is Pillow raising
  `FileNotFoundError` on a path that looks obviously correct.

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
`genfont.py`/`genttf.py` (font generation, pre-existing), `gen_kbs.py`
(generates the `seed/sync/etc/kbs/<layout>` keyboard-layout data files
from Linux's own XKB data -- see `docs/decisions.md` on layouts being
data files, not a compiled-in enum), `qmp_test.py`
(QEMU/QMP GUI testing helpers, see above), `boot_smoke_test.py` (fast
non-GUI boot check, see above), `gen_version.sh`/`set_version.sh`
(versioning, see the `version.h`/`VERSION` bullets above),
`ktest_run.py` (drives the in-kernel test suite over serial and turns
it into an exit code -- what `make test` and CI run), `vm.py` (start a
headless VM and run shell commands against it, getting text back -- see
above),
`device_git.sh` (Cowork-only: wraps a `git` command run over the device
bridge with the stale-`index.lock` workaround, see "Working in the
cloud sandbox vs. directly on the user's machine" above -- not needed,
and not applicable, on a direct local checkout).

The rest, added once the build/test/delivery loop above had enough
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
- **`shell_flow.py`** -- the same idea as `gui_flow.py`, for the
  PHYSICAL (pre-`gui`) shell instead of the GUI: `ShellFlow.
  run_command(cmd, subdir=...)` types a full command -- including
  spaces/hyphens/underscores/a few other punctuation chars
  `qmp_test.py`'s `send_text()` can't handle on its own -- presses
  Enter, waits, and screenshots, instead of hand-interleaving
  `send_text()`/`send_key('spc')`/`combo(['shift','minus'])` calls
  character by character every session (a real mistake -- a dropped
  space, a hyphen typed where an underscore was needed -- happened
  twice in the session this was built in). Returns a screenshot path,
  not parsed text: this kernel's console auto-selects a framebuffer
  backend (glyphs drawn as pixels) whenever GRUB provides one, which
  is the normal case for this project's QEMU launch flags, so there's
  no legacy-VGA-text-buffer memory-read shortcut to plain text the way
  there might be on a kernel that only ever used 0xB8000 -- see the
  module's own docstring.
- **`screenshot_diff.py`** -- Pillow-based pixel diff between two
  screenshots with a pass/fail `--threshold` (default 0.2%) and an
  optional `--out` diff-highlight image, for catching a rendering
  regression manual eyeballing might miss.
- **`tfs2_writer.py`** -- host-side TFS2 v3 read/write tool: get files
  onto (or off of) `disk.img` without booting toy-os. `format`
  initializes a blank/foreign image as an empty TFS2 v3 filesystem
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
  image. `delete`/`mkdir`/`cp` manage paths inside the image, so test
  state can be set up and cleaned up entirely from the host rather than
  booting toy-os to type `rm`. Scoped to direct+single-indirect blocks (~4.03 MB/file) -- see
  `docs/decisions.md` for why. `corrupt` injects a KNOWN inconsistency
  (`--leak N`, `--free-referenced N`, `--bad-pointer PATH`) so the
  kernel's `fsck` can be tested against damage whose exact shape is
  known in advance, and `--stage-journal PATH` (+ `--stage-journal-torn`)
  leaves an image in the state a crash mid-`persist_record()` produces,
  which is the only way to exercise `replay_journal()` without an actual
  power loss -- the inconsistencies `fsck` repairs are ones the
  kernel deliberately avoids producing, so without this it could only
  ever be tested against a clean disk and proven to report "clean".
- **`mkpart_test.py`** -- writes a synthetic legacy MBR or GPT partition
  table onto a disk image, for testing `kernel/drivers/partition.c`'s
  parser (`parttable` shell command). TFS2-mount-preserving: patches
  only the partition-table byte ranges TFS2 itself never touches
  (reads the existing LBA 0 sector first rather than blindly
  overwriting it), so the real filesystem underneath still mounts
  normally afterward instead of `tfs_init()` seeing foreign magic and
  auto-reformatting. `--mbr`/`--gpt`; see its own docstring for the
  CRC32/GUID-encoding details and `docs/decisions.md` for why GPT
  verification needed a host-compiled unit test instead of a live
  boot (TFS2's own journal header collides with the GPT header's LBA).

- **`run_release.sh`** -- standalone QEMU launcher shipped as a GitHub
  Release asset (not part of the build), for running from just a
  release download with no checkout. Gunzips `disk.img.gz` if needed,
  boots with `make run`'s same device/display flags. When cutting a
  release: rebuild `disk.img` fresh (`make clean-disk` first), then
  `gzip -k -9 disk.img` before attaching it -- it's a large SPARSE file
  (~9GB apparent, actual data much smaller), and GitHub's 2GB-per-asset
  limit plus plain bandwidth sense both rule out the raw file. See
  `docs/decisions.md`'s versioning entry for the full v0.0.9 writeup.

Add new tools here freely when something would save a future session
real time -- the bar is "does this fix a rederive-from-scratch cost,"
the same reasoning that produced all of the above.

## docs/

`docs/decisions.md` -- short, topic-indexed answers to "why does
toy-os work this way?" for the handful of decisions that come up again
once code has grown around them (e.g. "why is the VFS single-backend,
not mount points", "why doesn't `fs_delete` recurse"). Deliberately a
*pointer* file, not a second copy of the reasoning: each entry is a
couple sentences plus a link into the relevant changelog section
(`CHANGELOG.md` for the semver era, `CHANGELOG-archive-2.md` for Build
183-502, `CHANGELOG-archive.md` for anything older) or
source file, not the reasoning itself restated. It opens with a
grouped index of every entry -- add a line there when adding an entry,
or the index silently stops being one. README.md/
apps/README.md already cover architecture in depth and the three
changelog files together are the full chronological history with
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

List every file added or edited in the final response, as a compact
list (standing project instruction) -- always, regardless of mode.

Never write personal information (PII) into any file being edited or
added. If a change genuinely seems to need some, ask first, or
anonymize it and say so plainly.

Any genuinely reusable tooling built or used during a session (a
helper script, a test harness) belongs in `tools/`, not left as a
scratch/one-off -- see `## tools/` above for the bar ("does this fix a
rederive-from-scratch cost"). Update the files that describe `tools/`
(this file at minimum) to match when something's added there.

How the change actually reaches the user depends on which mode this
session is in (see "Working in the cloud sandbox vs. directly on the
user's machine" above for how to tell):

- **Cowork/device-bridge:** deliver files via `SendUserFile` +
  `mcp__remote-devices__device_commit_files` to the user's real
  checkout -- editing only the cloud sandbox copy doesn't reach the
  user's machine on its own.
- **Direct local checkout:** the files are already on the user's real
  checkout -- there's nothing to "deliver," just commit (and push, if
  asked) directly with plain `git`.
