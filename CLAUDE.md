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
a preemptive scheduler, a kernel-space window manager, and two
disk-backed filesystems (TFS3 the default, TFS2 kept as a
probe-selected second backend). No cross-compiler needed -- host and target are
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
  again before landing. (All three have since come back, each once a
  real caller turned up -- `k_strstr` for the shell's `Ctrl-R` history
  search, the case-folding pair for `timezone Helsinki`'s lookup, which
  is also why that folding is ASCII-only; see `docs/decisions.md`.
  That's the rule working, not an argument against it.)
- **Anything drawn follows `docs/gui-guidelines.md`.** Three things
  bite most often. (1) **`gfx_draw_string()` does not clip** -- use
  `gfx_draw_string_clipped()` and `gfx_text_width()` for anything in a
  fixed box; this caused the identical overlap bug in two different
  files, the second one written days after the first was documented.
  (2) **`on_click` fires on button-DOWN despite its name**, so a
  control that commits there can never be cancelled -- arm in
  `on_press`, act in `on_release`, which is what the title bar has
  always done. (3) **`gfx_set_clip_rect()` with a non-positive w/h sets an EMPTY
  clip -- nothing draws -- and only `gfx_clear_clip_rect()` removes a
  clip**; conflating the two once handed an app's whole `on_draw()` an
  unclipped screen (the damage sweep's long-standing "20px"
  violation). (4) **Interaction states come from `enum ui_state` /
  `ui_state_bg()`**, which derives hover/pressed from the control's own
  colour; don't hand-pick tints, and don't assume hover means "lighter"
  (on this near-white theme it has to darken -- `gfx_luminance()`
  decides).
- **Verify GUI changes by reading PIXEL VALUES, not by looking at the
  screenshot** (`tools/pixel_probe.py`). A hover state that moved the
  background by two units out of 255 looked perfectly plausible in a
  PNG and was invisible in practice; the number is what caught it.
  Always sample a control that should NOT have changed as well -- half
  the assertion is the neighbour staying put.
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
  `ui_checkbox.h`, `ui_button.h`/`ui_button_group.h`, `ui_textbox.h`,
  `ui_radio_list.h`, `ui_icon_grid.h`, `ui_textview.h`,
  `ui_listbox.h`/`ui_dropdown.h` -- the dropdown composes the listbox,
  and its popup needs `ui_dropdown_draw_popup()` called AFTER every
  other widget, since drawing is immediate-mode and z-order is call
  order -- and `ui_focus.h`, the per-window keyboard-focus ring a widget
  joins by exporting one `ui_focus_ops` table. **Route keys through
  `ui_focus_key()`, never by trying each widget in turn**: the first one
  tried swallows every key it recognises, which left a listbox next to a
  dropdown unreachable from the keyboard)
  and **`apps/theme.h`** (`THEME_*` named colors) are small apps-internal
  helpers, same peer-level pattern as `wm/wm.h`. Both are deliberately
  minimal on purpose -- see their top comments before adding to them.
  Add a new widget primitive or theme color only once a second real
  caller needs it, not preemptively. **And once a widget exists, its
  BEHAVIOUR belongs to it, not to the apps** -- input handling,
  hit-testing and geometry live in `apps/ui/`, and an app configures
  (colours, visibility policy, step sizes, who owns an ambiguous event)
  and forwards events rather than reimplementing them. That's a standing
  rule with an escape hatch, not an absolute: see
  `docs/gui-guidelines.md`'s "Behaviour belongs to the component"
  section for when not to, and `apps/ui/ui_textview.h` for the worked
  example -- scrolling used to be copy-pasted into three apps, and the
  third copy shipped a scrollbar that drew and did nothing. (`apps/widgets.h`/`.c` -- the
  single file all of `apps/ui/` was split out of -- no longer exists;
  see `docs/decisions.md`.)
