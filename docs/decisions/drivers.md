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

## Nordic keyboard/character support: Latin-1, not UTF-8

**A character is one byte of Latin-1 (ISO-8859-1): ASCII, plus the
Latin-1 Supplement 0xA0-0xFF.** Every byte-buffer boundary in this
kernel (`scrollback_cell`, file content, `SYS_WRITE`/`SYS_READ`'s
buffer+length) assumes one byte is one character is one glyph cell, and
Latin-1 keeps that true; UTF-8 would break it everywhere a multi-byte
letter crossed one. It began (build 501) as six Nordic letters baked
after ASCII; since 2026-10-04 the font carries the whole 0xA0-0xFF block
and the keyboard layouts type all of it. The ceiling is the point to
move past, and `docs/roadmap.md`'s UTF-8 migration is where that
happens -- not by widening this further.

**The font's slots are arithmetic, not a table.** ASCII 32-126 is slots
0-94 and Latin-1 0xA0-0xFF is 95-190 (`font_ttf_slot()` in the generated
`api/font_ttf.h`). Both rings and every atlas (the baked tables, fontd's
runtime faces, a client's private font) index the same way, so there is
no codepoint list for two rings to keep in step -- the six-letter design
had one (`font_ttf_extra_codepoints`), and ring 3 never learned to
address it, so a Nordic letter drew as a space in every GUI app. NBSP
draws blank; the soft hyphen draws as a hyphen, as on xterm and the
Linux console, since nothing here hyphenates.

**The KEY_* specials moved from 0x91-0xB9 to 0xF780-0xF79F and
0xF880-0xF888** -- off the Latin-1 block they overlapped (German
Shift+AltGr+1 types 0xA1, which was Shift+End). Above 0xFF because 41
codes do not fit in the C1 range; in Unicode's Private Use Area because
that is where macOS puts its function keys (NSUpArrowFunctionKey,
0xF700) and because the UTF-8 migration will carry codepoints in the
same stream -- a special at 0x100 would one day be a Latin Extended-A
letter. Two runs rather than one so that EVERY LOW BYTE IS A C1 CONTROL:
a key truncated to a byte by mistake becomes something no font draws
and IS_PRINTABLE_KEY refuses, never a letter and never Ctrl-C (a first
cut kept the old code as the low byte, so a truncated Shift+End typed
an inverted exclamation mark). The console terminal's queue already
had 16 bits of room; the byte-wide links (`tty_input()`, the release
table, `struct keycombo`) were widened, `tty_read()` hands a byte reader
nothing for a special, and a terminal turns a special into ANSI before
fd 0 sees it, so the byte-stream contract of `read()` held.

**`char` is signed in this build** (no `-funsigned-char`), so a
codepoint >= 0x80 is negative as a `char`. Build 501 found six gates
written `key >= 32 && key < 127` and a seventh worded differently
(`c < 128` in the kernel shell) that a grep for the first six missed;
it was found only by typing. `IS_PRINTABLE_KEY()` (`api/keyboard.h`) is
the one gate now, and a key is an `int` everywhere -- the layout's
tables are `uint16_t` for the same reason.

## Keyboard layouts are data files (`/usr/share/kbs/<name>`) generated from Linux's own XKB data, not a compiled-in enum

The original `se` layout only remapped the three Å/Ä/Ö keys -- everything
else stayed identical to `us`, including keys whose physical legend is
genuinely different on a real Nordic keyboard (the key beside right
Shift types `-`/`_` on a physical FI/SE keyboard, not `/`/`?`). Rather
than hand-fix scancodes one bug report at a time, layouts moved to
`/usr/share/kbs/<name>` data files, generated by `tools/gen_kbs.py` from
`xkbcli compile-keymap` (Linux's own, already-correct XKB layout
compiler -- no X server needed) instead of anyone re-deriving a
scancode chart by hand. Translation logic itself moved out of
`keyboard.c` into a new `kernel/lib/keyboard_layout.c`, since owning
per-region character tables was never really the driver's job (raw
scancode/shift-state handling is). See the commit that added it for the full implementation, and a real bug the
generator's first cut had (omitting Escape/Backspace/Tab/Enter from
its key list, which silently broke Enter the moment the shell started
loading layouts from generated files instead of the old compiled-in
ones -- found live, not by review).

**Dead keys compose in the kernel, behind every keyboard driver** (since
2026-10-04). That is the Linux console's shape -- an accent table the
keyboard driver consults (`KDSKBDIACR`) -- rather than X11's, where a
client-side input method composes. toy-os has no input method and the
console must compose too, so the state lives in
`kernel/lib/keyboard_layout.c` and every driver reaches it through
`keyboard.c`'s `key_event()`. The table is per layout and inside the
layout's own file (`dead:acute:e=0xE9`), generated from libX11's Compose
data, because `/usr/share/kbs`'s listing IS the Settings choice list and a
separate compose file there would appear as a layout. The behaviour is
Windows': dead + Space is the accent alone (XKB gives an apostrophe for
the acute), and dead + a key it does not compose with types the accent
then the key. Level 4 (Shift+AltGr) is read too; the driver always knew
both modifiers, only the tables lacked the column.

## GDB debugging: QEMU's stub under QEMU, the kernel's own stub on bare metal

**Two stubs, because they answer different questions.** `make debug`
boots frozen at CPU reset (`-s -S`) under QEMU's own GDB stub: the
emulator stops the CPU, so it needs no guest code at all, reaches the
first instruction after GRUB, and still works on a machine too wedged
to run anything. That was the whole answer while every machine this OS
ran on was emulated, and this entry used to say an in-kernel stub was
"simply unnecessary".

**Bare metal made it necessary.** Two laptops now run toy-os, most of
`docs/bugs.md` reproduces only on them, and no emulator stands behind a
real CPU. The only thing that can stop one is the kernel itself -- which
is why Linux has KGDB and Windows KD, both in the kernel image and both
off until the boot line asks (`kgdboc=`, `bcdedit /debug on`). So
`kernel/debug/` is a GDB Remote Serial Protocol stub, armed by
`kdebug=ttySN` and nothing else. It speaks the standard protocol rather
than a toy-os one because GDB, its DWARF reader and its disassembler are
the expensive part, and a stock `gdb` is on every developer machine.

**The shape is Linux's**: the protocol (`kernel/debug/gdbstub.c`) is
generic and the CPU half (`kernel/arch/x86_64/kdebug_x86.c`) is not, as
`kernel/debug/gdbstub.c` and `arch/x86/kernel/kgdb.c` split. Three
choices that differ from the obvious ones:

- **Software breakpoints are patched only while the kernel runs** --
  lifted on every stop, written back on every resume. Memory reads while
  stopped then show the real bytes, and the stub's own serial poll can
  never land on one mid-conversation. KGDB does the same.
- **Memory is reached through a page-table walk first**, so a bad
  address from GDB is an `E14` reply rather than a fault inside the
  debugger with interrupts off. Linux uses exception fixups
  (`copy_from_kernel_nofault`); this kernel has no fixup table, and a
  walk needs none.
- **KASLR is reported with `qOffsets`**, so an unmodified
  `build/kernel.bin` relocates its own symbols in GDB. Linux leaves this
  to the user (`nokaslr`, or `add-symbol-file` by hand).

