# A kernel debugger

A staged plan, in the shape `docs/fslock-design.md` and
`docs/smp-design.md` use. It answers one question: **how does a real
`gdb` stop a toy-os machine that no emulator is standing behind, set a
breakpoint in it, read and write its memory, and let it go?**

**Status (2026-09-27): stage 2, the halting stub over serial, is BUILT;
stage 3a, the network transport on a dedicated e1000, is BUILT and
tested under QEMU; stage 3b, the Lenovo's onboard r8169, is BUILT and
has stopped real hardware -- a breakpoint hit from a syscall, a
backtrace, memory, detach. Files can be sent to the machine through the
debugger (`remote put`, below). Stage 3c (the ASUS's one USB NIC) and stage 1
(live inspection without halting) are designed, not built. Stage 2 came first at the maintainer's choice, because every
later stage stands on it.**

## Why

QEMU's own stub (`make debug`) needs no guest code, and was the whole
answer while every toy-os machine was emulated. Two laptops run toy-os
now, and most of `docs/bugs.md` reproduces only on them; diagnosing one
has meant klog lines, a rebuild and a reflash per question. What a
debugger buys is asking the next question without the rebuild.

## What real systems do

- **Windows** has KD in every kernel image, off until `bcdedit /debug
  on` and a reboot -- the running system cannot turn it on. Transports:
  serial, 1394, USB, and since Windows 8 **KDNET**, which drives the NIC
  itself through a small polled module of its own, never through the
  OS's NIC driver or its TCP/IP stack, because those are frozen while
  the kernel is stopped. Traffic is encrypted with a key set at
  `bcdedit /dbgsettings net`. Separately, **local kernel debugging**
  (`kd -kl`, LiveKd) reads a running kernel's memory without stopping
  it.
- **Linux** has **KGDB**, a GDB remote-protocol stub (`kernel/debug/`,
  with `arch/x86/kernel/kgdb.c` for the CPU half), armed by `kgdboc=` on
  the boot line and `kgdbwait` to stop early. Mainline transports are
  serial (`kgdboc`) and the EHCI debug port; **there is no network
  transport in mainline** -- kgdboe existed out of tree, over netpoll,
  and was never merged. For looking at a RUNNING kernel Linux mostly
  does not stop it at all: `drgn` over `/proc/kcore` reads live memory
  with full types, and kprobes, ftrace and eBPF trace without halting.
- **QEMU's gdbstub** stops the emulated CPU, so it works before the
  kernel runs and on a machine too wedged to run anything -- and only
  under QEMU.

toy-os follows **Linux's shape for the stub** (the standard GDB protocol,
generic protocol code beside an arch half, armed only from the boot
line) and **Windows' shape for the network** (a transport that owns the
NIC through its own polled path, with a key). Copying the protocol
rather than inventing one is the decision with the most weight: GDB,
its DWARF reader and its disassembler are the expensive part, and a
stock `gdb` is on every developer machine.

## Stage 2: the halting stub over serial -- BUILT

`kdebug=ttyS1` (or `ttyS2`, `ttyS3`, optionally `,wait`) claims that port
at boot, polled and never interrupt-driven. From then on:

- **Entry.** `#BP` (a breakpoint or a compiled-in `int3`), `#DB` (a
  step, or a DR0-3 slot firing), an NMI, a `^C` or the first `$` of an
  attaching debugger (polled from the timer tick), a ring-0 fault about
  to panic (`kdebug_fatal()`, at the faulting frame), and
  `panic_finish()` for the panics that have no trap frame.
- **Protocol** (`kernel/debug/gdbstub.c`): `?`, `g`/`G` (the 24 core
  registers; GDB marks x87/SSE unavailable), `m`/`M`, `c`/`s`, `Z0`-`Z4`
  (software breakpoints; `Z1` hardware execute; `Z2` write and `Z4`
  access watchpoints -- x86 has no read-only watch, so `Z3` is "not
  supported"), `D`/`k` (both detach -- a kernel is not killed by its
  debugger), `qSupported`, `qOffsets` (the KASLR delta, so GDB relocates
  an unmodified `kernel.bin`), `qAttached`. Everything else gets the
  empty reply, which the protocol defines as "unsupported".
