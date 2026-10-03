# ping

**a `/bin` program.**

**Category:** Networking

## Synopsis

    ping [-c count] [-s size] <address>

## Options

- `-c <count>` -- how many echo requests to send; the default is four,
  and a value below 1 is raised to 1.
- `-s <size>` -- payload bytes per request, 1 to 65507; the default is
  56. Past 1472 the request leaves in IPv4 fragments and the reply comes
  back in them, so `ping -s 4000 10.0.2.2` exercises fragmentation and
  reassembly both ways. Not 0, unlike Linux: a receive returns 0 for
  "nothing yet", so an empty reply could not be told from none.

## Description

`/bin/ping` — send ICMP echo requests to an IPv4 address and report what
comes back. It is the smallest end-to-end proof that this machine's
network works, and that is what it is for: one command exercises the NIC
driver, ARP, the IPv4 header and checksum, ICMP, the socket layer and
the scheduler's idle receive path. A reply means all of them are right.

Each request carries 56 bytes of payload by default, which is what every
other `ping` sends, so a capture taken on the host looks like the traffic
anyone would expect. Every reply is compared with its request byte for
byte, as Linux's `ping` does -- for a fragmented one that is also the
check that each fragment landed where it belonged:

    ping: wrong data byte #1480 should be 0x76 but was 0x61

The exit status is the assertion worth scripting against: **0 if
anything replied, 1 if nothing did.**

## What it is not

**Not itself a resolver.** A name is accepted and resolved through
`userland/lib/uresolv.c`, the same library `host` uses, so the two
cannot disagree about what a name means. What that costs is one DNS
round trip before the first echo, and a different failure to report
when the name is the problem rather than the network.

**Not a flood or a latency benchmark.** There is no `-f` and no `-i`.
The reply is waited for in one blocking receive, so the time is a real
round trip -- but a single one, with no averaging or deviation.

**Not a raw socket.** The ICMP header is the kernel's; this program
supplies a payload and nothing else. A program here cannot emit an
arbitrary ICMP type, which on Linux is what `CAP_NET_RAW` gates and this
kernel has no privilege model to gate with.

## Output

    $ ping -c 2 example.com
    ping: example.com is 172.66.147.243
    PING example.com: 56 data bytes
    56 bytes from 172.66.147.243: icmp_seq=1 time=20.000 ms

The resolved address is printed before the first request, so a reply
from an unexpected host is attributable. With an address there is no
lookup and no such line:

    PING 10.0.2.2: 56 data bytes
    56 bytes from 10.0.2.2: icmp_seq=1 time=0.000 ms
    56 bytes from 10.0.2.2: icmp_seq=2 time=10.000 ms
    --- 10.0.2.2 ping statistics ---
    2 packets transmitted, 2 received, 0% packet loss

Under QEMU's user-mode networking the gateway is `10.0.2.2` and it
answers echo requests itself, so that address is the one to try first. A
round trip of `0.000 ms` there is real rather than a broken clock: the
emulator often delivers the reply inside the sending syscall.

An address on this subnet that nobody answers reports the ARP failure
rather than a timeout, because those are different problems:

    ping: no ARP reply for 10.0.2.99 -- is it on this subnet?

## See also

`netctl` for the addresses this uses and the counters that say whether
frames moved at all, `host` for resolution on its own, `netd` for where
the nameserver comes from, and `docs/conventions/kernel.md`'s networking
entry for how the layers below fit together.
