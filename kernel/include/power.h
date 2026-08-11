#ifndef POWER_H
#define POWER_H

// Resets the machine via the 8042 keyboard controller's reset line.
// Does not return.
void system_reboot(void);

// Powers the machine off, via QEMU/Bochs's ACPI PM1a_CNT I/O-port
// trick (outw 0x604, 0x2000) -- works today in this exact dev/test
// setup (QEMU's default PIIX4 ACPI emulation), per docs/roadmap.md's
// Shutdown item. NOT a real ACPI shutdown: it doesn't parse the
// FADT/PM1a_CNT address out of the guest's own ACPI tables the way a
// real OS would (that's the still-not-built ACPI table parsing item,
// docs/roadmap.md), it just writes the well-known value real hardware
// would only accept after that parsing. Falls back to a halt loop with
// an on-screen message if the port write doesn't take (e.g. real
// hardware, or a future emulator without this exact legacy behavior)
// so the machine always ends up in a safe, inert state either way.
// Does not return.
void system_poweroff(void);

#endif
