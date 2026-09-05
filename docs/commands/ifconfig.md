# ifconfig

**a `/bin` program.**

**Category:** Networking

## Synopsis

    ifconfig [<device> <address> [netmask [gateway]]]

## Description

`/bin/ifconfig` — the network devices this boot found, their addresses,
and their traffic counters. With arguments, it sets one device's
addresses.

Every NIC driver runs at boot and registers what it finds into the
device table (`kernel/include/kernel/netdev.h`). **A NAME IS AN IDENTITY
AND FOLLOWS THE CARD**: the kernel makes one from the last three bytes of
the MAC — `net-718ebf`, the vendor's own serial for that card — so
moving an adapter to a different socket renames nothing. Where it
currently sits is REPORTED beside it (`at pci3.0`) rather than encoded
into the name, which is where systemd's `enp3s0` scheme was deliberately
not followed. **No device gets an address from the kernel** — one comes
from `/bin/dhcp`, which init runs at boot and which takes every card
that has none, or from this command.

A field left out is left alone rather than cleared, so
`ifconfig net-718ebf 10.0.2.20` moves an address without restating the
netmask.
The honest cost of that is that an address cannot be *removed*.

## What it is not

**Not an up/down switch.** There is no administrative state in this
stack: a registered device is up. Adding `up`/`down` would mean
inventing the state to go with it, and the way to stop using a card is
not to address it.

**Not a route table.** Routing is two rules — an address inside a
device's own subnet goes straight to it, anything else goes to that
device's gateway — so there is nothing to list that is not already on
each device's line. A real routing table is a roadmap item.

**Not `lspci`.** That says what hardware is on the bus; this says what a
driver claimed and made usable. A card in one and not the other is
exactly the diagnosis.

## Output

    net-123456: e1000  52:54:00:12:34:56  at pci0.3  mtu 1500
        inet 10.0.2.15  netmask 255.255.255.0  gateway 10.0.2.2
        rx 4 packets, 358 bytes, 0 dropped
        tx 5 packets, 414 bytes, 0 dropped

**The counters are the diagnosis**, which is why they are not behind a
flag. They are the *core's*, not the driver's, so "received" means
"reached the stack": a card whose `rx` climbs while `tx` stays flat is
listening to a network it cannot answer on, and one whose `rx_dropped`
climbs alongside `rx` is being handed frames faster than the stack
drains them. Neither is visible from `ping` alone.

A card with no address prints `inet (unconfigured)`, which is what
every card looks like until `/bin/dhcp` has run — nothing assigns an
address at boot, so on a freshly booted machine this is a state to wait
through rather than a fault.

A machine with no card prints `no network devices` rather than nothing
at all — a real supported state, and it should not read like the command
failed.

## See also

`ping` for whether any of it works, `dhcp` for getting an address from
the network instead of by hand, `lspci` for what is on the bus, and
`docs/conventions/kernel.md`'s networking entry for the layering.