- **CPU half** (`kernel/arch/x86_64/kdebug_x86.c`): the trap frame as
  GDB's register file, DR0-DR7, and memory through a page-table walk of
  the current CR3 -- an address nothing maps is an `E14` reply, not a
  fault with interrupts off. Kernel text is read-only (W^X), so a
  breakpoint is written with CR0.WP cleared for that one store. A
  read-only USER page is refused: it may be copy-on-write shared.

**The traps, each of which breaks something silently:**

- **Nothing on the stopped path may take a lock, allocate or log.** It
  runs at an arbitrary instruction with interrupts off -- inside
  kmalloc, holding a mount lock, halfway through a klog line.
- **Software breakpoints are patched only while the kernel runs**:
  lifted on every stop, written back on every resume (KGDB does the
  same). A step does not patch the one it starts on, or it would
  execute the int3 instead of the real instruction.
- **The idle loop keeps its tick while the stub is armed**, because the
  tick is what hears a break-in. A tickless idle with nothing to do
  would sleep through the `^C`.
- **A single step masks IF** so it lands on the next instruction rather
  than in the timer's ISR, and restores it after -- so stepping a
  `cli`/`sti`/`popf` gets IF wrong. KGDB has the same blind spot.
- **Clearing CR0.WP is safe only with one CPU running.** SMP stage 3
  (`docs/smp-design.md`) must stop the other CPUs first -- an NMI IPI,
  KGDB's `kgdb_roundup_cpus()` -- before a breakpoint write or any stop.
- **Stopping stops time for everything the kernel talks to.** A USB
  audio stream underruns, TCP peers retransmit and may give up, and
  timers fire late all at once on resume. That is the nature of a
  halting debugger, not a bug to fix; stage 1 is the answer for the
  cases that cannot afford it.
- **`bt` stops at `isr_common`**, the interrupt frame: there is no CFI
  for the hop from a trap frame to what it interrupted. `info registers`
  on the stopped frame is the way across.
- **On real hardware a break-in can lose bytes**: the tick polls one
  byte per tick and a 16550's FIFO holds 16, so the rest of GDB's first
  packet can overrun. The packet fails its checksum and GDB resends it;
  QEMU's socket chardev is flow-controlled and never loses one.

Tested by `kdebug_test` KTESTs (a scripted transport plays GDB through a
real breakpoint in kernel text) and `tools/kdebug_test.py` (a real port,
a protocol client, then a real GDB).

## Stage 1: live inspection without halting -- DESIGNED

The drgn / `kd -kl` shape: read (and, deliberately, write) kernel memory
and resolve symbols on a RUNNING machine, reached over the network that
already works -- `remote.py` to `telnetd`. Nothing stops, so nothing a
halt breaks (audio, TCP, the desktop) is disturbed, and it works on the
ASUS today with no transport work.

- A privileged read/write of kernel virtual memory through the same
  page-table walk as stage 2 (`kdb_arch_mem_read/write`), exposed as a
  `/bin` program, and symbol-to-address lookup (`ksyms` only goes the
  other way today).
- Host-side, the useful half is `drgn`-like: a `tools/` script that
  reads a structure by NAME using `kernel.debug`'s DWARF and the KASLR
  delta, over `remote.py exec`.
- Gated like `telnetd`: off unless enabled, since it hands the network
  the kernel's memory.

## Stage 3: the network transport

KDNET's shape, because it is the only one that works on a machine whose
kernel is stopped.

### 3a: a dedicated e1000 -- BUILT

`kdebug=net,ip=A.B.C.D,key=HEX[,port=N][,nic=BB:DD.F][,wait]`.

- **The debugger OWNS a whole NIC**, claimed from the PCI bus before any
  driver binds it (`pci_device_claim()`): the last 82540EM, or the one
  `nic=` names. `kernel/drivers/net/e1000_kdb.c` drives it by polling
  alone, behind the `kdb_nic` interface (`kdebug_nic.h`) -- no
  interrupt, no lock, no allocation after bring-up. **Chosen over
  sharing the OS's NIC** (Linux netpoll's shape) because no NIC driver
  here has a lock: a stop that lands mid-transmit or mid-ISR leaves a
  ring half-updated, and reusing it corrupts it. A dedicated card has
  one owner by construction. The cost is a second NIC.
