# netd

**a `/bin` program.**

**Category:** Networking

## Synopsis

    netd

## Description

`/bin/netd` is the network daemon: it decides **what each card is
called** and **which cards get an address**. init starts it from
`/etc/services.d/netd`. It takes no arguments.

**Naming.** The kernel gives a card one name, made from the last three
bytes of its MAC — `net-718ebf` — and that is a bootstrap, not a
policy. The rules that turn it into `lan` live in `/etc/net.conf` and
are read here, in ring 3; the kernel only validates and applies a name
through `SYS_NET_RENAME` and has no opinion about what it should be.
That is udev renaming what the Linux kernel called `eth0`, and it is the
same split this project already made for NTP, DHCP and DNS.

**Addresses.** netd runs a lease per card in one loop: `udhcp_step()`
advances one interface and returns when it wants to be called again, so
the loop sleeps until the earliest deadline across every card and no
card blocks another. `dhcp -k` supervises exactly ONE card, which is why
the boot-time client is this and not that: on a two-NIC machine the
second card's lease would be renewed by nobody.

**It polls carrier rather than waiting on it.** `/bin/dhcp` waits up to
ten seconds for the wire, which is right for a command typed at a
prompt. Here it would stall every other card behind a port with no cable
in it, so netd simply does not step a card whose driver reports the link
down. A driver that cannot report carrier is not "down" and is stepped
normally.

## The rules file

`/etc/net.conf`. Absent, every card keeps the name the kernel gave it
and every card is leased an address.

The file is re-read on every pass, so `dhcp = none` takes effect at
once. **A NAME, THOUGH, IS APPLIED ONCE PER CARD, AT DISCOVERY** —
renaming a live interface would let a socket bound by name silently
follow to whatever now holds that name, which is the hazard this naming
scheme exists to remove. A changed name takes effect on the next boot,
or when the card is unplugged and put back.

    scheme = mac        how to name a card this file does not name:
                        mac | location | driver | kernel
    prefix = net        what a built name starts with
    dhcp   = all        which cards to lease for: all | none

    [54:ee:75:71:8e:bf] one section per card, keyed by MAC, by
    name = lan          location or by driver -- most specific wins
    dhcp = no           overrules the global `dhcp` for this card

**A CARD IS A SECTION, AND THAT IS WHAT LETS IT CARRY MORE THAN A NAME.**
A card keyed directly, outside any section, is also read -- that form
gives a card its name and nothing else, so `dhcp` there can only be a
machine-wide answer.

    54:ee:75:71:8e:bf = lan       this exact card, wherever it is plugged
    pci3.0            = builtin   whatever card is in that slot
    r8153             = usb       any card this driver claims

**Most specific wins** — a MAC, then a location, then a driver — so one
adapter can be named individually while a slot or a whole driver is
named by a line below it.

**A NAME IS AN IDENTITY AND FOLLOWS THE CARD.** `scheme = mac` is the
default because moving a USB adapter to another port must not rename it;
`location` is offered for anyone who wants the opposite, which is what
systemd's `enp3s0` does. Where a card currently sits is reported by
`ifconfig` as `at pci3.0` either way, so it is still findable.

## What it deliberately does not do

**Not a route table or a firewall.** Routing is two rules per device and
lives in the kernel; there is nothing here to configure.

**No hot-plug notification.** Nothing tells ring 3 that a card appeared
or a cable went in, so the loop wakes at least every ten seconds to
look. That is a poll, and it is honest about being one.

**It does not name a card it cannot name well.** `scheme = location` on
a driver that reports no location leaves the kernel's name alone rather
than inventing one that says nothing.

**It does not take over a card that already has an address.** One
configured by hand with `ifconfig` is left alone, which is `dhcp`'s rule
too. The cost is real and worth knowing: if netd itself is restarted
while a card it leased is up, it sees an addressed card and leaves it —
so that lease is not renewed until the next boot. `Restart=always` makes
that reachable, and telling the two apart needs the lease file to record
when it was granted, which it does not.

**It is not `/bin/dhcp`.** That command runs the same library and is the
way to lease a card by hand. Running both at once means two clients on
one port, and the second will fail to bind.

## See also

- `dhcp` — the same DHCP client, as a command
- `ifconfig` — what the cards are called and where they are
- `docs/decisions.md`, "An interface name is the card's identity"
