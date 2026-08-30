# Decisions: Drivers and hardware

Display, disk and input drivers, and the registries they announce themselves through.

Part of **[docs/decisions.md](../decisions.md)**, which indexes every
decision in this project and is GENERATED from these files -- run
`tools/gen_decisions_index.py` after adding an entry here, or
`tools/check_docs.py` will fail.

Write the reasoning HERE, in full: an entry that cannot be understood
without opening something else is not finished.

---

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
`pci.h`'s doc comment, not just here. the commit for build 470
for how this was root-caused (PIO-vs-DMA comparison, then a host-side
pre-seeded disk image to isolate the read path and trace the bounce
buffer).

## The DMA bounce buffer is 64KB because that's one PRD, not because 64KB benchmarked well

`ata.c`'s `DMA_BUF_FRAMES` is 16 (65536 bytes), which sets
`ATA_MAX_SECTORS_PER_XFER` to 128. The number comes from the hardware
interface, not tuning: a Physical Region Descriptor's byte-count field
is 16 bits, with 0 encoding 64KB, so 64KB is the largest single-PRD
transfer possible and 129 sectors would truncate to a genuinely wrong
value rather than a clamped one. Going bigger means scatter-gather --
multiple PRD entries -- which is a real feature, not a constant change.

Two related choices: the allocation failure path retries for the
original 2 frames rather than dropping to PIO (a smaller DMA window is
still far better than no DMA), and `ata_max_sectors_per_xfer()` reports
the runtime value separately from the compile-time maximum so callers
that batch (TFS2's coalescing) adapt instead of assuming. See
the commit that added it.

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
mechanism, since CF8/CFC is universally supported including by QEMU's
emulated chipset. **The original reason has since expired** and the
decision has not: that argument was "ECAM needs ACPI/MCFG table parsing
just to find its base address", and `kernel/acpi/` now parses tables and
lists MCFG where the chipset has one (q35 does; i440fx does not). What
is left is the argument that still holds -- CF8/CFC works on every
machine including the ones with no MCFG, and ECAM buys extended config
space, which nothing here reads. BARs are decoded (I/O-vs-memory, base address) but
NOT size-probed (the write-0xFFFFFFFF-and-read-back trick) -- that's
deferred to whichever future driver actually needs to map a BAR, since
it means temporarily disabling the device's decode and isn't needed
just to enumerate/identify what's present. See `pci.h`'s top comment
and the commit for build 390 for the full writeup -- this was the
first concrete milestone toward the TCP/IP prerequisites README.md's
**Build 380** entry laid out.

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
textfield, editor) and `userland/tests/echo.c` all
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
fixed. the commit for build 501 for the full writeup.

## Keyboard layouts are data files (`/etc/kbs/<name>`) generated from Linux's own XKB data, not a compiled-in enum

The original `se` layout only remapped the three Å/Ä/Ö keys -- everything
else stayed identical to `us`, including keys whose physical legend is
genuinely different on a real Nordic keyboard (the key beside right
Shift types `-`/`_` on a physical FI/SE keyboard, not `/`/`?`). Rather
than hand-fix scancodes one bug report at a time, layouts moved to
`/etc/kbs/<name>` data files, generated by `tools/gen_kbs.py` from
`xkbcli compile-keymap` (Linux's own, already-correct XKB layout
compiler -- no X server needed) instead of anyone re-deriving a
scancode chart by hand. Translation logic itself moved out of
`keyboard.c` into a new `kernel/lib/keyboard_layout.c`, since owning
per-region character tables was never really the driver's job (raw
scancode/shift-state handling is). See the commit that added it for the full implementation, including the AltGr/dead-key scope
limits (this driver has no AltGr handling at all, so those symbols
were never reachable regardless of the table) and a real bug the
generator's first cut had (omitting Escape/Backspace/Tab/Enter from
its key list, which silently broke Enter the moment the shell started
loading layouts from generated files instead of the old compiled-in
ones -- found live, not by review).

## GDB debugging: QEMU's built-in stub, not an in-kernel serial protocol implementation

