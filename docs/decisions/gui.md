# Decisions: GUI: window manager, compositor and widgets

The window protocol, the compositor, the toolkit, and how anything gets drawn.

Part of **[docs/decisions.md](../decisions.md)**, which indexes every
decision in this project and is GENERATED from these files -- run
`tools/gen_decisions_index.py` after adding an entry here, or
`tools/check_docs.py` will fail.

Write the reasoning HERE, in full: an entry that cannot be understood
without opening something else is not finished.

---

## The ring-3 WM owns the back buffer, and its death drops you to a text shell

Two forks settled while writing Milestone 41 stage 4's requirements
(`docs/wm-ring3-design.md`, R1 and R7), both of which had a defensible
cheaper answer.

**The back buffer.** The cheaper option was to leave `gfx.c`'s surface,
its dirty box and its write-combining publish in the kernel and map the
buffer into the ring-3 WM -- fewer lines move, and the PAT/WC reasoning
(which this project has already paid for once, on real hardware only)
stays in one audited place. Rejected: ring 0 would then be holding a
rasteriser's mutable state on behalf of a ring-3 client, which is
exactly the half-migration the staged plan exists to avoid, and it
leaves `gui damage verify on` -- the harness for the WM's worst bug
class -- straddling the boundary. So the WM allocates and owns its back
buffer, a guarded successor to `SYS_GUI_INIT` maps the framebuffer to
the *registered compositor only*, and damage verification becomes
WM-internal. The kernel keeps its own publish path regardless, because
the text console and the panic path still need one.

**What happens when the WM dies.** Respawning it is what a real desktop
does, and it was rejected for now because it puts a policy about *which
binary is the desktop* into the kernel. Instead a compositor
deregistering -- cleanly, by `SYS_KILL`, or by faulting, all the same
path -- makes `win_server.c` unmap the framebuffer, drop the client
windows and resume the VGA console, which is what `wm_run()` already
does for itself today (`userland/wm/wm.c`'s `vga_resume()` on exit, and the
`vga_sink` hazard commented just above it). The milestone's exit
criterion is that killing the WM is *survivable*, not that it is
invisible; making it invisible first would hide whether it is
survivable at all.

## The compositor's back buffer is ring-3 memory, and getting it there took two kernel fixes

Milestone 41's R1 decided that a ring-3 compositor allocates and owns
its own back buffer, rather than the kernel keeping `gfx.c`'s and
mapping it in -- because ring 0 holding a rasteriser's mutable state on
behalf of a ring-3 client is the half-migration the whole plan is shaped
to avoid. R6 then said `SYS_SBRK` covers the WM's allocation needs.

R6 was measured against `userland/wm/`'s existing call surface, where the
only allocation is growing `windows[]`, and it was wrong twice for the
same reason -- a back buffer is not in that surface, because in ring 0
it is `gfx.c`'s 8 MiB of `.bss`.

**Wrong about the size.** One screen of 32bpp pixels is 3.5 MiB at
1280x720 and 8.3 MiB at 1920x1080. The ring-3 heap was 1 MiB: heap base
`0x8000100000`, stack top `0x8000200000`, everything between them. So
the compositor could not allocate the one buffer it exists to own.
Fixed by moving the stack top to `0x8000F00000`, into address space that
was already free -- nothing lives between there and `WIN_CLIENT_BASE`
(`0x8001000000`), whose own comment said so. ~14 MiB at the time -- and
SUPERSEDED on 2026-08-18, when the stack top moved again to
`0x807FF00000` and `sbrk` stopped mapping what it reserves, making the
heap ~2046 MiB; see this file's sibling entry in `kernel.md`. ~14 MiB was a
1080p buffer with room to spare. What does not fit is that buffer AND
the damage-verify scratch copy at once (8.3 + 8.3 > 14); that is
debug-only, fails by reporting rather than faulting, and raising
`WIN_CLIENT_BASE` is the lever if it ever needs to.

**Wrong about it being reachable at all**, which is the more
interesting half. `SYS_SBRK`'s state was a set of file-globals in
`syscall.c` armed by `syscall_reset_heap()`, and the only caller of that
is `elf_run.c` -- the legacy blocking loader behind `run <name>` at the
physical shell. A SCHEDULER-spawned process, which is every GUI app and
everything `gui spawn` starts, never had a heap armed, so `SYS_SBRK`
returned -1 for it unconditionally. `syscall.h` even said so, in a
sentence describing the limit as intentional. Nothing noticed for as
long as it was true, because nothing spawned had ever asked for memory:
Toykit has no allocator, so a ring-3 app's storage is its globals and
its stack. The first program to call `sbrk()` from a spawned process was
the compositor, on its first line.

The break now lives in `struct sched_process` as a `struct sched_heap`,
armed when the slot is created rather than by a separate call --
**an init step reachable by only one entry point is a bug waiting for a
second entry point**, which is the same rule `vfs.c`'s `ensure_layout()`
and `shell_session_init()` already record. The legacy loader keeps its
own single slot, because it has no scheduler slot to put one in, but it
holds the SAME type and goes through the same handler, so the two owners
cannot drift in behaviour. `scheduler_current_heap()` returning NULL
means "the kernel context is running", never "this process has no heap".

**The lesson worth carrying:** a requirement measured from an existing
implementation's call surface can only find what that implementation
already does. R6 counted what the ring-0 WM allocates; what mattered was
what a ring-3 one would have to allocate that ring 0 had been getting
for free from `.bss`. When a migration moves a component across a
boundary, ask what the boundary was silently providing, not just what
the component asks for.

## A stale test fixture stops crossing the boundary it tests, silently

`guard_test` proves `SYS_SBRK` stops at the stack guard. It asked for
2 MiB, with a comment justifying the number: "the gap between the heap
base and the guard is about 1 MiB, so this is comfortably past it" --
and warning that an earlier 1 GiB version had passed against a kernel
with the bound REMOVED, because physical memory ran out before the stack
did.

Widening the heap to ~14 MiB made the 2 MiB request succeed. The branch
under test was no longer reached, and the failure surfaced as a
different check going red for a reason that pointed nowhere near the
cause. A number chosen against an address map is stale the moment that
map moves.

It walks to the boundary now: grow in 1 MiB steps until one is refused,
writing through every chunk handed back, capped at 64 steps. That
measures the property -- sbrk stops before the stack -- at any heap
size, and needs no edit the next time the map changes. Both halves of
the old comment's reasoning survive as parameters rather than as a
constant: the STEP is small enough that a refusal means the bound
refused it rather than the allocator giving up, and the CAP bounds a
kernel with no bound at all, which would otherwise loop until it had
mapped the address space. Verified by running it against exactly that
kernel: it dies with `RIP=0x0`, the return address having been
overwritten by the heap.

Generalising the repo's existing fixture rule: it is not enough to ask
whether a test's input crosses the branch today. Ask what the input is
sized AGAINST, and whether that thing can move independently of the
test.

## The kernel owns its idle work, so the WM's departure deletes a call rather than a capability

Milestone 41 stage 4a. `scheduler_idle()` (`api/scheduler.h`) is the one
place the kernel does "while I have nothing else to do" work; what it
owns today is `debug_console_poll()`.

The reason it exists is a dependency nobody had written down: the serial
debug console had no owner. It was polled from whichever loop happened
to be running -- the physical shell's key wait, `userland/wm/wm.c`'s event
loop, a long `cat`, the demo's timer -- and the WM's copy is the
load-bearing one, because all 23 GUI test tools and their ~300 checks
arrive over that console while the desktop is up. Moving the WM to ring
3 (stage 4) removes that loop from ring 0 and takes the whole test wire
with it, silently, in the direction that reads as "the tools are
broken". Naming the work kernel-side means the WM's departure deletes a
CALL and the capability stays.

**Why not the timer tick**, which is the obvious answer and is wrong: a
dispatched command can be `sh cat big`, which blocks on the filesystem,
and an interrupt handler must not. Nothing is lost by waiting for a
normal context -- COM1's receive is already interrupt-driven into a ring
buffer (`kernel/serial.h`), so the bytes are safe there and only the
line assembly is deferred.

**Console upkeep is deliberately NOT part of it.** `vga_cursor_tick()`
and `vga_present()` stay in `keyboard_getchar_mods()`: they belong to
whoever owns the screen, and the GUI desktop owns it while it is up.
What `scheduler_idle()` holds is work that is safe wherever the kernel
is idle.

Unifying the call sites also exposed a live bug. `debug_console_poll()`
is not re-entrant and genuinely re-enters -- a dispatched command's own
wait loop calls back into it -- while `dbg_dispatch()`'s `arg` points
INTO `line_buf`, so a nested call assembling the next command overwrote
the running one's arguments underneath it. It refuses the nested call
now; the bytes wait in the UART ring buffer, so nothing is dropped.

## A ring-3 write to the framebuffer is transient while the WM is still in ring 0, and the test has to say so

Milestone 41 stage 4a. `WIN_REQ_FB_MAP` grants the registered
compositor a writable, write-combining mapping of the real framebuffer;
`WIN_REQ_FB_PRESENT` publishes a rect. The obvious way to test that --
paint a block, look for it in a screenshot -- is wrong, and it fails
against a kernel where every part of the mechanism works.

The reason is the shape of the stage: the ring-0 WM still owns the
screen. A ring-3 write lands in the framebuffer and survives exactly
until the WM's next frame repaints that region. Measured rather than
argued: reading the same address back through the mapping returns the
written colour immediately after the write and the desktop background a
moment later. Two compositors writing one screen is what 4a IS, so the
transience is a property to assert, not a race to defeat.

**What is assertable** is that the mapping is the real screen, from two
independent sides -- the client reports the pixel it reads through the
new mapping, QEMU's screendump reports the same pixel from the
display's side, and they must agree. A mapping onto any other memory
cannot produce agreement.

**The positive control is worth copying, for what stayed green.**
Offsetting the granted physical base by 2 MiB reddens exactly the
two-observers check. It leaves "the write reads back through the
mapping" GREEN -- because a read-back proves only that *some* writable
page is mapped there, not that it is the framebuffer. A test built on
the read-back alone would have shipped a grant onto arbitrary memory.

Two mechanism notes worth having beside that. The memory type has to
reach the USER PTE (`vmm_map_user_page_type()`, `VMM_MT_WC`): the
kernel's identity map and the compositor's mapping are separate PTEs
with separate types, and `paging_set_write_combining()` only touches
the former, so without this the compositor gets a cached framebuffer --
the bug class TCG cannot show. And `WIN_REQ_FB_PRESENT` is required
rather than advisory because a `display_driver` may declare
`DISPLAY_CAP_NEEDS_FLUSH` (vmsvga does), where written pixels are
invisible until the driver is told; `display_flush()` already no-ops on
a scanned-out adapter, so one path serves both and stays exercised.

## The hardware cursor left the ring-3 migration, because it is switched off everywhere

Milestone 41 stage 4a listed "cursor and mouse-bounds ops over TWP" as
requirement R3, to be deferred to 4b so it would have a real caller.
Measuring it removed the requirement instead.

`DISPLAY_CAP_CURSOR` is declared by exactly one driver, `vmsvga` -- and
that driver sets `g_cursor_enabled = 0`, so it does not advertise the
capability at all by default. The reason is recorded in `vmsvga.c`: with
a RELATIVE PS/2 mouse the hardware cursor made the pointer jump, and the
configuration where it genuinely works is virtio-gpu plus virtio-input,
i.e. an ABSOLUTE pointer, which is Milestone 27a. Under the default
`-vga std` adapter there is no cursor capability in the first place.

So the hardware cursor is not untested -- it is unreachable on every
configuration this OS currently boots, and a boot log says so
(`caps: flush`, with `cursor hardware (disabled by default)`).

Two consequences. **A TWP cursor request would be a protocol path to a
capability nothing enables**, which is worse than the "only a test calls
it" problem it was meant to solve -- so R3 moves to M27a, where the
driver and the absolute pointer that make it work both arrive, rather
than to stage 4b. And **the cursor that actually exists is the WM's
software sprite**, which needs nothing from the kernel: a ring-3
compositor draws its pointer into the framebuffer it is already granted,
with its own damage, exactly as the ring-0 one does today.

Cursor THEMING is therefore independent of the migration entirely --
it is about the sprite the compositor draws, so it lives in the
compositor plus data files, with no protocol and no kernel change.

**The general lesson, which is this repo's own rule catching a plan
rather than a bug:** before designing a path to reach a capability, check
that the capability is switched ON somewhere. `tools/vm.py --vga vmware`
exists now partly because that check had no way to be run -- the
modesetting and cursor paths are both unreachable under the default
adapter, the same shape as `--cpu max` for SMEP/SMAP and
`--kvm --cpu host,+invtsc` for the TSC clocksource.

## Cursor themes: shapes are data, colours are not, and the app never sees pixels

Three layers, copied from where Windows and Wayland both ended up after
trying the alternative:

- an **app** names a shape (`arrow`, `text`, `resize-h`, ...);
- the **compositor** owns the theme and turns that name into pixels;
- the **display layer** owns any hardware plane.

Windows is `SetCursor(HCURSOR)` -- a handle, never pixels -- with themes
as `.cur`/`.ani` files named in the registry. Wayland originally had the
CLIENT supply the pixels (`wl_pointer.set_cursor`), which meant every
client had to find, load and scale the theme itself; `cursor-shape-v1`
exists specifically to undo that, letting the client name a shape and
the compositor supply the image. X11 sat in between: themes are
directories of images under `/usr/share/icons/<theme>/cursors/`, pushed
to the hardware cursor by the server. The consensus is worth copying
rather than re-deriving.

**A shape file carries COVERAGE, not colour** -- an outline mask and a
fill mask, coloured by the compositor at draw time. That is X11's classic
image+mask split, and it keeps the two things people change most often
independent: one shape set serves a light theme and a dark one, and a
colour change needs no new art. The arrow in this WM was already built
that way (two baked alpha arrays) before themes existed, so the format
followed the code rather than the other way round.

**The files are plain text**, a key=value header plus two grids of one
hex digit per pixel. There is no cursor authoring tool in this OS and
will not be one soon, so the format a person can open in `edit` beats
the compact binary that needs a host-side tool -- and the shape is
visible in the file. `tools/gen_cursors.py` generates the shipped themes
and doubles as the authoring path; it EXTRACTS the arrow from
`wm_render.c`'s own arrays rather than duplicating them, so the shipped
theme cannot drift from the built-in fallback.

**Scaling is integer nearest-neighbour.** A pointer wants a hard edge;
a smoothly scaled mask reads as blurry rather than large, and a
pixel-value test gets much harder to write. The size is its own
registered setting rather than being derived from `font_size`, because
pointer size is an accessibility choice people make independently of
text size.

**The built-in shapes stay as the floor.** A missing or malformed theme
file costs its own shape, not the pointer -- the same vendor-default /
`/etc`-override pattern `api/config_file.h` already uses. That floor is
also the trap: it means a theme that loads NOTHING still draws a
perfectly good pointer, which is exactly how the first working version
shipped with 0 of 6 shapes loaded and looked correct. Every check in
`tools/cursor_theme_test.py` therefore asserts on the load count or on a
pixel DIFFERENCE between two states, never on a cursor being present.

**Nothing here touches the kernel**, which is the point: the compositor
draws the pointer into the framebuffer it already owns. So the whole
mechanism moves to ring 3 with the WM, and it is independent of the
hardware cursor (which is switched off everywhere -- see the entry
above).

## A whole-file read is not re-entrant, and the VFS refuses the second one

`fs_read()` returns a pointer into the backend's own staging buffer.
Every backend implements it identically (`g_read_buf` in `tfs3.c` and
`tfs.c`): free the shared buffer, allocate one the size of the file,
then do a BLOCKING read into it.

The kernel context is a scheduler participant, so the WM can be
preempted in the middle of that read. A ring-3 process then makes a
syscall that also reads a file -- `etc_config_get()` on a setting, say
-- which frees the buffer the suspended read is still writing into and
allocates a smaller one for itself. The first read resumes and writes
past the end of somebody else's allocation.

That is not a theory. `heap debug on` caught it as a red-zone violation
on a 96-byte block whose right red-zone contained `ame=Calc` -- the tail
of `Name=Calculator`, from a `.desktop` file the desktop was loading
while Control Panel wrote a setting. The panic itself landed in
`split_block()`, allocating from the free list the overflow had already
corrupted, which is why the RIP pointed nowhere near the culprit.

**`fs_read()` refuses a nested call** (`vfs.c`), returning NULL as it
already does for a missing file. One guard, both backends, and it turns
silent corruption into a logged refusal. The alternatives were weighed:
a buffer per caller removes the class of bug rather than guarding it,
but it is a different API and every caller changes -- worth doing if
whole-file reads ever become common, not worth doing in the change that
stops a crash.

**The cost, stated because a caller cannot see it**: a refusal is
indistinguishable from "no such file" at the call site, so a caller that
reports "missing" may now be reporting "busy". Hence the klog line.

**The lesson that generalises past this kernel**: hundreds of small
writes should be survivable, and on Linux or Windows they are. The
storm that exposed this -- a UI applying a setting on every
pointer-motion event -- was a bug of its own, but fixing only the storm
would have hidden the corruption until something else interleaved two
file reads. When a pathological workload triggers a crash, fix the crash
first and the workload second.

## The panic names the function, from a table baked into the image

