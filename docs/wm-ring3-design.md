# The WM in ring 3 -- finishing Milestone 41

**Status: STAGES 0-3 BUILT (2026-08-16); stage 4's PREREQUISITES are
now complete too (2026-08-17) -- see the stage 4 section. Stage 4 is the WM itself and
is not started; its REQUIREMENTS are measured and written down
(2026-08-17), including two decisions settled deliberately (the WM owns
the back buffer; the kernel restores the text console when the WM
dies).** Written the way `docs/uapp-design.md` and
`docs/tfs3-design.md` were: decide the shape and the arguments first,
build it in named stages afterwards. Each stage below carries its own
built/not-built note -- read those before the prose around them, which
was written in advance and is corrected in place where building it
proved something different.

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
  same transport clients use. The 19 GUI test tools are the only proof
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

**4. A ring-3 allocator.** ~~The WM's per-window state is `kmalloc`'d.~~
**CORRECTION (2026-08-16): that claim was false.** `apps/wm/` allocates
nothing -- the only `kmalloc` reference in it was a COMMENT pointing at
`apps/calculator.c`, which stage 0 deleted. Its window table is a grown
block (`wm_windows_reserve()`) and that growth is its only allocation.
So stage 4 does not need an allocator to proceed.

Ring 3 still has `sbrk` and no `malloc` (Milestone 24), and Toykit has
no allocator at all -- which is why a menu is a const tree and why
`uui_table` pulls its rows instead of storing them. Building one is the
maintainer's decision for Toykit generally, not a blocker for this
milestone. **Check a stated blocker against the code before planning
around it**: this one survived unchallenged because it sounded
plausible.

Plus the smaller ones. Of these, only **syscalls for `etc_config_*`
(settings)** are still missing. Monotonic time is done and went further
than this asked for -- `SYS_TICKS` plus `SYS_MONOTONIC_NS` over a
clocksource registry (2026-08-17), so an animating client no longer has
to reach for `sys_gettime`, which is RTC wall-clock and wrong for
intervals. `scheduler_kill`/`scheduler_poll` for force-quit and reaping
are done. And
`MAX_PROCS` was **4** when this was written -- the WM itself would have
taken one, leaving three for the entire desktop, when a terminal running
a command is already two. It is `SCHED_MAX_PROCS` = 64 now, so this
particular blocker is gone.

### The consequence that is not in the roadmap

**All 19 GUI test tools drive the WM through `apps/wm/wm_debug.c`'s
`gui` command family** -- reached over the kernel's serial
debug console. `gui windows`, `gui click`, `gui probe`, `gui damage
verify`, `gui dialog`, `gui ctxmenu` and the rest are how 271 checks
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

### Stage 1 -- cross-process buffer sharing  [DONE 2026-08-16]

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

**What shipped** (`kernel/proc/win_server.c`, `win_server_test.c`, and
the ABI in `abi/win_proto.h`):

- `win_server_set_compositor(pid, pml4)` registers who may map, and
  **captures the address space** rather than looking it up per call.
  That keeps the mapping independent of which process happens to be
  current -- the same argument `win_server_ops` makes about taking
  `pid` -- and it is what makes the whole path reachable from a KTEST,
  which has no processes to look up.
- `win_compositor_vaddr(pid, window)` is the derived address, so a
  resize re-maps at the SAME place and the compositor is never told its
  pixels moved -- the trick that made client-side resize simple, reused.
  It also means the kernel can revoke without being told where the
  mapping is, which is why revocation is checkable rather than a matter
  of bookkeeping the two sides could disagree about.
- Revocation on all four paths that invalidate frames: explicit
  destroy, client death (`win_server_client_gone()`, the crash path,
  which is NOT the same code), resize (unmap before the free, re-map
  after, at the same address), and the compositor itself
  unregistering or dying.
