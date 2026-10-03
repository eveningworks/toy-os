# netctl

**a `/bin` program.**

**Category:** Networking

## Synopsis

    netctl [OPTION]... [COMMAND [ARG]...]

## Options

- `status [DEVICE]` (or nothing) -- every card, or one: its driver, MAC,
  where it sits, its address, link, the lease netd holds for it, and the
  traffic counters.
- `address DEVICE ADDRESS [NETMASK [GATEWAY]]` -- set an address by
  hand. A field left out is left alone, so `netctl address net-718ebf
  10.0.2.20` moves an address without restating the netmask.
- `renew DEVICE` -- ask for the lease again, the remembered address
  first (Windows' `ipconfig /renew`, networkctl's `renew`).
- `down DEVICE` -- switch the card off: nothing is sent or received
  whatever the cable says, and nothing is routed through it; its
  address stays, unused, for `up`.
- `up DEVICE` -- switch it back on; netd leases it at once, asking for
  the address it had. A card nobody leases (addressed by hand, or
  `dhcp = no`) is simply back, with the address it kept.
- `--no-wait` -- `renew` and `up` return once netd has the request,
  instead of waiting up to 15 seconds for a FRESH lease -- one netd has
  acknowledged since the request (the lease file's `acked`), not merely
  the address the card already had. Busy for longer than that, netd is
  said to be busy, not absent: it still has the request.
- `-h`, `--help` -- every command and option in one page.

`DEVICE` is a card's name as `netctl` lists it. Exit status: 0 done, 1
refused or failed, 2 bad usage (`Try 'netctl --help'`).

## Description

`/bin/netctl` is the network's one command: a read half (`status`, what
`ifconfig` printed) and a write half (`address`, and switching a card
with `renew`, `down` and `up`). It replaced `ifconfig` on 2026-10-03.

**A NAME IS AN IDENTITY AND FOLLOWS THE CARD**: the kernel makes one
from the last three bytes of the MAC -- `net-718ebf` -- so moving an
adapter to a different socket renames nothing; where it sits is reported
beside it (`at pci3.0`). `/etc/net.conf` can give it a friendlier name
(`netd`).

**`renew`, `down` and `up` are carried out by `/bin/netd`**, which owns
every lease, over its control channel (`lib/unetctl.h`) -- the way
systemd's `networkctl` asks networkd. netd answers at once and acts on
its next pass, so `netctl` then watches the card itself for the
outcome: `up` and `renew` print the address when it arrives, or say
that netd is still asking.

**Down is the kernel's switch** (Linux's `ip link set DEVICE down`): a
flag on the device, separate from carrier, that stops every frame both
ways and takes the address with it so nothing routes to a card that
will not answer. netd stops leasing a card that is down. The lease FILE
stays, which is why `up` gets the same address back. With netd not
running, `down` and `up` still flip the switch -- only nothing leases
the card afterwards -- and `renew` refuses.

## What it is not

**Not a route table.** Routing is two rules -- an address inside a
device's own subnet goes straight to it, anything else goes to that
device's gateway -- so there is nothing to list that is not already on
each card's lines.

**Not `lspci`.** That says what hardware is on the bus; this says what a
driver claimed and made usable. A card in one and not the other is the
diagnosis.

**Not where naming rules live** -- that is `/etc/net.conf`, read by netd.

## Output

    net-123456: e1000  52:54:00:12:34:56  at pci0.3  mtu 1500
        inet 10.0.2.15  netmask 255.255.255.0  gateway 10.0.2.2
        lease 24 h from 10.0.2.2
        rx 4 packets, 358 bytes, 0 dropped
        tx 5 packets, 414 bytes, 0 dropped

A card switched off adds `switched off (netctl up net-123456)`. A
`link` line appears only when the driver can tell -- a card with no way
to ask is not a card whose cable is out.

**The counters are the diagnosis**, which is why they are not behind a
flag. They are the core's, so "received" means "reached the stack": a
card whose `rx` climbs while `tx` stays flat is listening to a network
it cannot answer on, and one whose `rx_dropped` climbs alongside `rx`
is being handed frames faster than the stack drains them.

A card with no address prints `inet (unconfigured)`; a machine with no
card prints `no network devices` rather than nothing.

## See also

`netd` for leases and naming, `ping` for whether any of it works,
`netlog` for the connection log, `lspci` for what is on the bus. The
desktop's network flyout (the tray) shows the same card and offers
Renew and the on/off switch.