A panic used to print a bare address, and the kernel relocates itself to
a random base at boot -- so reading one meant scrolling back to the boot
banner for the relocation offset, subtracting by hand, and running
`addr2line` on another machine. In practice that meant a panic arrived
as a photograph and cost a round trip.

`tools/gen_syms.py` bakes the function symbols into a `.ksyms` section,
and the panic prints `in crash_gp_fault+0xa` plus a named stack scan.

**Three decisions inside that are worth keeping.**

*Placement.* `.ksyms` sits after everything it records, exactly as
`.krelocs` does, so adding the table cannot move a single address in it
and a first link pass's addresses stay valid in the second. `--verify`
re-derives it from the final image and fails the build if that ever
stops being true -- the expensive way to find out is a panic naming the
wrong function, which is worse than no name at all.

*No pointers in the blob.* A table of `const char *` or `&function`
would add one relocation PER SYMBOL to the ~8,000 the kernel already
patches at boot. So addresses are link-time values in u32 literals and
names live in a string table addressed by offset; the resolver subtracts
the relocation delta itself.

*A stack scan, not a frame-pointer walk.* The kernel builds at -O2,
which omits frame pointers, so an RBP chain would be fiction. The scan
overreports -- stale return addresses from earlier calls are still on
the stack -- and the output says so. Overreporting with a caveat beats
nothing.

The register dump earns its place for a specific reason: a #GP has no
CR2, so unlike a page fault there is no address to consult, and the bad
pointer is usually sitting in a register. The first real panic printed
`RAX=deadbeefdeadbeef`, which was exactly the address that faulted.

## Deliberate kernel faults are a table, gated by a boot flag

The panic path is the one path a kernel cannot exercise by accident and
must not get wrong. Validating a panic report used to mean editing a
debug command to dereference a bad pointer, rebuilding, and taking the
change back out -- which is why `kernel/core/crashtest.c` exists. Linux
ships the same idea for the same reason (`lkdtm`, sysrq-c).

**The kernel owns the LIST, not just the triggers.** A GUI app, a test
tool and any future front end enumerate it through `SYS_CRASHTEST`
rather than hardcoding, so a fault kind added in the kernel appears
everywhere with no edit -- the same rule the settings registry and the
`.desktop` entries already follow.

**It is gated on `faultinject`**, off by default, in the same style as
`nokaslr`/`nopat`/`notsc`/`ata nodma`: each of those exists so a path
that is otherwise unreachable can be reached deliberately, and a panic
on demand is exactly that. The refusal is what makes the gate testable
in the ordinary suite -- `tools/crashtest_test.py` runs in
`gui_regress.py` precisely because the dangerous half is closed.

The table covers RING-0 faults only. A ring-3 program needs no help to
dereference NULL; the Crash Test app performs those itself, which is
also what lets it demonstrate the contrast -- a ring-3 button ends the
process and the desktop carries on, a ring-0 button ends the machine.

## The desktop's app list is a directory of files, not a table in the kernel

`apps/gui_apps.c` held a C array of every app on the desktop, so adding
one meant editing the kernel and rebuilding it -- for a ring-3 program
that the kernel otherwise knows nothing about beyond a path to spawn.
With M41 moving the apps out of ring 0 one at a time, that table was
also the thing that would need editing on every single stage.

The list is `/usr/wm/desktop/*.desktop` now, one entry per app, scanned
at desktop startup: freedesktop's idea, and near enough its file format
that the entries are readable to anyone who has seen a Linux one
(`Name=`, `Exec=`, `Icon=`, `Categories=`). The parser is
`etc_config`'s, already in the tree for `/etc/toyos.conf` -- key=value
lines with `#` comments is exactly the format, so no second parser
exists. **Adding an app to the desktop is dropping a file in
`data/wm/desktop/`.**

`Exec=builtin:taskmgr` is the deliberate bridge: it names a
kernel-space app's callbacks rather than a binary, so the two apps that
cannot move to ring 3 yet (Task Manager and Control Panel need syscalls
that do not exist until M41 stage 4) live in the same list as everything
else. That form disappears when the last one moves, and nothing else
needs to change when it does.

Windowed binaries went to `/bin/wm/{system,apps,demos}/` at the same
time, the class taken from the SOURCE directory (`userland/gui/<class>/`)
the way `userland/gui` already meant `/bin`. The category a user sees is
therefore a property of where the code lives, not a string repeated in
two places that can disagree.

## Widgets are added once a second real caller needs them -- except the checkbox

`apps/widgets.h`'s standing rule (stated in its own top comment) is:
add a new widget primitive only once a second independent hand-rolled
implementation of the same idea turns up, not preemptively. Every
widget through build 480 followed that -- `widget_button` consolidated
three existing button implementations (wm.c's title-bar buttons,
Calculator's grid, Notepad's toolbar), `text_scrollback` and the
scrollbar widgets grew out of apps/terminal.c and were then reused
by apps/notepad.c and apps/editor.c. Build 490's `widget_textfield_*`
kept that pattern (Notepad's fixed filename was the identified real
need). Its `widget_checkbox_*`, though, was added explicitly ahead of
any real caller, by direct user request when asked to choose the
scope -- a deliberate, acknowledged exception to the rule above, not a
change to it: the rule still applies to whatever gets added *next*.
the commit for build 490 for the full writeup.

`ui_radio_list`, and later `ui_listbox`/`ui_dropdown`, were added the
same way -- ahead of a second caller, by explicit user request. Worth
being honest that this is now the majority of the recent additions
rather than a one-off exception, so the rule is doing less work than
its wording suggests. What it still buys is the *shape*: each of these
arrived as a real widget with its behaviour inside it rather than as
a helper an app calls, which is the part that actually prevents the
half-implemented second copy (see `docs/gui-guidelines.md`'s
"Behaviour belongs to the component"). `ui_dropdown` is a data point
for that: it needed a scrolling, keyboard-navigable, hover-tracking
list, and composing `ui_listbox` meant writing none of it twice.

## A table PULLS its rows, and stores none of them

`uui_table` (`userland/ui/uui_table.h`) holds no data. It asks the app
for one cell at a time through a `cell(ctx, row, col, out, cap)`
callback, and the app supplies `row_count`.

The obvious alternative -- hand the widget an array of rows -- needs
somewhere to put a copy, and Toykit has no allocator (which is also why
a menu is a const tree). Task Manager re-reads the whole process table
several times a second; copying it into the widget each time would mean
either a fixed maximum baked into the widget or an allocator built to
serve one caller.

Pulling has a second property that matters more than the memory: there
is no cached state to invalidate. Processes appear and exit under a live
view, and the table simply asks again on the next paint. A widget that
owned rows would need the app to remember to push updates into it, and
the failure mode of forgetting is a table showing plausible, stale
numbers -- which is worse than an empty one, because nothing looks
wrong.

What the app still owes: `uui_table_set_rows()` on a refresh, which
clamps the scroll position and the selection. That is a function rather
than an assignment precisely because both need clamping when the row
count shrinks.

## cpu_ns is a total, not a percentage

`struct proc_info.cpu_ns` (`kernel/include/abi/proc_info.h`) reports the
cumulative time a process has been the running one for. It does not
report a percentage, and the kernel deliberately does not compute one.

(It was `cpu_ticks`, a count of timer interrupts, until 2026-08-17. The
"total, not a percentage" argument below is unchanged by that; what
changed is that a total of TICKS is not a duration, which is a separate
entry -- see the clocksource one.)

A percentage is a difference between two samples divided by the time
between them, and only the consumer knows how far apart its samples
are. Reporting a percentage would bake the kernel's chosen interval into
the ABI, and any reader refreshing at a different rate would then be
reading a number that means something other than what it says. Linux
makes the same call with `/proc/stat`, which reports totals and leaves
the arithmetic to `top`.

The consequence is that a consumer needs a common clock for the
denominator, which is what `SYS_MONOTONIC_NS` is for -- the SAME
clocksource the scheduler bills against. Dividing a `cpu_ns` delta by a
delta of anything else (the RTC, say, or `SYS_TICKS`) gives a ratio of
two unrelated clocks that happens to look like a percentage.

## SYS_KILL is unprivileged, and killing the WM is the point

Any process may kill any other, including the window manager once it is
a ring-3 process. There is no permission check, and that is a decision
rather than an omission.

There is nothing to hang a check on. This kernel has no user model, no
capability system and no process groups, so a permission test would have
to invent the very concept it claims to enforce -- and a check that
always passes is worse than no check, because it reads as protection.
The header says so plainly and names `SYS_KILL` as the first syscall a
future privilege model has to gate.

Protecting the window manager specifically was considered and rejected
for a sharper reason: Milestone 41 stage 4's stated exit criterion is
that **killing the WM process must not panic the kernel**. Being able to
do it from Task Manager is a way to exercise the property the milestone
exists to establish, not a hole in it.

The polite counterpart is `WIN_REQ_CLOSE_PID`, which asks the target's
window to close and can be refused -- it runs the same
`wm_request_close()` the X button and Alt+F4 use, so there is no fourth
close path with its own rules. Windows draws the same line between End
Task and End Process.

## A lone button routes its own clicks; the group is for grids

`uui_button_ops` used to have `draw` and `hit` and nothing else, so a
single declared button was painted, hit-tested, and ignored every click
-- routing lived only in `uui_button_group`. Even one button therefore
needed a group wrapped around it.

That is not how the toolkits this is modelled on behave. A `QPushButton`
and a Win32 `BUTTON` both handle their own click; Qt's `QButtonGroup`
exists for EXCLUSIVITY (radio behaviour), not for delivering the press.
It is also a silent trap of exactly the kind this repo keeps paying for:
the button draws correctly, hit-tests correctly, and does nothing, with
no error anywhere to point at the cause.

So `uui_button_ops` gained `press`/`motion`/`release`, implementing the
same arm-on-press, commit-on-release, don't-commit-if-dragged-off rule
the group implements. The change is ADDITIVE -- the group still routes
its own buttons, so Calculator's keypad is untouched.

What the group is still good for is a GRID of many buttons handled as
one widget, which is genuinely less code than twenty layout items. It is
on `docs/roadmap.md`'s list to retire once that is no longer worth a
separate widget.

## A natural size must not depend on where the widget currently sits

`uui_button_group_natural_size()` computed its buttons' far edge FROM
THE ORIGIN -- `x1`/`y1` started at 0 and took the max of `b->x + b->w`.
That is the union's extent only while the group sits at (0,0), which
held for exactly as long as nothing ever moved a group.

The moment one took part in a layout it broke: placed at y=284, the
group reported a natural HEIGHT of 312 -- its offset plus its size. In a
column that inflated the space the layout believed its children needed,
so the growth allowance for the widget above it was eaten by a number
that was really a coordinate. The visible symptom was Task Manager's
table growing 16 px against a 300 px window resize, which reads as a
broken resize path rather than as a broken measurement.

The rule: a natural size is the size a widget WANTS, asked before anyone
has decided where it goes. One that varies with the widget's current
position is a feedback loop between layout and measurement, and it
converges on a wrong answer rather than failing outright.

It reports `max - min` per axis now. Callers whose widgets start at the
origin are unaffected, which is why nothing caught it earlier.

## A widget's ops->hit is a boolean, and a row index is not one

`uui_route.c` tests the slot as `!it->ops->hit(...)`. A widget whose own
`_hit()` returns a ROW INDEX therefore has to convert -- because row 0
is the one row whose index is falsey, so it reports "not hit" and cannot
be clicked, while every other row works.

`uui_listbox` shipped that way. The failure is close to invisible: the
widget draws, scrolls, highlights on hover and selects rows 1..n
perfectly, and only the first row is dead. It survived a 42-check test
tool. It was found by building `uui_table`, which copied the line and
reproduced the bug, and only then failed loudly enough to trace --
Task Manager's first row is the one a test naturally clicks.

`uui_radio_list` had `>= 0` all along; every other widget's `_hit()`
returns `uui_hit()`, which is already a boolean. So the trap only
applies to widgets that report WHICH item was hit, and there are exactly
two of those.

## Sorting: the widget owns the order, the app owns the comparison

`uui_table` sorts on a header click. The split is the decision, and it
is the one Win32, Qt and GTK all make:

- Win32's ListView sends `LVN_COLUMNCLICK` and the app calls
  `ListView_SortItems()` with a comparator; the CONTROL permutes its
  own items.
- Qt's `QSortFilterProxyModel` keeps a row mapping and compares through
  `lessThan()`, which the app overrides.
- GTK's `GtkTreeSortable` takes a sort function per column.

So an app supplies one `uui_table_cmp_fn` and gets the clickable
header, the arrow, click-again-to-reverse, and keyboard motion in the
sorted order without writing any of it. Task Manager's whole cost is a
13-line `compare_rows()`.

**Why the widget cannot just sort what it draws**, which is the obvious
alternative and the one that needs no app code at all: this table's
cells are FORMATTED STRINGS pulled through a callback. Sorting them
puts "10" before "9" and orders "4 KB" against "1 MB" meaninglessly --
three of Task Manager's five columns would be silently wrong. Qt's
default comparison only works because its models hand back typed
values; ours hands back display text. Comparing has to happen on the
app's real data, which only the app can reach.

**Why the widget owns the permutation rather than asking the app to
reorder its own rows.** The table stores no rows at all (see the entry
on pulling rows), so it has nothing to sort -- but it does own
`selected`, `hovered` and `top`, all of which mean different things
before and after a reorder. Keeping the mapping inside is what lets
every public row index on the widget stay an APP row: a selection
survives a re-sort instead of jumping to whatever landed in that slot,
and an app that sorts is otherwise unchanged.

The cost is a fixed `int order[UUI_TABLE_MAX_ROWS]` (256), because
Toykit has no allocator. Past that the tail shows unsorted rather than
being dropped -- a visible oddity beats a silent one, and Task
Manager's ceiling is `SYS_PROC_MAX` (64).

**The insertion sort is deliberate**, not a placeholder: it is STABLE,
which is what makes a second sort on another column keep the previous
column's order within ties -- the behaviour every desktop table has --
and n is bounded at 256. There is no `qsort` in this toolkit.

**Two bugs this turned up, both silent.** `tb_ops_hit` routed on
`uui_table_hit()`, which deliberately excludes the header and the
scrollbar column because it answers "which ROW" -- so the router never
delivered a press to either, and a header click reached nothing at all.
And `table_reveal()` compared `selected` (an app row) against `top` (a
view offset), which are only interchangeable while unsorted.

## The ring-3 UI Demo selects on contact where the kernel one committed on release

Milestone 41's stage 0 moved UI Demo to ring 3, and with it the 28
checks that are most of what proves the widget set works. Three of those
checks changed meaning, and the reason is worth stating once so a future
session does not "fix" the app back:

**A listbox row and a dropdown item act on CONTACT** in `userland/ui/`,
where the kernel widgets armed on press and committed on release. That
is not a regression in the port -- it is the ring-3 widgets' existing
contract, already shared by four shipping apps, and it is what Windows
and GTK do with a list. `docs/gui-guidelines.md`'s commit-on-release
rule is about controls whose action is NOT already visible: a button
fires something invisible, so a press dragged away from it must do
nothing, and UI Demo still asserts exactly that for its buttons. A
selection is visible as it happens, and a user who presses the wrong row
sees the wrong row highlighted rather than triggering something.

**A closed dropdown takes only the keys that OPEN it** (Down, Enter,
Space), where the kernel one cycled its value with the popup shut. The
ring-3 behaviour is the better one and was kept deliberately: an arrow
key cannot change a setting the user cannot see.

**The button group is not a focus stop.** Ring 3's group has no
keyboard activation, so a tab stop there would be a stop that does
nothing. The tab ring is textbox, dropdown, listbox.

What the port did NOT change is the layout, the widget list, or the log
grammar -- so the other 25 checks are the same assertions against
different code, which is the point of having moved the target.

## A compositor's view of a window is at a DERIVED address, and revocation is the feature

Milestone 41's stage 1 lets one process's window buffer be mapped into
another's address space, so a ring-3 window manager can composite
windows it does not own. Two things about its shape are worth stating
because both look like details and neither is.

**The address is derived, not returned.**
`win_compositor_vaddr(owner_pid, window)` is a formula, exactly as
`win_buffer_vaddr(window)` already was for a client's own buffer. The
map call reports the address anyway, so the value has one definition at
the call site rather than two -- but the compositor could compute it.

Three things fall out of that, and the third is the reason:

  1. A resize reallocates a window's frames and re-maps them at the
     SAME address, so a compositor's pointer survives a resize it did
     not initiate and never has to be told the pixels moved. This is
     the trick that made client-side resize simple, reused.
  2. There is no per-mapping table to keep in sync, so the two sides
     cannot disagree about where a window is.
  3. **The kernel can revoke a mapping without being told where it is.**
     That is what makes "the mapping is gone after the client died" a
     checkable property rather than a claim resting on bookkeeping.

**Mapping is three lines; revocation is the whole design.** Frames stop
belonging to a window on four separate paths -- an explicit destroy,
the client dying (a different code path: process teardown calls
`win_server_client_gone()`), a resize that reallocates, and the
compositor itself unregistering or exiting -- and every one of them has
to unmap BEFORE the frames go back to the allocator. Miss one and the
compositor reads memory that now belongs to something else, which
appears as flickering garbage inside one window and gets diagnosed as a
drawing bug for as long as that takes.

That is why the KTESTs assert against `vmm_validate_user_range()` --
the page tables -- rather than against the server's own `comp_mapped`
flag. The flag is precisely the thing that would be wrong. The recorded
positive control (disable revocation on destroy; exactly two checks go
red, on the page-table assertion) is in
`kernel/proc/win_server_test.c`.