- `win_server_create_raw()` / `_destroy_raw()` / `_resize_raw()` exist
  for the tests, because `win_server_request()` refuses everything when
  no presentation layer is registered and a `ktest` run has no desktop
  -- a test driving resize through the protocol would only ever test
  that refusal.

**Seven KTESTs, and the positive control is recorded in the file.**
Disabling `destroy_window()`'s revocation reddens exactly the two
revocation checks, on the page-table assertion rather than the flag;
the other five stay green because destroy, resize and unregister each
have their own path. Nothing outside the kernel uses any of this yet,
which is the stage's own definition.

### Stage 2 -- input to a compositor process -- **BUILT, 2026-08-16**

Landed as designed. What exists now, and the two decisions the survey
below left open, both settled in `docs/decisions.md`:

- **`WIN_REQ_SET_COMPOSITOR = 9`**, not a new syscall. The objection
  recorded below (every request is refused with no presentation layer
  registered, so a compositor could not register before the WM) turned
  out to cost six lines rather than a redesign: the case is handled
  ABOVE `win_server_request()`'s `!g_ops` guard, and `syscall.c`'s gate
  is applied after the copy-in so it can exempt this one type. Claiming
  replaces the previous holder; only the holder may release.
- **`WIN_EV_RAW_MOUSE` / `_KEY` / `_WHEEL` (10/11/12)**, carrying LEVEL
  STATE with screen coordinates -- the same thing the WM diffs today,
  so there is one differ rather than two. Pushed onto the compositor
  pid's existing `win_events` queue; no new transport, as predicted.
  Gated on CHANGE, or the tick-rate loop overflows a 32-deep queue while
  the user sits still.
- **The tap is inside `wm.c`'s loop, after the `wm_debug` overrides** --
  the survey's key finding, and it held.
- **`gui compositor [--json]`** in `wm_debug.c`: pid, queue depth,
  drops. The only view of a second consumer, since every other `gui`
  subcommand reports the WM's own state.
- **`userland/tests/compclient.c`** registers, logs the raw stream to
  stderr and quits on `q`. It owns NO window on purpose -- a window
  would drag focus, hit-testing and z-order into a test about none of
  those.
- **`tools/compositor_test.py`**, 16 checks, in `gui_regress.py`. Every
  injected input is asserted twice: in the compositor's log AND in UI
  Demo's, because "the compositor got the click" is also satisfied by an
  implementation that stole the stream. Two positive controls are
  recorded in `docs/decisions.md`; the reusable part is that removing
  the change-gate reddens exactly one check and leaves the neighbouring
  `dropped == 0` green, so that check is not covering what it looks like
  it covers.

Both input paths are live, as planned -- `gui_regress.py` passes
unchanged (15 tools), which is the evidence that the second consumer
changed nothing.

### Stage 2 -- input to a compositor process (original survey)

- A syscall delivering raw keyboard and mouse events to the registered
  compositor: the stream `wm_input.c` consumes today, before focus and
  hit-testing.
- Runs **alongside** today's routing, not replacing it. Both paths are
  live until stage 4 deletes the old one.

**Proven by:** a test client that registers as the compositor, receives
synthetic input via `gui click`/`gui key`, and logs it -- while the real
WM keeps working from the same stream. `gui_regress.py` unchanged
proves the second consumer changed nothing.

#### Measured, 2026-08-16 -- what stage 2 actually costs

Surveyed before building, so the next session starts from facts rather
than a re-read. Four findings change the shape of the work:

**There is no unified input event anywhere to reuse.** The WM does not
poll an event queue -- it reads *level state* and diffs it itself.
`mouse_get_state(&x, &y, &buttons)` (`kernel/drivers/mouse.c`) returns an
absolute clamped position plus a button bitmask, and `wm.c` derives
presses and releases by comparing against its own `prev_mx/prev_my/
prev_buttons`. The wheel is a read-and-reset accumulator
(`mouse_get_wheel_delta()`). Only the keyboard is a real queue -- a
256-entry ring in `kernel/drivers/keyboard.c` packed as
`key | (mods << 16)`, drained one key per iteration by
`keyboard_try_getchar_mods()`. The only event struct in the tree,
`struct win_event`, is *post*-routing and per-client. So stage 2 either
synthesizes press/release edges in the kernel or hands the compositor
the same level state and lets it diff -- and the second is what the WM
does today, which argues for it.

