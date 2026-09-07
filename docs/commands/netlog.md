# netlog

**a `/bin` program.**

**Category:** Networking

## Synopsis

    netlog [-i|-o] [-n <count>] [-f]

## Options

- `-o` -- outgoing connections only.
- `-i` -- incoming connections only; it and `-o` are exclusive, and the
  last one given wins. Without either, both directions print.
- `-n <count>` -- print only the last `<count>` matching records.
- `-f` -- keep printing new records as they happen; Ctrl-C ends it.

## Description

`/bin/netlog` — who this machine has talked to. One line per connection,
oldest first, with the time, the direction, the protocol, the program
that asked, and the address at the other end.

`ifconfig` reports what a card did in packets and bytes, which cannot
answer *what did this machine connect to, and which program did it*.
The kernel keeps a ring of connection records (`QUERY_CONNLOG`) and this
prints them.

## A connection, not a packet

That distinction is the whole design, and every real system that logs
this makes it:

| System | Mechanism | What it records |
|---|---|---|
| Linux | `iptables -m conntrack --ctstate NEW -j LOG` | the first packet of a flow |
| Linux | eBPF `tcpconnect` (bcc) | `tcp_v4_connect`, with PID and COMM |
| Windows | WFP audit 5156, Sysmon Event 3 | one event per connection, with the process |

So here: a **TCP open** makes one record however many segments follow
it, in either direction. A **UDP or ICMP socket** makes one the first
time it sends to a given destination — conntrack's *flow*, not
tcpdump's packet — which is what puts a DNS lookup, a DHCP exchange and
a ping in the log without any of them filling it.

The hooks are at the socket layer (`kernel/net/socket.c`), not at
`ipv4_output()`. A retransmit, an ARP retry and datagram number two are
all the same connection; and only the socket layer runs in the calling
process's context, so only there is the program's name the right one.
An inbound connection is logged at `accept()` for the same reason — the
SYN arrives inside somebody else's `net_poll()`, and the server is only
named once it takes the connection.

## Output

    14:22:07  out  udp   dhcp              10.0.2.2:67
    14:22:08  out  udp   host              10.0.2.3:53
    14:22:08  out  tcp   wget              93.184.216.34:80 (example.com)
    14:22:31  in   tcp   httpd             10.0.2.2:51488

The **address is always exact**; a hostname, when there is one, goes in
brackets after it and never instead of it.

`... N records lost` under `-f` means the ring (128 records) wrapped
before this got to them. That line is why each record carries a
sequence number: without one, a full ring and a quiet network look
identical.

`no connections recorded` is an ordinary state — nothing has connected
yet — and it says so rather than printing nothing, which reads like a
failure. It names `system.conn_log` when that is what is stopping it.
`no connections match that filter` is the other answer, kept separate:
an empty log and a `-i` that excluded everything are different
situations and only the first is worth naming the setting for.

## Where hostnames come from

**The kernel never parses DNS.** A ring-3 resolver reports what it
looked up (`SYS_NET_RESOLVED`, called from `uresolv_lookup()`), the
kernel keeps a 16-entry `address → name` cache, and a record picks up
whatever name that cache holds for its address.

This is Sysmon's shape — its Event 22 DNS records are what give its
Event 3 connection records a name — rather than Zeek's, which snoops
the wire. Two consequences, both deliberate:

- A name appears only for an address something resolved **through
  `uresolv` on this boot**. An address typed in as a dotted quad has
  none, correctly.
- **A name is what a program claimed**, not what the network answered.
  Any process can call the syscall. There is no privilege model in this
  kernel to gate it with, and the address in the record is unaffected.

## What it deliberately does not do

**It does not block anything.** This is a log, not a firewall — there is
no filtering layer in this stack to hang one off, and a command that
could refuse a connection would need one first.

**It does not survive a reboot.** The ring is memory. Draining it to a
file (`syslogd`'s shape, or `journald` reading `/dev/kmsg`) is a roadmap
item, not something this command does behind your back.

**It does not report bytes.** A connection record is made at the open
and never touched again; per-connection traffic counters are a
different structure, and `ifconfig`'s per-device ones already answer the
question people usually mean.

## Turning it off

`system.conn_log` decides what is recorded, and shows up in System
Settings under **Network**:

| Value | What is recorded |
|---|---|
| `all` | TCP opens, plus a UDP/ICMP socket's first send to a destination (the default) |
| `tcp` | TCP opens only, both directions |
| `off` | nothing |

    config set system.conn_log tcp

Changing it takes effect immediately and persists to `/etc/toyos.conf`.
Records already in the ring are kept.

## See also

`ifconfig` for the per-device counters, `ping` and `wget` for things
that make entries, `config` for the setting, and
`docs/conventions/kernel.md`'s connection-log entry for the layering.
