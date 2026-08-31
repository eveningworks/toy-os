# acpi

**a `/bin` program.**

**Category:** System information

## Synopsis

    acpi [--dump <SIG> [--at N] [--len N]]

## Description

`/bin/acpi` — what this machine's firmware described in its ACPI tables,
and what the kernel will do with it when asked to stop.

The reason it exists is narrow and worth stating: `reboot --poweroff`
either turns the machine off or it does not, and when it does not there
is nothing on screen to say which half failed. This is that. The line
that matters is `poweroff:` — `yes` names the sleep type and the port
the write will go to, `no` says the machine will fall back to the legacy
port write and then halt.

The other half is the processor list, which comes from the MADT rather
than from CPUID: CPUID describes the core running the instruction and
cannot count the others. Every core reports `online: no`, because this
kernel schedules on one — see `docs/smp-design.md` for the stages that
would change that.

## `--dump`: the raw bytes of one table

`acpi --dump DSDT --at 0x1200 --len 256` hex-dumps a range of a table by
its four-character signature. `--at` and `--len` take decimal or `0x`
hex; without them you get the whole table, which for a real DSDT is tens
of kilobytes.

It exists for the same reason `lsusb -D` does: **a parser for firmware
data has to be tested against real firmware data**, and a hand-written
fixture only ever agrees with the parser written beside it. These bytes
paste into a KTEST.

**`--dump` sees the DSDT; the table list above does not.** The DSDT is
not in the RSDT or XSDT — the FADT points at it — so a machine that
lists 22 tables lists none of the ones carrying AML. This flag has its
own enumeration with the DSDT appended, and every record carries its
signature.

**Do not ask for a whole big table through the debug console.** That
much hex outruns it and lines go missing, scattered rather than
truncated, which reads as a successful dump. `tools/acpi_dump.py` reads
it in ranges and verifies the ACPI checksum, which is the check that
catches exactly that.

## What it does not do

**It does not interpret AML.** There is no AML interpreter in this OS
and none is wanted. The one exception is a byte scan of the DSDT for the
`_S5_` object, because the sleep-type values a poweroff has to write
live in AML and nowhere else — and that scan accepts one encoding and
**refuses** everything else rather than guessing. A refusal shows up
here as `poweroff: no`.

**It does not dump table bodies.** Printing bytes nobody in this system
can decode would imply an interpreter that is not there.

**It changes nothing.** Every number is read through `SYS_QUERY`; there
is no `acpi enable` verb, and entering ACPI mode happens on the way down
inside `system_poweroff()`, not from a command.

## Output

    RSDP:            revision 2, from the multiboot2 tag
    Root table:      XSDT at 0x7ffe1a2b, 8 table(s)
    ACPI mode:       yes

    Power:
      poweroff:      yes  -- S5 type 0 to PM1a port 0x604
      reset:         yes  -- value 0xf to port 0xcf9

    Tables:
      FACP  0x7ffe0000     244 bytes  rev 3    BOCHS  BXPC
      APIC  0x7ffe0100     120 bytes  rev 1    BOCHS  BXPC
      ...

    Processors (MADT):
      cpu0    apic   id 0     acpi id 0     enabled   online: no
      1 listed, 1 I/O APIC(s), local APIC at 0xfee00000
      This kernel runs on one core; see docs/smp-design.md.

A machine with no ACPI at all prints that plainly and says the poweroff
will halt instead — a real supported state, not a failure.

`RSDP: ... from the BIOS-area scan` means the bootloader passed no ACPI
tag and the kernel found the pointer by scanning the EBDA and the BIOS
ROM area itself. Everything still works; it is worth noticing because it
is the first thing to check when a machine's tables look wrong.

## See also

`reboot` (and `reboot --poweroff`), which is what these numbers are for;
`lscpu`, whose "Logical CPUs" line reads the same processor list;
`dmesg`, where the same facts are logged at boot under `acpi:`.
