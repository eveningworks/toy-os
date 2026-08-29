# host

**a `/bin` program.**

**Category:** Networking

## Synopsis

    host <name> [server]

## Description

`/bin/host` — turn a name into an IPv4 address. Named after BIND's
`host` rather than `nslookup` because that is the one that does exactly
this and nothing else.

The nameserver comes from `/etc/resolv.conf`, which `dhcp` writes. A
second argument overrides it for one lookup, which is how you tell "the
resolver is wrong" apart from "the name is wrong".

The resolving itself lives in `userland/lib/uresolv.c` and is shared
with `ping`, so a name means the same thing in both.

## What it is not

**A only.** No AAAA (there is no IPv6 here), no MX, no TXT, no reverse
lookups. A caller wants an address to reach something at; every other
record type is a different program's question.

**No cache and no `/etc/hosts`.** Every lookup goes to the wire. That is
honest at this scale and it is the first thing to revisit if anything
ever resolves in a loop.

**An address given as the name is not looked up.** `host 10.0.2.2` says
so and stops, because asking a server to resolve an address is a
question about a name that is already an answer — and the reply would be
NXDOMAIN.

## Output

    $ host example.com
    example.com has address 172.66.147.243

Each failure names something different to check, which is why they are
separate messages rather than one:

| Message | What it means |
|---|---|
| `no nameserver configured` | `/etc/resolv.conf` has none — run `dhcp` |
| `not found` | the server answered, and the name does not exist |
| `no answer from <ip>` | the server did not reply at all |

Under QEMU's user networking the nameserver is `10.0.2.3`, a forwarder
that asks the *host's* resolver — so a lookup here is a real one against
the real internet, and it fails on a machine that is offline.

## See also

`dhcp` for where the nameserver comes from, `ping`, which resolves names
through the same library, and `ifconfig`.
