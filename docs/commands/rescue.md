# rescue

**a shell builtin.**

## Synopsis

    rescue [cmd ...]

## Description

The kernel's own copies of everything above, for when `/bin` is missing or damaged: `rescue ls`, `rescue cat`, `rescue stat`, `rescue df`, `rescue rm`, `rescue touch`, `rescue mkdir`, `rescue mv`, `rescue ln`, `rescue truncate`, `rescue sync`. `rescue` alone lists them. They can never shadow a real program — plain `rm` always runs `/bin/rm` — so you always know which one ran, the guarantee `sash` gets from spelling its copies `-ls`/`-rm`. They DIAGNOSE; they cannot put `/bin` back, since a shell cannot write an ELF. For that, boot `toy-os-live.iso` or re-seed from the host. `sync` is in the set so a rescue edit actually reaches the disk.