- **Every ring-3 program is just a `main()`.** `userland/crt0.asm`
  provides `_start` (reads argc/argv off the stack per SysV, calls
  `main`, passes its return to `sys_exit`) and `userland/sys.c` is
  libsys -- one typed wrapper per syscall. **Never hand-roll an
  `int $0x80` stub in a new program**; that duplication across twenty
  files is exactly what libsys replaced. `sys_call()` is the raw escape
  hatch and is for the `/tests` diagnostics that poke the raw ABI on
  purpose, not for ordinary code. Two things to know before touching
  `crt0.asm`: the entry ABI is the STANDARD SysV stack layout (argc at
  `(%rsp)`), and `%rsp` must be **16-aligned before `call main`** -- a
  `sub rsp, 8` there looks like it restores the old convention and
  instead faults every SSE-using binary while leaving plain ones
  working, see `docs/decisions.md`.
- **A ring-3 process can own a real window** (`apps/wm/wm_client.c` +
  `kernel/proc/win_server.c`, protocol in
  `kernel/include/abi/win_proto.h`). Two rules matter before touching
  it. **Every client operation is a typed MESSAGE carried by the one
  `SYS_WIN_REQUEST` syscall, never a syscall of its own** -- that is
  what keeps the boundary a protocol, so the window server can later
  move to ring 3 as a transport swap instead of a rewrite; see
  `docs/decisions.md`. And **the split is memory vs. presentation**:
  `win_server.c` owns ids/buffers/mappings/teardown (page tables and
  the frame allocator, which `apps/` can't reach), `wm_client.c` owns
  the window list, chrome, z-order and input routing, and they meet at
  a registered `struct win_server_ops` -- the same registry pattern as
  `display_driver`. **A client draws with `userland/ugfx.c`**, not with
  syscalls -- there is no drawing syscall and there shouldn't be, since
  only the framebuffer is privileged, not drawing. The one thing a
  client can't produce for itself is the font, which
  `WIN_REQ_FONT` maps READ-ONLY out of the kernel's own tables rather
  than copying (one instance in memory, and client text can't drift
  from the desktop's when `font_size` changes). Widgets for a client
  live in **`userland/uui.c`** (the ported `ui_button_group` and
  friends) with colours in `userland/utheme.h` -- port more from
  `apps/ui/` only when a client actually needs them, the same bar
  `apps/ui/` holds itself to. **A file needed by both the kernel and a
  client is COMPILED TWICE, never copied** (`build/userland/shared/`,
  see the Makefile): the two builds use different code models so the
  objects can't be shared, but the source can, which is why the ring-3
  and kernel Calculators cannot disagree about arithmetic. Only
  freestanding files qualify.
- **The window manager lives in `apps/wm/`** -- the core event
  loop/input/render split (`wm.c`/`wm_input.c`/`wm_render.c`, sharing
  state through `wm_internal.h`'s `extern`s) plus the pieces that grew
  their own files as they appeared: `desktop.c`, `start_menu.c`,
  `context_menu.c`, `confirm_dialog.c`, `file_picker.c`, `wm_tray.c`,
  `wm_client.c`.
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
  whole `## Build N` heading era), `CHANGELOG-archive-3.md` holds
  releases `[0.0.9]` and `[0.1.0]`, and `CHANGELOG.md` keeps
  `## [Unreleased]` plus every release after `[0.1.0]`. All four stay
  grep-able; a future split follows the same pattern rather than
  inventing a new one -- cut at a release heading, straight move, no
  rewording. Note
  `docs/decisions.md`'s `Build N` pointers name whichever file that
  build actually lives in -- keep them accurate when a split moves
  entries.
- **A graphics card is a `display_driver`, not a special case.**
  `kernel/include/kernel/display.h` defines the interface (required
  probe/get_surface; optional flush, cursor, accel, modeset, each behind
  a capability bit) and `kernel/drivers/display/` holds the registry plus
  the drivers -- `vesafb` (GRUB's framebuffer, registers last, always
  claims) and `vmsvga`. Adding a card is one file and one
  `display_register()` line; `gfx.c` is a rasteriser that never learns
  which card it's on. `display_probe()` REFUSES a driver whose
  capability bits and function pointers disagree, because a card that
  needs a flush and doesn't get one shows a frozen screen while memory
  holds the right pixels -- a genuinely hard bug to read, and one this
  project has already paid for twice.
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
make run-kvm    # same as run, but KVM-accelerated (-enable-kvm -cpu host) instead of
                # TCG emulation -- needs /dev/kvm readable
make debug # boots frozen (-s -S) for real GDB debugging -- see "Debugging with GDB" below
```

**`make run-kvm` is not a straight speedup, and throughput numbers from
the two modes are not comparable.** Measured on the same disk image,
same host, `stress 150`: TCG 22.8 MB/s write / 29.2 MB/s read, KVM
12.1 / 18.7 -- KVM about 1.9x *slower* for disk I/O. Guest code that's
actually computing gets much faster, but every port-I/O instruction
becomes a hardware VM exit (~1us) where TCG services one in-process
(~tens of ns), and this kernel's disk path is dense with `inb`/`outb`.
So: useful as a second mode to test in, and useful for anything
CPU-bound, but always say which mode a benchmark came from --
`tools/vm.py --kvm` runs the same configuration headlessly.
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
python3 tools/vm.py --kvm run "stress 150"   # same, KVM-accelerated (see `make run-kvm`)
python3 tools/vm.py --cpu Skylake-Client run "lscpu"  # a specific QEMU CPU model
```

**`--cpu MODEL` matters more than it sounds** for anything reading
CPUID: the default `qemu64` reports as **AuthenticAMD** with no CPUID
leaf 4, so cache-topology code takes the AMD `80000005H`/`80000006H`
fallback there and the leaf-4 path never runs at all. `--cpu
Skylake-Client` is GenuineIntel with leaf 4 populated; `--cpu max`
gives the widest feature set. Test CPU-dependent code against more than
one, or half of it is unexercised.

It drives the serial debug console's `sh <command>` rather than
emulating keystrokes, so there's no keyboard-layout dependence (a `se`
layout turns `write_test` into `write?test`), no dropped keys, and the
result is assertable instead of a screenshot to read. It only ever kills
a QEMU it started itself (its own `.vm.pid`), so an interactive `make
run` window is never at risk. GUI/rendering work still needs
`qmp_test.py`/`gui_flow.py` -- a text transcript says nothing about
whether a button is drawn in the right place.

**There is a GUI app built to be tested against: "UI Demo"**
(`apps/uidemo.c`). One of every `apps/ui/` widget at documented,
font-derived, content-relative offsets, and every interaction logged as
one parseable line (`uidemo: button 2`, `uidemo: check alpha on`,
`uidemo: cancel btn`). Combined with the `gui` commands below, a GUI
test becomes drive-and-assert over a single serial wire with no
screenshot in the loop -- and when a click lands on the wrong thing, the
log says which widget it actually hit. Read its top comment for the
layout table and the log grammar before writing coordinates by hand.

**`tools/gui_debug.py` -- ask the WM what it's doing, instead of
measuring a screenshot.** The serial debug console has a `gui` command
family now (`apps/wm/wm_debug.c`), live while the desktop is up:

```
gui windows [--json]     rects, content rects, z-order, focus
gui probe X Y [--json]   which window/region is at a point, and what overlay would take the click
gui menu | gui taskbar    row + button geometry, as the kernel computes it
gui state [--json]       overlays, cursor, armed drag/resize/press, damage rect
gui damage [verify on|off]  the damage rect; verify catches missed damage
gui open <App>           open a window directly -- no Start-menu clicking
gui click X Y | gui drag X1 Y1 X2 Y2 | gui key <c>   synthetic input
```

**`gui damage verify on` catches the WM's worst bug class.** The
compositor only repaints declared damage, so anything that changes on
screen without being declared leaves stale pixels -- no crash, no
assertion, often visible in one interaction only. Verify mode renders
every frame twice (damage-limited, then unrestricted) and reports any
differing pixel with coordinates. It found four real bugs in its first
minute. Turn it on whenever you touch drawing, damage, focus or
chrome; `docs/gui-guidelines.md` has the invariant it enforces.

Reach for this BEFORE QMP for anything that isn't literally about
pixels: it returns facts you can assert on rather than an image to read,
and `DebugConsole.menu_row("Terminal")` gives the real row centre
instead of `gui_flow.py`'s hardcoded menu arithmetic. Two things to
know: injected input enters at the WM loop **below the PS/2 driver**, so
it exercises WM/app logic and proves nothing about the mouse driver; and
it is **asynchronous** -- call `DebugConsole.settle()` before asserting,
because a command dispatched from inside `wm_run()` cannot block waiting
on `wm_run()`.

**`settle()` POLLS, and don't replace it with a sleep.** It waits on
`gui state`'s `pending` (the WM's own undelivered-event count) rather
than sleeping a fixed interval, because the loop is not a metronome: a
drag takes ~110ms normally and ~800ms under `gui damage verify on`, and
the number moves again with font size, window count and display driver.
The fixed sleep this replaced didn't fail loudly -- it let windows move
between a `gui windows` read and the command using those coordinates, so
drags grabbed the wrong thing and a damage test reported a different bug
on each run of the same script. **Also: `click()`/`drag()` return
`events()`, which filters to the `uidemo:` prefix and will silently drop
a `wm:` line** -- use `logs()`/`damage_bugs()` for anything the kernel
logs. Both traps cost a session real time; see `docs/decisions.md`.

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

