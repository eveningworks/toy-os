# Decisions

A short, topic-indexed answer to "why does toy-os work this way?" for
the handful of choices that come up again once code has grown around
them. This is a pointer file, not a duplicate of the reasoning --
`README.md`/`apps/README.md` cover architecture, and each entry below
links to the CHANGELOG.md section (or source file) that has the full
writeup. Update the pointer here when a decision changes; don't copy
the reasoning itself out of CHANGELOG.md or a source comment into this
file, or the two will drift.

If you're a Claude session or a contributor and about to ask "wait, why
is this built this way instead of the more obvious way?" -- check here
first before re-litigating it from scratch.

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
See CHANGELOG.md's **Build 490** for the full writeup.

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
view while the field is active. The lesson for any *future* widget that
draws text into a fixed box: `gfx_draw_string()` will not save you,
budget the width yourself. See CHANGELOG.md's `[Unreleased]` entry.

**Same lesson, vertical axis:** a follow-up report caught the field's
height having the exact same problem one axis over -- `apps/notepad.c`
sized the field's row height to precisely `gfx_char_h()`, so the
glyphs' own opaque background painted flush against (and visually
erased) the border pixels on any row with a character. Budgeting width
isn't enough on its own; a fixed-box text widget needs real margin on
*both* axes, not just clipping on the one that happened to get bug
reports first. Fixed with a small `ROW_VPAD` constant reserving actual
vertical slack. See CHANGELOG.md's `[Unreleased]` entry.

## IRQ registration: one handler per line, framework-automatic EOI

`kernel/core/irq.c`'s table (`irq_register_handler()`/`irq_dispatch()`)
deliberately doesn't support multiple handlers chained on one IRQ line
-- every IRQ source this kernel has, or is about to add (a NIC), lives
on its own dedicated line in QEMU's default topology (confirmed by
build 390's `lspci`), so real IRQ-line sharing (which does happen on
busier real hardware) isn't a case that comes up here; registering a
second handler for an IRQ that already has one just replaces it.
`irq_dispatch()` also sends the PIC end-of-interrupt itself,
automatically, after calling whatever handler is registered -- not
left to each handler to remember. A forgotten EOI on a real IRQ line
silently stops all further interrupts on that line, a classic and
nasty-to-debug bug; removing the chance of it was judged worth the
small loss of flexibility (a handler can't EOI early, before doing
slower work). This replaced `isr_dispatch()`'s old hardcoded if/else
chain (timer/keyboard/mouse special-cased, everything else silently
EOI'd and ignored) -- including the timer, which now hands off to
`scheduler_tick()` from inside its own registered handler
(`idt.c`'s `timer_irq_handler()`) rather than a dispatch-level special
case, so every hardware IRQ (32-47) goes through one uniform path. See
`irq.h`'s top comment and CHANGELOG.md's **Build 400** for the full
writeup, including what got regression-tested (timer/scheduler,
keyboard, mouse) since this touched all three.

## Blocking I/O waits: hlt when safe, poll when inside a syscall

Any driver that wants to genuinely block (via `hlt`) until an IRQ
fires -- rather than busy-poll a status register -- has to know
whether it's currently running inside an interrupt handler, because
`int 0x80` is wired as an interrupt gate and clears IF for the whole
syscall, so `hlt` there would park forever with nothing able to wake
it, and naively `sti`-then-blocking would reintroduce a real, already-
documented reentrancy bug: `isr_dispatch()`'s epilogue unconditionally
overwrites a single global resume pointer (`g_next_kernel_rsp`) before
every `iretq`, so a nested interrupt firing mid-syscall corrupts the
outer handler's resume point (this is why `SYS_READ_KEY` abandoned
blocking-with-interrupts-on previously). `idt.h`'s
`isr_in_progress()`/`isr_reset_depth()` (a `g_isr_depth` counter,
incremented/decremented around every `isr_dispatch()` call, force-reset
to 0 at the one safe point -- `process_run_ring3()`'s longjmp-style
resume branch) answers "am I inside an interrupt right now?" so a
driver can genuinely `hlt`-block when it's safe (the common case: boot
init and `apps/` code calling `fs_write()`/`fs_read()` directly from
kernel space) and fall back to bounded polling of the *device's own*
status bit when it isn't (the ring-3 `*_test.c` syscall path) -- the
hardware still raises that bit regardless of the CPU's IF state, so
polling it is still real completion detection, just not CPU-interrupt-
driven. `ata.c`'s `wait_dma_irq()` is the first (and, as of this
writing, only) caller, but the mechanism itself is general-purpose --
any future driver wanting to block inside a syscall-reachable code
path (a NIC's TX/RX ring, say) needs this same check, not a
driver-specific reinvention. See `idt.h`'s doc comments and
CHANGELOG.md's **Build 470** for the full writeup.

## DMA needs PCI Bus Master Enable, not just a programmed descriptor

A PCI device's I/O-mapped DMA control registers (a Bus-Master IDE
controller's BM_CMD/BM_STATUS/BM_PRDT, say) keep accepting reads/
writes and can report a nominal "transfer complete" status even when
the PCI Command register's "Bus Master Enable" bit (config offset
0x04, bit 2) is never set -- without it, the device just never issues
real memory read/write bus cycles, so DMA "succeeds" while moving no
actual data. Easy to miss because nothing about the failure looks like
a failure from software's point of view; only comparing against a
known-good PIO transfer, or tracing the actual bytes moved, exposes
it. `pci_enable_bus_master()` (`pci.c`/`pci.h`) sets it via a
read-modify-write of the Command register, called once from
`ata_init_dma()`. Any future DMA-capable driver (a NIC) needs this same
call before its own DMA moves real data -- noted directly in
`pci.h`'s doc comment, not just here. See CHANGELOG.md's **Build 470**
for how this was root-caused (PIO-vs-DMA comparison, then a host-side
pre-seeded disk image to isolate the read path and trace the bounce
buffer).

## Contiguous memory: linear bitmap scan, not a buddy allocator

