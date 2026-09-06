# logd

**a `/bin` program**, normally started as a service rather than by hand.

**Category:** Diagnostics

## Synopsis

    logd

## Description

`logd` keeps the kernel log in a file, so it survives the ring wrapping and
survives a reboot. It is started by init from `/etc/services.d/logd` and
read with `log`.

**Why a daemon rather than a dump at shutdown:** a machine that has to be
power-cycled never reaches a shutdown, and those are exactly the boots
worth reading afterwards.

**It drains TWO rings.** The kernel's, and the APPLICATION ring — one
record per write from any process whose stdout a spawn pointed at the log
(`SPAWN_FD_LOG`), which is what init gives every service. The tag is the
program's own name, taken by the kernel rather than supplied by the
writer, so it cannot be forged:

    [kernel] [0.90] usb: port 2: connected, low-speed, enabled
    [netd  ] [3.21] netd: eth0 is now net-123456

Both carry the same boot-relative stamp, so the merged file reads in
order. A service's line is stamped when its FIRST fragment was written,
not when `logd` drained it, which can be a second later.

**A record is a write, not a line**, and several programs here build one
line from several writes -- `cmd_fail_err()` sends five. `logd` joins
fragments per tag until a record says it ended the line, so the file
carries whole messages rather than the pieces they were sent in.

Nothing is reformatted. The kernel's `[0.90]` is boot-relative and is the
most reliable clock the machine has early on, before anything has set the
time — rewriting it as a wall clock would replace a number that is always
right with one that sometimes is not. `dmesg -a` converts it for display
when the wall clock is wanted.

**It starts from the OLDEST BYTE STILL RETAINED**, not from the moment it
runs, so everything logged before the daemon existed — the whole of boot —
is persisted too.

**A gap is reported rather than hidden**, on both rings. `logd` tracks an
absolute offset into the kernel's byte stream and a sequence number into
the application ring, and compares each against the oldest the kernel
still holds; if either outran it, it writes a line saying how much was
lost. A reader can tell the difference between a quiet machine
and one whose evidence was destroyed, which is the whole reason `klog_read()`
takes an absolute offset rather than a ring position.

**Size** is `storage.log_max`, in MiB, shared between `toyos.log` and the
previous boot's `toyos.log.1` — half each, so the pair fits the limit.
Setting it to **0 disables logging**, and `logd` then exits cleanly rather
than staying up doing nothing.

## See also

`log`, `dmesg`, `service status logd`, `config set storage.log_max <MiB>`.