**Test against a COPY of `disk.img` if the user might have their own
QEMU open.** Two separate hazards, both hit for real: QEMU takes a
write lock on the image, so a headless launch dies with `Failed to get
"write" lock` while an interactive `make run` holds it; and `make iso`
re-seeds `disk.img` every time, which rewrites the filesystem
underneath a VM already booted from it. `cp --reflink=auto disk.img
/tmp/test.img` and then `vm.py --disk /tmp/test.img` (or
`launch_qemu_cmd(disk=...)`) avoids both -- and is the right move
anyway whenever a test needs particular files on disk, since it leaves
the real image alone. Both also take `--qmp-port`/`--vnc`
(`qmp_port=`/`vnc_display=`) for running a second instance alongside
an existing one.

**A copy is not enough for `make iso`/`make verify`/`preflight.sh` --
ASK the user to close their QEMU first (standing request).** Working
against a copy dodges the write lock, but those three targets re-seed
the real `disk.img` regardless of what any test is pointed at, so
they rewrite the filesystem underneath a VM the user has open and
leave it running on a stale in-memory view. This is easy to miss
because nothing fails loudly at the time. What does NOT need them to
close anything: editing, `make all` (kernel only, never touches the
image), and `vm.py --disk <copy>`. So check whether a QEMU is running
(`ps aux | grep qemu-system`) before the verify gate rather than
after, and ask -- don't just work around it silently, and don't ask
for the cases in the previous sentence either.

