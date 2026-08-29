# dhcp

**a `/bin` program.**

**Category:** Networking

## Synopsis

    dhcp [<device>]

## Description

`/bin/dhcp` — ask the network for an address. It runs the four-message
exchange every DHCP client runs (DISCOVER, OFFER, REQUEST, ACK), applies
the result to the device, and writes the nameserver it was given to
`/etc/resolv.conf`.

**Nothing else here hands out an address.** The kernel brings a card up
unconfigured and this program is where an address comes from, as on
Linux. It applies its result through `SYS_NET_CONFIG`, the same call
`ifconfig` uses, so there is no privileged path here that a person could
not take by hand.

**With no argument it configures every device that has no address**,
which is what `dhclient` does when no interface is named, and a card
that already has one is left alone and said so. **Naming a device takes
that device whatever state it is in**, which is how a card is re-leased
by hand. The socket is bound to the card as well as to the port —
Linux's `SO_BINDTODEVICE`, and the reason `bind` takes a device at all:
a client must broadcast out of a *named* interface before any interface
has an address to route by.

**It runs at boot**, as init's `dhcp` one-shot
(`data/etc/services.d/dhcp`). `Restart=no`, because it asks once and
exits; `service` reports it as `done` or `failed`, which is how a
machine answers "did I get an address?".

## No server is not a failure

A device nobody offers a lease to claims a **link-local** address
instead — RFC 3927, which is what Windows calls APIPA:

    169.254.1.0 – 169.254.254.255, netmask 255.255.0.0, no gateway

The candidate is derived from the card's MAC, so a machine tends to
claim the same address across reboots and its neighbours' caches stay
true. Before taking it, three ARP probes go out a second apart (sender
`0.0.0.0`, which is what makes a request a *probe*); anybody answering
means the address is somebody else's and the next candidate is tried, up
to ten. Two ARP announcements follow the claim.

The probing is `SYS_NET_ARP_PROBE`, which asks whether an address is
answered for and does not block — how many probes to send and how long
to wait between them is the RFC's policy, and policy lives here rather
than in the kernel.

## What it is not

**Not a daemon, and the lease is not renewed.** A real client keeps a
timer and renews at half the lease; this asks once and applies what it
gets. That is a genuine limitation: a lease that expires under a
long-running machine leaves it using an address the server has since
given away.

**A link-local address is claimed and never defended.** RFC 3927 asks a
host to keep watching for a conflicting ARP after it has taken an
address, and to give it up or defend it; nothing here is watching, and
the kernel does not report a conflict to anybody.

## Output

    dhcp: net0: 192.168.76.20 netmask 255.255.255.0 gateway 192.168.76.2
    dhcp: nameserver 192.168.76.3 -> /etc/resolv.conf
    dhcp: lease 86400 seconds (not renewed -- see the manual)

and where nothing answered:

    dhcp: no offer on net0
    dhcp: net0: 169.254.205.161 netmask 255.255.0.0 link-local, no gateway

On QEMU's default network the lease is `10.0.2.15`, which is also what a
stack with a hardcoded address would show — so a default boot cannot
tell a working client from a broken one. `tools/net_test.py` boots a
guest on `192.168.76.0/24` for exactly that reason, and boots another on
a segment with no server at all for the link-local half.

A failure names which half did not happen, because they send you to
different places: `no offer on net0` means nothing answered the
broadcast, while `offered … and did not acknowledge it` means a server
is there and refused the request. Both then fall back to a link-local
claim, so neither is the end of the run.

## See also

`ifconfig` for what it changed, `service` for whether the boot-time run
worked, `host` for what the nameserver is for, and
`docs/conventions/kernel.md`'s networking entry for the layering.
