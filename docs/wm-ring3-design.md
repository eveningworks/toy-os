# The WM in ring 3 -- finishing Milestone 41

**Status: DESIGN, not implemented.** Written the way
`docs/uapp-design.md` and `docs/tfs3-design.md` were: decide the shape
and the arguments first, build it in named stages afterwards. Nothing
below is built yet, and the stages are deliberately sized so each one
ships on its own.

**The one-sentence version:** the window manager is still ~5,300 lines
of ring-0 C that draws by calling `gfx.c` directly; moving it to ring 3
is four kernel capabilities and a transport swap, not a rewrite --
because Milestone 41 chose option C and the protocol was designed for
this from the start.

**Names**, as settled in `docs/decisions.md`: **TWP** is the protocol
(`abi/win_proto.h`), **TWS** the server implementing it
(`kernel/proc/win_server.c` + `apps/wm/wm_client.c`), **Toykit** the
client toolkit (`userland/ui/`). This document is about moving TWS and
the window manager around it out of the kernel. TWP does not change
shape; that is the whole point of having designed it as a protocol.

Decisions settled deliberately rather than defaulted, each with a
section below:

- **Ring 0 loses its apps before it loses the WM.** Stage 0 empties
  `apps/` of GUI applications, which cuts what has to move by more than
  half. It does NOT delete `apps/ui/` outright -- see stage 0's own
  correction below; the WM is itself a heavy user of it.
- **The transport becomes an abstraction before anything moves.** The
  desktop must look identical at the end of stage 3, with the WM still
  in ring 0. That is what makes stage 4 a swap rather than a leap.
- **The `gui` debug commands move WITH the WM**, forwarded over the
  same transport clients use. The 13 GUI test tools are the only proof
  the desktop works; a migration that breaks them cannot be verified at
  any step, which would make the whole plan unfalsifiable.
- **Client buffers get mapped into the WM, not copied.** Copying every
  window every frame is the obvious alternative and is rejected -- it
  turns compositing into a memcpy of the whole screen per frame.
- **Both input paths run at once during stage 2**, so the flip in stage
  4 is a deletion rather than a cutover.

## What already exists, measured

The reason this is tractable is that option C's bet has been paying
off for a year of sessions. Check these before assuming anything needs
building:

- **A ring-3 process can already map the real framebuffer.**
  `SYS_GUI_INIT` (`kernel/proc/syscall.c`) fills a `struct gui_info`
  and maps the linear framebuffer into the caller at `GUI_FB_VADDR`.
  It is a legacy path (`userland/tests/win_test.c` uses it), unguarded
  and with no double-buffering or damage -- but the primitive a
  compositor needs exists and works.
- **A ring-3 rasteriser exists**: `userland/ui/ugfx.c`, with the
  kernel's own font tables mapped READ-ONLY through `WIN_REQ_FONT`
  rather than copied, so client text cannot drift from the desktop's.
- **The widget set already exists on both sides.** `userland/ui/` is
  2,811 lines against `apps/ui/`'s 2,310, one file per widget at
  matching relative paths. Two widgets (`uui_menubar`, `uui_statusbar`)
  exist only in ring 3.
- **The memory/presentation split is already a registry.**
  `struct win_server_ops` (`kernel/include/api/win_server.h`) is the
  seam: `win_server.c` owns ids, buffers, mappings and teardown;
  `wm_client.c` owns the window list, chrome, z-order and input
  routing. Stage 4 moves the second half out and leaves the first.
- **`vmm_map_user_page()` takes an explicit `pml4_phys`**
  (`kernel/include/kernel/vmm.h`), so mapping one process's frames into
  a second address space is mechanically possible today. The primitive
  exists; the policy, the lifetime rules and the revocation do not.
- **Blocking waits, `SYS_SPAWN`/`SYS_WAITPID`/`SYS_PIPE`, and the
  kernel context as a scheduler participant** all landed already -- the
  last of those was M41's step zero, and nothing else worked until it
  did.

