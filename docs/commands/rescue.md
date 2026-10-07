# rescue

**a shell builtin.**

**Category:** Files and the filesystem

## Synopsis

    rescue [cmd ...]

## Description

The kernel's own copies of everything above, for when `/bin` is missing or damaged: `rescue ls`, `rescue cat`, `rescue stat`, `rescue df`, `rescue rm`, `rescue touch`, `rescue mkdir`, `rescue mv`, `rescue ln`, `rescue truncate`, `rescue sync`, `rescue dmesg`, `rescue fsck`. `rescue` alone lists them. They can never shadow a real program — plain `rm` always runs `/bin/rm` — so you always know which one ran, the guarantee `sash` gets from spelling its copies `-ls`/`-rm`. They DIAGNOSE; they cannot put `/bin` back, since a shell cannot write an ELF. For that, boot `toy-os-live.iso` or re-seed from the host. `sync` is in the set so a rescue edit actually reaches the disk, and `dmesg` because a machine whose `/bin` will not load is exactly one that cannot run `/bin/dmesg` to find out why, and `fsck` (the root only) because that disk is the one most worth checking -- a stretch of the "what you need to put `/bin` back" rule, made deliberately and on this file's own grounds that the set's job is diagnosis.