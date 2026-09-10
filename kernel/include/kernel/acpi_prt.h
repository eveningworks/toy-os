#ifndef ACPI_PRT_H
#define ACPI_PRT_H

#include <stdint.h>

// PCI INTERRUPT ROUTING FROM THE FIRMWARE: the `_PRT` object of the PCI
// root (and of each bridge that has one), READ rather than executed.
//
// Measured on four machines (tools/aml_walk.py), an APIC-mode `_PRT` has
// exactly these shapes, all constants once one predicate is settled:
//   Name (_PRT, Package {...})                          -- a static package
//   Method (_PRT) { Return (Package {...}) }             -- QEMU i440fx
//   Method (_PRT) { If (PICx) { Return (A) } Return (B) } -- laptops, q35
//   Method (A)    { Return (^^A) }                       -- Lenovo, a reference
// where PICx is the flag \_PIC(1) would have set. This reader takes the
// branch a PIC-mode-off OS would take (a name beginning `PIC` reads as
// 1) and follows references; anything else is refused with -ENOTSUP,
// which pci_irq_line() answers with the BIOS's interrupt_line.
//
// An entry's source is a GSI outright (Source 0) or a link device whose
// `_CRS` must itself be a constant buffer; a link whose `_CRS` is a
// method that reads chipset registers (every PIC-mode table, and the
// whole of i440fx's) is refused the same way.

// The GSI for `pin` (0 = INTA) of device `dev` on `bus`, with its
// trigger and polarity. 0 on success; -ENOENT for no `_PRT` or no
// entry; -ENOTSUP for a table this cannot read without executing it.
int acpi_prt_lookup(uint8_t bus, uint8_t dev, uint8_t pin,
                    uint32_t *gsi, int *level, int *low);

#endif
