# Decisions: GUI: window manager, compositor and widgets

The window protocol, the compositor, the toolkit, and how anything gets drawn.

Part of **[docs/decisions.md](../decisions.md)**, which indexes every
decision in this project and is GENERATED from these files -- run
`tools/gen_decisions_index.py` after adding an entry here, or
`tools/check_docs.py` will fail.

Write the reasoning HERE, in full: an entry that cannot be understood
without opening something else is not finished.

---

## Reading the bochs adapter's capabilities is a WRITE, so it happens once

`bochs_accepts()` put the adapter into `DISPI_GETCAPS`, read the maximum
resolution and video memory, then wrote `ENABLE` back -- under a comment
saying "A read of capabilities only; the display is untouched". That
comment was wrong in the way that matters: GETCAPS is entered and left
by WRITING a live ENABLE register, and `bochs_drv_mode_count()` calls
`bochs_accepts()` once per ladder mode.

So every enumeration of the mode list toggled ENABLE dozens of times
while the desktop was on screen. Reported from a real session as the
screen breaking into coloured vertical bars for about a second on
opening System Settings, on selecting its Screen page, and on changing
resolution -- which are exactly the three things that enumerate modes.
Captured at 1920x1080: one frame per trigger where QEMU's surface is
**1920x56**, magenta and yellow-green.

The limits cannot change while the machine runs, so they are read once
at probe and answered from RAM. Three opens, zero bad frames, against
one per open before.

**The fix's own first version broke something else, and how says more
than what.** It restored ENABLE by rebuilding the value from `g_active`
-- which is 0 during probe, so the call DISABLED an adapter GRUB had
left enabled, and the mode-adoption path below it then claimed a display
that was switched off. `settings_test` went from 71 passed to 68 passed
and 1 failed, on a hover check with no visible connection to the
display. It saves and restores the register now, which is correct
wherever the call happens rather than correct where it was first put.

**The general shape, and it is not specific to this adapter:** a
function whose comment says it only reads, which reaches hardware
through a mode register, is worth distrusting. The cost here was
invisible to every automated test -- the suite drives the GUI and
asserts on settled frames, and this defect lives entirely in the frames
between them.



## Windows remember their geometry per APP, saved on close, keyed by a string

Every app reopened at a cascade position and its `default_size()`, so a
window you had sized and placed came back wrong every launch. The
compositor stores per-application geometry in `/etc/windows.conf` now
and applies it at create time.

**Keyed by `app_id`, not by `app_identity`.** The identity int looks
like the natural key -- it is the kernel's, which is exactly what
CLAUDE.md says a window's application identity is -- and it is wrong
here: `win_server.c` interns it from a spawn path into a per-boot table
in launch order, so "identity 3" is a different app tomorrow. `app_id`
is a string the app declares (`"settings"`) and the registry defaults to
its Exec basename. Desktop icon positions already key on a name for the
same reason.

**Saved in `close_window()`, not where geometry changes.** Geometry is
written at five call sites -- two drags, maximize, a client's resize
ack, and the pull-back-on-screen -- and a drag rewrites x/y every frame,
so saving there would be hundreds of whole-file rewrites per drag.
`close_window()` is one chokepoint that sees the final answer while the
slot is still live.

**A maximized window saves its RESTORE rect.** Saving the maximized
geometry would reopen the app at exactly screen size but not maximized:
a window that looks maximized, has no restore size to go back to, and
covers the taskbar. The restore rect is already maintained for
un-maximize, so this costs nothing.

**A client's size is a REQUEST, not an assignment.** A client owns its
buffer, so the WM cannot widen its window; the restore asks through
`wm_client_send_resize()`, the same path a user's resize drag uses, and
the client acks through `on_window_resized()`.

**Position is clamped against the SCREEN, size against the WORK AREA,
and the difference is deliberate.** A window may be dragged off an edge
or under the taskbar on purpose, so clamping position to the work area
would silently move a window the user had placed -- 200,150 came back as
200,120 the first time. But a window LARGER than the work area has its
close button off the edge, so size is clamped harder. `wm_ensure_reachable()`
is not enough on its own: its guarantee is that a sliver stays grabbable,
which restored 4000x3000 at (5000,5000) as a 1280-wide window at x=1216.

**Default on, `RememberGeometry=false` in the `.desktop` entry opts
out** -- because the alternative is every app asking for behaviour
people expect from all of them.

**The cache that had to go.** The parsed file was cached and invalidated
only when this module's own save rewrote it, which makes the WM blind to
every other writer -- a person editing the file, and the test that
writes a known geometry before opening a window. It reads fresh per
window opened now. The general shape is worth the sentence: **a cache
invalidated only by its owner's writes is correct only while its owner
is the only writer, and nothing had said that it was.**



## A meter reserves every row it could use, because its content is a value that changes

`uui_meter` (`userland/ui/uui_meter.h`) shows a caption, a big number,
a unit, an optional detail line and an optional bar -- and its
`natural_size()` reserves ALL of them whether or not the caller has set
them.

The first version counted the strings that were non-NULL, which reads
as the obvious economy and is a height that DEPENDS ON THE CONTENT.
Disk Mark lays its four tiles out while every value is still `--` and
the unit and detail are NULL, so each tile got a two-row box; the moment
a result arrived the meter drew four rows out through the bottom of its
own border and over the widget below it. Nothing errored, and the
layout had been correct ten seconds earlier.

That is CLAUDE.md's `natural_size` rule (a widget's size must not depend
on where it currently is) arriving from a direction the rule does not
literally name: not on its POSITION, but on its own current text. The
general form is that a widget whose whole purpose is to display a
CHANGING value must not measure itself from that value. `uui_label`
already solved it with `rows`, which the caller reserves and the content
fills; this is the same answer with the number fixed by the widget,
because unlike a label there is no meter worth having with fewer rows.

## Long work does not belong in a GUI client at all -- it belongs in a child process

Disk Mark's passes take minutes. The compositor pings every client on a
cadence (`WM_PING_INTERVAL_DEFAULT`, `WM_PING_TIMEOUT_DEFAULT` to
answer), so a client that blocks through its whole job earns the busy
pointer and then `(Not Responding)` -- correct behaviour by the WM and
useless behaviour by the app.

**THE FIRST ANSWER WAS WRONG AND IS WORTH KEEPING FOR WHY.** It sliced
the work across `on_tick` and bounded each slice at 120 ms of wall
clock, which sounds exactly right. It shipped, and the window still read
`(Not Responding)` through every pass. The reason is that a slice can
only be bounded between UNITS, and the unit was a 1 MiB transfer -- which
was then 1024 syscalls, because `SYS_WRITE_MAX` was 1 KiB and libsys
loops to complete a bigger buffer. One unit ran for seconds. (The cap
has been raised twice since; the shape of the mistake is the point.) **Bounding
a loop does nothing when one turn of it is unbounded**, and the budget
check never got to run.

The fix is not a smaller unit. It is that the work does not belong in
the event loop: `/bin/diskbench` does the I/O and the GUI polls what it
writes. That is CrystalDiskMark's own shape (a worker while the window
stays live) and the pattern already here -- the File Manager spawns
`/bin/cp` rather than reimplementing copying. Three things fell out of
it that the sliced version could not have:

* The window cannot stall however slow a device is, including one
  retrying a failing sector.
* The measurement stops being distorted by the tick cadence.
* The benchmark is runnable from a shell, where it can be measured with
  no desktop competing at all -- which is a materially different number:
  16 MiB is ~7 s standalone against ~25 s with the desktop up.

**A polled report is a SNAPSHOT, not a log.** The child first APPENDED
its progress and results to a file the GUI re-read. `sys_read` carries
at most 1 KiB per call, so the results -- which come last, after
hundreds of progress lines -- were never in what the poller saw: the
window sat at "Done." with four empty tiles. Writing the whole current
state each time, at most six lines, is always complete in one read.

**And the poll is itself I/O.** At the 30 ms tick an animation wants,
re-reading that file competed with the benchmark and roughly halved the
numbers. 500 ms is what `ui/uapp.h` names for a process list, and it is
what a poller wants.

`uapp_busy_begin()` remains the right tool for work that is slow and
SHORT, where a frozen window for a moment is the honest answer. Anything
long enough that a user would want progress is long enough to need a
child.

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

## A cursor theme may carry its own colours: an IMAGE shape beside the masks

The entry above made colour the compositor's and nothing else's. That
holds for what it was written about -- a theme that is a SHAPE set, where
one set should serve a light desktop and a dark one -- and it stopped
holding the day the maintainer asked for coloured, modern pointers
(2026-10-01, chosen from mockups: Graphite, Accent, Amber, Aurora). Those
are themes people pick BECAUSE of how they look: a gradient fill, a rim
in a different colour from the fill, a soft drop shadow, a busy ring
whose arc is the accent. None of that is expressible as two coverage
masks the compositor paints white and black, and a "fill colour" key
would have bought flat colours only -- Amber and Aurora are gradients.

So a shape is now one of TWO kinds, which is the split every real system
has:

- **X11's Xcursor** has both a mask cursor (bitmap + mask, the server
  colours it) and the ARGB cursor that every modern theme uses, one
  image per nominal size plus a hotspot. Plasma, Adwaita and Bibata are
  all ARGB.
- **Windows' `.cur`/`.ani`** carry 32-bit ARGB per size; the old
  monochrome AND/XOR cursor survives beside it. Windows 11's "pointer
  colour" setting is the compositor recolouring a WHITE arrow -- the
  mask idea -- and it is offered beside the ARGB schemes, not instead.
- **Wayland** ships pixels in a `wl_buffer` (`wl_pointer.set_cursor`);
  with `cursor-shape-v1` the compositor loads an Xcursor theme, ARGB.

toy-os copies that shape. A **mask** shape (`default`, `bold`) is
unchanged and still coloured by the compositor. An **image** shape names
straight-alpha QOI files beside its descriptor -- `image=` at 1x and
`image2=`/`image3=` rendered natively for the size setting -- and its
colours are the theme's own; the compositor only composites it. What
did NOT change is the layering above: an app still NAMES a shape and
never supplies pixels, and the theme still lives with the compositor.

Three decisions inside that:

- **QOI, not a new grid format.** A 25x25 ARGB grid in hex is ~5 KiB of
  text per scale, past the descriptor's 4 KiB read; QOI is lossless,
  has alpha, is already decoded in ring 3 (`lib/uimg`, the icon cache)
  and is ENCODED by Pillow, a foreign implementation, so the decoder is
  checked by real files -- the same reason `gen_icons.py` uses it.
- **Pre-rendered sizes, not a scaled 1x.** Doubling a soft-edged
  anti-aliased image with nearest-neighbour doubles its blur; Xcursor
  and `.cur` both carry a picture per size for that reason. The loader
  decodes ONLY the size in use, so a size change reloads the theme.
- **One accessor for both kinds** (`cursor_shape_argb()`), read by the
  software sprite and by the hardware plane's sprite builder, so the two
  paths cannot disagree about what a shape looks like.

The cost, measured under TCG: a theme load decodes ten QOIs and takes
~110 ms, once per settings change. Revisit if the shape count grows a
lot, or if a theme switch is ever on a hot path.

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

The list is `/usr/wm/applications/*.desktop` now, one entry per app, scanned
at desktop startup: freedesktop's idea, and near enough its file format
that the entries are readable to anyone who has seen a Linux one
(`Name=`, `Exec=`, `Icon=`, `Categories=`). The parser is
`etc_config`'s, already in the tree for `/etc/toyos.conf` -- key=value
lines with `#` comments is exactly the format, so no second parser
exists. **Adding an app to the desktop is dropping a file in
`data/wm/applications/`.**

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
or shared-ring transport breaks that assumption -- the same argument the
since-deleted `win_server_ops` made about taking `pid` explicitly. It
also makes the
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
rectangle is the entire difference. The divergence was visible only on a
window small enough that a menu would have overflowed it, which is why
shipping the widget first and the protocol second was the phasing chosen
rather than building both at once.

**The protocol landed on 2026-09-09** -- see "A popup is a surface of
its client" below. The bounds rectangle is now the FALLBACK: the level
is drawn in-window against it only when no popup surface is granted.
What changed in the widget was not the placement (the compositor does
that now) but the drawing, which was the half this entry did not
foresee: a level's rect stays in the window's coordinates and the draw
call subtracts it.

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

**The applet doesn't cache its setting.** The timezone applet read the
current city at draw time and wrote it straight back, persisting to
`/etc/toyos.conf`. A local copy would be a second source of truth, and
a shell command could change the same setting behind the window's back.
(Both of those accessors are gone: the timezone is a registered setting
now, and the database left ring 0 entirely -- `docs/decisions.md`.)

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
The fix was a static instance in `apps/editor.c` (deleted 2026-08-22
with the kernel-side editor); `/bin/edit` and `/bin/tosh` carry the same
comment on their own statics, for the same reason and against a ring-3
stack that also has one guard page.

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
happen at boot, not on first use**: it spins waiting for `coarse_ticks()`
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

## Start menu click flash: a deferred close via coarse_ticks(), not a blocking sleep

A Start menu click used to run the row's action and close the menu in
the same frame -- no visible confirmation the click landed, just an
instant jump to whatever opened. Adding a brief "you clicked this" flash
needed the menu to stay open and visibly highlighted for a short time
*after* the action already ran, which a single-threaded `hlt`-loop WM
(see `userland/wm/wm.c`'s top comment) can't do with an actual blocking
sleep -- that would freeze mouse/keyboard handling for every window,
not just the menu, for the duration.

Solved the same way the existing once-a-second clock redraw already
does (`wm_run()`'s `last_second`/`coarse_ticks()` check): record a
`coarse_ticks()` deadline (`start_menu_flash_until`) instead of blocking,
and check it every loop tick (`wm_update_start_menu_flash()`, called
unconditionally from `wm_run()`'s loop). The row's action still runs
immediately on click -- only the menu's `start_menu_open = 0` is
deferred until the deadline passes. This is the first *deliberately
timed* (not just event-triggered) UI state this codebase has beyond
that clock tick; if a future feature wants something similar (a toast
notification, a temporary status message), this is the pattern to
reuse rather than reinventing a delay mechanism -- `coarse_ticks()`
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

## The file picker was a WM-level modal overlay, not an `apps/ui/` widget -- SUPERSEDED

**This is history.** The WM's own `file_picker.c` was deleted on
2026-09-14: it had been unreachable since the apps moved to ring 3 (a
ring-3 client cannot call into the compositor's C functions), and the
chooser is `ui/uui_filedialog.h` in a window of its own now -- see "A
file chooser is a window of its own" below. What the entry argued was
right for the world it was written in, where a GUI app ran in ring 0
beside the window manager. Kept because the reasoning is the same
reasoning the confirm dialog and the context menu still run on.


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

**Why one file per setting** rather than sections in one file: it would
make identity (file, section, name) when the whole registry says
(namespace, name), and a directory of small descriptors is what lets a
setting's text be added one at a time and a malformed one cost exactly
that setting. It is also the convention `/etc/services.d`,
`/etc/config.d` and `/usr/wm/applications` already teach.

This entry also argued that teaching the parser sections would change
`etc_config_get(file, key)` at every call site. That turned out to be
wrong when sections were built: the section is an ADDED argument
(`etc_config_get_in`), so not one existing call site moved. The rest of
the reasoning stands and these files still carry no section — see
`docs/decisions/storage.md`.

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

---

## `uui_slider` has DISCRETE stops, and that is not a limitation to fix

Written for pointer acceleration, where the values are ordered levels --
off, low, medium, high -- and what a user means is "more" or "less". A
radio list says the same thing while saying nothing about the ORDER; a
dropdown hides every value but one behind a click. That ordering is the
whole argument for the control.

**Continuous was never an option, and building it would have been the
mistake.** The settings registry has exactly one value type that carries
a choice list -- an enum -- so every setting this can serve has a finite,
ordered, named set. A continuous slider needs a numeric setting type
with a range and a unit, which does not exist; building the widget for
it first would be a control with nothing to control, which is the
framework-with-no-users shape this project keeps refusing.

**The value is an INDEX into the same `options` array** the radio list
and the dropdown take, so an app reads it identically and a setting can
change its `Widget=` in `/etc` with no code change anywhere. Ticks are
drawn per stop, deliberately: a slider that looked continuous and then
snapped would read as a bug rather than as a design.

Two input details worth keeping. A drag tracks **x only** -- leaving the
track vertically must not cancel it, which is how every real slider
behaves -- and `press` returns non-zero on any hit even when the value
does not move, because the router takes its pointer grab only when press
does, and without the grab a drag stops the moment the cursor leaves the
thumb.

---

## A control below the fold is UNREACHABLE, not merely hard to reach

A scroll view with a `hit` clips its children from routing, so a press
never reaches a child outside the viewport. That is correct and
deliberate -- it is what stops off-screen rows being clickable -- and it
has a consequence for anything driving the UI: a control that has not
been scrolled into view is not a control that is awkward to click, it is
one that cannot be clicked at all, and a tool aiming at its unscrolled
coordinates gets silence rather than an error.

System Settings therefore reports each control's rect whenever it
MOVES, which covers a page change and a scroll with one rule. The
earlier version reported only on a page change and said nothing when the
page scrolled, so a tool driving a control below the fold had no idea
where it had gone -- and the control looked dead.

---

## The taskbar's layout is ONE function, and past a floor it groups by application

`win_btn_w()` used to return a constant, and three separate places
walked the window list with it -- `draw_taskbar()`, and both hit-tests in
`wm_input.c`. That is how buttons came to run off the screen edge and
under the clock with thirteen windows open: nothing could shrink a
button without the other two disagreeing about where it now was. A
fourth walk lived in the debug console and had ALREADY drifted, placing
button 0 at `sw + 4` where the real one sat at `4 + sw + 8` -- so every
button centre a test aimed at was eight pixels left of the button.

`taskbar_layout()` (`userland/wm/wm_taskbar.c`) is the only answer now
and all four callers read it. Recomputed per call rather than cached: it
depends on the window list, the live font metrics and the tray's width,
and a cache would be a fourth thing that can disagree with the other
three.

**The policy is Windows', not an invention.** Buttons take their natural
width while they fit, shrink toward a font-derived floor (three
characters plus padding -- below that a label stops telling you
anything, which is roughly where Windows and KDE both stop shrinking),
and past the floor windows of the same application collapse into one
button carrying a count, which opens a list of its windows on click.
Windows has shrunk-then-grouped since XP; KDE Plasma shrinks then wraps
to extra rows or scrolls; GNOME has no taskbar and Wayland has no
protocol for one, so this is panel-side either way.

**Where toy-os deliberately differs: it runs out.** Past the point where
even collapsed floor-width buttons will not fit, the extras are DROPPED
and counted through `taskbar_hidden()` rather than drawn off-screen.
Windows and KDE never run out because they scroll or wrap. A second row
was the tempting fix and was not taken, because `taskbar_h` is a
constant that the desktop icon area, the Start menu's anchor, the
context-menu clamp and every `wm_damage_rect(0, screen_h - taskbar_h,
...)` all derive from -- a variable height is a change to all of those,
for a case reached at about thirty windows. Reporting the shortfall
honestly costs nothing and is what a test can assert on; drawing
off-screen was the bug.

**Grouping needed an identity, and one already existed but was empty.**
`struct window.app_id` is the client's own name for what its window IS,
and on the ring-3 desktop it was `""` for every window: `wm_client.c`
passed a literal empty string to `on_window_created()`, because
`WIN_REQ_WINDOW_INFO` carries exactly one `text` field and that was
already the title. Both are `WIN_TITLE_LEN`, so they do not both fit,
and widening `struct win_request_msg` would cost every `WIN_REQ_PRESENT`
on the hot path. `WIN_REQ_WINDOW_APPID` is a second request instead,
asked once at create -- an app id never changes, unlike a title. Every
`uapp` declares one now, not just the single-instance ones, which is
what makes two Notepad processes group and Notepad plus Calculator not.
A window with no app id falls back to its client pid, so it can never be
merged with an unrelated window that also said nothing.

The naming rule that follows: a GROUPED button is named after the
application and an ungrouped one after its window. That is what Windows
and KDE show, and it is the only naming that stays true when the group's
frontmost window changes underneath it. A grouped button is also allowed
to be wider than a plain one, because there are by definition few of
them and its label carries a count -- at the plain natural width
"notepad (32)" truncates to "not (32)", which names nothing.

---

## A window's application identity is the KERNEL's, not the app's

The taskbar groups by application and `UAPP_SINGLE_INSTANCE` asks "is a
copy of me already running?". Both were keyed on `app_id` -- a string
each app declares about itself -- and that cannot be made safe.

**Two apps declaring the same string fail silently, in two directions.**
`WIN_REQ_ACTIVATE` returned the first window carrying the id, so if Task
Manager and System Settings both said `"system"`, launching Task Manager
while Settings was open would raise SETTINGS, answer "yes, your twin is
up", and Task Manager would exit 0 without ever drawing -- an app that
simply does not start. An app need not even be single-instance to be the
victim: it never asks, but its window is in the search set. And a taskbar
grouping by the string would merge two unrelated programs into one
button.

**No runtime check can catch it.** Two copies of ONE program legitimately
share an identity -- that is the case grouping exists for -- and they are
indistinguishable from a collision at the point of the check. A
build-time uniqueness check over the tree was considered and rejected: it
would keep a hand-maintained registry of names that must stay unique,
which is the "a fact someone has to remember to keep true" shape this
project has deleted everywhere else, and it does nothing for a binary
built outside the tree.

**So identity is derived, not declared:** the full path the owning
process was spawned from, taken from the scheduler at window-create time
(`scheduler_exec_path()`, `struct sched_process::exec_path`). Two
processes of one program share it; two programs never can; and no app can
influence it. `app_id` survives as a display name with no correctness
role.

This is what every real system does, and the giveaway is always the
fallback. Windows has AppUserModelID but groups by the EXECUTABLE when
none is set; macOS ties the bundle id to a bundle on disk; Wayland's
`xdg_toplevel.set_app_id` is a free string that is only dependable
because a compositor matches it against a `.desktop` FILE -- GNOME's
"my app doesn't group" bugs are almost all app_id/desktop-file
mismatches. X11 goes further and makes `WM_CLASS` a *(instance, class)*
pair. None of them trusts a self-declared name as the primary key.

Two consequences worth knowing. **The compositor receives a NUMBER, not
the path** -- `struct win_request_msg`'s only string field is
`WIN_TITLE_LEN` (32) while a path is `FS_PATH_MAX` (64), so shipping the
path would truncate it and two long paths sharing a prefix would collide
silently, reintroducing the exact bug. The kernel interns paths
(`app_identity_for()`) and hands out small indices. And
**`WIN_REQ_ACTIVATE` now takes no input at all**: a question whose answer
the asker cannot influence has no misdeclaration failure mode, and
`UAPP_SINGLE_INSTANCE` no longer needs an `app_id` to work.

---

## A shrinking window's compositor mapping keeps a POISONED TAIL

`comp_poison()` states the invariant: while a compositor is registered, a
window buffer's slot in its address space is never a HOLE, because the
compositor is a process and cannot be stopped mid-frame. That was written
for a window being DESTROYED, and it quietly did not cover one being made
SMALLER.

A shrink allocates new frames, remaps the slot to FEWER pages and frees
the old ones. But the compositor still holds the window's old width and
height -- it does not learn the new ones until it drains
`WIN_EV_CLIENT_RESIZED`, which is some frames later. Its very next blit
runs off the end of the new mapping into nothing, and the desktop dies.
Measured as a page fault in `ugfx_blit()` at exactly the pixel where the
old size passed the new one.

`comp_span` is the fix: the slot's extent is a HIGH-WATER MARK of every
size the window has ever been, and it never shrinks while the window
lives. Real frames occupy the first `pages` of it and the poison page
covers the rest, so a stale-size blit reads black for at most one frame,
which the compositor corrects the moment it adopts the new size. It costs
only page-table entries -- every poison page in the system is the same
borrowed frame.

The general lesson is the one the original entry already half-stated: a
ring-0 component becoming a process turns every "and then it will notice"
into a race, and an invariant written for one lifecycle event has to be
re-read against every other one that changes the same state.

---

## The damage verifier cannot judge client content, and says so

`gui damage verify on` renders each frame twice -- once clipped to the
declared damage, once unrestricted -- and reports a difference as a
missed damage declaration. It renders a third time to check the scene is
stable, which separates a real miss from a `render_scene()` that is not a
pure function of the frame.

That probe has a hole, and it is structural rather than an oversight: a
client window's content is **another process's memory**. TWP has no
`wl_buffer.release`-style handshake, so `WIN_REQ_PRESENT` is a
notification and not a promise to hold still, and a client may rewrite
its buffer between any two of those renders. The third render does not
catch it, because both renders it compares are unrestricted. The result
was 22 confident "real missed damage" reports on a desktop with no damage
bug in it -- every one of them on Terminal or Notepad, which blink a text
caret, and none on Calculator, which does not.

So `ugfx_verify_diff_masked()` excludes client CONTENT rectangles from
the comparison. What stays fully verified is everything the compositor
draws itself -- chrome, the desktop, the taskbar, menus, the cursor --
which is also where a missed damage declaration can actually originate,
since the WM is what declares damage.

**Masking is PER PIXEL, and that is the whole design.** The first attempt
voided any report whose diff landed in client content, and the positive
control caught it immediately: a deliberately removed taskbar damage
declaration produced one diff spanning a client window AND the taskbar
strip underneath it, so voiding the report threw away the half that was
genuinely verifiable and the control passed against a broken kernel. With
per-pixel masking the same control reddens three checks, every one at
y=700 -- the taskbar strip, exactly where it was broken.

Verifying client content properly needs a buffer-release protocol, which
is a real feature and not a fix for this. Until then the honest thing is
a narrower guarantee that holds, rather than a broad one that does not.

**And the narrowing immediately earned itself.** With the noise gone, a
random-walk sweep found a real missed declaration on the first run:
`on_window_title()` damaged the title bar and nothing else, while the
TASKBAR BUTTON carries the same title -- so every ring-3 window's button
showed the placeholder `Client` until something unrelated repainted the
strip. That is the common path rather than an edge, since a client sets
its real title immediately after opening, and it had been sitting under
22 confident false reports.

## Word wrapping is opt-in, and the row count is the APP's decision

`uui_label` gained `uui_label_set_wrap(l, rows)`: off by default, which
is `QLabel::setWordWrap` and `GtkLabel:wrap`. Most labels are a word or
two inside a control and wrapping one would look broken; the ones that
need it are PROSE -- a page description, a setting's explanation --
written by whoever registered the setting, with no length they are
promised to fit. Those were being clipped mid-word, and the only way to
read one was to widen the window.

**The height comes from `rows`, not from the text.** A real toolkit asks
a widget "how tall are you at this width?" -- Qt's `heightForWidth` --
which needs a second measure pass through the layout. `uui_layout` has
one pass and a rule (CLAUDE.md) that a widget's `natural_size` must not
depend on where it currently is. So the caller reserves rows and
wrapping fills them; text that still does not fit ellipsises on the last
line, so a shortfall is visible rather than being a sentence that
appears to end early.

**Reserving a row for everyone is not the answer, and this is the part
worth remembering.** The first version simply gave every description two
rows. It works, it is one line, and it made nine descriptions in ten a
row taller than they needed -- which was enough to push the last control
of the Mouse page below the scroll fold, where a control is UNREACHABLE
rather than merely awkward. Four settings-test checks caught it, all of
them about clicking a control that was no longer on screen. Wrapping is
meant to save the reader a resize, not to spend the space it saved.

So the row count is computed from the text and the label's width. That
does not break the `natural_size` rule: the rule forbids a widget
MEASURING ITSELF from its own placement during layout, and this is the
app deciding what to ask for before layout runs -- the same thing it
already did by writing `rows` by hand, computed instead of guessed. The
loop is safe in the direction it runs: width decides rows, rows changes
only height, and height does not feed back into width.

**The cadence took three attempts and each failure is instructive.**
Every frame: `relayout_page()` runs under the user and resets the scroll
position, so a long page cannot be scrolled at all. Once per APP: fits
only whichever page happened to open first, because every later page's
labels are laid out for the first time when that page opens and so
report a width of 0 -- the description that needed two rows silently
ellipsised. Once per PAGE is correct, and it is one extra layout per
navigation.

`settings_test.py` asserts BOTH halves -- that a description wider than
its label takes two rows, and that one which fits does not spend a
second -- from the widths the app itself reports, so there is no
threshold to re-check when the font changes. The second half is what
keeps the first honest: reserving two rows for everything passes the
first check and fails the second.

## `on_draw` runs BEFORE the widgets, and a wrong comment cost a whole page

`uapp.c`'s order is "clear, then the APP's own painting, then the
widgets, then overlays". The app paints UNDER its widgets deliberately:
an app whose `on_draw` begins by clearing the surface -- the natural
first line, and what every client wrote before the toolkit cleared for
them -- can then only wipe its own backdrop instead of everything the
toolkit just drew. That bug shipped twice before the order was settled.

System Settings' System Information page drew its text from `on_draw`.
The scroll view filled its rect immediately afterwards, and the page came
up EMPTY -- while the page title, the sidebar selection and the status
bar were all correct, because those are widgets. It read as a data
problem: the app was clearly navigating, clearly logging that it had
drawn, and the only thing missing was the content.

`settings.c`'s own comment asserted the opposite order ("on_draw runs
AFTER the widgets"), which is why nobody looked there. The fix is
`on_draw_over`, the hook that exists for exactly this and is documented
as "deliberately separate and deliberately last".

**Two things generalise.** A page that draws over widgets must also
avoid the widgets it is drawing over -- the first fix printed the
version string through the words "System Information", because both
started at the top of the page area; the overlay now starts below the
title, asked of the layout rather than computed a second time. And the
page had NO TEST: `settings_test` drove the Mouse page and the timezone
page and never opened this one, so the suite was built entirely from
"do X, check Y changed" and could not see a page nobody drove. It now
asserts on PIXELS -- ink in the page body, compared against the same
region on a settings page -- because "it responds" is not "it is drawn".

---

## Text is MEASURED now, not multiplied -- one advance chokepoint per ring

Every string width in this OS used to be `length * cell_width`, which
was true because every glyph was one fixed cell wide. With a
proportional face loadable from `/usr/share/fonts` it stopped being
true, and the interesting part is how small the change turned out to be:
`gfx_char_advance(c)` in the kernel and `ugfx_char_advance(c)` in ring
3, with `gfx_text_width()`, `gfx_text_fit_chars()`,
`gfx_draw_string()`, `gfx_draw_string_clipped()` and their `ugfx` peers
all rebuilt on them.

**Nothing else had to change, and that is the payoff of a rule this repo
already had.** `gfx.h` has told callers for a long time to ask
`gfx_text_width()` rather than writing `k_strlen(s) * gfx_char_w()`
themselves, and `gfx_test.c` exists specifically to pin those
chokepoints down "while every answer is still obvious, so the migration
has something that fails when it breaks them". Widgets that obeyed the
rule needed no edit at all; the migration was four functions in each
ring rather than every call site in the toolkit. This is the strongest
evidence the project has that a chokepoint added before it is needed
pays for itself.

**Drawing a glyph and advancing past it are now two different widths.**
`gfx_draw_char()` still paints the WHOLE cell, background included,
because that is the contract the console depends on -- a character
replacing a wider one must leave nothing behind. String drawing paints
only the glyph's advance instead, so a proportional face does not lay
down a cell's worth of background past the last letter and over whatever
the caller drew beside it.

**The console stays fixed-cell on purpose.** A terminal is monospace by
definition -- `fbcon` and the Windows console both are -- so a
proportional face gets a cell as wide as its WIDEST advance and the
console draws in it. Text in the console is then loosely spaced rather
than overlapping, which is the correct failure: ugly, and correct.

**Advances reach ring 3 in the font mapping, not in a second message.**
`WIN_REQ_FONT` already maps the glyph data; the advance table sits
immediately after it in the same allocation, and the request returns its
offset in `mods` (0 meaning "no advances -- every cell is `a` wide",
which is unambiguous because the glyph data always starts the mapping).
A separate request would have been a second thing a client could forget
to make, and a client that forgot would silently go back to multiplying.

## Fonts are TWO TIERS: a shared session font, and one an app rasterizes itself

A widget can now ask for a font -- a weight, a size, a face -- and the
answer comes from one of two places that deliberately do not resemble
each other.

**Tier 1, the session font.** The desktop's active face, in regular and
bold, rasterized once by `kernel/drivers/font_face.c` and mapped
read-only into every client by `WIN_REQ_FONT`. Free to a client, shared
between all of them, and it moves under them when `fontface`/`fontsize`
change (`WIN_EV_FONT`). This is the tier that makes every window's text
identical to the desktop's *by construction* rather than by each app
being careful, which is the property the request was built for.

**Tier 2, a private font.** An app opens a `.ttf` itself and rasterizes
it into its own heap (`ugfx_font_load`), at any size and any face. It
costs that app its memory and its rasterization time, nothing else can
see it, and no setting moves it.

**Why not one of them.** The obvious design is to extend tier 1 until it
covers everything: let `WIN_REQ_FONT` name a `(face, weight, px)` triple
and hand back a mapping. That fails on a property of this kernel rather
than on taste. **An atlas is never freed** -- clients hold long-lived
read-only mappings and there is no way to ask them to let go, which is
the hazard the poison-page fix (978ebf7) exists for -- so the cache is
bounded and refuses rather than evicting. A per-widget font is exactly
the workload that turns "8 sizes anyone tried" into an unbounded product
of faces, weights and sizes, in a cache that can only grow. Serving that
properly means refcounting mappings across processes, in ring 0, over
data parsed from untrusted files. The cost is real and the benefit is
sharing that almost nothing would use.

The opposite extreme -- delete tier 1, every client rasterizes
everything, which is exactly what Wayland does -- fails for two reasons
that are specific to this OS. The console and the panic path need glyphs
in ring 0 *before any process exists*, so the kernel keeps a rasterizer
whatever clients do. And "all text matches the desktop's setting" stops
being a fact and becomes something every app must honour separately.

So: the shared tier stays, bounded at two atlases (the two weights of
one face at one size), and anything it cannot express is the app's own
problem to solve with the same `ttf.c` -- which already compiled into
`libuapp.a` for exactly this, one commit earlier, with
`userland/tests/ttf_test.c` proving the link.

**The two tiers measure their CELL differently, and that is deliberate.**
`font_face.c` squeezes the session font's cell to
`ascent*0.89 + descent*0.60`, copying `tools/genttf.py`, and is right to:
that cell IS the layout grid. `gfx_char_h()` sets console rows, window
chrome and every font-derived measurement on the machine, so a looser
cell makes the whole UI taller and stops matching the baked metrics. The
price is the slight descender clipping every fixed-cell terminal font
accepts.

A private font is not a grid -- it is a run of text one app draws,
measured by nothing else -- so `ugfx_font_load()` uses the font's full
ascent and descent. Paying the squeeze there buys nothing and costs a
visibly flat 'g': at 24px in `liberation-sans` it removes 2.03px of a
5.09px descender, which is most of the tail. The formula was copied from
the kernel side when the private tier was written, which is exactly the
kind of inheritance to check rather than assume.

**One handle for both.** `struct ugfx_font` is either kind and a widget
never asks which. That is what lets a heading start out as the session's
bold weight and later become 24px Liberation without the widget
changing, and it is why the private tier needed no new widget API at
all.

## A weight is not a size, and that is why both are mapped at once

`fontsize` re-maps in place: the machine is at one size, and a size
change means every client re-asks and gets the new atlas at the same
address. Bold cannot work that way -- a widget picks a weight *per run
of text*, so regular and bold have to be readable at the same instant.

Hence `win_font_vaddr(weight)`: `WIN_FONT_VADDR` plus a 4 MiB stride,
two slots, both mapped for the life of the client. 4 MiB because that is
`font_face.c`'s entire atlas cache budget, so no single atlas can
overrun its slot; the gap below `WIN_COMPOSITOR_BASE` is 256 MiB and
address space costs nothing here.

**The weight rides in `window`,** which is the one field `WIN_REQ_FONT`
never used -- a font belongs to the session, not to a window, and the
ABI comment already said so. Widening `struct win_request_msg` for it
would have put four more bytes on the path of every request including
`WIN_REQ_PRESENT`, the hot path.

## Bold prefers a real file and falls back to smearing the regular one

A face is a family: `dejavu-sans-mono.ttf` plus an optional
`dejavu-sans-mono-bold.ttf`, paired by FILENAME and listed once. Where
the bold file exists it is loaded and rasterized like any other face --
real letterforms, real metrics.

Where it does not, the regular outlines are rasterized and thickened
(`ttf_embolden`), and every advance grows by the smear width so the next
letter does not lap onto the last column of the one before it. **This is
what GDI does** when a family has no bold face, and what Cairo and
DirectWrite fall back to. It is visibly worse than a designed bold,
especially at small sizes where the smear closes a counter.

The alternative -- "a family with no bold file has no bold" -- was
rejected because the failure is silent: bold would draw as regular, a
heading would be identical to its body text, and nothing would say why.
A worse bold that is visibly bold beats a correct-looking one that is
not there. `vera-mono` ships deliberately without a bold companion so
this path is exercised on every image rather than merely written.

Pairing by filename rather than by the font's own `name`/`OS/2` tables
is the same call the face name already makes. fontconfig and DirectWrite
read the metadata because they must cope with whatever a user has
installed; this directory is small and seeded by the build.

## Kerning is baked into the atlas as a DENSE matrix, in slot space

`ttf.c` reads the legacy format-0 horizontal `kern` subtable, and
`font_face.c` evaluates every pair in the 101-glyph set at build time
into a `count x count` array of signed pixels appended to the atlas.

**Dense, not the sorted pair list the file itself uses.** 101x101 is
10 KB against an atlas that is 14 KB at 14px and ~640 KB at 64px, and
the lookup is on a path that runs once per character drawn -- in two
rings, in measuring and in drawing. A sorted list would save a few KB
and put a binary search in the inner loop of every string measurement.
Dense also makes the layout UNCONDITIONAL, which is what lets a client
DERIVE the kern offset (`win_font_kern_offset()`) instead of being told
it, and so needs no new ABI field.

**In SLOT space, not glyph ids**, because a client has no `cmap` -- it
has an atlas and an index. That also means the matrix is meaningless
outside its own atlas, which is correct: it is scaled to that size.

**Only format 0, and that is a stopping point rather than an
oversight.** Modern faces keep kerning in GPOS, which is a shaping
engine's job -- HarfBuzz is ~50k lines and exists because doing it
properly means lookups, contextual rules and script logic. Of the faces
shipped, `liberation-sans` has a 908-pair `kern` table and
`dejavu-sans-mono` has none (it is monospace, so kerning it would be
wrong). A GPOS-only face renders unkerned, exactly as before.

**The trap, and it only exists in ring 0.** A glyph cell there is
OPAQUE -- every pixel is written, background included, which is what
lets the console overwrite a character in place. A negative kern moves
the pen left, so the new cell's leading columns sit on the previous
glyph's last ones and painting background there erases the tail of the
letter just drawn ("To" lost the right tip of the T's crossbar, which
reads as a rasterizer bug rather than a spacing one).
`draw_glyph_kerned()` blends exactly those columns over the framebuffer
instead. Ring 3 needs none of this: `ugfx_draw_char()` already skips
fully-background pixels.


## A selected font face was never BUILT unless /etc had a size key

`font_face_select()` loads and validates a `.ttf`; the atlas that makes
it drawable comes from `gfx_set_font_px()`. `font_config_init()` called
the first and then returned early when `/etc/toyos.conf` had no
`font_size` key -- so a machine with a `font_face` key, or with the
compiled-in default, had a face **active and unbuilt**. `fontface`
reported it by name while every glyph on screen came from the baked
tables.

**Why it survived a green suite for a whole feature's lifetime.** A
missing size key is the state of a freshly formatted disk and of no
developer's image: the key persists once anything writes it, and every
check in `tools/font_test.py` set a size before measuring. So the bug
was invisible everywhere except the one configuration a user actually
boots into. What it cost was the entire runtime-font feature on a fresh
image -- proportional advances, kerning and both weights all silently
fell back to the baked font, and the way it finally surfaced was "bold
looks exactly like regular".

The fix applies the size already in effect when there is no key, so a
selected face is always a drawn face. The check lives in
`font_test.py` and runs FIRST, before anything sets a face or a size,
because every later check repairs the state it is looking for. **It
cannot be a KTEST**: the font KTESTs that would run before it build an
atlas and restore it, so the invariant already holds by the time one
could assert it, and such a test passes on the broken build.

The general shape, which is the part worth keeping: **a "choose" step
and an "apply" step that can silently disagree.** Whenever a setting has
both, the invariant to assert is that a chosen thing is an applied
thing -- not that the key was read.


## A numeric setting is a TYPE, not four named levels

`mouse_speed` was an ENUM of `slow`/`normal`/`fast`/`veryfast`, and
`mouse_config.c` said exactly why: a choice list "is what lets a UI
present it at all without inventing a slider widget", and it bounded the
value, since a hand-edited 0 would freeze the pointer. Both of those are
workarounds for a missing registry feature rather than descriptions of
the setting, so `SETTING_TYPE_INT` removes them: `min`/`max`/`step` on
the descriptor, reported over the ABI as `imin`/`imax`/`istep`, and a
`unit` string.

**The range lives in code and is enforced by the REGISTRY.** A spinbox
knows the bounds and will not offer a value outside them, but `config
set system.mouse_speed 0` and a hand-edited `/etc/toyos.conf` reach
`setting_set()` without passing through any UI. `/etc/settings.d` may
already override a setting's *presentation* (which widget to use, and
its word wins) -- a range is not presentation, and letting a file widen
it would restore the exact hazard the enum existed to prevent.

**Refused, not clamped.** A caller that asked for 500 and silently got
300 has been told its request succeeded, and the next read disagrees
with what it sent. Out-of-range is a mistake worth reporting.

**The value is still a string everywhere.** `get` writes a number out,
`apply` parses one in, and the file holds text as it always did -- so
`config`, `etc_config.c` and every existing caller are untouched. Only
the bounds are new.

**Acceleration deliberately stays an ENUM.** Its values are device-count
thresholds where LOWER means MORE acceleration, so the numbers run
backwards from the effect; `high` is a better name than `3` for the same
reason `off` is better than `0`. Naming is doing real work there, not
standing in for a range -- which is the test for whether a setting wants
this type.

**Old values still load.** A disk written before the change says
`mouse_speed=slow`; the BOOT reader accepts the four old names and maps
them to percentages, while the registry does not (they are not numbers,
and a setting taking both would have two spellings of one value). The
next write stores a number, so it is a one-way migration rather than a
second format. That asymmetry is also what makes the KTEST
discriminating: `"slow"` is the only value the two layers disagree
about, so it is the only one that reddens when the registry's gate is
removed -- every numeric case still fails on `speed_apply`'s own check.

## A spinbox shows a value; a slider only shows a magnitude

A slider cannot show that the pointer is at 150%, only that it is
fastish, and it cannot accept an exact number by dragging. Every desktop
exposing a numeric setting pairs the two for that reason (KDE puts a
spinbox beside every slider); GNOME drops it and is regularly criticised
for it.

`uui_spinbox` **embeds a `uui_textbox`** rather than parsing keys
itself, because this project has one implementation of what editing
means (`uui_edit`) and a second living inside a spinbox would drift from
it the first time either gained a key. What the spinbox adds is the
steppers, the bounds, and the rule that the field holds a number.

**Typing does not change the value until it is committed.** The field
holds free text while being edited -- a half-typed "15" on the way to
"150" is out of range and must not be clamped to 25 under the user's
fingers -- and the value is adopted on Enter or when focus leaves. A
stepper has no intermediate state and applies immediately. So `value`
and the field's text can legitimately disagree for a moment, which is
the one surprising thing about the widget.

**A stepper at the end of its range draws disabled**, because a control
that looks live and does nothing is worse than one that says it cannot.

The arrows are drawn as triangles rather than spelled with characters,
since the atlas is 101 glyphs and has none. The direction is carried by
the WIDTH sequence with rows always running downward -- an up arrow
starts narrow at its apex and widens. The first version varied the y
direction instead and kept the widths, which drew both triangles upside
down *symmetrically*, so they looked like a matched pair and read as
deliberate until someone said so.

## A central theme object splits palette (colours) from metrics (sizes), Qt's QPalette + QStyle

The ring-3 toolkit's "theme" was a handful of fixed colour MACROS
(`UTHEME_TEXT` = `ugfx_rgb(20,20,20)`) plus per-app `PAD`/`GAP` constants
and per-widget hardcoded sizes. No single place owned appearance, so a
dark mode or a user accent colour would have meant editing all 81
`UTHEME_*` call sites and every spacing constant. `userland/ui/utheme.{h,c}`
makes it one live `struct utheme`.

**Why palette and metrics are split.** They change on different axes. A
palette does not move when the font size does, but a checkbox box or a
row's padding must -- so colours are struct fields (`utheme_current()`)
and metrics are FUNCTIONS of `ugfx_char_h()` (`utheme_pad/gap/indicator/
control_h`), which track `font_size` live with no rebuild. That is
exactly Qt's `QPalette` (colour roles) vs `QStyle` (pixel metrics), and
GTK's colours vs style properties. Rolling them into one struct would
have forced a rebuild-on-font-change for the colours too, for nothing.

**Why the `UTHEME_*` macros became accessors rather than a new API.**
They are redefined from `ugfx_rgb()` calls to `utheme_current()->role`,
so every existing site is theme-driven with no edit. This is only safe
because `ugfx_rgb()` is a FUNCTION -- the macros were already runtime
expressions, never usable in a file-scope static initialiser, so nothing
depended on them folding to a constant. Had any been a compile-time
constant, this would have had to be a mechanical migration of every site
instead. Checked before relying on it.

**Why the default values are apps/theme.h's, unchanged.** The whole
first step is a PURE REFACTOR with no visual change -- verified by the
full GUI suite staying green, since colours are used by every tool. Doing
it before any variant exists is the point: a dark mode is then a
`utheme_set()` swap that a green baseline can be diffed against, not a
change tangled up with the mechanism that enables it. See
`docs/roadmap.md`'s "Theme switching" for what a variant still needs
(`WIN_EV_THEME` to tell live clients, a dark value per role).

---

## An image decoder is a RING-3 LIBRARY, and the kernel never sees a JPEG

The obvious place for a JPEG decoder in this project is beside
`kernel/lib/ttf.c`: both parse a complicated file format, both feed
something drawn, and the font parser is already in ring 0 with every
read bounds-checked. Putting the image decoder there would have been
consistent, and it would have been wrong.

**What real systems do.** Linux has no image decoder in the kernel at
all -- its only in-kernel image is the boot logo, an UNCOMPRESSED PPM
converted to a C array at build time, precisely so nothing has to parse
anything. Windows keeps codecs in WIC, a USER-MODE pluggable codec
framework, and moved even font parsing out of the kernel into
`fontdrvhost` after a decade of GDI CVEs. Under Wayland the compositor
never parses an image format: a client hands it a buffer of pixels, and
KWin and Mutter decode a wallpaper in the shell process through Qt or
GdkPixbuf.

**Why toy-os follows rather than differs, when it deliberately differs
for fonts.** The font parser is in ring 0 because the CONSOLE needs
glyphs before any process exists -- a machine that cannot draw text
until a disk font has loaded cannot report why the disk font did not
load. Nothing in ring 0 needs an image. The desktop is a ring-3 process
now, so the one component that wants a decoded picture can simply call
`uimg_load()` itself. So the kernel gained no parser, no attack surface
and no syscall, and a malformed JPEG can at worst kill the one process
that opened it.

The concrete shape: `userland/lib/uimg.c` + `uimg_jpeg.c`, in
`libuapp.a`, used by `/bin/imginfo`, Image Viewer and the desktop. The
kernel is not involved anywhere.

## The codec table is a REGISTRY, with one row, on purpose

`uimg.c` dispatches on a table of `struct uimg_codec` -- probe by magic
bytes, then info/decode -- while exactly one format exists. A single
`if (looks_like_jpeg)` would have been shorter.

It is a table because this is the shape every system that ever gained a
second image format converged on (WIC's codec registry, GdkPixbuf's
loaders, Qt's image plugins) and because this project already has the
pattern and has been bitten by not applying it: `display_driver`,
`block_device`, `clocksource` and `syscall_table.c` are all registries,
and `tools/check_dispatch.py` exists because `syscall.c` grew to 37
`else if`s with nothing noticing. Adding PNG or QOI is now a file and a
row rather than a second mechanism beside the first.

**The probe decides what a file is, not its extension.** Image Viewer
lists a directory by reading the first sixteen bytes of each file, so a
JPEG named `.dat` is offered and a text file named `photo.jpg` is not.
Every real image library sniffs, for the same reason: an extension is a
hint typed by a person.

## A refusal is a RESULT: `-ENOTSUP` and `-EINVAL` are different answers

The decoder returns `-ENOTSUP` for a progressive JPEG, arithmetic
coding, 12-bit samples or CMYK, and `-EINVAL` for a file that is
malformed or truncated -- with a sentence in `uimg_last_error()` either
way, which is libjpeg's `jpeg_error_mgr` message in effect.

Collapsing the two into "it failed" would be a lie about whose fault it
is. A progressive JPEG is a perfectly good file that this build cannot
show; telling a user it is corrupt sends them to re-export a file that
was never broken. It also makes the refusals testable as behaviour:
`/tests/uimg_test` asserts the exact code, so a decoder that answered
"broken" to everything would fail, where a test asking only whether it
failed would pass it.

Progressive is the one worth naming twice. It is common on the web, and
it needs a genuinely different decoder -- coefficients arrive across
many scans and cannot be inverse-transformed until the last one -- so
half-implementing it would produce a plausible, wrong picture, which is
the failure mode this project refuses everywhere else too.

## Chroma is upsampled with libjpeg's triangle filter, and the reason is testability as much as quality

The first version replicated chroma samples when expanding a 4:2:0
image back to full resolution: each stored sample painted a 2x2 block.
That is the obvious implementation and it is visibly wrong -- a
saturated edge gains a two-pixel staircase, because the colour step
lands on a block boundary rather than on the edge.

It was replaced with the TRIANGLE FILTER libjpeg calls "fancy
upsampling" (3/4 of the nearer stored sample plus 1/4 of the next, in
each axis), matching libjpeg's arithmetic rather than merely being
equivalent to it. Two things came out of that, and the second is the
one worth recording:

- The picture is what every other viewer shows.
- **The disagreement with libjpeg dropped from 70 to 3 per channel**,
  which is what makes `tools/uimg_hostcheck.py` a real test. A
  tolerance of 70 accepts a decoder with a genuinely wrong IDCT; a
  tolerance of 3 does not. The residual 3 is not slack either -- two
  conforming IDCT implementations are ALLOWED to differ, since the JPEG
  spec fixes the transform and not the arithmetic.

The 4:2:2 case is written out separately from the 4:2:0 one even though
the general form covers it, because libjpeg's `h2v1` path rounds
differently by one LSB, and that one unit is the difference between
agreeing to 3 and agreeing to 4.

## The decoder is checked against libjpeg, in three places, and each covers what the others cannot

A decoder tested against its own output is self-consistent, and so is a
decoder with a wrong round constant -- the same trap
`docs/roadmap-details.md` records for the encryption milestone. So
every check here compares against libjpeg:

- **`tools/uimg_hostcheck.py`** compiles `uimg_jpeg.c` with the host gcc
  and runs ~180 generated images through both. This is the BREADTH: a
  JPEG decoder's bugs live in the combinations of subsampling, quality
  and dimensions, which is a sweep of hundreds of files rather than the
  handful anyone would commit.
- **`/tests/uimg_test`** runs nine committed vectors in RING 3, on this
  heap, in a real process -- the gap `/tests/klineedit_test` and
  `/tests/ttf_test` exist for. It says nothing new about the algorithm
  and everything about toy-os.
- **`tools/imgview_test.py`** compares the FRAMEBUFFER against libjpeg's
  decode of the same file. `aurora.jpg` is 1280x720 and so is the
  screen, so the default wallpaper is a pixel-for-pixel comparison with
  nothing resampled in between; its control is that the same samples
  must NOT match the other wallpaper.

## The wallpaper is a registered SETTING named like a font face, not a path in a file

The first version stored a PATH in `/etc/desktop.conf` under a key
Image Viewer wrote and the desktop read, with the key's absence meaning
"take the default" and an empty value meaning "no wallpaper". It worked,
and it was wrong in a way worth recording: it invented a private
protocol between two programs for something this system already has a
mechanism for.

It is two registered settings now -- `desktop.wallpaper` and
`desktop.wallpaper_mode` (declared in `/etc/settings.d`) -- and that
buys four things a private key could not:

- `config set desktop.wallpaper dusk` works from any shell, and
  `config list` shows it beside every other setting.
- System Settings gains a row with no edit to System Settings, because
  its pages are GENERATED from the registry.
- A UI has something to OFFER -- the choice list is a directory
  listing, so dropping a file into `/usr/share/wallpapers` adds a
  dropdown entry with no code change, exactly as a cursor theme does.
  (What the registry does NOT do is validate: `config set
  desktop.wallpaper nosuchimage` is accepted, and the desktop logs why
  nothing appeared and falls back to its plain colour. That is the same
  behaviour a bogus cursor theme has, and it is consistency rather than
  an oversight -- a persist-only setting owned by a ring-3 process has
  nobody in the kernel who could check it.)
- A test can establish the desktop's state. That one is not a nicety:
  `cursor_theme_test.py` and `font_test.py` both measure ink over a
  patch of DESKTOP, so a wallpaper makes every pixel differ from the
  background and both tools saturated the moment a default wallpaper
  shipped. They turn it off now, which is this project's standing rule
  -- establish the precondition, do not weaken the assertion.

**The value is a NAME, not a path** -- `aurora`, resolved to
`/usr/share/wallpapers/aurora.jpg`. That is the same rule a font face
(`fontface`) and a cursor theme already follow here: a user-selectable
resource is a file in a known directory, named by its filename without
the extension, so the choice list is a directory listing rather than a
list somebody maintains. The cost is real and is stated rather than
hidden: a picture elsewhere on the disk cannot be the wallpaper until it
is copied in, and Image Viewer says so instead of failing quietly.

The descriptors are PERSIST-ONLY (`.apply` is 0), exactly as the cursor
theme's are and for the same reason: `setting_register()` takes function
pointers and a ring-3 process cannot supply one, so the kernel owns the
DESCRIPTION -- name, label, legal values, which file -- and the desktop
owns the behaviour, noticing through the generation counter it already
polls. No image parsing, no path resolution and no policy entered the
kernel; what entered is a table of names.

## The wallpaper is drawn through `uui_image`, the same widget the viewer uses

The desktop could have blitted its background directly -- it owns the
screen surface, and a wallpaper is one `ugfx_blit()`. It goes through
the widget instead, which is what gave `uui_image` its second real
caller (this project's standing bar for a widget existing at all).

What that buys is not the fit maths, which is small, but the CACHE. A
scaled copy of a screen-sized picture costs tens of milliseconds to
produce and the desktop repaints on every damage event; an
implementation that resampled inside its draw call would stutter in a
way that reads as the compositor's fault. The widget recomputes the
scale only when the geometry, the fit mode or the image changes, so a
resolution change is handled without the desktop containing a word
about scaling.

## QOI is the second codec, and icons are why

The codec table's first extra row could have been PNG -- the format
anyone can produce -- and it is QOI instead. What forced the choice was
what an ICON needs, which is not what a photograph needs:

- **An alpha channel.** An icon is a rounded tile on transparency; it
  has to sit on a wallpaper. JPEG has no transparency at all, so an icon
  decoded from one arrives in an opaque rectangle.
- **Lossless edges.** At 48 pixels an icon is almost entirely edge, and
  a DCT rings around edges. The artefacts a photograph hides are exactly
  what a flat-colour pictogram shows.

Given those, QOI over PNG came down to cost and to testability. QOI is
one page of specification -- six chunk types, a 64-entry running hash,
no entropy coder -- which is ~150 lines here. PNG needs inflate before
it needs anything else, and inflate deserves a testing pass of its own
rather than arriving as a prerequisite of icons; it stays on the
roadmap, where its real argument is written down (any PNG from anywhere
just works).

The second half is that QOI is LOSSLESS, so the vectors compare
EXACTLY. JPEG's have to allow 3 per channel, which is the level two
conforming IDCTs may differ by -- a real tolerance, but one that a
subtly wrong decoder can hide inside. A tolerance of 0 cannot be hidden
inside.

**Nothing in this repo writes a QOI file.** Pillow encodes the icons and
the vectors, and Pillow's own decoder is the reference. That matters
more here than it did for JPEG: QOI is simple enough that writing an
encoder would have been easy, and then a misread chunk type would
round-trip perfectly through the matching bug -- self-consistent and
wrong, the failure this project keeps naming.

## An icon is a NAME, and the letter tile stayed as the fallback

`Icon=notepad` names an icon; `/usr/share/icons/notepad.qoi` is where
the lookup finds it. That is freedesktop's rule (a `.desktop` file names
an icon, the theme resolves it) and it is the third time this project
has made the same call: a font face is a filename in
`/usr/share/fonts`, a cursor theme is a directory in
`/usr/share/cursors`. Naming a PATH in the entry would have made every
entry carry a directory that only one component knows.

**A one-character `Icon=` is still a letter**, drawn in a tile exactly as
before, and a name whose file is missing falls back to it. That is what
let eleven entries keep working while the artwork was being drawn, and
it is why **Crash Test deliberately ships with no icon file**: the
fallback then runs on every boot rather than only in a test that
remembers to ask. `data/fonts/` plays the same trick, shipping
`vera-mono` with no bold companion so the synthesized-bold path is
exercised.

The cost, stated: an icon has to be IN that directory. A picture
elsewhere on the disk cannot be an app's icon without being copied in,
the same limit the wallpaper setting has.

## Icons are cached decoded-and-scaled, and the cache is what makes them affordable

`icon_get(name, size)` returns a borrowed pointer to an image decoded
from disk and resampled to that exact size, kept until the `.desktop`
entries reload. The desktop repaints its icon grid on every damage
event -- a mouse move, a window closing, a menu opening -- and decoding
eleven files and resampling each 64->48 on every one of those would be
milliseconds per frame spent producing the previous frame's pixels.

Two things fall out of it being a cache rather than a loader:

- **A missing file is cached as a negative result.** Crash Test has no
  icon; without remembering that, every repaint would attempt an open
  and log a failure, which is both slow and a log nobody can read.
- **The whole cache is dropped when the entries reload**, rather than
  entries being evicted individually. That is the one moment new artwork
  can have appeared, and an LRU here would be a policy with no
  measurement behind it. `gui icons` reports the count so a test can
  assert the cache IS a cache -- "the icon is drawn" says nothing about
  how often it was decoded.

## The window-to-launcher match is `AppId=`, because the obvious key is wrong

A taskbar button needs its app's icon, and a taskbar button is a
WINDOW -- which knows only the `app_id` its client declared. The
launcher knows the Exec path. Those two agree for almost every app here
and not for all of them: Shapes runs `/bin/wm/demos/shapes` and calls
itself `gfxdemo`, and Terminal runs `uterm`.

So an entry may state `AppId=`, defaulting to the Exec basename. This
is freedesktop's `StartupWMClass` and it exists for precisely this
mismatch -- GNOME and KDE need it because a window's WM_CLASS is chosen
by the application and the launcher file is named by whoever packaged
it. Deriving the pairing from the binary name alone would work until it
silently did not, which is the failure mode that key was invented for.

## The title bar's icon is the compositor's, and the client never names it

Every window's title bar carries its app's icon at the far left, and
the client is not asked for one. It is resolved exactly as the taskbar
button's already was: the window carries the `app_id` its client
declared, a `.desktop` entry carries `AppId=`, and `wm_window_icon_name()`
matches the two.

**That is Wayland's model, arrived at from the same direction.** A
client calls `xdg_toplevel.set_app_id` and the compositor finds the
artwork; `xdg-toplevel-icon-v1` came years later and only for the rare
client that genuinely needs to override it. X11 did the opposite --
`_NET_WM_ICON` is pixel data the client uploads -- which means every
toolkit ships its own scaling, an application can hand the compositor
anything, and the same app renders differently under two window
managers. toy-os has the Wayland arrangement already and there was no
reason to add a second, worse one beside it.

**Which is also why nothing new was added to the protocol.** A window
already had everything needed; what was missing was a second reader of
it. The lookup moved out of `wm_taskbar.c` to `wm.c` for that reason
alone -- "which app is this window" is a property of the window, not of
the strip that happened to be the first to draw it.

**Whether to show one at all is decided by whether one DECODES**, not
by whether a name resolves. `title_icon()` returns the picture and the
rect together, so a window whose artwork is missing (Crash Test, on
purpose) gets neither -- rather than a clickable square with nothing
visible in it, which is what a separate "where would the icon go"
helper would have produced. `gui windows --json` reports that same rect,
so a test asserts on what the compositor actually did instead of
re-deriving the geometry and drifting from it.

**Clicking it opens the window menu**, which is what Windows' system
menu, KWin's window-menu button and XFWM all do with that
corner. It opens on button-DOWN -- `docs/gui-guidelines.md` names a
menu as the documented exception to arm-then-commit-on-release -- and
it is anchored under the icon rather than at the cursor, because a menu
attached to a fixed piece of chrome should not move with the pointer.
It shares one implementation with the right-click menu
(`wm_open_window_menu()`); a second copy is precisely where Close would
have drifted back from `wm_request_close()` (ASK the client) to
`close_window()` (seize it), which this file has already shipped once.

GNOME deleted the titlebar icon and macOS never had one outside
document proxy icons, so this is a choice rather than a necessity --
but toy-os's chrome is Windows/KDE-shaped everywhere else, and an icon
in that corner is what makes the window menu discoverable at all.

## Text over a wallpaper is SHADOWED, because a guessed background stopped being one

Every `ugfx` text call takes a `bg` and alpha-blends each glyph's
partial coverage against it -- cheap, and correct as long as the caller
knows what is behind the text. The desktop's icon labels and its
version watermark both passed the flat desktop blue, which was exactly
right until wallpapers arrived. After that, every anti-aliased edge
carried a halo of a colour no longer anywhere on screen, and the
watermark's deliberately-dim ink had almost no contrast left over a
light photograph. It read as a rendering fault rather than as a quiet
watermark.

**`UGFX_TRANSPARENT` is the fix for the halo**: pass it as `bg` and the
blend reads the surface back. Only that path reads back, so every other
caller keeps the no-read-back contract unchanged. It is a sentinel
rather than a flag because 0xRRGGBB uses the low three bytes, so a
value with the top byte set cannot collide with a colour.

**Transparency alone does not fix legibility**, which is the second
half and the reason for `ugfx_draw_string_shadowed()`. A wallpaper is a
picture the user chose and can be any colour; no single ink works on
all of them. Every desktop solves this the same way -- macOS, GNOME and
KDE shadow their icon labels, Windows outlines them -- and a shadow is
the cheaper of the two here, being one extra pass rather than eight.

**The shadow's shade is DERIVED from the ink's own luminance**, not
hand-picked: light text gets a dark shadow and dark text a light one.
That is the rule `uui_state_bg()` already states, and for the same
reason -- this project has already shipped the equivalent mistake as a
hover nobody could see, because a hand-picked "lighter" tint is
invisible on a near-white theme.

**What was NOT done: sampling the backdrop and picking an ink from it.**
It sounds better and is worse. A gradient gives a different answer at
each end of a string, so the text either changes colour along its own
length or picks one point and is wrong everywhere else -- and the value
would have to be recomputed on every wallpaper change and every window
move behind it. A shadow is a fixed cost that works on any backdrop
without knowing anything about it.

## The overlays became a table, because the hover half kept being forgotten

Six popups the panel draws itself -- the Start menu, the context menu,
the calendar, the volume flyout, the file picker, the confirm dialog --
were each wired into the frame loop by hand. The click order lived in
`wm_input.c`, the draw order lived backwards in `wm_render.c`, and the
hover was wherever each one's author had put it. They are a registry
now (`userland/wm/wm_overlay.h`), the same move `display_driver`,
`block_device` and `sound_device` already made.

**The hover is what forced it, because forgetting it fails silently.**
A mouse move alone takes the compositor's cursor-only path -- no scene
repaint -- so a highlight derived from the live pointer inside a draw is
painted only when something ELSE asks for a frame. On an idle desktop
that is the clock, once a second. Of the six, two tracked a hovered
element and damaged their own rect, two more did the same from a
different call site under a different condition, one forced a
FULL-SCREEN repaint per move, and one did nothing at all -- and that
last one looked exactly like the sixth, which was simply never wired
up. Four arrangements for one behaviour is the shape this project
converts to a table on sight; what made it urgent is that the wrong
answer is invisible rather than wrong-looking.

**The table drives drawing and clicks TOO, and that is the load-bearing
part.** A hover-only registry would have left forgetting to join it
failing the same silent way -- the problem restated one level up. With
all three verbs on the same row, an overlay left out of the table never
appears on screen and cannot be clicked, which nobody ships. That is
the difference between a convention and a mechanism.

**What real systems do, and why this is not that.** In Wayland the
compositor sends `wl_pointer.enter`/`leave`/`motion` to whatever
surface is under the pointer: being a surface IS the registration, and
there is nothing to forget. KWin and Mutter get the same property from
hit-testing a scene graph. The honest comparison for what toy-os had is
X11, where a window receives `EnterNotify` only if it selected that
event in its mask -- and "forgot to select the event" is a classic X
bug, which is precisely what happened here. The panel's popups are not
surfaces (they are drawn straight into the compositor's own buffer), so
there is no scene graph for hover to fall out of; the table is the
smallest thing that gives the same guarantee. **If these popups ever
become real toolkit surfaces, this table is what they replace** -- the
toolkit already routes motion to widgets at every depth.

**Two details worth stating.** `hover_at()` returns an OPAQUE TOKEN
rather than a rect or a widget: any two controls must differ, the same
control must repeat, and 0 is none -- which lets one comparison in the
core serve a menu row, a slider, a `<` button and a dialog button with
no shared vocabulary between them. And **nothing re-hovers while the
primary button is down**, moved out of the two dialogs that had that
rule and applied to all six: a control being dragged or armed must not
hand its highlight to whatever the pointer passes over.

## The volume flyout is the panel's too, and it owns no audio state

Clicking the tray's speaker icon opens a slider, a mute toggle and the
output-device list, drawn by the window manager
(`userland/wm/volume_popup.c`) beside the calendar. The same argument
the calendar entry below makes applies -- adjusting the volume should
not cost a process spawn, a title bar and a taskbar button -- with one
honest difference worth recording, because it is the opposite of what
the real systems do.

**In Windows and KDE the audio applet is a separate process from the
shell.** Plasma's is a plasmoid, Windows' flyout is part of the shell
but the mixer behind it is not, and both can be replaced without
touching the panel. Here it is in the compositor. The reason is that
the tray API carries text and nothing else: an applet process would
need the tray to grow icons, a click callback back to the app, and a
positioning protocol for a panel-anchored window -- a `wm.h` ABI change
and a supervised process, to host one slider. That is the larger change
by a wide margin, and none of the flexibility it buys is wanted yet. If
a second applet ever appears, that is the moment to build the protocol
rather than a second special case.

**What keeps it honest is that it owns no audio state at all.** It
reads and writes two registered settings, `system.volume` and
`system.audio_device`, and its device rows ARE that setting's choice
list -- so a card plugged in after boot turns up as a row without this
file learning what a card is, and System Settings shows the same two
controls with no code shared between them. The popup is a VIEW.

**Two consequences that took a decision each.** The setting write
validates, applies and persists in one call, which is right for a
`config set` and wrong for a dragged slider -- a hundred `/etc` writes.
So the level is applied to the popup's own state immediately and the
write is debounced (~250 ms after the last movement, and immediately on
release). The alternative, an apply-without-persist path in the
registry, would be a second way for a setting to change and a new way
for the live value and the file to disagree; the debounce is entirely
inside the popup. And **mute is a level of zero rather than a second
piece of state**, for the same reason: the registry holds one number,
and a persisted mute flag beside it would be a second thing that can
disagree with the first. The cost is stated rather than hidden -- the
pre-mute level lives only in the popup, so a reboot while muted comes
back at zero.

## The calendar belongs to the panel, not to an application

Clicking the taskbar clock opens a month grid the window manager draws
itself (`userland/wm/calendar_popup.c`), a peer of the Start menu and
the context menu. The obvious alternative -- a `Calendar` app in
`userland/gui/apps/`, opened by the click -- was not taken.

**Nobody does it that way.** Windows 11's clock opens a flyout, GNOME
Shell's opens a panel, Plasma's digital-clock applet and XFCE's clock
plugin both open a popup; all four are drawn by the shell that owns the
panel. The reason is the interaction, not the architecture: looking at
the date is a glance, and a glance should not cost a process spawn, a
title bar, a taskbar button and a window to close afterwards.

**The toolkit could not have been reused anyway.** The WM hosts no
`uui` router -- it draws with `ugfx` directly, as `start_menu.c` and
`context_menu.c` do -- so a `uui_calendar` widget could not be dropped
into this popup without first building a widget host inside the
compositor. That would have been a widget with one hypothetical caller
and no way to use it, which is the opposite of this project's
second-real-caller bar. A standalone Calendar app, if one is ever
wanted for events or a year view, is where that widget earns its place.

**Six week rows, always.** Five fit most months and four fit a
non-leap February that starts in the first column, but a panel that
changed height as you paged would move its own `<` and `>` out from
under the cursor between clicks. Plasma and GNOME both reserve the full
six for the same reason.

**The days are not clickable, on purpose.** There is nothing to select a
day FOR until something stores events, and a cell that highlights and
does nothing reads as a control that is broken rather than as one that
is absent.

**A dismissing click is not uniformly swallowed.** Clicking outside an
open menu normally just dismisses it, and that is what happens over the
desktop or a window -- otherwise dismissing a popup could also raise a
window or launch an icon. The taskbar is the exception: a click there
dismisses AND falls through, so the Start button opens on the same click
that closed the calendar. Windows and Plasma both behave this way, and
the alternative reads as a dropped click. The clock itself is the third
case and is swallowed, which is what makes a second click on it a toggle
instead of a reopen.

## The Start button's appearance is three choices, not two

`desktop.start_button` takes `text`, `icon` or `both`. A boolean was the
obvious shape and is wrong: it cannot express `both`, which is what
Windows 95 through 7 actually shipped and what most people picture when
they hear "Start button". XFCE's Whisker Menu offers exactly these three
(Icon / Title / Icon and title) and KDE's Application Launcher exposes
"Icon and text" against an icon-only default, so three is the mainstream
shape rather than an invention here.

**The default is `both`, and it was `text` first for a testing reason
rather than a taste one.** The Start button's width is derived from what
is inside it, and every window button on the strip starts to the right of
that -- so the default moving shifts the entire taskbar under every
pixel-based GUI check in one commit. Shipping `text` meant the setting
arrived without that churn, and the argument was a migration one: it
expired once the setting existed and the checks had been read. `both`
followed, what Windows shipped and what the maintainer asked for then.
**Since 2026-09-29 it is `icon`** -- the mark alone, KDE's default and
Windows 11's shape -- because the redesigned strip the maintainer chose
from mockups (see "The taskbar has three styles") draws Start as a
square, and in the centred style a word would sit in the middle of a
row of icons. The churn was paid each time, deliberately, rather than
avoided forever.

**One decision, three readers.** `start_mark()` decides whether a mark
is shown at all; `start_btn_w()`, `draw_taskbar()` and
`gui taskbar --json` all ask it rather than each deciding. That is not
tidiness -- the failure it prevents is specific. The artwork can be
missing from the disk (the same case Crash Test exercises for app
icons), and the button then falls back to the word. If the width decided
independently it would still reserve an icon-sized square, giving a
narrow button with a clipped label in it: two of the three agreeing and
one not. The same reasoning as `title_icon()` returning the picture and
the rect together.

**The mark is not a pictogram.** Every other icon in `tools/gen_icons.py`
draws what an app IS. This button has no app behind it -- it opens a
menu of all of them -- so it is an abstract 2x2 grid, GNOME's "show
applications" mark. A picture of something would read as a shortcut to
one particular program in the very menu it opens. It also has no plate,
because the button already draws its own background and a second rounded
rectangle inside the first is just a border.

**And it is a NAME, not a path** (`START_ICON` -> `/usr/share/icons/start.qoi`),
the same rule app icons, font faces and cursor themes already follow, so
it goes through `icon_get()` and is cached decoded-and-scaled like
everything else on the strip.

## A right-click on a client's content belongs to the CLIENT, not to the window manager

Until Minesweeper, `wm_handle_right_click()` walked the window list and
opened the window menu for a right-click anywhere on a window --
including the middle of the app's own pixels. That was fine while every
window was drawn by the WM itself, and it quietly became a hole once
windows belonged to separate processes: **no ring-3 client could ever
receive a secondary click**, so no app could have a context menu, a
paste target, or -- the case that finally made it matter -- a way to
flag a mine.

**What real systems do.** All three give the button to the client and
claim it only on chrome. Windows sends `WM_RBUTTONDOWN` to the window
procedure and reserves the system menu for the title bar, the icon and
Alt+Space. X11 delivers button 3 to the client, and window managers that
want it take a MODIFIER chord instead (Alt+click in most of them,
KWin included). Wayland has no ambiguity at all: the compositor draws
the decorations, so anything inside the surface is the client's by
construction. toy-os was the outlier, and not deliberately -- it was the
shape a single-process WM had left behind.

**The split now.** `wm_handle_right_click()` tests the window's CONTENT
rect (`window_content_x/y/w/h()`) and, for a real client, raises the
window and sends `WIN_EV_MOUSE_DOWN` with button bit `0x2`. Everything
else -- the title bar, the 1px border, the resize margin, the taskbar
button, the desktop -- keeps the menu. The window menu is therefore no
longer reachable from the middle of an app's window, which is the
bargain every desktop above makes, and four ways in remain (the title
bar, the app icon, the taskbar button, Alt+F4).

**The bug this would have shipped without the second half.** A press is
released by the button that made it. `content_pressed` -- the WM state
that routes drag-tracking and the eventual `MOUSE_UP` to the pressed
window -- was hard-coded to end when bit `0x1` went up, because the left
button was the only one that ever set it. Arming it from a right-click
without that fix leaves a client holding a `MOUSE_DOWN` that is never
followed by a `MOUSE_UP`, which is precisely the bug `wm_input.c`
already documents shipping once for the left button ("keyboard input
worked and mouse input did not"). Hence `content_pressed_btn`.

**And the toolkit had to be told, or every app would have regressed.**
`uapp.c` routed any `MOUSE_DOWN` to the widget router, the focus ring
and the button group, because only one button had ever arrived. Deliver
a second one and a right-click starts arming Calculator's keys and
committing menu items. Qt and GTK both hand every button to the app and
act on button 1 alone; Toykit does the same now -- widgets on `0x1`,
`on_press` on everything -- so an app that has never heard of a
secondary click is unchanged, and one that wants it reads `buttons`.

**Why not make it opt-in per client.** A `WIN_HINT_*` saying "send me
right-clicks" was the conservative option and was rejected: it invents
a mechanism none of the three reference systems has, it makes the
default behaviour the wrong one, and the thing it protects -- a window
menu over app content -- is a convenience no real desktop offers.

## A key release is a new event type, and the four modifier keys are keys

`WIN_EV_KEY` was press-only. A client could know a key had been STRUCK
and never that it was HELD -- and "is W down right now?" is the entire
input model of a game, a drag modifier, or push-to-talk. Everything in
this tree edits text or clicks buttons, both of which act on the press,
so nothing had noticed.

**What everyone else does**, because press-only was the outlier here and
not a considered position: X11 sends `KeyPress` and `KeyRelease`;
Wayland's `wl_keyboard.key` carries a pressed/released state, and
`wl_keyboard.enter` additionally hands a client the set of keys already
held when focus arrives; Windows sends `WM_KEYDOWN`/`WM_KEYUP`. All
three keep text entry a separate concern -- Wayland literally splits it
into `text-input-v3` -- which is exactly what `api/keyboard.h` already
argued for when it made modifiers LATCHED at press time rather than
queryable as live state. So this is an addition beside that decision,
not a reversal of it.

**A separate event type rather than a flag on `WIN_EV_KEY`.** A client
written before this existed never asked for the new type and simply
never sees one; a flag on the press would have made every existing
client start receiving events it would decode as presses. That is the
promise `ui/uapp.h` makes about optional callbacks, kept at the layer
where it has to be kept.

### The release cannot ride the console byte stream, and that is what shaped it

The obvious implementation -- push releases where presses already go --
is impossible, and the reason is worth stating because it looks like an
arbitrary refusal. Presses go through `tty_input()` into the console
terminal: `keyboard.c` states that every code it produces fits in a
byte, which is what lets fd 0 be `read()`. A release is not a byte, and
a line discipline has no use for one -- nothing in `klineedit.c` would
ever ask whether W has come up. Pushing releases into that stream would
put bytes in front of every shell in the system to serve a consumer that
is not a shell.

So there is a second, parallel **transition queue**
(`keyboard_try_get_transition()`), and its contents are defined by one
rule: **everything the byte stream cannot represent** -- all releases,
and both edges of the four modifier keys. Ordinary presses are not
duplicated onto it. `win_input.c` turns a transition into
`WIN_EV_RAW_KEY` (a modifier press) or `WIN_EV_RAW_KEY_UP` (any
release).

### The modifier keys needed codes of their own

`KEY_SHIFT`, `KEY_CTRL`, `KEY_ALT` and `KEY_ALTGR` (0xA7-0xAA) exist
**only on the transition path** and are never pushed into the byte
stream -- pressing Shift must not put a byte in front of a shell, and
`klineedit.c` would otherwise have to learn to ignore four new codes.

They exist because a modifier produces no character, so it produced no
key event at all: it was only ever a bit riding along with some other
key. Doom is the worked example that forced the issue -- fire is Ctrl,
run is Shift, strafe is Alt, so **three of its five stock controls were
invisible to a client**. Left and right are the same key here, exactly
as `shift_pressed`/`ctrl_pressed` already treated them and for the same
reason Super does not distinguish sides; AltGr stays separate from Alt,
which is the one distinction this driver has always made and the one
that matters on a Nordic layout.

### A release carries what the PRESS produced

The driver remembers, per evdev keycode, which code that key's press
emitted, and the release carries the same one. Press W, hold it, press
Shift, release W: the release reports `'w'`, not `'W'`. A client that
watched `'w'` go down and saw `'W'` come up would clear nothing and hold
`'w'` for the rest of the session.

X11 and Wayland avoid this by delivering PHYSICAL keycodes and letting
the client translate with XKB. Doing the remembering in the driver keeps
one vocabulary on the wire, which is the property worth having here --
a toy-os client has no XKB and should not need one.

**The subtlety is autorepeat, and the rule is FIRST PRESS WINS.** A held
key repeats, and the repeats are translated afresh -- so W held while
Shift goes down starts reporting `'W'`. Recording each repeat would make
the eventual release report `'W'` and strand `'w'`. Recording only the
first press means the release always ends the hold it started. (Within a
single press the LAST push still wins, which is what makes Alt-B --
pushed as ESC then `'b'` -- release as `'b'`.) A non-zero remembered
code doubles as "this key is already down", so no separate held flag
exists to disagree with it.

**The residual limitation, stated rather than hidden**: a client keyed
on the translated code still sees the repeat `'W'` as a new key going
down, with no release ever coming for it. The real fix is to carry the
physical keycode alongside the translated one, which needs the byte
stream to carry a keycode it currently cannot. Nothing needs it yet: a
client that keys its held-set on what it received and tolerates an
unmatched release -- which it must anyway, see below -- is unaffected.

### The compositor's raw-key slot had to become a queue

`wm_rawin.c` held ONE key per frame, with a comment arguing that two
keys inside one 100Hz frame is not something the hardware can produce.
That was already optimistic under autorepeat and a slow frame, and it is
simply wrong now: a press and its release routinely land in the same
pump, as do a modifier and the key it modifies.

The cost of losing one changed too, which is the real argument. **A
dropped press is a keystroke the user repeats; a dropped release is a
key the client believes is held forever** -- in a game, a player who
will not stop walking. Both are a queue now, and the mouse's coalescing
is unchanged and still right for motion.

### An unmatched release is legal

The WM claims some presses as shortcuts -- Super toggles the Start menu,
Alt+F4 closes a window -- and those act on the PRESS, per
`docs/gui-guidelines.md`'s arm-then-commit rule; firing them again on the
way up would toggle the Start menu twice per keystroke. The release is
delivered to the focused client regardless, so a client can see a
release whose press it never saw. It must tolerate that. Every real
system produces the same shape -- an X11 grab does exactly this -- and
ignoring an up you have no down for is the correct implementation
anyway.

### Proving it needed a client that keeps a held set

`tools/keyup_test.py` drives `userland/tests/winclient.c`, which now
tracks what is currently down. The check that carries the test is the
one using `QMPSession.key_down()`: `send-key` presses and releases in
one go, so a test built on it cannot tell a working release path from a
guest that invented the release itself. Leaving the key physically down
and requiring the client to still report it held several frames later is
what nothing press-only can pass. Disabling `on_key_up` in `uapp.c`
turns 5 of 10 checks red, and the last one reports "still holds 5 keys"
-- a stuck key, seen from inside the client.

## The file manager is a commander, and it opens as an Explorer

The obvious build is an Explorer clone: a places sidebar, one content
pane, icons, copy/paste. Measured against this tree that plan is blocked
on two things toy-os does not have, and each of them is a milestone of
its own: **there is no clipboard** and **there is no drag-and-drop**
(both live in `docs/roadmap.md`'s "GUI clipboard + drag-and-drop",
unstarted). Copy/paste and drag-a-file-onto-a-window ARE Explorer's two
primary verbs, so building that shape first means either shipping a file
manager whose main actions are greyed out, or pulling an unrelated
milestone forward to serve one app.

Norton Commander (1986) answered this differently, and Midnight
Commander, Total Commander, Krusader and Far have kept the answer for
forty years: put TWO directories on screen and copying needs no transfer
mechanism at all. The source is the active pane, the destination is the
other one, F5 is copy. Nothing is carried, so nothing needs a carrier.

So the shape was chosen for what it does NOT require. It also inverts
the dependency in a useful way: instead of the file manager waiting on
the clipboard milestone, the clipboard milestone gets its most natural
first consumer (drag a file from a pane into Notepad) once it exists.

Two consequences worth stating. The keymap is the commanders'
(F5/F6/F7/F8, Tab, Enter, Backspace, Insert), which is free familiarity
for anyone who has used one and, incidentally, makes the app drivable
from a test with no pixel arithmetic at all. And **each pane carries its
own path strip**: one status line cannot say where two panes are, and
"which pane does F5 copy FROM" has to be answerable by looking rather
than by pressing something and finding out.

**The DEFAULT flipped on 2026-09-14, and the shape did not.** Both
premises above are spent: there is a system clipboard (`/bin/clipboardd`)
and there is drag-and-drop, brokered by the compositor. An argument from
what the system cannot do stops applying the moment it can, so the thing
it was holding up -- one pane, a folder tree, icons, the shape every
desktop's file manager has -- is what a first run gets now
(`g_single`/`g_tree_on`, both 1). The commander is a toolbar click away
and keeps its whole keymap.

What is NOT reopened is the commander itself. Two panes remain the
cheapest way to copy between two places, the F-keys still work, and both
flags persist per user -- so this decides a FIRST run, not a preference.
The 1986 reasoning is why the mode exists; it was never a reason to open
in it.

**The chrome became Windows 11 Explorer's on 2026-09-29**, chosen from
three mockups (Dolphin's places-and-details, Explorer's command bar,
Total Commander's F-key bar). What changed and why each piece is shaped
the way it is:

- **No menu bar.** A command bar carries the verbs -- New, Cut, Copy,
  Paste, Rename, Delete, then Sort and View drop-downs and a "See more"
  -- with a word beside the icons where the icon alone would be a
  guess. Every drop-down row names its key, which is where the F-keys
  are discoverable now that no menu lists them.
- **Places and Devices replace the tree** as the default left column.
  A device is a MOUNT read from the kernel (`QUERY_FSINFO`, what `df`
  prints) with how full it is, so a new disk appears with nothing
  registered. The tree is still there, under the places, from View.
- **A breadcrumb** (`uui_pathbar`) instead of a path field: every
  ancestor is one click away, and a click past the segments, or Ctrl+L,
  gives the text back. Split view keeps each pane's own strip, because
  one breadcrumb cannot say where two panes are.
- **Search is a filter on the folder you are in**, by name -- Dolphin's
  filter bar, not Explorer's recursive search, which would walk the disk
  on every keystroke. A new folder ends it, as in Explorer.
- **A details pane** (preview, facts, Open and Properties), on by
  default, toggled from the command bar.
- **The details view's columns are Dolphin's order** -- Name, Size,
  Modified, Type -- not Explorer's, because a narrow pane drops columns
  from the END and the name must be the last to lose room. Dropping
  from the end also keeps every column's index, which is the sort key.

The places list and the breadcrumb are widgets rather than parts of the
app because the file chooser (`uui_filedialog`) has both, and two
implementations of "where Documents is" had already begun to differ.

**Rename is in place, and Options can say otherwise** (2026-09-29). F2
and New edit the name where it stands, as Explorer and Dolphin do; a
dialog is the Options alternative for anyone who prefers it. Options is
a window of its own with a sidebar of pages (Dolphin's Configure
dialog), holding the start folder, single- or double-click, the view a
new window uses, thumbnails, hidden names, extensions, how Rename works
and whether Delete asks. **Delete asks by default and says why**: there
is no Recycle Bin, so the card names the file, what it is and that it
cannot come back -- the answer every desktop gives only for a
PERMANENT delete, which is the only kind here.

## File operations are child processes, not loops inside the window

F5 spawns `/bin/cp`, F8 spawns `/bin/rm`, and `on_tick()` reaps them
with `sys_waitpid_nohang()`. The alternative -- a copy loop inside the
GUI app -- is what GNOME Files and Explorer actually do, because they
have threads and an async I/O story. This system has neither, so an
in-process copy is a frozen window for its duration.

Four things fall out of spawning instead:

- **One implementation of what copying means.** A `/bin/cp` for the
  shell plus an in-app copy loop is the "make every fix twice" shape
  this repo has already deleted from the WM and from the `ls` wrapper
  builtin.
- **It is testable as text.** `cp -r` can be checked at a prompt with no
  compositor in the loop, and the GUI test asserts through `ls` rather
  than through the app's own view.
- **A failed or huge copy cannot take the window down with it.**
- **The queue is explicit.** Marked files are N operations, run one
  child at a time, so the status line can say "Copy 3/7" and name the
  one that failed.

The cost, stated rather than discovered later: **there is no byte-level
progress bar**, because a child reports an exit code and not a
percentage. A progress protocol is a later stage and needs something to
carry it.

## A mark names a ROW, so every reload clears the marks

`uui_fileview`'s marked set is a bitmap indexed by row. Rows are re-read
from the filesystem on every reload, so a mark that survived one would
point at whatever landed in that slot -- which is how a delete ends up
aimed at the wrong file. Clearing is therefore not a limitation but the
only correct behaviour for an index into data that was just replaced.

The consequence is a rule for callers: **snapshot the paths before
acting on marks.** The File Manager does, and it has to -- it reloads
whenever `SYS_FS_GENERATION` moves, and a copy in progress moves it, so
a queue that read the marks as it went would lose them halfway through
its own work.

The bitmap is sized in ROWS rather than entries: the listing plus the
synthetic `..`, one more than `SYS_LISTDIR_MAX`. That off-by-one is the
kind that only fires on a full directory.

## `Handles=` on a `.desktop` entry, and no MIME database

Double-clicking a file has to launch something, and the question is
where the mapping lives. freedesktop's answer is `mimeapps.list` over a
MIME database; Windows keys extensions off `HKCR`. What both have in
common, and what is worth copying, is that **the mapping is not inside
the file manager**.

So a `.desktop` entry declares `Handles=.txt .md .conf`, and the File
Manager scans the entries the desktop already reads. The app that opens
a file type is the one that says so, in the file that already declares
its name, icon and command.

The MIME database is left out deliberately. It would be a second
registry to seed, keep true and document, and this OS has one image
decoder that already identifies formats by sniffing magic bytes and one
text editor. The cost is honest and stated in the format's README: an
extension is a hint typed by a person, so a JPEG named `.dat` opens
nothing.

Two details that are decisions rather than accidents. Matching is
whole-token and case-insensitive against the extension INCLUDING its dot,
so `.md` cannot claim `.mdx` -- a substring search would. And **a
handler is spawned and never waited for**: it is a launch, not an
operation on files, and waiting for a text editor to exit would freeze
the manager for as long as somebody was editing.

## A glyph probe reads BOTH the kernel's atlas and a client's mapping, and compares them

A glyph that rasterised to nothing is pixel-identical on screen to a
space, to a character the font does not carry, and to a font that failed
to load. Nothing in this system could tell those four apart, and the
ambiguity cost a hunt that was never resolved: a client read a
session-font cell as entirely blank while the kernel had logged 101 of
101 glyphs built, and the VM was gone before anyone could ask which had
happened (`docs/bugs.md`).

**Why it inspects the RUNNING font rather than the file.** FreeType's
`ftdump`/`ftview`, `otfinfo` and `fc-match` all inspect a font file or a
fontconfig setup. That is the right tool when the rasteriser is a
library you can rerun offline — but here the rasteriser is in ring 0,
the atlas is built once at a chosen size and weight, and the interesting
bug is about what got *into* it. A file inspector would agree with the
screen only by coincidence. X11's `xfd` is the closer ancestor: it
showed a live server-side font's glyph grid. Most systems do not ship
one because most systems do not have a font living somewhere you cannot
re-derive.

**Why it reads two views.** A GUI client draws from its own read-only
mapping of the atlas (`WIN_REQ_FONT`); ring 0 draws from the atlas
itself. Those are different pieces of memory and the reported bug is
exactly the case where they disagree — so either view alone can be
perfectly healthy while the machine is not. The comparison is an FNV-1a
hash over the coverage bytes rather than two pictures side by side,
because "are these two bitmaps identical" is not a question a person
should be asked to answer by eye, and because shipping a bitmap through
a query record is not possible anyway (below).

**Why the two pictures have different bit depths**, which looks like an
inconsistency and is a split of two questions. The client view prints
8-bit coverage as a grayscale ramp: how dark the ink is belongs to
whoever draws it, and a glyph whose anti-aliasing collapsed is present,
non-blank and unreadable — a threshold would hide precisely that. The
kernel view prints a 1-bit ink map: *where* the ink is belongs to ring
0, which is where a glyph either got rasterised or did not. The
practical constraint agrees with the conceptual one — `QUERY_RECORD_MAX`
is 256 bytes and no cell's coverage bytes fit in that at any size worth
looking at, while `api/query.h`'s own advice for a class that needs more
is to be a list of smaller records, and a list of rows would need a
second selector `struct query_msg` has nowhere to put.

**`peak` rather than a has-ink flag.** A boolean answers "is this glyph
empty", and the failure that is genuinely hard to see is the one where
it is not empty and still cannot be read. The darkest byte in the cell
costs nothing to compute and distinguishes them; the ink bounding box is
reported beside it but is explicitly NOT the blankness test, because an
empty box and a single lit pixel at the cell origin are the same four
numbers.

**A codepoint is accepted as well as a character**, and that is load
bearing rather than convenient: a space cannot be passed as an argument
through any shell here, and a space is exactly what you compare a
suspected-blank glyph against. The six Latin-1 extras cannot be typed on
the layouts this machine ships either. A single character always wins,
so `font glyph 0` is the digit.

## A window has two buffers, both mapped, and present passes an index rather than remapping

The File Manager flickered on every selection: a flash of panel
background where the rows should be. The cause was not in the app. A
window had ONE buffer, the client drew straight into it, and
`comp_map()` mapped those same physical frames into the compositor --
which repaints on its own cadence, because the taskbar clock forces a
repaint every second whether or not any client has presented. One
buffer, two readers, no synchronisation. `WIN_REQ_PRESENT` said "the
client has finished drawing; composite it", but nothing stopped the
compositor compositing before that.

An app that clears its whole surface before drawing -- which `files.c`
does, and which is the ordinary way to write an `on_draw` -- therefore
had a window in which the buffer held nothing but background, and the
compositor could read it there.

**Wayland's answer, for Wayland's reason.** A client attaches a buffer
and commits it; the compositor never reads one that is being drawn.
That is not a refinement there, it is why the protocol is shaped that
way, and the same shape ports directly.

**Both buffers stay mapped, in both address spaces.** The obvious
implementation is to keep one virtual address and remap it to the other
buffer on each present -- and that costs a page-table edit and a TLB
flush per frame, in the client's address space and the compositor's, on
the hottest path there is. Mapping both once at `base` and `base +
WIN_BUFFER_HALF` makes a flip an integer in a message. A window's slot
is 64 MiB and the largest buffer `WIN_CLIENT_MAX_W * WIN_CLIENT_MAX_H *
4` is 8 MiB, so address space was never the constraint.

**Contiguous physical memory is the constraint**, and it is why the
failure mode is what it is. Each buffer is a `pmm_alloc_contiguous()`
run -- contiguous so the kernel-visible pointer can be a plain
`uint32_t *` over the whole buffer rather than a per-page walk on every
composite -- and that allocation is ALREADY what refuses a window when
memory fragments. Doubling it doubles the pressure. So a window whose
second allocation fails is created **single-buffered** rather than
refused: it tears exactly as every window did before this existed, which
is strictly better than not opening. `front` stays 0, the flip is a
no-op, and neither the client nor the compositor needs a special case --
which is the property that makes the degraded path safe rather than a
second code path to get wrong.

**The flip happens inside the request**, before the event is queued and
before the ring-0 presentation hook runs. A client must know which
buffer is safe to draw into the moment `present()` returns, and the
compositor must never be pointed at a buffer the client has already
started on; doing it in the other order leaves a window where both are
true at once. The request returns the new front index biased by one, so
zero still means "refused" -- the same trick that keeps a sentinel out
of the value space.

**The invariant is tested as memory, not as a flicker.** Catching a torn
frame means sampling the screen fast enough to land inside one client
redraw: timing-dependent, flaky, and -- worst -- a check that passes
MORE often the faster the machine gets, which is the wrong direction for
a regression test to fail in. What actually has to hold is that two
pointers differ, and that is decidable: `winshare`'s "a present flips
the buffer" writes a marker into the back buffer, asserts the
compositor's front view cannot see it, presents, and asserts it now can.
Verified by deleting the flip and watching exactly that test go red.

## A move event has to reach every widget, and for a long time it stopped one level down

`uui_router_press()` and `uui_router_wheel()` walk nested containers to
any depth. `uui_router_motion()` walked exactly ONE level: it looked at
each top-level item, expanded it if it declared children, and delivered
to those children — and no further. Nothing failed loudly, because a
widget that never hears the pointer simply never lights up.

System Settings nests four deep (window column → body row → scroll view
→ page column), so **every hover state in that app was dead**, including
the rows inside a dropdown's open popup — which is where it was finally
noticed, reported as "the dropdown does not highlight". `uui_listbox`
had drawn a hover row all along and `uui_dropdown` had forwarded motion
to it all along; the event never arrived.

Three things the fix had to get right, none of them obvious:

- **A widget the cursor has LEFT still has to hear the move**, or its
  highlight stays lit after the pointer has gone. So motion is delivered
  to every widget, unlike a press, which stops at the first taker.
- **A container that clips its children must not let them light up
  outside itself.** A scroll view lays its rows out past its own edges;
  a row scrolled out of sight is at coordinates the pointer can really
  be over. Skipping the subtree would strand a lit row, and passing the
  real point would light an invisible one — so a clipped subtree the
  cursor is outside of is told a point NO widget can contain
  (`UUI_NOWHERE`), which every widget hit-tests as "not me" and clears
  from. No widget knows about the convention.
- **An open popup gets the real point first**, before any clipping,
  because it is drawn outside its own rect and usually outside its
  container's. That is the same precedence `overlay_active` already gave
  presses and wheels; it is skipped in the walk afterwards, since
  hearing the move twice would light a row and immediately clear it.

## An open popup takes the KEY, and a focused widget's key change is reported like a click

Two gaps found while giving the timezone list type-ahead, both of the
same shape: the keyboard had no equivalent of something the pointer
already had.

**`uui_router_overlay_key()`.** An open dropdown popup covers the window
and is what the user is looking at, so Esc, Enter, the arrows and a
typed letter belong to it — not to whatever held focus before it opened.
Every real toolkit does this. It is the keyboard's half of the
`overlay_active` rule, and it is also what makes typing in a dropdown
work in an app that has no focus ring at all.

**And the app is told.** `uui_focus_key()` changed a focused widget's
value and nobody heard: `on_widget` fired for presses, motions, releases
and wheels, so a control whose value the KEYBOARD changed was silently
dropped by Apply. It now reports `UUI_REASON_KEY` with the same id a
click on that widget reports — looked up by pointer through
`uui_router_id_of()`, because ids belong to the router and a second copy
of them in the focus ring would be a second thing to keep in step. Tab
is excluded: it moved focus and changed no value.

System Settings acts on `UUI_REASON_KEY` as it does on a release, and
deliberately not on `UUI_REASON_MOTION` — the rule it already had is
about not acting on a pointer merely crossing a control, and typing is
not that.

## Type-ahead lives in the listbox, and a repeated letter cycles forever while a prefix expires

A 92-item timezone list is unusable by arrow key. Typing jumps to a
match, as it does in a Windows combobox and a KDE item view, and it
lives in `uui_listbox_key()` so the popup, a standalone listbox and
anything else composing one all gain it from one place.

**The two behaviours are separate, and only one of them expires.** Keys
typed within a second build a PREFIX (`h`, `e` → Helsinki, past Halifax
and Hanoi); the same single letter pressed again CYCLES to the next
match. A pause abandons the prefix — `h`, a long pause, then `e` must
mean "an e", not "he" — but it must not abandon cycling, because `h`
pressed twice a minute apart should still reach the second h. Making the
timeout reset both was the first version, and it is wrong in a way a
test with a slow harness would not have caught: it depends on how fast
the keys arrive.

**It matches what is SHOWN, not what is stored** — "Los Angeles", not
`losangeles` — because the label is the only string the user can see.
The match is a prefix test, which is also what makes it immune to
System Settings appending "   (current)" to the row in effect.

A CLOSED dropdown accepts letters too, changing its value in place. That
is deliberately not the rule the wheel follows: a wheel notch over a
closed dropdown is ignored because the pointer is merely passing over it
and the value would change unseen, whereas a typed letter can only
arrive at the control that has keyboard focus.

## The pointer's shape is NAMED by the client and CLAMPED by the compositor

An I-beam over a text field is a request the client has to make, because
it is the only thing that knows where its text is — and one the
compositor has to be able to overrule, because a client is a separate
process that can be slow, wedged or dying.

**Why the client names it rather than drawing it.** X11 let a client
supply a cursor image, and Wayland's original `wl_pointer.set_cursor`
did the same; both meant every client loading the cursor theme, and
Wayland eventually added `cursor-shape-v1` to undo it. Windows settled
the same way: `WM_SETCURSOR` arrives, the app calls `SetCursor(HCURSOR)`,
and the system paints. toy-os has no choice anyway — a client draws into
its own buffer and the sprite is composited above every window — but
`cursor_theme.h` had already committed to the layering in its header
comment ("an APP names a shape, the COMPOSITOR owns the theme") and
shipped a `text` shape in both themes. What was missing was purely the
message.

**Why not a region list.** The rejected alternative was a client
publishing text-region rects the compositor hit-tests itself: no round
trip and no latency. It is a second geometry model that goes stale on
every scroll and resize, and it can only ever express "text" — a wait
cursor, or a drag cursor later, needs a different mechanism beside it.
A named shape costs one message per CHANGE and expresses all of them.

**Why the client's list is shorter than the theme's.** A theme has six
shapes; `WIN_CURSOR_*` has two. The resize cursors are the compositor's
own conclusion about a frame the client does not own, and a client
naming `resize-h` would be claiming an edge it cannot drag. Keeping
`enum wm_cursor_kind` and `WIN_CURSOR_*` as two lists with an explicit
mapping is the same call `cursor_theme.c` already made between the WM's
enum and the file format's order, and for the same reason: adding a
shape to either must not silently re-point the other.

**Why the clamp is the load-bearing part.** Honouring a client's named
shape everywhere would mean the last shape it named outlives the pointer
being over it — and "it will notice" is exactly what stops being true
when a component becomes a process (the same lesson the poisoned
compositor mapping records). `client_cursor_at()` honours the shape only
inside that window's content area, only for the topmost window at the
point, and never under the taskbar or an open popup. That bounds every
failure — a dropped event, a wedged client, a client that never resets —
to "a wrong shape inside one window until the pointer crosses a
boundary", which needs nobody to notice for it to end. It costs one
hit-test the compositor was already doing to route input.

**Why the value rides the event.** Every other client→compositor event
here is thin: it says a window changed and the compositor re-reads with
`WIN_REQ_WINDOW_INFO`. That is right when the detail does not fit in 24
bytes. A shape is one small int, `WIN_REQ_WINDOW_INFO`'s four return
slots are all spoken for, and a round trip would sit between the pointer
entering a field and the shape changing — the one place here where a
frame of latency is visible. The cost is that a DROPPED event (a full
queue) is not recoverable by re-reading; the clamp is what makes that
bounded rather than permanent.

**Why the redraw hook is a comparison, not a flag.** A client answers a
motion event some frames later, so the shape usually changes while the
mouse is standing still — and the cheap render path only runs on a move.
`wm_cursor_shape_changed()` compares the resolved shape against what was
last actually drawn, which is the same shape `prev_cursor_*` takes
("where the sprite really is", not "where we think we put it"): there is
no dirty bit for a future caller to forget to set or to clear.

**Why a widget slot AND an app call.** Text is not one widget here.
`uui_textbox`/`uui_textview` declare `WIN_CURSOR_TEXT` in
`uui_widget_ops.cursor` and every app that ever uses one gets the I-beam
with no code — that is the router's whole purpose, and the alternative
was every app re-implementing the same hit-test-then-set. But Notepad
draws its document itself and the Terminal draws a character grid, and
those are the two most obviously text-shaped surfaces in the system. So
the app-facing `uapp_set_cursor()` exists for them, called after the
widget tree has answered so an app has the last word over its own
window. The Terminal names its shape ONCE at open rather than tracking
motion, because its whole content area is text — which is exactly what
xterm does, I-beam over its own scrollbar included.

The slot is optional and nothing enforces it: NULL means
`WIN_CURSOR_DEFAULT`, which is the right answer for almost every widget.
That is why `tools/check_widget_ops.py` has no rule about it, unlike
`release` beside `press` — a missing `cursor` fails at nothing.

## The busy pointer has two sources, and every client is pinged on a cadence

`wait` shipped in both cursor themes with no caller, alongside `text`.
Wiring it raised a question `text` did not: who decides an app is busy?

**Two sources, because neither covers the other.** An app knows when it
is about to do something slow ON PURPOSE — Notepad writing a file, Image
Viewer decoding a JPEG — and can say so before it goes quiet. That is
Win32's `SetCursor(IDC_WAIT)` idiom and it is the only source that can
distinguish "working" from "broken". But the case where a busy pointer
matters most is the one an app cannot report: it is wedged, and naming a
cursor needs the event loop that is wedged. So the compositor raises
`WAIT` for a window that has stopped answering pings, and lets it
OUTRANK whatever that window last named. A stale `TEXT` from a client
that died mid-motion must not survive the client.

**`uapp_busy_begin()`/`uapp_busy_end()` rather than two
`uapp_set_cursor()` calls,** because the second one would have to name
what to go back to, and every app would answer that differently and
wrongly — the pointer sitting over a document would get an arrow until
the next mouse move. The toolkit remembers. It deliberately does not
nest: there is one level of caller today and a depth counter would be
machinery for a case that does not exist.

**The request lands even though the caller is about to block.** It is a
syscall, so the shape reaches the kernel synchronously and the
compositor reads its own queue on its own schedule. The rule is only
that it must be sent FIRST — an app that starts the slow work and then
says so has already lost.

**Why the ping needed a cadence.** `wm_client_ping()` had exactly one
caller: `wm_client_send_close()`. So `not_responding` — and therefore
the title bar's `(Not Responding)`, and now the busy pointer — could
only ever appear while the user was trying to close a window, which is
the one moment a hang is least surprising and most likely to be
explained by the close itself. A window that wedged while nobody was
touching it looked perfectly healthy forever.

`WM_PING_INTERVAL_DEFAULT` is 2s against a 3s timeout. It measures from
the last ASK rather than the last answer, reusing `ping_sent_tick` —
which outlives the serial a pong clears — so a window is only asked
again once the previous answer has landed and worst-case detection is
interval + timeout. The cost is one wakeup per client per interval: a
ping is an event, and an event wakes a client blocked in
`sys_wait_event()`. That is 2s against the 30ms the Terminal already
ticks at, and it buys the only liveness signal this desktop has.

**What the cadence deliberately does NOT change: a hung window nobody is
closing still raises no dialog.** `check_liveness()` gates that on
`close_asked_tick` and it stays gated. A modal appearing on its own, over
whatever the user was doing, for a window they never touched, is worse
than the hang — that reasoning predates this change and the cadence does
not weaken it. The title bar mark and the pointer are ambient; the
dialog is an interruption.

**The consequence to accept:** an app that blocks for longer than the
timeout on purpose now gets `(Not Responding)` as well as the busy
pointer. That is what Windows does, and suppressing the mark for an app
that had named `WAIT` would let a genuinely hung app hide behind one
call. The honest reading is that the two marks say different things —
"this app said it is working" and "this app is not answering" — and an
app that is both really is both.

## The console stops presenting under a desktop, and a panic overrides that

The framebuffer console draws into a kernel-owned back buffer and
publishes it with one full blit. Nothing on that path asked who owned the
screen, so any ring-3 process writing to fd 1 painted the whole text
console over the desktop. `dmesg` covered 100% of it; DOOM was how it was
noticed, because doomgeneric prints a startup banner and DOOM is the
loudest program in the tree.

**Suppress the PRESENT, not the drawing.** The back buffer is the
kernel's own and nobody is looking at it while the desktop is up, so
drawing into it costs nothing and is what keeps the text. Stopping
`fb_putc` as well would leave a hole in the console's buffer and its
scrollback, and returning to the console would then show a screen that
never existed. This is exactly Linux's `KD_GRAPHICS`: fbcon keeps
tracking the text and stops painting, and switching back to the VT shows
you what accumulated.

The precedent was already here and applied to one caller: `vga_reflow()`
skips its `fb_clear()` under a compositor, and its comment names the
mechanism ("fb_clear() ends in vga_present(), which blits the console's
whole buffer over whatever the desktop has on screen"). The guard just
never moved to the place every caller passes through.

**A PANIC MUST OVERRIDE IT, and that is why there are two entry points.**
The fault handler calls `vga_present()` explicitly, because a panic halts
without ever reaching the idle loop that normally publishes. Guarding
that call would have made every panic under a running desktop invisible —
the machine would stop with the desktop's last frame on screen,
indistinguishable from a hang, which is the single worst thing to break
while fixing a cosmetic bleed. So `vga_present()` is the routine path and
carries the check, and `vga_present_force()` ignores it.

**Default-safe, in that direction specifically.** A new routine caller
gets the check without knowing it exists; the two callers that mean to
paint over whatever is there — the panic report, and `vga_resume()` — say
so. `vga_resume()` uses the forced form even though it runs after the
compositor has already been deregistered: a repaint whose whole purpose
is "the desktop is gone, show the console" must not become silently
skippable if someone later reorders `win_server_set_compositor()`.

**Where a GUI-launched program's output goes was left alone.** A child
inherits the WM's fds, so it still writes to `tty0` — now invisibly, and
readable after the desktop exits. Sending it to the kernel log instead
would show it in `dmesg` while the desktop is up, which is better for
debugging, but it makes a GUI-launched process's fds differ from a
shell-launched one's for no rule anyone could state. Discarding it would
destroy the output of an app trying to report why it failed to start.
Fixing the paint fixes every writer at once, which neither of those does.

**And it is not an argument for `Terminal=`.** The question "should DOOM
open a Terminal window to show that text?" has a clear answer: no. No
desktop shows a GUI app's diagnostics that way, and it would put a window
in front of the user on every launch. freedesktop's `Terminal=true` is
for programs whose INTERFACE is a terminal — `edit`, `tosh` — which toy-os
still cannot launch from the desktop at all. That is a real gap and it is
on the roadmap; it is a different feature from this bug.

**A ring-3 crash no longer takes the override** (2026-10-02). The
fault handler used to force-present for EVERY fault, so a crashing
app -- or the compositor itself -- flashed the whole text console over
the desktop for the second it took init to restart it, too briefly to
read. It now forces only when the fault is unrecoverable; a ring-3
crash goes through the routine path like any other output. What the
user sees instead is the next entry.

## A crash is said in the desktop's voice: a held frame, a notice, a dialog

Two things went wrong when a ring-3 program crashed. An app's crash said
nothing on screen at all -- its window vanished and a report landed in
`/var/crash`, found only by someone who knew `crashlog`. And a crash of
the COMPOSITOR flashed the kernel console for about a second, which is
too short to read and long enough to alarm.

**What real systems do.** Windows' display driver recovery (TDR) keeps
the last frame, blanks briefly, and says afterwards "Display driver
stopped responding and has recovered"; Windows Error Reporting and
Reliability Monitor hold the per-app record. macOS shows "<App> quit
unexpectedly" with Reopen and a report. KDE's DrKonqi notifies with
Details, and GNOME's ABRT keeps a list. Linux's own kernel side is
`signal(7)`'s "Core" default action: a crash is a signal whose default
dumps core, which is why a `SIGSEGV` sent by `kill` counts too. toy-os
copies the SHAPE of all four: a notice, a dialog, a list, and a frame
held across a compositor restart.

**The held frame is the KERNEL's, because nothing else is left.** When
the compositor dies, its windows' memory goes with it and init has not
started the next one yet. The kernel still has the scanout, so
`win_surface_hold_frame()` copies the front buffer into the console's
back buffer, dims it, draws a "Restarting the desktop" card, and
`vga_present()` goes quiet (`vga_held()`) until a new compositor
registers or `VGA_HOLD_S` passes -- then the console comes back, because
a desktop that is not returning must not leave a picture of itself
standing in for a hung machine. Only a compositor that CRASHED holds --
a fault, or a signal whose default dumps core (`signal_dumps_core()`). A
clean exit or a `SIGTERM` is someone asking the desktop to go, and hands
the screen straight back.

**The WM learns of a crash from the kernel, not by inference.** A window
disappearing is also what a clean exit looks like, and watching
`/var/crash` would miss a crash whose report was refused (a busy
filesystem, the legacy loader). So the kernel keeps a ring of this
boot's last eight ring-3 crashes as `QUERY_CRASH` records, each with a
monotonically increasing `seq`, noted BEFORE any refusal; the WM polls it
twice a second and tells each `seq` once. A restarted WM reads the ring
at startup, and a toywm crash within the last minute becomes "The
desktop restarted after a problem". A record says whether a report was
saved, so a refusal is said, not hidden.

**The notice is a PASSIVE overlay.** It takes clicks on its own cards
and nothing else, does not count as "a menu is open", and expires after
12 s unless the pointer is on it. An ordinary overlay would have
swallowed the next click anywhere on the desktop, which is the wrong
cost for a message that needs no answer.

**One app, two faces.** `/bin/wm/apps/crashreports --report <path>` is
the dialog (the notice's Details), and with no argument it is the list
(Start menu, and the dialog's All reports). They parse the same report
header and say the same things in the same words, which two programs
would drift apart on. The dialog has its own `app_id` because the WM
remembers a window's size per id.

**A core-dumping signal writes a report too.** `SIGSEGV`, `SIGILL`,
`SIGFPE` and `SIGABRT` whose default action kills the process now
write `Killed by SIG<name>`, as Linux dumps core for them. Without it
Crash Test's "crash the desktop" -- a `SIGSEGV` sent to toywm -- would
restart the desktop with nothing to say why.

## A client may post an event to itself, and only that

`WIN_REQ_EVENT_PUSH` was compositor-only, and for a good reason: it is
the one request in the protocol that reaches across processes, so a
client able to call it could synthesise a keystroke into any other
program. The protocol's entire access-control story is "a window belongs
to a process", and that request is the exception.

Threads made the restriction cost something. A Toykit app blocks in
`SYS_WAIT_EVENT`; a worker thread that finishes has no way to wake it,
so the completion is only noticed on the next `tick_ms` — and the whole
point of moving work off the event loop was to stop paying a cadence.
Every real toolkit solves this the same way and calls it something
different: Qt `postEvent`, GTK `g_idle_add`, Win32 `PostMessage`, and a
Wayland client an eventfd in its poll set.

**The relaxation is bounded twice: the target must be 0 ("me") and the
type must be `WIN_EV_USER`.** Bounding the target is the obvious half —
a process can already do anything it likes to its own state, so letting
it wake itself grants nothing new. Bounding the TYPE is the half worth
explaining: without it a client could push itself a `WIN_EV_KEY`, and
while that harms nobody else it would make "an event of type KEY came
from the compositor" false, which is a property a diagnostic reading a
log should be able to rely on. A stray self-post can never be mistaken
for input.

**Two numbers, not a pointer.** An event here is a message — fixed
layout, pointer-free, readable in a log — which is why the protocol
spells every other event out field by field rather than copying a
struct. A worker with a larger result puts it where both threads agreed
and posts an index. That also keeps the door open for the payload to
cross a process boundary later, which a pointer would have closed.

## Window corners are rounded by blending over the saved backdrop, not by a mask the compositor has to understand

The maintainer asked for subtle rounded corners on windows that are
not maximized (2026-09-02). KDE Plasma and Windows 11 both do this,
and both square the corners of a maximized window; Plasma does it in
the decoration plugin and DWM in the compositor, and either way the
compositor knows the window's shape.

toy-os's compositor does not, and deliberately. The obvious design is
a per-window shape mask consulted by damage tracking, hit testing and
the blit; that is Wayland's `wl_surface.set_opaque_region` and X11's
SHAPE extension, and it touches every path that thinks a window is a
rectangle. What was built instead leans on one property this
compositor already has: it repaints everything inside the damage box
back to front, with no occlusion culling. So the backdrop under a
corner is on the surface the moment a window starts to paint. The
renderer copies those few pixels aside, lets the window paint its
rectangle, then blends them back by a quarter-disc's coverage. No
other code learns that a corner is transparent. Hit testing stays
rectangular, which is also what Plasma and DWM do -- a click in the
corner is the window's.

The cost is honest: four small squares saved and blended per window
per frame, a few hundred pixel operations at a five-pixel radius, and
`CORNER_MAX_R` caps the static buffer. The one path that must not
change is `wm_render_cursor_move()`'s save-under, which never repaints
windows and therefore never sees a corner.

The radius is font-derived (`ugfx_char_h() / 2`, 8 px at the default
font, Plasma's) because every other chrome measurement here is -- a
third, the tab strip's radius, shipped first and the maintainer read
it as still square -- and hard pixels were
rejected for the same reason the title buttons are discs: at this size
an aliased arc reads as a staircase.

## Terminal tabs are the application's, not the window manager's

Three places could own tabbing and all three exist in the wild. Konsole,
GNOME Terminal and Windows Terminal put the strip in the APPLICATION.
KDE also does window tabbing in the COMPOSITOR — group any windows into
one tabbed frame — which would have given toy-os tabs for every app from
one implementation, and Terminal would have needed no changes at all.
And tmux does it inside the terminal PROTOCOL, which is why a tmux
session survives losing its terminal.

toy-os took the application's, and the argument is what a tab has to
carry. A tab here is a pty, a shell, a grid, 240 lines of scrollback, an
alternate screen and a title. A compositor-level tab is a whole window,
so eight tabs would be eight Terminal processes — and the strip could
show neither the shell's title nor anything about its state, because the
compositor knows only that a window exists. The protocol answer (tmux)
solves a problem this system does not have: nothing here can lose its
terminal and come back.

**What it costs is that no other app gets tabs for free**, which is why
the strip is a widget (`uui_tabs`) rather than terminal code: the second
app that wants one inherits the arithmetic, the hit-testing and the
press/release discipline. The widget deliberately knows nothing about
what a tab contains.

**A SESSION IS A FEW HUNDRED KiB and the cap is eight.** Grid,
scrollback and saved screen are what dominate; the grid GROWS to fit
the window since 2026-08-28 (a fixed 200x60 left a maximized 1080p
terminal with a dead band below row 60 and right of column 200), so the
bound is now the window itself, which the compositor clamps to
WIN_CLIENT_MAX. Eight sessions at a full 1920x1080 grid stay around two
megabytes. Slots are allocated on demand and REUSED rather
than freed, so opening and closing tabs all day does not churn the heap
— and a slot is only reusable once its reader thread has set `done`,
because handing a live thread's ring to a new tab would be two producers
on one queue.

## Terminal has no tick any more, and that is the point of the threads

The old window polled its one pty every 30 ms because there is no
`poll()` here to wait on the compositor and a child at the same time.
That cost 33 wake-ups a second on an idle window and put up to 30 ms of
lag on every echoed keystroke — and N tabs would have been N polls per
tick.

A reader thread per session blocks in a real `read()` instead, copies
what arrives into a single-producer/single-consumer ring, and calls
`uapp_post()`. The window's loop now blocks in `SYS_WAIT_EVENT` and
wakes only when a key arrives, the compositor says something, or a
reader has bytes. `tick_ms` and `on_tick` are gone from the descriptor
entirely.

**The ring rather than the parser is what the thread may touch**, and
that is not caution — the grid, the scrollback and the parser are read
by the painter on every frame, so a reader writing into them would be
racing the frame it is trying to cause. Bytes cross on the ring and the
main thread does every parse. The same rule every UI toolkit has, stated
in `docs/conventions/gui.md`.

**A full ring makes the reader WAIT.** Dropping output would corrupt a
screen in a way nobody could diagnose from the result — a missing escape
sequence looks like a terminal bug for the rest of the session. Waiting
costs that thread a slice and nothing else, because the thing it waits
for is the window draining, which is the window doing its job.

## The desktop's right-click menu is `uui_menubar`; its Start menu is not

Both were hand-drawn in `userland/wm/`, and the obvious tidy was to put
both on the toolkit's menu widget. Only one of them belonged there.

**The context menu did.** Qt's QMenu and GTK's GtkPopoverMenu each serve
a menu bar's dropdown and a right-click menu, and two implementations of
"a menu" drift in the ways a user notices: different padding, a
different tick, arrows that work in one and not the other. That had
already happened here — the panel drew a `>` character where an app drew
a triangle, two blocky strokes where an app drew an anti-aliased tick,
and labels at `y + 3` where an app centred them. `context_menu.c` is the
panel's item model over the widget now: a row is still a
label+callback+ctx, because `desktop.c` packs a `gui_app *` and a window
index into one, and the commit is looked back up from the widget's
`code`.

It works inside the compositor because `uui_popup_open()` is a no-op
without a provider and the panel installs none for its own surface — so
every level takes the in-window path and draws into `wm_surface()`,
clamped against the rectangle `wm_popup_place()` already defined.

**The Start menu did not.** It has per-row icons, an indent kept when
the artwork is missing, a group divider that consumes no row, a click
flash in its own colour on a tick deadline, and rows from a live
registry rather than a const tree. That is four features added to a
widget no other caller wants them in, to delete about eighty lines of
drawing — and it would make every app's menu carry the slots.

Real desktops split it the same way: Win11's Start is a XAML shell
surface and KDE's Kickoff a QML applet, neither built from the menu
control both use for an application's File menu.

## `uui_menubar` kept its hand-routed interface when it gained an ops table

Terminal needed a menu bar and already declared a routed widget (the tab
strip). That combination is a bug the toolkit could not express: the
router runs before an app's own `on_press` (`uapp.c`), so a popup drawn
over the strip commits a menu item AND clicks the tab underneath it.

`uui_widget_ops` already had the mechanism — `overlay_active`, built for
the dropdown, which offers a widget every press before anything is
hit-tested — and `uui_menubar` was the one control in the toolkit that
never declared it. So it got an ops table, and the ordering is now a
property of the design rather than of a guard someone has to remember.

**The obvious next step was to delete the hand-routed calls and convert
Notepad, Files, Imgview and Mines. Three of the four are converted; the
two interfaces are still not redundant.** An app with no routed widgets
— Minesweeper has none, and paints its board itself — gains nothing from
the table.

The argument that kept Notepad out was app-level modality: its file
dialog has to outrank the menu, and it expressed that by simply not
calling into it, where converting looked like it would need the router
to learn a notion of modality nothing else wanted. That turned out to be
wrong, and the reason is worth keeping. The dialog became a routed
`uui_dialog` declared LAST in `desc.widgets`, and modality is then a
property the widget already had: a widget claiming the overlay is
offered every press first, with no hit test, and is drawn after the
others. The app sequences nothing. What looked like a missing router
feature was a widget that had not been written yet.

So the rule is a rule about the APP, not about the widget:
`uapp_desc.widgets` non-empty means use the table. That is stated in
`docs/conventions/gui.md`, and it is the kind of thing a future session
would otherwise re-derive by shipping the click-through bug once.

**The commit code is parked rather than returned** because the ops
`release` slot returns "did anything change" and there is nowhere for a
code to travel. `uui_menubar_take_code()` clears on read, so a redraw
cannot replay a command — the alternative, a callback on the struct,
would have been a second reporting mechanism beside `on_widget` for a
widget that already has one.

## A tab strip's selected tab is a light lift, not the page's own colour

The first strip drew every tab as a four-sided box with an accent bar on
top. It read as a row of buttons, because that is what it was.

The replacement tried what Windows Terminal and Konsole do: fill the
selected tab with whatever the PAGE below it is, round its top corners
and drop its bottom edge, so the tab and the page are one shape. It was
built, and it is worth recording why it came back out. **A terminal's
page is black, and this theme's chrome is near-white**, so the selected
tab became a solid black block sitting in a light strip directly under a
light menu bar. The merge is a good effect when the chrome is already
dark — which is the case in every system that does it — and a heavy one
when it is not.

What replaced it is the other standard answer: the selected tab takes
the theme's FIELD colour, rounds its top corners, and carries a 2px
accent bar. VS Code marks a tab this way (`tab.activeBorderTop`).

**Where the accent goes and how dark the resting tabs are were both got
wrong first, and the corrections are the useful part.** The accent
started on the BOTTOM edge, on the reasoning that it is the edge
touching the page — but in a terminal the page is black, so a thin blue
line there has the least contrast of anywhere it could be. It is on the
TOP edge now, against the light chrome. And resting tabs were left the
strip's own colour, which made the selected tab a twenty-unit lift; that
shipped and was reported as hard to tell apart with three tabs open.
Resting tabs took the CONTROL colour next — darker than the ground, so
the strip reads as wells with one tab raised — and the lift was thirty.
It is fifty now: the maintainer asked for darker resting tabs, and they
got a theme token of their own (`tab_rest`) rather than a darkened
control colour, because every colour here is the theme's and a dark
mode needs to set this one independently.

**The tabs pack left at their natural width, and equal shares are the
fallback rather than the rule** (2026-09-02). The first strip shared
the whole width equally, GtkNotebook-style, so one tab stretched across
the window — the maintainer asked for Konsole's shape. The convention's
reason for equal shares was real: a title arrives from the shell
asynchronously, and a tab that resized under a pointer heading for its
close box moves the target. Chrome has the same problem and freezes tab
widths while the pointer is in the strip, relaying out when it leaves;
that is what was built. A count change relays out at once, which is
where this differs from Chrome (whose next close box slides under the
pointer after a close) — a natural-width strip cannot promise that
anyway, and the honest behaviour is the immediate one.

**The lesson generalises past this widget.** Twenty units out of 255
survived a pixel check, a screenshot review and a written claim that it
was enough. What settled it was three tabs open at once, which is the
state the strip exists for and not the state it was looked at in. A
contrast judgement made on two elements does not transfer to six.

**And the fills were not the whole problem.** A terminal's tabs report
their shell's directory, so every tab in the same place carries the
SAME label — three tabs reading `/` are indistinguishable however they
are shaded. `uui_tabs.numbered` prefixes `1: `, `2: ` past one tab
(Konsole's `%n: %d`, iTerm2's), which fixes identity and count together.
The widget draws it rather than the caller baking it into a label,
because a label is not owned and a shell rewrites its own
asynchronously.

**The Terminal no longer numbers its tabs** (2026-10-02, the
maintainer's call in the one-bar redesign). The cost is the one above:
two tabs in the same directory read alike again. `numbered` stays in the
widget for a strip that wants it.

**Every colour is the theme's, which is what the page-colour version
could not be.** `page_bg` had to be supplied by the caller, because the
widget cannot know what an app paints below it — so a dark mode would
have moved the strip and left that one fill behind. With the fill coming
from `utheme_current()`, swapping the palette moves the whole strip.

**The baseline is drawn under every tab and broken by the selected tab's
accent.** Computing where to break it would be a second piece of
arithmetic that has to agree with the fill to the pixel, which is the
shape of the close-box bug this widget's own comments warn about.

## The Terminal is one bar, and its menu bar hides behind a button

Chosen from mockups on 2026-10-02 (the Terminal redesign canvas) over
the app design language's full anatomy -- menu bar, coloured command
bar, status bar -- and over the same bar with a Session panel open by
default.

**Every other app here wears menu bar, command bar and status bar; the
Terminal deliberately does not.** In a terminal a row of chrome is a row
taken from the program running in it, so Windows Terminal, GNOME
Console and Ptyxis all put the tabs, a new-tab split button and a menu
button in ONE row and drop the menu bar. Konsole keeps one, and lets it
be hidden. toy-os follows the first group: the bar holds the tabs, "+"
and a ▾ listing `/etc/shells`, Find, the Session-panel toggle and ☰.
The menu bar still exists, hidden by default (`menubar=on` shows it);
F10 reveals it, and the ☰ menu holds the same commands, so hiding the
bar hides no command.

**The tabs could not move into the title bar** as Windows Terminal's
do: the window manager draws it (server-side decoration), and a client
that drew its own would be client-side decoration, a separate project.

**The selected tab keeps the light lift** (the tab-strip entry above).
The merged dark tab was mocked up again for this layout -- no light menu
bar above it, a grey page rather than black -- and the maintainer chose
the lift anyway. The tabs FLOAT in the bar (`uui_tabs.floating`: centred,
rounded all round, no baseline) -- Firefox's and libadwaita's tab bar --
because on the bar's foot the white tab ran straight into the page.

**The Session panel is the toy-os addition.** None of the three
terminals shows the tab's process; here a right-hand panel does (the
shell, its pid and group, the processes under it, scrollback use, and
Interrupt / Send EOF / Force quit), because the shell in a tab being a
real process on a real pty is what this terminal is. It is read from
`SYS_PROC_INFO` on each paint and repainted on the tick while open.

**Find is a toolkit widget that asks rather than searches.**
`uui_findbar` draws the query, the "N of M" readout and the buttons and
reports events; the app finds. A terminal's virtual buffer and a
document are searched differently, and a widget that searched one would
not fit the other.

**Options is a window of its own**, File Manager Options' sidebar of
pages, replacing a dialog drawn inside the terminal. The old placement
was argued from a client being unable to open a second window, which
`uapp_window_open()` has since made untrue; the terminal stays visible
beside the window either way.

## A focus indicator is one helper in the theme's accent, drawn by the widget

Every widget that accepts keyboard focus draws a 1px ring through
`uui_focus_ring()` (`userland/ui/uui_primitives.c`) in `UTHEME_ACCENT`.
Three alternatives were real and two were rejected.

**Why the accent rather than a wash of the control's own colour.** The
four widgets that had an indicator before this all derived it as
`uui_state_bg(fg, UUI_STATE_HOVER)` -- the same 22/255 shift the hover
state uses. That is defensible in isolation and wrong as a system:
focus and hover answer different questions ("where will my typing go"
against "what is under the pointer"), and a shared visual vocabulary
makes neither legible. `utheme.h` has named `accent` as the
`selection / highlight / focus / checkmark` role since it was written
and nothing had ever used it for focus. GTK, Qt and Win32 all draw
focus in the accent, and the File Manager had independently arrived at
`UTHEME_ACCENT` for its active-pane outline -- a second caller for the
same visual meaning, reached without coordination.

The cost is explicit: an accent ring does not follow a control's own
colour the way a derived wash does, so a widget on a background close
to the accent gets a weak indicator. Accepted, because there is one
palette and the accent is chosen to contrast with the surfaces in it.

**Why the widget passes the rect rather than the focus manager drawing
it.** `uui_focus` knows which item is current and every ops table
carries `bounds`, so one rectangle drawn there would have covered every
widget including any added later, with no per-widget code at all. It
was rejected because a rectangle round the whole widget is wrong for
exactly the widgets that needed this most: a table or a sidebar is a
list of rows and the keyboard's cursor is ON A ROW, so a box round a
300px table says "somewhere in here". The helper takes a rect for that
reason, and each widget passes the shape it knows -- the row for a
list, the thumb for a slider, the whole control for a spinbox (whose
Up/Down keys belong to the steppers, not to the field it embeds).

**Why a row widget falls back to ringing the box.** A list can hold
focus with nothing selected, or with the selection scrolled out of
view. Drawing nothing in that case would reintroduce the original
defect -- an indicator that disappears is not one -- so the ring goes
round the widget instead, which is still true and still findable.

**And why `uui_listbox` got one despite having declined.** Its ops
table carried a comment saying a listbox has no focused state of its
own because "selection is already visible". That is the argument this
change exists to reject: a selected row looks identical whether or not
the list is the control answering the arrow keys, so two lists side by
side say nothing about which one is listening. Windows greys an
unfocused ListView's selection and GTK dims it for the same reason.

See `docs/conventions/gui.md` for the rule, and
`/tests/focusring_test` for the assertion -- each widget checked both
ways, because a one-sided check passes on a control that rings itself
unconditionally.

## The icons view is a MODE of uui_fileview, not a new widget

`UUI_FILEVIEW_ICONS` joins `LIST` and `DETAILS` on the same widget,
even though it is the first mode that cannot forward to `uui_table` --
the grid draws, hit-tests, scrolls and moves the keyboard itself. A
separate `uui_iconview` widget was the obvious alternative and was
rejected.

**Why one widget.** The hard-won state is not the drawing: it is the
row model (the synthetic `..`, the caller-owned entries), the mark
bitmap and its clear-on-reload rule, the sort order, and a dozen path
accessors that apps and four test tools already speak. A second widget
would either duplicate all of it or grow a shared "directory model"
layer that only these two widgets would ever use. Win32 made the same
call: LVS_ICON is a STYLE of one ListView, not a sibling control, and
switching styles there keeps the selection for the same reason it does
here. The seam was already in place -- `uui_fileview_set_mode()`
existed, and every input path dispatches on the mode in one place.

**What stays the table's even in icons mode.** The selection
(`table.selected`), the marks, the sort permutation (the grid displays
`uui_table_source_row()` order, so directories still lead), and even
type-ahead -- a letter goes through `uui_table_key()` and only the
reveal is the grid's. Switching modes therefore never loses state, and
switching back finds the header sort untouched.

**The rubber band's selection IS the marks.** The sweep could have had
its own selection set beside the marks (the desktop's band has one),
but a file manager already has exactly one "set of files the next
operation acts on" and two would need a precedence rule no user could
predict. So every band motion applies toggle-to-match onto the marks,
and `rb_end()`'s plain-click-clears rule becomes "click empty space to
unmark everything" for free. The cost: a timed reload clears marks
mid-sweep, so the app skips its generation poll while
`uui_fileview_band_active()` -- the same interlock the desktop's
`desktop_drag_active()` encodes.

## uui_tree stays static -- a lazy tree is the app's rebuild

The File Manager's folder column needed a tree whose directories load
on expand. The obvious extension -- give `uui_tree` a
populate-children callback and let it splice nodes into its own model
-- was rejected; instead a node can DECLARE its parenthood
(`UUI_TREE_CLOSED`/`UUI_TREE_OPEN` in `uui_tree_node.kind`), and an
expander click on a declared node reports through
`uui_tree_set_on_toggle()` and changes nothing. The app relists,
rebuilds its flat array, and hands it back with
`uui_tree_set_nodes_keep()`.

**Why the widget cannot own the loading.** Toykit widgets own no
memory and do no I/O -- the nodes are a caller-owned flat array
precisely so there is no allocator and no teardown, and a widget that
lists directories has crossed into being an app. GtkTreeView splits at
the same joint (test-expand-row asks, the MODEL loads, row-expanded
reports); this is that split with the model layer left out, because
one caller does not buy a model abstraction.

**Why kind is per-node rather than a widget-level "lazy" flag.** A
declared parent is the only honest way to draw an expander on a node
whose children are absent from the array -- derivation from the depth
run cannot see them. With the app owning the open set, the widget's
collapse bitmap goes unused in a lazy tree, which is also what frees a
lazy tree from `UUI_TREE_MAX_NODES` (the cap bounds the bitmap, and
only `UUI_TREE_AUTO` nodes use it).

**The open set is keyed by PATH, ids are slots.** A rebuild renumbers
every node, so the app remembers what is open (and what was selected)
by path and re-selects after `set_nodes_keep()`. The widget keeps only
the scroll position across the swap -- dropping it flung the view back
to the root on every expand, which is why `set_nodes_keep()` exists
rather than apps poking `t->top`.

## The toolbar asks the menu bar's own item_flags, and tooltips ride the tick

`uui_toolbar` has no checked/enabled state of its own: an item carries
the CODE its menu item commits, and the widget asks the same
`item_flags(int code)` callback `uui_menubar` does. The alternative --
per-item state setters, Win32's TB_CHECKBUTTON -- was rejected because
it is exactly the two-sources drift the menu bar's own "state is asked
for, not stored" rule exists to prevent: a View toggle would have a
menu tick and a toolbar latch that some code path forgets to move
together. Qt solved this by making both controls host one QAction;
with no action object here, one callback keyed by code is the same
guarantee. The File Manager passes literally the same function to
both.

**Tooltips ride the app's tick.** A tooltip appears a moment after the
pointer STOPS, and no input event announces "time passed" -- something
must re-evaluate on a cadence, and a Toykit widget cannot repaint its
window. Rather than give the toolkit a timer service for one feature,
`uui_toolbar_tick()` is called from the app's existing `on_tick` and
returns "repaint needed". The stated cost: tooltip latency is the
app's tick granularity (the File Manager's 500ms tick puts a tip at
0.5-1s), and an app with no tick gets no tooltips -- its buttons still
work, so the degradation is the feature, not the control.

## Thumbnails decode on the tick -- not in the draw, not in a daemon

The icons view's thumbnails could have been decoded where they are
drawn (simplest), in a worker thread (the `uapp_post()` pattern's
stated use case), or by a thumbnailer service (what freedesktop
desktops actually run). The tick won.

**Why not the draw.** A JPEG decode is unbounded work, and the draw
path runs per frame: entering a folder of photos would freeze the
window for as many full decodes as it has files -- the exact class
CLAUDE.md's "long work belongs in a child process" rule exists for,
except a thumbnail's RESULT must land in the app's own memory, so a
`/bin` child cannot carry it. The widget contract states it: the thumb
callback is a LOOKUP, and `uui_fileview` never decodes.

**Why not a thread or a daemon (yet).** The worker-thread shape is
right in principle and was deliberately deferred: the tick version is
single-threaded, ~40 lines, and bounds the stall to one decode per
pass -- if a directory of huge photos ever makes that stall felt, the
thread is the upgrade and the cache/callback seam does not move.
A thumbnailer SERVICE (freedesktop's daemons, Windows' thumbcache) is
off the table for the same reason the association resolver has no
daemon: no query IPC, and nothing at this scale to amortise.

**The cache key is path + mtime + size.** A rewritten file re-decodes,
a renamed one misses and re-decodes under its new name, and eviction
is the icon cache's LRU-over-a-fixed-table. Pending entries decode
most-recently-WANTED first, so what is on screen populates before
what was scrolled past.

## File associations: declarations plus an override file, and no service

Asked directly ("a manageable database file? and a service that
handles it?"), so the answer is recorded. The declaration side already
existed -- `Handles=` on each app's `.desktop` entry -- and the choice
was what to add: a central database file replacing it, a resolver
daemon, or freedesktop's split. The split won: apps DECLARE where the
app lives (installing one brings its associations), the USER overrides
per extension in `/etc/mimeapps.conf`, and the override outranks.

**Why no central-file-as-single-source.** Windows' Registry shape
means installing an app no longer brings its associations -- the
central table must be edited in step with every app added or removed,
which is the third-file-someone-must-remember shape this repo keeps
deleting.

**Why no daemon.** Linux and KDE resolve associations IN-PROCESS
(gio/KService over generated caches); macOS's LaunchServices daemon is
the outlier. Here a daemon would need query IPC that does not exist
(no sockets; pipes carry no credentials and only 0/1/2 inherit), to
answer what `userland/lib/uopen.c` answers with one directory scan --
and a cache is not needed at a dozen entries. If entry counts ever
make the scan felt, the freedesktop answer is a GENERATED file
(mimeinfo.cache), still not a process.

**The override names a desktop ENTRY, not a binary.** `.txt=imgview`
survives an app's Exec moving; `/path` stays as the escape hatch; a
dangling override FALLS THROUGH to the declarations, because a stale
choice must degrade to the default, not to an unopenable type.
`/bin/open` is the second caller that earned the library, and its
`-s`/`-l` verbs are what make the file manageable without an editor.

## The hardware cursor: the kernel moves it, the compositor shapes it

The plane the M41 migration measured away (see the entry above on the
ring-3 migration) is consumed now: `wm_hwcursor.c` defines the sprite
over `WIN_REQ_FB_CURSOR` and the software sprite stands down. Three
calls were real forks.

**The kernel moves the plane; the compositor never sends a position.**
`win_input.c` calls `gfx_hw_cursor_move()` at the exact line it already
holds screen coordinates for the compositor's mouse event -- so pointer
motion costs ZERO syscalls, and the plane moves even before the WM's
event loop wakes. The alternative (everything through the request, one
syscall per motion at compositor cadence) was cleaner layering and
strictly worse at the one thing a pointer must be, which is immediate.
On QEMU + virtio-gpu the host renders the plane as the actual host
pointer, so motion is host-latency and crossing the window edge has
nothing to hand over -- the seamlessness this was built for.

**The handover is per SHAPE, not per boot.** `wm_hwcursor_sync(kind)`
answers for the shape being resolved this frame: a themed shape that
fits the plane's fixed 64x64 rides it; `cursor_size=huge` (3x scale can
exceed 64px) and the built-in resize/text/wait shapes (draw calls, not
masks) fall back to the software sprite for exactly as long as they are
the resolved shape. The sprite bookkeeping stands down BY THE SAME
ANSWER -- `draw_cursor_at()` skips save-under and draw, the damage adds
no cursor rect, and the prev box records zero size -- because the
roadmap's own warning was that this bookkeeping "has to be disabled
cleanly rather than bypassed" (a stranded-sprite bug lived there).

**The sprite is the theme's masks, composited once.** cursor_theme.h
promised the mask pair to a hardware plane when it was written; the
compositor folds outline-under-fill into straight ARGB per shape
change, instead of per pixel per frame. The plane belongs to the ROLE:
`win_server_set_compositor()` hides and disarms it wherever the role
dies, beside the framebuffer grant's revoke.

The trap that cost the debugging round: `vmm_copy_from_user()` returns
1 ON SUCCESS (vmm.h's convention), not 0 -- the DEFINE handler read it
errno-style and refused every valid sprite. Read the ABI comment of any
call you swap in; this entry is that rule's newest receipt.

## Scroll direction and speed are one kernel knob, not per-app options

`mouse.scroll_dir` (normal/inverted) and `mouse.scroll_step` (1..5x)
are registered settings applied in `mouse_get_wheel_delta()` -- the one
point every wheel source (PS/2's 4th byte, virtio-input's REL_WHEEL)
and every consumer share. Applied at the CONSUMING read rather than at
feed time, so a multiplier cannot re-scale notches that accumulated
between reads of the setting. Registering them beside the pointer's
speed/accel is what puts them on System Settings' Mouse group with no
UI code at all -- the settings registry's whole point. Per-app scroll
options were not considered seriously: an inverted wheel is a fact
about the MOUSE, and every desktop (macOS natural scrolling, Windows,
KDE) keys it system-wide.


## uui_scale is continuous; uui_slider is a discrete enum

Two value controls, and the split is GTK's: `GtkScale` is a number on a
range, and a widget for an ordered enum is a different control. It was
forced by the Audio Player needing a position bar and a volume control
and finding nothing in Toykit that fitted -- `uui_slider`'s value is an
INDEX into a caller's option array and it draws a tick per stop, which
is exactly wrong for a three-minute song, and `uui_meter` is a reading
with no `hit` at all.

**Two real callers in the change that added it**, which is this
project's bar for an abstraction rather than a plausible third one
later; the future volume-mixer UI and a copy-progress bar are the ones
after that.

**It carries no label**, unlike `uui_slider`, which draws the selected
option's name. A scale's readout is text the app already knows how to
format ("1:23 / 3:45", "60%") and where it belongs differs per app, so
it goes in a `uui_label` beside. That also sidesteps the trap
`uui_meter` documents: a widget whose height depends on which strings
happen to be set changes size as its value changes.

**Dragging reports every motion, and the app chooses what that means.**
Volume acts on `UUI_REASON_MOTION` and follows the thumb live; a seek
acts on `UUI_REASON_RELEASE`, because re-seeking a decoder per pixel is
work nobody asked for. A control that only reported the release could
not do the first, and one that only reported motion would make the seek
bar thrash -- so the widget reports both and stays out of it.

A click anywhere on the track JUMPS there rather than paging towards it.
That is what every scale outside a Win32 trackbar does, and paging needs
a second concept (the page size) to mean anything.

**A DRAG NEEDS THE BUTTON STILL DOWN, AND THE POINTER GRAB IS NOT THAT
FACT.** A widget holds the grab from its press to its release, so
"`dragging` is set" only means the press was ours -- a motion can arrive
inside that window with nothing held, and a handler that treats it as a
drag moves the value to wherever the pointer is. It cost a real check:
a click on the right of a volume scale set it to 100 on the press and a
button-up motion dragged it back to 0 before the release arrived, so
every click after the first read as 0 and the geometry looked wrong.
The fix is one mask test -- `uui_button` had consulted `buttons` all
along, for exactly this. `uui_slider` had the identical shape and was
fixed in the same change, unprompted by any failure: nothing drives a
settings slider that way, which is why it survived.


## A splitter owns a FRACTION, and the size it decides is pinned on the layout ITEM

`uui_splitter` could have owned a pixel column. It owns per mille of the
travel instead, and the app supplies the track on every layout pass.

The pixel version is simpler and wrong in a way that only shows up
later: a divider dragged in a 700-pixel window and then reopened
maximised leaves one side at the width it had when the window was small,
and the same value means something different after a font-size change or
on a different screen. Qt's `QSplitter` stores absolute sizes and then
spends `setStretchFactor` and a resize handler putting the proportion
back; a fraction is that behaviour with nothing to remember. It also
makes persistence trivial -- the number written to `/etc/files.conf` is
the number the widget holds.

**The drag is a DELTA from the press, never the pointer's absolute
position.** That is what lets one widget serve File Manager's
hand-computed rects and System Settings' `uui_layout` row: the layout
inserts a gap on each side of the band, so an absolute mapping from
pointer to fraction would be off by one gap in one of the two callers
and correct in the other -- the shape of bug that gets blamed on the
app. A delta is immune to every offset between the track's origin and
where the band actually ended up.

**The size goes on `uui_item.main_size`, not on the widget beside it.**
A divider's whole job is to own a size the widget next to it would
otherwise choose, and there were two ways to say that: a `set_width()`
on `uui_sidebar` -- then on `uui_tree`, then on `uui_listbox`, once per
widget type that ever sits beside a divider -- or one field on the thing
a container already consults. CSS calls it `flex-basis` and QSplitter
calls it `setSizes()`, and both put it on the CONTAINER's view of the
child for the same reason.

The cost, paid immediately: `struct uui_item` is initialised
POSITIONALLY by six apps, so the field had to go last, and adding it
mid-struct silently renumbered every one of them into setting the wrong
member. The compiler's only complaint was
`-Wmissing-field-initializers`, which is not the error that deserves.
Those arrays are designated-initialiser now, which is the actual fix --
the next field added to `uui_item` will not have to be careful.

## The resize cursors stopped being the frame's alone

`abi/win_proto.h` capped the client-facing cursor set at three shapes
and said why: "the resize cursors are the frame's, which a client does
not own". That was true while the only thing being resized was a window,
and it stopped being true the moment a client had a divider of its own
to drag.

Wayland made the same move in the other direction and is the check on
this one: `cursor-shape-v1` enumerates `ew-resize`, `ns-resize`,
`col-resize` and `row-resize` for clients precisely because a toolkit's
splitter has to name them, while the compositor still draws its own for
a window edge. `WIN_CURSOR_RESIZE_H`/`_RESIZE_V` are that, and
`resolve_cursor_kind()` keeps the frame's answer first, so hovering a
window edge shows the edge's cursor even if the client under it asked
for something else.

It cost two defines and one `switch` arm. The shapes had been on disk in
every cursor theme since themes existed (`data/cursors/*/resize-h`), and
the compositor was already resolving them for its own frame -- so what
was missing was a NAME a client could say, not a capability.

The testing half is worth recording because it decided the shape of the
check: `gui state --json` reports the RESOLVED shape now
(`cursor.shape`), so a test asks the compositor what it would draw
rather than trying to recognise a 15x21 sprite in a screenshot. A pixel
check would have been possible and would have proven less -- it could
not tell "the client asked for the wrong shape" from "the theme failed
to load".

## Properties is a process, not a dialog

Every other modal in the File Manager is drawn in its own window --
Rename, New folder, the delete confirmation -- so Properties being a
separate program is the odd one out and needs a reason.

There are three, and the first is the real one. **A folder's size is a
recursive walk**, and an event loop that is walking a directory tree is
an event loop that is not answering. The alternatives were a walk
spread across the File Manager's own tick (which makes every listing
redraw compete with a background job the user cannot see or stop) or a
modal that blocks until it finishes (which is the freeze, with a
different name). A child process has its own tick and its own event
loop, and closing its window ends the walk.

Second, **a Properties window you can leave open beside the listing is
more useful than one that blocks it** -- which is why Explorer's and
Dolphin's are both real windows, and why comparing two files means
opening two of them (so it is deliberately NOT single-instance).

Third, **anything that can name a path can open one**. The File Manager
is the first caller; the desktop's own icons are the obvious second, and
they get it for the cost of a `sys_spawn`.

What it gives up is the toolkit: a separate binary cannot share the
File Manager's selection, so it takes a path and stats it itself, and a
file deleted while its Properties window is open shows what was true
when it opened. That is the same staleness every real properties dialog
has and it is not worth a watch.

**The queue is fixed and a full one is reported.** A walk that cannot
reach everything says "at least 4.2 MB" rather than printing the partial
total as if it were the answer -- the same rule this codebase applies to
a formatter that will not fit and a parser that cannot be sure. A floor
presented as a total is a wrong answer wearing a right answer's clothes.


## The clipboard is a copy a SERVICE holds, not a promise from the source

X11 selections and Wayland's `wl_data_source` both work the same way:
the source application keeps the data and serves it on demand when
somebody pastes. That is why closing the app you copied from loses your
clipboard, and why every desktop ships a clipboard manager whose entire
job is to paste-and-re-copy in the background to work around it.

toy-os copies instead. A copy hands over the bytes and they stay handed
over; the source may exit immediately and the paste still works.

The trade is real and it is the right way round at this scale. Serving
on demand exists because X11 clipboards can hold a bitmap and nobody
wants a copy of it in the server, plus it lets the source offer several
TYPES and the paster pick one. Here the payload is a list of paths or a
run of text -- 64 KiB at the cap -- so the copy is cheaper than the
machinery for avoiding it, and the type negotiation would be a protocol
for two types the source already knows the answer to.

**It is a RING-3 SERVICE, and it took two goes to get there.** It began
as a buffer inside `kernel/proc/win_server.c` reached by `SYS_WIN_CLIP`,
for a reason that was real: the compositor is a process this desktop
kills on purpose (`compositor_death_test.py` exists), so a clipboard the
compositor owned would be emptied by a Force Quit. The mistake was
concluding that the only alternative to the compositor was the kernel.
It put untrusted user data and a piece of desktop POLICY -- what a
clipboard is, what a cut means, how large one may be -- in ring 0, which
is the one thing CLAUDE.md says ring 0 must not contain, and it only
ever grows: a history ring or an image format would have put megabytes
there.

Every system that has thought about this puts the clipboard in
userspace. macOS has `pboard`, a daemon. Android's `ClipboardManager`
lives in `system_server`. Windows is the outlier that copies into
system-owned memory, and that design sat in `win32k.sys` in kernel mode
and has been moving out ever since. The lifetime argument is answered by
SUPERVISION rather than by privilege: `/bin/clipboardd` is an init
service, so the clipboard outlives every app that copied into it and
outlives the compositor -- which was the case that mattered.

**The service is not in the data path, and that is the part worth
copying.** It creates a named shared-memory page and owns its lifetime;
clients read and write the page themselves (`userland/lib/uclip.c`), so
a paste costs no syscall and no context switch, and an app greying out
its Paste item watches a serial for free. That is cheaper than the
syscall it replaced, not a tax paid for the architecture. It also
deleted the broadcast: `WIN_EV_CLIPBOARD` existed to tell clients the
clipboard changed, and with the state in shared memory they can simply
look.

What the daemon does do is own the lifetime and break a lock whose
holder died. It does NOT survive its own restart -- a fresh page is
zeroed -- which is the same thing an X server dying does, and nothing
has asked for more.

**Readers use a seqlock; writers take a lock.** A seqlock alone assumes
one writer and any app may copy, so there are two mechanisms and they do
different jobs: the sequence number lets a reader detect that it read
across an update and retry, with no syscall and no blocking, and a
compare-exchange word keeps two copiers from interleaving. A writer that
cannot take the lock REFUSES rather than forcing it, because forcing it
would race a live holder; the daemon breaks a lock older than a couple
of seconds, since the only way one stays taken that long is an owner
that died inside it -- and if the sequence was left odd, the clipboard
is emptied rather than left as half of something.

**A refusal, never a truncation.** A copy larger than the cap puts
nothing on the clipboard. Half a cut set pasted is files silently left
behind in the source directory, and half a pasted paragraph is worse
than none -- the same failure mode this codebase's formatter and parser
rules exist to prevent.

**Two kinds, declared and never sniffed.** `kind` is FILES or TEXT, the
copier states it, and the paster asks. Sniffing -- guessing from the
bytes whether they look like paths -- is the version of this that
pastes `/etc/hostname` into a document as a line of text. FILES is 0 so
that a zeroed page means what the file-only clipboard meant.

## One Markdown parser, two renderers

`/bin/doc` renders this repository's own `docs/commands/*.md` to a
terminal, and Notepad now renders them to a window. Those are two very
different jobs -- one wraps at a column count and emits SGR, the other
wraps at a pixel width and picks font faces -- and the obvious way to
get the second was to write a small Markdown parser inside the widget.

That would have been two answers to "what does `**` mean here", in
documents nobody would think to open both ways. So `lib/umd.h` gained
two entry points instead: `umd_classify()` says what KIND a line is with
its marker already removed, and `umd_inline_walk()` walks `code`,
**bold**, `[text](link)`, the one backslash escape and the
transliteration, reporting styled CHARACTERS. The text renderer became
its first caller and the widget its second; `tools/umd_hostcheck.py`
renders all 110 pages at three widths and came out byte-identical across
the refactor, which is what made it safe to do at all.

**What deliberately did NOT move is the line breaking.** Where a line
breaks is a property of the renderer's medium -- umd's is columns and
the widget's is pixels -- so each keeps its own, and what they share is
only the parsing. Putting the wrap in umd would have meant a medium
parameter threaded through a function that has no business knowing about
fonts.

**The widget scrolls in pixels, and `utext` scrolls in lines.** That is
not an inconsistency: `utext` is a grid where every row is one character
cell, and a document is not -- a heading, a rule and a code block are
three different heights, so there is no line unit to count. It also
means one walk has to both measure and draw, since two walks would be
two ideas of how tall a heading is and the scrollbar would disagree with
the page.

## A preview that turns itself on, and can be turned off

Notepad opens a `.md` rendered and everything else as text. Neither half
of that is free: a preview that had to be asked for every time is a
feature nobody finds, and one that cannot be turned off is an editor
that cannot edit its own documents. Ctrl-E toggles, which is Obsidian's
binding for the same thing -- VS Code's Ctrl+Shift+V was rejected
because in an editor that combination conventionally means "paste as
plain text", and this Notepad has a clipboard now.

The preview is READ-ONLY. A keystroke that edited the buffer underneath
a rendered document would change what is on screen with no caret to say
where, so typing does nothing until Ctrl-E comes back to the source.
Word wrap is greyed out while it is up for the same reason: a document
reflows to the window whatever the editor's own setting is.

## Ctrl+C is not the window manager's to route

The roadmap said the standard keybindings should be "routed through the
WM". They are not, and the reason is one key: `Ctrl+C` is INTR in a
terminal. A compositor that intercepted it globally would take
interrupt away from the GUI Terminal, which is a real regression traded
for a convenience.

So the clipboard keys are ordinary keys that the focused client
interprets: the File Manager reads `0x03`/`0x18`/`0x16` as copy, cut and
paste, and the Terminal keeps `0x03` as INTR. Windows takes the same
view -- copy/paste keys are per-application there too, and Windows
Terminal specifically resolves the collision itself by copying when
there is a selection and interrupting when there is not, which is a
choice only the terminal can make.

The cost of not routing them centrally: every app that wants clipboard
keys binds them itself, and two apps could disagree about what Ctrl+C
means. That is what the Terminal needs in order to differ, so it is the
feature and not the bug.


## File operations moved into the process, and what that cost

The File Manager spawned `/bin/cp` and `/bin/rm` and reaped them on its
tick. That bought three things, and it is worth being precise about
which of them survived the change:

- **One implementation of copying.** KEPT, by a different route: the
  copy loop and the tree walk moved to `userland/lib/ufileop.h`, and the
  three shell programs became front ends over it. `tools/fileop_test.py`
  is what makes that claim checkable from a prompt.
- **A failed copy cannot take the window down.** LOST. A bug in the copy
  loop now faults the File Manager instead of one child process. What
  replaces it is that the same code runs as `/bin/cp` under a test, and
  that the loop is small and does one thing.
- **No byte-level progress**, which was the stated cost of spawning.
  GONE, and that is the gain: a child reports an exit code, so a
  progress bar and a cancel were impossible. So was asking anything --
  a spawned `cp` has no way to say "this file exists, what now?".

**How much to share is the interesting part.** Windows shares the
PRIMITIVE and not the policy: `CopyFileEx` in kernel32 is common, and
Explorer's `IFileOperation`, `robocopy` and `cmd`'s `copy` are three
separate engines above it. Linux shares nothing -- coreutils' `copy.c`,
GIO's `g_file_copy` and KIO's file worker are three unrelated
implementations of copying a file, present on one machine at once.

This shares more than Windows does: the primitive AND the tree walk,
with policy as callbacks. The reason is that Windows' split is paid for
by compatibility -- `robocopy` cannot change what `copy` does -- and
there is no such constraint here, so a second walk would be pure cost.
Linux's three are not a design at all; they are what happens when three
projects each need a copy and none can depend on the others.

**The state is the caller's**, which is `ttf.h`'s arrangement and for
`tfs3.c`'s reason: a GUI runs this on a worker thread while its main
thread lists directories, and file-scope scratch shared between them is
the re-entrancy bug this codebase keeps rediscovering. `struct ufileop`
is ~30 KB, so it lives at file scope in each caller, never on a frame.

**A cancelled copy deletes its partial file.** Half a file under the
right name is worse than no file, because nothing downstream can tell
the difference -- and the next thing to read it would get a truncated
document with no indication anything went wrong.

## A conflict is a question the worker asks and the event loop answers

Copying happens on a worker thread (`userland/fm/fm_jobs.c`), and the
thread that draws is the one that must answer -- so "this file exists,
what now?" crosses between them twice: the worker posts the question and
BLOCKS, the main loop opens `uui_dialog`, and the button's code goes back
to the worker as a decision.

**Every desktop does the copy off its UI thread and blocks the worker on
the answer.** Explorer's `IFileOperation` raises a modal and its copy
engine waits; Nautilus (GIO) does the same through `g_file_copy`'s
`GFileProgressCallback`; KIO's file worker is a separate PROCESS and
still blocks on a reply from the job's UI. The alternative -- deciding
without asking -- is what `/bin/cp` does, and it is why the GUI could not
use it.

**The handshake is two ints and a sleep, not a condition variable.**
`pthread_cond_wait` in `userland/libc/pthread.c` spins on `sys_yield()`,
so a worker parked on one burns a core for as long as the person takes
to read the dialog. The worker polls `sys_sleep_ms(30)` instead. That is
the wrong primitive in general and the right one here; when the
condition variable learns to block, this is one of the call sites to
revisit.

**Apply-to-all is per DECISION, not global.** Overwrite-all and skip-all
are different answers to the same question, so one sticky flag holding
"the last thing you said" is what every one of the systems above stores,
and `g_apply_all` is that.

**A cancel is a flag the worker reads without the lock**, because it
only ever goes 0 -> 1 and a torn read of that has no wrong value. The
lock covers the progress block, which the worker writes and the draw
reads.

## Why the conflict is a dialog when Properties is a process

`Properties is a process, not a dialog` argues for a separate window
wherever the thing being shown is a view of its own. The conflict
question is the opposite case, and the line between them is worth
stating because both are "a small box with buttons".

**A dialog is right when the answer BLOCKS work that is already
running.** The copy is mid-file with a worker parked on the reply, so
the question has to be in the window whose operation it belongs to, on
top, taking every key, and gone the moment it is answered. A process
would give it a taskbar button, a focus of its own and a lifetime the
copy does not control -- and a person could close it, leaving a thread
waiting for a window that no longer exists.

**A process is right when the thing shown OUTLIVES the action that
opened it.** Properties can stay open beside the file manager, be
compared with another Properties, and survive the app being closed.

Windows draws the same line: `IFileOperation`'s conflict sheet is a
modal owned by the copy, while the properties sheet is a shell window.
KDE differs and pays for it -- KIO's job dialogs are separate windows,
which is why a stalled copy in Dolphin can leave a dialog with no
obvious parent.

**Hence `uui_dialog` is a WIDGET, not a window.** It occupies no space
in a layout (`natural_size` is zero), draws through `draw_overlay` and
declares `overlay_active`, so an app gets a modal by adding one entry to
its widget list and nothing else -- the same shape `uui_menubar`'s popup
already had, generalised because the File Manager was about to grow a
second one-off box.

## The `gui` diagnostics reach ring 3 by POLLING, because a syscall may not wait in place

`/bin/guictl` exists so a machine with no serial console attached -- the
bare-metal laptop -- can be asked what its desktop is doing. The
channel it uses was already there: `SYS_WIN_DEBUG` is ring-3 reachable
(it was added so a ring-3 compositor could ANSWER these), and
`win_server_debug()` gates only `WIN_REQ_DEBUG_TAKE` and
`WIN_REQ_DEBUG_REPLY` by pid. Asking was never gated, because there was
only ever one asker.

**What did NOT work is the obvious thing: letting a process take the
path the console takes.** `debug_via_compositor()` posts the command and
then waits with `sti; hlt` until the compositor answers. From the serial
debug console that is correct and remains so -- the console is not a
scheduled process, so there is no trapframe to corrupt and nothing to
switch away from. From a syscall it is a **#GP inside `isr_common`**,
which is what the first run of `guictl state` produced;
`api/scheduler.h` had already written the rule down ("a blocking syscall
MUST go through this rather than waiting in place with interrupts on --
that was tried, and hangs after one event because `g_next_kernel_rsp`
isn't reentrant").

Three ways out, and why this one:

- **Block on a wait channel.** Correct, and what `SYS_SLEEP` and the
  socket receive already do. It needs the syscall handler to own the
  wait -- `scheduler_block_current_until()` takes the handler's own
  trapframe -- so `debug_via_compositor()` would have to be split and
  `sys_win_debug()` restructured around a retry. That is the right
  shape for something on a hot path. This is a diagnostic issued by
  hand.
- **Poll from ring 3.** The kernel posts and returns
  `WIN_DEBUG_F_PENDING`; the program sleeps 2 ms and asks again. No
  kernel wait at all, so nothing can be got wrong about reentrancy, and
  the cost is a syscall every 2 ms for the few milliseconds a `gui
  state` takes. Chosen.
- **Refuse ring-3 callers.** Which is where things already were.

**The one-slot channel is refused rather than shared.** `g_dbg_reply`
and its chunk cursor are single, and the comment above them said so:
"one slot: `gui` commands are issued one at a time by a console that
blocks on each, so a queue would be state with no second user." There
is a second user now, so a command arriving mid-drain gets `-EBUSY` --
the same reject-rather-than-guess call `fs_read()` makes for a nested
whole-file read. The claim is per-pid and expires after three seconds,
because a client killed mid-drain would otherwise hold the channel until
reboot, and a diagnostic nobody can run is worse than one that can be
raced.

**And the bug this surfaced, which predates all of it.**
`win_server_debug()` clears `msg->flags` at entry -- right for every
request but `WIN_REQ_DEBUG_REPLY`, where `flags` is an INPUT: the
compositor saying what its answer is. The clear ran first, so the reply
handler read zero. `WIN_DEBUG_F_UNKNOWN` was discarded, which is why
`gui nosuchthing` printed nothing on the serial console from the day the
desktop became a ring-3 process -- an unrecognised command was
indistinguishable from one that ran and had nothing to say, which is the
exact distinction that flag exists to make. `WIN_DEBUG_F_MORE` went with
it, so `g_dbg_reply_ready` was set on the first chunk of a multi-chunk
reply; that has never produced a wrong answer only because the
compositor sends its chunks back to back without yielding, which is a
race not yet lost rather than a race that is not there.

## The winshare KTESTs skip while a compositor holds the role, and the harness frees it

These tests need the compositor role, and the role is ONE GLOBAL that
the ring-3 desktop really holds on a `graphical` boot -- which is the
default target, so it is held during every ordinary `make test`. Taking
it is not a read: `win_server_set_compositor()` revokes the outgoing
compositor's framebuffer grant, drops every mapping it holds and
disarms the hardware cursor, and giving it up asks that compositor's
clients to close. The suite was killing the desktop it was running
under, and the failure surfaced as a DIFFERENT assertion each run --
whichever test the dying desktop happened to be racing.

**Restoring the role afterwards does not work**, which is the first
thing anyone proposes. The damage is done on the way IN: by the time a
test holds the role, the grant is already revoked and the mappings are
already gone. There is nothing left to put back.

**A safe pid does not exist either.** Every number below `SCHED_PID_MAX`
is one a real process can hold -- and pid 3, which the fixture used to hardcode, is exactly what
`toywm` gets on an ordinary boot. So the fixture was also creating and
destroying windows on the live desktop's own list. The pids are chosen
at run time from what the scheduler says is free.

**What was NOT done, and why.** The structural fix is to give
`win_server.c`'s globals a second instance so the tests never touch the
live role at all. That is the right shape and it restructures a
subsystem the desktop depends on, to fix a defect that only affects the
test suite -- a large blast radius for a small problem. Skipping alone
was also rejected: the default target is graphical, so the coverage
would be gone on essentially every run, including the gate.

So it is both halves. The tests refuse to run while anyone holds the
role, which makes a hand-run `make test` safe instead of destructive;
and `tools/ktest_run.py` frees the role first with `service stop
toywm`, so the gate still exercises them. `service stop` rather than
deleting the descriptor because init keeps `admin_stopped` in memory
and the request file is in `/run`, which init empties at startup --
nothing survives to the next boot,
so the next tool inherits no fixture.

**The harness asserts that the precondition actually held.** A suite
that skipped the tests the harness went out of its way to enable is not
a pass, and `ktest_run.py` fails the run when the skip reason appears
in the transcript. That check earned itself immediately: the first
version sent the stop as soon as the debug console was up, which is
BEFORE init has read `/etc/services.d`, so init answered `supervises no
service called toywm` and the desktop started anyway. It won the race
about half the time. The wait is on init's own readiness line now.

## The network tray item shows an icon and hides the address behind it

The desktop had no network surface at all -- no app, no Settings page --
so the tray item is the only place network state is visible (2026-09-12).

**Why an icon plus a panel, and not the address in the strip.** Windows
11, GNOME Shell and Plasma all put a state-shaped icon in the tray and
keep the address one click away (Plasma's Details tab, macOS's
Option-click); none of them spend panel width on an IP. The taskbar
wants that width, and the address is not the thing that changes -- the
state is. An always-visible IP is the conky/ops shape, not a desktop's.

**Why it writes nothing.** `sys_net_config()` exists, so a Renew or an
enable/disable is reachable in principle -- but the lease lives in
`/bin/netd` and there is no control path into it, so "renew" would
either be a lie or a second project. A button that half works is worse
than no button.

**"Connected" is decided by the ADDRESS, never by the link flag.**
`link_known` is three-valued in `abi/query_abi.h`: 0 means the driver
has no way to ask, which is NOT "down". The e1000 in a default QEMU
guest is exactly that case, so reading the link flag would paint a
disconnected icon on the machine this is most often looked at. An
address is evidence; a flag that may not exist is not.

**It does not say "DHCP".** `/bin/netd` holds `enum udhcp_state` in its
own process memory and publishes none of it, and a lease file on disk
records a REMEMBERED address with no expiry -- present after a release.
A static address set by `ifconfig` is also indistinguishable from a
leased one at the query layer. So the panel distinguishes only what the
ABI supports: an address, a self-assigned 169.254/16 one, or none.
Naming the source would need netd to publish its state, which is the
honest way to add it later.

**Two clocks, deliberately.** The device is read once a second, which
is the one poll in `wm.c` that is not a generation compare: there is no
netdev generation in the ABI, and the read is a `sys_query_record()`
memcpy out of a live kernel table with no I/O behind it -- not what the
poll convention was defending against (a whole-file disk read for the
desktop's entries). The item's VISIBILITY is on the settings
generation, because `tray_want_shown()` does read a file.

**State is shape, not colour.** A tray icon is blitted TINTED to the
panel's own ink (`wm_tray.c`), so "red for disconnected" is not
available at all -- there is one icon file per state, and the glyph is
a three-node graph rather than a globe because a globe is Windows'
"connected but no internet", a claim nothing here can check.

## Which tray items are shown is a setting, and `auto` asks the hardware

`userland/wm/brightness_popup.c` used to put a sun in the tray on every
machine, including a QEMU guest whose adapter has no backlight at all,
and the flyout explained itself with the registry's `unavailable`
sentence over a disabled slider. Windows 11 shows the brightness slider
only on a machine with one, and Plasma's Brightness and Colour applet
hides itself the same way.

**Why it was shown anyway, and why that stopped being the answer.** Two
reasons held it. The tree already answers "the hardware is missing" one
way -- the setting is registered regardless and reports a sentence, the
volume item stays with no sound card, and `setting_abi.h` asks every
client to show the sentence rather than silently disable the control --
so a brightness item that vanished would be the one control answering
differently. The second reason was the deciding one and is the one that
was actually load-bearing: **a hidden item cannot be tested.** Every GUI
tool here runs under QEMU, where the backlight never exists, so hiding
would leave the flyout's drawing, geometry, dismissal and mutual
exclusion exercised by nobody.

**What replaced it.** `desktop.tray_<item>`, three-valued: `auto` |
`always` | `never`, defaulting to `auto`, which consults the hardware.
That is Windows 11's Taskbar > System tray icons and Plasma's per-item
panel control, and it dissolves both objections rather than trading one
for the other. The consistency objection goes because the answer is now
a POLICY every tray item resolves the same way, not one control
disagreeing with its neighbours -- `tray_want_shown()` is one function
and a third item needs no new code. The testing objection goes because
`always` outranks the hardware probe: `brightness_test.py` pins it,
keeps every check it had, and adds four that assert the absence under
`auto` and `never`. A user who wants a control pinned regardless
outranks a hardware probe, and so does one who never wants to see it;
that is why three values and not a checkbox.

**Hiding KEEPS THE SLOT.** The obvious implementation is
`tray_unregister()`, and it is wrong: a slot index IS the item's
left-to-right position in the strip, so an item that came back landed
in whatever slot was free and silently reordered its neighbours.
`tray_set_hidden()` leaves the slot allocated and `tray_walk()` skips
it -- which also means the visibility may change at runtime, as it must,
since the setting can move and so can the answer to "is there a
backlight".

**Resolved on the SETTINGS GENERATION, never per frame.**
`tray_want_shown()` is a `sys_setting()` GET whose kernel getter reads
`/etc/desktop.conf`, so calling it every frame is a whole-file disk read
at frame rate -- the exact cost the WM's poll convention exists to avoid.
It hangs off `tray_slider_poll()`'s existing generation compare. The
known gap: a backlight appearing without any setting write would not be
noticed until the next generation bump.

## The brightness floor is 5%, not 0

A backlight duty of zero turns the
panel off. Mute is a volume of zero and comes back with a click; a
persisted brightness of zero comes back on the NEXT BOOT as a black
screen the user cannot see to fix, on a machine whose only recovery is
`config set` typed blind or a reboot that does not help. So the
registry refuses anything under 5, the flyout clamps to the registry's
range, and "screen off" is a separate, unpersisted action that the
roadmap lists.

## The page flip is a buffer-age protocol, and virtio-gpu got it so the suite could see it

A display that can point its scanout at a second buffer gives a
tear-free desktop for one register write, and the Intel driver can.
What stood in the way was the compositor: `ugfx_screen_present()`
copies only each frame's damage box into the scanout, so a second
scanout would show pixels two frames old outside that box. Three
shapes were on the table.

**Hide it in the kernel** by copying every damage rect into both
buffers at present time -- an extra ring-0 copy per frame, from
write-combined memory, which is exactly the cost the flip was meant to
remove. **Remap `WIN_FB_VADDR`** to whichever buffer is back on each
flip -- thousands of PTE writes and a TLB shootdown per frame, the
trick `win_proto.h` already refuses for window buffers. Or **tell the
compositor**, which is what Wayland does: `wl_buffer` plus
`buffer_age` says how many frames old the buffer it is about to draw
into is, and the client repaints the union of that many frames' damage.
That is the one built. `WIN_REQ_FB_MAP` maps every scanout and returns
the back index; `WIN_REQ_FB_PRESENT` flips and returns the new one; the
compositor keeps the previous frame's damage and copies the union. The
QEMU adapters with one buffer report index 0 forever, so nothing they
run changed.

**Three buffers and no wait, after two buffers and a wait tore worse
than nothing.** A flip lands at the next vblank, up to 16 ms later,
and the compositor must not draw into a buffer still being scanned.
The first version had two scanouts and waited for the PREVIOUS flip to
land at the start of the next present -- which guards the register
write and not the drawing: the compositor had already drawn the frame
into the buffer it was handed, and with two buffers that buffer is the
live one until the pending flip lands. Window drags tore visibly more
than before the flip existed, and the maintainer said so within the
hour. Waiting after each flip instead would stall the machine with
interrupts off for up to a frame per present (a present is a syscall,
and this laptop hangs outright on a wait there). The answer every
compositor without a vblank event reaches is triple buffering in
mailbox mode -- Windows' DWM, and Mutter's triple-buffering option:
the flip is a register write that never waits, a second write before
the first lands replaces it, and the buffer handed back is the one that
is neither live nor just asked for, which a third buffer always
provides. A frame drawn faster than vsync is dropped rather than shown,
which is what a desktop wants. The cost is a third framebuffer's worth
of contiguous frames (8 MiB at 1080p) and a buffer age of up to three
frames, which the damage ring covers.

**Why virtio-gpu grew a second resource too.** The Intel driver is
testable by eye on one machine. A protocol change to the path every
GUI tool drives cannot rest on that, and `SET_SCANOUT` to a second
resource is a real flip on a device every headless test can boot -- so
the suite exercises the buffer-age copy, the KTEST counts the commands
a flip costs, and `gui fb` reports flips happening. It is also the
second real caller the display capability needed to be a design rather
than one driver's convenience.

**What was measured.** On the laptop every present flips (`guictl fb`
reports flips equal to presents after boot). Under the virtio GPU the
pixel-restoring tools (calendar, volume, brightness, menubar, notepad)
pass on the three-scanout path, which is the check that catches a
wrong buffer age: a popup closed over stale pixels fails their
before/after comparison. The KTESTs flip a scanout and read the live
one back, and skip while a compositor holds the grant -- flipping
under a live desktop is exactly the thing the protocol forbids.

## A mode change is a kernel setting with one ordered function behind it, not a compositor request

Linux puts modesetting in the compositor: Mutter and KWin own KMS
through atomic commits, allocate their own framebuffers at the new
size and tell clients through `wl_output`. Windows lets any process ask
through `ChangeDisplaySettings`, the display driver switches, DWM
re-creates its surfaces and every window gets `WM_DISPLAYCHANGE`. toy-os
takes the Windows shape.

**Why kernel-driven.** The compositor here does not own the
framebuffer: the kernel allocates the scanouts, `gfx.c` draws the
console into the first, and the compositor is GRANTED a mapping. A
compositor-only request would leave the console and a text-mode boot
with no way to change mode, and would need the desktop to grow its own
settings UI for one control -- while the registry already gives a
setting a System Settings row, `config set`, persistence and an
`unavailable` sentence for free. So `system.resolution` is an ENUM
whose choices are the driver's mode list, its apply is
`screen_set_mode()`, and the compositor learns through `WIN_EV_SCREEN`
exactly as it learns about a font change. `font_config.c`'s discipline
is copied on purpose: ONE function changes the screen for real, so
there is one place that can forget a step.

**Why the grant keeps every address it ever had.** The compositor is a
process, preempted wherever it was -- possibly halfway through
`ugfx_screen_present()`'s blit. The mode change runs inside one syscall
with interrupts off, so nothing in ring 3 runs between the driver
freeing the old framebuffer (virtio-gpu really frees it) and the new
grant; but the blit RESUMES afterwards at the old geometry. Unmapping
the old extent would fault the desktop; leaving it mapped to freed
frames would scribble on whoever got them next. So `win_surface.c`
keeps a high-water mark and pads every slot past the real buffers with
one writable scratch frame, which is `comp_span`'s rule for a client
window applied to the screen. The stale stores land in the scratch
page, and the first frame after `WIN_EV_SCREEN` is unconditional.

**Why bochs now adopts GRUB's mode instead of declining.** The decision
below this one records bochs declining a boot it cannot improve, so
that vesafb takes the same pixels with less machinery. That was right
when a claim meant re-programming a live console for nothing; it is
wrong once claiming is what makes a later mode change possible at all,
since vesafb can never change one. bochs now claims by ADOPTING the
firmware's surface without a register write -- the same readout the
Intel driver does -- and the console is never blanked. The `-vga std`
adapter every headless test boots therefore has a modesetting driver,
which is what lets `modeset_test.py` run in the ordinary suite.

**What was measured.** On bochs, vmsvga and virtio-gpu a live
1280x720 to 1600x900 and back: a QMP screendump's own pixel size (the
device's answer) equals what `gui state` reports (the desktop's
belief) at every step, the Start menu paints at the new size, and a
maximized window fills the new screen with its pixels in the corner
outside the old mode. virtio-gpu keeps its three scanouts across the
change. Not built: a revert countdown for a mode the monitor cannot
show (Windows' fifteen seconds); every adapter this covers is an
emulator that shows any mode, and the Intel driver has no modeset yet.

## The scaling policy is a display-layer setting, not a driver detail

`system.scaling` (aspect, full, center) decides where a mode smaller
than a fixed panel lands. Linux exposes this per connector as the
`scaling mode` property, set by the compositor; Windows' Intel driver
puts three radio buttons in its own control panel. toy-os makes it a
registered setting beside Resolution, stored in the display layer
(`display_scaling()`), for the reason the resolution is one: the kernel
owns the framebuffer and the mode here, the registry gives a System
Settings row, `config set`, persistence and an `unavailable` sentence
for free, and a policy held by the layer is read by whichever driver's
`set_mode` runs -- at boot, before any compositor exists, the stored
resolution is placed the way the user chose. The alternative, a
per-driver knob, would have needed a third path into the Intel driver
and would have been invisible on every other machine; the registry's
sentence on a display without a scaler is the better answer to "why is
this greyed out". The window maths is a pure function so it is KTESTed
on every machine, and the arming order of the fitter's registers is the
driver's trap, recorded in `docs/conventions/gui.md`.

## Motion is coalesced in the kernel queue, and a notification is never the event shed

The compositor's event queue is 32 deep and drops the oldest on
overflow, which is the right policy for input: a client that has fallen
behind wants the current state, not the backlog. It was the wrong policy
for `WIN_EV_SCREEN`, which is not input but a fact -- the screen changed,
re-map the grant -- that the receiver has no other way to learn. On the
laptop a real mouse moving through one slow frame pushed 33 events
behind it and the notification was the one to go; the kernel scanned the
new mode while the desktop painted the old one.

The obvious fix, a deeper queue, only moves the cliff: a 125 Hz mouse
fills any depth in a long enough frame. The two rules that hold are the
ones real windowing systems settled on. **Motion is a state**: Windows
posts a single `WM_MOUSEMOVE` per thread queue and replaces it, and X
compresses `MotionNotify` in the server, so the kernel merges a move
into the newest queued move (same buttons, same window) and the queue
holds at most one. The compositor was already doing exactly this on its
side ("newest position wins"), so nothing observable changed except that
the flood no longer reaches the queue. **A notification outranks
input**: Wayland never drops an event at all -- the socket fills and the
client stalls -- and toy-os deliberately keeps dropping, because a
stalled input path is the worse failure here; but what it sheds is the
oldest INPUT event, raw or delivered, and only with none queued the
oldest of all. Both are in `win_events_push()` with a KTEST each, and
the positive control (both rules off) reddens exactly those two.

Deliberately NOT done: blocking the producer, as Wayland does. The
producers are the mouse IRQ and a client's syscall, and neither can
wait on a compositor.

## The layout log is written by the widgets, in one vocabulary, not by each app

**Decision.** A widget reports its own test-facing geometry: `bounds`
for its rect and a `describe` op for the sub-rects a driver needs (a
menu's titles and popup rows, a tab strip's slots, the picture inside an
image box), emitted through `ui/uui_describe.h` as
`<prefix>: layout <name>[.<part>] [i [j]] x y w h`. The app names each
widget (`uui_item.name`) and calls `uapp_log_layout()` once; a widget it
owns outside its arrays goes through `uapp_log_widget()`; only what the
app draws by hand is logged by the app.

**Why not the obvious way.** The obvious way had already been built:
`uapp_log_layout()` walked the router and printed `layout <id-number>
x y w h`, and seven apps still carried a logger of their own -- about
sixty lines -- because a numeric id is not what a test greps for, and a
bounds rect cannot say where a popup row is. Each app then spelled the
same menu geometry in its own words (`menutitle` in Minesweeper, `title`
in Image Viewer, `layout item` in Terminal), so a test helper written
for one app read nothing from the next, and a widget that gained a
sub-rect had to be re-logged in every app that used it.

This is the shape every desktop toolkit reached for the same audience:
Qt's `QAccessible` and GTK's AT-SPI expose a widget's geometry and
role BY NAME to a driver outside the process, and the widget answers
for itself. Copied for the shape only -- there is no role taxonomy and
no event stream here, just the rects a tool clicks.

**Two constraints that fixed the details.** The widgets are linked into
the compositor too, which has no `uapp`, so `describe` emits through a
callback in `struct uui_describe` rather than logging directly -- the
same call is what puts every line through uapp's `desktop.layout_log`
gate and per-frame dedupe. And `uui_item.main_size` was documented as
LAST because apps initialise the struct positionally, so `.name` sits
after it; a positional initialiser leaves it NULL, and an unnamed item
is simply absent from the log.

**What the second round found.** Files, Task Manager, Settings and UI
Demo followed the same day, with `describe` ops on the table, tree,
splitter and listbox -- and the walk reported nothing for half their
widgets, because eight ops tables had `set_geometry` and no `bounds`:
a widget a layout could PLACE but not REPORT. `check_widget_ops.py`
refuses that pairing now. The walk also reports a widget once even when
it is reachable through both `.layout` and `.widgets`. What stays the
app's is what no widget owns: a button inside a group, the File
Manager's selection and marks, a board. Disk Mark's tiles stay the app's -- one bounds line has no
slot for a profile index -- and go through the gate now, which is what
exposed its tool writing the gate setting through a second console on
the same socket. Minesweeper keeps hand-routing its menu bar -- its board
clicks live in `on_press`, and the router's press does not tell an app
whether a click dismissed a popup -- so it reports the bar through
`uapp_log_widget()` instead of joining the walk.


## Marked rows wear the selection colour, and the cursor row is the focus ring

The File Manager's marks (Insert/Space, a rubber band, Ctrl-click) were
drawn in a warm yellow, chosen so a mark and the selection could never
be confused: the selection is where the KEYS are, a mark is what an
operation will act on, and one colour for both seemed to hide the
first. Explorer, Dolphin, Nautilus and the Finder all disagree, and
they are right: a multi-selection is ONE selection with several rows,
and a second highlight colour makes the user learn a distinction the
operations do not make -- F5 copies the marked set OR the cursor row,
never both. So `uui_fileview.mark_bg` is the table's `sel_bg` now.

What the yellow was doing -- saying which row the keys are on -- is the
FOCUS RING's job, and the manager sets `table.focused` on its active
pane so the cursor row carries one. The inactive pane's selection has no
ring, which is also what every desktop does. The one thing a second
colour could still do, and this deliberately does not, is show marks in
a pane that is not active more loudly than its stale cursor; the
outline on the active pane already answers "which pane".


## A resized window adopts its new size when the client PRESENTS, not when it acks

Resizing a client window flashed: the whole content went black for
100-240 ms under TCG, measured on Image Viewer by sampling raw frames
through a grip drag. The cause was in two halves that agreed with each
other. `resize_window()` allocated new frames for BOTH of a window's
buffers and zeroed them, and `on_window_resized()` in the WM adopted the
new geometry the moment the client acked -- so the compositor drew
chrome around a buffer that was, by construction, empty, until the
client got a repaint out.

The obvious repairs both fall short. Copying the old pixels into the new
buffer (Windows' `WVR_VALIDRECTS` BitBlt, which is exactly what USER32
does to fill this gap) removes the black but shows content in the wrong
place for a frame, and content that MOVED -- a centred image -- lands
visibly offset. Snapshotting the composited window and stretching it
until the client catches up (KWin, DWM) is more code and a scratch
buffer, and it papers over the timing rather than removing it.

Wayland does not have the gap at all, because a `wl_buffer` carries its
own dimensions and a surface adopts them at `commit`: the compositor
keeps showing the old buffer at the old size until a new one arrives.
toy-os had already copied the configure/ack handshake and stopped one
step short of that. So the size is on the buffer now
(`struct win_buf`), a resize rebuilds only the BACK one, and
`WIN_EV_CLIENT_PRESENT` carries the front buffer's w/h for the WM to
adopt. There is no intermediate state to see, and no frame is ever
composited that was not fully drawn at the size it is shown at.

Three consequences worth knowing. The buffer left at the old size is
rebuilt inside the NEXT present, which is the only moment its pixels
have stopped being needed and the client has not yet started on it; a
failure there defers the flip rather than handing the client a buffer it
would overrun. A single-buffered window (its second allocation failed)
has nowhere to hide the change and still flashes -- the same degradation
it already accepts for tearing. And peak memory is unchanged at three
buffers during a resize, because the old back buffer is freed before the
replacement for the front one is asked for.

The alternative that was NOT taken, and why: keeping the old FRONT
frames alive as a third allocation until the present would let the
window shrink and grow with no rebuild at present time at all, but it
raises peak contiguous memory to four buffers -- 32 MiB at 1080p, from
an allocator `abi/win_proto.h` already documents as the thing that
refuses a window when memory fragments.

## A client window resizes LIVE, and the pacing is one proposal in flight

The rubber-band outline is gone. It was there because a client's buffer
cannot be resized by the WM, so the frame could not follow the pointer
without showing content stuck in a corner -- but that reasoning was
about the FRAME being ahead of the pixels, which adopting on the present
fixes outright. Windows and KDE both resize live, users expect it, and
an outline is what X11 window managers did before compositing made
anything else affordable.

What live resize needs that release-only did not is pacing: a drag
produces a motion event per frame, and a proposal per motion would queue
sizes a slow client draws and throws away, each costing two contiguous
allocations and a zeroed buffer. So `resize_pump()` sends the next
proposal only when the client ACKS the last one -- the client sets the
rate, exactly as a Wayland compositor waits for `ack_configure`. The
drag records what it WANTS and the ack sends it, which is also why the
ask outlives the drag: a client still answering the second-to-last
proposal gets the final size when it comes back.

Two details. An unanswered proposal times out after half a second,
because ignoring `WIN_EV_RESIZE` is legal (`abi/win_proto.h`) and a
client that does must not take the grip with it. And the ask is keyed on
the window's (pid, id) rather than only its index into `windows[]`,
because closing a window compacts that array -- a bare index would then
name somebody else and resize the wrong app.


## The outline came back as a setting, and `auto` is the default

Live resize landed by deleting the rubber-band outline outright, on the
grounds that Windows and KDE both drag windows live and an outline is
what X11 window managers did before compositing. That was right about
the default and wrong about the choice: KDE could delete its
*"Display content in moving/resizing windows"* switch in Plasma 5
because a KWin client redraws in milliseconds, and toy-os's do not.
Image Viewer takes 200-300 ms to become a size it was asked for under
TCG -- it rescales a JPEG per proposed size -- while Terminal takes
20-30 ms. A window lagging a third of a second behind the pointer is
worse than an outline that does not lag at all, and no single default
is right for both.

So the outline is back, as `desktop.resize_mode`. What is new is the
third choice.

**`auto` measures instead of asking the user to.** Windows and KDE both
make the choice in advance, for every application at once, which is the
part that has aged badly: the answer differs per app and the user has
no way to know which of theirs is slow. The window manager does know --
it already times each resize proposal against the present that adopts
it, because that pairing is what paces the proposals. A window over 100
ms (Nielsen's threshold for a response that reads as instantaneous) is
outlined; one under it follows.

**It REMEMBERS, per window, rather than only reacting.** A reaction
alone is nearly useless: a slow client's first frame arrives after a
short drag has already ended, so the fallback would never fire on the
drags that provoke it. `struct window.resize_lag_ms` holds the last
measurement, `auto` reads it when a drag begins, and a window already
known to be slow is outlined from the first pixel. The mid-drag switch
is kept for the long human drag whose measurement arrives while the
button is still down. Both directions self-correct, because even an
outlined drag measures its one resize on release. Per WINDOW rather
than per app because identity is the kernel's and a lag is not: two
windows of one program can be very different sizes, and size is most of
what the number measures.

**MOVING GETS NO `auto`.** Windows covers dragging and sizing with one
switch, and that is a fair fit for a system where both go through the
same repaint. Here the WM owns a window's position and moves it with no
client in the loop, so a move cannot fall behind and an `auto` for it
would be a choice that never happens. `desktop.move_mode` is therefore
`live` | `outline` -- offered because an outline move is a thing some
people want, not because anything is slow.

## Pace an interactive resize on the PRESENT, and clear on any of them

The first version paced proposals on the ACK: send one, wait for the
client to accept it, send the next. It looked right and measured
nothing useful, because a Toykit client acks from inside its event
handler and draws afterwards -- so the ack times a syscall round trip
(a few milliseconds for anything at all) while the present times what
the user actually waits for. Every client looked fast, and `auto` never
fired. Pacing on the present is also what Wayland does: a compositor
waits for the commit, not for `ack_configure`.

Two traps came with it, and both wedged a drag rather than degrading
it. **A proposal for the size a window already is produces a present
that changes nothing** -- and the press that begins a drag is itself a
motion, with a zero delta, so that is the first thing a drag asks for.
**And a rule that waited for a size-CHANGING present then waited
forever**, with every later size in the drag queued behind an in-flight
slot nothing would clear. The answers are separate: never propose the
size a window already has, and let ANY present clear the slot while
only a size-changing one feeds the measurement. Sending the next
proposal a frame early is harmless; never sending it is a drag that
stops following after its first step.

## The on-screen keyboard is compositor code, not an app

`userland/wm/osk.c` is a `wm_overlay` row that calls
`wm_client_send_key()` on the focused window. The obvious alternative --
an ordinary ring-3 program in `userland/gui/` -- was considered and
deliberately not built, and the reason is worth recording because the
app version is the one that looks more correct.

The three real systems split the same way. Windows ships `osk.exe`, an
ordinary process that calls `SendInput` and sets `WS_EX_NOACTIVATE` so
clicking it cannot take focus. KDE runs maliit as a separate process
speaking Wayland's `input-method-v1`. GNOME's is part of gnome-shell,
drawn by the compositor. X11's `xvkbd` and `onboard` fake events through
XTEST, and "any client can synthesise input into any other" is precisely
the property `virtual-keyboard-v1` exists to remove.

An app here would need TWO new mechanisms before a single key could be
typed: a way to send a key to a DIFFERENT client, and a window type that
does not take focus when clicked. The first is the expensive one -- it
is a general input-injection protocol, and once any ring-3 program can
call it, a keylogger and a click-jacker are ordinary programs. Wayland
gates its equivalent behind a compositor-granted privilege for exactly
this reason, which means the honest ring-3 design here is "an injection
syscall plus a permission model", not "an injection syscall".

The overlay needs neither. It is not a window, so focus is not a
question that arises; and the keystroke never leaves the compositor, so
no new capability is exposed to ring 3 at all. That is a smaller change
AND a smaller attack surface, which is an unusual pairing and the reason
this was not a close call.

What it costs is that the compositor now contains a keyboard layout --
a second copy of `kernel/lib/keyboard_layout.c`'s `FALLBACK_US`, which
is a real duplication and is why the convention entry says the two must
agree character for character. The roadmap carries both follow-ups:
reading `/etc/kbs` so the caps match the configured layout, and moving
the panel to a ring-3 app once there is a virtual-keyboard protocol
worth having. Neither is needed for the thing this was built for, which
is a machine whose only working input device is a touchpad.

**The encoding is the part that had to be got right, not the drawing.**
A keycap does not send "the letter plus a modifier bit": Ctrl-C is the
control code `0x03` and Alt-B is ESC then `'b'`, because that is what
`kernel/drivers/input/keyboard.c` produces and what every consumer --
the line editor above all -- already expects. `api/keyboard.h` even
warns that an app cannot usefully test `KEY_MOD_CTRL` for a letter,
since the fold has already happened by the time the key arrives. An OSK
that passed the bit instead would draw correctly, respond to every
click, and leave Ctrl-C doing nothing in the Terminal -- which is most
of what a machine with no keyboard needs it for.

## A client's identity is its spawn path, asked of the kernel, not kept per window

Single instance and taskbar grouping both key on "what program is this
client?", and both fail silently when that is wrong -- a wrong merge
puts two programs on one taskbar button, and a wrong match makes an app
exit without ever drawing, because a single-instance app reads "your
twin is up" as "go away". So the answer has to be something a client
cannot influence, which rules out the `app_id` it declares about itself:
two apps choosing one string is indistinguishable from two copies of one
program, which MUST match.

The kernel is the only party that knows a process's spawn path, so for
one stage it also answered the question -- `WIN_REQ_ACTIVATE` scanned
the window table for a window whose identity matched the asking process
and told the compositor which one to raise. That worked, and it is why
`struct client_window` carried an `app_identity` at all. It is also the
last thing keeping presentation state in ring 0 that PRESENT does not
need, so stage 6a split the two halves apart:

- **The kernel answers the FACT**, `QUERY_PROCPATH` -- pid and spawn
  path, one record per live process, no window state anywhere in it.
- **The compositor answers the QUESTION.** It owns the window list, so
  it is the party that can; it interns each client's path at window
  create and matches on the interned int.

Both real systems anchor identity the same way and for the same reason:
Windows falls back to the executable behind an AppUserModelID, macOS to
the bundle, and Wayland's `app_id` is only dependable because a
compositor matches it against a `.desktop` FILE rather than trusting the
string. Deriving it from the `.desktop` entry the launcher used was
considered and declined here: it covers only apps started FROM the
launcher, and fails silently for anything spawned another way.

**The cost is one round trip, and it is bounded.** Activate is now the
only message on the compositor channel that waits for a reply
(`uchan_call()`), where it used to be a syscall. It happens ONCE, before
a single-instance app opens anything -- the per-frame traffic a
synchronous carriage would ruin is untouched. No channel, or no answer
inside the timeout, is read as "no twin": the asymmetry is deliberate,
since a false yes hides an app completely and a false no opens a window
the user can see and close.

## A frame carries its own size, so nothing has to remember to say so

A window's two buffers can be different sizes: a resize rebuilds only
the one the client is about to draw into, so the front keeps showing the
last finished frame at the size it was drawn at. That is Wayland's rule
and it is what stops a resize flashing a window of black.

It follows that the compositor must know how big the buffer it is about
to show actually is, or it walks the pixels with the wrong row stride --
which draws the window sheared one pixel per row, and leaves it that way
until some later resize happens to correct it.

For several stages the kernel held that size, one record per buffer, and
a present answered from the record. Keeping it true was the client's
job: `WIN_REQ_RESIZE` and `WIN_REQ_BUFFER` existed for nothing else. So
the correctness of every frame depended on a message being sent, and one
day it was not -- Toykit decided whether a buffer needed rebuilding by
comparing PAGE-ROUNDED byte counts, a one-pixel resize left the count
unchanged, and the update was skipped. The window sheared.

The fix was to compare dimensions. The **design** answer is that the
frame should carry its own geometry, which is what stage 6b did:
`WIN_REQ_PRESENT` names the buffer, its generation and its width and
height, and the compositor adopts what it is about to show. Both
messages that existed to maintain the second copy are retired, because
there is no second copy.

Three things this decides beyond the bug:

- **The generation belongs to the object's creator.** The client
  replaces the shm object; only it can say which replacement this is.
  The kernel was counting on its behalf, from a message.
- **The front index is the client's too.** It knows which buffer it just
  drew. The kernel flipped and answered, which made every present a
  round trip for a fact the caller already had.
- **The table had nothing else load-bearing left**, which is why 6b
  could delete it rather than shrink it.

The general form, and the reason this earns an entry: **a second record
of a fact, kept in step by remembering to send a message, is a record
that will go stale.** Prefer carrying the fact with the thing it
describes. The alternatives here were a validating handshake (more
messages, same failure mode) or having the compositor measure the object
itself -- which it cannot, since an shm object's page count is rounded
and does not name a width.

## A popup is a surface of its client, placed by the compositor, and a press outside the client's surfaces dismisses it

`abi/win_proto.h` (`WIN_REQ_POPUP`, `struct win_popup_pos`,
`WIN_EV_POPUP_DONE`), `userland/wm/wm_client.c` (`on_popup_created()`,
`place_popup()`, `wm_client_popup_route()`), `userland/ui/uui_popup.h`,
`userland/ui/uapp.c`, `userland/ui/uui_menubar.c`; landed 2026-09-09.
`tools/popup_test.py` is the check.

**What real systems do.** Under Wayland a menu is an `xdg_popup`: a
child surface with an `xdg_positioner` -- an anchor rect in the PARENT's
coordinates, a gravity, and flip/slide/resize constraint adjustments --
that the COMPOSITOR resolves against the work area, answering with a
`configure` that says where it landed. `xdg_popup.grab` gives the popup
keyboard focus and a pointer grab; a press outside dismisses the whole
chain with `popup_done`, and that press is consumed. wlroots and KWin
stack popups as children of the parent's scene node. On Windows a menu
is an owned `#32768` HWND kept above its owner, `TrackPopupMenu`
captures the mouse, and a click outside cancels via `WM_CANCELMODE` --
consumed. Neither is bounded by its parent window, neither has chrome or
a taskbar button, and in both the OWNER stays the active window.

toy-os follows that shape. Six calls, each with the obvious alternative
it declined:

1. **The compositor places the popup.** The client sends the anchor in
   its parent's content coordinates and the side it prefers; the reply
   says where the popup landed, in the same coordinates. The alternative
   was the client resolving placement against the screen, which needs
   the compositor to push the window's screen position on every move --
   X11's shape, a new event on the drag path, and a race between the
   move and the menu. A client here never learns its screen position,
   and `place_popup()` is the widget's own flip/slide/clamp against the
   work area instead of the window, so a menu keeps clear of the taskbar
   the widget cannot see.
2. **A popup is a row in `windows[]`, not a `wm_overlay`.** As a window
   with `popup` set it inherits the blit, the damage tracking, the ping,
   the dead-client sweep and the buffer mapping for nothing; the four
   `window_content_*()` accessors answer the whole rect, which is the one
   place "no chrome" lives, and the taskbar and the geometry file skip it.
   An overlay would have meant painting a client's pixels from WM-owned
   draw code and a full-screen repaint every frame while open (the cost
   the Start menu was converted away from). What it gives up: a popup is
   drawn below the taskbar and cannot cover it. WM popups already keep
   clear of it (`wm_popup_place()`), so nothing on this desktop does.
3. **Where a press lands decides three ways, and the middle one is the
   subtle one.** Inside one of the owner's popups: delivered there.
   Inside another surface OF THE SAME CLIENT (its window's content):
   delivered normally, and the compositor dismisses nothing -- the client
   decides, which is how sliding to another menu title switches menus
   and how `uui_menubar_press()`'s "anything else closes" keeps working
   unchanged. Anywhere else -- another window, the desktop, the taskbar,
   the parent's own title bar: every popup of that client is closed,
   each is told with `WIN_EV_POPUP_DONE`, and the press goes no further.
   That is wlroots' rule exactly (a `wlr_seat_pointer_send_button` that
   reaches a surface of the grabbing client returns a serial; one that
   reaches nobody ends the grab), and it was chosen over "deliver the
   dismissing press to the parent too", which makes a button behind a
   menu fire on the click that closed the menu. Motion follows the same
   split: while a grab is up, hover reaches only the owner's surfaces.
4. **The parent keeps focus; the popup gets the keys.** `wm_focus_index()`
   is the topmost TOPLEVEL, so the active title bar, the taskbar tint
   and Alt+F4 stay the parent's (Alt+F4 must not ask a menu to close),
   while `wm_key_target()` routes the keys to the client's topmost popup,
   as `xdg_popup.grab` does. No focus event is sent to the parent when a
   popup opens: it did not lose anything.
5. **In the toolkit, coordinates stay the parent's and only the drawing
   moves.** `ui/uui_popup.h` is the seam (GTK's `GdkPopup`): a provider
   uapp installs, a widget asks through, and a refusal means "draw it in
   the window". A `uui_menu_level` keeps its rect in the window's content
   coordinates -- the compositor's reply -- so hit-testing, `describe`
   and every test rect are untouched; `draw_level_at()` subtracts the
   level's origin when the target is the popup's own surface. uapp does
   the inverse on the way in: a pointer event arriving on a popup slot is
   moved by that slot's offset before any widget sees it. Two rules fell
   out of the first run. **A LEAVE CARRIES NO POSITION**: the
   compositor says "the pointer left you" as a move to (-1,-1) in the
   surface's coordinates, harmless on the toplevel and a REAL point one
   pixel above-left of a popup once translated -- the menu bar, in
   Notepad's case, so every submenu hover switched menus. It is dropped
   for a popup, as `wl_pointer.leave` has no coordinates. And **AN EVENT
   FOR A SLOT NOT IN USE IS STALE** -- the client closed the popup while
   the press was in flight -- and is dropped rather than read as the
   toplevel's with raw coordinates.
6. **A popup is not drawn until its first present.** The buffer opened
   at create holds whatever the client has drawn so far, which for one
   frame is nothing; Wayland maps a surface on its first commit for the
   same reason. `on_window_created()` still has the flash for toplevels
   and is left alone -- a window opening is a different moment from a
   menu dropping.

Two consequences worth stating. **`WIN_CLIENT_MAX` is 8 now and it sizes
the CLIENT's table only** -- the compositor grows its list on demand and
never enforced it; a toplevel plus a five-deep menu is six surfaces.
And **three widgets use the surface now** -- `uui_menubar`'s levels,
`uui_dropdown`'s list and `uui_toolbar`'s tooltip. Each keeps its
in-window fallback for a refusal. The tooltip is what made the grab a
flag rather than a property of being a popup; see the entry below.

## A popup's GRAB is a flag, because a tooltip is a popup that must not take the pointer

`WIN_REQ_POPUP` gave every popup an implicit grab: while one was up, a
press inside it went there, and a press anywhere else dismissed every
popup of that client and was CONSUMED. That is right for a menu and
wrong for the two callers the roadmap named next.

A tooltip is a popup. It wants everything a menu wants -- to leave its
window, to be placed against the work area, to sit above everything --
and none of what a grab does. A tooltip that grabbed would swallow the
click meant for the button it is describing, so a button would go dead
for exactly as long as its own tip was showing.

Wayland separates at this line, which is what makes the split cheap to
copy: `xdg_surface.get_popup` creates the surface and `xdg_popup.grab`
is a SEPARATE request. A menu issues it; a tooltip does not. Win32 is
the same shape by another route -- a menu runs a modal loop with a
capture, a tooltip is a plain window that never captures.

So the grab is `WIN_POPUP_GRAB` in the positioner's flags word, which
was `reserved` and already documented as where flags would go. The
compositor stores it per window and `wm_client_popup_owner()` answers
only for a GRABBING popup, so a popup without one routes input as
though it were not there.

**OPT-IN rather than opt-out**, which is the one real choice here. A
forgotten flag then yields a menu that does not dismiss -- visible the
first time anyone opens it. Opt-out would make the same slip a tooltip
that eats clicks, which presents as a broken button somewhere else
entirely.

The migration's own lesson, and it cost a measured regression: **a
grabbing popup's KEYS go to the popup's slot, not its parent's.** A
dropdown inside the file chooser -- itself a dialog window with its own
router -- seeked with keys that were being delivered to the MAIN
window's widgets, because uapp translated a popup's pointer events back
to the owning window and let everything else fall through to the
toplevel. A popup now records which window it hangs off, and every
event type routes home.

## A face is lit by two geom helpers, and the face list stays the caller's

`geom.h`'s 3D section says it is not an engine -- no matrices, no
faces, no depth buffer -- and Shapes' shaded cube (2026-09-09) is the
first thing that could have argued otherwise. What it needed was a
polygon fill, a face normal and a Lambert shade; what it did NOT need
was for geom to know what a face is.

**What real renderers do.** Flat shading with back-face culling is the
whole of a Quake-era software renderer's lighting for a convex solid:
one cross product per face, one dot with the light, and no sort at all,
because on a convex body the faces that face away are exactly the ones
that are hidden. A mesh type (vertices + faces + per-face colour +
`draw_mesh()`) is what every small 3D library grows next, and then a
depth buffer, and then clipping planes.

**What toy-os does.** `geom_fill_polygon()` is the roadmap's polygon
fill, even-odd and sampled at PIXEL CENTRES so two faces sharing an
edge tile with no seam and no double coverage -- a square from (10,10)
to (20,20) covers the same 100 pixels a rect fill would. `geom_face_
normal3()` and `geom_shade()` are the two sums a caller's own face list
needs; both scale by the largest component before anything is
multiplied, because the cross product of two 160-unit edges is already
past Q16.16. The FACE LIST, the winding, the culling test and the
painter's order stay in the app, and that is the line: geom answers a
geometric question about three points, it never walks a model. Declined:
a `geom_mesh` drawn by one call (the engine geom.h refuses, and its
first caller is a demo), and lighting inside `gfxdemo.c` alone (the
cross product and the overflow are exactly what a second 3D caller
would re-derive wrong).

The one convention it adds is the winding rule, stated in `geom.h`:
with x right, y down and z away from the eye, a face wound clockwise on
screen has a normal pointing away, so a model winds its faces so the
normals point OUT and tests visibility against the eye. Gouraud was
declined for the cube specifically -- averaging a corner's three face
normals smooths the one thing a cube has, its edges.

## A client's events ride its own channel ring, and the kernel queue serves one reader

Stage 8 of `docs/winserver-ring3-design.md` (2026-09-09). Before it,
the compositor delivered every key, motion, resize and close by asking
the kernel to put it on the client's per-process queue
(`WIN_REQ_EVENT_PUSH`), and the client parked in `SYS_WAIT_EVENT`.

**What real systems do.** Linux keeps evdev in the kernel and nothing
else of a display server's traffic: a Wayland client's events cross a
Unix socket the compositor writes and the kernel merely carries.
Windows keeps a per-thread message queue in win32k and `PostMessage`
writes into it -- exactly the shape toy-os had, and the one Microsoft
has spent two decades paying for (win32k is the kernel's largest
attack surface). Stage 8 moves toy-os from the NT shape to the Wayland
shape, with shared memory in place of the socket because that is what
the channel already was.

**Why the inbox is inside the client's existing ring.** A second shm
object per client would double the compositor's per-frame name scan
and duplicate the head/tail/futex logic beside `uchan`. The client's
ring object grew a second single-writer ring the other way round, so
one object, one lifetime and one scan serve both directions; `uchan`
is where the SPSC discipline lives, and the compositor is its second
server-side user only by being the first to write back.

**Why a state is re-sent and input is dropped.** The kernel queue
could evict the oldest input to make room for a notification because
it owned both ends. A ring's writer cannot reach past the reader's
tail. Two answers were weighed: a bigger ring that drops the newest of
everything (simplest, and loses the invariant that a stuck client
still hears its close), or treating the notifications as what they
are -- STATES, whose latest value is all that matters -- and re-sending
them from a per-window pending mask when room appears. The second is
`xdg_surface.configure`'s model and was chosen; input is dropped-new
and counted, because a client 64 events behind is not one that wants
the backlog.

**Why the kernel does not announce its own death to clients.** It
would need the client list it just stopped keeping. A client that has
had nothing to read for one wait interval checks that the beacon's
live pid is the one its ring was granted to; a successor compositor
has a different pid and no knowledge of the window, so "the name
resolves" alone would be wrong. This is what a closed socket tells a
Wayland client, arriving on a timer instead of at once.

**Why `uapp_post()` has a spinlock and the ring does not.** A worker
posting from another thread is a second writer, and the inbox's
head belongs to the compositor. The post queue is private to the
process, guarded by an `xchg` spinlock, and wakes the loop by bumping
the inbox word with `lock xadd` -- the one word in the page with two
writers, stated in `uchan_page.h`. `WIN_EV_USER` is now an event the
client synthesises for itself and nothing else ever carries.

**Declined:** keeping `EVENT_PUSH` for self-posts only (a per-client
queue in the kernel for one message type); a kernel "creator died"
signal to clients (per-client state again, and sheddable under load);
and measuring latency by anything other than the ping the compositor
already sends (`tools/ping_rtt.py`), since a synthetic probe would
have measured the probe.

## Drag-and-drop is a toolkit session, not File Manager code, and the move-or-copy bit is the router's

The File Manager needed a file dragged between its panes and onto its
folder tree (2026-09-10). The router's pointer GRAB was the obstacle:
whichever widget consumes a press gets every motion and the release,
so the other pane and the tree were never offered anything. Two ways to
get past it.

**Track the drag in files.c** from the raw `on_motion`/`on_release`
the app still receives, with its own `pane_at()`. Smaller, and a
one-off: the tree widget could not be a target without the app
hit-testing rows on its behalf, and the next app wanting a drop
(Notepad, a file onto its window) would write the same thing again.

**A session in `uui_route.c`** -- the grab holder is asked once, past a
threshold, whether this press is a drag; if so the router hands motion
to the widget under the pointer as `drag_over` and the release as
`drop`, and names the target to the app. Qt (QDrag, dragEnterEvent/
dropEvent) and GTK (drag-motion, drag-drop) both put this in the
toolkit, and Wayland's `wl_data_device` is the same seam one level up.
Chosen: it costs one walk the router already had (`cursor_item()`'s),
and the fileview and tree each implement a target in a dozen lines.

**Move by default, copy with Ctrl, decided by the router.** Explorer
moves within a volume and copies across with Ctrl forcing a copy;
Dolphin asks Copy/Move/Link with a menu on every drop. toy-os has one
volume, so Explorer's within-volume case is the only case, and a
question on every drop would be the menu for a choice with one answer.
The bit lives on the payload (`d->copy`) and is set by the ROUTER from
the modifiers, not by the source or the target, so the ghost's "+" and
the drop's verb cannot disagree. A target that wants Dolphin's menu can
still open one from its `drop`.

**No drag cursor.** The cursor theme has five shapes and none of them
is a grabbing hand; adding one is a theme change on every machine for
a hint the ghost already gives. The ghost (icon, label, "+") is drawn
by the source after every overlay. Revisit if a second drag kind
arrives that has no natural ghost.

**Between windows is deliberately not here.** The pointer leaves the
source client's surface, so the compositor must carry a payload the
target learns of before the release -- the clipboard's typed formats
are the vocabulary for that, and it is a roadmap item, not a toolkit
extension.

## Cross-window drag is the compositor's, carried by a drag slot in the clipboard page

Dragging a file from the File Manager onto the desktop, or a desktop
icon into a pane (2026-09-10), needed something no client can do: know
what window the pointer is over once it leaves its own. Three shapes.

**Clients talk to each other** -- X11's XDND: the source finds the
window under the pointer through the server and sends it client
messages, the data travels as a selection. Every step is a round trip
between two processes that do not trust each other, and it needs a
"which window is at (x, y)" query toy-os deliberately does not expose to
clients. Rejected.

**The compositor brokers, the data rides shared memory** -- Wayland's
`wl_data_device`: the source starts a drag, the compositor sends
enter/motion/leave/drop to whichever surface is under the pointer, the
data source is separate from the clipboard. Chosen, minus the MIME
negotiation: there is one kind of payload here. The compositor already
had the state a drag hangs off (`content_pressed`, a held pointer
delivering motion to one window) and the hit test (topmost window under
the pointer), so `wm_dnd.c` is a hundred lines beside them.

**Reuse the clipboard as the carrier** -- put the dragged paths where
Ctrl+C puts them. Rejected because a drag then destroys what the user
copied, which every desktop treats as a bug; the page grew a second,
smaller slot instead (8 KiB: paths, not text), with the same seqlock
and writer lock. The slot is deliberately NOT cleared when the drag
ends: the drop reaches its target as a queued event, after the source
has seen its release, and the first version's release-time clear
handed the File Manager an empty payload. A stale slot costs nothing,
since only a drag's own events ever read it.

What is not built: a drag INTO Notepad or another app that has no
drop target (the router will offer it; nobody accepts), a drag cursor
shape, and offering the drag to a client's popup surfaces.

## The desktop shows its folder and nothing else, and the application database is a separate directory

Until 2026-09-10 `/usr/wm/desktop` did two jobs: it was the list of
installed applications (the Start menu) AND every entry in it was an
icon on the desktop, with `ShowIn=` the only way off. When
`/home/desktop` arrived the desktop became two lists glued into one
index space, and the maintainer's reading was the right one: two
directories both called "desktop", neither doing what its name said.

**What real systems do.** Linux keeps the application database in
`/usr/share/applications` and the desktop in `~/Desktop`; KDE's Folder
View and GNOME's (now retired) desktop icons show that folder, and
putting an app on the desktop is copying its `.desktop` file there.
Windows is the same shape: the Start menu is built from the Programs
folders, the desktop shows the Desktop folder, and "Pin to" or "Create
shortcut" adds a file. Nobody shows every installed application on
the desktop, and nobody has for twenty years.

**toy-os follows.** The database is `/usr/wm/applications`, the desktop
is exactly `/home/desktop`, a `.desktop` file there is a launcher read
by the same parser (`gui_app_read_entry()`), and "Add to desktop" on a
Start menu row writes one. A launcher on the desktop opens through the
registry entry with the same `AppId` so single-instance and remembered
geometry still apply, and falls back to its own `Exec=` so a launcher
outlives an uninstalled entry rather than going dead.

**Seeding once, not syncing.** Five launchers are put on a fresh
desktop so it is not blank, but through the seed writer's `once/`
mode, which copies a file only when it is missing. The `sync/` tree
would put a deleted launcher back on every `make iso` -- the exact
trap CLAUDE.md records for `seed/sync/` -- and a desktop the user
cannot clear is not a desktop. The cost is that a CHANGED default
launcher does not reach an existing image either, which is the right
side to err on for a file the user is expected to own.

**The one thing lost**: `ShowIn=desktop` no longer hides or shows an
icon; it now only governs the desktop menu's Open > submenu. Kept
rather than removed because the parser, the test and the entries all
carry it and the submenu is a real second surface.

## A fullscreen client is leased the display's scanouts, not imported into the GGTT

Asked and answered 2026-09-11, when direct scanout was built for the
laptop's Intel display (`docs/scanout-design.md`). The obvious design
is Wayland's: import the client's buffer into KMS and put it on the
primary plane. Here that buffer is a page list of scattered frames,
write-back cached, with an unaligned stride -- scanning it out means a
GGTT entry per frame, a write-combining retype per frame (each a 2 MiB
identity-map split) and a cache flush per present. That is the blitter
measurement's cost with none of its payoff.

**Chosen: the compositor LENDS its own grant.** `win_surface_grant()`
already gives the compositor three panel-sized scanouts, WC-typed and
GGTT-mapped, flipped by one register write; `WIN_REQ_FB_LEASE` maps
the same three into the fullscreen client and lets it present. That is
`drmModeCreateLease`'s shape -- a compositor handing a CRTC to another
process, as a VR runtime is handed a headset -- with direct scanout's
policy on top: per frame, topmost, unoccluded, opted in. Nothing in the
GPU driver changed, and the same mechanism works on virtio-gpu, which
is what makes it testable in the suite rather than only by eye.

**What it costs.** The client draws into write-combining memory, so it
must draw write-only -- `UAPP_SCANOUT` is that promise, and a widget
app that blends must not make it (it stays composed, correctly, with
its fullscreen state). And a fullscreen client's size must be the
panel's, which it is by definition of the state. What it does NOT cost
is a second copy of the buffer-rotation rule: the kernel's mailbox
(three buffers, never wait, `scanout_live()` decides the free one)
serves the lessee exactly as it serves the compositor.

**Why the hardware cursor is a precondition.** With the compositor not
presenting, its software cursor is not drawn; a lease without a cursor
plane would leave the pointer invisible. `-vga std` therefore composes
a fullscreen window, and the Intel driver and virtio-gpu lease. The
alternative -- the compositor drawing a cursor into the lessee's buffer
-- would have the two processes writing one buffer, which is the race
the whole design avoids.

**Why a lease ends in two phases.** The display is the compositor's
again the instant it asks, but the ex-lessee keeps its mapping until
its next present from its own buffer. A game draws continuously, so
when the lease ends it is mid-frame into the leased buffer; the first
version unmapped at once and DOOM took a page fault at `WIN_FB_VADDR`
20 ms later, on the laptop, twice. That is `wl_buffer.release`'s rule
-- a buffer is the client's until the compositor says otherwise, and
"otherwise" cannot be while the client is drawing into it. The kernel
is told of a dying address space before a page of it is freed
(`win_surface_space_gone()`), which is what makes the deferred unmap
safe: a mapping it still holds is in a space that still exists.

**Why the compositor forgets its buffers when a lease ends.** Buffer
age (`ugfx_screen_present()`'s damage union) assumes every scanout holds
one of this compositor's earlier frames. After a lease they hold the
client's, and the rotation index moved; `ugfx_screen_forget()` resets
both, so the first present after a lease is a whole screen. A partial
one showed the client's last frame through the desktop.

## A screensaver is a PROCESS the compositor spawns, and the timeout is what turns it on

**The split.** The compositor owns the idle clock; a saver owns the
pixels and nothing else. `wm_idle.c` counts the ticks since the last
input and spawns a program out of `/bin/wm/savers` when the machine has
been quiet for the configured minutes; the program is an ordinary
fullscreen client with no idea what it is being used for, and running
it by hand is a program that draws stars.

**Why not an overlay.** The on-screen keyboard and both tray flyouts are
overlays inside the compositor, and that was the cheaper option -- no
spawn, no descriptor, no process. It was declined because every new
saver would then be a compositor edit and every saver bug a compositor
bug. That is the argument X11 settled decades ago: the server blanks and
XScreenSaver draws in its own process, explicitly so a saver cannot take
the session down with it. Windows reached the same place from the other
direction, with a `.scr` the shell runs. Wayland moved only the CLOCK
into the compositor and exposes it through `ext-idle-notify-v1`, leaving
the drawing to a client -- which is exactly this arrangement.

**A saver is KILLED, not asked.** `wm_request_close()` lets a client
refuse, and every user-facing close goes through it -- correctly, for a
document with unsaved work. A screensaver is the one window where
refusing must not be possible: the user has touched the keyboard and
wants the machine back, and a saver that could decline would be a way to
lose it.

**No lock screen, deliberately.** toy-os has no accounts and no
passwords. A screen that demanded one would stop nobody and would cost a
way back in, which is a worse trade than the protection is worth. If
accounts ever exist, the locker is a separate program from the saver --
that separation is the whole lesson of XScreenSaver's history.

**The timeout is the enable.** Zero minutes means never, so there is no
second boolean that can disagree with it. Windows folds the same state
into its dropdown's "(None)"; the invalid combination simply cannot be
expressed here.

**Only this feature's own settings interrupt a running saver.** Every
setting in the system shares one generation counter, and the first
version stopped the saver on any change at all -- which killed it
whenever anything else was written, including the write that had just
selected it, so choosing a saver and starting it in one breath never
worked. The poll compares the two values it cares about instead. For
the same reason the idle clock is NOT reset by a settings write: a
script polling a setting is not a person at the keyboard.

**The Test button is a spawn, and nothing else.** Windows' Preview by
another name, and it needed no protocol: System Settings runs the
configured saver, and the compositor adopts any client whose SPAWN PATH
is in the savers directory. So a previewed saver is dismissed by input
exactly like one the idle clock started -- and a preview that could not
be dismissed would be a way to lose the machine, which is the failure
this arrangement makes impossible rather than merely unlikely. It also
means running a saver from a shell prompt behaves the same way.

**No pointer over a saver**, and on a machine with a hardware cursor
plane that has to be said rather than simply not drawn: the sprite is
not part of the composited image, so skipping the draw leaves it
exactly where it was. Starting a saver also damages where the pointer
is and asks for a frame, because skipping is not erasing and on a quiet
machine nothing else would repaint that region.

**What is NOT built: turning the backlight off.** A saver darkens
pixels; on a laptop the lamp stays on, which is most of the power. Doing
it properly needs a TRANSIENT display-power control, because
`system.brightness` is a persisted setting -- using it would overwrite
what the user chose and leave a dark screen behind any crash that
skipped the restore. That is DPMS, which is a different thing from
brightness in every system that has both, and it is on `docs/roadmap.md`.

## A saver's options are declared by a data file, not registered as settings

**The problem.** Every knob in every saver was a `#define` -- starfield's
star count, matrix's trail length, plasma's cell size. Making them
configurable means answering three separate questions: what options
exist, where the chosen values live, and who draws the controls.

**What other systems do.** Windows answers all three inside the saver: a
`.scr` run with `/c` opens its own configuration dialog and stores the
result wherever it likes, so the shell knows nothing and every saver
carries a settings window. XScreenSaver answers them declaratively: each
hack ships an XML descriptor of its options and `xscreensaver-settings`
GENERATES the dialog from it, with the chosen values becoming command
line arguments. GNOME dropped configurable savers entirely; Plasma's
lock-screen plugins are Windows' model in QML.

**Why XScreenSaver's, here.** toy-os already had every ingredient: the
saver list is a DIRECTORY (`choice_dir`), a setting's prose comes from a
data file (`/etc/settings.d`), and the Start menu is built from
`.desktop` files. A declarative descriptor is the shape this project
keeps reaching for, and the reason is always the same -- the thing that
knows is a file, so there is exactly one place to change and no C to
edit in a second component. A saver gains an option by growing a line;
System Settings never learns that any saver exists.

**Why NOT registry settings.** The obvious alternative was to scan the
descriptors at boot and register a `struct setting` per option, which
would have needed no change to System Settings at all. It was declined
on two counts. The registry is a BOOT-TIME list and what a saver offers
depends on which saver is SELECTED, which changes while the settings
page is open -- so the rows would all have to exist at once, and every
saver's options would be visible whether or not it was in use. And the
registry is a fixed-size table with a ceiling the ABI shares (`SETTING_MAX`,
45 of 56 used when this was written); six savers' worth of options would
have exhausted it, and raising a shared ABI constant to hold data that
is not settings is the wrong direction.

**So the settings page SYNTHESISES rows** at indices past the registry's
(`g_saver_base` in `settings.c`). That keeps one code path for layout,
staging, the focus ring and the status line; only the choice list and
the WRITE ask whether a row is an option. The alternative -- a second
kind of slot with its own drawing -- is the shape this app was built to
avoid, since it had one hand-written applet per setting before the
registry existed.

**The values are a file per saver, not arguments.** XScreenSaver passes
the options as argv, which toy-os could do (`SPAWN_ARGV` exists). A file
was chosen because a saver is spawned by three different things here --
the idle clock, the Test button, and a person at a shell prompt -- and
only the first two could be taught to build an argv. A saver run by hand
would silently draw its defaults, which is the kind of difference nobody
finds until they are debugging something else.

**The cost, stated: the Test button writes.** A child process reads a
file at startup, so there is no channel for a value that has only been
staged; a Test that skipped the write would preview the options you did
NOT pick. So it commits this saver's own options before spawning, and
says so in the status bar. It is the one place this page writes without
being told to, and the settings above the options still wait for Apply.

**A saver may declare nothing**, which is a state rather than an
absence: `blank` ships with no descriptor at all and the page simply
shows the two settings that select it. Making a descriptor mandatory
would mean shipping a file that says "no options", which is a file that
can go stale.

**What this cost elsewhere: the Test button moved up the page.** It was
the last item on the scrolling page, which was fine while the Screensaver
page held two rows and put it past the bottom edge the moment a saver's
three options were added above it -- unreachable, not merely awkward,
since a scroll view refuses a press outside its viewport. It now sits
directly under the saver chooser, which is where Windows puts Preview
and KDE its lock-screen preview: beside the thing it previews rather than
among the dialog's own verbs, and near enough to the top that no
saver's option list can push it off.

**The obvious fix was tried first and does not work, which is worth
recording because the symptom points somewhere else.** Moving it into
the ROOT layout beside OK/Apply/Cancel -- where an action arguably
belongs, and where CLAUDE.md's "keep the chrome outside the scroll view"
rule seems to send it -- produces a button of zero size. `uapp` runs
`uapp_desc.layout` at startup, on a resize and on a font change and at
no other time, so a root item whose `hidden` tracks the open page is
placed ONCE, while hidden, and never again. It was tried twice, as a
fourth column row (height 0) and as a row shared with the button group
(width 0), and both times the button drew its LABEL and nothing else --
which reads as a rendering bug and sent the first diagnosis after the
column's spare height instead. The rule underneath it: per-page
visibility belongs in the per-page item list, because that is the only
one rebuilt when the page changes.

## The Terminal's preferences are the app's, not the settings registry's

Every other knob in this system is a registered setting: a `struct
setting` in a `kernel/lib/*_config.c`, a row generated into System
Settings, a name `config get` and `usetting_get()` both answer. That
mechanism works and it is the one to reach for. The Terminal's palette,
font size, scrollback depth and cursor shape are deliberately not in it.

**Three reasons, in the order they decided it.**

The first is where the code would have to live. `setting_register()` is
the KERNEL's -- a setting is declared in ring 0 and enumerated over
`SYS_SETTING`. Registering `terminal.scheme` therefore means a
`kernel/lib/terminal_config.c`: ring-0 code describing a ring-3
application's appearance, which is the boundary `kapi.h` and
`userland/` exist to hold. Nothing about a colour scheme needs a
privilege level.

The second is what the registry is FOR. `system.font_size` reflows every
window on the desktop; `desktop.wallpaper` is the machine's. Those are
the system's, and putting them somewhere a person can find them without
knowing which app owns them is the whole value of one registry. A
terminal's palette is the opposite: somebody changing it is changing
that window. Konsole, GNOME Terminal and Windows Terminal all keep
theirs in the application, and none of the three puts a terminal's
colours in the desktop's settings -- which is a fact about where people
look, not about implementation.

The third is that the registry has no type for it. There is no colour
type and no colour widget, and a sixteen-entry palette is not a setting
in any case (see below).

**What was given up, stated plainly.** `config get terminal.scheme`
does not work; `/etc/terminal.conf` is read with `edit` or `cat`
instead. System Settings shows nothing about the Terminal. A second app
wanting preferences will write its own loader rather than inheriting
one -- `userland/term/term_conf.c` is about two hundred lines, and the
point at which that becomes a shared `uprefs` is the point at which
there is a second real caller, which is this project's usual bar and
not a plausible one.

**The file is `etc_config`'s format through `lib/uconf.h`,** so it is
not a private format either: `edit /etc/terminal.conf` works, the
parser is the kernel's own compiled a second time, and there is no
second set of answers to what a `name=value` document means.

## A colour scheme is a data file, and the file is in ANSI order

Sixteen colours is not sixteen settings. Konsole ships `.colorscheme`
files, GNOME Terminal a list of built-in schemes over a custom palette,
Windows Terminal named JSON objects that a profile references by name --
all three make a scheme a NAMED THING and let you pick one, and none of
them offers a grid of sixteen colour pickers as the primary control.
So `/usr/share/terminal/<name>.scheme`, picked from a list, which is
also the arrangement `/usr/wm/savers/<name>.saver` already uses here and
for the same reason: the thing that knows is a data file, so there is
one place to change and the UI carries no list.

**The permutation is the part worth recording.** A cell in the emulator
holds an `enum vga_color`, which is the IBM VGA ordering -- blue at 1,
red at 4 -- because that is what the shared ANSI parser resolves an
escape sequence to (`kernel/lib/ansi.c`, and the kernel console draws
from the same indices). Every palette anyone publishes is written in
ANSI order, red first. Authoring the files in VGA order would mean
hand-permuting a published palette on the way in, which is a step
nobody can check by looking at the result.

So the file is ANSI-ordered and `term_scheme_load()` permutes through
`ansi_color()` -- the parser's own table, so the two halves of the
system cannot drift into disagreeing about where red is.

**`Foreground=` and `Background=` name a `Color<N>`, and therefore
permute too.** They are indices rather than colours because SGR 39/49
return a CELL to the default pair and a cell holds an index; reading one
as a VGA slot instead put Solarized Dark's foreground on light red. It
rendered, it looked like a colour scheme, and it was the wrong one --
which is why `tools/term_scheme_hostcheck.py` exists and reimplements
the table rather than importing it.

**Changing a scheme remaps the cells already on screen.** A cell has no
"this is the default" bit, so moving the default pair from 7-on-0 to
12-on-8 would leave the whole scrollback drawn in two colours the new
scheme reserves for something else. The foreground and background move
independently, because a coloured `ls` leaves an explicit foreground
over a default background. The imprecision -- a cell that explicitly
asked for the old default colour moves with the rest -- is invisible in
practice, and the only alternative is a wider cell.

## The Terminal's caret does not blink by default

Konsole, GNOME Terminal and Windows Terminal all blink theirs; xterm
and VS Code's terminal do not. It is a genuine split, so the tiebreak
here was made on something this project can measure.

**A blinking caret makes a focused terminal ANIMATE**, and almost
everything this repo uses to judge something drawn is a comparison of
two SETTLED frames (`QMPSession.screenshot()` is settled by default).
A settled frame is two identical consecutive reads -- which a window
repainting twice a second supplies constantly, in either phase. The
comparison then cannot tell a real change from a caret that happened to
be off, and `uterm_test.py`'s tab-switch check failed exactly that way:
it had passed on the three runs before blinking was added and failed on
the next, with tab switching working perfectly.

So `cursor_blink` ships off and is one line in `/etc/terminal.conf` for
anyone who wants it. The cost of the other choice is not "a slightly
noisier screenshot" -- it is that every frame comparison against a
Terminal becomes unreliable, including ones written later by somebody
who does not know this is why.

**The cadence is the caret's and nothing else's.** The 500 ms tick
exists only to toggle the phase, and `on_tick` returns 0 -- repainting
nothing -- while blinking is off, so the default costs two wake-ups a
second and no frames. That is against the 33 a second this window paid
before its reader threads landed, and it is the reason the tick could be
reintroduced at all.

## Two font families, and why the slot is the old name plus arithmetic

The interface and a terminal want different faces. That is not a matter
of taste: a terminal is a GRID, and a grid drawn in a proportional face
does not line up. Every system that has both keeps both -- GNOME's
`font-name` beside `monospace-font-name`, Windows' UI font beside
Consolas, macOS's system font beside SF Mono.

toy-os had one, `system.font_face`, and it had to be monospace because
the Terminal used it. So the whole desktop looked like a terminal, and
the fix was never "pick a nicer face" -- it was that one setting could
not express the requirement.

**A WIDGET SAYS NOTHING AND GETS THE UI FAMILY.** The alternative was
`ugfx_font_session(family, weight)` at all 31 existing call sites, every
one of them a widget, every one of them saying `UI`. That was rejected:
it is 31 mechanical edits that prevent no bug. The case worth catching
is a TERMINAL silently drawing proportional text, and what catches that
is having to NAME `ugfx_font_mono()` -- the unusual requirement is the
one that gets spelled out. A widget inheriting the interface font is
the right default and reads better unqualified.

**THE SLOT IS `family * weights + weight`, ENCODED INTO THE EXISTING
ONE-NUMBER SHM NAME.** `FONT_SHM_NAME_FMT` was `"font.%d"` indexed by
weight; it is now indexed by slot, which leaves `font.0`/`font.1`
meaning precisely what they meant and adds the monospace family at
`font.2`/`font.3`. The ABI grew by addition rather than by a second
field and a new name format, so nothing that reads a font had to change
to keep working.

**`family` WAS APPENDED TO `struct font_shm`, NOT INSERTED.** Every
table in that object is reached through an explicit byte offset
(`glyph_off`, `adv_off`, `kern_off`) -- the header says so, and says
why -- so a reader built before the field computes the same addresses
and never looks at it. Inserting it beside `weight`, which is where it
belongs by meaning, would have shifted `cell_w` onwards and handed such
a reader plausible garbage metrics. Meaning lost to compatibility, and
the comment beside the field records that it was a choice.

**NOTHING REFUSES A PROPORTIONAL FACE IN THE MONOSPACE SLOT.** Whether
every advance is equal is knowable only after rasterizing, which
happens in ring 3; the setting's `apply` is in ring 0 and can validate
existence and nothing more -- the same honest limit `system.font_face`
already had. fontd logs it instead, and the published header carries a
`monospace` flag, so the condition is reportable rather than silent.

**Both defaults are the same face today**, so a machine that has never
been configured looks exactly as it did. This change adds the ability
to differ; choosing to is separate, and reversible in one setting.

## A settings category with one page collapses, and that needed a third row kind

**Superseded for System Settings on 2026-09-28** by "System Settings is
cards under a searchable sidebar, and leaving a changed page asks": every
category now keeps its heading, and a lone page sits under it like any
other. `UUI_SIDEBAR_TOP` stays in the widget -- the file chooser's
places list uses it.

System Settings generates its sidebar from the categories the settings
themselves declare, so the taxonomy is data and the widget is generic.
Splitting Appearance -- which had accumulated the wallpaper, the taskbar,
the tray, the week's first column and Diagnostics alongside the actual
look of things -- left several categories holding exactly one page, and a
heading with one child under it is a row that says nothing followed by a
row that says the same thing indented.

Collapsing it is obvious. What is not obvious is what the surviving row
IS. `uui_sidebar` had two kinds and neither fits: an ITEM is indented and
selectable, so the collapsed row reads as a child of whatever heading
precedes it; a HEADING is unindented and INERT, so the one page in the
category cannot be opened. The two properties had been one flag because
nothing had ever needed them apart.

So `UUI_SIDEBAR_TOP` is the third kind -- a heading's weight and indent
with an item's selectability -- and `heading` (how it draws) is now a
separate field from `inert` (whether it can be chosen). This is what
Windows' Settings and GNOME's do with a lone entry, and the split is the
honest shape: a row's appearance and a row's behaviour were never the
same question.

The alternative considered and rejected was for the app to emit the
collapsed row as an ITEM and un-indent it by hand. That puts sidebar
layout in every app that has a sidebar, which is the "make every fix
twice" shape this repo keeps deleting.

## A file chooser is a window of its own, and the toolkit grew a second toplevel to give it one

Notepad's Open/Save was a `uui_dialog` with a `uui_fileview` body --
a modal drawn inside Notepad's own window. That is the right shape for a
question ("Save changes?") and the wrong one for browsing a filesystem:
in a small window there is nowhere to put a listing. Image Viewer and
Audio Player each said so in their own top-of-file comments, and each
answered it by NOT having a chooser at all -- a sidebar pinned to one
directory (`set_navigable(0)`), which meant neither app could ever leave
the folder it was launched in. Three apps, three different amounts of
"cannot open a file".

What real systems do: Win32's `GetOpenFileName`/`IFileOpenDialog`, Qt's
`QFileDialog` and `GtkFileChooserDialog` are all IN-PROCESS -- a real
top-level window created by shared toolkit code in the calling app, and
modal to its owner. The out-of-process chooser (`xdg-desktop-portal` on
KDE and GNOME, macOS's `openAndSavePanelService`) exists for
SANDBOXING: the app cannot read the filesystem, so the portal reads it
and hands back one file. toy-os has no such boundary, so copying the
portal would be copying the size rather than the shape -- and it would
need an IPC to return the path plus a modality the compositor could not
express anyway.

So the chooser is in-process and the missing piece was a second
toplevel. `uapp` said "one process, one TOPLEVEL -- plus its popups",
and the popups already had everything expensive: a surface per slot,
buffers, presents, input routed by slot. What a dialog adds over a popup
is chrome, a place in the stack under its owner, and modality -- none of
which a popup may have, since a popup is dismissed by the first press
outside it and a filename half typed must not vanish on a stray click.
Hence a third surface kind (`WIN_REQ_DIALOG`) rather than a flag on the
second, and `uapp_window_open()` beside `uapp_run()`.

Three consequences worth stating, because each one is a trap:

**`bring_to_front()` keeps its contract.** Raising a window's dialogs
inside it is the obvious implementation and it is wrong: four call sites
in `wm_input.c` read `window_count - 1` immediately afterwards to start
a drag or a resize, so the owner's title bar armed a drag and the thing
dragged was the DIALOG. `raise_with_dialogs()` is the carrying version,
and only the taskbar needs it -- everywhere else the modal block gets
there first.

**Modality is a press redirect, not a disabled window.** Win32 disables
the owner HWND; here `wm_dialog_blocker()` is asked at the top of the
window walk in `wm_handle_left_click()` and the dialog is raised
instead. One place, so the title bar, the resize grip and the content
are all covered by the same answer -- and a window that could be dragged
away from its own modal is how the two end up on opposite sides of the
screen with nothing to say they are related.

**The layout log's dedupe is per surface.** It held one previous block,
compared per frame; a toplevel and a dialog reporting alternately never
match, so the dedupe would have been off exactly when two windows were
open. Two slots, indexed by "toplevel" and "a dialog".

**Modality is three doors, and the first version blocked one.** A press
on the owner was redirected, and that looked complete -- until the
question "can I still click Notepad's menus?" was actually asked. The
SECONDARY click had its own window walk with no check in it, so the
app's context menu was still live; and MOTION still reached the owner,
so its menu bar highlighted under the cursor while refusing every click,
which is a control advertising itself as usable. Win32 gets all three
from disabling the owner HWND. Here each is a `wm_dialog_blocker()`
call, and the lesson is that "modal" is a property of every input path,
not of the one you happened to test.

**And a directory has to pass the filter whatever the app says.** Each
of the three apps' filters answers "is this a file I can open", so each
says no to a folder -- correct for the sidebars they were written for,
and fatal in a chooser, where it lists an empty root with no way out.
The chooser gates the app's filter rather than asking every caller to
remember; the bug shipped and twenty-two green checks missed it, because
every one of them reached its directory by a Places row or a typed path
instead of by walking one.

The WM's own picker was deleted in the same change. It had been
unreachable since the apps moved to ring 3 -- 531 lines that nothing
called, plus fifteen live `!file_picker_open` guards threaded through
`wm.c`, `wm_render.c` and `wm_overlay.c` for a flag that was always 0.

## A widget resolves its colours when it draws, not when it is built

Every widget's `_init()` filled its colour fields with `ugfx_rgb()`
literals, and those literals were -- almost always -- exactly the
default palette. That looks harmless and is the reason a theme change
could not work: an app builds its widgets once, at open, so a palette
swapped afterwards would reach nothing that already existed. The
`utheme` object had shipped, `UTHEME_*` were already live reads of it,
and the widgets were still holding a private copy of its defaults taken
at construction time.

The fix is a sentinel rather than a constructor argument.
`UUI_COLOR_UNSET` (0xFF000000, outside the 24-bit range `ugfx_rgb()`
produces, so no real colour collides) means "no opinion"; `UUI_COLOR(v,
UTHEME_ROLE)` at the draw resolves it. An app that sets a colour still
wins, because any other value is not the sentinel -- which is what kept
this from being an audit of every app as well as every widget.

GTK and Qt both resolve a style at paint, for exactly this reason: a
`QPalette` is consulted by `QStyle::drawControl`, not copied into the
widget at construction. The alternative shape -- broadcast a theme
change and have every widget rebuild -- is what `WIN_EV_FONT` does for
metrics, and it is the wrong tool here: metrics change the LAYOUT, so a
client has to re-run it anyway, while a colour changes only what the
next frame paints.

**Converting a literal is only safe when it equals a role exactly.**
Nine of the table's colours, four of the textbox's and three of the menu
bar's did, and those are converted with the rendering provably
unchanged under the default theme. Five did not -- a table header's ink
at (40,40,40) against `text` at (20,20,20), the menu bar's bar and popup
grounds, its accel and disabled greys -- and mapping them to the nearest
role would have silently restyled every menu and every table header in
the same commit that claimed to change nothing. They stay literals, and
`docs/roadmap.md` says so.

**The palette gained three roles rather than reusing one.** `outline`
(150,155,165) appeared by hand in eight widgets and `selection_bg`
(205,220,240) in six, and neither is what it would otherwise have been
folded into: `border` (60,60,60) frames a window and is nearly black,
while `accent` is the saturated selection colour a focus ring uses. A
`separator` followed for a table's grid lines. Qt keeps the same
distinctions (WindowText against Mid/Dark, Highlight against Window),
and a dark mode is where collapsing them would show -- the three greys
move by different amounts, and a wash under dark text has to become a
wash under light text without becoming the accent.

## The text measurement functions are their own translation unit

`ugfx_text_width`, `_width_n`, `_fit_chars`, `_index_at_x`, `_next` and
`_prev` are in `userland/ui/ugfx_text.c` rather than in `ugfx.c` with
the rest of the drawing. They are the only text functions that touch no
font state and no surface: they stand entirely on `ugfx_char_advance()`
and `ugfx_kern()`, which stay with the atlas.

That is what makes them compilable on the host, and the reason it was
worth a file. This is the arithmetic that decides where a caret sits and
which character a click selects, and it is arithmetic whose errors are
invisible on a monospace face -- `n * char_w` and a real measurement are
the same number, so every one of ~23 wrong sites looked correct for as
long as the interface face was `dejavu-sans-mono`.
`tools/ugfx_text_hostcheck.py` compiles these two files against a
synthetic face where `i` is 3px and `W` is 20 and sweeps ~4,400 checks,
including the round trip that the x a character is DRAWN at hit-tests
back to that character. None of that is reachable from a screenshot,
and a guest test could only ever sample a few strings.

**Kerning is why a slice must be measured on the slice.** It is counted
between adjacent characters, before the advance of the second, so the
width of `str[start..i]` is not the difference of two offsets into the
whole string -- an offset includes a kern pair that drawing the slice on
its own never applies. `uui_textbox` measures the exact buffer it draws
for this reason, and `ugfx_text_width_n` documents it, because the
faster-looking version (one subtraction instead of a walk) is wrong by a
pixel or two per scroll step and would be reintroduced by anyone
optimising it.

## A screenshot is the compositor copying pixels, and the client encoding them

A capture has to start at the compositor, because nothing else can see
the composited frame: the mapped framebuffer is write-combining and is
never read, and the finished picture lives in the compositor's own back
buffer (`ui/ugfx.h`). The question was what crosses the boundary.

**The obvious design -- the client names a file and the compositor
writes it -- does not fit, and the reason is a number.** A client
channel slot is 64 bytes (`lib/uchan_page.h`) and `FS_PATH_MAX` is 64 on
its own, so a path cannot ride a request at all. Widening the message
was available and was not taken: `struct wmchan_msg` is on the path of
every `WIN_REQ_PRESENT`, which is once per client per frame.

So the compositor copies into a shared-memory object the CLIENT created
and granted, and the client does everything else. That is
`wlr-screencopy-v1` exactly (the client provides the buffer, the
compositor fills it) and it is KWin's `ScreenShot2` with a name instead
of a passed fd. X11's `XGetImage` on the root window is the shape this
deliberately is not -- any client reading the whole screen with no
mediation is now regarded as a hole, which is why Wayland forbids it and
routes captures through the compositor or a portal.

**What the split bought, beyond fitting in 64 bytes.** The compositor
contains no image encoder and never learns that file formats exist, in
the same way it contains no decoder (see `uimg.h`'s header for that
half). And `/bin/screenshot` is an ordinary program with no window: it
connects to the compositor like any client, so it works from a terminal,
from the physical console, and over `telnet` on a machine whose screen
nobody is watching. That last one is the whole reason a bare-metal
capture is possible -- `tools/remote.py screenshot` runs it on the
laptop and fetches the file, where `vm.py`'s QMP screendump has no
equivalent.

**A capture renders a frame before it copies.** A request is handled in
the middle of the frame loop's message pump, so anything that changed
this iteration is not in the back buffer yet -- including, critically,
the asking client having just been hidden. A client-side sleep was the
alternative and it would have been guessing at a frame rate this loop
does not have.

**`WIN_SHOT_NO_SELF` hides the asker rather than minimizing it.** The
first attempt added a `WIN_REQ_MINIMIZE` so the tool could put itself
away, which is a real gap in the protocol (`xdg_toplevel.set_minimized`
exists, and so does `ShowWindow(SW_MINIMIZE)`) -- but it has no inverse
here by design, since an app that could put itself back on screen
unasked is an app that steals focus. Leaving the client's windows out of
one rendered frame needs no new request, no restore path, and produces
the same thing on screen: the window goes away, the shot is taken, it
comes back.

## The pointer is excluded by default, and both directions are work

Every system's default is to leave the pointer out -- X11's `XGetImage`,
Windows' PrintScreen, GNOME and Spectacle's unticked checkbox -- because
the common use is documenting what is on screen, not where the mouse
was. toy-os follows.

What is not obvious is that "exclude it" and "include it" are each real
work, and which one is work depends on the machine. With a SOFTWARE
cursor the compositor has already drawn the sprite into its back buffer,
so excluding means UNDRAWING it; the pixels it covered are already saved
(`cursor_under`, which exists for the cheap cursor-move path), so the
result is the frame as it would have been rather than a repaint of it.
On a HARDWARE cursor plane the pointer was never in the back buffer, so
including it means drawing the sprite into the copy -- which is why
`wm_render.c` gained a destination surface for its cursor drawing at
all.

Neither is something a caller could get right from outside, which is why
this is a flag on the request rather than something a client does to the
pixels afterwards. The laptop exercises the second half and no emulated
setup here exercises it the same way: `guictl state` reports
`hwcursor: true` there and `false` under QEMU.

## Inflate is a library, and PNG stopped being write-only

PNG arrived here able to write and not read (the entry below records
why), and `uimg_decode()` answered `-ENOTSUP`. The decoder landed once
there was a reason to build inflate properly, and the question worth
recording is where inflate went.

**It is a library because it has two real callers, not because a
library is tidier.** `userland/lib/uinflate.c` serves the PNG codec and
`/bin/wget`'s gzip `Content-Encoding`; the deflate COMPRESSOR moved
there out of `uimg_png.c` at the same time, so one module owns both
directions and there is one place to test them. Had PNG stayed the only
caller it would have belonged inside the codec, which is where it sat
while it was write-only.

**It is ring 3 only, and the obvious-looking precedent does not
apply.** Linux carries `lib/zlib_inflate/` in the kernel -- but to
decompress its own kernel image and initrd. toy-os compresses neither,
so there is no ring-0 caller, and compiling it twice would be the
speculative generality this project's conventions warn against. The
file names nothing kernel-side, so the day a compressed module exists it
is a move rather than a rewrite.

**THE INPUT IS ONE CONTIGUOUS BUFFER AND THE OUTPUT IS A CALLBACK.**
This is the load-bearing decision. zlib's own interface streams both
ways, which is why its decoder is a state machine that can suspend
mid-symbol, mid-match and mid-block -- a dozen resume points whose bugs
appear only on input split at a particular byte. Consuming from one
buffer makes the decoder a straight loop, and emitting through a
callback means nothing needs the decompressed size in advance. The cost
is real and worth stating: a caller holds the whole compressed input.
For PNG that changed nothing (the IDAT chunks are concatenated before
inflating either way); for an HTTP body it is the compressed size,
which is what asking for gzip bought.

That cost is also why `wget` buffers a gzipped body and streams a plain
one -- the uncompressed path still goes straight to the sink.

**What the decoder refuses is as deliberate as what it reads.** 8-bit,
every colour type including palette, plus tRNS, not interlaced. The two
left out -- sub-byte and 16-bit depths, and Adam7 -- are where PNG
decoders go subtly wrong, and both are rare now. They are `-ENOTSUP`
rather than `-EINVAL` for the reason `uimg_jpeg.c` refuses progressive
that way: an app can say "this build cannot show 16-bit PNGs" instead of
calling a perfectly good file corrupt.

## PNG is written here and not read, and that is an honest state

**SUPERSEDED the same day, by the entry above: PNG reads now.** Kept
because the reasoning for shipping a write-only codec is still the
reasoning for the next format that arrives write-first, and because the
deflate half of it is unchanged.

`uimg.h`'s codec table now has an `encode` slot, and the PNG row fills
it while leaving `decode` NULL. That asymmetry is deliberate rather than
unfinished work waiting to be tidied.

PNG exists here for pictures that LEAVE the machine -- a host, a
browser, a bug report -- and reading one needs inflate, which is a
separate piece of work with its own testing pass. QOI covers everything
that stays: the Image Viewer already decodes it, it is lossless, and on
flat UI content it is smaller than PNG anyway. So the screenshot tools
default to QOI and offer PNG, and `uimg_decode()` answers `-ENOTSUP`
with a sentence saying this build writes the format and cannot read it.

That is a different answer from `-EINVAL`, and the distinction is the
reason `-ENOTSUP` exists in this library: an app can say "this build
cannot show PNGs" instead of calling a perfectly good file corrupt.

**The deflate is fixed-Huffman LZ77, and stored blocks were the trap.** A
spec-legal PNG can be written with uncompressed deflate blocks in about
thirty lines, and a 1920x1080 screenshot then weighs 6.2 MB -- which is
a minute per capture over the TFTP link this exists to serve. A desktop
is mostly flat colour, so a greedy match finder takes the same shot to a
few hundred KB, and that is what makes the bare-metal path usable at
all.

**Both encoders are checked by foreign decoders, never by ours.** An
encoder tested against this repo's own decoder passes whenever the two
share a mistake, and the two mistakes an image encoder actually makes
are exactly that shape: a QOI index table updated on the wrong chunk, or
a Huffman code packed least-significant-bit-first. Both produce a file
that round-trips perfectly here and that nothing else in the world can
open. `tools/uimg_codec_hostcheck.py` opens everything with Pillow and
additionally inflates each PNG with Python's `zlib`; the guest test
keeps the round trip only for what the host harness cannot see, which is
the encoders running in ring 3 on the machine that ships.

## A nested layout takes no margin, because margins compound

`uui_layout_margin()` read anything `<= 0` as "the font-derived
default", which is right for the layout that fills a window and wrong
for every layout inside one: System Settings nests three deep and paid
three character cells, 28 px before the sidebar and another 14 inside
the page, on a window whose page area was then too short for its own
content.

**Qt and GTK both put the padding at the window and none at each nesting
level** -- a Qt sub-layout's contents margins are zero, a GtkBox has no
padding of its own -- and that is the rule now: a layout laid out as a
CHILD contributes no margin unless it names one. An explicit `margin`
still wins, so a container that genuinely wants inner padding asks.

The flag is latched by the OPS TABLE, in `natural_size` as well as
`set_geometry`, because being asked either question through the ops
table is exactly what "somebody else is placing me" means -- and the
measurement happens first, so latching only in `set_geometry` would
measure one frame against a margin the layout then did not use. That
also covers a layout nested in a scroll view or a file dialog, not just
one nested in another layout.

The `margin = 1` idiom this replaces -- one pixel, meaning "not the
default" -- was a workaround for the same problem and is no longer
needed.

## The scroll view drives its scrollbar in pixels, not rows

`uui_scrollbar`'s maths is a pure ratio: the thumb is `h * visible /
total` tall and sits at `track_range * off / max_scroll`. Any consistent
unit works -- and rows were not consistent. The scroll view rounded
`total` UP (to keep the last partial row reachable), `visible` DOWN, and
the offset DOWN again, so a view scrolled fully to the top reported an
offset SHORT of the bar's own maximum and the thumb was drawn several
pixels below the track. The bottom was exact, which is why it read as
"the scrollbar will not go all the way up" rather than as a rounding
bug.

All three call sites -- draw, hit and drag -- agreed with each other,
which is why nothing misbehaved on a click: the thumb was in the place
the hit test also believed. Only the eye disagreed.

Pixels fix both ends exactly and make a thumb drag smooth instead of
snapping to a row. The three numbers come from one function (`sb_units`)
for the reason the bar's own geometry is shared: three callers deriving
the same three values separately is how they come to disagree.

## The tray says when somebody else is on this machine, and remoteness is derived

A machine reachable over the network gives its owner no way to see that
somebody is acting on it. KDE's krfb raises a tray indicator for as long
as a remote party is connected, every Wayland screen-share portal does
the same, and Windows shows a session notification -- because the one
thing the owner must never have to guess is whether they are alone on
their own machine. `desktop.tray_remote` = `auto` therefore means "while
a session is open", which is this item's version of the tray's
ask-the-hardware rule.

**REMOTENESS IS DERIVED, NOT DECLARED.** A session is remote when the
process that CREATED it was serving a connection --
`scheduler_make_session_leader()` is the one place a session is born, and
it asks whether the parent holds a connected socket. telnetd is handed
its connection by inetd and then spawns a shell with `SPAWN_SETSID`; a
GUI Terminal makes the same spawn holding no socket and is not remote.
Nothing in ring 3 is trusted to say so, and the peer travels with the
session, so every later record carries the address without anybody
passing it down.

The obvious version of that check asked fd 0 and found a PTY: telnetd
dup2s the pty onto 0/1/2 around the spawn so the child inherits them,
and keeps its socket on a descriptor of its own. The question worth
asking is not "what is this process reading" but "is it serving a
connection", which is every descriptor.

**A SEPARATE RING, NOT THE KLOG.** The kernel log is 16 KiB shared with
the kernel's own output, and a flash writes hundreds of transfer lines:
the evidence would destroy the evidence beside it, which is the failure
CLAUDE.md already records as a probe outrunning its log. `QUERY_REMOTELOG`
is its own 128-record ring, numbered like `QUERY_CONNLOG` so a reader
knows both what is new and how many it missed.

**TWO OF THE FOUR SOURCES ARE RING 3's, AND THAT IS WHY THERE IS A
SYSCALL.** The kernel sees sessions opening and closing and every program
a remote session spawns (`spawn_from_fs()` is the one funnel, which is
auditd's execve shape). What it cannot see is the command LINE -- `cd` and
`config set` spawn nothing at all -- and which file a transfer moved,
which only tftpd knows. `SYS_REMOTE_LOG` is that door, and **the kernel
decides whether to keep the record**: it is kept when the caller's
session is remote, with the SESSION's peer stamped on it rather than the
caller's claim, or when the caller names a peer it is serving (a
service, like tftpd). Anything else is dropped -- and still returns 0,
because a shell must not behave differently for being watched.

The alternatives were checked rather than assumed. `applog` is the right
shape (tagged by the kernel, follow-able by `seq`) but a process can only
get a log descriptor through `SPAWN_FD_LOG` on **stdout at spawn time**,
and both of these have their stdout bound elsewhere -- a pty and a
socket. `SYS_DIAG` is a relay to a ring-3 provider with one in-flight
slot, not a sink.

## A resize edge is a MASK, and the opposite edge is the anchor

`wm_find_resize_zone()` answers a `WM_EDGE_*` bitmask rather than the
`out_right`/`out_bottom` pair it used to, and only the right and bottom
edges started a resize before 2026-09-16 -- the whole title-bar row was
refused outright, which made the top edge and both top corners
unreachable on every window here, since the top edge IS the title bar.

**The mask is the shape Wayland's `xdg_toplevel.resize_edge` uses**, and
X11's `_NET_WM_MOVERESIZE` before it: four edges, a corner being two of
them at once. The alternative -- eight named zones, as Win32's
`WM_NCHITTEST` returns HTLEFT/HTTOPLEFT/... -- needs eight cases in
every consumer. With a mask the drag code asks "is LEFT set" four times
and never enumerates a corner at all, and the cursor picks its diagonal
from which pair is set.

**The opposite edge is the anchor, so the minimum size and the screen
edge clamp the ORIGIN rather than the extent.** A left drag that has hit
`MIN_CONTENT_W` must stop moving `x`; clamping the width instead lets
the window walk leftwards while staying the same size, which reads as a
move with extra steps. Each of the four edges therefore clamps a
different quantity, and that asymmetry is the whole of the arithmetic.

**The window moves LIVE, and a client sizes itself a frame or two
later**, so the anchored edge can visibly lag on a slow client -- the
compositor owns `x`/`y` and can move them at once, while `w`/`h` only
change when the client presents at the new size (`resize_pump()`). The
alternatives were considered and declined for now: applying the origin
on the ack, as KWin and Mutter do with `ack_configure`, is the correct
answer and needs `resize_pump()` to carry the edge mask and the anchor
origin; forcing outline mode on left/top drags makes one half of the
frame behave differently from the other. `desktop.resize_mode = outline`
already answers it for anyone the lag bothers, which is why the simple
version was taken first.

**The title bar's top strip resizes and the rest of it drags.** Windows
and KWin carve the same strip, and the ordering matters: the edge test
runs BEFORE the title-bar test, or the bar swallows the top edge again.

**A corner is bigger than its edges** (`RESIZE_CORNER`): within that
distance of a corner the perpendicular edge joins the mask, so a
diagonal drag has a square to aim at rather than the point the two
strips intersect in. Every desktop does this.

**AND THE BORDER IS MOSTLY OUTSIDE THE WINDOW** -- `RESIZE_OUTSIDE` (8)
hanging over whatever is behind, against `RESIZE_INSIDE` (2) on the
frame itself. This is Windows' `SM_CXPADDEDBORDER` and KWin's
resize-only border, and the reason is that the inside is expensive while
the outside is free: the top edge lies over the title bar, so every row
it claims stops dragging the window. An 8px inside border put the corner
16 rows into a 23-row bar and raised the diagonal at the bar's own
MIDPOINT -- reported as a corner cursor appearing far from the corner,
which is what it was. Thinning the inside alone would have shrunk the
target; moving it outside keeps a 10px-deep grab with a bar draggable
end to end.

**The band steals from whatever is behind it**, which is the bargain
every desktop with an invisible border makes, and it is bounded two
ways. A window that cannot be resized projects NO band -- otherwise a
fixed-size popup would swallow the resize border of the window under it
-- and because the search is topmost-first and a window owns everything
genuinely inside it, a lower window's band can never punch up through a
window in front. Both directions are asserted in
`resize_edges_test.py`, along with the band ENDING: a moat of
unclickable desktop around every window is the failure mode.

## Exif orientation is applied by the DECODER, not by whoever is showing the picture

A photograph shot in portrait is stored as the sensor read it, with a
tag saying how the camera was held. Somebody has to act on that tag, and
the two real answers disagree: libjpeg deliberately does NOT, handing
the caller markers and letting it decide, and GdkPixbuf makes it a
separate `gdk_pixbuf_apply_embedded_orientation()` call. Qt's
QImageReader has `setAutoTransform()`, off by default for source
compatibility and turned on by essentially every application that uses
it.

toy-os applies it in `uimg_jpeg.c`, because the library-versus-
application argument does not transfer. libjpeg and GdkPixbuf have
thousands of callers they cannot see, some of which genuinely want the
stored pixels; `uimg.h` has a handful in this tree, every one of which
is showing a picture to a person. An app that has to ask is an app that
will forget, and the failure is silent -- a sideways photograph looks
like a photograph. Browsers reached the same conclusion when CSS
`image-orientation: from-image` became the default.

Two consequences worth knowing. `uimg_info()` reports the size as it
will be SHOWN, dimensions swapped for the four orientations that
transpose, because the stored size is not a fact about the picture and a
caller sizing a window wants the one it will draw. And the transform
happens AS THE PIXELS ARE WRITTEN, inside the emit loop, rather than by
rotating a finished buffer: the second buffer for a 16-megapixel photo
is 64 MB, and the placement costs an index calculation.

## The JPEG encoder has no quality knob, and one integer is not worth an options struct

`uimg_encode()` takes an image and a format name. QOI and PNG need
nothing else; JPEG obviously could use a quality, and the obvious move
is to add an options struct so it can have one.

It has a constant instead (`J_ENC_QUALITY`, 85 -- libjpeg's default is
75 and every export dialog's is 85-92; 85 is where a photograph stops
ringing on a hard edge at normal viewing size). The bar this project
applies to adding API is a SECOND REAL CALLER, not a plausible one, and
there is one caller that writes a JPEG at all. An options struct that
exists to carry a single integer changes every call site in the tree,
makes the two formats that need no options carry a parameter they
ignore, and has to be designed now for knobs nobody has asked for --
subsampling, optimised tables, progressive output.

The knob should arrive when something needs to VARY it. A photo editor
with a save dialog is the case that forces it; a screenshot tool is not,
because it wants one answer and this is the answer.

## Image Viewer decodes one image at a time, and a request during a decode is remembered rather than run

Moving the decode off the paint loop is not the decision -- a 280 ms
freeze per image made that one for itself, and no viewer anywhere
decodes on its UI thread. The decision is what happens when a second
request arrives while the first is still going, which it does constantly:
arrowing down a folder fires one per row.

Three answers were available. Refuse the new request until the old one
lands, which makes the list feel stuck for as long as the decode takes.
Run them concurrently, which is N decoders competing for one CPU to
produce N-1 pictures nobody will look at, and needs a rule for which
result wins anyway. Or run one and REMEMBER the latest thing asked for,
which is what this does.

The cost is honest and small: at most one already-started decode
finishes and is thrown away, so arrowing quickly through ten files
decodes two of them. The alternative that looks cheaper -- cancelling
the in-flight decode -- needs a cancellation point inside the codec,
which means the codec learning about threads, and buys back one decode.

What this leaves is the property a test can hold on to: the app reports
how many frames it painted while a decode was in flight. A synchronous
decode paints none, which is why that number is the check
(`tools/imgview_test.py`) rather than a screenshot of the transient.

## Thumbnails have no rate, and their cache file is NAMED after the source

The File Manager decoded a couple of thumbnails per 500 ms tick, which
is four a second whatever the pictures cost. For a folder of
photographs that is the right order of magnitude by accident; for
`/usr/share/icons` -- 53 files, 25 KB, microseconds of real work -- it
meant thirteen seconds of cells filling in two at a time. A batch size
is a rate limit written as a constant, and the thing being limited was
never the bottleneck.

So there is no batch size now: a detached worker decodes one at a time
and posts each result back, and the queue drains as fast as the decoding
does. That is Nautilus's shape and Dolphin's -- neither runs on the UI
thread, and neither has a number to tune. Measured at 24 a second on TCG
for that directory, against a ceiling of 4.

The second half is the disk cache, and its one real decision is the
NAME. The freedesktop standard hashes the URI (MD5) and then has to
store `Thumb::URI` inside the PNG, because a hashed name cannot say what
it stood for and a collision shows the wrong picture. QOI has nowhere to
put that string, so copying the scheme would have meant either a second
file per entry or a format change. Writing the source path with `/` as
`%` gives the property the URI field exists to restore: the name IS the
answer, `ls` is the debugger, and two sources cannot collide. It costs a
bound -- a name longer than `sys_dirent`'s 64 bytes gets no cache file
and is thumbnailed in memory, which is the case a hash would have
handled.

Staleness is then "STRICTLY NEWER than its source", make's rule, rather
than a recorded mtime -- again because there is nowhere to record one.
Strictly, because mtimes are whole seconds: a source rewritten in the
second its entry was written would tie, and "not older" served the old
picture (thumbcache_test caught it, 2026-10-01). Git's index has the
same race ("racy git") and answers it the same way, by distrusting a
same-second entry; the cost is one extra decode. It is
worth more than the metadata version: a rewritten file overwrites its
own entry instead of adding one, so the directory is bounded by how many
distinct pictures have been looked at and not by how often they change.
Eviction is by age, oldest first, and that is an approximation stated
plainly: a cache HIT does not rewrite its file, so age is not use.
Recording a use would mean a disk write on every hit, which is the cost
the cache exists to avoid.

## Global shortcuts are the compositor's, and Super is a modifier that also acts alone

Nothing here was ever going to let an application grab a key. A Wayland
client cannot, deliberately -- one that could would be a keylogger -- so
the compositor matches every binding before routing, and toy-os already
had exactly that shape for Alt+F4. What changed is that the set became
DATA instead of a chain of `else if`s.

**They are registry settings, not a file of bindings.** KDE keeps
`kglobalshortcutsrc`, a `binding = command` file, and that buys arbitrary
custom commands. But System Settings here is registry-driven: a
registered setting gets a row, a label, validation and `config set` for
free, where a file would have needed a page written from scratch. GNOME
makes the same split and keeps its named actions in GSettings. The cost
is that the ACTION LIST IS FIXED in `kernel/lib/shortcut_actions.c`;
binding a key to an arbitrary command is a second mechanism and is on the
roadmap, not in this.

A value may name SEVERAL combinations, comma-separated, because one
action genuinely wants two keys -- the screenshot tool answers to
Shift+Super+S and to Print Screen, as Windows binds both. GNOME's
keybindings are arrays of strings for the same reason.

**The harder half is Super.** It used to open the Start menu on the
PRESS, which is fine for a key that only ever acts alone and impossible
for one that also modifies: holding Super to type Super+E would open the
menu on the way out of every shortcut. So Super is a modifier now
(`KEY_MOD_SUPER`), and "Super alone" became a compositor POLICY rather
than something the driver decides -- it acts on the RELEASE, and only
when no other key was pressed in between. Windows and KDE both behave
exactly this way. The driver no longer pushes `KEY_SUPER` into the byte
stream at all; it is a transition, like the other four modifiers, which
is also what stops Super reaching a shell.

**And a capture control needs the compositor to stand down.** Binding
Super+E means RECEIVING Super+E, which the compositor would otherwise
match and spend. `WIN_REQ_INHIBIT_SHORTCUTS` is Wayland's
`zwp_keyboard_shortcuts_inhibit_v1` under another name, and it is scoped
to the focused window and released automatically when that window loses
the focus -- so a client that crashes mid-capture cannot leave the
desktop with no shortcuts. Alt+F4 is deliberately NOT inhibited: it stays
the break combination, which costs the ability to rebind Alt+F4 itself
and buys a window holding the inhibitor always being closeable.

**What this keyboard cannot express, and why the Task Manager default is
not Windows'.** The driver drops Ctrl with anything that is not a letter,
and folds Ctrl+letter to a control code before anyone sees it. So
Ctrl+Shift+Esc can never be delivered -- it was the first default here
and could never have fired -- while Ctrl+Alt+Delete can, because Delete
is pushed before the Ctrl fold. `keycombo_matches()` undoes the fold so
that nothing above it has to know any of this.

## Notepad asks before throwing a document away, and CLOSING is not what it asks about

The obvious reading of "ask before closing" is an `on_close` handler,
and that is half the feature. The event worth confirming is not the
window going away -- it is the text going away, and File > New, File >
Open and picking a Recent file all do that just as completely as Alt+F4
does. Windows Notepad, gedit and Kate all ask on all of them.

So every one of those goes through one `confirm_discard()`, which either
runs the action at once (the document is clean) or parks it behind the
dialog. Three buttons, Qt's `QMessageBox::Save|Discard|Cancel` and every
editor's: Save, Don't Save, Cancel.

**The parking is the part that is not obvious.** The answer arrives
frames later through the widget router, so the action cannot be a return
value -- and Save on an UNTITLED document opens the file chooser, which
is a second wait stacked on the first. A close then completes only once
the bytes are actually on disk, and a cancelled chooser cancels the
close: quitting there would throw away exactly the text the dialog was
put up to protect.

The one that reads backwards until you hit it: **Don't Save must not
clear the dirty flag.** The actions that replace the document set it
themselves. The one that does not is Open, whose chooser can still be
cancelled -- and clearing the flag there leaves an edited document
marked clean, so nothing ever asks about it again.

## A modal owns the pointer's SHAPE too, and the toolkit resolves that rather than each app

**WITHDRAWN 2026-09-16, AND KEPT BECAUSE THE REASONING IS STILL RIGHT.**
The change this describes crash-looped `toywm` on the bare-metal laptop
with a general protection fault, cleanly A/B/A: good on the build
before it, crash-looping with it, good again with the three files backed
out. It never misbehaved in QEMU, where seven suites ran over it. The
MECHANISM IS NOT ESTABLISHED -- see `docs/bugs.md` -- so what follows is
the design to attempt again, not what the tree does today.

Making a dialog modal happened in three separate pieces, and the gap
between them is the interesting part. The click came first (the scrim
answers `hit`, so nothing behind it can be reached). The keyboard came
with it (`uui_dialog`'s `key` op returns 1 for every key). The CURSOR
was never wired, so an I-beam belonging to a text field nobody could
click sat over a modal whose entire purpose is that the app underneath
is untouchable.

The obvious fix is a predicate each app consults before naming its
cursor. It does not work, and the Terminal is why: it names the I-beam
ONCE in `on_open` -- the whole grid is text, as xterm does -- and has no
`on_motion` at all, so a predicate it is supposed to consult would never
be consulted. Any rule an app has to remember is a rule the app without
a motion handler cannot obey.

So it is resolved in the toolkit, in two halves. `uui_router_cursor()`
STOPS at an active overlay rather than falling through to the widgets
behind it -- the same precedence the press and the key already follow,
which is the rule this was missing rather than a new one. And `uapp`
keeps what the app ASKED FOR separate from what the compositor was
TOLD, resolving the two every frame that repaints. Per frame rather than
per motion, because a modal opening changes the right answer without
moving the pointer.

`WIN_CURSOR_WAIT` is exempt, and that exemption is not a special case so
much as a different kind of thing: a busy cursor is an OVERRIDE, not a
property of what the pointer happens to be over. Qt draws the same line
between `QApplication::setOverrideCursor()` and a widget's own cursor.
An app that starts a long operation behind its own dialog still has to
be able to say so.

What this buys is that Notepad was fixed without notepad.c changing, and
that the next app to draw its own region gets it right for free.

The Terminal is NOT evidence for that, and the measurement is worth
recording because the reasoning above invites the opposite claim: its
I-beam is already gone before any modal appears. `uapp` sets the cursor
from `uui_router_cursor()` on every motion, nothing claims a cursor over
the grid, so the first pointer movement inside the window replaces the
shape `on_open` asked for with the arrow. Measured with
`tools/predates.py` at HEAD and after this change: the I-beam is absent
in both, so it is a pre-existing bug (`docs/bugs.md`) rather than
anything this touched -- and it is why the Terminal could not be used to
demonstrate the fix.

## The Start menu is two columns with the search at the foot, and the power actions are not behind a folder

**Partly superseded (2026-10-01)** by "The Start menu is Kickoff's whole
shape", below: the search moved to a header and the power actions to a
labelled footer. What this entry says about two panes still holds; its
"no All folder, nothing scrolls" had already stopped being true.

The menu was one flat column: every app that showed in it, then the
three system actions. That is Windows 95's shape, and it scales exactly
as well -- nineteen entries made it 22 rows tall, alphabetical within
three groups nothing on screen named.

**What real systems do.** KDE's Kickoff is two panes -- a category
sidebar and the selected category's apps -- with a search field, and
XFCE's Whisker is the same with the field placeable at the bottom. KDE's
older Kicker made each category a row that flies out to the right, the
Win95 shape. Windows 7 put search at the FOOT with categories as folders
that expand in place; Windows 11 dropped folders for a pinned grid and
an "All apps" list; GNOME went to a full-screen grid. The two-pane form
is the one that stays legible as the list grows without needing a whole
screen, so toy-os follows Kickoff, with Windows 7's bottom-anchored
search because the field then sits directly above the Start button that
opened the menu.

**The power actions stay in the sidebar's foot rather than in a folder.**
Kickoff files them under a "Leave" tab, which makes shutting down two
clicks and a decision about which tab; Windows 7 keeps them in the
footer, one click from open. Mixing "select" rows and "act" rows in one
column is the cost, paid with a divider between them -- and the win is
that `Exit to shell` is exactly as reachable as it was before folders
existed, which also kept every tool that clicks it working.

**There is no "All" folder, and nothing scrolls.** An All folder would
be as tall as the old flat menu, which defeats the point, and a fixed
pane sized for it would leave two thirds of the popup empty for every
other folder. The search field is what answers "show me everything" --
it spans every folder, and it also matches the system actions, so
typing "shut" finds Shutdown the way it does on both KDE and Windows.
The pane is therefore as tall as the BIGGEST folder and a folder that
outgrows the screen is a real limit; a scroll region here would need a
wheel op on the overlay table and an offset in every one of the draw,
hit-test, keyboard and JSON paths, which is not worth buying before
something needs it.

**The keyboard reaches the menu through a new `key` op on the overlay
table** (`userland/wm/wm_overlay.h`), not through a special case in
`wm.c`. Overlays already shared draw/click/hover/press that way, and
typing was the fourth verb an overlay could not have. It is offered in
modality order like a click, and an overlay consumes only what it acts
on -- a key held with Ctrl/Alt/Super falls through -- because the
alternative is a global shortcut that silently stops working whenever a
popup is open. The context menu can take its keyboard the same way now
(`uui_menubar_key()` exists and has never been routed); that is left
undone rather than guessed at.

**Categories became freedesktop's names, single-valued.** `system | apps
| demos` described where the BINARIES live, not what the apps are, and
"apps" held eleven of nineteen. `Categories=` in a real `.desktop` file
is a semicolon-separated list and an app may appear in several menus;
that is deliberately not copied yet, because it buys nothing until
something genuinely belongs in two folders and it costs a variable-length
list in a fixed-size registry. An unknown key becomes its own folder
labelled with the key verbatim: the failure directions are asymmetric,
and an app nobody can find is far worse than an oddly-named folder.

## Favourites and Recent are one file keyed by AppId, and Recent is a sequence number rather than a clock

The Start menu grew folders before it had any notion of what a person
actually uses, so every app was equally far away. Kickoff opens on
Favorites and keeps Recent beside it; Windows 11's whole Start is those
two lists over a search box. toy-os follows, with the folders it already
had underneath.

**One file, `/etc/start-menu.conf`, holding both.** A pin is a
preference and a launch count is variable state, which the FHS would
split between `/etc` and `/var`. They are kept together because they are
read together, once, by one reader at startup -- and because a menu
whose favourites and history disagreed about which apps exist would be
worse than either living in the tidier directory.
`/etc/desktop.conf` is the precedent: the desktop's own state, in its
own file, beside the settings rather than inside them. The settings
registry was considered and rejected for the counter specifically: a
value that changes on every launch is not a preference, and writing it
through the settings path would put it in front of System Settings as
something to edit.

**Keyed by AppId.** A name can be edited and a row moves on every
reload -- the registry re-sorts by `(category, name)` each time -- so
both are the wrong handle for something persisted. `AppId` is the
`.desktop` entry's own stable identifier, already what the window
protocol matches a client on. The desktop's icon positions are keyed by
Name and pay exactly this price: rename an app and its position is lost.

**Recent is ordered by a SEQUENCE NUMBER, not a timestamp.** "Recent"
needs an ORDER and nothing else, and an order is all a counter gives:
each launch takes the next number, so the newest is the largest. That
works on a machine with no RTC, before NTP has run, and across a
timezone change -- none of which a wall clock survives, and this desktop
has all three. The count is kept beside it because a "frequently used"
view would need it and nothing else would have to change.

**The list is a folder, not a grid or a tab.** Kickoff makes Favorites a
TAB and Windows 11 a pinned grid, both of which need a second layout;
here they are two more rows in a sidebar that already existed, showing
their apps in the column that already existed. What that costs is the
distinction between "pinned" and "a category" being only the icon and
the position -- and what it buys is that pinning changed no layout code
at all.

## A pinned app is a toggle with two names, and the Start menu stays up under it

`Pin to Start` becomes `Unpin from Start` on an app that is already
pinned, rather than a row with a tick beside it. A ticked "Pinned" makes
the reader work out what clicking would do; the verb says it. Windows
and KDE both word it this way.

The menu is NOT dismissed by it, which is the opposite of what a launch
does. Pinning changes what the menu SHOWS -- the sidebar gains a
Favourites folder on the next frame -- and closing the menu would hide
the only feedback the action has. That is the same rule the desktop's
own context menu follows for a non-launching verb, and it is why
`ctx_toggle_pin()` damages the menu instead of closing it.


## The description is elided AND tooltipped, because those answer different questions

A `Comment=` longer than the menu is wide was silently cut: the strip
stopped mid-sentence and looked like a shorter, complete one.
`ugfx_draw_string_clipped()` has always RETURNED whether it cut, with a
comment telling callers to "shorten, ellipsise", and two places in the
tree already did -- the desktop's icon captions and `uui_label`'s last
wrapped line. The strip was simply the odd one out, and the fix is
`ugfx_draw_string_elided()` in the toolkit: its third caller is what
earns it a place there rather than a third copy.

**The mark is `..`, not an ellipsis character.** The font is indexed
from ASCII 32 (`kernel/drivers/font_ttf.c`), so U+2026 draws as nothing
-- an elided line would be indistinguishable from a complete one, which
is the exact failure being fixed. The desktop's captions worked this out
first; it is now written once where every caller can reach it.

**And eliding does not answer "what does it say".** So the panel grew a
tooltip: hover a row, wait, read the whole thing. Four options were
weighed -- eliding alone, shortening the shipped descriptions to fit,
a permanently two-line strip, and widening the menu to its longest
description. The last two make every user pay, in height or width, for
one wordy entry; the second is worth doing anyway but cannot help an
entry we did not ship.

**Always on hover, not only when truncated.** Explorer shows a tooltip
only for text it had to cut, which is the tidier rule and the wrong one
here: the description is the only place an app says what it IS, and it
is worth reading whether or not it fit.

**It takes no input at all** -- no grab, no hit region, and a click op
that always returns 0. A tooltip that consumed the click the person was
about to make is how this goes wrong everywhere, and the check for it is
in `start_menu_test.py`: clicking a row the tooltip covers must still
launch that row.

**Why the panel needs its own rather than `uui_toolbar`'s.** That one is
right for a ring-3 app: the widget rides the app's tick and opens a
popup SURFACE the compositor places. The panel draws its overlays
straight into the compositor's buffer and has neither a widget tree nor
surfaces, so what carries over is the RULES -- the delay constant, the
no-grab rule, re-arming on a move -- and not the code.

## A tray icon answers a hover and a press, and nothing latches

Every tray item fills with a rounded `uui_state_bg(bar,
UUI_STATE_HOVER)` while the pointer rests on it and
`UUI_STATE_PRESSED` while the left button is held on it -- Windows 11's
and Plasma's rule, and the one the strip's window buttons follow.

**It was press-only until 2026-09-29**, the macOS menu-bar rule, at the
maintainer's request -- argued at the time on the tray being a strip
the pointer passes THROUGH. The maintainer reversed it with the taskbar
redesign: once every other control on the strip lights on hover, a tray
that alone does not reads as dead rather than as quiet.

**Nothing latches, and that cost a decision.** Lighting the item while
its popup is open is the more informative behaviour and is what Windows
11 does. It was dropped because it makes the fill mean two things --
"your press landed" and "this flyout is the open one" -- and the second
is already said by the flyout itself being on screen, anchored to that
item. The check for it is the last pair in `tray_press_test.py`: the
calendar open, the pointer gone, the clock back at rest.

**Where it lives, and why not in each popup.** `wm_tray.c` owns the
hovered and pressed ids and applies them inside the single right-to-left
walk that draws the strip, so the clock, the flyout items, the
on-screen keyboard and anything registered later all get it with no
code of their own. The alternative -- each popup marking its own item --
is six call sites to keep in step and a seventh to forget, which is the
failure `wm_overlay.h`'s registry exists to prevent.

**And the fill is a shape, not a band.** It spans the strip's button
box (`btn_y`/`btn_h` from `taskbar_geom()`) with the buttons' corner
radius, so a tray item lights exactly as a window button does; a
full-height block reads as a section of the bar rather than as a
control. The rasteriser is `uui_fill_round_rect()`.

## The taskbar's layout is four independent settings over one layout function, and a floating panel deflates rather than the band moving

`desktop.taskbar_buttons` is `labelled` | `icons`, `desktop.taskbar_align`
and `desktop.start_position` are `left` | `center`, `desktop.taskbar_float`
is `off` | `on`, and `desktop.taskbar_theme` is `dark` | `light`. The
maintainer chose from three mockups -- labelled buttons (Windows 10 /
Plasma's labelled task manager), centred icons (Windows 11), a floating
panel (Plasma 6) -- with the first as the default.

**Independent settings, not a list of styles.** It shipped as one enum,
`taskbar_style` = classic | centered | floating, and every request after
it was a combination the enum could not name: floating WITH icons, icons
on the LEFT, Start in the corner beside centred icons. The maintainer
asked for the controls to be separate, which is Plasma's own shape --
"icons only" is a task-manager option, floating a panel property, the
launcher a widget placed anywhere. Windows 11 exposes only alignment,
and moves Start with the icons; that is one of the combinations here.
Every combination is valid, including a centred Start over left-aligned
buttons, which sits alone in the middle with the buttons clear of it.

**A layout is measurements, not a second taskbar.** One
`taskbar_layout()` places the buttons for every combination and one
`draw_taskbar()` draws them (branching
once per indicator: an underline, a pill, a tinted frame), and both ask
`taskbar_geom()` for the panel and button rects. The obvious shape --
a draw-and-hit-test pair per look -- is several copies of the thing
`wm_taskbar.h` exists to keep single: the strip once ran off the screen
because three walks of the window list disagreed.

**The band never changes with the panel's shape.** `taskbar_h` is the
bar plus `TASKBAR_FLOAT_GAP` while the panel floats, and a floating
panel DEFLATES to fill that band while any window is maximized -- the
case where a gap would be a strip of wallpaper between a window and
the bar. Plasma 6 does the same. The alternative, reserving only the
panel and growing the work area when it floats, would re-lay every
maximized window each time one was maximized, which is the moment it
matters least.

**Hit-test against the band, draw inside the panel.** The gap under a
floating panel is the strip's, not the desktop's, and the screen's
bottom edge hits the button above it -- a click thrown at the edge
should land, which is why Windows and KDE both do it. The Start button
owns the screen's corner whenever it is leftmost.

**The theme is the strip's own**, not the window theme's: Windows and
KDE both let the panel be dark over light windows, and this desktop's
strip was already dark over light chrome. The accent follows utheme's,
lightened on the dark strip so a three-pixel indicator still reads; the
hover tint is `uui_state_bg()`, as every control's is.

**The strip lists in OPEN order.** `windows[]` is z-order and a click
raises, so listing by index moved a button to the end of the strip every
time it was clicked -- which Windows and KDE never do. `struct
window.open_seq`, stamped where a window is created, is the order.

**Not built: Windows 11's grouped network-and-volume button.** The
centred mockup showed the two as one target; this desktop has one
flyout per tray item and no combined Quick Settings panel to open, so
the tray stays per-item in every layout (`docs/roadmap.md`).

## Taskbar peek is content thumbnails, rescaled on present, held by open order

Resting on a taskbar button shows a card of the window (Windows 11's
preview, Plasma's task tooltip): `desktop.taskbar_peek` = `off` |
`preview` (default) | `highlight`, the last dimming the desktop under the
window while the pointer is on its entry (Plasma's "Highlight windows",
Windows' Aero Peek). The maintainer chose Windows' card shape -- title,
close, thumbnail -- from mockups over Plasma's thumbnail-then-title,
because the close button is the action a preview is most often used for.

**Content, not chrome.** The thumbnail is the client's buffer, not the
framed window `wm_anim.c` snapshots for its ghosts: a title bar scaled
to a fifth is a stripe of unreadable text, and the card has its own
title row.

**Box-averaged once, not scaled per frame.** `uimg_scale()` reads every
source pixel, which is what keeps a thumbnail legible; the nearest-
neighbour blit the animations use is per-frame cheap and turns text to
noise. So the scaled copy is CACHED, marked stale by the client's
present, and rescaled at most every 200 ms -- a game presenting at
60 Hz costs five rescales a second while its card is up, and nothing
while it is not.

**Held by `open_seq`, never by index.** The card outlives clicks and
raises, both of which reorder `windows[]`.

**Highlight redraws the window rather than masking around it.** The
scene is drawn with the lifted window skipped, dimmed, and the window
drawn once more on top through the same `draw_one_window()` -- so the
dim and the lift obey the frame's damage clip like everything else,
and a minimized window can be lifted where it would be.

**A client buffer's alpha byte is zero, and the scaler now knows an
opaque image when it sees one.** `uimg_scale()` weighted colour by
alpha, so a `0x00RRGGBB` window scaled to black; `has_alpha == 0` --
already defined as "every pixel is opaque" -- now reads alpha as 255.
Decoded images carry 255 there anyway, so their results are unchanged
(`uimg_hostcheck.py`, `uimg_codec_hostcheck.py`).

## Taskbar buttons drag to reorder, live, and a drop commits a rank

A press on a taskbar button arms it; a release on it activates the
window, as Windows does -- the press no longer acts, because a press
that acts cannot become a drag. Moving 4px first (Windows'
`SM_CXDRAG`) starts one: the button follows the pointer and the others
glide aside to open the slot it would land in, the maintainer's choice
from mockups (Windows 11 and Plasma 6 both do this; the alternative,
an insertion marker with nothing moving, is a tab bar's). A file
dragged across windows (`wm_dnd`) resting half a second on a button
brings that window forward, which is the other half of dragging onto
a taskbar in both desktops.

**The live row is the layout's, not the renderer's.** `apply_drag()`
runs inside `taskbar_layout()`, so what is drawn, what `gui taskbar`
reports and what the drop commits are one answer -- the rule the
layout function exists for.

**Order is `task_rank`, identity is `open_seq`.** Two fields, because
they answer different questions: peek and the glide follow a window
across reorders by `open_seq`, and the strip sorts by `task_rank`. A
drop re-ranks every listed window from the same counter new windows
draw from, so ranks stay unique and a window opened later still lands
at the end.

**The order lasts the session.** Windows keeps a taskbar order only for
pinned apps, and toy-os has no pinned taskbar apps; remembering an
unpinned app's slot across restarts would be inventing the half of
pinning that is not visible. When pinning exists, its order is the one
to persist.

**The glide is time, not frames**: `utween_retarget()` per button keyed
by `open_seq`, 150 ms, so a slot that changes again mid-glide continues
from where the button is. `desktop.animations` off makes every move a
jump.

## The greys are a ladder with the page in the middle, macOS/Windows 10 spacing, not Plasma's

The default palette's neutral rungs, brightest first: `field_bg` (a
text box, a list) sits well above the page; `panel_bg` is the page an
app clears its window to; `bar_bg` (menu bar, status bar), `control_bg`
(a button face, a scrollbar track) and `window_bg` (the frame fill,
the tab-strip and scroll-view ground) are each a step BELOW the page;
`tab_rest` a step below those. A field is the thing that reads as
raised; chrome and faces read as recessed.

**Why the page moved down.** The page had been 245 against a 255
field -- a ten-level fill delta, weaker than any shipping light theme
(Plasma 239/252, Windows 10 240/255, macOS 236/255) -- and the
maintainer read a text box as barely a box. The ground is 236 now, the
macOS value, with every rung below it moved by about the same amount
so the ordering is unchanged.

**Why faces stay DARKER than the page rather than flipping to Plasma's
shape.** Plasma and Windows 11 draw a button lighter than its window
and give it an outline; this toolkit's buttons are flat fills with no
outline, so a lighter face would need a border drawn on every button
to keep its edge, and every button on the desktop would change
character for a change that was asked of the ground. macOS and
Windows 10 keep the face a step below the window, which is the shape
already here, so the fix moved the rungs and kept the shape.

**Why the change was a ladder edit and not one number.** Four widgets
had copied a rung as a literal: `uui_label` and `uui_slider` carried
their own 245, `uui_menubar` and `uui_statusbar` their own 235, and
the disabled wash blended toward a literal 235. Moving `panel_bg`
alone left labels as lighter boxes on every page and the menu bar
level with the page. Those default to `UUI_COLOR_UNSET` and resolve
at draw now, the bars through a `bar_bg` role that did not exist
because nothing had needed it to -- two callers made it one.

## Smooth scrolling is a displacement that eases to zero, not an animated position

Every scrolling widget glides ~150 ms after a wheel notch, a trough
click or a key that moves the view, through one helper
(`ui/uui_scrollanim.h`) over one tween (`lib/utween.h`). The widget's
own position -- `uui_table.top`, `uui_scrollview.offset`,
`utext.scroll_offset`, the icon grid's pixel offset -- still JUMPS to
its destination exactly as it did before; what changes is that the draw
adds a displacement to every content y, starting at "where the content
was" and easing to zero, and folds the same displacement into a
pixel-unit scrollbar so the thumb moves with the rows.

**Why not animate the position.** The obvious design tweens `top`
itself, which means every row widget switching to a pixel position with
a sub-row remainder, and every reader of the position -- `fm_view`'s
layout report, System Settings' sidebar log, UI Demo's status line,
every test that asserts a `top` or an `offset` -- learning about the
motion. A displacement touches the draw and nothing else: hit-tests
subtract it, the bar folds it in, and everything that asked "where is
the view" keeps getting the answer it always got. Qt's item views
animate a per-item scroll the same way (the scroll position is items,
the motion is pixels), and Explorer's list view scrolls by item with a
pixel glide over it.

**Why the widget arms it rather than the helper detecting a change.** A
position changes for reasons that are not the user scrolling: a
directory reload that shortens the list, a resize that re-clamps, a
selection restored by name. Gliding those reads as the content lurching
of its own accord. So the wheel, the trough and the keys say "the next
change is a scroll" and anything else draws where the position says,
at once. A thumb drag cancels outright: gui-guidelines rule 2 is that
the thumb follows the cursor 1:1, and an eased thumb under a held
button feels like a loose control.

**Why the tween takes its clock as an argument.** `utween.c` compiles
with a host gcc for `tools/utween_hostcheck.py`, which is what caught a
rounding overshoot (100 -> 0 visited -1 for a frame) before any guest
ran it. The same property lets the window manager use it later without
a second copy -- the roadmap had held the helper back until a second
real caller existed, and this is it.

**Why the frames come from a request flag, not a timer.** Toykit draws
on events and blocks; an animation has none. A widget still moving asks
for one more frame from its draw (`uui_anim_request()`), and uapp's
pump waits one frame instead of its long park while one is pending. So
a process wakes at ~60 Hz only while something moves and sleeps as
before otherwise, with nothing to register or unregister -- a widget
destroyed mid-glide leaves no dangling entry. A TWS timer would have
been a round trip per frame to say "yes, again".

**Why a registry setting.** `desktop.smooth_scroll` is read at each
`arm()`. Smooth scrolling is toolkit-wide, so it is the SYSTEM's knob
and not an app's (`docs/conventions/gui.md`, "AN APP'S OWN PREFERENCES
ARE THE APP'S"); Windows and GNOME expose the same switch as "animation
effects", macOS as "reduce motion". It lives on a new Appearance >
Effects page, where the window effects' toggles will join it.

**Why `utext` gates it behind `animate`.** `/bin/edit` draws once per
keystroke through the same text buffer; with the glide on there, a
displaced frame would sit until the next key. `uui_textview` and
Notepad turn it on because Toykit redraws them per frame.

## Shadows are a cached corner tile and a falloff table, drawn under the window, and part of its damage

Every toplevel casts a drop shadow (`userland/wm/wm_shadow.c`): larger
and darker when focused, smaller when not, none when maximized or
fullscreen; every popup -- a Toykit menu, the context menu, the Start
menu, a tray flyout -- casts a small one. `desktop.shadows` turns them
all off.

**Why no blur.** Mutter renders one blurred rounded rect per size class
and 9-slices it; Plasma ships a tile set. Neither blurs per frame, and a
software compositor with a 7 ms full frame on the laptop cannot afford
to either. Here the falloff past an edge is a quadratic table and each
corner a tile of `(radius + corner radius)^2` alphas computed once per
kind from the signed distance to the arc, mirrored to the four corners.
A shadow costs a perimeter band of blends and no square roots per
frame. Under TCG that band was measured at ~12 ms a damage-limited
frame for a 510x379 window; the laptop figure is the one that matters
and is what `gui compositor` reports.

**Why it is drawn BEFORE the window, not composited after.** The
rounded corners are cut by `corners_save()`/`corners_round()`, which
save what is beneath the corner and blend it back through the arc. With
the shadow already in the scene, what is beneath is shadow, so the cut
reveals a shadowed corner rather than bare desktop. Drawing the shadow
after the window would need a second pass under the arcs.

**Why the focused shadow differs.** Mutter, DWM and macOS all draw the
active window's shadow larger and darker; it says which window is up
without reading a title bar, which matters most when the title bars are
the same colour. Plasma's default strength is 50%, which is the
focused alpha here; the inactive one is smaller and lighter as Plasma's
inactive shadow is.

**Why the shadow is part of the damage rect.** A shadow is pixels
outside the window's rect, so the invariant `docs/gui-guidelines.md`
calls "the damage invariant" now reaches `wm_shadow_margin()` past the
frame. Rather than pad at each of the ~30 sites by hand, the sites that
damage a WINDOW's rect call `wm_damage_window_rect()`, which pads; the
ones that damage part of a window (a title bar) or something else (the
drag outline) do not. `window_intersects_damage()` grows the same way,
so a damage box that touches only a shadow still repaints the window
that casts it. `tools/damage_sweep.py` ran clean over 43 interactions
with the shadows on and its positive control still fired.

**Why the radii are line heights.** Plasma's medium shadow is about a
title bar tall at the default font; a fixed pixel radius would be right
at exactly one font size (`docs/gui-guidelines.md`, "Size everything
from the font").

## Window animations are ghosts: one snapshot, scaled and faded, in place of the window

A toplevel opens by scaling in from 92% while fading in, closes by
scaling out while fading, minimizes by shrinking into its taskbar
button and comes back from it -- 150 ms, ease-out, `userland/wm/wm_anim.c`.
`desktop.animations` turns them off. Maximize and restore-from-maximize
are not animated yet.

**Why a snapshot and not the live window.** DWM and KWin animate a
window's TEXTURE: the compositor already holds every window as one, so
scaling and fading it is a transform. Here a window is painted
procedurally into one back buffer -- chrome by `draw_window_chrome()`,
content by a blit -- and there is no per-window layer to transform.
Rendering the window once into a buffer of its own
(`wm_render_window_into()`, which points `wm_surface()` at that buffer
for the duration) gives it the texture DWM has for free, and every
animation is then the same operation: `ugfx_blit_scaled_alpha()` of
that buffer at a tweened rect and alpha. It also makes the ghost
INDEPENDENT of the window: a client that has exited still shrinks away,
because the ghost owns its pixels and `windows[]` can compact under it.

**Why the live window is hidden while its ghost runs.** An open or a
restore has a live window at the destination and a ghost on the way
there; drawing both would show the window pop in at full size under a
fading ghost. `wm_anim_hides()` keeps the window out of the scene until
the ghost lands, identified by (pid, window id) rather than by index,
since indices move.

**Why the starters run BEFORE the state changes.** The snapshot reads
the window's buffers and geometry as they are; `on_window_destroyed()`
unmaps the buffers a line later, and a minimize is skipped by the
scene the frame after. So every site calls the starter first. The open
animation starts at the FIRST PRESENT rather than at creation, for the
same reason: at creation there is nothing to snapshot.

**Why nearest-neighbour scaling and a constant alpha.** The ghost is on
screen for 150 ms; a bilinear filter would cost a multiply-add per
channel per pixel for a difference nobody sees in nine frames. The
source's own alpha byte is ignored: a window has none.

**Why the frames come from the WM loop's wait.** The loop parks up to
100 ms waiting for input; a ghost in flight caps that at
`WM_ANIM_FRAME_MS` and `wm_anim_step()` sets `redraw_pending` while it
moves, so the compositor runs at ~60 Hz only while something animates.
It is the same shape the toolkit uses for smooth scrolling.

**Why `gui state` reports `anims`.** Fifty GUI tools compare settled
frames. A frame captured mid-ghost is neither the before nor the after,
so `DebugConsole.settle()` waits for `anims` to reach zero exactly as it
waits for `pending` -- without it the suite would flake on every open.

## The client exports its widget map; the compositor cannot work it out

`gui probe X Y` could name a window, its title bar, a resize edge and
which overlay was in the way -- and then had nothing at all to say about
the control actually under the pointer, which is the thing a GUI test
wants. So every tool that needed one measured screenshots or scraped the
layout log, and 26 of them hand-parse those `: layout ` lines today.

**This is not an oversight in the compositor, it is the shape of the
system.** A client hands over pixels; its buttons and lists are its own
business, and nothing in the protocol ever told the window manager
otherwise. X11 gets widget geometry free because a widget IS a window
and `XQueryTree` walks them. Wayland cannot and does not: a compositor
there knows nothing about widget structure, which is why GNOME and KDE
expose it over AT-SPI, where the CLIENT exports its own accessibility
tree and something else reads it. Windows UI Automation inverts that --
the automation client calls into the provider and waits.

**The client pushes, because there is nothing to pull with.** Events
flow compositor -> client one way and carry no reply channel; the only
round trip in this protocol is client -> compositor (`uchan_call`).
Even the close "veto" is not transmitted -- the WM infers a refusal from
the ABSENCE of a destroy, disambiguated from a hung client by a
concurrent ping. Adding a WM-initiated question would have meant either
a new `WIN_EV_*`/`WIN_REQ_*` pair correlated by a serial (the
PING/PONG pattern, the only precedent) or a second reply slot in
`uchan_page.h`. Both are real machinery for debug data. The client
already knows its rects, so it sends them: `WIN_REQ_WIDGET_RESET` then
N × `WIN_REQ_WIDGET`, and the compositor keeps the set per window.

**Sent only when the map CHANGES.** The toolkit walks its named items
every frame -- a few dozen pointer dereferences and no formatting -- and
compares the result against what it last sent, so the messages go out on
a resize or a layout swap and not otherwise. That is what keeps it off
the per-frame cost of an app that redraws constantly.

**And deliberately NOT through the log.** The string-building layout log
(`uapp_log_layout()`, `desktop.layout_log`) stays exactly as it was:
opt-in, and called by the four apps that call it. Routing the map
through the log instead would have made `gui widgets` depend on a
diagnostic setting -- and that setting had been left ON on the
bare-metal laptop by a test tool, which turned one boot's persistent log
into 183 KiB of layout lines, eleven times its neighbours. A debugging
facility that floods the evidence is the wrong trade twice over.

**A dropped message costs an out-of-date map and nothing else.** It does
not round-trip and nothing decides anything on it; the next layout
change replaces the set. `WIN_WIDGET_MAX` bounds one window's map at 48,
and the allocation is made on first report rather than living in every
`struct window`, because most windows never report one -- a
kernel-space app has no toolkit behind it.

## Five mouse buttons, and the app decides what the extra ones mean

The kernel masked pointer buttons to three (`mouse_feed_buttons()`,
`& 0x07`), and a KTEST asserted that truncation. Above it the window
manager had edge detection for exactly two: bit 0 and bit 1. So a middle
click travelled the whole way up from the driver and was dropped by the
compositor having never reached a client, and the thumb buttons were
discarded three layers below that.

**The numbering follows evdev, not X11, and it was free.** X11 numbers
pointer buttons 1-3 left/middle/right and then spends 4-7 on the WHEEL,
which is why a thumb button is button 8 there and why every X11 app
carries that table. Wayland does not inherit it: `wl_pointer` carries
evdev codes and scrolling is a separate `axis` event. This protocol
already made that choice -- the wheel has been `WIN_EV_WHEEL` since it
existed -- so bits 3 and 4 were never spoken for and `WIN_MOUSE_BTN_SIDE`
and `_EXTRA` sit where Linux puts `BTN_SIDE` and `BTN_EXTRA`. Nothing had
to be renumbered and no ABI changed: `WIN_MOUSE_BUTTONS()` already masked
the low byte with the modifiers shifted clear of it.

**They are named for the POSITION, not for "back" and "forward".** What a
thumb button means is the application's decision, and the three systems
worth copying all agree: Wayland delivers an evdev code, Windows delivers
`WM_XBUTTONDOWN` with `XBUTTON1`/`XBUTTON2`, Qt exposes
`Qt::BackButton`/`Qt::ForwardButton` -- and in every case it is the
application (Explorer, Dolphin, a browser) that reads them as
navigation. A compositor that hard-coded "back" would be wrong in a
paint program, and one that synthesised Alt+Left instead would be lying
to the client about what happened.

**So the compositor delivers and does nothing else.** A thumb or middle
press goes to the client under the pointer -- by POSITION, which is both
Windows' rule and Wayland's -- and does not raise, focus, drag or touch
the chrome. Navigating back inside a window you are already looking at
should not reorder the desktop.

**The PS/2 driver has to ask twice.** A 5-button PS/2 mouse is Microsoft's
IntelliMouse Explorer, reached by a SECOND sample-rate knock
(`200, 200, 80`) after the wheel one, and it changes the packet: at
device ID 3 byte 3 is a signed 8-bit wheel count, at ID 4 it is a signed
FOUR-bit count with the two buttons in bits 4 and 5. Decoding an ID 4
packet with the ID 3 rule reads a thumb press as a wheel spin of -16,
which is why the two decode paths are split rather than shared.

**USB HID leans on a fact outside the spec, and that is stated rather
than hidden.** This driver has no report-descriptor parser, so it runs
boot protocol, whose mouse report formally defines three buttons. Every
real 5-button mouse puts the extra two in bits 3 and 4 of that same byte
and reports them in boot mode anyway. A device that does not simply
never sets the bits, so the cost of being wrong is zero -- but it is an
assumption about hardware rather than a guarantee from a document.

**virtio-input was worse than truncating.** Its range test ended at
`BTN_MIDDLE`, so codes `0x113`/`0x114` fell past it into the key branch
and were delivered as KEYBOARD keycodes -- a thumb click injected a
keystroke no keymap names. Found while widening the others.

## The mixer's per-application sliders are keyed by application NAME, and the daemon publishes who is playing in its beacon

The tray flyout had one slider because `soundd` mixed every client at
one gain. Making it per-stream needed two things the system did not
have: something stable to key a remembered volume on, and a way for the
panel to know what is playing.

**A stream's identity is its application's name, not its pid.** A
client's ring is called `snd.<pid>`, which means nothing next boot, so
a volume keyed on it could never be remembered -- and a mixer whose
settings evaporate is one nobody uses twice. The client writes its own
process name into the control page before the daemon may look at the
ring (`SND_APP_MAX`, and deliberately `PROC_NAME_MAX` rather than a
size of this ABI's own). PulseAudio keys `application.name` and the
Windows Volume Mixer keys the executable; neither keys a process id,
for this reason. The cost is honest and visible: two copies of one
program share a slider, and an unnamed client gets none -- it is shown
by pid and mixed at full gain, because `""` is every unnamed client's
key and one slider moving several programs is worse than no slider.

**The gains are a CONFIG FILE, not settings.** The registry is a fixed
catalogue of build-time knobs, each of which renders a System Settings
row; these keys appear one per program ever played, and could not be
registered at runtime even if a row each were wanted. `/etc/sound.conf`
is its own file rather than keys in `/etc/toyos.conf` for the same
reason -- an arbitrary program's name does not belong in the system's
own config. Both sides use the shared parser (`uconf_*` in ring 3),
which is what stops the panel and the daemon disagreeing about what the
file says.

**The panel reads the roster out of the daemon's BEACON.** That object
already existed as the "a daemon is running" rendezvous and carried
nothing; it now carries who is being mixed and at what gain. The
alternative was for the panel to open each client's own ring to read
its name, which would make the shell a reader of every application's
audio buffer for the sake of a label. The daemon is the only writer,
the page is mapped read-only, and a torn read is tolerated rather than
locked out -- a generation counter bumped either side of the rewrite,
and a reader that catches the middle looks again. A slider drawn one
frame late is not worth a lock in a page a dying daemon can leave
behind.

**The taper is the CARDS', not `usnd`'s.** A digital gain could have
been linear in amplitude, which is what `usnd_set_volume()` still does,
but the per-app slider sits in the same panel as the master and two
sliders that differ at the same position is a bug a user reports as
"the volume is wrong". Both are 40 dB, linear in dB, so 25% is 30 dB
down on either. That leaves `usnd`'s own knob as the outlier, which is
the open "one volume taper for every card" roadmap item rather than
something this change fixed.

## An overlay's damage is checked where it MOVES, not where it draws

A tray popup is anchored to its tray ITEM, and the tray's layout is not
fixed: hiding or showing an icon shifts everything left of it by that
icon's width, and the clock is re-measured every second in a
proportional font. So the panel's rect changes with nobody touching it,
it paints at the new anchor, and the pixels at the old one stay on
screen. Measured on the bare-metal laptop as a 46 px jump (one tray
item) and a 1 px one (a clock digit), reported by the WM's own damage
verifier as ~3 violations per 30 seconds of IDLE with a panel open.

**THE CHECK RUNS LAST IN THE FRAME, IMMEDIATELY BEFORE RENDERING, AND
THAT POSITION IS THE WHOLE FIX.** The obvious home is beside the other
`*_poll_config()` calls, and there it NEVER FIRED ONCE: the things that
move an overlay run late. `tray_update_clock()`, `remote_poll()` and
`network_poll()` sit in a once-a-second block after input handling, so
a tray popup's rect changes after every other poll has already run. A
check placed earlier reads the OLD layout, compares it against the old
drawn rect, finds them equal, and concludes nothing moved -- which is
indistinguishable from working.

**IT COMPARES AGAINST WHERE THE OVERLAY LAST DREW**, the `g_drawn`
record the overlay core already keeps for `wm_overlay_damage()`, and
damages BOTH rects: the one being vacated and the one being taken.
Damaging only the new rect leaves the old pixels exactly as they were,
which is the bug.

**WHY NOT MAKE THE TRAY ANNOUNCE ITS OWN RELAYOUT.** It would be
narrower and would need every future mover -- a resolution change, a
font change, a taskbar that grows -- to remember to announce itself.
Asking each open overlay where it is now is one comparison per open
overlay per frame, of which there is almost never more than one, and it
cannot be forgotten by something added later.

**WHAT MADE THIS EXPENSIVE, and it is a testing lesson rather than a
GUI one.** The fault does not reproduce under QEMU at all: it needs
tray icons that come and go, and the laptop has one that lights up for
REMOTE ACTIVITY -- so driving the machine to observe it was itself
creating it. And with shadows ON the verifier's first differing pixel
is the shadow's faint outer edge, a one-or-two-point blend difference
that reads as "composited twice" when the real fault is a whole panel
46 px from where it was. `config set shadows off` before reading those
colours.

## System Settings follows changes made elsewhere, except on a page with edits pending

A setting can change while System Settings is open: the tray toggles
one, `config set` writes one, another program applies its own. Settings
used to read the registry at startup and after its own Apply only, so an
open page went on showing a value that had changed underneath it. A test
found it (the Effects page kept offering the old effect's options), and
a person would too.

**What real systems do.** GNOME Settings applies every change at once,
so it simply mirrors dconf's change notifications. KDE's System Settings
stages edits behind Apply, like this one, and reloads a module when
`KConfigWatcher` reports its file changed. The classic Windows control
panels mostly do not follow outside changes at all.

**What toy-os does.** KDE's shape, since this window stages. Twice a
second it compares the settings registry's generation -- one cheap call,
the same one the WM uses -- and when it has moved it re-reads the
registry and redraws the open page. **A page with edits pending is left
alone**: rebuilding it would throw the user's staged values away, so it
is marked stale and refreshed as soon as those edits are applied or
cancelled. The same holds while an options dialog is open over it.

## System Settings is cards under a searchable sidebar, and leaving a changed page asks

Rebuilt 2026-09-28, from mockups the maintainer chose between. The page
had been a column of bold captions, grey descriptions and bare controls,
with every choice suffixed "(current)", square radio markers, OK / Apply /
Cancel bottom-left and a status bar repeating the page's name; the
sidebar was a flat list whose categories showed only as rules.

**What real systems do.** Windows 11 Settings, GNOME Settings (libadwaita
boxed lists) and KDE Plasma 6 (FormCard) all draw a setting as a ROW: its
name and a line of description on the left, its control on the right, a
toggle for anything on/off, and search at the top of the navigation.
Windows, GNOME and macOS apply every change at once; KDE's System
Settings STAGES behind Apply, with Defaults and Reset beside it, and asks
Apply / Discard / Cancel when you leave a module with changes.

**What toy-os does.** The row look of all four, and KDE's model for
changes, which this app already had (the decision above on following
outside changes depends on it). Concretely:

- each setting is a `uui_setting_row` card, and its control is picked
  from what its values ARE -- a switch for an on/off pair, a segmented
  control for a few short names, a list inside the card for a few long
  ones, a dropdown for many -- rather than from a count alone;
- the footer counts what is staged ("2 changes not applied") and holds
  Reset and Apply, live only while something is staged; OK and Cancel
  went, because closing the window is the cancel and leaving asks;
- leaving a page, or closing the window, with a change asks Apply /
  Discard / Cancel. The old rule discarded silently with a status line,
  which is how a user learns not to trust the app;
- the sidebar is a heading per category with its pages under it, and a
  search box above it filters by page name, category, and each setting's
  label, description and choice names -- the words a person searches by;
- one focus ring covers the window: search, sidebar, the page's controls,
  Reset, Apply; Tab scrolls the page to the card it focused.

**Rejected.** Instant apply, which would reverse the staging this app
and its outside-change handling are built on, and needs a revert timer
for the settings that can lose the screen (resolution) -- KDE's model is
the one that is safe without one. A home page of category tiles (Windows
11, GNOME) was offered and not chosen; search in the sidebar answers the
same question without a second navigation level.

## Ctrl and Alt reach a window as bits, and a game reads keys by position

The keyboard driver encoded Ctrl and Alt for a terminal -- Alt+key as
ESC then the key, Ctrl with a non-letter dropped -- and a window got the
same bytes. Doom plays with Ctrl (fire), Shift (run) and Alt (strafe)
held, so firing while pressing 1 switched no weapon, and strafing while
pressing Space (use) was an Esc press that opened the menu and never
came up. Every GUI app also saw Alt+x as an Esc that cancelled whatever
dialog was open.

**Two changes, one per kind of reader.** Real systems separate them:
SDL has scancodes beside its text-input event, Windows WM_KEYDOWN
beside WM_CHAR, Wayland `wl_keyboard.key`'s evdev keycode that the
client translates with xkbcommon, and X11 a keycode plus a state mask.
None of them lets a modifier change which key is reported.

1. **A window gets the key with its modifier bit.** Ctrl+1 is '1' with
   `KEY_MOD_CTRL`, Alt+B is 'b' with `KEY_MOD_ALT`. The terminal
   encoding moved to where the ANSI encoding of an arrow already lived
   -- `tty_input()` below the bypass, and the GUI Terminal for its pty
   -- the split this repo's conventions already stated ("what crosses a
   tty is ANSI; what reaches a window is a keysym") and the driver had
   not followed for Ctrl and Alt. **Ctrl+letter stays a control code**
   for windows too: every app's shortcuts are written against 0x13 and
   friends, and changing that would have been a rewrite of all of them
   for no user-visible gain. A character with Ctrl or Alt held is a
   SHORTCUT and is never typed: uapp routes it past the widgets to
   `on_key`, and a text widget declines it (`uui_key_is_shortcut()`),
   the line Qt and GTK draw.

2. **A game reads keys by POSITION**, on a second, opt-in stream:
   `WIN_EV_KEY_PHYS` carries the evdev keycode (`abi/input_keys.h`),
   both edges, no autorepeat, delivered only to a focused window that
   set `WIN_HINT_PHYS_KEYS` (uapp sets it when `on_phys_key` is
   present). **Opt-in** because every key would otherwise cost every
   client a second event it never reads; **in addition to** the
   translated keys, never instead, so text and shortcuts keep one path.
   The kernel dedups typematic repeats with a held bitmap -- a game
   counts presses. A press is withheld while a WM overlay is open (the
   Start menu is being typed into) but a release never is, and a
   window losing focus gets no releases for keys still held, which the
   client treats as all of them coming up -- `wl_keyboard.leave`'s rule.

**Doom maps positions to what a US keyboard prints**, the layout its
defaults and a WASD binding assume; a save-game name typed on a Nordic
layout gets US characters, which Doom's ASCII-only font could not show
otherwise anyway.

Not taken: **only B** (bits, no position stream) would have fixed
Ctrl+1 and Alt+Space but left a game's controls moving with the
layout; **only A** would have fixed Doom and left every other app
seeing Alt as Esc. **Moving Ctrl+letter to a bit as well** is the
Wayland shape but a rewrite of every shortcut table for nothing a user
sees.

## A client draws only into a buffer the compositor released, and a window has three

Two buffers and a flip ended the File Manager's flicker on selection,
and a second one came back while scrolling: the icon pane showed bare
window grey with the toolbar and tree drawn above it, 123 frames in 400
under TCG while `/bin` scrolled in icons view. The present was fire and
forget. A client presented buffer B and began repainting A at once --
and A was exactly what a composite preempted mid-read was still
scanning, so the composite finished on a half-cleared buffer and that
frame stayed up until the next present.

**Wayland's answer: `wl_buffer.release`.** The compositor says when it
has stopped reading a buffer, and a client draws only into one it has
been handed back; X11's Present has `PresentIdleNotify` for the same
reason. Here it is `WIN_EV_BUF_RELEASE`, sent for the old front while
the compositor handles the present that replaces it. That moment is
safe because the compositor is single-threaded: no composite is in
flight while a message is being handled, and nothing reads the old
front after it.

**Three buffers, so the client never waits** -- the mailbox shape the
scanout flip already uses, after two buffers and a wait tore worse than
none. One is shown, one may not have come back yet, one is free. GTK
and Qt keep a small pool of shm buffers for this. The cost is memory, so
the third is made lazily: the client picks its back buffer when a frame
STARTS, by which time the release for the previous front has usually
arrived, and two buffers alternate exactly as before. Picking at
present time would always find the old front still busy and use all
three on every frame.

**A frame with no free buffer is skipped, not waited for**: it stays
dirty and the release wakes the loop. A release lost to an overflowed
event queue would strand a buffer for ever, so after 250 ms the oldest
busy one is taken back; by then the compositor is either long past it or
not running at all.

Considered and not taken: **the compositor copying a frame at present
time** (a shm-copy compositor's habit). The copy still races, because a
compositor that is behind can handle a present after the client has
started on that buffer again -- only a handback closes the window.
`tools/half_frame_test.py` is the check, its positive control a
`surf_back()` that ignores the busy flags (81 frames in 240).

## Image Viewer is a filmstrip over a stage tinted by the picture, and its thumbnails are the File Manager's

The viewer was a filename list beside the picture. It is now Windows
Photos' and Gwenview's shape, chosen from mockups (2026-10-01): a
toolbar over a stage, the folder's thumbnails along a filmstrip below
it, a status bar with the zoom. Three things in it are decisions rather
than layout.

**The thumbnails are the File Manager's cache, moved into
`lib/uthumb.h`.** The File Manager already had a lookup that never
decodes on the draw path, one worker thread for the process's life and a
copy on disk under `/var/cache/thumbnails`. A second implementation in
the viewer would have been a second cache for the same files, filling
the disk twice and disagreeing about staleness. So the cache moved
verbatim and both apps adapt it: a `wake` callback stands in for each
app's own post, and a `stored` hook lets the File Manager claim the
filesystem generation its own cache write moved. The viewer is the
second real caller the toolkit's rule asks for. The strip itself is a
widget, `uui_thumbstrip`, that owns no pictures and asks a callback per
cell, so it is not tied to that cache either.

**The stage is tinted from the picture**, YouTube's "ambient mode": a
radial ground from the picture's darkened average to a deep shade of
its darkest quarter, a dark strip of the same, and a faint wash on the
chrome. It is computed from a 16x9 sample when a decode lands and
cached as a bitmap per stage size, so a frame costs a blit. The
alternative, a fixed dark grey, is what every viewer does and what made
this one read as unfinished. The chrome wash is kept faint on purpose:
dark text has to keep its contrast.

**The wheel zooms, about the pointer**, as Windows Photos does by
default. Gwenview steps through the folder on the wheel instead; here
Left/Right and the strip already do that, and zoom had no gesture of its
own. Above 100% only the visible part of the source is magnified,
nearest neighbour, straight from the decoded image (`uui_image`'s zoom),
so a 1600% view is never a copy 256 times the picture's size; below
100% the cached bilinear scale serves. Rotation is on screen only, an
exact quarter-turn copy (`uimg_rotate()`); writing it back to the file
would mean re-encoding a JPEG, which loses quality for a viewer's
convenience.

## The File Manager is colour-coded by what things do and what folders hold

Chosen from mockups (2026-10-01) over a navy places pane and a
wallpaper-tinted chrome: the chrome is the viewer's neutral grey, the
command bar's icons are coloured by what each command does, the places
are coloured tiles, a drive's usage bar is coloured by what the drive
is, the breadcrumb is chips with the current folder in the accent, and
a folder's icon says what it holds.

**The action colours are theme ROLES, not RGB in the app**
(`utheme_action()`). Two apps -- the Image Viewer and the File Manager --
colour the same kinds of command, and docs/gui-guidelines.md's rule is
a role once a second caller needs a colour. Named by what the command
does (navigate, view, create, edit, danger, arrange, media) rather than
by hue, so a dark theme or an accent picker can move them, and delete
is red in every app that has one. A static toolbar table cannot call
the theme, so `tint` holds the role number and the toolbar resolves it
when it draws.

**A folder's kind comes from its NAME** (`ufiletype_icon()`), the way
Plasma's folder-music and Explorer's special folders key on well-known
names: music, pictures and wallpapers, documents, fonts, sounds, icons,
cursors, terminal, services, hwdata, soundfonts, home. Reading a
folder's contents to guess would be I/O on the draw path, which
ufiletype already refuses for files. The table lives in the shared
lookup, so the file chooser shows the same folders the same way.

**The chrome is the Image Viewer's neutral grey (`UTHEME_CHROME`)**, the
places pane a lighter step of it and the pane of files white, where the
colour that matters is the icons'. It began as a light wash of the
accent, and beside the viewer that read as blue-grey against grey: two
apps of one design language with two chrome colours. The colour lives
in the command bar's icons, not in the bars.

## A held button keeps the pointer's window: no leave until the release

While a button is held or a press is armed, `wm_update_content_hover()`
changes nothing: the window under the press keeps the pointer, hears no
leave, and the window the pointer crosses hears no enter. The release
re-runs it, and that is when a leave or an enter goes out.

**This is Wayland's implicit grab** (a `wl_pointer` keeps its focus
surface for as long as a button is down), and Win32's `SetCapture` and
X11's automatic grab have the same shape. The obvious alternative, the
one toy-os had, was to treat "a button is held" as "no window is
hovered", so that hover highlights would not fight the press visual.
It also sent the pressed client a LEAVE on the first held move. A
client hears a leave as a move to `(-1, -1)` with no button bits, which
is exactly what a release looks like to an app tracking its own drag in
`on_motion`. So every app-level drag ended on its first motion, and the
Image Viewer's pan never moved at all. Widget drags survived because
the toolkit's router grabs on press and ignores the leave, which is why
the bug hid for so long.

Freezing the hover changes nothing visible for highlights: the press
visual still owns the feedback, and a stale hover cannot repaint
because nothing reaches `on_hover` until the release.

## New and redesigned apps share one design language, and the Image Viewer is its reference

The maintainer adopted the redesigned Image Viewer's look (2026-10-01)
as the look for future and redesigned apps. It is written as a section
of docs/gui-guidelines.md rather than left implicit in two apps. The
second example, the File Manager, showed that the look was being
re-derived per app: its colours started as RGB in the app until the
action roles moved into the theme.

**One language, not per-app styling**, because what made the two apps
read as modern was shared structure, not ornament: quiet grey chrome, a
command bar coloured by what commands do, a soft accent selection,
rounded shapes, colour that carries meaning, and full screen with
floating controls. Windows 11 (Photos, Explorer) and KDE (Gwenview,
Dolphin) each get their consistency the same way, from one set of
system-wide rules. **It is a reference app, not a framework**: pieces
become `userland/ui/` widgets when a second app needs them, as
`uui_thumbstrip` did, and a redesign is still mocked up before it is
coded.

## The File Manager's side column is one navigation tree, and every volume is a root of it

Chosen from mockups (2026-10-01) over Dolphin's collapsible sections
and Thunar's Places/Folders switch. The left pane is ONE `uui_tree`
(`userland/fm/fm_tree.c`): the places, a "This computer" heading, then
each mounted volume with its free space as a note and a small meter,
whose folders open beneath it. This is Windows 11 Explorer's navigation
pane. The pane before it stacked the places list over a separate folder
tree, which left the tree about a dozen rows and showed `/` as one
more folder under the volumes that already named it.

**A volume's mount point is not a folder of its parent's.** /boot is
the Boot volume's root, so it is left out of System's children (as
Explorer leaves D: out of C:). Without that, the same directory shows
twice, open in one place and closed in the other. Collapsing a branch
closes only paths on the same volume, so folding System leaves Boot
open.

**`uui_places` is not drawn in the File Manager; it is the tree's
source.** It still reads the mounts (QUERY_FSINFO) and still draws the
file chooser's side column. Its colour rule and a one-line note
(`uui_places_bar_colour()`, `uui_places_short_note()`) are what the
tree's volume rows show, so the two cannot disagree about a disk.

**The folder-tree toggle keeps its meaning**: off, the volumes are plain
rows and nothing opens; on, they are lazy parents. The widget gained a
`UUI_TREE_HEADER` kind (a caption that selection, the arrow keys, hover
and drops all step over), a note and meter per node, and
`UUI_SEL_ROUNDED`. Meters are all or none: if one volume's name and
note leave no room for its bar, no volume shows one. Chevrons replaced
the filled triangles in every tree, so Help and Device Manager match.

## ugfx's fills anti-alias by default, as Cairo's and Direct2D's do

`ugfx_fill_circle()`, `_ellipse()` and `_polygon()` blend their edge
pixels by coverage for every caller (`userland/ui/ugfx_fill.c`); there
is no flag. Until 2026-10-01 they were geom's aliased fills, and three
things drawn in one afternoon -- a play button, a speaker, a generated
cover -- were each called out as jagged on sight.

What the others do decided it. Cairo and Direct2D anti-alias every fill
unless told not to; Qt makes it a per-painter hint that nearly every
widget turns on; GDI and X11 core drawing never did, and look it. A
per-surface switch (Qt's shape) was the alternative: it leaves every
existing caller as it was until someone remembers, which is the failure
being fixed. The footprint was kept -- an integer point is a pixel's
centre and a circle reaches r + 1/2 -- so switching changed edges and
nothing else.

THE EXCEPTION IS A MESH: faces that share an edge each blend it half
way and the ground shows through. `uui_canvas` (Shapes, the teapot)
therefore still fills through `geom_fill_*` and its pixel-centre rule,
and lines keep their per-call `GEOM_AA`. Coverage is sixteen
sub-scanlines with exact horizontal spans in 1/256 px, a span's whole
pixels through a difference array; `tools/ugfx_fill_hostcheck.py`
holds it to Pillow at 64x.

## The Audio Player is a stage with the cover as the hero

Redesigned 2026-10-01 from mockups (P2 of three): the design language's
anatomy -- a command bar coloured by role, the track on a stage tinted
by its cover (the Image Viewer's ambient colours, now
`ui/uambient.h`), the playlist as a panel on the RIGHT that the bar
toggles, full screen dropping the chrome. Amberol's and GNOME Music's
shape rather than Windows Media Player's library sidebar, because this
player plays a folder, not a library.

Four calls a future session would otherwise re-litigate:

- **The play ORDER is separate from the list.** The list stays sorted
  by name; shuffle permutes an index array with the playing track at its
  head, so turning shuffle on never jumps away from what is playing, and
  repeat decides what Next does at either end.
- **Every track has a cover.** An embedded ID3 picture when there is one;
  otherwise a tile coloured from the TITLE's hash, so the same track
  always looks the same -- every file this image ships has no art.
- **The spectrum is sixteen Goertzel filters, not an FFT**: a filter per
  band is all a bar display needs, in integers (ring 3 has no floating
  point), over the frames the sink is PLAYING (`usnd_peek()`, behind the
  newest written by what is queued), so the bars match the sound.
- **Tags are parsed in ring 3 by `lib/utags.h`**, untrusted input like
  `ttf.h`: every length checked, an unknown encoding or an
  unsynchronised tag skipped rather than guessed.

## Every menu is one card, Windows 11's, and the compositor cuts a popup's corners

Chosen from mockups (2026-10-01) over Plasma's soft-accent menus and a
dark shell matching the taskbar, and applied to EVERY menu -- an app's
menu bar and right-click menus as well as the desktop's, the window
menu and the taskbar's. They are all `uui_menubar` already, so one
restyle reaches all of them, and the WM's context menu dropped the
panel colours it used to paint over the widget's: before this the
desktop's menu and an app's were two designs drawn by one widget.

**The card**: a ground a step lighter than the chrome, a hairline,
corners rounded at five-eighths of the line height, a shadow that
follows them but stays below the top edge, as before. **The rows** are
twice the line height plus air, with hover a
rounded pill inset from the edge; a row may carry an icon tinted by the
ACTION ROLE of what it does -- the colour the command bars already
give the same verbs -- and its key, right-aligned and dimmed. A file's
verbs can be a **command strip** across the top (Explorer's context
menu since Windows 11), and the hovered button names itself in a tip
inside the card, since a tooltip of its own would need a surface and a
timer the widget has neither of.

**A popup surface is painted SQUARE and the compositor rounds it**, as
it already rounds a window: it saves what is under the corners before
compositing the surface and cuts the arc after, reading the ring's
colour off the surface's own top edge. The alternative -- every widget
rounding its own surface -- blends the arc against the surface's stale
buffer, because a client never sees what is behind its popup; that is
why a window's corners were the compositor's in the first place.
Reading the edge colour rather than assuming one is what lets a menu, a
dropdown list and a tooltip, each with its own hairline, share one cut.
`uui_popup_radius()` is the one number both sides use.

**The desktop's Open > is grouped by the Start menu's categories.** It
was a flat list capped at sixteen, and with twenty desktop apps it lost
Task Manager, Terminal, System Update and UI Demo off the end without a
sign. KDE's and Openbox's root menus are categories; two levels of
submenu cost the context menu one more level in its row pool, and
nothing is capped any more.

**The desktop's selection is glass.** A solid accent block hid the
wallpaper and looked like a different control from the File Manager's
soft selection; white laid over the picture with a rounded edge is
Windows 11's desktop, and the hover state the desktop never had is the
same wash, fainter. Both use `uui_glass_round_rect()`, which the
rubber band now uses too: it was an outline only because the drawing
layer once had no alpha blend.

## The Start menu is Kickoff's whole shape: search in a header, two-line rows, power in a footer

Chosen from mockups (2026-10-01) over Windows 11's pinned grid with a
Recent section, and a full launcher grid with category chips. The two
panes stay -- they are what keep a growing list legible without a
whole screen -- and the rest follows Plasma 6's Kickoff.

**The search is a HEADER**, beside a settings button. At the foot it
sat above the Start button, which was the old argument; at the top it is
where every current launcher puts it (Kickoff, Windows 11, GNOME), and
typing goes to it with no click either way, so its place is about
reading order, not reach.

**An app row is two lines, the name and its `Comment=`.** The
description strip under the list showed one row's comment at a time and
only while that row was hovered; Kickoff shows every row's. The rows
are taller, so the column shows eight and scrolls past that, and the
tooltip is kept only for a comment the row had to cut.

**The power actions are labelled footer buttons**, Shut down with its
glyph and Restart with a chevron when a boot menu is behind it. They
were rows in the folder rail under a divider -- "act" rows mixed with
"select" rows; in the footer they are still one click from open, the
reason they never went behind a Leave tab, and the rail is folders only.
The row walk keeps its order and kinds (folders, actions, apps, the
description, search, and now the settings button), so the keyboard,
the hit test and every tool reading `gui menu --json` follow the move.

**The column's scrollbar is an OVERLAY bar** (chosen from mockups
2026-10-01, over an always-visible bar in a gutter): a thin thumb at
rest, the shared `uui_scrollbar`'s groove and thumb while the pointer
is on it or a drag holds it, floating over the rows' right end --
Windows 11's Start, Plasma and GNOME. It was a passive indicator so the
panel would not grow a second scrollbar; it has none, since the draw,
the hit test and the drag arithmetic are `uui_scrollbar`'s stateless
helpers, and the drag and a held groove click (which pages, then
repeats, Windows' and KDE's default) run from the overlay registry's
press op. The bar's rect is the hit zone, so it takes a click before
the row under it. **The widening is animated**: a 0..256 tween, out over
150 ms and back only after the pointer has been gone 400 ms (Windows
11's and GNOME's bars wait the same way), both through
`desktop.animation_speed` via `wm_anim_ms()`, so "instant" lands at
once; while it runs the WM waits a frame, not its idle park.

## A gallery's pictures are the caller's painter, and Preview= is read by the client

Chosen from mockups (2026-10-01) over a preview strip under the dropdown
and previews inside it: the cursor theme is a GALLERY of cards, each
showing five of the theme's shapes on a half-light, half-dark tile --
KDE's Cursors page, Windows 11's Themes grid. Built as `uui_gallery`, a
reusable widget, with the wallpaper picker on the roadmap as its second
caller.

**The picture is a callback (`draw_tile`), not a list of images.** What
a choice looks like is the caller's knowledge, and an image list would
make every caller render and own bitmaps at a size only the widget
knows; a painter is handed the tile's rect and draws into it, clipped.
The widget stays a radio group with cards -- selection, hover, keys,
commit on release -- and nothing else.

**`Preview=` is a word in the setting's text file that the CLIENT reads
(`uschema_text_word()`), not a field in `struct setting_msg`.** `Widget=`
crosses the ABI because every client must at least fall back on it; a
painter's name means something only to a client that has that painter,
and widening an ABI struct for one presentation word is the cost
without the reason. An unknown word draws cards with no picture -- the
setting still works.

**The shape format moved into `userland/lib/ucursor.c`.** Settings must
draw a theme without being the compositor, and a second copy of the
parser would drift from the one that decides what the pointer looks like.
The WM keeps the live theme, the settings, and its own file reads (whose
comment explains why it reads into its own buffer).

## The Start menu's settings are one page, read per open, and Recent off forgets

Chosen from mockups (2026-10-01): a Desktop > Start page holding the two
Start-button rows that used to sit on Taskbar plus five menu settings --
the list style (Detailed, Compact, Grid), the folder it opens on, Recent
on/off, folders on hover, and which power actions show. Windows 11 keeps
Start's layout and its recent/most-used switches under Personalization >
Start, apart from Taskbar; Plasma's Kickoff configures list or grid,
hover-switching and its footer buttons in one place. One page for
everything Start follows both.

**Read once per OPEN, not polled.** The menu is a transient popup: a
setting changed while it is closed applies the next time it opens, and a
change can never re-lay-out rows under the pointer. A per-frame
generation poll (cursor_theme.c's shape) buys nothing a popup that opens
fresh each time needs.

**The height cap is the Detailed card's in every style; Compact is
narrower.** Detailed and Grid share the card, so Grid shows more apps in
the same space. Compact first kept that width too, and the column was
mostly empty beside one-line names; it now fits its names (2026-10-02,
chosen from mockups), and the footer's version and power buttons set its
floor, about 460 px against 638.

**Recent off forgets.** Off stops recording AND removes the recorded
launches from `/etc/start-menu.conf`, as Windows clears its list when
"Show recently opened items" goes off. Hiding a list while keeping the
history it was built from would surprise anyone who turned it off for
privacy. Pins are untouched -- they are a choice, not a history.

**The power setting filters search too.** "Shut down only" hides Exit to
shell and Restart from the footer and from what typing finds: the
setting says which actions the menu OFFERS, and a hidden action that
search still ran would make the setting decorative.

**Hover waits 150 ms.** A folder opens under a pointer that rests on
it, not one passing over: the rail is a column, and a diagonal move to
the apps crosses other folders.

## Commands and widget changes are two callbacks, and a hover is not an event

`uapp` used to tell an app about everything through one callback,
`on_widget(id, reason)`: a list's selection moving, a slider dragged,
and a button pressed, dragged off and released -- three or four calls
per click, each carrying the item's id. Every app had to filter the
reason, and the ones that did not acted several times a click or on a
hover (Task Manager's four SIGSTOPs, a Settings dialog closing on the
hover that opened it). And because a dialog button's id WAS its command,
ids and command codes shared one number space: Crash Reports' table had
id 1, its Open command was 1, and every row the pointer crossed opened a
Notepad -- thirty-six on the bare-metal laptop, until the process table
was full.

**What real toolkits do.** Win32 has exactly the old shape --
`WM_COMMAND` carries a control id and a notification code -- and its
known failure is the same collision, fended off by convention (separate
`IDC_` and `IDM_` ranges) and by resource compilers warning on duplicate
ids. Qt and GTK avoid it by design: `clicked()` and `currentRowChanged()`
are different signals, and an item view emits nothing for a hover unless
`setMouseTracking(true)` asked it to (`entered()`).

**toy-os takes Qt's shape inside the one-callback-per-kind design it
already had.** A lone button's commit is `on_action(code)` -- the
callback a `uui_button_group` already used -- and nothing else about a
button reaches the app; `on_widget` carries value changes only. MOTION
reaches it only with a button held, or for an item flagged
`UUI_TRACK_HOVER`. And Win32's lesson is kept as a hard rule: `uapp`
looks up the item behind an id to route it, so a duplicate id -- or a
button with no `on_action` -- is refused at startup with a `uapp: BUG`
line rather than misrouted later.

**Why not a callback pointer per widget**, which is GTK's literal shape:
`uapp.h` deliberately keeps one switch per app rather than a function
pointer stapled to every widget struct, and the collision is fixed by
separating the KINDS of event, not by giving each widget its own door.
`tools/hover_sweep_test.py` is the standing check: it hovers every
widget of every Start-menu app and fails if anything appears.

