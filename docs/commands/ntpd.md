# ntpd

**a `/bin` program.**

**Category:** Networking

## Synopsis

    ntpd [-1 | -q] [-p port] [server]
      -1        sync once and exit, instead of staying resident
      -q        report the offset and change nothing
      -p port   the server's UDP port (default 123)

## Description

`/bin/ntpd` — ask a network time server what time it is, and set this
machine's clock to the answer. With no argument it stays resident and
keeps syncing; that is the form init runs at boot.

**It speaks SNTP (RFC 4330), not full NTP**, and the difference is real.
Full NTP disciplines the clock's *rate* against several servers and
never lets time run backwards. This asks one server and **steps**. The
reason is that toy-os's clock has no rate knob to turn: `api/ktime.h`'s
wall clock is an epoch plus a monotonic delta, and adding a
tick-rate adjustment is a different kernel feature from the one that
exists. chrony and `systemd-timesyncd` are the real-world comparison;
the latter also steps by default on a first sync.

**A step cannot move an interval.** Anything measuring elapsed time uses
`SYS_MONOTONIC_NS`, which this cannot touch, so the cost of stepping is
bounded to whatever reads wall time — a file's mtime, the taskbar clock,
`/bin/time`.

**Why it is a ring-3 program.** Which server to trust, how long to wait,
how often to ask and what to do when nobody answers are all policy. The
kernel owns the clock and exposes exactly one verb for it,
`SYS_SETTIME`, the same way it owns the network device and exposes
`SYS_NET_CONFIG`. `/bin/dhcp` is here for the same reason.

## What it reads

Three settings, all in `system` (`/etc/toyos.conf`), all editable in
System Settings under **Time & Locale → Network Time** and with `config`:

| Setting | Default | What it is |
|---|---|---|
| `system.ntp` | `off` | whether the resident service syncs at all |
| `system.ntp_server` | `pool.ntp.org` | the host to ask |
| `system.ntp_interval` | `60` | minutes between syncs, 1–1440 |

**They are re-read every cycle**, so changing one in System Settings
takes effect without restarting the service.

**A server named on the command line outranks the setting and is not
persisted.** `ntpd -q time.example.com` is a question about that host,
not a decision about this machine; `config set system.ntp_server` is how
the machine is configured.

**`-1` and `-q` ignore `system.ntp`.** The setting governs the
background service; somebody who typed the command has already said what
they want.

## It ships switched on and idle

`/etc/services.d/ntpd` starts at boot with `Restart=always`, and the
service does nothing at all until `system.ntp` is `on` — which it is
not, out of the box. **So a stock machine sends no packet to anyone.**

That is one switch rather than two: turning network time on in System
Settings works with no service to enable as well.
`systemd-timesyncd` is shaped the same way, gated by `timedatectl
set-ntp`. Compare `telnetd` and `tftpd`, which ship *disabled* instead —
those have no setting to gate them and listen the moment they run.

## What it refuses

The three refusals RFC 4330 requires of a client, each of them a server
saying its own answer is no good:

- **Leap indicator 3** — the server is not synchronised itself.
- **Stratum 0** — a kiss-o'-death packet. The four-character code is
  printed; `RATE` means back off, `DENY` means go away.
- **Stratum above 15**, or a zero transmit timestamp, or a reply whose
  originate timestamp is not the nonce this client sent.

A reply that is none of ours is not an error, it is somebody else's, and
it is ignored without ending the wait.

## The 2036 rollover is handled

NTP counts seconds since 1900 in a 32-bit field, which wraps on
2036-02-07. The top bit is read as the era, as RFC 4330 §3 says: set
means the epoch is 1900, clear means it is 2036. Reading it as 1900
unconditionally is correct today and puts the clock 136 years in the past
the moment it wraps — the shape of bug that ships precisely because
nothing can test it.

## How often it asks, and how close it gets

**One exchange per interval, and nothing in between.** Measured on the
test laptop with the interval set to its minimum of one minute: the
server it was pointed at logged requests at 11:13:47, 11:14:47,
11:15:47, 11:16:47 and 11:17:47 — five in a row, exactly sixty seconds
apart, with no extras. At the default of sixty minutes that is one NTP
packet and one DNS lookup an hour.

A sync that FAILS retries sooner, starting at thirty seconds and
doubling until it reaches the interval. So a machine whose first attempt
raced the network recovers in half a minute, and one on a segment with
no time server does not keep asking every thirty seconds for ever.
Within a single exchange the request is retransmitted at one, two and
four seconds if nothing answers, inside an eight second budget.

**Accuracy, measured the same day against `fi.pool.ntp.org`:** three
different pool members put the laptop at −2 ms, +0 ms and −4 ms
immediately after a sync, over round trips of 9 to 20 ms. That is the
half-round-trip assumption working as well as it can on a local network.
Before `SYS_SETTIME` carried nanoseconds the same machine sat a few
hundred milliseconds behind after every sync, which is what that bug
cost and why the fraction is not decoration.

## What is not built

- **No slew.** See above; the clock has no rate to adjust.
- **One server, not a quorum.** Real NTP polls several and discards
  outliers (the "falseticker" problem). One server is trusted completely
  here.
- **No authentication.** Neither symmetric keys nor NTS. A machine on a
  network where somebody can forge replies can be told any time at all.
- **No resolver cache, by design.** The server name is resolved every
  cycle, because a pool name answers with a different address each time
  and a client that cached the first one would hammer one member of the
  pool for as long as the machine is up.

## Verifying it

`tools/ntp_test.py` runs a Python SNTP server on the host's loopback and
drives the whole path against it — **nothing leaves the machine**, and it
works with the cable out. It carries a `--positive-control` that answers
with a deliberately wrong epoch conversion; the clock checks must go red.

**Its reboot check cannot pass under QEMU**, and that is the emulator
rather than this OS: QEMU re-seeds its emulated MC146818 from the *host*
clock on machine reset, so a guest's CMOS write cannot outlive a reboot
however correct it is. The write itself is proved in the same boot by
`kernel/core/ktime_test.c`'s "the RTC takes what was written to it",
which reads the hardware back through `rtc_read()`.

**On real hardware it survives, and that is measured** (2026-09-03, the
bare-metal test laptop). Its RTC was running 2h 59m fast; one `ntpd -1`
against a server on the local network brought it to the second, and
after a reboot `clock.utc` still matched that server exactly while
`clock.steps` read **0** — nothing had set the clock since boot, so the
time came from the CMOS. The kernel's own boot line agreed:
`rtc: hardware clock reads 2026-09-03 11:09:11 UTC`, where before the
sync it would have read three hours later. That is the half QEMU cannot
answer.

## Examples

    ntpd -q                     # what is my clock's offset? change nothing
    ntpd -1                     # sync once, from the configured server
    ntpd -1 time.example.com    # sync once, from that host
    config set system.ntp on    # let the resident service keep it set

## See also

`time` for what the clock now reads, `config` for the three settings,
`dhcp` for where the nameserver comes from, and
`docs/conventions/kernel.md`'s entry on the wall clock for the layering.
