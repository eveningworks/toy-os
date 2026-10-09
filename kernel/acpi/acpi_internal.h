#ifndef ACPI_INTERNAL_H
#define ACPI_INTERNAL_H

// Shared between the files of kernel/acpi/ and nowhere else. The split
// is by TABLE -- discovery, the FADT, the MADT, the facts -- but it is
// still one component, so the state is one struct reached through here
// rather than an interface between four (CLAUDE.md's rule for a split
// like userland/wm/'s).
#include "acpi.h"

struct acpi_state *acpi_state_mut(void);

// Can this physical range be read through boot.asm's identity map?
int acpi_phys_readable(uint64_t phys, uint32_t len);
const void *acpi_phys(uint64_t phys);

void acpi_fadt_init(void);
void acpi_power_boot(void);   // acpi_power.c: `acpimode=boot`
void acpi_madt_init(void);

#endif