**The tap must go inside `wm.c`'s loop, not at the driver.** `gui click`
and `gui key` inject into rings in `wm_debug.c` and are applied *after*
the real driver read -- mouse state is overridden for that one iteration
(`wm.c`, after `mouse_get_state()`), and an injected key is used only
when the real keyboard returned -1, so a human is never pre-empted.
Tapping `mouse_get_state()` directly would therefore make every
synthetic event invisible to the compositor, which is precisely the
thing the stage has to demonstrate.

**Registration already exists and has no way in.** Stage 1 landed
`win_server_set_compositor(pid, pml4)`, `win_server_compositor_pid()`,
and the access-control idiom the rest should copy
(`if (!g_comp_pid || requester_pid != g_comp_pid) return 0;`), including
revocation of every mapping when the compositor is replaced or cleared.
**Nothing outside `kernel/proc/win_server_test.c` calls any of it** --
there is no syscall and no `WIN_REQ_*` that reaches it. Filling that gap
is the first thing stage 2 does.

**Delivery needs no new transport.** Raw events can be pushed onto the
compositor pid's existing `win_events` queue with new `WIN_EV_*` types,
and `sys_wait_event()` works unchanged on the client side. Next free
`WIN_REQ_*` is 9, next free `WIN_EV_*` is 10, next free syscall number is
30. `struct win_event` has a spare `reserved` word if a raw event needs a
fifth payload field, and its `window` field is meaningless for raw input
-- a natural place to carry which device it came from.

Two consequences to decide before writing code. Registration as
`WIN_REQ_SET_COMPOSITOR = 9` costs only `win_proto.h` plus one `case` in
`win_server.c`, and matches TWP's "operations are messages, not
syscalls" bet; the one argument against is that `SYS_WIN_REQUEST` is
refused outright when no presentation layer is registered, so a
compositor could not register before the WM does. And the event queue is
**32 deep and drops the oldest** -- thin for a raw pointer-motion
stream, with `win_events_dropped()` as the observable. A `gui compositor`
subcommand reporting `win_server_compositor_pid()` plus queue depth and
drops is ~10 lines in `wm_debug.c`'s existing style and is the natural
"the second consumer is live" assertion, since no existing `gui`
subcommand can see one.

### Stage 3 -- transport swap  [DONE 2026-08-16]

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

#### Scope DECIDED, 2026-08-16 -- do not relitigate

Settled with the maintainer before stage 2 shipped, recorded here so the
next session starts from it rather than re-opening it:

- **Build the abstraction and the `gui` forwarding. DEFER the ring.**
  The shared-memory ring is a performance item, not a prerequisite:
  stage 4 needs the WM to talk over *something*, and the syscall
  transport already does. The `gui` forwarding is the load-bearing half
  -- every GUI tool drives the WM through `wm_debug.c` over the
  KERNEL's serial console, so the checks that prove the desktop
  works have to cross the transport before the WM can move at all.
- **The debug channel is a `WIN_REQ_*`/`WIN_EV_*` pair**, command string
  in and output back, not a side channel of its own. Same precedent
  stage 2 set with `WIN_REQ_SET_COMPOSITOR`: an operation is a message,
  so there is no new kernel entry point and nothing for a ring-3 server
  to re-plumb. The argument for a separate channel -- diagnostics are
  not app-facing traffic and arguably do not belong in the app protocol
  -- was heard and rejected as not worth a second mechanism to maintain
  and move.
