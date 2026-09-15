# log

**a `/bin` program.**

**Category:** Diagnostics

## Synopsis

    log [-n <lines>] [-u <tag>] [-l <level>] [-p] [-f] [--raw]
           -n  show only the last <lines>       -u  only lines from <tag>
           -l  crit|err|warn|info|debug, or 0-7 -- that level and worse
           -p  the PREVIOUS boot's log          -f  follow as it grows
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
- `-p` -- read the PREVIOUS boot's log, `/var/log/toyos.log.1`, instead
  of the current one.
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

**`log -p` is why this exists.** The previous boot's log is kept as
`/var/log/toyos.log.1`, so the question "what did it say before I rebooted
it" has an answer. The ring cannot answer it: it holds a few hundred
lines, so a driver logging once a second flushes an entire boot's log
inside five minutes.

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
`config get storage.log_max` (how big it may get; 0 disables logging).