**The network transport OWNS its NIC and authenticates without
encrypting.** `kdebug=net` claims a whole card before any driver binds
it, because no NIC driver here has a lock, and a stop landing mid-ring-
update in a SHARED card's driver corrupts the ring -- the hazard any
netpoll-shaped transport (Linux's out-of-tree kgdboe) carries, and the
one KDNET avoids by owning the hardware. Its datagrams carry HMAC-SHA256 and a
rising sequence number but no cipher: KDNET encrypts, but a cipher costs
a kernel implementation plus a third-party host package, while the MAC
needs only Python's stdlib. `docs/kdebug-design.md` has the rest,
including KDNIC for the one-NIC ASUS. `-g` stays in
`CFLAGS` at `-O2` for both stubs -- the same binary as every other
build, at the cost of some locals showing "optimized out".

## ATA's waits are bounded by wall-clock in one context and a spin count in the other

Every wait in `kernel/drivers/ata.c` that can block looks like it's
written twice, and the duplication is deliberate. A wall-clock budget
needs `coarse_ticks()` to advance, and it doesn't inside a syscall: `int
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

## An ATA command is done when the bus master says so, not when IRQ14 fires

The obvious completion test is "the interrupt handler ran", and
`ata.c` used exactly that: IRQ14 sets `g_dma_irq_fired`, and the waiter
treated the flag as completion. The flag is only a WAKE-UP, and it can
be stale -- an IRQ14 that lands while the next command is still
running. Where it comes from is NOT established; the polled syscall
path, which leaves its own IRQ14 pending for later, is one candidate. Measured on QEMU 11.1: about one such wake per
`usertest_run.py` pass, always with the bus master still `ACTIVE` and its
interrupt bit clear. The waiter then called the command finished and
wrote `BM_CMD=0`, stopping the engine mid-transfer.

On most QEMUs that is invisible, because the stop DRAINS the request
before the write returns. On QEMU 7.0 through 11.0.0 a drain that lands
during an IDE TRIM deadlocks the emulator itself (upstream `7e5cdb34`,
fixed by `095c08a7ba` in 10.2.3/11.0.1) -- which is how this surfaced:
Ubuntu 24.04's QEMU 8.2.2, the CI runner's, hung under the ring-3 suite
4 runs in 17, and never with TRIM off.

So `dma_irq_seen()` asks the hardware: the bus-master status register's
interrupt bit is completion, and a flag without it is cleared and waited
past. That is Linux's rule -- `ata_bmdma_port_intr()` treats an
interrupt without `ATA_DMA_INTR` as not its own. Linux's other half,
`ata_sff_lost_interrupt()`, which completes a command whose interrupt
went missing, was NOT needed: the waiter already reads the bit on every
wake and at the deadline.

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
looks. (For the ROOT this is now only the tie-break: storage.md's "The
root disk is the boot disk, not the fastest controller's" has why.)

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
layout tables (`/usr/share/kbs/*`) are keyed on set 1, so it feeds its own
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
(`KEY_HOME` is 0xF786, above every character); `INPUT_KEY_*` are
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

*(Superseded in part on 2026-09-02: it now CLAIMS by adopting GRUB's
mode without a register write when nothing better is on the ladder,
so that a runtime mode change is possible on `-vga std`. See
`docs/decisions/gui.md`, "A mode change is a kernel setting". The
reasoning below about not re-programming a live console still holds
and is why the adoption writes nothing.)*

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

## BAR sizes are probed at enumeration, not by the driver that needs one

The obvious shape for `pci_bar_mem_size()` was a function that probes
on demand: disable decode, write all-ones, read, restore. It is the
shape the roadmap item named. It was not built that way because the
probe turns the device's decode OFF for its duration, and a driver-time
call runs while that device may already be in use -- the framebuffer
console is a PCI BAR, and a second driver asking about the same device
would repeat the window. Linux settled this in `__pci_read_base()`: the
bus enumerator sizes every BAR once, before any driver binds, and a
driver only ever reads `pci_resource_len()`. Windows' pci.sys does the
same in its enumerator. toy-os follows: `pci_init()` runs before
interrupts and before `vga_init()`, so nothing is decoding anything,
and the result lives in `struct pci_device.bar_size[]`.

Two details are Linux's too and are worth keeping for the reason rather
than the precedent. The probe writes only the BARs the header type
actually has (six for type 0, two for type 1), because a bridge's 0x18
is its bus-number register and all-ones there renumbers the bus behind
it. And a host bridge keeps decoding throughout (`mmio_always_on`),
because some chipsets hang when the host bridge stops responding.

The cost accepted: `struct pci_device` is the `SYS_PCI_INFO` snapshot,
so the field is visible to `lspci`, which now prints it. That is a
feature rather than a leak -- a size is a fact about the device, not a
mechanism -- and the header's own rule (`pci_internal.h`) still keeps
the config-space WRITE that produced it out of ring 3.

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
vtable -- drivers call `virtqueue_submit()` by name. So `input_usbhid.c`
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
to run AFTER `xhci_reset_controller()`, so the reset -- a write to the
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

**The format is refused rather than negotiated.** (The ring has been s32 since 2026-10-06 -- "The sound ring is s32, and a 16-bit card narrows in its driver" below.) `abi/sound_abi.h`
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

## Intel HDA: a controller and the generic widget walk, with no quirk table

The laptop's own sound card is HD Audio, and the question was how much
of Linux's HDA stack to copy. Linux is `snd-hda-intel` (the controller),
a codec bus, a generic parser that walks the codec's widget graph, and
then a vendor module per codec family carrying the quirks -- the Realtek
one alone is ~12k lines of "this laptop needs GPIO 2 for its amplifier".
Windows is the same shape: `hdaudbus.sys`, a generic `hdaudio.sys`, and
the vendor's driver on top.

**Copy the shape, not the size.** `hda.c` is the controller plus the
generic walk: choose the output pin from what its default configuration
says it is wired to, route it back to a DAC through the connection
lists, unmute every amplifier on the way, set EAPD wherever a pin says it
has one. No quirk table, because a quirk table with one entry is a
guess and one with none is honest. The bar for adding an entry is a
machine that is silent under the walk, diagnosed from `hdadump` -- which
is why the dump exists as a boot word rather than a debug build.

**Pre-emptive EAPD was the one deliberate deviation** from "do nothing
speculative": the external amplifier enable is what most laptop speakers
sit behind, Linux sets it for nearly every Realtek and Conexant codec,
and it costs nothing on a pin that has no amplifier. The maintainer chose
it over "report and stop" before the first hardware run, and the test
laptop's Conexant CX20751 played on the first try.

**Every HDA controller is claimed, only an analog output is registered.**
A laptop has two controllers (the PCH's, and the GPU's for HDMI/DP), and
they are the same PCI class. Registering the display one would put a
device in `lsdev` and the volume flyout that plays nothing; skipping it
by device id would be a list that rots. So both are brought up, the walk
decides, and the one with no analog route is logged and put back into
reset. HDMI audio is a roadmap item because its codec needs the GPU's
power well (Linux's `snd_hdac_i915` binding), not because the class
driver is missing anything.

**NOSNOOP is cleared rather than the ring mapped uncached.** The laptop
played a kernel-written sine as "continuous clapping" and music as
crackle, with QEMU clean throughout: firmware leaves the PCH's DEVC
NOSNOOP bit set, so the DMA engine read RAM without snooping the caches
and played whatever had been evicted. Linux has two answers -- clear the
bit (`azx_init_pci()`, the default on Intel) or allocate the buffers
uncached (`snoop=0`, for controllers that cannot snoop). Clearing wins
here because the ring is ALSO mapped into the app that writes it, and
an uncached mapping would have to be made twice and kept in step. Found
by splitting the path in half: `kernel.hda_tone` plays a ring the kernel
filled once, with no app, no zeroing and no interrupts, so a bad sound
there is DMA, stream or codec alone -- two plausible refill-path theories
(LPIB reading ahead; BCIS firing before the last bytes are fetched) were
built, flashed and withdrawn before it. The lesson is the repo's own:
a mechanism that explains the symptoms is not the one that caused them.

**The volume taper is shared with the USB driver, not invented here.**
The first version mapped the percentage linearly onto amplifier steps,
which on a 74-step, 1 dB amplifier put 40% at -44 dB -- and the
maintainer heard nothing. `sound_usb.c` had already settled the
convention (0..100 linear in dB across 40 dB, 0 is mute, "the range a
physical knob covers"); one setting has to mean one thing on every card,
so `hda.c` follows it. What was NOT done is a perceptual curve (ALSA's
`-M` and PulseAudio use a cubic mapping, 60·log10(v)), because two
drivers agreeing beats one driver being more correct -- that move is the
roadmap's "one taper for every card".

## PCI drivers are matched by the bus, not found by the driver

Nine drivers each carried a loop over `pci_device_at()` looking for
their own class or vendor:device, took the first hit and stopped -- so a
second controller did not exist, "which driver has that device" was
answerable only from the boot log, and every one of the nine was a
line in `kernel_main()`. Linux's `pci_driver` and NT's PnP both turn
this around: the driver declares an id table and a `probe()`, the bus
enumerates and calls. `PCI_DRIVER(name, matches, probe)` is that shape
(`kernel/include/kernel/pci_driver.h`): a `.pci_drivers` section, and
one `pci_bind()` initcall at `INIT_BUS` that walks devices in
enumeration order against the tables.

**Why first-match-in-link-order rather than a priority.** A device that
two tables claim is a design mistake, not a preference to rank, and the
`pci_bind` KTEST fails on any present device two drivers match. The one
place a class is genuinely shared -- the USB host controller class,
where prog-if tells xHCI from EHCI -- is handled INSIDE `probe()`, which
refuses and logs the kind, so the log still says why USB did not come
up on an EHCI-only machine.

**Why the order moved from init order to slot order, and what that
cost.** Two NICs, two sound cards and two disks used to be ranked by
the order of calls in `kernel_main()`; they are ranked by bus address
now. Nothing that mattered depended on the old order: disk precedence is
explicit in `mount.c`, `audio_device` outranks discovery, and network
interface names never depended on order in the first place (they are
made from each card's MAC). The USB-before-sound ordering the laptop relied on
(a USB DAC plugged at boot became the active device) survives for the
same accidental reason -- the xHCI sits below the HDA on the PCH -- and
is not a guarantee; the setting is.

**What stays out.** `ata` (brought up from `vfs.c` before backends are
probed), the display drivers (the display registry chooses one at
`vga_init()`, before the walk), and USB class drivers (bound at
enumeration by the xHCI). Each has a registry whose ORDER is a decision
in its own right, and pulling them under PCI matching would move that
decision somewhere less visible.

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

## The MP3 decoder is ours, and only three of its tables came from anywhere else

The codec table's second row is MPEG-1 Layer III, `userland/lib/usnd_mp3.c`.
Writing one rather than vendoring minimp3 (CC0) is the same call
`uimg_jpeg.c` made against libjpeg, for the same reason: a decoder is
the part of this system most worth having written, it parses hostile
input in ring 3 where a mistake kills one process, and an implementation
nobody here understands is one nobody here can fix.

**What could not be written is DATA, and it is separated from the logic
for exactly that reason** (`usnd_mp3_tables.h`). Three tables have no
generating formula -- the Huffman codes, the 512-tap synthesis window,
and the scalefactor band edges. They are the standard's, recovered from
public-domain sources (minimp3, CC0; pdmp3, Unlicense) and cross-checked
against each other: all fifteen Huffman tables agree entry for entry
between two implementations that share no code. Everything a decoder
needs that IS derivable is derived at runtime instead of frozen here --
the alias coefficients from `ci[]`, the intensity ratios from
`tan(i*pi/12)`, the IMDCT windows from `sin()`. A table is what you
write when there is no formula, not a place to cache one.

**The Huffman tables carry their own proof.** Every one must be a
complete prefix code: Kraft's sum exactly 1 over exactly `dim*dim`
pairs. `usnd_mp3_selftest()` checks it in integer arithmetic, needs no
audio, and runs in both the guest test and the host harness. That check
earned its place before it ever shipped -- an early hand-written table 7
failed it, which is how the transcription approach was abandoned in
favour of recovering the data.

*Floating point.* The filterbank is float, which is available in ring 3
and nowhere else here (`kernel/arch/x86_64/fpu.c` enables SSE with an
eager per-process FXSAVE; the kernel is `-mno-sse`). `usnd_wav.c` used
to say this project had no floating point in either ring; that was
already wrong when it was written and is corrected.

*What it refuses, and why refusing is the honest answer.* Layer I/II,
MPEG-2/2.5, free-format and intensity stereo are `-ENOTSUP` -- a good
file this build will not play, which is the distinction `usnd.h`'s
three-way errno split exists for and the same call the JPEG decoder
makes on a progressive image. Intensity stereo is refused rather than
implemented because nothing available encodes it, and shipping an
untested path that silently produces wrong audio is worse than a
refusal that names itself.

*Seeking is approximate and says so.* A Layer III frame's main data does
not start in that frame -- `main_data_begin` points backwards up to 511
bytes into a bit reservoir -- so a frame cannot be decoded in isolation
and a seek cannot land exactly without an index. It jumps by the average
frame size and lets the reservoir refill, which is what every player does
with a file that has no seek table.

## MIDI is a codec that renders through a SoundFont, and the bank is built here

**What real systems do.** Windows plays MIDI through a software
wavetable synthesiser with a small bank that always ships (the
Microsoft GS Wavetable Synth, a ~3 MB `gm.dls` of Roland samples);
macOS does the same with Apple's DLS synth. Linux ships no default:
FluidSynth or TiMidity++ plus a SoundFont somebody installs (FluidR3_GM,
141 MB, MIT; GeneralUser GS, ~31 MB). DOS-era players used the OPL FM
chip, which DOSBox and libADLMIDI emulate. toy-os follows Windows'
shape -- a synth plus a bank that is always present -- with Linux's
escape hatch: any SoundFont dropped into `/usr/share/soundfonts`
outranks the built-in one.

**A codec row, not a MIDI port.** A `.mid` renders to PCM behind
`struct usnd_codec` (`userland/lib/usnd_mid.c`), which is how
GStreamer's fluiddec and TiMidity-as-a-decoder treat it, and is why
Audio Player, `aplay` and the File Manager play MIDI with no change but
a `Handles=` line. The alternative -- a live MIDI output port in
`soundd`, the shape of Windows' `midiOut`, the ALSA sequencer and
CoreMIDI -- is what a USB MIDI keyboard would need, and nothing needs
it yet; the synth (`usnd_synth.h`) takes channel messages and knows
nothing about files, so a port would be a second driver of the same
calls rather than a rewrite. It is on the roadmap.

**SoundFont 2, not OPL or a procedural synth.** SF2 is the format real
banks exist in, so the same engine plays a 1.4 MB generated bank and a
31 MB recorded one. OPL was rejected because every emulator worth using
is GPL or LGPL (the Doom port's `dbopl.c` is GPL-2, and `usnd` is MIT)
and it can only ever sound like 1992; a procedural synth, because it
can only ever sound like a game console. **The built-in bank is
GENERATED** (`tools/gen_sf2.py`): additive synthesis for an attack,
then one exact cycle as the loop -- seamless because each sample's rate
is `L x f0` for an integer cycle length -- one root per octave,
band-limited for its zone, and twenty timbres shared by the 128 GM
programs through preset-level generator offsets, the way a 1990s ROM
synth fitted GM into a few megabytes. It is ours, so the repository's
licence covers it; `make iso EXTRAS=1` fetches GeneralUser GS
(`tools/fetch_soundfont.py`) for anyone who wants recorded instruments.

**Faithful to FluidSynth where the spec is silent or wrong in
practice**, because the banks people have are voiced against it: the
attenuation generator counts 0.4 dB a unit (`ALT_ATTENUATION_SCALE`,
the EMU8000's reading -- read as the spec's centibel, quiet
instruments come out 2.5x too quiet); the default modulator list is
FluidSynth's, including a velocity-to-filter modulator that only acts
below velocity 64; and the modulation envelope's attack is convex, as
the spec says and a linear one got wrong. **Modulators are applied, not
skipped** as TinySoundFont does: GeneralUser GS sets each instrument's
velocity curve and velocity-driven filter sweep with them, and every
difference `tools/midi_hostcheck.py` found against FluidSynth before
they were implemented was one of those.

**The bank is read into memory, not mapped.** A page fault in the mixer
thread is a dropout, which is why FluidSynth loads and `mlock()`s its
banks by default. It is cached per process, and `usnd_load_info()`
never loads it -- a song's length comes from the score -- so browsing
MIDI files in the Player does not wait on 31 MB.

**The per-sample path is integer** (a 32.32 position, Q24 gains, a Q28
biquad on 24-bit samples). Under TCG every SSE instruction is a
softfloat helper call; the float version rendered GeneralUser at 4.3x
real time on the light first bars of the demo song and dropped out on
the dense ones, and the integer one runs at 10.6x under TCG and 60x
(the built-in bank) on the ASUS.

## usnd: audio files are decoded and mixed in ring 3, behind a sink

The kernel's contract stops at "one exclusive stream of 48 kHz stereo
s16". (The ring has been s32 since 2026-10-06 -- "The sound ring is s32, and a 16-bit card narrows in its driver" below.) Everything a person would call playing a file -- the formats, the
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
2 KiB chunks is ~94 messages a second, well inside `SYS_WRITE_MAX`;
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

## A device's configuration descriptor is KEPT, and reported RAW

`lsusb -D` prints the bytes a USB device sent about itself, out of a
copy the kernel keeps (one DMA frame per device, freed on detach) and
through `QUERY_USBDESC` -- a LIST whose records are 232-byte slices,
because `QUERY_RECORD_MAX` is 256 and `api/query.h` already says a class
needing more is a list of smaller records.

**Why raw rather than a decoded record**, which was the obvious
alternative and is what every other provider here does. The devices that
need explaining are exactly the ones no driver bound, and a driver
declines by walking CLASS-SPECIFIC descriptors that nothing else keeps
-- so a decoded record would have to know, in advance, about every class
this build does not support. That is the wrong way round. The raw bytes
also paste straight into a KTEST fixture, which is the only way a device
nobody here owns is ever tested against: `sound_usb.c`'s fixture is
QEMU's configuration captured this way, and a hand-written one would
only ever agree with the parser it was written beside.

The decode is in **ring 3**, in `/bin/lsusb`, for the reason the kernel
keeps no formatting: it is presentation, it is not needed to bind
anything, and a class the kernel does not implement can gain a decode
without touching the kernel.

**Why keep it rather than re-read it on demand.** A control transfer
from a query provider is possible -- the volume path already does one
from a syscall -- but it makes reading a fact depend on a device still
being present and still answering, and `lsusb`'s own page promises it
cannot hang on a misbehaving device. A frame per device is 4 KiB of a
budget nothing else is competing for.

**What this is not.** It is not `/dev/bus/usb`: there is no way to ask
the device something new, no string descriptors beyond the two
enumeration already read, and no second configuration. Those all want a
device-file interface, which wants a mount table (`docs/query-design.md`
has the same argument for why this is not `/proc`).

## UAC2: the rate is a request, and the clock is asked rather than chosen

`sound_usb.c` binds USB Audio Class 2.0 as well as 1.0. The two share
subtype numbers and agree on almost nothing else, and the differences
are not cosmetic: a UAC2 `FORMAT_TYPE` carries no channel count and **no
sample rate at all**, a feature unit's controls are two bits wide rather
than one, and the request encoding is different — UAC1 puts the
direction in the request code (`SET_CUR` 0x01, `GET_CUR` 0x81), UAC2 has
one `CUR` code and puts the direction in `bmRequestType`. A UAC1 request
sent to a UAC2 device is a request number it does not implement, so it
stalls, and nothing prints.

**The rate is therefore SET, and that request is the negotiation.** It
goes to a Clock Source entity, so binding one of these devices is the
first thing in this driver that writes to a device in order to decide
whether it can use it — which is why `set_clock_rate()` runs at BIND
and a refusal means not bound, rather than a discovery made later when
somebody presses play.

**Which clock is ASKED, not chosen.** The entity the streaming
interface's input terminal names may be a clock SELECTOR rather than a
source. The obvious implementation picks a source and sets the rate on
it; the reason this one issues a `GET_CUR` on the selector first is that
on the device it was written against — a Sound BlasterX G6 — the
selector is a front-panel mode: "DSP Clock" and "Stereo Direct". Picking
one at bind would silently override what the owner set on the hardware.
Measured: the device came up on Stereo Direct and the driver set the
rate there.

**The feature unit follows the speaker.** That device has EIGHT feature
units — one per input, one per path — and taking the first would put the
system volume slider on a microphone. The playback one is found by
walking to the output terminal whose type is not USB streaming and
taking its `bSourceID`. The same walk is correct for UAC1, where it
happens to pick what "the first one" would have.

**What is NOT built: the feedback endpoint.** The G6's OUT endpoint is
ASYNCHRONOUS and carries a feedback IN endpoint beside it, whose 16.16
value says how many samples per microframe the device's own clock
actually wants. Nothing here reads it — `xhci.c` has no isochronous IN —
so the host sends the nominal six frames every 125 us and the device's
buffer walks against its crystal at roughly 50 ppm, which is a click
every few minutes once its ~10 ms of lock delay is used up. Staged
deliberately: it is the difference between hearing the device and
hearing it indefinitely, the first is what proves every other piece, and
`docs/roadmap.md` carries the second.

**Why the sample width is converted rather than negotiated.** (The ring has been s32 since 2026-10-06 -- "The sound ring is s32, and a 16-bit card narrows in its driver" below.) The G6
offers 24-bit and 32-bit and no 16-bit at all, so "refuse anything that
is not `sound_abi.h`'s format" would have refused it outright. Widening
the ABI is a real project (rate and format negotiation through the ring,
`usnd` and every app); shifting an s16 sample into the top of a 3- or
4-byte subslot is one function. The trap that earns its comment is the
direction: into the BOTTOM of the subslot is a 256x attenuation, which
sounds like silence rather than like a bug.

## A per-chip vendor driver, in a kernel whose rule is "a class, not a device"

`net_usb_ecm.c` speaks CDC-ECM, the standard, and every other USB driver
here is a class driver on purpose. `rtl_usb.c` (then `net_usb_r8153.c`)
is a per-chip register map for one Realtek family, which is the
opposite. It exists because the standard did not work on the hardware.

**The measurement that decided it.** The TP-Link UE300 offers Realtek's
protocol as configuration 1 and CDC-ECM as configuration 2. In
configuration 2, on a live segment, toy-os received zero frames -- and
so did Linux's own `cdc_ether`, at SuperSpeed and again at high speed
(`docs/bugs.md`). An oracle sharing none of this code failed
identically, which is what turned "our driver is broken" into "this
adapter's ECM is a compatibility checkbox its maker never exercised":
Linux always binds `r8152` to configuration 1, so nothing selects
configuration 2 in normal use.

So the choice was not "standard versus vendor". It was "a vendor driver
or no working USB Ethernet at all on the only adapter this project has".
That is the same argument that gets a vendor driver into any real OS,
and it is worth writing down because the general rule -- prefer a class
-- is right and would otherwise have been applied to a case it does not
fit. The class driver STAYS, for an adapter whose ECM works.

**Why the vendor configuration is gated on an id table.** Class 0xFF
describes nothing at all, so "a vendor interface is driveable" would
have made enumeration pick the vendor configuration of every device that
has one and then write to its registers. `usb_r8153_claims()` is the
gate: a device nobody names keeps configuration 0 and whatever class
driver can take it. This is why the id table lives in the DRIVER and is
asked by enumeration, rather than enumeration carrying a list.

**Why BSD rather than Linux.** Both describe this chip. `r8152.c` is
GPL-2.0 and transcribing it into an MIT repository would relicense the
result; FreeBSD's `ure(4)` is BSD-2-clause, which is compatible if the
notice travels with what was adapted. Register addresses, bit names and
descriptor layouts are facts about the silicon and carry no notice --
the ORDER of the initialisation sequence, and the reasoning about which
writes it needs, are what was taken, and `LICENSE` carries Kevin Lo's
notice for them. The practical rule for a future session: a datasheet
fact is free, somebody's code is not, and "I only read it for reference"
is not a distinction a licence makes.

**Why receive aggregation is OFF, where ure(4) turns it on.** The device
can pack several frames into one bulk transfer, and the receive path
walks a packed transfer correctly (`usb_r8153_rx_step`, with KTESTs that
feed it two frames neither of which is 8-aligned). It is still disabled,
because with it on a bug in the walk and a dead receive path produce the
same symptom -- nothing arrives -- and this driver was written against
hardware that had just spent a day producing exactly that symptom for a
different reason. It costs throughput and nothing else, and turning it
on is one line once somebody wants the throughput.

## A driver lives with the class it registers with, not the bus it sits on

`kernel/drivers/` groups by CLASS REGISTRY -- `block/`, `display/`,
`net/`, `sound/`, `input/` -- with `virtio/` and `usb/` holding the
shared buses and nothing that rides on them. So USB HID is
`input/input_usbhid.c`, USB audio is `sound/sound_usb.c`, and both USB
Ethernet adapters are `net/net_usb_ecm.c` and `net/rtl_usb.c`.

**This was the other way round until 2026-08-31**, and the move is
worth recording because the two obvious references disagree flatly.
Linux files by function first: `drivers/net/usb/r8152.c`, `sound/usb/`,
`drivers/hid/usbhid/`, with `drivers/usb/` holding only `core/` and
`host/`. FreeBSD files by bus first: `sys/dev/usb/net/if_ure.c`,
`sys/dev/usb/input/` -- and `if_ure.c` is the very file
`rtl_usb.c` was written against, so "follow the reference" argued
for staying put.

**What decided it is that a class registry is the extension point.**
`display_driver`, `block_device`, `net_device`, `sound_device` and
`input_source` are how this kernel is meant to grow, and CLAUDE.md's
own delivery rule asks a change to draw what a component PLUGS INTO.
If the directory names the registry, it should be the complete list of
its implementers -- and it very nearly was: `block/` and `display/`
listed every one, while `net/` listed two of four and `sound/` one of
two, with USB the sole exception in both. A tree where "how do I add a
NIC?" is answered by one directory beats one where the answer is
"`net/`, and also grep `usb/`".

The bus does not lose anything by it. `usb/` is now the controller,
enumeration and the hub -- a coherent unit that can be described in a
sentence, where before it was the bus plus four unrelated devices that
happened to be on it.

**Symbol names did NOT follow the files.** `usb_hid_bind()`,
`usb_audio_bind()`, `usb_net_bind()` and `usb_r8153_bind()` keep their
names, declared in `usb.h`, because they name what the function IS --
the entry point a USB class driver exposes to enumeration -- and
enumeration is the caller. Renaming them to match a directory would
make the seam harder to see, not easier.

**And the USB drivers do not get virtio's adapter split.**
`virtio/virtio_net.c` plus `net/net_virtio.c` is a real dependency
boundary: the virtio driver has never heard of `netdev.h`, and the shim
is 32 lines stating which of its behaviour the class may use. A USB
class driver has no such seam -- the descriptor parsing, the framing,
the MAC and the link state ARE the class-specific work -- so a shim
would restate what the driver already knows. What still crosses the bus
seam there is a call to `xhci_*` by name, which is the same
no-vtable-without-a-second-implementer rule `usb.h` already argues.

## A driver declaration is DATA in a linker section, not a call in its init

`api/driver.h`'s `DRIVER_DECLARE` puts a `struct driver_decl` into the
`.drivers` section at file scope. It was `DRIVER_REGISTER("name",
"class")` -- a call at the top of the driver's `init()` -- and the
change is worth recording because the first version looked correct and
its rule was written down.

**The rule was "call it before you look for hardware", and it does not
survive contact with a probe.** A driver that finds nothing must still
appear: "compiled in but idle" is the answer no per-class registry can
give, and it is the reason `lsdrv -a` exists -- the flag half of a
command whose DEFAULT is the opposite question, what is driving
something right now. `ahci.c`, `xhci.c` and
`ac97.c` honoured that, the last with a comment saying why. `e1000.c`
and `vmsvga.c` did not -- `e1000_init()` returns at `if (!pci) return;`
("the ordinary case on a machine without one") a hundred lines above its
registration, so every QEMU boot without an e1000 listed no e1000
driver. The fact `lsdrv` was built to report was wrong in the common
case, and the listing gave no hint: an absent driver and an absent
device look identical.

**Nothing static could have caught it.** A checker can see whether a
file contains the call; it cannot see whether a return above it fires.
Making the declaration data removes the question rather than policing
it -- the entry is in the image whether or not a line of the driver ever
runs. `KTEST()` already does this with `.ktests`, Linux does it with
initcalls and KUnit, and the reason `ktest.h` gives is the same one:
a list you maintain by hand is a list that drifts.

**The split that falls out of it.** Presence is static and binding is
not, so they are recorded by different mechanisms: the section for the
first, `driver_bound()` from inside the class registry for the second.
The registry call replaced ten scattered ones, four of which were
DUPLICATES -- every net driver set `dev->driver` and then called
`driver_bound()` with the same string literal, two sources of truth for
one fact. The device struct's field is now the only place a driver
names itself, which is `blk_register()`'s existing "the registry checks
what the device claims" instinct applied to identity.

**What is still not enforceable, and what was built instead.** Nothing
can make a NEW file declare anything -- a file that says nothing
compiles. So `tools/check_drivers.py` requires every `.c` under
`kernel/drivers/`, and any file calling a class registry, to carry a
`DRIVER_DECLARE` or a `driver-none: <reason>` comment. That is Linux's
split too: `dev->driver` is set by the core during bind and cannot be
forgotten, while `driver_register()` is an explicit call no mechanism
forces. The waiver is a comment rather than a macro so that a file whose
whole point is not being a driver need not include `driver.h` to say so.

## The Intel display driver adopts the firmware's mode, because a modeset nobody can test is a black screen

*Superseded in part, 2026-09-03:* the driver still adopts the firmware's
mode at boot, but it CAN set one now -- the native mode, through
`intel_modeset.c`, reached only by `set_mode` or the `kernel.intel_cycle`
tunable. What made that safe was the staging this entry's argument
implied: the readout first (the EDID entry below), then one mechanism
per flash with the maintainer at the panel, each logging its readbacks
and proving itself by the frame counter and the lane status rather than
by eye. The boot path still writes nothing that can black the screen.

`kernel/drivers/display/intel_display.c` drives the bare-metal laptop's
Broadwell GPU, and the shape it takes is the opposite of vmsvga's and
bochs's: it never programs a mode. Linux's i915 reads out the state the
firmware programmed and adopts it ("fastboot"), then falls back to a
full modeset when the readout disagrees with what it wants. toy-os keeps
the readout and drops the fallback.

**Why.** Three facts, all measured on 2026-09-02. Nothing emulates this
GPU -- QEMU has no Intel display model and the dev machine's card is
NVIDIA, so there is no passthrough -- which makes every register write
verifiable only by eye on one machine. The GOP already lights the panel
at its native 1920x1080, so a modeset at boot would re-derive a mode
that is already on screen. And nothing above the display layer survives
a runtime mode change yet (the reason bochs and virtio-gpu refuse
`DISPLAY_CAP_MODESET`), so a driver that COULD set modes would have no
caller for it. The cost of a wrong PLL or transcoder write is a dark
panel and a reboot over the network; the benefit, today, is nothing.
What the readout enables is everything vesafb lacks that does not touch
the pipe: the cursor plane, the backlight and the power well.

**The probe claims only on exact agreement.** It finds the pipe whose
primary plane is enabled, in BGRX8888, with the stride GRUB reports and
a surface offset equal to GRUB's framebuffer address minus the aperture
base. Anything else -- a different pipe layout, a tiled surface, a
framebuffer outside the aperture, a non-gen8 id -- declines, and vesafb
takes the same pixels. A probe that changes nothing is what made the
first boot on the laptop safe to look at: it logged the whole readout
(plane, cursor, backlight and power-well registers, the framebuffer's
GGTT entry) and the write paths were written against those numbers.

**The cursor's GGTT slot is the first page past the stolen region**
rather than a free entry found by scanning. Firmware commonly points
every unused entry at a scratch page WITH the valid bit set, so "free"
is not a property a PTE reports; an offset the firmware had no reason
to map is. The slot's previous contents are logged in case a firmware
ever proves that wrong. The image's cache attributes copy the low bits
of the framebuffer's own PTE -- the one encoding known to work on that
machine -- and the buffer is CLFLUSHed after every upload, because the
display engine's reads do not snoop the CPU cache (the same class of
bug as HDA's NOSNOOP). The plane blends premultiplied alpha, as a
Wayland cursor surface is, so the compositor's straight sprite is
premultiplied on the way in.

**The backlight follows the firmware's PWM choice rather than
switching it.** Linux's `lpt_setup_backlight` moves a CPU-mode machine
onto the PCH override and disables the CPU PWM. Here the driver reads
which PWM is enabled and writes the duty into that one, leaving the
mode alone: the reversible edit on a machine that can only be watched
by eye. On the test laptop the firmware already runs PCH override, so
the two agree; a CPU-mode machine will exercise the other branch.

**What is deliberately not here, and where it went.** A page flip on
vblank needs the compositor to stop copying only its damage box (a
second scanout would show pixels two frames old outside it) -- Wayland's
`buffer_age`, a small protocol change over `WIN_REQ_FB_MAP`/`_PRESENT`
that `docs/roadmap-details.md` designs and the roadmap marks NEXT.
Screen blanking, modesetting and the blitter are roadmap items in the
same section. An HDMI audio codec answers now that the power well is
requested, and stays a sound-side item.

**One finding on the way, recorded in `docs/bugs.md`:** `sum` on the
laptop disagrees with zlib's crc32 on a 5 MB file while agreeing on a
10-byte one, though the bytes on disk are identical. The verified
flash procedure is therefore `remote.py get` and a host-side compare,
not the checksum.

## EDID is parsed once by the display layer, and the Intel driver proves its register map by readout before it writes it

Two calls made on 2026-09-03, the first day of Intel modesetting.

**The EDID lives in `display.c`, not in the driver that fetched it.**
Linux keeps one `drm_edid` parser and every connector -- DP, HDMI, eDP,
the virtio-gpu model -- hands it bytes; Windows likewise parses in the
OS and caches the block in the registry, and the miniport only reads
the wire. The obvious toy-os shape was a parser inside
`intel_display.c`, since the laptop's panel was the one reason to want
it. That was declined for a testing reason as much as a layering one:
nothing but the laptop can run the Intel driver, so a parser there is
untestable anywhere the gate runs, while QEMU's `-vga std` and
virtio-gpu both offer an EDID for the asking. With `read_edid` a
nullable op on `display_driver` and the parser in
`kernel/drivers/display/edid.c`, the suite exercises the parser and the
`display: EDID` log line on every default boot, a KTEST pins the
descriptor's nibble packing against a canned panel, and the Intel AUX
channel is one more source of bytes. It is deliberately NOT a
capability bit: capabilities are things the layer draws with and the
honesty check refuses a driver that claims one without the function; an
EDID is a fact about the monitor, and a display without one is not a
lying driver.

The fact reaches ring 3 as `QUERY_DISPLAY` -- a scalar record with the
mode on screen, the driver's name and capabilities, and the EDID's name
and preferred timing -- because a monitor's name is user-facing (About
shows it) and because a test tool needs a text answer. `lsdisplay` is
the `ls*` sibling that prints it.

**Stage 2 of the modesetting plan is a readout, and it is the design
rather than a detour.** i915's fastboot reads the hardware state back
into a `crtc_state` and runs the same comparison it uses to verify its
own modesets (`intel_pipe_config_compare`); a mismatch there is a bug in
the readout or in the encoder code, and it is found before a modeset,
not by one. toy-os copies that shape: `intel_readout.c` decodes the
transcoder timings, the DDI function control, the port clock and the
link M/N into the same `edid_timing` the parser produces, and logs
`MATCHES` or `DIFFERS FROM` against the panel's preferred timing. The
alternative -- write the native mode and see whether the panel comes
back -- costs a black screen per wrong bit on a machine that can only be
reached over the network, and a panel that comes back proves only that
the firmware's values were re-written, not that they were understood.
The decoders are pure functions so they are KTESTed on every machine;
the register walk itself is the one part only the laptop can run.

What was deliberately not done: `DISPLAY_CAP_MODESET` on the Intel
driver. Listing the native mode alone would be honest, but the setting
would then offer one choice where it now shows its sentence, and
`set_mode` would have nothing to do until stage 3 exists.


## One MSI-X vector per device, not one per queue

Every driver here that takes interrupts now asks for a message-signalled
vector before falling back to its pin, and each takes exactly ONE --
config-change notifications and every virtqueue point at MSI-X table
entry 0.

Linux does not do this. `virtio_pci_common.c` tries a vector for config
plus one per virtqueue, falls back to config plus one shared by all
queues, and only then to INTx; `pci_alloc_irq_vectors()` is built around
a caller asking for a RANGE and accepting fewer. Windows is the same
shape from the other end -- a driver declares `MessageNumberLimit` in
its INF and the HAL grants up to that, with StorAHCI taking a message
per port and NDIS RSS one per receive queue.

The reason to differ is that **a vector per queue is a CPU-steering
mechanism, and there is nothing here to steer at**. What per-queue
vectors buy is two queues being serviced on two cores at once; Linux has
`pci_alloc_irq_vectors_affinity()` for exactly that, and the MSI message
address carries the destination LAPIC id in bits 19:12 to make it
possible. `QUERY_CPUS` reports every application processor `online: no`
(`docs/smp-design.md` -- designed, not built), so N vectors on this
machine are N entries in a table all delivering to the same LAPIC,
running the same handler on the same core. The extra table entries would
be real and the benefit exactly zero.

So the shape is deliberately the cheap one, and the point to revisit it
is NAMED rather than left to be rediscovered: **when a second CPU comes
online, or when NVMe arrives** -- whichever is first. Both are roadmap
items. At that point `pci_msix_enable()` grows into a multi-entry
allocator and `MSI_ADDR_DEST(lapic_id())` in `pci_msi.c` stops being a
constant; that one line is where affinity will live, and it is the only
place in the kernel that names a message's destination.

**NVMe arrived first (2026-09-24), and the answer was still one vector.**
The argument above is about CPUs, not devices, and it held: the driver
creates ONE I/O queue pair and points it and the admin queue at table
entry 0. See "NVMe runs one I/O queue on one vector" below.

Two smaller calls fell out of the same work. **The ladder helper stops
at MSI**, not at the pin: `pci_msi_request()` tries MSI-X then MSI and
returns 0 for "use your pin", because what a driver does without a
vector is the part that genuinely differs -- AHCI stays polled, e1000
switches to a poll, AC'97 gives up, the xHCI polls regardless. A helper
that owned the third rung would have to encode four different answers.
And **virtio refuses plain MSI** rather than accepting it: a virtqueue
names its message by MSI-X table entry, which MSI has not got, so a
virtio device on MSI could signal configuration changes and nothing
else. No virtio device offers MSI without MSI-X, so the rung is dead
either way -- but taking it would have been a device that enumerates
perfectly and never delivers.

## A driver for hardware nothing emulates is written in two halves

QEMU models no Realtek PCIe NIC -- `rtl8139` is a different chip with a
different register set -- so `r8169.c` is the first driver here whose
hardware cannot be reached by any automated test. The obvious answers
were both bad: ship it untested, or refuse to write it.

What it does instead is split. The descriptor rules -- what a transmit
`opts1` must carry, what a completed receive `opts1` means -- are three
functions in `kernel/include/kernel/r8169.h` with no register access in
them, and `r8169_test.c` KTESTs them in QEMU like anything else. That
is `rtl_usb.c`'s shape, and it is chosen for the same reason: the
silent bugs live there. A receive length used as reported delivers four
bytes of Ethernet FCS as payload, which every checksum above then fails
on; a transmit ring with EOR nowhere sends the engine off the end of
it. Both look like a working driver right up until they do not, and
both were confirmed to fail the tests before the tests were believed.

The other half -- bring-up order, interrupts, the PHY -- is only ever
exercised on the real card, so the machine has to survive being wrong
about it. Two things make that true, and they are a pair. `grub.cfg`'s
"previous kernel" entry needs a nonzero GRUB timeout to be reachable at
all, which `remote.py flash` refuses to proceed without. And `nor8169`
on the boot line keeps the NEW kernel while leaving the card alone,
which is the more useful of the two when the question is whether the
NIC is what broke: the old kernel answers "it works without your
change", the flag answers "it works without your driver".

**The PHY is checked before it is forced.** Firmware that has no reason
to bring the PHY up -- PXE and wake-on-LAN both off -- leaves it
powered down, and the symptom is a link that never arrives with nothing
to retry. Always restarting autonegotiation would undo a working
firmware setup and add a wait to every boot; never doing it leaves that
machine dead. So the link state is read at probe and the MDIO power-up
and autonegotiation restart happen only when there is none, without
blocking the boot on the result -- the link-change interrupt is what
reports the answer.

**The match table is two device IDs, not the family.** `0x8168` and
`0x8136` share the registers and the bring-up. The PCI RTL8169
(`0x8169`) and the 2.5G RTL8125 (`0x8125`) each differ in the parts
this file would have to get right, and neither can be tested here --
claiming hardware on the strength of a family resemblance is how a
driver writes to somebody else's registers.


## A Realtek USB chip is an ops table over one transport core, and shares nothing with the PCI parts

The RTL8156 (2.5G, 2026-09-09) was the second Realtek USB chip to
drive, and the question it forced was what the family shares.

**What real systems do.** Linux's `r8152.c` is one driver for every
RTL815x with a `struct rtl_ops` per chip version -- init, enable,
disable, up, down, PHY config -- selected from the version register;
FreeBSD's `ure(4)` is one file with `if (flags & 8156)` branches through
the same functions. Neither shares a line with its PCI Realtek driver
(`r8169.c`, `re(4)`): a different transport (MMIO against vendor
control transfers), a different register map, and only the PHY in
common -- which Linux keeps in phylib, a layer toy-os does not have.

**What toy-os does.** `rtl_usb.c` is the core: the four-bytes-behind-a-
byte-enable register layer, the OCP PHY window, the 8/24-byte framing
walk, the bulk pair, link polling, binding. `rtl8153.c` and `rtl8156.c`
each fill a `struct rtl_usb_ops` and name the versions they drive; the
core refuses any version no file claims. The ops shape rather than
FreeBSD's flag branches because every later chip would add branches to
every function of one growing file, where a table adds a file -- and
because the 8156's init and reset genuinely are different sequences
sharing only helpers (`rtl_hw_reset`, `rtl_disable_teredo`,
`rtl_phy_status`), which is what a table expresses and a branch hides.

**Two consequences.** A future RTL8153B is `rtl8153b.c` and one row in
`rtl_usb_chip_for()`, with its own DRIVER_DECLARE so `lsdrv` names it.
And nothing was factored towards `r8169.c`: a "Realtek PHY" layer would
be the first thing shared, and that waits for a second PCI part
(RTL8125) to need it -- the second-real-caller bar, as everywhere here.

**Declined:** branches in one file (the reference's shape; the 8156
would have taken the file past 1,500 lines and the next chip further),
and receive aggregation on the 8156 at first bring-up -- the reference
enables it with 48 KiB buffers, and one frame per transfer is what
keeps a dead receive path and a broken descriptor walk distinguishable.
It is a roadmap item with a before/after to measure.

## A module is a relocatable object linked against one export list, and a config file says which drivers are built that way

Loadable drivers (2026-09-10, `docs/modules-design.md`) forced four
decisions the staged plan had left open or got wrong.

**What real systems do.** Linux's `.ko` is an ELF relocatable linked
in-kernel against `EXPORT_SYMBOL`s placed beside each definition;
`.config` says `=y` or `=m` per driver; `depmod` writes `modules.alias`
from the objects and udev loads by `modalias`. Windows loads `.sys`
images by INF hardware id, boot-start or demand-start from the
registry. FreeBSD's kernel config file lists `device` lines with the
rest as `kld` modules.

**The export list is one file, not beside each definition.** Linux
puts `EXPORT_SYMBOL` under the function; toy-os puts every one in
`kernel/core/kexports.c`, grouped by header. The reason is what the
list IS here: with one author and one tree the export set is a
deliberate contract of a few dozen names, and a file that reads as the
module ABI in one screen is worth more than exports travelling with
their code. A stale export is a compile error in that one file either
way. `tools/gen_modalias.py` checks every `.ko`'s imports against it at
build time so the runtime refusal (by name) is never the first notice.

**`build.conf` (named `drivers.conf` until 2026-09-24, when its build options outgrew the name) rather than a Makefile variable.** `MODULES=e1000` on
the command line was the obvious shape and the wrong one: the choice
is a property of the BUILD, meant to be read and edited like Linux's
`.config`, not retyped per invocation. One tracked file,
`<name> = builtin | module`, unlisted meaning builtin, and the name is
a source file's basename so nothing maps names to paths. `e1000 =
module` ships as the default so that every QEMU boot exercises the
loader, autoload and re-bind -- a module path only a test switches on
is a path that rots.

**`modules.alias` is generated at build time, not scanned at boot.**
The kernel could parse every `.ko` in `/lib/modules` at boot for its
match tables -- it has the parser -- but `depmod`'s shape is right:
derived from the objects by the same build that ships them, so it
cannot go stale relative to them, and the boot reads one small text
file instead of every module. The alias line carries all five match
fields (`*` for `PCI_ANY`), so a class match works as well as an id.

**Two corrections to the design doc, recorded because both would be
re-derived.** The plan put boot-time loading "at the level PCI binding
runs"; `INIT_BUS` precedes `INIT_FS`, so nothing on disk is readable
there -- it is `INIT_CONFIG`, followed by `pci_rebind()`. And the plan
allocated module memory with `kmalloc`: the identity map's RAM is NX
(`paging_enforce_wx()`), so a module's text needs its own frames and
`paging_set_kernel_exec()`, or it faults on the first instruction.

**The use count is addressed by ADDRESS, not by handle.** Linux's
`try_module_get(THIS_MODULE)` needs a per-module struct the build
emits; here `module_get(&any_static_in_this_file)` resolves the module
by the address's frames, so a module pins itself with no handle and no
macro, and a registry holding one of its callbacks can pin it the same
way. A bound PCI device pins through `pci_driver_table_bound()` without
it. **Deliberately not built:** module-to-module imports (only the
kernel exports), versioning and signing -- one author, one tree, one
build.

## The I/O APIC is programmed from the MADT, and `_PRT` is read by its shape rather than executed

Routing the legacy lines through the I/O APIC (2026-09-10, the second
half of `docs/smp-design.md`'s stage 2) forced three decisions.

**What real systems do.** Linux programs the redirection table from
the MADT (ISA IRQ n -> GSI n unless a source override says otherwise),
masks the 8259, and routes PCI INTx through ACPICA's evaluation of
`_PRT` after calling `\_PIC(1)`; with no ACPI it falls back to the
BIOS's PIRQ routing and the ELCR for the trigger. Windows' HAL does the
same from the MADT and `_PRT`. Both have a full AML interpreter.

**One call for a driver, not one per controller.** `irq_unmask()` and
`pci_irq_line()` are the whole driver-facing surface; `pic_clear_mask()`
became the 8259's private business. The alternative -- keep
`pic_clear_mask()` as a facade that forwards -- was cheaper for a day
and misnamed forever, and the twelve call sites were one mechanical
edit. This is `irq_chip`'s shape, sized for one chip at a time.

**`_PRT` by shape, with one stated assumption, rather than an
interpreter or nothing.** The tables were MEASURED first
(`tools/aml_walk.py`, on QEMU i440fx and q35 and both laptops): every
APIC-mode `_PRT` is a constant package behind one of four fixed shapes,
and the only non-constant in the way is the `PICx` flag `\_PIC(1)`
sets. So the reader recognises those shapes, reads any name beginning
`PIC` as 1, follows `Return (name)` references, and refuses everything
else -- a link whose `_CRS` reads hardware included -- with the BIOS's
`interrupt_line` as the fallback. The honest case against a `_PRT`
interpreter (`docs/aml-design.md`) stands; this is not one. The cost is
stated in `acpi_prt.h`: a firmware that decides APIC routing through
anything but that flag gets the fallback, silently correct on QEMU and
merely PIC-shaped on hardware, and `acpi_prt`'s live KTEST says how
many devices each machine routes.

**i440fx keeps `interrupt_line` on purpose.** Its `_PRT` names PIRQ
link devices whose `_CRS` methods read the chipset, which is exactly
the PIC-mode table a firmware hands an OS that never called `_PIC` --
and QEMU's GSI handler fans every PIRQ out to the same-numbered I/O
APIC input, so the BIOS's line IS the input. Making the reader execute
`_CRS` to reach the same number would be the interpreter for no gain.

## A device claim is state on the DEVICE, its gate is the driver's `remove()`, and a probe runs with interrupts on

Stage 2 of `docs/umdf-design.md` (2026-09-20) -- the kernel letting go
of a device so a ring-3 driver can have it. Stage 1 could only ever
grant a device no ring-0 driver wanted, which is every device that does
not matter.

**What real systems do.** Linux VFIO splits it in two: a device is
*released* by a runtime `unbind` write or a boot-time reservation
(`vfio-pci.ids=`, `pci-stub`), and *claimed* by opening
`/dev/vfio/<group>` -- the group fd IS the claim, closing it releases,
and the node's permissions plus `CAP_SYS_ADMIN` are the authorization.
Windows UMDF has no runtime release at all: the PnP manager assigns a
device to a host process when the INF is installed. macOS DriverKit
gates it on a code-signing entitlement. Genode's platform driver hands
out device sessions by a policy file naming which component gets which
device.

**The claim is a syscall pair and a table beside the binding, not an
fd.** The fd is VFIO's shape and `shm_fd_ops` is the in-tree precedent
for one you may only mmap, so a new kind would have been cheap. Two
things decided against it. A claim fd is inherited by `fork()` and
`dup`able, so "exactly one holder" quietly becomes "one open-file
description" and `lspci -k` can no longer name a pid. And the claim is
state about the DEVICE, not about the process: keeping it next to
`g_bound[]` means `pci_device_driver()` and `dev_claim_holder_pid()`
answer the same question from one place. The fd remains the exit if a
claim ever has to be passed between processes.

**The gate is the driver's `remove()`, because there is no uid to
check.** A device whose bound driver cannot let go can never be
claimed; among releasable devices it is first come, first served. That
is a driver CAPABILITY standing in for a privilege check, and it is
said plainly rather than dressed up as a permission model -- the same
honesty `MKPART_CONFIRM` is written with.

**What it actually admits, measured rather than assumed:** `hda`, and
the two NIC drivers -- a MODULE must have a `remove()` to be unloadable
at all, so `e1000` and `r8169` were `PCI_DRIVER_REMOVABLE` before this
existed. So a process can take the network card, which is a denial of
service no worse than `kill`ing `netd`, and it is why the next ring-3
driver starts at stage 3 rather than at zero. **No STORAGE controller
has a `remove()`**, so the root filesystem cannot be claimed out from
under the filesystem -- and that is the one a KTEST asserts directly,
rather than trusting a list that will change. When a user model arrives
the check has one home, `dev_claim_take()`.

**A dropped claim does NOT rebind.** A process that dies holding a
device leaves it unbound, because a supervised driver is restarted and
must find it free -- taking it back there would race the restart and
the ring-0 driver would win about half the time. `DEV_RELEASE_REBIND`
is how a device goes back deliberately, which is the half vfio-pci's
sticky unbind cannot do without sysfs.

**AND A `probe()` RUNS WITH INTERRUPTS ON, which the bus now
guarantees.** Every probe here was written for INITCALL context, where
IF is set and a driver may sleep. Two routes reach one from a syscall
-- `sys_modload` through `pci_rebind()`, and `SYS_DEV_RELEASE` through
`pci_device_rebind()` -- where `context_switch.asm` leaves IF clear.
`hda_probe()` waits 30 ms for the link, and on a machine whose
clocksource is the PIT that is a `coarse_ticks()` loop the timer can never
advance: the machine stopped dead at one instruction, with no panic and
no log, and QMP's `info registers` named it (`RBX=4`, the tick target
`clocksource_delay_ms(30)` computes). `driver_ctx_enter()` brackets
every probe AND every remove with interrupts on and preemption still
disabled, so what a callback walks is not re-entered and only the timer
is let in. Linux is
the same shape. The `sys_modload` half of this was latent before stage
2 and is fixed by the same function -- it had never bitten because the
two modular drivers' probes do not sleep.

## A ring-3 driver's DMA buffer is what turns bus mastering on, and the parser it walks with is the kernel's own

Stage 3 of `docs/umdf-design.md` put HD Audio's codec graph in a
process (`/bin/lscodec`). Two calls it forced, both with an obvious
wrong answer.

**The buffer had to exist at all, because the plan was wrong about the
hardware.** Stage 3 was written as "no DMA and no interrupt: CORB and
RIRB are MMIO rings, polled". They are DMA rings -- the controller is
given a PHYSICAL address and masters the bus to fetch verbs and post
responses. The spec's one DMA-free verb path is the Immediate Command
Interface, Linux's `single_cmd=1` fallback, and QEMU's `intel-hda` does
not implement it: its register table runs `GCAP`..`DPUBASE` and then
the stream descriptors. So an ICI-only stage 3 would have been
hardware-only, with no automated coverage on a project whose entire
suite is QEMU. The alternative -- a kernel "send one verb" syscall with
`hda` still bound, which is what Linux's `/dev/snd/hwdepC0D0` and
`hda-verb` are -- would have moved the parsing and none of the driver,
and left stages 1 and 2 unused by their first customer.

**So `SYS_DEV_DMA_ALLOC` is a separate grant from `SYS_DEV_MAP_BAR`,
and the separation is the point.** A claimed device with only its
registers mapped has `PCI_COMMAND.BUS_MASTER` clear and cannot reach
memory at all; the bit goes up when a buffer is asked for and comes
down when the claim drops. That is `pci_set_master()`/
`pci_clear_master()`, and `pci_disable_device()` is what VFIO calls on
close. It is not containment -- there is no IOMMU, so a device that can
be pointed at this buffer can be pointed anywhere -- but it does mean
the capability is granted rather than assumed, and that the teardown
has something to switch off. **Clearing it BEFORE the frames are freed
is the load-bearing order**: a process that dies holding a claim does
not stop its device, and a card still mastering the bus over freed
frames corrupts whatever the allocator hands out next, with nothing to
connect it back to the driver that died.

**Two things about the teardown are not obvious and both were wrong
first.** The frames belong to the CLAIM, so the mapping has to come
down with them: they are borrowed, nothing else disposes of them, and
an explicit release otherwise hands the allocator memory the releasing
process can still write -- what `meminfo audit` calls a dangling
mapping. And the kernel zeroes the buffer through the write-back
identity map while the holder maps it uncacheable, so it `wbinvd`s
afterwards; a dirty line evicted later would revert a descriptor ring 3
had already written. Neither is visible to the test suite here -- the
first needs a process to outlive its release, the second needs a real
cache, and TCG ignores PAT.

The buffer is capped (`DEV_DMA_MAX_BYTES`, 64 KiB), one per device, and
DMA32. Small on purpose: every byte of it is memory a device can be
told to write, and what stage 3 needed was 4 KiB of command ring. The
cap is also what keeps this from reading as stage 5's decision having
been taken -- the audio STREAM's buffer is bigger, longer-lived and
refilled by an app, and the case for an IOMMU first is stronger there.

**And the graph parser is COMPILED TWICE rather than moved.** The
kernel still needs it: it routes the speaker and plays. "Moving" it
would have meant a second implementation, which is the hazard the
duplicate window manager cost 10,400 lines to end. `kernel/lib/
hda_codec.c` builds into the kernel and into `/bin/lscodec`, the
`geom.c`/`klineedit.c` rule, with the transport as a callback -- so the
two rings cannot disagree about a codec, and `tools/hdacodec_test.py`
asserts they do not by comparing the pin and DAC each picked.

What that buys beyond avoiding drift is the thing the stage was for: a
KTEST can drive the parser from a FAKE CODEC, a switch statement that
lies about its node counts and connection lists, so the untrusted-input
half is exercised on every boot with no hardware present. The split
that fell out is READ versus WRITE -- everything that parses moved,
everything that configures (routing, amplifiers, pin control, the
stream) stayed in ring 0 -- and that is a better line than "the graph
moves", because the write half is exactly what needs stages 4 and 5.

## A sound driver in ring 3 is asked to start, and reporting a period is what proves it did

`SYS_SND_REGISTER` lets a process implement a `sound_device`, so
`/bin/hdad` (`/bin/snddrv` since a8597d0c) drives the HD Audio controller and `soundd`, `aplay` and the
Audio Player mix on top of it without a line changed. The `sound_device`
contract is four function pointers the core calls synchronously, and a
process cannot be called. Three things fall out of that.

**THE CORE ASKS, THROUGH A PAGE AND A WAKEWORD.** `start`, `stop` and
`set_volume` become a request written into a `struct snd_driver_page`
the driver shares, plus a bump of its wakeword — the same word stage 4
already delivers the controller's interrupt on, so the driver waits once
and both sources arrive there. The sequence number is BUMPED rather than
set, because two STARTs in a row are two requests and a driver comparing
only `op` would see the second as nothing new.

**SO `start()` IS ASYNCHRONOUS, and that is the one real semantic
change.** A ring-0 driver has programmed the engine by the time `start()`
returns; this one has only asked. It got away with it because the core
already published `running` for an app to watch rather than promising
the engine was live — but a caller that had assumed otherwise would have
been wrong here first, which is why it is written down.

**AND `running` BECOMES TRUE ON THE FIRST PERIOD REPORT**, not on the
request. `SYS_SND_PERIOD` is how the driver says where the card has
reached, and a driver that reports a position has demonstrably programmed
the engine — which is the only proof the core can have, since `start()`
could merely have asked. It costs one chunk of latency, 21 ms at 48 kHz.
The alternative considered was a poll hook the core calls per frame;
there is no such hook in this kernel, and inventing one to answer a
question a report already answers is a mechanism for nothing.

**WHAT DID NOT MOVE IS THE SAMPLES.** The shared ring, the exclusive
stream and the consumed-chunk zeroing stayed in `sound.c`; the driver is
handed the ring's PHYSICAL address and points a buffer descriptor at it.
That is what keeps a ring-3 driver's exposure to one buffer on a machine
with no IOMMU, and it is the same split DriverKit's audio drivers have.
Linux does the opposite — an ALSA driver owns its own DMA buffer — which
is right when the driver is in the kernel and wrong when the question is
what a process is trusted with.

**A POLITE KILL HANDS THE CARD BACK; A CRASH DOES NOT.** A claim dropped
by a dying process deliberately leaves the device UNBOUND, so `hdad`
catches `SIGTERM` and releases with `DEV_RELEASE_REBIND` — otherwise
`kill hdad` leaves the machine mute with no recovery a person would
guess. Windows' UMDF host shuts its device down on the way out for the
same reason. The crash case is left as it is on purpose: a card
half-programmed by a driver that died is not something to hand a kernel
driver automatically, and re-running the driver recovers it -- which the
`snddrv` service's restart does since 2026-10-07.

## A sound op carries its device, so one driver can serve several cards

`struct sound_device`'s ops took NO argument -- `start(void)`,
`stop(void)`, `set_volume(int)` -- so a driver had no way to tell which
of its cards an op was for. Everything downstream followed from that:
the USB driver kept a single `g_audio` and declined a second DAC, and
`hda.c` served its two controllers by GENERATING A TRAMPOLINE PAIR per
controller (`hda0_start`/`hda1_start`) whose only job was to name a
different global. With two USB DACs attached the second could not
appear in the tray's device list at all, which is what the maintainer
hit.

**THE OPS TAKE THEIR DEVICE, and a driver recovers its state from
`dev->priv`.** Linux's `snd_pcm_ops` are handed a substream and NT's
port/miniport model hands the miniport its own object, for exactly this
reason: per-card context belongs in the call, not in a global the
callee has to guess. The USB driver now holds an array and binds every
DAC, `hda.c`'s trampolines collapse to one set, and `ac97`/`sound_proc`
ignore the argument because they genuinely are single-instance. The
alternative -- a `container_of` off the embedded struct -- was not
taken because nothing else in this kernel has one, and `priv` is
already how `snd_driver.h` spells the same idea in ring 3.

**WHAT THIS SUPERSEDES.** An earlier revision of this entry argued for
a HOST PROCESS PER DEVICE as the answer to a second DAC -- a second
`snddrv --usb-id` registering its own row, on the UMDF/DriverKit
reading. That was solving the ops-signature problem from outside, and
it left the second DAC invisible until somebody typed a command. The
ring-3 host per device remains the right shape for running a driver
OUT of the kernel, and `--usb-id` remains how a test aims it; it is no
longer how a machine gets its second DAC.

**A DEVICE NAME IS ITS IDS, not its port**, because `audio_device`
persists the name across reboots and the kernel's device table hands
out the first FREE entry -- so a port or an index moves when anything
is unplugged. Two of the SAME model still collide, and the second takes
a numbered form; which of an identical pair keeps the plain name
follows enumeration order, so that pair cannot be pinned across a
replug. That is inherent to identical devices rather than a gap to
close.

**THE RING-3 SELECTOR IS STILL NOT A CONVENIENCE, because "the first
audio device" is not a stable phrase here.** `dev_alloc()` returns the first FREE
entry in the kernel's device table, so unplugging anything moves the
devices after it up: which DAC a host grabbed depended on what had
been unplugged earlier in the boot. `--usb-id` is how a test says
which path it exercised, which `docs/bugs.md` requires of any USB
audio result.

**AND THE PLUGIN NAMES THE DEVICE IT TOOK**, `usb-<vid><pid>`, filled
into `struct snd_dev` by `open()`. Two hosts both registering
`usbaudio-ring3` would collide, and the name is what the tray's volume
popup and the `audio_device` setting select on -- so per-device naming
is what makes a device picker possible at all, rather than a second UI.
The label comes from the manufacturer string and falls back to the ids,
because several devices here return mojibake for one descriptor while
the next reads perfectly.

## A USB audio restart waits out the last stream's descriptors rather than cancelling them, and the kernel refuses a post that would lap the ring

`usbaudio_stop()` cancels nothing: the descriptors already posted play
out and the endpoint goes quiet by itself. Their completions still
arrive, and while the driver is stopped nobody reads them. The next
`start()` then counted them as the new stream's, and each one made it
post a group it had not earned. At full speed that meant ten groups in
flight over a five-group buffer, with the controller sending slots that
had already been rewritten: a crackle on every second playback,
measured 2 in 2 on the G6 by ear and by the driver's `stop` line. At
high speed the same arithmetic is 320 TDs on a 255-TRB ring.

**THE OBVIOUS FIX IS WHAT LINUX AND WINDOWS DO, AND IT WAS DECLINED.**
snd-usb-audio kills its URBs on stop, which reaches xHCI as Stop
Endpoint plus Set TR Dequeue Pointer; Windows aborts the pipe and gets
the same pair. Here that is a new usbfs call and two xHCI commands, and
the case that matters most -- a DAC unplugged mid-stream -- is exactly
the one where those commands fail. Waiting instead costs at most one
buffer (20 ms) per restart, needs no new ABI, and is bounded at 100 ms
for a device that went away with TDs on it.

**AND THE KERNEL GUARDS THE RING WHATEVER A DRIVER DOES.**
`xhci_ring_push()` has no full check, and a lapped transfer ring does
not drop a packet: the controller meets the wrong cycle bit where it
expected its next TD and stops for good. A ring-3 driver must not be
able to cause that, so `xhci_isoch_post()` refuses a TRB that would
reach the last one an event named (`xhci_ring_room()`), as Linux's
xhci does with `room_on_ring()`. It learns room only from events, so
it sets IOC itself every 64 TRBs and reports only the ones the caller
asked for -- otherwise a caller posting a whole ring without IOC would
leave it "full" for ever, which a positive control did. QEMU's emulated
controller survives a lapped ring, so the emulator cannot show what the
guard prevents; only real hardware at high speed can.

## The mixer never runs ahead of what its clients have produced

soundd used to fill the whole hardware ring on every pass. A new
client sets `running` after its FIRST write, so the pass that took the
card often found 10-18 chunks of client audio and filled the other
13-21 with silence, and the client's next writes landed after that. The
result was a 21-128 ms hole inside the sound, always a whole number of
chunks: 7 playbacks in 8 on the emulated AC97, and visible on the G6 as
the ring map at `start`. The chain-above-the-driver capture that had
been called spotless measured a first playback, which is the one case
where it does not happen.

**ALSA's dmix and PipeWire both mix only what exists**, and so does
soundd now. A chunk is mixed when every playing client has a whole one
ready, or when the engine is within `MIN_LEAD_BYTES` (8 chunks, 85 ms)
of running out -- only then is silence committed, because only then is
it a real underrun. The engine is not started until 8 chunks are there,
or until the lead stops growing, which is a short sound that has all
arrived. PulseAudio's other answer -- fill everything, then REWIND when
late data comes -- was the alternative. It does not fit here: the USB
driver copies ~3 chunks ahead of the position it reports, so a rewind
would have to stay behind a copy cursor soundd cannot see.

**THE COST**: a client that is open but idle (a paused player) is
"not ready" for as long as it stays open, so everyone else plays on
the minimum lead instead of a full ring while it does. 85 ms is still
four of the mixer's 20 ms passes.

## NVMe runs one I/O queue on one vector, and a timeout disables the controller

`kernel/drivers/nvme.c` brings up the admin queue, one I/O queue pair
(32 entries, so 31 commands in flight) and MSI-X table entry 0 shared by
both. Linux's `nvme-pci` creates an I/O queue pair PER CPU, each on its
own vector with its affinity set, and NT's `stornvme` does the same
through StorPort. **What that buys is submission without a lock and
completion on the submitting core** -- both of which need a second core
to mean anything, and `QUERY_CPUS` has none online. So the shape is
Linux's at a size of one: the queue is a real queue (a batch is up to 31
commands behind one doorbell, through the block layer's
`submit_batch`), and the per-CPU part waits for `docs/smp-design.md`,
at which point the queue count and `pci_msix_enable()`'s single entry
grow together.

**Namespaces are disks, not partitions**, each registered on its own
(`nvme0`, `nvme1`, ...), because that is what they are to a filesystem:
separately sized, separately formatted -- one can be 512-byte and the
next 4096 -- and addressed by the device, not by an offset. Linux names
them `nvme0n1`, `nvme0n2`; here the block layer's `<driver><index>`
scheme applies unchanged (`docs/decisions/storage.md` has why), which
means a second CONTROLLER's first namespace would also be `nvmeN` --
acceptable while one controller is driven, and the thing to revisit if
a machine ever has two.

**A timeout disables the controller.** A command that did not complete
may still DMA into its buffer, and that buffer is about to be handed
back to a caller who will reuse it. The one operation that guarantees
the device has stopped is CC.EN=0, which is a controller reset -- so a
timeout costs every namespace until reboot, loudly. Linux instead sends
an ABORT for the command, and resets and re-creates the queues only if
the abort also times out. That is the better answer and a roadmap item;
what this avoids is the worse one, a stale DMA landing in memory the
kernel has already given to someone else, which is corruption with no
error at all.

**FLUSH is advertised only with a volatile write cache.** IDENTIFY's VWC
bit says whether the controller has one; without it a completed write is
already durable, the block layer's flush is correctly a no-op, and
claiming BLK_CAP_FLUSH would only add a command that does nothing.

## The keyboard's active layout is a live setting; the LIST is what persists

**Decided 2026-10-05**, with several layouts and Super+Space. Three
settings in Input > Keyboard: `system.keyboard_layouts` (the ordered
list, `fi,us,de`, persisted, the FIRST is what a boot starts with),
`system.keyboard_layout` (the ACTIVE one -- applying it loads the tables
and writes NOTHING), and `system.keyboard_dead_keys` (on/off, for every
layout).

**Why the active one is not persisted.** Super+Space is pressed many
times a day; writing `/etc/toyos.conf` on each would make a keystroke a
filesystem transaction, and would make "what a boot starts with" depend
on whichever layout happened to be active at shutdown. Windows keeps a
default input method separate from the current one, and KDE starts from
the first configured layout; both are this shape. Before the list
existed, `keyboard_layout=` WAS the persisted choice -- a machine that
has only that key boots with it as a list of one, and the in-kernel
shell's `keyboard <name>` now moves the name to the front of the list.

**Why it is still a registry setting rather than a new syscall.** Setting
it moves the registry generation, which is the one change signal ring 3
already polls: the WM's tray indicator and System Settings both learn of
a switch the way they learn of every other change, with no new channel.
The cost is the trap System Settings works around in `set_keyboard.c`:
its Try it field switches the layout itself, and a page whose generation
moved is reloaded, so it marks that generation as seen.

**Why dead keys are one switch, not per layout.** XKB does this per
layout (`de(nodeadkeys)`), Windows and macOS not at all (a different
layout, US-International). The maintainer chose one switch: 20 of the 21
layouts have dead keys, English (US) none, and the per-layout form needed
a setting per list entry for a distinction few want.


## The sound ring is s32, and a 16-bit card narrows in its driver

FLAC brought 24-bit files (2026-10-06), and a 24-bit file through a
16-bit stack is a 16-bit file. The ABI, `usnd`, `soundd` and every
driver now carry **s32 with full scale in the top bits** -- ALSA's
`S32_LE` -- end to end: a codec yields it, the library resamples and
mixes in it (64-bit accumulators), `soundd` sums its clients in 64 bits
and clamps once, and the ring is s32 (`SND_CTL_MAGIC` "SND2",
`sample_bits` in the control page, `SND_DRIVER_ABI` 3).

**One format, converted at the edge, rather than negotiated per
device.** (The sample FORMAT; the RATE has been the card's since the
same day -- "A card's rate follows what plays" below.) PipeWire and the Windows audio engine both mix in one wide
internal format and convert once, at the device; per-device negotiation
would put two formats through `soundd`, both sinks and every client, and
a stream that moves between cards mid-play (which this stack does) would
have to change format under itself. The cost is twice the ring
bandwidth, 384 KB/s, which is nothing. The kernel's no-floating-point
rule is why the wide format is s32 and not float32.

**The edge is the driver, and it is cheap where the card agrees.** HDA
keeps 20/24/32-bit samples MSB-justified in a 32-bit container, so a
codec reporting any of those DMAs the ring as it is; USB's 3- and 4-byte
subslots take the top bytes per packet, as the s16 version already
shifted them, and the deepest alternate setting now wins (the G6's
32-bit one). AC97 and a 16-bit-only HDA codec -- QEMU's -- cannot read
s32, so they DMA a 16-bit copy the core keeps SND_CONVERT_LEAD chunks
ahead of the engine (`struct snd_bounce`; `snddrv` does the same for a
ring-3 driver), the copy-ahead the USB driver always had. Narrowing
ROUNDS (`snd_s32_to_s16`) rather than truncating: half an LSB of error
rather than a whole one of bias; dither is not added.

**A chunk is a length of time**: 512 frames, unchanged, so soundd's
eight-chunk lead and every driver's copy-ahead mean what they meant;
the ring doubled to 128 KiB to keep its 341 ms.

**What stays 16-bit, deliberately:** `usnd_clip_from_pcm()` and
`usnd_push()` take s16, because DOOM's sources are; `usnd_peek()` gives
s16, because the Player's spectrum is tuned to it. Each converts at its
own edge.

**The proof is not a QEMU test.** Every emulated card here is 16-bit, so
the suite only proves the narrowing. `audio_loopback_test.py --depth`
plays a 24-bit tone at -100.8 dBFS -- under half a 16-bit step, which
a 16-bit path rounds to exact silence -- through a guest owning the G6,
and finds it on the line-in recording beside a calibration tone 20.8 dB
louder.

## A card's rate follows what plays, decided by soundd and converted in the client

Until 2026-10-06 the ring was 48 kHz, fixed, and every 44.1 kHz file --
most music -- went through usnd's linear resampler on its way to a card
that could play it as it was. Now each card has a format of its own
(`/etc/sound-cards.conf`, one `[<card>]` section, edited with `sndfmt`,
System Settings' Sound > Output or Device Manager): a fixed rate, or
**match** -- the default -- which takes the card to the rate of what
plays, from the rates the person allows (44.1 and 48 kHz unless told
otherwise), and a width, the deepest unless one is chosen.

**PipeWire's shape, not Windows'.** Windows and macOS run a device at one
format the user picks and resample everything else; PipeWire runs at
`default.clock.rate` and, with `allowed-rates`, switches the graph to a
stream's rate when nothing else is playing. That second behaviour is the
one that lets a FLAC reach the DAC untouched, and fixed rates stay
available for whoever wants Windows' model.

**The card changes rate only when nothing else is on it.** A rate switch
stops and restarts the engine, which is a gap; doing it under another
program's sound would cut it. So `soundd` answers a client's request
(`src_rate` in its ring's control page) with the card's current rate
whenever anyone else is playing, and switches only for a client playing
alone -- and then only once the audio already mixed for the card has
played, which is a few passes because the asking client writes nothing
until it is answered (`rate` reads 0 while it asks). A sound with no rate
of its own -- a click, a game's effects -- asks for nothing and leaves the
card where it is. The mix is therefore always at one rate, and the
client that disagrees with it resamples: the conversion is in the
CLIENT (usnd), as it always was, never in the daemon or the kernel.

**The kernel only checks.** `SND_CTL_FORMAT` (rate, width; only while
stopped) is refused unless the card lists both -- never rounded -- and
the driver records them and programs the card at the next start, which
every driver here already re-programs from scratch: HDA's format word
(a 44.1/48 kHz base times a multiplier over a divisor), AC97's front DAC
rate under Variable Rate Audio (each rate written and READ BACK at probe,
since a codec rounds what it cannot do), and on USB the UAC2 clock (its
rates from GET RANGE, the result read back) or a UAC1 endpoint's
sampling frequency control, with the width as an alternate setting. With
no daemon the playing program's own sink applies the same rule
(`usndfmt_pick_rate()`, one function both share).

**44.1 kHz on USB is fractional packets.** 44.1 frames a millisecond, or
5.5125 a high-speed microframe, so packets carry 44/45 (5/6) frames from
an accumulator, as every UAC host does; the in-kernel driver posts each
at its own length. The ring-3 driver posts a group at one length per
syscall, so it offers only rates with a whole number of frames per
packet (48/96/192 kHz) until that syscall takes lengths.

**The library's reference rate stays 48 kHz.** Clips, `usnd_push()` and
every position and duration in usnd's API are in 48 kHz frames, so DOOM
and the Player did not change; the mixer converts clips and pushed audio
to the card's rate when it differs, and reads the streaming voice at the
card's rate directly -- which is what makes a 44.1 kHz file at a 44.1 kHz
card untouched.

**Proved by pitch, on a recorder.** `audio_rate_test.py` plays a 44.1 kHz
1 kHz tone on QEMU's HDA and AC97 and measures the WAV capture by
interpolated zero crossings: exactly 1000.00 Hz and 1500 ms whether the
card switched or was held at 48 kHz, and a positive control (the 44.1
kHz format word replaced by 48 kHz's) records 1088.4 Hz.
`audio_loopback_test.py --rate --host` does it on BARE METAL with the G6
(1187.01 Hz, no phase jumps) -- not through a guest: QEMU's USB
passthrough breaks isochronous packets whose length varies, under KVM
(398 phase jumps in 398 5 ms windows) as under TCG (364), against 0 on
the ASUS and 0 from Linux on the host (docs/testing.md).


## A driver that declines a device leaves it UNBOUND, and says why

`pci_bind()` used to record a binding BEFORE `probe()` ran, and a probe
returned nothing, so a driver that looked at a device and drove nothing
-- `xhci` on the Lenovo's EHCI controller, `hda` on the ASUS's HDMI audio
with no analog codec -- still read as its driver in `lspci`, `devctl` and
the Device Manager. "A driver looked at it" was what `lspci` wanted when
that was written; a Device Manager wants "what drives this", and the two
had stopped being the same question.

**Linux's contract, now ours:** `probe()` returns 0 or a negative errno,
and a device whose probe failed is unbound. The REASON matters as much as
the outcome -- "no driver" with no why sends a person to the source -- so
the decline goes through `pci_probe_decline(dev, fmt, ...)`, Linux's
`dev_err_probe()`: logged under the driver's name, kept beside the device
(`QUERY_PCIDEV`'s `declined_by`/`why`, which survives the log wrapping)
and recorded as an event. The bus records the binding while the probe
runs, so the helper knows whose decline it is without a global, and
clears bus mastering when it declines; everything else the probe built
is the probe's to undo, and a MODULE's late decline must unregister its
interrupt, or the handler outlives the unload.

**Rejected: a third state, "bound but failed"** (Windows' Code 10, "this
device cannot start"). It would keep the device away from a process that
could claim it and from a module that could drive it, for a distinction
the reason text already makes. **Rejected: re-offering a declined device
to the same driver on every module load** -- a re-bind pass skips the
driver that already said no; `pci_device_rebind()`, asked for one device
on purpose, still asks it again.

**hda's names stay put.** A declined controller frees its slot, and
numbering by slot would have renamed the ASUS's Wildcat Point controller
`hda1` -> `hda0` -- and `audio_device` and each card's remembered format
are kept by that name. The number is the controller's place among the
bus's HDA functions instead, which is what it was before on every
machine whose controllers all bound.

## Device events are a KEPT ring of the lifecycle, not a filter over the kernel log

The Device Manager wanted per-device history ("Events", Windows'
Properties tab; Linux's uevents). Three shapes were offered (2026-10-06)
and the maintainer chose the first:

1. **A kernel ring of lifecycle records, by device id** --
   `kernel/core/devevent.c`, read as `QUERY_DEVEVENT`. Chosen.
2. The ring plus a `dev_printf()` so a driver's own lines join the
   device's history (Linux's `dev_info()` prefixes the device). Not now:
   every driver's log calls would move, and the ring would fill with
   chatter.
3. Filtering `dmesg` by driver name, no kernel work. Rejected: `ahci:`
   is not a device and two `hda` controllers share a prefix, and the
   128 KiB log wraps -- an event list that forgets the boot is no list.

**Kept, not broadcast**: nothing in ring 3 listens for device events
(there is no udev); a reader asks afterwards, and the Device Manager's
signature folds in the newest `seq`, so a change re-lists it. **The ring
is the latest 128**, with a `seq` that counts every event, so a reader
can tell it missed some. **Ids are `userland/lib/udevice.c`'s**
(`pci:00:1f.2`, `usb:14:2357:0601`, `blk:ahci0`): the kernel names a
device the way the reader does, which is why `usb_device_id()` exists and
why usb-audio's device id moved off the root port (a card behind a hub
used to carry an id no list contained).

## The xHCI driver is split as Linux's is: bring-up, transfers, root ports

**2026-10-07.** `xhci.c` had reached about 3,440 lines -- the largest file
in the tree -- with the root ports alone over a thousand, much of it a
recovery ladder measured on the laptops. Linux divides
`drivers/usb/host/` into `xhci.c` (bring-up), `xhci-ring.c` (commands,
transfers, events), `xhci-mem.c` and `xhci-hub.c` (the root hub's
ports); this driver already had a ring file, so it took the same seams:
`xhci.c`, `xhci_xfer.c`, `xhci_port.c`, over `xhci_internal.h`.
**Moved, not changed** -- checked in QEMU (`usb_test.py`) and on the
ASUS, whose network adapter is itself on USB. The next step Linux's
shape implies -- root ports driven by the hub driver through a virtual
root hub -- is on the roadmap and waits for both laptops, because it
does change behaviour.

## The ring-3 sound driver starts as a service, and a card keeps its kernel name whichever ring drives it

`snddrv` became the default on 2026-10-07: a `/etc/services.d/snddrv`
descriptor, `Restart=on-failure`, `Ready=notify` and ordered before
`soundd`. Windows starts a UMDF host per device from the PnP manager and
Linux runs `modprobe` from udev on each device event; toy-os has neither
a launcher nor hotplug for PCI, so ONE SERVICE was chosen over a
device-event launcher -- it drives the first PCI card that plays, which
is the machine's sound device on every machine tested. A launcher is
the shape for when a second card wants its own process.

**THE NAME IS THE CARD'S, NOT THE DRIVER'S.** The plugin registers as
`hda1` or `ac97` -- what the kernel's driver calls the same controller
(`hda_number()`, copied into `hda.so`) -- rather than the `hda-ring3`
it used while it was a diagnostic. `audio_device` and
`/etc/sound-cards.conf` are keyed by that name, so a ring-3 default
under its own name would have dropped every machine's chosen card and
remembered format on upgrade, and every `service stop` would rename the
card under the mixer. Linux's predictable network names make the same
call -- `enp0s3` is the device's bus position, not its driver -- and
ALSA is the cautionary case: its card id comes from the driver, so a
laptop moved from `snd-hda-intel` to SOF loses its saved mixer state.
What says WHICH RING is the
driver column (`lssound`'s last word, `ring3` or `hda`), and the label
comes from the shared `hda_codec_vendor_name()` so both rings show
"Realtek HD Audio".

**`--pci`: A USB DAC STAYS ON THE KERNEL'S DRIVER** until the ring-3
one plays 44.1 kHz -- `SYS_USB_ISOCH_POST` posts a group at one packet
length, and a DAC that was fine yesterday must not start resampling
today. **No card is a clean exit (0)**, so init reads it as "asked to
stop" rather than a crash loop. **A crash is the restart's job**: a
dying holder leaves the card unbound on purpose (stage 2 of
`docs/umdf-design.md`), and the service claims it again in about
100 ms.

**THE RING-0 DRIVERS STAY AS THE FALLBACK** -- `service stop snddrv`
hands the card back with `DEV_RELEASE_REBIND` -- until the service has
run on both laptops; deleting them is on the roadmap. `audio_test.py`
stops the service before the phases that are about the kernel's driver.

## A remote viewer's input enters the kernel's input core, not the compositor

**Decided 2026-10-08, with the VNC server (`docs/remote-desktop-design.md`).**
`/bin/remoted` feeds a viewer's keys and pointer through
`SYS_INPUT_INJECT` into `kernel/input.h`'s reporting functions --
`keyboard_key_event()`, the mouse state -- exactly where a USB or
virtio keyboard's go. Linux's uinput is the same shape; x11vnc's XTest is
the X11 equivalent.

**The obvious alternative was the compositor**, which is where Wayland
puts it: GNOME's and KDE's servers inject through the RemoteDesktop
portal (libei), so the compositor can tag and refuse emulated input. It
was rejected here because a key is not finished when it reaches the
compositor: the layout, Caps Lock, dead keys, the Ctrl encoding
(`api/keyboard.h`'s "Ctrl and Alt"), the by-position stream DOOM reads
and the key tap all happen in the kernel's keyboard path. Injecting
above that means a second translator in toywm, and two translators of
the same keystroke drift -- the exact failure the input core was built
to end (`kernel/input.h`'s history of the keycode-to-scancode table).

**What it costs, said plainly:**

- **A privilege check with no model behind it.** The syscall answers
  only a process spawned from `/bin/remoted` -- the spawn-path identity
  the compositor already uses for screensavers and app grouping. On a
  system with no users that is the honest ceiling; it stops a casual
  program typing into the desktop, not a determined one.
- **The kernel has to find a key for a character.** VNC sends keysyms,
  so `keyboard_layout_find()` inverts the layout and Shift/AltGr are
  pressed around the key as its level needs. A character the layout has
  no key for is dropped. RDP sends scancodes and needs none of this.
- **Buttons need their own mask.** Every device report carries its
  whole button mask, so a remote press beside a local mouse would be
  released by the local mouse's next packet; `mouse_inject_buttons()`
  keeps the remote mask apart and OR's it in.
- **A crashed server must not leave Shift down for the whole machine**:
  what it holds is released from `scheduler_on_exit()`.

## Video: MPEG-1 and Motion JPEG first, decoded in ring 3, timed by the sound

Decided 2026-10-09, with the Video Player. **What real systems do:**
FFmpeg (libavformat + libavcodec), GStreamer and Windows' Media
Foundation are all user-mode frameworks with the same two tables --
containers that find each stream's packets and their times, codecs that
turn packets into frames -- and none decodes in the kernel. toy-os
follows: `userland/lib/uvid.h` has both tables, beside `usnd` and
`uimg`, for the same reason those are in ring 3 (a parser of files
anyone downloads belongs in the process that opened them).

**Which codecs.** What every video is today is H.264 (or VP9/AV1), and
every real system decodes it in HARDWARE; toy-os has no video-decode
engine driver and decodes on the CPU. An H.264 decoder is roughly ten
times MPEG-1's code (CABAC, multiple reference lists, intra prediction
modes, the in-loop deblocking filter) and its patents expired only in
part. **MPEG-1** is the smallest real codec with I/P/B pictures and
motion compensation -- the shape every later one has -- at about 900
lines here, plays 640x360 with room to spare on the ASUS's Core M, and
is what the roadmap's video wallpaper already asked for. **Motion
JPEG** costs almost nothing beside it (a frame is a JPEG; `uimg_jpeg.c`
decodes it) and is what cameras and capture cards write. Anything else
is transcoded on another machine (`ffmpeg -c:v mpeg1video`) -- or
waits for the roadmap's H.264 item, which this does not foreclose: a
codec is a row.

**The sound is the clock.** Every player times pictures by the audio
device (mpv's default, GStreamer's audio sink as the pipeline clock,
Media Foundation's presentation clock): a late picture is DROPPED, never
waited for, because a dropped frame is invisible and a gap in the sound
is not. `usnd_position()` already reports what has been HEARD (the
ring's queue subtracted), so `lib/uvid_play.h` uses it directly. With no
sound device, or muted, the monotonic clock stands in. At speeds other
than 1x the sound is SILENCED rather than played at the wrong pitch:
usnd has no time stretch (WSOLA, what mpv's scaletempo does), and is on
the roadmap.

**A video's sound is a row in usnd's codec table, not pushed by the
player.** The alternative -- demux once, push the audio packets into
`usnd_push()` -- is FFmpeg's shape and reads the file once. It was
rejected because usnd's pushed stream has no seek and no position, and
both would have had to be rebuilt for it; as a codec row, `usnd_play()`
/ `usnd_seek_to()` / `usnd_position()` work unchanged, and `aplay` and
the Audio Player play a video's sound for nothing. The cost is that the
file is opened twice and each reader skips the other's packets -- about
a megabit a second of extra reading, against a disk that streams a
hundred times that.

**Decoding is on a thread, displaying on the main one.**
`uvid_play_tick()` only picks, from frames the decoder already copied
into slots of their own, the one due now; the decoder parks by sleeping,
because a condition variable spins here (`lib/uthumb.c`'s measurement).
**Paused is what is on screen**: a slow machine's decoder runs behind
the clock, and before this rule it caught up behind a pause, so the
picture moved while paused (found by `video_test.py` under TCG).

