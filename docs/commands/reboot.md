# reboot

**a `/bin` program.**

**Category:** Developer and diagnostic (`help tests`)

## Synopsis

    reboot [--poweroff | --entries | --entry <name|number>]

## Options

- `--poweroff` -- shut down instead.
- `--entries` -- list the GRUB menu, numbered as GRUB numbers it, with the
  default and any pending one-shot choice marked.
- `--entry <name|number>` -- restart into that entry **for one boot**. A
  title is matched exactly; a number is its position in `--entries`.

## Description

`/bin/reboot` — one program for both, with the destructive one not the
default. It does not sync: the kernel flushes on the way down, and a
second place that has to remember is the one that gets forgotten.

## Both are LADDERS, and the log says which rung ran

Neither verb is a single mechanism. `--poweroff` tries the sleep type
this machine's own `_S5_` object named, written to the port its own FADT
named; failing that, QEMU/Bochs's fixed `outw(0x604, 0x2000)`; failing
that, it halts with a message on screen. A plain `reboot` tries the
FADT's reset register, then the 8042 keyboard-controller pulse, then
halts.

**Every rung ends with the machine stopped, so what you can see cannot
tell you which one did it.** Each prints a klog line for exactly that
reason — `acpi: S5 via PM1a 0x604 type 0` versus
`power: falling back to the QEMU/Bochs PM1a_CNT port trick` — and that
is what `tools/poweroff_test.py` asserts on rather than on the machine
stopping.

This is why shutdown works on VirtualBox and real hardware rather than
only under QEMU: the old implementation wrote the fixed port
unconditionally, and QEMU's own tables happen to agree with it.

## Restarting into one entry, once

`reboot --entry "toy-os (no kernel debugger)"` is `grub-reboot` and a
reboot in one command -- the shape of systemd's `systemctl reboot
--boot-loader-entry=` and Windows' `bcdedit /bootsequence`. It writes
`next_entry` into GRUB's environment block (`/boot/boot/grub/grubenv`,
remounting the read-only `/boot` for that one write), and `grub.cfg`'s
stanza makes it the default and **saves it empty before booting
anything**. So an entry that hangs is taken once and the next reset
boots the default -- the reason it is safe on a machine nobody is
sitting at. The desktop's Start menu offers the same as a flyout on
Restart; `tools/remote.py reboot --entry` does it from the dev host.

It refuses, rather than reboot into the default while claiming
otherwise, when this machine's GRUB could not honour the choice: a
`grub.cfg` without the stanza, or a core image `/etc/grub-core.modules`
records without `loadenv`. `install --bootloader confirm` fixes the
second; the first needs the stanza from the repo's `grub.cfg`.

    $ reboot --entries
     0  toy-os  (default)
     1  toy-os (no kernel debugger)
     2  toy-os (previous kernel)
    $ reboot --entry 1
    reboot: next boot: toy-os (no kernel debugger)

## When it does nothing but halt

`acpi` is the command that says why, before you try. `poweroff: no`
means no FADT was found, or the DSDT's `_S5_` object was in a shape the
kernel refuses to guess at — the legacy write is then all that is left,
and on a machine that does not answer port 0x604 the result is the halt
screen. `reset: no` is the same story for a plain `reboot`, and is the
normal state on QEMU's default i440fx machine, whose FADT has no reset
register at all; the 8042 pulse handles it.

## What it deliberately does not do

**It does not sync**, per the note above. **It asks nothing**: the
confirmation lives in the desktop's Start menu, not here — a command
typed at a prompt is already an answer to "are you sure?".

## See also

`acpi` for the tables both paths read, `sync` for flushing without
stopping, and `docs/decisions.md`'s "ACPI stops at the tables" for why
the sleep type comes from a byte scan rather than an AML interpreter.