- **Write the limitation down rather than pretend otherwise.** An
  abstraction with exactly ONE implementation is shaped around that
  implementation and nothing proves it is not. This repo's own rule
  (`ata nodma`, `nopat`, TFS3 v1) is that an unreachable path is a
  guess. So `docs/decisions.md` gets an entry saying the seam is
  UNVALIDATED until a second transport exists, naming what is most
  likely wrong with it: batching, and who owns the copy. A cheap
  throwaway second implementation was considered and rejected -- it
  would prove the seam is not *syscall*-shaped without proving it fits
  anything real.

**So the exit criterion for stage 3 is not "the ring works", it is:**
`gui_regress.py` passes untouched with the `gui` commands travelling as
protocol messages, plus `damage_hunt.py` over several seeds because the
forwarding changes event timing.

#### Built, 2026-08-16 -- what it actually cost, and one correction

Exit criterion MET: `gui_regress.py` 16/16 tools, 243 checks, with **no
test tool edited at all**. The `gui` commands travel as
`WIN_REQ_DEBUG_CMD`/`WIN_EV_DEBUG_OUT` over `struct win_transport`, and
`kernel/core/debug_console.c` no longer calls into `apps/wm/`.

**The transport was the easy half; the OUTPUT was the work.** This
section described the stage as a transport abstraction plus forwarding,
which reads like plumbing. In practice `wm_debug.c` answered by writing
to `klog_write()` in 143 places, i.e. by a side effect on a serial port,
and a message has to carry BYTES -- so the real change was giving that
file an explicit output sink and converting every one of those sites.
Anything estimating stage 4 from this stage should count that kind of
work rather than the interface.

**Two things this stage needed that the plan did not mention:**

- **Chunking.** A reply exceeds any fixed message payload (`gui help` is
  1699 bytes), so `WIN_REQ_DEBUG_MORE` and a `WIN_DEBUG_F_MORE` flag
  exist. The flag is what makes a reply self-delimiting -- a chunk that
  exactly fills the buffer is otherwise indistinguishable from a
  truncated one, which a "read until short" client gets wrong.
- **Its own payload struct.** `struct win_event` is 24 bytes and
  `struct win_request_msg` carries `text[32]`; widening either would put
  a kilobyte copy on `WIN_REQ_PRESENT`, the once-per-frame hot path. See
  `docs/decisions.md`.

**`damage_hunt.py` over seeds 1-6: 5 of 6 violated the invariant, and
that is PRE-EXISTING, established by measurement rather than argument.**
Rebuilding `origin/main` and running the same seeds reproduces seed 4
byte for byte (81055 px, first at `(779,120)`, same damage rect -- the
fingerprint `docs/roadmap.md` already records) and reproduces seed 6's
random-walk violations too. Seed 6 first showed 4 on main against 5 on
this branch; a second main run showed 5, so that gap was run-to-run
variance, not the transport. Every violation is a `resize` interaction,
which this stage does not touch.

**What is NOT established:** whether the transport perturbs the timing
of that pre-existing family at all. "Both are broken the same way" is
what was measured; a rate comparison across many seeds on both builds
would be needed to say more, and was not run.

### Stage 4 -- the WM process

**Prerequisite status, 2026-08-16** (they were a flat list before, and
half of them have since landed for their own reasons):

- **ELF loader hardening -- DONE.** Every offset in the file is bounded
  against a size the loader is now told, PT_INTERP is refused rather
  than ignored, and both callers destroy the address space on failure
  instead of leaking it. 16 KTESTs (`kernel/proc/elf_test.c`).
- **The tick and process syscalls -- DONE.** `SYS_TICKS` (monotonic),
  `SYS_PROC_INFO` and `SYS_KILL`, built for Task Manager. `scheduler_
  poll` was already reachable.