- **Its own ARP and UDP framing** (`kernel/debug/kdebug_net.c`), NOT
  `kernel/net/`, which is frozen mid-call when the machine stops. It
  answers ARP for its `ip=`, announces itself once at boot, and replies
  to the MAC, IP and port of the last authenticated datagram -- so it
  never has to resolve anything.
- **Every datagram is authenticated**: `TKDH`/`TKDT` magic (the
  direction, so a reflected datagram fails), a sequence number, the
  first 16 bytes of HMAC-SHA256 under the key, then RSP bytes. A host
  datagram must carry a sequence number above every one accepted
  before, which is the replay protection. **Not encrypted**, at the
  maintainer's choice: KDNET encrypts, but a cipher here costs a kernel
  implementation and a third-party Python package on the host, since
  the stdlib has none; `hmac`/`hashlib` are stdlib. A sniffer on the LAN
  can read what a session reads. SHA-256 is `kernel/lib/ksha256.c`,
  shared with `libhash`.
- **Break-in** is the serial path's: the tick's poll reads the card's
  RX ring, so a `^C` or an attaching packet stops the kernel.
- **GDB reaches it through `tools/kdebug_bridge.py`**, TCP to keyed
  UDP. RSP's own acks and retransmits cover a lost datagram; the stub
  does no retransmission of its own.

**The trap it shipped with**: 8254x RDLEN/TDLEN must be multiples of 128
bytes, so eight descriptors is the minimum ring. A 4-descriptor TX ring
made the card resend a STALE buffer -- the boot-time ARP announcement --
in place of every reply, and the stub sat forever waiting for an ack to
a packet that never left. A pcap of the debugger's netdev
(`-object filter-dump`) and QEMU's `info registers` found it in two
steps.

### 3b: the Lenovo's r8169 -- BUILT

A second `kdb_nic` backend, `kernel/drivers/net/r8169_kdb.c`: the ring
hands descriptors back and forth with an OWN bit and a doorbell, with no
index registers to keep in step, and the bring-up is `r8169.c`'s order
without the interrupt or the PHY kick. The debugger owns the onboard NIC
at .104 and the OS networks through the RTL8156B USB adapter at .112.
**Verified 2026-09-27 on the Lenovo**: attach stopped it in its idle
loop, a breakpoint in `heap_total_bytes()` was hit by `meminfo` run over
the OS's own NIC, `bt` read through `sys_query` and `syscall_dispatch`,
and detach let the command finish.

**What it cost to get there, both in `docs/bugs.md`**: with BOTH NICs up
in the OS on one subnet, replies leave through the first matching
device, so run one per subnet; and a half-open TCP connection that never
completed used to hold its listener's backlog slot forever -- two of
them silenced telnetd until reboot. That one is fixed.

### 3c: single-NIC machines (the ASUS) -- DESIGNED

The ASUS's only NIC is the UE300 behind xHCI, so a dedicated card is not
an option there. The answer is KDNET's KDNIC: the debugger owns the
hardware and the OS's traffic is tunnelled THROUGH the debugger's driver
as a virtual `net_device`, so the ring still has one owner. Driving
xHCI's event ring by hand with interrupts off (`xhci_service()` is
public), from a stop that may have landed inside the xHCI interrupt
handler itself, is the riskiest piece of the plan and why it is last.

## Files through the debugger -- BUILT

**`remote put <host file> <target path>`** writes a file on the stopped
machine, so a kernel can be replaced over the debugger's own link on a
machine whose OS has no network. GDB sends it as `vFile:open`/`pwrite`/
`close` packets (host I/O); the stub answers those and refuses every
other vFile op, `remote get` included.

**THE STOPPED STUB MAY NOT TOUCH THE FILESYSTEM** -- it may have stopped
inside `kmalloc`, holding a mount lock, or halfway through a disk wait --
so the work is split in two. While stopped, `kdebug_files.c` only COPIES
the bytes into an 8 MiB area taken from the page allocator when the stub
is armed (the stopped path allocates nothing). On resume it wakes
`/bin/kdfiled`, a service, which takes each complete file through
`SYS_KDFILE` (`kdfile_abi.h`), writes it beside its target, renames it
into place and keeps the previous version as `<stem>.old` -- `kernel.old`
is the file GRUB's rescue entry boots, so only a path's FIRST put in a
boot rotates it. A read-only mount (`/boot`) is
remounted read-write for the write and read-only after. Windows'
`.kdfiles` has the same rule: the target pulls from the debugger at a
point of its own choosing, never from inside the stop.

