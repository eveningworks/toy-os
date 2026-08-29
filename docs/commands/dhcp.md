# dhcp

**a `/bin` program.**

**Category:** Networking

## Synopsis

    dhcp [<device>]

## Description

`/bin/dhcp` — ask the network for an address instead of inventing one.
It runs the four-message exchange every DHCP client runs (DISCOVER,
OFFER, REQUEST, ACK), applies the result to the device, and writes the
nameserver it was given to `/etc/resolv.conf`.

With no argument it configures the first device. With one it configures
that device by name, and the socket is bound to that card as well as to
the port — Linux's `SO_BINDTODEVICE`, and the reason `bind` takes a
device at all: a client must broadcast out of a *named* interface before
any interface has an address to route by.

**It is a ring-3 program, not something the kernel does.** DHCP is
policy — which offer to take, how long to wait, what to do with the
lease — and it applies its result through `SYS_NET_CONFIG`, the same
call `ifconfig` uses. There is no privileged path here that a person
could not take by hand.

## What it is not

**Not a daemon, and the lease is not renewed.** A real client keeps a
timer and renews at half the lease; this asks once and applies what it
gets. That is a genuine limitation: a lease that expires under a
long-running machine leaves it using an address the server has since
given away.

**Not run at boot.** The kernel gives the first device QEMU's
user-networking defaults (`10.0.2.15/24 via 10.0.2.2`) so a machine
boots usable, and this overwrites them when you run it. Making it a
service is a roadmap item and is blocked on a real bug rather than on
effort — a filesystem write during the desktop's startup wedges the
compositor (`docs/bugs.md`), and this writes `/etc/resolv.conf`.

## Output

    dhcp: net0: 192.168.76.20 netmask 255.255.255.0 gateway 192.168.76.2
    dhcp: nameserver 192.168.76.3 -> /etc/resolv.conf
    dhcp: lease 86400 seconds (not renewed -- see the manual)

On QEMU's default network the lease is `10.0.2.15`, which is also what
the kernel already configured — so on a default boot a working client
and a broken one print the same address. `tools/net_test.py` boots a
guest on `192.168.76.0/24` for exactly that reason.

A failure names which half did not happen, because they send you to
different places: `no offer on net0` means nothing answered the
broadcast, while `offered … and did not acknowledge it` means a server
is there and refused the request.

## See also

`ifconfig` for what it changed, `host` for what the nameserver is for,
and `docs/conventions/kernel.md`'s networking entry for the layering.
