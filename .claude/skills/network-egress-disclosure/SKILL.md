---
name: network-egress-disclosure
description: Use when work on toy-os has sent traffic beyond the developer's own machine -- a DNS lookup, a ping, an HTTP fetch to a public host -- and a commit message is about to be written.
---

# Disclosing network egress in a commit

## Overview

Testing this OS's network stack can reach real hosts on the internet
through QEMU's user-mode networking. The maintainer's machine is the one
that makes those connections, from their address, on their connection.

**Every session whose testing left the machine names every external host
it reached, in the final response to the maintainer.** NOT in the commit
message -- that was the rule until 2026-09-07 and the maintainer changed
it: the commit records what changed, and a list of hosts a test happened
to touch is not that. It is something the person wants to SEE, once,
while the session is in front of them.

## The rule

The final response ends with this block whenever testing sent a packet
to anything outside the local machine:

    External hosts contacted during testing:
      example.com (172.66.147.243, 104.20.23.154) -- HTTP GET, /bin/wget
      <the host's own resolver>, via SLIRP's forwarder at 10.0.2.3 -- DNS A lookups

One line per host: **the name, every address it resolved to, the
protocol or port, and what did it.** A name that resolved to several
addresses lists all of them -- a reader checking their own traffic logs
needs the address, not the name.

Say it even when the answer is nothing:

    External hosts contacted during testing: none (SLIRP and localhost only)

## What counts as external

| Destination | External? |
|---|---|
| `127.0.0.1`, a host process on loopback | No |
| `10.0.2.x` -- SLIRP's gateway, DNS forwarder, the guest | No |
| A public name or address the guest resolved or connected to | **Yes** |
| A DNS query that SLIRP forwarded to the host's real resolver | **Yes** |

SLIRP's `10.0.2.3` is a forwarder, not a resolver: a lookup through it
leaves the machine. That one is easy to miss because the address in the
guest looks local.

## Prefer local, so the disclosure stays short

The automated suite must not depend on the machine having connectivity.
`tools/net_test.py` fetches from a python `http.server` on loopback and
**skips** its DNS checks when the host cannot resolve. Reach outward
only when the internet is genuinely the thing under test, and say so.

## Where to get the addresses

- The tool's own output -- `wget` and `ping` print the resolved address
  before connecting, which is why they print it.
- `host <name>` in the guest, for a name you only resolved.
- The pcap, if a test captured one: every destination is in it.

## Red flags

- "It was only a DNS lookup" -- that left the machine. List it.
- "It's in the commit message" -- it does not go there any more, and a
  reader of the commit is not the audience for it.
- "It's the same host as last time" -- each session stands alone.
- "The test skips when offline, so it might not have run" -- it ran here.

## Real-world impact

The rule exists because three commits in one session (`c50cff4`,
`caeb3b5`, `7a4a645`) reached `example.com` for DNS, ICMP and HTTP, and
none of them said so. The maintainer had to ask what their machine had
been talking to.

**Where it goes changed on 2026-09-07 and the reason is worth keeping.**
It lived in the commit message first, on the argument that a commit is
what survives. The maintainer's call was that a commit should record the
CHANGE, and that a host list belongs in front of the person while they
can still act on it. What did not change is the obligation to say it, or
to list addresses rather than only names.