**Two M41 checklist items are marked `[ ]` and are actually done**:
client-side resize (`wm_client.c` sends `WIN_EV_RESIZE`; the
configure/ack handshake is covered by `tools/uapp_test.py`) and
force-closing an unresponsive client (`WIN_EV_PING`/`WIN_REQ_PONG` plus
`scheduler_kill()`, covered by `tools/forcequit_test.py`). Stage 0
corrects them.

## What is genuinely missing

Four kernel capabilities, plus the consequence nobody had written down.

**1. Client pixels must reach the WM's address space.** Today
`win_server.c` hands `wm_client.c` a plain kernel pointer, which works
only because the low 4GiB is identity-mapped. A ring-3 WM needs each
client's buffer mapped into *it*, revoked when the client dies or
resizes. This is blocker #1 and stage 1.

**2. Raw input must be delivered to the compositor.** Today the WM
*is* the router: it hit-tests, decides focus and pushes a `win_event`
into the owning client's queue. A ring-3 WM cannot receive its input
through the focus-routed queue it is itself responsible for filling. It
needs the raw stream, which nothing currently exposes.

**3. The transport must stop being a syscall.** `SYS_WIN_REQUEST`
carries every client operation as a typed message -- deliberately, so
the boundary is a protocol -- but it goes client → *kernel*. It must
become client → *WM process*. The message formats are already
pointer-free and fixed-layout for exactly this reason.

**4. A ring-3 allocator.** The WM's per-window state is `kmalloc`'d.
Ring 3 has `sbrk` and no `malloc` (Milestone 24). Toykit has no
allocator at all today -- which is why a menu is a const tree.

Plus the smaller ones, all of which stage 4 needs: syscalls for
`etc_config_*` (settings), a monotonic tick (`pit_ticks()`;
`sys_gettime` is RTC wall-clock and wrong for animation), and
`scheduler_kill`/`scheduler_poll` for force-quit and reaping. And
`MAX_PROCS` is **4** (`kernel/proc/scheduler.c`) -- the WM itself would
take one, leaving three for the entire desktop, when a terminal running
a command is already two.

### The consequence that is not in the roadmap

**All 13 GUI test tools drive the WM through `apps/wm/wm_debug.c`'s
`gui` command family** -- 814 lines reached over the kernel's serial
debug console. `gui windows`, `gui click`, `gui probe`, `gui damage
verify`, `gui dialog`, `gui ctxmenu` and the rest are how 175 checks
assert anything at all about the desktop. A ring-3 WM cannot answer a
command dispatched from inside the kernel's console.

That is not a detail to discover in stage 4. It is the reason stage 3
exists as its own step: the debug commands get forwarded over the
transport *while the WM is still in ring 0*, so the tools are proven
against the new path before the WM depends on it. `tools/*.py` are not
edited at any point.

### ELF loader hardening belongs here

`elf_load()` is never told the file's size -- `elf_run_from_fs()` has
it from `fs_read()` and discards it -- so `p_offset + p_filesz` is
unbounded and `load_segment()` will copy out of identity-mapped
physical memory past the buffer into a page it then maps into userland.
`p_vaddr` is unrange-checked, so a segment can claim the stack or heap
vaddr the runner maps afterwards. `PT_INTERP` is silently ignored, and
frames already mapped leak when a later segment fails. There are no
KTESTs for the loader at all.

This is tolerable while every ELF is one this build produced. It stops
being tolerable when the desktop itself is a ring-3 binary loaded at
boot, so it is stage 4's prerequisite rather than a separate security
milestone.

## Staging

Each stage is a commit that builds, tests and stands on its own -- the
A-E pattern TFS3 used and uapp repeated. **Stages 0 through 3 all end
with `tools/gui_regress.py` passing 13/13 with no test tool edited.**
That invariant is what steers the whole migration.

### Stage 0 -- empty ring 0 of applications  [DONE 2026-08-16]

The largest single reduction available, and it needs no new kernel
capability.

