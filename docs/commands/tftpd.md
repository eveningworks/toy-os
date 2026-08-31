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

Blocks are 512 bytes and each is acknowledged before the next is sent,
so a transfer is one round trip per block. A lost packet is retried
five times at a two-second timeout before the transfer is abandoned.

## What it is not

**No authentication.** Like the protocol, and here that means anybody
who can reach the port can replace any file under the root. The service
ships **disabled** for it:

    service enable tftpd
    service disable tftpd

Read `telnetd`'s page on the same subject; the warning is the same one
and slightly sharper, because this one writes.

**No option extension** (RFC 2347/2348), so there is no `blksize`
negotiation and blocks are always 512 bytes. A 200 KB binary is
therefore ~400 round trips and a few seconds. That is the first thing
to add if the wait becomes annoying.

**`netascii` is accepted and treated as `octet`**, not translated.
Every caller here is moving a binary, and a silent CRLF rewrite of an
ELF file is worse than not supporting a mode.

**A path containing `..` is refused outright**, not sanitised —
stripping it would leave the question of what the stripped path now
names, which is the class of bug path handling keeps producing.

## See also

`telnetd`, `wget`, `httpd`, `service`.
