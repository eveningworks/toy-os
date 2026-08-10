#ifndef IDT_H
#define IDT_H

#include <stdint.h>

struct interrupt_frame;

void idt_init(void);
void idt_set_gate(uint8_t vector, void (*handler)(void), uint8_t ist, uint8_t type_attr);

// Installs a callback invoked when a CPU exception (vector < 32) occurs
// while the faulting code was running in ring 3 (CS RPL == 3) -- after
// the generic "KERNEL PANIC" banner and register dump are printed, but
// before the machine halts. Pass NULL to remove. This exists so a
// ring-3 test/demo can print its own diagnostics without idt.c needing
// to know anything about it.
typedef void (*ring3_fault_hook_fn)(uint64_t vector, uint64_t error_code, uint64_t cs, uint64_t cr2);
void idt_set_ring3_fault_hook(ring3_fault_hook_fn hook);

#endif
