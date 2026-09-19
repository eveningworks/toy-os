#ifndef NETHEAL_CONFIG_H
#define NETHEAL_CONFIG_H

// `system.net_recover` and `system.net_recover_wait`: whether a machine
// that comes up with NO NETWORK reboots itself once, and how long it
// waits before deciding. Read by `/bin/netheal`, which does the work;
// nothing in the kernel acts on a change.
//
// It exists for a USB fault, not a network one -- see netheal.c and
// docs/bugs.md's "A SUPERSPEED DEVICE CAN LAND ON THE USB2 COMPANION
// PORT". It is a way to stop that stranding a headless machine, and
// deliberately NOT a fix: the bug stays open.
void netheal_setting_register(void);

#endif // NETHEAL_CONFIG_H