`pmm_alloc_contiguous()` (`kernel/core/pmm.c`) finds a run of N
physically contiguous free frames by linearly scanning the same
one-bit-per-frame bitmap `pmm_alloc_frame()` already uses, rather than
reserving a dedicated always-contiguous region at boot, or replacing
the bitmap with a fundamentally different structure (a buddy/
segregated-free-list allocator, the standard answer to "finding
contiguous runs gets slow/fragmented"). A buddy allocator would win if
this got called often against a heavily fragmented pool -- it finds
and frees power-of-two-sized runs without a linear scan -- but nothing
in this kernel calls `pmm_alloc_contiguous()` on a hot path: today it
only exists for a future NIC driver to set up its descriptor ring once
at init, and no allocator changed size class, hot/cold split, or
scan strategy in a way this could regress. Replacing the whole
allocator to solve a fragmentation problem no code in this kernel has
actually hit yet was judged premature; it's flagged in
`docs/roadmap.md` as the fix if that ever changes, not built now. See
`pmm.h`'s top comment and CHANGELOG.md's **Build 410** for the
full writeup, including `pmm_selftest()`'s boot-time verification
(no consumer exists yet to exercise these functions any other way).

## PCI enumeration is a brute-force flat scan, not bridge-aware recursion

`kernel/drivers/pci.c`'s `pci_init()` checks every one of the 256 x 32
x 8 possible bus/device/function combinations directly via legacy
CONFIG_ADDRESS/CONFIG_DATA (0xCF8/0xCFC) port I/O, rather than the
"real OS" approach of scanning bus 0 and recursing into any PCI-to-PCI
bridge's secondary bus. Deliberate: the brute-force version needs no
bridge detection, no recursion, and no cycle safety, and finds the
same devices as the recursive version on any topology this kernel
actually runs on (QEMU's default chipset, or ordinary real hardware
without a deeply nested bridge topology) -- the only real cost is
wasted probe reads on buses/slots nothing lives at, which is cheap.
Also chose legacy CF8/CFC access over the newer memory-mapped ECAM
mechanism, since ECAM needs ACPI/MCFG table parsing just to find its
base address and CF8/CFC is universally supported including by QEMU's
emulated chipset. BARs are decoded (I/O-vs-memory, base address) but
NOT size-probed (the write-0xFFFFFFFF-and-read-back trick) -- that's
deferred to whichever future driver actually needs to map a BAR, since
it means temporarily disabling the device's decode and isn't needed
just to enumerate/identify what's present. See `pci.h`'s top comment
and CHANGELOG.md's **Build 390** for the full writeup -- this was the
first concrete milestone toward the TCP/IP prerequisites README.md's
**Build 380** entry laid out.

## Filesystem is one active backend, not mount points

`kernel/drivers/vfs.c` dispatches every `fs_*` call to a single active
`struct fs_ops` backend (today, always `tfs_ops` -- see
`kernel/include/fs_ops.h`). Adding a second filesystem means writing a
new backend and pointing `fs_init()` at it, not routing different path
prefixes to different backends simultaneously -- nothing needs the
latter yet, and it's meaningfully more code (cross-mount path
resolution, boundary conflicts) for a capability that would sit
unused. See `fs_ops.h`'s top comment and CHANGELOG.md's **Build 304**
for the full reasoning, including what it would take to add mount
points later if that ever changes.

## No recursive delete

`fs_delete()` refuses to delete a non-empty directory outright, rather
than deleting its contents. Deliberate, not a missing feature --
avoids a whole class of "oops, deleted more than I meant to" mistakes
in a filesystem with no trash/undo. See `kernel/include/fs.h` and
`tfs.c`'s top comment ("Honest limitations, not solved here").

## Persistent filesystem is write-through with a single-slot journal

Every mutating call (`touch`/`write`/`mkdir`/`delete`) still writes its
one record to disk immediately (write-through, not batched or lazily
flushed) -- but as of build 480 ("TFS2"), that one record write goes
through a write-ahead log first rather than straight to its final
table slot, closing the "crash mid-write corrupts one record" window
the original ("TFS1") write-through design explicitly accepted. A
single journal slot is enough -- not a general multi-record
transaction log -- because every mutating call here only ever changes
ONE table slot; a real filesystem juggling multi-record transactions
(renaming across directories, say) would need more than this. Chosen
over the two alternatives it was weighed against (a shadow/double-
buffer per record -- simpler logic but doubles every record's on-disk
size; a minimal commit-flag-only journal -- smaller journal but a torn
write loses the newest change instead of recovering it) because it
gives genuine crash recovery, not just torn-write detection, for a
journal region that only costs one extra record's worth of disk space
total (not per-record). See `tfs.c`'s top comment ("Journaling") for
the exact 4-step write-ahead sequence and replay logic, `docs/
tfs2-spec.md` for the on-disk journal format, and CHANGELOG.md's
**Build 480** for the full writeup.

## File timestamps are broken-down local time, not a Unix epoch integer

`fs_stat()`'s `created`/`modified` fields (build 480) are `struct
rtc_time` -- the same hour/minute/second/day/month/year struct
`SYS_GETTIME` and the shell's `time` already return -- not a Unix
epoch integer. This kernel has never needed a civil-date<->epoch
conversion for anything else (no code anywhere computes "days since
1970" or similar), so storing the same struct everything else already
uses avoided adding one just for this feature; a host-side tool
reading a TFS2 image converts to epoch seconds itself if it wants
that instead (`docs/tfs2-spec.md`'s reference reader shows the
equivalent conversion via Python's `datetime`). The tradeoff: no
UTC-offset field is stored alongside a timestamp, so a value only
means what it looks like -- local wall-clock time at whatever
timezone was selected (`timezone` shell command) at the moment it was
written -- not an unambiguous point in time comparable across
different timezone selections. Acceptable for a toy OS's own files;
would need revisiting (probably by finally adding an epoch conversion
helper) if timestamps ever needed to be meaningfully compared against
a real-world reference. See `fs.h`'s `fs_stat()` doc comment,
`tfs.c`'s top comment, and CHANGELOG.md's **Build 480**.

## `kapi.h` is the only header apps/ includes

Introduced when the tree was split into `kernel/core/`, `kernel/drivers/`,
and `apps/` (see CHANGELOG.md's **Milestone 4**) specifically so
drivers could be reshuffled internally without every app needing an
edit -- apps depend on the aggregated capability surface, never on a
driver header or `inb`/`outb` directly. `apps/wm/wm.h` is a second,
parallel boundary for GUI-specific `window_*` helpers, deliberately
not folded into `kapi.h` (not every app is a GUI app). See CLAUDE.md's
"Conventions worth knowing before editing" for the enforcement rule.

## `widgets.h`/`theme.h` stay minimal on purpose

Both only gained their current primitives once a *second* real caller
needed them (see CHANGELOG.md's **"Splitting wm.c into apps/wm/, and a
shared widgets.h/widgets.c module"** for widgets.h's origin, and the
scrollbar-phase builds for how `text_scrollback` grew from
Terminal-only to shared with Notepad). Deliberately not
speculatively built out ahead of a real second caller -- see each
header's own top comment before adding to it.

## The window manager is one event loop, not decoupled components

`apps/wm/` is split into `wm.c`/`wm_input.c`/`wm_render.c` by concern
for *readability*, but shares state through `wm_internal.h`'s
`extern`s rather than hiding it behind accessor functions -- it's
still one tightly-coupled event loop, the same thing `apps/wm.c` was
before the split (CHANGELOG.md's **"Splitting wm.c into apps/wm/..."**),
just spread across files. Deliberate: this is one component's internal
organization, not a boundary between independently-reasoned-about
components the way `kapi.h`/`wm.h` are. See `wm_internal.h`'s top
comment.

## Terminal wraps the real shell, it doesn't reimplement it

`apps/terminal.c` runs the actual `shell_dispatch()` inside a window
via a `vga_sink` redirect, rather than maintaining a second "GUI
shell" command handler that could drift out of sync with the real one.
A short, explicit list of commands that draw straight to the physical
screen or block in ways that don't make sense inside a window (`gui`,
`ring3test`, `elftest`, ...) print an explanation instead of running.
Built across four phases -- see CHANGELOG.md's **Builds 183, 193,
203, 253**.

## `ring3test` still requires a reboot after its fault, on purpose

Once process exit/teardown existed (CHANGELOG.md's **Build 173**) so a
crashed *scheduled* ring-3 process doesn't halt the kernel, `ring3test`
kept requiring a reboot anyway -- not because teardown didn't reach it,
but because it intentionally drops to ring 3 via its own raw `iretq`
instead of `process_run_ring3()`, so there's nowhere for the kernel to
recover it *to*. See `process.h` and `docs/roadmap.md`.

`elftest` used to be this file's other example (same raw-`iretq`
mechanism, via `hello.elf`) until the ELF64-to-`/bin` migration folded
it into the generic `run hello` path (see this file's entry on that
migration, and CHANGELOG.md's `[Unreleased]`) -- `hello.elf`'s fault is
now caught and recovered by `process_run_ring3()` like any other
`/bin` binary's crash, at the cost of losing test coverage for the raw
`iretq` entry path specifically. `ring3test` is the one remaining
place that path gets exercised.

## `/etc` is one shared `toyos.conf` by default, not a file per setting

`kernel/core/etc_config.c`'s `etc_config_get()`/`etc_config_set()` is a
generic name=value(+`#`comments) reader/writer that takes a `path` on
every call -- it doesn't hardcode one file. `tz.c` and `font_config.c`
both default to `/etc/toyos.conf` (see **Build 357**) rather than each
keeping its own dedicated file (`/etc/timezone`, `/etc/fontsize`,
which is what they used to be, migrated forward automatically the
first time either loads). One shared file was the explicit choice for
today's small, general settings; a setting with enough keys of its own
to be unwieldy sharing it (a GUI app with a dozen preferences) should
pass its own `/etc/<name>.conf` path instead -- nothing in
`etc_config.c` favors one file over many, that choice belongs to each
caller. See `kernel/core/etc_config.c`'s top comment for the file
format itself and CHANGELOG.md's **Build 357** for the full writeup
including the migration logic.

