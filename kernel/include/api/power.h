#ifndef POWER_H
#define POWER_H

// Resets the machine: the FADT's reset register first, the 8042
// keyboard controller's reset line second, a halt loop last.
// Does not return.
void system_reboot(void);

// Powers the machine off through ACPI: the sleep type comes from this
// machine's own `_S5_` object and the port from its own FADT (see
// kernel/acpi/). Falls back to QEMU/Bochs's fixed 0x604 shortcut, and
// then to a halt loop with an on-screen message, so the machine always
// ends up in a safe, inert state whatever it turns out to be.
// Does not return.
void system_poweroff(void);

#endif
