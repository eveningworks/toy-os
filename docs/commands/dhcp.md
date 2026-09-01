# dhcp

**a `/bin` program.**

**Category:** Networking

## Synopsis

    dhcp [-k] [<device>]

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

**It waits for carrier before it asks anything.** Up to ten seconds,
polling `QUERY_NETDEV`'s link state, and only then falling back to
link-local. That is what every real client does — `systemd-networkd`'s
`ConfigureWithoutCarrier` defaults to *no*, and dhcpcd will not send on
a down link either.

It matters here because of when things happen at boot: init starts this
about **1.5 s** in, and a USB Ethernet PHY does not report link until
**4–6 s**. Before the carrier watch existed the wait happened *by
accident* — `sendto()` on a down link returns `EAGAIN`, and the retry
loop spent the DISCOVER's own four-second budget on it — so the attempt
expired at about 5.45 s. That is a coin flip against a 4–6 s link, and
it behaved like one: an address on some boots and link-local on others,
with `dhcp net0` by hand always working afterwards because by then the
wire was up.

A driver that cannot report carrier (`link_known` 0) is **not** treated
as down: waiting on an answer that will never come would turn a working
card into a ten-second delay followed by link-local.

**It renews.** Started as a service it stays resident and re-requests at
T1, half the lease (RFC 2131). A lease that is never renewed expires at
an hour the server chose, and the machine loses its address with nothing
to say why.

**One-shot is the default**; `-k` is what keeps it resident, and the
service descriptor passes it. The other way round was tried and was
wrong: `dhcp net0` typed at a prompt never returned, because the
supervisor does not exit. A command that holds the terminal unless you
know a flag is a worse default than one that needs a flag for the new
behaviour. (`-1` is accepted as a no-op, since it is what the first
version of this called the default.)

Deciding it from `isatty()` was the other candidate — this program
already uses that to choose *where diagnostics go* — and was rejected:
getting the log destination wrong is cosmetic, getting this wrong hangs
a prompt.

Two honest limits. It does **not** distinguish RENEWING (unicast to the
leasing server) from REBINDING (broadcast at T2) — it broadcasts
throughout, which the one server on one segment this was built against
answers identically. And only the **first** device with a real lease is
renewed; a second card would need its own timer, and no machine here has
ever held two leases at once.

**A link-local address is claimed and never defended.** RFC 3927 asks a
host to keep watching for a conflicting ARP after it has taken an
address, and to give it up or defend it; nothing here is watching, and
the kernel does not report a conflict to anybody.

## Output

    dhcp: net0: 192.168.76.20 netmask 255.255.255.0 gateway 192.168.76.2
    dhcp: nameserver 192.168.76.3 -> /etc/resolv.conf
    dhcp: lease 86400 seconds

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

## Its diagnostics go to the kernel log when it is a service

A service started by init has **no stdout anybody reads**, so every
`printf` here reached nothing — which is how a boot that fell back to
link-local left no record of why, and why the first diagnosis of it had
to be done by adding up timings from `dmesg`. It writes to fd 2 (the
kernel log) when stdout is not a terminal, and to the terminal when it
is, so `dmesg` carries the boot story and a prompt still shows you the
answer.

## The service descriptor has a size budget

`/etc/services.d/dhcp` — like every descriptor — is read through
`etc_config.c`'s buffer, **comments included**, and a file over that has
its last keys silently ignored. It happened while writing the comment
that used to explain all of the above, which is why the reasoning lives
on this page instead.

The limit is `ETC_CONFIG_BUF_MAX`, now **4096**. It was 1024, which a
descriptor with a real comment on it reaches. Note there are *two*
config constants and this is not the smaller one — an hour went into
trimming this file against `ETC_CONFIG_MAX`, which is the **rewrite**
path's buffer and never fires for a read. They are the same number now,
and `tools/check_config_size.py` fails the build before either can bite
again.

**And arguments go in `Args=`, not on `Exec=`.** `Exec=/bin/dhcp -k` is
reported as `dhcp failed to start`; the key that carries them is
`Args=`, as `tftpd`'s descriptor has always shown.
