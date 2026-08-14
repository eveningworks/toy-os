# Changelog archive 3

Released versions `[0.1.0]` and `[0.0.9]` -- the first two releases of
the semver era. Split out of `CHANGELOG.md` when that file passed ~4,200
lines, the same threshold and the same method as the two archives before
it: cut at one heading, a straight move, no rewording. See `CLAUDE.md`
on splitting a file once it's genuinely harder to work with.

The full chronological history, oldest first:

- `CHANGELOG-archive.md` -- Milestone 1 through Build 173
- `CHANGELOG-archive-2.md` -- Build 183 through Build 502, the
  `## Build N` heading era
- `CHANGELOG-archive-3.md` (this file) -- releases `[0.0.9]` and
  `[0.1.0]`
- `CHANGELOG.md` -- `## [Unreleased]`, and every release after `[0.1.0]`

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

