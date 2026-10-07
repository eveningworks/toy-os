# The network stack in ring 3

A staged plan, in the shape `docs/umdf-design.md` used. It answers
"should ARP, IPv4, ICMP, UDP and TCP leave the kernel, what would it
take, and what does it buy?" -- and it RE-ARGUES a decision rather than
starting from nothing: `docs/decisions/kernel.md`, "The network stack is
in the kernel, and a socket is a ping socket".

**NOTHING HERE IS BUILT (written 2026-10-07).** The stage markers go on
the headings when that changes.

## The finding that shapes the plan

**The 2026-08-29 decision declined a ring-3 stack for one concrete
reason: "sockets would become IPC to a service, and this OS has no IPC
that can carry them."** Half of that is no longer true. Since then the
tree gained shared memory with an owner (`SYS_SHM_GRANT`), a general
channel (`userland/lib/uchan.h`: a shm ring plus a reply slot), and an
eventfd-shaped wait over both (`SYS_WAKEWORD`) -- the compositor, sound
and the clipboard all run on them.

**The half that is still true is the FILE DESCRIPTOR.** A socket here is
an fd: `close`, process exit, `spawn`'s named fd 0/1 (`inetd -p 7
/bin/cat`) and `read`/`write` all work on it because the fd table does
the work, not the stack. A library that turned `socket()` into channel
messages would keep the messages and lose every one of those -- an
`inetd` child could not inherit its connection. So the question is not
"is there IPC" but **"what kernel object is a socket's fd once the stack
is elsewhere"**, and every real system that moved its stack out had to
answer exactly that.

## What real systems do

| system | where the stack runs | what an app's socket fd IS |
|---|---|---|
| **Linux, FreeBSD** | kernel | a kernel socket |
| **Windows** | kernel (`tcpip.sys`) | a kernel handle (AFD) |
| **Fuchsia** | user space (`netstack3`, before it gVisor's netstack) | a **`zx::socket`** -- a kernel byte-stream object; the app holds one end, the netstack the other, and control calls are FIDL messages |
| **QNX** | user space (`io-pkt`, a resource manager) | an fd whose every operation is a **message** to the process serving it |
| **MINIX 3** | user space (`lwip` service since 3.4, `inet` before) | an fd the VFS server forwards to a **socket driver** |
| **GNU Hurd** | user space (`pfinet`, Linux's old stack as a translator) | a Mach port to the translator |
| **Android** | kernel; its `netd` is a POLICY daemon | a kernel socket |

Two lessons. **Every user-space stack needed a kernel object for the
data path** -- Fuchsia's byte-stream socket or QNX/MINIX's "the fd is
served by a process" -- and none of them made the fd a library fiction.
**And toy-os's `netd` is Android's netd** -- naming and addresses over a
kernel stack -- so the ring-3 stack gets a different name here:
`netstack`, Fuchsia's.

## What exists, measured (2026-10-07)

| | |
|---|---|
| the stack | `kernel/net/` -- eth, ARP, IPv4 + fragment reassembly, ICMP, UDP, TCP, ~2.5k lines that parse what the wire sends |
| the NIC drivers | `kernel/drivers/net/` behind `net_device`; a driver's ISR copies a frame into `net_rx()`'s static queue and returns |
| what drives the stack | NO THREAD: `net_poll()` runs from `scheduler_idle()` and from the socket syscalls; TCP's timers ride the deadline of whichever process is waiting (`net_wait_deadline()`) |
| capacity | grown on demand since 2026-10-07 (`kernel/lib/kslots.c`); it was 8 sockets and 8 connections, kernel-wide |
| the fd | `socket_fd_ops` in `kernel/net/net_syscalls.c`; a socket is an index, as a pipe is |
| kernel consumers | `fd_peer_ip()` (`kernel/proc/syscall_fd.c`) asks a socket's peer so a session that arrived over the network is recognised (`remote_log.h`); `conn_log.c` records every flow from inside the stack; `kdebug_net.c` builds its own frames on a dedicated card and shares nothing |
| the policy half | `/bin/netd` (names, DHCP leases), `ntpd`, `netheal` -- already ring 3 |

The capacity row matters for the argument: eight sockets for the whole
machine was a limit a ring-3 stack would have lifted with `malloc` --
and a kernel table that grows lifted it instead (2026-10-07), so it is
NOT a reason to move.

## The choices

### 1. Where the line is

| | |
|---|---|
| **A. Everything above the NIC (recommended)** | The kernel keeps the drivers and hands frames to ring 3 through a per-card packet ring -- the shape `sound_device` already has (a shared ring, a wakeword). ARP, IP, ICMP, UDP and TCP move together. |
| B. TCP only | Moves the largest file and keeps IP in the kernel. No system splits there, because TCP needs IP's routing and ICMP's errors on every segment; the boundary would carry more traffic than either side. |
| C. Nothing | Keep the 2026-08-29 decision and bound the parsing harder instead. |

### 2. What a socket fd is

| | |
|---|---|
| **A. A kernel stream object (recommended)** | Fuchsia's `zx::socket`, POSIX's `socketpair(2)`: a kernel byte pipe with two ends. The app's end is an ordinary fd -- `read`, `write`, `close`, exit and `spawn`'s inheritance keep working untouched -- and `netstack` holds the other. Control calls (`connect`, `bind`, `listen`) are channel messages. **It has callers of its own**: `socketpair()` for userland, and the `AF_UNIX` notify socket `SYS_NOTIFY_READY` was built instead of. |
| B. A process-served fd | QNX's and MINIX's: every `read`/`write` on the fd becomes a message to `netstack`. More general -- it is also FUSE and a device served from ring 3 -- and two context switches per call, which is the price QNX pays and designs around. |
| C. A library fiction | `socket()` in libc speaks the channel directly. Least kernel work, and it breaks inheritance -- `inetd` stops working. Rejected above. |

### 3. Whose code

**Move the existing stack, compiled for ring 3** -- the `hda_codec.c`
and `usb_audio_parse.c` rule, one source for both rings while both
exist. lwIP (BSD) is what MINIX and most embedded user-space stacks
chose, and porting it would discard a TCP that has been checked against
real servers; it stays the fallback if the move proves the existing code
too kernel-shaped (`kmalloc`, `klog`, the idle-loop driving).

## The stages

Each stage has a caller of its own before the next one needs it -- the
order `docs/umdf-design.md` used, where the BAR grant and the claim were
useful before any driver moved.

### Stage 1 -- a packet ring per card

A process can claim a NIC's FRAMES (not its registers): received frames
are copied into a shared ring instead of `net_rx()`'s queue, and
transmits go the other way. Linux's `AF_PACKET`/TAP shape, gated by the
device claim that already exists (`dev_claim.c`; the NICs are in its
claimable set). **Its own caller: a packet capture** (`tcpdump`'s job),
which toy-os does not have. The kernel stack keeps every card nobody
claimed.

### Stage 2 -- a kernel stream object

`socketpair()`: two fds, a byte ring between them, blocking `read` with
the existing park/deadline machinery, EOF when the other end closes.
**Its own callers**: `socketpair()` in tolibc, and a credential-carrying
replacement for a pipe where a daemon wants a long-lived one.

### Stage 3 -- `netstack` on a SECOND card

`/bin/netstack` claims one card's packet ring and runs the moved code;
the kernel stack keeps serving the rest. QEMU gives a guest two NICs
(`vm.py --net both`), so the two stacks run side by side and every
network test can be pointed at either -- the way `snddrv` and the
kernel's `hda.c` coexist today. Sockets on that card are stream objects
plus a `uchan` for control.

### Stage 4 -- the default, then the deletion

The socket syscalls route to `netstack` when it runs (a `snddrv`-style
service, restarted on a crash -- which drops every connection, as
Fuchsia's restart does), then `kernel/net/` is deleted once the service
has run on both laptops. `fd_peer_ip()` asks `netstack` for a peer
instead of reading the socket table, and the connection log moves into
`netstack` with the code that writes it.

## What does NOT move

- **The NIC drivers.** They master the bus and there is no IOMMU
  (`docs/umdf-design.md`, "The finding that shapes the whole plan"); a
  ring-3 NIC driver buys crash isolation only, and a separate project.
- **`kdebug_net.c`.** The GDB stub's card is polled with interrupts off
  and must work when nothing else does; it shares nothing with the stack
  today and keeps it that way.
- **The fd.** The point of stage 2 is that the descriptor stays a kernel
  object.

## What it buys -- and this time it is containment

`fontd`, `clipboardd` and `snddrv` bought **crash isolation**. This buys
more, because the stack masters no bus: a `netstack` exploited through a
malformed TCP option can read and forge every connection on the machine,
and **cannot touch kernel memory**. The DMA caveat that limits
`docs/umdf-design.md` does not apply to code that only ever sees a
ring of frames. Fragment reassembly and TCP's option parsing are exactly
the input-handling that has produced remote kernel bugs elsewhere
(FragmentSmack, SACK Panic, both Linux).

## The case against

- **Nothing is broken.** The stack works on both laptops, through
  TFTP, `httpd`, `inetd`/`telnetd` and `wget` with TLS. The argument is
  preventive, as `docs/umdf-design.md`'s was.
- **A copy and two wakeups per packet.** Every frame crosses the ring
  boundary twice (NIC -> `netstack` -> app). No test here measures
  throughput on hardware; stage 3 must, against the kernel stack on the
  same machine, before stage 4 is allowed.
- **A `netstack` crash drops every connection**, where a kernel stack
  bug panics the machine. Better, but a person's TFTP transfer still
  fails, and the restart has to be fast.
- **Stages 1 and 2 are worth building regardless** -- a packet capture
  and `socketpair()` -- which is the strongest point in favour: the
  first half of this plan pays for itself even if stage 3 never starts.

## Decided so far

Nothing. The choices above carry recommendations, and the maintainer
picks before stage 1.
