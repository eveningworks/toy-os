#ifndef POWER_H
#define POWER_H

// Resets the machine via the 8042 keyboard controller's reset line.
// Does not return.
void system_reboot(void);

#endif
