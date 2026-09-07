# hwdata

**a `/bin` program.**

**Category:** System information

## Synopsis

```
hwdata update [pci|usb|all] [-n] [-k] [--from <url>]
```

## Options

- `-n` -- fetch and check, then throw the result away and say what it
  would have done. Everything but the writing, so the check that matters
  (does this URL actually serve a database?) is the part it performs.
- `-k` -- do not verify the server's certificate. Encrypted but
  **unauthenticated**: anyone able to answer in the server's place can
  hand you whatever file they like. `wget`'s flag, and the same warning.
- `--from <url>` -- fetch from here instead of the configured source.
  It names ONE file, so it is refused with `all` — say `hwdata update
  pci --from ...`.

## Description

Refreshes `/usr/share/hwdata/pci.ids` and `/usr/share/hwdata/usb.ids`,
the files that turn `8086:1237` into a vendor and a device name for
[lspci](lspci.md) and [lsusb](lsusb.md). Both ship with the system, so
both are as old as the build; a machine whose devices came out later
shows numbers where a name should be.

```
/$ hwdata update -n
pci: https://pci-ids.ucw.cz/v2.2/pci.ids
  connecting to 34.107.221.82 -- TLSv1.3, TLS_AES_256_GCM_SHA384
  would replace 1653928 bytes with 1655102 (2519 vendors)
  Version: 2026.09.02
usb: http://www.linux-usb.org/usb.ids
  connecting to 18.209.126.135
  would replace 730605 bytes with 730605 (3810 vendors)
  Version: 2026.06.26
```

`lspci --update` and `lsusb --update` are this command, handed off to.
The logic lives here and not in either of them because it reaches TLS,
and linking mbedTLS behind a program whose job is to print a table would
be paying for the fetch on every boot that never does one.

## The old file is not touched until a whole new one has arrived

The download goes to `<path>.new` beside the target and is renamed over
it only after it has been checked. That is `docs/update-design.md`'s
shape, and it is the reason this exists rather than a note in `lspci`'s
page saying to run `wget -O /usr/share/hwdata/pci.ids <url>`.

That command works. It also opens the file with `O_TRUNC`, so a download
that dies halfway leaves a **truncated database that still parses** —
half the vendors resolve, the rest show numbers, and nothing anywhere
says why. A refresh that fails should leave you exactly where you were.

The check is two numbers, and neither is enough alone: at least 64 KiB,
and at least 100 lines shaped like a vendor (four hex digits in column
zero, two spaces, a name). The size floor rejects an error page, a
redirect body or a captive portal; the vendor count rejects a page that
is merely large. A file that fails either is deleted and the live one is
left alone, with the counts printed so you can see what arrived.

## Where it fetches from

`/etc/hwdata.conf` holds `pci_url` and `usb_url`. Delete the file and
the built-in upstream URLs are used unchanged; point them at a mirror
and a machine with no route to the internet can still be updated from
one on the LAN.

**A default build ships an empty trust store** — `/etc/ssl/certs` is
legitimately empty unless the image was built with `EXTRAS=1`, which
installs Mozilla's roots. `pci-ids.ucw.cz` is https only, so on such a
build the pci fetch fails on the certificate and says so, naming the
three ways out: build with `EXTRAS=1`, point `--from` at a mirror, or
pass `-k` and accept an unauthenticated download. `linux-usb.org` offers
no https at all, so the usb fetch is plain http and is neither encrypted
nor authenticated. That matters less than it sounds for a file of names
and is stated because the alternative is silence about it.

## It never runs on its own

Nothing here is on a timer and nothing calls it at boot. The databases
are a convenience; a system that reaches the internet unasked to refresh
one is doing something its owner did not ask for. This is a command a
person types.

## Fetching is not distributing

Both files are redistributed with this system under their BSD terms. A
copy this command fetches is one the person running it obtained for
themselves — the same distinction `tools/fetch_extras.py` draws for the
Doom IWAD, and the reason a fetch needs no licence prompt while shipping
an image that carries the result would.

## See also

[lspci](lspci.md), [lsusb](lsusb.md), [wget](wget.md)
