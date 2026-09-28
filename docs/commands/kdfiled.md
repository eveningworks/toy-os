# kdfiled

**a `/bin` program.**

**Category:** Diagnostics

## Synopsis

    kdfiled

## Description

`/bin/kdfiled` — writes to disk the files a kernel debugger sent with
gdb's `remote put`. A service (`/etc/services.d/kdfiled`); nobody runs it
by hand.

**It exists because the debugger cannot write a file itself.** While the
machine is stopped, the stub (`kernel/debug/`) may not touch the
filesystem -- it may have stopped inside `kmalloc` or holding a mount's
lock -- so it only copies the bytes gdb sends into a staging area in RAM.
Once the machine runs again the kernel wakes this service, which takes
each staged file (`SYS_KDFILE`, `kdfile_abi.h`) and writes it with
ordinary locking. Windows' `.kdfiles` has the same rule: the target pulls
from the debugger at a point of its own choosing.

    (gdb) remote put build/kernel.bin /boot/boot/kernel.bin
    (gdb) continue      # the file is written once the machine runs

**Each file is written beside its target and renamed into place, and the
previous version is kept as `<stem>.old`** -- `kernel.bin` becomes
`kernel.old`, which is the file `grub.cfg`'s rescue entry boots. **Only
the first put of a path in a boot rotates it**; a second one before a
reboot replaces the new file and leaves `.old` as the kernel that is
actually running. A read-only mount (`/boot` is one) is remounted
read-write for the write and read-only again after -- and a remount that
fails puts the old mode back rather than leaving the point unmounted. The outcome goes to the kernel log with the
SHA-256 of the bytes as staged -- `kdebug: kdfiled wrote <path>, <n>
bytes, sha256 <hex>` -- so `toy-dmesg` in the same gdb session can check
it.

**What it does NOT do:** read a file back (`remote get` needs the
filesystem while stopped, and the stub refuses it), or stage more than
8 MiB at once. On a boot with no `kdebug=` the kernel answers `ENODEV`
and it exits straight away.
