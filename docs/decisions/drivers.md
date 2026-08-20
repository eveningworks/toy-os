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
mechanism, since ECAM needs ACPI/MCFG table parsing just to find its
base address and CF8/CFC is universally supported including by QEMU's
emulated chipset. BARs are decoded (I/O-vs-memory, base address) but
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

**The cursor plane is real and has no consumer yet.** virtio-gpu's
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