**Nothing is lost for the boot by an interruption**: an open that finds a
file still open reclaims it (GDB opens one at a time, so it was an
abandoned put), and a file its taker died holding goes back to the next
taker. **A FILE WITH ONE FAILED WRITE IS DISCARDED AT CLOSE (`EIO`), never handed
on**: GDB closes after an error, and a partial `kernel.bin` renamed into
place would boot nothing. The outcome is logged with the SHA-256 of the
staged bytes -- `kdebug: kdfiled wrote <path>, <n> bytes, sha256 <hex>`
-- which `toy-dmesg` in the same session can compare with `sha256sum`.

**The network transport takes a frame off the card only while a whole
payload fits in its input FIFO**: a `pwrite` is a burst of full frames,
and a frame taken with no room for it was lost from the middle of a
packet. Leaving it on the card lets the NIC's own ring hold it.

## Not planned

- **User-space debugging.** A debugger for ring-3 processes is `ptrace`
  and belongs with signals and `strace` (`docs/signals-design.md`), not
  with the kernel stub.
- **`monitor` commands** (`qRcmd` -- KDB's shape): replaced by host-side
  gdb helpers, below, which make a stopped kernel run no code at all.

## Threads and the gdb helpers -- BUILT

**Every process is a GDB thread** (thread id = pid) and the kernel
context is one more, id 1000 -- KGDB presents tasks the same way. The
stub answers `qfThreadInfo`, `qC`, `Hg`, `T` and `qThreadExtraInfo`
(`init, blocked`), and a stop reply names its thread (`T05thread:a;`).
A thread that is not the one that stopped is read from where it is
PARKED (`struct kernel_context`: rsp, rip and the callee-saved
registers); everything else was never saved, and goes out as GDB's
"unavailable" rather than as a guess. Parked threads are read-only (`G`
refuses). **Memory is read in the SELECTED thread's address space**: its
page tables are walked and a user page is reached through its physical
address, which the identity map covers for all RAM; kernel addresses
are the same in every space and take the ordinary path.
`kernel/proc/sched_debug.c` is the scheduler's read-only, lock-free view
for this, so `kernel/debug/` never includes the scheduler's internals.

**`tools/gdb/toyos.py`** is Linux's `scripts/gdb` shape -- `lx-dmesg`,
`lx-ps` -- and WinDbg's `!process`: helpers that READ the stopped machine
from the host. `toy-dmesg`, `toy-ps`, and an UNWINDER for the one frame
GDB could not get past: `isr_common` has no CFI, so every backtrace used
to end there. At its call's return address (and at `isr_resume_frame`)
RSP points at the frame `isr.asm` pushed, so the unwinder reads it and
hands GDB the interrupted code's registers. A ring-3 frame ends the
backtrace unless its program's symbols are loaded (GDB's fallback walks
a symbol-less stack into garbage), and so does `kernel_main` (`boot.asm`
has no CFI).

**`toy-symbols`** is Linux's `lx-symbols`: symbols for what `kernel.bin`
does not hold, read out of the stopped machine. A loaded MODULE is
`g_mods[]`'s base plus a per-section layout the host RECOMPUTES from
`build/modules/<name>.o` -- the kernel keeps only the base, and
`module.c`'s `layout()` is two passes in section-index order (text,
then the rest of SHF_ALLOC after the page-rounded text), which the
helper repeats. The SELECTED thread's program is `procs[].exec_path`,
mapped to its ELF by `panic_resolve.elf_for_program()` (by basename, so
two programs sharing one can be confused -- the helper names the file it
loaded); executables and `ld-toy.so` are fixed-base ET_EXEC, so no
offset; each `/lib/*.so` is the file mapping of it at file offset 0,
whose base is the bias. There is no `r_debug`: ld-toy keeps its own
table in process memory, and the kernel's region list already says it.