**Why the compositor's address space is captured at registration**
rather than looked up per call: a mapping must not depend on which
process happens to be current when the request arrives, since a batched
or shared-ring transport breaks that assumption -- the same argument
`win_server_ops` makes about taking `pid` explicitly. It also makes the
path reachable from a KTEST, which has no processes to look up, and
that is the only reason these properties are tested at all.

## Revoking a compositor's window mapping leaves the zero page behind, not a hole

Force Quit killed the ring-3 desktop, deterministically, and the fault
names the whole design problem: `CR2` was
`win_compositor_vaddr(victim_pid, 0)` and `RIP` was `ugfx_blit`.

The chain. The WM calls `sys_kill()`; the kernel runs
`win_server_client_gone()` -> `destroy_window()`, which unmapped the
compositor's view of the dying window and freed its frames
SYNCHRONOUSLY, and only QUEUED `WIN_EV_CLIENT_DESTROYED`. The syscall
returns to a compositor whose own window list still names that window,
and its next frame blits from the address the kernel just took away.

The asymmetry was documented in `abi/win_proto.h` -- "by the time the
compositor reads it the buffer is gone" -- and stating it did not make
it survivable. It was free while the WM was ring 0, where the teardown
and the compositing were the same thread of control in one address
space. A compositor that is a PROCESS cannot be held inside a kernel
teardown, so there is no moment at which "the mapping may go now" is
true.

**So a revoked slot is remapped to a single shared, read-only zero page
rather than unmapped** (`comp_poison()` in `kernel/proc/win_server.c`).
The invariant is: while a compositor is registered, a window buffer's
slot in its address space is never a hole. A stale blit reads black for
at most one frame, until the queued event is drained and the window
leaves the compositor's list.

Note this is NOT a use-after-free -- the frames really are freed and
handed back to the allocator, which is the property the original unmap
existed to guarantee. The rejected alternative, leaving the real
mapping in place, IS one, and it fails as garbage inside a window that
gets diagnosed as a drawing bug.

Three things it costs, all small and all in the same file. The poison
has to be taken back out before real frames are mapped over it
(`comp_unpoison()`, called from `comp_map()`) -- **because
`vmm_map_user_page_type()` does not invalidate the TLB when it replaces
a PRESENT entry**, so a live compositor would go on reading zeros from a
window that draws perfectly. The page count of the DEAD window is what
must be unmapped, so it is recorded per window rather than recomputed
from the new one. And a compositor losing the role drops its poison
along with its mappings, since both live in an address space that is
about to stop existing.

Read-only rather than writable on purpose: a compositor composites OUT
of a client buffer and never writes one, so a write there is a bug worth
faulting on rather than absorbing into a page every dead window shares.

The refcounted alternative -- keep the frames until the compositor
releases them, which is Wayland's `wl_buffer.release` -- is the more
general answer and was rejected as too much machinery for the problem:
a new protocol message, lifetime tracking on both sides, and a
compositor that never acknowledges leaks frames indefinitely. Poison
needs one frame and no protocol.

Force quit was the deterministic case, not the only one: the same
unmap runs when any client exits on its own, and a ring-3 compositor
preempted mid-blit could always have faulted on it. That race is what
the invariant closes.

## Kernel stacks are 16 KiB, guarded, and canaried -- and the syscall dispatcher is why they had to be

A `/bin/config` write from the ring-3 desktop panicked the machine with
a #GP on `iretq` in a process that had done nothing wrong. The chain
took a day to find and none of it was where the symptom pointed:

  * The spawned child's syscall path -- `SYS_SETTING` -> the setting's
    `apply` -> `etc_config` rewrite -> VFS -> TFS3 journal -> ATA --
    used more than its whole 8 KiB kernel stack. Measured with a
    canary-painted stack: **8192 of 8192**, i.e. it ran off the bottom.
  * `kstack[]` was the LAST member of `struct sched_process`, so past
    the bottom lay ~670 bytes of that slot's own fields and then the
    PREVIOUS slot's stack -- including the saved trapframe sitting at
    its top.
  * The overflow zeroed that trapframe. The victim was the window
    manager, which was resumed from it on the next switch and `iretq`'d
    into CS=0, RIP=0.

**The single biggest consumer was `syscall_dispatch()` itself: a 4832-byte
frame**, because it is one long if/else chain and GCC does not overlap
the message structs of branches that cannot run together. Nearly 5 KiB
gone on every syscall entry before any handler starts. That was found by
adding `-Wframe-larger-than` -- not by reading the code, which several
sessions had done.

Four changes, which together are what Linux and Windows both do:

**16 KiB stacks.** `THREAD_SIZE` on x86-64 Linux, and Windows' x64
kernel stack. Linux ran 8 KiB for years and raised it for the same
reason: depth here is many medium frames, not one big one.

**A guard page below each stack.** The stacks moved out of
`struct sched_process` into their own page-aligned array with an
unmapped page beneath each, so an overflow FAULTS. This is
`CONFIG_VMAP_STACK`; before 4.9 Linux had precisely this bug, stacks in
the direct map with the next task's data underneath. `paging_unmap_kernel_page()`
splits the 2 MiB identity mapping to do it, and must run after
`paging_enforce_wx()`, which rewrites every PDE.

**A double-fault handler on an IST.** Without it the guard page is worse
than useless: RSP lands on the guard, the push that would report the #PF
faults too, and the CPU triple-faults -- a silent reboot. `tss.ist[0]`
existed as a field and had never been populated. This is the only gate
that needs one.

**A canary at each stack base**, checked on every switch
(Linux's `STACK_END_MAGIC` / `CONFIG_SCHED_STACK_END_CHECK`). It covers
what the guard page cannot: a frame big enough to STEP OVER the guard
and land in the neighbour anyway.

Plus the preventive half: `-Wframe-larger-than=1024` for `kernel/`
(2048 for `apps/`, which runs on the kernel context's own stack rather
than a per-process one).

**`syscall_dispatch()`'s 4832 bytes turned out to be ONE local, and the
obvious suspects were all wrong.** It was waived by name at first, with
the extraction scheduled; when that was done the cause was a 4 KiB
`unsigned char rbuf[SYS_GETRANDOM_MAX]` in the getrandom branch. Every
other local in the function -- a dozen message structs, four separate
KiB-sized bounce buffers -- overlapped in its shadow, so extracting them
first changed the total **by nothing at all**, twice. `-fstack-usage`
answered it in one command after two rounds of reasoning about which
struct was biggest.

The lesson generalises past this function: **a frame is not the sum of
what you can see; ask the compiler.** GCC overlaps locals whose live
ranges are disjoint and refuses to once an address escapes, and which is
which is not visible by reading. The dispatcher is ~864 bytes now, the
KiB-sized branches are `noinline` handlers that pay for themselves only
when they run, and their bounce buffers come from the heap. Measured end
to end, the deepest path in the kernel (a setting write through the TFS3
journal to ATA) went from **8680 bytes to 4456** -- from 53% of a kernel
stack to 27%.

**Dynamic growth was considered and rejected**, and no mainstream kernel
does it: growing on demand means taking a fault with no usable stack, so
every kernel path must tolerate a fault at any push. Linux stacks are
fixed; a 2024 dynamic-kernel-stacks RFC is not merged. Windows' one
escape hatch is `KeExpandKernelStackAndCallout()`, where a driver
explicitly runs one callout on a temporarily bigger stack. Growth also
hides depth problems rather than surfacing them, and surfacing them is
what the frame check and the canary are for.

Two things the reporting needed, both found by running the control
(shrink back to 8 KiB and re-run the original repro):

  * **The panic's stack scan faulted inside the panic handler**, because
    an overflow leaves RSP on an unmapped page and the existing
    plausibility check only rejected wild values. It checks each page is
    present now.
  * **The scan starts from the stack's BASE after an overflow**, not
    from RSP. RSP is on the guard page, so scanning from it yields
    nothing -- and the call chain that got too deep is the entire
    actionable content. It now prints `resolve -> tfs3_size ->
    read_inode -> atac_read`, which names the path to shorten.

**The legacy loader had its own stack, and fixing only the scheduler's
left the bug reachable from the likeliest place to hit it.**
`process.c`'s `g_legacy_kstack` was a bare 8 KiB array with a comment
claiming it was "sized to match the scheduler's own per-process
kstacks" -- true when written, false the moment those changed. `run` and
`config set` typed at the physical shell go through THAT path, so
`config set cursor_size normal` double-faulted the kernel on committed
`HEAD`, with the panic itself faulting on the way out. Both owners share
`kernel/kstack.h` now, which exists precisely so the next size or policy
change cannot apply to one and not the other. **When a comment asserts
two things are the same size, check whether anything enforces it.**

**Measuring the high-water mark: scan UP from the base, not down from
the top.** The obvious cheap version walks down from the known peak and
stops at the first poison byte, touching only what is new. It is wrong,
and wrong quietly: real stack data contains `0xAA` bytes, so it stops at
the first coincidental one. Measured -- a path that genuinely used 8680
bytes reported **184**, exactly one trapframe, because byte 185 happened
to be `0xAA`. Linux's `stack_not_used()` scans from the bottom for the
same reason. The known peak bounds the walk instead, so it shortens as
the peak grows.

**And attribute only the syscall that GREW the mark.** The peak is a
property of the stack, so recording it at every syscall exit credits
every later call with the deepest one's number -- `write` looked as
expensive as the setting write that really did it. `kstack syscalls`
now reports the one syscall that pushed the water line down: for the
overflow above, `#34` (`SYS_SETTING`) at 8680 bytes and nothing else.

## The third inert scrollbar: drawing one and handling it are separate jobs

`uui_listbox` drew a scrollbar and handled none of its input. Not
"handled it badly" -- a press on the strip did not reach the widget at
all, because `uui_listbox_hit()` deliberately excludes the bar column so
that clicking it cannot select a row. Dragging the thumb did nothing,
clicking the track did nothing, and inside a dropdown popup a click on
the bar fell through to the dismiss branch and CLOSED the popup, which
is the most annoying possible answer to "I tried to scroll".

Found by the maintainer dragging it on the desktop, after a 28-check
suite passed clean. That is the third time this project has shipped a
bar that draws and does nothing (`apps/ui/ui_textview.h` records the
first two), and the recurrence is the interesting part, so:

**Why it keeps happening.** `uui_scrollbar` is deliberately a stateless
drawing-and-hit-testing primitive -- correct design, and the reason
`ui_textview` and `uui_textview` can compose it. But it means DRAWING a
bar is one call and MAKING IT WORK is a separate set of them, and a
control that does the first and not the second looks finished. Every
screenshot is right. Every "does it respond" check passes, because the
rest of the control responds.

**What actually stops it.** `docs/gui-guidelines.md`'s scrollbar section
gained a point 9: a control that draws a bar must handle one, and the
check has to assert the view MOVED. An absence check cannot catch this
-- "the drag changed no selection" passes perfectly against a dead bar,
and it stayed green under the positive control while the four real
checks went red. That is the general lesson worth keeping: when a
control does nothing, every assertion of the form "X did not happen"
still passes.

The fix put the behaviour in the widget (`uui_listbox_press()` /
`_drag()` / `_drag_end()`), not in UI Demo -- behaviour belongs to the
component, so the four shipping apps that use a listbox and every
dropdown popup get it too, rather than each app growing its own copy
and one of them getting the grab offset wrong again.

## The toolkit routes pointer input; an app configures and is told what changed

Every ring-3 app used to dispatch mouse input by hand: try the dropdown,
then the listbox, then each button; remember whether a button is down so
motion means "drag"; remember to call drag_end on release. The rule this
violated is this project's own -- behaviour belongs to the component
(`docs/gui-guidelines.md`) -- and input handling is behaviour.

The measurement that settled it, taken before any code changed:

| app | widget-input forwarding calls |
|---|---|
| Calculator | 0 |
| Terminal, winclient, uiclient | 0 |
| Shapes | 1 |
| Notepad | 3 |
| UI Demo | **21** |

Calculator wrote NONE because `uapp_desc.buttons` already routed one
widget type for it. So the model was proven and simply stopped at button
groups; UI Demo paid twenty-one calls for using anything else.

**The cost was never verbosity, it was silence.** A widget whose input
an app forgot to forward is not a compile error and not a visible
defect: it draws correctly and does nothing. That is exactly how
`uui_listbox` shipped a scrollbar that could not be dragged.

**What replaced it.** `uui_widget_ops` gained `press`/`motion`/
`release`/`wheel`, `uui_route.h` walks the same `struct uui_item` array
a layout already holds, and `uapp` calls it before the app's own
callbacks. An app declares widgets with ids and gets
`on_widget(app, id, reason)`; it reads the new value from the widget
(`uui_dropdown_selected()`, `cb.checked`, `list.selected`). UI Demo went
from 21 forwarding calls to none -- what is left there is hover
REPORTING for the tests, which is the app's own business.

Three decisions inside it worth keeping:

- **The pointer GRAB.** Whoever consumes a press gets every motion and
  the release, wherever the cursor goes. One rule, and it removes all
  the per-app drag bookkeeping: a thumb drag that leaves the scrollbar
  keeps scrolling, and a button dragged off still receives its release
  and so can decline to commit.
- **`overlay_active`.** An open dropdown popup is drawn outside its own
  rect, so `hit` cannot route it. A widget declaring an overlay is
  offered every press first -- input order being the reverse of draw
  order, stated once in the router instead of re-derived by each app.
- **An id plus a REASON, not a per-widget callback.** The id is the
  app's (no allocator here, and one switch reads better than a callback
  pointer on every widget struct). The reason names the input that
  arrived -- press, motion, release, wheel -- which is the difference
  between "scrolled with the wheel" and "dragged the thumb", and cannot
  be recovered from widget state afterwards.

One deliberate behaviour change fell out: **the wheel goes to the widget
under the cursor**, not to whichever widget the app tried first. That is
what a user expects, and it is why `uapp` tracks the last pointer
position -- `WIN_EV_WHEEL` carries notches and no coordinates, exactly
as Wayland's axis event does, because the client already knows where its
pointer is.

## What editing text MEANS lives in one place, and the storage does not

Editable text existed three times over and behaved differently in each:
`uui_textbox` (single-line) had a caret and NO selection at all, `utext`
(multi-line) had selection primitives but no keymap, and Notepad
hand-wrote the keymap on top of them -- about sixty lines deciding what
Ctrl+A means, what Shift+Left does, and what typing with a selection
active should do. A second app wanting a text field would have written
that a fourth time.

`uui_edit` (`userland/ui/uui_edit.h`) owns the CURSOR, the SELECTION and
the KEYMAP, and delegates every read or write of characters through four
accessors. That split is the whole design: a 48-byte line and an 8 KB
ring cannot share storage, but they absolutely should share what
Backspace with a selection active does. It is the same shape
`kernel/lib/klineedit.c` already has kernel-side, where the physical
shell and the GUI Terminal share one line editor and each only paints
the result.

What a text field does now, which it could not before: click to place
the caret, drag to select, Ctrl+A, Shift+arrows, typing replaces the
selection, Backspace and Delete remove it. All of that arrived in
`uui_textbox` by deleting its keymap rather than by writing one.

**Two things the core deliberately does not decide.** Enter is never
consumed -- a field commits, a document inserts a newline, and that is
the widget's call, so Notepad still handles it and the field still
leaves it to the app. And Up/Down are offered only when the caller
supplies line accessors: a single-line field has no line above, so the
core declines the key rather than swallowing it, and the app can use it
for something else.

The multi-line side kept its public API (`utext_sel_*`, `utext_cursor_*`)
because Notepad calls those directly; they delegate now instead of
reimplementing. The one visible change is `struct utext`'s three fields
becoming one `struct uui_edit ed`, so there is a single owner of the
caret rather than two structures that could disagree.

## A GUI test spawns its client directly, instead of typing at a Terminal

Eight of the thirteen GUI test tools used to open the kernel-space
Terminal and type `run <name>` at it to get a ring-3 client on screen.
Stage 0 deleted that Terminal, and the failure was not subtle: `gui open
Terminal` now spawns the RING-3 terminal, whose window does not exist
when the injected keys arrive, so ten tools failed at their first check
at once.

They use `gui spawn` now, via `DebugConsole.spawn(path, title)`, which
waits for the window and gives the client a moment to present its first
frame. That was already the documented advice -- `wm_debug.c` added `gui
spawn` precisely so a test would stop dragging a Terminal's allowlist,
its single pending-process slot and its shell into a test about
something else -- and stage 0 is simply what forced it.

Two traps the conversion exposed, both of which read as widget bugs:

- **A window in the WM's list is not a window with pixels.** The
  Terminal-typing path had enough incidental latency to hide this;
  spawning directly does not. A capture taken too early reads DESKTOP
  through the window's rect, which scored as ~230k "ink" and made every
  later comparison meaningless. Wait for the app's own layout line where
  it has one.
- **A klog line can land in the MIDDLE of a `--json` reply.** The
  console has no per-writer buffering, so a process exiting while `gui
  windows --json` is printing splices its message through the object.
  `DebugConsole.json()` re-asks rather than trying to unpick the
  fragment: the splice is a collision, not a property of the answer, and
  a repair heuristic would silently accept genuinely malformed output.

## A client's menus are clamped to its own window, and that is one rectangle away from not being

`userland/ui/uui_menubar.c`; landed with the menu bar, see
the git history.

