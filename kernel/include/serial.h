#ifndef SERIAL_H
#define SERIAL_H

void serial_init(void);
void serial_write(const char *s);
void serial_putc(char c);

// Wires up COM1's IRQ4 for RX (see serial_try_getc() below) -- MUST be
// called after idt_init(), never from/before serial_init() itself; see
// serial.c's own comment on why the ordering matters.
void serial_irq_init(void);

// Non-blocking read of one received byte, or -1 if none is waiting.
// Bytes arrive via COM1's IRQ4 (see serial.c's serial_irq_handler(),
// registered by serial_init() the same way ata.c registers its own IRQ
// -- irq_register_handler() + pic_clear_mask(), not a special case in
// idt.c), landing in a small ring buffer here rather than being read
// straight off the UART by whoever calls this -- an interrupt-driven
// RX line needs the byte drained promptly (the next one can arrive and
// overwrite it before a slow poller gets around to reading it),
// exactly the same reasoning keyboard.c's ring buffer exists for.
// First real caller: kernel/core/debug_console.c's serial debug
// console -- see docs/decisions.md.
int serial_try_getc(void);

#endif