- **`MAX_PROCS` -- long since raised** to `SCHED_MAX_PROCS` = 64.
- **A ring-3 allocator -- DECIDED, NOT BUILT.** Note the original
  blocker text below is WRONG and stayed wrong for a while: it said the
  WM's per-window state is `kmalloc`'d, which traced to
  `apps/calculator.c` -- a file stage 0 deleted. `apps/wm/` allocates
  nothing; its window table is a grown block now, and the growth is the
  only allocation it does. So stage 4 does not NEED an allocator. The
  maintainer's call is to build one anyway, for Toykit generally
  (Milestone 24 work), rather than to make the ring-3 WM permanently
  allocation-free.
- **The settings syscalls -- DONE (2026-08-17), and they went further
  than this asked for.** Rather than exposing `etc_config_*` directly,
  the kernel grew a SETTINGS REGISTRY (`kernel/include/api/setting.h`):
  a subsystem registers its setting the way a `display_driver`
  registers, and `SYS_SETTING` hands ring 3 the whole list -- names,
  labels, legal values, and the file each one lives in. Plus a
  config-FILE registry (`api/config_file.h`) indexing the /etc
  documents themselves, extensible from ring 3 by dropping a descriptor
  in `/etc/config.d` exactly as `.desktop` files extend the Start menu.
  `SYS_SYSINFO` covers the memory/disk figures System Info needed.

  Why a registry instead of the two syscalls this line asked for:
  nothing could previously answer "what settings exist", so a Control
  Panel had to carry its own list -- a second source of truth that
  drifts the moment a subsystem adds a key. The ring-3 Control Panel is
  GENERATED from the registry instead and contains no list at all.

- **Control Panel -- MOVED (2026-08-17).** `userland/gui/system/cpanel.c`.
  With it gone, `Exec=builtin:` has no users, the mechanism is deleted
  from `gui_apps.c`, and **ring 0 contains no applications at all**.
  `tools/cpanel_test.py` (14 checks) drives it.

  Moving it also turned up four Toykit bugs, all general: `uui_listbox`
  and `uui_radio_list` had no `natural_size`/`set_geometry` in their ops
  tables, so neither could be POSITIONED by a layout; the radio list had
  no font-derived metric defaults, so an unassigned field made it
  zero-sized (invisible and unclickable at once); `uui_statusbar` had no
  ops table; and `hidden` was honoured by the input router but not by
  the layout's draw, so a "hidden" page stayed fully visible while every
  log line said it had been hidden.

The shape, then: `apps/wm/` becomes a ring-3 binary linked against
Toykit and the ported widgets, spawned instead of called.
`win_server.c` stays in the kernel -- it owns page tables and the frame
allocator, which is exactly what ring 3 must not have.

#### The requirements, measured (2026-08-17)

Not estimated. `apps/wm/` is **7,363 lines across 21 files**, and every
kernel symbol it names was extracted from its own source. That surface
falls into seven groups, and the useful result is that **five of them
already have a ring-3 path**: the filesystem (`SYS_OPEN`/`READ`/
`WRITE`/`LISTDIR`/`UNLINK`), process control (`SYS_SPAWN`/`KILL`/
`WAITPID`/`PROC_INFO`/`TICKS`), the clock (`SYS_GETTIME`), settings
(the `SYS_SETTING` registry) and logging (`sys_eprint`). Raw input and
the font arrived in stages 1-3. What follows is only what is left --
each item with what exists, what is missing, and what would prove it.