On Windows, a popped-up menu is a real `HWND` of the built-in `#32768`
class, positioned in SCREEN coordinates and constrained against the
monitor work area -- it can sit anywhere on the desktop, far outside the
window that owns it, and a menu taller than the screen grows scroll
arrows. On KDE it is a `Qt::Popup` toplevel, which under Wayland is
literally an `xdg_popup`: a child surface with a positioner (anchor
rect, gravity, and `constraint_adjustment` flags -- flip_x, flip_y,
slide_x, slide_y, resize) that the COMPOSITOR resolves, plus an implicit
grab. Every submenu is another such surface.

A TWP client can do none of that. It draws into its own window buffer
and has no mapping of the framebuffer or of any other window, which is
enforced rather than asked for (`docs/gui-guidelines.md`). So the menu
resolves the same vocabulary -- flip to the other side, slide along the
other axis, clamp last -- against a BOUNDS RECTANGLE the caller passes
in (`uui_menubar_set_bounds`), and Notepad passes its content rect.

The point of writing it that way rather than hardcoding the window: that
rectangle is the entire difference. When TWP grows a popup surface
(`docs/roadmap.md`, M41's `WIN_REQ_POPUP`) the widget is handed the
screen rect instead and the placement code is already correct -- one
rect, not a rewrite. The divergence is visible only on a window small
enough that a menu would have overflowed it, which is why shipping the
widget first and the protocol second was the phasing chosen rather than
building both at once.

## A menu bar opens on press, which is the one place the commit-on-release rule bends

`docs/gui-guidelines.md` says a control must not act until the button is
released over it, and `uui_menubar` opens a menu on button-DOWN anyway.
That is deliberate and it is what Windows, KDE, GTK and macOS all do:
pressing a title shows its menu at once, and you may then either release
and click an item or keep the button held, slide down, and release over
the item you want. Requiring a full click to open would break the second
gesture entirely.

The half of the rule that matters is kept -- the ITEM still commits on
release, so pressing "Exit" and dragging off it does nothing. Opening a
menu is not an action; it is showing the actions.

Worth knowing when testing it: that cancel path is the ONLY check in
`tools/menubar_test.py` that can tell a correct menu from one that
commits on press. Every other check presses and releases in the same
place, so a commit-on-press build passes all of them. Confirmed by
building exactly that and watching one check go red.

## Esc doesn't close a window; Alt+F4 does, and it is a WM shortcut rather than an app key

`userland/wm/wm.c`'s key loop and `wm_request_close()`; landed with the
menu bar's follow-up, see the git history.

Six ring-3 apps used to quit on Esc, which was always a papercut and
became a real hazard once Esc was also the key that closes a menu: one
stray press in Notepad with no menu open discarded unsaved text. Neither
Windows nor KDE closes a window on Esc. It is now app-local everywhere --
cancel a dialog, close a menu, clear a selection -- and closes nothing.

**Alt+F4 is handled by the window manager, not delivered to the focused
app.** That is what both desktops actually do: Windows routes it through
`DefWindowProc` to `WM_SYSCOMMAND`/`SC_CLOSE`, and KDE's is a KWin
global shortcut, so in neither case does the application see the
keystroke. The alternative -- deliver the key and let each app call
`uapp_quit()` -- was rejected for three reasons: every app would have to
implement it or the shortcut would silently do nothing, each
kernel-space app would need its own copy, and an app stuck in a bad
state could never be closed from the keyboard, which is precisely the
case the shortcut exists for.

**The app still decides what happens**, because the WM ASKS rather than
tears down: `wm_request_close()` sends `WIN_EV_CLOSE` to a client, and
`uapp_desc.on_close` returning 0 refuses. Alt+F4 and the title bar's X
are indistinguishable to an app -- exactly as they are on Windows, where
both arrive as `WM_CLOSE` -- so no client needed editing to gain the
shortcut, and none can tell the two apart in order to behave
inconsistently between them. A `reason` field on the close event was
considered and dropped: neither model desktop distinguishes these at the
app level, and nothing in the tree wanted it.

**Why all three closes share one function.** They did not, and the odd
one out was broken: the context menu's Close called `close_window()`
directly, destroying a ring-3 window without ever sending
`WIN_EV_CLOSE`. It survived because no test could open a context menu.
`gui rclick` and `gui ctxmenu` exist now for that reason, and
`winclient` refuses its first two close requests so each route is
measured rather than assumed.

**Encoding**: `KEY_F4` + `KEY_MOD_ALT`, not a combined `KEY_ALT_F4`
code. The Shift+arrow family got discrete codes because the terminal
encoding genuinely cannot express them; a function key has no such
problem -- nothing is folded, the modifier bits ride alongside it
already, and matching on them generalises to any future Alt+F<n>. The
driver returns on the function-key check before the Alt-prefixes-with-
ESC path, so Alt+F4 arrives as one key with the modifier set rather than
as ESC followed by something.

**Still not solved: an app that ignores the request keeps its window.**
That is the honest consequence of a polite handshake, and force-quitting
needs a not-responding timeout plus a way to kill the process. See
`docs/roadmap.md`, Milestone 41.

## Not-responding is a PING, not a close timeout -- because "refused" and "wedged" look identical to a timer

`abi/win_proto.h`'s `WIN_EV_PING`/`WIN_REQ_PONG`, `userland/wm/wm_client.c`'s
liveness section, `scheduler_kill()`. See the git history.

The obvious build is a timer: send `WIN_EV_CLOSE`, and if no
`WIN_REQ_DESTROY` arrives within N seconds, offer to force-quit. It is
smaller, needs no protocol change, and is wrong.

A client is entitled to refuse a close -- `uapp_desc.on_close` returning
0 is a documented, tested part of the toolkit, and `winclient` does
exactly that. To a timer, a client that declined and a client that is
wedged are the same observation: the window is still there N seconds
later. Acting on that means either offering to kill apps that
deliberately said no, or not offering it for apps that are genuinely
hung. There is no threshold that separates them, because the thing that
separates them was never measured.

So the server asks a question only a running event loop can answer.
`WIN_EV_PING` carries a serial; `WIN_REQ_PONG` echoes it. That is
xdg_shell's ping/pong and ICCCM's `_NET_WM_PING`, adopted for the same
reason both exist: from outside, a wedged client and an idle one are
indistinguishable -- neither draws, neither sends -- so the only honest
thing a compositor can otherwise say about an unresponsive window is
nothing.

Three details worth keeping:

**The toolkit answers, not the app.** `uapp_run()` handles the ping with
no callback and no app involvement. A check an app could forget to
answer would report every un-updated app as hung; and answering from the
event loop is precisely the right test, because an app stuck inside its
own `on_draw` never reaches that line.

**The serial is load-bearing.** Without it, a late pong from a previous
ping clears the current one -- so an app answering one round behind, the
exact behaviour of a badly overloaded app, always looks healthy.

**A hung window nobody is touching gets the title-bar mark and nothing
more.** The dialog is modal and appears over whatever the user is doing;
raising one unprompted, for a window they never interacted with, is
worse than the hang. It is offered only when they have actually asked
the window to close.

Force Quit kills the PROCESS (`scheduler_kill()`), not just the window.
Dropping the window alone would leave a process drawing into a buffer
that is no longer mapped, which is the exact hazard the close handshake
exists to avoid -- and would leave the scheduler slot held, which on a
4-slot table is four rescues before the desktop stops launching
anything.

Still not built: a general "this app is hung" indication outside a close
attempt (the ping is only sent when the WM asks a window to close, so
that is the only time the mark can appear), and any way to recover a
client that is hung but has NOT been asked to close.

## Angles are measured in turns, not radians

`kernel/lib/fixed.c`'s `fx_sin()`/`fx_cos()` take an angle where
`FX_ONE` is one FULL rotation, not `2*pi` radians and not 360 degrees.

Three reasons, in order of how much they matter here:

- **Wrapping is free.** A rotation counter that just increments forever
  wraps correctly on its own, because the units and the fixed-point
  representation agree. In radians, wrapping means a modulo against an
  irrational constant, done in fixed point, every frame.
- **The quarter angles are EXACT.** `fx_sin(FX_ONE/4)` is exactly
  `FX_ONE`. A radian API cannot promise that -- `pi/2` is not
  representable, so the answer is 0.9999-something, and every test of
  the trig has to carry a tolerance instead of an equality.
- **Pi never enters the code.** In a fixed-point path its only possible
  contribution is rounding error.

The cost is that a caller thinking in degrees converts (`deg * FX_ONE /
360`), which is one multiply and reads fine. Callers here think in
rotations anyway -- "a quarter turn per second" is the natural way to
say what an animation does.

See `kernel/lib/geom_test.c`'s first three tests, which are equalities
rather than tolerances precisely because of this choice.

## The geometry rasteriser draws through a callback, not into a framebuffer

`kernel/lib/geom.c` never touches memory that looks like a screen. Its
entire output interface is:

    struct geom_target { void (*plot)(void*, int, int, uint32_t, uint8_t); void *ctx; };

The obvious alternative -- take a surface pointer, a stride and a format
-- would be faster (no indirect call per pixel) and is what a real
graphics stack does at the bottom. It was rejected because it forces the
module to know about pixel formats, clipping and address spaces, and
each of those has more than one answer here:

- `gfx.c` plots into the kernel's framebuffer, routing opaque pixels to
  `gfx_put_pixel()` and partial ones to `gfx_blend_pixel()`.
- A ring-3 app plots into its own window surface, in a different address
  space, through `userland/ui/ugfx.c`.
- `uui_canvas` wraps that again to CLIP -- which is the case that
  justifies the design on its own, see the next entry.
- `kernel/lib/geom_test.c` plots into an array and asserts on
  coordinates, with no framebuffer, no window and no display driver
  involved at all. That test file could not exist against a
  surface-pointer API without faking a surface.

The per-pixel indirect call is a real cost. At the sizes this OS draws
-- a few thousand pixels per shape per frame -- it is not a measurable
one, and it buys four callers that would otherwise be four copies.

## The canvas widget clips in the plot callback, not by trimming geometry

`uui_canvas_line()`/`_ellipse()`/`_polyline()` do not compute the
intersection of the shape with the canvas rect. They install a plot
callback that drops any pixel outside it.

Trimming the geometry is the textbook approach and is faster: a line
clipped by Cohen-Sutherland rasterises only the pixels it will keep,
where this rasterises every pixel and discards some. It was rejected
because it needs a DIFFERENT correct implementation per shape -- line,
polyline, ellipse, filled ellipse -- and the curved cases are exactly
where clipping maths goes subtly wrong. Four implementations that must
agree with each other is the shape of bug this project has paid for
before (see the `kpath` entry: three path resolvers that disagreed).

One bounds test, at the bottom, cannot disagree with itself across
shapes. The wasted work is bounded by how far outside the box the caller
drew, which for a widget whose whole job is to hold a drawing is small.

## A Start-menu entry can be a launcher for a ring-3 program

`struct gui_app` (apps/gui_apps.h) grew one field, `exec_path`. When
it is non-NULL the entry is not an app at all -- it names a binary in
`/bin`, and `open_app()` spawns it instead of creating a window.

The registry was the only way into the Start menu and the desktop, and
it could only describe apps implemented as kernel callbacks. That was
fine until Calculator, Notepad and Terminal moved to ring 3, at which
point the desktop could not launch any of them: they were reachable
only by typing `run calculator` in a Terminal, while the Start menu
went on opening the kernel-space versions. Nothing was broken and the
Task Manager was correctly reporting `[r0]` -- it just looked exactly
like a bug, which is its own kind of defect.

Two things about the design worth keeping:

**A launcher always spawns; it never focuses an existing window.**
Single-instance behaviour (`multi_instance == 0`) works by finding a
window whose `app` pointer matches, and a ring-3 client window has no
`gui_app` at all (wm_client.c sets it to 0) -- so the WM *cannot*
enforce it here without matching on titles, which is guesswork. More
importantly it shouldn't: whether a second copy of a program may run
is the program's own decision on any real system, and a launcher
launches.

**It deliberately does not use `window_start_process()`.** That slot
exists to deliver a process's exit to a specific window's
`on_process_exit` callback, and it is WM-global -- one tracked process
at a time. A launched client has no such callback, and the window
server already tears its window down when it dies, so routing launches
through it would cap the desktop at one ring-3 app for no benefit.

## The default font size is a one-line change, and that is the point

`kernel/drivers/gfx.c`'s `cur_font_size` has moved three times now
(16x32 -> 11x22 -> FONT_SIZE_18 -> FONT_SIZE_14), each time because
there was more real UI on screen than the previous default had been
chosen against.

It stays a one-liner because every layout in the system is
font-DERIVED, not font-assuming: window sizes come from each app's
`default_size()` (called at open time with the active font), chrome
heights from `gfx_char_h()`, the desktop's column pitch from
`gfx_char_w()`, the Start menu's row height and origin from both. The
14pt change was verified by running the full GUI regression suite --
82 checks across 7 tools -- unchanged, and all of it passed.

The exception is worth knowing, because it has now cost three
re-measurements: HARDCODED PIXEL CONSTANTS IN TEST TOOLS do not
reflow. `tools/gui_flow.py`'s `TASKBAR_H`/`ITEM_H` are calibrated
numbers, and a stale one doesn't fail loudly -- it clicks the wrong
row. That is the standing argument for
`DebugConsole.menu_row(label)`/`gui menu --json`, which ask the kernel
where things actually are, over any constant a tool writes down.

## The desktop icon grid wraps, and clips its labels

Two related decisions, both forced by the registry growing from seven
entries to eleven.

**The default layout wraps into a new column at the bottom edge**
rather than being one unbounded column (`icon_row[i] = i`). An icon
past the bottom of the screen is not just invisible, it is
unclickable -- an app can be in the registry and unreachable from the
desktop, with nothing on screen to indicate why.

**Labels are clipped to the column pitch, with a ".." marker.** The
pitch is fixed (not sized to the longest label present -- one long
name would otherwise widen every column on the desktop, a tradeoff
already rejected once), and it is sized for a 13-character label:
"Control Panel" and "Task Manager", the longest that should never be
cut. Longer ones are truncated rather than allowed to overflow.

Overflow used to be an accepted cosmetic quirk, and honestly was one:
with a single column, a long label ran off to the right over empty
background and stayed perfectly readable. The moment a second column
existed it landed on that column's labels instead and made both
unreadable. Note that clipping ALONE would not have fixed it -- at the
old 76px square-cell pitch only about seven characters fit, so
"Calculator" and "Calculator (ring 3)" both became "Calcu..". The
pitch had to widen too. `gfx_draw_string_clipped()` is what does the
cutting, per `docs/gui-guidelines.md`'s first rule.

## The Control Panel's applets are a registry table, not gui_apps

**HISTORICAL (2026-08-17): `apps/control_panel.c` is deleted.** Control
Panel is a ring-3 program now (`userland/gui/system/cpanel.c`) and has
no applet table at all -- its rows come from the settings registry, so
the "adding an applet is adding a row" property below was superseded by
"adding a SETTING adds a row, from anywhere in the kernel". Kept because
the reasoning about plug-in conventions is still the reasoning that
shaped what replaced it.

`apps/control_panel.c` held a static `struct applet` table -- name,
`draw(x,y,w,h)`, `click(...)` -- deliberately mirroring
`gui_apps.h`'s `gui_app_registry[]` rather than inventing a second
plug-in convention. Adding an applet is adding a row, the same property
the app registry has, and a reader who knows one knows the other.

An applet is explicitly *not* a `gui_app`: no window of its own, it
draws into a rectangle the Control Panel gives it, and it can't be
opened from the Start menu. That was the alternative considered
(applets as ordinary apps, Control Panel as a launcher) and it was
rejected for making "Control Panel" a menu rather than a panel, and for
putting one Start-menu entry per setting.

Two things shipped with it worth keeping:

**Two applets from the start, not one.** Date & Time is the real one;
System Info is read-only and trivial. A plug-in mechanism with exactly
one plug-in demonstrates nothing about being pluggable -- the second
entry is what makes the icon grid, the drill-in and the Back button
meaningful instead of an elaborate way to show a single page.

**The applet doesn't cache its setting.** The timezone applet reads
`tz_current_index()` at draw time and writes `tz_set_index()`, which
persists to `/etc/toyos.conf` itself. A local copy would be a second
source of truth, and the `timezone` shell command can change the same
setting behind the window's back.

Two bugs caught by QMP testing rather than review, both of the "draws
perfectly, does nothing" kind: `on_click`'s coordinates are
**content-relative** while everything drawn is absolute, so the first
version hit-tested in the wrong space and clicks silently did nothing;
and the icon labels were laid out in a hardcoded 104px cell that "Date
& Time" overflows, which `gfx_draw_string()` cheerfully drew straight
over the neighbouring label (see the entry below on that function not
clipping -- this is that lesson recurring, in a new file, four days
after it was written down).

See `apps/control_panel.c`'s top comment and the git history.

## `gfx_draw_string()` doesn't clip to a width -- callers that need that do their own

`gfx_draw_string()` (`kernel/drivers/gfx.c`) takes no width parameter at
all -- it draws every character it's given, one `gfx_char_w()` cell at a
time, and only stops at a real newline or the end of the string.
Whatever clipping happens is `gfx_put_pixel()`'s ordinary
screen/window-bounds check, not anything aware of a caller's intended
box. `widget_textfield_draw()` (`apps/widgets.h`/`.c`) used to have a
doc comment claiming text longer than the field was "simply clipped the
same way every other text-drawing call in this codebase already is" --
that was never true, there's no such clipping to inherit, and a long
filename actually drew straight past the field's border into whatever
was next to it (a real user-reported bug, caught from a screenshot).
Fixed by having `widget_textfield_draw()` compute its own visible
character count from `w` and slice `tf->buf` before ever calling
`gfx_draw_string()`, sliding the visible window to keep the cursor in
view while the field is active. The lesson recorded at the time was:
`gfx_draw_string()` will not save you, budget the width yourself.

**That lesson did not hold, and the fix is now a function.** The very
next caller to draw text into a fixed box -- the Control Panel's applet
labels -- hit the identical bug, rendering `Date & TSystem Info`, in a
file written days after this entry existed. A rule that has to be
remembered at every call site is one that will be forgotten at some call
site, so the budgeting now has somewhere to live:
`gfx_draw_string_clipped(x, y, max_w, ...)` draws bounded and returns
whether the string fitted, and `gfx_text_width()`/`gfx_text_fit_chars()`
are the measurement half for callers doing their own windowing.
`gfx_draw_string()` itself is unchanged -- clipping it would alter every
existing caller -- so the guidance is now "use the clipped variant for a
fixed box" rather than "remember to do this by hand".

`gfx_text_width()` also pays a later debt: Milestone 21 (proportional
font metrics) lists exactly that function as something it needs, and
every `k_strlen(s) * gfx_char_w()` open-coded at a call site is a place
that silently breaks when a glyph stops being one cell wide. See
the git history.

**Same lesson, vertical axis:** a follow-up report caught the field's
height having the exact same problem one axis over -- `apps/notepad.c`
sized the field's row height to precisely `gfx_char_h()`, so the
glyphs' own opaque background painted flush against (and visually
erased) the border pixels on any row with a character. Budgeting width
isn't enough on its own; a fixed-box text widget needs real margin on
*both* axes, not just clipping on the one that happened to get bug
reports first. Fixed with a small `ROW_VPAD` constant reserving actual
vertical slack. See the commit that added it.