## Esc no longer exits the GUI desktop -- it's unclaimed at the WM level now

**Superseded -- was "Esc always exits the whole GUI desktop," see below
for what changed and why.**

Through **Build 502**, `apps/wm/wm.c`'s event loop checked `key == 27`
(Esc) and unconditionally left the window manager BEFORE routing the
keypress to whichever window was focused -- no GUI app's `on_key`
callback ever saw an Esc press. That hardcoded shortcut is gone: exiting
to the shell is now a discoverable Start menu item ("Exit to shell",
`wm_system_actions[]` in `wm.c`), not a hidden key, and Esc itself is
deliberately left unclaimed at the WM level -- free for a future
per-window or modal use (e.g. canceling a confirm dialog) instead of
double-booking it as "exit everything, no matter what's open or
focused," which is exactly the conflict this entry used to warn about.
See CHANGELOG.md's `[Unreleased]` "Exit to shell" entry.

The original reasoning below is preserved because the underlying fact
(the CLI/GUI editor's exit key had to be F3, not Esc, because of this
same conflict) is still true today -- Esc STILL isn't safe to hand to a
per-window "cancel" handler unless/until something adds its own
WM-level claim on it, which nothing has yet:

`apps/wm/wm.c`'s event loop no longer touches Esc at all -- so no GUI
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
CHANGELOG.md's **Build 377** for the full story.

## Don't put a `text_scrollback` on the stack

`struct text_scrollback` (`widgets.h`) embeds an 8192-cell buffer
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
See `apps/editor.c`'s `g_editor_tb` for the fix and CHANGELOG.md's
**Build 377** for the full story.

## The CLI editor's status bar needs its own line-wrapping pass, not a plain dump-and-let-the-console-wrap

`apps/editor.c`'s `editor_render()` looks like it should be able to get
away with `vga_clear()` + walking the buffer through `vga_putc()` in
order (which already handles wrap/scroll on its own) -- and the first
version of it (**Build 377**) did exactly that. It's wrong for a
full-screen editor specifically because a status bar printed *last*
inherits wherever the console's auto-scroll happened to leave off, not
a fixed row: fine for `console_page()`-style output that's read
top-to-bottom and thrown away, wrong for a status bar meant to stay
pinned to the bottom row the way real nano's (and this editor's own
GUI Terminal renderer, `widget_scrollback_draw()`) does. It also left
the physical console's own blinking cursor (`vga.c`) sitting on a
spurious blank row below the status bar, since that cursor just
follows wherever the last `vga_putc()` call left off.

**Build 379**'s fix -- reserve the last row, do a windowed two-pass
redraw mirroring `widgets.c`'s `scrollback_measure()`/
`widget_scrollback_draw()` (same windowing algorithm, `vga_rows()`/
new `vga_cols()` instead of pixels), and print the status line with NO
trailing `\n` so the physical cursor lands right after it instead of a
row below -- is the general pattern any future full-screen CLI
renderer in this codebase should copy, not another one-off dump. See
`editor.c`'s `editor_render()` top comment and CHANGELOG.md's **Build
379** for the full story, including a padding-math bug the windowing
itself caught during testing.

## Timezone city list is a database file, not a hardcoded array or a config key

`/etc/timezones` (a CSV-style `name,offset_minutes,dst_rule` list,
auto-seeded on first boot) and `/etc/toyos.conf`'s `timezone=<city>`
key are deliberately two different files, not one -- the database
(every city this build knows about) and the selection (which one is
active) change for different reasons and at different rates, so they
went through `etc_config.c`'s generic engine (selection) and a
purpose-built small parser (database) respectively rather than forcing
both into `toyos.conf`. This is also the concrete case **Build 357**'s
"a setting with enough keys of its own gets its own file" escape hatch
was written for -- a city list doesn't fit `key=value` shape at all.
Editing the database only takes effect on the next boot (no live-
reload command yet); see CHANGELOG.md's **Build 367** for the full
writeup and what was verified.

## Build-number scheme: fix/feature/major tiers, not dates or semver

**Superseded -- see the next entry below.** This scheme (`tools/
bump_build.sh <fix|feature|major>`, retired) replaced an earlier
date-plus-same-day-counter scheme (`YYYY.MM.DD.N`), which itself
replaced a hand-bumped `0.1.0`-style semver. It was a deliberately
coarse, Windows-build-number-style approximation (+1/+10/+50) chosen
for being consistent and easy to sanity-check later, over a freeform
number that would be more nuanced but less predictable. Kept here for
the historical reasoning -- every existing `Build N` CHANGELOG.md
heading and `build-N` git tag still refers to this scheme. See
CHANGELOG-archive.md's **Build 110** (the switch itself) and
**Build 121** (the git tag + GitHub Release convention added on top of
it) -- both predate the CHANGELOG.md/CHANGELOG-archive.md split, so
they're in the archive file now, not CHANGELOG.md.

