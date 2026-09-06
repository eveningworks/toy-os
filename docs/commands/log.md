# log

**a `/bin` program.**

**Category:** Diagnostics

## Synopsis

    log [-n <lines>] [-u <tag>] [-p] [-f]
           -n  show only the last <lines>       -u  only lines from <tag>
           -p  the PREVIOUS boot's log          -f  follow as it grows

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

**`log -p` is why this exists.** The previous boot's log is kept as
`/var/log/toyos.log.1`, so the question "what did it say before I rebooted
it" has an answer. That question came up repeatedly while chasing an
intermittent USB fault: the ring holds a few hundred lines, a driver
logging once a second flushed an entire boot log inside five minutes, and
the boot that mattered could not be examined at all.

**`-u` matches the tag exactly**, not as a substring. `logd` writes the tag
first and pads it to a fixed width, so `log -u netd` cannot be satisfied by
a message that merely mentions netd.

**Everything here is a convenience over `cat` and `grep`.** The file is
plain text:

    [kernel] [0.00] toy-os 0.3.0-dev built 2026-09-06 17:11:18
    [kernel] [0.90] usb: port 2: connected, low-speed, enabled

so `grep usb /var/log/toyos.log` works, and if this program is ever broken
the log is still readable. That property is deliberate: every breakthrough
in the USB investigation came from reading raw log text.

**`-f` is a poll**, not a notification — there is no inotify here and the
file grows from another process, so the only honest way to watch it is to
look again. It never returns; Ctrl-C ends it.

## See also

`dmesg` (the in-memory ring), `logd` (the daemon that writes the file),
`config get storage.log_max` (how big it may get; 0 disables logging).
