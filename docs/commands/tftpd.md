# tftpd

**a `/bin` program.**

**Category:** Networking

## Synopsis

    tftpd [-p <port>] [-r <root>] [-1]

## Description

`/bin/tftpd` — file transfer over the network, both directions
(RFC 1350).

**The pair to `telnetd`.** A remote shell cannot put a rebuilt binary
on the machine or take a log file off it, and neither can any other
program here — `wget` fetches, but only over HTTP and only inbound.
This is the half that moves files, and together the two are enough to
develop against a bare-metal machine you are not sitting at:

    curl -T build/userland/bin/ls tftp://10.0.0.5/bin/ls    # push
    curl -o crash.log tftp://10.0.0.5/tmp/crash.log         # pull

**Why TFTP and not something else.** It is what lab and netboot gear
has always used for exactly this job — small enough to fit in a boot
ROM, which is why the protocol exists at all — and `curl` already
speaks it, so nothing has to be written on the host side. An FTP or an
HTTP `PUT` would each have been more code here for a worse client story.

`-r` is the root every request is resolved against, `/` by default so
that `/bin` is writable. Narrow it if a directory is all you need.

## An upload replaces a file; it never truncates one

A transfer lands on `<path>.tftp-new` and only takes the real name once
its last block is in. Two things follow, and both matter for pushing a
binary to a machine you are not sitting at.

**A reader never sees a half-written file.** The old contents stay
under the old name for the whole transfer — which matters most for a
shared library, because `ld-toy` maps a `.so`'s segments *file-backed*,
so overwriting one under a running program can fault in the new bytes
beneath the old relocations.

**An aborted transfer changes nothing.** A timeout, or a client that
goes away, used to leave the target truncated: a failed push of
`/boot/boot/kernel.bin` destroyed the kernel it was replacing. Now the
partial file is removed and the original is untouched.

**Publishing is three steps, not one, because `rename` here is
create-only.** `fs_rename()` refuses an existing destination in all
three backends — TFS3, FAT32 and ramfs each answer "destination taken"
— so the old file is moved to `<path>.tftp-old`, the new one takes the
name, and only then is the old one dropped; a failure at any step is
undone. A `.tftp-old` left in a directory means a machine died between
those two operations, and it holds the file that was there before.

## `-1`, and why a TFTP transfer fails across a firewall

By default a transfer gets a fresh ephemeral port — RFC 1350's TID — and
that is the single most common reason TFTP does not work between two
machines. The reply arrives from a port the client never sent to, so a
stateful firewall sees a **new inbound flow** rather than a reply and
drops it. Linux ships `nf_conntrack_tftp` for no other purpose than to
teach conntrack about this.

That is not a theory here: the ACKs left the guest (its `tx` counter
climbed by exactly the retry count) and never reached a client one hop
away, and `curl` failed identically — an independent client, so not this
code. Answering from port 69 instead made the same transfer succeed.

`-1` is that: answer from the request socket, so the reply matches the
tuple the client sent to and any filter accepts it. **The cost is one
transfer at a time**, because port 69 is busy for the duration, which is
why it is not the default. Loading `nf_conntrack_tftp` on the client's
host is the correct fix; `-1` is the one that needs nothing from the
host, and it is what the shipped service descriptor uses.

## The transfer

A request arrives on port 69; the reply comes from a **new** port, and
the client sends everything afterwards to that one. That is the
protocol rather than an implementation choice — RFC 1350 calls it the
TID — and answering from port 69 would work once and then collide with
the next client's request.

**Options are negotiated** (RFC 2347): `blksize` up to **1428**
(RFC 2348), `windowsize` up to **3** (RFC 7440), and `tsize` on a read.
A client that asks for nothing gets RFC 1350's defaults — 512-byte
blocks, one round trip each — and still works, which is what keeps a
boot ROM able to talk to this. The server's log line names what was
actually agreed:

    tftpd: wrote /bin/ls, 150336 bytes (blksize 1428, window 3)

A lost packet is retried five times at a two-second timeout before the
transfer is abandoned.

**Why those two numbers**, since neither was chosen for speed:

**1428 is the MTU's**, not a preference. `kernel/net/ipv4.c` does not
fragment or reassemble — a fragmented datagram is *dropped* — so a
block that does not fit an Ethernet frame does not transfer slowly, it
does not transfer at all. `SYS_NET_MSG_MAX` is 1472, making 1468 the
true ceiling; 1428 is what RFC 2348 names, leaving room for a tunnel in
the path. A client asking for more is answered with 1428, and the OACK
says so.

**3 is the RECEIVER'S SOCKET QUEUE.** `kernel/net/socket.c` holds
`SOCK_QUEUE` (4) datagrams per socket and leaves one slot unused, so
three arrive and the rest of a window is discarded at the door —
dropped on arrival, not lost in the network. A window bigger than that
is *slower than no window at all*: measured, a window of 16 took 611
seconds for 1 MiB against 21 for plain lockstep, because every round
trip delivered three blocks and retransmitted thirteen. Raising it
means raising `SOCK_QUEUE` first.

**Writes are buffered to 64 KiB.** Every `write()` is one complete TFS3
transaction ending in two barriers, so a block per write made a 4.7 MB
push over nine thousand of them. Measured on the bare-metal laptop
before this: 32 ms per 512-byte block, of which ~1.8 ms was the network
— the rest was a 10 ms scheduler tick per round trip and ~22 ms of
filesystem. All three are what the numbers above address.

## What it is not

**No authentication.** Like the protocol, and here that means anybody
who can reach the port can replace any file under the root. The service
ships **disabled** for it:

    service enable tftpd
    service disable tftpd

Read `telnetd`'s page on the same subject; the warning is the same one
and slightly sharper, because this one writes.

**`netascii` is accepted and treated as `octet`**, not translated.
Every caller here is moving a binary, and a silent CRLF rewrite of an
ELF file is worse than not supporting a mode.

**A path containing `..` is refused outright**, not sanitised —
stripping it would leave the question of what the stripped path now
names, which is the class of bug path handling keeps producing.

## See also

`telnetd`, `wget`, `httpd`, `service`.