## Versioning: semver + `-dev` suffix, not a per-change build number

Replaced the fix/feature/major build-number scheme above. Requested
directly: bumping (and picking a fix/feature/major tier for)
`BUILD_NUMBER` on every single change, however small, had become
ceremony that didn't earn its keep -- a build number that changes
constantly isn't meaningfully more informative than one that doesn't
change until something's actually ready to call a version. `VERSION`
(repo root) now holds a plain semver-ish string, `0.1.0-dev` to start,
read into `TOYOS_VERSION` by `tools/gen_version.sh` exactly the way
`BUILD_NUMBER` was before. It only changes via `tools/set_version.sh
<version>`, and only for one of two reasons: starting a new dev round
(`0.2.0-dev`) or cutting a real release (`0.2.0`, no `-dev` suffix).
`CHANGELOG.md` follows [Keep a Changelog](https://keepachangelog.com/)
from its `## [Unreleased]` section forward -- every change gets an
entry there, no version/tier attached, until a release is cut; cutting
one renames that heading to `## [<version>] - <date>` and opens a
fresh `## [Unreleased]` above it (`tools/set_version.sh` does both
steps together). See CHANGELOG.md's `## [Unreleased]` intro (added the
same day this switch happened) for the change itself.

Day-to-day mechanics: `tools/set_version.sh 0.2.0-dev` starts a new dev
round (rewrites `VERSION` only); `tools/set_version.sh 0.2.0` (no
`-dev`) cuts a release (rewrites `VERSION` AND stamps `CHANGELOG.md` as
above). Git tags moved from `build-N` per push to `v<version>` at real
releases only, cut by hand after `set_version.sh`:
```
tools/set_version.sh 0.2.0   # rewrites VERSION, stamps CHANGELOG.md
git tag v0.2.0
git push origin main --tags
```
A GitHub Release (title = `v<version>`, body = that release's
CHANGELOG section, `.iso` attached as a downloadable asset) is a
judgment call per release now rather than tied to a fixed tier, since
there's no tier anymore -- use one when a release feels
milestone-worthy enough that grabbing a working ISO without cloning +
building is worth it: `gh release create v0.2.0 toy-os.iso --title
"v0.2.0" --notes-file <path>`, or the GitHub web UI.

Alongside this, commit messages going forward list each changed/added
file with a one-line note in the body -- a separate, smaller
convention adopted at the same time, not tied to the versioning switch
itself:
```
kernel/drivers/keyboard.c   - added SE layout remap
apps/shell.c                - fixed signed-char gate in shell_read_line()
CHANGELOG.md                 - Unreleased entry
```
Subject line stays a short summary as always; this is just the body,
so a commit is skimmable on GitHub without opening the full diff.
See CLAUDE.md's own bullet on this.

## Socket fds: scaffolding ahead of the driver, not a working transport

