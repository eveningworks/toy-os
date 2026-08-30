# reboot

**a `/bin` program.**

**Category:** Developer and diagnostic (`help tests`)

## Synopsis

    reboot [--poweroff]

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
