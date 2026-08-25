# Testing toy-os

How to run this OS and prove a change works. CLAUDE.md keeps the rules a
session needs before it knows to look anything up -- the build targets,
the stale-ISO trap, the diagnosis rules -- and points here for the rest.

**The order of cheapness.** Reach for the cheapest thing that can answer
the question:

1. `boot_smoke_test.py` -- does it still boot cleanly? Seconds. Most of
   what a kernel/driver/syscall change needs verified.
2. `make test` / `vm.py exec` -- does it work? Text you can assert on.
3. QMP (`qmp_test.py`, `gui_flow.py`) -- does it LOOK right? Only when
   the answer genuinely depends on pixels.

A text transcript says nothing about whether a button is drawn in the
right place; a clean boot log says nothing about rendering. But a
screenshot is the most expensive evidence there is, and the worst to
assert on -- so do not reach for step 3 when step 2 would do.

## The in-kernel suite, and driving a VM in text

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
python3 tools/vm.py --kvm run "stress 150"   # same, KVM-accelerated (see `make run KVM=1`)
python3 tools/vm.py --cpu Skylake-Client run "lscpu"  # a specific QEMU CPU model
python3 tools/vm.py --vga vmware start   # vmsvga: the one with a HARDWARE cursor
#   (std, vmware and virtio can all set modes -- std through the bochs driver)
python3 tools/vm.py --instance 2 --disk /tmp/b.img start  # a second VM, alongside
```

**`vm.py exec` OUTPUT NEVER REACHES THE SCREEN, so do not look for it in
a screendump.** A command sent this way runs over the serial debug
console, whose replies go back down the wire -- the console output is
redirected into a SINK for the duration (`vga_set_sink()`, api/vga.h)
rather than being drawn. So `vm.py exec "ls /"` and `vm.py shot` show
two different things, and a screenshot taken to check what a command
printed shows whatever was on screen BEFORE it. Two consequences worth
knowing:

- To see a command's output on the physical console, TYPE it --
  `tools/shell_flow.py`'s `ShellFlow.run_command()`, which handles the
  spaces and punctuation `send_text()` drops (a hand-typed
  `ls -1 /etc` arrived as `ls1etc`). That also means the guest needs the
  US keyboard layout pinned, since a qcode names a physical key by its
  US label -- `kbd=us` on the GRUB line, or `sh keyboard us`.
- Anything about COLOUR is invisible over `exec` by construction, since
  the console parses `ESC[...m` into a colour before the sink ever sees
  a byte (kernel/lib/ansi.c). What `exec` CAN prove is the negative: if
  the parser were not wired in, the escapes would arrive as literal
  `[1;36m` text. The colour itself has to be read as PIXELS from a
  typed-at console -- which is how `/bin/ls`'s was verified.

**And the physical console may not be visible at all.** On a
`graphical` boot the desktop owns the screen and the kernel shell stands
down (CLAUDE.md); a text console to type at means booting with
`target=text`.

`--instance N` is how you run more than one VM at once: pidfile, serial
socket, QMP port and VNC display are all derived from N, so slot 2 can
never stop slot 0's VM or connect to its console. Slot 0 is the default
and is exactly what it always was.

**Some CPU features are unreachable in BOTH default modes, and the
invariant TSC is one.** `--cpu` applies under `--kvm` too now, and that
combination is the ONLY way to run the kernel's TSC clocksource:

```
python3 tools/vm.py --kvm --cpu host,+invtsc start   # the TSC path
```

TCG does not implement `invtsc` at all (`-cpu max,+invtsc` prints
`warning: TCG doesn't support requested feature` and clears the bit),
and KVM withholds it even under `-cpu host`, because a guest that has
seen it cannot be live-migrated. So the default test environment
exercises only the PIT-backed clocksource, and a green suite says
nothing about the TSC one. `notsc` on the GRUB line reaches the same
split from the other side. **Generalise it: before concluding a feature
"just isn't available in QEMU", check whether it needs an explicit `+`
flag AND which accelerator supports it** -- the two are independent, and
this cost real time.

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


### Five ways a harness built on `vm.py` reports the wrong thing

Each of these has cost a session real time here, and none of them fails
in a way that points at itself.

**`"ready" in output` is the wrong readiness test, because "al-ready"
contains "ready".** `vm.py start` answers a slot that is already taken
with `vm: already running`, so the obvious check passes on the one
output that means the opposite -- and the tool then talks to a serial
socket that does not exist, several steps later. Use
**`vm.started_ok(output)`**, which exists so there is one place to be
right; eight tools here carried the substring test before it was found.

**A stopped guest keeps its QMP port for about a minute, so two boots
cannot share a slot.** A tool's own `QMPSession` leaves the connection
in TIME_WAIT on the server side, and `port_guard` correctly refuses the
next boot on a held port. Take a **fresh slot per boot**
(`port_guard.find_free_instance()`) rather than waiting it out; until
that is understood it reads as "the second configuration does not boot".

**`DebugConsole.send()` returns on a quiet period, not on completion.**
So timing a command with it measures nothing, and output left in the
socket by a previous command comes back attached to the next one. Assert
on the guest's own state -- a file, a `ps` row -- not on how long a
`send()` took.

**A SPAWNED program's stdout never reaches the serial socket.** It goes
to the console framebuffer; the legacy `run` loader's output does come
back. So `sh <prog>` returns text you can assert on and `sh spawn
/bin/<prog>` returns only the loader's own lines. That decides a test's
shape: anything printing a table gets run through the legacy loader, and
anything needing a scheduler slot (i.e. anything that sleeps) gets
checked through the process table instead. **And fd 2 is the KERNEL
LOG**, so `sys_eprint()` from a ring-3 program reaches `dmesg` even when
its stdout is a framebuffer nobody can see -- which is the way to
instrument a spawned program.

**"Is it still running?" must read `ps`'s STATE column, not the name.**
The kernel shell's `spawn` does not reap, so earlier runs leave zombies
and `"name" in ps_output` answers yes forever. That check passed with
the feature under test wholly broken.

## What the emulator does and does not model

**`make run KVM=1` is not a straight speedup, and throughput numbers from
the two modes are not comparable.** Measured on the same disk image,
same host, `stress 150`: TCG 22.8 MB/s write / 29.2 MB/s read, KVM
12.1 / 18.7 -- KVM about 1.9x *slower* for disk I/O. Guest code that's
actually computing gets much faster, but every port-I/O instruction
becomes a hardware VM exit (~1us) where TCG services one in-process
(~tens of ns), and this kernel's disk path is dense with `inb`/`outb`.
So: useful as a second mode to test in, and useful for anything
CPU-bound, but always say which mode a benchmark came from --
`tools/vm.py --kvm` runs the same configuration headlessly.

**TESTING ON VIRTIO-BLK, and why CI does.** `ktest_run.py
--virtio-disk PATH` and `vm.py --virtio-disk PATH` attach a second disk
on virtio-blk; it then carries the filesystem, while the IDE drive
stays attached so the `[ata]`/`[atac]` suites keep a real drive instead
of skipping. `make run DISK=virtio` is the interactive form, and boots
with NO IDE controller at all -- so the filesystem mounts only if the
whole virtio path works. (`VIRTIO=1` also does it, along with the GPU
and input -- it is the switch over every device class, not a disk
flag.)

**The reason this is a CI job and not just an option:** it found a real
driver bug that reproduced NOWHERE locally. The runner has QEMU 8.2.2
against 11.1 here and its CPU makes the kernel pick a different
clocksource, and under that combination `virtqueue_poll()` timed out on
a budget that was ~12 ms rather than the 5 s it appeared to offer, then
let late completions permanently desync the used ring. Three
diagnoses were wrong before the instrumentation was good enough to
answer it. Which is the general point: **a second configuration is
worth more than a second run of the first one.**

**THAT PENALTY IS ATA'S, NOT KVM'S -- and virtio-blk removes it.**
Measured on the same host with `make run KVM=1 DISK=virtio`:

| | write | read |
|---|---|---|
| ATA, TCG (`stress 150`) | 22.8 MB/s | 29.2 MB/s |
| ATA, KVM (`stress 150`) | 12.1 MB/s | 18.7 MB/s |
| **virtio-blk, KVM** (`stress 100`) | **120.4 MB/s** | **74.6 MB/s** |
| **virtio-blk, KVM** (`stress 2000`) | **115.3 MB/s** | **72.5 MB/s** |

About **10x ATA's write throughput under KVM**, and it barely degrades
with size -- 2000 MB written, read back and verified byte for byte in
44 s, where the same work over ATA would have taken minutes.

The mechanism is the point, and it is why the numbers invert rather
than merely improve. ATA is slower under KVM because its path is dense
with `inb`/`outb` and every one of those is a hardware VM exit costing
~1us. A virtqueue has almost no port I/O at all: the driver writes
descriptors into shared memory the device reads directly, and the only
register access per request is a single doorbell store. So the exits
that made ATA slow under KVM simply are not there, and KVM's compute
advantage is left with nothing working against it.

Practical consequence: **quote the transport as well as the mode.**
"22.8 MB/s write" now means nothing without saying whether it was ATA
or virtio, and a throughput figure recorded before virtio-blk existed
is an ATA figure whether or not it says so.

**The other reason to reach for KVM has nothing to do with speed: it
HONOURS GUEST MEMORY TYPES and TCG does not.** Under `make run` a
write-combined or uncached framebuffer behaves exactly like cached RAM,
so an entire class of graphics performance bug cannot happen there --
which is what made the console's write-combined scroll regression
reproduce on the maintainer's laptop and under `make run KVM=1`, and
nowhere else. If a report is "slow only on real hardware", try
`python3 tools/vm.py --kvm run "gfxbench 20"` BEFORE concluding it is
untestable here; a previous session recorded exactly that conclusion and
it was wrong. See `docs/decisions.md`.

**`gfxbench` measures the framebuffer, and its number is MEANINGLESS
under QEMU.** QEMU's framebuffer is cached host RAM; real hardware's is
uncached MMIO, where every store is a bus transaction the CPU stalls on.
That makes a whole class of performance bug structurally invisible to
every test here -- a clean `gui_regress` says nothing about it. The
framebuffer is write-combined at display probe now (PAT, or an MTRR
under `nopat`), and `gfxbench` prints which mechanism is live beside its
timings. Expect ~2500 MB/s in emulation and treat that as evidence the
figure is not real. **If a user reports something that reproduces only
on hardware, ask what the emulator models differently BEFORE doubting
the report.** See `docs/decisions.md`.

**AND KVM HIDES A DIFFERENT CLASS AGAIN -- TIMING.** The memory-type
note above is about what TCG does not model; this is about what it makes
too slow to notice. Under KVM a transfer completes in microseconds, so a
driver race whose window is "between issuing a command and arming its
completion flag" goes from unreachable to constant. That is a real bug
this project shipped: `dma_issue()` cleared `g_dma_irq_fired` AFTER the
command byte, an interrupt landing in that window was wiped, and the
waiter then burned the whole 5s `DMA_WAIT_TICKS` budget before a retry
that succeeded instantly. The desktop froze for 2-5 seconds per disk
read under `make run KVM=1` and was perfect under `make run`.

Generalise it: **every automated test in this repo runs TCG**, so a
green suite says nothing about anything whose behaviour depends on how
FAST the emulated hardware is. When a user reports something this
environment cannot reproduce, try `--kvm` before doubting the report --
and note the report may be of a symptom (a freeze) whose cause is a race,
not slowness. `tools/kvm_soak.py` is the standing check for this.

**But `make run KVM=1` DOES honour guest memory types, and that is the one
way to reproduce this class of bug locally.** TCG ignores PAT entirely,
so a write-combined framebuffer behaves exactly like a cached one under
plain `make run`; KVM does not. That distinction is what turned "slow
only on the maintainer's laptop" into a measurement:
`python3 tools/vm.py --kvm run "gfxbench 20"` reported **178.5 ms** per
scrolled text line against **0.5 ms** once the console stopped reading
the framebuffer. Reach for `--kvm` before concluding a hardware-only
report is untestable.

**The framebuffer console draws into a back buffer and PUBLISHES
separately, and the invariant is that it never READS the framebuffer.**
Write-combining is a write optimisation that makes reads strictly worse
(uncached, no prefetch), so the console's old scroll -- shifting visible
pixels up in place -- became its slowest operation the moment WC landed.
It draws through `gfx.c`'s back buffer now and publishes with
`gfx_present()`. **The trap: a path that prints and then HALTS without
reaching a flush point leaves its text in RAM only.** The flush points
are `vga_present()` in `keyboard_getchar_mods()`'s idle loop, a
tick-throttled present at the end of `vga_putc()`, and an explicit call
on `idt.c`'s panic path -- add one to any new print-then-halt path.
Note there is deliberately no dirty flag in `vga.c`: `gfx.c` already
tracks the dirty box and `gfx_present()` no-ops when empty, so a second
copy could only disagree with it silently. See `docs/decisions.md`.


## Interactive runs, and the two extra ISOs

**WHICH MEDIUM A RUN BOOTS, because it is no longer always the ISO.**
`disk.img` carries GRUB and the kernel (`tools/install_grub.py`), so an
ordinary `make run` -- and every headless launch through `vm.py`,
`launch_qemu_cmd()`, `boot_smoke_test.py` or `serial_console.py` -- is
`-boot order=c` with **no `-cdrom` at all**. The ISO is still what the
live and demo images ARE, and what a release ships.

The choice is DERIVED from the image, by `install_grub.boot_medium()`:
an image with no GRUB on it (one built before this layout) boots the ISO
instead, so an old checkout keeps working. Override with `BOOT=disk` /
`BOOT=cd` on `make run`, or `--boot` on `vm.py`. Two consequences for
testing:

- **`make iso` is still the target to run before a headless test**, even
  though the ISO is not what boots. It is what re-seeds `disk.img` and
  reinstalls the kernel on it; `make all` reaches no medium at all.
  `tools/iso_guard.py` compares against `build/.bootdisk` on a disk boot
  and against `toy-os.iso` on a CD boot, so a stale one is refused
  either way.
- **A tool that builds its own image hardcodes `-boot order=d`** --
  `partition_test.py`, `virtio_boot_test.py`, `run_release.sh`. Nothing
  installed a bootloader on those, and omitting the order entirely is
  the worst option: SeaBIOS boots any disk with `0x55AA` at LBA 0 and
  then hangs inside the partition table with no serial output at all.

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
moves. Bit an actual user session once (see the commit for build 293's Makefile fix) -- looked exactly like a driver bug, wasn't one.


**The live and demo ISOs are SEPARATE artifacts, on purpose.** The
ordinary `toy-os.iso` carries no GRUB module: a 129 MiB one took the
boot smoke test from 1.6s to 7.0s locally and blew CI's 12s timeout
outright, because GRUB reads the whole module off the emulated CD-ROM
before the kernel starts. The live image is 24 MiB now (partial block
groups), so folding it back into the default ISO is possible -- but
measure the boot first. `tools/live_boot_test.py` drives the live one
(launch with `launch_qemu_cmd(disk=None)`, which omits `-drive`
entirely); the demo one is `data/wm/demo.script`, a text file on the
image, performed one step per WM iteration through `wm_debug_dispatch()`
-- the same path the GUI tests use.


## Driving the desktop: the apps built to be tested against

**There is a GUI app built to be tested against: "UI Demo"**
(`userland/gui/uidemo.c` -- a RING-3 process since Milestone 41's
stage 0; it was `apps/uidemo.c`). One of every Toykit widget at documented,
font-derived, content-relative offsets, and every interaction logged as
one parseable line (`uidemo: button 2`, `uidemo: check alpha on`,
`uidemo: cancel btn`). Combined with the `gui` commands below, a GUI
test becomes drive-and-assert over a single serial wire with no
screenshot in the loop -- and when a click lands on the wrong thing, the
log says which widget it actually hit. Read its top comment for the
layout table and the log grammar before writing coordinates by hand.

**Its ring-3 counterpart is "Shapes"** (`userland/gui/gfxdemo.c`, `run
shapes` from a Terminal) -- **two scenes, switched with the `2D / 3D`
button or `S`**: a rotating wireframe triangle and ellipse, or a
perspective-projected wireframe CUBE with depth-shaded edges. Both are
drawn with the shared geometry module, with the same one-line-per-state
log grammar (`gfxdemo: aa off`, `gfxdemo: scene 3d`) and self-reported
`gfxdemo: layout canvas <x> <y> <w> <h>` / `layout buttons <x> <y> <w>
<h> <pitch> <count>` -- take button positions from the second of those
rather than deriving them from the canvas rect. Use it as the known
target when testing
`kernel/lib/geom.c`, `uui_canvas`, or ring-3 drawing generally;
`tools/gfxdemo_test.py` drives it. **A ring-3 app's diagnostics go to
`sys_eprint()` (stderr), not `sys_print()`** -- stderr reaches the
kernel log and `dmesg`, where a test can read it, while a GUI client's
stdout goes nowhere useful (it has no terminal attached) and a spawned
process's stdout goes into its parent's pipe.

**`tools/gui_debug.py` -- ask the WM what it's doing, instead of
measuring a screenshot.** The serial debug console has a `gui` command
family now (`userland/wm/wm_debug.c`), live while the desktop is up:

```
gui windows [--json]     rects, content rects, z-order, focus
gui probe X Y [--json]   which window/region is at a point, and what overlay would take the click
gui menu | gui taskbar    row + button geometry, as the kernel computes it
gui ctxmenu [--json]     the OPEN right-click menu's rows, same shape as `menu`
gui state [--json]       overlays, cursor, armed drag/resize/press, damage rect
gui damage [verify on|off]  the damage rect; verify catches missed damage
gui open <App>           open a window directly -- no Start-menu clicking
gui dialog [--json]      the open confirm dialog's message and button CENTRES
gui compositor [--json]  the registered compositor pid, queue depth, drops
gui spawn PATH [args]    run a ring-3 binary directly -- no Terminal in the loop
gui watchdog [<ms>|off]  slow-frame threshold, plus how often it fired
gui pingtimeout [<ticks>] not-responding timeout -- a TEST LEVER, see below
gui kill PID             end a process -- `gui spawn`'s counterpart
gui click X Y | gui rclick X Y | gui drag X1 Y1 X2 Y2 | gui key <c> [alt|ctrl|shift]
```

**`gui pingtimeout` exists to make a test faster, and that is a
legitimate reason.** The WM marks a client not-responding after 3
seconds (`wm_internal.h`), and `tools/forcequit_test.py` waits that out
about ten times -- which made it a 72-second tool and therefore the whole
GUI suite's wall-clock floor, since a fan-out's wall clock is its slowest
item and not its total. The timeout's VALUE is not what that tool
asserts (its own docstring already treats raising it as a positive
control), so it turns it down to 0.4s and finishes in 34s, taking the
suite from 76s to 61s. Two rules if you use it: the tool must ASSERT the
knob took, or every wait silently becomes shorter than the thing it is
waiting for and the tool fails looking like the feature is broken; and
any negative check ("no dialog appears") must wait a MULTIPLE of the
timeout, so shrinking it cannot quietly weaken the proof. It is
deliberately not a registered setting -- it has no user-facing meaning,
and a persisted value would change how the desktop treats a slow app on
every later boot.

`gui spawn` is the ONLY way a test starts a client now -- since M41's
stage 0 there is no kernel-space Terminal to type `run <name>` at, and
`gui open Terminal` spawns the ring-3 one, whose window does not exist
yet when injected keys arrive. `DebugConsole.spawn(path, title)` wraps
it, waits for the window and gives the client a moment to present its
first frame; a capture taken before that reads DESKTOP through the
window's rect. It also takes ARGS now (`gui spawn /tests/spin_test 40`).
It is the one to reach for when a test needs a ring-3 client:
`gui open` can only launch what is in the app registry, so tests used to
open a Terminal and type at it, dragging that Terminal's allowlist, its
pending-process slot and its shell into a test about something else --
and a client that never exits could not be tested at all, because the
Terminal that spawned it then could not be closed. `gui dialog` reports
the confirm dialog's buttons; do not scan for them by colour, which
assumed "Yes"/"No" sizing and breaks on "Force Quit"/"Wait".

`rclick`/`ctxmenu` are newer than the rest and exist for a specific
reason: no test could open a context menu at all, which is how its
Close row went on destroying ring-3 windows without their close
handshake while the X button beside it asked politely. Use
`DebugConsole.ctxmenu_row("Close")` rather than deriving a row from
`item_h`. And note `gui key` takes modifier words -- `gui key 0xa5 alt`
is Alt+F4, which is the only way to close a window from a test now that
Esc doesn't.

**`gui damage verify on` catches the WM's worst bug class.** The
compositor only repaints declared damage, so anything that changes on
screen without being declared leaves stale pixels -- no crash, no
assertion, often visible in one interaction only. Verify mode renders
every frame twice (damage-limited, then unrestricted) and reports any
differing pixel with coordinates. It found four real bugs in its first
minute. **Read the whole report line before believing it**: it also
carries the diff's BOUNDING BOX (63 px are a caret, a border or a
scrollbar depending on their extent) and a verdict from a THIRD render
of the same frame -- `scene stable (real missed damage)` means the
comparison is trustworthy, `SCENE UNSTABLE -- verdict void` means
`render_scene()` disagreed with itself and the report proves nothing.
See `docs/decisions.md`. Turn it on whenever you touch drawing, damage, focus or
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

**`DebugConsole.capture_panic()` is how you read a dying guest.** A
panicking kernel never returns a prompt, so the normal command/response
cycle cannot complete and every read looks like a timeout; sending an
empty line and taking what arrives before the read gives up is the
panic block. **Do NOT open a second connection to the serial socket** --
the console already holds it and the new one receives nothing, which
cost three attempts before the helper existed.

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


## The fast boot check, and CI

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
make all && make iso` plus `check_deps.py`, `check_layout.py`,
`boot_smoke_test.py`, `ktest_run.py` and `usertest_run.py` on every
push/PR
to `main` -- so a build break or boot regression is caught
automatically, independent of whether a session (or a human) remembered
to verify locally first. This doesn't replace verifying locally before
delivering a change (still do that -- see "Working in the cloud
sandbox" above), it's a second, automatic check behind it.


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
independently hand-rolled similar scripts, never committed them, and
paid the cost of hitting these gotchas fresh each time -- that's why
this module exists now. Import it
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
- **That VNC head is also how the user WATCHES a headless run live**, and
  it needs no setup or flag -- the head is always there. The display
  number is derived from the VM slot: `vm.py start` (slot 0) and
  `qmp_test.py`'s `launch_qemu_cmd()` default are `:5` (TCP 5905),
  `vm.py --instance N` is `:5+N`, and `gui_regress.py`'s four parallel
  slots are `:5`-`:8`. Any viewer works (`remmina -c vnc://localhost:5905`
  is what's installed on the maintainer's machine -- but that URI form
  cannot be view-only; use `tools/watch_vm.sh` below, which launches a
  saved profile that is). **View-only matters**: a connected viewer's real mouse motion goes into the same
  emulated PS/2 device the synthetic input uses, and the two fighting
  looks exactly like a flaky test rather than like interference.
  Attaching or detaching mid-run is free.
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
  tool. It hands QEMU an ABSOLUTE path on purpose --
  QEMU resolves `screendump`'s filename against its own working
  directory, and `-daemonize` leaves that somewhere other than the repo,
  so a relative path reports `{"return": {}}` (success) and writes the
  file somewhere else; the only symptom is Pillow raising
  `FileNotFoundError` on a path that looks obviously correct.

**Screenshots are a TESTING TOOL, not a deliverable.** Take as many as
a check needs, into a scratch directory. Do NOT save them into
`screenshots/` as evidence -- that convention is retired (2026-08-15, at
the maintainer's request: the artifacts were not being used and
producing them slowed the loop down). `screenshots/` is kept for what
is already in it, not added to.

Show the user a screenshot when SEEING it is the answer -- a layout
that has to be looked at, a rendering question a number can't settle.
Don't attach one to prove a check passed: `docs/gui-guidelines.md`
already asks for pixel values with a control point (`pixel_probe.py`),
and a pass/fail table from `gui_regress.py` is better evidence than an
image, because a reader has to interpret the image and can only read
the table.