**R1. The framebuffer, and who owns the back buffer. DECIDED: the WM
does.** The 100-odd `gfx_*` call sites are mostly rasteriser calls
`ugfx.c` already mirrors; what has no ring-3 owner is the *surface* --
`gfx_present()`, the write-combining publish, the dirty box,
`gfx_blit()`, `gfx_get_pixel()`, `gfx_set_font_size()`. The alternative
considered and rejected was leaving `gfx.c`'s back buffer in the kernel
and mapping it into the WM: fewer lines move and the PAT/WC reasoning
stays in one audited place, but ring 0 would then hold a rasteriser's
mutable state on behalf of a ring-3 client, which is the half-migration
this whole plan is shaped to avoid. So: a **guarded successor to
`SYS_GUI_INIT`** maps the linear framebuffer into the *registered
compositor only* (the role already exists -- `WIN_REQ_SET_COMPOSITOR`,
stage 1), the WM allocates and owns its back buffer, and a present
publishes a rect. The write-combining publish path is duplicated into
`ugfx.c` rather than shared, because it is a handful of lines and the
kernel still needs its own for the text console (R7).

  The trap to carry into it: `gfxbench` numbers under QEMU are
  meaningless (TCG ignores guest memory types), so the WC path must be
  measured under `tools/vm.py --kvm` or not claimed at all -- this is
  the one class of regression the whole suite is structurally blind to,
  and moving the publish is exactly the change that could cause it.

**R2. Damage verification.** `gui damage verify on` is
`gfx_verify_snapshot()`/`_diff()`/`_release()` -- kernel functions that
render a frame twice and compare. `damage_sweep.py` and
`damage_hunt.py` are built entirely on it, and it is the only harness
this project has for the WM's worst bug class. With R1 decided it
becomes a **WM-internal** facility: the WM owns both buffers, so it can
snapshot, re-render unrestricted and diff without the kernel present at
all, reporting through the same `dbg_out` sink stage 3 gave it. The
requirement is that `gui damage verify on` keeps working *with the same
output grammar* -- pixel count, bounding box, and the third-render
stability verdict -- because the tools parse it.

**R3. The cursor.** `gfx_hw_cursor_available()`/`_define()`/`_move()`
sit on the display driver, which is hardware and stays in ring 0, so
these become TWP requests or a small syscall. `mouse_init()` and
`mouse_set_bounds()` are the other half: the WM currently initialises
the mouse and tells it the screen size. Bounds are the compositor's
business (it knows the screen), the device is not. Falling back to a
software cursor drawn by the WM was considered and is *not* the
default -- the hardware cursor is what keeps the pointer moving
independently of damage, and losing it would show up as cursor lag
nothing in the suite asserts on.

**R4. `fs_generation()`.** One counter the VFS bumps on every change;
the WM compares it once per frame to decide whether to re-read
`/usr/wm/desktop/` (`wm.c:600`). There is no syscall for it, and
without one the live `.desktop` reload either dies or degrades into
re-listing a directory every frame -- which is real I/O in the
compositor loop, the exact thing the counter exists to avoid.
Cheapest correct answer: one more field in `SYS_SYSINFO`, or a
one-value `SYS_FS_GENERATION`. `desktop_entries_test.py` (13 checks)
is its gate.

**R5. The serial debug console's drain point.** This is the
requirement nobody would predict, and it is load-bearing for the
entire test suite. `debug_console_poll()` has exactly two callers that
matter: `keyboard_getchar_mods()`'s idle loop (the physical shell) and
**`wm_run()`'s loop** (`wm.c:759`). While the desktop is up, the WM
*is* what keeps the kernel's serial console answering -- and all 19 GUI
tools, all 271 checks, arrive over that wire. Move the WM to ring 3 and
ring 0 has no drain point at all for the whole time the desktop is
running. The kernel needs its own: the timer tick or an idle-path poll,
owned by nothing in `apps/`. Prove it by asserting the console still
answers *while a ring-3 process is spinning* -- `sched_gui_test.py`
already encodes that trick in the other direction.

**R6. Memory.** `apps/wm/` allocates in exactly one place: growing
`windows[]` (`wm_windows_reserve()`). `SYS_SBRK` covers it, so no
allocator is required (see the prerequisite note above). The rule that
comes with it is unchanged and gets *more* dangerous in ring 3, where
a stale `struct window *` faults instead of corrupting: index, never
cache across anything that can open a window.