- Correct the two stale M41 checklist items (resize, force-close).
- **Delete** the kernel-side Notepad, Calculator and Terminal
  (`apps/notepad.c`, `apps/calculator.c`, `apps/terminal.c`) -- the
  ring-3 versions ship, are in the Start menu via `exec_path`, and have
  their own test tools. Remove their `gui_apps.c` registry entries and
  rename the "(ring 3)" suffixes off the survivors.
- **Port** About, Task Manager, Control Panel and UI Demo to
  `userland/gui/`. Task Manager needs a process-list syscall and
  Control Panel needs `etc_config` access -- both are on stage 4's list
  anyway, so building them here means stage 4 inherits them proven.
- UI Demo is the interesting one: it is the test target for
  `apps/ui/`, so porting it means `tools/uidemo_test.py` (28 checks)
  now drives the *userland* widgets. That is the point -- those are the
  widgets that survive.

**CORRECTION, from building it.** This section claimed stage 0 "ends
with `apps/ui/` having no callers, and deletable". That was wrong: the
WINDOW MANAGER is itself a heavy user of `apps/ui/` -- seven files in
`apps/wm/` include `ui/ui.h` (`desktop.c`, `file_picker.c`,
`start_menu.c`, `wm_render.c`, `wm_input.c`, `context_menu.c`,
`confirm_dialog.c`). What stage 0 actually deletes is the half of it
the apps owned: the checkbox, dropdown, listbox and text view. The
primitives, button, button group, textbox, radio list, icon grid,
scrollbar and scrollback stay until the WM moves in stage 4, and
`ui_focus` stays with them because the surviving button group and
textbox export focus tables into it.

**What actually shipped.** The three duplicated apps (Notepad,
Calculator, Terminal) deleted; About and UI Demo ported to
`userland/gui/` and launched through `exec_path`; four widgets deleted
from `apps/ui/`. Task Manager and Control Panel did NOT move -- both
need a syscall ring 3 does not have (a process list, and
`etc_config`), and those are stage 4's, so moving them here would have
made stage 0 a stage that adds kernel capability, which is exactly what
it is defined not to be. They are the remaining kernel-space apps.

**Three things it turned up that were not in the plan:**

  1. **`uui_textview` did not exist.** UI Demo's text view had no ring-3
     twin, so one was ported (`userland/ui/uui_textview.c`) over `utext`
     and `uui_scrollbar`.
  2. **A real WM bug.** Opening a window never told the previously
     focused CLIENT it had lost focus -- only `bring_to_front()` sent
     focus events, and `wm_client.c`'s own create path. A client
     therefore kept drawing a caret for input going elsewhere. Nothing
     had crossed that path because every test opened its second window
     before the client existed.
  3. **The test tools had to change after all**, though not their
     checks: eight of them opened the kernel-space Terminal and typed
     `run <name>` at it. With that Terminal gone, `gui open Terminal`
     spawns the ring-3 one, whose window does not exist yet when the
     keys arrive. They use `gui spawn` now (`DebugConsole.spawn()`),
     which is what `wm_debug.c` added it for.

**Proven by:** `gui_regress.py` 13/13, with `uidemo_test.py`'s 28 checks
now driving the RING-3 widgets. A deleted app that something still
references is a link error, not a silent gap.

### Stage 1 -- cross-process buffer sharing

- A kernel API to map an existing client window's frames into a second
  process's address space, built on `vmm_map_user_page()`, with
  revocation on client death and on the resize that reallocates a
  buffer.
- Guarded: only the registered compositor process may ask. A window
  buffer is a client's private memory and mapping it into an arbitrary
  process is a hole, not a feature.
- Nothing uses it yet except tests.

**Proven by:** KTESTs that map a buffer into a second address space,
write through one mapping and read through the other, then destroy the
client and assert the mapping is gone. The last of those is the one
that matters -- a revocation bug leaves the WM reading freed frames,
which will look like a compositing glitch and be diagnosed as one.

### Stage 2 -- input to a compositor process

- A syscall delivering raw keyboard and mouse events to the registered
  compositor: the stream `wm_input.c` consumes today, before focus and
  hit-testing.