## `widgets.h`/`theme.h` stay minimal on purpose

Both only gained their current primitives once a *second* real caller
needed them (the commit \""Splitting wm.c into userland/wm/, and a
shared widgets.h/widgets.c module"\" for widgets.h's origin, and the
scrollbar-phase builds for how `text_scrollback` grew from
Terminal-only to shared with Notepad). Deliberately not
speculatively built out ahead of a real second caller -- see each
header's own top comment before adding to it.

## The window manager is one event loop, not decoupled components

`userland/wm/` is split into `wm.c`/`wm_input.c`/`wm_render.c` by concern
for *readability*, but shares state through `wm_internal.h`'s
`extern`s rather than hiding it behind accessor functions -- it's
still one tightly-coupled event loop, the same thing the single
pre-split `wm.c` was (the commit \""Splitting wm.c into userland/wm/..."\"),
just spread across files. Deliberate: this is one component's internal
organization, not a boundary between independently-reasoned-about
components the way `kapi.h`/`wm.h` are. See `wm_internal.h`'s top
comment.

## Esc no longer exits the GUI desktop -- it's unclaimed at the WM level now

**Superseded -- was "Esc always exits the whole GUI desktop," see below
for what changed and why.**

Through **Build 502**, `userland/wm/wm.c`'s event loop checked `key == 27`
(Esc) and unconditionally left the window manager BEFORE routing the
keypress to whichever window was focused -- no GUI app's `on_key`
callback ever saw an Esc press. That hardcoded shortcut is gone: exiting
to the shell is now a discoverable Start menu item ("Exit to shell",
`wm_system_actions[]` in `wm.c`), not a hidden key, and Esc itself is
deliberately left unclaimed at the WM level -- free for a future
per-window or modal use (e.g. canceling a confirm dialog) instead of
double-booking it as "exit everything, no matter what's open or
focused," which is exactly the conflict this entry used to warn about.
see the git history "Exit to shell" entry.

The original reasoning below is preserved because the underlying fact
(the CLI/GUI editor's exit key had to be F3, not Esc, because of this
same conflict) is still true today -- Esc STILL isn't safe to hand to a
per-window "cancel" handler unless/until something adds its own
WM-level claim on it, which nothing has yet:

`userland/wm/wm.c`'s event loop no longer touches Esc at all -- so no GUI
app's `on_key` callback receives it any differently than before, it's
just not a WM-level exit anymore either. Found the hard way while
wiring the CLI/GUI text editor's exit key to Esc (**Build 377**):
worked fine at the physical console (no WM in that path at all) but
silently could never be received by an editor session running inside
the GUI Terminal, because the old Esc-exits-WM check intercepted it
first. If a future GUI app wants a per-window "cancel this" key today,
it still needs to be something other than Esc until a WM-level Esc
handler (e.g. a confirm dialog's cancel) actually exists -- **Build
377** picked F3 for the editor's exit specifically to sidestep this.
See `wm.c`'s own comment at the old check's former location and
the commit for build 377 for the full story.

## Don't put a `text_scrollback` on the stack

`struct text_scrollback` (`apps/ui/ui_scrollback.h` today; `widgets.h`
when this was written) embeds an 8192-cell buffer
(`SCROLLBACK_CAP`) -- around 16KB, the ENTIRE size of the kernel's boot
stack (see `boot.asm`), which is what `shell_main()`'s whole call chain
already runs on (there's no separate kernel stack per "process" the way
ring-3 processes get one). `apps/notepad.c`'s `g_notepad` was already a
static instance for exactly this reason, but **Build 377**'s first pass
at the CLI text editor put a fresh one on the stack inside `editor_run()`
anyway and it silently corrupted nearby memory several calls deep into
`shell_main()` -- no crash, just the font size randomly shrinking and
later keystrokes quietly not registering. Any new caller of
`text_scrollback` (or anything else sized against `SCROLLBACK_CAP`) on a
kernel-context call path needs a static instance, not a local variable.
See `apps/editor.c`'s `g_editor_tb` for the fix and the commit for build 377 for the full story.

## Console scrollback is a character ring in vga.c, and the boot log is echoed to it

Two changes that only make sense together: the kernel now mirrors its
log to the physical console while booting, and the console keeps a
scrollback ring so what scrolled past is still readable.

Neither works alone. `klog_write()` went to the serial port and the
`dmesg` ring only, so the screen showed "toy-os booting..." and then the
shell -- scrollback would have had nothing of the boot to scroll back
to. And echoing the log without scrollback just moves text past too fast
to read. `kernel_main()` turns the echo on early and off again just
before `apps_start()`, so it covers boot and nothing else: leaving it on
would put every ATA retry and filesystem warning on top of whatever the
shell or the GUI is drawing.

Design points worth keeping:

- **The ring stores a colour per CELL, not per line.** Output here is
  routinely multi-coloured within a line (a green prompt then grey
  input, `ls`'s per-type colouring), and replaying it in one colour
  would be a visibly worse copy of what you saw. 256 lines x 256 cols x
  2 bytes = 128KB of `.bss`, sized like `g_bitmap` for the same reason:
  it must work before `heap_init()`.
- **Wrapping is recorded as a line break**, so the ring reproduces the
  *screen* rather than the logical text. The alternative reproduces text
  better but can't redraw what you actually saw after a `fontsize`
  change.
- **PageUp/PageDown are swallowed by `keyboard_getchar()`, the blocking
  reader -- deliberately not by `keyboard_try_getchar()`**, which is
  what the window manager polls. The GUI Terminal and Notepad have their
  own PageUp/PageDown scrolling of their own widgets; swallowing the
  keys at the driver level would break both.
- **New output snaps the view back to live** before writing, so the
  history and the live console can't interleave into nonsense on screen.
- **`vga_clear()` does not discard history**, which is what a terminal
  does -- and it's load-bearing here: boot itself clears the screen (via
  `vga_reflow()` when the persisted font size loads), so a scroll-back
  limit computed as "only if there's more history than fits a screen"
  concluded there was nothing to scroll to. The limit has an explicit
  one-step case for exactly that.

See the commit that added it.

## Button press/release feedback: a general `on_press`/`on_release` WM mechanism, not a Calculator-only hack

Calculator's buttons already used the shared `widget_button()`
(`apps/widgets.c`) -- the actual gap was that nothing in the window
manager ever told an app "the mouse is down and still on this button,"
so no app could draw a pressed state even if it wanted to. Two ways to
close that: give Calculator its own private mouse-tracking (poll
button state directly in `calculator_draw()` somehow), or add a real
event to the window manager's app-callback contract. Went with the
latter -- `gui_apps.h` gained `on_press(win, cx, cy)`/`on_release(win)`,
driven from `userland/wm/wm_input.c`'s `wm_update_drag_resize()` the same
way `content_dragging`/`on_drag` already work, with a matching
`content_pressed` index in `wm_internal.h`. Reasoning: this codebase
already has one precedent for "click vs. hold-and-drag needs its own
event pair distinct from `on_click`" (`on_drag_start`/`on_drag`), and
press/release is exactly that same shape -- a private per-app
workaround would have solved Calculator alone and left the next app
that wants pressed-button feedback (or a held-scrollbar-thumb, or a
press-and-repeat spinner) to reinvent it from scratch. See
the commit that added it for the mechanism's actual shape
(why it fires on the initial button-down tick, why it returns 1 only
when the "hot" button changes, how drag-off-before-release un-presses
without triggering the button).

## ui_button/ui_button_group: Brutal-OS-inspired, but not a full retained view system

Asked directly to look at Brutal OS's `libs/brutal-ui/button.c`/`.h` and
consider whether toy-os should have "own libs for GUI apps" the same
way. Worth separating two things that question conflates: the
*insulation boundary* (apps not reaching into WM internals) already
existed -- every `apps/*.c` file only ever includes `wm/wm.h` (the
public `window_*` API) and `apps/ui/ui.h` (was `widgets.h`), never `wm_internal.h` (that's
`userland/wm/*.c`'s own private `extern` state). What Brutal's button.c
actually demonstrates is different: a widget as a self-contained
*object* that owns its state (`press`/`over` flags) and reacts to
events, versus toy-os's old style of `widgets.h` being pure draw
functions (`widget_button(x, y, w, h, ...)`) with every app hand-rolling
its own state next to them (`calculator.c`'s `g_pressed_index`).

Went with a scaled-down version of that idea (`ui_button`/
`ui_button_group`), not Brutal's full model. Brutal's `UiView` is a
generic base struct every widget inherits via a cast macro
(`ui_button$(VIEW)`), composed into a tree (`ui_view_mount()`), laid out
with a string DSL (`"dock p-8"`), and dispatched a general `UiEvent`
enum including `UI_EVENT_ENTER`/`UI_EVENT_LEAVE` for hover. toy-os's WM
doesn't have (or need) most of that: there's no view tree, no
mouse-enter/leave dispatch, no generic layout engine, and building one
just to host a button object would be solving a problem this GUI
doesn't have yet. `ui_button` is a plain struct with geometry, label,
colors, and a `pressed` flag; `ui_button_group` is the part that owns
hit-testing and "which one is down" over a caller-owned array. Both
drop straight into the existing `gui_apps.h` `on_press`/`on_click`/
`on_release` contract (see the entry above this one) rather than
inventing a second event model beside it.

**Why `apps/calculator.c` only, not also `apps/notepad.c`'s Save/Load
buttons in the same change:** they're the obvious second caller, but
migrating them isn't quite a pure rename -- `notepad_click()`'s
hit-test for Save/Load currently uses the *full* `TOOLBAR_H` height
(`widget_hit(save_x0, 0, BTN_W, TOOLBAR_H, cx, cy)`), taller than the
button `notepad_draw()` actually paints (`by`/`bh`, inset by
`BTN_MARGIN`) -- a small pre-existing looseness where clicking just
above/below the visible button still works. Routing that through
`ui_button_group`, which hit-tests against the button's own drawn
geometry, would tighten that hitbox and change what currently works --
a real (if minor) behavior change bundled into what should be a
no-behavior-change refactor. Left for its own change if wanted; it's
not blocked on anything (the "second real caller" bar is already met
by `calculator.c`), see `docs/roadmap.md`.

**Two things above have since changed and are worth reading in the
past tense.** Notepad's toolbar did get migrated to `ui_button_group`
(so the tightened hitbox happened, deliberately). And "no
mouse-enter/leave dispatch" stopped being true when `gui_apps.h`'s
`on_hover` landed with the GUI guidelines: `ui_button` carries a
`hovered` flag beside `pressed` now, driven by
`ui_button_group_hover()`. See the entry below.

## The display layer: cards are drivers, and capabilities must not lie

`gfx.c` used to be a rasteriser AND the framebuffer's owner, and the
moment a second card existed it grew `#include "vmsvga.h"` plus seven
hardcoded calls to that device. `kernel/include/kernel/display.h` is
the interface that replaced it: required `probe`/`get_surface`, optional
`flush`/cursor/accel/modeset behind capability bits, with the registry
in `kernel/drivers/display/`.

Two decisions worth keeping. **GRUB's framebuffer is a driver**
(`vesafb`), registering last as the fallback that always claims -- which
removes the old default-path-vs-driver-path asymmetry AND is the second
implementation that makes the interface a design rather than a guess.
It's deliberately the opposite kind of device from `vmsvga`: passive,
scanned, no cursor, no accel, no modeset, so every optional part of the
interface is exercised by exactly one of the two.

And **`display_probe()` refuses a driver whose caps and function
pointers disagree.** That looks like paranoia and isn't: a card that
needs a flush and doesn't get one renders perfectly into memory and
shows a frozen screen. This project paid for that bug twice in one
session before the check existed.

## The entropy source stops short of a CSPRNG, on purpose

`kernel/lib/krandom.c` is RDSEED/RDRAND when the CPU has them, TSC
jitter when it doesn't, and a mixing function over whichever it got. It
is explicitly NOT a CSPRNG -- no entropy accounting, no reseed
schedule, no backtracking resistance -- and `krandom_quality()` exists
so a caller can find that out rather than assume otherwise.

The alternative considered was the Linux shape: an entropy pool fed by
interrupt/keyboard/mouse timing, seeding a ChaCha20 DRBG. Rejected for
this kernel because it would mean shipping and maintaining a stream
cipher, plus event hooks across several drivers, to dress up an input
whose actual quality is set by the machine underneath -- and under TCG
that input is timing measured against a software timestamp counter.
Entropy accounting on top of that would be a number that looks like a
guarantee and isn't, which is the failure mode this repo's testing
notes complain about most. Two honest labels beat one dishonest pool.

What was measured rather than assumed: three separate boots on the
jitter path produced three different values, so the fallback is not
deterministic under emulation. That is the claim it needed to survive;
it is not a claim about cryptographic strength. See the commit that added it.

## Damage verification: the invariant nothing enforced

The WM repaints only the declared damage region, which is correct only
if everything that changes is declared -- from eight sites across three
files, by hand. A miss is stale pixels with no crash and no failing
assertion, and every rendering bug here has been that shape.

`gui damage verify on` renders each frame twice, damage-limited then
unrestricted, and reports any differing pixel. It found four real bugs
in its first minute: a window losing focus repainting its title bar
undeclared, the taskbar clock relying on a full-repaint fallback that
other damage cancels, the alpha-blended cursor compositing over its own
previous frame, and the first frame of a session being narrowed by an
event that arrived before it.

The general lesson, which is why this is written down rather than just
built: when a subsystem's correctness rests on a convention every caller
must remember, the fix is not more care -- it's making the convention
checkable. See the git history.

## Both cursor paths record where they drew the sprite

`wm_render_frame()` (the full path) and `wm_render_cursor_move()` (the
cheap "mouse moved, nothing else changed" path) both draw the cursor, so
both set `prev_cursor_*` -- "where the sprite actually is", which is what
the next damage-limited frame uses to decide where to erase it from.

The cheap path used not to, and it looked safe: consecutive cheap moves
restore what the previous one saved, so nothing is left behind. What it
missed is a FULL frame landing while the cursor has already moved on --
that frame damages the position before the cheap move and the position
after it, never the one in between, and a cursor stays on screen. It was
filed as a harmless inconsistency in `docs/roadmap.md` for weeks while
its own symptom sat two entries above it, filed as an unexplained
"resize" damage violation. See the commit that added it --
including why a test driving this with `gui click` cannot catch it.

## A damage-verify failure renders the frame a THIRD time before believing itself

The two-render comparison above can only tell you the renders DISAGREED,
and there are exactly two ways that happens: a genuinely missed damage
declaration, or a `render_scene()` that isn't a pure function of the
frame's state -- in which case the comparison measured nothing. The
second isn't hypothetical, because the two passes don't do the same
work: the unrestricted one calls every window's `on_draw()`, while the
damage-limited one skips windows outside the damage box entirely.

So a report re-renders the same unrestricted frame once more, in the
same frame and under the same load, and states which case it is
("scene stable (real missed damage)" / "SCENE UNSTABLE -- verdict
void") alongside the diff's bounding box. This was built to settle a
recorded known issue whose two hypotheses needed opposite fixes and
which then failed to reproduce at all -- the probe stayed because the
next such report should not have to re-open the same question. See
the commit that added it, and note the harness half of it:
`damage_hunt.py` was scoring a crashed sweep as a PASS.

## GUI testing asks the kernel, rather than measuring a screenshot

The serial debug console gained a `gui` command family
(`userland/wm/wm_debug.c`) that reports window rects, Start-menu geometry,
hit-test results and the WM's own state, and can open windows and inject
clicks/drags/keys. It works while the desktop is up because
`debug_console_poll()` is already called from `wm_run()`'s idle loop.

The reason it exists: every GUI test before it derived its coordinates
from a screenshot by hand and then hardcoded them. `tools/gui_flow.py`
still carries `MENU_TOP_Y = 475` and `ITEM_H = 27` with a comment
recording that they had drifted once and been re-measured off a live
screenshot. The kernel computes those numbers; asking it removes the
whole category. (They were right, as it happens -- `gui menu` reports
475 and 27 -- but now that's checkable instead of assumed.)

Three properties worth knowing before using or extending it.

**Injected input enters below the PS/2 driver.** It goes into
`wm_run()`'s loop as a synthetic (x, y, buttons) triple, so it exercises
WM and app logic and proves nothing about the mouse driver or keyboard
layout. QMP remains the tool for "does input arrive at all", and for
anything whose answer is genuinely a picture.

**It has to be asynchronous.** The commands are dispatched from inside
`wm_run()` (that's where `debug_console_poll()` runs), so a `gui click`
that waited for its own events to drain would be blocking the very loop
that drains them. Enqueue-and-return is the only safe shape -- the same
trap that made a lazy CPU-clock calibration hang inside a syscall.

**A click is four events, not one.** Move, press, a held tick, release,
consumed one per frame. Every control in this GUI arms on press and
commits on release (`docs/gui-guidelines.md`), which only behaves
normally if press and release land on different frames.

It lives in `userland/wm/` because it reads the window table;
`kernel/core/debug_console.c` only recognises the word `gui` and routes
it, the same direction that file already reaches `apps/` for `sh`.

## The WM clips each app's on_draw() to its window -- containment, not optimisation

`wm_render_frame()` narrows the clip rect to a window's content area
around its `on_draw()` call, then restores the scene clip. That looks
like a compositor optimisation and isn't: the damage-region clip already
handles that. It's there because nothing else stopped an app from
drawing outside its own window, and one of them did -- shrinking the
Control Panel sent System Info's lower rows down the desktop, perfectly
legible, outside any frame.

Two things had to be true at once for that. `gfx_draw_string_clipped()`
bounds **width only** -- it takes a `max_w` and has no row budget, so
the name promises more than it delivers, and the horizontal edge clipped
correctly while the bottom didn't exist as a concept. And the only clip
active while apps drew was the damage box, which by construction covers
the desktop under a window.

The intersection is computed by hand in `wm_render.c` because
`gfx_set_clip_rect()` **replaces** the active rect rather than
intersecting it. Setting the content rect naively would have widened the
damage clip back out and quietly undone Phase 1+2's whole point -- worth
knowing before adding a second nested clip anywhere.

Apps should still budget their own height (`docs/gui-guidelines.md` says
so): clipped-away drawing still costs the CPU that produced it, and a
page that stops at the last row that fits looks better than one sliced
through its glyphs. The WM clip is the backstop, not the plan.

## CPU info: one syscall, because "supported" and "enabled" sit on opposite sides of a privilege boundary

`lscpu` could have needed no kernel help at all -- `CPUID` is an
unprivileged instruction, so a ring-3 program can read the vendor,
brand string, family/model/stepping, feature bits and cache topology
entirely by itself. That's the opposite of `lspci`, which needs
`SYS_PCI_COUNT`/`SYS_PCI_INFO` because PCI config space is port I/O.

What ring 3 *cannot* do is read `CR0`/`CR4`/`EFER`. So the question
"does this CPU support SSE2" and the question "did the OS turn SSE2 on"
have different answers, from different places, with different privilege
requirements. The second one is the interesting half here -- SSE2 is
supported by every x86-64 CPU ever built, and this kernel didn't enable
it until `CR4.OSFXSR` was set (see the FP entry below). A `cpuinfo`
that collapsed the two would be strictly less informative than one that
keeps them apart.

Given the `enabled` half needs a syscall regardless, `SYS_CPU_INFO`
returns the whole `struct cpu_info` rather than only the privileged
part. The alternative -- ring 3 doing its own `CPUID` and asking the
kernel only for the control-register bits -- is architecturally tidier
but means two mechanisms and, worse, a second copy of the decoding
(the extended family/model combining rules, leaf 4's `(value - 1)`
encodings). This codebase already has that mistake on display:
`userland/bin/lspci.c` carries "its own class/subclass -> name table"
because `pci_class_name()` is kernel code, and that copy can drift.
The feature *name* table is shared instead, via `api/cpu_features.h` --
a header both sides include, deliberately kept out of `kapi.h` so the
~40 files that don't print CPU flags don't each carry a 90-entry array.

Two implementation notes worth having written down. **Cache topology
needs both vendors' leaves**: leaf 4 is the modern path, but the
default `qemu64` model reports as AuthenticAMD and populates neither
leaf 4 nor AMD's `8000001DH`, so AMD's older `80000005H`/`80000006H`
are a real fallback rather than legacy completeness -- without them the
default VM shows no caches at all. And **the clock calibration has to
happen at boot, not on first use**: it spins waiting for `pit_ticks()`
to advance, and every interrupt gate here (including `int 0x80`) clears
IF, so a lazy calibration reached through the syscall waits forever for
a tick that cannot arrive. That was found as `/bin/lscpu` hanging with
no output and no fault. `tools/vm.py --cpu MODEL` exists so the
model-dependent paths can actually be tested.

## Floating point is ring-3 only, and eager -- the same call Linux and Windows made

Asked whether SSE/SSE2/FPU could be enabled, and whether to do it
kernel-wide "if Linux and Windows have it kernel-wide." They don't.
Linux compiles its own kernel with `-mno-sse -mno-sse2 -mno-mmx
-mno-80387` (the same flags this project's `CFLAGS` already carried) and
makes kernel-side SIMD an explicitly bracketed
`kernel_fpu_begin()`/`kernel_fpu_end()` region used by a handful of
subsystems (AES-NI, RAID6); Windows requires
`KeSaveExtendedProcessorState()`/`KeRestoreExtendedProcessorState()`
around any kernel-mode FP. So here: `userland/`'s ring-3 ELFs are built
without those flags and get real hardware `double`/`float`, while
`kernel/` and `apps/` keep them.

The reason it's the right split rather than merely the conservative one:
with SSE enabled, GCC emits XMM registers in ordinary code -- struct
copies and inlined `memcpy` included, not just code that mentions a
float. An interrupt can land on any instruction, so an FP-enabled kernel
needs an FXSAVE on the interrupt path itself, on every vector. Keeping
the kernel FP-free confines state movement to where the scheduler
actually swaps ring-3 processes, which already exists
(`scheduler.c`'s `switch_to()`).

Note the toy-os-specific wrinkle, because it's a real difference from
the systems being copied: `apps/` here is ring 0, compiled into the
kernel image. So "userland only" is narrower than it sounds -- Calculator
and the WM don't get float, only `userland/`'s ELFs do. If a GUI app
ever genuinely needs it, the answer is the `kernel_fpu_begin()` bracket,
not flipping the whole kernel.

Save/restore is **eager**, not the classic lazy `CR0.TS` + `#NM` scheme.
Lazy is what CVE-2018-3665 (Lazy FP State Restore) exploited to read
another task's registers, and Linux removed its lazy path entirely in
4.14; `FXRSTOR` costs on the order of 100 cycles against a 100 Hz tick,
so the trade isn't close. A KTEST asserts `CR0.TS` stays clear so this
can't be quietly undone.

Two things found by testing rather than reasoning, both worth knowing
before touching this code. **FXSAVE does not write all 512 bytes** --
the tail from offset 464 is "available for software" and left as-is,
which is why `fpu_init_state()` copies a full template instead of
FXSAVE-ing into each new area, and why a round-trip test has to zero its
buffers first. And **a freestanding `_start` wants `RSP % 16 == 8` at
entry, not 0**: the SysV process-entry convention describes what a real
crt0 sees, and a real crt0 realigns before calling `main`, but these
`_start`s are plain C functions GCC compiles as if a return address were
pushed. Handing one a 16-aligned RSP puts every aligned local off by
eight; it took a `#GP` in ring 3 to find that. See the git history.

## Button groups commit on RELEASE, and there is no ui_button_group_click()

Both apps built on `ui_button_group` used to act from `gui_apps.h`'s
`on_click`, via a `ui_button_group_click()` that hit-tested a point and
returned that button's code. `on_click` fires on button-**DOWN** (see
`userland/wm/wm_input.c`'s dispatch, and `gui_apps.h`'s own warning), so
both Calculator's keys and Notepad's `Open...`/`Save As...` committed
the instant the mouse went down: press, drag away, release, and the
digit was still entered and the file picker still opened. Confirmed by
running that exact gesture under QMP, not by reading the code -- see
the git history.

`ui_button_group_release()` returns the released button's code now
(`-1` if none). That works because `ui_button_group_press()`
re-hit-tests every tick, so a button the cursor has left is already
unpressed and releasing there returns `-1` -- the cancel falls out of
state the group was maintaining anyway, with no "armed control" field
in either app. `ui_button_group_click()` was deleted rather than kept
alongside: it has no notion of a press to cancel, so any control
acting on it is uncancellable by construction, and it had no callers
left. The narrow act-on-contact cases `on_click` is genuinely for
(placing a text cursor, focusing a field) can bring one back if one
ever actually needs it.

Worth noting how this was found, because it generalises: it surfaced
while testing an unrelated change (hover wiring) *because the cancel
path was tested at all*. `docs/gui-guidelines.md` asks for that
explicitly -- press-drag-off-release is a separate test from
press-release, and only the second one had ever been run on these two.

## Start menu click flash: a deferred close via pit_ticks(), not a blocking sleep

A Start menu click used to run the row's action and close the menu in
the same frame -- no visible confirmation the click landed, just an
instant jump to whatever opened. Adding a brief "you clicked this" flash
needed the menu to stay open and visibly highlighted for a short time
*after* the action already ran, which a single-threaded `hlt`-loop WM
(see `userland/wm/wm.c`'s top comment) can't do with an actual blocking
sleep -- that would freeze mouse/keyboard handling for every window,
not just the menu, for the duration.

Solved the same way the existing once-a-second clock redraw already
does (`wm_run()`'s `last_second`/`pit_ticks()` check): record a
`pit_ticks()` deadline (`start_menu_flash_until`) instead of blocking,
and check it every loop tick (`wm_update_start_menu_flash()`, called
unconditionally from `wm_run()`'s loop). The row's action still runs
immediately on click -- only the menu's `start_menu_open = 0` is
deferred until the deadline passes. This is the first *deliberately
timed* (not just event-triggered) UI state this codebase has beyond
that clock tick; if a future feature wants something similar (a toast
notification, a temporary status message), this is the pattern to
reuse rather than reinventing a delay mechanism -- `pit_ticks()`
deadline + a per-tick check, never a blocking sleep in the WM loop.
See the commit that added it for the full mechanism.

## Title-bar buttons: press-then-commit-on-release, reusing the content_pressed shape

Minimize/maximize/close used to act the instant `wm_handle_left_click()`
saw a mouse-down on them (`left_edge_down`, `wm.c`'s main loop) -- a
slipped click on close had no recovery, and there was no hover feedback
at all. Requested to match Windows/KDE: mouse-down only arms the button,
the action fires on mouse-up *only if the cursor is still over that same
button*, and dragging off cancels silently.

This is structurally the same problem `content_pressed` already solved
for app buttons (Calculator's `on_press`/`on_release`, see the
`ui_button`/`ui_button_group` entry above and `wm_update_drag_resize()`
in `wm_input.c`): a press starts a "which target is armed" state, every
tick while held recomputes whether the cursor is still over that target
(only redrawing when that changes), and release either commits or
cancels depending on where the cursor ended up. `title_btn_armed_win`/
`title_btn_armed_kind`/`title_btn_pressed_active` + a new
`wm_update_title_btn_press()` mirror that shape exactly, just at the WM
level instead of the app level -- there wasn't an existing WM-level
"armed target" concept to reuse, so this is a second, parallel instance
of the same pattern rather than a shared implementation. If a third
armable-target case shows up, that's the point to consider factoring the
pattern out.

Hover (cursor over a button, not held) is a separate, simpler piece --
`title_hover_win`/`title_hover_kind`, recomputed fresh every tick from
the live mouse position by `wm_update_title_hover()`, same "derive live,
don't persist a stale answer" approach as the Start menu's own hover
(see that entry above). It deliberately goes quiet while a button's
armed (`wm_update_title_hover()` no-ops then) -- the press visual takes
over, so the two never fight over what to draw.

Deliberately does *not* `bring_to_front()` a window just because its
title-bar button was pressed -- only the committed action does that
(maximize already did; minimize/close never did), so a press-then-
drag-off-then-release cancel has no visible side effect whatsoever, not
even a restack. See the commit that added it for the full
mechanism and what was verified.

## apps/ui/: a directory for retained-widget objects, once there were three

`ui_button.c`/`.h` and `ui_button_group.c`/`.h` lived directly in
`apps/` at first (there was only one pair, no directory felt warranted
yet -- same "don't split preemptively" judgment call `CLAUDE.md`
describes for files in general). Adding `ui_textbox.c`/`.h` as a third
pair made a flat `apps/` start to mix two different kinds of file
(whole *apps* like `calculator.c`/`notepad.c`, and small *widget*
building blocks they both depend on) -- the same signal that split
`userland/wm/` out earlier, applied one level up. `apps/ui/` follows that
exact precedent: its own `Makefile` wildcard/rule (`UI_C`/`UI_OBJ`,
mirroring `WM_C`/`WM_OBJ`), files included via a relative path
(`"ui/ui.h"`) from `apps/`'s own files.

The umbrella `ui.h` is a separate, smaller decision: every GUI app that
uses more than one widget had to remember one `#include` per widget
(`calculator.c` needed both `ui_button.h` and `ui_button_group.h`
already, before textbox existed at all) -- purely a convenience
aggregate, adds no declarations of its own, just `#include`s every
`ui_*.h` in the directory so a future widget is picked up by every app
that already has `#include "ui/ui.h"`, no per-app change needed. Chose
this over folding `ui_button_group` into `ui_button.h` (the other
option on the table): a bare `ui_button` used alone (no group) would
otherwise still pull in group's array/hit-testing logic it doesn't
need, and umbrella + separate files keeps that single-responsibility
split while still solving the actual pain (remembering multiple
`#include` lines).

## Calculator is the first `multi_instance` GUI app

`gui_apps.h`'s `multi_instance` flag (see the git history) is opt-in
per app, and Calculator was the one asked for by name when multi-
window support was requested ("open two or three calculators") -- it's
also the simplest existing app to convert: no filesystem state, no
scrollback buffer, just a handful of small structs (`calc_state` +
button array + button group) that fit cleanly into one `kzalloc()`'d
`struct calculator_instance` per window. Notepad and Terminal weren't
converted in the same change -- they're not asked for as multi-window
yet, and each has more state (Notepad's filename/dirty-flag/scrollback,
Terminal's shell subprocess plumbing) that would make the conversion a
bigger, separate decision about what "two Terminals" even means (two
independent shells? a shared one?) rather than a mechanical port.
Window titles for multiple windows of the same app deliberately stay
identical (no "(2)" suffix) -- an explicit choice when this was built,
not an oversight; distinguishing same-titled windows in the taskbar is
left for a future change if it turns out to matter in practice.

## `apps/widgets.c`/`.h` no longer exist -- and `ui_scrollback`/`ui_scrollbar` didn't get an owned-geometry wrapper

When every widget still in `apps/widgets.c`/`.h` (the base `widget_hit`/
`widget_button` primitives, `text_scrollback`, the scrollbar, the
checkbox) moved into `apps/ui/` by explicit request, it was a pure file
move -- same function names/signatures, no rename, no redesign -- the
same precedent `ui_button_group.c` had already set when IT moved from
`apps/` into `apps/ui/` ("same content, no behavior change"). The one
design question worth recording: `ui_button`/`ui_textbox` own their own
geometry (`x/y/w/h` fields, a `set_geometry()` call), so why didn't
`ui_scrollback`/`ui_scrollbar` get the same treatment? Because owning
geometry only pays for itself when a caller would otherwise have to
carry that state itself across frames -- and every real
`text_scrollback`/scrollbar caller (Notepad, Terminal, the editor)
already recomputes its content rect from the window's live size on
every single frame (that's what makes resize support work at all), so
there's no per-frame bookkeeping an owned-geometry wrapper would
actually remove. `ui_textbox`'s geometry, by contrast, genuinely is
mostly-static (a fixed-position field that only moves on a font-size
change), which is exactly the case an owned `set_geometry()` call
saves real work for. See the commit that added it.

## The desktop's right-click quick-launch menu doesn't distinguish icons from empty space

`userland/wm/desktop.c`'s `desktop_handle_right_click()` always opens the
same full quick-launch menu (one row per `gui_app_registry` entry)
regardless of whether the click landed on a specific icon -- a
per-icon menu (e.g. "Open" / a future "Rename"/"Properties") was
explicitly scoped out this round, not an oversight: desktop icons
don't have any per-icon identity or state beyond "which
`gui_app_registry` index am I" yet (no rename, no repositioning, no
custom icon), so a per-icon menu would have nothing more useful to
offer than the quick-launch menu already does. Revisit once icons gain
real per-icon state worth a dedicated menu for. See `docs/roadmap.md`
and the commit that added it (the desktop/context-menu
bullet).

## `context_menu.h`'s items carry a `void *ctx`, but `start_menu.h`'s don't

`struct start_action` (`start_menu.h`) is a fixed, compile-time-known
array (`wm_system_actions[]`) -- every action's callback is a distinct
named function with nothing to parameterize, so a bare `void
(*on_select)(void)` was always enough. `struct context_menu_item`
(`context_menu.h`), by contrast, is built fresh at open time from
runtime data (which window, which app) -- "Close window" needs to know
*which* window, "Open" needs to know *which* app -- so its callback
signature carries a `void *ctx` the caller stashes that data in (a
`gui_app` pointer, or a small `static int` holding a window index) and
gets back unchanged when a row is selected. Not applied retroactively
to `start_menu.h` since nothing there needs it and the two aren't a
shared abstraction to begin with (see `userland/wm/start_menu.h`'s own top
comment on why it isn't a general "menu" type). See the commit that added it.

## Click-to-position/selection lives in the shared `text_scrollback` widget, not a Notepad-only one

When asked to add click-to-position and text selection, user chose
extending the shared `apps/ui/ui_scrollback.c` widget (used by
Notepad, Terminal, and `apps/editor.c`) over building a new
Notepad-only widget. Terminal and `editor.c` never call the new
selection API, so it's inert there -- but any future caller of
`text_scrollback` gets click-to-position/selection for free, and there
was no plausible reason for the underlying pixel<->buffer-index math
(and its correctness) to exist twice. See the commit that added it for the full implementation.

## The file picker is a WM-level modal overlay (`userland/wm/file_picker.c`), not an `apps/ui/` widget

`apps/ui/` widgets are content-relative: they draw and hit-test
against coordinates local to the window that owns them, and a widget
never needs to know about other windows or the desktop. A file picker
doesn't fit that shape -- it has to draw on top of *every* window
(including ones that didn't open it), catch clicks before the window
underneath ever sees them, and stay open across the same kind of
screen-absolute modal lifecycle `confirm_dialog.c`/`context_menu.c`/
`start_menu.c` already use (shared open/close state in
`wm_internal.h`, drawn last in `wm_render_frame()`, given first
refusal on input in `wm_input.c`/`wm.c`). So it was built the same
way those three are built, as a fourth WM-level overlay, not shoehorned
into the widget library just because it looks like "a dialog with
some UI in it." Any future app-opened dialog that needs to sit above
arbitrary other windows (a color picker, an "are you sure" variant,
etc.) should follow this same WM-overlay pattern rather than trying to
make it work as an `apps/ui/` widget instantiated by the calling app.
See the commit that added it for the full feature writeup
(navigation model, Notepad's Open.../Save As... integration, the
`redraw_pending` bug found via QMP testing on `../` double-click
navigation).

## `struct window *` isn't a stable per-window identity across frames -- don't cache one

Caught live during Milestone 1 phase 3's QMP testing (wiring `wm_run()`
to poll a pending write, `userland/wm/wm.c`/`wm.h` -- see the commit that added it): `apps/notepad.c` originally cached the `struct
window *` passed to `notepad_open()` once, in a static, and reused it
later (across many frames) to register a steppable write against.
Wrong -- `bring_to_front()` (`wm.c`) reorders `windows[]` (a fixed
`struct window windows[MAX_WINDOWS]` array) by copying window
*contents* between slots (`windows[i] = windows[i + 1]`, etc.), not by
moving pointers/identity. A `struct window *` captured once can end up,
after any later reorder, pointing at a completely different window's
data -- same memory address, different window. Every existing WM
callback (`on_click`, `on_key`, ...) was already safe from this because
it always receives a freshly-resolved pointer for the CURRENT frame
(`wm_input.c`'s hit-testing loop looks the window up by its live index
every time) and never holds onto it past that one call.

Anything that needs a window handle to survive across MULTIPLE frames
(the new case here: a write polled once per frame until it completes)
can't reuse that pattern -- either capture the pointer fresh at a point
where it's provably still valid (`window_start_write()`'s caller in
notepad.c now does this: captured in `notepad_click()` when Save As...
is pressed, valid because the file picker it opens next is modal, so no
other window can be reordered while it's open -- see
`wm_handle_left_click()`'s dispatch order in `wm_input.c`), or track it
by an index the WM itself keeps accurate across reorders (which
`pending_write_win` does, fixed up inside `bring_to_front()`/
`close_window()` -- see those functions' own comments). The bug was
silent and easy to miss by code review alone: the underlying write
still completed correctly on disk every time (verified via
`tools/tfs2_writer.py ls`), only the UI-facing completion callback fired
against the wrong window, so Notepad's Save As... button just stayed
disabled forever. Caught by clicking a second window in front of
Notepad before Save, not by reading the code -- worth remembering next
time a change wants to hold a `struct window *` past the callback it
was handed in.

## Why the compositor uses one scene-wide damage region, not per-window exposure tracking

The window manager used to redraw everything -- `desktop_draw()`'s full
clear plus every window/taskbar/menu -- on any scene change at all,
including a once-a-second clock tick. Milestone 19's "real" dirty-rect
compositor replaces that with a scene-level damage-region accumulator
(`wm_damage_rect()`, `userland/wm/wm_render.c`) that's deliberately
separate from `gfx.c`'s existing pixel-level dirty-rect tracking
(`dirty_mark()`/`gfx_present()`'s blit-only-the-touched-bbox
optimization) -- that layer already existed and still does its job one
level lower, blitting only the touched region to the real framebuffer
after software rendering finishes. The new layer sits above it,
deciding what even gets *drawn* in software in the first place, via a
new `gfx_set_clip_rect()` primitive that gates `gfx_put_pixel()` (not
`gfx_get_pixel()` -- a caller reading existing pixels, e.g. to save
content before drawing over it, still wants the real framebuffer
regardless of the active clip).

The core design choice: when something in the damaged region needs
repainting, redraw *everything* within that region, back-to-front
(desktop, then windows in z-order, then taskbar/menus), rather than
computing which specific sub-rectangles got newly exposed by a move/
close/reorder. Explicitly tracking exposure would mean, for every
window-geometry change, diffing the old rect against every
window/desktop area now underneath it -- a real polygon-clipping
problem. Redraw-by-z-order-within-the-damaged-bbox sidesteps that
entirely: whatever should be visible in that region gets drawn last in
the correct order, so occlusion just falls out of the existing paint
order for free. The tradeoff is redrawing a few more pixels than the
minimal exposure set would require (anything already-correct inside
the damaged bbox gets repainted too) -- deliberately accepted as
"simple and correct" over "minimal and fragile," consistent with the
gfx.c dirty-rect layer's own bounding-box-not-rect-list tradeoff.
Verified directly via QMP: dragging Calculator off of Notepad and
confirming Notepad's revealed area redraws correctly and only the
damaged bbox is touched (`screenshots/2026-08-12/
compositor-exposure-after-drag.png`).

Damage sources are a mix of one automatic path and several explicit
ones, because not every scene change is a pure geometry diff:
`compute_window_damage()` (`wm_render.c`) diffs each window's
position/size/visibility against fields stored directly on
`struct window` (`last_x/y/w/h/last_visible`) every frame, which
catches drags/resizes/minimize/restore automatically. But z-order
swaps (`bring_to_front()`), open/close (`open_app()`/`close_window()`),
and interaction-driven redraws that don't change any window's rect at
all (desktop icon drag, `window_invalidate()`, focused-window key/wheel
delivery) all report their own damage explicitly, since there's no
before/after rect diff to detect them from.

Two real bugs surfaced only by interactive QMP testing, not by
re-reading the code:

- **Taskbar staleness on close.** Closing Calculator via its title-bar
  X left its taskbar button drawn (confirmed by screenshot -- code
  review alone missed it because the closing window's own rect *was*
  correctly damaged, just not the taskbar strip). The taskbar's button
  list/layout/tint depends on state outside any single window's rect,
  so `close_window()`, `open_app()`, `bring_to_front()`, and the
  visibility-flip branch of `compute_window_damage()` (minimize/
  restore of the frontmost window changes its own tint) all now also
  damage the taskbar strip explicitly
  (`screenshots/2026-08-12/compositor-close-taskbar-fixed.png`).
- **Desktop icon drag highlight sliver.** Dragging a desktop icon left
  a thin stale highlight-colored line on screen along the drag path.
  `desktop_draw()`'s selection-highlight rect draws 4px *above* the
  icon's own y (`y - 4`, to include the highlight border), but the
  drag's damage strip in `desktop.c` was anchored exactly at the icon's
  y with no top margin, so that top 4px escaped the damaged region and
  was never repainted over. Fixed with a
  `DESKTOP_DRAG_DAMAGE_MARGIN` applied to the damage strip's top edge,
  matching the highlight rect's own offset
  (`screenshots/2026-08-12/compositor-icon-drag-no-artifact.png`).

Deliberately out of scope in the Phase 1+2 round above (falls back to
the old full-screen repaint on any menu/taskbar-content-click/dialog
change, which is safe -- never worse than before, just not optimized):
precise damage reporting for those interactions. That's still open.

**Phase 3** (a later round): skip `draw_window_chrome()`/`on_draw()`/
`draw_resize_grip()` entirely for a window whose rect doesn't
intersect the frame's damage box, rather than calling them and letting
`gfx_set_clip_rect()` drop their writes -- `wm_render.c`'s
`window_intersects_damage()`, consulted in `wm_render_frame()`'s
per-window loop only when a damage box was actually reported that
frame. This is exactly the kind of change that turns a latent bug into
a visible one: `bring_to_front()` (`userland/wm/wm.c`) had only ever
damaged the newly-promoted window's rect, never the
previously-frontmost window's -- but that window's titlebar tint
(focused blue vs. unfocused gray) changes on every z-order swap too.
Under Phase 1+2 this was silently harmless: the previously-frontmost
window's chrome still got *called* every frame regardless, and its
tint pixels happened to fall inside whatever damage box was active in
every scenario tested at the time, so the missing damage report never
actually produced a wrong pixel. Once Phase 3 started skipping the
call itself for windows outside the damage box, that same gap became a
real bug -- a window that had just lost focus would keep showing its
old blue titlebar indefinitely, since nothing would ever call its
chrome-drawing code again until some other damage happened to cover
it. Fixed by having `bring_to_front()` damage the previously-frontmost
window's rect alongside the newly-promoted one. Verified via QMP:
opened two non-overlapping windows, swapped focus between them
repeatedly via taskbar clicks, confirmed both titlebar tints updated
correctly on every swap
(`screenshots/2026-08-12/compositor-phase3-focus-tint-fixed.png`).

## The taskbar/tray falls back to full-screen repaint on purpose, not as an oversight

`userland/wm/wm_tray.c`'s `tray_damage()` deliberately does NOT call
`wm_damage_rect()` -- it just sets `redraw_pending`, relying on
`wm_render_frame()`'s full-screen fallback, the same as menus/dialogs
(see the compositor entry above). This looks like it's leaving an easy
optimization on the table (the tray API shipped the same day as
Milestone 19's damage-region work), but an earlier version DID scope
tray/clock updates to just the taskbar strip via `wm_damage_rect()`,
and it caused two real bugs, both caught live on the user's own
machine rather than in QMP testing:

1. **Black desktop on GUI entry.** `tray_init()` (which registers the
   clock as tray item 0) runs during `wm_run()`'s setup, before the
   main loop starts. Its `wm_damage_rect()` call poisoned the very
   first frame's "no damage reported yet -- unknown, be safe, draw
   everything" full-screen fallback into a taskbar-only clip, so
   `desktop_draw()`/window chrome ran but had every pixel outside that
   strip clipped away -- the desktop was simply never drawn.
2. **Mouse cursor drift.** The once-a-second clock tick used to report
   no damage at all, which forced a full-screen fallback redraw every
   second -- an implicit, never-designed-as-such safety net that kept
   `wm_render.c`'s cursor-under-pixels snapshot (`cursor_under`, used
   by the cheap `wm_render_cursor_move()` path) resynced against the
   real screen. Scoping the tick's damage to just the taskbar strip
   silently removed that safety net, and the cursor stopped tracking
   correctly.

Both are root-caused and fixed in the same the git history `[Unreleased]`
entry (search "tray damage-scoping regression"). The fix was simply to
stop scoping tray damage at all, matching every other still-unscoped
piece of WM chrome -- not to fix the two bugs while keeping the
optimization. Scoping the tray/taskbar precisely is still a real,
open piece of the Milestone 19 compositor plan (`docs/roadmap.md`), but
it needs the pre-loop-damage and once-a-second-safety-net issues above
solved properly first, not just reverted -- a future attempt should
budget for both, not assume the first bug found is the only one.

## An overlay forces a full repaint, because "declares no damage" is not the same as "is drawn unrestricted"

The Start menu, context menu, file picker and confirm dialog draw
outside any window's rect and declare no damage of their own. The
compositor's design note called that "falls back to a full-screen
repaint" -- and for a long time it was true, but only by accident: an
overlay frame usually had nothing *else* reporting damage either, so
the frame went unrestricted for that reason rather than because anyone
arranged it.

The moment something else declares damage in the same frame, the
fallback inverts. The frame becomes damage-limited, the overlay is
clipped away, and whatever was on screen before it stays there.
`wm_render_frame()` (`userland/wm/wm_render.c`) now discards the damage box
outright while any overlay is open, which makes the documented
behaviour actually hold instead of depending on a coincidence.

Blunt on purpose: giving each overlay a real damage rect is the better
end state and needs geometry that only `start_menu` exposes today (see
`docs/roadmap.md`'s Milestone 12 entry). Correct-by-construction first,
precise later -- the same order the compositor's other phases took. See
the git history damage-sweep entry for the reproducer
and the two further bugs the change uncovered underneath it.

## A dropdown's popup is a second draw call the app makes last, not a WM overlay

`ui_dropdown`'s popup list is drawn by `ui_dropdown_draw_popup()`, which
the app calls **after every other widget** -- not by `ui_dropdown_draw()`
itself, and not through the WM's overlay machinery that the file picker,
context menu and confirm dialog use.

Drawing here is immediate-mode: z-order is call order, and there is no
retained view tree to sort. A popup drawn from `ui_dropdown_draw()`
would sit at whatever position the dropdown occupies in the app's draw
sequence, and anything drawn after it would paint over the list. Hiding
that inside one call would need a deferred-draw list this GUI doesn't
have and doesn't otherwise want.

Making it a WM overlay was the other candidate, and is what a real combo
box does -- Windows' popup escapes its window entirely. It was rejected
because those overlays are `userland/wm/` internals with WM-level modality,
and a widget in `apps/ui/` reaching into them would invert the layering
this directory is built on (see `apps/ui/ui_button.h`). The cost is
real and is stated in the header: `wm_render_frame()` clips each app's
`on_draw()` to its content rect, so the popup **cannot leave the
window**. It flips above the box when there is no room below and shrinks
to what is available otherwise; the scrollbar it inherits from
`ui_listbox` is what keeps that acceptable rather than a truncation.

Input is the mirror rule -- the popup is on top, so it gets first
refusal, and an app forwards to the dropdown *before* the widgets
underneath it. See the commit that added it for the
worked example in UI Demo.

## ui_listbox counts scroll from the top; ui_scrollbar counts from the bottom

`ui_scrollbar.h`'s `scroll_offset` is 0 at the **bottom** (pinned to the
newest line), increasing toward the oldest. That convention is right for
the terminal scrollback it grew up alongside, and three callers depend
on it. A list is the other way round: 0 is the first item.

`ui_listbox` therefore works entirely in list coordinates and converts
at the boundary, in two one-line helpers (`listbox_bar_offset()` /
`listbox_top_from_bar()`) that are the only place the two conventions
meet. Changing `ui_scrollbar` to a neutral orientation was the
alternative; it was rejected because it would have touched Terminal,
Notepad and `ui_textview` for the benefit of one new caller, and a
silently-inverted scrollbar is a bug that looks like a rendering
glitch rather than a logic error.

## GUI tests wait on the WM's queue depth, not on a sleep derived from frame rate

`tools/gui_debug.py`'s `settle()` used to sleep a fixed 250ms after
injecting synthetic input, reasoning that events drain one per WM frame
at 100Hz, so a click's four need ~40ms and a drag's eleven ~110ms.

The premise is false: the WM loop is not a metronome. A drag measured
at ~800ms with `gui damage verify on`, which renders every frame twice
and diffs the whole screen -- roughly 70ms per event, seven times the
assumed rate -- and it moves again with the font size, the window count
or the display driver. The fixed sleep therefore raced. Windows moved
between a test's `gui windows` and the command using those coordinates,
so a drag grabbed the wrong thing, and `tools/damage_sweep.py`'s
ancestor reported a *different* bug on each run of the same script.

`gui state` reports `pending` (undelivered injected events,
`wm_debug_input_pending()`) and `settle()` polls it to zero. Anything
derived from frame rate is a guess; the queue depth is a fact. The
commands themselves stay asynchronous and non-blocking -- they are
dispatched from inside the very loop that drains them, so waiting has
to happen on the host side (see `userland/wm/wm_debug.h`).

## Modifier keys ride alongside the key, they don't re-encode it

The input ring carries `(mods << 16) | key`. The KEY half is unchanged
and still terminal-encoded -- Ctrl-A is 0x01, Alt-B is ESC then 'b', as
the "Ctrl and Alt" section of `api/keyboard.h` has always described --
so `keyboard_getchar()` returns exactly what it always did and every CLI
consumer is untouched. The mods half is *additional*, read through
`keyboard_getchar_mods()` / `keyboard_try_getchar_mods()`.

**The motivating case is Shift-Tab.** Shift only swaps the layout's
character table, and Tab has no shifted variant, so Shift-Tab and Tab
are both 0x09 and a focus ring cannot cycle backwards. There is no way
to express it in the terminal encoding at all.

The alternative considered was another discrete `KEY_*` code, as the
`KEY_SHIFT_ARROW_*` and `KEY_CTRL_ARROW_*` families got. That was right
once and doesn't scale: each new GUI combination needs another constant
and another line in `keyboard.c`, and there are only ~32 free codes
before the Nordic block at 0xC4. A live "what is held now?" query was
rejected for the reason those families exist in the first place -- state
read after the fact can disagree with the keypress it describes. The
mods are sampled inside `ring_push()`, at scancode-processing time, the
same instant the layout table picks between 'a' and 'A'.

Consequence worth knowing: `KEY_MOD_CTRL` is reported but a GUI should
rarely match on it, because Ctrl has *already* folded the letter away.
`key == 'a' && (mods & KEY_MOD_CTRL)` is never true; match 0x01. Shift
is the useful bit precisely because it doesn't fold the key.

`gui key <c> [shift|ctrl|alt|altgr]` sends them, and
`gui_apps.h`'s `on_key` grew a `mods` parameter -- four apps implement
it, so extending the signature beat a hidden accessor valid only during
the callback.

## Keyboard focus is an app-level ring with a per-widget ops table, not a WM concept

`apps/ui/ui_focus.h` owns "which widget gets keys", Tab/Shift-Tab
cycling, and the focus ring. It exists because routing keys by trying
each widget in turn breaks as soon as two of them take the keyboard:
whichever is tried first swallows everything it recognises. UI Demo hit
this the day `ui_dropdown` landed -- a dropdown handles arrows even
while CLOSED, so the listbox below it could never be arrowed at all.

**Per-widget ops table, not a switch.** A widget joins by exporting one
`const struct ui_focus_ops` (key/hit/draw_ring/accepts_focus/
set_focused). The alternative -- a `ui_widget_kind` enum and a switch
inside `ui_focus.c` -- would put every widget's name in that file, and a
widget that forgot its case would fail silently at runtime rather than
at the call site. Exporting a table is the same "adding one is adding a
row" property `gui_app_registry[]` and the Control Panel's applet table
already have.

**App-level, not WM-level.** The WM already decides which WINDOW has the
keyboard; a second global focus would have to be kept in agreement with
it. Tab order is array order -- the caller writing the array already
controls it, and every alternative (positional sorting, explicit
indices) is more machinery for a list nobody has found too long to
reorder by hand.

Two smaller calls inside it: a `ui_button_group` is ONE focus stop with
arrows moving between its buttons, because a row of related controls is
one stop in every real toolkit; and the ring WRAPS while `ui_listbox`
CLAMPS, which is not an inconsistency -- a tab ring is a cycle with no
ends, a list has a first and last item whose boundaries mean something.

## An empty clip rect draws nothing -- it is not gfx_clear_clip_rect()

`gfx_set_clip_rect()` with a non-positive w/h sets an EMPTY clip: every
pixel write is rejected until the clip is changed or cleared. It used to
do the opposite -- treat non-positive as "clear the clip", full screen
drawable -- while `gfx.h` described that same case as "(nothing draws)".
The one caller that can produce an empty rect
(`userland/wm/wm_render.c`'s `clip_to_window_content()`, intersecting a
window's content with the frame's damage box) was written against the
words, not the behaviour, so a frame whose damage grazed a window's
border without reaching its content handed that app's `on_draw()` an
UNCLIPPED screen. That was the damage sweep's long-standing "20 px"
violation (the resize grip buried by a clock-tick frame) and the
intermittent 76k-px resize one; the recorded known-issue's own probe
detail turned out to be wrong, a fresh reminder to measure before
fixing. The rule worth keeping: the two states are different operations
on purpose -- `gfx_clear_clip_rect()` is the only way to remove the
clip, and a computed rectangle with nothing in it must clip everything
out, for the same reason a formatter that can't fit writes nothing.
See the commit that added it for the full diagnosis.


## A scroll view is a CONTAINER widget, and scrolling re-runs the layout at a shifted origin

`uui_layout` deliberately OVERFLOWS when given less room than its
children want: each child gets its natural size and the rest are placed
past the container's bottom edge. That is the right call for a layout --
squashing a control into a size it said it could not use is worse -- but
it means a window shrunk below its content silently HIDES part of it,
with no scrollbar and nothing on screen to say so.

Control Panel showed it plainly: shrink the window and six of seven
timezones became unreachable and the status bar vanished entirely.
Nothing in Toykit scrolled a PAGE; `uui_listbox` and `uui_table` scroll
their own rows and that is all.

**Scrolling re-runs the content layout at a shifted origin.** Everything
else follows from that one decision. A child's rect is always its real
on-screen rect, so hit-testing, focus and drawing need no coordinate
translation anywhere -- which is the part that usually makes scroll
views fiddly. The cost is that a child scrolled out of view is genuinely
somewhere else rather than hidden, so input has to be CLIPPED to the
viewport or a row above the top would still take clicks. Re-laying out
per wheel notch is not the extravagance it sounds: `uui_layout` is not
retained mode, a re-run is arithmetic over a handful of items, and a
window resize already does exactly this.

**The router gained a generic `children` slot, replacing a type check.**
`uui_route.c` used to recognise containers by comparing `ops` against
`uui_layout_ops`. That worked for exactly one container and silently
swallowed every other container's child ids -- a press inside a scroll
view reported the SCROLL VIEW's id, so Control Panel's radio buttons
stopped applying: the app switches on `ID_CHOICES` and was being told
`ID_PAGE`. Any widget can now declare the items it holds, and a child
reports its own id however deeply it is nested.

Two rules ride with that slot. **A container with a `hit` clips its
children to itself** -- that is how the scroll view stops children
outside the viewport being clickable, and why `uui_layout` deliberately
declares no `hit` at all. And **when no child consumes an event, it
falls through to the container**, which is how the scroll view's own
scrollbar gets its clicks and how the wheel reaches it only after the
children have declined: child first, then ancestor, as every real
toolkit does it.

**Drawing needed two more slots, and the reason is a trap worth
stating.** The ROUTER paints the children, so a container cannot clip
what it does not paint -- `children_begin`/`children_end` wrap that
pass, and the scroll view sets its clip in one and clears it (and paints
its scrollbar) in the other. The container's own `draw` is deliberately
NOT called when it has children: `uui_layout_ops` has one that paints
its items, for callers driving a layout directly, and calling it here
painted every child twice. A container's background therefore goes in
`children_begin`.

**And `uui_layout` now lets a UUI_FILL child ABSORB A SHORTFALL, not
just leftover space.** A child flagged to stretch is elastic by
declaration, so `spare` is allowed to go negative and the same share-out
that grows it shrinks it, down to a floor. Without that the scroll view
was placed at its full natural height and pushed the status bar off the
window regardless -- the scroll view fixed what was inside it and could
do nothing about what came after it. A container with nothing
stretchable still overflows, which remains better than squashing a child
below the size it stated.

Tested by `tools/cpanel_test.py`'s last four checks. Their positive
control is worth reading before trusting them: reverting the layout
change alone left "the status bar survives" GREEN at its first
threshold, because the scrolled page's own content lands in those rows
and "is there any ink" is satisfied either way. Measured 4675 px
present against 777 px with the bug, so the check asserts 2000.

---

## The settings UI is a tree and a page, and it is called System Settings

Rebuilt 2026-08-19. Two decisions, and the second is the one that keeps
the app honest.

**The shape is KDE System Settings': a navigation tree on the left, one
page on the right.** GNOME Settings, Windows Settings and macOS Ventura
all converged on sidebar-plus-pane; the TREE half is specifically KDE's,
and it is what lets a category collapse once the registry grows past a
screenful. What it replaced was two tab buttons and a flat listbox --
honest, generated from the registry, and with exactly two levels of
structure available no matter how many settings existed.

**The categories come from the KERNEL, not from the app.** `struct
setting` gained a `category` string, so the sidebar is generated exactly
as the rows already were: a setting registered anywhere appears under a
heading with no edit to the app. The alternative -- a name-to-category
table in System Settings -- was rejected for the reason the app holds no
list of settings either: it is a second source of truth that drifts the
moment a subsystem adds a key, which is the specific failure the
registry was built to end. Grouping by NAMESPACE was the free
alternative and would have produced exactly one branch, since every
setting today lives in `system`.

A free string rather than an enum, so a ring-3 program declaring its own
config file can name a section the kernel has never heard of; the UI
groups by exact match and files anything unrecognised under `"General"`.
NULL becomes that default at the ABI boundary, not in each client --
otherwise every UI would carry its own copy of the fallback.

**And the name.** *Control Panel* is Windows', and this app shows
exactly the SETTINGS registry -- not facts, not tunables (see
`docs/settings-and-queries.md`'s vocabulary). KDE and macOS both call it
System Settings, and the qualifier earns its keep because a
per-application settings window is something toy-os may grow later,
which is why GNOME's bare *Settings* was not taken.

## A page heading is chrome, and drawing it in the page is wrong

Worth its own entry because the wrong version looked right in the code
and only failed at a particular scroll offset.

System Settings' first version painted the setting's name at the top of
the page rect in `on_draw`. That runs AFTER the toolkit paints the
declared widgets, so the heading landed on top of the first choice.
Reserving space for it only moves the problem: the page SCROLLS, so the
content slides underneath a heading that stays put.

A fixed heading is CHROME and belongs outside the scroll view, which
needs a label widget this toolkit does not have -- and adding one for a
string with no behaviour is a widget for the sake of having one. The
information (which setting, which file) went into the status bar
instead, which is already chrome and already outside. One less widget,
and nothing overlaps at any scroll offset.

The general form: ask what a piece of text IS -- content that scrolls,
or chrome that does not -- before deciding where to draw it.

## `uui_tree`: the nodes are the app's, the structure is the widget's

The app supplies one flat `const` array in display order, each node
carrying its DEPTH; the widget derives parent/child from the depth run,
exactly as an indented outline reads. No allocation, no ownership, no
teardown -- the same call `uui_menubar` made for its const menu trees,
and still the right one now that ring 3 has `malloc`: a declared tree
needs no teardown and cannot leak.

What the widget owns is what a tree DOES: which rows are collapsed,
which is selected, where the view is scrolled, and the mapping from a
screen row to a node once collapsing has hidden some. Collapsed state is
a BITMAP on the widget rather than a flag on the node, because the nodes
are the caller's `const` array and writing into it would make that
`const` a lie.

Three consequences worth stating:

- **Every public index is a NODE, and every stored handle is an ID.**
  Rows move as things collapse; ids do not. `uui_tree_select_id()`
  expands whatever was hiding the node, because selecting something and
  leaving it invisible looks exactly like the call doing nothing.
- **The expander toggles without navigating.** Clicking a triangle to
  see what is inside a section is not the same gesture as choosing that
  section, and conflating them would change the page every time somebody
  explored.
- **`natural_size` counts every node, collapsed or not.** A tree that
  shrank when collapsed would make the layout twitch under the user's
  own click -- the same rule `uui_button_group_natural_size()` broke by
  measuring from the origin, and for the same reason: natural size is
  what a widget WANTS, asked before anyone knows its state.

---

## A widget's ops table is the contract, and a missing slot fails silently

THREE widgets had short tables, all found in one afternoon and all with
every function they needed already written:

- `uui_dropdown_ops` -- no `natural_size`, no `set_geometry`.
- `uui_checkbox_ops` -- no `natural_size`, no `set_geometry`, and no
  `release`, so the router never even NAMED it to its app (it reports a
  widget only when that widget has a release op). The checkbox toggled
  on screen and the app was never told.
- And the same shape one level up: `uui_scrollview` re-laid its content
  out when its own rect or its offset moved, but not when the content's
  ITEM LIST changed -- see the entry below.

`uui_dropdown_ops` is the worked example. Both functions existed -- `uui_dropdown_natural_size()`
and `uui_dropdown_set_geometry()` are right there in the same file --
and only the table was short.

The consequence: **a dropdown declared in a `uui_layout` was never
positioned or measured.** It stayed 0x0 at the origin, and the layout,
unable to size a child, placed nothing sensible after it. A whole page
below the dropdown simply did not appear.

It went unnoticed for as long as it did because no app had put one in a
layout: UI Demo positions its widgets by hand, and System Settings was
the first to declare one. That is the general shape worth remembering --
**an ops slot nobody fills is a capability nobody has tested**, and it
fails at a distance from its cause, in a container, as a missing
sibling rather than as a broken widget.

Two habits follow. When adding a widget, fill the table against
`uui_widget.h` rather than against the widget you copied -- a table
copied from a neighbour inherits its gaps. And **when a layout
misbehaves, check the ops tables of everything in it before suspecting
the layout**: the whole of this failure looked like `uui_layout`
stopping after four children, and `uui_layout_run()` turned out to have
no early exit at all. Reading it settled in two minutes what three
experiments from the outside had not.

---

## `uui_label` exists because a caption is a layout child

Every app that wanted a caption drew it in `on_draw` and worked out its
own coordinates. That is fine until the thing it captions moves, and
until the page scrolls -- System Settings hit both in one day. A page
heading painted in `on_draw` landed on top of the first control, because
the toolkit paints declared widgets first and calls `on_draw` after;
reserving space for it by hand would then have left content sliding
underneath it as the page scrolled.

So: a widget with no behaviour whose entire job is to occupy a row the
layout has accounted for. It has **no `hit`**, deliberately, so the
router never offers it a press and a click passes through to whatever is
behind -- a caption that swallowed clicks would be a control that does
nothing, which is a bug shape this toolkit keeps a rule about.

Its text is POINTED AT rather than copied, so a buffer must outlive it.
And its natural HEIGHT does not depend on its text (`rows`, default 1),
because a caption whose height varied with its content would reflow the
page every time it changed.

---

## Settings carry text in `/etc`, and behaviour in the kernel

`struct setting` gained `category` and `group` -- which page a setting
appears on -- but its DESCRIPTION, its choices' display names and its
presentation hints live in `/etc/settings.d/<namespace>.<name>`, in
`etc_config`'s existing `name=value` format.

**Why the split.** `label` is compiled in because a setting without one
cannot be presented at all. Prose is the part somebody rewords, and
eventually translates, and neither should need a kernel rebuild. It is
also the part that can be ABSENT without breaking anything, which is
what makes a file safe for it: the compiled-in label is the floor, so a
missing file costs one setting its extra text and nothing else, and the
files can be added one at a time.

**Why one file per setting** rather than sections in one file: the
parser is compiled twice (kernel and ring 3) and shared with `.desktop`
entries and `/etc/services.d`, so teaching it sections would change
`etc_config_get(file, key)` at every call site and make identity
(file, section, name) when the whole registry says (namespace, name).
A directory of small descriptors needs no parser change and is the
convention `/etc/services.d`, `/etc/config.d` and `/usr/wm/desktop`
already teach.

**The display name is not the value.** `Choice.losangeles=Los Angeles`
changes only what is shown; `losangeles` is still what is stored and
what `config set` takes. Keeping them apart is what lets the UI read
well without `/etc` filling with prettified tokens a later parser would
have to accept.

**`Widget=` is a HINT, never an instruction.** A client with no such
control must still show the setting some other way, because the setting
has to remain changeable. It lives in the text file rather than in
`struct setting` because it is presentation, and the kernel's descriptor
says what a setting IS.

**`Applies=reboot` says the thing `result` cannot.** `SETTING_OP_SET`
answers SAVED / UNSAVED / INVALID, which is about persistence.
`system.default_target` persists perfectly and yet visibly does nothing,
because init reads it at boot -- and a UI reporting plain "saved" there
tells the same kind of lie `SETTING_UNSAVED` exists to prevent.

**Grouping changes nothing about storage.** `category` and `group` are
registry metadata; each setting still writes its own `name=value` line
to its own file. Moving a setting between groups migrates no data.

---

## A scroll view lays out its content when the ITEM LIST changes, too

`place_content()` was called from two places -- the viewport's rect
moving, and the scroll offset moving -- and its own comment said so:
"called whenever anything it depends on moves: the viewport's rect, or
the offset". The content's ITEM LIST is a third dependency, and nothing
called it because no app had ever changed a scroll view's contents at
runtime.

System Settings does: picking a different page rebuilds the item list.
Without a re-layout the NEW widgets were never positioned at all --
zero rect, invisible, unclickable -- while the widgets carried over from
the previous page kept the PREVIOUS page's rects. On screen that reads
as a layout that stops after four children, which is the wrong place to
look by a whole layer.

**It is automatic, not a call an app must remember.** `sv_children()` --
the one accessor the router uses before routing input into the children
and before painting them -- compares the content's `items` pointer and
`count` against what was last laid out, and re-positions if they differ.
`uui_scrollview_content_changed()` still exists and is still the honest
thing to call at the point of change, but an app that forgets it now
gets a correct page rather than an invisible one.

That choice is this project's standing rule applied: an init step
reachable by only one entry point is a bug waiting for a second entry
point. Verified by removing the app's explicit call and confirming the
geometry was identical.