**R7. Lifecycle, and what happens when the WM dies. DECIDED: the
kernel restores the text console.** Today `wm_run()` returns and calls
`vga_resume()` itself (`wm.c:838`), having first shut the client list
down in order. In ring 3 that ordering has to survive as a kernel-side
teardown: `win_server.c` sees the compositor deregister (cleanly, or by
`scheduler_kill()`, or by faulting -- all three are the same path), so
it unmaps the framebuffer, drops every client window, and resumes the
VGA console. The physical shell's `gui` command becomes spawn-and-wait.
Respawning the WM was considered and rejected for now: it puts a policy
about *which binary is the desktop* into the kernel, and the milestone's
exit criterion is that killing the WM is survivable, not invisible.

  The `vga_sink` hazard in `wm.c:820`'s comment is the thing to
  re-read before building this -- exiting GUI mode with a stale sink
  installed silently swallows the shell's own output, and the ring-3
  version has the same hazard with a longer gap between the two halves.

**R8. What compiles twice, and what moves outright.** `apps/ui/`,
`apps/theme.h` and `kernel/lib/rubberband.h` are all already
mirrored or shared-source; `gui_apps.c` (the `.desktop` scan) moves
wholesale and becomes an ordinary ring-3 consumer of `SYS_LISTDIR` --
note its 32-entry-per-call cap is a real constraint on a desktop
directory, so either it grows an offset or the scan iterates.
`wm_debug.c` moves with the WM, which is what stage 3 was for.

**R9. What gets DELETED, not moved.** This is where stage 4 pays some
of its cost back, and it should be counted as part of the work:
- The `pending_write`/`pending_read`/`pending_proc` step machinery
  (`wm.c:843` onward). It exists solely because a ring-0 `wm_run()`
  must never block on the filesystem. A ring-3 WM is a *process* -- it
  can call `read`/`write` and be descheduled like anything else, and
  the whole one-block-per-frame dance goes away.
- Stage 2's duplicate input path, once `WIN_EV_RAW_*` is the only one.
- Stage 3's direct debug hook.
- `SYS_GUI_INIT` in its unguarded form, once R1's successor exists.

#### Sub-staging

7,363 lines is too much to flip in one commit, and every stage in this
document so far has been chosen so the suite passes at its end. Stage 4
should be cut the same way -- a plausible split, to be confirmed when it
starts: **4a** the kernel capabilities (R1's map/present, R3, R4, R5)
with the WM still in ring 0 calling them, which is the stage-3 trick
again and keeps the suite green throughout; **4b** the ring-3 binary
drawing the desktop with input still routed the old way; **4c** the
input and debug cutover plus R9's deletions; **4d** the death path (R7)
and its test. 4a is the one that can be built and proven without
moving anything.

**Proven by:** the same 19 tools, 271 checks, unchanged, for the
fourth time -- plus `damage_sweep.py --positive-control` first, since
R2 rebuilds the harness those runs depend on, and a clean sweep from a
harness that is checking nothing looks identical. And one new property
worth its own test: **killing the WM process must not panic the
kernel**, and must land the user at a working text shell. That is the
entire point of the milestone, and it is the first stage at which it
becomes true.

## Testing

Nothing here introduces a new harness. The migration is steered by the
one that exists:

- `tools/gui_regress.py` -- 19 tools, 271 checks, each on its own disk
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

- 2026-08-17: stage 4's requirements measured from `apps/wm/`'s own
  call surface and written up as R1-R9, with a 4a-4d sub-staging
  sketch. Two forks settled rather than defaulted (framebuffer
  ownership, WM-death policy). The unexpected one is R5: the kernel's
  serial debug console is drained by `wm_run()`'s loop, so moving the
  WM out silently takes the whole 271-check test wire down with it.
- 2026-08-16: written, after Milestone 2 closed and v0.2.0 shipped.
  Prompted by the question "what is still needed to get the whole WM
  working in ring 3?" -- the answer being four kernel capabilities, a
  transport swap, and one test-tooling consequence that had never been
  written down.