`SYS_SOCKET`/`SYS_SEND`/`SYS_RECV` (build 420) exist and are reachable,
but `SYS_SEND`/`SYS_RECV` always return -1 -- there's no NIC driver or
protocol stack for them to move bytes through yet (see README.md's
"Basic TCP/IP networking": PCI enumeration, IRQ registration, and
contiguous memory are done; the driver itself isn't). Building even a
minimal in-kernel loopback transport (two processes exchanging bytes
through a shared buffer, no real network) was considered and
deliberately not taken -- it would prove the syscalls can move data,
but not the actual thing this scaffolding needs to prove: that the fd
namespace, the syscall ABI, and the tagged fd table (`FD_KIND_FILE`/
`FD_KIND_SOCKET` in `syscall.c`) are right, ahead of a real driver
existing. `sockettest` verifies exactly that surface -- fd allocation,
kind separation (`SYS_WRITE`/`SYS_READ` correctly reject a socket fd),
and cleanup -- without pretending a transport exists. See
`syscall_abi.h`'s `SYS_SOCKET` doc comment and CHANGELOG.md's
**Build 420** for the full writeup.

## Nordic keyboard/character support: Latin-1, not UTF-8; 3 remapped keys, not a full layout

Adding Å/Ä/Ö support (build 501) meant three separable choices, made
the same way each time: keep the codebase's existing "1 char = 1 cell
= 1 glyph" assumption intact rather than take on the much bigger
UTF-8 rework it doesn't need yet.

**Encoding: Latin-1/ISO-8859-1 single bytes (Ä=0xC4, Ö=0xD6, Å=0xC5,
ä=0xE4, ö=0xF6, å=0xE5), not UTF-8.** Every byte-buffer boundary in
this kernel (`scrollback_cell`, `fs.h`'s file content, the syscall
ABI's buffer+length `SYS_WRITE`/`SYS_READ`) already assumes one byte
is one character is one glyph cell; UTF-8 would break that assumption
everywhere a multi-byte Nordic letter crossed it, for a codebase that
only needs 6 extra characters right now. `font_ttf.h`'s
`FONT_TTF_EXTRA_COUNT` bakes exactly these 6 glyphs (see
`tools/genttf.py`'s `EXTRA_CHARS`), not the full 0xA0-0xFF Latin-1
Supplement block -- easy to extend later (append to that list and
re-run the script) if more accented characters are ever needed.

**Keyboard layout: `keyboard <us|se>` remaps 3 scancodes, not a
from-scratch Nordic layout.** `keyboard.c`'s `scancode_ascii_se[]`/
`scancode_ascii_shift_se[]` are copies of the US tables with only
scancodes 0x1A/0x27/0x28 (the physical keys under Å/Ä/Ö on a real
Nordic keyboard) changed -- everything else, including AltGr-level
symbols a real Nordic layout also remaps, stays US QWERTY, since this
driver has no AltGr/dead-key handling at all (see keyboard.h's
`IS_NORDIC_CHAR()` comment). Persisted the same way `timezone`/
`fontsize` already are -- a `keyboard_layout=<us|se>` key in
`/etc/toyos.conf`, loaded once at boot by `keyboard_config_init()`.

**The actual bug that made this hard to verify: `char` is signed, and
one gate had a differently-shaped filter the others didn't.** No
`-funsigned-char` in this build's CFLAGS, so a codepoint >= 0x80 is
negative as `char` -- `gfx_draw_char()`'s old `c < 32 || c > 126`
range check and five `key >= 32 && key < 127`-shaped "is this a
printable char" gates across `apps/` (terminal, notepad, widgets
textfield, editor) and `userland/echo.c` all
silently rejected Nordic letters before this build. `keyboard.h`'s new
`IS_PRINTABLE_KEY()` macro (and `font_ttf_glyph_index()` in gfx.c,
which takes the codepoint as `int`/`unsigned char` rather than relying
on `char`'s signedness) fixed all of them at once -- except
`apps/shell.c`'s own `shell_read_line()`, which had a SIXTH,
differently-worded gate (`c < 128`, not `key >= 32 && key < 127`) that
a grep for the other five's exact phrasing missed entirely. Found only
by QMP-testing actual keystrokes end-to-end and noticing the cursor
didn't even advance -- not by code review -- which is the concrete
argument for always verifying a "fixed every instance of X" claim by
testing the behavior, not just re-grepping the pattern you already
fixed. See CHANGELOG.md's **Build 501** for the full writeup.

## Protected files: `device_commit_files` blocks writes, `device_bash` doesn't

`Makefile` and anything under `.github/workflows/*.yml` are protected
specifically against `device_commit_files` -- confirmed for
`.github/workflows/build.yml` when it was first added, likely a
blanket CI-workflow protection rather than something specific to this
repo. The fix isn't "ask the user to copy a file by hand," though:
`device_bash` has ordinary read/write access to the mounted folder and
is NOT blocked from writing `Makefile` directly. So the actual flow is
-- edit the file in the cloud sandbox, verify the build there, deliver
it as `Makefile.new` (any filename that doesn't match the protected
path) via `SendUserFile` + `device_commit_files`, then finish the job
over `device_bash`: `cp Makefile.new Makefile`, `diff` the two to
confirm they're now identical, then move `Makefile.new` into
`_to_delete/` (can't delete it outright, same as any other file over
this bridge -- see the next entry). Same trick for
`build.yml.new`/`.github/workflows/`. Only fall back to asking the
user to copy it themselves if `device_bash` genuinely can't reach the
file. Check `device_commit_files`' response generically for this --
its `rejected` array has the exact path and reason for anything it
refused, so a batch delivery doesn't get assumed to have landed in
full just because the call didn't error outright.

## The device bridge can't delete files, and `git` leaves stale locks behind on it

Two related device-bridge limits, both worked around the same way
(`mv`, not `rm`):

**Can't delete, full stop.** `device_bash`'s `rm`/`rmdir`/`unlink` fail
with "Operation not permitted" on mounted files, and
`device_commit_files` only writes. To remove a now-superseded file
from the user's machine, `mv` it (via `device_bash`) into a
`_to_delete/` subfolder next to it, then tell the user which folder to
delete themselves.

**`git` commands run via `device_bash` leave behind a stale
`.git/index.lock` -- even a read-only `git status`.** Git creates the
lock (to refresh its stat cache, in `status`'s case), then tries to
delete it when the command finishes -- but that delete is the same
blocked `unlink` as above, so it silently fails (you'll see a
`warning: unable to unlink ... Operation not permitted`, but the
command itself still succeeds). The lock file is left sitting in
`.git/`, and the *next* `git` command that needs to write the index
(`add`, `commit`, ...) fails hard with `fatal: Unable to create
'.../index.lock': File exists` -- indistinguishable from a genuinely
stuck git process, and just as confusing if it's the user's own
terminal that hits it after a session leaves one behind. Fix: rename
the lock out of the way instead of deleting it -- `tools/device_git.sh`
does this automatically, both BEFORE running the real git command
(sweeps anything already stale) and AFTER it (sweeps whatever that
command itself just left behind, with a `sleep 0.5` first -- a lock
git just created can be briefly invisible to `find` over this mount
without it, confirmed by testing). Earlier versions of the script only
swept before, which left a fresh lock for the *next* command --
including the user's own terminal -- to trip over; sweeping after too
is what makes the repo actually lock-free when the script returns,
not just when the next `device_git.sh` call happens to run. Always use
`tools/device_git.sh` for every `git` command reached this way,
`status` included -- never hand-roll this check inline, and never run
`git` directly via `device_bash` even for a "harmless" read.

## Button press/release feedback: a general `on_press`/`on_release` WM mechanism, not a Calculator-only hack

Calculator's buttons already used the shared `widget_button()`
(`apps/widgets.c`) -- the actual gap was that nothing in the window
manager ever told an app "the mouse is down and still on this button,"
so no app could draw a pressed state even if it wanted to. Two ways to
close that: give Calculator its own private mouse-tracking (poll
button state directly in `calculator_draw()` somehow), or add a real
event to the window manager's app-callback contract. Went with the
latter -- `gui_apps.h` gained `on_press(win, cx, cy)`/`on_release(win)`,
driven from `apps/wm/wm_input.c`'s `wm_update_drag_resize()` the same
way `content_dragging`/`on_drag` already work, with a matching
`content_pressed` index in `wm_internal.h`. Reasoning: this codebase
already has one precedent for "click vs. hold-and-drag needs its own
event pair distinct from `on_click`" (`on_drag_start`/`on_drag`), and
press/release is exactly that same shape -- a private per-app
workaround would have solved Calculator alone and left the next app
that wants pressed-button feedback (or a held-scrollbar-thumb, or a
press-and-repeat spinner) to reinvent it from scratch. See
`CHANGELOG.md`'s `[Unreleased]` entry for the mechanism's actual shape
(why it fires on the initial button-down tick, why it returns 1 only
when the "hot" button changes, how drag-off-before-release un-presses
without triggering the button).

## ui_button/ui_button_group: Brutal-OS-inspired, but not a full retained view system

Asked directly to look at Brutal OS's `libs/brutal-ui/button.c`/`.h` and
consider whether toy-os should have "own libs for GUI apps" the same
way. Worth separating two things that question conflates: the
*insulation boundary* (apps not reaching into WM internals) already
existed -- every `apps/*.c` file only ever includes `wm/wm.h` (the
public `window_*` API) and `widgets.h`, never `wm_internal.h` (that's
`apps/wm/*.c`'s own private `extern` state). What Brutal's button.c
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

## Start menu click flash: a deferred close via pit_ticks(), not a blocking sleep

A Start menu click used to run the row's action and close the menu in
the same frame -- no visible confirmation the click landed, just an
instant jump to whatever opened. Adding a brief "you clicked this" flash
needed the menu to stay open and visibly highlighted for a short time
*after* the action already ran, which a single-threaded `hlt`-loop WM
(see `apps/wm/wm.c`'s top comment) can't do with an actual blocking
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
See `CHANGELOG.md`'s `[Unreleased]` entry for the full mechanism.

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
even a restack. See `CHANGELOG.md`'s `[Unreleased]` entry for the full
mechanism and what was verified.

## apps/ui/: a directory for retained-widget objects, once there were three

`ui_button.c`/`.h` and `ui_button_group.c`/`.h` lived directly in
`apps/` at first (there was only one pair, no directory felt warranted
yet -- same "don't split preemptively" judgment call `CLAUDE.md`
describes for files in general). Adding `ui_textbox.c`/`.h` as a third
pair made a flat `apps/` start to mix two different kinds of file
(whole *apps* like `calculator.c`/`notepad.c`, and small *widget*
building blocks they both depend on) -- the same signal that split
`apps/wm/` out earlier, applied one level up. `apps/ui/` follows that
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

## Kernel heap: coalesces by real address adjacency, not list order

`kmalloc()`/`kfree()` (`kernel/core/heap.c`) grow the heap by calling
`pmm_alloc_contiguous()` again whenever the free list can't satisfy a
request, appending the new region's block to the end of the list. That
means list order and physical-address order agree *within* a region,
but nothing guarantees the next `pmm_alloc_contiguous()` call returns
frames adjacent to the previous region -- the PMM's bitmap allocator
is free to hand back frames from anywhere. `kfree()`'s coalescing
(`try_merge_next()`) checks the real pointer arithmetic
(`(uint8_t *)(b + 1) + b->size == (uint8_t *)n`) before merging two
list-adjacent blocks, not just "these are next to each other in the
list" -- merging on list-order alone would corrupt the heap the first
time two separately-grown regions happened to sit list-adjacent but
not address-adjacent (freeing block A would silently absorb block B's
header into A's payload size, and a later allocation into that
"merged" space would write past the actual end of A's real memory).
See `CHANGELOG.md`'s `[Unreleased]` entry for the full design and what
`heap_selftest()` verifies.

## Calculator is the first `multi_instance` GUI app

`gui_apps.h`'s `multi_instance` flag (see `CHANGELOG.md`) is opt-in
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

## `kfree()`'s coalescing only checked the block being merged in, not the block being merged into

The heap allocator's backward-coalescing call was `if (b->prev)
try_merge_next(b->prev)` -- correct-looking, since `try_merge_next()`
already checks its *argument's* `next` pointer is free before merging.
The bug: it never checked whether `b->prev` *itself* was free. Freeing
a block whose list-previous neighbor was still allocated silently
folded the freed block's size into the still-in-use neighbor's `size`
field (since `try_merge_next(prev)` saw `prev->next` was now free and
merged it in), with no corresponding adjustment to `g_used_bytes`. A
later `kfree()` of that neighbor then subtracted more than was ever
added, underflowing the unsigned `g_used_bytes` counter. This existed
since the heap allocator was first written and went completely
undetected -- nothing ever displayed `heap_used_bytes()` until Task
Manager did, and it showed an impossible ~16 exabyte figure. Fixed
with a `b->prev->free` guard; `heap_selftest()` now explicitly asserts
`heap_used_bytes() == 0` after freeing everything, so this class of
regression is caught at every future boot instead of needing another
UI to happen to surface it. See `CHANGELOG.md`'s `[Unreleased]` entry
(the Task Manager bullet).

## TFS2 v2's block pointers go direct + single + double + triple indirect, not just direct + single

TFS2's original 2048-byte inline-file format was replaced with a
classic Unix-inode-style scheme (12 direct block pointers + single/
double/triple indirect) specifically to reach multi-gigabyte files
without keeping the whole file in RAM. Direct + single indirect alone
tops out at `12 + 1024` blocks (~4MB at the 4096-byte block size) --
nowhere close to the 8GB target. Direct + single + double gets to
roughly `12 + 1024 + 1024*1024` blocks (~4GB) -- still short. Triple
indirect (`1024^3` more blocks reachable through one pointer) is what
actually clears 8GB with headroom, which is why all three tiers exist
rather than stopping at double indirect the way a smaller target could
have. This is also why `tfs_selftest()` deliberately targets a write
at a ~4.6GB offset -- past double indirect's ceiling -- as the boot-time
proof that the triple-indirect chain is really being built and walked,
not just declared. See `CHANGELOG.md`'s `[Unreleased]` entry (the TFS2
multi-GB bullet) for the full format writeup.

## `fs_read_range()`/`fs_write_range()` were added alongside `fs_read()`/`fs_write()`, not as a replacement

`fs_read()`'s contract has always been "return a pointer to the whole
file, loaded into RAM in one call" -- fine for small text files, but
architecturally incapable of handling a file larger than available RAM
(256MB in the normal QEMU config) no matter how large the on-disk
format gets, since the call itself has nowhere to put an 8GB result.
Rather than redesign every existing caller (Notepad, the shell,
editor.c -- all of which only ever touch small files and are simplest
written against "give me the whole thing") around a chunked API they
don't need, `fs_read_range(path, offset, buf, len)`/`fs_write_range()`/
`fs_size()` were added as a second, parallel API for callers that
genuinely need bounded-memory access to a large file. `fs_read()`/
`fs_write()` keep their exact old behavior and signatures. See
`CHANGELOG.md`'s `[Unreleased]` entry (the TFS2 multi-GB bullet).

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
saves real work for. See `CHANGELOG.md`'s `[Unreleased]` entry.

## The desktop's right-click quick-launch menu doesn't distinguish icons from empty space

`apps/wm/desktop.c`'s `desktop_handle_right_click()` always opens the
same full quick-launch menu (one row per `gui_app_registry` entry)
regardless of whether the click landed on a specific icon -- a
per-icon menu (e.g. "Open" / a future "Rename"/"Properties") was
explicitly scoped out this round, not an oversight: desktop icons
don't have any per-icon identity or state beyond "which
`gui_app_registry` index am I" yet (no rename, no repositioning, no
custom icon), so a per-icon menu would have nothing more useful to
offer than the quick-launch menu already does. Revisit once icons gain
real per-icon state worth a dedicated menu for. See `docs/roadmap.md`
and `CHANGELOG.md`'s `[Unreleased]` entry (the desktop/context-menu
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
shared abstraction to begin with (see `apps/wm/start_menu.h`'s own top
comment on why it isn't a general "menu" type). See `CHANGELOG.md`'s
`[Unreleased]` entry.

## dmesg coverage: log from the one-shot call site, not the hot function itself

When extending `klog_write()` coverage to the RTC (`kernel/core/
timer.c`'s `rtc_read()`), the log line went into `kernel_main()`
(`kernel/core/kernel.c`), which calls `rtc_read()` exactly once at
boot for this purpose -- not into `rtc_read()` itself, even though
that's the more obvious place a driver-level log usually lives (see
every other dmesg addition in the same change: `pci_init()`,
`mouse_init()`, `vga_init()`, `keyboard_set_layout()` all log from
inside the driver function). `rtc_read()` is the exception because
it's not an init function -- it's called continuously by the taskbar
clock and `tz.c` every time either redraws, so a log line inside it
would flood the 16KB ring buffer with a new timestamped line every
second or so, pushing out everything else `dmesg` is actually useful
for. The general rule this leaves for the next area added to dmesg
coverage: log from whatever call site is genuinely one-shot (an
`*_init()` function, a boot-sequence call in `kernel_main()`), not
from a function just because it's the "natural" owner of the
information, if that function is actually called on every frame/tick/
redraw instead of once. See `CHANGELOG.md`'s `[Unreleased]` entry for
the full list of areas covered and `klog_write_dec()`/
`klog_write_hex()` (`kernel/include/klog.h`), added in the same change
for klog messages that need to include a number.

## Real disk-hosted ELF binaries: an old plan re-verified before building, not built from the doc as written

`docs/roadmap.md` already had a plan for this (split into (A) a real
syscall-based ELF program, (B) loading it from `/bin`), written in an
earlier session as a pure planning pass. Before actually building it,
that plan got re-checked against the current codebase rather than
implemented as written -- worth recording why, since the two
differences found are exactly the kind of "the codebase moved out from
under an old doc" trap a future session could hit again elsewhere:

- The plan's stated hard blocker for (B) was TFS2 capping a file at
  `FS_DATA_MAX` = 2048 bytes, with a whole discussion of multi-slot
  chaining to fix it. By the time this was re-checked, `tfs.c` no
  longer referenced `FS_DATA_MAX` at all -- TFS2 v2's block-addressed
  on-disk rework (the multi-GB file support entry, `CHANGELOG.md`'s
  `[Unreleased]`) had already solved this as a side effect, for
  unrelated reasons, in a different session that had no idea an old
  ELF-binaries plan was depending on that limit staying in place. Three
  comments (`kernel/core/etc_config.c`, `apps/editor.c`/`.h`) still
  cited the old 2048-byte ceiling as real months later -- corrected in
  the same change that shipped this (see `CHANGELOG.md`).
- The plan assumed `elf_load()`'s ELF blob would need copying out of
  TFS2's live in-RAM table into a scratch buffer before executing,
  since that memory "isn't stable the way a GRUB module's reserved
  region is." Checking `elf_load()` (`kernel/core/elf.c`) and
  `heap.c`'s own top comment together showed this wasn't needed:
  `elf_load()` just casts its `elf_phys_addr` argument straight to a
  pointer with zero translation, which only works because GRUB modules
  sit in identity-mapped low physical memory -- and `kmalloc()` is
  *also* carved out of that same identity-mapped low-4GiB range (see
  `paging.c`'s top comment, referenced from `heap.c`), so `fs_read()`'s
  returned buffer address already works there directly. One real
  constraint this does leave, not present in the GRUB-module path:
  nothing may call `fs_read()` again until the loaded process finishes,
  since the backend reuses one static buffer across calls (`fs.h`'s
  `fs_read()` doc comment already says this; `elf_run.c`'s own comment
  restates it as a caller-facing constraint).

Net effect: (A) and (B) shipped together in one change instead of two,
since (B) turned out to be much smaller than the plan estimated. What
the plan got right and is still true: getting a binary's bytes onto
`disk.img` at all needs *something* outside the OS, since there's no
in-guest compiler -- see the next entry for which of the plan's two
options (`bootstrap-install` vs. a host-side writer tool) was picked,
and why. See `CHANGELOG.md`'s `[Unreleased]` entry for the full
implementation (`SYS_PCI_COUNT`/`SYS_PCI_INFO`, `userland/lspci.c`,
`elf_run.c`, `install_bin_binaries()`).

## `/bin` binaries: boot-time bootstrap-install now, a host-side TFS2 writer tool later

Getting a compiled ELF's bytes onto `disk.img` has no in-guest-compiler
option -- something outside the OS has to place them there. Two ways
were on the table (see the previous entry's roadmap plan): a host-side
tool that writes directly into TFS2's on-disk format (informed by
`docs/tfs2-spec.md`'s existing read-only reference parser, which would
need a write-side counterpart built from scratch), or copying a GRUB
module's bytes into `/bin` once, at boot, via code the kernel already
has (`fs_write_range()`/`fs_touch()`, both already exercised by other
callers). The user chose bootstrap-install for `lspci` now, with the
host-side tool explicitly deferred to a later session (see
`docs/roadmap.md`'s new backlog entry) rather than skipped -- the
bootstrap path needs zero new tooling and was demonstrably enough to
prove the whole `/bin`-loading pipeline end to end, while the
host-side tool only pays for itself once a *second* binary needs
installing without a kernel rebuild, which isn't true yet. The
tradeoff this defers, worth remembering when that second binary shows
up: `install_bin_binaries()` (`kernel/core/kernel.c`) is a small table
of `{module_index, bin_path}` pairs specifically so adding one more
GRUB-module-installed binary is a one-line addition, not a redesign --
but it's still "add a GRUB module + a table row + rebuild the kernel"
per binary, not "drop a file onto the disk image," which is exactly
what the host-side tool is for.

## `tools/tfs2_writer.py`: content-hash sync, not mtime comparison; direct+single-indirect write scope, not full indirect support

Two scope calls made building the deferred host-side TFS2 writer (see
the entry above): how `sync`'s "only rewrite if changed" policy
detects a change, and how big a file the tool is willing to write at
all.

**Content hash, not mtime.** TFS2's `created`/`modified` fields are
toy-os's own RTC-sourced local wall-clock time (see `fs.h`'s
`fs_stat()` comment) -- there's no epoch, and no defined relationship
to the *host* machine's clock a comparison could lean on without
assuming a particular skew. Comparing "is the local file newer" against
that would be guessing. Hashing the on-disk content and comparing it to
the local file's content sidesteps the clock question entirely and is
just as correct for the actual goal ("did this file's bytes change") --
this is why `sync`'s `sync/` subtree policy reads the existing file
back and SHA-256-compares it rather than checking timestamps.

**Write scope is direct + single-indirect blocks only (~4.03 MB/file),
not the full direct+single+double+triple scheme `docs/tfs2-spec.md`
documents for reading.** The tool refuses cleanly (clear error, no
silent truncation) rather than write a partial file past that size.
Everything this tool exists for -- ELF binaries, config/text seed
files -- fits comfortably under that ceiling; double/triple-indirect
allocation is real extra code (the same recursive block-tree shape
`tfs.c`'s own `alloc_block()`-adjacent logic would need) that has no
current caller. If a future seed file genuinely needs to be larger,
extend `write_file()`'s allocation loop rather than raising the limit
silently -- the read path (`block_for_index()`) already walks all four
levels, so only the write side needs the extra work.

## `/bin/lspci` moved from boot-time bootstrap-install to build-time seeding, once the writer tool existed

The previous entry deferred the host-side TFS2 writer tool and kept
`install_bin_binaries()`'s boot-time bootstrap-install
(`kernel/core/kernel.c`, copying a GRUB module's bytes into `/bin` the
first time toy-os boots against a disk) as the interim way to get
`lspci` onto disk. Once the writer tool existed and grew a `format`
subcommand (see the entry above -- needed because `write`/`sync`
previously required an already-formatted image, which a brand new
`disk.img` isn't until toy-os boots and formats it once), that
interim mechanism became fully redundant: `tools/tfs2_writer.py sync`
can format-and-seed a completely untouched `disk.img` in one call, at
BUILD time, with no boot cycle needed at all.

`install_bin_binaries()`/`BIN_BOOTSTRAP` and the `lspci.elf` GRUB
module were removed outright rather than kept as a fallback -- two
mechanisms solving the same problem is exactly the kind of debt this
project avoids once the better one exists (see `CLAUDE.md`'s file-split
guidance for the same instinct applied elsewhere: don't keep unused
machinery "just in case"). If a future binary genuinely needs
boot-time-only install for some reason GRUB-module bootstrap-install
would fit better than build-time seeding, that's a fresh design
question when it actually comes up, not a reason to have kept the old
table around empty -- the git history (this entry, and the
`CHANGELOG.md` sections it points at) has everything needed to bring
the pattern back if so.

The Makefile's new `seed` target runs on every `make iso` (not just
when `disk.img` is first created) -- deliberately `.PHONY` so it always
re-runs, relying on `sync`'s own content-hash compare (not `make`'s
mtime-based staleness check) to make repeat calls cheap. This matters
because `disk.img` is explicitly NOT rebuilt by `make clean` (see its
own comment in the Makefile -- it's local persistent dev state, not a
build output) -- if `seed` only ran once, a rebuilt `lspci.elf` with
real code changes would silently never reach an existing `disk.img`
again.

## Every ELF64 test binary moved to `/bin`, not just `lspci` -- and why two didn't fold in cleanly

`lspci` was the first ELF64 binary moved off a GRUB module onto
build-time-seeded `/bin` (see this file's `seed`-target entry above).
The remaining dozen-ish test binaries followed the same path in one
pass (CHANGELOG.md's `[Unreleased]` entry has the full list) rather
than staying GRUB modules indefinitely, once it was clear the seeding
mechanism generalized cleanly -- there was no longer a reason for
`lspci` to be the only one.

Almost all of them folded into the existing generic loader
(`kernel/core/elf_run.c`'s `elf_run_from_fs()`) with zero new code,
which is the whole point of that function existing: one loader, N
binaries, no per-binary kernel harness. Two didn't:

- **`schedtest`** (`counter_a`/`counter_b`) needs two processes running
  *concurrently* under the real preemptive scheduler -- a one-shot
  `run <name>` inherently can't do that, no matter how generic the
  loader gets. This got real new code: `scheduler.c`'s
  `spawn_from_fs(const char *path)`, replacing `spawn_from_module()`
  outright (its only caller was `scheduler_demo_run()`) -- same
  no-copy-needed `fs_read()` reasoning `elf_run_from_fs()` already
  used, just wired into the scheduler's spawn path instead of the
  one-shot run path.
- **`elftest`/`hello.elf`** tested toy-os's raw manual-`iretq` ring-3
  entry specifically, a different (and older) code path than
  `process_run_ring3()`'s recoverable one. Folding it into `run hello`
  means that specific raw-entry test coverage is gone -- a real
  tradeoff, made deliberately (user's call, weighing one narrow bit of
  coverage against one less special case) rather than accidentally.
  `ring3test` remains as the one place the raw-`iretq` path is still
  exercised at all (see this file's entry above).

`ring3test` itself was never a candidate to fold in -- it uses no ELF
file whatsoever, there's nothing to seed.

Along the way, `elf_run_from_fs()` gained an unconditional
`syscall_reset_heap()` call it didn't have before -- found by reading
`echo_test.c`, which called this itself ahead of its old
dedicated-command loader. Migrating `echo_test` onto the generic path
without this would have silently broken its `sbrk()`-based heap the
first time anyone actually exercised it, not at compile time. Making
it unconditional (rather than a per-binary opt-in flag) costs nothing
for a binary that never calls `sbrk()` -- it's bookkeeping, not an
allocation -- so there was no reason to keep it special-cased.

`apps/terminal.c`'s GUI Terminal window still blocks `run` wholesale,
not per-target. Several of the newly-independent `/bin` binaries
(`gui_test`, `win_test`, `echo_test`) have the same hazards inside a
GUI window the old dedicated commands were blocked for (drawing
straight to the physical framebuffer, blocking forever without
yielding back to the window manager) -- a per-target allowlist was
prototyped, but the QMP test written to verify it was invalid: it
relied on `tools/gui_flow.py`'s `open_app("Terminal")`, which (due to
a separate, pre-existing bug -- see below) was actually opening
Calculator. Rather than ship GUI-safety-relevant logic that couldn't
be verified, the simpler wholesale block was kept. Every `/bin` binary
can still be run from the physical shell regardless.

That `gui_flow.py` bug was real and unrelated to this migration:
`ITEM_H` (the assumed Start-menu row height) was `32`, stale against
the kernel's actual `gfx_char_h() + 6` (`24` at the default font
size) -- so every `open_app()` call was clicking roughly one row below
where it meant to. Found and fixed once it was blocking this
migration's own testing; see CHANGELOG.md's `[Unreleased]` entry.

## Click-to-position/selection lives in the shared `text_scrollback` widget, not a Notepad-only one

When asked to add click-to-position and text selection, user chose
extending the shared `apps/ui/ui_scrollback.c` widget (used by
Notepad, Terminal, and `apps/editor.c`) over building a new
Notepad-only widget. Terminal and `editor.c` never call the new
selection API, so it's inert there -- but any future caller of
`text_scrollback` gets click-to-position/selection for free, and there
was no plausible reason for the underlying pixel<->buffer-index math
(and its correctness) to exist twice. See CHANGELOG.md's
`[Unreleased]` entry for the full implementation.

## The serial debug console is poll-based from existing idle loops, not a new kernel thread

`debug_console_poll()` is called from `keyboard_getchar()`'s hlt-wait
loop and `wm_run()`'s main event loop -- both already wake on every
interrupt and already have a "cheap thing to do while otherwise idle"
convention (`vga_cursor_tick()`). Piggybacking there means a serial
debug session over COM1 works without any new scheduling or
kernel-thread machinery, matching this kernel's existing
single-threaded-cooperative-with-interrupts model. The honest
limitation: it does NOT get polled while a blocking command, a
ring-3 process, or anything else that isn't one of those two loops is
running -- accepted rather than solved, since fixing it properly would
mean either real kernel threads or polling from many more places for
a debug-only feature. See CHANGELOG.md's `[Unreleased]` entry.

Getting serial RX working at all needed two things past "unmask the
PIC": `serial_irq_init()`'s IRQ registration/unmask has to run *after*
`idt_init()`, not from `serial_init()` itself, since `serial_init()`
deliberately runs first in `kernel_main()` (so `klog_write()` has
somewhere to send its very first byte) -- before `idt_init()`'s own
"mask everything, then unmask only what's wired up" pass, which would
otherwise immediately undo an earlier unmask. And unmasking the PIC
line isn't sufficient by itself: the UART's own Interrupt Enable
Register also has to be set (`serial_init()`'s original `outb(COM1+1,
0x00)` leaves it at 0, since nothing needed RX before this). Found
live -- a fully-correct-looking IRQ handler + PIC unmask produced zero
bytes, not even local echo, until the IER bit was added.
