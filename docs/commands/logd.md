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

**It writes what the kernel said, verbatim, with a tag in front:**

    [kernel] [0.90] usb: port 2: connected, low-speed, enabled

Nothing is reformatted. The kernel's `[0.90]` is boot-relative and is the
most reliable clock the machine has early on, before anything has set the
time — rewriting it as a wall clock would replace a number that is always
right with one that sometimes is not. `dmesg -a` converts it for display
when the wall clock is wanted.

**It starts from the OLDEST BYTE STILL RETAINED**, not from the moment it
runs, so everything logged before the daemon existed — the whole of boot —
is persisted too.

**A gap is reported rather than hidden.** `logd` tracks an absolute offset
into the kernel's byte stream and compares it against the oldest byte the
kernel still holds; if the ring outran it, it writes a line saying how many
bytes were lost. A reader can tell the difference between a quiet machine
and one whose evidence was destroyed, which is the whole reason `klog_read()`
takes an absolute offset rather than a ring position.

**Size** is `storage.log_max`, in MiB, shared between `toyos.log` and the
previous boot's `toyos.log.1` — half each, so the pair fits the limit.
Setting it to **0 disables logging**, and `logd` then exits cleanly rather
than staying up doing nothing.

## See also

`log`, `dmesg`, `service status logd`, `config set storage.log_max <MiB>`.
