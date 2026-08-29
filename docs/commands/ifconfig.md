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
device table (`kernel/include/kernel/netdev.h`), named `net0`, `net1` in
registration order. **Only the first device gets an address
automatically** — QEMU's user-networking defaults, because nothing here
speaks DHCP yet — so a second card shows as unconfigured until this
command gives it something.

A field left out is left alone rather than cleared, so
`ifconfig net0 10.0.2.20` moves an address without restating the netmask.
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

    net0: e1000  52:54:00:12:34:56  mtu 1500
        inet 10.0.2.15  netmask 255.255.255.0  gateway 10.0.2.2
        rx 4 packets, 358 bytes, 0 dropped
        tx 5 packets, 414 bytes, 0 dropped

**The counters are the diagnosis**, which is why they are not behind a
flag. They are the *core's*, not the driver's, so "received" means
"reached the stack": a card whose `rx` climbs while `tx` stays flat is
listening to a network it cannot answer on, and one whose `rx_dropped`
climbs alongside `rx` is being handed frames faster than the stack
drains them. Neither is visible from `ping` alone.

A machine with no card prints `no network devices` rather than nothing
at all — a real supported state, and it should not read like the command
failed.

## See also

`ping` for whether any of it works, `lspci` for what is on the bus, and
`docs/conventions/kernel.md`'s networking entry for the layering.
