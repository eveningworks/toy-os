#ifndef KDEBUG_NIC_H
#define KDEBUG_NIC_H

// A NIC the kernel debugger OWNS (docs/kdebug-design.md, stage 3): taken
// from the PCI bus before any driver binds it, and driven by polling
// alone. No interrupts, no locks, no allocation after claim(), because
// recv() and send() run with the machine stopped at any instruction.
// KDNET's shape; the OS never sees the card.

#include <stdint.h>
#include "pci.h"

struct kdb_nic {
    const char *name;
    // Does this backend drive `pci`? Pure: no side effects.
    int (*match)(const struct pci_device *pci);
    // Bring the card up for polling and read its MAC. Runs once, early
    // in boot (after pmm_init, before heap_init). 1 on success.
    int (*claim)(const struct pci_device *pci, uint8_t mac[6]);
    // One received frame into buf, its length; 0 when none is waiting.
    int (*recv)(uint8_t *buf, int cap);
    // One frame out. 1 sent, 0 when the ring stayed full.
    int (*send)(const void *frame, int len);
};

extern const struct kdb_nic kdb_nic_e1000;

#endif
