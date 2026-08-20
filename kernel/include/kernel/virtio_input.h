#ifndef VIRTIO_INPUT_H
#define VIRTIO_INPUT_H

#include <stdint.h>

// virtio-input: keyboards, mice and tablets on the virtio transport.
// See kernel/drivers/virtio/virtio_input.c.
//
// Claims every virtio-input device on the bus (QEMU exposes one PCI
// device per `-device virtio-keyboard-pci` / `virtio-mouse-pci` /
// `virtio-tablet-pci`) and registers each with the input core, which is
// what routes their events into the same keyboard and pointer state the
// PS/2 pair feeds. Silent and allocation-free when there is none.
//
// Called from kernel_main() after pmm_init() -- virtqueues need frames
// -- and after idt_init(), because the devices are serviced by their
// PCI interrupt line.
void virtio_input_init(void);

// How many devices were claimed.
int virtio_input_count(void);

// Events decoded, cumulative -- exported so a test can assert that
// input really arrived through this driver rather than through PS/2.
uint32_t virtio_input_events(void);

#endif
