# lscpu

**a `/bin` program.**

**Category:** System information

## Synopsis

    lscpu

## Description

CPU identity and features, over `SYS_CPU_INFO`: vendor, brand, family/
model/stepping, measured or CPUID-reported speed, and which protection
features are actually ENABLED (SSE, NX, SMEP, SMAP).

Enabled rather than merely supported, which is the distinction worth
having: a CPU advertising SMAP that the kernel never turned on offers
no protection, and only one of those two numbers says so.

## Counting cores is a different question, from a different source

CPUID describes **the core executing the instruction** and cannot see
the others, so the `Logical CPUs` line does not come from it — it comes
from the ACPI MADT, over `QUERY_CPUS`. A machine whose firmware supplied
no MADT reports `1 (no ACPI MADT -- nothing counted the others)`, which
is honest rather than a guess.

The line says `1 online` on every machine, because this kernel schedules
on one core. That is a statement about the scheduler, not about the
hardware, and the two numbers beside it are the ones the firmware
reported. `acpi` prints the per-core detail; `docs/smp-design.md` has
the stages that would change the third number.

## See also

`acpi` for the processor list in full, `lspci` and `lsusb` for the same
shape of question about other hardware.