`make debug` (`CLAUDE.md`'s "Debugging with GDB" section) boots toy-os
frozen at CPU reset (`-s -S`) so a real `gdb` on the host can attach
via `target remote localhost:1234` -- real breakpoints, single-step,
register/memory inspection. This is QEMU's own built-in GDB remote
stub: QEMU emulates the CPU directly, so it can expose full debugger
control over whatever's running in the guest without the guest OS
needing to implement anything at all.

Worth stating explicitly because the first framing of this idea (a
`/btw` suggestion) got it wrong: it proposed toy-os's kernel would need
to "speak the GDB remote serial protocol" itself -- real, substantial
protocol work (packet framing, register/memory read-write commands,
breakpoint handling) on top of `kernel/core/debug_console.c`'s existing
scope (a handful of if/else-dispatched diagnostic commands). That's
simply unnecessary: `-s`/`-S` are ordinary QEMU flags, no different in
kind from `-vnc`/`-serial file:...` already used throughout
`tools/qmp_test.py`'s testing setup, and they work today with zero
toy-os code changes. Confirmed directly, not just asserted: `break
kernel_main` + `continue` over a real `gdb` session correctly ran the
CPU from reset through GRUB/multiboot2 and stopped exactly at
`kernel_main`, with a real backtrace showing source file/line.

The one actual gap, now closed: `CFLAGS`/`USERLAND_CFLAGS` never
carried `-g`, so `kernel.bin` and every userland ELF had zero DWARF
debug info -- GDB could still technically attach, but would only ever
see raw addresses, no function names or source lines, making
`break kernel_main`-style debugging impossible. Added `-g` to both,
kept at `-O2` rather than dropping to `-Og`/`-O0` for a separate debug
build -- same binary as always, just now carrying symbols, at the cost
of some locals showing "optimized out" in GDB. A real, deliberate
build-config-simplicity tradeoff, not an oversight.

## ATA's waits are bounded by wall-clock in one context and a spin count in the other

Every wait in `kernel/drivers/ata.c` that can block looks like it's
written twice, and the duplication is deliberate. A wall-clock budget
needs `pit_ticks()` to advance, and it doesn't inside a syscall: `int
0x80` is wired as an interrupt gate, so IF stays clear for the whole
handler and no timer IRQ ever increments the counter. A wall-clock loop
reached from there wouldn't time out, it would hang the machine. So
`wait_dma_irq()` and `wait_not_busy()` both branch on
`isr_in_progress()` (`idt.h`) -- real elapsed time when it's safe, a
fixed `ATA_POLL_LIMIT` spin when it isn't. Same split, same reason, as
the `hlt`-when-safe/poll-when-inside-a-syscall rule in the entry on
blocking I/O waits above.

**Why this is worth an entry rather than just a comment:** the two
bounds were written years apart in project time, and for a long stretch
only the completion wait had the wall-clock half. `DMA_WAIT_TICKS` was
deliberately *widened* to 5s to absorb host-side I/O stalls, while
`wait_not_busy()` sat at a fixed 100000-iteration spin -- which measures
out to ~12ms, giving three retries ~37ms in total. The driver was
therefore 135x more patient about a command in flight than about a
drive still finishing the previous one, and a host stall (a Btrfs
commit, an ISO being written to the same disk) hit the impatient half.
The general lesson is the one worth carrying: **a spin count is not a
duration.** It measures the guest CPU, which keeps running at full
speed during exactly the host-side stalls it's supposed to absorb, so
any timeout that must survive one has to be denominated in real time.

See `ata.c`'s `wait_not_busy()`/`DMA_WAIT_TICKS` comments and
the git history.

## The PIO fallback is reachable on purpose (`ata nodma`), because unreachable fallback code is a guess

`kernel/drivers/ata.c` has two transfer paths: Bus-Master DMA, and a PIO
fallback for machines where DMA can't be brought up. `ata_init_dma()`
succeeds under QEMU and on ordinary PC hardware -- so the fallback had
never executed on any machine this OS boots, and there was no way to
make it. Roughly a hundred lines of driver that only run in an
emergency, and had never been observed running at all.

`ata_set_dma_forced_off()` (the `ata nodma on|off` command) exists to
close that. It is not a debugging convenience bolted on: it's what makes
the path testable, and `kernel/drivers/ata_test.c` drives the same
switch so PIO executes on every `make test`. The first time it ran, it
worked -- which is the outcome that was *hoped for* before and merely
assumed.

It has a second use that isn't hypothetical. Comparing a known-good PIO
transfer against a suspect DMA one is how an earlier session root-caused
a DMA failure to a host-side filesystem stall rather than a driver bug;
at the time that comparison required hand-editing the driver.

**The implementation detail worth keeping:** every place that asks "DMA
or PIO?" goes through a single `dma_in_use()` helper rather than testing
the flags itself. `ata_max_sectors_per_xfer()` reports a smaller cap on
PIO (8 sectors vs 128), and `tfs.c` batches block writes against that
number -- so a dispatch site that disagreed with the cap site by even
one condition would hand the PIO path a transfer it cannot carry. One
helper makes that disagreement unexpressible.

Related, same file, same session: `wait_drq()` now records *why* it
failed in `g_pio_fail_reason`, mirroring `g_dma_fail_reason` -- a
pass/fail return for control flow, a reason string for whoever reads
`dmesg`. See `ata.c` and the git history.


## Colour is an escape sequence the console parses, not a syscall

`/bin/ls` coloured directories by calling `SYS_SET_COLOR`, which sets the
console's current attributes directly. That works exactly as long as the
program's output goes to the console -- and `ls > out.txt` recoloured the
console while its bytes went to the file, while `ls | cat` coloured
whatever the console happened to be printing at the time. The colour was
travelling on a side channel, so it landed somewhere other than the text
it belonged to.

An escape sequence travels IN the byte stream, so it lands wherever the
output lands. That is why every real terminal does it this way, and it
is what makes `--color=never` a thing a program can meaningfully offer:
with a syscall there is nothing to suppress, because the escape never
existed as data.

**Where the parser sits, and why.** `kernel/lib/ansi.c`, driven from the
top of `vga_putc()` -- BEFORE the output-sink check. One parser serves
the physical console and any installed sink (a GUI Terminal's
scrollback), and neither of them knows what an escape is. Putting it
after the sink check would have meant a second parser for the second
surface, which is the drift this repo keeps paying to avoid. The parser
itself touches no hardware, which is what lets a KTEST exercise it with
no display at all.

**What is implemented is SGR only.** Colour and attributes. Cursor
movement, clearing and scrolling regions are RECOGNISED and swallowed,
not printed -- a terminal that cannot do something should decline
quietly rather than spray `[2J` across the screen, and that swallowing
is the parser's third state rather than an afterthought. Real terminal
emulation belongs to the TTY milestone; this console owns its own cursor
and scrollback, and a program moving the cursor would fight both.

**The mapping is a table because the two orders differ.** ANSI counts
0..7 as black/red/green/yellow/blue/magenta/cyan/white; VGA's bits are
blue-green-red, so ANSI's 1 (red) is VGA's 4 and VGA's 1 is blue.
Arithmetic here would be a bug waiting for somebody to simplify it.
Index 7 maps to `VGA_LIGHT_GREY` rather than `VGA_WHITE` so that its
bright form is white instead of running off the end of the 16-colour
palette.

**What is deliberately missing: `isatty()`.** `--color=auto` means
"colour to a terminal, plain to a pipe", and nothing here can ask what
an fd is -- the kernel knows a description's kind (`syscall_fd.c`) and
exposes it to nobody. So `/bin/ls` offers `never` and `always` and
defaults its FORMAT to one entry per line, which is the shape safe to
parse. Making columns the default broke Notepad's dialog test the same
day, because that test reads `ls /` a line at a time -- which is exactly
the pipe case a real ls would have detected.

## A guest spin-wait without `pause` starves the host thread it is waiting for

`virtqueue_poll()` busy-waits for the device to publish a used-ring
entry. It spun with no `pause` instruction, and under KVM that is not a
missed optimisation -- it is a hang.

The reported symptom: on `make run KVM=1 DISK=virtio` with an SDL
display, the FIRST `VIRTIO_BLK_T_FLUSH` of a run never completed. Reads
and writes went through; only flushes timed out, with 254 of 256
descriptors still free, so nothing was backed up. TFS3 then abandoned
every journal transaction, correctly.

**Why only virtio, and only flush.** Three properties have to line up,
which is why nothing here caught it:

  * `chain_done()` reads `vq->used->idx` -- PLAIN GUEST RAM. Every other
    polled path in this kernel (ata.c, ahci.c) reads MMIO or a port,
    which always traps to the hypervisor and therefore yields. The
    virtqueue is the only spin that can run without ever exiting.
  * A FLUSH is the only request whose completion waits on the HOST's own
    fsync, run by QEMU's main-loop thread. A read or a write is usually
    satisfied from the host page cache and completes fast enough that
    even a starved main loop gets there.
  * KVM's Pause-Loop Exiting is how a hypervisor notices a spinning
    guest and schedules something else, and it triggers on `pause`. A
    spin without one is indistinguishable from useful work, so the vCPU
    keeps its whole timeslice -- starving the very thread that would
    complete the request. With an SDL display that thread is also doing
    the rendering, which is what made it reproducible on one machine and
    not another.

Under TCG none of this applies: the emulated vCPU yields constantly and
the host is never starved. **A suite that is entirely TCG is
structurally blind to this class**, which is the general lesson and the
reason `tools/kvm_soak.py` exists.

**CONFIRMED BY THE FIX, not by a reproduction.** It was never reproduced
on the machine it was diagnosed on -- that host had no display for SDL,
and six attempts under KVM with virtio as the root all passed, including
the reporter's exact device mix and a positive control that cut the
poll budget by a thousandfold and still did not fire. The diagnosis was
inference from four facts the log made available: the first request in
flight (254 of 256 descriptors free, so not a queue running down),
flushes only, KVM only, and one machine only. Adding `cpu_relax()` fixed
it on the reporting machine first try. Worth knowing because the commit
that introduced it says "not reproduced" and a later reader would
otherwise be right to distrust it.

**AND THE FIRST VERSION OF THE FIX COST A THIRD OF WRITE THROUGHPUT.**
Pausing from the first iteration is free on an IDLE host -- measured
identical, 3470 against 3397 KB/s -- and is not free on a busy one: PLE
yields the vCPU, and getting rescheduled behind a QEMU thread rendering
at 1080p takes milliseconds. Reads barely noticed, because the host page
cache answers before a spin gets going. Writes collapsed to 0.2 MB/s and
16 ms per 4 KiB operation -- a scheduling round trip, not a disk --
because a journal barrier is a real host fsync and so is the one wait
that actually reaches the backoff. **The property that makes `pause`
necessary is the same one that makes it expensive**, so it has to be
earned: `VIRTQ_SPIN_TIGHT` iterations without it first, which covers any
completion the host already has in hand, and `pause` only once the wait
is clearly long. Every adaptive spinlock has this shape for this reason.

`cpu_relax()` (barrier.h) is the fix, and it helps twice: PLE can now
deschedule the spinning vCPU, and each iteration costs tens of cycles
instead of a few -- so `VIRTQ_POLL_BACKSTOP`, which is an ITERATION
COUNT rather than a duration, is worth roughly an order of magnitude
more wall-clock time for free. That is the honest mitigation for a bound
whose own comment admits it "is worth more or less time on a faster or
slower machine".

## virtio: one shared transport, modern-only, polled

**Why a shared core rather than a self-contained virtio-blk.** Every
virtio device -- block, net, GPU, entropy, input -- speaks the same
transport: the same PCI capability discovery, the same feature
negotiation, the same ring. Only the payload differs. Writing that once
means the next device is a `.c` file rather than a bring-up project,
which is what `docs/roadmap.md` means by "every item below depends only
on this". Linux splits it the same way (`drivers/virtio/` beneath the
drivers that use it) and so does Windows' virtio-win (one `VirtIOLib`
inside viostor and netkvm).

What was deliberately NOT copied is their size: no bus type, no
driver-match table, no probe/remove callbacks, no vtable over several
transports, no packed ring, no indirect descriptors, no event-index
suppression, no MSI-X, no DMA/IOMMU layer, no hotplug. A driver here
scans PCI in its own init and calls the transport functions directly,
which is `vmsvga.c`'s existing precedent. The core is ~600 lines
against Linux's ~9,000 for the equivalent.

**Note the two axes.** The virtio core is orthogonal to the class
registries this kernel already has. virtio-blk plugs into
`block_device` exactly as ATA does; a later virtio-gpu plugs into
`display_driver` beside vmsvga. There is no "virtio registry", because
the thing a device needs to be discoverable is a registry for its
CLASS, and those already exist. virtio-net is the one that will need a
new one, since nothing here describes a NIC yet.

**Modern-only (virtio 1.x), and a device offering no
`VIRTIO_F_VERSION_1` is refused rather than half-driven.** The legacy
0.9.5 I/O-port layout would be a second path that nothing exercises,
and this repo's standing rule is that an unexercised path is an
unvalidated one. Refusing in one place (`virtio_begin()`) is what keeps
that decision from leaking into every device driver.

This does NOT mean only modern DEVICES work, and the distinction cost
real time to discover: QEMU's `virtio-blk-pci` defaults to
`disable-legacy=auto`, so `-drive if=virtio` on the default pc-i440fx
machine produces a **transitional** device -- PCI id `1af4:1001`,
revision 0 -- not the modern `1af4:1042`. A driver matching only
`0x1040 + type` finds nothing on the most natural command line anyone
would type, and the feature looks silently broken. The type is
therefore read from the PCI device id for a modern device and from the
SUBSYSTEM device id for a transitional one; the transitional device is
then claimed through its modern half.

**Polling, not interrupts, for now.** An interrupt would only be the
device saying "used->idx changed", which the poll reads directly -- so
polling is a correct implementation rather than a shortcut. It costs
CPU, and it buys proving the ring, the DMA and the descriptor chaining
before legacy INTx is introduced as a second thing that can be wrong.
INTx here is level-triggered and shared, and `irq.c`'s handler table is
one-per-line with a silent replace on re-registration, so a second
virtio device sharing ATA's line would make ATA's handler vanish with
no error. That is a real change, and it is separable. `virtio_pci_find()`
sets `PCI_CMD_INTX_DISABLE` to match, which is the one line the
interrupt work will delete.

MSI-X, which is what Linux actually uses and which sidesteps sharing
entirely, needs a local APIC, an IOAPIC and IDT vectors above 47 --
this kernel installs 0-47 plus `0x80`. That is an APIC project wearing
a virtio hat, and is deliberately out of scope.

**No BAR size probing.** virtio 1.x gives every capability a
`bar`/`offset`/`length` triple, so a driver never needs the BAR's
extent. Size probing exists so an OS can ASSIGN addresses, which
SeaBIOS did before GRUB loaded us.

**Which disk wins, and why it is decided in `vfs.c`.** `blk_register()`
is last-writer-wins, so order alone would decide it somewhere nobody
looks.

It shipped conservative -- virtio only when ATA had no disk -- on the
reasoning that a real installed system must not be quietly displaced.
**That was reversed once there were numbers.** virtio-blk is preferred
whenever a virtio disk is attached, and ATA is the fallback and the
legacy path:

- ~10x ATA's write throughput under KVM (`docs/testing.md` has the table)
- the kernel test suite runs 6.4 s on virtio against 11.9 s on ATA
- and it passed 3 runs in 3 where ATA passed 2 in 3, on a clean disk
  each time -- ATA's remaining flake being a host-I/O stall its ~1 s
  pre-issue budget cannot absorb, which a virtqueue is not exposed to

A machine with only an IDE disk is unaffected: there is no virtio
device to prefer. `novirtio` on the boot line forces ATA, which is what
keeps the legacy path reachable and therefore tested -- the same reason
`nopat` and `notsc` exist.

**FAULT INJECTION MOVED TO THE BLOCK LAYER because of this.** The four
filesystem error-path KTESTs armed `fault_fail_next_ata_writes()`, an
ATA-specific injector, so the moment the filesystem was mounted on
virtio they stopped testing anything -- the writes they expected to
fail simply succeeded, and all four failed. That is this repo's
recurring "an interface with one implementation is unvalidated" problem
pointed at the test infrastructure: only one backend could be made to
fail on demand, and virtio-blk's error paths could not be tested at
all.

`blk_read_sectors()`/`blk_write_sectors()` now consult
`fault_should_fail_block_read/write()`, so any backend can be failed.
The ATA-specific pair STAYS, and that is not redundancy: ATA's
write-back cache sits below the block layer, so "the drive refused this
write" happens on a path `blk_*` cannot reach -- which is why ATA
already had two injection points rather than one.

A rating field like `clocksource`'s was rejected. With two block
devices a rating is a number nobody can justify, and a mechanism with
one real user. It becomes the right answer at three.

**Capabilities from negotiated features, unlike `block_ata.c`.** ATA
advertises `BLK_CAP_FLUSH`/`TRIM` unconditionally and refuses per call,
because identify data is not necessarily settled when its adapter
registers. virtio feature negotiation has already completed by then and
definitively answers "can this device flush", so here the bit can mean
exactly that. The divergence is the point, not an inconsistency.

**A timed-out chain's descriptors are leaked on purpose.** The device
still owns those buffers and may write into them at any later moment,
so returning them to the free pool would hand a live DMA target to the
next request. The queue runs down and then refuses, which is the right
end state for a device that has stopped answering; the alternative is
silent corruption. `virtqueue_lost_chains()` counts them.

**Init ordering is load-bearing.** `virtio_blk_init()` runs after
`heap_init()`, not beside `pci_init()` where a PCI-scanning driver
otherwise belongs: a virtqueue's rings come from
`pmm_alloc_contiguous()`. Placed early it found its device, negotiated
features, and then failed with "queue 0 needs 3 contiguous frames and
none were free" -- which reads as a device problem rather than an
ordering one.

**What is knowingly not right.** The register windows are reached
through the identity map, which is write-back cached; device registers
should be uncacheable, and the only memory-type control this kernel has
is `paging_set_write_combining()` (also wrong -- WC gathers and
reorders, which is fine for a framebuffer and fatal for a doorbell).
TCG ignores PAT entirely, so this is invisible here and would only
matter on hardware. Recorded rather than papered over; a
`paging_set_uncacheable()` is its own change.

Likewise a BAR above 4 GiB is refused with a message rather than
supported, because this kernel identity-maps only the low 4 GiB and has
no kernel-range mapper. Not reachable on pc-i440fx, where SeaBIOS fits
the 16 KiB virtio BAR into the 32-bit hole.

## virtio-rng SEEDS krandom, it does not serve it

**Why an entropy device at all.** Under QEMU's default CPU model the
guest has neither RDSEED nor RDRAND, so `krandom` falls back to TSC
jitter -- and the timestamp counter it measures against is itself
software under TCG, which makes that fallback weakest in exactly the
environment this OS usually runs in. `virtio-rng` is real host entropy,
one `-device virtio-rng-pci` away, on a transport that already existed.

**Seeding, not serving, and that is how everyone does it.** A draw is a
device round trip plus a spin-poll, so `krandom_u64()` cannot be one.
The device registers as a krandom SOURCE instead
(`krandom_register_source()`): mixed in once at registration and again
every `KRANDOM_RESEED_DRAWS` draws. Linux splits it identically --
virtio-rng is an hwrng that reseeds the CRNG, never the per-call
generator -- and Windows' viorng feeds CNG rather than answering each
request.

**The seam is a callback, not an include.** `krandom.c` knows nothing
about virtio; the driver hands it a `fill` function. That is what lets
the entropy source arrive long after `krandom_init()`, which it must:
a virtqueue needs PCI, the frame allocator and contiguous frames, none
of which exist that early in `kernel_main()`.

**Which means the stack canary does not get it.** `stack_guard_randomize()`
runs before `pmm_init()`, so the canary keeps whatever `krandom_init()`
had -- jitter, on a machine with no RDSEED. Reordering boot so the
canary could use the device would put the frame allocator ahead of the
guard-page work that depends on it, to improve a value that is drawn
once and never rotated. Everything drawn after boot gets the device.

**`KRANDOM_VIRTIO` sits between JITTER and HW, and the enum is ordered
by trust** so that registering a source is a comparison rather than an
assignment (a CPU with RDSEED does not become less trustworthy because
a virtio device turned up). Below HW because RDSEED is the CPU's own
source, available per draw with no device round trip and no third
party; above JITTER because host entropy is real and emulated jitter
may be nearly deterministic. The hypervisor is not a new party to
trust -- it already owns this machine's memory.

**The driver probes before it registers.** A present-but-wedged device
that raised the reported quality to virtio-rng while contributing
nothing would be the one lie this tier must not tell, so
`virtio_rng_init()` completes a real 8-byte request first and stays
unregistered if it fails.

**The bounce buffer is not an optimisation.** virtio-blk hands the
device the caller's buffer after checking it is below 4 GiB; that is
wrong here, because a caller may pass a kernel stack address and kernel
stacks are mapped with a guard page rather than identity mapped -- so
their virtual address is not their physical one. Filling through a
64-byte static removes the question.

**Testing it cost a second device, and that is measured rather than
tidy.** The transport KTESTs in `virtio_test.c` claim a device and
reset it when they are done. Once a driver holds the rng for the life
of the kernel, running them against that device kills the live entropy
source: with the tests pointed at index 0, "krandom: the registered
entropy source declined a reseed" appears in `dmesg` a few hundred
draws later, and nothing else says anything at all. So
`tools/serial_console.py` attaches TWO `virtio-rng-pci` and the tests
claim the LAST one. The decline line exists for the same reason -- it
is the only way a source that has stopped answering is visible, since
a failed reseed is otherwise indistinguishable from a busy one.

## virtio-gpu: a display driver that had to move the boot order

**Why it is two files.** `kernel/drivers/virtio/virtio_gpu.c` speaks the
command protocol; `kernel/drivers/display/display_virtio.c` adapts it to
the `display_driver` registry. That is the same split virtio-blk already
makes (`virtio_blk.c` plus `block_virtio.c` for `block_device`), and it
is the two-axis shape this repo keeps returning to: what a device SITS
ON (the virtio transport) is not what it PLUGS INTO (the class
registry).

**pmm_init() moved, and that is the interesting part.** Every display
driver before this one probed with nothing but port I/O and a BAR, so
`display_probe()` ran long before the frame allocator existed. A virtio
device cannot: its virtqueues come from `pmm_alloc_contiguous()`, and so
does the framebuffer it is about to own. `pmm_init()` depends only on
the multiboot memory map and the kernel's own symbols -- both true from
the first instruction of `kernel_main()` -- so it moved ahead of the
display block, which is a one-line change with no new mechanism.

The alternative was a late handover: boot on vesafb, bring virtio-gpu up
after the heap, then re-point the active driver. That needs `gfx` to
re-read its surface, the console and the compositor to repaint, and it
has to deal with a ring-3 compositor already holding a framebuffer
grant. Moving a call that had no dependencies was smaller by a wide
margin.

**The framebuffer is CONTIGUOUS because gfx needs one linear mapping.**
`RESOURCE_ATTACH_BACKING` takes a scatter-gather list and would happily
accept scattered frames, but this kernel's only kernel-side mapping is
the identity map of the low 4 GiB, so physically contiguous is what
makes it virtually contiguous. One allocation, one memory entry.

**No DISPLAY_CAP_MODESET, even though it sets modes.** It programs a
mode at probe, which is what makes `video=<W>x<H>` work on a device
where GRUB's own mode list would have decided instead. What it cannot do
is change mode AFTER boot: `virtio_gpu_set_mode()` allocates a new
framebuffer and frees the old one, while `gfx.c` caches the surface
pointer from `gfx_init()` and `win_surface.c` has mapped those exact
frames into the compositor. A live mode change would leave both writing
into freed memory. Advertising the capability and returning success
while handing the system a dangling framebuffer is precisely the failure
`display_probe()`'s honesty check exists to prevent, arriving from the
other side -- so the capability is not claimed, and the runtime switch
is a roadmap item that needs a gfx re-init and a compositor re-grant.

**The mode LADDER wins over the host's preference, which looks
backwards.** `display_mode_candidate(0)` IS `video=<W>x<H>` when that
flag was given, and a flag the user typed must not be silently overruled
by what QEMU happens to be showing. `GET_DISPLAY_INFO`'s preferred rect
is the last resort instead. QEMU resizes its window to whatever scanout
the guest sets, so honouring the flag costs nothing.

**The cursor plane is real and (since 2026-08-29) the compositor is its consumer** -- see `docs/decisions/gui.md`'s hardware-cursor entry for the protocol and the handover rules. The paragraph below records the state it was built into. virtio-gpu's
second queue carries `UPDATE_CURSOR`/`MOVE_CURSOR`, and this driver
implements them -- but the ring-3 compositor draws a software sprite, so
`gfx_hw_cursor_*()` had ZERO callers and had had none since vmsvga
introduced them. A capability with no caller is a capability nobody
would notice being wrong, so the shell gained `hwcursor`, a diagnostic
that defines, moves and hides a square through the plane. The
compositor actually using it is a roadmap item: the WM's damage
bookkeeping (`prev_cursor_*`, `damage_cursor()`) has to stand down for
the pointer, which is a change to the careful part of the renderer
rather than to a driver.

**A HARDWARE CURSOR IS INVISIBLE TO `screendump`, and that is not a
harness bug.** QEMU hands a device-composited cursor to the display
client out of band -- the VNC cursor pseudo-encoding, an SDL cursor --
exactly as a real GPU hands it to scanout hardware. It is never drawn
into the surface `screendump` captures. So `tools/virtio_gpu_test.py`
asserts what CAN be observed: the commands complete, and showing the
pointer repaints no framebuffer pixels (which is the property a plane
has and a sprite does not). Anyone reaching for a pixel assertion here
is asserting that the cursor failed to be a cursor.

**Polled, like every other virtio device here.** Linux's `drm/virtio`
completes on an interrupt with fences; this transport deliberately has
no interrupts yet (see `virtqueue.c`), and the used ring says the same
thing at the cost of CPU. Measured under TCG, a full-screen `gfxbench`
frame is FASTER on virtio-gpu than on stdvga (3.0 ms against 3.9 ms),
because the framebuffer is ordinary guest RAM rather than emulated MMIO
and the two round trips per flush cost less than the MMIO writes they
replace. That is a TCG number and says nothing about hardware.

**How it is tested, and why it needed a tool of its own.** Nothing else
here boots with a virtio GPU -- every GUI tool and `make test` launch
`-vga std` -- so the driver's KTESTs would have skipped on every run,
which is worse than having none because the suite stays green either
way. `tools/virtio_gpu_test.py` supplies the hardware and checks both
halves: `ktest virtio-gpu` inside the guest for the driver's own state,
and the pixels from outside for what actually reached the screen. Its
oracle for the PIXEL FORMAT is a second boot on `-vga std`: the same OS
drawing the same desktop on a known-good layout, which is the only check
that survived the positive controls -- "is anything on screen" passes on
a black screen, and a channel-order test passes on a format that rotates
channels rather than swapping two.

## AHCI enumerates every port and drives one, and NCQ is reported rather than used

An AHCI host bus adapter owns up to 32 ports, each with a 32-slot
command list, and the whole reason it outperforms legacy IDE on real
hardware is NCQ -- several commands in flight at once, retired out of
order. Linux drives it that way (`drivers/ata/ahci.c` over `libahci`,
each port a full libata port); Windows' `storahci.sys` is a StorPort
miniport with the same shape.

`kernel/drivers/ahci.c` deliberately takes only half of that. It
ENUMERATES every implemented port and reports what is on each -- the
link state, the speed, the signature -- because that is the hardware's
actual structure and a driver that pretended a port it never looked at
does not exist would be lying about what it found. But exactly one
drive becomes the block device, driven through slot 0 with one command
outstanding.

The reason is that a queue has nothing to queue. `blk_active()` is
singular, `struct block_device` has no asynchronous form, and every
caller above it -- TFS3, FAT32, the partition scan -- issues one
transfer and waits for it. NCQ would add a slot allocator, a per-slot
completion path and `PxSACT` handling in order to keep exactly one
command in flight, which is what the code does now with none of that.
The honest version of this is to SAY so, which is why `/bin/ahci`
prints "offered by the HBA, not used" rather than leaving a reader to
infer from `CAP.SNCQ` that queuing is happening. The prerequisite is an
asynchronous block interface, not more AHCI code, and it is on
`docs/roadmap.md` as such.

The same reasoning covers `CAP.S64A`: the HBA offers 64-bit DMA
addresses and this driver's buffers come from `pmm_alloc_contiguous()`
below 4 GiB, so every `*U` register is written zero. Reported, not used.

## A discard capability comes from the device's ANSWER, not from its feature bit -- and only the host can prove one worked

Two things about TRIM/discard were learned the expensive way here, and
both generalise past storage.

**A negotiated feature is not an available one.** virtio-blk may
negotiate `VIRTIO_BLK_F_DISCARD` and then advertise a
`max_discard_sectors` of ZERO, which means it cannot accept a discard at
all -- and QEMU does exactly that unless the drive was given
`discard=unmap`. A driver reading the feature bit alone would declare
`BLK_CAP_TRIM`, and then fail every discard the filesystem issued, at
which point `block.h`'s both-directions honesty check has been satisfied
by a capability that lies. So `block_virtio.c` declares the bit from the
MAXIMUM, and `block_ahci.c` from IDENTIFY word 169 -- both read at
REGISTRATION, where the answer is already settled. `block_ata.c` still
advertises blind and documents why (its identify data is not necessarily
settled that early), which is the exception that makes the rule
readable.

**A TRIM that discards nothing is invisible from inside the guest.**
This is not hypothetical: `ata.c`'s first DSM implementation sent the
range list over PIO, the drive accepted the command, returned no error,
and nothing whatsoever was discarded. Every guest-side check passes. The
only oracle that cannot be fooled is OUTSIDE -- the ALLOCATED size of
the sparse host image (`st_blocks`), which the guest cannot influence.
So `tools/ahci_test.py` and `tools/virtio_boot_test.py` both write
40 MiB, delete it, and require the image back at its baseline, and the
KTESTs deliberately cover only the REFUSALS (zero count, past the end,
a range that wraps) because a real discard on the mounted root would
destroy the filesystem the test is running from.

**Assert BOTH halves of a reclaim.** The first version of that check
only looked for the blocks coming back, and passed on an image that had
never grown -- `shutil.copyfile` had produced a fully-allocated 9.6 GB
copy, so there was no sparseness left to lose. "It went back down" is
satisfied by "it never went up". The growth assertion is what turned a
silent pass into a visible failure.

## AHCI is not cached, and that is the difference from ata.c rather than an oversight

`ata_cache.c` is a write-back sector cache under
`ata_read_sectors()`/`ata_write_sectors()`, and its own header explains
that it lives in the driver rather than the block layer precisely so no
bypass path can exist. Adding a second driver was the moment to ask
whether the cache should move up a layer and serve both.

It should not, because the cache is not there to be fast in general --
it is there because ATA's PIO fallback makes a 512-byte metadata read
genuinely expensive, and because ATA's DMA path costs one command and
one completion interrupt per transfer however small. AHCI's does too,
but a cache that saves an interrupt while ADDING the risk a write-back
cache carries is not obviously a win, and virtio-blk -- measured ~10x
ATA's throughput -- was left uncached for exactly this reason and has
not wanted one.

What the absence buys is that `BLK_CAP_FLUSH` on `block_ahci.c` means a
real FLUSH CACHE EXT reaching the drive, with nothing above it that
could be holding dirty lines. A capability bit that means what it says
is worth more here than a cache nobody has measured a need for; the
moment somebody measures one, generalising `atac_*` to hold a
`block_device` is the shape to reach for, and it is a roadmap item
rather than a comment.

## `noahci` is a precedence word, not a kill switch

The block layer's disk precedence is virtio-blk, then AHCI, then legacy
ATA (`kernel/fs/mount.c`, one line, in one place). Two boot words step
it down a rung each: `novirtio` was already there and `noahci` joins it,
for the reason `nopat` and `notsc` exist -- a fallback nothing can reach
is a guess, and legacy IDE is still the only disk on some machines.

What it deliberately does NOT do is disable the driver. `ahci_init()`
still runs, finds the HBA, brings up the port, IDENTIFYs the drive and
reports all of it through `/bin/ahci`; `blk_ahci_init()` is the only
thing that consults the word. The alternative -- skipping the whole
driver -- would make the flag untestable in the useful direction,
because "the machine came up on ATA" and "the AHCI driver crashed
before it could claim anything" produce the same boot. With the split,
`tools/ahci_test.py` can assert both halves at once: the driver ran, and
the block layer did not take it.

The cost is worth naming. On a machine whose ONLY disk is the SATA one,
`noahci` yields ramfs rather than a fallback, because there is nothing
on the IDE controller to fall back to. That is the correct outcome and
it looks like a failure, so the boot log says which rung it landed on.

## The input core: one vocabulary, a source registry, and evdev as canonical

**What was wrong.** There was no seam at all. `i8042_poll()` fed bytes
straight into `keyboard_feed_byte()` and `mouse_feed_byte()`, and each
of those owned both the decoding AND the state every consumer reads. A
second kind of input device had nowhere to plug in: "add a driver" would
have meant "edit the PS/2 driver". virtio-input was the second kind and
USB HID will be the third, so the seam went in first.

**The canonical event is evdev-shaped**, which is the whole design
decision. A key is a KEYCODE, not an AT scancode; a pointer reports
relative motion, an absolute position, a button mask or wheel notches.
Linux made the same call for the same reason -- `atkbd` translates AT
set 1 INTO keycodes, USB HID translates usages into them, and
virtio-input carries them natively because its events ARE
`struct input_event`. Windows does the equivalent with HID as the
internal model. Either way the PC encoding is a SOURCE format and never
the internal one: a USB keyboard has no scancodes, and making it invent
some would be inventing a legacy it never had.

**What PS/2 does, stated rather than hidden.** The 8042 driver does NOT
round-trip through the vocabulary -- it already speaks set 1, and the
layout tables (`/etc/kbs/*`) are keyed on set 1, so it feeds its own
state machine directly. New sources come in through
`input_report_key()`, which translates keycode -> set 1 once. That table
is the seam's only piece of legacy and it is small, because evdev
keycodes 1..83 ARE the set-1 make codes (not a coincidence: that is
where the numbering came from). It disappears when the layout files are
re-keyed to evdev codes, which is a roadmap item rather than a
prerequisite.

**The registry is the same pattern as everything else here** --
`display_driver`, `block_device`, `clocksource`. A source declares a
name, its capabilities, its IRQ if it has one, and a `poll()` if it does
not. Keeping both servicing shapes in one table is what stops "how does
this device get serviced?" being a different question per driver, and
`lsdev` prints the answer.

**`INPUT_KEY_*` is prefixed and `KEY_*` is not, deliberately.**
`keyboard.h`'s `KEY_*` are the codes this kernel's key RING carries
(`KEY_HOME` is 0x9B, chosen to sit outside ASCII); `INPUT_KEY_*` are
what the wire carries before translation (`INPUT_KEY_HOME` is 102,
Linux's number). Four collided outright when the header was first
written -- a silent collision between two key vocabularies would have
surfaced as "Home does something odd on one keyboard".

**Bounds are asked for, not assumed.** `mouse_get_bounds()` exists
because a KTEST computed absolute positions against `gfx_width()` and
was wrong by 16x: the pointer's bounds are whatever
`mouse_set_bounds()` was last given, which on a boot where nothing has
set them is a small default, not the screen.

## virtio-input, and the transport's first interrupts

**An event queue is the shape no virtio device here had.** Block,
entropy and GPU are request-response: the driver asks, the device
answers, the driver waits on `virtqueue_poll()`. An input queue is the
opposite -- the driver hands over a pile of EMPTY buffers and the device
fills them when the user does something, which may be never. Waiting
would be waiting for a keypress. Hence `virtqueue_take()`: non-blocking,
"whatever is there", loop until empty. Every buffer taken goes straight
back, because a queue that runs out of buffers does not fail loudly, it
just stops reporting input.

**Config space is a WINDOW, not a struct.** Write `select`/`subsel`,
then read what they name -- which is how a device says whether it is a
keyboard, a mouse or a tablet. It does not say so directly; it says
which event types it emits, and the useful fact is that the answer's
SIZE is nonzero, so no bitmap is decoded here at all. This is also why
`virtio_cfg_write8()` had to exist: every previous device's config space
was read-only.

**INTERRUPTS, and why INTx rather than MSI-X.** MSI is delivered as a
memory write to a Local APIC, and this kernel has none -- the 8259 PIC
is all there is. So virtio-input uses legacy INTx, and that forced two
changes worth recording:

- `irq.c` now holds a CHAIN of handlers per line. The old one-per-line
  rule was justified on the measured grounds that every device sat on
  its own line; three virtio-input functions are routed by the chipset
  onto whichever PIRQ their slot maps to, and QEMU duly puts the mouse
  and the tablet on IRQ 10 together. Sharing is what INTx IS.
- Interrupts are OPT-IN per device. `virtio_pci_find()` sets
  `PCI_CMD_INTX_DISABLE` for everything it claims, and only a driver
  that installs a handler clears it. The failure mode of getting this
  wrong is not a missed interrupt: an INTx line is LEVEL-triggered and
  stays asserted until the device is serviced, so a device left free to
  assert it with nobody reading its ISR wedges the machine. That is not
  a theoretical hazard -- the positive control that made the handler
  return immediately did not merely stop events, it hung the guest.

**The ISR read is the "was it me?"**, and it is destructive, so it
happens exactly once per device per interrupt. On a shared line every
handler runs and each asks its own device; stopping at the first
claimant would leave a second device asserting forever.

**An interrupt-driven source has no `poll()`**, which is also what makes
the drain lock-free: the idle path never touches a queue the handler
owns. A device the chipset routed nowhere keeps its poll instead, so no
device ends up with neither.

## The `bochs` driver DECLINES the display unless it can improve the mode

`video=<W>x<H>` (docs/boot-flags.md) existed for months and did nothing
on the adapter every default boot and every headless test actually uses.
The reason is a layering fact that is easy to miss: `vesafb` is not a
driver in the modesetting sense at all -- it reports the framebuffer GRUB
negotiated from `boot.asm`'s multiboot2 request and has no way to ask for
another. `vmsvga` and `virtio-gpu` can program a CRTC, but they own
specific hardware (`VGA=vmware`, `VGA=virtio`); the ordinary `-vga std`
adapter had no modesetting driver, so its mode was fixed at build time by
an assembled constant.

`kernel/drivers/display/bochs.c` is that missing driver: the Bochs DISPI
register window (ports 0x01CE/0x01CF), which QEMU's stdvga and
`bochs-display` implement and Linux's `bochs-drm` drives. It is what
makes `make iso KCMDLINE="video=1920x1080"` mean something on a default
boot.

**The design call worth recording is that it DECLINES rather than
claims.** `display_probe()` activates the first driver whose `probe()`
returns 1, and the obvious implementation claims the hardware whenever it
finds it. That would be wrong here: `display_mode_candidate()`'s ladder
walks largest-first and ends at whatever GRUB already gave us, so on an
ordinary boot with no `video=` flag the driver would find itself
"setting" the mode already on screen -- which means blanking and
re-programming a live console that has already drawn into it, on every
single boot, to achieve nothing. So the probe breaks out of the ladder
the moment a candidate is no larger than GRUB's own geometry and returns
0, and `vesafb` -- a strictly simpler driver for the same pixels -- claims
instead. Verified as a pair, which is what makes it evidence: the default
ISO logs `display: using "vesafb" -- 1280x720`, and an ISO built with
`KCMDLINE="video=1920x1080"` logs `bochs: set 1920x1080 (GRUB had
1280x720)`.

**The bound that actually bites is video MEMORY, not the resolution.**
The adapter's advertised `max_w`/`max_h` (read through the `GETCAPS`
mode, which temporarily repurposes the XRES/YRES/BPP registers) are
generous and say nothing about whether the mode fits: QEMU's stdvga
defaults to 16 MiB of `vgamem_mb`, which holds 1920x1080x4 at 8.3 MiB and
does not hold 3840x2160x4 at 33.2 MiB. Programming a mode that does not
fit gives a live display scanning past the end of its own memory -- a
torn or black screen with nothing logged anywhere. So the driver reads
`DISPI_INDEX_VIDEO_MEMORY_64K` and refuses the candidate, which puts it
on the next ladder rung instead of on a broken screen. `-device
VGA,vgamem_mb=64` is the host-side answer for a 4K guest.

Two smaller things, both the same shape as `vmsvga`'s: every check
happens BEFORE the first register write of the mode-set sequence, because
once XRES goes out the old mode is gone whether or not the rest succeeds
-- that is what makes walking a ladder safe. And `DISPLAY_CAP_MODESET` is
deliberately NOT advertised: the cap means the display layer may change a
mode at RUNTIME, and nothing above here survives that yet (gfx.c's back
buffer and every ring-3 compositor mapping are sized at their own init),
so claiming it would be exactly the dishonest capability
`display_probe()` exists to refuse.

## gfx.c's back buffer is allocated, and the constant it used to be was a hidden ceiling on the display

It was `static uint32_t back_buffer[1920 * 1080]` -- 8 MiB of `.bss`,
sized for the largest mode anyone expected to want. Two things were wrong
with that, and only the second one matters much.

It costs 8 MiB of kernel image on a machine that comes up at 640x480 and
will never use it. That is the small one.

The real problem is that it was a ceiling on the DISPLAY MODE that
nothing at the display layer could see. `gfx_set_double_buffered()`
returns 0 for a surface larger than the array, so a driver that
programmed a bigger mode came up with a perfectly live screen that the
rasteriser silently declined to double-buffer, falling back to drawing
straight into the framebuffer -- correct, and slow enough to watch. The
symptom is "the console got mysteriously slow", which points nowhere near
an array bound. `display.c` compensated by stating `DISPLAY_MAX_W/H` as a
mirror of the array's dimensions with a comment explaining that the two
had to be kept in step by hand: exactly the "a number some other file has
to keep true" shape CLAUDE.md says to delete rather than document.

It is `pmm_alloc_contiguous()` at `gfx_init()` now, sized to the mode
that is actually on screen. Contiguous because it is indexed as one flat
array and the kernel's identity map is what makes a physical address
usable as a pointer; at `gfx_init()` because `kernel_main()` calls it
right after `pmm_init()`, before anything else has taken a frame, which
is the only point where a 33 MiB request is one a pristine allocator can
still satisfy. A failure is not fatal and never was -- the pointer stays
NULL and every path takes the existing direct-to-framebuffer fallback.

`DISPLAY_MAX_W/H` is 3840x2160 now, and it is a policy rather than a
mirror of an array: past that the numbers stop being sensible for a
machine this OS boots on.

---

## A TrueType rasterizer in the kernel, with the baked font kept as the fallback

`tools/genttf.py` has always rendered JetBrains Mono offline into
`kernel/drivers/font_ttf.c` -- eight sizes, 101 glyphs, decided at build
time. Loading a real `.ttf` from `/usr/share/fonts` and rasterizing it
on demand replaces where glyphs COME FROM, and the interesting decisions
are all about where that code runs and what it is allowed to touch.

**Ring 0, deliberately, and this is the one place toy-os copies Windows
rather than Linux.** Linux has no font rasterizer in the kernel at all:
`fbcon` uses baked bitmap fonts, FreeType and fontconfig are userspace,
and under X11 and Wayland the CLIENT rasterizes -- a compositor never
opens a font file. Windows put font parsing in `win32k.sys` (GDI), spent
a decade of remote-code-execution CVEs on it, and moved it out to a
sandboxed user-mode font driver (`fontdrvhost.exe`) in Windows 10. That
history is not an argument for doing it in ring 0; two local facts are.
The console needs glyphs before any process exists -- including on the
panic path -- so a userspace font service cannot serve the whole
machine. And `WIN_REQ_FONT` exists precisely so every GUI client shares
ONE font with the desktop; a client-side rasterizer would put a copy of
the font in every process and let each one drift.

So the mitigation had to go in the parser instead. `kernel/lib/ttf.c`
touches the file buffer only through `rd_u8`/`rd_u16`/`rd_i16`/`rd_u32`,
each of which bounds-checks against the file length and answers 0 rather
than reading past it; there is no pointer arithmetic into the file
anywhere in it. Per-glyph complexity is capped (points, contours, edges)
and a glyph past a cap renders BLANK rather than partially -- a half-
decoded outline fills wrongly, and a wrong glyph is worse than a missing
one. Composite recursion is depth-limited, because a composite cycle is
a hostile font rather than a deep one. Moving the parse to ring 3 is a
roadmap item, and what it needs is a shared-memory mechanism that does
not exist yet.

**Fixed point, because there is no floating point here.** `-mno-sse` is
deliberate (see `fpu.h`), so outlines are scaled and flattened in Q16.16
through `fixed.h`. A font unit is an `int16_t` and `unitsPerEm` is
1000-2048, so `fu << 16` fits in an `int32_t` with nothing to spare and
everything past that goes through `fx_mul`'s `int64_t`. Anti-aliasing is
four sub-scanlines per pixel row with exact horizontal span coverage:
one is visibly aliased at 8px, eight is indistinguishable from four.
Filling is NONZERO winding, not even-odd -- with even-odd an `o` comes
out solid, which is recognisable enough in a screenshot to pass an
eyeball check, and is therefore asserted numerically in a KTEST.

**No allocation, so one implementation serves both rings.** Everything
transient lives in a caller-supplied `struct ttf_scratch` (~69 KB, far
past either ring's stack budget), which is what lets `ttf.c` be compiled
a second time into `libuapp.a` under the repo's shared-source rule
(`geom.c`, `klineedit.c`, `heap_core.c`). `/tests/ttf_test` is the real
second caller, and it exists for the reason `klineedit_test` does: the
KTESTs would pass whether or not a byte of this were reachable from ring
3.

**An atlas comes from the frame allocator and is never freed.** Two
separate reasons, both about the mapping. `WIN_REQ_FONT` maps the active
atlas read-only into every client and a mapping is page-granular, so an
atlas allocated with `kmalloc` would hand every client a read-only
window onto whatever else shared its pages -- `pmm_alloc_contiguous()`
gives it pages of its own. And clients hold that mapping for as long as
they live, with no way for the kernel to ask them to let go, so freeing
an atlas on a font-size change would point the compositor at reallocated
memory: the same class of bug the poison-page fix (978ebf7) exists to
prevent. Atlases are therefore cached and retained, bounded by entry
count and total bytes, and a build past either bound is REFUSED (the
current size keeps drawing) rather than evicting something that is being
read.

**The cell is measured with `genttf.py`'s formula, not the font's raw
metrics.** Layout everywhere in this OS is derived from `gfx_char_w()`
and `gfx_char_h()`, so a runtime face at 14px producing a visibly
different cell from the baked 14px would reflow every window on the
machine the moment a face was selected. The raw `hhea` ascent and
descent fit every glyph with no clipping and produce a taller, looser
cell than a terminal font has; the same 0.89/0.6 tightening the baked
sizes were generated with keeps the two within a pixel, at the cost of
the same slight accent and descender clipping every fixed-cell terminal
font accepts.

**The atlas is the BAKED glyph set, and that is a bound rather than an
oversight.** A runtime atlas rasterizes exactly the 101 slots
`font_ttf.h` describes, in the same order, so a loaded face's remaining
thousands of glyphs are parsed and unreachable. Two reasons it is drawn
here. The slot order is ABI -- `WIN_REQ_FONT` shares it, and a client
built against a different count would index into the wrong glyph rather
than fail -- so widening it is a protocol change, not a constant. And
the question underneath it is "what is a character?", which this OS has
already answered as Latin-1 (see the Nordic/Latin-1 entry above); a
loaded font does not change that answer, so the ceiling belongs to UTF-8
migration and not here. `docs/roadmap-details.md` states it as the
limitation it is.

**A face is named by its filename, and the baked font is not a face.**
`dejavu-sans-mono.ttf` is `dejavu-sans-mono`, the same convention cursor
themes use for directories -- so listing what is available is a
directory listing rather than something that must open every file and
parse its internal `name` table. `builtin` names the absence of a face,
so "go back to the kernel's own glyphs" is a value the setting can hold
and round-trip rather than a missing key. A failed select leaves the
previous face active: dropping to the baked font because of a typo would
lose the user's font for the wrong reason.

## A virtio device is published to its handler BEFORE its interrupt is enabled

`virtio_enable_intx()` used to do two things in one call: clear
`PCI_CMD_INTX_DISABLE` and report which line the chipset had routed the
function to. That reads as a convenience and is a trap, because the
caller cannot know the line until the device is already able to
interrupt. Everything the handler depends on therefore got set up
*after* the device went live.

`virtio_input.c` had exactly that shape. Per device it enabled INTx,
unmasked the PIC, printed two log lines, and only then incremented
`g_count` -- the bound its own interrupt handler looped to. A device
outside that count is a device whose ISR nobody reads, and the ISR read
is what deasserts a level-triggered INTx line. So an interrupt arriving
in the window did not cost an event; it hung the machine, because the
PIC re-delivered it forever and the boot thread never ran again. The two
`klog_printf()` calls sitting inside the window made it milliseconds
wide on a serial console.

It presented as a boot that stopped mid-log-line while enumerating the
third virtio-input device, on some boots and not others. All three
components of "sometimes" turned out to be measurable:

- **The third device**, because the first two have already unmasked the
  line it shares, so its window opens at `virtio_intx_enable()` rather
  than at the PIC unmask.
- **The tablet specifically**, because it has events waiting the moment
  the pointer is over the window. A keyboard sitting idle never asserts.
- **KVM**, because at TCG speed the guest is slow relative to nothing in
  particular -- the race simply did not reproduce there. Injecting
  pointer motion through the whole boot: 3 hangs in 3 boots under KVM,
  0 in 3 under TCG, same build.

The fix is an ordering one, and the API was split to make the ordering
expressible: `virtio_intx_line()` asks (no side effects),
`virtio_intx_enable()` commits. Between them the driver registers its
handler, fills in the fields the handler tests, and registers its input
source. The PIC unmask comes last.

The handler was also changed to scan the whole device array on
`present && irq` rather than the first `g_count` entries. That is not
belt-and-braces: `g_count` exists to count *finished* enumeration, and
an interrupt handler has no business depending on when a loop body ends.
Both fields it now tests are set before the device can assert.

**The generalisation, which is the part worth carrying:** any "enable"
that hands back information the caller needs in order to be ready is an
API that forces its callers into a race. Split it. The same shape exists
wherever a subsystem is registered and armed in one step -- a timer that
returns its own handle, a queue that starts consuming as it is created.

`tools/virtio_input_test.py` carries the regression check and only runs
it where it can fail (with `/dev/kvm`); it says so when it skips, since
a check that cannot fail is worse than no check.

## USB is xHCI only, with no HCD abstraction, and its MMIO is left write-back

Three decisions taken together when `kernel/drivers/usb/` was written,
because each is only defensible in the light of the others.

**xHCI and nothing else.** UHCI, OHCI and EHCI are perhaps a quarter of
the code between them, and `docs/roadmap-details.md` had recommended
starting with UHCI as "dramatically simpler if the goal is first proving
out the general model". That was rejected. The reason this driver exists
at all is a machine with no PS/2 port, and Intel dropped the EHCI
companion controllers at Skylake -- so such a machine presents xHCI and
nothing else, and a UHCI driver would be an entire controller for a bus
no hardware made since roughly 2010 has. QEMU's `piix3-usb-uhci` would
be its only home. The cost is real: xHCI has no simple mode, so nothing
works until the command ring, the event ring, the cycle bit and the
doorbells all work at once. It is a small scheduler, not a
poke-a-register device. The mitigation was to split the ring arithmetic
into `xhci_ring.c`, which contains no controller and is therefore
KTESTable on a machine with no USB at all.

**No `struct usb_hcd` ops table.** The usual argument against a
premature abstraction is that the second implementer might not arrive;
here there is no plausible second implementer at all, having just
refused the only candidates. The in-repo precedent is exact rather than
analogous: `virtio_pci.c` + `virtqueue.c` are a shared transport with
four device drivers on top, and there is no `struct virtio_transport`
vtable -- drivers call `virtqueue_submit()` by name. So `usb_hid.c`
calls `xhci_control()` by name. What *was* built is `xhci.h`, a plain
header splitting xHCI mechanics from USB semantics, because that seam
had two real callers on the day it was written (a keyboard and a mouse)
and mass storage would be a third. If a second controller ever appears,
converting six functions to an ops table is a mechanical afternoon;
building the table first would have been the cost with none of the
benefit.

**MMIO stays write-back, and no `paging_set_uncached()` was built.**
`boot.asm` identity-maps the low 4 GiB write-back with 2 MiB pages, so
the xHCI BAR gets a WB PTE, and the reflex is to add an uncached-mapping
API before touching a register window. It was deliberately not added,
for reasons that are an argument rather than a test result -- which is
itself the point.

On real hardware the MTRRs win: firmware marks the PCI hole UC, and UC
from either MTRR or PAT beats a WB PTE, so the effective type is already
right. Under TCG, PAT is ignored entirely. Under KVM, emulated MMIO
traps to the hypervisor on access regardless of the guest's memory type.
So the question is invisible to `make verify`, to `gui_regress.py` and
to CI, and a `paging_set_uncached()` written today would be untested
code guarding an untested hazard -- the shape this repo refuses
elsewhere (`ata nodma` exists precisely so its fallback is reachable).
The 2 MiB granularity of the identity map is a second obstacle: a
per-page memory type would need the mapping split first.

The residual risk, stated rather than dismissed: a machine whose
firmware leaves the controller's BAR inside a WB MTRR range, or places
it above TOLUD outside any variable range. There the CPU could cache and
reorder register accesses, and the driver would misbehave in ways that
look like a device fault. **If xHCI works under QEMU and misbehaves on
real hardware, start here.** That sentence is the deliverable; the code
would have been worse.

What *is* required, and is not about caching at all, is `volatile` on
every register access -- `xhci.c` copies `virtio_pci.c`'s accessors.
Polling `USBSTS.CNR` through a non-volatile read is one hoist away from
an infinite loop, on TCG and KVM alike.

## USB took interrupts from its first commit, and the cost is one function

The xHCI driver was built interrupt-driven rather than polled-first,
which was not the safer order.

The safe order was available: `input.h` documents a `poll()` hook for a
source with no interrupt, `scheduler_idle()` already owns the kernel's
idle work, and `virtio_input.c` ships a polled path as a live fallback.
Polling would have let the whole device stack land with no interrupt
code at all, so that when INTx did arrive, any new hang was
unambiguously that change. Against it: latency under a polled drain is
bounded by the idle cadence, so it degrades under load -- the worst kind
of bug to report -- and every other PCI input device here is
interrupt-driven. Interrupts were chosen, and the polled path is
retained for a controller whose pin is unusable, in exactly
`virtio_input.c`'s shape (`.poll = irq ? 0 : thunk`).

**The cost is concentrated in one function.** xHCI's "was it me?" is
strictly harder than virtio's single destructive byte read:
`USBSTS.EINT` and `IMAN.IP` are both RW1C, so acknowledging means
writing a 1 back to exactly the bit being cleared. A read-modify-write
that writes the register back either fails to deassert the shared
level-triggered line -- a storm, the failure that hung this guest 3
boots in 3 under KVM during virtio-input and 0 in 3 under TCG -- or
clears a status bit belonging to another device on the line.
`ack_interrupt()` in `xhci.c` is the only place either bit is written.

Because that failure is invisible under TCG, `tools/usb_test.py` grew a
`--kvm` flag and the change was not believed until it passed there. A
green TCG suite says nothing about this class.

## The xHCI BIOS handoff, and why "QEMU does not implement it" was the wrong reason to skip it

`xhci.c` logged the USB Legacy Support capability and deliberately did
not act on it. The comment said why: the capability is the BIOS handoff,
QEMU does not implement one, so there was nothing to hand off from.

That reasoning is sound about QEMU and wrong about the world. The
capability exists precisely on the machines that are not QEMU, and on
those the BIOS **owns the controller** until an OS asks for it. While it
does, register accesses trap into the firmware's SMM handler. A driver
that starts resetting and configuring is poking a device somebody else
is still servicing.

**What that looks like from outside is not a driver bug.** One Intel
laptop froze during bring-up with no pattern: mid-log-line, at a
different point each boot, and the point MOVED when unrelated trace
logging was added. Nothing in the driver's own control flow explains a
hang that relocates when you print more — a CPU that has entered SMM and
not returned does.

So the handoff is built, in Linux's shape
(`drivers/usb/host/pci-quirks.c`, `quirk_usb_handoff_xhci`): set the OS
Owned semaphore, wait for the BIOS Owned semaphore to clear, force it
clear on timeout rather than refusing to proceed, and then disable every
SMI source in USBLEGCTLSTS and write-1-clear its status bits. The SMI
disable is the half that actually protects the driver, and it runs
whether or not ownership was granted.

**THE ORDERING IS THE DECISION, not the code.** Linux does this in a PCI
quirk that runs BEFORE the driver binds. Here the capability walk used
to run AFTER `reset_controller()`, so the reset -- a write to the
operational registers -- happened while the firmware still owned the
device, before anything had even discovered that it did. The walk moved
ahead of the reset. A handoff performed after the first write is not a
handoff.

**It is untestable here and that is stated rather than hidden.** QEMU
advertises no legacy-support capability, so `legsup_off` stays 0 and the
whole path is dead in every test in this repo. `usb_test.py` proves only
that bring-up still works on a controller with nothing to hand off.
**CONFIRMED on the machine 2026-08-28**: the same laptop boots past
bring-up and an external wireless mouse works.

The general lesson is the one this repo keeps meeting: **"the emulator
does not have it" is a reason the code is untested, never a reason the
code is unnecessary.** The same sentence had already been written about
the scratchpad-buffer branch a few days earlier, and that branch turned
out to be fine while this one was fatal.

## The xHCI poll runs beside its IRQ, deliberately unlike virtio-input

The convention up to this point was virtio_input.c's either/or: an
interrupt-driven source leaves `poll` NULL, so the two can never both
run. The xHCI controller source now registers its poll UNCONDITIONALLY,
IRQ or not, and that is a decision worth defending because it breaks a
stated rule.

The reason is what the two drivers trust. virtio's interrupt is
negotiated with a hypervisor that also implements the device, so "the
IRQ works" is part of the same contract as "the device works". xHCI's
INTx line comes from PCI config byte 0x3C, which is whatever the BIOS
wrote there -- and on a modern PCH routing through the IOAPIC (which
this kernel does not program; the PIC is all there is), that value can
be plausible and DEAD. A driver that trusts it registers everything
with `poll = NULL` and the mouse is silently, permanently deaf, with
`usb: running, interrupt-driven` in the log as the only witness. There
is no way to *detect* a dead line that is cheaper than simply also
polling: the poll drains an already-empty ring in a handful of reads
when the IRQ is live, and it runs only from `scheduler_idle()`.

What makes the double drain safe is the single-consumer guards --
`xhci_service()`'s existing one, plus one added to
`usb_hid_service_all()` when the poll became unconditional. The
alternative shape, a watchdog that notices `irqs_seen == 0` and
retro-registers poll thunks, was considered and dropped: it is more
state, it has a window before it fires, and its only payoff is a purer
lsdev line.

Deferred work rides the same poll for a different reason: hot-plug
enumeration and halt recovery are synchronous command submissions, and
the event drain they would have to run inside is single-consumer -- a
command issued from there waits on a completion the drain itself would
have to pop. Linux defers to a hub worker thread for the same shape of
reason; this kernel's idle poll is its worker thread.

Two consequences measured rather than assumed: a boot-time connect
change arrives BEFORE the port scan has recorded anything, so the
deferred attach must skip a port that already has an enumerated device
(the first version re-reset the boot keyboard's port and dropped its
first keystrokes -- caught by usb_test's wrap phase and predates.py);
and a failed enumeration must disable its slot, or a retry burns a
fresh slot per attempt (slots 2 and 3 leaked in one boot log before
the failure paths called `xhci_disable_slot()`).


## Sound: one exclusive stream over a shared ring, and the kernel never mixes

The audio subsystem's three calls, made before the first driver landed
(the AC'97 is the first `struct sound_device`; HDA and USB audio are
later implementers of the same registry, the display/block/clocksource
shape again).

**Exclusive, no kernel mixer.** A second `SYS_SND_OPEN` is refused
with -EBUSY, the compositor-role pattern. Every modern OS keeps mixing
OUT of the kernel -- ALSA's dmix is a library, PulseAudio/PipeWire and
Windows' audio engine are userspace -- because mixing drags resampling
and format policy in with it. If two audible apps ever matter here,
the answer is a userspace sound daemon owning the one stream.

**A mapped ring, not a write() call.** The app asked for the
lower-latency shape and it costs less than it looks: a control page
plus 64 KiB of samples mapped at a fixed address (`SND_MAP_VADDR`,
the window-buffer idiom), the kernel publishing the hardware position
per completion interrupt -- ALSA's mmap mode in miniature. Steady
state costs ZERO syscalls, where an OSS-style write() pays one per
chunk plus a copy. The buffer is physically contiguous so the AC'97's
descriptor list points straight into it: the app's samples DMA out
with no copy anywhere in the path.

**A consumed chunk is zeroed before hw_pos moves past it.** The
engine loops the ring forever once started (LVI kept one behind CIV),
which is what makes underrun handling free -- zero IS silence in
signed PCM -- and what makes the ring safe to abandon: a stalled or
killed app degrades to silence instead of looping its last 341ms.
That one rule replaced an underrun-detection path, an app write
cursor the kernel would have to trust, and a stop-on-starvation
state machine. The `sound` KTEST asserts the zeroing walk; disabling
the memset was the positive control, and it reddened the KTEST before
the host-side recording noticed -- the unit guard is the sharp one.

**The volume is a setting, not a syscall.** `kernel/lib/sound_config.c`
registers `volume` (0-100) beside the pointer knobs; the registry
gives it the System Settings row, `config set volume 40`, /etc
persistence and range enforcement, and the driver contributes one
`set_volume` op mapping percent onto its attenuators.


## The Local APIC, and one device off the shared pin

The 8259 PIC gave this kernel sixteen level-triggered lines that
devices share, and every consequence of that has been paid for at least
once: a "was it mine?" read in every handler on a line, a boot hang
when virtio-input asserted a line nothing was reading yet, and -- the
question that started this -- an xHCI and an AC'97 both landing on
IRQ 11 because that is what the BIOS wrote in config byte 0x3C. The
Local APIC is enabled now and the xHCI delivers through an MSI-X vector
instead.

**Why this could not have been done earlier, stated because `virtio.h`
has carried the sentence for a year: an MSI is a memory write to
0xFEE00000, and until something enables the Local APIC there is nothing
at that address to receive it.** The PCI half -- find the capability,
write an address and a vector, set the enable bit -- is thirty lines.
The LAPIC is the dependency, and it is why "just use MSI" was never a
small change.

**Virtual wire mode is the whole risk.** Enabling the LAPIC takes the
CPU's INTR pin away from the 8259, which then reaches the CPU only
through the LAPIC's LINT0 input. Programmed for ExtINT, every legacy
line keeps working exactly as before; left masked, the timer, the
keyboard and the disk stop together on the first tick, and the symptom
is a machine that hangs during boot with no clue as to why. That single
register write is the difference, and it is why `lapic_init()` does it
before it returns rather than leaving it to a later stage.

**MSI-X before MSI, which is not the order the names suggest.** Linux's
`pci_alloc_irq_vectors()` tries MSI-X, then MSI, then INTx, and the
reason showed up immediately here: QEMU's `qemu-xhci` advertises MSI-X
and no MSI at all, so an implementation that only knew MSI worked when
forced with `msi=on` and quietly stayed on its pin on the default
machine. MSI-X is also the only form with a future here -- its table
gives one address and data pair PER ENTRY, which is what a multi-queue
device (NVMe, virtio-net with several queues) needs and what MSI, with
one shared pair and a power-of-two vector block, cannot express.

**What was deliberately NOT done.** No I/O APIC: the legacy lines still
go through the 8259, which is fine because virtual wire keeps them
working and nothing else wants a vector yet. No per-queue vectors: the
xHCI has sixteen interrupters and this uses one, because one device
with one queue is what proves the mechanism. And nothing here starts
another processor -- `docs/smp-design.md` still owns that, and the
LAPIC arriving does not change its staging.

**The escape hatch is a boot flag, and it is the same argument `nopat`
and `novirtio` make.** `nomsi` keeps the LAPIC off entirely, which is
also the path a CPU with no APIC takes -- so the fallback is not a
theory, it is a configuration that has to keep working and can be
booted on demand. Measured: with `nomsi` the controller is back on
IRQ 11 and USB input still works.

**And the test had to be about DELIVERY.** Configuration proves
nothing, because every control transfer in the xHCI driver polls the
event ring: a controller whose interrupts vanish still enumerates its
devices, registers them with the input core and logs "running". The
only thing an interrupt is load-bearing for is an asynchronous HID
report, so `tools/msi_test.py` moves the mouse and requires both the
interrupt count and the decoded-report count to rise. With INTx
disabled by the MSI-X programming, an interrupt that arrives can only
have come from the vector.

## USB audio: one more sound device, and the three things that were not obvious

The AC'97 was written as "the first `struct sound_device`", and a USB
card was the test of whether that claim was true. Mostly it was -- the
class driver registers and the ABI did not move -- but three things had
to change, and the reasons are worth keeping.

**Isochronous transfers, which the xHCI driver did not have.** USB
Audio Class streaming is isochronous only: there is no bulk fallback,
so this was not optional. What is different from the interrupt
endpoints already there is not the TRB type but the ERROR MODEL. An
isochronous endpoint does not halt, is never retried (CErr is 0), and
its ring running dry produces a Ring Underrun event carrying no TRB
pointer at all -- normal life for a stream between tracks. The first
version treated those as transfer errors, which would reset a stream
that had merely stuttered; Linux's xhci-hcd makes the same distinction,
and the reason it matters here is that a reset turns a glitch into
permanent silence. Scheduling uses Start-Isoch-ASAP on every TD rather
than computed Frame IDs: the alternative is tracking the controller's
frame counter and predicting one interval ahead, which buys nothing for
a stream that is simply continuous. **This is the one part that is
QEMU-tested and not hardware-tested**, and a real controller that
insists on Frame IDs is where it would show.

**The format is refused rather than negotiated.** `abi/sound_abi.h`
fixes 48 kHz stereo s16, and QEMU's device offers exactly that -- but
the driver checks rather than assuming, and leaves a device offering
anything else unbound. Resampling already exists one layer up
(`userland/lib/usnd.h`), and putting a second copy in the kernel to
accommodate a 44.1 kHz headset would be the wrong half of the system
growing the capability. A machine with such a headset gets a logged
refusal, which is a worse outcome than resampling and a much better one
than a resampler nobody tests.

**The samples are copied, and the class comment already allowed for
it.** `kernel/sound.h` said a card that cannot scatter-gather over one
buffer "copies in ITS half; the ABI does not move" -- this is that
card. 192 bytes per frame divides neither the 64 KiB ring nor its 2 KiB
chunks, and an xHCI TRB may not cross a 64 KiB boundary, so a zero-copy
packet needs chained split TRBs at two kinds of edge. Linux's
snd-usb-audio copies into per-URB buffers for the same reason. The cost
is 192 KB/s of memcpy, which is nothing; the benefit is that the
isochronous ring never has to reason about the sound core's geometry.

**And one bug that was not about audio at all.** The volume control
looked impossible for an afternoon: every class control transfer timed
out after two million polls. Two causes, stacked. A feature-unit
request is addressed to the AUDIOCONTROL interface, and the driver was
sending the STREAMING one -- so the device failed the transfer. But the
reason that presented as a HANG rather than an error is that
`xhci_control()` armed its waiter on the Status Stage TRB alone, and a
control transfer that fails is reported against the stage that failed
(the Data Stage), whose event named a TRB nobody was waiting for. The
Status Stage then never executed, ep0 stayed halted, and every
subsequent request to that device timed out too. The waiter matches any
TRB in the slot's ep0 ring now, and the recovery set is every code that
halts an endpoint rather than STALL alone. That bug was reachable by
any device that refuses any request -- it had simply never been reached,
because QEMU's other devices refuse nothing.

## Sound device selection: the first one discovered, until somebody chooses

With two cards in a machine the question "which one plays?" has to have
an answer, and the honest options were: the first discovered, the most
recently plugged, or a stored choice.

**The rule is: the first device discovered, and a choice outranks it.**
`audio_device` (a registered setting, so it gets `/etc` persistence, a
System Settings row and `config set` for free) holds `auto` or a device
name. A pick is STICKY -- it survives that card being unplugged and
takes effect again when it comes back -- which is why an absent name
falls through to auto rather than being rewritten to whatever is left.

The rejected alternative was "the most recently plugged device wins
while the stream is idle", which is what PipeWire and Windows both do:
plug in a headset, hear it in the headset. It was rejected because it
makes the default answer depend on enumeration order at boot -- the
same machine picks differently depending on whether USB or PCI is
scanned first -- and because a plug silently moving audio away from
where it was playing is the behaviour people turn off first in the
systems that have it. Discovery order is at least stable and
inspectable (`lsdev` marks the active device), and choosing takes one
click in the tray flyout.

**A registration never steals a stream that is open.** The app holding
it asked for a device that still works, and switching under it would
drop audio mid-word for a plug it never saw.

**Losing the active device is a state an AC'97 cannot reach**, so the
ABI grew one field for it: `device_gone` in the control page, published
beside `running: 0`. Without it an app cannot tell "somebody stopped
me" from "the hardware is gone", which is the difference between
resuming and reopening -- and the consumed-chunk zeroing means both
sound identical (silence). The stream then falls back to another
registered device for the NEXT open rather than the machine going mute.

## usnd: audio files are decoded and mixed in ring 3, behind a sink

The kernel's contract stops at "one exclusive stream of 48 kHz stereo
s16". Everything a person would call playing a file -- the formats, the
rate conversion, several sounds at once -- is `userland/lib/usnd.h`, in
the process that wants the sound. The argument is the one `uimg.h`
already makes about images: ALSA's dmix is a library, PulseAudio,
PipeWire and Windows' audio engine are userspace, and mixing drags
resampling and format policy with it. A malformed WAV can at worst take
down the one process that opened it.

**Three seams, and each is a thing that does not exist yet.**

*The codec table.* A `struct usnd_codec` row -- probe, open, read,
seek, close -- the registry shape `display_driver`, `block_device` and
`uimg_codec` already use. WAV is the one row. **A codec never
resamples**: it reports its file's native rate and channel count and
yields s16 frames in it, and the library converts once, in one place.
That is where PulseAudio, PipeWire and CoreAudio all put resampling,
and it is what makes MP3 a file and a row rather than a second decoder
with its own opinion about the device.

*The sink.* Where mixed samples GO (`usnd_sink.h`), and the reason no
part of the public header mentions the ring, `hw_pos` or `SND_*`. An app
says "play this file", never "write here", so a sound daemon can become
a second row -- tried first, the device as the fallback -- with no app
changing. `libasound` made exactly this move when PulseAudio appeared,
and PipeWire kept both.

**What a daemon needs is RENDEZVOUS, not shared memory.** 192 KB/s in
2 KiB chunks is ~94 messages a second against a 64 KiB `SYS_WRITE_MAX`;
two copies and a scheduling hop are free against a 341 ms ring, and a
shared-memory ring per client is what a 2 ms latency target needs, which
this is not. What is genuinely missing is that pipes here are INHERITED
rather than connected: a client the daemon did not spawn cannot reach
it. TWP already solves that for the compositor, by role; generalising
its registry to named endpoints is the smaller move, and AF_UNIX is the
portable spelling of the same thing.

*The voices.* An eight-voice mixer per process, because a game's effects
overlap and one voice would cut them. With a daemon it becomes the app's
submix -- a PulseAudio sink input -- and the daemon sums across apps;
the code does not change either way.

**A worker thread, not an `on_tick`.** Every real system pumps audio
from a dedicated thread (WASAPI's, CoreAudio's IOProc) because a missed
refill is audible. Slicing across a GUI client's tick would work until a
repaint or a directory read ran long, and 341 ms is not much slack. The
thread touches only the voice table and the sink, which is what
`ui/uapp.h`'s worker rule already requires.

**An idle mixer PINS its cursor rather than writing silence.** Three
options and only one is right. Not writing at all leaves the write
cursor where the hardware left it, so the next sound starts a whole ring
late. Writing silence keeps the cursor moving but makes "how much is
queued" meaningless -- and the position readout is computed from exactly
that, so it would run BACKWARDS while paused. So the worker stops
writing, lets the tail drain, and then flushes each pass to hold the
cursor one chunk behind the hardware.

**A refusal is not a corruption.** `-EINVAL` means the bytes are broken,
`-ENOTSUP` means a good file this build will not play (a float WAV --
there is no floating point in this project, in either ring), and
`usnd_last_error()` carries the sentence. `uimg.h`'s split, for the same
reason: an app that says "this build cannot play that" is telling the
truth, and one that says "corrupt file" is not.


## Doom's audio: the rest of the port, not a rewrite

**doomgeneric IS Chocolate Doom with the platform layer and sound
removed**, which is the fact the whole design turns on. 24 files in
`userland/ports/doom/` carry Simon Howard's copyright; `sound_module_t`,
`music_module_t`, the GENMIDI handling and `mus2mid.h` (shipped without
its `.c`) are all Chocolate Doom's. So "add sound to Doom" was never a
question of writing an audio engine -- it was a question of putting back
files that were removed, and writing the small part that was genuinely
SDL's.

**Switching to a different port was considered and refused.** It would
have cost `dg_toyos.c`, the app, the `TOYKEY_*` static asserts that keep
`api/keyboard.h` and `doomkeys.h` apart, the build integration and
`doom_test.py` -- all shaped around the DG_* API -- to solve a problem
smaller than the rewrite, and it reverses this file's own entry on
vendoring doomgeneric.

**The version was chosen by MEASUREMENT.** `chocolate-doom-2.1.0` is the
tag at which `memio.c` is byte-identical to doomgeneric's copy and
`i_sound.h` differs only by the declarations doomgeneric appended --
so `sound_module_t` and `music_module_t` match exactly and
`i_oplmusic.c` compiles against the headers already here. Later tags
drift (2.3.0's `i_sound.h` differs by 46 lines). Guessing a version
would have produced a shim layer instead of a drop-in.

**Effects map onto usnd voices; there is no second mixer.** `s_sound.c`
already does distance attenuation, channel allocation and stealing, and
hands `I_StartSound` a channel, a volume and a separation -- so the
backend is a mapping and nothing else, which is why `dg_sound.c`
contains no DSP. That is also what Chocolate Doom does (SDL_mixer
channels plus `Mix_SetPanning`); the roadmap's old wording, "Doom doing
its own effect mixing in userspace", described PrBoom+'s shape and was
written before `usnd` existed.

**THREE SHIMS, ALL ON OUR SIDE, BECAUSE PATCHING VENDORED CODE IS THE
THING THE DIRECTORY EXISTS TO PREVENT.** Enabling `FEATURE_SOUND` -- on
the compiler command line, not by editing `doomfeatures.h` -- makes the
code reach for what the SDL port had:

  - `SDL_mixer.h`, included by `i_sound.c` and never used. An empty
    header in `userland/backends/doom/compat/`.
  - `SDL.h`, for big-endian byte swaps and a mutex/condition pair.
    Mapped onto `__builtin_bswap` and pthreads, so `OPL_Delay()` really
    blocks rather than being stubbed.
  - `opl_sdl_driver`, named unconditionally by `opl.c`'s driver list.
    `opl_toyos.c` exports that symbol; the driver's own `name` says
    `toyos`, so only the C symbol is borrowed.

`-D__DJGPP__` would have suppressed the first two and was rejected: it
changes real behaviour in five other files (`i_swap.h`'s byte swapping,
`i_endoom.c`, `i_system.c`'s exit path).

**THE OPL RENDER THREAD IS NOT AN OPTIMISATION -- INIT DEADLOCKS WITHOUT
IT.** `opl.c`'s `InitDriver` calls `OPL_Detect()`, which calls
`OPL_Delay()`, which schedules a callback and blocks on a condition
variable until it fires -- and callbacks only fire from the render path,
which advances the clock. SDL got away with pumping from Doom's own loop
because its audio thread was already running by the time init ran; here
nothing was, so the game hung inside `I_InitMusic` with its window stuck
on the pre-WAD title. The driver starts its own thread instead, which
also decouples the tempo from Doom's frame rate -- worth having on an
emulator whose speed varies. **The OPL clock is SAMPLES PRODUCED, never
wall time**, for the same reason.

**The music carries a fixed 3x gain, because the two paths are not
level with each other.** Measured on the attract demo with each path
isolated: music alone peaked at 1735, effects alone at about 19000 --
roughly 21 dB apart, which leaves the music inaudible under gunfire.
Doom's own music and sfx sliders set the level WITHIN each path; the
level BETWEEN them is a property of how OPL synthesis compares to
full-scale sampled effects, and is therefore ours. Nothing can clip:
`usnd`'s mixer saturates rather than wrapping.

**Effects are precached at startup, unlike Chocolate Doom**, which
decodes on first use unless libsamplerate is on. About 11 MB for the
full set, flat and predictable, against a hitch the first time each
sound is heard. The trap that cost a debugging round: `S_Init`
precaches over the WHOLE of `S_sfx[]`, including a dummy entry with an
empty name, and `W_GetNumForName` calls `I_Error` on a miss -- so the
precache loop must use `W_CheckNumForName` or the game dies before its
window opens.

## The first NIC is the e1000, and the second one landed with it

The roadmap said "NIC driver (rtl8139 first)", which was written before
anyone measured what the emulator actually offers. QEMU's default `pc`
machine attaches an **e1000 (8086:100E) with user-mode networking
whenever no `-net`/`-netdev` option is given** -- confirmed with `info
pci` on a bare `qemu-system-x86_64` -- so every guest this project has
ever booted, including every test in the suite, has had an unclaimed NIC
sitting on the bus. That makes the e1000 the one card a driver can
assume: no flag, no new run-target axis, and every existing tool's guest
gains a network the day the driver exists.

**Both drivers landed together rather than one then the other**, which
is the opposite of how the disk drivers arrived. The reason is the
lesson `multidisk_test.py` was written for: an interface with one
implementer gets shaped around that implementer, silently, and nothing
notices until the second one arrives. `struct net_device` is the fifth
registry here (`display_driver`, `block_device`, `clocksource`,
`sound_device`), and the previous four all took their shape from one
driver and had to be re-cut. Writing virtio-net in the same change cost
about two hundred lines, because `virtio_pci.c` and `virtqueue.c`
already existed -- far less than re-cutting the class later.

The split follows the storage precedent exactly: the transport-side
driver is `kernel/drivers/virtio/virtio_net.c` (queues, the 12-byte
header, the ISR) and the class adapter is
`kernel/drivers/net/net_virtio.c`, the same pair as `virtio_blk.c` +
`block_virtio.c`. The adapter is the only file that knows both sides are
the same card, which is what lets `virtio_net.c` include no networking
header at all.

**No offloads are negotiated, deliberately.** Checksum offload and GSO
are what virtio-net is good at, and each one changes what the stack
above is handed -- a frame whose checksum is not filled in yet, or one
larger than the MTU. Neither is a saving worth having before there is a
TCP to be fast at.

**The header is always 12 bytes**, because `virtio_begin()` requires
`VIRTIO_F_VERSION_1` and `num_buffers` is unconditional there. In legacy
virtio it is 10 unless `VIRTIO_NET_F_MRG_RXBUF` was negotiated, and
getting it wrong shows up as every received frame being two bytes
shifted -- which parses as garbage rather than failing.