- Runs **alongside** today's routing, not replacing it. Both paths are
  live until stage 4 deletes the old one.

**Proven by:** a test client that registers as the compositor, receives
synthetic input via `gui click`/`gui key`, and logs it -- while the real
WM keeps working from the same stream. `gui_regress.py` unchanged
proves the second consumer changed nothing.

### Stage 3 -- transport swap

The stage that makes stage 4 a swap rather than a leap.

- `struct win_transport` behind TWP, the same one-struct-of-function-
  pointers pattern `display_driver` and the VFS backend probe already
  use here. `SYS_WIN_REQUEST` becomes one implementation of it.
- A shared-memory ring as the second implementation -- the step that
  makes the syscall count stop scaling with event rate.
- **Forward the `gui` debug commands over the transport**, so
  `wm_debug.c`'s serial front end no longer calls into the WM directly.

The WM does not move. The desktop must be pixel-identical and every
test tool must pass untouched. If that is not true, stage 4 will not be
either.

**Proven by:** `gui_regress.py` 13/13 on the ring transport, plus
`damage_hunt.py` over several seeds -- the transport changes event
timing, and this bug family lives in orderings.

### Stage 4 -- the WM process

- Prerequisites: a ring-3 allocator, the settings/tick/process
  syscalls, a larger `MAX_PROCS`, and the ELF loader hardening above.
- `apps/wm/` becomes a ring-3 binary linked against Toykit and the
  ported widgets, spawned at boot. `win_server.c` stays in the kernel:
  it owns page tables and the frame allocator, which is exactly what
  ring 3 must not have.
- `gfx.c`'s rasteriser is already mirrored by `ugfx.c`. What actually
  moves is the compositor loop, damage tracking, chrome and the input
  routing -- and the framebuffer access becomes the guarded successor
  to `SYS_GUI_INIT`.
- Delete the old input path from stage 2 and the direct debug hook from
  stage 3.

**Proven by:** the same 13 tools, unchanged, for the fourth time -- and
one new property worth its own test: killing the WM process must not
panic the kernel. That is the entire point of the milestone, and it is
the first stage at which it becomes true.

## Testing

Nothing here introduces a new harness. The migration is steered by the
one that exists:

- `tools/gui_regress.py` -- 13 tools, 175 checks, each on its own disk
  copy and VM. Passes unchanged at the end of every stage.
- `tools/damage_sweep.py` / `damage_hunt.py` -- the damage invariant,
  which stage 3 and stage 4 both perturb. Run with
  `--positive-control` first; a clean sweep and a sweep that is
  checking nothing are the same output otherwise.
- `make test` -- KTESTs for stages 1 and 2, which are kernel work with
  no visible surface.
- `tools/faulttest_run.py` -- stage 4 changes the ELF loader and the
  address-space layout, which is exactly what it covers.

**The rule this document is built around:** ask what a broken version
would still pass. A migration whose test suite moves with it, one stage
at a time, is the only version of this that is falsifiable at every
step.

## Out of scope

- **Growable, non-contiguous client buffers.** `win_server.c` uses
  `pmm_alloc_contiguous()`, so a fragmented allocator can refuse a large
  window and the refusal is silent by design. Doing it properly needs a
  way to map scattered frames into a contiguous kernel virtual range,
  which this kernel has no helper for. Tied to Milestone 8's demand
  paging; do it there, not here.
- **Multiple windows per process.** The protocol carries window ids and
  `win_server.c` already tracks `WIN_CLIENT_MAX` per client, but nothing
  opens more than one, so the path is untested rather than missing.
- **A shared library for Toykit.** Every client statically links it
  today. Milestone 35.
- **Anything about how the desktop looks.** This is a plumbing change;
  a pixel that moves is a bug in it.

## Revision history

- 2026-08-16: written, after Milestone 2 closed and v0.2.0 shipped.
  Prompted by the question "what is still needed to get the whole WM
  working in ring 3?" -- the answer being four kernel capabilities, a
  transport swap, and one test-tooling consequence that had never been
  written down.
