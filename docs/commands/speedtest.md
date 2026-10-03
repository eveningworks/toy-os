# speedtest

**a `/bin` program.**

**Category:** Networking

## Synopsis

    speedtest [-l] [-s <id>] [-n <streams>] [-t <seconds>] [--no-upload] [-k] [--weak-entropy] [--url <http-url>]

## Options

- `-l` -- list the servers speedtest.net offers this machine, nearest
  first, and stop.
- `-s <id>` -- test against that server (an id from `-l`) instead of
  choosing by latency.
- `-n <streams>` -- parallel connections per direction, 1 to 6; the
  default is 4, as Ookla's own clients use.
- `-t <seconds>` -- how long each direction runs, 3 to 60; the default
  is 10. The first two seconds are not counted.
- `--no-upload` -- measure download and latency only.
- `-k`, `--weak-entropy` -- as for `wget`: skip certificate verification
  of the server-list fetch, or accept this machine's weak randomness for
  it. The tests themselves are plain HTTP.
- `--url <http-url>` -- no speedtest.net at all: download that URL over
  and over for the test's duration and report the rate.

## Description

`/bin/speedtest` measures latency, download and upload against a
speedtest.net server:

1. It fetches `https://www.speedtest.net/api/js/servers` -- the servers
   nearest this machine's public address -- through `libhttp`.
2. It pings the five nearest (the best of three `latency.txt` round
   trips on one kept-alive connection) and keeps the fastest. Distance is
   where Ookla thinks the machine is; latency is where the packets go.
3. It downloads `/download?size=…` on N connections at once, each a
   thread, for the test's duration, and reports the rate after a
   two-second warm-up -- every connection starts in slow start, and a
   rate averaged from zero under-reports a fast link.
4. It uploads to `/upload` the same way.

    Retrieving speedtest.net server list...
    Selecting best server based on ping...
    Hosted by Elisa Oyj (Helsinki) [20 km]: 4.321 ms
    Testing download speed (4 streams, 10 s)...
    Download: 94.12 Mbit/s
    Testing upload speed (4 streams, 10 s)...
    Upload: 41.07 Mbit/s

The exit status is 0 when both directions produced a number.

## What it measures, honestly

**The link AND this machine's TCP stack.** On a fast link the number is
as likely to be the stack's ceiling as the line's: a 256 KiB receive
window, NewReno congestion control, no SACK, and on a USB adapter the
driver's own rings (`docs/decisions/kernel.md`, "TCP throughput").
`--url` against a server on the same LAN is how to tell the two apart --
a LAN has no internet in it.

**Not Ookla's protocol, and not an official client.** The endpoints are
the ones the open-source `speedtest-cli` uses, not a published API, so a
change on speedtest.net's side breaks this; a failure names the step
that failed. Ookla's own CLI is a closed Linux binary and cannot run
here.

**Upload counts bytes handed to the kernel**, not bytes the server
confirmed. The difference is at most one send ring per stream, which
the warm-up and a ten-second test make negligible.

## Trust

The server list comes over https and is verified like any `wget` fetch
-- which on a default build, with an empty trust store, means it fails
until `-k` or `make iso EXTRAS=1`. The tests themselves are plain HTTP
on port 8080, as every speedtest client's are: what they carry is
random bytes.

## See also

`wget` for one fetch, `ping` for latency to a single host, `netctl`
for the counters that say whether frames were dropped.
