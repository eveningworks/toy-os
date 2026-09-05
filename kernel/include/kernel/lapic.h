#ifndef LAPIC_H
#define LAPIC_H

#include <stdint.h>

// The Local APIC -- the CPU's own interrupt controller, and the thing
// MSI is addressed TO.
//
// WHY IT EXISTS HERE AT ALL. An MSI is not a wire: it is a memory write
// the device performs to 0xFEE00000, which the CPU's Local APIC turns
// into an interrupt on the vector the write's DATA names. So "use
// something better than shared INTx lines" starts here and cannot start
// anywhere else -- the PCI half is a capability write and is the easy
// part. virtio.h has said "MSI is delivered as a memory write to the
// Local APIC, and this kernel has none" for as long as there has been
// a virtio driver.
//
// WHAT THIS IS NOT. It is not SMP: nothing here starts another
// processor, and `docs/smp-design.md` still owns that. It is not an
// I/O APIC either -- the 8259 PIC still routes every legacy line, and
// the ACPI MADT's I/O APIC entry is still only reported. What this adds
// is the LAPIC itself, the vectors above the PIC's, and one device
// (the xHCI) delivering through them instead of a shared pin.
//
// **THE TRAP, AND IT IS THE WHOLE REASON THIS FILE HAS A LONG COMMENT:
// ENABLING THE LAPIC MOVES THE 8259's WIRE.** Before it is enabled the
// PIC drives the CPU's INTR pin directly, which is how every interrupt
// in this kernel has ever arrived. Once the LAPIC is enabled, INTR is
// the LAPIC's, and the PIC reaches the CPU only through the LAPIC's
// LINT0 input -- which must be programmed for ExtINT delivery or the
// timer, the keyboard and the disk all stop at once. That configuration
// has a name, "virtual wire mode", and lapic_init() sets it up before
// it returns. Nothing else in this file is subtle; that is.

// Where the MSI vectors live. Above the PIC's 32-47, with room for
// sixteen -- more than a machine this size has devices, and the number
// is bounded by the IDT stubs in isr.asm rather than by anything here.
#define LAPIC_VECTOR_BASE  48
#define LAPIC_VECTOR_COUNT 16
#define LAPIC_VECTOR_LAST  (LAPIC_VECTOR_BASE + LAPIC_VECTOR_COUNT - 1)

// The spurious vector. The architecture requires the low four bits to
// be set on some old parts, so 0xFF is the conventional choice and the
// one every OS uses.
#define LAPIC_SPURIOUS_VECTOR 0xFF

// Brings the Local APIC up, in virtual wire mode so the 8259 keeps
// working. Safe to call on a machine that has no APIC (it reports and
// returns) and safe to call twice. Called from kernel_main() AFTER
// idt_init() -- the spurious vector must have a gate before anything
// can deliver to it -- and before any driver asks for an MSI vector.
//
// `nomsi` on the GRUB line skips it entirely, which leaves every device
// on the PIC exactly as before. That word is the one-line answer to
// "did the APIC break this?", and docs/boot-flags.md carries it.
void lapic_init(void);

// Is the Local APIC up? 0 means every driver must stay on INTx.
int lapic_present(void);

// End of interrupt, for a vector the LAPIC delivered. NOT for the
// PIC's lines -- those are acknowledged to the 8259 by irq.c, and
// sending this instead would leave the PIC's in-service bit set
// forever.
void lapic_eoi(void);

// This CPU's LAPIC id, which is what an MSI message address names as
// its destination.
uint8_t lapic_id(void);

// Claims one MSI vector for `handler`, or 0 when none is free (or the
// LAPIC is not up). The vector is what a device's MSI capability is
// programmed with; the handler runs from the IDT gate for it, and the
// EOI is sent for you, exactly as irq.c does for a line.
uint8_t lapic_alloc_vector(void (*handler)(uint64_t *regs));

// Hands a vector back. Only for a caller that claimed one and then
// found the device had no MSI capability to program it into -- the
// vector space is small (LAPIC_VECTOR_COUNT), and a driver that fell
// back to its pin must not keep a slot it will never be delivered on.
void lapic_free_vector(uint8_t vector);

// Dispatches a delivered MSI vector. Called from isr_dispatch() and
// from nowhere else.
void lapic_dispatch_vector(uint8_t vector, uint64_t *regs);

// How many vectors are claimed, and one line about the controller, for
// `lsdev`. Returns 0 when there is no LAPIC.
int lapic_summary(char *buf, uint32_t cap);

#endif