**`strace <binary>` is often the fastest way to see what a `/bin`
binary is doing** -- one decoded line per syscall
(`open("notes.txt", O_WRITE|O_CREAT) = 3`), and the same lines land in
`dmesg`, so `python3 tools/vm.py exec "strace file_test"` returns text
you can assert on. Reach for it before adding temporary `klog_write()`
calls inside a syscall handler; see `kernel/proc/strace.c`.

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
  make iso` + `check_layout.py` + `boot_smoke_test.py` + `ktest_run.py`
  + a `git status --short` summary (`fs_switch_test.py` is NOT in it --
  that one needs a disk copy and a longer boot cycle, run it yourself
  after `kernel/fs/` changes), so
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
- **`check_layout.py`** -- verifies `disk.img`'s directory structure
  matches `docs/filesystem-layout.md`'s table, which is the source of
  truth for where things live on the OS's own filesystem. Runs in
  `preflight.sh` and CI. Fails in both directions (an undocumented
  directory on the image, or a documented-as-present one missing), and
  understands the table's "Created by" column -- a `build`-created
  directory must exist on a freshly built image, a `boot`-created one
  needn't until the OS has run. **Read that doc before adding a
  directory, a config file, or any new seeded data**: it also records
  the budgets (64-byte caller-side path buffers everywhere; the
  256-record table on TFS2-legacy images only -- TFS3, the default
  since Milestone 15, has ~590k inodes) and the `sync`-never-deletes
  trap that makes moving a seeded file need an explicit cleanup.
- **`pixel_probe.py`** -- reads exact pixel values out of screenshots,
  and tabulates the same points across several (`--compare a.png b.png
  --at 85,100 --at 215,100`), flagging which moved and which didn't.
  This is how `docs/gui-guidelines.md` says to verify a GUI change, and
  the rule exists because a hover state that shifted the background by
  TWO units out of 255 looked entirely plausible in a PNG. Always
  include a point that should NOT change -- half the assertion is the
  neighbour staying put. `--box N` averages a square, for anti-aliased
  edges where a single pixel is a coin toss.
- **`check_layout.py`** -- see the `docs/` section: verifies the built
  image's directories against `docs/filesystem-layout.md`. Runs in
  `preflight.sh` and CI.
- **`dialog_test.py`** -- verifies the confirm dialog's Yes/No buttons
  by PIXEL VALUE: hover moves the hovered button and leaves its
  neighbour alone, a press dragged off doesn't commit, No closes it.
  Two traps it encodes: hover needs the REAL cursor parked (use
  `DebugConsole.warp_cursor()` -- `gui move` holds for one WM iteration
  only, and `QMPSession.goto()` is open-loop and undershoots a large
  jump), and don't sample the pixel under the cursor sprite.
- **`uidemo_test.py`** -- drives UI Demo's widgets and asserts on its
  log (27 checks: click selection, cancel paths, keyboard navigation,
  Tab/Shift-Tab focus cycling, Space activating a focused button,
  wheel-scrolls-without-selecting, the dropdown popup's open/commit/
  dismiss/Esc, and keyboard focus). Exits non-zero on a failed check.
  Run it after touching anything in `apps/ui/`. Geometry comes from the
  app's own `uidemo: layout ...` lines rather than from re-deriving row
  offsets in Python -- the Python copy drifts silently the moment a row
  is added to the app, which is exactly what happened when the dropdown
  and listbox rows landed mid-file.
- **`calculator_client_test.py`** -- drives the RING-3 Calculator
  (`userland/calculator.c`) and asserts on it (8 checks). Worth reading
  for two techniques: it uses **no OCR** -- every check is a round trip
  (a state change must alter the display's pixels, and returning to the
  same logical state must restore them EXACTLY), which proves rendering
  and arithmetic together and also catches a right number drawn in the
  wrong place; and its last check presses a button, drags OFF it and
  releases, which must NOT commit. That one matters because a client
  acting on button-down passes every other check and fails only that.
  Geometry is derived from the window's reported content size, not
  hardcoded, so it survives a font-size change.
- **`uiclient_test.py`** -- drives `userland/uiclient.c`, the ring-3
  client that renders real text with `userland/ugfx.c`, and asserts on
  it (8 checks: text actually rendered, the button drew, a click and a
  key each repaint, the unchanged label comes back identical, the close
  handshake works). Two things it encodes: "text was rendered" is
  asserted as INK COVERAGE in a band rather than a single-pixel sample
  (a glyph run puts a countable number of non-background pixels in its
  rows; a blank window and a solid fill are both distinguishable that
  way), and **a client's `stdout` goes to the owning Terminal's
  scrollback, not the serial console** -- so `DebugConsole.logs()`
  can't see a client's own log lines even though a shell-spawned
  process's are visible. Run it after touching `userland/ugfx.c` or
  the font-sharing path.
- **`winclient_test.py`** -- drives `userland/winclient.c`, the ring-3
  client that owns a real window on the desktop, and asserts the
  windowing protocol end to end (8 checks: the window appears in the
  WM's own list at the requested size, the client's pixels reach the
  screen, a key and a click each route to it and make it redraw, the
  window behind it does NOT change, the close handshake completes, the
  desktop survives). Geometry comes from `gui windows` and content from
  PIXEL VALUES with a control point, per `docs/gui-guidelines.md`. Run
  it after touching `apps/wm/wm_client.c`, `kernel/proc/win_server.c`,
  or anything in `abi/win_proto.h`.
- **`sched_gui_test.py`** -- proves the desktop stays ALIVE while a
  ring-3 process runs, the end-to-end counterpart to
  `kernel/proc/sched_test.c`'s KTESTs. The trick it encodes: the `gui`
  debug commands are dispatched from inside `wm_run()`, so a frozen WM
  cannot answer one -- which makes "did the WM answer?" a direct
  liveness test with no screenshot to interpret. Every sample is paired
  with the WM's own `proc_pid` (`gui state --json`) so only samples
  overlapping a genuinely live process count; OVERLAP is the claim, not
  speed. Run it after touching the scheduler, `wm_run()`'s loop, or
  anything about process spawning. Both it and the KTESTs were checked
  as positive controls with the change disabled (0 overlapping samples
  there, versus a continuously responsive desktop) -- do that again
  before trusting a clean run, same reasoning as `damage_sweep.py`'s
  `--positive-control`.
- **`damage_sweep.py`** -- drives the WM through the interactions that
  historically break the damage invariant with `gui damage verify on`,
  and exits non-zero on a violation. Run it after touching anything
  that draws, damages, focuses or changes window chrome. `--random N
  --seed S` adds a seeded random walk (the seed prints on every run, so
  a failure replays exactly); `--positive-control` inverts the exit
  code, for proving the harness detects a real violation before
  trusting a clean run -- a clean sweep otherwise can't be told apart
  from a sweep that isn't checking anything, which has happened here
  for real.
- **`screenshot_diff.py`** -- Pillow-based pixel diff between two
  screenshots with a pass/fail `--threshold` (default 0.2%) and an
  optional `--out` diff-highlight image, for catching a rendering
  regression manual eyeballing might miss.
- **`tfs2_writer.py`** -- host-side TFS2 v3 read/write tool: get files
  onto (or off of) `disk.img` without booting toy-os. **`trim` returns
  every free block's space to the host** by punching holes through them
  -- run it if `du disk.img` ever looks large. The image is sparse when
  created and only ever loses that: a block written once stays allocated
  on the host even after toy-os deletes the file that owned it, and the
  dev image had reached 8.1 GiB actually allocated against 2.3 MiB in
  use before this existed. The kernel issues ATA TRIM as it frees blocks
  now (`ata_trim()`, plus `discard=unmap` on every `-drive` line), which
  stops new images getting there; `trim` is for images already in that
  state, and for the host-side seeding path, which never boots the
  kernel. Non-destructive: only blocks the filesystem already considers
  free are touched. `format`
  initializes a blank/foreign image as an empty TFS2 v3 filesystem --
  note that running it on a BLANK image opts that image out of the
  TFS3 default; that's `seed_disk.py`'s job to decide, not a thing to
  do casually; `write`/`read`
  for a single file; `ls` for a directory listing; `sync <seed-dir>` to
  mirror a whole seed tree in (`once/` = copy-once, `sync/` =
  content-hash-synced -- see its own docstring and
  `docs/decisions.md`). `write`/`sync` auto-format a blank image first
  (no-op if already formatted), so a completely fresh `disk.img` can be
  seeded in one call with no toy-os boot in between -- the Makefile's
  `seed` target (runs on every `make iso`) goes through
  `tools/seed_disk.py` now, which delegates here only when the image's
  magic says TFS2 (a fresh/blank image gets TFS3 -- see
  `tfs3_writer.py` below). This replaced the old boot-time
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
- **`tfs3_writer.py`** -- the TFS3 sibling of `tfs2_writer.py`: format
  (writes superblock backups + GDT snapshots, wipes a stale TFS2
  signature per the wipefs rule, keeps images sparse by skipping/
  hole-punching the zeroed inode tables) / ls / read / write / mkdir /
  delete / sync (`once/` + `sync/` convention) / trim / info /
  corrupt (`--leak`, `--free-referenced`, `--bad-link-count`,
  `--smash-superblock`, `--stage-journal[-torn]` -- known damage for
  fsck/backup/journal-replay testing, same reasoning as
  tfs2_writer's). Spec: `docs/tfs3-spec.md`; the kernel backend
  (`kernel/fs/tfs3.c`) is kept in lockstep and the same bar applies
  as tfs2_writer's: direct+single-indirect write scope only.
  **TFS3 is the default format for FRESH images** (blank-disk policy
  in `vfs.c` and `seed_disk.py`); an existing TFS2 disk.img keeps
  mounting as TFS2 -- `make clean-disk && make iso` is the deliberate
  move. Use whichever writer matches the image's magic (both refuse
  the other's images; `trim` before gzipping a release image means
  the MATCHING tool's trim).
- **`seed_disk.py`** -- the format-aware seeding front-end the
  Makefile's `seed` target calls: probes the image's magic, delegates
  `sync` to the matching writer, and formats a blank image with the
  default (tfs3) -- the same policy the kernel's blank-disk path
  applies at boot.
- **`fs_switch_test.py`** -- boots a COPY of disk.img and proves the
  multi-backend story end-to-end: probe mounts the image's own
  format, `fsformat` live-switches both ways (wipefs rule included),
  writes work on each side, files survive reboots, fsck ends clean.
  Run it after touching anything in `kernel/fs/`; it exercises the
  probe/format/remount/reboot cycle no KTEST can (the suite runs
  inside one booted kernel).
- **`mkpart_test.py`** -- writes a synthetic legacy MBR or GPT partition
  table onto a disk image, for testing `kernel/drivers/partition.c`'s
  parser (`parttable` shell command). Its mount-preserving guarantee
  was designed for (and verified against) TFS2 images; a TFS3 image
  deliberately leaves its first 32 KiB untouched for exactly this, so
  coexistence is by-design there, but the tool hasn't been re-verified
  against one -- check before trusting it on TFS3. TFS2-mount-preserving: patches
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
  (9GB apparent, ~2MB of real data on a freshly-trimmed image), and
  GitHub's 2GB-per-asset limit plus plain bandwidth sense both rule out
  the raw file. Run the matching writer tool's trim
  (`tools/tfs3_writer.py trim disk.img` for a fresh-built image,
  `tfs2_writer.py` for an old TFS2 one) before gzipping
  -- sparseness is only ever lost, and an untrimmed image compresses
  whatever stale data it is still carrying. See
  `docs/decisions.md`'s versioning entry for the full v0.0.9 writeup.

Add new tools here freely when something would save a future session
real time -- the bar is "does this fix a rederive-from-scratch cost,"
the same reasoning that produced all of the above.

## docs/

`docs/decisions.md` -- short, topic-indexed answers to "why does
toy-os work this way?" for the handful of decisions that come up again
once code has grown around them (e.g. "why does the VFS run one
ACTIVE backend (probe-selected), not mount points", "why doesn't
`fs_delete` recurse"). Deliberately a
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
`docs/filesystem-layout.md` -- what lives where on the OS's own disk
(`/bin` vs `/tests` vs `/usr/share` vs `/etc`), the rules for adding to
it, the deliberate divergences from the FHS, and the budgets that constrain it
(64-byte caller-side paths everywhere; the 256-record table on
TFS2-legacy images only). Not advisory: `tools/check_layout.py` reads its table and fails
`preflight`/CI if the built image disagrees, in either direction. Read
it before adding a directory, a config file or any seeded data.

`docs/gui-guidelines.md` -- how the GUI is supposed to look and behave:
the four `enum ui_state` interaction states and their flat (non-bevelled)
rendering, the press-then-commit-on-release rule every control follows,
when feedback IS and ISN'T wanted, the `on_hover` contract, text/layout
budgeting, and how to verify a GUI change properly. Read it before
touching anything drawn.

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
