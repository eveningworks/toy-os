# log

**a `/bin` program.**

**Category:** Diagnostics

## Synopsis

    log [-n <lines>] [-u <tag>] [-l <level>] [-p [N]] [-f] [--raw] [--list]
           -n  show only the last <lines>       -u  only lines from <tag>
           -l  crit|err|warn|info|debug, or 0-7 -- that level and worse
           -p  the previous boot's log; -p N goes N boots back
           --list  the retained boot logs       -f  follow as it grows
           --raw  keep the <N> level marker the kernel wrote

## Options

- `-n <lines>` -- print only the last `<lines>` lines of the file; with
  `-u` the tag filter applies within that tail, so what comes out is the
  matching lines among the last `<lines>`, not the last `<lines>`
  matching ones.
- `-u <tag>` -- only lines from `<tag>`; the tag is matched exactly, not
  as a substring.
- `-l <level>` -- only lines at that level or worse, by name (`crit`,
  `err`, `warn`, `info`, `debug`) or by Linux's digit. **Only KERNEL
  lines carry a level**; an application line has none and is never
  filtered out, because `-l err` silently hiding every service's output
  would be worse than showing too much.
- `-p [N]` -- read an earlier boot's log from `/var/log/boot/` instead of
  the current one. `-p` alone is the previous boot, `-p 3` is three boots
  back. The count is resolved against the boot counter in
  `/var/lib/logd.seq`, not against what the directory happens to hold, so
  a log deleted by hand leaves a hole rather than shifting everything
  along. Only a leading digit is taken as the count, so `log -p -n 50`
  still means "the previous boot, last 50 lines". A depth past the
  counter and a depth that lands on a boot `storage.log_keep` has
  already pruned are DIFFERENT answers -- "no boot N back" and "boot N
  is no longer retained" -- because they send you looking in different
  places.
- `--list` -- print the retained boot logs: number, size, and when each
  one started. What is actually on disk, which is not the same as
  `storage.log_keep` -- a machine that has booted three times has three.
  A log written before logd wrote headers shows `(no header)`, and a
  boot whose clock reads EARLIER than its predecessor's is marked
  `< clock went back` -- see **Which boot is which** below.
- `-f` -- print the file and keep printing as it grows, ignoring `-n`.
  It never returns; Ctrl-C ends it.
- `--raw` -- keep the `<N>` level marker in the output instead of hiding
  it, as `dmesg --raw` does.

## Description

`log` reads the persistent log that `logd` keeps in `/var/log/toyos.log` —
the kernel's own output, tagged `kernel`, plus whatever each service
printed, tagged with the service's name.

**It is not `dmesg`, and the difference is the point.** `dmesg` shows the
kernel's RING: what is still in memory, this boot only, gone the moment it
wraps. `log` shows the FILE: what was persisted, across boots, still there
after the machine has been rebooted to recover it. Neither replaces the
other — a machine that has just crashed has an interesting ring, and a
machine you have already rebooted has only the file.

**A kernel line carries its level as text**, written into the stamp by
`klog_write()` (see `dmesg`'s page for why it lives there). So the file
is greppable without this program at all:

    $ grep '<3>' /var/log/toyos.log
    [kernel] [0.51] <3> ata: dma write failed after 3 attempts (lba 4096)

which is the property this file is built around: if `log` is broken, the
log is still readable.

**`log -p` is why this exists.** Completed boots are kept as
`/var/log/boot/<n>.log`, so the question "what did it say before I
rebooted it" has an answer. The ring cannot answer it: it holds a few
hundred lines, so a driver logging once a second flushes an entire
boot's log inside five minutes.

**And `-p N` is why there is more than one of them.** Until 2026-09-19
exactly one rotated file was kept, which answers that question for the
LAST boot and nothing further back -- no use for a fault that appears on
one boot in several, which is what the USB NIC wedge is. Ten are kept
now (`storage.log_keep`), so a bad boot can be compared against the good
ones around it:

    $ log --list
      boot      size  started
        14      93 K  2026-09-19 14:02:11
        15     104 K  2026-09-19 15:48:03
        16      97 K  2026-09-19 17:05:28
        17      31 K  2026-09-19 19:33:45   (this boot)
    $ log -p 2 -u kernel | grep usb

**Which boot is which.** The first line of every boot's file is logd's
own header:

    [logd  ] logd: boot 115 started 2026-09-19 17:05:28

**THE NUMBER IS THE IDENTITY AND THE ORDER; THE DATE IS ADVISORY.** The
number comes from a counter in `/var/lib/logd.seq` and cannot lie, which
is why `-p N` counts back from it. Nothing in the machine can vouch for
the clock, so three cases are separated:

- **never set** -- a machine whose RTC reads 1970, 1980 or 2000 gets
  `clock not set` rather than a date that looks like data.
- **set but wrong** -- a dead CMOS battery or an RTC in local time is
  undetectable from inside one boot. `--list` marks a boot whose date
  reads earlier than its predecessor's, so the fault is visible rather
  than quietly misleading; the numbering is unaffected.
- **corrected later by `ntpd`** -- the header is the time at logd's
  start, so it is pre-correction. Nothing else in the file contradicts
  it: every other stamp is boot-relative.

journald splits the same way, and for the same reason -- a boot ID is
the identity and the realtime stamp is a hint, which is why `journalctl
--list-boots` can show times that do not sort.

**A boot that fills its share STOPS rather than evicting the history.**
Each boot gets `storage.log_max / (storage.log_keep + 1)`, and one that
reaches it says so on its last line. journald rotates within a boot
instead, which lets one runaway logger flush every older boot -- exactly
the failure the retention exists to survive.

**`-u` matches the tag exactly**, not as a substring. `logd` writes the tag
first and pads it to a fixed width, so `log -u netd` cannot be satisfied by
a message that merely mentions netd.

**Everything here is a convenience over `cat` and `grep`.** The file is
plain text:

    [kernel] [0.00] toy-os 0.3.0-dev built 2026-09-06 17:11:18
    [kernel] [0.90] usb: port 2: connected, low-speed, enabled

so `grep usb /var/log/toyos.log` works, and if this program is ever broken
the log is still readable. That property is deliberate.

**`-f` is a poll**, not a notification — there is no inotify here and the
file grows from another process, so the only honest way to watch it is to
look again. It never returns; Ctrl-C ends it.

## See also

`dmesg` (the in-memory ring), `logd` (the daemon that writes the file),
`config get storage.log_max` (the total budget; 0 disables logging),
`config get storage.log_keep` (how many completed boots are kept).
